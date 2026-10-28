// engines/unified/src/kernels/qwen4exp_gdn.cu - GDN (Gated-DeltaNet) linear attention for qwen4exp.
// See qwen4exp_gdn.cuh for the contract, provenance and the ref/gdn.py order.
//
// NOT COMPILED ON CI/dev-host — built only behind STRATA_U_CUDA on the owner's GPU box (design §1.3).
// The kernel bodies below transcribe the reference math (upstream native_gdn.cu / native_gdn_preprocess
// .cu / fused_gdn.cu, ref/gdn.py). The box compiles, runs -Werror=all-warnings, and parities against
// ref/gdn.py::gdn_decode and the upstream engine's per-layer dump (generate.cpp --dump-layer).
#include "kernels/qwen4exp_gdn.cuh"

#include <cuda_runtime.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace u::qwen4exp {
namespace {

constexpr int kThreads = 128;

__device__ __forceinline__ float silu(float x) { return x / (1.0f + __expf(-x)); }
__device__ __forceinline__ float sigmoidf(float x) { return 1.0f / (1.0f + __expf(-x)); }
// softplus, numerically stable (ref/gdn.py: log1p(exp(x)) with the large-x branch).
__device__ __forceinline__ float softplus(float x) { return x > 20.0f ? x : log1pf(__expf(x)); }

// Causal depthwise short-conv over the channel axis + SiLU, history carried in place. One thread per
// (channel). ref/gdn.py: conv1d then SiLU on the WHOLE output, before the q|k|v split.
__global__ void conv_silu_kernel(const float* __restrict__ qkv, const float* __restrict__ conv_w,
                                 float* __restrict__ conv_state, float* __restrict__ out,
                                 int conv_channels, int d_conv, int T, bool write_state) {
  const int c = blockIdx.x * blockDim.x + threadIdx.x;
  if (c >= conv_channels) return;
  const int hist = d_conv - 1;
  // history rows are (d_conv-1, conv_channels), oldest first; load this channel's window.
  float win[8];  // d_conv <= 8 (qwen4exp: 4)
  for (int j = 0; j < hist; ++j) win[j] = conv_state[(size_t)j * conv_channels + c];
  for (int t = 0; t < T; ++t) {
    const float x = qkv[(size_t)t * conv_channels + c];
    // conv_w GGUF shape [d_conv, conv_channels], ne0=d_conv contiguous: channel c's tap j at c*d_conv + j.
    float acc = 0.0f;
    for (int tap = 0; tap < hist; ++tap) acc += conv_w[(size_t)c * d_conv + tap] * win[tap];
    acc += conv_w[(size_t)c * d_conv + hist] * x;  // the current tap
    out[(size_t)t * conv_channels + c] = silu(acc);
    // shift the window (oldest out, x in)
    for (int j = 0; j < hist - 1; ++j) win[j] = win[j + 1];
    if (hist > 0) win[hist - 1] = x;
  }
  if (write_state)
    for (int j = 0; j < hist; ++j) conv_state[(size_t)j * conv_channels + c] = win[j];
}

// Split the (conv_channels) SiLU'd output into q (S*h_k), k (S*h_k), v (S*h_v) and L2-normalize q,k
// per head (v is NOT normalized). The conv layout is [q heads | k heads | v heads] contiguous.
// One block per (head, token); the block reduces the head's S-vector norm. ref/gdn.py gdn_l2_norm.
__global__ void split_l2norm_kernel(const float* __restrict__ h, int S, int h_k, int h_v, int T,
                                    float eps, float* __restrict__ q, float* __restrict__ k,
                                    float* __restrict__ v) {
  // blockIdx.x: head index in the concatenated [q(h_k) | k(h_k) | v(h_v)] ordering; blockIdx.y: token.
  const int total_heads = 2 * h_k + h_v;
  const int head = blockIdx.x;
  const int t = blockIdx.y;
  if (head >= total_heads || t >= T) return;
  const int conv_channels = S * total_heads;
  const float* src = h + (size_t)t * conv_channels + (size_t)head * S;

  const bool is_v = head >= 2 * h_k;
  if (is_v) {
    // v: copy, no norm. (one warp-strided copy)
    float* dst = v + (size_t)t * (S * h_v) + (size_t)(head - 2 * h_k) * S;
    for (int e = threadIdx.x; e < S; e += blockDim.x) dst[e] = src[e];
    return;
  }
  // q/k: L2 norm over the S elements.
  __shared__ float red[kThreads / 32];
  float part = 0.0f;
  for (int e = threadIdx.x; e < S; e += blockDim.x) part += src[e] * src[e];
  // warp + block reduce
  for (int o = 16; o > 0; o >>= 1) part += __shfl_down_sync(0xffffffffu, part, o);
  if ((threadIdx.x & 31) == 0) red[threadIdx.x >> 5] = part;
  __syncthreads();
  float ss = 0.0f;
  if (threadIdx.x < blockDim.x / 32) ss = red[threadIdx.x];
  for (int o = 16; o > 0; o >>= 1) ss += __shfl_down_sync(0xffffffffu, ss, o);
  __shared__ float inv;
  if (threadIdx.x == 0) inv = rsqrtf(ss + eps);
  __syncthreads();
  const bool is_k = head >= h_k;
  float* dst = (is_k ? k : q) + (size_t)t * (S * h_k) + (size_t)(is_k ? head - h_k : head) * S;
  for (int e = threadIdx.x; e < S; e += blockDim.x) dst[e] = src[e] * inv;
}

// Per-v-head scalar gates. One thread per (v-head, token).
__global__ void gates_kernel(const float* __restrict__ alpha_in, const float* __restrict__ beta_in,
                             const float* __restrict__ dt_bias, const float* __restrict__ ssm_a,
                             int h_v, int T, float* __restrict__ gate, float* __restrict__ beta) {
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= h_v * T) return;
  const int hv = i % h_v;
  beta[i] = sigmoidf(beta_in[i]);
  gate[i] = softplus(alpha_in[i] + dt_bias[hv]) * ssm_a[hv];  // ssm_a < 0
}

// The recurrent delta-rule step, one block per v-head (32 threads), state carried in place.
// state is (h_v, S, S): S[hv] row-major, row = KEY axis. GQA: v-head hv reads q/k group hv/(h_v/h_k).
// ref/gdn.py: S=diag(exp(g))@S; delta=beta*(v - S^T k); S += k delta^T; o=(1/sqrt S) S^T q.
// Transcribed from upstream native_gdn.cu step<<<>>> (the kernel the launcher dispatches).
__global__ void recurrence_kernel(float* __restrict__ state, const float* __restrict__ q,
                                  const float* __restrict__ k, const float* __restrict__ v,
                                  const float* __restrict__ gate, const float* __restrict__ beta,
                                  int S, int h_k, int h_v, int T, float scale, float* __restrict__ o) {
  const int hv = blockIdx.x;
  if (hv >= h_v) return;
  const int hk = hv % h_k;                  // the q/k head this v-head reads (GQA repeat = tiling, native_gdn.cu q_head = head % h_k)
  float* Shv = state + (size_t)hv * S * S;   // this head's (S key rows, S? ) — see layout note below
  const int lane = threadIdx.x;             // 32 lanes cover S=128 in 4 strides

  for (int t = 0; t < T; ++t) {
    const float g = gate[(size_t)t * h_v + hv];
    const float eg = __expf(g);
    const float b = beta[(size_t)t * h_v + hv];
    const float* qt = q + (size_t)t * (S * h_k) + (size_t)hk * S;
    const float* kt = k + (size_t)t * (S * h_k) + (size_t)hk * S;
    const float* vt = v + (size_t)t * (S * h_v) + (size_t)hv * S;
    float* ot = o + (size_t)t * (S * h_v) + (size_t)hv * S;

    // S = diag(exp(g)) @ S : scale every KEY row by eg (per-head scalar decay). row r = key axis.
    for (int r = lane; r < S; r += 32)
      for (int c = 0; c < S; ++c) Shv[(size_t)r * S + c] *= eg;

    // delta = beta * (v - S^T k) : (S^T k)[c] = sum_r S[r,c] * k[r]
    // then S += k (delta)^T : S[r,c] += k[r] * delta[c]
    // and  o = (1/sqrt S) S^T q : o[c] = scale * sum_r S[r,c] * q[r]
    // Done column-strided across lanes (each lane owns a set of columns c).
    for (int c = lane; c < S; c += 32) {
      float sTk = 0.0f;
      for (int r = 0; r < S; ++r) sTk += Shv[(size_t)r * S + c] * kt[r];  // pre-update S^T k
      const float delta = b * (vt[c] - sTk);
      // rank-1 update then the output share the (now-updated) state column: o = (1/sqrt S) S^T q
      float sTq = 0.0f;
      for (int r = 0; r < S; ++r) {
        const float srC = Shv[(size_t)r * S + c] + kt[r] * delta;  // S += k delta^T
        Shv[(size_t)r * S + c] = srC;
        sTq += srC * qt[r];
      }
      ot[c] = scale * sTq;
    }
  }
}

__device__ __forceinline__ float gdn_warp_sum(float v) {
#pragma unroll
  for (int o = 16; o > 0; o >>= 1) v += __shfl_xor_sync(0xffffffffu, v, o);
  return v;
}

// The same delta-rule step for S = 128 in the shape of upstream native_gdn.cu step<<<>>>: one WARP per
// (v-head, value column) holding that column's 128 key rows in registers (4 per lane), grid (h_v, S/4) x
// block (32, 4) -- ~100x the threads of recurrence_kernel's 32 a head (nsys: 75 us -> a few us a layer).
// State layout here is (h_v, S cols, S rows), rows contiguous: element (row i, col c) of head hv at
// hv*S*S + c*S + i, so a warp's 4 row loads are coalesced. Same math: kv = S^T k on the old state,
// delta = beta*(v - exp(g)*kv), S' = exp(g)*S + k delta^T, o = scale * S'^T q.
__global__ void __launch_bounds__(128)
    recurrence_s128_kernel(float* __restrict__ state, const float* __restrict__ q, const float* __restrict__ k,
                           const float* __restrict__ v, const float* __restrict__ gate,
                           const float* __restrict__ beta, int h_k, int h_v, int T, float scale,
                           float* __restrict__ o, bool write_state) {
  constexpr int S = 128;
  const int hv = (int)blockIdx.x;
  const int col = (int)blockIdx.y * 4 + (int)threadIdx.y;
  const int lane = (int)threadIdx.x;
  const int hk = hv % h_k;  // GQA repeat = tiling (native_gdn.cu q_head = head % h_k)
  float* sc = state + ((size_t)hv * S + col) * S;
  float s[4];
#pragma unroll
  for (int r = 0; r < 4; ++r) s[r] = sc[r * 32 + lane];
  for (int t = 0; t < T; ++t) {
    const float* qt = q + (size_t)t * (S * h_k) + (size_t)hk * S;
    const float* kt = k + (size_t)t * (S * h_k) + (size_t)hk * S;
    float kr[4], qr[4];
#pragma unroll
    for (int r = 0; r < 4; ++r) {
      kr[r] = kt[r * 32 + lane];
      qr[r] = qt[r * 32 + lane];
    }
    const float g = __expf(gate[(size_t)t * h_v + hv]);
    float kv = 0.0f;
#pragma unroll
    for (int r = 0; r < 4; ++r) kv += s[r] * kr[r];
    kv = gdn_warp_sum(kv);
    const float delta = (v[(size_t)t * (S * h_v) + (size_t)hv * S + col] - g * kv) * beta[(size_t)t * h_v + hv];
    float at = 0.0f;
#pragma unroll
    for (int r = 0; r < 4; ++r) {
      s[r] = g * s[r] + kr[r] * delta;
      at += s[r] * qr[r];
    }
    at = gdn_warp_sum(at);
    if (lane == 0) o[(size_t)t * (S * h_v) + (size_t)hv * S + col] = at * scale;
  }
  if (write_state)
#pragma unroll
    for (int r = 0; r < 4; ++r) sc[r * 32 + lane] = s[r];
}

// y = rms_norm(o, ssm_norm) * sigmoid(z) per v-head. One block per (v-head, token).
__global__ void out_gate_kernel(const float* __restrict__ o, const float* __restrict__ ssm_norm,
                                const float* __restrict__ z, int S, int h_v, int T, float eps,
                                float* __restrict__ y) {
  const int hv = blockIdx.x;
  const int t = blockIdx.y;
  if (hv >= h_v || t >= T) return;
  const float* ot = o + (size_t)t * (S * h_v) + (size_t)hv * S;
  const float* zt = z + (size_t)t * (S * h_v) + (size_t)hv * S;
  float* yt = y + (size_t)t * (S * h_v) + (size_t)hv * S;
  __shared__ float red[kThreads / 32];
  float part = 0.0f;
  for (int e = threadIdx.x; e < S; e += blockDim.x) part += ot[e] * ot[e];
  for (int off = 16; off > 0; off >>= 1) part += __shfl_down_sync(0xffffffffu, part, off);
  if ((threadIdx.x & 31) == 0) red[threadIdx.x >> 5] = part;
  __syncthreads();
  float ss = 0.0f;
  if (threadIdx.x < blockDim.x / 32) ss = red[threadIdx.x];
  for (int off = 16; off > 0; off >>= 1) ss += __shfl_down_sync(0xffffffffu, ss, off);
  __shared__ float inv;
  if (threadIdx.x == 0) inv = rsqrtf(ss / (float)S + eps);
  __syncthreads();
  for (int e = threadIdx.x; e < S; e += blockDim.x)
    yt[e] = (ot[e] * inv * ssm_norm[e]) * sigmoidf(zt[e]);  // SIGMOID gate (qwen4exp)
}

}  // namespace

void gdn_conv_silu(const float* qkv, const float* conv_w, float* conv_state, const GdnShapes& s, int T,
                   float* out, void* stream, bool write_state) {
  const int blocks = (s.conv_channels + kThreads - 1) / kThreads;
  conv_silu_kernel<<<blocks, kThreads, 0, (cudaStream_t)stream>>>(qkv, conv_w, conv_state, out,
                                                                  s.conv_channels, s.d_conv, T, write_state);
}

void gdn_split_l2norm(const float* h, const GdnShapes& s, int T, float eps, float* q, float* k,
                      float* v, void* stream) {
  dim3 grid((unsigned)(2 * s.k_heads + s.v_heads), (unsigned)T);
  split_l2norm_kernel<<<grid, kThreads, 0, (cudaStream_t)stream>>>(h, s.state, s.k_heads, s.v_heads, T,
                                                                   eps, q, k, v);
}

void gdn_gates(const float* alpha_in, const float* beta_in, const float* dt_bias, const float* ssm_a,
               const GdnShapes& s, int T, float* gate, float* beta, void* stream) {
  const int n = s.v_heads * T;
  const int blocks = (n + kThreads - 1) / kThreads;
  gates_kernel<<<blocks, kThreads, 0, (cudaStream_t)stream>>>(alpha_in, beta_in, dt_bias, ssm_a,
                                                              s.v_heads, T, gate, beta);
}

void gdn_recurrence(float* state, const float* q, const float* k, const float* v, const float* gate,
                    const float* beta, const GdnShapes& s, int T, float* o, void* stream, bool write_state) {
  const float scale = 1.0f / sqrtf((float)s.state);  // 1/sqrt(S), applied on q inside the kernel
  if (s.state == 128) {  // the warp-per-column form (its own state layout: rows contiguous per column)
    recurrence_s128_kernel<<<dim3((unsigned)s.v_heads, 128 / 4), dim3(32, 4), 0, (cudaStream_t)stream>>>(
        state, q, k, v, gate, beta, s.k_heads, s.v_heads, T, scale, o, write_state);
    return;
  }
  if (!write_state) {  // the generic form updates the state in place token by token: no read-only mode
    std::fprintf(stderr, "qwen4exp: gdn_recurrence without a state write needs state == 128\n");
    std::abort();
  }
  recurrence_kernel<<<(unsigned)s.v_heads, 32, 0, (cudaStream_t)stream>>>(state, q, k, v, gate, beta,
                                                                          s.state, s.k_heads, s.v_heads,
                                                                          T, scale, o);
}

void gdn_out_gate(const float* o, const float* ssm_norm, const float* z, const GdnShapes& s, int T,
                  float eps, float* y, void* stream) {
  dim3 grid((unsigned)s.v_heads, (unsigned)T);
  out_gate_kernel<<<grid, kThreads, 0, (cudaStream_t)stream>>>(o, ssm_norm, z, s.state, s.v_heads, T,
                                                               eps, y);
}

// TODO(box): the state layout (h_v, S, S) above follows ref/gdn.py's "(S, h_v, S) here vs (S, S, h_v)
// there" note (layer.hpp L26-27); native_gdn.cu's step<<<dim3(h_v,1,S/4), dim3(32,4)>>> uses a
// 4-warp tiling of the S/S update that is faster than the single-warp loop here. Transcribe that
// tiling (native_gdn.cu L48-83) once the single-warp version parities — the math is identical, the
// tiling is the speed. Also wire the chunked prefill (upstream prefill/gdn_chunk.cu) if T>1 prompt
// chunks are served; the serve path here is T=1 decode + a simple T>1 teacher-forced prefill.

}  // namespace u::qwen4exp
