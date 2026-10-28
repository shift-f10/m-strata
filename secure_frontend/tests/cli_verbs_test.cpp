// SP7 M1 Task 5: the `strata` CLI verbs — service {enable,disable,status} and
// uninstall [--purge] — each driven as a real child process against temp roots
// (a temp HOME/XDG, a PATH containing only a fake systemctl, a typed word on
// stdin, a canned HTTP listener for the health probe). No real systemd, ever.
#include "support/config_json.hpp"
#include "support/process_runner.hpp"
#include "support/test_support.hpp"

#include <cstdint>
#include <atomic>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
using namespace secure_frontend_test;
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

std::filesystem::path cli_binary;

void note(const std::string& what, const ProcessResult& result) {
  std::cerr << "  [" << what << "] exit=" << result.exit_code << " out=" << result.out << " err=" << result.err << "\n";
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// The honest temp picture: a validated config under a temp HOME, a private
// secret dir with the token file, an existing state dir, models and logs dirs.
struct Scene final {
  std::filesystem::path scratch;
  std::filesystem::path home;
  std::filesystem::path xdg;
  std::filesystem::path config;
  std::filesystem::path models;
  std::filesystem::path logs;
  std::filesystem::path secret;
  std::filesystem::path token;
  std::filesystem::path state;
  std::filesystem::path unit;
  std::uint16_t port{0};
  std::vector<std::pair<std::string, std::string>> env;
  std::vector<std::string> hooks_args;  // hooks-only global options, before --config (e.g. --test-system-dir)

  explicit Scene(const std::uint16_t listener_port) {
    scratch = make_scratch_directory();
    home = scratch / "home";
    xdg = home / ".config";
    std::filesystem::create_directories(home);
    models = scratch / "models";
    make_private_directory(models);
    logs = scratch / "logs";
    make_private_directory(logs);
    secret = scratch / "secret";
    make_private_directory(secret);
    token = secret / "token";
    write_file(token, "tok3n", 0600);
    state = scratch / "state";
    make_private_directory(state);
    const auto engine = scratch / "engine";
    write_file(engine, "binary", 0700);
    port = listener_port;
    config = scratch / "config.json";
    write_file(config, render_config_json({
                           {"schema_version", "1"},
                           {"listener.port", std::to_string(port)},
                           {"roots.models", json_literal(models.string())},
                           {"roots.state", json_literal(state.string())},
                           {"roots.logs", json_literal(logs.string())},
                           {"roots.secret", json_literal(secret.string())},
                           {"engine.executable", json_literal(engine.string())},
                           {"engine.model_id", json_literal("m:t")},
                           {"token.source", json_literal("file:token")},
                       }));
    unit = xdg / "systemd" / "user" / "strata.service";
    env = {{"HOME", home.string()}, {"XDG_CONFIG_HOME", xdg.string()}, {"PATH", "/bin:/usr/bin"}};
  }
  ProcessResult run(const std::vector<std::string>& verb_args, const std::string& stdin_text = "") const {
    ProcessSpec spec{cli_binary, hooks_args, {}};
    spec.arguments.push_back("--config");
    spec.arguments.push_back(config.string());
    spec.environment = env;
    if (!stdin_text.empty()) {
      const auto stdin_path = scratch / "stdin";
      write_file(stdin_path, stdin_text, 0600);
      spec.stdin_file = stdin_path;
    }
    for (const auto& argument : verb_args) spec.arguments.push_back(argument);
    return run_to_completion(spec);
  }
};

#if !defined(_WIN32)
// The fake systemctl: every argv recorded; is-enabled answers like a fresh
// install (exit 1, "No such file"), enable/disable succeed.
void write_systemctl_shim(const std::filesystem::path& dir, const std::filesystem::path& record) {
  const auto shim = dir / "systemctl";
  std::ofstream script(shim);
  script << "#!/bin/sh\n"
         << "printf '%s ' \"$@\" >> \"" << record.string() << "\"\n"
         << "printf '\\n' >> \"" << record.string() << "\"\n"
         << "for a in \"$@\"; do\n"
         << "  if [ \"$a\" = \"is-enabled\" ]; then\n"
         << "    echo \"Failed to read unit strata.service: No such file or directory\" >&2\n"
         << "    exit 1\n"
         << "  fi\n"
         << "done\n"
         << "exit 0\n";
  script.close();
  chmod(shim.c_str(), 0755);
}

void cli_service_enable_status_disable_roundtrip() {
  const Scene s(18443);
  const auto shim_dir = s.scratch / "shim";
  std::filesystem::create_directories(shim_dir);
  const auto record = s.scratch / "systemctl.calls";
  write_systemctl_shim(shim_dir, record);
  Scene scene = s;
  scene.env = {{"HOME", scene.home.string()}, {"XDG_CONFIG_HOME", scene.xdg.string()}, {"PATH", shim_dir.string()}};

  const auto enable = scene.run({"service", "enable"});
  if (enable.exit_code != 0) note("enable", enable);
  expect(enable.exit_code == 0, "service enable exits 0 with the fake systemctl present");
  std::error_code ec;
  if (!std::filesystem::exists(scene.unit, ec)) {
    expect(false, "the unit file was written under XDG_CONFIG_HOME/systemd/user");
  } else {
    const std::string text = read_text(scene.unit);
    expect(text.find("ExecStart=") != std::string::npos, "the written unit carries ExecStart");
    expect(text.find("NoNewPrivileges=yes") != std::string::npos, "the written unit is the hardened golden text");
  }
  const std::string calls = read_text(record);
  expect(calls.find("enable --now") != std::string::npos, "systemctl --user enable --now was called");

  const auto status = scene.run({"service", "status"});
  expect(status.exit_code == 0, "service status exits 0");
  expect(status.out.find("service: enabled:") != std::string::npos, "status prints the enabled line");
  expect(status.out.find("service: active:") != std::string::npos, "status prints the active line");

  const auto disable = scene.run({"service", "disable"});
  expect(disable.exit_code == 0, "service disable exits 0");
  expect(!std::filesystem::exists(scene.unit), "disable removed our unit file");
}

void cli_service_enable_without_user_manager() {
  const Scene s(18444);
  const auto enable = s.run({"service", "enable"});
  expect(enable.exit_code != 0, "no reachable user manager is a non-zero exit");
  std::error_code ec;
  expect(!std::filesystem::exists(s.unit, ec), "nothing was written to the unit path");
  const bool hinted = enable.out.find("loginctl enable-linger") != std::string::npos ||
                      enable.err.find("loginctl enable-linger") != std::string::npos;
  expect(hinted, "the answer names the loginctl enable-linger note");
}

void cli_uninstall_plain_removes_and_keeps() {
  const Scene s(18445);
  const auto out = s.run({"uninstall"});
  if (out.exit_code != 0) note("uninstall", out);
  expect(out.exit_code == 0, "a quiet uninstall exits 0");
  std::error_code ec;
  expect(!std::filesystem::exists(s.token, ec), "the token file is gone");
  expect(!std::filesystem::exists(s.config, ec), "the config file is gone");
  expect(!std::filesystem::exists(s.state, ec), "the state dir is gone");
  expect(std::filesystem::exists(s.models, ec), "models are kept by default");
  expect(std::filesystem::exists(s.logs, ec), "logs are kept");
  expect(out.out.find(s.models.generic_string()) != std::string::npos, "the kept models path is printed");
  expect(out.out.find(s.logs.generic_string()) != std::string::npos, "the kept logs path is printed");
}

void cli_uninstall_purge_without_the_word_refuses() {
  const Scene s(18446);
  const auto out = s.run({"uninstall", "--purge"});  // stdin is the null device
  expect(out.exit_code != 0, "--purge without the typed word exits non-zero");
  std::error_code ec;
  expect(std::filesystem::exists(s.models, ec) && std::filesystem::exists(s.config, ec), "nothing was removed");
  const bool typed_hint = out.out.find("type") != std::string::npos || out.err.find("type") != std::string::npos;
  expect(typed_hint, "the answer asks for a typed confirmation");
}

void cli_uninstall_purge_with_typed_word_removes_models() {
  const Scene s(18447);
  const auto out = s.run({"uninstall", "--purge"}, "purge\n");
  if (out.exit_code != 0) note("purge", out);
  expect(out.exit_code == 0, "--purge with the typed word exits 0");
  std::error_code ec;
  expect(!std::filesystem::exists(s.models, ec), "models were removed");
  expect(!std::filesystem::exists(s.state, ec), "state was removed too");
  expect(std::filesystem::exists(s.logs, ec), "logs stay even under --purge");
}
#endif  // !_WIN32

#if defined(_WIN32)
// The Task Scheduler roundtrip as a real CLI run (windows-runtime label: runs
// only in a Windows test build — the owner's box; the cross gate compiles it).
// Its schtasks is the fake tool in a test directory (hooks-only --test-system-dir):
// strata runs a bare tool name from System32 only, so a PATH shim is never found —
// the .cmd shim this case used before never ran, and the REAL schtasks did
// (session log 2026-10-21; its /Create failed, so nothing was registered).
void cli_service_enable_status_disable_taskscheduler() {
  const Scene s(18448);
  const auto shim_dir = s.scratch / "shim";
  std::filesystem::create_directories(shim_dir);
  std::filesystem::copy_file(STRATA_FAKE_SYSTEM_TOOL_PATH, shim_dir / "schtasks.exe");
  const auto record = shim_dir / "schtasks.calls";
  Scene scene = s;
  scene.env = {{"HOME", scene.home.string()}, {"XDG_CONFIG_HOME", scene.xdg.string()}};
  scene.hooks_args = {"--test-system-dir", shim_dir.string()};
  const auto task_file = scene.xdg / "strata" / "strata-task.xml";

  const auto enable = scene.run({"service", "enable"});
  if (enable.exit_code != 0) note("enable", enable);
  expect(enable.exit_code == 0, "service enable exits 0 with the fake schtasks present");
  std::error_code ec;
  expect(std::filesystem::exists(task_file, ec), "the task XML was written under the user config home");
  expect(contains(read_text(record), "/Create /F /TN strata /XML"), "schtasks /Create /F /XML ran");

  const auto status = scene.run({"service", "status"});
  expect(status.exit_code == 0, "service status exits 0");
  expect(status.out.find("service: enabled:") != std::string::npos, "status prints the enabled line");

  const auto disable = scene.run({"service", "disable"});
  expect(disable.exit_code == 0, "service disable exits 0");
  expect(!std::filesystem::exists(task_file, ec), "disable removed our task file");
  expect(contains(read_text(record), "/End /TN strata\n/Delete /F /TN strata\n"), "disable ran schtasks /End then /Delete /F");
}
#endif  // _WIN32

void cli_uninstall_refuses_while_a_server_answers() {
  // A canned HTTP listener stands in for a foreground serve; the CLI's health
  // probe must refuse the uninstall with the Ctrl+C message and change nothing.
#if !defined(_WIN32)
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  expect(listener >= 0, "probe listener socket");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  (void)bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  (void)listen(listener, 2);
  socklen_t length = sizeof(address);
  (void)getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
  const std::uint16_t port = ntohs(address.sin_port);
  const Scene s(port);
  std::atomic<bool> stop{false};
  auto accept_loop = [&]() {
    while (!stop) {
      const int client = accept(listener, nullptr, nullptr);
      if (client < 0) return;
      (void)send(client, "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n", 45, 0);
      close(client);
    }
  };
  std::thread server_thread(accept_loop);
  const auto out = s.run({"uninstall"});
  stop = true;
  (void)shutdown(listener, SHUT_RDWR);  // wakes the blocked accept on POSIX
  close(listener);
  server_thread.join();
  expect(out.exit_code != 0, "an answering server blocks the uninstall");
  const bool ctrlc = out.out.find("Ctrl+C") != std::string::npos || out.err.find("Ctrl+C") != std::string::npos;
  expect(ctrlc, "the refusal tells the user to stop the foreground server with Ctrl+C");
  std::error_code ec;
  expect(std::filesystem::exists(s.config, ec) && std::filesystem::exists(s.state, ec) && std::filesystem::exists(s.token, ec),
         "nothing was removed while the server answers");
#else
  // The Windows runtime proof of this path waits for the owner's box (Task 6).
#endif
}

void cli_usage_lists_the_new_verbs() {
  const auto out = run_to_completion(ProcessSpec{cli_binary, {"--help"}, {}});
  expect(out.exit_code == 0, "--help exits 0");
  expect(out.out.find("service enable|disable|status") != std::string::npos, "usage lists the service verbs");
  expect(out.out.find("uninstall [--purge]") != std::string::npos, "usage lists uninstall");
  expect(out.out.find("doctor [--downloader] [--deep]") != std::string::npos, "usage shows doctor --deep");
}
}  // namespace

int main(const int argc, char** const argv) {
  std::string want_case;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == "--case") want_case = argv[i + 1];
  }
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) != "--case" && argv[i] != want_case) cli_binary = argv[i];
  }
  if (cli_binary.empty()) {
    std::cerr << "usage: cli_verbs_test [--case <name>] <strata>\n";
    return 2;
  }
  const std::vector<std::pair<std::string, void (*)()>> cases = {
      {"cli_usage_lists_the_new_verbs", cli_usage_lists_the_new_verbs},
      {"cli_uninstall_refuses_while_a_server_answers", cli_uninstall_refuses_while_a_server_answers},
#if !defined(_WIN32)
      {"cli_service_enable_status_disable_roundtrip", cli_service_enable_status_disable_roundtrip},
      {"cli_service_enable_without_user_manager", cli_service_enable_without_user_manager},
      {"cli_uninstall_plain_removes_and_keeps", cli_uninstall_plain_removes_and_keeps},
      {"cli_uninstall_purge_without_the_word_refuses", cli_uninstall_purge_without_the_word_refuses},
      {"cli_uninstall_purge_with_typed_word_removes_models", cli_uninstall_purge_with_typed_word_removes_models},
#endif
#if defined(_WIN32)
      {"cli_service_enable_status_disable_taskscheduler", cli_service_enable_status_disable_taskscheduler},
#endif
  };
  if (!want_case.empty()) {
    for (const auto& [name, body] : cases) {
      if (name == want_case) {
        body();
        return failures == 0 ? 0 : 1;
      }
    }
    std::cerr << "no such case: " << want_case << '\n';
    return 2;
  }
  for (const auto& [name, body] : cases) {
    failures = 0;
    body();
    std::cout << name << (failures == 0 ? ": ok" : ": FAILED") << '\n';
  }
  return failures == 0 ? 0 : 1;
}
