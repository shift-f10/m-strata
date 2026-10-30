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
// }
//
// Each kind has its own block, named after it ("engine", "tool", "notifier", "diagnostics"), and only its own. Every
// other key is refused, as is any text that is not printable ASCII within its bound.

#include <cstddef>
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
};

inline constexpr std::size_t kMaxPluginManifestBytes = 256U * 1024U;
inline constexpr std::size_t kMaxPluginFiles = 16U;

// A plugin id: a lowercase letter, then up to 31 lowercase letters, digits or '_' (as the engine's launcher takes it).
[[nodiscard]] bool valid_plugin_id(std::string_view id) noexcept;

// The manifest of the folder named `folder_id`, checked; or why it is not one, in words (naming the key at fault).
// A code kind (engine, tool, notifier) needs this platform's files; a diagnostics plugin has none.
[[nodiscard]] std::variant<PluginManifest, std::string> parse_plugin_manifest(std::string_view text, std::string_view folder_id);

}  // namespace secure_frontend
