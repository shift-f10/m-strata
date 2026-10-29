#pragma once
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include "support/non_elevated_launch.hpp"
#include "support/windows_command_line.hpp"
#include <cwchar>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace secure_frontend_test {

struct ProcessSpec final {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  std::vector<std::pair<std::string, std::string>> environment;  // the child's whole environment
  std::filesystem::path stdin_file{};                              // empty: the null device, as before
};

struct ProcessResult final {
  int exit_code;  // -1: killed at the timeout; -2: could not start
  std::string out;
  std::string err;
};

inline bool contains(const std::string& haystack, const std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

// stdin is the null device (or spec.stdin_file when set); stdout/stderr go to
// files in a scratch directory
// (files, not pipes, so a chatty child can never block on a full pipe).
class ChildProcess final {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ~ChildProcess() {
    terminate();
#if defined(_WIN32)
    if (process_ != nullptr) CloseHandle(process_);
#endif
  }

  bool start(const ProcessSpec& spec) {
    output_ = make_scratch_directory();
#if defined(_WIN32)
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE in = spec.stdin_file.empty()
                    ? CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr)
                    : CreateFileW(spec.stdin_file.c_str(), GENERIC_READ, FILE_SHARE_READ, &inherit, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE out = CreateFileW((output_ / "stdout").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE err = CreateFileW((output_ / "stderr").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    std::wstring command = quote_windows_argument(spec.executable.wstring());
    for (const auto& argument : spec.arguments) command += L" " + quote_windows_argument(widen(argument));
    std::vector<std::wstring> entries;
    bool has_system_root = false;
    for (const auto& [key, value] : spec.environment) {
      entries.push_back(widen(key) + L"=" + widen(value));
      if (_wcsicmp(widen(key).c_str(), L"SystemRoot") == 0) has_system_root = true;
    }
    wchar_t system_root[MAX_PATH] = {};
    if (!has_system_root && GetEnvironmentVariableW(L"SystemRoot", system_root, MAX_PATH) > 0) {
      entries.push_back(std::wstring{L"SystemRoot="} + system_root);  // Winsock needs it
    }
    std::sort(entries.begin(), entries.end());
    std::wstring block;
    for (const auto& entry : entries) { block += entry; block.push_back(L'\0'); }
    if (entries.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = in;
    startup.hStdOutput = out;
    startup.hStdError = err;
    PROCESS_INFORMATION info{};
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
    // G4: strata-unified-server refuses an elevated token, so it always starts
    // through the shared non-elevated launch (no override flag exists); any
    // other executable keeps the plain CreateProcessW.
    // Selected by name: a renamed copy of the server takes the plain path and
    // is then refused by G4 itself (a loud failure, never a bypass).
    const bool server = spec.executable.stem() == L"strata-unified-server";
    const bool ok = server ? create_non_elevated_process(spec.executable.wstring(), command, flags, block.data(), startup, info,
                                                         "process_runner")
                           : CreateProcessW(spec.executable.c_str(), command.data(), nullptr, nullptr, TRUE, flags,
                                            block.data(), nullptr, &startup, &info) != 0;
    CloseHandle(in);
    CloseHandle(out);
    CloseHandle(err);
    if (!ok) return false;
    if (server) (void)ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    process_ = info.hProcess;
    return true;
#else
    const std::string out = (output_ / "stdout").string();
    const std::string err = (output_ / "stderr").string();
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, spec.stdin_file.empty() ? "/dev/null" : spec.stdin_file.c_str(), O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_addopen(&actions, 2, err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    std::vector<std::string> argv_text{spec.executable.string()};
    argv_text.insert(argv_text.end(), spec.arguments.begin(), spec.arguments.end());
    std::vector<char*> argv;
    for (auto& argument : argv_text) argv.push_back(argument.data());
    argv.push_back(nullptr);
    std::vector<std::string> env_text;
    for (const auto& [key, value] : spec.environment) env_text.push_back(key + "=" + value);
    std::vector<char*> envp;
    for (auto& entry : env_text) envp.push_back(entry.data());
    envp.push_back(nullptr);
    const int rc = posix_spawn(&pid_, argv_text.front().c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) pid_ = -1;
    return rc == 0;
#endif
  }

  std::optional<int> wait_for(const std::chrono::milliseconds timeout) {
    if (exit_code_.has_value()) return exit_code_;
#if defined(_WIN32)
    if (process_ == nullptr) return std::nullopt;
    if (WaitForSingleObject(process_, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) return std::nullopt;
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    exit_code_ = static_cast<int>(code);
    return exit_code_;
#else
    if (pid_ <= 0) return std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      int status = 0;
      const pid_t reaped = waitpid(pid_, &status, WNOHANG);
      if (reaped == pid_) {
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        return exit_code_;
      }
      if (reaped < 0 || std::chrono::steady_clock::now() >= deadline) return std::nullopt;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
#endif
  }

  void terminate() {
    if (exit_code_.has_value()) return;
#if defined(_WIN32)
    if (process_ == nullptr) return;
    TerminateProcess(process_, 1);
    (void)wait_for(std::chrono::seconds(5));
#else
    if (pid_ <= 0) return;
    (void)kill(pid_, SIGTERM);
    if (!wait_for(std::chrono::seconds(5)).has_value()) {
      (void)kill(pid_, SIGKILL);
      (void)wait_for(std::chrono::seconds(5));
    }
#endif
  }

  [[nodiscard]] long pid() const noexcept {
#if defined(_WIN32)
    return process_ != nullptr ? static_cast<long>(GetProcessId(process_)) : -1L;
#else
    return static_cast<long>(pid_);
#endif
  }
  [[nodiscard]] std::string stdout_text() const { return read_file(output_ / "stdout"); }
  [[nodiscard]] std::string stderr_text() const { return read_file(output_ / "stderr"); }

 private:
#if defined(_WIN32)
  static std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
  }
  HANDLE process_{nullptr};
#else
  pid_t pid_{-1};
#endif
  std::filesystem::path output_;
  std::optional<int> exit_code_;
};

inline ProcessResult run_to_completion(const ProcessSpec& spec,
                                       const std::chrono::milliseconds timeout = std::chrono::seconds(60)) {
  ChildProcess child;
  if (!child.start(spec)) return ProcessResult{-2, {}, "spawn failed"};
  const auto code = child.wait_for(timeout);
  if (!code.has_value()) {
    child.terminate();
    return ProcessResult{-1, child.stdout_text(), child.stderr_text()};
  }
  return ProcessResult{*code, child.stdout_text(), child.stderr_text()};
}

}  // namespace secure_frontend_test
