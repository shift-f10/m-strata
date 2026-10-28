#include "acquire_cases.hpp"
#include "secure_frontend/models_store.hpp"
#include "support/expect.hpp"
#include "support/test_support.hpp"

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"

#include <aclapi.h>
#include <sddl.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>

namespace acquire_cases {

using secure_frontend::must_rel;
using secure_frontend_test::expect;

namespace {

// Owner is the current user and the DACL is protected (what has_private_owner_dacl requires first).
bool owner_is_user_with_protected_dacl(const std::filesystem::path& path) {
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
                            &dacl, nullptr, &descriptor) != ERROR_SUCCESS) {
    return false;
  }
  bool ok = false;
  LPWSTR owner_text = nullptr;
  SECURITY_DESCRIPTOR_CONTROL control = 0U;
  DWORD revision = 0U;
  if (owner != nullptr && dacl != nullptr && ConvertSidToStringSidW(owner, &owner_text) != 0 &&
      GetSecurityDescriptorControl(descriptor, &control, &revision) != 0) {
    ok = std::wstring{owner_text} == secure_frontend_test::current_user_sid_string() && (control & SE_DACL_PROTECTED) != 0U;
  }
  if (owner_text != nullptr) LocalFree(owner_text);
  LocalFree(descriptor);
  return ok;
}

}  // namespace

// Parity with POSIX's st_uid/geteuid checks: an existing .lock or a resumed .part is
// adopted only when it is owned by the user under a protected owner-only DACL.
void store_adopts_only_private_files_windows() {
  auto opened = secure_frontend::open_root(secure_frontend_test::make_scratch_directory());
  const auto root = std::get<secure_frontend::CanonicalRoot>(std::move(opened));
  const secure_frontend::StoreEnvironment env;
  expect(!secure_frontend::ensure_directory(root, must_rel("s")), "directory");
  {
    auto created = secure_frontend::open_part_file(root, must_rel("s/new.part"), secure_frontend::PartOpen::create_new, env);
    expect(std::holds_alternative<secure_frontend::PartFile>(created), "part file created");
  }
  expect(owner_is_user_with_protected_dacl(root.join(must_rel("s/new.part"))), "a created .part is private (owner + protected DACL)");
  expect(std::holds_alternative<secure_frontend::PartFile>(
             secure_frontend::open_part_file(root, must_rel("s/new.part"), secure_frontend::PartOpen::open_existing, env)),
         "a private .part resumes");
  {
    auto lock = secure_frontend::lock_models_root(root);
    expect(std::holds_alternative<secure_frontend::ModelsLock>(lock), "lock created");
  }
  expect(owner_is_user_with_protected_dacl(root.join(must_rel(".lock"))), "a created .lock is private (owner + protected DACL)");
  expect(std::holds_alternative<secure_frontend::ModelsLock>(secure_frontend::lock_models_root(root)), "a private .lock is reused");

  const auto foreign_part = root.join(must_rel("s/foreign.part"));
  secure_frontend_test::write_file(foreign_part, "abc");  // inherited, unprotected DACL
  expect(secure_frontend_test::grant_everyone_write(foreign_part), "Everyone may write the planted .part");
  const auto resumed = secure_frontend::open_part_file(root, must_rel("s/foreign.part"), secure_frontend::PartOpen::open_existing, env);
  const auto* part_error = std::get_if<secure_frontend::AcquireError>(&resumed);
  expect(part_error != nullptr && part_error->reason == secure_frontend::AcquireReason::publish_failed,
         "a non-private .part is never resumed");

  auto other = secure_frontend::open_root(secure_frontend_test::make_scratch_directory());
  const auto other_root = std::get<secure_frontend::CanonicalRoot>(std::move(other));
  const auto foreign_lock = other_root.join(must_rel(".lock"));
  secure_frontend_test::write_file(foreign_lock, "");
  expect(secure_frontend_test::grant_everyone_write(foreign_lock), "Everyone may write the planted .lock");
  const auto locked = secure_frontend::lock_models_root(other_root);
  const auto* lock_error = std::get_if<secure_frontend::AcquireError>(&locked);
  expect(lock_error != nullptr && lock_error->reason == secure_frontend::AcquireReason::publish_failed,
         "a non-private .lock is never adopted");
}

void store_primitives_windows() {
  auto opened = secure_frontend::open_root(secure_frontend_test::make_scratch_directory());
  const auto root = std::get<secure_frontend::CanonicalRoot>(std::move(opened));
  const secure_frontend::StoreEnvironment env;
  auto lock = secure_frontend::lock_models_root(root);
  expect(std::holds_alternative<secure_frontend::ModelsLock>(lock), "LockFileEx exclusive");
  const auto busy = secure_frontend::lock_models_root(root);
  expect(std::holds_alternative<secure_frontend::AcquireError>(busy) &&
             std::get<secure_frontend::AcquireError>(busy).reason == secure_frontend::AcquireReason::models_busy,
         "LOCKFILE_FAIL_IMMEDIATELY → models_busy");
  expect(!secure_frontend::ensure_directory(root, must_rel("v/w")), "private directories");
  {
    auto part = secure_frontend::open_part_file(root, must_rel("v/w/a.part"), secure_frontend::PartOpen::create_new, env);
    auto& file = std::get<secure_frontend::PartFile>(part);
    expect(!file.preallocate(4096) && file.size() == 0, "FileAllocationInfo keeps the length");
    expect(!file.append("abc") && !file.sync() && file.size() == 3, "append + FlushFileBuffers");
    expect(!file.truncate_to_zero() && !file.append("xy") && file.size() == 2, "truncate then append");
  }
  expect(!secure_frontend::write_new_file(root, must_rel("v/w/b"), "B"), "second file");
  expect(secure_frontend::rename_within(root, must_rel("v/w/a.part"), must_rel("v/w/b"), env).has_value(), "no replace");
  expect(!secure_frontend::rename_within(root, must_rel("v/w/a.part"), must_rel("v/w/a"), env), "rename");
  // A process holding the file without FILE_SHARE_DELETE makes deletion fail after the retries.
  const auto held_path = root.join(must_rel("v/w/a"));
  HANDLE held = CreateFileW(held_path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  expect(held != INVALID_HANDLE_VALUE, "engine-like open handle");
  expect(secure_frontend::remove_file_within(root, must_rel("v/w/a")).has_value(), "sharing violation → error after retries");
  CloseHandle(held);
  const auto outside = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(outside / "keep.txt", "keep");
  expect(secure_frontend_test::create_junction(root.join(must_rel("v/j")), outside), "junction");
  expect(!secure_frontend::remove_tree_within(root, must_rel("v")), "tree removed; the junction itself, never its target");
  expect(std::filesystem::exists(outside / "keep.txt") && !secure_frontend::exists_within(root, must_rel("v")), "target intact");
  expect(std::holds_alternative<std::uint64_t>(env.free_bytes(root)), "GetDiskFreeSpaceExW");
  expect(std::get<secure_frontend::FileSystemKind>(env.file_system_kind(root)) == secure_frontend::FileSystemKind::other, "NTFS");
}

}  // namespace acquire_cases
#endif
