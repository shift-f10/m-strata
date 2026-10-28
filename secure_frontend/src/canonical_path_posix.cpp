#if !defined(_WIN32)
#include "canonical_path_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#else
struct open_how {
  std::uint64_t flags;
  std::uint64_t mode;
  std::uint64_t resolve;
};
#define RESOLVE_NO_XDEV 0x01
#define RESOLVE_NO_MAGICLINKS 0x02
#define RESOLVE_NO_SYMLINKS 0x04
#define RESOLVE_BENEATH 0x08
#endif
#ifndef SYS_openat2
#define SYS_openat2 437
#endif
#endif

namespace secure_frontend {

OwnedHandle::~OwnedHandle() {
  if (handle_ >= 0) ::close(handle_);
}

NativeHandle OwnedHandle::release() noexcept {
  const NativeHandle kept = handle_;
  handle_ = -1;
  return kept;
}

void OpenedFile::close_now() noexcept {
  if (owns_ && handle_ >= 0) ::close(handle_);
  owns_ = false;
}

std::variant<std::size_t, PathError> OpenedFile::read_some(char* const buffer, const std::size_t capacity) const {
  while (true) {
    const ssize_t count = ::read(handle_, buffer, capacity);
    if (count >= 0) return static_cast<std::size_t>(count);
    if (errno != EINTR) return detail::path_error(PathErrorCode::read_failed, "the file could not be read");
  }
}

namespace {
using detail::path_error;

int final_flags(const OpenKind kind) {
  return kind == OpenKind::directory ? (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                                     // O_NONBLOCK: a FIFO cannot block before fstat rejects it.
                                     : (O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
}

PathError from_errno(const int error) {
  switch (error) {
    case ELOOP: return path_error(PathErrorCode::symlink_not_allowed, "a path component is a symbolic link");
    case EXDEV: return path_error(PathErrorCode::cross_device, "the path crosses into another file system");
    case ENOENT: return path_error(PathErrorCode::not_found, "the path does not exist");
    case ENOTDIR: return path_error(PathErrorCode::not_directory, "a path component is not a directory");
    default: return path_error(PathErrorCode::open_failed, "the path could not be opened");
  }
}

int try_openat2(const CanonicalRoot& root, const RelativePath& path, const int flags) {
#if defined(__linux__)
  open_how how{};
  how.flags = static_cast<std::uint64_t>(flags);
  how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV;
  return static_cast<int>(::syscall(SYS_openat2, root.handle(), path.generic().c_str(), &how, sizeof(how)));
#else
  (void)root;
  (void)path;
  (void)flags;
  errno = ENOSYS;
  return -1;
#endif
}

std::variant<int, PathError> walk_components(const CanonicalRoot& root, const RelativePath& path, const int flags) {
  int current = -1;
  const auto& parts = path.components();
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const bool last = i + 1U == parts.size();
    const int base = current < 0 ? root.handle() : current;
    const int next = ::openat(base, parts[i].c_str(), last ? flags : (O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    const int saved = errno;
    if (next < 0) {
      PathError error = from_errno(saved);
      struct stat entry {};
      if (saved == ENOTDIR && ::fstatat(base, parts[i].c_str(), &entry, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(entry.st_mode)) {
        error = path_error(PathErrorCode::symlink_not_allowed, "a path component is a symbolic link");
      }
      if (current >= 0) ::close(current);
      return error;
    }
    if (current >= 0) ::close(current);
    struct stat st {};
    if (::fstat(next, &st) != 0 || static_cast<std::uint64_t>(st.st_dev) != root.device_id()) {
      ::close(next);
      return path_error(PathErrorCode::cross_device, "the path crosses into another file system");
    }
    current = next;
  }
  return current;
}
}  // namespace

std::variant<OpenedFile, PathError> open_beneath(const CanonicalRoot& root, const RelativePath& path,
                                                 const OpenOptions& options, const WalkMode mode) {
  const int flags = final_flags(options.kind);
  int fd = -1;
  if (mode == WalkMode::automatic) {
    fd = try_openat2(root, path, flags);
    if (fd < 0 && errno != ENOSYS && errno != EPERM) return from_errno(errno);
  }
  if (fd < 0) {
    auto walked = walk_components(root, path, flags);
    if (auto* error = std::get_if<PathError>(&walked)) return *error;
    fd = std::get<int>(walked);
  }
  OwnedHandle guard(fd);
  struct stat st {};
  if (::fstat(fd, &st) != 0) return path_error(PathErrorCode::open_failed, "the opened path could not be inspected");
  if (options.kind == OpenKind::directory && !S_ISDIR(st.st_mode)) {
    return path_error(PathErrorCode::not_directory, "the path is not a directory");
  }
  if (options.kind == OpenKind::regular_file && !S_ISREG(st.st_mode)) {
    return path_error(PathErrorCode::not_regular_file, "the path is not a regular file");
  }
  if (static_cast<std::uint64_t>(st.st_dev) != root.device_id()) {
    return path_error(PathErrorCode::cross_device, "the path crosses into another file system");
  }
  if (options.reject_hard_links && st.st_nlink > 1) {
    return path_error(PathErrorCode::hard_link_not_allowed, "the file has more than one hard link");
  }
  if (options.trust == FileTrust::owner_only) {
    if (st.st_uid != ::geteuid()) return path_error(PathErrorCode::not_owned_by_user, "the file must be owned by the current user");
    if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
      return path_error(PathErrorCode::permissions_too_broad, "the file must not grant group or other access");
    }
  } else if (options.trust == FileTrust::not_shared_writable) {
    if (st.st_uid != ::geteuid() && st.st_uid != 0) {
      return path_error(PathErrorCode::untrusted_owner, "the file must be owned by the current user or root");
    }
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      return path_error(PathErrorCode::shared_writable, "the file must not be writable by group or others");
    }
  }
  return detail::PathFactory::opened(guard.release(), static_cast<std::uint64_t>(st.st_size));
}

namespace detail {

std::variant<CanonicalRoot, PathError> open_existing_root(const std::filesystem::path& requested, const RootPolicy policy,
                                                          const bool /*also_trusted_installer: Windows only*/) {
  const std::string text = requested.string();
  if (text.empty() || text.find('\0') != std::string::npos) return path_error(PathErrorCode::empty, "the root path is empty");
  if (!requested.is_absolute()) return path_error(PathErrorCode::not_absolute, "roots must be absolute paths");
  char* resolved = ::realpath(text.c_str(), nullptr);
  if (resolved == nullptr) {
    return path_error(PathErrorCode::root_unavailable, "the root does not exist or cannot be resolved");
  }
  const std::string canonical{resolved};
  std::free(resolved);
#if defined(O_PATH)
  const int fd = ::open(canonical.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
#else
  const int fd = ::open(canonical.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
#endif
  if (fd < 0) {
    return path_error(errno == ENOTDIR ? PathErrorCode::not_directory : PathErrorCode::root_unavailable,
                      "the root could not be opened as a directory");
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
    ::close(fd);
    return path_error(PathErrorCode::not_directory, "the root is not a directory");
  }
  if (policy != RootPolicy::external) {
    if (st.st_uid != ::geteuid() && st.st_uid != 0) {
      ::close(fd);
      return path_error(PathErrorCode::untrusted_owner, "the root must be owned by the current user or root");
    }
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      ::close(fd);
      return path_error(PathErrorCode::shared_writable, "the root must not be writable by group or others");
    }
  }
  return PathFactory::root(std::filesystem::path{canonical}, fd, static_cast<std::uint64_t>(st.st_dev), policy == RootPolicy::external);
}

}  // namespace detail

std::variant<CanonicalFile, PathError> open_executable(const std::filesystem::path& requested,
                                                      const ExecutableWriters /*writers: Windows only*/) {
  using detail::path_error;
  const std::string text = requested.string();
  if (text.empty() || !requested.is_absolute()) {
    return path_error(PathErrorCode::not_absolute, "the engine executable must be an absolute path");
  }
  char* resolved = ::realpath(text.c_str(), nullptr);
  if (resolved == nullptr) return path_error(PathErrorCode::not_found, "the engine executable does not exist");
  const std::string canonical{resolved};
  std::free(resolved);
  struct stat st {};
  if (::stat(canonical.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return path_error(PathErrorCode::not_regular_file, "the engine executable must be a regular file");
  }
  if (st.st_uid != ::geteuid() && st.st_uid != 0) {
    return path_error(PathErrorCode::untrusted_owner, "the engine executable must be owned by the current user or root");
  }
  if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    return path_error(PathErrorCode::shared_writable, "the engine executable must not be writable by group or others");
  }
  const std::filesystem::path canonical_path{canonical};
  struct stat parent {};
  if (::stat(canonical_path.parent_path().c_str(), &parent) != 0 || (parent.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
      (parent.st_uid != ::geteuid() && parent.st_uid != 0)) {
    return path_error(PathErrorCode::shared_writable,
                      "the engine executable's directory must be private (owned by you or root, not group/other-writable)");
  }
  return detail::PathFactory::file(canonical_path);
}

std::optional<PathError> create_directory_within(const CanonicalRoot& root, const RelativePath& path) {
  using detail::path_error;
  int current = -1;
  for (const auto& part : path.components()) {
    const int base = current < 0 ? root.handle() : current;
    if (::mkdirat(base, part.c_str(), 0700) != 0 && errno != EEXIST) {
      if (current >= 0) ::close(current);
      return path_error(PathErrorCode::create_failed, "a directory could not be created");
    }
    const int next = ::openat(base, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    const int saved = errno;
    if (next < 0) {
      struct stat entry {};
      const bool is_link = (saved == ENOTDIR || saved == ELOOP) &&
                           ::fstatat(base, part.c_str(), &entry, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(entry.st_mode);
      if (current >= 0) ::close(current);
      if (is_link) return path_error(PathErrorCode::symlink_not_allowed, "a path component is a symbolic link");
      return saved == ENOTDIR ? path_error(PathErrorCode::not_directory, "a path component is not a directory")
                              : path_error(PathErrorCode::create_failed, "a directory could not be opened");
    }
    if (current >= 0) ::close(current);
    struct stat st {};
    if (::fstat(next, &st) != 0 || static_cast<std::uint64_t>(st.st_dev) != root.device_id()) {
      ::close(next);
      return path_error(PathErrorCode::cross_device, "the path crosses into another file system");
    }
    if (!root.external() && (st.st_uid != ::geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0)) {
      ::close(next);
      return path_error(PathErrorCode::shared_writable, "an existing directory on the way is not private");
    }
    current = next;
  }
  if (current >= 0) ::close(current);
  return std::nullopt;
}

std::variant<std::vector<DirectoryEntry>, PathError> list_directory(const CanonicalRoot& root,
                                                                    const std::optional<RelativePath>& below) {
  using detail::path_error;
  int fd = -1;
  if (below.has_value()) {
    auto opened = open_beneath(root, *below, OpenOptions{.kind = OpenKind::directory});
    if (const auto* failure = std::get_if<PathError>(&opened)) return *failure;
    fd = ::fcntl(std::get<OpenedFile>(opened).native(), F_DUPFD_CLOEXEC, 0);
  } else {
    fd = ::openat(root.handle(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  }
  if (fd < 0) return path_error(PathErrorCode::open_failed, "the directory could not be opened");
  DIR* directory = ::fdopendir(fd);
  if (directory == nullptr) {
    ::close(fd);
    return path_error(PathErrorCode::open_failed, "the directory could not be listed");
  }
  std::vector<DirectoryEntry> entries;
  for (;;) {
    errno = 0;  // readdir returns nullptr both at the end and on error
    const dirent* entry = ::readdir(directory);
    if (entry == nullptr) {
      if (errno != 0) {
        ::closedir(directory);
        return path_error(PathErrorCode::read_failed, "the directory could not be read");
      }
      break;
    }
    const std::string name{entry->d_name};
    if (name == "." || name == "..") continue;
    struct stat st {};
    if (::fstatat(::dirfd(directory), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      ::closedir(directory);
      return path_error(PathErrorCode::open_failed, "a directory entry could not be inspected");
    }
    const EntryType type = S_ISLNK(st.st_mode) ? EntryType::symlink
                           : S_ISDIR(st.st_mode) ? EntryType::directory
                           : S_ISREG(st.st_mode) ? EntryType::regular_file
                                                 : EntryType::other;
    entries.push_back(DirectoryEntry{name, type, static_cast<std::uint64_t>(st.st_size)});
  }
  ::closedir(directory);
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
  return entries;
}

bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) noexcept {
  const std::filesystem::path base = root.has_filename() ? root : root.parent_path();
  auto r = base.begin();
  auto c = candidate.begin();
  for (; r != base.end(); ++r, ++c) {
    if (c == candidate.end() || *r != *c) return false;
  }
  return true;
}

}  // namespace secure_frontend
#endif
