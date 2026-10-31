#pragma once

// An install's plugins as its server finds them when it starts (the owner, 2026-10-30; plugin_registry.hpp): the
// folder <install>/plugins, beside the engine's folder; the first-party plugins make recorded in the engine folder's
// build.json; the owner's choices in <roots.state>/plugins.json. The server decides from this what runs and what the
// Debugging plugin turns on; strata-window and strata-tui ask before they start a server while it is on.

#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/plugin_family.hpp"
#include "secure_frontend/plugin_registry.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace secure_frontend {

struct InstallPlugins final {
  std::filesystem::path folder;               // <install>/plugins
  std::vector<FirstPartyPlugin> first_party;  // the engine folder's build.json's (none without one)
  std::string install_key{};                  // its signing_key: the install's own plugin signing key (empty: none)
  std::vector<PluginEntry> entries;           // each with its state; none when plugins.json cannot be read
  bool choices_read{true};                    // false: plugins.json is there but is not one this server reads
};

// The plugins beside `engine_folder` (<install>/engine), with the owner's choices in `state_dir` (nullopt: none known,
// so make's record alone decides a first-party plugin). An unreadable choices file lets nothing run.
[[nodiscard]] InstallPlugins install_plugins(const std::filesystem::path& engine_folder,
                                             const std::optional<std::filesystem::path>& state_dir);

// The engine plugins that are on, as the families the server runs (plugin spec section 5; set_plugin_families takes
// them): each one's manifest and its library's SHA-256 - engine_library_file's, as it was hashed and trusted - in the
// plugins' order. A plugin waiting for approval, turned off, changed since it was approved or refused makes none.
[[nodiscard]] std::vector<PluginFamilySource> engine_families(const InstallPlugins& plugins);

// A tool's or notifier's program as the server runs it (plugin spec section 10): its file in its folder, its arguments
// and environment filled in from its settings - each value as the config holds it, or its default - with a secret read
// from the secret folder (only ever into the environment), over the closed environment the server gives it.
struct PluginProgram final {
  std::string id;
  PluginRun run;                                                // its manifest's block (its note, its call's limit)
  std::filesystem::path program;
  std::vector<std::string> arguments;
  std::map<std::string, std::string, std::less<>> environment;
  std::map<std::string, PluginValue> values;                    // its settings' values, never a secret's (a notifier's events)
};
// The plugins of `kind` that are on, each as it runs. One whose settings do not fit its manifest (a required one not
// set, a value it refuses) is left out, its id and why in `left_out`. `environment` gives each its closed environment.
[[nodiscard]] std::vector<PluginProgram> plugin_programs(
    const InstallPlugins& plugins, PluginKind kind, const PluginSettingsConfig& config,
    const std::optional<std::filesystem::path>& secret_dir,
    const std::function<std::map<std::string, std::string, std::less<>>(std::string_view id)>& environment,
    std::vector<std::pair<std::string, std::string>>* left_out);

}  // namespace secure_frontend
