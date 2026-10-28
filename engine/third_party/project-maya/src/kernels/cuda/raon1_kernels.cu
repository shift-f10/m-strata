// src/kernels/cuda/raon1_kernels.cu - raon1-specific CUDA kernels.  See strata/kernels/raon1.hpp.
#include "strata/kernels/raon1.hpp"

#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <cstdio>

namespace strata::kernels {

// bfloat16 -> float (the fast path keeps attn_k_b / attn_v_b as BF16, where the reference step path dequantizes
// them to F32); visible to the anonymous-namespace kernels below.
__device__ __forceinline__ float rk_bf(uint16_t h) { return __uint_as_float((uint32_t) h << 16); }
// -infinity by its bits (MSVC's INFINITY is an overflowing 1e300 * 1e300, which nvcc warns about: #221-D)
__device__ __forceinline__ float rk_neg_inf() { return __int_as_float((int) 0xff800000u); }

namespace {

constexpr int RK_THREADS = 256;

// Numerically stable softplus: log(1 + e^z) = max(z,0) + log1p(e^-|z|).
__device__ __forceinline__ float softplus_stable(float z) {
    return fmaxf(z, 0.0f) + log1pf(expf(-fabsf(z)));
}

__global__ void raon1_rda_vscale_kernel(const float* __restrict__ beta, const float* __restrict__ delta,
                                        float* __restrict__ v, int head_dim, long long n) {
    const long long i = (long long) blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const int h = (int) (i / head_dim);
    const float b = beta[h];
    const float d = delta[h];
    const float erase = softplus_stable(-b);
    const float write = softplus_stable(-(b + d));
    v[i] *= expf(erase - write);
}

}  // namespace

void raon1_rda_vscale(const float* beta, const float* delta, float* v, int head_dim, int n_head, void* stream) {
    const long long n = (long long) head_dim * n_head;
    if (n <= 0) return;
    const long long blocks = (n + RK_THREADS - 1) / RK_THREADS;
    raon1_rda_vscale_kernel<<<(unsigned) blocks, RK_THREADS, 0, (cudaStream_t) stream>>>(beta, delta, v, head_dim, n);
}

// raon1 Gated-MLA decode attention (one query token, n_cells cache cells).  NoPE, absorbed: the score runs over
// kw = kv_lora + n_extra channels (Qcur = [absorbed-nope latent | 64 extra]; kcache row = [norm latent | 64
// extra]); the VALUE is only the first kv_lora of each cell (the latent), then wv_b maps kv_lora -> v_head.
// One WARP per head; ctx[kv_lora] is held distributed across the 32 lanes (kv_lora must be a multiple of 32).
// Online (flash-style) softmax so nothing scales with n_cells in memory.  ik_llama.cpp build_raon1.cpp.
namespace {
__global__ void raon1_mla_attn_kernel(const float* __restrict__ q_abs, const float* __restrict__ kcache,
                                      int n_cells, const float* __restrict__ wv_b, int kv_lora, int n_extra,
                                      int v_head, float scale, float* __restrict__ out) {
    const int h    = blockIdx.x;                 // one head per block
    const int lane = threadIdx.x & 31;           // one warp per block
    const int kw   = kv_lora + n_extra;
    const int J    = kv_lora / 32;               // ctx entries this lane owns (kv_lora % 32 == 0)
    const float* qh = q_abs + (size_t) kw * h;

    float ctx[64];                               // J <= kv_lora/32 (512/32 = 16); 64 is a safe cap
    for (int j = 0; j < J; ++j) ctx[j] = 0.0f;
    float m = rk_neg_inf(), l = 0.0f;

    for (int s = 0; s < n_cells; ++s) {
        const float* kc = kcache + (size_t) kw * s;
        float part = 0.0f;
        for (int i = lane; i < kw; i += 32) part += qh[i] * kc[i];
        for (int off = 16; off > 0; off >>= 1) part += __shfl_down_sync(0xffffffffu, part, off);
        const float score = __shfl_sync(0xffffffffu, part, 0) * scale;   // broadcast; identical on all lanes
        const float m_new = fmaxf(m, score);
        const float corr  = (m == rk_neg_inf()) ? 0.0f : __expf(m - m_new);
        const float p     = __expf(score - m_new);
        for (int j = 0; j < J; ++j) ctx[j] = ctx[j] * corr + p * kc[lane + 32 * j];
        l = l * corr + p;
        m = m_new;
    }
    const float inv = (l > 0.0f) ? 1.0f / l : 0.0f;
    for (int j = 0; j < J; ++j) ctx[j] *= inv;

    // out[v] = sum_c wv_b[c + kv_lora*v + kv_lora*v_head*h] * ctx[c]  (reduce the lane-distributed ctx over the warp)
    for (int v = 0; v < v_head; ++v) {
        const float* wv = wv_b + (size_t) kv_lora * v + (size_t) kv_lora * v_head * h;
        float acc = 0.0f;
        for (int j = 0; j < J; ++j) acc += wv[lane + 32 * j] * ctx[j];
        for (int off = 16; off > 0; off >>= 1) acc += __shfl_down_sync(0xffffffffu, acc, off);
        if (lane == 0) out[(size_t) v_head * h + v] = acc;
    }
}
}  // namespace

void raon1_mla_attention(const float* q_abs, const float* kcache, int n_cells, const float* wv_b, int kv_lora,
                         int n_extra, int n_head, int v_head, float scale, float* out, void* stream) {
    if (n_cells <= 0 || n_head <= 0) return;
    raon1_mla_attn_kernel<<<(unsigned) n_head, 32, 0, (cudaStream_t) stream>>>(q_abs, kcache, n_cells, wv_b, kv_lora,
                                                                               n_extra, v_head, scale, out);
}

// Build raon1's Qcur [kw = kv_lora + n_extra, n_head] from q_b's output [head_k = qk_nope + n_extra, n_head]:
// the first kv_lora channels are q's nope part absorbed through wk_b (same layout as q_absorb_kernel), and the
// last n_extra channels are q's rope/extra part copied through un-rotated (NoPE).
namespace {
__global__ void raon1_build_qcur_kernel(const float* __restrict__ wk_b, const float* __restrict__ q,
                                        float* __restrict__ qcur, int qk_nope, int n_extra, int kv_lora,
                                        int n_head) {
    const int kw     = kv_lora + n_extra;
    const int head_k = qk_nope + n_extra;
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= kw * n_head) return;
    const int h = idx / kw;
    const int c = idx % kw;
    if (c < kv_lora) {
        const float* w  = wk_b + (size_t) qk_nope * kv_lora * h;   // (e, c) at e + qk_nope*c
        const float* qh = q + (size_t) head_k * h;
        float acc = 0.0f;
        for (int e = 0; e < qk_nope; ++e) acc += w[(size_t) e + (size_t) qk_nope * c] * qh[e];
        qcur[idx] = acc;
    } else {
        const int ce = c - kv_lora;
        qcur[idx] = q[(size_t) (qk_nope + ce) + (size_t) head_k * h];
    }
}

__global__ void raon1_gate_mul_kernel(float* __restrict__ x, const float* __restrict__ gate, long long n) {
    const long long i = (long long) blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    x[i] *= 1.0f / (1.0f + expf(-gate[i]));
}
}  // namespace

void raon1_build_qcur(const float* wk_b, const float* q, float* qcur, int qk_nope, int n_extra, int kv_lora,
                      int n_head, void* stream) {
    const int n = (kv_lora + n_extra) * n_head;
    if (n <= 0) return;
    raon1_build_qcur_kernel<<<(unsigned) ((n + RK_THREADS - 1) / RK_THREADS), RK_THREADS, 0,
                              (cudaStream_t) stream>>>(wk_b, q, qcur, qk_nope, n_extra, kv_lora, n_head);
}

void raon1_gate_mul(float* x, const float* gate, int64_t n, void* stream) {
    if (n <= 0) return;
    const long long blocks = (n + RK_THREADS - 1) / RK_THREADS;
    raon1_gate_mul_kernel<<<(unsigned) blocks, RK_THREADS, 0, (cudaStream_t) stream>>>(x, gate, n);
}

// raon1 mHC (Raon-mHC), the Helmert-matrix hyper-connections (NOT GLM's Sinkhorn mHC).  The fn projection emits
// hc_mix_dim = 2*hc + (hc-1)^2 values: hc pre gates, hc post gates, then (hc-1)^2 Helmert coordinates.  See
// ik_llama / KRAFTON llama.cpp src/models/raon1.cpp build_hc_pre.  `mixes` is fn . R (already gemv'd, NOT yet
// divided by the joint rms).  Computes pre[hc], post[hc], comb[hc*hc] (comb[d*hc + s]).  One block; the Helmert
// algebra (hc <= 8) runs on thread 0 after a block-wide rms reduction over R.
namespace {
__device__ __forceinline__ float raon1_H(int i, int j) {   // Helmert H[i][j], i<hc, j<nm
    const float num = (i <= j) ? 1.0f : (i == j + 1) ? -(float) (j + 1) : 0.0f;
    return num / sqrtf((float) (j + 1) * (float) (j + 2));
}

__global__ void raon1_hc_gates_kernel(const float* __restrict__ R, const float* __restrict__ mixes,
                                      const float* __restrict__ scale, const float* __restrict__ base,
                                      float hc_eps, float magnitude, int n_embd, int hc,
                                      float* __restrict__ pre, float* __restrict__ post,
                                      float* __restrict__ comb) {
    const long long hc_dim = (long long) hc * n_embd;
    __shared__ float sh[256];
    float acc = 0.0f;
    for (long long k = threadIdx.x; k < hc_dim; k += blockDim.x) { const float v = R[k]; acc += v * v; }
    sh[threadIdx.x] = acc;
    __syncthreads();
    for (int s = blockDim.x / 2; s > 0; s >>= 1) {
        if (threadIdx.x < s) sh[threadIdx.x] += sh[threadIdx.x + s];
        __syncthreads();
    }
    if (threadIdx.x != 0) return;
    const float rms = sqrtf(sh[0] / (float) hc_dim + hc_eps);
    const int nm = hc - 1;

    for (int i = 0; i < hc; ++i) {
        const float mp = mixes[i] / rms;
        pre[i] = 1.0f / (1.0f + expf(-(mp * scale[0] + base[i])));
        const float mo = mixes[hc + i] / rms;
        post[i] = magnitude / (1.0f + expf(-(mo * scale[1] + base[hc + i])));
    }

    // z (nm x nm, column index fastest): z[b + nm*a] from the affine of the Helmert coordinates.
    float z[64];
    for (int j = 0; j < nm * nm; ++j) z[j] = (mixes[2 * hc + j] / rms) * scale[2] + base[2 * hc + j];

    const float gamma = 1.0f / (1.0f + expf(-scale[3]));
    // e[d][s] = sum_{a,b} H[d][a] H[s][b] z[b + nm*a]   (= H z H^T, matching the ggml mul_mat order)
    float e[64];
    for (int d = 0; d < hc; ++d)
        for (int s = 0; s < hc; ++s) {
            float a2 = 0.0f;
            for (int a = 0; a < nm; ++a)
                for (int b = 0; b < nm; ++b) a2 += raon1_H(d, a) * raon1_H(s, b) * z[b + nm * a];
            e[d * hc + s] = a2;
        }
    // rho = 1 + max(0, max_{d,s} (-e / chart_base)),  chart_base = 0.5*I + 0.5/hc
    float maxr = 0.0f;
    for (int d = 0; d < hc; ++d)
        for (int s = 0; s < hc; ++s) {
            const float cb = 0.5f * (d == s ? 1.0f : 0.0f) + 0.5f / (float) hc;
            const float r = -e[d * hc + s] / cb;
            if (r > maxr) maxr = r;
        }
    const float rho = 1.0f + maxr;
    // comb = I + gamma*(phi - I),  phi = chart_base + e/rho
    for (int d = 0; d < hc; ++d)
        for (int s = 0; s < hc; ++s) {
            const float cb = 0.5f * (d == s ? 1.0f : 0.0f) + 0.5f / (float) hc;
            const float I = (d == s) ? 1.0f : 0.0f;
            const float phi = cb + e[d * hc + s] / rho;
            comb[d * hc + s] = I + gamma * (phi - I);
        }
}

__global__ void raon1_hc_mix_kernel(const float* __restrict__ R, const float* __restrict__ pre, int n_embd, int hc,
                                    float* __restrict__ mixed) {
    const int e = blockIdx.x * blockDim.x + threadIdx.x;
    if (e >= n_embd) return;
    float acc = 0.0f;
    for (int i = 0; i < hc; ++i) acc += pre[i] * R[(size_t) i * n_embd + e];
    mixed[e] = acc;
}
}  // namespace

void raon1_hc_gates(const float* R, const float* mixes, const float* scale, const float* base, float hc_eps,
                    float magnitude, int n_embd, int hc, float* pre, float* post, float* comb, void* stream) {
    raon1_hc_gates_kernel<<<1, 256, 0, (cudaStream_t) stream>>>(R, mixes, scale, base, hc_eps, magnitude, n_embd, hc,
                                                                pre, post, comb);
}

void raon1_hc_mix(const float* R, const float* pre, int n_embd, int hc, float* mixed, void* stream) {
    if (n_embd <= 0) return;
    const int blocks = (n_embd + RK_THREADS - 1) / RK_THREADS;
    raon1_hc_mix_kernel<<<(unsigned) blocks, RK_THREADS, 0, (cudaStream_t) stream>>>(R, pre, n_embd, hc, mixed);
}

// ---- BF16-weight variants for the fast path (attn_k_b / attn_v_b are BF16 there, F32 in the reference) --------
namespace {
__global__ void raon1_build_qcur_bf16_kernel(const uint16_t* __restrict__ wk_b, const float* __restrict__ q,
                                             float* __restrict__ qcur, int qk_nope, int n_extra, int kv_lora,
                                             int n_head) {
    const int kw = kv_lora + n_extra, head_k = qk_nope + n_extra;
    const int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= kw * n_head) return;
    const int h = idx / kw, c = idx % kw;
    if (c < kv_lora) {
        const uint16_t* w = wk_b + (size_t) qk_nope * kv_lora * h;
        const float* qh = q + (size_t) head_k * h;
        float acc = 0.0f;
        for (int e = 0; e < qk_nope; ++e) acc += rk_bf(w[(size_t) e + (size_t) qk_nope * c]) * qh[e];
        qcur[idx] = acc;
    } else {
        qcur[idx] = q[(size_t) (qk_nope + (c - kv_lora)) + (size_t) head_k * h];
    }
}

__global__ void raon1_mla_attn_bf16_kernel(const float* __restrict__ q_abs, const float* __restrict__ kcache,
                                           int n_cells, const uint16_t* __restrict__ wv_b, int kv_lora, int n_extra,
                                           int v_head, float scale, float* __restrict__ out) {
    const int h = blockIdx.x, lane = threadIdx.x & 31, kw = kv_lora + n_extra, J = kv_lora / 32;
    const float* qh = q_abs + (size_t) kw * h;
    float ctx[64];
    for (int j = 0; j < J; ++j) ctx[j] = 0.0f;
    float m = rk_neg_inf(), l = 0.0f;
    for (int sc = 0; sc < n_cells; ++sc) {
        const float* kc = kcache + (size_t) kw * sc;
        float part = 0.0f;
        for (int i = lane; i < kw; i += 32) part += qh[i] * kc[i];
        for (int off = 16; off > 0; off >>= 1) part += __shfl_down_sync(0xffffffffu, part, off);
        const float score = __shfl_sync(0xffffffffu, part, 0) * scale;
        const float m_new = fmaxf(m, score);
        const float corr = (m == rk_neg_inf()) ? 0.0f : __expf(m - m_new);
        const float pp = __expf(score - m_new);
        for (int j = 0; j < J; ++j) ctx[j] = ctx[j] * corr + pp * kc[lane + 32 * j];
        l = l * corr + pp;
        m = m_new;
    }
    const float inv = (l > 0.0f) ? 1.0f / l : 0.0f;
    for (int j = 0; j < J; ++j) ctx[j] *= inv;
    for (int v = 0; v < v_head; ++v) {
        const uint16_t* wv = wv_b + (size_t) kv_lora * v + (size_t) kv_lora * v_head * h;
        float acc = 0.0f;
        for (int j = 0; j < J; ++j) acc += rk_bf(wv[lane + 32 * j]) * ctx[j];
        for (int off = 16; off > 0; off >>= 1) acc += __shfl_down_sync(0xffffffffu, acc, off);
        if (lane == 0) out[(size_t) v_head * h + v] = acc;
    }
}
}  // namespace

void raon1_build_qcur_bf16(const uint16_t* wk_b, const float* q, float* qcur, int qk_nope, int n_extra, int kv_lora,
                           int n_head, void* stream) {
    const int n = (kv_lora + n_extra) * n_head;
    if (n <= 0) return;
    raon1_build_qcur_bf16_kernel<<<(unsigned) ((n + RK_THREADS - 1) / RK_THREADS), RK_THREADS, 0,
                                   (cudaStream_t) stream>>>(wk_b, q, qcur, qk_nope, n_extra, kv_lora, n_head);
}

void raon1_mla_attention_bf16(const float* q_abs, const float* kcache, int n_cells, const uint16_t* wv_b, int kv_lora,
                              int n_extra, int n_head, int v_head, float scale, float* out, void* stream) {
    if (n_cells <= 0 || n_head <= 0) return;
    raon1_mla_attn_bf16_kernel<<<(unsigned) n_head, 32, 0, (cudaStream_t) stream>>>(q_abs, kcache, n_cells, wv_b,
                                                                                   kv_lora, n_extra, v_head, scale, out);
}

// ---- the prompt path's batched forms --------------------------------------------------------------------------
namespace {
// a 256-thread block's sum (every thread gets it); red holds 8 floats
__device__ __forceinline__ float rk_block_sum(float v, float* red) {
    for (int off = 16; off > 0; off >>= 1) v += __shfl_down_sync(0xffffffffu, v, off);
    const int lane = threadIdx.x & 31, warp = threadIdx.x >> 5;
    __syncthreads();   // (red may still be read from an earlier call)
    if (lane == 0) red[warp] = v;
    __syncthreads();
    float t = 0.0f;
    for (int w = 0; w < (int) (blockDim.x >> 5); ++w) t += red[w];
    return t;
}
__device__ __forceinline__ float rk_block_max(float v, float* red) {
    for (int off = 16; off > 0; off >>= 1) v = fmaxf(v, __shfl_down_sync(0xffffffffu, v, off));
    const int lane = threadIdx.x & 31, warp = threadIdx.x >> 5;
    __syncthreads();
    if (lane == 0) red[warp] = v;
    __syncthreads();
    float t = rk_neg_inf();
    for (int w = 0; w < (int) (blockDim.x >> 5); ++w) t = fmaxf(t, red[w]);
    return t;
}

__global__ void __launch_bounds__(RK_THREADS) raon1_rms_rows_kernel(const float* __restrict__ in, int ld_in,
                                                                    const float* __restrict__ w, float eps, int n,
                                                                    float* __restrict__ out, int ld_out) {
    __shared__ float red[RK_THREADS / 32];
    const float* x = in + (size_t) blockIdx.x * ld_in;
    float* y = out + (size_t) blockIdx.x * ld_out;
    float ss = 0.0f;
    for (int i = threadIdx.x; i < n; i += RK_THREADS) ss += x[i] * x[i];
    const float inv = rsqrtf(rk_block_sum(ss, red) / (float) n + eps);
    for (int i = threadIdx.x; i < n; i += RK_THREADS) y[i] = x[i] * inv * w[i];
}

__global__ void raon1_rda_vscale_rows_kernel(const float* __restrict__ beta, int ld_beta, const float* __restrict__ delta,
                                             int ld_delta, float* __restrict__ v, int ld_v, int head_dim, int n_head,
                                             long long n) {
    const long long i = (long long) blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const long long per = (long long) n_head * head_dim;
    const int t = (int) (i / per), r = (int) (i % per), h = r / head_dim;
    const float b = beta[(size_t) t * ld_beta + h];
    const float d = delta[(size_t) t * ld_delta + h];
    v[(size_t) t * ld_v + r] *= expf(softplus_stable(-b) - softplus_stable(-(b + d)));
}

__global__ void __launch_bounds__(RK_THREADS) raon1_softmax_causal_kernel(float* __restrict__ sc, int n_cells, int qb,
                                                                          int first) {
    __shared__ float red[RK_THREADS / 32];
    const int t = blockIdx.x, h = blockIdx.y;
    float* col = sc + ((size_t) t * gridDim.y + h) * n_cells;   // (query t, head h): column t * n_head + h
    (void) qb;
    const int vis = min(n_cells, first + t + 1);
    float m = rk_neg_inf();
    for (int j = threadIdx.x; j < vis; j += RK_THREADS) m = fmaxf(m, col[j]);
    m = rk_block_max(m, red);
    float l = 0.0f;
    for (int j = threadIdx.x; j < vis; j += RK_THREADS) {
        const float e = expf(col[j] - m);
        col[j] = e;
        l += e;
    }
    const float inv = 1.0f / rk_block_sum(l, red);
    for (int j = threadIdx.x; j < n_cells; j += RK_THREADS) col[j] = j < vis ? col[j] * inv : 0.0f;
}
}  // namespace

void raon1_rms_rows(const float* in, int ld_in, const float* w, float eps, int n, int T, float* out, int ld_out,
                    void* stream) {
    if (T <= 0 || n <= 0) return;
    raon1_rms_rows_kernel<<<(unsigned) T, RK_THREADS, 0, (cudaStream_t) stream>>>(in, ld_in, w, eps, n, out, ld_out);
}

void raon1_rda_vscale_rows(const float* beta, int ld_beta, const float* delta, int ld_delta, float* v, int ld_v,
                           int head_dim, int n_head, int T, void* stream) {
    const long long n = (long long) T * n_head * head_dim;
    if (n <= 0) return;
    raon1_rda_vscale_rows_kernel<<<(unsigned) ((n + RK_THREADS - 1) / RK_THREADS), RK_THREADS, 0,
                                   (cudaStream_t) stream>>>(beta, ld_beta, delta, ld_delta, v, ld_v, head_dim, n_head, n);
}

void raon1_softmax_causal(float* sc, int n_cells, int qb, int n_head, int first, void* stream) {
    if (n_cells <= 0 || qb <= 0 || n_head <= 0) return;
    raon1_softmax_causal_kernel<<<dim3((unsigned) qb, (unsigned) n_head), RK_THREADS, 0, (cudaStream_t) stream>>>(
        sc, n_cells, qb, first);
}

// ---- raon1's latent cache formats, and the decode's split attention over them ------------------------------------
namespace {
// value `lane` of block k (32 values) of a cache row
template <int TYPE>
__device__ __forceinline__ float rk_cell(const uint8_t* __restrict__ row, int k, int lane) {
    if (TYPE == 8) {
        const uint8_t* b = row + (size_t) k * 34;
        return __half2float(*(const __half*) b) * (float) (int8_t) b[2 + lane];
    }
    if (TYPE == 2) {
        const uint8_t* b = row + (size_t) k * 18;
        const uint8_t q = b[2 + (lane & 15)];
        return __half2float(*(const __half*) b) * (float) ((lane < 16 ? (q & 15) : (q >> 4)) - 8);
    }
    return ((const float*) row)[32 * k + lane];
}

// one warp a block of 32 values: T rows of kw values into cache rows rb bytes apart
template <int TYPE>
__global__ void __launch_bounds__(RK_THREADS) rk_lat_put_kernel(const float* __restrict__ x, int T, int kw,
                                                               uint8_t* __restrict__ dst, size_t rb) {
    const int nb = kw / 32, lane = threadIdx.x & 31;
    const long long w = (long long) blockIdx.x * (RK_THREADS / 32) + (threadIdx.x >> 5);
    if (w >= (long long) T * nb) return;   // (w is the whole warp's)
    const int t = (int) (w / nb), k = (int) (w % nb);
    const float v = x[(size_t) t * kw + 32 * k + lane];
    uint8_t* const row = dst + (size_t) t * rb;
    if (TYPE == 8) {
        // quantize_row_q8_0_ref: d = amax / 127, q = round(x / d)
        float amax = fabsf(v);
        for (int off = 16; off > 0; off >>= 1) amax = fmaxf(amax, __shfl_xor_sync(0xffffffffu, amax, off));
        const float d = amax / 127.0f, id = d != 0.0f ? 1.0f / d : 0.0f;
        uint8_t* const b = row + (size_t) k * 34;
        if (lane == 0) *(__half*) b = __float2half_rn(d);
        b[2 + lane] = (uint8_t) (int8_t) roundf(v * id);
    } else if (TYPE == 2) {
        // quantize_row_q4_0_ref: max = the value of the largest |x| (the first of equals), d = max / -8, code j of
        // the block's first half in byte j's low nibble and code j + 16 in its high one, min(15, (int8_t) (x/d + 8.5))
        float a = fabsf(v), mx = v;
        int ix = lane;
        for (int off = 16; off > 0; off >>= 1) {
            const float oa = __shfl_xor_sync(0xffffffffu, a, off), om = __shfl_xor_sync(0xffffffffu, mx, off);
            const int oi = __shfl_xor_sync(0xffffffffu, ix, off);
            if (oa > a || (oa == a && oi < ix)) {
                a = oa;
                mx = om;
                ix = oi;
            }
        }
        const float d = mx / -8.0f, id = d != 0.0f ? 1.0f / d : 0.0f;
        uint8_t* const b = row + (size_t) k * 18;
        if (lane == 0) *(__half*) b = __float2half_rn(d);
        const float hi = __shfl_down_sync(0xffffffffu, v, 16);   // lane j < 16: x[j + 16]
        if (lane < 16) {
            const int c0 = min(15, (int) (int8_t) (v * id + 8.5f)), c1 = min(15, (int) (int8_t) (hi * id + 8.5f));
            b[2 + lane] = (uint8_t) (c0 | (c1 << 4));
        }
    } else {
        ((float*) row)[32 * k + lane] = v;
    }
}

template <int TYPE>
__global__ void rk_lat_get_kernel(const uint8_t* __restrict__ src, long long n, int kw, size_t rb,
                                  float* __restrict__ out) {
    const long long i = (long long) blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const long long t = i / kw;
    const int c = (int) (i % kw);
    out[i] = rk_cell<TYPE>(src + (size_t) t * rb, c >> 5, c & 31);
}

constexpr int kRkNbMax = 24, kRkJMax = 16;   // kw <= 768, kv_lora <= 512 (raon1: 576, 512)

// one warp a (head, chunk): the chunk's cells' online softmax - its max, its sum and its unnormalized context
template <int TYPE>
__global__ void __launch_bounds__(32) rk_mla_part_kernel(const float* __restrict__ q_abs, const uint8_t* __restrict__ cache,
                                                         size_t rb, int n_cells, int chunk, int kv_lora, int n_extra,
                                                         float scale, float* __restrict__ part) {
    const int h = blockIdx.x, sp = blockIdx.y, ns = (int) gridDim.y, lane = threadIdx.x;
    const int kw = kv_lora + n_extra, nb = kw / 32, J = kv_lora / 32;
    float q[kRkNbMax], ctx[kRkJMax];
#pragma unroll
    for (int k = 0; k < kRkNbMax; ++k) q[k] = k < nb ? q_abs[(size_t) kw * h + 32 * k + lane] : 0.0f;
#pragma unroll
    for (int j = 0; j < kRkJMax; ++j) ctx[j] = 0.0f;
    float m = rk_neg_inf(), l = 0.0f;
    const int s0 = sp * chunk, s1 = min(n_cells, s0 + chunk);
    for (int s = s0; s < s1; ++s) {
        const uint8_t* const row = cache + (size_t) s * rb;
        float v[kRkNbMax];
        float dot = 0.0f;
#pragma unroll
        for (int k = 0; k < kRkNbMax; ++k) {
            v[k] = k < nb ? rk_cell<TYPE>(row, k, lane) : 0.0f;
            dot += q[k] * v[k];
        }
        for (int off = 16; off > 0; off >>= 1) dot += __shfl_xor_sync(0xffffffffu, dot, off);
        const float score = dot * scale;
        const float m_new = fmaxf(m, score);
        const float corr = m == rk_neg_inf() ? 0.0f : expf(m - m_new);
        const float p = expf(score - m_new);
#pragma unroll
        for (int j = 0; j < kRkJMax; ++j) ctx[j] = ctx[j] * corr + (j < J ? p * v[j] : 0.0f);
        l = l * corr + p;
        m = m_new;
    }
    float* const o = part + ((size_t) h * ns + sp) * (size_t) (kv_lora + 2);
    if (lane == 0) {
        o[0] = m;
        o[1] = l;
    }
#pragma unroll
    for (int j = 0; j < kRkJMax; ++j)
        if (j < J) o[2 + 32 * j + lane] = ctx[j];
}

// one block a head: the chunks' softmaxes combined into the context, then out = wv_b[h] . context
__global__ void __launch_bounds__(128) rk_mla_combine_kernel(const float* __restrict__ part, int ns, int kv_lora,
                                                             const uint16_t* __restrict__ wv_b, int v_head,
                                                             float* __restrict__ out) {
    __shared__ float cs[kRkJMax * 32];
    __shared__ float wgt[kRaon1MlaSplits];
    const int h = blockIdx.x, tid = threadIdx.x, lane = tid & 31, warp = tid >> 5;
    const size_t pst = (size_t) (kv_lora + 2);
    const float* const base = part + (size_t) h * ns * pst;
    if (tid == 0) {
        float M = rk_neg_inf();
        for (int i = 0; i < ns; ++i) M = fmaxf(M, base[i * pst]);
        float L = 0.0f;
        for (int i = 0; i < ns; ++i) {
            const float mi = base[i * pst], w = mi == rk_neg_inf() ? 0.0f : expf(mi - M);
            wgt[i] = w;
            L += w * base[i * pst + 1];
        }
        const float inv = L > 0.0f ? 1.0f / L : 0.0f;
        for (int i = 0; i < ns; ++i) wgt[i] *= inv;
    }
    __syncthreads();
    for (int c = tid; c < kv_lora; c += blockDim.x) {
        float a = 0.0f;
        for (int i = 0; i < ns; ++i) a += wgt[i] * base[i * pst + 2 + c];
        cs[c] = a;
    }
    __syncthreads();
    const uint16_t* const wv = wv_b + (size_t) kv_lora * v_head * h;
    for (int v = warp; v < v_head; v += (int) (blockDim.x >> 5)) {
        float a = 0.0f;
        for (int c = lane; c < kv_lora; c += 32) a += rk_bf(wv[(size_t) kv_lora * v + c]) * cs[c];
        for (int off = 16; off > 0; off >>= 1) a += __shfl_down_sync(0xffffffffu, a, off);
        if (lane == 0) out[(size_t) v_head * h + v] = a;
    }
}
}  // namespace

void raon1_lat_put(const float* rows, int T, int kw, int type, void* dst, void* stream) {
    if (T <= 0) return;
    const size_t rb = raon1_lat_row_bytes(type, kw);
    const long long warps = (long long) T * (kw / 32);
    const unsigned blocks = (unsigned) ((warps + RK_THREADS / 32 - 1) / (RK_THREADS / 32));
    const cudaStream_t s = (cudaStream_t) stream;
    uint8_t* const d = (uint8_t*) dst;
    if (type == 8) rk_lat_put_kernel<8><<<blocks, RK_THREADS, 0, s>>>(rows, T, kw, d, rb);
    else if (type == 2) rk_lat_put_kernel<2><<<blocks, RK_THREADS, 0, s>>>(rows, T, kw, d, rb);
    else rk_lat_put_kernel<0><<<blocks, RK_THREADS, 0, s>>>(rows, T, kw, d, rb);
}

void raon1_lat_get(const void* src, int n, int kw, int type, float* out, void* stream) {
    const long long total = (long long) n * kw;
    if (total <= 0) return;
    const size_t rb = raon1_lat_row_bytes(type, kw);
    const unsigned blocks = (unsigned) ((total + RK_THREADS - 1) / RK_THREADS);
    const cudaStream_t s = (cudaStream_t) stream;
    const uint8_t* const p = (const uint8_t*) src;
    if (type == 8) rk_lat_get_kernel<8><<<blocks, RK_THREADS, 0, s>>>(p, total, kw, rb, out);
    else if (type == 2) rk_lat_get_kernel<2><<<blocks, RK_THREADS, 0, s>>>(p, total, kw, rb, out);
    else rk_lat_get_kernel<0><<<blocks, RK_THREADS, 0, s>>>(p, total, kw, rb, out);
}

void raon1_mla_decode(const float* q_abs, const void* cache, int type, int n_cells, const uint16_t* wv_b, int kv_lora,
                      int n_extra, int n_head, int v_head, float scale, float* part, float* out, void* stream) {
    if (n_cells <= 0 || n_head <= 0) return;
    const int kw = kv_lora + n_extra;
    if (kv_lora % 32 != 0 || n_extra % 32 != 0 || kv_lora > kRkJMax * 32 || kw > kRkNbMax * 32) {
        std::fprintf(stderr, "raon1_mla_decode: kv_lora %d / extra %d out of its range\n", kv_lora, n_extra);
        return;
    }
    // chunks of at least 128 cells, at most kRaon1MlaSplits of them a head (a short context: one, the plain kernel)
    const int want = (n_cells + 127) / 128;
    const int ns = want < 1 ? 1 : want > kRaon1MlaSplits ? kRaon1MlaSplits : want;
    const int chunk = (n_cells + ns - 1) / ns;
    const size_t rb = raon1_lat_row_bytes(type, kw);
    const cudaStream_t s = (cudaStream_t) stream;
    const uint8_t* const c = (const uint8_t*) cache;
    const dim3 grid((unsigned) n_head, (unsigned) ns);
    if (type == 8) rk_mla_part_kernel<8><<<grid, 32, 0, s>>>(q_abs, c, rb, n_cells, chunk, kv_lora, n_extra, scale, part);
    else if (type == 2) rk_mla_part_kernel<2><<<grid, 32, 0, s>>>(q_abs, c, rb, n_cells, chunk, kv_lora, n_extra, scale, part);
    else rk_mla_part_kernel<0><<<grid, 32, 0, s>>>(q_abs, c, rb, n_cells, chunk, kv_lora, n_extra, scale, part);
    rk_mla_combine_kernel<<<(unsigned) n_head, 128, 0, s>>>(part, ns, kv_lora, wv_b, v_head, out);
}

}  // namespace strata::kernels
