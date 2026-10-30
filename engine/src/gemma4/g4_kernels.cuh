// strata-gemma4-engine: the CUDA kernels (plan Task 5) and their host-side launchers.
// Weights are Q8_0 repacked into an int8 plane and an fp16 scale plane (one scale per 32
// values of a row), BF16 or F32. Activations are f32; the KV caches are f16, laid out
// [kv_head][slot][head_dim]. Every launcher runs on the given stream and never synchronises.
#pragma once

#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

namespace g4k {

enum class WKind : int { q8 = 0, bf16 = 1, f32 = 2 };

// One weight matrix: rows outputs of cols inputs. q8: w = int8[rows*cols], s = half[rows*cols/32];
// bf16: w = bf16[rows*cols]; f32: w = float[rows*cols]. The pointers may be device memory or
// mapped pinned host memory (the GPU reads it over PCIe).
struct Mat {
  WKind kind;
  int rows;
  int cols;
  const void* w;
  const __half* s;
};

// One expert's two matrices (gate_up [1408 x 2816], down [2816 x 704]) as seen by the GPU.
struct ExpertPtr {
  const void* gu_w;
  const __half* gu_s;
  const void* dn_w;
  const __half* dn_s;
};

// y[t][rows] = W x[t] for t < T (T small; one pass over W per token).
void matvec(const Mat& m, const float* x, float* y, int T, cudaStream_t st);

// Grouped by expert: for k < n_pick, y[k*rows ..] = W_{e_k} x[k*x_stride ..], e_k = picks[k];
// gate_up (which=0) or down (which=1) of table[e_k]. kind and shape are the layer's.
void matvec_experts(WKind kind, int which, int rows, int cols, const ExpertPtr* table, const int* picks, int n_pick,
                    const float* x, int x_stride, float* y, cudaStream_t st);

// out[r][i] = x[r][i] * rsqrt(mean(x[r]^2) + eps) * (w ? w[i] : 1) * scale * (w2 ? w2[i] : 1).
void rmsnorm(const float* x, const float* w, const float* w2, float scale, float* out, int rows, int n, float eps,
             cudaStream_t st);

// out[r] = x[r] + rms(y[r]) * w (the sandwich norm after attention).
void add_rmsnorm(const float* x, const float* y, const float* w, float* out, int rows, int n, float eps, cudaStream_t st);

// The layer's end (spec Â§2.3): d = rms(dense)*w1, m = rms(moe)*w2, f = rms(d+m)*w3,
// x_out = (x1 + f) * (*layer_scale, or 1 when null).
void ffn_end(const float* x1, const float* dense, const float* moe, const float* w1, const float* w2, const float* w3,
             const float* layer_scale, float* x_out, int rows, int n, float eps, cudaStream_t st);

// Token embedding rows (Q8_0 planes) times `scale`.
void embed_q8(const int* ids, int T, const int8_t* q, const __half* s, int n, float scale, float* out, cudaStream_t st);

// q/k norms (weighted), v rms (unweighted), NEOX RoPE over the whole head (freq factors
// for full layers, may be null), then q out (f32 [T][16][hd], and f16 when q16 != null) and
// the K/V cache rows: slot = pos (cap = context) or pos % ring (sliding).
struct QkvPost {
  const float* q_raw;  // [T][n_head*hd]
  const float* k_raw;  // [T][n_kv*hd]
  const float* v_raw;  // [T][n_kv*hd] (== k_raw on full layers)
  const float* q_norm;
  const float* k_norm;
  const float* freq_factors;  // [hd/2] or null
  float rope_base;
  int hd;
  int n_head;
  int n_kv;
  int pos0;
  int T;
  int ring;  // 0: full layer (slot = pos); else the ring rows
  int cap;   // rows per KV head in the cache
  float eps;
  float* q_out;
  __half* q16;
  __half* k_cache;
  __half* v_cache;
  float theta_scale{0.0f};  // set by qkv_post from rope_base and hd (callers leave it 0)
  const int* d_pos{nullptr};  // non-null: the first row's position is this device word, not pos0 (a captured decode graph)
};
void qkv_post(const QkvPost& p, cudaStream_t st);

// Decode attention for one query position `pos` (scale 1.0): keys [max(0, pos-window+1), pos]
// (window 0 = all), slot = p or p % ring. out [n_head][hd]. scratch: n_head * max_splits * (hd + 2) floats.
void attn_decode(const float* q, const __half* k_cache, const __half* v_cache, int hd, int n_head, int n_kv, int cap,
                 int ring, int window, int pos, float* out, float* scratch, int max_splits, cudaStream_t st);
[[nodiscard]] int attn_decode_splits(int keys);
[[nodiscard]] int attn_decode_keys(int pos, int window);  // the keys attn_decode reads at `pos`
// attn_decode with the position in device memory (a captured decode graph): the same arithmetic.  `n_split` is
// attn_decode_splits(attn_decode_keys(pos, window)) for every position the launch serves (the graph's bucket).
void attn_decode_dev(const float* q, const __half* k_cache, const __half* v_cache, int hd, int n_head, int n_kv, int cap,
                     int ring, int window, const int* d_pos, int n_split, float* out, float* scratch, int max_splits,
                     cudaStream_t st);

// Prompt attention softmax: s is [G][Ts][L] scores of queries at qpos0 + t (t < Ts) against L
// key slots; p gets the masked softmax as f16. Slot positions: full (ring 0): slot index,
// valid when <= qpos; sliding: the latest position <= last_pos congruent to the slot
// mod ring, valid when within the window. Invalid keys get 0.
void attn_softmax(const float* s, __half* p, int G, int Ts, int L, int qpos0, int ring, int window, int last_pos,
                  cudaStream_t st);

// Router: probs = softmax(logits[t][128]); picks = top-8 of probs, weights = their probs
// renormalised (sum clamped to >= 6.1e-5). counts (may be null) += 1 per pick.
void router_topk(const float* logits, int T, int* picks, float* weights, int* counts, cudaStream_t st);

// m[r][i] = gelu_tanh(g[r][i]) * u[r][i] with g at g + r*g_stride, u at u + r*u_stride.
void geglu(const float* g, int g_stride, const float* u, int u_stride, float* m, int rows, int n, cudaStream_t st);

// moe[n] = sum_k y[k][n] * weights[k] * down_scale[picks[k]] (decode: one token, n_pick rows).
void moe_combine(const float* y, const float* weights, const int* picks, const float* down_scale, int n_pick, int n,
                 float* moe, cudaStream_t st);

// x = cap * tanh(x / cap).
void softcap(float* x, int n, float cap, cudaStream_t st);

// out[0] = argmax(x[0..n)) (ties: the lowest index), or -2 when the maximum is not finite.
// scratch: 2 * 1024 ints/floats.
void argmax(const float* x, int n, int* out, void* scratch, cudaStream_t st);

// The k highest of x[0..n) (k <= 64), highest first, ties by the lower index: vals[0..k),
// ids[0..k). *nonfinite = 1 when any x is NaN or infinite (else left as is). scratch: at least
// topk_scratch_bytes(n) bytes.
void topk(const float* x, int n, int k, float* vals, int* ids, int* nonfinite, void* scratch, cudaStream_t st);
[[nodiscard]] std::size_t topk_scratch_bytes(int n);
inline constexpr int kTopkMax = 64;

// out = dequantised W (f32 [rows][cols]).
void dequant(const Mat& m, float* out, cudaStream_t st);
// The same for one expert matrix of table[e] (which 0: gate_up, 1: down).
void dequant_expert(WKind kind, int which, int rows, int cols, const ExpertPtr* table, int e, float* out, cudaStream_t st);

// Prompt MoE helpers. gather: out[i] = x[order[i] / 8] (rows of n). scatter_combine:
// moe[t] = sum_k y[inv[t*8+k]] * weights[t*8+k] * down_scale[picks[t*8+k]].
void gather_rows(const float* x, const int* order, int count, int n, float* out, cudaStream_t st);
void scatter_combine(const float* y, const int* inv, const float* weights, const int* picks, const float* down_scale, int T,
                     int n, float* moe, cudaStream_t st);

// f32 -> f16 copy.
void to_half(const float* x, __half* out, long long n, cudaStream_t st);

// dst[index] = value, in stream order (the expert table's updates: the value travels as a kernel argument, so the
// host never stages it and never waits).
void set_u64(unsigned long long* dst, int index, unsigned long long value, cudaStream_t st);

// One decode step's expert plan for Strata's grouped expert kernel (kernels::native_expert_grouped): one group per
// pick (the router's top-k picks are distinct), group k's blob at ptrtab[picks[k]] (the expert's VRAM slot or its
// arena blob's device alias), its one entry token 0 writing row k.  *n_groups = n_pick; grp_start has n_pick + 1.
void plan_decode(const int* picks, int n_pick, const unsigned long long* ptrtab, unsigned long long* grp_ptr,
                 int* grp_start, int* n_groups, int* ent_dst, int* ent_tok, cudaStream_t st);

// ---- MTP (spec §12): the verify window and the drafter ----

// matvec for T <= kMatvecRowsMax rows that reads each weight row once for all of them; row t's arithmetic is matvec's
// on that row alone (the same chunks in the same order, the same reduction), so y[t] is bitwise matvec's.
inline constexpr int kMatvecRowsMax = 8;
void matvec_rows(const Mat& m, const float* x, float* y, int T, cudaStream_t st);

// A verify window's expert plan when no CPU lane serves it (every expert in VRAM, or --cpu-lane off): one group per
// distinct expert of the n_ent routed entries (n_ent <= 64), in the order the experts first appear, a group's entries
// ascending; group g's blob at ptrtab[its expert]; entry a writes row a and reads token a / 8.  *n_groups is written.
void plan_window(const int* picks, int n_ent, const unsigned long long* ptrtab, unsigned long long* grp_ptr, int* grp_start,
                 int* n_groups, int* ent_dst, int* ent_tok, cudaStream_t st);

// The routing counts of a window's kept rows: counts[l * 128 + picks[l * stride + a]] += 1 for a < n_ent, l < n_layers.
void count_picks(const int* picks, int stride, int n_layers, int n_ent, int* counts, cudaStream_t st);

// out[r] = (x[r] + rms(y[r]) * w) * (*scale): the drafter's layer end (its FFN sandwich norm, the residual and
// layer_output_scale, llama.cpp gemma4-assistant.cpp:175-181).
void add_rmsnorm_scale(const float* x, const float* y, const float* w, const float* scale, float* out, int rows, int n, float eps,
                       cudaStream_t st);

}  // namespace g4k
