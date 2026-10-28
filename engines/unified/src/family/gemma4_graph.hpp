// strata-unified-engine: the Gemma-4-26B-A4B (arch "gemma4") per-family model graph.
//
// Gemma4 is RE-EXPRESSED onto the shared kernel set (owner decision 2, design §3.2/§3.5: unify
// gemma4, NO cuBLAS-module carve-out). The 26/26-verified, llama.cpp-parity standalone engine at
// engines/gemma4/ is the READ-ONLY port reference and parity oracle; this lane maps its math onto
// kernels/common.cuh (u::k::) + u_experts.hpp (u::ExpertTier) and keeps only the genuinely
// gemma4-specific kernels in src/kernels/gemma4_attn.* and src/kernels/gemma4_glue.*.
//
// Published text-only shape (engines/gemma4/src/g4_model.hpp; the unified geometry guard pins the
// common fields and refuses anything else):
//   30 layers; hidden 2816; 16 attention heads; GQA 8 KV (sliding) / 2 KV (full); two head widths
//   256 (sliding) / 512 (full); 5 full layers out of 30 (V-FROM-K: full layers carry no attn_v,
//   V is the K projection normed-but-not-roped); DUAL RoPE (sliding NEOX base 1e4; full
//   PROPORTIONAL base 1e6 with the rope_freqs factors); attention scale 1.0; per layer BOTH a
//   dense GeGLU MLP (ff 2112) AND a SOFTMAX top-8 routed MoE (128 experts, ff 704) combined by a
//   double-sandwich norm; tied Q8_0 head; final logit soft-cap 30. No mmproj (text only).
//
// HOST/BOX SPLIT: this header is host-includable (the dispatch reaches make_gemma4_graph through
// it with host types only — the device state is a PIMPL). The method bodies and all CUDA live in
// gemma4_graph.cu, built only behind STRATA_U_CUDA on the owner's GPU box. On a host/CI build the
// factory is not linked and u::make_family_graph returns the host stub. Do NOT attempt a CUDA
// build here (no GPU); the box compiles and parities this against engines/gemma4/.
#pragma once

#include "u_family.hpp"
#include "u_geometry.hpp"
#include "u_gguf.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace u {

// The gemma4-specific geometry beyond the common (guarded) u::Geometry. The guard pins the common
// fields (block_count 30, embedding 2816, head_count 16, key_length 512, expert_count 128,
// expert_used 8, expert_feed_forward 704, feed_forward 2112, vocab 262144, softmax router, rope);
// the per-layer sliding/full pattern, the two head widths, the KV-head split, the two RoPE bases,
// the soft-cap and the special-token ids come from the gemma4.* metadata and are read by
// from_gguf(). Member initializers are the published 26B-A4B values (documentation + a floor for a
// GGUF that omits an optional key); from_gguf() overwrites them from the file and fails closed on a
// missing/ill-typed REQUIRED key. Mirrors engines/gemma4/src/g4_model.hpp (ModelDesc/LayerDesc).
struct Gemma4Geometry final {
  // --- mirrored from the guarded u::Geometry (pinned) ---
  int n_layers = 30;
  int n_embd = 2816;
  int n_head = 16;
  int n_ff = 2112;        // dense MLP intermediate (GeGLU)
  int n_ff_exp = 704;     // MoE expert intermediate
  int n_expert = 128;
  int n_exp_used = 8;     // routed experts per token (softmax top-8)
  int n_vocab = 262144;

  // --- gemma4.* specifics (from_gguf) ---
  int head_swa = 256;     // key_length_swa / value_length_swa (sliding layers)
  int head_full = 512;    // key_length / value_length (full layers)
  int kv_swa = 8;         // attention.head_count_kv on a sliding layer
  int kv_full = 2;        // attention.head_count_kv on a full layer
  int window = 1024;      // attention.sliding_window
  int rope_freqs_len = 256;  // rope_freqs vector length (one factor per rotated pair of a 512 head)

  float rms_eps = 1e-6f;          // attention.layer_norm_rms_epsilon
  float rope_base_full = 1e6f;    // rope.freq_base
  float rope_base_swa = 1e4f;     // rope.freq_base_swa
  float softcap = 30.0f;          // final_logit_softcapping
  std::int64_t context_train = 262144;  // context_length

  std::int32_t bos_id = 2;
  std::int32_t eos_id = 1;
  std::int32_t turn_close_id = 106;  // the "<turn|>" token, found by its literal in the vocab

  // Per-layer sliding/full pattern (attention.sliding_window_pattern, 30 bools: true = sliding).
  // head_count_kv must agree (kv_swa on sliding, kv_full on full). Empty until from_gguf() runs.
  std::vector<std::uint8_t> sliding;  // 1 = sliding layer, 0 = full layer

  [[nodiscard]] bool is_sliding(int il) const {
    return il >= 0 && il < (int)sliding.size() && sliding[(std::size_t)il] != 0;
  }
  [[nodiscard]] int head_dim(int il) const { return is_sliding(il) ? head_swa : head_full; }
  [[nodiscard]] int kv_heads(int il) const { return is_sliding(il) ? kv_swa : kv_full; }

  // Reads the gemma4.* metadata (beyond the common u::Geometry) into `out`, pinning the common
  // fields from `common` and applying the published defaults for optional keys. Fails closed with
  // `err` on a missing/ill-typed required key or a pattern that is not the 26B-A4B shape. Defined
  // in gemma4_graph.cu. Mirrors g4_model.cpp check_model (64-190).
  static bool from_gguf(const GgufFile& file, const Geometry& common, Gemma4Geometry& out, std::string& err);
};

// Opaque device state (per-layer weights, the f16 KV caches, u::ExpertTier, the scratch arenas and
// the CUDA stream). Defined in gemma4_graph.cu so this header needs no CUDA headers.
struct Gemma4Device;

// The Gemma4 graph. The shared substrate drives it through u::FamilyGraph; the per-layer schedule
// (dense GeGLU MLP + softmax-routed MoE over the sliding/full attention) is gemma4's. CUDA state is
// held behind a PIMPL so this declaration is host-includable.
class Gemma4Graph final : public FamilyGraph {
 public:
  ~Gemma4Graph() override;
  Gemma4Graph(const Gemma4Graph&) = delete;
  Gemma4Graph& operator=(const Gemma4Graph&) = delete;

  [[nodiscard]] FamilyId family() const override { return FamilyId::gemma4; }
  [[nodiscard]] std::uint32_t context() const override { return context_; }
  [[nodiscard]] bool available() const override { return loaded_; }
  [[nodiscard]] std::string describe() const override;
  [[nodiscard]] bool generate(const Gen& request, const LineSink& sink, std::string& error) override;

 private:
  Gemma4Graph();  // built by make_gemma4_graph
  // The device bring-up (shared loader seam): u_main memory-maps the whole model and calls this once
  // after construction. `mapped` is the GGUF data-section base (file_base + GgufFile::data_offset), so
  // every GgufTensor::offset is relative to it. Mirrors GlmGraph::load, the working idiom in this tree.
  [[nodiscard]] bool load(const GgufFile& gguf, const std::uint8_t* mapped, std::string& err) override;
  // One token at position pos_ through all 30 layers; writes the soft-capped logits for the head.
  // The prompt is consumed the same way (token by token), which keeps the whole forward on the
  // shared kernels; the batched cuBLAS prefill of the reference is a documented box fast-path.
  bool forward_one(std::int32_t id, bool want_logits, std::string& err);  // head() only when want_logits
  // The decode forward body (embed -> 30 layers -> optional head) as ONE enqueue on `st`, reading the
  // token id from tok_pinned and the query position from pos_dev; no host sync, no pos update, no
  // rebalance. Shared by forward_one and the captured decode graph. `st` is a cudaStream_t, passed as
  // void* so this header stays CUDA-free. `attn_pos_for_splits` fixes each layer's attention grid (host)
  // for the largest position the caller will reach; the kernels clamp to the live pos_dev.
  bool enqueue_forward(void* st, bool want_logits, int attn_pos_for_splits, std::string& err);
  // Capture (once) the per-token decode graph sized for `attn_pos_for_splits` and instantiate it. Returns
  // false with `err` set if capture/instantiate is unavailable; the caller then decodes via forward_one.
  bool ensure_decode_graph(int attn_pos_for_splits, std::string& err);
  // BATCHED PREFILL: T prompt tokens at pos_..pos_+T-1 through all 30 layers in one shot (projections
  // and the MoE as per-expert cublasSgemm over dequantised weights, the attention as cublas
  // strided-batched score/output GEMMs around u::gemma4::attn_softmax). The faithful port of the
  // reference's Gemma4Engine::forward_batch (g4_engine.cu:635); it fills the KV caches and the residual
  // but computes NO logits (generate() runs the last <=4 tokens through forward_one for the sampled
  // logits). ids are the raw u32 token ids. Only reached when prefill_ready_ (the scratch fit at load).
  bool forward_batch(const std::uint32_t* ids, int T, std::string& err);
  bool head(std::string& err);          // final norm + tied Q8_0 head + soft-cap
  void reset_sequence();                // drop the KV state (pos_ = 0)

  Gemma4Geometry g_{};
  Geometry common_{};  // the guarded common geometry, stashed by make_gemma4_graph for load()
  std::uint32_t context_ = 0;
  bool loaded_ = false;
  bool prefill_ready_ = false;  // the batched-prefill scratch fit in VRAM at load (else prefill = forward_one)
  std::int64_t pos_ = 0;  // tokens held in the KV caches since the last reset
  int since_rebalance_ = 0;  // forward_one calls since the last live expert-hit rebalance (gate: ~8)
  std::unique_ptr<Gemma4Device> dev_;

  friend std::unique_ptr<FamilyGraph> make_gemma4_graph(const Geometry&, std::uint32_t, std::string&);
};

// The Gemma4 factory (design contract). Builds and loads the graph for a guarded geometry; nullptr
// with `error` set on a load failure. The CUDA branch of u::make_family_graph (u_family.hpp)
// forward-declares and dispatches this for FamilyId::gemma4.
//
// FACTORY SEAM (shared, box reconciles): the graph needs the parsed GgufFile (weights + the
// gemma4.* metadata Gemma4Geometry::from_gguf reads). The host foundation's make_family_graph takes
// only the guarded Geometry, so the GgufFile reaches load() through the CUDA dispatch once the
// shared loader wires it (see the TODO in make_gemma4_graph). Until then load() fails closed and
// the graph reports unavailable — expected on the first box integration build, not a family bug.
[[nodiscard]] std::unique_ptr<FamilyGraph> make_gemma4_graph(const Geometry& geom, std::uint32_t context,
                                                             std::string& error);

}  // namespace u
