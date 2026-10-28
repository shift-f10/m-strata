// engines/unified/src/family/glm_graph.cu - the GLM-5.3-Flash (arch "glm5next") family graph.
//
// PORTED FROM Project Maya's Glm5Model: the correctness-first per-op decode path
//   engines/maya-ref/src/core/glm_model.cu::step_layers (L2198), ::load (the arena carve, L747),
//   ::forward (L997), ::reset (L834)
// and the reference engines/maya-ref/ref/glm.py (the op-by-op specification). The GPU box compiles and
// parity-checks this against glm.py's dumps (design §1.3; plan T1.3/T1.6). It is NOT built on the host
// (the host links u::detail::HostStubGraph - u_family.hpp).
//
// WHY THE PER-OP PATH, NOT THE FAST PATH: the reference ships a heavily tuned fast path
// (glm_fast_path.cu, glm_fast.cu, CUDA-graph capture, KV-streaming residency, the pipelined split)
// worth thousands of lines. The unified engine's lean mandate (design §1.2: "port only what we serve")
// makes the correctness-first per-op path the right first port: it IS the decode path (one token at a
// time, the state carried in the caches), it is what the parity gate checks, and the fast path's
// residency/graph/split optimizations are a later, box-measured layer. The deep draft path
// (NextN/DFlash) is structured in kernels/glm_dflash.cuh with a box TODO.
//
// ========================= SHARED CONTRACTS THIS GRAPH RELIES ON =========================
// Generic kernels: kernels/common.cuh, namespace u::k (landed by lane T1.1). This graph uses, by their
// real signatures there: f32_gemv / bf16_gemv / mmvq_f32 (the matmul, dispatched by gemv_() below),
// rms_norm (IN PLACE), add_inplace, scale_inplace, f32_to_bf16, router_sigmoid, SamplerParams +
// sample_tokens. GLM-specific ops NOT in common.cuh are provided by this family's own kernels
// (kernels/glm_*): the clamped SwiGLU (glm_swiglu_clamp), the indexer LayerNorm (glm_layer_norm), the
// beta sigmoid (glm_sigmoid_inplace), and the KDA/DSA/mHC/NextN kernels.
//
// Expert tier: u_experts.hpp, u::ExpertTier (lane T1.2). u_experts.hpp is not landed yet; the interface
// this graph assumes is reproduced below so the data flow is unambiguous, and the box reconciles the
// exact names against the finalized header (the same seam the ds4/raon1 lanes carry):
//
//   class u::ExpertTier {   // the shared VRAM/RAM/CPU MoE facility + expert-hit profile (design §3.5)
//    public:
//     // Receives one MoE layer's routed-expert row tensors at load (sigmoid-routed 288 experts of
//     // n_ff_exp each, with the GLM-family clamped SwiGLU at `swiglu_limit`).
//     bool register_layer(int layer, const u::WSlot& gate_exps, const u::WSlot& up_exps,
//                         const u::WSlot& down_exps, int n_expert, int n_ff_exp, float swiglu_limit,
//                         std::string& err);
//     // out[n_embd] = sum_i weights[i] * down_e(clamped_swiglu(gate_e x, up_e x)) over the k ids[i].
//     // ids/weights are DEVICE pointers (u::k::router_sigmoid's output, k contiguous for T=1); xq is
//     // the Q8_1 activation the tier's mmvq wants, or null to let the tier quantize x itself.
//     bool run_moe(int layer, const int* ids, const float* weights, int k, const float* x,
//                  const void* xq, float* out, void* stream, std::string& err);
//   };
// =========================================================================================
#include "family/glm_graph.hpp"

#include "kernels/common.cuh"     // u::k generic kernels (landed, lane T1.1)
#include "kernels/glm_kda.cuh"
#include "kernels/glm_dsa.cuh"
#include "kernels/glm_hc.cuh"
#include "kernels/glm_ffn.cuh"
#include "kernels/glm_dflash.cuh"
#include "u_experts.hpp"          // u::ExpertTier (lane T1.2; interface assumed above until landed)
#include "u_gguf.hpp"             // u::GgufFile, u::GgufTensor
#include "u_calibration.hpp"      // u::Calibration (the per-box adaptive-tier profile, spec C1)

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace u {

namespace {
#define GLM_CU(call, err)                                                                            \
  do {                                                                                               \
    const cudaError_t e_ = (call);                                                                   \
    if (e_ != cudaSuccess) {                                                                         \
      (err) = std::string("glm5next: ") + cudaGetErrorString(e_) + " @ " __FILE__ ":" +              \
              std::to_string(__LINE__);                                                              \
      return false;                                                                                  \
    }                                                                                                \
  } while (0)
}  // namespace

GlmGraph::GlmGraph(GlmShape shape, std::uint32_t context)
    : g_(std::move(shape)), context_(context), max_ctx_(context) {}

GlmGraph::~GlmGraph() {
  if (w_arena_) cudaFree(w_arena_);
  if (dsa_f32_) cudaFree(dsa_f32_);
  if (state_) cudaFree(state_);
  if (sc_) cudaFree(sc_);
  if (d_pos_) cudaFree(d_pos_);
  if (d_tok_) cudaFree(d_tok_);
  if (q8_scratch_) cudaFree(q8_scratch_);
  if (bf16_x_) cudaFree(bf16_x_);
}

std::string GlmGraph::describe() const {
  int kda = 0;
  for (int il = 0; il < g_.n_layers; ++il) kda += g_.is_recr(il) ? 1 : 0;
  return "glm5next(" + std::to_string(g_.n_layers) + "L " + std::to_string(kda) + "KDA/" +
         std::to_string(g_.n_layers - kda) + "DSA, " + std::to_string(g_.n_expert) + "x" +
         std::to_string(g_.n_exp_used) + "+1 moe, ctx " + std::to_string(context_) + ")";
}

const WSlot& GlmGraph::W(int il, const char* suffix) const {
  return ws_.at("blk." + std::to_string(il) + "." + suffix);
}

// y[0,n_out) = W x, dispatched on the weight's stored type (see glm_graph.hpp). Errors surface via
// cudaGetLastError at the next synchronizing call (the shared kernels are void launches).
void GlmGraph::gemv_(const WSlot& w, const float* x, float* out, std::int64_t n_out, std::int64_t n_in,
                     void* stream) {
  if (w.type == 0) {   // F32
    k::f32_gemv(x, w.f32, out, n_in, n_out, stream);
  } else if (w.type == (int) k::Quant::bf16) {
    k::f32_to_bf16(x, bf16_x_, n_in, stream);
    k::bf16_gemv(bf16_x_, (const std::uint16_t*) w.q, out, n_in, n_out, stream);
  } else {
    k::mmvq_f32((k::Quant) w.type, w.q, x, q8_scratch_, out, (int) n_in, (int) n_out, 1, stream);
  }
}

void GlmGraph::rmsnorm_to(float* out, const float* in, const float* w, std::int64_t n, float eps,
                          void* stream) {
  cudaMemcpy(out, in, (std::size_t) n * sizeof(float), cudaMemcpyDeviceToDevice);
  k::rms_norm(out, w, 1, n, eps, stream);
}

// ================================ load: weights + arenas ================================
//
// SEAM: u::make_family_graph / make_glm_graph take only the Geometry (u_family.hpp), not the GGUF, so
// the substrate's dispatch calls load() with the mapped GGUF after construction (or make_glm_graph is
// extended to receive it). Faithful port of the reference's upload loop + state/scratch carve.
bool GlmGraph::load(const GgufFile& gguf, const std::uint8_t* mapped, std::string& err) {
  // ---- per-layer schedule from the GGUF. make_glm_graph set an all-KDA PLACEHOLDER (it has no file);
  // the real KDA/DSA pattern (glm5-next.attention.head_count_kv: 0 => KDA, nonzero => DSA) and the NextN
  // count are checkpoint-specific and MUST come from the file before the arena is sized or the forward
  // trusted. Arch key prefix is "glm5-next." (llama.cpp) or "glm5next." (unsloth).
  auto meta = [&](const std::string& suffix) -> const GgufValue* {
    if (const auto* v = gguf.find("glm5-next." + suffix)) return v;
    return gguf.find("glm5next." + suffix);
  };
  {
    const int block_count = g_.n_layers;  // make_glm_graph seeded n_layers = block_count
    int nextn = 0;
    if (const auto* v = meta("nextn_predict_layers"); v != nullptr && v->type != GgufType::array)
      nextn = (int) v->integer;
    const GgufValue* hk = meta("attention.head_count_kv");
    if (hk == nullptr || hk->type != GgufType::array || (int) hk->integers.size() < block_count) {
      err = "glm5next: glm5-next.attention.head_count_kv missing or shorter than block_count";
      return false;
    }
    const int trunk = block_count - nextn;
    if (trunk <= 0) { err = "glm5next: nextn_predict_layers >= block_count"; return false; }
    g_.nextn = nextn;
    g_.n_layers = trunk;
    g_.n_head_kv.assign((std::size_t) trunk, 0);
    for (int il = 0; il < trunk; ++il) g_.n_head_kv[(std::size_t) il] = (int) hk->integers[(std::size_t) il];
    g_.indexer_full.assign((std::size_t) trunk, 1);  // released config: full indexer every layer
    std::fprintf(stderr, "glm5next: %d trunk layers (%d KDA / %d DSA), nextn=%d\n", trunk,
                 trunk - (int) std::count_if(g_.n_head_kv.begin(), g_.n_head_kv.end(), [](int x) { return x != 0; }),
                 (int) std::count_if(g_.n_head_kv.begin(), g_.n_head_kv.end(), [](int x) { return x != 0; }), nextn);
  }

  auto is_nextn = [&](const std::string& n) {
    return n.rfind("blk." + std::to_string(g_.n_layers) + ".", 0) == 0;
  };
  auto is_routed_expert = [](const std::string& n) {
    return n.find("ffn_gate_exps") != std::string::npos || n.find("ffn_up_exps") != std::string::npos ||
           n.find("ffn_down_exps") != std::string::npos;
  };

  // ---- upload the non-expert tensors to a device arena, map name -> WSlot. Quantized tensors are
  // kept raw (gemv_ dispatches them through u::k::mmvq_f32); F32/BF16 get their typed pointer. The
  // routed experts and the NextN block (blk.<n_layers>.*) are skipped here (the tier / the draft path).
  // TODO(gpu-box): the reference dequantizes the small tensors to F32 at load for the correctness
  // path; here they are served in their stored type through gemv_, which the parity gate validates.
  std::uint64_t arena = 0;
  for (const auto& t : gguf.tensors) {
    if (is_nextn(t.name) || is_routed_expert(t.name)) continue;
    arena += (t.bytes + 255u) & ~std::uint64_t{255u};
  }
  GLM_CU(cudaMalloc(&w_arena_, arena), err);
  w_arena_bytes_ = arena;
  std::uint64_t at = 0;
  for (const auto& t : gguf.tensors) {
    if (is_nextn(t.name) || is_routed_expert(t.name)) continue;
    // host_data is the loader-resolved absolute pointer (handles multi-part split models); fall back to
    // mapped + offset for a single-file caller that did not set it.
    const std::uint8_t* src = t.host_data ? t.host_data : mapped + t.offset;
    GLM_CU(cudaMemcpy((std::uint8_t*) w_arena_ + at, src, t.bytes, cudaMemcpyHostToDevice), err);
    WSlot slot;
    slot.n_out = (std::int64_t) (t.dims.empty() ? 0 : t.dims.back());
    slot.n_in = (std::int64_t) (t.dims.empty() ? 0 : t.dims.front());
    if (t.type == TensorType::f32) {
      slot.type = 0;
      slot.f32 = (const float*) ((std::uint8_t*) w_arena_ + at);
    } else {
      slot.type = (int) t.type;   // u::k::Quant ids match the GGML ids (common.cuh enum Quant)
      slot.q = (const void*) ((std::uint8_t*) w_arena_ + at);
      if (t.type == TensorType::bf16) slot.f32 = nullptr;
    }
    ws_[t.name] = slot;
    at += (t.bytes + 255u) & ~std::uint64_t{255u};
  }

  // ---- attn_k_b / attn_v_b are consumed as F32 by glm_q_absorb / glm_mla_attention (the absorbed-MLA
  // math), but this checkpoint ships them Q6_K. Dequantize each to a dedicated F32 arena at load and
  // repoint its WSlot (mirrors the reference pack, which stores k_b/v_b raw F32). They are the only
  // weights read as f32* rather than matvec'd, so only these two per DSA layer need it.
  auto is_kvb = [](const std::string& n) {
    return n.find(".attn_k_b.weight") != std::string::npos || n.find(".attn_v_b.weight") != std::string::npos;
  };
  std::uint64_t f32_bytes = 0;
  for (const auto& t : gguf.tensors) {
    if (is_nextn(t.name) || !is_kvb(t.name)) continue;
    if (ws_[t.name].type != (int) TensorType::q6_k) continue;
    f32_bytes += ((std::uint64_t) t.elements() * sizeof(float) + 255u) & ~std::uint64_t{255u};
  }
  if (f32_bytes > 0) {
    GLM_CU(cudaMalloc(&dsa_f32_, f32_bytes), err);
    std::uint64_t fat = 0;
    for (const auto& t : gguf.tensors) {
      if (is_nextn(t.name) || !is_kvb(t.name)) continue;
      WSlot& slot = ws_[t.name];
      if (slot.type != (int) TensorType::q6_k) continue;
      float* dst = (float*) ((std::uint8_t*) dsa_f32_ + fat);
      k::dequant_q6k(slot.q, (int) t.elements(), dst, nullptr);
      slot.type = 0;
      slot.f32 = dst;
      slot.q = nullptr;
      fat += ((std::uint64_t) t.elements() * sizeof(float) + 255u) & ~std::uint64_t{255u};
    }
  }

  // ---- hand each MoE layer's routed experts to the shared tier (it owns residency; it receives the
  // host-mapped source for its own VRAM/RAM/disk placement + the expert-hit-profile warm start, §3.5).
  experts_ = std::make_unique<ExpertTier>();
  for (int il = g_.dense_lead; il < g_.n_layers; ++il) {
    const std::string b = "blk." + std::to_string(il) + ".";
    auto slot = [&](const char* nm) -> WSlot {
      const GgufTensor* t = gguf.tensor(b + nm);
      WSlot s;
      if (t == nullptr) return s;
      s.type = t->type == TensorType::f32 ? 0 : (int) t->type;
      const std::uint8_t* src = t->host_data ? t->host_data : mapped + t->offset;  // split-model aware
      s.q = (const void*) src;
      s.f32 = t->type == TensorType::f32 ? (const float*) src : nullptr;
      s.n_in = (std::int64_t) g_.n_embd;
      s.n_out = (std::int64_t) g_.n_ff_exp;
      return s;
    };
    if (!experts_->register_layer(il, slot("ffn_gate_exps.weight"), slot("ffn_up_exps.weight"),
                                  slot("ffn_down_exps.weight"), g_.n_expert, g_.n_ff_exp, g_.swiglu_exp, err))
      return false;
  }

  // ---- the state arena (persists across tokens). Faithful port of glm_model.cu L747-776.
  const std::int64_t hc_dim = (std::int64_t) g_.hc * g_.n_embd;
  const std::int64_t max_pools = max_ctx_ / g_.idx_kpool;
  std::int64_t floats = 2 * hc_dim;   // R and its hc_post ping-pong
  kda_S_.assign(g_.n_layers, -1);
  kda_conv_.assign(g_.n_layers, -1);
  dsa_lat_.assign(g_.n_layers, -1);
  dsa_ik_.assign(g_.n_layers, -1);
  dsa_ig_.assign(g_.n_layers, -1);
  dsa_pool_.assign(g_.n_layers, -1);
  for (int il = 0; il < g_.n_layers; ++il) {
    if (g_.is_recr(il)) {
      kda_S_[il] = floats;
      floats += (std::int64_t) g_.d_inner() * g_.kda_head_dim;
      kda_conv_[il] = floats;
      floats += (std::int64_t) 3 * g_.d_inner() * (g_.d_conv - 1);
    } else {
      dsa_lat_[il] = floats;
      floats += (std::int64_t) g_.kv_lora * max_ctx_;
      dsa_ik_[il] = floats;
      floats += (std::int64_t) g_.idx_key * max_ctx_;
      dsa_ig_[il] = floats;
      floats += (std::int64_t) g_.idx_key * max_ctx_;
      dsa_pool_[il] = floats;
      floats += (std::int64_t) g_.idx_key * max_pools;
    }
  }
  state_bytes_ = (std::uint64_t) floats * sizeof(float);
  GLM_CU(cudaMalloc(&state_, state_bytes_), err);

  // ---- the scratch arena (one token's activations; reused each token). Port of glm_model.cu L779-826.
  const std::int64_t ff_max = std::max<std::int64_t>(g_.n_ff_dense, (std::int64_t) g_.n_ff_exp * g_.n_shared);
  std::int64_t s = 0;
  auto take = [&](std::int64_t n) { const std::int64_t at2 = s; s += n; return at2; };
  sc_mixed = take(g_.n_embd);
  sc_emb = take(g_.n_embd);
  sc_x = take(g_.n_embd);
  sc_pre = take(g_.hc);
  sc_post = take(g_.hc);
  sc_comb = take((std::int64_t) g_.hc * g_.hc);
  sc_inv = take(1);
  sc_proj = take(g_.d_inner());
  sc_conv[0] = take(g_.d_inner());
  sc_conv[1] = take(g_.d_inner());
  sc_conv[2] = take(g_.d_inner());
  sc_g = take(g_.d_inner());
  sc_g1 = take(g_.d_inner());
  sc_beta = take(g_.n_head);
  sc_tmp = take(std::max<std::int64_t>({(std::int64_t) g_.kda_head_dim, (std::int64_t) g_.q_lora,
                                        (std::int64_t) g_.idx_key, (std::int64_t) g_.n_expert}));
  sc_scan = take(g_.d_inner());
  sc_g2 = take(g_.d_inner());
  sc_gated = take(g_.d_inner());
  sc_qr = take(g_.q_lora);
  sc_kv = take(g_.kv_lora);
  sc_q = take((std::int64_t) g_.n_head * g_.qk_nope);
  sc_qabs = take((std::int64_t) g_.kv_lora * g_.n_head);
  sc_iq = take((std::int64_t) g_.idx_heads * g_.idx_key);
  sc_iw = take(g_.idx_heads);
  sc_score = take(max_pools);
  sc_cells = take(g_.n_sel_max());
  sc_attn = take((std::int64_t) g_.v_head * g_.n_head);
  sc_gate = take(ff_max);
  sc_up = take(ff_max);
  sc_h = take(ff_max);
  sc_dn = take(g_.n_embd);
  sc_moe = take(g_.n_embd);
  sc_sh = take(g_.n_embd);
  sc_ffn = take(g_.n_embd);
  sc_mixer = take(g_.n_embd);
  sc_head = take(g_.n_embd);
  sc_logits = take(g_.n_vocab);
  sc_ids = take(g_.n_exp_used);   // int32 (same 4 bytes)
  sc_rw = take(g_.n_exp_used);
  sc_bytes_ = (std::uint64_t) s * sizeof(float);
  GLM_CU(cudaMalloc(&sc_, sc_bytes_), err);
  GLM_CU(cudaMalloc(&d_pos_, sizeof(int)), err);
  GLM_CU(cudaMalloc(&d_tok_, sizeof(int)), err);

  // ---- matmul-dispatch scratch (gemv_): widest reduction dim over every GEMV this graph issues.
  max_n_in_ = std::max<std::int64_t>({(std::int64_t) g_.n_embd, (std::int64_t) g_.d_inner(),
                                      (std::int64_t) g_.n_head * g_.v_head, (std::int64_t) g_.n_ff_dense,
                                      (std::int64_t) g_.n_ff_exp * g_.n_shared, (std::int64_t) g_.q_lora,
                                      (std::int64_t) g_.kda_head_dim});
  GLM_CU(cudaMalloc(&q8_scratch_, k::q8_1_scratch_bytes((int) max_n_in_, 1)), err);
  GLM_CU(cudaMalloc(&bf16_x_, (std::size_t) max_n_in_ * sizeof(std::uint16_t)), err);

  // ---- hot-expert VRAM residency. With everything else allocated, spend the free VRAM (less a headroom)
  // on resident expert slots: run_moe reads a resident expert straight from its slot and streams only the
  // rest from the mapped host over PCIe. Cold-start here; the gated reg_rebalance in generate() converges
  // the resident set onto the hot experts. A budget of 0 (no headroom) leaves it fully streaming.
  {
    std::size_t vfree = 0, vtotal = 0;
    cudaMemGetInfo(&vfree, &vtotal);
    const std::size_t headroom = (std::size_t) Calibration::profile().residency_headroom_mib << 20;  // calibratable
                                                             // (the pool is one contiguous cudaMalloc)
    const std::int64_t budget = vfree > headroom ? (std::int64_t) (vfree - headroom) : 0;

    // Optional routing-profile warm start: an STRP profile from a prior run (STRATA_GLM_EXPERT_PROFILE)
    // ranks the experts hottest-first so the resident set starts hot instead of converging from cold.
    std::vector<std::pair<std::int32_t, std::int32_t>> ranked;
    if (const char* prof = std::getenv("STRATA_GLM_EXPERT_PROFILE")) {
      std::int64_t pslots = 0;
      std::string perr;
      if (read_expert_profile(prof, g_.n_layers, g_.n_expert, ranked, pslots, perr)) {
        std::fprintf(stderr, "glm5next: warm-start profile '%s' (%zu ranked experts)\n", prof, ranked.size());
      } else {
        std::fprintf(stderr, "glm5next: WARN profile '%s' ignored: %s (cold start)\n", prof, perr.c_str());
        ranked.clear();
      }
    }
    if (!experts_->reg_finalize(budget, ranked, Calibration::profile().warm_seed,
                                Calibration::profile().stream_prefetch, err))
      return false;
    experts_->reg_set_cpu_lane(Calibration::profile().cpu_expert_frac, Calibration::profile().cpu_workers);
    std::fprintf(stderr,
                 "glm5next: expert residency %lld VRAM slots (%lld/layer of %d), %.2f GiB, %s start; the rest "
                 "stream from host\n",
                 (long long) experts_->reg_slots(), (long long) (experts_->reg_slots() / std::max(1, g_.n_layers)),
                 g_.n_expert, (double) ((std::int64_t) vfree - (std::int64_t) headroom) / (1024.0 * 1024.0 * 1024.0),
                 ranked.empty() ? "cold" : "warm");
    std::fflush(stderr);
  }

  loaded_ = true;
  reset();
  return true;
}

void GlmGraph::reset() {
  pos_ = 0;
  if (state_) cudaMemset(state_, 0, state_bytes_);   // zero R, every KDA state, every DSA cache
}

// ================================ step: one token through all layers ================================
//
// Faithful port of Glm5Model::step_layers (glm_model.cu L2198), single device, single stream, T=1.
// Each token: embed -> mHC init of the 4-stream residual R -> 45 layers (mHC-read + attn-norm +
// KDA|DSA mixer + mHC-write, then mHC-read + ffn-norm + dense|MoE FFN + mHC-write) -> head (mean over
// streams, output-norm, head GEMV into the logits). Cross-token state lives in the KDA recurrent
// states and the DSA latent/indexer caches; R is rebuilt from the embedding each token.
bool GlmGraph::step(std::int32_t token, std::string& err) {
  const GlmShape& g = g_;
  const std::int64_t p = pos_;
  float* const S = state_;
  float* const A = sc_;
  float* R = S;   // the residual stack lives at the state arena's head

  glm::GlmHcShapes hcs;
  hcs.n_embd = g.n_embd;
  hcs.hc = g.hc;
  hcs.sinkhorn_iters = g.sinkhorn_iters;
  glm::GlmHcWorkspace hcws;
  glm::glm_hc_workspace_init(hcs, A + sc_inv, hcws);
  const float prescale = 1.0f / std::sqrt((float) (g.idx_key * g.idx_heads));

  // ---- embed + hc_init. token_embd is (n_vocab, n_embd); row `token` at token*n_embd. A plain F32
  // copy when the embedding is F32; a per-row dequant when it is quantized (GLM-5.3 ships Q6_K here),
  // mirroring glm_model.cu's pack_emb (which dequantizes one row/token).
  {
    const WSlot& emb = W("token_embd.weight");
    if (emb.type == 0 && emb.f32 != nullptr) {
      GLM_CU(cudaMemcpy(A + sc_emb, emb.f32 + (std::int64_t) token * g.n_embd,
                        (std::size_t) g.n_embd * sizeof(float), cudaMemcpyDeviceToDevice), err);
    } else if (emb.type == (int) k::Quant::q6_k && emb.q != nullptr) {
      const std::size_t row_bytes = k::weight_bytes(k::Quant::q6_k, g.n_embd, 1);
      k::dequant_q6k((const std::uint8_t*) emb.q + (std::size_t) token * row_bytes, g.n_embd, A + sc_emb, nullptr);
    } else {
      err = "glm5next: token_embd.weight is neither F32 nor Q6_K (embedding dequant unsupported)";
      return false;
    }
  }
  glm::glm_hc_init(A + sc_emb, R, g.n_embd, g.hc, nullptr);

  const bool trace = std::getenv("STRATA_GLM_TRACE") != nullptr;
  for (int il = 0; il < g.n_layers; ++il) {
    if (trace) {
      float r3[3] = {0, 0, 0};
      cudaMemcpy(r3, R, sizeof(r3), cudaMemcpyDeviceToHost);
      std::fprintf(stderr, "[glm] L%d attn %s R=[%.4f %.4f %.4f]\n", il, g.is_recr(il) ? "KDA" : "DSA", r3[0], r3[1],
                   r3[2]);
      std::fflush(stderr);
      if (cudaDeviceSynchronize() != cudaSuccess) {
        err = std::string("glm5next: pre-L") + std::to_string(il) + " attn: " + cudaGetErrorString(cudaGetLastError());
        return false;
      }
    }
    // ---- mHC read + attn norm (rms_norm is in place in u::k; rmsnorm_to keeps the two-buffer shape)
    glm::glm_hc_pre(R, W(il, "hc_attn_fn.weight").f32, W(il, "hc_attn_scale.weight").f32,
                    W(il, "hc_attn_base.weight").f32, g.norm_eps, g.hc_eps, hcs, hcws, A + sc_mixed,
                    A + sc_pre, A + sc_post, A + sc_comb, nullptr);
    rmsnorm_to(A + sc_x, A + sc_mixed, W(il, "attn_norm.weight").f32, g.n_embd, g.norm_eps, nullptr);

    if (g.is_recr(il)) {
      // ---- the KDA mixer (glm_model.cu L2229)
      const char* streams[] = {"attn_q.weight", "attn_k.weight", "attn_v.weight"};
      const char* convs[] = {"ssm_conv1d_q.weight", "ssm_conv1d_k.weight", "ssm_conv1d_v.weight"};
      for (int si = 0; si < 3; ++si) {
        gemv_(W(il, streams[si]), A + sc_x, A + sc_proj, g.d_inner(), g.n_embd, nullptr);
        glm::glm_kda_conv(A + sc_proj, W(il, convs[si]).f32,
                          S + kda_conv_[il] + (std::int64_t) si * g.d_inner() * (g.d_conv - 1),
                          g.d_inner(), g.d_conv, 1, A + sc_conv[si], nullptr);
      }
      // q, k are L2-normalized per head; v is NOT
      glm::glm_l2_norm_heads(A + sc_conv[0], A + sc_conv[0], g.kda_head_dim, g.n_head, 1, 1e-6f, nullptr);
      glm::glm_l2_norm_heads(A + sc_conv[1], A + sc_conv[1], g.kda_head_dim, g.n_head, 1, 1e-6f, nullptr);
      // the decay gate: g1 = lb * sigmoid(-(f_b (f_a x) + dt_b) * A)  (A = ssm_a holds -exp(A_log))
      gemv_(W(il, "ssm_f_a.weight"), A + sc_x, A + sc_tmp, g.kda_head_dim, g.n_embd, nullptr);
      gemv_(W(il, "ssm_f_b.weight"), A + sc_tmp, A + sc_g, g.d_inner(), g.kda_head_dim, nullptr);
      k::add_inplace(A + sc_g, W(il, "ssm_dt.bias").f32, g.d_inner(), nullptr);
      glm::glm_kda_gate(A + sc_g, W(il, "ssm_a").f32, g.kda_lb, g.kda_head_dim, g.n_head, 1, A + sc_g1, nullptr);
      // beta = sigmoid(ssm_beta x)  per head
      gemv_(W(il, "ssm_beta.weight"), A + sc_x, A + sc_beta, g.n_head, g.n_embd, nullptr);
      glm::glm_sigmoid_inplace(A + sc_beta, g.n_head, nullptr);
      // the recurrent delta rule, state carried in place
      glm::glm_kda_recurrence(A + sc_conv[0], A + sc_conv[1], A + sc_conv[2], A + sc_g1, A + sc_beta,
                              S + kda_S_[il], g.n_head, g.kda_head_dim, 1, A + sc_scan, nullptr);
      // the low-rank output gate g2, then rms_norm(o, ssm_norm) * sigmoid(g2)
      gemv_(W(il, "ssm_g_a.weight"), A + sc_x, A + sc_tmp, g.kda_head_dim, g.n_embd, nullptr);
      gemv_(W(il, "ssm_g_b.weight"), A + sc_tmp, A + sc_g2, g.d_inner(), g.kda_head_dim, nullptr);
      glm::glm_kda_out_gate(A + sc_scan, W(il, "ssm_norm.weight").f32, A + sc_g2, g.kda_head_dim, g.n_head, 1,
                            g.norm_eps, A + sc_gated, nullptr);
      gemv_(W(il, "attn_output.weight"), A + sc_gated, A + sc_mixer, g.n_embd, g.d_inner(), nullptr);
    } else {
      // ---- the DSA mixer (NoPE-MLA + the learned k-pool indexer) (glm_model.cu L2266)
      gemv_(W(il, "attn_q_a.weight"), A + sc_x, A + sc_qr, g.q_lora, g.n_embd, nullptr);
      k::rms_norm(A + sc_qr, W(il, "attn_q_a_norm.weight").f32, 1, g.q_lora, g.norm_eps, nullptr);
      gemv_(W(il, "attn_kv_a_mqa.weight"), A + sc_x, A + sc_kv, g.kv_lora, g.n_embd, nullptr);
      k::rms_norm(A + sc_kv, W(il, "attn_kv_a_norm.weight").f32, 1, g.kv_lora, g.norm_eps, nullptr);
      // append the compressed latent for position p (this is the entire KV cache: no per-head K/V)
      float* lat = S + dsa_lat_[il];
      GLM_CU(cudaMemcpy(lat + (std::int64_t) g.kv_lora * p, A + sc_kv, (std::size_t) g.kv_lora * sizeof(float),
                        cudaMemcpyDeviceToDevice), err);
      // q absorbed through wk_b
      gemv_(W(il, "attn_q_b.weight"), A + sc_qr, A + sc_q, (std::int64_t) g.n_head * g.qk_nope, g.q_lora, nullptr);
      glm::glm_q_absorb(W(il, "attn_k_b.weight").f32, A + sc_q, A + sc_qabs, g.qk_nope, g.kv_lora, g.n_head, 1,
                        nullptr);

      // the indexer: per-token key/gate into their caches; a pool's key when it completes
      gemv_(W(il, "indexer.attn_k.weight"), A + sc_x, A + sc_tmp, g.idx_key, g.n_embd, nullptr);
      glm::glm_layer_norm(A + sc_tmp, W(il, "indexer.k_norm.weight").f32, W(il, "indexer.k_norm.bias").f32,
                          A + sc_tmp, g.idx_key, 1, g.norm_eps, nullptr);
      GLM_CU(cudaMemcpy(S + dsa_ik_[il] + (std::int64_t) g.idx_key * p, A + sc_tmp,
                        (std::size_t) g.idx_key * sizeof(float), cudaMemcpyDeviceToDevice), err);
      gemv_(W(il, "indexer_compressor_gate.weight"), A + sc_x, A + sc_tmp, g.idx_key, g.n_embd, nullptr);
      GLM_CU(cudaMemcpy(S + dsa_ig_[il] + (std::int64_t) g.idx_key * p, A + sc_tmp,
                        (std::size_t) g.idx_key * sizeof(float), cudaMemcpyDeviceToDevice), err);
      const std::int64_t pool_done = (p + 1) / g.idx_kpool;   // pools whose last member is <= p
      if ((p + 1) % g.idx_kpool == 0) {
        const std::int64_t pi = pool_done - 1;
        glm::glm_dsa_pool(S + dsa_ik_[il] + (std::int64_t) g.idx_key * pi * g.idx_kpool,
                          S + dsa_ig_[il] + (std::int64_t) g.idx_key * pi * g.idx_kpool,
                          W(il, "indexer_compressor_ape.weight").f32, g.idx_key, g.idx_kpool, 1,
                          S + dsa_pool_[il] + (std::int64_t) g.idx_key * pi, nullptr);
      }
      gemv_(W(il, "indexer.attn_q_b.weight"), A + sc_qr, A + sc_iq, (std::int64_t) g.idx_heads * g.idx_key,
            g.q_lora, nullptr);
      // weights = (indexer.proj x) * prescale  (the shared gemv has no scale arg -> scale_inplace)
      gemv_(W(il, "indexer.proj.weight"), A + sc_x, A + sc_iw, g.idx_heads, g.n_embd, nullptr);
      k::scale_inplace(A + sc_iw, g.idx_heads, prescale, nullptr);
      // the incomplete tail (select_tail) makes the CURRENT token visible before the first pool
      // completes; the selection runs whenever pools exist OR the tail is on (glm_model.cu L2311).
      if (pool_done > 0 || g.idx_select_tail) {
        if (pool_done > 0)
          glm::glm_dsa_score(A + sc_iq, S + dsa_pool_[il], A + sc_iw, g.idx_key, g.idx_heads, 1,
                             (int) pool_done, A + sc_score, nullptr);
        const int top_pools = (int) std::min<std::int64_t>(g.top_pools_max(), pool_done);
        const int n_sel = g.idx_kpool * top_pools + (g.idx_select_tail ? g.idx_kpool - 1 : 0);
        const int p32 = (int) p;
        GLM_CU(cudaMemcpy(d_pos_, &p32, sizeof(int), cudaMemcpyHostToDevice), err);
        glm::glm_dsa_select(A + sc_score, (int) pool_done, g.idx_kpool, top_pools, g.idx_select_tail, 1, n_sel,
                            d_pos_, (int*) (A + sc_cells), nullptr);
        glm::glm_mla_attention(A + sc_qabs, lat, (const int*) (A + sc_cells), W(il, "attn_v_b.weight").f32,
                               g.kv_lora, g.n_head, g.v_head, g.qk_nope, 1, n_sel, A + sc_attn, nullptr);
      } else {
        GLM_CU(cudaMemset(A + sc_attn, 0, (std::size_t) g.v_head * g.n_head * sizeof(float)), err);
      }
      gemv_(W(il, "attn_output.weight"), A + sc_attn, A + sc_mixer, g.n_embd,
            (std::int64_t) g.n_head * g.v_head, nullptr);
    }

    // ---- mHC write (ping-pong to the second stack), then the FFN half's mHC read + norm
    glm::glm_hc_post(A + sc_mixer, R, A + sc_post, A + sc_comb, hcs, S + (std::int64_t) g.hc * g.n_embd, nullptr);
    R = S + (std::int64_t) g.hc * g.n_embd;
    glm::glm_hc_pre(R, W(il, "hc_ffn_fn.weight").f32, W(il, "hc_ffn_scale.weight").f32,
                    W(il, "hc_ffn_base.weight").f32, g.norm_eps, g.hc_eps, hcs, hcws, A + sc_mixed, A + sc_pre,
                    A + sc_post, A + sc_comb, nullptr);
    rmsnorm_to(A + sc_x, A + sc_mixed, W(il, "ffn_norm.weight").f32, g.n_embd, g.norm_eps, nullptr);

    if (trace) {
      std::fprintf(stderr, "[glm] L%d ffn %s\n", il, il < g.dense_lead ? "dense" : "moe");
      std::fflush(stderr);
      if (cudaDeviceSynchronize() != cudaSuccess) {
        err = std::string("glm5next: L") + std::to_string(il) + " attn-half: " + cudaGetErrorString(cudaGetLastError());
        return false;
      }
    }
    if (il < g.dense_lead) {
      // ---- the dense leading FFN (clamped SwiGLU, shexp limit)
      gemv_(W(il, "ffn_gate.weight"), A + sc_x, A + sc_gate, g.n_ff_dense, g.n_embd, nullptr);
      gemv_(W(il, "ffn_up.weight"), A + sc_x, A + sc_up, g.n_ff_dense, g.n_embd, nullptr);
      glm::glm_swiglu_clamp(A + sc_gate, A + sc_up, g.swiglu_shexp, g.n_ff_dense, A + sc_h, nullptr);
      gemv_(W(il, "ffn_down.weight"), A + sc_h, A + sc_ffn, g.n_embd, g.n_ff_dense, nullptr);
    } else {
      // ---- the sigmoid-routed MoE: router -> ids/weights (device) -> the shared tier; then the
      // shared expert inline; sum into sc_ffn. (glm_model.cu L2352)
      gemv_(W(il, "ffn_gate_inp.weight"), A + sc_x, A + sc_tmp, g.n_expert, g.n_embd, nullptr);
      k::router_sigmoid(A + sc_tmp, W(il, "exp_probs_b.bias").f32, 1, g.n_expert, g.n_exp_used, g.w_scale,
                        g.norm_w != 0, (int*) (A + sc_ids), A + sc_rw, nullptr);
      if (!experts_->run_moe(il, (const int*) (A + sc_ids), A + sc_rw, g.n_exp_used, A + sc_x, nullptr,
                             A + sc_moe, nullptr, err))
        return false;
      // the shared expert (dense, one per layer): clamped SwiGLU, shexp limit
      gemv_(W(il, "ffn_gate_shexp.weight"), A + sc_x, A + sc_gate, (std::int64_t) g.n_ff_exp * g.n_shared,
            g.n_embd, nullptr);
      gemv_(W(il, "ffn_up_shexp.weight"), A + sc_x, A + sc_up, (std::int64_t) g.n_ff_exp * g.n_shared,
            g.n_embd, nullptr);
      glm::glm_swiglu_clamp(A + sc_gate, A + sc_up, g.swiglu_shexp, (std::int64_t) g.n_ff_exp * g.n_shared,
                            A + sc_h, nullptr);
      gemv_(W(il, "ffn_down_shexp.weight"), A + sc_h, A + sc_sh, g.n_embd,
            (std::int64_t) g.n_ff_exp * g.n_shared, nullptr);
      // sc_ffn = moe + shexp
      GLM_CU(cudaMemcpy(A + sc_ffn, A + sc_moe, (std::size_t) g.n_embd * sizeof(float),
                        cudaMemcpyDeviceToDevice), err);
      k::add_inplace(A + sc_ffn, A + sc_sh, g.n_embd, nullptr);
    }

    glm::glm_hc_post(A + sc_ffn, R, A + sc_post, A + sc_comb, hcs, S, nullptr);
    R = S;
  }

  // ---- the head: mean over the hc streams, output_norm (in place), logits
  glm::glm_hc_mean(R, A + sc_head, g.n_embd, g.hc, 1, nullptr);
  k::rms_norm(A + sc_head, W("output_norm.weight").f32, 1, g.n_embd, g.norm_eps, nullptr);
  gemv_(W("output.weight"), A + sc_head, A + sc_logits, g.n_vocab, g.n_embd, nullptr);

  ++pos_;
  return true;
}

// ================================ sampling + generate ================================

bool GlmGraph::sample_next(const Gen& request, std::int32_t& out_id, std::string& err) {
  // Parse the request's sampler params (the converged protocol's GEN key=values) into u::k::SamplerParams.
  k::SamplerParams sp;   // the struct's own defaults (top_k 20, top_p 0.95, temp 1.0) stand unless set
  sp.counter = (std::uint64_t) pos_;
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
      err = "glm5next: malformed sampler param '" + kv.key + "'";
      return false;
    }
  }
  if (sp.temperature <= 0.0f) sp.greedy = true;
  // sample_tokens reads/writes DEVICE memory (common.cuh contract): sc_logits and d_tok_ are device.
  // TODO(gpu-box): thread the penalty history through SamplerParams::history for repeat penalties
  // (glm_model.cu pick_tokens builds per-row histories); the battery exercises greedy + top-p first.
  k::sample_tokens(sc_ + sc_logits, 1, g_.n_vocab, nullptr, 0, sp, d_tok_, nullptr);
  int id = 0;
  if (cudaMemcpy(&id, d_tok_, sizeof(int), cudaMemcpyDeviceToHost) != cudaSuccess) {
    err = std::string("glm5next: sampler copy: ") + cudaGetErrorString(cudaGetLastError());
    return false;
  }
  out_id = (std::int32_t) id;
  return true;
}

// Runs one request: prefill the prompt ids, then sample + emit `T <id>` per token up to max_new. The
// serve loop appends DONE; a failed step/sample sets `error` and returns false. The NextN/DFlash
// speculative path is a box TODO (kernels/glm_dflash.cuh); this is the correctness-first token path
// the parity gate and the generation battery check first.
bool GlmGraph::generate(const Gen& request, const LineSink& sink, std::string& error) {
  if (!loaded_) {
    error = "glm5next: not loaded (GlmGraph::load must run on the GPU box with the mapped GGUF)";
    return false;
  }
  reset();
  if (request.ids.empty()) { error = "glm5next: empty prompt"; return false; }
  if ((std::int64_t) request.ids.size() + (std::int64_t) request.max_new > max_ctx_) {
    error = "glm5next: the sequence exceeds max_ctx"; return false;
  }

  // ---- prefill: feed every prompt token; the LAST token's logits predict the first generated token.
  using Clk = std::chrono::steady_clock;
  const auto t_pf = Clk::now();
  for (std::size_t i = 0; i < request.ids.size(); ++i)
    if (!step((std::int32_t) request.ids[i], error)) return false;
  if (cudaDeviceSynchronize() != cudaSuccess) { error = "glm5next: prefill sync failed"; return false; }
  const auto t_dec0 = Clk::now();
  sink("PP " + std::to_string(request.ids.size()) + " " + std::to_string(request.ids.size()) + " 0 0");
  experts_->reset_reg_stats();  // measure the expert cache over the decode phase only

  // ---- decode: sample, emit, feed back, up to max_new. run_moe notes routing heat each token; a gated
  // reg_rebalance (every 8 tokens, like gemma4) swaps the hottest non-resident experts into the coldest
  // resident slots so the resident set converges onto what this prompt actually routes to.
  std::uint32_t produced = 0;
  int since_reb = 0;
  for (std::uint32_t n = 0; n < request.max_new; ++n) {
    std::int32_t id = 0;
    if (!sample_next(request, id, error)) return false;
    sink("T " + std::to_string((std::uint32_t) id));
    ++produced;
    if (n + 1 < request.max_new) {
      if (!step(id, error)) return false;   // the last emit needs no further forward
      if (++since_reb >= Calibration::profile().rebalance_every) {
        since_reb = 0;
        if (experts_->reg_rebalance(Calibration::profile().rebalance_margin, nullptr, error) < 0) return false;
      }
    }
  }
  if (cudaDeviceSynchronize() != cudaSuccess) { error = "glm5next: decode sync failed"; return false; }
  const auto t_end = Clk::now();
  const double pf_ms = std::chrono::duration<double, std::milli>(t_dec0 - t_pf).count();
  const double dec_ms = std::chrono::duration<double, std::milli>(t_end - t_dec0).count();
  // The unified serve loop does not append DONE for this family; emit it here (gemma4 does the same), so
  // a driver sees the terminal record and the timing.
  sink("DONE " + std::to_string(produced) + " " + std::to_string(request.ids.size()) + " " +
       std::to_string(pf_ms) + " " + std::to_string(dec_ms) + " length 0 0 0");
  const std::int64_t rh = experts_->reg_resident_hits(), rm = experts_->reg_stream_misses();
  const double hitpct = (rh + rm) > 0 ? 100.0 * (double) rh / (double) (rh + rm) : 0.0;
  std::fprintf(stderr,
               "glm5next: prefill %zu tok / %.0f ms, decode %u tok / %.0f ms = %.2f tok/s | experts "
               "resident_hits=%lld stream_misses=%lld (%.1f%% resident)\n",
               request.ids.size(), pf_ms, produced, dec_ms, dec_ms > 0 ? produced / (dec_ms / 1000.0) : 0.0,
               (long long) rh, (long long) rm, hitpct);
  std::fflush(stderr);

  // Optional: persist the learned routing profile (STRP) so a later run can warm-start the resident set
  // hot. reg_ranked() ranks every (layer,expert) resident-first then by decayed routing heat.
  if (const char* save = std::getenv("STRATA_GLM_EXPERT_PROFILE_SAVE")) {
    std::string serr;
    if (write_expert_profile(save, g_.n_layers, g_.n_expert, experts_->reg_ranked(), serr))
      std::fprintf(stderr, "glm5next: saved expert profile -> '%s'\n", save);
    else
      std::fprintf(stderr, "glm5next: WARN could not save profile '%s': %s\n", save, serr.c_str());
    std::fflush(stderr);
  }
  return true;
}

// ================================ make_glm_graph ================================
//
// Builds the GLM graph from the guarded common geometry. The GLM-specific dims u::Geometry does not
// carry are taken as the released GLM-5.3-Flash defaults; the PER-LAYER SCHEDULE must not be guessed
// (it decides KDA vs DSA per layer and is checkpoint-specific - see the SEAM note).
std::unique_ptr<FamilyGraph> make_glm_graph(const Geometry& geom, std::uint32_t context, std::string& error) {
  error.clear();
  if (geom.family != FamilyId::glm5next) {
    error = "make_glm_graph: geometry is not glm5next";
    return nullptr;
  }
  GlmShape sh;
  // ---- the common dims from the guarded geometry (these u::Geometry does carry).
  sh.n_embd = (int) geom.embedding_length;
  sh.n_layers = (int) geom.block_count;
  sh.n_head = (int) geom.head_count;
  sh.n_vocab = (int) geom.vocab_size;
  sh.n_expert = (int) geom.expert_count;
  sh.n_exp_used = (int) geom.expert_used_count;
  sh.n_shared = (int) geom.expert_shared_count;
  sh.n_ff_exp = (int) geom.expert_feed_forward_length;
  sh.n_ff_dense = (int) geom.feed_forward_length;
  // ---- the GLM-specific dims u::Geometry does NOT carry stay at the released GLM-5.3-Flash defaults
  // (the GlmShape member initializers, mirroring glm.py::Glm5Config): q_lora/kv_lora/qk_nope/v_head,
  // kda_head_dim/d_conv/kda_lb, the indexer (idx_*), hc/sinkhorn/hc_eps, dense_lead, swiglu limits,
  // w_scale/norm_w.
  //
  // SEAM / TODO(substrate, u_geometry): the per-layer schedule MUST come from the GGUF - it is not
  // safely defaultable. For GLM the guard must surface:
  //   * glm5next.attention.head_count_kv  (per-layer array; 0 => KDA) -> GlmShape::n_head_kv
  //   * glm5next.attention.indexer.types  (per-layer; absent => all full) -> GlmShape::indexer_full
  //   * glm5next.nextn_predict_layers -> nextn (and n_layers = block_count - nextn)
  //   * any indexer / LoRA / kda scalar where a checkpoint differs from the released defaults
  // (glm_model.cu::Glm5Geometry::from_gguf L534-599 reads exactly these). Until u::Geometry carries
  // them, the schedule below is a DOCUMENTED PLACEHOLDER; the box MUST overwrite n_head_kv from the
  // real GGUF before trusting any parity result. The released model is 34 KDA / 11 DSA over 45 layers,
  // but the exact positions come from the file - we do NOT fabricate a 34/11 pattern (a silent guess
  // at which layers are DSA), so this leaves every layer KDA as a well-formed placeholder.
  if (sh.n_layers <= 0) { error = "make_glm_graph: block_count must be positive"; return nullptr; }
  sh.n_head_kv.assign((std::size_t) sh.n_layers, 0);     // PLACEHOLDER (all-KDA); box overrides from GGUF
  sh.indexer_full.assign((std::size_t) sh.n_layers, 1);  // released config: full indexer every layer

  auto graph = std::make_unique<GlmGraph>(std::move(sh), context);
  // NOTE: weights are not uploaded here (make_glm_graph has no GGUF). The substrate's dispatch calls
  // GlmGraph::load(gguf, mapped, err) before the first generate(); until then available() is false.
  return graph;
}

}  // namespace u
