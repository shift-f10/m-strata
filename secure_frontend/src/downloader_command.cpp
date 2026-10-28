#include "secure_frontend/downloader_command.hpp"

#include <array>
#include <tuple>

namespace secure_frontend {
namespace {

// Matched by exact equality of the token name before '/', so list order does not matter
// ("rustls-ffi" and "rustls" are distinct names).
constexpr std::array<std::string_view, 12> kTlsBackends{"OpenSSL", "LibreSSL", "BoringSSL", "quictls", "GnuTLS",
                                                        "Schannel", "wolfSSL", "mbedTLS", "rustls-ffi", "rustls",
                                                        "SecureTransport", "BearSSL"};

bool parse_number(std::string_view& text, std::uint32_t& out) {
  std::size_t used = 0;
  std::uint64_t value = 0;
  while (used < text.size() && text[used] >= '0' && text[used] <= '9' && used < 6) {
    value = value * 10U + static_cast<std::uint64_t>(text[used] - '0');
    ++used;
  }
  if (used == 0) return false;
  out = static_cast<std::uint32_t>(value);
  text.remove_prefix(used);
  return true;
}

}  // namespace

std::vector<std::string> build_transfer_argv(const std::string_view executable, const TransferRequest& request) {
  std::vector<std::string> argv{std::string{executable}, "-q", "--config", "-", "--silent", "--include",
                                "--suppress-connect-headers", "--globoff", "--proto", "=https", "--max-redirs", "0",
                                "--tlsv1.2", "--connect-timeout", std::to_string(request.connect_timeout_s),
                                "--speed-limit", "1024", "--speed-time", std::to_string(request.stall_timeout_s),
                                "--max-time", std::to_string(request.max_time_s), "--user-agent", "strata",
                                "--header", "Accept-Encoding: identity"};
  if (request.proxy.has_value()) {
    argv.emplace_back("--proxy");
    argv.push_back(render_proxy(*request.proxy));
  } else {
    argv.emplace_back("--noproxy");
    argv.emplace_back("*");
    if (request.pin.has_value()) {
      argv.emplace_back("--resolve");
      argv.push_back(resolve_argument(request.host, *request.pin));
    }
  }
  if (request.range_from.has_value()) {
    argv.emplace_back("--range");
    // A bounded piece "lo-hi" (parallel ranged downloads); range_from alone stays open-ended "lo-".
    argv.push_back(std::to_string(*request.range_from) + "-" +
                   (request.range_to.has_value() ? std::to_string(*request.range_to) : std::string{}));
  }
#if defined(SECURE_FRONTEND_TEST_HOOKS)
  if (!request.test_cacert.empty()) {
    argv.emplace_back("--cacert");
    argv.push_back(request.test_cacert);
  }
#endif
  return argv;
}

std::string build_config_stdin(const TransferRequest& request) { return "url = \"" + request.url + "\"\n"; }

std::vector<std::string> build_probe_argv(const std::string_view executable) {
  return {std::string{executable}, "-q", "--version"};
}

std::optional<ProbeResult> parse_probe_output(const std::string_view text) {
  const auto end = text.find_first_of("\r\n");
  std::string_view line = text.substr(0, end);
  if (!line.starts_with("curl ")) return std::nullopt;
  std::string_view version = line.substr(5);
  ProbeResult result;
  if (!parse_number(version, result.major) || !version.starts_with('.')) return std::nullopt;
  version.remove_prefix(1);
  if (!parse_number(version, result.minor) || !version.starts_with('.')) return std::nullopt;
  version.remove_prefix(1);
  if (!parse_number(version, result.patch)) return std::nullopt;
  std::size_t start = 0;
  while (start < line.size()) {
    auto stop = line.find(' ', start);
    if (stop == std::string_view::npos) stop = line.size();
    const auto token = line.substr(start, stop - start);
    const auto name = token.substr(0, token.find('/'));
    for (const auto backend : kTlsBackends) {
      if (name == backend) {
        result.tls_backend = std::string{backend};
        return result;
      }
    }
    start = stop + 1;
  }
  return std::nullopt;
}

bool meets_version_floor(const ProbeResult& probe, const DownloaderPlatform platform) noexcept {
  const auto have = std::tie(probe.major, probe.minor, probe.patch);
  if (platform == DownloaderPlatform::windows) return have >= std::make_tuple(8U, 4U, 0U);
  return have >= std::make_tuple(7U, 81U, 0U);
}

std::optional<AcquireReason> map_curl_exit(const int exit_code, const bool run_budget_exhausted) noexcept {
  switch (exit_code) {
    case 0: return std::nullopt;
    case 5: case 6: return AcquireReason::dns_failed;
    case 7: return AcquireReason::connect_failed;
    case 18: case 52: case 55: case 56: case 92: return AcquireReason::size_short;
    case 28: return run_budget_exhausted ? AcquireReason::time_budget_exceeded : AcquireReason::stalled;
    case 35: return AcquireReason::tls_failed;
    case 58: case 59: case 60: case 77: case 83: case 90: case 91: return AcquireReason::tls_untrusted;
    case 1: case 2: case 3: case 4: case 43: case 48: return AcquireReason::downloader_unsupported;
    default: return AcquireReason::transport_failed;
  }
}

}  // namespace secure_frontend
