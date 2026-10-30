#include "secure_frontend/plugin_manifest.hpp"

#include "secure_frontend/json.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>
#include <utility>

namespace secure_frontend {
namespace {

constexpr JsonLimits kLimits{.max_bytes = kMaxPluginManifestBytes, .max_depth = 8U, .max_container_entries = 256U,
                             .max_string_bytes = 4096U, .allow_fractions = false};

constexpr std::array<std::pair<std::string_view, PluginKind>, 4> kKinds{
    {{"engine", PluginKind::engine}, {"tool", PluginKind::tool}, {"notifier", PluginKind::notifier}, {"diagnostics", PluginKind::diagnostics}}};
constexpr std::array<std::pair<std::string_view, DiagnosticSwitch>, 3> kSwitches{
    {{"logging.requests", DiagnosticSwitch::log_requests},
     {"logging.engine_output", DiagnosticSwitch::log_engine_output},
     {"troubleshooting", DiagnosticSwitch::troubleshooting}}};

#if defined(_WIN32)
constexpr std::string_view kPlatform = "windows";
#else
constexpr std::string_view kPlatform = "linux";
#endif

const JsonValue* member(const JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& entry : *members) {
    if (entry.key == key) return &entry.value;
  }
  return nullptr;
}

bool printable(const std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](const char c) { return c >= 0x20 && c < 0x7F; });
}

// A text of printable ASCII, `low` to `high` long; why not, naming the key.
std::variant<std::string, std::string> text_of(const JsonValue& root, const std::string_view key, const std::size_t low,
                                               const std::size_t high) {
  const JsonValue* value = member(root, key);
  const std::string* text = value == nullptr ? nullptr : value->if_string();
  if (text == nullptr || text->size() < low || text->size() > high || !printable(*text)) {
    return std::variant<std::string, std::string>{std::in_place_index<1>,
                                                  std::string{key} + " must be printable text of " + std::to_string(low) +
                                                      " to " + std::to_string(high) + " characters"};
  }
  return std::variant<std::string, std::string>{std::in_place_index<0>, *text};
}

bool version_text(const std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](const char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '+';
  });
}

// A file of the plugin's folder: '/'-separated names of letters, digits, '.', '-', '_', none of them "." or "..".
bool inside_path(const std::string_view path) {
  if (path.empty() || path.size() > 128U || path.front() == '/' || path.back() == '/') return false;
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto end = path.find('/', start);
    const auto part = path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (part.empty() || part == "." || part == "..") return false;
    for (const char c : part) {
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) {
        return false;
      }
    }
    if (end == std::string_view::npos) break;
    start = end + 1U;
  }
  return true;
}


constexpr std::array<std::pair<std::string_view, SettingKind>, 7> kSettingKinds{
    {{"text", SettingKind::text}, {"url", SettingKind::url}, {"secret", SettingKind::secret}, {"integer", SettingKind::integer},
     {"boolean", SettingKind::boolean}, {"choice", SettingKind::choice}, {"text_list", SettingKind::text_list}}};

constexpr std::size_t kMaxValueBytes = 1024U;
constexpr std::size_t kMaxListItems = 16U;

// A setting's name: a lowercase letter, then lowercase letters, digits or '_' (as a plugin id).
bool setting_name(const std::string_view name) noexcept { return valid_plugin_id(name); }

// An environment variable's name the manifest may set: an uppercase letter or '_', then uppercase letters, digits or
// '_'; never one the server's children rely on (PATH, SYSTEMROOT, TEMP, TMP, HOME) or the server's own (STRATA_*).
bool environment_name(const std::string_view name) noexcept {
  if (name.empty() || name.size() > 64U || (name.front() >= '0' && name.front() <= '9')) return false;
  if (!std::all_of(name.begin(), name.end(), [](const char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; })) {
    return false;
  }
  return name != "PATH" && name != "SYSTEMROOT" && name != "TEMP" && name != "TMP" && name != "HOME" && !name.starts_with("STRATA_");
}

bool http_url(const std::string_view text) noexcept {
  const auto rest = text.starts_with("https://") ? text.substr(8U) : text.starts_with("http://") ? text.substr(7U) : std::string_view{};
  return !rest.empty() && rest.front() != '/' && text.size() <= kMaxValueBytes && printable(text);
}

// "{name}" exactly: the setting it names; else nullopt.
std::optional<std::string> template_setting(const std::string_view text) {
  if (text.size() < 3U || text.front() != '{' || text.back() != '}') return std::nullopt;
  return std::string{text.substr(1U, text.size() - 2U)};
}

// The names inside {...} in a text (network's words); nullopt when a brace is left open or a name is not one.
std::optional<std::vector<std::string>> names_in(const std::string_view text) {
  std::vector<std::string> names;
  std::size_t at = 0;
  while (at < text.size()) {
    const auto open = text.find_first_of("{}", at);
    if (open == std::string_view::npos) break;
    if (text[open] == '}') return std::nullopt;
    const auto close = text.find('}', open);
    if (close == std::string_view::npos) return std::nullopt;
    names.emplace_back(text.substr(open + 1U, close - open - 1U));
    at = close + 1U;
  }
  return names;
}

// A JSON value as a setting's value (a whole number, a boolean, a text, a list of texts); nullopt: none of these.
std::optional<PluginValue> plugin_value(const JsonValue& value) {
  if (const auto* b = value.if_boolean()) return PluginValue{*b};
  if (const auto* i = value.if_integer()) return PluginValue{*i};
  if (const auto* s = value.if_string()) return PluginValue{*s};
  if (const auto* items = value.if_array()) {
    std::vector<std::string> texts;
    for (const auto& item : *items) {
      const auto* s = item.if_string();
      if (s == nullptr) return std::nullopt;
      texts.push_back(*s);
    }
    return PluginValue{std::move(texts)};
  }
  return std::nullopt;
}

const PluginSetting* setting_of(const std::vector<PluginSetting>& settings, const std::string_view name) {
  const auto found = std::find_if(settings.begin(), settings.end(), [name](const PluginSetting& s) { return s.name == name; });
  return found == settings.end() ? nullptr : &*found;
}

// One declared setting; or why not, naming it.
std::variant<PluginSetting, std::string> parse_setting(const JsonValue& item) {
  const auto* members = item.if_object();
  if (members == nullptr) return std::string{"each setting must be an object"};
  constexpr std::array<std::string_view, 10> kSettingKeys{"name", "kind", "label", "help", "required", "default",
                                                          "min", "max", "choices", "max_items"};
  const JsonValue* name = member(item, "name");
  if (name == nullptr || name->if_string() == nullptr || !setting_name(*name->if_string())) {
    return std::string{"each setting's name must be a lowercase letter, then lowercase letters, digits or '_'"};
  }
  PluginSetting setting;
  setting.name = *name->if_string();
  const std::string about = "setting " + setting.name + ": ";
  for (const auto& entry : *members) {
    if (std::find(kSettingKeys.begin(), kSettingKeys.end(), entry.key) == kSettingKeys.end()) {
      return about + "unknown key " + (printable(entry.key) && entry.key.size() <= 64U ? entry.key : std::string{"(unprintable)"});
    }
  }
  const JsonValue* kind = member(item, "kind");
  const auto known = kind == nullptr || kind->if_string() == nullptr
                         ? kSettingKinds.end()
                         : std::find_if(kSettingKinds.begin(), kSettingKinds.end(), [&kind](const auto& entry) { return entry.first == *kind->if_string(); });
  if (known == kSettingKinds.end()) return about + "kind must be text, url, secret, integer, boolean, choice or text_list";
  setting.kind = known->second;
  for (const auto& [key, field, low, high] : {std::tuple<std::string_view, std::string*, std::size_t, std::size_t>{"label", &setting.label, 1U, 64U},
                                              {"help", &setting.help, 0U, 400U}}) {
    auto read = text_of(item, key, low, high);
    if (read.index() == 1U) return about + std::get<1>(std::move(read));
    *field = std::get<0>(std::move(read));
  }
  if (const JsonValue* required = member(item, "required")) {
    if (required->if_boolean() == nullptr) return about + "required must be true or false";
    setting.required = *required->if_boolean();
  }
  const auto whole = [&item](const std::string_view key) -> std::optional<std::int64_t> {
    const JsonValue* value = member(item, key);
    return value != nullptr && value->if_integer() != nullptr ? std::optional<std::int64_t>{*value->if_integer()} : std::nullopt;
  };
  const bool integer = setting.kind == SettingKind::integer;
  if (integer) {
    const auto low = whole("min");
    const auto high = whole("max");
    if (!low || !high || *low > *high) return about + "an integer needs whole-number min and max, min not above max";
    setting.min = *low;
    setting.max = *high;
  } else if (member(item, "min") != nullptr || member(item, "max") != nullptr) {
    return about + "only an integer has min and max";
  }
  const JsonValue* choices = member(item, "choices");
  const bool listed = setting.kind == SettingKind::choice || setting.kind == SettingKind::text_list;
  if (choices != nullptr) {
    const auto* list = choices->if_array();
    if (!listed || list == nullptr || list->empty() || list->size() > kMaxListItems) {
      return about + "choices belong to a choice or a text list: 1-16 texts";
    }
    for (const auto& choice : *list) {
      const std::string* text = choice.if_string();
      if (text == nullptr || text->empty() || text->size() > 64U || !printable(*text) ||
          std::find(setting.choices.begin(), setting.choices.end(), *text) != setting.choices.end()) {
        return about + "each choice must be printable text of 1 to 64 characters, each once";
      }
      setting.choices.push_back(*text);
    }
  } else if (setting.kind == SettingKind::choice) {
    return about + "a choice needs its choices";
  }
  if (const auto most = whole("max_items")) {
    if (setting.kind != SettingKind::text_list || *most < 1 || *most > static_cast<std::int64_t>(kMaxListItems)) {
      return about + "max_items belongs to a text list: 1-16";
    }
    setting.max_items = static_cast<std::size_t>(*most);
  } else if (member(item, "max_items") != nullptr) {
    return about + "max_items must be a whole number";
  } else if (setting.kind == SettingKind::text_list) {
    setting.max_items = kMaxListItems;
  }
  if (const JsonValue* fallback = member(item, "default")) {
    if (setting.kind == SettingKind::secret) return about + "a secret has no default";
    const auto value = plugin_value(*fallback);
    if (!value) return about + "its default must be of its kind";
    if (const auto problem = setting_value_problem(setting, *value)) return "default of " + *problem;
    setting.default_value = *value;
  }
  return setting;
}

// A template: a fixed text with no braces, or "{setting}" naming a declared one (never a secret's where `secret_ok` is
// false); or why not.
std::variant<PluginTemplate, std::string> parse_template(const std::string_view where, const JsonValue& value,
                                                         const std::vector<PluginSetting>& settings, const bool secret_ok) {
  const std::string* text = value.if_string();
  if (text == nullptr || text->size() > 256U || !printable(*text)) return std::string{where} + " must be printable texts of at most 256 characters";
  if (const auto name = template_setting(*text)) {
    const PluginSetting* setting = setting_of(settings, *name);
    if (setting == nullptr) return std::string{where} + " names " + *name + ", not one of its settings";
    if (setting->kind == SettingKind::secret && !secret_ok) {
      return std::string{where} + " may not hold the secret " + *name + " (a secret goes only in its environment)";
    }
    return PluginTemplate{.text = {}, .setting = *name};
  }
  if (text->find_first_of("{}") != std::string::npos) {
    return std::string{where} + " are each a fixed text or one {setting}, whole: " + *text;
  }
  return PluginTemplate{.text = *text, .setting = {}};
}

// A tool's or notifier's block (spec section 10); or why not.
std::variant<PluginRun, std::string> parse_run(const JsonValue& block, const PluginKind kind, const std::vector<std::string>& files) {
  const std::string_view block_name = kind == PluginKind::tool ? "tool" : "notifier";
  constexpr std::array<std::string_view, 8> kRunKeys{"program", "arguments", "environment", "network", "note", "call_timeout_s",
                                                     "settings", "presets"};
  for (const auto& entry : *block.if_object()) {
    const bool tool_only = entry.key == "note" || entry.key == "call_timeout_s";
    if (tool_only && kind != PluginKind::tool) return std::string{block_name} + ": a notifier has no " + entry.key;
    if (std::find(kRunKeys.begin(), kRunKeys.end(), entry.key) == kRunKeys.end()) {
      return std::string{block_name} + ": unknown key " +
             (printable(entry.key) && entry.key.size() <= 64U ? entry.key : std::string{"(unprintable)"});
    }
  }
  PluginRun run;
  // its settings first: the rest name them
  if (const JsonValue* settings = member(block, "settings")) {
    const auto* list = settings->if_array();
    if (list == nullptr || list->size() > kMaxPluginSettings) return std::string{"settings lists at most 16 settings"};
    for (const auto& item : *list) {
      auto parsed = parse_setting(item);
      if (const auto* problem = std::get_if<std::string>(&parsed)) return *problem;
      auto& setting = std::get<PluginSetting>(parsed);
      if (setting_of(run.settings, setting.name) != nullptr) return "setting " + setting.name + " is declared twice";
      run.settings.push_back(std::move(setting));
    }
  }
  const JsonValue* program = member(block, "program");
  const JsonValue* mine = program != nullptr ? member(*program, kPlatform) : nullptr;
  if (mine == nullptr || mine->if_string() == nullptr || std::find(files.begin(), files.end(), *mine->if_string()) == files.end()) {
    return std::string{"program must name this platform's program, one of its files ("} + std::string{kPlatform} + ")";
  }
  run.program = *mine->if_string();
  if (const JsonValue* arguments = member(block, "arguments")) {
    const auto* list = arguments->if_array();
    if (list == nullptr || list->size() > 32U) return std::string{"arguments lists at most 32 texts"};
    for (const auto& item : *list) {
      auto parsed = parse_template("arguments", item, run.settings, false);
      if (const auto* problem = std::get_if<std::string>(&parsed)) return *problem;
      // (an argument always has its value: a setting it names is required, or has a default)
      if (const auto& name = std::get<PluginTemplate>(parsed).setting; !name.empty()) {
        const PluginSetting* setting = setting_of(run.settings, name);
        if (!setting->required && !setting->default_value) {
          return "arguments name " + name + ": a setting an argument names needs a value - make it required, or give it a default";
        }
      }
      run.arguments.push_back(std::get<PluginTemplate>(std::move(parsed)));
    }
  }
  if (const JsonValue* environment = member(block, "environment")) {
    const auto* entries = environment->if_object();
    if (entries == nullptr || entries->size() > 16U) return std::string{"environment sets at most 16 variables"};
    for (const auto& entry : *entries) {
      if (!environment_name(entry.key)) {
        return "environment may not set " + (printable(entry.key) && entry.key.size() <= 64U ? entry.key : std::string{"(unprintable)"}) +
               ": a name is uppercase letters, digits and '_', never PATH, SYSTEMROOT, TEMP, TMP, HOME or STRATA_*";
      }
      auto parsed = parse_template("environment values", entry.value, run.settings, true);
      if (const auto* problem = std::get_if<std::string>(&parsed)) return *problem;
      run.environment.emplace_back(entry.key, std::get<PluginTemplate>(std::move(parsed)));
    }
  }
  for (const auto& [key, field, high] : {std::tuple<std::string_view, std::string*, std::size_t>{"network", &run.network, 200U},
                                         {"note", &run.note, 1000U}}) {
    if (member(block, key) == nullptr) continue;
    auto read = text_of(block, key, 0U, high);
    if (read.index() == 1U) return std::get<1>(std::move(read));
    *field = std::get<0>(std::move(read));
  }
  if (const auto names = names_in(run.network); !names) {
    return std::string{"network may name settings only as {setting}"};
  } else {
    for (const auto& name : *names) {
      const PluginSetting* setting = setting_of(run.settings, name);
      if (setting == nullptr || setting->kind == SettingKind::secret) {
        return "network names " + name + ": not one of its settings, or a secret";
      }
    }
  }
  if (const auto names = names_in(run.note); !names || std::any_of(names->begin(), names->end(), [](const std::string& n) { return n != "today"; })) {
    return std::string{"note may hold only {today}"};
  }
  if (const JsonValue* limit = member(block, "call_timeout_s")) {
    const auto* seconds = limit->if_integer();
    if (seconds == nullptr || *seconds < 1 || *seconds > 600) return std::string{"call_timeout_s must be a whole number of seconds, 1-600"};
    run.call_timeout = std::chrono::seconds(*seconds);
  }
  if (const JsonValue* presets = member(block, "presets")) {
    const auto* list = presets->if_array();
    if (list == nullptr || list->size() > 8U) return std::string{"presets lists at most 8 presets"};
    for (const auto& item : *list) {
      const JsonValue* values = member(item, "values");
      if (item.if_object() == nullptr || item.if_object()->size() != 2U || values == nullptr || values->if_object() == nullptr) {
        return std::string{"each preset is {\"label\", \"values\"}"};
      }
      auto label = text_of(item, "label", 1U, 32U);
      if (label.index() == 1U) return "preset " + std::get<1>(std::move(label));
      PluginPreset preset{.label = std::get<0>(std::move(label)), .values = {}};
      for (const auto& entry : *values->if_object()) {
        const PluginSetting* setting = setting_of(run.settings, entry.key);
        const auto value = plugin_value(entry.value);
        if (setting == nullptr || setting->kind == SettingKind::secret || !value) {
          return "preset " + preset.label + " sets " + (printable(entry.key) && entry.key.size() <= 64U ? entry.key : std::string{"(unprintable)"}) +
                 ": not one of its settings, a secret, or not a value";
        }
        if (const auto problem = setting_value_problem(*setting, *value)) return "preset " + preset.label + ": " + *problem;
        preset.values[entry.key] = *value;
      }
      run.presets.push_back(std::move(preset));
    }
  }
  return run;
}

}  // namespace

std::string_view setting_kind_name(const SettingKind kind) noexcept {
  for (const auto& [name, value] : kSettingKinds) {
    if (value == kind) return name;
  }
  return "text";  // unreachable: every kind is in kSettingKinds
}

std::optional<std::string> setting_value_problem(const PluginSetting& setting, const PluginValue& value) {
  const std::string about = setting.name + " must be ";
  const auto* text = std::get_if<std::string>(&value);
  switch (setting.kind) {
    case SettingKind::text:
    case SettingKind::secret:
      if (text == nullptr || text->size() > kMaxValueBytes || !printable(*text)) return about + "printable text of at most 1024 characters";
      return std::nullopt;
    case SettingKind::url:
      if (text == nullptr || !http_url(*text)) return about + "an http:// or https:// address";
      return std::nullopt;
    case SettingKind::integer: {
      const auto* number = std::get_if<std::int64_t>(&value);
      if (number == nullptr || *number < setting.min || *number > setting.max) {
        return about + "a whole number from " + std::to_string(setting.min) + " to " + std::to_string(setting.max);
      }
      return std::nullopt;
    }
    case SettingKind::boolean:
      if (!std::holds_alternative<bool>(value)) return about + "true or false";
      return std::nullopt;
    case SettingKind::choice:
      if (text == nullptr || std::find(setting.choices.begin(), setting.choices.end(), *text) == setting.choices.end()) {
        std::string listed;
        for (const auto& choice : setting.choices) listed += (listed.empty() ? "" : ", ") + choice;
        return about + "one of " + listed;
      }
      return std::nullopt;
    case SettingKind::text_list: {
      const auto* items = std::get_if<std::vector<std::string>>(&value);
      if (items == nullptr || items->size() > setting.max_items) {
        return about + "a list of at most " + std::to_string(setting.max_items) + " texts";
      }
      for (const auto& item : *items) {
        const bool allowed = setting.choices.empty() ? !item.empty() && item.size() <= 256U && printable(item)
                                                     : std::find(setting.choices.begin(), setting.choices.end(), item) != setting.choices.end();
        if (!allowed) return about + (setting.choices.empty() ? "a list of printable texts of 1 to 256 characters" : "a list of its choices");
      }
      return std::nullopt;
    }
  }
  return about + "of its kind";  // unreachable
}

std::variant<std::map<std::string, PluginValue>, std::string> plugin_values(const PluginRun& run, const std::map<std::string, PluginValue>& set,
                                                                         const std::set<std::string>& secrets_set) {
  std::map<std::string, PluginValue> values;
  for (const auto& setting : run.settings) {
    if (setting.kind == SettingKind::secret) {
      if (setting.required && !secrets_set.contains(setting.name)) return "its setting " + setting.name + " is required: set it in the Plugins tab";
      continue;
    }
    const auto found = set.find(setting.name);
    if (found != set.end()) {
      if (const auto problem = setting_value_problem(setting, found->second)) return "its setting " + *problem;
      values[setting.name] = found->second;
    } else if (setting.default_value) {
      values[setting.name] = *setting.default_value;
    } else if (setting.required) {
      return "its setting " + setting.name + " is required: set it in the Plugins tab";
    }
  }
  return values;
}

std::string value_text(const PluginValue& value) {
  if (const auto* b = std::get_if<bool>(&value)) return *b ? "true" : "false";
  if (const auto* i = std::get_if<std::int64_t>(&value)) return std::to_string(*i);
  if (const auto* s = std::get_if<std::string>(&value)) return *s;
  std::string joined;
  for (const auto& item : std::get<std::vector<std::string>>(value)) joined += (joined.empty() ? "" : ",") + item;
  return joined;
}

std::string_view plugin_kind_name(const PluginKind kind) noexcept {
  for (const auto& [name, value] : kKinds) {
    if (value == kind) return name;
  }
  return "diagnostics";  // unreachable: every kind is in kKinds
}

std::string_view diagnostic_switch_name(const DiagnosticSwitch value) noexcept {
  for (const auto& [name, entry] : kSwitches) {
    if (entry == value) return name;
  }
  return "troubleshooting";  // unreachable: every switch is in kSwitches
}

bool valid_plugin_id(const std::string_view id) noexcept {
  if (id.empty() || id.size() > 32U || id.front() < 'a' || id.front() > 'z') return false;
  return std::all_of(id.begin(), id.end(), [](const char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

std::variant<PluginManifest, std::string> parse_plugin_manifest(const std::string_view text, const std::string_view folder_id) {
  if (text.size() > kMaxPluginManifestBytes) return std::string{"the manifest is too large"};
  const auto parsed = parse_json(text, kLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  if (root == nullptr || root->if_object() == nullptr) return std::string{"the manifest is not a JSON object"};
  constexpr std::array<std::string_view, 13> kKeys{"schema", "id", "kind", "name", "version", "publisher", "license", "about",
                                                   "files", "engine", "tool", "notifier", "diagnostics"};
  for (const auto& entry : *root->if_object()) {
    if (std::find(kKeys.begin(), kKeys.end(), entry.key) == kKeys.end()) {
      return "unknown key " + (printable(entry.key) && entry.key.size() <= 64U ? entry.key : std::string{"(unprintable)"});
    }
  }
  const JsonValue* schema = member(*root, "schema");
  if (schema == nullptr || schema->if_string() == nullptr || *schema->if_string() != "m-strata.plugin/1") {
    return std::string{"schema must be m-strata.plugin/1"};
  }
  PluginManifest manifest;
  const JsonValue* id = member(*root, "id");
  if (id == nullptr || id->if_string() == nullptr || !valid_plugin_id(*id->if_string()) || *id->if_string() != folder_id) {
    return std::string{"id must be the plugin folder's name (a lowercase letter, then lowercase letters, digits or '_')"};
  }
  manifest.id = *id->if_string();
  const JsonValue* kind = member(*root, "kind");
  const auto known = kind == nullptr || kind->if_string() == nullptr
                         ? kKinds.end()
                         : std::find_if(kKinds.begin(), kKinds.end(), [&kind](const auto& entry) { return entry.first == *kind->if_string(); });
  if (known == kKinds.end()) return std::string{"kind must be engine, tool, notifier or diagnostics"};
  manifest.kind = known->second;
  for (const auto& [key, field, low, high] :
       {std::tuple<std::string_view, std::string*, std::size_t, std::size_t>{"name", &manifest.name, 1U, 64U},
        {"version", &manifest.version, 1U, 32U},
        {"publisher", &manifest.publisher, 1U, 64U},
        {"license", &manifest.license, 1U, 64U},
        {"about", &manifest.about, 0U, 1000U}}) {
    auto read = text_of(*root, key, low, high);
    if (read.index() == 1U) return std::get<1>(std::move(read));
    *field = std::get<0>(std::move(read));
  }
  if (!version_text(manifest.version)) return std::string{"version may hold only letters, digits, '.', '-' and '+'"};
  // its kind's block, and no other kind's
  for (const auto& [name, value] : kKinds) {
    const JsonValue* block = member(*root, name);
    if (value == manifest.kind && (block == nullptr || block->if_object() == nullptr)) {
      return std::string{name} + " must be an object: a " + std::string{name} + " plugin's own settings";
    }
    if (value != manifest.kind && block != nullptr) {
      return std::string{name} + " belongs to another kind of plugin";
    }
  }
  // this platform's code files
  const JsonValue* files = member(*root, "files");
  if (files != nullptr) {
    if (files->if_object() == nullptr) return std::string{"files must name each platform's files"};
    for (const auto& entry : *files->if_object()) {
      if (entry.key != "windows" && entry.key != "linux") return std::string{"files may name only windows and linux"};
      const auto* list = entry.value.if_array();
      if (list == nullptr || list->size() > kMaxPluginFiles) return std::string{"files lists at most 16 files per platform"};
      for (const auto& item : *list) {
        const std::string* path = item.if_string();
        if (path == nullptr || !inside_path(*path)) {
          return std::string{"files must be inside the plugin's folder, '/'-separated, with no '.' or '..'"};
        }
        if (entry.key == kPlatform) manifest.files.push_back(*path);
      }
    }
  }
  if (manifest.kind == PluginKind::diagnostics) {
    if (files != nullptr) return std::string{"a diagnostics plugin has no files"};
    const JsonValue* switches = member(*member(*root, "diagnostics"), "switches");
    const auto* list = switches == nullptr ? nullptr : switches->if_array();
    if (list == nullptr || list->empty() || list->size() > kSwitches.size()) {
      return std::string{"diagnostics needs its switches: logging.requests, logging.engine_output, troubleshooting"};
    }
    for (const auto& item : *list) {
      const std::string* name = item.if_string();
      const auto found = name == nullptr ? kSwitches.end()
                                         : std::find_if(kSwitches.begin(), kSwitches.end(), [&name](const auto& entry) { return entry.first == *name; });
      if (found == kSwitches.end() || std::find(manifest.switches.begin(), manifest.switches.end(), found->second) != manifest.switches.end()) {
        return std::string{"diagnostics switches may be logging.requests, logging.engine_output and troubleshooting, each once"};
      }
      manifest.switches.push_back(found->second);
    }
    if (member(*root, "diagnostics")->if_object()->size() != 1U) return std::string{"diagnostics holds only its switches"};
  } else if (manifest.files.empty()) {
    return std::string{"files must name this platform's files ("} + std::string{kPlatform} + ")";
  }
  if (manifest.kind == PluginKind::tool || manifest.kind == PluginKind::notifier) {
    auto run = parse_run(*member(*root, plugin_kind_name(manifest.kind)), manifest.kind, manifest.files);
    if (const auto* problem = std::get_if<std::string>(&run)) return *problem;
    manifest.run = std::get<PluginRun>(std::move(run));
  }
  return manifest;
}

}  // namespace secure_frontend
