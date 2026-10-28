#include "f_model.hpp"

#include <cmath>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace fl {
namespace {

using g4::GgufTensor;
using g4::GgufType;

ModelError refuse(std::string what) { return ModelError{"flint model: " + std::move(what)}; }

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

// A bool[kLayers] array, as the patterns are written.
bool layer_pattern(const GgufFile& f, std::string_view key, std::vector<bool>& out) {
  const auto* v = f.find(key);
  if (v == nullptr || v->type != GgufType::array || v->element != GgufType::boolean || v->integers.size() != kLayers) return false;
  out.clear();
  for (const auto x : v->integers) out.push_back(x != 0);
  return true;
}

// norm: an F32 vector; matrix: Q8_0 or BF16 (the kernels' weights, the experts' blobs); router: F32, BF16 or Q8_0
// (a quantizer usually keeps it as the source has it).
enum class Kind { norm, matrix, router };

struct Located {
  int shard{0};
  const GgufFile* file{nullptr};
  const GgufTensor* tensor{nullptr};
};

// Binds `name` with the expected dims (ggml order) and kind; records the name as seen.
bool bind(const std::map<std::string, Located>& index, std::set<std::string>& bound, const std::string& name,
          const std::vector<std::uint64_t>& dims, Kind kind, TensorSpan& out, std::string& why) {
  const auto it = index.find(name);
  if (it == index.end()) {
    why = "missing tensor " + name;
    return false;
  }
  const GgufTensor& t = *it->second.tensor;
  if (t.dims != dims) {
    why = "tensor " + name + " has the wrong shape";
    return false;
  }
  const bool type_ok = kind == Kind::norm     ? t.type == TensorType::f32
                       : kind == Kind::matrix ? (t.type == TensorType::q8_0 || t.type == TensorType::bf16)
                                              : (t.type == TensorType::f32 || t.type == TensorType::bf16 || t.type == TensorType::q8_0);
  if (!type_ok) {
    why = "tensor " + name + " has a type the kernels do not take";
    return false;
  }
  out.type = t.type;
  out.cols = dims[0];
  out.rows = dims.size() > 1 ? dims[1] : 1;
  out.experts = dims.size() > 2 ? dims[2] : 1;
  out.shard = it->second.shard;
  out.file_offset = it->second.file->data_offset + t.offset;
  out.bytes = t.bytes;
  out.present = true;
  bound.insert(name);
  return true;
}

}  // namespace

std::variant<ModelDesc, ModelError> check_model(const std::vector<GgufFile>& shards) {
  if (shards.empty() || shards.size() > static_cast<std::size_t>(kMaxShards)) return refuse("no shards, or too many");
  const GgufFile& f = shards[0];
  const auto* arch = f.find("general.architecture");
  if (arch == nullptr || arch->type != GgufType::string || arch->text != "flint") return refuse("the architecture is not flint");
  // the shards: each knows its number and the count (a one-file model may leave both out)
  for (std::size_t s = 0; s < shards.size(); ++s) {
    std::int64_t no = -1, count = -1;
    const bool has_no = integer_key(shards[s], "split.no", no), has_count = integer_key(shards[s], "split.count", count);
    if (shards.size() == 1 && !has_no && !has_count) break;
    if (!has_no || !has_count || no != static_cast<std::int64_t>(s) || count != static_cast<std::int64_t>(shards.size())) {
      return refuse("split.no / split.count do not match the shards read");
    }
  }
  struct Exact {
    const char* key;
    std::int64_t value;
  };
  for (const Exact& e : {Exact{"flint.block_count", kLayers}, Exact{"flint.embedding_length", kEmbd},
                         Exact{"flint.feed_forward_length", kFf}, Exact{"flint.attention.head_count", kHeads},
                         Exact{"flint.attention.head_count_kv", kKvHeads}, Exact{"flint.attention.key_length", kHead},
                         Exact{"flint.attention.value_length", kHead}, Exact{"flint.expert_count", kExperts},
                         Exact{"flint.expert_used_count", kExpertsUsed}, Exact{"flint.expert_feed_forward_length", kFfExp},
                         Exact{"flint.expert_shared_feed_forward_length", kFfShexp},
                         Exact{"flint.leading_dense_block_count", kDenseLayers}, Exact{"flint.attention.sliding_window", kWindow},
                         Exact{"flint.expert_gating_func", 2}}) {   // 2: sigmoid (llama.cpp's LLAMA_EXPERT_GATING_FUNC_TYPE_SIGMOID)
    std::int64_t v = 0;
    if (!integer_key(f, e.key, v) || v != e.value) return refuse(std::string{e.key} + " is not flint's value");
  }
  const auto* norm = f.find("flint.expert_weights_norm");
  if (norm == nullptr || norm->type != GgufType::boolean || norm->integer != 1) {
    return refuse("flint.expert_weights_norm must be true (the top experts' weights renormalised)");
  }
  ModelDesc m;
  double real = 0.0;
  if (!real_key(f, "flint.attention.layer_norm_rms_epsilon", real) || real <= 0.0 || real > 1e-3) {
    return refuse("bad flint.attention.layer_norm_rms_epsilon");
  }
  m.rms_eps = static_cast<float>(real);
  if (!real_key(f, "flint.rope.freq_base", real) || real <= 1.0) return refuse("bad flint.rope.freq_base");
  m.rope_base = static_cast<float>(real);
  if (f.find("flint.expert_weights_scale") != nullptr) {
    if (!real_key(f, "flint.expert_weights_scale", real) || real <= 0.0 || real > 1e3) return refuse("bad flint.expert_weights_scale");
    m.expert_weights_scale = static_cast<float>(real);
  }
  std::int64_t ctx = 0;
  if (!integer_key(f, "flint.context_length", ctx) || ctx < 1 || ctx > (1 << 22)) return refuse("bad flint.context_length");
  m.context_train = ctx;
  std::vector<bool> window, rope;
  if (!layer_pattern(f, "flint.attention.sliding_window_pattern", window)) {
    return refuse("flint.attention.sliding_window_pattern must list 49 layers");
  }
  if (!layer_pattern(f, "flint.attention.rope_pattern", rope)) return refuse("flint.attention.rope_pattern must list 49 layers");

  const auto* tokens = f.find("tokenizer.ggml.tokens");
  if (tokens == nullptr || tokens->type != GgufType::array || tokens->element != GgufType::string ||
      tokens->texts.size() != static_cast<std::size_t>(kVocab)) {
    return refuse("tokenizer.ggml.tokens must hold 150144 tokens");
  }
  std::int64_t eos = -1, bos = -1;
  if (!integer_key(f, "tokenizer.ggml.eos_token_id", eos) || eos < 0 || eos >= kVocab) return refuse("bad tokenizer.ggml.eos_token_id");
  if (!integer_key(f, "tokenizer.ggml.bos_token_id", bos) || bos < 0 || bos >= kVocab) return refuse("bad tokenizer.ggml.bos_token_id");
  m.eos_id = static_cast<std::int32_t>(eos);
  m.bos_id = static_cast<std::int32_t>(bos);
  if (const auto* eog = f.find("tokenizer.ggml.eog_token_ids"); eog != nullptr) {
    if (eog->type != GgufType::array || eog->integers.empty() || eog->integers.size() > 64) return refuse("bad tokenizer.ggml.eog_token_ids");
    for (const auto id : eog->integers) {
      if (id < 0 || id >= kVocab) return refuse("an end-of-generation id is outside the vocabulary");
      m.eog_ids.push_back(static_cast<std::int32_t>(id));
    }
  }
  bool eos_listed = false;
  for (const auto id : m.eog_ids) eos_listed = eos_listed || id == m.eos_id;
  if (!eos_listed) m.eog_ids.push_back(m.eos_id);

  // every tensor of every shard, each in exactly one
  std::map<std::string, Located> index;
  std::size_t total = 0;
  for (std::size_t s = 0; s < shards.size(); ++s) {
    for (const auto& t : shards[s].tensors) {
      ++total;
      if (!index.emplace(t.name, Located{static_cast<int>(s), &shards[s], &t}).second) {
        return refuse("tensor " + t.name + " appears twice");
      }
    }
  }
  std::set<std::string> bound;
  std::string why;
  const std::uint64_t E = kEmbd, Q = std::uint64_t(kHeads) * kHead, KV = std::uint64_t(kKvHeads) * kHead;
  if (!bind(index, bound, "token_embd.weight", {E, kVocab}, Kind::matrix, m.token_embd, why) ||
      !bind(index, bound, "output_norm.weight", {E}, Kind::norm, m.output_norm, why) ||
      !bind(index, bound, "output.weight", {E, kVocab}, Kind::matrix, m.output, why)) {
    return refuse(why);
  }
  for (int i = 0; i < kLayers; ++i) {
    LayerDesc& L = m.layers[static_cast<std::size_t>(i)];
    L.sliding = window[static_cast<std::size_t>(i)];
    L.rope = rope[static_cast<std::size_t>(i)];
    L.moe = i >= kDenseLayers;
    const std::string p = "blk." + std::to_string(i) + ".";
    bool ok = bind(index, bound, p + "attn_norm.weight", {E}, Kind::norm, L.attn_norm, why) &&
              bind(index, bound, p + "attn_q.weight", {E, Q}, Kind::matrix, L.attn_q, why) &&
              bind(index, bound, p + "attn_k.weight", {E, KV}, Kind::matrix, L.attn_k, why) &&
              bind(index, bound, p + "attn_v.weight", {E, KV}, Kind::matrix, L.attn_v, why) &&
              bind(index, bound, p + "attn_output.weight", {Q, E}, Kind::matrix, L.attn_output, why) &&
              bind(index, bound, p + "attn_q_norm.weight", {kHead}, Kind::norm, L.attn_q_norm, why) &&
              bind(index, bound, p + "attn_k_norm.weight", {kHead}, Kind::norm, L.attn_k_norm, why) &&
              bind(index, bound, p + "attn_sinks.weight", {kHeads}, Kind::norm, L.attn_sinks, why) &&
              bind(index, bound, p + "ffn_norm.weight", {E}, Kind::norm, L.ffn_norm, why);
    if (ok && !L.moe) {
      ok = bind(index, bound, p + "ffn_gate.weight", {E, kFf}, Kind::matrix, L.ffn_gate, why) &&
           bind(index, bound, p + "ffn_up.weight", {E, kFf}, Kind::matrix, L.ffn_up, why) &&
           bind(index, bound, p + "ffn_down.weight", {kFf, E}, Kind::matrix, L.ffn_down, why);
    } else if (ok) {
      ok = bind(index, bound, p + "ffn_gate_inp.weight", {E, kExperts}, Kind::router, L.gate_inp, why) &&
           bind(index, bound, p + "ffn_gate_exps.weight", {E, kFfExp, kExperts}, Kind::matrix, L.gate_exps, why) &&
           bind(index, bound, p + "ffn_up_exps.weight", {E, kFfExp, kExperts}, Kind::matrix, L.up_exps, why) &&
           bind(index, bound, p + "ffn_down_exps.weight", {kFfExp, E, kExperts}, Kind::matrix, L.down_exps, why) &&
           bind(index, bound, p + "ffn_gate_shexp.weight", {E, kFfShexp}, Kind::matrix, L.gate_shexp, why) &&
           bind(index, bound, p + "ffn_up_shexp.weight", {E, kFfShexp}, Kind::matrix, L.up_shexp, why) &&
           bind(index, bound, p + "ffn_down_shexp.weight", {kFfShexp, E}, Kind::matrix, L.down_shexp, why);
      // an expert blob holds gate rows then up rows of one type (kernels::NativeExpertLayout's gu_type)
      if (ok && L.gate_exps.type != L.up_exps.type) {
        why = "blk." + std::to_string(i) + "'s gate and up experts differ in type";
        ok = false;
      }
    }
    if (!ok) return refuse(why);
  }
  if (bound.size() != total) return refuse("the file carries a tensor this engine does not know");
  return m;
}

std::vector<std::string> split_names(const std::string& any) {
  static const char tag[] = "-of-";
  const std::size_t at = any.rfind(tag);
  if (at == std::string::npos || at < 6 || any[at - 6] != '-' || any.size() != at + 4 + 5 + 5 ||
      any.compare(at + 9, 5, ".gguf") != 0) {
    return {any};
  }
  int no = 0, n = 0;
  for (std::size_t i = 0; i < 5; ++i) {
    const char a = any[at - 5 + i], b = any[at + 4 + i];
    if (a < '0' || a > '9' || b < '0' || b > '9') return {any};
    no = no * 10 + (a - '0');
    n = n * 10 + (b - '0');
  }
  if (n < 1 || n > kMaxShards || no < 1 || no > n) return {};
  std::vector<std::string> out;
  for (int i = 1; i <= n; ++i) {
    std::string p = any;
    const std::string num = std::to_string(100000 + i).substr(1);   // five digits
    p.replace(at - 5, 5, num);
    out.push_back(std::move(p));
  }
  return out;
}

std::variant<ModelDesc, ModelError> read_model(const std::string& any_shard) {
  const auto names = split_names(any_shard);
  if (names.empty()) return refuse("the model's shard name is malformed");
  std::vector<GgufFile> files;
  for (std::size_t s = 0; s < names.size(); ++s) {
    auto parsed = g4::read_gguf_header(names[s]);
    if (const auto* e = std::get_if<g4::GgufError>(&parsed)) {
      return refuse("shard " + std::to_string(s + 1) + " of " + std::to_string(names.size()) + ": " + e->message);
    }
    files.push_back(std::get<GgufFile>(std::move(parsed)));
  }
  auto checked = check_model(files);
  if (auto* m = std::get_if<ModelDesc>(&checked)) m->shards = names;
  return checked;
}

}  // namespace fl
