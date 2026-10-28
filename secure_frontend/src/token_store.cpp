#include "secure_frontend/token_store.hpp"

#include "secure_frontend/token_generator.hpp"
#include "secure_frontend/token_reader.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#if defined(_WIN32)
#include "windows_security.hpp"

#include <wincrypt.h>
#include <dpapi.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace secure_frontend {
namespace {

TokenStoreError fail(const TokenStoreErrorCode code, const char* message) {
  return TokenStoreError{code, message};
}

std::string random_suffix() {
  unsigned char nonce[8] = {};
  if (!fill_random(nonce, sizeof(nonce))) return {};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (const unsigned char b : nonce) { out.push_back(kHex[b >> 4U]); out.push_back(kHex[b & 15U]); }
  return out;
}

#if !defined(_WIN32)
int publish_without_replacing(const int dir, const char* temp, const char* name) {
#if defined(RENAME_NOREPLACE)
  if (::renameat2(dir, temp, dir, name, RENAME_NOREPLACE) == 0) return 0;
  if (errno != ENOSYS && errno != EINVAL) return -1;
#endif
  if (::linkat(dir, temp, dir, name, 0) != 0) return -1;  // EEXIST if the name is taken
  ::unlinkat(dir, temp, 0);
  return 0;
}
#endif

}  // namespace

#if !defined(_WIN32)
std::optional<TokenStoreError> write_private_file(const CanonicalRoot& root, const RelativePath& file,
                                                  const std::string_view bytes, const TokenStoreMode mode) {
  const auto& parts = file.components();
  int dir = -1;
  if (parts.size() == 1U) {
    dir = ::openat(root.handle(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  } else {
    std::string parent;
    for (std::size_t i = 0; i + 1U < parts.size(); ++i) parent += (parent.empty() ? "" : "/") + parts[i];
    auto opened = open_beneath(root, std::get<RelativePath>(parse_relative_path(parent)), OpenOptions{.kind = OpenKind::directory});
    if (std::holds_alternative<PathError>(opened)) return fail(TokenStoreErrorCode::unsafe_location, "the directory for the file cannot be opened safely");
    dir = ::fcntl(std::get<OpenedFile>(opened).native(), F_DUPFD_CLOEXEC, 0);
  }
  if (dir < 0) return fail(TokenStoreErrorCode::write_failed, "the directory for the file cannot be opened");
  OwnedHandle dir_guard(dir);
  struct stat ds {};
  if (::fstat(dir, &ds) != 0 || ds.st_uid != ::geteuid() || (ds.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    return fail(TokenStoreErrorCode::unsafe_location, "the directory holding the file must be owned by you with mode 0700");
  }
  const std::string& name = parts.back();
  struct stat existing {};
  if (::fstatat(dir, name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
    if (!S_ISREG(existing.st_mode)) return fail(TokenStoreErrorCode::unsafe_location, "the target exists and is not a regular file");
    if (mode == TokenStoreMode::create_new) return fail(TokenStoreErrorCode::already_exists, "the file already exists");
  } else if (errno != ENOENT) {
    // Only "absent" lets the write go ahead; any other failure fails closed.
    return fail(TokenStoreErrorCode::unsafe_location, "the target cannot be inspected");
  }
  const std::string suffix = random_suffix();
  if (suffix.empty()) return fail(TokenStoreErrorCode::write_failed, "no randomness for the temporary file name");
  const std::string temp = "." + name + ".tmp-" + suffix;
  const int fd = ::openat(dir, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return fail(TokenStoreErrorCode::write_failed, "the temporary file cannot be created");
  bool ok = ::fchmod(fd, 0600) == 0;
  std::size_t written = 0U;
  while (ok && written < bytes.size()) {
    const ssize_t count = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) ok = false;
    else written += static_cast<std::size_t>(count);
  }
  ok = ok && ::fsync(fd) == 0;
  ok = ::close(fd) == 0 && ok;
  if (!ok) {
    ::unlinkat(dir, temp.c_str(), 0);
    return fail(TokenStoreErrorCode::write_failed, "the file could not be written");
  }
  const int published = mode == TokenStoreMode::create_new ? publish_without_replacing(dir, temp.c_str(), name.c_str())
                                                           : ::renameat(dir, temp.c_str(), dir, name.c_str());
  const int saved = errno;
  if (published != 0) {
    ::unlinkat(dir, temp.c_str(), 0);
    return fail(saved == EEXIST ? TokenStoreErrorCode::already_exists : TokenStoreErrorCode::write_failed,
                "the file could not be put in place");
  }
  (void)::fsync(dir);
  return std::nullopt;
}

std::optional<TokenStoreError> store_token(const CanonicalRoot& secret_root, const RelativePath& file,
                                           const SecretBytes& token, const TokenStoreMode mode) {
  SecretBytes content;
  if (!content.assign(token.view()) || !content.append("\n")) return fail(TokenStoreErrorCode::write_failed, "the token is too long");
  return write_private_file(secret_root, file, content.view(), mode);
}
#endif

#if defined(_WIN32)
namespace ws = windows_security;

namespace {

bool same_file(const HANDLE a, const HANDLE b) {
  BY_HANDLE_FILE_INFORMATION x{};
  BY_HANDLE_FILE_INFORMATION y{};
  return GetFileInformationByHandle(a, &x) != 0 && GetFileInformationByHandle(b, &y) != 0 &&
         x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh &&
         x.nFileIndexLow == y.nFileIndexLow;
}

bool same_path(const std::wstring& a, const std::wstring& b) {
  return !a.empty() && a.size() == b.size() &&
         CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

}  // namespace

std::optional<TokenStoreError> write_private_file(const CanonicalRoot& root, const RelativePath& file,
                                                  const std::string_view bytes, const TokenStoreMode mode) {
  // The holding directory is validated as on Linux: the root itself, or the
  // parent opened with open_beneath (no reparse point on the way, final path
  // inside the root). The creation path is the validated root + relative path.
  const auto& parts = file.components();
  std::wstring directory = root.path().wstring();
  std::optional<OpenedFile> validated;
  if (parts.size() > 1U) {
    std::string parent;
    for (std::size_t i = 0; i + 1U < parts.size(); ++i) {
      parent += (parent.empty() ? "" : "/") + parts[i];
      directory += L'\\' + path_from_utf8(parts[i]).wstring();
    }
    auto opened = open_beneath(root, std::get<RelativePath>(parse_relative_path(parent)), OpenOptions{.kind = OpenKind::directory});
    if (std::holds_alternative<PathError>(opened)) return fail(TokenStoreErrorCode::unsafe_location, "the directory for the file cannot be opened safely");
    validated.emplace(std::move(std::get<OpenedFile>(opened)));
  }
  const HANDLE checked = validated ? validated->native() : root.handle();
  // Pin: a handle without FILE_SHARE_DELETE, held across create and move, so the
  // directory cannot be renamed or replaced. It must be the validated directory.
  // (open_beneath's own handle shares only FILE_SHARE_READ, which could refuse the
  // rename's write-access open of the target directory, so it is not the pin.)
  HANDLE dir = CreateFileW(ws::extended_path(directory).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (dir == INVALID_HANDLE_VALUE) return fail(TokenStoreErrorCode::write_failed, "the directory for the file cannot be opened");
  OwnedHandle dir_guard(dir);
  if (ws::is_reparse_point(dir) || !same_file(dir, checked)) {
    return fail(TokenStoreErrorCode::unsafe_location, "the directory for the file changed while it was opened");
  }
  validated.reset();
  const std::wstring pinned_path = ws::final_path_for_handle(dir);
  std::array<std::byte, SECURITY_MAX_SID_SIZE> storage{};
  PSID user = nullptr;
  if (pinned_path.empty() || !ws::current_user_sid(storage, &user) || !ws::has_private_owner_dacl(dir, user)) {
    return fail(TokenStoreErrorCode::unsafe_location, "the directory holding the file must have a protected owner-only ACL");
  }
  const std::wstring target = directory + L'\\' + path_from_utf8(parts.back()).wstring();
  const DWORD attributes = GetFileAttributesW(ws::extended_path(target).c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    if ((attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0U) {
      return fail(TokenStoreErrorCode::unsafe_location, "the target exists and is not a regular file");
    }
    if (mode == TokenStoreMode::create_new) return fail(TokenStoreErrorCode::already_exists, "the file already exists");
  } else if (const DWORD code = GetLastError(); code != ERROR_FILE_NOT_FOUND) {
    // As on Linux: only "absent" lets the write go ahead; any other failure fails closed.
    return fail(TokenStoreErrorCode::unsafe_location, "the target cannot be inspected");
  }
  const std::string suffix = random_suffix();
  if (suffix.empty()) return fail(TokenStoreErrorCode::write_failed, "no randomness for the temporary file name");
  const std::wstring temp = directory + L"\\." + path_from_utf8(parts.back()).wstring() + L".tmp-" + path_from_utf8(suffix).wstring();
  ws::PrivateSecurityAttributes security(false);
  if (security.get() == nullptr) return fail(TokenStoreErrorCode::protect_failed, "a private security descriptor cannot be built");
  HANDLE out = CreateFileW(ws::extended_path(temp).c_str(), GENERIC_WRITE | DELETE, 0, security.get(), CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out == INVALID_HANDLE_VALUE) return fail(TokenStoreErrorCode::write_failed, "the temporary file cannot be created");
  // The temp must have landed in the pinned directory (an ancestor swapped
  // between the checks and the path-based create would put it elsewhere).
  if (!same_path(std::filesystem::path{ws::final_path_for_handle(out)}.parent_path().wstring(), pinned_path)) {
    FILE_DISPOSITION_INFO dispose{TRUE};
    SetFileInformationByHandle(out, FileDispositionInfo, &dispose, sizeof(dispose));
    CloseHandle(out);
    return fail(TokenStoreErrorCode::unsafe_location, "the temporary file was not created in the checked directory");
  }
  DWORD written = 0;
  bool ok = bytes.empty() || (WriteFile(out, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0 &&
                              written == bytes.size());
  ok = FlushFileBuffers(out) != 0 && ok;
  CloseHandle(out);
  if (!ok) {
    DeleteFileW(ws::extended_path(temp).c_str());
    return fail(TokenStoreErrorCode::write_failed, "the file could not be written");
  }
  const DWORD flags = MOVEFILE_WRITE_THROUGH | (mode == TokenStoreMode::replace ? MOVEFILE_REPLACE_EXISTING : 0U);
  if (MoveFileExW(ws::extended_path(temp).c_str(), ws::extended_path(target).c_str(), flags) == 0) {
    const DWORD code = GetLastError();
    DeleteFileW(ws::extended_path(temp).c_str());
    return fail(code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS ? TokenStoreErrorCode::already_exists
                                                                          : TokenStoreErrorCode::write_failed,
                "the file could not be put in place");
  }
  return std::nullopt;
}

std::optional<TokenStoreError> store_token(const CanonicalRoot& secret_root, const RelativePath& file,
                                           const SecretBytes& token, const TokenStoreMode mode) {
  DATA_BLOB input{static_cast<DWORD>(token.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(token.view().data()))};
  DATA_BLOB output{};
  if (CryptProtectData(&input, L"strata token", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output) == 0) {
    return fail(TokenStoreErrorCode::protect_failed, "DPAPI could not protect the token");
  }
  std::string content{kDpapiTokenMagic};
  content.append(reinterpret_cast<const char*>(output.pbData), output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  return write_private_file(secret_root, file, content, mode);
}
#endif

}  // namespace secure_frontend
