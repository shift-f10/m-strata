#pragma once

#include "secure_frontend/canonical_path.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

// A GGUF file's metadata (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §4.2): the header only - the
// magic, version 2 or 3, the counts and the key/value pairs; the tensor infos and the tensor data that follow are never
// read. Every count, string and array is bounded; integers of every width are int64 (a u64 above INT64_MAX is
// refused), f32/f64 are double, booleans bool, an array one of strings, integers (booleans as 0/1) or reals.
struct GgufArray final {
  std::vector<std::string> strings;
  std::vector<std::int64_t> integers;
  std::vector<double> reals;
};
using GgufValue = std::variant<std::int64_t, double, bool, std::string, GgufArray>;

struct GgufMetadata final {
  std::uint32_t version{0};
  std::uint64_t tensor_count{0};
  std::uint64_t header_bytes{0};   // where the metadata ends (the tensor infos begin)
  std::map<std::string, GgufValue, std::less<>> values;

  [[nodiscard]] const std::string* string(std::string_view key) const;
  [[nodiscard]] std::optional<std::int64_t> integer(std::string_view key) const;
  [[nodiscard]] std::optional<double> real(std::string_view key) const;     // an integer value too
  [[nodiscard]] std::optional<bool> boolean(std::string_view key) const;
  [[nodiscard]] const std::vector<std::string>* strings(std::string_view key) const;
  [[nodiscard]] const std::vector<std::int64_t>* integers(std::string_view key) const;
};

struct GgufLimits final {
  std::size_t max_keys{4096};
  std::size_t max_array{std::size_t{1} << 21};       // Gemma 4's 514,906 merges and 262,144 tokens fit
  std::size_t max_string{std::size_t{1} << 20};      // a chat template; a token is far shorter
  std::uint64_t max_header_bytes{std::uint64_t{256} << 20};
};

enum class GgufErrorCode { not_gguf, unsupported_version, truncated, too_large, malformed, read_failed };
// `message` is fixed text; it never holds a key, a value or a path.
struct GgufError final {
  GgufErrorCode code;
  std::string message;
};
using GgufResult = std::variant<GgufMetadata, GgufError>;

// The header from bytes (tests) or from an open file (read in bounded chunks from its start).
[[nodiscard]] GgufResult parse_gguf_metadata(std::string_view bytes, const GgufLimits& limits = {});
[[nodiscard]] GgufResult read_gguf_metadata(const OpenedFile& file, const GgufLimits& limits = {});

}  // namespace secure_frontend
