// engines/unified/src/kernels/ds4_mla.cu
// -----------------------------------------------------------------------------
// Bodies for the ds4 MLA / latent-KV / hyper-connection kernels declared in
// ds4_mla.cuh. Ported from engines/maya-ref/src/ds4/ds4_kernels.cu (MIT; math is
// DeepSeek inference/model.py). The tractable element-wise kernels are ported
// faithfully; the numerically-heavy ones (finish's packed quant, the sinkhorn
// hc_split, the ratio compressor, sparse_attn) are STRUCTURED STUBS with precise
// TODOs for the GPU box, which fills and parity-checks them (plan 2026-10-23 T1.4).
//
// This file is compiled ONLY on the GPU box (STRATA_U_CUDA ON, CUDA language). On
// the host / CI build it is an empty translation unit so the tree stays CI-green
// with no CUDA toolkit (design §1.3, plan "the hard boundary").
// -----------------------------------------------------------------------------
#include "kernels/ds4_mla.cuh"

#if defined(STRATA_U_CUDA)

#include <cstdio>

#include <cuda_runtime.h>
#if !defined(STRATA_USE_HIP)
#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <cuda_fp8.h>
#endif

namespace u::ds4 {
namespace {

constexpr int kThreads = 256;
__host__ __device__ constexpr int cdiv(int a, int b) { return (a + b - 1) / b; }

// BF16 bit-pattern -> float (round-trip exact). Mirrors ds4_numerics.cuh's helpers;
// the box may replace with the shared bf16 header if common.cuh exposes one.
__device__ __forceinline__ float bf16f(uint16_t b) {
    const uint32_t u = (uint32_t) b << 16;
    return __uint_as_float(u);
}
[[maybe_unused]] __device__ __forceinline__ uint16_t f2bf16(float f) {
#if !defined(STRATA_USE_HIP)
    return __nv_bfloat16_raw(__float2bfloat16_rn(f)).x;
#else
    const uint32_t u = __float_as_uint(f);
    if ((u & 0x7fffffffu) > 0x7f800000u) return (uint16_t) ((u >> 16) | 0x40u);
    return (uint16_t) ((u + 0x7fffu + ((u >> 16) & 1u)) >> 16);
#endif
}

// ---------------------------------------------------------------- embed / init_pre
// PORT: ds4_kernels.cu:149 embed_kernel, :2367 init_pre_kernel (faithful).
__global__ void embed_kernel(const uint16_t* __restrict__ rows, float* __restrict__ h, int dim, int hc) {
    const int e = blockIdx.x * blockDim.x + threadIdx.x, t = blockIdx.y;
    if (e >= dim) return;
    const float v = bf16f(rows[(size_t) t * dim + e]);
    for (int s = 0; s < hc; ++s) h[((size_t) t * hc + s) * dim + e] = v;
}
__global__ void init_pre_kernel(float* pre, int T) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < T * 8) pre[i] = (i % 8) == 0 ? 1.0f : 0.0f;
}

// ---------------------------------------------------------------- swiglu (faithful)
// PORT: ds4_kernels.cu:573 swiglu_kernel.
__global__ void swiglu_kernel(const float* __restrict__ g, const float* __restrict__ u, int n, float lim,
                              float* __restrict__ h) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float gv = g[i], uv = u[i];
    if (lim > 0.0f) { gv = fminf(gv, lim); uv = fminf(fmaxf(uv, -lim), lim); }
    h[i] = gv / (1.0f + expf(-gv)) * uv;
}

// ---------------------------------------------------------------- finish
// RMSNorm (+optional RoPE) over one vector, written back to x. The packed-cache
// write path (f.pdst / f.dst) is a STRUCTURED STUB: it computes the normed/rotated
// values but does NOT yet pack E4M3 / E2M1 codes+scales.
// TODO(box): port the packed kPack path of finish_kernel<true> from
//   engines/maya-ref/src/ds4/ds4_kernels.cu (finish_kernel template, ~ln 160) and
//   its act_quant / fp4_act_quant E4M3/E2M1 roundings (ds4_numerics.cuh:42-61); keep
//   the exact scale-per-32/16 grids (ds4_kernels.cuh:45-48) for KV parity.
__global__ void finish_norm_rope_kernel(float* __restrict__ x, int len, long long stride, Finish f) {
    const int v = blockIdx.x;
    float* row = x + (size_t) v * stride;
    // RMSNorm across the vector (block reduction).
    __shared__ float ss;
    float local = 0.0f;
    for (int i = threadIdx.x; i < len; i += blockDim.x) local += row[i] * row[i];
    for (int o = warpSize / 2; o > 0; o >>= 1) local += __shfl_down_sync(0xffffffffu, local, o);
    __shared__ float warp[32];
    if ((threadIdx.x & (warpSize - 1)) == 0) warp[threadIdx.x / warpSize] = local;
    __syncthreads();
    if (threadIdx.x == 0) {
        float s = 0.0f;
        for (int i = 0; i < cdiv((int) blockDim.x, warpSize); ++i) s += warp[i];
        ss = rsqrtf(s / (float) len + f.eps);
    }
    __syncthreads();
    const float scale = ss;
    for (int i = threadIdx.x; i < len; i += blockDim.x) {
        float val = row[i] * scale;
        if (f.w != nullptr) val *= f.w[i];
        row[i] = val;
    }
    __syncthreads();
    // RoPE on the last rd values as adjacent complex pairs (== u::k::rope).
    if (f.rd > 0 && f.freqs != nullptr) {
        const int base = len - f.rd;
        const int pos_v = f.pos + (v / (f.vpp > 0 ? f.vpp : 1)) * f.pos_step;
        for (int i = threadIdx.x; i < f.rd / 2; i += blockDim.x) {
            const float ang = (float) pos_v * f.freqs[i];
            float c = cosf(ang), sdir = sinf(ang);
            if (f.inverse) sdir = -sdir;
            const int a = base + 2 * i, b = a + 1;
            const float re = row[a], im = row[b];
            row[a] = re * c - im * sdir;
            row[b] = re * sdir + im * c;
        }
    }
    // TODO(box): if f.pdst / f.dst set, pack the row to the KV cache format for
    // f.quant (kv_row_bytes) instead of leaving it in x. See the header note.
}

// ---------------------------------------------------------------- hyper-connections
// PORT: ds4_kernels.cu hc_mix_kernel / hc_pre_norm_kernel / hc_post_kernel.
__global__ void hc_mix_kernel(const float* __restrict__ h, const float* __restrict__ fn, int hcdim, float eps,
                              float* __restrict__ mixes) {
    const int m = blockIdx.x, t = blockIdx.y;
    const float* hr = h + (size_t) t * hcdim;
    float dot = 0.0f, sq = 0.0f;
    for (int i = threadIdx.x; i < hcdim; i += blockDim.x) { dot += hr[i] * fn[(size_t) m * hcdim + i]; sq += hr[i] * hr[i]; }
    for (int o = warpSize / 2; o > 0; o >>= 1) { dot += __shfl_down_sync(0xffffffffu, dot, o); sq += __shfl_down_sync(0xffffffffu, sq, o); }
    __shared__ float sdot[32], ssq[32];
    const int lane = threadIdx.x & (warpSize - 1), wid = threadIdx.x / warpSize;
    if (lane == 0) { sdot[wid] = dot; ssq[wid] = sq; }
    __syncthreads();
    if (threadIdx.x == 0) {
        float d = 0.0f, q = 0.0f;
        for (int i = 0; i < cdiv((int) blockDim.x, warpSize); ++i) { d += sdot[i]; q += ssq[i]; }
        mixes[(size_t) t * 32 + m] = d * rsqrtf(q / (float) hcdim + eps);
    }
}
__global__ void hc_pre_norm_kernel(const float* __restrict__ h, const float* __restrict__ mix,
                                   const float* __restrict__ w, int dim, int hc, float eps, float* __restrict__ x) {
    const int t = blockIdx.x;
    float* xr = x + (size_t) t * dim;
    const float* mr = mix + (size_t) t * 32;
    __shared__ float ss;
    float sq = 0.0f;
    for (int e = threadIdx.x; e < dim; e += blockDim.x) {
        float a = 0.0f;
        for (int s = 0; s < hc; ++s) a += mr[s] * h[((size_t) t * hc + s) * dim + e];
        xr[e] = a;
        sq += a * a;
    }
    for (int o = warpSize / 2; o > 0; o >>= 1) sq += __shfl_down_sync(0xffffffffu, sq, o);
    __shared__ float warp[32];
    if ((threadIdx.x & (warpSize - 1)) == 0) warp[threadIdx.x / warpSize] = sq;
    __syncthreads();
    if (threadIdx.x == 0) { float s = 0.0f; for (int i = 0; i < cdiv((int) blockDim.x, warpSize); ++i) s += warp[i]; ss = rsqrtf(s / (float) dim + eps); }
    __syncthreads();
    for (int e = threadIdx.x; e < dim; e += blockDim.x) xr[e] = xr[e] * ss * (w != nullptr ? w[e] : 1.0f);
}
__global__ void hc_post_kernel(const float* __restrict__ y, const float* __restrict__ res,
                               const float* __restrict__ post, const float* __restrict__ comb, int dim, int hc,
                               float* __restrict__ out) {
    const int e = blockIdx.x * blockDim.x + threadIdx.x, t = blockIdx.y;
    if (e >= dim * hc) return;
    const int k = e / dim, ee = e % dim;
    float a = post[(size_t) t * 8 + k] * y[(size_t) t * dim + ee];
    for (int j = 0; j < hc; ++j) a += comb[(size_t) t * 64 + j * hc + k] * res[((size_t) t * hc + j) * dim + ee];
    out[((size_t) t * hc + k) * dim + ee] = a;
}
__global__ void hc_mean_kernel(const float* __restrict__ h, int hc, int dim, float* __restrict__ out,
                               long long out_stride) {
    const int e = blockIdx.x * blockDim.x + threadIdx.x, t = blockIdx.y;
    if (e >= dim) return;
    float a = 0.0f;
    for (int s = 0; s < hc; ++s) a += h[((size_t) t * hc + s) * dim + e];
    out[(size_t) t * out_stride + e] = a / (float) hc;
}

// (MoE gather/combine is not here: u::ExpertTier runs the routed experts and
//  u::k::add_inplace / moe_combine sums shared + routed — see ds4_mla.cuh note.)

}  // namespace

// ============================================================ launchers

void embed(const uint16_t* rows, int T, float* h, int dim, int hc, cudaStream_t s) {
    embed_kernel<<<dim3(cdiv(dim, kThreads), T), kThreads, 0, s>>>(rows, h, dim, hc);
}
void init_pre(float* pre, int T, cudaStream_t s) { init_pre_kernel<<<cdiv(T * 8, 128), 128, 0, s>>>(pre, T); }
void swiglu(const float* g, const float* u, int n, float lim, float* h, cudaStream_t s) {
    if (n > 0) swiglu_kernel<<<cdiv(n, kThreads), kThreads, 0, s>>>(g, u, n, lim, h);
}

void finish(float* x, int n_vec, int len, long long stride, const Finish& f, cudaStream_t s) {
    if (n_vec <= 0) return;
    // TODO(box): the reference caps len <= kFinMax*kThreads and has a kPack path
    //   for f.pdst (ds4_kernels.cu:2780). Here we run the norm+rope skeleton; the
    //   packed-cache write is not implemented (see finish_norm_rope_kernel TODO).
    finish_norm_rope_kernel<<<n_vec, kThreads, 0, s>>>(x, len, stride, f);
    if (f.pdst != nullptr || f.dst != nullptr)
        std::fprintf(stderr, "ds4_mla finish: packed/dst write path is a stub (TODO box)\n");
}

void hc_mix(const float* h, const float* fn, int hcdim, int n_mix, int T, float eps, float* mixes, cudaStream_t s) {
    hc_mix_kernel<<<dim3(n_mix, T), kThreads, 0, s>>>(h, fn, hcdim, eps, mixes);
}
void hc_split(const float*, const float*, const float*, int, int, float, int T, float* pre, float* post, float* comb,
              cudaStream_t s) {
    // STRUCTURED STUB. TODO(box): port hc_split_kernel (the sinkhorn pre/post/comb
    //   split) from engines/maya-ref/src/ds4/ds4_kernels.cu; strides pre/post 8,
    //   comb 64 (ds4_kernels.cuh:66-71). Zero so the residual add is a no-op until then.
    if (T > 0) {
        cudaMemsetAsync(pre, 0, (size_t) T * 8 * sizeof(float), s);
        cudaMemsetAsync(post, 0, (size_t) T * 8 * sizeof(float), s);
        cudaMemsetAsync(comb, 0, (size_t) T * 64 * sizeof(float), s);
    }
}
void hc_pre_norm(const float* h, const float* mix, const float* w, int dim, int hc, int T, float eps, float* x,
                 cudaStream_t s) {
    hc_pre_norm_kernel<<<T, 1024, 0, s>>>(h, mix, w, dim, hc, eps, x);
}
void hc_post(const float* y, const float* res, const float* post, const float* comb, int dim, int hc, int T,
             float* out, cudaStream_t s) {
    hc_post_kernel<<<dim3(cdiv(dim * hc, kThreads), T), kThreads, 0, s>>>(y, res, post, comb, dim, hc, out);
}
void hc_mean(const float* h, int T, int hc, int dim, float* out, long long out_stride, cudaStream_t s) {
    if (T > 0) hc_mean_kernel<<<dim3(cdiv(dim, kThreads), T), kThreads, 0, s>>>(h, hc, dim, out, out_stride);
}

void compress_win(const float*, const float*, float*, float*, int, int, int, int d, const float*, float, float* latent,
                  cudaStream_t s) {
    // STRUCTURED STUB. TODO(box): port compress_kernel from ds4_kernels.cu (~ln
    //   588): the ratio-r window compressor (softmax-over-slots pool + rmsnorm(w)),
    //   per ds4_kernels.cuh:109-113. Until then the latent rows are left untouched.
    (void) d; (void) latent; (void) s;
    std::fprintf(stderr, "ds4_mla compress_win: stub (TODO box: compress_kernel)\n");
}

void ring_to_ext(const uint8_t*, uint16_t* ext, int64_t, int n, int d, cudaStream_t s) {
    // STRUCTURED STUB. TODO(box): port ring_to_ext (ds4_kernels.cuh:186-190): decode
    //   the kKvFp8 ring rows [(p0-n+r)%n] to BF16 into ext (zeros before position 0).
    //   Needs the E4M3 decode (ds4_numerics.cuh:27-40). Zero for now.
    if (n > 0) cudaMemsetAsync(ext, 0, (size_t) n * d * sizeof(uint16_t), s);
}
void ext_to_ring(const uint16_t*, uint8_t*, int64_t, int, int, int, cudaStream_t,
                 uint8_t*) {
    // STRUCTURED STUB. TODO(box): port ext_to_ring (ds4_kernels.cuh:191-192): pack the
    //   window's new BF16 rows back into the kKvFp8 ring (group-of-32 power-of-two
    //   scale), optionally backing up replaced rows for speculative rollback.
    std::fprintf(stderr, "ds4_mla ext_to_ring: stub (TODO box)\n");
}
void ring_restore(uint8_t*, const uint8_t*, int64_t, int, int, int, int, cudaStream_t) {
    // STRUCTURED STUB. TODO(box): ring_restore (ds4_kernels.cuh:193-194).
    std::fprintf(stderr, "ds4_mla ring_restore: stub (TODO box)\n");
}

void sparse_attn(const float*, const uint16_t*, const void*, const int*, int, const AttnRows& a, const float*,
                 int heads, int d, int T, float, float* out, cudaStream_t s, int, int, const AttnScratch*) {
    // STRUCTURED STUB (the MLA attention core). TODO(box): port sparse_attn from
    //   engines/maya-ref/src/ds4/ds4_kernels.cu, including the FP32 reference path,
    //   the sm_80+ tensor-core (mma.sync BF16) path and the RDNA WMMA path, plus the
    //   learned per-head sink logit and the kKvFp4 / kKvBf16 ckv decode
    //   (ds4_kernels.cuh:159-185). Parity target: tools/ds4/ref.py. Zero so the
    //   graph links and runs end-to-end (producing wrong logits) until filled.
    (void) a;
    if (T > 0) cudaMemsetAsync(out, 0, (size_t) T * heads * d * sizeof(float), s);
}

void engram_dequant(const uint8_t*, const uint8_t*, int, int, float*, cudaStream_t, bool) {
    // STRUCTURED STUB. TODO(box): port engram_dequant (ds4_kernels.cuh:217-220):
    //   FP8-E4M3 rows (or ggml Q6_K superblocks when q6k) -> F32.
    std::fprintf(stderr, "ds4_mla engram_dequant: stub (TODO box)\n");
}
void engram_gate(float*, const float*, const float*, int, int, int, float, cudaStream_t) {
    // STRUCTURED STUB. TODO(box): port engram_gate (ds4_kernels.cuh:225-227): the
    //   gated n-gram value add into the residual stream at engram layers.
    std::fprintf(stderr, "ds4_mla engram_gate: stub (TODO box)\n");
}

}  // namespace u::ds4

#endif  // STRATA_U_CUDA
