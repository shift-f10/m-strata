#pragma once

// SP5 M1 Task 13 (Ruling T13-b): the supervised serve path, extracted VERBATIM from
// secure_server_main.cpp so `strata serve` runs the same code the standalone server
// runs. Nothing here parses a command line: the caller decides where the
// FrontendConfig came from (flags + file, or a config file alone). Foreground only —
// there is no daemonization, no server.json writer, and no lock (the pin's
// serve/server.py has none; the PX server.json writer has never existed).

#include "secure_frontend/frontend_config.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace secure_frontend {

// The full boot: state roots, compose, tokenizer, supervisor, listener, pool — and
// the SP3 I1 shutdown path (SIGTERM/Ctrl handler -> drain -> 0). Returns a process
// exit code; the messages are the server's own (stderr/stdout).
// What a config's old rows said of the plugins that took them over (move_old_plugin_rows): kept as each plugin's choice
// once the state folder is checked.
struct OldPluginChoices final {
  std::optional<bool> search{};  // search.enabled
  std::optional<bool> email{};   // alerts.email.enabled
};

// `old`: kept as the plugins' choices, and the old SMTP password moved to the email plugin, once the state folder is
// checked.
[[nodiscard]] int server_run(const FrontendConfig& config, bool expert, const OldPluginChoices& old = {});

// strata-unified-server --check: describe, launch-config issues, then the token read.
[[nodiscard]] int server_check(const FrontendConfig& config, const std::vector<std::string>& overridden, bool expert);
// Model launcher (plan 2026-10-23 Task 3): the boot of a main config with models.cards. The
// cards are loaded (a bad one refused by name, D9), each validated with the main config; the
// server-wide settings come from the main config; the card models.start names starts after
// bind (none: the server waits for the page, D5). The same SP3 I1 shutdown. With
// models.downloads the download routes are served (plan 2026-10-23-model-downloads Task 4): each
// transfer is a `strata --config <config_file> pull` child, so the config file is needed.
[[nodiscard]] int launcher_run(const FrontendConfigDraft& main, const std::optional<std::filesystem::path>& config_file,
                               const OldPluginChoices& old = {});
// --check for a launcher config: the server's settings, each card's verdict, the token read.
[[nodiscard]] int launcher_check(const FrontendConfigDraft& main, const std::vector<std::string>& overridden);

// Web search and email alerts became plugins (owner 2026-10-30; plugin spec section 10): a config's old search.* and
// alerts.email.* rows are read as plugins.search.*, tools.max_rounds and plugins.email.* (move_plugin_rows). A serve
// (`serve`) also moves them in `config_file`, once, and says so on stderr; a --check says on stdout what a start would
// move. Returns what the old rows said of each plugin's on or off (server_run, launcher_run).
[[nodiscard]] OldPluginChoices move_old_plugin_rows(FrontendConfigDraft& draft, const std::optional<std::filesystem::path>& config_file,
                                                    bool serve);

}  // namespace secure_frontend
