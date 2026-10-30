#pragma once

// A tool's or notifier's secret settings (docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md, section
// 10): each in the server's secret folder (roots.secret) as plugin-<id>-<name>, kept as the API key is - Windows DPAPI
// for this user, elsewhere 0600 - never in config.json, never shown again, and handed only to that plugin's process.

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

inline constexpr std::size_t kMaxPluginSecretBytes = 1024U;

// The secret's file name: plugin-<id>-<name> (an id and a name as a plugin's are; nullopt for anything else).
[[nodiscard]] std::optional<std::string> plugin_secret_file(std::string_view id, std::string_view name);
// Stored (nullopt), or why not: 1-1024 printable characters, an id and a name that are ones, the folder private.
[[nodiscard]] std::optional<std::string> store_plugin_secret(const std::filesystem::path& secret_dir, std::string_view id,
                                                             std::string_view name, std::string_view value);
// The secret, read; nullopt: none stored, or one that does not read back. The caller wipes it once it is handed on.
[[nodiscard]] std::optional<std::string> read_plugin_secret(const std::filesystem::path& secret_dir, std::string_view id,
                                                            std::string_view name);
[[nodiscard]] bool plugin_secret_set(const std::filesystem::path& secret_dir, std::string_view id, std::string_view name);
// Gone (true), or still there (false: it could not be removed).
[[nodiscard]] bool remove_plugin_secret(const std::filesystem::path& secret_dir, std::string_view id, std::string_view name);

}  // namespace secure_frontend
