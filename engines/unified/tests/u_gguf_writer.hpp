// Test-only: builds GGUF v3 images in memory (metadata, tensor directory, zero or given data).
// Mirrored from engines/gemma4/tests/gguf_writer.hpp.
#pragma once

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace u_test {

class GgufBuilder final {
 public:
  void u32(const std::string& key, std::uint32_t v) { begin(key, 4); put32(v); ++kv_; }
  void i32(const std::string& key, std::int32_t v) { begin(key, 5); put32(static_cast<std::uint32_t>(v)); ++kv_; }
  void f32(const std::string& key, float v) { begin(key, 6); putf(v); ++kv_; }
  void boolean(const std::string& key, bool v) { begin(key, 7); meta_.push_back(v ? 1 : 0); ++kv_; }
  void str(const std::string& key, const std::string& v) { begin(key, 8); put_string(v); ++kv_; }
  void u64(const std::string& key, std::uint64_t v) { begin(key, 10); put64(v); ++kv_; }
  void f64(const std::string& key, double v) { begin(key, 12); putd(v); ++kv_; }
  void arr_i32(const std::string& key, const std::vector<std::int32_t>& v) {
    begin(key, 9); put32(5); put64(v.size());
    for (auto x : v) put32(static_cast<std::uint32_t>(x));
    ++kv_;
  }
  void arr_u32(const std::string& key, const std::vector<std::uint32_t>& v) {
    begin(key, 9); put32(4); put64(v.size());
    for (auto x : v) put32(x);
    ++kv_;
  }
  void arr_bool(const std::string& key, const std::vector<bool>& v) {
    begin(key, 9); put32(7); put64(v.size());
    for (bool x : v) meta_.push_back(x ? 1 : 0);
    ++kv_;
  }
  void arr_f32(const std::string& key, const std::vector<float>& v) {
    begin(key, 9); put32(6); put64(v.size());
    for (float x : v) putf(x);
    ++kv_;
  }
  void arr_str(const std::string& key, const std::vector<std::string>& v) {
    begin(key, 9); put32(8); put64(v.size());
    for (const auto& s : v) put_string(s);
    ++kv_;
  }
  // Raw access for malformed images.
  std::vector<std::uint8_t>& raw_meta() { return meta_; }
  void bump_kv(std::uint64_t n = 1) { kv_ += n; }

  // A tensor whose data is `bytes` long (zero-filled unless `data` is given).
  void tensor(const std::string& name, const std::vector<std::uint64_t>& dims, std::uint32_t type, std::uint64_t bytes,
              const std::vector<std::uint8_t>& data = {}) {
    Tensor t{name, dims, type, bytes, data, std::uint64_t(-1)};
    tensors_.push_back(t);
  }
  // Overrides the offset written for the last tensor (malformed images).
  void last_offset(std::uint64_t off) { tensors_.back().forced_offset = off; }

  // The bytes the data section needs (each tensor padded to the alignment).
  [[nodiscard]] std::uint64_t data_size(std::uint64_t alignment = 32) const {
    std::uint64_t at = 0;
    for (const auto& t : tensors_) at += (t.bytes + alignment - 1) / alignment * alignment;
    return at;
  }

  // header_only: stop after the (padded) tensor directory; the caller passes the whole
  // file's size (header + data_size()) to the parser.
  [[nodiscard]] std::vector<std::uint8_t> bytes(std::uint64_t alignment = 32, bool header_only = false) const {
    std::vector<std::uint8_t> out;
    auto p32 = [&out](std::uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i))); };
    auto p64 = [&out](std::uint64_t v) { for (int i = 0; i < 8; ++i) out.push_back(std::uint8_t(v >> (8 * i))); };
    out.insert(out.end(), {'G', 'G', 'U', 'F'});
    p32(3);
    p64(tensors_.size());
    p64(kv_);
    out.insert(out.end(), meta_.begin(), meta_.end());
    std::vector<std::uint64_t> offsets;
    std::uint64_t at = 0;
    for (const auto& t : tensors_) {
      offsets.push_back(at);
      at += (t.bytes + alignment - 1) / alignment * alignment;
    }
    for (std::size_t i = 0; i < tensors_.size(); ++i) {
      const auto& t = tensors_[i];
      p64(t.name.size());
      out.insert(out.end(), t.name.begin(), t.name.end());
      p32(static_cast<std::uint32_t>(t.dims.size()));
      for (auto d : t.dims) p64(d);
      p32(t.type);
      p64(t.forced_offset != std::uint64_t(-1) ? t.forced_offset : offsets[i]);
    }
    while (out.size() % alignment != 0) out.push_back(0);
    if (header_only) return out;
    for (std::size_t i = 0; i < tensors_.size(); ++i) {
      const auto& t = tensors_[i];
      const std::size_t base = out.size();
      out.resize(base + static_cast<std::size_t>((t.bytes + alignment - 1) / alignment * alignment), 0);
      if (!t.data.empty()) std::memcpy(out.data() + base, t.data.data(), std::min<std::size_t>(t.data.size(), t.bytes));
    }
    return out;
  }

 private:
  struct Tensor {
    std::string name;
    std::vector<std::uint64_t> dims;
    std::uint32_t type;
    std::uint64_t bytes;
    std::vector<std::uint8_t> data;
    std::uint64_t forced_offset;
  };
  void put32(std::uint32_t v) { for (int i = 0; i < 4; ++i) meta_.push_back(std::uint8_t(v >> (8 * i))); }
  void put64(std::uint64_t v) { for (int i = 0; i < 8; ++i) meta_.push_back(std::uint8_t(v >> (8 * i))); }
  void putf(float v) { std::uint32_t uval; std::memcpy(&uval, &v, 4); put32(uval); }
  void putd(double v) { std::uint64_t uval; std::memcpy(&uval, &v, 8); put64(uval); }
  void put_string(const std::string& s) { put64(s.size()); meta_.insert(meta_.end(), s.begin(), s.end()); }
  void begin(const std::string& key, std::uint32_t type) { put_string(key); put32(type); }

  std::vector<std::uint8_t> meta_;
  std::uint64_t kv_{0};
  std::vector<Tensor> tensors_;
};

}  // namespace u_test
