#pragma once

// An install's plugins as its server finds them when it starts (the owner, 2026-10-30; plugin_registry.hpp): the
// folder <install>/plugins, beside the engine's folder; the first-party plugins make recorded in the engine folder's
// build.json; the owner's choices in <roots.state>/plugins.json. The server decides from this what runs and what the
// Debugging plugin turns on; strata-window and strata-tui ask before they start a server while it is on.

#include "secure_frontend/plugin_registry.hpp"

#include <filesystem>
#include <optional>
#include <vector>

namespace secure_frontend {

struct InstallPlugins final {
  std::filesystem::path folder;               // <install>/plugins
  std::vector<FirstPartyPlugin> first_party;  // the engine folder's build.json's (none without one)
  std::vector<PluginEntry> entries;           // each with its state; none when plugins.json cannot be read
  bool choices_read{true};                    // false: plugins.json is there but is not one this server reads
};

// The plugins beside `engine_folder` (<install>/engine), with the owner's choices in `state_dir` (nullopt: none known,
// so make's record alone decides a first-party plugin). An unreadable choices file lets nothing run.
[[nodiscard]] InstallPlugins install_plugins(const std::filesystem::path& engine_folder,
                                             const std::optional<std::filesystem::path>& state_dir);

}  // namespace secure_frontend
