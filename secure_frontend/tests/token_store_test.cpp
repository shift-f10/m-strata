#include "secure_frontend/token_generator.hpp"
#include "secure_frontend/token_reader.hpp"
#include "secure_frontend/token_store.hpp"
#include "support/test_support.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <variant>

#if defined(_WIN32)
#include <aclapi.h>
#else
#include <sys/stat.h>
#endif

namespace {
int failures = 0;
// The helpers are [[maybe_unused]] so either platform's case set alone compiles under -Werror.
[[maybe_unused]] void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

[[maybe_unused]] secure_frontend::CanonicalRoot root_at(const std::filesystem::path& path) {
  return std::get<secure_frontend::CanonicalRoot>(secure_frontend::open_root(path));
}
[[maybe_unused]] secure_frontend::RelativePath rel(const char* text) {
  return std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path(text));
}
[[maybe_unused]] secure_frontend::SecretBytes fresh_token() {
  return std::get<secure_frontend::SecretBytes>(secure_frontend::generate_token());
}

#if !defined(_WIN32)
void token_store_creates_0600_file_in_0700_dir() {
  const auto dir = secure_frontend_test::make_scratch_directory();
  const auto token = fresh_token();
  expect(!secure_frontend::store_token(root_at(dir), rel("token"), token, secure_frontend::TokenStoreMode::create_new),
         "a token is stored in a 0700 directory");
  struct stat st {};
  stat((dir / "token").c_str(), &st);
  expect((st.st_mode & 0777) == 0600 && st.st_uid == geteuid(), "the file is 0600 and owned by the user");
  expect(secure_frontend_test::read_file(dir / "token") == std::string{token.view()} + "\n", "token plus one LF");
  std::size_t entries = 0;
  for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(dir)) ++entries;
  expect(entries == 1U, "no temporary file is left behind");
  const auto again = secure_frontend::store_token(root_at(dir), rel("token"), fresh_token(), secure_frontend::TokenStoreMode::create_new);
  expect(again.has_value() && again->code == secure_frontend::TokenStoreErrorCode::already_exists, "create_new never overwrites");
  const auto shared = secure_frontend_test::make_scratch_directory();
  chmod(shared.c_str(), 0750);
  const auto unsafe = secure_frontend::store_token(root_at(shared), rel("token"), token, secure_frontend::TokenStoreMode::create_new);
  expect(unsafe.has_value() && unsafe->code == secure_frontend::TokenStoreErrorCode::unsafe_location,
         "a directory with group access is refused");
  // A holding directory that is owner-only but lacks the search bit opens (read
  // access suffices), yet fstatat inside it fails with EACCES, not ENOENT; that
  // must fail closed as unsafe_location rather than count as "absent".
  const auto outer = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::make_private_directory(outer / "sub");
  chmod((outer / "sub").c_str(), 0600);
  const auto blind = secure_frontend::store_token(root_at(outer), rel("sub/token"), token, secure_frontend::TokenStoreMode::replace);
  chmod((outer / "sub").c_str(), 0700);
  expect(blind.has_value() && blind->code == secure_frontend::TokenStoreErrorCode::unsafe_location,
         "a target that cannot be inspected is refused (only ENOENT counts as absent)");
}

void token_store_rotate_is_atomic() {
  const auto dir = secure_frontend_test::make_scratch_directory();
  const auto first = fresh_token();
  const auto second = fresh_token();
  const std::string a = std::string{first.view()} + "\n";
  const std::string b = std::string{second.view()} + "\n";
  expect(!secure_frontend::store_token(root_at(dir), rel("token"), first, secure_frontend::TokenStoreMode::create_new), "initial");
  expect(!secure_frontend::store_token(root_at(dir), rel("token"), second, secure_frontend::TokenStoreMode::replace),
         "a replace succeeds");
  expect(secure_frontend_test::read_file(dir / "token") == b, "a replace puts the new token in place");
  std::atomic<bool> stop{false};
  std::atomic<int> torn{0};
  std::atomic<long> reads{0};
  std::thread reader([&] {
    while (!stop) {
      const std::string seen = secure_frontend_test::read_file(dir / "token");
      if (seen != a && seen != b) ++torn;
      ++reads;
    }
  });
  int failed_replaces = 0;
  for (int i = 0; i < 200; ++i) {
    if (secure_frontend::store_token(root_at(dir), rel("token"), i % 2 == 0 ? first : second,
                                     secure_frontend::TokenStoreMode::replace)) {
      ++failed_replaces;
    }
  }
  stop = true;
  reader.join();
  expect(failed_replaces == 0, "every replace succeeds");
  expect(reads > 0, "the concurrent reader ran");
  expect(torn == 0, "a concurrent reader always sees the old or the new token, never a partial file");
  // 200 alternating writes starting with `first` end on `second`.
  expect(secure_frontend_test::read_file(dir / "token") == b, "the last replace is the content on disk");
  std::size_t entries = 0;
  for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(dir)) ++entries;
  expect(entries == 1U, "no temporary file is left behind after replaces");
}

void token_store_refuses_symlinked_target() {
  const auto dir = secure_frontend_test::make_scratch_directory();
  const auto outside = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(outside / "victim", "unchanged");
  std::filesystem::create_symlink(outside / "victim", dir / "token");
  const auto result = secure_frontend::store_token(root_at(dir), rel("token"), fresh_token(), secure_frontend::TokenStoreMode::replace);
  expect(result.has_value() && result->code == secure_frontend::TokenStoreErrorCode::unsafe_location, "a symlinked target is refused");
  expect(secure_frontend_test::read_file(outside / "victim") == "unchanged", "the link target is untouched");
  expect(std::filesystem::is_symlink(dir / "token"), "the symlink itself is not replaced either");
}

// Defect (c): store_token writes the token plus one LF; the reader must accept
// exactly that file and hand back the token without the newline.
void token_store_round_trips_through_reader() {
  const auto dir = secure_frontend_test::make_scratch_directory();
  const auto token = fresh_token();
  expect(!secure_frontend::store_token(root_at(dir), rel("token"), token, secure_frontend::TokenStoreMode::create_new), "stored");
  const auto source = std::get<secure_frontend::SecretSource>(secure_frontend::make_protected_file_secret_source("token"));
  const auto read = secure_frontend::read_token(source, dir);
  const auto* error = std::get_if<secure_frontend::TokenReadError>(&read);
  expect(error == nullptr, "a stored token reads back: " + (error ? error->message : std::string{}));
  const auto* value = std::get_if<secure_frontend::SecretBytes>(&read);
  expect(value != nullptr && value->view() == token.view(), "the value read back equals the stored token, without the LF");
}
#endif

#if defined(_WIN32)
std::filesystem::path protected_secret_dir() {
  const auto dir = secure_frontend_test::make_scratch_directory() / "secret";
  secure_frontend_test::make_protected_directory(dir);
  return dir;
}

void token_store_windows_round_trips_through_dpapi() {
  const auto dir = protected_secret_dir();
  const auto token = fresh_token();
  const auto stored = secure_frontend::store_token(root_at(dir), rel("token"), token, secure_frontend::TokenStoreMode::create_new);
  expect(!stored.has_value(), "the token is stored: " + (stored ? stored->message : std::string{}));
  const std::string raw = secure_frontend_test::read_file(dir / "token");
  expect(raw.rfind("STRATAK1", 0) == 0, "the file starts with the STRATAK1 magic");
  expect(raw.find(std::string{token.view()}) == std::string::npos, "the token is not stored in plaintext");
  const auto source = std::get<secure_frontend::SecretSource>(secure_frontend::make_protected_file_secret_source("token"));
  const auto read = secure_frontend::read_token(source, dir);
  const auto* error = std::get_if<secure_frontend::TokenReadError>(&read);
  expect(error == nullptr, "the DPAPI file reads back: " + (error ? error->message : std::string{}));
  const auto* value = std::get_if<secure_frontend::SecretBytes>(&read);
  expect(value != nullptr && value->view() == token.view(), "the value read back is the token that was stored");
}

void token_store_windows_file_has_private_dacl() {
  const auto dir = protected_secret_dir();
  expect(!secure_frontend::store_token(root_at(dir), rel("token"), fresh_token(), secure_frontend::TokenStoreMode::create_new),
         "stored");
  // Checked independently of windows_security: owner is the current user, the
  // DACL is protected, and it holds exactly two allow-ACEs (user and SYSTEM).
  HANDLE file = CreateFileW((L"\\\\?\\" + std::filesystem::absolute(dir / "token").wstring()).c_str(), READ_CONTROL,
                            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) { expect(false, "the stored file opens for READ_CONTROL"); return; }
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD status = GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                                       nullptr, &dacl, nullptr, &descriptor);
  CloseHandle(file);
  if (status != ERROR_SUCCESS || descriptor == nullptr) { expect(false, "security info is readable"); return; }
  const auto sid_text = [](PSID sid) {
    LPWSTR text = nullptr;
    if (sid == nullptr || ConvertSidToStringSidW(sid, &text) == 0) return std::wstring{};
    std::wstring out{text};
    LocalFree(text);
    return out;
  };
  const std::wstring user = secure_frontend_test::current_user_sid_string();
  expect(!user.empty() && sid_text(owner) == user, "the owner is the current user");
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  GetSecurityDescriptorControl(descriptor, &control, &revision);
  expect((control & SE_DACL_PROTECTED) != 0, "the DACL is protected (nothing inherited)");
  expect(dacl != nullptr && dacl->AceCount == 2, "exactly two ACEs: the user and SYSTEM");
  bool user_seen = false;
  bool system_seen = false;
  for (DWORD i = 0; dacl != nullptr && i < dacl->AceCount; ++i) {
    void* raw = nullptr;
    if (GetAce(dacl, i, &raw) == 0) continue;
    const auto* header = static_cast<const ACE_HEADER*>(raw);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
    auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
    const std::wstring sid = sid_text(reinterpret_cast<PSID>(&ace->SidStart));
    user_seen = user_seen || sid == user;
    system_seen = system_seen || sid == L"S-1-5-18";
  }
  expect(user_seen && system_seen, "the allow-ACEs name the user and SYSTEM");
  LocalFree(descriptor);
}

void token_reader_windows_accepts_plaintext_under_private_dacl() {
  const auto dir = protected_secret_dir();
  const std::string token = "operator-token-0123456789abcdefghijklmnop";
  secure_frontend_test::write_protected_file(dir / "token", token);
  const auto source = std::get<secure_frontend::SecretSource>(secure_frontend::make_protected_file_secret_source("token"));
  const auto read = secure_frontend::read_token(source, dir);
  expect(!std::holds_alternative<secure_frontend::TokenReadError>(read), "an operator-made plaintext file is accepted");
  const auto* value = std::get_if<secure_frontend::SecretBytes>(&read);
  expect(value != nullptr && value->view() == token, "the plaintext value read back is the operator's token");
}
#endif
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
#if !defined(_WIN32)
      {"token_store_creates_0600_file_in_0700_dir", token_store_creates_0600_file_in_0700_dir},
      {"token_store_rotate_is_atomic", token_store_rotate_is_atomic},
      {"token_store_refuses_symlinked_target", token_store_refuses_symlinked_target},
      {"token_store_round_trips_through_reader", token_store_round_trips_through_reader},
#else
      {"token_store_windows_round_trips_through_dpapi", token_store_windows_round_trips_through_dpapi},
      {"token_store_windows_file_has_private_dacl", token_store_windows_file_has_private_dacl},
      {"token_reader_windows_accepts_plaintext_under_private_dacl", token_reader_windows_accepts_plaintext_under_private_dacl},
#endif
  }, failures);
}
