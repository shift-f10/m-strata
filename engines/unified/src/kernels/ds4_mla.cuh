// engines/unified/src/kernels/ds4_mla.cuh
// -----------------------------------------------------------------------------
// DeepSeek-V4.1-Flash (ds4) family-specific device kernels: Multi-head Latent
// Attention (MLA) with the compressed / latent KV window, the hyper-connection
// residual streams, and the small fused primitives the MLA graph needs.
//
// PORT ORIGIN (reviewed, not a live dependency; design 2026-10-23 §1.2, §3.2):
//   engines/maya-ref/src/ds4/ds4_kernels.cuh   (the contracts, verbatim intent)
//   engines/maya-ref/src/ds4/ds4_kernels.cu    (the kernel bodies to port)
//   engines/maya-ref/src/ds4/ds4_model.cu      (how the graph calls them)
//   engines/maya-ref/src/ds4/ds4_numerics.cuh  (BF16 / FP8 / FP4 conversions)
//
// SCOPE (lean serve path, design §1.2): single-GPU window path only. Tensor
// parallelism, EXL3 exotic layouts, vision and the multi-GPU hand-offs of the
// reference are intentionally dropped here; the DS4 cache numerics and the MLA
// math are kept faithful. The GENERIC kernels (quantized GEMV / MMVQ, RMSNorm,
// RoPE, softmax, the sigmoid router+top-k, the sampler) are NOT duplicated here:
// the graph calls the shared set in kernels/common.cuh (u::k::...) and the
// shared expert tiering in u_experts.hpp (u::ExpertTier). Only the ops that are
// DeepSeek-specific live in these ds4_* files.
//
// Every pointer below is device memory, every activation is F32 with a window's
// rows (1..kWin) back to back, unless the doc says otherwise. The KV caches keep
// DeepSeek's own FP8 / FP4 roundings packed exactly as the reference stores them
// (ds4_kernels.cuh:23-36). The math is DeepSeek's inference/model.py (MIT); the
// box checks it against tools/ds4/ref.py (ds4_kernels.cuh:1-7).
// -----------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(STRATA_U_CUDA)
#include <cuda_runtime.h>
#else
// Host-only translation units (CI / dev host) never compile the kernels, but the
// graph header still mentions cudaStream_t in signatures it does not call. Alias
// it to void* so the declarations parse without the CUDA toolkit.
using cudaStream_t = void*;
#endif

namespace u::ds4 {

// A window takes at most kWin tokens; a routed token takes at most kPicks
// experts (ds4_kernels.cuh:20-21). These bound every per-window scratch size.
constexpr int kWin = 16;
constexpr int kPicks = 8;

// -----------------------------------------------------------------------------
// KV cache numerics (ds4_kernels.cuh:23-36). A row of `d` values is packed into
// kv_row_bytes(fmt, d) bytes, padded to 16 (the window hand-offs copy 16-byte
// units):
//   kKvFp8   window ring   : d E4M3 codes, then d/32 power-of-two scales (UE8M0)
//   kKvFp4   compressed KV : d/2 bytes E2M1 codes, then d/16 E4M3 scales
//   kKvFp4p  index keys    : d/2 bytes E2M1 codes, then d/32 UE8M0 scales
//   kKvBf16  plain BF16 rows (a DSpark block's own rows; the parity reference)
constexpr int kKvBf16 = 0, kKvFp8 = 1, kKvFp4 = 2, kKvFp4p = 3;

constexpr std::size_t kv_row_bytes(int fmt, int d) {
    return fmt == kKvBf16  ? (std::size_t) d * 2
           : fmt == kKvFp8 ? ((std::size_t) d + (std::size_t) d / 32 + 15) / 16 * 16
           : fmt == kKvFp4 ? ((std::size_t) d / 2 + (std::size_t) d / 16 + 15) / 16 * 16
                           : ((std::size_t) d / 2 + (std::size_t) d / 32 + 15) / 16 * 16;
}
// A packed FP4 row (Finish::pdst): len/2 code bytes, one scale byte a group
// (quant 2: 16 values, quant 3: 32). (ds4_kernels.cuh:62-63)
constexpr std::size_t fp4_row_bytes(int len, int quant) {
    return (std::size_t) len / 2 + (std::size_t) len / (quant == 2 ? 16 : 32);
}

// -----------------------------------------------------------------------------
// Embedding fan-out and the per-vector fused "finish".

// h[t][s][e] = BF16 rows[t][e] for each of the `hc` hyper-connection streams.
// (ds4_kernels.cuh:38-39, ds4_kernels.cu:149) PORTED faithfully in ds4_mla.cu.
void embed(const uint16_t* rows_bf16, int T, float* h, int dim, int hc, cudaStream_t s);

// rows[t][0] = 1, the rest 0: the first layer's pre-mix (stream 0).
// (ds4_kernels.cuh:288, ds4_kernels.cu:2367) PORTED faithfully.
void init_pre(float* pre, int T, cudaStream_t s);

// h = silu(min(g, lim)) * clamp(u, -lim, lim)   (lim <= 0: no clamp); n values.
// (ds4_kernels.cuh:106-107, ds4_kernels.cu:573) PORTED faithfully.
void swiglu(const float* g, const float* u, int n, float lim, float* h, cudaStream_t s);

// The per-vector finish (ds4_kernels.cuh:41-64): n_vec vectors of `len` values at
// x + v*stride, each applying, in order,
//   norm  (w != null): x = x * rsqrt(mean(x^2) + eps) * w       (== u::k::rmsnorm)
//   rope  (rd > 0)    : the last rd values rotated by pos_v*freqs[i]
//                       (inverse: the conjugate)                (== u::k::rope)
//   quant 0/1/2/3     : none / FP8-E4M3 / FP4-E2M1(scale/16) / FP4(scale/32)
// and written back to x, or as BF16 to `dst` (dst_f16), or PACKED to `pdst` in the
// cache format for `quant` (kv_row_bytes). This is the DeepSeek-specific fusion of
// RMSNorm + RoPE + cache-quant; it deliberately composes the SAME arithmetic as the
// generic u::k::rmsnorm / u::k::rope but writes DS4's packed KV grids in one pass,
// so it stays a ds4_* kernel rather than a common.cuh one.
// PORT: ds4_kernels.cu:2780 (finish) + finish_kernel<kPack> (template around
//       ds4_kernels.cu:160). Faithful skeleton in ds4_mla.cu; the packed E4M3/E2M1
//       rounding is delegated to ds4_numerics-equivalent helpers (TODO there).
struct Finish {
    const float* w = nullptr;
    float eps = 1e-20f;
    int rd = 0, pos = 0, inverse = 0, vpp = 1, pos_step = 1;
    const float* freqs = nullptr;   // rd/2 values
    int quant = 0;
    uint16_t* dst = nullptr;
    long long dst_stride = 0;
    int dst_f16 = 0;
    uint8_t* pdst = nullptr;
    long long pdst_stride = 0;
};
void finish(float* x, int n_vec, int len, long long stride, const Finish& f, cudaStream_t s);

// -----------------------------------------------------------------------------
// Hyper-connections (ds4_kernels.cuh:66-83). DeepSeek-V4.1 carries `hc` residual
// streams; each attention / MoE block mixes and re-combines them.
//   hc_mix       : mixes[t][m] = (h_flat . fn[m]) * rsqrt(mean(h_flat^2)+eps)
//   hc_split     : pre / post / comb via the sinkhorn split (comb[t][j*hc+k])
//   hc_pre_norm  : x[t] = rmsnorm(sum_s mix[t][s] h[t][s]) * w   (w null: no norm)
//   hc_post      : out[t][k] = post[t][k] y[t] + sum_j comb[t][j][k] res[t][j]
// Strides: mixes 32, pre/post 8, comb 64 floats a row.
// PORT: ds4_kernels.cu:2802-2824 (launchers) + their kernels. hc_mix/hc_pre_norm/
//       hc_post are faithful in ds4_mla.cu; the sinkhorn hc_split is a structured
//       stub (TODO: ds4_kernels.cu hc_split_kernel).
void hc_mix(const float* h, const float* fn, int hcdim, int n_mix, int T, float eps, float* mixes, cudaStream_t s);
void hc_split(const float* mixes, const float* scale, const float* base, int hc, int iters, float hc_eps, int T,
              float* pre, float* post, float* comb, cudaStream_t s);
void hc_pre_norm(const float* h, const float* mix, const float* w, int dim, int hc, int T, float eps, float* x,
                 cudaStream_t s);
void hc_post(const float* y, const float* res, const float* post, const float* comb, int dim, int hc, int T,
             float* out, cudaStream_t s);
// out[t][e] = mean over the hc streams of h[t][s][e] (the DSpark drafter's taps),
// rows out_stride floats apart. (ds4_kernels.cuh:290-291)
void hc_mean(const float* h, int T, int hc, int dim, float* out, long long out_stride, cudaStream_t s);

// -----------------------------------------------------------------------------
// The sliding-window KV ring and the ratio compressor (the "latent KV").

// The ratio-r compressor over a window (ds4_kernels.cuh:109-113): token t
// (position p0+t) fills slot (p0+t)%r of the state; the token that completes a
// group writes the pooled latent (softmax over the slots per value, then
// rmsnorm(w)) to the next row of `latent`.
// PORT: ds4_kernels.cu:2866 (launcher) + compress_kernel (ds4_kernels.cu:~588).
//       Structured stub in ds4_mla.cu (TODO cites compress_kernel).
void compress_win(const float* kv, const float* score, float* kv_state, float* score_state, int ratio, int p0, int T,
                  int d, const float* w, float eps, float* latent, cudaStream_t s);

// The window buffer <-> ring (ds4_kernels.cuh:186-194):
//   ring_to_ext : ext[r] = ring[(p0-n+r)%n] for the n rows before p0 (zeros
//                 before position 0); the ring is kKvFp8, the window buffer BF16.
//   ext_to_ring : ring[(p0+t)%n] = ext[n+t] for t<T (optional `backup` of the
//                 rows it replaces, for a speculative window's rollback).
//   ring_restore: ring[(p0+t)%n] = backup[t] for keep<=t<T.
// PORT: ds4_kernels.cu ring_to_ext/ext_to_ring/ring_restore. ring_to_ext and
//       ext_to_ring are faithful in ds4_mla.cu; ring_restore is a thin reuse.
void ring_to_ext(const uint8_t* ring, uint16_t* ext, int64_t p0, int n, int d, cudaStream_t s);
void ext_to_ring(const uint16_t* ext, uint8_t* ring, int64_t p0, int n, int T, int d, cudaStream_t s,
                 uint8_t* backup = nullptr);
void ring_restore(uint8_t* ring, const uint8_t* backup, int64_t p0, int n, int keep, int T, int d, cudaStream_t s);

// -----------------------------------------------------------------------------
// Sparse / windowed attention (ds4_kernels.cuh:159-185). Query t attends to its
// window rows win0[t]..win0[t]+nwin[t]-1 of `win` (BF16) and the nsel[t] selected
// compressed rows ckv[sel[t][i]] (format ckv_fmt). K == V, a learned sink logit
// per head. This is the numerically-critical MLA kernel; the box owns the tuned
// tensor-core / staged variants.
struct AttnRows {
    int win0[kWin], nwin[kWin], nsel[kWin];
    int gen = 0, t0 = 0, win = 0, R = 0, kcap = 0;
    long long p0 = 0, lo = 0;
};
struct AttnScratch {
    uint16_t* rows = nullptr;   // [max_t*cap][d] BF16 staging
    int max_t = 0, cap = 0;
};
// PORT: ds4_kernels.cu sparse_attn + its kernels (BF16-staged and per-head). This
//       is a STRUCTURED STUB in ds4_mla.cu (writes zeros) with a precise TODO: the
//       reference has FP32, tensor-core (sm_80+) and WMMA (RDNA) paths the box must
//       bring over and parity-check (ds4_kernels.cuh:160-185, ds4_kernels.cu).
void sparse_attn(const float* q, const uint16_t* win, const void* ckv, const int* sel, int sel_stride,
                 const AttnRows& a, const float* sink, int heads, int d, int T, float scale, float* out, cudaStream_t s,
                 int ckv_fmt = kKvFp4, int heads_per_block = 0, const AttnScratch* scr = nullptr);

// NOTE: the routed-expert GEMMs AND their weighted combine (shared + routed) are the
// shared facilities — u::ExpertTier runs the experts, u::k::add_inplace / moe_combine
// sums them — so the reference's gather_rows / combine MoE plumbing is not ported
// here (ds4_kernels.cuh:229-245); the ds4 kernels stay the MLA / index / DSpark ops.

// -----------------------------------------------------------------------------
// Engram (DeepSeek's n-gram associative memory, ds4_kernels.cuh:217-227). Only the
// engram_layer_ids layers gate on it. It is an auxiliary memory path; represented
// here as a structured stub (TODO) so the layer schedule stays faithful while the
// box ports the dequant + gate kernels and the host-side n-gram reads.
void engram_dequant(const uint8_t* rows, const uint8_t* scales, int n, int hd, float* out, cudaStream_t s,
                    bool q6k = false);
void engram_gate(float* h, const float* kv, const float* w, int hc, int dim, int T, float eps, cudaStream_t s);

}  // namespace u::ds4
