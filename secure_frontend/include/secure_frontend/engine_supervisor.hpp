#pragma once

#include "secure_frontend/cgroup_scope.hpp"
#include "secure_frontend/chat_execution.hpp"
#include "secure_frontend/config_defaults.hpp"
#include "secure_frontend/engine_environment.hpp"
#include "secure_frontend/engine_process.hpp"
#include "secure_frontend/engine_protocol_client.hpp"
#include "secure_frontend/exit_status.hpp"
#include "secure_frontend/host_readings.hpp"
#include "secure_frontend/restart_policy.hpp"
#include "secure_frontend/supervised_process.hpp"
#include "secure_frontend/supervisor_clock.hpp"
#include "secure_frontend/supervisor_wake.hpp"
#include "secure_frontend/tokenizer_pack.hpp"

#include <map>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace secure_frontend {

// Spec §3.8; the transition table is in the SP2 plan header. SP5 M1 Task 6 adds
// `unloaded` (runconfig.py:35-38): the engine is torn down when idle, or never
// started when lazy_load; a request wakes it and the same composed launch respawns it.
enum class SupervisorState { starting, ready, generating, stopping, backoff, circuit_open, shutting_down, stopped, unloaded };

// The /health view of one state. `engine` and `recovery` are fixed literals;
// `recovery` ("restarting" | "exhausted") is empty unless engine == "failed".
struct HealthSnapshot final {
  SupervisorState state;
  bool healthy;
  std::string_view engine;
  std::string_view recovery;
};

[[nodiscard]] HealthSnapshot health_for(SupervisorState state) noexcept;

// One engine generation: fresh process, protocol client and chat service per
// spawn (spec §3.2), kept alive by a shared_ptr until the last Lease ends.
struct EngineInstance final {
  std::uint64_t number{0};
  std::unique_ptr<SupervisedProcess> process;
  std::unique_ptr<EngineProtocolClient> protocol;
  std::unique_ptr<ChatExecutionService> chat;
};

class EngineSupervisor;
// What a Lease reaches its supervisor through; shutdown() detaches it.
struct LeaseLink;

// The serving thread's borrow of the current generation, taken in `ready`.
// Releasing it returns the supervisor from `generating` to `ready`. A Lease
// may outlive shutdown() and even the supervisor (RF4): once shutdown() has
// returned it no longer refers to the supervisor, and destroying it only drops
// its own reference to the generation.
class Lease final {
 public:
  Lease(Lease&& other) noexcept;
  Lease& operator=(Lease&&) = delete;
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;
  ~Lease();

  [[nodiscard]] ChatExecutionService& chat() const noexcept;
  [[nodiscard]] EngineProtocolClient& protocol() const noexcept;
  [[nodiscard]] SupervisedProcess& process() const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept;

 private:
  friend class EngineSupervisor;
  Lease(std::shared_ptr<LeaseLink> link, std::shared_ptr<EngineInstance> instance) noexcept;
  std::shared_ptr<LeaseLink> link_;
  std::shared_ptr<EngineInstance> instance_;
};

struct SupervisorConfig final {
  RestartPolicy policy{};
  ChatExecutionLimits chat_limits{};
  NativeChatProfile profile{NativeChatProfile::qwen};
  EngineProtocolFlavor flavor{EngineProtocolFlavor::strata_0143};
  std::chrono::milliseconds ready_poll_slice{config_defaults::kReadyPollSlice};
  std::chrono::milliseconds monitor_poll{config_defaults::kMonitorFallbackPoll};
  std::chrono::milliseconds shutdown_stdin_wait{config_defaults::kShutdownStdinWait};
  std::chrono::milliseconds shutdown_term_wait{config_defaults::kShutdownTermWait};
  // Linux: become the child subreaper and sweep orphaned descendants after every generation (spec §3.7b).
  bool sweep_orphans{true};
  // Linux: log the /dev/nvidia-uvm hint when a generation ends before READY under PR_SET_NO_NEW_PRIVS.
  bool no_new_privileges_hint{false};
  // SP5 M1 Task 6 (parity rows lazy_load / idle_unload_s, runconfig.py:35-38): lazy_start
  // sleeps unloaded until the first request wakes it; idle_unload > 0 tears an idle
  // generation down after that many whole seconds without a request (0: never, the pin default).
  bool lazy_start{false};
  std::chrono::seconds idle_unload{0};
};

// Logged once when a generation ends before READY on its own while
// PR_SET_NO_NEW_PRIVS is set: the NVIDIA driver may need to exec setuid
// nvidia-modprobe to create /dev/nvidia-uvm*, which NNP blocks (spec open question 5).
inline constexpr std::string_view kNoNewPrivilegesHint =
    "engine ended before READY while PR_SET_NO_NEW_PRIVS is set; if this host relies on nvidia-modprobe to create "
    "/dev/nvidia-uvm, create the device nodes at boot (as root: nvidia-modprobe -u -c=0, or enable nvidia-persistenced) "
    "or start the frontend with --allow-new-privileges";

// Logged once by the supervisor thread when it stops with a generation it
// could not reap (spec C4: exit 0 means no engine is left); main then exits 1.
inline constexpr std::string_view kEngineLeftUnreapedLine = "engine supervisor stopped with an unreaped engine: exit 1";

// Receives fixed-literal lines such as "engine generation 2 ended: abort".
using SupervisorLog = std::function<void(std::string_view line)>;

// Owns every engine generation for the frontend's lifetime (spec §3.1): one
// supervisor thread spawns, consumes READY, watches, kills and reaps; it is
// the only reaper. The serving side borrows the current generation through
// acquire() and reports an unusable one with report_fatal().
//
// Thread safety (SP3 D-1; covered by supervisor_serving_api_is_thread_safe):
// - The serving API (acquire, snapshot, report_fatal, wait_for_change,
//   wait_for_state, stop_requested, generations_started) and releasing a Lease
//   (~Lease) may be called concurrently from any number of threads. Each one
//   takes `mutex_` (or reads an atomic) for the duration of the call only;
//   none forks, reaps, kills or waits on a child. That stays on the supervisor
//   thread (spec §3.1), with one exception per platform: on POSIX, a Lease
//   that outlives the supervisor and holds a generation the supervisor
//   abandoned (never reaped) drops the last reference, and that process's
//   destructor runs terminate(250 ms) (group kill, then a bounded wait) on
//   the thread destroying the Lease; on Windows the same holds of a
//   generation that WAS reaped but a Lease still held (it waits in
//   retired_): once the supervisor is gone, its process cleanup (handle
//   release) runs on the thread destroying the Lease.
// - At most one Lease exists at a time: acquire() returns nullopt while one is
//   out, so callers need no lock of their own around it.
// - A Lease is used by one thread at a time: its accessors and the
//   ChatExecutionService/EngineProtocolClient behind them are not
//   synchronized. It may move to another thread (e.g. be destroyed there).
// - Not concurrent: set_on_stopped() and start() run once, before serving, on
//   the owning thread; shutdown() is called from one thread (the owning
//   thread, or the destructor). shutdown() may run while other threads use the
//   serving API or hold a Lease: from `shutting_down` on, acquire() returns
//   nullopt and waiters wake.
// - Nothing may call into the supervisor once its destructor has begun, and
//   every serving-API call (including a wait_for_change/wait_for_state still
//   blocked) must have returned before it begins: the destructor does not wait
//   for callers. A Lease is the exception: it may outlive shutdown() and the
//   supervisor (RF4, see Lease).
class EngineSupervisor final {
 public:
  EngineSupervisor(SupervisorConfig config, ProcessFactory factory,
                   std::shared_ptr<SupervisorClock> clock = std::make_shared<SteadySupervisorClock>(), SupervisorLog log = {});
  ~EngineSupervisor();
  EngineSupervisor(const EngineSupervisor&) = delete;
  EngineSupervisor& operator=(const EngineSupervisor&) = delete;

  // Starts the supervisor thread, which spawns generation 1. Call once.
  // false = refused (no wake to reach the thread, or no thread): nothing runs.
  [[nodiscard]] bool start();
  // Only in `ready`; anywhere else nullopt (the caller answers a generic 503 at once).
  [[nodiscard]] std::optional<Lease> acquire();
  // The leased generation is unusable (fatal line, drain failure, write timeout,
  // failed protocol): kill it, count a failure, restart per policy.
  // KillReason::client_stop_drain_failed (SP3 D-2: the drain after a STOP sent
  // for a client that went away failed) kills and restarts the same way but
  // counts no failure: the restart follows at once (zero-length `backoff`) and
  // the failure count is unchanged, except that a generation healthy for
  // healthy_window still resets it to 0. If the engine had already ended by
  // itself, its end counts as usual.
  // Bound (controller ruling): at most config_defaults::
  // kMaxConsecutiveUncountedRestarts (3) such restarts in a row with no
  // successful DONE on any generation between them; later ones count like
  // drain_failed, so an engine that always ignores STOP still reaches
  // circuit_open. A successful DONE ends the streak; a counted failure does not.
  // Production code derives this reason only through kill_reason_for(protocol)
  // (abandoned + a STOP sent with EngineStopCause::client_cancelled); passing
  // it here directly is for tests only. Two edges SP3 should know: a stray
  // begin_generation() while a client-STOP generation is still active is
  // refused but erases the recorded cause, so that generation's failure counts
  // as drain_failed (errs toward counting); and a STOPped generation that
  // finishes with DONE (a cancel that completed) is a successful DONE and
  // ends the uncounted streak.
  void report_fatal(const Lease& lease, KillReason reason);
  // One sample per /health request (SP0's sample-once rule).
  [[nodiscard]] HealthSnapshot snapshot() const;
  // Blocks until the state is no longer `from` or `timeout` passes, and returns
  // the state then (still `from` on timeout). The wait is not a poll: it uses
  // the condition variable that every state change notifies under `mutex_`,
  // the same one wait_for_state() uses. It observes states, not transitions:
  // the predicate is re-checked when the waiter wakes, so a change that is
  // undone before then is missed (a release immediately followed by another
  // acquire leaves `generating`, and the wait goes on). Use
  // wait_for_change(generating, t) to wait for a Lease release (-> ready). The same wait wakes when the leased
  // generation fails (report_fatal or a crash -> stopping) and on shutdown
  // (-> shutting_down), so a waiter never sleeps through either. Once the state
  // is `stopped` nothing changes it, so a wait from `stopped` runs to its timeout.
  [[nodiscard]] HealthSnapshot wait_for_change(SupervisorState from, std::chrono::milliseconds timeout) const;
  // Requests the orderly shutdown (spec §3.7) and waits for it; past `bound`
  // the process exits 1. Idempotent.
  void shutdown(std::chrono::milliseconds bound);
  // A model stop while the server goes on (the launcher's stop, switch, failed start or calibration step; owner
  // 2026-10-28, the "unreaped engine" after a model stop): shutdown(), except that a killed engine still ending is
  // waited for up to policy.reap_timeout more, as a crashed one is (spec V10). A big model takes longer than the 5 s
  // server bound (ruling RF5) to free its memory and end; the next model must not start beside it. hurry() cuts that
  // wait short; past it the engine is left unreaped, as shutdown() leaves one. Called like shutdown().
  void stop_engine();
  // Ends stop_engine's wait for a killed engine, now or when it comes (the server's own shutdown keeps its bound).
  // Any thread; nothing else changes.
  void hurry() noexcept;
  // True once shutdown() was called or a shutdown signal arrived.
  [[nodiscard]] bool stop_requested() const noexcept;
  // After shutdown() has returned: true when a generation was never reaped
  // (kEngineLeftUnreapedLine was logged). Spec C4: the process must not exit 0
  // then; main returns 1.
  [[nodiscard]] bool engine_left_unreaped() const noexcept { return engine_left_unreaped_.load(); }
  // The wake object install_shutdown_signals() fires.
  [[nodiscard]] SupervisorWake& wake() noexcept { return wake_; }
  // Called on the supervisor thread after every engine process is gone (main
  // passes HttpSocketServer::shutdown_listener). Set before start().
  void set_on_stopped(std::function<void()> on_stopped);

  // SP5 M1 Task 6 (parity rows lazy_load / idle_unload_s; pin :4100-4109 counts only
  // requests, and the idle thread acts "without requests"): the serving path brackets
  // every request around these two calls; both are no-ops when idle_unload is 0 (the
  // pin's "never" default). note_request_started clears the armed idle window;
  // note_request_finished re-arms it when the engine is ready.
  void note_request_started();
  void note_request_finished();
  // The armed idle window's remaining time (nullopt: unloaded or idle off). Test seam
  // and wake diagnostics; the GenerationGate does not depend on it.
  [[nodiscard]] std::optional<std::chrono::milliseconds> idle_unload_deadline() const;
  // A request found the supervisor unloaded (lazy start or idle teardown): wake it so
  // the same composed launch spawns the next generation (the pin's "loads on use").
  void wake_for_request();

  // Test seams.
  [[nodiscard]] bool wait_for_state(SupervisorState state, std::chrono::milliseconds timeout) const;
  [[nodiscard]] std::uint64_t generations_started() const noexcept { return generations_started_.load(); }
  // The engine's own startup readings (INFO before READY), captured when it reached READY;
  // the Monitor reads the expert tier from these so a just-started model shows experts loaded
  // without a first generation. Empty before READY, or when the engine printed none.
  [[nodiscard]] std::map<std::string, std::string> ready_readings() const;
  // The Monitor (owner 2026-10-28), once a second: the engine that is up (ready or generating) - a number that changes
  // with each READY and never repeats in this process, its READY readings, its process's resident memory and its
  // conversation cache (SupervisedProcess). nullopt while none is up (starting, failed, unloaded, stopped).
  [[nodiscard]] std::optional<EngineSample> engine_sample() const;

 private:
  friend class Lease;
  void release(std::shared_ptr<EngineInstance> instance) noexcept;
  void run_guarded() noexcept;
  void run();
  void fail_closed() noexcept;
  std::shared_ptr<EngineInstance> spawn_generation();
  KillReason supervise(EngineInstance& instance, std::chrono::milliseconds& healthy_for);
  std::optional<KillReason> await_ready(EngineInstance& instance, EngineReady& ready);
  KillReason watch(EngineInstance& instance);
  // How an ended generation affects the restart policy (SP3 D-2: `uncounted`).
  enum class GenerationEnd { counted, uncounted, unreaped };
  GenerationEnd end_generation(const std::shared_ptr<EngineInstance>& instance, KillReason reason);
  bool schedule_restart(std::chrono::milliseconds healthy_for, GenerationEnd ended);
  void wait_in_circuit_open();
  void shutdown_generation(const std::shared_ptr<EngineInstance>& instance);
  // SP5 M1 Task 6: idle teardown — the same orderly stop as shutdown_generation
  // (stdin EOF first), but the unloaded supervisor parks instead of ending.
  bool unload_generation(const std::shared_ptr<EngineInstance>& instance);
  bool wait_reaped(SupervisedProcess& process, std::chrono::milliseconds limit, const std::function<bool()>& cut = {});
  // Parked while unloaded; wakes on wake_for_request() latch or shutdown.
  void wait_asleep();
  void retire(const std::shared_ptr<EngineInstance>& instance, bool reaped);
  void sweep_retired();
  void finish();
  void note_shutdown_started() noexcept;
  void detach_leases() noexcept;
  void set_state(SupervisorState state);
  void set_state(SupervisorState state, std::shared_ptr<EngineInstance> current);
  void log_end(std::uint64_t generation, std::string_view kind);
  void log_status(std::uint64_t generation, const ExitStatus& status);

  SupervisorConfig config_;
  ProcessFactory factory_;
  std::shared_ptr<SupervisorClock> clock_;
  SupervisorLog log_;
  RestartTracker tracker_;
  SupervisorWake wake_;
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  SupervisorState state_{SupervisorState::stopped};
  std::shared_ptr<EngineInstance> current_;
  KillReason pending_kill_{KillReason::none};
  // Supervisor thread only: consecutive uncounted client-STOP restarts (SP3 D-2 bound).
  std::uint32_t uncounted_streak_{0};
  // Ended generations still borrowed by a Lease: destroyed by sweep_retired()
  // on the supervisor thread once the Lease lets go (spec §3.1; on Windows the
  // destructor terminates the Job and waits).
  std::vector<std::shared_ptr<EngineInstance>> retired_;
  std::vector<std::shared_ptr<EngineInstance>> abandoned_;
  std::atomic<std::uint64_t> generations_started_{0};
  std::map<std::string, std::string> ready_readings_;  // guarded by mutex_; set at READY
  std::uint64_t ready_serial_{0};                      // guarded by mutex_; set at READY (engine_sample)
  std::atomic<bool> shutdown_requested_{false};
  std::atomic<bool> slow_stop_{false};  // stop_engine(): a killed engine still ending is waited for
  std::atomic<bool> hurried_{false};    // hurry(): no more of that wait
  std::atomic<bool> engine_left_unreaped_{false};  // set by finish(): abandoned_ was not empty
  std::shared_ptr<LeaseLink> lease_link_;
  std::function<void()> on_stopped_;
  // Spec §3.7 bound: set when the supervisor thread starts the orderly
  // shutdown; finish() waits for main's shutdown() until then, else exits 1.
  std::chrono::steady_clock::time_point shutdown_deadline_{};
  bool main_acknowledged_{false};
  bool nnp_hint_logged_{false};  // kNoNewPrivilegesHint, at most once per supervisor
  // SP5 M1 Task 6 (idle_unload_s / lazy_load): the unload/lazy machinery. asleep_ and
  // wake_pending_ are guarded by mutex_; wake_pending_ latches a request that arrives in
  // the window between the idle decision and the park. idle_deadline_ is armed at READY
  // and on Lease release, cleared on acquire and at request start (in_flight_ counts the
  // serving path's pre-acquire window).
  bool asleep_{false};
  bool wake_pending_{false};
  std::optional<SupervisorClock::time_point> idle_deadline_;
  std::atomic<std::uint32_t> in_flight_{0};
  std::thread thread_;
};

// Maps the protocol's terminal failure to the §3.5 kill reason. An abandoned
// protocol (ignored STOP) is drain_failed, or client_stop_drain_failed when
// that STOP was sent with EngineStopCause::client_cancelled (SP3 D-2). This is
// the only production source of client_stop_drain_failed.
[[nodiscard]] KillReason kill_reason_for(const EngineProtocolClient& protocol) noexcept;

// The serving thread's one call per chat request: borrows the current
// generation, runs the request, and reports a generation left unusable.
// nullopt = no generation is ready (answer a generic 503 at once).
[[nodiscard]] std::optional<ChatExecutionOutcome> execute_with_lease(EngineSupervisor& supervisor, const ChatCompletionRequest& request,
                                                                     const TokenizerPack& tokenizer);

// What the production factory launches for every generation.
struct EngineLaunchPlan final {
  EngineCommand command;                           // command.environment is replaced when `environment` is set
  std::optional<EngineEnvironment> environment{};  // spec §3.9; temp_dir is the per-generation root
  // SP6 §4.9: derives the family's forced/suppressed rows; tests inject their own.
  FamilyEnvironmentHook family_hook{family_environment_rows};
  bool use_delegated_cgroup{false};                // Linux: cgroup.kill when a delegated cgroup is detected
};

// Production factory: an EngineStdioProcess started from `plan` per generation.
// `log` (default: stderr, like the supervisor's) receives the fixed literal
// "engine generation <n>: cgroup unavailable" when a detected delegated cgroup
// cannot hold a generation, which then runs without one, and the fixed literal
// "engine generation <n>: family environment refused" when the family's rows
// are refused: that generation is a spawn failure and nothing is spawned.
[[nodiscard]] ProcessFactory make_stdio_process_factory(EngineLaunchPlan plan, SupervisorLog log = {});

}  // namespace secure_frontend
