#include "acquire_cases.hpp"
#include "secure_frontend/models_store.hpp"
#include "secure_frontend/sha256.hpp"
#include "support/expect.hpp"
#include "support/test_support.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace acquire_cases {
namespace {

using secure_frontend::must_rel;
using secure_frontend_test::expect;

secure_frontend::CanonicalRoot scratch_root() {
  auto opened = secure_frontend::open_root(secure_frontend_test::make_scratch_directory());
  return std::get<secure_frontend::CanonicalRoot>(std::move(opened));
}

class TestEnvironment final : public secure_frontend::StoreEnvironment {
 public:
  std::uint64_t free{std::uint64_t{1} << 40U};
  secure_frontend::FileSystemKind kind{secure_frontend::FileSystemKind::other};
  bool fallback{false};
  std::optional<std::uint64_t> fail_after;
  std::variant<std::uint64_t, secure_frontend::AcquireError> free_bytes(const secure_frontend::CanonicalRoot&) const override { return free; }
  std::variant<secure_frontend::FileSystemKind, secure_frontend::AcquireError> file_system_kind(
      const secure_frontend::CanonicalRoot&) const override { return kind; }
  bool force_rename_fallback() const override { return fallback; }
  std::optional<std::uint64_t> fail_writes_after_bytes() const override { return fail_after; }
};

}  // namespace

void concurrent_pull_same_manifest_second_is_models_busy() {
  const auto root = scratch_root();
  auto first = secure_frontend::lock_models_root(root);
  expect(std::holds_alternative<secure_frontend::ModelsLock>(first), "first verb takes .lock");
  auto second = secure_frontend::lock_models_root(root);
  const auto* busy = std::get_if<secure_frontend::AcquireError>(&second);
  expect(busy != nullptr && busy->reason == secure_frontend::AcquireReason::models_busy, "second verb exits 7 models_busy");
  first = secure_frontend::AcquireError{secure_frontend::AcquireReason::usage_error, ""};  // releases the lock
  expect(std::holds_alternative<secure_frontend::ModelsLock>(secure_frontend::lock_models_root(root)), "released on destruction");
}

void part_file_create_append_resume_truncate() {
  const auto root = scratch_root();
  const secure_frontend::StoreEnvironment env;
  expect(!secure_frontend::ensure_directory(root, must_rel(".staging/k/weights")), "directories created within the root");
  const auto part = must_rel(".staging/k/weights/a.gguf.part");
  {
    auto created = secure_frontend::open_part_file(root, part, secure_frontend::PartOpen::create_new, env);
    auto& file = std::get<secure_frontend::PartFile>(created);
    expect(!file.preallocate(1000), "preallocation succeeds or is skipped");
    // F-35: the on-disk length, not the cached member, proves KEEP_SIZE.
    expect(file.size() == 0 && std::filesystem::file_size(root.join(part)) == 0, "KEEP_SIZE: preallocation does not change the length");
    expect(!file.append("hello") && file.size() == 5, "append");
    expect(!file.sync(), "fsync");
  }
  expect(std::holds_alternative<secure_frontend::AcquireError>(
             secure_frontend::open_part_file(root, part, secure_frontend::PartOpen::create_new, env)),
         "create_new refuses an existing file");
  auto reopened = secure_frontend::open_part_file(root, part, secure_frontend::PartOpen::open_existing, env);
  auto& file = std::get<secure_frontend::PartFile>(reopened);
  expect(file.size() == 5, "resume sees the length");
  expect(!file.append(" world") && file.size() == 11, "appends at the end");
  expect(!file.truncate_to_zero() && file.size() == 0, "restart from byte 0");
  file.close();
#if !defined(_WIN32)
  struct stat info {};
  expect(::stat(root.join(part).c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "part files are 0600");
#endif
}

void part_file_write_at_fills_offsets() {
  const auto root = scratch_root();
  const secure_frontend::StoreEnvironment env;
  expect(!secure_frontend::ensure_directory(root, must_rel(".staging/w/weights")), "dir");
  const auto part = must_rel(".staging/w/weights/p.gguf.part");
  {
    auto created = secure_frontend::open_part_file(root, part, secure_frontend::PartOpen::create_new, env);
    auto& file = std::get<secure_frontend::PartFile>(created);
    expect(!file.preallocate(11), "preallocate");
    expect(!file.write_at(6, "world"), "write the tail piece at its offset");
    expect(!file.write_at(0, "hello "), "write the head piece at its offset");
    expect(!file.sync(), "sync");
  }
  const auto got = secure_frontend::read_small_file(root, part, 100);
  expect(std::holds_alternative<std::string>(got) && std::get<std::string>(got) == "hello world",
         "the offsets assemble the whole file regardless of write order");
}

void rename_within_never_replaces() {
  const auto root = scratch_root();
  const secure_frontend::StoreEnvironment env;
  expect(!secure_frontend::write_new_file(root, must_rel("a"), "A") && !secure_frontend::write_new_file(root, must_rel("b"), "B"), "files");
  const auto refused = secure_frontend::rename_within(root, must_rel("a"), must_rel("b"), env);
  expect(refused && refused->reason == secure_frontend::AcquireReason::publish_failed, "an existing target is never replaced");
  expect(std::get<std::string>(secure_frontend::read_small_file(root, must_rel("b"), 10)) == "B", "target intact");
  expect(!secure_frontend::rename_within(root, must_rel("a"), must_rel("c"), env), "rename to a free name");
  expect(!secure_frontend::exists_within(root, must_rel("a")) && secure_frontend::exists_within(root, must_rel("c")), "moved");
}

void rename_fallback_never_replaces() {
  const auto root = scratch_root();
  TestEnvironment env;
  env.fallback = true;
  expect(!secure_frontend::ensure_directory(root, must_rel("src/inner")), "dir");
  expect(!secure_frontend::write_new_file(root, must_rel("dst"), "taken"), "target");
  expect(secure_frontend::rename_within(root, must_rel("src"), must_rel("dst"), env).has_value(), "fallback refuses an existing target");
  expect(!secure_frontend::rename_within(root, must_rel("src"), must_rel("moved"), env), "fallback renames a directory");
  expect(secure_frontend::exists_within(root, must_rel("moved/inner")), "directory moved with its content");
}

void remove_tree_removes_links_without_following() {
  const auto root = scratch_root();
  const auto outside = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(outside / "keep.txt", "keep");
  expect(!secure_frontend::ensure_directory(root, must_rel("t/sub")), "dirs");
  expect(!secure_frontend::write_new_file(root, must_rel("t/sub/f"), "x"), "file");
#if !defined(_WIN32)
  std::filesystem::create_directory_symlink(outside, root.join(must_rel("t/link")));
#endif
  expect(!secure_frontend::remove_tree_within(root, must_rel("t")), "tree removed bottom-up");
  expect(!secure_frontend::exists_within(root, must_rel("t")), "gone");
  expect(std::filesystem::exists(outside / "keep.txt"), "a link's target is never touched");
  expect(!secure_frontend::remove_tree_within(root, must_rel("t")), "absent is fine");
}

void hash_into_rejects_hard_links_and_counts_bytes() {
  const auto root = scratch_root();
  expect(!secure_frontend::write_new_file(root, must_rel("f"), "abc"), "file");
  secure_frontend::Sha256 hasher;
  const auto counted = secure_frontend::hash_into(root, must_rel("f"), hasher, 10);
  expect(std::get<std::uint64_t>(counted) == 3 && hasher.finish() == secure_frontend::sha256("abc"), "bytes hashed");
  secure_frontend::Sha256 limited;
  expect(std::holds_alternative<secure_frontend::AcquireError>(secure_frontend::hash_into(root, must_rel("f"), limited, 2)),
         "a file longer than max_bytes is refused");
#if !defined(_WIN32)
  // F-3: link() is __wur under _FORTIFY_SOURCE (release preset); check it.
  expect(::link(root.join(must_rel("f")).c_str(), root.join(must_rel("g")).c_str()) == 0, "hard link created");
  secure_frontend::Sha256 linked;
  expect(std::holds_alternative<secure_frontend::AcquireError>(secure_frontend::hash_into(root, must_rel("g"), linked, 10)),
         "a hard-linked file is refused");
  const auto listed = std::get<std::vector<secure_frontend::StagedEntry>>(secure_frontend::list_files_recursive(root));
  expect(listed.size() == 2 && !listed[0].regular && !listed[1].regular, "hard-linked files are listed as not regular");
#endif
}

// Final fix F1 (§8.3): a whole-file re-hash honours the interrupt flag between chunks.
void hash_into_stops_when_interrupted() {
  const auto root = scratch_root();
  const std::string two_chunks(secure_frontend::kSha256ReadChunkBytes + 1, 'b');
  expect(!secure_frontend::write_new_file(root, must_rel("big"), two_chunks), "two chunks");
  const std::atomic<bool> clear{false};
  secure_frontend::Sha256 whole;
  const auto counted = secure_frontend::hash_into(root, must_rel("big"), whole, ~std::uint64_t{0}, &clear);
  expect(std::holds_alternative<std::uint64_t>(counted) && std::get<std::uint64_t>(counted) == two_chunks.size(),
         "a clear flag hashes the whole file");
  const std::atomic<bool> set{true};
  secure_frontend::Sha256 stopped;
  const auto failure = secure_frontend::hash_into(root, must_rel("big"), stopped, ~std::uint64_t{0}, &set);
  expect(std::holds_alternative<secure_frontend::AcquireError>(failure) &&
             std::get<secure_frontend::AcquireError>(failure).reason == secure_frontend::AcquireReason::interrupted,
         "a set flag stops the hash with `interrupted`");
  expect(secure_frontend::exists_within(root, must_rel("big")), "the file is left in place");
}

void store_environment_seams_report_space_and_fs_kind() {
  const auto root = scratch_root();
  const secure_frontend::StoreEnvironment real;
  expect(std::get<std::uint64_t>(real.free_bytes(root)) > 0, "statvfs / GetDiskFreeSpaceExW");
  expect(std::holds_alternative<secure_frontend::FileSystemKind>(real.file_system_kind(root)), "statfs / volume information");
  TestEnvironment env;
  env.fail_after = 4;
  expect(!secure_frontend::ensure_directory(root, must_rel("d")), "dir");
  auto created = secure_frontend::open_part_file(root, must_rel("d/x.part"), secure_frontend::PartOpen::create_new, env);
  auto& file = std::get<secure_frontend::PartFile>(created);
  expect(!file.append("1234"), "within the simulated space");
  const auto full = file.append("5");
  expect(full && full->reason == secure_frontend::AcquireReason::disk_space && file.size() == 4, "ENOSPC keeps the .part");
}

// F-6: every component is P1-valid but the joined path exceeds P1's 1024 bytes
// (untrusted bundle media can be this deep). Listing and removal fail closed
// with an AcquireError; neither throws.
void overlong_joined_paths_fail_closed_without_throwing() {
  const auto root = scratch_root();
  const std::string level(200, 'a');
  const std::string deep = level + "/" + level + "/" + level + "/" + level + "/" + level;  // 1004 bytes
  expect(!secure_frontend::ensure_directory(root, must_rel(deep)), "1004-byte directory chain");
  secure_frontend_test::make_private_directory(root.join(must_rel(deep)) / std::string(30, 'b'));  // 1035 bytes joined
  expect(!secure_frontend::try_join_rel(must_rel(deep), std::string(30, 'b')).has_value(), "over-long join is nullopt");
  const auto listed = secure_frontend::list_files_recursive(root);
  const auto* list_error = std::get_if<secure_frontend::AcquireError>(&listed);
  expect(list_error != nullptr && list_error->reason == secure_frontend::AcquireReason::bundle_invalid,
         "listing an over-long tree is bundle_invalid");
  const auto removed = secure_frontend::remove_tree_within(root, must_rel(level));
  expect(removed && removed->reason == secure_frontend::AcquireReason::publish_failed, "removal of an over-long tree is publish_failed");
  expect(secure_frontend::exists_within(root, must_rel(deep)), "nothing past the failure point was assumed removed");
}

}  // namespace acquire_cases
