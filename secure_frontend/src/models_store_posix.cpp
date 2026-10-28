#include "secure_frontend/models_store.hpp"

#include "secure_frontend/model_index_model.hpp"

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/falloc.h>
#include <linux/magic.h>
#include <sys/vfs.h>
#endif

namespace secure_frontend {
namespace {

AcquireError error(const AcquireReason reason, const std::string& detail) { return AcquireError{reason, detail}; }
AcquireError io_error(const int code, const std::string& detail) {
  return error(code == ENOSPC || code == EDQUOT ? AcquireReason::disk_space : AcquireReason::publish_failed, detail);
}

// The directory holding `path` (or the root itself), as an owned O_DIRECTORY fd.
// On failure, *absent (when given) tells a missing parent from one that cannot be
// reached safely (a link, a non-directory, no permission).
int parent_fd(const CanonicalRoot& root, const RelativePath& path, bool* absent = nullptr) {
  if (absent) *absent = false;
  const auto& parts = path.components();
  if (parts.size() == 1U) return ::openat(root.handle(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  std::string parent;
  for (std::size_t i = 0; i + 1U < parts.size(); ++i) parent += (parent.empty() ? "" : "/") + parts[i];
  auto opened = open_beneath(root, must_rel(parent), OpenOptions{OpenKind::directory});  // a prefix of a valid path
  if (const auto* failure = std::get_if<PathError>(&opened)) {
    if (absent) *absent = failure->code == PathErrorCode::not_found;
    return -1;
  }
  return ::fcntl(std::get<OpenedFile>(opened).native(), F_DUPFD_CLOEXEC, 0);
}

int dir_fd(const CanonicalRoot& root, const std::optional<RelativePath>& path) {
  if (!path) return ::openat(root.handle(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  auto opened = open_beneath(root, *path, OpenOptions{OpenKind::directory});
  if (std::holds_alternative<PathError>(opened)) return -1;
  return ::fcntl(std::get<OpenedFile>(opened).native(), F_DUPFD_CLOEXEC, 0);
}

int rename_noreplace(const int from_dir, const char* from, const int to_dir, const char* to) {
#if defined(__linux__) && defined(SYS_renameat2)
  return static_cast<int>(::syscall(SYS_renameat2, from_dir, from, to_dir, to, 1U /* RENAME_NOREPLACE */));
#else
  (void)from_dir;
  (void)from;
  (void)to_dir;
  (void)to;
  errno = ENOSYS;
  return -1;
#endif
}

// A read-only directory (0500, e.g. from make_read_only_directory) cannot have entries
// removed. Restore owner rwx on the directory's own handle (never by path); best effort:
// a failure surfaces as the removal error that follows.
void restore_owner_write(const CanonicalRoot& root, const RelativePath& path) {
  OwnedHandle fd(dir_fd(root, path));
  struct stat info {};
  if (fd.get() < 0 || ::fstat(fd.get(), &info) != 0 || info.st_uid != ::geteuid()) return;
  if ((info.st_mode & S_IRWXU) != S_IRWXU) (void)::fchmod(fd.get(), (info.st_mode & 07777) | S_IRWXU);
}

}  // namespace

std::variant<std::uint64_t, AcquireError> StoreEnvironment::free_bytes(const CanonicalRoot& root) const {
  struct statvfs info {};
  if (::fstatvfs(root.handle(), &info) != 0) return error(AcquireReason::disk_space, "free space cannot be determined");
  return static_cast<std::uint64_t>(info.f_bavail) * static_cast<std::uint64_t>(info.f_frsize);
}

std::variant<FileSystemKind, AcquireError> StoreEnvironment::file_system_kind(const CanonicalRoot& root) const {
#if defined(__linux__)
  struct statfs info {};
  if (::fstatfs(root.handle(), &info) != 0) return error(AcquireReason::bundle_layout, "file system type cannot be determined");
  return static_cast<unsigned long>(info.f_type) == MSDOS_SUPER_MAGIC ? FileSystemKind::fat : FileSystemKind::other;
#else
  (void)root;
  return FileSystemKind::other;
#endif
}

ModelsLock& ModelsLock::operator=(ModelsLock&& other) noexcept {
  if (this != &other) {
    if (owned_) ::close(handle_);
    handle_ = other.handle_;
    owned_ = other.owned_;
    other.owned_ = false;
  }
  return *this;
}

ModelsLock::~ModelsLock() {
  if (owned_) ::close(handle_);  // closing the descriptor releases the flock
}

std::variant<ModelsLock, AcquireError> lock_models_root(const CanonicalRoot& root) {
  const std::string lock_name{kLockFileName};
  const int fd = ::openat(root.handle(), lock_name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return error(AcquireReason::publish_failed, lock_name + " cannot be opened");
  struct stat lock_info {};
  if (::fstat(fd, &lock_info) != 0 || !S_ISREG(lock_info.st_mode) || lock_info.st_uid != ::geteuid()) {
    ::close(fd);
    return error(AcquireReason::publish_failed, lock_name + " is not a regular file owned by you");
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int code = errno;
    ::close(fd);
    return error(code == EWOULDBLOCK ? AcquireReason::models_busy : AcquireReason::publish_failed,
                 "another pull, import, rm or update holds roots.models/" + lock_name);
  }
  return ModelsLock{fd};
}

PartFile::PartFile(PartFile&& other) noexcept
    : handle_(other.handle_), size_(other.size_), path_(std::move(other.path_)), fail_after_(other.fail_after_), open_(other.open_) {
  other.open_ = false;
}

PartFile::~PartFile() { close(); }

void PartFile::close() noexcept {
  if (open_) ::close(handle_);
  open_ = false;
}

std::optional<AcquireError> PartFile::append(const std::string_view bytes) {
  if (fail_after_ && size_ + bytes.size() > *fail_after_) return error(AcquireReason::disk_space, path_);
  std::size_t done = 0;
  while (done < bytes.size()) {
    // pwrite at the tracked size_ — equivalent to O_APPEND+write() for the sole writer (the model
    // lock guarantees one), and it keeps write_at() honest on the same fd (see open_part_file).
    const ssize_t n = ::pwrite(handle_, bytes.data() + done, bytes.size() - done, static_cast<off_t>(size_ + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return io_error(errno, path_);
    done += static_cast<std::size_t>(n);
    size_ += static_cast<std::uint64_t>(n);
  }
  return std::nullopt;
}

std::optional<AcquireError> PartFile::write_at(const std::uint64_t offset, const std::string_view bytes) {
  if (fail_after_ && offset + bytes.size() > *fail_after_) return error(AcquireReason::disk_space, path_);
  std::size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t n = ::pwrite(handle_, bytes.data() + done, bytes.size() - done, static_cast<off_t>(offset + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return io_error(errno, path_);
    done += static_cast<std::size_t>(n);
  }
  return std::nullopt;
}

std::optional<AcquireError> PartFile::truncate_to_zero() {
  if (::ftruncate(handle_, 0) != 0) return io_error(errno, path_);
  size_ = 0;
  return std::nullopt;
}

std::optional<AcquireError> PartFile::preallocate(const std::uint64_t total_size) {
#if defined(__linux__)
  if (::fallocate(handle_, FALLOC_FL_KEEP_SIZE, 0, static_cast<off_t>(total_size)) != 0) {
    if (errno == EOPNOTSUPP || errno == ENOSYS || errno == EINVAL) return std::nullopt;  // unsupported FS: skipped
    return io_error(errno, path_);
  }
#else
  (void)total_size;
#endif
  return std::nullopt;
}

std::optional<AcquireError> PartFile::sync() {
  if (::fsync(handle_) != 0) return io_error(errno, path_);
  return std::nullopt;
}

std::variant<PartFile, AcquireError> open_part_file(const CanonicalRoot& root, const RelativePath& path, const PartOpen mode,
                                                    const StoreEnvironment& env) {
  OwnedHandle dir(parent_fd(root, path));
  if (dir.get() < 0) return error(AcquireReason::publish_failed, path.generic() + ": directory cannot be opened safely");
  // O_WRONLY (no O_APPEND): a part fd serves BOTH append() and write_at(), and on Linux an
  // O_APPEND descriptor overrides pwrite()'s offset argument — every write_at would land at EOF
  // and shred the sparse assembly (regression caught by part_file_write_at_fills_offsets).
  // append() therefore pwrite's at the tracked size_ below; the store's ModelsLock (this file,
  // acquired for every pull/import/rm/update) makes this fd the sole writer, so the offset is
  // stable.
  const int flags = O_WRONLY | O_CLOEXEC | O_NOFOLLOW | (mode == PartOpen::create_new ? O_CREAT | O_EXCL : 0);
  const int fd = ::openat(dir.get(), path.components().back().c_str(), flags, 0600);
  if (fd < 0) return io_error(errno, path.generic() + ": cannot be opened");
  struct stat info {};
  // External roots (bundle directories, SP1 delta f): removable media report any owner.
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1 || (!root.external() && info.st_uid != ::geteuid())) {
    ::close(fd);
    return error(AcquireReason::publish_failed, path.generic() + ": not a private single-link regular file");
  }
  // vfat/exfat refuse mode changes they cannot store; import never relies on bundle modes (§5.3).
  if (mode == PartOpen::create_new && ::fchmod(fd, 0600) != 0 && !(root.external() && errno == EPERM)) {
    const int code = errno;
    ::close(fd);
    return io_error(code, path.generic());
  }
  return PartFile{fd, static_cast<std::uint64_t>(info.st_size), path.generic(), env.fail_writes_after_bytes()};
}

std::optional<AcquireError> ensure_directory(const CanonicalRoot& root, const RelativePath& path) {
  if (const auto failure = create_directory_within(root, path)) {
    return error(AcquireReason::publish_failed, path.generic() + ": " + failure->message);
  }
  return std::nullopt;
}

std::optional<AcquireError> make_read_only_directory(const CanonicalRoot& root, const RelativePath& path) {
  if (auto failure = ensure_directory(root, path)) return failure;
  OwnedHandle fd(dir_fd(root, path));
  if (fd.get() < 0 || ::fchmod(fd.get(), 0500) != 0) return error(AcquireReason::publish_failed, path.generic() + ": mode 0500");
  return std::nullopt;
}

std::optional<AcquireError> rename_within(const CanonicalRoot& root, const RelativePath& from, const RelativePath& to,
                                          const StoreEnvironment& env) {
  OwnedHandle from_dir(parent_fd(root, from));
  OwnedHandle to_dir(parent_fd(root, to));
  if (from_dir.get() < 0 || to_dir.get() < 0) return error(AcquireReason::publish_failed, to.generic() + ": directory cannot be opened");
  const char* from_name = from.components().back().c_str();
  const char* to_name = to.components().back().c_str();
  if (!env.force_rename_fallback()) {
    if (rename_noreplace(from_dir.get(), from_name, to_dir.get(), to_name) == 0) return std::nullopt;
    if (errno == EEXIST) return error(AcquireReason::publish_failed, to.generic() + ": target exists");
    // EINVAL / ENOSYS / EOPNOTSUPP: the file system (some FUSE / NFS) or kernel lacks RENAME_NOREPLACE.
    if (errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP) return io_error(errno, to.generic());
  }
  // Fallback (§4.5 step 4): only under .lock, only when the target is absent.
  struct stat info {};
  if (::fstatat(to_dir.get(), to_name, &info, AT_SYMLINK_NOFOLLOW) == 0) {
    return error(AcquireReason::publish_failed, to.generic() + ": target exists");
  }
  if (errno != ENOENT) return io_error(errno, to.generic());
  if (::renameat(from_dir.get(), from_name, to_dir.get(), to_name) != 0) return io_error(errno, to.generic());
  return std::nullopt;
}

std::optional<AcquireError> remove_file_within(const CanonicalRoot& root, const RelativePath& path) {
  bool absent = false;
  OwnedHandle dir(parent_fd(root, path, &absent));
  if (dir.get() < 0) {  // parent absent: nothing to remove; unreachable: an error, never "removed"
    return absent ? std::nullopt : std::optional<AcquireError>{error(AcquireReason::publish_failed, path.generic() + ": directory cannot be opened")};
  }
  if (::unlinkat(dir.get(), path.components().back().c_str(), 0) != 0 && errno != ENOENT) return io_error(errno, path.generic());
  return std::nullopt;
}

std::optional<AcquireError> remove_tree_within(const CanonicalRoot& root, const RelativePath& path) {
  bool absent = false;
  OwnedHandle parent(parent_fd(root, path, &absent));
  if (parent.get() < 0) {  // parent absent: nothing to remove; unreachable: an error, never "removed"
    return absent ? std::nullopt : std::optional<AcquireError>{error(AcquireReason::publish_failed, path.generic() + ": directory cannot be opened")};
  }
  const char* name = path.components().back().c_str();
  struct stat info {};
  if (::fstatat(parent.get(), name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
    return errno == ENOENT ? std::nullopt : std::optional<AcquireError>{io_error(errno, path.generic())};
  }
  if (!S_ISDIR(info.st_mode)) {
    if (::unlinkat(parent.get(), name, 0) != 0) return io_error(errno, path.generic());
    return std::nullopt;
  }
  restore_owner_write(root, path);
  auto listed = list_directory(root, path);
  if (std::holds_alternative<PathError>(listed)) return error(AcquireReason::publish_failed, path.generic() + ": cannot be listed");
  for (const auto& entry : std::get<std::vector<DirectoryEntry>>(listed)) {
    if (!std::holds_alternative<RelativePath>(parse_relative_path(entry.name))) {
      OwnedHandle dir(dir_fd(root, path));
      if (dir.get() < 0 || ::unlinkat(dir.get(), entry.name.c_str(), entry.type == EntryType::directory ? AT_REMOVEDIR : 0) != 0) {
        return error(AcquireReason::publish_failed, path.generic() + ": an entry cannot be removed");
      }
      continue;
    }
    // F-6: names come from disk; a joined path past P1's limits fails closed instead of throwing.
    const auto child = try_join_rel(path, entry.name);
    if (!child) return error(AcquireReason::publish_failed, path.generic() + ": an entry exceeds the portable path limits");
    if (auto failure = remove_tree_within(root, *child)) return failure;
  }
  if (::unlinkat(parent.get(), name, AT_REMOVEDIR) != 0 && errno != ENOENT) return io_error(errno, path.generic());
  return std::nullopt;
}

std::optional<AcquireError> sync_directory(const CanonicalRoot& root, const std::optional<RelativePath>& path) {
  OwnedHandle fd(dir_fd(root, path));
  if (fd.get() < 0 || ::fsync(fd.get()) != 0) return error(AcquireReason::publish_failed, "directory fsync failed");
  return std::nullopt;
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
  OwnedHandle parent(parent_fd(root, path));
  struct stat info {};
  return parent.get() >= 0 && ::fstatat(parent.get(), path.components().back().c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0;
}

}  // namespace secure_frontend
