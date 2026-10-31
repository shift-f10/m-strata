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

#include "secure_frontend/engine_family_kinds.hpp"
#include "secure_frontend/family_descriptor.hpp"
#include "secure_frontend/model_option_spec.hpp"

#include <chrono>
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
  std::chrono::seconds call_timeout{120};                           // a tool's: one call's limit (1-600 s)
};

// An engine plugin's block (plugin spec section 3, as built): the family it adds, what its descriptor
// (family_descriptor.hpp) will hold, each part from a closed list this server implements. Its library is the file the
// launcher loads, multiengine_<id>.dll (libmultiengine_<id>.so on Linux), which its files must name.
//   "engine": {"abi": 1, "dialect": "strata_0143" | "unified", "chat": a reviewed renderer (qwen, glm53, gemma4, deepseek,
//              raon1, flint, minimax), "tokenizer": {"source": "gguf" | "pack", "mode": a tokenizer mode},
//              "models": {"gguf_architectures": [...], "context_key", "mtp_key"?},
//              "arguments": [{"flag", "value": none | integer | fraction | gibibytes | on_off | gguf_file | choice, "min",
//                             "max", "choices"}],                               (the only flags that reach its engine)
//              "options": [{"name", "kind", "label", "help", "flag", "min", "max", "values", "choices", "default"}]?,
//              "sampling": {"temperature", "top_p", "top_k"?, "min_p"?}?,
//              "calibration": {"before_first_start", "keep_margin", "steps": [{"flag", "values"}]}?,
//              "monitor": {"experts_vram", "experts_ram"}?, "capabilities": {"vision", "web_search", "mtp"}}
// The server composes --native/--pack, --family, --family-sha256, --gpu and --serve itself: no plugin declares them.
enum class PluginArgumentValue { none, integer, fraction, gibibytes, on_off, gguf_file, choice };
struct PluginArgument final {
  std::string flag;
  PluginArgumentValue value{PluginArgumentValue::none};
  std::int64_t minimum{0};  // integer and gibibytes
  std::int64_t maximum{0};
  std::vector<std::string> choices{};  // choice
};
// A card's option on a declared flag, as the built-in tables declare theirs (model_option_spec.hpp); devices has no flag
// (the server composes --gpu from it).
struct PluginEngineOption final {
  std::string name;
  ModelOptionKind kind{ModelOptionKind::boolean};
  std::string label;
  std::string help;
  std::string flag;
  std::int64_t minimum{0};  // integer, integer_or_auto; devices: at most `maximum` GPUs
  std::int64_t maximum{0};
  double minimum_decimal{0.0};  // decimal
  double maximum_decimal{0.0};
  std::vector<std::string> choices{};    // choice
  std::vector<std::int64_t> values{};    // integer kinds: the only values offered, ascending
  std::optional<std::string> default_value{};  // as a card holds it: "true", "65536", "0.5", "auto", "q8"
};
// One calibration step: the candidates for a declared integer or fraction flag - numbers (a fraction's at most three
// decimal places), or for an integer flag {cores}, {threads}, {cores/N} and {cores*N/M} (this PC's physical cores and
// threads).
struct PluginCalibrationStep final {
  std::string flag;
  std::vector<std::string> values;
};
struct PluginEngine final {
  int abi{1};
  EngineProtocolFlavor dialect{EngineProtocolFlavor::strata_0143};
  NativeChatProfile chat{NativeChatProfile::qwen};
  TokenizerSource tokenizer_source{TokenizerSource::gguf};
  TokenizerMode tokenizer_mode{TokenizerMode::byte_bpe};
  std::vector<std::string> gguf_architectures;
  std::string context_key;
  std::string mtp_key{};  // empty: none
  std::vector<PluginArgument> arguments;
  std::vector<PluginEngineOption> options{};
  RecommendedSampling sampling{};
  bool calibrates_before_first_start{false};
  double keep_margin{0.03};
  std::vector<PluginCalibrationStep> calibration{};
  std::string monitor_experts_vram{};  // the engine's STAT/INFO key counting its experts in VRAM (empty: none)
  std::string monitor_experts_ram{};   // counting them in RAM, or giving that tier's MiB or GiB (kExpertRamSizeKeys)
  bool vision{false};
  bool web_search{false};
  DraftLayerKind mtp{DraftLayerKind::none};
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
  std::optional<PluginEngine> engine{};     // kind engine
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
