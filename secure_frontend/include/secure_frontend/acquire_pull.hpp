#pragma once

#include "secure_frontend/acquire_progress.hpp"
#include "secure_frontend/acquire_publish.hpp"
#include "secure_frontend/fetch_file.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/manifest_diff.hpp"
#include "secure_frontend/model_index_store.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

struct PullRequest final {
  std::optional<std::string> reference;               // name[:tag] in the catalog
  std::optional<std::filesystem::path> manifest_file;  // --manifest <local file> (no URL form)
  std::optional<std::string> trust_digest;             // --trust-manifest sha256:<hex>
  bool yes{false};
  std::optional<std::filesystem::path> bundle_dir;     // --bundle <dir>
  bool allow_downgrade{false};
  bool verify{false};
  bool discard_partial{false};
  // F-51: copied into FetchSettings::test_cacert in SECURE_FRONTEND_TEST_HOOKS builds only;
  // a release build never reads it.
  std::string test_cacert;
};

class DownloaderProbe {
 public:
  virtual ~DownloaderProbe() = default;
  // §3.2: validates pull.downloader and runs <curl> -q --version; after confirmation only.
  [[nodiscard]] virtual std::optional<AcquireError> probe() = 0;
  // "full" | "reduced:no_landlock" | "reduced:readable:<key>"; never spawns.
  [[nodiscard]] virtual std::string confinement() const = 0;
};

struct PullServices final {
  HopTransport& transport;
  DownloaderProbe& probe;
  Resolver& resolver;
  AcquireClock& clock;
  const StoreEnvironment& store;
  LoadedVersions& loaded;
  Confirmer& ui;
  ProgressSink& progress;
  const std::atomic<bool>& interrupted;
};

struct PullOutcome final {
  ExitCode exit{ExitCode::success};
  std::optional<AcquireError> error;
  std::optional<IndexEntry> installed;
  bool already_installed{false};
  bool bundle_complete{false};
};

// Spec §1.3 / §3.7 / §4.6 / §6.1 / §5.3: resolve and trust the manifest, print the
// summary, confirm, stage (resume, reuse, fetch), verify and publish — or, with
// bundle_dir, fill a bundle directory instead of roots.models.
[[nodiscard]] PullOutcome run_pull(const PullRequest& request, const CanonicalRoot& models, const PullSettings& settings,
                                   PullServices& services);
// §6.1 update consent, shared by pull and import (the caller holds .lock).
struct UpdateReview final {
  std::optional<InstalledModel> installed;  // nullopt: the installed version cannot be read (license unknown)
  std::optional<ManifestDiff> diff;         // set exactly when `installed` is
};
// Loads the installed version and prints the manifest diff through `ui` (the console only,
// never acquire.log, §8.1), or a fixed line with the reason name when it cannot be read.
[[nodiscard]] UpdateReview review_update(const CanonicalRoot& models, const IndexEntry& installed, const AcquireManifest& candidate,
                                         Confirmer& ui);
// §6.1: a license change, or an installed license that is unknown, needs an interactive y
// even with --yes (yes_applies=false). false: declined.
[[nodiscard]] bool confirm_license_change(const UpdateReview& review, const AcquireManifest& candidate, Confirmer& ui);
// Trimmed (space, tab, CR, LF), ASCII case-insensitive "y" or "yes"; anything else is No.
[[nodiscard]] bool parse_confirmation(std::string_view answer) noexcept;
// Decimal gigabytes with one digit: 68'000'000'000 -> "68.0 GB".
[[nodiscard]] std::string format_gb(std::uint64_t bytes);
[[nodiscard]] FetchSettings fetch_settings_from(const PullSettings& settings);

}  // namespace secure_frontend
