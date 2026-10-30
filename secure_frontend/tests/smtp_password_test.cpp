// Email alerts' old password, moved to the email plugin (plugin spec 2026-10-30, section 10): smtp-password is read as it
// was kept (DPAPI on Windows), moved to plugin-email-password and removed, once; a password the plugin already has stays;
// with none, nothing happens; the log line never holds the password.
#include "secure_frontend/plugin_secrets.hpp"
#include "secure_frontend/smtp_password.hpp"
#include "support/test_support.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

bool file_there(const std::filesystem::path& file) {
  std::error_code error;
  return std::filesystem::exists(file, error);
}

void the_old_password_moves_to_the_email_plugin() {
  const auto secret = secure_frontend_test::make_scratch_directory();
  expect(secure_frontend::store_smtp_password(secret, "line\nbreak").has_value(), "a password with a line break is refused");
  expect(!secure_frontend::store_smtp_password(secret, "mailbox pass").has_value(), "an old install's password is stored");
  const auto old = secure_frontend::read_smtp_password(secret);
  expect(old.has_value() && old->view() == "mailbox pass", "and reads back");
#if defined(_WIN32)
  expect(secure_frontend_test::read_file(secret / "smtp-password").find("mailbox pass") == std::string::npos, "never in the clear on disk");
#endif
  const auto moved = secure_frontend::move_smtp_password(secret);
  expect(moved.has_value() && moved->find("moved") != std::string::npos && moved->find("mailbox pass") == std::string::npos,
         "moved, said without the password: " + moved.value_or("(nothing)"));
  const auto now = secure_frontend::read_plugin_secret(secret, "email", "password");
  expect(now == std::optional<std::string>{"mailbox pass"}, "it is the email plugin's password now");
  expect(!file_there(secret / "smtp-password"), "the old file is gone");
  expect(!secure_frontend::move_smtp_password(secret).has_value(), "a second time, nothing to move");
}

void a_password_the_plugin_has_stays() {
  const auto secret = secure_frontend_test::make_scratch_directory();
  expect(!secure_frontend::store_plugin_secret(secret, "email", "password", "newer pass").has_value(), "the plugin has its own");
  expect(!secure_frontend::store_smtp_password(secret, "older pass").has_value(), "and an old file is there too");
  const auto moved = secure_frontend::move_smtp_password(secret);
  expect(moved.has_value() && moved->find("older pass") == std::string::npos, "said: " + moved.value_or("(nothing)"));
  expect(secure_frontend::read_plugin_secret(secret, "email", "password") == std::optional<std::string>{"newer pass"}, "the plugin's own stays");
  expect(!file_there(secret / "smtp-password"), "the old one is removed");
  const auto none = secure_frontend_test::make_scratch_directory();
  expect(!secure_frontend::move_smtp_password(none).has_value() && !secure_frontend::plugin_secret_set(none, "email", "password"),
         "with no old password, nothing happens");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"the_old_password_moves_to_the_email_plugin", the_old_password_moves_to_the_email_plugin},
      {"a_password_the_plugin_has_stays", a_password_the_plugin_has_stays},
  }, failures);
}
