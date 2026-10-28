// engines/unified/src/family/qwen4exp_graph.cu - the Qwen3.8-Flash-Next (arch "qwen4exp") family graph.
//
// PORTED FROM the working GLM-5.3 graph (engines/unified/src/family/glm_graph.cu): the two models are
// architectural siblings (GDN<->KDA gated delta-net, QSA<->DSA sparse attention, 4-stream hyper-
// connections<->mHC, i-quant MoE on the shared u::ExpertTier), so the load/step/generate shape is the
// GLM one with the qwen4exp tensor names, the GDN/QSA/HC kernels (kernels/qwen4exp_*), and the softmax
// top-k router. The upstream references are named per op in the kernel headers and layer.hpp.
//
// STAGE STATUS (bring-up): load() is wired (geometry from the GGUF, weight upload, ExpertTier register,
// the state/scratch carve, residency). The step() trunk and the decode loop land next; PLE (external
// n-gram table, applied at 1 layer) is deferred so the --native trunk can be validated first. Built
// only behind STRATA_U_CUDA on the owner's GPU box (design §1.3); the host build links the stub.
#include "family/qwen4exp_graph.hpp"

#include "kernels/common.cuh"
#include "kernels/u_iq.cuh"  // k::dequant_iq4xs (token_embd gather)
#include "kernels/qwen4exp_gdn.cuh"
#include "kernels/qwen4exp_hc.cuh"
#include "kernels/qwen4exp_ple.cuh"
#include "kernels/qwen4exp_qsa.cuh"
#include "u_experts.hpp"
#include "u_gguf.hpp"
#include "u_calibration.hpp"

#include <cuda_profiler_api.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace u {

namespace {
#define QWEN_CU(call, err)                                                                           \
  do {                                                                                               \
    const cudaError_t e_ = (call);                                                                   \
    if (e_ != cudaSuccess) {                                                                         \
      (err) = std::string("qwen4exp: ") + cudaGetErrorString(e_) + " @ " __FILE__ ":" +              \
              std::to_string(__LINE__);                                                              \
      return false;                                                                                  \
    }                                                                                                \
  } while (0)

// Per-layer weight-name prefix, llama.cpp/upstream convention ("blk.<il>.").
std::string blk(int il, const char* suffix) { return "blk." + std::to_string(il) + "." + suffix; }

bool is_routed_expert(const std::string& n) {
  return n.find("ffn_gate_exps") != std::string::npos || n.find("ffn_up_exps") != std::string::npos ||
         n.find("ffn_down_exps") != std::string::npos;
}
// Tensors NOT uploaded to the device weight arena: the routed experts (streamed by the tier) and the
// PLE n-gram table `per_layer_token_embd.weight` (a ~28.8 GB IQ4_NL table of 320M rows read on demand
// from the host, NEVER resident — ple.cuh). Uploading it oversubscribes VRAM (WDDM spills to host).
bool skip_from_arena(const std::string& n) {
  return is_routed_expert(n) || n.find("per_layer_token_embd") != std::string::npos;
}

// Host IEEE half -> float (the PLE n-gram table is IQ4_NL: an fp16 scale per 32-elem block).
float half_to_float(std::uint16_t h) {
  const std::uint32_t sign = (std::uint32_t)(h & 0x8000u) << 16;
  std::uint32_t exp = (h >> 10) & 0x1fu, man = h & 0x3ffu, f;
  if (exp == 0) {
    if (man == 0) {
      f = sign;
    } else {
      exp = 127 - 15 + 1;
      while ((man & 0x400u) == 0) { man <<= 1; --exp; }
      man &= 0x3ffu;
      f = sign | (exp << 23) | (man << 13);
    }
  } else if (exp == 0x1fu) {
    f = sign | 0x7f800000u | (man << 13);
  } else {
    f = sign | ((exp - 15 + 127) << 23) | (man << 13);
  }
  float out;
  std::memcpy(&out, &f, 4);
  return out;
}

// Dequantize one IQ4_NL row of `n` elements (n % 32 == 0; 18 B / 32 elems) into `out`. Matches ggml
// dequantize_row_iq4_nl: per block {fp16 d, 16 qs bytes}, y[j]=d*kv[qs[j]&0xf], y[j+16]=d*kv[qs[j]>>4].
void dequant_iq4nl_row(const std::uint8_t* blk_bytes, int n, float* out) {
  static const float kv[16] = {-127.f, -104.f, -83.f, -65.f, -49.f, -35.f, -22.f, -10.f,
                               1.f,    13.f,   25.f,  38.f,  53.f,  69.f,  89.f,  113.f};
  const int nb = n / 32;
  for (int b = 0; b < nb; ++b) {
    const std::uint8_t* blk = blk_bytes + (std::size_t)b * 18;
    std::uint16_t hd;
    std::memcpy(&hd, blk, 2);
    const float d = half_to_float(hd);
    const std::uint8_t* qs = blk + 2;
    float* y = out + (std::size_t)b * 32;
    for (int j = 0; j < 16; ++j) {
      y[j] = d * kv[qs[j] & 0xf];
      y[j + 16] = d * kv[qs[j] >> 4];
    }
  }
}

// STRATA_QWEN_PROF=1: coarse per-phase decode timing (sync-bracketed; perturbs absolute speed but the
// attn / moe / other split is the root-cause signal). Reset after prefill, printed in the decode summary.
bool g_prof = false;
double g_prof_attn_ms = 0.0, g_prof_moe_ms = 0.0;
}  // namespace

// ------------------------------------------------------------------------------------------------
// Geometry: the qwen4exp.* metadata beyond the common (guarded) u::Geometry.
// ------------------------------------------------------------------------------------------------
bool Qwen4ExpGeometry::from_gguf(const GgufFile& file, const Geometry& common, Qwen4ExpGeometry& out,
                                 std::string& err) {
  out.n_embd = (int)common.embedding_length;
  out.n_vocab = (int)common.vocab_size;
  out.n_expert = (int)common.expert_count;
  out.n_exp_used = (int)common.expert_used_count;
  out.n_ff_exp = (int)common.expert_feed_forward_length;
  out.n_shared = (int)common.expert_shared_count;
  out.rope_freq_base = common.rope_freq_base;
  // The shared expert is a NAMED tensor (ffn_*_shexp), not counted by expert_shared_count — which this
  // checkpoint omits (so the common guard read 0). Detect it from the tensor itself.
  if (out.n_shared == 0 && file.tensor("blk.0.ffn_gate_shexp.weight") != nullptr) out.n_shared = 1;

  const std::string arch = common.arch;  // "qwen4exp"
  auto geti = [&](const char* suffix, int def) -> int {
    const GgufValue* v = file.find(arch + suffix);
    return (v != nullptr && v->type != GgufType::array) ? (int)v->integer : def;
  };
  auto getf = [&](const char* suffix, float def) -> float {
    const GgufValue* v = file.find(arch + suffix);
    return (v != nullptr && v->type != GgufType::array) ? (float)v->real : def;
  };

  const int block_count = geti(".block_count", (int)common.block_count);
  out.n_mtp_layers = geti(".nextn_predict_layers", 0);  // qwen4exp ships MTP externally => 0
  out.n_layers = block_count - out.n_mtp_layers;

  out.qsa_interval = geti(".full_attention_interval", 4);
  out.rms_eps = getf(".attention.layer_norm_rms_epsilon", 1e-6f);

  out.ssm_state = geti(".ssm.state_size", 128);
  out.ssm_k_heads = geti(".ssm.group_count", 16);
  out.ssm_v_heads = geti(".ssm.time_step_rank", 48);
  out.ssm_d_conv = geti(".ssm.conv_kernel", 4);
  out.ssm_value_dim = geti(".ssm.inner_size", out.ssm_state * out.ssm_v_heads);
  out.ssm_conv_channels = 2 * out.ssm_state * out.ssm_k_heads + out.ssm_value_dim;

  out.n_head = geti(".attention.head_count", 24);
  out.n_head_kv = geti(".attention.head_count_kv", 2);
  out.head_dim = geti(".attention.key_length", (int)common.head_dim);
  out.n_rot = geti(".rope.dimension_count", 64);
  out.idx_q_heads = geti(".attention.indexer.head_count", 4);
  out.idx_key_dim = geti(".attention.indexer.key_length", 128);
  out.idx_top_k = geti(".attention.indexer.top_k", 2048);

  out.hc = geti(".hyper_connection.count", 4);
  out.hc_lr = geti(".hyper_connection.low_rank", 320);

  if (out.n_layers <= 0 || out.n_embd <= 0 || out.n_head <= 0 || out.n_head_kv <= 0 || out.head_dim <= 0 ||
      out.n_expert <= 0 || out.n_exp_used <= 0 || out.n_exp_used > out.n_expert || out.n_ff_exp <= 0 ||
      out.ssm_state <= 0 || out.ssm_k_heads <= 0 || out.ssm_v_heads <= 0 || out.ssm_d_conv <= 0 ||
      out.qsa_interval < 2) {
    err = "qwen4exp geometry: a key is zero or out of range";
    return false;
  }
  if (out.ssm_value_dim != out.ssm_state * out.ssm_v_heads) {
    err = "qwen4exp geometry: ssm.inner_size != ssm.state_size * ssm.time_step_rank";
    return false;
  }
  if (out.n_head % out.n_head_kv != 0) {
    err = "qwen4exp geometry: head_count is not a multiple of head_count_kv";
    return false;
  }
  if (out.n_layers % out.qsa_interval != 0) {
    err = "qwen4exp geometry: layer count is not a multiple of full_attention_interval";
    return false;
  }
  return true;
}

// ================================ construction / describe ================================
Qwen4ExpGraph::Qwen4ExpGraph(const Geometry& common, std::uint32_t context)
    : geom_(common), context_(context), max_ctx_(context) {
  // Seed the common fields; the GDN/QSA/HC/PLE keys are refined from the GGUF in load() (from_gguf).
  g_.n_embd = (int)common.embedding_length;
  g_.n_layers = (int)common.block_count;
  g_.n_vocab = (int)common.vocab_size;
  g_.n_expert = (int)common.expert_count;
  g_.n_exp_used = (int)common.expert_used_count;
  g_.n_ff_exp = (int)common.expert_feed_forward_length;
  g_.n_shared = (int)common.expert_shared_count;
  g_.head_dim = (int)common.head_dim;
  g_.rope_freq_base = common.rope_freq_base;
}

Qwen4ExpGraph::~Qwen4ExpGraph() {
  if (cs_) cudaStreamDestroy((cudaStream_t)cs_);
  if (graph_exec_) cudaGraphExecDestroy((cudaGraphExec_t)graph_exec_);
  if (d_tok_in_) cudaFree(d_tok_in_);
  if (h_tok_in_) cudaFreeHost(h_tok_in_);
  if (h_pos_) cudaFreeHost(h_pos_);
  if (h_ple_emb_) cudaFreeHost(h_ple_emb_);
  if (d_ids_all_) cudaFree(d_ids_all_);
  if (h_ids_all_) cudaFreeHost(h_ids_all_);
  if (w_arena_) cudaFree(w_arena_);
  if (state_) cudaFree(state_);
  if (sc_) cudaFree(sc_);
  if (d_pos_) cudaFree(d_pos_);
  if (d_tok_) cudaFree(d_tok_);
  if (q8_scratch_) cudaFree(q8_scratch_);
  if (bf16_x_) cudaFree(bf16_x_);
  for (std::vector<void*>* v : {&win_exec_, &commit_exec_})
    for (void* e : *v)
      if (e) cudaGraphExecDestroy((cudaGraphExec_t)e);
  for (void* d : {(void*)wa_, (void*)rw_, (void*)rp_, (void*)ple_ng_win_, (void*)ple_hist_win_, q8_win_,
                  (void*)d_tok_win_, (void*)d_pos_win_, (void*)d_pos_rows_win_, (void*)d_ids_win_,
                  (void*)d_out_win_})
    if (d) cudaFree(d);
  for (void* h : {(void*)h_tok_win_, (void*)h_pos_win_, (void*)h_pos_rows_win_, (void*)h_ple_win_,
                  (void*)h_ids_win_, (void*)h_out_win_, (void*)h_mio_, (void*)h_mout_})
    if (h) cudaFreeHost(h);
  for (void* d : {mtp_dense_, (void*)mtp_exp_, (void*)mtp_res_, (void*)mtp_tail0_, mtp_head_, (void*)mtp_dvocab_,
                  (void*)mtp_kc_, (void*)mtp_vc_, (void*)mtp_sc_, (void*)mtp_ids_, (void*)mtp_eff_, mtp_xq8_, mtp_hq8_,
                  (void*)d_mtok_, (void*)d_mcell_, (void*)d_mrows_, (void*)d_mpick_, (void*)d_mtokout_,
                  (void*)d_mprob_})
    if (d) cudaFree(d);
}

std::string Qwen4ExpGraph::describe() const {
  return "qwen4exp(Qwen3.8-Flash-Next: " + std::to_string(g_.n_layers) + "L [" +
         std::to_string(g_.n_gdn()) + " GDN + " + std::to_string(g_.n_qsa()) + " QSA], hidden " +
         std::to_string(g_.n_embd) + ", " + std::to_string(g_.n_expert) + "x" +
         std::to_string(g_.n_exp_used) + "+" + std::to_string(g_.n_shared) + " MoE, ctx " +
         std::to_string(context_) + ")";
}

const WSlot& Qwen4ExpGraph::W(int il, const char* suffix) const { return ws_.at(blk(il, suffix)); }
bool Qwen4ExpGraph::has(int il, const char* suffix) const { return ws_.count(blk(il, suffix)) != 0; }

// y[0,n_out) = W x, dispatched on the weight's stored type.
void Qwen4ExpGraph::gemv_(const WSlot& w, const float* x, float* out, std::int64_t n_out,
                          std::int64_t n_in, void* stream) {
  if (w.type == 0) {
    k::f32_gemv(x, w.f32, out, n_in, n_out, stream);
  } else if (w.type == (int)k::Quant::bf16) {
    // the f32 activation rounded in-kernel, 16-byte weight loads (no f32_to_bf16 launch); an unaligned
    // operand falls back to the two-launch form
    if (!k::bf16_gemv_f32x(x, (const std::uint16_t*)w.q, out, n_in, n_out, 1, stream)) {
      static bool s_warned = false;
      if (!s_warned) {
        s_warned = true;
        std::fprintf(stderr, "qwen4exp: a bf16 GEMV (n_in %lld) is unaligned: the two-launch form runs it\n",
                     (long long)n_in);
      }
      k::f32_to_bf16(x, bf16_x_, n_in, stream);
      k::bf16_gemv(bf16_x_, (const std::uint16_t*)w.q, out, n_in, n_out, stream);
    }
  } else {
    k::mmvq_f32((k::Quant)w.type, w.q, x, q8_scratch_, out, (int)n_in, (int)n_out, 1, stream);
  }
}

// gemv_ over `ncols` activation columns (x + c*n_in -> out + c*n_out), each column bitwise a gemv_ call
// on it: the multi-column kernels' contract (STRATA_KERNEL_SELFTEST), the verify window's GEMVs.
void Qwen4ExpGraph::gemv_multi_(const WSlot& w, const float* x, float* out, std::int64_t n_out, std::int64_t n_in,
                                int ncols, void* stream) {
  if (ncols == 1) {
    gemv_(w, x, out, n_out, n_in, stream);
  } else if (w.type == 0) {
    for (int c = 0; c < ncols; ++c) k::f32_gemv(x + c * n_in, w.f32, out + c * n_out, n_in, n_out, stream);
  } else if (w.type == (int)k::Quant::bf16) {
    if (!k::bf16_gemv_f32x(x, (const std::uint16_t*)w.q, out, n_in, n_out, ncols, stream))
      for (int c = 0; c < ncols; ++c) gemv_(w, x + c * n_in, out + c * n_out, n_out, n_in, stream);  // as gemv_ falls back
  } else {
    k::mmvq_f32((k::Quant)w.type, w.q, x, q8_win_, out, (int)n_in, (int)n_out, ncols, stream);
  }
}

void Qwen4ExpGraph::rmsnorm_to(float* out, const float* in, const float* w, std::int64_t n, float eps,
                               void* stream) {
  cudaMemcpy(out, in, (std::size_t)n * sizeof(float), cudaMemcpyDeviceToDevice);
  k::rms_norm(out, w, 1, n, eps, stream);
}

// ================================ load: weights + arenas ================================
bool Qwen4ExpGraph::load(const GgufFile& gguf, const std::uint8_t* mapped, std::string& err) {
  // ---- the exact geometry from the file (GDN/QSA/HC keys + the trunk layer count).
  if (!Qwen4ExpGeometry::from_gguf(gguf, geom_, g_, err)) return false;
  std::fprintf(stderr, "qwen4exp: %d trunk layers (%d GDN / %d QSA), %d experts x%d + %d shared, hidden %d\n",
               g_.n_layers, g_.n_gdn(), g_.n_qsa(), g_.n_expert, g_.n_exp_used, g_.n_shared, g_.n_embd);

  // ---- upload the non-expert tensors to a device arena, map name -> WSlot. The routed experts
  // (ffn_*_exps) go to the shared tier; everything else (dense/norm/router/hc/ssm/qsa/shexp/embed/head)
  // is served in its stored type through gemv_ (the parity gate validates the mmvq path).
  std::uint64_t arena = 0;
  for (const auto& t : gguf.tensors) {
    if (skip_from_arena(t.name)) continue;
    arena += (t.bytes + 255u) & ~std::uint64_t{255u};
  }
  QWEN_CU(cudaMalloc(&w_arena_, arena), err);
  w_arena_bytes_ = arena;
  std::uint64_t at = 0;
  for (const auto& t : gguf.tensors) {
    if (skip_from_arena(t.name)) continue;
    const std::uint8_t* src = t.host_data ? t.host_data : mapped + t.offset;
    QWEN_CU(cudaMemcpy((std::uint8_t*)w_arena_ + at, src, t.bytes, cudaMemcpyHostToDevice), err);
    WSlot slot;
    slot.n_out = (std::int64_t)(t.dims.empty() ? 0 : t.dims.back());
    slot.n_in = (std::int64_t)(t.dims.empty() ? 0 : t.dims.front());
    if (t.type == TensorType::f32) {
      slot.type = 0;
      slot.f32 = (const float*)((std::uint8_t*)w_arena_ + at);
    } else {
      slot.type = (int)t.type;
      slot.q = (const void*)((std::uint8_t*)w_arena_ + at);
    }
    ws_[t.name] = slot;
    at += (t.bytes + 255u) & ~std::uint64_t{255u};
  }

  // ---- hand each layer's routed experts to the shared tier (MoE on every layer; softmax top-k).
  // qwen4exp uses a plain (non-clamped) SwiGLU inside the expert FFN; pass a large limit as a no-op
  // clamp so the tier's clamped-SwiGLU path reduces to plain SwiGLU (silu(gate)*up). [TODO(box): confirm
  // the tier honours this; if it hard-codes the GLM clamp, add a plain-SwiGLU mode.]
  experts_ = std::make_unique<ExpertTier>();
  for (int il = 0; il < g_.n_layers; ++il) {
    auto slot = [&](const char* nm) -> WSlot {
      const GgufTensor* t = gguf.tensor(blk(il, nm));
      WSlot s;
      if (t == nullptr) return s;
      s.type = t->type == TensorType::f32 ? 0 : (int)t->type;
      const std::uint8_t* src = t->host_data ? t->host_data : mapped + t->offset;
      s.q = (const void*)src;
      s.f32 = t->type == TensorType::f32 ? (const float*)src : nullptr;
      s.n_in = (std::int64_t)g_.n_embd;
      s.n_out = (std::int64_t)g_.n_ff_exp;
      return s;
    };
    if (!experts_->register_layer(il, slot("ffn_gate_exps.weight"), slot("ffn_up_exps.weight"),
                                  slot("ffn_down_exps.weight"), g_.n_expert, g_.n_ff_exp, 1e30f, err,
                                  ExpertTier::Act::swiglu))
      return false;
  }

  // ---- pin the routed experts in RAM (as the upstream engine does): a streamed expert's copy from the
  // pageable mmap made the host thread do a staging memcpy per tensor; from pinned memory it is a true
  // async DMA. STRATA_QWEN_PIN_EXPERTS=0 keeps the mmap.
  // If the CPU expert lane will read the pinned arena (STRATA_QWEN_CPU_LANE=1), pin it CACHED: the GPU's
  // zero-copy misses prefer write-combined, but a CPU core cannot read WC memory at any speed.
  if (const char* cl = std::getenv("STRATA_QWEN_CPU_LANE"); cl != nullptr && cl[0] == '1')
    experts_->reg_set_host_cached(true);
  if (const char* pe = std::getenv("STRATA_QWEN_PIN_EXPERTS"); pe == nullptr || pe[0] != '0') {
    const auto t_pin = std::chrono::steady_clock::now();
    const std::int64_t pinned = experts_->reg_pin_host_arena(std::numeric_limits<std::int64_t>::max());
    std::fprintf(stderr, "qwen4exp: pinned %.2f GiB of routed experts in RAM (%.1f s)\n",
                 (double)pinned / (1024.0 * 1024.0 * 1024.0),
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t_pin).count());
  }

  // ---- PLE (per-layer embedding) metadata + the host-mapped n-gram table. ple_layer_ < 0 disables it.
  if (const GgufValue* L = gguf.find("qwen4exp.ple.layers"); L != nullptr && !L->integers.empty()) {
    auto geti = [&](const char* k, std::int64_t d) { const GgufValue* v = gguf.find(k); return v ? v->integer : d; };
    ple_layer_ = (int)L->integers[0];
    ple_ngram_ = (int)geti("qwen4exp.ple.ngram_size", 3);
    ple_per_gram_ = (int)geti("qwen4exp.ple.heads_per_ngram", 8);
    ple_n_heads_ = (ple_ngram_ - 1) * ple_per_gram_;
    ple_kern_ = (int)geti("qwen4exp.ple.conv_kernel", 4);
    ple_eos_ = geti("qwen4exp.ple.eos_token_id", 0);
    if (const GgufValue* m = gguf.find("qwen4exp.ple.layer_multipliers"); m)
      for (auto x : m->integers) ple_mult_.push_back((std::uint64_t)x);
    if (const GgufValue* o = gguf.find("qwen4exp.ple.head_offsets"); o)
      for (auto x : o->integers) ple_off_.push_back((std::uint32_t)x);
    if (const GgufValue* vs = gguf.find("qwen4exp.ple.head_vocab_sizes"); vs)
      for (auto x : vs->integers) ple_vocab_.push_back((std::uint32_t)x);
    // The shared host addressing (u_ple.hpp): the same params, ONE Spec, used by
    // ple_prepare_into (anchored for the host by ple_ngram_window_and_row_addressing).
    ple_spec_.ngram = ple_ngram_;
    ple_spec_.per_gram = ple_per_gram_;
    ple_spec_.eos = (int) ple_eos_;
    ple_spec_.mult = ple_mult_;
    ple_spec_.off = ple_off_;
    ple_spec_.vocab = ple_vocab_;
    const GgufTensor* tbl = gguf.tensor("per_layer_token_embd.weight");
    if (tbl != nullptr && tbl->dims.size() >= 2) {
      ple_head_dim_ = (int)tbl->dims.front();
      ple_n_rows_ = (std::int64_t)tbl->dims[1];
      ple_row_bytes_ = (std::uint32_t)(ple_head_dim_ / 32 * 18);  // IQ4_NL: 18 B per 32 elems
      ple_table_ = tbl->host_data ? tbl->host_data : mapped + tbl->offset;
    }
    if (ple_table_ == nullptr || (int)ple_mult_.size() < ple_ngram_ ||
        (int)ple_off_.size() < ple_n_heads_ || (int)ple_vocab_.size() < ple_n_heads_ ||
        ple_head_dim_ * ple_n_heads_ != g_.n_embd) {
      std::fprintf(stderr, "qwen4exp: PLE metadata/table incomplete; PLE disabled\n");
      ple_layer_ = -1;
    } else {
      ple_emb_host_.assign((std::size_t)g_.n_embd, 0.0f);
      std::fprintf(stderr, "qwen4exp: PLE on layer %d (%d heads x %d dim, ngram %d, %lld rows)\n",
                   ple_layer_, ple_n_heads_, ple_head_dim_, ple_ngram_, (long long)ple_n_rows_);
    }
  }

  // ---- the state arena (persists across tokens). The hc residual streams live at the head (ping-pong
  // between two (hc, n_embd) stacks), then per GDN layer the recurrence state S (state*state*v_heads)
  // and the conv history ((d_conv-1)*conv_channels), per QSA layer the K/V caches (head_dim*n_head_kv*
  // max_ctx each) and the indexer pooled-key cache (idx_key_dim*idx_q_heads*n_pools).
  const std::int64_t hc_dim = (std::int64_t)g_.hc * g_.n_embd;
  const std::int64_t n_pools = max_ctx_ / g_.idx_block;
  std::int64_t floats = hc_dim;  // the hc residual streams R (updated in place by hc_inject; no ping-pong)
  gdn_S_.assign(g_.n_layers, -1);
  gdn_conv_.assign(g_.n_layers, -1);
  qsa_k_.assign(g_.n_layers, -1);
  qsa_v_.assign(g_.n_layers, -1);
  qsa_pool_.assign(g_.n_layers, -1);
  for (int il = 0; il < g_.n_layers; ++il) {
    if (!g_.is_qsa(il)) {
      gdn_S_[il] = floats;
      floats += (std::int64_t)g_.ssm_state * g_.ssm_state * g_.ssm_v_heads;
      gdn_conv_[il] = floats;
      floats += (std::int64_t)(g_.ssm_d_conv - 1) * g_.ssm_conv_channels;
    } else {
      qsa_k_[il] = floats;
      floats += (std::int64_t)g_.head_dim * g_.n_head_kv * max_ctx_;
      qsa_v_[il] = floats;
      floats += (std::int64_t)g_.head_dim * g_.n_head_kv * max_ctx_;
      qsa_pool_[il] = floats;
      floats += (std::int64_t)g_.idx_key_dim * g_.idx_q_heads * n_pools;
    }
  }
  if (ple_layer_ >= 0) {  // PLE conv history: (kern-1)*dilation vectors of hc_dim, oldest-first (dil = ngram)
    ple_hist_ = floats;
    floats += (std::int64_t)(ple_kern_ - 1) * ple_ngram_ * hc_dim;
  }
  state_bytes_ = (std::uint64_t)floats * sizeof(float);
  QWEN_CU(cudaMalloc(&state_, state_bytes_), err);

  // ---- the scratch arena (one token's activations; reused each token).
  const std::int64_t value_dim = g_.ssm_value_dim;                       // GDN value width (6144)
  const std::int64_t key_dim = (std::int64_t)g_.ssm_state * g_.ssm_k_heads;  // GDN q/k width (2048)
  const std::int64_t conv_ch = g_.ssm_conv_channels;                     // GDN conv channels (10240)
  const std::int64_t qdim = (std::int64_t)g_.n_head * g_.head_dim;       // QSA q width (6144)
  const std::int64_t kvdim = (std::int64_t)g_.n_head_kv * g_.head_dim;   // QSA k/v width (512)
  const std::int64_t ff_sh = (std::int64_t)g_.n_ff_exp * g_.n_shared;    // shared-expert FFN (640)
  std::int64_t s = 0;
  auto take = [&](std::int64_t n) { const std::int64_t at2 = s; s += std::max<std::int64_t>(n, 1); return at2; };
  sc_emb = take(g_.n_embd);          // embedding row (f32)
  sc_xn = take(hc_dim);              // HC: per-stream normed streams (flat hc_dim)
  sc_lo = take(g_.hc_lr);            // HC: low-rank down output (320)
  sc_hg = take(hc_dim);              // HC: up-projected gate (hc_dim)
  sc_inj = take(g_.hc);              // HC: inject_raw (hc)
  sc_bin = take(g_.n_embd);          // HC: block input / collapse output (n_embd)
  sc_qkv = take(conv_ch);            // GDN qkv proj
  sc_cv = take(conv_ch);             // GDN conv+silu
  sc_q = take(key_dim);
  sc_k = take(key_dim);
  sc_v = take(value_dim);
  sc_al = take(g_.ssm_v_heads);
  sc_be = take(g_.ssm_v_heads);
  sc_gt = take(g_.ssm_v_heads);
  sc_z = take(value_dim);
  sc_o = take(value_dim);
  sc_y = take(value_dim);
  sc_qg = take(2 * qdim);            // QSA q|gate (interleaved, 12288)
  sc_qq = take(qdim);                // QSA q (deinterleaved)
  sc_qg2 = take(qdim);               // QSA gate (deinterleaved)
  sc_qk = take(kvdim);
  sc_qv = take(kvdim);
  sc_at = take(qdim);                // QSA attention output
  sc_mixer = take(g_.n_embd);        // mixer output (GDN|QSA)
  sc_rlog = take(g_.n_expert);
  sc_ids = take(g_.n_exp_used);
  sc_rw = take(g_.n_exp_used);
  sc_moe = take(g_.n_embd);
  sc_sg = take(ff_sh);
  sc_su = take(ff_sh);
  sc_sh = take(ff_sh);
  sc_shd = take(g_.n_embd);
  sc_ffn = take(g_.n_embd);
  sc_head = take(g_.n_embd);
  sc_logits = take(g_.n_vocab);
  sc_ple_emb = take(g_.n_embd);
  sc_ple_key = take(hc_dim);
  sc_ple_val = take(g_.n_embd);
  sc_ple_kn = take(hc_dim);
  sc_ple_qn = take(hc_dim);
  sc_ple_gate = take(g_.hc);
  sc_ple_gated = take(hc_dim);
  sc_ple_ng = take(hc_dim);
  sc_ple_conv = take(hc_dim);
  sc_bytes_ = (std::uint64_t)s * sizeof(float);
  QWEN_CU(cudaMalloc(&sc_, sc_bytes_), err);
  // one position per rope row (k::rope_neox_base reads pos[row], rows = heads): n_head entries, all the
  // step's position; kv_append / attention read entry 0
  pos_rows_ = std::max(g_.n_head, g_.n_head_kv);
  QWEN_CU(cudaMalloc(&d_pos_, (std::size_t)pos_rows_ * sizeof(int)), err);
  QWEN_CU(cudaMalloc(&d_tok_, sizeof(int)), err);
  {
    cudaStream_t cstream = nullptr;
    QWEN_CU(cudaStreamCreate(&cstream), err);  // blocking stream: legacy-default-stream ops stay ordered with it
    cs_ = cstream;
  }

  // ---- matmul-dispatch scratch (gemv_): widest reduction dim over every GEMV this graph issues.
  max_n_in_ = std::max<std::int64_t>({(std::int64_t)g_.n_embd, value_dim, qdim, hc_dim,
                                      (std::int64_t)g_.hc_lr, ff_sh});
  QWEN_CU(cudaMalloc(&q8_scratch_, k::q8_1_scratch_bytes((int)((max_n_in_ + 31) / 32 * 32), 1)), err);
  QWEN_CU(cudaMalloc(&bf16_x_, (std::size_t)max_n_in_ * sizeof(std::uint16_t)), err);

  // ---- the MTP draft layer (speculative decode, S3; STRATA_QWEN_MTP=<mtp/rt dir>): loaded before the expert
  // residency is sized, since it keeps ~1 GiB in VRAM (dense weights, 512 experts, the draft head, its K/V).
  if (const char* st = std::getenv("STRATA_QWEN_SPEC_T"); st != nullptr && std::atoi(st) >= 2) {
    if (const char* md = std::getenv("STRATA_QWEN_MTP"); md != nullptr && md[0] != '\0') {
      if (!mtp_load_(md, err)) return false;
      mtp_on_ = true;
      if (const char* mp = std::getenv("STRATA_QWEN_SPEC_MIN_P"); mp != nullptr && mp[0] != '\0')
        spec_min_p_ = (float)std::atof(mp);
    }
  }

  // ---- hot-expert VRAM residency (reuse the GLM scheme): spend the free VRAM (less a headroom) on
  // resident expert slots; the rest stream from the mapped host. Cold start; generate() rebalances.
  {
    std::size_t vfree = 0, vtotal = 0;
    cudaMemGetInfo(&vfree, &vtotal);
    const std::size_t headroom = (std::size_t)Calibration::profile().residency_headroom_mib << 20;  // calibratable
    const std::int64_t budget = vfree > headroom ? (std::int64_t)(vfree - headroom) : 0;
    // Optional warm start (STRATA_QWEN_EXPERT_PROFILE): an STRP expert-hit profile -- upstream's own
    // data/expert-profile.bin reads as is (48 x 512 pairs) -- fills the slots hottest-first instead of with
    // experts 0..quota-1, so the first tokens of a run do not wait for the adaptive tier to converge.
    std::vector<std::pair<std::int32_t, std::int32_t>> ranked;
    if (const char* prof = std::getenv("STRATA_QWEN_EXPERT_PROFILE"); prof != nullptr && prof[0] != '\0') {
      std::int64_t pslots = 0;
      std::string perr;
      if (read_expert_profile(prof, g_.n_layers, g_.n_expert, ranked, pslots, perr)) {
        std::fprintf(stderr, "qwen4exp: warm-start expert profile '%s' (%zu ranked pairs)\n", prof, ranked.size());
      } else {
        std::fprintf(stderr, "qwen4exp: WARN expert profile '%s' ignored: %s (cold start)\n", prof, perr.c_str());
        ranked.clear();
      }
    }
    if (!experts_->reg_finalize(budget, ranked, Calibration::profile().warm_seed,
                                Calibration::profile().stream_prefetch, err))
      return false;
    experts_->reg_set_cpu_lane(Calibration::profile().cpu_expert_frac, Calibration::profile().cpu_workers);
    std::fprintf(stderr,
                 "qwen4exp: expert residency %lld VRAM slots (%lld/layer of %d), budget %.2f GiB "
                 "(free %.2f / total %.2f GiB); the rest stream\n",
                 (long long)experts_->reg_slots(), (long long)(experts_->reg_slots() / std::max(1, g_.n_layers)),
                 g_.n_expert, (double)budget / (1024.0 * 1024.0 * 1024.0),
                 (double)vfree / (1024.0 * 1024.0 * 1024.0), (double)vtotal / (1024.0 * 1024.0 * 1024.0));
    std::fflush(stderr);
  }

  // ---- device-decided routed MoE (STRATA_QWEN_MOE_DEVICE=0 keeps the host-planned run_moe path): the
  // router's ids stay on the device, the k experts run as grouped launches choosing resident-slot vs
  // pinned-host weights on the device -- no per-layer id copy + stream sync, ~7 launches a layer.
  QWEN_CU(cudaMalloc(reinterpret_cast<void**>(&d_ids_all_), (std::size_t)g_.n_layers * g_.n_exp_used * sizeof(int)),
          err);
  QWEN_CU(cudaHostAlloc(reinterpret_cast<void**>(&h_ids_all_),
                        (std::size_t)g_.n_layers * g_.n_exp_used * sizeof(std::int32_t), cudaHostAllocDefault),
          err);
  moe_dev_.assign((std::size_t)g_.n_layers, 0);
  if (const char* md = std::getenv("STRATA_QWEN_MOE_DEVICE"); md == nullptr || md[0] != '0') {
    if (!experts_->reg_publish_device_residency(err)) return false;
    int n_dev = 0;
    for (int il = 0; il < g_.n_layers; ++il) {
      moe_dev_[(std::size_t)il] = experts_->reg_device_ready(il) ? 1 : 0;
      n_dev += moe_dev_[(std::size_t)il];
    }
    moe_dev_any_ = n_dev > 0;
    std::fprintf(stderr, "qwen4exp: device-decided MoE on %d of %d layers\n", n_dev, g_.n_layers);
    // A missed expert is read zero-copy inside the grouped launches; STRATA_QWEN_STAGE_MISSES=1 stages it into
    // VRAM first with one wide copy kernel (upstream's PCIe share, its default pcie_mode 2). Token-exact
    // either way; on the RTX 4080 SUPER box staging measured 1.4% slower (45.8 vs 46.4 tok/s), so it is opt-in.
    const char* sm = std::getenv("STRATA_QWEN_STAGE_MISSES");
    const bool stage = sm != nullptr && sm[0] == '1';
    experts_->reg_set_device_miss_path(stage);
    // STRATA_ROUTE_TAIL_SKIP, upstream's decode default (DETAILS.md "Missed experts in a verify window"): a
    // missed expert routed at rank 7 or lower is neither copied nor computed (its contribution is zero, the
    // others keep their weights). On unless 90% or more of the experts are cached; =0 off (exact), =1..9
    // another rank. Decode only: generate() sets it per phase.
    const double cached = experts_->reg_cached_fraction();
    tail_rank_ = cached < 0.90 ? 7 : 0;
    if (const char* ts = std::getenv("STRATA_ROUTE_TAIL_SKIP"); ts != nullptr && ts[0] != '\0') {
      const int r = std::atoi(ts);
      tail_rank_ = r > 0 && r < 10 ? r : 0;
    }
    std::fprintf(stderr,
                 "qwen4exp: misses %s; STRATA_ROUTE_TAIL_SKIP=%d (%s; %.1f%% of the experts cached): a missed "
                 "decode expert routed at rank %d or lower is %s\n",
                 stage ? "staged into VRAM by a copy kernel" : "read zero-copy over PCIe",
                 tail_rank_, tail_rank_ > 0 ? "ON" : "off", 100.0 * cached, tail_rank_ > 0 ? tail_rank_ : 7,
                 tail_rank_ > 0 ? "skipped" : "computed (exact)");
    // ---- the CPU expert lane (STRATA_QWEN_CPU_LANE=1): a layer's missed decode experts computed on CPU
    // cores from the pinned arena in parallel with the GPU's resident ones (upstream ExpertPool +
    // verify.cpp doorbell). Zero-copy misses only (not staging). STRATA_QWEN_CPU_PCIE=frac keeps that
    // fraction of the misses on the GPU's zero-copy path (default 0: every miss to the CPU);
    // STRATA_QWEN_CPU_WORKERS pool threads (<= 0: one per physical core but the caller's).
    if (const char* cl = std::getenv("STRATA_QWEN_CPU_LANE"); cl != nullptr && cl[0] == '1' && !stage) {
      int workers = 0;
      if (const char* w = std::getenv("STRATA_QWEN_CPU_WORKERS"); w != nullptr) workers = std::atoi(w);
      int pcie_num = 0;
      if (const char* pf = std::getenv("STRATA_QWEN_CPU_PCIE"); pf != nullptr)
        pcie_num = (int)(std::atof(pf) * 256.0 + 0.5);
      std::string lerr;
      cpu_lane_on_ = experts_->reg_lane_enable(kWinMax, g_.n_exp_used, workers, pcie_num, lerr);
      if (!cpu_lane_on_) std::fprintf(stderr, "qwen4exp: CPU expert lane off (%s)\n", lerr.c_str());
    }
  }

  // ---- the per-token inputs of the captured decode step (pinned host -> device inside the graph).
  QWEN_CU(cudaMalloc(reinterpret_cast<void**>(&d_tok_in_), sizeof(int)), err);
  QWEN_CU(cudaHostAlloc(reinterpret_cast<void**>(&h_tok_in_), sizeof(int), cudaHostAllocDefault), err);
  QWEN_CU(cudaHostAlloc(reinterpret_cast<void**>(&h_pos_), (std::size_t)pos_rows_ * sizeof(int), cudaHostAllocDefault),
          err);
  QWEN_CU(cudaHostAlloc(reinterpret_cast<void**>(&h_ple_emb_), (std::size_t)g_.n_embd * sizeof(float),
                        cudaHostAllocDefault),
          err);
  {
    // A replayed graph needs every launch argument fixed: the whole MoE on the device path, the IQ4_XS embed
    // (device row), and no host-side debug/profile hooks inside the step.
    bool all_dev = moe_dev_any_;
    for (int il = 0; il < g_.n_layers; ++il) all_dev = all_dev && moe_dev_[(std::size_t)il] != 0;
    const WSlot& emb = W("token_embd.weight");
    const bool iq_embed = emb.type == (int)k::Quant::iq4_xs && emb.q != nullptr;
    auto env_set = [](const char* key) { const char* v = std::getenv(key); return v != nullptr && v[0] != '\0'; };
    const bool debug = env_set("STRATA_QWEN_TAP_FILE") || env_set("STRATA_QWEN_ROUTE_FILE") || env_set("STRATA_QWEN_PROF");
    const char* gv = std::getenv("STRATA_QWEN_GRAPH");
    graph_enabled_ = (gv == nullptr || gv[0] != '0') && all_dev && iq_embed && !debug;
    std::fprintf(stderr, "qwen4exp: CUDA-graph decode %s\n",
                 graph_enabled_ ? "on (captured after the first step)"
                                : (!all_dev ? "off (not every layer is on the device MoE path)"
                                            : !iq_embed ? "off (token_embd is not IQ4_XS)"
                                                        : debug ? "off (a debug/profile env is set)" : "off (STRATA_QWEN_GRAPH=0)"));
    // The adaptive tier without blocking the decode (every layer reads residency on the device):
    // reg_rebalance_async; STRATA_QWEN_ADAPT_ASYNC=0 keeps the blocking reg_rebalance at the calibration
    // profile's cadence. Async defaults to upstream's tuned --adapt-every 1 --adapt-swaps 160 (RTX 4080 SUPER,
    // 256-token greedy: 51.7 tok/s against 50.0 blocking every 8, hit 93.0%); STRATA_QWEN_ADAPT_EVERY / _SWAPS
    // override (<= 0 swaps: no cap).
    const char* aa = std::getenv("STRATA_QWEN_ADAPT_ASYNC");
    adapt_async_ = all_dev && (aa == nullptr || aa[0] != '0');
    adapt_every_ = adapt_async_ ? 1 : Calibration::profile().rebalance_every;
    adapt_swaps_ = adapt_async_ ? 160 : 0;
    if (const char* ae = std::getenv("STRATA_QWEN_ADAPT_EVERY"); ae != nullptr && std::atoi(ae) > 0)
      adapt_every_ = std::atoi(ae);
    if (const char* as = std::getenv("STRATA_QWEN_ADAPT_SWAPS"); as != nullptr) adapt_swaps_ = std::atoi(as);
    std::fprintf(stderr, "qwen4exp: adaptive tier %s, every %d tokens, swap cap %d%s\n",
                 adapt_async_ ? "async (copies beside the decode)" : "blocking", adapt_every_, adapt_swaps_,
                 adapt_swaps_ <= 0 ? " (none)" : "");
  }

  // ---- speculative decode (STRATA_QWEN_SPEC_T = 2..8): the verify window's buffers. Its drafts come from
  // STRATA_QWEN_DRAFT_FILE (a reference stream; the S2 exactness check) until the MTP drafter lands (S3).
  if (const char* st = std::getenv("STRATA_QWEN_SPEC_T"); st != nullptr && std::atoi(st) >= 2) {
    bool all_dev = moe_dev_any_;
    for (int il = 0; il < g_.n_layers; ++il) all_dev = all_dev && moe_dev_[(std::size_t)il] != 0;
    const WSlot& emb = W("token_embd.weight");
    if (!all_dev || emb.type != (int)k::Quant::iq4_xs || emb.q == nullptr || g_.ssm_state != 128) {
      std::fprintf(stderr, "qwen4exp: speculative decode off (needs the device MoE on every layer, an IQ4_XS "
                           "token_embd and GDN state 128)\n");
    } else {
      spec_t_ = std::min(std::atoi(st), kWinMax);
      const std::int64_t W8 = kWinMax;  // (hc_dim, conv_ch, key_dim, value_dim: the arena sizing's, above)
      QWEN_CU(cudaMalloc(reinterpret_cast<void**>(&wa_), (std::size_t)W8 * sc_bytes_), err);
      QWEN_CU(cudaMalloc(reinterpret_cast<void**>(&rw_), (std::size_t)(W8 * hc_dim) * sizeof(float)), err);
      std::int64_t rf = 0;
      for (std::vector<std::int64_t>* v : {&rp_qkv_, &rp_q_, &rp_k_, &rp_v_, &rp_gt_, &rp_be_})
        v->assign((std::size_t)g_.n_layers, -1);
      for (int il = 0; il < g_.n_layers; ++il) {
        if (g_.is_qsa(il)) continue;
        rp_qkv_[(std::size_t)il] = rf; rf += W8 * conv_ch;
        rp_q_[(std::size_t)il] = rf; rf += W8 * key_dim;
        rp_k_[(std::size_t)il] = rf; rf += W8 * key_dim;
        rp_v_[(std::size_t)il] = rf; rf += W8 * value_dim;
        rp_gt_[(std::size_t)il] = rf; rf += W8 * g_.ssm_v_heads;
        rp_be_[(std::size_t)il] = rf; rf += W8 * g_.ssm_v_heads;
      }
      QWEN_CU(cudaMalloc(reinterpret_cast<void**>(&rp_), (std::size_t)rf * sizeof(float)), err);
      if (ple_layer_ >= 0) {
        QWEN_CU(cudaMalloc(reinterpret_cast<void**>(&ple_ng_win_), (std::size_t)(W8 * hc_dim) * sizeof(float)), err);
        QWEN_CU(cudaMalloc(reinterpret_cast<void**>(&ple_hist_win_),
                           (std::size_t)((ple_kern_ - 1) * ple_ngram_ * hc_dim) * sizeof(float)),
                err);
      }
      QWEN_CU(cudaMalloc(&q8_win_, k::q8_1_scratch_bytes((int)((max_n_in_ + 31) / 32 * 32), kWinMax)), err);
      const std::size_t ids_n = (std::size_t)g_.n_layers * (std::size_t)kWinMax * (std::size_t)g_.n_exp_used;
      QWEN_CU(cudaMalloc(reinterpret_cast<void**>(&d_tok_win_), kWinMax * sizeof(int)), err);
      QWEN_CU(cudaMalloc(reinterpret_cast<void**>(&d_pos_win_), kWinMax * sizeof(int)), err);
      QWEN_CU(cudaMalloc(reinterpret_cast<void**>(&d_pos_rows_win_), (std::size_t)kWinMax * pos_rows_ * sizeof(int)),
              err);
      QWEN_CU(cudaHostAlloc(reinterpret_cast<void**>(&h_pos_rows_win_), (std::size_t)kWinMax * pos_rows_ * sizeof(int),
                            cudaHostAllocDefault),
              err);
      QWEN_CU(cudaMalloc(reinterpret_cast<void**>(&d_ids_win_), ids_n * sizeof(int)), err);
      QWEN_CU(cudaMalloc(reinterpret_cast<void**>(&d_out_win_), kWinMax * sizeof(int)), err);
      QWEN_CU(cudaHostAlloc(reinterpret_cast<void**>(&h_tok_win_), kWinMax * sizeof(int), cudaHostAllocDefault), err);
      QWEN_CU(cudaHostAlloc(reinterpret_cast<void**>(&h_pos_win_), kWinMax * sizeof(int), cudaHostAllocDefault), err);
      QWEN_CU(cudaHostAlloc(reinterpret_cast<void**>(&h_ple_win_), (std::size_t)(W8 * g_.n_embd) * sizeof(float),
                            cudaHostAllocDefault),
              err);
      QWEN_CU(cudaHostAlloc(reinterpret_cast<void**>(&h_ids_win_), ids_n * sizeof(std::int32_t), cudaHostAllocDefault),
              err);
      QWEN_CU(cudaHostAlloc(reinterpret_cast<void**>(&h_out_win_), kWinMax * sizeof(int), cudaHostAllocDefault), err);
      win_exec_.assign(kWinMax + 1, nullptr);
      commit_exec_.assign(kWinMax + 1, nullptr);
      win_seen_.assign(kWinMax + 1, 0);
      commit_seen_.assign(kWinMax + 1, 0);
      if (const char* df = std::getenv("STRATA_QWEN_DRAFT_FILE"); df != nullptr && df[0] != '\0') {
        std::FILE* f = std::fopen(df, "r");
        long long v = 0;
        while (f != nullptr && std::fscanf(f, "%lld", &v) == 1) spec_drafts_.push_back((std::int32_t)v);
        if (f) std::fclose(f);
      }
      if (const char* dc = std::getenv("STRATA_QWEN_DRAFT_CORRUPT"); dc != nullptr) spec_corrupt_ = std::atoi(dc);
      std::fprintf(stderr,
                   "qwen4exp: speculative decode, verify windows of up to %d tokens (%.1f MiB of window "
                   "buffers); drafts from %s%s\n",
                   spec_t_,
                   (double)(W8 * (std::int64_t)sc_bytes_ + (W8 * hc_dim + rf) * 4) / (1024.0 * 1024.0),
                   mtp_on_ ? "the MTP draft layer"
                           : spec_drafts_.empty() ? "nowhere (every draft is rejected)" : "STRATA_QWEN_DRAFT_FILE",
                   mtp_on_ ? (", the chain continues while a draft's probability is >= " +
                              std::to_string(spec_min_p_)).c_str()
                           : spec_corrupt_ > 0 ? ", every K-th window's last draft corrupted" : "");
    }
  }

  mtp_on_ = mtp_on_ && spec_t_ >= 2;
  loaded_ = true;
  reset();
  return true;
}

void Qwen4ExpGraph::reset() {
  pos_ = 0;
  if (state_) cudaMemset(state_, 0, state_bytes_);
  ple_tokens_.clear();  // the PLE n-gram window restarts with the sequence
}

// ================================ step: one token through all layers ================================
//
// The HC-wrapped trunk (upstream qwen4exp.cpp, decode T=1): embed -> hc_init (4 identical streams) ->
// 48 layers {hc_read(attn)+GDN|QSA+hc_write, hc_read(ffn)+MoE+hc_write} -> hc_collapse(=result_norm) ->
// head. HC combine = per-stream RMS*gamma -> low-rank down/silu(/hc)/up -> xn*sigmoid(gate) -> mean;
// inject = +block_out*2sigmoid(inject_raw/hc). PLE (1 layer) + the QSA sparse indexer are DEFERRED:
// QSA attends densely over the KV cache (the exact superset of the top-k selection).
bool Qwen4ExpGraph::device_step_(std::int32_t token, std::string& err) {
  const Qwen4ExpGeometry& g = g_;
  const std::int64_t p = pos_;
  const float eps = g.rms_eps;
  float* const S = state_;
  float* const A = sc_;
  float* const R = S;  // the hc residual streams live at the state arena's head (hc, n_embd)

  const std::int64_t hc_dim = (std::int64_t)g.hc * g.n_embd;
  const std::int64_t value_dim = g.ssm_value_dim;
  const std::int64_t conv_ch = g.ssm_conv_channels;
  const std::int64_t qdim = (std::int64_t)g.n_head * g.head_dim;
  const std::int64_t kvdim = (std::int64_t)g.n_head_kv * g.head_dim;
  const std::int64_t ff_sh = (std::int64_t)g.n_ff_exp * g.n_shared;
  qwen4exp::HcShapes hcs{g.n_embd, g.hc, g.hc_lr};
  qwen4exp::GdnShapes gds{g.ssm_state, g.ssm_k_heads, g.ssm_v_heads, g.ssm_d_conv, g.ssm_conv_channels,
                         g.ssm_value_dim};
  qwen4exp::QsaShapes qss{g.head_dim, g.n_head, g.n_head_kv, g.n_rot, g.idx_q_heads, g.idx_key_dim,
                         g.idx_block, g.idx_top_k};

  // ---- debug tap (STRATA_QWEN_TAP_FILE): dump per-layer HC residual streams at one position for
  // parity vs the upstream oracle's STRATA_TAP_FILE. Record = {int32 tag,pos,n_embd,hc}{int32 nfloats}{nfloats f32}.
  // tag: -1 embed row, 0..n_layers-1 the layer's post-FFN-inject residual (all hc streams), 1000 final collapsed hidden.
  static std::FILE* s_tap = [] {
    const char* q = std::getenv("STRATA_QWEN_TAP_FILE");
    return (q != nullptr && q[0] != '\0') ? std::fopen(q, "wb") : (std::FILE*)nullptr;
  }();
  static const int s_tap_pos = [] {
    const char* q = std::getenv("STRATA_QWEN_TAP_POS");
    return (q != nullptr && q[0] != '\0') ? std::atoi(q) : 4;
  }();
  static const std::vector<int> s_tap_layers = [] {
    std::vector<int> v;
    const char* q = std::getenv("STRATA_QWEN_TAP_LAYERS");
    const std::string s = (q != nullptr && q[0] != '\0') ? q : "0,1,2,5,15,25,35,45";
    for (std::size_t i = 0; i < s.size();) {
      const std::size_t j = s.find(',', i);
      v.push_back(std::atoi(s.substr(i, j == std::string::npos ? std::string::npos : j - i).c_str()));
      if (j == std::string::npos) break;
      i = j + 1;
    }
    return v;
  }();
  static std::FILE* s_route = [] {
    const char* q = std::getenv("STRATA_QWEN_ROUTE_FILE");
    return (q != nullptr && q[0] != '\0') ? std::fopen(q, "wb") : (std::FILE*)nullptr;
  }();
  const bool tap_now = s_tap != nullptr && (int)p == s_tap_pos;
  const bool route_now = s_route != nullptr && (int)p == s_tap_pos;
  static std::vector<float> s_tap_host;
  auto tap_dump = [&](int tag, const float* dev, std::int64_t nf) {
    if (s_tap == nullptr) return;
    if ((std::int64_t)s_tap_host.size() < nf) s_tap_host.resize((std::size_t)nf);
    cudaMemcpy(s_tap_host.data(), dev, (std::size_t)nf * sizeof(float), cudaMemcpyDeviceToHost);
    const std::int32_t hdr[4] = {(std::int32_t)tag, (std::int32_t)p, (std::int32_t)g.n_embd, (std::int32_t)g.hc};
    const std::int32_t nf32 = (std::int32_t)nf;
    std::fwrite(hdr, sizeof hdr, 1, s_tap);
    std::fwrite(&nf32, sizeof nf32, 1, s_tap);
    std::fwrite(s_tap_host.data(), sizeof(float), (std::size_t)nf, s_tap);
    std::fflush(s_tap);
  };
  auto tap_in = [&](int l) {
    return tap_now && std::find(s_tap_layers.begin(), s_tap_layers.end(), l) != s_tap_layers.end();
  };
  auto psync = []() { cudaDeviceSynchronize(); return std::chrono::steady_clock::now(); };

  // ---- embed (token_embd is IQ4_XS) -> 4 identical hc streams.
  {
    const WSlot& emb = W("token_embd.weight");
    if (emb.type == (int)k::Quant::iq4_xs && emb.q != nullptr) {
      const std::size_t rb = k::weight_bytes(k::Quant::iq4_xs, g.n_embd, 1);
      // the token id comes from the pinned scalar step() wrote: fixed launch arguments (graph-capturable)
      QWEN_CU(cudaMemcpyAsync(d_tok_in_, h_tok_in_, sizeof(int), cudaMemcpyHostToDevice, (cudaStream_t)cs_), err);
      k::dequant_iq4xs_row(emb.q, rb, d_tok_in_, g.n_embd, A + sc_emb, cs_);
    } else if (emb.type == 0 && emb.f32 != nullptr) {
      QWEN_CU(cudaMemcpyAsync(A + sc_emb, emb.f32 + (std::int64_t)token * g.n_embd,
                         (std::size_t)g.n_embd * sizeof(float), cudaMemcpyDeviceToDevice, (cudaStream_t)cs_), err);
    } else {
      err = "qwen4exp: token_embd.weight is neither IQ4_XS nor F32";
      return false;
    }
    for (int c = 0; c < g.hc; ++c)
      QWEN_CU(cudaMemcpyAsync(R + (std::int64_t)c * g.n_embd, A + sc_emb, (std::size_t)g.n_embd * sizeof(float),
                         cudaMemcpyDeviceToDevice, (cudaStream_t)cs_), err);
  }
  if (tap_now) tap_dump(-1, A + sc_emb, g.n_embd);  // embed row

  QWEN_CU(cudaMemcpyAsync(d_pos_, h_pos_, (std::size_t)pos_rows_ * sizeof(int), cudaMemcpyHostToDevice,
                          (cudaStream_t)cs_),
          err);  // pinned (step() wrote it)

  for (int il = 0; il < g.n_layers; ++il) {
    if (il == ple_layer_) {
      // ---- PLE: n-gram hash of the recent tokens -> ple_n_heads table rows -> a gated per-layer
      // embedding added into the wide residual R (upstream build_ple; applied before this layer's attn).
      // (the n-gram hash + row gather ran on the host in ple_prepare(); h_ple_emb_ holds the rows)
      QWEN_CU(cudaMemcpyAsync(A + sc_ple_emb, h_ple_emb_, (std::size_t)g.n_embd * sizeof(float),
                              cudaMemcpyHostToDevice, (cudaStream_t)cs_), err);  // pinned
      gemv_(W(il, "ple_key.weight"), A + sc_ple_emb, A + sc_ple_key, hc_dim, g.n_embd, cs_);
      gemv_(W(il, "ple_value.weight"), A + sc_ple_emb, A + sc_ple_val, g.n_embd, g.n_embd, cs_);
      qwen4exp::hc_rms_gamma(A + sc_ple_key, W(il, "ple_norm_key.weight").f32, hcs, eps, A + sc_ple_kn, cs_);
      qwen4exp::hc_rms_gamma(R, W(il, "ple_norm_query.weight").f32, hcs, eps, A + sc_ple_qn, cs_);
      qwen4exp::ple_gate(A + sc_ple_kn, A + sc_ple_qn, g.n_embd, g.hc, A + sc_ple_gate, cs_);
      qwen4exp::ple_gated_value(A + sc_ple_val, A + sc_ple_gate, g.n_embd, g.hc, A + sc_ple_gated, cs_);
      qwen4exp::hc_rms_gamma(A + sc_ple_gated, W(il, "ple_norm_conv.weight").f32, hcs, eps, A + sc_ple_ng, cs_);
      qwen4exp::ple_conv_silu(A + sc_ple_ng, W(il, "ple_conv1d.weight").q, state_ + ple_hist_, g.n_embd,
                              g.hc, ple_kern_, ple_ngram_, A + sc_ple_conv, cs_);
      qwen4exp::ple_add2(R, A + sc_ple_gated, A + sc_ple_conv, (int)hc_dim, cs_);
    }
    auto Wl = [&](const char* suf) -> const WSlot& { return W(il, suf); };
    // ---- attention sub-block: HC read -> GDN|QSA -> HC write
    auto read = [&](const char* pfx) {
      qwen4exp::hc_rms_gamma(R, Wl((std::string(pfx) + "_norm.weight").c_str()).f32, hcs, eps, A + sc_xn, cs_);
      gemv_(Wl((std::string(pfx) + "_down.weight").c_str()), A + sc_xn, A + sc_lo, g.hc_lr, hc_dim, cs_);
      k::scale_inplace(A + sc_lo, g.hc_lr, 1.0f / g.hc, cs_);
      k::silu_inplace(A + sc_lo, g.hc_lr, cs_);
      gemv_(Wl((std::string(pfx) + "_up.weight").c_str()), A + sc_lo, A + sc_hg, hc_dim, g.hc_lr, cs_);
      qwen4exp::hc_gated_mean(A + sc_xn, A + sc_hg, hcs, A + sc_bin, cs_);
      gemv_(Wl((std::string(pfx) + "_inject.weight").c_str()), A + sc_xn, A + sc_inj, g.hc, hc_dim, cs_);
    };

    std::chrono::steady_clock::time_point _pt_attn;
    if (g_prof) _pt_attn = psync();
    read("hc_attn");
    if (tap_in(il)) tap_dump(3000 + il, A + sc_bin, g.n_embd);  // HC-read(attn) output = attn block input
    if (!g.is_qsa(il)) {
      // ---- GDN mixer (block input A+sc_bin -> A+sc_mixer)
      gemv_(Wl("attn_qkv.weight"), A + sc_bin, A + sc_qkv, conv_ch, g.n_embd, cs_);
      gemv_(Wl("attn_gate.weight"), A + sc_bin, A + sc_z, value_dim, g.n_embd, cs_);
      gemv_(Wl("ssm_alpha.weight"), A + sc_bin, A + sc_al, g.ssm_v_heads, g.n_embd, cs_);
      gemv_(Wl("ssm_beta.weight"), A + sc_bin, A + sc_be, g.ssm_v_heads, g.n_embd, cs_);
      qwen4exp::gdn_conv_silu(A + sc_qkv, Wl("ssm_conv1d.weight").f32, S + gdn_conv_[il], gds, 1, A + sc_cv, cs_);
      qwen4exp::gdn_split_l2norm(A + sc_cv, gds, 1, 1e-6f, A + sc_q, A + sc_k, A + sc_v, cs_);
      qwen4exp::gdn_gates(A + sc_al, A + sc_be, Wl("ssm_dt.bias").f32, Wl("ssm_a").f32, gds, 1, A + sc_gt,
                          A + sc_be, cs_);
      qwen4exp::gdn_recurrence(S + gdn_S_[il], A + sc_q, A + sc_k, A + sc_v, A + sc_gt, A + sc_be, gds, 1,
                               A + sc_o, cs_);
      qwen4exp::gdn_out_gate(A + sc_o, Wl("ssm_norm.weight").f32, A + sc_z, gds, 1, eps, A + sc_y, cs_);
      gemv_(Wl("ssm_out.weight"), A + sc_y, A + sc_mixer, g.n_embd, value_dim, cs_);
    } else {
      // ---- QSA mixer (dense attention; indexer deferred)
      gemv_(Wl("attn_q.weight"), A + sc_bin, A + sc_qg, 2 * qdim, g.n_embd, cs_);
      // deinterleave per head: qg head h = [q(head_dim) | gate(head_dim)] at stride 2*head_dim
      QWEN_CU(cudaMemcpy2DAsync(A + sc_qq, (std::size_t)g.head_dim * sizeof(float), A + sc_qg,
                           (std::size_t)2 * g.head_dim * sizeof(float), (std::size_t)g.head_dim * sizeof(float),
                           (std::size_t)g.n_head, cudaMemcpyDeviceToDevice, (cudaStream_t)cs_), err);
      QWEN_CU(cudaMemcpy2DAsync(A + sc_qg2, (std::size_t)g.head_dim * sizeof(float), A + sc_qg + g.head_dim,
                           (std::size_t)2 * g.head_dim * sizeof(float), (std::size_t)g.head_dim * sizeof(float),
                           (std::size_t)g.n_head, cudaMemcpyDeviceToDevice, (cudaStream_t)cs_), err);
      gemv_(Wl("attn_k.weight"), A + sc_bin, A + sc_qk, kvdim, g.n_embd, cs_);
      gemv_(Wl("attn_v.weight"), A + sc_bin, A + sc_qv, kvdim, g.n_embd, cs_);
      k::rms_norm(A + sc_qq, Wl("attn_q_norm.weight").f32, g.n_head, g.head_dim, eps, cs_);
      k::rms_norm(A + sc_qk, Wl("attn_k_norm.weight").f32, g.n_head_kv, g.head_dim, eps, cs_);
      k::rope_neox_base(A + sc_qq, A + sc_qq, g.n_head, g.head_dim, g.n_rot, (float)g.rope_freq_base, d_pos_, cs_);
      k::rope_neox_base(A + sc_qk, A + sc_qk, g.n_head_kv, g.head_dim, g.n_rot, (float)g.rope_freq_base, d_pos_, cs_);
      float* kc = S + qsa_k_[il];
      float* vc = S + qsa_v_[il];
      // append at the device position and attend over [0, pos]: no per-token launch argument (capturable)
      qwen4exp::qsa_kv_append(A + sc_qk, A + sc_qv, d_pos_, qss, 1, kc, vc, cs_);
      k::attn_decode_devlen(A + sc_qq, kc, vc, A + sc_at, g.n_head, g.n_head_kv, g.head_dim, d_pos_,
                            1.0f / std::sqrt((float)g.head_dim), cs_);
      qwen4exp::qsa_out_gate(A + sc_at, A + sc_qg2, qss, 1, A + sc_qq, cs_);  // y = o * sigmoid(gate)
      gemv_(Wl("attn_output.weight"), A + sc_qq, A + sc_mixer, g.n_embd, qdim, cs_);
    }
    if (tap_in(il)) tap_dump(4000 + il, A + sc_mixer, g.n_embd);  // attn/GDN mixer output (pre-inject)
    qwen4exp::hc_inject(R, A + sc_mixer, A + sc_inj, hcs, cs_);
    if (tap_in(il)) tap_dump(5000 + il, R, hc_dim);  // residual after attn inject (all hc streams)
    if (g_prof) g_prof_attn_ms += std::chrono::duration<double, std::milli>(psync() - _pt_attn).count();

    // ---- MoE sub-block: HC read -> router + experts + shared -> HC write
    read("hc_ffn");
    if (tap_in(il)) tap_dump(2000 + il, A + sc_bin, g.n_embd);  // MoE/router input (vs oracle STRATA_DUMP_MOE_INPUT)
    gemv_(Wl("ffn_gate_inp.weight"), A + sc_bin, A + sc_rlog, g.n_expert, g.n_embd, cs_);
    // routed ids land in this layer's slot of d_ids_all_ (device): the device-decided MoE reads them there,
    // and one D2H at the end of the token brings every layer's ids to the host for the routing heat.
    int* const ids_l = d_ids_all_ + (std::int64_t)il * g.n_exp_used;
    k::router_topk(A + sc_rlog, 1, g.n_expert, g.n_exp_used, ids_l, A + sc_rw, cs_);
    if (route_now) {
      static std::vector<int> s_rid;
      s_rid.resize((std::size_t)g.n_exp_used);
      cudaMemcpy(s_rid.data(), ids_l, (std::size_t)g.n_exp_used * sizeof(int), cudaMemcpyDeviceToHost);
      const std::int32_t rh[2] = {(std::int32_t)il, (std::int32_t)g.n_exp_used};
      std::fwrite(rh, sizeof rh, 1, s_route);
      std::fwrite(s_rid.data(), sizeof(int), (std::size_t)g.n_exp_used, s_route);
      std::fflush(s_route);
    }
    std::chrono::steady_clock::time_point _pt_moe;
    if (g_prof) _pt_moe = psync();
    if (moe_dev_[(std::size_t)il] != 0) {  // device-decided: grouped launches, no host routing/sync
      if (!experts_->reg_run_token_device(il, ids_l, A + sc_rw, g.n_exp_used, A + sc_bin, A + sc_moe, cs_, err))
        return false;
    } else if (!experts_->run_moe(il, ids_l, A + sc_rw, g.n_exp_used, A + sc_bin, nullptr, A + sc_moe, cs_, err)) {
      return false;
    }
    if (g_prof) g_prof_moe_ms += std::chrono::duration<double, std::milli>(psync() - _pt_moe).count();
    // shared expert (always on), sigmoid-gated by ffn_gate_inp_shexp . x
    gemv_(Wl("ffn_gate_shexp.weight"), A + sc_bin, A + sc_sg, ff_sh, g.n_embd, cs_);
    gemv_(Wl("ffn_up_shexp.weight"), A + sc_bin, A + sc_su, ff_sh, g.n_embd, cs_);
    k::swiglu(A + sc_sg, A + sc_su, A + sc_sh, ff_sh, cs_);
    gemv_(Wl("ffn_down_shexp.weight"), A + sc_sh, A + sc_shd, g.n_embd, ff_sh, cs_);
    gemv_(Wl("ffn_gate_inp_shexp.weight"), A + sc_bin, A + sc_sg, 1, g.n_embd, cs_);  // scalar logit (device)
    // sc_ffn = moe + sigmoid(logit)*shd, gate applied on the device (no per-layer D2H stall).
    qwen4exp::shexp_combine(A + sc_moe, A + sc_shd, A + sc_sg, g.n_embd, A + sc_ffn, cs_);
    qwen4exp::hc_inject(R, A + sc_ffn, A + sc_inj, hcs, cs_);
    if (tap_in(il)) tap_dump(il, R, hc_dim);  // layer's post-FFN-inject residual (all hc streams)
  }

  // ---- final HC collapse (IS the output norm) -> head
  qwen4exp::hc_rms_gamma(R, W("output_hc_norm.weight").f32, hcs, eps, A + sc_xn, cs_);
  gemv_(W("output_hc_down.weight"), A + sc_xn, A + sc_lo, g.hc_lr, hc_dim, cs_);
  k::scale_inplace(A + sc_lo, g.hc_lr, 1.0f / g.hc, cs_);
  k::silu_inplace(A + sc_lo, g.hc_lr, cs_);
  gemv_(W("output_hc_up.weight"), A + sc_lo, A + sc_hg, hc_dim, g.hc_lr, cs_);
  qwen4exp::hc_gated_mean(A + sc_xn, A + sc_hg, hcs, A + sc_bin, cs_);
  gemv_(W("output.weight"), A + sc_bin, A + sc_logits, g.n_vocab, g.n_embd, cs_);
  if (tap_now) {
    tap_dump(1000, A + sc_bin, g.n_embd);  // final collapsed hidden (pre-head)
    static std::vector<float> s_lg;
    s_lg.resize((std::size_t)g.n_vocab);
    cudaMemcpy(s_lg.data(), A + sc_logits, (std::size_t)g.n_vocab * sizeof(float), cudaMemcpyDeviceToHost);
    int am = 0;
    for (int i = 1; i < g.n_vocab; ++i)
      if (s_lg[(std::size_t)i] > s_lg[(std::size_t)am]) am = i;
    std::fprintf(stderr, "qwen4exp TAP: pos %d argmax id=%d val=%.4f (logit[0]=%.4f)\n", (int)p, am,
                 s_lg[(std::size_t)am], s_lg[0]);
  }
  if (moe_dev_any_)  // every layer's routed ids -> host (pinned), read after the next cs_ sync
    QWEN_CU(cudaMemcpyAsync(h_ids_all_, d_ids_all_, (std::size_t)g.n_layers * g.n_exp_used * sizeof(int),
                            cudaMemcpyDeviceToHost, (cudaStream_t)cs_), err);
  (void)token;  // read only by the F32-embed fallback and the debug taps
  return true;
}

// ================================ the verify window (speculative decode, S2) ================================
//
// window_forward_(T): the T tokens h_tok_win_[0..T) at positions h_pos_win_[0..T) through every layer, the
// per-token logits into the window scratch. Mirrors device_step_ op for op: the GEMVs run as one multi-
// column call over the T tokens (each column bitwise the single-column call), every other kernel per token
// (or over T rows of a row-independent kernel), so row t is bitwise the single-token step after token t.
// The GDN conv/recurrence read the states without writing them (their inputs stay in rp_ for the commit),
// the PLE conv runs on a copy of its history, and the K/V of all T positions are appended.
bool Qwen4ExpGraph::window_forward_(int T, std::string& err) {
  const Qwen4ExpGeometry& g = g_;
  const float eps = g.rms_eps;
  float* const S = state_;
  const std::int64_t hc_dim = (std::int64_t)g.hc * g.n_embd;
  const std::int64_t value_dim = g.ssm_value_dim;
  const std::int64_t conv_ch = g.ssm_conv_channels;
  const std::int64_t qdim = (std::int64_t)g.n_head * g.head_dim;
  const std::int64_t kvdim = (std::int64_t)g.n_head_kv * g.head_dim;
  const std::int64_t ff_sh = (std::int64_t)g.n_ff_exp * g.n_shared;
  const std::int64_t k_used = g.n_exp_used;
  qwen4exp::HcShapes hcs{g.n_embd, g.hc, g.hc_lr};
  qwen4exp::GdnShapes gds{g.ssm_state, g.ssm_k_heads, g.ssm_v_heads, g.ssm_d_conv, g.ssm_conv_channels,
                         g.ssm_value_dim};
  qwen4exp::QsaShapes qss{g.head_dim, g.n_head, g.n_head_kv, g.n_rot, g.idx_q_heads, g.idx_key_dim,
                         g.idx_block, g.idx_top_k};
  const auto cs = (cudaStream_t)cs_;
  // window buffer `off` (an sc_ slice) holding vectors of `len` floats: token t's vector
  auto WB = [&](std::int64_t off, std::int64_t len, int t) -> float* {
    return wa_ + (std::int64_t)kWinMax * off + (std::int64_t)t * len;
  };
  auto RW = [&](int t) -> float* { return rw_ + (std::int64_t)t * hc_dim; };

  // ---- inputs (pinned host -> device) and the embed -> hc streams, per token
  QWEN_CU(cudaMemcpyAsync(d_tok_win_, h_tok_win_, (std::size_t)T * sizeof(int), cudaMemcpyHostToDevice, cs), err);
  QWEN_CU(cudaMemcpyAsync(d_pos_win_, h_pos_win_, (std::size_t)T * sizeof(int), cudaMemcpyHostToDevice, cs), err);
  QWEN_CU(cudaMemcpyAsync(d_pos_rows_win_, h_pos_rows_win_, (std::size_t)T * pos_rows_ * sizeof(int),
                          cudaMemcpyHostToDevice, cs),
          err);
  {
    const WSlot& emb = W("token_embd.weight");
    const std::size_t rb = k::weight_bytes(k::Quant::iq4_xs, g.n_embd, 1);
    for (int t = 0; t < T; ++t) {
      k::dequant_iq4xs_row(emb.q, rb, d_tok_win_ + t, g.n_embd, WB(sc_emb, g.n_embd, t), cs_);
      for (int c = 0; c < g.hc; ++c)
        QWEN_CU(cudaMemcpyAsync(RW(t) + (std::int64_t)c * g.n_embd, WB(sc_emb, g.n_embd, t),
                                (std::size_t)g.n_embd * sizeof(float), cudaMemcpyDeviceToDevice, cs),
                err);
    }
  }

  for (int il = 0; il < g.n_layers; ++il) {
    auto Wl = [&](const char* suf) -> const WSlot& { return W(il, suf); };
    if (il == ple_layer_) {
      QWEN_CU(cudaMemcpyAsync(WB(sc_ple_emb, g.n_embd, 0), h_ple_win_, (std::size_t)T * g.n_embd * sizeof(float),
                              cudaMemcpyHostToDevice, cs),
              err);
      gemv_multi_(Wl("ple_key.weight"), WB(sc_ple_emb, g.n_embd, 0), WB(sc_ple_key, hc_dim, 0), hc_dim, g.n_embd, T,
                  cs_);
      gemv_multi_(Wl("ple_value.weight"), WB(sc_ple_emb, g.n_embd, 0), WB(sc_ple_val, g.n_embd, 0), g.n_embd,
                  g.n_embd, T, cs_);
      QWEN_CU(cudaMemcpyAsync(ple_hist_win_, S + ple_hist_,
                              (std::size_t)((ple_kern_ - 1) * ple_ngram_ * hc_dim) * sizeof(float),
                              cudaMemcpyDeviceToDevice, cs),
              err);
      for (int t = 0; t < T; ++t) {
        float* ng = ple_ng_win_ + (std::int64_t)t * hc_dim;
        qwen4exp::hc_rms_gamma(WB(sc_ple_key, hc_dim, t), Wl("ple_norm_key.weight").f32, hcs, eps,
                               WB(sc_ple_kn, hc_dim, t), cs_);
        qwen4exp::hc_rms_gamma(RW(t), Wl("ple_norm_query.weight").f32, hcs, eps, WB(sc_ple_qn, hc_dim, t), cs_);
        qwen4exp::ple_gate(WB(sc_ple_kn, hc_dim, t), WB(sc_ple_qn, hc_dim, t), g.n_embd, g.hc,
                           WB(sc_ple_gate, g.hc, t), cs_);
        qwen4exp::ple_gated_value(WB(sc_ple_val, g.n_embd, t), WB(sc_ple_gate, g.hc, t), g.n_embd, g.hc,
                                  WB(sc_ple_gated, hc_dim, t), cs_);
        qwen4exp::hc_rms_gamma(WB(sc_ple_gated, hc_dim, t), Wl("ple_norm_conv.weight").f32, hcs, eps, ng, cs_);
        qwen4exp::ple_conv_silu(ng, Wl("ple_conv1d.weight").q, ple_hist_win_, g.n_embd, g.hc, ple_kern_, ple_ngram_,
                                WB(sc_ple_conv, hc_dim, t), cs_);
        qwen4exp::ple_add2(RW(t), WB(sc_ple_gated, hc_dim, t), WB(sc_ple_conv, hc_dim, t), (int)hc_dim, cs_);
      }
    }
    // ---- HC read (pfx) for every window token: per-token norms / gated means, multi-column GEMVs
    auto read = [&](const char* pfx) {
      const std::string P(pfx);
      qwen4exp::hc_rms_gamma(RW(0), Wl((P + "_norm.weight").c_str()).f32, hcs, eps, WB(sc_xn, hc_dim, 0), cs_, T);
      gemv_multi_(Wl((P + "_down.weight").c_str()), WB(sc_xn, hc_dim, 0), WB(sc_lo, g.hc_lr, 0), g.hc_lr, hc_dim, T,
                  cs_);
      k::scale_inplace(WB(sc_lo, g.hc_lr, 0), (std::int64_t)T * g.hc_lr, 1.0f / g.hc, cs_);
      k::silu_inplace(WB(sc_lo, g.hc_lr, 0), (std::int64_t)T * g.hc_lr, cs_);
      gemv_multi_(Wl((P + "_up.weight").c_str()), WB(sc_lo, g.hc_lr, 0), WB(sc_hg, hc_dim, 0), hc_dim, g.hc_lr, T,
                  cs_);
      qwen4exp::hc_gated_mean(WB(sc_xn, hc_dim, 0), WB(sc_hg, hc_dim, 0), hcs, WB(sc_bin, g.n_embd, 0), cs_, T);
      gemv_multi_(Wl((P + "_inject.weight").c_str()), WB(sc_xn, hc_dim, 0), WB(sc_inj, g.hc, 0), g.hc, hc_dim, T,
                  cs_);
    };

    read("hc_attn");
    float* const bin = WB(sc_bin, g.n_embd, 0);
    if (!g.is_qsa(il)) {
      float* const rqkv = rp_ + rp_qkv_[(std::size_t)il];
      float* const rq = rp_ + rp_q_[(std::size_t)il];
      float* const rk = rp_ + rp_k_[(std::size_t)il];
      float* const rv = rp_ + rp_v_[(std::size_t)il];
      float* const rgt = rp_ + rp_gt_[(std::size_t)il];
      float* const rbe = rp_ + rp_be_[(std::size_t)il];
      gemv_multi_(Wl("attn_qkv.weight"), bin, rqkv, conv_ch, g.n_embd, T, cs_);
      gemv_multi_(Wl("attn_gate.weight"), bin, WB(sc_z, value_dim, 0), value_dim, g.n_embd, T, cs_);
      gemv_multi_(Wl("ssm_alpha.weight"), bin, WB(sc_al, g.ssm_v_heads, 0), g.ssm_v_heads, g.n_embd, T, cs_);
      gemv_multi_(Wl("ssm_beta.weight"), bin, WB(sc_be, g.ssm_v_heads, 0), g.ssm_v_heads, g.n_embd, T, cs_);
      qwen4exp::gdn_conv_silu(rqkv, Wl("ssm_conv1d.weight").f32, S + gdn_conv_[(std::size_t)il], gds, T,
                              WB(sc_cv, conv_ch, 0), cs_, /*write_state=*/false);
      qwen4exp::gdn_split_l2norm(WB(sc_cv, conv_ch, 0), gds, T, 1e-6f, rq, rk, rv, cs_);
      qwen4exp::gdn_gates(WB(sc_al, g.ssm_v_heads, 0), WB(sc_be, g.ssm_v_heads, 0), Wl("ssm_dt.bias").f32,
                          Wl("ssm_a").f32, gds, T, rgt, rbe, cs_);
      qwen4exp::gdn_recurrence(S + gdn_S_[(std::size_t)il], rq, rk, rv, rgt, rbe, gds, T, WB(sc_o, value_dim, 0),
                               cs_, /*write_state=*/false);
      qwen4exp::gdn_out_gate(WB(sc_o, value_dim, 0), Wl("ssm_norm.weight").f32, WB(sc_z, value_dim, 0), gds, T, eps,
                             WB(sc_y, value_dim, 0), cs_);
      gemv_multi_(Wl("ssm_out.weight"), WB(sc_y, value_dim, 0), WB(sc_mixer, g.n_embd, 0), g.n_embd, value_dim, T,
                  cs_);
    } else {
      gemv_multi_(Wl("attn_q.weight"), bin, WB(sc_qg, 2 * qdim, 0), 2 * qdim, g.n_embd, T, cs_);
      for (int t = 0; t < T; ++t) {  // deinterleave per head: [q(head_dim) | gate(head_dim)] at 2*head_dim
        QWEN_CU(cudaMemcpy2DAsync(WB(sc_qq, qdim, t), (std::size_t)g.head_dim * sizeof(float), WB(sc_qg, 2 * qdim, t),
                                  (std::size_t)2 * g.head_dim * sizeof(float), (std::size_t)g.head_dim * sizeof(float),
                                  (std::size_t)g.n_head, cudaMemcpyDeviceToDevice, cs),
                err);
        QWEN_CU(cudaMemcpy2DAsync(WB(sc_qg2, qdim, t), (std::size_t)g.head_dim * sizeof(float),
                                  WB(sc_qg, 2 * qdim, t) + g.head_dim, (std::size_t)2 * g.head_dim * sizeof(float),
                                  (std::size_t)g.head_dim * sizeof(float), (std::size_t)g.n_head,
                                  cudaMemcpyDeviceToDevice, cs),
                err);
      }
      gemv_multi_(Wl("attn_k.weight"), bin, WB(sc_qk, kvdim, 0), kvdim, g.n_embd, T, cs_);
      gemv_multi_(Wl("attn_v.weight"), bin, WB(sc_qv, kvdim, 0), kvdim, g.n_embd, T, cs_);
      k::rms_norm(WB(sc_qq, qdim, 0), Wl("attn_q_norm.weight").f32, (std::int64_t)T * g.n_head, g.head_dim, eps, cs_);
      k::rms_norm(WB(sc_qk, kvdim, 0), Wl("attn_k_norm.weight").f32, (std::int64_t)T * g.n_head_kv, g.head_dim, eps,
                  cs_);
      for (int t = 0; t < T; ++t) {
        k::rope_neox_base(WB(sc_qq, qdim, t), WB(sc_qq, qdim, t), g.n_head, g.head_dim, g.n_rot,
                          (float)g.rope_freq_base, d_pos_rows_win_ + (std::int64_t)t * pos_rows_, cs_);
        k::rope_neox_base(WB(sc_qk, kvdim, t), WB(sc_qk, kvdim, t), g.n_head_kv, g.head_dim, g.n_rot,
                          (float)g.rope_freq_base, d_pos_rows_win_ + (std::int64_t)t * pos_rows_, cs_);
      }
      float* kc = S + qsa_k_[(std::size_t)il];
      float* vc = S + qsa_v_[(std::size_t)il];
      qwen4exp::qsa_kv_append(WB(sc_qk, kvdim, 0), WB(sc_qv, kvdim, 0), d_pos_win_, qss, T, kc, vc, cs_);
      for (int t = 0; t < T; ++t)  // token t attends [0, pos_t]: the same length the single-token step uses
        k::attn_decode_devlen(WB(sc_qq, qdim, t), kc, vc, WB(sc_at, qdim, t), g.n_head, g.n_head_kv, g.head_dim,
                              d_pos_win_ + t, 1.0f / std::sqrt((float)g.head_dim), cs_);
      qwen4exp::qsa_out_gate(WB(sc_at, qdim, 0), WB(sc_qg2, qdim, 0), qss, T, WB(sc_qq, qdim, 0), cs_);
      gemv_multi_(Wl("attn_output.weight"), WB(sc_qq, qdim, 0), WB(sc_mixer, g.n_embd, 0), g.n_embd, qdim, T, cs_);
    }
    qwen4exp::hc_inject(RW(0), WB(sc_mixer, g.n_embd, 0), WB(sc_inj, g.hc, 0), hcs, cs_, T);

    // ---- MoE: router for the T tokens at once, the routed experts per token, the shared expert multi-column
    read("hc_ffn");
    gemv_multi_(Wl("ffn_gate_inp.weight"), bin, WB(sc_rlog, g.n_expert, 0), g.n_expert, g.n_embd, T, cs_);
    int* const ids_l = d_ids_win_ + (std::int64_t)il * kWinMax * k_used;
    k::router_topk(WB(sc_rlog, g.n_expert, 0), T, g.n_expert, g.n_exp_used, ids_l, WB(sc_rw, k_used, 0), cs_);
    if (!experts_->reg_run_token_device(il, ids_l, WB(sc_rw, k_used, 0), g.n_exp_used, WB(sc_bin, g.n_embd, 0),
                                        WB(sc_moe, g.n_embd, 0), cs_, err, T))
      return false;
    gemv_multi_(Wl("ffn_gate_shexp.weight"), bin, WB(sc_sg, ff_sh, 0), ff_sh, g.n_embd, T, cs_);
    gemv_multi_(Wl("ffn_up_shexp.weight"), bin, WB(sc_su, ff_sh, 0), ff_sh, g.n_embd, T, cs_);
    k::swiglu(WB(sc_sg, ff_sh, 0), WB(sc_su, ff_sh, 0), WB(sc_sh, ff_sh, 0), (std::int64_t)T * ff_sh, cs_);
    gemv_multi_(Wl("ffn_down_shexp.weight"), WB(sc_sh, ff_sh, 0), WB(sc_shd, g.n_embd, 0), g.n_embd, ff_sh, T, cs_);
    gemv_multi_(Wl("ffn_gate_inp_shexp.weight"), bin, WB(sc_sg, 1, 0), 1, g.n_embd, T, cs_);  // scalar logits
    qwen4exp::shexp_combine(WB(sc_moe, g.n_embd, 0), WB(sc_shd, g.n_embd, 0), WB(sc_sg, 1, 0), g.n_embd,
                            WB(sc_ffn, g.n_embd, 0), cs_, T);
    qwen4exp::hc_inject(RW(0), WB(sc_ffn, g.n_embd, 0), WB(sc_inj, g.hc, 0), hcs, cs_, T);
  }

  // ---- final HC collapse -> head, per token rows
  qwen4exp::hc_rms_gamma(RW(0), W("output_hc_norm.weight").f32, hcs, eps, WB(sc_xn, hc_dim, 0), cs_, T);
  gemv_multi_(W("output_hc_down.weight"), WB(sc_xn, hc_dim, 0), WB(sc_lo, g.hc_lr, 0), g.hc_lr, hc_dim, T, cs_);
  k::scale_inplace(WB(sc_lo, g.hc_lr, 0), (std::int64_t)T * g.hc_lr, 1.0f / g.hc, cs_);
  k::silu_inplace(WB(sc_lo, g.hc_lr, 0), (std::int64_t)T * g.hc_lr, cs_);
  gemv_multi_(W("output_hc_up.weight"), WB(sc_lo, g.hc_lr, 0), WB(sc_hg, hc_dim, 0), hc_dim, g.hc_lr, T, cs_);
  qwen4exp::hc_gated_mean(WB(sc_xn, hc_dim, 0), WB(sc_hg, hc_dim, 0), hcs, WB(sc_bin, g.n_embd, 0), cs_, T);
  gemv_multi_(W("output.weight"), WB(sc_bin, g.n_embd, 0), WB(sc_logits, g.n_vocab, 0), g.n_vocab, g.n_embd, T, cs_);
  QWEN_CU(cudaMemcpyAsync(h_ids_win_, d_ids_win_,
                          (std::size_t)g.n_layers * kWinMax * k_used * sizeof(int), cudaMemcpyDeviceToHost, cs),
          err);
  return true;
}

// The commit: the first n_keep window tokens become permanent -- each GDN layer's conv history and recurrent
// state advanced by replaying their stored inputs in place (bitwise the single-token updates), the PLE
// history by replaying their conv inputs.
bool Qwen4ExpGraph::window_commit_dev_(int n_keep, std::string& err) {
  (void)err;
  const Qwen4ExpGeometry& g = g_;
  float* const S = state_;
  const std::int64_t hc_dim = (std::int64_t)g.hc * g.n_embd;
  qwen4exp::GdnShapes gds{g.ssm_state, g.ssm_k_heads, g.ssm_v_heads, g.ssm_d_conv, g.ssm_conv_channels,
                         g.ssm_value_dim};
  float* const junk_cv = wa_ + (std::int64_t)kWinMax * sc_cv;  // replay outputs, discarded
  float* const junk_o = wa_ + (std::int64_t)kWinMax * sc_o;
  for (int il = 0; il < g.n_layers; ++il) {
    if (g.is_qsa(il)) continue;
    qwen4exp::gdn_conv_silu(rp_ + rp_qkv_[(std::size_t)il], W(il, "ssm_conv1d.weight").f32,
                            S + gdn_conv_[(std::size_t)il], gds, n_keep, junk_cv, cs_, /*write_state=*/true);
    qwen4exp::gdn_recurrence(S + gdn_S_[(std::size_t)il], rp_ + rp_q_[(std::size_t)il], rp_ + rp_k_[(std::size_t)il],
                             rp_ + rp_v_[(std::size_t)il], rp_ + rp_gt_[(std::size_t)il],
                             rp_ + rp_be_[(std::size_t)il], gds, n_keep, junk_o, cs_, /*write_state=*/true);
  }
  if (ple_layer_ >= 0)
    for (int t = 0; t < n_keep; ++t)
      qwen4exp::ple_conv_silu(ple_ng_win_ + (std::int64_t)t * hc_dim, W(ple_layer_, "ple_conv1d.weight").q,
                              S + ple_hist_, g.n_embd, g.hc, ple_kern_, ple_ngram_,
                              wa_ + (std::int64_t)kWinMax * sc_ple_conv, cs_);
  return true;
}

// Run window (window = true) or commit graph n: eagerly the first time (its lazy allocations happen), then
// captured once and replayed. Fixed launch arguments per n: the inputs come from pinned host buffers.
bool Qwen4ExpGraph::launch_captured_(std::vector<void*>& exec, std::vector<char>& seen, int n, bool window,
                                     std::string& err) {
  const auto cs = (cudaStream_t)cs_;
  auto body = [&]() { return window ? window_forward_(n, err) : window_commit_dev_(n, err); };
  if (exec[(std::size_t)n] != nullptr) {
    if (cudaGraphLaunch((cudaGraphExec_t)exec[(std::size_t)n], cs) != cudaSuccess) {
      err = std::string("qwen4exp: window graph launch: ") + cudaGetErrorString(cudaGetLastError());
      return false;
    }
    return true;
  }
  if (!graph_enabled_ || !seen[(std::size_t)n]) {
    seen[(std::size_t)n] = 1;
    return body();
  }
  cudaGraph_t gph = nullptr;
  bool ok = cudaStreamBeginCapture(cs, cudaStreamCaptureModeThreadLocal) == cudaSuccess;
  if (ok) ok = body();
  const cudaError_t ce = cudaStreamEndCapture(cs, &gph);
  cudaGraphExec_t ex = nullptr;
  if (ok && ce == cudaSuccess && gph != nullptr && cudaGraphInstantiate(&ex, gph, 0) == cudaSuccess) {
    cudaGraphDestroy(gph);
    exec[(std::size_t)n] = ex;
    return cudaGraphLaunch(ex, cs) == cudaSuccess;
  }
  if (gph != nullptr) cudaGraphDestroy(gph);
  err = std::string("qwen4exp: ") + (window ? "window" : "commit") + " graph capture failed: " +
        (!err.empty() ? err : cudaGetErrorString(ce != cudaSuccess ? ce : cudaGetLastError()));
  return false;
}

// One verify window over tokens[0..T) at pos_..: the inputs, the window, the T greedy picks into h_out_win_.
bool Qwen4ExpGraph::run_window_(int T, const std::int32_t* tokens, std::string& err) {
  if (cudaStreamSynchronize((cudaStream_t)cs_) != cudaSuccess) {
    err = std::string("qwen4exp: window sync: ") + cudaGetErrorString(cudaGetLastError());
    return false;
  }
  for (int t = 0; t < T; ++t) {
    h_tok_win_[t] = tokens[t];
    h_pos_win_[t] = (int)(pos_ + t);
    for (int r = 0; r < pos_rows_; ++r) h_pos_rows_win_[t * pos_rows_ + r] = (int)(pos_ + t);
  }
  if (ple_layer_ >= 0) {  // each token's PLE rows see the window tokens before it
    for (int t = 0; t < T; ++t) {
      ple_prepare_into(tokens[t], pos_ + t, h_ple_win_ + (std::int64_t)t * g_.n_embd);
      ple_tokens_.push_back(tokens[t]);
    }
    ple_tokens_.resize(ple_tokens_.size() - (std::size_t)T);
  }
  if (cpu_lane_on_ && !experts_->reg_lane_arm(lane_decode_, cs_, err)) return false;
  if (!launch_captured_(win_exec_, win_seen_, T, true, err)) return false;
  if (cpu_lane_on_ && !experts_->reg_lane_serve(T, cs_, err)) return false;  // beside the window launch
  k::SamplerParams sp;
  sp.greedy = true;
  k::sample_tokens(wa_ + (std::int64_t)kWinMax * sc_logits, T, g_.n_vocab, nullptr, 0, sp, d_out_win_, cs_);
  if (cudaMemcpyAsync(h_out_win_, d_out_win_, (std::size_t)T * sizeof(int), cudaMemcpyDeviceToHost,
                      (cudaStream_t)cs_) != cudaSuccess ||
      cudaStreamSynchronize((cudaStream_t)cs_) != cudaSuccess) {  // the one real sync a window
    err = std::string("qwen4exp: window picks copy: ") + cudaGetErrorString(cudaGetLastError());
    return false;
  }
  return true;
}

// Keep tokens[0..n_keep) of the last window: the state replay, the positions, the PLE window, routing heat.
bool Qwen4ExpGraph::commit_window_(int n_keep, const std::int32_t* tokens, std::string& err) {
  if (!launch_captured_(commit_exec_, commit_seen_, n_keep, false, err)) return false;
  const int k = g_.n_exp_used;
  for (int t = 0; t < n_keep; ++t) {  // routing heat + hit counters for the kept tokens only
    for (int il = 0; il < g_.n_layers; ++il)
      std::memcpy(h_ids_all_ + (std::int64_t)il * k, h_ids_win_ + ((std::int64_t)il * kWinMax + t) * k,
                  (std::size_t)k * sizeof(std::int32_t));
    experts_->reg_note_device_token(h_ids_all_, k, moe_dev_);
    if (ple_layer_ >= 0) ple_tokens_.push_back(tokens[t]);
  }
  pos_ += n_keep;
  return true;
}

// The decode loop with verify windows. `produced` = 1 (the prefill's pick, already sent). Each window runs
// the last pick plus up to spec_t_-1 drafts; the drafts the window's own picks agree with are kept, plus the
// window's next pick -- so the stream is exactly greedy decode's. Drafts come from spec_drafts_ (a
// reference stream, the S2 check) until the MTP drafter lands.
bool Qwen4ExpGraph::decode_spec_(const Gen& request, const LineSink& sink, std::uint32_t& produced,
                                 std::string& err) {
  std::int32_t last = 0;
  {
    // the prefill's pick is the stream's first token: sample_next left it in d_tok_
    int id = 0;
    if (cudaMemcpy(&id, d_tok_, sizeof(int), cudaMemcpyDeviceToHost) != cudaSuccess) {
      err = "qwen4exp: spec first pick";
      return false;
    }
    last = (std::int32_t)id;
  }
  std::int64_t windows = 0, offered = 0, accepted = 0;
  using SClk = std::chrono::steady_clock;
  double ms_win = 0.0, ms_mtp = 0.0, ms_commit = 0.0;  // host wall time per phase (each ends in a sync)
  auto ms_since = [](SClk::time_point a) { return std::chrono::duration<double, std::milli>(SClk::now() - a).count(); };
  std::int32_t tokens[kWinMax];
  std::int32_t mdrafts[kWinMax];
  int n_md = 0;
  auto budget_drafts = [&]() {  // drafts the next window may hold: the window size and the picks still wanted
    return std::max(0, std::min(spec_t_, (int)(request.max_new - produced)) - 1);
  };
  if (mtp_on_) {  // the first round: the prompt's last cell, its main residual (the state head) and the first pick
    const auto t0 = SClk::now();
    n_md = mtp_chain_(state_, last, pos_ - 1, budget_drafts(), mdrafts, err);
    if (n_md < 0) return false;
    ms_mtp += ms_since(t0);
  }
  while (produced < request.max_new) {
    const int budget = (int)(request.max_new - produced);  // picks still wanted
    int T = std::max(1, std::min(spec_t_, budget));
    tokens[0] = last;
    if (mtp_on_) {
      T = std::min(T, 1 + n_md);
      for (int i = 1; i < T; ++i) tokens[i] = mdrafts[i - 1];
    } else {
      for (int i = 1; i < T; ++i) {  // draft i = the reference stream's token at stream index produced-1+i
        const std::size_t ri = (std::size_t)produced - 1 + (std::size_t)i;
        tokens[i] = ri < spec_drafts_.size() ? spec_drafts_[ri] : 0;
      }
      if (spec_corrupt_ > 0 && T > 1 && windows % spec_corrupt_ == spec_corrupt_ - 1)
        tokens[T - 1] = (tokens[T - 1] + 1) % g_.n_vocab;
    }
    const auto tw = SClk::now();
    if (!run_window_(T, tokens, err)) return false;
    ms_win += ms_since(tw);
    int n_acc = 0;
    while (n_acc + 1 < T && tokens[n_acc + 1] == h_out_win_[n_acc]) ++n_acc;
    ++windows;
    offered += T - 1;
    accepted += n_acc;
    for (int i = 0; i <= n_acc && produced < request.max_new; ++i) {
      sink("T " + std::to_string((std::uint32_t)h_out_win_[i]));
      ++produced;
    }
    last = h_out_win_[n_acc];
    if (mtp_on_ && produced < request.max_new) {
      // the MTP round: K/V for the accepted cells (main residual t + the token after it), then the chain from the
      // last kept cell (its main residual + the window's next pick) -- before the next window rewrites rw_
      const std::int64_t p0 = pos_;
      const std::int64_t hc_dim = (std::int64_t)g_.hc * g_.n_embd;
      const auto tm = SClk::now();
      if (!mtp_kv_rows_(rw_, n_acc, tokens + 1, p0, err)) return false;
      if (!commit_window_(n_acc + 1, tokens, err)) return false;
      n_md = mtp_chain_(rw_ + (std::int64_t)n_acc * hc_dim, last, p0 + n_acc, budget_drafts(), mdrafts, err);
      if (n_md < 0) return false;
      ms_mtp += ms_since(tm);
    } else {
      const auto tc = SClk::now();
      if (!commit_window_(n_acc + 1, tokens, err)) return false;
      ms_commit += ms_since(tc);
    }
    if (adapt_async_) {  // one round a window, its swap cap scaled by the tokens it kept (the plain path's rate)
      const int cap = adapt_swaps_ > 0 ? adapt_swaps_ * (n_acc + 1) : adapt_swaps_;
      if (experts_->reg_rebalance_async(Calibration::profile().rebalance_margin, cap, cs_, err) < 0) return false;
    }
  }
  std::fprintf(stderr, "qwen4exp: speculative decode: %lld windows, drafts accepted %lld of %lld (%.2f tokens a "
                       "window)%s\n",
               (long long)windows, (long long)accepted, (long long)offered,
               windows > 0 ? (double)(accepted + windows) / (double)windows : 0.0,
               mtp_on_ ? (", MTP " + std::to_string(mtp_rounds_) + " rounds / " + std::to_string(mtp_steps_) +
                          " draft steps").c_str()
                       : "");
  std::fprintf(stderr, "qwen4exp: speculative decode wall time: windows %.0f ms (%.1f a window), MTP + commit %.0f ms, "
                       "commit alone %.0f ms\n",
               ms_win, windows > 0 ? ms_win / (double)windows : 0.0, ms_mtp, ms_commit);
  return true;
}

// The PLE n-gram hash of the recent tokens -> ple_n_heads table rows (IQ4_NL, host-mapped), dequantized
// into the pinned h_ple_emb_ that the step's PLE upload reads. Host work, done before the step launches.
void Qwen4ExpGraph::ple_prepare(std::int32_t token) { ple_prepare_into(token, pos_, h_ple_emb_); }

// The same rows for `token` at position p into dst [n_embd]; ple_tokens_ must hold the tokens before p.
// The window / hash / row addressing delegates to the shared host helpers in u_ple.hpp (the SAME code
// the host case ple_ngram_window_and_row_addressing anchors — no drift between what the host verifies
// and what the box runs); only the IQ4_NL row gather from the host-mapped table stays here.
void Qwen4ExpGraph::ple_prepare_into(std::int32_t token, std::int64_t p, float* dst) {
  const int hd = ple_head_dim_, ng = ple_ngram_, pg = ple_per_gram_;
  std::int32_t prev[8];
  const std::size_t have = p > 0 ? (std::size_t) std::min<std::int64_t>((std::int64_t) ng - 1, p) : 0u;
  for (std::size_t j = 0; j < have; ++j) prev[j] = ple_tokens_[(std::size_t) (p - 1 - (std::int64_t) j)];
  std::int64_t ctx[8] = {0};
  u::ple::ngram_ctx(ple_spec_, token, prev, have, ctx);
  for (int nn = 2; nn <= ng; ++nn) {
    const std::uint64_t mixed = u::ple::mixed_n(ple_spec_, ctx, nn);
    const int base = (nn - 2) * pg;
    for (int gg = 0; gg < pg; ++gg) {
      const int h_i = base + gg;
      const std::int64_t row = u::ple::head_row(ple_spec_, mixed, h_i);
      dequant_iq4nl_row(ple_table_ + (std::size_t) row * ple_row_bytes_, hd, dst + (std::size_t) h_i * hd);
    }
  }
}

// One token through all layers at cache position pos_ (T=1). The per-token inputs go to pinned host
// buffers (token, position, the PLE rows); the device work (device_step_) has fixed launch arguments, so
// after one eager warm-up step it is captured ONCE into a CUDA graph and replayed every token: ~2.7k
// kernel launches a token become one graph launch (the host was the bottleneck at ~10 us a launch).
bool Qwen4ExpGraph::step(std::int32_t token, std::string& err) {
  // the previous step must have consumed the pinned inputs before they are rewritten (normally already
  // idle: the sampler and the prefill loop sync the stream)
  if (cudaStreamSynchronize((cudaStream_t)cs_) != cudaSuccess) {
    err = std::string("qwen4exp: step sync: ") + cudaGetErrorString(cudaGetLastError());
    return false;
  }
  if (cpu_lane_on_ && !experts_->reg_lane_arm(lane_decode_, cs_, err)) return false;
  h_tok_in_[0] = token;
  for (int r = 0; r < pos_rows_; ++r) h_pos_[r] = (int)pos_;
  if (ple_layer_ >= 0) ple_prepare(token);
  const auto cs = (cudaStream_t)cs_;
  if (graph_exec_ != nullptr) {
    if (cudaGraphLaunch((cudaGraphExec_t)graph_exec_, cs) != cudaSuccess) {
      err = std::string("qwen4exp: graph launch: ") + cudaGetErrorString(cudaGetLastError());
      return false;
    }
  } else if (graph_enabled_ && graph_warm_) {
    cudaGraph_t gph = nullptr;
    bool ok = cudaStreamBeginCapture(cs, cudaStreamCaptureModeThreadLocal) == cudaSuccess;
    if (ok) ok = device_step_(token, err);
    const cudaError_t ce = cudaStreamEndCapture(cs, &gph);
    cudaGraphExec_t ex = nullptr;
    if (ok && ce == cudaSuccess && gph != nullptr && cudaGraphInstantiate(&ex, gph, 0) == cudaSuccess) {
      std::size_t nodes = 0;
      cudaGraphGetNodes(gph, nullptr, &nodes);
      cudaGraphDestroy(gph);
      graph_exec_ = ex;
      std::fprintf(stderr, "qwen4exp: decode step captured into a CUDA graph (%zu nodes)\n", nodes);
      if (cudaGraphLaunch(ex, cs) != cudaSuccess) {
        err = std::string("qwen4exp: graph launch: ") + cudaGetErrorString(cudaGetLastError());
        return false;
      }
    } else {
      if (gph != nullptr) cudaGraphDestroy(gph);
      std::fprintf(stderr, "qwen4exp: CUDA-graph capture failed (%s); decoding eagerly\n",
                   !err.empty() ? err.c_str() : cudaGetErrorString(ce != cudaSuccess ? ce : cudaGetLastError()));
      (void)cudaGetLastError();
      err.clear();
      graph_enabled_ = false;
      if (!device_step_(token, err)) return false;  // this step, eagerly
    }
  } else {
    if (!device_step_(token, err)) return false;
    graph_warm_ = true;  // the lazy allocations (attention / grouped-MoE scratch) happened: capture next
  }
  if (cpu_lane_on_ && !experts_->reg_lane_serve(1, cs_, err)) return false;  // answered the layers' doorbells
  if (ple_layer_ >= 0) ple_tokens_.push_back(token);  // grow the PLE n-gram window (this position done)
  if (moe_dev_any_) ids_pending_ = true;
  ++pos_;
  return true;
}

void Qwen4ExpGraph::flush_routing() {
  if (!ids_pending_) return;
  experts_->reg_note_device_token(h_ids_all_, g_.n_exp_used, moe_dev_);
  ids_pending_ = false;
}

bool Qwen4ExpGraph::sample_next(const Gen& request, std::int32_t& out_id, std::string& err) {
  k::SamplerParams sp;
  sp.counter = (std::uint64_t)pos_;
  for (const KeyVal& kv : request.params) {
    try {
      if (kv.key == "temperature" || kv.key == "temp") sp.temperature = std::stof(kv.value);
      else if (kv.key == "top_k") sp.top_k = std::stoi(kv.value);
      else if (kv.key == "top_p") sp.top_p = std::stof(kv.value);
      else if (kv.key == "min_p") sp.min_p = std::stof(kv.value);
      else if (kv.key == "seed") sp.seed = (std::uint64_t)std::stoull(kv.value);
    } catch (...) {
      err = "qwen4exp: malformed sampler param '" + kv.key + "'";
      return false;
    }
  }
  if (sp.temperature <= 0.0f) sp.greedy = true;
  k::sample_tokens(sc_ + sc_logits, 1, g_.n_vocab, nullptr, 0, sp, d_tok_, cs_);
  int id = 0;
  if (cudaMemcpyAsync(&id, d_tok_, sizeof(int), cudaMemcpyDeviceToHost, (cudaStream_t)cs_) != cudaSuccess ||
      cudaStreamSynchronize((cudaStream_t)cs_) != cudaSuccess) {  // the one real sync per token
    err = std::string("qwen4exp: sampler copy: ") + cudaGetErrorString(cudaGetLastError());
    return false;
  }
  out_id = (std::int32_t)id;
  return true;
}

bool Qwen4ExpGraph::generate(const Gen& request, const LineSink& sink, std::string& error) {
  if (!loaded_) {
    error = "qwen4exp: not loaded";
    return false;
  }
  reset();
  if (request.ids.empty()) { error = "qwen4exp generate: empty prompt"; return false; }
  if ((std::int64_t)request.ids.size() + (std::int64_t)request.max_new > max_ctx_) {
    error = "qwen4exp generate: the sequence exceeds max_ctx"; return false;
  }
  using Clk = std::chrono::steady_clock;
  // the prompt computes every routed expert (upstream's tail skip is decode-only)
  lane_decode_ = false;  // prefill computes every routed expert (the lane, like tail-skip, is decode-only)
  if (moe_dev_any_ && !experts_->reg_set_tail_skip(0, error)) return false;
  const auto t_pf = Clk::now();
  for (std::size_t i = 0; i < request.ids.size(); ++i) {
    if (mtp_on_ && i >= 1) {  // MTP cell i-1: the main residual there (still in the state head) + token i
      const std::int32_t tok = (std::int32_t)request.ids[i];
      if (!mtp_kv_rows_(state_, 1, &tok, (std::int64_t)i - 1, error)) return false;
    }
    if (!step((std::int32_t)request.ids[i], error)) return false;
    if (moe_dev_any_) {  // note each prompt token's routing heat (its ids D2H is on cs_)
      if (cudaStreamSynchronize((cudaStream_t)cs_) != cudaSuccess) { error = "qwen4exp: prefill sync failed"; return false; }
      flush_routing();
    }
  }
  if (cudaDeviceSynchronize() != cudaSuccess) { error = "qwen4exp: prefill sync failed"; return false; }
  const auto t_dec0 = Clk::now();
  sink("PP " + std::to_string(request.ids.size()) + " " + std::to_string(request.ids.size()) + " 0 0");
  experts_->reset_reg_stats();
  if (cpu_lane_on_) experts_->reg_lane_reset_stats();
  lane_decode_ = cpu_lane_on_;  // the lane takes its share of the decode misses from here
  if (moe_dev_any_ && !experts_->reg_set_tail_skip(tail_rank_, error)) return false;
  g_prof = std::getenv("STRATA_QWEN_PROF") != nullptr;
  g_prof_attn_ms = 0.0;
  g_prof_moe_ms = 0.0;
  // STRATA_QWEN_PROF_RANGE=1: bracket the decode with cudaProfilerStart/Stop so an external profiler
  // (nsys --capture-range=cudaProfilerApi) records the decode only, not the load-time expert fill.
  const bool prof_range = std::getenv("STRATA_QWEN_PROF_RANGE") != nullptr;
  if (prof_range) cudaProfilerStart();

  std::uint32_t produced = 0;
  int since_reb = 0;
  if (spec_t_ >= 2 && request.max_new > 0) {  // verify windows (the first pick comes from the prefill)
    std::int32_t id0 = 0;
    if (!sample_next(request, id0, error)) return false;
    flush_routing();
    sink("T " + std::to_string((std::uint32_t)id0));
    produced = 1;
    if (!decode_spec_(request, sink, produced, error)) return false;
  }
  for (std::uint32_t n = produced; n < request.max_new; ++n) {
    std::int32_t id = 0;
    if (!sample_next(request, id, error)) return false;  // syncs cs_: the last step's ids are on the host
    flush_routing();
    sink("T " + std::to_string((std::uint32_t)id));
    ++produced;
    if (n + 1 < request.max_new) {
      if (!step(id, error)) return false;
      if (++since_reb >= adapt_every_ && adapt_async_) {
        since_reb = 0;  // no sync: the round plans from the heat up to the last sampled token
        if (experts_->reg_rebalance_async(Calibration::profile().rebalance_margin, adapt_swaps_, cs_, error) < 0)
          return false;
      } else if (adapt_async_) {  // between rounds: a landed batch turns resident for the next step
        if (!experts_->reg_land_async(cs_, error)) return false;
      } else if (since_reb >= adapt_every_) {
        since_reb = 0;
        if (moe_dev_any_) {  // count this token against the residency it ran with, before it moves
          if (cudaStreamSynchronize((cudaStream_t)cs_) != cudaSuccess) { error = "qwen4exp: decode sync failed"; return false; }
          flush_routing();
        }
        if (experts_->reg_rebalance(Calibration::profile().rebalance_margin, nullptr, error) < 0) return false;
      }
    }
  }
  if (cudaDeviceSynchronize() != cudaSuccess) { error = "qwen4exp: decode sync failed"; return false; }
  if (prof_range) cudaProfilerStop();
  const auto t_end = Clk::now();
  const double pf_ms = std::chrono::duration<double, std::milli>(t_dec0 - t_pf).count();
  const double dec_ms = std::chrono::duration<double, std::milli>(t_end - t_dec0).count();
  sink("DONE " + std::to_string(produced) + " " + std::to_string(request.ids.size()) + " " +
       std::to_string(pf_ms) + " " + std::to_string(dec_ms) + " length 0 0 0");
  std::fprintf(stderr, "qwen4exp: prefill %zu tok / %.0f ms, decode %u tok / %.0f ms = %.2f tok/s\n",
               request.ids.size(), pf_ms, produced, dec_ms, dec_ms > 0 ? produced / (dec_ms / 1000.0) : 0.0);
  if (cpu_lane_on_) experts_->reg_lane_report();
  {
    const std::int64_t rh = experts_->reg_resident_hits(), rm = experts_->reg_stream_misses();
    const std::int64_t ts = experts_->reg_tail_skipped();
    std::fprintf(stderr,
                 "qwen4exp: expert cache over the decode: resident_hits=%lld stream_misses=%lld (%.1f%% hit), "
                 "tail_skipped=%lld\n",
                 (long long)rh, (long long)rm, rh + rm + ts > 0 ? 100.0 * (double)rh / (double)(rh + rm + ts) : 0.0,
                 (long long)ts);
  }
  if (g_prof && produced > 0)
    std::fprintf(stderr,
                 "qwen4exp PROF (sync-bracketed, per token): attn %.2f ms, moe(run_moe) %.2f ms, "
                 "other %.2f ms\n",
                 g_prof_attn_ms / produced, g_prof_moe_ms / produced,
                 (dec_ms - g_prof_attn_ms - g_prof_moe_ms) / produced);
  std::fflush(stderr);
  return true;
}

// ================================ make_qwen4exp_graph ================================
std::unique_ptr<FamilyGraph> make_qwen4exp_graph(const Geometry& geom, std::uint32_t context,
                                                 std::string& error) {
  error.clear();
  if (geom.family != FamilyId::qwen4exp) {
    error = "make_qwen4exp_graph: geometry is not qwen4exp";
    return nullptr;
  }
  if (geom.block_count <= 0) {
    error = "make_qwen4exp_graph: block_count must be positive";
    return nullptr;
  }
  // Weights are not uploaded here (no GGUF yet); the substrate calls Qwen4ExpGraph::load(gguf, mapped)
  // before the first generate(). Until then available() is false.
  return std::unique_ptr<FamilyGraph>(new Qwen4ExpGraph(geom, context));
}

}  // namespace u
