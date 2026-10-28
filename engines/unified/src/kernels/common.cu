// strata-unified-engine: the shared generic GPU kernels (see common.cuh).
//
// PROVENANCE. These are deliberate, reviewed ports (design goal iii -- one
// verified source, not a live submodule dependency). The quantized matvec, its
// Q8_1 quantization and the block layouts/dot orders are, through Maya, pinned to
// llama.cpp 3cf03257f219afbe7334045ff7c6a06ac68c627d (MIT, (c) 2023-2026 the ggml
// authors): ggml/src/ggml-cuda/{quantize.cu,vecdotq.cuh,mmvq.cu,common.cuh} and
// ggml-common.h. Each function cites the Maya file:line it came from.
//
// SCOPE / NON-BLOAT. Only Q8_0/Q4_K/Q5_K/Q6_K of Maya's formats are carried, and
// the multi-column (ncols 2..8) path is a loop of single-column calls -- bitwise
// equal to a single-column call, and far less code than the fused multi-column
// traits kernel. NVIDIA sm_75+, so `__dp4a` is a hardware instruction. This TU is
// compiled with --use_fast_math (the pinned CUDA oracle's requirement; see the
// CUDA block in engines/unified/CMakeLists.txt).
//
// WHAT IS DELIBERATELY LEFT FOR THE GPU BOX (reported, not hidden):
//  * the int8 tensor-core MMQ prefill (Maya strata_mmq / strata_prefill): a
//    measured perf optimization. Quantized prefill here reuses `mmvq` in column
//    groups, which is numerically correct for parity; MMQ is a speed pass to add
//    on the box after a family parities.
//  * speculative-sampling draft/verify (Maya sample_draft/sample_verify): the
//    DFlash/NextN drafting lane's concern (design 2026-10-23 s6.5, "the one detail
//    left"); the base sampler here is greedy + the full top-k/top-p/min-p chain.
//  * the fused multi-column MMVQ layout (a throughput option for large verify
//    windows); the single-column loop is the exact, lean default.
#include <math_constants.h>  // CUDART_INF_F
#include "kernels/common.cuh"
#include "kernels/u_iq.cuh"  // i-quant (IQ2_XXS/IQ2_S/IQ3_XXS) decode matvec for the GLM MoE experts

#include <cublas_v2.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace u {
namespace k {
namespace {

// ================================ shared constants and helpers ================================

constexpr int QK = 256;    // K-quant superblock elements
constexpr int Q8K = 32;    // Q8_1 block elements
constexpr int QI = 32;     // K-quant quants per 32-bit int group for the dot
constexpr int VDR = 2;     // K-quant values decoded per thread step
constexpr int WARPS = 4;
constexpr int WARP = 32;
constexpr int QUANT_THREADS = 256;

// Q8_1 activation block: fp16 scale + fp16 warp-sum of the ORIGINAL floats, then
// 32 int8 codes. native_mmvq.cu:55 / :141-152.
struct Q81Block {
  half2 ds;
  int8_t qs[32];
};
struct Q80Block {  // native_mmvq.cu:95
  half d;
  int8_t qs[32];
};
struct Q4KBlock {  // native_mmvq.cu:75
  half2 dm;
  uint8_t scales[12];
  uint8_t qs[128];
};
struct Q5KBlock {  // native_mmvq.cu:49
  half2 dm;
  uint8_t scales[12];
  uint8_t qh[32];
  uint8_t qs[128];
};
struct Q6KBlock {  // native_mmvq.cu:80
  uint8_t ql[128];
  uint8_t qh[64];
  int8_t scales[16];
  half d;
};
static_assert(sizeof(Q81Block) == 36 && alignof(Q81Block) == 4);
static_assert(sizeof(Q80Block) == 34 && alignof(Q80Block) == 2 && offsetof(Q80Block, qs) == 2);
static_assert(sizeof(Q4KBlock) == 144 && alignof(Q4KBlock) == 4 &&
              offsetof(Q4KBlock, scales) == 4 && offsetof(Q4KBlock, qs) == 16);
static_assert(sizeof(Q5KBlock) == 176 && alignof(Q5KBlock) == 4 && offsetof(Q5KBlock, scales) == 4 &&
              offsetof(Q5KBlock, qh) == 16 && offsetof(Q5KBlock, qs) == 48);
static_assert(sizeof(Q6KBlock) == 210 && alignof(Q6KBlock) == 2 &&
              offsetof(Q6KBlock, qh) == 128 && offsetof(Q6KBlock, scales) == 192 &&
              offsetof(Q6KBlock, d) == 208);

__device__ __forceinline__ float warp_sum(float x) {  // native_mmvq.cu:124
#pragma unroll
  for (int off = WARP / 2; off > 0; off >>= 1) x += __shfl_xor_sync(0xffffffffu, x, off, WARP);
  return x;
}
__device__ __forceinline__ float warp_max(float x) {  // native_mmvq.cu:132
#pragma unroll
  for (int off = WARP / 2; off > 0; off >>= 1) x = fmaxf(x, __shfl_xor_sync(0xffffffffu, x, off, WARP));
  return x;
}

// Q3_K/Q6_K's odd strides give alternate blocks only 2-byte alignment, so an int
// is read as a pair of little-endian int16. native_mmvq.cu:327.
__device__ __forceinline__ int load_int_b2(const void* ptr, int i32) {
  const auto* x = static_cast<const uint16_t*>(ptr);
  return (int)(x[2 * i32]) | ((int)(x[2 * i32 + 1]) << 16);
}

inline unsigned grid_for(std::int64_t n, int threads) {
  return (unsigned)((n + threads - 1) / threads);
}
void launch_check(const char* what) {
  const cudaError_t e = cudaGetLastError();
  if (e != cudaSuccess) {
    std::fprintf(stderr, "u::k::%s launch: %s\n", what, cudaGetErrorString(e));
    std::exit(1);
  }
}
void sync_if_needed(void* stream, const char* what) {
  if (stream != nullptr) return;
  const cudaError_t e = cudaDeviceSynchronize();
  if (e != cudaSuccess) {
    std::fprintf(stderr, "u::k::%s: %s\n", what, cudaGetErrorString(e));
    std::exit(1);
  }
}

// ================================ Q8_1 quantization ================================
// native_mmvq.cu:140-152.
__launch_bounds__(QUANT_THREADS, 1) __global__
    void quantize_q8_1_kernel(const float* __restrict__ x, Q81Block* __restrict__ y, int n_in) {
  const int i = (int)blockIdx.x * QUANT_THREADS + (int)threadIdx.x;
  if (i >= n_in) return;  // n_in is a multiple of 32: only whole warps return
  const float xi = x[i];
  float amax = fabsf(xi);
#pragma unroll
  for (int off = WARP / 2; off > 0; off >>= 1) amax = fmaxf(amax, __shfl_xor_sync(0xffffffffu, amax, off, WARP));
  const float sum = warp_sum(xi);
  const float d = fminf(amax / 127.0f, 65504.0f);
  const int8_t q = amax == 0.0f ? 0 : (int8_t)fmaxf(-127.0f, fminf(127.0f, roundf(xi / d)));
  y[i / Q8K].qs[i % Q8K] = q;
  if (i % Q8K == 0) y[i / Q8K].ds = make_half2(d, fmaxf(-65504.0f, fminf(65504.0f, sum)));
}

// ================================ K-quant / Q8_0 vec-dot (pinned) ================================

// vec_dot_q5_K_q8_1. native_mmvq.cu:155-215.
__device__ __forceinline__ float q5_q8_dot_impl(const int* vl, const int* vh, const int* u,
                                                 const uint8_t* sc, const uint8_t* m, const half2& dm5,
                                                 const float* d8) {
  float sumf_d = 0.0f, sumf_m = 0.0f;
#pragma unroll
  for (int i = 0; i < 2; ++i) {
    const int vl0i = (vl[0] >> (4 * i)) & 0x0f0f0f0f;
    const int vl1i = (vl[1] >> (4 * i)) & 0x0f0f0f0f;
    const int vh0i = ((vh[0] >> i) << 4) & 0x10101010;
    const int vh1i = ((vh[1] >> i) << 4) & 0x10101010;
    const int v0i = vl0i | vh0i;
    const int v1i = vl1i | vh1i;
    const int dot1 = __dp4a(v0i, u[2 * i], __dp4a(v1i, u[2 * i + 1], 0));
    const int dot2 = __dp4a(0x01010101, u[2 * i], __dp4a(0x01010101, u[2 * i + 1], 0));
    sumf_d += d8[i] * (dot1 * sc[i]);
    sumf_m += d8[i] * (dot2 * m[i]);
  }
  const float2 dm5f = __half22float2(dm5);
  return dm5f.x * sumf_d - dm5f.y * sumf_m;
}
__device__ __forceinline__ float q5_q8_dot(const Q5KBlock* bq5, const Q81Block* bq8, int iqs) {
  int vl[2], vh[2], u[4];
  float d8[2];
  const int bq8_offset = 2 * ((iqs / 2) / 4);
  const int* ql = reinterpret_cast<const int*>(bq5->qs + 16 * bq8_offset + 4 * ((iqs / 2) % 4));
  const int* qh = reinterpret_cast<const int*>(bq5->qh + 4 * ((iqs / 2) % 4));
  vl[0] = ql[0];
  vl[1] = ql[4];
  vh[0] = qh[0] >> bq8_offset;
  vh[1] = qh[4] >> bq8_offset;
  const uint16_t* scales = reinterpret_cast<const uint16_t*>(bq5->scales);
  const int j = bq8_offset / 2;
  const int jm = j & 1;
  const uint32_t s0 = scales[jm], s2 = scales[jm + 2], s4 = scales[jm + 4];
  const uint32_t hi = (uint32_t)(-(int32_t)(j >= 2));
  uint16_t aux[2];
  aux[0] = (uint16_t)(((s0 & 0x3f3f) & ~hi) | ((((s4 >> 0) & 0x0f0f) | ((s0 & 0xc0c0) >> 2)) & hi));
  aux[1] = (uint16_t)(((s2 & 0x3f3f) & ~hi) | ((((s4 >> 4) & 0x0f0f) | ((s2 & 0xc0c0) >> 2)) & hi));
  const uint8_t* sc = reinterpret_cast<const uint8_t*>(aux);
  const uint8_t* m = sc + 2;
#pragma unroll
  for (int i = 0; i < 2; ++i) {
    const Q81Block* bq8i = bq8 + bq8_offset + i;
    d8[i] = __low2float(bq8i->ds);
    const int* q8 = reinterpret_cast<const int*>(bq8i->qs) + ((iqs / 2) % 4);
    u[2 * i] = q8[0];
    u[2 * i + 1] = q8[4];
  }
  return q5_q8_dot_impl(vl, vh, u, sc, m, bq5->dm, d8);
}

// vec_dot_q4_K_q8_1. native_mmvq.cu:495-547.
__device__ __forceinline__ float q4_q8_dot_impl(const int* v, const int* u, const uint8_t* sc,
                                                 const uint8_t* m, const half2& dm4, const float* d8) {
  float sumf_d = 0.0f, sumf_m = 0.0f;
#pragma unroll
  for (int i = 0; i < 2; ++i) {
    const int v0i = (v[0] >> (4 * i)) & 0x0f0f0f0f;
    const int v1i = (v[1] >> (4 * i)) & 0x0f0f0f0f;
    const int dot1 = __dp4a(v1i, u[2 * i + 1], __dp4a(v0i, u[2 * i], 0));
    const int dot2 = __dp4a(0x01010101, u[2 * i + 1], __dp4a(0x01010101, u[2 * i], 0));
    sumf_d += d8[i] * (dot1 * sc[i]);
    sumf_m += d8[i] * (dot2 * m[i]);
  }
  const float2 dm4f = __half22float2(dm4);
  return dm4f.x * sumf_d - dm4f.y * sumf_m;
}
__device__ __forceinline__ float q4_q8_dot(const Q4KBlock* bq4, const Q81Block* bq8, int iqs) {
  int v[2], u[4];
  float d8[2];
  const int bq8_offset = 2 * ((iqs / 2) / 4);
  const int* ql = reinterpret_cast<const int*>(bq4->qs + 16 * bq8_offset + 4 * ((iqs / 2) % 4));
  v[0] = ql[0];
  v[1] = ql[4];
  const uint16_t* scales = reinterpret_cast<const uint16_t*>(bq4->scales);
  const int j = bq8_offset / 2;
  const int jm = j & 1;
  const uint32_t s0 = scales[jm], s2 = scales[jm + 2], s4 = scales[jm + 4];
  const uint32_t hi = (uint32_t)(-(int32_t)(j >= 2));
  uint16_t aux[2];
  aux[0] = (uint16_t)(((s0 & 0x3f3f) & ~hi) | ((((s4 >> 0) & 0x0f0f) | ((s0 & 0xc0c0) >> 2)) & hi));
  aux[1] = (uint16_t)(((s2 & 0x3f3f) & ~hi) | ((((s4 >> 4) & 0x0f0f) | ((s2 & 0xc0c0) >> 2)) & hi));
  const uint8_t* sc = reinterpret_cast<const uint8_t*>(aux);
  const uint8_t* m = sc + 2;
#pragma unroll
  for (int i = 0; i < 2; ++i) {
    const Q81Block* bq8i = bq8 + bq8_offset + i;
    d8[i] = __low2float(bq8i->ds);
    const int* q8 = reinterpret_cast<const int*>(bq8i->qs) + ((iqs / 2) % 4);
    u[2 * i] = q8[0];
    u[2 * i + 1] = q8[4];
  }
  return q4_q8_dot_impl(v, u, sc, m, bq4->dm, d8);
}

// vec_dot_q6_K_q8_1. native_mmvq.cu:593-623.
__device__ __forceinline__ float q6_q8_dot_impl(int vl, int vh, const int* u, const int8_t* scales,
                                                 float d, const float* d8) {
  float sumf = 0.0f;
#pragma unroll
  for (int i = 0; i < 2; ++i) {
    const int sc = scales[4 * i];
    const int vil = (vl >> (4 * i)) & 0x0f0f0f0f;
    const int vih = ((vh >> (4 * i)) << 4) & 0x30303030;
    const int vi = __vsubss4(vil | vih, 0x20202020);
    sumf += d8[i] * (__dp4a(vi, u[i], 0) * sc);
  }
  return d * sumf;
}
__device__ __forceinline__ float q6_q8_dot(const Q6KBlock* w, const Q81Block* x, int iqs) {
  const int bq8_offset = 4 * (iqs / 16) + (iqs % 16) / 8;
  const int scale_offset = 8 * (iqs / 16) + (iqs % 16) / 4;
  const int vh_shift = 2 * ((iqs % 16) / 8);
  const int vl = load_int_b2(w->ql, iqs);
  const int vh = load_int_b2(w->qh, 8 * (iqs / 16) + iqs % 8) >> vh_shift;
  int u[2];
  float d8[2];
#pragma unroll
  for (int i = 0; i < 2; ++i) {
    u[i] = reinterpret_cast<const int*>(x[bq8_offset + 2 * i].qs)[iqs % 8];
    d8[i] = __low2float(x[bq8_offset + 2 * i].ds);
  }
  return q6_q8_dot_impl(vl, vh, u, w->scales + scale_offset, (float)w->d, d8);
}

// vec_dot_q8_0_q8_1. native_mmvq.cu:708-720.
__device__ __forceinline__ float q8_0_q8_dot(const Q80Block* w, const Q81Block* x, int iqs) {
  int sumi = 0;
#pragma unroll
  for (int i = 0; i < 2; ++i) {
    const int v = load_int_b2(w->qs, iqs + i);
    const int u = reinterpret_cast<const int*>(x->qs)[iqs + i];
    sumi = __dp4a(v, u, sumi);
  }
  return (float)w->d * __low2float(x->ds) * (float)sumi;
}

// ================================ MMVQ kernels (ncols == 1) ================================
//
// 4 warps, ROWS rows per block (1, or 4 for small K), the oracle's warp-ascending
// shared reduction then an XOR tree. native_mmvq.cu:219-257 (q5_k), :551-589
// (q4_k), :627-662 (q6_k), :738-773 (q8_0, the "small" template).

template <bool SmallK>
__launch_bounds__(WARPS* WARP, 1) __global__
    void q4_k_kernel(const Q4KBlock* __restrict__ w, const Q81Block* __restrict__ x,
                     float* __restrict__ y, int n_in, int n_out) {
  constexpr int ROWS = SmallK ? WARPS : 1;
  constexpr int BPI = VDR * WARPS * WARP / QI;
  const int tid = WARP * (int)threadIdx.y + (int)threadIdx.x;
  const int row0 = ROWS * (int)blockIdx.x;
  const int blocks_per_row = n_in / QK;
  float tmp[ROWS] = {};
  for (int kbx = tid / (QI / VDR); kbx < blocks_per_row; kbx += BPI) {
    const int kby = kbx * (QK / Q8K);
    const int kqs = VDR * (tid % (QI / VDR));
#pragma unroll
    for (int i = 0; i < ROWS; ++i)
      if (row0 + i < n_out)
        tmp[i] += q4_q8_dot(w + (std::size_t)(row0 + i) * blocks_per_row + kbx, x + kby, kqs);
  }
  __shared__ float partial[WARPS - 1][ROWS][WARP];
  if (threadIdx.y > 0)
#pragma unroll
    for (int i = 0; i < ROWS; ++i) partial[threadIdx.y - 1][i][threadIdx.x] = tmp[i];
  __syncthreads();
  if (threadIdx.y > 0) return;
#pragma unroll
  for (int i = 0; i < ROWS; ++i) {
#pragma unroll
    for (int l = 0; l < WARPS - 1; ++l) tmp[i] += partial[l][i][threadIdx.x];
    tmp[i] = warp_sum(tmp[i]);
    if ((int)threadIdx.x == i && row0 + i < n_out) y[row0 + i] = tmp[i];
  }
}

template <bool SmallK>
__launch_bounds__(WARPS* WARP, 1) __global__
    void q5_k_kernel(const Q5KBlock* __restrict__ w, const Q81Block* __restrict__ x,
                     float* __restrict__ y, int n_in, int n_out) {
  constexpr int ROWS = SmallK ? WARPS : 1;
  constexpr int BPI = VDR * WARPS * WARP / QI;
  const int tid = WARP * (int)threadIdx.y + (int)threadIdx.x;
  const int row0 = ROWS * (int)blockIdx.x;
  const int blocks_per_row = n_in / QK;
  float tmp[ROWS] = {};
  for (int kbx = tid / (QI / VDR); kbx < blocks_per_row; kbx += BPI) {
    const int kby = kbx * (QK / Q8K);
    const int kqs = VDR * (tid % (QI / VDR));
#pragma unroll
    for (int i = 0; i < ROWS; ++i)
      if (row0 + i < n_out)
        tmp[i] += q5_q8_dot(w + (std::size_t)(row0 + i) * blocks_per_row + kbx, x + kby, kqs);
  }
  __shared__ float partial[WARPS - 1][ROWS][WARP];
  if (threadIdx.y > 0)
#pragma unroll
    for (int i = 0; i < ROWS; ++i) partial[threadIdx.y - 1][i][threadIdx.x] = tmp[i];
  __syncthreads();
  if (threadIdx.y > 0) return;
#pragma unroll
  for (int i = 0; i < ROWS; ++i) {
#pragma unroll
    for (int l = 0; l < WARPS - 1; ++l) tmp[i] += partial[l][i][threadIdx.x];
    tmp[i] = warp_sum(tmp[i]);
    if ((int)threadIdx.x == i && row0 + i < n_out) y[row0 + i] = tmp[i];
  }
}

template <bool SmallK>
__launch_bounds__(WARPS* WARP, 1) __global__
    void q6_k_kernel(const Q6KBlock* __restrict__ w, const Q81Block* __restrict__ x,
                     float* __restrict__ y, int n_in, int n_out) {
  constexpr int ROWS = SmallK ? WARPS : 1;
  constexpr int BPI = WARPS * WARP / 32;
  const int tid = WARP * (int)threadIdx.y + (int)threadIdx.x;
  const int row0 = ROWS * (int)blockIdx.x;
  const int blocks_per_row = n_in / 256;
  float tmp[ROWS] = {};
  for (int kbx = tid / 32; kbx < blocks_per_row; kbx += BPI) {
    const int kby = kbx * 8;
    const int kqs = tid % 32;
#pragma unroll
    for (int i = 0; i < ROWS; ++i)
      if (row0 + i < n_out)
        tmp[i] += q6_q8_dot(w + (std::size_t)(row0 + i) * blocks_per_row + kbx, x + kby, kqs);
  }
  __shared__ float partial[WARPS - 1][ROWS][WARP];
  if (threadIdx.y > 0)
#pragma unroll
    for (int i = 0; i < ROWS; ++i) partial[threadIdx.y - 1][i][threadIdx.x] = tmp[i];
  __syncthreads();
  if (threadIdx.y > 0) return;
#pragma unroll
  for (int i = 0; i < ROWS; ++i) {
#pragma unroll
    for (int l = 0; l < WARPS - 1; ++l) tmp[i] += partial[l][i][threadIdx.x];
    tmp[i] = warp_sum(tmp[i]);
    if ((int)threadIdx.x == i && row0 + i < n_out) y[row0 + i] = tmp[i];
  }
}

template <bool SmallK>
__launch_bounds__(WARPS* WARP, 1) __global__
    void q8_0_kernel(const Q80Block* __restrict__ w, const Q81Block* __restrict__ x,
                     float* __restrict__ y, int n_in, int n_out) {
  constexpr int Qi = 8;  // QI for Q8_0; VDR=2 (native_mmvq.cu:738, small template)
  constexpr int ROWS = SmallK ? WARPS : 1;
  constexpr int BPI = 2 * WARPS * WARP / Qi;
  const int tid = WARP * (int)threadIdx.y + (int)threadIdx.x;
  const int row0 = ROWS * (int)blockIdx.x;
  const int blocks_per_row = n_in / 32;
  float tmp[ROWS] = {};
  for (int kbx = tid / (Qi / 2); kbx < blocks_per_row; kbx += BPI) {
    const int kqs = 2 * (tid % (Qi / 2));
#pragma unroll
    for (int i = 0; i < ROWS; ++i)
      if (row0 + i < n_out)
        tmp[i] += q8_0_q8_dot(w + (std::size_t)(row0 + i) * blocks_per_row + kbx, x + kbx, kqs);
  }
  __shared__ float partial[WARPS - 1][ROWS][WARP];
  if (threadIdx.y > 0)
#pragma unroll
    for (int i = 0; i < ROWS; ++i) partial[threadIdx.y - 1][i][threadIdx.x] = tmp[i];
  __syncthreads();
  if (threadIdx.y > 0) return;
#pragma unroll
  for (int i = 0; i < ROWS; ++i) {
#pragma unroll
    for (int l = 0; l < WARPS - 1; ++l) tmp[i] += partial[l][i][threadIdx.x];
    tmp[i] = warp_sum(tmp[i]);
    if ((int)threadIdx.x == i && row0 + i < n_out) y[row0 + i] = tmp[i];
  }
}

// ================================ MMVQ kernels, NC = 2..8 columns (the speculative verify window) =================
// The single-column kernels above with the column loop inside (upstream native_mmvq.cu plan v0.3 P3): per
// (column, row) the accumulation over kbx, the cross-warp sum and the warp tree are the ncols = 1 kernel's, so
// every column is BITWISE a single-column call on that column (STRATA_KERNEL_SELFTEST checks it). The weight
// block is re-read per column from L1, so a window of NC tokens still reads the weights from DRAM once. Each
// traits struct transcribes one format's thread-to-block mapping from its kernel above.
struct Q4KTr {
  using Block = Q4KBlock;
  static constexpr int BPR = QK, TPB = QI / VDR, BPI = VDR * WARPS * WARP / QI, KBY = QK / Q8K;
  __device__ static int kqs(int tid) { return VDR * (tid % (QI / VDR)); }
  __device__ static float dot(const Block* w, const Q81Block* x, int kqs) { return q4_q8_dot(w, x, kqs); }
};
struct Q5KTr {
  using Block = Q5KBlock;
  static constexpr int BPR = QK, TPB = QI / VDR, BPI = VDR * WARPS * WARP / QI, KBY = QK / Q8K;
  __device__ static int kqs(int tid) { return VDR * (tid % (QI / VDR)); }
  __device__ static float dot(const Block* w, const Q81Block* x, int kqs) { return q5_q8_dot(w, x, kqs); }
};
struct Q6KTr {
  using Block = Q6KBlock;
  static constexpr int BPR = 256, TPB = 32, BPI = WARPS * WARP / 32, KBY = 8;
  __device__ static int kqs(int tid) { return tid % 32; }
  __device__ static float dot(const Block* w, const Q81Block* x, int kqs) { return q6_q8_dot(w, x, kqs); }
};
struct Q80Tr {
  using Block = Q80Block;
  static constexpr int BPR = 32, TPB = 8 / 2, BPI = 2 * WARPS * WARP / 8, KBY = 1;
  __device__ static int kqs(int tid) { return 2 * (tid % (8 / 2)); }
  __device__ static float dot(const Block* w, const Q81Block* x, int kqs) { return q8_0_q8_dot(w, x, kqs); }
};

template <class Tr, bool SmallK, int NC>
__launch_bounds__(WARPS* WARP, 1) __global__
    void mmvq_multi_kernel(const typename Tr::Block* __restrict__ w, const Q81Block* __restrict__ x, int x_stride,
                           float* __restrict__ y, int n_in, int n_out) {
  constexpr int ROWS = SmallK ? WARPS : 1;
  const int tid = WARP * (int)threadIdx.y + (int)threadIdx.x;
  const int row0 = ROWS * (int)blockIdx.x;
  const int blocks_per_row = n_in / Tr::BPR;
  float tmp[NC][ROWS] = {};
  for (int kbx = tid / Tr::TPB; kbx < blocks_per_row; kbx += Tr::BPI) {
    const int kby = kbx * Tr::KBY;
    const int kqs = Tr::kqs(tid);
#pragma unroll
    for (int i = 0; i < ROWS; ++i)
      if (row0 + i < n_out) {
        const typename Tr::Block* wb = w + (std::size_t)(row0 + i) * blocks_per_row + kbx;
#pragma unroll
        for (int c = 0; c < NC; ++c) tmp[c][i] += Tr::dot(wb, x + (std::size_t)c * x_stride + kby, kqs);
      }
  }
  __shared__ float partial[WARPS - 1][NC][ROWS][WARP];
  if (threadIdx.y > 0)
#pragma unroll
    for (int c = 0; c < NC; ++c)
#pragma unroll
      for (int i = 0; i < ROWS; ++i) partial[threadIdx.y - 1][c][i][threadIdx.x] = tmp[c][i];
  __syncthreads();
  if (threadIdx.y > 0) return;
#pragma unroll
  for (int c = 0; c < NC; ++c)
#pragma unroll
    for (int i = 0; i < ROWS; ++i) {
#pragma unroll
      for (int l = 0; l < WARPS - 1; ++l) tmp[c][i] += partial[l][c][i][threadIdx.x];
      tmp[c][i] = warp_sum(tmp[c][i]);
      if ((int)threadIdx.x == i && row0 + i < n_out) y[(std::size_t)c * n_out + row0 + i] = tmp[c][i];
    }
}

// The same SmallK rule as launch_mmvq_col (a row with fewer than BPI blocks takes WARPS rows a block).
template <class Tr>
void launch_mmvq_multi_tr(const void* w, const Q81Block* x, int x_stride, float* y, int n_in, int n_out, int nc,
                          cudaStream_t s) {
  const dim3 threads(WARP, WARPS);
  const auto* wp = static_cast<const typename Tr::Block*>(w);
  const bool small = n_in / Tr::BPR < Tr::BPI;
#define U_MMVQ_MULTI(N)                                                                                         \
  case N:                                                                                                       \
    if (small)                                                                                                  \
      mmvq_multi_kernel<Tr, true, N><<<grid_for(n_out, WARPS), threads, 0, s>>>(wp, x, x_stride, y, n_in, n_out); \
    else                                                                                                        \
      mmvq_multi_kernel<Tr, false, N><<<(unsigned)n_out, threads, 0, s>>>(wp, x, x_stride, y, n_in, n_out);       \
    break;
  switch (nc) {
    U_MMVQ_MULTI(2) U_MMVQ_MULTI(3) U_MMVQ_MULTI(4) U_MMVQ_MULTI(5) U_MMVQ_MULTI(6) U_MMVQ_MULTI(7) U_MMVQ_MULTI(8)
    default: break;
  }
#undef U_MMVQ_MULTI
}

// One single-column MMVQ launch. The SmallK split (rows-per-block = WARPS when a
// row has fewer than BPI blocks, else 1) is the oracle's. native_mmvq.cu:1166-1190.
void launch_mmvq_col(Quant q, const void* w, const Q81Block* x, float* y, int n_in, int n_out,
                     cudaStream_t s) {
  const dim3 threads(WARP, WARPS);
  switch (q) {
    case Quant::q4_k: {
      const auto* wp = static_cast<const Q4KBlock*>(w);
      if (n_in / QK < VDR * WARPS * WARP / QI)
        q4_k_kernel<true><<<grid_for(n_out, WARPS), threads, 0, s>>>(wp, x, y, n_in, n_out);
      else
        q4_k_kernel<false><<<(unsigned)n_out, threads, 0, s>>>(wp, x, y, n_in, n_out);
      break;
    }
    case Quant::q5_k: {
      const auto* wp = static_cast<const Q5KBlock*>(w);
      if (n_in / QK < VDR * WARPS * WARP / QI)
        q5_k_kernel<true><<<grid_for(n_out, WARPS), threads, 0, s>>>(wp, x, y, n_in, n_out);
      else
        q5_k_kernel<false><<<(unsigned)n_out, threads, 0, s>>>(wp, x, y, n_in, n_out);
      break;
    }
    case Quant::q6_k: {
      const auto* wp = static_cast<const Q6KBlock*>(w);
      if (n_in / 256 < WARPS * WARP / 32)
        q6_k_kernel<true><<<grid_for(n_out, WARPS), threads, 0, s>>>(wp, x, y, n_in, n_out);
      else
        q6_k_kernel<false><<<(unsigned)n_out, threads, 0, s>>>(wp, x, y, n_in, n_out);
      break;
    }
    case Quant::q8_0: {
      const auto* wp = static_cast<const Q80Block*>(w);
      if (n_in / 32 < 2 * WARPS * WARP / 8)
        q8_0_kernel<true><<<grid_for(n_out, WARPS), threads, 0, s>>>(wp, x, y, n_in, n_out);
      else
        q8_0_kernel<false><<<(unsigned)n_out, threads, 0, s>>>(wp, x, y, n_in, n_out);
      break;
    }
    case Quant::iq2_xxs:
    case Quant::iq3_xxs:
    case Quant::iq2_s:
    case Quant::iq3_s:
    case Quant::iq4_xs:
    case Quant::iq4_nl:
    case Quant::q2_0:
      // i-quants (and the Strata-custom Q2_0) use codebook-grid / LUT lookups, a different access
      // pattern than the k-quant template; dispatched to the ported path in u_iq.cu (one warp/row).
      iq_mmvq_col((int)q, w, x, y, n_in, n_out, s);
      break;
    default:
      throw std::invalid_argument("u::k::mmvq: not a quantized format");
  }
}

// ================================ Q6_K row dequant ================================
// ggml dequantize_row_q6_K, one CUDA block per 256-element super-block, 32 threads (one per l). For the
// GLM embedding gather (a quantized token_embd row -> f32). Mirrors ggml-quants.c dequantize_row_q6_K.
__global__ void dequant_q6k_kernel(const Q6KBlock* __restrict__ x, float* __restrict__ y, int nblocks) {
  const int ib = (int)blockIdx.x;
  if (ib >= nblocks) return;
  const int l = (int)threadIdx.x;  // 0..31
  const Q6KBlock& b = x[ib];
  const float d = __half2float(b.d);
  float* yb = y + (std::size_t)ib * 256;
#pragma unroll
  for (int half = 0; half < 2; ++half) {
    const uint8_t* ql = b.ql + half * 64;
    const uint8_t* qh = b.qh + half * 32;
    const int8_t* sc = b.scales + half * 8;
    float* yh = yb + half * 128;
    const int is = l / 16;
    const int8_t q1 = (int8_t)((ql[l + 0] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
    const int8_t q2 = (int8_t)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
    const int8_t q3 = (int8_t)((ql[l + 0] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32;
    const int8_t q4 = (int8_t)((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32;
    yh[l + 0] = d * sc[is + 0] * q1;
    yh[l + 32] = d * sc[is + 2] * q2;
    yh[l + 64] = d * sc[is + 4] * q3;
    yh[l + 96] = d * sc[is + 6] * q4;
  }
}

// ================================ BF16 / F32 GEMV ================================
// bf16_gemv.cu:18-44 (warp form; the few-rows case is a block-per-row reduction). F32 mirrors the warp form.
__global__ void bf16_gemv_warp_kernel(const uint16_t* __restrict__ x, const uint16_t* __restrict__ w,
                                      float* __restrict__ y, long long n_in, long long n_out) {
  const int warps_per_block = (int)(blockDim.x >> 5);
  const long long o = (long long)blockIdx.x * warps_per_block + (threadIdx.x >> 5);
  if (o >= n_out) return;
  const int lane = threadIdx.x & 31;
  const uint16_t* row = w + o * n_in;
  float acc = 0.0f;
  for (long long i = lane; i < n_in; i += 32) acc += f32_from_bf16(x[i]) * f32_from_bf16(row[i]);
  for (int off = 16; off > 0; off >>= 1) acc += __shfl_down_sync(0xffffffffu, acc, off);
  if (lane == 0) y[o] = acc;
}
// One BLOCK per output row for the short-and-wide shapes (n_out < 64 with n_in in the thousands: the
// qwen4exp HC inject 4x10240, ssm_alpha/beta 48x2560, a shared-expert gate 1x2560). The thread-per-row
// form ran each such row as one serial n_in-long dot product (~157 us a call, 63% of qwen4exp's decode GPU
// time under nsys); here the block's threads stride over n_in and reduce across the warps.
__global__ void bf16_gemv_block_kernel(const uint16_t* __restrict__ x, const uint16_t* __restrict__ w,
                                       float* __restrict__ y, long long n_in, long long n_out) {
  const long long o = blockIdx.x;  // uniform per block, so the early return never splits a __syncthreads
  if (o >= n_out) return;
  const uint16_t* row = w + o * n_in;
  float acc = 0.0f;
  for (long long i = threadIdx.x; i < n_in; i += blockDim.x) acc += f32_from_bf16(x[i]) * f32_from_bf16(row[i]);
  for (int off = 16; off > 0; off >>= 1) acc += __shfl_down_sync(0xffffffffu, acc, off);
  __shared__ float part[32];
  const int lane = (int)(threadIdx.x & 31), wid = (int)(threadIdx.x >> 5);
  if (lane == 0) part[wid] = acc;
  __syncthreads();
  if (wid == 0) {
    const int nw = (int)((blockDim.x + 31) >> 5);
    float v = lane < nw ? part[lane] : 0.0f;
    for (int off = 16; off > 0; off >>= 1) v += __shfl_down_sync(0xffffffffu, v, off);
    if (lane == 0) y[o] = v;
  }
}
// ---- BF16 GEMV over NC f32 activation columns (bf16_gemv_f32x). Each 16-byte weight load (8 bf16) is applied
// to every column; a column's activation is rounded to bf16 in the kernel (bitwise f32_to_bf16 + a bf16 x).
// The per-(column, row) arithmetic -- the thread's slice of n_in, its explicit fma order, the warp and block
// reductions -- depends on (n_in, n_out) only, never on NC: every column is bitwise a single-column call.
__device__ __forceinline__ void bf16x8_unpack(const uint4 v, float f[8]) {
  const uint32_t u[4] = {v.x, v.y, v.z, v.w};
#pragma unroll
  for (int i = 0; i < 4; ++i) {
    f[2 * i] = __uint_as_float(u[i] << 16);
    f[2 * i + 1] = __uint_as_float(u[i] & 0xFFFF0000u);
  }
}
__device__ __forceinline__ float bf16_round(float f) { return f32_from_bf16(bf16_from_f32(f)); }
template <int NC>
__device__ __forceinline__ void bf16v_accumulate(const uint4 wv, const float* __restrict__ x, long long n_in,
                                                 long long i8, float acc[NC]) {
  float wf[8];
  bf16x8_unpack(wv, wf);
#pragma unroll
  for (int c = 0; c < NC; ++c) {
    const float4* xp = reinterpret_cast<const float4*>(x + (long long)c * n_in) + 2 * i8;
    const float4 a = xp[0], b = xp[1];
    const float xs[8] = {a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w};
#pragma unroll
    for (int e = 0; e < 8; ++e) acc[c] = __fmaf_rn(bf16_round(xs[e]), wf[e], acc[c]);
  }
}
// one warp per row (rows with few 16-byte chunks: n_in <= 512)
template <int NC>
__global__ void bf16v_warp_kernel(const float* __restrict__ x, const uint16_t* __restrict__ w,
                                  float* __restrict__ y, long long n_in, long long n_out) {
  const int warps_per_block = (int)(blockDim.x >> 5);
  const long long o = (long long)blockIdx.x * warps_per_block + (threadIdx.x >> 5);
  if (o >= n_out) return;
  const int lane = threadIdx.x & 31;
  const uint4* row = reinterpret_cast<const uint4*>(w + o * n_in);
  const long long n8 = n_in >> 3;
  float acc[NC];
#pragma unroll
  for (int c = 0; c < NC; ++c) acc[c] = 0.0f;
  for (long long i = lane; i < n8; i += 32) bf16v_accumulate<NC>(row[i], x, n_in, i, acc);
#pragma unroll
  for (int c = 0; c < NC; ++c) {
    float v = acc[c];
    for (int off = 16; off > 0; off >>= 1) v += __shfl_down_sync(0xffffffffu, v, off);
    if (lane == 0) y[(long long)c * n_out + o] = v;
  }
}
// one block per row (long rows: HC down 320x10240, the router 512x2560, the HC inject 4x10240)
template <int NC>
__global__ void __launch_bounds__(1024) bf16v_block_kernel(const float* __restrict__ x, const uint16_t* __restrict__ w,
                                   float* __restrict__ y, long long n_in, long long n_out) {
  const long long o = blockIdx.x;  // uniform per block, so the early return never splits a __syncthreads
  if (o >= n_out) return;
  const uint4* row = reinterpret_cast<const uint4*>(w + o * n_in);
  const long long n8 = n_in >> 3;
  float acc[NC];
#pragma unroll
  for (int c = 0; c < NC; ++c) acc[c] = 0.0f;
  for (long long i = threadIdx.x; i < n8; i += blockDim.x) bf16v_accumulate<NC>(row[i], x, n_in, i, acc);
  __shared__ float part[NC][32];
  const int lane = (int)(threadIdx.x & 31), wid = (int)(threadIdx.x >> 5);
#pragma unroll
  for (int c = 0; c < NC; ++c) {
    float v = acc[c];
    for (int off = 16; off > 0; off >>= 1) v += __shfl_down_sync(0xffffffffu, v, off);
    if (lane == 0) part[c][wid] = v;
  }
  __syncthreads();
  if (wid == 0) {
    const int nw = (int)((blockDim.x + 31) >> 5);
#pragma unroll
    for (int c = 0; c < NC; ++c) {
      float v = lane < nw ? part[c][lane] : 0.0f;
      for (int off = 16; off > 0; off >>= 1) v += __shfl_down_sync(0xffffffffu, v, off);
      if (lane == 0) y[(long long)c * n_out + o] = v;
    }
  }
}

__global__ void f32_gemv_warp_kernel(const float* __restrict__ x, const float* __restrict__ w,
                                     float* __restrict__ y, long long n_in, long long n_out) {
  const int warps_per_block = (int)(blockDim.x >> 5);
  const long long o = (long long)blockIdx.x * warps_per_block + (threadIdx.x >> 5);
  if (o >= n_out) return;
  const int lane = threadIdx.x & 31;
  const float* row = w + o * n_in;
  float acc = 0.0f;
  for (long long i = lane; i < n_in; i += 32) acc += x[i] * row[i];
  for (int off = 16; off > 0; off >>= 1) acc += __shfl_down_sync(0xffffffffu, acc, off);
  if (lane == 0) y[o] = acc;
}

// ================================ RMSNorm ================================
// elementwise.cu:87-110 (warp-per-row, with the row guard the QSA bug paid for).
__global__ void rms_norm_kernel(float* __restrict__ x, const float* __restrict__ w, std::int64_t rows,
                                std::int64_t cols, float eps) {
  const int lane = threadIdx.x & 31;
  const std::int64_t row = ((std::int64_t)blockIdx.x * (blockDim.x >> 5)) + (threadIdx.x >> 5);
  if (row >= rows) return;  // the guard is not decoration (elementwise.cu:92)
  float* r = x + row * cols;
  float acc = 0.0f;
  for (std::int64_t c = lane; c < cols; c += 32) acc += r[c] * r[c];
  for (int off = 16; off > 0; off >>= 1) acc += __shfl_down_sync(0xffffffffu, acc, off);
  float inv = 0.0f;
  if (lane == 0) inv = rsqrtf(acc / (float)cols + eps);
  inv = __shfl_sync(0xffffffffu, inv, 0);
  for (std::int64_t c = lane; c < cols; c += 32) r[c] = (w ? r[c] * w[c] : r[c]) * inv;
}

// ================================ activations / glue ================================
__global__ void silu_kernel(float* __restrict__ x, std::int64_t n) {  // elementwise.cu:71
  const std::int64_t i = (std::int64_t)blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const double v = (double)x[i];  // double then cast: matches the reference oracle's last bits
  x[i] = (float)(v / (1.0 + exp(-v)));
}
__device__ __forceinline__ float gelu_tanh(float x) {
  // ggml's tanh gelu: 0.5 x (1 + tanh(sqrt(2/pi)(x + 0.044715 x^3)))
  const float c = 0.7978845608028654f;  // sqrt(2/pi)
  return 0.5f * x * (1.0f + tanhf(c * (x + 0.044715f * x * x * x)));
}
__global__ void gelu_kernel(float* __restrict__ x, std::int64_t n) {
  const std::int64_t i = (std::int64_t)blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) x[i] = gelu_tanh(x[i]);
}
__global__ void swiglu_kernel(const float* __restrict__ gate, const float* __restrict__ up,
                              float* __restrict__ out, std::int64_t n) {
  const std::int64_t i = (std::int64_t)blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const double v = (double)gate[i];
  out[i] = (float)(v / (1.0 + exp(-v))) * up[i];
}
// glm_ffn.cu:13-22: gate clamped ABOVE only, up on both sides, precise expf.
__global__ void swiglu_clamp_kernel(const float* __restrict__ gate, const float* __restrict__ up,
                                    float* __restrict__ out, std::int64_t n, float limit) {
  const std::int64_t i = (std::int64_t)blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const float g = fminf(gate[i], limit);
  const float uu = fminf(fmaxf(up[i], -limit), limit);
  out[i] = (g / (1.0f + expf(-g))) * uu;
}
__global__ void geglu_kernel(const float* __restrict__ gate, const float* __restrict__ up,
                             float* __restrict__ out, std::int64_t n) {
  const std::int64_t i = (std::int64_t)blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) out[i] = gelu_tanh(gate[i]) * up[i];
}
__global__ void scale_kernel(float* __restrict__ x, std::int64_t n, float s) {
  const std::int64_t i = (std::int64_t)blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) x[i] *= s;
}
__global__ void add_kernel(float* __restrict__ dst, const float* __restrict__ src, std::int64_t n) {
  const std::int64_t i = (std::int64_t)blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) dst[i] += src[i];
}
__global__ void to_bf16_kernel(const float* __restrict__ x, uint16_t* __restrict__ y, std::int64_t n) {
  const std::int64_t i = (std::int64_t)blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) y[i] = bf16_from_f32(x[i]);
}
__global__ void to_f16_kernel(const float* __restrict__ x, uint16_t* __restrict__ y, std::int64_t n) {
  const std::int64_t i = (std::int64_t)blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) y[i] = __half_as_ushort(__float2half(x[i]));
}

// ================================ RoPE ================================
// rope.cu:56-72 (table, NEOX partial). Image/mrope paths dropped (text-only).
__global__ void rope_table_kernel(const float* __restrict__ x, float* __restrict__ out, long long rows,
                                  int head_dim, int n_rot, const float* __restrict__ cos_tab,
                                  const float* __restrict__ sin_tab, const int* __restrict__ pos) {
  const long long r = (long long)blockIdx.x * blockDim.x + threadIdx.x;
  if (r >= rows) return;
  const int half = n_rot / 2;
  const float* xr = x + r * head_dim;
  float* orow = out + r * head_dim;
  for (int d = n_rot; d < head_dim; ++d) orow[d] = xr[d];  // the partial rotation's untouched tail
  const std::size_t toff = (std::size_t)pos[r] * half;
  for (int i = 0; i < half; ++i) {
    const float c = cos_tab[toff + i], s = sin_tab[toff + i];
    const float a = xr[i], b = xr[half + i];
    orow[i] = a * c - b * s;
    orow[half + i] = a * s + b * c;
  }
}
// native_rope.cu:41-59: device-derived theta from freq_base, NEOX pairs.
__global__ void rope_base_kernel(const float* __restrict__ x, float* __restrict__ out, int rows,
                                 int head_dim, int n_rot, float theta_scale, const int* __restrict__ pos) {
  const int row = blockIdx.y;
  const int pair = blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= rows || pair >= head_dim / 2) return;
  const std::size_t start = (std::size_t)row * head_dim;
  const int half = n_rot / 2;
  if (pair >= half) {  // the untouched tail of the partial rotation
    if (x != out) {
      out[start + 2 * pair] = x[start + 2 * pair];
      out[start + 2 * pair + 1] = x[start + 2 * pair + 1];
    }
    return;
  }
  const float theta = (float)pos[row] * powf(theta_scale, (float)pair);
  const float c = cosf(theta), s = sinf(theta);
  const float a = x[start + pair], b = x[start + pair + half];
  out[start + pair] = a * c - b * s;
  out[start + pair + half] = a * s + b * c;
}

// ================================ softmax ================================
__global__ void softmax_kernel(float* __restrict__ x, std::int64_t rows, std::int64_t cols) {
  const std::int64_t row = blockIdx.x;
  if (row >= rows) return;
  float* r = x + row * cols;
  const int tid = threadIdx.x, nt = blockDim.x;
  __shared__ float red[32];
  float mx = -FLT_MAX;
  for (std::int64_t c = tid; c < cols; c += nt) mx = fmaxf(mx, r[c]);
  mx = warp_max(mx);
  if ((tid & 31) == 0) red[tid >> 5] = mx;
  __syncthreads();
  if (tid < 32) {
    const int nw = (nt + 31) >> 5;
    float v = tid < nw ? red[tid] : -FLT_MAX;
    v = warp_max(v);
    if (tid == 0) red[0] = v;
  }
  __syncthreads();
  mx = red[0];
  float sum = 0.0f;
  for (std::int64_t c = tid; c < cols; c += nt) {
    const float e = __expf(r[c] - mx);
    r[c] = e;
    sum += e;
  }
  sum = warp_sum(sum);
  if ((tid & 31) == 0) red[tid >> 5] = sum;
  __syncthreads();
  if (tid < 32) {
    const int nw = (nt + 31) >> 5;
    float v = tid < nw ? red[tid] : 0.0f;
    v = warp_sum(v);
    if (tid == 0) red[0] = v;
  }
  __syncthreads();
  const float inv = 1.0f / red[0];
  for (std::int64_t c = tid; c < cols; c += nt) r[c] *= inv;
}

// ================================ attention: online-softmax split-K + merge ================================
//
// Generic flash-decode, online softmax (the rescale exp(m_i - m) pattern from
// native_flash_attn.cu:107-110). One block per (head, split); blockDim = head_dim
// (<= 1024). Thread d owns accumulator dim d; the per-key q.k dot is a block
// reduction over the slice of the context. Partial layout per (head, split):
// [m, l, acc[0..head_dim)]. `attn_merge` combines splits. The family MLA/KDA/DSA
// attentions keep their own specialized paths; this is the fallback.
__device__ __forceinline__ void attn_split_body(const float* __restrict__ q, const float* __restrict__ kk,
                                                const float* __restrict__ vv, const float* __restrict__ mask,
                                                float* __restrict__ partials, int heads, int kv_heads,
                                                int head_dim, int ctx, float scale, int splits) {
  // sh: head_dim floats for the scaled query, then one float per warp for the
  // per-key dot reduction (blockDim is a multiple of 32, up to 1024 => <=32 warps).
  extern __shared__ float sh[];
  float* sq = sh;
  float* swarp = sh + head_dim;
  const int head = blockIdx.x;
  const int split = blockIdx.y;
  const int d = threadIdx.x;  // 0..blockDim-1; dims >= head_dim contribute 0
  const int nt = blockDim.x;
  const int nwarp = nt >> 5;
  const int lane = d & 31, warp = d >> 5;
  const int kvh = head / (heads / kv_heads);
  const float* qh = q + (std::size_t)head * head_dim;
  if (d < head_dim) sq[d] = qh[d] * scale;
  __syncthreads();

  const int slice = (ctx + splits - 1) / splits;
  const int t0 = split * slice;
  const int t1 = min(ctx, t0 + slice);

  float m = -FLT_MAX / 2.0f, l = 0.0f, acc = 0.0f;
  for (int t = t0; t < t1; ++t) {
    const float* kt = kk + ((std::size_t)t * kv_heads + kvh) * head_dim;
    float part = (d < head_dim) ? sq[d] * kt[d] : 0.0f;
    part = warp_sum(part);                     // reduce within each warp
    if (lane == 0) swarp[warp] = part;
    __syncthreads();
    float score = 0.0f;
    for (int w = 0; w < nwarp; ++w) score += swarp[w];  // sum warp partials (nwarp <= 32)
    if (mask) score += mask[t];
    const float m_new = fmaxf(m, score);
    const float rescale = __expf(m - m_new);
    const float p = __expf(score - m_new);
    l = l * rescale + p;
    const float vt = (d < head_dim) ? vv[((std::size_t)t * kv_heads + kvh) * head_dim + d] : 0.0f;
    acc = acc * rescale + p * vt;
    m = m_new;
    __syncthreads();
  }
  if (d < head_dim) {
    float* out = partials + ((std::size_t)head * splits + split) * (head_dim + 2);
    if (d == 0) {
      out[0] = m;
      out[1] = l;
    }
    out[2 + d] = acc;
  }
}
__global__ void attn_split_kernel(const float* __restrict__ q, const float* __restrict__ kk,
                                  const float* __restrict__ vv, const float* __restrict__ mask,
                                  float* __restrict__ partials, int heads, int kv_heads, int head_dim,
                                  int ctx, float scale, int splits) {
  attn_split_body(q, kk, vv, mask, partials, heads, kv_heads, head_dim, ctx, scale, splits);
}
// The same, with the context length read from the device (ctx = d_pos[0] + 1): the launch arguments
// no longer change per token, so a decode step can be captured once into a CUDA graph and replayed.
__global__ void attn_split_devlen_kernel(const float* __restrict__ q, const float* __restrict__ kk,
                                         const float* __restrict__ vv, float* __restrict__ partials,
                                         int heads, int kv_heads, int head_dim, const int* __restrict__ d_pos,
                                         float scale) {
  const int ctx = max(1, d_pos[0] + 1);
  attn_split_body(q, kk, vv, nullptr, partials, heads, kv_heads, head_dim, ctx, scale, 1);
}
__global__ void attn_merge_kernel(const float* __restrict__ partials, float* __restrict__ out, int heads,
                                  int head_dim, int splits) {
  const int head = blockIdx.x;
  const int d = threadIdx.x;
  if (head >= heads || d >= head_dim) return;
  const float* base = partials + (std::size_t)head * splits * (head_dim + 2);
  float gm = -FLT_MAX / 2.0f;
  for (int s = 0; s < splits; ++s) gm = fmaxf(gm, base[(std::size_t)s * (head_dim + 2)]);
  float gl = 0.0f, acc = 0.0f;
  for (int s = 0; s < splits; ++s) {
    const float* p = base + (std::size_t)s * (head_dim + 2);
    const float w = __expf(p[0] - gm);
    gl += w * p[1];
    acc += w * p[2 + d];
  }
  out[(std::size_t)head * head_dim + d] = acc / gl;
}

// ================================ MoE routers ================================
// router_top10.cu:69-187 (softmax top-k, tree max + parallel exp + k argmax rounds).
__global__ void router_topk_kernel(const float* __restrict__ logits, int n_tokens, int n_expert, int k,
                                   int* __restrict__ ids, float* __restrict__ weights) {
  extern __shared__ unsigned char s_raw[];
  const std::size_t taken_bytes = ((std::size_t)n_expert + 15u) & ~(std::size_t)15u;
  unsigned char* s_taken = s_raw;
  double* s_ex = (double*)(s_raw + taken_bytes);
  float* s_p = (float*)(s_ex + n_expert);
  __shared__ float s_red[512 / 32];
  __shared__ int s_rid[512 / 32];
  __shared__ double s_sum;
  const int t = blockIdx.x;
  if (t >= n_tokens) return;
  const int tid = threadIdx.x, nt = blockDim.x;
  const float* l = logits + (std::size_t)t * n_expert;

  float mx = -CUDART_INF_F;
  for (int e = tid; e < n_expert; e += nt) mx = fmaxf(mx, l[e]);
  for (int off = 16; off > 0; off >>= 1) mx = fmaxf(mx, __shfl_down_sync(0xffffffffu, mx, off));
  if ((tid & 31) == 0) s_red[tid >> 5] = mx;
  __syncthreads();
  if (tid < 32) {
    const int nw = (nt + 31) >> 5;
    float v = tid < nw ? s_red[tid] : -CUDART_INF_F;
    for (int off = 16; off > 0; off >>= 1) v = fmaxf(v, __shfl_down_sync(0xffffffffu, v, off));
    if (tid == 0) s_red[0] = v;
  }
  __syncthreads();
  mx = s_red[0];
  for (int e = tid; e < n_expert; e += nt) s_ex[e] = exp((double)l[e] - (double)mx);
  __syncthreads();
  if (tid == 0) {
    double sum = 0.0;
    for (int e = 0; e < n_expert; ++e) sum += s_ex[e];
    s_sum = sum;
  }
  __syncthreads();
  const float inv = (float)(1.0 / s_sum);
  for (int e = tid; e < n_expert; e += nt) s_p[e] = (float)(s_ex[e] * inv);
  for (int e = tid; e < n_expert; e += nt) s_taken[e] = 0;
  __syncthreads();
  for (int i = 0; i < k; ++i) {
    float bv = -CUDART_INF_F;
    int bi = n_expert;
    for (int e = tid; e < n_expert; e += nt) {
      if (s_taken[e]) continue;
      const float pe = s_p[e];
      if (pe > bv) { bv = pe; bi = e; }
    }
    for (int off = 16; off > 0; off >>= 1) {
      const float ov = __shfl_down_sync(0xffffffffu, bv, off);
      const int oi = __shfl_down_sync(0xffffffffu, bi, off);
      if (ov > bv || (ov == bv && oi < bi)) { bv = ov; bi = oi; }
    }
    if ((tid & 31) == 0) { s_red[tid >> 5] = bv; s_rid[tid >> 5] = bi; }
    __syncthreads();
    if (tid < 32) {
      const int nw = (nt + 31) >> 5;
      float v = tid < nw ? s_red[tid] : -CUDART_INF_F;
      int ix = tid < nw ? s_rid[tid] : n_expert;
      for (int off = 16; off > 0; off >>= 1) {
        const float ov = __shfl_down_sync(0xffffffffu, v, off);
        const int oi = __shfl_down_sync(0xffffffffu, ix, off);
        if (ov > v || (ov == v && oi < ix)) { v = ov; ix = oi; }
      }
      if (tid == 0 && ix < n_expert) {
        ids[(std::size_t)t * k + i] = ix;
        weights[(std::size_t)t * k + i] = v;
        s_taken[ix] = 1;
      }
    }
    __syncthreads();
  }
  __syncthreads();
  if (tid == 0) {  // renormalise with ggml's lower clamp 2^-14
    double s = 0.0;
    for (int i = 0; i < k; ++i) s += (double)weights[(std::size_t)t * k + i];
    const double sc = fmax(s, 6.103515625e-05);
    for (int i = 0; i < k; ++i)
      weights[(std::size_t)t * k + i] = (float)((double)weights[(std::size_t)t * k + i] / sc);
  }
}

// router_sigmoid.cu:29-74.
__global__ void router_sigmoid_kernel(const float* __restrict__ logits, const float* __restrict__ bias,
                                      int n_tokens, int n_expert, int k, float w_scale, int norm_w,
                                      int* __restrict__ ids, float* __restrict__ weights) {
  extern __shared__ float s_mem[];
  float* s_p = s_mem;
  float* s_sel = s_mem + n_expert;
  const int t = blockIdx.x;
  if (t >= n_tokens) return;
  const int tid = threadIdx.x, nt = blockDim.x;
  const float* l = logits + (std::size_t)t * n_expert;
  for (int e = tid; e < n_expert; e += nt) {
    const float p = 1.0f / (1.0f + __expf(-l[e]));
    s_p[e] = p;
    s_sel[e] = bias ? p + bias[e] : p;
  }
  __syncthreads();
  for (int e = tid; e < n_expert; e += nt) {
    const float v = s_sel[e];
    int rank = 0;
    for (int f = 0; f < n_expert; ++f) {
      const float u = s_sel[f];
      rank += u > v ? 1 : 0;
      rank += (f < e && u == v) ? 1 : 0;
    }
    if (rank < k) ids[(std::size_t)t * k + rank] = e;
  }
  __syncthreads();
  if (tid == 0) {
    double sum = 0.0;
    for (int i = 0; i < k; ++i) sum += (double)s_p[ids[(std::size_t)t * k + i]];
    const float inv = (float)(norm_w ? 1.0 / fmax(sum, 6.103515625e-5) : 1.0);
    for (int i = 0; i < k; ++i)
      weights[(std::size_t)t * k + i] = (float)((double)s_p[ids[(std::size_t)t * k + i]] * inv * w_scale);
  }
}

// ================================ MoE combine ================================
// native_moe.cu:34-48.
__global__ void moe_combine_kernel(const float* __restrict__ parts, const float* __restrict__ weights,
                                   const float* __restrict__ shared, float* __restrict__ output,
                                   std::int64_t n_embd, int k) {
  const std::int64_t tk = blockIdx.y;
  parts += tk * k * n_embd;
  weights += tk * k;
  if (shared) shared += tk * n_embd;
  output += tk * n_embd;
  const std::int64_t col = (std::int64_t)blockIdx.x * blockDim.x + threadIdx.x;
  if (col >= n_embd) return;
  float sum = parts[col] * weights[0];
  for (int e = 1; e < k; ++e) sum += parts[(std::int64_t)e * n_embd + col] * weights[e];
  if (shared) sum += shared[col];
  output[col] = sum;
}

// ================================ sampler ================================
// Philox 4x32-10 + penalties + greedy + the top-k/top-p/min-p/temperature chain.
// sampler.cu:30-68 (philox/penalties), :91-157 (greedy). The sampled selection is
// the k argmax rounds the radix-select replaced; its ORDER (descending penalised
// logit, ties to the lower index) and the top_p/min_p/temperature/draw chain are
// the serial reference exactly (sampler.cu:401-447). The radix-select fast path is
// left for the box's perf pass.
__device__ __forceinline__ void philox_round(uint32_t& c0, uint32_t& c1, uint32_t& c2, uint32_t& c3,
                                             uint32_t key0) {
  const uint32_t hi0 = __umulhi(0x9E3779B9u, c0);
  const uint32_t hi1 = __umulhi(0xBB67AE85u, c2);
  const uint32_t lo0 = 0x9E3779B9u * c0;
  const uint32_t lo1 = 0xBB67AE85u * c2;
  const uint32_t n0 = hi1 ^ c1 ^ key0;
  const uint32_t n1 = lo1;
  const uint32_t n2 = hi0 ^ c3;
  const uint32_t n3 = lo0;
  c0 = n0; c1 = n1; c2 = n2; c3 = n3;
}
__device__ __forceinline__ float philox_uniform(uint64_t seed, uint64_t counter) {
  uint32_t c0 = (uint32_t)counter, c1 = (uint32_t)(counter >> 32);
  uint32_t c2 = (uint32_t)seed, c3 = (uint32_t)(seed >> 32);
  for (int i = 0; i < 10; ++i) philox_round(c0, c1, c2, c3, (uint32_t)i);
  return (float)(c0 >> 8) * (1.0f / 16777216.0f);
}
__device__ __forceinline__ int history_count(const int* __restrict__ h, int n, int v) {
  int c = 0;
  for (int i = 0; i < n; ++i) c += (h[i] == v);
  return c;
}
__device__ __forceinline__ float apply_penalties(float logit, int count, float repeat, float freq,
                                                 float present) {
  if (count <= 0) return logit;
  if (logit <= 0.0f) logit *= repeat;
  else logit /= repeat;
  logit -= (float)count * freq + present;
  return logit;
}

__global__ void sampler_greedy_kernel(const float* __restrict__ logits, int n_vocab,
                                      const int* __restrict__ history, int history_len,
                                      SamplerParams p, int* __restrict__ out) {
  const int t = blockIdx.x;
  const float* l = logits + (std::size_t)t * n_vocab;
  const int* hrow = history ? history + (std::size_t)t * history_len : nullptr;
  int hlen = 0;
  if (hrow) {
    hlen = p.penalty_last_n < history_len ? p.penalty_last_n : history_len;
    if (hlen < 0) hlen = 0;
    hrow += history_len - hlen;
  }
  extern __shared__ unsigned int penal_bits[];
  const int bits_words = (n_vocab + 31) / 32;
  const bool use_bits = hrow != nullptr && hlen > 0 && bits_words > 0;
  if (use_bits) {
    for (int w = threadIdx.x; w < bits_words; w += blockDim.x) penal_bits[w] = 0u;
    __syncthreads();
    for (int i = threadIdx.x; i < hlen; i += blockDim.x)
      if (hrow[i] >= 0 && hrow[i] < n_vocab) atomicOr(&penal_bits[hrow[i] >> 5], 1u << (hrow[i] & 31));
    __syncthreads();
  }
  auto hit = [&](int v) -> int {
    if (!use_bits || !(penal_bits[v >> 5] & (1u << (v & 31)))) return 0;
    return history_count(hrow, hlen, v);
  };
  float bv = -CUDART_INF_F;
  int best = n_vocab;
  for (int v = threadIdx.x; v < n_vocab; v += blockDim.x) {
    const float s = apply_penalties(p.logit_bias ? l[v] + p.logit_bias[v] : l[v], hit(v),
                                    p.penalty_repeat, p.penalty_freq, p.penalty_present);
    if (s > bv) { bv = s; best = v; }
  }
  for (int off = 16; off > 0; off >>= 1) {
    const float ov = __shfl_down_sync(0xffffffffu, bv, off);
    const int oi = __shfl_down_sync(0xffffffffu, best, off);
    if (ov > bv || (ov == bv && oi < best)) { bv = ov; best = oi; }
  }
  __shared__ float sv[32];
  __shared__ int si[32];
  const int warp = threadIdx.x >> 5, lane = threadIdx.x & 31;
  if (lane == 0) { sv[warp] = bv; si[warp] = best; }
  __syncthreads();
  if (warp == 0) {
    const int nw = (blockDim.x + 31) >> 5;
    float wv = lane < nw ? sv[lane] : -CUDART_INF_F;
    int wi = lane < nw ? si[lane] : n_vocab;
    for (int off = 16; off > 0; off >>= 1) {
      const float ov = __shfl_down_sync(0xffffffffu, wv, off);
      const int oi = __shfl_down_sync(0xffffffffu, wi, off);
      if (ov > wv || (ov == wv && oi < wi)) { wv = ov; wi = oi; }
    }
    if (lane == 0) out[t] = wi < n_vocab ? wi : 0;
  }
}

// The sampled chain. Selection = k stable-descending argmax rounds using a
// composite threshold (value, index) strictly below the previous pick -- no
// taken-bitmap, O(k * n_vocab). Then top_p / min_p / temperature / Philox draw on
// thread 0 (double precision), in llama.cpp's order.
constexpr int KMAX = 64;
__global__ void __launch_bounds__(1024)
    sampler_kernel(const float* __restrict__ logits, int n_vocab, int n_tokens,
                   const int* __restrict__ history, int history_len, SamplerParams p,
                   int* __restrict__ out) {
  const int t = blockIdx.x;
  if (t >= n_tokens) return;
  const float* l = logits + (std::size_t)t * n_vocab;
  const int* hrow = history ? history + (std::size_t)t * history_len : nullptr;
  int hlen = 0;
  if (hrow) {
    hlen = p.penalty_last_n < history_len ? p.penalty_last_n : history_len;
    if (hlen < 0) hlen = 0;
    hrow += history_len - hlen;
  }
  extern __shared__ unsigned int penal_bits[];
  const int bits_words = (n_vocab + 31) / 32;
  const bool use_bits = hrow != nullptr && hlen > 0 && bits_words > 0;
  if (use_bits) {
    for (int w = threadIdx.x; w < bits_words; w += blockDim.x) penal_bits[w] = 0u;
    __syncthreads();
    for (int i = threadIdx.x; i < hlen; i += blockDim.x)
      if (hrow[i] >= 0 && hrow[i] < n_vocab) atomicOr(&penal_bits[hrow[i] >> 5], 1u << (hrow[i] & 31));
    __syncthreads();
  }
  auto hit = [&](int v) -> int {
    if (!use_bits || !(penal_bits[v >> 5] & (1u << (v & 31)))) return 0;
    return history_count(hrow, hlen, v);
  };
  auto score = [&](int v) -> float {
    return apply_penalties(p.logit_bias ? l[v] + p.logit_bias[v] : l[v], hit(v), p.penalty_repeat,
                           p.penalty_freq, p.penalty_present);
  };
  int k = (p.top_k > 0 && p.top_k < KMAX) ? p.top_k : KMAX;
  if (k > n_vocab) k = n_vocab;

  __shared__ int sel_ids[KMAX];
  __shared__ float sel_logit[KMAX];
  __shared__ float s_red[1024 / 32];
  __shared__ int s_rid[1024 / 32];
  __shared__ float sh_prev_v;
  __shared__ int sh_prev_i;
  const int tid = threadIdx.x, nt = blockDim.x;
  if (tid == 0) { sh_prev_v = CUDART_INF_F; sh_prev_i = -2147483647 - 1; }
  __syncthreads();

  for (int round = 0; round < k; ++round) {
    const float pv = sh_prev_v;
    const int pi = sh_prev_i;
    // best valid candidate: (score < pv) or (score == pv and index > pi); pick the
    // max by (score, -index).
    float bv = -CUDART_INF_F;
    int bi = n_vocab;
    for (int v = tid; v < n_vocab; v += nt) {
      const float s = score(v);
      const bool valid = (s < pv) || (s == pv && v > pi);
      if (valid && (s > bv || (s == bv && v < bi))) { bv = s; bi = v; }
    }
    for (int off = 16; off > 0; off >>= 1) {
      const float ov = __shfl_down_sync(0xffffffffu, bv, off);
      const int oi = __shfl_down_sync(0xffffffffu, bi, off);
      if (ov > bv || (ov == bv && oi < bi)) { bv = ov; bi = oi; }
    }
    if ((tid & 31) == 0) { s_red[tid >> 5] = bv; s_rid[tid >> 5] = bi; }
    __syncthreads();
    if (tid < 32) {
      const int nw = (nt + 31) >> 5;
      float v = tid < nw ? s_red[tid] : -CUDART_INF_F;
      int ix = tid < nw ? s_rid[tid] : n_vocab;
      for (int off = 16; off > 0; off >>= 1) {
        const float ov = __shfl_down_sync(0xffffffffu, v, off);
        const int oi = __shfl_down_sync(0xffffffffu, ix, off);
        if (ov > v || (ov == v && oi < ix)) { v = ov; ix = oi; }
      }
      if (tid == 0) {
        sel_ids[round] = ix < n_vocab ? ix : 0;
        sel_logit[round] = ix < n_vocab ? v : -CUDART_INF_F;
        sh_prev_v = v;
        sh_prev_i = ix;
      }
    }
    __syncthreads();
  }

  if (tid != 0) return;
  const float inv_t = p.temperature > 0.0f ? 1.0f / p.temperature : 0.0f;
  int n_keep = k;
  float mx = sel_logit[0];
  for (int i = 1; i < k; ++i) mx = fmaxf(mx, sel_logit[i]);
  if (p.top_p < 1.0f) {
    double sum = 0.0;
    for (int i = 0; i < k; ++i) sum += exp((double)sel_logit[i] - (double)mx);
    double cum = 0.0;
    int cut = k;
    for (int i = 0; i < k; ++i) {
      cum += exp((double)sel_logit[i] - (double)mx) / sum;
      if (cum >= (double)p.top_p) { cut = i + 1; break; }
    }
    if (cut < p.min_keep) cut = p.min_keep < k ? p.min_keep : k;
    n_keep = cut;
  }
  if (p.min_p > 0.0f) {
    const float thresh = sel_logit[0] + logf(p.min_p);
    for (int i = 0; i < n_keep; ++i)
      if (sel_logit[i] < thresh) { n_keep = i; break; }
  }
  if (n_keep < 1) n_keep = 1;
  auto scaled = [&](int i) { return sel_logit[i] * inv_t; };
  float smx = scaled(0);
  for (int i = 1; i < n_keep; ++i) smx = fmaxf(smx, scaled(i));
  double sum = 0.0;
  for (int i = 0; i < n_keep; ++i) sum += exp((double)scaled(i) - (double)smx);
  const float u = philox_uniform(p.seed, p.counter + (uint64_t)t);
  double cum = 0.0;
  int pick = sel_ids[n_keep - 1];
  for (int i = 0; i < n_keep; ++i) {
    cum += exp((double)scaled(i) - (double)smx) / sum;
    if ((double)u < cum) { pick = sel_ids[i]; break; }
  }
  out[t] = pick;
}

}  // namespace

// ================================ public API ================================

bool is_quantized(Quant q) noexcept {
  return q == Quant::q8_0 || q == Quant::q4_k || q == Quant::q5_k || q == Quant::q6_k ||
         q == Quant::iq2_xxs || q == Quant::iq3_xxs || q == Quant::iq2_s ||
         q == Quant::iq4_nl || q == Quant::iq3_s || q == Quant::iq4_xs || q == Quant::q2_0 ||
         q == Quant::nvfp4;
}
int block_elems(Quant q) noexcept {
  switch (q) {
    case Quant::q8_0:
    case Quant::iq4_nl: return 32;
    case Quant::q2_0: return 64;
    case Quant::nvfp4: return 64;   // flint native: 64-elem blocks (4 UE4M3 sub-scales + 32 nibble bytes)
    case Quant::q4_k:
    case Quant::q5_k:
    case Quant::q6_k:
    case Quant::iq2_xxs:
    case Quant::iq3_xxs:
    case Quant::iq2_s:
    case Quant::iq3_s:
    case Quant::iq4_xs: return 256;
    default: return 1;  // f32 / bf16
  }
}
std::size_t weight_bytes(Quant q, int n_in, int n_out) {
  if (n_in <= 0 || n_out <= 0) throw std::invalid_argument("u::k::weight_bytes: n_in,n_out > 0");
  std::size_t block_bytes = 0, elems = (std::size_t)block_elems(q);
  switch (q) {
    case Quant::f32: return (std::size_t)n_in * n_out * sizeof(float);
    case Quant::bf16: return (std::size_t)n_in * n_out * sizeof(uint16_t);
    case Quant::q8_0: block_bytes = 34; break;
    case Quant::q4_k: block_bytes = 144; break;
    case Quant::q5_k: block_bytes = 176; break;
    case Quant::q6_k: block_bytes = 210; break;
    case Quant::iq2_xxs: block_bytes = 66; break;
    case Quant::iq3_xxs: block_bytes = 98; break;
    case Quant::iq2_s: block_bytes = 82; break;
    case Quant::iq4_nl: block_bytes = 18; break;   // 32-elem block
    case Quant::q2_0: block_bytes = 18; break;      // 64-elem block
    case Quant::nvfp4: block_bytes = 36; break;     // 64-elem block: 4 UE4M3 sub-scales + 32 nibble bytes
    case Quant::iq3_s: block_bytes = 110; break;
    case Quant::iq4_xs: block_bytes = 136; break;
  }
  if ((std::size_t)n_in % elems != 0) throw std::invalid_argument("u::k::weight_bytes: n_in not a block multiple");
  return (std::size_t)(n_in / (int)elems) * block_bytes * (std::size_t)n_out;
}

std::size_t q8_1_scratch_bytes(int n_in, int ncols) {
  if (n_in <= 0 || n_in % Q8K != 0 || ncols < 1 || ncols > 8)
    throw std::invalid_argument("u::k::q8_1_scratch_bytes: n_in%32==0, 1<=ncols<=8");
  return (std::size_t)ncols * (n_in / Q8K) * sizeof(Q81Block);
}

void quantize_q8_1(const float* x, void* x_q8_1, int n_in, int ncols, void* stream) {
  if (n_in <= 0 || n_in % Q8K != 0 || ncols < 1) throw std::invalid_argument("u::k::quantize_q8_1: bad shape");
  const int n_total = n_in * ncols;  // columns contiguous; blocks stay inside a column
  quantize_q8_1_kernel<<<grid_for(n_total, QUANT_THREADS), QUANT_THREADS, 0, (cudaStream_t)stream>>>(
      x, static_cast<Q81Block*>(x_q8_1), n_total);
  launch_check("quantize_q8_1");
  sync_if_needed(stream, "quantize_q8_1");
}

void mmvq(Quant q, const void* weights, const void* x_q8_1, float* y, int n_in, int n_out, int ncols,
          void* stream) {
  if (!is_quantized(q)) throw std::invalid_argument("u::k::mmvq: not a quantized format");
  if (n_in <= 0 || n_in % block_elems(q) != 0 || n_out <= 0 || ncols < 1 || ncols > 8)
    throw std::invalid_argument("u::k::mmvq: bad shape");
  const auto s = (cudaStream_t)stream;
  const int x_stride = n_in / Q8K;  // Q8_1 blocks per activation column
  const auto* xq = static_cast<const Q81Block*>(x_q8_1);
  // ncols > 1: one fused launch reading each weight block once for every column, each column bitwise a
  // single-column call (see mmvq_multi_kernel / iq_mmvq_multi).
  if (ncols == 1) {
    launch_mmvq_col(q, weights, xq, y, n_in, n_out, s);
  } else {
    switch (q) {
      case Quant::q4_k: launch_mmvq_multi_tr<Q4KTr>(weights, xq, x_stride, y, n_in, n_out, ncols, s); break;
      case Quant::q5_k: launch_mmvq_multi_tr<Q5KTr>(weights, xq, x_stride, y, n_in, n_out, ncols, s); break;
      case Quant::q6_k: launch_mmvq_multi_tr<Q6KTr>(weights, xq, x_stride, y, n_in, n_out, ncols, s); break;
      case Quant::q8_0: launch_mmvq_multi_tr<Q80Tr>(weights, xq, x_stride, y, n_in, n_out, ncols, s); break;
      case Quant::iq2_xxs:
      case Quant::iq3_xxs:
      case Quant::iq2_s:
      case Quant::iq3_s:
      case Quant::iq4_xs:
      case Quant::iq4_nl:
      case Quant::q2_0: iq_mmvq_multi((int)q, weights, xq, x_stride, y, n_in, n_out, ncols, s); break;
      default: throw std::invalid_argument("u::k::mmvq: not a quantized format");
    }
  }
  launch_check("mmvq");
  sync_if_needed(stream, "mmvq");
}

void mmvq_f32(Quant q, const void* weights, const float* x, void* scratch_q8_1, float* y, int n_in,
              int n_out, int ncols, void* stream) {
  quantize_q8_1(x, scratch_q8_1, n_in, ncols, stream);
  mmvq(q, weights, scratch_q8_1, y, n_in, n_out, ncols, stream);
}

void bf16_gemv(const uint16_t* x, const uint16_t* w, float* y, std::int64_t n_in, std::int64_t n_out,
               void* stream) {
  if (n_in <= 0 || n_out <= 0) return;
  const int threads = 256;
  if (n_out >= 64) {  // warp-per-row (coalesced) once there are enough rows (bf16_gemv.cu:102)
    const int warps = threads / 32;
    bf16_gemv_warp_kernel<<<grid_for(n_out, warps), threads, 0, (cudaStream_t)stream>>>(x, w, y, n_in, n_out);
  } else {  // few rows: a whole block per row (see bf16_gemv_block_kernel)
    bf16_gemv_block_kernel<<<(unsigned)n_out, threads, 0, (cudaStream_t)stream>>>(x, w, y, n_in, n_out);
  }
  launch_check("bf16_gemv");
  sync_if_needed(stream, "bf16_gemv");
}

bool bf16_gemv_f32x(const float* x, const uint16_t* w, float* y, std::int64_t n_in, std::int64_t n_out, int ncols,
                    void* stream) {
  if (n_in <= 0 || n_out <= 0 || ncols < 1 || ncols > 8 || n_in % 8 != 0 ||
      (reinterpret_cast<std::uintptr_t>(x) & 15) != 0 || (reinterpret_cast<std::uintptr_t>(w) & 15) != 0)
    return false;
  const auto s = (cudaStream_t)stream;
  // The launch shape is a function of (n_in, n_out) only -- never of ncols -- so a column's arithmetic is the
  // same whatever the column count (the verify window's bitwise contract).
  const bool warp_rows = n_in <= 512;
  const int threads = warp_rows ? 256 : (n_out < 160 ? 1024 : 256);
  const unsigned grid = warp_rows ? (unsigned)grid_for(n_out, threads / 32) : (unsigned)n_out;
#define U_BF16V(NC)                                                                       \
  case NC:                                                                                \
    if (warp_rows) bf16v_warp_kernel<NC><<<grid, threads, 0, s>>>(x, w, y, n_in, n_out);  \
    else bf16v_block_kernel<NC><<<grid, threads, 0, s>>>(x, w, y, n_in, n_out);           \
    break;
  switch (ncols) {
    U_BF16V(1) U_BF16V(2) U_BF16V(3) U_BF16V(4) U_BF16V(5) U_BF16V(6) U_BF16V(7) U_BF16V(8)
    default: break;
  }
#undef U_BF16V
  launch_check("bf16_gemv_f32x");
  sync_if_needed(stream, "bf16_gemv_f32x");
  return true;
}

void f32_gemv(const float* x, const float* w, float* y, std::int64_t n_in, std::int64_t n_out, void* stream) {
  if (n_in <= 0 || n_out <= 0) return;
  const int threads = 256, warps = threads / 32;
  f32_gemv_warp_kernel<<<grid_for(n_out, warps), threads, 0, (cudaStream_t)stream>>>(x, w, y, n_in, n_out);
  launch_check("f32_gemv");
  sync_if_needed(stream, "f32_gemv");
}

void dequant_q6k(const void* w, int n, float* out, void* stream) {
  if (n <= 0 || n % 256 != 0) throw std::invalid_argument("u::k::dequant_q6k: n % 256 == 0");
  const int nblocks = n / 256;
  dequant_q6k_kernel<<<(unsigned)nblocks, 32, 0, (cudaStream_t)stream>>>(static_cast<const Q6KBlock*>(w), out,
                                                                         nblocks);
  launch_check("dequant_q6k");
  sync_if_needed(stream, "dequant_q6k");
}

namespace {
void cublas_or_die(cublasStatus_t st, const char* what) {
  if (st != CUBLAS_STATUS_SUCCESS) {
    std::fprintf(stderr, "u::k::%s: cuBLAS status %d\n", what, (int)st);
    std::exit(1);
  }
}
}  // namespace

void gemm_prefill_f32(void* cublas_handle, const float* a, const float* b, float* c, int m, int n,
                      int kdim, void* stream) {
  // Row-major C[m,n] = A[m,k] B[k,n] via column-major cublasSgemm: compute
  // C^T = B^T A^T, i.e. sgemm(N,N, n,m,k, B, n, A, k, C, n).
  auto h = (cublasHandle_t)cublas_handle;
  cublas_or_die(cublasSetStream(h, (cudaStream_t)stream), "gemm_prefill_f32:setstream");
  const float alpha = 1.0f, beta = 0.0f;
  cublas_or_die(cublasSgemm(h, CUBLAS_OP_N, CUBLAS_OP_N, n, m, kdim, &alpha, b, n, a, kdim, &beta, c, n),
                "gemm_prefill_f32");
  sync_if_needed(stream, "gemm_prefill_f32");
}

void gemm_prefill_bf16(void* cublas_handle, const uint16_t* a, const uint16_t* b, float* c, int m, int n,
                       int kdim, void* stream) {
  auto h = (cublasHandle_t)cublas_handle;
  cublas_or_die(cublasSetStream(h, (cudaStream_t)stream), "gemm_prefill_bf16:setstream");
  const float alpha = 1.0f, beta = 0.0f;
  // Same transpose trick, BF16 inputs with an F32 accumulator.
  cublas_or_die(cublasGemmEx(h, CUBLAS_OP_N, CUBLAS_OP_N, n, m, kdim, &alpha, b, CUDA_R_16BF, n, a,
                             CUDA_R_16BF, kdim, &beta, c, CUDA_R_32F, n, CUBLAS_COMPUTE_32F,
                             CUBLAS_GEMM_DEFAULT),
                "gemm_prefill_bf16");
  sync_if_needed(stream, "gemm_prefill_bf16");
}

void rms_norm(float* x, const float* w, std::int64_t rows, std::int64_t cols, float eps, void* stream) {
  if (rows <= 0 || cols <= 0) return;
  const unsigned wpb = 4;
  rms_norm_kernel<<<(unsigned)((rows + wpb - 1) / wpb), wpb * 32, 0, (cudaStream_t)stream>>>(x, w, rows, cols, eps);
  launch_check("rms_norm");
  sync_if_needed(stream, "rms_norm");
}

void silu_inplace(float* x, std::int64_t n, void* stream) {
  if (n <= 0) return;
  silu_kernel<<<grid_for(n, 256), 256, 0, (cudaStream_t)stream>>>(x, n);
  launch_check("silu_inplace");
  sync_if_needed(stream, "silu_inplace");
}
void gelu_inplace(float* x, std::int64_t n, void* stream) {
  if (n <= 0) return;
  gelu_kernel<<<grid_for(n, 256), 256, 0, (cudaStream_t)stream>>>(x, n);
  launch_check("gelu_inplace");
  sync_if_needed(stream, "gelu_inplace");
}
void swiglu(const float* gate, const float* up, float* out, std::int64_t n, void* stream) {
  if (n <= 0) return;
  swiglu_kernel<<<grid_for(n, 256), 256, 0, (cudaStream_t)stream>>>(gate, up, out, n);
  launch_check("swiglu");
  sync_if_needed(stream, "swiglu");
}
void swiglu_clamped(const float* gate, const float* up, float* out, std::int64_t n, float limit, void* stream) {
  if (n <= 0) return;
  swiglu_clamp_kernel<<<grid_for(n, 256), 256, 0, (cudaStream_t)stream>>>(gate, up, out, n, limit);
  launch_check("swiglu_clamped");
  sync_if_needed(stream, "swiglu_clamped");
}
void geglu(const float* gate, const float* up, float* out, std::int64_t n, void* stream) {
  if (n <= 0) return;
  geglu_kernel<<<grid_for(n, 256), 256, 0, (cudaStream_t)stream>>>(gate, up, out, n);
  launch_check("geglu");
  sync_if_needed(stream, "geglu");
}
void scale_inplace(float* x, std::int64_t n, float s, void* stream) {
  if (n <= 0) return;
  scale_kernel<<<grid_for(n, 256), 256, 0, (cudaStream_t)stream>>>(x, n, s);
  launch_check("scale_inplace");
  sync_if_needed(stream, "scale_inplace");
}
void add_inplace(float* dst, const float* src, std::int64_t n, void* stream) {
  if (n <= 0) return;
  add_kernel<<<grid_for(n, 256), 256, 0, (cudaStream_t)stream>>>(dst, src, n);
  launch_check("add_inplace");
  sync_if_needed(stream, "add_inplace");
}
void f32_to_bf16(const float* x, uint16_t* y, std::int64_t n, void* stream) {
  if (n <= 0) return;
  to_bf16_kernel<<<grid_for(n, 256), 256, 0, (cudaStream_t)stream>>>(x, y, n);
  launch_check("f32_to_bf16");
  sync_if_needed(stream, "f32_to_bf16");
}
void f32_to_f16(const float* x, uint16_t* y, std::int64_t n, void* stream) {
  if (n <= 0) return;
  to_f16_kernel<<<grid_for(n, 256), 256, 0, (cudaStream_t)stream>>>(x, y, n);
  launch_check("f32_to_f16");
  sync_if_needed(stream, "f32_to_f16");
}

void build_rope_table(int n_rot, double theta, int max_pos, float* cos_tab, float* sin_tab) {  // HOST
  const int half = n_rot / 2;
  for (int p = 0; p < max_pos; ++p)
    for (int i = 0; i < half; ++i) {
      const double inv = std::pow(theta, -2.0 * (double)i / (double)n_rot);
      const double ang = (double)p * inv;
      cos_tab[(std::size_t)p * half + i] = (float)std::cos(ang);
      sin_tab[(std::size_t)p * half + i] = (float)std::sin(ang);
    }
}
void rope_neox_table(const float* x, float* out, std::int64_t rows, int head_dim, int n_rot,
                     const float* cos_tab, const float* sin_tab, const int* pos, void* stream) {
  if (rows <= 0 || n_rot <= 0) return;
  if (n_rot % 2 != 0 || n_rot > head_dim) throw std::invalid_argument("u::k::rope_neox_table: n_rot even and <= head_dim");
  rope_table_kernel<<<grid_for(rows, 128), 128, 0, (cudaStream_t)stream>>>(x, out, rows, head_dim, n_rot, cos_tab,
                                                                           sin_tab, pos);
  launch_check("rope_neox_table");
  sync_if_needed(stream, "rope_neox_table");
}
void rope_neox_base(const float* x, float* out, std::int64_t rows, int head_dim, int n_rot,
                    float freq_base, const int* pos, void* stream) {
  if (rows <= 0 || n_rot <= 0) return;
  if (n_rot % 2 != 0 || n_rot > head_dim || rows > 65535)
    throw std::invalid_argument("u::k::rope_neox_base: n_rot even, <= head_dim, rows <= 65535");
  const float theta_scale = powf(freq_base, -2.0f / (float)n_rot);
  dim3 grid((head_dim / 2 + 127) / 128, (unsigned)rows);
  rope_base_kernel<<<grid, 128, 0, (cudaStream_t)stream>>>(x, out, (int)rows, head_dim, n_rot, theta_scale, pos);
  launch_check("rope_neox_base");
  sync_if_needed(stream, "rope_neox_base");
}

void softmax_rows(float* x, std::int64_t rows, std::int64_t cols, void* stream) {
  if (rows <= 0 || cols <= 0) return;
  const int threads = cols < 1024 ? (int)(((cols + 31) / 32) * 32) : 1024;
  softmax_kernel<<<(unsigned)rows, threads, 0, (cudaStream_t)stream>>>(x, rows, cols);
  launch_check("softmax_rows");
  sync_if_needed(stream, "softmax_rows");
}

void attn_decode_split(const float* q, const float* kk, const float* vv, const float* mask, float* partials,
                       int heads, int kv_heads, int head_dim, int ctx, float scale, int splits,
                       void* stream) {
  if (heads <= 0 || head_dim <= 0 || ctx <= 0 || splits <= 0) return;
  if (head_dim > 1024 || heads % kv_heads != 0) throw std::invalid_argument("u::k::attn: head_dim<=1024, heads%kv_heads==0");
  const int threads = ((head_dim + 31) / 32) * 32;
  const std::size_t smem = ((std::size_t)head_dim + threads) * sizeof(float);
  dim3 grid((unsigned)heads, (unsigned)splits);
  attn_split_kernel<<<grid, threads, smem, (cudaStream_t)stream>>>(q, kk, vv, mask, partials, heads, kv_heads,
                                                                   head_dim, ctx, scale, splits);
  launch_check("attn_decode_split");
  sync_if_needed(stream, "attn_decode_split");
}
void attn_merge(const float* partials, float* out, int heads, int head_dim, int splits, void* stream) {
  if (heads <= 0 || head_dim <= 0 || splits <= 0) return;
  const int threads = ((head_dim + 31) / 32) * 32;
  attn_merge_kernel<<<(unsigned)heads, threads, 0, (cudaStream_t)stream>>>(partials, out, heads, head_dim, splits);
  launch_check("attn_merge");
  sync_if_needed(stream, "attn_merge");
}
void attn_decode(const float* q, const float* k, const float* v, const float* mask, float* out, int heads,
                 int kv_heads, int head_dim, int ctx, float scale, void* stream) {
  // One split is the plain online-softmax decode; the merge then normalizes. This
  // convenience form caches a small device partials buffer (heads*(head_dim+2))
  // so the caller needs no scratch; a family that wants no internal allocation
  // calls attn_decode_split + attn_merge with its own buffer.
  static thread_local float* scratch = nullptr;
  static thread_local std::size_t scratch_elems = 0;
  const std::size_t need = (std::size_t)heads * (head_dim + 2);
  if (need > scratch_elems) {
    if (scratch) cudaFree(scratch);
    if (cudaMalloc(&scratch, need * sizeof(float)) != cudaSuccess) {
      std::fprintf(stderr, "u::k::attn_decode: cudaMalloc failed\n");
      std::exit(1);
    }
    scratch_elems = need;
  }
  attn_decode_split(q, k, v, mask, scratch, heads, kv_heads, head_dim, ctx, scale, 1, stream);
  attn_merge(scratch, out, heads, head_dim, 1, stream);
}

void attn_decode_devlen(const float* q, const float* k, const float* v, float* out, int heads, int kv_heads,
                        int head_dim, const int* d_pos, float scale, void* stream) {
  if (heads <= 0 || head_dim <= 0) return;
  if (head_dim > 1024 || heads % kv_heads != 0) throw std::invalid_argument("u::k::attn: head_dim<=1024, heads%kv_heads==0");
  static thread_local float* scratch = nullptr;  // allocated on the first (eager) call, never during capture
  static thread_local std::size_t scratch_elems = 0;
  const std::size_t need = (std::size_t)heads * (head_dim + 2);
  if (need > scratch_elems) {
    if (scratch) cudaFree(scratch);
    if (cudaMalloc(&scratch, need * sizeof(float)) != cudaSuccess) {
      std::fprintf(stderr, "u::k::attn_decode_devlen: cudaMalloc failed\n");
      std::exit(1);
    }
    scratch_elems = need;
  }
  const int threads = ((head_dim + 31) / 32) * 32;
  const std::size_t smem = ((std::size_t)head_dim + threads) * sizeof(float);
  attn_split_devlen_kernel<<<dim3((unsigned)heads, 1), threads, smem, (cudaStream_t)stream>>>(
      q, k, v, scratch, heads, kv_heads, head_dim, d_pos, scale);
  launch_check("attn_decode_devlen");
  sync_if_needed(stream, "attn_decode_devlen");
  attn_merge(scratch, out, heads, head_dim, 1, stream);
}

void router_topk(const float* logits, int n_tokens, int n_expert, int k, int* ids, float* weights,
                 void* stream) {
  if (n_tokens <= 0 || n_expert <= 0 || k <= 0) return;
  if (k > KMAX || n_expert > 512 * 64) throw std::invalid_argument("u::k::router_topk: k<=64, n_expert<=32768");
  int threads = n_expert < 512 ? n_expert : 512;
  threads = (threads + 31) & ~31;
  const std::size_t taken_bytes = ((std::size_t)n_expert + 15u) & ~(std::size_t)15u;
  const std::size_t smem = taken_bytes + (std::size_t)n_expert * sizeof(double) + (std::size_t)n_expert * sizeof(float);
  router_topk_kernel<<<(unsigned)n_tokens, threads, smem, (cudaStream_t)stream>>>(logits, n_tokens, n_expert, k, ids,
                                                                                  weights);
  launch_check("router_topk");
  sync_if_needed(stream, "router_topk");
}
void router_sigmoid(const float* logits, const float* bias, int n_tokens, int n_expert, int k,
                    float w_scale, bool norm_w, int* ids, float* weights, void* stream) {
  if (n_tokens <= 0 || n_expert <= 0 || k <= 0) return;
  if (k > KMAX || n_expert > 512) throw std::invalid_argument("u::k::router_sigmoid: k<=64, n_expert<=512");
  int threads = (n_expert + 31) & ~31;
  const std::size_t smem = (std::size_t)2 * n_expert * sizeof(float);
  router_sigmoid_kernel<<<(unsigned)n_tokens, threads, smem, (cudaStream_t)stream>>>(
      logits, bias, n_tokens, n_expert, k, w_scale, norm_w ? 1 : 0, ids, weights);
  launch_check("router_sigmoid");
  sync_if_needed(stream, "router_sigmoid");
}

void moe_combine(const float* parts, const float* weights, const float* shared, float* output,
                 std::int64_t n_embd, std::int64_t k, void* stream) {
  if (n_embd <= 0 || k < 1 || k > 64) throw std::invalid_argument("u::k::moe_combine: 1<=k<=64, n_embd>0");
  moe_combine_kernel<<<dim3(grid_for(n_embd, 256), 1), 256, 0, (cudaStream_t)stream>>>(parts, weights, shared,
                                                                                       output, n_embd, (int)k);
  launch_check("moe_combine");
  sync_if_needed(stream, "moe_combine");
}
void moe_combine_multi(const float* parts, const float* weights, const float* shared, float* output,
                       std::int64_t n_embd, std::int64_t k, int n_tok, void* stream) {
  if (n_embd <= 0 || k < 1 || k > 64 || n_tok < 1) throw std::invalid_argument("u::k::moe_combine_multi: bad shape");
  moe_combine_kernel<<<dim3(grid_for(n_embd, 256), (unsigned)n_tok), 256, 0, (cudaStream_t)stream>>>(
      parts, weights, shared, output, n_embd, (int)k);
  launch_check("moe_combine_multi");
  sync_if_needed(stream, "moe_combine_multi");
}

void sample_tokens(const float* logits, int n_tokens, int n_vocab, const int* history, int history_len,
                   const SamplerParams& p, int* out, void* stream) {
  if (n_tokens <= 0 || n_vocab <= 0) return;
  if (p.penalty_last_n > 0 && (history == nullptr || history_len <= 0))
    throw std::invalid_argument("u::k::sample_tokens: penalty_last_n needs a history");
  const unsigned shmem = (history != nullptr && history_len > 0 && p.penalty_last_n > 0)
                             ? (unsigned)((n_vocab + 31) / 32) * sizeof(unsigned)
                             : 0;
  if (p.greedy || p.temperature <= 0.0f)
    sampler_greedy_kernel<<<(unsigned)n_tokens, 1024, shmem, (cudaStream_t)stream>>>(logits, n_vocab, history,
                                                                                     history_len, p, out);
  else
    sampler_kernel<<<(unsigned)n_tokens, 1024, shmem, (cudaStream_t)stream>>>(logits, n_vocab, n_tokens, history,
                                                                              history_len, p, out);
  launch_check("sample_tokens");
  sync_if_needed(stream, "sample_tokens");
}

}  // namespace k
}  // namespace u
