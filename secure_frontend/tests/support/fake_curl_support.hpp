#pragma once
// SP4 §10.1: installs the fake downloader into <scratch>/downloader/ (a
// directory that holds no root, config or secret, so the Landlock read rule
// for the downloader's parent never reduces confinement) and spawns it the way
// the frontend does, with the record descriptor as fd 4 / CRT entry 4.
#include "secure_frontend/confinement_plan.hpp"
#include "support/test_support.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace secure_frontend_test {

struct FakeCurlInstall final {
  std::filesystem::path executable;
  std::filesystem::path scenario_dir;
};

inline FakeCurlInstall install_fake_curl(const std::filesystem::path& scratch, const std::filesystem::path& built_fake,
                                         const std::string_view scenario_text,
                                         const std::vector<std::pair<std::string, std::string>>& fixtures = {}) {
  const auto dir = scratch / "downloader";
  make_private_directory(dir);
  make_private_directory(dir / "scenario");
  const auto executable = dir / built_fake.filename();
  std::filesystem::copy_file(built_fake, executable, std::filesystem::copy_options::overwrite_existing);
#if !defined(_WIN32)
  std::filesystem::permissions(executable, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
#endif
  write_file(dir / "scenario" / "scenario.txt", scenario_text);
  for (const auto& [name, bytes] : fixtures) write_file(dir / "scenario" / name, bytes);
  return {std::filesystem::canonical(executable), dir / "scenario"};
}

inline std::vector<std::string> read_record_lines(const std::filesystem::path& record) {
  std::vector<std::string> lines;
  std::ifstream input(record, std::ios::binary);
  for (std::string line; std::getline(input, line);) {
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}

struct SpawnResult final {
  int exit_code{-1};
  std::string stdout_bytes;
};

#if !defined(_WIN32)
// Empty environment; fd 0 = config pipe, fd 1 = output pipe, fd 2 = /dev/null, fd 4 = record_fd (if >= 0).
inline SpawnResult spawn_with_fd4(const std::vector<std::string>& argv, const std::string_view stdin_bytes, const int record_fd) {
  int in[2];
  int out[2];
  if (::pipe2(in, O_CLOEXEC) != 0) return {};
  if (::pipe2(out, O_CLOEXEC) != 0) {
    ::close(in[0]);
    ::close(in[1]);
    return {};
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, in[0], 0);
  posix_spawn_file_actions_adddup2(&actions, out[1], 1);
  posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
  if (record_fd >= 0) posix_spawn_file_actions_adddup2(&actions, record_fd, 4);
  std::vector<char*> args;
  for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  char* envp[] = {nullptr};
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, argv[0].c_str(), &actions, nullptr, args.data(), envp);
  posix_spawn_file_actions_destroy(&actions);
  ::close(in[0]);
  ::close(out[1]);
  SpawnResult result;
  if (spawned != 0) {
    ::close(in[1]);
    ::close(out[0]);
    return result;
  }
  // A child that exits before reading its stdin must surface as EPIPE here, not
  // kill the test binary. Ignored only after the spawn, so the child keeps the
  // default SIGPIPE disposition.
  struct sigaction ignore_pipe{};
  ignore_pipe.sa_handler = SIG_IGN;
  sigemptyset(&ignore_pipe.sa_mask);
  struct sigaction previous_pipe{};
  const bool pipe_ignored = ::sigaction(SIGPIPE, &ignore_pipe, &previous_pipe) == 0;
  std::size_t written = 0;
  while (written < stdin_bytes.size()) {
    const auto n = ::write(in[1], stdin_bytes.data() + written, stdin_bytes.size() - written);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    written += static_cast<std::size_t>(n);
  }
  ::close(in[1]);
  if (pipe_ignored) ::sigaction(SIGPIPE, &previous_pipe, nullptr);
  char buffer[65536];
  for (ssize_t n; (n = ::read(out[0], buffer, sizeof buffer)) > 0;) result.stdout_bytes.append(buffer, static_cast<std::size_t>(n));
  ::close(out[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  return result;
}
#else
// MSVCRT argv quoting (backslashes double only before a quote).
inline std::wstring quote_windows_command_line(const std::vector<std::string>& argv) {
  std::wstring line;
  for (const auto& narrow : argv) {
    const std::wstring arg = std::filesystem::path(narrow).wstring();
    if (!line.empty()) line.push_back(L' ');
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
      line += arg;
      continue;
    }
    line.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t c : arg) {
      if (c == L'\\') {
        ++backslashes;
        continue;
      }
      if (c == L'"') line.append(backslashes * 2 + 1, L'\\');
      else line.append(backslashes, L'\\');
      backslashes = 0;
      line.push_back(c);
    }
    line.append(backslashes * 2, L'\\');
    line.push_back(L'"');
  }
  return line;
}

// Empty environment; handles 0/1 pipes, 2 NUL, 3 unused, 4 = record (CRT block entry 4).
inline SpawnResult spawn_with_crt_entry4(const std::vector<std::string>& argv, const std::string_view stdin_bytes,
                                         void* record_handle) {
  SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr;
  if (!CreatePipe(&in_read, &in_write, &inherit, 0) || !CreatePipe(&out_read, &out_write, &inherit, 0)) return {};
  SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
  HANDLE null_device = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
  namespace cf = secure_frontend::crt_flags;
  const std::array<std::uint64_t, 5> handles{reinterpret_cast<std::uintptr_t>(in_read), reinterpret_cast<std::uintptr_t>(out_write),
                                             reinterpret_cast<std::uintptr_t>(null_device), secure_frontend::kInvalidHandleBits,
                                             reinterpret_cast<std::uintptr_t>(record_handle)};
  const std::array<std::uint8_t, 5> flags{cf::kOpen | cf::kPipe, cf::kOpen | cf::kPipe, cf::kOpen | cf::kDevice, 0, cf::kOpen};
  auto block = secure_frontend::build_crt_fd_block(handles, flags, sizeof(HANDLE));
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = in_read;
  startup.hStdOutput = out_write;
  startup.hStdError = null_device;
  startup.cbReserved2 = static_cast<WORD>(block.size());
  startup.lpReserved2 = block.data();
  std::wstring command = quote_windows_command_line(argv);
  wchar_t environment[2] = {L'\0', L'\0'};
  PROCESS_INFORMATION info{};
  const BOOL created = CreateProcessW(std::filesystem::path(argv[0]).wstring().c_str(), command.data(), nullptr, nullptr, TRUE,
                                      CREATE_UNICODE_ENVIRONMENT, environment, nullptr, &startup, &info);
  CloseHandle(in_read);
  CloseHandle(out_write);
  CloseHandle(null_device);
  SpawnResult result;
  if (!created) {
    CloseHandle(in_write);
    CloseHandle(out_read);
    return result;
  }
  DWORD written = 0;
  if (!stdin_bytes.empty()) WriteFile(in_write, stdin_bytes.data(), static_cast<DWORD>(stdin_bytes.size()), &written, nullptr);
  CloseHandle(in_write);
  char buffer[65536];
  DWORD n = 0;
  while (ReadFile(out_read, buffer, sizeof buffer, &n, nullptr) && n > 0) result.stdout_bytes.append(buffer, n);
  CloseHandle(out_read);
  WaitForSingleObject(info.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(info.hProcess, &code);
  result.exit_code = static_cast<int>(code);
  CloseHandle(info.hThread);
  CloseHandle(info.hProcess);
  return result;
}
#endif

}  // namespace secure_frontend_test
