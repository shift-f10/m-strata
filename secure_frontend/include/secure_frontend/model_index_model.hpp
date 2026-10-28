#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

inline constexpr std::string_view kIndexFileName = "index.json";
inline constexpr std::string_view kLockFileName = ".lock";
inline constexpr std::string_view kStagingDirName = ".staging";
inline constexpr std::string_view kStagingCwdName = ".cwd";
inline constexpr std::string_view kStagingManifestName = "MANIFEST";
inline constexpr std::string_view kVersionManifestName = "manifest.json";
inline constexpr std::size_t kIndexMaxBytes = std::size_t{4} * 1024U * 1024U;

enum class ModelOrigin { catalog, third_party };
[[nodiscard]] std::string_view origin_name(ModelOrigin origin) noexcept;

struct IndexEntry final {
  std::string name;
  std::string tag;
  std::string version;
  std::uint32_t sequence{0};
  std::string dir;              // v<sequence>-<12 hex>, below <name>/<tag>/
  std::string manifest_sha256;  // of <dir>/manifest.json
  ModelOrigin origin{ModelOrigin::catalog};
};

struct RetiredEntry final {
  std::string name;
  std::string tag;
  std::string dir;
  std::string manifest_sha256;
};

// roots.models/index.json: the single source of truth for installed models (§4.5).
struct ModelIndex final {
  std::vector<IndexEntry> models;
  std::vector<RetiredEntry> retired;
};

// Compact strict JSON, models sorted by (name, tag), retired by (name, tag, dir), one trailing LF.
[[nodiscard]] std::string render_model_index(const ModelIndex& index);
[[nodiscard]] const IndexEntry* find_model(const ModelIndex& index, std::string_view name, std::string_view tag) noexcept;
// Names `entry`; a previous entry of the same name:tag in another dir moves to `retired`.
[[nodiscard]] ModelIndex with_installed(ModelIndex index, IndexEntry entry);
[[nodiscard]] ModelIndex without_model(ModelIndex index, std::string_view name, std::string_view tag);
[[nodiscard]] ModelIndex without_retired(ModelIndex index, std::string_view name, std::string_view tag, std::string_view dir);

// v<sequence>-<first 12 hex of the manifest digest> (§4.3); never derived from the human version.
[[nodiscard]] std::string version_directory_name(std::uint32_t sequence, std::string_view manifest_sha256);
// First 32 hex of the manifest digest (§3.7).
[[nodiscard]] std::string staging_key(std::string_view manifest_sha256);
// Exactly 32 lowercase hex: so ".cwd" and every other name is never a key.
[[nodiscard]] bool is_staging_key_name(std::string_view name) noexcept;

}  // namespace secure_frontend
