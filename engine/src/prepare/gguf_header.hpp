#pragma once
// strata-prepare (docs/superpowers/specs/2026-10-27-strata-prepare-design.md): a GGUF v3 header, read the way upstream
// Strata's tools/gguf_reader.py reads it (the reference the packs are checked against byte for byte) - its type names,
// its block geometry, its data-start rule. Tensor data is never read here.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace strata_prepare {

// A metadata value as Python holds it: an int (every integer type and bool), a float, a string, or a list.
struct MetaValue {
  enum class Kind { integer, real, text, list } kind = Kind::integer;
  std::int64_t integer = 0;
  double real = 0.0;
  std::string text;
  std::vector<MetaValue> items;
};

struct TensorInfo {
  std::string name;
  std::vector<std::uint64_t> shape;   // GGUF order
  std::uint32_t type_id = 0;
  std::string type_name;              // gguf_reader.py's GGML_TYPES name, or "type<N>"
  std::uint64_t offset = 0;           // from the data section's start
  [[nodiscard]] std::uint64_t elements() const;
  // gguf_reader.py's expected_bytes(): nullopt for an unknown type or a count that is not whole blocks
  [[nodiscard]] std::optional<std::uint64_t> expected_bytes() const;
};

class GgufHeader {
 public:
  // Throws std::runtime_error with gguf_reader.py's message on a file that is not GGUF v3.
  explicit GgufHeader(const std::filesystem::path& path);
  std::filesystem::path path;
  std::uint32_t version = 0;
  std::map<std::string, MetaValue> metadata;
  std::vector<std::string> keys;     // metadata's keys in the file's order (the Python dict's; a repeated key keeps its first place)
  std::vector<TensorInfo> tensors;   // in the file's order
  std::uint64_t alignment = 32;
  std::uint64_t data_start = 0;
  std::uint64_t file_size = 0;
  [[nodiscard]] const MetaValue* get(const std::string& key) const;
};

// gguf_reader.py's GGML_TYPES name for a type id ("type<N>" when it has none).
[[nodiscard]] std::string type_name_of(std::uint32_t type_id);

// bytes.decode("utf-8", "replace") re-encoded as UTF-8: each maximal invalid subpart becomes U+FFFD, as Python does
// (every string GgufHeader holds has been through it, as gguf_reader.py's _str decodes them).
[[nodiscard]] std::string utf8_replace(const std::string& bytes);

}  // namespace strata_prepare
