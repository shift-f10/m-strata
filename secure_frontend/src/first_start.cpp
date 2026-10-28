#include "secure_frontend/first_start.hpp"

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/frontend_init.hpp"
#include "secure_frontend/local_folders.hpp"
#include "secure_frontend/token_store.hpp"

#include <cstdint>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#endif

namespace secure_frontend {
namespace {

constexpr std::string_view kConfigName = "config.json";
constexpr std::string_view kTokenName = "token";

ConfigDraftEntry path_text(const std::filesystem::path& path) { return ConfigDraftEntry{path_to_utf8(path), ConfigSource::file}; }
ConfigDraftEntry text(const std::string_view value) { return ConfigDraftEntry{std::string{value}, ConfigSource::file}; }
ConfigDraftEntry flag(const bool value) { return ConfigDraftEntry{value, ConfigSource::file}; }

std::optional<CanonicalRoot> private_folder(const std::filesystem::path& path) {
  auto opened = open_root(path, RootPolicy::create_private);
  if (auto* root = std::get_if<CanonicalRoot>(&opened)) return std::move(*root);
  return std::nullopt;
}

// The install folder is owner-only when it opens as a trusted root and its engine, if there is one, as a trusted
// executable (the checks the server runs on them at every start).
bool install_folder_is_private(const std::filesystem::path& install_dir) {
  if (!std::holds_alternative<CanonicalRoot>(open_root(install_dir, RootPolicy::existing))) return false;
  std::error_code ignored;
  const auto engine = bundled_engine(install_dir);
  return !std::filesystem::exists(engine, ignored) || std::holds_alternative<CanonicalFile>(open_executable(engine));
}

}  // namespace

DataFolder data_folder_layout(const std::filesystem::path& root) {
  return DataFolder{.root = root, .config_file = root / std::string{kConfigName}, .secret = root / "secret",
                    .state = root / "state", .downloads = root / "downloads", .models = root / "models", .logs = root / "logs", .cache = root / "cache"};
}

std::filesystem::path executable_directory() {
#if defined(_WIN32)
  std::wstring buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return {};
  buffer.resize(length);
  return std::filesystem::path(buffer).parent_path();
#else
  std::error_code ec;
  const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path{} : self.parent_path();
#endif
}

std::filesystem::path bundled_engine(const std::filesystem::path& install_dir) {
#if defined(_WIN32)
  return install_dir / "engine" / "multiengine.exe";
#else
  return install_dir / "engine" / "multiengine";
#endif
}

FrontendConfigDraft first_start_config(const DataFolder& folder, const std::filesystem::path& engine) {
  FrontendConfigDraft draft;
  auto& e = draft.entries;
  e["schema_version"] = ConfigDraftEntry{std::int64_t{1}, ConfigSource::file};
  e["listener.host"] = text("127.0.0.1");
  e["listener.port"] = ConfigDraftEntry{std::int64_t{8080}, ConfigSource::file};
  e["listener.web_ui"] = flag(true);
  e["listener.monitor"] = flag(true);
  e["listener.login_link"] = flag(true);
  e["token.source"] = text("file:" + std::string{kTokenName});
  e["roots.secret"] = path_text(folder.secret);
  e["roots.state"] = path_text(folder.state);
  e["roots.logs"] = path_text(folder.logs);
  e["roots.cache"] = path_text(folder.cache);
  e["models.cards"] = ConfigDraftEntry{std::vector<std::string>{}, ConfigSource::file};
  e["models.control"] = flag(true);
  e["models.downloads"] = flag(true);
  e["models.downloads_root"] = path_text(folder.downloads);
  e["models.default_root"] = path_text(folder.models);
  e["models.default_engine"] = path_text(engine);
  return draft;
}

std::variant<FirstStartResult, FirstStartError> ensure_data_folder(const DataFolder& folder,
                                                                  const std::filesystem::path& engine) {
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(folder.config_file, ec);
  if (std::filesystem::exists(status)) {
    if (!std::filesystem::is_regular_file(status)) return FirstStartError{"data/config.json is not a regular file"};
    return FirstStartResult{.created = false};
  }
  auto root = private_folder(folder.root);
  if (!root) return FirstStartError{"the data folder could not be created as an owner-only folder"};
  for (const auto& dir : {folder.secret, folder.state, folder.downloads, folder.models,
                          folder.logs, folder.cache}) {
    if (!private_folder(dir)) return FirstStartError{"a folder in the data folder could not be created owner-only"};
  }
  auto secret = private_folder(folder.secret);
  const auto token_name = parse_relative_path(kTokenName);
  const auto config_name = parse_relative_path(kConfigName);
  if (!secret || !std::holds_alternative<RelativePath>(token_name) || !std::holds_alternative<RelativePath>(config_name)) {
    return FirstStartError{"the data folder's secret folder could not be opened"};
  }
  // A start interrupted after the key and before the config keeps that key (the config is written last).
  if (!std::filesystem::exists(std::filesystem::symlink_status(folder.secret / std::string{kTokenName}, ec))) {
    if (auto failure = generate_token(*secret, std::get<RelativePath>(token_name), false)) {
      return FirstStartError{"a new API key could not be written: " + failure->message};
    }
  }
  const std::string config = render_config_document(first_start_config(folder, engine));
  if (auto failure = write_private_file(*root, std::get<RelativePath>(config_name), config, TokenStoreMode::create_new)) {
    return FirstStartError{"data/config.json could not be written: " + failure->message};
  }
  return FirstStartResult{.created = true};
}

std::variant<bool, FirstStartError> secure_install_folder(const std::filesystem::path& install_dir) {
  if (install_folder_is_private(install_dir)) return false;
  // the tree made owner-only (local_folders.hpp): icacls <dir> /inheritance:r /grant:r <user>:(OI)(CI)F, then
  // icacls <dir>\* /reset /T; POSIX go-w
  if (!make_tree_owner_only(install_dir)) return FirstStartError{"the install folder could not be made owner-only"};
  if (!install_folder_is_private(install_dir)) {
    return FirstStartError{"the install folder is still not owner-only (it must be owned by this user, SYSTEM or the "
                           "Administrators)"};
  }
  return true;
}

}  // namespace secure_frontend
