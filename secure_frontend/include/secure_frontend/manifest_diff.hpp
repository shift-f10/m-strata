#pragma once

#include "secure_frontend/acquire_manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace secure_frontend {

// Spec §6.1. Lines only use manifest data (paths, sizes, digests, hosts,
// license fields), never URLs of sources.
struct ManifestDiff final {
  std::vector<std::string> lines;
  bool license_changed{false};          // needs an interactive y even with --yes
  std::uint64_t bytes_to_download{0};   // added or changed files only
  std::vector<std::size_t> reusable_files;  // candidate indexes with the same path and digest as an installed file
};

[[nodiscard]] ManifestDiff diff_manifests(const AcquireManifest& installed, const AcquireManifest& candidate);

}  // namespace secure_frontend
