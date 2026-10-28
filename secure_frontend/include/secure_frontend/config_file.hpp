#pragma once

#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/json.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace secure_frontend {

inline constexpr std::size_t kMaxConfigFileBytes = 64U * 1024U;

// The dotted path of the first config member whose SP5 parity row is
// `rejected` (document order, any depth), or nullopt. A path the schema knows
// as a key or a section never matches: typed names win over the flat reject.
// parse_config_text checks this before unknown-key detection so the issue
// names the key and its excluding rule (spec D-3).
[[nodiscard]] std::optional<std::string> find_rejected_key(const JsonValue& root);

// Converts config file text into a draft without touching the filesystem.
// Every JSON or shape problem becomes a ConfigIssue; values are never echoed.
[[nodiscard]] std::variant<FrontendConfigDraft, ConfigErrors> parse_config_text(
    std::string_view text, const std::filesystem::path& config_directory);

// parse_config_text's reader over an already parsed JSON object (model launcher: a model card's
// "config" member, plan 2026-10-23 Task 1). The same rules in the same order: a fraction outside
// a decimal row, an excluded upstream key, then unknown keys, duplicates and types.
[[nodiscard]] std::variant<FrontendConfigDraft, ConfigErrors> parse_config_object(
    const JsonValue& root, const std::filesystem::path& config_directory);
// The text of an operator file read the way read_config_file reads the config (the directory
// opened as a trusted root, the file beneath it without following a link, not writable by
// others, bounded at kMaxConfigFileBytes); issues name "config file". For model cards.
struct TrustedConfigText final {
  std::string text;
  std::filesystem::path directory;
};
[[nodiscard]] std::variant<TrustedConfigText, ConfigErrors> read_trusted_config_text(const std::filesystem::path& file);
// Reads a config file (bounded at kMaxConfigFileBytes) and parses it; the
// file's directory becomes the draft's config_directory. Task 12 replaces the
// read with a trusted, handle-based open.
[[nodiscard]] std::variant<FrontendConfigDraft, ConfigErrors> read_config_file(const std::filesystem::path& file);

// Overlays command-line entries on a file draft: flags win, and every key set
// in both is listed in overridden_by_command_line.
[[nodiscard]] FrontendConfigDraft merge_drafts(FrontendConfigDraft file, const FrontendConfigDraft& flags);

// Nests dotted key paths into sections: {"a.b": "1"} becomes {"a": {"b": 1}}.
// Values are already-encoded JSON literals, written as given. Two-space indent,
// no trailing newline; a member's position follows the map's order.
[[nodiscard]] std::string render_config_literals(const std::map<std::string, std::string>& literals);

// Strict JSON (render_config_literals plus a final newline); values exactly as
// in the draft. The inverse of parse_config_text for the draft's entries.
[[nodiscard]] std::string render_config_document(const FrontendConfigDraft& draft);

}  // namespace secure_frontend
