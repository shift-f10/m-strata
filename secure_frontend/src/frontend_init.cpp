#include "secure_frontend/frontend_init.hpp"

#include "secure_frontend/config_file.hpp"
#include "secure_frontend/token_generator.hpp"

#include <cstdint>
#include <system_error>
#include <utility>
#include <vector>
#include <variant>

namespace secure_frontend {

std::optional<TokenStoreError> write_default_config(const CanonicalRoot& dir, const RelativePath& file,
                                                    const std::string_view json, const bool force) {
  return write_private_file(dir, file, json, force ? TokenStoreMode::replace : TokenStoreMode::create_new);
}

std::optional<TokenStoreError> generate_token(const CanonicalRoot& secret_root, const RelativePath& file, const bool force) {
  auto generated = secure_frontend::generate_token();
  if (const auto* failure = std::get_if<TokenGenerationError>(&generated)) {
    return TokenStoreError{TokenStoreErrorCode::write_failed, failure->message};
  }
  return store_token(secret_root, file, std::get<SecretBytes>(generated),
                     force ? TokenStoreMode::replace : TokenStoreMode::create_new);
}

std::optional<InitError> init_frontend(const std::filesystem::path& config_file, const FrontendConfigDraft& flags,
                                       const bool force) {
  std::error_code ignored;
  const auto absolute = std::filesystem::absolute(config_file, ignored);
  const auto config_name = parse_relative_path(path_to_utf8(absolute.filename()));
  if (!std::holds_alternative<RelativePath>(config_name)) {
    return InitError{InitErrorCode::invalid_config, "the config file name is not portable", {}};
  }
  FrontendConfigDraft draft = flags;
  draft.entries["schema_version"] = ConfigDraftEntry{std::int64_t{1}, ConfigSource::command_line};
  // Validate before creating anything, so a bad option leaves no directory
  // behind. validate() has no side effects. While the config directory does not
  // exist yet, the one issue it must produce -- an unset roots.secret that would
  // default to that directory -- is expected and settled by the full check below.
  const bool directory_exists = std::filesystem::is_directory(absolute.parent_path(), ignored);
  if (directory_exists) draft.config_directory = absolute.parent_path();
  if (auto early = validate(draft); auto* issues = std::get_if<ConfigErrors>(&early)) {
    std::erase_if(*issues, [directory_exists](const ConfigIssue& issue) {
      return !directory_exists && issue.key_path == "roots.secret" && issue.code == ConfigIssueCode::missing_required;
    });
    if (!issues->empty()) {
      return InitError{InitErrorCode::invalid_config, "the configuration is not valid", std::move(*issues)};
    }
  }
  auto directory = open_root(absolute.parent_path(), RootPolicy::create_private);
  if (const auto* failure = std::get_if<PathError>(&directory)) {
    return InitError{InitErrorCode::write_failed, "the config directory: " + failure->message, {}};
  }
  const auto& config_dir = std::get<CanonicalRoot>(directory);
  draft.config_directory = config_dir.path();
  auto validated = validate(draft);
  if (auto* issues = std::get_if<ConfigErrors>(&validated)) {
    return InitError{InitErrorCode::invalid_config, "the configuration is not valid", std::move(*issues)};
  }
  const FrontendConfig& config = std::get<ValidatedConfig>(validated).get();
  const bool needs_token = config.token.kind() == SecretSourceKind::protected_file;
  const RelativePath& config_rel = std::get<RelativePath>(config_name);
  const auto token_rel = parse_relative_path(config.token.location());
  // Refused before anything is written: with --force the token is replaced
  // first, so a config target that can never be replaced must not rotate it.
  const auto config_status = std::filesystem::symlink_status(config_dir.join(config_rel), ignored);
  if (std::filesystem::exists(config_status) && !std::filesystem::is_regular_file(config_status)) {
    return InitError{InitErrorCode::write_failed, "the config file path exists and is not a regular file", {}};
  }
  if (!force) {
    if (std::filesystem::exists(config_status)) {
      return InitError{InitErrorCode::config_exists, "the config file already exists; use --force to replace it", {}};
    }
    if (needs_token && std::filesystem::exists(std::filesystem::symlink_status(config.roots.secret->join(std::get<RelativePath>(token_rel)), ignored))) {
      return InitError{InitErrorCode::token_exists, "the token file already exists; use --force to rotate it", {}};
    }
  }
  if (needs_token) {
    if (auto failure = generate_token(*config.roots.secret, std::get<RelativePath>(token_rel), force)) {
      return InitError{failure->code == TokenStoreErrorCode::already_exists ? InitErrorCode::token_exists : InitErrorCode::token_failed,
                       "the token: " + failure->message, {}};
    }
  }
  FrontendConfigDraft written = flags;  // only what the operator chose, plus the version
  written.entries["schema_version"] = ConfigDraftEntry{std::int64_t{1}, ConfigSource::command_line};
  if (auto failure = write_default_config(config_dir, config_rel, render_config_document(written), force)) {
    return InitError{failure->code == TokenStoreErrorCode::already_exists ? InitErrorCode::config_exists : InitErrorCode::write_failed,
                     "the config file: " + failure->message, {}};
  }
  // init, like serve, creates the optional logs/cache roots (Task 14); validate() above only checked them.
  if (auto issues = create_runtime_roots(config); !issues.empty()) {
    return InitError{InitErrorCode::write_failed, "a runtime directory could not be created", std::move(issues)};
  }
  return std::nullopt;
}

}  // namespace secure_frontend
