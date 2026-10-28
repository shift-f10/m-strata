// strata-unified-engine: the Qwen3.6-35B-A3B (arch "qwen35moe") model graph — implementation.
//
// Ported from upstream/src/qwen35/q36_engine.cu (the production engine) with the clean fp32
// reference q36ref.cu as the spec. The per-layer schedule, the GDN / full-attention mixers and the
// softmax MoE follow the reference forward (q36ref.cu:609 forward); the family-unique kernels are in
// src/kernels/qwen35moe_gdn.* and src/kernels/qwen35moe_attn.*. Everything generic rides the shared
// substrate: the generic kernels in kernels/common.cuh (u::k::) and the expert-tiering facility in
// u_experts.hpp (u::ExpertTier).
//
// NOT COMPILED ON CI/dev-host. The unified engine builds the family .cu only behind STRATA_U_CUDA on
// the owner's GPU box (design §1.3). A host/CI build links the stub and u::make_family_graph returns
// it. This file is written for the box, which compiles and parities it against the real qwen35moe
// weights (tools/qwen35/: per-layer hidden states, greedy tokens, teacher-forced KL).
//
// ================================================================================================
// ASSUMED SHARED API — reconcile with the LANDED kernels/common.cuh (u::k::) and u_experts.hpp
// (u::ExpertTier) before the first box build. These are the shared seams this lane does not own; the
// signatures below are what the call sites in forward()/layer_*()/generate() assume. Where the
// landed header differs, adjust the call sites — the math and the qwen35moe kernels do not change.
//
//   namespace u::k {   // kernels/common.cuh (LANDED — the calls below use its real signatures)
//     enum class Quant { f32, bf16, q8_0, q4_k, q5_k, q6_k };
//     void mmvq_f32(Quant q, const void* weights, const float* x, void* scratch_q8_1, float* y,
//                   int n_in, int n_out, int ncols, void* stream);         // quantized matvec/matmul
//     void f32_gemv(const float* x, const float* w, float* y, int64 n_in, int64 n_out, void* stream);
//     void rms_norm(float* x, const float* w, int64 rows, int64 cols, float eps, void* stream);
//     void swiglu(const float* gate, const float* up, float* out, int64 n, void* stream);
//     void add_inplace(float* dst, const float* src, int64 n, void* stream);
//     void scale_inplace(float* x, int64 n, float s, void* stream);
//     void router_topk(const float* logits, int n_tokens, int n_expert, int k, int* ids,
//                      float* weights, void* stream);   // SOFTMAX top-k renormalized (THE qwen router)
//     void moe_combine(const float* parts, const float* weights, const float* shared, float* output,
//                      int64 n_embd, int64 k, void* stream);
//     void sample_tokens(const float* logits, int n_tokens, int n_vocab, const int* history,
//                        int history_len, const SamplerParams& p, int* out, void* stream);
//   }
//   // u_experts.hpp — the VRAM/RAM expert-tiering facility. One token's routed-expert FFN sum:
//   bool u::ExpertTier::compute(int64 layer, const int32* ids /*host*/, const float* weights /*host*/,
//                               const float* x /*device [n_embd]*/, float* out /*device [n_embd]*/,
//                               int64 k, void* stream, std::string& err);
//   // (the shared-expert SCALAR-sigmoid gate is qwen-specific, so this graph owns the shared expert
//   //  and does NOT set ExpertTier::Config.has_shared_expert — see layer_moe.)
//
// THREE SEAMS THIS LANE FLAGS FOR THE BOX (none is a bug in this family):
//   SEAM-1 (GgufFile): the factory contract make_qwen35moe_graph() is handed only the guarded
//     Geometry, but load()/Qwen35MoeGeometry::from_gguf need the parsed GgufFile (weights + the
//     qwen35moe.* metadata). Same seam the Raon lane flagged (raon1_graph.cu:373). The CUDA dispatch
//     that calls this factory must pass the GgufFile through; until then load() returns false.
//   SEAM-2 (shared loader): the device upload (dequant policy, w_arena_ fill, ExpertTier::open +
//     set_host_arena for the 256 experts/layer, the state_/kv_/sc_ arenas) is the shared loader's
//     job (T1.1/T1.2). This lane SIZES and NAMES the layout (gdn_state_/gdn_conv_/kv_k_/kv_v_) so it
//     is reviewable; the allocation + upload land with the shared loader.
//   SEAM-3 (sampler/sessions): the serve loop here drives u::k::sample_tokens + the u_protocol sink;
//     reuse/sessions (RESUME) are the host engine's, not this graph's.
// ================================================================================================
#include "qwen35moe_graph.hpp"

#include "kernels/common.cuh"
#include "kernels/qwen35moe_attn.cuh"
#include "kernels/qwen35moe_gdn.cuh"
#include "u_experts.hpp"

#include <cuda_runtime.h>

#include <string>
#include <vector>

namespace u {
namespace {

// Per-layer weight-name prefix, llama.cpp/GGUF convention. The qwen35moe tensor names are the ones
// the layout table checks (upstream/include/strata/core/family_layout.hpp:148-216): e.g.
// "blk.<il>.attn_qkv.weight", "blk.<il>.ssm_conv1d.weight", "blk.<il>.attn_q.weight",
// "blk.<il>.ffn_gate_inp.weight", "blk.<il>.ffn_gate_exps.weight". [[maybe_unused]] until the box
// wires the call sites onto the shared weight map.
[[maybe_unused]] std::string blk(int il, const char* suffix) {
  return "blk." + std::to_string(il) + "." + suffix;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// Geometry: the qwen35moe.* metadata beyond the common (guarded) u::Geometry. Mirrors
// strata::core::geometry_from_gguf (family_layout.hpp:30). SEAM-1: needs the parsed GgufFile.
// ------------------------------------------------------------------------------------------------
bool Qwen35MoeGeometry::from_gguf(const GgufFile& file, const Geometry& common, Qwen35MoeGeometry& out,
                                  std::string& err) {
  // Common, guarded fields straight from u::Geometry (u_geometry.cpp already range-checked them).
  out.n_embd = (int) common.embedding_length;
  out.n_head = (int) common.head_count;
  out.head_dim = (int) common.head_dim;  // 256 (attention.key_length)
  out.n_vocab = (int) common.vocab_size;
  out.n_expert = (int) common.expert_count;
  out.n_exp_used = (int) common.expert_used_count;  // 8
  out.n_shared = (int) common.expert_shared_count;  // 1
  out.n_ff_exp = (int) common.expert_feed_forward_length;
  out.block_count = (int) common.block_count;
  out.rope_base = common.rope_freq_base;  // 1e7

  const std::string arch = common.arch;  // "qwen35moe"
  auto geti = [&](const char* suffix, std::int64_t def) -> std::int64_t {
    const GgufValue* v = file.find(arch + suffix);
    return (v != nullptr) ? v->integer : def;
  };
  auto getf = [&](const char* suffix, double def) -> double {
    const GgufValue* v = file.find(arch + suffix);
    return (v != nullptr) ? v->real : def;
  };

  // attention.head_count_kv is NOT in the common geometry (u_geometry reads only head_count); read it
  // here. GQA 16/2 for qwen35moe.
  out.n_head_kv = (int) geti(".attention.head_count_kv", 2);

  // The GDN (ssm.*) dims. All required for qwen35moe (the upstream guard requires them); the defaults
  // are the published shape so an inspectable graph exists before the box run.
  out.ssm_d_conv = (int) geti(".ssm.conv_kernel", 4);
  out.ssm_state_size = (int) geti(".ssm.state_size", 128);     // D
  out.ssm_k_heads = (int) geti(".ssm.group_count", 16);        // HK
  out.ssm_v_heads = (int) geti(".ssm.time_step_rank", 32);     // HV
  out.ssm_value_dim = (int) geti(".ssm.inner_size", out.ssm_state_size * out.ssm_v_heads);  // HV*D
  out.ssm_conv_channels = 2 * out.ssm_state_size * out.ssm_k_heads + out.ssm_value_dim;     // C

  // The layer schedule and the shared-expert width / rope / norm knobs.
  out.qsa_interval = (int) geti(".full_attention_interval", 4);
  out.n_ff_shexp = (int) geti(".expert_shared_feed_forward_length", 0);
  out.n_rot = (int) geti(".rope.dimension_count", 64);
  out.eps = (float) getf(".attention.layer_norm_rms_epsilon", 1e-6);
  out.n_mtp = (int) geti(".nextn_predict_layers", 0);  // the in-file MTP block count (0 or 1)
  out.n_layers = out.block_count - out.n_mtp;

  // Shape invariants (upstream family_layout.hpp:87-94). Fail closed.
  if (out.ssm_value_dim != out.ssm_state_size * out.ssm_v_heads) {
    err = "qwen35moe geometry: ssm.inner_size != ssm.state_size * ssm.time_step_rank";
    return false;
  }
  if (out.ssm_v_heads % out.ssm_k_heads != 0) {
    err = "qwen35moe geometry: ssm.time_step_rank is not a multiple of ssm.group_count";
    return false;
  }
  if (out.n_head % out.n_head_kv != 0) {
    err = "qwen35moe geometry: attention.head_count is not a multiple of attention.head_count_kv";
    return false;
  }
  if (out.qsa_interval < 2 || out.n_layers % out.qsa_interval != 0) {
    err = "qwen35moe geometry: the trunk layer count is not a multiple of full_attention_interval";
    return false;
  }
  if (out.ssm_d_conv != 4 || out.ssm_state_size != 128) {
    // The GDN kernels (qwen35moe_gdn.cu) are built for d_conv 4 and a 128-wide state (the register
    // layout and the conv history are sized for these). A different shape needs a kernel change —
    // refuse rather than compute wrong numbers. (compiled_geometry_mismatch, family_layout.hpp:99.)
    err = "qwen35moe geometry: this engine's GDN kernels are built for ssm.conv_kernel 4 and "
          "ssm.state_size 128";
    return false;
  }
  if (out.n_ff_shexp <= 0) {
    err = "qwen35moe geometry: expert_shared_feed_forward_length is missing (the shared expert width)";
    return false;
  }
  return true;
}

// ------------------------------------------------------------------------------------------------
// Load / state layout. SEAM-1 + SEAM-2.
// ------------------------------------------------------------------------------------------------
bool Qwen35MoeGraph::load(const Geometry& common, std::uint32_t context, std::string& err) {
  context_ = context;
  // Copy the guarded common fields; the qwen35moe-specific ones keep their published defaults until
  // Qwen35MoeGeometry::from_gguf runs (SEAM-1: it needs the GgufFile, which the factory does not yet
  // receive). This mirrors the Raon lane (raon1_graph.cu:161).
  g_.n_embd = (int) common.embedding_length;
  g_.n_head = (int) common.head_count;
  g_.head_dim = (int) common.head_dim;
  g_.n_vocab = (int) common.vocab_size;
  g_.n_expert = (int) common.expert_count;
  g_.n_exp_used = (int) common.expert_used_count;
  g_.n_shared = (int) common.expert_shared_count;
  g_.n_ff_exp = (int) common.expert_feed_forward_length;
  g_.block_count = (int) common.block_count;
  g_.rope_base = common.rope_freq_base;
  // Published schedule until from_gguf confirms it on the box: 40 trunk layers, interval 4.
  g_.n_layers = g_.block_count - g_.n_mtp;
  g_.ssm_conv_channels = 2 * g_.ssm_state_size * g_.ssm_k_heads + g_.ssm_value_dim;

  // Size and NAME the per-layer state layout (reviewable). The device allocation + weight upload are
  // the shared loader's (SEAM-2). Per the reference init_state (q36ref.cu:558):
  //   * per GDN layer (is_attn false): recurrent state S = HV*D*D floats; conv history = C*(d_conv-1);
  //   * per full-attention layer (is_attn true): K cache + V cache = HKV*ctx*HD floats each.
  gdn_state_.assign((size_t) g_.n_layers, -1);
  gdn_conv_.assign((size_t) g_.n_layers, -1);
  kv_k_.assign((size_t) g_.n_layers, -1);
  kv_v_.assign((size_t) g_.n_layers, -1);
  // TODO(box, SEAM-2): with the GgufFile and the shared loader:
  //   1. Qwen35MoeGeometry::from_gguf(file, common, g_, err) — the real dims + schedule.
  //   2. upload every tensor to w_arena_ (dequant policy per the shared loader); fill w_ (quantized
  //      matrices: attn_qkv/attn_gate/ssm_out/attn_q/k/v/output/ffn_*_exps/ffn_*_shexp/token_embd/
  //      output) and f_ (f32 vectors: *_norm.weight, ssm_a, ssm_dt.bias, ssm_conv1d.weight,
  //      ffn_gate_inp_shexp.weight).
  //   3. ExpertTier::open({n_layers, n_expert=256, n_embd, n_ff=n_ff_exp, gate/up/down quants,
  //      blob offsets, vram_slots from --expert-slots, per_layer_admission=true,
  //      has_shared_expert=false}) + set_host_arena over the GGUF expert blobs + warm_start.
  //   4. carve state_ (GDN S + conv), kv_ (K/V caches), sc_ (activation/router/expert/logits scratch
  //      sized from n_embd, ssm_conv_channels, ssm_value_dim, n_head*head_dim, n_ff_exp, n_ff_shexp,
  //      n_expert, n_vocab and the max chunk T); cudaStreamCreate(&stream_).
  //   5. loaded_ = true.
  loaded_ = false;
  err = "qwen35moe load: device upload + GgufFile are wired by the shared loader (SEAM-1/SEAM-2); "
        "not linked in this lane";
  return false;
}

void Qwen35MoeGraph::reset_sequence() {
  pos_ = 0;
  // TODO(box): cudaMemsetAsync every GDN recurrent state (state_ ranges named by gdn_state_) and conv
  // history (gdn_conv_) to 0; the KV caches (kv_k_/kv_v_) are append-only, so pos_ = 0 restores them.
  // (q36_engine.cu:1516 zeroes conv + S at session start.)
}

// ------------------------------------------------------------------------------------------------
// Per-layer mixers + MoE. Each is the documented kernel-call sequence (shared u::k:: + the family
// qwen35moe:: kernels); sc_ slices (h, qkv, z, alpha, beta, q, gate, kc, vc, router, expert outputs)
// are carved by the shared scratch layout (SEAM-2). Reference: q36ref.cu:609-706 forward.
// ------------------------------------------------------------------------------------------------
bool Qwen35MoeGraph::layer_gdn(int il, int T, std::string& err) {
  // GDN linear-attention mixer (reference q36ref.cu:616-639; production q36_engine.cu:1884-1927).
  // Pre-norm h = rmsnorm(x, attn_norm) is done by the caller (forward). Then, for a T-token chunk:
  //   1. qkv   = mmvq_f32(attn_qkv.weight,  h)   -> [T, C]        (C = ssm_conv_channels, q|k|v)
  //      z     = mmvq_f32(attn_gate.weight, h)   -> [T, ssm_value_dim]   (the SiLU gate)
  //      alpha = mmvq_f32(ssm_alpha.weight, h)   -> [T, HV]
  //      beta  = mmvq_f32(ssm_beta.weight,  h)   -> [T, HV]
  //   2. u::qwen35moe::gdn_gate_beta(alpha, ssm_dt.bias, ssm_a, beta, HV, T)   // log-decay + sigmoid
  //   3. u::qwen35moe::gdn_conv(gdn_conv_[il], qkv, ssm_conv1d.weight, hc, C, 2*HK, eps, T, /*write*/true)
  //   4. u::qwen35moe::gdn_recurrence(gdn_state_[il], hc, C, alpha, beta, z, ssm_norm.weight, eps,
  //                                   gdn_y, HK, HV, T, /*write*/true)        // delta rule + gated norm
  //   5. y = mmvq_f32(ssm_out.weight, gdn_y) -> [T, n_embd]
  //   6. x += y   (u::k::add_inplace, done by forward)
  // The alpha/beta/z/qkv projections are quantized (mmvq); ssm_a/ssm_dt.bias/ssm_conv1d/ssm_norm are
  // f32 (f_). TODO(box, SEAM-2): the scratch slices + the weight map; no math changes.
  (void) il;
  (void) T;
  err = "qwen35moe layer_gdn: scratch/weight layout pending the shared loader (SEAM-2); kernels ready";
  return false;
}

bool Qwen35MoeGraph::layer_attn(int il, int T, std::int64_t pos0, std::string& err) {
  // Full-attention mixer (reference q36ref.cu:640-651; production q36_engine.cu:1941-1996). Pre-norm
  // h = rmsnorm(x, attn_norm) by the caller. For a T-token chunk whose token 0 is at position pos0:
  //   1. qg = mmvq_f32(attn_q.weight, h) -> [T, H, 2*HD]   (q | gate per head)
  //      kx = mmvq_f32(attn_k.weight, h) -> [T, HKV, HD]
  //      vx = mmvq_f32(attn_v.weight, h) -> [T, HKV, HD]
  //   2. u::qwen35moe::attn_prep(qg, kx, vx, attn_q_norm.weight, attn_k_norm.weight, q, gate,
  //                              kv_k_[il], kv_v_[il], H, HKV, HD=256, n_rot=64, (int)pos0, ctx,
  //                              rope_base, eps, T)
  //      (per-head RMS + NEOX RoPE over the first 64 dims; k/v appended to the caches at pos0+t)
  //   3. u::qwen35moe::attn(q, kv_k_[il], kv_v_[il], gate, att, H, HKV, HD, (int)pos0, ctx,
  //                         1.f/sqrtf(HD), T)    // softmax(q.k/sqrt(HD)) @ v * sigmoid(gate)
  //   4. y = mmvq_f32(attn_output.weight, att) -> [T, n_embd]
  //   5. x += y   (by forward)
  // TODO(box, SEAM-2): scratch slices + weight map. The split-K / FP16 prompt path (q36_engine.cu:301
  // k_attn_part, :439 k_attn_softmax16) is the box's throughput option for long prompts.
  (void) il;
  (void) T;
  (void) pos0;
  err = "qwen35moe layer_attn: scratch/weight layout pending the shared loader (SEAM-2); kernels ready";
  return false;
}

bool Qwen35MoeGraph::layer_moe(int il, int T, std::string& err) {
  // MoE: 256 experts, top-8 SOFTMAX-routed + renormalized, plus one always-on sigmoid-gated shared
  // expert (reference q36ref.cu:654-698; production q36_engine.cu:2009-2107). Pre-norm
  // h2 = rmsnorm(x, post_attention_norm) by the caller. For each token row t (loop; the box adds the
  // grouped multi-token expert path, q36_engine.cu:1818 k_group, for prefill throughput):
  //   1. logits = mmvq_f32(ffn_gate_inp.weight, h2) -> [T, n_expert]
  //   2. u::k::router_topk(logits, T, n_expert=256, k=8, ids_dev, wts_dev)  // SOFTMAX top-8 renorm
  //        ROUTER NOTE / TODO-R: the shared softmax top-k router is the qwen35moe router — NOT
  //        router_sigmoid. u_geometry.cpp:85 mislabels qwen35moe's router as sigmoid; the box/
  //        substrate lane must set it to softmax (RouterKind::softmax). The sigmoid in qwen35moe is
  //        only the shared-expert scalar gate (step 4).
  //   3. routed experts: copy ids_dev/wts_dev of row t to host, then
  //        u::ExpertTier::compute(il, ids_host, wts_host, h2_row_t, routed_t, k=8, stream_, err)
  //        (silu(gate_e . h2)*(up_e . h2) -> down_e, summed by the renormalized weights)
  //   4. shared expert (sigmoid scalar gate; this graph owns it — NOT ExpertTier):
  //        sg = mmvq_f32(ffn_gate_shexp.weight, h2) -> [n_ff_shexp]
  //        su = mmvq_f32(ffn_up_shexp.weight,   h2) -> [n_ff_shexp]
  //        sh = u::k::swiglu(sg, su, n_ff_shexp)
  //        sy = mmvq_f32(ffn_down_shexp.weight, sh) -> [n_embd]
  //        gscal = f32_gemv(ffn_gate_inp_shexp.weight /*f32 [n_embd]*/, h2) -> scalar
  //        x_t += routed_t + sigmoid(gscal) * sy    (reference k_moe_sum, q36ref.cu:339)
  // TODO(box, SEAM-2): scratch slices, the host copy of ids/wts, the ExpertTier wiring. ExpertTier is
  // opened WITHOUT has_shared_expert (the sigmoid scalar gate is family-specific, handled above).
  (void) il;
  (void) T;
  err = "qwen35moe layer_moe: ExpertTier + scratch wiring pending the shared loader (SEAM-2); "
        "router/shared-expert structure above";
  return false;
}

bool Qwen35MoeGraph::forward(const std::int32_t* tokens, int n_tokens, std::int64_t pos0,
                             std::string& err) {
  // The trunk (reference q36ref.cu:609 forward). State persists across calls (append tokens); it is
  // reset per sequence. For a chunk of n_tokens tokens whose token 0 is at sequence position pos0:
  //   embed:  gather token_embd.weight rows for `tokens` into the residual x [T, n_embd]
  //           (k_row dequant, reference q36ref.cu:592 embed)
  //   for il in [0, n_layers):
  //       h = rmsnorm(x, blk(il,"attn_norm.weight"))                      // u::k::rms_norm
  //       if g_.is_attn(il)  layer_attn(il, T, pos0)   else  layer_gdn(il, T)   // x += mixer(h)
  //       h2 = rmsnorm(x, blk(il,"post_attention_norm.weight"))
  //       layer_moe(il, T)                                                // x += moe(h2)
  //   h = rmsnorm(x, "output_norm.weight"); logits = mmvq_f32("output.weight", h_last) -> sc_ logits
  // Positions: the full-attention layers write their K/V at pos0+t; GDN carries its recurrent state.
  if (!loaded_) {
    err = "qwen35moe forward: graph not loaded";
    return false;
  }
  for (int il = 0; il < g_.n_layers; ++il) {
    const bool ok = (g_.is_attn(il) ? layer_attn(il, n_tokens, pos0, err) : layer_gdn(il, n_tokens, err)) &&
                    layer_moe(il, n_tokens, err);
    if (!ok) return false;
  }
  pos_ = pos0 + n_tokens;
  (void) tokens;
  return true;
}

// ------------------------------------------------------------------------------------------------
// The GGUF-native MTP (multi-token-prediction) draft block, blk.<n_layers> (production
// q36_engine.cu:2136 forward_mtp, :2691 spec_step; loaded only when g_.n_mtp > 0 and --mtp N > 0).
// The MTP block is one extra decoder layer (an attention + MoE layer like the trunk's) plus an
// eh_proj over concat(enorm(embedding), hnorm(hidden)) and a shared_head_norm. Drafting NEVER
// changes the greedy text: the verify window decides, so a wrong draft is simply rejected (the
// answer is the plain greedy answer whatever N is — q36_engine.cu:2108). The base trunk parities
// first; speculative decode is enabled once the trunk parities (design: drafting follows).
// ------------------------------------------------------------------------------------------------
bool Qwen35MoeGraph::forward_mtp(int T, std::int64_t pos0, const float* seeds, bool draft,
                                 std::string& err) {
  // Reference structure (q36_engine.cu:2136 forward_mtp):
  //   cat = [ rms(embed(tok_t), nextn.enorm.weight) | rms(seeds_t, nextn.hnorm.weight) ]  (2*n_embd)
  //   x   = mmvq_f32(nextn.eh_proj.weight, cat)                  -> [T, n_embd]
  //   h   = rmsnorm(x, blk(n_layers,"attn_norm.weight"))
  //   run one attention layer (layer_attn against the MTP block's own K/V at state index n_layers)
  //   + one MoE layer (layer_moe), then hmtp = rms(x, nextn.shared_head_norm.weight)
  //   if draft: logits_m = mmvq_f32("output.weight", hmtp_last); pick_m = argmax (the proposed token)
  (void) T;
  (void) pos0;
  (void) seeds;
  (void) draft;
  err = "qwen35moe forward_mtp: MTP block pending the trunk parity + shared loader (SEAM-2); "
        "structure above (q36_engine.cu:2136)";
  return false;
}

int Qwen35MoeGraph::spec_step(int x_tok, const std::vector<std::int32_t>& fix, int draft_n,
                              std::vector<std::int32_t>& drafts, std::int32_t* picks, std::string& err) {
  // One speculative step with draft_n MTP drafts (greedy), returning the accepted count a (production
  // q36_engine.cu:2691 spec_step): chain draft_n greedy proposals from the MTP block, then verify
  // them in the trunk in one window [x, d1..dN]; accept the longest matching prefix; commit a+1
  // positions. picks[0..a] are the verified tokens. TODO(box): the chaining + verify + commit (needs
  // forward_mtp + a verify-window forward + the MTP block's K/V); drafting follows the trunk parity.
  (void) x_tok;
  (void) fix;
  (void) draft_n;
  (void) drafts;
  (void) picks;
  err = "qwen35moe spec_step: MTP speculative decode pending the trunk parity (q36_engine.cu:2691)";
  return -1;
}

// ------------------------------------------------------------------------------------------------
// FamilyGraph surface.
// ------------------------------------------------------------------------------------------------
Qwen35MoeGraph::~Qwen35MoeGraph() {
  // TODO(box): free w_arena_, state_, kv_, sc_; delete the ExpertTier; destroy stream_.
  if (w_arena_) cudaFree(w_arena_);
  if (state_) cudaFree(state_);
  if (kv_) cudaFree(kv_);
  if (sc_) cudaFree(sc_);
  if (stream_) cudaStreamDestroy(static_cast<cudaStream_t>(stream_));
}

std::string Qwen35MoeGraph::describe() const {
  return "qwen35moe(Qwen3.6-35B-A3B: " + std::to_string(g_.n_layers) + "L, GDN+full(" +
         std::to_string(g_.qsa_interval) + "), " + std::to_string(g_.n_head) + "/" +
         std::to_string(g_.n_head_kv) + " heads hd" + std::to_string(g_.head_dim) + ", " +
         std::to_string(g_.n_expert) + "x" + std::to_string(g_.n_exp_used) + "+" +
         std::to_string(g_.n_shared) + " MoE" + (g_.n_mtp > 0 ? ", MTP" : "") + ")";
}

bool Qwen35MoeGraph::generate(const Gen& request, const LineSink& sink, std::string& error) {
  // The serve loop (host-driven, device compute; production q36_engine.cu serve :3625-3675, emitting
  // through the u_protocol sink instead of printf). Steps:
  //   1. reset_sequence(); process the prompt request.ids in chunks through forward(), emitting
  //      PP <done> <total> <ms> <tok_s> progress per chunk (u::emit of u::PromptProgress).
  //   2. decode up to request.max_new:
  //        greedy/sample the last row's logits (u::k::sample_tokens with request.params);
  //        emit "T <id>" (u::Token); append the token with forward({id}, 1, pos_); stop on an EOS id
  //        or a STOP. With --mtp N and a greedy request, use spec_step for verified multi-token steps
  //        (the emitted text is identical to plain greedy).
  //   3. emit the terminal DONE <produced> <prompt> <prompt_ms> <decode_ms> <finish>
  //      <spec_hits> <spec_steps> <reused> (u::Done; spec_hits/steps are the accepted/offered draft
  //      counts, 0 without --mtp).
  if (!loaded_) {
    error = "qwen35moe generate: graph not loaded (device upload wired by the shared loader, SEAM-2)";
    return false;
  }
  reset_sequence();
  if (request.ids.empty()) {
    error = "qwen35moe generate: empty prompt";
    return false;
  }
  if (!forward(reinterpret_cast<const std::int32_t*>(request.ids.data()), (int) request.ids.size(),
               /*pos0=*/0, error)) {
    return false;
  }
  // TODO(box, SEAM-3): the sampling + emit loop (u::k::sample_tokens against the Gen params:
  // temperature/top_p/top_k/min_p/penalties/seed), the PP/T/DONE records (u_protocol emit), stop-id
  // handling, and the --mtp spec_step path. Structured; the trunk forward() above is this lane's
  // piece. Fail closed until wired.
  (void) sink;
  error = "qwen35moe generate: decode loop pending the shared sampler integration (SEAM-3)";
  return false;
}

// ------------------------------------------------------------------------------------------------
// Factory (the design contract).
// ------------------------------------------------------------------------------------------------
std::unique_ptr<FamilyGraph> make_qwen35moe_graph(const Geometry& geom, std::uint32_t context,
                                                  std::string& error) {
  error.clear();
  if (geom.family != FamilyId::qwen35moe) {
    error = "make_qwen35moe_graph: geometry is not qwen35moe";
    return nullptr;
  }
  auto graph = std::unique_ptr<Qwen35MoeGraph>(new Qwen35MoeGraph());
  // SEAM-1: load() needs the parsed GgufFile (weights + the qwen35moe.* metadata that
  // Qwen35MoeGeometry::from_gguf reads). The host foundation's make_family_graph currently passes only
  // the guarded Geometry; the CUDA dispatch that calls this factory must pass the GgufFile through (a
  // shared-file change this lane does not own — the Raon lane flagged the same seam,
  // raon1_graph.cu:373). Until that seam lands, load() returns false and the graph reports
  // unavailable — expected on the first integration build, not a bug in this family.
  if (!graph->load(geom, context, error)) return nullptr;
  return graph;
}

}  // namespace u
