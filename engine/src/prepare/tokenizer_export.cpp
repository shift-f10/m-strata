// strata-prepare tokenizer: Project Maya's tools/strata_tokenizer.py (v1.1.30) with our raon pre-tokenizer added to it,
// Tokenizer.from_gguf, the checks of Tokenizer.__init__ and extract(), in their order; the Python module is the
// reference (tools/secure/strata_prepare_parity.py).
#include "tokenizer_export.hpp"

#include "gguf_header.hpp"
#include "pack_common.hpp"
#include "py_json.hpp"

#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace strata_prepare {
namespace fs = std::filesystem;
namespace {

// The pre-tokenizer patterns extract() ships (transcribed in strata_tokenizer.py from llama.cpp's llama-vocab.cpp).
const char* const kQwen35Pattern =
    R"x((?:'[sS]|'[tT]|'[rR][eE]|'[vV][eE]|'[mM]|'[lL][lL]|'[dD])|[^\r\n\p{L}\p{N}]?[\p{L}\p{M}]+|\p{N}| ?[^\s\p{L}\p{M}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+)x";
const char* const kGlmPattern =
    R"x((?:'[sS]|'[tT]|'[rR][eE]|'[vV][eE]|'[mM]|'[lL][lL]|'[dD])|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}{1,3}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+)x";
// raon (Raon-1-mini), as our addition to strata_tokenizer.py ships it (RAON_PATTERN; from ik_llama.cpp PR #2733,
// LLAMA_VOCAB_PRE_TYPE_RAON): standard BPE, no ignore_merges, so its source line stays QWEN35's as the Python writes it.
// The Python ships the CJK class as escape text (a backslash, "u" and four hex digits, not the characters), so it is
// written here with escaped backslashes.
const char* const kRaonPattern =
    " ?[\\u4e00-\\u9fa5\\u3040-\\u309f\\u30a0-\\u30ff]+| ?(?:(?:(?![\\u4e00-\\u9fa5\\u3040-\\u309f\\u30a0-\\u30ff])" R"x(\p{L})\p{M}*)+)x"
    "(?: (?:(?:(?![\\u4e00-\\u9fa5\\u3040-\\u309f\\u30a0-\\u30ff])"
    R"x(\p{L})\p{M}*)+)*|\p{N}{1,3}(?=(?:\p{N}{3})*(?!\p{N}))| ?[^\s\p{L}\p{N}]+[\r\n/]*|\s*[\r\n]+|\s+(?!\S)|\s+)x";
const char* const kGlmSource =
    ".ref/llama.cpp src/llama-vocab.cpp L406 (LLAMA_VOCAB_PRE_TYPE_CHATGLM4; glm4/glm5 set ignore_merges)";
const char* const kQwen35Source = ".ref/llama.cpp src/llama-vocab.cpp L396 (LLAMA_VOCAB_PRE_TYPE_QWEN35)";

struct Refused : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// repr() of a metadata value where the Python messages show one (None for a missing one)
std::string repr_of(const MetaValue* v) {
  if (v == nullptr) return "None";
  if (v->kind == MetaValue::Kind::text) return python_repr(v->text);
  if (v->kind == MetaValue::Kind::integer) return std::to_string(v->integer);
  if (v->kind == MetaValue::Kind::real) return python_float_repr(v->real);
  return "[...]";
}

// list(md[key]) of a list of strings
std::vector<std::string> strings_of(const MetaValue& v, const std::string& key) {
  if (v.kind != MetaValue::Kind::list) throw Refused(key + " is not a list");
  std::vector<std::string> out;
  out.reserve(v.items.size());
  for (const auto& item : v.items) {
    if (item.kind != MetaValue::Kind::text) throw Refused(key + " holds a value that is not a string");
    out.push_back(item.text);
  }
  return out;
}

struct PairHash {
  std::size_t operator()(const std::pair<std::string, std::string>& p) const noexcept {
    const std::hash<std::string> h;
    return h(p.first) ^ (h(p.second) * 1000003U);
  }
};

}  // namespace

int export_tokenizer(const fs::path& gguf, const fs::path& out_dir, std::ostream& log) {
  try {
    // extract(): out.mkdir(parents=True, exist_ok=True) comes first
    const fs::path out = out_dir / "tokenizer";
    fs::create_directories(out);
    const GgufHeader g(gguf);
    // ---- Tokenizer.from_gguf
    std::string missing;
    for (const char* key : {"tokenizer.ggml.tokens", "tokenizer.ggml.merges"}) {
      if (g.get(key) == nullptr) missing += (missing.empty() ? "" : ", ") + std::string{key};
    }
    if (!missing.empty()) throw Refused("GGUF is missing " + missing + "; it does not carry a tokenizer");
    const MetaValue* model = g.get("tokenizer.ggml.model");
    if (model == nullptr || model->kind != MetaValue::Kind::text || model->text != "gpt2") {
      throw Refused("expected a byte-level BPE tokenizer (model 'gpt2'), got " + repr_of(model));
    }
    Json special = Json::object();   // {k: int(md[k]) ...}: the file's key order
    std::string special_repr;
    for (const auto& key : g.keys) {
      if (!starts_with(key, "tokenizer.ggml.") || !ends_with(key, "_token_id")) continue;
      const auto id = python_int(g.metadata.at(key));
      if (!id) throw Refused(key + ": not an integer");
      special.set(key, Json::integer(*id));
      special_repr += (special_repr.empty() ? "" : ", ") + python_repr(key) + ": " + std::to_string(*id);
    }
    const std::vector<std::string> tokens = strings_of(*g.get("tokenizer.ggml.tokens"), "tokenizer.ggml.tokens");
    const std::vector<std::string> merges = strings_of(*g.get("tokenizer.ggml.merges"), "tokenizer.ggml.merges");
    // list(md.get("tokenizer.ggml.token_type") or []) or None
    std::optional<std::vector<std::int64_t>> token_types;
    if (const MetaValue* types = g.get("tokenizer.ggml.token_type"); types != nullptr && python_truthy(*types)) {
      if (types->kind != MetaValue::Kind::list) throw Refused("tokenizer.ggml.token_type is not a list");
      token_types.emplace();
      for (const auto& item : types->items) {
        const auto v = python_int(item);
        if (!v || item.kind != MetaValue::Kind::integer) throw Refused("tokenizer.ggml.token_type holds a value that is not an integer");
        token_types->push_back(*v);
      }
    }
    const MetaValue* pre_value = g.get("tokenizer.ggml.pre");
    const std::string pre = pre_value == nullptr ? std::string{"qwen35"} : pre_value->kind == MetaValue::Kind::text ? pre_value->text : std::string{};
    // ---- Tokenizer.__init__'s checks
    if (pre_value != nullptr && pre_value->kind != MetaValue::Kind::text) {
      throw Refused("the " + repr_of(pre_value) + " pre-tokenizer is not transcribed; known: qwen35, glm4, glm5, deepseek3, minimax, raon");
    }
    if (pre != "qwen35" && pre != "glm4" && pre != "glm5" && pre != "deepseek3" && pre != "minimax" && pre != "raon") {
      throw Refused("the " + python_repr(pre) + " pre-tokenizer is not transcribed; known: qwen35, glm4, glm5, deepseek3, minimax, raon");
    }
    const bool ignore_merges = pre == "glm4" || pre == "glm5";
    std::unordered_map<std::string, std::size_t> ids;
    ids.reserve(tokens.size());
    for (std::size_t i = 0; i < tokens.size(); ++i) ids[tokens[i]] = i;
    if (ids.size() != tokens.size()) {
      throw Refused("vocabulary has duplicate tokens: " + std::to_string(tokens.size()) + " entries, " + std::to_string(ids.size()) + " unique");
    }
    // ranks[(left, right)] = i: a repeated pair takes its last rank (the dict keeps its first place, merges.txt sorts by rank)
    std::unordered_map<std::pair<std::string, std::string>, std::size_t, PairHash> ranks;
    ranks.reserve(merges.size());
    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.reserve(merges.size());
    for (std::size_t i = 0; i < merges.size(); ++i) {
      const auto& m = merges[i];
      const auto space = m.find(' ');
      if (space == std::string::npos || m.find(' ', space + 1) != std::string::npos) {
        throw Refused("merge " + std::to_string(i) + " is not a pair: " + python_repr(m));
      }
      std::pair<std::string, std::string> pair{m.substr(0, space), m.substr(space + 1)};
      if (ids.count(pair.first) == 0U || ids.count(pair.second) == 0U) {
        throw Refused("merge " + std::to_string(i) + " names a token outside the vocabulary: " + python_repr(m));
      }
      ranks[pair] = i;
      pairs.push_back(std::move(pair));
    }
    if (token_types) {
      for (std::size_t i = 0; i < token_types->size(); ++i) {
        const auto ty = (*token_types)[i];
        if ((ty == 3 || ty == 4) && i >= tokens.size()) throw Refused("token_type " + std::to_string(i) + " has no token");
      }
    }
    // ---- extract()
    // vocab.json: json.dumps(tk.ids, ensure_ascii=False) - the tokens in order (none repeats)
    std::string vocab = "{";
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      if (i != 0U) vocab += ", ";
      vocab += python_json_string(tokens[i], false) + ": " + std::to_string(i);
    }
    write_text(out / "vocab.json", vocab + "}");
    // merges.txt: "\n".join("%s %s" % k for k, _ in sorted(tk.ranks.items(), key=rank))
    std::string merged;
    std::size_t n_merges = 0;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
      if (ranks.at(pairs[i]) != i) continue;   // listed again later: it sorts there
      merged += (n_merges++ != 0U ? "\n" : "") + pairs[i].first + " " + pairs[i].second;
    }
    write_text(out / "merges.txt", merged);
    // token_type.json: json.dumps(tk.token_types)
    std::string types = "null";
    if (token_types) {
      types = "[";
      for (std::size_t i = 0; i < token_types->size(); ++i) types += (i ? ", " : "") + std::to_string((*token_types)[i]);
      types += "]";
    }
    write_text(out / "token_type.json", types);
    const Json cfg = Json::object()
                         .set("model", Json::text("gpt2"))
                         .set("pre", Json::text(pre))
                         .set("vocab_size", Json::integer(static_cast<std::int64_t>(tokens.size())))
                         .set("n_merges", Json::integer(static_cast<std::int64_t>(n_merges)))
                         .set("special_ids", special)
                         .set("add_bos_token", Json::boolean(false))
                         .set("ignore_merges", Json::boolean(ignore_merges))
                         .set("pre_pattern", Json::text(pre == "raon" ? kRaonPattern : ignore_merges ? kGlmPattern : kQwen35Pattern))
                         .set("pre_pattern_source", Json::text(ignore_merges ? kGlmSource : kQwen35Source));
    write_text(out / "tokenizer.json", cfg.dumps(1, false));
    // the model's own chat template, when it has one: written as it is (newline="\n")
    if (const MetaValue* tpl = g.get("tokenizer.chat_template"); tpl != nullptr && python_truthy(*tpl)) {
      if (tpl->kind != MetaValue::Kind::text) throw Refused("tokenizer.chat_template is not a string");
      write_lf(out / "chat_template.jinja", tpl->text);
    }
    // main()'s report
    log << "tokenizer/: vocab " << tokens.size() << ", merges " << n_merges << ", pre " << pre << ", specials {" << special_repr << "}\n";
    return 0;
  } catch (const std::exception& e) {
    log << e.what() << "\n";
    return 1;
  }
}

}  // namespace strata_prepare
