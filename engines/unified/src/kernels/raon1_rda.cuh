// strata-unified-engine: Raon-1-mini RDA ("Raon Delta Attention") — the gated-delta LINEAR
// attention used on 18 of the 25 layers. This is Raon's near-twin of GLM-5.3-Flash's KDA
// (engines/maya-ref/src/kernels/cuda/glm_kda.cu): the same short causal conv + gated-delta
// outer-product recurrence + RMS-gated output, re-expressed on the unified engine's host-only
// launcher convention (void* stream, so the header stays includable without cuda_runtime).
//
// GLM ANALOGY (piece by piece — the math is copied from glm_kda, the dims are Raon's):
//   rda_conv       <- glm_kda_conv / glm_kda_conv_slide : a depthwise causal conv over d_conv
//                     taps with a rolling history column store, then SiLU. One thread per
//                     (channel, token); the slide is a second kernel to avoid the in-place race
//                     GLM documents.
//   rda_gate       <- glm_kda_gate : the per-(head,element) decay gate, reshaping (d_inner,T)
//                     into (head_dim,n_head,T). g1[i] = lower_bound * sigmoid(-g[i]*decay_a[h]).
//   rda_recurrence <- glm_kda_recurrence : ONE BLOCK PER HEAD, tokens looped inside the block so
//                     the sequential state dependency never leaves the SM. Per token:
//                       S[i][:] *= exp(g1[i])           (gate scales the KEY axis)
//                       u[j]     = sum_i S[i][j]*k[i]   (= S^T k)
//                       delta[j] = beta*(v[j]-u[j])
//                       S[i][j] += k[i]*delta[j]        (delta-rule outer update)
//                       o[j]     = (1/sqrt(dk)) * sum_i S[i][j]*q[i]
//   rda_out_gate   <- glm_kda_out_gate : RMS over head_dim, then o*inv*norm_w*sigmoid(g2).
//
// All arithmetic is f32 (GLM keeps the KDA gates/router/recurrence in F32 for parity; Raon
// inherits that contract). The box checks these against the real Raon-1-mini weights.
//
// OPEN TODOs for the GPU box (do not guess silently — the structure is GLM's; confirm Raon's):
//   - Whether RDA carries the d_conv short conv at all, and its tap count (GLM KDA uses 4,
//     qwen35moe GDN uses 4). rda_conv is written for the GLM layout; drop the call in the graph
//     if Raon's GGUF ships no conv_w / conv-window metadata.
//   - The decay gate's exact form: GLM's lower_bound (kda_lb = -5) and per-head decay_a (ssm_a)
//     with g1 = lb*sigmoid(-g*a). Confirm Raon uses the same squash and sign.
//   - beta's source (GLM reads a per-(token,head) beta projected + sigmoid'd). Confirm.
#pragma once

#include <cstdint>

namespace u::raon {

// Depthwise causal conv over d_conv taps on the projected channels (d_inner = head_dim*n_head),
// then SiLU, writing `out` (d_inner, T). `conv_state` holds the last d_conv-1 columns and is slid
// forward in place after the reads. Mirrors glm_kda_conv (+ _slide). No-op for d_conv <= 1.
void rda_conv(const float* x_proj, const float* conv_w, float* conv_state, int d_inner, int d_conv,
              int T, float* out, void* stream);

// The decay gate: g1[e,h,t] = lower_bound * sigmoid(-g[e,h,t] * decay_a[h]), reshaping g's
// (d_inner, T) layout into (head_dim, n_head, T). Mirrors glm_kda_gate.
void rda_gate(const float* g, const float* decay_a, float lower_bound, int head_dim, int n_head,
              int T, float* g1, void* stream);

// The gated-delta recurrence (one block per head, tokens inside). `state` is n_head*head_dim*head_dim
// floats, carried across calls (decode) and reset per sequence. Mirrors glm_kda_recurrence.
void rda_recurrence(const float* q, const float* k, const float* v, const float* g1,
                    const float* beta, float* state, int n_head, int head_dim, int T, float* out,
                    void* stream);

// The RMS-normed, sigmoid-gated output write: gated = o*rsqrt(mean(o^2)+eps)*norm_w*sigmoid(g2).
// Mirrors glm_kda_out_gate.
void rda_out_gate(const float* o, const float* norm_w, const float* g2, int head_dim, int n_head,
                  int T, float eps, float* gated, void* stream);

}  // namespace u::raon
