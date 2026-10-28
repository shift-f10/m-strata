// SP7 M1 Task 4 (spec D-6): the uninstall planner. Pure decisions over an
// injected UninstallEnv; the loopback health probe is the only I/O, read-only.
#include "uninstall_plan.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace secure_frontend {
namespace uninstall {
namespace {

// `path` sits at or under `root` once both are canonicalized (weakly_canonical
// also resolves the parent directories of a path that is itself not on disk,
// which an uninstall of a half-installed tree must tolerate).
bool at_or_under(const std::filesystem::path& path, const std::filesystem::path& root) {
  std::error_code path_ec;
  std::error_code root_ec;
  const auto canonical_path = std::filesystem::weakly_canonical(path, path_ec);
  const auto canonical_root = std::filesystem::weakly_canonical(root, root_ec);
  if (path_ec || root_ec) return false;
  if (canonical_path == canonical_root) return true;
  // Component-wise prefix: native() is wstring on Windows, so compare the
  // paths through their components instead of their native strings.
  auto root_part = canonical_root.begin();
  auto path_part = canonical_path.begin();
  for (const auto root_end = canonical_root.end(); root_part != root_end; ++root_part, ++path_part) {
    if (path_part == canonical_path.end() || *path_part != *root_part) return false;
  }
  return path_part != canonical_path.end();
}

bool inside_any_allowed(const std::filesystem::path& path, const std::vector<std::filesystem::path>& allowed) {
  return std::any_of(allowed.begin(), allowed.end(), [&path](const std::filesystem::path& root) {
    return !root.empty() && at_or_under(path, root);  // an unset optional root bounds nothing
  });
}

}  // namespace

UninstallPlan plan_uninstall(const UninstallEnv& env) {
  UninstallPlan plan;
  const auto refuse = [&plan](const std::string& reason) {
    plan.deny_reason = reason;
    return plan;
  };

  // D-6: an install that is not where its metadata says it is is never touched.
  if (!env.install_location_ok) {
    return refuse("strata is not installed where the install metadata says it is; nothing was removed (reinstall over it or repair the metadata first)");
  }

  // The mandatory pair: without both there is no coherent uninstall (a bare
  // leftover is a manual rm of two files, and we do not guess paths).
  if (env.config_path.empty() || env.state_root.empty()) {
    return refuse("uninstall needs your config file and the state directory it names; run it with the --config you use");
  }
  if (env.unit_installed && env.unit_path.empty()) {
    return refuse("the service is reported installed but its unit file location is unknown; nothing was removed");
  }

  // D-6: every removal must sit inside an allowed root. Check before deciding
  // anything else: an escape is a configuration someone should fix knowingly.
  std::vector<std::filesystem::path> candidates;
  if (env.unit_installed) candidates.push_back(env.unit_path);
  if (!env.token_path.empty()) candidates.push_back(env.token_path);
  candidates.push_back(env.config_path);
  candidates.push_back(env.state_root);
  if (env.purge && !env.models_root.empty()) candidates.push_back(env.models_root);
  for (const auto& candidate : candidates) {
    if (!inside_any_allowed(candidate, env.allowed_roots)) {
      return refuse("refusing to remove a path outside the allowed roots: " + candidate.generic_string());
    }
  }

  // D-6: models go only with --purge, and --purge asks for a typed word the
  // scriptable --yes can never supply.
  if (env.purge && !env.purge_confirmed) {
    return refuse("--purge removes your downloaded models too: it asks you to type the confirmation word it shows; --yes does not satisfy it");
  }

  // The stop decision (the D-6 amendment: the health probe, not a lockfile).
  if (env.server_listening) {
    if (!env.unit_installed) {
      plan.stop = StopDecision::foreground_running_refuse;
      return refuse("a strata server is answering on your configured port with no service unit installed; stop it with Ctrl+C in the terminal that started it, then run this again");
    }
    if (!env.manager_available) {
      return refuse("a strata server is answering and its service unit is installed, but the user service manager is not reachable, so the server cannot be stopped safely (start a user session with a service manager; loginctl enable-linger keeps it running)");
    }
    plan.stop = StopDecision::via_service_manager;
  }

  // The ordered removal list: the service first, then the secrets, the config,
  // the state; models only under --purge, last.
  if (env.unit_installed) plan.removals.push_back(env.unit_path);
  if (!env.token_path.empty()) plan.removals.push_back(env.token_path);
  plan.removals.push_back(env.config_path);
  plan.removals.push_back(env.state_root);
  if (env.purge && !env.models_root.empty()) plan.removals.push_back(env.models_root);

  // Kept, and printed so the user knows where things still are (D-6).
  if (!env.purge && !env.models_root.empty()) plan.kept.push_back(env.models_root);
  if (!env.logs_root.empty()) plan.kept.push_back(env.logs_root);

  plan.proceed = true;
  return plan;
}

bool server_answers_on(const IpAddress& host, const std::uint16_t port) {
  const bool is_v4 = host.family() == IpFamily::v4;
#if defined(_WIN32)
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
  const SOCKET probe = WSASocketW(is_v4 ? AF_INET : AF_INET6, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0);
  const auto close_probe = [](const SOCKET s) { closesocket(s); WSACleanup(); };
#else
  const int probe = ::socket(is_v4 ? AF_INET : AF_INET6, SOCK_STREAM, 0);
  const auto close_probe = [](const int s) { ::close(s); };
#endif
#if defined(_WIN32)
  if (probe == INVALID_SOCKET) return false;
#else
  if (probe < 0) return false;
#endif
#if defined(_WIN32)
  const DWORD timeout_ms = 2000;
  (void)setsockopt(probe, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof timeout_ms);
  (void)setsockopt(probe, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof timeout_ms);
#else
  const timeval timeout{2, 0};
  (void)setsockopt(probe, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  (void)setsockopt(probe, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
#endif
  bool answered = false;
  if (is_v4) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    std::memcpy(&address.sin_addr, host.bytes().data(), 4U);
    answered = ::connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
  } else {
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(port);
    std::memcpy(address.sin6_addr.s6_addr, host.bytes().data(), 16U);
    answered = ::connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
  }
  if (answered) {
    static const char request[] = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    answered = ::send(probe, request, static_cast<int>(sizeof request - 1U), 0) > 0;
    char buffer[16]{};
    const int got = answered ? ::recv(probe, buffer, sizeof buffer, 0) : 0;
    answered = got >= 8 && std::string_view(buffer, static_cast<std::size_t>(got)).compare(0, 5, "HTTP/") == 0;
  }
  close_probe(probe);
  return answered;
}

}  // namespace uninstall
}  // namespace secure_frontend
