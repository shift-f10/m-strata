#pragma once

// Paths and process helpers shared by the SP2 tests, on top of SP1's
// tests/support/test_support.hpp (case runner, owner-only scratch
// directories). The including target must call secure_frontend_test_paths()
// in CMake, which defines STRATA_FAKE_ENGINE_PATH and
// STRATA_UNIFIED_SERVER_PATH.

#include "support/test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <aclapi.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace test_support {

inline constexpr int kSkipReturnCode = 77;

// A case that cannot run on this host (SP2 allows exactly two; see Global
// Constraints). Prints the reason, removes SP1's scratch directories and exits
// 77, which the test's SKIP_RETURN_CODE turns into "Skipped". Call it before
// the case creates any process.
[[noreturn]] inline void skip(const std::string_view reason) {
  std::cout << "SKIP: " << reason << '\n' << std::flush;
  secure_frontend_test::remove_scratch_directories();
  std::exit(kSkipReturnCode);
}

inline std::filesystem::path fake_engine_path() { return std::filesystem::path(STRATA_FAKE_ENGINE_PATH); }
// Gemma 4 plan 1 Task 11: the supervision fake reads more options from this file beside its own
// executable, one per line, after its command line (supervision_fake_engine_main.cpp spells the
// same name). ServerOptions::fake_engine_options writes it.
inline constexpr std::string_view kFakeEngineOptionsFile = "fake-engine.options";
// SP5 M1 Task 11: the test-only maya-vision stand-in (tests/fake_vision_child_main.cpp).
inline std::filesystem::path fake_vision_child_path() { return std::filesystem::path(STRATA_FAKE_VISION_CHILD_PATH); }
inline std::filesystem::path server_path() { return std::filesystem::path(STRATA_UNIFIED_SERVER_PATH); }

// This test binary's own path. Fails the test loudly if the OS call fails or
// truncates (K5), instead of returning a garbage path.
inline std::filesystem::path current_executable() {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    std::cerr << "FATAL: GetModuleFileNameW failed or truncated the path\n";
    std::exit(EXIT_FAILURE);
  }
  return std::filesystem::path(std::wstring(buffer.data(), length));
#else
  std::vector<char> buffer(4096);
  const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
  if (length <= 0 || static_cast<std::size_t>(length) >= buffer.size()) {
    std::cerr << "FATAL: readlink(/proc/self/exe) failed or truncated the path\n";
    std::exit(EXIT_FAILURE);
  }
  return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
#endif
}

inline std::string utf8_path(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

#if defined(_WIN32)
// Objects an elevated process creates are owned by BUILTIN\Administrators,
// which the LUA-restricted token the harness gives the server (Task 3) holds
// deny-only. Grant the current user -- the restricted token's user -- full
// control, inherited by everything created below `path` (SetNamedSecurityInfoW
// also propagates it to what already exists there).
inline bool grant_current_user(const std::filesystem::path& path) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  DWORD size = 0;
  (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> user(size);
  const bool have_user = size != 0 && GetTokenInformation(token, TokenUser, user.data(), size, &size);
  CloseHandle(token);
  if (!have_user) return false;
  EXPLICIT_ACCESSW access{};
  access.grfAccessPermissions = GENERIC_ALL;
  access.grfAccessMode = GRANT_ACCESS;
  access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.TrusteeType = TRUSTEE_IS_USER;
  access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
  std::wstring wide = path.wstring();
  PACL old_dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (GetNamedSecurityInfoW(wide.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl, nullptr,
                            &descriptor) != ERROR_SUCCESS) {
    return false;
  }
  PACL new_dacl = nullptr;
  const bool built = SetEntriesInAclW(1, &access, old_dacl, &new_dacl) == ERROR_SUCCESS;
  LocalFree(descriptor);
  if (!built) return false;
  const bool set = SetNamedSecurityInfoW(wide.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl,
                                         nullptr) == ERROR_SUCCESS;
  LocalFree(new_dacl);
  return set;
}
#endif

// A fresh owner-only directory <scratch>/<tag>: SP1's make_scratch_directory()
// creates a new 0700 directory below $SECURE_FRONTEND_TEST_TMP
// (<build>/test-tmp; Windows: the per-user %TEMP%) that run_cases removes
// after the case, so state from an earlier run never leaks into a test.
inline std::filesystem::path make_temp_dir(const std::string_view tag) {
  const auto path = secure_frontend_test::make_scratch_directory() / std::string(tag);
  secure_frontend_test::make_private_directory(path);
#if defined(_WIN32)
  if (!grant_current_user(path)) {
    std::cerr << "FATAL: could not grant the current user access to a test directory\n";
    std::exit(EXIT_FAILURE);
  }
#endif
  return path;
}

inline bool process_alive(const long pid) {
  if (pid <= 0) return false;
#if defined(_WIN32)
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) return false;
  const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  CloseHandle(process);
  return alive;
#else
  if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) return false;
  std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
  const std::string content((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
  const auto close = content.rfind(')');
  // A zombie has ended; only its parent has not collected it yet.
  return content.empty() || close == std::string::npos || close + 2 >= content.size() || content[close + 2] != 'Z';
#endif
}

inline bool wait_until_gone(const long pid, const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (process_alive(pid)) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

inline std::optional<long> read_pidfile(const std::filesystem::path& path, const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    std::ifstream input(path);
    long pid = 0;
    if (input >> pid && pid > 0) return pid;
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

#if defined(__linux__)
// The cgroup v2 directory `pid` runs in, from /proc/<pid>/cgroup ("0::<path>");
// nullopt when the process is gone or has no v2 entry.
inline std::optional<std::filesystem::path> cgroup_directory_of(const long pid) {
  std::ifstream input("/proc/" + std::to_string(pid) + "/cgroup");
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("0::/", 0) == 0) return std::filesystem::path("/sys/fs/cgroup") / line.substr(4);
  }
  return std::nullopt;
}
#endif

inline void kill_process(const long pid) {
  if (pid <= 0) return;
#if defined(_WIN32)
  HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
  if (process != nullptr) {
    (void)TerminateProcess(process, 1);
    CloseHandle(process);
  }
#else
  (void)kill(static_cast<pid_t>(pid), SIGKILL);
#endif
}

// True when a TCP socket can bind ::1 (port 0). Uses only bind, never connect.
// The caller holds a WinsockGuard on Windows.
inline bool ipv6_loopback_available() {
#if defined(_WIN32)
  const SOCKET s = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return false;
#else
  const int s = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  if (s < 0) return false;
#endif
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_addr.s6_addr[15] = 1;   // ::1, without the in6addr_loopback symbol
  const bool ok = ::bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
#if defined(_WIN32)
  closesocket(s);
#else
  ::close(s);
#endif
  return ok;
}

}  // namespace test_support
