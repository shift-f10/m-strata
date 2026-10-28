#pragma once

#include "secure_frontend/acquire_progress.hpp"
#include "secure_frontend/acquire_publish.hpp"
#include "secure_frontend/model_index_store.hpp"

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>

namespace secure_frontend {

struct OpenedBundle final {
  CanonicalRoot root;
  TrustedManifest manifest;
};

// §5.1-§5.2: canonicalized once (RootPolicy::external), never a drive root ("/", "X:\";
// a mount point is fine: a USB stick's or ISO's root);
// marker exact; manifest trusted by catalog digest or the typed digest; SHA256SUMS
// agrees (never a trust source); files/ holds exactly the manifest's files, no
// link/junction/hard link/device/FIFO. Reads only.
[[nodiscard]] std::variant<OpenedBundle, AcquireError> open_bundle(const std::filesystem::path& dir,
                                                                   const std::optional<std::string>& trust_digest,
                                                                   const ManifestRules& rules, ProgressSink& progress);
// Reads and hashes every file; writes nothing (strata verify).
[[nodiscard]] std::optional<AcquireError> verify_bundle_files(const OpenedBundle& bundle, ProgressSink& progress);
// Takes roots.models/.lock and decides first: "already installed" asks nothing. Then, under
// the lock, §6.1 as pull: on an update the manifest diff (console only) and the license
// questions --yes never answers; then "Import …? [y/N]" (answered by `yes`); declined is
// AcquireReason::declined. Copies each file into .staging/<k>/ while hashing it and publishes
// exactly as pull does; never writes into the bundle.
[[nodiscard]] std::variant<InstallOutcome, AcquireError> import_bundle(const OpenedBundle& bundle, const CanonicalRoot& models,
                                                                       bool allow_downgrade, LoadedVersions& loaded,
                                                                       const StoreEnvironment& env, ProgressSink& progress,
                                                                       Confirmer& ui, bool yes);

struct BundleTarget final {
  CanonicalRoot root;
  bool resumed{false};
};
// §5.3: license first (license_forbids_bundle, nothing written), then the target must
// not exist, be empty, or (allow_resume) hold only this manifest's interrupted pull --bundle;
// never a drive root nor (POSIX) a mount point; FAT refused for files above env.fat_max_file_bytes(); free space checked.
[[nodiscard]] std::variant<BundleTarget, AcquireError> prepare_bundle_target(const std::filesystem::path& dir,
                                                                             const TrustedManifest& manifest, bool allow_resume,
                                                                             const StoreEnvironment& env);
// manifest.json; then MISSING (some file absent) or SHA256SUMS and STRATA-BUNDLE last; .staging removed.
[[nodiscard]] std::optional<AcquireError> finish_bundle(const CanonicalRoot& target, const TrustedManifest& manifest);
// strata export: re-hashes every installed file while copying (bit rot is not propagated).
[[nodiscard]] std::optional<AcquireError> export_model(const CanonicalRoot& models, const InstalledModel& model,
                                                       const std::filesystem::path& target, const StoreEnvironment& env,
                                                       ProgressSink& progress);
// <to>.part while hashing; renamed to <to> only when size and digest match; else on_mismatch.
// `interrupted` (§8.3, final fix F1): checked before every chunk; set → the <to>.part is removed
// and AcquireError interrupted is returned. nullptr: never interrupted.
[[nodiscard]] std::variant<std::uint64_t, AcquireError> copy_verified(const CanonicalRoot& from_root, const RelativePath& from,
                                                                      const CanonicalRoot& to_root, const RelativePath& to,
                                                                      const ManifestFile& file, const StoreEnvironment& env,
                                                                      ProgressSink& progress, AcquireReason on_mismatch,
                                                                      const std::atomic<bool>* interrupted = nullptr);

}  // namespace secure_frontend
