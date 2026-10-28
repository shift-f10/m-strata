// engines/unified/src/family/glm_graph.hpp - the GLM-5.3-Flash (arch "glm5next") family graph for the
// unified engine, ported from Project Maya's Glm5Model (engines/maya-ref/src/core/glm_model.{hpp,cu})
// onto the unified engine's shared substrate (u::FamilyGraph, the generic kernels in kernels/common.cuh,
// and the shared expert facility u::ExpertTier).
//
// This is the CUDA port the GPU box compiles and parity-checks; it is NOT built on the host (the host
// links u::detail::HostStubGraph instead - u_family.hpp). Design: docs/superpowers/specs/2026-10-23-
// unified-engine-design.md §3.2; plan T1.3.
//
// WHAT THIS GRAPH OWNS (GLM-specific):
//   * the per-layer schedule: 45 layers, each a KDA recurrent mixer (is_recr) or a DSA NoPE-MLA mixer
//     + the learned k-pool indexer, wrapped in mHC (manifold hyper-connection) read/write around a
//     4-stream residual, then a dense (leading 3) or sigmoid-MoE FFN, also mHC-wrapped;
//   * the GLM-specific kernels (kernels/glm_kda, glm_dsa, glm_hc, glm_ffn, glm_dflash);
//   * the latent KV cache (kv_lora floats per DSA cell) + the indexer key/gate/pool caches;
//   * the NextN/DFlash speculative-draft path (structured; see kernels/glm_dflash.cuh).
//
// WHAT IT BORROWS FROM THE SUBSTRATE (NOT duplicated here):
//   * the generic kernels (quantized matmul/mmvq, rms/layer norm, sigmoid, axpy/add, router_sigmoid,
//     the sampler) from kernels/common.cuh, namespace u::k - see the assumed contract in glm_graph.cu;
//   * the sigmoid-routed MoE experts (288 experts / 8 used / 1 shared) through u::ExpertTier, which
//     owns the VRAM/RAM/disk tiering + the expert-hit profile warm start (design §3.5).
#pragma once

#include "u_family.hpp"
#include "u_geometry.hpp"
#include "u_experts.hpp"   // u::ExpertTier + the shared u::WSlot

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace u {

// Forward declaration of the mapped-GGUF accessor the graph's load() takes (defined in u_gguf.hpp;
// u::ExpertTier and the shared u::WSlot come from u_experts.hpp, included above).
struct GgufFile;

// Every number the GLM graph's kernels depend on, the glm5next shape. Ported from
// strata::core::Glm5Geometry (engines/maya-ref/src/core/glm_model.hpp L66) with the released
// GLM-5.3-Flash defaults, mirroring engines/maya-ref/ref/glm.py::Glm5Config.
//
// SEAM (see make_glm_graph in glm_graph.cu): the common fields come from the guarded u::Geometry;
// the GLM-specific fields below are either GLM constants or must be surfaced by the geometry guard.
// The per-layer schedule `n_head_kv` (0 => KDA) and `indexer_full` are checkpoint-specific and CANNOT
// be defaulted safely - they are the one thing the substrate's u_geometry MUST plumb through for GLM.
struct GlmShape final {
  int n_embd = 4096, n_layers = 45, n_head = 64, n_vocab = 154880;
  int nextn = 0;                      // NextN/MTP layers counted inside block_count (46 w/ nextn 1)
  std::vector<int> n_head_kv;         // per layer; 0 marks a KDA (recurrent) layer
  int q_lora = 1536, kv_lora = 512, qk_nope = 256, v_head = 256;
  int kda_head_dim = 128, d_conv = 4;
  float kda_lb = -5.0f;
  int idx_heads = 32, idx_key = 128, idx_top_k = 2048, idx_kpool = 4;
  int idx_select_tail = 1;
  std::vector<int> indexer_full;      // per layer; this runner implements full indexers only
  int hc = 4, sinkhorn_iters = 20;
  float hc_eps = 1e-6f, norm_eps = 1e-5f;
  int n_expert = 288, n_exp_used = 8, n_ff_exp = 2048, n_shared = 1, n_ff_dense = 12288, dense_lead = 3;
  float w_scale = 1.0f;
  int norm_w = 1;
  float swiglu_exp = 10.0f, swiglu_shexp = 10.0f;

  int d_inner() const { return kda_head_dim * n_head; }
  int top_pools_max() const { return idx_top_k / idx_kpool; }
  int n_sel_max() const { return idx_top_k + (idx_select_tail ? idx_kpool - 1 : 0); }
  bool is_recr(int il) const { return n_head_kv[(std::size_t) il] == 0; }
};

// The GLM-5.3-Flash family graph. One instance serves one model on one device; generate() runs one
// request, appending tokens to the live sequence state (the serve loop resets between requests).
class GlmGraph final : public FamilyGraph {
 public:
  GlmGraph(GlmShape shape, std::uint32_t context);
  ~GlmGraph() override;
  GlmGraph(const GlmGraph&) = delete;
  GlmGraph& operator=(const GlmGraph&) = delete;

  [[nodiscard]] FamilyId family() const override { return FamilyId::glm5next; }
  [[nodiscard]] std::uint32_t context() const override { return context_; }
  [[nodiscard]] bool available() const override { return loaded_; }
  [[nodiscard]] std::string describe() const override;

  // Runs one request: prefills the prompt ids, then samples + emits `T <id>` per token (with optional
  // PP / STAT via the sink) up to max_new or a stop. Returns true (the serve loop appends DONE) or
  // sets `error`. See glm_graph.cu.
  [[nodiscard]] bool generate(const Gen& request, const LineSink& sink, std::string& error) override;

  // Uploads the non-expert GLM tensors to the device weight arena, sizes the state + scratch arenas
  // for `max_ctx`, and hands the routed-expert tensors to the tier. Called by make_glm_graph (the
  // substrate threads the mapped GGUF through - see the seam note in glm_graph.cu). `mapped` is the
  // GGUF's data section base; offsets are GgufTensor::offset from there.
  [[nodiscard]] bool load(const GgufFile& gguf, const std::uint8_t* mapped, std::string& error);

 private:
  // One token through all layers at cache position `pos_` (the serve/decode path; T=1). Faithful port
  // of Glm5Model::step_layers (glm_model.cu L2198).
  [[nodiscard]] bool step(std::int32_t token, std::string& error);
  // Writes the last step's logits (device) and samples the next id via u::k::sample_tokens.
  [[nodiscard]] bool sample_next(const Gen& request, std::int32_t& out_id, std::string& error);
  void reset();   // zeroes the residual stack + every KDA state + every DSA cache

  GlmShape g_;
  std::uint32_t context_ = 0;
  bool loaded_ = false;
  std::int64_t pos_ = 0;        // the next cache position
  std::int64_t max_ctx_ = 0;

  std::unique_ptr<ExpertTier> experts_;   // the shared MoE facility (sigmoid-routed, 288/8/1)

  // ---- the device weight arena (non-expert tensors) + the name -> slot map (ws_map_ in the ref)
  float* w_arena_ = nullptr;
  std::uint64_t w_arena_bytes_ = 0;
  float* dsa_f32_ = nullptr;  // F32 dequant of the DSA attn_k_b/attn_v_b weights (glm_q_absorb/mla need f32)
  std::map<std::string, WSlot> ws_;
  [[nodiscard]] const WSlot& W(const std::string& name) const { return ws_.at(name); }
  [[nodiscard]] const WSlot& W(int il, const char* suffix) const;

  // ---- the state arena (persists across tokens): the residual stack R lives at its head, then per
  // KDA layer the recurrent state S + the 3 conv histories, per DSA layer the latent cache + indexer
  // key/gate/pool caches. Offsets are floats into state_; -1 = the layer has no such state. Ported
  // from glm_model.cu L747-776.
  float* state_ = nullptr;
  std::uint64_t state_bytes_ = 0;
  std::vector<std::int64_t> kda_S_, kda_conv_, dsa_lat_, dsa_ik_, dsa_ig_, dsa_pool_;

  // ---- the scratch arena (one token's activations; reused each token). Ported from glm_model.cu
  // L779-826. Offsets are floats into sc_.
  float* sc_ = nullptr;
  std::uint64_t sc_bytes_ = 0;
  int* d_pos_ = nullptr;        // the select kernel reads the query position from device memory
  int* d_tok_ = nullptr;        // the sampler's one-int output

  // ---- matmul-dispatch scratch for gemv_() over the shared kernels (u::k): a Q8_1 activation buffer
  // for the quantized mmvq path, and a BF16 activation buffer for the bf16 path. Sized for the widest
  // reduction dim in load().
  void* q8_scratch_ = nullptr;
  std::uint16_t* bf16_x_ = nullptr;
  std::int64_t max_n_in_ = 0;
  // y[0,n_out) = W x, dispatching on the WSlot's type: F32 -> u::k::f32_gemv, BF16 -> f32_to_bf16 +
  // u::k::bf16_gemv, quantized -> u::k::mmvq_f32. No bias/scale (the shared kernels have none); the
  // one prescale site (indexer.proj) follows with u::k::scale_inplace.
  void gemv_(const WSlot& w, const float* x, float* out, std::int64_t n_out, std::int64_t n_in, void* stream);
  // out[0,n) = rms_norm(in, w): a device copy then the in-place shared u::k::rms_norm (which has no
  // separate-output form).
  void rmsnorm_to(float* out, const float* in, const float* w, std::int64_t n, float eps, void* stream);
  std::int64_t sc_mixed = 0, sc_emb = 0, sc_x = 0, sc_pre = 0, sc_post = 0, sc_comb = 0, sc_inv = 0;
  std::int64_t sc_proj = 0, sc_conv[3] = {0, 0, 0}, sc_g = 0, sc_g1 = 0, sc_beta = 0, sc_tmp = 0;
  std::int64_t sc_scan = 0, sc_g2 = 0, sc_gated = 0, sc_qr = 0, sc_kv = 0, sc_q = 0, sc_qabs = 0;
  std::int64_t sc_iq = 0, sc_iw = 0, sc_score = 0, sc_cells = 0, sc_attn = 0, sc_gate = 0, sc_up = 0;
  std::int64_t sc_h = 0, sc_dn = 0, sc_moe = 0, sc_sh = 0, sc_ffn = 0, sc_mixer = 0, sc_head = 0;
  std::int64_t sc_logits = 0, sc_ids = 0, sc_rw = 0;
};

// Builds the GLM graph for a guarded geometry. `context` is the serving context (the geometry's
// context_length clamped by --max-context). Returns the graph (not yet weight-loaded: the substrate
// calls GlmGraph::load with the mapped GGUF), or sets `error` and returns nullptr on a bad shape.
[[nodiscard]] std::unique_ptr<FamilyGraph> make_glm_graph(const Geometry& geom, std::uint32_t context,
                                                          std::string& error);

}  // namespace u
