// strata-unified-engine: the Raon-1-mini (arch "raon1") per-family model graph.
//
// Raon-1-mini is a NEW family (no reference engine exists) but architecturally a near-twin of
// GLM-5.3-Flash, so this graph is modeled on engines/maya-ref/src/core/glm_model.cu and reuses the
// GLM-shaped kernels: RDA (Raon Delta Attention) is GLM's KDA gated-delta LINEAR attention
// (src/kernels/raon1_rda.*, ported from glm_kda), and Gated-MLA is GLM's absorbed-MLA latent
// attention (src/kernels/raon1_mla.*, ported from glm_dsa's MLA path) with Raon's dims and NoPE.
// The MoE (128 experts, 6 routed + 1 shared, sigmoid router + renormalize) rides the shared
// expert-tiering facility (u_experts.hpp) and the shared router (kernels/common.cuh).
//
// Published shape (design doc §0 + Raon notes; u_geometry.cpp PINS the common fields exactly):
//   28.8B total / 4.3B active MoE; 25 layers; hidden 2560; 40 attention heads; head_dim 128;
//   dense intermediate 12032; moe_intermediate 1088; 128 routed experts, 6 routed + 1 shared per
//   token; SIGMOID router, renormalize; first_k_dense_replace = 1 (layer 0 is a dense MLP);
//   vocab 259840; context 262144. NoPE (no rotary anywhere).
//   Attention is a HYBRID: 18 of 25 layers RDA (gated-delta linear), 7 Gated MLA (latent/MLA,
//   QK 192 / V 128, query/KV LoRA ranks 576 / 512). Ships a DFlash-style draft block.
//
// HOST/BOX SPLIT: this header is host-includable (the dispatch calls make_raon1_graph through it
// with only host types). The method bodies and the CUDA state live in raon1_graph.cu, built only
// behind STRATA_U_CUDA on the owner's GPU box (design §1.3). On a host/CI build the family factory
// is not linked and u::make_family_graph returns the host stub.
#pragma once

#include "u_family.hpp"
#include "u_geometry.hpp"
#include "u_gguf.hpp"       // u::GgufFile (the load() seam parameter; host-safe header)
#include "u_experts.hpp"    // u::ExpertTier + the shared u::WSlot

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace u {

// The Raon-specific geometry the graph's kernels need, beyond the common u::Geometry (which
// u_geometry already pins). Mirrors GLM's Glm5Geometry (engines/maya-ref/include/strata/core/
// glm_model.hpp): the common fields come from u::Geometry; the attention-internal dims and the
// per-layer attention-type array are read from the raon1.* metadata in raon1_graph.cu, with the
// published defaults below and TODOs where the exact GGUF key is to be confirmed on the box.
struct Raon1Geometry final {
  // --- mirrored from the guarded u::Geometry (published, pinned) ---
  int n_embd = 2560;
  int n_layers = 25;
  int n_head = 40;       // attention heads (RDA heads; MLA query heads)
  int head_dim = 128;    // RDA head dim AND the MLA value head dim (V 128)
  int n_vocab = 259840;
  int n_ff_dense = 12032;  // layer-0 dense MLP intermediate
  int n_ff_exp = 1088;     // MoE (and shared-expert) intermediate
  int n_expert = 128;
  int n_exp_used = 6;      // routed experts per token
  int n_shared = 1;        // always-on shared expert
  int dense_lead = 1;      // first_k_dense_replace: layers [0, dense_lead) are dense MLP

  // --- Gated-MLA latent attention (7 of 25 layers), confirm key names on the box ---
  int q_lora = 576;    // query down-projection rank
  int kv_lora = 512;   // KV latent rank (the cached latent width per token)
  int qk_nope = 192;   // query-key head dim — ENTIRELY nope (NoPE family); also the score scale
  int v_head = 128;    // value head dim (== head_dim)

  // --- RDA gated-delta linear attention (18 of 25 layers) ---
  int d_conv = 4;            // TODO(box): confirm the short-conv tap count, or 0/absent if no conv
  float rda_lower_bound = -5.0f;  // TODO(box): confirm the decay-gate lower bound (GLM kda_lb = -5)

  // --- MoE routing / norms ---
  RouterKind router = RouterKind::sigmoid;
  bool router_renormalize = true;  // renormalize the top-k routed weights to sum 1 (design doc)
  float w_scale = 1.0f;            // TODO(box): confirm raon1.expert_weights_scale
  float swiglu_clamp = 0.0f;       // TODO(box): confirm a SwiGLU clamp limit (GLM clamps at 10); 0 = none
  float norm_eps = 1e-5f;          // TODO(box): confirm raon1.attention.layer_norm_rms_epsilon

  // --- the DFlash-style draft block (design decision 5) ---
  int nextn = 0;  // draft layers counted inside block_count (as GLM's nextn); set from metadata

  // Per-layer attention-type array: head_count_kv[il] == 0 marks an RDA (linear) layer, nonzero a
  // Gated-MLA layer — exactly GLM's is_recr convention (glm_model.hpp: is_recr = n_head_kv==0).
  // 18 zeros + 7 nonzeros over layers 1..24 (layer 0 is dense MLP regardless of attention type).
  std::vector<int> head_count_kv;

  [[nodiscard]] bool is_rda(int il) const {
    return il >= 0 && il < (int)head_count_kv.size() && head_count_kv[(size_t)il] == 0;
  }
  [[nodiscard]] bool is_dense_mlp(int il) const { return il < dense_lead; }
  [[nodiscard]] int d_inner() const { return head_dim * n_head; }  // RDA projected width

  // Reads the raon1.* metadata (beyond the common u::Geometry) into `out`, applying the published
  // defaults and the per-layer head_count_kv array. Fails closed with `err` on a missing/ill-typed
  // key. Defined in raon1_graph.cu.
  static bool from_gguf(const GgufFile& file, const Geometry& common, Raon1Geometry& out,
                        std::string& err);
};

// The Raon-1-mini graph. One shared substrate drives it through u::FamilyGraph; the per-layer
// schedule and the RDA/Gated-MLA/MoE/draft pieces are Raon's. CUDA state is held as raw device
// pointers (host-typed), so the class declaration is host-includable; the bodies are in the .cu.
class Raon1Graph final : public FamilyGraph {
 public:
  ~Raon1Graph() override;
  Raon1Graph(const Raon1Graph&) = delete;
  Raon1Graph& operator=(const Raon1Graph&) = delete;

  [[nodiscard]] FamilyId family() const override { return FamilyId::raon1; }
  [[nodiscard]] std::uint32_t context() const override { return context_; }
  [[nodiscard]] bool available() const override { return loaded_; }
  [[nodiscard]] std::string describe() const override;
  [[nodiscard]] bool generate(const Gen& request, const LineSink& sink, std::string& error) override;

  // The weights seam: the substrate calls this with the merged multi-part GGUF right
  // after the factory returns (FamilyGraph::load, u_family.hpp:48 / u_main.cpp:423 —
  // the same seam GlmGraph::load / Ds4Graph::load proved or landed). It reads the
  // raon1.* geometry metadata, uploads the non-expert tensors, registers the routed
  // experts with the shared tier (+ residency) and carves the RDA/MLA state arena.
  // The per-layer mixers land next (their kernels are already ported); until then
  // forward()/generate() fail closed — honestly, after a real load. available()
  // reports true once this load lands the weights.
  [[nodiscard]] bool load(const GgufFile& gguf, const std::uint8_t* mapped, std::string& error) override;

  // Built by make_raon1_graph once load() has succeeded.
  Raon1Graph() = default;

 private:
  // Weights, state and forward are defined in raon1_graph.cu (all CUDA).
  // One token (or a prompt batch of T tokens) through all layers; writes the last token's logits.
  bool forward(const std::int32_t* tokens, int n_tokens, std::string& err);
  void reset_sequence();  // zero every RDA recurrence state + conv history; drop the MLA latent cache

  // One decoder layer: dense MLP (layer < dense_lead) or MoE, over the RDA or Gated-MLA mixer.
  bool layer_rda(int il, int T, std::string& err);         // RDA linear-attention mixer
  bool layer_mla(int il, int T, std::string& err);         // Gated-MLA latent-attention mixer
  bool layer_ffn(int il, int T, std::string& err);         // dense MLP or routed+shared MoE

  Raon1Geometry g_{};
  Geometry geom_{};        // the guarded common geometry (from make_raon1_graph; keys read at load)
  std::uint32_t context_ = 0;
  bool loaded_ = false;
  std::int64_t pos_ = 0;  // tokens consumed since the last reset

  // Device arenas (float* = device pointers; populated on the box). Named like GLM's glm_model.
  std::map<std::string, WSlot> w_;  // uploaded (non-expert) tensor name -> device slot (u_experts WSlot)
  std::unique_ptr<ExpertTier> experts_;  // the shared tier (registered path + residency), like GLM's
  float* w_arena_ = nullptr;
  float* state_ = nullptr;   // RDA recurrence S + conv histories, MLA latent caches
  std::size_t state_bytes_ = 0;
  float* sc_ = nullptr;      // per-forward scratch (activations, router, expert outputs, logits);
                             // sized WITH the mixer wiring (next unit) — layout not final yet
  int* d_pos_ = nullptr;     // query cache positions, on device (mla_attention reads them)

  // Per-layer state offsets into state_ (floats; -1 if the layer has no such state). Mirrors GLM.
  std::vector<std::int64_t> rda_state_, rda_conv_, mla_latent_;

  friend std::unique_ptr<FamilyGraph> make_raon1_graph(const Geometry&, std::uint32_t, std::string&);
};

// The Raon-1-mini factory (design contract). Builds and loads the graph for a guarded geometry;
// returns nullptr with `error` set on a load failure. The dispatch wires this into the CUDA branch
// of u::make_family_graph for FamilyId::raon1 (that wiring is a shared-file change, not this lane).
[[nodiscard]] std::unique_ptr<FamilyGraph> make_raon1_graph(const Geometry& geom,
                                                            std::uint32_t context, std::string& error);

}  // namespace u
