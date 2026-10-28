// strata-unified-engine: the Gemma-4-26B-A4B (arch "gemma4") model graph — implementation.
//
// Gemma4 RE-EXPRESSED on the shared kernel set (owner decision 2; design §3.2/§3.5: unify gemma4,
// NO cuBLAS-module carve-out). The 26/26-verified standalone engine at engines/gemma4/ is the
// READ-ONLY port reference + llama.cpp-parity oracle; this file maps its forward onto
// kernels/common.cuh (u::k::) + u_experts.hpp (u::ExpertTier), keeping only the genuinely
// gemma4-specific kernels in src/kernels/gemma4_attn.* (dual-RoPE qkv_post, split-K windowed/full
// decode attention, prefill softmax) and src/kernels/gemma4_glue.* (logit soft-cap, scaled Q8_0
// embedding gather). The reference's cuBLAS-specific prefill code (the prompt projection GEMMs =
// dequant-to-f32 + cublasSgemm, and the strided-batched attention) IS now ported, as the BATCHED
// PREFILL fast-path (forward_batch + the file-local prefill_* helpers below), to close the ~2x prompt
// gap with the reference; the decode path (forward_one + the device-table MoE) is unchanged.
//
// NOT COMPILED ON CI/dev-host: the engine builds src/family/*.cu + src/kernels/*.cu only behind
// STRATA_U_CUDA on the owner's GPU box. On a host/CI build the factory is not linked and
// u::make_family_graph returns the host stub. There is NO GPU here; this is a faithful, structured,
// documented port for the box to compile and parity against engines/gemma4/.
//
// ============================================================================================
// ASSUMED SHARED API — the seams this lane does NOT own (kernels/common.cuh landed as T1.1,
// u_experts.hpp as T1.2). The signatures below are what this file CALLS; if the landed header
// differs, adjust the call sites here (the gemma4 math and the gemma4-specific kernels do not
// change). As of writing these match the landed headers; the two gemma4-only reconciliations are
// flagged [RECONCILE].
//
//   namespace u::k {   // kernels/common.cuh — the one shared generic kernel set
//     bool  is_quantized(Quant q);
//     // decode/small-batch quantized matvec: quantize x to Q8_1 (scratch) then W·x. ncols=1 here.
//     std::size_t q8_1_scratch_bytes(int n_in, int ncols);
//     void  mmvq_f32(Quant q, const void* W, const float* x, void* scratch_q8_1, float* y,
//                    int n_in, int n_out, int ncols, void* stream);
//     void  bf16_gemv(const uint16_t* x, const uint16_t* w, float* y, int64 n_in, int64 n_out, void*);
//     void  f32_gemv (const float* x, const float* w, float* y, int64 n_in, int64 n_out, void*);
//     void  f32_to_bf16(const float* x, uint16_t* y, int64 n, void*);
//     void  rms_norm(float* x /*in place*/, const float* w /*nullable*/, int64 rows, int64 cols,
//                    float eps, void*);
//     void  geglu(const float* gate, const float* up, float* out, int64 n, void*);  // gelu(gate)*up
//     void  scale_inplace(float* x, int64 n, float s, void*);
//     void  add_inplace(float* dst, const float* src, int64 n, void*);
//     // softmax top-k (dense-prob form): ids=argsort(-softmax(logits))[:k]; weights=p/max(sum,2^-14)
//     // — BIT-IDENTICAL to gemma4's router (g4 router_topk, incl the 6.1e-5 clamp).
//     void  router_topk(const float* logits, int n_tokens, int n_expert, int k, int* ids,
//                       float* weights, void*);
//     void  sample_tokens(const float* logits, int n_tokens, int n_vocab, const int* history,
//                         int history_len, const SamplerParams& p, int* out, void*);
//     struct SamplerParams { int top_k; float top_p,min_p,temperature; int min_keep; ... uint64 seed; };
//   }
//
//   class u::ExpertTier {   // the shared VRAM/RAM expert-tiering + expert-hit-profile facility (§3.5)
//     struct Config { int64 n_layers,n_expert,n_embd,n_ff; k::Quant gate,up,down; Act act; float
//                     act_limit; int64 gate_off,up_off,down_off,blob_bytes,vram_slots;
//                     bool per_layer_admission, has_shared_expert; };
//     enum class Act { swiglu, swiglu_clamped, geglu };   // gemma4 => geglu  (landed: supported)
//     bool open(const Config&, std::string&);
//     bool set_host_arena(const uint8_t* base, int64 bytes, std::string&);     // pinned RAM blobs
//     bool warm_start(const std::vector<std::pair<int32,int32>>& ranked, std::string&);  // §3.5
//     // out[n_embd] = sum_i weights[i] * down(geglu(gate·x, up·x))[ids[i]].  ids/weights are HOST
//     // arrays of length k; x,out are DEVICE [n_embd]. (Decode path; k<=64.)
//     bool compute(int64 layer, const int32* ids, const float* weights, const float* x, float* out,
//                  int64 k, void* stream, std::string&);
//     void note_routing(int64 layer, const int32* ids, int64 k);               // live-rebalance heat
//     int64 rebalance(float margin, void* stream, std::string&);               // live correction
//   };
//
//   // [RECONCILE 1 — ACTIVATION]: gemma4 experts are GeGLU (gelu(gate)*up), not SwiGLU. The landed
//   //   ExpertTier::compute already supports Act::geglu (u_experts.hpp:70, u_experts.cu:395 calls
//   //   k::geglu), so Config.act = Act::geglu is all that is needed. The DENSE MLP uses u::k::geglu
//   //   directly (common.cuh even names it "gemma4's GeGLU dense MLP"). RESOLVED — no box change.
//   // [RECONCILE 2 — PER-EXPERT DOWN SCALE]: gemma4 carries ffn_down_exps.scale[n_expert] (a scalar
//   //   per expert). The reference folds it into the combine: moe[n] = Σ y_k·w_k·down_scale[e_k]
//   //   (g4_engine.cu:548 moe_combine → g4_kernels.cu:482). ExpertTier::compute multiplies each
//   //   expert output by weights[i] AFTER the down projection, so folding down_scale into the HOST
//   //   weight array before compute — weights[i] *= down_scale[ids[i]] — is EXACT. Done in
//   //   forward_one below. If the box prefers the scale applied inside the tier, pass it through
//   //   Config instead; the math is identical.
//   // [BOX NOTE — MIXED EXPERT QUANT]: the 26B-A4B Q8_0 file has uniform Q8_0 experts (fits the one
//   //   Config.{gate,up,down} Quant). The Q8_K_XL variant has layer-29 BF16 down (g4_engine.cu:46);
//   //   the lean ExpertTier cut is quantized-only (u_experts.cu:388), so that variant is a documented
//   //   box extension (per-layer Config or a bf16 expert path), not this lane's concern.
//
// --------------------------------------------------------------------------------------------
// g4 -> shared/gemma4 MAPPING (piece by piece; reference lines are engines/gemma4/src/...):
//   embed (Q8_0 · sqrt(d))           -> u::gemma4::embed_q8                  g4_engine.cu:521 / g4_kernels.cu:207
//   q/k/v/o/gate/up/down/router mm   -> u::k::mmvq_f32 (Q8_0) | bf16_gemv|f32_gemv   g4_engine.cu:525-542 (g4k::matvec)
//   attn_norm/ffn_norm/pre_ffw_norm2 -> copy + u::k::rms_norm (in place)     g4_engine.cu:524,536,544 (g4k::rmsnorm)
//   qkv dual-RoPE + norm + cache     -> u::gemma4::qkv_post                  g4_engine.cu:528-531 / g4_kernels.cu:217
//   decode attention (win/full)      -> u::gemma4::attn_decode              g4_engine.cu:532 / g4_kernels.cu:276,357
//   post-attention sandwich          -> u::k::rms_norm + u::k::add_inplace   g4_engine.cu:535 (g4k::add_rmsnorm)
//   dense GeGLU MLP                  -> u::k::geglu (SHARED)                 g4_engine.cu:539 / g4_kernels.cu:470
//   router pre-norm (·gate_inp_scale·1/√d) -> rms_norm + scale_inplace       g4_engine.cu:541
//   softmax top-8 router             -> u::k::router_topk (SHARED)          g4_engine.cu:543 / g4_kernels.cu:413
//   128-expert top-8 MoE (GeGLU)     -> u::gemma4::Gemma4Experts (device table) g4_engine.cu:545-548
//   layer tail double-sandwich norm  -> rms_norm×3 + add + scale            g4_engine.cu:549 / g4_kernels.cu:183 (ffn_end)
//   final norm + tied Q8_0 head      -> rms_norm + mmvq_f32                 g4_engine.cu:510-512
//   logit soft-cap (cap 30)          -> u::gemma4::softcap                  g4_engine.cu:513 / g4_kernels.cu:486
//   sampling / greedy                -> u::k::sample_tokens (SHARED)        g4_engine.cu:697 (greedy/top_candidates)
//   expert placement / rebalance     -> u::ExpertTier warm_start+rebalance  g4_engine.cu:308,723 (now §3.5 shared)
//
// PREFILL (BATCHED): the reference batches the prompt through cuBLAS (forward_batch g4_engine.cu:635,
// prompt_attention :557, moe_batch :591) and is ~2x faster than a token-by-token prompt. That path is
// now ported here as Gemma4Graph::forward_batch + the file-local prefill_* helpers: the projections and
// the per-expert MoE run as dequant-to-f32 + cublasSgemm (prefill_gemm / prefill_moe), the attention as
// cublas strided-batched score/output GEMMs around u::gemma4::attn_softmax (prefill_attention), and the
// norms / GeGLU / layer-tail reuse the SAME shared kernels forward_one uses. Porting the reference's
// exact prompt math makes the batched prefill track the parity oracle. generate() consumes the BULK of
// the prompt in forward_batch chunks and the LAST <=4 tokens through forward_one, so the sampled logits
// come from the decode path that already holds token-for-token greedy parity (the reference's `process`
// strategy, g4_engine.cu:668). If the batched scratch does not fit in VRAM, prefill_ready_ stays false
// and the whole prompt falls back to forward_one — correct, just slower, all on the shared kernels.
// ============================================================================================
#include "gemma4_graph.hpp"

#include "kernels/common.cuh"
#include "kernels/gemma4_attn.cuh"
#include "kernels/gemma4_glue.cuh"
#include "kernels/gemma4_moe.cuh"
#include "u_protocol.hpp"

#include <cublas_v2.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <vector>

namespace u {
namespace {

// DEBUG (env STRATA_U_DUMP=<file>): append one device row [ne] of f32 to the file. Used to compare
// per-layer activations against engines/gemma4's --dump-layers for parity debugging. No-op unless set.
void debug_dump(const float* d_x, void* stream, int ne) {
  const char* path = std::getenv("STRATA_U_DUMP");
  if (path == nullptr) return;
  cudaStreamSynchronize(static_cast<cudaStream_t>(stream));
  std::vector<float> row(static_cast<std::size_t>(ne));
  if (cudaMemcpy(row.data(), d_x, static_cast<std::size_t>(ne) * sizeof(float), cudaMemcpyDeviceToHost) != cudaSuccess)
    return;
  std::FILE* f = std::fopen(path, "ab");
  if (f != nullptr) {
    std::fwrite(row.data(), sizeof(float), static_cast<std::size_t>(ne), f);
    std::fclose(f);
  }
}

using Clock = std::chrono::steady_clock;
inline double secs(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

// BATCHED PREFILL chunk: tokens consumed per forward_batch (the prefill scratch is sized for this). Kept
// <= the sliding window so the window*2 KV ring always holds a chunk's needed sliding keys (the ring is
// fixed at load; see the reference's ring = kWindow + max(chunk, kWindow), g4_engine.cu:377). generate()
// clamps the chunk to g_.window defensively; 256 <= 1024 for the 26B-A4B shape, so it never binds.
constexpr int kPrefillChunk = 256;

// One weight matrix as the shared kernels see it: a quant format and a DEVICE pointer, with the
// GGUF shape (n_in = contiguous reduction dim, n_out = row count). Replaces the reference's
// g4k::Mat (g4_kernels.cuh:21). Q8_0/Q4_K/Q5_K/Q6_K go through mmvq; bf16/f32 through the GEMVs.
struct WeightRef {
  k::Quant quant{k::Quant::q8_0};
  const void* w{nullptr};
  int n_in{0};
  int n_out{0};
};

// A GGUF value helper mirroring u_geometry.cpp.
bool int_key(const GgufFile& f, const std::string& key, std::int64_t& out) {
  const auto* v = f.find(key);
  if (v == nullptr) return false;
  switch (v->type) {
    case GgufType::u8: case GgufType::i8: case GgufType::u16: case GgufType::i16:
    case GgufType::u32: case GgufType::i32: case GgufType::u64: case GgufType::i64:
      out = v->integer;
      return true;
    default: return false;
  }
}
bool real_key(const GgufFile& f, const std::string& key, double& out) {
  const auto* v = f.find(key);
  if (v == nullptr || (v->type != GgufType::f32 && v->type != GgufType::f64)) return false;
  out = v->real;
  return std::isfinite(out);
}

}  // namespace

// Per-layer device descriptor. Weight pointers index the uploaded arena; the f16 KV caches are
// per layer (sliding layers ring, full layers linear). Mirrors g4_engine.cu DevLayer (71-92),
// with the expert blobs owned by u::gemma4::Gemma4Experts (the device-table MoE) instead of here.
struct Gemma4DevLayer {
  bool sliding{true};
  int hd{256};
  int n_kv{8};
  int cap{0};  // rows per KV head in the cache (ring for sliding, context for full)
  WeightRef q, k, v, o, gate, up, down, router;
  const float* attn_norm{};
  const float* q_norm{};
  const float* k_norm{};
  const float* post_attn_norm{};
  const float* ffn_norm{};
  const float* post_ffw_norm_1{};
  const float* gate_inp_scale{};
  const float* pre_ffw_norm_2{};
  const float* post_ffw_norm_2{};
  const float* post_ffw_norm{};
  float layer_scale{1.0f};              // layer_output_scale (1 when the tensor is absent)
  std::vector<float> down_scale;        // ffn_down_exps.scale[n_expert], read to host (RECONCILE 2)
  std::uint16_t* kc{};                  // [n_kv][cap][hd] f16
  std::uint16_t* vc{};                  // [n_kv][cap][hd] f16
};

// All CUDA state behind the PIMPL (Gemma4Device, forward-declared in the header).
struct Gemma4Device {
  cudaStream_t stream{};
  std::vector<Gemma4DevLayer> layers;

  // Embedding / head (tied Q8_0 token_embd): `embd_blocks` is the raw GGUF Q8_0 tensor for the
  // scaled embedding gather; `head` is the same tensor as a WeightRef for the tied output matvec.
  const void* embd_blocks{};
  WeightRef head{};
  const float* output_norm{};
  const float* rope_freqs{};  // [head_full/2] proportional-rope factors (full layers)

  gemma4::Gemma4Experts experts;  // ZERO-HOST-SYNC device-table MoE (replaces ExpertTier for gemma4)

  // scratch (device), all sized in allocate(); one prompt/decode token wide (lean baseline).
  int* ids_d{};
  int* pos_dev{};        // DEVICE query position for the captured decode graph (qkv_post + attn_decode read it)
  // ---- captured decode graph (one CUDA graph replayed per decode token; see generate()) ----
  int* tok_pinned{};               // pinned [1]: the next token id the graph's embed reads (host writes it)
  cudaGraph_t decode_graph{};      // captured embed->layers->head sequence (no rebalance, no sampler)
  cudaGraphExec_t decode_exec{};   // instantiated graph
  bool graph_ready{false};
  int graph_splits_cap{0};         // the fixed per-call attention split count the graph was captured with
  float *x{}, *x1{}, *h{}, *o{}, *dense{}, *rn{}, *e_in{}, *moe{};
  float *qraw{}, *qout{}, *attn{}, *kraw{}, *vraw{};
  float *g{}, *u{}, *m{}, *rl{};
  int* picks{};       // device [n_exp_used]
  float* wts{};       // device [n_exp_used]
  void* q8scratch{};  // Q8_1 activation scratch for mmvq_f32 (sized for the largest n_in)
  float* dscratch{};  // attn split partials: n_head * max_splits * (head_full+2)
  float* logits{};    // [n_vocab]
  float* xn{};        // [n_embd]
  int* sample_d{};    // [1] sampled id
  int max_splits{0};
  std::uint16_t* bf16_x{};  // device bf16 activation scratch for the bf16-weight matvec path [max n_in]

  // ---- BATCHED PREFILL state (the forward_batch fast-path; all null/0 when prefill_ready_ is false) ----
  // The reference keeps one cuBLAS handle (g4_engine.cu:142) for the prompt projection SGEMMs and the
  // strided-batched attention; the stream is bound once at load. The scratch below is the chunk-wide
  // (kPrefillChunk tokens) twin of the one-token decode scratch above, plus the MoE bucket buffers.
  cublasHandle_t blas{};
  float *xb{}, *x1b{}, *hb{}, *ob{}, *denseb{}, *rnb{}, *e_inb{}, *moeb{};  // [C][n_embd] residual lanes
  float *qrawb{}, *qoutb{}, *attnb{};   // [C][n_head*head_full] (packed at the layer's n_head*hd stride)
  float *krawb{}, *vrawb{};             // [C][max n_kv*hd]
  std::uint16_t* q16b{};                // [C][n_head*head_full] f16 queries for the attention score GEMM
  float *gb{}, *ub{}, *mb{};            // [C][n_ff] dense GeGLU lanes
  float* rlb{};                         // [C][n_expert] router logits
  int* picksb{};                        // [C*n_exp_used] router picks (device)
  float* wtsb{};                        // [C*n_exp_used] router weights (device)
  int* ids_db{};                        // [C] prompt token ids (device)
  // MoE-batch working set: A = C*n_exp_used routed rows.
  int *orderb{}, *invb{};               // [A] the bucket permutation and its inverse (device)
  float *xgb{}, *gub{}, *mxb{}, *ygb{}; // gathered e_in [A][n_embd], fused gate|up [A][2*n_ff_exp],
                                        // geglu [A][n_ff_exp], down [A][n_embd]
  float *w1{}, *w2{};                   // one dequantised expert matrix: [2*n_ff_exp][n_embd], [n_embd][n_ff_exp]
  float* wdq{};                         // one dequantised projection matrix [max n_out*n_in]
  float* s_buf{};                       // [s_floats] attention scores (f32)
  std::uint16_t* p_buf{};               // [s_floats] attention probabilities (f16)
  std::size_t s_floats{0};
  void* prefill_pinned{};               // the one pinned host block (ids_hb | picks_hb | order_hb)
  int* ids_hb{};                        // [C]  pinned
  int* picks_hb{};                      // [A]  pinned (the one D2H copy per layer, to bucket by expert)
  int* order_hb{};                      // [2A] pinned: order [A] then inv [A]
  std::vector<int> moe_count, moe_offset, moe_fill;  // host expert histogram / prefix offsets / fill cursors

  // Owned by load() (the loader hands us the read-only mmap; we own what we upload/copy):
  std::uint8_t* weight_arena{};  // device: every non-expert weight (one 256B-aligned cudaMalloc). The
                                 // experts are NOT here: Gemma4Experts repacks them into its own
                                 // cudaHostAllocMapped arena (+ VRAM slots for the hottest), reached
                                 // by the per-layer device ExpertPtr tables.

  ~Gemma4Device() {
    experts.close();
    for (auto& L : layers) {
      if (L.kc) cudaFree(L.kc);
      if (L.vc) cudaFree(L.vc);
    }
    if (decode_exec) cudaGraphExecDestroy(decode_exec);
    if (decode_graph) cudaGraphDestroy(decode_graph);
    if (tok_pinned) cudaFreeHost(tok_pinned);
    for (void* p : {(void*)ids_d, (void*)pos_dev, (void*)x, (void*)x1, (void*)h, (void*)o, (void*)dense, (void*)rn,
                    (void*)e_in, (void*)moe, (void*)qraw, (void*)qout, (void*)attn, (void*)kraw, (void*)vraw, (void*)g,
                    (void*)u, (void*)m, (void*)rl, (void*)picks, (void*)wts, q8scratch, (void*)dscratch, (void*)logits,
                    (void*)xn, (void*)sample_d, (void*)bf16_x}) {
      if (p) cudaFree(p);
    }
    free_prefill();
    if (blas) cublasDestroy(blas);
    // The device weight arena and the proportional-rope factors load() uploaded are freed here; the
    // mapped expert arena, VRAM slots and device tables are owned and freed by Gemma4Experts (close).
    if (weight_arena) cudaFree(weight_arena);
    if (rope_freqs) cudaFree(const_cast<float*>(rope_freqs));
    if (stream) cudaStreamDestroy(stream);
  }

  // Free every batched-prefill buffer and null it (safe to call twice: at the load fallback and in the
  // destructor). The cuBLAS handle and the one-token decode scratch are owned elsewhere.
  void free_prefill() {
    for (void* p : {(void*)xb, (void*)x1b, (void*)hb, (void*)ob, (void*)denseb, (void*)rnb, (void*)e_inb,
                    (void*)moeb, (void*)qrawb, (void*)qoutb, (void*)attnb, (void*)krawb, (void*)vrawb,
                    (void*)gb, (void*)ub, (void*)mb, (void*)rlb, (void*)wtsb, (void*)xgb, (void*)gub,
                    (void*)mxb, (void*)ygb, (void*)w1, (void*)w2, (void*)wdq, (void*)s_buf, (void*)q16b,
                    (void*)p_buf, (void*)ids_db, (void*)picksb, (void*)orderb, (void*)invb}) {
      if (p) cudaFree(p);
    }
    xb = x1b = hb = ob = denseb = rnb = e_inb = moeb = qrawb = qoutb = attnb = krawb = vrawb = gb = ub = mb =
        rlb = wtsb = xgb = gub = mxb = ygb = w1 = w2 = wdq = s_buf = nullptr;
    q16b = p_buf = nullptr;
    ids_db = picksb = orderb = invb = nullptr;
    if (prefill_pinned) cudaFreeHost(prefill_pinned);
    prefill_pinned = nullptr;
    ids_hb = picks_hb = order_hb = nullptr;
  }

  // Decode matvec: y[n_out] = W · x[n_in], re-expressing g4k::matvec (g4_kernels.cu:674) on the
  // shared kernels. Quantized weights go through mmvq_f32 (x quantized to Q8_1 in q8scratch);
  // bf16/f32 through the shared GEMVs.
  void matvec(const WeightRef& wm, const float* xin, float* y) {
    if (k::is_quantized(wm.quant)) {
      k::mmvq_f32(wm.quant, wm.w, xin, q8scratch, y, wm.n_in, wm.n_out, 1, stream);
    } else if (wm.quant == k::Quant::bf16) {
      // bf16 weights (the UD file's layer-1/29 attn+dense matrices): convert the f32 activation to
      // bf16 in the dedicated scratch, then the shared bf16 GEMV. (The shared bf16_gemv takes bf16
      // activations; this drops a few activation mantissa bits vs the reference's f32-against-bf16
      // dot — a documented parity point for the box, not a correctness bug.)
      k::f32_to_bf16(xin, bf16_x, wm.n_in, stream);
      k::bf16_gemv(bf16_x, reinterpret_cast<const std::uint16_t*>(wm.w), y, wm.n_in, wm.n_out, stream);
    } else {
      k::f32_gemv(xin, reinterpret_cast<const float*>(wm.w), y, wm.n_in, wm.n_out, stream);
    }
  }

  void copy(float* dst, const float* src, int n) {
    cudaMemcpyAsync(dst, src, static_cast<std::size_t>(n) * sizeof(float), cudaMemcpyDeviceToDevice, stream);
  }
  // Device-to-device copy of `n` floats (n can exceed INT_MAX / an int row count — the batched lanes).
  void copy_n(float* dst, const float* src, std::size_t n) {
    cudaMemcpyAsync(dst, src, n * sizeof(float), cudaMemcpyDeviceToDevice, stream);
  }
};

// ------------------------------------------------------------------------------------------------
// Geometry: the gemma4.* metadata beyond the common (guarded, pinned) u::Geometry. Mirrors
// g4_model.cpp check_model (64-190); fails closed on any missing/ill-typed required key.
// ------------------------------------------------------------------------------------------------
bool Gemma4Geometry::from_gguf(const GgufFile& file, const Geometry& common, Gemma4Geometry& out, std::string& err) {
  auto refuse = [&](const std::string& what) { err = "gemma4 geometry: " + what; return false; };

  // Common, pinned fields straight from the guarded geometry (u_geometry.cpp pins them to the
  // 26B-A4B shape before this runs).
  out.n_layers = (int)common.block_count;        // 30
  out.n_embd = (int)common.embedding_length;     // 2816
  out.n_head = (int)common.head_count;           // 16
  out.n_ff = (int)common.feed_forward_length;    // 2112
  out.n_ff_exp = (int)common.expert_feed_forward_length;  // 704
  out.n_expert = (int)common.expert_count;       // 128
  out.n_exp_used = (int)common.expert_used_count;  // 8
  out.n_vocab = (int)common.vocab_size;          // 262144
  out.head_full = (int)common.head_dim;          // key_length = 512
  out.context_train = common.context_length;
  out.rope_base_full = (float)common.rope_freq_base;  // rope.freq_base (full base)

  std::int64_t v = 0;
  double r = 0.0;
  const std::string a = "gemma4";

  // Two head widths and the KV split (g4_model.cpp:74-77,105-118). key_length_swa must be 256,
  // and the sliding/full KV head counts 8/2.
  if (!int_key(file, a + ".attention.key_length_swa", v)) return refuse("missing gemma4.attention.key_length_swa");
  out.head_swa = (int)v;
  if (!int_key(file, a + ".attention.sliding_window", v) || v < 1) return refuse("bad gemma4.attention.sliding_window");
  out.window = (int)v;

  // RoPE bases: full from the common geometry; swa from the gemma4-specific key (g4_model.cpp:91-94).
  if (!real_key(file, a + ".rope.freq_base_swa", r) || r <= 1.0) return refuse("bad gemma4.rope.freq_base_swa");
  out.rope_base_swa = (float)r;
  out.rope_freqs_len = out.head_full / 2;  // one factor per rotated pair of a 512-wide head

  // Norm epsilon + final logit soft-cap (g4_model.cpp:87-98).
  if (!real_key(file, a + ".attention.layer_norm_rms_epsilon", r) || r <= 0.0 || r > 1e-3)
    return refuse("bad gemma4.attention.layer_norm_rms_epsilon");
  out.rms_eps = (float)r;
  if (!real_key(file, a + ".final_logit_softcapping", r) || r <= 0.0 || r > 1000.0)
    return refuse("bad gemma4.final_logit_softcapping");
  out.softcap = (float)r;

  // Special-token ids (g4_model.cpp:120-137). turn_close is found by scanning the vocab for the
  // literal "<turn|>" (a deterministic lookup, not model-data behavior selection).
  std::int64_t eos = -1, bos = -1;
  if (!int_key(file, "tokenizer.ggml.eos_token_id", eos) || eos < 0 || eos >= out.n_vocab)
    return refuse("bad tokenizer.ggml.eos_token_id");
  if (!int_key(file, "tokenizer.ggml.bos_token_id", bos) || bos < 0 || bos >= out.n_vocab)
    return refuse("bad tokenizer.ggml.bos_token_id");
  out.eos_id = (std::int32_t)eos;
  out.bos_id = (std::int32_t)bos;
  const auto* tokens = file.find("tokenizer.ggml.tokens");
  if (tokens == nullptr || tokens->type != GgufType::array || tokens->element != GgufType::string ||
      (int)tokens->texts.size() != out.n_vocab) {
    return refuse("tokenizer.ggml.tokens must hold n_vocab tokens");
  }
  int turn_close = -1;
  for (std::size_t i = 0; i < tokens->texts.size(); ++i) {
    if (tokens->texts[i] != "<turn|>") continue;
    if (turn_close >= 0) return refuse("the <turn|> token appears twice");
    turn_close = (int)i;
  }
  if (turn_close < 0) return refuse("the vocabulary has no <turn|> token");
  out.turn_close_id = turn_close;

  // Per-layer sliding/full pattern + KV cross-check (g4_model.cpp:104-118). sliding_window_pattern
  // is a 30-bool array (true = sliding); head_count_kv a 30-int array that must be 8 on sliding
  // layers and 2 on full layers. The 26B-A4B shape is 25 sliding + 5 full.
  const auto* pattern = file.find(a + ".attention.sliding_window_pattern");
  const auto* kv = file.find(a + ".attention.head_count_kv");
  if (pattern == nullptr || pattern->type != GgufType::array || pattern->element != GgufType::boolean ||
      (int)pattern->integers.size() != out.n_layers) {
    return refuse("gemma4.attention.sliding_window_pattern must list n_layers bools");
  }
  if (kv == nullptr || kv->type != GgufType::array || (int)kv->integers.size() != out.n_layers) {
    return refuse("gemma4.attention.head_count_kv must list n_layers ints");
  }
  out.sliding.assign((std::size_t)out.n_layers, 0);
  int n_full = 0;
  for (int il = 0; il < out.n_layers; ++il) {
    const bool is_sliding = pattern->integers[(std::size_t)il] != 0;
    out.sliding[(std::size_t)il] = is_sliding ? 1 : 0;
    const int want_kv = is_sliding ? out.kv_swa : out.kv_full;
    if ((int)kv->integers[(std::size_t)il] != want_kv) return refuse("a layer's KV head count is not the 26B-A4B value");
    n_full += is_sliding ? 0 : 1;
  }
  // TODO(box): confirm n_full == 5 against the real file (the published 26B-A4B shape). Do NOT
  // hardcode which 5 — the pattern array decides. (void) keeps -Werror quiet if the check is
  // compiled out; the box turns it into a hard assert against the parity oracle.
  (void)n_full;
  return true;
}

// ------------------------------------------------------------------------------------------------
// Construction / load / lifetime.
// ------------------------------------------------------------------------------------------------
Gemma4Graph::Gemma4Graph() : dev_(std::make_unique<Gemma4Device>()) {}
Gemma4Graph::~Gemma4Graph() = default;

bool Gemma4Graph::load(const GgufFile& file, const std::uint8_t* mapped, std::string& err) {
  // The shared loader (u_main) memory-mapped the whole model and calls this once after construction;
  // `mapped` is the GGUF data-section base, so every GgufTensor::offset is relative to it. context_
  // and common_ were stashed by make_gemma4_graph. Mirrors GlmGraph::load (the working idiom here):
  // ONE 256B-aligned device arena for the non-expert weights (cudaMemcpy each tensor from `mapped +
  // offset`, quantized kept RAW for u::k::mmvq), then the experts repacked into Gemma4Experts' mapped
  // arena + VRAM slots (the device-table MoE). The forward_one/head/generate schedule below is fed by
  // exactly the device state built here.
  loaded_ = false;
  auto cu = [&](cudaError_t e, const char* what) -> bool {
    if (e != cudaSuccess) {
      err = std::string("gemma4 load: ") + what + ": " + cudaGetErrorString(e);
      return false;
    }
    return true;
  };

  // 1. gemma4.* metadata on top of the guarded common geometry: per-layer sliding pattern, the two
  //    head widths, the KV-head split, both RoPE bases, the soft-cap and the special tokens. Fails
  //    closed on a missing/ill-typed required key (and sets g_.rope_freqs_len = head_full/2 = 256).
  if (!Gemma4Geometry::from_gguf(file, common_, g_, err)) return false;

  // 2. The CUDA stream + the decode-attention split bound (dscratch is sized from it in step 7).
  if (!cu(cudaStreamCreate(&dev_->stream), "stream create")) return false;
  dev_->max_splits = gemma4::attn_decode_splits(static_cast<int>(context_));

  // 3. The device weight arena: every tensor EXCEPT the experts (host arena, step 5), the two small
  //    host-read scalars (ffn_down_exps.scale, layer_output_scale) and rope_freqs (step 6). Quantized
  //    tensors are kept raw (matvec dispatches them through u::k::mmvq_f32); F32 norms get their ptr.
  auto on_device = [](const std::string& n) -> bool {
    return n.find("ffn_gate_up_exps.weight") == std::string::npos &&
           n.find("ffn_down_exps.weight") == std::string::npos &&
           n.find("ffn_down_exps.scale") == std::string::npos &&
           n.find("layer_output_scale.weight") == std::string::npos && n != "rope_freqs.weight";
  };
  std::uint64_t arena = 0;
  for (const auto& t : file.tensors)
    if (on_device(t.name)) arena += (t.bytes + 255u) & ~std::uint64_t{255u};
  if (arena == 0) {
    err = "gemma4 load: the file carries no non-expert weights";
    return false;
  }
  if (!cu(cudaMalloc(reinterpret_cast<void**>(&dev_->weight_arena), arena), "weight arena cudaMalloc")) return false;
  std::map<std::string, const std::uint8_t*> dptr;
  std::uint64_t at = 0;
  for (const auto& t : file.tensors) {
    if (!on_device(t.name)) continue;
    if (!cu(cudaMemcpy(dev_->weight_arena + at, mapped + t.offset, t.bytes, cudaMemcpyHostToDevice), "weight upload"))
      return false;
    dptr[t.name] = dev_->weight_arena + at;
    at += (t.bytes + 255u) & ~std::uint64_t{255u};
  }

  auto nptr = [&](const std::string& name) -> const float* {
    auto it = dptr.find(name);
    return it == dptr.end() ? nullptr : reinterpret_cast<const float*>(it->second);
  };
  // n_in = ggml dims[0] (contiguous reduction dim); n_out = dims.back() (row count). Matches GLM.
  auto wref = [&](const std::string& name, WeightRef& out) -> bool {
    const GgufTensor* t = file.tensor(name);
    auto it = dptr.find(name);
    if (t == nullptr || it == dptr.end()) {
      err = "gemma4 load: missing tensor " + name;
      return false;
    }
    out.quant = static_cast<k::Quant>(static_cast<int>(t->type));  // TensorType ids == k::Quant ids
    out.w = it->second;
    out.n_in = static_cast<int>(t->dims.empty() ? 0 : t->dims.front());
    out.n_out = static_cast<int>(t->dims.size() >= 2 ? t->dims.back() : (t->dims.empty() ? 0 : t->dims.front()));
    return true;
  };

  // token_embd is the tied Q8_0 head: the SAME raw device bytes feed u::gemma4::embed_q8 (scaled
  // gather) and the tied-head matvec (u::k::mmvq over the Q8_0 blocks). output_norm is the final norm.
  const GgufTensor* te = file.tensor("token_embd.weight");
  if (te == nullptr || dptr.find("token_embd.weight") == dptr.end()) {
    err = "gemma4 load: missing tensor token_embd.weight";
    return false;
  }
  if (te->type != TensorType::q8_0) {
    err = "gemma4 load: token_embd.weight must be Q8_0 (the tied head's kernel)";
    return false;
  }
  dev_->embd_blocks = dptr["token_embd.weight"];
  dev_->head.quant = k::Quant::q8_0;
  dev_->head.w = dev_->embd_blocks;
  dev_->head.n_in = static_cast<int>(te->dims.front());   // n_embd
  dev_->head.n_out = static_cast<int>(te->dims.back());   // n_vocab
  dev_->output_norm = nptr("output_norm.weight");
  if (dev_->output_norm == nullptr) {
    err = "gemma4 load: missing tensor output_norm.weight";
    return false;
  }

  // 4. Per-layer weights + the f16 KV caches. cap: sliding = ring (window*2, the lean default the box
  //    tunes with the prompt chunk, cf. g4_engine.cu:377); full = the serving context. Full layers are
  //    V-FROM-K, so they carry no attn_v tensor (qkv_post derives V from the K projection).
  const int ring = g_.window * 2;
  int kvmax = 0;
  dev_->layers.resize(static_cast<std::size_t>(g_.n_layers));
  for (int il = 0; il < g_.n_layers; ++il) {
    Gemma4DevLayer& L = dev_->layers[static_cast<std::size_t>(il)];
    const std::string b = "blk." + std::to_string(il) + ".";
    L.sliding = g_.is_sliding(il);
    L.hd = g_.head_dim(il);
    L.n_kv = g_.kv_heads(il);
    kvmax = std::max(kvmax, L.n_kv * L.hd);

    if (!wref(b + "attn_q.weight", L.q) || !wref(b + "attn_k.weight", L.k) ||
        !wref(b + "attn_output.weight", L.o) || !wref(b + "ffn_gate.weight", L.gate) ||
        !wref(b + "ffn_up.weight", L.up) || !wref(b + "ffn_down.weight", L.down) ||
        !wref(b + "ffn_gate_inp.weight", L.router))  // router is F32 => matvec routes to f32_gemv
      return false;
    if (L.sliding && !wref(b + "attn_v.weight", L.v)) return false;

    L.attn_norm = nptr(b + "attn_norm.weight");
    L.q_norm = nptr(b + "attn_q_norm.weight");
    L.k_norm = nptr(b + "attn_k_norm.weight");
    L.post_attn_norm = nptr(b + "post_attention_norm.weight");
    L.ffn_norm = nptr(b + "ffn_norm.weight");
    L.post_ffw_norm_1 = nptr(b + "post_ffw_norm_1.weight");
    L.gate_inp_scale = nptr(b + "ffn_gate_inp.scale");
    L.pre_ffw_norm_2 = nptr(b + "pre_ffw_norm_2.weight");
    L.post_ffw_norm_2 = nptr(b + "post_ffw_norm_2.weight");
    L.post_ffw_norm = nptr(b + "post_ffw_norm.weight");
    if (L.attn_norm == nullptr || L.q_norm == nullptr || L.k_norm == nullptr || L.post_attn_norm == nullptr ||
        L.ffn_norm == nullptr || L.post_ffw_norm_1 == nullptr || L.gate_inp_scale == nullptr ||
        L.pre_ffw_norm_2 == nullptr || L.post_ffw_norm_2 == nullptr || L.post_ffw_norm == nullptr) {
      err = "gemma4 load: a required F32 norm vector is missing in " + b;
      return false;
    }

    // layer_output_scale (optional; host scalar straight from the mmap, 1.0 when absent).
    L.layer_scale = 1.0f;
    if (const GgufTensor* ls = file.tensor(b + "layer_output_scale.weight"))
      if (ls->bytes >= sizeof(float)) std::memcpy(&L.layer_scale, mapped + ls->offset, sizeof(float));

    // ffn_down_exps.scale -> host (handed to Gemma4Experts in step 5; uploaded to the device there and
    // applied on-device in moe_combine via the device picks).
    const GgufTensor* ds = file.tensor(b + "ffn_down_exps.scale");
    if (ds == nullptr || ds->bytes < static_cast<std::uint64_t>(g_.n_expert) * sizeof(float)) {
      err = "gemma4 load: missing/short tensor " + b + "ffn_down_exps.scale";
      return false;
    }
    L.down_scale.assign(static_cast<std::size_t>(g_.n_expert), 0.0f);
    std::memcpy(L.down_scale.data(), mapped + ds->offset, static_cast<std::size_t>(g_.n_expert) * sizeof(float));

    L.cap = L.sliding ? ring : static_cast<int>(context_);
    const std::size_t cache = static_cast<std::size_t>(L.n_kv) * static_cast<std::size_t>(L.cap) *
                              static_cast<std::size_t>(L.hd);
    if (!cu(cudaMalloc(reinterpret_cast<void**>(&L.kc), cache * sizeof(std::uint16_t)), "KV cache (kc)") ||
        !cu(cudaMalloc(reinterpret_cast<void**>(&L.vc), cache * sizeof(std::uint16_t)), "KV cache (vc)"))
      return false;
  }

  // 5. Experts -> the ZERO-HOST-SYNC device-table MoE (u::gemma4::Gemma4Experts, the port of the
  //    reference's placement). Each layer's experts are REPACKED at load from the mmap into a
  //    cudaHostAllocMapped arena in the oracle's blob layout (Q8_0 -> int8 plane + fp16 scale plane;
  //    BF16 raw) -- PER-LAYER quant, so the UD file's BF16 experts on layers 1/29 sit alongside the
  //    Q8_0 rest. load_layer takes the fused ffn_gate_up_exps base and the ffn_down_exps base; it
  //    treats gate|up as one [2*n_ff_exp, n_embd] matrix (the gate/up split is the strided GeGLU's
  //    job at matvec time), exactly like g4_engine.cu's load_experts.
  if (!dev_->experts.open(g_.n_layers, g_.n_expert, g_.n_embd, g_.n_ff_exp, g_.n_exp_used, err)) return false;
  for (int il = 0; il < g_.n_layers; ++il) {
    const std::string b = "blk." + std::to_string(il) + ".";
    const GgufTensor* gu = file.tensor(b + "ffn_gate_up_exps.weight");
    const GgufTensor* dn = file.tensor(b + "ffn_down_exps.weight");
    if (gu == nullptr || dn == nullptr) {
      err = "gemma4 load: missing expert tensor in " + b;
      return false;
    }
    // The fused ffn_gate_up_exps (one [2*n_ff_exp, n_embd] matrix per expert) and ffn_down_exps are
    // repacked from the mmap into Gemma4Experts' mapped arena in the reference's blob layout; the
    // gate|up split is handled by the strided GeGLU at matvec time, exactly like the oracle.
    Gemma4DevLayer& L = dev_->layers[static_cast<std::size_t>(il)];
    if (!dev_->experts.load_layer(il, mapped + gu->offset, static_cast<int>(gu->type), mapped + dn->offset,
                                  static_cast<int>(dn->type), L.down_scale.data(), err))
      return false;
  }

  // 6. rope_freqs (full-layer "proportional" factors, [head_full/2]). Read rope_freqs.weight [256] F32
  //    when the file carries it (exact parity with the reference, which reads this tensor); otherwise
  //    COMPUTE the published factors (design §2.4 / LC conversion/gemma.py:734-761): 64 ones then the
  //    rest 1e30 (128 rotated dims = 64 pairs; higher pairs frozen). The kernel uses
  //    theta_i = pos * base^(-2i/hd) / rope_freqs[i], so a 1e30 factor zeroes the rotation.
  {
    const int rlen = g_.rope_freqs_len;  // head_full/2 = 256
    std::vector<float> rope_h(static_cast<std::size_t>(rlen));
    const GgufTensor* rf = file.tensor("rope_freqs.weight");
    if (rf != nullptr && rf->type == TensorType::f32 &&
        rf->bytes >= static_cast<std::uint64_t>(rlen) * sizeof(float)) {
      std::memcpy(rope_h.data(), mapped + rf->offset, static_cast<std::size_t>(rlen) * sizeof(float));
    } else {
      const int rotated_pairs = 64;  // 128 rotated dims / 2 (the 26B-A4B proportional rope)
      for (int i = 0; i < rlen; ++i)
        rope_h[static_cast<std::size_t>(i)] = (i < rotated_pairs) ? 1.0f : 1e30f;
    }
    float* rope_dev = nullptr;
    if (!cu(cudaMalloc(reinterpret_cast<void**>(&rope_dev), static_cast<std::size_t>(rlen) * sizeof(float)),
            "rope_freqs cudaMalloc"))
      return false;
    if (!cu(cudaMemcpy(rope_dev, rope_h.data(), static_cast<std::size_t>(rlen) * sizeof(float),
                       cudaMemcpyHostToDevice),
            "rope_freqs upload")) {
      cudaFree(rope_dev);
      return false;
    }
    dev_->rope_freqs = rope_dev;
  }

  // 7. The per-token scratch (lean baseline: one token wide). Sizes from the device-struct contract;
  //    q8scratch covers the widest matvec reduction dim (the full-layer o-projection, n_head*head_full).
  const int ne = g_.n_embd, nff = g_.n_ff;
  const int nhf = g_.n_head * g_.head_full;
  const int max_n_in = std::max(std::max(ne, nff), nhf);
  auto fm = [&](float*& p, std::int64_t n, const char* what) -> bool {
    return cu(cudaMalloc(reinterpret_cast<void**>(&p), static_cast<std::size_t>(n) * sizeof(float)), what);
  };
  const bool scratch_ok =
      cu(cudaMalloc(reinterpret_cast<void**>(&dev_->ids_d), sizeof(int)), "ids_d") && fm(dev_->x, ne, "x") &&
      fm(dev_->x1, ne, "x1") && fm(dev_->h, ne, "h") && fm(dev_->o, ne, "o") && fm(dev_->dense, ne, "dense") &&
      fm(dev_->rn, ne, "rn") && fm(dev_->e_in, ne, "e_in") && fm(dev_->moe, ne, "moe") &&
      fm(dev_->qraw, nhf, "qraw") && fm(dev_->qout, nhf, "qout") && fm(dev_->attn, nhf, "attn") &&
      fm(dev_->kraw, kvmax, "kraw") && fm(dev_->vraw, kvmax, "vraw") && fm(dev_->g, nff, "g") &&
      fm(dev_->u, nff, "u") && fm(dev_->m, nff, "m") && fm(dev_->rl, g_.n_expert, "rl") &&
      cu(cudaMalloc(reinterpret_cast<void**>(&dev_->picks), static_cast<std::size_t>(g_.n_exp_used) * sizeof(int)),
         "picks") &&
      fm(dev_->wts, g_.n_exp_used, "wts") &&
      cu(cudaMalloc(&dev_->q8scratch, k::q8_1_scratch_bytes(max_n_in, 1)), "q8scratch") &&
      fm(dev_->dscratch, static_cast<std::int64_t>(g_.n_head) * dev_->max_splits * (g_.head_full + 2), "dscratch") &&
      fm(dev_->logits, g_.n_vocab, "logits") && fm(dev_->xn, ne, "xn") &&
      cu(cudaMalloc(reinterpret_cast<void**>(&dev_->sample_d), sizeof(int)), "sample_d") &&
      cu(cudaMalloc(reinterpret_cast<void**>(&dev_->bf16_x), static_cast<std::size_t>(max_n_in) * sizeof(std::uint16_t)),
         "bf16_x") &&
      cu(cudaMalloc(reinterpret_cast<void**>(&dev_->pos_dev), sizeof(int)), "pos_dev") &&
      cu(cudaHostAlloc(reinterpret_cast<void**>(&dev_->tok_pinned), sizeof(int), cudaHostAllocDefault), "tok_pinned");
  if (!scratch_ok) return false;

  // 7b. cuBLAS handle + the BATCHED PREFILL scratch (the forward_batch fast-path). The handle serves the
  //     prompt projection SGEMMs and the strided-batched attention (its stream is bound once here). The
  //     scratch is the chunk-wide (kPrefillChunk tokens) twin of the one-token decode scratch above, plus
  //     the MoE bucket buffers. BEST-EFFORT: if it does not fit, prefill_ready_ stays false and generate()
  //     consumes the prompt token-by-token through forward_one (correct, just slower), so a tight-VRAM box
  //     still serves. Allocated BEFORE the expert-residency budget (step 8) so that budget sees this VRAM.
  if (cublasCreate(&dev_->blas) != CUBLAS_STATUS_SUCCESS ||
      cublasSetStream(dev_->blas, dev_->stream) != CUBLAS_STATUS_SUCCESS) {
    err = "gemma4 load: cuBLAS handle create failed";
    return false;
  }
  {
    const int C = kPrefillChunk;
    const int A = C * g_.n_exp_used;
    const int gu_rows = 2 * g_.n_ff_exp;
    std::int64_t max_weight = 0;  // widest non-expert projection (n_out*n_in): the full-layer o-projection
    for (const auto& L : dev_->layers) {
      auto upd = [&](const WeightRef& w) {
        max_weight = std::max<std::int64_t>(max_weight, static_cast<std::int64_t>(w.n_out) * w.n_in);
      };
      upd(L.q); upd(L.k); upd(L.o); upd(L.gate); upd(L.up); upd(L.down); upd(L.router);
      if (L.sliding) upd(L.v);
    }
    dev_->s_floats = static_cast<std::size_t>(32) << 20;  // prompt-attention scores (reference sizing)
    dev_->moe_count.assign(static_cast<std::size_t>(g_.n_expert), 0);
    dev_->moe_offset.assign(static_cast<std::size_t>(g_.n_expert) + 1, 0);
    dev_->moe_fill.assign(static_cast<std::size_t>(g_.n_expert), 0);

    auto pf = [&](float*& ptr, std::int64_t n) -> bool {
      return cudaMalloc(reinterpret_cast<void**>(&ptr), static_cast<std::size_t>(n) * sizeof(float)) == cudaSuccess;
    };
    const std::int64_t Cne = static_cast<std::int64_t>(C) * ne;
    const std::int64_t Cnhf = static_cast<std::int64_t>(C) * nhf;
    const std::int64_t Ckv = static_cast<std::int64_t>(C) * kvmax;
    const std::int64_t Cnff = static_cast<std::int64_t>(C) * nff;
    const std::int64_t Ane = static_cast<std::int64_t>(A) * ne;
    bool pok =
        pf(dev_->xb, Cne) && pf(dev_->x1b, Cne) && pf(dev_->hb, Cne) && pf(dev_->ob, Cne) &&
        pf(dev_->denseb, Cne) && pf(dev_->rnb, Cne) && pf(dev_->e_inb, Cne) && pf(dev_->moeb, Cne) &&
        pf(dev_->qrawb, Cnhf) && pf(dev_->qoutb, Cnhf) && pf(dev_->attnb, Cnhf) &&
        pf(dev_->krawb, Ckv) && pf(dev_->vrawb, Ckv) &&
        pf(dev_->gb, Cnff) && pf(dev_->ub, Cnff) && pf(dev_->mb, Cnff) &&
        pf(dev_->rlb, static_cast<std::int64_t>(C) * g_.n_expert) && pf(dev_->wtsb, A) &&
        pf(dev_->xgb, Ane) && pf(dev_->gub, static_cast<std::int64_t>(A) * gu_rows) &&
        pf(dev_->mxb, static_cast<std::int64_t>(A) * g_.n_ff_exp) && pf(dev_->ygb, Ane) &&
        pf(dev_->w1, static_cast<std::int64_t>(gu_rows) * ne) &&
        pf(dev_->w2, static_cast<std::int64_t>(ne) * g_.n_ff_exp) && pf(dev_->wdq, max_weight) &&
        pf(dev_->s_buf, static_cast<std::int64_t>(dev_->s_floats));
    pok = pok &&
          cudaMalloc(reinterpret_cast<void**>(&dev_->q16b), static_cast<std::size_t>(Cnhf) * sizeof(std::uint16_t)) == cudaSuccess &&
          cudaMalloc(reinterpret_cast<void**>(&dev_->p_buf), dev_->s_floats * sizeof(std::uint16_t)) == cudaSuccess &&
          cudaMalloc(reinterpret_cast<void**>(&dev_->ids_db), static_cast<std::size_t>(C) * sizeof(int)) == cudaSuccess &&
          cudaMalloc(reinterpret_cast<void**>(&dev_->picksb), static_cast<std::size_t>(A) * sizeof(int)) == cudaSuccess &&
          cudaMalloc(reinterpret_cast<void**>(&dev_->orderb), static_cast<std::size_t>(A) * sizeof(int)) == cudaSuccess &&
          cudaMalloc(reinterpret_cast<void**>(&dev_->invb), static_cast<std::size_t>(A) * sizeof(int)) == cudaSuccess;
    void* pin = nullptr;
    if (pok && cudaHostAlloc(&pin, (static_cast<std::size_t>(C) + 3u * static_cast<std::size_t>(A)) * sizeof(int),
                             cudaHostAllocDefault) == cudaSuccess) {
      dev_->prefill_pinned = pin;
      dev_->ids_hb = static_cast<int*>(pin);
      dev_->picks_hb = dev_->ids_hb + C;
      dev_->order_hb = dev_->picks_hb + A;  // order [A] then inv [A]
      prefill_ready_ = true;
    } else {
      dev_->free_prefill();  // return the VRAM to the expert budget; generate() falls back to forward_one
      cudaGetLastError();    // clear the sticky OOM so later CUDA calls are not poisoned
      prefill_ready_ = false;
      std::fprintf(stderr, "gemma4 load: batched-prefill scratch did not fit; prefill runs token-by-token\n");
    }
  }

  // 8. Expert VRAM residency: keep the hottest S experts per layer in VRAM slots, read the rest from
  //    the mapped host arena over PCIe. Budget = most of the free VRAM left after the weights/KV/
  //    scratch (leave ~768 MB headroom). Cold start here (experts 0..S-1); forward_one's gated live
  //    rebalance converges the resident set to the hot experts. finalize is best-effort for the slots
  //    (a short VRAM alloc just runs fully mapped-host).
  {
    std::size_t vfree = 0, vtotal = 0;
    if (cudaMemGetInfo(&vfree, &vtotal) != cudaSuccess) vfree = 0;
    const std::size_t headroom = static_cast<std::size_t>(768ull << 20);
    const std::int64_t budget = vfree > headroom ? static_cast<std::int64_t>(vfree - headroom) : 0;
    if (!dev_->experts.finalize(budget, err)) return false;
  }

  // INSTRUMENTATION: the VRAM budget breakdown + resident experts/layer, for the old-vs-new comparison
  // (the reference prints "54 expert slots a layer (42.2% resident), VRAM 15844 of 16376 MiB"). No
  // behaviour change. non_expert_used = everything except the resident slot pool (weights, KV, the
  // prefill scratch, the CUDA context).
  {
    std::size_t vf = 0, vt = 0;
    cudaMemGetInfo(&vf, &vt);
    const std::int64_t resident = dev_->experts.resident_bytes();
    const double used = static_cast<double>(vt - vf);
    std::fprintf(stderr,
                 "gemma4 vram: engine=new total_mib=%.0f used_mib=%.0f expert_resident_mib=%.0f "
                 "non_expert_mib=%.0f slots_per_layer=%d resident_pct=%.1f prefill_ready=%d\n",
                 static_cast<double>(vt) / 1048576.0, used / 1048576.0,
                 static_cast<double>(resident) / 1048576.0, (used - static_cast<double>(resident)) / 1048576.0,
                 dev_->experts.slots_per_layer(),
                 100.0 * dev_->experts.slots_per_layer() / static_cast<double>(g_.n_expert),
                 prefill_ready_ ? 1 : 0);
  }

  pos_ = 0;
  loaded_ = true;
  return true;
}

void Gemma4Graph::reset_sequence() {
  pos_ = 0;
  // The KV caches are append-only, so position 0 alone restores them; no memset is needed (a full
  // layer overwrites slot = pos, a sliding layer slot = pos%ring). Mirrors g4 truncate(0)
  // (g4_engine.cu:103): the serve loop's reuse rule sets the kept prefix.
}

// ================================================================================================
// BATCHED PREFILL helpers (file-local). These are the cuBLAS-specific pieces the unified engine did not
// carry for the lean decode baseline: the dequant-to-f32 + cublasSgemm projection GEMMs, the cublas
// strided-batched attention around u::gemma4::attn_softmax, and the host-bucketed per-expert MoE SGEMM.
// Each is a faithful port of the reference engine's forward_batch path (engines/gemma4/src/g4_engine.cu),
// so the prompt math tracks the parity oracle. The norms / GeGLU / combine reuse the SAME shared kernels
// the decode path uses (u::k::rms_norm / u::k::geglu / the composed layer tail), keeping prefill and
// decode on one set of norm/activation math.
// ================================================================================================
namespace {

// Dequantise a RAW GGUF Q8_0 projection weight [n_out][n_in] to dense f32: one thread per 32-value block
// ([fp16 d][32 int8], 34 bytes). value = float(int8) * __half2float(d) — the same per-value arithmetic the
// decode dot and the reference's planar dequant use, so the SGEMM output tracks the reference. (The
// unified keeps non-expert weights in the raw GGUF layout for u::k::mmvq; this reads that layout directly.)
__global__ void k_dequant_weight_q8(const std::uint8_t* __restrict__ blocks, float* __restrict__ out,
                                    long long n_blocks, int n_in) {
  const long long b = static_cast<long long>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (b >= n_blocks) return;
  const int nb_row = n_in >> 5;  // 32-value blocks per row
  const long long row = b / nb_row;
  const int blk = static_cast<int>(b % nb_row);
  const std::uint8_t* bp = blocks + b * 34;
  const float d = __half2float(*reinterpret_cast<const __half*>(bp));
  const signed char* qs = reinterpret_cast<const signed char*>(bp + 2);
  float* o = out + row * static_cast<long long>(n_in) + static_cast<long long>(blk) * 32;
#pragma unroll
  for (int j = 0; j < 32; ++j) o[j] = static_cast<float>(qs[j]) * d;
}

// Dequantise a RAW BF16 projection weight to f32 (the uint16 is the f32 top half). Matches bf16_bits.
__global__ void k_dequant_weight_bf16(const std::uint16_t* __restrict__ w, float* __restrict__ out, long long n) {
  const long long i = static_cast<long long>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (i >= n) return;
  out[i] = __uint_as_float(static_cast<unsigned int>(w[i]) << 16);
}

inline unsigned grid1d(long long n) { return static_cast<unsigned>((n + 255) / 256); }

// W (Q8_0 or BF16, raw GGUF) -> dense f32 in `out`. F32 weights are used in place (prefill_gemm skips this).
void dequant_weight(const WeightRef& wm, float* out, cudaStream_t st) {
  const long long rows = wm.n_out, cols = wm.n_in;
  if (wm.quant == k::Quant::q8_0) {
    const long long nblk = rows * (cols / 32);
    k_dequant_weight_q8<<<grid1d(nblk), 256, 0, st>>>(static_cast<const std::uint8_t*>(wm.w), out, nblk,
                                                      static_cast<int>(cols));
  } else if (wm.quant == k::Quant::bf16) {
    const long long n = rows * cols;
    k_dequant_weight_bf16<<<grid1d(n), 256, 0, st>>>(static_cast<const std::uint16_t*>(wm.w), out, n);
  }
}

// y[T][rows] = x[T][cols] . w[rows][cols]^T in one cublasSgemm — EXACTLY the reference's sgemm
// (g4_engine.cu:476): column-major y^T = w^T(OP_T) * x^T, lda = cols, ldb = cols, ldc = rows.
bool prefill_sgemm(Gemma4Device& d, int rows, int T, int cols, const float* w, const float* x, float* y,
                   std::string& err) {
  const float one = 1.0f, zero = 0.0f;
  if (cublasSgemm(d.blas, CUBLAS_OP_T, CUBLAS_OP_N, rows, T, cols, &one, w, cols, x, cols, &zero, y, rows) !=
      CUBLAS_STATUS_SUCCESS) {
    err = "gemma4 prefill: cuBLAS SGEMM failed";
    return false;
  }
  return true;
}

// One prompt projection: dequantise W to f32 (Q8_0/BF16) into the shared wdq scratch, or use it in place
// (F32 router), then prefill_sgemm. Mirrors the reference's gemm (g4_engine.cu:483). The dequant and the
// SGEMM run on the one stream, so the SGEMM finishes reading wdq before the next projection overwrites it.
bool prefill_gemm(Gemma4Device& d, const WeightRef& wm, const float* x, float* y, int T, std::string& err) {
  const float* wf = nullptr;
  if (wm.quant == k::Quant::f32) {
    wf = reinterpret_cast<const float*>(wm.w);
  } else if (wm.quant == k::Quant::q8_0 || wm.quant == k::Quant::bf16) {
    dequant_weight(wm, d.wdq, d.stream);
    wf = d.wdq;
  } else {
    err = "gemma4 prefill: a projection weight is neither F32, BF16 nor Q8_0 (k-quant projection is a box extension)";
    return false;
  }
  return prefill_sgemm(d, wm.n_out, T, wm.n_in, wf, x, y, err);
}

// Prompt attention over the chunk's T queries (scale 1.0), byte-faithful to the reference's
// prompt_attention (g4_engine.cu:557): per KV head, a cublas strided-batched score GEMM (f16 q16 . f16 K),
// u::gemma4::attn_softmax (the windowed/causal mask, provided in gemma4_attn.cuh), then a strided-batched
// output GEMM (f16 probs . f16 V). Tiled over Ts queries so a tile's scores fit in s_buf.
bool prefill_attention(Gemma4Device& d, const Gemma4Geometry& g, const Gemma4DevLayer& L, int pos0, int T,
                       std::string& err) {
  const int ring = L.sliding ? L.cap : 0;
  const int Lk = L.sliding ? std::min(L.cap, pos0 + T) : pos0 + T;
  const int G = g.n_head / L.n_kv;
  const int hd = L.hd;
  const int window = L.sliding ? g.window : 0;
  const int Ts = static_cast<int>(std::max<std::size_t>(
      1, std::min<std::size_t>(static_cast<std::size_t>(T),
                               d.s_floats / (static_cast<std::size_t>(G) * static_cast<std::size_t>(Lk)))));
  const float one = 1.0f, zero = 0.0f;
  for (int gg = 0; gg < L.n_kv; ++gg) {
    const std::uint16_t* K = L.kc + static_cast<std::size_t>(gg) * L.cap * hd;
    const std::uint16_t* V = L.vc + static_cast<std::size_t>(gg) * L.cap * hd;
    for (int t0 = 0; t0 < T; t0 += Ts) {
      const int ts = std::min(Ts, T - t0);
      const std::uint16_t* Q =
          d.q16b + (static_cast<std::size_t>(t0) * g.n_head + static_cast<std::size_t>(gg) * G) * hd;
      float* O = d.attnb + (static_cast<std::size_t>(t0) * g.n_head + static_cast<std::size_t>(gg) * G) * hd;
      if (cublasGemmStridedBatchedEx(d.blas, CUBLAS_OP_T, CUBLAS_OP_N, Lk, ts, hd, &one, K, CUDA_R_16F, hd, 0,
                                     Q, CUDA_R_16F, g.n_head * hd, hd, &zero, d.s_buf, CUDA_R_32F, Lk,
                                     static_cast<long long>(ts) * Lk, G, CUBLAS_COMPUTE_32F,
                                     CUBLAS_GEMM_DEFAULT) != CUBLAS_STATUS_SUCCESS) {
        err = "gemma4 prefill: cuBLAS attention scores failed";
        return false;
      }
      gemma4::attn_softmax(d.s_buf, d.p_buf, G, ts, Lk, pos0 + t0, ring, window, pos0 + T - 1, d.stream);
      if (cublasGemmStridedBatchedEx(d.blas, CUBLAS_OP_N, CUBLAS_OP_N, hd, ts, Lk, &one, V, CUDA_R_16F, hd, 0,
                                     d.p_buf, CUDA_R_16F, Lk, static_cast<long long>(ts) * Lk, &zero, O,
                                     CUDA_R_32F, g.n_head * hd, hd, G, CUBLAS_COMPUTE_32F,
                                     CUBLAS_GEMM_DEFAULT) != CUBLAS_STATUS_SUCCESS) {
        err = "gemma4 prefill: cuBLAS attention output failed";
        return false;
      }
    }
  }
  if (cudaGetLastError() != cudaSuccess) { err = "gemma4 prefill: attention launch failed"; return false; }
  return true;
}

// One layer's MoE over the chunk, the faithful port of the reference's moe_batch (g4_engine.cu:591). Copies
// the device router picks to the host (THE one sync per layer on the prefill path), buckets the
// T*n_exp_used picks by expert, gathers each expert's token rows, dequantises and cublasSgemms its gate|up
// then (after a GeGLU) its down matrix against those rows, scatters the outputs back and combines them with
// the router weights and the per-expert ffn_down_exps.scale on the device. Also tallies the routing counts.
bool prefill_moe(Gemma4Device& d, const Gemma4Geometry& g, int il, int T, std::string& err) {
  const int A = T * g.n_exp_used;
  // The one sync per layer (prefill only): the device picks must reach the host to bucket by expert.
  if (cudaMemcpyAsync(d.picks_hb, d.picksb, sizeof(int) * static_cast<std::size_t>(A), cudaMemcpyDeviceToHost,
                      d.stream) != cudaSuccess ||
      cudaStreamSynchronize(d.stream) != cudaSuccess) {
    err = "gemma4 prefill: MoE picks copy failed";
    return false;
  }
  std::vector<int>& count = d.moe_count;
  std::vector<int>& offset = d.moe_offset;
  std::vector<int>& fill = d.moe_fill;
  std::fill(count.begin(), count.end(), 0);
  for (int a = 0; a < A; ++a) ++count[static_cast<std::size_t>(d.picks_hb[a])];
  offset[0] = 0;
  for (int e = 0; e < g.n_expert; ++e)
    offset[static_cast<std::size_t>(e) + 1] =
        offset[static_cast<std::size_t>(e)] + count[static_cast<std::size_t>(e)];
  for (int e = 0; e < g.n_expert; ++e) fill[static_cast<std::size_t>(e)] = offset[static_cast<std::size_t>(e)];
  int* order_h = d.order_hb;
  int* inv_h = d.order_hb + A;
  for (int a = 0; a < A; ++a) {
    const int at = fill[static_cast<std::size_t>(d.picks_hb[a])]++;
    order_h[at] = a;
    inv_h[a] = at;
  }
  cudaMemcpyAsync(d.orderb, order_h, sizeof(int) * static_cast<std::size_t>(A), cudaMemcpyHostToDevice, d.stream);
  cudaMemcpyAsync(d.invb, inv_h, sizeof(int) * static_cast<std::size_t>(A), cudaMemcpyHostToDevice, d.stream);
  gemma4::gather_rows(d.e_inb, d.orderb, A, g.n_embd, g.n_exp_used, d.xgb, d.stream);

  const gemma4::ExpertPtr* table = d.experts.layer_table(il);
  if (table == nullptr) { err = "gemma4 prefill: the expert table is not finalized"; return false; }
  const gemma4::WKind gu_k = d.experts.layer_gu_kind(il);
  const gemma4::WKind dn_k = d.experts.layer_dn_kind(il);
  const int gu_rows = d.experts.gu_rows();
  for (int e = 0; e < g.n_expert; ++e) {
    const int n = count[static_cast<std::size_t>(e)];
    if (n == 0) continue;
    const int off = offset[static_cast<std::size_t>(e)];
    gemma4::dequant_expert(gu_k, 0, gu_rows, g.n_embd, table, e, d.w1, d.stream);
    if (!prefill_sgemm(d, gu_rows, n, g.n_embd, d.w1, d.xgb + static_cast<std::size_t>(off) * g.n_embd,
                       d.gub + static_cast<std::size_t>(off) * gu_rows, err))
      return false;
  }
  gemma4::geglu_experts(d.gub, gu_rows, d.gub + g.n_ff_exp, gu_rows, d.mxb, A, g.n_ff_exp, d.stream);
  for (int e = 0; e < g.n_expert; ++e) {
    const int n = count[static_cast<std::size_t>(e)];
    if (n == 0) continue;
    const int off = offset[static_cast<std::size_t>(e)];
    gemma4::dequant_expert(dn_k, 1, g.n_embd, g.n_ff_exp, table, e, d.w2, d.stream);
    if (!prefill_sgemm(d, g.n_embd, n, g.n_ff_exp, d.w2, d.mxb + static_cast<std::size_t>(off) * g.n_ff_exp,
                       d.ygb + static_cast<std::size_t>(off) * g.n_embd, err))
      return false;
  }
  gemma4::scatter_combine(d.ygb, d.invb, d.wtsb, d.picksb, d.experts.layer_down_scale_dev(il), T, g.n_embd,
                          g.n_exp_used, d.moeb, d.stream);
  gemma4::count_picks(d.picksb, A, d.experts.layer_counts_dev(il), d.stream);
  if (cudaGetLastError() != cudaSuccess) { err = "gemma4 prefill: MoE kernel launch failed"; return false; }
  return true;
}

// env STRATA_U_DUMP: append this layer's T residual rows (for parity vs engines/gemma4 --dump-layers). The
// batched dump is layer-major (one T-row block per layer) unlike forward_one's one-row-per-call, so the box
// compares it per layer. Debug-only; no-op unless the env var is set.
void debug_dump_batch(const float* d_x, void* stream, int ne, int T) {
  const char* path = std::getenv("STRATA_U_DUMP");
  if (path == nullptr) return;
  cudaStreamSynchronize(static_cast<cudaStream_t>(stream));
  std::vector<float> rows(static_cast<std::size_t>(T) * static_cast<std::size_t>(ne));
  if (cudaMemcpy(rows.data(), d_x, rows.size() * sizeof(float), cudaMemcpyDeviceToHost) != cudaSuccess) return;
  std::FILE* f = std::fopen(path, "ab");
  if (f != nullptr) {
    std::fwrite(rows.data(), sizeof(float), rows.size(), f);
    std::fclose(f);
  }
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// One token through all 30 layers (decode path; also used per prompt token). Concrete re-expression
// of g4_engine.cu forward_one (517-555) on the shared + gemma4 kernels. Runtime-reached only once
// the box wires load() to succeed (loaded_), but compiled here so the schedule is reviewable.
// ------------------------------------------------------------------------------------------------
bool Gemma4Graph::enqueue_forward(void* stream, bool want_logits, int attn_pos_for_splits, std::string& err) {
  Gemma4Device& d = *dev_;
  cudaStream_t st = static_cast<cudaStream_t>(stream);
  const int ne = g_.n_embd, nff = g_.n_ff, k = g_.n_exp_used;
  const float emb_scale = std::sqrt((float)ne);

  // Embedding (Q8_0 row gather · sqrt(d)). g4_engine.cu:519-521. The token id is read from the pinned cell
  // (stable address, host-written before each call/replay) and the position from pos_dev, so this whole
  // sequence is position-free on the host and safe to capture into a CUDA graph.
  cudaMemcpyAsync(d.ids_d, d.tok_pinned, sizeof(int), cudaMemcpyHostToDevice, st);
  gemma4::embed_q8(d.ids_d, 1, d.embd_blocks, ne, emb_scale, d.x, st);

  for (int il = 0; il < g_.n_layers; ++il) {
    Gemma4DevLayer& L = d.layers[(std::size_t)il];
    const int hd = L.hd, nkv = L.n_kv;
    const int ring = L.sliding ? L.cap : 0;        // sliding caches ring; full are linear
    const int window = L.sliding ? g_.window : 0;

    // --- attention ---
    d.copy(d.h, d.x, ne);
    k::rms_norm(d.h, L.attn_norm, 1, ne, g_.rms_eps, st);     // g4:524
    d.matvec(L.q, d.h, d.qraw);                                // g4:525
    d.matvec(L.k, d.h, d.kraw);                                // g4:526
    if (L.sliding) d.matvec(L.v, d.h, d.vraw);                 // g4:527 (full layers: V-from-K)
    gemma4::QkvPost p{};
    p.q_raw = d.qraw;
    p.k_raw = d.kraw;
    p.v_raw = L.sliding ? d.vraw : d.kraw;   // V-FROM-K on full layers
    p.q_norm = L.q_norm;
    p.k_norm = L.k_norm;
    p.freq_factors = L.sliding ? nullptr : d.rope_freqs;  // proportional rope on full layers
    p.rope_base = L.sliding ? g_.rope_base_swa : g_.rope_base_full;
    p.hd = hd; p.n_head = g_.n_head; p.n_kv = nkv; p.pos0 = attn_pos_for_splits; p.pos0_dev = d.pos_dev; p.T = 1;
    p.ring = ring; p.cap = L.cap; p.eps = g_.rms_eps;
    p.q_out = d.qout; p.q16 = nullptr; p.k_cache = L.kc; p.v_cache = L.vc;
    gemma4::qkv_post(p, st);                                   // g4:528-531
    // Attention over keys [lo, pos]. The split count (grid.y) is FIXED here from attn_pos_for_splits (the
    // largest position the caller/graph will reach), so the launch is identical every decode step; the
    // kernel reads the live position from pos_dev and clamps each split, extra splits writing neutral
    // partials the merge discards (bitwise no-op). g4:532.
    const int lo_a = window > 0 ? (attn_pos_for_splits - window + 1 > 0 ? attn_pos_for_splits - window + 1 : 0) : 0;
    const int nsplit = gemma4::attn_decode_splits(attn_pos_for_splits - lo_a + 1);
    gemma4::attn_decode(d.qout, L.kc, L.vc, hd, g_.n_head, nkv, L.cap, ring, window, d.pos_dev, nsplit, d.attn,
                        d.dscratch, d.max_splits, st);         // g4:532
    d.matvec(L.o, d.attn, d.o);                                // g4:534

    // post-attention sandwich: x1 = x + rms(o)·post_attn_norm, FUSED into one launch (g4k::add_rmsnorm)
    // instead of rms_norm+copy+add — same math, 2 fewer kernel launches per layer (WDDM launch cost).
    gemma4::add_rmsnorm(d.x, d.o, L.post_attn_norm, d.x1, 1, ne, g_.rms_eps, st);

    // --- dense GeGLU MLP (ff 2112) --- g4:536-540
    d.copy(d.h, d.x1, ne);
    k::rms_norm(d.h, L.ffn_norm, 1, ne, g_.rms_eps, st);
    d.matvec(L.gate, d.h, d.g);
    d.matvec(L.up, d.h, d.u);
    k::geglu(d.g, d.u, d.m, nff, st);                          // SHARED gemma4 GeGLU
    d.matvec(L.down, d.m, d.dense);

    // --- softmax top-8 router --- g4:541-543
    d.copy(d.rn, d.x1, ne);
    k::rms_norm(d.rn, L.gate_inp_scale, 1, ne, g_.rms_eps, st);      // rms(x1)·gate_inp_scale
    k::scale_inplace(d.rn, ne, 1.0f / emb_scale, st);               // ·(1/√d)
    d.matvec(L.router, d.rn, d.rl);
    k::router_topk(d.rl, 1, g_.n_expert, k, d.picks, d.wts, st);    // SHARED (== g4 router)

    // --- 128-expert top-8 MoE (GeGLU) via the ZERO-HOST-SYNC device-table dispatch --- g4:544-548.
    // picks/wts are already on the DEVICE (router_topk). The per-layer device ExpertPtr table is
    // indexed by the device picks, and the per-expert ffn_down_exps.scale is applied on the device in
    // moe_combine, so the whole MoE enqueues on the stream with NO host sync (g4_engine.cu's design).
    d.copy(d.e_in, d.x1, ne);
    k::rms_norm(d.e_in, L.pre_ffw_norm_2, 1, ne, g_.rms_eps, st);
    if (!d.experts.run_moe(il, d.picks, d.wts, d.e_in, d.moe, st, err)) return false;

    // --- layer tail: double-sandwich norm + layer scale --- g4:549, now the FUSED g4k::ffn_end in one
    // launch: x = (x1 + rms(rms(dense)·w1 + rms(moe)·w2)·w3)·layer_scale. Replaces the 7-kernel composed
    // form (3 rms_norm + 2 add + copy + scale) — same math as the reference's fused kernel (which the box
    // parity-checks against), 6 fewer launches per layer.
    gemma4::ffn_end(d.x1, d.dense, d.moe, L.post_ffw_norm_1, L.post_ffw_norm_2, L.post_ffw_norm,
                    L.layer_scale, d.x, 1, ne, g_.rms_eps, st);
    debug_dump(d.x, st, ne);  // env STRATA_U_DUMP: per-layer residual, for parity vs engines/gemma4
  }
  // The LM head (tied matvec over the whole vocab) is only needed where a token is sampled: the last
  // prompt token and every decode step. Skipping it on the other prompt tokens cuts prefill cost.
  if (want_logits && !head(err)) return false;
  return true;
}

bool Gemma4Graph::forward_one(std::int32_t id, bool want_logits, std::string& err) {
  if (!loaded_) { err = "gemma4 forward: graph not loaded"; return false; }
  Gemma4Device& d = *dev_;
  cudaStream_t st = d.stream;
  const int pos = (int)pos_;
  // Non-graph (prompt / fallback) path: feed the token + position, run the shared body with the attention
  // grid sized exactly for this position, then the gated live rebalance. Bitwise-identical to the capture.
  *d.tok_pinned = id;
  gemma4::pos_set(d.pos_dev, pos, st);
  if (!enqueue_forward(st, want_logits, pos, err)) return false;
  // Live expert-hit rebalance, GATED to ~every 8 tokens: one D2H counts copy + the slot swaps, on the
  // stream, ordered after this token's reads. g4_engine.cu rebalance.
  if (++since_rebalance_ >= 8) {
    since_rebalance_ = 0;
    if (d.experts.rebalance(256, st, err) < 0) return false;
  }
  pos_ = pos + 1;
  return true;
}

bool Gemma4Graph::ensure_decode_graph(int attn_pos_for_splits, std::string& err) {
  Gemma4Device& d = *dev_;
  int cap_pos = attn_pos_for_splits;
  if (cap_pos > (int)context_ - 1) cap_pos = (int)context_ - 1;
  if (d.graph_ready && d.graph_splits_cap >= cap_pos) return true;  // the live graph already covers it
  if (d.decode_exec) { cudaGraphExecDestroy(d.decode_exec); d.decode_exec = nullptr; }
  if (d.decode_graph) { cudaGraphDestroy(d.decode_graph); d.decode_graph = nullptr; }
  d.graph_ready = false;
  // The stream must be idle before capture so the prefill's enqueued work is not swept in.
  if (cudaStreamSynchronize(d.stream) != cudaSuccess) { err = "gemma4 decode graph: pre-capture sync failed"; return false; }
  if (cudaStreamBeginCapture(d.stream, cudaStreamCaptureModeThreadLocal) != cudaSuccess) {
    err = "gemma4 decode graph: begin capture failed";
    return false;
  }
  // Capture the per-token body (embed -> layers -> head) plus the tail pos++ so each replay advances the
  // device position on its own. No rebalance (host logic) and no sampler (needs a host readback) here.
  const bool ok = enqueue_forward(d.stream, /*want_logits=*/true, cap_pos, err);
  gemma4::pos_inc(d.pos_dev, d.stream);
  cudaGraph_t g = nullptr;
  const cudaError_t ce = cudaStreamEndCapture(d.stream, &g);
  if (!ok || ce != cudaSuccess || g == nullptr) {
    if (g) cudaGraphDestroy(g);
    cudaGetLastError();  // clear any sticky capture error so the fallback path runs clean
    if (ok) err = "gemma4 decode graph: capture failed";
    return false;
  }
  if (cudaGraphInstantiate(&d.decode_exec, g, 0ull) != cudaSuccess) {
    cudaGraphDestroy(g);
    cudaGetLastError();
    err = "gemma4 decode graph: instantiate failed";
    return false;
  }
  d.decode_graph = g;
  d.graph_ready = true;
  d.graph_splits_cap = cap_pos;
  return true;
}

// ------------------------------------------------------------------------------------------------
// BATCHED PREFILL: T prompt tokens through all 30 layers in one shot. Concrete re-expression of the
// reference's Gemma4Engine::forward_batch (g4_engine.cu:635). The projections and the MoE run as
// dequant-to-f32 + cublasSgemm (prefill_gemm / prefill_moe); the attention as the cublas strided-batched
// score/output GEMMs around u::gemma4::attn_softmax (prefill_attention). The norms, GeGLU and the layer
// tail reuse the SAME shared kernels as forward_one, so prefill and decode share one norm/activation math.
// Writes the KV caches and the residual but NO logits: generate() runs the final <=4 tokens through
// forward_one for the sampled logits (the decode path that already holds parity).
// ------------------------------------------------------------------------------------------------
bool Gemma4Graph::forward_batch(const std::uint32_t* ids, int T, std::string& err) {
  if (!loaded_) { err = "gemma4 forward_batch: graph not loaded"; return false; }
  if (!prefill_ready_) { err = "gemma4 forward_batch: prefill scratch unavailable"; return false; }
  if (T <= 0 || T > kPrefillChunk) { err = "gemma4 forward_batch: bad chunk size"; return false; }
  Gemma4Device& d = *dev_;
  cudaStream_t st = d.stream;
  const int ne = g_.n_embd, nff = g_.n_ff, kk = g_.n_exp_used;
  const int pos0 = static_cast<int>(pos_);
  const float emb_scale = std::sqrt(static_cast<float>(ne));
  const std::size_t Tne = static_cast<std::size_t>(T) * static_cast<std::size_t>(ne);

  // Embedding (Q8_0 row gather . sqrt(d)) for all T tokens. g4_engine.cu:637-639.
  for (int t = 0; t < T; ++t) d.ids_hb[t] = static_cast<int>(ids[t]);
  cudaMemcpyAsync(d.ids_db, d.ids_hb, sizeof(int) * static_cast<std::size_t>(T), cudaMemcpyHostToDevice, st);
  gemma4::embed_q8(d.ids_db, T, d.embd_blocks, ne, emb_scale, d.xb, st);

  for (int il = 0; il < g_.n_layers; ++il) {
    Gemma4DevLayer& L = d.layers[static_cast<std::size_t>(il)];
    const int hd = L.hd, nkv = L.n_kv;
    const int ring = L.sliding ? L.cap : 0;  // sliding caches ring (slot = pos%ring); full are linear

    // --- attention --- g4_engine.cu:642-649
    d.copy_n(d.hb, d.xb, Tne);
    k::rms_norm(d.hb, L.attn_norm, T, ne, g_.rms_eps, st);
    if (!prefill_gemm(d, L.q, d.hb, d.qrawb, T, err)) return false;
    if (!prefill_gemm(d, L.k, d.hb, d.krawb, T, err)) return false;
    if (L.sliding && !prefill_gemm(d, L.v, d.hb, d.vrawb, T, err)) return false;  // full layers: V-from-K
    gemma4::QkvPost p{};
    p.q_raw = d.qrawb;
    p.k_raw = d.krawb;
    p.v_raw = L.sliding ? d.vrawb : d.krawb;
    p.q_norm = L.q_norm;
    p.k_norm = L.k_norm;
    p.freq_factors = L.sliding ? nullptr : d.rope_freqs;
    p.rope_base = L.sliding ? g_.rope_base_swa : g_.rope_base_full;
    p.hd = hd; p.n_head = g_.n_head; p.n_kv = nkv; p.pos0 = pos0; p.T = T;
    p.ring = ring; p.cap = L.cap; p.eps = g_.rms_eps;
    p.q_out = d.qoutb; p.q16 = d.q16b; p.k_cache = L.kc; p.v_cache = L.vc;  // q16 feeds the score GEMM
    gemma4::qkv_post(p, st);
    if (!prefill_attention(d, g_, L, pos0, T, err)) return false;
    if (!prefill_gemm(d, L.o, d.attnb, d.ob, T, err)) return false;

    // post-attention sandwich: x1 = x + rms(o).post_attn_norm (composed, as forward_one). g4:650
    k::rms_norm(d.ob, L.post_attn_norm, T, ne, g_.rms_eps, st);
    d.copy_n(d.x1b, d.xb, Tne);
    k::add_inplace(d.x1b, d.ob, static_cast<std::int64_t>(Tne), st);

    // --- dense GeGLU MLP (ff 2112) --- g4:651-654
    d.copy_n(d.hb, d.x1b, Tne);
    k::rms_norm(d.hb, L.ffn_norm, T, ne, g_.rms_eps, st);
    if (!prefill_gemm(d, L.gate, d.hb, d.gb, T, err)) return false;
    if (!prefill_gemm(d, L.up, d.hb, d.ub, T, err)) return false;
    k::geglu(d.gb, d.ub, d.mb, static_cast<std::int64_t>(T) * nff, st);  // elementwise == the per-row GeGLU
    if (!prefill_gemm(d, L.down, d.mb, d.denseb, T, err)) return false;

    // --- softmax top-8 router --- g4:655-657
    d.copy_n(d.rnb, d.x1b, Tne);
    k::rms_norm(d.rnb, L.gate_inp_scale, T, ne, g_.rms_eps, st);          // rms(x1).gate_inp_scale
    k::scale_inplace(d.rnb, static_cast<std::int64_t>(Tne), 1.0f / emb_scale, st);  // .(1/sqrt d)
    if (!prefill_gemm(d, L.router, d.rnb, d.rlb, T, err)) return false;
    k::router_topk(d.rlb, T, g_.n_expert, kk, d.picksb, d.wtsb, st);      // SHARED (== decode router)

    // --- 128-expert top-8 MoE (GeGLU) via the host-bucketed per-expert SGEMM --- g4:658-659
    d.copy_n(d.e_inb, d.x1b, Tne);
    k::rms_norm(d.e_inb, L.pre_ffw_norm_2, T, ne, g_.rms_eps, st);
    if (!prefill_moe(d, g_, il, T, err)) return false;

    // --- layer tail: double-sandwich norm + layer scale (composed, as forward_one) --- g4:660
    k::rms_norm(d.denseb, L.post_ffw_norm_1, T, ne, g_.rms_eps, st);
    k::rms_norm(d.moeb, L.post_ffw_norm_2, T, ne, g_.rms_eps, st);
    k::add_inplace(d.denseb, d.moeb, static_cast<std::int64_t>(Tne), st);
    k::rms_norm(d.denseb, L.post_ffw_norm, T, ne, g_.rms_eps, st);
    d.copy_n(d.xb, d.x1b, Tne);
    k::add_inplace(d.xb, d.denseb, static_cast<std::int64_t>(Tne), st);
    if (L.layer_scale != 1.0f) k::scale_inplace(d.xb, static_cast<std::int64_t>(Tne), L.layer_scale, st);
    debug_dump_batch(d.xb, st, ne, T);  // env STRATA_U_DUMP: per-layer residual rows, for parity debugging
  }
  // No head here (generate() samples from the last <=4 tokens via forward_one). The KV caches now hold
  // positions pos0..pos0+T-1; advance pos_ for the next chunk / the decode tail.
  pos_ = pos0 + T;
  return true;
}

// Final norm + tied Q8_0 head + logit soft-cap. g4_engine.cu head (510-515).
bool Gemma4Graph::head(std::string& err) {
  Gemma4Device& d = *dev_;
  cudaStream_t st = d.stream;
  d.copy(d.xn, d.x, g_.n_embd);
  k::rms_norm(d.xn, d.output_norm, 1, g_.n_embd, g_.rms_eps, st);
  d.matvec(d.head, d.xn, d.logits);                 // tied: mmvq over token_embd Q8_0
  gemma4::softcap(d.logits, g_.n_vocab, g_.softcap, st);
  if (cudaGetLastError() != cudaSuccess) { err = "gemma4 head: CUDA launch failed"; return false; }
  return true;
}

// ------------------------------------------------------------------------------------------------
// FamilyGraph surface.
// ------------------------------------------------------------------------------------------------
std::string Gemma4Graph::describe() const {
  return "gemma4(Gemma-4-26B-A4B: " + std::to_string(g_.n_layers) + "L, " + std::to_string(g_.n_head) +
         "h, hidden " + std::to_string(g_.n_embd) + ", dense ff " + std::to_string(g_.n_ff) + " + 128x" +
         std::to_string(g_.n_exp_used) + " MoE ff " + std::to_string(g_.n_ff_exp) +
         ", sliding/full dual-RoPE, soft-cap, tied Q8_0 head)";
}

namespace {
// Maps the GEN params (key=value) onto the shared sampler. Unknown keys are ignored (the frontend
// owns the full param set); unparseable values keep the default. Penalties need token history and
// are left off in the lean baseline (history null) — a documented box extension.
k::SamplerParams sampler_from(const Gen& req) {
  k::SamplerParams sp;
  for (const KeyVal& kv : req.params) {
    try {
      if (kv.key == "temperature" || kv.key == "temp") sp.temperature = std::stof(kv.value);
      else if (kv.key == "top_p") sp.top_p = std::stof(kv.value);
      else if (kv.key == "min_p") sp.min_p = std::stof(kv.value);
      else if (kv.key == "top_k") sp.top_k = std::stoi(kv.value);
      else if (kv.key == "seed") sp.seed = (std::uint64_t)std::stoull(kv.value);
      else if (kv.key == "greedy") sp.greedy = (kv.value == "1" || kv.value == "true");
    } catch (...) { /* keep the default on a bad value (fail soft for sampling knobs) */ }
  }
  return sp;
}
}  // namespace

bool Gemma4Graph::generate(const Gen& request, const LineSink& sink, std::string& error) {
  // The serve loop, re-expressing g4's decode over the shared forward + sampler. Steps: consume the
  // prompt (emit PP), then sample/emit/append up to max_new (honor eos / <turn|> / the context
  // limit), then emit the terminal DONE. Speculative fields stay 0 (gemma4 has no draft block).
  if (!loaded_) {
    error = "gemma4 generate: graph not loaded (device upload wired by the shared loader seam)";
    return false;
  }
  if (request.ids.empty()) { error = "gemma4 generate: empty prompt"; return false; }
  Gemma4Device& d = *dev_;
  reset_sequence();

  const auto t_start = Clock::now();
  const int n_prompt = (int)request.ids.size();
  if (pos_ + n_prompt > (std::int64_t)context_) { error = "gemma4 generate: prompt exceeds context"; return false; }

  // Consume the prompt the reference's `process` way (g4_engine.cu:668): the BULK through forward_batch in
  // chunks, and the LAST <=4 tokens through forward_one so the sampled logits come from the decode path
  // that already holds token-for-token greedy parity. When the batched scratch did not fit (prefill_ready_
  // false) or the prompt is <=4 tokens, the whole prompt falls back to forward_one (the lean baseline).
  int done = 0;
  // DIAG: STRATA_U_NO_BATCH=1 forces token-by-token prefill with the batched scratch still allocated,
  // to isolate the decode cost of the scratch VRAM from the batched-prefill rebalance.
  const bool use_batch = prefill_ready_ && n_prompt > 4 && std::getenv("STRATA_U_NO_BATCH") == nullptr;
  if (use_batch) {
    const int chunk = std::min(kPrefillChunk, g_.window);
    const int bulk = n_prompt - 4;  // reserve the last 4 prompt tokens for the decode path
    while (done < bulk) {
      const int T = std::min(bulk - done, chunk);
      if (!forward_batch(&request.ids[(std::size_t)done], T, error)) return false;
      done += T;
      // Mirror process(): consume the chunk's routing heat into the resident expert set, then sync + PP.
      // Scale the chunk's T*8 picks down to a per-gated-step (~8-token) magnitude so the score EWMA
      // isn't spiked — an unscaled chunk fold biases the resident set to the prompt and starves decode.
      if (d.experts.rebalance(256, d.stream, error, 8.0 / static_cast<double>(T)) < 0) return false;
      if (cudaStreamSynchronize(d.stream) != cudaSuccess) { error = "gemma4 generate: prefill sync failed"; return false; }
      const double ms = secs(t_start, Clock::now()) * 1000.0;
      const double tok_s = ms > 0.0 ? (double)done / (ms / 1000.0) : 0.0;
      sink(emit(PromptProgress{(std::uint32_t)done, (std::uint32_t)n_prompt, ms, tok_s}));
    }
  }
  for (int i = done; i < n_prompt; ++i) {
    // The decode path (mmvq + the device-table MoE). Only the LAST token needs logits (to sample the first
    // output); the head is skipped on the rest.
    if (!forward_one((std::int32_t)request.ids[(std::size_t)i], i == n_prompt - 1, error)) return false;
    const double ms = secs(t_start, Clock::now()) * 1000.0;
    const double tok_s = ms > 0.0 ? (double)(i + 1) / (ms / 1000.0) : 0.0;
    sink(emit(PromptProgress{(std::uint32_t)(i + 1), (std::uint32_t)n_prompt, ms, tok_s}));
  }
  const double prompt_ms = secs(t_start, Clock::now()) * 1000.0;

  const k::SamplerParams sp = sampler_from(request);
  // INSTRUMENTATION: clear the expert hit/miss/fill tallies so the decode-phase report below reflects
  // ONLY steady-state decode (the prompt's rebalance folds are excluded). No behaviour change.
  d.experts.reset_stats();

  // DECODE via a captured CUDA graph: embed -> 30 layers -> head is recorded ONCE and replayed per token,
  // so each step costs one graph launch instead of ~1100 kernel launches (the dominant WDDM cost the
  // instrumentation pinned). The token id goes through tok_pinned and the position through pos_dev + the
  // graph's tail pos++, so one static graph serves every step. The sampler (a host readback) and the
  // ~every-8-token rebalance (host swap logic) stay OUTSIDE the graph. STRATA_U_NO_GRAPH forces the
  // forward_one path, and a capture failure falls back to it too, so the engine always serves.
  const bool want_graph = std::getenv("STRATA_U_NO_GRAPH") == nullptr;
  bool use_graph = false;
  if (want_graph) {
    std::string gerr;
    if (ensure_decode_graph((int)pos_ + (int)request.max_new, gerr)) {
      gemma4::pos_set(d.pos_dev, (int)pos_, d.stream);  // the position the first replay will consume
      use_graph = true;
    } else {
      std::fprintf(stderr, "gemma4 decode: CUDA graph unavailable (%s); using forward_one\n", gerr.c_str());
    }
  }

  const auto t_decode = Clock::now();
  std::uint32_t produced = 0;
  Finish finish = Finish::length;
  for (std::uint32_t n = 0; n < request.max_new; ++n) {
    if (pos_ >= (std::int64_t)context_) { finish = Finish::length; break; }
    // Sample from the current (last) logits. History null => no penalties in the lean baseline.
    k::sample_tokens(d.logits, 1, g_.n_vocab, nullptr, 0, sp, d.sample_d, d.stream);
    int tok = -1;
    cudaMemcpyAsync(&tok, d.sample_d, sizeof(int), cudaMemcpyDeviceToHost, d.stream);
    if (cudaStreamSynchronize(d.stream) != cudaSuccess) { error = "gemma4 generate: sampler sync failed"; return false; }
    if (tok < 0) { finish = Finish::nonfinite; break; }      // -2 from a non-finite logit row
    if (tok == g_.eos_id || tok == g_.turn_close_id) { finish = Finish::stop; break; }
    sink(emit(Token{(std::uint32_t)tok}));
    ++produced;
    if (use_graph) {
      // Append `tok` and refresh the logits by replaying the captured graph (reads tok_pinned + pos_dev,
      // writes d.logits, and advances pos_dev in its tail). pos_ mirrors the device increment.
      *d.tok_pinned = tok;
      if (cudaGraphLaunch(d.decode_exec, d.stream) != cudaSuccess) { error = "gemma4 generate: graph launch failed"; return false; }
      pos_ = pos_ + 1;
      if (++since_rebalance_ >= 8) {  // gated live rebalance, OUTSIDE the graph (patches the device tables)
        since_rebalance_ = 0;
        if (d.experts.rebalance(256, d.stream, error) < 0) return false;
      }
    } else if (!forward_one((std::int32_t)tok, /*want_logits=*/true, error)) {
      return false;  // append; refresh logits (non-graph fallback)
    }
  }
  const double decode_ms = secs(t_decode, Clock::now()) * 1000.0;

  // INSTRUMENTATION: decode-phase expert cache behaviour. hits = routed picks whose expert was resident
  // in the slot pool; misses = picks served over PCIe from mapped-host; fills = slot refills (one expert
  // load + one eviction each). pcie_miss_mib is the derived decode PCIe traffic (misses x per-expert
  // bytes) -- the transfer cost the capacity hypothesis predicts. hits/misses are tallied only on the
  // ~every-8-token gated rebalance, so they sample the routing, not every pick. No behaviour change.
  {
    const std::uint64_t h = d.experts.stat_hits(), m = d.experts.stat_misses(), f = d.experts.stat_fills();
    const double tot = static_cast<double>(h + m);
    const double blob = d.experts.slots_per_layer() > 0
                            ? static_cast<double>(d.experts.resident_bytes()) /
                                  (static_cast<double>(d.experts.slots_per_layer()) * static_cast<double>(g_.n_layers))
                            : 0.0;
    std::fprintf(stderr,
                 "gemma4 decode stats: engine=new produced=%u decode_tok_s=%.1f hits=%llu misses=%llu "
                 "hit_rate=%.1f fills=%llu pcie_miss_mib=%.0f\n",
                 produced, decode_ms > 0.0 ? produced / (decode_ms / 1000.0) : 0.0,
                 static_cast<unsigned long long>(h), static_cast<unsigned long long>(m),
                 tot > 0.0 ? 100.0 * static_cast<double>(h) / tot : 0.0, static_cast<unsigned long long>(f),
                 static_cast<double>(m) * blob / 1048576.0);
  }

  sink(emit(Done{produced, (std::uint32_t)n_prompt, prompt_ms, decode_ms, finish, 0, 0, 0}));
  return true;
}

// ------------------------------------------------------------------------------------------------
// Factory.
// ------------------------------------------------------------------------------------------------
std::unique_ptr<FamilyGraph> make_gemma4_graph(const Geometry& geom, std::uint32_t context, std::string& error) {
  error.clear();
  if (geom.family != FamilyId::gemma4) {
    error = "make_gemma4_graph: geometry is not gemma4";
    return nullptr;
  }
  // Construct only. The shared loader seam (u_main) memory-maps the whole model and calls
  // graph->load(gguf, mapped, err) AFTER construction (GlmGraph already works this way; u_family.hpp
  // dispatches here, u_main.cpp calls load). Stash the guarded geometry + serving context for that
  // call. The graph is returned non-null and reports available()==false until load() succeeds.
  auto graph = std::unique_ptr<Gemma4Graph>(new Gemma4Graph());
  graph->common_ = geom;
  graph->context_ = context;
  return graph;
}

}  // namespace u
