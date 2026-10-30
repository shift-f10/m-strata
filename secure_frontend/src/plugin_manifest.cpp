#include "secure_frontend/plugin_manifest.hpp"

#include "secure_frontend/json.hpp"

#include <algorithm>
#include <array>
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

}  // namespace

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
  return manifest;
}

}  // namespace secure_frontend
