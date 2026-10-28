#include "secure_frontend/secret_source.hpp"
#include "secure_frontend/token_reader.hpp"
#include "support/process_runner.hpp"
#include "support/test_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
int failures = 0;
void expect(const bool condition, const std::string_view message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
using secure_frontend::TokenReadErrorCode;
const std::string kToken = "file-token-0123456789abcdefghijklmnopqrstuv";  // 43 bytes

void set_variable(const char* name, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}
void unset_variable(const char* name) {
#if defined(_WIN32)
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

// A secret root the reader accepts: 0700 (POSIX) or a protected owner-only DACL (Windows).
std::filesystem::path secret_root() {
#if defined(_WIN32)
  const auto root = secure_frontend_test::make_scratch_directory() / "secret";
  secure_frontend_test::make_protected_directory(root);
  return root;
#else
  return secure_frontend_test::make_scratch_directory();
#endif
}
void write_token_file(const std::filesystem::path& path, const std::string_view bytes) {
#if defined(_WIN32)
  secure_frontend_test::write_protected_file(path, bytes);
#else
  secure_frontend_test::write_file(path, bytes, 0600);
#endif
}
secure_frontend::TokenReadResult read_file_source(const std::filesystem::path& root, const std::string_view path) {
  const auto source = std::get<secure_frontend::SecretSource>(secure_frontend::make_protected_file_secret_source(path));
  return secure_frontend::read_token(source, root);
}
secure_frontend::TokenReadResult read_env(const char* name) {
  const auto source = std::get<secure_frontend::SecretSource>(secure_frontend::make_environment_secret_source(name));
  return secure_frontend::read_token(source, std::filesystem::path{"."});
}
bool is_token(const secure_frontend::TokenReadResult& result, const std::string_view expected) {
  const auto* token = std::get_if<secure_frontend::SecretBytes>(&result);
  return token != nullptr && token->view() == expected;
}
bool is_error(const secure_frontend::TokenReadResult& result, const TokenReadErrorCode code) {
  const auto* error = std::get_if<secure_frontend::TokenReadError>(&result);
  return error != nullptr && error->code == code;
}

// ---- portable (environment) ------------------------------------------------
void reads_the_named_environment_variable() {
  set_variable("SF_TOKEN_READER_TEST", kToken);
  expect(is_token(read_env("SF_TOKEN_READER_TEST"), kToken), "environment source should be read");
  unset_variable("SF_TOKEN_READER_TEST");
}

void rejects_control_characters_in_environment_token() {
  set_variable("SF_TOKEN_READER_TEST", "bad\tvalue-0123456789abcdefghijklmnop");
  expect(is_error(read_env("SF_TOKEN_READER_TEST"), TokenReadErrorCode::invalid_token), "control byte in env token");
  unset_variable("SF_TOKEN_READER_TEST");
}

void auth02_short_operator_token_rejected() {
  set_variable("SF_TOKEN_READER_TEST", std::string(31, 'a'));
  expect(is_error(read_env("SF_TOKEN_READER_TEST"), TokenReadErrorCode::token_too_short), "31 bytes is too short");
  set_variable("SF_TOKEN_READER_TEST", std::string(32, 'a'));
  expect(is_token(read_env("SF_TOKEN_READER_TEST"), std::string(32, 'a')), "32 bytes is the floor");
  unset_variable("SF_TOKEN_READER_TEST");
}

// ---- protected files (Linux; windows-runtime on Windows) -------------------
void reads_an_owner_only_regular_file_below_the_root() {
  const auto root = secret_root();
  write_token_file(root / "token", kToken);
  expect(is_token(read_file_source(root, "token"), kToken), "owner-only regular file should be read");
}

void rejects_control_characters_in_file_token() {
  const auto root = secret_root();
  write_token_file(root / "token", "file-token\x01" + kToken);
  expect(is_error(read_file_source(root, "token"), TokenReadErrorCode::invalid_token), "control byte in file token");
}

void auth02_token_file_with_trailing_lf_accepted() {
  const auto root = secret_root();
  write_token_file(root / "token", kToken + "\n");
  expect(is_token(read_file_source(root, "token"), kToken), "one trailing LF is stripped (defect c)");
}

void auth02_token_file_with_trailing_crlf_accepted() {
  const auto root = secret_root();
  write_token_file(root / "token", kToken + "\r\n");
  expect(is_token(read_file_source(root, "token"), kToken), "one trailing CRLF is stripped");
}

void auth02_token_file_with_two_newlines_rejected() {
  const auto root = secret_root();
  write_token_file(root / "token", kToken + "\n\n");
  expect(is_error(read_file_source(root, "token"), TokenReadErrorCode::invalid_token), "two newlines are rejected");
}

void auth02_token_file_with_bom_rejected() {
  const auto root = secret_root();
  write_token_file(root / "token", "\xEF\xBB\xBF" + kToken);
  expect(is_error(read_file_source(root, "token"), TokenReadErrorCode::invalid_token), "a UTF-8 BOM is rejected");
}

void auth02_token_file_over_4k_rejected() {
  const auto root = secret_root();
  write_token_file(root / "token", std::string(4097, 'a'));
  expect(is_error(read_file_source(root, "token"), TokenReadErrorCode::token_too_large), "a file over 4 KiB is rejected");
  write_token_file(root / "long", std::string(257, 'a'));
  expect(is_error(read_file_source(root, "long"), TokenReadErrorCode::token_too_large), "a token over 256 bytes is rejected");
}

// Review Focus 3: what Notepad and copy-paste do to a token file.
void auth02_notepad_edits_rejected_with_reason() {
  const auto root = secret_root();
  const std::pair<std::string, const char*> cases[] = {
      {"\xEF\xBB\xBF" + kToken, "byte order mark"}, {kToken + " ", "whitespace"}, {kToken + " \r\n", "whitespace"},
      {"\"" + kToken + "\"", "quotes"},               {kToken + "\r\n\r\n", "newline"}};
  int index = 0;
  for (const auto& [bytes, reason] : cases) {
    const std::string name = "token" + std::to_string(index++);
    write_token_file(root / name, bytes);
    const auto result = read_file_source(root, name);
    const auto* error = std::get_if<secure_frontend::TokenReadError>(&result);
    expect(error != nullptr && error->code == TokenReadErrorCode::invalid_token &&
               error->message.find(reason) != std::string::npos,
           std::string{"Notepad-style edit must be rejected naming the "} + reason);
  }
  write_token_file(root / "ok", kToken + "\r\n");
  expect(is_token(read_file_source(root, "ok"), kToken), "Notepad's single CRLF is still fine");
}

#if !defined(_WIN32)
// ---- POSIX-only file semantics ----------------------------------------------
void rejects_a_group_or_world_accessible_file() {
  const auto root = secret_root();
  secure_frontend_test::write_file(root / "token", kToken, 0640);
  expect(is_error(read_file_source(root, "token"), TokenReadErrorCode::insecure_file_permissions), "group-readable token");
}

void rejects_a_symlink_that_escapes_the_root() {
  const auto root = secret_root();
  const auto outside = secret_root();
  write_token_file(outside / "token", kToken);
  std::filesystem::create_symlink(outside / "token", root / "token");
  expect(is_error(read_file_source(root, "token"), TokenReadErrorCode::symlink_not_allowed),
         "a symlinked token file is rejected, never followed");
}

void rejects_a_non_regular_file() {
  const auto root = secret_root();
  mkfifo((root / "token").c_str(), 0600);
  expect(is_error(read_file_source(root, "token"), TokenReadErrorCode::not_regular_file), "FIFO rejected before any read");
}

void cfg03_rejects_hardlinked_token_file() {
  const auto root = secret_root();
  write_token_file(root / "token", kToken);
  expect(link((root / "token").c_str(), (root / "second-name").c_str()) == 0, "test setup: link");
  expect(is_error(read_file_source(root, "token"), TokenReadErrorCode::hard_link_not_allowed),
         "a token file with a second hard link must be rejected");
}

// K8: the child is started with the token in its *initial* environment block
// (what /proc/<pid>/environ shows), reads it, then inspects itself.
void cfg05_env_token_absent_from_proc_environ() {
  const auto& args = secure_frontend_test::extra_arguments();
  if (!args.empty() && args.front() == "--child") {
    expect(is_token(read_env("SF_ENV_TOKEN"), kToken), "the child reads the token");
    expect(std::getenv("SF_ENV_TOKEN") == nullptr, "the variable is unset after reading");
    const std::string environ_block = secure_frontend_test::read_file("/proc/self/environ");
    expect(environ_block.find(kToken) == std::string::npos, "/proc/self/environ no longer contains the token");
    return;
  }
  const auto self = std::filesystem::read_symlink("/proc/self/exe");
  const auto result = secure_frontend_test::run_to_completion(secure_frontend_test::ProcessSpec{
      self, {"--case", "cfg05_env_token_absent_from_proc_environ", "--child"}, {{"SF_ENV_TOKEN", kToken}}});
  expect(result.exit_code == 0, "child check failed: " + result.err);
}
#endif
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"reads_the_named_environment_variable", reads_the_named_environment_variable},
      {"rejects_control_characters_in_environment_token", rejects_control_characters_in_environment_token},
      {"auth02_short_operator_token_rejected", auth02_short_operator_token_rejected},
      {"reads_an_owner_only_regular_file_below_the_root", reads_an_owner_only_regular_file_below_the_root},
      {"rejects_control_characters_in_file_token", rejects_control_characters_in_file_token},
      {"auth02_token_file_with_trailing_lf_accepted", auth02_token_file_with_trailing_lf_accepted},
      {"auth02_token_file_with_trailing_crlf_accepted", auth02_token_file_with_trailing_crlf_accepted},
      {"auth02_token_file_with_two_newlines_rejected", auth02_token_file_with_two_newlines_rejected},
      {"auth02_token_file_with_bom_rejected", auth02_token_file_with_bom_rejected},
      {"auth02_token_file_over_4k_rejected", auth02_token_file_over_4k_rejected},
      {"auth02_notepad_edits_rejected_with_reason", auth02_notepad_edits_rejected_with_reason},
#if !defined(_WIN32)
      {"rejects_a_group_or_world_accessible_file", rejects_a_group_or_world_accessible_file},
      {"rejects_a_symlink_that_escapes_the_root", rejects_a_symlink_that_escapes_the_root},
      {"rejects_a_non_regular_file", rejects_a_non_regular_file},
      {"cfg03_rejects_hardlinked_token_file", cfg03_rejects_hardlinked_token_file},
      {"cfg05_env_token_absent_from_proc_environ", cfg05_env_token_absent_from_proc_environ},
#endif
  }, failures);
}
