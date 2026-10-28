#include "secure_frontend/downloader.hpp"

#if defined(_WIN32)
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/win32_prelude.hpp"
#include "win_child_launch.hpp"

#include <sddl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace secure_frontend {
namespace {

class Handle final {
 public:
  explicit Handle(HANDLE h = nullptr) noexcept : h_(h) {}
  ~Handle() { reset(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  [[nodiscard]] HANDLE get() const noexcept { return h_; }
  [[nodiscard]] bool valid() const noexcept { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }
  void reset(HANDLE h = nullptr) noexcept {
    if (valid()) CloseHandle(h_);
    h_ = h;
  }
  [[nodiscard]] HANDLE release() noexcept {
    HANDLE h = h_;
    h_ = nullptr;
    return h;
  }

 private:
  HANDLE h_;
};

// MSVCRT argv quoting (backslashes double only before a quote). A deliberate copy
// of the tests' quote_windows_command_line (Task 12): SP2's quoting in
// engine_stdio_process.cpp is file-local and test support is never linked into a
// library (controller ruling I12).
std::wstring quote_command_line(const std::vector<std::string>& argv) {
  std::wstring line;
  for (const auto& narrow : argv) {
    const std::wstring arg = path_from_utf8(narrow).wstring();
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
      line.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
      backslashes = 0;
      line.push_back(c);
    }
    line.append(backslashes * 2, L'\\');
    line.push_back(L'"');
  }
  return line;
}

// §3.2 "Privilege": a restricted version of our own token at Low integrity.
// nullptr on any failure; the caller fails closed.
HANDLE make_low_integrity_token() {
  HANDLE own = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT, &own)) {
    return nullptr;
  }
  const Handle own_guard(own);
  HANDLE restricted = nullptr;
  if (!CreateRestrictedToken(own, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr, &restricted)) return nullptr;
  Handle restricted_guard(restricted);
  PSID low = nullptr;
  const std::wstring low_text = path_from_utf8(kLowIntegritySid).wstring();
  if (!ConvertStringSidToSidW(low_text.c_str(), &low)) return nullptr;
  TOKEN_MANDATORY_LABEL label{};
  label.Label.Attributes = SE_GROUP_INTEGRITY;
  label.Label.Sid = low;
  const BOOL set = SetTokenInformation(restricted, TokenIntegrityLevel, &label,
                                       static_cast<DWORD>(sizeof label) + GetLengthSid(low));
  LocalFree(low);
  if (!set) return nullptr;
  return restricted_guard.release();
}

}  // namespace

// validate_downloader is shared (downloader_common.cpp: SP1 open_executable + TrustedInstaller).
int landlock_abi() noexcept { return 0; }

std::string ConfinedDownloader::confinement() const { return "full"; }  // Low integrity + Job; read-up remains (§3.1)

ConfinedDownloader::SpawnOutput ConfinedDownloader::spawn(const std::vector<std::string>& argv, const std::string_view stdin_bytes,
                                                          const std::function<bool(std::string_view)>& on_bytes) {
  SpawnOutput output;
  const Handle token(make_low_integrity_token());
  if (!token.valid()) {
    output.failure = AcquireError{AcquireReason::downloader_confinement_failed, "a Low-integrity token cannot be created"};
    return output;  // fail closed (§3.2 "Unavailable", Windows column)
  }
  SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE in_read = nullptr;
  HANDLE in_write = nullptr;
  if (!CreatePipe(&in_read, &in_write, &inherit, 0)) {
    output.failure = AcquireError{AcquireReason::downloader_unsupported, "pipes cannot be created"};
    return output;
  }
  Handle child_in(in_read);
  Handle parent_in(in_write);
  HANDLE out_read = nullptr;
  HANDLE out_write = nullptr;
  if (!CreatePipe(&out_read, &out_write, &inherit, 0)) {
    output.failure = AcquireError{AcquireReason::downloader_unsupported, "pipes cannot be created"};
    return output;
  }
  const Handle parent_out(out_read);
  Handle child_out(out_write);
  if (!SetHandleInformation(parent_in.get(), HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(parent_out.get(), HANDLE_FLAG_INHERIT, 0)) {
    output.failure = AcquireError{AcquireReason::downloader_unsupported, "pipes cannot be created"};
    return output;
  }
  Handle null_device(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
  if (!null_device.valid()) {
    output.failure = AcquireError{AcquireReason::downloader_unsupported, "the null device cannot be opened"};
    return output;
  }
  Handle record;
  std::vector<HANDLE> extra;
  std::vector<std::uint8_t> block;
#if defined(SECURE_FRONTEND_TEST_HOOKS)
  // §10.1 record channel: CRT entry 4. Ruling R5: OPEN_ALWAYS + append, once per spawn.
  if (!options_.record_file.empty()) {
    record.reset(CreateFileW(options_.record_file.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  }
  if (record.valid()) {
    namespace cf = crt_flags;
    extra.push_back(record.get());
    const std::array<std::uint64_t, kCrtBlockEntries> handles{
        reinterpret_cast<std::uintptr_t>(child_in.get()), reinterpret_cast<std::uintptr_t>(child_out.get()),
        reinterpret_cast<std::uintptr_t>(null_device.get()), kInvalidHandleBits, reinterpret_cast<std::uintptr_t>(record.get())};
    const std::array<std::uint8_t, kCrtBlockEntries> flags{cf::kOpen | cf::kPipe, cf::kOpen | cf::kPipe, cf::kOpen | cf::kDevice, 0,
                                                           cf::kOpen};
    block = build_crt_fd_block(handles, flags, sizeof(HANDLE));
  }
#endif
  // Release builds: extra and block stay empty, so cbReserved2 = 0 (§3.2).
  std::wstring windows(MAX_PATH, L'\0');
  const UINT length = GetSystemWindowsDirectoryW(windows.data(), MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    output.failure = AcquireError{AcquireReason::downloader_unsupported, "the Windows directory cannot be determined"};
    return output;
  }
  windows.resize(length);
  std::wstring environment = L"SystemRoot=" + windows;  // the whole environment (§3.2 PRIV-04, Windows column)
  environment.push_back(L'\0');
  environment.push_back(L'\0');
  // Job limits stay at their defaults (ActiveProcessLimit = 1, every UI restriction; ruling I2).
  const win_launch::WinLaunchSpec spec{path_from_utf8(argv.front()).wstring(),
                                       quote_command_line(argv),
                                       std::move(environment),
                                       options_.cwd.wstring(),
                                       child_in.get(),
                                       child_out.get(),
                                       null_device.get(),
                                       token.get(),
                                       std::move(extra),
                                       std::move(block)};
  auto launched = win_launch::launch_in_job(spec);
  child_in.reset();
  child_out.reset();
  null_device.reset();
  record.reset();
  if (const auto* code = std::get_if<DWORD>(&launched)) {
    // classify_spawn_report is POSIX-only (ruling): the DWORD is mapped here.
    output.failure = AcquireError{*code == ERROR_FILE_NOT_FOUND || *code == ERROR_PATH_NOT_FOUND ? AcquireReason::downloader_missing
                                  : *code == ERROR_ACCESS_DENIED                                ? AcquireReason::downloader_untrusted_path
                                                                                                : AcquireReason::downloader_unsupported,
                                  "pull.downloader could not be started in its Job"};
    return output;
  }
  const auto& child = std::get<win_launch::WinLaunched>(launched);
  const Handle process(child.process);
  const Handle thread(child.thread);  // ruling I2: a real handle, closed here
  const Handle job(child.job);        // closing it kills anything left (KILL_ON_JOB_CLOSE)
  const auto interrupted = [this] { return options_.interrupted != nullptr && options_.interrupted->load(); };
  const auto kill = [&output, &job] {
    TerminateJobObject(job.get(), 1);
    output.killed = true;
  };
  std::size_t written = 0;
  while (written < stdin_bytes.size()) {
    DWORD n = 0;
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(stdin_bytes.size() - written, 1U << 20U));
    if (!WriteFile(parent_in.get(), stdin_bytes.data() + written, chunk, &n, nullptr) || n == 0) break;
    written += n;
  }
  parent_in.reset();  // the probe's stdin is empty and closed at once, so CRT entries never shift
  char buffer[65536];
  while (true) {
    if (interrupted()) {
      kill();
      break;
    }
    DWORD available = 0;
    if (!PeekNamedPipe(parent_out.get(), nullptr, 0, nullptr, &available, nullptr)) break;  // writer closed: EOF
    if (available == 0) {
      if (WaitForSingleObject(process.get(), 50) != WAIT_OBJECT_0) continue;
      if (!PeekNamedPipe(parent_out.get(), nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
    }
    DWORD n = 0;
    if (!ReadFile(parent_out.get(), buffer, sizeof buffer, &n, nullptr) || n == 0) break;
    if (!on_bytes(std::string_view{buffer, n})) {
      kill();
      break;
    }
  }
  while (WaitForSingleObject(process.get(), 250) == WAIT_TIMEOUT) {
    if (!output.killed && interrupted()) kill();
  }
  DWORD code = 0;
  if (!output.killed && GetExitCodeProcess(process.get(), &code)) output.exit = static_cast<int>(code);
  return output;
}

}  // namespace secure_frontend
#endif
