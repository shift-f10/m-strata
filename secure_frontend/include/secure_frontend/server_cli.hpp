#pragma once

#include "secure_frontend/frontend_config.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

enum class ServerCommand { serve, check, init, help };

struct ServerInvocation final {
  ServerCommand command{ServerCommand::serve};
  std::optional<std::filesystem::path> config_file;
  // GUI-first start-up (spec 2026-10-27 §3): the data folder (default <the executable's folder>/data) a start with no
  // --config and no configuration option uses; excludes --config.
  std::optional<std::filesystem::path> data_dir;
  bool force{false};
  bool expert{false};  // --expert: bring-up mode (SP4 §4.9 d) for serve and --check; no effect on init
  FrontendConfigDraft flags;  // every entry has ConfigSource::command_line
};

// Command-line problems. The message names a flag at most, never a value.
struct CliError final {
  std::string message;
};

// Each existing option sets exactly one configuration key (README table).
// Values are stored uncoerced except that an all-digit value for an integer
// key becomes an integer; validate() judges everything else.
[[nodiscard]] std::variant<ServerInvocation, CliError> parse_server_command_line(int argc, const char* const* argv);

// The option name up to '=', at most 64 printable ASCII bytes ('?' for others).
[[nodiscard]] std::string safe_option_name(std::string_view argument);

[[nodiscard]] std::string server_usage();

// defaults < config file < flags, then validate(). `overridden` receives the
// keys set in both the file and on the command line.
[[nodiscard]] ConfigValidationResult load_server_config(const ServerInvocation& invocation,
                                                        std::vector<std::string>* overridden);
// defaults < config file < flags, not yet validated: with models.cards the model launcher
// validates the main config together with each card (server_bootstrap.hpp).
[[nodiscard]] std::variant<FrontendConfigDraft, ConfigErrors> load_server_draft(const ServerInvocation& invocation,
                                                                               std::vector<std::string>* overridden);

}  // namespace secure_frontend
