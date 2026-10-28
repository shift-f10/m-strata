#pragma once

#include "secure_frontend/acquire_pull.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/model_index_store.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace secure_frontend {

struct CliStreams final {
  std::istream& in;
  std::ostream& out;
  std::ostream& err;
  bool interactive;  // stdin is a terminal; otherwise every [y/N] is No unless --yes applies
};

// Seams; an empty member means the production default.
struct CliServices final {
  std::function<std::unique_ptr<HopTransport>(const FrontendConfig&)> transport;     // Task 27
  std::function<std::unique_ptr<DownloaderProbe>(const FrontendConfig&)> probe;      // Task 27
  std::function<std::unique_ptr<Resolver>()> resolver;                               // default SystemResolver (Task 27)
  std::function<std::unique_ptr<LoadedVersions>(const FrontendConfig&)> loaded;      // default NoServerDiscovery
  const StoreEnvironment* store{nullptr};                                            // default StoreEnvironment
  AcquireClock* clock{nullptr};                                                      // default SystemClock (Task 27)
  // default: the network verbs' process-wide SIGINT/SIGTERM (Windows console) flag (Task 27)
  const std::atomic<bool>* interrupted{nullptr};
  // SECURE_FRONTEND_TEST_HOOKS builds only; set from --test-record-file / --test-force-no-landlock (Task 27).
  std::filesystem::path test_record_file;
  bool test_force_no_landlock{false};
  // SECURE_FRONTEND_TEST_HOOKS builds only (F-51 / Task 22 carry): copied into PullRequest::test_cacert
  // by the network verbs (Task 27); a release build never reads it. Empty = absent.
  std::string test_cacert;
  // SECURE_FRONTEND_TEST_HOOKS builds only (SP7 M2 Task 3): the `strata update` test pins a release
  // key without recompiling, and simulates a D-4 versioned install by naming this process's "own"
  // executable. A release build never reads either. Empty = absent (production defaults).
  std::string test_pinned_release_key_hex;
  std::filesystem::path test_self_executable;
  // SECURE_FRONTEND_TEST_HOOKS builds only, Windows: set from --test-system-dir; `service` and
  // `uninstall` run their system tool (schtasks) from this directory instead of System32, so
  // a test never reaches the real Task Scheduler. A release build never reads it. Empty = absent.
  std::filesystem::path test_tool_directory;
};

class StreamConfirmer final : public Confirmer {
 public:
  explicit StreamConfirmer(CliStreams& io) : io_(io) {}
  void say(std::string_view line) override;
  // --yes (when it applies) answers yes; a non-interactive stdin answers No; otherwise one
  // line is read and parsed by parse_confirmation; EOF is No.
  [[nodiscard]] bool confirm(std::string_view question, bool yes_applies) override;

 private:
  CliStreams& io_;
};

class StreamProgress final : public ProgressSink {
 public:
  explicit StreamProgress(CliStreams& io) : io_(io) {}
  void notice(std::string_view line) override;
  void file_started(std::string_view path, std::uint64_t total) override;
  void file_bytes(std::string_view path, std::uint64_t done, std::uint64_t total) override;

 private:
  CliStreams& io_;
  std::uint64_t last_reported_{0};
};

// strata --config <file> <verb> [arguments]; strata update; strata --help. Returns the exit code (§8.3).
// Reads no environment variable value (doctor checks proxy variables for presence only, §3.8).
[[nodiscard]] int run_strata(std::span<const std::string> args, CliStreams& streams, CliServices& services);

// The engine argv tail the manifest's launch block implies, paths relative to roots.models.
[[nodiscard]] std::string implied_engine_arguments(const InstalledModel& model);

// list --all and doctor (§3.7, §4.5): retired, orphaned and per-key staging usage with sizes,
// plus a count of entries the scan skipped (links, non-portable names).
int print_store_usage(const CanonicalRoot& models, const ModelIndex& index, CliStreams& io);

}  // namespace secure_frontend
