// strata-unified-engine: Qwen3.6-35B-A3B (arch "qwen35moe") periodic FULL ATTENTION — the 10 of
// 40 trunk layers with l % full_attention_interval == full_attention_interval - 1 (interval 4, so
// layers 3, 7, 11, ... 39). These layers carry a real KV cache; the 30 GDN layers do not.
//
// SHAPE (q36ref.cu top comment; Qwen35MoeGeometry::from_gguf re-reads it on the box):
//   H  = n_head         = 16 query heads
//   HKV= n_head_kv       = 2 key/value heads (GQA: 8 query heads per kv head)
//   HD = head_dim        = 256 (attention.key_length == value_length)
//   per query head the attn_q projection is [q (HD) | gate (HD)] interleaved, 2*HD per head;
//   q and k are RMS-normed per head with attn_q_norm/attn_k_norm [HD];
//   NEOX RoPE over the first n_rot = 64 dims only, rope.freq_base = 1e7;
//   score scale = 1/sqrt(HD) = 1/16; the output is gated by sigmoid(gate) per element.
//
// PORTED FROM (faithful): upstream/src/qwen35/q36_engine.cu k_attn_prep (q36_engine.cu:256) for
// the per-head RMS + NEOX RoPE + KV-cache write + output-gate keep, and the single-pass softmax
// attention of the clean fp32 reference q36ref.cu k_attn (q36ref.cu:294), extended over a T-token
// chunk (one block per (query head, token)). The production splits the long-prompt attention over
// key chunks (k_attn_part q36_engine.cu:301 + k_attn_merge q36_engine.cu:341, online-softmax
// merge) and runs an FP16 cuBLAS prompt path; both are left for the GPU box (see the .cu note) —
// this lean serve port keeps the one-pass form, correct for decode and serve-sized prompt chunks.
//
// Text-only (design goal: this family is text-only): the M-RoPE section table (mtab) and the
// picture rotary offset (rdelta) of the production k_attn_prep are dropped — every position uses
// one rotary index (q36ref.cu:280 note: "the M-RoPE sections all carry the same position").
//
// HOST-ONLY LAUNCHER ABI (void* stream); built only behind STRATA_U_CUDA on the owner's box.
#pragma once

#include <cstdint>

namespace u::qwen35moe {

// (1) PER-HEAD PREP (q36_engine.cu:256 k_attn_prep, text-only). For a T-token chunk whose token 0
// is at sequence position `pos0`:
//   `qg` is attn_q·h  [T, H, 2*HD] (q then gate per head); `kx`/`vx` are attn_k·h / attn_v·h
//   [T, HKV, HD]. `qw`/`kw` are attn_q_norm/attn_k_norm [HD].
//   query head: q[t,h] = RoPE(rms(qg_q[t,h]) * qw), and gate[t,h] = qg_gate[t,h] (kept raw, the
//       sigmoid is applied in the attention output); both [T, H, HD].
//   kv head:    kc[kh, pos0+t] = RoPE(rms(kx[t,kh]) * kw); vc[kh, pos0+t] = vx[t,kh]; the caches
//       are [HKV, ctx, HD].
// Launch grid (H + HKV, T), block HD (== 256). base = rope.freq_base, eps = layer_norm_rms_epsilon.
void attn_prep(const float* qg, const float* kx, const float* vx, const float* qw, const float* kw,
               float* q, float* gate, float* kc, float* vc, int H, int HKV, int HD, int n_rot,
               int pos0, int ctx, float base, float eps, int T, void* stream);

// (2) GQA SOFTMAX ATTENTION + sigmoid output gate (q36ref.cu:294 k_attn, per-token, over a T-token
// chunk). For query head h of token t (kv head = h / (H/HKV)), attending causally over cells
// [0, pos0+t]:
//   out[t,h,:] = ( softmax_p( scale * q[t,h] . kc[kvh,p] ) @ vc[kvh,p] ) * sigmoid(gate[t,h,:]).
// `out` is [T, H, HD]. Launch grid (H, T), block HD (== 256), dynamic shared = (pos0+T)*sizeof
// (float) for the per-row score row. NOTE (box): the dynamic score row is bounded by the context;
// past ~12K cells it exceeds the 48 KB shared limit — switch to the split-K path (k_attn_part +
// k_attn_merge, q36_engine.cu:301/:341) there. scale = 1/sqrt(HD).
void attn(const float* q, const float* kc, const float* vc, const float* gate, float* out, int H,
          int HKV, int HD, int pos0, int ctx, float scale, int T, void* stream);

}  // namespace u::qwen35moe
