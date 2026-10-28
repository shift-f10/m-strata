// The per-model half of the boot, moved out of server_bootstrap.cpp (model launcher plan
// 2026-10-23 Task 3) so the single-model boot and the launcher compose a model the same way.
// The comments of the moved code are kept with it.

#include "secure_frontend/model_runtime.hpp"

#include "secure_frontend/chat_execution.hpp"
#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/engine_arguments.hpp"
#include "secure_frontend/engine_environment.hpp"
#include "secure_frontend/acquire_reason.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/gguf_metadata.hpp"
#include "secure_frontend/subsystem_limits.hpp"
#include "secure_frontend/engine_stdio_process.hpp"
#include "secure_frontend/exit_status.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace secure_frontend {
namespace {

// The tokenizer in the header of the model file beneath roots.models (held open like every model file), under the
// pack loader's rules.
TokenizerPackLoadResult gguf_tokenizer(const CanonicalRoot& models, const std::optional<RelativePath>& file, const TokenizerMode mode) {
  if (!file.has_value()) return TokenizerPackError{TokenizerPackErrorCode::required_file_missing, "the launch names no model file"};
  auto opened = open_beneath(models, *file);
  if (auto* failure = std::get_if<PathError>(&opened)) {
    return TokenizerPackError{TokenizerPackErrorCode::file_read_failed, "the model file did not open: " + failure->message};
  }
  const auto header = read_gguf_metadata(std::get<OpenedFile>(opened));
  if (const auto* failure = std::get_if<GgufError>(&header)) return TokenizerPackError{TokenizerPackErrorCode::file_read_failed, failure->message};
  return load_tokenizer_from_gguf(std::get<GgufMetadata>(header), mode);
}

// The encoder of a launch that reads images (spec 2026-10-28-vision-for-cards-design.md; SP5 M1 Task 11): the program
// beside the engine - upstream's strata-vision, which the out folder builds from upstream/tools/vision, else an older kit's
// maya-vision - and the scratch folder its pictures are staged in. A failure is the log line the boot used to print.
std::variant<VisionPlan, std::string> plan_vision(const FrontendConfig& config, const ComposedLaunch& launch) {
  // Ruling T12-j: ENC/QUIT names ride protocol lines space-separated, and the GENI line names the combined file by
  // absolute path - a state path holding whitespace can never host the scratch dir.
  const std::filesystem::path& state_dir = *config.roots.state;
  if (state_dir.string().find_first_of(" \t") != std::string::npos) {
    return std::string{"vision: the state directory path must not contain spaces."};
  }
  // Ruling T12-c: image scratch is <state>/vision, owner-only, and IS the child's cwd.
  VisionPlan plan;
  plan.scratch = state_dir / "vision";
  if (!create_owner_only_directory(plan.scratch)) return std::string{"vision: unable to prepare the image scratch directory."};
  const auto beside = config.engine.executable.path().parent_path();
  bool upstream = true;
  for (const char* name : {"strata-vision", "maya-vision"}) {
    std::filesystem::path candidate = beside / name;
#if defined(_WIN32)
    candidate += L".exe";
#endif
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec)) {
      plan.executable = candidate;
      break;
    }
    upstream = false;
  }
  if (plan.executable.empty()) return std::string{"vision: no strata-vision beside the engine executable."};
  plan.arguments = compose_vision_arguments(VisionChildSpec{.mmproj = *launch.vision_mmproj, .model = *launch.vision_model,
                                                            .threads = config.vision.threads, .max_tokens = config.engine.max_tokens,
                                                            .no_flash_attn = config.vision.no_flash_attn, .gpu = config.vision.gpu,
                                                            .upstream_encoder = upstream});
  plan.no_new_privileges = config_defaults::kNoNewPrivilegesDefault && !config.engine.allow_new_privileges;
  // The STRATA_VISION_READY_S / _ENCODE_S counterparts (0 = wait for ever, the pin's :291 rule).
  plan.limits.policy.ready_timeout = std::chrono::milliseconds{std::chrono::seconds{config.vision.ready_timeout_s}};
  plan.limits.encode_timeout = std::chrono::milliseconds{std::chrono::seconds{config.vision.encode_timeout_s}};
  plan.max_side = config.vision.max_side;
  plan.resident = config.vision.resident;
  return plan;
}

}  // namespace

ProcessFactory vision_process_factory(const VisionPlan& plan) {
  // The engine's confinement family minus the per-generation cgroup delegation (recorded M1 gap); the environment stays
  // an empty allowlist and the cwd is the scratch dir (T11-ee); the pin's reader_args carry no --serve (T11-ff).
  return [exe = plan.executable, argv = plan.arguments, cwd = plan.scratch, nnp = plan.no_new_privileges](std::uint64_t) -> SpawnResult {
    auto child = std::make_unique<EngineStdioProcess>();
    EngineCommand command{.executable = exe, .arguments = argv, .environment = {}, .working_directory = cwd,
                          .no_new_privileges = nnp, .kill_exit_code = kVisionKillExitCode, .insert_serve = false};
    if (const auto error = child->start(command)) return SpawnFailure{error->message};
    return child;
  };
}

VisionRole make_vision_role(const VisionPlan& plan, ProcessFactory factory) {
  VisionRole role;
  role.supervisor = std::make_unique<VisionSupervisor>(std::move(factory), plan.limits);
  VisionService::Limits limits;
  limits.payload.max_side = plan.max_side;  // vision.max_side, the pre-decode probe (T12-c)
  limits.scratch_dir = plan.scratch;
  role.service = std::make_unique<VisionService>(*role.supervisor, std::move(limits));
  return role;
}

std::variant<ComposedModel, ComposeFailure> compose_model(const FrontendConfig& config, const bool expert) {
  // Derived once, before bind/spawn; each subsystem keeps its own const copy.
  auto schema_limits = chat_schema_limits(config);
  // SP4 §2.3/§4.9 (Task 24b): managed models launch from the installed manifest;
  // --expert keeps SP1's operator path (inventory, roots.tokenizer_pack).
  auto composed = compose_engine_launch(config, expert);
  if (auto* failure = std::get_if<LaunchFailure>(&composed)) {
    return ComposeFailure{std::move(failure->messages), "the engine launch could not be composed (see the server log)",
                          failure->exit == ExitCode::usage ? EXIT_FAILURE : exit_code_value(failure->exit)};
  }
  auto& launch = std::get<ComposedLaunch>(composed);
  // SP5 M1 Task 6 (parity row mtp; pin serve/server.py:2922-2925): an explicit "mtp":
  // false is applied at every start and the dropped flags are reported once, fixed text.
  if (!launch.mtp_removed.empty()) {
    std::string flags;
    for (const auto& flag : launch.mtp_removed) {
      if (!flags.empty()) flags += ' ';
      flags += flag;
    }
    std::cerr << "[strata] \"mtp\": false - starting without the draft layer (" << flags << " left out)\n";
  }
  // SP5 M1 Task 12 (C-9, Ruling T12-b): the content-array gate is a MODEL capability —
  // it opens only when vision.enabled AND the installed manifest declares launch.mmproj
  // (compose refuses that pairing for glm53, so the gate implies the qwen renderer).
  // The request itself can never open it.
  // Gemma 4 plan 1 Task 10: only the qwen profile reads images (gemma4 is text only, D2).
  if (config.vision.enabled && launch.vision_mmproj.has_value() && launch.profile == EngineProfile::qwen) {
    schema_limits.vision_capable = true;
  }
  // Session log step 14: per-request reasoning_effort reaches the qwen renderer; Gemma 4
  // plan 1 Task 3 (spec §6.6): and the gemma4 one (none = thinking off, else on). glm53 refuses it.
  schema_limits.per_request_reasoning_effort = per_request_reasoning_effort_for(launch.profile);
  // Gemma 4 plan 1 Task 4 (spec §6.2): the pack's mode is the launch profile's, never the files'.
  // GUI-first start-up (spec 2026-10-27 §4.2): a card with no pack reads it from the model file's header.
  const bool from_gguf = !launch.tokenizer_root.has_value();
  auto tokenizer_result = from_gguf ? gguf_tokenizer(config.roots.models, launch.tokenizer_gguf, tokenizer_mode_for(launch.profile))
                                    : load_local_tokenizer_pack(*launch.tokenizer_root, tokenizer_mode_for(launch.profile));
  if (auto* error = std::get_if<TokenizerPackError>(&tokenizer_result)) {
    if (from_gguf) {
      return ComposeFailure{{"Unable to read the tokenizer from the model file's header (roots.models): " + error->message},
                            "the tokenizer could not be read from the model file (see the server log)", EXIT_FAILURE};
    }
    return ComposeFailure{{std::string{"Unable to load local tokenizer pack ("} +
                           (launch.managed ? "the model's tokenizer" : "roots.tokenizer_pack") + "): " + error->message},
                          "the tokenizer pack could not be loaded (see the server log)", EXIT_FAILURE};
  }
  auto& tokenizer = std::get<TokenizerPack>(tokenizer_result);
  // model.name (session log step 13; the pin's model_name): the name the API serves; the
  // launch's own id (the inventory file, or name:tag) when it is unset.
  std::string model_id = config.engine.served_name.value_or(launch.model_id);
  // SP5 M1 Task 8 (model.aliases; set_aliases :3774-3777): drop an alias equal to the
  // served name, keep order, dedup — the route and /v1/models share one effective list.
  std::vector<std::string> model_aliases;
  for (const auto& alias : config.engine.aliases) {
    if (alias != model_id && std::find(model_aliases.begin(), model_aliases.end(), alias) == model_aliases.end())
      model_aliases.push_back(alias);
  }
  const bool glm = engine_family_for(launch.profile) == EngineFamily::glm;
  auto* engine_arguments = &launch.engine_args;
  // SP5 M1 Task 5 (parity row effort_position, serve/server.py:2836-2856): reasoning.effort_position
  // "end" composes the engine's --tail-role-token checkpoint flag (probe rules in the composer).
  // Session log step 16: the prompt puts a non-default level's sentence right before the
  // answer only when the engine took the flag; otherwise it stays at the top (the notice says so).
  // Gemma 4 plan 1 Task 3: the tail is the qwen engine's only (the config refuses
  // reasoning.effort_position on an explicit gemma4 profile; glm53's runner has no such flag).
  bool effort_at_end = false;
  if (launch.profile == EngineProfile::qwen && config.reasoning.effort_position == EffortPosition::end) {
    const auto tail = compose_effort_end_arguments(config.engine.executable.path(), tokenizer);
    std::cerr << "[strata] " << tail.notice << '\n';
    engine_arguments->insert(engine_arguments->end(), tail.arguments.begin(), tail.arguments.end());
    effort_at_end = !tail.arguments.empty();
  }
  QwenEffort qwen_effort = QwenEffort::high;  // unset: the template's default
  if (config.reasoning.effort == ReasoningEffort::low) qwen_effort = QwenEffort::low;
  if (config.reasoning.effort == ReasoningEffort::medium) qwen_effort = QwenEffort::medium;

  // Profile, protocol flavor, chat limits and the READY deadline all come from
  // the validated config (limits.engine.ready_timeout_s drives the policy).
  SupervisorConfig supervisor = supervisor_config(config);
  // SP4 §4.9 c: the profile is the composed launch's (a managed model's
  // manifest sets it; engine.profile is --expert only), not the config key.
  supervisor.profile = native_chat_profile_for(launch.profile);
  // SP5 M2 Task 7 (R-T5-b): maya_118 is the glm53 dialect (Task 5), same selection as
  // supervisor_config in subsystem_limits.cpp (the same function, pinned by its test).
  supervisor.flavor = protocol_flavor_for(launch.profile);
  const std::filesystem::path& state_dir = *config.roots.state;
  std::vector<std::filesystem::path> lib_dirs = config.engine.lib_dirs;
  if (lib_dirs.empty()) lib_dirs.push_back(config.engine.executable.path().parent_path());
  EngineLaunchPlan plan{.command = {.executable = config.engine.executable.path(), .arguments = std::move(*engine_arguments),
                                    .environment = {}}};
  // Spec §3.9: the engine's whole environment, rendered per generation; nothing
  // is inherited from this server's environment.
  auto environment = engine_environment_for(state_dir, config.roots.cache, std::move(lib_dirs));
  // SP5 M2 Tasks 4/8 (D-7, D-8): a composed glm53 launch is a GLM-family child —
  // the closed typed environment (the §4.9 safety defaults, the projected typed
  // rows, the CUDA selectors) replaces the strata rendering, and the vision-turn
  // gate decides whether the VLEND-family row may reach the child at all.
  if (glm) {
    environment.family = EngineFamily::glm;
    environment.glm_gpus = config.glm.gpus;
    environment.glm_knobs = glm_environment_knobs(config);
    environment.glm_vision_turn = config.vision.enabled && !config.vision.resident;
  }
  plan.environment = std::move(environment);
  // Spec §3.12: the engine's cwd-relative files (`pack/full`, stall dumps)
  // land in <state_dir>/engine, created by prepare_state_dir() before the spawn.
  plan.command.working_directory = state_dir / "engine";
  // Spec §3.7(d): detected, never configured; off unless this process runs in
  // a writable delegated cgroup v2.
  plan.use_delegated_cgroup = true;
  // Spec §3.12 (G2): PR_SET_NO_NEW_PRIVS on the engine unless
  // engine.allow_new_privileges (--allow-new-privileges); when it is set, an
  // engine that dies before READY gets the one-time /dev/nvidia-uvm hint.
  plan.command.no_new_privileges = config_defaults::kNoNewPrivilegesDefault && !config.engine.allow_new_privileges;
#if defined(__linux__)
  supervisor.no_new_privileges_hint = plan.command.no_new_privileges;
#endif
  ChatRouteConfig chat{.schema = schema_limits, .model_id = model_id, .profile = supervisor.profile,
                       .max_prompt_tokens = config.limits.chat.max_prompt_tokens,
                       .max_completion_tokens = config.limits.chat.max_completion_tokens,
                       .default_max_tokens = config.limits.chat.default_max_tokens,
                       .sampling_default = config.sampling,
                       .fit_max_tokens = config.engine.fit_max_tokens,
                       .qwen_thinking = config.reasoning.effort != ReasoningEffort::none,
                       .qwen_effort = qwen_effort,
                       .effort_at_end = effort_at_end,
                       .gemma4_thinking_default = gemma4_thinking_default(config),
                       .flint_thinking_default = flint_thinking_default(config),
                       .minimax_effort_default = minimax_effort_default(config),
                       .minimum_output_tokens = config.engine.min_tokens.value_or(0U),
                       .maximum_output_tokens = config.engine.max_tokens.value_or(0U),
                       .model_aliases = model_aliases,
                       .queue_timeout = config.limits.concurrency.queue_timeout,
                       .write_stall = config.limits.http.write_stall,
                       .response_write_deadline = config.limits.http.response_write_deadline,
                       .stream_keepalive = config.limits.http.stream_keepalive,
                       .vision = nullptr,
                       .monitor = nullptr};
  // the picture encoder when this model reads images (the gate above)
  std::optional<VisionPlan> vision;
  if (schema_limits.vision_capable && launch.vision_model.has_value()) {
    auto planned = plan_vision(config, launch);
    if (auto* failure = std::get_if<std::string>(&planned)) {
      return ComposeFailure{{std::move(*failure)}, "the picture encoder could not be set up (see the server log)", EXIT_FAILURE};
    }
    vision = std::get<VisionPlan>(std::move(planned));
  }
  return ComposedModel{std::move(launch), std::move(tokenizer), std::move(model_id), std::move(model_aliases), std::move(chat),
                       std::move(supervisor), std::move(plan), std::move(vision)};
}

// Creates <state>/engine, the temp root and the CUDA cache owner-only and
// clears the temp root: no engine runs yet, so temp directories a previous
// frontend left behind are stale. Where the CUDA cache lives is
// engine_environment_for()'s decision: <roots.cache>/cuda (that root was
// already created 0700 by create_runtime_roots() and is left as it is) or
// <state>/cache/cuda, whose parent below the state dir is created owner-only too.
bool prepare_state_dir(const std::filesystem::path& state_dir, const EngineEnvironment& environment) {
  std::vector<std::filesystem::path> directories{state_dir / "engine", environment.temp_dir};
  const auto below_state = [&state_dir](const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(state_dir);
    return !relative.empty() && relative != "." && *relative.begin() != "..";
  };
  if (below_state(environment.cuda_cache_dir.parent_path())) directories.push_back(environment.cuda_cache_dir.parent_path());
  directories.push_back(environment.cuda_cache_dir);
  for (const auto& directory : directories) {
    if (!create_owner_only_directory(directory)) return false;
  }
  return clear_directory_contents(environment.temp_dir);
}

ModelRuntime::ModelRuntime(ComposedModel model, ProcessFactory factory, const GateLimits gate, ServeMonitor* monitor,
                           std::shared_ptr<SupervisorClock> clock, ProcessFactory vision_factory)
    : model_id_(model.model_id),
      aliases_(model.aliases),
      tokenizer_(std::move(model.tokenizer)),
      supervisor_(model.supervisor, std::move(factory), std::move(clock)),
      port_(supervisor_),
      gate_(gate),
      vision_resident_(!model.vision.has_value() || model.vision->resident),
      vision_(model.vision.has_value()
                  ? make_vision_role(*model.vision, vision_factory ? std::move(vision_factory) : vision_process_factory(*model.vision))
                  : VisionRole{}),
      chat_(
          [this, &model, monitor] {
            ChatRouteConfig chat = std::move(model.chat);
            chat.monitor = monitor;
            chat.vision = vision_.service.get();  // nullptr for a text-only model: the route's image gate stays closed
            return chat;
          }(),
          port_, gate_, tokenizer_) {}

bool ModelRuntime::start_vision() {
  if (vision_.supervisor == nullptr || !vision_resident_) return true;
  vision_.supervisor->start();
  return vision_.supervisor->up();
}

void ModelRuntime::stop_vision() noexcept {
  if (vision_.supervisor != nullptr) vision_.supervisor->stop();
}

}  // namespace secure_frontend
