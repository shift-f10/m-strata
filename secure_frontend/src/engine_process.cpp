#include "secure_frontend/engine_process.hpp"
#include "secure_frontend/exit_status.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "posix_child_launch.hpp"
#endif

namespace secure_frontend {
namespace {

#if !defined(_WIN32)
// POSIX-only: the Windows wait path blocks on the process handle instead.
constexpr auto kPollInterval = std::chrono::milliseconds(10);
#endif

bool invalid_environment_name(const std::string& name) {
  return name.empty() || name.find('=') != std::string::npos || name.find('\0') != std::string::npos;
}

bool contains_nul(const std::string& value) {
  return value.find('\0') != std::string::npos;
}

// Checks the native form: path::string() narrows through the ANSI code page on
// Windows and can throw or mangle a non-ASCII path.
bool path_contains_nul(const std::filesystem::path& value) {
  return value.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos;
}

std::optional<ProcessError> validate_command(const EngineCommand& command) {
  if (command.executable.empty()) {
    return ProcessError{ProcessErrorCode::invalid_command, "engine executable must not be empty"};
  }
  if (path_contains_nul(command.executable)) {
    return ProcessError{ProcessErrorCode::invalid_command, "engine executable path must not contain NUL bytes"};
  }
  for (const auto& argument : command.arguments) {
    if (contains_nul(argument)) {
      return ProcessError{ProcessErrorCode::invalid_command, "engine arguments must not contain NUL bytes"};
    }
  }
  for (const auto& [name, value] : command.environment) {
    if (invalid_environment_name(name) || contains_nul(value)) {
      return ProcessError{ProcessErrorCode::invalid_command, "engine environment contains an invalid entry"};
    }
  }
  return std::nullopt;
}

#if !defined(_WIN32)
// POSIX-only: Windows reads the exit code directly with GetExitCodeProcess.
int normalized_exit_code(const int status) {
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 1;
}
#endif

#if defined(_WIN32)
std::optional<std::wstring> utf8_to_wide(const std::string& text) {
  if (text.empty()) return std::wstring{};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return std::nullopt;
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                          result.data(), length) != length) {
    return std::nullopt;
  }
  return result;
}

std::wstring quote_windows_argument(const std::wstring_view argument) {
  std::wstring quoted{L"\""};
  std::size_t slashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++slashes;
    } else if (character == L'\"') {
      quoted.append(slashes * 2 + 1, L'\\');
      quoted += character;
      slashes = 0;
    } else {
      quoted.append(slashes, L'\\');
      quoted += character;
      slashes = 0;
    }
  }
  quoted.append(slashes * 2, L'\\');
  quoted += L'\"';
  return quoted;
}
#endif

}  // namespace

EngineProcess::~EngineProcess() { cleanup(); }

EngineProcess::EngineProcess(EngineProcess&& other) noexcept
#if defined(_WIN32)
    : process_handle_(other.process_handle_), job_handle_(other.job_handle_), exit_code_(other.exit_code_) {
  other.process_handle_ = nullptr;
  other.job_handle_ = nullptr;
#else
    : process_id_(other.process_id_), exit_code_(other.exit_code_) {
  other.process_id_ = -1;
#endif
}

EngineProcess& EngineProcess::operator=(EngineProcess&& other) noexcept {
  if (this != &other) {
    cleanup();
#if defined(_WIN32)
    process_handle_ = other.process_handle_;
    job_handle_ = other.job_handle_;
    other.process_handle_ = nullptr;
    other.job_handle_ = nullptr;
#else
    process_id_ = other.process_id_;
    other.process_id_ = -1;
#endif
    exit_code_ = other.exit_code_;
  }
  return *this;
}

std::optional<ProcessError> EngineProcess::start(const EngineCommand& command) {
  if (running()) {
    return ProcessError{ProcessErrorCode::already_running, "an engine process is already running"};
  }
  if (const auto validation = validate_command(command); validation.has_value()) {
    return validation;
  }
  exit_code_.reset();

#if defined(_WIN32)
  std::wstring command_line = quote_windows_argument(command.executable.wstring());
  for (const auto& argument : command.arguments) {
    const auto wide = utf8_to_wide(argument);
    if (!wide.has_value()) {
      return ProcessError{ProcessErrorCode::invalid_command, "engine argument is not valid UTF-8"};
    }
    command_line += L' ';
    command_line += quote_windows_argument(*wide);
  }
  std::wstring environment;
  for (const auto& [name, value] : command.environment) {
    const auto wide_name = utf8_to_wide(name);
    const auto wide_value = utf8_to_wide(value);
    if (!wide_name.has_value() || !wide_value.has_value()) {
      return ProcessError{ProcessErrorCode::invalid_command, "engine environment is not valid UTF-8"};
    }
    environment += *wide_name;
    environment += L'=';
    environment += *wide_value;
    environment += L'\0';
  }
  environment += L'\0';
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) return ProcessError{ProcessErrorCode::platform_failure, "CreateJobObjectW failed"};
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    CloseHandle(job);
    return ProcessError{ProcessErrorCode::platform_failure, "SetInformationJobObject failed"};
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  // Spec §3.7: a crashing engine must never wait on a Windows Error Reporting
  // dialog; the error mode is inherited by the child. Set once per process.
  static const UINT previous_error_mode = SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
  (void)previous_error_mode;
  // Start suspended so the child is inside the Job before it runs a single
  // instruction: no early descendant can escape the kill-on-close Job (PROC-05).
  // CREATE_NEW_PROCESS_GROUP: Ctrl-C is disabled in the engine's group and
  // group-targeted Ctrl-Break reaches only the frontend. CREATE_NO_WINDOW
  // (SP2 Task 15 ruling): the engine runs without the frontend's console, so a
  // console-wide Ctrl-Break or a closed console window reaches only the
  // frontend, whose console handler stops the engine in order (spec §3.7).
  // Same flags as the production twin (engine_stdio_process.cpp); this
  // test-only class (RF12) speaks no stdio protocol. Authoritative in MSYS2.
  // It keeps its own CreateProcessW (no stdio redirection, bInheritHandles
  // FALSE, so no handle list) rather than win_launch::launch_in_job (SP2 R4),
  // which always passes three inheritable standard handles.
  const std::wstring working_directory = command.working_directory.wstring();
  if (!CreateProcessW(command.executable.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                      CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW,
                      environment.data(), working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &info)) {
    const DWORD error = GetLastError();
    CloseHandle(job);
    // Spec §3.6/G3: never retried via runas.
    return ProcessError{ProcessErrorCode::spawn_failed,
                        error == ERROR_ELEVATION_REQUIRED ? "engine requires elevation; refused" : "CreateProcessW failed"};
  }
  if (!AssignProcessToJobObject(job, info.hProcess)) {
    TerminateProcess(info.hProcess, 1);  // still suspended: it never ran
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    CloseHandle(job);
    return ProcessError{ProcessErrorCode::platform_failure, "AssignProcessToJobObject failed"};
  }
  if (ResumeThread(info.hThread) == static_cast<DWORD>(-1)) {
    TerminateJobObject(job, kSupervisorKillExitCode);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    CloseHandle(job);
    return ProcessError{ProcessErrorCode::platform_failure, "ResumeThread failed"};
  }
  CloseHandle(info.hThread);
  process_handle_ = info.hProcess;
  job_handle_ = job;
  return std::nullopt;
#else
  // Everything the child needs is built before fork(); the child body is async-signal-safe.
  posix_launch::PreparedExec prepared;
  posix_launch::prepare(prepared, command, /*insert_serve=*/false);
  if (!posix_launch::open_cgroup(prepared, command)) {
    return ProcessError{ProcessErrorCode::platform_failure, "engine cgroup setup failed"};
  }
  int error_pipe[2];
  if (pipe2(error_pipe, O_CLOEXEC) != 0) return ProcessError{ProcessErrorCode::platform_failure, "pipe failed"};
  pid_t child = -1;
  {
    posix_launch::BlockAllSignals blocked;
    child = fork();
    if (child == 0) {
      // Inherited stdio is kept; every other descriptor is closed before execve.
      posix_launch::exec_child(prepared, -1, -1, /*null_stderr=*/false, error_pipe[1]);
    }
  }
  if (child < 0) {
    close(error_pipe[0]); close(error_pipe[1]);
    return ProcessError{ProcessErrorCode::spawn_failed, "fork failed"};
  }
  close(error_pipe[1]);
  // Same report reading as the production twin: execve and hook failures read distinctly (RF11).
  const auto report = posix_launch::classify_report(posix_launch::read_report(error_pipe[0]));
  close(error_pipe[0]);
  if (report.kind != posix_launch::ChildReportKind::none) {
    int status = 0;
    (void)waitpid(child, &status, 0);
    return ProcessError{ProcessErrorCode::spawn_failed, posix_launch::describe_report(report)};
  }
  process_id_ = child;
  return std::nullopt;
#endif
}

bool EngineProcess::running() {
  if (exit_code_.has_value()) return false;
#if defined(_WIN32)
  if (process_handle_ == nullptr) return false;
#else
  if (process_id_ < 0) return false;
#endif
  return !wait_for(std::chrono::milliseconds(0)).has_value();
}

std::optional<int> EngineProcess::wait_for(const std::chrono::milliseconds timeout) {
  if (exit_code_.has_value()) return exit_code_;
#if defined(_WIN32)
  if (process_handle_ == nullptr) return std::nullopt;
  const auto wait = WaitForSingleObject(static_cast<HANDLE>(process_handle_),
      timeout.count() <= 0 ? 0U : static_cast<DWORD>(std::min<std::int64_t>(timeout.count(), 0xFFFFFFFE)));
  if (wait != WAIT_OBJECT_0) return std::nullopt;
  DWORD code = 1;
  if (!GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code)) return std::nullopt;
  exit_code_ = static_cast<int>(code);
  return exit_code_;
#else
  if (process_id_ < 0) return std::nullopt;
  const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds(0));
  do {
    int status = 0;
    const pid_t result = waitpid(process_id_, &status, WNOHANG);
    if (result == process_id_) {
      process_id_ = -1;
      exit_code_ = normalized_exit_code(status);
      return exit_code_;
    }
    if (result < 0 && errno == ECHILD) {
      process_id_ = -1;
      exit_code_ = 1;
      return exit_code_;
    }
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(kPollInterval);
  } while (true);
#endif
}

bool EngineProcess::terminate(const std::chrono::milliseconds grace) {
  if (exit_code_.has_value()) return true;
#if defined(_WIN32)
  if (process_handle_ == nullptr) return true;
  if (TerminateJobObject(static_cast<HANDLE>(job_handle_), kSupervisorKillExitCode) == 0) return false;
  return wait_for(grace).has_value();
#else
  if (process_id_ < 0) return true;
  if (kill(-process_id_, SIGTERM) != 0 && errno != ESRCH) return false;
  if (wait_for(grace).has_value()) return true;
  if (kill(-process_id_, SIGKILL) != 0 && errno != ESRCH) return false;
  return wait_for(std::chrono::seconds(1)).has_value();
#endif
}

void EngineProcess::cleanup() noexcept {
  (void)terminate(std::chrono::milliseconds(250));
#if defined(_WIN32)
  if (process_handle_ != nullptr) { CloseHandle(static_cast<HANDLE>(process_handle_)); process_handle_ = nullptr; }
  if (job_handle_ != nullptr) { CloseHandle(static_cast<HANDLE>(job_handle_)); job_handle_ = nullptr; }
#endif
}

}  // namespace secure_frontend
