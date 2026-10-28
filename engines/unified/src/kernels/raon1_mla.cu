// strata-unified-engine: Raon-1-mini Gated-MLA kernels. See raon1_mla.cuh for the GLM analogy,
// the three Raon differences (NoPE, dense, Raon dims) and the open TODOs. The attention kernel is
// glm_mla_attention_kernel (engines/maya-ref/src/kernels/cuda/glm_dsa.cu) rewritten as a DENSE
// causal pass with an online (streaming) softmax so it needs no O(context) score buffer.
//
// NOT COMPILED ON CI/dev-host: built only behind STRATA_U_CUDA on the owner's GPU box, and
// verified there against the real Raon-1-mini weights (design §1.3).
#include <math_constants.h>  // CUDART_INF_F
#include "raon1_mla.cuh"

#include <cuda_runtime.h>

namespace u::raon {
namespace {

constexpr int MLA_THREADS = 256;

__device__ __forceinline__ float dsigmoid(float x) { return 1.0f / (1.0f + __expf(-x)); }

// q_abs[c, h, t] = sum_e wk_b[c, e, h] * q[e, h, t]   (fold K's up-proj into the query; the latent
// space is kv_lora-wide). One thread per (c, h, t). wk_b layout [kv_lora, qk_nope, n_head].
__global__ void mla_absorb_q_kernel(const float* __restrict__ q, const float* __restrict__ wk_b,
                                     int kv_lora, int qk_nope, int n_head, int T,
                                     float* __restrict__ q_abs) {
  const int idx = blockIdx.x * blockDim.x + threadIdx.x;
  const int total = kv_lora * n_head * T;
  if (idx >= total) return;
  const int c = idx % kv_lora;
  const int h = (idx / kv_lora) % n_head;
  const int t = idx / (kv_lora * n_head);
  const float* qh = q + (size_t)qk_nope * h + (size_t)qk_nope * n_head * t;
  const float* wk = wk_b + (size_t)c + (size_t)kv_lora * (size_t)qk_nope * h;  // [c + kv_lora*(e + qk_nope*h)]
  float acc = 0.0f;
  for (int e = 0; e < qk_nope; ++e) acc += wk[(size_t)kv_lora * e] * qh[e];
  q_abs[idx] = acc;
}

// Dense causal MLA with ONLINE softmax: one block per (query, head). A single pass over the
// visible cells accumulates the softmax-weighted latent ctx (kv_lora wide) without materializing
// the per-cell probabilities — the flash-attention running-max/running-denom recurrence. Then the
// v-absorption out[v,h] = sum_c wv_b[c,v,h]*ctx[c]. Equivalent to glm_mla_attention_kernel over the
// full [0, pos] cell set.
__global__ void mla_attention_kernel(const float* __restrict__ q_abs, const float* __restrict__ latents,
                                      const int* __restrict__ pos, const float* __restrict__ wv_b,
                                      int kv_lora, int n_head, int v_head, int qk_nope, int T,
                                      float* __restrict__ out) {
  const int block = blockIdx.x;
  const int h = block % n_head;
  const int t = block / n_head;
  const int tid = threadIdx.x;
  const int nt = blockDim.x;
  const float scale = rsqrtf((float)qk_nope);
  const int last = pos[t];  // inclusive: cells [0, last] are visible to query t

  extern __shared__ float smem[];  // ctx accumulator (kv_lora) + 2 reduction scalars
  float* s_ctx = smem;             // running weighted sum of latents, rescaled on each new max
  float* s_scalar = smem + kv_lora;  // [0] running max m, [1] running denom l
  __shared__ float s_score;          // the current cell's score, computed on thread 0

  const float* qa = q_abs + (size_t)kv_lora * h + (size_t)kv_lora * n_head * t;

  for (int e = tid; e < kv_lora; e += nt) s_ctx[e] = 0.0f;
  if (tid == 0) {
    s_scalar[0] = -CUDART_INF_F;
    s_scalar[1] = 0.0f;
  }
  __syncthreads();

  // Online softmax: walk cells, one thread computes each cell's score (a kv_lora dot) and all
  // threads fold it into the shared running (max, denom, ctx). The flash-attention recurrence kept
  // exact (correctness-first, as GLM's single-thread softmax is); the box's fast path can tile this.
  for (int s = 0; s <= last; ++s) {
    const float* lat = latents + (size_t)kv_lora * s;
    if (tid == 0) {
      float dot = 0.0f;
      for (int e = 0; e < kv_lora; ++e) dot += qa[e] * lat[e];
      s_score = dot * scale;
    }
    __syncthreads();
    const float score = s_score;

    // new running max and the rescale factor for the existing accumulator (every thread reads the
    // same m_old = s_scalar[0] here; thread 0 must not overwrite it until all have read — hence the
    // barrier before the scalar update below)
    const float m_old = s_scalar[0];
    const float m_new = fmaxf(m_old, score);
    const float rescale = (m_old == -CUDART_INF_F) ? 0.0f : __expf(m_old - m_new);
    const float p = __expf(score - m_new);
    for (int e = tid; e < kv_lora; e += nt) s_ctx[e] = s_ctx[e] * rescale + p * lat[e];
    __syncthreads();  // all threads have read s_scalar[0] and rescaled s_ctx
    if (tid == 0) {
      s_scalar[1] = s_scalar[1] * rescale + p;
      s_scalar[0] = m_new;
    }
    __syncthreads();  // the updated running (max, denom) visible before the next cell
  }

  // normalize the accumulated ctx by the denom
  const float inv = 1.0f / s_scalar[1];
  for (int e = tid; e < kv_lora; e += nt) s_ctx[e] *= inv;
  __syncthreads();

  // out[v, h, t] = sum_c wv_b[c, v, h] * ctx[c]   (v-absorption; wv_b [kv_lora, v_head, n_head])
  for (int v = tid; v < v_head; v += nt) {
    float acc = 0.0f;
    for (int c = 0; c < kv_lora; ++c)
      acc += wv_b[(size_t)c + (size_t)kv_lora * v + (size_t)kv_lora * v_head * h] * s_ctx[c];
    out[(size_t)v + v_head * h + (size_t)v_head * n_head * t] = acc;
  }
}

__global__ void mla_out_gate_kernel(const float* __restrict__ out, const float* __restrict__ gate,
                                     int v_head, int n_head, int T, float* __restrict__ gated) {
  const int idx = blockIdx.x * blockDim.x + threadIdx.x;
  const int total = v_head * n_head * T;
  if (idx >= total) return;
  gated[idx] = out[idx] * dsigmoid(gate[idx]);
}

}  // namespace

void mla_absorb_q(const float* q, const float* wk_b, int kv_lora, int qk_nope, int n_head, int T,
                  float* q_abs, void* stream) {
  if (kv_lora <= 0 || qk_nope <= 0 || n_head <= 0 || T <= 0) return;
  const int total = kv_lora * n_head * T;
  mla_absorb_q_kernel<<<(total + MLA_THREADS - 1) / MLA_THREADS, MLA_THREADS, 0, (cudaStream_t)stream>>>(
      q, wk_b, kv_lora, qk_nope, n_head, T, q_abs);
}

void mla_attention(const float* q_abs, const float* latents, const int* pos, const float* wv_b,
                   int kv_lora, int n_head, int v_head, int qk_nope, int T, float* out, void* stream) {
  if (kv_lora <= 0 || n_head <= 0 || v_head <= 0 || T <= 0) return;
  const size_t smem = ((size_t)kv_lora + 2) * sizeof(float);
  mla_attention_kernel<<<T * n_head, MLA_THREADS, smem, (cudaStream_t)stream>>>(
      q_abs, latents, pos, wv_b, kv_lora, n_head, v_head, qk_nope, T, out);
}

void mla_out_gate(const float* out, const float* gate, int v_head, int n_head, int T, float* gated,
                  void* stream) {
  if (v_head <= 0 || n_head <= 0 || T <= 0) return;
  const int total = v_head * n_head * T;
  mla_out_gate_kernel<<<(total + MLA_THREADS - 1) / MLA_THREADS, MLA_THREADS, 0, (cudaStream_t)stream>>>(
      out, gate, v_head, n_head, T, gated);
}

}  // namespace u::raon
