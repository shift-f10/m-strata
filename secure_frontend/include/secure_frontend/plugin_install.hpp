#pragma once

// Adding a plugin from the page, and removing one (docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md,
// section 12; the owner, 2026-10-30: "enable the ability to add and remove plugins via the webui", "it should check the
// trust before processing it", removing should "move it to the side but once moved to the side, they can also be
// permanently removed"). Everything happens in the install's plugins folder, so every move is a rename on one volume:
// a plugin is staged in <plugins>/.staging/<id> and renamed to <plugins>/<id>; one removed is renamed to
// <plugins>/.removed/<id>-<UTC time>, from where it is put back or deleted. A name starting with '.' is never a plugin
// id, and neither folder holds a plugin.json of its own, so discovery never takes one for a plugin.
//
// Adding takes only a plugin signed by a key this server trusts (plugin_signature.hpp), for its id and this platform:
// its manifest checked as discovery checks one, its signature read before any file is, then each file the manifest
// lists for this platform hashed where it is (a folder) or inflated (a zip, zip_reader.hpp) and compared with the
// signed statement - and only then copied into staging, hashed again as written. Every refusal is a sentence for the
// page, said before anything is written. The plugin is discovered at the next start, trusted through its signature.

#include "secure_frontend/plugin_registry.hpp"
#include "secure_frontend/plugin_signature.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

struct AddedPlugin final {
  std::string id;
  std::string name;
  std::string version;
  std::string publisher;
  std::string signer;  // the trusted key's name
  std::string key;     // the key that signed it (64 hex)
};

// From a folder on this PC: an absolute path, not a link, holding plugin.json, plugin.sig and the files the manifest
// lists for this platform (each a plain file, at most 1 GiB in all). Else why not.
[[nodiscard]] std::variant<AddedPlugin, std::string> add_plugin_from_folder(const std::filesystem::path& plugins_dir,
                                                                            const std::filesystem::path& folder,
                                                                            const std::vector<TrustedKey>& keys);
// From a .zip held in memory: its entries at the root or in one top folder - plugin.json, plugin.sig and exactly the
// files the manifest lists for this platform (folders aside). Else why not.
[[nodiscard]] std::variant<AddedPlugin, std::string> add_plugin_from_zip(const std::filesystem::path& plugins_dir,
                                                                         std::span<const std::uint8_t> zip,
                                                                         const std::vector<TrustedKey>& keys);

inline constexpr std::string_view kStagingFolder = ".staging";
inline constexpr std::string_view kRemovedFolder = ".removed";

// A plugin moved aside: its folder <plugins>/.removed/<entry>, <entry> = <id>-<YYYYMMDD>T<HHMMSS>Z (then -2, -3...
// when one was moved aside under that id in the same second).
struct RemovedPlugin final {
  std::string entry;
  std::string id;
  std::string name;     // its plugin.json's (empty when that cannot be read)
  std::string version;  // likewise
  std::string when;     // when it was moved aside: YYYY-MM-DDTHH:MM:SSZ
};

// Moves <plugins>/<id> aside, named for `now`. A built-in plugin (one of make's, in `first_party`) is refused: it is
// only turned off. Else why not.
[[nodiscard]] std::variant<RemovedPlugin, std::string> move_plugin_aside(const std::filesystem::path& plugins_dir,
                                                                         std::string_view id,
                                                                         const std::vector<FirstPartyPlugin>& first_party,
                                                                         std::chrono::system_clock::time_point now);
// What was moved aside, oldest first (by entry name: by id, then time).
[[nodiscard]] std::vector<RemovedPlugin> removed_plugins(const std::filesystem::path& plugins_dir);
// Moves `entry` back to <plugins>/<id>; refused while a plugin of that id is installed. Else why not.
[[nodiscard]] std::variant<RemovedPlugin, std::string> restore_plugin(const std::filesystem::path& plugins_dir,
                                                                      std::string_view entry);
// Deletes `entry` for good. nullopt: deleted; else why not.
[[nodiscard]] std::optional<std::string> purge_plugin(const std::filesystem::path& plugins_dir, std::string_view entry);

// What the owner chose for plugin `id` - its approval and on/off - forgotten, so one removed starts over. The publisher
// keys stay.
void forget_plugin_choices(PluginChoices& choices, std::string_view id);

}  // namespace secure_frontend
