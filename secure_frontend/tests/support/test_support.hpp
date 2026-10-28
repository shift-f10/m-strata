#pragma once
// Shared by every secure_frontend test binary (SP1 §3.10): a framework-free
// case runner -- `<binary> --case <name> [args...]` runs one case, no
// arguments runs all, `--list` prints the names -- plus owner-only scratch
// directories below the build tree.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <aclapi.h>
#include <sddl.h>
#include <winioctl.h>
#include <cstdint>
#include <stdexcept>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace secure_frontend_test {

struct Case final {
  std::string_view name;
  std::function<void()> run;
};

// Arguments after `--case <name>` (the ARGS of secure_frontend_add_cases).
inline std::vector<std::string>& extra_arguments() {
  static std::vector<std::string> arguments;
  return arguments;
}

inline std::vector<std::filesystem::path>& scratch_registry() {
  static std::vector<std::filesystem::path> directories;
  return directories;
}

#if defined(_WIN32)
// The current process user's SID as text ("S-1-5-21-..."), or empty on failure.
// Test-side twin of windows_security::current_user_sid_string (test_support is
// header-only and not every test binary links secure_frontend_paths).
inline std::wstring current_user_sid_string() {
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) return {};
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> buffer(size);
  const BOOL ok = size != 0 ? GetTokenInformation(token, TokenUser, buffer.data(), size, &size) : FALSE;
  CloseHandle(token);
  if (ok == 0) return {};
  LPWSTR text = nullptr;
  if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text) == 0) return {};
  std::wstring out{text};
  LocalFree(text);
  return out;
}

// CreateDirectoryW with an explicit, protected DACL: full control for the
// current user, LocalSystem and Administrators, nothing inherited. %TEMP% is
// not trusted to be private -- on GitHub's setup-msys2 runners TEMP lives under
// D:\a\_temp, whose ACL can grant Authenticated Users write, and an inherited
// ACE like that would make every open_root(scratch) report shared_writable.
// Returns ERROR_SUCCESS, or the GetLastError() value captured immediately after
// the failing call (before any LocalFree/CloseHandle can overwrite it).
inline DWORD create_private_directory_w(const std::wstring& path) {
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) return GetLastError();
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> buffer(size);
  const bool have_user = size != 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != 0;
  const DWORD token_error = have_user ? ERROR_SUCCESS : GetLastError();
  CloseHandle(token);
  if (!have_user) return token_error != ERROR_SUCCESS ? token_error : ERROR_INVALID_DATA;
  wchar_t* sid_text = nullptr;
  if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid_text) == 0) {
    return GetLastError();
  }
  // D:P = protected DACL; OICI = inherited by files and subdirectories; FA = full access.
  const std::wstring sddl = std::wstring{L"D:P(A;OICI;FA;;;"} + sid_text + L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
  LocalFree(sid_text);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) == 0) {
    return GetLastError();
  }
  SECURITY_ATTRIBUTES attributes{static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES)), descriptor, FALSE};
  const DWORD error = CreateDirectoryW(path.c_str(), &attributes) != 0 ? ERROR_SUCCESS : GetLastError();
  LocalFree(descriptor);
  return error;
}
#endif

// A fresh, empty directory only the current user can use. POSIX: mkdtemp
// (mode 0700) below $SECURE_FRONTEND_TEST_TMP, which secure_frontend_add_cases
// points at <build>/test-tmp. Windows: below %TEMP%, created with an explicit
// protected DACL (user, SYSTEM, Administrators) instead of inheriting %TEMP%'s.
inline std::filesystem::path make_scratch_directory() {
#if defined(_WIN32)
  const std::filesystem::path base = std::filesystem::temp_directory_path();
  static unsigned counter = 0;
  DWORD last_error = ERROR_SUCCESS;
  for (int attempt = 0; attempt < 1000; ++attempt) {
    const auto candidate = base / ("sf-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++counter) +
                                   "-" + std::to_string(GetTickCount64()));
    last_error = create_private_directory_w(L"\\\\?\\" + std::filesystem::absolute(candidate).wstring());
    if (last_error == ERROR_SUCCESS) {
      scratch_registry().push_back(candidate);
      return candidate;
    }
  }
  std::cerr << "unable to create a scratch directory below " << base.string() << ": error " << last_error << '\n';
  std::exit(EXIT_FAILURE);
#else
  const char* configured = std::getenv("SECURE_FRONTEND_TEST_TMP");
  const std::filesystem::path base = configured != nullptr && *configured != '\0'
                                         ? std::filesystem::path{configured}
                                         : std::filesystem::temp_directory_path();
  std::error_code ignored;
  std::filesystem::create_directories(base, ignored);
  std::string pattern = (base / "sf-XXXXXX").string();
  if (mkdtemp(pattern.data()) == nullptr) {
    std::cerr << "mkdtemp failed below " << base.string() << ": " << std::strerror(errno) << '\n';
    std::exit(EXIT_FAILURE);
  }
  scratch_registry().emplace_back(pattern);
  return std::filesystem::path{pattern};
#endif
}

// mkdir with mode 0700 (the umask cannot widen it), or CreateDirectoryW with
// the same explicit protected DACL as make_scratch_directory (never inherited).
inline void make_private_directory(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::wstring extended = L"\\\\?\\" + std::filesystem::absolute(path).wstring();
  const DWORD error = create_private_directory_w(extended);
  if (error != ERROR_SUCCESS && error != ERROR_ALREADY_EXISTS) {
    std::cerr << "CreateDirectoryW failed: " << path.string() << ": error " << error << '\n';
    std::exit(EXIT_FAILURE);
  }
#else
  if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
    std::cerr << "mkdir failed: " << path.string() << ": " << std::strerror(errno) << '\n';
    std::exit(EXIT_FAILURE);
  }
#endif
}

// Writes bytes (replacing the file) and, on POSIX, sets `mode` exactly.
inline void write_file(const std::filesystem::path& path, const std::string_view bytes, const unsigned mode = 0600) {
#if defined(_WIN32)
  (void)mode;
  const std::wstring extended = L"\\\\?\\" + std::filesystem::absolute(path).wstring();
  HANDLE file = CreateFileW(extended.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    std::cerr << "CreateFileW failed: " << path.string() << ": error " << error << '\n';
    std::exit(EXIT_FAILURE);
  }
  DWORD written = 0;
  const bool ok = bytes.empty() || (WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0 &&
                                    written == bytes.size());
  const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  if (!ok) {
    std::cerr << "WriteFile failed: " << path.string() << ": error " << error << '\n';
    std::exit(EXIT_FAILURE);
  }
#else
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  if (chmod(path.c_str(), static_cast<mode_t>(mode)) != 0) {
    std::cerr << "chmod failed: " << path.string() << ": " << std::strerror(errno) << '\n';
    std::exit(EXIT_FAILURE);
  }
#endif
}

#if defined(_WIN32)
// O:<user>D:P(A;[OICI];FA;;;<user>)(A;[OICI];FA;;;SY): the owner is the user (not
// the token's default owner, which is Administrators for an elevated admin), so
// has_private_owner_dacl accepts it. Same SDDL as windows_security's
// PrivateSecurityAttributes, kept test-side so the helper needs no product code.
struct ProtectedAttributes final {
  PSECURITY_DESCRIPTOR descriptor{nullptr};
  SECURITY_ATTRIBUTES attributes{};
  explicit ProtectedAttributes(const bool directory) {
    const std::wstring sid = current_user_sid_string();
    if (sid.empty()) throw std::runtime_error("the current user's SID is unavailable");
    const std::wstring inherit = directory ? L"OICI" : L"";
    const std::wstring sddl = L"O:" + sid + L"D:P(A;" + inherit + L";FA;;;" + sid + L")(A;" + inherit + L";FA;;;SY)";
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) == 0) {
      throw std::runtime_error("the protected security descriptor could not be built: error " +
                               std::to_string(GetLastError()));
    }
    attributes = SECURITY_ATTRIBUTES{static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES)), descriptor, FALSE};
  }
  ~ProtectedAttributes() { if (descriptor != nullptr) LocalFree(descriptor); }
  ProtectedAttributes(const ProtectedAttributes&) = delete;
  ProtectedAttributes& operator=(const ProtectedAttributes&) = delete;
};

inline void make_protected_directory(const std::filesystem::path& path) {
  ProtectedAttributes sa(true);
  const std::wstring extended = L"\\\\?\\" + std::filesystem::absolute(path).wstring();
  if (CreateDirectoryW(extended.c_str(), &sa.attributes) == 0) {
    throw std::runtime_error("protected mkdir failed: error " + std::to_string(GetLastError()));
  }
}

inline void write_protected_file(const std::filesystem::path& path, const std::string_view bytes) {
  ProtectedAttributes sa(false);
  const std::wstring extended = L"\\\\?\\" + std::filesystem::absolute(path).wstring();
  HANDLE file = CreateFileW(extended.c_str(), GENERIC_WRITE, 0, &sa.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("protected file create failed: error " + std::to_string(GetLastError()));
  }
  DWORD written = 0;
  const bool ok = bytes.empty() || (WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0 &&
                                    written == bytes.size());
  const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  if (!ok) throw std::runtime_error("protected file write failed: error " + std::to_string(error));
}

// Adds an explicit Everyone:FILE_GENERIC_WRITE ACE to `path`'s DACL.
inline bool grant_everyone_write(const std::filesystem::path& path) {
  PACL old_dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl, nullptr,
                            &descriptor) != ERROR_SUCCESS) return false;
  DWORD sid_size = SECURITY_MAX_SID_SIZE;
  std::vector<unsigned char> world(SECURITY_MAX_SID_SIZE);
  CreateWellKnownSid(WinWorldSid, nullptr, world.data(), &sid_size);
  EXPLICIT_ACCESS_W access{};
  access.grfAccessPermissions = FILE_GENERIC_WRITE;
  access.grfAccessMode = GRANT_ACCESS;
  access.grfInheritance = NO_INHERITANCE;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
  access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(world.data());
  PACL updated = nullptr;
  const bool ok = SetEntriesInAclW(1, &access, old_dacl, &updated) == ERROR_SUCCESS &&
                  SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                        nullptr, nullptr, updated, nullptr) == ERROR_SUCCESS;
  if (updated != nullptr) LocalFree(updated);
  LocalFree(descriptor);
  return ok;
}

// A directory junction (mount point) at `link` pointing at `target`; needs no privilege.
inline bool create_junction(const std::filesystem::path& link, const std::filesystem::path& target) {
  if (CreateDirectoryW(link.c_str(), nullptr) == 0) return false;
  HANDLE directory = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (directory == INVALID_HANDLE_VALUE) return false;
  const std::wstring print = std::filesystem::absolute(target).wstring();
  const std::wstring substitute = L"\\??\\" + print;
  const std::size_t path_bytes = (substitute.size() + 1U + print.size() + 1U) * sizeof(wchar_t);
  std::vector<unsigned char> buffer(16U + path_bytes, 0U);
  const auto put16 = [&buffer](const std::size_t offset, const std::uint16_t value) { std::memcpy(buffer.data() + offset, &value, 2U); };
  const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
  std::memcpy(buffer.data(), &tag, 4U);
  put16(4U, static_cast<std::uint16_t>(8U + path_bytes));                        // ReparseDataLength
  put16(8U, 0U);                                                                // SubstituteNameOffset
  put16(10U, static_cast<std::uint16_t>(substitute.size() * sizeof(wchar_t)));  // SubstituteNameLength
  put16(12U, static_cast<std::uint16_t>((substitute.size() + 1U) * sizeof(wchar_t)));  // PrintNameOffset
  put16(14U, static_cast<std::uint16_t>(print.size() * sizeof(wchar_t)));       // PrintNameLength
  std::memcpy(buffer.data() + 16U, substitute.c_str(), (substitute.size() + 1U) * sizeof(wchar_t));
  std::memcpy(buffer.data() + 16U + (substitute.size() + 1U) * sizeof(wchar_t), print.c_str(), (print.size() + 1U) * sizeof(wchar_t));
  DWORD returned = 0;
  const BOOL ok = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
                                  nullptr, 0, &returned, nullptr);
  CloseHandle(directory);
  return ok != 0;
}
#endif

// A --state-dir root: what serve's owner-only check (is_private_state_root)
// accepts. Windows: make_protected_directory's descriptor (explicit O:<user>,
// protected DACL for the user and SYSTEM) -- make_private_directory leaves the
// owner to the token default, which is Administrators for an elevated admin.
// An existing directory is accepted, as make_private_directory does. POSIX:
// make_private_directory (0700).
inline void make_owner_private_directory(const std::filesystem::path& path) {
#if defined(_WIN32)
  ProtectedAttributes sa(true);
  const std::wstring extended = L"\\\\?\\" + std::filesystem::absolute(path).wstring();
  if (CreateDirectoryW(extended.c_str(), &sa.attributes) == 0) {
    const DWORD error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS) {
      std::cerr << "owner-private CreateDirectoryW failed: " << path.string() << ": error " << error << '\n';
      std::exit(EXIT_FAILURE);
    }
  }
#else
  make_private_directory(path);
#endif
}

inline std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline void remove_scratch_directories() {
  for (const auto& directory : scratch_registry()) {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  scratch_registry().clear();
}

// `failures` is the calling file's own counter (files whose expect() throws pass a zero).
inline int run_cases(const int argc, char** argv, const std::initializer_list<Case> cases, const int& failure_count) {
  // Cleanup also runs on std::exit() from a helper, so a setup failure cannot
  // leave scratch directories behind (a no-op after the normal-path cleanup).
  // The registry is constructed first so it outlives the atexit handler.
  scratch_registry();
  static const bool cleanup_registered = std::atexit(remove_scratch_directories) == 0;
  (void)cleanup_registered;
  std::vector<const Case*> selected;
  if (argc >= 2 && std::string_view{argv[1]} == "--list") {
    for (const auto& c : cases) std::cout << c.name << '\n';
    return EXIT_SUCCESS;
  }
  if (argc >= 3 && std::string_view{argv[1]} == "--case") {
    for (int index = 3; index < argc; ++index) extra_arguments().emplace_back(argv[index]);
    for (const auto& c : cases) if (c.name == std::string_view{argv[2]}) selected.push_back(&c);
    if (selected.empty()) { std::cerr << "unknown case: " << argv[2] << '\n'; return EXIT_FAILURE; }
  } else if (argc == 1) {
    for (const auto& c : cases) selected.push_back(&c);
  } else {
    std::cerr << "usage: " << argv[0] << " [--list | --case <name> [args...]]\n";
    return EXIT_FAILURE;
  }
  for (const Case* c : selected) {
    try {
      c->run();
    } catch (const std::exception& error) {
      std::cerr << "FAILED: " << c->name << ": " << error.what() << '\n';
      remove_scratch_directories();
      return EXIT_FAILURE;
    } catch (...) {
      std::cerr << "FAILED: " << c->name << ": unknown exception\n";
      remove_scratch_directories();
      return EXIT_FAILURE;
    }
  }
  remove_scratch_directories();
  return failure_count == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace secure_frontend_test
