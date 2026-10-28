#include "secure_frontend/engine_process.hpp"
#include "secure_frontend/engine_stdio_process.hpp"

#include "support/fake_engine_dump.hpp"
#include "support/test_paths.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using secure_frontend::EngineReadKind;

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

// Exit code of a fake that is not given a stdio protocol (EngineProcess keeps the parent's stdio).
std::optional<int> exit_code_of(const std::vector<std::string>& arguments) {
  secure_frontend::EngineProcess process;
  if (process.start({.executable = test_support::fake_engine_path(), .arguments = arguments, .environment = {}}).has_value()) {
    return std::nullopt;
  }
  return process.wait_for(10s);
}

struct Child final {
  secure_frontend::EngineStdioProcess transport;
  explicit Child(const std::vector<std::string>& arguments, const std::filesystem::path& executable = test_support::fake_engine_path()) {
    expect(!transport.start({.executable = executable, .arguments = arguments, .environment = {}}).has_value(),
           "fake engine must start");
  }
  ~Child() { (void)transport.terminate(std::chrono::milliseconds(250)); }
  std::string line(const std::chrono::milliseconds timeout = 5s) {
    const auto result = transport.read_line(timeout);
    return result.kind == EngineReadKind::line ? result.line : std::string("<kind ") + std::to_string(static_cast<int>(result.kind)) + ">";
  }
  EngineReadKind kind(const std::chrono::milliseconds timeout) { return transport.read_line(timeout).kind; }
  void send(const std::string& text) {
    std::string error;
    expect(transport.write_line(text, secure_frontend::deadline_after(std::chrono::seconds(5)), error), "write must succeed: " + text);
  }
};

std::string unhex(const std::string& hex) {
  std::string out;
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    out.push_back(static_cast<char>(std::stoi(hex.substr(index, 2), nullptr, 16)));
  }
  return out;
}

void fake_engine_modes_selftest() {
  // Exit paths (raw exit codes as EngineProcess normalizes them: POSIX 128 + signal).
  expect(exit_code_of({"--exit-before-ready=3"}) == 3, "--exit-before-ready=3 exits 3");
  expect(exit_code_of({"--crash-after-ready=9"}) == 9, "--crash-after-ready=<code> exits with that code");
  const auto start = std::chrono::steady_clock::now();
  expect(exit_code_of({"--crash-after-ready=7", "--crash-delay-ms=200"}) == 7, "--crash-delay-ms delays the crash");
  expect(std::chrono::steady_clock::now() - start >= 200ms, "the crash waited --crash-delay-ms");
  const auto aborted = exit_code_of({"--crash-after-ready=ABRT"});
#if defined(_WIN32)
  expect(aborted && (*aborted == 3 || static_cast<unsigned>(*aborted) == 0xC0000409U), "ABRT is exit 3 or 0xC0000409 on Windows");
  const auto segv = exit_code_of({"--crash-after-ready=SEGV"});
  expect(segv && static_cast<unsigned>(*segv) == 0xC0000005U, "SEGV is an access violation on Windows");
#else
  expect(aborted == 128 + 6, "ABRT dies by SIGABRT");
  expect(exit_code_of({"--crash-after-ready=KILL"}) == 128 + 9, "KILL dies by SIGKILL");
  expect(exit_code_of({"--crash-after-ready=SEGV"}) == 128 + 11, "SEGV dies by SIGSEGV");
  expect(exit_code_of({"--crash-after-ready=TERM"}) == 128 + 15, "TERM dies by SIGTERM");
#endif

  {  // READY variants
    Child child({"--split-ready", "--ready-delay-ms=300"});
    expect(child.kind(100ms) == EngineReadKind::timeout, "--ready-delay-ms holds READY back");
    expect(child.line() == "READY 4096 stop", "split READY arrives whole");
  }
  {
    Child child({"--ready-context=65536"});
    expect(child.line() == "READY 65536 stop", "--ready-context sets the advertised context");
  }
  {
    Child child({"--hang-before-ready"});
    expect(child.kind(300ms) == EngineReadKind::timeout, "--hang-before-ready never prints READY");
  }
  {
    Child child({"--echo-cmdline"});
    const std::string cmdline = child.line();
    expect(cmdline.rfind("CMDLINE ", 0) == 0 && unhex(cmdline.substr(8)).find("--echo-cmdline") != std::string::npos,
           "--echo-cmdline reports the command line before READY: " + cmdline);
    expect(child.line() == "READY 4096 stop", "READY follows CMDLINE");
  }

  {  // Generation behaviors
    Child child({"--hang-in-gen"});
    expect(child.line() == "READY 4096 stop", "READY");
    child.send("GEN 1 1");
    expect(child.kind(300ms) == EngineReadKind::timeout, "--hang-in-gen prints nothing for GEN");
    child.send("STOP");
    expect(child.kind(300ms) == EngineReadKind::timeout, "--hang-in-gen never answers STOP");
  }
  {
    Child child({"--abort-in-gen"});
    expect(child.line() == "READY 4096 stop", "READY");
    child.send("GEN 1 1");
    expect(child.line() == "T 7", "--abort-in-gen prints one token");
    expect(child.kind(5s) == EngineReadKind::closed, "then the engine is gone");
  }
  const std::vector<std::string> prefixes{"ERR verify: timed out at layer ", "ERR verify batch: timed out at layer ",
                                          "ERR verify: an earlier window never finished",
                                          "ERR verify batch: an earlier window never finished"};
  for (std::size_t index = 0; index < prefixes.size(); ++index) {
    Child child({"--fatal-line=" + std::to_string(index)});
    expect(child.line() == "READY 4096 stop", "READY");
    child.send("GEN 1 1");
    expect(child.line() == "T 7", "fatal mode prints a token first");
    expect(child.line().rfind(prefixes[index], 0) == 0, "fatal mode prints prefix " + std::to_string(index));
    expect(child.line() == "DONE stop", "then a trailing DONE the frontend must not accept");
    child.send("GEN 1 1");
    expect(child.line() == "T 7", "the engine lives on after the fatal line");
  }
  {
    Child child({"--stall-stdin"});
    expect(child.line() == "READY 4096 stop", "READY");
    child.send("GEN 1 1");
    expect(child.kind(300ms) == EngineReadKind::timeout, "--stall-stdin never reads the GEN");
  }

  {  // Restart helpers and recording
    const auto dir = test_support::make_temp_dir("fake-modes");
    const std::string counter = "--counter-file=" + test_support::utf8_path(dir / "count");
    {
      Child first({"--fail-first=1", counter});
      expect(first.kind(5s) == EngineReadKind::closed, "the first start fails before READY");
    }
    {
      Child second({"--fail-first=1", counter});
      expect(second.line() == "READY 4096 stop", "the second start is healthy");
    }
    const auto record = dir / "record.txt";
    {
      Child child({"--record-file=" + test_support::utf8_path(record)});
      expect(child.line() == "READY 4096 stop", "READY");
      child.send("GEN 1 1");
      expect(child.line() == "T 7" && child.line() == "DONE stop", "a normal generation");
      child.send("QUIT");
      expect(child.kind(5s) == EngineReadKind::closed, "QUIT ends the engine");
    }
    std::ifstream recorded(record);
    const std::string content((std::istreambuf_iterator<char>(recorded)), std::istreambuf_iterator<char>());
    expect(content == "GEN 1 1\nQUIT\n", "--record-file holds every stdin line, saw: " + content);

    const auto pidfile = dir / "grandchild.pid";
    {
      Child child({"--spawn-grandchild", "--grandchild-pidfile=" + test_support::utf8_path(pidfile)});
      expect(child.line() == "READY 4096 stop", "READY");
      const auto grandchild = test_support::read_pidfile(pidfile, 5s);
      expect(grandchild.has_value() && test_support::process_alive(*grandchild), "the grandchild is running");
      if (grandchild) test_support::kill_process(*grandchild);
    }
  }
}

void fake_engine_dump_is_parseable() {
  const auto fake = test_support::fake_engine_path();
  const std::string odd = "a b\nc \xE2\x9C\x93";
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = fake, .arguments = {odd}, .environment = {{"STRATA_T", "1"}, {"ZZ", "z=z"}}}).has_value(),
         "fake engine must start");
  expect(transport.read_line(5s).line == "READY 4096 stop", "READY");
  const auto dump = test_support::request_dump(transport, 5s);
  expect(dump.has_value(), "DUMP must parse");
  if (dump) {
    expect(dump->argv == std::vector<std::string>{test_support::utf8_path(fake), "--serve", odd}, "argv round-trips through ARGV");
    expect(dump->env == std::vector<std::string>{"STRATA_T=1", "ZZ=z=z"}, "ENV is the sorted child environment");
    expect(!dump->cwd.empty(), "CWD is reported");
#if defined(_WIN32)
    expect(dump->windows_pid != 0, "HANDLES carries the child's own pid");
    expect(dump->ids.count("sid") == 1 && dump->ids.count("elevated") == 1 && dump->ids.count("integrity") == 1, "Windows IDS fields");
#else
    expect(dump->fds == std::vector<int>{0, 1, 2}, "FDS lists exactly stdio");
    expect(dump->sig_blocked.size() == 16U && dump->sig_ignored.size() == 16U, "SIG carries SigBlk/SigIgn masks");
    for (const char* key : {"uid", "gid", "euid", "egid", "nnp", "capeff"}) expect(dump->ids.count(key) == 1, std::string("IDS has ") + key);
#endif
  }
  (void)transport.terminate(std::chrono::milliseconds(250));

  const auto dir = test_support::make_temp_dir("dump");
  const auto file = dir / "dump.txt";
  secure_frontend::EngineStdioProcess second;
  expect(!second.start({.executable = fake, .arguments = {"--dump-file=" + test_support::utf8_path(file)}, .environment = {}}).has_value(),
         "fake engine must start");
  expect(second.read_line(5s).line == "READY 4096 stop", "READY after --dump-file");
  const auto from_file = test_support::read_dump_file(file, 5s);
  expect(from_file.has_value() && from_file->argv.size() == 3U, "--dump-file writes the same parseable report");
  (void)second.terminate(std::chrono::milliseconds(250));
}

// SP3 Task 14 (spec §9.2, M1 modes): streaming fake — cycled token ids,
// counts, intervals, finishes, STOP with a delay, prefill delay; the SP2
// short form stays unchanged when no new flag is given.
void fake_engine_stream_modes_selftest() {
  {
    Child child({"--token-ids=5,6", "--tokens=5", "--token-interval-ms=10"});
    expect(child.line() == "READY 4096 stop", "ready");
    child.send("GEN 100 1,2,3");
    std::vector<std::string> lines;
    for (int i = 0; i < 6; ++i) lines.push_back(child.line());
    expect(lines == std::vector<std::string>{"T 5", "T 6", "T 5", "T 6", "T 5", "DONE 5 3 0 0 stop"}, "ids cycle until --tokens, full DONE");
  }
  {
    Child child({"--tokens=50", "--token-interval-ms=5"});
    (void)child.line();
    child.send("GEN 3 1");
    expect(child.line() == "T 7" && child.line() == "T 7" && child.line() == "T 7" && child.line() == "DONE 3 1 0 0 length",
           "max_new reached first ends with length");
  }
  {
    Child child({"--tokens=1", "--finish=nonfinite"});
    (void)child.line();
    child.send("GEN 10 1,2");
    expect(child.line() == "T 7" && child.line() == "DONE 1 2 0 0 nonfinite", "--finish sets the finish field");
  }
  {
    Child child({"--tokens=10000", "--token-interval-ms=5", "--stop-delay-ms=100"});
    (void)child.line();
    child.send("GEN 100000 1");
    expect(child.line() == "T 7", "generation running");
    const auto sent = std::chrono::steady_clock::now();
    child.send("STOP");
    std::string line;
    do { line = child.line(); } while (line == "T 7");
    expect(line.rfind("DONE ", 0) == 0 && line.size() > 7 && line.substr(line.size() - 6) == "cancel", "STOP ends with DONE ... cancel: " + line);
    expect(std::chrono::steady_clock::now() - sent >= 100ms, "--stop-delay-ms delays the cancel");
  }
  {
    Child child({"--prefill-delay-ms=300", "--tokens=1"});
    (void)child.line();
    child.send("GEN 5 1");
    expect(child.kind(200ms) == secure_frontend::EngineReadKind::timeout, "--prefill-delay-ms holds the first T back");
    expect(child.line() == "T 7", "then the token");
  }
  {
    Child child({});
    (void)child.line();
    child.send("GEN 5 1");
    expect(child.line() == "T 7" && child.line() == "DONE stop", "without new modes the SP2 short form is unchanged");
  }
  {  // M2: STOP during prefill is acted on at once
    Child child({"--prefill-delay-ms=3000", "--tokens=5"});
    (void)child.line();
    child.send("GEN 5 1");
    std::this_thread::sleep_for(50ms);
    const auto sent = std::chrono::steady_clock::now();
    child.send("STOP");
    expect(child.line() == "DONE 0 1 0 0 cancel", "STOP during prefill: no token, cancel");
    expect(std::chrono::steady_clock::now() - sent < 1s, "not after the 3 s prefill");
  }
  {  // M2: STOP during a long per-token pause is acted on at once
    Child child({"--tokens=10", "--token-interval-ms=3000"});
    (void)child.line();
    child.send("GEN 10 1");
    expect(child.line() == "T 7", "first token");
    const auto sent = std::chrono::steady_clock::now();
    child.send("STOP");
    expect(child.line() == "DONE 1 1 0 0 cancel", "the emitted token counts");
    expect(std::chrono::steady_clock::now() - sent < 1s, "not after the 3 s pause");
  }
  {  // M2: --burst-tokens skips the pause for the first n tokens
    Child child({"--tokens=6", "--burst-tokens=4", "--token-interval-ms=400"});
    (void)child.line();
    child.send("GEN 6 1");
    const auto start = std::chrono::steady_clock::now();
    for (int index = 0; index < 4; ++index) expect(child.line() == "T 7", "burst token");
    expect(std::chrono::steady_clock::now() - start < 300ms, "four tokens without a pause");
    expect(child.line() == "T 7" && std::chrono::steady_clock::now() - start >= 350ms, "then the pause applies");
  }
  {  // M2: --progress-records during prefill
    Child child({"--prefill-delay-ms=300", "--progress-records", "--tokens=1"});
    (void)child.line();
    child.send("GEN 1 1");
    expect(child.line() == "PP 1 3" && child.line() == "PP 2 3" && child.line() == "PP 3 3" && child.line() == "T 7", "PP then the token");
  }
  {  // M2: --err-mid-gen
    Child child({"--tokens=5", "--err-mid-gen=2"});
    (void)child.line();
    child.send("GEN 5 1");
    const std::string a = child.line(), b = child.line(), err = child.line();
    expect(a == "T 7" && b == "T 7" && err == "ERR test", "ERR after two tokens: " + err);
  }
  {  // M2: --crash-mid-gen
    Child child({"--tokens=5", "--crash-mid-gen=1"});
    (void)child.line();
    child.send("GEN 5 1");
    expect(child.line() == "T 7", "one token before the crash");
    expect(child.kind(5s) == secure_frontend::EngineReadKind::closed, "--crash-mid-gen exits");
  }
}

// Gemma 4 plan 1 Task 11: options in fake-engine.options beside the executable join the
// command line's (ServerOptions::fake_engine_options writes the file for the gemma4 profile,
// whose engine-argument allowlist refuses the fake's own flags). A copy of the fake in a scratch
// directory reads the file there; CRLF and blank lines are tolerated.
void fake_engine_reads_options_beside_itself() {
  const auto directory = secure_frontend_test::make_scratch_directory();
  const auto copy = directory / test_support::fake_engine_path().filename();
  std::error_code error;
  std::filesystem::copy_file(test_support::fake_engine_path(), copy, error);
  expect(!error, "the fake is copied into a scratch directory: " + error.message());
  if (error) return;
  const auto record = directory / "record.txt";
  secure_frontend_test::write_file(directory / test_support::kFakeEngineOptionsFile,
                                   "--token-ids=5,6\r\n\n--record-file=" + test_support::utf8_path(record) + "\n", 0600);
  {
    Child child({"--ready-context=512"}, copy);
    expect(child.line() == "READY 512 stop", "the command line's own options still apply");
    child.send("GEN 10 1,2,3");
    const std::string first = child.line(), second = child.line(), done = child.line();
    expect(first == "T 5" && second == "T 6" && done == "DONE 2 3 0 0 stop",
           "the file's --token-ids apply (stream mode, full DONE): " + first + " | " + second + " | " + done);
  }
  expect(secure_frontend_test::read_file(record).find("GEN 10 1,2,3\n") != std::string::npos,
         "the file's --record-file applies (its \\r dropped): the GEN line is recorded");
  {
    Child plain({});
    (void)plain.line();
    plain.send("GEN 5 1");
    expect(plain.line() == "T 7" && plain.line() == "DONE stop", "the build-tree fake has no options file: SP2 short form");
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  return secure_frontend_test::run_cases(argc, argv,
                                         {{"fake_engine_modes_selftest", fake_engine_modes_selftest},
                                          {"fake_engine_dump_is_parseable", fake_engine_dump_is_parseable},
                                          {"fake_engine_stream_modes_selftest", fake_engine_stream_modes_selftest},
                                          {"fake_engine_reads_options_beside_itself", fake_engine_reads_options_beside_itself}},
                                         failures);
}
