#pragma once

#include "secure_frontend/acquire_manifest.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

inline constexpr std::string_view kBundleMarkerName = "STRATA-BUNDLE";
inline constexpr std::string_view kBundleMarkerContent = "strata-bundle 1\n";
inline constexpr std::string_view kBundleManifestName = "manifest.json";
inline constexpr std::string_view kBundleSumsName = "SHA256SUMS";
inline constexpr std::string_view kBundleMissingName = "MISSING";
inline constexpr std::string_view kBundleFilesDir = "files";

struct SumsLine final {
  std::string sha256;
  std::string path;
};

// "<64 hex>  <relative path>\n", LF, sorted bytewise by path, covering
// manifest.json and every files/<path> (§5.1). For humans; never a trust source.
[[nodiscard]] std::string render_sha256sums(std::string_view manifest_sha256, const AcquireManifest& manifest);
// Strict inverse: two spaces, LF only, lowercase hex, strictly increasing paths, non-empty.
[[nodiscard]] std::optional<std::vector<SumsLine>> parse_sha256sums(std::string_view text);
// Same paths, same digests as the manifest (§5.1: else bundle_invalid).
[[nodiscard]] bool sums_agree(std::span<const SumsLine> lines, std::string_view manifest_sha256, const AcquireManifest& manifest);

// Paths of files without sources, sorted bytewise, one per line (§2.7).
[[nodiscard]] std::string render_missing(const AcquireManifest& manifest);
[[nodiscard]] std::optional<std::vector<std::string>> parse_missing(std::string_view text);

}  // namespace secure_frontend
