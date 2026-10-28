#pragma once

#include "secure_frontend/acquire_manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// One regular file (or other entry) found below .staging/<k>/, '/'-separated.
struct StagedEntry final {
  std::string path;
  std::uint64_t size{0};
  bool regular{true};
};

// Spec §3.7: a run boundary is a trust boundary; every staged byte is re-hashed.
enum class FileStep {
  fetch_from_zero,            // nothing usable on disk
  rehash_then_resume,         // <path>.part, L < size: hash L bytes, then --range L-
  rehash_complete_part,       // <path>.part, L == size: hash, then rename to <path>
  rehash_verified_candidate,  // <path> from an earlier run: hash before trusting
};

struct FilePlan final {
  std::size_t file_index;
  FileStep step;
  std::uint64_t existing_bytes;
};

struct StagingPlan final {
  std::vector<FilePlan> files;       // one per manifest file, manifest order
  std::vector<std::string> remove;   // entries to delete before transfer, sorted
  std::uint64_t staged_bytes{0};     // bytes kept (for the §4.6 preflight)
};

[[nodiscard]] StagingPlan plan_staging(const AcquireManifest& manifest, std::span<const StagedEntry> staged);

struct StagingKeyInfo final {
  std::string key;
  std::string name;
  std::string tag;
  std::uint64_t bytes{0};
};

// Other keys holding a manifest of the same name:tag (§3.7 "Abandoned partials").
[[nodiscard]] std::vector<StagingKeyInfo> abandoned_keys(std::span<const StagingKeyInfo> keys, std::string_view own_key,
                                                         std::string_view name, std::string_view tag);

}  // namespace secure_frontend
