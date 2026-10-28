// engines/unified/src/kernels/qwen4exp_ple.cu - PLE (Per-Layer Embedding) arithmetic for qwen4exp.
// See qwen4exp_ple.cuh for the contract and provenance (out/.../src/models/qwen4exp.cpp build_ple).
// Built behind STRATA_U_CUDA on the owner's GPU box. The grouped RMS norms are the HC read's
// hc_rms_gamma (reused by the graph); the host-side n-gram hash + IQ4_NL row gather live in the graph.
#include "kernels/qwen4exp_ple.cuh"

#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <cmath>

namespace u::qwen4exp {
namespace {

constexpr int kThreads = 256;

__device__ __forceinline__ float sigmoidf(float x) { return 1.0f / (1.0f + __expf(-x)); }
__device__ __forceinline__ float siluf(float x) { return x / (1.0f + __expf(-x)); }

// One block per stream c: s = (sum_e kn*qn)/sqrt(n_embd); gate = sigmoid(sgn(s)*sqrt(clamp(|s|,1e-6))).
__global__ void gate_kernel(const float* __restrict__ kn, const float* __restrict__ qn, int n_embd,
                            float* __restrict__ gate) {
  const int c = blockIdx.x;
  const float* a = kn + (size_t)c * n_embd;
  const float* b = qn + (size_t)c * n_embd;
  float local = 0.0f;
  for (int e = threadIdx.x; e < n_embd; e += blockDim.x) local += a[e] * b[e];
  for (int o = warpSize / 2; o > 0; o >>= 1) local += __shfl_xor_sync(0xffffffffu, local, o);
  __shared__ float warp[32];
  const int lane = threadIdx.x % warpSize, wid = threadIdx.x / warpSize;
  if (lane == 0) warp[wid] = local;
  __syncthreads();
  if (threadIdx.x == 0) {
    float t = 0.0f;
    const int nw = (blockDim.x + warpSize - 1) / warpSize;
    for (int i = 0; i < nw; ++i) t += warp[i];
    const float s = t / sqrtf((float)n_embd);
    const float mag = sqrtf(fmaxf(fabsf(s), 1e-6f));
    const float v = s > 0.0f ? mag : (s < 0.0f ? -mag : 0.0f);
    gate[c] = sigmoidf(v);
  }
}

// gated[c*n_embd + e] = value[e] * gate[c].
__global__ void gated_value_kernel(const float* __restrict__ value, const float* __restrict__ gate,
                                   int n_embd, int hc, float* __restrict__ gated) {
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= hc * n_embd) return;
  const int c = i / n_embd, e = i % n_embd;
  gated[i] = value[e] * gate[c];
}

// Dilated causal depthwise conv + SiLU over the per-channel history. One thread per (stream, embd elem)
// = one channel of the hc_dim vector. conv_w is [d_conv, hc_dim] (ne0=d_conv): channel ch tap k at
// ch*d_conv + k. hist is (kern-1)*dil vectors of hc_dim, oldest-first; shifted by one per call.
__global__ void conv_silu_kernel(const float* __restrict__ normed, const __half* __restrict__ conv_w,
                                 float* __restrict__ hist, int n_embd, int hc, int kern, int dil,
                                 float* __restrict__ conv_out) {
  const int ch = blockIdx.x * blockDim.x + threadIdx.x;  // channel in [0, hc*n_embd)
  const int hc_dim = hc * n_embd;
  if (ch >= hc_dim) return;
  const int hist_len = (kern - 1) * dil;
  const float cur = normed[ch];
  // tap k (k=0..kern-1) reads (kern-1-k)*dil tokens back; tap kern-1 = current.
  float acc = 0.0f;
  for (int k = 0; k < kern; ++k) {
    const int back = (kern - 1 - k) * dil;  // positions back (0 = current)
    float x;
    if (back == 0) {
      x = cur;
    } else {
      // hist oldest-first: index hist_len-back is `back` tokens back.
      const int hi = hist_len - back;
      x = (hi >= 0) ? hist[(size_t)hi * hc_dim + ch] : 0.0f;
    }
    acc += __half2float(conv_w[(size_t)ch * kern + k]) * x;
  }
  conv_out[ch] = siluf(acc);
  // advance history: shift oldest out, append `cur` as the newest (index hist_len-1).
  for (int j = 0; j < hist_len - 1; ++j)
    hist[(size_t)j * hc_dim + ch] = hist[(size_t)(j + 1) * hc_dim + ch];
  if (hist_len > 0) hist[(size_t)(hist_len - 1) * hc_dim + ch] = cur;
}

__global__ void add2_kernel(float* __restrict__ hidden, const float* __restrict__ a,
                            const float* __restrict__ b, int n) {
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) hidden[i] += a[i] + b[i];
}

}  // namespace

void ple_gate(const float* key_norm, const float* query_norm, int n_embd, int hc, float* gate,
              void* stream) {
  gate_kernel<<<hc, 256, 0, (cudaStream_t)stream>>>(key_norm, query_norm, n_embd, gate);
}

void ple_gated_value(const float* value, const float* gate, int n_embd, int hc, float* gated,
                     void* stream) {
  const int n = hc * n_embd, blocks = (n + kThreads - 1) / kThreads;
  gated_value_kernel<<<blocks, kThreads, 0, (cudaStream_t)stream>>>(value, gate, n_embd, hc, gated);
}

void ple_conv_silu(const float* normed, const void* conv_w_f16, float* hist, int n_embd, int hc, int kern,
                   int dil, float* conv_out, void* stream) {
  const int n = hc * n_embd, blocks = (n + kThreads - 1) / kThreads;
  conv_silu_kernel<<<blocks, kThreads, 0, (cudaStream_t)stream>>>(
      normed, (const __half*)conv_w_f16, hist, n_embd, hc, kern, dil, conv_out);
}

void ple_add2(float* hidden, const float* a, const float* b, int n, void* stream) {
  const int blocks = (n + kThreads - 1) / kThreads;
  add2_kernel<<<blocks, kThreads, 0, (cudaStream_t)stream>>>(hidden, a, b, n);
}

}  // namespace u::qwen4exp
