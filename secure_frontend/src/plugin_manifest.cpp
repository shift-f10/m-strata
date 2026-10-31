#include "secure_frontend/plugin_manifest.hpp"

#include "secure_frontend/json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <initializer_list>
#include <limits>
#include <system_error>
#include <tuple>
#include <utility>

namespace secure_frontend {
namespace {

constexpr JsonLimits kLimits{.max_bytes = kMaxPluginManifestBytes, .max_depth = 8U, .max_container_entries = 256U,
                             .max_string_bytes = 4096U, .allow_fractions = true};  // (an engine's sampling and margin)

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

// ---------------------------------------------------------------------------------------------------------------------
// The engine block (plugin spec section 3, as built; plugin_manifest.hpp): what an engine plugin's family descriptor
// will hold, each part from a closed list this server implements, and a wrong one refused by name.

constexpr std::array<std::pair<std::string_view, EngineProtocolFlavor>, 2> kDialects{
    {{"strata_0143", EngineProtocolFlavor::strata_0143}, {"unified", EngineProtocolFlavor::unified}}};
constexpr std::array<std::pair<std::string_view, NativeChatProfile>, 7> kRenderers{
    {{"qwen", NativeChatProfile::qwen}, {"glm53", NativeChatProfile::glm53}, {"gemma4", NativeChatProfile::gemma4},
     {"deepseek", NativeChatProfile::deepseek}, {"raon1", NativeChatProfile::raon1}, {"flint", NativeChatProfile::flint},
     {"minimax", NativeChatProfile::minimax}}};
constexpr std::array<std::pair<std::string_view, TokenizerMode>, 7> kTokenizerModes{
    {{"byte_bpe", TokenizerMode::byte_bpe}, {"spm_bpe", TokenizerMode::spm_bpe}, {"raon_bpe", TokenizerMode::raon_bpe},
     {"flint_bpe", TokenizerMode::flint_bpe}, {"deepseek_bpe", TokenizerMode::deepseek_bpe}, {"glm_bpe", TokenizerMode::glm_bpe},
     {"minimax_bpe", TokenizerMode::minimax_bpe}}};
constexpr std::array<std::pair<std::string_view, PluginArgumentValue>, 7> kArgumentValues{
    {{"none", PluginArgumentValue::none}, {"integer", PluginArgumentValue::integer}, {"fraction", PluginArgumentValue::fraction},
     {"gibibytes", PluginArgumentValue::gibibytes}, {"on_off", PluginArgumentValue::on_off},
     {"gguf_file", PluginArgumentValue::gguf_file}, {"choice", PluginArgumentValue::choice}}};
constexpr std::array<std::pair<std::string_view, ModelOptionKind>, 6> kOptionKinds{
    {{"boolean", ModelOptionKind::boolean}, {"integer", ModelOptionKind::integer}, {"decimal", ModelOptionKind::decimal},
     {"choice", ModelOptionKind::choice}, {"integer_or_auto", ModelOptionKind::integer_or_auto}, {"devices", ModelOptionKind::devices}}};
constexpr std::array<std::pair<std::string_view, DraftLayerKind>, 3> kMtpKinds{
    {{"none", DraftLayerKind::none}, {"file", DraftLayerKind::file}, {"in_model", DraftLayerKind::in_model}}};
// what the server composes for every engine plugin: never one of its own flags
constexpr std::array<std::string_view, 6> kComposedFlags{"--native", "--pack", "--family", "--family-sha256", "--gpu", "--serve"};
constexpr std::size_t kMaxEngineArguments = 32U;
constexpr std::size_t kMaxEngineOptions = 24U;
constexpr std::size_t kMaxEngineChoices = 16U;
constexpr std::size_t kMaxOptionValues = 32U;
constexpr std::size_t kMaxCalibrationSteps = 4U;
constexpr std::size_t kMaxCandidates = 8U;
constexpr std::int64_t kMaxEngineInteger = std::int64_t{1} << 40U;

template <typename T, std::size_t N>
std::optional<T> named(const std::array<std::pair<std::string_view, T>, N>& table, const JsonValue* value) {
  const std::string* text = value == nullptr ? nullptr : value->if_string();
  if (text == nullptr) return std::nullopt;
  for (const auto& [name, item] : table) {
    if (name == *text) return item;
  }
  return std::nullopt;
}
template <typename T, std::size_t N>
std::string name_in(const std::array<std::pair<std::string_view, T>, N>& table, const T value) {
  for (const auto& [name, item] : table) {
    if (item == value) return std::string{name};
  }
  return {};
}

// `object` holds only the keys in `allowed` and every one in `required`; else why not, at `where`.
std::optional<std::string> keys_problem(const JsonValue* object, const std::string& where, const std::initializer_list<std::string_view> allowed,
                                        const std::initializer_list<std::string_view> required) {
  if (object == nullptr || object->if_object() == nullptr) return where + " must be an object";
  for (const auto& entry : *object->if_object()) {
    if (std::find(allowed.begin(), allowed.end(), entry.key) == allowed.end()) {
      return where + " has an unknown key " + (printable(entry.key) && entry.key.size() <= 64U ? entry.key : std::string{"(unprintable)"});
    }
  }
  for (const auto key : required) {
    if (member(*object, key) == nullptr) return where + " needs " + std::string{key};
  }
  return std::nullopt;
}

std::optional<double> number_of(const JsonValue* value) {
  if (value == nullptr) return std::nullopt;
  if (const auto* whole = value->if_integer()) return static_cast<double>(*whole);
  if (const auto* decimal = value->if_decimal()) return *decimal;
  return std::nullopt;
}

// Lowercase letters and digits, and `extra`; 1 to `most` characters, the first a letter or a digit.
bool word(const std::string_view text, const std::size_t most, const std::string_view extra) {
  const auto plain = [](const char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
  return !text.empty() && text.size() <= most && plain(text.front()) &&
         std::all_of(text.begin(), text.end(), [&](const char c) { return plain(c) || extra.find(c) != std::string_view::npos; });
}
bool flag_name(const std::string_view text) { return text.starts_with("--") && word(text.substr(2U), 31U, "-"); }

std::string decimal_text(const double value) {
  char buffer[32];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, value);
  return error == std::errc{} ? std::string(buffer, end) : std::string{"0"};
}

// 1 to 16, as a calibration candidate's N or M is written: one or two digits
int small_number(const std::string_view digits) {
  if (digits.empty() || digits.size() > 2U || !std::all_of(digits.begin(), digits.end(), [](const char c) { return c >= '0' && c <= '9'; })) {
    return 0;
  }
  const int value = digits.size() == 1U ? digits[0] - '0' : (digits[0] - '0') * 10 + (digits[1] - '0');
  return value >= 1 && value <= 16 ? value : 0;
}

// A calibration candidate for `argument`: for an integer flag {cores}, {threads}, {cores/N}, {cores*N/M} (counts of this
// PC's cores) or a number within its bounds; for a fraction flag a share from 0 to 1 with at most three decimal places, as
// the sweep writes one (0, 1, 0.25, 1.0).
bool candidate(const std::string_view text, const PluginArgument& argument) {
  if (argument.value == PluginArgumentValue::fraction) {
    if (text == "0" || text == "1") return true;
    if (text.size() < 3U || text.size() > 5U || text[1] != '.' || (text[0] != '0' && text[0] != '1')) return false;
    const auto decimals = text.substr(2U);
    if (!std::all_of(decimals.begin(), decimals.end(), [](const char c) { return c >= '0' && c <= '9'; })) return false;
    return text[0] == '0' || std::all_of(decimals.begin(), decimals.end(), [](const char c) { return c == '0'; });
  }
  if (text == "{cores}" || text == "{threads}") return true;
  if (text.starts_with("{cores") && text.ends_with("}")) {
    const auto rest = text.substr(6U, text.size() - 7U);
    if (rest.starts_with("/")) return small_number(rest.substr(1U)) != 0;
    if (!rest.starts_with("*")) return false;
    const auto slash = rest.find('/');
    return slash != std::string_view::npos && small_number(rest.substr(1U, slash - 1U)) != 0 && small_number(rest.substr(slash + 1U)) != 0;
  }
  if (text.empty() || text.size() > 16U || !std::all_of(text.begin(), text.end(), [](const char c) { return c >= '0' && c <= '9'; })) {
    return false;
  }
  std::int64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size() && value >= argument.minimum && value <= argument.maximum;
}

// One "arguments" entry; why not, naming its flag.
std::variant<PluginArgument, std::string> parse_engine_argument(const JsonValue& item, const std::vector<PluginArgument>& before) {
  if (auto problem = keys_problem(&item, "engine.arguments' entry", {"flag", "value", "min", "max", "choices"}, {"flag", "value"})) {
    return *problem;
  }
  const std::string* flag = member(item, "flag")->if_string();
  if (flag == nullptr || !flag_name(*flag)) {
    return std::string{"engine.arguments: a flag must be --name (lowercase letters, digits and '-', at most 33 characters)"};
  }
  if (std::find(kComposedFlags.begin(), kComposedFlags.end(), *flag) != kComposedFlags.end()) {
    return "engine.arguments: " + *flag + " is the server's own (it composes --native, --pack, --family, --family-sha256, --gpu and --serve)";
  }
  if (std::any_of(before.begin(), before.end(), [&flag](const PluginArgument& other) { return other.flag == *flag; })) {
    return "engine.arguments names " + *flag + " twice";
  }
  PluginArgument argument;
  argument.flag = *flag;
  const auto value = named(kArgumentValues, member(item, "value"));
  if (!value) return "engine.arguments: " + *flag + "'s value must be none, integer, fraction, gibibytes, on_off, gguf_file or choice";
  argument.value = *value;
  const JsonValue* low = member(item, "min");
  const JsonValue* high = member(item, "max");
  if (*value == PluginArgumentValue::integer || *value == PluginArgumentValue::gibibytes) {
    const bool gib = *value == PluginArgumentValue::gibibytes;
    const auto* lowest = low == nullptr ? nullptr : low->if_integer();
    const auto* highest = high == nullptr ? nullptr : high->if_integer();
    if (lowest == nullptr || highest == nullptr || *lowest > *highest || *lowest < -kMaxEngineInteger || *highest > kMaxEngineInteger ||
        (gib && (*lowest < 0 || *highest > 1024))) {
      return "engine.arguments: " + *flag + " needs min and max, whole numbers, min at most max" + (gib ? std::string{" (0 to 1024 GiB)"} : std::string{});
    }
    argument.minimum = *lowest;
    argument.maximum = *highest;
  } else if (low != nullptr || high != nullptr) {
    return "engine.arguments: " + *flag + " takes no min or max";
  }
  const JsonValue* choices = member(item, "choices");
  if (*value == PluginArgumentValue::choice) {
    const auto* list = choices == nullptr ? nullptr : choices->if_array();
    const std::string bad = "engine.arguments: " + *flag +
                            " needs its choices, 1 to 16, each once (lowercase letters, digits, '.', '_' or '-', at most 32)";
    if (list == nullptr || list->empty() || list->size() > kMaxEngineChoices) return bad;
    for (const auto& each : *list) {
      const std::string* text = each.if_string();
      if (text == nullptr || !word(*text, 32U, "._-") ||
          std::find(argument.choices.begin(), argument.choices.end(), *text) != argument.choices.end()) {
        return bad;
      }
      argument.choices.push_back(*text);
    }
  } else if (choices != nullptr) {
    return "engine.arguments: " + *flag + " takes no choices";
  }
  return argument;
}

// One "options" entry, on a declared flag; why not, naming the option.
std::variant<PluginEngineOption, std::string> parse_engine_option(const JsonValue& item, const PluginEngine& engine) {
  if (auto problem = keys_problem(&item, "engine.options' entry",
                                  {"name", "kind", "label", "help", "flag", "min", "max", "values", "choices", "default"},
                                  {"name", "kind", "label"})) {
    return *problem;
  }
  const std::string* name = member(item, "name")->if_string();
  if (name == nullptr || !valid_plugin_id(*name)) {
    return std::string{"engine.options: a name must be a lowercase letter, then lowercase letters, digits or '_' (at most 32)"};
  }
  if (std::any_of(engine.options.begin(), engine.options.end(), [&name](const PluginEngineOption& other) { return other.name == *name; })) {
    return "engine.options names " + *name + " twice";
  }
  // (a plugin family's cards have the server's options after its own: plugin_family.hpp)
  if (server_card_option(*name) != nullptr) return "engine.options: " + *name + " is one of the server's own options (every card has it)";
  const std::string where = "engine.options: " + *name;
  PluginEngineOption option;
  option.name = *name;
  const auto kind = named(kOptionKinds, member(item, "kind"));
  if (!kind) return where + "'s kind must be boolean, integer, decimal, choice, integer_or_auto or devices";
  option.kind = *kind;
  auto label = text_of(item, "label", 1U, 64U);
  if (label.index() == 1U) return where + "'s " + std::get<1>(label);
  option.label = std::get<0>(std::move(label));
  if (member(item, "help") != nullptr) {
    auto help = text_of(item, "help", 0U, 200U);
    if (help.index() == 1U) return where + "'s " + std::get<1>(help);
    option.help = std::get<0>(std::move(help));
  }
  const JsonValue* flag = member(item, "flag");
  const JsonValue* low = member(item, "min");
  const JsonValue* high = member(item, "max");
  const JsonValue* values = member(item, "values");
  const JsonValue* choices = member(item, "choices");
  const JsonValue* fallback = member(item, "default");
  if (*kind == ModelOptionKind::devices) {
    if (flag != nullptr) return where + " takes no flag: the server composes --gpu from it";
    const auto* most = high == nullptr ? nullptr : high->if_integer();
    if (most == nullptr || *most < 1 || *most > 64) return where + "'s max is how many GPUs it takes, 1 to 64";
    if (low != nullptr || values != nullptr || choices != nullptr) return where + " takes only a label, help, max and default";
    if (fallback != nullptr) {
      if (fallback->if_string() == nullptr || *fallback->if_string() != "auto") return where + "'s default can only be auto";
      option.default_value = "auto";
    }
    option.maximum = *most;
    return option;
  }
  const std::string* flag_text = flag == nullptr ? nullptr : flag->if_string();
  if (flag_text == nullptr) return where + " needs the flag it sets";
  const auto argument = std::find_if(engine.arguments.begin(), engine.arguments.end(),
                                     [&flag_text](const PluginArgument& each) { return each.flag == *flag_text; });
  if (argument == engine.arguments.end()) return where + "'s flag " + *flag_text + " is not among its arguments";
  option.flag = *flag_text;
  const bool integer = *kind == ModelOptionKind::integer || *kind == ModelOptionKind::integer_or_auto;
  const bool fits = (*kind == ModelOptionKind::boolean && (argument->value == PluginArgumentValue::on_off || argument->value == PluginArgumentValue::none)) ||
                    (integer && (argument->value == PluginArgumentValue::integer || argument->value == PluginArgumentValue::gibibytes)) ||
                    (*kind == ModelOptionKind::decimal && argument->value == PluginArgumentValue::fraction) ||
                    (*kind == ModelOptionKind::choice && argument->value == PluginArgumentValue::choice);
  if (!fits) {
    return where + " (" + name_in(kOptionKinds, *kind) + ") cannot set " + *flag_text + " (" + name_in(kArgumentValues, argument->value) + ")";
  }
  if (integer) {
    const auto* lowest = low == nullptr ? nullptr : low->if_integer();
    const auto* highest = high == nullptr ? nullptr : high->if_integer();
    if (lowest == nullptr || highest == nullptr || *lowest > *highest || *lowest < argument->minimum || *highest > argument->maximum) {
      return where + "'s min and max must be whole numbers within " + *flag_text + "'s, " + std::to_string(argument->minimum) + " to " +
             std::to_string(argument->maximum);
    }
    option.minimum = *lowest;
    option.maximum = *highest;
    if (values != nullptr) {
      const auto* list = values->if_array();
      const std::string bad = where + "'s values must rise, within its min and max (1 to 32 of them)";
      if (list == nullptr || list->empty() || list->size() > kMaxOptionValues) return bad;
      for (const auto& each : *list) {
        const auto* number = each.if_integer();
        if (number == nullptr || *number < option.minimum || *number > option.maximum || (!option.values.empty() && *number <= option.values.back())) {
          return bad;
        }
        option.values.push_back(*number);
      }
    }
    if (choices != nullptr) return where + " takes no choices";
  } else if (*kind == ModelOptionKind::decimal) {
    const auto lowest = number_of(low);
    const auto highest = number_of(high);
    if (!lowest || !highest || *lowest < 0.0 || *lowest > *highest || *highest > 1.0) return where + "'s min and max must be within 0 to 1";
    option.minimum_decimal = *lowest;
    option.maximum_decimal = *highest;
    if (values != nullptr || choices != nullptr) return where + " takes no values or choices";
  } else if (*kind == ModelOptionKind::choice) {
    const auto* list = choices == nullptr ? nullptr : choices->if_array();
    const std::string bad = where + "'s choices must be among " + *flag_text + "'s, each once";
    if (list == nullptr || list->empty()) return bad;
    for (const auto& each : *list) {
      const std::string* text = each.if_string();
      if (text == nullptr || std::find(argument->choices.begin(), argument->choices.end(), *text) == argument->choices.end() ||
          std::find(option.choices.begin(), option.choices.end(), *text) != option.choices.end()) {
        return bad;
      }
      option.choices.push_back(*text);
    }
    if (low != nullptr || high != nullptr || values != nullptr) return where + " takes no min, max or values";
  } else if (low != nullptr || high != nullptr || values != nullptr || choices != nullptr) {
    return where + " takes no min, max, values or choices";
  }
  if (fallback != nullptr) {
    const std::string bad = where + "'s default is not one it takes";
    const auto in_values = [&option](const std::int64_t number) {
      return option.values.empty() || std::find(option.values.begin(), option.values.end(), number) != option.values.end();
    };
    if (*kind == ModelOptionKind::boolean) {
      if (fallback->if_boolean() == nullptr) return bad;
      option.default_value = *fallback->if_boolean() ? "true" : "false";
    } else if (integer) {
      if (*kind == ModelOptionKind::integer_or_auto && fallback->if_string() != nullptr && *fallback->if_string() == "auto") {
        option.default_value = "auto";
      } else {
        const auto* number = fallback->if_integer();
        if (number == nullptr || *number < option.minimum || *number > option.maximum || !in_values(*number)) return bad;
        option.default_value = std::to_string(*number);
      }
    } else if (*kind == ModelOptionKind::decimal) {
      const auto number = number_of(fallback);
      if (!number || *number < option.minimum_decimal || *number > option.maximum_decimal) return bad;
      option.default_value = decimal_text(*number);
    } else {
      const std::string* text = fallback->if_string();
      if (text == nullptr || std::find(option.choices.begin(), option.choices.end(), *text) == option.choices.end()) return bad;
      option.default_value = *text;
    }
  }
  return option;
}

std::variant<PluginEngine, std::string> parse_engine(const JsonValue& block, const std::string& id, const std::vector<std::string>& files) {
  if (auto problem = keys_problem(&block, "engine",
                                  {"abi", "dialect", "chat", "tokenizer", "models", "arguments", "options", "sampling", "calibration",
                                   "monitor", "capabilities"},
                                  {"abi", "dialect", "chat", "tokenizer", "models", "arguments", "capabilities"})) {
    return *problem;
  }
  PluginEngine engine;
  // its library: the file the launcher loads
  const std::string library = engine_library_file(id);
  if (std::find(files.begin(), files.end(), library) == files.end()) {
    return "files must name its engine library, " + library + " (the file the engine's launcher loads)";
  }
  const auto* abi = member(block, "abi")->if_integer();
  if (abi == nullptr || *abi != 1) return std::string{"engine.abi must be 1, this server's engine ABI"};
  const auto dialect = named(kDialects, member(block, "dialect"));
  if (!dialect) return std::string{"engine.dialect must be strata_0143 or unified (a vendor's own engine dialect is not a plugin's)"};
  engine.dialect = *dialect;
  const auto chat = named(kRenderers, member(block, "chat"));
  if (!chat) return std::string{"engine.chat must name a reviewed renderer: qwen, glm53, gemma4, deepseek, raon1, flint or minimax"};
  engine.chat = *chat;

  const JsonValue* tokenizer = member(block, "tokenizer");
  if (auto problem = keys_problem(tokenizer, "engine.tokenizer", {"source", "mode"}, {"source", "mode"})) return *problem;
  const std::string* source = member(*tokenizer, "source")->if_string();
  if (source == nullptr || (*source != "gguf" && *source != "pack")) return std::string{"engine.tokenizer.source must be gguf or pack"};
  engine.tokenizer_source = *source == "gguf" ? TokenizerSource::gguf : TokenizerSource::pack;
  const auto mode = named(kTokenizerModes, member(*tokenizer, "mode"));
  if (!mode) {
    return std::string{"engine.tokenizer.mode must be one this server implements: byte_bpe, spm_bpe, raon_bpe, flint_bpe, deepseek_bpe, "
                       "glm_bpe or minimax_bpe"};
  }
  engine.tokenizer_mode = *mode;

  const JsonValue* models = member(block, "models");
  if (auto problem = keys_problem(models, "engine.models", {"gguf_architectures", "context_key", "mtp_key"}, {"gguf_architectures", "context_key"})) {
    return *problem;
  }
  const auto* architectures = member(*models, "gguf_architectures")->if_array();
  const std::string bad_architectures =
      "engine.models.gguf_architectures must list 1 to 8 GGUF architectures, each once (lowercase letters, digits, '.', '_' or '-', at most 32)";
  if (architectures == nullptr || architectures->empty() || architectures->size() > 8U) return bad_architectures;
  for (const auto& item : *architectures) {
    const std::string* text = item.if_string();
    if (text == nullptr || !word(*text, 32U, "._-") ||
        std::find(engine.gguf_architectures.begin(), engine.gguf_architectures.end(), *text) != engine.gguf_architectures.end()) {
      return bad_architectures;
    }
    engine.gguf_architectures.push_back(*text);
  }
  for (const auto& [key, field] : {std::pair<std::string_view, std::string*>{"context_key", &engine.context_key},
                                   std::pair<std::string_view, std::string*>{"mtp_key", &engine.mtp_key}}) {
    const JsonValue* value = member(*models, key);
    if (value == nullptr) continue;
    const std::string* text = value->if_string();
    if (text == nullptr || !word(*text, 64U, "._-")) {
      return "engine.models." + std::string{key} + " must be a GGUF key (lowercase letters, digits, '.', '_' or '-', at most 64)";
    }
    *field = *text;
  }

  const auto* arguments = member(block, "arguments")->if_array();
  if (arguments == nullptr || arguments->empty() || arguments->size() > kMaxEngineArguments) {
    return std::string{"engine.arguments must list 1 to 32 flags: the only ones that reach its engine"};
  }
  for (const auto& item : *arguments) {
    auto argument = parse_engine_argument(item, engine.arguments);
    if (auto* problem = std::get_if<std::string>(&argument)) return *problem;
    engine.arguments.push_back(std::get<PluginArgument>(std::move(argument)));
  }

  if (const JsonValue* options = member(block, "options")) {
    const auto* list = options->if_array();
    if (list == nullptr || list->size() > kMaxEngineOptions) return std::string{"engine.options must list at most 24 options"};
    for (const auto& item : *list) {
      auto option = parse_engine_option(item, engine);
      if (auto* problem = std::get_if<std::string>(&option)) return *problem;
      engine.options.push_back(std::get<PluginEngineOption>(std::move(option)));
    }
  }

  if (const JsonValue* sampling = member(block, "sampling")) {
    if (auto problem = keys_problem(sampling, "engine.sampling", {"temperature", "top_p", "top_k", "min_p"}, {"temperature", "top_p"})) {
      return *problem;
    }
    const auto temperature = number_of(member(*sampling, "temperature"));
    if (!temperature || *temperature < 0.0 || *temperature > 2.0) return std::string{"engine.sampling.temperature must be 0 to 2"};
    const auto top_p = number_of(member(*sampling, "top_p"));
    if (!top_p || *top_p <= 0.0 || *top_p > 1.0) return std::string{"engine.sampling.top_p must be above 0, at most 1"};
    engine.sampling.temperature = *temperature;
    engine.sampling.top_p = *top_p;
    if (const JsonValue* top_k = member(*sampling, "top_k")) {
      const auto* number = top_k->if_integer();
      if (number == nullptr || *number < 1 || *number > 1000) return std::string{"engine.sampling.top_k must be a whole number from 1 to 1000"};
      engine.sampling.top_k = *number;
    }
    if (const JsonValue* min_p = member(*sampling, "min_p")) {
      const auto number = number_of(min_p);
      if (!number || *number < 0.0 || *number > 1.0) return std::string{"engine.sampling.min_p must be 0 to 1"};
      engine.sampling.min_p = *number;
    }
  }

  if (const JsonValue* calibration = member(block, "calibration")) {
    if (auto problem = keys_problem(calibration, "engine.calibration", {"before_first_start", "keep_margin", "steps"}, {"steps"})) {
      return *problem;
    }
    if (const JsonValue* before = member(*calibration, "before_first_start")) {
      if (before->if_boolean() == nullptr) return std::string{"engine.calibration.before_first_start must be true or false"};
      engine.calibrates_before_first_start = *before->if_boolean();
    }
    if (const JsonValue* margin = member(*calibration, "keep_margin")) {
      const auto number = number_of(margin);
      if (!number || *number < 0.0 || *number > 0.2) return std::string{"engine.calibration.keep_margin must be 0 to 0.2"};
      engine.keep_margin = *number;
    }
    const auto* steps = member(*calibration, "steps")->if_array();
    if (steps == nullptr || steps->size() > kMaxCalibrationSteps) return std::string{"engine.calibration.steps must list at most 4 steps"};
    for (const auto& item : *steps) {
      if (auto problem = keys_problem(&item, "engine.calibration's step", {"flag", "values"}, {"flag", "values"})) return *problem;
      const std::string* flag = member(item, "flag")->if_string();
      const auto argument = flag == nullptr ? engine.arguments.end()
                                            : std::find_if(engine.arguments.begin(), engine.arguments.end(),
                                                           [&flag](const PluginArgument& each) { return each.flag == *flag; });
      if (argument == engine.arguments.end() ||
          (argument->value != PluginArgumentValue::integer && argument->value != PluginArgumentValue::fraction)) {
        return std::string{"engine.calibration: a step's flag must be one of its integer or fraction flags"};
      }
      if (std::any_of(engine.calibration.begin(), engine.calibration.end(), [&flag](const PluginCalibrationStep& other) { return other.flag == *flag; })) {
        return "engine.calibration names " + *flag + " twice";
      }
      PluginCalibrationStep step;
      step.flag = *flag;
      const auto* values = member(item, "values")->if_array();
      const std::string bad = "engine.calibration: " + *flag +
                              (argument->value == PluginArgumentValue::fraction
                                   ? "'s candidates must be numbers from 0 to 1, at most three decimal places (1 to 8 of them)"
                                   : "'s candidates must be numbers within its bounds, {cores}, {threads}, {cores/N} or {cores*N/M} (1 to 8 of them)");
      if (values == nullptr || values->empty() || values->size() > kMaxCandidates) return bad;
      for (const auto& each : *values) {
        const std::string* text = each.if_string();
        if (text == nullptr || !candidate(*text, *argument)) return bad;
        step.values.push_back(*text);
      }
      engine.calibration.push_back(std::move(step));
    }
  }

  if (const JsonValue* monitor = member(block, "monitor")) {
    if (auto problem = keys_problem(monitor, "engine.monitor", {"experts_vram", "experts_ram"}, {})) return *problem;
    for (const auto& [key, field] : {std::pair<std::string_view, std::string*>{"experts_vram", &engine.monitor_experts_vram},
                                     std::pair<std::string_view, std::string*>{"experts_ram", &engine.monitor_experts_ram}}) {
      const JsonValue* value = member(*monitor, key);
      if (value == nullptr) continue;
      // its experts in VRAM counted; its RAM tier counted or sized (the Monitor's view: engine_memory_view)
      const std::string* text = value->if_string();
      const auto among = [text](const auto& keys) { return std::find(keys.begin(), keys.end(), *text) != keys.end(); };
      if (key == "experts_vram" && (text == nullptr || !among(kExpertCountKeys))) {
        return std::string{"engine.monitor.experts_vram must be one of the engine keys that count experts"};
      }
      if (text == nullptr || (!among(kExpertCountKeys) && !among(kExpertRamSizeKeys))) {
        return std::string{"engine.monitor.experts_ram must be one of the engine keys that count experts or give their RAM"};
      }
      *field = *text;
    }
  }

  const JsonValue* capabilities = member(block, "capabilities");
  if (auto problem = keys_problem(capabilities, "engine.capabilities", {"vision", "web_search", "mtp"}, {"vision", "web_search", "mtp"})) {
    return *problem;
  }
  for (const auto& [key, field] : {std::pair<std::string_view, bool*>{"vision", &engine.vision},
                                   std::pair<std::string_view, bool*>{"web_search", &engine.web_search}}) {
    const auto* flag = member(*capabilities, key)->if_boolean();
    if (flag == nullptr) return "engine.capabilities." + std::string{key} + " must be true or false";
    *field = *flag;
  }
  const auto mtp = named(kMtpKinds, member(*capabilities, "mtp"));
  if (!mtp) return std::string{"engine.capabilities.mtp must be none, file or in_model"};
  engine.mtp = *mtp;
  return engine;
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

std::string engine_library_file(const std::string_view id) {
  return kPlatform == "windows" ? "multiengine_" + std::string{id} + ".dll" : "libmultiengine_" + std::string{id} + ".so";
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
  if (manifest.kind == PluginKind::engine) {
    auto engine = parse_engine(*member(*root, "engine"), manifest.id, manifest.files);
    if (const auto* problem = std::get_if<std::string>(&engine)) return *problem;
    manifest.engine = std::get<PluginEngine>(std::move(engine));
  }
  return manifest;
}

}  // namespace secure_frontend
