#include "secure_frontend/install_plugins.hpp"

#include "secure_frontend/about_info.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/plugin_secrets.hpp"

#include <algorithm>
#include <set>
#include <variant>

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

std::vector<PluginProgram> plugin_programs(
    const InstallPlugins& plugins, const PluginKind kind, const PluginSettingsConfig& config,
    const std::optional<std::filesystem::path>& secret_dir,
    const std::function<std::map<std::string, std::string, std::less<>>(std::string_view id)>& environment,
    std::vector<std::pair<std::string, std::string>>* left_out) {
  std::vector<PluginProgram> programs;
  for (const auto& entry : plugins.entries) {
    if (entry.state != PluginState::on || !entry.manifest || entry.manifest->kind != kind || !entry.manifest->run) continue;
    const PluginRun& run = *entry.manifest->run;
    // its values as the config holds them (a decimal is no setting's value), and which of its secrets are set
    std::map<std::string, PluginValue> set;
    if (const auto found = config.values.find(entry.id); found != config.values.end()) {
      for (const auto& [name, value] : found->second) {
        if (const auto* b = std::get_if<bool>(&value)) set[name] = *b;
        else if (const auto* i = std::get_if<std::int64_t>(&value)) set[name] = *i;
        else if (const auto* s = std::get_if<std::string>(&value)) set[name] = *s;
        else if (const auto* items = std::get_if<std::vector<std::string>>(&value)) set[name] = *items;
      }
    }
    std::set<std::string> secrets_set;
    for (const auto& setting : run.settings) {
      if (setting.kind == SettingKind::secret && secret_dir && plugin_secret_set(*secret_dir, entry.id, setting.name)) secrets_set.insert(setting.name);
    }
    auto checked = plugin_values(run, set, secrets_set);
    if (const auto* why = std::get_if<std::string>(&checked)) {
      if (left_out != nullptr) left_out->emplace_back(entry.id, *why);
      continue;
    }
    const auto& values = std::get<std::map<std::string, PluginValue>>(checked);
    PluginProgram program{.id = entry.id, .run = run, .program = plugins.folder / entry.id / path_from_utf8(run.program),
                          .arguments = {}, .environment = environment(entry.id), .values = values};
    // (a setting an argument names always has a value: the manifest reader made sure)
    for (const auto& argument : run.arguments) {
      program.arguments.push_back(argument.setting.empty() ? argument.text : value_text(values.at(argument.setting)));
    }
    for (const auto& [name, value] : run.environment) {
      if (value.setting.empty()) {
        program.environment[name] = value.text;
        continue;
      }
      const auto setting = std::find_if(run.settings.begin(), run.settings.end(), [&value](const PluginSetting& s) { return s.name == value.setting; });
      if (setting != run.settings.end() && setting->kind == SettingKind::secret) {
        if (auto secret = secret_dir ? read_plugin_secret(*secret_dir, entry.id, value.setting) : std::nullopt) {
          program.environment[name] = *secret;
          std::fill(secret->begin(), secret->end(), '\0');
        }
      } else if (const auto found = values.find(value.setting); found != values.end()) {
        program.environment[name] = value_text(found->second);
      }
    }
    programs.push_back(std::move(program));
  }
  return programs;
}

}  // namespace secure_frontend
