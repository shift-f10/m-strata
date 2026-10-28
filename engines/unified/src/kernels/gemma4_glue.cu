// strata-unified-engine: Gemma-4-26B-A4B glue kernels (see gemma4_glue.cuh).
// Ported from engines/gemma4/src/g4_kernels.cu (read-only parity oracle); embed_q8 reads the GGUF
// Q8_0 layout directly (no repack) so the unified engine keeps one Q8_0 format end to end.
#include "gemma4_glue.cuh"

#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <cstddef>

namespace u::gemma4 {
namespace {

unsigned blocks(long long n, int per) { return static_cast<unsigned>((n + per - 1) / per); }

// g4_kernels.cu k_softcap (486-489).
__global__ void k_softcap(float* __restrict__ x, int n, float cap) {
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) x[i] = cap * tanhf(x[i] / cap);
}

// One GGUF Q8_0 block: [__half d][32 x int8]. 34 bytes, no padding (the ggml block_q8_0 layout
// the unified Q8_1/MMVQ path is pinned to). Dequant of element i of a row: int8[i] * d(block i/32).
struct BlockQ80 {
  __half d;
  std::int8_t qs[32];
};

// g4_kernels.cu k_embed_q8 (207-214), reading the on-disk Q8_0 blocks instead of repacked planes.
__global__ void k_embed_q8(const int* __restrict__ ids, const BlockQ80* __restrict__ blocks_base, int n, float scale,
                           float* __restrict__ out) {
  const int t = blockIdx.y;
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  const std::size_t nb = static_cast<std::size_t>(n) / 32;       // blocks per row
  const std::size_t row = static_cast<std::size_t>(ids[t]);
  const BlockQ80& blk = blocks_base[row * nb + static_cast<std::size_t>(i) / 32];
  const float d = __half2float(blk.d);
  out[static_cast<std::size_t>(t) * n + i] = float(blk.qs[i % 32]) * d * scale;
}

// Block reduction (g4_kernels.cu warp_sum 15-18 / block_sum 28-37): every thread returns the sum over
// the whole block. `red` is __shared__[32] (one slot per warp). blockDim.x must be a multiple of 32.
__device__ __forceinline__ float warp_sum(float v) {
  for (int o = 16; o > 0; o >>= 1) v += __shfl_xor_sync(0xffffffffu, v, o);
  return v;
}
__device__ float block_sum(float v, float* red) {
  const int lane = threadIdx.x & 31, warp = threadIdx.x >> 5, warps = blockDim.x >> 5;
  v = warp_sum(v);
  __syncthreads();
  if (lane == 0) red[warp] = v;
  __syncthreads();
  float t = lane < warps ? red[lane] : 0.0f;
  t = warp_sum(t);
  return t;
}

// out = x + rms(y)*w, one block per row. g4_kernels.cu k_add_rmsnorm (173-181).
__global__ void k_add_rmsnorm(const float* __restrict__ x, const float* __restrict__ y, const float* __restrict__ w,
                              float* __restrict__ out, int n, float eps) {
  __shared__ float red[32];
  const std::size_t base = static_cast<std::size_t>(blockIdx.x) * n;
  float ss = 0.0f;
  for (int i = threadIdx.x; i < n; i += blockDim.x) ss += y[base + i] * y[base + i];
  const float inv = rsqrtf(block_sum(ss, red) / float(n) + eps);
  for (int i = threadIdx.x; i < n; i += blockDim.x) out[base + i] = x[base + i] + y[base + i] * inv * w[i];
}

// out = (x1 + rms(rms(dense)*w1 + rms(moe)*w2)*w3) * ls, one block per row. The intermediate (d+m) lives
// in dynamic shared memory (n floats). g4_kernels.cu k_ffn_end (183-205); `ls` is passed as a scalar
// (the reference took a device ptr) since the unified graph holds the per-layer scale on the host.
__global__ void k_ffn_end(const float* __restrict__ x1, const float* __restrict__ dense, const float* __restrict__ moe,
                          const float* __restrict__ w1, const float* __restrict__ w2, const float* __restrict__ w3,
                          float ls, float* __restrict__ out, int n, float eps) {
  __shared__ float red[32];
  extern __shared__ float sum[];  // n floats: rms(dense)*w1 + rms(moe)*w2
  const std::size_t base = static_cast<std::size_t>(blockIdx.x) * n;
  float sd = 0.0f, sm = 0.0f;
  for (int i = threadIdx.x; i < n; i += blockDim.x) {
    sd += dense[base + i] * dense[base + i];
    sm += moe[base + i] * moe[base + i];
  }
  const float id = rsqrtf(block_sum(sd, red) / float(n) + eps);
  const float im = rsqrtf(block_sum(sm, red) / float(n) + eps);
  float ss = 0.0f;
  for (int i = threadIdx.x; i < n; i += blockDim.x) {
    const float v = dense[base + i] * id * w1[i] + moe[base + i] * im * w2[i];
    sum[i] = v;
    ss += v * v;
  }
  const float is = rsqrtf(block_sum(ss, red) / float(n) + eps);
  for (int i = threadIdx.x; i < n; i += blockDim.x) out[base + i] = (x1[base + i] + sum[i] * is * w3[i]) * ls;
}

// Single-thread device write to the position cell: increment (for the graph tail) or set to `value`.
__global__ void k_pos_write(int* __restrict__ pos_dev, int value, bool inc) {
  if (inc)
    *pos_dev += 1;
  else
    *pos_dev = value;
}

}  // namespace

void softcap(float* x, int n, float cap, void* stream) {
  k_softcap<<<blocks(n, 256), 256, 0, static_cast<cudaStream_t>(stream)>>>(x, n, cap);
}

void embed_q8(const int* ids, int T, const void* q8_blocks, int n, float scale, float* out, void* stream) {
  k_embed_q8<<<dim3(blocks(n, 256), static_cast<unsigned>(T)), 256, 0, static_cast<cudaStream_t>(stream)>>>(
      ids, static_cast<const BlockQ80*>(q8_blocks), n, scale, out);
}

void add_rmsnorm(const float* x, const float* y, const float* w, float* out, int rows, int n, float eps, void* stream) {
  k_add_rmsnorm<<<static_cast<unsigned>(rows), 256, 0, static_cast<cudaStream_t>(stream)>>>(x, y, w, out, n, eps);
}

void ffn_end(const float* x1, const float* dense, const float* moe, const float* w1, const float* w2, const float* w3,
             float layer_scale, float* out, int rows, int n, float eps, void* stream) {
  k_ffn_end<<<static_cast<unsigned>(rows), 256, static_cast<std::size_t>(n) * sizeof(float),
              static_cast<cudaStream_t>(stream)>>>(x1, dense, moe, w1, w2, w3, layer_scale, out, n, eps);
}

void pos_set(int* pos_dev, int value, void* stream) {
  k_pos_write<<<1, 1, 0, static_cast<cudaStream_t>(stream)>>>(pos_dev, value, false);
}

void pos_inc(int* pos_dev, void* stream) {
  k_pos_write<<<1, 1, 0, static_cast<cudaStream_t>(stream)>>>(pos_dev, 0, true);
}

}  // namespace u::gemma4
