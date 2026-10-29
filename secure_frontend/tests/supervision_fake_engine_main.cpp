// strata-supervision-fake-engine: a test-only stand-in for `strata --serve` (SP2 F1).
// Never installed. (SP1's tests/fake_engine_main.cpp is a different, simpler fake.) It speaks the solo serve protocol the frontend uses
// (READY, GEN -> T/DONE, STOP, QUIT) and adds the failure modes the
// supervision tests need; see the mode list in the SP2 plan header. SP5 M2
// Task 6 adds --dialect=maya: the same payloads with Project Maya v1.1.21's
// engine cadence (NOTE after GEN, 5-field PP, STAT every few tokens).
// Unknown arguments (--serve, --native PATH, --pack DIR, --gpu LIST, ...) are
// accepted and ignored, as the real engine accepts them. Gemma 4 plan 1 Task 11:
// more options may come from fake-engine.options beside the executable
// (option_arguments below).
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <sddl.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

extern char** environ;
#endif

namespace {

// The engine's fatal verification lines (upstream/serve/server.py FATAL_PREFIXES).
constexpr std::string_view kFatalPrefixes[] = {
    "ERR verify: timed out at layer ",
    "ERR verify batch: timed out at layer ",
    "ERR verify: an earlier window never finished",
    "ERR verify batch: an earlier window never finished",
};

struct Options final {
  bool ignore_term{false};
  bool spew{false};
  bool split_ready{false};
  int ready_delay_ms{0};
  unsigned long ready_context{4096};
  std::optional<int> exit_before_ready;
  bool hang_before_ready{false};
  std::optional<std::string> crash_after_ready;
  int crash_delay_ms{0};
  bool hang_in_gen{false};
  bool abort_in_gen{false};
  std::optional<int> fatal_line;
  bool stall_stdin{false};
  int slow_stdin_bytes{0};  // > 0: after READY, only drain stdin, this many bytes per interval
  int slow_stdin_interval_ms{100};
  // SP3 Task 14 (spec §9.2, M1 modes): streaming generations. Any of these set
  // stream_modes, which switches GEN handling to run_generation and makes DONE
  // take the full six-field form.
  std::vector<unsigned long> token_ids{7};       // cycled across the generation
  // web search: the replies of the generations after the first, in turn (generation k answers script k mod n,
  // token_ids being script 0), so one engine can write a call and then the answer
  std::vector<std::vector<unsigned long>> next_token_ids;
  std::optional<unsigned long> tokens;           // T records; default: the id-list length
  int token_interval_ms{0};                      // pause between T records
  std::string finish{"stop"};                    // DONE finish field
  int prefill_delay_ms{0};                       // before the first T
  int stop_delay_ms{0};                          // between seeing STOP and the cancel DONE
  // SP3 M2 Task 2 (spec §9.2, R-F): M2 modes on M1's reader.
  bool progress_records{false};                  // PP <step> <steps> lines during prefill
  std::optional<unsigned long> crash_mid_gen;    // exit 1 after this many T records
  std::optional<unsigned long> err_mid_gen;      // "ERR test" after this many T records, then idle
  unsigned long burst_tokens{0};                 // first n tokens without the token-interval pause
  bool stream_modes{false};
  // GUI-first start-up (calibration): the DONE record's decode_ms (strata dialect), divided by FACTOR for each
  // --faster-with=FLAG:VALUE:FACTOR whose FLAG VALUE pair is on the command line, so a sweep has a win to find.
  double decode_ms{0.0};
  std::vector<std::tuple<std::string, std::string, double>> faster_with;
  std::vector<std::string> command_line;
  // SP5 M2 Task 6: --dialect=maya makes the fake speak Project Maya v1.1.21's engine
  // framing on top of the same T/DONE payloads — NOTE right after GEN (generate.cpp:1347),
  // the 5-field PP per prefill chunk (:6102), and a STAT cadence every few tokens
  // (:2014). maya_118 consumes them (Task 5); the other flavors must fail closed.
  std::string dialect;
  // Issue 27: with --dialect=maya, --maya-vram adds Maya v1.1.21's pre-READY startup records.
  bool maya_vram{false};
  // The main engine prints its expert split in the startup INFO; "<primary>,<total>" makes the
  // fake emit `INFO ... expert_slots_primary=<primary> expert_slots=<total>` before READY.
  std::string ready_experts;
  int fail_first{0};
  std::string counter_file;
  std::string record_file;
  bool spawn_grandchild{false};
  bool grandchild_setsid{false};
  bool grandchild_ignore_term{false};
  std::string grandchild_pidfile;
  bool echo_cmdline{false};
  std::string dump_file;
  // MiniMax's prefill calibration (maya-mm3 --calibrate-prefill OUT): each such run appends its command line here;
  // --prefill-fail makes it exit 1 without writing OUT
  std::string prefill_log;
  bool prefill_fail{false};
  std::optional<int> write_inherited;
};

#if !defined(_WIN32)
// POSIX only: Windows reports GetCommandLineW() instead (clang's
// -Wunused-but-set-global rejects them there).
int g_argc = 0;
char** g_argv = nullptr;
#endif

bool value_of(const std::string_view argument, const std::string_view name, std::string& value) {
  if (argument.size() <= name.size() || argument.substr(0, name.size()) != name || argument[name.size()] != '=') return false;
  value = std::string(argument.substr(name.size() + 1));
  return true;
}

// Gemma 4 plan 1 Task 11: the directory holding this executable (empty when unknown).
std::filesystem::path own_directory([[maybe_unused]] char** argv) {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return {};
  return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
#else
  std::error_code error;
  const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
  if (!error) return self.parent_path();
  return argv[0] != nullptr ? std::filesystem::path(argv[0]).parent_path() : std::filesystem::path{};
#endif
}

// The command line's options, then (Gemma 4 plan 1 Task 11) one more per line of
// fake-engine.options beside this executable, when present: a trailing \r is dropped and
// blank lines are skipped. The test harness writes it when the frontend's engine-argument
// allowlist (engine.profile gemma4: strata_gemma4) would refuse these flags as engine.args.
std::vector<std::string> option_arguments(const int argc, char** argv) {
  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
  const auto directory = own_directory(argv);
  if (directory.empty()) return arguments;
  std::ifstream file(directory / "fake-engine.options", std::ios::binary);
  for (std::string line; std::getline(file, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) arguments.push_back(std::move(line));
  }
  return arguments;
}

Options parse_options(const std::vector<std::string>& arguments) {
  Options options;
  std::string value;
  options.command_line = arguments;
  for (const std::string& text : arguments) {
    const std::string_view argument{text};
    if (argument == "--ignore-term") options.ignore_term = true;
    else if (argument == "--spew") options.spew = true;
    else if (argument == "--split-ready") options.split_ready = true;
    else if (argument == "--maya-vram") options.maya_vram = true;
    else if (value_of(argument, "--ready-experts", value)) options.ready_experts = value;
    else if (argument == "--hang-before-ready") options.hang_before_ready = true;
    else if (argument == "--hang-in-gen") options.hang_in_gen = true;
    else if (argument == "--abort-in-gen") options.abort_in_gen = true;
    else if (argument == "--stall-stdin") options.stall_stdin = true;
    else if (argument == "--spawn-grandchild") options.spawn_grandchild = true;
    else if (argument == "--grandchild-setsid") options.grandchild_setsid = true;
    else if (argument == "--grandchild-ignore-term") options.grandchild_ignore_term = true;
    else if (argument == "--echo-cmdline") options.echo_cmdline = true;
    else if (value_of(argument, "--ready-delay-ms", value)) options.ready_delay_ms = std::stoi(value);
    else if (value_of(argument, "--ready-context", value)) options.ready_context = std::stoul(value);
    else if (value_of(argument, "--exit-before-ready", value)) options.exit_before_ready = std::stoi(value);
    else if (value_of(argument, "--crash-after-ready", value)) options.crash_after_ready = value;
    else if (value_of(argument, "--crash-delay-ms", value)) options.crash_delay_ms = std::stoi(value);
    else if (value_of(argument, "--fatal-line", value)) options.fatal_line = std::clamp(std::stoi(value), 0, 3);
    else if (value_of(argument, "--fail-first", value)) options.fail_first = std::stoi(value);
    else if (value_of(argument, "--counter-file", value)) options.counter_file = value;
    else if (value_of(argument, "--record-file", value)) options.record_file = value;
    else if (value_of(argument, "--grandchild-pidfile", value)) options.grandchild_pidfile = value;
    else if (value_of(argument, "--dump-file", value)) options.dump_file = value;
    else if (value_of(argument, "--prefill-log", value)) options.prefill_log = value;
    else if (argument == "--prefill-fail") options.prefill_fail = true;
    else if (value_of(argument, "--write-inherited", value)) options.write_inherited = std::stoi(value);
    else if (value_of(argument, "--slow-stdin", value)) options.slow_stdin_bytes = std::max(1, std::stoi(value));
    else if (value_of(argument, "--slow-stdin-interval-ms", value)) options.slow_stdin_interval_ms = std::max(1, std::stoi(value));
    else if (value_of(argument, "--token-ids", value)) {
      options.token_ids.clear();
      std::stringstream ids(value);
      for (std::string id; std::getline(ids, id, ',');) options.token_ids.push_back(std::stoul(id));
      options.stream_modes = true;
    }
    else if (value_of(argument, "--token-ids-next", value)) {
      std::vector<unsigned long> script;
      std::stringstream ids(value);
      for (std::string id; std::getline(ids, id, ',');) script.push_back(std::stoul(id));
      options.next_token_ids.push_back(std::move(script));
      options.stream_modes = true;
    }
    else if (value_of(argument, "--tokens", value)) { options.tokens = std::stoul(value); options.stream_modes = true; }
    else if (value_of(argument, "--token-interval-ms", value)) { options.token_interval_ms = std::stoi(value); options.stream_modes = true; }
    else if (value_of(argument, "--finish", value)) { options.finish = value; options.stream_modes = true; }
    else if (value_of(argument, "--prefill-delay-ms", value)) { options.prefill_delay_ms = std::stoi(value); options.stream_modes = true; }
    else if (value_of(argument, "--stop-delay-ms", value)) { options.stop_delay_ms = std::stoi(value); options.stream_modes = true; }
    else if (argument == "--progress-records") { options.progress_records = true; options.stream_modes = true; }
    else if (value_of(argument, "--crash-mid-gen", value)) { options.crash_mid_gen = std::stoul(value); options.stream_modes = true; }
    else if (value_of(argument, "--err-mid-gen", value)) { options.err_mid_gen = std::stoul(value); options.stream_modes = true; }
    else if (value_of(argument, "--burst-tokens", value)) { options.burst_tokens = std::stoul(value); options.stream_modes = true; }
    else if (value_of(argument, "--decode-ms", value)) options.decode_ms = std::stod(value);
    else if (value_of(argument, "--faster-with", value)) {
      const auto first = value.find(':');
      const auto last = value.rfind(':');
      if (first != std::string::npos && last != first) {
        options.faster_with.emplace_back(value.substr(0, first), value.substr(first + 1, last - first - 1), std::stod(value.substr(last + 1)));
      }
    }
    else if (value_of(argument, "--dialect", value)) {
      options.dialect = value;
      if (value == "maya") {  // a Maya engine always streams and prints its prefill progress
        options.stream_modes = true;
        options.progress_records = true;
      }
    }
  }
  return options;
}

void sleep_ms(const int milliseconds) { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }

[[noreturn]] void sleep_forever() {
  for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

// --slow-stdin=BYTES: a slow but steady stdin reader (SP2 R1). It reads at
// most BYTES raw bytes every --slow-stdin-interval-ms (default 100) and
// discards them, never answering, until EOF. Each 64 KiB write chunk drains
// well inside 1 s while a large GEN line takes far longer overall.
void drain_stdin_slowly(const int bytes, const int interval_ms) {
  std::vector<char> buffer(static_cast<std::size_t>(bytes));
  for (;;) {
#if defined(_WIN32)
    DWORD count = 0;
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) || count == 0) return;
#else
    const ssize_t count = read(STDIN_FILENO, buffer.data(), buffer.size());
    if (count <= 0) return;
#endif
    sleep_ms(interval_ms);
  }
}

std::string hex(const std::string_view bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2U);
  for (const unsigned char byte : bytes) {
    out.push_back(kDigits[byte >> 4U]);
    out.push_back(kDigits[byte & 0x0fU]);
  }
  return out;
}

#if defined(_WIN32)
std::string utf8(const std::wstring_view text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(std::max(size, 0)), '\0');
  if (size > 0) (void)WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
  return out;
}
#endif

// Written to <path>.tmp and renamed, so a reader never sees a partial number.
void write_pidfile(const std::string& path, const long pid) {
  const std::string temporary = path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    out << pid << '\n';
  }
  std::error_code ignored;
  std::filesystem::rename(temporary, path, ignored);
}

[[noreturn]] void crash(const std::string& how) {
  std::cout.flush();
#if !defined(_WIN32)
  const rlimit no_core{0, 0};
  (void)setrlimit(RLIMIT_CORE, &no_core);  // a test crash must not leave core files in the build tree
#endif
  if (how == "ABRT") std::abort();
#if defined(_WIN32)
  if (how == "SEGV") RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
  if (how == "KILL") (void)TerminateProcess(GetCurrentProcess(), 137);
  if (how == "TERM") (void)TerminateProcess(GetCurrentProcess(), 143);
#else
  if (how == "SEGV" || how == "KILL" || how == "TERM") {
    const int signal_number = how == "SEGV" ? SIGSEGV : how == "KILL" ? SIGKILL : SIGTERM;
    (void)std::signal(signal_number, SIG_DFL);
    (void)std::raise(signal_number);
  }
#endif
  std::_Exit(std::stoi(how));
}

#if defined(_WIN32)
long spawn_grandchild(const Options&) {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return 0;
  const std::wstring self(buffer.data(), length);
  std::wstring line = L"\"" + self + L"\" --fake-sleeper";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  if (!CreateProcessW(self.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) return 0;
  CloseHandle(info.hThread);
  CloseHandle(info.hProcess);
  return static_cast<long>(info.dwProcessId);
}
#else
long spawn_grandchild(const Options& options) {
  const pid_t child = fork();
  if (child != 0) return child < 0 ? 0 : static_cast<long>(child);
  if (options.grandchild_setsid) (void)setsid();
  if (options.grandchild_ignore_term) (void)std::signal(SIGTERM, SIG_IGN);
  const int null_fd = open("/dev/null", O_RDWR);  // never hold the protocol pipes open
  if (null_fd >= 0) {
    (void)dup2(null_fd, STDIN_FILENO);
    (void)dup2(null_fd, STDOUT_FILENO);
    (void)close(null_fd);
  }
  // POSIX: the grandchild writes its own pidfile after setsid/SIG_IGN, so a
  // test that reads the pid never races the grandchild's setup.
  if (!options.grandchild_pidfile.empty()) write_pidfile(options.grandchild_pidfile, static_cast<long>(getpid()));
  sleep_ms(60000);
  std::_Exit(0);
}
#endif

#if !defined(_WIN32)
// This process's open descriptors, dropping the directory iterator's own one
// (its link target is /proc/<pid>/fd).
std::string open_descriptors() {
  const std::string iterator_target = "/proc/" + std::to_string(getpid()) + "/fd";
  std::string fds;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
    std::error_code ignored;
    const auto target = std::filesystem::read_symlink(entry.path(), ignored).string();
    if (target == iterator_target) continue;
    if (!fds.empty()) fds += ',';
    fds += entry.path().filename().string();
  }
  return fds;
}
#endif

std::string join_hex(const std::vector<std::string>& items) {
  std::string out;
  for (const auto& item : items) {
    if (!out.empty()) out += ',';
    out += hex(item);
  }
  return out;
}

#if !defined(_WIN32)
std::string status_field(const std::string_view name) {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.size() > name.size() && line.compare(0, name.size(), name) == 0 && line[name.size()] == ':') {
      const auto first = line.find_first_not_of(" \t", name.size() + 1U);
      return first == std::string::npos ? std::string{} : line.substr(first);
    }
  }
  return {};
}
#else
std::string token_report() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return "sid=? elevated=? integrity=?";
  std::string sid_text = "?";
  DWORD size = 0;
  (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> user(size);
  if (size != 0 && GetTokenInformation(token, TokenUser, user.data(), size, &size)) {
    LPWSTR sid = nullptr;
    if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) {
      sid_text = utf8(sid);
      LocalFree(sid);
    }
  }
  TOKEN_ELEVATION elevation{};
  DWORD elevation_size = sizeof(elevation);
  const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, elevation_size, &elevation_size) && elevation.TokenIsElevated != 0;
  DWORD rid = 0;
  size = 0;
  (void)GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
  std::vector<unsigned char> label(size);
  if (size != 0 && GetTokenInformation(token, TokenIntegrityLevel, label.data(), size, &size)) {
    PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
    rid = *GetSidSubAuthority(sid, static_cast<DWORD>(*GetSidSubAuthorityCount(sid) - 1U));
  }
  CloseHandle(token);
  return "sid=" + sid_text + " elevated=" + (elevated ? "1" : "0") + " integrity=" + std::to_string(rid);
}
#endif

// The DUMP report (SP2 plan, Task 2). Every list is hex so one record stays one line.
std::vector<std::string> dump_lines() {
  std::vector<std::string> lines;
  std::vector<std::string> arguments;
  std::vector<std::string> environment;
  std::string cwd;
#if defined(_WIN32)
  lines.push_back("HANDLES " + std::to_string(GetCurrentProcessId()));
  int count = 0;
  LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
  for (int index = 0; wide != nullptr && index < count; ++index) arguments.push_back(utf8(wide[index]));
  if (wide != nullptr) LocalFree(wide);
  LPWCH block = GetEnvironmentStringsW();
  for (const wchar_t* entry = block; entry != nullptr && *entry != L'\0'; entry += std::wcslen(entry) + 1U) environment.push_back(utf8(entry));
  if (block != nullptr) FreeEnvironmentStringsW(block);
  std::vector<wchar_t> directory(32768);
  const DWORD length = GetCurrentDirectoryW(static_cast<DWORD>(directory.size()), directory.data());
  cwd = utf8(std::wstring_view(directory.data(), length));
  lines.push_back("SIG none");
#else
  lines.push_back("FDS " + open_descriptors());
  for (int index = 0; index < g_argc; ++index) arguments.emplace_back(g_argv[index]);
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) environment.emplace_back(*entry);
  std::error_code ignored;
  cwd = std::filesystem::current_path(ignored).string();
  lines.push_back("SIG blk=" + status_field("SigBlk") + " ign=" + status_field("SigIgn"));
#endif
  std::sort(environment.begin(), environment.end());
  lines.push_back("ARGV " + join_hex(arguments));
  lines.push_back("ENV " + join_hex(environment));
  lines.push_back("CWD " + hex(cwd));
#if defined(_WIN32)
  lines.push_back("IDS " + token_report());
#else
  lines.push_back("IDS uid=" + std::to_string(getuid()) + " gid=" + std::to_string(getgid()) + " euid=" + std::to_string(geteuid()) +
                  " egid=" + std::to_string(getegid()) + " nnp=" + status_field("NoNewPrivs") + " capeff=" + status_field("CapEff"));
#endif
  lines.emplace_back("END");
  return lines;
}

void dump() {
  for (const auto& line : dump_lines()) std::cout << line << '\n';
  std::cout << std::flush;
}

// Written to <path>.tmp and renamed, so a reader never sees a partial report.
// The report is built before the file is opened, so FDS never lists it.
void write_dump_file(const std::string& path) {
  const auto lines = dump_lines();
  const std::string temporary = path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc | std::ios::binary);
    for (const auto& line : lines) out << line << '\n';
  }
  std::error_code ignored;
  std::filesystem::rename(temporary, path, ignored);
}

// --write-inherited=N (SP2 R4, the SP4 launch seam): writes "INHERITED <N>\n"
// to descriptor N (POSIX) or, on Windows, to the handle at entry N of the
// Microsoft CRT descriptor block in STARTUPINFO.lpReserved2 (int n, n flag
// bytes, n HANDLEs, packed). The block is parsed here, not through the fake's
// own CRT, so the test sees exactly what the launcher passed. False: absent.
bool write_inherited(const int index) {
  const std::string text = "INHERITED " + std::to_string(index) + "\n";
#if defined(_WIN32)
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  GetStartupInfoW(&startup);
  const unsigned char* block = startup.lpReserved2;
  const std::size_t size = startup.cbReserved2;
  int count = 0;
  if (block == nullptr || size < sizeof(count)) return false;
  std::memcpy(&count, block, sizeof(count));
  if (index < 0 || count <= index || size < sizeof(count) + static_cast<std::size_t>(count) * (1U + sizeof(HANDLE))) return false;
  HANDLE handle = nullptr;
  std::memcpy(&handle, block + sizeof(count) + static_cast<std::size_t>(count) + static_cast<std::size_t>(index) * sizeof(HANDLE), sizeof(HANDLE));
  DWORD written = 0;
  return WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE && written == text.size();
#else
  return write(index, text.data(), text.size()) == static_cast<ssize_t>(text.size());
#endif
}

void echo_cmdline() {
#if defined(_WIN32)
  std::cout << "CMDLINE " << hex(utf8(GetCommandLineW())) << '\n' << std::flush;
#else
  std::string joined;
  for (int index = 0; index < g_argc; ++index) {
    if (index != 0) joined += ' ';
    joined += g_argv[index];
  }
  std::cout << "CMDLINE " << hex(joined) << '\n' << std::flush;
#endif
}

// Stdin is read on its own thread (spec §9.2) so STOP is seen while T records
// are written. Each line is recorded the moment it arrives.
class StdinReader final {
 public:
  explicit StdinReader(std::string record_file) : record_file_(std::move(record_file)) {
    thread_ = std::thread([this] {
      std::string line;
      while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!record_file_.empty()) {
          // Binary: the tests match "\nSTOP\n"; a text stream writes \r\n on Windows.
          std::ofstream out(record_file_, std::ios::app | std::ios::binary);
          out << line << '\n';
        }
        std::lock_guard lock(mutex_);
        lines_.push_back(line);
        ready_.notify_all();
      }
      std::lock_guard lock(mutex_);
      eof_ = true;
      ready_.notify_all();
    });
    thread_.detach();   // the process exits through return/_Exit; std::getline may block forever
  }
  // Next line, or nullopt at EOF.
  std::optional<std::string> next() {
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [this] { return !lines_.empty() || eof_; });
    if (lines_.empty()) return std::nullopt;
    std::string line = std::move(lines_.front());
    lines_.pop_front();
    return line;
  }
  // True when a STOP line is waiting (and consumes it).
  bool take_stop() {
    std::lock_guard lock(mutex_);
    const auto it = std::find(lines_.begin(), lines_.end(), "STOP");
    if (it == lines_.end()) return false;
    lines_.erase(it);
    return true;
  }

 private:
  std::string record_file_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::string> lines_;
  bool eof_{false};
};

// Sleeps up to `milliseconds` in 1 ms steps; true (and STOP consumed) when STOP arrived meanwhile.
bool sleep_or_stop(StdinReader& reader, const int milliseconds) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  do {
    if (reader.take_stop()) return true;
    sleep_ms(1);
  } while (std::chrono::steady_clock::now() < until);
  return reader.take_stop();
}

// GENI <max_new> [k=v ...] <embeddings-file> <ids>, read as upstream's engine reads it (engine/src/program/generate.cpp:
// the file is the first token without '=', a .sve; the ids follow). True when the line is not one: the engine answers
// its ERR (the owner's first real picture, 2026-10-28, sent the older pin's order, ids then the file).
bool geni_out_of_order(const std::string& line) {
  if (line.rfind("GENI ", 0) != 0) return false;
  std::istringstream fields(line.substr(5));
  unsigned long max_new = 0;
  fields >> max_new;
  std::string token;
  while (fields >> token) {
    if (token.find('=') != std::string::npos) continue;
    return token.size() < 5U || token.compare(token.size() - 4U, 4U, ".sve") != 0;
  }
  return true;
}

void run_generation(const Options& options, const std::string& line, StdinReader& reader) {
  // GEN <max_new> [k=v ...] <ids>   and the SP5 M1 Task 12 variant
  // GENI <max_new> [k=v ...] <embeddings-file> <ids>, read as upstream's engine reads it (generate.cpp: the file is the
  // first token without '=', the ids follow): a GENI whose ids come before the file is refused with the engine's ERR.
  const bool geni = line.rfind("GENI ", 0) == 0;
  std::istringstream fields(line.substr(geni ? 5 : 4));
  unsigned long max_new = 0;
  fields >> max_new;
  std::string token;
  std::size_t prompt = 0;
  bool file_seen = false;
  while (fields >> token) {
    if (token.find('=') != std::string::npos) continue;
    if (geni && !file_seen) {   // the embeddings file (the dispatch refused a GENI without one first)
      file_seen = true;
      continue;
    }
    prompt = static_cast<std::size_t>(std::count(token.begin(), token.end(), ',')) + 1U;
  }
  static std::size_t generation_count = 0;  // this engine's generations so far (web search: the scripts in turn)
  const std::size_t script = generation_count++ % (options.next_token_ids.size() + 1U);
  const std::vector<unsigned long>& token_ids = script == 0U ? options.token_ids : options.next_token_ids[script - 1U];
  const unsigned long total = options.tokens.value_or(token_ids.size());
  bool stopped = false;
  // The maya dialect's conversation note right after GEN (generate.cpp:1347 prints
  // "NOTE sessions: ..." in serve mode); NOTE/PP/STAT are cadence records only —
  // the token payloads stay the strata T/DONE records.
  const bool maya = options.dialect == "maya";
  if (maya) std::cout << "NOTE sessions: 1\n" << std::flush;
  if (options.prefill_delay_ms > 0) {
    const int steps = options.progress_records ? std::max(1, options.prefill_delay_ms / 100) : 1;
    for (int step = 1; step <= steps && !stopped; ++step) {
      stopped = sleep_or_stop(reader, options.prefill_delay_ms / steps);
      if (options.progress_records && !stopped)
        std::cout << "PP " << step << ' ' << steps << (maya ? " 120 33.3 0" : "") << '\n' << std::flush;
    }
  }
  unsigned long generated = 0;
  std::string finish = options.finish;
  for (; !stopped && generated < total; ++generated) {
    if (reader.take_stop()) { stopped = true; break; }
    if (generated >= max_new) { finish = "length"; break; }
    if (options.crash_mid_gen && generated == *options.crash_mid_gen) { std::cout << std::flush; std::_Exit(1); }
    if (options.err_mid_gen && generated == *options.err_mid_gen) { std::cout << "ERR test\n" << std::flush; return; }
    std::cout << "T " << token_ids[generated % token_ids.size()] << '\n' << std::flush;
    if (maya && generated % 3U == 2U)  // the expert-tier cadence, every few tokens (generate.cpp:2014)
      std::cout << "STAT tok_s=40.00 ms_tok=25.00 vram_hit=0.7500 ram_fetch=0.2500 disk=0.000 promo=0.010 "
                   "vram_used=8192 vram_slots=256 ram_used=4096 ram_slots=1024 ram_gb=8.0 vram_gb=12.0\n"
                << std::flush;
    if (options.token_interval_ms > 0 && generated + 1 >= options.burst_tokens && sleep_or_stop(reader, options.token_interval_ms)) {
      ++generated;  // the token just written counts
      stopped = true;
      break;
    }
  }
  if (stopped) {
    finish = "cancel";
    if (options.stop_delay_ms > 0) sleep_ms(options.stop_delay_ms);
  }
  // SP5-P2 Task 5: the maya dialect answers with the v1.1.21 single-session DONE record
  // (generate.cpp:2111): DONE <produced> <prompt> <prompt_ms> <decode_ms> <finish>
  // <spec-hits> <spec-steps> <reuse> <tier-hits> <tier-total>; the strata dialect keeps
  // the pin's six-field form.
  if (maya)
    std::cout << "DONE " << generated << ' ' << prompt << " 300.0 40.5 " << finish << " 0 0 0 0 0\n" << std::flush;
  else {
    double decode_ms = options.decode_ms;
    for (const auto& [flag, wanted, factor] : options.faster_with) {
      for (std::size_t i = 0; i + 1 < options.command_line.size(); ++i) {
        if (options.command_line[i] == flag && options.command_line[i + 1] == wanted && factor > 0.0) decode_ms /= factor;
      }
    }
    std::cout << "DONE " << generated << ' ' << prompt << " 0 " << decode_ms << ' ' << finish << '\n' << std::flush;
  }
}

int serve(const Options& options) {
#if !defined(_WIN32)
  if (options.ignore_term) (void)std::signal(SIGTERM, SIG_IGN);
#endif
  if (!options.record_file.empty()) std::ofstream(options.record_file, std::ios::trunc).flush();
  if (options.fail_first > 0 && !options.counter_file.empty()) {
    int started = 0;
    {
      std::ifstream in(options.counter_file);
      in >> started;
    }
    {
      std::ofstream out(options.counter_file, std::ios::trunc);
      out << (started + 1) << '\n';
    }
    if (started < options.fail_first) return 1;
  }
  // A real engine may emit human diagnostics on stderr before READY; they must
  // never reach the strict stdout protocol stream.
  std::cerr << "diagnostic: private engine startup\n" << std::flush;
  if (options.write_inherited && !write_inherited(*options.write_inherited)) return 3;
  if (!options.dump_file.empty()) write_dump_file(options.dump_file);
  // maya-mm3 --calibrate-prefill OUT (mm3_main.cpp calibrate_prefill): its JSON in OUT, a calibration line on stderr, exit 0
  for (std::size_t i = 0; i + 1U < options.command_line.size(); ++i) {
    if (options.command_line[i] != "--calibrate-prefill") continue;
    if (!options.prefill_log.empty()) {
      std::ofstream log(options.prefill_log, std::ios::app | std::ios::binary);
      for (std::size_t j = 0; j < options.command_line.size(); ++j) log << (j == 0U ? "" : " ") << options.command_line[j];
      log << '\n';
    }
    if (options.prefill_fail) return 1;
    std::ofstream out(options.command_line[i + 1U], std::ios::binary | std::ios::trunc);
    out << R"({"key": "fake", "file": "prefill-tune.txt", "chunk": 2048, "gemm": "blas", "tok_s": 950.5, "default_tok_s": 900.0, )"
           R"("tokens": 4096, "all_resident": false, "lt_available": false, "seconds": 1, "trials": []})" << '\n';
    std::cerr << "calibration: chunk 2048, BLAS GEMMs: 950.5 tok/s (the defaults 900.0) - kept for this PC\n" << std::flush;
    return 0;
  }
  if (options.exit_before_ready) return *options.exit_before_ready;
  if (options.hang_before_ready) sleep_forever();
  if (options.echo_cmdline) echo_cmdline();
  if (options.ready_delay_ms > 0) sleep_ms(options.ready_delay_ms);
  // Maya v1.1.21's fast path: the startup INFO, then one VRAM record per GPU part before
  // READY (maya-ref src/program/generate.cpp:1044, :1054-1062).
  if (options.dialect == "maya" && options.maya_vram) {
    std::cout << "INFO context=" << options.ready_context << " kv=int8 engine_kind=glm-fast\n"
              << "VRAM dev=0 pci=0000:01:00.0 total=17163091968 free=1073741824 weights=9000000000 kv=1 kv_ram=0 act=1 "
                 "drafter=0 window=0 pool=2 tail=0 borrow=0 vision=0 slots=0 ctx="
              << options.ready_context << " layers=0-77\n"
              << std::flush;
  }
  if (!options.ready_experts.empty()) {
    const auto comma = options.ready_experts.find(',');
    std::cout << "INFO context=" << options.ready_context
              << " expert_slots_primary=" << options.ready_experts.substr(0, comma)
              << " expert_slots=" << (comma == std::string::npos ? std::string{} : options.ready_experts.substr(comma + 1U))
              << "\n" << std::flush;
  }
  const std::string ready = "READY " + std::to_string(options.ready_context) + " stop\n";
  if (options.split_ready) {
    std::cout << ready.substr(0, 3) << std::flush;
    sleep_ms(100);
    std::cout << ready.substr(3) << std::flush;
  } else {
    std::cout << ready << std::flush;
  }
  if (options.spawn_grandchild) {
    const long grandchild = spawn_grandchild(options);
#if defined(_WIN32)
    if (!options.grandchild_pidfile.empty()) write_pidfile(options.grandchild_pidfile, grandchild);
#endif
    if (options.grandchild_pidfile.empty()) std::cout << "GRANDCHILD " << grandchild << '\n' << std::flush;
  }
  if (options.crash_after_ready) {
    if (options.crash_delay_ms > 0) sleep_ms(options.crash_delay_ms);
    crash(*options.crash_after_ready);
  }
  if (options.stall_stdin) sleep_forever();
  if (options.slow_stdin_bytes > 0) {
    drain_stdin_slowly(options.slow_stdin_bytes, options.slow_stdin_interval_ms);
    return 0;
  }
  std::string line;
  if (options.spew) {
    // One record larger than kMaxReadLineBytes with no newline, then stay alive.
    std::cout << std::string(9000, 'x') << std::flush;
    while (std::getline(std::cin, line)) {
    }
    return 0;
  }
  StdinReader reader(options.record_file);
  while (const auto next = reader.next()) {
    const std::string& record = *next;
    if (record.rfind("GEN ", 0) == 0 || record.rfind("GENI ", 0) == 0) {
      if (geni_out_of_order(record)) {
        std::cout << "ERR expected: GEN <max_new> <id,id,...> or GENI <max_new> <file> <id,id,...>\n" << std::flush;
        continue;
      }
      if (options.hang_in_gen) sleep_forever();  // stops acting on stdin: STOP is never seen
      if (options.abort_in_gen) {
        std::cout << "T 7\n" << std::flush;
        crash("ABRT");
      }
      if (options.fatal_line) {
        const auto index = static_cast<std::size_t>(*options.fatal_line);
        std::cout << "T 7\n" << kFatalPrefixes[index] << (index < 2U ? "12" : "") << "\nDONE stop\n" << std::flush;
        continue;
      }
      if (options.stream_modes) { run_generation(options, record, reader); continue; }
      std::cout << "T 7\nDONE stop\n" << std::flush;
    } else if (record == "QUIT") {
      return 0;
    } else if (record == "DUMP") {
      dump();
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
  // The crash modes (ABRT, SEGV, --abort-in-gen) must end the process at once.
  // Without this a crash can wait on a Windows Error Reporting / critical-error
  // dialog on the MSYS2 runner and the test hangs until its CTest timeout. The
  // error mode is inherited by --fake-sleeper grandchildren.
  // --default-error-mode skips this, so the fake runs with the error mode it
  // inherited from the frontend (SP2 Task 17: proc04_windows_abort_does_not_stall
  // exercises the frontend's launch settings, not the fake's own).
  bool default_error_mode = false;
  for (int index = 1; index < argc; ++index) default_error_mode = default_error_mode || std::string_view(argv[index]) == "--default-error-mode";
  if (!default_error_mode) (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
#if !defined(_WIN32)
  g_argc = argc;
  g_argv = argv;
#endif
  for (int index = 1; index < argc; ++index) {
    if (std::string_view(argv[index]) == "--fake-sleeper") {
      sleep_ms(60000);
      return 0;
    }
    // Session log step 16 (the Monitor's GpuSampler): nvidia-smi's csv answer, every 50 ms,
    // --fake-nvidia-smi=N lines then exit (no N: until killed).
    const std::string_view argument{argv[index]};
    if (argument == "--fake-nvidia-smi" || argument.rfind("--fake-nvidia-smi=", 0) == 0) {
      const long lines = argument.size() > 18U ? std::stol(std::string{argument.substr(18U)}) : -1L;
      for (long printed = 0; lines < 0 || printed < lines; ++printed) {
        std::cout << "Fake GPU, 42, 2048, 16384, 55, 120.50, 320.00, 4, 4, 16\n" << std::flush;
        sleep_ms(50);
      }
      return 0;
    }
  }
  // The detached stdin reader can be blocked inside std::getline when serve
  // returns: exit() runs the stdio cleanup, which locks every FILE —
  // including the stdin the reader holds — and main would wait on that lock
  // forever (observed: QUIT leaves the fake alive, deadlock in exit). The
  // explicit flush + _Exit is the "return/_Exit" exit the detached thread's
  // comment promises: protocol lines are flushed at emit time, dump output
  // here, and record-file appends are flushed at each close.
  const int rc = serve(parse_options(option_arguments(argc, argv)));
  std::cout.flush();
  std::cerr.flush();
  std::_Exit(rc);
}
