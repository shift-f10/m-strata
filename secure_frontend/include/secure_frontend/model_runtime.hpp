#pragma once

// The per-model half of the boot (model launcher plan 2026-10-23 Task 3): what one model needs
// before its engine spawns, composed from one validated config, and the serving objects one
// running model owns. server_bootstrap.cpp composes its one model through compose_model(); the
// launcher (model_launcher.hpp) composes one per start and keeps the running one in a
// ModelRuntime shared with every request in flight.

#include "secure_frontend/chat_route.hpp"
#include "secure_frontend/engine_supervisor.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/generation_gate.hpp"
#include "secure_frontend/launch_composition.hpp"
#include "secure_frontend/tokenizer_pack.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

struct ComposedModel final {
  ComposedLaunch launch;  // engine_args moved into plan; the vision fields stay for the boot
  TokenizerPack tokenizer;
  std::string model_id;              // model.name, else the launch's own id
  std::vector<std::string> aliases;  // model.aliases without the served name, deduplicated
  ChatRouteConfig chat;              // vision and monitor unset: the caller sets them
  SupervisorConfig supervisor;
  EngineLaunchPlan plan;             // the environment rendered; the state directory not yet prepared
};

// Why a model could not be composed. `messages` go to the server's log (key paths and fixed
// reasons); `reason` is the fixed text the page may show; `exit_code` is the boot's.
struct ComposeFailure final {
  std::vector<std::string> messages;
  std::string_view reason;
  int exit_code;
};

// compose_engine_launch, the tokenizer pack in the profile's mode, the served name and aliases,
// the effort-end tail, the supervisor config, the closed environment and the launch plan, and
// the chat route's settings — everything server_run used to do before it built the supervisor.
// Spawns nothing; notices (the mtp strip, the effort tail) go to stderr like before.
[[nodiscard]] std::variant<ComposedModel, ComposeFailure> compose_model(const FrontendConfig& config, bool expert);

// Creates <state>/engine, the temp root and the CUDA cache owner-only and clears the temp root
// (no engine runs yet, so what is there is stale). Run right before a model's engine spawns.
[[nodiscard]] bool prepare_state_dir(const std::filesystem::path& state_dir, const EngineEnvironment& environment);

// One running model: its tokenizer, supervisor, port, gate and chat route, built in place and
// never moved (the route holds references into it). Destroyed only when the last request in
// flight drops its shared_ptr, after the launcher shut the supervisor down.
class ModelRuntime final {
 public:
  ModelRuntime(ComposedModel model, ProcessFactory factory, GateLimits gate, ServeMonitor* monitor,
               std::shared_ptr<SupervisorClock> clock = std::make_shared<SteadySupervisorClock>());
  ModelRuntime(const ModelRuntime&) = delete;
  ModelRuntime& operator=(const ModelRuntime&) = delete;

  [[nodiscard]] const std::string& model_id() const noexcept { return model_id_; }
  [[nodiscard]] const std::vector<std::string>& aliases() const noexcept { return aliases_; }
  [[nodiscard]] EngineSupervisor& supervisor() noexcept { return supervisor_; }
  [[nodiscard]] SupervisorPort& port() noexcept { return port_; }
  [[nodiscard]] GenerationGate& gate() noexcept { return gate_; }
  [[nodiscard]] ChatRoute& chat() noexcept { return chat_; }
  // The flag a request's context points at instead of the pool's: set when the server shuts
  // down or when a switch cuts this model's generation (D3), it ends the generation the way a
  // shutdown does.
  [[nodiscard]] const std::atomic<bool>* stop_flag() const noexcept { return &stop_; }
  void raise_stop() noexcept { stop_.store(true); }
  // Set when a switch or stop begins retiring this model: a request it refuses or cuts then is
  // told the server is switching models, not shutting down.
  [[nodiscard]] bool retiring() const noexcept { return retiring_.load(); }
  void mark_retiring() noexcept { retiring_.store(true); }
  // Requests between the route's entry and the end of their response or stream.
  [[nodiscard]] std::size_t in_flight() const noexcept { return in_flight_.load(); }
  void enter() noexcept { in_flight_.fetch_add(1); }
  void leave() noexcept { in_flight_.fetch_sub(1); }

 private:
  std::string model_id_;
  std::vector<std::string> aliases_;
  TokenizerPack tokenizer_;
  EngineSupervisor supervisor_;
  SupervisorPort port_;
  GenerationGate gate_;
  ChatRoute chat_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> retiring_{false};
  std::atomic<std::size_t> in_flight_{0};
};

}  // namespace secure_frontend
