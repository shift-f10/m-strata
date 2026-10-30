#pragma once

// Which plugins this install has, and which may run (docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md,
// sections 6 and 8; the owner's decisions 2 and 4, 2026-10-30: approve each plugin, its SHA-256 pinned; plugins `make`
// built are trusted through the build record). Every folder of <install>/plugins that holds a plugin.json is read and
// checked (plugin_manifest.hpp), its manifest and this platform's files hashed, and given a state:
//   refused  its manifest or its folder is not one this server takes (other users of this PC can change the folder, a
//            file it names is missing), or this server does not run its kind yet - the reason says which
//   on, off  trusted: a first-party plugin whose SHA-256s are its build record's, or one the owner approved whose
//            SHA-256s are still the approved ones; on unless turned off (a first-party one starts as its record says)
//   changed  approved once, but its manifest or a file changed since: it waits to be approved again
//   waiting  never approved
// The owner's approvals and on/off live in <roots.state>/plugins.json (PluginChoices). What runs is decided when the
// server starts.

#include "secure_frontend/plugin_manifest.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

enum class PluginState { on, off, waiting, changed, refused };
[[nodiscard]] std::string_view plugin_state_name(PluginState state) noexcept;

// What the trust pins: the manifest's SHA-256 and each of this platform's files' (lowercase hex, by relative path).
struct PluginPin final {
  std::string manifest;
  std::map<std::string, std::string> files;
  bool operator==(const PluginPin&) const = default;
};

// The owner's choices: the plugins approved (with what was approved) and each one's on/off.
struct PluginChoices final {
  std::map<std::string, PluginPin> approved;
  std::map<std::string, bool> enabled;
};
[[nodiscard]] std::string render_plugin_choices(const PluginChoices& choices);
// Strict, bounded: nullopt for anything that is not such a file.
[[nodiscard]] std::optional<PluginChoices> parse_plugin_choices(std::string_view text);

// A first-party plugin as `make` recorded it in engine/build.json: what it built, and whether it starts on.
struct FirstPartyPlugin final {
  std::string id;
  PluginPin pin;
  bool on{false};
};

struct PluginEntry final {
  std::string id;                          // its folder's name
  std::optional<PluginManifest> manifest;  // nullopt: refused before it could be read
  PluginPin pin;                           // its SHA-256s now
  bool first_party{false};
  PluginState state{PluginState::refused};
  std::string reason;                      // refused, changed or waiting: why, in words
};

// The kinds this server runs (more as their phases land).
[[nodiscard]] bool plugin_kind_runs(PluginKind kind) noexcept;

// Every plugin in `plugins_dir`, by id; an entry that is not a folder holding a plugin.json is not one.
[[nodiscard]] std::vector<PluginEntry> discover_plugins(const std::filesystem::path& plugins_dir,
                                                        const std::vector<FirstPartyPlugin>& first_party,
                                                        const PluginChoices& choices);

}  // namespace secure_frontend
