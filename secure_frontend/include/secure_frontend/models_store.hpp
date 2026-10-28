#pragma once

#include "secure_frontend/acquire_reason.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/sha256.hpp"
#include "secure_frontend/staging_plan.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace secure_frontend {

inline constexpr std::uint64_t kFreeSpaceMarginBytes = std::uint64_t{1} << 30U;
inline constexpr std::uint64_t kFreeSpaceRecheckBytes = std::uint64_t{1} << 30U;

enum class FileSystemKind { other, fat };

// Spec §10.1 seams. Production code uses this class as is.
class StoreEnvironment {
 public:
  virtual ~StoreEnvironment() = default;
  [[nodiscard]] virtual std::variant<std::uint64_t, AcquireError> free_bytes(const CanonicalRoot& root) const;
  [[nodiscard]] virtual std::variant<FileSystemKind, AcquireError> file_system_kind(const CanonicalRoot& root) const;
  [[nodiscard]] virtual bool force_rename_fallback() const { return false; }
  [[nodiscard]] virtual std::optional<std::uint64_t> fail_writes_after_bytes() const { return std::nullopt; }
  // SECURE_FRONTEND_TEST_HOOKS builds only (publish_staged never asks otherwise): true fails
  // §4.5 step 4 (.staging/<k> -> version directory) with publish_failed.
  [[nodiscard]] virtual bool fail_version_rename() const { return false; }
  [[nodiscard]] virtual std::uint64_t fat_max_file_bytes() const { return 0xFFFFFFFFULL; }
  // §4.6: free space is re-checked against kFreeSpaceMarginBytes every this many bytes written.
  [[nodiscard]] virtual std::uint64_t recheck_interval_bytes() const { return kFreeSpaceRecheckBytes; }
};

// For names this code builds itself (".staging/<key>", "<name>/<tag>/v…").
// Throws std::logic_error if `text` is not a P1 relative path.
[[nodiscard]] RelativePath must_rel(std::string_view text);
[[nodiscard]] RelativePath join_rel(const RelativePath& base, std::string_view more);
// For names read from disk (F-6): nullopt, never a throw, when the joined path is
// not P1-valid (e.g. over 1024 bytes although every component is valid).
[[nodiscard]] std::optional<RelativePath> try_join_rel(const RelativePath& base, std::string_view more);

class ModelsLock final {
 public:
  explicit ModelsLock(NativeHandle handle) noexcept : handle_(handle) {}
  ModelsLock(ModelsLock&& other) noexcept : handle_(other.handle_), owned_(other.owned_) { other.owned_ = false; }
  ModelsLock& operator=(ModelsLock&& other) noexcept;
  ModelsLock(const ModelsLock&) = delete;
  ModelsLock& operator=(const ModelsLock&) = delete;
  ~ModelsLock();

 private:
  NativeHandle handle_;
  bool owned_{true};
};

// roots.models/.lock (0600 / private DACL), exclusive, non-blocking (§4.8).
[[nodiscard]] std::variant<ModelsLock, AcquireError> lock_models_root(const CanonicalRoot& root);

class PartFile final {
 public:
  PartFile(NativeHandle handle, std::uint64_t size, std::string path, std::optional<std::uint64_t> fail_after) noexcept
      : handle_(handle), size_(size), path_(std::move(path)), fail_after_(fail_after) {}
  PartFile(PartFile&& other) noexcept;
  PartFile& operator=(PartFile&& other) = delete;
  PartFile(const PartFile&) = delete;
  PartFile& operator=(const PartFile&) = delete;
  ~PartFile();
  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] std::optional<AcquireError> append(std::string_view bytes);
  // Writes bytes at an absolute offset (parallel ranged downloads, spec
  // docs/superpowers/specs/2026-10-23-parallel-downloader-design.md). Unlike append() it does
  // not move a write cursor or change size(); the caller tracks completeness (the piece
  // manifest) and serializes concurrent write_at calls to one PartFile.
  [[nodiscard]] std::optional<AcquireError> write_at(std::uint64_t offset, std::string_view bytes);
  [[nodiscard]] std::optional<AcquireError> truncate_to_zero();
  // fallocate(FALLOC_FL_KEEP_SIZE) / FileAllocationInfo: never changes size(); unsupported = success.
  [[nodiscard]] std::optional<AcquireError> preallocate(std::uint64_t total_size);
  [[nodiscard]] std::optional<AcquireError> sync();
  void close() noexcept;

 private:
  NativeHandle handle_;
  std::uint64_t size_;
  std::string path_;  // relative, for error details
  std::optional<std::uint64_t> fail_after_;
  bool open_{true};
};

enum class PartOpen { create_new, open_existing };
[[nodiscard]] std::variant<PartFile, AcquireError> open_part_file(const CanonicalRoot& root, const RelativePath& path,
                                                                  PartOpen mode, const StoreEnvironment& env);

[[nodiscard]] std::optional<AcquireError> ensure_directory(const CanonicalRoot& root, const RelativePath& path);
[[nodiscard]] std::optional<AcquireError> make_read_only_directory(const CanonicalRoot& root, const RelativePath& path);
// Never replaces; renameat2(RENAME_NOREPLACE), else fstatat-absent + renameat (caller holds .lock);
// Windows MoveFileExW(WRITE_THROUGH) with sharing-violation retries.
[[nodiscard]] std::optional<AcquireError> rename_within(const CanonicalRoot& root, const RelativePath& from,
                                                        const RelativePath& to, const StoreEnvironment& env);
[[nodiscard]] std::optional<AcquireError> remove_file_within(const CanonicalRoot& root, const RelativePath& path);
[[nodiscard]] std::optional<AcquireError> remove_tree_within(const CanonicalRoot& root, const RelativePath& path);
[[nodiscard]] std::optional<AcquireError> sync_directory(const CanonicalRoot& root, const std::optional<RelativePath>& path);
[[nodiscard]] std::optional<AcquireError> write_new_file(const CanonicalRoot& root, const RelativePath& path, std::string_view bytes);
// §4.1 atomic_replace_within: SP1's write_private_file(…, TokenStoreMode::replace).
[[nodiscard]] std::optional<AcquireError> replace_small_file(const CanonicalRoot& root, const RelativePath& path,
                                                             std::string_view bytes);
// `interrupted` (§8.3, final fix F1): checked before every chunk; set → AcquireError interrupted,
// so a whole-file re-hash of a 30-55 GB shard stops within one chunk. nullptr: never interrupted.
[[nodiscard]] std::variant<std::uint64_t, AcquireError> hash_into(const CanonicalRoot& root, const RelativePath& path,
                                                                  Sha256& hasher, std::uint64_t max_bytes,
                                                                  const std::atomic<bool>* interrupted = nullptr);
[[nodiscard]] std::variant<std::string, AcquireError> read_small_file(const CanonicalRoot& root, const RelativePath& path,
                                                                      std::size_t max_bytes);
// A joined path over P1's limits fails closed with bundle_invalid (F-6).
[[nodiscard]] std::variant<std::vector<StagedEntry>, AcquireError> list_files_recursive(const CanonicalRoot& root,
                                                                                        const RelativePath& dir);
[[nodiscard]] std::variant<std::vector<StagedEntry>, AcquireError> list_files_recursive(const CanonicalRoot& root);
[[nodiscard]] bool exists_within(const CanonicalRoot& root, const RelativePath& path);

}  // namespace secure_frontend
