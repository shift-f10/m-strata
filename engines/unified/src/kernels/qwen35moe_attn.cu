// strata-unified-engine: Qwen3.6-35B-A3B periodic full-attention kernels. See qwen35moe_attn.cuh
// for the shapes, the per-head math and the q36_engine.cu / q36ref.cu lines each was ported from.
//
// attn_prep is q36_engine.cu:256 k_attn_prep with the text-only simplification (no M-RoPE section
// table, no picture rotary offset — q36ref.cu:280). attn is the single-pass softmax of q36ref.cu:294
// k_attn, one block per (query head, token), with the GQA kv-head map and the sigmoid output gate.
// Both are f32 (the reference/parity contract). Only the namespace, the host-only launcher ABI and
// the trimming of the device-pos / CUDA-graph / split-K / FP16-prompt fast paths differ from the
// production; the GPU box re-adds the split-K + FP16 prompt path for throughput (k_attn_part :301,
// k_attn_merge :341, k_kv_f16 :428, k_attn_softmax16 :439).
//
// NOT COMPILED ON CI/dev-host: built only behind STRATA_U_CUDA on the owner's NVIDIA box.
#include <math_constants.h>  // CUDART_INF_F
#include "qwen35moe_attn.cuh"

#include <cuda_runtime.h>

namespace u::qwen35moe {
namespace {

__device__ __forceinline__ float sigm(float v) { return 1.0f / (1.0f + __expf(-v)); }

// 256-thread block reductions (one thread per head_dim element).
__device__ float block_sum(float v, float* sh) {
  sh[threadIdx.x] = v;
  __syncthreads();
  for (int s = blockDim.x / 2; s > 0; s >>= 1) {
    if ((int) threadIdx.x < s) sh[threadIdx.x] += sh[threadIdx.x + s];
    __syncthreads();
  }
  const float r = sh[0];
  __syncthreads();
  return r;
}
__device__ float block_max(float v, float* sh) {
  sh[threadIdx.x] = v;
  __syncthreads();
  for (int s = blockDim.x / 2; s > 0; s >>= 1) {
    if ((int) threadIdx.x < s) sh[threadIdx.x] = fmaxf(sh[threadIdx.x], sh[threadIdx.x + s]);
    __syncthreads();
  }
  const float r = sh[0];
  __syncthreads();
  return r;
}

// ---- (1) per-head RMS + NEOX RoPE + KV write + gate keep (q36_engine.cu:256 k_attn_prep) -------
// grid (H + HKV, T), block HD (== 256). head < H is a query head; head >= H is a kv head (head-H).
__global__ void __launch_bounds__(256) k_attn_prep(const float* qg, const float* kx, const float* vx,
                                                   const float* qw, const float* kw, float* q, float* gate,
                                                   float* kc, float* vc, int H, int HKV, int HD, int n_rot,
                                                   int pos0, int ctx, float base, float eps, int T) {
  __shared__ float sh[256];
  __shared__ float buf[256];
  const int head = blockIdx.x, t = blockIdx.y;
  const int pos = pos0 + t;
  const bool isq = head < H;
  const float* src = isq ? qg + ((size_t) t * H + head) * 2 * HD : kx + ((size_t) t * HKV + head - H) * HD;
  const int i = threadIdx.x;
  const float v0 = i < HD ? src[i] : 0.f;
  const float s = block_sum(v0 * v0, sh);
  const float r = rsqrtf(s / (float) HD + eps);
  if (i < HD) buf[i] = v0 * r * (isq ? qw[i] : kw[i]);
  __syncthreads();
  if (i >= HD) return;
  float v = buf[i];
  const int half = n_rot / 2;
  if (i < n_rot) {  // NEOX partial rotary over the first n_rot dims, pairs (p, p+half)
    const int p = i < half ? i : i - half;
    const float theta = (float) pos * powf(base, -2.0f * (float) p / (float) n_rot);
    float sn, cs;
    sincosf(theta, &sn, &cs);
    v = i < half ? buf[i] * cs - buf[i + half] * sn : buf[i - half] * sn + buf[i] * cs;
  }
  if (isq) {
    q[((size_t) t * H + head) * HD + i] = v;
    gate[((size_t) t * H + head) * HD + i] = qg[((size_t) t * H + head) * 2 * HD + HD + i];  // the gate half
  } else {
    const int kh = head - H;
    kc[((size_t) kh * ctx + pos) * HD + i] = v;
    vc[((size_t) kh * ctx + pos) * HD + i] = vx[((size_t) t * HKV + kh) * HD + i];
  }
}

// ---- (2) single-pass GQA softmax attention + sigmoid output gate (q36ref.cu:294 k_attn) --------
// grid (H, T), block HD (== 256), dynamic shared sc[n_pos]. One block per (query head h, token t).
__global__ void __launch_bounds__(256) k_attn(const float* q, const float* kc, const float* vc,
                                              const float* gate, float* out, int H, int HKV, int HD,
                                              int pos0, int ctx, float scale) {
  extern __shared__ float sc[];  // n_pos scores for this (h, t)
  __shared__ float sh[256];
  const int h = blockIdx.x, t = blockIdx.y;
  const int n_pos = pos0 + t + 1;
  const int kvh = h / (H / HKV);
  const float* qh = q + ((size_t) t * H + h) * HD;
  for (int p = threadIdx.x; p < n_pos; p += blockDim.x) {
    const float* kp = kc + ((size_t) kvh * ctx + p) * HD;
    float a = 0.f;
    for (int i = 0; i < HD; ++i) a += qh[i] * kp[i];
    sc[p] = a * scale;
  }
  __syncthreads();
  float mx = -CUDART_INF_F;
  for (int p = threadIdx.x; p < n_pos; p += blockDim.x) mx = fmaxf(mx, sc[p]);
  mx = block_max(mx, sh);
  float sum = 0.f;
  for (int p = threadIdx.x; p < n_pos; p += blockDim.x) {
    sc[p] = __expf(sc[p] - mx);
    sum += sc[p];
  }
  sum = block_sum(sum, sh);
  const int i = threadIdx.x;  // i < HD == blockDim.x
  float acc = 0.f;
  for (int p = 0; p < n_pos; ++p) acc += sc[p] * vc[((size_t) kvh * ctx + p) * HD + i];
  const size_t gi = ((size_t) t * H + h) * HD + i;
  out[gi] = acc / sum * sigm(gate[gi]);
}

inline cudaStream_t cs(void* s) { return static_cast<cudaStream_t>(s); }

}  // namespace

void attn_prep(const float* qg, const float* kx, const float* vx, const float* qw, const float* kw,
               float* q, float* gate, float* kc, float* vc, int H, int HKV, int HD, int n_rot,
               int pos0, int ctx, float base, float eps, int T, void* stream) {
  k_attn_prep<<<dim3((unsigned) (H + HKV), (unsigned) T), 256, 0, cs(stream)>>>(
      qg, kx, vx, qw, kw, q, gate, kc, vc, H, HKV, HD, n_rot, pos0, ctx, base, eps, T);
}

void attn(const float* q, const float* kc, const float* vc, const float* gate, float* out, int H,
          int HKV, int HD, int pos0, int ctx, float scale, int T, void* stream) {
  const size_t shbytes = (size_t) (pos0 + T) * sizeof(float);  // the longest row's score buffer
  k_attn<<<dim3((unsigned) H, (unsigned) T), 256, shbytes, cs(stream)>>>(q, kc, vc, gate, out, H, HKV, HD,
                                                                        pos0, ctx, scale);
}

}  // namespace u::qwen35moe
