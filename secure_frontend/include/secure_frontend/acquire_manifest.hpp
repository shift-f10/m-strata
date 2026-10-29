#pragma once

#include "secure_frontend/acquire_reason.hpp"
#include "secure_frontend/engine_version.hpp"
#include "secure_frontend/test_hooks.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// raw: SECURE_FRONTEND_TEST_HOOKS builds only. gguf (model downloads, owner 2026-10-23): .gguf
// weights (and at most one mmproj) fetched from Hugging Face to be prepared on this machine; no
// launch keys, never launched as a managed model.
// deepseek and raon1 run on the merged unified engine (unified-engine design §2.5); they are
// recognized here but gated in engine_gate until the merged engine is installed (plan 4).
// mtp (owner 2026-10-29): Qwen3.8's MTP draft layer's tensors, each a window of a checkpoint shard (built on this
// machine by strata-prepare mtp; never launched)
enum class ManifestProfile { qwen, glm53, gemma4, raw, gguf, deepseek, raon1, mtp };
enum class ManifestFamily { strata, glm, gemma4, none, unified };
// SP5 M1 Task 11 (D-9): FileRole gains mmproj — the vision encoder artifact, listed like
// any other file, AT MOST ONE entry, named by the optional launch.mmproj (Ruling T11-bb).
enum class FileRole { weights, pack, tokenizer, mmproj };

struct ManifestLicense final {
  std::string id;
  std::string url;
  bool bundle_allowed{false};
  std::string notice;
};

struct ManifestFile final {
  std::string path;  // '/'-separated, relative to the version directory
  FileRole role{FileRole::weights};
  std::uint64_t size{0};
  std::string sha256;
  std::vector<std::string> sources;  // empty: arrives only by import (§2.7)
  std::optional<ManifestLicense> license;
  // Owner 2026-10-29 (Qwen3.8's MTP draft layer, upstream's tools/mtp_fetch.py): the file is the window [offset,
  // offset + size) of each source - a tensor of a checkpoint shard - fetched with that bounded range and taken only
  // from a 206 of exactly it. Absent: the whole source.
  std::optional<std::uint64_t> source_offset{};
};

struct ManifestLaunch final {
  std::optional<std::string> native;
  std::optional<std::string> ple_gguf;
  std::optional<std::string> pack;
  std::optional<std::string> tokenizer_pack;
  std::optional<std::string> mmproj;  // SP5 M1 Task 11 (D-9): the vision encoder artifact (Ruling T11-bb)
};

// Schema version 1 (spec §2.2). Identity is the SHA-256 of the exact bytes (Task 14).
struct AcquireManifest final {
  std::string name;
  std::string tag;
  std::string version;
  std::uint32_t sequence{0};
  ManifestProfile profile{ManifestProfile::qwen};
  ManifestFamily family{ManifestFamily::strata};
  EngineVersion min_version;
  ManifestLicense license;
  std::vector<std::string> hosts;
  std::vector<std::string> redirect_hosts;
  std::vector<ManifestFile> files;
  ManifestLaunch launch;
};

inline constexpr std::uint64_t kDefaultMaxTotalBytes = std::uint64_t{1} << 40U;  // pull.max_total_bytes default
inline constexpr std::size_t kManifestMaxFiles = 4096;
inline constexpr std::size_t kManifestMaxSources = 8;
inline constexpr std::size_t kManifestMaxHosts = 16;
inline constexpr std::size_t kManifestMaxStringBytes = 1024;
inline constexpr std::size_t kManifestMaxNoticeBytes = 512;
inline constexpr std::uint64_t kManifestMaxFileBytes = std::uint64_t{1} << 40U;
inline constexpr std::string_view kPartSuffix = ".part";
inline constexpr std::array<std::string_view, 5> kReservedTopLevelNames{"manifest.json", "manifest", "strata-bundle",
                                                                        "sha256sums", "missing"};

struct ManifestRules final {
  bool allow_raw_profile{kTestHooksEnabled};
  std::uint64_t max_total_bytes{kDefaultMaxTotalBytes};
};

// key_path names the offending key ("files[3].sources[0]"); message is fixed text.
struct ManifestIssue final {
  AcquireReason reason;
  std::string key_path;
  std::string_view message;
};

// Every struct-level rule of spec §2.1-§2.4, including SP1's P1 path grammar for
// file paths (so no caller can skip it). Returns the first issue.
[[nodiscard]] std::optional<ManifestIssue> validate_manifest(const AcquireManifest& manifest, const ManifestRules& rules);

[[nodiscard]] std::string ascii_lower(std::string_view text);
// [a-z0-9][a-z0-9._-]{0,127}, not a Windows device name (with any extension),
// no trailing '.', not "index.json". Callers lowercase first.
[[nodiscard]] bool is_valid_model_name(std::string_view name) noexcept;
// YYYY.MM.DD.N with N 1..999 (spec §2.1 "version"; also index.json entries).
[[nodiscard]] bool is_version_label(std::string_view text) noexcept;

struct ModelReference final {
  std::string name;
  std::optional<std::string> tag;
};
// "name" or "name:tag", ASCII-lowercased, both parts valid model names.
[[nodiscard]] std::optional<ModelReference> parse_model_reference(std::string_view text);
[[nodiscard]] std::string model_ref_text(std::string_view name, std::string_view tag);

[[nodiscard]] std::uint64_t total_bytes(const AcquireManifest& manifest) noexcept;
[[nodiscard]] const ManifestLicense& effective_license(const AcquireManifest& manifest, std::size_t file_index) noexcept;
[[nodiscard]] bool bundle_allowed(const AcquireManifest& manifest) noexcept;
[[nodiscard]] bool has_sourceless_files(const AcquireManifest& manifest) noexcept;
[[nodiscard]] std::string_view profile_name(ManifestProfile profile) noexcept;
[[nodiscard]] std::string_view family_name(ManifestFamily family) noexcept;
[[nodiscard]] std::string_view role_name(FileRole role) noexcept;

}  // namespace secure_frontend
