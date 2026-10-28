// strata-unified-engine: the Qwen3.6-35B-A3B (GGUF architecture "qwen35moe") per-family model graph.
//
// Qwen3.6-35B-A3B is a 35B-total / 3B-active hybrid MoE. Its per-layer schedule over 40 trunk layers:
//   * GDN (Gated-DeltaNet) LINEAR attention on 30 of 40 layers (l % 4 != 3), NO KV cache — the
//     history lives in the per-layer recurrent state S + a short causal-conv history;
//   * periodic FULL attention on 10 of 40 layers (l % 4 == 3): GQA 16 query / 2 kv heads, head_dim
//     256, per-head q/k RMS-norm, NEOX RoPE over the first 64 dims (base 1e7), sigmoid output gate;
//   * every layer a MoE: 256 experts, top-8, SOFTMAX-routed and renormalized, PLUS one always-on
//     shared expert gated by a sigmoid scalar (sigmoid(w_shexp_gate . h));
//   * an optional GGUF-native MTP (multi-token-prediction) draft block at blk.<n_layers>, driven by
//     --mtp N (N verified drafts per speculative step) for greedy requests.
//
// PORTED FROM upstream/src/qwen35/q36_engine.cu (the production engine) with the clean fp32
// reference q36ref.cu as the spec (its top comment is the whole forward pass). The family-unique
// kernels live in src/kernels/qwen35moe_gdn.* (the GDN linear path) and src/kernels/qwen35moe_attn.*
// (the full-attention path); everything generic rides the shared substrate — the generic kernels in
// kernels/common.cuh (u::k::) and the VRAM/RAM expert-tiering facility in u_experts.hpp
// (u::ExpertTier). See qwen35moe_graph.cu's ASSUMED-SHARED-API block for the seams this lane does
// not own.
//
// ROUTER NOTE (reconcile with the substrate): u_geometry.cpp pins qwen35moe's router to
// RouterKind::sigmoid, but q36ref.cu / q36_engine.cu route the MoE with a SOFTMAX over all 256
// experts, top-8, renormalized (q36ref.cu:21, :655-680; q36_engine.cu:306 k_route_group uses
// softmax). This graph therefore calls the shared SOFTMAX router u::k::router_topk, NOT
// router_sigmoid. The sigmoid in qwen35moe is only the shared-expert SCALAR gate. The box/substrate
// lane should correct u_geometry's qwen35moe router to softmax (see qwen35moe_graph.cu TODO-R).
//
// HOST/BOX SPLIT (as the Raon lane): this header is host-includable (only host types cross the
// factory seam). The method bodies and all CUDA state live in qwen35moe_graph.cu, built only behind
// STRATA_U_CUDA on the owner's GPU box. On a host/CI build the factory is not linked and
// u::make_family_graph returns the host stub.
#pragma once

#include "u_family.hpp"
#include "u_geometry.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace u {

// The qwen35moe-specific geometry the graph's kernels need, beyond the common u::Geometry (which
// u_geometry already guards). The common, guarded fields are copied in; the GDN/attention-internal
// dims, the layer schedule interval, the shared-expert width, n_rot/eps and the MTP-layer count are
// read from the qwen35moe.* metadata in qwen35moe_graph.cu (Qwen35MoeGeometry::from_gguf), mirroring
// strata::core::geometry_from_gguf (upstream/include/strata/core/family_layout.hpp:30). The defaults
// below are Qwen3.6-35B-A3B's published shape so the layout is reviewable before the box run.
struct Qwen35MoeGeometry final {
  // --- common (guarded, copied from u::Geometry) ---
  int n_embd = 0;        // embedding_length
  int n_head = 16;       // attention.head_count (full-attention query heads)
  int n_head_kv = 2;     // attention.head_count_kv (NOT in the common geometry; read from qwen35moe.*)
  int head_dim = 256;    // attention.key_length (== value_length)
  int n_vocab = 0;       // vocab_size
  int n_expert = 256;    // expert_count
  int n_exp_used = 8;    // expert_used_count (top-k)
  int n_shared = 1;      // expert_shared_count (always-on shared experts; qwen35moe = 1)
  int n_ff_exp = 0;      // expert_feed_forward_length (MoE expert intermediate)

  // --- block schedule ---
  int block_count = 0;   // qwen35moe.block_count (TRUNK + MTP; the key counts both — upstream
                         // family_layout.hpp:30 does n_layers = block_count - n_mtp_layers)
  int n_mtp = 0;         // qwen35moe.nextn_predict_layers (0 or 1; the MTP block is blk.<n_layers>)
  int n_layers = 0;      // the trunk layer count = block_count - n_mtp
  int qsa_interval = 4;  // full_attention_interval: layer l is full attention iff l % qsa == qsa-1

  // --- GDN (Gated-DeltaNet linear attention) ---
  int ssm_d_conv = 4;         // ssm.conv_kernel (the causal conv taps; history is d_conv-1 columns)
  int ssm_state_size = 128;   // ssm.state_size (the key/value head width, and the DxD state side) = D
  int ssm_k_heads = 16;       // ssm.group_count (q heads and k heads) = HK
  int ssm_v_heads = 32;       // ssm.time_step_rank (v heads; also the gate z heads) = HV
  int ssm_value_dim = 4096;   // ssm.inner_size (= HV*D; the gate z width, and gdn_recurrence's y)
  int ssm_conv_channels = 0;  // derived: 2*HK*D + HV*D (the W_qkv output width C), = 8192

  // --- full attention ---
  int n_rot = 64;             // rope.dimension_count (the rotated slice of head_dim)
  double rope_base = 1e7;     // rope.freq_base (common u::Geometry also carries this)

  // --- MoE / shared expert ---
  int n_ff_shexp = 0;         // expert_shared_feed_forward_length (shared-expert intermediate)
  // The router is SOFTMAX top-k renormalized (see the ROUTER NOTE above). RouterKind in the common
  // geometry is wrong for this family; the graph uses u::k::router_topk regardless.

  // --- norms ---
  float eps = 1e-6f;          // attention.layer_norm_rms_epsilon

  [[nodiscard]] bool is_attn(int il) const { return il % qsa_interval == qsa_interval - 1; }
  [[nodiscard]] int d_conv_hist() const { return ssm_d_conv - 1; }  // conv-history columns per channel

  // Reads the qwen35moe.* metadata (beyond the common u::Geometry) into `out`, applying the published
  // defaults and deriving ssm_conv_channels and n_layers. Fails closed with `err` on a missing or
  // ill-shaped key. Defined in qwen35moe_graph.cu. Needs the parsed GgufFile (the factory seam below).
  static bool from_gguf(const GgufFile& file, const Geometry& common, Qwen35MoeGeometry& out,
                        std::string& err);
};

// The Qwen3.6-35B-A3B graph. CUDA state is held as raw device pointers (host-typed), so the class
// declaration is host-includable; the bodies are in the .cu, behind STRATA_U_CUDA.
class Qwen35MoeGraph final : public FamilyGraph {
 public:
  ~Qwen35MoeGraph() override;
  Qwen35MoeGraph(const Qwen35MoeGraph&) = delete;
  Qwen35MoeGraph& operator=(const Qwen35MoeGraph&) = delete;

  [[nodiscard]] FamilyId family() const override { return FamilyId::qwen35moe; }
  [[nodiscard]] std::uint32_t context() const override { return context_; }
  [[nodiscard]] bool available() const override { return loaded_; }
  [[nodiscard]] std::string describe() const override;
  [[nodiscard]] bool generate(const Gen& request, const LineSink& sink, std::string& error) override;
  using FamilyGraph::load;  // the private load(Geometry,...) below is an overload, not a hidden virtual (nvcc #997-D)

  Qwen35MoeGraph() = default;

 private:
  bool load(const Geometry& common, std::uint32_t context, std::string& err);
  // One token (or a prompt chunk of T tokens) through the trunk; the last row's logits land in sc_.
  bool forward(const std::int32_t* tokens, int n_tokens, std::int64_t pos0, std::string& err);
  void reset_sequence();  // zero every GDN recurrent state + conv history; drop the KV caches

  // Per-layer pieces (each writes its mixer/MoE result back into the residual x). See the .cu.
  bool layer_gdn(int il, int T, std::string& err);   // GDN linear-attention mixer (30 of 40 layers)
  bool layer_attn(int il, int T, std::int64_t pos0, std::string& err);  // full attention (10 layers)
  bool layer_moe(int il, int T, std::string& err);   // 256 top-8 softmax MoE + sigmoid-gated shared

  // The MTP draft block (decision: --mtp N). Structured per the production; see the .cu.
  bool forward_mtp(int T, std::int64_t pos0, const float* seeds, bool draft, std::string& err);
  int spec_step(int x_tok, const std::vector<std::int32_t>& fix, int draft_n,
                std::vector<std::int32_t>& drafts, std::int32_t* picks, std::string& err);

  Qwen35MoeGeometry g_{};
  std::uint32_t context_ = 0;
  bool loaded_ = false;
  std::int64_t pos_ = 0;  // sequence position (tokens consumed since the last reset)
  int mtp_d_ = 0;         // --mtp N: drafts per speculative step (0 = plain decode)

  // Device weights + arenas (float*/void* are DEVICE pointers; the shared loader fills them on the
  // box). Named like the production Model/Layer (q36_engine.cu:835).
  std::map<std::string, const void*> w_;  // tensor name -> device pointer (quantized weights)
  std::map<std::string, const float*> f_; // small f32 vectors (norms, ssm_a, dt, conv taps, router)
  void* w_arena_ = nullptr;               // the weight blob (experts + dense matrices)
  float* state_ = nullptr;                // GDN recurrent state S (per GDN layer) + conv history
  float* kv_ = nullptr;                   // KV caches (per full-attention layer)
  float* sc_ = nullptr;                   // per-forward activation/router/expert/logits scratch
  void* expert_tier_ = nullptr;           // u::ExpertTier* (opaque here; the .cu owns the type)
  void* stream_ = nullptr;                // cudaStream_t

  // Per-layer state offsets into state_/kv_ (floats; -1 if the layer has no such state). Sized in
  // load() so the layout is reviewable; the device allocation lands with the shared loader.
  std::vector<std::int64_t> gdn_state_, gdn_conv_, kv_k_, kv_v_;

  friend std::unique_ptr<FamilyGraph> make_qwen35moe_graph(const Geometry&, std::uint32_t, std::string&);
};

// The Qwen3.6-35B-A3B factory (the design contract). Builds and loads the graph for a guarded
// geometry; returns nullptr with `error` set on a load failure. The dispatch wires this into the
// CUDA branch of u::make_family_graph for FamilyId::qwen35moe (that wiring already exists:
// u_family.hpp:93/:100) — but load() needs the parsed GgufFile, which the factory signature does not
// yet carry; see the seam TODO in the .cu.
[[nodiscard]] std::unique_ptr<FamilyGraph> make_qwen35moe_graph(const Geometry& geom,
                                                               std::uint32_t context, std::string& error);

}  // namespace u
