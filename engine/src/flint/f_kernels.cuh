// multiengine_flint: the CUDA kernels and their host-side launchers - Gemma 4's (g4_kernels.cuh) where flint computes the
// same thing, flint's own where it differs (attention sinks, the per-layer RoPE, V without a norm, the sigmoid router,
// SwiGLU, the combine without a per-expert scale). Weights are Q8_0 repacked into an int8 plane and an fp16 scale plane
// (one scale per 32 values of a row), BF16 or F32. Activations are f32; the KV caches are f16, laid out
// [kv_head][slot][head_dim]. Every launcher runs on the given stream and never synchronises.
#pragma once

#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

namespace flk {

inline constexpr int kHead = 128;      // flint's head width
inline constexpr int kGroup = 8;       // query heads per KV head (32 / 4)
inline constexpr int kPicks = 6;       // routed experts a token
inline constexpr int kRouted = 256;    // the router's experts

enum class WKind : int { q8 = 0, bf16 = 1, f32 = 2 };

// One weight matrix: rows outputs of cols inputs. q8: w = int8[rows*cols], s = half[rows*cols/32]; bf16: w =
// bf16[rows*cols]; f32: w = float[rows*cols].
struct Mat {
  WKind kind;
  int rows;
  int cols;
  const void* w;
  const __half* s;
};

// y[t][rows] = W x[t] for t < T (T small; one pass over W per token).
void matvec(const Mat& m, const float* x, float* y, int T, cudaStream_t st);

// out[r][i] = x[r][i] * rsqrt(mean(x[r]^2) + eps) * w[i].
void rmsnorm(const float* x, const float* w, float* out, int rows, int n, float eps, cudaStream_t st);

// out[r][i] = x[r][i] + a[r][i] (+ b[r][i] when b is not null): the residual adds.
void add_rows(const float* x, const float* a, const float* b, float* out, int rows, int n, cudaStream_t st);

// Token embedding rows (a Q8_0 or BF16 table, unscaled).
void embed(const int* ids, int T, const Mat& table, float* out, cudaStream_t st);

// q and k: RMSNorm over the head (weighted), then NeoX RoPE (element i with i + 64) when `rope`; v: as it is. Then q
// out (f32 [T][32][128], and f16 when q16 != null) and the K/V cache rows: slot = pos (ring 0) or pos % ring.
struct QkvPost {
  const float* q_raw;  // [T][32*128]
  const float* k_raw;  // [T][4*128]
  const float* v_raw;  // [T][4*128]
  const float* q_norm;
  const float* k_norm;
  bool rope;
  float rope_base;
  int pos0;
  int T;
  int ring;  // 0: slot = pos; else the ring rows
  int cap;   // rows per KV head in the cache
  float eps;
  float* q_out;
  __half* q16;
  __half* k_cache;
  __half* v_cache;
  float theta_scale;  // set by qkv_post from rope_base (callers leave it 0)
  const int* d_pos;   // non-null: the first row's position is this device word, not pos0 (a captured decode graph)
};
void qkv_post(const QkvPost& p, cudaStream_t st);

// Decode attention for one query position: keys [max(0, pos - window + 1), pos] (window 0 = all), slot = p or p % ring;
// scores scaled by `scale`; each head's softmax also counts its sink logit (sinks[head], unscaled, no value), as
// llama.cpp's soft_max with sinks. out [32][128]. scratch: 32 * max_splits * (128 + 2) floats.
void attn_decode(const float* q, const __half* k_cache, const __half* v_cache, int cap, int ring, int window, int pos,
                 const float* sinks, float scale, float* out, float* scratch, int max_splits, cudaStream_t st);
[[nodiscard]] int attn_decode_splits(int keys);
[[nodiscard]] int attn_decode_keys(int pos, int window);
// attn_decode with the position in device memory (a captured decode graph); n_split as attn_decode_splits gives it for
// every position the launch serves.
void attn_decode_dev(const float* q, const __half* k_cache, const __half* v_cache, int cap, int ring, int window, const int* d_pos,
                     int n_split, const float* sinks, float scale, float* out, float* scratch, int max_splits, cudaStream_t st);

// Prompt attention softmax: s is [G][Ts][L] raw scores of queries at qpos0 + t (t < Ts) against L key slots (one KV
// head's G query heads, whose sinks are sinks[0..G)); p gets softmax(scale * s, with the sink) as f16. Slot positions:
// ring 0: the slot index, valid when <= qpos; else the latest position <= last_pos congruent to the slot mod ring,
// valid when within the window. Invalid keys get 0.
void attn_softmax(const float* s, __half* p, int G, int Ts, int L, int qpos0, int ring, int window, int last_pos, const float* sinks,
                  float scale, cudaStream_t st);

// Router: probs = sigmoid(logits[t][256]); picks = the 6 highest probs (ties: the lower id), weights = their probs
// renormalised (the sum clamped to >= 6.1035e-5, as llama.cpp) times w_scale. counts (may be null) += 1 per pick.
void router_topk(const float* logits, int T, int* picks, float* weights, int* counts, float w_scale, cudaStream_t st);

// m[r][i] = silu(g[r][i]) * u[r][i] with g at g + r*g_stride, u at u + r*u_stride.
void swiglu(const float* g, int g_stride, const float* u, int u_stride, float* m, int rows, int n, cudaStream_t st);

// moe[i] = sum_k y[k][i] * weights[k] (decode: one token, n_pick rows).
void moe_combine(const float* y, const float* weights, int n_pick, int n, float* moe, cudaStream_t st);
// moe[t][i] = sum_k y[inv[t*6+k]][i] * weights[t*6+k] (a prompt chunk's rows in any order).
void scatter_combine(const float* y, const int* inv, const float* weights, int T, int n, float* moe, cudaStream_t st);

// out[0] = argmax(x[0..n)) (ties: the lowest index), or -2 when the maximum is not finite. scratch: 2 * 1024 words.
void argmax(const float* x, int n, int* out, void* scratch, cudaStream_t st);

// The k highest of x[0..n) (k <= 64), highest first, ties by the lower index. *nonfinite = 1 when any x is NaN or
// infinite. scratch: at least topk_scratch_bytes(n) bytes.
void topk(const float* x, int n, int k, float* vals, int* ids, int* nonfinite, void* scratch, cudaStream_t st);
[[nodiscard]] std::size_t topk_scratch_bytes(int n);
inline constexpr int kTopkMax = 64;

// out = dequantised W (f32 [rows][cols]).
void dequant(const Mat& m, float* out, cudaStream_t st);

// f32 -> f16 copy.
void to_half(const float* x, __half* out, long long n, cudaStream_t st);

// dst[index] = value, in stream order.
void set_u64(unsigned long long* dst, int index, unsigned long long value, cudaStream_t st);

// One decode step's expert plan for Strata's grouped expert kernel (kernels::native_expert_grouped): one group per pick,
// group k's blob at ptrtab[picks[k]], its one entry token 0 writing row k.
void plan_decode(const int* picks, int n_pick, const unsigned long long* ptrtab, unsigned long long* grp_ptr, int* grp_start,
                 int* n_groups, int* ent_dst, int* ent_tok, cudaStream_t st);

}  // namespace flk
