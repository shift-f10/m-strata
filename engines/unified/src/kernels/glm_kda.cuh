// engines/unified/src/kernels/glm_kda.cuh - the glm5-next KDA (Kimi Delta Attention) layer pieces,
// the GLM-specific linear-attention mixer for the unified engine.
//
// PORTED FROM (reviewed, verbatim math, renamed into the unified engine's u::glm namespace):
//   engines/maya-ref/include/strata/kernels/glm_kda.hpp   (the math + layout contracts)
//   engines/maya-ref/src/kernels/cuda/glm_kda.cu          (the five kernels)
// Reference spec: engines/maya-ref/ref/glm.py::kda_layer / conv1d_causal, transcribed from
//   engines/maya-ref/ref/upstream-llama.cpp/glm5-next.cpp L687-763 and ggml_gated_delta_net.
//
// glm5-next's 34 linear-attention layers (of 45) are KDA: the gated-delta net with the per-head
// scalar decay replaced by a PER-CHANNEL decay on the KEY axis, produced by a low-rank gate and
// squashed through the architecture's lower-bound form. Per token, per head, with S the (dk, dv)
// state (dk = dv = head_dim here):
//
//     S     = diag(exp(g1)) @ S      the gate scales the KEY axis
//     delta = beta * (v - S^T k)     the delta correction (what makes it delta-net, not linear)
//     S    += k (delta)^T            the outer-product state update
//     o     = (1/sqrt(dv)) * S^T q   the 1/sqrt(S_v) scale is INSIDE the recurrence kernel
//
// THE GATE (the graph does the two low-rank GEMVs via the shared matmul; these kernels do the
// shapes and the squash):
//     g  = f_b^T (f_a^T x) + dt_b          low-rank: rank = head_dim, not a direct d_inner proj
//     g1 = lower_bound * sigmoid(-g * A)   A = ssm_a holds -exp(A_log) (negative), so
//                                          exp(g1) lands in (e^lb, 1), per KEY-axis channel
// The output gate g2 = g_b^T (g_a^T x) is the same low-rank trick, applied as
// rms_norm(o, ssm_norm) * sigmoid(g2) per head (glm_kda_out_gate).
//
// LAYOUT CONTRACTS (everything f32, ggml order - features fastest, no transposes between the shared
// matmul's outputs and these kernels):
//   x_proj / conv out : (d_inner, T)                 element (c, t) at c + d_inner*t
//   conv_w            : (d_conv, d_inner)             element (tap, c) at tap + d_conv*c
//   conv_state        : (d_conv-1, d_inner)          history rows, OLDEST first; (j, c) at j + (d_conv-1)*c
//   q, k, v, g1, out  : (head_dim, n_head, T)        element (e, h) at e + head_dim*h (+ T stride)
//   beta              : (n_head, T)                   element (h, t) at h + n_head*t
//   state             : (n_head, head_dim, head_dim)  S[h] row-major, row = KEY axis; updated in place
//
// DECODE-FIRST: the recurrence kernel carries the state across T tokens inside one block per head,
// which is both the decode shape and the teacher-forced-prefill shape; the serve path calls it with
// T=1 per token (see family/glm_graph.cu).
#pragma once

#include <cstdint>

namespace u::glm {

/// silu(conv1d(x_proj)) for ONE stream (call three times, for q/k/v), with the (d_inner, d_conv-1)
/// history updated in place. Output position t's window is x_proj[t-(d_conv-1) .. t]; taps older
/// than the sequence come from the state, oldest first.
void glm_kda_conv(const float* x_proj, const float* conv_w, float* conv_state, int d_inner, int d_conv,
                  int T, float* out, void* stream);

/// The decay gate's squash: g1[e, h, t] = lower_bound * sigmoid(-g[e + head_dim*h, t] * A[h]).
/// `g` is (d_inner, T) - the f_b GEMV output plus dt_b, in ggml head order.
void glm_kda_gate(const float* g, const float* ssm_a, float lower_bound, int head_dim, int n_head, int T,
                  float* g1, void* stream);

/// The recurrent delta rule over T tokens, per head, state and conv carried in place. `out` includes
/// the 1/sqrt(head_dim) scale. `state` is (n_head, head_dim, head_dim), zeroed by the caller for a
/// fresh sequence.
void glm_kda_recurrence(const float* q, const float* k, const float* v, const float* g1, const float* beta,
                        float* state, int n_head, int head_dim, int T, float* out, void* stream);

/// rms_norm(o, ssm_norm) * sigmoid(g2), per head: `o` and `g2` are (head_dim, n_head, T), `norm_w`
/// is (head_dim,), `eps` is the model's rms epsilon.
void glm_kda_out_gate(const float* o, const float* norm_w, const float* g2, int head_dim, int n_head, int T,
                      float eps, float* gated, void* stream);

/// GDN L2-normalize per head: x / sqrt(||x||^2 + eps) over each head_dim column, in place is allowed
/// (out may alias x). Used on KDA's q and k conv outputs (v is NOT normalized). Ported from
/// glm_model.cu's l2_heads_kernel (ref/glm.py::gdn_l2_norm). This is a GLM-lineage helper kept with
/// the KDA kernels; qwen's GDN shares the same primitive - if the substrate later exposes a generic
/// u::k::l2_norm_heads, the graph can call that instead (the math is identical).
void glm_l2_norm_heads(const float* x, float* out, int head_dim, int n_head, int T, float eps, void* stream);

/// In place: x[i] = sigmoid(x[i]) over n elements. KDA's beta = sigmoid(ssm_beta x). The shared
/// common.cuh exposes silu/gelu but not a bare sigmoid, so this GLM-lineage helper (the reference's
/// sigmoid_kernel) lives with the KDA kernels. The DSA router's sigmoid is inside u::k::router_sigmoid.
void glm_sigmoid_inplace(float* x, int n, void* stream);

}  // namespace u::glm
