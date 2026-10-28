#include "secure_frontend/engine_supervisor.hpp"
#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/cgroup_scope.hpp"
#include "secure_frontend/engine_environment.hpp"
#include "secure_frontend/parity_table.hpp"
#include "secure_frontend/engine_stdio_process.hpp"
#include "secure_frontend/privilege_check.hpp"
#include "secure_frontend/shutdown_signal.hpp"
#include "secure_frontend/vision_supervisor.hpp"

#include "support/fake_engine_dump.hpp"
#include "support/fake_supervised_process.hpp"
#include "support/server_harness.hpp"
#include "support/test_paths.hpp"
#include "support/test_tokenizer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <cerrno>
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using secure_frontend::SupervisorState;

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

void supervisor_health_literals_per_state() {
  struct Row {
    SupervisorState state;
    bool healthy;
    std::string_view engine;
    std::string_view recovery;
  };
  const std::vector<Row> rows{
      {SupervisorState::starting, false, "starting", ""},     {SupervisorState::ready, true, "ready", ""},
      {SupervisorState::generating, true, "generating", ""},  {SupervisorState::stopping, false, "failed", "restarting"},
      {SupervisorState::backoff, false, "failed", "restarting"}, {SupervisorState::circuit_open, false, "failed", "exhausted"},
      {SupervisorState::shutting_down, false, "failed", ""},  {SupervisorState::stopped, false, "failed", ""},
      // SP5 M1 Task 6: unloaded is served-from-cold: healthy, and /health keeps the closed
      // literal set — "starting" names the load that the next request triggers (Ruling T6-e).
      {SupervisorState::unloaded, true, "starting", ""},
  };
  for (const auto& row : rows) {
    const auto health = secure_frontend::health_for(row.state);
    expect(health.state == row.state && health.healthy == row.healthy && health.engine == row.engine && health.recovery == row.recovery,
           "health literals for state " + std::to_string(static_cast<int>(row.state)));
  }
  secure_frontend::EngineSupervisor idle({}, [](std::uint64_t) -> secure_frontend::SpawnResult { return secure_frontend::SpawnFailure{"unused"}; });
  expect(idle.snapshot().state == SupervisorState::stopped && !idle.acquire().has_value(), "before start(): stopped, no lease");
}

secure_frontend::EngineLaunchPlan fake_plan(std::vector<std::string> arguments) {
  return {.command = {.executable = test_support::fake_engine_path(), .arguments = std::move(arguments), .environment = {}}};
}

// The Monitor's expert tier (owner 2026-10-23: "Every model should be showing the number of
// experts loaded along with memory used"). The engine prints its expert split in the startup
// INFO; the supervisor captures those readings at READY so they are available before any chat.
void supervisor_captures_startup_readings_at_ready() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) {
    child.push_line("INFO context=4096 kv=int8 expert_slots=384 expert_slots_primary=212");
    child.push_line("READY 4096 stop");
  });
  auto clock = std::make_shared<test_support::ManualClock>();
  secure_frontend::EngineSupervisor supervisor({}, fakes.factory(), clock);
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 5s), "generation 1 ready");
  const auto readings = supervisor.ready_readings();
  expect(readings.count("expert_slots") == 1U && readings.at("expert_slots") == "384" &&
             readings.count("expert_slots_primary") == 1U && readings.at("expert_slots_primary") == "212",
         "the startup INFO's expert split is captured at READY");
  supervisor.shutdown(5s);
}

void proc04_restart_backoff_then_circuit_open() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.end({false, 1U}); });
  auto clock = std::make_shared<test_support::ManualClock>();
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor({}, fakes.factory(), clock, log.sink());
  std::mutex seen_mutex;
  std::vector<std::string_view> recovery_in_backoff;
  clock->on_sleep([&] {
    std::lock_guard lock(seen_mutex);
    recovery_in_backoff.push_back(supervisor.snapshot().recovery);
  });
  expect(supervisor.start(), "the supervisor starts");
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 5s), "the 3rd consecutive failure opens the circuit");
  expect(clock->delays() == std::vector<std::chrono::milliseconds>{1000ms, 2000ms}, "backoff is exactly 1 s, then 2 s");
  expect(fakes.spawned() == 3U, "exactly three generations");
  {
    std::lock_guard lock(seen_mutex);
    expect(recovery_in_backoff == std::vector<std::string_view>{"restarting", "restarting"}, "backoff reports recovery=restarting");
  }
  const auto health = supervisor.snapshot();
  expect(!health.healthy && health.engine == "failed" && health.recovery == "exhausted", "circuit_open is failed/exhausted");
  expect(!supervisor.acquire().has_value(), "no lease in circuit_open");
  expect(log.contains("engine generation 1 ended: error_exit"), "each end is logged with its classification");
  supervisor.shutdown(5s);
}

void proc04_healthy_window_resets_failure_count() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  auto clock = std::make_shared<test_support::ManualClock>();
  secure_frontend::EngineSupervisor supervisor({}, fakes.factory(), clock);
  const auto fail_now = [&supervisor] {
    auto lease = supervisor.acquire();
    if (lease) supervisor.report_fatal(*lease, secure_frontend::KillReason::protocol_failed);
    return lease.has_value();
  };
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 5s), "generation 1 ready");
  clock->advance(301s);  // healthy for longer than the 300 s window
  expect(fail_now(), "lease on generation 1");
  expect(test_support::wait_generation_ready(supervisor, 2U, 5s), "generation 2 ready");
  expect(fail_now(), "generation 2 fails at once and counts on top");
  expect(test_support::wait_generation_ready(supervisor, 3U, 5s), "generation 3 ready");
  clock->advance(301s);
  expect(fail_now(), "generation 3 was healthy again");
  expect(test_support::wait_generation_ready(supervisor, 4U, 5s), "generation 4 ready: the circuit stayed closed");
  expect(clock->delays() == std::vector<std::chrono::milliseconds>{1000ms, 2000ms, 1000ms},
         "1 s, 2 s, then 1 s again after a healthy window reset the count");
  supervisor.shutdown(5s);
}

// SP5 M1 Task 6 (parity row idle_unload_s; pin serve/server.py:8081 and the idle thread
// :4100-4109 — unload is process teardown, and the next request restarts it). The supervisor
// tears the idle engine down (orderly: stdin EOF first, the fake exits 0 like the real engine),
// parks unloaded without counting a failure, and wakes for the request path with the same
// composed launch (no config reload).
void idle_unload_tears_down_then_respawns_on_next_request() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  auto clock = std::make_shared<test_support::ManualClock>();
  secure_frontend::SupervisorConfig config;
  config.idle_unload = std::chrono::seconds(60);
  config.monitor_poll = 20ms;  // fake children have no pidfd: deadline checks ride the poll slices
  secure_frontend::EngineSupervisor supervisor(config, fakes.factory(), clock);
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 5s), "generation 1 ready");
  {
    const auto lease = supervisor.acquire();
    expect(lease.has_value(), "a lease serves a request");
  }  // releasing the Lease re-arms the idle deadline at now + 60 s
  expect(supervisor.idle_unload_deadline().has_value(), "the idle deadline is armed after the request");
  clock->advance(61s);
  expect(supervisor.wait_for_state(SupervisorState::unloaded, 5s), "the idle deadline tears the engine down");
  expect(fakes.spawned() == 1U, "the unload does not respawn by itself");
  expect(!supervisor.acquire().has_value(), "no lease while unloaded");
  supervisor.wake_for_request();  // the GenerationGate does this for a request that finds the engine unloaded
  expect(test_support::wait_generation_ready(supervisor, 2U, 5s), "the request path gets generation 2 on the same composed launch");
  expect(supervisor.snapshot().state == SupervisorState::ready, "ready again");
  supervisor.shutdown(5s);
}

// SP5 M1 Task 6: the timer never fires while a generation is running or a request is in
// flight (the pin's idle thread only acts "without requests").
void idle_unload_never_fires_during_a_generation() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  auto clock = std::make_shared<test_support::ManualClock>();
  secure_frontend::SupervisorConfig config;
  config.idle_unload = std::chrono::seconds(60);
  config.monitor_poll = 20ms;
  secure_frontend::EngineSupervisor supervisor(config, fakes.factory(), clock);
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 5s), "generation 1 ready");
  auto lease = supervisor.acquire();
  expect(lease.has_value(), "the lease is held across the deadline");
  supervisor.note_request_started();  // a serving thread mid-request
  clock->advance(61s);
  expect(!supervisor.wait_for_state(SupervisorState::unloaded, 250ms),
         "the timer never fires while a generation runs or a request is in flight");
  supervisor.note_request_finished();
  lease.reset();  // the generation ends; the idle window restarts from here
  clock->advance(61s);
  expect(supervisor.wait_for_state(SupervisorState::unloaded, 5s), "it fires once the request and the generation are over");
  expect(fakes.spawned() == 1U, "still one generation until a request wakes it");
  supervisor.shutdown(5s);
}

// SP5 M1 Task 6 (parity row lazy_load; pin :7925 -> StrataEngine(lazy=True) :966-993: the child
// is NOT started; the first request path starts it — no engine argument composes for it,
// which is why this asserts the deferral, not argv (Ruling T6-b)).
void lazy_load_defers_the_first_spawn() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  secure_frontend::SupervisorConfig config;
  config.lazy_start = true;  // engine.lazy_load
  secure_frontend::EngineSupervisor supervisor(config, fakes.factory());
  expect(supervisor.start(), "the supervisor starts");
  expect(supervisor.wait_for_state(SupervisorState::unloaded, 5s), "a lazy start is unloaded, not starting");
  expect(fakes.spawned() == 0U, "no engine process until the first request");
  expect(!supervisor.acquire().has_value(), "no lease while unloaded");
  supervisor.wake_for_request();
  expect(test_support::wait_generation_ready(supervisor, 1U, 5s), "the first request's path spawns and gets READY");
  supervisor.shutdown(5s);
}

void proc04_recovers_after_transient_crash() {
  const auto dir = test_support::make_temp_dir("transient");
  secure_frontend::SupervisorConfig config;
  config.policy.backoff_initial = 50ms;
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor(
      config, secure_frontend::make_stdio_process_factory(fake_plan({"--fail-first=1", "--counter-file=" + test_support::utf8_path(dir / "count")})),
      std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 2U, 10s), "a one-off failure is recovered by generation 2");
  expect(log.contains("engine generation 1 ended: error_exit"), "generation 1's exit 1 is logged as error_exit");
  supervisor.shutdown(5s);
}

void proc04_missing_engine_opens_circuit_without_spinning() {
  const auto dir = test_support::make_temp_dir("missing-engine");
  auto clock = std::make_shared<test_support::ManualClock>();
  test_support::LogCapture log;
  const secure_frontend::EngineLaunchPlan plan{.command = {.executable = dir / "no-such-engine", .arguments = {}, .environment = {}}};
  secure_frontend::EngineSupervisor supervisor({}, secure_frontend::make_stdio_process_factory(plan), clock, log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 5s), "three spawn failures open the circuit");
  expect(supervisor.generations_started() == 3U, "exactly three attempts: no tight loop");
  expect(clock->delays() == std::vector<std::chrono::milliseconds>{1000ms, 2000ms}, "spawn failures back off like any failure");
  expect(log.contains("engine generation 1 ended: spawn_failed"), "logged as spawn_failed");
  const auto health = supervisor.snapshot();
  expect(health.engine == "failed" && health.recovery == "exhausted", "/health: failed / exhausted");
  supervisor.shutdown(5s);
}

// Review Focus 6: a client can make the engine answer ERR (SP0 keeps the
// per-request answer), and an ERR leaves the protocol failed, so the
// generation is replaced and counts as a failure. Three such requests in a
// row, inside the healthy window, open the circuit until the frontend restarts.
void proc04_client_err_lines_open_circuit_until_restart() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) {
    child.push_line("READY 4096 stop");
    child.push_line("ERR invalid request");  // the answer to the first GEN
  });
  auto clock = std::make_shared<test_support::ManualClock>();
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor({}, fakes.factory(), clock, log.sink());
  expect(supervisor.start(), "the supervisor starts");
  const auto tokenizer = test_support::byte_tokenizer();
  const secure_frontend::ChatCompletionRequest request{"fake-model", {{"user", "Hi"}}, 2U};
  for (std::uint64_t generation = 1U; generation <= 3U; ++generation) {
    expect(test_support::wait_generation_ready(supervisor, generation, 5s), "generation " + std::to_string(generation) + " ready");
    const auto outcome = secure_frontend::execute_with_lease(supervisor, request, tokenizer);
    expect(outcome && std::holds_alternative<secure_frontend::ChatExecutionError>(*outcome), "the ERR request fails for its own client");
  }
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 5s), "the third client-triggered ERR opens the circuit");
  expect(fakes.spawned() == 3U, "no fourth generation: only a frontend restart closes the circuit");
  expect(clock->delays() == std::vector<std::chrono::milliseconds>{1000ms, 2000ms}, "the first two ERR generations back off like any failure");
  expect(log.contains("engine generation 3 ended: killed_by_supervisor(protocol_failed)"), "each ERR generation is killed as protocol_failed");
  const auto health = supervisor.snapshot();
  expect(health.engine == "failed" && health.recovery == "exhausted", "/health: failed / exhausted until restart");
  expect(!supervisor.acquire().has_value(), "no lease for any later client");
  supervisor.shutdown(5s);
}

// Spec §3.1: only the supervisor thread destroys a generation (on Windows the
// destructor terminates the Job and waits). A Lease that outlives its
// generation's end must not make the serving thread drop the last reference.
void supervisor_retires_ended_generation_on_its_own_thread() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  auto clock = std::make_shared<test_support::ManualClock>();
  secure_frontend::EngineSupervisor supervisor({}, fakes.factory(), clock, test_support::LogCapture{}.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 5s), "generation 1 ready");
  auto lease = supervisor.acquire();
  expect(lease.has_value(), "lease on generation 1");
  if (!lease) {
    supervisor.shutdown(5s);
    return;
  }
  supervisor.report_fatal(*lease, secure_frontend::KillReason::protocol_failed);
  expect(test_support::wait_generation_ready(supervisor, 2U, 5s), "generation 2 ready while generation 1 is still leased");
  const auto first = fakes.child(1U);
  lease.reset();  // the serving thread lets go of the ended generation
  {
    std::lock_guard lock(first->mutex);
    expect(first->destroyed_on != std::this_thread::get_id(), "the serving thread did not destroy generation 1");
  }
  supervisor.shutdown(5s);
  std::lock_guard lock(first->mutex);
  expect(first->destroyed_on == first->spawned_on, "generation 1 was destroyed on the supervisor thread");
}

void proc04_monitor_detects_idle_crash() {
#if defined(__linux__)
  {
    // Prove the pidfd path, not just the 100 ms fallback: on Linux (5.3+; the
    // dev host and CI run 5.15+) a started engine must offer a monitor handle.
    secure_frontend::EngineStdioProcess probe;
    expect(!probe.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value(), "probe start");
    expect(probe.monitor_handle() != secure_frontend::kNoMonitorHandle,
           "Linux: the monitor waits on a pidfd (pidfd_open), not the fallback poll");
    (void)probe.terminate(250ms);
  }
#endif
  secure_frontend::SupervisorConfig config;
  config.policy.max_consecutive_failures = 1U;  // one end opens the circuit: nothing respawns during the check
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor(
      config, secure_frontend::make_stdio_process_factory(fake_plan({"--crash-after-ready=KILL", "--crash-delay-ms=300"})),
      std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(supervisor.wait_for_state(SupervisorState::ready, 10s), "ready");
  const auto ready_seen = std::chrono::steady_clock::now();
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 3s), "an idle death is noticed with no request");
  const auto noticed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ready_seen);
  expect(noticed <= 1300ms, "within 1 s of the crash (which is 300 ms after READY); took " + std::to_string(noticed.count()) + " ms");
#if defined(_WIN32)
  expect(log.contains("engine generation 1 ended: error_exit"), "the fake's TerminateProcess(137) is an error exit");
#else
  expect(log.contains("engine generation 1 ended: killed_external"), "a SIGKILL we did not send is killed_external");
#endif
  supervisor.shutdown(5s);
}

void proc04_monitor_fallback_without_pidfd() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  secure_frontend::SupervisorConfig config;
  config.policy.max_consecutive_failures = 1U;
  config.monitor_poll = 20ms;
  secure_frontend::EngineSupervisor supervisor(config, fakes.factory());
  expect(supervisor.start(), "the supervisor starts");
  expect(supervisor.wait_for_state(SupervisorState::ready, 5s), "ready");
  const auto started = std::chrono::steady_clock::now();
  fakes.child(1U)->end({false, 0U});  // no monitor handle: only the fallback poll can see this
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 2s), "the fallback poll notices the exit");
  expect(std::chrono::steady_clock::now() - started < 500ms, "within a few poll intervals");
  supervisor.shutdown(5s);
}

// Spec §3.3 / §3.1: a wake that is not shutdown (here: a Lease on the ended
// generation let go during the backoff) neither shortens the delay nor waits
// for it to sweep that generation.
void proc04_backoff_survives_stale_wake() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  secure_frontend::EngineSupervisor supervisor({}, fakes.factory(), std::make_shared<secure_frontend::SteadySupervisorClock>(),
                                               test_support::LogCapture{}.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 5s), "generation 1 ready");
  auto lease = supervisor.acquire();
  expect(lease.has_value(), "lease on generation 1");
  if (!lease) {
    supervisor.shutdown(5s);
    return;
  }
  supervisor.report_fatal(*lease, secure_frontend::KillReason::protocol_failed);
  expect(supervisor.wait_for_state(SupervisorState::backoff, 5s), "backoff after the first failure");
  const auto backoff_seen = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(100ms);
  lease.reset();  // wakes the supervisor mid-backoff
  const auto first = fakes.child(1U);
  const auto swept_by = std::chrono::steady_clock::now() + 300ms;
  bool swept = false;
  while (!swept && std::chrono::steady_clock::now() < swept_by) {
    {
      std::lock_guard lock(first->mutex);
      swept = first->destroyed_on != std::thread::id{};
    }
    if (!swept) std::this_thread::sleep_for(5ms);
  }
  expect(swept, "the released generation is swept during the backoff, not after it");
  expect(supervisor.generations_started() == 1U, "the wake did not end the backoff early");
  expect(test_support::wait_generation_ready(supervisor, 2U, 5s), "generation 2 after the backoff");
  const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - backoff_seen);
  expect(waited >= 950ms, "the full 1 s backoff elapsed; took " + std::to_string(waited.count()) + " ms");
  supervisor.shutdown(5s);
}

void proc04_fatal_prefix_kills_engine() {
  // Protocol level: each prefix fails the generation; the trailing DONE is never consumed.
  for (std::size_t index = 0; index < secure_frontend::kEngineFatalPrefixes.size(); ++index) {
    auto child = std::make_shared<test_support::FakeChild>();
    test_support::FakeChildTransport transport(child);
    child->push_line("READY 4096 stop");
    secure_frontend::EngineProtocolClient protocol(transport);
    secure_frontend::EngineReady ready{};
    expect(!protocol.await_ready(1s, ready).has_value(), "READY");
    expect(!protocol.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(), "GEN");
    child->push_line("T 1");
    child->push_line(std::string(secure_frontend::kEngineFatalPrefixes[index]) + "12");
    child->push_line("DONE stop");
    secure_frontend::EngineGeneration generation{};
    const auto error = protocol.read_generation(1s, generation);
    expect(error && error->code == secure_frontend::EngineProtocolErrorCode::fatal_engine_line, "prefix " + std::to_string(index) + " is fatal");
    expect(protocol.closed() && secure_frontend::kill_reason_for(protocol) == secure_frontend::KillReason::fatal_line, "maps to fatal_line");
    std::lock_guard lock(child->mutex);
    expect(child->reads.size() == 1U && child->reads.front().line == "DONE stop", "the trailing DONE is not accepted");
  }
  // Supervisor level: the generation is killed and replaced.
  const auto tokenizer = test_support::byte_tokenizer();
  const secure_frontend::ChatCompletionRequest request{"fake-model", {{"user", "Hi"}}, 2U};
  for (int index = 0; index < 4; ++index) {
    secure_frontend::SupervisorConfig config;
    config.policy.backoff_initial = 20ms;
    test_support::LogCapture log;
    secure_frontend::EngineSupervisor supervisor(
        config, secure_frontend::make_stdio_process_factory(fake_plan({"--fatal-line=" + std::to_string(index)})),
        std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
    expect(supervisor.start(), "the supervisor starts");
    expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "generation 1 ready");
    const auto outcome = secure_frontend::execute_with_lease(supervisor, request, tokenizer);
    expect(outcome && std::holds_alternative<secure_frontend::ChatExecutionError>(*outcome), "the request fails; no result after a fatal line");
    expect(test_support::wait_generation_ready(supervisor, 2U, 10s), "the engine is replaced");
    expect(log.contains("engine generation 1 ended: killed_by_supervisor(fatal_line)"), "killed as fatal_line, prefix " + std::to_string(index));
    supervisor.shutdown(5s);
  }
}

// Spec §3.8: a fatal verify line before READY is a fatal_line kill, not a
// generic protocol failure.
void proc04_fatal_line_before_ready_kills_as_fatal_line() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) {
    child.push_line(std::string(secure_frontend::kEngineFatalPrefixes.front()) + "3");
  });
  secure_frontend::SupervisorConfig config;
  config.policy.max_consecutive_failures = 1U;  // one end opens the circuit: nothing respawns
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor(config, fakes.factory(), std::make_shared<test_support::ManualClock>(), log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 5s), "the generation ends before READY");
  expect(log.contains("engine generation 1 ended: killed_by_supervisor(fatal_line)"), "killed as fatal_line, not protocol_failed");
  supervisor.shutdown(5s);
}

void proc04_ready_timeout_kills_and_counts() {
  secure_frontend::SupervisorConfig config;
  config.policy.ready_timeout = 200ms;
  config.policy.max_consecutive_failures = 2U;
  config.policy.backoff_initial = 20ms;
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor(config, secure_frontend::make_stdio_process_factory(fake_plan({"--hang-before-ready"})),
                                               std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 5s), "two READY timeouts open the circuit: each one counts");
  expect(supervisor.generations_started() == 2U, "two generations");
  expect(log.contains("engine generation 1 ended: killed_by_supervisor(ready_timeout)"), "killed as ready_timeout");
  supervisor.shutdown(5s);
}

void proc04_drain_failure_kills_and_restarts() {
  secure_frontend::SupervisorConfig config;
  config.chat_limits.generation_timeout = 200ms;
  config.chat_limits.stop_drain_timeout = 200ms;
  config.policy.backoff_initial = 20ms;
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor(config, secure_frontend::make_stdio_process_factory(fake_plan({"--hang-in-gen"})),
                                               std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "generation 1 ready");
  const auto tokenizer = test_support::byte_tokenizer();
  const secure_frontend::ChatCompletionRequest request{"fake-model", {{"user", "Hi"}}, 2U};
  const auto outcome = secure_frontend::execute_with_lease(supervisor, request, tokenizer);
  expect(outcome && std::holds_alternative<secure_frontend::ChatExecutionError>(*outcome), "the request fails after STOP is ignored");
  expect(test_support::wait_generation_ready(supervisor, 2U, 10s), "a new generation replaces the wedged one");
  expect(log.contains("engine generation 1 ended: killed_by_supervisor(drain_failed)"), "killed as drain_failed");
  supervisor.shutdown(5s);
}

// SP3 D-2: the cause of the STOP travels with the protocol to kill_reason_for.
// A drain that fails after a client-requested STOP maps to
// client_stop_drain_failed; after the frontend's own timeout STOP (the
// default, and what ChatExecutionService sends) it stays drain_failed. The
// cause belongs to one generation request: the next GEN forgets it.
void sp3_d2_kill_reason_follows_stop_cause() {
  using secure_frontend::EngineStopCause;
  using secure_frontend::KillReason;
  const auto drain_fails_after = [](const std::optional<EngineStopCause> cause, const bool client_stop_drained_first) {
    auto child = std::make_shared<test_support::FakeChild>();
    child->push_line("READY 4096 stop");
    test_support::FakeChildTransport transport(child);
    secure_frontend::EngineProtocolClient protocol(transport);
    secure_frontend::EngineReady ready{};
    if (protocol.await_ready(1s, ready).has_value()) return KillReason::none;
    if (client_stop_drained_first) {
      if (protocol.begin_generation(2U, {1U, 2U}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value() || protocol.stop(EngineStopCause::client_cancelled).has_value()) return KillReason::none;
      child->push_line("DONE 0 2 1 1 cancel");
      secure_frontend::EngineGeneration drained{};
      if (protocol.read_generation(1s, drained).has_value()) return KillReason::none;
    }
    if (protocol.begin_generation(2U, {1U, 2U}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value()) return KillReason::none;
    if ((cause ? protocol.stop(*cause) : protocol.stop()).has_value()) return KillReason::none;
    secure_frontend::EngineGeneration drained{};
    const auto drain = protocol.read_generation(20ms, drained);
    if (!drain || drain->code != secure_frontend::EngineProtocolErrorCode::timeout) return KillReason::none;
    protocol.abandon();
    return secure_frontend::kill_reason_for(protocol);
  };
  expect(drain_fails_after(EngineStopCause::client_cancelled, false) == KillReason::client_stop_drain_failed,
         "a failed drain after a client STOP is client_stop_drain_failed");
  expect(drain_fails_after(EngineStopCause::generation_timeout, false) == KillReason::drain_failed,
         "a failed drain after a timeout STOP stays drain_failed");
  expect(drain_fails_after(std::nullopt, false) == KillReason::drain_failed, "stop() defaults to the timeout cause");
  expect(drain_fails_after(std::nullopt, true) == KillReason::drain_failed,
         "an earlier request's client STOP does not carry over to the next GEN");
  expect(secure_frontend::kill_reason_literal(KillReason::client_stop_drain_failed) == "client_stop_drain_failed",
         "fixed log literal");
}

// SP3 P6 cancel (SP3 spec §2.6): STOP for a gone client, drain within the
// bound, report through the lease. The fake never answers GEN and ignores
// STOP, so the drain times out and the protocol is abandoned.
bool client_stop_drain_fails(secure_frontend::EngineSupervisor& supervisor) {
  auto lease = supervisor.acquire();
  if (!lease) return false;
  auto& protocol = lease->protocol();
  if (protocol.begin_generation(2U, {1U, 2U}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value()) return false;
  if (protocol.stop(secure_frontend::EngineStopCause::client_cancelled).has_value()) return false;
  secure_frontend::EngineGeneration drained{};
  const auto drain = protocol.read_generation(20ms, drained);
  if (!drain || drain->code != secure_frontend::EngineProtocolErrorCode::timeout) return false;
  protocol.abandon();
  supervisor.report_fatal(*lease, secure_frontend::kill_reason_for(protocol));
  return true;
}

void counted_failure(secure_frontend::EngineSupervisor& supervisor, const std::uint64_t generation) {
  expect(test_support::wait_generation_ready(supervisor, generation, 5s), "generation " + std::to_string(generation) + " ready");
  auto lease = supervisor.acquire();
  expect(lease.has_value(), "lease on generation " + std::to_string(generation));
  if (lease) supervisor.report_fatal(*lease, secure_frontend::KillReason::protocol_failed);
}

void client_stop_failure(secure_frontend::EngineSupervisor& supervisor, const std::uint64_t generation) {
  const std::string name = "generation " + std::to_string(generation);
  expect(test_support::wait_generation_ready(supervisor, generation, 5s), name + " ready");
  expect(client_stop_drain_fails(supervisor), name + ": the drain after a client STOP fails");
}

// One successful GEN -> DONE on the current generation, through a lease.
void successful_done(secure_frontend::EngineSupervisor& supervisor, const test_support::FakeFactory& fakes, const std::uint64_t generation) {
  expect(test_support::wait_generation_ready(supervisor, generation, 5s), "generation " + std::to_string(generation) + " ready");
  auto lease = supervisor.acquire();
  expect(lease.has_value(), "lease for the DONE");
  if (!lease) return;
  expect(!lease->protocol().begin_generation(2U, {1U, 2U}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(), "GEN accepted");
  fakes.child(generation)->push_line("DONE 0 2 1 1 length");
  secure_frontend::EngineGeneration done{};
  expect(!lease->protocol().read_generation(1s, done).has_value(), "the generation ends with a successful DONE");
}

static_assert(secure_frontend::config_defaults::kMaxConsecutiveUncountedRestarts == 3U, "the tests below assume the ruled bound of 3");

// SP3 D-2 (risk R4): a client that disconnects during a long prefill must not
// open the circuit. Each failed drain after a client STOP kills and replaces
// the generation exactly as drain_failed does, but records no failure: no
// backoff delay, and the count is still 0 afterwards (three counted failures
// are then needed, with the first delays 1 s, 2 s). Three in a row is the
// ruled bound (kMaxConsecutiveUncountedRestarts).
void sp3_d2_client_stop_drain_failure_restarts_uncounted() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  auto clock = std::make_shared<test_support::ManualClock>();
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor({}, fakes.factory(), clock, log.sink());
  expect(supervisor.start(), "the supervisor starts");
  for (std::uint64_t generation = 1U; generation <= 3U; ++generation) client_stop_failure(supervisor, generation);
  expect(test_support::wait_generation_ready(supervisor, 4U, 5s), "generation 4 ready: three client-STOP drain failures never opened the circuit");
  expect(clock->delays().empty(), "no backoff: the failures were not counted");
  for (std::uint64_t generation = 1U; generation <= 3U; ++generation) {
    const auto child = fakes.child(generation);
    expect(child && child->kills >= 1, "generation " + std::to_string(generation) + " was killed");
    expect(log.contains("engine generation " + std::to_string(generation) + " ended: killed_by_supervisor(client_stop_drain_failed)"),
           "generation " + std::to_string(generation) + " logged with the fixed client-STOP literal");
  }
  for (std::uint64_t generation = 4U; generation <= 6U; ++generation) counted_failure(supervisor, generation);
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 5s), "three counted failures open the circuit");
  expect(clock->delays() == std::vector<std::chrono::milliseconds>{1000ms, 2000ms}, "the count started from 0: 1 s, 2 s, then open");
  expect(fakes.spawned() == 6U, "exactly six generations");
  supervisor.shutdown(5s);
}

// Fix round 1, finding 1: an uncounted end still honours the healthy window
// (spec §3.3/§3.8). Two counted failures, then a generation healthy past the
// window ends by a client-STOP drain failure: the count is back to 0, so three
// more counted failures are needed to open the circuit.
void sp3_d2_uncounted_end_resets_count_after_healthy_window() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  auto clock = std::make_shared<test_support::ManualClock>();
  secure_frontend::EngineSupervisor supervisor({}, fakes.factory(), clock);
  expect(supervisor.start(), "the supervisor starts");
  counted_failure(supervisor, 1U);
  counted_failure(supervisor, 2U);
  expect(test_support::wait_generation_ready(supervisor, 3U, 5s), "generation 3 ready");
  clock->advance(301s);  // healthy for longer than the 300 s window
  client_stop_failure(supervisor, 3U);
  for (std::uint64_t generation = 4U; generation <= 5U; ++generation) counted_failure(supervisor, generation);
  expect(test_support::wait_generation_ready(supervisor, 6U, 5s), "generation 6 ready: the healthy window reset the count");
  counted_failure(supervisor, 6U);
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 5s), "the third counted failure after the reset opens the circuit");
  expect(clock->delays() == std::vector<std::chrono::milliseconds>{1000ms, 2000ms, 1000ms, 2000ms},
         "1 s, 2 s; uncounted reset; 1 s, 2 s, then open");
  supervisor.shutdown(5s);
}

// Fix round 1, finding 2 (controller ruling): the exemption is bounded. After
// kMaxConsecutiveUncountedRestarts uncounted restarts with no successful DONE
// between them, a client-STOP drain failure counts like drain_failed, so an
// engine that always ignores STOP still reaches circuit_open.
void sp3_d2_uncounted_streak_is_bounded() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  auto clock = std::make_shared<test_support::ManualClock>();
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor({}, fakes.factory(), clock, log.sink());
  expect(supervisor.start(), "the supervisor starts");
  for (std::uint64_t generation = 1U; generation <= 3U; ++generation) client_stop_failure(supervisor, generation);
  client_stop_failure(supervisor, 4U);
  expect(test_support::wait_generation_ready(supervisor, 5U, 5s), "generation 5 ready");
  expect(clock->delays() == std::vector<std::chrono::milliseconds>{1000ms}, "the 4th in a row counts: 1 s");
  client_stop_failure(supervisor, 5U);
  client_stop_failure(supervisor, 6U);
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 5s), "an engine that keeps ignoring STOP opens the circuit");
  expect(clock->delays() == std::vector<std::chrono::milliseconds>{1000ms, 2000ms}, "1 s, 2 s, then open");
  expect(fakes.spawned() == 6U, "exactly six generations");
  expect(log.contains("engine generation 6 ended: killed_by_supervisor(client_stop_drain_failed)"), "same fixed literal when counted");
  supervisor.shutdown(5s);
}

// Fix round 1, finding 2: a successful DONE resets the streak. Two uncounted,
// then a generation that completes a request before its client-STOP drain
// failure, then two more: five uncounted in all, no backoff.
void sp3_d2_done_resets_uncounted_streak() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  auto clock = std::make_shared<test_support::ManualClock>();
  secure_frontend::EngineSupervisor supervisor({}, fakes.factory(), clock);
  expect(supervisor.start(), "the supervisor starts");
  client_stop_failure(supervisor, 1U);
  client_stop_failure(supervisor, 2U);
  successful_done(supervisor, fakes, 3U);
  client_stop_failure(supervisor, 3U);
  client_stop_failure(supervisor, 4U);
  client_stop_failure(supervisor, 5U);
  expect(test_support::wait_generation_ready(supervisor, 6U, 5s), "generation 6 ready");
  expect(clock->delays().empty(), "the DONE on generation 3 reset the streak: none of the five counted");
  supervisor.shutdown(5s);
}

// SP3 D-2, the counted side: the frontend's own timeout STOP (ChatExecutionService)
// followed by a failed drain is drain_failed and counts, as V7
// (proc04_drain_failure_kills_and_restarts) requires; three open the circuit.
void sp3_d2_timeout_stop_drain_failure_still_counts() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  auto clock = std::make_shared<test_support::ManualClock>();
  test_support::LogCapture log;
  secure_frontend::SupervisorConfig config;
  config.chat_limits.generation_timeout = 20ms;
  config.chat_limits.stop_drain_timeout = 20ms;
  secure_frontend::EngineSupervisor supervisor(config, fakes.factory(), clock, log.sink());
  expect(supervisor.start(), "the supervisor starts");
  const auto tokenizer = test_support::byte_tokenizer();
  const secure_frontend::ChatCompletionRequest request{"fake-model", {{"user", "Hi"}}, 2U};
  for (std::uint64_t generation = 1U; generation <= 3U; ++generation) {
    expect(test_support::wait_generation_ready(supervisor, generation, 5s), "generation " + std::to_string(generation) + " ready");
    const auto outcome = secure_frontend::execute_with_lease(supervisor, request, tokenizer);
    expect(outcome && std::holds_alternative<secure_frontend::ChatExecutionError>(*outcome), "the timed-out request fails");
  }
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 5s), "three timeout-STOP drain failures open the circuit");
  expect(clock->delays() == std::vector<std::chrono::milliseconds>{1000ms, 2000ms}, "each one counts: 1 s, 2 s, then open");
  expect(log.contains("engine generation 3 ended: killed_by_supervisor(drain_failed)"), "logged as drain_failed");
  supervisor.shutdown(5s);
}

// SP3 D-2 exempts only our own kill: a generation that ended by itself is a
// failure whatever reason the serving side reported.
void sp3_d2_client_stop_counts_when_engine_ended_itself() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  auto clock = std::make_shared<test_support::ManualClock>();
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor({}, fakes.factory(), clock, log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 5s), "generation 1 ready");
  {
    auto lease = supervisor.acquire();
    expect(lease.has_value(), "lease on generation 1");
    if (lease) {
      fakes.child(1U)->end({false, 1U});  // the engine exits 1 on its own
      supervisor.report_fatal(*lease, secure_frontend::KillReason::client_stop_drain_failed);
    }
  }
  expect(test_support::wait_generation_ready(supervisor, 2U, 5s), "generation 2 ready");
  expect(clock->delays() == std::vector<std::chrono::milliseconds>{1000ms}, "the engine's own end counts");
  expect(log.contains("engine generation 1 ended: error_exit"), "classified by its own status");
  supervisor.shutdown(5s);
}

std::size_t open_handle_count() {
#if defined(_WIN32)
  DWORD count = 0;
  (void)GetProcessHandleCount(GetCurrentProcess(), &count);
  return count;
#else
  std::size_t count = 0;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd", error)) {
    (void)entry;
    ++count;  // includes the iterator's own descriptor: the same on every call
  }
  return count;
#endif
}

#if defined(_WIN32)
// Diagnostics only: how many handles of each object type this process holds
// (every valid handle value, typed by NtQueryObject ObjectTypeInformation,
// which never blocks, unlike ObjectNameInformation on a pipe).
std::map<std::string, int> open_handle_types() {
  using NtQueryObjectFn = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto query =
      ntdll == nullptr ? nullptr : reinterpret_cast<NtQueryObjectFn>(reinterpret_cast<void (*)()>(GetProcAddress(ntdll, "NtQueryObject")));
  struct TypeName {
    USHORT length;
    USHORT maximum_length;
    PWSTR buffer;
  };
  std::map<std::string, int> types;
  alignas(8) std::array<unsigned char, 4096> storage{};
  for (std::uintptr_t value = 4U; value < 0x10000U; value += 4U) {
    HANDLE handle = reinterpret_cast<HANDLE>(value);
    DWORD flags = 0;
    if (GetHandleInformation(handle, &flags) == 0) continue;
    std::string name = "?";
    ULONG returned = 0;
    if (query != nullptr && query(handle, 2 /* ObjectTypeInformation */, storage.data(), static_cast<ULONG>(storage.size()), &returned) >= 0) {
      TypeName type{};
      std::memcpy(&type, storage.data(), sizeof(type));
      name.clear();
      for (USHORT index = 0; type.buffer != nullptr && index < type.length / sizeof(wchar_t); ++index) {
        name.push_back(static_cast<char>(type.buffer[index] < 0x80 ? type.buffer[index] : L'?'));
      }
    }
    ++types[name];
  }
  return types;
}
#endif

void proc04_no_leak_across_restarts() {
  secure_frontend::SupervisorConfig config;
  config.policy.max_consecutive_failures = 100U;
  config.policy.backoff_initial = 1ms;
  config.policy.backoff_max = 1ms;
  secure_frontend::EngineSupervisor supervisor(config, secure_frontend::make_stdio_process_factory(fake_plan({})));
  expect(supervisor.start(), "the supervisor starts");
  std::optional<std::size_t> baseline;
#if defined(_WIN32)
  // Ruling (winfix): on Windows the baseline is taken at generation 2, after
  // one restart. Real Windows showed exactly +1 handle from generation 2 on and
  // never more (88 vs 87 at generations 2..11, CI run 38598235525): a one-time
  // allocation on the first restart path (OS/CRT state initialised on first
  // use), not a leak. Ten restarts still each compare 3..11 against it, so any
  // per-restart leak still fails. The handles that appeared between
  // generations 1 and 2 are printed by type to name the one-time allocation.
  constexpr std::uint64_t kBaselineGeneration = 2U;
  std::map<std::string, int> first_generation_handles;
  std::string new_at_generation_2;  // printed after the loop: a first write to stdout must not land inside the measurement
#else
  constexpr std::uint64_t kBaselineGeneration = 1U;
#endif
  for (std::uint64_t generation = 1U; generation <= 11U; ++generation) {  // 10 forced restarts
    expect(test_support::wait_generation_ready(supervisor, generation, 10s), "generation " + std::to_string(generation) + " ready");
    const std::size_t handles = open_handle_count();
#if defined(_WIN32)
    if (generation == 1U) first_generation_handles = open_handle_types();
    if (generation == 2U) {
      std::string added;
      for (const auto& [type, count] : open_handle_types()) {
        const int before = first_generation_handles.contains(type) ? first_generation_handles.at(type) : 0;
        if (count != before) added += " " + type + " " + std::to_string(before) + "->" + std::to_string(count);
      }
      new_at_generation_2 = added.empty() ? std::string(" none") : added;
    }
#endif
    if (generation == kBaselineGeneration) baseline = handles;
    if (baseline) {
      expect(handles == *baseline, "descriptor/handle count stable at generation " + std::to_string(generation) + ": " +
                                       std::to_string(handles) + " vs " + std::to_string(*baseline));
    }
#if !defined(_WIN32)
    const auto children = test_support::children_of(static_cast<long>(getpid()));
    expect(children.size() == 1U, "exactly one live engine, saw " + std::to_string(children.size()));
    for (const long child : children) expect(test_support::process_alive(child), "no zombie among the children");
#endif
    auto lease = supervisor.acquire();
    expect(lease.has_value(), "lease");
    if (!lease) break;
    auto* stdio = dynamic_cast<secure_frontend::EngineStdioProcess*>(&lease->process());
    expect(stdio != nullptr && stdio->pending_bytes_for_tests() == 0U, "a new generation starts with an empty stdout buffer");
    supervisor.report_fatal(*lease, secure_frontend::KillReason::protocol_failed);
  }
#if defined(_WIN32)
  std::cout << "handle types that changed from generation 1 to 2 (one-time, before the baseline):" << new_at_generation_2 << '\n';
#endif
  supervisor.shutdown(5s);
}

void proc04_unreaped_child_opens_circuit() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) {
    child.reapable = false;  // the kill lands, but the child can never be reaped
    child.push_line("READY 4096 stop");
  });
  secure_frontend::SupervisorConfig config;
  config.policy.reap_timeout = 100ms;
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor(config, fakes.factory(), std::make_shared<test_support::ManualClock>(), log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(supervisor.wait_for_state(SupervisorState::ready, 5s), "ready");
  {
    auto lease = supervisor.acquire();
    expect(lease.has_value(), "lease");
    if (lease) supervisor.report_fatal(*lease, secure_frontend::KillReason::protocol_failed);
  }
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 2s), "an unreaped child opens the circuit");
  expect(fakes.spawned() == 1U, "no respawn while the child is unreaped, although failures are below the limit");
  expect(supervisor.snapshot().recovery == "exhausted", "recovery=exhausted");
  expect(log.contains("engine generation 1 ended: unreaped"), "logged as unreaped");
  supervisor.shutdown(5s);
  // C4 (final review I2): the abandoned generation is still an engine left.
  expect(supervisor.engine_left_unreaped(), "shutdown reports the engine left behind");
  expect(log.contains(secure_frontend::kEngineLeftUnreapedLine), "the fixed-literal engine-left line is logged");
}

// Final review M1: an engine that ends before READY on its own (exit 1) is
// its own end even when its status is not yet reapable at end_generation's
// first check, so the supervisor's kill lands on a dead child. The NNP hint
// keys on that classification, not on whether a kill was sent.
void proc06_nnp_hint_when_exit_status_lags() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) {
    child.end({false, 1U});
    child.unreapable_polls = 1;
  });
  secure_frontend::SupervisorConfig config;
  config.no_new_privileges_hint = true;
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor(config, fakes.factory(), std::make_shared<test_support::ManualClock>(), log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 5s), "three early exits open the circuit");
  expect(log.contains("engine generation 1 ended: error_exit"), "the engine's own exit 1 is classified error_exit");
  expect(log.contains(secure_frontend::kNoNewPrivilegesHint), "the NNP hint is logged for an engine that ended before READY");
  supervisor.shutdown(5s);
}

// C4 (final review I2): "exit 0 means no engine left". A generation that the
// orderly shutdown cannot reap is reported (fixed-literal line), so main
// returns 1 instead of 0.
void supervisor_unreaped_at_shutdown_reports_engine_left() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) {
    child.reapable = false;  // EOF and the kills land, but the child can never be reaped
    child.push_line("READY 4096 stop");
  });
  secure_frontend::SupervisorConfig config;
  config.shutdown_stdin_wait = 50ms;
  config.shutdown_term_wait = 50ms;
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor(config, fakes.factory(), std::make_shared<test_support::ManualClock>(), log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 5s), "generation 1 ready");
  expect(!supervisor.engine_left_unreaped(), "nothing reported while serving");
  supervisor.shutdown(5s);
  expect(supervisor.snapshot().state == SupervisorState::stopped, "the shutdown still completes within its bound");
  expect(log.contains("engine generation 1 ended: unreaped"), "the generation is logged as unreaped");
  expect(supervisor.engine_left_unreaped(), "shutdown reports the engine left behind");
  expect(log.contains(secure_frontend::kEngineLeftUnreapedLine), "the fixed-literal engine-left line is logged");
}

#if !defined(_WIN32)
// The sweep reaped `pid` (not merely killed it): it is no longer our child,
// not even as a zombie. Hygiene: this process is the subreaper, so a
// grandchild the sweep missed is ours; kill it only while it is alive (a
// recycled pid is never signalled) and collect it.
void expect_swept_and_reaped(const long pid) {
  int status = 0;
  const pid_t waited = waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
  expect(waited == -1 && errno == ECHILD, "the swept grandchild was reaped, not left a zombie");
  if (waited == 0 && test_support::process_alive(pid)) test_support::kill_process(pid);
  if (waited == 0) (void)waitpid(static_cast<pid_t>(pid), &status, 0);
}

void proc05_setsid_grandchild_is_reaped() {
  const auto dir = test_support::make_temp_dir("c2");
  const auto pidfile = dir / "grandchild.pid";
  secure_frontend::SupervisorConfig config;
  config.policy.max_consecutive_failures = 1U;
  secure_frontend::EngineSupervisor supervisor(
      config, secure_frontend::make_stdio_process_factory(
                  fake_plan({"--spawn-grandchild", "--grandchild-setsid", "--grandchild-pidfile=" + test_support::utf8_path(pidfile)})));
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "ready");
  const auto grandchild = test_support::read_pidfile(pidfile, 5s);
  expect(grandchild.has_value(), "the escaping grandchild started");
  if (!grandchild) return;
  {
    auto lease = supervisor.acquire();
    if (lease) supervisor.report_fatal(*lease, secure_frontend::KillReason::protocol_failed);
  }
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 5s), "the generation ended");
  expect(test_support::wait_until_gone(*grandchild, 2s), "a setsid grandchild outside the engine's group is swept");
  expect_swept_and_reaped(*grandchild);
  supervisor.shutdown(5s);
}

void proc05_setsid_grandchild_is_reaped_at_shutdown() {
  const auto dir = test_support::make_temp_dir("c2-shutdown");
  const auto pidfile = dir / "grandchild.pid";
  secure_frontend::EngineSupervisor supervisor(
      {}, secure_frontend::make_stdio_process_factory(
              fake_plan({"--spawn-grandchild", "--grandchild-setsid", "--grandchild-pidfile=" + test_support::utf8_path(pidfile)})));
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "ready");
  const auto grandchild = test_support::read_pidfile(pidfile, 5s);
  expect(grandchild.has_value(), "the escaping grandchild started");
  if (!grandchild) return;
  expect(test_support::process_alive(*grandchild), "precondition: the grandchild is alive before shutdown");
  supervisor.shutdown(5s);
  expect(test_support::wait_until_gone(*grandchild, 2s), "a setsid grandchild alive at shutdown is swept");
  expect_swept_and_reaped(*grandchild);
}

// C5 (spec §3.7d): with a delegated cgroup v2 each generation runs in its own
// child cgroup, and cgroup.kill ends a setsid escape with the orphan sweep off.
void proc05_cgroup_kill_when_delegated() {
  const auto cgroups = secure_frontend::EngineCgroups::detect();
  if (!cgroups) {
    test_support::skip("no delegated cgroup v2 is writable here; run under `systemd-run --user --scope -p Delegate=yes` to exercise it");
  }
  const auto dir = test_support::make_temp_dir("c5");
  const auto pidfile = dir / "grandchild.pid";
  secure_frontend::SupervisorConfig config;
  config.policy.max_consecutive_failures = 1U;
  config.sweep_orphans = false;  // prove that cgroup.kill does the work, not the subreaper sweep
  auto plan = fake_plan({"--spawn-grandchild", "--grandchild-setsid", "--grandchild-pidfile=" + test_support::utf8_path(pidfile)});
  plan.use_delegated_cgroup = true;
  secure_frontend::EngineSupervisor supervisor(config, secure_frontend::make_stdio_process_factory(plan));
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "ready");
  const auto grandchild = test_support::read_pidfile(pidfile, 5s);
  expect(grandchild.has_value(), "grandchild started");
  const auto generation = cgroups->generation_directory(1U);
  if (grandchild) {
    expect(test_support::cgroup_directory_of(*grandchild) == generation, "the setsid grandchild runs in the generation's cgroup");
  }
  {
    auto lease = supervisor.acquire();
    if (lease) supervisor.report_fatal(*lease, secure_frontend::KillReason::protocol_failed);
  }
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 5s), "the generation ended");
  if (grandchild) {
    expect(test_support::wait_until_gone(*grandchild, 2s), "cgroup.kill ends a setsid grandchild with no sweep");
    test_support::kill_process(*grandchild);
  }
  expect(!std::filesystem::exists(generation), "the generation's cgroup is removed");
  supervisor.shutdown(5s);
}
#endif

#if defined(_WIN32)
// Supervisor-level smoke for the Windows launch settings (spec §3.7): the fake
// runs with --default-error-mode, so it has only the error mode it inherits
// from the frontend. Abort / fastfail path; error mode only (std::abort raises
// no SEH exception, so the Job's DIE_ON_UNHANDLED_EXCEPTION is not exercised).
void proc04_windows_abort_does_not_stall() {
  secure_frontend::SupervisorConfig config;
  config.policy.backoff_initial = 20ms;
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor(config,
                                               secure_frontend::make_stdio_process_factory(fake_plan({"--abort-in-gen", "--default-error-mode"})),
                                               std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "ready");
  const auto tokenizer = test_support::byte_tokenizer();
  const auto started = std::chrono::steady_clock::now();
  (void)secure_frontend::execute_with_lease(supervisor, {"fake-model", {{"user", "Hi"}}, 2U}, tokenizer);
  expect(log.wait_for("engine generation 1 ended: abort", 5s), "the abort is observed, not stalled on a WER dialog");
  expect(std::chrono::steady_clock::now() - started < 5s, "within 5 s");
  supervisor.shutdown(5s);
}

// Sibling smoke: a real unhandled exception (access violation) with the
// inherited error mode, so the Job's DIE_ON_UNHANDLED_EXCEPTION is on the path.
void proc04_windows_crash_does_not_stall() {
  secure_frontend::SupervisorConfig config;
  config.policy.backoff_initial = 20ms;
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor(
      config,
      secure_frontend::make_stdio_process_factory(fake_plan({"--crash-after-ready=SEGV", "--crash-delay-ms=300", "--default-error-mode"})),
      std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "ready");
  const auto started = std::chrono::steady_clock::now();
  expect(log.wait_for("engine generation 1 ended: crashed", 5s), "the crash is observed and classified crashed, not stalled on WER");
  expect(std::chrono::steady_clock::now() - started < 5s, "within 5 s");
  supervisor.shutdown(5s);
}

// K2 (spec §3.10): an engine that never reads stdin cannot hang the serving
// thread; the stalled chunk is cancelled at its deadline and the supervisor
// kills the engine as write_timeout.
void proc04_windows_write_timeout_kills_engine() {
  secure_frontend::SupervisorConfig config;
  config.policy.backoff_initial = 20ms;
  // The request must end through the write deadline, deterministically: a
  // short generation_timeout (2 s) bounds the case even if a write completed,
  // and the prompt cap is raised for the large prompt below.
  config.chat_limits.generation_timeout = 2s;
  config.chat_limits.maximum_prompt_tokens = 100000U;
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor(
      config, secure_frontend::make_stdio_process_factory(fake_plan({"--stall-stdin", "--ready-context=131072"})),
      std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "ready");
  const auto tokenizer = test_support::byte_tokenizer();
  // 60,000 '0' bytes are 60,000 prompt tokens of id 239 (three digits plus a
  // comma each): the GEN line is about 240 KB, more than 3x the 64 KiB pipe
  // buffer, and this engine never reads stdin, so a chunk must stall past the
  // 1 s no-progress deadline. (60,000 stays under the renderer's 65,536-byte
  // per-message cap, so the request reaches the write.)
  const secure_frontend::ChatCompletionRequest request{"fake-model", {{"user", std::string(60000U, '0')}}, 2U};
  const auto started = std::chrono::steady_clock::now();
  const auto outcome = secure_frontend::execute_with_lease(supervisor, request, tokenizer);
  expect(outcome && std::holds_alternative<secure_frontend::ChatExecutionError>(*outcome), "the request fails");
  expect(std::chrono::steady_clock::now() - started < 5s, "the blocked write is cancelled at its deadline");
  expect(log.wait_for("engine generation 1 ended: killed_by_supervisor(write_timeout)", 5s), "killed as write_timeout");
  supervisor.shutdown(5s);
}
#endif

// R1 (owner ruling 2026-10-14, spec §3.10): an engine that drains stdin
// slowly but steadily (16 KiB per 100 ms, so every 64 KiB chunk completes well
// inside the 1 s per-chunk deadline) still cannot hold the serving thread past
// the request's generation deadline: the GEN write is bounded overall by it
// and fails through the write_timeout kill path. Portable (POSIX and Windows).
void proc04_slow_stdin_write_bounded_by_request_deadline() {
  secure_frontend::SupervisorConfig config;
  config.policy.backoff_initial = 20ms;
  config.chat_limits.generation_timeout = 2s;
  config.chat_limits.maximum_prompt_tokens = 250000U;
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor(
      config, secure_frontend::make_stdio_process_factory(fake_plan({"--slow-stdin=16384", "--slow-stdin-interval-ms=100", "--ready-context=262144"})),
      std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "ready");
  const auto tokenizer = test_support::byte_tokenizer();
  // Four 60,000-byte messages (each under the renderer's 64 KiB per-message
  // cap) are ~240,000 prompt tokens of id 239: a GEN line of ~960 KB, which the
  // engine drains at <= 160 KiB/s, i.e. ~6 s, three times the 2 s deadline.
  const std::string content(60000U, '0');
  const secure_frontend::ChatCompletionRequest request{
      "fake-model", {{"user", content}, {"assistant", content}, {"user", content}, {"assistant", content}, {"user", "go"}}, 2U};
  const auto started = std::chrono::steady_clock::now();
  const auto outcome = secure_frontend::execute_with_lease(supervisor, request, tokenizer);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(outcome && std::holds_alternative<secure_frontend::ChatExecutionError>(*outcome), "the request fails");
  expect(elapsed < 3500ms, "the write ends within the 2 s request deadline + 1.5 s slack, took " +
                               std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) + " ms");
  expect(log.wait_for("engine generation 1 ended: killed_by_supervisor(write_timeout)", 5s), "killed as write_timeout");
  supervisor.shutdown(5s);
}

// SP5 M1 Task 11 (D-9, rulings T11-w/x): the supervised vision role with the pin's child.
// The fake (tests/fake_vision_child_main.cpp) records its argv and answers READY; these
// cases prove the launch shape and the SP2-style resource policy (oversize line -> kill,
// restart once, then circuit).

namespace {

std::string read_whole_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

secure_frontend::ProcessFactory vision_factory(const std::filesystem::path& record, const std::vector<std::string>& args,
                                               const std::filesystem::path& cwd = {}) {
  // Value captures: the caller's temporaries (the argument list) die at the end of the
  // full expression that stores this factory; the factory runs later, on start().
  return [record, args, cwd](std::uint64_t) -> secure_frontend::SpawnResult {
    auto child = std::make_unique<secure_frontend::EngineStdioProcess>();
    std::vector<std::string> argv{"--record-file=" + test_support::utf8_path(record)};
    argv.insert(argv.end(), args.begin(), args.end());
    secure_frontend::EngineCommand command{.executable = test_support::fake_vision_child_path(),
                                           .arguments = std::move(argv),
                                           .environment = {},
                                           .working_directory = cwd,
                                           .insert_serve = false};  // like the bootstrap: the reader takes no --serve (T11-ff)
    if (const auto error = child->start(command)) {
      return secure_frontend::SpawnFailure{error->message};
    }
    return child;
  };
}

int spawns_in(const std::string& record_text) {
  int count = 0;
  for (std::size_t at = record_text.find("SPAWN\n"); at != std::string::npos; at = record_text.find("SPAWN\n", at + 1U)) {
    ++count;
  }
  return count;
}

}  // namespace

// SP5-P2 Task 6: the encoder is the engine's own `maya-vision` at the new pin, and its
// grammar (engines/maya-ref tools/vision/strata_vision.cpp:61-79 at 451f9cb6) has NO
// --min-tokens slot — an unknown argument exits 2 there, where upstream v0.2.2's
// reader_args (serve/server.py:2573) still carried the #767 hand-edit. The composition
// must land inside the engine's accepted set.
void vision_child_argv_matches_maya_vision_grammar_at_pin() {
  const auto argv = secure_frontend::compose_vision_arguments({
      .mmproj = "/p/mmproj.gguf", .model = "/p/model.gguf", .threads = 4, .max_tokens = 2048, .no_flash_attn = true});
  expect(std::find(argv.begin(), argv.end(), "--min-tokens") == argv.end(),
         "no --min-tokens: the maya-vision grammar at the pin rejects unknown arguments (exit 2)");
  expect((argv == std::vector<std::string>{"--mmproj", "/p/mmproj.gguf", "--model", "/p/model.gguf", "--threads", "4",
                                           "--max-tokens", "2048", "--no-flash-attn"}),
         "the encoder argv sits inside the maya-vision grammar at the pin");
}

// Vision for cards (2026-10-28): the encoder the out folder ships is upstream's strata-vision (upstream/tools/vision),
// whose server writes --mmproj, --model, then --gpu, --threads and --max-tokens (serve/server.py reader_args) and whose
// grammar spells flash attention --flash-attn on|off|auto: no --no-flash-attn there (an unknown argument exits 2).
void vision_child_argv_follows_upstreams_strata_vision() {
  const auto argv = secure_frontend::compose_vision_arguments({.mmproj = "/p/mmproj.gguf", .model = "/p/model.gguf", .threads = 4,
                                                               .max_tokens = 2048, .no_flash_attn = true, .gpu = true,
                                                               .upstream_encoder = true});
  expect((argv == std::vector<std::string>{"--mmproj", "/p/mmproj.gguf", "--model", "/p/model.gguf", "--gpu", "--threads", "4",
                                           "--max-tokens", "2048", "--flash-attn", "off"}),
         "upstream's order, --gpu after the files, flash attention in upstream's spelling");
  const auto cpu = secure_frontend::compose_vision_arguments({.mmproj = "/p/m.gguf", .model = "/p/x.gguf", .upstream_encoder = true});
  expect((cpu == std::vector<std::string>{"--mmproj", "/p/m.gguf", "--model", "/p/x.gguf"}), "the CPU and the defaults: the files only");
}

// The pin's vision block form, re-anchored by SP5-P2 Task 6 on maya-vision's grammar at
// 451f9cb6: --mmproj, --model, then the optional --threads / --max-tokens; our added
// --no-flash-attn rides last and the grammar itself carries it. The role opens on the
// child's READY line and QUITs it down.
void vision_child_launches_with_pinned_args_when_enabled() {
  const auto dir = test_support::make_temp_dir("vision-launch");
  const auto record = dir / "record.txt";
  const auto argv = secure_frontend::compose_vision_arguments({
      .mmproj = test_support::utf8_path(dir / "mmproj.gguf"), .model = test_support::utf8_path(dir / "native.gguf"),
      .threads = 4, .max_tokens = 2048, .no_flash_attn = true});
  expect((argv == std::vector<std::string>{"--mmproj", test_support::utf8_path(dir / "mmproj.gguf"), "--model",
                                           test_support::utf8_path(dir / "native.gguf"), "--threads", "4", "--max-tokens",
                                           "2048", "--no-flash-attn"}),
         "compose_vision_arguments renders the grammar at the maya-vision pin (the added flag last)");
  secure_frontend::VisionLimits limits;
  limits.policy.backoff_initial = std::chrono::milliseconds{10};
  secure_frontend::VisionSupervisor supervisor(vision_factory(record, argv), limits);
  supervisor.start();
  expect(supervisor.state() == secure_frontend::VisionState::up, "the READY gate opens the role");
  supervisor.stop();
  expect(supervisor.state() == secure_frontend::VisionState::stopped, "QUIT closes the role (pin :2792)");
  const std::string text = read_whole_file(record);
  expect(spawns_in(text) == 1, "exactly one spawn: " + text);
  expect(text.find("ARG --mmproj\nARG " + test_support::utf8_path(dir / "mmproj.gguf") + "\nARG --model\nARG " +
                         test_support::utf8_path(dir / "native.gguf") + "\n") != std::string::npos,
         "the child received the mmproj/model pair first, like the pin");
  expect(text.find("ARG --threads\nARG 4\nARG --max-tokens\nARG 2048\nARG --no-flash-attn\n") != std::string::npos,
         "threads and the token bounds ride the flag names of the grammar at the pin (:2562-2565 at the old pin; maya-vision strata_vision.cpp at 451f9cb6; Ruling T11-dd)");
}

// SP2 policy on this role (plan): an oversize stdout line is a failure (the shared
// 8192-byte line bound), the child is killed, restart happens ONCE, then the circuit.
void vision_child_respects_resource_limits() {
  const auto dir = test_support::make_temp_dir("vision-limits");
  const auto record = dir / "record.txt";
  secure_frontend::VisionLimits limits;
  limits.policy.max_consecutive_failures = 2U;
  limits.policy.backoff_initial = std::chrono::milliseconds{10};
  secure_frontend::VisionSupervisor supervisor(
      vision_factory(record, {"--mode=oversize", "--mmproj", "m", "--model", "n"}), limits);
  supervisor.start();
  expect(supervisor.state() == secure_frontend::VisionState::circuit_open,
         "the oversize line kills; one restart; then the circuit (SP2 policy, PROC/DOS rows)");
  const int oversize_spawns = spawns_in(read_whole_file(record));
  expect(oversize_spawns == 2, "restart once then circuit (spawn count): " + std::to_string(oversize_spawns));
  supervisor.stop();
}

// SP5 M1 Task 12 (C-9, Ruling T12-e): the pin's ENC round trip (serve/server.py:2752-2780).
// "ENC <img> <out>" is answered "OK <n>" — n is the image-token count that sizes the
// prompt's pad run — the .sve lands in the child's cwd, and the child stays up for the
// next image. After an explicit stop() the role does NOT wake (that stop is shutdown;
// only a never-started lazy role wakes, the pin's _vision_up at :2646).
void vision_encode_round_trip_at_the_pin() {
  const auto dir = test_support::make_temp_dir("vision-enc");
  const auto record = dir / "record.txt";
  secure_frontend_test::write_file(dir / "a.img", "imgbytes");
  secure_frontend::VisionLimits limits;
  limits.policy.backoff_initial = std::chrono::milliseconds{10};
  secure_frontend::VisionSupervisor supervisor(vision_factory(record, {"--mmproj", "m", "--model", "n"}, dir), limits);
  supervisor.start();
  expect(supervisor.up(), "the role is up");
  const auto first = supervisor.encode("a.img", "a.sve");
  const auto* encoded = std::get_if<secure_frontend::VisionEncoded>(&first);
  expect(encoded != nullptr && encoded->image_tokens == 5U, "the OK line's count is the image-token count (:2778)");
  expect(secure_frontend_test::read_file(dir / "a.sve") == std::string("\x00sve0", 5U),
         "the embedding file lands in the child's cwd under the requested name (#480)");
  const auto second = supervisor.encode("a.img", "b.sve");
  expect(std::holds_alternative<secure_frontend::VisionEncoded>(second), "the child stays up for the next ENC");
  expect(spawns_in(read_whole_file(record)) == 1, "one generation serves every ENC");
  supervisor.stop();
  const auto after = supervisor.encode("a.img", "c.sve");
  const auto* error = std::get_if<secure_frontend::VisionEncodeError>(&after);
  expect(error != nullptr && error->kind == secure_frontend::VisionEncodeError::Kind::not_running &&
             spawns_in(read_whole_file(record)) == 1,
         "an explicit stop() is shutdown: encode refuses, it does not wake the role");
}

// The failure paths at the pin: ERR -> the request fails, the child lives (the pin raises
// and returns the encoder to the pool); a silent child is killed within the encode timeout
// (VISION_ENCODE_S, :328) and the role goes stopped so the NEXT request wakes it (:2646);
// a child that dies after answering is detected on the next attempt and woken.
void vision_encode_failures_take_the_pinned_paths() {
  {
    const auto dir = test_support::make_temp_dir("vision-badenc");
    const auto record = dir / "record.txt";
    secure_frontend::VisionSupervisor supervisor(vision_factory(record, {"--mode=badenc"}, dir), secure_frontend::VisionLimits{});
    supervisor.start();
    const auto result = supervisor.encode("x.img", "x.sve");
    const auto* error = std::get_if<secure_frontend::VisionEncodeError>(&result);
    expect(error != nullptr && error->kind == secure_frontend::VisionEncodeError::Kind::encoder_rejected,
           "ERR answers close the image, not the role (:2774)");
    expect(supervisor.up(), "the child stays up after an ERR line");
    supervisor.stop();
  }
  {
    const auto dir = test_support::make_temp_dir("vision-silent");
    const auto record = dir / "record.txt";
    secure_frontend::VisionLimits limits;
    limits.encode_timeout = std::chrono::milliseconds{200};
    secure_frontend::VisionSupervisor supervisor(vision_factory(record, {"--mode=silentenc"}, dir), limits);
    supervisor.start();
    const auto started = std::chrono::steady_clock::now();
    const auto first = supervisor.encode("x.img", "x.sve");
    const auto* error = std::get_if<secure_frontend::VisionEncodeError>(&first);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    expect(error != nullptr && error->kind == secure_frontend::VisionEncodeError::Kind::no_reply,
           "a silent encoder is a failed image (:1317 family)");
    expect(elapsed < std::chrono::seconds{2}, "the encode timeout bounds the wait (200 ms + slack)");
    expect(!supervisor.up(), "the silent child was killed and reaped: the role is stopped for the next request");
    const auto second = supervisor.encode("x.img", "x.sve");  // wakes, times out again
    expect(std::holds_alternative<secure_frontend::VisionEncodeError>(second) && spawns_in(read_whole_file(record)) == 2,
           "the next request starts the encoder again (pin _vision_up :2646)");
    supervisor.stop();
  }
  {
    const auto dir = test_support::make_temp_dir("vision-dieenc");
    const auto record = dir / "record.txt";
    secure_frontend::VisionSupervisor supervisor(vision_factory(record, {"--mode=dieenc"}, dir), secure_frontend::VisionLimits{});
    supervisor.start();
    expect(std::holds_alternative<secure_frontend::VisionEncoded>(supervisor.encode("x.img", "x.sve")),
           "the first ENC is answered");
    const auto second = supervisor.encode("x.img", "y.sve");  // the child exited after answering
    expect(std::holds_alternative<secure_frontend::VisionEncodeError>(second), "a closed child fails the image");
    expect(!supervisor.up(), "the dead role is stopped, not silently kept 'up'");
    const auto third = supervisor.encode("x.img", "z.sve");  // wake
    expect(std::holds_alternative<secure_frontend::VisionEncoded>(third) && spawns_in(read_whole_file(record)) == 2,
           "the next request wakes a fresh encoder");
    supervisor.stop();
  }
}

// RF4: an exception that escapes the supervisor thread (the factory, the log
// sink, the clock) opens the circuit instead of reaching std::terminate, and
// a generation still alive at that point is killed and reaped first.
class ThrowingClock final : public secure_frontend::SupervisorClock {
 public:
  [[nodiscard]] time_point now() const override {
    if (armed_.load()) throw std::runtime_error("clock");
    return std::chrono::steady_clock::now();
  }
  [[nodiscard]] bool sleep_backoff(const std::chrono::milliseconds delay, secure_frontend::SupervisorWake& wake) override {
    return wake.wait(secure_frontend::kNoMonitorHandle, delay) == secure_frontend::SupervisorWake::Event::timeout;
  }
  void arm() { armed_.store(true); }

 private:
  std::atomic<bool> armed_{false};
};

void expect_exhausted_then_stopped(secure_frontend::EngineSupervisor& supervisor, const std::string& what) {
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 5s), what + " opens the circuit");
  const auto health = supervisor.snapshot();
  expect(!health.healthy && health.engine == "failed" && health.recovery == "exhausted", what + ": /health is failed/exhausted");
  expect(!supervisor.acquire().has_value(), what + ": no lease");
  supervisor.shutdown(5s);
  expect(supervisor.snapshot().state == SupervisorState::stopped, what + ": the shutdown still completes");
}

void supervisor_exception_opens_circuit() {
  {
    secure_frontend::EngineSupervisor supervisor(
        {}, [](std::uint64_t) -> secure_frontend::SpawnResult { throw std::runtime_error("factory"); },
        std::make_shared<test_support::ManualClock>(), [](std::string_view) {});
    expect(supervisor.start(), "the supervisor starts");
    expect_exhausted_then_stopped(supervisor, "a throwing factory");
  }
  {
    test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.end({false, 1U}); });
    secure_frontend::EngineSupervisor supervisor({}, fakes.factory(), std::make_shared<test_support::ManualClock>(),
                                                 [](std::string_view) { throw std::runtime_error("log"); });
    expect(supervisor.start(), "the supervisor starts");
    expect_exhausted_then_stopped(supervisor, "a throwing log sink");
    expect(fakes.spawned() == 1U, "no restart after the exception");
  }
  {
    test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
    auto clock = std::make_shared<ThrowingClock>();
    secure_frontend::EngineSupervisor supervisor({}, fakes.factory(), clock, [](std::string_view) {});
    expect(supervisor.start(), "the supervisor starts");
    expect(test_support::wait_generation_ready(supervisor, 1U, 5s), "generation 1 ready");
    clock->arm();
    {
      auto lease = supervisor.acquire();
      expect(lease.has_value(), "lease on generation 1");
      // The supervisor thread reads the clock as the generation ends: it throws with generation 1 still current.
      if (lease) supervisor.report_fatal(*lease, secure_frontend::KillReason::protocol_failed);
      expect(supervisor.wait_for_state(SupervisorState::circuit_open, 5s), "a throwing clock opens the circuit");
    }
    const auto child = fakes.child(1U);
    expect(child != nullptr && child->kills >= 1, "the live generation is killed before the circuit opens");
    expect_exhausted_then_stopped(supervisor, "a throwing clock");
    expect(fakes.spawned() == 1U, "no restart after the exception");
  }
}

// Spec §3.7: with no Lease out, shutdown closes engine stdin first (the engine
// exits 0 on EOF), then always kills the group; the end is logged as the
// supervisor's own shutdown, not as a clean exit.
void supervisor_shutdown_sends_eof_first_then_kills() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor({}, fakes.factory(), std::make_shared<test_support::ManualClock>(), log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 5s), "generation 1 ready");
  const auto started = std::chrono::steady_clock::now();
  supervisor.shutdown(5s);
  expect(std::chrono::steady_clock::now() - started < 1s, "an engine that exits on EOF is not waited on for the 2 s budget");
  expect(supervisor.snapshot().state == SupervisorState::stopped, "stopped");
  const auto child = fakes.child(1U);
  expect(child != nullptr, "generation 1 exists");
  if (!child) return;
  std::lock_guard lock(child->mutex);
  expect(child->exit.has_value() && !child->exit->signaled && child->exit->value == 0U, "the engine ended on stdin EOF (exit 0)");
  expect(child->kills == 1, "the group is SIGKILLed anyway (spec §3.7: always), saw " + std::to_string(child->kills));
  expect(log.contains("engine generation 1 ended: killed_by_supervisor(shutdown)"), "logged as the supervisor's shutdown");
  expect(!supervisor.engine_left_unreaped() && !log.contains(secure_frontend::kEngineLeftUnreapedLine),
         "a reaped engine is not reported as left (main exits 0)");
}

// RF4 / spec §3.7: a Lease still out at shutdown keeps engine stdin (no EOF
// under a request), and it must not outlive shutdown() pointing at the
// supervisor: destroying it after the supervisor is gone touches nothing.
void supervisor_lease_outliving_shutdown_is_safe() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  test_support::LogCapture log;
  alignas(secure_frontend::EngineSupervisor) unsigned char storage[sizeof(secure_frontend::EngineSupervisor)];
  auto* supervisor = new (storage) secure_frontend::EngineSupervisor({}, fakes.factory(), std::make_shared<test_support::ManualClock>(), log.sink());
  expect(supervisor->start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(*supervisor, 1U, 5s), "generation 1 ready");
  std::optional<secure_frontend::Lease> lease = supervisor->acquire();
  expect(lease.has_value(), "lease on generation 1");
  supervisor->shutdown(5s);
  expect(supervisor->snapshot().state == SupervisorState::stopped, "shutdown completes with the Lease still out");
  if (const auto child = fakes.child(1U)) {
    std::lock_guard lock(child->mutex);
    expect(child->exit.has_value() && child->exit->signaled == test_support::fake_kill_status().signaled &&
               child->exit->value == test_support::fake_kill_status().value,
           "no EOF under a leased request: the engine is killed, not closed");
  }
  expect(log.contains("engine generation 1 ended: killed_by_supervisor(shutdown)"), "logged as the supervisor's shutdown");
  supervisor->~EngineSupervisor();
  // Poison the supervisor's memory: a Lease that still pointed at it would lock
  // a garbage mutex and write to a garbage descriptor.
  std::memset(storage, 0xA5, sizeof(storage));
  lease.reset();
  expect(true, "the Lease outlived the supervisor without touching it");
}

// SP3 D-1: a serving thread blocked in wait_for_change(generating) wakes as
// soon as another thread destroys the Lease (no polling), and a wait with
// nothing happening ends at its timeout with the unchanged state.
void supervisor_wait_wakes_on_lease_release() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor({}, fakes.factory(), std::make_shared<test_support::ManualClock>(), log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 5s), "generation 1 ready");
  std::optional<secure_frontend::Lease> lease = supervisor.acquire();
  expect(lease.has_value(), "lease on generation 1");

  const auto quiet_start = std::chrono::steady_clock::now();
  const auto quiet = supervisor.wait_for_change(SupervisorState::generating, 200ms);
  const auto quiet_elapsed = std::chrono::steady_clock::now() - quiet_start;
  expect(quiet.state == SupervisorState::generating, "a timed-out wait returns the unchanged state");
  expect(quiet_elapsed >= 200ms && quiet_elapsed < 1s, "the wait ends at its timeout, not before and not much after");

  std::atomic<bool> waiting{false};
  std::optional<secure_frontend::HealthSnapshot> woke;
  std::chrono::steady_clock::time_point woke_at{};
  std::thread waiter([&] {
    waiting.store(true);
    woke = supervisor.wait_for_change(SupervisorState::generating, 10s);
    woke_at = std::chrono::steady_clock::now();
  });
  while (!waiting.load()) std::this_thread::yield();
  std::this_thread::sleep_for(100ms);  // let the waiter block on the condition
  const auto released_at = std::chrono::steady_clock::now();
  lease.reset();
  waiter.join();
  expect(woke.has_value() && woke->state == SupervisorState::ready, "the waiter sees `ready` after the release");
  expect(woke_at >= released_at && woke_at - released_at < 1s, "the waiter wakes promptly (< 1 s) on the release");
  expect(supervisor.acquire().has_value(), "the released generation can be leased again");
}

// SP3 D-1: a waiter never sleeps through a shutdown, even with a Lease out
// (shutdown kills the leased generation, spec §3.7).
void supervisor_wait_wakes_on_shutdown() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor({}, fakes.factory(), std::make_shared<test_support::ManualClock>(), log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 5s), "generation 1 ready");
  std::optional<secure_frontend::Lease> lease = supervisor.acquire();
  expect(lease.has_value(), "lease on generation 1");
  std::atomic<bool> waiting{false};
  std::optional<secure_frontend::HealthSnapshot> woke;
  std::chrono::steady_clock::time_point woke_at{};
  std::thread waiter([&] {
    waiting.store(true);
    woke = supervisor.wait_for_change(SupervisorState::generating, 10s);
    woke_at = std::chrono::steady_clock::now();
  });
  while (!waiting.load()) std::this_thread::yield();
  std::this_thread::sleep_for(100ms);
  const auto shutdown_at = std::chrono::steady_clock::now();
  supervisor.shutdown(5s);
  waiter.join();
  expect(woke.has_value() && (woke->state == SupervisorState::shutting_down || woke->state == SupervisorState::stopped),
         "the waiter sees the shutdown");
  expect(woke_at >= shutdown_at && woke_at - shutdown_at < 1s, "the waiter wakes promptly (< 1 s) on shutdown");
  lease.reset();  // RF4: after shutdown() it no longer refers to the supervisor
}

// SP3 D-1: acquire / snapshot / wait_for_change / Lease release from many
// threads at once against the fake engine: no crash, no deadlock (bounded
// join), at most one live Lease, and acquire() is empty while one is out.
void supervisor_serving_api_is_thread_safe() {
  constexpr int kThreads = 8;
  constexpr auto kRunFor = 2s;
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor({}, secure_frontend::make_stdio_process_factory(fake_plan({})),
                                               std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
  expect(supervisor.start(), "the supervisor starts");
  expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "generation 1 ready");

  std::atomic<int> live{0};
  std::atomic<int> violations{0};
  std::atomic<std::uint64_t> leases{0};
  std::atomic<std::uint64_t> empty_acquires{0};
  std::mutex done_mutex;
  std::condition_variable done_changed;
  int done = 0;
  const auto stop_at = std::chrono::steady_clock::now() + kRunFor;
  std::vector<std::thread> threads;
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([&] {
      while (std::chrono::steady_clock::now() < stop_at) {
        const auto seen = supervisor.snapshot();
        if (seen.state != SupervisorState::ready && seen.state != SupervisorState::generating) violations.fetch_add(1);
        if (auto lease = supervisor.acquire()) {
          leases.fetch_add(1);
          if (live.fetch_add(1) != 0) violations.fetch_add(1);  // two Leases at once
          if (supervisor.acquire().has_value()) violations.fetch_add(1);
          if (supervisor.snapshot().state != SupervisorState::generating) violations.fetch_add(1);
          if (lease->generation() != 1U) violations.fetch_add(1);
          std::this_thread::yield();
          live.fetch_sub(1);
        } else {
          empty_acquires.fetch_add(1);
          (void)supervisor.wait_for_change(SupervisorState::generating, 50ms);
        }
      }
      std::lock_guard lock(done_mutex);
      ++done;
      done_changed.notify_all();
    });
  }
  bool joined = false;
  {
    std::unique_lock lock(done_mutex);
    joined = done_changed.wait_for(lock, kRunFor + 20s, [&] { return done == kThreads; });
  }
  if (!joined) {
    std::cerr << "FAILED: the serving threads deadlocked (no bounded join)\n";
    std::_Exit(EXIT_FAILURE);
  }
  for (auto& thread : threads) thread.join();
  expect(violations.load() == 0, "no invariant violation, saw " + std::to_string(violations.load()));
  expect(leases.load() > 0 && empty_acquires.load() > 0, "the threads both leased and were refused while leased");
  expect(supervisor.snapshot().state == SupervisorState::ready, "the supervisor is back in `ready`");
  expect(supervisor.generations_started() == 1U, "no restart: the engine was never killed");
  supervisor.shutdown(5s);
  expect(supervisor.snapshot().state == SupervisorState::stopped, "stopped");
}

// Sets a variable of this test process for one case and restores the previous
// value (or removes it) when the case ends.
class ScopedParentVariable final {
 public:
  ScopedParentVariable(const char* name, const char* value) : name_(name) {
    if (const char* previous = secure_frontend::crt_getenv(name)) previous_ = std::string(previous);
    set(value);
  }
  ~ScopedParentVariable() {
    if (previous_) {
      set(previous_->c_str());
    } else {
#if defined(_WIN32)
      (void)_putenv_s(name_, "");  // an empty value removes it
#else
      (void)unsetenv(name_);
#endif
    }
  }
  ScopedParentVariable(const ScopedParentVariable&) = delete;
  ScopedParentVariable& operator=(const ScopedParentVariable&) = delete;

 private:
  void set(const char* value) const {
#if defined(_WIN32)
    (void)_putenv_s(name_, value);
#else
    (void)setenv(name_, value, 1);
#endif
  }
  const char* name_;
  std::optional<std::string> previous_;
};

// N1 (spec §3.9): the engine's environment is exactly the rendered allowlist.
void proc02_child_env_is_exact_allowlist() {
  // A hostile parent environment: none of it may reach the engine.
  const ScopedParentVariable token("STRATA_TEST_TOKEN_ENV", "secret-token-value");
  const ScopedParentVariable api_key("STRATA_API_KEY", "k");
  const ScopedParentVariable home("HOME", "/home/someone");
  const ScopedParentVariable preload("LD_PRELOAD", "/nonexistent/libstrata-test.so");
  const ScopedParentVariable proxy("HTTPS_PROXY", "http://proxy.invalid:3128");
  const ScopedParentVariable devices("CUDA_VISIBLE_DEVICES", "0");
  const auto state = test_support::make_temp_dir("n1");
  const secure_frontend::EngineEnvironment environment{
      .lib_dirs = {state / "lib"}, .temp_dir = state / "engine-tmp", .cuda_cache_dir = state / "cache" / "cuda"};
  auto plan = fake_plan({});
  plan.environment = environment;
  auto spawned = secure_frontend::make_stdio_process_factory(plan)(1U);
  auto* process = std::get_if<std::unique_ptr<secure_frontend::SupervisedProcess>>(&spawned);
  expect(process != nullptr, "spawn");
  if (process == nullptr) return;
  auto& transport = (*process)->transport();
  expect(transport.read_line(5s).line == "READY 4096 stop", "READY");
  const auto dump = test_support::request_dump(transport, 5s);
  expect(dump.has_value(), "DUMP");
  if (!dump) return;
  auto rendered = environment;
  rendered.temp_dir = state / "engine-tmp" / "1";
  std::vector<std::string> expected;
  for (const auto& [name, value] : secure_frontend::render_engine_environment(rendered)) expected.push_back(name + "=" + value);
  std::sort(expected.begin(), expected.end());
  // On a mismatch, name the entries that differ (names only, never values).
  std::string differing;
  const auto missing_from = [&differing](const std::vector<std::string>& side, const std::vector<std::string>& other,
                                         const char* mark) {
    for (const auto& entry : side) {
      if (std::find(other.begin(), other.end(), entry) == other.end()) differing += mark + entry.substr(0, entry.find('='));
    }
  };
  missing_from(dump->env, expected, " +");
  missing_from(expected, dump->env, " -");
  expect(dump->env == expected, "the child environment is exactly the rendered allowlist; differing:" + differing);
  for (const auto& entry : dump->env) {
    for (const std::string_view banned : secure_frontend::kNeverForwardedEnvironment) {
      expect(entry.rfind(std::string(banned) + "=", 0) != 0, "never forwarded: " + std::string(banned));
    }
    expect(entry.rfind("STRATA_TEST_TOKEN_ENV=", 0) != 0, "the token variable never reaches the engine");
    if (entry.rfind("STRATA_", 0) == 0) {
      expect(entry.rfind("STRATA_HIPBLASLT_TUNING=", 0) == 0 || entry.rfind("STRATA_WATCHDOG_S=", 0) == 0 ||
                 entry.rfind("STRATA_WATCHDOG_IO_S=", 0) == 0,
             "no other STRATA_* variable: " + entry.substr(0, entry.find('=')));
    }
  }
  expect(secure_frontend::is_owner_only_directory(state / "engine-tmp" / "1"), "the generation's temp directory exists, owner-only");
  // The directory goes away once the generation is reaped, exactly as
  // end_generation / shutdown_generation do it (kill, reap, release_resources).
  (*process)->kill_group();
  for (int step = 0; step < 500 && !(*process)->try_reap(); ++step) std::this_thread::sleep_for(10ms);
  (*process)->release_resources(1000ms);
  expect(!std::filesystem::exists(state / "engine-tmp" / "1"), "the generation's temp directory is removed after the reap");
}

#if !defined(_WIN32)
// RF4: the shutdown signal handler preserves errno, also when its wake write
// fails (a full self-pipe gives EAGAIN).
void shutdown_signal_preserves_errno() {
  secure_frontend::SupervisorWake wake;
  expect(wake.valid(), "wake pipe");
  for (int i = 0; i < 200000; ++i) wake.notify();  // fill the non-blocking pipe: the next write fails
  secure_frontend::install_shutdown_signals(wake);
  expect(!secure_frontend::shutdown_signalled(), "not signalled before SIGTERM");
  errno = EDOM;
  (void)std::raise(SIGTERM);
  const int after = errno;
  expect(after == EDOM, "errno survives the handler, saw " + std::to_string(after));
  expect(secure_frontend::shutdown_signalled(), "SIGTERM sets the shutdown flag");
  expect(wake.wait(secure_frontend::kNoMonitorHandle, 0ms) == secure_frontend::SupervisorWake::Event::woken, "the wake is readable");
  secure_frontend::release_shutdown_signals(wake);
}

// RF4: without its wake pipe the supervisor thread could never be woken for
// shutdown, so start() refuses and spawns nothing.
void supervisor_start_refuses_without_wake() {
  test_support::FakeFactory fakes([](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); });
  rlimit saved{};
  expect(getrlimit(RLIMIT_NOFILE, &saved) == 0, "read RLIMIT_NOFILE");
  rlimit none = saved;
  none.rlim_cur = 0;  // no new descriptor at all: pipe2 fails with EMFILE
  expect(setrlimit(RLIMIT_NOFILE, &none) == 0, "lower RLIMIT_NOFILE");
  auto supervisor = std::make_unique<secure_frontend::EngineSupervisor>(
      secure_frontend::SupervisorConfig{}, fakes.factory(), std::make_shared<test_support::ManualClock>(), [](std::string_view) {});
  expect(setrlimit(RLIMIT_NOFILE, &saved) == 0, "restore RLIMIT_NOFILE");
  expect(!supervisor->start(), "start() refuses when the wake could not be created");
  expect(supervisor->snapshot().state == SupervisorState::stopped && fakes.spawned() == 0U, "nothing is spawned");
  supervisor.reset();
}
#endif

// G4 (spec §3.12): the frontend runs as the invoking unprivileged user.
void proc06_refuses_privileged_start() {
  expect(secure_frontend::privileged_start_error({.posix_root = true, .windows_elevated = false}).has_value(), "root is refused");
  expect(secure_frontend::privileged_start_error({.posix_root = false, .windows_elevated = true}).has_value(), "an elevated token is refused");
  expect(!secure_frontend::privileged_start_error({.posix_root = false, .windows_elevated = false}).has_value(), "an unprivileged user starts");
#if !defined(_WIN32)
  expect(secure_frontend::current_process_privileges().posix_root == (geteuid() == 0), "the probe reads the effective uid");
#endif
}

// Task 29 Step 5a: the CUDA cache follows roots.cache when it is set.
void engine_environment_uses_cache_root_when_set() {
  const std::filesystem::path state{"/srv/state"};
  const auto plain = secure_frontend::engine_environment_for(state, std::nullopt, {"/opt/engine"});
  expect(plain.cuda_cache_dir == state / "cache" / "cuda", "without roots.cache the CUDA cache stays below the state dir");
  expect(plain.temp_dir == state / "engine-tmp", "the per-generation temp root is <state>/engine-tmp");
  expect(plain.lib_dirs == std::vector<std::filesystem::path>{"/opt/engine"}, "lib dirs pass through");
  const auto cached = secure_frontend::engine_environment_for(state, std::filesystem::path{"/var/cache/strata"}, {});
  expect(cached.cuda_cache_dir == std::filesystem::path{"/var/cache/strata"} / "cuda", "roots.cache holds the CUDA cache when set");
}

// SP6 env family (R5): the byte pin of the strata rendering, captured before
// the family field existed. Every member is set so every rendered row is pinned.
secure_frontend::EngineEnvironment representative_posix_environment() {
  return {.lib_dirs = {"/opt/strata/lib", "/opt/strata/cuda"},
          .temp_dir = "/srv/state/engine-tmp/7",
          .cuda_cache_dir = "/srv/cache/cuda",
          .hipblaslt_tuning = std::filesystem::path{"/opt/strata/tuning.txt"},
          .watchdog_s = 30U,
          .watchdog_io_s = 5U};
}

// Backslash paths render the same bytes on both hosts (one file name on POSIX).
secure_frontend::EngineEnvironment representative_windows_environment() {
  return {.lib_dirs = {"C:\\strata\\lib", "C:\\strata\\cuda"},
          .temp_dir = "C:\\state\\engine-tmp\\7",
          .cuda_cache_dir = "C:\\cache\\cuda",
          .hipblaslt_tuning = std::filesystem::path{"C:\\strata\\tuning.txt"},
          .watchdog_s = 30U,
          .watchdog_io_s = 5U};
}

const secure_frontend::WindowsSystemValues kPinnedSystem{.system_root = "C:\\Windows", .system_drive = "C:", .processors = 8U};

const secure_frontend::EnvironmentBlock kPinnedPosixBlock{{"CUDA_CACHE_PATH", "/srv/cache/cuda"},
                                                          {"LD_LIBRARY_PATH", "/opt/strata/lib:/opt/strata/cuda"},
                                                          {"PATH", "/usr/bin:/bin"},
                                                          {"STRATA_HIPBLASLT_TUNING", "/opt/strata/tuning.txt"},
                                                          {"STRATA_WATCHDOG_IO_S", "5"},
                                                          {"STRATA_WATCHDOG_S", "30"},
                                                          {"TMPDIR", "/srv/state/engine-tmp/7"}};

const secure_frontend::EnvironmentBlock kPinnedWindowsBlock{{"CUDA_CACHE_PATH", "C:\\cache\\cuda"},
                                                            {"NUMBER_OF_PROCESSORS", "8"},
                                                            {"PATH", "C:\\strata\\lib;C:\\strata\\cuda;C:\\Windows\\System32"},
                                                            {"STRATA_HIPBLASLT_TUNING", "C:\\strata\\tuning.txt"},
                                                            {"STRATA_WATCHDOG_IO_S", "5"},
                                                            {"STRATA_WATCHDOG_S", "30"},
                                                            {"SystemDrive", "C:"},
                                                            {"SystemRoot", "C:\\Windows"},
                                                            {"TEMP", "C:\\state\\engine-tmp\\7"},
                                                            {"TMP", "C:\\state\\engine-tmp\\7"},
                                                            {"WINDIR", "C:\\Windows"}};

void sp6_env_strata_family_renders_unchanged() {
#if !defined(_WIN32)
  expect(secure_frontend::render_posix_environment(representative_posix_environment()) == kPinnedPosixBlock,
         "POSIX strata rendering is byte-identical to the pin");
#endif
  expect(secure_frontend::render_windows_environment(representative_windows_environment(), kPinnedSystem) == kPinnedWindowsBlock,
         "Windows strata rendering is byte-identical to the pin");
  // The family defaults to strata, and naming it explicitly changes nothing.
  using secure_frontend::EngineFamily;
  expect(representative_posix_environment().family == EngineFamily::strata, "the family defaults to strata");
  auto strata_windows = representative_windows_environment();
  strata_windows.family = EngineFamily::strata;
  const auto strata_rows = secure_frontend::family_environment_rows(strata_windows);
  expect(strata_rows.forced.empty() && strata_rows.suppressed.empty(), "the strata hook forces and suppresses nothing");
  const auto strata_family = secure_frontend::apply_family_environment(
      secure_frontend::render_windows_environment(strata_windows, kPinnedSystem), strata_windows);
  expect(strata_family == kPinnedWindowsBlock, "strata through the family step: Windows pin");
#if !defined(_WIN32)
  auto strata_posix = representative_posix_environment();
  strata_posix.family = EngineFamily::strata;
  expect(secure_frontend::render_family_environment(strata_posix) == kPinnedPosixBlock, "strata through the family step: POSIX pin");
#endif
  // SP5 M2 Task 4 (D-7): the glm hook is no longer empty — it carries the
  // §4.9 safety defaults and the CUDA selectors; the exact block is pinned by
  // glm_child_env_is_exactly_the_named_typed_set. The strata pins above stay
  // byte-identical.
  auto glm_windows = representative_windows_environment();
  glm_windows.family = EngineFamily::glm;
  const auto glm_rows = secure_frontend::family_environment_rows(glm_windows);
  expect(!glm_rows.forced.empty() && !glm_rows.suppressed.empty(), "the glm hook now forces and suppresses its rows (D-7)");
  expect(secure_frontend::apply_family_environment(secure_frontend::render_windows_environment(glm_windows, kPinnedSystem), glm_windows) !=
             kPinnedWindowsBlock,
         "glm no longer renders the strata pin: it renders the GLM family's rows");
  // The config path builds the strata family.
  expect(secure_frontend::engine_environment_for("/srv/state", std::nullopt, {}).family == EngineFamily::strata,
         "engine_environment_for builds the strata family");
}

using secure_frontend::FamilyEnvironmentRows;

// Test-only hooks (the production one is family_environment_rows).
FamilyEnvironmentRows test_gpu_rows(const secure_frontend::EngineEnvironment&) {
  return {.forced = {{"STRATA_WATCHDOG_S", "0"}, {"CUDA_DEVICE_ORDER", "PCI_BUS_ID"}, {"CUDA_VISIBLE_DEVICES", "1"}}};
}
FamilyEnvironmentRows test_gpu_rows_reversed(const secure_frontend::EngineEnvironment&) {
  return {.forced = {{"CUDA_VISIBLE_DEVICES", "1"}, {"CUDA_DEVICE_ORDER", "PCI_BUS_ID"}, {"STRATA_WATCHDOG_S", "0"}}};
}
// Derives a value from the EngineEnvironment the way SP6 derives CUDA_VISIBLE_DEVICES
// from the configured gpu list (absent -> 0). The stand-in source is watchdog_io_s:
// no GLM config field is added (YAGNI).
FamilyEnvironmentRows test_derived_rows(const secure_frontend::EngineEnvironment& environment) {
  return {.forced = {{"CUDA_DEVICE_ORDER", "PCI_BUS_ID"},
                     {"CUDA_VISIBLE_DEVICES", environment.watchdog_io_s ? std::to_string(*environment.watchdog_io_s) : "0"}}};
}
FamilyEnvironmentRows test_suppress_rows(const secure_frontend::EngineEnvironment&) {
  return {.forced = {{"STRATA_WATCHDOG_S", "0"}}, .suppressed = {"STRATA_WATCHDOG_IO_S", "STRATA_HIPBLASLT_TUNING"}};
}
FamilyEnvironmentRows test_duplicate_rows(const secure_frontend::EngineEnvironment&) {
  return {.forced = {{"CUDA_VISIBLE_DEVICES", "0"}, {"CUDA_VISIBLE_DEVICES", "1"}}};
}
FamilyEnvironmentRows test_nul_rows(const secure_frontend::EngineEnvironment&) {
  return {.forced = {{"CUDA_VISIBLE_DEVICES", std::string("0\0x", 3)}}};
}

void sp6_env_forced_values_override_and_add() {
  // The Windows rendering of backslash paths is byte-stable on both hosts.
  const auto environment = representative_windows_environment();
  const auto generic = secure_frontend::render_windows_environment(environment, kPinnedSystem);
  const auto block = secure_frontend::apply_family_environment(generic, environment, test_gpu_rows);
  auto expected = kPinnedWindowsBlock;
  expected["STRATA_WATCHDOG_S"] = "0";  // override: replaces the generic value 30
  expected.emplace("CUDA_DEVICE_ORDER", "PCI_BUS_ID");
  expected.emplace("CUDA_VISIBLE_DEVICES", "1");
  expect(block == expected, "forced values override and add; every other row is untouched");
  // Deterministic: the table's order does not change the result; a second merge is idempotent.
  expect(secure_frontend::apply_family_environment(generic, environment, test_gpu_rows_reversed) == expected, "table order does not matter");
  auto again = expected;
  expect(secure_frontend::merge_forced_environment(again, test_gpu_rows(environment)) && again == expected, "a second merge is idempotent");
  auto untouched = kPinnedWindowsBlock;
  expect(secure_frontend::merge_forced_environment(untouched, {}) && untouched == kPinnedWindowsBlock, "an empty table changes nothing");
}

// Fix round 1, item 1: the controller-ruled forcible set, each name on its own.
void sp6_env_forcible_names_are_the_ruled_set() {
  namespace parity = secure_frontend::parity;
  // SP6 §4.9's ruling, as amended by D-7 (2026-10-21): the safety constants,
  // the two GPU selectors, and every typed engine_env name of the GLM parity
  // table — the table is the allowlist (one source of truth). Rejected names
  // stay out; every other STRATA_*/MAYA_* name stays out.
  std::vector<std::string> ruled{"STRATA_GLM_SLOTS", "STRATA_GLM_LOOP_THINK", "STRATA_GLM_LOOP_ANSWER",
                                 "STRATA_GLM_SPLIT", "STRATA_WATCHDOG_S", "CUDA_DEVICE_ORDER", "CUDA_VISIBLE_DEVICES"};
  for (const auto name : parity::maya_env_knobs()) {
    const auto* row = parity::maya_parity_row(name);
    if (row == nullptr) continue;
    const bool forced = row->disposition == parity::ParityDisposition::typed &&
                        row->destination == parity::ParityDestination::engine_env;
    const bool listed = std::find(ruled.begin(), ruled.end(), std::string{name}) != ruled.end();
    if (forced && !listed) ruled.emplace_back(name);
    if (!forced && listed)
      expect(false, "a rejected or non-engine_env inventory name is on the ruled seed: " + std::string{name});
  }
  expect(secure_frontend::kFamilyForcibleEnvironment.size() == ruled.size(), "the forcible list is exactly the ruled set");
  for (const auto& name : ruled) {
    const bool listed = std::find(secure_frontend::kFamilyForcibleEnvironment.begin(),
                                  secure_frontend::kFamilyForcibleEnvironment.end(), std::string_view{name}) !=
                        secure_frontend::kFamilyForcibleEnvironment.end();
    expect(listed, "forcible: " + name);
    auto block = kPinnedWindowsBlock;
    FamilyEnvironmentRows rows{.forced = {{name, "0"}}};
    expect(secure_frontend::merge_forced_environment(block, rows) && block.at(name) == "0", "forcible renders: " + name);
  }
}

// Fix round 1, item 2: a value derived from the EngineEnvironment, through the real renderer.
void sp6_env_hook_derives_values_from_environment() {
  auto environment = representative_windows_environment();  // watchdog_io_s = 5
  auto block = secure_frontend::apply_family_environment(secure_frontend::render_windows_environment(environment, kPinnedSystem),
                                                         environment, test_derived_rows);
  expect(block.has_value() && block->at("CUDA_VISIBLE_DEVICES") == "5" && block->at("CUDA_DEVICE_ORDER") == "PCI_BUS_ID",
         "the hook derives CUDA_VISIBLE_DEVICES from the environment");
  environment.watchdog_io_s.reset();
  block = secure_frontend::apply_family_environment(secure_frontend::render_windows_environment(environment, kPinnedSystem), environment,
                                                    test_derived_rows);
  expect(block.has_value() && block->at("CUDA_VISIBLE_DEVICES") == "0", "an absent source renders 0");
  // The native renderer takes the same hook.
  const auto native = secure_frontend::render_family_environment(environment, test_derived_rows);
  expect(native.has_value() && native->at("CUDA_VISIBLE_DEVICES") == "0", "render_family_environment applies the hook");
}

// Fix round 1, item 4: the fixed suppressible set.
void sp6_env_hook_suppresses_only_optional_rows() {
  const auto environment = representative_windows_environment();
  const auto block = secure_frontend::apply_family_environment(
      secure_frontend::render_windows_environment(environment, kPinnedSystem), environment, test_suppress_rows);
  auto expected = kPinnedWindowsBlock;
  expected.erase("STRATA_WATCHDOG_IO_S");
  expected.erase("STRATA_HIPBLASLT_TUNING");
  expected["STRATA_WATCHDOG_S"] = "0";
  expect(block == expected, "the hook suppresses STRATA_WATCHDOG_IO_S and STRATA_HIPBLASLT_TUNING");
  for (const std::string_view name : {"STRATA_WATCHDOG_IO_S", "STRATA_HIPBLASLT_TUNING", "STRATA_WATCHDOG_S"}) {
    auto rows_block = kPinnedWindowsBlock;
    expect(secure_frontend::merge_forced_environment(rows_block, {.suppressed = {std::string(name)}}) && !rows_block.contains(std::string(name)),
           "suppressible: " + std::string(name));
  }
  for (const std::string_view name : {"PATH", "TEMP", "TMP", "CUDA_CACHE_PATH", "SystemRoot", "LD_LIBRARY_PATH", "TMPDIR", "CUDA_VISIBLE_DEVICES", ""}) {
    auto rows_block = kPinnedWindowsBlock;
    expect(!secure_frontend::merge_forced_environment(rows_block, {.suppressed = {std::string(name)}}) && rows_block == kPinnedWindowsBlock,
           "not suppressible: " + std::string(name));
  }
  // Ambiguous: a row both forced and suppressed, or suppressed twice.
  auto both = kPinnedWindowsBlock;
  expect(!secure_frontend::merge_forced_environment(both, {.forced = {{"STRATA_WATCHDOG_S", "0"}}, .suppressed = {"STRATA_WATCHDOG_S"}}) &&
             both == kPinnedWindowsBlock,
         "forced and suppressed at once is refused");
  auto twice = kPinnedWindowsBlock;
  expect(!secure_frontend::merge_forced_environment(twice, {.suppressed = {"STRATA_WATCHDOG_S", "STRATA_WATCHDOG_S"}}) &&
             twice == kPinnedWindowsBlock,
         "a duplicate suppression is refused");
}

void sp6_env_forced_values_reject_never_list() {
  using Row = secure_frontend::ForcedEnvironmentValue;
  const std::array<std::array<Row, 2>, 10> rejected{{
      {{{"CUDA_VISIBLE_DEVICES", "0"}, {"LD_PRELOAD", "/tmp/x.so"}}},  // a valid row first: all-or-nothing
      {{{"CUDA_DEVICE_ORDER", "PCI_BUS_ID"}, {"LD_AUDIT", "/tmp/x.so"}}},
      {{{"CUDA_VISIBLE_DEVICES", "0"}, {"HIP_VISIBLE_DEVICES", "0"}}},
      {{{"CUDA_VISIBLE_DEVICES", "0"}, {"STRATA_API_KEY", "k"}}},
      {{{"CUDA_VISIBLE_DEVICES", "0"}, {"HOME", "/home/x"}}},
      {{{"CUDA_VISIBLE_DEVICES", "0"}, {"HTTPS_PROXY", "http://p"}}},
      {{{"CUDA_VISIBLE_DEVICES", "0"}, {"STRATA_GLM_DFLASH", "1"}}},  // not on the forcible list
      {{{"CUDA_VISIBLE_DEVICES", "0"}, {"PATH", "/tmp"}}},             // generic base rows are not forcible
      {{{"CUDA_VISIBLE_DEVICES", "0"}, {"CUDA_VISIBLE_DEVICES", "1"}}},  // a duplicate name is ambiguous
      {{{"CUDA_VISIBLE_DEVICES", "0"}, {"", "x"}}},
  }};
  for (const auto& table : rejected) {
    auto block = kPinnedPosixBlock;
    const FamilyEnvironmentRows rows{.forced = {table.begin(), table.end()}};
    expect(!secure_frontend::merge_forced_environment(block, rows), "rejected table (second row " + table[1].name + ")");
    expect(block == kPinnedPosixBlock, "a rejected table leaves the block unchanged (" + table[1].name + ")");
  }
  auto nul = kPinnedPosixBlock;
  expect(!secure_frontend::merge_forced_environment(nul, test_nul_rows({})) && nul == kPinnedPosixBlock, "a NUL in a value is refused");
  // Every never-list name except the two frontend-rendered GPU selectors (RF9) is refused.
  for (const std::string_view banned : secure_frontend::kNeverForwardedEnvironment) {
    auto block = kPinnedPosixBlock;
    const bool gpu_selector = banned == "CUDA_DEVICE_ORDER" || banned == "CUDA_VISIBLE_DEVICES";
    expect(secure_frontend::merge_forced_environment(block, {.forced = {{std::string(banned), "x"}}}) == gpu_selector,
           "never-list name: " + std::string(banned));
  }
  // An invalid derived table: the family step yields nothing (no partial block).
  const auto environment = representative_windows_environment();
  expect(!secure_frontend::apply_family_environment(kPinnedWindowsBlock, environment, test_duplicate_rows).has_value(),
         "a refused family merge yields no environment");
  expect(!secure_frontend::render_family_environment(environment, test_nul_rows).has_value(), "the native family renderer refuses too");
}

// Fix round 1, item 3: a refused family merge is a spawn failure with a fixed
// literal, and nothing is spawned (never with an empty environment).
void sp6_env_refused_family_is_spawn_failure() {
  const auto state = test_support::make_temp_dir("sp6-refused");
  auto plan = fake_plan({});
  plan.environment = secure_frontend::EngineEnvironment{
      .lib_dirs = {}, .temp_dir = state / "engine-tmp", .cuda_cache_dir = state / "cache" / "cuda"};
  for (const auto hook : {test_duplicate_rows, test_nul_rows}) {
    plan.family_hook = hook;
    test_support::LogCapture log;
    auto spawned = secure_frontend::make_stdio_process_factory(plan, log.sink())(1U);
    const auto* failure = std::get_if<secure_frontend::SpawnFailure>(&spawned);
    expect(failure != nullptr && failure->message == "engine environment refused", "a refused family merge is a spawn failure");
    expect(log.contains("engine generation 1: family environment refused"), "the fixed literal is logged");
    expect(!std::filesystem::exists(state / "engine-tmp" / "1"), "the generation's temp directory is removed");
  }
}

// SP5 M2 Task 4 (spec D-7, superseding A10's bound on SP6 §4.9): the glm family
// renders the closed typed child environment. The allowlist is the GLM parity
// table's typed engine_env set plus the SP6 safety constants and the two GPU
// selectors; the §4.9 safety values are forced defaults a typed row can move;
// unknown names never appear and the rejected DFLASH row never reaches the
// child (C-5). The exact-block assertions stand in for an env-echo child: the
// sp6 process case already proved hook rows reach a real child's environment.
void glm_child_env_is_exactly_the_named_typed_set() {
  namespace parity = secure_frontend::parity;
  const secure_frontend::EngineEnvironment environment{
      .lib_dirs = {std::filesystem::path{"/opt/cuda/lib"}},
      .temp_dir = std::filesystem::path{"/state/engine-tmp/1"},
      .cuda_cache_dir = std::filesystem::path{"/state/cache/cuda"},
      .family = secure_frontend::EngineFamily::glm,
  };
  const auto block = secure_frontend::render_family_environment(environment);
  expect(block.has_value(), "the glm family renders");
  if (!block) return;
  // The forced safety defaults (SP6 §4.9 as amended by D-7) arrive with forced values.
  for (const std::string_view name : {"STRATA_GLM_SLOTS", "STRATA_GLM_LOOP_THINK", "STRATA_GLM_LOOP_ANSWER",
                                      "STRATA_GLM_SPLIT", "STRATA_WATCHDOG_S"}) {
    const auto found = block->find(std::string{name});
    expect(found != block->end() && found->second == "0", "forced default: " + std::string{name});
  }
  // GPU selection is environment for this family only (serve/server.py:1280-1288,
  // SP6 §4.9 A8): no card named renders device 0.
  expect(block->count("CUDA_DEVICE_ORDER") == 1U && block->at("CUDA_DEVICE_ORDER") == "PCI_BUS_ID" &&
             block->count("CUDA_VISIBLE_DEVICES") == 1U && block->at("CUDA_VISIBLE_DEVICES") == "0",
         "the glm family renders the CUDA selectors");
  // Every typed engine_env name of the table is forceable: the table is the allowlist.
  for (const auto name : parity::maya_env_knobs()) {
    const auto* row = parity::maya_parity_row(name);
    if (row == nullptr || row->disposition != parity::ParityDisposition::typed ||
        row->destination != parity::ParityDestination::engine_env)
      continue;
    auto probe = *block;
    const secure_frontend::FamilyEnvironmentRows rows{.forced = {{std::string{name}, "7"}}};
    expect(secure_frontend::merge_forced_environment(probe, rows) && probe.at(std::string{name}) == "7",
           "the table's typed engine_env name is on the allowlist: " + std::string{name});
  }
  // Nothing else named STRATA_*/MAYA_* reaches the child.
  for (const auto& [name, value] : *block) {
    static_cast<void>(value);
    if (name.rfind("STRATA_", 0) == 0 || name.rfind("MAYA_", 0) == 0) {
      const auto* row = parity::maya_parity_row(name);
      expect(row != nullptr && row->disposition == parity::ParityDisposition::typed &&
                 row->destination == parity::ParityDestination::engine_env,
             "no non-table STRATA_*/MAYA_* name reaches the child: " + name);
    }
  }
}

// SP6 §4.9's safety values stay forced unless a typed row moves them; the
// glm family omits the rows MAYA v1.1.21 has no literal for (SP6 §4.9).
void forced_defaults_stay_forced_unless_the_table_moves_them() {
  auto environment = secure_frontend::EngineEnvironment{
      .lib_dirs = {},
      .temp_dir = std::filesystem::path{"/state/engine-tmp/1"},
      .cuda_cache_dir = std::filesystem::path{"/state/cache/cuda"},
      .watchdog_s = 180U,
      .watchdog_io_s = 5U,
      .family = secure_frontend::EngineFamily::glm,
  };
  const auto block = secure_frontend::render_family_environment(environment);
  expect(block.has_value(), "the glm family renders");
  if (!block) return;
  expect(block->count("STRATA_WATCHDOG_IO_S") == 0U, "the glm family suppresses the watchdog-IO row (SP6 §4.9: MAYA v1.1.21 has no such literal)");
  expect(block->at("STRATA_WATCHDOG_S") == "0", "the engine watchdog stays off (the M5 forced default overrides the generic row)");
  expect(block->at("STRATA_GLM_SLOTS") == "0" && block->at("STRATA_GLM_LOOP_THINK") == "0" &&
             block->at("STRATA_GLM_LOOP_ANSWER") == "0" && block->at("STRATA_GLM_SPLIT") == "0",
         "the §4.9 safety defaults stand when nothing moves them");
  // A typed row moves a forced default: replace, never duplicate (the same
  // merge gate the family hook renders through).
  auto probe = *block;
  const secure_frontend::FamilyEnvironmentRows rows{.forced = {{"STRATA_GLM_SLOTS", "2"}, {"STRATA_GLM_SPLIT", "7,12"}}};
  expect(secure_frontend::merge_forced_environment(probe, rows) && probe.at("STRATA_GLM_SLOTS") == "2" &&
             probe.at("STRATA_GLM_SPLIT") == "7,12",
         "a typed row moves a forced default");
  // An unknown name fails closed: the merge refuses, the block stays as it was.
  auto guarded = *block;
  const secure_frontend::FamilyEnvironmentRows sneaky{.forced = {{"STRATA_GLM_MADE_UP", "1"}}};
  expect(!secure_frontend::merge_forced_environment(guarded, sneaky) && guarded.count("STRATA_GLM_MADE_UP") == 0U,
         "an unknown name is refused (the closed list)");
  // Through the family step too: the whole environment is lost, never a partial one.
  const auto refused = secure_frontend::apply_family_environment(
      secure_frontend::render_engine_environment(environment), environment,
      [](const secure_frontend::EngineEnvironment&) {
        return secure_frontend::FamilyEnvironmentRows{.forced = {{"STRATA_GLM_MADE_UP", "1"}}};
      });
  expect(!refused.has_value(), "an unknown name through the family step refuses the whole environment (never a partial spawn)");
}

// C-5: STRATA_GLM_DFLASH stays a rejected row on the A7 licence ground: the
// allowlist keeps it out even though the table now opens every other documented
// knob (the config surface has no dflash key to set — the typed_config unknown-
// key rule and the parity meta-test dflash_is_rejected_on_licence cover that).
void dflash_never_reaches_the_child() {
  namespace parity = secure_frontend::parity;
  const auto* row = parity::maya_parity_row("STRATA_GLM_DFLASH");
  expect(row != nullptr && row->disposition == parity::ParityDisposition::rejected, "the DFLASH row is rejected (C-5)");
  const auto& forcible = secure_frontend::kFamilyForcibleEnvironment;
  expect(std::find(forcible.begin(), forcible.end(), std::string_view{"STRATA_GLM_DFLASH"}) == forcible.end(),
         "DFLASH is not on the forcible list (the closed list keeps the table's rejection)");
  const secure_frontend::EngineEnvironment environment{
      .lib_dirs = {},
      .temp_dir = std::filesystem::path{"/state/engine-tmp/1"},
      .cuda_cache_dir = std::filesystem::path{"/state/cache/cuda"},
      .family = secure_frontend::EngineFamily::glm,
  };
  const auto block = secure_frontend::render_family_environment(environment);
  expect(block.has_value(), "the glm family renders");
  if (!block) return;
  expect(block->count("STRATA_GLM_DFLASH") == 0U, "the rendered glm environment never carries DFLASH");
  auto probe = *block;
  const secure_frontend::FamilyEnvironmentRows rows{.forced = {{"STRATA_GLM_DFLASH", "1"}}};
  expect(!secure_frontend::merge_forced_environment(probe, rows), "forcing DFLASH is refused: not on the allowlist");
}

// Fix round 1, item 2: the derived value reaches a real child through the spawn path.
void sp6_env_derived_value_reaches_child() {
  const auto state = test_support::make_temp_dir("sp6-derived");
  auto plan = fake_plan({});
  plan.environment = secure_frontend::EngineEnvironment{
      .lib_dirs = {}, .temp_dir = state / "engine-tmp", .cuda_cache_dir = state / "cache" / "cuda", .watchdog_io_s = 3U};
  plan.family_hook = test_derived_rows;
  auto spawned = secure_frontend::make_stdio_process_factory(plan)(1U);
  auto* process = std::get_if<std::unique_ptr<secure_frontend::SupervisedProcess>>(&spawned);
  expect(process != nullptr, "spawn");
  if (process == nullptr) return;
  auto& transport = (*process)->transport();
  expect(transport.read_line(5s).line == "READY 4096 stop", "READY");
  const auto dump = test_support::request_dump(transport, 5s);
  expect(dump.has_value(), "DUMP");
  if (dump) {
    const auto has = [&](const std::string& entry) { return std::find(dump->env.begin(), dump->env.end(), entry) != dump->env.end(); };
    expect(has("CUDA_VISIBLE_DEVICES=3") && has("CUDA_DEVICE_ORDER=PCI_BUS_ID"), "the derived rows reach the child");
  }
  (*process)->kill_group();
  for (int step = 0; step < 500 && !(*process)->try_reap(); ++step) std::this_thread::sleep_for(10ms);
  (*process)->release_resources(1000ms);
}

// SP5 M2 Task 6: the fake engine's --dialect=maya mode emits the vendored
// NOTE/STAT/5-field-PP framing, and it rides Task 7's composed GLM launch argv.
// The proof is adversarial: under the strict strata dialect those records are
// unknown lines and the generation dies (protocol_failed kill); under maya_118
// the same fake completes. A --dialect=maya that emitted nothing fails the
// first half — the fake's records are what is proven, not the client's.
void fake_maya_dialect_round_trips_only_under_the_maya_dialect() {
  const auto tokenizer = test_support::byte_tokenizer();
  const secure_frontend::ChatCompletionRequest request{"fake-model", {{"user", "Hi"}}, 2U};
  const std::vector<std::string> argv{"--dialect=maya", "--glm-pack", "/models/glm", "--max-context", "32768",
                                      "--prefill", "auto", "--kv-streaming", "on", "--tokens=4"};
  {  // strata dialect: the maya records are unknown lines.
    secure_frontend::SupervisorConfig strict;  // flavor stays strata_0143
    strict.policy.backoff_initial = 20ms;
    test_support::LogCapture log;
    secure_frontend::EngineSupervisor supervisor(strict, secure_frontend::make_stdio_process_factory(fake_plan(argv)),
                                                 std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
    expect(supervisor.start(), "the strict supervisor starts");
    expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "the fake reaches READY");
    const auto outcome = secure_frontend::execute_with_lease(supervisor, request, tokenizer);
    expect(outcome && std::holds_alternative<secure_frontend::ChatExecutionError>(*outcome),
           "the maya records fail closed against the strata dialect");
    expect(test_support::wait_generation_ready(supervisor, 2U, 10s), "the killed generation is replaced");
    expect(log.contains("engine generation 1 ended: killed_by_supervisor(protocol_failed)"),
           "the unknown record kills the generation");
    supervisor.shutdown(5s);
  }
  {  // maya dialect: the same records complete the generation.
    secure_frontend::SupervisorConfig maya;
    maya.flavor = secure_frontend::EngineProtocolFlavor::maya_118;
    test_support::LogCapture log;
    secure_frontend::EngineSupervisor supervisor(maya, secure_frontend::make_stdio_process_factory(fake_plan(argv)),
                                                 std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
    expect(supervisor.start(), "the maya supervisor starts");
    expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "the maya fake reaches READY");
    const auto outcome = secure_frontend::execute_with_lease(supervisor, request, tokenizer);
    const auto* completion = outcome ? std::get_if<secure_frontend::ChatExecutionResult>(&*outcome) : nullptr;
    expect(completion != nullptr && completion->generated_tokens == 2U, "the maya dialect serves the generation");
    expect(!log.contains("engine generation 1 ended: killed_by_supervisor(protocol_failed)"),
           "nothing is unknown under maya_118");
    supervisor.shutdown(5s);
  }
}

// Issue 27 (docs/validation/2026-10-21-windows-gpu-session.md): Maya v1.1.21 prints its startup
// INFO and one VRAM record per GPU part before READY (maya-ref src/program/generate.cpp:1044,
// :1054-1062). Under maya_118 the engine reaches READY and serves; under the strict dialect the
// unknown record ends the start as protocol_failed and READY is never reported.
void fake_maya_vram_records_reach_ready_only_under_the_maya_dialect() {
  const auto tokenizer = test_support::byte_tokenizer();
  const secure_frontend::ChatCompletionRequest request{"fake-model", {{"user", "Hi"}}, 2U};
  const std::vector<std::string> argv{"--dialect=maya", "--maya-vram", "--glm-pack", "/models/glm", "--max-context", "32768",
                                      "--prefill", "auto", "--kv-streaming", "on", "--tokens=4"};
  {  // strata dialect: the VRAM record before READY is unknown.
    secure_frontend::SupervisorConfig strict;
    strict.policy.backoff_initial = 20ms;
    test_support::LogCapture log;
    secure_frontend::EngineSupervisor supervisor(strict, secure_frontend::make_stdio_process_factory(fake_plan(argv)),
                                                 std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
    expect(supervisor.start(), "the strict supervisor starts");
    expect(!test_support::wait_generation_ready(supervisor, 1U, 2s), "a strict client never reports READY past a VRAM record");
    // LogCapture matches whole lines.
    expect(log.wait_for("engine generation 1 ended: killed_by_supervisor(protocol_failed)", 5s),
           "the unknown pre-READY record kills the start");
    supervisor.shutdown(5s);
  }
  {  // maya dialect: the same startup reaches READY and serves.
    secure_frontend::SupervisorConfig maya;
    maya.flavor = secure_frontend::EngineProtocolFlavor::maya_118;
    test_support::LogCapture log;
    secure_frontend::EngineSupervisor supervisor(maya, secure_frontend::make_stdio_process_factory(fake_plan(argv)),
                                                 std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
    expect(supervisor.start(), "the maya supervisor starts");
    expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "the v1.1.21 startup reaches READY under maya_118");
    const auto outcome = secure_frontend::execute_with_lease(supervisor, request, tokenizer);
    const auto* completion = outcome ? std::get_if<secure_frontend::ChatExecutionResult>(&*outcome) : nullptr;
    expect(completion != nullptr && completion->generated_tokens == 2U, "and serves the generation");
    expect(!log.contains("engine generation 1 ended: killed_by_supervisor(protocol_failed)"), "nothing is unknown under maya_118");
    supervisor.shutdown(5s);
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  return secure_frontend_test::run_cases(argc, argv, {{"supervisor_health_literals_per_state", supervisor_health_literals_per_state},
                                                {"proc04_restart_backoff_then_circuit_open", proc04_restart_backoff_then_circuit_open},
                                                {"supervisor_captures_startup_readings_at_ready", supervisor_captures_startup_readings_at_ready},
                                                {"proc04_healthy_window_resets_failure_count", proc04_healthy_window_resets_failure_count},
                                                {"proc04_recovers_after_transient_crash", proc04_recovers_after_transient_crash},
                                                {"proc04_missing_engine_opens_circuit_without_spinning", proc04_missing_engine_opens_circuit_without_spinning},
                                                {"proc04_client_err_lines_open_circuit_until_restart", proc04_client_err_lines_open_circuit_until_restart},
                                                {"supervisor_retires_ended_generation_on_its_own_thread", supervisor_retires_ended_generation_on_its_own_thread},
                                                {"proc04_monitor_detects_idle_crash", proc04_monitor_detects_idle_crash},
                                                {"proc04_monitor_fallback_without_pidfd", proc04_monitor_fallback_without_pidfd},
                                                {"proc04_backoff_survives_stale_wake", proc04_backoff_survives_stale_wake},
                                                {"proc04_fatal_prefix_kills_engine", proc04_fatal_prefix_kills_engine},
                                                {"proc04_fatal_line_before_ready_kills_as_fatal_line", proc04_fatal_line_before_ready_kills_as_fatal_line},
                                                {"proc04_ready_timeout_kills_and_counts", proc04_ready_timeout_kills_and_counts},
                                                {"proc04_drain_failure_kills_and_restarts", proc04_drain_failure_kills_and_restarts},
                                                {"proc04_no_leak_across_restarts", proc04_no_leak_across_restarts},
                                                {"sp3_d2_kill_reason_follows_stop_cause", sp3_d2_kill_reason_follows_stop_cause},
                                                {"sp3_d2_client_stop_drain_failure_restarts_uncounted", sp3_d2_client_stop_drain_failure_restarts_uncounted},
                                                {"sp3_d2_timeout_stop_drain_failure_still_counts", sp3_d2_timeout_stop_drain_failure_still_counts},
                                                {"sp3_d2_client_stop_counts_when_engine_ended_itself", sp3_d2_client_stop_counts_when_engine_ended_itself},
                                                {"sp3_d2_uncounted_end_resets_count_after_healthy_window", sp3_d2_uncounted_end_resets_count_after_healthy_window},
                                                {"sp3_d2_uncounted_streak_is_bounded", sp3_d2_uncounted_streak_is_bounded},
                                                {"sp3_d2_done_resets_uncounted_streak", sp3_d2_done_resets_uncounted_streak},
                                                {"proc04_unreaped_child_opens_circuit", proc04_unreaped_child_opens_circuit},
                                                {"proc06_nnp_hint_when_exit_status_lags", proc06_nnp_hint_when_exit_status_lags},
                                                {"supervisor_unreaped_at_shutdown_reports_engine_left", supervisor_unreaped_at_shutdown_reports_engine_left},
                                                {"supervisor_exception_opens_circuit", supervisor_exception_opens_circuit},
                                                {"supervisor_shutdown_sends_eof_first_then_kills", supervisor_shutdown_sends_eof_first_then_kills},
                                                {"supervisor_lease_outliving_shutdown_is_safe", supervisor_lease_outliving_shutdown_is_safe},
                                                {"supervisor_wait_wakes_on_lease_release", supervisor_wait_wakes_on_lease_release},
                                                {"supervisor_wait_wakes_on_shutdown", supervisor_wait_wakes_on_shutdown},
                                                {"supervisor_serving_api_is_thread_safe", supervisor_serving_api_is_thread_safe},
                                                {"proc06_refuses_privileged_start", proc06_refuses_privileged_start},
                                                {"engine_environment_uses_cache_root_when_set", engine_environment_uses_cache_root_when_set},
                                                {"sp6_env_strata_family_renders_unchanged", sp6_env_strata_family_renders_unchanged},
                                                {"sp6_env_forced_values_override_and_add", sp6_env_forced_values_override_and_add},
                                                {"sp6_env_forced_values_reject_never_list", sp6_env_forced_values_reject_never_list},
                                                {"sp6_env_forcible_names_are_the_ruled_set", sp6_env_forcible_names_are_the_ruled_set},
                                                {"glm_child_env_is_exactly_the_named_typed_set", glm_child_env_is_exactly_the_named_typed_set},
                                                {"forced_defaults_stay_forced_unless_the_table_moves_them",
                                                 forced_defaults_stay_forced_unless_the_table_moves_them},
                                                {"dflash_never_reaches_the_child", dflash_never_reaches_the_child},
                                                {"sp6_env_hook_derives_values_from_environment", sp6_env_hook_derives_values_from_environment},
                                                {"sp6_env_hook_suppresses_only_optional_rows", sp6_env_hook_suppresses_only_optional_rows},
                                                {"sp6_env_refused_family_is_spawn_failure", sp6_env_refused_family_is_spawn_failure},
                                                {"idle_unload_tears_down_then_respawns_on_next_request", idle_unload_tears_down_then_respawns_on_next_request},
                                                {"idle_unload_never_fires_during_a_generation", idle_unload_never_fires_during_a_generation},
                                                {"lazy_load_defers_the_first_spawn", lazy_load_defers_the_first_spawn},
                                                {"sp6_env_derived_value_reaches_child", sp6_env_derived_value_reaches_child},
                                                {"fake_maya_dialect_round_trips_only_under_the_maya_dialect",
                                                 fake_maya_dialect_round_trips_only_under_the_maya_dialect},
                                                {"fake_maya_vram_records_reach_ready_only_under_the_maya_dialect",
                                                 fake_maya_vram_records_reach_ready_only_under_the_maya_dialect},
                                                {"proc02_child_env_is_exact_allowlist", proc02_child_env_is_exact_allowlist},
#if !defined(_WIN32)
                                                {"shutdown_signal_preserves_errno", shutdown_signal_preserves_errno},
                                                {"supervisor_start_refuses_without_wake", supervisor_start_refuses_without_wake},
                                                {"proc05_setsid_grandchild_is_reaped", proc05_setsid_grandchild_is_reaped},
                                                {"proc05_setsid_grandchild_is_reaped_at_shutdown", proc05_setsid_grandchild_is_reaped_at_shutdown},
                                                {"proc05_cgroup_kill_when_delegated", proc05_cgroup_kill_when_delegated},
#endif
#if defined(_WIN32)
                                                {"proc04_windows_abort_does_not_stall", proc04_windows_abort_does_not_stall},
                                                {"proc04_windows_crash_does_not_stall", proc04_windows_crash_does_not_stall},
                                                {"proc04_windows_write_timeout_kills_engine", proc04_windows_write_timeout_kills_engine},
#endif
                                                {"proc04_slow_stdin_write_bounded_by_request_deadline", proc04_slow_stdin_write_bounded_by_request_deadline},
                                                {"vision_child_launches_with_pinned_args_when_enabled", vision_child_launches_with_pinned_args_when_enabled},
                                                {"vision_child_argv_matches_maya_vision_grammar_at_pin", vision_child_argv_matches_maya_vision_grammar_at_pin},
                                                {"vision_child_argv_follows_upstreams_strata_vision", vision_child_argv_follows_upstreams_strata_vision},
                                                {"vision_child_respects_resource_limits", vision_child_respects_resource_limits},
                                                {"vision_encode_round_trip_at_the_pin", vision_encode_round_trip_at_the_pin},
                                                {"vision_encode_failures_take_the_pinned_paths", vision_encode_failures_take_the_pinned_paths},
                                               }, failures);
}
