#include "secure_frontend/models_store.hpp"

#include "secure_frontend/token_store.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace secure_frontend {

RelativePath must_rel(const std::string_view text) {
  auto parsed = parse_relative_path(text);
  if (auto* path = std::get_if<RelativePath>(&parsed)) return std::move(*path);
  throw std::logic_error("internal relative path is not P1-valid");
}

RelativePath join_rel(const RelativePath& base, const std::string_view more) { return must_rel(base.generic() + "/" + std::string{more}); }

std::optional<RelativePath> try_join_rel(const RelativePath& base, const std::string_view more) {
  auto parsed = parse_relative_path(base.generic() + "/" + std::string{more});
  if (auto* path = std::get_if<RelativePath>(&parsed)) return std::move(*path);
  return std::nullopt;
}

std::optional<AcquireError> replace_small_file(const CanonicalRoot& root, const RelativePath& path, const std::string_view bytes) {
  if (const auto failure = write_private_file(root, path, bytes, TokenStoreMode::replace)) {
    return AcquireError{AcquireReason::index_write_failed, path.generic() + " (" + failure->message + ")"};
  }
  return std::nullopt;
}

std::variant<std::uint64_t, AcquireError> hash_into(const CanonicalRoot& root, const RelativePath& path, Sha256& hasher,
                                                    const std::uint64_t max_bytes, const std::atomic<bool>* interrupted) {
  auto opened = open_beneath(root, path, OpenOptions{OpenKind::regular_file, FileTrust::any, true});
  if (std::holds_alternative<PathError>(opened)) {
    return AcquireError{AcquireReason::publish_failed, path.generic() + ": not a single-link regular file below the root"};
  }
  const auto& file = std::get<OpenedFile>(opened);
  if (file.size() > max_bytes) return AcquireError{AcquireReason::size_exceeded, path.generic()};
  std::string buffer(kSha256ReadChunkBytes, '\0');
  std::uint64_t total = 0;
  while (true) {
    if (interrupted != nullptr && interrupted->load()) return AcquireError{AcquireReason::interrupted, path.generic()};
    auto read = file.read_some(buffer.data(), buffer.size());
    if (std::holds_alternative<PathError>(read)) return AcquireError{AcquireReason::publish_failed, path.generic() + ": read failed"};
    const auto n = std::get<std::size_t>(read);
    if (n == 0) break;
    total += n;
    if (total > max_bytes) return AcquireError{AcquireReason::size_exceeded, path.generic()};
    hasher.update(std::string_view{buffer.data(), n});
  }
  return total;
}

std::variant<std::string, AcquireError> read_small_file(const CanonicalRoot& root, const RelativePath& path, const std::size_t max_bytes) {
  auto opened = open_beneath(root, path, OpenOptions{OpenKind::regular_file, FileTrust::any, true});
  if (std::holds_alternative<PathError>(opened)) return AcquireError{AcquireReason::index_inconsistent, path.generic() + ": cannot be opened"};
  auto bytes = std::get<OpenedFile>(opened).read_all(max_bytes);
  if (std::holds_alternative<PathError>(bytes)) return AcquireError{AcquireReason::index_inconsistent, path.generic() + ": too large or unreadable"};
  return std::get<std::string>(std::move(bytes));
}

namespace {

std::variant<std::vector<StagedEntry>, AcquireError> list_from(const CanonicalRoot& root, const std::optional<RelativePath>& start) {
  std::vector<StagedEntry> out;
  std::vector<std::pair<std::optional<RelativePath>, std::string>> pending{{start, std::string{}}};
  while (!pending.empty()) {
    auto [dir, prefix] = std::move(pending.back());
    pending.pop_back();
    auto listed = list_directory(root, dir);
    if (std::holds_alternative<PathError>(listed)) {
      return AcquireError{AcquireReason::publish_failed, (dir ? dir->generic() : std::string{"root"}) + ": cannot be listed"};
    }
    for (const auto& entry : std::get<std::vector<DirectoryEntry>>(listed)) {
      const std::string relative = prefix.empty() ? entry.name : prefix + "/" + entry.name;
      auto own_name = parse_relative_path(entry.name);
      if (!std::holds_alternative<RelativePath>(own_name)) {
        out.push_back(StagedEntry{relative, entry.size, false});  // not P1-portable: never ours, never descended
        continue;
      }
      // F-6: names come from disk (possibly untrusted media); a joined path past P1's
      // limits fails closed instead of throwing.
      auto child = dir ? try_join_rel(*dir, entry.name) : std::optional<RelativePath>{std::get<RelativePath>(std::move(own_name))};
      if (!child) return AcquireError{AcquireReason::bundle_invalid, "a path below the tree exceeds the portable path limits"};
      if (entry.type == EntryType::directory) {
        pending.emplace_back(std::move(*child), relative);
        continue;
      }
      bool regular = entry.type == EntryType::regular_file;
      if (regular) {  // hard links count as not regular (§4.4 check 2, §5.2)
        regular = std::holds_alternative<OpenedFile>(open_beneath(root, *child, OpenOptions{OpenKind::regular_file, FileTrust::any, true}));
      }
      out.push_back(StagedEntry{relative, entry.size, regular});
    }
  }
  std::sort(out.begin(), out.end(), [](const StagedEntry& a, const StagedEntry& b) { return a.path < b.path; });
  return out;
}

}  // namespace

std::variant<std::vector<StagedEntry>, AcquireError> list_files_recursive(const CanonicalRoot& root, const RelativePath& dir) {
  return list_from(root, dir);
}

std::variant<std::vector<StagedEntry>, AcquireError> list_files_recursive(const CanonicalRoot& root) { return list_from(root, std::nullopt); }

}  // namespace secure_frontend
