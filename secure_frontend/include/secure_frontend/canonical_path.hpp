#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace secure_frontend {

enum class PathErrorCode {
  empty, too_long, component_too_long, absolute, traversal, invalid_character, control_character, reserved_name,
  trailing_dot_or_space, short_name_alias, invalid_utf8, drive_or_stream,
  not_absolute, unsupported_root_form, network_location, root_unavailable, untrusted_owner, shared_writable,
  not_found, not_directory, not_regular_file, not_owned_by_user, permissions_too_broad, symlink_not_allowed,
  escapes_root, cross_device, hard_link_not_allowed, open_failed, read_failed, too_large, create_failed,
};

// `message` is fixed text that never contains the path or any part of it.
struct PathError final {
  PathErrorCode code;
  std::string message;
};

// The one portable relative-path grammar (SP1 P1). Input may use '/' or '\';
// components are 1-255 bytes of valid UTF-8, the whole path at most 1024
// bytes. Rejected: empty components, "." and "..", a leading separator,
// ':' anywhere (drives, alternate data streams), < > " | ? *, bytes below
// 0x20 and 0x7F (NUL included), a trailing dot or space, Windows reserved
// device names with any extension (CON PRN AUX NUL COM0-9 LPT0-9 incl. the
// superscript digits, CONIN$ CONOUT$ CLOCK$), and 8.3 aliases ('~' + digit).
inline constexpr std::size_t kRelativePathMaxBytes = 1024;  // the whole P1 path
class RelativePath final {
 public:
  [[nodiscard]] const std::string& generic() const noexcept { return generic_; }
  [[nodiscard]] const std::vector<std::string>& components() const noexcept { return components_; }

 private:
  friend std::variant<RelativePath, PathError> parse_relative_path(std::string_view text);
  RelativePath(std::string generic, std::vector<std::string> components)
      : generic_(std::move(generic)), components_(std::move(components)) {}
  std::string generic_;
  std::vector<std::string> components_;
};

[[nodiscard]] std::variant<RelativePath, PathError> parse_relative_path(std::string_view text);

// The single reserved-device-name list (also used for model IDs).
[[nodiscard]] bool is_windows_reserved_component(std::string_view component) noexcept;

// UTF-8 <-> std::filesystem::path without the Windows ANSI code page.
[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view text);
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path);

#if defined(_WIN32)
using NativeHandle = void*;  // HANDLE
#else
using NativeHandle = int;    // file descriptor
#endif

class OwnedHandle final {
 public:
  explicit OwnedHandle(NativeHandle handle) noexcept : handle_(handle) {}
  ~OwnedHandle();
  OwnedHandle(const OwnedHandle&) = delete;
  OwnedHandle& operator=(const OwnedHandle&) = delete;
  [[nodiscard]] NativeHandle get() const noexcept { return handle_; }
  // Gives up ownership (the destructor then closes nothing) and returns the handle.
  [[nodiscard]] NativeHandle release() noexcept;

 private:
  NativeHandle handle_;
};

namespace detail {
struct PathFactory;
}

// A root directory resolved once to its canonical absolute form (the root
// itself may be reached through a symlink or junction) and held open. It is
// owned by the current user (or root / SYSTEM / Administrators) and not
// writable by anyone else. Everything below it is opened relative to it.
// A root opened with RootPolicy::external is canonical and held open the same way, but its owner and write access were not checked (bundle directories only).
class CanonicalRoot final {
 public:
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] NativeHandle handle() const noexcept { return handle_->get(); }
  [[nodiscard]] std::uint64_t device_id() const noexcept { return device_; }
  // True only for a root opened with RootPolicy::external.
  [[nodiscard]] bool external() const noexcept { return external_; }
  // The canonical root joined with a validated relative path (native separators).
  [[nodiscard]] std::filesystem::path join(const RelativePath& relative) const;

 private:
  friend struct detail::PathFactory;
  CanonicalRoot(std::filesystem::path path, std::shared_ptr<const OwnedHandle> handle, const std::uint64_t device,
                const bool external)
      : path_(std::move(path)), handle_(std::move(handle)), device_(device), external_(external) {}
  std::filesystem::path path_;
  std::shared_ptr<const OwnedHandle> handle_;
  std::uint64_t device_;
  bool external_;
};

// engine.executable: absolute, canonical, a regular file not writable by
// group/other (other SIDs on Windows), in a directory with the same property.
class CanonicalFile final {
 public:
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  friend struct detail::PathFactory;
  explicit CanonicalFile(std::filesystem::path path) : path_(std::move(path)) {}
  std::filesystem::path path_;
};

// create_private: a missing last component is created 0700 / private DACL
// (reached only from serve and init, never from validation or --check).
// external (SP4 §5): a bundle directory on any local file system, including
// removable exFAT/FAT media whose modes the OS ignores: canonicalized and held
// open like any root (links below it are still refused), but its owner and
// write access are not checked, neither for the root nor for directories
// created below it. Used only for bundle source and target directories.
// model_folder (owner 2026-10-29, experimental: "support SMB shares or CIFS shares or NFS shares for models"): a model
// folder. On this PC's own drives it is checked as `existing`; on a network share - a UNC path \\server\share\..., a
// mapped network drive, a Linux NFS/CIFS/SMB mount - it opens like `external` (the share's permissions are not
// checked: this PC cannot vouch for them) while network_shares() allows it, held under its \\server\share name on
// Windows. Without the opt-in Windows refuses a share (network_location) and Linux checks a mount as any folder.
enum class RootPolicy { existing, create_private, external, model_folder };

// roots.allow_network_shares (experimental, off by default): whether a model_folder root may be on a network share.
// Set once at config load.
void set_network_shares(bool on) noexcept;
[[nodiscard]] bool network_shares() noexcept;
// A Linux statfs f_type that is a network file system: NFS, SMB, CIFS, SMB2 (pure: tested on every platform).
[[nodiscard]] bool is_network_file_system(unsigned long long type) noexcept;

// Owner opt-in (roots.allow_untrusted_media): tolerate model/engine files on exFAT/FAT or junction-mounted
// volumes by skipping owner-only / not-shared-writable / reparse checks. Set once at config load; opt-in only.
void set_relaxed_paths(bool on) noexcept;
[[nodiscard]] bool relaxed_paths() noexcept;

[[nodiscard]] std::variant<CanonicalRoot, PathError> open_root(const std::filesystem::path& requested,
                                                               RootPolicy policy = RootPolicy::existing);
// also_trusted_installer (SP4 §3.2, pull.downloader): on Windows the TrustedInstaller
// service is an additional permitted owner/writer of the file and its directory
// (System32\curl.exe). Ignored on POSIX. The default is SP1's rule, unchanged.
enum class ExecutableWriters { standard, also_trusted_installer };
[[nodiscard]] std::variant<CanonicalFile, PathError> open_executable(const std::filesystem::path& requested,
                                                                    ExecutableWriters writers = ExecutableWriters::standard);

// Creates every missing component of `path` below `root` with mode 0700
// (Linux) or a protected owner-only DACL (Windows), refusing links and
// non-directories on the way. Nothing is created outside `root`.
[[nodiscard]] std::optional<PathError> create_directory_within(const CanonicalRoot& root, const RelativePath& path);

// Side-effect free (used by validation and therefore --check): if `requested`
// exists it must open as a private root, and its canonical path is returned; if
// it does not exist, its parent must open as a private root and its last
// component must be one valid relative name, and canonical(parent)/name is
// returned. Nothing is created, written or held open.
[[nodiscard]] std::variant<std::filesystem::path, PathError> check_creatable_root(const std::filesystem::path& requested);

// Containment on a separator boundary: "/r" does not contain "/r2". Windows
// compares case-insensitively (CompareStringOrdinal).
[[nodiscard]] bool is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) noexcept;

// Pure string checks, compiled on every platform so they are tested everywhere.
// Accepts only drive-absolute X:\... roots; rejects drive-relative C:foo,
// rooted \foo, UNC \\server\share, \\?\, \\.\, \??\, streams and reserved names. allow_unc (a model folder
// while network shares are allowed): \\server\share[\...] too, in backslashes, its names checked as a drive path's.
[[nodiscard]] std::optional<PathError> check_windows_root_form(std::wstring_view text, bool allow_unc = false);
// GetFinalPathNameByHandleW output -> X:\... ; \\?\UNC\... is network_location, or \\server\share\... with allow_unc.
[[nodiscard]] std::variant<std::wstring, PathError> classify_final_path(std::wstring_view final_path, bool allow_unc = false);

enum class OpenKind { regular_file, directory };
enum class FileTrust { any, not_shared_writable, owner_only };
struct OpenOptions final {
  OpenKind kind{OpenKind::regular_file};
  FileTrust trust{FileTrust::any};
  bool reject_hard_links{false};
};
// fallback_only: tests force the openat(O_NOFOLLOW) walk.
enum class WalkMode { automatic, fallback_only };

class OpenedFile final {
 public:
  OpenedFile(OpenedFile&& other) noexcept : handle_(other.handle_), size_(other.size_), owns_(other.owns_) { other.owns_ = false; }
  OpenedFile& operator=(OpenedFile&& other) noexcept;
  OpenedFile(const OpenedFile&) = delete;
  OpenedFile& operator=(const OpenedFile&) = delete;
  ~OpenedFile();
  [[nodiscard]] NativeHandle native() const noexcept { return handle_; }
  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  // Reads at most `capacity` bytes at the current position; 0 means end of file.
  [[nodiscard]] std::variant<std::size_t, PathError> read_some(char* buffer, std::size_t capacity) const;
  // The whole (regular) file, or too_large if it exceeds max_bytes.
  [[nodiscard]] std::variant<std::string, PathError> read_all(std::size_t max_bytes) const;

 private:
  friend struct detail::PathFactory;
  OpenedFile(const NativeHandle handle, const std::uint64_t size) noexcept : handle_(handle), size_(size), owns_(true) {}
  void close_now() noexcept;
  NativeHandle handle_;
  std::uint64_t size_;
  bool owns_;
};

// Opens `path` below `root` without following any link: Linux uses openat2
// (RESOLVE_BENEATH | NO_SYMLINKS | NO_MAGICLINKS | NO_XDEV) when the kernel has
// it, else a component-wise openat(O_NOFOLLOW) walk with st_dev checks;
// Windows opens each component with FILE_FLAG_OPEN_REPARSE_POINT and checks
// its reparse tag, final-path containment and (owner_only) protected DACL.
[[nodiscard]] std::variant<OpenedFile, PathError> open_beneath(const CanonicalRoot& root, const RelativePath& path,
                                                               const OpenOptions& options = {},
                                                               WalkMode mode = WalkMode::automatic);

enum class EntryType { regular_file, directory, symlink, other };  // symlink covers junctions and other reparse points
struct DirectoryEntry final {
  std::string name;  // UTF-8
  EntryType type;
  std::uint64_t size;
};
// Lists `root` (or a directory below it, opened with open_beneath) without
// following links; a junction or any reparse point reports EntryType::symlink.
// Entries are sorted by name.
[[nodiscard]] std::variant<std::vector<DirectoryEntry>, PathError> list_directory(
    const CanonicalRoot& root, const std::optional<RelativePath>& below = std::nullopt);

}  // namespace secure_frontend
