#pragma once

#include "secure_frontend/acquire_reason.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/manifest_json.hpp"
#include "secure_frontend/model_index_model.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

struct ModelRootFile final {
  std::string path;  // '/'-separated, relative to roots.models
  std::uint64_t size;
};

// Spec §4.9 delta b: every regular file below roots.models with its size, for
// list --all and doctor. Derives no model IDs, so the same shard name in
// current, retired and orphaned versions is fine. Links/junctions and other
// entries are skipped and never followed. Sorted by path.
[[nodiscard]] std::variant<std::vector<ModelRootFile>, PathError> scan_model_root_files(const CanonicalRoot& root);

// The same scan, plus how many entries it skipped (links/junctions, devices, FIFOs,
// names that are not P1-portable) so list --all and doctor can say so (Task 15 carry).
struct ModelRootScan final {
  std::vector<ModelRootFile> files;
  std::size_t skipped{0};
};
[[nodiscard]] std::variant<ModelRootScan, PathError> scan_model_root(const CanonicalRoot& root);

// Strict JSON (§4.5): exact keys, format/schema_version, version labels,
// dir == v<sequence>-<12 hex> (retired dirs too), names valid, one models entry
// per name:tag. Any failure: index_inconsistent.
[[nodiscard]] std::variant<ModelIndex, AcquireError> parse_model_index(std::string_view bytes);
// roots.models/index.json (<= kIndexMaxBytes, owner-only); absent = empty index.
[[nodiscard]] std::variant<ModelIndex, AcquireError> read_model_index(const CanonicalRoot& models);

struct InstalledModel final {
  IndexEntry entry;
  ParsedManifest manifest;
  RelativePath version_dir;  // <name>/<tag>/<dir>
};
// Re-hashes <dir>/manifest.json (<= 1 MiB) against the entry, parses it, and
// checks name/tag/version/sequence; any mismatch, and an entry that
// parse_model_index would refuse, is index_inconsistent (fail closed, no throw).
[[nodiscard]] std::variant<InstalledModel, AcquireError> load_installed(const CanonicalRoot& models, const IndexEntry& entry);
// name[:tag] in the index (F-58: one lookup for strata verbs and the managed launch).
// No match: model_unknown (detail: the reference). Several tags for a bare name:
// usage_error ("several tags installed; name one as <name:tag>").
[[nodiscard]] std::variant<IndexEntry, AcquireError> find_installed(const ModelIndex& index, const ModelReference& reference);

// For validated names only: throws std::logic_error if the result is not P1-valid.
[[nodiscard]] RelativePath version_dir_path(std::string_view name, std::string_view tag, std::string_view dir);

}  // namespace secure_frontend
