// SP5 M1 Task 13 (Ruling T13-b): the supervised serve path moved VERBATIM out of
// secure_server_main.cpp — the same code strata-unified-server runs and the code
// `strata serve` runs. This file parses no command line and installs no daemon:
// foreground boot, pinned order, SP3 I1 shutdown (see server_bootstrap.hpp).

#include "secure_frontend/server_bootstrap.hpp"

#include "secure_frontend/about_info.hpp"
#include "secure_frontend/card_routes.hpp"
#include "secure_frontend/login_link.hpp"
#if defined(SECURE_FRONTEND_EMAIL_ALERTS)
#include "secure_frontend/alert_routes.hpp"
#include "secure_frontend/email_alerts.hpp"
#endif
#if defined(SECURE_FRONTEND_WEB_SEARCH)
#include "secure_frontend/first_start.hpp"
#include "secure_frontend/search_routes.hpp"
#include "secure_frontend/search_service.hpp"
#endif
#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/api_router.hpp"
#include "secure_frontend/chat_execution.hpp"
#include "secure_frontend/chat_route.hpp"
#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/config.hpp"
#include "secure_frontend/config_routes.hpp"
#include "secure_frontend/connection_pool.hpp"
#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/download_catalog.hpp"
#include "secure_frontend/download_manager.hpp"
#include "secure_frontend/engine_arguments.hpp"
#include "secure_frontend/engine_environment.hpp"
#include "secure_frontend/engine_stdio_process.hpp"
#include "secure_frontend/engine_supervisor.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/generation_gate.hpp"
#include "secure_frontend/host_readings.hpp"
#include "secure_frontend/http_policy.hpp"
#include "secure_frontend/http_socket_server.hpp"
#include "secure_frontend/image_payload.hpp"
#include "secure_frontend/launch_composition.hpp"
#include "secure_frontend/model_cards.hpp"
#include "secure_frontend/model_launcher.hpp"
#include "secure_frontend/model_runtime.hpp"
#include "secure_frontend/privilege_check.hpp"
#include "secure_frontend/process_defaults.hpp"
#include "secure_frontend/request_log.hpp"
#include "secure_frontend/serve_monitor.hpp"
#include "secure_frontend/shutdown_signal.hpp"
#include "secure_frontend/subsystem_limits.hpp"
#include "secure_frontend/test_hooks.hpp"
#include "secure_frontend/token_store.hpp"
#include "secure_frontend/token_reader.hpp"
#include "secure_frontend/token_verifier.hpp"
#include "secure_frontend/tokenizer_pack.hpp"
#include "secure_frontend/vision_supervisor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {


using secure_frontend::EngineProfile;
using secure_frontend::FrontendConfig;

// The page's About (owner 2026-10-27): the engine folder's build record and this PC's hardware, read once at the start;
// the GPU as the Monitor last read it, when the Monitor runs.
secure_frontend::MetricsProvider about_provider(const std::filesystem::path& engine,
                                                const std::optional<secure_frontend::ServeMonitor>& monitor) {
  // multi-GPU (owner 2026-10-28): every GPU as nvidia-smi lists them, for a card's GPUs choice - read once, on the first
  // request (nvidia-smi as the Monitor runs it: from the system directory, a fixed argv and environment)
  struct Listed final {
    std::once_flag once;
    std::vector<secure_frontend::GpuListing> gpus;
  };
  return [build = secure_frontend::read_engine_build(engine.parent_path()), hardware = secure_frontend::hardware_facts(), &monitor,
          listed = std::make_shared<Listed>()] {
    auto facts = hardware;
    if (monitor.has_value()) facts.gpu = monitor->gpu_fact();
    std::call_once(listed->once, [&listed] {
      listed->gpus = secure_frontend::list_gpus({.executable = secure_frontend::nvidia_smi_path(),
                                                 .arguments = secure_frontend::nvidia_smi_list_arguments(),
                                                 .environment = secure_frontend::nvidia_smi_environment(),
                                                 .no_new_privileges = true, .insert_serve = false});
    });
    facts.gpus = listed->gpus;
    return secure_frontend::render_about_json(build, facts);
  };
}

std::string host_header(const secure_frontend::Config& config) {
  const bool ipv6 = config.host.find(':') != std::string::npos;
  return (ipv6 ? "[" + config.host + "]" : config.host) + ":" + std::to_string(config.port);
}

secure_frontend::TokenReadResult read_configured_token(const FrontendConfig& config) {
  if (config.roots.secret.has_value()) return secure_frontend::read_token(config.token, *config.roots.secret);
  return secure_frontend::read_token(config.token, std::filesystem::path{"."});
}

int server_check_impl(const FrontendConfig& config, const std::vector<std::string>& overridden, const bool expert) {
  std::cout << secure_frontend::describe(config);
  for (const auto& key : overridden) std::cout << key << ": the command line overrides the config file\n";
  std::cout.flush();
  // SP4 §4.9 c/d (F-8): the launch-related config errors serve would refuse, for
  // the same mode (managed, or --expert). Pure: roots.models is not read.
  if (const auto issues = secure_frontend::launch_config_issues(config, expert); !issues.empty()) {
    for (const auto& issue : issues) std::cerr << "Configuration error: " << issue << '\n';
    return EXIT_FAILURE;
  }
  if (config.listener.loopback_without_key) {
    std::cout << "token: not used (listener.loopback_without_key: loopback only)\nConfiguration is valid.\n";
    return EXIT_SUCCESS;
  }
  // The verdict follows the token read: a configuration whose token cannot be
  // read is not reported valid first.
  const auto token = read_configured_token(config);
  if (const auto* error = std::get_if<secure_frontend::TokenReadError>(&token)) {
    std::cerr << "token: unavailable: " << error->message << '\n'
              << "Configuration is not usable: the API token cannot be read.\n";
    return EXIT_FAILURE;
  }
  std::cout << "token: available (value not shown)\nConfiguration is valid.\n";
  return EXIT_SUCCESS;
}

// roots.state (--state-dir) holds the engine's cwd, temp and CUDA cache (spec
// §3.9, §3.12). validate() only checked that it is absolute (--check stays
// side-effect free); serve refuses, before anything is created, bound or
// spawned, unless it is an existing owner-only directory.
bool state_dir_is_usable(const FrontendConfig& config) {
  if (!config.roots.state) {
    std::cerr << "roots.state (--state-dir) is required to serve.\n";
    return false;
  }
  // POSIX: lstat -- a real directory, not a symlink, owned by the effective
  // uid, no group/other bits. Windows: opened without following a junction or
  // symlink (refused), owner the current user, protected DACL with allow-ACEs
  // only for the user, SYSTEM and Administrators (is_private_state_root).
  const bool owner_only = secure_frontend::is_private_state_root(*config.roots.state);
  if (!owner_only) {
    std::cerr << "--state-dir must be an existing owner-only directory.\n";
    return false;
  }
  return true;
}

// listener.monitor (session log step 16): this machine's readings once a second; the GPU's
// through the system's nvidia-smi, its own child (no shell, ProgramFiles alone, the Job),
// started after bind like every child and stopped (before the supervisor) on every way out.
// Owner 2026-10-28: also every disk's reads and writes (upstream's telemetry.py _disk()) and the engine that is up
// (its process's memory, its READY readings, its conversation cache), from `engine` - which outlives this sampler.
struct HostSampler final {
  using EngineSampler = std::function<std::optional<secure_frontend::EngineSample>()>;
  secure_frontend::ServeMonitor* monitor{nullptr};
  secure_frontend::GpuSampler gpu;
  std::atomic<bool> stopping{false};
  std::thread thread;
  void start(secure_frontend::ServeMonitor& target, EngineSampler engine) {
    monitor = &target;
    gpu.start({.executable = secure_frontend::nvidia_smi_path(), .arguments = secure_frontend::nvidia_smi_arguments(),
               .environment = secure_frontend::nvidia_smi_environment(), .no_new_privileges = true, .insert_serve = false});
    thread = std::thread([this, engine = std::move(engine)] {
      secure_frontend::CpuMeter cpu;
      secure_frontend::DiskMeter disk;
      while (!stopping.load()) {
        secure_frontend::HostSample sample;
        sample.cpu_percent = cpu.sample();
        sample.memory = secure_frontend::read_memory();
        sample.gpu = gpu.latest();
        if (!sample.gpu.has_value()) sample.gpu_note = gpu.note();
        if (const auto rates = disk.sample()) {
          sample.disk_read_mb = rates->read_mb;
          sample.disk_write_mb = rates->write_mb;
        }
        if (engine) sample.engine = engine();
        monitor->add_host_sample(sample);
        for (int slice = 0; slice < 10 && !stopping.load(); ++slice) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }
  ~HostSampler() {
    stopping = true;
    if (thread.joinable()) thread.join();
    gpu.stop();
  }
};

// The downloads' children (strata, the prepare Python) get a closed environment: the system's
// own variables and a private temp folder, plus STRATA_GGUF_PY when the server has it (upstream's
// pack tools find gguf-py through it).
std::map<std::string, std::string, std::less<>> download_environment(const std::filesystem::path& temp) {
  std::map<std::string, std::string, std::less<>> environment;
  const std::string temp_text = secure_frontend::path_to_utf8(temp);
#if defined(_WIN32)
  const auto system = secure_frontend::query_windows_system_values();
  environment.emplace("SystemRoot", system.system_root);
  environment.emplace("WINDIR", system.system_root);
  environment.emplace("SystemDrive", system.system_drive);
  environment.emplace("PATH", system.system_root + "\\System32");
  environment.emplace("TEMP", temp_text);
  environment.emplace("TMP", temp_text);
#else
  environment.emplace("PATH", "/usr/bin:/bin");
  environment.emplace("TMPDIR", temp_text);
#endif
  if (const char* gguf_py = secure_frontend::crt_getenv("STRATA_GGUF_PY"); gguf_py != nullptr && *gguf_py != '\0') {
    environment.emplace("STRATA_GGUF_PY", gguf_py);
  }
  return environment;
}

#if defined(SECURE_FRONTEND_WEB_SEARCH)
// Web search (spec 2026-10-27-web-search-design §2): strata-search beside the server, in the downloads' closed
// environment with its own owner-only temp folder under the state root; nullopt (and why) when that cannot be made.
std::optional<secure_frontend::SearchServiceSettings> search_settings(const FrontendConfig& config) {
  if (!config.roots.state.has_value()) return std::nullopt;
  const auto temp = *config.roots.state / "search";
  if (const auto opened = secure_frontend::open_root(temp, secure_frontend::RootPolicy::create_private);
      const auto* failure = std::get_if<secure_frontend::PathError>(&opened)) {
    std::cerr << "search: " << secure_frontend::path_to_utf8(temp) << ": " << failure->message << '\n';
    return std::nullopt;
  }
#if defined(_WIN32)
  const auto program = secure_frontend::executable_directory() / "strata-search.exe";
#else
  const auto program = secure_frontend::executable_directory() / "strata-search";
#endif
  return secure_frontend::SearchServiceSettings{.program = program,
                                                .url = config.search.url,
                                                .results = config.search.results,
                                                .timeout = config.search.timeout,
                                                .environment = download_environment(temp),
                                                .today = {},
                                                .log = [](const std::string_view line) { std::cerr << line << '\n' << std::flush; }};
}
#endif

// Owner 2026-10-29: a server that copies its engines' own output into this log says so at every start, with what that
// output can hold and what to do about it (docs/security/threat-model.md, the second engine-stderr exception).
// Owner 2026-10-29 ("mark that as experimental"): a server that takes model folders on network shares says so at every
// start, with what a share allows (docs/security/threat-model.md, model folders on network shares).
void warn_of_network_shares(const FrontendConfig& config) {
  if (!config.roots.allow_network_shares) return;
  std::cerr << "strata: warning: roots.allow_network_shares is true (experimental): model folders may be on network "
               "shares, used as they are - anyone who can write to a share can change what a model runs, a model's "
               "first load streams it over the network, and an engine fails if the share drops while it reads. Turn it "
               "off in Settings > Model folders when you do not need it.\n";
}

void warn_of_engine_output(const FrontendConfig& config) {
  if (secure_frontend::debugging_build()) {
    std::cerr << "strata: warning: this is a debugging build (make --debugging): it logs every request and the engine's own "
                 "output unless the config turns them off. Build again without --debugging for everyday use.\n";
  }
  if (!config.logging.engine_output) return;
  // Settings > Troubleshooting is a debugging build's only (owner 2026-10-29); any other build names the config row
  std::cerr << "strata: warning: logging.engine_output is true: the engine's own output goes to this log, and it can hold "
               "what the engine prints of your prompts and the paths of your model files. Turn it off when you are done "
            << (secure_frontend::debugging_build() ? "(Settings > Troubleshooting)" : "(logging.engine_output in the config file)")
            << ", keep this log private, and look through it before you share it.\n";
}

int server_run_impl(const FrontendConfig& config, const bool expert) {
  // Ruling T13-c: server-grade process defaults, also when serve arrives
  // through the strata CLI (install_process_defaults is idempotent).
  secure_frontend::install_process_defaults();
  if (!state_dir_is_usable(config)) return EXIT_FAILURE;
  // roots.logs / roots.cache: validate() only checked them, so --check stays
  // side-effect free; serve creates a missing one here, before bind or spawn.
  if (const auto issues = secure_frontend::create_runtime_roots(config); !issues.empty()) {
    for (const auto& issue : issues) std::cerr << "Configuration error: " << secure_frontend::format_issue(issue) << '\n';
    return EXIT_FAILURE;
  }
  // Every reason below is fixed text (plus, for the inventory, an entry name
  // below roots.models, spec §6.1): never a token or a configured value.
  // listener.loopback_without_key (owner decision 2026-10-21): no key is read at all; load
  // refused a token.source next to it, and bind re-checks that the listener is loopback.
  const bool keyless = config.listener.loopback_without_key;
  secure_frontend::BearerTokenValidator validator;
  if (!keyless) {
    auto token_result = read_configured_token(config);
    auto* token = std::get_if<secure_frontend::SecretBytes>(&token_result);
    if (token == nullptr) {
      std::cerr << "Unable to read API token: " << std::get<secure_frontend::TokenReadError>(token_result).message << '\n';
      return EXIT_FAILURE;
    }
    validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(*token)));
  }

  const secure_frontend::Config listener = secure_frontend::listener_config(config);
  // Model launcher plan 2026-10-23 Task 3: the per-model half of the boot (compose, the
  // tokenizer pack, the served name and aliases, the effort tail, the supervisor config, the
  // closed environment and the launch plan, the chat route's settings) is compose_model() in
  // model_runtime.cpp, which the launcher shares. Its comments moved with it.
  auto composed_result = secure_frontend::compose_model(config, expert);
  if (auto* failure = std::get_if<secure_frontend::ComposeFailure>(&composed_result)) {
    for (const auto& message : failure->messages) std::cerr << message << '\n';
    return failure->exit_code;
  }
  auto& model = std::get<secure_frontend::ComposedModel>(composed_result);
  const std::string& model_id = model.model_id;
  const secure_frontend::SupervisorConfig& supervisor_config = model.supervisor;
  const std::filesystem::path& state_dir = *config.roots.state;
  if (!secure_frontend::prepare_state_dir(state_dir, *model.plan.environment)) {
    std::cerr << "Unable to prepare the state or cache directory.\n";
    return EXIT_FAILURE;
  }
  // The supervisor thread spawns, watches and restarts the engine (spec §3.1).
  // Declared before the listener and the router, so it outlives both; a Lease
  // lives only inside one ChatRoute::handle call.
  secure_frontend::EngineSupervisor supervisor(supervisor_config, secure_frontend::make_stdio_process_factory(std::move(model.plan)));

  const std::string authority = host_header(listener);  // the startup line only
  // SP3 Task 16 (spec §2.2, §2.5): SupervisorPort serializes the frontend's
  // snapshot/acquire/report_fatal into SP2 (D-1); GenerationGate holds the
  // single generation slot with a FIFO queue; ChatRoute validates (P4) before a
  // queue place is taken and runs the generation through the lease (P6).
  // Declaration order here — supervisor, port, gate, chat route, then router,
  // server, pool — keeps every object a pool worker touches alive until the
  // pool is joined.
  secure_frontend::SupervisorPort port(supervisor);
  secure_frontend::GenerationGate gate({.max_queued = config.limits.concurrency.max_queued_generations,
                                        .queue_timeout = config.limits.concurrency.queue_timeout});
  // SP5 M1 Task 11 (D-9 vision split; Rulings T11-y/T11-ee/T11-z) and Task 12 (C-9): the supervised encoder child and
  // the route's encoder service, from compose_model's plan (model_runtime.cpp plan_vision, shared with the cards since
  // 2026-10-28: upstream's strata-vision beside the engine, else maya-vision; the mmproj artifact and the weights from
  // the manifest's launch.mmproj; the scratch <state>/vision). Construction spawns nothing - the resident start below
  // runs only after bind, so a listener that cannot bind still never leaves an encoder child behind (CFG-01 family).
  secure_frontend::VisionRole vision_role;
  if (config.vision.enabled) {
    if (!model.vision.has_value()) {  // compose guards; fail closed twice
      std::cerr << "vision: this model declares no mmproj artifact.\n";
      return EXIT_FAILURE;
    }
    vision_role = secure_frontend::make_vision_role(*model.vision, secure_frontend::vision_process_factory(*model.vision));
  }
  secure_frontend::VisionSupervisor* const vision = vision_role.supervisor.get();
  secure_frontend::VisionService* const vision_service = vision_role.service.get();
  // listener.monitor (session log step 16): the Monitor's meter. The chat route tells it about
  // each generation, the host sampler below adds a reading a second, GET /metrics renders it.
  std::optional<secure_frontend::ServeMonitor> monitor;
  if (config.listener.monitor) monitor.emplace();
  secure_frontend::ChatRouteConfig chat_config = model.chat;
  chat_config.vision = vision_service;
  chat_config.monitor = monitor.has_value() ? &*monitor : nullptr;
#if defined(SECURE_FRONTEND_WEB_SEARCH)
  std::optional<secure_frontend::SearchService> search;  // outlives the route
  if (config.search.enabled) {
    const auto settings = search_settings(config);
    if (!settings.has_value()) return EXIT_FAILURE;
    search.emplace(*settings);
    chat_config.tools = &*search;
    chat_config.tool_rounds = config.search.max_rounds;
  }
#endif
  secure_frontend::ChatRoute chat_route(std::move(chat_config), port, gate, model.tokenizer);
  // Spec §4.1: every request goes through the header-first router (Host,
  // Origin, preflight, bearer token, route table, Content-Type, body limit)
  // before a body byte is read. Spec §3.6 (E9, re-anchor 4 / P26): /v1/models
  // lists only the served model, launch.model_id, owned by "local".
  secure_frontend::ApiRouter router(secure_frontend::ApiRouterConfig{
      .policy = secure_frontend::make_http_policy(config.listener.host, config.listener.port, config.listener.host_aliases,
                                                  config.listener.cors_origins),
      .validator = validator,
      .model_id = model_id,
      .owned_by = "local",
      .aliases = model.aliases,
      .chat_body_limit = config.limits.chat.max_body_bytes,
      .health = [&port] {
        const auto s = port.snapshot();
        return secure_frontend::HealthView{s.healthy, s.engine, s.recovery};
      },
      // ChatRoute: P4 validation, then the gate, then P6 through the lease
      // (spec §2.2). The route answers its own refusals.
      .chat = [&chat_route](const std::string_view body, const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
        return chat_route.handle(body, context);
      },
      .web_ui = config.listener.web_ui,
      .require_key = !keyless,
      .metrics = monitor.has_value() ? secure_frontend::MetricsProvider{[&monitor, &port, &gate, &model_id] {
        const auto snapshot = port.snapshot();
        return monitor->render_json({.model = model_id, .engine = snapshot.engine, .queued = gate.waiting(),
                                     .unloaded = snapshot.state == secure_frontend::SupervisorState::unloaded});
      }}
                                     : secure_frontend::MetricsProvider{},
      .about = about_provider(config.engine.executable.path(), monitor)});
  const secure_frontend::ConnectionLimits connection_bounds = secure_frontend::connection_limits(config);
  std::string bind_error;
  auto server = secure_frontend::HttpSocketServer::bind(listener, static_cast<int>(config.listener.backlog), &bind_error);
  if (!server) { std::cerr << "Unable to bind secure listener.\n"; return EXIT_FAILURE; }
  // Bound first, spawned second: a listener that cannot bind never leaves an
  // engine child behind (CFG-01). The token was read above, before any spawn.
  // Spec §3.7: once every engine is gone the supervisor shuts the listener,
  // which wakes the accept() below. Signals route to the supervisor's wake.
  supervisor.set_on_stopped([&server] { server->shutdown_listener(); });
  secure_frontend::install_shutdown_signals(supervisor.wake());
  // Spec §8: the operator-facing startup summary — the process's own settings
  // only (stderr, once; like --check prints them). Never any secret or path.
  std::cerr << "strata: listening " << authority << ", workers " << config.limits.concurrency.max_concurrent_requests
            << ", queued generations " << config.limits.concurrency.max_queued_generations << '\n' << std::flush;
  if (!supervisor.start()) { std::cerr << "Unable to start the engine supervisor.\n"; return EXIT_FAILURE; }
  // Every way out of this scope, an exception included, stops the supervisor
  // while the listener its on-stopped hook uses still exists.
  struct StopSupervisor final {
    secure_frontend::EngineSupervisor& supervisor;
    ~StopSupervisor() { supervisor.shutdown(secure_frontend::config_defaults::kShutdownBound); }
  } stop_supervisor{supervisor};
  HostSampler host_sampler;
  if (monitor.has_value()) host_sampler.start(*monitor, [&supervisor] { return supervisor.engine_sample(); });
  // SP5 M1 Task 11 (Ruling T11-z): resident (the pin's eager default, serve/server.py:
  // 2511) starts the encoder now — no READY within the policy, and boot fails, because
  // vision.enabled is a promise of images. resident=false stays stopped until the FIRST
  // image wakes it: the pin's _vision_up laziness (:2646) lives in VisionSupervisor::
  // encode. The objects were built before the ChatRoute; the spawn happens here, after
  // bind, so a listener that cannot bind still never leaves an encoder child behind.
  if (config.vision.enabled && config.vision.resident) {
    vision->start();
    if (!vision->up()) {
      std::cerr << "vision: the encoder did not reach READY; refusing to serve without it.\n";
      return EXIT_FAILURE;
    }
    std::cerr << "[strata] vision: the encoder is resident\n";
  }
  if (keyless) {
    std::cerr << "strata: warning: listener.loopback_without_key is true: the API and the page answer without a key on "
              << authority << " (loopback only; Host and Origin are still checked)\n";
  }
  warn_of_engine_output(config);
  warn_of_network_shares(config);
  std::cout << "Secure server listening on http://" << authority
            << (keyless ? " (no API key: loopback only; Ctrl-C to stop)\n" : " (authenticated; Ctrl-C to stop)\n");
  std::cout.flush();
  // Task 12: the server is concurrent (spec §2.1). The accept loop runs on this
  // thread; the workers exist for the life of the pool, which is constructed
  // after supervisor.start() and destroyed (already joined) before the
  // supervisor shutdown below.
  secure_frontend::RequestLogger request_log(config.logging.requests, secure_frontend::stderr_sink());
  secure_frontend::ConnectionPool pool(
      *server,
      secure_frontend::PoolLimits{.workers = config.limits.concurrency.max_concurrent_requests,
                                  .per_remote_cap = config.limits.concurrency.max_connections_per_remote_address,
                                  .connection = connection_bounds},
      router,
      // Spec §8: one fixed-field line per finished connection when logging.requests is on.
      [&request_log](const secure_frontend::FinishedConnection& finished) { request_log.record(secure_frontend::log_record_for(finished)); });
  // Pre-flight P33 (kept per ruling T16-pool: shutdown_signalled() alone would
  // miss the recovery-exhausted exit): the supervisor's stop request ends the
  // accept loop; the on-stopped hook closing the listener is the second exit
  // path. T12b: a dead listener ends the loop with PoolExit::listener_failed,
  // handled exactly as SP2's loop handled it: orderly supervisor shutdown, exit
  // non-zero. The on-shutdown hook (Task 16) wakes gate waiters as
  // shutting_down before the workers are joined.
  const auto pool_exit = pool.run_until([&supervisor] { return supervisor.stop_requested(); }, [&gate] { gate.shutdown(); });
  if (pool_exit == secure_frontend::PoolExit::listener_failed) {
    std::cerr << "listener stopped\n";
  if (vision) vision->stop();  // the encoder role first: spawned last, stopped first
    supervisor.shutdown(secure_frontend::config_defaults::kShutdownBound);
    return EXIT_FAILURE;
  }
  if (vision) vision->stop();  // the encoder role first: spawned last, stopped first
  supervisor.shutdown(secure_frontend::config_defaults::kShutdownBound);
  // Spec C4: exit 0 means no engine is left; an unreaped one was logged.
  return supervisor.engine_left_unreaped() ? EXIT_FAILURE : EXIT_SUCCESS;
}

// Model launcher (plan 2026-10-23 Task 3): the cards that load and validate together with the
// main config at their own defaults (D9: a bad card is refused by name, the others load). The
// server's own settings are the main config's, read through the first usable card.
struct UsableCards final {
  std::vector<secure_frontend::ModelCard> cards;
  std::shared_ptr<const FrontendConfig> server;
};

// One card with the main config at its defaults; null (each issue logged by the card's name)
// when it does not validate or launch.
std::shared_ptr<const FrontendConfig> usable_card_config(const secure_frontend::FrontendConfigDraft& main,
                                                         const secure_frontend::ModelCard& card) {
  auto validated = secure_frontend::card_config(main, card, secure_frontend::default_option_values(card));
  if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&validated)) {
    for (const auto& issue : *issues) std::cerr << "model card " << card.name << " refused: " << secure_frontend::format_issue(issue) << '\n';
    return nullptr;
  }
  auto config = std::get<secure_frontend::ValidatedConfig>(validated).shared();
  if (const auto issues = secure_frontend::launch_config_issues(*config, true); !issues.empty()) {
    for (const auto& issue : issues) std::cerr << "model card " << card.name << " refused: " << issue << '\n';
    return nullptr;
  }
  return config;
}

std::optional<UsableCards> usable_cards(const secure_frontend::FrontendConfigDraft& main) {
  if (const auto issues = secure_frontend::main_config_card_issues(main); !issues.empty()) {
    for (const auto& issue : issues) std::cerr << "Configuration error: " << secure_frontend::format_issue(issue) << '\n';
    return std::nullopt;
  }
  const auto entry = main.entries.find("models.cards");
  const auto* listed = entry == main.entries.end() ? nullptr : std::get_if<std::vector<std::string>>(&entry->second.value);
  if (listed == nullptr) {
    std::cerr << "Configuration error: models.cards: must be an array of strings\n";
    return std::nullopt;
  }
  std::vector<std::filesystem::path> files;
  for (const auto& text : *listed) files.push_back(secure_frontend::path_from_utf8(text));
  auto loaded = secure_frontend::load_model_cards(files);
  for (const auto& refusal : loaded.refused) {
    for (const auto& issue : refusal.issues) {
      std::cerr << "model card " << refusal.position << (refusal.name.empty() ? std::string{} : " (" + refusal.name + ")")
                << " refused: " << secure_frontend::format_issue(issue) << '\n';
    }
  }
  UsableCards usable;
  for (auto& card : loaded.cards) {
    auto config = usable_card_config(main, card);
    if (config == nullptr) continue;
    if (usable.server == nullptr) usable.server = config;
    usable.cards.push_back(std::move(card));
  }
  if (usable.cards.empty()) {
    // GUI-first start-up (spec 2026-10-27 §3): no card yet - the server's rows from the main config, and the page adds
    // the first card (from a download or a folder). Without models.default_engine there is nothing to serve.
    if (main.entries.contains("models.default_engine")) {
      auto card_less = secure_frontend::card_less_server_config(main);
      if (auto* config = std::get_if<secure_frontend::ValidatedConfig>(&card_less)) {
        usable.server = config->shared();
        return usable;
      }
      for (const auto& issue : std::get<secure_frontend::ConfigErrors>(card_less)) {
        std::cerr << "Configuration error: " << secure_frontend::format_issue(issue) << '\n';
      }
      return std::nullopt;
    }
    std::cerr << "models.cards: no model card is usable; refusing to serve.\n";
    return std::nullopt;
  }
  return usable;
}

int launcher_check_impl(const secure_frontend::FrontendConfigDraft& main, const std::vector<std::string>& overridden) {
  const auto usable = usable_cards(main);
  if (!usable) return EXIT_FAILURE;
  const FrontendConfig& config = *usable->server;
  std::cout << secure_frontend::describe(config);
  for (const auto& key : overridden) std::cout << key << ": the command line overrides the config file\n";
  for (const auto& card : usable->cards) {
    std::cout << "model card " << card.name << ": usable (" << secure_frontend::profile_name(card.profile) << ", "
              << card.options.size() << " page options)\n";
  }
  std::cout.flush();
  if (config.listener.loopback_without_key) {
    std::cout << "token: not used (listener.loopback_without_key: loopback only)\nConfiguration is valid.\n";
    return EXIT_SUCCESS;
  }
  const auto token = read_configured_token(config);
  if (const auto* error = std::get_if<secure_frontend::TokenReadError>(&token)) {
    std::cerr << "token: unavailable: " << error->message << '\n'
              << "Configuration is not usable: the API token cannot be read.\n";
    return EXIT_FAILURE;
  }
  std::cout << "token: available (value not shown)\nConfiguration is valid.\n";
  return EXIT_SUCCESS;
}

secure_frontend::HttpResponse json_answer(const int status, std::string body) {
  return {.status = status, .headers = {{"Content-Type", "application/json; charset=utf-8"}}, .body = std::move(body)};
}

secure_frontend::HttpResponse refusal_answer(const secure_frontend::ModelLauncher::Refusal& refusal) {
  return secure_frontend::make_error_response(refusal.error, {.param = refusal.param});
}

// POST /v1/strata/models/start: {"name": "<card>", "options": {...}, "calibrate": false} (options optional; calibrate
// false: start without calibrating first, owner 2026-10-29).
secure_frontend::HttpResponse start_answer(secure_frontend::ModelLauncher& launcher, const std::string_view body, const bool remote) {
  constexpr secure_frontend::JsonLimits kStartLimits{.max_bytes = secure_frontend::kModelControlBodyLimit, .max_depth = 3U,
                                                     .max_container_entries = 64U, .max_string_bytes = 256U, .allow_fractions = true};
  const auto parsed = secure_frontend::parse_json(body, kStartLimits);
  const auto* root = std::get_if<secure_frontend::JsonValue>(&parsed);
  if (root == nullptr) return secure_frontend::make_error_response(secure_frontend::ApiError::invalid_json);
  const auto* members = root->if_object();
  if (members == nullptr) return secure_frontend::make_error_response(secure_frontend::ApiError::invalid_request);
  const std::string* name = nullptr;
  const secure_frontend::JsonValue* options = nullptr;
  bool may_calibrate = true;  // "calibrate": false - the page's "Start without calibrating" (owner 2026-10-29)
  for (const auto& member : *members) {
    if (member.key == "name" && member.value.if_string() != nullptr) name = member.value.if_string();
    else if (member.key == "options" && member.value.if_object() != nullptr) options = &member.value;
    else if (member.key == "calibrate" && member.value.if_boolean() != nullptr) may_calibrate = *member.value.if_boolean();
    else return secure_frontend::make_error_response(secure_frontend::ApiError::invalid_request);
  }
  if (name == nullptr) return secure_frontend::make_error_response(secure_frontend::ApiError::invalid_request);
  if (const auto refused = launcher.start(*name, options, remote, may_calibrate)) return refusal_answer(*refused);
  std::string text = "{\"state\":\"starting\",\"name\":";
  secure_frontend::append_json_string(&text, *name);
  return json_answer(202, text + "}\n");
}

// GUI-first start-up (spec 2026-10-27 §5.3): POST /v1/strata/models/calibrate {"name", "options"?, "then_start"?,
// "auto_recalibrate"?}.
secure_frontend::HttpResponse calibrate_answer(secure_frontend::ModelLauncher& launcher, const std::string_view body, const bool remote) {
  constexpr secure_frontend::JsonLimits kLimits{.max_bytes = secure_frontend::kModelControlBodyLimit, .max_depth = 3U,
                                                .max_container_entries = 64U, .max_string_bytes = 256U, .allow_fractions = true};
  const auto parsed = secure_frontend::parse_json(body, kLimits);
  const auto* root = std::get_if<secure_frontend::JsonValue>(&parsed);
  if (root == nullptr) return secure_frontend::make_error_response(secure_frontend::ApiError::invalid_json);
  const auto* members = root->if_object();
  if (members == nullptr) return secure_frontend::make_error_response(secure_frontend::ApiError::invalid_request);
  const std::string* name = nullptr;
  const secure_frontend::JsonValue* options = nullptr;
  bool then_start = false;
  std::optional<bool> automatic;
  for (const auto& member : *members) {
    if (member.key == "name" && member.value.if_string() != nullptr) name = member.value.if_string();
    else if (member.key == "options" && member.value.if_object() != nullptr) options = &member.value;
    else if (member.key == "then_start" && member.value.if_boolean() != nullptr) then_start = *member.value.if_boolean();
    else if (member.key == "auto_recalibrate" && member.value.if_boolean() != nullptr) automatic = *member.value.if_boolean();
    else return secure_frontend::make_error_response(secure_frontend::ApiError::invalid_request);
  }
  if (name == nullptr) return secure_frontend::make_error_response(secure_frontend::ApiError::invalid_request);
  if (const auto refused = launcher.calibrate(*name, options, then_start, automatic, remote)) return refusal_answer(*refused);
  std::string text = "{\"state\":\"calibrating\",\"name\":";
  secure_frontend::append_json_string(&text, *name);
  return json_answer(202, text + "}\n");
}

// POST /v1/strata/models/calibrate/auto {"name", "on"}.
secure_frontend::HttpResponse auto_recalibrate_answer(secure_frontend::ModelLauncher& launcher, const std::string_view body) {
  constexpr secure_frontend::JsonLimits kLimits{.max_bytes = secure_frontend::kModelControlBodyLimit, .max_depth = 2U,
                                                .max_container_entries = 4U, .max_string_bytes = 256U};
  const auto parsed = secure_frontend::parse_json(body, kLimits);
  const auto* root = std::get_if<secure_frontend::JsonValue>(&parsed);
  const auto* members = root == nullptr ? nullptr : root->if_object();
  const std::string* name = nullptr;
  std::optional<bool> on;
  if (members != nullptr) {
    for (const auto& member : *members) {
      if (member.key == "name" && member.value.if_string() != nullptr) name = member.value.if_string();
      else if (member.key == "on" && member.value.if_boolean() != nullptr) on = *member.value.if_boolean();
      else return secure_frontend::make_error_response(secure_frontend::ApiError::invalid_request);
    }
  }
  if (name == nullptr || !on.has_value()) return secure_frontend::make_error_response(secure_frontend::ApiError::invalid_request);
  if (const auto refused = launcher.set_auto_recalibrate(*name, *on)) return refusal_answer(*refused);
  return json_answer(200, std::string{"{\"auto_recalibrate\":"} + (*on ? "true" : "false") + "}\n");
}

// A calibration's report for a bug report (owner 2026-10-30): POST /v1/strata/models/calibrate/report {"name"} ->
// {"name", "file", "date", "report"} (its Markdown); not_found when the card has none since this server started.
secure_frontend::HttpResponse calibration_report_answer(const secure_frontend::ModelLauncher& launcher, const std::string_view body) {
  constexpr secure_frontend::JsonLimits kLimits{.max_bytes = secure_frontend::kModelControlBodyLimit, .max_depth = 2U,
                                                .max_container_entries = 4U, .max_string_bytes = 256U};
  const auto parsed = secure_frontend::parse_json(body, kLimits);
  const auto* root = std::get_if<secure_frontend::JsonValue>(&parsed);
  const auto* members = root == nullptr ? nullptr : root->if_object();
  const std::string* name = nullptr;
  if (members != nullptr) {
    for (const auto& member : *members) {
      if (member.key == "name" && member.value.if_string() != nullptr) name = member.value.if_string();
      else return secure_frontend::make_error_response(secure_frontend::ApiError::invalid_request);
    }
  }
  if (name == nullptr) return secure_frontend::make_error_response(secure_frontend::ApiError::invalid_request);
  const auto report = launcher.calibration_report(*name);
  if (!report.has_value()) return secure_frontend::make_error_response(secure_frontend::ApiError::not_found);
  std::string text = "{\"name\":";
  secure_frontend::append_json_string(&text, *name);
  text += ",\"file\":";
  secure_frontend::append_json_string(&text, report->file);
  text += ",\"date\":";
  secure_frontend::append_json_string(&text, report->date);
  text += ",\"report\":";
  secure_frontend::append_json_string(&text, report->text);
  return json_answer(200, text + "}\n");
}

// Model downloads (plan 2026-10-23-model-downloads Task 4): the cards prepared downloads wrote
// (<roots.state>/cards/*.json, by file name), each read and validated like a listed card; one
// whose name is already held is refused by name.
void load_download_cards(const secure_frontend::FrontendConfigDraft& main, const std::filesystem::path& directory,
                         std::vector<secure_frontend::ModelCard>& cards) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec) && it->path().extension() == ".json") files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());
  for (const auto& file : files) {
    const std::string shown = secure_frontend::path_to_utf8(file.filename());
    if (cards.size() >= secure_frontend::kMaxLauncherCards) {
      std::cerr << "download card " << shown << " refused: the launcher holds " << secure_frontend::kMaxLauncherCards << " cards\n";
      break;
    }
    auto read = secure_frontend::read_model_card(file);
    if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&read)) {
      for (const auto& issue : *issues) std::cerr << "download card " << shown << " refused: " << secure_frontend::format_issue(issue) << '\n';
      continue;
    }
    auto& card = std::get<secure_frontend::ModelCard>(read);
    if (std::any_of(cards.begin(), cards.end(), [&card](const secure_frontend::ModelCard& held) { return held.name == card.name; })) {
      std::cerr << "download card " << shown << " (" << card.name << ") refused: a card of that name is already loaded\n";
      continue;
    }
    if (usable_card_config(main, card) == nullptr) continue;
    cards.push_back(std::move(card));
  }
}

// What a prepared Qwen download's card takes from models.download_engine_card: its engine.
std::optional<secure_frontend::DownloadCardTemplate> download_card_template(const FrontendConfig& config,
                                                                            const std::vector<secure_frontend::ModelCard>& cards) {
  if (!config.models.download_engine_card.has_value()) return std::nullopt;
  for (const auto& card : cards) {
    if (card.name != *config.models.download_engine_card) continue;
    const auto& entries = card.config.entries;
    const auto text = [&entries](const std::string_view key) -> const std::string* {
      const auto found = entries.find(std::string{key});
      return found == entries.end() ? nullptr : std::get_if<std::string>(&found->second.value);
    };
    const auto* executable = text("engine.executable");
    if (card.profile != secure_frontend::EngineProfile::qwen || executable == nullptr) break;
    secure_frontend::DownloadCardTemplate made;
    made.engine_executable = *executable;
    if (const auto found = entries.find("engine.lib_dirs"); found != entries.end()) {
      if (const auto* dirs = std::get_if<std::vector<std::string>>(&found->second.value)) made.engine_lib_dirs = *dirs;
    }
    if (const auto* gpu = text("engine.gpu")) made.engine_gpu = *gpu;
    return made;
  }
  std::cerr << "[strata] downloads: models.download_engine_card names no usable qwen card; a prepared download gets no card\n";
  return std::nullopt;
}


int launcher_run_impl(const secure_frontend::FrontendConfigDraft& main, const std::optional<std::filesystem::path>& config_file) {
  // Ruling T13-c: server-grade process defaults (idempotent).
  secure_frontend::install_process_defaults();
  auto usable = usable_cards(main);
  if (!usable) return EXIT_FAILURE;
  const std::shared_ptr<const FrontendConfig> server_config = usable->server;
  const FrontendConfig& config = *server_config;
  if (!state_dir_is_usable(config)) return EXIT_FAILURE;
  if (const auto issues = secure_frontend::create_runtime_roots(config); !issues.empty()) {
    for (const auto& issue : issues) std::cerr << "Configuration error: " << secure_frontend::format_issue(issue) << '\n';
    return EXIT_FAILURE;
  }
  const bool keyless = config.listener.loopback_without_key;
  secure_frontend::BearerTokenValidator validator;
  // GUI-first start-up (spec 2026-10-27 §3, D1): the one-time login link, and the copy of the key it hands out once.
  std::optional<secure_frontend::LoginLinks> login_links;
  std::shared_ptr<secure_frontend::SecretBytes> login_key;
  if (!keyless) {
    auto token_result = read_configured_token(config);
    auto* token = std::get_if<secure_frontend::SecretBytes>(&token_result);
    if (token == nullptr) {
      std::cerr << "Unable to read API token: " << std::get<secure_frontend::TokenReadError>(token_result).message << '\n';
      return EXIT_FAILURE;
    }
    if (config.listener.login_link) {
      login_key = std::make_shared<secure_frontend::SecretBytes>();
      if (!login_key->assign(token->view())) {
        std::cerr << "listener.login_link: the key could not be held for the login link\n";
        return EXIT_FAILURE;
      }
      login_links.emplace();
    }
    validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(*token)));
  }
  secure_frontend::ChatHandler login_link_handler;
  if (login_links.has_value()) {
    // A loopback client with the current one-time code gets the key, once; anything else learns nothing.
    login_link_handler = [&links = *login_links, login_key](const std::string_view body,
                                                            const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
      if (!context.loopback_peer) return secure_frontend::make_error_response(secure_frontend::ApiError::control_not_allowed);
      constexpr secure_frontend::JsonLimits kLimits{.max_bytes = secure_frontend::kLoginLinkBodyLimit, .max_depth = 2U,
                                                    .max_container_entries = 4U, .max_string_bytes = 128U};
      const auto parsed = secure_frontend::parse_json(body, kLimits);
      const auto* root = std::get_if<secure_frontend::JsonValue>(&parsed);
      const auto* members = root != nullptr ? root->if_object() : nullptr;
      const std::string* code = nullptr;
      if (members != nullptr) {
        for (const auto& member : *members) {
          if (member.key == "code") code = member.value.if_string();
        }
      }
      if (code == nullptr) return secure_frontend::make_error_response(secure_frontend::ApiError::invalid_request);
      if (!links.claim(*code, secure_frontend::LoginLinks::Clock::now())) {
        return secure_frontend::make_error_response(secure_frontend::ApiError::login_link_refused);
      }
      std::string answer = "{\"key\":";
      secure_frontend::append_json_string(&answer, login_key->view());
      answer += "}\n";
      auto response = json_answer(200, std::move(answer));
      response.headers.emplace_back("Cache-Control", "no-store");
      return response;
    };
  }
  const secure_frontend::Config listener = secure_frontend::listener_config(config);
  const std::string authority = host_header(listener);  // the startup line only
  std::optional<secure_frontend::ServeMonitor> monitor;
  if (config.listener.monitor) monitor.emplace();
  // Model downloads: the store and the downloads' own folders, owner-only; the cards prepared
  // downloads wrote load beside the listed ones.
  const std::filesystem::path downloads_dir = *config.roots.state / "downloads";
  const std::filesystem::path download_cards_dir = *config.roots.state / "cards";
  // The operator's listed cards come first (read-only from the page); the cards the server wrote follow.
  const std::size_t operator_cards = usable->cards.size();
  if (config.models.control && !config.models.downloads) {
    // GUI-first start-up (spec 2026-10-27 §4.1): the cards the page wrote, owner-only beside the engine's state.
    if (const auto opened = secure_frontend::open_root(download_cards_dir, secure_frontend::RootPolicy::create_private);
        const auto* failure = std::get_if<secure_frontend::PathError>(&opened)) {
      std::cerr << "models.control: " << secure_frontend::path_to_utf8(download_cards_dir) << ": " << failure->message << '\n';
      return EXIT_FAILURE;
    }
    load_download_cards(main, download_cards_dir, usable->cards);
  }
  if (config.models.downloads) {
    if (!config_file.has_value() || !config.models.downloads_root.has_value()) {
      std::cerr << "models.downloads: needs the config file (--config) and models.downloads_root\n";
      return EXIT_FAILURE;
    }
    for (const auto& directory : {*config.models.downloads_root, downloads_dir, downloads_dir / "tmp", download_cards_dir}) {
      if (const auto opened = secure_frontend::open_root(directory, secure_frontend::RootPolicy::create_private);
          const auto* failure = std::get_if<secure_frontend::PathError>(&opened)) {
        std::cerr << "models.downloads: " << secure_frontend::path_to_utf8(directory) << ": " << failure->message << '\n';
        return EXIT_FAILURE;
      }
    }
    load_download_cards(main, download_cards_dir, usable->cards);
  }
  const std::size_t card_count = usable->cards.size();
  std::optional<secure_frontend::DownloadCardTemplate> card_template;
  if (config.models.downloads) card_template = download_card_template(config, usable->cards);
  // D7: the page's last options per card, owner-only beside the engine's state.
  secure_frontend::LauncherSettings launcher_settings{
      .main = main,
      .cards = std::move(usable->cards),
      .operator_cards = operator_cards,
      .memory_file = *config.roots.state / "model-options.json",
      .gate = {.max_queued = config.limits.concurrency.max_queued_generations, .queue_timeout = config.limits.concurrency.queue_timeout},
      .monitor = monitor.has_value() ? &*monitor : nullptr};
  // GUI-first start-up (spec 2026-10-27 §5a): email alerts, in a build that has them and when they are on.
#if defined(SECURE_FRONTEND_EMAIL_ALERTS)
  std::optional<secure_frontend::EmailAlerts> alerts;
  if (config.alerts.email.enabled && config.roots.secret.has_value()) {
    alerts.emplace(secure_frontend::AlertSettings{.config = config.alerts.email, .secret_dir = config.roots.secret->path(),
                                                  .server_name = secure_frontend::server_display_name()});
  }
  const auto raise_alert = [&alerts](const std::string_view event, const std::string& subject, const std::string& body) {
    if (alerts.has_value()) (void)alerts->raise(event, subject, body);
  };
  launcher_settings.on_event = raise_alert;
#endif
  // Web search (spec 2026-10-27-web-search-design §2): the server's tool for every started model, when it is on.
#if defined(SECURE_FRONTEND_WEB_SEARCH)
  std::optional<secure_frontend::SearchService> search;  // outlives the launcher
  if (config.search.enabled) {
    const auto settings = search_settings(config);
    if (!settings.has_value()) return EXIT_FAILURE;
    search.emplace(*settings);
    launcher_settings.tools = &*search;
    launcher_settings.tool_rounds = config.search.max_rounds;
  }
#endif
  // GUI-first start-up (spec 2026-10-27 §5): each card's calibration, owner-only beside the engine's state; the GPU
  // for its key from the Monitor's readings (unknown with the Monitor off).
  if (config.models.control) {
    launcher_settings.calibration_dir = *config.roots.state / "calibration";
    if (monitor.has_value()) launcher_settings.gpu_fact = [&monitor] { return monitor->gpu_fact(); };
    // A calibration's report for a bug report (owner 2026-10-30): <roots.logs>/calibration-reports, with this PC's
    // hardware read when one is made (nvidia-smi as the Monitor runs it) and the engine folder's build record.
    if (config.roots.logs.has_value()) launcher_settings.report_dir = *config.roots.logs / "calibration-reports";
    launcher_settings.report_machine = [engine = config.models.default_engine.value_or(config.engine.executable.path()), &monitor] {
      secure_frontend::ReportMachine machine{secure_frontend::hardware_facts(), secure_frontend::physical_cores(),
                                             secure_frontend::read_memory(), {}, secure_frontend::read_engine_build(engine.parent_path())};
      if (monitor.has_value()) machine.hardware.gpu = monitor->gpu_fact();
      machine.gpus = secure_frontend::report_gpus({.executable = secure_frontend::nvidia_smi_path(),
                                                   .arguments = secure_frontend::nvidia_smi_report_arguments(),
                                                   .environment = secure_frontend::nvidia_smi_environment(),
                                                   .no_new_privileges = true, .insert_serve = false});
      return machine;
    };
  }
  secure_frontend::ModelLauncher launcher(std::move(launcher_settings));
  // D2: model control for key holders on loopback; remote clients only with models.control_remote, and only from
  // models.control_remote_networks when it lists any (owner 2026-10-27).
  const bool remote_allowed = config.models.control_remote;
  const std::vector<secure_frontend::IpNetwork> control_networks = config.models.control_remote_networks;
  const auto allowed = [remote_allowed, control_networks](const secure_frontend::RequestContext& context) {
    return secure_frontend::model_control_allowed(context.loopback_peer, context.peer, remote_allowed, control_networks);
  };
  // The page's model files and its settings (owner 2026-10-27): this PC, and the hosts and subnets
  // models.control_remote_networks lists - never through models.control_remote alone.
  const auto files_allowed = [remote_allowed, control_networks](const secure_frontend::RequestContext& context) {
    return secure_frontend::model_files_allowed(context.loopback_peer, context.peer, remote_allowed, control_networks);
  };
  secure_frontend::ModelControl control;
  if (config.models.control) {
    control.list = [&launcher, allowed](const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
      if (!allowed(context)) return secure_frontend::make_error_response(secure_frontend::ApiError::control_not_allowed);
      return json_answer(200, launcher.list_json());
    };
    control.start = [&launcher, allowed](const std::string_view body,
                                         const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
      if (!allowed(context)) return secure_frontend::make_error_response(secure_frontend::ApiError::control_not_allowed);
      return start_answer(launcher, body, !context.loopback_peer);
    };
    control.stop = [&launcher, allowed](const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
      if (!allowed(context)) return secure_frontend::make_error_response(secure_frontend::ApiError::control_not_allowed);
      if (const auto refused = launcher.stop(!context.loopback_peer)) return refusal_answer(*refused);
      return json_answer(202, "{\"state\":\"stopped\"}\n");
    };
    // GUI-first start-up (spec 2026-10-27 §5.3): calibrate, cancel, and the "recalibrate when settings change" switch.
    control.calibrate = [&launcher, allowed](const std::string_view body,
                                             const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
      if (!allowed(context)) return secure_frontend::make_error_response(secure_frontend::ApiError::control_not_allowed);
      return calibrate_answer(launcher, body, !context.loopback_peer);
    };
    control.cancel_calibration = [&launcher, allowed](const std::string_view,
                                                      const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
      if (!allowed(context)) return secure_frontend::make_error_response(secure_frontend::ApiError::control_not_allowed);
      if (const auto refused = launcher.cancel_calibration()) return refusal_answer(*refused);
      return json_answer(202, "{\"state\":\"cancelling\"}\n");
    };
    control.auto_recalibrate = [&launcher, allowed](const std::string_view body,
                                                    const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
      if (!allowed(context)) return secure_frontend::make_error_response(secure_frontend::ApiError::control_not_allowed);
      return auto_recalibrate_answer(launcher, body);
    };
    control.calibration_report = [&launcher, allowed](const std::string_view body,
                                                      const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
      if (!allowed(context)) return secure_frontend::make_error_response(secure_frontend::ApiError::control_not_allowed);
      return calibration_report_answer(launcher, body);
    };
  }
  // Model downloads (O1-O4): every transfer and link lookup a `strata` child, the prepare step
  // the embedded script under the configured Python; a prepared card joins the launcher.
  std::optional<secure_frontend::DownloadManager> downloads;
  secure_frontend::ModelDownloads download_routes;
  if (config.models.downloads) {
    secure_frontend::DownloadSettings settings;
    settings.catalog = secure_frontend::parse_download_catalog(secure_frontend::embedded_download_catalog());
    settings.strata_executable = secure_frontend::strata_beside_this_executable();
    std::error_code ec;
    settings.config_file = std::filesystem::absolute(*config_file, ec);
    settings.downloads_root = *config.models.downloads_root;
    settings.work_dir = downloads_dir;
    settings.cards_dir = download_cards_dir;
    settings.prepare_python = config.models.prepare_python;
    settings.prepare_tools = config.models.prepare_tools;
    if (config.models.prepare_python.has_value()) {
      auto root = secure_frontend::open_root(downloads_dir);
      const auto script = secure_frontend::parse_relative_path("prepare_download.py");
      if (std::holds_alternative<secure_frontend::CanonicalRoot>(root) && std::holds_alternative<secure_frontend::RelativePath>(script) &&
          !secure_frontend::write_private_file(std::get<secure_frontend::CanonicalRoot>(root), std::get<secure_frontend::RelativePath>(script),
                                               secure_frontend::embedded_prepare_script(), secure_frontend::TokenStoreMode::replace)) {
        settings.prepare_script = downloads_dir / "prepare_download.py";
      } else {
        std::cerr << "[strata] downloads: the prepare script could not be written; Qwen downloads stay files only\n";
      }
    }
    settings.card_template = card_template;
    // GUI-first start-up (spec 2026-10-27 §4.3): a downloaded Gemma 4 or flint GGUF becomes a card on this engine.
    settings.gguf_engine = config.models.default_engine;
    settings.child_environment = download_environment(downloads_dir / "tmp");
    settings.on_card = [&launcher, &main](const std::string&, const std::filesystem::path& file) {
      auto read = secure_frontend::read_model_card(file);
      if (const auto* issues = std::get_if<secure_frontend::ConfigErrors>(&read)) {
        for (const auto& issue : *issues) std::cerr << "download card refused: " << secure_frontend::format_issue(issue) << '\n';
        return false;
      }
      auto& card = std::get<secure_frontend::ModelCard>(read);
      if (usable_card_config(main, card) == nullptr) return false;
      return launcher.add_card(std::move(card));
    };
    // owner 2026-10-28: the list says which entries are already on this PC (a card from the download, or one from a folder)
    settings.cards = [&launcher] { return launcher.cards(); };
    settings.log = [](const std::string_view line) { std::cerr << line << '\n' << std::flush; };
#if defined(SECURE_FRONTEND_EMAIL_ALERTS)
    settings.on_event = raise_alert;
#endif
    settings.seams = secure_frontend::production_download_seams();
    downloads.emplace(std::move(settings));
    download_routes.handle = [&manager = *downloads, allowed](const secure_frontend::RouteId route, const std::string_view body,
                                                             const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
      if (!allowed(context)) return secure_frontend::make_error_response(secure_frontend::ApiError::control_not_allowed);
      return secure_frontend::download_route_answer(manager, route, body);
    };
  }
  // GUI-first start-up (spec 2026-10-27 §4): the page's cards and folders, with model control (files_allowed's clients).
  std::optional<secure_frontend::CardRoutes> card_routes;
  secure_frontend::ModelCardRoutes card_route_handler;
  if (config.models.control) {
    secure_frontend::CardRouteSettings settings{.cards_dir = download_cards_dir, .engine = config.models.default_engine, .main = main};
    if (!keyless) settings.key_matches = validator;
    settings.config_file = config_file;  // the Model folders setting is saved there (owner 2026-10-27)
    settings.client_allowed = files_allowed;
    if (downloads.has_value()) {
      // a deleted downloaded model leaves its download store too (§4.5)
      settings.remove_download = [&manager = *downloads](const std::string_view id) -> std::optional<std::string> {
        const auto refused = manager.remove_installed(id);
        if (!refused) return std::nullopt;
        return std::string{refused->code};
      };
    }
    // owner 2026-10-27: a Qwen3.8 or GLM-5.3 model added from its folder has its pack built by strata-prepare, beside the
    // engine for new cards; a Qwen3.8 pack goes in <roots.state>/packs
    if (config.models.default_engine.has_value()) {
#if defined(_WIN32)
      const auto tool = config.models.default_engine->parent_path() / "strata-prepare.exe";
#else
      const auto tool = config.models.default_engine->parent_path() / "strata-prepare";
#endif
      std::error_code ec;
      if (std::filesystem::is_regular_file(tool, ec)) {
        settings.packs_dir = *config.roots.state / "packs";
        settings.prepare = [tool, environment = download_environment(downloads_dir / "tmp"),
                            directory = *config.roots.state](const std::vector<std::string>& arguments) {
          secure_frontend::EngineCommand command;
          command.executable = tool;
          command.arguments = arguments;
          command.environment = environment;
          command.working_directory = directory;
          command.no_new_privileges = true;
          command.insert_serve = false;
          return secure_frontend::run_prepare_command(secure_frontend::production_download_seams().spawn, command, {});
        };
      }
    }
    card_routes.emplace(launcher, std::move(settings));
    card_route_handler.handle = [&routes = *card_routes](const secure_frontend::RouteId route, const std::string_view body,
                                                         const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
      return routes.answer(route, body, context);
    };
  }
  // GUI-first start-up (spec 2026-10-27 §5a): the page's alert settings, with model control (files_allowed's clients).
  secure_frontend::ModelCardRoutes alert_route_handler;
#if defined(SECURE_FRONTEND_EMAIL_ALERTS)
  std::optional<secure_frontend::AlertRoutes> alert_routes;
  if (config.models.control && config.roots.secret.has_value()) {
    alert_routes.emplace(secure_frontend::AlertRouteSettings{.config_file = config_file, .secret_dir = config.roots.secret->path(),
                                                             .server_name = secure_frontend::server_display_name(),
                                                             .client_allowed = files_allowed});
    alert_route_handler.handle = [&routes = *alert_routes](const secure_frontend::RouteId route, const std::string_view body,
                                                           const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
      return routes.answer(route, body, context);
    };
  }
#endif
  // The Settings tab (spec 2026-10-28-server-window-design, Part 1): the server's settings and the key, with model control
  // (files_allowed's clients; rotation this PC only). The key can be rotated only where this server reads it from its
  // store (a file: token.source), not a --token-env start.
  secure_frontend::ModelCardRoutes settings_route_handler;
  std::optional<secure_frontend::ConfigRoutes> config_routes;
  if (config.models.control) {
    secure_frontend::ConfigRouteSettings settings{.config_file = config_file, .client_allowed = files_allowed};
    if (config.token.kind() == secure_frontend::SecretSourceKind::protected_file && config.roots.secret.has_value()) {
      settings.secret_dir = config.roots.secret->path();
      settings.token_file = config.token.location();
    }
    settings.log = [](const std::string_view line) { std::cerr << line << '\n'; };
    config_routes.emplace(std::move(settings));
    settings_route_handler.handle = [&routes = *config_routes](const secure_frontend::RouteId route, const std::string_view body,
                                                               const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
      return routes.answer(route, body, context);
    };
  }
  // Web search (spec 2026-10-27-web-search-design §2): the page's search settings, with model control (files_allowed's clients).
  secure_frontend::ModelCardRoutes search_route_handler;
#if defined(SECURE_FRONTEND_WEB_SEARCH)
  std::optional<secure_frontend::SearchRoutes> search_routes;
  if (config.models.control) {
    if (const auto settings = search_settings(config)) {
      search_routes.emplace(secure_frontend::SearchRouteSettings{.config_file = config_file,
                                                                 .program = settings->program,
                                                                 .environment = settings->environment,
                                                                 .running = search.has_value(),
                                                                 .log = settings->log,
                                                                 .client_allowed = files_allowed});
      search_route_handler.handle = [&routes = *search_routes](const secure_frontend::RouteId route, const std::string_view body,
                                                               const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
        return routes.answer(route, body, context);
      };
    }
  }
#endif
  secure_frontend::ApiRouter router(secure_frontend::ApiRouterConfig{
      .policy = secure_frontend::make_http_policy(config.listener.host, config.listener.port, config.listener.host_aliases,
                                                  config.listener.cors_origins),
      .validator = validator,
      .model_id = {},
      .owned_by = "local",
      .aliases = {},
      .chat_body_limit = config.limits.chat.max_body_bytes,
      .health = [&launcher] { return launcher.health(); },
      .chat = [&launcher](const std::string_view body, const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
        return launcher.chat(body, context);
      },
      .web_ui = config.listener.web_ui,
      .require_key = !keyless,
      .metrics = monitor.has_value() ? secure_frontend::MetricsProvider{[&launcher] { return launcher.metrics_json(); }}
                                     : secure_frontend::MetricsProvider{},
      .about = about_provider(config.models.default_engine.value_or(config.engine.executable.path()), monitor),
      .served = [&launcher] { return launcher.served(); },
      .control = std::move(control),
      .downloads = std::move(download_routes),
      .cards = std::move(card_route_handler),
      .alerts = std::move(alert_route_handler),
      .search = std::move(search_route_handler),
      .settings = std::move(settings_route_handler),
      .login_link = std::move(login_link_handler)});
  const secure_frontend::ConnectionLimits connection_bounds = secure_frontend::connection_limits(config);
  std::string bind_error;
  auto server = secure_frontend::HttpSocketServer::bind(listener, static_cast<int>(config.listener.backlog), &bind_error);
  if (!server) { std::cerr << "Unable to bind secure listener.\n"; return EXIT_FAILURE; }
  // Spec §3.7: a signal sets the process-wide flag the accept loop below polls; the wake only
  // has to exist (no supervisor runs before a card starts). Released before it is destroyed.
  secure_frontend::SupervisorWake signal_wake;
  secure_frontend::install_shutdown_signals(signal_wake);
  struct ReleaseSignals final {
    secure_frontend::SupervisorWake& wake;
    ~ReleaseSignals() { secure_frontend::release_shutdown_signals(wake); }
  } release_signals{signal_wake};
  std::string remote_control = " (control: loopback and remote)";
  if (!control_networks.empty()) {
    remote_control = " (control: loopback and";
    for (std::size_t i = 0; i < control_networks.size(); ++i) {
      remote_control += (i == 0U ? " " : ", ") + secure_frontend::ip_network_text(control_networks[i]);
    }
    remote_control += ")";
  }
  std::cerr << "strata: listening " << authority << ", workers " << config.limits.concurrency.max_concurrent_requests
            << ", queued generations " << config.limits.concurrency.max_queued_generations << ", model cards " << card_count
            << (config.models.control ? (config.models.control_remote ? remote_control : std::string{" (control: loopback)"})
                                      : std::string{" (control off)"})
            << (config.models.downloads ? ", downloads on" : "") << '\n' << std::flush;
  HostSampler host_sampler;
  if (monitor.has_value()) host_sampler.start(*monitor, [&launcher] { return launcher.engine_sample(); });
  if (keyless) {
    std::cerr << "strata: warning: listener.loopback_without_key is true: the API and the page answer without a key on "
              << authority << " (loopback only; Host and Origin are still checked)\n";
  }
  warn_of_engine_output(config);
  warn_of_network_shares(config);
  // D5: models.start names the card started at boot (after bind, like every spawn).
  if (config.models.start.has_value()) {
    if (const auto refused = launcher.start(*config.models.start, nullptr, false)) {
      std::cerr << "[strata] models: models.start names no usable card (or it was refused); waiting for the page\n";
      (void)refused;
    }
  } else {
    std::cerr << "[strata] models: no model started (models.start is unset); waiting for the page\n";
  }
  std::cout << "Secure server listening on http://" << authority
            << (keyless ? " (no API key: loopback only; Ctrl-C to stop)\n" : " (authenticated; Ctrl-C to stop)\n");
#if defined(SECURE_FRONTEND_EMAIL_ALERTS)
  raise_alert("server_started", "the server started",
              "The Strata server is listening on " + authority + " with " + std::to_string(card_count) + " model cards.");
#endif
  if (login_links.has_value()) {
    // On the console only (never a log): the link works from this PC, once, within ten minutes.
    if (const auto code = login_links->issue(secure_frontend::LoginLinks::Clock::now())) {
      const std::string link_authority = config.listener.host.scope() == secure_frontend::AddressScope::loopback
                                             ? authority
                                             : "127.0.0.1:" + std::to_string(config.listener.port);
      std::cout << "Sign in: open " << secure_frontend::login_link_url(link_authority, *code)
                << " in a browser on this PC (one-time link, valid 10 minutes; restart for a new one)\n";
    }
  }
  std::cout.flush();
  secure_frontend::RequestLogger request_log(config.logging.requests, secure_frontend::stderr_sink());
  secure_frontend::PoolExit pool_exit = secure_frontend::PoolExit::stopped;
  {
    secure_frontend::ConnectionPool pool(
        *server,
        secure_frontend::PoolLimits{.workers = config.limits.concurrency.max_concurrent_requests,
                                    .per_remote_cap = config.limits.concurrency.max_connections_per_remote_address,
                                    .connection = connection_bounds},
        router,
        [&request_log](const secure_frontend::FinishedConnection& finished) { request_log.record(secure_frontend::log_record_for(finished)); });
    // The pool's shutdown hook ends the running model's generation like a shutdown (the
    // runtime's stop flag) and refuses its queue, before the workers are joined.
    pool_exit = pool.run_until([] { return secure_frontend::shutdown_signalled(); }, [&launcher] { launcher.begin_shutdown(); });
  }
  if (downloads) downloads->shutdown();  // a running transfer stops; its partial files are kept
  launcher.shutdown();
  secure_frontend::notify_shutdown_complete();
  if (pool_exit == secure_frontend::PoolExit::listener_failed) {
    std::cerr << "listener stopped\n";
    return EXIT_FAILURE;
  }
  // Spec C4: exit 0 means no engine is left; an unreaped one was logged.
  return launcher.engine_left_unreaped() ? EXIT_FAILURE : EXIT_SUCCESS;
}

}  // namespace

namespace secure_frontend {

// The exported entry points (the anonymous-namespace boot helpers above are
// file-local; only these two names leave this translation unit).
int server_run(const FrontendConfig& config, const bool expert) { return server_run_impl(config, expert); }
int server_check(const FrontendConfig& config, const std::vector<std::string>& overridden, bool expert) {
  return server_check_impl(config, overridden, expert);
}
int launcher_run(const FrontendConfigDraft& main, const std::optional<std::filesystem::path>& config_file) {
  return launcher_run_impl(main, config_file);
}
int launcher_check(const FrontendConfigDraft& main, const std::vector<std::string>& overridden) {
  return launcher_check_impl(main, overridden);
}

}  // namespace secure_frontend
