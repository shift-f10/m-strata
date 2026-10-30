#pragma once

// Plugins (the owner, 2026-10-30: "make our multiengine plugin capable ...", "we also need to have a plugin management
// tab to manage these", "we should make the email, websearch, debugging all plugins as well";
// docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md, section 8). A plugin is one folder of the install,
// <install>/plugins/<id>/, described by its plugin.json (schema m-strata.plugin/1). This reads and checks one manifest;
// plugin_registry.hpp finds them, hashes them and decides which may run. No plugin ever runs inside this server: an
// engine plugin runs in the engine's process, every other kind as a child of the server.
//
// {
//   "schema": "m-strata.plugin/1",
//   "id": "debugging", "kind": "diagnostics",
//   "name": "Debugging", "version": "1.0.0", "publisher": "M-Strata", "license": "AGPL-3.0-only", "about": "...",
//   "files": {"windows": ["multiengine_acme3.dll"], "linux": ["libmultiengine_acme3.so"]},   (a code kind's files)
//   "diagnostics": {"switches": ["logging.requests", "logging.engine_output", "troubleshooting"]}
//   "tool" / "notifier": {"program": {"windows": "x.exe", "linux": "x"}, "arguments": ["--url", "{url}"],
//                         "environment": {"API_KEY": "{key}"}, "network": "the instance at {url}", "note": "...",
//                         "settings": [{"name", "kind", "label", "help", "required", "default", "min", "max",
//                                       "choices", "max_items"}], "presets": [{"label", "values": {...}}]}
// }
//
// Each kind has its own block, named after it ("engine", "tool", "notifier", "diagnostics"), and only its own. Every
// other key is refused, as is any text that is not printable ASCII within its bound.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

enum class PluginKind { engine, tool, notifier, diagnostics };
[[nodiscard]] std::string_view plugin_kind_name(PluginKind kind) noexcept;

// What a diagnostics plugin turns on while it is on (what a `--debugging` build did): one log line per request, the
// engine's own output in the log, and Settings > Troubleshooting.
enum class DiagnosticSwitch { log_requests, log_engine_output, troubleshooting };
[[nodiscard]] std::string_view diagnostic_switch_name(DiagnosticSwitch value) noexcept;

// A setting a tool or notifier plugin declares (spec section 10, the owner's decision 6): its value lives in config.json
// as plugins.<id>.<name>, a secret's in roots.secret as plugin-<id>-<name> (plugin_secrets.hpp).
enum class SettingKind { text, url, secret, integer, boolean, choice, text_list };
[[nodiscard]] std::string_view setting_kind_name(SettingKind kind) noexcept;

// A setting's value as config.json holds it (a secret's never: it is only ever handed to the plugin's process).
using PluginValue = std::variant<bool, std::int64_t, std::string, std::vector<std::string>>;

struct PluginSetting final {
  std::string name;   // a lowercase letter, then lowercase letters, digits or '_' (at most 32)
  SettingKind kind{SettingKind::text};
  std::string label;
  std::string help;
  bool required{false};
  std::optional<PluginValue> default_value{};  // of its kind; never a secret's
  std::int64_t min{0};                         // an integer's bounds
  std::int64_t max{0};
  std::vector<std::string> choices{};          // a choice's; a text list's allowed items (empty: any text)
  std::size_t max_items{0};                    // a text list's most items (1-16)
};

// An argument or an environment value: a fixed text, or one setting's value.
struct PluginTemplate final {
  std::string text{};     // fixed
  std::string setting{};  // or the setting it names
  bool operator==(const PluginTemplate&) const = default;
};

// Values the Plugins tab fills in at once (the IONOS fill-in of email's settings).
struct PluginPreset final {
  std::string label;
  std::map<std::string, PluginValue> values;
};

// How a tool or notifier plugin's program runs, and its settings (spec section 10).
struct PluginRun final {
  std::string program;                                              // this platform's: one of its files
  std::vector<PluginTemplate> arguments;                            // never a secret's
  std::vector<std::pair<std::string, PluginTemplate>> environment;  // NAME -> value (a secret only here)
  std::string network;                                              // what it connects to, in words ({setting} allowed)
  std::string note;                                                 // a tool's system-message note ({today} allowed)
  std::vector<PluginSetting> settings;
  std::vector<PluginPreset> presets;
};

struct PluginManifest final {
  std::string id;          // its folder's name
  PluginKind kind{PluginKind::diagnostics};
  std::string name;
  std::string version;
  std::string publisher;
  std::string license;
  std::string about;
  std::vector<std::string> files;           // this platform's code files, relative to its folder ('/'-separated)
  std::vector<DiagnosticSwitch> switches;   // kind diagnostics
  std::optional<PluginRun> run;             // kinds tool and notifier
};

inline constexpr std::size_t kMaxPluginSettings = 16U;

// A value for `setting`, checked against its manifest: why not, in words naming the setting; nullopt: it fits.
[[nodiscard]] std::optional<std::string> setting_value_problem(const PluginSetting& setting, const PluginValue& value);
// The values a plugin runs with: each setting's own (a secret's never: `secrets_set` names the secrets stored), a
// missing one's default, all checked; or why not (a required one missing, a value its manifest refuses). A value for a
// setting the manifest does not declare is left out.
[[nodiscard]] std::variant<std::map<std::string, PluginValue>, std::string> plugin_values(
    const PluginRun& run, const std::map<std::string, PluginValue>& set, const std::set<std::string>& secrets_set);
// A value as an argument or an environment variable takes it: true/false, a number, the text, a list's items joined
// by ','.
[[nodiscard]] std::string value_text(const PluginValue& value);

inline constexpr std::size_t kMaxPluginManifestBytes = 256U * 1024U;
inline constexpr std::size_t kMaxPluginFiles = 16U;

// A plugin id: a lowercase letter, then up to 31 lowercase letters, digits or '_' (as the engine's launcher takes it).
[[nodiscard]] bool valid_plugin_id(std::string_view id) noexcept;

// The manifest of the folder named `folder_id`, checked; or why it is not one, in words (naming the key at fault).
// A code kind (engine, tool, notifier) needs this platform's files; a diagnostics plugin has none.
[[nodiscard]] std::variant<PluginManifest, std::string> parse_plugin_manifest(std::string_view text, std::string_view folder_id);

}  // namespace secure_frontend
