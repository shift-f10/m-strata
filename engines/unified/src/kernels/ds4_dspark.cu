// engines/unified/src/kernels/ds4_dspark.cu
// -----------------------------------------------------------------------------
// Bodies for the ds4 DSpark drafter kernels declared in ds4_dspark.cuh.
// markov_add and confidence are ported faithfully from
// engines/maya-ref/src/ds4/ds4_kernels.cu (markov_kernel :2045, confidence_kernel
// :2159). The tensor-parallel head-group kernels (markov_share / argmax_share /
// markov_pick) are STRUCTURED STUBS: the lean serve path is single-GPU, so the
// graph uses markov_add + the shared sampler instead. TODO(box) ports the TP forms
// for multi-GPU head groups (plan 2026-10-23 T1.4).
//
// Compiled only on the GPU box (STRATA_U_CUDA ON); an empty TU on host / CI.
// -----------------------------------------------------------------------------
#include "kernels/ds4_dspark.cuh"

#if defined(STRATA_U_CUDA)

#include <cstdio>
#include <cuda_runtime.h>
#if !defined(STRATA_USE_HIP)
#include <cuda_bf16.h>
#include <cuda_fp16.h>
#endif

namespace u::ds4 {
namespace {
constexpr int kThreads = 256;
__host__ __device__ constexpr int cdiv(int a, int b) { return (a + b - 1) / b; }

__device__ __forceinline__ float bf16f(uint16_t b) { return __uint_as_float((uint32_t) b << 16); }
__device__ __forceinline__ float warp_sum(float a) {
    for (int o = 16; o > 0; o >>= 1) a += __shfl_down_sync(0xffffffffu, a, o);
    return a;
}
__device__ __forceinline__ float block_sum(float a) {
    __shared__ float warp[32];
    a = warp_sum(a);
    const int lane = threadIdx.x & 31, wid = threadIdx.x >> 5;
    if (lane == 0) warp[wid] = a;
    __syncthreads();
    if (wid == 0) {
        a = lane < cdiv((int) blockDim.x, 32) ? warp[lane] : 0.0f;
        a = warp_sum(a);
    }
    return a;
}

// PORT: ds4_kernels.cu:2045 markov_kernel (faithful): a warp a vocab row,
// logits[v] += E[*tok] . H[v], E staged in shared memory, H read as half2 vec4s.
__global__ void __launch_bounds__(kThreads) markov_kernel(float* __restrict__ logits, const uint16_t* __restrict__ E,
                                                          const uint16_t* __restrict__ H, const int* __restrict__ tok,
                                                          int V, int R) {
    extern __shared__ float er[];
    const uint16_t* erow = E + (size_t) (*tok) * R;
    for (int i = threadIdx.x; i < R; i += blockDim.x) er[i] = bf16f(erow[i]);
    __syncthreads();
    const int lane = threadIdx.x & 31;
    const int v = blockIdx.x * (kThreads / 32) + (threadIdx.x >> 5);
    if (v >= V) return;
    const uint16_t* hr = H + (size_t) v * R;
    float a = 0.0f;
#if !defined(STRATA_USE_HIP)
    for (int c = lane * 8; c < R; c += 32 * 8) {
        const uint4 q = *(const uint4*) (hr + c);
        const __half2* hp = (const __half2*) &q;
#pragma unroll
        for (int i = 0; i < 4; ++i) {
            const float2 f = __half22float2(hp[i]);
            a += f.x * er[c + 2 * i] + f.y * er[c + 2 * i + 1];
        }
    }
#else
    // TODO(box): HIP half2 path (the reference's CUDA uses __half22float2).
    for (int c = lane; c < R; c += 32) a += __half2float(*(const __half*) &hr[c]) * er[c];
#endif
    a = warp_sum(a);
    if (lane == 0) logits[v] += a;
}

// PORT: ds4_kernels.cu:2159 confidence_kernel (faithful):
// out[t] = sigmoid(w . [x[t] | E[tok[t]]]).
__global__ void __launch_bounds__(1024) confidence_kernel(const float* __restrict__ x, const uint16_t* __restrict__ E,
                                                          const int* __restrict__ tok, const float* __restrict__ w,
                                                          int dim, int R, float* __restrict__ out) {
    const int t = blockIdx.x;
    const float* xt = x + (size_t) t * dim;
    const uint16_t* er = E + (size_t) tok[t] * R;
    float a = 0.0f;
    for (int i = threadIdx.x; i < dim + R; i += blockDim.x) a += w[i] * (i < dim ? xt[i] : bf16f(er[i - dim]));
    a = block_sum(a);
    if (threadIdx.x == 0) out[t] = 1.0f / (1.0f + expf(-a));
}
}  // namespace

void markov_add(float* logits, const uint16_t* E, const uint16_t* H, const int* tok, int V, int R, cudaStream_t s) {
    markov_kernel<<<cdiv(V, kThreads / 32), kThreads, (size_t) R * sizeof(float), s>>>(logits, E, H, tok, V, R);
}
void confidence(const float* x, const uint16_t* E, const int* tok, const float* w, int T, int dim, int R, float* out,
                cudaStream_t s) {
    if (T > 0) confidence_kernel<<<T, 1024, 0, s>>>(x, E, tok, w, dim, R, out);
}

void markov_share(float*, const uint16_t*, const uint16_t*, int, int, cudaStream_t) {
    // STRUCTURED STUB (single-GPU serve path uses markov_add). TODO(box): port
    //   markov_share_kernel for TP head groups (ds4_kernels.cuh:296-300).
    std::fprintf(stderr, "ds4_dspark markov_share: stub (TODO box, TP only)\n");
}
void argmax_share(const float*, int, int, unsigned*, unsigned, cudaStream_t) {
    // STRUCTURED STUB. TODO(box): argmax_share_kernel (ds4_kernels.cuh:301).
    std::fprintf(stderr, "ds4_dspark argmax_share: stub (TODO box, TP only)\n");
}
void markov_pick(const unsigned*, int, unsigned, int*, const uint16_t*, uint16_t*, int, cudaStream_t) {
    // STRUCTURED STUB. TODO(box): markov_pick_kernel (ds4_kernels.cuh:302-303).
    std::fprintf(stderr, "ds4_dspark markov_pick: stub (TODO box, TP only)\n");
}

}  // namespace u::ds4

#endif  // STRATA_U_CUDA
