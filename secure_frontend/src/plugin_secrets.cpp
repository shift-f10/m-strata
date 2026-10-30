#include "secure_frontend/plugin_secrets.hpp"

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/token_store.hpp"

#include <algorithm>
#include <system_error>
#include <variant>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <dpapi.h>
#endif

namespace secure_frontend {
namespace {

#if defined(_WIN32)
constexpr std::string_view kDpapiMagic = "STRATA-PLUGIN-DPAPI1\n";
#endif

bool plugin_name(const std::string_view text) noexcept {
  return !text.empty() && text.size() <= 32U && text.front() >= 'a' && text.front() <= 'z' &&
         std::all_of(text.begin(), text.end(), [](const char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

bool value_ok(const std::string_view value) noexcept {
  return !value.empty() && value.size() <= kMaxPluginSecretBytes &&
         std::all_of(value.begin(), value.end(), [](const char c) { return c >= 0x20 && c < 0x7F; });
}

}  // namespace

std::optional<std::string> plugin_secret_file(const std::string_view id, const std::string_view name) {
  if (!plugin_name(id) || !plugin_name(name)) return std::nullopt;
  return "plugin-" + std::string{id} + "-" + std::string{name};
}

std::optional<std::string> store_plugin_secret(const std::filesystem::path& secret_dir, const std::string_view id,
                                               const std::string_view name, const std::string_view value) {
  const auto file_name = plugin_secret_file(id, name);
  if (!file_name) return std::string{"not a plugin's setting"};
  if (!value_ok(value)) return std::string{"a secret must be 1-1024 printable characters"};
  auto root = open_root(secret_dir, RootPolicy::existing);
  const auto file = parse_relative_path(*file_name);
  if (!std::holds_alternative<CanonicalRoot>(root) || !std::holds_alternative<RelativePath>(file)) {
    return std::string{"the server's secret folder could not be opened"};
  }
  std::string content;
#if defined(_WIN32)
  DATA_BLOB input{static_cast<DWORD>(value.size()), reinterpret_cast<BYTE*>(const_cast<char*>(value.data()))};
  DATA_BLOB output{};
  if (CryptProtectData(&input, L"strata plugin secret", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output) == 0) {
    return std::string{"the secret could not be protected"};
  }
  content.assign(kDpapiMagic);
  content.append(reinterpret_cast<const char*>(output.pbData), output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
#else
  content.assign(value);
#endif
  const auto failure = write_private_file(std::get<CanonicalRoot>(root), std::get<RelativePath>(file), content, TokenStoreMode::replace);
  std::fill(content.begin(), content.end(), '\0');
  if (failure) return std::string{"the secret could not be written to the server's secret folder"};
  return std::nullopt;
}

std::optional<std::string> read_plugin_secret(const std::filesystem::path& secret_dir, const std::string_view id,
                                              const std::string_view name) {
  const auto file_name = plugin_secret_file(id, name);
  if (!file_name) return std::nullopt;
  auto root = open_root(secret_dir, RootPolicy::existing);
  const auto file = parse_relative_path(*file_name);
  if (!std::holds_alternative<CanonicalRoot>(root) || !std::holds_alternative<RelativePath>(file)) return std::nullopt;
  auto opened = open_beneath(std::get<CanonicalRoot>(root), std::get<RelativePath>(file));
  if (!std::holds_alternative<OpenedFile>(opened)) return std::nullopt;
  auto read = std::get<OpenedFile>(opened).read_all(8192);
  if (!std::holds_alternative<std::string>(read)) return std::nullopt;
  std::string raw = std::get<std::string>(std::move(read));
  std::optional<std::string> value;
#if defined(_WIN32)
  if (raw.rfind(kDpapiMagic, 0) == 0U) {
    DATA_BLOB input{static_cast<DWORD>(raw.size() - kDpapiMagic.size()), reinterpret_cast<BYTE*>(raw.data() + kDpapiMagic.size())};
    DATA_BLOB output{};
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output) != 0) {
      const std::string_view plain{reinterpret_cast<const char*>(output.pbData), output.cbData};
      if (value_ok(plain)) value = std::string{plain};
      SecureZeroMemory(output.pbData, output.cbData);
      LocalFree(output.pbData);
    }
  }
#else
  if (value_ok(raw)) value = raw;
#endif
  std::fill(raw.begin(), raw.end(), '\0');
  return value;
}

bool plugin_secret_set(const std::filesystem::path& secret_dir, const std::string_view id, const std::string_view name) {
  auto value = read_plugin_secret(secret_dir, id, name);
  if (!value) return false;
  std::fill(value->begin(), value->end(), '\0');
  return true;
}

bool remove_plugin_secret(const std::filesystem::path& secret_dir, const std::string_view id, const std::string_view name) {
  const auto file_name = plugin_secret_file(id, name);
  if (!file_name) return false;
  auto root = open_root(secret_dir, RootPolicy::existing);
  if (!std::holds_alternative<CanonicalRoot>(root)) return false;
  const auto path = std::get<CanonicalRoot>(root).path() / *file_name;
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(path, ec);
  if (status.type() == std::filesystem::file_type::not_found) return true;
  if (ec || !std::filesystem::is_regular_file(status)) return false;
  return std::filesystem::remove(path, ec) && !ec;
}

}  // namespace secure_frontend
