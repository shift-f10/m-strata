#pragma once

// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §4.2, §4.3, §6 D4): the folders
// the page may pick on this PC - the local drives (fixed and removable: no network, optical or RAM drive) with their
// free space, one folder's sub-folders and GGUF files - and a model folder made private so the server trusts it.
// Paths come from the page: each is checked here (absolute, local, no "..") and never echoed in an error.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

struct LocalVolume final {
  std::string path;   // "C:\" (Windows), "/" or a mount point (POSIX); UTF-8
  std::string label;  // the volume's label; may be empty
  std::uint64_t free_bytes{0};
  std::uint64_t total_bytes{0};
};
inline constexpr std::size_t kMaxLocalVolumes = 32;
[[nodiscard]] std::vector<LocalVolume> local_volumes();

struct FolderEntry final {
  std::string name;
  bool folder{false};       // a sub-folder; else a .gguf file
  std::uint64_t bytes{0};   // a file's size
};
struct FolderListing final {
  std::string path;                  // the folder (absolute, normalized; UTF-8)
  std::string parent;                // empty at a drive's root
  std::vector<FolderEntry> entries;  // the sub-folders, then the .gguf files, each by name; no hidden entry, no link
  bool truncated{false};             // more than kMaxFolderEntries were found
  std::uint64_t free_bytes{0};       // the folder's volume
};
// Fixed text only: never the path.
struct FolderError final {
  std::string message;
};
inline constexpr std::size_t kMaxFolderEntries = 1000;

// A path from the page, checked: valid UTF-8, absolute, no "." or ".." part; Windows: a drive letter path on a fixed
// or removable drive (no UNC, device or drive-relative path).
[[nodiscard]] std::variant<std::filesystem::path, FolderError> local_folder_path(std::string_view path_utf8);
[[nodiscard]] std::variant<FolderListing, FolderError> list_local_folder(std::string_view path_utf8);
// The free space of the folder's volume (for the caller; nullopt when it cannot be read).
[[nodiscard]] std::optional<std::uint64_t> free_space(const std::filesystem::path& folder);

// A whole folder tree made owner-only: Windows, a protected DACL (the user, SYSTEM and Administrators, inherited by
// everything below, every object below reset to inherit it only); POSIX, no group or other write (links are never
// followed). False when a step fails.
[[nodiscard]] bool make_tree_owner_only(const std::filesystem::path& root);

// The page's "Make this folder private" for a model folder: only a folder of model files - a local folder that is not
// a drive's root and holds at least one .gguf file, no link and at most 256 entries, and no sub-folder but the ones a
// model's tools put beside it (pack, pack-<name>, maya-ds4, maya-ds4-<name>; Hugging Face's .cache), each with no link
// anywhere in it and at most 4096 entries in all - and then checked as the server checks a model folder (open_root).
// nullopt: done.
[[nodiscard]] std::optional<FolderError> make_model_folder_private(const std::filesystem::path& folder);

// A new folder `name` in `parent` (an existing local folder, which others may write - a drive's root), owner-only from
// its creation: Windows, created with a protected DACL (the user, SYSTEM, Administrators: others can neither rename
// nor delete it); POSIX, mode 0700, and not inside a folder anyone may rename entries of (world-writable without the
// sticky bit). An existing folder of that name is answered only when it is already private. The name: 1-64 letters,
// digits, spaces, dots, dashes and underscores, not starting with a dot or space nor ending with one. Then checked as
// the server checks a model folder (open_root); the result is its canonical path.
[[nodiscard]] std::variant<std::filesystem::path, FolderError> make_private_folder(const std::filesystem::path& parent,
                                                                                    std::string_view name);

}  // namespace secure_frontend
