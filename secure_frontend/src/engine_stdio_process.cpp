#include "secure_frontend/engine_stdio_process.hpp"

#include "secure_frontend/cgroup_scope.hpp"
#include "secure_frontend/config_defaults.hpp"
#include "secure_frontend/deadline.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"

#include <sddl.h>

#include <variant>

#include "secure_frontend/token_generator.hpp"
#include "win_child_launch.hpp"
#else
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "posix_child_launch.hpp"
#endif

namespace secure_frontend {
namespace {

bool contains_nul(const std::string_view value) { return value.find('\0') != std::string_view::npos; }
// Checks the native form: path::string() narrows through the ANSI code page on
// Windows and can throw or mangle a non-ASCII path.
bool path_contains_nul(const std::filesystem::path& value) {
  return value.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos;
}

std::optional<ProcessError> validate(const EngineCommand& command) {
  if (command.executable.empty() || path_contains_nul(command.executable)) {
    return ProcessError{ProcessErrorCode::invalid_command, "engine executable is invalid"};
  }
  for (const auto& arg : command.arguments) {
    if (arg == "--serve" || contains_nul(arg)) {
      return ProcessError{ProcessErrorCode::invalid_command, "engine arguments contain an invalid entry"};
    }
  }
  for (const auto& [name, value] : command.environment) {
    if (name.empty() || name.find('=') != std::string::npos || contains_nul(name) || contains_nul(value)) {
      return ProcessError{ProcessErrorCode::invalid_command, "engine environment contains an invalid entry"};
    }
  }
  return std::nullopt;
}

#if !defined(_WIN32)
void close_fd(int& fd) noexcept { if (fd >= 0) { (void)close(fd); fd = -1; } }

// A pidfd becomes readable when the child exits, so the supervisor can wait on
// the child itself (kernel 5.3+). -1 means "poll instead".
int open_pidfd(const pid_t pid) noexcept {
#if defined(__linux__) && defined(SYS_pidfd_open)
  return static_cast<int>(syscall(SYS_pidfd_open, pid, 0U));
#else
  (void)pid;
  return -1;
#endif
}

bool write_without_sigpipe(const int fd, const char* data, const std::size_t count, ssize_t& written) {
  sigset_t block{};
  sigemptyset(&block);
  sigaddset(&block, SIGPIPE);
  sigset_t prior{};
  if (pthread_sigmask(SIG_BLOCK, &block, &prior) != 0) return false;
  written = write(fd, data, count);
  const int saved_errno = errno;
  if (written < 0 && saved_errno == EPIPE && sigismember(&prior, SIGPIPE) == 0) {
    timespec zero{};
    (void)sigtimedwait(&block, nullptr, &zero);
  }
  (void)pthread_sigmask(SIG_SETMASK, &prior, nullptr);
  errno = saved_errno;
  return true;
}
#else
std::optional<std::wstring> to_wide(const std::string& text) {
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0 && !text.empty()) return std::nullopt;
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  if (length != 0 && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length) != length) return std::nullopt;
  return result;
}
std::wstring quote(const std::wstring_view arg) {
  std::wstring result{L"\""}; std::size_t slashes = 0;
  for (const wchar_t ch : arg) { if (ch == L'\\') ++slashes; else if (ch == L'\"') { result.append(slashes * 2 + 1, L'\\'); result += ch; slashes = 0; } else { result.append(slashes, L'\\'); result += ch; slashes = 0; } }
  result.append(slashes * 2, L'\\'); result += L'\"'; return result;
}
// Fixed text plus the numeric Win32 error code: no paths, names or secrets.
std::string with_last_error(const char* what) { return std::string(what) + " (Win32 error " + std::to_string(GetLastError()) + ")"; }
void close_handle(void*& handle) noexcept { if (handle != nullptr) { CloseHandle(static_cast<HANDLE>(handle)); handle = nullptr; } }

// 128 random bits for the pipe name (spec §3.11), from SP1's fail-closed CSPRNG.
std::optional<std::wstring> random_pipe_suffix() {
  std::array<unsigned char, 16> bytes{};
  if (!fill_random(bytes.data(), bytes.size())) return std::nullopt;
  static constexpr wchar_t kDigits[] = L"0123456789abcdef";
  std::wstring text;
  for (const unsigned char byte : bytes) {
    text.push_back(kDigits[byte >> 4U]);
    text.push_back(kDigits[byte & 0x0fU]);
  }
  return text;
}

// D:P(A;;GA;;;<current user SID>): protected, one ACE, current user only.
// The caller LocalFree()s the result.
PSECURITY_DESCRIPTOR owner_only_descriptor() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;
  DWORD size = 0;
  (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<std::byte> buffer(size);
  const bool have_user = size != 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
  CloseHandle(token);
  if (!have_user) return nullptr;
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) return nullptr;
  const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
  LocalFree(sid);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return nullptr;
  return descriptor;
}
#endif

}  // namespace

EngineStdioProcess::~EngineStdioProcess() { cleanup(); }

std::optional<ProcessError> EngineStdioProcess::start(const EngineCommand& command) {
  if (running()) return ProcessError{ProcessErrorCode::already_running, "an engine process is already running"};
  if (const auto error = validate(command); error.has_value()) return error;
  // Ruling T11-x: remember the role's kill code; supervisor kills tag the child with it.
  kill_exit_code_ = command.kill_exit_code;
  max_read_line_ = command.max_read_line_bytes;
  cleanup(); pending_stdout_.clear(); exit_status_.reset();
#if defined(_WIN32)
  const std::wstring executable = command.executable.wstring();
  std::wstring line = quote(executable);
  // SP5 M1 Task 11 (Ruling T11-ff): the literal --serve insertion is the ENGINE protocol;
  // the pin's vision child takes none (serve/server.py:2556-2568), so insert_serve gates it.
  if (command.insert_serve) line += L" --serve";
  for (const auto& arg : command.arguments) { auto wide = to_wide(arg); if (!wide) return ProcessError{ProcessErrorCode::invalid_command, "engine argument is not valid UTF-8"}; line += L' ' + quote(*wide); }
  std::wstring environment;
  for (const auto& [name, value] : command.environment) { auto n = to_wide(name); auto v = to_wide(value); if (!n || !v) return ProcessError{ProcessErrorCode::invalid_command, "engine environment is not valid UTF-8"}; environment += *n + L"=" + *v; environment.push_back(L'\0'); }
  environment.push_back(L'\0');
  SECURITY_ATTRIBUTES attrs{sizeof(attrs), nullptr, TRUE}; HANDLE child_stdin = nullptr, child_stdout = nullptr, child_stderr = nullptr, parent_stdin = nullptr, parent_stdout = nullptr;
  // Spec §3.11: an owner-only (protected DACL, current user), single-instance
  // pipe with a 128-bit random name that rejects remote clients. The frontend
  // end is overlapped so writes have a deadline (§3.10). After our own client
  // open the pipe's client pid must be ours, which closes the window in which
  // another process could connect first. The parent end is not inheritable;
  // only the child's read end (opened with `attrs`) is.
  const auto suffix = random_pipe_suffix();
  PSECURITY_DESCRIPTOR descriptor = owner_only_descriptor();
  if (!suffix || descriptor == nullptr) {
    if (descriptor != nullptr) LocalFree(descriptor);
    return ProcessError{ProcessErrorCode::platform_failure, "stdio pipe setup failed"};
  }
  SECURITY_ATTRIBUTES pipe_attrs{sizeof(pipe_attrs), descriptor, FALSE};
  const std::wstring pipe_name = L"\\\\.\\pipe\\secure_frontend-" + std::to_wstring(GetCurrentProcessId()) + L"-" + *suffix;
  parent_stdin = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                  PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                  static_cast<DWORD>(config_defaults::kStdinWriteChunk), 0, 0, &pipe_attrs);
  LocalFree(descriptor);
  if (parent_stdin == INVALID_HANDLE_VALUE) parent_stdin = nullptr;
  if (parent_stdin != nullptr) {
    child_stdin = CreateFileW(pipe_name.c_str(), GENERIC_READ, 0, &attrs, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (child_stdin == INVALID_HANDLE_VALUE) child_stdin = nullptr;
  }
  ULONG client_pid = 0;
  if (child_stdin != nullptr) {
    const bool queried = GetNamedPipeClientProcessId(parent_stdin, &client_pid) != FALSE;
    if (!queried || client_pid != GetCurrentProcessId()) {
      // A foreign client: report it as access denied rather than a stale error code.
      if (queried) SetLastError(ERROR_ACCESS_DENIED);
      const DWORD reason = GetLastError();
      CloseHandle(child_stdin);
      child_stdin = nullptr;
      SetLastError(reason);
    }
  }
  if (parent_stdin == nullptr || child_stdin == nullptr || !CreatePipe(&parent_stdout, &child_stdout, &attrs, 0) || !SetHandleInformation(parent_stdout, HANDLE_FLAG_INHERIT, 0)) { if (child_stdin) CloseHandle(child_stdin); if (parent_stdin) CloseHandle(parent_stdin); if (child_stdout) CloseHandle(child_stdout); if (parent_stdout) CloseHandle(parent_stdout); return ProcessError{ProcessErrorCode::platform_failure, with_last_error("stdio pipe setup failed")}; }
  child_stderr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attrs, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (child_stderr == INVALID_HANDLE_VALUE) { CloseHandle(child_stdin); CloseHandle(parent_stdin); CloseHandle(child_stdout); CloseHandle(parent_stdout); return ProcessError{ProcessErrorCode::platform_failure, with_last_error("stderr sink setup failed")}; }
  // The Job, creation flags, handle list, cwd and error mode live in
  // win_launch::launch_in_job (SP2 R4: shared with SP4's downloader); the
  // engine passes none of the SP4 seam inputs and turns the two Job options
  // (default ON for new callers, RF11 amended) off explicitly, so its launch is unchanged:
  // Job = KILL_ON_JOB_CLOSE | DIE_ON_UNHANDLED_EXCEPTION, created suspended,
  // assigned, resumed; CREATE_NO_WINDOW (SP2 Task 15 ruling). Authoritative in MSYS2.
  // Spec §3.12: the engine's cwd-relative files land in <state_dir>/engine; empty: inherit.
  win_launch::WinLaunchStep step = win_launch::WinLaunchStep::none;
  const auto launched = win_launch::launch_in_job({.executable = executable,
                                                   .command_line = line,
                                                   .environment_block = environment,
                                                   .cwd = command.working_directory.wstring(),
                                                   .stdin_read = child_stdin,
                                                   .stdout_write = child_stdout,
                                                   .stderr_handle = child_stderr,
                                                   .primary_token = nullptr,
                                                   .extra_inherited_handles = {},
                                                   .crt_fd_block = {},
                                                   .limit_active_process_to_one = false,
                                                   .restrict_ui = false},
                                                  &step);
  CloseHandle(child_stdin); CloseHandle(child_stdout); CloseHandle(child_stderr);
  if (const auto* code = std::get_if<DWORD>(&launched)) {
    CloseHandle(parent_stdin); CloseHandle(parent_stdout);
    const std::string number = " (Win32 error " + std::to_string(*code) + ")";
    switch (step) {
      case win_launch::WinLaunchStep::create_process:
        // Spec §3.6/G3: an engine that asks for elevation is a spawn failure; it is never retried via runas.
        if (*code == ERROR_ELEVATION_REQUIRED) return ProcessError{ProcessErrorCode::spawn_failed, "engine requires elevation; refused"};
        return ProcessError{ProcessErrorCode::spawn_failed, "CreateProcessW failed" + number};
      case win_launch::WinLaunchStep::job:
        return ProcessError{ProcessErrorCode::platform_failure, "CreateJobObjectW failed" + number};
      case win_launch::WinLaunchStep::attributes:
        return ProcessError{ProcessErrorCode::platform_failure, "process handle-list setup failed" + number};
      case win_launch::WinLaunchStep::assign_job:
        return ProcessError{ProcessErrorCode::platform_failure, "AssignProcessToJobObject failed" + number};
      case win_launch::WinLaunchStep::resume:
      case win_launch::WinLaunchStep::none:
        break;
    }
    return ProcessError{ProcessErrorCode::platform_failure, "ResumeThread failed" + number};
  }
  const auto& child = std::get<win_launch::WinLaunched>(launched);
  CloseHandle(child.thread);
  process_handle_ = child.process; job_handle_ = child.job; stdin_write_ = parent_stdin; stdout_read_ = parent_stdout; stdin_pipe_name_ = pipe_name; return std::nullopt;
#else
  // Everything the child needs is built before fork(); the child body is async-signal-safe.
  posix_launch::PreparedExec prepared;
  posix_launch::prepare(prepared, command, command.insert_serve);
  if (!posix_launch::open_cgroup(prepared, command)) {
    return ProcessError{ProcessErrorCode::platform_failure, "engine cgroup setup failed"};
  }
  // O_CLOEXEC on every pipe end: nothing created here can leak into this or any
  // other child.  The child's ends reach fds 0/1 through dup2, which clears it.
  int input[2]{-1, -1}; int output[2]{-1, -1}; int errors[2]{-1, -1};
  // The frontend's stdin end is also O_NONBLOCK (spec §3.10): write_line()
  // bounds every wait with poll(), never with a blocking write. The flag is on
  // the write end's own file description; the engine's read end is unaffected.
  const auto nonblocking = [](const int fd) { const int flags = fcntl(fd, F_GETFL); return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0; };
  if (pipe2(input, O_CLOEXEC) != 0 || pipe2(output, O_CLOEXEC) != 0 || pipe2(errors, O_CLOEXEC) != 0 || !nonblocking(input[1])) { for (int* pipe_fd : {input, output, errors}) { close_fd(pipe_fd[0]); close_fd(pipe_fd[1]); } return ProcessError{ProcessErrorCode::platform_failure, "pipe setup failed"}; }
  pid_t child = -1;
  {
    posix_launch::BlockAllSignals blocked;
    child = fork();
    if (child == 0) posix_launch::exec_child(prepared, input[0], output[1], /*null_stderr=*/true, errors[1]);
  }
  if (child < 0) { for (int* pipe_fd : {input, output, errors}) { close_fd(pipe_fd[0]); close_fd(pipe_fd[1]); } return ProcessError{ProcessErrorCode::spawn_failed, "fork failed"}; }
  // EOF on the report pipe = execve succeeded; an errno = a launch step or execve
  // failed; 'H' + errno = a pre-exec hook failed (SP4 seam; never from the engine,
  // which passes no hook) — each with its own text (RF11).
  close_fd(input[0]); close_fd(output[1]); close_fd(errors[1]); const auto report = posix_launch::classify_report(posix_launch::read_report(errors[0])); close_fd(errors[0]);
  if (report.kind != posix_launch::ChildReportKind::none) { close_fd(input[1]); close_fd(output[0]); int ignored = 0; (void)waitpid(child, &ignored, 0); return ProcessError{ProcessErrorCode::spawn_failed, posix_launch::describe_report(report)}; }
  process_id_ = child; process_group_ = child; pidfd_ = open_pidfd(child); stdin_write_ = input[1]; stdout_read_ = output[0];
  cgroup_dir_ = command.cgroup_procs ? std::optional<std::filesystem::path>(command.cgroup_procs->parent_path()) : std::nullopt;
  return std::nullopt;
#endif
}

std::optional<ExitStatus> EngineStdioProcess::try_reap() {
  if (exit_status_) return exit_status_;
#if defined(_WIN32)
  if (process_handle_ == nullptr || WaitForSingleObject(static_cast<HANDLE>(process_handle_), 0) != WAIT_OBJECT_0) return std::nullopt;
  DWORD code = 0;
  if (!GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code)) code = 0xFFFFFFFFU;
  exit_status_ = ExitStatus{false, static_cast<std::uint32_t>(code)};
  return exit_status_;
#else
  if (process_id_ < 0) return std::nullopt;
  int status = 0;
  const pid_t value = waitpid(process_id_, &status, WNOHANG);
  if (value == process_id_) {
    exit_status_ = WIFSIGNALED(status) ? ExitStatus{true, static_cast<std::uint32_t>(WTERMSIG(status))}
                                       : ExitStatus{false, static_cast<std::uint32_t>(WIFEXITED(status) ? WEXITSTATUS(status) : 255)};
  } else if (value < 0 && errno == ECHILD) {
    // Reaped by someone else: the status is unknown. Cannot happen while only the supervisor reaps.
    exit_status_ = ExitStatus{false, 255U};
  } else {
    return std::nullopt;
  }
  process_id_ = -1;
  close_fd(pidfd_);
  return exit_status_;
#endif
}

bool EngineStdioProcess::running() {
#if defined(_WIN32)
  return process_handle_ != nullptr && !try_reap().has_value();
#else
  return process_id_ >= 0 && !try_reap().has_value();
#endif
}

namespace {
// How long the next chunk may wait: the per-chunk deadline, cut short by the
// record's overall deadline (spec §3.10, owner ruling 2026-10-14). Zero means
// the overall deadline has passed.
std::chrono::milliseconds chunk_wait(const MonotonicClock::time_point deadline) {
  return std::min(remaining_until(deadline), config_defaults::kStdinWriteDeadline);
}
}  // namespace

bool EngineStdioProcess::write_line(const std::string_view line, const MonotonicClock::time_point deadline, std::string& error) {
  if (line.empty() || line.size() > kMaxWriteLineBytes || line.find('\n') != std::string_view::npos || line.find('\r') != std::string_view::npos || contains_nul(line)) { error = "protocol line is invalid"; return false; }
  std::string record(line); record.push_back('\n');
#if defined(_WIN32)
  if (!stdin_write_) { error = "engine stdin is closed"; return false; }
  // Spec §3.10: overlapped writes in chunks; a chunk that does not complete
  // within kStdinWriteDeadline, or by the record's overall deadline, is
  // cancelled and the write fails, so a hung or slowly draining engine can
  // never hold the serving thread (the supervisor then kills it).
  HANDLE pipe = static_cast<HANDLE>(stdin_write_);
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr) { error = "engine stdin write failed"; return false; }
  bool ok = true;
  std::size_t offset = 0;
  while (ok && offset < record.size()) {
    const auto wait = chunk_wait(deadline);
    if (wait.count() == 0) {
      error = "engine stdin is not writable";
      ok = false;
      break;
    }
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(record.size() - offset, config_defaults::kStdinWriteChunk));
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    (void)ResetEvent(event);
    DWORD written = 0;
    if (!WriteFile(pipe, record.data() + offset, chunk, nullptr, &overlapped) && GetLastError() != ERROR_IO_PENDING) {
      error = "engine stdin write failed";
      ok = false;
      break;
    }
    if (WaitForSingleObject(event, static_cast<DWORD>(wait.count())) != WAIT_OBJECT_0) {
      (void)CancelIoEx(pipe, &overlapped);
      (void)GetOverlappedResult(pipe, &overlapped, &written, TRUE);  // the cancelled I/O must finish before `overlapped` goes away
      error = "engine stdin is not writable";
      ok = false;
      break;
    }
    if (!GetOverlappedResult(pipe, &overlapped, &written, FALSE) || written == 0) {
      error = "engine stdin write failed";
      ok = false;
      break;
    }
    offset += written;
  }
  CloseHandle(event);
  return ok;
#else
  if (stdin_write_ < 0) { error = "engine stdin is closed"; return false; }
  // The stdin end is O_NONBLOCK (start()), so a write never blocks past the
  // poll: each chunk waits for writability at most chunk_wait(deadline).
  std::size_t offset = 0;
  while (offset < record.size()) {
    const auto wait = chunk_wait(deadline);
    if (wait.count() == 0) { error = "engine stdin is not writable"; return false; }
    pollfd pollfd{stdin_write_, POLLOUT, 0};
    const int status = poll(&pollfd, 1, static_cast<int>(wait.count()));
    if (status < 0 && errno == EINTR) continue;
    if (status <= 0) { error = "engine stdin is not writable"; return false; }
    const std::size_t chunk = std::min(record.size() - offset, config_defaults::kStdinWriteChunk);
    ssize_t count = -1;
    if (!write_without_sigpipe(stdin_write_, record.data() + offset, chunk, count)) { error = "engine stdin write failed"; return false; }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (count <= 0) { error = "engine stdin write failed"; return false; }
    offset += static_cast<std::size_t>(count);
  }
  return true;
#endif
}

std::optional<EngineReadResult> EngineStdioProcess::take_pending_line() {
  const auto newline = pending_stdout_.find('\n');
  if (newline == std::string::npos) return std::nullopt;
  std::string line = pending_stdout_.substr(0, newline);
  pending_stdout_.erase(0, newline + 1);
  if (!line.empty() && line.back() == '\r') line.pop_back();
  if (line.size() > max_read_line_ || line.find('\0') != std::string::npos) return EngineReadResult{EngineReadKind::failure, {}, "engine emitted an invalid line"};
  return EngineReadResult{EngineReadKind::line, std::move(line), {}};
}

EngineReadResult EngineStdioProcess::read_line(const std::chrono::milliseconds timeout) {
  // K1: a zero budget is already expired on every path, even with a buffered line.
  if (timeout.count() <= 0) return {EngineReadKind::timeout, {}, {}};
  if (auto pending = take_pending_line(); pending.has_value()) return std::move(*pending);
#if defined(_WIN32)
  // Iterative and bounded exactly like POSIX: an oversized line is a failure,
  // the deadline is a timeout, and a dead child with an empty pipe is closed.
  if (!stdout_read_) return {EngineReadKind::closed, {}, {}};
  const auto deadline = deadline_after(timeout);
  std::array<char, 1024> bytes{};
  bool child_exit_seen = false;
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(static_cast<HANDLE>(stdout_read_), nullptr, 0, nullptr, &available, nullptr)) return {EngineReadKind::closed, {}, {}};
    if (available != 0) {
      DWORD read = 0;
      const DWORD wanted = std::min<DWORD>(available, static_cast<DWORD>(bytes.size()));
      if (!ReadFile(static_cast<HANDLE>(stdout_read_), bytes.data(), wanted, &read, nullptr) || read == 0) return {EngineReadKind::closed, {}, {}};
      pending_stdout_.append(bytes.data(), read);
      if (pending_stdout_.size() > max_read_line_ && pending_stdout_.find('\n') == std::string::npos) return {EngineReadKind::failure, {}, "engine emitted an oversized line"};
      if (auto pending = take_pending_line(); pending.has_value()) return std::move(*pending);
      continue;
    }
    if (remaining_until(deadline).count() == 0) return {EngineReadKind::timeout, {}, {}};
    // A 1 ms pause that returns at once when the child has exited.  Output the
    // child wrote before exiting is already in the pipe, so one more empty peek
    // after the exit was observed means nothing further can arrive.
    const DWORD waited = WaitForSingleObject(static_cast<HANDLE>(process_handle_), 1);
    if (waited == WAIT_FAILED) return {EngineReadKind::failure, {}, "engine process wait failed"};
    if (waited == WAIT_OBJECT_0) {
      if (child_exit_seen) return {EngineReadKind::closed, {}, {}};
      child_exit_seen = true;
    }
  }
#else
  if (stdout_read_ < 0) {
    return {EngineReadKind::closed, {}, {}};
  }
  // One deadline for the whole call: every poll waits only for the remaining
  // budget, so a record split across several writes is still read whole as
  // long as its last byte arrives before the deadline.
  const auto deadline = deadline_after(timeout);
  std::array<char, 1024> bytes{};
  for (;;) {
    const auto remaining = remaining_until(deadline);
    const auto bounded = std::clamp<std::int64_t>(remaining.count(), 0, 0x7fffffff);
    pollfd pollfd{stdout_read_, POLLIN | POLLHUP, 0};
    const int status = poll(&pollfd, 1, static_cast<int>(bounded));
    if (status < 0 && errno == EINTR) continue;
    if (status == 0) {
      return {EngineReadKind::timeout, {}, {}};
    }
    if (status < 0) {
      return {EngineReadKind::failure, {}, "engine stdout poll failed"};
    }
    const ssize_t count = read(stdout_read_, bytes.data(), bytes.size());
    if (count == 0) {
      return {EngineReadKind::closed, {}, {}};
    }
    if (count < 0) {
      return {EngineReadKind::failure, {}, "engine stdout read failed"};
    }
    pending_stdout_.append(bytes.data(), static_cast<std::size_t>(count));
    if (pending_stdout_.size() > max_read_line_ && pending_stdout_.find('\n') == std::string::npos) {
      return {EngineReadKind::failure, {}, "engine emitted an oversized line"};
    }
    if (auto pending = take_pending_line(); pending.has_value()) return std::move(*pending);
  }
#endif
}

bool EngineStdioProcess::terminate(const std::chrono::milliseconds grace) {
#if defined(_WIN32)
  if (!process_handle_) return true;
  if (!TerminateJobObject(static_cast<HANDLE>(job_handle_), kill_exit_code_)) return false;
  if (WaitForSingleObject(static_cast<HANDLE>(process_handle_), static_cast<DWORD>(std::max<std::int64_t>(0, grace.count()))) !=
      WAIT_OBJECT_0) {
    return false;
  }
  // Keep the status of our own kill, so exit_status() is set and classify_exit()
  // credits it to the supervisor ("a supervisor kill classifies as ours").
  (void)try_reap();
  return true;
#else
  if (process_group_ < 0) return true;  // never started, or already fully cleaned up
  if (process_id_ >= 0) {
    if (kill(-process_group_, SIGTERM) != 0 && errno != ESRCH) return false;
    const auto end = std::chrono::steady_clock::now() + grace;
    while (std::chrono::steady_clock::now() < end && !try_reap()) usleep(10000);
  }
  // Spec §3.7(a): always SIGKILL the group, even when the leader was already
  // reaped: members that ignored SIGTERM or outlived the leader keep the group
  // id valid and must die too.
  (void)kill(-process_group_, SIGKILL);
  for (int step = 0; step < 50 && process_id_ >= 0; ++step) {
    if (try_reap()) break;
    usleep(10000);
  }
  if (process_id_ >= 0) return false;  // abandoned: still unreaped after the bound (header contract)
  close_fd(stdin_write_);
  close_fd(stdout_read_);
  process_group_ = -1;
  return true;
#endif
}

std::intptr_t EngineStdioProcess::monitor_handle() const noexcept {
#if defined(_WIN32)
  return process_handle_ != nullptr ? reinterpret_cast<std::intptr_t>(process_handle_) : kNoMonitorHandle;
#else
  return pidfd_ >= 0 ? static_cast<std::intptr_t>(pidfd_) : kNoMonitorHandle;
#endif
}

void EngineStdioProcess::close_stdin() noexcept {
#if defined(_WIN32)
  close_handle(stdin_write_);
#else
  close_fd(stdin_write_);
#endif
}

void EngineStdioProcess::signal_group_terminate() noexcept {
#if !defined(_WIN32)
  if (process_group_ > 0) (void)kill(-process_group_, SIGTERM);
#endif
}

void EngineStdioProcess::kill_group() noexcept {
#if defined(_WIN32)
  if (job_handle_ != nullptr) (void)TerminateJobObject(static_cast<HANDLE>(job_handle_), kill_exit_code_);
#else
  if (cgroup_dir_) (void)kill_cgroup(*cgroup_dir_);  // escape-proof when delegated (spec §3.7d)
  // Spec §3.7a: the process-group id stays valid while any member lives, so
  // this reaches grandchildren even after the leader was reaped.
  if (process_group_ > 0) (void)kill(-process_group_, SIGKILL);
#endif
}

void EngineStdioProcess::release_resources(const std::chrono::milliseconds bound) noexcept {
#if !defined(_WIN32)
  // The cgroup empties just after the leader is reaped, so even a 0 ms caller
  // (shutdown, exception path) gets kCgroupRemoveBound; a failed removal keeps
  // cgroup_dir_ so cleanup() kills and retries it.
  if (cgroup_dir_ && remove_cgroup(*cgroup_dir_, std::max(bound, kCgroupRemoveBound))) cgroup_dir_.reset();
  process_group_ = -1;  // never signal a group id the kernel may hand out again
#else
  (void)bound;
#endif
  if (generation_temp_dir_) {
    // Called after the reap (end_generation, shutdown_generation): nothing of
    // this generation can still be writing here.
    try {
      std::error_code ignored;
      std::filesystem::remove_all(*generation_temp_dir_, ignored);
    } catch (...) {
    }
    generation_temp_dir_.reset();
  }
}

long EngineStdioProcess::pid() const noexcept {
#if defined(_WIN32)
  return process_handle_ != nullptr ? static_cast<long>(GetProcessId(static_cast<HANDLE>(process_handle_))) : -1;
#else
  return process_group_;
#endif
}

void EngineStdioProcess::cleanup() noexcept { (void)terminate(std::chrono::milliseconds(250));
#if defined(_WIN32)
  close_handle(stdin_write_); close_handle(stdout_read_); close_handle(process_handle_); close_handle(job_handle_);
#else
  close_fd(stdin_write_); close_fd(stdout_read_); close_fd(pidfd_);
  if (cgroup_dir_) {  // never released, or its removal failed: last try
    (void)kill_cgroup(*cgroup_dir_);
    if (remove_cgroup(*cgroup_dir_, kCgroupRemoveBound)) cgroup_dir_.reset();
  }
#endif
}

}  // namespace secure_frontend
