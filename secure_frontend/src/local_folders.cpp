#include "secure_frontend/local_folders.hpp"

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>

#if defined(_WIN32)
#include "windows_security.hpp"
#else
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/vfs.h>
#endif

#include <cerrno>

#include <fstream>
#include <set>
#include <sstream>
#endif

namespace secure_frontend {
namespace {

constexpr std::size_t kMaxScannedEntries = 100000;   // a listing reads at most this many directory entries
constexpr std::size_t kMaxModelFolderEntries = 256;
constexpr std::size_t kMaxModelTreeEntries = 4096;   // with the packs' and the download cache's files

bool ascii_less(const std::string& a, const std::string& b) {
  const auto lower = [](const unsigned char c) { return static_cast<char>(std::tolower(c)); };
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [&lower](const char x, const char y) {
    return lower(static_cast<unsigned char>(x)) < lower(static_cast<unsigned char>(y));
  });
}

bool gguf_name(const std::string& name) {
  if (name.size() <= 5U) return false;
  std::string tail = name.substr(name.size() - 5U);
  std::transform(tail.begin(), tail.end(), tail.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return tail == ".gguf";
}

std::optional<std::string> utf8_name(const std::filesystem::path& name) {
  try {
    std::string text = path_to_utf8(name);
    if (text.empty() || !is_valid_utf8(text)) return std::nullopt;
    return text;
  } catch (...) {
    return std::nullopt;   // a name that is no valid UTF-16 / UTF-8
  }
}

#if defined(_WIN32)
// Fixed and removable drives only (no network, optical or RAM drive); no error dialog for an empty card reader. While
// network shares are allowed (experimental), a network drive too.
bool local_drive(const std::wstring& root) {
  const UINT type = GetDriveTypeW(root.c_str());
  return type == DRIVE_FIXED || type == DRIVE_REMOVABLE || (type == DRIVE_REMOTE && network_shares());
}

// The root GetDriveTypeW takes: X:\, or a share's \\server\share\ (std::filesystem's root of a UNC path stops at
// \\server\ on MSVC, which is no drive).
std::wstring drive_root(const std::filesystem::path& path) {
  const std::wstring text = path.wstring();
  if (text.rfind(L"\\\\", 0) != 0) return path.root_path().wstring();
  const auto server_end = text.find(L'\\', 2U);
  if (server_end == std::wstring::npos) return text + L"\\";
  const auto share_end = text.find(L'\\', server_end + 1U);
  return share_end == std::wstring::npos ? text + L"\\" : text.substr(0, share_end + 1U);
}

struct QuietErrors final {
  DWORD old{0};
  QuietErrors() { SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old); }
  ~QuietErrors() { SetThreadErrorMode(old, nullptr); }
  QuietErrors(const QuietErrors&) = delete;
  QuietErrors& operator=(const QuietErrors&) = delete;
};

// Hidden, system and reparse-point (link, junction) entries are not offered.
bool skipped_attributes(const std::filesystem::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes == INVALID_FILE_ATTRIBUTES ||
         (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U;
}

bool is_link(const std::filesystem::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
}
#else
bool skipped_attributes(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec)) || ec;
}

bool is_link(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::is_symlink(std::filesystem::symlink_status(path, ec)) || ec;
}

// /proc/self/mounts escapes a space, tab, newline and backslash as \ooo.
std::string unescape_mount(const std::string& text) {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto octal = [&text](const std::size_t at) { return text[at] >= '0' && text[at] <= '7'; };
    if (text[i] == '\\' && i + 3U < text.size() && octal(i + 1U) && octal(i + 2U) && octal(i + 3U)) {
      out.push_back(static_cast<char>(((text[i + 1U] - '0') << 6) | ((text[i + 2U] - '0') << 3) | (text[i + 3U] - '0')));
      i += 3U;
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

std::optional<LocalVolume> statvfs_volume(const std::string& mount) {
  struct statvfs info {};
  if (::statvfs(mount.c_str(), &info) != 0 || info.f_blocks == 0U) return std::nullopt;
  const auto unit = static_cast<std::uint64_t>(info.f_frsize != 0U ? info.f_frsize : info.f_bsize);
  return LocalVolume{.path = mount, .label = {}, .free_bytes = static_cast<std::uint64_t>(info.f_bavail) * unit,
                     .total_bytes = static_cast<std::uint64_t>(info.f_blocks) * unit};
}
#endif

}  // namespace

std::vector<LocalVolume> local_volumes() {
  std::vector<LocalVolume> volumes;
#if defined(_WIN32)
  const QuietErrors quiet;
  wchar_t buffer[512]{};
  const DWORD length = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(buffer) - 1U), buffer);
  if (length == 0U || length >= std::size(buffer)) return volumes;
  for (const wchar_t* root = buffer; *root != L'\0' && volumes.size() < kMaxLocalVolumes; root += wcslen(root) + 1U) {
    const std::wstring drive{root};
    if (!local_drive(drive)) continue;
    const bool network = GetDriveTypeW(drive.c_str()) == DRIVE_REMOTE;
    ULARGE_INTEGER available{};
    ULARGE_INTEGER total{};
    if (GetDiskFreeSpaceExW(drive.c_str(), &available, &total, nullptr) == 0 || total.QuadPart == 0U) continue;
    wchar_t label[MAX_PATH + 1]{};
    std::string label_text;
    if (GetVolumeInformationW(drive.c_str(), label, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0) != 0) {
      label_text = utf8_name(std::filesystem::path{std::wstring{label}}).value_or("");
    }
    volumes.push_back(LocalVolume{.path = path_to_utf8(std::filesystem::path{drive}), .label = std::move(label_text),
                                  .free_bytes = available.QuadPart, .total_bytes = total.QuadPart, .network = network});
  }
#else
  if (auto root = statvfs_volume("/")) volumes.push_back(std::move(*root));
  // Linux: the mounted local block filesystems (no pseudo, network or boot filesystem); while network shares are allowed
  // (experimental), the NFS, CIFS and SMB mounts too.
  static const std::set<std::string> kNetwork{"nfs", "nfs4", "cifs", "smb3", "smbfs"};
  static const std::set<std::string> kLocal{"ext2", "ext3", "ext4", "xfs", "btrfs", "f2fs", "zfs", "exfat", "vfat",
                                            "ntfs", "ntfs3", "fuseblk", "jfs", "reiserfs", "bcachefs"};
  std::ifstream mounts("/proc/self/mounts");
  std::set<std::string> seen{"/"};
  std::string line;
  while (volumes.size() < kMaxLocalVolumes && std::getline(mounts, line)) {
    std::istringstream fields(line);
    std::string device;
    std::string mount;
    std::string type;
    if (!(fields >> device >> mount >> type)) continue;
    const bool network = kNetwork.count(type) != 0U;
    if (kLocal.count(type) == 0U && !(network && network_shares())) continue;
    mount = unescape_mount(mount);
    const auto under = [&mount](const std::string_view prefix) {
      return mount == prefix || (mount.size() > prefix.size() && mount.compare(0, prefix.size(), prefix) == 0 && mount[prefix.size()] == '/');
    };
    if (under("/proc") || under("/sys") || under("/dev") || under("/run") || under("/boot") || under("/snap")) continue;
    if (!seen.insert(mount).second || !is_valid_utf8(mount)) continue;
    if (auto volume = statvfs_volume(mount)) {
      volume->network = network;
      volumes.push_back(std::move(*volume));
    }
  }
#endif
  return volumes;
}

#if defined(_WIN32)
// a share's path, or a mapped network drive, while network shares are off: the switch that allows it
constexpr const char* kSharesOff =
    "the folder is on a network share: turn on Settings > Model folders > \"Use model folders on network shares\" "
    "(experimental) and restart the server to use it";
#endif

std::variant<std::filesystem::path, FolderError> local_folder_path(const std::string_view path_utf8) {
  if (path_utf8.empty()) return FolderError{"type or pick a folder"};
  if (path_utf8.size() > 4096U || !is_valid_utf8(path_utf8) || path_utf8.find('\0') != std::string_view::npos) {
    return FolderError{"the folder's path is not valid"};
  }
#if defined(_WIN32)
  // "X:\..." only: no UNC (\\server), device (\\?\, \\.\) or drive-relative (X:folder) path - but a share's folder
  // \\server\share\... while network shares are allowed (experimental), in the form a model folder's root takes
  const bool drive = path_utf8.size() >= 3U && std::isalpha(static_cast<unsigned char>(path_utf8[0])) != 0 && path_utf8[1] == ':' &&
                     (path_utf8[2] == '\\' || path_utf8[2] == '/');
  const bool share = network_shares() && path_utf8.size() > 2U && path_utf8[0] == '\\' && path_utf8[1] == '\\' &&
                     !check_windows_root_form(path_from_utf8(path_utf8).wstring(), true).has_value();
  if (!drive && !share) {
    if (network_shares()) {
      return FolderError{"the folder must be a full path on a drive of this PC (like D:\\models) or on a share "
                         "(like \\\\server\\share\\models)"};
    }
    const bool unc = path_utf8.size() > 2U && path_utf8[0] == '\\' && path_utf8[1] == '\\' && path_utf8[2] != '?' && path_utf8[2] != '.';
    return FolderError{unc ? kSharesOff : "the folder must be a full path on a drive of this PC (like D:\\models)"};
  }
#endif
  const std::filesystem::path path = path_from_utf8(path_utf8);
  if (!path.is_absolute()) return FolderError{"the folder must be a full path"};
  for (const auto& part : path.relative_path()) {
    if (part == "." || part == "..") return FolderError{"the folder's path may not hold . or .."};
  }
#if defined(_WIN32)
  if (!local_drive(drive_root(path))) {
    return FolderError{network_shares() ? "the folder must be on a drive of this PC or a share it reaches" : kSharesOff};
  }
#endif
  std::filesystem::path normal = path.lexically_normal();
  if (!normal.has_filename() && normal != normal.root_path()) normal = normal.parent_path();
  return normal;
}

bool on_network_share(const std::filesystem::path& folder) {
#if defined(_WIN32)
  const std::wstring root = drive_root(folder);
  if (root.rfind(L"\\\\", 0) == 0) return true;  // \\server\share\ (a device path never reaches here: it is no folder)
  const QuietErrors quiet;
  return GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
#elif defined(__linux__)
  struct statfs info {};
  return ::statfs(folder.c_str(), &info) == 0 && is_network_file_system(static_cast<unsigned long long>(info.f_type));
#else
  (void)folder;
  return false;
#endif
}

std::optional<std::uint64_t> free_space(const std::filesystem::path& folder) {
  std::error_code ec;
  const auto info = std::filesystem::space(folder, ec);
  if (ec) return std::nullopt;
  return static_cast<std::uint64_t>(info.available);
}

std::variant<FolderListing, FolderError> list_local_folder(const std::string_view path_utf8) {
  auto checked = local_folder_path(path_utf8);
  if (auto* error = std::get_if<FolderError>(&checked)) return std::move(*error);
  const auto& path = std::get<std::filesystem::path>(checked);
#if defined(_WIN32)
  const QuietErrors quiet;
#endif
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(path, ec);
  if (ec || !std::filesystem::is_directory(status)) return FolderError{"there is no such folder"};
  if (path != path.root_path() && is_link(path)) return FolderError{"the folder is a link; pick the folder it points to"};
  FolderListing listing;
  listing.path = path_to_utf8(path);
  listing.parent = path == path.root_path() ? std::string{} : path_to_utf8(path.parent_path());
  std::size_t scanned = 0;
  std::filesystem::directory_iterator it(path, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) return FolderError{"the folder could not be read"};
  for (const std::filesystem::directory_iterator end; it != end; it.increment(ec)) {
    if (ec || ++scanned > kMaxScannedEntries) {
      listing.truncated = true;
      break;
    }
    const auto name = utf8_name(it->path().filename());
    if (!name || name->front() == '.' || skipped_attributes(it->path())) continue;
    std::error_code entry_ec;
    const auto entry_status = it->symlink_status(entry_ec);
    if (entry_ec) continue;
    FolderEntry entry{.name = *name};
    if (std::filesystem::is_directory(entry_status)) {
      entry.folder = true;
    } else if (std::filesystem::is_regular_file(entry_status) && gguf_name(*name)) {
      entry.bytes = static_cast<std::uint64_t>(it->file_size(entry_ec));
      if (entry_ec) continue;
    } else {
      continue;
    }
    if (listing.entries.size() >= kMaxFolderEntries) {
      listing.truncated = true;
      break;
    }
    listing.entries.push_back(std::move(entry));
  }
  std::sort(listing.entries.begin(), listing.entries.end(), [](const FolderEntry& a, const FolderEntry& b) {
    if (a.folder != b.folder) return a.folder;
    if (ascii_less(a.name, b.name)) return true;
    if (ascii_less(b.name, a.name)) return false;
    return a.name < b.name;
  });
  listing.free_bytes = free_space(path).value_or(0U);
  return listing;
}

#if defined(_WIN32)
bool make_tree_owner_only(const std::filesystem::path& root) {
  const std::wstring sid = windows_security::current_user_sid_string();
  if (sid.empty()) return false;
  const std::wstring sddl = L"D:P(A;OICI;FA;;;" + sid + L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) == 0) return false;
  BOOL present = FALSE;
  BOOL defaulted = FALSE;
  PACL dacl = nullptr;
  bool ok = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) != 0 && present != FALSE;
  if (ok) {
    std::wstring path = std::filesystem::absolute(root).wstring();
    ok = TreeResetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT,
                                     DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl,
                                     nullptr, FALSE, nullptr, ProgressInvokeNever, nullptr) == ERROR_SUCCESS;
  }
  LocalFree(descriptor);
  return ok;
}
#else
bool make_tree_owner_only(const std::filesystem::path& root) {
  const auto strip = [](const std::filesystem::path& path) {
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) return false;
    if (S_ISLNK(info.st_mode)) return true;
    const mode_t wanted = info.st_mode & ~static_cast<mode_t>(S_IWGRP | S_IWOTH);
    return (info.st_mode & 07777) == (wanted & 07777) || ::chmod(path.c_str(), wanted & 07777) == 0;
  };
  if (!strip(root)) return false;
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::none, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (!strip(it->path())) return false;
  }
  return !ec;
}
#endif

namespace {

// The folders a model's own tools put beside its files: its packs (pack/ and pack-<name>/, as Maya's setup and this
// server put a GLM-5.3 or raon1 pack; maya-ds4/ and maya-ds4-<name>/ for DeepSeek, maya-mm3/ and maya-mm3-<name>/ for
// MiniMax) and Hugging Face's download cache.
bool model_tool_folder(const std::string& name) {
  const auto named = [&name](const std::string_view base) {
    return name == base || (name.size() > base.size() + 1U && name.compare(0, base.size(), base) == 0 && name[base.size()] == '-');
  };
  return named("pack") || named("maya-ds4") || named("maya-mm3") || name == ".cache";
}

// Such a folder's whole tree: no link anywhere in it (none is followed), at most kMaxModelTreeEntries entries in all.
std::optional<FolderError> check_model_tool_tree(const std::filesystem::path& folder, std::size_t& entries) {
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(folder, std::filesystem::directory_options::none, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (++entries > kMaxModelTreeEntries) return FolderError{"the folder holds too many files to be a model folder"};
    if (is_link(it->path())) return FolderError{"the folder holds a link; only a folder of model files can be made private"};
  }
  if (ec) return FolderError{"the folder could not be read"};
  return std::nullopt;
}

// the MTP draft layer's runtime files as upstream's setup leaves them in mtp/rt (tools/mtp_rt.py, refresh_draft_vocab)
constexpr std::array<std::string_view, 3> kMtpRuntimeFiles{"dense.bin", "experts.bin", "draft_vocab.bin"};

}  // namespace

bool holds_mtp_runtime(const std::filesystem::path& rt) {
  std::error_code ec;
  return std::all_of(kMtpRuntimeFiles.begin(), kMtpRuntimeFiles.end(), [&rt, &ec](const std::string_view file) {
    return std::filesystem::is_regular_file(rt / file, ec) && !std::filesystem::is_symlink(rt / file, ec);
  });
}

std::optional<FolderError> make_model_folder_private(const std::filesystem::path& folder) {
  if (!folder.is_absolute()) return FolderError{"the folder must be a full path"};
  if (on_network_share(folder)) {
    return FolderError{"a folder on a network share can't be made private from this PC; with network shares on "
                       "(Settings > Model folders, experimental) it is used as it is"};
  }
  auto checked = local_folder_path(path_to_utf8(folder));
  if (auto* error = std::get_if<FolderError>(&checked)) return std::move(*error);
  const auto& path = std::get<std::filesystem::path>(checked);
  if (path == path.root_path()) return FolderError{"a drive's root can't be made private; pick the model's own folder"};
  std::error_code ec;
  if (!std::filesystem::is_directory(std::filesystem::symlink_status(path, ec)) || ec) return FolderError{"there is no such folder"};
  if (is_link(path)) return FolderError{"the folder is a link; pick the folder it points to"};
  // an MTP draft layer (owner 2026-10-29: "when I select the mtp I get the error that the folder was not made private"):
  // its runtime folder, or the folder holding it as rt/ beside the fetched tensors/ - no .gguf file needed
  const bool draft_layer = holds_mtp_runtime(path);
  const bool draft_home = holds_mtp_runtime(path / "rt");
  std::size_t entries = 0;
  std::size_t tree_entries = 0;
  std::size_t models = 0;
  for (std::filesystem::directory_iterator it(path, ec), end; it != end; it.increment(ec)) {
    if (ec || ++entries > kMaxModelFolderEntries) return FolderError{"the folder holds too many files to be a model folder"};
    std::error_code entry_ec;
    const auto status = it->symlink_status(entry_ec);
    if (entry_ec || is_link(it->path())) return FolderError{"the folder holds a link; only a folder of model files can be made private"};
    const auto name = utf8_name(it->path().filename());
    if (std::filesystem::is_directory(status)) {
      const bool layer_folder = draft_home && name && (*name == "rt" || *name == "tensors");
      if (!name || !(model_tool_folder(*name) || layer_folder)) {
        return FolderError{"the folder holds other folders; only a folder of model files (and their packs) can be made private"};
      }
      if (auto failure = check_model_tool_tree(it->path(), tree_entries)) return failure;
      continue;
    }
    if (std::filesystem::is_regular_file(status) && name && gguf_name(*name)) ++models;
  }
  if (ec) return FolderError{"the folder could not be read"};
  if (models == 0U && !draft_layer && !draft_home) return FolderError{"the folder holds no .gguf file and no MTP draft layer"};
#if !defined(_WIN32)
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0 || info.st_uid != ::geteuid()) return FolderError{"the folder belongs to another user"};
#endif
  if (!make_tree_owner_only(path)) return FolderError{"the folder could not be made private (it must belong to this user)"};
  if (!std::holds_alternative<CanonicalRoot>(open_root(path, RootPolicy::existing))) {
    return FolderError{"the folder is still not private (it must belong to this user, SYSTEM or the Administrators)"};
  }
  return std::nullopt;
}

std::variant<std::filesystem::path, FolderError> make_private_folder(const std::filesystem::path& parent, const std::string_view name) {
  const bool good_name = !name.empty() && name.size() <= 64U && name.front() != '.' && name.front() != ' ' && name.back() != '.' &&
                         name.back() != ' ' && std::all_of(name.begin(), name.end(), [](const char c) {
                           return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                                  c == '_' || c == ' ';
                         });
  if (!good_name) return FolderError{"a folder name is letters, digits, spaces, dots, dashes and underscores"};
  if (on_network_share(parent)) {
    return FolderError{"a private folder can't be made on a network share from this PC; make the folder on the share itself"};
  }
  auto checked = local_folder_path(path_to_utf8(parent));
  if (auto* error = std::get_if<FolderError>(&checked)) return std::move(*error);
  const auto& base = std::get<std::filesystem::path>(checked);
  std::error_code ec;
  if (!std::filesystem::is_directory(std::filesystem::symlink_status(base, ec)) || ec) return FolderError{"there is no such folder"};
  const auto target = base / path_from_utf8(name);
  bool existed = false;
#if defined(_WIN32)
  windows_security::PrivateSecurityAttributes attributes(true);
  if (attributes.get() == nullptr) return FolderError{"the folder could not be made"};
  if (CreateDirectoryW(windows_security::extended_path(std::filesystem::absolute(target).wstring()).c_str(), attributes.get()) == 0) {
    if (GetLastError() != ERROR_ALREADY_EXISTS) return FolderError{"the folder could not be made"};
    existed = true;
  }
#else
  struct stat info {};
  if (::lstat(base.c_str(), &info) != 0) return FolderError{"there is no such folder"};
  if ((info.st_mode & S_IWOTH) != 0 && (info.st_mode & S_ISVTX) == 0) {
    return FolderError{"anyone may rename what is in that folder; pick another"};
  }
  if (::mkdir(target.c_str(), 0700) != 0) {
    if (errno != EEXIST) return FolderError{"the folder could not be made"};
    existed = true;
  }
#endif
  auto opened = open_root(target, RootPolicy::existing);
  if (auto* root = std::get_if<CanonicalRoot>(&opened)) return root->path();
  return FolderError{existed ? "a folder of that name is already there (and is not private)" : "the new folder is not private"};
}

}  // namespace secure_frontend
