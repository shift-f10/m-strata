#include "secure_frontend/engine_process.hpp"
#include "secure_frontend/crt_environment.hpp"
#include "support/test_paths.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <unistd.h>
#endif

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

using test_support::current_executable;

int run_fake_engine(const int argc, char** argv) {
  if (argc != 3 || std::string(argv[2]) != "literal; exit 73") return 41;
  const char* const allowed = secure_frontend::crt_getenv("STRATA_ALLOWED_TEST");
  if (allowed == nullptr || std::string(allowed) != "present") return 42;
  // The names a real parent has (HOME/PATH on POSIX; PATH, SystemRoot, USERPROFILE on Windows): none may leak.
  for (const char* name : {"HOME", "PATH", "SystemRoot", "USERPROFILE"}) {
    if (secure_frontend::crt_getenv(name) != nullptr) return 43;
  }
  return 0;
}

void starts_without_a_shell_and_uses_only_allowlisted_environment() {
  secure_frontend::EngineProcess process;
  const secure_frontend::EngineCommand command{
      .executable = current_executable(),
      .arguments = {"--fake-engine", "literal; exit 73"},
      .environment = {{"STRATA_ALLOWED_TEST", "present"}},
  };
  const auto error = process.start(command);
  expect(!error.has_value(), "a valid fake engine should start");
  const auto result = process.wait_for(std::chrono::seconds(2));
  if (!(result.has_value() && *result == 0)) {
    std::cerr << "fake engine exit: " << (result.has_value() ? std::to_string(static_cast<unsigned>(*result)) : "none")
              << " (hex 0x" << std::hex << (result.has_value() ? static_cast<unsigned>(*result) : 0U) << std::dec << ")\n";
  }
  expect(result.has_value() && *result == 0,
         "arguments must reach the child literally, without a shell, and environment must be allowlisted");
}

// N3: the leak check above is only meaningful when the parent really has
// the names it looks for.
void proc02_engine_process_env_checks_windows_names() {
  expect(secure_frontend::crt_getenv("PATH") != nullptr, "precondition: the parent has PATH");
#if defined(_WIN32)
  expect(secure_frontend::crt_getenv("SystemRoot") != nullptr && secure_frontend::crt_getenv("USERPROFILE") != nullptr, "precondition: the parent has SystemRoot and USERPROFILE");
#endif
  starts_without_a_shell_and_uses_only_allowlisted_environment();
}

void rejects_empty_executable() {
  secure_frontend::EngineProcess process;
  const auto error = process.start({.executable = {}, .arguments = {}, .environment = {}});
  expect(error.has_value() && error->code == secure_frontend::ProcessErrorCode::invalid_command,
         "an empty executable must be rejected before process creation");
}

void rejects_malformed_allowlist_entry() {
  secure_frontend::EngineProcess process;
  const auto error = process.start(
      {.executable = current_executable(), .arguments = {}, .environment = {{"BAD=NAME", "value"}}});
  expect(error.has_value() && error->code == secure_frontend::ProcessErrorCode::invalid_command,
         "environment allowlist names must not permit an embedded equals sign");
}

void rejects_embedded_nul_in_exec_boundary() {
  secure_frontend::EngineProcess process;
  const auto executable_error = process.start(
      {.executable = std::filesystem::path{std::string{"ignored\0truncated", 17}},
       .arguments = {},
       .environment = {}});
  expect(executable_error.has_value() &&
             executable_error->code == secure_frontend::ProcessErrorCode::invalid_command,
         "executable paths must not allow embedded NUL truncation");

  const auto argument_error = process.start(
      {.executable = current_executable(),
       .arguments = {std::string{"--fake-engine\0--fake-sleeper", 28}},
       .environment = {}});
  expect(argument_error.has_value() &&
             argument_error->code == secure_frontend::ProcessErrorCode::invalid_command,
         "argv values must not allow embedded NUL truncation");
}

void termination_cleans_up_a_process_group() {
  secure_frontend::EngineProcess process;
  const secure_frontend::EngineCommand command{
      .executable = current_executable(), .arguments = {"--fake-sleeper"}, .environment = {}};
  expect(!process.start(command).has_value(), "sleeper should start");
  expect(process.terminate(std::chrono::milliseconds(200)), "termination should reap the child process group");
  const auto result = process.wait_for(std::chrono::milliseconds(1));
  expect(result.has_value(), "terminated child should have an observable exit result");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--fake-engine") return run_fake_engine(argc, argv);
  if (argc >= 2 && std::string(argv[1]) == "--fake-sleeper") {
    std::this_thread::sleep_for(std::chrono::seconds(30));
    return 0;
  }
  return secure_frontend_test::run_cases(argc, argv, {
      {"starts_without_a_shell_and_uses_only_allowlisted_environment", starts_without_a_shell_and_uses_only_allowlisted_environment},
      {"rejects_empty_executable", rejects_empty_executable},
      {"rejects_malformed_allowlist_entry", rejects_malformed_allowlist_entry},
      {"rejects_embedded_nul_in_exec_boundary", rejects_embedded_nul_in_exec_boundary},
      {"termination_cleans_up_a_process_group", termination_cleans_up_a_process_group},
      {"proc02_engine_process_env_checks_windows_names", proc02_engine_process_env_checks_windows_names},
  }, failures);
}
