// strata-unified-engine: Qwen3.6-35B-A3B (arch "qwen35moe") GDN — the Gated-DeltaNet LINEAR
// attention used on 30 of the 40 trunk layers (every layer with l % full_attention_interval !=
// full_attention_interval - 1; qwen35moe pins full_attention_interval = 4, so l % 4 != 3).
// These layers carry NO KV cache — the whole history lives in the recurrent state S and a short
// causal-conv history.
//
// PORTED FROM (faithful, math unchanged):
//   upstream/src/qwen35/q36_engine.cu   k_conv_seq   (q36_engine.cu:605) — the T-token causal
//        4-tap conv + SiLU + per-head L2-norm on the q/k channels;
//   upstream/src/qwen35/q36_engine.cu   k_gdn_gb     (q36_engine.cu:225) — the gate/beta
//        epilogue (softplus(alpha+dt)*ssm_a, sigmoid(beta)) applied in place before the step;
//   upstream/src/qwen35/q36_engine.cu   k_gdn_seq    (q36_engine.cu:672) — the gated delta-rule
//        recurrence over T tokens with the register-resident DxD state, fused with the
//        SiLU-gated RMS output norm (k_gdn_norm, q36_engine.cu:813, is folded in here).
// The clean fp32 reference for the same math (THE SPEC) is q36ref.cu: k_conv (q36ref.cu:200),
// k_gdn (q36ref.cu:212), k_gated_norm (q36ref.cu:250). The reference applies the q/k L2-norm and
// the 1/sqrt(D) q-scale inside k_gdn; the production (and this port) fuses the q/k L2-norm into
// the conv and applies the 1/sqrt(D) on the output instead — bitwise-equivalent, the ordering the
// production verifies with STRATA_Q36_VERIFY.
//
// SHAPES (from strata::core::geometry_from_gguf / q36ref.cu top comment; the box re-reads them
// from the GGUF via Qwen35MoeGeometry::from_gguf):
//   C   = ssm_conv_channels = 2*HK*D + HV*D   (q | k | v laid out contiguously), = 8192
//   HK  = ssm_k_heads  (q heads and k heads)                                      = 16
//   HV  = ssm_v_heads  (v heads; also the gate z heads)                           = 32
//   D   = ssm_state_size (head width, and the DxD state side)                     = 128
//   qk_heads passed to the conv = 2*HK (the head-blocks that get the L2-norm)     = 32
//   the gate z width ZV = HV*D = ssm_value_dim                                    = 4096
//   d_conv = ssm_conv_kernel = 4 (the conv history is d_conv-1 = 3 columns/channel)
//
// HOST-ONLY LAUNCHER ABI (the unified convention, kernels/common.cuh): `stream` is a
// cudaStream_t passed as void*, so this header stays includable from a host translation unit.
// Built only behind STRATA_U_CUDA on the owner's NVIDIA box.
#pragma once

#include <cstdint>

namespace u::qwen35moe {

// (1) GATE/BETA EPILOGUE, in place over T rows of HV heads (q36_engine.cu:225 k_gdn_gb):
//   alpha[t,h] <- softplus(alpha[t,h] + dt[h]) * ssm_a[h]      (the LOG-space decay g)
//   beta [t,h] <- sigmoid(beta[t,h])
// `alpha`/`beta` are the W_alpha·h / W_beta·h projections (shared mmvq, [T, HV]); `dt` is
// ssm_dt.bias [HV], `ssm_a` is ssm_a [HV]. The recurrence below reads exp(alpha) as the decay.
void gdn_gate_beta(float* alpha, const float* dt, const float* ssm_a, float* beta, int HV, int T,
                   void* stream);

// (2) CAUSAL CONV + SiLU + per-head L2-norm (q36_engine.cu:605 k_conv_seq, simple single-session
// path). `qkv` is [T, C] (the W_qkv·h projection). For each channel c and token t:
//   sum = conv_w[c,0]*x_{t-3} + conv_w[c,1]*x_{t-2} + conv_w[c,2]*x_{t-1} + conv_w[c,3]*x_t
//   y   = silu(sum);  and for the q/k head channels (head block < qk_heads) y is L2-normalized
//         over its head's D channels: y *= rsqrt(sum_D(y^2) + eps).
// The v channels are left as silu(sum). `hist` holds the last d_conv-1 columns per channel
// ([C][d_conv-1], here [C][3]); when `write` it is advanced by T (decode and prompt chunks). `out`
// is [T, C], the input to the recurrence (q | k | v, already normalized where it matters).
// Requires C % D == 0 and D == 128 (one 128-thread block per head of 128 channels).
void gdn_conv(float* hist, const float* qkv, const float* conv_w, float* out, int C, int qk_heads,
              float eps, int T, bool write, void* stream);

// (3) GATED DELTA-RULE RECURRENCE + fused SiLU-gated RMS output norm (q36_engine.cu:672 k_gdn_seq
// with q36_engine.cu:813 k_gdn_norm folded in; the fp32 spec is q36ref.cu:212 + :250). `h` is the
// conv output [T, C] (q = h[:, 0:HK*D], k = h[:, HK*D:2*HK*D], v = h[:, 2*HK*D:C]); `gate`/`beta`
// are [T, HV] after gdn_gate_beta (gate is log-space); `z` is the W_gate·h gate projection
// [T, ZV] (ZV = HV*D); `gamma` is ssm_norm.weight [D]. State `S` is [HV][D][D] (row-major
// [v-head][state-row][state-col]), carried across calls and reset per sequence. For each token t,
// per v-head head (its k-head qh = head % HK):
//   g = exp(gate[t,head]);  S[head] *= g;  u = S[head] @ k;  delta = (v - u) * beta[t,head]
//   S[head] += outer(delta, k)  (delta the state-row axis, k the state-col axis — see note);
//   o[col] = (S[head] @ q)[col] / sqrt(D);
//   y[t, head*D + col] = rms_D(o) * gamma[col] * silu(z[t, head*D + col]).
// `write` stores the advanced state (false on a verify window; serve always writes). `y` may be
// null to only advance the state (the commit path). Launch grid (HV, 1), block (128, 4).
void gdn_recurrence(float* S, const float* h, int C, const float* gate, const float* beta,
                    const float* z, const float* gamma, float eps, float* y, int HK, int HV, int T,
                    bool write, void* stream);

}  // namespace u::qwen35moe
