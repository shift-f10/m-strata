// strata-unified-engine: Raon-1-mini RDA kernels. See raon1_rda.cuh for the GLM analogy and the
// open TODOs. Ported faithfully from engines/maya-ref/src/kernels/cuda/glm_kda.cu (the gated-delta
// "KDA" linear attention), with Raon's dims (n_head 40, head_dim 128) and the unified launcher
// convention. The math is unchanged from GLM's; only the namespace and the launcher ABI differ.
//
// NOT COMPILED ON CI/dev-host: the unified engine builds the family .cu only behind STRATA_U_CUDA
// on the owner's GPU box (design §1.3 / plan "What is verifiable where"). This file is written for
// that build and verified there against the real Raon-1-mini weights.
#include "raon1_rda.cuh"

#include <cuda_runtime.h>

#include <cstdio>
#include <cstdlib>

namespace u::raon {
namespace {

constexpr int RDA_THREADS = 256;
constexpr int RDA_MAX_D_CONV = 16;  // the slide's register staging bound (as GLM's KD_MAX_D_CONV)

__device__ __forceinline__ float dsigmoid(float x) { return 1.0f / (1.0f + __expf(-x)); }

// one thread per (channel, token): the d_conv taps read x_proj where the window is inside the
// sequence and the state columns where it is not, then SiLU. Reads history only; the slide is a
// separate kernel (an in-place slide while earlier tokens' threads still read the old columns is
// a race — see glm_kda.cu).
__global__ void rda_conv_kernel(const float* __restrict__ x_proj, const float* __restrict__ conv_w,
                                const float* __restrict__ conv_state, int d_inner, int d_conv, int T,
                                float* __restrict__ out) {
  const int idx = blockIdx.x * blockDim.x + threadIdx.x;
  const int total = d_inner * T;
  if (idx >= total) return;
  const int c = idx % d_inner;  // ggml order: features fastest
  const int t = idx / d_inner;
  const int hist = d_conv - 1;

  float acc = 0.0f;
  for (int tap = 0; tap < d_conv; ++tap) {
    const int src = t - hist + tap;
    const float x = src >= 0 ? x_proj[(size_t)src * d_inner + c]
                             : conv_state[(size_t)(src + hist) + (size_t)hist * c];
    acc += conv_w[(size_t)tap + (size_t)d_conv * c] * x;
  }
  out[(size_t)t * d_inner + c] = acc / (1.0f + __expf(-acc));  // silu
}

// one thread per channel: the history becomes the last (d_conv-1) columns of [state | x_proj],
// oldest first; values are staged in registers before the write.
__global__ void rda_conv_slide_kernel(const float* __restrict__ x_proj, float* __restrict__ conv_state,
                                       int d_inner, int d_conv, int T) {
  const int c = blockIdx.x * blockDim.x + threadIdx.x;
  if (c >= d_inner) return;
  const int hist = d_conv - 1;
  float nv[RDA_MAX_D_CONV];
  for (int j = 0; j < hist; ++j) {
    const int src = T - hist + j;  // may still reach into the old state when T < hist
    nv[j] = src >= 0 ? x_proj[(size_t)src * d_inner + c]
                     : conv_state[(size_t)(src + hist) + (size_t)hist * c];
  }
  for (int j = 0; j < hist; ++j) conv_state[(size_t)j + (size_t)hist * c] = nv[j];
}

// one thread per output element: the lower-bound squash, reshaping (d_inner, T) into
// (head_dim, n_head, T) on the way through.
__global__ void rda_gate_kernel(const float* __restrict__ g, const float* __restrict__ decay_a,
                                 float lower_bound, int head_dim, int n_head, int T,
                                 float* __restrict__ g1) {
  const int idx = blockIdx.x * blockDim.x + threadIdx.x;
  const int total = head_dim * n_head * T;
  if (idx >= total) return;
  const int e = idx % head_dim;
  const int h = (idx / head_dim) % n_head;
  const int t = idx / (head_dim * n_head);
  // g arrives (d_inner, T) in ggml head order: row e + head_dim*h
  const float v = g[(size_t)(e + head_dim * h) + (size_t)t * head_dim * n_head];
  g1[idx] = lower_bound * dsigmoid(-v * decay_a[h]);
}

// ONE BLOCK PER HEAD, tokens looped INSIDE the block (the state is carried by the block, so the
// sequential dependency never leaves the SM). The 1/sqrt(dk) scale lands on o here, as in GLM.
__global__ void rda_recurrence_kernel(const float* __restrict__ q, const float* __restrict__ k,
                                       const float* __restrict__ v, const float* __restrict__ g1,
                                       const float* __restrict__ beta, float* __restrict__ state,
                                       int n_head, int head_dim, int T, float* __restrict__ out) {
  const int h = blockIdx.x;
  if (h >= n_head) return;
  const int dk = head_dim;
  const int tid = threadIdx.x;
  const int nt = blockDim.x;
  const float scale = rsqrtf((float)dk);

  float* S = state + (size_t)h * dk * dk;  // S[i*dk + j], row i = KEY axis
  extern __shared__ float smem[];          // u (dk) then delta (dk)
  float* s_u = smem;
  float* s_delta = smem + dk;

  for (int t = 0; t < T; ++t) {
    const float* qt = q + ((size_t)t * n_head + h) * dk;
    const float* kt = k + ((size_t)t * n_head + h) * dk;
    const float* vt = v + ((size_t)t * n_head + h) * dk;
    const float* gt = g1 + ((size_t)t * n_head + h) * dk;
    const float b = beta[(size_t)t * n_head + h];

    // the gate scales the KEY axis: S[i][:] *= exp(g1[i])
    for (int i = tid; i < dk; i += nt) {
      const float decay = __expf(gt[i]);
      for (int j = 0; j < dk; ++j) S[(size_t)i * dk + j] *= decay;
    }
    __syncthreads();

    // u[j] = sum_i S[i][j] * k[i]   (= (S^T k)[j])
    for (int j = tid; j < dk; j += nt) {
      float acc = 0.0f;
      for (int i = 0; i < dk; ++i) acc += S[(size_t)i * dk + j] * kt[i];
      s_u[j] = acc;
    }
    __syncthreads();

    // delta[j] = beta * (v[j] - u[j])
    for (int j = tid; j < dk; j += nt) s_delta[j] = b * (vt[j] - s_u[j]);
    __syncthreads();

    // S[i][j] += k[i] * delta[j]
    for (int idx = tid; idx < dk * dk; idx += nt) {
      const int i = idx / dk, j = idx % dk;
      S[(size_t)i * dk + j] += kt[i] * s_delta[j];
    }
    __syncthreads();

    // o[j] = (1/sqrt(dk)) * sum_i S[i][j] * q[i]
    for (int j = tid; j < dk; j += nt) {
      float acc = 0.0f;
      for (int i = 0; i < dk; ++i) acc += S[(size_t)i * dk + j] * qt[i];
      out[((size_t)t * n_head + h) * dk + j] = acc * scale;
    }
    __syncthreads();
  }
}

// one thread per (head, token): two passes over head_dim — the sum of squares for the RMS, then
// the normed-and-gated write.
__global__ void rda_out_gate_kernel(const float* __restrict__ o, const float* __restrict__ norm_w,
                                     const float* __restrict__ g2, int head_dim, int n_head, int T,
                                     float eps, float* __restrict__ gated) {
  const int idx = blockIdx.x * blockDim.x + threadIdx.x;
  if (idx >= n_head * T) return;
  const int h = idx % n_head;
  const int t = idx / n_head;
  const float* ot = o + ((size_t)t * n_head + h) * head_dim;
  const float* gt = g2 + ((size_t)t * n_head + h) * head_dim;

  float ss = 0.0f;
  for (int e = 0; e < head_dim; ++e) ss += ot[e] * ot[e];
  const float inv = rsqrtf(ss / (float)head_dim + eps);
  for (int e = 0; e < head_dim; ++e)
    gated[((size_t)t * n_head + h) * head_dim + e] = ot[e] * inv * norm_w[e] * dsigmoid(gt[e]);
}

}  // namespace

void rda_conv(const float* x_proj, const float* conv_w, float* conv_state, int d_inner, int d_conv,
              int T, float* out, void* stream) {
  if (d_inner <= 0 || d_conv <= 1 || T <= 0) return;
  if (d_conv > RDA_MAX_D_CONV + 1) {
    std::fprintf(stderr, "rda_conv: d_conv %d exceeds the kernel's %d\n", d_conv, RDA_MAX_D_CONV + 1);
    std::exit(1);
  }
  cudaStream_t st = (cudaStream_t)stream;
  const int total = d_inner * T;
  rda_conv_kernel<<<(total + RDA_THREADS - 1) / RDA_THREADS, RDA_THREADS, 0, st>>>(
      x_proj, conv_w, conv_state, d_inner, d_conv, T, out);
  rda_conv_slide_kernel<<<(d_inner + RDA_THREADS - 1) / RDA_THREADS, RDA_THREADS, 0, st>>>(
      x_proj, conv_state, d_inner, d_conv, T);
}

void rda_gate(const float* g, const float* decay_a, float lower_bound, int head_dim, int n_head,
              int T, float* g1, void* stream) {
  if (head_dim <= 0 || n_head <= 0 || T <= 0) return;
  const int total = head_dim * n_head * T;
  rda_gate_kernel<<<(total + RDA_THREADS - 1) / RDA_THREADS, RDA_THREADS, 0, (cudaStream_t)stream>>>(
      g, decay_a, lower_bound, head_dim, n_head, T, g1);
}

void rda_recurrence(const float* q, const float* k, const float* v, const float* g1,
                    const float* beta, float* state, int n_head, int head_dim, int T, float* out,
                    void* stream) {
  if (n_head <= 0 || head_dim <= 0 || T <= 0) return;
  if (head_dim > RDA_THREADS) {
    std::fprintf(stderr, "rda_recurrence: head_dim %d exceeds the kernel's %d threads\n", head_dim,
                 RDA_THREADS);
    std::exit(1);
  }
  const size_t smem = (size_t)2 * head_dim * sizeof(float);
  rda_recurrence_kernel<<<n_head, RDA_THREADS, smem, (cudaStream_t)stream>>>(q, k, v, g1, beta, state,
                                                                             n_head, head_dim, T, out);
}

void rda_out_gate(const float* o, const float* norm_w, const float* g2, int head_dim, int n_head,
                  int T, float eps, float* gated, void* stream) {
  if (head_dim <= 0 || n_head <= 0 || T <= 0) return;
  const int total = n_head * T;
  rda_out_gate_kernel<<<(total + RDA_THREADS - 1) / RDA_THREADS, RDA_THREADS, 0, (cudaStream_t)stream>>>(
      o, norm_w, g2, head_dim, n_head, T, eps, gated);
}

}  // namespace u::raon
