// engines/unified/src/family/ds4_graph.cu
// -----------------------------------------------------------------------------
// The DeepSeek-V4.1-Flash (ds4) family graph, ported from engines/maya-ref/src/ds4
// (ds4_model.cu / ds4_main.cpp, MIT) onto the unified engine's shared facilities.
// This is the CORE SERVE GRAPH: geometry load, the per-layer forward pass (MLA
// attention over the compressed/latent KV window + the learned sparse index, then
// the sigmoid-routed MoE through u::ExpertTier), the head, sampling, the DSpark
// speculative round, and the generate() serve loop emitting T / PP / DONE via the
// sink.
//
// WHAT IS SHARED (NOT duplicated here) — design §1.2, §3.1, plan T1.1/T1.2:
//   kernels/common.cuh : u::k::  matmul / mmvq (quantized weight GEMV), rmsnorm,
//                        rope, softmax, router_sigmoid (sigmoid router + bias + top-k),
//                        sampler.
//   u_experts.hpp      : u::ExpertTier  — the VRAM/RAM/CPU expert tiering + the
//                        expert-hit-profile warm start + online rebalance; it runs
//                        the routed-expert GEMMs given the router picks.
// WHAT IS DS4-SPECIFIC (in kernels/ds4_*.cu): MLA / latent-KV window, the learned
//   index/sparse attention, the hyper-connection residual streams, and DSpark.
//
// SCOPE (lean serve path): single GPU. The reference's multi-GPU tensor
// parallelism, the threaded/async mailbox decode, EXL3 exotic layouts, vision and
// the slot/checkpoint conversation machinery are dropped (design §1.2). The deep
// paths that remain relevant (full DSpark sampled speculative acceptance, the exotic
// KV FP8/FP4 packing, chunked prefill) are STRUCTURED STUBS with precise TODOs
// citing ds4_model.cu file:line; the GPU box fills + parity-checks them (plan T1.4).
//
// Compiled only on the GPU box (STRATA_U_CUDA ON, CUDA language). On host / CI this
// is an empty translation unit; the engine serves via u::make_family_graph's stub.
// -----------------------------------------------------------------------------
#include "family/ds4_graph.hpp"

#if defined(STRATA_U_CUDA)

#include "kernels/common.cuh"     // u::k::  (shared generic kernels; T1.1)
#include "u_experts.hpp"          // u::ExpertTier (shared expert tier; landed)
#include "u_gguf.hpp"             // u::GgufFile, u::GgufTensor, u::GgufValue (the mapped GGUF)
#include "u_calibration.hpp"      // u::Calibration (the per-box adaptive-tier profile, spec C1)
#include "kernels/ds4_mla.cuh"
#include "kernels/ds4_index.cuh"
#include "kernels/ds4_dspark.cuh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <cuda_runtime.h>

// -----------------------------------------------------------------------------
// SHARED CONTRACTS. u::k (kernels/common.cuh) is landed and used directly: this
// graph calls k::mmvq_f32 / k::f32_gemv / k::bf16_gemv (the matvec per quant),
// k::rms_norm, k::swiglu, k::add_inplace, k::f32_gemv (router logits),
// k::router_sigmoid (ds4 is a sigmoid-routed family), k::sample_tokens /
// k::SamplerParams. See the Weight helper (wmul) for the quant dispatch.
//
// u::ExpertTier (u_experts.hpp, lane T1.2) IS landed: this graph uses the registered path
// exactly as glm_graph.cu does — register_layer(il, gate_exps, up_exps, down_exps, n_expert,
// n_ff_exp, swiglu_limit, err) at load (host-mapped WSlots; the tier owns VRAM/RAM placement),
// run_window(il, x, ids, weights, T, k, n_embd, n_ff, swiglu_limit, out, stream, err) in the
// forward (window T>1 of router picks), and reg_finalize/reg_set_cpu_lane for residency
// (the Calibration profile + the STRATA_DS4_EXPERT_PROFILE warm-start env, same as GLM's).
// -----------------------------------------------------------------------------

namespace u {
namespace {

inline double now_ms() {
  using namespace std::chrono;
  return (double) duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count() / 1000.0;
}

#define DS4_CK(expr)                                                                    \
  do {                                                                                  \
    const cudaError_t e_ = (expr);                                                      \
    if (e_ != cudaSuccess) { err = std::string("ds4 cuda: ") + cudaGetErrorString(e_); return false; } \
  } while (0)

using ds4::kWin;
using ds4::kPicks;

// Extended ds4 geometry: the common dims from u::Geometry plus the MLA / index /
// DSpark dims DeepSeek carries. load_geometry() reads them from the GGUF's own
// metadata — the file spells general.architecture "deepseek41" (Maya's
// tools/ds4/pack_gguf.py:387) and every key below is that file's, mapped exactly as
// pack_gguf.py geometry() (:139-193) maps it to the config.json names Maya's
// Geo::from (ds4_model.cu:468-496) consumes. NEWER_KEYS (pack_gguf.py:221-229): some
// converters spell a few keys differently; both spellings are accepted, first hit wins.
struct Ds4Geometry {
  // common (from u::Geometry)
  int vocab = 0, dim = 0, nl = 0, heads = 0, ff = 0, n_exp = 0, k_exp = 0, shared = 0;
  int64_t context = 0;
  double rope_theta = 1e4;
  // MLA
  int hd = 0;            // head_dim (the per-head K=V latent width)
  int q_lora = 0;        // query down-projection rank
  int o_lora = 0;        // output / kv latent rank
  int o_groups = 0;      // grouped output projection
  int rd = 0;            // rope dim (a suffix of hd)
  int hc = 1;            // hyper-connection streams
  int win = 0;           // sliding window of raw KV rows
  float eps = 1e-20f, hc_eps = 1e-6f, route_scale = 1.0f, swiglu_limit = 0.0f;
  int sinkhorn = 0;
  double comp_theta = 0.0;   // attention.compress_rope_freq_base (the compressor's rope base)
  // learned index
  int ix_heads = 0, ix_hd = 0, ix_topk = 0, cand_topk = 0, cand_blk = 0, cand_layer = -1;
  // per-layer: compress ratio (0 = not a KV source), and which layers source KV /
  // index / engram (per-layer source-layer id lists, -1 = none).
  std::vector<int> ratios, kv_src, ix_src, eg_layers;
  // DSpark drafter
  int n_mtp = 0, d_block = 0, d_noise = 0, d_rank = 0;
  std::vector<int> d_targets;
};

// Reads the extended geometry from the mapped GGUF. `seed` is the guarded common
// geometry (u_geometry checked the ranges under the file's own arch prefix); the
// ds4-specific keys below are read here, fail-closed: a missing/ill-typed required
// key refuses the load with the key named. Mirrors pack_gguf.py geometry() 1:1.
bool load_geometry(const GgufFile& gguf, const Geometry& seed, std::uint32_t context, Ds4Geometry& g,
                   std::string& err) {
  const std::string a = seed.arch + ".";
  auto key = [&](const char* suffix) -> const GgufValue* { return gguf.find(a + suffix); };
  // first-of: accept a NEWER_KEYS alternate spelling (ours, then theirs).
  auto first_of = [&](std::initializer_list<const char*> suffixes) -> const GgufValue* {
    for (const char* s : suffixes)
      if (const GgufValue* v = key(s)) return v;
    return nullptr;
  };
  auto scalar = [&](std::initializer_list<const char*> suffixes, bool required, auto& out,
                    const char* what) -> bool {
    const GgufValue* v = first_of(suffixes);
    if (v == nullptr || v->type == GgufType::array) {
      if (!required) return true;
      err = "ds4: geometry key " + a + *suffixes.begin() + " missing or not a scalar (" + what + ")";
      return false;
    }
    if constexpr (std::is_same_v<std::decay_t<decltype(out)>, double> ||
                  std::is_same_v<std::decay_t<decltype(out)>, float>) {
      out = (std::decay_t<decltype(out)>) (v->type == GgufType::f32 || v->type == GgufType::f64 ? v->real
                                                                                              : (double) v->integer);
    } else {
      out = (std::decay_t<decltype(out)>) v->integer;
    }
    return true;
  };
  auto list = [&](std::initializer_list<const char*> suffixes, std::vector<int>& out, bool required) -> bool {
    const GgufValue* v = first_of(suffixes);
    if (v == nullptr) {
      if (!required) return true;
      err = "ds4: geometry key " + a + *suffixes.begin() + " missing (list)";
      return false;
    }
    if (v->type != GgufType::array) { err = "ds4: geometry key " + a + *suffixes.begin() + " not a list"; return false; }
    out.clear();
    out.reserve(v->integers.size());
    for (std::int64_t x : v->integers) out.push_back((int) x);  // explicit narrowing (the GGUF ints are 64-bit; /WX)
    return true;
  };

  g.vocab = (int) seed.vocab_size;
  g.dim = (int) seed.embedding_length;
  g.nl = (int) seed.block_count;
  g.heads = (int) seed.head_count;
  g.hd = (int) seed.head_dim;                     // <arch>.attention.key_length
  g.ff = (int) seed.expert_feed_forward_length;   // moe_intermediate_size
  g.n_exp = (int) seed.expert_count;
  g.k_exp = (int) seed.expert_used_count;
  g.shared = (int) seed.expert_shared_count;
  g.context = (int64_t) context;
  g.rope_theta = seed.rope_freq_base;
  if (!scalar({"attention.q_lora_rank"}, true, g.q_lora, "q_lora_rank")) return false;
  if (!scalar({"attention.output_lora_rank"}, true, g.o_lora, "output_lora_rank")) return false;
  if (!scalar({"attention.output_group_count"}, true, g.o_groups, "output_group_count")) return false;
  if (!scalar({"rope.dimension_count"}, true, g.rd, "qk_rope_head_dim")) return false;
  if (!scalar({"hyper_connection.count"}, true, g.hc, "hc_mult")) return false;
  if (!scalar({"hyper_connection.sinkhorn_iterations"}, false, g.sinkhorn, "sinkhorn")) return false;
  if (!scalar({"hyper_connection.epsilon"}, false, g.hc_eps, "hc_eps")) return false;
  if (!scalar({"attention.layer_norm_rms_epsilon"}, false, g.eps, "rms_eps")) return false;
  if (!scalar({"attention.sliding_window"}, true, g.win, "sliding_window")) return false;
  if (!scalar({"expert_weights_scale"}, false, g.route_scale, "routed_scaling_factor")) return false;
  if (!scalar({"attention.compress_rope_freq_base"}, false, g.comp_theta, "compress rope theta")) return false;
  if (g.comp_theta == 0.0) g.comp_theta = g.rope_theta;
  if (!scalar({"attention.indexer.head_count"}, true, g.ix_heads, "index_n_heads")) return false;
  if (!scalar({"attention.indexer.key_length"}, true, g.ix_hd, "index_head_dim")) return false;
  if (!scalar({"attention.indexer.top_k"}, true, g.ix_topk, "index_topk")) return false;
  if (!scalar({"attention.indexer.candidate_top_k_blocks", "attention.candidate_topk_blocks"}, false, g.cand_topk,
               "candidate_topk_blocks")) return false;
  if (!scalar({"attention.indexer.candidate_block_size", "attention.candidate_block_size"}, false, g.cand_blk,
               "candidate_block_size")) return false;
  {
    int cl = -1;
    if (!scalar({"attention.indexer.candidate_source_layer", "attention.candidate_source_layer_id"}, false, cl,
                 "candidate_source_layer")) return false;
    g.cand_layer = cl;
  }
  // swiglu_clamp_exp is a per-layer list; the engine takes one limit (pack_gguf.py:190:
  // differing values are refused, not averaged).
  {
    const GgufValue* v = key("swiglu_clamp_exp");
    if (v != nullptr && v->type == GgufType::array && !v->reals.empty()) {
      const double first = v->reals[0];
      for (const double c : v->reals)
        if (std::fabs(c - first) > 1e-6) { err = "ds4: swiglu_clamp_exp differs between layers (the engine takes one limit)"; return false; }
      g.swiglu_limit = (float) first;
    }
  }
  if (!list({"attention.compress_ratios"}, g.ratios, true)) return false;
  if (!list({"attention.kv_source_layers", "kv_source_layer_ids"}, g.kv_src, true)) return false;
  if (!list({"attention.indexer.source_layers", "index_source_layer_ids"}, g.ix_src, true)) return false;
  if (!list({"engram.layers", "engram.layer_ids"}, g.eg_layers, false)) return false;
  if (!scalar({"dspark.block_size"}, false, g.d_block, "dspark_block_size")) return false;
  if (!scalar({"dspark.noise_token_id"}, false, g.d_noise, "dspark_noise_token_id")) return false;
  if (!scalar({"dspark.markov_rank"}, false, g.d_rank, "dspark_markov_rank")) return false;
  if (!list({"dspark.target_layers", "dspark_target_layer_ids"}, g.d_targets, false)) return false;

  if ((int) g.ratios.size() < g.nl) { err = "ds4: compress_ratios shorter than block_count"; return false; }
  if ((int) g.kv_src.size() < g.nl || (int) g.ix_src.size() < g.nl) {
    err = "ds4: kv_source/index_source lists shorter than block_count";
    return false;
  }
  // The DSpark drafter blocks (Geo::from :510-513): extra ratio entries beyond nl, the
  // dspark keys, and the mtp.* tensors being actually present — all three, else no drafter.
  g.n_mtp = g.d_block > 0 && (int) g.ratios.size() > g.nl && gguf.tensor("mtp.0.attn_q_a.weight") != nullptr
                ? (int) g.ratios.size() - g.nl
                : 0;
  if (g.dim <= 0 || g.nl <= 0 || g.heads <= 0 || g.hd <= 0 || g.vocab <= 0 || g.q_lora <= 0 || g.o_lora <= 0 ||
      g.o_groups <= 0 || g.rd <= 0 || g.hc <= 0 || g.win <= 0 || g.ix_heads <= 0 || g.ix_hd <= 0 || g.ix_topk <= 0) {
    err = "ds4: geometry has a non-positive required dim (see load_geometry)";
    return false;
  }
  return true;
}

// DeepSeek's precompute_freqs_cis (YaRN when original > 0), ported verbatim from
// ds4_model.cu rope_freqs (:500-521): dim/2 F32 entries, the same smooth-ramp blend.
std::vector<float> rope_freqs(int dim, int original, double base, double factor, double beta_fast,
                              double beta_slow) {
  std::vector<float> f((std::size_t) dim / 2);
  for (int i = 0; i < dim / 2; ++i) {
    const float e = (float) (2 * i) / (float) dim;
    f[(std::size_t) i] = 1.0f / std::pow((float) base, e);
  }
  if (original > 0) {
    auto corr = [&](double rot) {
      return dim * std::log(original / (rot * 2 * 3.14159265358979323846)) / (2 * std::log(base));
    };
    const double low = std::max(std::floor(corr(beta_fast)), 0.0);
    const double high = std::min(std::ceil(corr(beta_slow)), (double) dim - 1);
    const float den = (float) std::max(high - low, 1e-3);
    for (int i = 0; i < dim / 2; ++i) {
      float ramp = ((float) i - (float) low) / den;
      ramp = std::min(1.0f, std::max(0.0f, ramp));
      const float smooth = 1.0f - ramp;
      f[(std::size_t) i] = f[(std::size_t) i] / (float) factor * (1.0f - smooth) + f[(std::size_t) i] * smooth;
    }
  }
  return f;
}

// A resident weight matrix: a quantized/bf16/f32 blob plus its format. Replaces the
// reference's x3::Mat (EXL3) with the unified engine's quant set (u::k::Quant). The
// GGUF loader fills {q, w}; the matvec goes through Impl::wmul -> u::k mmvq/gemv.
struct Weight {
  k::Quant q = k::Quant::bf16;
  const void* w = nullptr;
};

// One transformer layer's resident weights (lean; TP mates / EXL3 classes dropped).
// Origin: ds4_model.cu struct Layer (:704-812) trimmed to the serve path.
struct Layer {
  int il = 0, ratio = 0;
  bool kv_src = false, ix_src = false, owns_k = false, cand_src = false, uses_cand = false;
  int src = -1, ik_src = -1, eg = -1;
  // attention
  Weight wq_a, wq_b, wkv, wo_b, c_wkv, c_wgate, i_wq_b, i_wk;
  Weight wo_a[16];
  const float* q_norm = nullptr;
  const float* kv_norm = nullptr;
  const float* attn_norm = nullptr;
  const float* ffn_norm = nullptr;
  const float* c_norm = nullptr;
  const float* ik_norm = nullptr;
  const float* sink = nullptr;
  const float* hca_fn = nullptr; const float* hca_scale = nullptr; const float* hca_base = nullptr;
  const float* hcf_fn = nullptr; const float* hcf_scale = nullptr; const float* hcf_base = nullptr;
  const uint16_t* i_wproj = nullptr;
  // MoE
  Weight s1, s3, s2;           // shared expert (gate, up, down)
  const float* gate_w = nullptr;   // router, F32 [n_exp][dim]
  const float* gate_bias = nullptr;
  // engram
  Weight eg_wkv;
  const float* eg_w = nullptr;
  // KV window ring + compressor state (device)
  uint8_t* win_ring = nullptr;     // [win] rows of kKvFp8(hd)
  float* c_kv = nullptr;           // compressor partial group (ratio>1)
  float* c_score = nullptr;
  int n_exp = 0, k_exp = 0;
};

// A KV source layer's compressed KV + index keys (append-only). Origin: struct Src
// (ds4_model.cu:814-834), trimmed to one device.
struct Src {
  int layer = 0, ratio = 1;
  int64_t cap = 0;
  uint8_t* ckv = nullptr;   // rows of kKvFp4(hd)
  uint8_t* ik = nullptr;    // rows of kKvFp4p(ix_hd)
};

}  // namespace

// =============================================================================
// Ds4Graph::Impl — the resident model and the window scratch.
// =============================================================================
struct Ds4Graph::Impl {
  Ds4Geometry g;
  Geometry geom0;                      // the guarded common geometry (from make_ds4_graph)
  std::uint32_t ctx = 0;
  bool ready = false;

  std::vector<Layer> L;
  std::vector<Src> srcs;
  std::unique_ptr<ExpertTier> experts;   // the shared tier facility (landed)

  // head
  const float* head_norm = nullptr;
  Weight head;

  // rope frequency tables (F32, precomputed on load): the window / compressor bases.
  float* freqs_win = nullptr;
  float* freqs_comp = nullptr;

  // generation state
  int64_t pos = 0;
  std::vector<int32_t> toks;

  // device stream + window scratch (allocated in alloc_scratch). Sizes are rows of
  // up to kWin tokens; the routed-expert rows are kWin*kPicks. Origin: struct Dev
  // (ds4_model.cu:524-584) trimmed to the serve path.
  cudaStream_t st = nullptr;
  float *h = nullptr, *h2 = nullptr, *x = nullptr, *pre = nullptr;
  float *mix = nullptr, *a_pre = nullptr, *a_post = nullptr, *a_comb = nullptr;
  float *f_pre = nullptr, *f_post = nullptr, *f_comb = nullptr;
  float *qr = nullptr, *q = nullptr, *kvr = nullptr, *ckr = nullptr, *cgr = nullptr, *latent = nullptr;
  float *ikr = nullptr, *iq = nullptr, *iw = nullptr, *scores = nullptr;
  float *o = nullptr, *oa = nullptr, *oa_scr = nullptr, *ao = nullptr;
  float *glog = nullptr, *sg = nullptr, *su = nullptr, *sh = nullptr, *sh_out = nullptr;
  float *rw = nullptr, *moe_routed = nullptr, *logits_dev = nullptr;
  uint16_t *emb_rows = nullptr, *ext = nullptr;
  int *ridx = nullptr, *topk = nullptr, *cand = nullptr, *tok_dev = nullptr;
  void* q8_scratch = nullptr;         // Q8_1 activation scratch for mmvq_f32 (<=8 cols)
  uint16_t* x16 = nullptr;            // one activation column as BF16, for bf16_gemv
  ds4::TopkScratch tks{};
  ds4::AttnScratch attn_scr{};
  uint16_t* embed_pinned = nullptr;   // the embedding table (BF16), host-mapped or pinned
  uint16_t* h_emb = nullptr;          // a window's embedding rows (pinned)
  int n_logits = 0;

  // device arena for the uploaded (non-expert) weights, and the repacked grouped
  // attn_output_a slices (see load()).
  std::uint8_t* w_arena = nullptr;
  std::uint8_t* woa_arena = nullptr;

  // The quant-dispatched matvec: y[T][n_out] = W . x[T][n_in], W in Weight::q.
  // Quantized -> k::mmvq_f32 in column groups of <=8; bf16 -> per-column k::bf16_gemv
  // (x converted to BF16 in x16); f32 -> per-column k::f32_gemv. Replaces the
  // reference's x3::mv/job (ds4_model.cu mv/mv_n :8909-8945).
  void wmul(const Weight& m, const float* xin, float* y, int n_in, int n_out, int T);

  // strides (ds4_model.cu:6145-6147, 3242-3247)
  int topk_stride() const { return std::max(g.ix_topk, 32); }
  int cand_stride() const { return std::max(g.cand_topk, 32); }
  long long bm_stride() const { return (long long) (g.context + 64) / std::max(1, g.cand_blk) + 64; }
  std::size_t ckv_rb() const { return ds4::kv_row_bytes(ds4::kKvFp4, g.hd); }
  std::size_t ik_rb() const { return ds4::kv_row_bytes(ds4::kKvFp4p, g.ix_hd); }

  // Weights load: called by the substrate with the merged GGUF (FamilyGraph::load
  // seam, u_family.hpp:48 - same as glm_graph.cu). Geometry keys, upload, caches,
  // residency; sets ready.
  bool load(const GgufFile& gguf, const std::uint8_t* mapped, std::string& err);
  // load() phases (each honest-size, fail-closed):
  bool upload_weights(const GgufFile& gguf, const std::uint8_t* mapped, std::string& err);
  bool wire_layers(const GgufFile& gguf, const std::uint8_t* mapped, std::string& err);
  bool init_freqs(const GgufFile& gguf, std::string& err);
  bool init_experts(const GgufFile& gguf, const std::uint8_t* mapped, std::string& err);
  std::map<std::string, Weight> wm;   // uploaded GGUF tensor name -> device Weight
  bool alloc_scratch(std::string& err);
  bool forward(const int* tk, int T, int logits_rows, std::string& err);
  bool layer_forward(Layer& l, int64_t p0, int T, std::string& err);
  bool head_forward(int T, std::string& err);
  bool step(int token, bool want_logits, std::string& err) { return forward(&token, 1, want_logits ? 1 : 0, err); }
  int sample(const k::SamplerParams& sp);
  bool draft(int y, std::vector<int>& out, std::vector<float>& conf, const k::SamplerParams& sp, std::string& err);
  bool verify(int y, const std::vector<int>& drafts, const k::SamplerParams& sp, int& accepted, int& next,
              std::string& err);
  ~Impl();
};

// --------------------------------------------------------------- load
//
// Ported shape of the reference's load(): ds4_model.cu load() (:2306), alloc (:2750),
// place_experts (:4541), load_experts (:5293) - re-expressed on the unified engine's
// GgufFile + arena upload (the pattern glm_graph.cu::load proved on the box) and the
// shared ExpertTier. The mapped GGUF is the merged multi-part file (u_main.cpp:423);
// tensor host_data resolves across shards.

bool Ds4Graph::Impl::load(const GgufFile& gguf, const std::uint8_t* mapped, std::string& err) {
  if (ready) { err = "ds4: load called twice"; return false; }
  if (!load_geometry(gguf, geom0, ctx, g, err)) return false;
  if (st == nullptr) DS4_CK(cudaStreamCreate(&st));
  if (!alloc_scratch(err)) return false;
  if (!upload_weights(gguf, mapped, err)) return false;
  if (!wire_layers(gguf, mapped, err)) return false;
  if (!init_freqs(gguf, err)) return false;
  if (!init_experts(gguf, mapped, err)) return false;
  ready = true;
  pos = 0;
  toks.clear();
  std::fprintf(stderr,
               "ds4: %dL, %d heads hd %d, q_lora %d, o_lora %d x %d groups, rope rd %d, hc %d (sinkhorn %d), "
               "win %d, ix %dx%d topk %d (cand %d x %d), %d experts x %d (k %d, shared %d), mtp %d, ctx %lld\n",
               g.nl, g.heads, g.hd, g.q_lora, g.o_lora, g.o_groups, g.rd, g.hc, g.sinkhorn, g.win, g.ix_heads,
               g.ix_hd, g.ix_topk, g.cand_topk, g.cand_blk, g.n_exp, g.ff, g.k_exp, g.shared, g.n_mtp,
               (long long) g.context);
  std::fflush(stderr);
  return true;
}

// Upload every non-expert tensor to one device arena and index it by name (wm). The
// routed experts (ffn_*_exps) stay host-mapped for the tier (residency, §3.5); the
// mtp.* drafter tensors are skipped (the DSpark draft path is the box TODO below);
// token_embd stays host-side (embed_pinned); the Engram hash parameter tensors (older
// spellings; pack_gguf.py:231 HASH_TENSORS) are metadata the geometry key list already
// covers; and attn_output_a is repacked per output group straight from the mapped
// image in wire_layers (skipping it here avoids holding the grouped tensor twice).
// Everything else in the file is uploaded. Quantized tensors are kept raw -
// wmul dispatches them through u::k mmvq/gemv, exactly as glm's gemv_ does.
bool Ds4Graph::Impl::upload_weights(const GgufFile& gguf, const std::uint8_t* mapped, std::string& err) {
  auto skip = [](const std::string& n) {
    return n.find("ffn_gate_exps") != std::string::npos || n.find("ffn_up_exps") != std::string::npos ||
           n.find("ffn_down_exps") != std::string::npos || n.rfind("mtp.", 0) == 0 ||
           n.find("attn_output_a.weight") != std::string::npos ||
           n == "token_embd.weight" || n == "engram_multipliers.weight" || n == "engram_primes.weight" ||
           n == "engram_offsets.weight" || n == "engram_token_map.weight";
  };
  std::uint64_t arena = 0;
  for (const auto& t : gguf.tensors)
    if (!skip(t.name)) arena += (t.bytes + 255u) & ~std::uint64_t{255u};
  if (arena == 0) { err = "ds4: no uploadable tensors in the GGUF"; return false; }
  DS4_CK(cudaMalloc((void**) &w_arena, arena));
  std::uint64_t at = 0;
  for (const auto& t : gguf.tensors) {
    if (skip(t.name)) continue;
    // host_data is the loader-resolved absolute pointer (multi-part shards); fall back to
    // mapped + offset for single-file callers (same contract glm_graph.cu relies on).
    const std::uint8_t* src = t.host_data ? t.host_data : mapped + t.offset;
    DS4_CK(cudaMemcpy(w_arena + at, src, t.bytes, cudaMemcpyHostToDevice));
    Weight w;
    w.q = (k::Quant) (int) t.type;   // u::k::Quant ids match the GGML type ids (common.cuh)
    w.w = w_arena + at;
    wm[t.name] = w;
    at += (t.bytes + 255u) & ~std::uint64_t{255u};
  }
  return true;
}

// Per-layer wiring from the uploaded wm map (tensor names exactly as the GGUF spells
// them, pack_gguf.py LAYER/GLOBAL tables): attention, compressor, indexer,
// hyper-connections, shared expert, router, the FP8 window ring, the compressor
// partial group (ratio x hd floats each, Maya :4362-4363), and the compressed-KV /
// index-key source caches. attn_output_a is repacked per output group into contiguous
// slices (the pack does the same, "attn.wo_a.slice one entry a group").
bool Ds4Graph::Impl::wire_layers(const GgufFile& gguf, const std::uint8_t* mapped, std::string& err) {
  const int HD = g.hd, NH = g.heads, OG = g.o_groups;
  if (OG < 1 || (NH * HD) % OG != 0) { err = "ds4: heads*head_dim not divisible into output groups"; return false; }
  const int per = (NH * HD) / OG;   // attn_output_a input columns per group
  // One arena for every group-sliced attn_output_a (the tensor is skipped by the upload
  // phase; each layer's OG compact [o_lora][per] slices are copied in below).
  std::size_t woa_total = 0;
  for (const auto& t : gguf.tensors)
    if (t.name.find("attn_output_a.weight") != std::string::npos && t.name.rfind("mtp.", 0) != 0)
      woa_total += (t.bytes + 255u) & ~std::uint64_t{255u};
  if (woa_total != 0) DS4_CK(cudaMalloc((void**) &woa_arena, woa_total));
  std::uint64_t woa_at = 0;
  std::vector<std::string> missing;
  auto need = [&](const std::string& n) -> Weight {
    auto it = wm.find(n);
    if (it == wm.end()) { missing.push_back(n); return Weight{}; }
    return it->second;
  };
  auto f32p = [&](const std::string& n, const float*& dst) {
    const Weight w = need(n);
    if (w.q != k::Quant::f32) { missing.push_back(n + " (F32 required)"); return; }
    dst = (const float*) w.w;
  };
  // One Src entry per source layer, shared by all its consumers; ckv allocated only for
  // KV-source entries, ik only for index-source entries (ds4_model.cu :2745-2752).
  auto entry = [&](int layer_id, bool want_ckv, bool want_ik) -> int {
    int idx = -1;
    for (std::size_t i = 0; i < srcs.size(); ++i)
      if (srcs[i].layer == layer_id) { idx = (int) i; break; }
    if (idx < 0) {
      Src s;
      s.layer = layer_id;
      s.ratio = (layer_id >= 0 && layer_id < (int) g.ratios.size() && g.ratios[(std::size_t) layer_id] > 0)
                    ? g.ratios[(std::size_t) layer_id] : 1;
      s.cap = (std::int64_t) (g.context / s.ratio) + 64;
      srcs.push_back(s);
      idx = (int) srcs.size() - 1;
    }
    Src& sr = srcs[(std::size_t) idx];
    if (want_ckv && sr.ckv == nullptr) {
      if (cudaMalloc((void**) &sr.ckv, (std::size_t) sr.cap * ckv_rb()) != cudaSuccess) { err = "ds4: ckv cache alloc"; return -1; }
      cudaMemset(sr.ckv, 0, (std::size_t) sr.cap * ckv_rb());
    }
    if (want_ik && sr.ik == nullptr) {
      if (cudaMalloc((void**) &sr.ik, (std::size_t) sr.cap * ik_rb()) != cudaSuccess) { err = "ds4: index-key cache alloc"; return -1; }
      cudaMemset(sr.ik, 0, (std::size_t) sr.cap * ik_rb());
    }
    return idx;
  };

  L.assign((std::size_t) g.nl, Layer{});
  int eg_seen = 0;
  for (int il = 0; il < g.nl; ++il) {
    Layer& l = L[(std::size_t) il];
    l.il = il;
    l.ratio = g.ratios[(std::size_t) il];
    const int kvsrc = il < (int) g.kv_src.size() ? g.kv_src[(std::size_t) il] : -1;
    const int ixsrc = il < (int) g.ix_src.size() ? g.ix_src[(std::size_t) il] : -1;
    if (kvsrc >= 0) {
      l.kv_src = true;
      l.src = entry(kvsrc, true, false);
      if (l.src < 0) return false;
    }
    if (ixsrc >= 0) {
      l.ix_src = true;
      l.owns_k = (ixsrc == il);
      l.ik_src = entry(ixsrc, false, true);
      if (l.ik_src < 0) return false;
    }
    for (int e : g.eg_layers)
      if (e == il) { l.eg = eg_seen++; break; }
    l.cand_src = (il == g.cand_layer);
    l.uses_cand = g.cand_topk > 0;
    l.n_exp = g.n_exp;
    l.k_exp = g.k_exp;

    const std::string b = "blk." + std::to_string(il) + ".";
    l.wq_a = need(b + "attn_q_a.weight");
    l.wq_b = need(b + "attn_q_b.weight");
    l.wkv = need(b + "attn_kv.weight");
    l.wo_b = need(b + "attn_output_b.weight");
    f32p(b + "attn_q_a_norm.weight", l.q_norm);
    f32p(b + "attn_kv_a_norm.weight", l.kv_norm);
    f32p(b + "attn_norm.weight", l.attn_norm);
    f32p(b + "ffn_norm.weight", l.ffn_norm);
    f32p(b + "attn_sinks.weight", l.sink);
    f32p(b + "hc_attn_fn.weight", l.hca_fn);
    f32p(b + "hc_attn_scale.weight", l.hca_scale);
    f32p(b + "hc_attn_base.weight", l.hca_base);
    f32p(b + "hc_ffn_fn.weight", l.hcf_fn);
    f32p(b + "hc_ffn_scale.weight", l.hcf_scale);
    f32p(b + "hc_ffn_base.weight", l.hcf_base);
    l.s1 = need(b + "ffn_gate_shexp.weight");
    l.s3 = need(b + "ffn_up_shexp.weight");
    l.s2 = need(b + "ffn_down_shexp.weight");
    f32p(b + "ffn_gate_inp.weight", l.gate_w);      // router logits are read as F32 (fail-closed else)
    f32p(b + "exp_probs_b.bias", l.gate_bias);
    if (l.ratio > 1) {
      l.c_wkv = need(b + "attn_compressor_kv.weight");
      l.c_wgate = need(b + "attn_compressor_gate.weight");
      f32p(b + "attn_compressor_norm.weight", l.c_norm);
    }
    if (l.ix_src) {
      l.i_wq_b = need(b + "indexer.attn_q_b.weight");
      l.i_wk = need(b + "indexer.attn_k.weight");
      f32p(b + "indexer.k_norm.weight", l.ik_norm);
      // attn.indexer.weights_proj: the forward gemv_f16_rows wants BF16 specifically.
      {
        auto it = wm.find(b + "indexer.proj.weight");
        if (it == wm.end()) missing.push_back(b + "indexer.proj.weight");
        else if (it->second.q != k::Quant::bf16) missing.push_back(b + "indexer.proj.weight (BF16 required)");
        else l.i_wproj = (const uint16_t*) it->second.w;
      }
    }
    // ---- grouped output projection slices. The GGUF tensor maps NH*HD inputs ->
    // OG*o_lora rows (dims.front()=in, dims.back()=out, ggml order). Group gi takes
    // rows [gi*o_lora, (gi+1)*o_lora) and columns [gi*per, (gi+1)*per); repacked to
    // a compact contiguous [o_lora rows][per cols] matrix (the pack stores one entry
    // a group, "attn.wo_a.slice"), so wmul's kernels see rows with pitch exactly n_in.
    // Byte geometry: F32/BF16 rows are element rows; Q6_K super-blocks 256 elements
    // along the input row (162 B per block), so a byte-level column slice stays
    // contiguous exactly when the group boundaries are 256-aligned — true for every
    // published ds4 shape. Any other quant fails closed (box: extend; the ds4 GGUFs
    // ship f32/bf16/q6_k on this tensor).
    if (OG > 1) {
      const GgufTensor* t = gguf.tensor(b + "attn_output_a.weight");
      if (t == nullptr) { missing.push_back(b + "attn_output_a.weight"); }
      else if (t->dims.empty() || (int) t->dims.front() != NH * HD || (int) t->dims.back() != OG * g.o_lora) {
        err = "ds4: " + t->name + " dims are not (heads*head_dim x groups*lora)";
        return false;
      } else {
        std::size_t row_pitch = 0, slice_w = 0, col_esz = 0;
        switch (t->type) {
          case TensorType::f32:  row_pitch = (std::size_t) NH * HD * 4u; slice_w = (std::size_t) per * 4u; col_esz = 4u; break;
          case TensorType::bf16: row_pitch = (std::size_t) NH * HD * 2u; slice_w = (std::size_t) per * 2u; col_esz = 2u; break;
          case TensorType::q6_k:
            if ((NH * HD) % 256 != 0 || per % 256 != 0) {
              err = "ds4: attn_output_a Q6_K group columns are not 256-aligned (box: dequant instead)";
              return false;
            }
            row_pitch = ((std::size_t) NH * HD / 256u) * 162u;
            slice_w = ((std::size_t) per / 256u) * 162u;
            break;
          default:
            err = "ds4: attn_output_a type " + std::to_string((int) t->type) +
                  " is not sliceable at load (box: extend; published files ship f32/bf16/q6_k here)";
            return false;
        }
        const std::uint8_t* src = t->host_data ? t->host_data : mapped + t->offset;
        for (int gi = 0; gi < OG; ++gi) {
          const std::size_t c0 =
              col_esz != 0 ? (std::size_t) gi * per * col_esz : ((std::size_t) gi * per / 256u) * 162u;
          std::uint8_t* dst = woa_arena + woa_at + (std::size_t) gi * g.o_lora * slice_w;
          const std::uint8_t* srow = src + (std::size_t) gi * g.o_lora * row_pitch + c0;
          if (cudaMemcpy2D(dst, slice_w, srow, row_pitch, slice_w, (std::size_t) g.o_lora,
                           cudaMemcpyHostToDevice) != cudaSuccess) {
            err = "ds4: attn_output_a group slice copy";
            return false;
          }
          l.wo_a[gi].q = (k::Quant) (int) t->type;
          l.wo_a[gi].w = dst;
        }
        woa_at += (std::size_t) OG * g.o_lora * slice_w;
        woa_at = (woa_at + 255u) & ~std::uint64_t{255u};
      }
    }
    // ---- per-layer device caches: the FP8 raw-KV window ring; the compressor's
    // partial group (ratio x hd floats each, zeroed; Maya :4362-4363, :5740-5741).
    const std::size_t ring_rb = ds4::kv_row_bytes(ds4::kKvFp8, HD);
    if (cudaMalloc((void**) &l.win_ring, (std::size_t) g.win * ring_rb) != cudaSuccess) { err = "ds4: window ring alloc"; return false; }
    cudaMemset(l.win_ring, 0, (std::size_t) g.win * ring_rb);
    if (l.ratio > 1) {
      const std::size_t cb = (std::size_t) l.ratio * HD * sizeof(float);
      if (cudaMalloc((void**) &l.c_kv, cb) != cudaSuccess) { err = "ds4: compressor alloc"; return false; }
      if (cudaMalloc((void**) &l.c_score, cb) != cudaSuccess) { err = "ds4: compressor score alloc"; return false; }
      cudaMemset(l.c_kv, 0, cb);
      cudaMemset(l.c_score, 0, cb);
    }
  }
  if (!missing.empty()) { err = "ds4: GGUF is missing required tensor(s), first: " + missing[0]; return false; }

  // head + embedding.
  f32p("output_norm.weight", head_norm);
  head = need("output.weight");
  if (!missing.empty()) { err = "ds4: head tensors missing, first: " + missing[0]; return false; }
  const GgufTensor* emb = gguf.tensor("token_embd.weight");
  if (emb == nullptr) { err = "ds4: token_embd.weight missing"; return false; }
  if (emb->type != TensorType::bf16) { err = "ds4: token_embd.weight must be BF16 (embed_pinned feeds BF16 rows)"; return false; }
  // host-side table (forward copies single rows over PCIe). The mapped image is the
  // source; TODO(box): pin the hot row range / prefetch, as the reference bounces do.
  embed_pinned = (uint16_t*) (emb->host_data ? emb->host_data : mapped + emb->offset);
  return true;
}

// The two RoPE frequency tables, ported from ds4_model.cu rope_freqs (:500-521):
// the window/MLA base gets YaRN from the rope.scaling.* keys (identity when no
// original_context_length), the compressor base uses attention.compress_rope_freq_base
// with no YaRN. rd/2 F32 entries each (Maya sizes them 64 floats).
bool Ds4Graph::Impl::init_freqs(const GgufFile& gguf, std::string& err) {
  const std::string a = geom0.arch + ".";
  auto num = [&](const char* s, double def) -> double {
    const GgufValue* v = gguf.find(a + s);
    if (v == nullptr || v->type == GgufType::array) return def;
    return v->type == GgufType::f32 || v->type == GgufType::f64 ? v->real : (double) v->integer;
  };
  const int orig = (int) num("rope.scaling.original_context_length", 0.0);
  double factor = num("rope.scaling.factor", 1.0);
  double bf = num("rope.scaling.yarn_beta_fast", 32.0);
  double bs = num("rope.scaling.yarn_beta_slow", 1.0);
  if (factor <= 0.0) factor = 1.0;
  if (bf <= 0.0) bf = 32.0;
  if (bs <= 0.0) bs = 1.0;
  const std::vector<float> fw = rope_freqs(g.rd, orig, g.rope_theta, factor, bf, bs);
  const std::vector<float> fc = rope_freqs(g.rd, 0, g.comp_theta, 1.0, 32.0, 1.0);
  const std::size_t nb = fw.size() * sizeof(float);
  DS4_CK(cudaMalloc((void**) &freqs_win, nb));
  DS4_CK(cudaMalloc((void**) &freqs_comp, nb));
  DS4_CK(cudaMemcpy(freqs_win, fw.data(), nb, cudaMemcpyHostToDevice));
  DS4_CK(cudaMemcpy(freqs_comp, fc.data(), nb, cudaMemcpyHostToDevice));
  return true;
}

// Register every layer's routed-expert stack with the shared tier (host-mapped
// WSlots; the tier owns VRAM/RAM placement and the CPU lane), then finalize VRAM
// residency warm-started from the hit profile — the same shape glm_graph.cu's load
// proved on the box, with the ds4 profile env names. The reference's residency
// equivalents are place_experts (:4541) / load_experts (:5293).
bool Ds4Graph::Impl::init_experts(const GgufFile& gguf, const std::uint8_t* mapped, std::string& err) {
  experts = std::make_unique<ExpertTier>();
  for (int il = 0; il < g.nl; ++il) {
    const std::string b = "blk." + std::to_string(il) + ".";
    auto slot = [&](const char* nm) -> WSlot {
      const GgufTensor* t = gguf.tensor(b + nm);
      WSlot s;
      if (t == nullptr) return s;
      s.type = t->type == TensorType::f32 ? 0 : (int) t->type;
      const std::uint8_t* src = t->host_data ? t->host_data : mapped + t->offset;  // split-model aware
      s.q = (const void*) src;
      s.f32 = t->type == TensorType::f32 ? (const float*) src : nullptr;
      s.n_in = g.dim;
      s.n_out = g.ff;
      return s;
    };
    const WSlot gate = slot("ffn_gate_exps.weight"), up = slot("ffn_up_exps.weight"),
                down = slot("ffn_down_exps.weight");
    if (gate.q == nullptr || up.q == nullptr || down.q == nullptr) {
      err = "ds4: blk." + std::to_string(il) + " routed expert tensors (ffn_gate/up/down_exps) missing";
      return false;
    }
    if (!experts->register_layer(il, gate, up, down, g.n_exp, g.ff, g.swiglu_limit, err)) return false;
  }
  // ---- hot-expert VRAM residency: spend the free VRAM (less a calibratable headroom)
  // on resident expert slots; cold-start, the gated reg_rebalance in generate()
  // converges the resident set onto the hot experts. Budget 0 = fully streaming.
  {
    std::size_t vfree = 0, vtotal = 0;
    cudaMemGetInfo(&vfree, &vtotal);
    const std::size_t headroom = (std::size_t) Calibration::profile().residency_headroom_mib << 20;
    const std::int64_t budget = vfree > headroom ? (std::int64_t) (vfree - headroom) : 0;
    std::vector<std::pair<std::int32_t, std::int32_t>> ranked;
    if (const char* prof = std::getenv("STRATA_DS4_EXPERT_PROFILE")) {
      std::int64_t pslots = 0;
      std::string perr;
      if (read_expert_profile(prof, g.nl, g.n_exp, ranked, pslots, perr)) {
        std::fprintf(stderr, "ds4: warm-start profile '%s' (%zu ranked experts)\n", prof, ranked.size());
      } else {
        std::fprintf(stderr, "ds4: WARN profile '%s' ignored: %s (cold start)\n", prof, perr.c_str());
        ranked.clear();
      }
    }
    if (!experts->reg_finalize(budget, ranked, Calibration::profile().warm_seed,
                               Calibration::profile().stream_prefetch, err))
      return false;
    experts->reg_set_cpu_lane(Calibration::profile().cpu_expert_frac, Calibration::profile().cpu_workers);
    std::fprintf(stderr,
                 "ds4: expert residency %lld VRAM slots (%lld/layer of %d), %.2f GiB, %s start; the rest "
                 "stream from host\n",
                 (long long) experts->reg_slots(), (long long) (experts->reg_slots() / std::max(1, g.nl)),
                 g.n_exp, (double) ((std::int64_t) vfree - (std::int64_t) headroom) / (1024.0 * 1024.0 * 1024.0),
                 ranked.empty() ? "cold" : "warm");
    std::fflush(stderr);
  }
  return true;
}

bool Ds4Graph::Impl::alloc_scratch(std::string& err) {
  const int D = g.dim, H = std::max(1, g.hc), HD = g.hd, NH = g.heads, FF = g.ff;
  const int W = kWin;
  auto da = [&](auto** pp, std::size_t count) -> bool {
    using T = std::remove_reference_t<decltype(**pp)>;
    void* q = nullptr;
    const cudaError_t e = cudaMalloc(&q, count * sizeof(T));
    if (e != cudaSuccess) { err = std::string("ds4 alloc: ") + cudaGetErrorString(e); return false; }
    *pp = static_cast<T*>(q);
    return true;
  };
  // rows of up to kWin tokens; routed-expert rows kWin*kPicks (ds4_model.cu:2750+).
  const std::size_t WH = (std::size_t) W * H * D;
  if (!(da(&h, WH) && da(&h2, WH) && da(&x, (std::size_t) W * D) && da(&pre, (std::size_t) W * 8) &&
        da(&mix, (std::size_t) W * 32) && da(&a_pre, (std::size_t) W * 8) && da(&a_post, (std::size_t) W * 8) &&
        da(&a_comb, (std::size_t) W * 64) && da(&f_pre, (std::size_t) W * 8) && da(&f_post, (std::size_t) W * 8) &&
        da(&f_comb, (std::size_t) W * 64) && da(&qr, (std::size_t) W * std::max(1, g.q_lora)) &&
        da(&q, (std::size_t) W * NH * HD) && da(&kvr, (std::size_t) W * HD) && da(&ckr, (std::size_t) W * HD) &&
        da(&cgr, (std::size_t) W * HD) && da(&latent, (std::size_t) W * HD) &&
        da(&ikr, (std::size_t) W * std::max(1, g.ix_hd)) &&
        da(&iq, (std::size_t) W * std::max(1, g.ix_heads) * std::max(1, g.ix_hd)) && da(&iw, (std::size_t) W * 64) &&
        da(&scores, (std::size_t) W * (g.context + 64)) && da(&o, (std::size_t) W * NH * HD) &&
        da(&oa, (std::size_t) W * std::max(1, g.o_groups) * std::max(1, g.o_lora)) &&
        da(&oa_scr, (std::size_t) W * std::max(1, g.o_lora)) && da(&ao, (std::size_t) W * D) &&
        da(&glog, (std::size_t) W * 1024) && da(&sg, (std::size_t) W * FF) && da(&su, (std::size_t) W * FF) &&
        da(&sh, (std::size_t) W * FF) && da(&sh_out, (std::size_t) W * D) &&
        da(&rw, (std::size_t) W * kPicks) && da(&moe_routed, (std::size_t) W * D) &&
        da(&logits_dev, (std::size_t) W * g.vocab) && da(&emb_rows, (std::size_t) W * D) &&
        da(&ext, (std::size_t) (g.win + W) * HD) && da(&ridx, (std::size_t) W * kPicks) &&
        da(&topk, (std::size_t) W * topk_stride()) && da(&cand, (std::size_t) W * cand_stride()) && da(&tok_dev, W)))
    return false;
  DS4_CK(cudaMallocHost((void**) &h_emb, (std::size_t) W * D * sizeof(uint16_t)));
  // Q8_1 scratch for the widest matvec (dim or ff, up to 8 columns a call) and a
  // one-column BF16 activation buffer for bf16_gemv.
  const int max_in = std::max(std::max(D, FF), std::max(std::max(1, g.q_lora), NH * HD));
  DS4_CK(cudaMalloc(&q8_scratch, k::q8_1_scratch_bytes(max_in, 8)));
  if (!da(&x16, (std::size_t) max_in)) return false;
  // tks / attn_scr sizing: ds4_model.cu:2768-2769. Left for the box when the index
  // dims are wired (topk_stride()/ix_hd depend on ds4.* keys).
  return true;
}

// The quant-dispatched matvec. See the declaration for the contract.
void Ds4Graph::Impl::wmul(const Weight& m, const float* xin, float* y, int n_in, int n_out, int T) {
  if (m.q == k::Quant::f32) {
    for (int t = 0; t < T; ++t)
      k::f32_gemv(xin + (std::size_t) t * n_in, (const float*) m.w, y + (std::size_t) t * n_out, n_in, n_out, st);
  } else if (m.q == k::Quant::bf16) {
    for (int t = 0; t < T; ++t) {
      k::f32_to_bf16(xin + (std::size_t) t * n_in, x16, n_in, st);
      k::bf16_gemv(x16, (const uint16_t*) m.w, y + (std::size_t) t * n_out, n_in, n_out, st);
    }
  } else {
    for (int c0 = 0; c0 < T; c0 += 8) {
      const int nc = std::min(8, T - c0);
      k::mmvq_f32(m.q, m.w, xin + (std::size_t) c0 * n_in, q8_scratch, y + (std::size_t) c0 * n_out, n_in, n_out, nc,
                  st);
    }
  }
}

// --------------------------------------------------------------- the forward pass

// One transformer layer over a window of T tokens at p0, single GPU. Faithful to
// the host-routed serve path of ds4_model.cu layer() (:11432-11941), re-expressed
// on the shared kernels; TP / async-mailbox / EXL3-class branches dropped (§1.2).
bool Ds4Graph::Impl::layer_forward(Layer& l, int64_t p0, int T, std::string& err) {
  const int D = g.dim, H = std::max(1, g.hc), HD = g.hd, NH = g.heads, FF = g.ff;
  const float* freqs = l.ratio ? freqs_comp : freqs_win;

  // -- Engram (only engram layers; ds4_model.cu:11445-11460). STRUCTURED: the
  //    kernels are stubs (ds4_mla engram_*); the host n-gram read is the box's.
  if (l.eg >= 0 && l.eg_w != nullptr) {
    // TODO(box): issue the n-gram rows (ds4_model.cu eg_issue :6254 / eg_finish
    //   :6334), dequant, project eg_wkv, then engram_gate into h. Skipped here.
  }

  // -- Attention block: hyper-connection mix/split, then MLA.
  ds4::hc_mix(h, l.hca_fn, H * D, (2 + H) * H, T, g.eps, mix, st);
  ds4::hc_split(mix, l.hca_scale, l.hca_base, H, g.sinkhorn, g.hc_eps, T, a_pre, a_post, a_comb, st);
  ds4::hc_pre_norm(h, pre, l.attn_norm, D, H, T, g.eps, x, st);

  // query: down (wq_a) -> RMSNorm(q_norm) -> up (wq_b) -> RoPE
  wmul(l.wq_a, x, qr, D, g.q_lora, T);
  ds4::Finish fq; fq.w = l.q_norm; fq.eps = g.eps;
  ds4::finish(qr, T, g.q_lora, g.q_lora, fq, st);
  wmul(l.wq_b, qr, q, g.q_lora, NH * HD, T);
  ds4::Finish fr; fr.rd = g.rd; fr.pos = (int) p0; fr.vpp = NH; fr.freqs = freqs;
  ds4::finish(q, T * NH, HD, HD, fr, st);

  // KV latent for this window's rows -> the FP8 window ring.
  ds4::ring_to_ext(l.win_ring, ext, p0, g.win, HD, st);
  wmul(l.wkv, x, kvr, D, HD, T);
  ds4::Finish fk; fk.w = l.kv_norm; fk.eps = g.eps; fk.rd = g.rd; fk.pos = (int) p0; fk.freqs = freqs;
  fk.quant = 1; fk.dst = ext + (std::size_t) g.win * HD; fk.dst_stride = HD;
  ds4::finish(kvr, T, HD, HD, fk, st);
  ds4::ext_to_ring(ext, l.win_ring, p0, g.win, T, HD, st, nullptr);

  // per-query window extents; nsel filled after the index selects.
  ds4::AttnRows ar{};
  for (int t = 0; t < T; ++t) {
    const int first = (int) std::max<int64_t>(g.win - p0, t + 1);
    ar.win0[t] = first;
    ar.nwin[t] = g.win + t - first + 1;
    ar.nsel[t] = 0;
  }

  const void* ckv = nullptr;
  if (l.ratio > 0 && l.src >= 0) {
    Src& sr = srcs[(std::size_t) l.src];
    const int R = l.ratio;
    const int64_t g_first = p0 / R, g_end = (p0 + T) / R;
    const int ng = (int) std::max<int64_t>(0, g_end - g_first);
    // compress this window's KV into the latent groups.
    if (l.kv_src) {
      wmul(l.c_wkv, x, ckr, D, HD, T);
      if (R > 1) {
        wmul(l.c_wgate, x, cgr, D, HD, T);
        ds4::compress_win(ckr, cgr, l.c_kv, l.c_score, R, (int) p0, T, HD, l.c_norm, g.eps, latent, st);
      } else {
        ds4::Finish fc; fc.w = l.c_norm; fc.eps = g.eps;
        ds4::finish(ckr, T, HD, HD, fc, st);
        DS4_CK(cudaMemcpyAsync(latent, ckr, (std::size_t) T * HD * 4, cudaMemcpyDeviceToDevice, st));
      }
    }
    // per-query compressed-position limits.
    ds4::Rows lim{};
    int clen_max = 0;
    for (int t = 0; t < T; ++t) {
      lim.n[t] = (int) ((p0 + t + 1) / R);
      lim.k[t] = std::min(g.ix_topk, lim.n[t]);
      clen_max = std::max(clen_max, lim.n[t]);
    }
    // the learned sparse index: score the compressed positions and pick top-k.
    if (l.ix_src && l.ik_src >= 0) {
      Src& ks = srcs[(std::size_t) l.ik_src];
      if (l.owns_k && ng > 0) {
        wmul(l.i_wk, latent, ikr, HD, g.ix_hd, ng);
        ds4::Finish fi; fi.rd = g.rd; fi.pos = (int) (g_first * R); fi.pos_step = R; fi.freqs = freqs;
        fi.quant = 3; fi.pdst = ks.ik + (std::size_t) g_first * ik_rb(); fi.pdst_stride = (long long) ik_rb();
        ds4::finish(ikr, ng, g.ix_hd, g.ix_hd, fi, st);
      }
      if (clen_max > 0) {
        wmul(l.i_wq_b, qr, iq, g.q_lora, g.ix_heads * g.ix_hd, T);
        ds4::Finish fi; fi.rd = g.rd; fi.pos = (int) p0; fi.vpp = g.ix_heads; fi.freqs = freqs; fi.quant = 3;
        ds4::finish(iq, T * g.ix_heads, g.ix_hd, g.ix_hd, fi, st);
        ds4::gemv_f16_rows(l.i_wproj, x, (std::size_t) D, g.ix_heads, D, T,
                           1.0f / std::sqrt((float) g.ix_hd) / std::sqrt((float) g.ix_heads), iw, 64, st);
        const long long ss = (long long) g.context + 64;
        ds4::index_score(iq, iw, ks.ik, g.ix_heads, g.ix_hd, T, clen_max, nullptr, 0, 1, lim, scores, ss, st);
        if (l.cand_src) {
          ds4::Rows cr = lim;
          for (int t = 0; t < T; ++t) cr.k[t] = g.cand_topk;
          ds4::cand_blocks(scores, ss, T, cr, g.cand_blk, /*bm*/ scores, bm_stride(), cand, cand_stride(), st);
        }
        ds4::topk(scores, ss, T, lim, nullptr, 0, 1, topk, topk_stride(), st, &tks);
      }
    }
    // write the compressed KV rows (FP4) for the layers that read this source.
    if (l.kv_src && ng > 0) {
      ds4::Finish fc; fc.rd = g.rd; fc.pos = (int) (g_first * R); fc.pos_step = R; fc.freqs = freqs;
      fc.quant = 2; fc.pdst = sr.ckv + (std::size_t) g_first * ckv_rb(); fc.pdst_stride = (long long) ckv_rb();
      ds4::finish(latent, ng, HD, HD, fc, st);
    }
    for (int t = 0; t < T; ++t) ar.nsel[t] = lim.n[t] > 0 ? std::min(g.ix_topk, lim.n[t]) : 0;
    ckv = sr.ckv;
  }

  // sparse attention over the window rows + the selected compressed rows.
  ds4::sparse_attn(q, ext, ckv, topk, topk_stride(), ar, l.sink, NH, HD, T,
                   1.0f / std::sqrt((float) HD), o, st, ds4::kKvFp4, 0, &attn_scr);
  ds4::Finish fo; fo.rd = g.rd; fo.pos = (int) p0; fo.vpp = NH; fo.freqs = freqs; fo.inverse = 1;
  ds4::finish(o, T * NH, HD, HD, fo, st);
  // output projection: grouped wo_a then wo_b. Each group's slice (a compact
  // [o_lora][per] repack made at load) writes [T][o_lora] into oa_scr; the groups
  // are then scattered into the interleaved oa rows [T][OG*o_lora] that wo_b reads
  // (the pack stores one entry a group the same way). T is at most kWin, so the
  // scatter is kWin*OG small D2D copies - correctness first, box can fuse it into
  // the gemv epilogue.
  const int OG = std::max(1, g.o_groups), per = (NH * HD) / OG;
  for (int gi = 0; gi < OG; ++gi) wmul(l.wo_a[gi], o + (std::size_t) gi * per, oa_scr, per, g.o_lora, T);
  for (int t = 0; t < T; ++t)
    for (int gi = 0; gi < OG; ++gi)
      if (cudaMemcpyAsync(oa + ((std::size_t) t * OG + gi) * g.o_lora, oa_scr + (std::size_t) t * g.o_lora,
                          (std::size_t) g.o_lora * sizeof(float), cudaMemcpyDeviceToDevice, st) != cudaSuccess) {
        err = "ds4: wo_a group scatter";
        return false;
      }
  wmul(l.wo_b, oa, ao, OG * g.o_lora, D, T);
  ds4::hc_post(ao, h, a_post, a_comb, D, H, T, h2, st);
  std::swap(h, h2);

  // -- MoE block: hyper-connection mix/split, then shared + routed experts.
  ds4::hc_mix(h, l.hcf_fn, H * D, (2 + H) * H, T, g.eps, mix, st);
  ds4::hc_split(mix, l.hcf_scale, l.hcf_base, H, g.sinkhorn, g.hc_eps, T, f_pre, f_post, f_comb, st);
  ds4::hc_pre_norm(h, a_pre, l.ffn_norm, D, H, T, g.eps, x, st);

  // the shared expert (always-on FFN). ds4::swiglu is DeepSeek's CLAMPED SwiGLU
  // (swiglu_limit); the generic k::swiglu is the unclamped join, so the clamp stays
  // a ds4 variant for parity.
  wmul(l.s1, x, sg, D, FF, T);
  wmul(l.s3, x, su, D, FF, T);
  ds4::swiglu(sg, su, T * FF, g.swiglu_limit, sh, st);
  wmul(l.s2, sh, sh_out, FF, D, T);

  // route (F32 logits per token) + the sigmoid router, then the routed experts via
  // the shared tier (VRAM/RAM/CPU hidden). ds4 is a sigmoid-routed family.
  // TODO(box): the reference scores experts as sqrt(softplus(logits)) (noaux_tc,
  //   ds4_kernels.cuh:96-98), not plain sigmoid. Confirm k::router_sigmoid matches
  //   DeepSeek's selection+weights for parity, else add a ds4 router variant.
  for (int t = 0; t < T; ++t)
    k::f32_gemv(x + (std::size_t) t * D, l.gate_w, glog + (std::size_t) t * l.n_exp, D, l.n_exp, st);
  k::router_sigmoid(glog, l.gate_bias, T, l.n_exp, l.k_exp, g.route_scale, /*norm_w*/ true, ridx, rw, st);
  if (!experts->run_window(l.il, x, ridx, rw, T, l.k_exp, D, FF, g.swiglu_limit, moe_routed, st, err)) return false;

  // moe result = routed + shared; then the hyper-connection recombine.
  k::add_inplace(moe_routed, sh_out, (std::int64_t) T * D, st);
  ds4::hc_post(moe_routed, h, f_post, f_comb, D, H, T, h2, st);
  std::swap(h, h2);
  DS4_CK(cudaMemcpyAsync(pre, f_pre, (std::size_t) T * 8 * 4, cudaMemcpyDeviceToDevice, st));
  return true;
}

bool Ds4Graph::Impl::head_forward(int T, std::string& err) {
  // collapse the hyper-connection streams, final RMSNorm, then the LM head. The
  // reference does this on the head device (head_rows_dev, ds4_model.cu:11173).
  const int D = g.dim, H = std::max(1, g.hc);
  ds4::hc_pre_norm(h, pre, head_norm, D, H, T, g.eps, x, st);
  // only the last n_logits rows need logits.
  const int r0 = std::max(0, T - n_logits);
  wmul(head, x + (std::size_t) r0 * D, logits_dev, D, g.vocab, n_logits);
  (void) err;
  return true;
}

// Feeds a window of T tokens at the current position, leaving logits for the last
// `logits_rows` rows on the device. Mirrors ds4_model.cu forward()/forward_serial
// (:11198-11310) for a single GPU.
bool Ds4Graph::Impl::forward(const int* tk, int T, int logits_rows, std::string& err) {
  if (T < 1 || T > kWin) { err = "ds4: a window of 1.." + std::to_string(kWin) + " tokens"; return false; }
  if (pos + T > g.context) { err = "ds4: the context is full"; return false; }
  for (int t = 0; t < T; ++t)
    if (tk[t] < 0 || tk[t] >= g.vocab) { err = "ds4: token out of range"; return false; }
  const int64_t p0 = pos;
  const int D = g.dim, H = std::max(1, g.hc);
  for (int t = 0; t < T; ++t) toks.push_back(tk[t]);
  n_logits = std::min(logits_rows, T);
  // embedding rows -> hyper-connection streams.
  for (int t = 0; t < T; ++t)
    std::memcpy(h_emb + (std::size_t) t * D, embed_pinned + (std::size_t) tk[t] * D, (std::size_t) D * 2);
  DS4_CK(cudaMemcpyAsync(emb_rows, h_emb, (std::size_t) T * D * 2, cudaMemcpyHostToDevice, st));
  ds4::embed(emb_rows, T, h, D, H, st);
  ds4::init_pre(pre, T, st);
  for (int il = 0; il < g.nl; ++il)
    if (!layer_forward(L[(std::size_t) il], p0, T, err)) return false;
  if (!head_forward(T, err)) return false;
  DS4_CK(cudaStreamSynchronize(st));
  pos += T;
  // TODO(box): seed the DSpark drafter blocks with this window's tap rows
  //   (ds4_model.cu seed_drafter :8946) so draft() can run next.
  return true;
}

// --------------------------------------------------------------- sampling

int Ds4Graph::Impl::sample(const k::SamplerParams& sp) {
  const int r0 = std::max(0, n_logits - 1);
  // Penalty history: the tail of the generated/prompt tokens (ds4_model.cu pen_rows
  // :9174 builds the same window over the token history; nullptr/0 = penalties off).
  const std::size_t hn = sp.penalty_last_n > 0
                             ? std::min<std::size_t>((std::size_t) sp.penalty_last_n, toks.size()) : 0u;
  const int* hist = hn > 0 ? toks.data() + (toks.size() - hn) : nullptr;
  k::sample_tokens(logits_dev + (std::size_t) r0 * g.vocab, 1, g.vocab, hist, (int) hn, sp, tok_dev, st);
  int t = -1;
  cudaMemcpyAsync(&t, tok_dev, 4, cudaMemcpyDeviceToHost, st);
  cudaStreamSynchronize(st);
  return t;
}

// --------------------------------------------------------------- DSpark drafter

// Proposes g.d_block draft tokens after y, each with a confidence. GREEDY core is
// faithful (the drafter's argmax); the SAMPLED speculative draws are a structured
// stub. Origin: ds4_model.cu draft_ (:9023-9169).
bool Ds4Graph::Impl::draft(int y, std::vector<int>& out, std::vector<float>& conf, const k::SamplerParams& sp,
                           std::string& err) {
  out.clear();
  conf.clear();
  if (g.n_mtp == 0 || g.d_block < 1) return true;   // no drafter loaded
  (void) y; (void) sp;
  // TODO(box): run the mtp.* blocks as window layers over [y, noise...], then the
  //   drafter's final norm + the trunk head, the Markov bias (ds4::markov_add), the
  //   per-position argmax (k::sample_tokens, greedy) and the confidence head
  //   (ds4::confidence). The SAMPLED path (speculative draws from the request's own
  //   chain, DS4_DSPARK_SAMPLE) and the TP Markov-share head are the deep stubs here.
  //   Reference: ds4_model.cu:9023-9169. Until wired, offer no drafts (the serve
  //   loop falls back to a plain step, which is correct, just not accelerated).
  (void) err;
  return true;
}

// Verifies [y, drafts] in one window and reports how many drafts the trunk accepts
// and the next token. GREEDY acceptance is faithful; the sampled speculative-
// sampling rule (min(1, p/q) with a residual draw) is a structured stub falling back
// to greedy. Origin: ds4_model.cu verify (:9231-9307).
bool Ds4Graph::Impl::verify(int y, const std::vector<int>& drafts, const k::SamplerParams& sp, int& accepted,
                            int& next, std::string& err) {
  const int n = (int) std::min<std::size_t>(drafts.size(), (std::size_t) kWin - 1);
  const int T = n + 1;
  int tk[kWin];
  tk[0] = y;
  for (int i = 0; i < n; ++i) tk[i + 1] = drafts[(std::size_t) i];
  // run the window once, logits for all T rows.
  const int64_t p0 = pos;
  if (!forward(tk, T, T, err)) return false;
  // greedy acceptance: the trunk's argmax agreeing with each draft in turn.
  std::vector<int> argmax((std::size_t) T);
  k::SamplerParams g0 = sp;
  g0.greedy = true;
  g0.temperature = 0.0f;
  // sample the T rows greedily (one call a row keeps it simple + matches the stub).
  for (int i = 0; i < T; ++i) {
    k::sample_tokens(logits_dev + (std::size_t) i * g.vocab, 1, g.vocab, nullptr, 0, g0, tok_dev, st);
    cudaMemcpyAsync(&argmax[(std::size_t) i], tok_dev, 4, cudaMemcpyDeviceToHost, st);
    cudaStreamSynchronize(st);
  }
  int m = 0;
  while (m < n && argmax[(std::size_t) m] == drafts[(std::size_t) m]) ++m;
  next = argmax[(std::size_t) m];
  accepted = m;
  // roll the state back to keep y + the m accepted drafts (positions p0 .. p0+m).
  // TODO(box): the ring / compressor rollback for the rejected tail
  //   (ds4_model.cu rollback :9311, ring_restore / comp_restore_batch) and re-seed
  //   the drafter (seed_drafter). Here we rewind the host position/tokens only; the
  //   device KV past p0+m+1 is overwritten when those positions are fed again.
  if (m + 1 < T) {
    pos = p0 + m + 1;
    toks.resize((std::size_t) pos);
  }
  if (!sp.greedy) {
    // Sampled speculative-sampling acceptance is the deep stub: we accepted on the
    // greedy rule above, which is exact only for temperature<=0. TODO(box): port the
    // min(1,p/q) + residual-draw rule (ds4_model.cu:9260-9295, k::sample_verify).
  }
  return true;
}

Ds4Graph::Impl::~Impl() {
  auto fr = [](auto*& p) { if (p) cudaFree(p); p = nullptr; };
  for (Layer& l : L) { fr(l.win_ring); fr(l.c_kv); fr(l.c_score); }
  for (Src& s : srcs) { fr(s.ckv); fr(s.ik); }
  fr(w_arena); fr(woa_arena); fr(freqs_win); fr(freqs_comp);
  fr(h); fr(h2); fr(x); fr(pre); fr(mix); fr(a_pre); fr(a_post); fr(a_comb); fr(f_pre); fr(f_post); fr(f_comb);
  fr(qr); fr(q); fr(kvr); fr(ckr); fr(cgr); fr(latent); fr(ikr); fr(iq); fr(iw); fr(scores);
  fr(o); fr(oa); fr(oa_scr); fr(ao);
  fr(glog); fr(sg); fr(su); fr(sh); fr(sh_out); fr(rw); fr(moe_routed); fr(logits_dev);
  fr(emb_rows); fr(ext); fr(ridx); fr(topk); fr(cand); fr(tok_dev); fr(x16);
  if (q8_scratch) cudaFree(q8_scratch);
  if (h_emb) cudaFreeHost(h_emb);
  if (st) cudaStreamDestroy(st);
}

// =============================================================================
// Ds4Graph — the public FamilyGraph.
// =============================================================================
Ds4Graph::Ds4Graph(std::unique_ptr<Impl> impl) : p_(std::move(impl)) {}
Ds4Graph::~Ds4Graph() = default;

std::uint32_t Ds4Graph::context() const { return p_->ctx; }
bool Ds4Graph::available() const { return p_ && p_->ready; }
std::string Ds4Graph::describe() const { return "ds4(MLA+latent-KV+index+DSpark)"; }

bool Ds4Graph::load(const GgufFile& gguf, const std::uint8_t* mapped, std::string& error) {
  return p_->load(gguf, mapped, error);
}

// One generation request on the converged `u` protocol. Mirrors the serve/round
// loop of engines/maya-ref/src/ds4/ds4_main.cpp (serve :361, round :204, the
// decode loop :1002-1044), emitting PP / T / DONE through the sink.
bool Ds4Graph::generate(const Gen& request, const LineSink& sink, std::string& error) {
  Impl& m = *p_;
  if (!m.ready) { error = "ds4: model not loaded"; return false; }
  m.pos = 0;
  m.toks.clear();

  const std::size_t total = request.ids.size();
  const double t0 = now_ms();

  // ---- prefill: feed the prompt in windows of kWin, emitting PP progress.
  std::size_t done = 0;
  std::vector<int> ids(request.ids.begin(), request.ids.end());
  while (done < total) {
    const int n = (int) std::min<std::size_t>((std::size_t) kWin, total - done);
    const bool last = done + (std::size_t) n == total;
    // the prompt's last token gets a logit row (we sample from it); the rest none.
    if (!m.forward(ids.data() + done, n, last ? 1 : 0, error)) return false;
    done += (std::size_t) n;
    const double ms = now_ms() - t0;
    sink(emit(PromptProgress{(std::uint32_t) done, (std::uint32_t) total, ms,
                             ms > 0 ? 1000.0 * (double) done / ms : 0.0}));
  }

  // ---- decode: speculative rounds when a drafter is loaded, else a plain step.
  const double t1 = now_ms();
  // Parse the request's sampler params (the converged protocol's GEN key=values) into
  // u::k::SamplerParams - the same mapping glm's sample_next applies, so a client
  // drives both families identically (ds4_main parse_request's counterpart).
  k::SamplerParams sp;   // the struct's own defaults (top_k 20, top_p 0.95, temp 1.0) stand unless set
  sp.counter = (std::uint64_t) m.pos;
  for (const KeyVal& kv : request.params) {
    try {
      if (kv.key == "temperature" || kv.key == "temp") sp.temperature = std::stof(kv.value);
      else if (kv.key == "top_k") sp.top_k = std::stoi(kv.value);
      else if (kv.key == "top_p") sp.top_p = std::stof(kv.value);
      else if (kv.key == "min_p") sp.min_p = std::stof(kv.value);
      else if (kv.key == "seed") sp.seed = (std::uint64_t) std::stoull(kv.value);
      else if (kv.key == "penalty_last_n") sp.penalty_last_n = std::stoi(kv.value);
      else if (kv.key == "penalty_repeat") sp.penalty_repeat = std::stof(kv.value);
    } catch (...) {
      error = "ds4: malformed sampler param '" + kv.key + "'";
      return false;
    }
  }
  if (sp.temperature <= 0.0f) sp.greedy = true;

  const std::int64_t limit =
      request.max_new > 0 ? (std::int64_t) request.max_new : m.g.context - (std::int64_t) total;
  std::int64_t produced = 0;
  std::uint32_t spec_hits = 0, spec_steps = 0;
  int since_reb = 0;
  Finish finish = Finish::length;

  int tok = m.sample(sp);
  std::vector<int> got;
  for (;;) {
    sink(emit(Token{(std::uint32_t) tok}));
    ++produced;
    // gated residency rebalance (glm pattern): converge the resident set on the hot experts.
    if (++since_reb >= u::Calibration::profile().rebalance_every) {
      since_reb = 0;
      if (m.experts->reg_rebalance(u::Calibration::profile().rebalance_margin, nullptr, error) < 0) return false;
    }
    bool is_eos = false;   // TODO(box): the request/engine EOS set (ds4_main eos list).
    if (is_eos) { finish = Finish::stop; break; }
    if (produced >= limit || m.pos >= m.g.context) { finish = Finish::length; break; }
    if (got.empty()) {
      const int room = (int) std::min<std::int64_t>(limit - produced, m.g.context - m.pos);
      std::vector<int> drafts;
      std::vector<float> conf;
      int acc = 0, nxt = tok;
      bool did_spec = false;
      if (room > 1 && m.g.n_mtp > 0 && m.draft(tok, drafts, conf, sp, error) && !drafts.empty()) {
        // keep drafts while confident and within room.
        int keep = 0;
        while (keep < (int) drafts.size() && keep < room - 1 && conf[(std::size_t) keep] >= 0.5f) ++keep;
        drafts.resize((std::size_t) keep);
        if (keep > 0) {
          if (!m.verify(tok, drafts, sp, acc, nxt, error)) return false;
          for (int i = 0; i < acc; ++i) got.push_back(drafts[(std::size_t) i]);
          got.push_back(nxt);
          spec_steps += (std::uint32_t) keep;
          spec_hits += (std::uint32_t) acc;
          did_spec = true;
        }
      }
      if (!did_spec) {
        if (!m.step(tok, true, error)) return false;
        got.push_back(m.sample(sp));
      }
      std::reverse(got.begin(), got.end());
    }
    tok = got.back();
    got.pop_back();
  }

  // Optional routing-profile save: an STRP profile of this run's expert hits, to warm
  // start a later run (STRATA_DS4_EXPERT_PROFILE_SAVE; glm's save block, same format).
  if (const char* save = std::getenv("STRATA_DS4_EXPERT_PROFILE_SAVE")) {
    std::string serr;
    if (!write_expert_profile(save, m.g.nl, m.g.n_exp, m.experts->reg_ranked(), serr))
      std::fprintf(stderr, "ds4: WARN profile save '%s' failed: %s\n", save, serr.c_str());
    else
      std::fprintf(stderr, "ds4: wrote expert profile '%s'\n", save);
    std::fflush(stderr);
  }

  const double t2 = now_ms();
  sink(emit(Done{(std::uint32_t) produced, (std::uint32_t) total, t1 - t0, t2 - t1, finish, spec_hits, spec_steps,
                 /*reused*/ 0}));
  return true;
}

// The factory. Builds the graph for the guarded geometry + serving context and
// returns it WITHOUT loading: the substrate drives the FamilyGraph::load seam with
// the mapped multi-part GGUF right after (u_main.cpp:423) — the same contract
// make_glm_graph uses. available() reports false until that load lands.
std::unique_ptr<FamilyGraph> make_ds4_graph(const Geometry& geom, std::uint32_t context, std::string& error) {
  error.clear();
  if (geom.family != FamilyId::ds4) { error = "make_ds4_graph: not a ds4 geometry"; return nullptr; }
  auto impl = std::make_unique<Ds4Graph::Impl>();
  impl->geom0 = geom;
  impl->ctx = context;
  return std::unique_ptr<FamilyGraph>(new Ds4Graph(std::move(impl)));
}

}  // namespace u

#endif  // STRATA_U_CUDA
