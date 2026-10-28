#pragma once
// strata-prepare: what its packers share - iq_pack.cpp (upstream Strata's tools/iq_pack.py) and maya_pack.cpp (Project
// Maya's fork of it): the model's shards as both Python tools read them, Python's file writes, the float conversions.

#include "gguf_header.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace strata_prepare {

struct Where {
  const GgufHeader* g;
  const TensorInfo* t;
  std::size_t shard;
};

enum class ModelFlavor {
  upstream,   // upstream's Model: its check_split, every type of its gguf_reader.py
  maya,       // Maya's fork: no split check, and its gguf_reader.py has no APR types (143-146): such a tensor is unsupported
};

// iq_pack.py's Model: every shard of one model (<name>-0000N-of-0000M.gguf beside the first), each tensor where it is,
// in the Python dict's order. Throws std::runtime_error with the Python tool's message.
class Model {
 public:
  Model(const std::filesystem::path& first, ModelFlavor flavor);
  [[nodiscard]] const Where* find(const std::string& name) const;
  [[nodiscard]] std::vector<unsigned char> bytes(const Where& w) const;
  [[nodiscard]] std::vector<unsigned char> read_range(std::size_t shard, std::uint64_t at, std::uint64_t size) const;

  std::vector<std::filesystem::path> paths;
  std::vector<std::unique_ptr<GgufHeader>> files;
  std::vector<std::uint64_t> sizes;
  std::vector<std::pair<std::string, Where>> order;   // the Python dict's insertion order
  std::unordered_map<std::string, std::size_t> index;

 private:
  void check_split() const;
};

// "\r\n" on Windows, else "\n": what Path.write_text writes for "\n" (os.linesep).
[[nodiscard]] const char* line_end();
// The bytes Path.write_text(text, encoding="utf-8") writes: each "\n" as line_end().
[[nodiscard]] std::string text_bytes(const std::string& text);
// Path.write_text(text, encoding="utf-8"). Throws std::runtime_error when the file cannot be written.
void write_text(const std::filesystem::path& path, const std::string& text);
// open(path, "w", encoding="utf-8", newline="\n"): the text as it is.
void write_lf(const std::filesystem::path& path, const std::string& text);
void replace_file(const std::filesystem::path& from, const std::filesystem::path& to);   // os.replace
void unlink_missing_ok(const std::filesystem::path& path);
// A file's bytes, empty when it cannot be read.
[[nodiscard]] std::string read_file_text(const std::filesystem::path& path);
// str(path) as UTF-8 (path::string() is the ANSI code page on Windows, and throws for a name it cannot hold), and back.
[[nodiscard]] std::string utf8(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path path_from_utf8(const std::string& text);

[[nodiscard]] std::string lower(std::string s);
[[nodiscard]] std::string fmt(const char* format, double value);   // "%.2f" % value
[[nodiscard]] bool starts_with(std::string_view s, std::string_view prefix) noexcept;
[[nodiscard]] bool ends_with(std::string_view s, std::string_view suffix) noexcept;

// int(value) of a metadata value: an integer, a float truncated, a decimal string; nullopt where Python's int() raises.
[[nodiscard]] std::optional<std::int64_t> python_int(const MetaValue& value);
// Python's truth value of a metadata value (0, 0.0, "" and [] are false).
[[nodiscard]] bool python_truthy(const MetaValue& value) noexcept;

[[nodiscard]] float f32_from_f16(std::uint16_t h) noexcept;
[[nodiscard]] float f32_from_bf16(std::uint16_t b) noexcept;
[[nodiscard]] bool all_finite(const std::vector<float>& values) noexcept;
// The tensor's values as float32: float types exactly, quantized ones through ggml's own dequantizer (gguf-py's numpy
// dequantizers are tested against these). nullopt for a type ggml cannot dequantize.
[[nodiscard]] std::optional<std::vector<float>> dequantize(const std::vector<unsigned char>& raw, const TensorInfo& t);

}  // namespace strata_prepare
