#pragma once

#include "secure_frontend/acquire_manifest.hpp"
#include "secure_frontend/acquire_progress.hpp"
#include "secure_frontend/address_policy.hpp"
#include "secure_frontend/download_pieces.hpp"
#include "secure_frontend/downloader_command.hpp"
#include "secure_frontend/fetch_schedule.hpp"
#include "secure_frontend/models_store.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

struct HopExit final {
  std::optional<int> curl_exit;           // absent when killed or never started
  std::optional<AcquireError> failure;    // spawn/confinement failure (downloader_*)
  bool killed{false};
};

// One downloader process per hop (§3.3). on_bytes receives curl's stdout as it
// arrives; returning false kills the downloader's process group / Job at once.
class HopTransport {
 public:
  virtual ~HopTransport() = default;
  [[nodiscard]] virtual HopExit run(const TransferRequest& request, const std::function<bool(std::string_view)>& on_bytes) = 0;
};

class Resolver {
 public:
  virtual ~Resolver() = default;
  // nullopt: name resolution failed (dns_failed).
  [[nodiscard]] virtual std::optional<std::vector<ResolvedAddress>> resolve(std::string_view host) = 0;
};

// getaddrinfo(AI_ADDRCONFIG), spec §3.5. Never used by tests (no real network).
class SystemResolver final : public Resolver {
 public:
  [[nodiscard]] std::optional<std::vector<ResolvedAddress>> resolve(std::string_view host) override;
};

struct FetchSettings final {
  std::optional<ProxySetting> proxy;
  bool allow_private_addresses{false};
  std::uint32_t connect_timeout_s{30};
  std::uint32_t stall_timeout_s{120};
  std::string test_cacert;  // SECURE_FRONTEND_TEST_HOOKS builds only; ignored otherwise
  // Parallel ranged downloads (spec 2026-10-23-parallel-downloader-design.md): 1 keeps one stream
  // (the default — every caller is single-stream until it opts in). >1 fetches a large file as
  // `piece_bytes` pieces over this many confined children; applies only without a proxy, when the
  // file is at least `min_parallel_bytes` and splits into at least two pieces, and the server
  // honours ranges with a stable ETag (else it falls back to one stream).
  std::uint32_t connections{1};
  std::uint64_t piece_bytes{kDownloadPieceBytes};
  std::uint64_t min_parallel_bytes{std::uint64_t{256} << 20U};
  std::uint64_t pieces_per_run{kDownloadRun};  // pieces one request asks for (tests use 1)
};

struct FetchContext final {
  HopTransport& transport;
  Resolver& resolver;
  AcquireClock& clock;
  const RunBudget& budget;
  const FetchSettings& settings;
  const std::atomic<bool>& interrupted;
  ProgressSink& progress;
  const StoreEnvironment& store;
  const CanonicalRoot& root;
};

// `file` is open at its current length; `hasher` already holds those bytes
// (re-hashed this run, §3.7); resumed_from_disk = the bytes came from an earlier run.
struct FetchTarget final {
  RelativePath part;
  RelativePath final_path;
  PartFile& file;
  Sha256& hasher;
  bool resumed_from_disk;
};

inline constexpr int kMaxRedirectHops = 3;

// Reasons that end the whole run at once (no next source).
[[nodiscard]] bool is_run_fatal(AcquireReason reason) noexcept;

// The TransferRequest for one hop: url and host both come from `url`, so the
// --resolve pin always names the host curl contacts (§3.5).
[[nodiscard]] TransferRequest hop_request(const HttpsUrl& url, const FetchSettings& settings, std::uint64_t max_time_s);

// Spec §3.4-§3.7 for one manifest file. On success the file is fsynced, closed and
// renamed to final_path. On failure the error carries the last source's reason; a
// verification reason has deleted the .part, a transport reason has kept it.
[[nodiscard]] std::optional<AcquireError> fetch_file(FetchContext& context, const AcquireManifest& manifest, std::size_t file_index,
                                                     FetchTarget& target);

}  // namespace secure_frontend
