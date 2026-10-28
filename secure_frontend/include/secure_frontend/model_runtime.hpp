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
#include "secure_frontend/image_payload.hpp"
#include "secure_frontend/launch_composition.hpp"
#include "secure_frontend/tokenizer_pack.hpp"
#include "secure_frontend/vision_supervisor.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

// The picture encoder of a model with vision on (SP5 M1 Task 11's role; for cards, spec
// 2026-10-28-vision-for-cards-design.md): the program beside the engine - upstream's strata-vision, or maya-vision in an
// older kit - its arguments, its scratch folder (<state>/vision, owner-only: its working directory, where the request's
// pictures are staged) and its limits. Composed with the model; nothing spawns until the runtime starts it.
struct VisionPlan final {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  std::filesystem::path scratch;
  bool no_new_privileges{true};
  VisionLimits limits;
  std::int64_t max_side{2048};
  bool resident{true};  // vision.resident: started with the model (false: the first picture starts it)
};

struct ComposedModel final {
  ComposedLaunch launch;  // engine_args moved into plan; the vision fields stay for the boot
  TokenizerPack tokenizer;
  std::string model_id;              // model.name, else the launch's own id
  std::vector<std::string> aliases;  // model.aliases without the served name, deduplicated
  ChatRouteConfig chat;              // vision and monitor unset: the caller sets them
  SupervisorConfig supervisor;
  EngineLaunchPlan plan;             // the environment rendered; the state directory not yet prepared
  std::optional<VisionPlan> vision{};  // set when vision.enabled and the launch reads images (qwen only)
};

// The encoder's process for a plan: a direct child with no shell, an empty environment, the scratch folder as its working
// directory, the engine's privilege rule.
[[nodiscard]] ProcessFactory vision_process_factory(const VisionPlan& plan);

// The encoder role and the route's image service for a plan (the runtime's and the single-model boot's). Spawns nothing.
struct VisionRole final {
  std::unique_ptr<VisionSupervisor> supervisor;
  std::unique_ptr<VisionService> service;
};
[[nodiscard]] VisionRole make_vision_role(const VisionPlan& plan, ProcessFactory factory);

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
  // With model.vision, the encoder runs from `vision_factory` (empty: vision_process_factory) and the chat route reads
  // pictures through it; the caller starts it (start_vision) once the engine is up and stops it before the engine.
  ModelRuntime(ComposedModel model, ProcessFactory factory, GateLimits gate, ServeMonitor* monitor,
               std::shared_ptr<SupervisorClock> clock = std::make_shared<SteadySupervisorClock>(), ProcessFactory vision_factory = {});
  ModelRuntime(const ModelRuntime&) = delete;
  ModelRuntime& operator=(const ModelRuntime&) = delete;

  [[nodiscard]] const std::string& model_id() const noexcept { return model_id_; }
  [[nodiscard]] const std::vector<std::string>& aliases() const noexcept { return aliases_; }
  [[nodiscard]] EngineSupervisor& supervisor() noexcept { return supervisor_; }
  [[nodiscard]] SupervisorPort& port() noexcept { return port_; }
  [[nodiscard]] GenerationGate& gate() noexcept { return gate_; }
  [[nodiscard]] ChatRoute& chat() noexcept { return chat_; }
  // The picture encoder, or nullptr for a text-only model.
  [[nodiscard]] VisionSupervisor* vision() noexcept { return vision_.supervisor.get(); }
  // A resident encoder is started (true when it reached READY, or when there is none to start); a lazy one waits for
  // the first picture.
  [[nodiscard]] bool start_vision();
  void stop_vision() noexcept;
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
  bool vision_resident_{true};
  VisionRole vision_;  // before the route, which points at its service
  ChatRoute chat_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> retiring_{false};
  std::atomic<std::size_t> in_flight_{0};
};

}  // namespace secure_frontend
