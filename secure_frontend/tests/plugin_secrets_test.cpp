// A tool's or notifier's secret settings (the owner, 2026-10-30, decision 6; plugin spec section 10): kept in the secret
// folder as plugin-<id>-<name> - Windows DPAPI, elsewhere 0600 - read back for that plugin's process only, and removed.
#include "secure_frontend/plugin_secrets.hpp"
#include "support/test_support.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

void a_secret_is_stored_read_and_removed() {
  const auto secret = secure_frontend_test::make_scratch_directory();
  expect(!secure_frontend::plugin_secret_set(secret, "email", "password") && !secure_frontend::read_plugin_secret(secret, "email", "password"),
         "none stored yet");
  expect(!secure_frontend::store_plugin_secret(secret, "email", "password", "hunter2 with spaces").has_value(), "stored");
  expect(secure_frontend::plugin_secret_set(secret, "email", "password"), "set");
  const auto back = secure_frontend::read_plugin_secret(secret, "email", "password");
  expect(back.has_value() && *back == "hunter2 with spaces", "read back as it was");
  std::ifstream in(secret / "plugin-email-password", std::ios::binary);
  const std::string raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  in.close();
#if defined(_WIN32)
  expect(!raw.empty() && raw.find("hunter2") == std::string::npos, "protected by DPAPI on disk");
#else
  struct stat info {};
  expect(::stat((secret / "plugin-email-password").c_str(), &info) == 0 && (info.st_mode & 0077) == 0, "owner-only on disk");
#endif
  expect(!secure_frontend::store_plugin_secret(secret, "email", "password", "another").has_value() &&
             secure_frontend::read_plugin_secret(secret, "email", "password") == std::string{"another"},
         "replaced");
  expect(secure_frontend::remove_plugin_secret(secret, "email", "password") && !secure_frontend::plugin_secret_set(secret, "email", "password"),
         "removed");
  expect(secure_frontend::remove_plugin_secret(secret, "email", "password"), "removing one not there is no failure");
  // refused: an empty value, a line break, too long, an id or a name that is not one
  for (const std::string& bad : {std::string{}, std::string{"a\nb"}, std::string(1025U, 'x'), std::string{"tab\there"}}) {
    expect(secure_frontend::store_plugin_secret(secret, "email", "password", bad).has_value(), "refused value of " + std::to_string(bad.size()));
  }
  expect(secure_frontend::store_plugin_secret(secret, "../x", "password", "v").has_value() &&
             secure_frontend::store_plugin_secret(secret, "email", "Pass", "v").has_value() && !secure_frontend::plugin_secret_file("a", "../b"),
         "an id or a name that is not one");
  expect(secure_frontend::plugin_secret_file("email", "password") == std::string{"plugin-email-password"}, "its file name");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"a_secret_is_stored_read_and_removed", a_secret_is_stored_read_and_removed},
  }, failures);
}
