#if defined(_WIN32)
#include "canonical_path_internal.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "windows_security.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace secure_frontend {
namespace ws = windows_security;

OwnedHandle::~OwnedHandle() {
  if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
}

NativeHandle OwnedHandle::release() noexcept {
  const NativeHandle kept = handle_;
  handle_ = nullptr;
  return kept;
}

void OpenedFile::close_now() noexcept {
  if (owns_ && handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  owns_ = false;
}

std::variant<std::size_t, PathError> OpenedFile::read_some(char* const buffer, const std::size_t capacity) const {
  DWORD count = 0;
  const DWORD request = static_cast<DWORD>(std::min<std::size_t>(capacity, 1U << 20U));
  if (ReadFile(handle_, buffer, request, &count, nullptr) == 0) {
    return detail::path_error(PathErrorCode::read_failed, "the file could not be read");
  }
  return static_cast<std::size_t>(count);
}

std::variant<OpenedFile, PathError> open_beneath(const CanonicalRoot& root, const RelativePath& path,
                                                 const OpenOptions& options, const WalkMode) {
  using detail::path_error;
  std::array<std::byte, SECURITY_MAX_SID_SIZE> user_storage{};
  PSID user = nullptr;
  if (!ws::current_user_sid(user_storage, &user)) return path_error(PathErrorCode::open_failed, "the current user is unknown");
  if (options.trust == FileTrust::owner_only && !ws::has_private_owner_dacl(root.handle(), user)) {
    return path_error(PathErrorCode::permissions_too_broad, "the root must have a protected owner-only access control list");
  }
  std::wstring current = root.path().wstring();
  const auto& parts = path.components();
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const bool last = i + 1U == parts.size();
    current += L'\\';
    current += path_from_utf8(parts[i]).wstring();
    DWORD access = READ_CONTROL | FILE_READ_ATTRIBUTES;  // READ_CONTROL: GetSecurityInfo needs it (defect b)
    if (last && options.kind == OpenKind::regular_file) access |= GENERIC_READ;
    HANDLE raw = CreateFileW(ws::extended_path(current).c_str(), access, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
      const DWORD code = GetLastError();
      return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
                 ? path_error(PathErrorCode::not_found, "the path does not exist")
                 : path_error(PathErrorCode::open_failed, "the path could not be opened");
    }
    OwnedHandle guard(raw);
    if (ws::is_reparse_point(raw)) {
      return path_error(PathErrorCode::symlink_not_allowed, "a path component is a symbolic link, junction or other reparse point");
    }
    const auto final_path = classify_final_path(ws::final_path_for_handle(raw));
    const auto* final_text = std::get_if<std::wstring>(&final_path);
    if (final_text == nullptr || !is_within(root.path(), std::filesystem::path{*final_text})) {
      return path_error(PathErrorCode::escapes_root, "the path resolves outside the root");
    }
    if (options.trust == FileTrust::owner_only && !ws::has_private_owner_dacl(raw, user)) {
      return path_error(PathErrorCode::permissions_too_broad, "every path component must have a protected owner-only ACL");
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    GetFileInformationByHandleEx(raw, FileAttributeTagInfo, &tag, sizeof(tag));
    const bool directory = (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
    if (!last) {
      if (!directory) return path_error(PathErrorCode::not_directory, "a path component is not a directory");
      continue;
    }
    if (options.kind == OpenKind::directory && !directory) return path_error(PathErrorCode::not_directory, "the path is not a directory");
    if (options.kind == OpenKind::regular_file && (directory || GetFileType(raw) != FILE_TYPE_DISK)) {
      return path_error(PathErrorCode::not_regular_file, "the path is not a regular file");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(raw, &info) == 0) return path_error(PathErrorCode::open_failed, "the file cannot be inspected");
    if (options.reject_hard_links && info.nNumberOfLinks > 1U) {
      return path_error(PathErrorCode::hard_link_not_allowed, "the file has more than one hard link");
    }
    if (options.trust == FileTrust::not_shared_writable) {
      switch (ws::write_access_is_private(raw)) {
        case ws::Trust::trusted: break;
        case ws::Trust::untrusted_owner: return path_error(PathErrorCode::untrusted_owner, "the file must be owned by you, SYSTEM or Administrators");
        case ws::Trust::shared_writable: return path_error(PathErrorCode::shared_writable, "the file must not be writable by other users");
        case ws::Trust::unreadable: return path_error(PathErrorCode::open_failed, "the file's security settings cannot be read");
      }
    }
    const std::uint64_t size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow;
    return detail::PathFactory::opened(guard.release(), size);
  }
  return path_error(PathErrorCode::open_failed, "the path has no components");
}

namespace detail {

std::variant<CanonicalRoot, PathError> open_existing_root(const std::filesystem::path& requested, const RootPolicy policy,
                                                          const bool also_trusted_installer) {
  const std::wstring text = requested.wstring();
  if (auto error = check_windows_root_form(text)) return *error;
  std::wstring full(32768, L'\0');
  const DWORD length = GetFullPathNameW(text.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
  if (length == 0U || length >= full.size()) return path_error(PathErrorCode::root_unavailable, "the root cannot be resolved");
  full.resize(length);
  // No FILE_FLAG_OPEN_REPARSE_POINT: the root itself may be a junction or symlink.
  HANDLE handle = CreateFileW(ws::extended_path(full).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return path_error(PathErrorCode::root_unavailable, "the root does not exist or cannot be opened");
  OwnedHandle guard(handle);
  if (policy == RootPolicy::external) {
    // exFAT/FAT or junction-mounted media: FileAttributeTagInfo and the final-path classifier do not work
    // there, so confirm "directory" with GetFileAttributesW (which does) and use the normalized path.
    const DWORD attrs = GetFileAttributesW(ws::extended_path(full).c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
      return path_error(PathErrorCode::not_directory, "the root is not a directory");
    }
    return PathFactory::root(std::filesystem::path{full}, guard.release(), 0U, true);
  }
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) == 0 ||
      (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
    return path_error(PathErrorCode::not_directory, "the root is not a directory");
  }
  auto classified = classify_final_path(ws::final_path_for_handle(handle));
  if (auto* error = std::get_if<PathError>(&classified)) return *error;
  // External roots (bundle directories) skip this: FAT/exFAT volumes have no ACLs, so
  // Trust::unreadable is accepted there too.
  if (policy != RootPolicy::external) {
    switch (ws::write_access_is_private(handle, also_trusted_installer)) {
      case ws::Trust::trusted: break;
      case ws::Trust::untrusted_owner:
        return path_error(PathErrorCode::untrusted_owner, "the root must be owned by you, SYSTEM or Administrators");
      case ws::Trust::shared_writable:
        return path_error(PathErrorCode::shared_writable, "the root must not be writable by other users");
      case ws::Trust::unreadable:
        return path_error(PathErrorCode::root_unavailable, "the root's security settings cannot be read");
    }
  }
  // Ownership moves to the CanonicalRoot.
  return PathFactory::root(std::filesystem::path{std::get<std::wstring>(classified)}, guard.release(), 0U,
                           policy == RootPolicy::external);
}

}  // namespace detail

std::variant<CanonicalFile, PathError> open_executable(const std::filesystem::path& requested,
                                                      const ExecutableWriters writers) {
  using detail::path_error;
  const std::wstring text = requested.wstring();
  if (auto error = check_windows_root_form(text)) return *error;
  // \\?\ disables Win32 normalization ('/' separators, '.' and '..'), so the
  // path is made full and normalized first, exactly as open_existing_root does.
  std::wstring full(32768, L'\0');
  const DWORD length = GetFullPathNameW(text.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
  if (length == 0U || length >= full.size()) {
    return path_error(PathErrorCode::not_found, "the engine executable path cannot be resolved");
  }
  full.resize(length);
  HANDLE handle = CreateFileW(ws::extended_path(full).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return path_error(PathErrorCode::not_found, "the engine executable does not exist");
  OwnedHandle guard(handle);
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) == 0 ||
      (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U || GetFileType(handle) != FILE_TYPE_DISK) {
    return path_error(PathErrorCode::not_regular_file, "the engine executable must be a regular file");
  }
  auto classified = classify_final_path(ws::final_path_for_handle(handle));
  if (auto* error = std::get_if<PathError>(&classified)) return *error;
  const bool also_trusted_installer = writers == ExecutableWriters::also_trusted_installer;
  const std::filesystem::path canonical{std::get<std::wstring>(classified)};
  if (relaxed_paths()) {
    // Owner opt-in: the engine may sit in a dev/build folder writable by others; it is a regular file that
    // exists, so accept it. Local opt-in only.
    return detail::PathFactory::file(canonical);
  }
  if (ws::write_access_is_private(handle, also_trusted_installer) != ws::Trust::trusted) {
    return path_error(PathErrorCode::shared_writable, "the engine executable must not be writable by other users");
  }
  auto parent = detail::open_existing_root(canonical.parent_path(), RootPolicy::existing, also_trusted_installer);
  if (std::holds_alternative<PathError>(parent)) {
    return path_error(PathErrorCode::shared_writable, "the engine executable's folder must not be writable by other users");
  }
  return detail::PathFactory::file(canonical);
}

std::optional<PathError> create_directory_within(const CanonicalRoot& root, const RelativePath& path) {
  using detail::path_error;
  ws::PrivateSecurityAttributes attributes(true);
  if (attributes.get() == nullptr) return path_error(PathErrorCode::create_failed, "a private security descriptor could not be built");
  std::wstring current = root.path().wstring();
  for (const auto& part : path.components()) {
    current += L'\\';
    current += path_from_utf8(part).wstring();
    if (CreateDirectoryW(ws::extended_path(current).c_str(), attributes.get()) == 0 && GetLastError() != ERROR_ALREADY_EXISTS) {
      return path_error(PathErrorCode::create_failed, "a directory could not be created");
    }
    HANDLE raw = CreateFileW(ws::extended_path(current).c_str(), FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return path_error(PathErrorCode::create_failed, "a directory could not be opened");
    OwnedHandle guard(raw);
    if (ws::is_reparse_point(raw)) return path_error(PathErrorCode::symlink_not_allowed, "a path component is a junction or link");
    FILE_ATTRIBUTE_TAG_INFO tag{};
    GetFileInformationByHandleEx(raw, FileAttributeTagInfo, &tag, sizeof(tag));
    if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) return path_error(PathErrorCode::not_directory, "a path component is not a directory");
  }
  return std::nullopt;
}

std::variant<std::vector<DirectoryEntry>, PathError> list_directory(const CanonicalRoot& root,
                                                                    const std::optional<RelativePath>& below) {
  using detail::path_error;
  std::wstring directory = root.path().wstring();
  // Held open (no FILE_SHARE_DELETE) for the whole listing so the directory
  // cannot be renamed or replaced while it is enumerated.
  std::optional<OpenedFile> pinned;
  if (below.has_value()) {
    auto opened = open_beneath(root, *below, OpenOptions{.kind = OpenKind::directory});  // refuses reparse points on the way
    if (const auto* failure = std::get_if<PathError>(&opened)) return *failure;
    pinned.emplace(std::move(std::get<OpenedFile>(opened)));
    for (const auto& component : below->components()) directory += L'\\' + path_from_utf8(component).wstring();
  }
  WIN32_FIND_DATAW data{};
  HANDLE find = FindFirstFileExW(ws::extended_path(directory + L"\\*").c_str(), FindExInfoBasic, &data,
                                 FindExSearchNameMatch, nullptr, 0);
  if (find == INVALID_HANDLE_VALUE) return path_error(PathErrorCode::open_failed, "the directory could not be listed");
  std::vector<DirectoryEntry> entries;
  do {
    const std::wstring name{data.cFileName};
    if (name == L"." || name == L"..") continue;
    const DWORD attributes = data.dwFileAttributes;
    const EntryType type = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ? EntryType::symlink
                           : (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U   ? EntryType::directory
                                                                             : EntryType::regular_file;
    const std::uint64_t size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32U) | data.nFileSizeLow;
    entries.push_back(DirectoryEntry{path_to_utf8(std::filesystem::path{name}), type, size});
  } while (FindNextFileW(find, &data) != 0);
  const DWORD last_error = GetLastError();
  FindClose(find);
  if (last_error != ERROR_NO_MORE_FILES) {
    return path_error(PathErrorCode::read_failed, "the directory could not be read");
  }
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
  return entries;
}

bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) noexcept {
  std::wstring base = root.wstring();
  const std::wstring text = candidate.wstring();
  while (base.size() > 3U && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
  if (base.empty() || text.size() < base.size() ||
      CompareStringOrdinal(base.c_str(), static_cast<int>(base.size()), text.c_str(), static_cast<int>(base.size()), TRUE) !=
          CSTR_EQUAL) {
    return false;
  }
  return text.size() == base.size() || base.back() == L'\\' || text[base.size()] == L'\\' || text[base.size()] == L'/';
}

}  // namespace secure_frontend
#endif
