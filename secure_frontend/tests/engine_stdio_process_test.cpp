#include "secure_frontend/engine_protocol_client.hpp"
#include "secure_frontend/engine_stdio_process.hpp"

#include "support/fake_engine_dump.hpp"
#include "support/test_paths.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <thread>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
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

// --- the Monitor (owner 2026-10-28): the engine's memory, its conversation cache from its stderr ---------------
// The payload the fake writes to its stderr, built at run time from its lower-case spelling as the fake builds it, so
// this binary holds no copy of it but this buffer.
struct Needle final {
  std::array<char, 19> bytes{};
  Needle() {
    static const char lower[] = "usersecretpayloadqz";
    volatile unsigned char flip = 0x20U;
    for (std::size_t index = 0; index < bytes.size(); ++index) bytes[index] = static_cast<char>(static_cast<unsigned char>(lower[index]) ^ flip);
  }
};

bool region_holds(const unsigned char* begin, const unsigned char* end, const Needle& needle) {
  const auto* skip = reinterpret_cast<const unsigned char*>(needle.bytes.data());
  const unsigned char first = static_cast<unsigned char>(needle.bytes[0]);
  for (const unsigned char* at = begin; at + needle.bytes.size() <= end; ++at) {
    if (*at != first || (at >= skip && at < skip + needle.bytes.size())) continue;
    if (std::memcmp(at, needle.bytes.data(), needle.bytes.size()) == 0) return true;
  }
  return false;
}

// Every writable private page of this process (the heap, the stacks, the globals): does it hold the payload?
bool this_process_holds(const Needle& needle) {
#if defined(_WIN32)
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  auto* address = static_cast<unsigned char*>(system.lpMinimumApplicationAddress);
  MEMORY_BASIC_INFORMATION region{};
  while (address < static_cast<unsigned char*>(system.lpMaximumApplicationAddress) && VirtualQuery(address, &region, sizeof(region)) == sizeof(region)) {
    auto* const base = static_cast<unsigned char*>(region.BaseAddress);
    const bool writable = (region.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) != 0U && (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0U;
    if (region.State == MEM_COMMIT && writable && region_holds(base, base + region.RegionSize, needle)) return true;
    address = base + region.RegionSize;
  }
  return false;
#else
  std::ifstream maps("/proc/self/maps");
  for (std::string line; std::getline(maps, line);) {
    unsigned long long from = 0;
    unsigned long long to = 0;
    char perms[5]{};
    if (std::sscanf(line.c_str(), "%llx-%llx %4s", &from, &to, perms) != 3 || perms[0] != 'r' || perms[1] != 'w') continue;
    if (line.find("[vvar") != std::string::npos || line.find("[vsyscall]") != std::string::npos) continue;
    if (region_holds(reinterpret_cast<const unsigned char*>(from), reinterpret_cast<const unsigned char*>(to), needle)) return true;
  }
  return false;
#endif
}

void wait_ready(secure_frontend::EngineStdioProcess& transport, const std::chrono::seconds bound) {
  const auto until = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < until) {
    const auto read = transport.read_line(std::chrono::milliseconds(200));
    if (read.kind == secure_frontend::EngineReadKind::line && read.line.rfind("READY ", 0) == 0) return;
    expect(read.kind == secure_frontend::EngineReadKind::line || read.kind == secure_frontend::EngineReadKind::timeout, "the fake engine must stay up");
  }
  throw std::runtime_error("no READY within the bound");
}

// Upstream's conversation cache lines (ConvCacheLog) reach the Monitor as numbers; every other stderr line - one with
// a user-text-looking payload among them - is dropped, and nothing of it stays in this process.
void stderr_gives_only_the_conversation_cache_numbers(const std::filesystem::path& fake) {
  const Needle needle;
  secure_frontend::ConversationCacheState state;
  {
    secure_frontend::EngineStdioProcess transport;
    expect(!transport.start({.executable = fake, .arguments = {"--fake-engine", "--stderr-cache-lines"}, .environment = {},
                             .read_conversation_cache = true})
                .has_value(),
           "fake engine must start");
    wait_ready(transport, std::chrono::seconds(10));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < until) {
      const auto seen = transport.conversation_cache();
      expect(seen.has_value(), "the reader runs for this launch");
      state = *seen;
      if (state.parks == 2U && state.restores == 1U && state.parked == 1U) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    expect(transport.terminate(std::chrono::milliseconds(500)), "child cleanup must succeed");
  }  // the reader is joined here
  expect(state.parks == 2U && state.restores == 1U && state.parked == 1U && state.bytes == 2097152U && state.evictions == 0U &&
             state.last_event == std::optional<secure_frontend::ConversationCacheEvent>{secure_frontend::ConversationCacheEvent::restored} &&
             state.last_tokens == std::optional<std::uint64_t>{1200U} && state.last_at.has_value(),
         "two parks and a restore, then a drop; the near miss and the over-long line counted nothing");
  expect(!this_process_holds(needle), "no byte of the payload is left anywhere in this process");
  secure_frontend::EngineStdioProcess plain;
  expect(!plain.start({.executable = fake, .arguments = {"--fake-engine", "--stderr-cache-lines"}, .environment = {}}).has_value(),
         "fake engine must start");
  wait_ready(plain, std::chrono::seconds(10));
  expect(!plain.conversation_cache().has_value(), "without the flag stderr goes to NUL and nothing is read");
  (void)plain.terminate(std::chrono::milliseconds(500));
}

// The drain keeps up: 16 MiB of stderr (and one 1 MiB line) before READY never stall the engine, and the cache line
// after them is still read.
void stderr_flood_never_blocks_the_engine(const std::filesystem::path& fake) {
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine", "--stderr-flood=16384"}, .environment = {},
                           .read_conversation_cache = true})
              .has_value(),
         "fake engine must start");
  wait_ready(transport, std::chrono::seconds(60));
  std::optional<secure_frontend::ConversationCacheState> state;
  for (int i = 0; i < 250 && !(state && state->parked == 7U); ++i) {
    state = transport.conversation_cache();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  expect(state.has_value() && state->parked == 7U && state->parks == 1U, "the line after the flood is read");
  const auto started = std::chrono::steady_clock::now();
  expect(transport.terminate(std::chrono::milliseconds(500)), "child cleanup must succeed");
  expect(std::chrono::steady_clock::now() - started < std::chrono::seconds(5), "the stop does not wait on the pipe");
}

// Owner 2026-10-29 (logging.engine_output, to see why an engine fails on a new GPU): with a line sink every stderr line
// reaches it in order - printable ASCII, a line past kStderrLineBytes cut there and saying so - and the conversation
// cache is still read only where read_conversation_cache asks for it.
void stderr_lines_reach_the_sink_when_asked(const std::filesystem::path& fake) {
  expect(secure_frontend::printable_stderr_line("a\x1b[31mb\tc\x7f\xc3\xa9", false) == "a?[31mb c???",
         "control bytes, DEL and bytes past ASCII become ?, a tab a space");
  expect(secure_frontend::printable_stderr_line("abc", true) == "abc [cut at 4096 bytes]", "a cut line says so");
  const Needle needle;
  const std::string payload{needle.bytes.begin(), needle.bytes.end()};
  for (const bool cache : {false, true}) {
    std::mutex mutex;
    std::vector<std::string> lines;
    const auto count = [&] {
      std::lock_guard lock(mutex);
      return lines.size();
    };
    {
      secure_frontend::EngineStdioProcess transport;
      expect(!transport.start({.executable = fake, .arguments = {"--fake-engine", "--stderr-cache-lines"}, .environment = {},
                               .read_conversation_cache = cache,
                               .stderr_lines = [&](const std::string_view line) {
                                 std::lock_guard lock(mutex);
                                 lines.emplace_back(line);
                               }})
                  .has_value(),
             "fake engine must start");
      wait_ready(transport, std::chrono::seconds(10));
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (count() < 8U && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(20));
      const auto read = transport.conversation_cache();
      expect(cache ? read.has_value() && read->parks == 2U && read->restores == 1U : !read.has_value(),
             "the conversation cache is read only where asked");
      expect(transport.terminate(std::chrono::milliseconds(500)), "child cleanup must succeed");
    }  // the reader is joined here
    // the fake's start-up line, then --stderr-cache-lines' seven
    expect(lines.size() == 8U, "every line, once: " + std::to_string(lines.size()));
    if (lines.size() != 8U) continue;
    expect(lines[0] == "diagnostic: private engine startup", "the start-up line: " + lines[0]);
    expect(lines[1] == "strata serve: session 0: prompt: " + payload, "a line as the engine wrote it: " + lines[1]);
    expect(lines[3].rfind("strata serve: conversation cache: parked 1200 tokens", 0) == 0 &&
               lines[5].rfind("strata serve: conversation cache: restored 1200 tokens", 0) == 0,
           "in the order written");
    expect(lines[6] == std::string(secure_frontend::kStderrLineBytes, 'x') + " [cut at 4096 bytes]", "the over-long line, cut at the bound");
    expect(lines[7] == "strata serve: conversation cache: dropped 1 superseded copy of this conversation; parked=1",
           "the last line: " + lines[7]);
  }
}

// The running engine's memory (Windows working set, Linux VmRSS), and none once it ended.
void resident_bytes_read_the_running_engine(const std::filesystem::path& fake) {
  expect(secure_frontend::parse_proc_status_resident_bytes("Name:\tstrata\nVmPeak:\t  9000 kB\nVmRSS:\t   123456 kB\nThreads:\t4\n") ==
             std::optional<std::uint64_t>{123456ULL * 1024ULL},
         "VmRSS in bytes");
  for (const char* bad : {"", "VmRSS:\t abc kB\n", "VmRSS:\t 12 MB\n", "VmRSS:\t 12\n", "VmRSSx: 12 kB\n", "Name: x\n"}) {
    expect(!secure_frontend::parse_proc_status_resident_bytes(bad).has_value(), std::string{"refused: "} + bad);
  }
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.resident_bytes().has_value(), "no child: nothing");
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine"}, .environment = {}}).has_value(), "fake engine must start");
  wait_ready(transport, std::chrono::seconds(10));
  const auto running = transport.resident_bytes();
  expect(running.has_value() && *running > (256U << 10U), "the running child's memory");
  expect(transport.terminate(std::chrono::milliseconds(500)), "child cleanup must succeed");
  expect(!transport.resident_bytes().has_value(), "an ended child: nothing");
}

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
      {"stderr_gives_only_the_conversation_cache_numbers", [] { stderr_gives_only_the_conversation_cache_numbers(fake); }},
      {"stderr_flood_never_blocks_the_engine", [] { stderr_flood_never_blocks_the_engine(fake); }},
      {"stderr_lines_reach_the_sink_when_asked", [] { stderr_lines_reach_the_sink_when_asked(fake); }},
      {"resident_bytes_read_the_running_engine", [] { resident_bytes_read_the_running_engine(fake); }},
#if defined(_WIN32)
      {"job_object_terminates_grandchild", [] { job_object_terminates_grandchild(fake); }},
#else
      {"child_inherits_only_stdio_descriptors", [] { child_inherits_only_stdio_descriptors(fake); }},
      {"child_receives_exact_argv_and_env", [] { child_receives_exact_argv_and_env(fake); }},
      {"terminate_reaps_killed_child", [] { terminate_reaps_killed_child(fake); }},
#endif
  }, no_counted_failures);
}
