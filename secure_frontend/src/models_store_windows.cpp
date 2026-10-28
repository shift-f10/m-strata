#include "secure_frontend/models_store.hpp"

#if defined(_WIN32)
#include "secure_frontend/model_index_model.hpp"
#include "secure_frontend/win32_prelude.hpp"
#include "windows_security.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace secure_frontend {
namespace {

namespace ws = windows_security;

AcquireError win_error(const DWORD code, const std::string& detail) {
  return AcquireError{code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL ? AcquireReason::disk_space
                                                                                 : AcquireReason::publish_failed,
                      detail};
}

std::wstring native(const CanonicalRoot& root, const RelativePath& path) { return ws::extended_path(root.join(path).wstring()); }

bool same_file(const HANDLE a, const HANDLE b) {
  BY_HANDLE_FILE_INFORMATION x{};
  BY_HANDLE_FILE_INFORMATION y{};
  return GetFileInformationByHandle(a, &x) != 0 && GetFileInformationByHandle(b, &y) != 0 &&
         x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
}

// The validated, pinned parent directory of `path` (handle without FILE_SHARE_DELETE,
// held across the path-based operation, as SP1's write_private_file does).
class PinnedParent final {
 public:
  PinnedParent(const CanonicalRoot& root, const RelativePath& path) {
    const auto& parts = path.components();
    std::optional<OpenedFile> validated;
    std::wstring directory = root.path().wstring();
    if (parts.size() > 1U) {
      std::string parent;
      for (std::size_t i = 0; i + 1U < parts.size(); ++i) {
        parent += (parent.empty() ? "" : "/") + parts[i];
        directory += L'\\' + path_from_utf8(parts[i]).wstring();
      }
      auto opened = open_beneath(root, must_rel(parent), OpenOptions{OpenKind::directory});  // a prefix of a valid path
      if (const auto* failure = std::get_if<PathError>(&opened)) {
        absent_ = failure->code == PathErrorCode::not_found;
        return;
      }
      validated.emplace(std::move(std::get<OpenedFile>(opened)));
    }
    handle_ = CreateFileW(ws::extended_path(directory).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      const DWORD code = GetLastError();
      absent_ = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
      return;
    }
    const HANDLE checked = validated ? static_cast<HANDLE>(validated->native()) : static_cast<HANDLE>(root.handle());
    if (ws::is_reparse_point(handle_) || !same_file(handle_, checked)) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      return;
    }
    pinned_ = ws::final_path_for_handle(handle_);
  }
  ~PinnedParent() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  }
  PinnedParent(const PinnedParent&) = delete;
  PinnedParent& operator=(const PinnedParent&) = delete;
  [[nodiscard]] bool ok() const noexcept { return handle_ != INVALID_HANDLE_VALUE && !pinned_.empty(); }
  // Not ok() because the parent does not exist (as opposed to a link, a mismatch or a denial).
  [[nodiscard]] bool absent() const noexcept { return absent_; }
  [[nodiscard]] const std::wstring& final_path() const noexcept { return pinned_; }

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
  std::wstring pinned_;
  bool absent_{false};
};

bool same_path(const std::wstring& a, const std::wstring& b) {
  return !a.empty() && a.size() == b.size() &&
         CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

// Spec §11: antivirus/indexer handles → retry 10 times over 5 s.
template <typename Operation>
bool with_sharing_retries(Operation operation) {
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (operation()) return true;
    const DWORD code = GetLastError();
    if (code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    SetLastError(code);
  }
  return false;
}

// Parity with POSIX's st_uid == geteuid() checks (SP1 token_store precedent): an existing
// .lock or .part is adopted only when the user owns it under a protected owner-only DACL.
bool private_to_user(const HANDLE handle) {
  std::array<std::byte, SECURITY_MAX_SID_SIZE> storage{};
  PSID user = nullptr;
  return ws::current_user_sid(storage, &user) && ws::has_private_owner_dacl(handle, user);
}

bool absent(const DWORD code) { return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND; }

// A read-only directory (FRFX DACL, e.g. from make_read_only_directory) blocks deleting its
// entries. Put back the private owner-only DACL on a handle that is checked to be the
// directory open_beneath validated (never a reparse point); the owner always holds
// WRITE_DAC. Best effort: a failure surfaces as the removal error that follows.
void restore_owner_access(const CanonicalRoot& root, const RelativePath& path) {
  auto validated = open_beneath(root, path, OpenOptions{OpenKind::directory});
  if (std::holds_alternative<PathError>(validated)) return;
  const HANDLE h = CreateFileW(native(root, path).c_str(), WRITE_DAC | READ_CONTROL | FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  OwnedHandle guard(h);
  if (ws::is_reparse_point(h) || !same_file(h, static_cast<HANDLE>(std::get<OpenedFile>(validated).native()))) return;
  const std::wstring sddl = L"D:P(A;OICI;FA;;;" + ws::current_user_sid_string() + L")(A;OICI;FA;;;SY)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return;
  BOOL present = FALSE;
  BOOL defaulted = FALSE;
  PACL dacl = nullptr;
  if (GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present) {
    (void)SetSecurityInfo(h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl,
                          nullptr);
  }
  LocalFree(descriptor);
}

}  // namespace

std::variant<std::uint64_t, AcquireError> StoreEnvironment::free_bytes(const CanonicalRoot& root) const {
  ULARGE_INTEGER available{};
  if (!GetDiskFreeSpaceExW((root.path().wstring() + L"\\").c_str(), &available, nullptr, nullptr)) {
    return AcquireError{AcquireReason::disk_space, "free space cannot be determined"};
  }
  return static_cast<std::uint64_t>(available.QuadPart);
}

std::variant<FileSystemKind, AcquireError> StoreEnvironment::file_system_kind(const CanonicalRoot& root) const {
  wchar_t name[MAX_PATH + 1] = {};
  if (!GetVolumeInformationByHandleW(static_cast<HANDLE>(root.handle()), nullptr, 0, nullptr, nullptr, nullptr, name, MAX_PATH + 1)) {
    return AcquireError{AcquireReason::bundle_layout, "file system type cannot be determined"};
  }
  const std::wstring fs{name};
  return fs == L"FAT" || fs == L"FAT32" ? FileSystemKind::fat : FileSystemKind::other;
}

ModelsLock& ModelsLock::operator=(ModelsLock&& other) noexcept {
  if (this != &other) {
    if (owned_) CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = other.handle_;
    owned_ = other.owned_;
    other.owned_ = false;
  }
  return *this;
}

ModelsLock::~ModelsLock() {
  if (owned_) CloseHandle(static_cast<HANDLE>(handle_));  // closing the handle releases LockFileEx
}

std::variant<ModelsLock, AcquireError> lock_models_root(const CanonicalRoot& root) {
  const std::string lock_name{kLockFileName};
  ws::PrivateSecurityAttributes security(false);
  const HANDLE h = CreateFileW(native(root, must_rel(lock_name)).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               security.get(), OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (h == INVALID_HANDLE_VALUE) return AcquireError{AcquireReason::publish_failed, lock_name + " cannot be opened"};
  BY_HANDLE_FILE_INFORMATION lock_info{};
  if (!GetFileInformationByHandle(h, &lock_info) ||
      (lock_info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 || !private_to_user(h)) {
    CloseHandle(h);
    return AcquireError{AcquireReason::publish_failed, lock_name + " is not a private regular file owned by you"};
  }
  OVERLAPPED overlapped{};
  if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &overlapped)) {
    const DWORD code = GetLastError();
    CloseHandle(h);
    return AcquireError{code == ERROR_LOCK_VIOLATION ? AcquireReason::models_busy : AcquireReason::publish_failed,
                        "another pull, import, rm or update holds roots.models/" + lock_name};
  }
  return ModelsLock{h};
}

PartFile::PartFile(PartFile&& other) noexcept
    : handle_(other.handle_), size_(other.size_), path_(std::move(other.path_)), fail_after_(other.fail_after_), open_(other.open_) {
  other.open_ = false;
}

PartFile::~PartFile() { close(); }

void PartFile::close() noexcept {
  if (open_) CloseHandle(static_cast<HANDLE>(handle_));
  open_ = false;
}

std::optional<AcquireError> PartFile::append(const std::string_view bytes) {
  if (fail_after_ && size_ + bytes.size() > *fail_after_) return AcquireError{AcquireReason::disk_space, path_};
  std::size_t done = 0;
  while (done < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, std::size_t{1} << 30U));
    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(handle_), bytes.data() + done, chunk, &written, nullptr) || written == 0) {
      return win_error(GetLastError(), path_);
    }
    done += written;
    size_ += written;
  }
  return std::nullopt;
}

std::optional<AcquireError> PartFile::write_at(const std::uint64_t offset, const std::string_view bytes) {
  if (fail_after_ && offset + bytes.size() > *fail_after_) return AcquireError{AcquireReason::disk_space, path_};
  std::size_t done = 0;
  while (done < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, std::size_t{1} << 30U));
    const std::uint64_t at = offset + done;
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFULL);
    ov.OffsetHigh = static_cast<DWORD>(at >> 32U);
    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(handle_), bytes.data() + done, chunk, &written, &ov) || written == 0) {
      return win_error(GetLastError(), path_);
    }
    done += written;
  }
  return std::nullopt;
}

std::optional<AcquireError> PartFile::truncate_to_zero() {
  LARGE_INTEGER zero{};
  if (!SetFilePointerEx(static_cast<HANDLE>(handle_), zero, nullptr, FILE_BEGIN) || !SetEndOfFile(static_cast<HANDLE>(handle_))) {
    return win_error(GetLastError(), path_);
  }
  size_ = 0;
  return std::nullopt;
}

std::optional<AcquireError> PartFile::preallocate(const std::uint64_t total_size) {
  FILE_ALLOCATION_INFO info{};
  info.AllocationSize.QuadPart = static_cast<LONGLONG>(total_size);
  if (!SetFileInformationByHandle(static_cast<HANDLE>(handle_), FileAllocationInfo, &info, sizeof info)) {
    const DWORD code = GetLastError();
    if (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL) return win_error(code, path_);
  }
  return std::nullopt;  // any other failure: preallocation is skipped, never the download
}

std::optional<AcquireError> PartFile::sync() {
  if (!FlushFileBuffers(static_cast<HANDLE>(handle_))) return win_error(GetLastError(), path_);
  return std::nullopt;
}

std::variant<PartFile, AcquireError> open_part_file(const CanonicalRoot& root, const RelativePath& path, const PartOpen mode,
                                                    const StoreEnvironment& env) {
  const PinnedParent parent(root, path);
  if (!parent.ok()) return AcquireError{AcquireReason::publish_failed, path.generic() + ": directory cannot be opened safely"};
  ws::PrivateSecurityAttributes security(false);
  const HANDLE h = CreateFileW(native(root, path).c_str(), GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, security.get(),
                               mode == PartOpen::create_new ? CREATE_NEW : OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (h == INVALID_HANDLE_VALUE) return win_error(GetLastError(), path.generic() + ": cannot be opened");
  BY_HANDLE_FILE_INFORMATION info{};
  const bool regular = GetFileInformationByHandle(h, &info) != 0 &&
                       (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0 &&
                       info.nNumberOfLinks == 1 && (root.external() || private_to_user(h));  // FAT/exFAT: no ACLs (§5.3)
  const bool inside = same_path(std::filesystem::path{ws::final_path_for_handle(h)}.parent_path().wstring(), parent.final_path());
  LARGE_INTEGER end{};
  LARGE_INTEGER zero{};
  if (!regular || !inside || !SetFilePointerEx(h, zero, &end, FILE_END)) {
    CloseHandle(h);
    return AcquireError{AcquireReason::publish_failed, path.generic() + ": not a private single-link regular file"};
  }
  return PartFile{h, static_cast<std::uint64_t>(end.QuadPart), path.generic(), env.fail_writes_after_bytes()};
}

std::optional<AcquireError> ensure_directory(const CanonicalRoot& root, const RelativePath& path) {
  if (const auto failure = create_directory_within(root, path)) {
    return AcquireError{AcquireReason::publish_failed, path.generic() + ": " + failure->message};
  }
  return std::nullopt;
}

std::optional<AcquireError> make_read_only_directory(const CanonicalRoot& root, const RelativePath& path) {
  if (auto failure = ensure_directory(root, path)) return failure;
  const std::wstring sddl = L"D:P(A;OICI;FRFX;;;" + ws::current_user_sid_string() + L")(A;OICI;FA;;;SY)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
    return AcquireError{AcquireReason::publish_failed, path.generic() + ": read-only DACL"};
  }
  const BOOL set = SetFileSecurityW(native(root, path).c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
  LocalFree(descriptor);
  if (!set) return AcquireError{AcquireReason::publish_failed, path.generic() + ": read-only DACL"};
  return std::nullopt;
}

std::optional<AcquireError> rename_within(const CanonicalRoot& root, const RelativePath& from, const RelativePath& to,
                                          const StoreEnvironment&) {
  const PinnedParent from_parent(root, from);
  const PinnedParent to_parent(root, to);
  if (!from_parent.ok() || !to_parent.ok()) return AcquireError{AcquireReason::publish_failed, to.generic() + ": directory cannot be opened"};
  const std::wstring source = native(root, from);
  const std::wstring target = native(root, to);
  // No MOVEFILE_REPLACE_EXISTING: an existing target is never replaced, so the
  // force_rename_fallback() seam has nothing to select on Windows.
  if (with_sharing_retries([&] { return MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0; })) return std::nullopt;
  const DWORD code = GetLastError();
  if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) return AcquireError{AcquireReason::publish_failed, to.generic() + ": target exists"};
  return win_error(code, to.generic());
}

std::optional<AcquireError> remove_file_within(const CanonicalRoot& root, const RelativePath& path) {
  const PinnedParent parent(root, path);
  if (!parent.ok()) {  // parent absent: nothing to remove; unreachable: an error, never "removed"
    if (parent.absent()) return std::nullopt;
    return AcquireError{AcquireReason::publish_failed, path.generic() + ": directory cannot be opened"};
  }
  const std::wstring target = native(root, path);
  if (with_sharing_retries([&] { return DeleteFileW(target.c_str()) != 0; })) return std::nullopt;
  const DWORD code = GetLastError();
  if (absent(code)) return std::nullopt;
  return win_error(code, path.generic());
}

std::optional<AcquireError> remove_tree_within(const CanonicalRoot& root, const RelativePath& path) {
  const std::wstring target = native(root, path);
  const DWORD attributes = GetFileAttributesW(target.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = GetLastError();
    return absent(code) ? std::nullopt : std::optional<AcquireError>{win_error(code, path.generic())};
  }
  const PinnedParent parent(root, path);
  if (!parent.ok()) return AcquireError{AcquireReason::publish_failed, path.generic() + ": directory cannot be opened"};
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {  // junction or link: remove the link itself, never its target
    const bool removed = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? RemoveDirectoryW(target.c_str()) != 0 : DeleteFileW(target.c_str()) != 0;
    return removed ? std::nullopt : std::optional<AcquireError>{win_error(GetLastError(), path.generic())};
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return remove_file_within(root, path);
  restore_owner_access(root, path);
  auto listed = list_directory(root, path);
  if (std::holds_alternative<PathError>(listed)) return AcquireError{AcquireReason::publish_failed, path.generic() + ": cannot be listed"};
  for (const auto& entry : std::get<std::vector<DirectoryEntry>>(listed)) {
    // Unlike POSIX (which unlinks such entries), an entry P1 rejects (e.g. an 8.3 alias) is
    // never something SP4 creates on Windows, so it fails closed.
    if (!std::holds_alternative<RelativePath>(parse_relative_path(entry.name))) {
      return AcquireError{AcquireReason::publish_failed, path.generic() + ": an entry has a non-portable name"};
    }
    // F-6: names come from disk; a joined path past P1's limits fails closed instead of throwing.
    const auto child = try_join_rel(path, entry.name);
    if (!child) return AcquireError{AcquireReason::publish_failed, path.generic() + ": an entry exceeds the portable path limits"};
    if (auto failure = remove_tree_within(root, *child)) return failure;
  }
  if (with_sharing_retries([&] { return RemoveDirectoryW(target.c_str()) != 0; })) return std::nullopt;
  const DWORD code = GetLastError();
  if (absent(code)) return std::nullopt;
  return win_error(code, path.generic());
}

std::optional<AcquireError> sync_directory(const CanonicalRoot&, const std::optional<RelativePath>&) {
  return std::nullopt;  // NTFS journals metadata; renames use MOVEFILE_WRITE_THROUGH
}

std::optional<AcquireError> write_new_file(const CanonicalRoot& root, const RelativePath& path, const std::string_view bytes) {
  const StoreEnvironment env;
  auto opened = open_part_file(root, path, PartOpen::create_new, env);
  if (auto* failure = std::get_if<AcquireError>(&opened)) return *failure;
  auto& file = std::get<PartFile>(opened);
  if (auto failure = file.append(bytes)) return failure;
  return file.sync();
}

bool exists_within(const CanonicalRoot& root, const RelativePath& path) {
  return GetFileAttributesW(native(root, path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace secure_frontend
#endif
