// include/strata/core/raon1_geometry.hpp - the raon1 (Raon-1-mini) model geometry, parsed from the GGUF's
// raon1.* metadata.
//
// raon1 is a GLM-5.3-Flash variant served by strata_glm (see docs/superpowers/specs/2026-10-25-raon1-mini-port-
// blueprint.md): a per-layer HYBRID of RDA (a gated-delta SSM mixer; is_recr layers, n_head_kv == 0) and
// Gated-MLA (DeepSeek-style latent attention with a decoupled-RoPE split and an output gate; n_head_kv != 0),
// over a sigmoid-router MoE with a shared expert and 4-stream hyper-connections.  It REUSES strata_glm's kernel
// library; this struct is the raon1 twin of Glm5Geometry, differing in: a decoupled-RoPE dim on the MLA layers
// (qk_rope), the `situ` activation (replacing GLM's clamped SwiGLU), a full-rank KDA output gate plus an extra
// per-head rda projection, and NO sparse indexer.  Every default below is the value confirmed from the shipped
// Raon-1-mini-preview Q8_0 GGUF (731 tensors); from_gguf overrides each from the file's own metadata.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace strata {
class GgufFile;
}

namespace strata::core {

// Members are 0 / "unset" by default: the geometry comes from the GGUF, never from hardcoded constants, so a
// reader that fails to parse a key is caught rather than silently fabricating a shape.  The confirmed
// Raon-1-mini-preview values (what from_gguf should produce) are in the trailing comments.
struct Raon1Geometry {
    int n_embd = 0, n_layers = 0, n_head = 0, n_vocab = 0;   // 2560, 25, 40, 259840
    int64_t n_ctx_train = 0;                                 // 262144

    // Per layer: 0 marks an RDA (recurrent) layer, nonzero a Gated-MLA layer.  Confirmed pattern is
    // [0,0,0,1] x6 then 1 -> 18 RDA + 7 MLA {3,7,11,15,19,23,24}; the mixer split spans all n_layers
    // (layer 0 is an RDA mixer with a dense FFN, since dense_lead applies to the FFN tier only).
    std::vector<int> n_head_kv;

    // Gated-MLA (absorbed DeepSeek-style latent attention, NoPE).  qk = attention.key_length_mla =
    // qk_nope + qk_rope.  Per the reference impl (ik_llama.cpp PR #2733 build_raon1.cpp), raon1 is NoPE: the
    // qk_rope (rope.dimension_count) channels are carried as EXTRA pass-through K/Q channels and are NOT rotated;
    // the kv cache row width is kv_lora + qk_rope (= 576).  v_head == qk_nope holds (128 == 128).
    int q_lora = 0, kv_lora = 0;                             // 576, 512
    int qk = 0, qk_rope = 0, v_head = 0;                     // 192, 64 (extra NoPE channels), 128
    float latent_norm_eps = 0.0f;    // 1e-6 (attention.latent_norm_rms_epsilon; attn_kv_a_norm / attn_q_a_norm)
    bool mla_gate = true;            // the Gated-MLA output gate (attn_gate); raon1 has NO sparse indexer

    // RDA (gated-delta SSM mixer, reusing the KDA building blocks).
    int kda_head_dim = 0, d_conv = 0, dt_rank = 0;           // 128, 4, 128 (dt_rank = ssm.time_step_rank)
    float kda_lb = 0.0f;                                     // -5 (kda.gate_lower_bound)

    // MoE (sigmoid router + renormalize + one shared expert); dense FFN on the leading layer(s).
    int n_expert = 0, n_exp_used = 0, n_ff_exp = 0, n_shared = 0, n_ff_dense = 0, dense_lead = 0;  // 128,6,1088,1,12032,1
    float w_scale = 0.0f;            // 1.0 (expert_weights_scale)
    int norm_w = 0;                  // 1 (expert_weights_norm; renormalize the top-k gates)
    int gating_func = 0;             // 2 = sigmoid (expert_gating_func)

    // The `situ` activation (replaces GLM's clamped SwiGLU), in both the dense and expert MLPs.
    float situ_beta = 0.0f, situ_linear_beta = 0.0f;         // 4.0, 25.0

    // Hyper-connections (4 streams; the mHC kernels pin 4).
    int hc = 0;                                              // 4
    float hc_eps = 0.0f, hc_magnitude = 0.0f, norm_eps = 0.0f;  // 1e-5, 2.0, 1e-5

    int qk_nope() const { return qk - qk_rope; }
    int d_inner() const { return kda_head_dim * n_head; }
    bool is_recr(int il) const { return n_head_kv[(size_t) il] == 0; }

    // Parses general.architecture == "raon1" metadata into `out`.  Returns false with `err` set when a key is
    // malformed or a value is outside what the runner supports (sigmoid gating, hc == 4, qk_nope == v_head).
    static bool from_gguf(const strata::GgufFile& g, Raon1Geometry& out, std::string& err);
};

}  // namespace strata::core
