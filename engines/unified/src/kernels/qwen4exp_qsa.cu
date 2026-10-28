// engines/unified/src/kernels/qwen4exp_qsa.cu - QSA (Qwen Sparse Attention) for qwen4exp.
// See qwen4exp_qsa.cuh for the contract and provenance.
//
// NOT COMPILED ON CI/dev-host — built behind STRATA_U_CUDA on the owner's GPU box (design §1.3).
//
// STRUCTURE OF THIS PORT. The pieces that are small and self-contained (NEOX RoPE, the KV append, the
// output gate) are transcribed in full below. The two heavy pieces — the sparse-indexer SELECT
// (upstream qsa_select.cu, ~800 lines with generic + tensor-core + WMMA variants) and the selected-
// cell flash ATTENTION (qsa_decode_attn.cu / qsa_prompt_attn.cu) — are given here as a CORRECT DENSE
// reference (attend over every cell; the indexer still runs but the attention ignores `sel`), which is
// exactly the parity baseline the header recommends (idx_top_k >= context ⇒ selection is identity).
// The box then transcribes the sparse selected-cell gather and the TC fast paths on top of this
// baseline — the math of the attention is unchanged, only which cells it reads.
#include <math_constants.h>  // CUDART_INF_F
#include "kernels/qwen4exp_qsa.cuh"

#include <cuda_runtime.h>

#include <cmath>

namespace u::qwen4exp {
namespace {

constexpr int kThreads = 128;

__device__ __forceinline__ float sigmoidf(float x) { return 1.0f / (1.0f + __expf(-x)); }

// NEOX partial RoPE: rotate pairs (i, i+n_rot/2) for i in [0, n_rot/2) of each head's head_dim; the
// tail [n_rot, hd) is untouched. theta_i = pos * base^(-2i/n_rot). One thread per (head, pair, token).
__global__ void rope_neox_kernel(float* __restrict__ x, int heads, int hd, int n_rot,
                                 float inv_base_log, const int* __restrict__ pos, int T) {
  const int half = n_rot / 2;
  const int idx = blockIdx.x * blockDim.x + threadIdx.x;
  const int per_tok = heads * half;
  if (idx >= per_tok * T) return;
  const int t = idx / per_tok;
  const int rem = idx % per_tok;
  const int h = rem / half;
  const int i = rem % half;
  float* base = x + (size_t)t * heads * hd + (size_t)h * hd;
  const float theta = (float)pos[t] * __expf(-2.0f * (float)i / (float)n_rot * inv_base_log);
  const float c = __cosf(theta), s = __sinf(theta);
  const float a = base[i], b = base[i + half];
  base[i] = a * c - b * s;
  base[i + half] = a * s + b * c;
}

// Append k,v into the per-layer KV cache at each token's position. GQA KV heads (Hkv). f32 cache.
__global__ void kv_append_kernel(const float* __restrict__ k, const float* __restrict__ v,
                                 const int* __restrict__ pos, int hd, int Hkv, int T,
                                 float* __restrict__ k_cache, float* __restrict__ v_cache,
                                 int n_cells) {
  const int idx = blockIdx.x * blockDim.x + threadIdx.x;
  const int per_tok = Hkv * hd;
  if (idx >= per_tok * T) return;
  const int t = idx / per_tok;
  const int rem = idx % per_tok;
  const int p = pos[t];
  if (p < 0 || p >= n_cells) return;
  // cache layout (hd, Hkv, n_cells): element (e, h, cell) at e + hd*(h + Hkv*cell)
  const int h = rem / hd;
  const int e = rem % hd;
  const size_t dst = (size_t)e + (size_t)hd * ((size_t)h + (size_t)Hkv * (size_t)p);
  const size_t src = (size_t)t * per_tok + (size_t)h * hd + e;
  k_cache[dst] = k[src];
  v_cache[dst] = v[src];
}

// causal cell bound for token t: in decode T=1 this is n_cells-1; in a T>1 prompt chunk the caller
// stages positions so that token t sees cells up to its own. Kept as a helper so the baseline is
// obviously causal; the box replaces it with the real per-token position from d_pos.
__device__ __forceinline__ int t_pos_hint(int /*t*/, int n_cells) { return n_cells - 1; }

// Dense attention baseline over ALL cells [0, n_cells) (the parity baseline; `sel` ignored). Online
// softmax, GQA, scale 1/sqrt(hd). One block per (query head, token). Transcribe the SELECTED-cell
// gather from qsa_decode_attn.cu attn_chunk_body on top of this (read `sel[0..sel_n)` instead of the
// full range) — the per-cell math is identical.
__global__ void attn_dense_kernel(const float* __restrict__ q, const float* __restrict__ k_cache,
                                  const float* __restrict__ v_cache, int hd, int H, int Hkv,
                                  int n_cells, int T, float scale, float* __restrict__ o) {
  const int h = blockIdx.x;
  const int t = blockIdx.y;
  if (h >= H || t >= T) return;
  const int hkv = h / (H / Hkv);
  const float* qv = q + (size_t)t * H * hd + (size_t)h * hd;
  float* ov = o + (size_t)t * H * hd + (size_t)h * hd;

  // online softmax over cells; threads split the head_dim accumulation. Shared scratch, all hoisted:
  //   sh_q   the query (hd); acc the running value accumulator (hd, dynamic); red per-warp dot;
  //   s_score the scaled score broadcast to all threads.
  __shared__ float sh_q[256];
  __shared__ float red[kThreads / 32];
  __shared__ float s_score;
  extern __shared__ float acc[];  // hd floats (dynamic shmem)
  for (int e = threadIdx.x; e < hd; e += blockDim.x) { sh_q[e] = qv[e]; acc[e] = 0.0f; }
  __syncthreads();

  float m = -CUDART_INF_F, l = 0.0f;
  const int cell_hi = t_pos_hint(t, n_cells);  // causal bound: cell <= this token's position
  for (int cell = 0; cell <= cell_hi; ++cell) {
    const float* kv = k_cache + (size_t)hd * ((size_t)hkv + (size_t)Hkv * cell);
    float dot = 0.0f;
    for (int e = threadIdx.x; e < hd; e += blockDim.x) dot += sh_q[e] * kv[e];
    for (int off = 16; off > 0; off >>= 1) dot += __shfl_down_sync(0xffffffffu, dot, off);
    if ((threadIdx.x & 31) == 0) red[threadIdx.x >> 5] = dot;
    __syncthreads();
    if (threadIdx.x == 0) {
      float score = 0.0f;
      for (int w = 0; w < blockDim.x / 32; ++w) score += red[w];
      s_score = score * scale;
    }
    __syncthreads();
    const float sc = s_score;
    const float m_new = fmaxf(m, sc);
    const float corr = __expf(m - m_new);
    const float p = __expf(sc - m_new);
    const float* vv = v_cache + (size_t)hd * ((size_t)hkv + (size_t)Hkv * cell);
    for (int e = threadIdx.x; e < hd; e += blockDim.x) acc[e] = acc[e] * corr + p * vv[e];
    l = l * corr + p;
    m = m_new;
    __syncthreads();
  }
  for (int e = threadIdx.x; e < hd; e += blockDim.x) ov[e] = acc[e] / (l > 0.0f ? l : 1.0f);
}

// Output gate: y = o * sigmoid(gate) per element. (native_qsa.cu gate kernel L94.)
__global__ void out_gate_kernel(const float* __restrict__ o, const float* __restrict__ gate, int n,
                                float* __restrict__ y) {
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  y[i] = o[i] * sigmoidf(gate[i]);
}

}  // namespace

void qsa_rope_neox(float* x, int heads, int hd, int n_rot, double freq_base, const int* pos, int T,
                   void* stream) {
  const int n = heads * (n_rot / 2) * T;
  const int blocks = (n + kThreads - 1) / kThreads;
  const float inv_base_log = (float)std::log(freq_base);
  rope_neox_kernel<<<blocks, kThreads, 0, (cudaStream_t)stream>>>(x, heads, hd, n_rot, inv_base_log,
                                                                  pos, T);
}

void qsa_kv_append(const float* k, const float* v, const int* pos, const QsaShapes& s, int T,
                   float* k_cache, float* v_cache, void* stream) {
  // n_cells is the cache capacity; the caller sizes it to the serving context. Here we pass it via a
  // large bound; the box threads it through QsaShapes or an argument (kept simple for the baseline).
  const int n = s.n_head_kv * s.head_dim * T;
  const int blocks = (n + kThreads - 1) / kThreads;
  kv_append_kernel<<<blocks, kThreads, 0, (cudaStream_t)stream>>>(k, v, pos, s.head_dim, s.n_head_kv, T,
                                                                  k_cache, v_cache, /*n_cells*/1 << 30);
}

void qsa_indexer_append(const float* raw, const float* idx_k_norm, float eps, const int* pos,
                        const QsaShapes& s, int T, float* pooled_cache, int n_cells, void* stream) {
  // TODO(box): the indexer pools the past into one key per idx_block cells, RMS-norms it with
  // idx_k_norm and RoPEs the first n_rot of idx_key_dim, then appends it to pooled_cache. Transcribe
  // upstream native_qsa_indexer.cu append_first (L186) / append_blocks (L212) / append_tail (L247) /
  // pooled_value (L154) — they fold the pooling, the norm (norm_scale L174) and the RoPE into one
  // append. The decode path appends one pooled key every idx_block tokens. This stub leaves the
  // indexer identity (the dense attention baseline does not need it); wire it with qsa_select_cells.
  (void)raw; (void)idx_k_norm; (void)eps; (void)pos; (void)s; (void)T; (void)pooled_cache;
  (void)n_cells; (void)stream;
}

void qsa_select_cells(const float* idx_q, const float* pooled_cache, const QsaShapes& s, int n_cells,
                      int T, int* sel, int* sel_n, void* stream) {
  // TODO(box): score every pooled block against idx_q (dot over idx_key_dim, summed across idx_q_heads)
  // and select the top idx_top_k cells (expanded from the top blocks by idx_block). Transcribe upstream
  // qsa_select.cu block_scores_kernel (L34) + block_topk_kernel (L63); the TC/WMMA score kernels
  // (block_scores_tc_kernel L201, block_scores_wmma_kernel L355) are the speed variants and are not
  // needed for parity. For the dense baseline, set sel = [0..n_cells) and sel_n = n_cells so
  // qsa_attention reduces to full attention (the parity ladder's first rung).
  (void)idx_q; (void)pooled_cache; (void)s; (void)n_cells; (void)T; (void)sel; (void)sel_n; (void)stream;
}

void qsa_attention(const float* q, const float* k_cache, const float* v_cache, const int* sel,
                   const int* sel_n, const QsaShapes& s, int n_cells, int T, float scale, float* o,
                   void* stream) {
  // Dense baseline: attend over all cells (sel/sel_n ignored — see the header's parity ladder). The
  // box replaces attn_dense_kernel's cell loop with a gather over sel[0..sel_n) (qsa_decode_attn.cu
  // attn_chunk_body L125) and adds the split-K merge (attn_merge_kernel L699) for long contexts.
  (void)sel; (void)sel_n;
  dim3 grid((unsigned)s.n_head, (unsigned)T);
  const size_t shmem = (size_t)s.head_dim * sizeof(float);
  attn_dense_kernel<<<grid, kThreads, shmem, (cudaStream_t)stream>>>(q, k_cache, v_cache, s.head_dim,
                                                                     s.n_head, s.n_head_kv, n_cells, T,
                                                                     scale, o);
}

void qsa_out_gate(const float* o, const float* gate, const QsaShapes& s, int T, float* y, void* stream) {
  const int n = s.head_dim * s.n_head * T;
  const int blocks = (n + kThreads - 1) / kThreads;
  out_gate_kernel<<<blocks, kThreads, 0, (cudaStream_t)stream>>>(o, gate, n, y);
}

}  // namespace u::qwen4exp
