// strata-unified-engine: the Raon-1-mini (arch "raon1") model graph — implementation.
//
// Modeled on engines/maya-ref/src/core/glm_model.cu (Raon is a near-twin of GLM-5.3-Flash). The
// per-layer schedule, the RDA/Gated-MLA mixers and the sigmoid MoE follow GLM's forward; the
// family-unique kernels are in src/kernels/raon1_rda.* and src/kernels/raon1_mla.* (ported from
// glm_kda / glm_dsa). Everything generic rides the shared substrate: the generic kernels in
// kernels/common.cuh (u::k::) and the expert-tiering facility in u_experts.hpp (u::ExpertTier).
//
// NOT COMPILED ON CI/dev-host. The unified engine builds the family .cu only behind STRATA_U_CUDA
// on the owner's GPU box (design §1.3 / plan "What is verifiable where"); a host/CI build links the
// stub and u::make_family_graph returns it. This file is written for the box, which compiles and
// parities it against the real Raon-1-mini weights.
//
// ============================================================================================
// SHARED API (landed; this lane calls it directly — the old "assumed" block predating T1.1/T1.2
// is gone, the signatures below are the real ones from kernels/common.cuh + u_experts.hpp):
//
//   namespace u::k {   // kernels/common.cuh
//     f32_gemv(x, W_f32, y, n_in, n_out, stream)                      // F32 row-GEMV
//     bf16_gemv(x16, W_bf16, y, n_in, n_out, stream)                  // BF16 row-GEMV
//     mmvq_f32(Quant, Wq, x, scratch_q8_1, y, n_in, n_out, ncols, stream)  // quantized GEMV
//     q8_1_scratch_bytes(n_in, ncols)                                 // scratch sizing
//     rms_norm(x_inout, gamma, rows, cols, eps, stream)               // in-place row norms
//     swiglu / swiglu_clamped(gate, up, out, n, [limit,] stream)
//     add_inplace(dst, src, n, stream)
//     router_sigmoid(logits, bias, T, n_expert, k, scale, norm_w, ids, weights, stream)
//     sample_tokens(logits, rows, n_vocab, history, hist_len, params, out_ids, stream)
//   }
//
//   u::ExpertTier (u_experts.hpp) — registered path, exactly as glm_graph.cu / ds4_graph.cu use:
//     register_layer(il, gate, up, down, n_expert, n_ff_exp, swiglu_limit, err)  (host WSlots)
//     run_moe(il, ids, weights, k, x, xq, out, stream, err)                     (T=1 per call)
//     reg_finalize(budget, ranked, warm_seed, stream_prefetch, err) + reg_set_cpu_lane + reg_rebalance
// ============================================================================================
#include "raon1_graph.hpp"

#include "kernels/common.cuh"
#include "kernels/raon1_mla.cuh"
#include "kernels/raon1_rda.cuh"
#include "u_calibration.hpp"   // u::Calibration (the per-box adaptive-tier profile, spec C1)
#include "u_experts.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace u {
namespace {

// Per-layer weight-name prefix, llama.cpp/GLM convention ("blk.<il>."). The exact Raon tensor
// base names (q/k/v projections, conv, decay, LoRA A/B, router, experts) are confirmed on the box
// against the real GGUF — they follow GLM's where the pieces match (see the per-call TODOs).
// [[maybe_unused]]: the call sites live in the layer_*() bodies the box wires onto the shared
// scratch/weight layout (the mixers below are structural until then); -Werror would flag it.
[[maybe_unused]] std::string blk(int il, const char* suffix) {
  return "blk." + std::to_string(il) + "." + suffix;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// Geometry: the raon1.* metadata beyond the common (pinned) u::Geometry.
// ------------------------------------------------------------------------------------------------
bool Raon1Geometry::from_gguf(const GgufFile& file, const Geometry& common, Raon1Geometry& out,
                              std::string& err) {
  // The common, pinned fields come straight from the guarded geometry (u_geometry.cpp exact()s
  // them, so these are already validated against Raon's published shape).
  out.n_embd = (int)common.embedding_length;
  out.n_layers = (int)common.block_count;
  out.n_head = (int)common.head_count;
  out.head_dim = (int)common.head_dim;  // 128: RDA head dim and MLA value head dim
  out.n_vocab = (int)common.vocab_size;
  out.n_ff_dense = (int)common.feed_forward_length;
  out.n_ff_exp = (int)common.expert_feed_forward_length;
  out.n_expert = (int)common.expert_count;
  out.n_exp_used = (int)common.expert_used_count;
  out.n_shared = (int)common.expert_shared_count;
  out.router = common.router;  // sigmoid (pinned)
  out.v_head = out.head_dim;

  const std::string arch = common.arch;  // "raon1"
  auto geti = [&](const char* suffix, int def) -> int {
    const GgufValue* v = file.find(arch + suffix);
    return (v != nullptr) ? (int)v->integer : def;
  };
  auto getf = [&](const char* suffix, float def) -> float {
    const GgufValue* v = file.find(arch + suffix);
    return (v != nullptr) ? (float)v->real : def;
  };

  // first_k_dense_replace == leading_dense_block_count (GLM's dense_lead). Published = 1.
  out.dense_lead = geti(".leading_dense_block_count", 1);

  // MLA LoRA ranks and head dims. TODO(box): confirm the exact raon1.* key names — these follow
  // the deepseek2/GLM convention (q_lora_rank / kv_lora_rank / key_length / value_length). The
  // published values are the defaults so a GGUF that omits them still loads the documented shape.
  out.q_lora = geti(".attention.q_lora_rank", 576);
  out.kv_lora = geti(".attention.kv_lora_rank", 512);
  out.qk_nope = geti(".attention.key_length_mla", 192);  // TODO(box): MLA QK head dim key name
  out.v_head = geti(".attention.value_length", out.head_dim);  // V 128

  // RDA short conv + decay-gate lower bound (GLM KDA analogues). TODO(box): confirm presence/keys.
  out.d_conv = geti(".rda.conv_kernel", 4);                 // 0/absent => the graph skips rda_conv
  out.rda_lower_bound = getf(".rda.gate_lower_bound", -5.0f);

  // MoE routing / norm knobs.
  out.w_scale = getf(".expert_weights_scale", 1.0f);
  out.swiglu_clamp = getf(".swiglu_clamp", 0.0f);  // 0 => no clamp (plain SwiGLU)
  out.norm_eps = getf(".attention.layer_norm_rms_epsilon", 1e-5f);
  out.router_renormalize = geti(".expert_weights_norm", 1) != 0;

  // DFlash draft layers counted inside block_count (GLM's nextn). 0 => no draft block loaded.
  out.nextn = geti(".nextn_predict_layers", 0);

  // The per-layer attention-type array: attention.head_count_kv, one int per layer. 0 marks an RDA
  // (linear) layer, nonzero a Gated-MLA layer — GLM's is_recr convention exactly
  // (glm_model.cu:568-570, glm_model.hpp:90). Raon's published split is 18 RDA / 7 MLA.
  out.head_count_kv.assign((size_t)out.n_layers, 0);
  const GgufValue* kv = file.find(arch + ".attention.head_count_kv");
  if (kv != nullptr && !kv->integers.empty()) {
    if ((int)kv->integers.size() != out.n_layers) {
      err = "raon1 geometry: attention.head_count_kv has " + std::to_string(kv->integers.size()) +
            " entries, expected " + std::to_string(out.n_layers);
      return false;
    }
    for (int il = 0; il < out.n_layers; ++il) out.head_count_kv[(size_t)il] = (int)kv->integers[(size_t)il];
  } else {
    // TODO(box): the released Raon-1-mini GGUF is expected to ship the per-layer array. If it does
    // not, the exact 18/7 RDA/MLA pattern must be filled in here from the model card and confirmed
    // against the real weights — do NOT invent a pattern. Refuse until then.
    err = "raon1 geometry: attention.head_count_kv (per-layer RDA/MLA map) is missing; the exact "
          "18/7 pattern must be confirmed on the box before serving (do not guess)";
    return false;
  }

  const int n_rda = [&] { int n = 0; for (int x : out.head_count_kv) n += (x == 0); return n; }();
  const int n_mla = out.n_layers - n_rda;
  (void)n_mla;  // the 18/7 expectation is checked at box bring-up against the parity oracle
  return true;
}

// ------------------------------------------------------------------------------------------------
// Load / state.
//
// The FamilyGraph::load seam (u_family.hpp:48, called by the substrate at u_main.cpp:423 with the
// merged multi-part GGUF right after the factory returns) — the same contract GlmGraph::load proved
// on the box and Ds4Graph::load follows. What lands HERE (unit 1 of this lane): the raon1.* geometry
// metadata (Raon1Geometry::from_gguf), the device upload of every non-expert tensor into one arena
// (w_ slots; upload is NAME-AGNOSTIC, so the still-TODO(box) exact tensor names never block the
// load — the mixer unit picks them from the real file), the routed experts' registration with the
// shared ExpertTier + VRAM residency (the glm/ds4 pattern, raon1 profile env), and the state arena
// carve (per RDA layer: recurrence S [n_head x head_dim x head_dim] + short-conv history; per
// Gated-MLA layer: the append-only latent cache [context x kv_lora] floats — the F32 form; Q8/KV
// packing is a box perf option). sc_ / d_pos_ size with the mixer layout and land with unit 2;
// forward() fails closed on the mixer wiring, never on a half-allocated scratch.
bool Raon1Graph::load(const GgufFile& gguf, const std::uint8_t* mapped, std::string& err) {
  if (loaded_) { err = "raon1: load called twice"; return false; }
  if (!Raon1Geometry::from_gguf(gguf, geom_, g_, err)) return false;

  auto is_routed_expert = [](const std::string& n) {
    return n.find("ffn_gate_exps") != std::string::npos || n.find("ffn_up_exps") != std::string::npos ||
           n.find("ffn_down_exps") != std::string::npos;
  };
  auto is_draft = [&](const std::string& n) {   // GLM's nextn convention: blk.N. with N >= n_layers
    if (n.rfind("blk.", 0) != 0) return false;
    const std::size_t d = n.find('.', 4);
    if (d == std::string::npos) return false;
    try {
      return std::stoll(n.substr(4, d - 4)) >= g_.n_layers;
    } catch (...) {
      return false;
    }
  };

  // ---- upload every non-expert tensor to one device arena (the glm/ds4 pattern). The routed
  // experts stay host-mapped (the tier owns their placement, §3.5); the draft block's tensors are
  // skipped with the draft path itself (decision 5: base trunk first).
  std::uint64_t arena = 0;
  for (const auto& t : gguf.tensors)
    if (!is_routed_expert(t.name) && !is_draft(t.name)) arena += (t.bytes + 255u) & ~std::uint64_t{255u};
  if (arena == 0) { err = "raon1: no uploadable tensors in the GGUF"; return false; }
  if (cudaMalloc((void**) &w_arena_, arena) != cudaSuccess) { err = "raon1: weight arena alloc"; return false; }
  std::uint64_t at = 0;
  for (const auto& t : gguf.tensors) {
    if (is_routed_expert(t.name) || is_draft(t.name)) continue;
    const std::uint8_t* src = t.host_data ? t.host_data : mapped + t.offset;  // split-model aware
    if (cudaMemcpy(w_arena_ + at, src, t.bytes, cudaMemcpyHostToDevice) != cudaSuccess) {
      err = "raon1: upload " + t.name;
      return false;
    }
    WSlot s;
    s.type = (int) t.type;
    s.q = w_arena_ + at;   // device pointer (f32 also mirrored into f32 below)
    s.f32 = t.type == TensorType::f32 ? (const float*) (w_arena_ + at) : nullptr;
    s.n_in = (std::int64_t) t.dims.front();
    s.n_out = (std::int64_t) t.dims.back();
    w_[t.name] = s;
    at += (t.bytes + 255u) & ~std::uint64_t{255u};
  }

  // ---- routed experts: host-mapped WSlots into the tier, layers [dense_lead, n_layers)
  // (the dense lead layer has plain ffn_gate/up/down tensors, uploaded above). GLM naming
  // convention for the stacked expert tensors (ffn_gate_exps / ffn_up_exps / ffn_down_exps).
  experts_ = std::make_unique<ExpertTier>();
  for (int il = g_.dense_lead; il < g_.n_layers; ++il) {
    const std::string b = "blk." + std::to_string(il) + ".";
    auto slot = [&](const char* nm) -> WSlot {
      const GgufTensor* t = gguf.tensor(b + nm);
      WSlot s;
      if (t == nullptr) return s;
      s.type = t->type == TensorType::f32 ? 0 : (int) t->type;
      const std::uint8_t* src = t->host_data ? t->host_data : mapped + t->offset;
      s.q = (const void*) src;
      s.f32 = t->type == TensorType::f32 ? (const float*) src : nullptr;
      s.n_in = g_.n_embd;
      s.n_out = g_.n_ff_exp;
      return s;
    };
    const WSlot gate = slot("ffn_gate_exps.weight"), up = slot("ffn_up_exps.weight"),
                down = slot("ffn_down_exps.weight");
    if (gate.q == nullptr || up.q == nullptr || down.q == nullptr) {
      err = "raon1: blk." + std::to_string(il) + " routed expert tensors (ffn_gate/up/down_exps) missing";
      return false;
    }
    if (!experts_->register_layer(il, gate, up, down, g_.n_expert, g_.n_ff_exp, g_.swiglu_clamp, err))
      return false;
  }
  // ---- hot-expert VRAM residency: free VRAM less a calibratable headroom, warm-started from the
  // hit profile (STRATA_RAON1_EXPERT_PROFILE), converging later via the gated reg_rebalance — the
  // glm/ds4 block verbatim with the raon1 env name.
  {
    std::size_t vfree = 0, vtotal = 0;
    cudaMemGetInfo(&vfree, &vtotal);
    const std::size_t headroom = (std::size_t) Calibration::profile().residency_headroom_mib << 20;
    const std::int64_t budget = vfree > headroom ? (std::int64_t) (vfree - headroom) : 0;
    std::vector<std::pair<std::int32_t, std::int32_t>> ranked;
    if (const char* prof = std::getenv("STRATA_RAON1_EXPERT_PROFILE")) {
      std::int64_t pslots = 0;
      std::string perr;
      if (read_expert_profile(prof, g_.n_layers, g_.n_expert, ranked, pslots, perr)) {
        std::fprintf(stderr, "raon1: warm-start profile '%s' (%zu ranked experts)\n", prof, ranked.size());
      } else {
        std::fprintf(stderr, "raon1: WARN profile '%s' ignored: %s (cold start)\n", prof, perr.c_str());
        ranked.clear();
      }
    }
    if (!experts_->reg_finalize(budget, ranked, Calibration::profile().warm_seed,
                                Calibration::profile().stream_prefetch, err))
      return false;
    experts_->reg_set_cpu_lane(Calibration::profile().cpu_expert_frac, Calibration::profile().cpu_workers);
    std::fprintf(stderr, "raon1: expert residency %lld VRAM slots (%lld/layer of %d), %.2f GiB, %s start\n",
                 (long long) experts_->reg_slots(), (long long) (experts_->reg_slots() / std::max(1, g_.n_layers - g_.dense_lead)),
                 g_.n_expert, (double) budget / (1024.0 * 1024.0 * 1024.0), ranked.empty() ? "cold" : "warm");
    std::fflush(stderr);
  }

  // ---- state arena carve (offsets in FLOATS into state_; -1 = the layer has no such state).
  rda_state_.assign((std::size_t) g_.n_layers, -1);
  rda_conv_.assign((std::size_t) g_.n_layers, -1);
  mla_latent_.assign((std::size_t) g_.n_layers, -1);
  std::int64_t floats = 0;
  int n_rda = 0, n_mla = 0;
  for (int il = 0; il < g_.n_layers; ++il) {
    if (g_.is_rda(il)) {
      ++n_rda;
      rda_state_[(std::size_t) il] = floats;
      floats += (std::int64_t) g_.n_head * g_.head_dim * g_.head_dim;   // per-head delta S [d_k x d_v]
      if (g_.d_conv > 1) {
        rda_conv_[(std::size_t) il] = floats;
        floats += (std::size_t) g_.d_inner() * (g_.d_conv - 1);          // short-conv history (proj cat)
      }
    } else {
      ++n_mla;
      mla_latent_[(std::size_t) il] = floats;
      floats += (std::int64_t) context_ * g_.kv_lora;                    // append-only latent cache
    }
  }
  state_bytes_ = (std::size_t) floats * sizeof(float);
  if (state_bytes_ != 0 && cudaMalloc((void**) &state_, state_bytes_) != cudaSuccess) {
    err = "raon1: state arena alloc";
    return false;
  }
  if (state_bytes_ != 0) cudaMemset(state_, 0, state_bytes_);

  // sc_ (per-forward scratch) and d_pos_ (MLA query positions) size WITH the mixer layout and land
  // with the mixer wiring unit; forward()/layer_*() fail closed on the wiring, never here.
  loaded_ = true;
  pos_ = 0;
  std::fprintf(stderr,
               "raon1: %dL (%d RDA + %d MLA), hidden %d, %d experts top-%d + %d shared (dense_lead %d, "
               "ff %d/%d), q_lora %d kv_lora %d qk %d v %d, d_conv %d, ctx %u, vocab %d, nextn %d; "
               "state %.2f GiB\n",
               g_.n_layers, n_rda, n_mla, g_.n_embd, g_.n_expert, g_.n_exp_used, g_.n_shared, g_.dense_lead,
               g_.n_ff_dense, g_.n_ff_exp, g_.q_lora, g_.kv_lora, g_.qk_nope, g_.v_head, g_.d_conv, context_,
               g_.n_vocab, g_.nextn, (double) state_bytes_ / (1024.0 * 1024.0 * 1024.0));
  std::fflush(stderr);
  return true;
}

void Raon1Graph::reset_sequence() {
  pos_ = 0;
  // Zero every RDA recurrence S + conv history; the MLA latent caches are append-only and reused
  // from position 0 (the logical length is the position itself).
  if (state_ != nullptr && state_bytes_ != 0) cudaMemset(state_, 0, state_bytes_);
}

// ------------------------------------------------------------------------------------------------
// Per-layer mixers. Each writes its mixer output back into the residual scratch; the GLM analogy is
// named per step. sc_ slices (x, q, k, v, g, ...) are carved by the shared scratch layout (TODO box).
// ------------------------------------------------------------------------------------------------
bool Raon1Graph::layer_rda(int il, int T, std::string& err) {
  // GLM analogy: glm_model.cu KDA mixer. Pre-norm -> projections -> short conv (+SiLU) -> gate ->
  // gated-delta recurrence -> RMS out-gate -> output projection. NoPE: no rope anywhere.
  //
  //   1. rmsnorm(x, attn_norm.weight)                         u::k::rmsnorm
  //   2. project q, k, v, g (the gate pre-activation) and beta from the normed x
  //        q,k,v: matmul to (head_dim*n_head);  g: to d_inner;  beta: to n_head (per-head scalar)
  //        u::k::matmul / u::k::mmvq with blk(il,"attn_q.weight") etc. TODO(box): exact Raon names
  //   3. u::raon::rda_conv(x_proj=[q|k|v or the mixed proj], conv_w, conv_state, d_inner, d_conv,T)
  //        skipped when g_.d_conv <= 1 (see Raon1Geometry TODO)
  //   4. u::raon::rda_gate(g, decay_a=blk(il,"ssm_a"), g_.rda_lower_bound, head_dim, n_head, T, g1)
  //   5. u::raon::rda_recurrence(q, k, v, g1, beta, state_+rda_state_[il], n_head, head_dim, T, o)
  //   6. u::raon::rda_out_gate(o, norm_w=blk(il,"attn_out_norm"), g2, head_dim, n_head, T, eps, go)
  //   7. matmul(go -> n_embd) with blk(il,"attn_output.weight"); add into the residual
  //
  // TODO(box): confirm (a) the projection tensors and whether q/k/v share one fused proj like GLM's
  // packed KDA proj, (b) beta's projection + sigmoid, (c) the g2 (out-gate) source, (d) d_conv.
  (void)il; (void)T;
  err = "raon1 layer_rda: mixer wiring pending (this lane's unit 2); the raon1_rda kernels are landed";
  return false;
}

bool Raon1Graph::layer_mla(int il, int T, std::string& err) {
  // GLM analogy: glm_model.cu DSA/MLA mixer, DENSE (no indexer) + NoPE. Pre-norm -> q/kv LoRA
  // projections -> absorb -> dense latent attention -> v-absorb -> gate -> output projection.
  //
  //   1. rmsnorm(x, attn_norm.weight)                         u::k::rmsnorm
  //   2. q:  matmul x -> q_a (q_lora); rmsnorm(q_a, attn_q_a_norm); matmul -> q (n_head*qk_nope)
  //      kv: matmul x -> kv_a (kv_lora); rmsnorm(kv_a, attn_kv_a_norm) = the latent for this token,
  //          APPENDED to the latent cache at state_+mla_latent_[il] + pos (append-only).
  //      NoPE: no rope split on q or on the latent (u_geometry pins RopeKind::nope).
  //   3. u::raon::mla_absorb_q(q, wk_b=blk(il,"attn_k_b.weight"), kv_lora, qk_nope, n_head, T, q_abs)
  //   4. u::raon::mla_attention(q_abs, latents=cache, d_pos_, wv_b=blk(il,"attn_v_b.weight"),
  //                             kv_lora, n_head, v_head, qk_nope, T, o)   // dense, online softmax
  //   5. u::raon::mla_out_gate(o, gate, v_head, n_head, T, go)   // the "Gated" in Gated-MLA
  //        TODO(box): confirm the gate's source/shape (a projection from the normed x, or absent).
  //   6. matmul(go [n_head*v_head] -> n_embd) with blk(il,"attn_output.weight"); add into residual
  //
  // TODO(box): confirm the kv_a/kv_b split (whether the cached latent is the post-norm kv_lora
  // vector, deepseek2/GLM style), the q_b up-projection width (n_head*qk_nope), and that Raon's MLA
  // is dense (no k-pool indexer). If an indexer ships, port glm_dsa pool/score/select ahead of (4).
  (void)il; (void)T;
  err = "raon1 layer_mla: mixer wiring pending (this lane's unit 2); the raon1_mla kernels are landed";
  return false;
}

bool Raon1Graph::layer_ffn(int il, int T, std::string& err) {
  // Layer 0 (il < dense_lead): a plain dense SwiGLU MLP over n_ff_dense=12032.
  // Layers 1..24: sigmoid-routed MoE (128 experts, top-6) + 1 always-on shared expert over
  // n_ff_exp=1088. GLM analogy: glm_model.cu MoE tail (router_sigmoid + grouped experts + shexp).
  //
  //   pre-norm: rmsnorm(x, ffn_norm.weight)                               u::k::rmsnorm
  //
  //   dense (il < dense_lead):
  //     g = matmul(xn, ffn_gate.weight); u = matmul(xn, ffn_up.weight)
  //     h = u::k::swiglu(g, u, g_.swiglu_clamp, n_ff_dense)               // silu(g)*u
  //     y = matmul(h, ffn_down.weight); add into residual
  //
  //   MoE (il >= dense_lead):
  //     logits = matmul(xn, ffn_gate_inp.weight)            // [T, n_expert]
  //     u::k::router_sigmoid(logits, bias=blk(il,"exp_probs_b.bias"), T, n_expert, n_exp_used,
  //                          g_.router_renormalize, g_.w_scale, ids, w)   // top-6 ids + norm weights
  //     u::ExpertTier::apply(il, xn, ids, w, n_exp_used, T, y, stream)    // routed experts sum
  //     // shared expert (always on): SwiGLU over n_ff_exp with ffn_{gate,up,down}_shexp, add to y
  //     y += shexp(xn);  add y into residual
  //
  // TODO(box): confirm (a) the dense activation is SwiGLU (not GELU) and the clamp limit, (b) the
  // router bias tensor name, (c) that the shared expert uses the same n_ff_exp width, (d) whether
  // the routed-weight renormalize happens before or after the w_scale (GLM scales post-norm).
  (void)il; (void)T;
  err = "raon1 layer_ffn: MoE wiring pending (this lane's unit 2; ExpertTier is landed — register_layer "
        "already ran at load)";
  return false;
}

bool Raon1Graph::forward(const std::int32_t* tokens, int n_tokens, std::string& err) {
  // The trunk, GLM's forward shape: embed -> for each layer {mixer (RDA|MLA) ; FFN (dense|MoE)} ->
  // final norm -> output head. State persists across calls (append tokens); reset per sequence.
  //
  //   embed: gather token_embd.weight rows for `tokens` into the residual x  (NoPE: no pos add)
  //   for il in [0, n_layers):
  //       if g_.is_rda(il)  layer_rda(il, n_tokens)   else  layer_mla(il, n_tokens)
  //       layer_ffn(il, n_tokens)       // dense when il < dense_lead, else MoE
  //   rmsnorm(x, output_norm.weight); logits = matmul(x_last, output.weight)  -> sc_ logits
  //
  // Position bookkeeping: d_pos_[t] = pos_ + t for the MLA cache visibility; pos_ += n_tokens.
  if (!loaded_) { err = "raon1 forward: graph not loaded"; return false; }
  for (int il = 0; il < g_.n_layers; ++il) {
    const bool ok = (g_.is_rda(il) ? layer_rda(il, n_tokens, err) : layer_mla(il, n_tokens, err)) &&
                    layer_ffn(il, n_tokens, err);
    if (!ok) return false;
  }
  pos_ += n_tokens;
  (void)tokens;
  return true;
}

// ------------------------------------------------------------------------------------------------
// The DFlash-style draft block (design decision 5: drafting is included in Plan 1, reusing GLM's
// NextN/DFlash; the fallback is "base graph parities first, drafting follows"). Written here as a
// documented structure only — the base RDA/MLA/MoE trunk above is what the first box parity gate
// checks; speculative decode is enabled once the trunk parities. GLM analogy: glm_fast_path.cu
// fast_mtp + the NextN block (one extra decoder layer at index n_layers + a draft head).
//
// TODO(box): if g_.nextn > 0, load the draft block's tensors (blk.<n_layers>.*: enorm/hnorm, the
// eh_proj, its own attn + ffn, the shared draft head) and, after forward() of position p with the
// chosen token for p+1, run the block at p to propose p+2 (greedy), then verify in the trunk. The
// exact DFlash chaining/verification is confirmed against the real Raon draft block — do not guess
// the proposal/accept rule here.
// ------------------------------------------------------------------------------------------------

// ------------------------------------------------------------------------------------------------
// FamilyGraph surface.
// ------------------------------------------------------------------------------------------------
Raon1Graph::~Raon1Graph() {
  // TODO(box): free w_arena_, state_, sc_, d_pos_ and release the ExpertTier registration.
  if (w_arena_) cudaFree(w_arena_);
  if (state_) cudaFree(state_);
  if (sc_) cudaFree(sc_);
  if (d_pos_) cudaFree(d_pos_);
}

std::string Raon1Graph::describe() const {
  return "raon1(Raon-1-mini: 25L, " + std::to_string(g_.n_head) + "h, hidden " +
         std::to_string(g_.n_embd) + ", 128x" + std::to_string(g_.n_exp_used) + "+" +
         std::to_string(g_.n_shared) + " MoE, RDA+GatedMLA, NoPE)";
}

bool Raon1Graph::generate(const Gen& request, const LineSink& sink, std::string& error) {
  // The serve loop (host-driven, device compute). GLM analogy: the maya serve loop over forward() +
  // the sampler. Steps:
  //   1. reset_sequence(); forward(request.ids) to consume the prompt, emitting PP progress.
  //   2. loop up to request.max_new: sample the last logits (u::k::sample with the request's
  //      params), emit "T <id>", append the token with forward({id}, 1), honor STOP/stop ids.
  //   3. emit the terminal DONE <produced> <prompt> <prompt_ms> <decode_ms> <finish> 0 0 0.
  // Speculative hits/steps stay 0 until the DFlash draft block is enabled (decision 5).
  if (!loaded_) {
    error = "raon1 generate: graph not loaded (device upload wired by the shared loader)";
    return false;
  }
  reset_sequence();
  if (request.ids.empty()) { error = "raon1 generate: empty prompt"; return false; }
  if (!forward((const int32_t*)request.ids.data(), (int)request.ids.size(), error)) return false;

  // TODO(box): the sampling + emit loop against u::k::sample_tokens and the Gen params (temperature,
  // top_p, seed, stop ids) and the PP/T/DONE records (u_protocol) — the sampler is landed; the loop
  // lands with the mixer unit. Fail closed until then.
  (void)sink;
  error = "raon1 generate: decode loop lands with the mixer wiring (this lane's unit 2)";
  return false;
}

// ------------------------------------------------------------------------------------------------
// Factory. Builds the graph for the guarded geometry + serving context WITHOUT loading; the
// substrate drives the FamilyGraph::load seam with the mapped multi-part GGUF right after this
// returns (u_main.cpp:423) — the same contract make_glm_graph / make_ds4_graph use. available()
// reports false until that load lands.
// ------------------------------------------------------------------------------------------------
std::unique_ptr<FamilyGraph> make_raon1_graph(const Geometry& geom, std::uint32_t context,
                                              std::string& error) {
  error.clear();
  if (geom.family != FamilyId::raon1) {
    error = "make_raon1_graph: geometry is not raon1";
    return nullptr;
  }
  auto graph = std::unique_ptr<Raon1Graph>(new Raon1Graph());
  graph->geom_ = geom;      // guarded common dims seed; the raon1.* keys are read at load
  graph->context_ = context;
  return graph;
}

}  // namespace u
