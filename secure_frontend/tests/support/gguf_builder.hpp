#pragma once

// GUI-first start-up (spec 2026-10-27 §4.2): GGUF headers built in memory for the tests - the magic, the version, the
// counts, then each key, its type and its value - and one carrying a tokenizer pack's vocabulary, as a model file the
// page can add (its tokenizer is read from the header).

#include "secure_frontend/tokenizer_pack.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace test_support {

class GgufBuilder final {
 public:
  explicit GgufBuilder(const std::uint32_t version = 3U, const std::uint64_t tensors = 0U) : version_(version), tensors_(tensors) {}
  GgufBuilder& u32(const std::string& key, const std::uint32_t v) { return add(key, 4U, pod(v)); }
  GgufBuilder& i32(const std::string& key, const std::int32_t v) { return add(key, 5U, pod(v)); }
  GgufBuilder& f32(const std::string& key, const float v) { return add(key, 6U, pod(v)); }
  GgufBuilder& boolean(const std::string& key, const bool v) { return add(key, 7U, std::string(1, v ? '\1' : '\0')); }
  GgufBuilder& str(const std::string& key, const std::string& v) { return add(key, 8U, string(v)); }
  GgufBuilder& strings(const std::string& key, const std::vector<std::string>& v) {
    std::string body = pod(std::uint32_t{8}) + pod(static_cast<std::uint64_t>(v.size()));
    for (const auto& s : v) body += string(s);
    return add(key, 9U, body);
  }
  GgufBuilder& i32s(const std::string& key, const std::vector<std::int32_t>& v) {
    std::string body = pod(std::uint32_t{5}) + pod(static_cast<std::uint64_t>(v.size()));
    for (const auto i : v) body += pod(i);
    return add(key, 9U, body);
  }
  GgufBuilder& raw(const std::string& key, const std::uint32_t type, const std::string& body) { return add(key, type, body); }
  [[nodiscard]] std::string bytes(const std::string& after = "") const {
    return pod(std::uint32_t{0x46554747U}) + pod(version_) + pod(tensors_) + pod(count_) + kvs_ + after;
  }
  template <typename T>
  static std::string pod(const T value) {
    std::string out(sizeof(T), '\0');
    std::memcpy(out.data(), &value, sizeof(T));
    return out;
  }
  static std::string string(const std::string& value) { return pod(static_cast<std::uint64_t>(value.size())) + value; }

 private:
  GgufBuilder& add(const std::string& key, const std::uint32_t type, const std::string& body) {
    kvs_ += string(key) + pod(type) + body;
    ++count_;
    return *this;
  }
  std::uint32_t version_;
  std::uint64_t tensors_;
  std::uint64_t count_{0};
  std::string kvs_;
};

// A model file's header: its architecture and `pack` as its tokenizer (tokenizer.ggml.model `model`: gpt2 for the
// byte modes, gemma4 for spm_bpe), its tokens, types and merges.
inline GgufBuilder gguf_with_tokenizer(const secure_frontend::TokenizerPack& pack, const std::string& model, const std::string& arch) {
  std::vector<std::int32_t> types;
  for (const int type : pack.token_types) types.push_back(static_cast<std::int32_t>(type));
  std::vector<std::string> merges;
  for (const auto& [left, right] : pack.merges_by_rank) merges.push_back(left + " " + right);
  GgufBuilder builder;
  builder.str("general.architecture", arch)
      .str("tokenizer.ggml.model", model)
      .strings("tokenizer.ggml.tokens", pack.tokens_by_id)
      .i32s("tokenizer.ggml.token_type", types)
      .strings("tokenizer.ggml.merges", merges);
  return builder;
}

}  // namespace test_support
