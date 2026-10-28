#pragma once

#include "secure_frontend/acquire_reason.hpp"
#include "secure_frontend/acquire_url.hpp"
#include "secure_frontend/address_policy.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// One request, one hop (spec §3.3). `url` is a validated source URL or Location
// and goes only into the stdin config, never into argv.
struct TransferRequest final {
  std::string url;
  std::string host;
  std::optional<ResolvedAddress> pin;        // only without a proxy
  std::optional<std::uint64_t> range_from;   // only when resuming
  std::optional<std::uint64_t> range_to;     // with range_from: a bounded piece "lo-hi" (parallel downloads)
  std::optional<ProxySetting> proxy;
  std::uint32_t connect_timeout_s{30};
  std::uint32_t stall_timeout_s{120};
  std::uint64_t max_time_s{172800};          // remaining run budget, >= 1
  std::string test_cacert;                   // SECURE_FRONTEND_TEST_HOOKS builds only; empty = absent
};

// <curl> -q --config - --silent --include --suppress-connect-headers --globoff
// --proto =https --max-redirs 0 --tlsv1.2 --connect-timeout N --speed-limit 1024
// --speed-time N --max-time N --user-agent strata --header "Accept-Encoding: identity"
// (--noproxy * | --proxy P) [--resolve h:443:a] [--range N-] [--cacert F (hooks)]
[[nodiscard]] std::vector<std::string> build_transfer_argv(std::string_view executable, const TransferRequest& request);
// Exactly: url = "<url>"\n (the URL grammar excludes '"' and '\').
[[nodiscard]] std::string build_config_stdin(const TransferRequest& request);
// Exactly: <curl> -q --version
[[nodiscard]] std::vector<std::string> build_probe_argv(std::string_view executable);

struct ProbeResult final {
  std::uint32_t major{0};
  std::uint32_t minor{0};
  std::uint32_t patch{0};
  std::string tls_backend;
};
// First line "curl X.Y.Z[...] (...) libcurl/... <TLS>/..."; nullopt if not curl or no known TLS backend.
[[nodiscard]] std::optional<ProbeResult> parse_probe_output(std::string_view text);

enum class DownloaderPlatform { linux_like, windows };
#if defined(_WIN32)
inline constexpr DownloaderPlatform kThisDownloaderPlatform = DownloaderPlatform::windows;
#else
inline constexpr DownloaderPlatform kThisDownloaderPlatform = DownloaderPlatform::linux_like;
#endif
// D3/C3: >= 8.4.0 on Windows, >= 7.81.0 on Linux.
[[nodiscard]] bool meets_version_floor(const ProbeResult& probe, DownloaderPlatform platform) noexcept;

// Spec §3.4 curl exit-code table; nullopt for 0 (the status table decides).
[[nodiscard]] std::optional<AcquireReason> map_curl_exit(int exit_code, bool run_budget_exhausted) noexcept;

}  // namespace secure_frontend
