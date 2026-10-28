// strata-prepare pack --maya-ds4 / --maya-mm3: Project Maya's tools/ds4/pack_gguf.py and tools/mm3/pack_gguf.py
// (v1.1.30), function for function (the Python names are kept so they read side by side). The Python tools are the
// reference: every file this writes is compared with theirs byte for byte (tools/secure/strata_prepare_parity.py).
#include "maya_moe_pack.hpp"

#include "gguf_header.hpp"
#include "pack_common.hpp"
#include "py_json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace strata_prepare {
namespace fs = std::filesystem;
namespace {

// raise SystemExit(message): the tool stops with it
struct Refused : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// ---- tools/ds4/pack_gguf.py's GGUF reader ------------------------------------------------------------------------

// TYPES: ggml type -> (name, values a block, bytes a block); 67 is unsloth's fork's MXFP8, 43 llama.cpp's own
struct TypeRow {
  std::uint32_t id;
  const char* name;
  std::uint64_t per;
  std::uint64_t bpb;
};
constexpr std::array<TypeRow, 21> kTypes{{
    {0, "F32", 1, 4},          {1, "F16", 1, 2},         {8, "Q8_0", 32, 34},      {30, "BF16", 1, 2},
    {39, "MXFP4", 32, 17},     {67, "MXFP8", 32, 33},    {43, "MXFP8", 32, 33},    {10, "Q2_K", 256, 84},
    {11, "Q3_K", 256, 110},    {12, "Q4_K", 256, 144},   {13, "Q5_K", 256, 176},   {14, "Q6_K", 256, 210},
    {16, "IQ2_XXS", 256, 66},  {17, "IQ2_XS", 256, 74},  {18, "IQ3_XXS", 256, 98}, {19, "IQ1_S", 256, 50},
    {20, "IQ4_NL", 32, 18},    {21, "IQ3_S", 256, 110},  {22, "IQ2_S", 256, 82},   {23, "IQ4_XS", 256, 136},
    {29, "IQ1_M", 256, 56},
}};

const TypeRow* type_of(const std::uint32_t id) {
  for (const auto& row : kTypes) {
    if (row.id == id) return &row;
  }
  return nullptr;
}

using Kv = std::map<std::string, MetaValue>;

struct GTensor {
  std::string name;
  std::uint32_t typ;
  std::vector<std::uint64_t> dims;   // ggml order, ne0 innermost
  std::uint64_t off;                 // from the data section's start
};

// class Gguf: one shard's metadata and tensor infos
struct Gguf {
  fs::path path;
  Kv kv;
  std::vector<GTensor> tensors;
  std::uint64_t data_start = 0;
  std::uint64_t file_size = 0;
};

Gguf read_gguf(const fs::path& path) {
  const GgufHeader h(path);
  Gguf g{path, h.metadata, {}, h.data_start, h.file_size};
  for (const auto& t : h.tensors) g.tensors.push_back(GTensor{t.name, t.type_id, t.shape, t.offset});
  return g;
}

std::uint64_t elements(const std::vector<std::uint64_t>& dims) {
  std::uint64_t n = 1;
  for (const auto d : dims) n *= d;
  return n;
}

// short(): why a file is shorter than its own tensor directory says (a download still running), or nullopt
std::optional<std::string> short_of(const Gguf& g) {
  std::uint64_t need = g.data_start;
  for (const auto& t : g.tensors) {
    if (const auto* row = type_of(t.typ)) need = std::max(need, g.data_start + t.off + elements(t.dims) / row->per * row->bpb);
  }
  if (g.file_size >= need) return std::nullopt;
  return utf8(g.path.filename()) + ": " + std::to_string(g.file_size) + " of " + std::to_string(need) + " bytes";
}

// ---- metadata as Python holds it ---------------------------------------------------------------------------------

const MetaValue* get(const Kv& kv, const std::string& key) {
  const auto it = kv.find(key);
  return it == kv.end() ? nullptr : &it->second;
}

MetaValue integer(const std::int64_t v) {
  MetaValue m;
  m.integer = v;
  return m;
}

MetaValue real(const double v) {
  MetaValue m;
  m.kind = MetaValue::Kind::real;
  m.real = v;
  return m;
}

MetaValue list_of(std::vector<MetaValue> items) {
  MetaValue m;
  m.kind = MetaValue::Kind::list;
  m.items = std::move(items);
  return m;
}

// str(v) of a value a pack's text file prints ("%s"); a list as its items joined with ","
std::string py_str(const MetaValue& v) {
  switch (v.kind) {
    case MetaValue::Kind::integer: return std::to_string(v.integer);
    case MetaValue::Kind::real: return python_float_repr(v.real);
    case MetaValue::Kind::text: return v.text;
    case MetaValue::Kind::list: {
      std::string out;
      for (std::size_t i = 0; i < v.items.size(); ++i) out += (i ? "," : "") + py_str(v.items[i]);
      return out;
    }
  }
  return {};
}

std::int64_t as_int(const MetaValue* v, const std::string& what) {
  const auto n = v == nullptr ? std::nullopt : python_int(*v);
  if (!n) throw Refused(what + ": not an integer");
  return *n;
}

double as_number(const MetaValue& v, const std::string& what) {
  if (v.kind == MetaValue::Kind::integer) return static_cast<double>(v.integer);
  if (v.kind == MetaValue::Kind::real) return v.real;
  throw Refused(what + ": not a number");
}

// list(v): a list's items
const std::vector<MetaValue>& as_list(const MetaValue* v, const std::string& what) {
  static const std::vector<MetaValue> kNone;
  if (v == nullptr) return kNone;
  if (v->kind != MetaValue::Kind::list) throw Refused(what + ": not a list");
  return v->items;
}

std::string text_of(const Kv& kv, const std::string& key) {
  const auto* v = get(kv, key);
  return v != nullptr && v->kind == MetaValue::Kind::text ? v->text : std::string{};
}

// The ordered lines of a geometry: a key and its value (nullopt: None, the line left out)
using Geometry = std::vector<std::pair<std::string, std::optional<MetaValue>>>;

std::string geometry_text(const Geometry& geo) {
  std::string out;
  for (const auto& [k, v] : geo) {
    if (v) out += k + " " + py_str(*v) + "\n";
  }
  return out;
}

// ---- the files both packs write ---------------------------------------------------------------------------------

// Path.read_text(encoding="utf-8"): universal newlines ("\r\n" and "\r" read as "\n")
std::string read_text_universal(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw Refused(utf8(path) + ": cannot be read");
  const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\r') {
      out.push_back('\n');
      if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
    } else {
      out.push_back(raw[i]);
    }
  }
  return out;
}

// write_tokenizer's common part: vocab.json, merges.txt, token_type.json (pack.py's layout from the GGUF)
std::vector<std::string> tokens_of(const Kv& kv) {
  const auto* tokens = get(kv, "tokenizer.ggml.tokens");
  if (tokens == nullptr) throw Refused("KeyError: 'tokenizer.ggml.tokens'");
  std::vector<std::string> out;
  for (const auto& t : as_list(tokens, "tokenizer.ggml.tokens")) {
    if (t.kind != MetaValue::Kind::text) throw Refused("tokenizer.ggml.tokens: not strings");
    out.push_back(t.text);
  }
  return out;
}

void write_vocab_merges_types(const fs::path& out, const Kv& kv, const std::vector<std::string>& tokens) {
  std::unordered_set<std::string> unique(tokens.begin(), tokens.end());
  if (unique.size() != tokens.size()) throw Refused("the GGUF's vocabulary has duplicate token strings");
  fs::create_directories(out);
  std::string vocab = "{";
  for (std::size_t i = 0; i < tokens.size(); ++i) vocab += (i ? ", " : "") + python_json_string(tokens[i], false) + ": " + std::to_string(i);
  write_text(out / "vocab.json", vocab + "}");
  // merges: "\n".join(kv.get(merges) or [])
  std::string merges;
  const auto* m = get(kv, "tokenizer.ggml.merges");
  if (m != nullptr && python_truthy(*m)) {
    const auto& items = as_list(m, "tokenizer.ggml.merges");
    for (std::size_t i = 0; i < items.size(); ++i) {
      if (items[i].kind != MetaValue::Kind::text) throw Refused("tokenizer.ggml.merges: not strings");
      merges += (i ? "\n" : "") + items[i].text;
    }
  }
  write_text(out / "merges.txt", merges);
  // token_type.json: json.dumps([int(t) for t in kv.get(token_type) or [1] * len(tokens)])
  std::string types = "[";
  const auto* ty = get(kv, "tokenizer.ggml.token_type");
  if (ty != nullptr && python_truthy(*ty)) {
    const auto& items = as_list(ty, "tokenizer.ggml.token_type");
    for (std::size_t i = 0; i < items.size(); ++i) types += (i ? ", " : "") + std::to_string(as_int(&items[i], "tokenizer.ggml.token_type"));
  } else {
    for (std::size_t i = 0; i < tokens.size(); ++i) types += i ? ", 1" : "1";
  }
  write_text(out / "token_type.json", types + "]");
}

// One `t` entry of tensors.txt
struct Entry {
  std::string name;
  std::string dtype;
  std::size_t file;
  std::uint64_t off;
  std::uint64_t bytes;
  std::vector<std::uint64_t> shape;   // outermost first (torch order)
};

std::string tensors_text(const std::vector<std::string>& files, const std::vector<Entry>& tensors) {
  std::string out;
  for (std::size_t i = 0; i < files.size(); ++i) out += "file " + std::to_string(i) + " " + files[i] + "\n";
  for (const auto& t : tensors) {
    out += "t " + t.name + " " + t.dtype + " " + std::to_string(t.file) + " " + std::to_string(t.off) + " " + std::to_string(t.bytes);
    out += " ";
    for (std::size_t i = 0; i < t.shape.size(); ++i) out += (i ? " " : "") + std::to_string(t.shape[i]);
    out += "\n";
  }
  return out;
}

std::string summary_by_type(const std::vector<Entry>& tensors) {
  std::map<std::string, std::uint64_t> by;
  for (const auto& t : tensors) by[t.dtype] += t.bytes;
  std::string out;
  for (const auto& [k, v] : by) out += (out.empty() ? "" : ", ") + k + " " + fmt("%.1f", static_cast<double>(v) / 1e9) + " GB";
  return out;
}

// sorted(model.glob("*.gguf")), or the named files
std::vector<fs::path> gguf_files(const fs::path& model, const std::vector<std::string>& names) {
  std::vector<fs::path> out;
  if (!names.empty()) {
    for (const auto& n : names) out.push_back(model / path_from_utf8(n));
    return out;
  }
  std::error_code ec;
  if (!fs::is_directory(model, ec)) return out;
  std::vector<std::pair<std::string, fs::path>> found;
  for (const auto& entry : fs::directory_iterator(model, ec)) {
    std::string name = utf8(entry.path().filename());
#if defined(_WIN32)
    const std::string key = lower(name);   // Windows: glob and sort ignore case
#else
    const std::string key = name;
#endif
    if (key.size() >= 5U && key.compare(key.size() - 5U, 5U, ".gguf") == 0) found.emplace_back(key, entry.path());
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (auto& [key, path] : found) out.push_back(std::move(path));
  return out;
}

// os.path.relpath(p, model) with "/" separators
std::string rel(const fs::path& p, const fs::path& model) {
  std::string r = utf8(p.lexically_normal().lexically_relative(model.lexically_normal()));
  if (r.empty()) r = utf8(p);   // (another drive: no relative path)
  std::replace(r.begin(), r.end(), '\\', '/');
  return r;
}

// "the shards of more than one GGUF split" or "N of the M shards are here"
void check_split(const fs::path& model, const std::vector<fs::path>& paths, const std::size_t n, const Kv& kv) {
  const auto* count = get(kv, "split.count");
  const std::int64_t split = count == nullptr ? 1 : as_int(count, "split.count");
  if (split == static_cast<std::int64_t>(n)) return;
  static const std::regex of(R"(-of-(\d{5})\.gguf$)");
  std::set<std::string> counts;
  for (const auto& p : paths) {
    std::smatch m;
    const std::string name = utf8(p.filename());
    if (std::regex_search(name, m, of)) counts.insert(m[1].str());
  }
  if (counts.size() < 2U) throw Refused(utf8(model) + ": " + std::to_string(paths.size()) + " of the " + std::to_string(split) + " shards are here");
  std::string list;
  for (const auto& c : counts) list += (list.empty() ? "" : ", -of-") + c;
  throw Refused(utf8(model) + ": the shards of more than one GGUF split (-of-" + list + "): name the model's own with --shards");
}

fs::path resolved(const fs::path& p) {
  std::error_code ec;
  fs::path c = fs::weakly_canonical(fs::absolute(p), ec);
  if (ec) c = fs::absolute(p);
  return c.make_preferred();
}

// ---- DeepSeek-V4.1: tools/ds4/pack_gguf.py -----------------------------------------------------------------------

const std::map<std::string, std::string> kDs4Layer{
    {"attn_norm.weight", "attn_norm.weight"},
    {"attn_sinks.weight", "attn.attn_sink"},
    {"attn_q_a.weight", "attn.wq_a.weight"},
    {"attn_q_a_norm.weight", "attn.q_norm.weight"},
    {"attn_q_b.weight", "attn.wq_b.weight"},
    {"attn_kv.weight", "attn.wkv.weight"},
    {"attn_kv_a_norm.weight", "attn.kv_norm.weight"},
    {"attn_output_a.weight", "attn.wo_a.slice"},
    {"attn_output_b.weight", "attn.wo_b.weight"},
    {"attn_compressor_kv.weight", "attn.compressor.wkv.weight"},
    {"attn_compressor_gate.weight", "attn.compressor.wgate.weight"},
    {"attn_compressor_norm.weight", "attn.compressor.norm.weight"},
    {"indexer.attn_q_b.weight", "attn.indexer.wq_b.weight"},
    {"indexer.attn_k.weight", "attn.indexer.wk.weight"},
    {"indexer.k_norm.weight", "attn.indexer.k_norm.weight"},
    {"indexer.proj.weight", "attn.indexer.weights_proj.weight"},
    {"hc_attn_fn.weight", "hc_attn_fn"},
    {"hc_attn_base.weight", "hc_attn_base"},
    {"hc_attn_scale.weight", "hc_attn_scale"},
    {"hc_ffn_fn.weight", "hc_ffn_fn"},
    {"hc_ffn_base.weight", "hc_ffn_base"},
    {"hc_ffn_scale.weight", "hc_ffn_scale"},
    {"ffn_norm.weight", "ffn_norm.weight"},
    {"ffn_gate_inp.weight", "ffn.gate.weight"},
    {"exp_probs_b.bias", "ffn.gate.bias"},
    {"exp_probs_b_vl.bias", "ffn.gate.bias_vl"},
    {"ffn_gate_shexp.weight", "ffn.shared_experts.w1.weight"},
    {"ffn_up_shexp.weight", "ffn.shared_experts.w3.weight"},
    {"ffn_down_shexp.weight", "ffn.shared_experts.w2.weight"},
    {"ffn_gate_exps.weight", "ffn.experts.w1"},
    {"ffn_up_exps.weight", "ffn.experts.w3"},
    {"ffn_down_exps.weight", "ffn.experts.w2"},
    {"engram_embd.weight", "engram.embed.weight"},
    {"engram_kv.weight", "engram.wkv.weight"},
    {"engram_gate.weight", "engram.gate.weight"},
    {"engram_wkv.weight", "engram.wkv.weight"},
    {"engram_q.weight", "engram.q_weight"},
    {"engram_k.weight", "engram.k_weight"},
};
const std::map<std::string, std::string> kDs4Global{
    {"token_embd.weight", "embed.weight"}, {"output_norm.weight", "norm.weight"}, {"output.weight", "head.weight"}};
// newer converters' spellings of the metadata -> the ones this pack reads
constexpr std::array<std::pair<const char*, const char*>, 11> kNewerKeys{{
    {"kv_source_layer_ids", "attention.kv_source_layers"},
    {"index_source_layer_ids", "attention.indexer.source_layers"},
    {"attention.candidate_source_layer_id", "attention.indexer.candidate_source_layer"},
    {"attention.candidate_topk_blocks", "attention.indexer.candidate_top_k_blocks"},
    {"attention.candidate_block_size", "attention.indexer.candidate_block_size"},
    {"dspark_target_layer_ids", "dspark.target_layers"},
    {"engram.layer_ids", "engram.layers"},
    {"engram_max_ngram_size", "engram.max_ngram_size"},
    {"engram_n_heads", "engram.head_count"},
    {"engram_head_dim", "engram.head_dim"},
    {"engram_pad_token_id", "engram.pad_token_id"},
}};
const std::map<std::string, std::string> kHashTensors{{"engram_multipliers.weight", "engram.multipliers"},
                                                      {"engram_primes.weight", "engram.primes"},
                                                      {"engram_offsets.weight", "engram.offsets"},
                                                      {"engram_token_map.weight", "engram.token_map"}};

Geometry ds4_geometry(const Kv& kv, const std::int64_t vocab) {
  const std::string a = text_of(kv, "general.architecture");
  const auto g = [&](const std::string& k) -> std::optional<MetaValue> {
    const auto* v = get(kv, a + "." + k);
    return v == nullptr ? std::nullopt : std::optional<MetaValue>{*v};
  };
  const auto gd = [&](const std::string& k, MetaValue d) -> std::optional<MetaValue> { return g(k).value_or(std::move(d)); };
  const auto list_g = [&](const std::string& k) { return list_of(as_list(get(kv, a + "." + k), k)); };   // list(g(k, []))
  // (g("swiglu_clamp_exp") or [0.0])[0]
  const auto clamp_value = get(kv, a + ".swiglu_clamp_exp");
  MetaValue limit = real(0.0);
  if (clamp_value != nullptr && python_truthy(*clamp_value)) {
    const auto& items = as_list(clamp_value, "swiglu_clamp_exp");
    limit = items.front();
    for (const auto& c : items) {
      if (std::fabs(as_number(c, "swiglu_clamp_exp") - as_number(items.front(), "swiglu_clamp_exp")) > 1e-6) {
        std::string shown = "[";
        for (std::size_t i = 0; i < std::min<std::size_t>(8U, items.size()); ++i) shown += (i ? ", " : "") + py_str(items[i]);
        throw Refused("swiglu_clamp_exp differs between layers " + shown + "]: the engine takes one swiglu_limit");
      }
    }
  }
  const auto* eos = get(kv, "tokenizer.ggml.eos_token_id");
  return Geometry{
      {"vocab_size", integer(vocab)},
      {"hidden_size", g("embedding_length")},
      {"num_hidden_layers", g("block_count")},
      {"num_attention_heads", g("attention.head_count")},
      {"head_dim", g("attention.key_length")},
      {"qk_rope_head_dim", g("rope.dimension_count")},
      {"q_lora_rank", g("attention.q_lora_rank")},
      {"o_lora_rank", g("attention.output_lora_rank")},
      {"o_groups", g("attention.output_group_count")},
      {"n_routed_experts", g("expert_count")},
      {"num_experts_per_tok", g("expert_used_count")},
      {"moe_intermediate_size", g("expert_feed_forward_length")},
      {"sliding_window", g("attention.sliding_window")},
      {"hc_mult", g("hyper_connection.count")},
      {"hc_sinkhorn_iters", g("hyper_connection.sinkhorn_iterations")},
      {"hc_eps", g("hyper_connection.epsilon")},
      {"index_n_heads", g("attention.indexer.head_count")},
      {"index_head_dim", g("attention.indexer.key_length")},
      {"index_topk", g("attention.indexer.top_k")},
      {"candidate_source_layer_id", g("attention.indexer.candidate_source_layer")},
      {"candidate_topk_blocks", g("attention.indexer.candidate_top_k_blocks")},
      {"candidate_block_size", g("attention.indexer.candidate_block_size")},
      {"rope_scaling.original_max_position_embeddings", g("rope.scaling.original_context_length")},
      {"rope_scaling.factor", g("rope.scaling.factor")},
      {"rope_scaling.beta_fast", g("rope.scaling.yarn_beta_fast")},
      {"rope_scaling.beta_slow", g("rope.scaling.yarn_beta_slow")},
      {"eos_token_id", eos != nullptr ? std::optional<MetaValue>{*eos} : std::optional<MetaValue>{integer(1)}},
      {"rms_norm_eps", g("attention.layer_norm_rms_epsilon")},
      {"routed_scaling_factor", g("expert_weights_scale")},
      {"swiglu_limit", limit},
      {"rope_theta", g("rope.freq_base")},
      {"compress_rope_theta", g("attention.compress_rope_freq_base")},
      {"compress_ratios", list_g("attention.compress_ratios")},
      {"kv_source_layer_ids", list_g("attention.kv_source_layers")},
      {"index_source_layer_ids", list_g("attention.indexer.source_layers")},
      {"engram_layer_ids", list_g("engram.layers")},
      {"max_position_embeddings", g("context_length")},
      {"dspark_block_size", gd("dspark.block_size", integer(0))},
      {"dspark_noise_token_id", gd("dspark.noise_token_id", integer(0))},
      {"dspark_markov_rank", gd("dspark.markov_rank", integer(0))},
      {"dspark_n_routed_experts", gd("dspark.expert_count", integer(0))},
      {"dspark_num_experts_per_tok", gd("dspark.expert_used_count", integer(0))},
      {"dspark_target_layer_ids", list_g("dspark.target_layers")},
  };
}

// ple_to_engram: llama.cpp's ple.* names for the Engram metadata -> the engram.* keys (each layer's offsets its own)
void ple_to_engram(Kv& kv) {
  const std::string a = text_of(kv, "general.architecture");
  const auto* engram_layers = get(kv, a + ".engram.layers");
  const auto* ple_layers = get(kv, a + ".ple.layers");
  if ((engram_layers != nullptr && python_truthy(*engram_layers)) || ple_layers == nullptr || !python_truthy(*ple_layers)) return;
  const auto layers = as_list(ple_layers, "ple.layers");
  const auto ngram = as_int(get(kv, a + ".ple.ngram_size"), "ple.ngram_size");
  const auto heads = as_int(get(kv, a + ".ple.heads_per_ngram"), "ple.heads_per_ngram");
  const auto offs = as_list(get(kv, a + ".ple.head_offsets"), "ple.head_offsets");
  const auto sizes = as_list(get(kv, a + ".ple.head_vocab_sizes"), "ple.head_vocab_sizes");
  const auto cols = static_cast<std::size_t>((ngram - 1) * heads);
  if (offs.size() != layers.size() * cols || sizes.size() != offs.size()) {
    throw Refused("ple: " + std::to_string(offs.size()) + " head offsets, " + std::to_string(sizes.size()) + " sizes for " +
                  std::to_string(layers.size()) + " layers x " + std::to_string(cols) + " columns");
  }
  if (cols == 0U) throw Refused("ple: no hash columns");
  std::vector<MetaValue> per;
  std::vector<MetaValue> rows;
  for (std::size_t i = 0; i < layers.size(); ++i) {
    const auto first = as_int(&offs[i * cols], "ple.head_offsets");
    for (std::size_t c = 0; c < cols; ++c) per.push_back(integer(as_int(&offs[i * cols + c], "ple.head_offsets") - first));
    rows.push_back(integer(as_int(&offs[i * cols + cols - 1], "ple.head_offsets") + as_int(&sizes[i * cols + cols - 1], "ple.head_vocab_sizes") - first));
  }
  kv[a + ".engram.layers"] = list_of(layers);
  kv[a + ".engram.max_ngram_size"] = integer(ngram);
  kv[a + ".engram.head_count"] = integer(heads);
  if (const auto* hd = get(kv, a + ".embedding_length_per_layer_input")) kv[a + ".engram.head_dim"] = *hd;
  kv[a + ".engram.multipliers"] = list_of(as_list(get(kv, a + ".ple.layer_multipliers"), "ple.layer_multipliers"));
  kv[a + ".engram.primes"] = list_of(sizes);
  kv[a + ".engram.offsets"] = list_of(std::move(per));
  kv[a + ".engram.num_embeddings"] = list_of(std::move(rows));
}

// newer_keys: NEWER_KEYS's first names under the second (an existing key wins); the Engram hash tensors read, left out
void newer_keys(std::vector<Gguf>& gs, Kv& kv) {
  const std::string a = text_of(kv, "general.architecture");
  for (const auto& [theirs, ours] : kNewerKeys) {
    if (const auto* v = get(kv, a + "." + theirs); v != nullptr && kv.count(a + "." + ours) == 0U) kv[a + "." + ours] = *v;
  }
  for (auto& g : gs) {
    std::vector<GTensor> keep;
    for (auto& t : g.tensors) {
      const auto hash = kHashTensors.find(t.name);
      if (hash == kHashTensors.end()) {
        keep.push_back(std::move(t));
        continue;
      }
      if (t.typ != 26U && t.typ != 27U) {
        throw Refused(utf8(g.path.filename()) + ": " + t.name + " has ggml type " + std::to_string(t.typ) + " (I32 / I64 expected)");
      }
      const std::size_t size = t.typ == 26U ? 4U : 8U;
      const auto n = static_cast<std::size_t>(elements(t.dims));
      std::ifstream f(g.path, std::ios::binary);
      f.seekg(static_cast<std::streamoff>(g.data_start + t.off));
      std::vector<unsigned char> raw(n * size);
      if (n != 0U) f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
      if (!f) throw Refused(utf8(g.path.filename()) + ": " + t.name + " could not be read");
      std::vector<MetaValue> values;
      values.reserve(n);
      for (std::size_t i = 0; i < n; ++i) {
        if (size == 4U) {
          std::int32_t v = 0;
          std::memcpy(&v, raw.data() + 4U * i, 4);
          values.push_back(integer(v));
        } else {
          std::int64_t v = 0;
          std::memcpy(&v, raw.data() + 8U * i, 8);
          values.push_back(integer(v));
        }
      }
      if (kv.count(a + "." + hash->second) == 0U) kv[a + "." + hash->second] = list_of(std::move(values));
    }
    g.tensors = std::move(keep);
  }
}

// split_ple: llama.cpp's one Engram tensor (every layer's table in turn) -> each layer's own blk.N.engram_embd
void split_ple(Gguf& g, const Kv& kv) {
  const std::string a = text_of(kv, "general.architecture");
  const auto it = std::find_if(g.tensors.begin(), g.tensors.end(), [](const GTensor& t) { return t.name == "per_layer_token_embd.weight"; });
  if (it == g.tensors.end()) return;
  const GTensor ple = *it;
  const auto layers = as_list(get(kv, a + ".engram.layers"), "engram.layers");
  const auto rows = as_list(get(kv, a + ".engram.num_embeddings"), "engram.num_embeddings");
  std::int64_t sum = 0;
  for (const auto& r : rows) sum += as_int(&r, "engram.num_embeddings");
  const auto* row_type = type_of(ple.typ);
  if (ple.dims.size() != 2U || row_type == nullptr || rows.size() != layers.size() || sum != static_cast<std::int64_t>(ple.dims[1])) {
    throw Refused(utf8(g.path.filename()) + ": " + ple.name + " does not split in the Engram layers of their rows");
  }
  const std::uint64_t row = ple.dims[0] / row_type->per * row_type->bpb;
  std::vector<GTensor> kept;
  for (auto& t : g.tensors) {
    if (t.name != ple.name) kept.push_back(std::move(t));
  }
  std::uint64_t start = 0;
  for (std::size_t i = 0; i < layers.size(); ++i) {
    const auto n = static_cast<std::uint64_t>(as_int(&rows[i], "engram.num_embeddings"));
    kept.push_back(GTensor{"blk." + std::to_string(as_int(&layers[i], "engram.layers")) + ".engram_embd.weight", ple.typ, {ple.dims[0], n},
                           ple.off + start * row});
    start += n;
  }
  g.tensors = std::move(kept);
}

// engram_set: the Engram tables' GGUF set for a quant that leaves them out: (its folder, its files), or none
std::pair<std::optional<fs::path>, std::vector<Gguf>> engram_set(const fs::path& model, const fs::path& explicit_dir, std::ostream& log) {
  std::vector<fs::path> dirs;
  if (!explicit_dir.empty()) dirs.push_back(resolved(explicit_dir));
  else dirs = {model / "Engram", model.parent_path() / "Engram"};
  for (const auto& d : dirs) {
    const auto paths = gguf_files(d, {});
    if (paths.empty()) {
      if (!explicit_dir.empty()) throw Refused(utf8(d) + ": no .gguf files (--engram: the folder of the Engram tables' GGUF set)");
      continue;
    }
    std::vector<Gguf> egs;
    std::optional<std::string> why;
    try {
      for (const auto& p : paths) egs.push_back(read_gguf(p));
      for (const auto& g : egs) {
        if (!why) why = short_of(g);
      }
    } catch (const std::exception& e) {
      why = e.what();
    }
    if (!why) return {d, std::move(egs)};
    if (!explicit_dir.empty()) throw Refused(utf8(d) + ": not whole (" + *why + ")");
    log << utf8(d) << ": not whole (" << *why << ") - not used\n";
  }
  return {std::nullopt, {}};
}

// write_engram: engram.bin, (layers, hash columns); nothing written for a model without Engram layers
std::pair<std::size_t, std::size_t> write_engram(const fs::path& out, const Kv& kv, const std::int64_t vocab) {
  const std::string a = text_of(kv, "general.architecture");
  const auto layers = as_list(get(kv, a + ".engram.layers"), "engram.layers");
  if (layers.empty()) return {0, 0};
  const auto ngram = as_int(get(kv, a + ".engram.max_ngram_size"), "engram.max_ngram_size");
  const auto heads = as_int(get(kv, a + ".engram.head_count"), "engram.head_count");
  const auto hd = as_int(get(kv, a + ".engram.head_dim"), "engram.head_dim");
  const auto tm = as_list(get(kv, a + ".engram.token_map"), "engram.token_map");
  const auto mult = as_list(get(kv, a + ".engram.multipliers"), "engram.multipliers");
  const auto primes = as_list(get(kv, a + ".engram.primes"), "engram.primes");
  const auto offs = as_list(get(kv, a + ".engram.offsets"), "engram.offsets");
  const auto cols = static_cast<std::size_t>((ngram - 1) * heads);
  const std::size_t nl = layers.size();
  if (mult.size() != nl * static_cast<std::size_t>(ngram) || primes.size() != nl * cols || offs.size() != nl * cols) {
    throw Refused("engram: " + std::to_string(mult.size()) + " multipliers, " + std::to_string(primes.size()) + " primes, " +
                  std::to_string(offs.size()) + " offsets for " + std::to_string(nl) + " layers x " + std::to_string(cols) +
                  " columns (ngram " + std::to_string(ngram) + ")");
  }
  const auto pad_id = as_int(get(kv, a + ".engram.pad_token_id"), "engram.pad_token_id");
  if (pad_id < 0 || static_cast<std::size_t>(pad_id) >= tm.size()) throw Refused("engram: the pad token is outside the token map");
  std::string bin = "DSE1";
  const auto i32 = [&bin](const std::int64_t v) {
    if (v < std::numeric_limits<std::int32_t>::min() || v > std::numeric_limits<std::int32_t>::max()) throw Refused("engram: a value past int32");
    const auto x = static_cast<std::int32_t>(v);
    bin.append(reinterpret_cast<const char*>(&x), 4);
  };
  const auto i64 = [&bin](const std::int64_t v) { bin.append(reinterpret_cast<const char*>(&v), 8); };
  for (const auto v : {vocab, static_cast<std::int64_t>(nl), ngram, heads, hd, as_int(&tm[static_cast<std::size_t>(pad_id)], "engram.token_map")}) i32(v);
  for (std::int64_t i = 0; i < vocab; ++i) i32(static_cast<std::size_t>(i) < tm.size() ? as_int(&tm[static_cast<std::size_t>(i)], "engram.token_map") : 0);
  for (const auto& v : mult) i64(as_int(&v, "engram.multipliers"));
  for (const auto& v : primes) i64(as_int(&v, "engram.primes"));
  for (const auto& v : offs) i64(as_int(&v, "engram.offsets"));
  write_lf(out, bin);
  return {nl, cols};
}

}  // namespace

int maya_ds4_pack(const MoePackOptions& options, std::ostream& log) {
  try {
    const fs::path model = resolved(options.model);
    const fs::path out = options.out.empty() ? model / "maya-ds4" : options.out;
    const auto shards = gguf_files(model, options.shards);
    if (shards.empty()) throw Refused(utf8(model) + ": no .gguf files");
    for (const auto& p : shards) {
      if (!fs::is_regular_file(p)) throw Refused(utf8(p) + ": not there");
    }
    std::vector<Gguf> all;
    for (const auto& p : shards) all.push_back(read_gguf(p));
    // a folder of shards beside a separate Engram GGUF: its tables, read below, not a shard
    std::vector<Gguf> gs;
    std::vector<Gguf> side;
    for (auto& g : all) (text_of(g.kv, "general.architecture") == "deepseek41-engram" ? side : gs).push_back(std::move(g));
    const auto with_arch = std::find_if(gs.begin(), gs.end(), [](const Gguf& g) { return g.kv.count("general.architecture") != 0U; });
    if (with_arch == gs.end() || text_of(with_arch->kv, "general.architecture") != "deepseek41") {
      const std::string arch = with_arch == gs.end() ? "None" : python_repr(text_of(with_arch->kv, "general.architecture"));
      throw Refused(utf8(model) + ": not a DeepSeek-V4.1 GGUF (architecture " + arch + ")");
    }
    Kv kv = with_arch->kv;
    check_split(model, shards, gs.size(), kv);
    newer_keys(gs, kv);
    ple_to_engram(kv);
    for (auto& g : gs) split_ple(g, kv);
    std::set<std::string> have;
    for (const auto& g : gs) {
      for (const auto& t : g.tensors) have.insert(t.name);
    }
    const std::string arch = "deepseek41";
    const auto layer_ids = [&kv, &arch] {
      std::vector<std::int64_t> ids;
      for (const auto& v : as_list(get(kv, arch + ".engram.layers"), "engram.layers")) ids.push_back(as_int(&v, "engram.layers"));
      return ids;
    };
    // each Engram layer: its tables, its kv projection and its gate (folded, or its q and k apart)
    const std::array<std::vector<std::string>, 3> parts{{{"engram_embd"}, {"engram_kv", "engram_wkv"}, {"engram_gate", "engram_q"}}};
    const auto blk = [](const std::int64_t il, const std::string& p) { return "blk." + std::to_string(il) + "." + p + ".weight"; };
    bool lacking = layer_ids().empty();
    for (const auto il : layer_ids()) {
      for (const auto& alts : parts) {
        lacking = lacking || std::none_of(alts.begin(), alts.end(), [&](const std::string& p) { return have.count(blk(il, p)) != 0U; });
      }
    }
    const std::string own = text_of(kv, arch + ".engram.file");
    std::optional<fs::path> eg_dir;
    std::vector<Gguf> egs;
    if (lacking && options.engram.empty() && (!side.empty() || (!own.empty() && fs::is_regular_file(model / path_from_utf8(own))))) {
      eg_dir = model;
      if (!side.empty()) egs = std::move(side);
      else egs.push_back(read_gguf(model / path_from_utf8(own)));
    } else if (lacking || !options.engram.empty()) {
      auto found = engram_set(model, options.engram, log);
      eg_dir = std::move(found.first);
      egs = std::move(found.second);
    }
    std::vector<Gguf> eg_from;
    if (eg_dir) {
      for (const auto& g : egs) {
        for (const auto& [k, v] : g.kv) {
          if (k.find(".engram.") != std::string::npos || k.find(".ple.") != std::string::npos || ends_with(k, ".embedding_length_per_layer_input")) {
            const bool engram = k.find(".engram.") != std::string::npos;
            if (engram && kv.count(k) != 0U && py_str(kv[k]) != py_str(v)) {
              log << "Engram " << k.substr(k.find(".engram.") + 8) << ": the tables' own (" << py_str(v).substr(0, 60) << ") over the quant's\n";
            }
            if (engram || kv.count(k) == 0U) kv[k] = v;
          }
        }
      }
      ple_to_engram(kv);
      for (auto& g : egs) split_ple(g, kv);
      static const std::regex engram_tensor(R"(^blk\.\d+\.engram_)");
      for (auto& g : egs) {
        std::vector<GTensor> picked;
        for (const auto& t : g.tensors) {
          if (!std::regex_search(t.name, engram_tensor) || have.count(t.name) != 0U) continue;
          // what the quant's shards hold under another name stays the quant's
          bool alias = false;
          for (const auto& [x, y] : std::array<std::pair<std::string, std::string>, 2>{{{"engram_kv", "engram_wkv"}, {"engram_gate", "engram_q"}}}) {
            if (ends_with(t.name, "." + x + ".weight")) {
              std::string other = t.name;
              for (auto at = other.find(x); at != std::string::npos; at = other.find(x, at + y.size())) other.replace(at, x.size(), y);
              alias = alias || have.count(other) != 0U;
            }
          }
          if (!alias) picked.push_back(t);
        }
        if (!picked.empty()) {
          g.tensors = std::move(picked);
          eg_from.push_back(std::move(g));
        }
      }
    }
    for (const auto il : layer_ids()) {
      for (const auto& alts : parts) {
        bool found = false;
        for (const auto& p : alts) {
          found = found || have.count(blk(il, p)) != 0U;
          for (const auto& g : eg_from) {
            for (const auto& t : g.tensors) found = found || t.name == blk(il, p);
          }
        }
        if (!found) {
          throw Refused(blk(il, alts.front()) +
                        ": missing - not in the quant's shards nor in an Engram set (--engram, or <model>/../Engram/: unsloth's "
                        "UD-Q8_K_XL shard of the Engram tables)");
        }
      }
    }
    std::vector<const Gguf*> gs_all;
    std::vector<std::string> files;
    for (const auto& g : gs) {
      gs_all.push_back(&g);
      files.push_back(utf8(g.path.filename()));
    }
    for (const auto& g : eg_from) {
      gs_all.push_back(&g);
      files.push_back(rel(g.path, model));
    }

    std::vector<Entry> tensors;
    std::vector<std::string> unknown;
    static const std::regex in_layer(R"(^blk\.(\d+)\.(.+)$)");
    for (std::size_t fi = 0; fi < gs_all.size(); ++fi) {
      const Gguf& g = *gs_all[fi];
      for (const auto& t : g.tensors) {
        const auto* row = type_of(t.typ);
        if (row == nullptr) {
          throw Refused(utf8(g.path.filename()) + ": " + t.name + " has ggml type " + std::to_string(t.typ) + ", which the engine does not read");
        }
        if (t.dims.empty() || t.dims[0] % row->per != 0U) {
          throw Refused(utf8(g.path.filename()) + ": " + t.name + " rows of " + std::to_string(t.dims.empty() ? 0U : t.dims[0]) +
                        " are not whole " + row->name + " blocks");
        }
        const std::uint64_t nbytes = elements(t.dims) / row->per * row->bpb;
        std::smatch m;
        std::string ck;
        if (std::regex_match(t.name, m, in_layer) && kDs4Layer.count(m[2].str()) != 0U) {
          ck = "layers." + m[1].str() + "." + kDs4Layer.at(m[2].str());
        } else if (kDs4Global.count(t.name) != 0U) {
          ck = kDs4Global.at(t.name);
        } else {
          unknown.push_back(t.name);
          continue;
        }
        std::vector<std::uint64_t> shape(t.dims.rbegin(), t.dims.rend());   // ggml ne0 innermost -> torch order
        const std::uint64_t base = g.data_start + t.off;
        if (ends_with(ck, ".attn.wo_a.slice")) {
          // [o_groups * o_lora][in] -> o_groups entries of [o_lora][in]
          const auto n = static_cast<std::uint64_t>(as_int(get(kv, arch + ".attention.output_group_count"), "attention.output_group_count"));
          if (shape.size() == 3U && shape[0] == n) shape = {n * shape[1], shape[2]};
          if (shape.size() != 2U || n == 0U || shape[0] % n != 0U) {
            throw Refused(utf8(g.path.filename()) + ": " + t.name + " does not split in " + std::to_string(n) + " groups");
          }
          const std::uint64_t part = nbytes / n;
          for (std::uint64_t i = 0; i < n; ++i) tensors.push_back(Entry{ck + "." + std::to_string(i) + ".weight", row->name, fi, base + i * part, part, {shape[0] / n, shape[1]}});
          continue;
        }
        if (ck.find(".ffn.experts.") != std::string::npos) {
          // [n_exp][out][in] -> an entry an expert: layers.N.ffn.experts.E.w1.weight
          const std::uint64_t n = shape[0];
          const std::uint64_t part = n == 0U ? 0U : nbytes / n;
          const auto dot = ck.rfind('.');
          const std::string pre = ck.substr(0, dot);
          const std::string role = ck.substr(dot + 1);
          for (std::uint64_t i = 0; i < n; ++i) {
            tensors.push_back(Entry{pre + "." + std::to_string(i) + "." + role + ".weight", row->name, fi, base + i * part, part,
                                    std::vector<std::uint64_t>(shape.begin() + 1, shape.end())});
          }
          continue;
        }
        tensors.push_back(Entry{ck, row->name, fi, base, nbytes, shape});
      }
    }
    if (!unknown.empty()) {
      std::string list;
      for (std::size_t i = 0; i < std::min<std::size_t>(12U, unknown.size()); ++i) list += (i ? ", " : "") + unknown[i];
      throw Refused("tensors the pack does not map (" + std::to_string(unknown.size()) + "): " + list);
    }
    if (!eg_from.empty()) log << "Engram tables from " << utf8(*eg_dir) << " (" << eg_from.size() << " shards)\n";
    // (not ported: tools/ds4/drafter_pack.py's DSpark drafter and vision_pack.py's vision tower, made from DeepSeek's
    //  checkpoint, beside the model - the pack is the GGUF's, text only)
    for (const auto& [what, file] : {std::pair<const char*, fs::path>{"the DSpark drafter", model.parent_path() / "dspark" / "dspark.json"},
                                     std::pair<const char*, fs::path>{"the vision tower", model.parent_path() / "vision" / "vision.json"}}) {
      if (fs::is_regular_file(file)) log << "note: " << what << " beside the model (" << utf8(file) << ") is not packed: strata-prepare packs the GGUF set, text only\n";
    }
    const auto embed = std::find_if(tensors.begin(), tensors.end(), [](const Entry& e) { return e.name == "embed.weight"; });
    if (embed == tensors.end()) throw Refused("StopIteration: no embed.weight (token_embd)");
    const auto vocab = static_cast<std::int64_t>(embed->shape.front());
    const auto geo = ds4_geometry(kv, vocab);

    fs::create_directories(out);
    write_text(out / "ds4.txt", geometry_text(geo) + "format gguf\nmodel_dir " + utf8(model) + "\n");
    write_text(out / "tensors.txt", tensors_text(files, tensors));
    const auto [nl, cols] = write_engram(out / "engram.bin", kv, vocab);
    // the server's chat template: Maya's (pack.py's, so both formats chat alike); the GGUF's when it is not there
    std::optional<std::string> chat_template;
    if (!options.chat_template.empty() && fs::exists(options.chat_template)) chat_template = read_text_universal(options.chat_template);
    else if (const auto* t = get(kv, "tokenizer.chat_template"); t != nullptr && t->kind == MetaValue::Kind::text) chat_template = t->text;
    const auto tokens = tokens_of(kv);
    write_vocab_merges_types(out / "tokenizer", kv, tokens);
    const auto special_of = [&kv](const char* key, const std::int64_t fallback) {
      const auto* v = get(kv, key);
      return v == nullptr ? fallback : as_int(v, key);
    };
    write_text(out / "tokenizer" / "tokenizer.json",
               Json::object()
                   .set("pre", Json::text("deepseek3"))
                   .set("special_ids", Json::object()
                                           .set("bos", Json::integer(special_of("tokenizer.ggml.bos_token_id", 0)))
                                           .set("eos", Json::integer(special_of("tokenizer.ggml.eos_token_id", 1)))
                                           .set("pad", Json::integer(special_of("tokenizer.ggml.padding_token_id", 2))))
                   .dumps(1));
    if (chat_template && !chat_template->empty()) write_text(out / "tokenizer" / "chat_template.jinja", *chat_template);
    log << utf8(model.filename()) << ": " << tensors.size() << " tensors in " << files.size() << " files (" << summary_by_type(tensors)
        << "), Engram " << nl << " layers x " << cols << " hash columns -> " << utf8(out) << "\n";
    return 0;
  } catch (const std::exception& e) {
    log << e.what() << "\n";
    return 1;
  }
}

// ---- MiniMax-M3.1: tools/mm3/pack_gguf.py ------------------------------------------------------------------------

namespace {

const std::string kMm3Arch = "minimax-m3";
const std::string kMm3P = "language_model.model.";
const std::map<std::string, std::string> kMm3Layer{
    {"attn_norm.weight", "input_layernorm.weight"},
    {"ffn_norm.weight", "post_attention_layernorm.weight"},
    {"attn_q.weight", "self_attn.q_proj.weight"},
    {"attn_k.weight", "self_attn.k_proj.weight"},
    {"attn_v.weight", "self_attn.v_proj.weight"},
    {"attn_output.weight", "self_attn.o_proj.weight"},
    {"attn_q_norm.weight", "self_attn.q_norm.weight"},
    {"attn_k_norm.weight", "self_attn.k_norm.weight"},
    {"indexer.q_proj.weight", "self_attn.index_q_proj.weight"},
    {"indexer.k_proj.weight", "self_attn.index_k_proj.weight"},
    {"indexer.q_norm.weight", "self_attn.index_q_norm.weight"},
    {"indexer.k_norm.weight", "self_attn.index_k_norm.weight"},
    {"ffn_gate_inp.weight", "block_sparse_moe.gate.weight"},
    {"exp_probs_b.bias", "block_sparse_moe.e_score_correction_bias"},
    {"ffn_gate_shexp.weight", "block_sparse_moe.shared_experts.gate_proj.weight"},
    {"ffn_up_shexp.weight", "block_sparse_moe.shared_experts.up_proj.weight"},
    {"ffn_down_shexp.weight", "block_sparse_moe.shared_experts.down_proj.weight"},
    {"ffn_gate_exps.weight", "block_sparse_moe.experts.w1"},
    {"ffn_up_exps.weight", "block_sparse_moe.experts.w3"},
    {"ffn_down_exps.weight", "block_sparse_moe.experts.w2"},
    {"ffn_gate.weight", "mlp.gate_proj.weight"},
    {"ffn_up.weight", "mlp.up_proj.weight"},
    {"ffn_down.weight", "mlp.down_proj.weight"},
};
const std::map<std::string, std::string> kMm3Global{{"token_embd.weight", kMm3P + "embed_tokens.weight"},
                                                    {"output_norm.weight", kMm3P + "norm.weight"},
                                                    {"output.weight", "language_model.lm_head.weight"}};
constexpr std::array<const char*, 4> kIndexer{"indexer.q_proj.weight", "indexer.k_proj.weight", "indexer.q_norm.weight",
                                              "indexer.k_norm.weight"};
constexpr double kSwigluAlpha = 1.702;   // MiniMax-M3's (llama.cpp's LLM_FFN_SWIGLU_OAI_MOE)
constexpr double kSwigluLimit = 7.0;

Geometry mm3_geometry(const Kv& kv, const std::int64_t vocab, const std::int64_t shared_ff) {
  const auto g = [&kv](const std::string& k) -> std::optional<MetaValue> {
    const auto* v = get(kv, kMm3Arch + "." + k);
    return v == nullptr ? std::nullopt : std::optional<MetaValue>{*v};
  };
  const auto gd = [&](const std::string& k, MetaValue d) -> std::optional<MetaValue> { return g(k).value_or(std::move(d)); };
  const auto nl = as_int(get(kv, kMm3Arch + ".block_count"), "block_count");
  const auto* lead_v = get(kv, kMm3Arch + ".leading_dense_block_count");
  const std::int64_t lead = lead_v == nullptr ? 0 : as_int(lead_v, "leading_dense_block_count");
  const auto hd = as_int(get(kv, kMm3Arch + ".attention.key_length"), "attention.key_length");
  const auto rot = as_int(get(kv, kMm3Arch + ".rope.dimension_count"), "rope.dimension_count");
  if (hd == 0) throw Refused("ZeroDivisionError: attention.key_length is 0");
  const auto* shared_count = get(kv, kMm3Arch + ".expert_shared_count");
  std::vector<MetaValue> freq;
  for (std::int64_t l = 0; l < nl; ++l) freq.push_back(integer(l < lead ? 0 : 1));
  const MetaValue ones = list_of(std::vector<MetaValue>(static_cast<std::size_t>(std::max<std::int64_t>(0, nl)), integer(1)));
  return Geometry{
      {"hidden_size", g("embedding_length")},
      {"num_hidden_layers", integer(nl)},
      {"num_attention_heads", g("attention.head_count")},
      {"num_key_value_heads", g("attention.head_count_kv")},
      {"head_dim", integer(hd)},
      {"rotary_dim", integer(rot)},
      {"partial_rotary_factor", real(static_cast<double>(rot) / static_cast<double>(hd))},
      {"vocab_size", integer(vocab)},
      {"num_local_experts", g("expert_count")},
      {"num_experts_per_tok", g("expert_used_count")},
      {"intermediate_size", g("expert_feed_forward_length")},
      {"dense_intermediate_size", g("feed_forward_length")},
      {"shared_intermediate_size", integer(shared_ff * (shared_count == nullptr ? 1 : as_int(shared_count, "expert_shared_count")))},
      {"rope_theta", g("rope.freq_base")},
      {"rms_norm_eps", g("attention.layer_norm_rms_epsilon")},
      {"routed_scaling_factor", gd("expert_weights_scale", real(1.0))},
      {"swiglu_alpha", gd("swiglu_alpha", real(kSwigluAlpha))},
      {"swiglu_limit", gd("swiglu_limit", real(kSwigluLimit))},
      {"max_position_embeddings", g("context_length")},
      {"moe_layer_freq", list_of(std::move(freq))},
      {"sparse_attention_config.sparse_attention_freq", ones},
      {"sparse_attention_config.sparse_disable_index_value", ones},
      {"sparse_attention_config.sparse_num_index_heads", g("attention.indexer.head_count")},
      {"sparse_attention_config.sparse_index_dim", g("attention.indexer.key_length")},
      {"sparse_attention_config.sparse_topk_blocks", g("attention.indexer.top_k")},
      {"sparse_attention_config.sparse_block_size", g("attention.indexer.block_size")},
      {"sparse_attention_config.sparse_local_block", gd("attention.indexer.local_blocks", integer(1))},
      {"sparse_attention_config.sparse_init_block", integer(0)},
      {"norms_plus_one", integer(1)},
  };
}

// `if recipe.get("tier")` of json.loads(kv.get("maya.recipe", "{}")): the tier's str() when it is truthy (a non-empty
// string, a non-zero number, true), else nullopt - also for text that is not a JSON object (json.loads's ValueError)
std::optional<std::string> recipe_tier(const std::string& json) {
  std::size_t i = 0;
  const auto ws = [&] {
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) ++i;
  };
  // a JSON string at i (without escapes beyond \" \\ \/ and \uXXXX of ASCII), or nullopt
  const auto string_at = [&]() -> std::optional<std::string> {
    if (i >= json.size() || json[i] != '"') return std::nullopt;
    std::string s;
    for (++i; i < json.size() && json[i] != '"'; ++i) {
      if (json[i] == '\\' && i + 1 < json.size()) {
        const char e = json[++i];
        if (e == 'u' && i + 4 < json.size()) {
          s.push_back(static_cast<char>(std::stoi(json.substr(i + 1, 4), nullptr, 16) & 0x7f));
          i += 4;
        } else {
          s.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e);
        }
      } else {
        s.push_back(json[i]);
      }
    }
    if (i >= json.size()) return std::nullopt;
    ++i;
    return s;
  };
  // skips one value (nested objects and arrays by bracket depth, strings honoured)
  const auto skip_value = [&]() -> bool {
    ws();
    if (i < json.size() && json[i] == '"') return string_at().has_value();
    int depth = 0;
    for (; i < json.size(); ++i) {
      const char c = json[i];
      if (c == '"') {
        if (!string_at()) return false;
        --i;
        continue;
      }
      if (c == '{' || c == '[') ++depth;
      else if (c == '}' || c == ']') {
        if (depth == 0) return true;
        --depth;
        if (depth == 0) {
          ++i;
          return true;
        }
      } else if (c == ',' && depth == 0) {
        return true;
      }
    }
    return depth == 0;
  };
  ws();
  if (i >= json.size() || json[i] != '{') return std::nullopt;
  ++i;
  std::optional<std::string> tier;
  for (;;) {
    ws();
    if (i < json.size() && json[i] == '}') break;
    const auto key = string_at();
    ws();
    if (!key || i >= json.size() || json[i] != ':') return std::nullopt;
    ++i;
    ws();
    if (*key == "tier") {
      const std::size_t start = i;
      if (i < json.size() && json[i] == '"') {
        tier = string_at();
        if (tier && tier->empty()) tier.reset();   // "" is falsy
      } else {
        if (!skip_value()) return std::nullopt;
        std::string raw = json.substr(start, i - start);
        while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\n' || raw.back() == '\r' || raw.back() == '\t')) raw.pop_back();
        char* end = nullptr;
        const double number = std::strtod(raw.c_str(), &end);
        const bool numeric = !raw.empty() && end == raw.c_str() + raw.size();
        if (raw == "true") tier = "True";
        else if (raw == "false" || raw == "null" || raw == "{}" || raw == "[]" || (numeric && number == 0.0)) tier.reset();
        else tier = raw;   // (a number as written; a nested value as JSON)
      }
    } else if (!skip_value()) {
      return std::nullopt;
    }
    ws();
    if (i < json.size() && json[i] == ',') {
      ++i;
      continue;
    }
    if (i < json.size() && json[i] == '}') break;
    return std::nullopt;
  }
  return tier;
}

}  // namespace

int maya_mm3_pack(const MoePackOptions& options, std::ostream& log) {
  try {
    const fs::path model = resolved(options.model);
    const fs::path out = options.out.empty() ? model / "maya-mm3" : options.out;
    // find_shards: the main set's shards and the indexers' side file
    const auto paths_all = gguf_files(model, options.shards);
    if (paths_all.empty()) throw Refused(utf8(model) + ": no .gguf files");
    for (const auto& p : paths_all) {
      if (!fs::is_regular_file(p)) throw Refused(utf8(p) + ": not there");
    }
    std::vector<fs::path> paths;
    std::optional<fs::path> side;
    for (const auto& p : paths_all) {
      if (ends_with(utf8(p.filename()), "-maya-indexers.gguf")) {
        if (!side) side = p;
      } else {
        paths.push_back(p);
      }
    }
    if (!options.indexers.empty()) {
      side = resolved(options.indexers);
      if (!fs::is_regular_file(*side)) throw Refused(utf8(*side) + ": not there (--indexers)");
    }
    std::vector<Gguf> gs;
    for (const auto& p : paths) gs.push_back(read_gguf(p));
    const auto with_arch = std::find_if(gs.begin(), gs.end(), [](const Gguf& g) { return g.kv.count("general.architecture") != 0U; });
    if (with_arch == gs.end() || text_of(with_arch->kv, "general.architecture") != kMm3Arch) {
      const std::string arch = with_arch == gs.end() ? "None" : python_repr(text_of(with_arch->kv, "general.architecture"));
      throw Refused(utf8(model) + ": not a MiniMax-M3 GGUF (architecture " + arch + ")");
    }
    const Kv kv = with_arch->kv;
    check_split(model, paths, gs.size(), kv);
    for (const auto& g : gs) {
      if (const auto why = short_of(g)) throw Refused(utf8(model) + ": not whole (" + *why + ") - a download still running?");
    }
    std::optional<Gguf> sg;
    if (side) {
      sg = read_gguf(*side);
      if (text_of(sg->kv, "general.architecture") != kMm3Arch || short_of(*sg)) throw Refused(utf8(*side) + ": not a whole MiniMax-M3 indexers file");
      const std::string for_ = text_of(sg->kv, "maya.indexers.for");
      std::string name = text_of(kv, "general.name");
      std::replace(name.begin(), name.end(), ' ', '-');
      if (!for_.empty() && !name.empty() && for_ != name) log << "note: " << utf8(side->filename()) << " says it is for " << for_ << " (the set: " << name << ")\n";
    }
    std::vector<std::string> files;
    for (const auto& p : paths) files.push_back(rel(p, model));
    if (sg) files.push_back(rel(*side, model));
    for (const auto& f : files) {
      if (f.find(' ') != std::string::npos) throw Refused("a file name with a space: " + f);
    }
    const auto n_exp = static_cast<std::uint64_t>(as_int(get(kv, kMm3Arch + ".expert_count"), "expert_count"));
    const auto nl = as_int(get(kv, kMm3Arch + ".block_count"), "block_count");
    std::vector<Entry> tensors;
    std::vector<std::string> unknown;
    std::set<std::pair<std::int64_t, std::string>> have;
    std::vector<const Gguf*> all;
    for (const auto& g : gs) all.push_back(&g);
    if (sg) all.push_back(&*sg);
    static const std::regex in_layer(R"(^blk\.(\d+)\.(.+)$)");
    for (std::size_t fi = 0; fi < all.size(); ++fi) {
      const Gguf& g = *all[fi];
      for (const auto& t : g.tensors) {
        const auto* row = type_of(t.typ);
        if (row == nullptr) {
          throw Refused(utf8(g.path.filename()) + ": " + t.name + " has ggml type " + std::to_string(t.typ) + ", which the engine does not read");
        }
        if (t.dims.empty() || t.dims[0] % row->per != 0U) {
          throw Refused(utf8(g.path.filename()) + ": " + t.name + " rows of " + std::to_string(t.dims.empty() ? 0U : t.dims[0]) +
                        " are not whole " + row->name + " blocks");
        }
        const std::uint64_t nbytes = elements(t.dims) / row->per * row->bpb;
        std::smatch m;
        std::string ck;
        if (std::regex_match(t.name, m, in_layer) && kMm3Layer.count(m[2].str()) != 0U) {
          ck = kMm3P + "layers." + m[1].str() + "." + kMm3Layer.at(m[2].str());
          have.insert({std::stoll(m[1].str()), m[2].str()});
        } else if (kMm3Global.count(t.name) != 0U) {
          ck = kMm3Global.at(t.name);
        } else {
          unknown.push_back(t.name);
          continue;
        }
        std::vector<std::uint64_t> shape(t.dims.rbegin(), t.dims.rend());
        const std::uint64_t base = g.data_start + t.off;
        if (ends_with(ck, ".experts.w1") || ends_with(ck, ".experts.w2") || ends_with(ck, ".experts.w3")) {
          if (shape[0] != n_exp) {
            throw Refused(utf8(g.path.filename()) + ": " + t.name + " holds " + std::to_string(shape[0]) + " experts, not " + std::to_string(n_exp));
          }
          const std::uint64_t part = n_exp == 0U ? 0U : nbytes / n_exp;
          const auto dot = ck.rfind('.');
          for (std::uint64_t i = 0; i < n_exp; ++i) {
            tensors.push_back(Entry{ck.substr(0, dot) + "." + std::to_string(i) + "." + ck.substr(dot + 1) + ".weight", row->name, fi,
                                    base + i * part, part, std::vector<std::uint64_t>(shape.begin() + 1, shape.end())});
          }
          continue;
        }
        tensors.push_back(Entry{ck, row->name, fi, base, nbytes, shape});
      }
    }
    if (!unknown.empty()) {
      std::string list;
      for (std::size_t i = 0; i < std::min<std::size_t>(12U, unknown.size()); ++i) list += (i ? ", " : "") + unknown[i];
      throw Refused("tensors the pack does not map (" + std::to_string(unknown.size()) + "): " + list);
    }
    std::vector<std::int64_t> lacking;
    for (std::int64_t li = 0; li < nl; ++li) {
      bool all_there = true;
      for (const char* n : kIndexer) all_there = all_there && have.count({li, n}) != 0U;
      if (!all_there) lacking.push_back(li);
    }
    if (!lacking.empty()) {
      std::string list;
      for (std::size_t i = 0; i < std::min<std::size_t>(8U, lacking.size()); ++i) list += (i ? ", " : "") + std::to_string(lacking[i]);
      throw Refused("the sparse-attention indexer of layer" + std::string{lacking.size() > 1U ? "s" : ""} + " " + list + " is not in the set" +
                    (sg ? " nor in " + utf8(side->filename()) : std::string{}) +
                    ": MiniMax-M3.1 runs it in every layer. The Maya sets keep layers 0-2's in <name>-maya-indexers.gguf beside the "
                    "shards - download it into " + utf8(model) + " (or name it with --indexers)");
    }
    // (not ported: the EXL3 repo's vision tower and DSpark draft beside the model - the pack is the GGUF set's, text only)
    for (const char* companions : {"MiniMax-M3.1-Flash-companions", "MiniMax-M3.1-Flash-EXL3-3.5bpw"}) {
      if (fs::is_regular_file(model.parent_path() / companions / "config.json")) {
        log << "note: " << utf8(model.parent_path() / companions) << " is not packed: strata-prepare packs the GGUF set, text only\n";
      }
    }
    const auto embd = std::find_if(tensors.begin(), tensors.end(), [](const Entry& e) { return e.name == kMm3Global.at("token_embd.weight"); });
    if (embd == tensors.end()) throw Refused("StopIteration: no token_embd.weight");
    const auto shexp = std::find_if(tensors.begin(), tensors.end(), [](const Entry& e) { return ends_with(e.name, "shared_experts.gate_proj.weight"); });
    const auto geo = mm3_geometry(kv, static_cast<std::int64_t>(embd->shape.front()),
                                  shexp == tensors.end() ? 0 : static_cast<std::int64_t>(shexp->shape.front()));

    fs::create_directories(out);
    // write_tokenizer: pack.py's layout, pre "minimax", Maya's chat template
    const auto tokens = tokens_of(kv);
    {
      std::unordered_set<std::string> unique(tokens.begin(), tokens.end());
      if (unique.size() != tokens.size()) throw Refused("the GGUF's vocabulary has duplicate token strings");
    }
    const auto* nfc = get(kv, "tokenizer.ggml.normalizer.nfc");
    if (text_of(kv, "tokenizer.ggml.model") != "gpt2" || (nfc != nullptr && !python_truthy(*nfc))) {
      throw Refused("the GGUF's tokenizer is not the NFC byte-level BPE the `minimax` scheme transcribes");
    }
    const auto bos = as_int(get(kv, "tokenizer.ggml.bos_token_id"), "tokenizer.ggml.bos_token_id");
    const auto eos = as_int(get(kv, "tokenizer.ggml.eos_token_id"), "tokenizer.ggml.eos_token_id");
    if (options.chat_template.empty()) throw Refused("Maya's MiniMax chat template (tools/mm3/chat_template.jinja) is not named");
    const std::string chat_template = read_text_universal(options.chat_template);
    write_vocab_merges_types(out / "tokenizer", kv, tokens);
    write_text(out / "tokenizer" / "tokenizer.json",
               Json::object()
                   .set("pre", Json::text("minimax"))
                   .set("special_ids", Json::object().set("bos", Json::integer(bos)).set("eos", Json::integer(eos)))
                   .dumps(1));
    write_text(out / "tokenizer" / "chat_template.jinja", chat_template);
    std::set<std::string> dense;
    std::map<std::string, int> exp_types;   // (each layer and role once: its expert 0's entry)
    for (const auto& t : tensors) {
      if (t.name.find(".experts.") == std::string::npos && t.dtype != "F32") dense.insert(t.dtype);
      if (t.name.find(".experts.0.") != std::string::npos) ++exp_types[t.dtype];
    }
    std::string text = geometry_text(geo);
    text += "bos_token_id " + std::to_string(bos) + "\neos_token_id " + std::to_string(eos) + "\n";
    text += "special.bos " + std::to_string(bos) + "\nspecial.eos " + std::to_string(eos) + "\n";
    text += "format gguf\n";
    std::string quant = get(kv, "general.name") != nullptr ? py_str(*get(kv, "general.name")) : utf8(model.filename());
    std::replace(quant.begin(), quant.end(), ' ', '-');
    text += "quant_name " + quant + "\n";
    if (const auto* recipe = get(kv, "maya.recipe"); recipe != nullptr && recipe->kind == MetaValue::Kind::text) {
      if (const auto tier = recipe_tier(recipe->text)) text += "maya.tier " + *tier + "\n";
    }
    std::string dense_list;
    for (const auto& d : dense) dense_list += (dense_list.empty() ? "" : ",") + d;
    text += "gguf.dense " + dense_list + "\n";
    std::string experts_list;
    for (const auto& [k, v] : exp_types) experts_list += (experts_list.empty() ? "" : ",") + k + "x" + std::to_string(v);
    text += "gguf.experts " + experts_list + "\n";
    text += "gguf.indexers " + (sg ? rel(*side, model) : std::string{"-"}) + "\n";
    text += "model_dir " + utf8(model) + "\n";
    write_lf(out / "mm3.txt", text);
    write_lf(out / "tensors.txt", tensors_text(files, tensors));
    log << utf8(model.filename()) << ": " << tensors.size() << " tensors in " << files.size() << " files (" << summary_by_type(tensors) << ")"
        << (sg ? ", the dense layers' indexers from " + utf8(side->filename()) : std::string{}) << " -> " << utf8(out) << "\n";
    log << "  experts (layer x role): ";
    bool first = true;
    for (const auto& [k, v] : exp_types) {
      log << (first ? "" : ", ") << k << " " << v;
      first = false;
    }
    log << "\n  special ids: {'bos': " << bos << ", 'eos': " << eos << "}\n  vision tower: none (text only); DSpark draft: none\n";
    return 0;
  } catch (const std::exception& e) {
    log << e.what() << "\n";
    return 1;
  }
}

}  // namespace strata_prepare
