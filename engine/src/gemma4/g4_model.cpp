#include "g4_model.hpp"

#include <cmath>
#include <set>
#include <string_view>

namespace g4 {
namespace {

ModelError refuse(std::string what) { return ModelError{"gemma4 model: " + std::move(what)}; }

bool integer_key(const GgufFile& f, std::string_view key, std::int64_t& out) {
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

bool real_key(const GgufFile& f, std::string_view key, double& out) {
  const auto* v = f.find(key);
  if (v == nullptr || (v->type != GgufType::f32 && v->type != GgufType::f64)) return false;
  out = v->real;
  return std::isfinite(out);
}

enum class Kind { vector_f32, matrix, router };

// Binds `name` with the expected dims (ggml order) and kind; records the name as seen.
bool bind(const GgufFile& f, std::set<std::string>& bound, const std::string& name, std::vector<std::uint64_t> dims, Kind kind,
          TensorSpan& out, std::string& why) {
  const GgufTensor* t = f.tensor(name);
  if (t == nullptr) {
    why = "missing tensor " + name;
    return false;
  }
  if (t->dims != dims) {
    why = "tensor " + name + " has the wrong shape";
    return false;
  }
  const bool type_ok = kind == Kind::matrix ? (t->type == TensorType::q8_0 || t->type == TensorType::bf16)
                                            : t->type == TensorType::f32;
  if (!type_ok) {
    why = "tensor " + name + " has a type the kernels do not take";
    return false;
  }
  out.type = t->type;
  out.cols = dims[0];
  out.rows = dims.size() > 1 ? dims[1] : 1;
  out.experts = dims.size() > 2 ? dims[2] : 1;
  out.file_offset = f.data_offset + t->offset;
  out.bytes = t->bytes;
  out.present = true;
  bound.insert(name);
  return true;
}

}  // namespace

std::variant<ModelDesc, ModelError> check_model(const GgufFile& f) {
  const auto* arch = f.find("general.architecture");
  if (arch == nullptr || arch->type != GgufType::string || arch->text != "gemma4") return refuse("the architecture is not gemma4");
  struct Exact {
    const char* key;
    std::int64_t value;
  };
  for (const Exact& e : {Exact{"gemma4.block_count", kLayers}, Exact{"gemma4.embedding_length", kEmbd},
                         Exact{"gemma4.feed_forward_length", kFf}, Exact{"gemma4.attention.head_count", kHeads},
                         Exact{"gemma4.expert_count", kExperts}, Exact{"gemma4.expert_used_count", kExpertsUsed},
                         Exact{"gemma4.expert_feed_forward_length", kFfExp}, Exact{"gemma4.attention.key_length", kHeadFull},
                         Exact{"gemma4.attention.value_length", kHeadFull}, Exact{"gemma4.attention.key_length_swa", kHeadSwa},
                         Exact{"gemma4.attention.value_length_swa", kHeadSwa}, Exact{"gemma4.attention.sliding_window", kWindow},
                         Exact{"gemma4.rope.dimension_count", kHeadFull}, Exact{"gemma4.rope.dimension_count_swa", kHeadSwa}}) {
    std::int64_t v = 0;
    if (!integer_key(f, e.key, v) || v != e.value) return refuse(std::string{e.key} + " is not the 26B-A4B value");
  }
  for (const char* zero : {"gemma4.attention.shared_kv_layers", "gemma4.embedding_length_per_layer_input"}) {
    std::int64_t v = 0;
    if (integer_key(f, zero, v) && v != 0) return refuse(std::string{zero} + " must be 0 (only the 26B-A4B shape is loaded)");
  }
  ModelDesc m;
  double real = 0.0;
  if (!real_key(f, "gemma4.attention.layer_norm_rms_epsilon", real) || real <= 0.0 || real > 1e-3) {
    return refuse("bad gemma4.attention.layer_norm_rms_epsilon");
  }
  m.rms_eps = static_cast<float>(real);
  if (!real_key(f, "gemma4.rope.freq_base", real) || real <= 1.0) return refuse("bad gemma4.rope.freq_base");
  m.rope_base_full = static_cast<float>(real);
  if (!real_key(f, "gemma4.rope.freq_base_swa", real) || real <= 1.0) return refuse("bad gemma4.rope.freq_base_swa");
  m.rope_base_swa = static_cast<float>(real);
  if (!real_key(f, "gemma4.final_logit_softcapping", real) || real <= 0.0 || real > 1000.0) {
    return refuse("bad gemma4.final_logit_softcapping");
  }
  m.softcap = static_cast<float>(real);
  std::int64_t ctx = 0;
  if (!integer_key(f, "gemma4.context_length", ctx) || ctx < 1 || ctx > (1 << 22)) return refuse("bad gemma4.context_length");
  m.context_train = ctx;

  const auto* kv = f.find("gemma4.attention.head_count_kv");
  const auto* pattern = f.find("gemma4.attention.sliding_window_pattern");
  if (kv == nullptr || kv->type != GgufType::array || kv->integers.size() != kLayers) {
    return refuse("gemma4.attention.head_count_kv must list 30 layers");
  }
  if (pattern == nullptr || pattern->type != GgufType::array || pattern->element != GgufType::boolean ||
      pattern->integers.size() != kLayers) {
    return refuse("gemma4.attention.sliding_window_pattern must list 30 layers");
  }
  for (int i = 0; i < kLayers; ++i) {
    LayerDesc& L = m.layers[static_cast<std::size_t>(i)];
    L.sliding = pattern->integers[static_cast<std::size_t>(i)] != 0;
    L.head_dim = L.sliding ? kHeadSwa : kHeadFull;
    L.kv_heads = L.sliding ? kKvHeadsSwa : kKvHeadsFull;
    if (kv->integers[static_cast<std::size_t>(i)] != L.kv_heads) return refuse("a layer's KV head count is not the 26B-A4B value");
  }

  const auto* tokens = f.find("tokenizer.ggml.tokens");
  if (tokens == nullptr || tokens->type != GgufType::array || tokens->element != GgufType::string ||
      tokens->texts.size() != static_cast<std::size_t>(kVocab)) {
    return refuse("tokenizer.ggml.tokens must hold 262144 tokens");
  }
  std::int64_t eos = -1, bos = -1;
  if (!integer_key(f, "tokenizer.ggml.eos_token_id", eos) || eos < 0 || eos >= kVocab) return refuse("bad tokenizer.ggml.eos_token_id");
  if (!integer_key(f, "tokenizer.ggml.bos_token_id", bos) || bos < 0 || bos >= kVocab) return refuse("bad tokenizer.ggml.bos_token_id");
  m.eos_id = static_cast<std::int32_t>(eos);
  m.bos_id = static_cast<std::int32_t>(bos);
  int turn_close = -1;
  for (std::size_t i = 0; i < tokens->texts.size(); ++i) {
    if (tokens->texts[i] != "<turn|>") continue;
    if (turn_close >= 0) return refuse("the <turn|> token appears twice");
    turn_close = static_cast<int>(i);
  }
  if (turn_close < 0) return refuse("the vocabulary has no <turn|> token");
  m.turn_close_id = turn_close;

  std::set<std::string> bound;
  std::string why;
  const std::uint64_t E = kEmbd;
  if (!bind(f, bound, "token_embd.weight", {E, kVocab}, Kind::matrix, m.token_embd, why) ||
      !bind(f, bound, "output_norm.weight", {E}, Kind::vector_f32, m.output_norm, why) ||
      !bind(f, bound, "rope_freqs.weight", {kRopeFreqs}, Kind::vector_f32, m.rope_freqs, why)) {
    return refuse(why);
  }
  if (m.token_embd.type != TensorType::q8_0) return refuse("token_embd.weight must be Q8_0 (the tied head's kernel)");
  for (int i = 0; i < kLayers; ++i) {
    LayerDesc& L = m.layers[static_cast<std::size_t>(i)];
    const std::string p = "blk." + std::to_string(i) + ".";
    const std::uint64_t hd = static_cast<std::uint64_t>(L.head_dim);
    const std::uint64_t q = kHeads * hd, k = static_cast<std::uint64_t>(L.kv_heads) * hd;
    const bool ok =
        bind(f, bound, p + "attn_norm.weight", {E}, Kind::vector_f32, L.attn_norm, why) &&
        bind(f, bound, p + "attn_q.weight", {E, q}, Kind::matrix, L.attn_q, why) &&
        bind(f, bound, p + "attn_k.weight", {E, k}, Kind::matrix, L.attn_k, why) &&
        (f.tensor(p + "attn_v.weight") == nullptr || bind(f, bound, p + "attn_v.weight", {E, k}, Kind::matrix, L.attn_v, why)) &&
        bind(f, bound, p + "attn_output.weight", {q, E}, Kind::matrix, L.attn_output, why) &&
        bind(f, bound, p + "attn_q_norm.weight", {hd}, Kind::vector_f32, L.attn_q_norm, why) &&
        bind(f, bound, p + "attn_k_norm.weight", {hd}, Kind::vector_f32, L.attn_k_norm, why) &&
        bind(f, bound, p + "post_attention_norm.weight", {E}, Kind::vector_f32, L.post_attention_norm, why) &&
        bind(f, bound, p + "ffn_norm.weight", {E}, Kind::vector_f32, L.ffn_norm, why) &&
        bind(f, bound, p + "ffn_gate.weight", {E, kFf}, Kind::matrix, L.ffn_gate, why) &&
        bind(f, bound, p + "ffn_up.weight", {E, kFf}, Kind::matrix, L.ffn_up, why) &&
        bind(f, bound, p + "ffn_down.weight", {kFf, E}, Kind::matrix, L.ffn_down, why) &&
        bind(f, bound, p + "post_ffw_norm_1.weight", {E}, Kind::vector_f32, L.post_ffw_norm_1, why) &&
        bind(f, bound, p + "ffn_gate_inp.weight", {E, kExperts}, Kind::router, L.gate_inp, why) &&
        bind(f, bound, p + "ffn_gate_inp.scale", {E}, Kind::vector_f32, L.gate_inp_scale, why) &&
        bind(f, bound, p + "pre_ffw_norm_2.weight", {E}, Kind::vector_f32, L.pre_ffw_norm_2, why) &&
        bind(f, bound, p + "ffn_gate_up_exps.weight", {E, 2 * kFfExp, kExperts}, Kind::matrix, L.gate_up_exps, why) &&
        bind(f, bound, p + "ffn_down_exps.weight", {kFfExp, E, kExperts}, Kind::matrix, L.down_exps, why) &&
        bind(f, bound, p + "ffn_down_exps.scale", {kExperts}, Kind::vector_f32, L.down_exps_scale, why) &&
        bind(f, bound, p + "post_ffw_norm_2.weight", {E}, Kind::vector_f32, L.post_ffw_norm_2, why) &&
        bind(f, bound, p + "post_ffw_norm.weight", {E}, Kind::vector_f32, L.post_ffw_norm, why) &&
        (f.tensor(p + "layer_output_scale.weight") == nullptr ||
         bind(f, bound, p + "layer_output_scale.weight", {1}, Kind::vector_f32, L.layer_output_scale, why));
    if (!ok) return refuse(why);
    if (!L.sliding && L.attn_v.present) return refuse("a full-attention layer carries attn_v (this file shape is not the 26B-A4B one)");
    if (L.sliding && !L.attn_v.present) return refuse("missing tensor " + p + "attn_v.weight");
  }
  if (bound.size() != f.tensors.size()) {
    for (const auto& t : f.tensors) {
      if (bound.count(t.name) == 0) {
        return refuse(t.name == "output.weight" ? std::string{"an untied output.weight is not supported"}
                                                : std::string{"the file carries a tensor this engine does not know"});
      }
    }
  }
  return m;
}

}  // namespace g4
