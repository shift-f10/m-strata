// engines/unified/src/kernels/qwen4exp_hc.cu - the corrected hyper-connection kernels (see the header):
// per-stream RMS+gamma, the gated mean over streams, and the per-stream inject. The low-rank down/up/
// inject GEMVs are the graph's (u::k mmvq/bf16); these are the elementwise/reduction pieces.
#include "kernels/qwen4exp_hc.cuh"

#include <cuda_runtime.h>

namespace u::qwen4exp {
namespace {

// One block per stream: RMS over n_embd, then * gamma.
// blockIdx.y = token: token t's hc streams at + t*hc*n_embd (in and out), the same block arithmetic per token.
__global__ void rms_gamma_kernel(const float* __restrict__ streams, const float* __restrict__ gamma,
                                 int n_embd, int hc, float eps, float* __restrict__ xn) {
  const int c = blockIdx.x;  // stream
  const size_t tok = (size_t)blockIdx.y * hc * n_embd;
  const float* in = streams + tok + (size_t)c * n_embd;
  const float* g = gamma + (size_t)c * n_embd;
  float* out = xn + tok + (size_t)c * n_embd;
  __shared__ float ssum;
  float local = 0.0f;
  for (int e = threadIdx.x; e < n_embd; e += blockDim.x) local += in[e] * in[e];
  // block reduce
  for (int o = warpSize / 2; o > 0; o >>= 1) local += __shfl_xor_sync(0xffffffffu, local, o);
  __shared__ float warp[32];
  const int lane = threadIdx.x % warpSize, wid = threadIdx.x / warpSize;
  if (lane == 0) warp[wid] = local;
  __syncthreads();
  if (threadIdx.x == 0) {
    float t = 0.0f;
    const int nw = (blockDim.x + warpSize - 1) / warpSize;
    for (int i = 0; i < nw; ++i) t += warp[i];
    ssum = rsqrtf(t / n_embd + eps);
  }
  __syncthreads();
  const float inv = ssum;
  for (int e = threadIdx.x; e < n_embd; e += blockDim.x) out[e] = in[e] * inv * g[e];
}

// block_in[e] = mean_c( xn[c*n_embd+e] * sigmoid(gate[c*n_embd+e]) ).
__global__ void gated_mean_kernel(const float* __restrict__ xn, const float* __restrict__ gate, int n_embd,
                                  int hc, float* __restrict__ block_in) {
  const int e = blockIdx.x * blockDim.x + threadIdx.x;
  if (e >= n_embd) return;
  const size_t tok = (size_t)blockIdx.y * hc * n_embd;  // blockIdx.y = token
  xn += tok;
  gate += tok;
  block_in += (size_t)blockIdx.y * n_embd;
  float acc = 0.0f;
  for (int c = 0; c < hc; ++c) {
    const size_t i = (size_t)c * n_embd + e;
    acc += xn[i] / (1.0f + __expf(-gate[i]));
  }
  block_in[e] = acc / hc;
}

// out[e] = moe[e] + sigmoid(gate_logit[0]) * shd[e]. The shared-expert gate stays on the device.
__global__ void shexp_combine_kernel(const float* __restrict__ moe, const float* __restrict__ shd,
                                     const float* __restrict__ gate_logit, int n, float* __restrict__ out) {
  const int e = blockIdx.x * blockDim.x + threadIdx.x;
  if (e >= n) return;
  const size_t tok = (size_t)blockIdx.y * n;  // blockIdx.y = token (its logit at gate_logit[token])
  moe += tok;
  shd += tok;
  out += tok;
  const float g = 1.0f / (1.0f + __expf(-gate_logit[blockIdx.y]));
  out[e] = moe[e] + g * shd[e];
}

// streams[c*n_embd+e] += block_out[e] * 2*sigmoid(inject_raw[c]/hc).
__global__ void inject_kernel(float* __restrict__ streams, const float* __restrict__ block_out,
                              const float* __restrict__ inject_raw, int n_embd, int hc) {
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= hc * n_embd) return;
  streams += (size_t)blockIdx.y * hc * n_embd;  // blockIdx.y = token
  block_out += (size_t)blockIdx.y * n_embd;
  inject_raw += (size_t)blockIdx.y * hc;
  const int c = i / n_embd, e = i % n_embd;
  const float w = 2.0f / (1.0f + __expf(-inject_raw[c] / hc));
  streams[i] += block_out[e] * w;
}

}  // namespace

void hc_rms_gamma(const float* streams, const float* gamma, const HcShapes& s, float eps, float* xn,
                  void* stream, int T) {
  rms_gamma_kernel<<<dim3((unsigned)s.hc, (unsigned)T), 256, 0, (cudaStream_t)stream>>>(streams, gamma, s.n_embd,
                                                                                      s.hc, eps, xn);
}

void hc_gated_mean(const float* xn, const float* gate, const HcShapes& s, float* block_in, void* stream, int T) {
  const int threads = 256, blocks = (s.n_embd + threads - 1) / threads;
  gated_mean_kernel<<<dim3((unsigned)blocks, (unsigned)T), threads, 0, (cudaStream_t)stream>>>(xn, gate, s.n_embd,
                                                                                             s.hc, block_in);
}

void hc_inject(float* streams, const float* block_out, const float* inject_raw, const HcShapes& s,
               void* stream, int T) {
  const int n = s.hc * s.n_embd, threads = 256, blocks = (n + threads - 1) / threads;
  inject_kernel<<<dim3((unsigned)blocks, (unsigned)T), threads, 0, (cudaStream_t)stream>>>(streams, block_out,
                                                                                         inject_raw, s.n_embd, s.hc);
}

void shexp_combine(const float* moe, const float* shd, const float* gate_logit, int n, float* out,
                   void* stream, int T) {
  const int threads = 256, blocks = (n + threads - 1) / threads;
  shexp_combine_kernel<<<dim3((unsigned)blocks, (unsigned)T), threads, 0, (cudaStream_t)stream>>>(moe, shd,
                                                                                                gate_logit, n, out);
}

}  // namespace u::qwen4exp
