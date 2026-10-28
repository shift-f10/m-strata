// engines/unified/src/kernels/qwen4exp_gdn.cuh - Qwen3.8-Flash-Next GDN (Gated-DeltaNet) linear
// attention, the family-specific mixer for the 36 non-QSA layers of qwen4exp.
//
// PORTED FROM (reviewed, verbatim math, renamed into the unified engine's u::qwen4exp namespace):
//   upstream/include/strata/core/layer.hpp        (the composed order + the layout/format contract)
//   upstream/src/core/layer.cpp  gdn_layer()       (the compose: conv/SiLU, l2-norm, gates, recurrence)
//   upstream/src/kernels/cuda/native_gdn.cu  native_gdn_step()  L98-121 (the recurrence kernel)
//   upstream/src/kernels/cuda/gdn.cu, fused_gdn.cu, native_gdn_preprocess.cu (the preprocessing)
// Reference spec: upstream ref/gdn.py::gdn_decode (layer.hpp L8-33 transcribes its docstring verbatim).
//
// GDN is a gated-delta net: like GLM's KDA (engines/unified/src/kernels/glm_kda.*) but with a
// PER-HEAD scalar decay from a softplus gate, and the qwen4exp detail that the output gate is a
// SIGMOID (qwen3.5 used SiLU — layer.hpp L16). The order below is ref/gdn.py's docstring, verbatim:
//
//     qkv  = wqkv @ x                  (conv_channels = q|k|v, each a run of state-wide heads)
//     conv -> SiLU                     (SiLU on the WHOLE conv output, BEFORE the q|k|v split)
//     q,k  = l2_norm                   (per head; v is NOT normalised)
//     beta = sigmoid(wbeta @ x)        (per v-head scalar)
//     gate = softplus(walpha @ x + dt) * ssm_a     (per v-head scalar; ssm_a = -exp(A_log) < 0)
//     recurrent delta-rule step        (state S carried across tokens; q scaled by 1/sqrt(state))
//     y    = rms_norm(o, ssm_norm) * sigmoid(z)    (SIGMOID output gate for qwen4exp)
//     out  = ssm_out @ y
//
// The graph does the projection GEMVs (wqkv, wbeta, walpha, ssm_out) via the shared matmul
// (u::k::mmvq / the BF16 path). These kernels do the conv, the activations/gates, the recurrence and
// the output gate — the pieces that are not a plain GEMV.
//
// LAYOUT CONTRACTS (f32, ggml order — features fastest; no transposes between the shared matmul's
// outputs and these kernels). h_k = ssm_k_heads(16), h_v = ssm_v_heads(48), S = ssm_state(128):
//   qkv / conv out    : (conv_channels, T)                   element (c, t) at c + conv_channels*t
//   conv_w            : (d_conv, conv_channels)               (tap, c) at tap + d_conv*c
//   conv_state        : (d_conv-1, conv_channels)             history, OLDEST first; (j,c) at j+(d_conv-1)*c
//   q, k              : (S, h_k, T)                           (e,h) at e + S*h (q/k share h_k heads)
//   v, o              : (S, h_v, T)                           value heads (h_v of them)
//   alpha,beta,gate,z : (h_v, T)                              per v-head scalars
//   ssm_a             : (h_v,)                                -exp(A_log), negative
//   state             : (h_k? , S, h_v) packed as upstream GdnShapes: S*S*h_v f32, updated in place
//   out (to ssm_out)  : (value_dim=S*h_v, T)
//
// DECODE-FIRST: the recurrence carries the state across the T tokens inside one block per v-head
// (decode T=1; teacher-forced prefill T>1). The upstream prefill chunk path (gdn_chunk.cu) is a
// box extension; the serve path calls this per token.
#pragma once

#include <cstdint>

namespace u::qwen4exp {

// GDN geometry the kernels pin (mirrors Qwen4ExpGeometry / upstream GdnShapes).
struct GdnShapes final {
  int state = 128;        // S
  int k_heads = 16;       // h_k
  int v_heads = 48;       // h_v
  int d_conv = 4;
  int conv_channels = 10240;  // 2*S*h_k + S*h_v
  int value_dim = 6144;       // S*h_v
};

// silu(conv1d_causal(qkv)) over ALL conv_channels, with the (d_conv-1, conv_channels) history updated
// in place (oldest tap first). SiLU is applied to the whole conv output BEFORE the q|k|v reshape
// (layer.hpp L21-25 — applying it after the split is the same arithmetic on different data).
// Ported from upstream fused_gdn.cu conv path / gdn.cu conv1d. `out` is (conv_channels, T).
// write_state = false: the T outputs from the current history, which is left untouched (the speculative
// verify window; its commit replays the kept tokens with write_state = true, bitwise the same history).
void gdn_conv_silu(const float* qkv, const float* conv_w, float* conv_state,
                   const GdnShapes& s, int T, float* out, void* stream, bool write_state = true);

// Split the SiLU'd conv output into q,k,v and L2-normalize q and k PER HEAD (v is NOT normalized).
// `h` is the conv output (conv_channels, T); writes q,k (S,h_k,T) and v (S,h_v,T). Ported from
// upstream native_gdn_preprocess.cu (l2_norm over q/k heads) + ref/gdn.py gdn_l2_norm.
void gdn_split_l2norm(const float* h, const GdnShapes& s, int T, float eps,
                      float* q, float* k, float* v, void* stream);

// The per-v-head scalar gates from the walpha/wbeta GEMV outputs:
//   beta[hv,t] = sigmoid(wbeta_out[hv,t])
//   gate[hv,t] = softplus(walpha_out[hv,t] + dt_bias[hv]) * ssm_a[hv]   (ssm_a < 0)
// `beta_in`/`alpha_in` are (h_v,T) GEMV outputs; `dt_bias`,`ssm_a` are (h_v,). Ported from
// upstream native_gdn_preprocess.cu gate path (ref/gdn.py: softplus gate, sigmoid beta).
void gdn_gates(const float* alpha_in, const float* beta_in, const float* dt_bias, const float* ssm_a,
               const GdnShapes& s, int T, float* gate, float* beta, void* stream);

// The recurrent delta-rule step over T tokens, per v-head, `state` carried in place. The output `o`
// includes the 1/sqrt(state) scale on q (applied by this kernel — ref/gdn.py puts it here).
//   per token: S = diag(exp(gate)) @ S ; delta = beta*(v - S^T k) ; S += k delta^T ; o = (1/sqrt S) S^T q
// Faithful transcription of upstream native_gdn.cu step<<<>>> (native_gdn_step L98-121). `state` is
// S*S*h_v f32, zeroed by the caller for a fresh sequence, and private to this function (its layout is
// (h_v, S cols, S rows) for S == 128, the warp-per-column kernel; (h_v, S rows, S cols) otherwise).
// `q`,`k` are (S,h_k,T); `v`,`o` (S,h_v,T); `gate`,`beta` (h_v,T). GQA: v-head hv reads q/k head hv % h_k.
// write_state = false (S == 128 only): the T outputs from the current state, which is left untouched (the
// speculative verify window); the T-token loop keeps the state in registers, so its outputs are bitwise T
// single-token calls, and a commit replaying the kept tokens with write_state = true lands the same state.
void gdn_recurrence(float* state, const float* q, const float* k, const float* v,
                    const float* gate, const float* beta, const GdnShapes& s, int T,
                    float* o, void* stream, bool write_state = true);

// The output gate: y[i] = rms_norm(o, ssm_norm) * sigmoid(z), per v-head. `o` is (S,h_v,T), `z`
// (value_dim,T) is the GEMV gate pre-activation, `ssm_norm` (S,). qwen4exp uses SIGMOID here
// (layer.hpp L16; qwen3.5 used SiLU). Ported from upstream native_gr_norm.cu / fused_gdn.cu out path.
void gdn_out_gate(const float* o, const float* ssm_norm, const float* z, const GdnShapes& s, int T,
                  float eps, float* y, void* stream);

}  // namespace u::qwen4exp
