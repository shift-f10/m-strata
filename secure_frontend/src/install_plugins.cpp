#include "secure_frontend/install_plugins.hpp"

#include "secure_frontend/about_info.hpp"

namespace secure_frontend {

InstallPlugins install_plugins(const std::filesystem::path& engine_folder, const std::optional<std::filesystem::path>& state_dir) {
  InstallPlugins plugins{.folder = engine_folder.parent_path() / "plugins", .first_party = {}, .entries = {}, .choices_read = true};
  if (const auto build = read_engine_build(engine_folder)) {
    for (const auto& plugin : build->plugins) {
      plugins.first_party.push_back({.id = plugin.id, .pin = {.manifest = plugin.manifest, .files = plugin.files}, .on = plugin.on});
    }
  }
  const auto choices = state_dir ? read_plugin_choices(*state_dir) : std::optional<PluginChoices>{PluginChoices{}};
  plugins.choices_read = choices.has_value();
  if (choices) plugins.entries = discover_plugins(plugins.folder, plugins.first_party, *choices);
  return plugins;
}

}  // namespace secure_frontend
