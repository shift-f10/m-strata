// engines/unified/src/kernels/qwen4exp_ple.cuh - Qwen3.8-Flash-Next PLE (Per-Layer Embedding).
//
// PLE is applied at the ONE layer named by `qwen4exp.ple.layers` (= [1] for this model), BEFORE that
// layer's attention block. For each token it gathers ple_n_heads (16) rows of a ~28.8 GB IQ4_NL table
// (`per_layer_token_embd.weight`, host-mapped, never resident), indexed by an n-gram hash of the recent
// token ids (host-side: see Qwen4ExpGraph). Those rows -> emb[head_dim*n_heads = n_embd] drive a small
// gated per-layer embedding that is added back into the wide residual.
//
// Reference: out/.../src/models/qwen4exp.cpp build_ple / build_inp_ple (verified op-by-op). The grouped
// RMS norms are the HC read's hc_rms_gamma (reused). This header is the PLE-specific arithmetic:
//   key   = ple_key  @ emb                              [hc_dim]          (graph gemv_)
//   value = ple_value@ emb                              [n_embd]          (graph gemv_)
//   kn    = hc_rms_gamma(key,   ple_norm_key)           [hc_dim]
//   qn    = hc_rms_gamma(hidden,ple_norm_query)         [hc_dim]   (hidden = the wide residual R)
//   s[c]  = sum_e kn[c,e]*qn[c,e] / sqrt(n_embd);  gate[c] = sigmoid( sgn(s)*sqrt(clamp(|s|,1e-6)) )   (ple_gate)
//   gated[c,e] = value[e] * gate[c]                     [hc_dim]          (ple_gated_value)
//   ng    = hc_rms_gamma(gated, ple_norm_conv)          [hc_dim]
//   conv[c,e] = silu( sum_k w[k,e] * ng_hist[(kern-1-k)*dil back][c,e] )  dilated causal depthwise conv  (ple_conv_silu)
//   R[c,e] += gated[c,e] + conv[c,e]                                      (ple_add2)
#pragma once

#include <cstdint>

namespace u::qwen4exp {

// gate[c] = sigmoid( sgn(s)*sqrt(clamp(|s|,1e-6)) ), s[c] = (sum_e kn[c,e]*qn[c,e]) / sqrt(n_embd).
// One block per stream c (hc blocks); reduces over n_embd.
void ple_gate(const float* key_norm, const float* query_norm, int n_embd, int hc, float* gate, void* stream);

// gated[c*n_embd + e] = value[e] * gate[c]  (value broadcast across the hc streams).
void ple_gated_value(const float* value, const float* gate, int n_embd, int hc, float* gated, void* stream);

// Dilated causal depthwise conv over the per-channel history + SiLU, history advanced in place.
// conv_out[c,e] = silu( sum_{k=0..kern-1} w[k, c*n_embd+e] * tap_k ), tap_k = the value (kern-1-k)*dil
// tokens back (tap kern-1 = the current `normed`). `hist` holds the last (kern-1)*dil normed vectors
// (hc_dim each), oldest-first; it is shifted by one (oldest out, `normed` in) per call. conv_w is the
// GGUF ple_conv1d.weight [d_conv, hc_dim] (ne0=d_conv contiguous: tap k of channel ch at ch*d_conv + k).
void ple_conv_silu(const float* normed, const void* conv_w_f16, float* hist, int n_embd, int hc, int kern,
                   int dil, float* conv_out, void* stream);

// hidden[i] += a[i] + b[i], i in [0, n).
void ple_add2(float* hidden, const float* a, const float* b, int n, void* stream);

}  // namespace u::qwen4exp
