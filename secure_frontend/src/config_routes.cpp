#include "secure_frontend/config_routes.hpp"

#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/config_writer.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/frontend_init.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/model_cards.hpp"
#include "secure_frontend/secret_source.hpp"
#include "secure_frontend/token_reader.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <variant>
#include <vector>

namespace secure_frontend {
namespace {

constexpr JsonLimits kBodyLimits{.max_bytes = kModelControlBodyLimit, .max_depth = 4U, .max_container_entries = 64U,
                                 .max_string_bytes = 2048U};

// The rows the Settings tab manages (spec 2026-10-28 Part 1): the listener and LAN access, limits and timeouts, folders;
// the engine's output in the log (owner 2026-10-29).
// The key itself is not a row (its store is rotated, never written through here).
constexpr std::array<std::string_view, 15> kManaged{
    "listener.host", "listener.port", "listener.allow_remote", "listener.allow_wildcard_bind", "listener.host_aliases",
    "models.control_remote", "models.control_remote_networks", "limits.engine.generation_timeout_s",
    "limits.engine.ready_timeout_s", "limits.chat.max_prompt_tokens", "limits.chat.max_completion_tokens", "pull.connections",
    "models.downloads_root", "models.default_root", "logging.engine_output"};

std::string json_text(const std::string_view text) {
  std::string out;
  append_json_string(&out, text);
  return out;
}

HttpResponse json_answer(const int status, std::string body) {
  return {.status = status, .headers = {{"Content-Type", "application/json; charset=utf-8"}, {"Cache-Control", "no-store"}},
          .body = std::move(body)};
}

HttpResponse refused(const std::string& reason) { return make_error_response(ApiError::settings_refused, {.reason = reason}); }

std::string_view type_name(const ConfigValueType type) {
  switch (type) {
    case ConfigValueType::boolean: return "boolean";
    case ConfigValueType::integer: return "integer";
    case ConfigValueType::string: return "string";
    case ConfigValueType::string_array: return "string array";
    case ConfigValueType::decimal: return "number";
  }
  return "string";  // unreachable: every type is a case above
}

std::string value_json(const ConfigDraftValue& value) {
  if (const auto* flag = std::get_if<bool>(&value)) return *flag ? "true" : "false";
  if (const auto* number = std::get_if<std::int64_t>(&value)) return std::to_string(*number);
  if (const auto* text = std::get_if<std::string>(&value)) return json_text(*text);
  if (const auto* list = std::get_if<std::vector<std::string>>(&value)) {
    std::string out = "[";
    for (std::size_t i = 0; i < list->size(); ++i) out += (i != 0U ? "," : "") + json_text((*list)[i]);
    return out + "]";
  }
  return "null";  // a decimal: no managed row is one
}

std::string default_json(const ConfigKey& key) {
  switch (key.type) {
    case ConfigValueType::boolean: return key.default_boolean ? "true" : "false";
    case ConfigValueType::integer:
      return key.default_integer < key.minimum || key.default_integer > key.maximum ? "null" : std::to_string(key.default_integer);
    case ConfigValueType::string: return key.default_string.empty() ? "null" : json_text(key.default_string);
    case ConfigValueType::string_array: return "[]";
    case ConfigValueType::decimal: return "null";
  }
  return "null";
}

// The page's value as the key's draft value; nullopt: the wrong type for the key.
std::optional<ConfigDraftValue> typed_value(const ConfigKey& key, const JsonValue& json) {
  switch (key.type) {
    case ConfigValueType::boolean:
      if (const auto* flag = json.if_boolean()) return ConfigDraftValue{*flag};
      break;
    case ConfigValueType::integer:
      if (const auto* number = json.if_integer()) return ConfigDraftValue{*number};
      break;
    case ConfigValueType::string:
      if (const auto* text = json.if_string()) return ConfigDraftValue{*text};
      break;
    case ConfigValueType::string_array:
      if (const auto* items = json.if_array()) {
        std::vector<std::string> list;
        for (const auto& item : *items) {
          const auto* text = item.if_string();
          if (text == nullptr) return std::nullopt;
          list.push_back(*text);
        }
        return ConfigDraftValue{std::move(list)};
      }
      break;
    case ConfigValueType::decimal: break;
  }
  return std::nullopt;
}

// The issues the server would find in this config at start: a cards config by the card-less rules (the server's
// own while no card runs), any other by validate().
ConfigErrors start_up_issues(const FrontendConfigDraft& draft) {
  const auto checked = draft.entries.contains("models.cards") ? card_less_server_config(draft) : validate(draft);
  if (const auto* issues = std::get_if<ConfigErrors>(&checked)) return *issues;
  return {};
}

}  // namespace

ConfigRoutes::ConfigRoutes(ConfigRouteSettings settings) : settings_(std::move(settings)) {}

HttpResponse ConfigRoutes::answer(const RouteId route, const std::string_view body, const RequestContext& context) {
  // the key is this PC's business alone; the other settings the business of the clients the other settings answer
  const bool allowed = route == RouteId::strata_key_rotate ? context.loopback_peer
                                                           : (settings_.client_allowed ? settings_.client_allowed(context)
                                                                                       : context.loopback_peer);
  if (!allowed) return make_error_response(ApiError::control_not_allowed);
  switch (route) {
    case RouteId::strata_config: return settings_json();
    case RouteId::strata_config_settings: return save(body);
    case RouteId::strata_key_rotate: return rotate(body);
    default: return make_error_response(ApiError::not_found);
  }
}

HttpResponse ConfigRoutes::settings_json() const {
  FrontendConfigDraft draft;
  if (settings_.config_file.has_value()) {
    auto read = read_config_file(*settings_.config_file);
    if (!std::holds_alternative<FrontendConfigDraft>(read)) return refused("the config file could not be read");
    draft = std::get<FrontendConfigDraft>(std::move(read));
  }
  std::string out = std::string{"{\"editable\":"} + (settings_.config_file ? "true" : "false") + ",\"settings\":[";
  bool first = true;
  for (const auto path : kManaged) {
    const auto* key = find_config_key(path);
    if (key == nullptr) continue;
    const auto row = draft.entries.find(path);
    out += (first ? "" : ",") + std::string{"{\"key\":"} + json_text(path) + ",\"type\":" + json_text(type_name(key->type)) +
           ",\"value\":" + (row == draft.entries.end() ? std::string{"null"} : value_json(row->second.value)) +
           ",\"default\":" + default_json(*key);
    if (key->type == ConfigValueType::integer) out += ",\"min\":" + std::to_string(key->minimum) + ",\"max\":" + std::to_string(key->maximum);
    out += ",\"help\":" + json_text(key->description) + "}";
    first = false;
  }
  out += std::string{"],\"key\":{\"rotatable\":"} + (settings_.secret_dir ? "true" : "false") + "}}\n";
  return json_answer(200, std::move(out));
}

HttpResponse ConfigRoutes::save(const std::string_view body) {
  if (!settings_.config_file.has_value()) return refused("this server was started without a config file to write");
  const auto parsed = parse_json(body, kBodyLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root == nullptr ? nullptr : root->if_object();
  if (members == nullptr || members->size() != 1U || (*members)[0].key != "values" || (*members)[0].value.if_object() == nullptr) {
    return make_error_response(ApiError::invalid_request);
  }
  // Each value checked for its key's type here; every other rule by the whole file's check below.
  std::vector<std::pair<std::string, std::optional<ConfigDraftValue>>> changes;
  for (const auto& member : *(*members)[0].value.if_object()) {
    const auto* key = std::find(kManaged.begin(), kManaged.end(), member.key) == kManaged.end() ? nullptr : find_config_key(member.key);
    if (key == nullptr) return refused(member.key.substr(0, 64) + " is not a setting this page manages");
    if (member.value.kind() == JsonKind::null) {
      changes.emplace_back(member.key, std::nullopt);
      continue;
    }
    auto value = typed_value(*key, member.value);
    if (!value) return refused(member.key + " must be " + std::string{type_name(key->type) == "string array" ? "a list of text" : type_name(key->type)});
    // an empty folder or host is the row left out (its default)
    if (const auto* text = std::get_if<std::string>(&*value); text != nullptr && text->empty()) value.reset();
    changes.emplace_back(member.key, std::move(value));
  }
  const auto apply = [&changes](FrontendConfigDraft& draft) {
    for (const auto& [row, value] : changes) {
      if (value) draft.entries[row] = ConfigDraftEntry{*value, ConfigSource::file};
      else draft.entries.erase(row);
    }
  };
  const std::scoped_lock lock(file_);
  auto read = read_config_file(*settings_.config_file);
  if (!std::holds_alternative<FrontendConfigDraft>(read)) return refused(std::string{config_update_reason(ConfigUpdateFailure::unreadable)});
  const auto& before = std::get<FrontendConfigDraft>(read);
  FrontendConfigDraft after = before;
  apply(after);
  // The server's own start-up rules on the whole file: an issue the change brings is refused by name; one the file
  // already had is not this save's (it stays as it was).
  const auto old_issues = start_up_issues(before);
  for (const auto& issue : start_up_issues(after)) {
    const bool known = std::any_of(old_issues.begin(), old_issues.end(), [&issue](const ConfigIssue& old) {
      return old.key_path == issue.key_path && old.code == issue.code;
    });
    if (!known) return refused(format_issue(issue));
  }
  if (const auto failure = update_config_file(*settings_.config_file, apply)) return refused(std::string{config_update_reason(*failure)});
  if (settings_.log) settings_.log("[strata] settings: the server settings were saved (they apply at the next start)");
  return json_answer(200, "{\"saved\":true,\"restart_needed\":true}\n");
}

HttpResponse ConfigRoutes::rotate(const std::string_view body) {
  const auto parsed = parse_json(body, kBodyLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root == nullptr ? nullptr : root->if_object();
  if (members == nullptr || members->size() != 1U || (*members)[0].key != "confirm" || (*members)[0].value.if_string() == nullptr ||
      *(*members)[0].value.if_string() != "ROTATE") {
    return make_error_response(ApiError::invalid_request);
  }
  if (!settings_.secret_dir.has_value()) {
    return refused("this server's key comes from an environment variable (a development start): there is no key store to rotate");
  }
  const auto store = open_root(*settings_.secret_dir);
  const auto file = parse_relative_path(settings_.token_file);
  const auto source = make_protected_file_secret_source(settings_.token_file);
  if (!std::holds_alternative<CanonicalRoot>(store) || !std::holds_alternative<RelativePath>(file) ||
      !std::holds_alternative<SecretSource>(source)) {
    return refused("the key store could not be opened");
  }
  const std::scoped_lock lock(file_);
  // the store's own path, as `strata key rotate`: CSPRNG bytes, atomic replacement, owner-only
  if (generate_token(std::get<CanonicalRoot>(store), std::get<RelativePath>(file), true)) return refused("a new key could not be written");
  const auto read = read_token(std::get<SecretSource>(source), std::get<CanonicalRoot>(store));
  const auto* key = std::get_if<SecretBytes>(&read);
  if (key == nullptr) return refused("the new key could not be read back");
  if (settings_.log) settings_.log("[strata] settings: the API key was rotated (the new one applies at the next start)");
  return json_answer(200, "{\"key\":" + json_text(key->view()) + ",\"restart_needed\":true}\n");
}

}  // namespace secure_frontend
