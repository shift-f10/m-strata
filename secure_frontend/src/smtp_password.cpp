#include "secure_frontend/smtp_password.hpp"

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/plugin_secrets.hpp"
#include "secure_frontend/token_store.hpp"

#include <algorithm>
#include <variant>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <dpapi.h>
#endif

namespace secure_frontend {
namespace {

#if defined(_WIN32)
constexpr std::string_view kDpapiMagic = "STRATA-SMTP-DPAPI1\n";
#endif
constexpr std::size_t kMaxPasswordBytes = 256;

bool password_ok(const std::string_view password) {
  return !password.empty() && password.size() <= kMaxPasswordBytes &&
         std::none_of(password.begin(), password.end(), [](const char c) { return c == '\r' || c == '\n' || c == '\0'; });
}

}  // namespace

std::optional<std::string> store_smtp_password(const std::filesystem::path& secret_dir, const std::string_view password) {
  if (!password_ok(password)) return std::string{"the password must be 1-256 characters without line breaks"};
  auto root = open_root(secret_dir, RootPolicy::existing);
  const auto file = parse_relative_path(kSmtpPasswordFile);
  if (!std::holds_alternative<CanonicalRoot>(root) || !std::holds_alternative<RelativePath>(file)) {
    return std::string{"the server's secret folder could not be opened"};
  }
  std::string content;
#if defined(_WIN32)
  DATA_BLOB input{static_cast<DWORD>(password.size()), reinterpret_cast<BYTE*>(const_cast<char*>(password.data()))};
  DATA_BLOB output{};
  if (CryptProtectData(&input, L"strata smtp password", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output) == 0) {
    return std::string{"the password could not be protected"};
  }
  content.assign(kDpapiMagic);
  content.append(reinterpret_cast<const char*>(output.pbData), output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
#else
  content.assign(password);
#endif
  const auto failure = write_private_file(std::get<CanonicalRoot>(root), std::get<RelativePath>(file), content, TokenStoreMode::replace);
  std::fill(content.begin(), content.end(), '\0');
  if (failure) return std::string{"the password could not be written to the secret folder"};
  return std::nullopt;
}

std::optional<SecretBytes> read_smtp_password(const std::filesystem::path& secret_dir) {
  auto root = open_root(secret_dir, RootPolicy::existing);
  const auto file = parse_relative_path(kSmtpPasswordFile);
  if (!std::holds_alternative<CanonicalRoot>(root) || !std::holds_alternative<RelativePath>(file)) return std::nullopt;
  auto opened = open_beneath(std::get<CanonicalRoot>(root), std::get<RelativePath>(file));
  if (!std::holds_alternative<OpenedFile>(opened)) return std::nullopt;
  auto read = std::get<OpenedFile>(opened).read_all(4096);
  if (!std::holds_alternative<std::string>(read)) return std::nullopt;
  std::string raw = std::get<std::string>(std::move(read));
  SecretBytes password;
  bool ok = false;
#if defined(_WIN32)
  if (raw.rfind(kDpapiMagic, 0) == 0U) {
    DATA_BLOB input{static_cast<DWORD>(raw.size() - kDpapiMagic.size()), reinterpret_cast<BYTE*>(raw.data() + kDpapiMagic.size())};
    DATA_BLOB output{};
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output) != 0) {
      const std::string_view plain{reinterpret_cast<const char*>(output.pbData), output.cbData};
      ok = password_ok(plain) && password.assign(plain);
      SecureZeroMemory(output.pbData, output.cbData);
      LocalFree(output.pbData);
    }
  }
#else
  ok = password_ok(raw) && password.assign(raw);
#endif
  std::fill(raw.begin(), raw.end(), '\0');
  if (!ok) return std::nullopt;
  return password;
}

std::optional<std::string> move_smtp_password(const std::filesystem::path& secret_dir) {
  auto root = open_root(secret_dir, RootPolicy::existing);
  if (!std::holds_alternative<CanonicalRoot>(root)) return std::nullopt;
  const auto old_file = std::get<CanonicalRoot>(root).path() / std::string{kSmtpPasswordFile};
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(old_file, ec);
  if (status.type() == std::filesystem::file_type::not_found) return std::nullopt;
  const auto remove_old = [&old_file] {
    std::error_code removed;
    return std::filesystem::remove(old_file, removed) && !removed;
  };
  if (plugin_secret_set(secret_dir, "email", "password")) {
    return remove_old() ? "[strata] plugins: the email plugin has its own password; the old smtp-password was removed"
                        : "[strata] plugins: the email plugin has its own password; the old smtp-password could not be removed";
  }
  auto password = read_smtp_password(secret_dir);
  if (!password) return "[strata] plugins: the old smtp-password could not be read; set the email plugin's password in the Plugins tab";
  if (const auto failure = store_plugin_secret(secret_dir, "email", "password", password->view())) {
    return "[strata] plugins: the old smtp-password was not moved to the email plugin (" + *failure + ")";
  }
  password->clear();
  return remove_old() ? "[strata] plugins: the SMTP password moved to the email plugin's secret"
                      : "[strata] plugins: the SMTP password moved to the email plugin's secret; the old smtp-password could not be removed";
}

}  // namespace secure_frontend
