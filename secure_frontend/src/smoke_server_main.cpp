#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/api_router.hpp"
#include "secure_frontend/config_defaults.hpp"
#include "secure_frontend/config.hpp"
#include "secure_frontend/http_policy.hpp"
#include "secure_frontend/http_request_handler.hpp"
#include "secure_frontend/http_socket_server.hpp"
#include "secure_frontend/model_inventory.hpp"
#include "secure_frontend/process_defaults.hpp"
#include "secure_frontend/secret_source.hpp"
#include "secure_frontend/token_reader.hpp"
#include "secure_frontend/token_verifier.hpp"

#include <charconv>
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

namespace {

using secure_frontend::SecretSource;

struct Options final {
  std::optional<std::string> host;
  std::optional<std::uint32_t> port;
  std::optional<std::string> token_environment;
  std::optional<std::string> token_file;
  std::optional<std::filesystem::path> secret_root;
  std::optional<std::filesystem::path> models_root;
  bool allow_remote{false};
  bool allow_wildcard_bind{false};
};

void usage(std::ostream& output) {
  output << "Usage: strata-secure-smoke-server --models-root PATH "
            "(--token-env NAME | --token-file RELATIVE --secret-root PATH) [options]\n"
            "\n"
            "Options:\n"
            "  --host IP                 Bind IP literal (default: 127.0.0.1)\n"
            "  --port PORT               TCP port 1-65535 (default: 8080)\n"
            "  --allow-remote            Required for every non-loopback bind\n"
            "  --allow-wildcard-bind     Also required for 0.0.0.0 or ::\n"
            "  --models-root PATH        Existing local directory of .gguf files\n"
            "  --token-env NAME          Read bearer token from one environment variable\n"
            "  --token-file RELATIVE     Read token from owner-only file below --secret-root\n"
            "  --secret-root PATH        Required with --token-file\n"
            "  --help                     Print this help\n"
            "\n"
            "No models are downloaded, loaded, or executed. This smoke server exposes only\n"
            "authenticated GET /health and GET /v1/models; all other routes return 404.\n";
}

bool next_value(const int argc, char* argv[], int* index, std::string* output, std::string* error) {
  if (*index + 1 >= argc) {
    *error = std::string{"missing value for "} + argv[*index];
    return false;
  }
  *output = argv[++*index];
  if (output->empty()) {
    *error = "empty command-line option value";
    return false;
  }
  return true;
}

bool parse_port(const std::string_view value, std::uint32_t* output) {
  std::uint32_t parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return false;
  *output = parsed;
  return true;
}

bool parse_options(const int argc, char* argv[], Options* options, bool* help, std::string* error) {
  *help = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--help") {
      *help = true;
      return true;
    }
    if (argument == "--allow-remote") {
      if (options->allow_remote) { *error = "duplicate --allow-remote"; return false; }
      options->allow_remote = true;
      continue;
    }
    if (argument == "--allow-wildcard-bind") {
      if (options->allow_wildcard_bind) { *error = "duplicate --allow-wildcard-bind"; return false; }
      options->allow_wildcard_bind = true;
      continue;
    }
    std::string value;
    if (!next_value(argc, argv, &index, &value, error)) return false;
    if (argument == "--host") {
      if (options->host.has_value()) { *error = "duplicate --host"; return false; }
      options->host = std::move(value);
    } else if (argument == "--port") {
      std::uint32_t port = 0;
      if (options->port.has_value() || !parse_port(value, &port)) {
        *error = "--port must be an unsigned decimal integer";
        return false;
      }
      options->port = port;
    } else if (argument == "--models-root") {
      if (options->models_root.has_value()) { *error = "duplicate --models-root"; return false; }
      options->models_root = std::filesystem::path{std::move(value)};
    } else if (argument == "--token-env") {
      if (options->token_environment.has_value()) { *error = "duplicate --token-env"; return false; }
      options->token_environment = std::move(value);
    } else if (argument == "--token-file") {
      if (options->token_file.has_value()) { *error = "duplicate --token-file"; return false; }
      options->token_file = std::move(value);
    } else if (argument == "--secret-root") {
      if (options->secret_root.has_value()) { *error = "duplicate --secret-root"; return false; }
      options->secret_root = std::filesystem::path{std::move(value)};
    } else {
      *error = std::string{"unknown option: "} + std::string{argument};
      return false;
    }
  }
  if (!options->models_root.has_value()) { *error = "--models-root is required"; return false; }
  if (options->token_environment.has_value() == options->token_file.has_value()) {
    *error = "supply exactly one of --token-env or --token-file";
    return false;
  }
  if (options->token_file.has_value() != options->secret_root.has_value()) {
    *error = "--token-file and --secret-root must be supplied together";
    return false;
  }
  return true;
}

std::string host_header(const secure_frontend::Config& config) {
  const bool ipv6 = config.host.find(':') != std::string::npos;
  return (ipv6 ? "[" + config.host + "]" : config.host) + ":" + std::to_string(config.port);
}

int run(const Options& options) {
  const auto source_result = options.token_environment.has_value()
      ? secure_frontend::make_environment_secret_source(*options.token_environment)
      : secure_frontend::make_protected_file_secret_source(*options.token_file);
  const auto* source = std::get_if<SecretSource>(&source_result);
  if (source == nullptr) {
    std::cerr << "Invalid token source.\n";
    return EXIT_FAILURE;
  }
  const std::filesystem::path secret_root = options.secret_root.value_or(std::filesystem::path{"."});
  auto token_result = secure_frontend::read_token(*source, secret_root);
  auto* token = std::get_if<secure_frontend::SecretBytes>(&token_result);
  if (token == nullptr) {
    std::cerr << "Unable to read API token.\n";
    return EXIT_FAILURE;
  }
  auto verifier = std::make_shared<const secure_frontend::TokenVerifier>(std::move(*token));

  const secure_frontend::ConfigInput input{
      .host = options.host,
      .port = options.port,
      .api_key = std::string{"configured"},
      .allow_remote = options.allow_remote,
      .allow_wildcard_bind = options.allow_wildcard_bind,
  };
  const auto config_result = secure_frontend::validate_config(input);
  const auto* config = std::get_if<secure_frontend::Config>(&config_result);
  if (config == nullptr) {
    std::cerr << "Invalid listener configuration.\n";
    return EXIT_FAILURE;
  }

  const auto models_result = secure_frontend::enumerate_local_models(*options.models_root);
  const auto* manifests = std::get_if<std::vector<secure_frontend::ModelManifest>>(&models_result);
  if (manifests == nullptr) {
    std::cerr << "Unable to inventory local models.\n";
    return EXIT_FAILURE;
  }
  // SP3 Task 6b: the router serves one model id (spec §3.6, E9); this smoke
  // binary serves the first local model, if any. Task 8b reworks the smoke mains.
  std::string model_id;
  std::string owned_by{"local"};
  if (!manifests->empty()) {
    model_id = manifests->front().id;
    owned_by = manifests->front().owned_by;
  }

  const std::string authority = host_header(*config);
  const auto bind_address = secure_frontend::parse_listener_address(config->host);
  if (!bind_address.has_value()) {
    std::cerr << "Invalid listener configuration.\n";
    return EXIT_FAILURE;
  }
  // The bound authority (plus localhost on loopback) needs no alias. This smoke
  // binary has no typed config and so no listener.host_aliases: a wildcard bind
  // keeps its own authority as the one alias, or no Host could ever match. No CORS.
  const std::vector<std::string> aliases =
      bind_address->scope() == secure_frontend::AddressScope::wildcard ? std::vector<std::string>{authority}
                                                                        : std::vector<std::string>{};
  secure_frontend::HttpPolicy policy = secure_frontend::make_http_policy(*bind_address, config->port, aliases, {});
  secure_frontend::ApiRouter router(secure_frontend::ApiRouterConfig{
      .policy = std::move(policy),
      .validator = secure_frontend::make_bearer_validator(verifier),
      .model_id = std::move(model_id),
      .owned_by = std::move(owned_by),
      .chat_body_limit = static_cast<std::size_t>(secure_frontend::config_defaults::kChatMaxBodyBytes),
      .health = [] { return secure_frontend::HealthView{true, "ready", ""}; },
      // No engine here: the chat route answers 404 like every other non-GET route.
      .chat = [](std::string_view, const secure_frontend::RequestContext&) -> secure_frontend::RouteResult {
        return secure_frontend::make_error_response(secure_frontend::ApiError::not_found);
      }});
  auto server = secure_frontend::HttpSocketServer::bind(
      *config, static_cast<int>(secure_frontend::config_defaults::kListenerBacklog), nullptr);
  if (!server.has_value()) {
    std::cerr << "Unable to bind secure listener.\n";
    return EXIT_FAILURE;
  }
  std::cout << "Secure smoke server listening on http://" << authority
            << " (authenticated; Ctrl-C to stop)\n";
  std::cout.flush();
  const secure_frontend::ConnectionLimits limits{};   // SP1's schema defaults
  for (;;) {
    std::string error;
    if (!server->serve_one(router, limits, &error) && server->listener_failed()) {
      std::cerr << "listener stopped: " << error << '\n';
      return EXIT_FAILURE;
    }
    // Client failures are intentionally opaque; the listener remains alive.
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  secure_frontend::install_process_defaults();
  // Spec §10: Winsock is initialized once per process (bind() no longer calls WSAStartup).
  const secure_frontend::WinsockGuard winsock;
  if (!winsock.ok()) {
    std::cerr << "Networking initialization failed.\n";
    return EXIT_FAILURE;
  }
  Options options;
  bool help = false;
  std::string error;
  if (!parse_options(argc, argv, &options, &help, &error)) {
    std::cerr << "Error: " << error << "\n";
    usage(std::cerr);
    return EXIT_FAILURE;
  }
  if (help) {
    usage(std::cout);
    return EXIT_SUCCESS;
  }
  return run(options);
}
