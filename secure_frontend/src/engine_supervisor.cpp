#include "secure_frontend/engine_supervisor.hpp"

#include "descendant_sweep.hpp"
#include "secure_frontend/cgroup_scope.hpp"
#include "secure_frontend/deadline.hpp"
#include "secure_frontend/engine_stdio_process.hpp"
#include "secure_frontend/shutdown_signal.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

constexpr auto kReapPoll = std::chrono::milliseconds(10);

// Polls try_reap() every kReapPoll until it yields a status or `limit` passes
// (supervisor thread only). Shared by fail_closed and end_generation.
std::optional<ExitStatus> reap_within(SupervisedProcess& process, const std::chrono::milliseconds limit) {
  const auto deadline = deadline_after(limit);
  std::optional<ExitStatus> status = process.try_reap();
  while (!status && remaining_until(deadline).count() > 0) {
    std::this_thread::sleep_for(kReapPoll);
    status = process.try_reap();
  }
  return status;
}

void log_to_stderr(const std::string_view line) {
  std::cerr << line << '\n';
  std::cerr.flush();
}

}  // namespace

HealthSnapshot health_for(const SupervisorState state) noexcept {
  switch (state) {
    case SupervisorState::starting: return {state, false, "starting", ""};
    case SupervisorState::ready: return {state, true, "ready", ""};
    case SupervisorState::generating: return {state, true, "generating", ""};
    // SP5 M1 Task 6: the unloaded engine is served-from-cold — healthy, with the literal
    // set kept closed: "starting" names the load the next request triggers (Ruling T6-e).
    case SupervisorState::unloaded: return {state, true, "starting", ""};
    case SupervisorState::stopping:
    case SupervisorState::backoff: return {state, false, "failed", "restarting"};
    case SupervisorState::circuit_open: return {state, false, "failed", "exhausted"};
    case SupervisorState::shutting_down:
    case SupervisorState::stopped: return {state, false, "failed", ""};
  }
  return {SupervisorState::stopped, false, "failed", ""};
}

KillReason kill_reason_for(const EngineProtocolClient& protocol) noexcept {
  const auto code = protocol.failure_code();
  if (!code) return KillReason::protocol_failed;
  switch (*code) {
    case EngineProtocolErrorCode::transport_write_failed: return KillReason::write_timeout;
    case EngineProtocolErrorCode::fatal_engine_line: return KillReason::fatal_line;
    // An ignored STOP sent for a client that went away: not counted (SP3 D-2, risk R4).
    case EngineProtocolErrorCode::abandoned:
      return protocol.stop_cause() == EngineStopCause::client_cancelled ? KillReason::client_stop_drain_failed : KillReason::drain_failed;
    default: return KillReason::protocol_failed;
  }
}

std::optional<ChatExecutionOutcome> execute_with_lease(EngineSupervisor& supervisor, const ChatCompletionRequest& request,
                                                       const TokenizerPack& tokenizer) {
  auto lease = supervisor.acquire();
  if (!lease) return std::nullopt;
  ChatExecutionOutcome outcome = lease->chat().execute(request, tokenizer);
  // A closed or failed protocol leaves the generation unusable. Any ERR line
  // keeps SP0's per-request answer; the generation itself is replaced.
  if (lease->protocol().closed()) supervisor.report_fatal(*lease, kill_reason_for(lease->protocol()));
  return outcome;
}

ProcessFactory make_stdio_process_factory(EngineLaunchPlan plan, SupervisorLog log) {
  std::optional<EngineCgroups> cgroups;
  if (plan.use_delegated_cgroup) cgroups = EngineCgroups::detect();
  return [plan = std::move(plan), cgroups, log = log ? std::move(log) : SupervisorLog(log_to_stderr)](
             const std::uint64_t generation) -> SpawnResult {
    EngineCommand command = plan.command;
    std::optional<std::filesystem::path> temp_dir;
    if (plan.environment) {
      EngineEnvironment environment = *plan.environment;
      // Generation n gets <temp root>/<n>. It is removed by release_resources()
      // after generation n is reaped (end_generation, shutdown_generation); the
      // server clears <state>/engine-tmp/* at startup for anything a previous
      // frontend left behind.
      environment.temp_dir = environment.temp_dir / std::to_string(generation);
      if (!create_owner_only_directory(environment.temp_dir)) return SpawnFailure{"engine temp directory setup failed"};
      temp_dir = environment.temp_dir;
      auto rendered = render_family_environment(environment, plan.family_hook);
      if (!rendered) {
        // Fixed literal, no value or path. Never spawn with an empty or partial environment.
        log("engine generation " + std::to_string(generation) + ": family environment refused");
        std::error_code ignored;
        std::filesystem::remove_all(*temp_dir, ignored);  // nothing ran in it
        return SpawnFailure{"engine environment refused"};
      }
      command.environment = std::move(*rendered);
    }
    if (cgroups) {
      command.cgroup_procs = cgroups->prepare_generation(generation);
      // Fixed literal, no path (privacy rule): the generation runs without a cgroup.
      if (!command.cgroup_procs) log("engine generation " + std::to_string(generation) + ": cgroup unavailable");
    }
    auto process = std::make_unique<EngineStdioProcess>();
    if (const auto error = process->start(command); error.has_value()) {
      // open_cgroup, fork or execve failed: the generation's directory is empty
      // (an execve failure's child was already reaped), so remove it.
      if (command.cgroup_procs) (void)remove_cgroup(command.cgroup_procs->parent_path(), kCgroupRemoveBound);
      std::error_code ignored;
      if (temp_dir) std::filesystem::remove_all(*temp_dir, ignored);  // nothing ran in it
      return SpawnFailure{error->message};
    }
    if (temp_dir) process->set_generation_temp_dir(*temp_dir);
    return std::unique_ptr<SupervisedProcess>(std::move(process));
  };
}

// `owner` is cleared by EngineSupervisor::detach_leases() (shutdown() and the
// destructor) under `mutex`, which a Lease holds while it calls release().
struct LeaseLink final {
  explicit LeaseLink(EngineSupervisor* supervisor) noexcept : owner(supervisor) {}
  std::mutex mutex;
  EngineSupervisor* owner;
};

Lease::Lease(std::shared_ptr<LeaseLink> link, std::shared_ptr<EngineInstance> instance) noexcept
    : link_(std::move(link)), instance_(std::move(instance)) {}
Lease::Lease(Lease&& other) noexcept : link_(std::move(other.link_)), instance_(std::move(other.instance_)) {}
Lease::~Lease() {
  if (!link_ || !instance_) return;
  std::lock_guard lock(link_->mutex);
  if (link_->owner != nullptr) link_->owner->release(std::move(instance_));
}
ChatExecutionService& Lease::chat() const noexcept { return *instance_->chat; }
EngineProtocolClient& Lease::protocol() const noexcept { return *instance_->protocol; }
SupervisedProcess& Lease::process() const noexcept { return *instance_->process; }
std::uint64_t Lease::generation() const noexcept { return instance_->number; }

EngineSupervisor::EngineSupervisor(SupervisorConfig config, ProcessFactory factory, std::shared_ptr<SupervisorClock> clock,
                                   SupervisorLog log)
    : config_(std::move(config)),
      factory_(std::move(factory)),
      clock_(std::move(clock)),
      log_(log ? std::move(log) : SupervisorLog(log_to_stderr)),
      tracker_(config_.policy),
      lease_link_(std::make_shared<LeaseLink>(this)) {
  if (config_.sweep_orphans) (void)become_child_subreaper();
}

EngineSupervisor::~EngineSupervisor() {
  if (thread_.joinable()) shutdown(config_defaults::kShutdownBound);
  detach_leases();
  // The thread is joined: a late signal must not fire this wake once it is gone.
  release_shutdown_signals(wake_);
}

void EngineSupervisor::set_on_stopped(std::function<void()> on_stopped) { on_stopped_ = std::move(on_stopped); }

// SP5 M1 Task 6 (idle_unload_s): the serving path brackets every request. Both are
// no-ops with the pin's "never" default (idle_unload 0).
void EngineSupervisor::note_request_started() {
  if (config_.idle_unload.count() == 0) return;
  ++in_flight_;
  std::lock_guard lock(mutex_);
  idle_deadline_.reset();  // a request in flight, even pre-Lease, is activity
}

void EngineSupervisor::note_request_finished() {
  if (config_.idle_unload.count() == 0) return;
  if (in_flight_.load() > 0U) --in_flight_;
  std::lock_guard lock(mutex_);
  if (state_ == SupervisorState::ready) idle_deadline_ = clock_->now() + config_.idle_unload;
  changed_.notify_all();
}

std::optional<std::chrono::milliseconds> EngineSupervisor::idle_unload_deadline() const {
  std::lock_guard lock(mutex_);
  if (!idle_deadline_.has_value()) return std::nullopt;
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*idle_deadline_ - clock_->now());
  return remaining > std::chrono::milliseconds(0) ? remaining : std::chrono::milliseconds(0);
}

void EngineSupervisor::wake_for_request() {
  {
    // One latch, one owner: wait_asleep consumes wake_pending_ and clears asleep_;
    // spawn_generation clears a latch left while a generation was already serving.
    std::lock_guard lock(mutex_);
    wake_pending_ = true;
    changed_.notify_all();
  }
  wake_.notify();
}

bool EngineSupervisor::start() {
  // Without the wake, shutdown() and report_fatal() could never reach the
  // supervisor thread: refuse rather than run a thread nothing can stop (RF4).
  if (!wake_.valid()) return false;
  {
    std::lock_guard lock(mutex_);
    if (thread_.joinable()) return true;
    // SP5 M1 Task 6 (engine.lazy_load, pin :7925 -> StrataEngine(lazy=True) :966-993):
    // a lazy start never spawns at boot; it sleeps unloaded until the first request.
    if (config_.lazy_start) {
      asleep_ = true;
      state_ = SupervisorState::unloaded;
    } else {
      state_ = SupervisorState::starting;
    }
    changed_.notify_all();
  }
  try {
    thread_ = std::thread([this] { run_guarded(); });
  } catch (const std::system_error&) {
    set_state(SupervisorState::stopped, nullptr);
    return false;
  }
  return true;
}

std::optional<Lease> EngineSupervisor::acquire() {
  std::lock_guard lock(mutex_);
  if (state_ != SupervisorState::ready || !current_) return std::nullopt;
  state_ = SupervisorState::generating;
  idle_deadline_.reset();  // the idle window restarts when the Lease comes back
  changed_.notify_all();
  return Lease(lease_link_, current_);
}

// The Lease's reference is dropped under the lock: current_ or retired_ still
// holds the instance, so its destructor never runs on the serving thread. A
// retired one wakes the supervisor thread so it is swept promptly.
void EngineSupervisor::release(std::shared_ptr<EngineInstance> instance) noexcept {
  bool retired = false;
  {
    std::lock_guard lock(mutex_);
    if (state_ == SupervisorState::generating && current_ == instance) {
      state_ = SupervisorState::ready;
      // SP5 M1 Task 6 (idle_unload_s): the idle window restarts from the release.
      if (config_.idle_unload.count() > 0) idle_deadline_ = clock_->now() + config_.idle_unload;
      changed_.notify_all();
    }
    retired = current_ != instance;
    instance.reset();
  }
  if (retired) wake_.notify();
}

void EngineSupervisor::report_fatal(const Lease& lease, const KillReason reason) {
  {
    std::lock_guard lock(mutex_);
    if (current_ != lease.instance_ || (state_ != SupervisorState::ready && state_ != SupervisorState::generating)) return;
    pending_kill_ = reason == KillReason::none ? KillReason::protocol_failed : reason;
    state_ = SupervisorState::stopping;
    changed_.notify_all();
  }
  wake_.notify();
}

HealthSnapshot EngineSupervisor::snapshot() const {
  std::lock_guard lock(mutex_);
  return health_for(state_);
}

bool EngineSupervisor::stop_requested() const noexcept { return shutdown_requested_.load() || shutdown_signalled(); }

bool EngineSupervisor::wait_for_state(const SupervisorState state, const std::chrono::milliseconds timeout) const {
  std::unique_lock lock(mutex_);
  return changed_.wait_for(lock, timeout, [this, state] { return state_ == state; });
}

HealthSnapshot EngineSupervisor::wait_for_change(const SupervisorState from, const std::chrono::milliseconds timeout) const {
  std::unique_lock lock(mutex_);
  (void)changed_.wait_for(lock, timeout, [this, from] { return state_ != from; });
  return health_for(state_);
}

void EngineSupervisor::shutdown(const std::chrono::milliseconds bound) {
  shutdown_requested_.store(true);
  wake_.notify();
  if (!thread_.joinable()) {
    detach_leases();
    return;
  }
  {
    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock, bound, [this] { return state_ == SupervisorState::stopped; })) {
      // Spec §3.7: an orderly shutdown that cannot finish within the bound exits 1.
      std::_Exit(EXIT_FAILURE);
    }
    // finish() waits for this, so a main thread that never gets here (stuck
    // after a shutdown signal) still ends the process within the bound.
    main_acknowledged_ = true;
    changed_.notify_all();
  }
  thread_.join();
  detach_leases();
}

// RF4: a Lease still out (its generation stays in retired_ until the
// supervisor is destroyed) never calls into the supervisor after this.
void EngineSupervisor::detach_leases() noexcept {
  std::lock_guard lock(lease_link_->mutex);
  lease_link_->owner = nullptr;
}

// The supervisor thread's entry: nothing may escape it into std::terminate
// (RF4). An exception from the factory, the log sink, the clock or an
// allocation fails closed instead.
void EngineSupervisor::run_guarded() noexcept {
  try {
    run();
    return;
  } catch (...) {
  }
  fail_closed();
}

// After an escaped exception: kill and reap the generation still current, open
// the circuit (no restart until the frontend is restarted) and wait for
// shutdown, so shutdown() still ends in `stopped` within its bound.
void EngineSupervisor::fail_closed() noexcept {
  try {
    std::shared_ptr<EngineInstance> instance;
    {
      std::lock_guard lock(mutex_);
      instance = current_;
    }
    if (instance && instance->process) {
      SupervisedProcess& process = *instance->process;
      process.kill_group();
      const bool reaped = reap_within(process, config_.policy.reap_timeout).has_value();
      retire(instance, reaped);
      if (reaped) {
        // Spec §3.7(b): sweep after every generation, as end_generation and
        // shutdown_generation do; 250 ms keeps a later shutdown inside its bound.
        if (config_.sweep_orphans) (void)sweep_orphans({}, std::chrono::milliseconds(250));
        process.release_resources(std::chrono::milliseconds(0));
      }
    }
    set_state(SupervisorState::circuit_open, nullptr);
    try {
      log_("engine supervisor failed: circuit open");
    } catch (...) {
    }
    wait_in_circuit_open();
  } catch (...) {
  }
  try {
    finish();
  } catch (...) {
    // Nothing left to try: shutdown() exits 1 past its bound (spec §3.7).
  }
}

void EngineSupervisor::run() {
  for (;;) {
    sweep_retired();
    if (stop_requested()) break;
    bool parked = false;
    {
      std::lock_guard lock(mutex_);
      parked = asleep_;  // mutex_-guarded: wake_for_request() flips it from a serving thread
    }
    if (parked) {
      wait_asleep();  // SP5 M1 Task 6: lazy start / idle unload; wakes on a request
      if (stop_requested()) break;
    }
    std::chrono::milliseconds healthy_for{0};
    GenerationEnd ended = GenerationEnd::counted;  // a spawn failure counts
    // Scoped to the generation: once retired, only retired_ and Leases hold it,
    // so the backoff and circuit_open sweeps can destroy it.
    if (auto instance = spawn_generation()) {
      const KillReason reason = supervise(*instance, healthy_for);
      if (reason == KillReason::shutdown) {
        shutdown_generation(instance);
        break;
      }
      if (reason == KillReason::idle_unload) {
        // SP5 M1 Task 6 (pin :4100-4109): the teardown was asked for, not a failure:
        // no failure is recorded, the healthy window is banked, and the supervisor
        // sleeps unloaded until a request wakes it for the same composed launch.
        if (!unload_generation(instance)) {
          if (!schedule_restart(healthy_for, GenerationEnd::unreaped)) {
            wait_in_circuit_open();
            break;
          }
        } else {
          tracker_.note_healthy(healthy_for);
        }
        if (stop_requested()) break;
        {
          // Parked: the state says unloaded again (unload_generation passed through
          // `stopping` while it reaped), so /health and the gate see the cold engine.
          std::lock_guard lock(mutex_);
          asleep_ = true;
          state_ = SupervisorState::unloaded;
          idle_deadline_.reset();
          changed_.notify_all();
        }
        continue;
      }
      ended = end_generation(instance, reason);
    }
    if (stop_requested()) break;
    if (!schedule_restart(healthy_for, ended)) {
      wait_in_circuit_open();
      break;
    }
  }
  finish();
}

std::shared_ptr<EngineInstance> EngineSupervisor::spawn_generation() {
  const std::uint64_t number = generations_started_.fetch_add(1U) + 1U;
  set_state(SupervisorState::starting, nullptr);
  SpawnResult spawned = factory_(number);
  if (std::holds_alternative<SpawnFailure>(spawned)) {
    log_end(number, exit_kind_literal(ExitKind::spawn_failed));
    return nullptr;
  }
  auto instance = std::make_shared<EngineInstance>();
  instance->number = number;
  instance->process = std::move(std::get<std::unique_ptr<SupervisedProcess>>(spawned));
  instance->protocol = std::make_unique<EngineProtocolClient>(instance->process->transport(), EngineProtocolLimits{}, config_.flavor);
  set_state(SupervisorState::starting, instance);
  {
    // SP5 M1 Task 6: the spawn itself consumes any wake latched during the unload
    // window (the request this wake was for is served by this generation).
    std::lock_guard lock(mutex_);
    wake_pending_ = false;
  }
  return instance;
}

std::map<std::string, std::string> EngineSupervisor::ready_readings() const {
  std::lock_guard lock(mutex_);
  return ready_readings_;
}

KillReason EngineSupervisor::supervise(EngineInstance& instance, std::chrono::milliseconds& healthy_for) {
  EngineReady ready{};
  if (const auto reason = await_ready(instance, ready); reason.has_value()) return *reason;
  {
    // The engine's startup INFO is complete at READY; the Monitor reads the expert tier from it.
    std::lock_guard lock(mutex_);
    ready_readings_ = instance.protocol->engine_readings();
  }
  instance.chat = std::make_unique<ChatExecutionService>(*instance.protocol, config_.chat_limits, config_.profile);
  instance.chat->adopt_ready(ready);
  const auto ready_at = clock_->now();
  set_state(SupervisorState::ready);
  // SP5 M1 Task 6 (idle_unload_s): the first idle window starts at READY.
  if (config_.idle_unload.count() > 0) {
    std::lock_guard lock(mutex_);
    idle_deadline_ = ready_at + config_.idle_unload;
    changed_.notify_all();
  }
  const KillReason reason = watch(instance);
  healthy_for = std::chrono::duration_cast<std::chrono::milliseconds>(clock_->now() - ready_at);
  return reason;
}

// Eager READY (spec §3.4), bounded by policy.ready_timeout (§3.5(2)), in short
// slices so shutdown and an early exit are seen promptly.
std::optional<KillReason> EngineSupervisor::await_ready(EngineInstance& instance, EngineReady& ready) {
  const auto deadline = clock_->now() + config_.policy.ready_timeout;
  for (;;) {
    if (stop_requested()) return KillReason::shutdown;
    const auto remaining = remaining_until(deadline, clock_->now());
    if (remaining.count() == 0) return KillReason::ready_timeout;
    const auto error = instance.protocol->await_ready(std::min(remaining, config_.ready_poll_slice), ready);
    if (!error) return std::nullopt;
    if (error->code == EngineProtocolErrorCode::closed) return KillReason::none;  // it ended; its exit status says why
    // A fatal verify line before READY is fatal_line (§3.8); others protocol_failed.
    if (error->code != EngineProtocolErrorCode::timeout) return kill_reason_for(*instance.protocol);
    if (instance.process->try_reap()) return KillReason::none;
  }
}

KillReason EngineSupervisor::watch(EngineInstance& instance) {
  for (;;) {
    {
      std::lock_guard lock(mutex_);
      if (pending_kill_ != KillReason::none) return std::exchange(pending_kill_, KillReason::none);
      // SP5 M1 Task 6 (parity row idle_unload_s, pin :4100-4109): with a window armed,
      // no Lease out, no request in flight and the deadline passed, this generation is
      // unloaded on purpose. The state flips here so no Lease can slip in afterwards.
      if (config_.idle_unload.count() > 0 && in_flight_.load() == 0U && state_ == SupervisorState::ready &&
          idle_deadline_.has_value() && clock_->now() >= *idle_deadline_) {
        idle_deadline_.reset();
        state_ = SupervisorState::unloaded;
        changed_.notify_all();
        return KillReason::idle_unload;
      }
    }
    if (stop_requested()) return KillReason::shutdown;
    if (instance.process->try_reap()) return KillReason::none;
    sweep_retired();
    // Spec V4: wait on the child itself, so a death between requests is seen
    // at once; without a pidfd, poll every monitor_poll. SP5 M1 Task 6: while an
    // idle window is armed the wait is bounded by the deadline (capped by
    // monitor_poll), so the unload fires without a wake for pidfd children too.
    const std::intptr_t handle = instance.process->monitor_handle();
    auto slice = handle == kNoMonitorHandle ? config_.monitor_poll : std::chrono::milliseconds(-1);
    {
      std::lock_guard lock(mutex_);
      if (config_.idle_unload.count() > 0 && state_ == SupervisorState::ready && idle_deadline_.has_value())
        slice = std::min(slice < std::chrono::milliseconds(0) ? config_.monitor_poll : slice,
                         std::max(std::chrono::milliseconds(1),
                                  std::chrono::duration_cast<std::chrono::milliseconds>(*idle_deadline_ - clock_->now())));
    }
    (void)wake_.wait(handle, slice);
  }
}

// `stopping` (spec §3.8): kill the whole group/Job, reap within reap_timeout,
// classify and log. `uncounted` only for our own client_stop_drain_failed
// kill (SP3 D-2) while the streak is under kMaxConsecutiveUncountedRestarts;
// an engine that ended by itself counts whatever was reported.
EngineSupervisor::GenerationEnd EngineSupervisor::end_generation(const std::shared_ptr<EngineInstance>& instance, const KillReason reason) {
  set_state(SupervisorState::stopping);
  SupervisedProcess& process = *instance->process;
  const bool ended_on_its_own = process.try_reap().has_value();
  process.kill_group();  // spec §3.7a: always, even after the leader has ended
  const std::optional<ExitStatus> status = reap_within(process, config_.policy.reap_timeout);
  retire(instance, status.has_value());
  if (!status) {
    log_end(instance->number, "unreaped");
    return GenerationEnd::unreaped;
  }
  // Spec §3.7(b): orphans (setsid, setpgid, double fork) re-parented to us are
  // killed and reaped; no engine is alive at this point.
  if (config_.sweep_orphans) (void)sweep_orphans({}, std::chrono::milliseconds(1000));
  process.release_resources(std::chrono::milliseconds(1000));
  const KillReason sent = ended_on_its_own ? KillReason::none : (reason == KillReason::none ? KillReason::protocol_failed : reason);
  const ExitClassification classified = classify_exit(*status, sent);
  log_end(instance->number, describe_exit(classified));
  // Keyed on the classification, not on `sent`: an engine that died before
  // READY may not be reapable yet at the first check, so a kill was "sent" to
  // a dead child; its status still says it ended on its own (final review M1).
  if (config_.no_new_privileges_hint && !nnp_hint_logged_ && !instance->chat && classified.kind != ExitKind::killed_by_supervisor) {
    nnp_hint_logged_ = true;
    log_(kNoNewPrivilegesHint);
  }
  // The streak of uncounted restarts ends at any successful DONE; a counted
  // failure does not end it (controller ruling, SP3 D-2 bound).
  if (instance->protocol && instance->protocol->completed_generations() > 0U) uncounted_streak_ = 0U;
  const bool client_stop = classified.kind == ExitKind::killed_by_supervisor && classified.reason == KillReason::client_stop_drain_failed;
  if (!client_stop || uncounted_streak_ >= config_defaults::kMaxConsecutiveUncountedRestarts) return GenerationEnd::counted;
  ++uncounted_streak_;
  return GenerationEnd::uncounted;
}

bool EngineSupervisor::schedule_restart(const std::chrono::milliseconds healthy_for, const GenerationEnd ended) {
  if (ended == GenerationEnd::unreaped) {
    // Spec V10 / K3: never spawn while a killed child is unreaped; past
    // reap_timeout the circuit opens whatever the failure count.
    set_state(SupervisorState::circuit_open, nullptr);
    return false;
  }
  if (ended == GenerationEnd::uncounted) {
    // SP3 D-2 (risk R4): a client that went away during a long prefill must
    // not open the circuit. No failure is recorded, so neither the count nor
    // the backoff grows; `backoff` lasts zero time and the next spawn follows.
    // The healthy-window reset (spec §3.3) still applies.
    tracker_.note_healthy(healthy_for);
    set_state(SupervisorState::backoff, nullptr);
    return true;
  }
  const RestartDecision decision = tracker_.record_failure(healthy_for);
  if (decision.circuit_open) {
    set_state(SupervisorState::circuit_open, nullptr);
    return false;
  }
  set_state(SupervisorState::backoff, nullptr);
  // Spec §3.3: only shutdown cuts the delay short. Any other wake (a released
  // Lease, a stale report_fatal notify) sweeps and sleeps out the remainder.
  const auto restart_at = clock_->now() + decision.delay;
  for (auto remaining = decision.delay; remaining.count() > 0 && !stop_requested();
       remaining = std::chrono::ceil<std::chrono::milliseconds>(restart_at - clock_->now())) {
    if (clock_->sleep_backoff(remaining, wake_)) break;
    sweep_retired();
  }
  return true;
}

void EngineSupervisor::wait_in_circuit_open() {
  while (!stop_requested()) {
    sweep_retired();
    (void)wake_.wait(kNoMonitorHandle, std::chrono::milliseconds(-1));
  }
}

// SP5 M1 Task 6: parked while unloaded (lazy start or idle teardown). Sweeps like the
// circuit wait; leaves on a latched request wake (wake_for_request) or shutdown. The
// state stays `unloaded` until spawn_generation reports `starting`.
void EngineSupervisor::wait_asleep() {
  for (;;) {
    sweep_retired();
    {
      std::lock_guard lock(mutex_);
      if (stop_requested() || wake_pending_) {
        wake_pending_ = false;
        asleep_ = false;
        changed_.notify_all();
        return;
      }
    }
    (void)wake_.wait(kNoMonitorHandle, std::chrono::milliseconds(-1));
  }
}

// Spec §3.7: EOF first (the engine exits 0 on EOF) unless a Lease still uses
// engine stdin; then SIGTERM to the group; then always SIGKILL the group
// (§3.7a); reap, sweep (§3.7b), retire, remove a delegated cgroup (§3.7d).
// Worst case 2 + 1 + 1 + 0.25 + 0.1 (kCgroupRemoveBound) = 4.35 s, inside the
// 5 s bound (ruling RF5).
void EngineSupervisor::shutdown_generation(const std::shared_ptr<EngineInstance>& instance) {
  note_shutdown_started();
  bool lease_outstanding = false;
  {
    std::lock_guard lock(mutex_);
    lease_outstanding = state_ == SupervisorState::generating;
    state_ = SupervisorState::shutting_down;  // no new Lease from here on
    changed_.notify_all();
  }
  SupervisedProcess& process = *instance->process;
  const auto wait_reaped = [this, &process](const std::chrono::milliseconds limit) {
    const auto deadline = deadline_after(limit);
    while (!process.try_reap()) {
      const auto remaining = remaining_until(deadline);
      if (remaining.count() == 0) return false;
      (void)wake_.wait(process.monitor_handle(), std::min(remaining, config_.monitor_poll));
    }
    return true;
  };
  bool reaped = false;
  if (!lease_outstanding) {
    process.close_stdin();
    reaped = wait_reaped(config_.shutdown_stdin_wait);
  }
  if (!reaped) {
    process.signal_group_terminate();
    reaped = wait_reaped(config_.shutdown_term_wait);
  }
  process.kill_group();  // always, even after the leader has ended
  if (!reaped) reaped = wait_reaped(config_.shutdown_term_wait);
  retire(instance, reaped);
  if (!reaped) {
    log_end(instance->number, "unreaped");
    return;
  }
  // Spec §3.7 / §3.7(b): shutdown sweeps too, so an escape alive at shutdown
  // dies with the frontend; 250 ms fits the shutdown bound.
  if (config_.sweep_orphans) (void)sweep_orphans({}, std::chrono::milliseconds(250));
  process.release_resources(std::chrono::milliseconds(0));
  // We asked this generation to end (§3.6 "exit sent by us"), so an exit 0 on
  // our EOF or SIGTERM is killed_by_supervisor(shutdown), not clean_exit.
  ExitClassification ended = classify_exit(*process.try_reap(), KillReason::shutdown);
  if (ended.kind == ExitKind::clean_exit) ended = {ExitKind::killed_by_supervisor, KillReason::shutdown};
  log_end(instance->number, describe_exit(ended));
}

// SP5 M1 Task 6 (parity row idle_unload_s; pin :4100-4109 + :6252): the same orderly stop
// the shutdown path uses — STDIN EOF first, real engine exits 0 — but the supervisor
// keeps running and parks unloaded. The end is an intentional unload, so the log names
// it, not the exit classification. Returns false only when even KILL could not reap:
// the caller routes that through the circuit like any other unreaped end (spec V10).
bool EngineSupervisor::unload_generation(const std::shared_ptr<EngineInstance>& instance) {
  bool lease_outstanding = false;
  {
    std::lock_guard lock(mutex_);
    lease_outstanding = state_ == SupervisorState::generating;  // the idle check excludes this; belt-on-braces
    state_ = SupervisorState::stopping;
    changed_.notify_all();
  }
  SupervisedProcess& process = *instance->process;
  const auto wait_reaped = [this, &process](const std::chrono::milliseconds limit) {
    const auto deadline = deadline_after(limit);
    while (!process.try_reap()) {
      const auto remaining = remaining_until(deadline);
      if (remaining.count() == 0) return false;
      (void)wake_.wait(process.monitor_handle(), std::min(remaining, config_.monitor_poll));
    }
    return true;
  };
  bool reaped = false;
  if (!lease_outstanding) {
    process.close_stdin();
    reaped = wait_reaped(config_.shutdown_stdin_wait);
  }
  if (!reaped) {
    process.signal_group_terminate();
    reaped = wait_reaped(config_.shutdown_term_wait);
  }
  process.kill_group();  // always, even after the leader has ended
  if (!reaped) reaped = wait_reaped(config_.shutdown_term_wait);
  retire(instance, reaped);
  if (!reaped) {
    log_end(instance->number, "unreaped");
    return false;
  }
  if (config_.sweep_orphans) (void)sweep_orphans({}, std::chrono::milliseconds(250));
  process.release_resources(std::chrono::milliseconds(0));
  log_end(instance->number, "unloaded");
  return true;
}

// Takes the ended generation out of service. A reaped one waits in retired_
// for its Lease; an unreaped one is kept alive in abandoned_, since destroying
// it would wait for the child again.
void EngineSupervisor::retire(const std::shared_ptr<EngineInstance>& instance, const bool reaped) {
  std::lock_guard lock(mutex_);
  current_.reset();
  pending_kill_ = KillReason::none;
  (reaped ? retired_ : abandoned_).push_back(instance);
}

// Supervisor thread only: destroys retired generations no Lease still holds.
void EngineSupervisor::sweep_retired() {
  std::vector<std::shared_ptr<EngineInstance>> released;
  {
    std::lock_guard lock(mutex_);
    const auto still_leased = std::partition(retired_.begin(), retired_.end(),
                                             [](const std::shared_ptr<EngineInstance>& instance) { return instance.use_count() > 1; });
    released.assign(std::make_move_iterator(still_leased), std::make_move_iterator(retired_.end()));
    retired_.erase(still_leased, retired_.end());
  }
}

// Every engine process we could reap is gone: stop the listener, report
// `stopped`, release a Windows console handler, then give main until the §3.7
// bound to finish. A generation left in abandoned_ (never reaped) is reported
// so main exits 1, not 0 (spec C4, final review I2).
void EngineSupervisor::finish() {
  note_shutdown_started();
  sweep_retired();
  bool unreaped = false;
  {
    std::lock_guard lock(mutex_);
    unreaped = !abandoned_.empty();
  }
  if (unreaped) {
    engine_left_unreaped_.store(true);
    log_(kEngineLeftUnreapedLine);
  }
  set_state(SupervisorState::shutting_down, nullptr);
  if (on_stopped_) on_stopped_();
  set_state(SupervisorState::stopped);
  notify_shutdown_complete();
  std::unique_lock lock(mutex_);
  if (!changed_.wait_until(lock, shutdown_deadline_, [this] { return main_acknowledged_; })) std::_Exit(EXIT_FAILURE);
}

void EngineSupervisor::note_shutdown_started() noexcept {
  if (shutdown_deadline_ == std::chrono::steady_clock::time_point{}) {
    shutdown_deadline_ = std::chrono::steady_clock::now() + config_defaults::kShutdownBound;
  }
}

void EngineSupervisor::set_state(const SupervisorState state) {
  std::lock_guard lock(mutex_);
  state_ = state;
  changed_.notify_all();
}

void EngineSupervisor::set_state(const SupervisorState state, std::shared_ptr<EngineInstance> current) {
  std::lock_guard lock(mutex_);
  state_ = state;
  current_ = std::move(current);
  changed_.notify_all();
}

void EngineSupervisor::log_end(const std::uint64_t generation, const std::string_view kind) {
  log_("engine generation " + std::to_string(generation) + " ended: " + std::string(kind));
}

}  // namespace secure_frontend
