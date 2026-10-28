#include "secure_frontend/model_cards.hpp"

#include "secure_frontend/config_file.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/model_inventory.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <utility>

namespace secure_frontend {
namespace {

// The config file's bounds, two levels deeper: the card's own object and its config/options
// members sit above the config rows (card -> config -> defaults -> sampling -> row).
constexpr JsonLimits kCardJsonLimits{.max_bytes = kMaxConfigFileBytes, .max_depth = 6U, .max_container_entries = 256U,
                                     .max_string_bytes = 4096U, .allow_fractions = true};

void add(ConfigErrors& errors, std::string key, const ConfigIssueCode code, std::string reason) {
  errors.push_back(ConfigIssue{std::move(key), code, std::move(reason)});
}

bool starts_with(const std::string_view text, const std::string_view prefix) noexcept {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

// The served-name grammar of model.name (frontend_config.cpp is_valid_model_alias): one model
// id of the inventory grammar, at most 64 characters.
bool valid_card_name(const std::string_view name) {
  return !name.empty() && name.size() <= 64U && std::holds_alternative<std::string>(validate_model_id(name));
}

// "folder", or "download:" and a download catalog id ([a-z0-9._-]{1,64}, download_catalog.hpp).
bool valid_card_source(const std::string_view source) noexcept {
  if (source == "folder") return true;
  constexpr std::string_view kDownload = "download:";
  if (!starts_with(source, kDownload)) return false;
  const auto id = source.substr(kDownload.size());
  return !id.empty() && id.size() <= 64U && std::all_of(id.begin(), id.end(), [](const char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}

bool argument_is_flag(const std::string_view argument, const std::string_view flag) noexcept {
  return argument == flag || (starts_with(argument, flag) && argument.size() > flag.size() && argument[flag.size()] == '=');
}

}  // namespace

std::string_view profile_name(const EngineProfile profile) noexcept {
  switch (profile) {
    case EngineProfile::qwen: return "qwen";
    case EngineProfile::glm53: return "glm53";
    case EngineProfile::gemma4: return "gemma4";
    case EngineProfile::deepseek: return "deepseek";
    case EngineProfile::raon1: return "raon1";
    case EngineProfile::flint: return "flint";
    case EngineProfile::minimax: return "minimax";
  }
  return "qwen";  // unreachable: every value is a case above
}

bool is_card_owned_key(const std::string_view path) noexcept {
  if (path == "engine.profile" || path == "engine.lazy_load" || path == "engine.idle_unload_s") return false;
  // vision.*: a card reads images with its own encoder file (spec 2026-10-28-vision-for-cards-design.md)
  static constexpr std::array<std::string_view, 9> kExact{
      "model.aliases", "roots.models", "roots.tokenizer_pack", "limits.chat.max_prompt_tokens",
      "limits.chat.max_completion_tokens", "limits.chat.default_max_tokens", "vision.enabled", "vision.mmproj", "vision.gpu"};
  if (std::find(kExact.begin(), kExact.end(), path) != kExact.end()) return true;
  return starts_with(path, "engine.") || starts_with(path, "defaults.sampling.") || starts_with(path, "reasoning.");
}

std::variant<ModelCard, ConfigErrors> parse_model_card(const std::string_view text, const std::filesystem::path& directory,
                                                       std::string* name_read) {
  const auto parsed = parse_json(text, kCardJsonLimits);
  if (const auto* error = std::get_if<JsonError>(&parsed)) {
    const auto code = error->code == JsonErrorCode::duplicate_key ? ConfigIssueCode::duplicate_key : ConfigIssueCode::malformed_document;
    return ConfigErrors{ConfigIssue{"model card", code,
                                    std::string{describe_json_error(error->code)} + " (byte " + std::to_string(error->offset) + ")"}};
  }
  const auto* members = std::get<JsonValue>(parsed).if_object();
  if (members == nullptr) return ConfigErrors{ConfigIssue{"model card", ConfigIssueCode::malformed_document, "must be a JSON object"}};

  ConfigErrors errors;
  ModelCard card;
  const JsonValue* config = nullptr;
  const JsonValue* options = nullptr;
  bool version = false;
  bool named = false;
  bool profiled = false;
  for (const auto& member : *members) {
    if (member.key == "schema_version") {
      const auto* value = member.value.if_integer();
      if (value == nullptr || *value != 1) add(errors, "schema_version", ConfigIssueCode::invalid_value, "must be 1");
      version = true;
    } else if (member.key == "name") {
      const auto* value = member.value.if_string();
      if (value == nullptr || !valid_card_name(*value)) {
        add(errors, "name", ConfigIssueCode::invalid_value,
            "must be 1-64 characters of [A-Za-z0-9._-] (name or name:tag), starting with a letter or digit");
      } else {
        card.name = *value;
        if (name_read != nullptr) *name_read = *value;
      }
      named = true;
    } else if (member.key == "profile") {
      const auto* value = member.value.if_string();
      if (value != nullptr && *value == "qwen") card.profile = EngineProfile::qwen;
      else if (value != nullptr && *value == "gemma4") card.profile = EngineProfile::gemma4;
      else if (value != nullptr && *value == "raon1") card.profile = EngineProfile::raon1;  // glm_pack, ChatML
      else if (value != nullptr && *value == "flint") card.profile = EngineProfile::flint;  // multiengine_flint
      // launcher spec D10, met 2026-10-27: the page sends reasoning_effort only to a card whose profile takes it
      else if (value != nullptr && *value == "glm53") card.profile = EngineProfile::glm53;  // multiengine_maya
      else if (value != nullptr && *value == "deepseek") card.profile = EngineProfile::deepseek;  // multiengine_maya_ds4
      else if (value != nullptr && *value == "minimax") card.profile = EngineProfile::minimax;    // multiengine_maya_mm3
      else {
        add(errors, "profile", ConfigIssueCode::invalid_value, "must be qwen, gemma4, glm53, deepseek, raon1, flint or minimax");
      }
      profiled = true;
    } else if (member.key == "source") {
      const auto* value = member.value.if_string();
      if (value == nullptr || !valid_card_source(*value)) {
        add(errors, "source", ConfigIssueCode::invalid_value, "must be \"folder\" or \"download:\" and a catalog id");
      } else {
        card.source = *value;
      }
    } else if (member.key == "config") {
      config = &member.value;
    } else if (member.key == "options") {
      options = &member.value;
    } else {
      add(errors, member.key, ConfigIssueCode::unknown_key, "unknown key");
    }
  }
  if (!version) add(errors, "schema_version", ConfigIssueCode::missing_required, "is required");
  if (!named) add(errors, "name", ConfigIssueCode::missing_required, "is required");
  if (!profiled) add(errors, "profile", ConfigIssueCode::missing_required, "is required");
  if (config == nullptr) add(errors, "config", ConfigIssueCode::missing_required, "is required");
  if (!errors.empty()) return errors;

  // The config object: the main config's reader, then the card-owned filter.
  auto draft = parse_config_object(*config, directory);
  if (auto* issues = std::get_if<ConfigErrors>(&draft)) return std::move(*issues);
  card.config = std::move(std::get<FrontendConfigDraft>(draft));
  for (const auto& [path, entry] : card.config.entries) {
    if (path == "engine.profile") add(errors, path, ConfigIssueCode::invalid_value, "is set by the card's profile");
    else if (path == "model.name") add(errors, path, ConfigIssueCode::invalid_value, "is set by the card's name");
    else if (!is_card_owned_key(path)) {
      add(errors, path, ConfigIssueCode::invalid_value, "is not a model card row (it belongs in the main config)");
    }
  }

  if (options != nullptr) {
    auto limits = parse_card_options(card.profile, *options);
    if (auto* issues = std::get_if<ConfigErrors>(&limits)) {
      errors.insert(errors.end(), issues->begin(), issues->end());
    } else {
      card.options = std::move(std::get<std::vector<ModelOptionLimits>>(limits));
    }
  }
  // The page's value is the only source of a flag a listed option sets.
  if (const auto args = card.config.entries.find("engine.args"); args != card.config.entries.end()) {
    if (const auto* arguments = std::get_if<std::vector<std::string>>(&args->second.value)) {
      for (const auto& limit : card.options) {
        for (const auto flag : option_flags(card.profile, limit.spec->name)) {
          if (std::any_of(arguments->begin(), arguments->end(), [flag](const std::string& a) { return argument_is_flag(a, flag); })) {
            add(errors, "engine.args", ConfigIssueCode::conflicting_values,
                "holds " + std::string{flag} + ", which the card's " + std::string{limit.spec->name} + " option sets");
          }
        }
      }
    }
  }
  if (!errors.empty()) return errors;
  return card;
}

std::variant<ModelCard, ConfigErrors> read_model_card(const std::filesystem::path& file, std::string* name_read) {
  auto text = read_trusted_config_text(file);
  if (auto* errors = std::get_if<ConfigErrors>(&text)) {
    for (auto& issue : *errors) issue.key_path = "model card";
    return std::move(*errors);
  }
  const auto& read = std::get<TrustedConfigText>(text);
  return parse_model_card(read.text, read.directory, name_read);
}

LoadedCards load_model_cards(const std::vector<std::filesystem::path>& files) {
  LoadedCards loaded;
  std::set<std::string, std::less<>> names;
  for (std::size_t i = 0; i < files.size(); ++i) {
    std::string name;
    auto card = read_model_card(files[i], &name);
    if (auto* issues = std::get_if<ConfigErrors>(&card)) {
      loaded.refused.push_back(CardRefusal{i + 1U, std::move(name), std::move(*issues)});
      continue;
    }
    auto& parsed = std::get<ModelCard>(card);
    if (!names.insert(parsed.name).second) {
      loaded.refused.push_back(CardRefusal{
          i + 1U, parsed.name, {ConfigIssue{"name", ConfigIssueCode::duplicate_key, "is already the name of an earlier card"}}});
      continue;
    }
    loaded.cards.push_back(std::move(parsed));
  }
  return loaded;
}

ConfigErrors main_config_card_issues(const FrontendConfigDraft& main) {
  ConfigErrors errors;
  for (const auto& [path, entry] : main.entries) {
    if (is_card_owned_key(path) || path == "engine.profile" || path == "model.name") {
      add(errors, path, ConfigIssueCode::conflicting_values, "is set by each model card when models.cards is set");
    }
  }
  return errors;
}

FrontendConfigDraft card_draft(const FrontendConfigDraft& main, const ModelCard& card, const ModelOptionValues& values) {
  FrontendConfigDraft draft = main;
  for (const auto& [path, entry] : card.config.entries) draft.entries[path] = entry;
  draft.entries["engine.profile"] = ConfigDraftEntry{std::string{profile_name(card.profile)}, ConfigSource::file};
  draft.entries["model.name"] = ConfigDraftEntry{card.name, ConfigSource::file};
  apply_option_values(card.profile, values, draft);
  draft.card = true;
  return draft;
}

ConfigValidationResult card_config(const FrontendConfigDraft& main, const ModelCard& card, const ModelOptionValues& values) {
  return validate(card_draft(main, card, values));
}

namespace {

// The main config with models.default_engine standing in for a card's engine, a placeholder model id and `models_row`
// (an entry of the main config) as roots.models: the server rows of a config with no card yet. Never launched.
std::variant<FrontendConfigDraft, ConfigErrors> card_less_draft(const FrontendConfigDraft& main,
                                                               const std::initializer_list<std::string_view> models_rows) {
  if (auto issues = main_config_card_issues(main); !issues.empty()) return issues;
  const auto engine = main.entries.find("models.default_engine");
  if (engine == main.entries.end() || !std::holds_alternative<std::string>(engine->second.value)) {
    return ConfigErrors{ConfigIssue{"models.default_engine", ConfigIssueCode::missing_required,
                                    "is needed to serve with no model card (the engine of the cards the page adds)"}};
  }
  FrontendConfigDraft draft = main;
  draft.entries["engine.executable"] = engine->second;
  draft.entries["engine.model_id"] = ConfigDraftEntry{std::string{"none"}, ConfigSource::file};
  for (const auto row : models_rows) {
    if (const auto folder = main.entries.find(row); folder != main.entries.end()) {
      draft.entries["roots.models"] = folder->second;
      return draft;
    }
  }
  return ConfigErrors{ConfigIssue{"models.default_root", ConfigIssueCode::missing_required,
                                  "is needed to serve with no model card (the folder the page offers for models)"}};
}

}  // namespace

ConfigValidationResult card_less_server_config(const FrontendConfigDraft& main) {
  auto draft = card_less_draft(main, {"models.default_root", "models.downloads_root", "roots.state"});
  if (auto* issues = std::get_if<ConfigErrors>(&draft)) return std::move(*issues);
  return validate(std::get<FrontendConfigDraft>(draft));
}

ConfigValidationResult launcher_store_config(const FrontendConfigDraft& main) {
  if (auto issues = main_config_card_issues(main); !issues.empty()) return issues;
  const auto root = main.entries.find("models.downloads_root");
  if (root == main.entries.end()) {
    return ConfigErrors{ConfigIssue{"models.downloads_root", ConfigIssueCode::missing_required,
                                    "is needed for strata's own verbs with models.cards (it is their model store)"}};
  }
  const auto cards = main.entries.find("models.cards");
  const auto* listed = cards == main.entries.end() ? nullptr : std::get_if<std::vector<std::string>>(&cards->second.value);
  if (listed == nullptr) return ConfigErrors{ConfigIssue{"models.cards", ConfigIssueCode::wrong_type, "must be an array of strings"}};
  std::vector<std::filesystem::path> files;
  for (const auto& text : *listed) files.push_back(path_from_utf8(text));
  auto loaded = load_model_cards(files);
  std::optional<ConfigErrors> first_issues;
  for (const auto& card : loaded.cards) {
    FrontendConfigDraft draft = card_draft(main, card, default_option_values(card));
    draft.entries["roots.models"] = root->second;
    auto result = validate(draft);
    if (std::holds_alternative<ValidatedConfig>(result)) return result;
    if (!first_issues) first_issues = std::get<ConfigErrors>(std::move(result));
  }
  if (first_issues) return std::move(*first_issues);
  // GUI-first start-up: no card yet (a first download is how the first card comes), the downloads' store as the root.
  if (main.entries.contains("models.default_engine")) {
    auto draft = card_less_draft(main, {"models.downloads_root"});
    if (auto* issues = std::get_if<ConfigErrors>(&draft)) return std::move(*issues);
    return validate(std::get<FrontendConfigDraft>(draft));
  }
  return ConfigErrors{ConfigIssue{"models.cards", ConfigIssueCode::invalid_value, "no model card is usable"}};
}

ModelOptionValues default_option_values(const ModelCard& card) {
  ModelOptionValues values;
  for (const auto& limit : card.options) values.emplace(std::string{limit.spec->name}, limit.default_value);
  return values;
}

}  // namespace secure_frontend
