#include "secure_frontend/engine_protocol_client.hpp"
#include "secure_frontend/engine_stdio_process.hpp"

#include "support/fake_engine_dump.hpp"
#include "support/test_paths.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

void expect(const bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void test_round_trip(const std::filesystem::path& fake) {
  secure_frontend::EngineStdioProcess transport;
  secure_frontend::EngineCommand command{.executable = fake, .arguments = {"--fake-engine"}, .environment = {}};
  const auto start_error = transport.start(command);
  expect(!start_error.has_value(), "fake engine must start");
  secure_frontend::EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  expect(!client.await_ready(std::chrono::seconds(2), ready).has_value(), "READY must parse");
  expect(ready.max_context_tokens == 4096 && ready.supports_stop, "READY capabilities must survive transport");
  // E4: the GEN line for a full context is far beyond 8 KiB; the write cap must allow it.
  std::string big(100000U, '1');
  std::string werr;
  const bool big_written = transport.write_line("GEN 1 " + big, secure_frontend::deadline_after(std::chrono::seconds(5)), werr);
  expect(big_written, "a 100 KB GEN line must be writable: " + werr);
  const auto reply_token = transport.read_line(std::chrono::seconds(2));
  const auto reply_done = transport.read_line(std::chrono::seconds(2));
  expect(reply_token.kind == secure_frontend::EngineReadKind::line && reply_token.line == "T 7" &&
             reply_done.kind == secure_frontend::EngineReadKind::line && reply_done.line == "DONE stop",
         "the fake engine must answer the 100 KB GEN line");
  expect(!client.begin_generation(2, {1, 2, 3}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(), "GEN must write through transport");
  secure_frontend::EngineGeneration generation{};
  expect(!client.read_generation(std::chrono::seconds(2), generation).has_value(), "generation must read through transport");
  expect(generation.token_ids == std::vector<std::uint32_t>{7}, "token must survive transport");
  expect(!client.quit().has_value(), "QUIT must write through transport");
  expect(transport.terminate(std::chrono::milliseconds(250)), "child cleanup must succeed");
}

void test_invalid_record() {
  secure_frontend::EngineStdioProcess transport;
  std::string error;
  expect(!transport.write_line("bad\nline", secure_frontend::deadline_after(std::chrono::seconds(5)), error), "multiline writes must be rejected");
}

// R1: a record whose overall deadline has already passed is not written; the
// write fails the same way as a stalled chunk ("not writable").
void write_line_fails_once_deadline_passed(const std::filesystem::path& fake) {
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine"}, .environment = {}}).has_value(), "fake engine must start");
  (void)transport.read_line(std::chrono::seconds(2));  // READY
  std::string error;
  const auto passed = secure_frontend::MonotonicClock::now() - std::chrono::milliseconds(1);
  expect(!transport.write_line("GEN 1 1", passed, error) && error == "engine stdin is not writable", "a passed deadline fails the write: " + error);
  expect(transport.read_line(std::chrono::milliseconds(300)).kind == secure_frontend::EngineReadKind::timeout, "nothing reached the engine");
  (void)transport.terminate(std::chrono::milliseconds(250));
}

void read_line_rejects_oversize_on_all_platforms(const std::filesystem::path& fake) {
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine", "--spew"}, .environment = {}}).has_value(),
         "fake engine must start");
  (void)transport.read_line(std::chrono::seconds(2));  // READY
  const auto result = transport.read_line(std::chrono::seconds(2));
  expect(result.kind == secure_frontend::EngineReadKind::failure, "an oversized engine line must be a failure on every platform");
  (void)transport.terminate(std::chrono::milliseconds(250));
}

void read_line_waits_for_a_record_split_across_writes(const std::filesystem::path& fake) {
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine", "--split-ready"}, .environment = {}}).has_value(),
         "fake engine must start");
  const auto ready = transport.read_line(std::chrono::seconds(2));
  expect(ready.kind == secure_frontend::EngineReadKind::line && ready.line == "READY 4096 stop",
         "a record split across writes must be read whole within the budget, not time out early");
  (void)transport.terminate(std::chrono::milliseconds(250));
}

#if defined(_WIN32)
void job_object_terminates_grandchild(const std::filesystem::path& fake) {
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine", "--spawn-grandchild"}, .environment = {}})
              .has_value(),
         "fake engine must start");
  const auto ready = transport.read_line(std::chrono::seconds(5));
  expect(ready.kind == secure_frontend::EngineReadKind::line && ready.line == "READY 4096 stop", "READY must arrive");
  const auto report = transport.read_line(std::chrono::seconds(5));
  expect(report.kind == secure_frontend::EngineReadKind::line && report.line.rfind("GRANDCHILD ", 0) == 0,
         "the fake must report its grandchild");
  const unsigned long pid = std::stoul(report.line.substr(11));
  expect(pid != 0, "the grandchild must have started");
  // The handle pins the process object, so no PID reuse can confuse the check.
  HANDLE grandchild = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  expect(grandchild != nullptr, "the live grandchild must be openable before terminate");
  (void)transport.terminate(std::chrono::milliseconds(250));
  const bool gone = WaitForSingleObject(grandchild, 2000) == WAIT_OBJECT_0;
  CloseHandle(grandchild);
  expect(gone, "the grandchild must die with the Job (P4)");
}
#endif

#if !defined(_WIN32)
void child_inherits_only_stdio_descriptors(const std::filesystem::path& fake) {
  // Two extra descriptors without CLOEXEC in the parent; a correct launcher must not leak them.
  const int leak_a = open("/dev/null", O_RDONLY);
  const int leak_b = open("/dev/null", O_RDONLY);
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine"}, .environment = {}}).has_value(),
         "fake engine must start");
  (void)transport.read_line(std::chrono::seconds(2));  // READY
  const auto dump = test_support::request_dump(transport, std::chrono::seconds(2));
  close(leak_a);
  close(leak_b);
  expect(dump.has_value() && dump->fds == std::vector<int>{0, 1, 2}, "child must see only stdio descriptors");
  (void)transport.terminate(std::chrono::milliseconds(250));
}

void child_receives_exact_argv_and_env(const std::filesystem::path& fake) {
  secure_frontend::EngineStdioProcess transport;
  const std::string weird = "literal; $(echo x) \"quoted\" \xE2\x9C\x93 \n-less";
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine", weird}, .environment = {{"STRATA_T", "1"}}})
              .has_value(),
         "fake engine must start");
  (void)transport.read_line(std::chrono::seconds(2));  // READY
  const auto dump = test_support::request_dump(transport, std::chrono::seconds(2));
  expect(dump.has_value(), "DUMP must parse");
  if (dump) {
    const std::vector<std::string> expected{fake.string(), "--serve", "--fake-engine", weird};
    expect(dump->argv == expected, "argv must arrive literally, no shell");
    expect(dump->env == std::vector<std::string>{"STRATA_T=1"}, "the environment is exactly the allowlist passed in (N2)");
  }
  (void)transport.terminate(std::chrono::milliseconds(250));
}

void terminate_reaps_killed_child(const std::filesystem::path& fake) {
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine", "--ignore-term"}, .environment = {}}).has_value(),
         "fake engine must start");
  (void)transport.read_line(std::chrono::seconds(2));  // READY: SIGTERM is now ignored
  const pid_t pid = transport.pid_for_tests();
  expect(pid > 0, "the fake engine must have a pid");
  expect(transport.terminate(std::chrono::milliseconds(200)), "terminate must succeed even when SIGTERM is ignored");
  expect(transport.pid_for_tests() == -1, "terminate must clear the child pid once it is reaped");
  errno = 0;
  const pid_t waited = waitpid(pid, nullptr, WNOHANG);
  expect(waited == -1 && errno == ECHILD, "the killed child must already be reaped by terminate");
  std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
  const std::string content((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
  expect(content.empty() || content.find(") Z ") == std::string::npos, "no zombie may remain after terminate, saw: " + content.substr(0, 64));
}
#endif

}  // namespace

// SP1 Task 2's case list, unchanged; only the engine is now the standalone fake.
int main(int argc, char* argv[]) {
  static const int no_counted_failures = 0;  // expect() throws; run_cases reports the exception
  static const auto fake = test_support::fake_engine_path();
  return secure_frontend_test::run_cases(argc, argv, {
      {"test_round_trip", [] { test_round_trip(fake); }},
      {"test_invalid_record", [] { test_invalid_record(); }},
      {"write_line_fails_once_deadline_passed", [] { write_line_fails_once_deadline_passed(fake); }},
      {"read_line_rejects_oversize_on_all_platforms", [] { read_line_rejects_oversize_on_all_platforms(fake); }},
      {"read_line_waits_for_a_record_split_across_writes", [] { read_line_waits_for_a_record_split_across_writes(fake); }},
#if defined(_WIN32)
      {"job_object_terminates_grandchild", [] { job_object_terminates_grandchild(fake); }},
#else
      {"child_inherits_only_stdio_descriptors", [] { child_inherits_only_stdio_descriptors(fake); }},
      {"child_receives_exact_argv_and_env", [] { child_receives_exact_argv_and_env(fake); }},
      {"terminate_reaps_killed_child", [] { terminate_reaps_killed_child(fake); }},
#endif
  }, no_counted_failures);
}
