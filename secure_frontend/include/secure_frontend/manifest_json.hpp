#pragma once

#include "secure_frontend/acquire_manifest.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace secure_frontend {

inline constexpr std::size_t kManifestMaxBytes = std::size_t{1} << 20U;
inline constexpr std::size_t kManifestMaxDepth = 6;

// Identity = SHA-256 of `bytes`, kept verbatim (never re-serialized).
struct ParsedManifest final {
  AcquireManifest manifest;
  std::string bytes;
  std::string sha256;
};

using ManifestParseResult = std::variant<ParsedManifest, ManifestIssue>;

// Strict JSON through SP1's parse_json (UTF-8, no BOM, duplicate keys and
// non-integers rejected), then: unknown keys, types, printable ASCII, the
// format/schema_version gate, and validate_manifest() (which applies the P1
// path grammar, so no caller of either function can skip it).
[[nodiscard]] ManifestParseResult parse_manifest(std::string_view bytes, const ManifestRules& rules);

}  // namespace secure_frontend
