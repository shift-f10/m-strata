// SP7 M1 Task 4 (spec D-6): the uninstall plan — ordered removals under allowed
// roots only, the server-stop decision from the loopback health probe, and the
// --purge typed-word gate. Every case is the pure planner over an injected
// UninstallEnv; the one e2e case puts a real canned HTTP listener on loopback.
#include "uninstall_plan.hpp"

#include "secure_frontend/config.hpp"
#include "support/test_support.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
using NativeSocket = SOCKET;
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using NativeSocket = int;
#endif

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
bool contains(const std::string& haystack, std::string_view needle) { return haystack.find(needle) != std::string::npos; }

using namespace secure_frontend::uninstall;
using secure_frontend::parse_listener_address;

// Scratch roots under one private parent; every path is absolute and simple.
struct Fixture final {
  std::filesystem::path base;
  std::filesystem::path unit_dir;
  std::filesystem::path secret_dir;
  std::filesystem::path config_file;
  std::filesystem::path state_dir;
  std::filesystem::path models_dir;
  std::filesystem::path logs_dir;
  Fixture() {
    base = secure_frontend_test::make_scratch_directory();
    unit_dir = base / "config" / "systemd" / "user";
    secret_dir = base / "secret";
    config_file = base / "config" / "strata.toml";
    state_dir = base / "state";
    models_dir = base / "models";
    logs_dir = base / "logs";
  }
  // The default honest picture: everything installed, service enabled, no
  // server answering, install metadata matching.
  UninstallEnv env() const {
    UninstallEnv e;
    e.unit_path = unit_dir / "strata.service";
    e.token_path = secret_dir / "token";
    e.config_path = config_file;
    e.state_root = state_dir;
    e.models_root = models_dir;
    e.logs_root = logs_dir;
    e.allowed_roots = {base / "config", secret_dir, state_dir, models_dir, logs_dir};
    e.unit_installed = true;
    e.manager_available = true;
    return e;
  }
};

bool equals(const std::vector<std::filesystem::path>& got, const std::vector<std::filesystem::path>& want) {
  return got == want;
}

void uninstall_plan_no_listener_orders_removals() {
  const Fixture f;
  const UninstallPlan plan = plan_uninstall(f.env());
  expect(plan.proceed, "a quiet uninstall proceeds");
  expect(plan.stop == StopDecision::none, "no listener means nothing to stop");
  expect(equals(plan.removals, {f.env().unit_path, f.env().token_path, f.env().config_path, f.env().state_root}),
         "removals are exactly unit, token, config, state in that order");
  expect(equals(plan.kept, {f.env().models_root, f.env().logs_root}), "models and logs are kept and reported");
}

void uninstall_plan_listener_with_unit_stops_via_manager() {
  Fixture f;
  UninstallEnv e = f.env();
  e.server_listening = true;
  const UninstallPlan plan = plan_uninstall(e);
  expect(plan.proceed, "an installed service can be stopped through its manager");
  expect(plan.stop == StopDecision::via_service_manager, "the server is stopped via the service manager first");
  expect(plan.removals.size() == 4U, "the same four removals follow the stop");
}

void uninstall_plan_listener_without_unit_refuses_with_ctrlc_hint() {
  Fixture f;
  UninstallEnv e = f.env();
  e.server_listening = true;
  e.unit_installed = false;
  e.unit_path.clear();
  const UninstallPlan plan = plan_uninstall(e);
  expect(!plan.proceed, "a foreground serve blocks the uninstall");
  expect(plan.stop == StopDecision::foreground_running_refuse, "the decision names the foreground server");
  expect(contains(plan.deny_reason, "Ctrl+C"), "the refusal tells the user to press Ctrl+C");
}

void uninstall_plan_listener_with_unit_but_no_manager_refuses() {
  Fixture f;
  UninstallEnv e = f.env();
  e.server_listening = true;
  e.manager_available = false;
  const UninstallPlan plan = plan_uninstall(e);
  expect(!plan.proceed, "no user manager means we cannot run the service stop");
  expect(contains(plan.deny_reason, "manager"), "the refusal says why: the service manager is unreachable");
}

void uninstall_plan_purge_needs_the_typed_word() {
  Fixture f;
  UninstallEnv e = f.env();
  e.purge = true;
  const UninstallPlan blocked = plan_uninstall(e);
  expect(!blocked.proceed, "--purge without the typed word does not proceed");
  expect(contains(blocked.deny_reason, "type"), "the refusal asks for a typed confirmation");
  expect(contains(blocked.deny_reason, "--yes"), "the refusal says --yes does not satisfy it");

  UninstallEnv ok = f.env();
  ok.purge = true;
  ok.purge_confirmed = true;
  const UninstallPlan plan = plan_uninstall(ok);
  expect(plan.proceed, "--purge with the typed word proceeds");
  expect(plan.removals.size() == 5U && plan.removals.back() == ok.models_root, "models join the removals last under --purge");
  expect(equals(plan.kept, {ok.logs_root}), "logs are kept even under --purge");
}

void uninstall_plan_refuses_paths_outside_allowed_roots() {
  Fixture f;
  UninstallEnv e = f.env();
  e.token_path = std::filesystem::path("/etc") / "strata-token";
  const UninstallPlan plan = plan_uninstall(e);
  expect(!plan.proceed, "a removal outside the allowed roots is refused");
  expect(contains(plan.deny_reason, "allowed roots"), "the refusal names the allowed-roots rule");
}

void uninstall_plan_missing_optional_paths_are_skipped() {
  Fixture f;
  UninstallEnv e = f.env();
  e.unit_installed = false;
  e.unit_path.clear();
  e.token_path.clear();  // an env: token source has no file of ours
  const UninstallPlan plan = plan_uninstall(e);
  expect(plan.proceed, "a plain install can still be uninstalled");
  expect(equals(plan.removals, {e.config_path, e.state_root}), "only the config and the state dir are listed");
}

void uninstall_plan_refuses_when_install_metadata_mismatches() {
  Fixture f;
  UninstallEnv e = f.env();
  e.install_location_ok = false;
  const UninstallPlan plan = plan_uninstall(e);
  expect(!plan.proceed, "an install that is not where the metadata says is refused");
  expect(contains(plan.deny_reason, "install"), "the refusal speaks about the install location");
}

// --- the loopback health probe e2e (the fake server up / down) ----------------

void close_native(const NativeSocket socket) {
#if defined(_WIN32)
  closesocket(socket);
#else
  close(socket);
#endif
}

// One canned HTTP response on 127.0.0.1:<port>, then closes. The answer we care
// about is the 401: an unauthenticated GET /health still proves the server answers.
struct CannedServer final {
  NativeSocket listener;
  std::uint16_t port{0};
  std::thread thread;
  explicit CannedServer(std::string response) {
#if defined(_WIN32)
    WSADATA wsa{};
    (void)WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    (void)listen(listener, 1);
    socklen_t length = sizeof(address);
    (void)getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
    port = ntohs(address.sin_port);
    thread = std::thread([this, response = std::move(response)] {
      const NativeSocket client = accept(listener, nullptr, nullptr);
#if defined(_WIN32)
      if (client == INVALID_SOCKET) return;
#else
      if (client < 0) return;
#endif
      char buffer[1024];
      std::string request;
      while (request.find("\r\n\r\n") == std::string::npos) {
        const int got = recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        request.append(buffer, static_cast<std::size_t>(got));
      }
      (void)send(client, response.data(), static_cast<int>(response.size()), 0);
      close_native(client);
    });
  }
  ~CannedServer() {
    thread.join();
    close_native(listener);
  }
};

void uninstall_health_probe_answers_on_a_listener_and_not_without_one() {
  const auto loopback = parse_listener_address("127.0.0.1");
  expect(loopback.has_value(), "loopback parses");
  std::uint16_t up_port = 0;
  {
    const CannedServer server{"HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"};
    up_port = server.port;
    expect(server_answers_on(*loopback, up_port), "a 401 from our server counts as listening");
  }
  // The canned socket is fully closed now: nothing answers on that port anymore.
  expect(!server_answers_on(*loopback, up_port), "after the server closes the probe says no");
}
}  // namespace

int main(const int argc, char** const argv) {
  const std::vector<std::pair<std::string, void (*)()>> cases = {
      {"uninstall_plan_no_listener_orders_removals", uninstall_plan_no_listener_orders_removals},
      {"uninstall_plan_listener_with_unit_stops_via_manager", uninstall_plan_listener_with_unit_stops_via_manager},
      {"uninstall_plan_listener_without_unit_refuses_with_ctrlc_hint", uninstall_plan_listener_without_unit_refuses_with_ctrlc_hint},
      {"uninstall_plan_listener_with_unit_but_no_manager_refuses", uninstall_plan_listener_with_unit_but_no_manager_refuses},
      {"uninstall_plan_purge_needs_the_typed_word", uninstall_plan_purge_needs_the_typed_word},
      {"uninstall_plan_refuses_paths_outside_allowed_roots", uninstall_plan_refuses_paths_outside_allowed_roots},
      {"uninstall_plan_missing_optional_paths_are_skipped", uninstall_plan_missing_optional_paths_are_skipped},
      {"uninstall_plan_refuses_when_install_metadata_mismatches", uninstall_plan_refuses_when_install_metadata_mismatches},
      {"uninstall_health_probe_answers_on_a_listener_and_not_without_one", uninstall_health_probe_answers_on_a_listener_and_not_without_one},
  };
  if (argc >= 3 && std::string(argv[1]) == "--case") {
    const std::string want = argv[2];
    for (const auto& [name, body] : cases) {
      if (name == want) {
        body();
        return failures == 0 ? 0 : 1;
      }
    }
    std::cerr << "no such case: " << want << '\n';
    return 2;
  }
  for (const auto& [name, body] : cases) {
    failures = 0;
    body();
    std::cout << name << (failures == 0 ? ": ok" : ": FAILED") << '\n';
  }
  return failures == 0 ? 0 : 1;
}
