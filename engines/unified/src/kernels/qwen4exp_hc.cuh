// engines/unified/src/kernels/qwen4exp_hc.cuh - Qwen3.8-Flash-Next hyper-connections (count 4,
// low_rank 320), the gated-residual wrapping EVERY layer.
//
// CORRECTED against the upstream reference (llama.cpp qwen4exp.cpp build_hc_mix / build_hc_combine):
// the combine/collapse is NOT a length-hc scalar mix -- it is a PER-ELEMENT low-rank gate followed by a
// plain mean over the hc streams. Only the INJECT weight is a per-stream hc-scalar. Per sub-block, over
// hc=4 residual streams of n_embd=2560 (flat hc_dim = hc*n_embd = 10240):
//   gr_read (combine):  xn   = rms_norm_per_stream(R) * hc_norm          (n_embd, hc)
//                       lo   = hc_down @ xn ; lo = silu(lo * 1/hc)       (hc_lr=320)   [graph GEMV + silu]
//                       gate = hc_up @ lo                                 (hc_dim)      [graph GEMV]
//                       block_in = mean_c( xn[:,c] * sigmoid(gate[:,c]) ) (n_embd)
//                       inject_raw = hc_inject @ xn                       (hc)          [graph GEMV]
//   gr_write (inject):  R[:,c] += block_out * (2*sigmoid(inject_raw[c]/hc))
//   final collapse:     same as gr_read with output_hc_norm/down/up, no inject; block_in IS result_norm.
// The GEMVs (hc_down/up/inject) are the graph's (u::k mmvq/bf16); these kernels do the per-stream RMS +
// gamma, the gated mean, and the inject. Gammas are applied as stored (converter-folded).
#pragma once

#include <cstdint>

namespace u::qwen4exp {

struct HcShapes final {
  int n_embd = 2560;
  int hc = 4;
  int hc_lr = 320;
};

// Each takes T tokens (the speculative verify window; T = 1 is one token): token t's operands at its own
// stride -- streams/xn/gate t*hc*n_embd, block_in/block_out/moe/shd/out t*n_embd, inject_raw t*hc,
// gate_logit t -- and every token runs the single-token arithmetic (blockIdx.y = token).

// xn[:,c] = rms_norm(streams[:,c]) * gamma[:,c], per stream over n_embd. `streams`/`gamma`/`xn` are
// (n_embd, hc) flat (stream c at c*n_embd). `gamma` is hc_*_norm.weight [n_embd, hc].
void hc_rms_gamma(const float* streams, const float* gamma, const HcShapes& s, float eps, float* xn,
                  void* stream, int T = 1);

// block_in[e] = (1/hc) * sum_c xn[c*n_embd+e] * sigmoid(gate[c*n_embd+e]). `xn`,`gate` are (n_embd,hc).
void hc_gated_mean(const float* xn, const float* gate, const HcShapes& s, float* block_in, void* stream,
                   int T = 1);

// streams[c*n_embd+e] += block_out[e] * (2 * sigmoid(inject_raw[c] / hc)). `inject_raw` is (hc).
void hc_inject(float* streams, const float* block_out, const float* inject_raw, const HcShapes& s,
               void* stream, int T = 1);

// out[e] = moe[e] + sigmoid(gate_logit[0]) * shd[e]  -- the shared-expert combine with a DEVICE-side
// sigmoid gate (gate_logit is a 1-float device buffer), so the MoE decode never stalls on a per-layer
// D2H of the scalar logit. n = n_embd.
void shexp_combine(const float* moe, const float* shd, const float* gate_logit, int n, float* out,
                   void* stream, int T = 1);

}  // namespace u::qwen4exp
