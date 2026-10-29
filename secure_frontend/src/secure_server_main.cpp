#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/api_router.hpp"
#include "secure_frontend/chat_execution.hpp"
#include "secure_frontend/chat_route.hpp"
#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/config.hpp"
#include "secure_frontend/connection_pool.hpp"
#include "secure_frontend/engine_arguments.hpp"
#include "secure_frontend/engine_environment.hpp"
#include "secure_frontend/engine_stdio_process.hpp"
#include "secure_frontend/engine_supervisor.hpp"
#include "secure_frontend/first_start.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/frontend_init.hpp"
#include "secure_frontend/generation_gate.hpp"
#include "secure_frontend/http_policy.hpp"
#include "secure_frontend/http_socket_server.hpp"
#include "secure_frontend/image_payload.hpp"
#include "secure_frontend/launch_composition.hpp"
#include "secure_frontend/privilege_check.hpp"
#include "secure_frontend/process_defaults.hpp"
#include "secure_frontend/request_log.hpp"
#include "secure_frontend/server_cli.hpp"
#include "secure_frontend/server_bootstrap.hpp"
#include "secure_frontend/shutdown_signal.hpp"
#include "secure_frontend/subsystem_limits.hpp"
#include "secure_frontend/test_hooks.hpp"
#include "secure_frontend/token_reader.hpp"
#include "secure_frontend/token_verifier.hpp"
#include "secure_frontend/tokenizer_pack.hpp"
#include "secure_frontend/utf8_argv.hpp"
#include "secure_frontend/vision_supervisor.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>



int main(int argc, char* argv[]) {
  // Spec §10: Winsock is initialized once per process (bind() no longer calls WSAStartup).
  const secure_frontend::WinsockGuard winsock;
#if defined(SECURE_FRONTEND_TEST_HOOKS)
  std::cerr << "warning: test hooks compiled in (" << secure_frontend::kTestHooksMarker << "); never ship this binary\n";
#endif
  secure_frontend::install_process_defaults();
#if defined(_WIN32)
  // Spec A2: UTF-8 from the wide command line, not the ANSI code page (utf8_argv.hpp).
  const auto utf8_arguments = secure_frontend::utf8_command_line(argc, argv);
  if (!utf8_arguments) {
    std::cerr << "Error: the command line is not valid Unicode\nRun strata-unified-server --help for usage.\n";
    return EXIT_FAILURE;
  }
  std::vector<const char*> utf8_argv;
  for (const auto& argument : *utf8_arguments) utf8_argv.push_back(argument.c_str());
  utf8_argv.push_back(nullptr);
  const auto parsed = secure_frontend::parse_server_command_line(static_cast<int>(utf8_arguments->size()), utf8_argv.data());
#else
  const auto parsed = secure_frontend::parse_server_command_line(argc, argv);
#endif
  if (const auto* error = std::get_if<secure_frontend::CliError>(&parsed)) {
    std::cerr << "Error: " << error->message << "\nRun strata-unified-server --help for usage.\n";
    return EXIT_FAILURE;
  }
  const auto& invocation = std::get<secure_frontend::ServerInvocation>(parsed);
  if (invocation.command == secure_frontend::ServerCommand::help) {
    std::cout << secure_frontend::server_usage();
    return EXIT_SUCCESS;
  }
  // Spec §3.12 (G4): never as root or with an elevated Windows token, for
  // init and --check too. No flag or variable overrides this.
  if (const auto refused = secure_frontend::privileged_start_error(secure_frontend::current_process_privileges())) {
    std::cerr << *refused << '\n';
    return EXIT_FAILURE;
  }
  if (invocation.command == secure_frontend::ServerCommand::init) {
    const auto failure = secure_frontend::init_frontend(*invocation.config_file, invocation.flags, invocation.force);
    if (failure.has_value()) {
      std::cerr << "init: " << failure->message << '\n';
      for (const auto& issue : failure->issues) std::cerr << "Configuration error: " << secure_frontend::format_issue(issue) << '\n';
      return EXIT_FAILURE;
    }
    std::cout << "init: wrote the configuration file and, for a file token source, a new token (not shown).\n";
    return EXIT_SUCCESS;
  }
  // GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §3): with no --config and no
  // flag that names the model (uses_data_folder) the server uses its data folder, <the executable's folder>/data by
  // default, the options given overriding its config; a serve creates it the first time (a --check never creates
  // anything).
  secure_frontend::ServerInvocation started = invocation;
  if (secure_frontend::uses_data_folder(invocation)) {
    const auto install = secure_frontend::executable_directory();
    const auto folder = secure_frontend::data_folder_layout(invocation.data_dir.value_or(install / "data"));
    if (invocation.command == secure_frontend::ServerCommand::serve) {
      // the engine beside the server must be owner-only for the server to start it (a copied folder may not be)
      const auto secured = secure_frontend::secure_install_folder(install);
      if (const auto* failure = std::get_if<secure_frontend::FirstStartError>(&secured)) {
        std::cerr << "Install folder: " << failure->message << '\n';
        return EXIT_FAILURE;
      }
      if (std::get<bool>(secured)) std::cerr << "strata: made the install folder owner-only (the engine runs from it)\n";
      const auto ensured = secure_frontend::ensure_data_folder(folder, secure_frontend::bundled_engine(install));
      if (const auto* failure = std::get_if<secure_frontend::FirstStartError>(&ensured)) {
        std::cerr << "Data folder: " << failure->message << '\n';
        return EXIT_FAILURE;
      }
      if (std::get<secure_frontend::FirstStartResult>(ensured).created) {
        std::cerr << "strata: first start - created the data folder (config.json, the API key, cards and state); "
                     "add a model on the page's Models tab\n";
      }
    }
    started.config_file = folder.config_file;
  }
  std::vector<std::string> overridden;
  // Model launcher (plan 2026-10-23 Task 3): with models.cards the cards hold the models, and
  // the main config is validated together with each of them.
  const auto draft = secure_frontend::load_server_draft(started, &overridden);
  if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&draft)) {
    for (const auto& issue : *issues) std::cerr << "Configuration error: " << secure_frontend::format_issue(issue) << '\n';
    return EXIT_FAILURE;
  }
  if (std::get<secure_frontend::FrontendConfigDraft>(draft).entries.contains("models.cards")) {
    const auto& main_draft = std::get<secure_frontend::FrontendConfigDraft>(draft);
    if (invocation.command == secure_frontend::ServerCommand::check) return secure_frontend::launcher_check(main_draft, overridden);
    return secure_frontend::launcher_run(main_draft, started.config_file);
  }
  const auto loaded = secure_frontend::validate(std::get<secure_frontend::FrontendConfigDraft>(draft));
  if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&loaded)) {
    for (const auto& issue : *issues) std::cerr << "Configuration error: " << secure_frontend::format_issue(issue) << '\n';
    return EXIT_FAILURE;
  }
  const auto config = std::get<secure_frontend::ValidatedConfig>(loaded).shared();
  if (invocation.command == secure_frontend::ServerCommand::check) return secure_frontend::server_check(*config, overridden, invocation.expert);
  return secure_frontend::server_run(*config, invocation.expert);
}
