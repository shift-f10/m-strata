// strata-unified-engine: the shared generic GPU kernels, namespace `u::k`.
//
// WHAT THIS IS. The one reviewed kernel set every family graph (glm5next, ds4,
// raon1, qwen35moe, qwen4exp, gemma4) calls for the pieces that are NOT family
// specific: quantized matvec (MMVQ decode, 1..8 columns), BF16/F32 GEMV and a
// cuBLAS prefill GEMM, RMSNorm, NEOX RoPE (table + a parameterized base/dim
// variant), SiLU/GELU/SwiGLU and small elementwise glue, a row softmax and an
// online-softmax attention split-K + merge, the MoE routers (softmax top-k and
// sigmoid) and the weighted MoE combine, and the sampler chain
// (greedy / top-k / top-p / min-p / penalties / seed). A family's own graph
// shape (KDA/MLA/GDN/DSA, draft blocks, sliding windows) is NOT here; it lives
// in src/family/<fam>.cu and is built on these.
//
// NON-BLOAT (design 2026-10-23, goal i). We port ONLY what we serve. The quant
// set is Q8_0 / Q4_K / Q5_K / Q6_K / BF16 / F32 -- not Maya's full 10+ types
// (Q2_0, Q3_K, Q4_0, Q5_0, IQ4_NL, IQ4_XS and the EXL3/iq_pack paths are not
// carried). Everything is NVIDIA sm_75+ (Turing and newer), so `__dp4a` is a
// hardware instruction and the sm_60 vmad fallback is not ported.
//
// PROVENANCE. Each kernel documents the Maya file:line it was ported from in
// common.cu. The quantized kernels are, through Maya, pinned to llama.cpp
// 3cf03257f219afbe7334045ff7c6a06ac68c627d (MIT); their block layouts and dot
// orders are byte-for-byte the pinned oracle's, so a quantized model's weights
// are read exactly as llama.cpp reads them.
//
// CONTRACT (shared by every launcher here). All pointers are DEVICE pointers,
// at least 4-byte aligned, non-overlapping unless a signature says otherwise.
// `stream` is a `cudaStream_t` passed as `void*`; a null stream means "use the
// default stream and synchronize before returning" (test/debug ergonomics),
// a non-null stream enqueues and returns. Shapes use GGUF order: `n_in` is the
// contiguous reduction dimension, `n_out` the weight-row count.
//
// This header is pure C++ (no CUDA headers) for the launcher declarations, so a
// host translation unit can include it; the device bit-conversion helpers below
// compile only under a CUDA/HIP compiler.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace u {
namespace k {

// ================================ quantization formats ================================
//
// The only formats the unified engine serves. The integer values are the stable
// GGML type ids, so a family can map a GGUF `general.file_type` / tensor type
// straight onto this enum without a ggml runtime dependency.
enum class Quant : int {
  f32 = 0,     // GGML_TYPE_F32
  bf16 = 30,   // GGML_TYPE_BF16
  q8_0 = 8,    // GGML_TYPE_Q8_0  (34 bytes / 32 elems)
  q4_k = 12,   // GGML_TYPE_Q4_K  (144 bytes / 256 elems)
  q5_k = 13,   // GGML_TYPE_Q5_K  (176 bytes / 256 elems)
  q6_k = 14,   // GGML_TYPE_Q6_K  (210 bytes / 256 elems)
  iq2_xxs = 16,  // GGML_TYPE_IQ2_XXS (66 bytes / 256 elems) -- GLM MoE experts, decode via u_iq.cu
  iq4_nl = 20,   // GGML_TYPE_IQ4_NL  (18 bytes / 32 elems)  -- qwen4exp down experts + dense
  iq3_s = 21,    // GGML_TYPE_IQ3_S   (110 bytes / 256 elems)-- qwen4exp gate/up experts (some layers)
  iq3_xxs = 18,  // GGML_TYPE_IQ3_XXS (98 bytes / 256 elems)
  iq2_s = 22,    // GGML_TYPE_IQ2_S   (82 bytes / 256 elems)
  iq4_xs = 23,   // GGML_TYPE_IQ4_XS  (136 bytes / 256 elems)-- qwen4exp dense/shexp + token_embd
  q2_0 = 42,     // Strata-custom     (18 bytes / 64 elems)  -- qwen4exp down experts (some layers)
  nvfp4 = 40,    // GGML_TYPE_NVFP4   (36 bytes / 64 elems)  -- flint native quant (4 UE4M3 sub-scale
                 // bytes + 32 nibble bytes; H1: id + size math, H2 host ref kernels/nvfp4_ref.hpp;
                 // the device dot lands with flint C1; spec 2026-10-25-flint-engine-design)
};

// True for the four quantized formats that go through the MMVQ path (not f32/bf16).
[[nodiscard]] bool is_quantized(Quant q) noexcept;
// Elements per block for `q` (32, 256, or 1 for f32/bf16). `n_in` must be a
// multiple of this.
[[nodiscard]] int block_elems(Quant q) noexcept;
// Bytes of a row-major weight matrix [n_out, n_in] stored in `q`.
[[nodiscard]] std::size_t weight_bytes(Quant q, int n_in, int n_out);

// ================================ MMVQ: quantized matvec / small-batch matmul ================================
//
// The decode-and-small-batch matmul: y[n_out, ncols] = W[n_out, n_in] . x[n_in, ncols],
// with W in a quantized format and x quantized to Q8_1 first. `ncols` is 1..8
// (decode = 1; speculative-verify and small prefill chunks use 2..8). Column j
// of x is contiguous at x + j*n_in and its Q8_1 blocks at j*(n_in/32); column j
// of y is y + j*n_out. Every column is BITWISE equal to a single-column call.
//
// For a long-prompt prefill, call this in column groups of <= 8; that is correct
// (just not the tensor-core MMQ speed -- see common.cu's note on what is left
// for the GPU box).

// Bytes of the Q8_1 activation scratch for `ncols` columns of width `n_in`.
[[nodiscard]] std::size_t q8_1_scratch_bytes(int n_in, int ncols = 1);

// Dequantize a contiguous Q6_K buffer of `n` elements (n % 256 == 0) to f32. Used for the GLM embedding
// gather (a quantized token_embd row -> f32), where a plain D2D copy does not apply. ggml-exact.
void dequant_q6k(const void* w, int n, float* out, void* stream);
// x (finite f32, `ncols` contiguous columns of `n_in`) -> Q8_1 blocks. One
// quantization serves every weight matrix sharing this x.
void quantize_q8_1(const float* x, void* x_q8_1, int n_in, int ncols, void* stream);

// W (quantized) . (already-Q8_1 x) -> y. `q` must be one of q8_0/q4_k/q5_k/q6_k.
void mmvq(Quant q, const void* weights, const void* x_q8_1, float* y,
          int n_in, int n_out, int ncols, void* stream);
// Convenience: quantize x to `scratch` (size q8_1_scratch_bytes) then mmvq.
void mmvq_f32(Quant q, const void* weights, const float* x, void* scratch_q8_1,
              float* y, int n_in, int n_out, int ncols, void* stream);

// ================================ BF16 / F32 GEMV and cuBLAS prefill ================================
//
// For unquantized weight matrices (BF16 or F32). GEMV is the decode path; the
// cuBLAS GEMM is the prefill path. BF16 is stored as the raw uint16 top-half of
// an f32 (see bf16_from_f32 below).

// y[n_out] = W[n_out, n_in] (bf16) . x[n_in] (bf16), fp32 accumulate.
void bf16_gemv(const uint16_t* x, const uint16_t* w, float* y,
               std::int64_t n_in, std::int64_t n_out, void* stream);
// y[c*n_out + r] = W[n_out, n_in] (bf16) . bf16(x[c*n_in ...]) for c < ncols (1..8): the f32 columns are rounded
// to bf16 in the kernel (bitwise the same as f32_to_bf16 + bf16_gemv's x), 16-byte weight loads shared by every
// column, fp32 accumulate. Every column is bitwise a single-column call (the speculative verify window's
// contract), though not bitwise bf16_gemv (its own reduction order). Needs n_in % 8 == 0 and 16-byte-aligned x
// and w; returns false (nothing launched) otherwise.
[[nodiscard]] bool bf16_gemv_f32x(const float* x, const uint16_t* w, float* y, std::int64_t n_in,
                                  std::int64_t n_out, int ncols, void* stream);
// y[n_out] = W[n_out, n_in] (f32) . x[n_in] (f32).
void f32_gemv(const float* x, const float* w, float* y,
              std::int64_t n_in, std::int64_t n_out, void* stream);

// Prefill GEMM via cuBLAS: C[m, n] = A[m, k] . B[k, n], row-major f32, computed
// as a single cublasSgemm. `handle` is a `cublasHandle_t` as `void*` (the engine
// owns one per device); pass the stream the handle is set to. For BF16 weights,
// `gemm_prefill_bf16` runs cublasGemmEx with CUDA_R_16BF inputs and an f32
// accumulator. These are the unquantized prefill path; quantized prefill goes
// through `mmvq` in column groups until the box adds tensor-core MMQ.
void gemm_prefill_f32(void* cublas_handle, const float* a, const float* b, float* c,
                      int m, int n, int kdim, void* stream);
void gemm_prefill_bf16(void* cublas_handle, const uint16_t* a, const uint16_t* b, float* c,
                       int m, int n, int kdim, void* stream);

// ================================ norms, activations, glue ================================

// RMSNorm per row, with an optional weight: r[c] = (w ? r[c]*w[c] : r[c]) * rsqrt(mean(r^2)+eps).
// `w` may be null. Rows are independent; `cols` is the row width.
void rms_norm(float* x, const float* w, std::int64_t rows, std::int64_t cols, float eps, void* stream);

// In place: x[i] = silu(x[i]) = x / (1 + exp(-x)). Double-precision inner math
// (matches the reference oracle in the last bits).
void silu_inplace(float* x, std::int64_t n, void* stream);
// In place: x[i] = gelu(x[i]) (the tanh approximation ggml uses).
void gelu_inplace(float* x, std::int64_t n, void* stream);
// Gated FFN activation: out[i] = silu(gate[i]) * up[i], over `n` elements. `out`
// may alias `gate`. This is the plain SwiGLU join (qwen families).
void swiglu(const float* gate, const float* up, float* out, std::int64_t n, void* stream);
// The GLM-family clamped SwiGLU (glm5next, raon1): out[i] = silu(min(gate[i],
// limit)) * clamp(up[i], -limit, +limit) -- the gate clamped ABOVE ONLY, the up
// on BOTH sides, precise expf. `limit` is the config's swiglu_limit (10.0 on the
// released GLM). ggml ggml_swiglu_clamp.
void swiglu_clamped(const float* gate, const float* up, float* out, std::int64_t n, float limit, void* stream);
// out[i] = gelu(gate[i]) * up[i] (gemma4's GeGLU dense MLP).
void geglu(const float* gate, const float* up, float* out, std::int64_t n, void* stream);

void scale_inplace(float* x, std::int64_t n, float s, void* stream);     // x *= s
void add_inplace(float* dst, const float* src, std::int64_t n, void* stream);  // dst += src
void f32_to_bf16(const float* x, uint16_t* y, std::int64_t n, void* stream);
void f32_to_f16(const float* x, uint16_t* y, std::int64_t n, void* stream);

// ================================ RoPE ================================
//
// NEOX partial rotary embedding (pairs (i, i+n_rot/2), only the first n_rot of
// head_dim are rotated). Two entry points:
//  * table form -- the caller builds a (max_pos, n_rot/2) cos/sin table on the
//    host with `build_rope_table` (float64 pow/cos/sin, the reference's values)
//    and the kernel is a pure rotation. Preferred where a cached table fits.
//  * base/dim form -- the kernel derives theta on device from `freq_base`; no
//    table, for families that vary base/dim per layer. One row = one head_dim
//    vector; `rows` = tokens*heads.
void build_rope_table(int n_rot, double theta, int max_pos, float* cos_tab, float* sin_tab);  // HOST
void rope_neox_table(const float* x, float* out, std::int64_t rows, int head_dim, int n_rot,
                     const float* cos_tab, const float* sin_tab, const int* pos, void* stream);
void rope_neox_base(const float* x, float* out, std::int64_t rows, int head_dim, int n_rot,
                    float freq_base, const int* pos, void* stream);

// ================================ softmax + attention ================================

// Row-wise softmax in place over `cols`, per row (numerically stable). Used for
// router logits, attention scores, and the head.
void softmax_rows(float* x, std::int64_t rows, std::int64_t cols, void* stream);

// Online-softmax attention, decode (one query row per head). For head h:
//   out[h, :hd] = sum_t softmax_t(scale * q.k_t + mask_t) * v_t
// over `ctx` keys/values. `q` is [heads, hd]; `k`,`v` are [ctx, kv_heads, hd]
// (kv head = h / (heads/kv_heads), i.e. GQA). `mask` (optional) is [ctx] added
// to the score (0 for attend, -inf for masked). This is the generic flash-decode
// the family attentions reuse when their layout matches; MLA/KDA/DSA keep their
// own specialized paths.
void attn_decode(const float* q, const float* k, const float* v, const float* mask,
                 float* out, int heads, int kv_heads, int head_dim, int ctx, float scale,
                 void* stream);
// attn_decode (no mask) with the context length read from the DEVICE: ctx = d_pos[0] + 1. Its launch
// arguments are the same every token, so a decode step that uses it is CUDA-graph capturable.
void attn_decode_devlen(const float* q, const float* k, const float* v, float* out, int heads, int kv_heads,
                        int head_dim, const int* d_pos, float scale, void* stream);

// Split-K partials + merge, for very long contexts: each of `splits` blocks
// attends over its slice of the context and writes a partial (running max `m`,
// running sum `l`, and the unnormalized accumulator `acc[hd]`) into
// `partials` (layout per head: [splits] of {m, l, acc[hd]}); `attn_merge`
// combines them into the normalized `out[heads, hd]`. The online-softmax rescale
// is `exp(m_i - m)`, as in `attn_decode`.
void attn_decode_split(const float* q, const float* k, const float* v, const float* mask,
                       float* partials, int heads, int kv_heads, int head_dim, int ctx,
                       float scale, int splits, void* stream);
void attn_merge(const float* partials, float* out, int heads, int head_dim, int splits, void* stream);

// ================================ MoE routers + combine ================================

// Softmax top-k router (the dense-probability form): p = softmax(logits over
// all experts); ids = stable argsort(-p)[:k] (ties by ascending index);
// weights = gather(p, ids) / max(sum, 2^-14). `logits` is [n_tokens, n_expert];
// `ids`/`weights` are [n_tokens, k]. k <= 64, n_expert <= 32768.
void router_topk(const float* logits, int n_tokens, int n_expert, int k,
                 int* ids, float* weights, void* stream);

// Sigmoid router (glm5next / raon1 / qwen35moe): p = sigmoid(logits);
// sel = p + bias (bias is a per-layer [n_expert] vector, applied to the SELECTION
// only); ids = top-k of sel (stable, ties by index); weights = gather of the
// UNBIASED p in rank order, * w_scale, optionally renormalized to sum 1.
// `bias` may be null. k <= 64, n_expert <= 512.
void router_sigmoid(const float* logits, const float* bias, int n_tokens, int n_expert, int k,
                    float w_scale, bool norm_w, int* ids, float* weights, void* stream);

// Weighted MoE combine: output[n_embd] = sum_{i<k} parts[i, :] * weights[i]
// (+ shared[:] when given). `parts` is [k, n_embd], `weights` [k], 1 <= k <= 64.
// `_multi` combines `n_tok` tokens at once (parts [n_tok, k, n_embd]).
void moe_combine(const float* parts, const float* weights, const float* shared,
                 float* output, std::int64_t n_embd, std::int64_t k, void* stream);
void moe_combine_multi(const float* parts, const float* weights, const float* shared,
                       float* output, std::int64_t n_embd, std::int64_t k, int n_tok, void* stream);

// ================================ sampler ================================
//
// llama.cpp's default chain, in order: penalties -> top_k -> top_p -> min_p ->
// temperature -> pick. The order IS the content (Maya sampler.cu header, issue
// #53): one penalties stage, first; temperature AFTER the truncation filters;
// top_p cuts before min_p. Greedy (temperature <= 0 or `greedy`) is the argmax,
// ties to the lowest index.
struct SamplerParams {
  int top_k = 20;             // 1..64 as given; 0 or >64 keep the widest list (64)
  float top_p = 0.95f;        // 1.0 disables
  float min_p = 0.0f;         // 0 disables; keeps p >= min_p * p_max
  float temperature = 1.0f;   // <= 0 means greedy
  int min_keep = 1;           // top_p keeps at least this many
  int penalty_last_n = 0;     // 0 disables; window over `history`
  float penalty_repeat = 1.0f;
  float penalty_freq = 0.0f;
  float penalty_present = 0.0f;
  std::uint64_t seed = 0;     // drives Philox 4x32-10 (counter-based on (seed, index))
  std::uint64_t counter = 0;  // absolute draw index of row 0; advance across decode calls
  bool greedy = false;
  const float* logit_bias = nullptr;  // optional [n_vocab] device array added to every row
};

// logits [n_tokens, n_vocab] (DEVICE) -> one token id per row in `out` (DEVICE).
// `history` is [n_tokens, history_len] int32 (the tail is the penalty window),
// or null with history_len 0 when no penalties apply.
void sample_tokens(const float* logits, int n_tokens, int n_vocab, const int* history,
                   int history_len, const SamplerParams& p, int* out, void* stream);

// ================================ device bit-conversion helpers ================================
//
// Ported from Maya's bf16_bits.hpp / f16_bits.hpp. bf16 is the top half of an
// f32; the forward direction rounds to nearest even with the tie on bit 16 (the
// lowest KEPT bit). Available on host and device so a table build and a kernel
// agree bit for bit.
#if defined(__CUDACC__) || defined(__HIPCC__)
#define U_K_HD __host__ __device__
#else
#define U_K_HD
#endif

U_K_HD inline uint16_t bf16_from_f32(float f) {
  uint32_t i;
  std::memcpy(&i, &f, 4);
  i = (i + ((i >> 16) & 1u) + 0x7FFFu) & 0xFFFF0000u;
  return static_cast<uint16_t>(i >> 16);
}
U_K_HD inline float f32_from_bf16(uint16_t h) {
  const uint32_t i = static_cast<uint32_t>(h) << 16;
  float f;
  std::memcpy(&f, &i, 4);
  return f;
}

#undef U_K_HD

}  // namespace k
}  // namespace u
