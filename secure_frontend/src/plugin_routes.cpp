#include "secure_frontend/plugin_routes.hpp"

#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/config_writer.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/plugin_secrets.hpp"
#include "secure_frontend/token_store.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

constexpr JsonLimits kBodyLimits{.max_bytes = kModelControlBodyLimit, .max_depth = 2U, .max_container_entries = 8U,
                                 .max_string_bytes = 256U};

std::string json_text(const std::string_view text) {
  std::string out;
  append_json_string(&out, text);
  return out;
}

std::string json_or_null(const std::string_view text) { return text.empty() ? std::string{"null"} : json_text(text); }

const JsonValue* member_of(const JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& entry : *members) {
    if (entry.key == key) return &entry.value;
  }
  return nullptr;
}

// A value as compact JSON (a Try's arguments, as the tool hub sends them).
void compact(const JsonValue& value, std::string& out) {
  if (const auto* items = value.if_array()) {
    out += '[';
    for (std::size_t i = 0; i < items->size(); ++i) {
      if (i > 0U) out += ',';
      compact((*items)[i], out);
    }
    out += ']';
  } else if (const auto* members = value.if_object()) {
    out += '{';
    for (std::size_t i = 0; i < members->size(); ++i) {
      if (i > 0U) out += ',';
      append_json_string(&out, (*members)[i].key);
      out += ':';
      compact((*members)[i].value, out);
    }
    out += '}';
  } else {
    out += python_json_text(value);
  }
}

HttpResponse json_answer(const int status, std::string body) {
  return {.status = status, .headers = {{"Content-Type", "application/json; charset=utf-8"}, {"Cache-Control", "no-store"}},
          .body = std::move(body)};
}

HttpResponse refused(const std::string& reason) { return make_error_response(ApiError::plugin_refused, {.reason = reason}); }

HttpResponse unreadable_choices() {
  return refused("plugins.json in the data folder's state folder is not one this server reads: fix it or move it away");
}

// {"id"} (an approval) or {"id", "on"} (a switch), nothing else; nullopt: not such a body.
struct PluginRequest final {
  std::string id;
  bool on{false};
};
std::optional<PluginRequest> parse_request(const std::string_view body, const bool with_on) {
  const auto parsed = parse_json(body, kBodyLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root == nullptr ? nullptr : root->if_object();
  if (members == nullptr || members->size() != (with_on ? 2U : 1U)) return std::nullopt;
  PluginRequest request;
  bool id = false;
  bool on = false;
  for (const auto& member : *members) {
    if (member.key == "id" && member.value.if_string() != nullptr) {
      request.id = *member.value.if_string();
      id = true;
    } else if (with_on && member.key == "on" && member.value.if_boolean() != nullptr) {
      request.on = *member.value.if_boolean();
      on = true;
    }
  }
  if (!id || (with_on && !on)) return std::nullopt;
  return request;
}

// A settings body: {"id", "values": {name: value or null}} - at most 16 values, each a whole number, true or false, a
// text or a list of texts (a text at most 1024 bytes).
constexpr JsonLimits kSettingsLimits{.max_bytes = 32U * 1024U, .max_depth = 3U, .max_container_entries = 32U,
                                     .max_string_bytes = 1024U, .allow_fractions = false};

std::string value_json(const PluginValue& value) {
  if (const auto* b = std::get_if<bool>(&value)) return *b ? "true" : "false";
  if (const auto* i = std::get_if<std::int64_t>(&value)) return std::to_string(*i);
  if (const auto* s = std::get_if<std::string>(&value)) return json_text(*s);
  std::string out = "[";
  for (const auto& item : std::get<std::vector<std::string>>(value)) out += (out.size() > 1U ? "," : "") + json_text(item);
  return out + "]";
}

// The plugins.<id>.<name> rows of a config file, by plugin and setting (a decimal is no setting's value: left out).
using PluginValues = std::map<std::string, std::map<std::string, PluginValue>>;
PluginValues values_in(const FrontendConfigDraft& draft) {
  PluginValues values;
  for (const auto& [path, entry] : draft.entries) {
    if (!plugin_setting_path(path)) continue;
    const auto dot = path.find('.', 8U);
    auto& slot = values[path.substr(8U, dot - 8U)];
    const std::string name = path.substr(dot + 1U);
    if (const auto* b = std::get_if<bool>(&entry.value)) slot[name] = *b;
    else if (const auto* i = std::get_if<std::int64_t>(&entry.value)) slot[name] = *i;
    else if (const auto* s = std::get_if<std::string>(&entry.value)) slot[name] = *s;
    else if (const auto* items = std::get_if<std::vector<std::string>>(&entry.value)) slot[name] = *items;
  }
  return values;
}

// The words of `network` with each {setting} filled in: its value, else its default, else as written.
std::string network_now(const PluginRun& run, const std::map<std::string, PluginValue>& values) {
  std::string out;
  std::size_t at = 0;
  while (at < run.network.size()) {
    const auto open = run.network.find('{', at);
    const auto close = open == std::string::npos ? std::string::npos : run.network.find('}', open);
    if (close == std::string::npos) {
      out += run.network.substr(at);
      break;
    }
    out += run.network.substr(at, open - at);
    const std::string name = run.network.substr(open + 1U, close - open - 1U);
    const auto setting = std::find_if(run.settings.begin(), run.settings.end(), [&name](const PluginSetting& s) { return s.name == name; });
    if (const auto value = values.find(name); value != values.end()) out += value_text(value->second);
    else if (setting != run.settings.end() && setting->default_value) out += value_text(*setting->default_value);
    else out += "{" + name + "}";
    at = close + 1U;
  }
  return out;
}

// A tool's or notifier's settings (each with its rules and its value, a secret's only as set or not), its presets, what
// it connects to and what keeps it from running; none for any other plugin.
std::string settings_json(const PluginManifest* manifest, const std::map<std::string, PluginValue>& values,
                          const std::set<std::string>& secrets_set) {
  const PluginRun* run = manifest != nullptr && manifest->run ? &*manifest->run : nullptr;
  std::string out = ",\"settings\":[";
  bool first = true;
  for (const auto& setting : run != nullptr ? run->settings : std::vector<PluginSetting>{}) {
    out += (first ? "{" : ",{") + std::string{"\"name\":"} + json_text(setting.name) + ",\"kind\":" + json_text(setting_kind_name(setting.kind)) +
           ",\"label\":" + json_text(setting.label) + ",\"help\":" + json_text(setting.help) + ",\"required\":" + (setting.required ? "true" : "false");
    first = false;
    if (setting.kind == SettingKind::secret) {
      out += std::string{",\"set\":"} + (secrets_set.contains(setting.name) ? "true" : "false") + "}";
      continue;
    }
    out += ",\"default\":" + (setting.default_value ? value_json(*setting.default_value) : std::string{"null"});
    if (setting.kind == SettingKind::integer) out += ",\"min\":" + std::to_string(setting.min) + ",\"max\":" + std::to_string(setting.max);
    if (!setting.choices.empty()) out += ",\"choices\":" + value_json(PluginValue{setting.choices});
    if (setting.kind == SettingKind::text_list) out += ",\"max_items\":" + std::to_string(setting.max_items);
    const auto value = values.find(setting.name);
    out += ",\"value\":" + (value != values.end() ? value_json(value->second) : std::string{"null"}) + "}";
  }
  out += "],\"presets\":[";
  first = true;
  for (const auto& preset : run != nullptr ? run->presets : std::vector<PluginPreset>{}) {
    out += (first ? "" : ",") + std::string{"{\"label\":"} + json_text(preset.label) + ",\"values\":{";
    first = false;
    bool first_value = true;
    // in the order the manifest declares its settings
    for (const auto& setting : run->settings) {
      const auto value = preset.values.find(setting.name);
      if (value == preset.values.end()) continue;
      out += (first_value ? "" : ",") + json_text(setting.name) + ":" + value_json(value->second);
      first_value = false;
    }
    out += "}}";
  }
  std::optional<std::string> problem;
  if (run != nullptr) {
    auto checked = plugin_values(*run, values, secrets_set);
    if (const auto* why = std::get_if<std::string>(&checked)) problem = *why;
  }
  return out + "],\"network\":" + json_text(run != nullptr ? network_now(*run, values) : std::string{}) +
         ",\"settings_problem\":" + (problem ? json_text(*problem) : std::string{"null"});
}

// A running tool plugin's tools as the "Try" form needs them: each name, description and string parameters.
std::string tools_json(const std::vector<McpTool>* tools) {
  constexpr JsonLimits kSchemaLimits{.max_bytes = 1U << 20U, .max_depth = 32U, .max_container_entries = 4096U,
                                     .max_string_bytes = 1U << 16U, .allow_fractions = true};
  std::string out = ",\"tools\":[";
  bool first = true;
  for (const auto& tool : tools != nullptr ? *tools : std::vector<McpTool>{}) {
    out += (first ? "" : ",") + std::string{"{\"name\":"} + json_text(tool.name) + ",\"description\":" + json_text(tool.description) +
           ",\"string_parameters\":[";
    first = false;
    const auto parsed = parse_json(tool.input_schema, kSchemaLimits);
    const auto* schema = std::get_if<JsonValue>(&parsed);
    const auto* properties = schema != nullptr && schema->if_object() != nullptr ? member_of(*schema, "properties") : nullptr;
    bool first_parameter = true;
    if (properties != nullptr && properties->if_object() != nullptr) {
      for (const auto& property : *properties->if_object()) {
        const auto* type = member_of(property.value, "type");
        if (type == nullptr || type->if_string() == nullptr || *type->if_string() != "string") continue;
        out += (first_parameter ? "" : ",") + json_text(property.key);
        first_parameter = false;
      }
    }
    out += "]}";
  }
  return out + "]";
}

// The keys this server trusts, as the list and the keys route answer them.
std::string keys_json(const std::vector<TrustedKey>& keys) {
  std::string out = "[";
  bool first = true;
  for (const auto& key : keys) {
    out += (first ? "" : ",") + std::string{"{\"key\":"} + json_text(key.key) + ",\"name\":" + json_text(key.name) +
           ",\"install\":" + (key.install ? "true" : "false") + "}";
    first = false;
  }
  return out + "]";
}

std::string entry_json(const PluginEntry& entry, const bool running, const std::map<std::string, PluginValue>& values,
                       const std::set<std::string>& secrets_set, const std::vector<McpTool>* tools) {
  const auto* manifest = entry.manifest ? &*entry.manifest : nullptr;
  std::string out = "{\"id\":" + json_text(entry.id);
  out += ",\"kind\":" + (manifest != nullptr ? json_text(plugin_kind_name(manifest->kind)) : std::string{"null"});
  out += ",\"name\":" + (manifest != nullptr ? json_text(manifest->name) : std::string{"null"});
  out += ",\"version\":" + (manifest != nullptr ? json_text(manifest->version) : std::string{"null"});
  out += ",\"publisher\":" + (manifest != nullptr ? json_text(manifest->publisher) : std::string{"null"});
  out += ",\"license\":" + (manifest != nullptr ? json_text(manifest->license) : std::string{"null"});
  out += ",\"about\":" + (manifest != nullptr ? json_text(manifest->about) : std::string{"null"});
  out += std::string{",\"first_party\":"} + (entry.first_party ? "true" : "false");
  out += ",\"state\":" + json_text(plugin_state_name(entry.state)) + ",\"reason\":" + json_or_null(entry.reason);
  out += std::string{",\"running\":"} + (running ? "true" : "false");
  // its signature (spec section 11): whose key, signed by whom, or why it is not taken
  if (entry.signature) {
    out += ",\"signature\":{\"key\":" + json_or_null(entry.signature->key) + ",\"signer\":" + json_or_null(entry.signature->signer) +
           ",\"problem\":" + json_or_null(entry.signature->problem) + "}";
  } else {
    out += ",\"signature\":null";
  }
  out += ",\"manifest_sha256\":" + json_or_null(entry.pin.manifest) + ",\"files\":[";
  bool first = true;
  for (const auto& [path, sum] : entry.pin.files) {
    out += (first ? "" : ",") + std::string{"{\"path\":"} + json_text(path) + ",\"sha256\":" + json_text(sum) + "}";
    first = false;
  }
  out += "],\"switches\":[";
  first = true;
  if (manifest != nullptr) {
    for (const auto value : manifest->switches) {
      out += (first ? "" : ",") + json_text(diagnostic_switch_name(value));
      first = false;
    }
  }
  return out + "]" + settings_json(manifest, values, secrets_set) + tools_json(tools) + "}";
}

}  // namespace

PluginRoutes::PluginRoutes(PluginRouteSettings settings) : settings_(std::move(settings)) {}

HttpResponse PluginRoutes::answer(const RouteId route, const std::string_view body, const RequestContext& context) {
  const bool allowed = settings_.client_allowed ? settings_.client_allowed(context) : context.loopback_peer;
  if (!allowed) return make_error_response(ApiError::control_not_allowed);
  switch (route) {
    case RouteId::strata_plugins: return list();
    case RouteId::strata_plugins_approve: return approve(body);
    case RouteId::strata_plugins_switch: return change(body);
    case RouteId::strata_plugins_settings: return save_settings(body);
    case RouteId::strata_plugins_try: return try_tool(body);
    case RouteId::strata_plugins_test: return test_notifier(body);
    case RouteId::strata_plugins_keys: return keys(body);
    default: return make_error_response(ApiError::not_found);
  }
}

std::optional<PluginChoices> PluginRoutes::choices() const {
  return settings_.state_dir ? read_plugin_choices(*settings_.state_dir) : std::optional<PluginChoices>{PluginChoices{}};
}

bool PluginRoutes::save(const PluginChoices& choices) {
  return settings_.state_dir && write_plugin_choices(*settings_.state_dir, choices);
}

HttpResponse PluginRoutes::list() const {
  const auto chosen = choices();
  if (!chosen) return unreadable_choices();
  std::string out = "{\"folder\":" + json_text(path_to_utf8(settings_.plugins_dir)) + ",\"editable\":" +
                    (settings_.state_dir ? "true" : "false") + ",\"plugins\":[";
  // the settings as the config file holds them now (a change shows before the restart that applies it)
  PluginValues values;
  if (settings_.config_file) {
    auto read = read_config_file(*settings_.config_file);
    if (const auto* draft = std::get_if<FrontendConfigDraft>(&read)) values = values_in(*draft);
  }
  const auto listed = settings_.tools ? settings_.tools() : std::map<std::string, std::vector<McpTool>>{};
  bool first = true;
  for (const auto& entry : discover_plugins(settings_.plugins_dir, settings_.first_party, *chosen, settings_.install_key)) {
    std::set<std::string> secrets_set;
    if (settings_.secret_dir && entry.manifest && entry.manifest->run) {
      for (const auto& setting : entry.manifest->run->settings) {
        if (setting.kind == SettingKind::secret && plugin_secret_set(*settings_.secret_dir, entry.id, setting.name)) secrets_set.insert(setting.name);
      }
    }
    const auto tools = listed.find(entry.id);
    out += (first ? "" : ",") + entry_json(entry, settings_.running.contains(entry.id), values[entry.id], secrets_set,
                                           tools != listed.end() ? &tools->second : nullptr);
    first = false;
  }
  return json_answer(200, out + "],\"keys\":" + keys_json(trusted_keys(settings_.install_key, *chosen)) + "}\n");
}

HttpResponse PluginRoutes::approve(const std::string_view body) {
  const auto request = parse_request(body, false);
  if (!request) return make_error_response(ApiError::invalid_request);
  if (!settings_.state_dir) return refused("this server was started without a data folder to keep the approvals in");
  const std::scoped_lock lock(file_);
  auto chosen = choices();
  if (!chosen) return unreadable_choices();
  const auto entries = discover_plugins(settings_.plugins_dir, settings_.first_party, *chosen, settings_.install_key);
  const auto entry = std::find_if(entries.begin(), entries.end(), [&request](const PluginEntry& item) { return item.id == request->id; });
  if (entry == entries.end()) return refused("there is no plugin " + request->id.substr(0, 64) + " in the plugins folder");
  if (entry->state == PluginState::on || entry->state == PluginState::off) {
    return refused(entry->first_party ? "it is already trusted: this install was built with it" : "it is already approved");
  }
  if (entry->state == PluginState::refused) return refused(entry->reason);
  // What is approved is what is there now: its manifest's and its files' SHA-256s, pinned.
  chosen->approved[entry->id] = entry->pin;
  chosen->enabled[entry->id] = true;
  if (!save(*chosen)) return refused("plugins.json could not be written in the data folder's state folder");
  const auto state = plugin_kind_runs(entry->manifest->kind) ? PluginState::on : PluginState::refused;
  if (settings_.log) settings_.log("[strata] plugins: " + entry->id + " was approved (it applies at the next start)");
  return json_answer(200, "{\"id\":" + json_text(entry->id) + ",\"state\":" + json_text(plugin_state_name(state)) + ",\"restart_needed\":true}\n");
}

HttpResponse PluginRoutes::change(const std::string_view body) {
  const auto request = parse_request(body, true);
  if (!request) return make_error_response(ApiError::invalid_request);
  if (!settings_.state_dir) return refused("this server was started without a data folder to keep the choices in");
  const std::scoped_lock lock(file_);
  auto chosen = choices();
  if (!chosen) return unreadable_choices();
  const auto entries = discover_plugins(settings_.plugins_dir, settings_.first_party, *chosen, settings_.install_key);
  const auto entry = std::find_if(entries.begin(), entries.end(), [&request](const PluginEntry& item) { return item.id == request->id; });
  if (entry == entries.end()) return refused("there is no plugin " + request->id.substr(0, 64) + " in the plugins folder");
  if (entry->state == PluginState::waiting || entry->state == PluginState::changed) return refused("approve it first: " + entry->reason);
  if (entry->state == PluginState::refused) return refused(entry->reason);
  chosen->enabled[entry->id] = request->on;
  if (!save(*chosen)) return refused("plugins.json could not be written in the data folder's state folder");
  if (settings_.log) {
    settings_.log("[strata] plugins: " + entry->id + " was turned " + (request->on ? "on" : "off") + " (it applies at the next start)");
  }
  return json_answer(200, "{\"id\":" + json_text(entry->id) + ",\"state\":" + (request->on ? "\"on\"" : "\"off\"") + ",\"restart_needed\":true}\n");
}

}  // namespace secure_frontend

namespace secure_frontend {

HttpResponse PluginRoutes::save_settings(const std::string_view body) {
  // {"id", "values": {name: value or null}}, nothing else
  const auto parsed = parse_json(body, kSettingsLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root == nullptr ? nullptr : root->if_object();
  const JsonValue* id_value = nullptr;
  const JsonValue* values = nullptr;
  if (members != nullptr && members->size() == 2U) {
    for (const auto& member : *members) {
      if (member.key == "id") id_value = &member.value;
      if (member.key == "values") values = &member.value;
    }
  }
  if (id_value == nullptr || id_value->if_string() == nullptr || values == nullptr || values->if_object() == nullptr ||
      values->if_object()->size() > kMaxPluginSettings) {
    return make_error_response(ApiError::invalid_request);
  }
  const std::string& id = *id_value->if_string();
  const std::scoped_lock lock(file_);
  const auto chosen = choices();
  if (!chosen) return unreadable_choices();
  const auto entries = discover_plugins(settings_.plugins_dir, settings_.first_party, *chosen, settings_.install_key);
  const auto entry = std::find_if(entries.begin(), entries.end(), [&id](const PluginEntry& e) { return e.id == id; });
  if (entry == entries.end()) return refused("there is no plugin " + id + " in the plugins folder");
  if (!entry->manifest || !entry->manifest->run || entry->manifest->run->settings.empty()) return refused(id + " has no settings");
  const auto& run = *entry->manifest->run;
  // each value checked against its setting before anything is written
  struct Change final {
    const PluginSetting* setting;
    std::optional<PluginValue> value;  // nullopt: back to the default (a secret: removed)
  };
  std::vector<Change> changes;
  bool in_file = false;
  bool secret = false;
  for (const auto& member : *values->if_object()) {
    const auto setting = std::find_if(run.settings.begin(), run.settings.end(), [&member](const PluginSetting& s) { return s.name == member.key; });
    if (setting == run.settings.end()) {
      return refused(id + " has no setting " + (member.key.size() <= 64U ? member.key : member.key.substr(0U, 64U)));
    }
    Change change{&*setting, std::nullopt};
    if (member.value.kind() != JsonKind::null) {
      std::optional<PluginValue> value;
      if (const auto* b = member.value.if_boolean()) value = *b;
      else if (const auto* i = member.value.if_integer()) value = *i;
      else if (const auto* s = member.value.if_string()) value = *s;
      else if (const auto* items = member.value.if_array()) {
        std::vector<std::string> texts;
        for (const auto& item : *items) {
          if (item.if_string() == nullptr) break;
          texts.push_back(*item.if_string());
        }
        if (texts.size() == items->size()) value = std::move(texts);
      }
      if (!value) return refused(setting->name + " must be a value of its kind");
      if (const auto problem = setting_value_problem(*setting, *value)) return refused(*problem);
      change.value = std::move(*value);
    }
    (setting->kind == SettingKind::secret ? secret : in_file) = true;
    changes.push_back(std::move(change));
  }
  if (in_file && !settings_.config_file) return refused("this server was started without a config file to write");
  if (secret && !settings_.secret_dir) return refused("this server was started without a secret folder to keep a secret in");
  if (in_file) {
    const auto failure = update_config_file(*settings_.config_file, [&changes, &id](FrontendConfigDraft& draft) {
      for (const auto& change : changes) {
        if (change.setting->kind == SettingKind::secret) continue;
        const std::string path = "plugins." + id + "." + change.setting->name;
        if (!change.value) {
          draft.entries.erase(path);
          continue;
        }
        ConfigDraftValue value;
        if (const auto* b = std::get_if<bool>(&*change.value)) value = *b;
        else if (const auto* i = std::get_if<std::int64_t>(&*change.value)) value = *i;
        else if (const auto* s = std::get_if<std::string>(&*change.value)) value = *s;
        else value = std::get<std::vector<std::string>>(*change.value);
        draft.entries.insert_or_assign(path, ConfigDraftEntry{std::move(value), ConfigSource::file});
      }
    });
    if (failure) return refused(std::string{config_update_reason(*failure)});
  }
  for (const auto& change : changes) {
    if (change.setting->kind != SettingKind::secret) continue;
    if (!change.value) {
      if (!remove_plugin_secret(*settings_.secret_dir, id, change.setting->name)) return refused(change.setting->name + " could not be removed");
    } else if (const auto problem = store_plugin_secret(*settings_.secret_dir, id, change.setting->name, std::get<std::string>(*change.value))) {
      return refused(change.setting->name + ": " + *problem);
    }
  }
  if (settings_.log) settings_.log("[strata] plugins: " + id + "'s settings were saved (they apply at the next start)");
  return json_answer(200, "{\"id\":" + json_text(id) + ",\"saved\":true,\"restart_needed\":true}\n");
}

}  // namespace secure_frontend

namespace secure_frontend {

HttpResponse PluginRoutes::try_tool(const std::string_view body) {
  // {"id", "tool", "arguments": {...}}, nothing else
  constexpr JsonLimits kTryLimits{.max_bytes = kModelControlBodyLimit, .max_depth = 4U, .max_container_entries = 64U, .max_string_bytes = 4096U,
                                  .allow_fractions = true};
  const auto parsed = parse_json(body, kTryLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root == nullptr ? nullptr : root->if_object();
  const JsonValue* id = nullptr;
  const JsonValue* tool = nullptr;
  const JsonValue* arguments = nullptr;
  if (members != nullptr && members->size() == 3U) {
    id = member_of(*root, "id");
    tool = member_of(*root, "tool");
    arguments = member_of(*root, "arguments");
  }
  if (id == nullptr || id->if_string() == nullptr || tool == nullptr || tool->if_string() == nullptr || tool->if_string()->empty() ||
      tool->if_string()->size() > 128U || arguments == nullptr || arguments->if_object() == nullptr) {
    return make_error_response(ApiError::invalid_request);
  }
  const std::string& name = *id->if_string();
  const auto chosen = choices();
  if (!chosen) return unreadable_choices();
  const auto entries = discover_plugins(settings_.plugins_dir, settings_.first_party, *chosen, settings_.install_key);
  const auto entry = std::find_if(entries.begin(), entries.end(), [&name](const PluginEntry& e) { return e.id == name; });
  if (entry == entries.end()) return refused("there is no plugin " + name + " in the plugins folder");
  if (!entry->manifest || entry->manifest->kind != PluginKind::tool) return refused(name + " is not a tool plugin");
  if (!settings_.try_tool) return refused("this server runs no tool plugins: one runs once it is on and its settings are filled in, after a restart");
  std::string arguments_json;
  compact(*arguments, arguments_json);
  const ToolResult result = settings_.try_tool(name, *tool->if_string(), arguments_json);
  return json_answer(200, std::string{"{\"ok\":"} + (result.ok ? "true" : "false") + ",\"text\":" + json_text(result.text) +
                              ",\"truncated\":" + (result.truncated ? "true" : "false") + ",\"ms\":" + std::to_string(result.took.count()) + "}\n");
}

}  // namespace secure_frontend

namespace secure_frontend {

HttpResponse PluginRoutes::test_notifier(const std::string_view body) {
  const auto request = parse_request(body, false);
  if (!request) return make_error_response(ApiError::invalid_request);
  const auto chosen = choices();
  if (!chosen) return unreadable_choices();
  const auto entries = discover_plugins(settings_.plugins_dir, settings_.first_party, *chosen, settings_.install_key);
  const auto entry = std::find_if(entries.begin(), entries.end(), [&request](const PluginEntry& e) { return e.id == request->id; });
  if (entry == entries.end()) return refused("there is no plugin " + request->id + " in the plugins folder");
  if (!entry->manifest || entry->manifest->kind != PluginKind::notifier) return refused(request->id + " is not a notifier plugin");
  if (!settings_.test_notifier) {
    return refused("this server runs no notifier plugins: one runs once it is on and its settings are filled in, after a restart");
  }
  const NotifierTest result = settings_.test_notifier(request->id);
  return json_answer(200, std::string{"{\"ok\":"} + (result.ok ? "true" : "false") + ",\"reply\":" + json_text(result.reply) + "}\n");
}

}  // namespace secure_frontend

namespace secure_frontend {

HttpResponse PluginRoutes::keys(const std::string_view body) {
  // {"add": "<64 hex>", "name"} or {"remove": "<64 hex>"}, nothing else
  const auto parsed = parse_json(body, kBodyLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root == nullptr ? nullptr : root->if_object();
  const JsonValue* add = members != nullptr && members->size() == 2U ? member_of(*root, "add") : nullptr;
  const JsonValue* name = add != nullptr ? member_of(*root, "name") : nullptr;
  const JsonValue* remove = members != nullptr && members->size() == 1U ? member_of(*root, "remove") : nullptr;
  const bool adding = add != nullptr && add->if_string() != nullptr && name != nullptr && name->if_string() != nullptr;
  const bool removing = remove != nullptr && remove->if_string() != nullptr;
  if (!adding && !removing) return make_error_response(ApiError::invalid_request);
  if (!settings_.state_dir) return refused("this server keeps no choices: start it with a state folder to trust a publisher's key");
  const std::string key = adding ? *add->if_string() : *remove->if_string();
  if (!valid_signing_key(key)) return refused("a key is its public half, 64 lowercase hex characters");
  if (key == settings_.install_key) {
    return refused(adding ? "that is the install's own key: it is trusted already" : "the install's own key cannot be removed here");
  }
  auto chosen = choices();
  if (!chosen) return unreadable_choices();
  if (adding) {
    if (!valid_key_name(*name->if_string())) return refused("the name must be 1-64 printable characters");
    if (!chosen->keys.contains(key) && chosen->keys.size() >= kMaxPublisherKeys) return refused("at most 16 publisher keys can be trusted");
    chosen->keys[key] = *name->if_string();
  } else {
    if (chosen->keys.erase(key) == 0U) return refused("that key is not one this server trusts");
  }
  if (!save(*chosen)) return refused("plugins.json could not be written");
  if (settings_.log) {
    settings_.log(adding ? "[strata] plugins: the publisher key " + key.substr(0, 16) + "... (" + *name->if_string() + ") is trusted"
                         : "[strata] plugins: the publisher key " + key.substr(0, 16) + "... is no longer trusted");
  }
  return json_answer(200, "{\"keys\":" + keys_json(trusted_keys(settings_.install_key, *chosen)) + ",\"restart_needed\":true}\n");
}

}  // namespace secure_frontend
