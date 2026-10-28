# SP2 Portable Supervision Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace SP0's start-once engine launch with an `EngineSupervisor` that owns every engine generation for the frontend's lifetime (eager READY, exit-status capture and classification, bounded restart with backoff, a circuit breaker, a monitor that sees death between requests), guarantees that no descendant outlives the frontend however it stops, gives the child a typed environment, a reset signal state, a scoped cwd, an owner-only pipe and no stray handles, and pins every `PROC-01`..`PROC-06`, `EXT-01`, `EXT-02` row to a CTest test named by acceptance ID.

**Architecture:** One long-lived supervisor thread is the only code that forks, reaps (`try_reap`), and kills engine processes; the single-threaded HTTP serving loop borrows the current generation through a `Lease` and reports an unusable generation with `report_fatal`. Each spawn builds a fresh `EngineInstance` (process + protocol client + chat service) behind a `shared_ptr`. Linux cleanup is process-group SIGKILL + `PR_SET_CHILD_SUBREAPER` sweep + `PR_SET_PDEATHSIG`, with cgroup v2 `cgroup.kill` only when a delegated cgroup is detected; Windows cleanup is the kill-on-close Job (now also `DIE_ON_UNHANDLED_EXCEPTION`) plus a console-control handler. A standalone test-only `strata-supervision-fake-engine` and a server harness drive every PROC/EXT test.

**Tech Stack:** C++20 (g++ 11.4 locally, clang in CI), CMake 4.4.3 + Ninja 1.13.2 from `.venv-dev/`, llvm-mingw 20261006 (clang 23.1.3) cross builds, wine 6.0.3 (smoke only), GitHub Actions MSYS2 `CLANG64` (authoritative Windows runtime). OS APIs only: Linux `prctl`, `pidfd_open`, `/proc`, cgroup v2; Win32 Job Objects, named pipes with explicit DACLs, overlapped I/O, console control handlers, `BCryptGenRandom`, `NtQueryInformationProcess`.

**Spec:** `docs/superpowers/specs/2026-10-13-sp2-portable-supervision-design.md` (APPROVED by the owner 2026-10-13 19:08 UTC; owner decisions in its Status line). Fact base: `docs/superpowers/research/sp2-supervision-proc.md`. House style and the launch code this plan extends: `docs/superpowers/plans/2026-10-13-sp0-foundation-hardening.md` (Tasks 10 and 11).

## Global Constraints

- **SP1 coordination (shared pre-task):** SP1 Task 2 (test registration: `secure_frontend_add_cases(<binary> <case>... [ARGS <arg>...] [LABELS <label>...] [TIMEOUT <seconds>])` with tests named `<binary>.<case>`, `secure_frontend_add_test_executable(<name> <source> <libraries>...)`, the `windows-runtime` label through `${SECURE_FRONTEND_WINDOWS_RUNTIME}`, the wine test preset's `"filter": {"exclude": {"label": "windows-runtime"}}` (the preset form of `ctest -LE windows-runtime`) plus its `WINEPREFIX` environment, and `tests/support/test_support.hpp` with `secure_frontend_test::run_cases`, `make_scratch_directory`, `make_private_directory`, `write_file`, `read_file`) lands on `main` BEFORE SP2 Task 1 as a shared pre-task; SP2 consumes it and defines none of it. Before Task 1 the SP2 branch is rebased onto that `main` (Task 1 Step 0 checks it). Every SP2 CTest test is therefore named `<binary>.<case>` (for example `secure_frontend_engine_supervisor_tests.proc04_restart_backoff_then_circuit_open`); `ctest -R <case>` still selects it because `-R` is an unanchored regex, and every `set_tests_properties` below uses the full `<binary>.<case>` name.
- **SP1 merge boundary:** SP1 Task 5 replaces `secure_server_main.cpp` with a table-driven `kFlags` parser (`src/server_cli.cpp`) and typed `FrontendConfig`. Tasks 6b, 15 (its `main` wiring step), 16, 18 (Step 4), 19, 25 and 26 change `secure_server_main.cpp` and carry the banner **WAITS FOR SP1 MERGE**; they run only after SP1 has merged, re-anchored on SP1's files as their notes describe. Their code blocks show the behaviour against the SP0 hand parser the cited lines refer to; the SP1-shaped code (schema keys, `kFlags` rows, `FrontendConfig` fields) for every SP2 option is in Task 29, and each WAITS task applies its own row from there. All other tasks touch no `secure_server_main.cpp` and run in parallel with SP1. **Execution order is the order the tasks appear in this file:** 1, 2, 3, 4, 5, 6a, 7–14, 17, 20–24 (parallel-safe), then 6b, 15, 16, 18, 19, 25, 26, 27, 28 (after the SP1 merge), then 29. Task numbers are stable IDs (every cross-reference uses them), not positions.
- **Test-only fake engine:** SP2's fake is `secure_frontend/tests/supervision_fake_engine_main.cpp`, CMake target `strata-supervision-fake-engine` (and its Windows twin `strata-supervision-fake-engine-elevated`). SP1 owns `tests/fake_engine_main.cpp` / `secure_frontend_fake_engine`; SP2 never edits it (Task 29 only adds a `/health` poll to SP1's e2e harness).
- **Where to work:** the git worktree `/opt/projects/ai/.worktrees/sp2` on branch `sp2-portable-supervision` (`.venv-dev` and `.toolchains` are symlinks to the main checkout). Every relative path in this plan is relative to that worktree root; `secure_frontend/` means `/opt/projects/ai/.worktrees/sp2/secure_frontend`. Line numbers cited for existing code are those of commit `7ddcd755` (the worktree's base before the SP1 Task 2 rebase). Files SP1 Task 2 rewrites (the tests block of `secure_frontend/CMakeLists.txt`, the test presets in `CMakePresets.json`, the `main()` of every file in `secure_frontend/tests/`) are cited by quoted text, never by line; a `CMakeLists.txt:<n>` citation is a `7ddcd755` line number that SP1 Task 2's helper block (inserted after line 31) shifts down, so locate it by the quoted target name; after an earlier task edits a file, find the cited code by its quoted text.
- **Commands:** always `export PATH=/opt/projects/ai/.venv-dev/bin:$PATH` first, then from `/opt/projects/ai/.worktrees/sp2/secure_frontend`: `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc`. Windows cross build: `cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross` (must compile with `-Werror`); `ctest --preset windows-llvm-mingw-cross` runs the `.exe` files under wine with SP1's `WINEPREFIX`. Every direct wine call in this plan is written `WINEPREFIX=$PWD/out/wineprefix wine out/build/windows-llvm-mingw-cross/<binary>.exe --case <case>` (run from `secure_frontend/`, the same prefix the preset uses).
- **Test counts:** with SP1's per-case registration the starting totals are not 20/19; Task 1 Step 0 records them as N₀ (linux-gcc) and W₀ (wine). Each task's run step states its total as "previous total + k", where k is the number of CTest tests that task registers (wine: the unlabeled ones), and the handoff row records the exact `tests passed` line observed. Two tests may report "Skipped" (below); a skip is not a failure.
- **Clang locally:** clang is not installed on the dev host, so there is no local `linux-clang` run. The `windows-llvm-mingw-cross` build (clang 23.1.3, `-Werror`) catches most clang-only diagnostics; the CI `linux-clang` job is authoritative for Linux clang.
- **Aggregates stay designated-initializer compatible:** every NEW member of an existing aggregate carries a default member initializer: `EngineCommand::cgroup_procs{}` (Task 16), `EngineCommand::working_directory{}` (Task 25), `EngineCommand::no_new_privileges{false}` (Task 26), `EngineLaunchPlan::environment{}` (Task 18), `EngineLaunchPlan::use_delegated_cgroup{false}` (Task 16), `EngineEnvironment::{hipblaslt_tuning, watchdog_s, watchdog_io_s}{}` (Task 18), `test_support::ServerOptions::state_dir{}` (Task 18; every `ServerOptions` member has one). Existing designated initializers such as `{.executable = ..., .arguments = ..., .environment = {}}` omit the new members and must keep compiling under `-Werror` (`-Wmissing-field-initializers` on g++ 11.4, `-Wmissing-designated-field-initializers` on clang 23; both verified: an omitted member with a `{}` default initializer is not diagnosed, one without it is).
- **Windows truth (spec §2):** Job Objects, handles, DACLs, console events and crash exit codes are claimed only from the blocking MSYS2 `CLANG64` CI job. wine is a smoke test. Every test whose assertions depend on Windows runtime semantics is registered with `LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME}` (SP1's variable: `windows-runtime` on `WIN32`, empty elsewhere); SP1's wine preset excludes that label; the MSYS2 job runs everything. For Windows-only (W) tasks RED/GREEN means: cross-compile with `-Werror`, run the test exe under wine and **record** the result in the handoff row, and mark the row "authoritative in MSYS2 — pending the owner's push". W rows count as done only when the MSYS2 job is green.
- **Test registration:** new test binaries are framework-free `main()` programs with `int failures` / `void expect(bool, const std::string&)` in an anonymous namespace whose `main` returns `secure_frontend_test::run_cases(argc, argv, {{"<case>", <function>}, ...}, failures)` (SP1). They are built with `secure_frontend_add_test_executable(...)` (which also calls `secure_frontend_set_warnings`) plus SP2's `secure_frontend_test_paths(<target>)` (Task 1: fake/server paths), and registered with `secure_frontend_add_cases(<binary> <case>... TIMEOUT 120 [LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME}])`. A case that cannot run on this host calls `test_support::skip("reason")` (Task 1, `tests/support/test_paths.hpp`): it prints `SKIP: <reason>`, removes SP1's scratch directories and exits 77, and the test carries `SKIP_RETURN_CODE 77`. Only two tests may skip, each with its reason printed: `secure_frontend_engine_child_tests.proc03_other_uid_cannot_open_engine_fd` (no second uid outside CI) and `secure_frontend_engine_supervisor_tests.proc05_cgroup_kill_when_delegated` (no delegated cgroup v2). Every process-level test gets `TIMEOUT 120` (the K2 write-timeout test `TIMEOUT 300`).
- **Test fixtures meet SP1's rules:** the harness token is 43 base64url characters (≥ 32 bytes); every state/model/tokenizer directory a test creates is 0700 (`secure_frontend_test::make_private_directory`, or `make_temp_dir`, which builds on SP1's `make_scratch_directory`) and every fixture file 0600 (`secure_frontend_test::write_file`). Fixtures live below SP1's scratch root (`<build>/test-tmp`, Windows the per-user `%TEMP%`), never `/tmp`.
- **Elevation in tests (no override flag):** on Windows the harness (Task 3) always starts `strata-secure-server` with a LUA-restricted copy of its own token (`CreateRestrictedToken(LUA_TOKEN)` + `CreateProcessAsUserW`), grants that token's user full control on every directory the tests create (objects an elevated admin creates are owned by Administrators, which the LUA token holds deny-only), and asserts the server's token is non-elevated. If the LUA token still reports elevation it falls back to `SaferComputeTokenFromLevel(SAFER_LEVELID_NORMALUSER)`; if that is still elevated the harness fails with a fixed message and the executor STOPS and asks the owner. There is NO runtime `--allow-elevated-for-tests` (or similar) flag anywhere in SP2.
- **Owner-acknowledged deviations (recorded, not hidden):** (1) G1 asserts a *relative* identity rule — the engine is never MORE elevated and never at a HIGHER integrity level than the frontend (Windows), and has the frontend's uid/gid (POSIX) — instead of the spec's absolute "not elevated", because the CI runners and any elevated developer shell run the test binary itself elevated (Task 25). (2) G3 (an engine that requires elevation is `spawn_failed`, never `runas`) is UNVERIFIED in CI (the runners are elevated, so error 740 cannot occur) and under wine (no UAC). Its code ships and its case stays in `secure_frontend_engine_child_tests` for manual runs, but it is not registered with CTest and is not a skipped test; the handoff records it as a validation-kit item for the owner's offline Windows box (Task 25).
- **No third-party dependencies** in `secure_frontend/`; standard library and OS APIs only. New system libraries allowed on Windows: `shell32`, `advapi32`, `bcrypt`, `ntdll` (via `GetProcAddress` only), `ws2_32`.
- **Off limits:** no change under `upstream/` (the submodule) or our root `CMakeLists.txt` (`secure_frontend/src` and `secure_frontend/include` are the frontend and are in scope). Stop and report to the owner before any such change, before any second-user test outside CI, before any GPU run, and before any `/health` change beyond adding `recovery`.
- **No `rm -rf`, no `sudo`, nothing installed** outside `.venv-dev/` and `.toolchains/`. (The only `sudo` in SP2 is `sudo -n -u nobody` inside the GitHub Linux runner for R2, Task 24.)
- **SP1-bound values:** until Task 29 every value SP1 makes typed configuration is a named constant in exactly one header, `secure_frontend/include/secure_frontend/supervision_defaults.hpp` (Task 6a); Task 29 folds it into SP1's `config_defaults.hpp`. The SP2 options (`--state-dir`, `--engine-lib-dir`, `--gpu`, `--allow-new-privileges`) become SP1 schema keys and `kFlags` rows (`roots.state`, `engine.lib_dirs`, `engine.gpu`, `engine.allow_new_privileges`; Task 29).
- **Warnings are errors:** every new target gets `secure_frontend_set_warnings` — test binaries through `secure_frontend_add_test_executable`, and explicitly the test-support library and `strata-supervision-fake-engine`.
- **Thread rule (spec §3.1):** only the supervisor thread forks, reaps (`try_reap`, `waitpid`, process-handle waits) and kills engine processes. The serving thread uses only `EngineSupervisor::acquire`, the `Lease` it gets (protocol read/write), `report_fatal`, and `snapshot`. Nothing on the request path calls `EngineStdioProcess::running()`.
- **Privacy:** log lines are fixed literals (`engine generation <n> ended: <kind>`); never print tokens, prompts, engine paths or exit codes to clients. `/health` gains only the optional fixed literal `recovery` (`restarting` | `exhausted`), present only with `engine=failed`.
- **TDD:** each task writes its test first and runs it to see it fail for the stated reason, then fixes, then runs the full `ctest --preset linux-gcc`. Rows whose spec "Fix" column is `—` or "existing test fixed" (V8, C7, N2, N3, N5, R1, R3, A1, A2, X1, X2) have no fix of their own: their test is the deliverable and must pass on its first run; if it fails, stop and report the defect it found.
- **Commits:** one local commit per task, never pushed:
  `git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "<subject>" -m "<body naming every row ID and CTest test name>" -m "<trailers>"`. Use the attribution trailers the harness reports at execution time; at the time of writing they were
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and `Claude-Session: <session link>`.
- **Handoff in-task:** every task adds or updates its row in the "SP2 deliverables" table of `docs/AGENT_HANDOFF.md` (created in Task 1) with the linux-gcc result line and the recorded wine line. Proof status never lives only in chat.
- **Engine facts (unchanged from SP0):** stdin lines are unbounded; `READY <ctx> stop`; `GEN <max_new> <id,...>`; `T <id>`; `DONE ...`; STOP yields `DONE ... cancel`; QUIT/EOF → exit 0; `ERR` paths → exit 1; CUDA fault → `std::_Exit(1)`; watchdog → `std::abort()` (SIGABRT; Windows 3 or `0xC0000409`); the engine's stdin reader is a thread on `read(0)`/`_read(0)` (`generate.cpp:8860-8900`); `--gpu LIST` makes the engine set `CUDA_DEVICE_ORDER`/`CUDA_VISIBLE_DEVICES` (or HIP) itself (`generate.cpp:1765-1786`).

## Review Focus

1. A `--state-dir` that is group/world-writable, owned by someone else, a symlink, or missing must stop the server at startup with a fixed error and spawn nothing (an attacker-writable engine cwd and temp dir would let another user plant `pack/full` or read stall dumps) — pinned in Task 18 (`state_dir_must_be_owner_only`).
2. A wrong `--engine` path (missing or not executable) must give three quick spawn failures, then `circuit_open`, while the server keeps answering `503 {"status":"unavailable","engine":"failed","recovery":"exhausted"}` — no tight respawn loop, no exit — pinned in Task 7 (`proc04_missing_engine_opens_circuit_without_spinning`).
3. SIGTERM while a chat request is in flight must answer that client with a generic `503` (or close it), exit 0 within 5 s, and leave no engine process — pinned in Task 15 (`proc05_sigterm_during_generation_exits_cleanly`).
4. A malformed `--gpu` value (`0,0`, `-1`, `1,,2`, ` 1`, `256`, `01`, empty, 17 entries) must be rejected at startup with a fixed message and never reach the engine — pinned in Task 19 (`gpu_list_rejects_malformed_values` and CTest `strata_secure_server_rejects_malformed_gpu`).
5. A second SIGTERM (or Ctrl-C pressed twice) during shutdown must not abort the orderly path: still exit 0, no engine left — pinned in Task 15 (`proc05_double_sigterm_is_idempotent`).
6. Three client-triggerable ERR lines open the circuit until a restart: any authenticated client whose request makes the engine answer `ERR` leaves that generation unusable (SP0 keeps the per-request answer; the supervisor replaces the generation and counts a failure), so three such requests in a row, inside the 300 s healthy window, put the frontend in `circuit_open` (`recovery=exhausted`) until an operator restarts it. This is the owner's restart policy applied to a client-reachable failure, accepted as such; reviewers should confirm it is acceptable rather than read it as a bug — pinned in Task 7 (`proc04_client_err_lines_open_circuit_until_restart`).

## Supervisor state machine (spec §3.8 — every implementer reads this)

| State | Entered when | Leaves to | `/health` status and body |
| --- | --- | --- | --- |
| `starting` | `start()` called; or a spawn succeeded | `ready` on READY; `stopping` on exit, protocol failure, fatal line, READY timeout | `503` `{"status":"unavailable","engine":"starting"}` |
| `ready` | READY parsed; or a lease released | `generating` on `acquire()`; `stopping` on monitor-observed exit or `report_fatal` | `200` `{"status":"ok","engine":"ready"}` |
| `generating` | a lease was taken | `ready` when the lease is released; `stopping` on `report_fatal` or exit | `200` `{"status":"ok","engine":"generating"}` |
| `stopping` | any unrequested end or kill | `backoff` once reaped + swept and failures < max; `circuit_open` if failures = max or the reap exceeded `reap_timeout` | `503` `{"status":"unavailable","engine":"failed","recovery":"restarting"}` |
| `backoff` | after `stopping` | `starting` when the delay expires (next spawn); a spawn failure counts and returns to `backoff`/`circuit_open` | `503` `{"status":"unavailable","engine":"failed","recovery":"restarting"}` |
| `circuit_open` | consecutive failures reached max, or an unreaped child | none (operator restarts the frontend) | `503` `{"status":"unavailable","engine":"failed","recovery":"exhausted"}` |
| `shutting_down` | SIGTERM/SIGINT/console event, or `main` exits | `stopped` | `503` `{"status":"unavailable","engine":"failed"}` |
| `stopped` | cleanup finished (also the state before `start()`) | — | not served |

Rules: `acquire()` succeeds only in `ready`; everywhere else a chat gets an immediate generic `503 {"error":"Service Unavailable"}`. A failure is any unrequested end, every §3.5 kill and any spawn failure (the first included). A generation that was `ready`/`generating` for at least `healthy_window` resets the consecutive-failure count to 0 before its own failure counts. Delays: 1 s, 2 s, 4 s … capped at 60 s; the 3rd consecutive failure opens the circuit. Each `/health` request uses one `snapshot()`. Log line per ended generation: `engine generation <n> ended: <kind>` with `<kind>` one of `clean_exit`, `error_exit`, `spawn_failed`, `abort`, `killed_external`, `crashed`, `unreaped`, `killed_by_supervisor(<reason>)` and `<reason>` one of `fatal_line`, `ready_timeout`, `drain_failed`, `write_timeout`, `protocol_failed`, `shutdown`.

## Interfaces (exact; later tasks rely on these names)

```cpp
// supervision_defaults.hpp (Task 6a) — the only home of SP1-bound constants until Task 29 folds them into SP1's config_defaults.hpp.
namespace secure_frontend::supervision_defaults {
inline constexpr std::uint32_t kMaxConsecutiveFailures = 3U;
inline constexpr std::chrono::milliseconds kBackoffInitial{1'000};
inline constexpr std::uint32_t kBackoffFactor = 2U;
inline constexpr std::chrono::milliseconds kBackoffMax{60'000};
inline constexpr std::chrono::milliseconds kHealthyWindow{300'000};
inline constexpr std::chrono::milliseconds kReadyTimeout{900'000};
inline constexpr std::chrono::milliseconds kReapTimeout{30'000};
inline constexpr std::chrono::milliseconds kMonitorFallbackPoll{100};
inline constexpr std::chrono::milliseconds kReadyPollSlice{100};
inline constexpr std::chrono::milliseconds kShutdownStdinWait{2'000};
inline constexpr std::chrono::milliseconds kShutdownTermWait{1'000};
inline constexpr std::chrono::milliseconds kShutdownBound{5'000};
inline constexpr std::chrono::milliseconds kStdinWriteDeadline{1'000};
inline constexpr std::size_t kStdinWriteChunk = 65'536U;
inline constexpr bool kNoNewPrivilegesDefault = true;
}

// exit_status.hpp (Task 5)
struct ExitStatus final { bool signaled{false}; std::uint32_t value{0}; };
enum class KillReason { none, fatal_line, ready_timeout, drain_failed, write_timeout, protocol_failed, shutdown };
enum class ExitKind { clean_exit, error_exit, spawn_failed, abort, killed_external, killed_by_supervisor, crashed };
enum class ExitPlatform { posix, windows };
struct ExitClassification final { ExitKind kind; KillReason reason; };
inline constexpr std::uint32_t kSupervisorKillExitCode = 0x20000001U;
ExitClassification classify_exit(const ExitStatus&, KillReason sent_by_supervisor, ExitPlatform = kNativeExitPlatform) noexcept;
std::string describe_exit(const ExitClassification&);

// restart_policy.hpp (struct: Task 6a; tracker: Task 7)
struct RestartPolicy final { std::uint32_t max_consecutive_failures; std::chrono::milliseconds backoff_initial;
  std::uint32_t backoff_factor; std::chrono::milliseconds backoff_max; std::chrono::milliseconds healthy_window;
  std::chrono::milliseconds ready_timeout; std::chrono::milliseconds reap_timeout; };   // defaults from supervision_defaults
struct RestartDecision final { bool circuit_open{false}; std::chrono::milliseconds delay{0}; std::uint32_t consecutive_failures{0}; };
class RestartTracker final { public: explicit RestartTracker(RestartPolicy) noexcept;
  RestartDecision record_failure(std::chrono::milliseconds healthy_for) noexcept; std::uint32_t consecutive_failures() const noexcept; };

// supervisor_wake.hpp (Task 6a)
inline constexpr std::intptr_t kNoMonitorHandle = -1;
class SupervisorWake final { public: enum class Event { woken, monitored, timeout };
  void notify() noexcept;   // async-signal-safe on POSIX
  Event wait(std::intptr_t monitored, std::chrono::milliseconds timeout) noexcept; };   // negative timeout = forever

// supervisor_clock.hpp (Task 6a) — the injectable clock / backoff timer
class SupervisorClock { public: using time_point = std::chrono::steady_clock::time_point; virtual ~SupervisorClock() = default;
  virtual time_point now() const = 0; virtual bool sleep_backoff(std::chrono::milliseconds, SupervisorWake&) = 0; };
class SteadySupervisorClock final : public SupervisorClock { ... };

// supervised_process.hpp (Task 6a)
class SupervisedProcess { public: virtual ~SupervisedProcess() = default;
  virtual EngineLineTransport& transport() noexcept = 0;
  virtual std::optional<ExitStatus> try_reap() = 0;
  virtual std::intptr_t monitor_handle() const noexcept = 0;
  virtual void close_stdin() noexcept = 0;
  virtual void signal_group_terminate() noexcept = 0;
  virtual void kill_group() noexcept = 0;
  virtual void release_resources(std::chrono::milliseconds bound) noexcept = 0;
  virtual long pid() const noexcept = 0; };
struct SpawnFailure final { std::string message; };
using SpawnResult = std::variant<std::unique_ptr<SupervisedProcess>, SpawnFailure>;
using ProcessFactory = std::function<SpawnResult(std::uint64_t generation)>;

// engine_supervisor.hpp (Task 6a; wired into the server in Task 6b)
enum class SupervisorState { starting, ready, generating, stopping, backoff, circuit_open, shutting_down, stopped };
struct HealthSnapshot final { SupervisorState state; bool healthy; std::string_view engine; std::string_view recovery; };
HealthSnapshot health_for(SupervisorState) noexcept;
struct EngineInstance final { std::uint64_t number{0}; std::unique_ptr<SupervisedProcess> process;
  std::unique_ptr<EngineProtocolClient> protocol; std::unique_ptr<ChatExecutionService> chat; };
class Lease final { public: Lease(Lease&&) noexcept; ~Lease();
  ChatExecutionService& chat() const noexcept; EngineProtocolClient& protocol() const noexcept;
  SupervisedProcess& process() const noexcept; std::uint64_t generation() const noexcept; };
struct SupervisorConfig final { RestartPolicy policy{}; ChatExecutionLimits chat_limits{};
  NativeChatProfile profile{NativeChatProfile::qwen}; EngineProtocolFlavor flavor{EngineProtocolFlavor::strata_0143};
  std::chrono::milliseconds ready_poll_slice; std::chrono::milliseconds monitor_poll;
  std::chrono::milliseconds shutdown_stdin_wait; std::chrono::milliseconds shutdown_term_wait;
  bool sweep_orphans{true}; bool no_new_privileges_hint{false}; };
using SupervisorLog = std::function<void(std::string_view line)>;
class EngineSupervisor final { public:
  EngineSupervisor(SupervisorConfig, ProcessFactory, std::shared_ptr<SupervisorClock> = std::make_shared<SteadySupervisorClock>(), SupervisorLog = {});
  void start(); std::optional<Lease> acquire(); void report_fatal(const Lease&, KillReason);
  HealthSnapshot snapshot() const; void shutdown(std::chrono::milliseconds bound);
  bool stop_requested() const noexcept; SupervisorWake& wake() noexcept;            // wake(): Task 15
  void set_on_stopped(std::function<void()>);                                        // Task 15
  bool wait_for_state(SupervisorState, std::chrono::milliseconds) const;              // test seam
  std::uint64_t generations_started() const noexcept; };                              // test seam
KillReason kill_reason_for(const EngineProtocolClient&) noexcept;
std::optional<ChatExecutionOutcome> execute_with_lease(EngineSupervisor&, const ChatCompletionRequest&, const TokenizerPack&);
struct EngineLaunchPlan final { EngineCommand command; /* Task 18: */ std::optional<EngineEnvironment> environment{};
  /* Task 16: */ bool use_delegated_cgroup{false}; };
ProcessFactory make_stdio_process_factory(EngineLaunchPlan);
// Task 26: logged once when a generation ends before READY under PR_SET_NO_NEW_PRIVS.
inline constexpr std::string_view kNoNewPrivilegesHint = "engine ended before READY while PR_SET_NO_NEW_PRIVS is set; ...";

// engine_environment.hpp (Task 18)
struct EngineEnvironment final { std::vector<std::filesystem::path> lib_dirs; std::filesystem::path temp_dir;
  std::filesystem::path cuda_cache_dir; std::optional<std::filesystem::path> hipblaslt_tuning{};
  std::optional<unsigned> watchdog_s{}; std::optional<unsigned> watchdog_io_s{}; };
struct WindowsSystemValues final { std::string system_root; std::string system_drive; unsigned processors{1}; };
using EnvironmentBlock = std::map<std::string, std::string, std::less<>>;
inline constexpr std::array<std::string_view, 16> kNeverForwardedEnvironment{ /* STRATA_API_KEY, LD_PRELOAD, ..., HIP_VISIBLE_DEVICES */ };
EnvironmentBlock render_posix_environment(const EngineEnvironment&);
EnvironmentBlock render_windows_environment(const EngineEnvironment&, const WindowsSystemValues&);
WindowsSystemValues query_windows_system_values();                     // GetSystemWindowsDirectoryW / GetSystemInfo
EnvironmentBlock render_engine_environment(const EngineEnvironment&);   // native
bool create_owner_only_directory(const std::filesystem::path&); bool is_owner_only_directory(const std::filesystem::path&);
bool clear_directory_contents(const std::filesystem::path&) noexcept; // startup: empties <state>/engine-tmp

// EngineCommand gains, in this declaration order (Tasks 16, 25, 26):
//   std::optional<std::filesystem::path> cgroup_procs{}; std::filesystem::path working_directory{}; bool no_new_privileges{false};
// EngineStdioProcess gains (Task 18): void set_generation_temp_dir(std::filesystem::path) — removed in release_resources() after reap.
// cgroup_scope.hpp (Task 16, include/secure_frontend/): class EngineCgroups { static std::optional<EngineCgroups> detect();
//   std::optional<std::filesystem::path> prepare_generation(std::uint64_t) const; const std::filesystem::path& root() const noexcept; };
//   bool kill_cgroup(const std::filesystem::path&) noexcept; bool remove_cgroup(const std::filesystem::path&, std::chrono::milliseconds) noexcept;
// shutdown_signal.hpp (Task 15): void install_shutdown_signals(SupervisorWake&) noexcept; bool shutdown_signalled() noexcept;
//   void notify_shutdown_complete() noexcept;
// HttpSocketServer (Task 15): void shutdown_listener() noexcept;
// privilege_check.hpp (Task 26): struct ProcessPrivileges { bool posix_root; bool windows_elevated; };
//   ProcessPrivileges current_process_privileges(); std::optional<std::string_view> privileged_start_error(const ProcessPrivileges&) noexcept;
// engine_arguments.hpp (Task 19): std::optional<std::vector<unsigned>> parse_gpu_list(std::string_view);
//   build_engine_arguments(profile, selected, models_root, user_arguments, const std::vector<unsigned>& gpus = {});
```

**Fake-engine mode flags** (`strata-supervision-fake-engine`, Tasks 1–2; `=value` forms only; anything else, e.g. `--serve`, `--native P`, `--pack D`, `--gpu L`, is accepted and ignored): `--ignore-term`, `--spew`, `--split-ready`, `--ready-delay-ms=<n>`, `--ready-context=<n>` (default 4096), `--exit-before-ready=<code>`, `--hang-before-ready`, `--crash-after-ready=<ABRT|KILL|SEGV|TERM|code>`, `--crash-delay-ms=<n>`, `--hang-in-gen` (on GEN stops reading stdin forever, so STOP is never seen), `--abort-in-gen`, `--fatal-line=<0..3>`, `--stall-stdin` (after READY never reads stdin), `--fail-first=<n> --counter-file=<p>`, `--record-file=<p>` (created at start; every stdin line appended), `--spawn-grandchild`, `--grandchild-setsid`, `--grandchild-ignore-term`, `--grandchild-pidfile=<p>`, `--echo-cmdline`, `--dump-file=<p>` (Task 2: the DUMP report written to `<p>` before READY); role `--fake-sleeper` (60 s sleeper). Protocol extension `DUMP` → `FDS`/`HANDLES`, `ARGV`, `ENV`, `SIG`, `CWD`, `IDS`, `END` (Task 2). `--ready-delay-ms`, `--ready-context`, `--crash-delay-ms`, `--stall-stdin`, `--dump-file` are implementer additions beyond the spec's F1 list, needed by V2, V4, K2, A2 and X2.

## File map

| File | Responsibility | Task |
| --- | --- | --- |
| `secure_frontend/tests/support/test_support.hpp`, `secure_frontend_add_cases`, `secure_frontend_add_test_executable`, `windows-runtime` label, wine preset filter | **SP1 Task 2 (shared pre-task; SP2 defines none of it)** | — |
| `secure_frontend/tests/support/test_paths.hpp` | fake/server paths, `skip()`, temp dirs (on SP1 scratch dirs), Windows user grant, pid helpers, `current_executable()` | 1, 3, 4 |
| `secure_frontend/tests/supervision_fake_engine_main.cpp` | `strata-supervision-fake-engine` | 1, 2 |
| `secure_frontend/tests/support/fake_engine_dump.hpp` | DUMP parser | 2 |
| `secure_frontend/tests/support/test_tokenizer.hpp` | byte tokenizer (in memory and on disk) | 3 |
| `secure_frontend/tests/support/server_harness.{hpp,cpp}` | start/stop `strata-secure-server`, HTTP client, descendants | 3 |
| `secure_frontend/tests/support/fake_supervised_process.hpp` | `FakeChild`, `FakeFactory`, `ManualClock` | 7 |
| `secure_frontend/include/secure_frontend/deadline.hpp` | `deadline_after`, `remaining_until` | 4 |
| `secure_frontend/include/secure_frontend/exit_status.hpp`, `src/exit_status.cpp` | exit capture types, classification | 5 |
| `secure_frontend/include/secure_frontend/supervision_defaults.hpp` | SP1-bound constants (folded into SP1's `config_defaults.hpp` and deleted in Task 29) | 6a, 29 |
| `secure_frontend/include/secure_frontend/{restart_policy,supervisor_wake,supervisor_clock,supervised_process,engine_supervisor}.hpp`, `src/{restart_policy,supervisor_wake,engine_supervisor}.cpp` | the supervisor | 6a, 6b, 7–11 |
| `secure_frontend/src/descendant_sweep.{hpp,cpp}` | Linux subreaper + orphan sweep | 13 |
| `secure_frontend/include/secure_frontend/shutdown_signal.hpp`, `src/shutdown_signal.cpp` | SIGTERM/SIGINT/console → wake | 15 |
| `secure_frontend/include/secure_frontend/cgroup_scope.hpp`, `src/cgroup_scope.cpp` | delegated cgroup v2 (`EngineCgroups`) | 16 |
| `secure_frontend/include/secure_frontend/engine_environment.hpp`, `src/engine_environment.cpp` | typed allowlist | 18 |
| `secure_frontend/include/secure_frontend/privilege_check.hpp`, `src/privilege_check.cpp` | refuse root/elevated | 26 |
| tests: `fake_engine_test.cpp`, `deadline_test.cpp`, `engine_child_test.cpp`, `engine_supervisor_test.cpp`, `server_test.cpp` | acceptance tests by ID | 1–27 |
| `secure_frontend/src/server_cli.cpp`, `src/frontend_config.cpp`, `include/secure_frontend/{frontend_config,config_defaults}.hpp` (SP1's files) | SP2 options as schema keys and `kFlags` rows; defaults folded in | 29 |

Test binaries and their cases (each CTest test is `<binary>.<case>`): `secure_frontend_fake_engine_tests` (F1, F2); `secure_frontend_deadline_tests` (K1); `secure_frontend_engine_child_tests` (one `EngineStdioProcess`/factory child: V1, C1, N4, N5, N6, R1, R2, R3, A1, G1, G2, G3); `secure_frontend_engine_supervisor_tests` (supervisor with fake or real children: V3–V7, V9, V10, C2, C5, C6-abort, N1, K2); `secure_frontend_server_tests` (whole `strata-secure-server` through the harness: F3, V2, V8, V11, C3, C4, C6, C7, A2, X1, X2, RF1, RF3, RF5, G4-default).

---

## Tasks, in execution order

Parallel-safe (no `secure_server_main.cpp` change; run while SP1 is in flight, after SP1 Task 2 is on `main`): Tasks 1, 2, 3, 4, 5, 6a, 7, 8, 9, 10, 11, 12, 13, 14, 17, 20, 21, 22, 23, 24. **After the SP1 merge** (each carries or contains the WAITS FOR SP1 MERGE banner, or needs a task that does): Tasks 6b, 15, 16, 18, 19, 25, 26, 27, 28, then 29 (post-SP1 integration). Task numbers are stable IDs used by every cross-reference; this file lists the tasks in the order to run them.

### Task 1: Standalone `strata-supervision-fake-engine` on SP1's per-case registration (F1)

**Files:**
- Create: `secure_frontend/tests/support/test_paths.hpp`
- Create: `secure_frontend/tests/supervision_fake_engine_main.cpp`
- Create: `secure_frontend/tests/fake_engine_test.cpp`
- Modify: `secure_frontend/tests/engine_stdio_process_test.cpp` (whole file: the in-binary fake and the `--fake-engine`/`--fake-sleeper` self-exec branches of `main` go; tests use the standalone fake; `main` keeps SP1's `secure_frontend_test::run_cases`)
- Modify: `secure_frontend/CMakeLists.txt` (tests block as rewritten by SP1 Task 2: after the `secure_frontend_add_cases(secure_frontend_engine_stdio_process_tests ${_stdio_cases} TIMEOUT 300)` line — fake engine target, `secure_frontend_test_paths()`, new test binary)
- Modify: `docs/AGENT_HANDOFF.md` (new "SP2 deliverables" section after the SP0 deliverables table, before "The native frontend enforces these in-process boundaries")
- Not modified (SP1 Task 2 owns them): `secure_frontend/CMakePresets.json`, `tests/support/test_support.hpp`, the registration helpers.

**Interfaces:**
- Consumes (SP1 Task 2): `secure_frontend_add_test_executable`, `secure_frontend_add_cases(<binary> <case>... [ARGS ...] [LABELS ...] [TIMEOUT s])` → tests `<binary>.<case>`, `${SECURE_FRONTEND_WINDOWS_RUNTIME}`, `secure_frontend_test::{run_cases, make_scratch_directory, make_private_directory, write_file, read_file, remove_scratch_directories}`.
- Produces: executable `strata-supervision-fake-engine` (modes in the plan header); `test_support::kSkipReturnCode` (77), `test_support::skip(reason)`, `test_support::fake_engine_path()`, `test_support::make_temp_dir(tag)`, `test_support::utf8_path(path)`, Windows `test_support::grant_current_user(path)`, `test_support::process_alive(pid)`, `test_support::wait_until_gone(pid, timeout)`, `test_support::read_pidfile(path, timeout)`, `test_support::kill_process(pid)`; CMake `secure_frontend_test_paths(target)` (defines `STRATA_FAKE_ENGINE_PATH`; links `advapi32` on Windows).

- [ ] **Step 0: Confirm the SP1 pre-task and record the baseline**

Run (from the worktree root): `git log --oneline -1 main && git merge-base --is-ancestor main HEAD && echo rebased` and `grep -c 'function(secure_frontend_add_cases binary)' secure_frontend/CMakeLists.txt && test -f secure_frontend/tests/support/test_support.hpp && echo sp1-task2-present`
Expected: `rebased`, `1`, `sp1-task2-present`. If any is missing, STOP: SP1 Task 2 has not landed on `main` (or this branch is not rebased onto it) and SP2 must not define its own registration.
Run: `cd secure_frontend && cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc 2>&1 | tail -1` and the same for `windows-llvm-mingw-cross`.
Expected: `100% tests passed, 0 tests failed out of N₀` and `... out of W₀`. Write N₀ and W₀ into the handoff row of Step 8; every later count in this plan is relative to them.

- [ ] **Step 1: Add the shared test paths and process helpers** — `secure_frontend/tests/support/test_paths.hpp`:

```cpp
#pragma once

// Paths and process helpers shared by the SP2 tests, on top of SP1's
// tests/support/test_support.hpp (case runner, owner-only scratch
// directories). The including target must call secure_frontend_test_paths()
// in CMake, which defines STRATA_FAKE_ENGINE_PATH (Task 3 adds
// STRATA_SECURE_SERVER_PATH).

#include "support/test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <aclapi.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace test_support {

inline constexpr int kSkipReturnCode = 77;

// A case that cannot run on this host (SP2 allows exactly two; see Global
// Constraints). Prints the reason, removes SP1's scratch directories and exits
// 77, which the test's SKIP_RETURN_CODE turns into "Skipped". Call it before
// the case creates any process.
[[noreturn]] inline void skip(const std::string_view reason) {
  std::cout << "SKIP: " << reason << '\n' << std::flush;
  secure_frontend_test::remove_scratch_directories();
  std::exit(kSkipReturnCode);
}

inline std::filesystem::path fake_engine_path() { return std::filesystem::path(STRATA_FAKE_ENGINE_PATH); }

inline std::string utf8_path(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

#if defined(_WIN32)
// Objects an elevated process creates are owned by BUILTIN\Administrators,
// which the LUA-restricted token the harness gives the server (Task 3) holds
// deny-only. Grant the current user -- the restricted token's user -- full
// control, inherited by everything created below `path` (SetNamedSecurityInfoW
// also propagates it to what already exists there).
inline bool grant_current_user(const std::filesystem::path& path) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  DWORD size = 0;
  (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> user(size);
  const bool have_user = size != 0 && GetTokenInformation(token, TokenUser, user.data(), size, &size);
  CloseHandle(token);
  if (!have_user) return false;
  EXPLICIT_ACCESSW access{};
  access.grfAccessPermissions = GENERIC_ALL;
  access.grfAccessMode = GRANT_ACCESS;
  access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.TrusteeType = TRUSTEE_IS_USER;
  access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
  std::wstring wide = path.wstring();
  PACL old_dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (GetNamedSecurityInfoW(wide.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl, nullptr,
                            &descriptor) != ERROR_SUCCESS) {
    return false;
  }
  PACL new_dacl = nullptr;
  const bool built = SetEntriesInAclW(1, &access, old_dacl, &new_dacl) == ERROR_SUCCESS;
  LocalFree(descriptor);
  if (!built) return false;
  const bool set = SetNamedSecurityInfoW(wide.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl,
                                         nullptr) == ERROR_SUCCESS;
  LocalFree(new_dacl);
  return set;
}
#endif

// A fresh owner-only directory <scratch>/<tag>: SP1's make_scratch_directory()
// creates a new 0700 directory below $SECURE_FRONTEND_TEST_TMP
// (<build>/test-tmp; Windows: the per-user %TEMP%) that run_cases removes
// after the case, so state from an earlier run never leaks into a test.
inline std::filesystem::path make_temp_dir(const std::string_view tag) {
  const auto path = secure_frontend_test::make_scratch_directory() / std::string(tag);
  secure_frontend_test::make_private_directory(path);
#if defined(_WIN32)
  if (!grant_current_user(path)) {
    std::cerr << "FATAL: could not grant the current user access to a test directory\n";
    std::exit(EXIT_FAILURE);
  }
#endif
  return path;
}

inline bool process_alive(const long pid) {
  if (pid <= 0) return false;
#if defined(_WIN32)
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) return false;
  const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  CloseHandle(process);
  return alive;
#else
  if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) return false;
  std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
  const std::string content((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
  const auto close = content.rfind(')');
  // A zombie has ended; only its parent has not collected it yet.
  return content.empty() || close == std::string::npos || close + 2 >= content.size() || content[close + 2] != 'Z';
#endif
}

inline bool wait_until_gone(const long pid, const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (process_alive(pid)) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

inline std::optional<long> read_pidfile(const std::filesystem::path& path, const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    std::ifstream input(path);
    long pid = 0;
    if (input >> pid && pid > 0) return pid;
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

inline void kill_process(const long pid) {
  if (pid <= 0) return;
#if defined(_WIN32)
  HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
  if (process != nullptr) {
    (void)TerminateProcess(process, 1);
    CloseHandle(process);
  }
#else
  (void)kill(static_cast<pid_t>(pid), SIGKILL);
#endif
}

}  // namespace test_support
```

- [ ] **Step 2: Write the failing self-test** — `secure_frontend/tests/fake_engine_test.cpp`:

```cpp
#include "secure_frontend/engine_process.hpp"
#include "secure_frontend/engine_stdio_process.hpp"

#include "support/test_paths.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using secure_frontend::EngineReadKind;

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

// Exit code of a fake that is not given a stdio protocol (EngineProcess keeps the parent's stdio).
std::optional<int> exit_code_of(const std::vector<std::string>& arguments) {
  secure_frontend::EngineProcess process;
  if (process.start({.executable = test_support::fake_engine_path(), .arguments = arguments, .environment = {}}).has_value()) {
    return std::nullopt;
  }
  return process.wait_for(10s);
}

struct Child final {
  secure_frontend::EngineStdioProcess transport;
  explicit Child(const std::vector<std::string>& arguments) {
    expect(!transport.start({.executable = test_support::fake_engine_path(), .arguments = arguments, .environment = {}}).has_value(),
           "fake engine must start");
  }
  ~Child() { (void)transport.terminate(std::chrono::milliseconds(250)); }
  std::string line(const std::chrono::milliseconds timeout = 5s) {
    const auto result = transport.read_line(timeout);
    return result.kind == EngineReadKind::line ? result.line : std::string("<kind ") + std::to_string(static_cast<int>(result.kind)) + ">";
  }
  EngineReadKind kind(const std::chrono::milliseconds timeout) { return transport.read_line(timeout).kind; }
  void send(const std::string& text) {
    std::string error;
    expect(transport.write_line(text, error), "write must succeed: " + text);
  }
};

std::string unhex(const std::string& hex) {
  std::string out;
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    out.push_back(static_cast<char>(std::stoi(hex.substr(index, 2), nullptr, 16)));
  }
  return out;
}

void fake_engine_modes_selftest() {
  // Exit paths (raw exit codes as EngineProcess normalizes them: POSIX 128 + signal).
  expect(exit_code_of({"--exit-before-ready=3"}) == 3, "--exit-before-ready=3 exits 3");
  expect(exit_code_of({"--crash-after-ready=9"}) == 9, "--crash-after-ready=<code> exits with that code");
  const auto start = std::chrono::steady_clock::now();
  expect(exit_code_of({"--crash-after-ready=7", "--crash-delay-ms=200"}) == 7, "--crash-delay-ms delays the crash");
  expect(std::chrono::steady_clock::now() - start >= 200ms, "the crash waited --crash-delay-ms");
  const auto aborted = exit_code_of({"--crash-after-ready=ABRT"});
#if defined(_WIN32)
  expect(aborted && (*aborted == 3 || static_cast<unsigned>(*aborted) == 0xC0000409U), "ABRT is exit 3 or 0xC0000409 on Windows");
  const auto segv = exit_code_of({"--crash-after-ready=SEGV"});
  expect(segv && static_cast<unsigned>(*segv) == 0xC0000005U, "SEGV is an access violation on Windows");
#else
  expect(aborted == 128 + 6, "ABRT dies by SIGABRT");
  expect(exit_code_of({"--crash-after-ready=KILL"}) == 128 + 9, "KILL dies by SIGKILL");
  expect(exit_code_of({"--crash-after-ready=SEGV"}) == 128 + 11, "SEGV dies by SIGSEGV");
  expect(exit_code_of({"--crash-after-ready=TERM"}) == 128 + 15, "TERM dies by SIGTERM");
#endif

  {  // READY variants
    Child child({"--split-ready", "--ready-delay-ms=300"});
    expect(child.kind(100ms) == EngineReadKind::timeout, "--ready-delay-ms holds READY back");
    expect(child.line() == "READY 4096 stop", "split READY arrives whole");
  }
  {
    Child child({"--ready-context=65536"});
    expect(child.line() == "READY 65536 stop", "--ready-context sets the advertised context");
  }
  {
    Child child({"--hang-before-ready"});
    expect(child.kind(300ms) == EngineReadKind::timeout, "--hang-before-ready never prints READY");
  }
  {
    Child child({"--echo-cmdline"});
    const std::string cmdline = child.line();
    expect(cmdline.rfind("CMDLINE ", 0) == 0 && unhex(cmdline.substr(8)).find("--echo-cmdline") != std::string::npos,
           "--echo-cmdline reports the command line before READY: " + cmdline);
    expect(child.line() == "READY 4096 stop", "READY follows CMDLINE");
  }

  {  // Generation behaviors
    Child child({"--hang-in-gen"});
    expect(child.line() == "READY 4096 stop", "READY");
    child.send("GEN 1 1");
    expect(child.kind(300ms) == EngineReadKind::timeout, "--hang-in-gen prints nothing for GEN");
    child.send("STOP");
    expect(child.kind(300ms) == EngineReadKind::timeout, "--hang-in-gen never answers STOP");
  }
  {
    Child child({"--abort-in-gen"});
    expect(child.line() == "READY 4096 stop", "READY");
    child.send("GEN 1 1");
    expect(child.line() == "T 7", "--abort-in-gen prints one token");
    expect(child.kind(5s) == EngineReadKind::closed, "then the engine is gone");
  }
  const std::vector<std::string> prefixes{"ERR verify: timed out at layer ", "ERR verify batch: timed out at layer ",
                                          "ERR verify: an earlier window never finished",
                                          "ERR verify batch: an earlier window never finished"};
  for (std::size_t index = 0; index < prefixes.size(); ++index) {
    Child child({"--fatal-line=" + std::to_string(index)});
    expect(child.line() == "READY 4096 stop", "READY");
    child.send("GEN 1 1");
    expect(child.line() == "T 7", "fatal mode prints a token first");
    expect(child.line().rfind(prefixes[index], 0) == 0, "fatal mode prints prefix " + std::to_string(index));
    expect(child.line() == "DONE stop", "then a trailing DONE the frontend must not accept");
    child.send("GEN 1 1");
    expect(child.line() == "T 7", "the engine lives on after the fatal line");
  }
  {
    Child child({"--stall-stdin"});
    expect(child.line() == "READY 4096 stop", "READY");
    child.send("GEN 1 1");
    expect(child.kind(300ms) == EngineReadKind::timeout, "--stall-stdin never reads the GEN");
  }

  {  // Restart helpers and recording
    const auto dir = test_support::make_temp_dir("fake-modes");
    const std::string counter = "--counter-file=" + test_support::utf8_path(dir / "count");
    {
      Child first({"--fail-first=1", counter});
      expect(first.kind(5s) == EngineReadKind::closed, "the first start fails before READY");
    }
    {
      Child second({"--fail-first=1", counter});
      expect(second.line() == "READY 4096 stop", "the second start is healthy");
    }
    const auto record = dir / "record.txt";
    {
      Child child({"--record-file=" + test_support::utf8_path(record)});
      expect(child.line() == "READY 4096 stop", "READY");
      child.send("GEN 1 1");
      expect(child.line() == "T 7" && child.line() == "DONE stop", "a normal generation");
      child.send("QUIT");
      expect(child.kind(5s) == EngineReadKind::closed, "QUIT ends the engine");
    }
    std::ifstream recorded(record);
    const std::string content((std::istreambuf_iterator<char>(recorded)), std::istreambuf_iterator<char>());
    expect(content == "GEN 1 1\nQUIT\n", "--record-file holds every stdin line, saw: " + content);

    const auto pidfile = dir / "grandchild.pid";
    {
      Child child({"--spawn-grandchild", "--grandchild-pidfile=" + test_support::utf8_path(pidfile)});
      expect(child.line() == "READY 4096 stop", "READY");
      const auto grandchild = test_support::read_pidfile(pidfile, 5s);
      expect(grandchild.has_value() && test_support::process_alive(*grandchild), "the grandchild is running");
      if (grandchild) test_support::kill_process(*grandchild);
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  return secure_frontend_test::run_cases(argc, argv, {{"fake_engine_modes_selftest", fake_engine_modes_selftest}}, failures);
}
```

- [ ] **Step 3: Register the binary on SP1's helpers, then run to verify it fails**

In `secure_frontend/CMakeLists.txt` (tests block as SP1 Task 2 wrote it), directly after the line
`    secure_frontend_add_cases(secure_frontend_engine_stdio_process_tests ${_stdio_cases} TIMEOUT 300)` insert:

```cmake
    # SP2 F1: test-only stand-in for `strata --serve`. Never installed. SP1's own
    # fake (tests/fake_engine_main.cpp, secure_frontend_fake_engine) is separate.
    add_executable(strata-supervision-fake-engine tests/supervision_fake_engine_main.cpp)
    target_include_directories(strata-supervision-fake-engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(strata-supervision-fake-engine PRIVATE cxx_std_20)
    secure_frontend_set_warnings(strata-supervision-fake-engine)
    if(WIN32)
        target_link_libraries(strata-supervision-fake-engine PRIVATE shell32 advapi32)
    endif()

    # Paths every SP2 test needs (fixture directories come from SP1's
    # make_scratch_directory below ${SECURE_FRONTEND_TEST_TMP}).
    function(secure_frontend_test_paths target)
        target_compile_definitions(${target} PRIVATE
            "STRATA_FAKE_ENGINE_PATH=\"$<TARGET_FILE:strata-supervision-fake-engine>\"")
        add_dependencies(${target} strata-supervision-fake-engine)
        if(WIN32)
            target_link_libraries(${target} PRIVATE advapi32)  # grant_current_user()
        endif()
    endfunction()

    # SP1 Task 2 already builds and registers secure_frontend_engine_stdio_process_tests.
    secure_frontend_test_paths(secure_frontend_engine_stdio_process_tests)

    secure_frontend_add_test_executable(secure_frontend_fake_engine_tests tests/fake_engine_test.cpp
        secure_frontend_engine_process secure_frontend_engine_stdio_process)
    secure_frontend_test_paths(secure_frontend_fake_engine_tests)
    # Crash exit codes are Windows runtime semantics: MSYS2 only.
    secure_frontend_add_cases(secure_frontend_fake_engine_tests fake_engine_modes_selftest
        TIMEOUT 120 LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
```

Run: `cmake --preset linux-gcc && cmake --build --preset linux-gcc 2>&1 | tail -5`
Expected: FAIL — `supervision_fake_engine_main.cpp` does not exist (`Cannot find source file: tests/supervision_fake_engine_main.cpp`).

- [ ] **Step 4: Write `secure_frontend/tests/supervision_fake_engine_main.cpp`**

```cpp
// strata-supervision-fake-engine: a test-only stand-in for `strata --serve` (SP2 F1).
// Never installed. (SP1's tests/fake_engine_main.cpp is a different, simpler fake.) It speaks the solo serve protocol the frontend uses
// (READY, GEN -> T/DONE, STOP, QUIT) and adds the failure modes the
// supervision tests need; see the mode list in the SP2 plan header.
// Unknown arguments (--serve, --native PATH, --pack DIR, --gpu LIST, ...) are
// accepted and ignored, as the real engine accepts them.
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <shellapi.h>
#else
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

// The engine's fatal verification lines (upstream/serve/server.py FATAL_PREFIXES).
constexpr std::string_view kFatalPrefixes[] = {
    "ERR verify: timed out at layer ",
    "ERR verify batch: timed out at layer ",
    "ERR verify: an earlier window never finished",
    "ERR verify batch: an earlier window never finished",
};

struct Options final {
  bool ignore_term{false};
  bool spew{false};
  bool split_ready{false};
  int ready_delay_ms{0};
  unsigned long ready_context{4096};
  std::optional<int> exit_before_ready;
  bool hang_before_ready{false};
  std::optional<std::string> crash_after_ready;
  int crash_delay_ms{0};
  bool hang_in_gen{false};
  bool abort_in_gen{false};
  std::optional<int> fatal_line;
  bool stall_stdin{false};
  int fail_first{0};
  std::string counter_file;
  std::string record_file;
  bool spawn_grandchild{false};
  bool grandchild_setsid{false};
  bool grandchild_ignore_term{false};
  std::string grandchild_pidfile;
  bool echo_cmdline{false};
};

int g_argc = 0;
char** g_argv = nullptr;

bool value_of(const std::string_view argument, const std::string_view name, std::string& value) {
  if (argument.size() <= name.size() || argument.substr(0, name.size()) != name || argument[name.size()] != '=') return false;
  value = std::string(argument.substr(name.size() + 1));
  return true;
}

Options parse_options(const int argc, char** argv) {
  Options options;
  std::string value;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument == "--ignore-term") options.ignore_term = true;
    else if (argument == "--spew") options.spew = true;
    else if (argument == "--split-ready") options.split_ready = true;
    else if (argument == "--hang-before-ready") options.hang_before_ready = true;
    else if (argument == "--hang-in-gen") options.hang_in_gen = true;
    else if (argument == "--abort-in-gen") options.abort_in_gen = true;
    else if (argument == "--stall-stdin") options.stall_stdin = true;
    else if (argument == "--spawn-grandchild") options.spawn_grandchild = true;
    else if (argument == "--grandchild-setsid") options.grandchild_setsid = true;
    else if (argument == "--grandchild-ignore-term") options.grandchild_ignore_term = true;
    else if (argument == "--echo-cmdline") options.echo_cmdline = true;
    else if (value_of(argument, "--ready-delay-ms", value)) options.ready_delay_ms = std::stoi(value);
    else if (value_of(argument, "--ready-context", value)) options.ready_context = std::stoul(value);
    else if (value_of(argument, "--exit-before-ready", value)) options.exit_before_ready = std::stoi(value);
    else if (value_of(argument, "--crash-after-ready", value)) options.crash_after_ready = value;
    else if (value_of(argument, "--crash-delay-ms", value)) options.crash_delay_ms = std::stoi(value);
    else if (value_of(argument, "--fatal-line", value)) options.fatal_line = std::clamp(std::stoi(value), 0, 3);
    else if (value_of(argument, "--fail-first", value)) options.fail_first = std::stoi(value);
    else if (value_of(argument, "--counter-file", value)) options.counter_file = value;
    else if (value_of(argument, "--record-file", value)) options.record_file = value;
    else if (value_of(argument, "--grandchild-pidfile", value)) options.grandchild_pidfile = value;
  }
  return options;
}

void sleep_ms(const int milliseconds) { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }

[[noreturn]] void sleep_forever() {
  for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

std::string hex(const std::string_view bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2U);
  for (const unsigned char byte : bytes) {
    out.push_back(kDigits[byte >> 4U]);
    out.push_back(kDigits[byte & 0x0fU]);
  }
  return out;
}

#if defined(_WIN32)
std::string utf8(const std::wstring_view text) {
  if (text.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(std::max(size, 0)), '\0');
  if (size > 0) (void)WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
  return out;
}
#endif

// Written to <path>.tmp and renamed, so a reader never sees a partial number.
void write_pidfile(const std::string& path, const long pid) {
  const std::string temporary = path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    out << pid << '\n';
  }
  std::error_code ignored;
  std::filesystem::rename(temporary, path, ignored);
}

[[noreturn]] void crash(const std::string& how) {
  std::cout.flush();
#if !defined(_WIN32)
  const rlimit no_core{0, 0};
  (void)setrlimit(RLIMIT_CORE, &no_core);  // a test crash must not leave core files in the build tree
#endif
  if (how == "ABRT") std::abort();
#if defined(_WIN32)
  if (how == "SEGV") RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
  if (how == "KILL") (void)TerminateProcess(GetCurrentProcess(), 137);
  if (how == "TERM") (void)TerminateProcess(GetCurrentProcess(), 143);
#else
  if (how == "SEGV" || how == "KILL" || how == "TERM") {
    const int signal_number = how == "SEGV" ? SIGSEGV : how == "KILL" ? SIGKILL : SIGTERM;
    (void)std::signal(signal_number, SIG_DFL);
    (void)std::raise(signal_number);
  }
#endif
  std::_Exit(std::stoi(how));
}

#if defined(_WIN32)
long spawn_grandchild(const Options&) {
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return 0;
  const std::wstring self(buffer.data(), length);
  std::wstring line = L"\"" + self + L"\" --fake-sleeper";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  if (!CreateProcessW(self.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) return 0;
  CloseHandle(info.hThread);
  CloseHandle(info.hProcess);
  return static_cast<long>(info.dwProcessId);
}
#else
long spawn_grandchild(const Options& options) {
  const pid_t child = fork();
  if (child != 0) return child < 0 ? 0 : static_cast<long>(child);
  if (options.grandchild_setsid) (void)setsid();
  if (options.grandchild_ignore_term) (void)std::signal(SIGTERM, SIG_IGN);
  const int null_fd = open("/dev/null", O_RDWR);  // never hold the protocol pipes open
  if (null_fd >= 0) {
    (void)dup2(null_fd, STDIN_FILENO);
    (void)dup2(null_fd, STDOUT_FILENO);
    (void)close(null_fd);
  }
  // POSIX: the grandchild writes its own pidfile after setsid/SIG_IGN, so a
  // test that reads the pid never races the grandchild's setup.
  if (!options.grandchild_pidfile.empty()) write_pidfile(options.grandchild_pidfile, static_cast<long>(getpid()));
  sleep_ms(60000);
  std::_Exit(0);
}
#endif

#if !defined(_WIN32)
// This process's open descriptors, dropping the directory iterator's own one
// (its link target is /proc/<pid>/fd).
std::string open_descriptors() {
  const std::string iterator_target = "/proc/" + std::to_string(getpid()) + "/fd";
  std::string fds;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
    std::error_code ignored;
    const auto target = std::filesystem::read_symlink(entry.path(), ignored).string();
    if (target == iterator_target) continue;
    if (!fds.empty()) fds += ',';
    fds += entry.path().filename().string();
  }
  return fds;
}
#endif

// Protocol extension used by the PROC tests (Task 2 replaces this with the full report).
void dump() {
#if !defined(_WIN32)
  std::cout << "FDS " << open_descriptors() << '\n';
  std::cout << "ARGV";
  for (int index = 0; index < g_argc; ++index) std::cout << '\x1f' << g_argv[index];
  std::cout << '\n' << std::flush;
#endif
}

void echo_cmdline() {
#if defined(_WIN32)
  std::cout << "CMDLINE " << hex(utf8(GetCommandLineW())) << '\n' << std::flush;
#else
  std::string joined;
  for (int index = 0; index < g_argc; ++index) {
    if (index != 0) joined += ' ';
    joined += g_argv[index];
  }
  std::cout << "CMDLINE " << hex(joined) << '\n' << std::flush;
#endif
}

int serve(const Options& options) {
#if !defined(_WIN32)
  if (options.ignore_term) (void)std::signal(SIGTERM, SIG_IGN);
#endif
  if (!options.record_file.empty()) std::ofstream(options.record_file, std::ios::trunc).flush();
  if (options.fail_first > 0 && !options.counter_file.empty()) {
    int started = 0;
    {
      std::ifstream in(options.counter_file);
      in >> started;
    }
    {
      std::ofstream out(options.counter_file, std::ios::trunc);
      out << (started + 1) << '\n';
    }
    if (started < options.fail_first) return 1;
  }
  // A real engine may emit human diagnostics on stderr before READY; they must
  // never reach the strict stdout protocol stream.
  std::cerr << "diagnostic: private engine startup\n" << std::flush;
  if (options.exit_before_ready) return *options.exit_before_ready;
  if (options.hang_before_ready) sleep_forever();
  if (options.echo_cmdline) echo_cmdline();
  if (options.ready_delay_ms > 0) sleep_ms(options.ready_delay_ms);
  const std::string ready = "READY " + std::to_string(options.ready_context) + " stop\n";
  if (options.split_ready) {
    std::cout << ready.substr(0, 3) << std::flush;
    sleep_ms(100);
    std::cout << ready.substr(3) << std::flush;
  } else {
    std::cout << ready << std::flush;
  }
  if (options.spawn_grandchild) {
    const long grandchild = spawn_grandchild(options);
#if defined(_WIN32)
    if (!options.grandchild_pidfile.empty()) write_pidfile(options.grandchild_pidfile, grandchild);
#endif
    if (options.grandchild_pidfile.empty()) std::cout << "GRANDCHILD " << grandchild << '\n' << std::flush;
  }
  if (options.crash_after_ready) {
    if (options.crash_delay_ms > 0) sleep_ms(options.crash_delay_ms);
    crash(*options.crash_after_ready);
  }
  if (options.stall_stdin) sleep_forever();
  std::string line;
  if (options.spew) {
    // One record larger than kMaxReadLineBytes with no newline, then stay alive.
    std::cout << std::string(9000, 'x') << std::flush;
    while (std::getline(std::cin, line)) {
    }
    return 0;
  }
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!options.record_file.empty()) {
      std::ofstream out(options.record_file, std::ios::app);
      out << line << '\n';
    }
    if (line.rfind("GEN ", 0) == 0) {
      if (options.hang_in_gen) sleep_forever();  // stops reading stdin: STOP is never seen
      if (options.abort_in_gen) {
        std::cout << "T 7\n" << std::flush;
        crash("ABRT");
      }
      if (options.fatal_line) {
        const auto index = static_cast<std::size_t>(*options.fatal_line);
        std::cout << "T 7\n" << kFatalPrefixes[index] << (index < 2U ? "12" : "") << "\nDONE stop\n" << std::flush;
        continue;
      }
      std::cout << "T 7\nDONE stop\n" << std::flush;
    } else if (line == "QUIT") {
      return 0;
    } else if (line == "DUMP") {
      dump();
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
  // The crash modes (ABRT, SEGV, --abort-in-gen) must end the process at once.
  // Without this a crash can wait on a Windows Error Reporting / critical-error
  // dialog on the MSYS2 runner and the test hangs until its CTest timeout. The
  // error mode is inherited by --fake-sleeper grandchildren.
  (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
  g_argc = argc;
  g_argv = argv;
  for (int index = 1; index < argc; ++index) {
    if (std::string_view(argv[index]) == "--fake-sleeper") {
      sleep_ms(60000);
      return 0;
    }
  }
  return serve(parse_options(argc, argv));
}
```

- [ ] **Step 5: Point `engine_stdio_process_test.cpp` at the standalone fake**

Replace the whole file with (same cases as before; the fake now lives in `strata-supervision-fake-engine`, which also accepts the legacy `--fake-engine` argument and ignores it):

```cpp
#include "secure_frontend/engine_protocol_client.hpp"
#include "secure_frontend/engine_stdio_process.hpp"

#include "support/test_paths.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

void expect(const bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void test_round_trip(const std::filesystem::path& fake) {
  secure_frontend::EngineStdioProcess transport;
  secure_frontend::EngineCommand command{.executable = fake, .arguments = {"--fake-engine"}, .environment = {}};
  const auto start_error = transport.start(command);
  expect(!start_error.has_value(), "fake engine must start");
  secure_frontend::EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  expect(!client.await_ready(std::chrono::seconds(2), ready).has_value(), "READY must parse");
  expect(ready.max_context_tokens == 4096 && ready.supports_stop, "READY capabilities must survive transport");
  // E4: the GEN line for a full context is far beyond 8 KiB; the write cap must allow it.
  std::string big(100000U, '1');
  std::string werr;
  const bool big_written = transport.write_line("GEN 1 " + big, werr);
  expect(big_written, "a 100 KB GEN line must be writable: " + werr);
  const auto reply_token = transport.read_line(std::chrono::seconds(2));
  const auto reply_done = transport.read_line(std::chrono::seconds(2));
  expect(reply_token.kind == secure_frontend::EngineReadKind::line && reply_token.line == "T 7" &&
             reply_done.kind == secure_frontend::EngineReadKind::line && reply_done.line == "DONE stop",
         "the fake engine must answer the 100 KB GEN line");
  expect(!client.begin_generation(2, {1, 2, 3}).has_value(), "GEN must write through transport");
  secure_frontend::EngineGeneration generation{};
  expect(!client.read_generation(std::chrono::seconds(2), generation).has_value(), "generation must read through transport");
  expect(generation.token_ids == std::vector<std::uint32_t>{7}, "token must survive transport");
  expect(!client.quit().has_value(), "QUIT must write through transport");
  expect(transport.terminate(std::chrono::milliseconds(250)), "child cleanup must succeed");
}

void test_invalid_record() {
  secure_frontend::EngineStdioProcess transport;
  std::string error;
  expect(!transport.write_line("bad\nline", error), "multiline writes must be rejected");
}

void read_line_rejects_oversize_on_all_platforms(const std::filesystem::path& fake) {
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine", "--spew"}, .environment = {}}).has_value(),
         "fake engine must start");
  (void)transport.read_line(std::chrono::seconds(2));  // READY
  const auto result = transport.read_line(std::chrono::seconds(2));
  expect(result.kind == secure_frontend::EngineReadKind::failure, "an oversized engine line must be a failure on every platform");
  (void)transport.terminate(std::chrono::milliseconds(250));
}

void read_line_waits_for_a_record_split_across_writes(const std::filesystem::path& fake) {
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine", "--split-ready"}, .environment = {}}).has_value(),
         "fake engine must start");
  const auto ready = transport.read_line(std::chrono::seconds(2));
  expect(ready.kind == secure_frontend::EngineReadKind::line && ready.line == "READY 4096 stop",
         "a record split across writes must be read whole within the budget, not time out early");
  (void)transport.terminate(std::chrono::milliseconds(250));
}

#if defined(_WIN32)
void job_object_terminates_grandchild(const std::filesystem::path& fake) {
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine", "--spawn-grandchild"}, .environment = {}})
              .has_value(),
         "fake engine must start");
  const auto ready = transport.read_line(std::chrono::seconds(5));
  expect(ready.kind == secure_frontend::EngineReadKind::line && ready.line == "READY 4096 stop", "READY must arrive");
  const auto report = transport.read_line(std::chrono::seconds(5));
  expect(report.kind == secure_frontend::EngineReadKind::line && report.line.rfind("GRANDCHILD ", 0) == 0,
         "the fake must report its grandchild");
  const unsigned long pid = std::stoul(report.line.substr(11));
  expect(pid != 0, "the grandchild must have started");
  // The handle pins the process object, so no PID reuse can confuse the check.
  HANDLE grandchild = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  expect(grandchild != nullptr, "the live grandchild must be openable before terminate");
  (void)transport.terminate(std::chrono::milliseconds(250));
  const bool gone = WaitForSingleObject(grandchild, 2000) == WAIT_OBJECT_0;
  CloseHandle(grandchild);
  expect(gone, "the grandchild must die with the Job (P4)");
}
#endif

#if !defined(_WIN32)
void child_inherits_only_stdio_descriptors(const std::filesystem::path& fake) {
  // Two extra descriptors without CLOEXEC in the parent; a correct launcher must not leak them.
  const int leak_a = open("/dev/null", O_RDONLY);
  const int leak_b = open("/dev/null", O_RDONLY);
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine"}, .environment = {}}).has_value(),
         "fake engine must start");
  std::string err;
  (void)transport.read_line(std::chrono::seconds(2));  // READY
  expect(transport.write_line("DUMP", err), "DUMP must write");
  const auto fds = transport.read_line(std::chrono::seconds(2));
  (void)transport.read_line(std::chrono::seconds(2));  // ARGV
  close(leak_a);
  close(leak_b);
  expect(fds.kind == secure_frontend::EngineReadKind::line && fds.line == "FDS 0,1,2", "child must see only stdio, saw: " + fds.line);
  (void)transport.terminate(std::chrono::milliseconds(250));
}

void child_receives_exact_argv_and_env(const std::filesystem::path& fake) {
  secure_frontend::EngineStdioProcess transport;
  const std::string weird = "literal; $(echo x) \"quoted\" \xE2\x9C\x93 \n-less";
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine", weird}, .environment = {{"STRATA_T", "1"}}})
              .has_value(),
         "fake engine must start");
  std::string err;
  (void)transport.read_line(std::chrono::seconds(2));  // READY
  expect(transport.write_line("DUMP", err), "DUMP must write");
  (void)transport.read_line(std::chrono::seconds(2));  // FDS
  const std::string expected = std::string("ARGV\x1f") + fake.string() + "\x1f--serve\x1f--fake-engine\x1f" + weird;
  // The argument's embedded newline splits the ARGV record into protocol lines; rejoin them.
  auto argv = transport.read_line(std::chrono::seconds(2));
  std::string got = argv.line;
  while (argv.kind == secure_frontend::EngineReadKind::line && got.size() < expected.size()) {
    argv = transport.read_line(std::chrono::seconds(2));
    if (argv.kind == secure_frontend::EngineReadKind::line) got += '\n' + argv.line;
  }
  expect(got == expected, "argv must arrive literally, no shell: " + got);
  (void)transport.terminate(std::chrono::milliseconds(250));
}

void terminate_reaps_killed_child(const std::filesystem::path& fake) {
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine", "--ignore-term"}, .environment = {}}).has_value(),
         "fake engine must start");
  (void)transport.read_line(std::chrono::seconds(2));  // READY: SIGTERM is now ignored
  const pid_t pid = transport.pid_for_tests();
  expect(pid > 0, "the fake engine must have a pid");
  expect(transport.terminate(std::chrono::milliseconds(200)), "terminate must succeed even when SIGTERM is ignored");
  expect(transport.pid_for_tests() == -1, "terminate must clear the child pid once it is reaped");
  errno = 0;
  const pid_t waited = waitpid(pid, nullptr, WNOHANG);
  expect(waited == -1 && errno == ECHILD, "the killed child must already be reaped by terminate");
  std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
  const std::string content((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
  expect(content.empty() || content.find(") Z ") == std::string::npos, "no zombie may remain after terminate, saw: " + content.substr(0, 64));
}
#endif

}  // namespace

// SP1 Task 2's case list, unchanged; only the engine is now the standalone fake.
int main(int argc, char* argv[]) {
  static const int no_counted_failures = 0;  // expect() throws; run_cases reports the exception
  static const auto fake = test_support::fake_engine_path();
  return secure_frontend_test::run_cases(argc, argv, {
      {"test_round_trip", [] { test_round_trip(fake); }},
      {"test_invalid_record", [] { test_invalid_record(); }},
      {"read_line_rejects_oversize_on_all_platforms", [] { read_line_rejects_oversize_on_all_platforms(fake); }},
      {"read_line_waits_for_a_record_split_across_writes", [] { read_line_waits_for_a_record_split_across_writes(fake); }},
#if defined(_WIN32)
      {"job_object_terminates_grandchild", [] { job_object_terminates_grandchild(fake); }},
#else
      {"child_inherits_only_stdio_descriptors", [] { child_inherits_only_stdio_descriptors(fake); }},
      {"child_receives_exact_argv_and_env", [] { child_receives_exact_argv_and_env(fake); }},
      {"terminate_reaps_killed_child", [] { terminate_reaps_killed_child(fake); }},
#endif
  }, no_counted_failures);
}
```

The case names and SP1's `secure_frontend_add_cases(secure_frontend_engine_stdio_process_tests ${_stdio_cases} TIMEOUT 300)` registration stay as they are.

- [ ] **Step 6: Run to verify everything passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc 2>&1 | tail -3`
Expected: `100% tests passed, 0 tests failed out of N₀ + 1` (the new entry is `secure_frontend_fake_engine_tests.fake_engine_modes_selftest`; `ctest --preset linux-gcc -N | grep fake_engine_modes_selftest` lists it). The `secure_frontend_engine_stdio_process_tests.*` cases still pass against the standalone fake.

- [ ] **Step 7: Cross-compile and record the wine smoke**

Run: `cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross 2>&1 | tail -2 && ctest --preset windows-llvm-mingw-cross 2>&1 | tail -3`
Expected: the build succeeds with `-Werror` and produces `strata-supervision-fake-engine.exe`; ctest prints `100% tests passed, 0 tests failed out of W₀` (`fake_engine_modes_selftest` is labeled `windows-runtime` and excluded). Record the exact line.

- [ ] **Step 8: Create the handoff section**

In `docs/AGENT_HANDOFF.md`, insert directly before the paragraph that starts `The native frontend enforces these in-process boundaries`:

```markdown
### SP2 deliverables (branch `sp2-portable-supervision`, in progress)

Item IDs are from §4 of the SP2 spec. W rows are verified only by the
`windows-msys2-clang` CI job (pending the owner's push); the wine column is a smoke result.

| Item | What changed | Regression test | Local result (linux-gcc; wine) |
| --- | --- | --- | --- |
| F1 | Standalone test-only `strata-supervision-fake-engine` with the SP2 failure modes (crash modes never wait on WER); SP2 tests registered by acceptance ID through SP1's `secure_frontend_add_cases` (`<binary>.<case>`); baseline after SP1 Task 2: N₀ = <recorded>, W₀ = <recorded>. | `secure_frontend_fake_engine_tests.fake_engine_modes_selftest` | <recorded linux line>; wine <recorded> (label excluded) |
```

- [ ] **Step 9: Commit**

```bash
git add secure_frontend/tests/support/test_paths.hpp secure_frontend/tests/supervision_fake_engine_main.cpp \
  secure_frontend/tests/fake_engine_test.cpp secure_frontend/tests/engine_stdio_process_test.cpp \
  secure_frontend/CMakeLists.txt docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "test: standalone strata-supervision-fake-engine with SP2 failure modes on SP1's per-case registration (F1)" \
  -m "F1: secure_frontend_fake_engine_tests.fake_engine_modes_selftest. engine_stdio_process_tests now launch the standalone fake. Registration, the windows-runtime label and the wine filter are SP1 Task 2's." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 2: DUMP report (ENV, SIG, CWD, IDS, HANDLES, portable ARGV) and its parser (F2)

**Files:**
- Modify: `secure_frontend/tests/supervision_fake_engine_main.cpp` (replace `dump()`; add `dump_lines()`, `--dump-file`)
- Create: `secure_frontend/tests/support/fake_engine_dump.hpp`
- Modify: `secure_frontend/tests/fake_engine_test.cpp` (add case `fake_engine_dump_is_parseable`)
- Modify: `secure_frontend/tests/engine_stdio_process_test.cpp` (`child_inherits_only_stdio_descriptors`, `child_receives_exact_argv_and_env` use the parser)
- Modify: `secure_frontend/CMakeLists.txt` (register the case)
- Modify: `docs/AGENT_HANDOFF.md` (F2 row)

**Interfaces:**
- Consumes: `strata-supervision-fake-engine` (Task 1).
- Produces: DUMP report lines, in order: POSIX `FDS <n,n,...>` / Windows `HANDLES <own pid>`; `ARGV <hex>,<hex>,...` (Windows: `CommandLineToArgvW(GetCommandLineW())` as UTF-8); `ENV <hex>,...` (sorted `NAME=value`); `SIG blk=<16 hex> ign=<16 hex>` (Windows `SIG none`); `CWD <hex>`; `IDS uid=<n> gid=<n> euid=<n> egid=<n> nnp=<0|1> capeff=<16 hex>` / Windows `IDS sid=<S-1-...> elevated=<0|1> integrity=<rid>`; `END`. `--dump-file=<p>` writes the same lines to `<p>` (via `<p>.tmp` + rename) before READY.
  `test_support::FakeDump{fds, windows_pid, argv, env, sig_blocked, sig_ignored, cwd, ids}`, `parse_dump(lines)`, `request_dump(EngineLineTransport&, timeout)`, `read_dump_file(path, timeout)`.

- [ ] **Step 1: Write the failing test**

Add to `secure_frontend/tests/fake_engine_test.cpp` (include `"support/fake_engine_dump.hpp"` at the top) and register it in `main`:

```cpp
void fake_engine_dump_is_parseable() {
  const auto fake = test_support::fake_engine_path();
  const std::string odd = "a b\nc \xE2\x9C\x93";
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = fake, .arguments = {odd}, .environment = {{"STRATA_T", "1"}, {"ZZ", "z=z"}}}).has_value(),
         "fake engine must start");
  expect(transport.read_line(5s).line == "READY 4096 stop", "READY");
  const auto dump = test_support::request_dump(transport, 5s);
  expect(dump.has_value(), "DUMP must parse");
  if (dump) {
    expect(dump->argv == std::vector<std::string>{test_support::utf8_path(fake), "--serve", odd}, "argv round-trips through ARGV");
    expect(dump->env == std::vector<std::string>{"STRATA_T=1", "ZZ=z=z"}, "ENV is the sorted child environment");
    expect(!dump->cwd.empty(), "CWD is reported");
#if defined(_WIN32)
    expect(dump->windows_pid != 0, "HANDLES carries the child's own pid");
    expect(dump->ids.count("sid") == 1 && dump->ids.count("elevated") == 1 && dump->ids.count("integrity") == 1, "Windows IDS fields");
#else
    expect(dump->fds == std::vector<int>{0, 1, 2}, "FDS lists exactly stdio");
    expect(dump->sig_blocked.size() == 16U && dump->sig_ignored.size() == 16U, "SIG carries SigBlk/SigIgn masks");
    for (const char* key : {"uid", "gid", "euid", "egid", "nnp", "capeff"}) expect(dump->ids.count(key) == 1, std::string("IDS has ") + key);
#endif
  }
  (void)transport.terminate(std::chrono::milliseconds(250));

  const auto dir = test_support::make_temp_dir("dump");
  const auto file = dir / "dump.txt";
  secure_frontend::EngineStdioProcess second;
  expect(!second.start({.executable = fake, .arguments = {"--dump-file=" + test_support::utf8_path(file)}, .environment = {}}).has_value(),
         "fake engine must start");
  expect(second.read_line(5s).line == "READY 4096 stop", "READY after --dump-file");
  const auto from_file = test_support::read_dump_file(file, 5s);
  expect(from_file.has_value() && from_file->argv.size() == 3U, "--dump-file writes the same parseable report");
  (void)second.terminate(std::chrono::milliseconds(250));
}
```

`main` becomes:

```cpp
int main(int argc, char* argv[]) {
  return secure_frontend_test::run_cases(argc, argv,
                                         {{"fake_engine_modes_selftest", fake_engine_modes_selftest},
                                          {"fake_engine_dump_is_parseable", fake_engine_dump_is_parseable}},
                                         failures);
}
```

In `secure_frontend/CMakeLists.txt` change the registration of `secure_frontend_fake_engine_tests` to:

```cmake
    secure_frontend_add_cases(secure_frontend_fake_engine_tests fake_engine_modes_selftest fake_engine_dump_is_parseable
        TIMEOUT 120 LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset linux-gcc 2>&1 | tail -3`
Expected: FAIL — `support/fake_engine_dump.hpp: No such file or directory`.

- [ ] **Step 3: Write the parser** — `secure_frontend/tests/support/fake_engine_dump.hpp`:

```cpp
#pragma once

#include "secure_frontend/engine_protocol_client.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace test_support {

// One DUMP report from strata-supervision-fake-engine (format: SP2 plan, Task 2).
struct FakeDump final {
  std::vector<int> fds;                 // POSIX: open descriptors
  unsigned long windows_pid{0};         // Windows: HANDLES <pid>
  std::vector<std::string> argv;
  std::vector<std::string> env;         // sorted NAME=value
  std::string sig_blocked;              // POSIX SigBlk, 16 hex digits
  std::string sig_ignored;              // POSIX SigIgn, 16 hex digits
  std::string cwd;
  std::map<std::string, std::string, std::less<>> ids;
};

inline std::optional<std::string> from_hex(const std::string_view hex) {
  if (hex.size() % 2U != 0U) return std::nullopt;
  const auto nibble = [](const char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  std::string out;
  for (std::size_t index = 0; index < hex.size(); index += 2U) {
    const int high = nibble(hex[index]);
    const int low = nibble(hex[index + 1U]);
    if (high < 0 || low < 0) return std::nullopt;
    out.push_back(static_cast<char>((high << 4) | low));
  }
  return out;
}

inline std::optional<std::vector<std::string>> hex_list(const std::string_view payload) {
  std::vector<std::string> items;
  if (payload.empty()) return items;
  std::size_t begin = 0;
  for (;;) {
    const auto comma = payload.find(',', begin);
    const auto item = from_hex(payload.substr(begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin));
    if (!item) return std::nullopt;
    items.push_back(*item);
    if (comma == std::string_view::npos) return items;
    begin = comma + 1U;
  }
}

inline std::map<std::string, std::string, std::less<>> key_values(const std::string_view payload) {
  std::map<std::string, std::string, std::less<>> values;
  std::size_t begin = 0;
  while (begin < payload.size()) {
    auto end = payload.find(' ', begin);
    if (end == std::string_view::npos) end = payload.size();
    const auto pair = payload.substr(begin, end - begin);
    const auto equals = pair.find('=');
    if (equals != std::string_view::npos) values.emplace(std::string(pair.substr(0, equals)), std::string(pair.substr(equals + 1U)));
    begin = end + 1U;
  }
  return values;
}

// Parses the report lines up to (not including) END.
inline std::optional<FakeDump> parse_dump(const std::vector<std::string>& lines) {
  FakeDump dump;
  for (const auto& line : lines) {
    const auto space = line.find(' ');
    const std::string_view tag = std::string_view(line).substr(0, space);
    const std::string_view payload = space == std::string::npos ? std::string_view{} : std::string_view(line).substr(space + 1U);
    if (tag == "FDS") {
      std::size_t begin = 0;
      while (begin < payload.size()) {
        auto end = payload.find(',', begin);
        if (end == std::string_view::npos) end = payload.size();
        dump.fds.push_back(std::stoi(std::string(payload.substr(begin, end - begin))));
        begin = end + 1U;
      }
    } else if (tag == "HANDLES") {
      dump.windows_pid = std::stoul(std::string(payload));
    } else if (tag == "ARGV" || tag == "ENV") {
      const auto items = hex_list(payload);
      if (!items) return std::nullopt;
      (tag == "ARGV" ? dump.argv : dump.env) = *items;
    } else if (tag == "SIG") {
      const auto values = key_values(payload);
      if (const auto found = values.find("blk"); found != values.end()) dump.sig_blocked = found->second;
      if (const auto found = values.find("ign"); found != values.end()) dump.sig_ignored = found->second;
    } else if (tag == "CWD") {
      const auto cwd = from_hex(payload);
      if (!cwd) return std::nullopt;
      dump.cwd = *cwd;
    } else if (tag == "IDS") {
      dump.ids = key_values(payload);
    } else {
      return std::nullopt;
    }
  }
  return dump;
}

// Sends DUMP and reads the report (at most 16 lines, each within `timeout`).
inline std::optional<FakeDump> request_dump(secure_frontend::EngineLineTransport& transport, const std::chrono::milliseconds timeout) {
  std::string error;
  if (!transport.write_line("DUMP", error)) return std::nullopt;
  std::vector<std::string> lines;
  for (int count = 0; count < 16; ++count) {
    const auto result = transport.read_line(timeout);
    if (result.kind != secure_frontend::EngineReadKind::line) return std::nullopt;
    if (result.line == "END") return parse_dump(lines);
    lines.push_back(result.line);
  }
  return std::nullopt;
}

// Reads a report written with --dump-file=<path>, waiting up to `timeout` for it.
inline std::optional<FakeDump> read_dump_file(const std::filesystem::path& path, const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    std::ifstream input(path, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line == "END") return parse_dump(lines);
      lines.push_back(line);
    }
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

}  // namespace test_support
```

- [ ] **Step 4: Replace the fake's `dump()` with the full report and add `--dump-file`**

In `secure_frontend/tests/supervision_fake_engine_main.cpp`: add `std::string dump_file;` to `Options`, parse it with `else if (value_of(argument, "--dump-file", value)) options.dump_file = value;`, add `#include <sddl.h>` to the Windows includes and `extern char** environ;` after the POSIX includes (outside the namespace). Replace `void dump()` with:

```cpp
std::string join_hex(const std::vector<std::string>& items) {
  std::string out;
  for (const auto& item : items) {
    if (!out.empty()) out += ',';
    out += hex(item);
  }
  return out;
}

#if !defined(_WIN32)
std::string status_field(const std::string_view name) {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.size() > name.size() && line.compare(0, name.size(), name) == 0 && line[name.size()] == ':') {
      const auto first = line.find_first_not_of(" \t", name.size() + 1U);
      return first == std::string::npos ? std::string{} : line.substr(first);
    }
  }
  return {};
}
#else
std::string token_report() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return "sid=? elevated=? integrity=?";
  std::string sid_text = "?";
  DWORD size = 0;
  (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> user(size);
  if (size != 0 && GetTokenInformation(token, TokenUser, user.data(), size, &size)) {
    LPWSTR sid = nullptr;
    if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) {
      sid_text = utf8(sid);
      LocalFree(sid);
    }
  }
  TOKEN_ELEVATION elevation{};
  DWORD elevation_size = sizeof(elevation);
  const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, elevation_size, &elevation_size) && elevation.TokenIsElevated != 0;
  DWORD rid = 0;
  size = 0;
  (void)GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
  std::vector<unsigned char> label(size);
  if (size != 0 && GetTokenInformation(token, TokenIntegrityLevel, label.data(), size, &size)) {
    PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
    rid = *GetSidSubAuthority(sid, static_cast<DWORD>(*GetSidSubAuthorityCount(sid) - 1U));
  }
  CloseHandle(token);
  return "sid=" + sid_text + " elevated=" + (elevated ? "1" : "0") + " integrity=" + std::to_string(rid);
}
#endif

// The DUMP report (SP2 plan, Task 2). Every list is hex so one record stays one line.
std::vector<std::string> dump_lines() {
  std::vector<std::string> lines;
  std::vector<std::string> arguments;
  std::vector<std::string> environment;
  std::string cwd;
#if defined(_WIN32)
  lines.push_back("HANDLES " + std::to_string(GetCurrentProcessId()));
  int count = 0;
  LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
  for (int index = 0; wide != nullptr && index < count; ++index) arguments.push_back(utf8(wide[index]));
  if (wide != nullptr) LocalFree(wide);
  LPWCH block = GetEnvironmentStringsW();
  for (const wchar_t* entry = block; entry != nullptr && *entry != L'\0'; entry += std::wcslen(entry) + 1U) environment.push_back(utf8(entry));
  if (block != nullptr) FreeEnvironmentStringsW(block);
  std::vector<wchar_t> directory(32768);
  const DWORD length = GetCurrentDirectoryW(static_cast<DWORD>(directory.size()), directory.data());
  cwd = utf8(std::wstring_view(directory.data(), length));
  lines.push_back("SIG none");
#else
  lines.push_back("FDS " + open_descriptors());
  for (int index = 0; index < g_argc; ++index) arguments.emplace_back(g_argv[index]);
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) environment.emplace_back(*entry);
  std::error_code ignored;
  cwd = std::filesystem::current_path(ignored).string();
  lines.push_back("SIG blk=" + status_field("SigBlk") + " ign=" + status_field("SigIgn"));
#endif
  std::sort(environment.begin(), environment.end());
  lines.push_back("ARGV " + join_hex(arguments));
  lines.push_back("ENV " + join_hex(environment));
  lines.push_back("CWD " + hex(cwd));
#if defined(_WIN32)
  lines.push_back("IDS " + token_report());
#else
  lines.push_back("IDS uid=" + std::to_string(getuid()) + " gid=" + std::to_string(getgid()) + " euid=" + std::to_string(geteuid()) +
                  " egid=" + std::to_string(getegid()) + " nnp=" + status_field("NoNewPrivs") + " capeff=" + status_field("CapEff"));
#endif
  lines.emplace_back("END");
  return lines;
}

void dump() {
  for (const auto& line : dump_lines()) std::cout << line << '\n';
  std::cout << std::flush;
}

void write_dump_file(const std::string& path) {
  const std::string temporary = path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc | std::ios::binary);
    for (const auto& line : dump_lines()) out << line << '\n';
  }
  std::error_code ignored;
  std::filesystem::rename(temporary, path, ignored);
}
```

Add `#include <cwchar>` to the includes. In `serve`, directly after the `std::cerr << "diagnostic: ..."` line insert:

```cpp
  if (!options.dump_file.empty()) write_dump_file(options.dump_file);
```

- [ ] **Step 5: Move the two Linux launcher tests onto the parser**

In `secure_frontend/tests/engine_stdio_process_test.cpp` add `#include "support/fake_engine_dump.hpp"` and replace `child_inherits_only_stdio_descriptors` and `child_receives_exact_argv_and_env` with:

```cpp
void child_inherits_only_stdio_descriptors(const std::filesystem::path& fake) {
  // Two extra descriptors without CLOEXEC in the parent; a correct launcher must not leak them.
  const int leak_a = open("/dev/null", O_RDONLY);
  const int leak_b = open("/dev/null", O_RDONLY);
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine"}, .environment = {}}).has_value(),
         "fake engine must start");
  (void)transport.read_line(std::chrono::seconds(2));  // READY
  const auto dump = test_support::request_dump(transport, std::chrono::seconds(2));
  close(leak_a);
  close(leak_b);
  expect(dump.has_value() && dump->fds == std::vector<int>{0, 1, 2}, "child must see only stdio descriptors");
  (void)transport.terminate(std::chrono::milliseconds(250));
}

void child_receives_exact_argv_and_env(const std::filesystem::path& fake) {
  secure_frontend::EngineStdioProcess transport;
  const std::string weird = "literal; $(echo x) \"quoted\" \xE2\x9C\x93 \n-less";
  expect(!transport.start({.executable = fake, .arguments = {"--fake-engine", weird}, .environment = {{"STRATA_T", "1"}}})
              .has_value(),
         "fake engine must start");
  (void)transport.read_line(std::chrono::seconds(2));  // READY
  const auto dump = test_support::request_dump(transport, std::chrono::seconds(2));
  expect(dump.has_value(), "DUMP must parse");
  if (dump) {
    const std::vector<std::string> expected{fake.string(), "--serve", "--fake-engine", weird};
    expect(dump->argv == expected, "argv must arrive literally, no shell");
  }
  (void)transport.terminate(std::chrono::milliseconds(250));
}
```

- [ ] **Step 6: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc 2>&1 | tail -3`
Expected: `100% tests passed, 0 tests failed out of <Task 1 total> + 1` (new: `secure_frontend_fake_engine_tests.fake_engine_dump_is_parseable`).

- [ ] **Step 7: Cross-compile and record wine**

Run: `cmake --build --preset windows-llvm-mingw-cross 2>&1 | tail -2 && WINEPREFIX=$PWD/out/wineprefix wine out/build/windows-llvm-mingw-cross/secure_frontend_fake_engine_tests.exe --case fake_engine_dump_is_parseable; echo "exit $?"`
Expected: compiles with `-Werror`. Record the wine exit code (wine implements `CommandLineToArgvW` and token queries; a wine failure is recorded, not a plan failure — the MSYS2 job is authoritative). `ctest --preset windows-llvm-mingw-cross` still prints W₀ (the new case is labeled).

- [ ] **Step 8: Handoff row** — add to the SP2 table:

```markdown
| F2 | DUMP reports FDS/HANDLES, ARGV (Windows via `CommandLineToArgvW`), sorted ENV, SIG masks, CWD, IDS (uid/gid/NoNewPrivs/CapEff; Windows SID/elevation/integrity), END; `--dump-file`; parser `tests/support/fake_engine_dump.hpp`. | `fake_engine_dump_is_parseable` | <recorded linux line>; wine: <recorded> |
```

- [ ] **Step 9: Commit**

```bash
git add secure_frontend/tests docs/AGENT_HANDOFF.md secure_frontend/CMakeLists.txt
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "test: fake engine DUMP reports env, signals, cwd, ids and portable argv (F2)" \
  -m "F2: fake_engine_dump_is_parseable. child_inherits_only_stdio_descriptors and child_receives_exact_argv_and_env read the report through the shared parser." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 3: Server harness (restricted token on Windows, no orphans), byte tokenizer, process-tree helpers (F3)

**Files:**
- Create: `secure_frontend/tests/support/test_tokenizer.hpp`
- Create: `secure_frontend/tests/support/server_harness.hpp`
- Create: `secure_frontend/tests/support/server_harness.cpp`
- Create: `secure_frontend/tests/server_test.cpp`
- Modify: `secure_frontend/tests/support/test_paths.hpp` (add `server_path()`)
- Modify: `secure_frontend/CMakeLists.txt` (`secure_frontend_test_paths` also defines the server path; static library `secure_frontend_test_support`; binary `secure_frontend_server_tests`; the three server case lists)
- Modify: `docs/AGENT_HANDOFF.md` (F3 row)

**Interfaces:**
- Consumes: `test_support::fake_engine_path()`, `make_temp_dir`, `utf8_path`, `grant_current_user` (Task 1); `secure_frontend_test::{make_private_directory, write_file}` (SP1).
- Produces: `test_support::byte_tokenizer()`, `write_tokenizer_pack(dir)` (token id k decodes to byte `(k + 65) mod 256`, so the fake's `T 7` is `"H"`; files 0600); `test_support::ServerProcess` (`start(ServerOptions)`, `pid()`, `port()`, `root()`, `get(target, timeout)`, `post_chat(body, timeout)`, `wait_until_listening(timeout)`, `wait_for_health(body, timeout)`, `stop(Stop)`, `wait_exit(timeout)`, `port_refuses()`, `stderr_text()`, `descendants()`, `active_processes()`); `ServerOptions{engine_args, extra_args, extra_env, engine}` (every member default-initialized); `children_of(pid)`, `descendants_of(pid)`, `process_token_elevated(pid)`, `http_request(...)`, `pick_free_port()`; literals `kHealthStarting`, `kHealthReady`, `kGeneric503`, `kBadRequest`, `kChatBody`, `kHarnessTokenEnv`, `kHarnessToken` (43 base64url characters); CMake lists `SECURE_FRONTEND_SERVER_CASES`, `SECURE_FRONTEND_SERVER_LINUX_CASES`, `SECURE_FRONTEND_SERVER_WINDOWS_CASES`.
- Harness guarantees: (1) on Windows the server always runs with a LUA-restricted, non-elevated token (Safer `NORMALUSER` fallback; otherwise `start()` fails with a fixed message and the executor stops and asks the owner — there is no test-only override flag); (2) the destructor kills every descendant the harness has seen and reaps the ones re-parented to the test process (Linux: the test process is a child subreaper), so no `--hang-*` / `--stall-stdin` fake or grandchild outlives a test on the dev host; Windows: the kill-on-close harness Job is terminated.

- [ ] **Step 1: Write the failing self-test** — `secure_frontend/tests/server_test.cpp`:

```cpp
#include "support/server_harness.hpp"
#include "support/test_paths.hpp"

#include <chrono>
#include <iostream>
#include <string>

namespace {

using namespace std::chrono_literals;
using test_support::ServerProcess;

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

std::string describe(const std::optional<test_support::HttpResult>& result) {
  return result ? std::to_string(result->status) + " " + result->body : std::string("no response");
}

void harness_server_lifecycle_selftest() {
  expect(test_support::kHarnessToken.size() == 43U, "the harness token is 43 base64url characters (SP1: at least 32 bytes)");
  auto server = ServerProcess::start({});
  expect(server != nullptr, "the harness must start strata-secure-server");
  if (!server) return;
  expect(server->wait_until_listening(10s), "the listener must answer");
#if defined(_WIN32)
  expect(!test_support::process_token_elevated(server->pid()), "the harness runs the server with a non-elevated token");
#endif
  const auto models = server->get("/v1/models");
  expect(models && models->status == 200 && models->body.find("\"fake-model\"") != std::string::npos,
         "the fixture model is listed: " + describe(models));
  const auto health = server->get("/health");
  expect(health && (health->status == 200 || health->status == 503), "/health answers: " + describe(health));
  server->stop(ServerProcess::Stop::kill);
  expect(server->wait_exit(5s).has_value(), "a killed server is reaped");
}

}  // namespace

int main(int argc, char* argv[]) {
  return secure_frontend_test::run_cases(argc, argv, {{"harness_server_lifecycle_selftest", harness_server_lifecycle_selftest}}, failures);
}
```

CMake: replace the `secure_frontend_test_paths` function from Task 1 with

```cmake
    function(secure_frontend_test_paths target)
        target_compile_definitions(${target} PRIVATE
            "STRATA_FAKE_ENGINE_PATH=\"$<TARGET_FILE:strata-supervision-fake-engine>\""
            "STRATA_SECURE_SERVER_PATH=\"$<TARGET_FILE:strata-secure-server>\"")
        add_dependencies(${target} strata-supervision-fake-engine strata-secure-server)
        if(WIN32)
            target_link_libraries(${target} PRIVATE advapi32)  # grant_current_user()
        endif()
    endfunction()
```

and add after `secure_frontend_fake_engine_tests`:

```cmake
    # F3: start/stop the real server with the fake engine; process-tree helpers.
    add_library(secure_frontend_test_support STATIC tests/support/server_harness.cpp)
    target_include_directories(secure_frontend_test_support PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(secure_frontend_test_support PUBLIC cxx_std_20)
    target_link_libraries(secure_frontend_test_support PUBLIC secure_frontend_tokenizer_pack)
    if(WIN32)
        target_link_libraries(secure_frontend_test_support PUBLIC ws2_32 advapi32)
    endif()
    secure_frontend_set_warnings(secure_frontend_test_support)
    secure_frontend_test_paths(secure_frontend_test_support)

    secure_frontend_add_test_executable(secure_frontend_server_tests tests/server_test.cpp secure_frontend_test_support)
    secure_frontend_test_paths(secure_frontend_server_tests)
    # Later tasks append case names to these three set() lines only. Every
    # server-level test is windows-runtime on Windows: it relies on console
    # groups, Jobs, tokens and process exit codes.
    set(SECURE_FRONTEND_SERVER_CASES harness_server_lifecycle_selftest)
    set(SECURE_FRONTEND_SERVER_LINUX_CASES)
    set(SECURE_FRONTEND_SERVER_WINDOWS_CASES)
    if(WIN32)
        list(APPEND SECURE_FRONTEND_SERVER_CASES ${SECURE_FRONTEND_SERVER_WINDOWS_CASES})
    else()
        list(APPEND SECURE_FRONTEND_SERVER_CASES ${SECURE_FRONTEND_SERVER_LINUX_CASES})
    endif()
    secure_frontend_add_cases(secure_frontend_server_tests ${SECURE_FRONTEND_SERVER_CASES}
        TIMEOUT 120 LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --preset linux-gcc && cmake --build --preset linux-gcc 2>&1 | tail -3`
Expected: FAIL — `Cannot find source file: tests/support/server_harness.cpp`.

- [ ] **Step 3: Write the tokenizer fixture** — `secure_frontend/tests/support/test_tokenizer.hpp`:

```cpp
#pragma once

#include "secure_frontend/tokenizer_pack.hpp"

#include "support/test_support.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace test_support {

// GPT-2 byte-alphabet symbol for one byte (the encoding tokenizer packs use).
inline std::string byte_symbol(const unsigned char byte) {
  if (byte >= 33U && byte <= 126U) return std::string(1U, static_cast<char>(byte));
  unsigned int printable_before = 0U;
  for (unsigned int value = 33U; value <= 126U && value < byte; ++value) ++printable_before;
  const unsigned int code_point = 256U + static_cast<unsigned int>(byte) - printable_before;
  return {static_cast<char>(0xc0U | (code_point >> 6U)), static_cast<char>(0x80U | (code_point & 0x3fU))};
}

// Token id k is the single byte (k + 65) mod 256, so the fake engine's `T 7` decodes to "H"
// (and a '0' byte is id 239: three decimal digits on the GEN line, used by K2 in Task 22).
inline unsigned char byte_for_id(const std::size_t id) { return static_cast<unsigned char>((id + 65U) & 0xffU); }

// Every byte as an ordinary token (type 1) plus the Qwen framing controls (type 3).
inline secure_frontend::TokenizerPack byte_tokenizer() {
  secure_frontend::TokenizerPack pack;
  for (std::size_t id = 0; id < 256U; ++id) {
    pack.tokens_by_id.push_back(byte_symbol(byte_for_id(id)));
    pack.token_types.push_back(1);
  }
  pack.tokens_by_id.emplace_back("<|im_start|>");
  pack.token_types.push_back(3);
  pack.tokens_by_id.emplace_back("<|im_end|>");
  pack.token_types.push_back(3);
  return pack;
}

// The same pack as the three files load_local_tokenizer_pack() reads, each
// written 0600 (SP1's fixture rule) into an existing 0700 directory.
inline void write_tokenizer_pack(const std::filesystem::path& directory) {
  const auto pack = byte_tokenizer();
  std::string vocab = "{";
  for (std::size_t id = 0; id < pack.tokens_by_id.size(); ++id) {
    if (id != 0U) vocab += ',';
    vocab += '"';
    for (const char character : pack.tokens_by_id[id]) {
      if (character == '"' || character == '\\') vocab += '\\';
      vocab += character;
    }
    vocab += "\":" + std::to_string(id);
  }
  vocab += '}';
  std::string types = "[";
  for (std::size_t index = 0; index < pack.token_types.size(); ++index) {
    if (index != 0U) types += ',';
    types += std::to_string(pack.token_types[index]);
  }
  types += ']';
  secure_frontend_test::write_file(directory / "vocab.json", vocab, 0600);
  secure_frontend_test::write_file(directory / "merges.txt", "", 0600);
  secure_frontend_test::write_file(directory / "token_type.json", types, 0600);
}

}  // namespace test_support
```

Add to `secure_frontend/tests/support/test_paths.hpp`, after `fake_engine_path()`:

```cpp
inline std::filesystem::path server_path() { return std::filesystem::path(STRATA_SECURE_SERVER_PATH); }
```

- [ ] **Step 4: Write the harness header** — `secure_frontend/tests/support/server_harness.hpp`:

```cpp
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace test_support {

inline constexpr std::string_view kHarnessTokenEnv = "STRATA_HARNESS_TOKEN";
// 43 base64url characters (32 random-looking bytes' worth): SP1 rejects tokens under 32 bytes.
inline constexpr std::string_view kHarnessToken = "SP2harnessToken_0123456789-abcdefghijklmnop";
inline constexpr std::string_view kHealthStarting = "{\"status\":\"unavailable\",\"engine\":\"starting\"}\n";
inline constexpr std::string_view kHealthReady = "{\"status\":\"ok\",\"engine\":\"ready\"}\n";
inline constexpr std::string_view kGeneric503 = "{\"error\":\"Service Unavailable\"}\n";
inline constexpr std::string_view kBadRequest = "{\"error\":\"Bad Request\"}\n";
inline constexpr std::string_view kChatBody = R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"max_tokens":2})";

// POSIX: the direct children of `pid` (from every thread's /proc/<pid>/task/<tid>/children). Windows: empty.
std::vector<long> children_of(long pid);
// POSIX: every descendant of `pid`, breadth first. Windows: empty.
std::vector<long> descendants_of(long pid);
// Windows: TokenElevation of process `pid` (true when it cannot be read). POSIX: false.
bool process_token_elevated(long pid);
// A loopback TCP port that was free a moment ago.
std::uint16_t pick_free_port();

struct HttpResult final {
  int status{0};
  std::string body;
};

// One authenticated HTTP/1.1 request to 127.0.0.1:<port>; nullopt if no complete status line arrived in time.
std::optional<HttpResult> http_request(std::uint16_t port, std::string_view method, std::string_view target, std::string_view body,
                                       std::chrono::milliseconds timeout);
bool port_refuses(std::uint16_t port);

// Every member has a default initializer, so designated initializers that name
// only some of them keep compiling when later tasks add members (Task 18: state_dir).
struct ServerOptions final {
  std::vector<std::string> engine_args{};            // each passed as --engine-arg <value>
  std::vector<std::string> extra_args{};             // appended verbatim
  std::map<std::string, std::string> extra_env{};    // added to the server's own environment
  std::optional<std::filesystem::path> engine{};     // default: the fake engine
};

// strata-secure-server as a test child: fixture models (fake-model.gguf, 0600)
// and the byte tokenizer pack in 0700 directories, a free port, token from
// kHarnessTokenEnv; stdout/stderr go to <root>/stdout.log and <root>/stderr.log.
// Windows: started suspended with a non-elevated (LUA-restricted) token in its
// own console process group (so CTRL_BREAK_EVENT can target it) inside a
// kill-on-close harness Job used to count descendants and to clean up.
// Linux: the test process becomes a child subreaper, so descendants orphaned
// by a server that died are re-parented here and reaped by the destructor.
class ServerProcess final {
 public:
  enum class Stop { term, interrupt, kill, ctrl_break };

  static std::unique_ptr<ServerProcess> start(const ServerOptions& options);
  ~ServerProcess();
  ServerProcess(const ServerProcess&) = delete;
  ServerProcess& operator=(const ServerProcess&) = delete;

  [[nodiscard]] long pid() const noexcept { return pid_; }
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
  [[nodiscard]] std::optional<HttpResult> get(std::string_view target,
                                              std::chrono::milliseconds timeout = std::chrono::seconds(5)) const;
  [[nodiscard]] std::optional<HttpResult> post_chat(std::string_view body,
                                                    std::chrono::milliseconds timeout = std::chrono::seconds(5)) const;
  [[nodiscard]] bool wait_until_listening(std::chrono::milliseconds timeout);
  [[nodiscard]] bool wait_for_health(std::string_view body, std::chrono::milliseconds timeout) const;
  // POSIX: SIGTERM / SIGINT / SIGKILL (ctrl_break sends SIGINT). Windows: CTRL_BREAK_EVENT to the
  // server's console group for term/interrupt/ctrl_break, TerminateProcess for kill.
  void stop(Stop how);
  // Exit code; POSIX 128 + signal for a signal death.
  [[nodiscard]] std::optional<int> wait_exit(std::chrono::milliseconds timeout);
  [[nodiscard]] bool port_refuses() const;
  [[nodiscard]] std::string stderr_text() const;
  // POSIX: every descendant. Windows: every process in the harness Job except the server.
  [[nodiscard]] std::vector<long> descendants() const;
  // Windows: the harness Job's ActiveProcesses. POSIX: 1 + descendants() while the server lives, else 0.
  [[nodiscard]] unsigned long active_processes();

 private:
  ServerProcess() = default;
  bool launch(const std::vector<std::string>& arguments, const std::map<std::string, std::string>& environment);
  // POSIX: adds the server's current descendants to known_descendants_ (the destructor kills them all).
  void remember_descendants() const;

  std::filesystem::path root_;
  std::uint16_t port_{0};
  long pid_{-1};
  std::optional<int> exit_code_;
  mutable std::set<long> known_descendants_;
#if defined(_WIN32)
  void* process_{nullptr};
  void* job_{nullptr};
#endif
};

}  // namespace test_support
```

- [ ] **Step 5: Write the harness** — `secure_frontend/tests/support/server_harness.cpp`:

```cpp
#include "server_harness.hpp"

#include "test_paths.hpp"
#include "test_tokenizer.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <winsafer.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

namespace test_support {
namespace {

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void close_socket(const Socket socket) { (void)closesocket(socket); }
void ensure_winsock() {
  static const bool started = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  (void)started;
}
std::wstring widen(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(std::max(size, 0)), L'\0');
  if (size > 0) (void)MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
  return out;
}
std::wstring quote(const std::wstring& argument) {
  std::wstring result{L"\""};
  std::size_t slashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++slashes;
    } else if (character == L'"') {
      result.append(slashes * 2 + 1, L'\\');
      result += character;
      slashes = 0;
    } else {
      result.append(slashes, L'\\');
      result += character;
      slashes = 0;
    }
  }
  result.append(slashes * 2, L'\\');
  result += L'"';
  return result;
}

bool token_elevated(HANDLE token) {
  TOKEN_ELEVATION elevation{};
  DWORD size = sizeof(elevation);
  return GetTokenInformation(token, TokenElevation, &elevation, size, &size) && elevation.TokenIsElevated != 0;
}

// Spec G4: the server refuses an elevated token, and GitHub's Windows runners
// (and any elevated developer shell) are elevated. So the harness ALWAYS starts
// the server with a LUA-restricted copy of its own token (CreateProcessAsUserW
// needs no extra privilege for a restricted copy of the caller's token). There
// is no test-only override flag in the server.
HANDLE lua_restricted_token() {
  HANDLE own = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &own)) return nullptr;
  HANDLE restricted = nullptr;
  if (!CreateRestrictedToken(own, LUA_TOKEN | DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr, &restricted)) restricted = nullptr;
  CloseHandle(own);
  return restricted;
}

// Fallback when the LUA copy still reports elevation: a Safer NORMALUSER token.
HANDLE safer_normal_user_token() {
  SAFER_LEVEL_HANDLE level = nullptr;
  if (!SaferCreateLevel(SAFER_SCOPEID_USER, SAFER_LEVELID_NORMALUSER, SAFER_LEVEL_OPEN, &level, nullptr)) return nullptr;
  HANDLE token = nullptr;
  if (!SaferComputeTokenFromLevel(level, nullptr, &token, 0, nullptr)) token = nullptr;
  (void)SaferCloseLevel(level);
  return token;
}

// A non-elevated primary token for the server, or nullptr: then the executor
// STOPS and asks the owner (Global Constraints: no override flag exists).
HANDLE non_elevated_server_token() {
  HANDLE token = lua_restricted_token();
  if (token != nullptr && !token_elevated(token)) return token;
  if (token != nullptr) CloseHandle(token);
  token = safer_normal_user_token();
  if (token != nullptr && !token_elevated(token)) return token;
  if (token != nullptr) CloseHandle(token);
  return nullptr;
}
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void close_socket(const Socket socket) { (void)close(socket); }
void ensure_winsock() {}
#endif

void set_timeouts(const Socket socket, const std::chrono::milliseconds timeout) {
#if defined(_WIN32)
  const DWORD milliseconds = static_cast<DWORD>(timeout.count());
  (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
  (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
#else
  timeval value{};
  value.tv_sec = static_cast<time_t>(timeout.count() / 1000);
  value.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
  (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
  (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
#endif
}

Socket connect_loopback(const std::uint16_t port, const std::chrono::milliseconds timeout) {
  ensure_winsock();
  const Socket socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_handle == kInvalidSocket) return kInvalidSocket;
  set_timeouts(socket_handle, timeout);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(socket_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    close_socket(socket_handle);
    return kInvalidSocket;
  }
  return socket_handle;
}

}  // namespace

std::vector<long> children_of(const long pid) {
  std::vector<long> children;
#if !defined(_WIN32)
  std::error_code error;
  for (const auto& task : std::filesystem::directory_iterator("/proc/" + std::to_string(pid) + "/task", error)) {
    std::ifstream input(task.path() / "children");
    long child = 0;
    while (input >> child) children.push_back(child);
  }
  std::sort(children.begin(), children.end());
  children.erase(std::unique(children.begin(), children.end()), children.end());
#else
  (void)pid;
#endif
  return children;
}

std::vector<long> descendants_of(const long pid) {
  std::vector<long> result;
  std::deque<long> pending{pid};
  while (!pending.empty()) {
    const long current = pending.front();
    pending.pop_front();
    for (const long child : children_of(current)) {
      result.push_back(child);
      pending.push_back(child);
    }
  }
  return result;
}

bool process_token_elevated(const long pid) {
#if defined(_WIN32)
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) return true;
  HANDLE token = nullptr;
  const bool opened = OpenProcessToken(process, TOKEN_QUERY, &token) != 0;
  CloseHandle(process);
  if (!opened) return true;
  const bool elevated = token_elevated(token);
  CloseHandle(token);
  return elevated;
#else
  (void)pid;
  return false;
#endif
}

std::uint16_t pick_free_port() {
  ensure_winsock();
  const Socket socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
#if defined(_WIN32)
  int length = sizeof(address);
#else
  socklen_t length = sizeof(address);
#endif
  std::uint16_t port = 0;
  if (socket_handle != kInvalidSocket && bind(socket_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
      getsockname(socket_handle, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
    port = ntohs(address.sin_port);
  }
  if (socket_handle != kInvalidSocket) close_socket(socket_handle);
  return port;
}

std::optional<HttpResult> http_request(const std::uint16_t port, const std::string_view method, const std::string_view target,
                                       const std::string_view body, const std::chrono::milliseconds timeout) {
  const Socket socket_handle = connect_loopback(port, timeout);
  if (socket_handle == kInvalidSocket) return std::nullopt;
  std::string request = std::string(method) + " " + std::string(target) + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                        "\r\nAuthorization: Bearer " + std::string(kHarnessToken) + "\r\nConnection: close\r\n";
  if (!body.empty()) request += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  request += "\r\n";
  request.append(body);
  std::size_t sent = 0;
  while (sent < request.size()) {
#if defined(_WIN32)
    const int count = send(socket_handle, request.data() + sent, static_cast<int>(request.size() - sent), 0);
#else
    const ssize_t count = send(socket_handle, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
#endif
    if (count <= 0) {
      close_socket(socket_handle);
      return std::nullopt;
    }
    sent += static_cast<std::size_t>(count);
  }
  std::string response;
  std::array<char, 4096> buffer{};
  for (;;) {
#if defined(_WIN32)
    const int count = recv(socket_handle, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
    const ssize_t count = recv(socket_handle, buffer.data(), buffer.size(), 0);
#endif
    if (count <= 0) break;
    response.append(buffer.data(), static_cast<std::size_t>(count));
  }
  close_socket(socket_handle);
  if (response.size() < 12U || response.rfind("HTTP/1.1 ", 0) != 0) return std::nullopt;
  HttpResult result;
  result.status = std::stoi(response.substr(9, 3));
  const auto split = response.find("\r\n\r\n");
  if (split != std::string::npos) result.body = response.substr(split + 4U);
  return result;
}

bool port_refuses(const std::uint16_t port) {
  const Socket socket_handle = connect_loopback(port, std::chrono::milliseconds(500));
  if (socket_handle == kInvalidSocket) return true;
  close_socket(socket_handle);
  return false;
}

std::unique_ptr<ServerProcess> ServerProcess::start(const ServerOptions& options) {
#if defined(__linux__)
  // Orphans of a server that died (or was SIGKILLed by a test) re-parent to
  // this process, so the destructor can kill and reap them.
  static const bool subreaper = prctl(PR_SET_CHILD_SUBREAPER, 1UL, 0UL, 0UL, 0UL) == 0;
  (void)subreaper;
#endif
  std::unique_ptr<ServerProcess> server(new ServerProcess());
  server->root_ = make_temp_dir("server");  // 0700; Windows: the current user is granted access
  const auto models = server->root_ / "models";
  const auto tokenizer = server->root_ / "tokenizer";
  secure_frontend_test::make_private_directory(models);     // 0700
  secure_frontend_test::make_private_directory(tokenizer);  // 0700
  secure_frontend_test::write_file(models / "fake-model.gguf", "GGUF", 0600);
  write_tokenizer_pack(tokenizer);                           // 0600 files
  server->port_ = pick_free_port();
  std::vector<std::string> arguments{utf8_path(server_path()), "--models-root", utf8_path(models), "--tokenizer-pack",
                                     utf8_path(tokenizer), "--engine", utf8_path(options.engine.value_or(fake_engine_path())),
                                     "--model-id", "fake-model", "--token-env", std::string(kHarnessTokenEnv), "--port",
                                     std::to_string(server->port_)};
  for (const auto& argument : options.engine_args) {
    arguments.emplace_back("--engine-arg");
    arguments.push_back(argument);
  }
  arguments.insert(arguments.end(), options.extra_args.begin(), options.extra_args.end());
  std::map<std::string, std::string> environment{{std::string(kHarnessTokenEnv), std::string(kHarnessToken)}};
#if defined(_WIN32)
  // The server itself (not the engine) needs these for Winsock and the CRT.
  for (const char* name : {"SystemRoot", "SystemDrive", "WINDIR", "PATH", "TEMP", "TMP"}) {
    if (const char* value = std::getenv(name)) environment.emplace(name, value);
  }
#else
  environment.emplace("PATH", "/usr/bin:/bin");
#endif
  for (const auto& [name, value] : options.extra_env) environment[name] = value;
  if (!server->launch(arguments, environment)) {
    std::cerr << "harness: could not launch strata-secure-server\n";
    return nullptr;
  }
  return server;
}

bool ServerProcess::launch(const std::vector<std::string>& arguments, const std::map<std::string, std::string>& environment) {
  const std::string out_log = utf8_path(root_ / "stdout.log");
  const std::string err_log = utf8_path(root_ / "stderr.log");
#if defined(_WIN32)
  std::wstring command_line;
  for (const auto& argument : arguments) {
    if (!command_line.empty()) command_line += L' ';
    command_line += quote(widen(argument));
  }
  std::wstring block;
  for (const auto& [name, value] : environment) {
    block += widen(name) + L"=" + widen(value);
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  HANDLE token = non_elevated_server_token();
  if (token == nullptr) {
    std::cerr << "harness: no non-elevated token for strata-secure-server (LUA and Safer NORMALUSER both elevated); "
                 "STOP and ask the owner -- there is no test-only override\n";
    return false;
  }
  SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
  HANDLE output = CreateFileW(widen(out_log).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS, 0, nullptr);
  HANDLE error = CreateFileW(widen(err_log).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS, 0, nullptr);
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  const bool ready = input != INVALID_HANDLE_VALUE && output != INVALID_HANDLE_VALUE && error != INVALID_HANDLE_VALUE && job != nullptr &&
                     SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = input;
  startup.hStdOutput = output;
  startup.hStdError = error;
  PROCESS_INFORMATION info{};
  const std::wstring executable = widen(arguments.front());
  const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED;
  const bool created = ready && CreateProcessAsUserW(token, executable.c_str(), command_line.data(), nullptr, nullptr, TRUE, flags,
                                                     block.data(), nullptr, &startup, &info);
  CloseHandle(token);
  for (HANDLE handle : {input, output, error}) {
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
  if (!created) {
    if (job != nullptr) CloseHandle(job);
    return false;
  }
  // Still suspended: refuse to run a server whose token is elevated after all.
  HANDLE server_token = nullptr;
  const bool elevated = OpenProcessToken(info.hProcess, TOKEN_QUERY, &server_token) == 0 || token_elevated(server_token);
  if (server_token != nullptr) CloseHandle(server_token);
  if (elevated || !AssignProcessToJobObject(job, info.hProcess)) {
    if (elevated) std::cerr << "harness: the server token is elevated; STOP and ask the owner\n";
    (void)TerminateProcess(info.hProcess, 1);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    CloseHandle(job);
    return false;
  }
  (void)ResumeThread(info.hThread);
  CloseHandle(info.hThread);
  process_ = info.hProcess;
  job_ = job;
  pid_ = static_cast<long>(info.dwProcessId);
  return true;
#else
  std::vector<std::string> argument_storage = arguments;
  std::vector<char*> argv;
  for (auto& argument : argument_storage) argv.push_back(argument.data());
  argv.push_back(nullptr);
  std::vector<std::string> environment_storage;
  for (const auto& [name, value] : environment) environment_storage.push_back(name + "=" + value);
  std::vector<char*> envp;
  for (auto& entry : environment_storage) envp.push_back(entry.data());
  envp.push_back(nullptr);
  const pid_t child = fork();
  if (child < 0) return false;
  if (child == 0) {
    const int input = open("/dev/null", O_RDONLY);
    const int output = open(out_log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const int error = open(err_log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (input < 0 || output < 0 || error < 0 || dup2(input, 0) < 0 || dup2(output, 1) < 0 || dup2(error, 2) < 0) _exit(126);
    execve(argv[0], argv.data(), envp.data());
    _exit(127);
  }
  pid_ = static_cast<long>(child);
  return true;
#endif
}

void ServerProcess::remember_descendants() const {
#if !defined(_WIN32)
  if (!exit_code_ && pid_ > 0) {
    for (const long pid : descendants_of(pid_)) known_descendants_.insert(pid);
  }
#endif
}

// No fake engine (--hang-*, --stall-stdin) or grandchild may outlive a test on
// the dev host, however the test ended.
ServerProcess::~ServerProcess() {
  remember_descendants();
  if (!exit_code_ && pid_ > 0) {
    stop(Stop::kill);
    (void)wait_exit(std::chrono::seconds(5));
  }
#if defined(_WIN32)
  if (job_ != nullptr) (void)TerminateJobObject(static_cast<HANDLE>(job_), 1);
  if (process_ != nullptr) CloseHandle(static_cast<HANDLE>(process_));
  if (job_ != nullptr) CloseHandle(static_cast<HANDLE>(job_));  // kill-on-close: nothing of the server survives the harness
#else
  for (const long pid : known_descendants_) {
    (void)kill(static_cast<pid_t>(pid), SIGKILL);
    // Re-parented to this subreaper: reap it. Not our child (ECHILD) or already gone: done.
    for (int step = 0; step < 100; ++step) {
      if (waitpid(static_cast<pid_t>(pid), nullptr, WNOHANG) != 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
#endif
}

std::optional<HttpResult> ServerProcess::get(const std::string_view target, const std::chrono::milliseconds timeout) const {
  return http_request(port_, "GET", target, {}, timeout);
}

std::optional<HttpResult> ServerProcess::post_chat(const std::string_view body, const std::chrono::milliseconds timeout) const {
  return http_request(port_, "POST", "/v1/chat/completions", body, timeout);
}

bool ServerProcess::wait_until_listening(const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (get("/health", std::chrono::milliseconds(500)).has_value()) return true;
    if (wait_exit(std::chrono::milliseconds(0)).has_value()) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

bool ServerProcess::wait_for_health(const std::string_view body, const std::chrono::milliseconds timeout) const {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto health = get("/health", std::chrono::milliseconds(500));
    if (health && health->body == body) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

void ServerProcess::stop(const Stop how) {
  if (pid_ <= 0) return;
  remember_descendants();
#if defined(_WIN32)
  if (how == Stop::kill) {
    (void)TerminateProcess(static_cast<HANDLE>(process_), 1);
  } else {
    (void)GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(pid_));
  }
#else
  const int signal_number = how == Stop::term ? SIGTERM : how == Stop::kill ? SIGKILL : SIGINT;
  (void)kill(static_cast<pid_t>(pid_), signal_number);
#endif
}

std::optional<int> ServerProcess::wait_exit(const std::chrono::milliseconds timeout) {
  if (exit_code_) return exit_code_;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
#if defined(_WIN32)
    if (WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_OBJECT_0) {
      DWORD code = 1;
      (void)GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
      exit_code_ = static_cast<int>(code);
      return exit_code_;
    }
#else
    int status = 0;
    if (waitpid(static_cast<pid_t>(pid_), &status, WNOHANG) == static_cast<pid_t>(pid_)) {
      exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      return exit_code_;
    }
#endif
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

bool ServerProcess::port_refuses() const { return test_support::port_refuses(port_); }

std::string ServerProcess::stderr_text() const {
  std::ifstream input(root_ / "stderr.log", std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

std::vector<long> ServerProcess::descendants() const {
#if defined(_WIN32)
  std::vector<unsigned char> buffer(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 256U * sizeof(ULONG_PTR));
  auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
  list->NumberOfAssignedProcesses = 256U;
  std::vector<long> pids;
  if (QueryInformationJobObject(static_cast<HANDLE>(job_), JobObjectBasicProcessIdList, list, static_cast<DWORD>(buffer.size()), nullptr)) {
    const ULONG_PTR* ids = list->ProcessIdList;
    for (DWORD index = 0; index < list->NumberOfProcessIdsInList; ++index) {
      const long pid = static_cast<long>(ids[index]);
      if (pid != pid_) pids.push_back(pid);
    }
  }
  return pids;
#else
  remember_descendants();
  return descendants_of(pid_);
#endif
}

unsigned long ServerProcess::active_processes() {
#if defined(_WIN32)
  JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
  if (!QueryInformationJobObject(static_cast<HANDLE>(job_), JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
    return 0;
  }
  return accounting.ActiveProcesses;
#else
  if (wait_exit(std::chrono::milliseconds(0)).has_value()) return 0;
  remember_descendants();
  return 1U + static_cast<unsigned long>(descendants_of(pid_).size());
#endif
}

}  // namespace test_support
```

- [ ] **Step 6: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc 2>&1 | tail -3`
Expected: `100% tests passed, 0 tests failed out of <Task 2 total> + 1` (new: `secure_frontend_server_tests.harness_server_lifecycle_selftest`). Then `pgrep -f strata-supervision-fake-engine || echo none` prints `none`: the harness left no fake behind.

- [ ] **Step 7: Cross-compile and record wine** — `cmake --build --preset windows-llvm-mingw-cross 2>&1 | tail -2` compiles with `-Werror` (`winsafer.h`, `CreateRestrictedToken`, `CreateProcessAsUserW`, `SetEntriesInAclW` all resolve against the MinGW-w64 headers and `advapi32`); `WINEPREFIX=$PWD/out/wineprefix wine out/build/windows-llvm-mingw-cross/secure_frontend_server_tests.exe --case harness_server_lifecycle_selftest; echo "exit $?"` — record the exit code (wine has no UAC; its token answers are not authoritative). `ctest --preset windows-llvm-mingw-cross` stays at the Task 2 wine total (the case is labeled). The restricted-token launch is proven only by the MSYS2 job; if that job shows `harness: no non-elevated token ...` or `the server token is elevated`, STOP and ask the owner.

- [ ] **Step 8: Handoff row**

```markdown
| F3 | `tests/support/server_harness.{hpp,cpp}` starts `strata-secure-server` with the fake engine (0700 fixture dirs, 0600 files, 43-character token, free port); on Windows always with a LUA-restricted non-elevated token (Safer `NORMALUSER` fallback; no override flag) and asserts it; stops it by signal / `CTRL_BREAK_EVENT` / kill; the destructor kills every descendant it has seen (Linux subreaper reaps orphans; Windows Job); counts descendants; reads stderr; `test_tokenizer.hpp`. | `secure_frontend_server_tests.harness_server_lifecycle_selftest` | <recorded linux line>; wine: <recorded>; Windows token path authoritative in MSYS2 |
```

- [ ] **Step 9: Commit**

```bash
git add secure_frontend/tests secure_frontend/CMakeLists.txt docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "test: server harness with a non-elevated server token and no orphaned fakes (F3)" \
  -m "F3: secure_frontend_server_tests.harness_server_lifecycle_selftest. The harness starts strata-secure-server with the fake engine and a byte tokenizer pack (SP1 fixture rules: 0700 dirs, 0600 files, 43-character token), on Windows under a LUA-restricted token, and kills every descendant it has seen when it goes away." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 4: One deadline rule; `terminate` contract; checked `current_executable()` (K1, K3, K5)

**Files:**
- Create: `secure_frontend/include/secure_frontend/deadline.hpp`
- Create: `secure_frontend/tests/deadline_test.cpp`
- Modify: `secure_frontend/src/engine_protocol_client.cpp:102-113` (`await_ready` deadline loop) and `:174-188` (`read_generation` deadline loop)
- Modify: `secure_frontend/src/engine_stdio_process.cpp:213-219` and `:252-256` (`read_line`)
- Modify: `secure_frontend/include/secure_frontend/engine_stdio_process.hpp:31` (K3 comment on `terminate`)
- Modify: `secure_frontend/tests/support/test_paths.hpp` (checked `current_executable()`, K5)
- Modify: `secure_frontend/tests/engine_process_test.cpp:28-38` (use the checked helper; SP1 Task 2's `main` is otherwise unchanged)
- Modify: `secure_frontend/CMakeLists.txt` (deadline test binary; `secure_frontend_test_paths(secure_frontend_engine_process_tests)`)
- Modify: `docs/AGENT_HANDOFF.md` (K1/K3/K5 row; remove the two "Known small gaps" bullets about the duplicated deadline loops and the `current_executable()` check, and the one about the undocumented `terminate` contract)

**Interfaces:**
- Produces: `secure_frontend::deadline_after(std::chrono::milliseconds timeout, MonotonicClock::time_point now = MonotonicClock::now())`, `secure_frontend::remaining_until(MonotonicClock::time_point deadline, MonotonicClock::time_point now = MonotonicClock::now())` (never negative); the rule "a zero (or negative) budget is already expired: no read, no wait, on every path"; `test_support::current_executable()`.

- [ ] **Step 1: Write the failing test** — `secure_frontend/tests/deadline_test.cpp`:

```cpp
#include "secure_frontend/deadline.hpp"
#include "secure_frontend/engine_protocol_client.hpp"
#include "secure_frontend/engine_stdio_process.hpp"

#include "support/test_paths.hpp"

#include <chrono>
#include <deque>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

class CountingTransport final : public secure_frontend::EngineLineTransport {
 public:
  std::deque<secure_frontend::EngineReadResult> reads;
  int read_calls{0};
  bool write_line(std::string_view, std::string&) override { return true; }
  secure_frontend::EngineReadResult read_line(std::chrono::milliseconds) override {
    ++read_calls;
    if (reads.empty()) return {secure_frontend::EngineReadKind::timeout, {}, {}};
    auto result = std::move(reads.front());
    reads.pop_front();
    return result;
  }
};

secure_frontend::EngineReadResult line(std::string text) { return {secure_frontend::EngineReadKind::line, std::move(text), {}}; }

void deadline_zero_timeout_is_immediate_everywhere() {
  using secure_frontend::EngineProtocolErrorCode;
  const auto now = std::chrono::steady_clock::now();
  expect(secure_frontend::remaining_until(secure_frontend::deadline_after(0ms, now), now) == 0ms, "zero budget: nothing remains");
  expect(secure_frontend::remaining_until(secure_frontend::deadline_after(-5ms, now), now) == 0ms, "a negative budget is zero");
  expect(secure_frontend::remaining_until(secure_frontend::deadline_after(1500ms, now), now) == 1500ms, "a positive budget is kept");
  expect(secure_frontend::remaining_until(now, now + 1ms) == 0ms, "a passed deadline leaves zero, never a negative value");

  // await_ready: expired before the first read; READY stays queued.
  CountingTransport transport;
  transport.reads = {line("READY 4096 stop")};
  secure_frontend::EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  const auto ready_error = client.await_ready(0ms, ready);
  expect(ready_error && ready_error->code == EngineProtocolErrorCode::timeout, "await_ready(0) is an immediate timeout");
  expect(transport.read_calls == 0 && transport.reads.size() == 1U, "await_ready(0) reads nothing");
  expect(!client.await_ready(1s, ready).has_value(), "the queued READY is still delivered afterwards");

  // read_generation: the same rule.
  transport.reads = {line("T 1"), line("DONE stop")};
  expect(!client.begin_generation(1, {1}).has_value(), "GEN");
  const int calls_before = transport.read_calls;
  secure_frontend::EngineGeneration generation{};
  const auto generation_error = client.read_generation(0ms, generation);
  expect(generation_error && generation_error->code == EngineProtocolErrorCode::timeout, "read_generation(0) is an immediate timeout");
  expect(transport.read_calls == calls_before && transport.reads.size() == 2U, "read_generation(0) reads nothing");

  // read_line on the real transport: a buffered line is not consumed by a zero budget.
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value(), "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  std::string error;
  expect(child.write_line("GEN 1 1", error), "GEN");
  expect(child.read_line(5s).line == "T 7", "T 7 (the fake writes DONE in the same flush, so it waits in the buffer)");
  std::this_thread::sleep_for(100ms);
  expect(child.read_line(0ms).kind == secure_frontend::EngineReadKind::timeout, "read_line(0) is an immediate timeout even with a buffered line");
  expect(child.read_line(5s).line == "DONE stop", "the buffered line is still delivered afterwards");
  (void)child.terminate(250ms);
}

}  // namespace

int main(int argc, char* argv[]) {
  return secure_frontend_test::run_cases(argc, argv, {{"deadline_zero_timeout_is_immediate_everywhere", deadline_zero_timeout_is_immediate_everywhere}},
                                 failures);
}
```

Create `secure_frontend/include/secure_frontend/deadline.hpp` now (the test needs it to compile; the bug is in its callers):

```cpp
#pragma once

#include <algorithm>
#include <chrono>

namespace secure_frontend {

using MonotonicClock = std::chrono::steady_clock;

// The one deadline rule (SP2 K1) used by EngineProtocolClient::await_ready,
// EngineProtocolClient::read_generation and EngineStdioProcess::read_line:
// a deadline is `now + timeout` with a negative timeout treated as zero, and
// the remaining budget is never negative. A zero budget means "already
// expired": the call returns a timeout without reading or waiting, even when
// a complete line is already buffered. SP1 rejects 0 in configuration; this
// rule only decides what a 0 that slips through does.
[[nodiscard]] inline MonotonicClock::time_point deadline_after(const std::chrono::milliseconds timeout,
                                                                const MonotonicClock::time_point now = MonotonicClock::now()) noexcept {
  return now + std::max(timeout, std::chrono::milliseconds::zero());
}

[[nodiscard]] inline std::chrono::milliseconds remaining_until(const MonotonicClock::time_point deadline,
                                                                const MonotonicClock::time_point now = MonotonicClock::now()) noexcept {
  if (now >= deadline) return std::chrono::milliseconds::zero();
  return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
}

}  // namespace secure_frontend
```

CMake (tests block):

```cmake
    secure_frontend_add_test_executable(secure_frontend_deadline_tests tests/deadline_test.cpp secure_frontend_engine_stdio_process)
    secure_frontend_test_paths(secure_frontend_deadline_tests)
    # Portable and unlabeled: it also runs under wine.
    secure_frontend_add_cases(secure_frontend_deadline_tests deadline_zero_timeout_is_immediate_everywhere TIMEOUT 120)
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R deadline_zero --output-on-failure 2>&1 | grep FAILED`
Expected: `FAILED: await_ready(0) reads nothing` and `FAILED: read_line(0) is an immediate timeout even with a buffered line` (today `await_ready` reads with a 0 budget and `read_line` returns a buffered line first).

- [ ] **Step 3: Use the rule in all three loops**

`secure_frontend/src/engine_protocol_client.cpp`: add `#include "secure_frontend/deadline.hpp"`. In `await_ready`, replace lines 107-113 (from `const auto duration = ...` to the `if (response.kind == EngineReadKind::timeout)` line's opening) with:

```cpp
  const auto deadline = deadline_after(timeout);
  while (true) {
    const auto remaining = remaining_until(deadline);
    if (remaining.count() == 0) {
      return EngineProtocolError{EngineProtocolErrorCode::timeout, "engine did not report READY before timeout"};
    }
    const auto response = transport_.read_line(remaining);
    if (response.kind == EngineReadKind::timeout) {
```

In `read_generation`, replace lines 178-186 (from `const auto deadline = ...` through `if (response.kind == EngineReadKind::timeout) {`) with:

```cpp
  const auto deadline = deadline_after(timeout);
  while (true) {
    const auto remaining = remaining_until(deadline);
    if (remaining.count() == 0) {
      return EngineProtocolError{EngineProtocolErrorCode::timeout, "engine generation read timed out"};
    }
    const auto response = transport_.read_line(remaining);
    if (response.kind == EngineReadKind::timeout) {
```

`secure_frontend/src/engine_stdio_process.cpp`: add `#include "secure_frontend/deadline.hpp"`. Replace line 214 (`if (auto pending = take_pending_line(); ...`) with:

```cpp
  // K1: a zero budget is already expired on every path, even with a buffered line.
  if (timeout.count() <= 0) return {EngineReadKind::timeout, {}, {}};
  if (auto pending = take_pending_line(); pending.has_value()) return std::move(*pending);
```

In the Windows branch replace `const auto deadline = std::chrono::steady_clock::now() + timeout;` (line 219) with `const auto deadline = deadline_after(timeout);` and `if (std::chrono::steady_clock::now() >= deadline) return {EngineReadKind::timeout, {}, {}};` (line 234) with `if (remaining_until(deadline).count() == 0) return {EngineReadKind::timeout, {}, {}};`. In the POSIX branch replace lines 252-256 (deadline, `bytes`, loop head, `remaining`, `bounded`) with:

```cpp
  const auto deadline = deadline_after(timeout);
  std::array<char, 1024> bytes{};
  for (;;) {
    const auto remaining = remaining_until(deadline);
    const auto bounded = std::clamp<std::int64_t>(remaining.count(), 0, 0x7fffffff);
```

- [ ] **Step 4: K3 and K5**

In `secure_frontend/include/secure_frontend/engine_stdio_process.hpp`, replace line 31 with:

```cpp
  // Sends SIGTERM to the child's process group (Windows: terminates the Job),
  // waits up to `grace`, then SIGKILLs the group and reaps the leader for at
  // most 500 ms. Returns true once the leader is reaped (or was never
  // started). Returns false ONLY if the leader is still unreaped after that
  // bound: the object is then "abandoned" — do not start() it again; the
  // supervisor applies spec V10 (no respawn until reaped; past reap_timeout
  // the circuit opens).
  [[nodiscard]] bool terminate(std::chrono::milliseconds grace);
```

In `secure_frontend/tests/support/test_paths.hpp` add `#include <iostream>` and `#include <vector>` and, after `server_path()`:

```cpp
// This test binary's own path. Fails the test loudly if the OS call fails or
// truncates (K5), instead of returning a garbage path.
inline std::filesystem::path current_executable() {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    std::cerr << "FATAL: GetModuleFileNameW failed or truncated the path\n";
    std::exit(EXIT_FAILURE);
  }
  return std::filesystem::path(std::wstring(buffer.data(), length));
#else
  std::vector<char> buffer(4096);
  const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
  if (length <= 0 || static_cast<std::size_t>(length) >= buffer.size()) {
    std::cerr << "FATAL: readlink(/proc/self/exe) failed or truncated the path\n";
    std::exit(EXIT_FAILURE);
  }
  return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
#endif
}
```

In `secure_frontend/tests/engine_process_test.cpp` delete the local `current_executable()` (lines 28-38), add `#include "support/test_paths.hpp"` and `using test_support::current_executable;` inside the anonymous namespace (SP1's `main` keeps calling `current_executable()` unchanged). In CMake add `secure_frontend_test_paths(secure_frontend_engine_process_tests)` directly after SP1's `secure_frontend_add_cases(secure_frontend_engine_process_tests ...)` call.

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc 2>&1 | tail -3`
Expected: `100% tests passed, 0 tests failed out of <Task 3 total> + 1` (new: `secure_frontend_deadline_tests.deadline_zero_timeout_is_immediate_everywhere`; the SP0 protocol and chat cases still pass: none relies on a zero budget).

- [ ] **Step 6: Cross-compile and run wine** — `cmake --build --preset windows-llvm-mingw-cross 2>&1 | tail -2 && ctest --preset windows-llvm-mingw-cross 2>&1 | tail -3`. Expected: compiles; `100% tests passed, 0 tests failed out of <Task 3 wine total> + 1` (the new deadline test is portable and unlabeled). Record the line.

- [ ] **Step 7: Handoff** — add the row and delete the three resolved "Known small gaps" bullets named under **Files**:

```markdown
| K1, K3, K5 | `deadline.hpp`: one `deadline_after`/`remaining_until` rule in `await_ready`, `read_generation`, `read_line`; a zero budget is an immediate timeout everywhere. `terminate` contract documented (false only when still unreaped = abandoned). `current_executable()` checks `readlink`/`GetModuleFileNameW`. | `deadline_zero_timeout_is_immediate_everywhere` | <recorded linux line>; wine <recorded> |
```

- [ ] **Step 8: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "engine I/O: one deadline rule for READY, generation and line reads; terminate contract (K1, K3, K5)" \
  -m "K1: deadline_zero_timeout_is_immediate_everywhere. K3: terminate() returns false only when the leader is still unreaped. K5: current_executable() fails loudly on readlink/GetModuleFileNameW errors." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 5: Exit-status capture and classification (V1)

**Files:**
- Create: `secure_frontend/include/secure_frontend/exit_status.hpp`
- Create: `secure_frontend/src/exit_status.cpp`
- Create: `secure_frontend/tests/engine_child_test.cpp`
- Modify: `secure_frontend/include/secure_frontend/engine_stdio_process.hpp` (`try_reap`, `exit_status`, member `exit_status_`)
- Modify: `secure_frontend/src/engine_stdio_process.cpp:93` (`start` resets the status), `:175-190` (`running` via `try_reap`), `:284` (Windows `terminate`: kill code and `try_reap`) and `:155` (Windows kill code)
- Modify: `secure_frontend/src/engine_process.cpp:197` and `:283` (Windows kill code)
- Modify: `secure_frontend/CMakeLists.txt:62-64` (add `src/exit_status.cpp` to `secure_frontend_engine_stdio_process`), new test binary
- Modify: `docs/AGENT_HANDOFF.md` (V1 row)

**Interfaces:**
- Produces: `ExitStatus`, `KillReason`, `ExitKind`, `ExitPlatform`, `kNativeExitPlatform`, `ExitClassification`, `kSupervisorKillExitCode` (`0x20000001`), `classify_exit(status, sent, platform)`, `exit_kind_literal(ExitKind)`, `kill_reason_literal(KillReason)`, `describe_exit(ExitClassification)`; `EngineStdioProcess::try_reap()` → `std::optional<ExitStatus>`, `EngineStdioProcess::exit_status()`.
- Classification rule (spec §3.6): a kill counts as ours only when the observed end matches what we send (POSIX SIGKILL or SIGTERM; Windows exit code `0x20000001`) and a reason was recorded; otherwise the observed status decides, so an engine that aborted before our kill landed is still `abort`.

- [ ] **Step 1: Write the failing test** — `secure_frontend/tests/engine_child_test.cpp`:

```cpp
#include "secure_frontend/engine_stdio_process.hpp"
#include "secure_frontend/exit_status.hpp"

#include "support/test_paths.hpp"

#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using secure_frontend::ExitKind;
using secure_frontend::ExitPlatform;
using secure_frontend::ExitStatus;
using secure_frontend::KillReason;

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

std::optional<ExitStatus> wait_reaped(secure_frontend::EngineStdioProcess& child, const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (auto status = child.try_reap()) return status;
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(10ms);
  }
}

void proc04_exit_status_is_classified() {
  struct Row {
    ExitStatus status;
    KillReason sent;
    ExitPlatform platform;
    ExitKind kind;
    KillReason reason;
    const char* name;
  };
  const std::vector<Row> table{
      {{false, 0U}, KillReason::none, ExitPlatform::posix, ExitKind::clean_exit, KillReason::none, "posix exit 0"},
      {{false, 1U}, KillReason::none, ExitPlatform::posix, ExitKind::error_exit, KillReason::none, "posix exit 1 (ERR path or CUDA fault)"},
      {{false, 127U}, KillReason::none, ExitPlatform::posix, ExitKind::error_exit, KillReason::none, "posix exit 127 after a successful exec"},
      {{true, 6U}, KillReason::none, ExitPlatform::posix, ExitKind::abort, KillReason::none, "posix SIGABRT (watchdog)"},
      {{true, 9U}, KillReason::none, ExitPlatform::posix, ExitKind::killed_external, KillReason::none, "posix SIGKILL not ours (OOM)"},
      {{true, 11U}, KillReason::none, ExitPlatform::posix, ExitKind::crashed, KillReason::none, "posix SIGSEGV"},
      {{true, 9U}, KillReason::fatal_line, ExitPlatform::posix, ExitKind::killed_by_supervisor, KillReason::fatal_line, "posix our SIGKILL"},
      {{true, 15U}, KillReason::shutdown, ExitPlatform::posix, ExitKind::killed_by_supervisor, KillReason::shutdown, "posix our SIGTERM"},
      {{true, 6U}, KillReason::protocol_failed, ExitPlatform::posix, ExitKind::abort, KillReason::none, "aborted before our kill landed"},
      {{false, 0U}, KillReason::none, ExitPlatform::windows, ExitKind::clean_exit, KillReason::none, "windows exit 0"},
      {{false, 1U}, KillReason::none, ExitPlatform::windows, ExitKind::error_exit, KillReason::none, "windows exit 1"},
      {{false, 3U}, KillReason::none, ExitPlatform::windows, ExitKind::abort, KillReason::none, "windows abort() exit 3"},
      {{false, 0xC0000409U}, KillReason::none, ExitPlatform::windows, ExitKind::abort, KillReason::none, "windows fast-fail abort"},
      {{false, 0xC0000005U}, KillReason::none, ExitPlatform::windows, ExitKind::crashed, KillReason::none, "windows access violation"},
      {{false, 0x20000001U}, KillReason::ready_timeout, ExitPlatform::windows, ExitKind::killed_by_supervisor, KillReason::ready_timeout,
       "windows our TerminateJobObject code"},
  };
  for (const auto& row : table) {
    const auto got = secure_frontend::classify_exit(row.status, row.sent, row.platform);
    expect(got.kind == row.kind && got.reason == row.reason, std::string("classification: ") + row.name);
  }
  expect(secure_frontend::describe_exit({ExitKind::killed_by_supervisor, KillReason::fatal_line}) == "killed_by_supervisor(fatal_line)",
         "describe a supervisor kill");
  expect(secure_frontend::describe_exit({ExitKind::abort, KillReason::none}) == "abort", "describe an abort");

  // Real children: the raw status is captured by try_reap() and classified natively.
  struct Live {
    std::string argument;
    ExitKind kind;
  };
  std::vector<Live> live{{"--exit-before-ready=1", ExitKind::error_exit}, {"--crash-after-ready=ABRT", ExitKind::abort},
                         {"--crash-after-ready=SEGV", ExitKind::crashed}};
#if !defined(_WIN32)
  live.push_back({"--crash-after-ready=KILL", ExitKind::killed_external});
#endif
  for (const auto& run : live) {
    secure_frontend::EngineStdioProcess child;
    expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {run.argument}, .environment = {}}).has_value(),
           "start " + run.argument);
    const auto status = wait_reaped(child, 10s);
    expect(status.has_value(), "the child must end: " + run.argument);
    if (status) {
      expect(secure_frontend::classify_exit(*status, KillReason::none).kind == run.kind, "live classification: " + run.argument);
      expect(child.exit_status() == status, "exit_status() keeps the reaped status");
    }
  }
  // Our own kill is ours, never confused with the engine's `return 1`.
  secure_frontend::EngineStdioProcess killed;
  expect(!killed.start({.executable = test_support::fake_engine_path(), .arguments = {"--hang-before-ready"}, .environment = {}}).has_value(),
         "start hang");
  expect(killed.terminate(100ms), "terminate");
  const auto status = killed.exit_status();
  expect(status && secure_frontend::classify_exit(*status, KillReason::shutdown).kind == ExitKind::killed_by_supervisor,
         "a supervisor kill classifies as ours");
}

}  // namespace

int main(int argc, char* argv[]) {
  return secure_frontend_test::run_cases(argc, argv, {{"proc04_exit_status_is_classified", proc04_exit_status_is_classified}}, failures);
}
```

CMake (tests block) — later tasks append case names to the three `set()` lines only (Windows-only cases to `_WINDOWS_CASES`, Linux-only to `_LINUX_CASES`):

```cmake
    secure_frontend_add_test_executable(secure_frontend_engine_child_tests tests/engine_child_test.cpp
        secure_frontend_engine_stdio_process secure_frontend_test_support)
    secure_frontend_test_paths(secure_frontend_engine_child_tests)
    set(SECURE_FRONTEND_CHILD_CASES proc04_exit_status_is_classified)
    set(SECURE_FRONTEND_CHILD_LINUX_CASES)
    set(SECURE_FRONTEND_CHILD_WINDOWS_CASES)
    if(WIN32)
        list(APPEND SECURE_FRONTEND_CHILD_CASES ${SECURE_FRONTEND_CHILD_WINDOWS_CASES})
    else()
        list(APPEND SECURE_FRONTEND_CHILD_CASES ${SECURE_FRONTEND_CHILD_LINUX_CASES})
    endif()
    secure_frontend_add_cases(secure_frontend_engine_child_tests ${SECURE_FRONTEND_CHILD_CASES}
        TIMEOUT 120 LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: FAIL — `secure_frontend/exit_status.hpp: No such file or directory`.

- [ ] **Step 3: Write `exit_status.hpp`**

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace secure_frontend {

// Raw end of an engine child, kept instead of discarded (spec §3.6).
// POSIX: `signaled` = WIFSIGNALED, `value` = WTERMSIG or WEXITSTATUS.
// Windows: `signaled` is false and `value` is GetExitCodeProcess.
struct ExitStatus final {
  bool signaled{false};
  std::uint32_t value{0};
  friend bool operator==(const ExitStatus&, const ExitStatus&) = default;
};

// Why the supervisor ended a generation (spec §3.5); `none` when it did not.
enum class KillReason { none, fatal_line, ready_timeout, drain_failed, write_timeout, protocol_failed, shutdown };

// Diagnostic only: every kind counts as a failure, and the literal appears only
// in the frontend's own log line, never in a client response.
enum class ExitKind { clean_exit, error_exit, spawn_failed, abort, killed_external, killed_by_supervisor, crashed };

enum class ExitPlatform { posix, windows };
#if defined(_WIN32)
inline constexpr ExitPlatform kNativeExitPlatform = ExitPlatform::windows;
#else
inline constexpr ExitPlatform kNativeExitPlatform = ExitPlatform::posix;
#endif

struct ExitClassification final {
  ExitKind kind;
  KillReason reason;
};

// TerminateJobObject exit code: a customer code (bit 29), never an engine `return 1`.
inline constexpr std::uint32_t kSupervisorKillExitCode = 0x20000001U;

// `sent_by_supervisor` is the reason recorded when the supervisor sent a kill
// (KillReason::none otherwise). The kill is credited to the supervisor only if
// the observed end is what it sends (POSIX SIGKILL/SIGTERM, Windows
// kSupervisorKillExitCode); otherwise the status decides.
[[nodiscard]] ExitClassification classify_exit(const ExitStatus& status, KillReason sent_by_supervisor,
                                               ExitPlatform platform = kNativeExitPlatform) noexcept;
[[nodiscard]] std::string_view exit_kind_literal(ExitKind kind) noexcept;
[[nodiscard]] std::string_view kill_reason_literal(KillReason reason) noexcept;
// "abort", or "killed_by_supervisor(fatal_line)".
[[nodiscard]] std::string describe_exit(const ExitClassification& classification);

}  // namespace secure_frontend
```

`secure_frontend/src/exit_status.cpp`:

```cpp
#include "secure_frontend/exit_status.hpp"

#if !defined(_WIN32)
#include <csignal>
#endif

namespace secure_frontend {
namespace {
// Linux signal numbers, spelled out so the POSIX table is testable on every host.
constexpr std::uint32_t kSigAbrt = 6U;
constexpr std::uint32_t kSigKill = 9U;
constexpr std::uint32_t kSigTerm = 15U;
#if defined(__linux__)
static_assert(SIGABRT == 6 && SIGKILL == 9 && SIGTERM == 15, "Linux signal numbers");
#endif
}  // namespace

ExitClassification classify_exit(const ExitStatus& status, const KillReason sent, const ExitPlatform platform) noexcept {
  if (platform == ExitPlatform::windows) {
    if (sent != KillReason::none && status.value == kSupervisorKillExitCode) return {ExitKind::killed_by_supervisor, sent};
    if (status.value == 0U) return {ExitKind::clean_exit, KillReason::none};
    if (status.value == 3U || status.value == 0xC0000409U) return {ExitKind::abort, KillReason::none};
    if ((status.value & 0xC0000000U) == 0xC0000000U) return {ExitKind::crashed, KillReason::none};
    return {ExitKind::error_exit, KillReason::none};
  }
  if (status.signaled) {
    if (sent != KillReason::none && (status.value == kSigKill || status.value == kSigTerm)) return {ExitKind::killed_by_supervisor, sent};
    if (status.value == kSigAbrt) return {ExitKind::abort, KillReason::none};
    if (status.value == kSigKill) return {ExitKind::killed_external, KillReason::none};
    return {ExitKind::crashed, KillReason::none};
  }
  if (status.value == 0U) return {ExitKind::clean_exit, KillReason::none};
  return {ExitKind::error_exit, KillReason::none};
}

std::string_view exit_kind_literal(const ExitKind kind) noexcept {
  switch (kind) {
    case ExitKind::clean_exit: return "clean_exit";
    case ExitKind::error_exit: return "error_exit";
    case ExitKind::spawn_failed: return "spawn_failed";
    case ExitKind::abort: return "abort";
    case ExitKind::killed_external: return "killed_external";
    case ExitKind::killed_by_supervisor: return "killed_by_supervisor";
    case ExitKind::crashed: return "crashed";
  }
  return "crashed";
}

std::string_view kill_reason_literal(const KillReason reason) noexcept {
  switch (reason) {
    case KillReason::none: return "none";
    case KillReason::fatal_line: return "fatal_line";
    case KillReason::ready_timeout: return "ready_timeout";
    case KillReason::drain_failed: return "drain_failed";
    case KillReason::write_timeout: return "write_timeout";
    case KillReason::protocol_failed: return "protocol_failed";
    case KillReason::shutdown: return "shutdown";
  }
  return "none";
}

std::string describe_exit(const ExitClassification& classification) {
  std::string text(exit_kind_literal(classification.kind));
  if (classification.kind == ExitKind::killed_by_supervisor) {
    text += '(';
    text += kill_reason_literal(classification.reason);
    text += ')';
  }
  return text;
}

}  // namespace secure_frontend
```

- [ ] **Step 4: Capture the status in `EngineStdioProcess`**

Header: add `#include "secure_frontend/exit_status.hpp"`; in the public section after `terminate`:

```cpp
  // Non-blocking: reaps the child once it has ended and keeps its raw status
  // (POSIX waitpid status; Windows GetExitCodeProcess). Later calls return the
  // same status. Lifecycle call: only the owning (supervisor) thread uses it.
  [[nodiscard]] std::optional<ExitStatus> try_reap();
  [[nodiscard]] std::optional<ExitStatus> exit_status() const noexcept { return exit_status_; }
```

and the private member `std::optional<ExitStatus> exit_status_;` after `pending_stdout_`.

`secure_frontend/src/engine_stdio_process.cpp`: in `start` replace line 93 (`cleanup(); pending_stdout_.clear();`) with `cleanup(); pending_stdout_.clear(); exit_status_.reset();`. Replace `running()` (lines 175-190) with:

```cpp
std::optional<ExitStatus> EngineStdioProcess::try_reap() {
  if (exit_status_) return exit_status_;
#if defined(_WIN32)
  if (process_handle_ == nullptr || WaitForSingleObject(static_cast<HANDLE>(process_handle_), 0) != WAIT_OBJECT_0) return std::nullopt;
  DWORD code = 0;
  if (!GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code)) code = 0xFFFFFFFFU;
  exit_status_ = ExitStatus{false, static_cast<std::uint32_t>(code)};
  return exit_status_;
#else
  if (process_id_ < 0) return std::nullopt;
  int status = 0;
  const pid_t value = waitpid(process_id_, &status, WNOHANG);
  if (value == process_id_) {
    exit_status_ = WIFSIGNALED(status) ? ExitStatus{true, static_cast<std::uint32_t>(WTERMSIG(status))}
                                       : ExitStatus{false, static_cast<std::uint32_t>(WIFEXITED(status) ? WEXITSTATUS(status) : 255)};
  } else if (value < 0 && errno == ECHILD) {
    // Reaped by someone else: the status is unknown. Cannot happen while only the supervisor reaps.
    exit_status_ = ExitStatus{false, 255U};
  } else {
    return std::nullopt;
  }
  process_id_ = -1;
  return exit_status_;
#endif
}

bool EngineStdioProcess::running() {
#if defined(_WIN32)
  return process_handle_ != nullptr && !try_reap().has_value();
#else
  return process_id_ >= 0 && !try_reap().has_value();
#endif
}
```

Windows kill code and status (V1 on Windows): `exit_status.hpp` is already included through the header. In the `ResumeThread` failure branch of `start` (line 155) replace `TerminateJobObject(job, 1);` with `TerminateJobObject(job, kSupervisorKillExitCode);`. Replace the Windows branch of `terminate` (line 284, one line today) with:

```cpp
  if (!process_handle_) return true;
  if (!TerminateJobObject(static_cast<HANDLE>(job_handle_), kSupervisorKillExitCode)) return false;
  if (WaitForSingleObject(static_cast<HANDLE>(process_handle_), static_cast<DWORD>(std::max<std::int64_t>(0, grace.count()))) !=
      WAIT_OBJECT_0) {
    return false;
  }
  // Keep the status of our own kill, so exit_status() is set and classify_exit()
  // credits it to the supervisor ("a supervisor kill classifies as ours").
  (void)try_reap();
  return true;
```

`secure_frontend/src/engine_process.cpp` (the SP0 twin, no `try_reap`; its `wait_for` already records the code): add `#include "secure_frontend/exit_status.hpp"` after `#include "secure_frontend/engine_process.hpp"`. In `start`, the `ResumeThread` failure branch (line 197) becomes

```cpp
  if (ResumeThread(info.hThread) == static_cast<DWORD>(-1)) {
    TerminateJobObject(job, kSupervisorKillExitCode);
```

and in `terminate` line 283 becomes

```cpp
  if (TerminateJobObject(static_cast<HANDLE>(job_handle_), kSupervisorKillExitCode) == 0) return false;
```

In `secure_frontend/CMakeLists.txt` change lines 62-64 to:

```cmake
add_library(secure_frontend_engine_stdio_process
    src/engine_stdio_process.cpp
    src/exit_status.cpp
)
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc 2>&1 | tail -3`
Expected: `100% tests passed, 0 tests failed out of <Task 4 total> + 1` (new: `secure_frontend_engine_child_tests.proc04_exit_status_is_classified`).

- [ ] **Step 6: Cross-compile and record wine** — compiles with `-Werror`; `WINEPREFIX=$PWD/out/wineprefix wine out/build/windows-llvm-mingw-cross/secure_frontend_engine_child_tests.exe --case proc04_exit_status_is_classified; echo "exit $?"` — record (wine's crash exit codes are not authoritative). `ctest --preset windows-llvm-mingw-cross` stays at the Task 4 wine total (the case is labeled). On Windows the last check ("a supervisor kill classifies as ours") depends on `terminate()` calling `try_reap()`; it is authoritative only in MSYS2.

- [ ] **Step 7: Handoff row**

```markdown
| V1 | `EngineStdioProcess::try_reap()` keeps the raw `waitpid` / `GetExitCodeProcess` status; `classify_exit` maps it to `clean_exit`/`error_exit`/`spawn_failed`/`abort`/`killed_external`/`killed_by_supervisor(reason)`/`crashed` (log only); Windows Job kill code `0x20000001`, and Windows `terminate()` records it through `try_reap()`. | `proc04_exit_status_is_classified` (B) | <recorded linux line>; wine: <recorded>; Windows authoritative in MSYS2 |
```

- [ ] **Step 8: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "engine process: keep and classify the child's exit status; Windows kill code 0x20000001 (V1)" \
  -m "V1: proc04_exit_status_is_classified. try_reap() replaces the status-discarding running() path." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 6a: `EngineSupervisor` core, unit tests and `adopt_ready` — one supervisor thread, eager READY, leases, `recovery` literal (V2, V11 core)

This task lands the supervisor library, its unit tests, `ChatExecutionService::adopt_ready`, `EngineProtocolClient::failure_code` and the router's `recovery` literal. It touches no `secure_server_main.cpp`, so it runs in parallel with SP1; the SP0 server keeps its start-once engine until Task 6b wires the supervisor in (after the SP1 merge). Restart policy (Task 7), the monitor (Task 8) and the kill triggers (Tasks 9–10) build on it; until Task 7 every unrequested end opens the circuit.

**Files:**
- Create: `secure_frontend/include/secure_frontend/supervision_defaults.hpp`
- Create: `secure_frontend/include/secure_frontend/restart_policy.hpp` (the `RestartPolicy` struct)
- Create: `secure_frontend/include/secure_frontend/supervisor_wake.hpp`, `secure_frontend/src/supervisor_wake.cpp`
- Create: `secure_frontend/include/secure_frontend/supervisor_clock.hpp`
- Create: `secure_frontend/include/secure_frontend/supervised_process.hpp`
- Create: `secure_frontend/include/secure_frontend/engine_supervisor.hpp`, `secure_frontend/src/engine_supervisor.cpp`
- Create: `secure_frontend/tests/engine_supervisor_test.cpp`
- Modify: `secure_frontend/include/secure_frontend/engine_stdio_process.hpp` (implements `SupervisedProcess`), `secure_frontend/src/engine_stdio_process.cpp` (`start` records `process_group_`; new lifecycle members)
- Modify: `secure_frontend/include/secure_frontend/engine_protocol_client.hpp:110-139`, `secure_frontend/src/engine_protocol_client.cpp:71-75` (`failure_code()`)
- Modify: `secure_frontend/include/secure_frontend/chat_execution.hpp:73-76`, `secure_frontend/src/chat_execution.cpp` (`adopt_ready`)
- Modify: `secure_frontend/include/secure_frontend/api_router.hpp:23-46`, `secure_frontend/src/api_router.cpp:24-38,92-102` (`recovery`; the 4th constructor argument defaults to `{}`, so the SP0 `secure_server_main.cpp` still compiles unchanged)
- Modify: `secure_frontend/tests/api_router_test.cpp:105-116` (SP0 health test updated, not deleted) and new case; `secure_frontend/tests/chat_execution_test.cpp` (adopted READY case)
- Modify: `secure_frontend/CMakeLists.txt` (library `secure_frontend_engine_supervisor` and its warnings; supervisor test binary and its case lists; the two new router/chat cases)
- Modify: `docs/AGENT_HANDOFF.md` (V2/V11 core row)

**Interfaces:**
- Consumes: `ExitStatus`, `KillReason`, `classify_exit`, `describe_exit`, `kSupervisorKillExitCode`, `EngineStdioProcess::try_reap` (Task 5); `deadline_after`, `remaining_until` (Task 4).
- Produces: everything listed for Task 6a in the plan header's Interfaces block: `supervision_defaults::*`, `RestartPolicy`, `kNoMonitorHandle`, `SupervisorWake`, `SupervisorClock`, `SteadySupervisorClock`, `SupervisedProcess`, `SpawnFailure`, `SpawnResult`, `ProcessFactory`, `SupervisorState`, `HealthSnapshot`, `health_for`, `EngineInstance`, `Lease`, `SupervisorConfig`, `SupervisorLog`, `EngineSupervisor`, `kill_reason_for`, `execute_with_lease`, `EngineLaunchPlan{command}`, `make_stdio_process_factory`; `EngineProtocolClient::failure_code()`; `ChatExecutionService::adopt_ready(const EngineReady&)`; `ApiRouter(health, models, engine_state, recovery_state = {})`; `EngineStdioProcess::pending_bytes_for_tests()`. CMake lists `SECURE_FRONTEND_SUPERVISOR_{UNIT,PROCESS,LINUX,WINDOWS}_CASES`.

- [ ] **Step 1: Write the failing tests**

`secure_frontend/tests/api_router_test.cpp` — replace `health_samples_status_before_state_once_each` (lines 105-116) with the SP2 mapping, and add a recovery case (add `{"health_carries_recovery_only_with_failed", health_carries_recovery_only_with_failed},` to SP1's `run_cases` list in `main`):

```cpp
void health_samples_status_before_state_once_each() {
  // SP2 (spec §3.4): READY is consumed eagerly, so `starting` means the model
  // is loading and is reported 503. The providers are called in order b, s, r.
  std::string calls;
  std::string_view snapshot{"failed"};
  const secure_frontend::ApiRouter snapshotting(
      [&] { calls += 'b'; snapshot = "starting"; return false; },
      [] { return std::vector<secure_frontend::ApiModel>{}; },
      [&]() -> std::string_view { calls += 's'; return snapshot; },
      [&]() -> std::string_view { calls += 'r'; return {}; });
  const auto response = snapshotting.dispatch("GET", "/health");
  expect(calls == "bsr", "health must call bool, then state, then recovery, once each; saw: " + calls);
  expect(response.status == 503 && response.body == "{\"status\":\"unavailable\",\"engine\":\"starting\"}\n",
         "a starting engine is 503 with engine=starting");
}

void health_carries_recovery_only_with_failed() {
  const auto with = [](std::string_view state, std::string_view recovery, bool healthy) {
    return secure_frontend::ApiRouter([healthy] { return healthy; }, [] { return std::vector<secure_frontend::ApiModel>{}; },
                                      [state]() -> std::string_view { return state; },
                                      [recovery]() -> std::string_view { return recovery; })
        .dispatch("GET", "/health");
  };
  const auto restarting = with("failed", "restarting", false);
  expect(restarting.status == 503 && restarting.body == "{\"status\":\"unavailable\",\"engine\":\"failed\",\"recovery\":\"restarting\"}\n",
         "failed + restarting");
  expect(with("failed", "exhausted", false).body == "{\"status\":\"unavailable\",\"engine\":\"failed\",\"recovery\":\"exhausted\"}\n",
         "failed + exhausted");
  expect(with("failed", "", false).body == "{\"status\":\"unavailable\",\"engine\":\"failed\"}\n", "no recovery field when empty");
  expect(with("ready", "restarting", true).body == "{\"status\":\"unavailable\"}\n", "recovery with a non-failed state fails closed");
  expect(with("failed", "/secret", false).body == "{\"status\":\"unavailable\"}\n", "an unknown recovery literal fails closed");
}
```

`secure_frontend/tests/engine_supervisor_test.cpp`:

```cpp
#include "secure_frontend/engine_supervisor.hpp"

#include "support/test_paths.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

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
  };
  for (const auto& row : rows) {
    const auto health = secure_frontend::health_for(row.state);
    expect(health.state == row.state && health.healthy == row.healthy && health.engine == row.engine && health.recovery == row.recovery,
           "health literals for state " + std::to_string(static_cast<int>(row.state)));
  }
  secure_frontend::EngineSupervisor idle({}, [](std::uint64_t) -> secure_frontend::SpawnResult { return secure_frontend::SpawnFailure{"unused"}; });
  expect(idle.snapshot().state == SupervisorState::stopped && !idle.acquire().has_value(), "before start(): stopped, no lease");
}

}  // namespace

int main(int argc, char* argv[]) {
  return secure_frontend_test::run_cases(argc, argv, {{"supervisor_health_literals_per_state", supervisor_health_literals_per_state}}, failures);
}
```

`secure_frontend/tests/chat_execution_test.cpp` — add, and add `{"chat_uses_adopted_ready_without_waiting", chat_uses_adopted_ready_without_waiting},` to SP1's `run_cases` list in `main`:

```cpp
void chat_uses_adopted_ready_without_waiting() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt + "OK");
  FakeTransport transport;
  transport.reads = {line("READY 512 stop"), line("T " + std::to_string(token_id_for(tokenizer, "O"))), line("DONE 1")};
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::EngineReady ready{};
  expect(!protocol.await_ready(std::chrono::seconds(1), ready).has_value(), "the supervisor consumed READY");
  secure_frontend::ChatExecutionService service(protocol);
  service.adopt_ready(ready);
  expect(service.health() == secure_frontend::EngineHealth::ready, "an adopted READY is ready before any request");
  const auto result = service.execute(valid_request(), tokenizer);
  expect(std::holds_alternative<secure_frontend::ChatExecutionResult>(result), "execute does not wait for a second READY");
}
```

- [ ] **Step 2: Run to verify the failures**

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m1 error`
Expected: FAIL — `secure_frontend/engine_supervisor.hpp: No such file or directory` (and `ApiRouter` has no 4-argument constructor; `adopt_ready` is not a member). The server-level behavior (SP0's `200 starting`, the lazy READY wait) is unchanged by this task; its RED/GREEN is Task 6b's.

- [ ] **Step 3: Add the supervisor headers and sources**

`secure_frontend/include/secure_frontend/supervision_defaults.hpp`:

```cpp
#pragma once

// SP2's named stand-ins for values SP1 makes typed, validated configuration
// (SP2 spec §2). This is the ONLY header that holds them; SP1 replaces each
// constant with its config field when the branches merge.

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace secure_frontend::supervision_defaults {

// Restart policy (spec §3.3, owner decision 2026-10-13).
inline constexpr std::uint32_t kMaxConsecutiveFailures = 3U;
inline constexpr std::chrono::milliseconds kBackoffInitial{1'000};
inline constexpr std::uint32_t kBackoffFactor = 2U;
inline constexpr std::chrono::milliseconds kBackoffMax{60'000};
inline constexpr std::chrono::milliseconds kHealthyWindow{300'000};
inline constexpr std::chrono::milliseconds kReadyTimeout{900'000};   // upstream STRATA_ENGINE_READY_S
inline constexpr std::chrono::milliseconds kReapTimeout{30'000};

// Supervisor timing.
inline constexpr std::chrono::milliseconds kMonitorFallbackPoll{100};  // kernels without pidfd_open
inline constexpr std::chrono::milliseconds kReadyPollSlice{100};       // READY waits stay responsive to shutdown

// Orderly shutdown (spec §3.7): EOF, wait, SIGTERM, wait, SIGKILL; past the bound exit 1.
inline constexpr std::chrono::milliseconds kShutdownStdinWait{2'000};
inline constexpr std::chrono::milliseconds kShutdownTermWait{1'000};
inline constexpr std::chrono::milliseconds kShutdownBound{5'000};

// Engine stdin writes (spec §3.10): fail when a chunk makes no progress for this long.
inline constexpr std::chrono::milliseconds kStdinWriteDeadline{1'000};
inline constexpr std::size_t kStdinWriteChunk = 65'536U;

// PR_SET_NO_NEW_PRIVS for the engine (owner decision 2026-10-13: default ON;
// --allow-new-privileges turns it off for the GPU-box validation kit).
inline constexpr bool kNoNewPrivilegesDefault = true;

}  // namespace secure_frontend::supervision_defaults
```

`secure_frontend/include/secure_frontend/restart_policy.hpp`:

```cpp
#pragma once

#include "secure_frontend/supervision_defaults.hpp"

#include <chrono>
#include <cstdint>

namespace secure_frontend {

// Spec §3.3 restart policy. Defaults come from supervision_defaults.hpp; SP1
// turns them into typed configuration.
struct RestartPolicy final {
  std::uint32_t max_consecutive_failures{supervision_defaults::kMaxConsecutiveFailures};
  std::chrono::milliseconds backoff_initial{supervision_defaults::kBackoffInitial};
  std::uint32_t backoff_factor{supervision_defaults::kBackoffFactor};
  std::chrono::milliseconds backoff_max{supervision_defaults::kBackoffMax};
  std::chrono::milliseconds healthy_window{supervision_defaults::kHealthyWindow};
  std::chrono::milliseconds ready_timeout{supervision_defaults::kReadyTimeout};
  std::chrono::milliseconds reap_timeout{supervision_defaults::kReapTimeout};
};

}  // namespace secure_frontend
```

`secure_frontend/include/secure_frontend/supervisor_wake.hpp`:

```cpp
#pragma once

#include <chrono>
#include <cstdint>

namespace secure_frontend {

// "Nothing to monitor": no pidfd (POSIX) or no process handle (Windows).
inline constexpr std::intptr_t kNoMonitorHandle = -1;

// The supervisor thread's single wait point: a non-blocking self-pipe (POSIX)
// or a manual-reset event (Windows) fired by report_fatal(), shutdown() and
// the shutdown signal handlers, optionally waited on together with the
// engine's pidfd / process handle.
class SupervisorWake final {
 public:
  enum class Event { woken, monitored, timeout };

  SupervisorWake();
  ~SupervisorWake();
  SupervisorWake(const SupervisorWake&) = delete;
  SupervisorWake& operator=(const SupervisorWake&) = delete;

  // Async-signal-safe on POSIX (a single write(2)); SetEvent on Windows.
  void notify() noexcept;
  // Waits for notify(), for `monitored` to become signalled (pidfd readable /
  // process handle signalled; kNoMonitorHandle = none), or for `timeout`
  // (negative = forever). A signal that interrupts the wait reports `woken`.
  [[nodiscard]] Event wait(std::intptr_t monitored, std::chrono::milliseconds timeout) noexcept;

 private:
  void drain() noexcept;
#if defined(_WIN32)
  void* event_{nullptr};
#else
  int read_fd_{-1};
  int write_fd_{-1};
#endif
};

}  // namespace secure_frontend
```

`secure_frontend/src/supervisor_wake.cpp`:

```cpp
#include "secure_frontend/supervisor_wake.hpp"

#include <algorithm>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace secure_frontend {

SupervisorWake::SupervisorWake() {
#if defined(_WIN32)
  event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#else
  int fds[2]{-1, -1};
  if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
    read_fd_ = fds[0];
    write_fd_ = fds[1];
  }
#endif
}

SupervisorWake::~SupervisorWake() {
#if defined(_WIN32)
  if (event_ != nullptr) CloseHandle(static_cast<HANDLE>(event_));
#else
  if (read_fd_ >= 0) (void)close(read_fd_);
  if (write_fd_ >= 0) (void)close(write_fd_);
#endif
}

void SupervisorWake::notify() noexcept {
#if defined(_WIN32)
  if (event_ != nullptr) (void)SetEvent(static_cast<HANDLE>(event_));
#else
  // EAGAIN on a full pipe is fine: the pipe is already readable.
  if (write_fd_ >= 0) {
    const char byte = 'w';
    (void)!write(write_fd_, &byte, 1);
  }
#endif
}

void SupervisorWake::drain() noexcept {
#if defined(_WIN32)
  if (event_ != nullptr) (void)ResetEvent(static_cast<HANDLE>(event_));
#else
  char buffer[64];
  while (read_fd_ >= 0 && read(read_fd_, buffer, sizeof(buffer)) > 0) {
  }
#endif
}

SupervisorWake::Event SupervisorWake::wait(const std::intptr_t monitored, const std::chrono::milliseconds timeout) noexcept {
#if defined(_WIN32)
  HANDLE handles[2]{static_cast<HANDLE>(event_), reinterpret_cast<HANDLE>(monitored)};
  const DWORD count = monitored == kNoMonitorHandle ? 1U : 2U;
  const DWORD milliseconds = timeout.count() < 0 ? INFINITE : static_cast<DWORD>(std::min<std::int64_t>(timeout.count(), 0xFFFFFFFE));
  const DWORD result = WaitForMultipleObjects(count, handles, FALSE, milliseconds);
  if (result == WAIT_OBJECT_0) {
    drain();
    return Event::woken;
  }
  if (result == WAIT_OBJECT_0 + 1U) return Event::monitored;
  return Event::timeout;
#else
  pollfd fds[2]{{read_fd_, POLLIN, 0}, {static_cast<int>(monitored), POLLIN, 0}};
  const nfds_t count = monitored == kNoMonitorHandle ? 1U : 2U;
  const int milliseconds = timeout.count() < 0 ? -1 : static_cast<int>(std::min<std::int64_t>(timeout.count(), 0x7fffffff));
  const int ready = poll(fds, count, milliseconds);
  if (ready < 0) return errno == EINTR ? Event::woken : Event::timeout;
  if (ready == 0) return Event::timeout;
  if (fds[0].revents != 0) {
    drain();
    return Event::woken;
  }
  return Event::monitored;
#endif
}

}  // namespace secure_frontend
```

`secure_frontend/include/secure_frontend/supervisor_clock.hpp`:

```cpp
#pragma once

#include "secure_frontend/supervisor_wake.hpp"

#include <chrono>

namespace secure_frontend {

// Injectable time source and backoff timer, so PROC-04 policy tests are
// deterministic (spec §4.1 F3). Only the supervisor thread calls it.
class SupervisorClock {
 public:
  using time_point = std::chrono::steady_clock::time_point;
  virtual ~SupervisorClock() = default;
  [[nodiscard]] virtual time_point now() const = 0;
  // Waits out one backoff delay. Returns false when `wake` fired first (the
  // caller then re-checks for shutdown).
  [[nodiscard]] virtual bool sleep_backoff(std::chrono::milliseconds delay, SupervisorWake& wake) = 0;
};

class SteadySupervisorClock final : public SupervisorClock {
 public:
  [[nodiscard]] time_point now() const override { return std::chrono::steady_clock::now(); }
  [[nodiscard]] bool sleep_backoff(const std::chrono::milliseconds delay, SupervisorWake& wake) override {
    return wake.wait(kNoMonitorHandle, delay) == SupervisorWake::Event::timeout;
  }
};

}  // namespace secure_frontend
```

`secure_frontend/include/secure_frontend/supervised_process.hpp`:

```cpp
#pragma once

#include "secure_frontend/engine_protocol_client.hpp"
#include "secure_frontend/exit_status.hpp"
#include "secure_frontend/supervisor_wake.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace secure_frontend {

// What the supervisor needs from one engine child. Thread contract (spec
// §3.1): transport() is used by the serving thread while it holds a Lease;
// every other member is called only by the supervisor thread.
class SupervisedProcess {
 public:
  virtual ~SupervisedProcess() = default;
  [[nodiscard]] virtual EngineLineTransport& transport() noexcept = 0;
  // Non-blocking; reaps the child once it has ended and returns its raw status
  // (the same status on every later call).
  [[nodiscard]] virtual std::optional<ExitStatus> try_reap() = 0;
  // POSIX pidfd or Windows process HANDLE for the monitor; kNoMonitorHandle when none.
  [[nodiscard]] virtual std::intptr_t monitor_handle() const noexcept = 0;
  // Closes the frontend's end of engine stdin (the engine exits 0 on EOF).
  virtual void close_stdin() noexcept = 0;
  // POSIX: SIGTERM to the process group. Windows: no-op (no console signal reaches the engine).
  virtual void signal_group_terminate() noexcept = 0;
  // POSIX: SIGKILL to the process group, even after the leader was reaped
  // (spec §3.7a). Windows: TerminateJobObject(kSupervisorKillExitCode). Never waits.
  virtual void kill_group() noexcept = 0;
  // After the leader is reaped: forget the group id and release per-generation
  // resources (delegated cgroup), waiting at most `bound`.
  virtual void release_resources(std::chrono::milliseconds bound) noexcept = 0;
  // Leader pid for logs and sweeps; -1 when unknown.
  [[nodiscard]] virtual long pid() const noexcept = 0;
};

// A spawn that did not produce a running child. `message` is fixed text, no paths.
struct SpawnFailure final {
  std::string message;
};

using SpawnResult = std::variant<std::unique_ptr<SupervisedProcess>, SpawnFailure>;

// Called only on the supervisor thread: PR_SET_PDEATHSIG fires when the
// forking thread exits, so every engine is forked from that long-lived thread.
using ProcessFactory = std::function<SpawnResult(std::uint64_t generation)>;

}  // namespace secure_frontend
```

`secure_frontend/include/secure_frontend/engine_supervisor.hpp`:

```cpp
#pragma once

#include "secure_frontend/chat_execution.hpp"
#include "secure_frontend/engine_process.hpp"
#include "secure_frontend/engine_protocol_client.hpp"
#include "secure_frontend/exit_status.hpp"
#include "secure_frontend/restart_policy.hpp"
#include "secure_frontend/supervised_process.hpp"
#include "secure_frontend/supervision_defaults.hpp"
#include "secure_frontend/supervisor_clock.hpp"
#include "secure_frontend/supervisor_wake.hpp"
#include "secure_frontend/tokenizer_pack.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace secure_frontend {

// Spec §3.8; the transition table is in the SP2 plan header.
enum class SupervisorState { starting, ready, generating, stopping, backoff, circuit_open, shutting_down, stopped };

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

// The serving thread's borrow of the current generation, taken in `ready`.
// Releasing it returns the supervisor from `generating` to `ready`.
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
  Lease(EngineSupervisor& owner, std::shared_ptr<EngineInstance> instance) noexcept;
  EngineSupervisor* owner_;
  std::shared_ptr<EngineInstance> instance_;
};

struct SupervisorConfig final {
  RestartPolicy policy{};
  ChatExecutionLimits chat_limits{};
  NativeChatProfile profile{NativeChatProfile::qwen};
  EngineProtocolFlavor flavor{EngineProtocolFlavor::strata_0143};
  std::chrono::milliseconds ready_poll_slice{supervision_defaults::kReadyPollSlice};
  std::chrono::milliseconds monitor_poll{supervision_defaults::kMonitorFallbackPoll};
  std::chrono::milliseconds shutdown_stdin_wait{supervision_defaults::kShutdownStdinWait};
  std::chrono::milliseconds shutdown_term_wait{supervision_defaults::kShutdownTermWait};
  // Linux: become the child subreaper and sweep orphaned descendants after every generation (spec §3.7b).
  bool sweep_orphans{true};
  // Linux: log the /dev/nvidia-uvm hint when a generation ends before READY under PR_SET_NO_NEW_PRIVS.
  bool no_new_privileges_hint{false};
};

// Receives fixed-literal lines such as "engine generation 2 ended: abort".
using SupervisorLog = std::function<void(std::string_view line)>;

// Owns every engine generation for the frontend's lifetime (spec §3.1): one
// supervisor thread spawns, consumes READY, watches, kills and reaps; it is
// the only reaper. The serving thread borrows the current generation through
// acquire() and reports an unusable one with report_fatal().
class EngineSupervisor final {
 public:
  EngineSupervisor(SupervisorConfig config, ProcessFactory factory,
                   std::shared_ptr<SupervisorClock> clock = std::make_shared<SteadySupervisorClock>(), SupervisorLog log = {});
  ~EngineSupervisor();
  EngineSupervisor(const EngineSupervisor&) = delete;
  EngineSupervisor& operator=(const EngineSupervisor&) = delete;

  // Starts the supervisor thread, which spawns generation 1. Call once.
  void start();
  // Only in `ready`; anywhere else nullopt (the caller answers a generic 503 at once).
  [[nodiscard]] std::optional<Lease> acquire();
  // The leased generation is unusable (fatal line, drain failure, write timeout,
  // failed protocol): kill it, count a failure, restart per policy.
  void report_fatal(const Lease& lease, KillReason reason);
  // One sample per /health request (SP0's sample-once rule).
  [[nodiscard]] HealthSnapshot snapshot() const;
  // Requests the orderly shutdown and waits for it; past `bound` the process exits 1.
  void shutdown(std::chrono::milliseconds bound);
  [[nodiscard]] bool stop_requested() const noexcept;

  // Test seams.
  [[nodiscard]] bool wait_for_state(SupervisorState state, std::chrono::milliseconds timeout) const;
  [[nodiscard]] std::uint64_t generations_started() const noexcept { return generations_started_.load(); }

 private:
  friend class Lease;
  void release(const EngineInstance& instance) noexcept;
  void run();
  std::shared_ptr<EngineInstance> spawn_generation();
  KillReason supervise(EngineInstance& instance, std::chrono::milliseconds& healthy_for);
  std::optional<KillReason> await_ready(EngineInstance& instance, EngineReady& ready);
  KillReason watch(EngineInstance& instance);
  bool end_generation(const std::shared_ptr<EngineInstance>& instance, KillReason reason);
  bool schedule_restart(std::chrono::milliseconds healthy_for, bool reaped);
  void wait_in_circuit_open();
  void shutdown_generation(const std::shared_ptr<EngineInstance>& instance);
  void finish();
  void set_state(SupervisorState state);
  void set_state(SupervisorState state, std::shared_ptr<EngineInstance> current);
  void log_end(std::uint64_t generation, std::string_view kind);

  SupervisorConfig config_;
  ProcessFactory factory_;
  std::shared_ptr<SupervisorClock> clock_;
  SupervisorLog log_;
  SupervisorWake wake_;
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  SupervisorState state_{SupervisorState::stopped};
  std::shared_ptr<EngineInstance> current_;
  KillReason pending_kill_{KillReason::none};
  std::vector<std::shared_ptr<EngineInstance>> abandoned_;
  std::atomic<std::uint64_t> generations_started_{0};
  std::atomic<bool> shutdown_requested_{false};
  std::thread thread_;
};

// Maps the protocol's terminal failure to the §3.5 kill reason.
[[nodiscard]] KillReason kill_reason_for(const EngineProtocolClient& protocol) noexcept;

// The serving thread's one call per chat request: borrows the current
// generation, runs the request, and reports a generation left unusable.
// nullopt = no generation is ready (answer a generic 503 at once).
[[nodiscard]] std::optional<ChatExecutionOutcome> execute_with_lease(EngineSupervisor& supervisor, const ChatCompletionRequest& request,
                                                                     const TokenizerPack& tokenizer);

// What the production factory launches for every generation.
struct EngineLaunchPlan final {
  EngineCommand command;
};

// Production factory: an EngineStdioProcess started from `plan` per generation.
[[nodiscard]] ProcessFactory make_stdio_process_factory(EngineLaunchPlan plan);

}  // namespace secure_frontend
```

`secure_frontend/src/engine_supervisor.cpp`:

```cpp
#include "secure_frontend/engine_supervisor.hpp"

#include "secure_frontend/deadline.hpp"
#include "secure_frontend/engine_stdio_process.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

constexpr auto kReapPoll = std::chrono::milliseconds(10);

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

ProcessFactory make_stdio_process_factory(EngineLaunchPlan plan) {
  return [plan = std::move(plan)](const std::uint64_t) -> SpawnResult {
    auto process = std::make_unique<EngineStdioProcess>();
    if (const auto error = process->start(plan.command); error.has_value()) return SpawnFailure{error->message};
    return std::unique_ptr<SupervisedProcess>(std::move(process));
  };
}

Lease::Lease(EngineSupervisor& owner, std::shared_ptr<EngineInstance> instance) noexcept
    : owner_(&owner), instance_(std::move(instance)) {}
Lease::Lease(Lease&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)), instance_(std::move(other.instance_)) {}
Lease::~Lease() {
  if (owner_ != nullptr && instance_) owner_->release(*instance_);
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
      log_(log ? std::move(log) : SupervisorLog(log_to_stderr)) {}

EngineSupervisor::~EngineSupervisor() {
  if (thread_.joinable()) shutdown(supervision_defaults::kShutdownBound);
}

void EngineSupervisor::start() {
  {
    std::lock_guard lock(mutex_);
    if (thread_.joinable()) return;
    state_ = SupervisorState::starting;
    changed_.notify_all();
  }
  thread_ = std::thread([this] { run(); });
}

std::optional<Lease> EngineSupervisor::acquire() {
  std::lock_guard lock(mutex_);
  if (state_ != SupervisorState::ready || !current_) return std::nullopt;
  state_ = SupervisorState::generating;
  changed_.notify_all();
  return Lease(*this, current_);
}

void EngineSupervisor::release(const EngineInstance& instance) noexcept {
  std::lock_guard lock(mutex_);
  if (state_ == SupervisorState::generating && current_.get() == &instance) {
    state_ = SupervisorState::ready;
    changed_.notify_all();
  }
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

bool EngineSupervisor::stop_requested() const noexcept { return shutdown_requested_.load(); }

bool EngineSupervisor::wait_for_state(const SupervisorState state, const std::chrono::milliseconds timeout) const {
  std::unique_lock lock(mutex_);
  return changed_.wait_for(lock, timeout, [this, state] { return state_ == state; });
}

void EngineSupervisor::shutdown(const std::chrono::milliseconds bound) {
  shutdown_requested_.store(true);
  wake_.notify();
  if (!thread_.joinable()) return;
  {
    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock, bound, [this] { return state_ == SupervisorState::stopped; })) {
      // Spec §3.7: an orderly shutdown that cannot finish within the bound exits 1.
      std::_Exit(EXIT_FAILURE);
    }
  }
  thread_.join();
}

void EngineSupervisor::run() {
  for (;;) {
    if (stop_requested()) break;
    auto instance = spawn_generation();
    std::chrono::milliseconds healthy_for{0};
    bool reaped = true;
    if (instance) {
      const KillReason reason = supervise(*instance, healthy_for);
      if (reason == KillReason::shutdown) {
        shutdown_generation(instance);
        break;
      }
      reaped = end_generation(instance, reason);
    }
    if (stop_requested()) break;
    if (!schedule_restart(healthy_for, reaped)) {
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
  return instance;
}

KillReason EngineSupervisor::supervise(EngineInstance& instance, std::chrono::milliseconds& healthy_for) {
  EngineReady ready{};
  if (const auto reason = await_ready(instance, ready); reason.has_value()) return *reason;
  instance.chat = std::make_unique<ChatExecutionService>(*instance.protocol, config_.chat_limits, config_.profile);
  instance.chat->adopt_ready(ready);
  const auto ready_at = clock_->now();
  set_state(SupervisorState::ready);
  const KillReason reason = watch(instance);
  healthy_for = std::chrono::duration_cast<std::chrono::milliseconds>(clock_->now() - ready_at);
  return reason;
}

// Eager READY (spec §3.4): consumed on this thread right after spawn, in short
// slices so shutdown and an early exit are seen promptly.
std::optional<KillReason> EngineSupervisor::await_ready(EngineInstance& instance, EngineReady& ready) {
  for (;;) {
    if (stop_requested()) return KillReason::shutdown;
    const auto error = instance.protocol->await_ready(config_.ready_poll_slice, ready);
    if (!error) return std::nullopt;
    if (error->code == EngineProtocolErrorCode::closed) return KillReason::none;  // it ended; its exit status says why
    if (error->code != EngineProtocolErrorCode::timeout) return KillReason::protocol_failed;
    if (instance.process->try_reap()) return KillReason::none;
  }
}

KillReason EngineSupervisor::watch(EngineInstance& instance) {
  for (;;) {
    {
      std::lock_guard lock(mutex_);
      if (pending_kill_ != KillReason::none) return std::exchange(pending_kill_, KillReason::none);
    }
    if (stop_requested()) return KillReason::shutdown;
    if (instance.process->try_reap()) return KillReason::none;
    (void)wake_.wait(kNoMonitorHandle, std::chrono::milliseconds(-1));
  }
}

// `stopping` (spec §3.8): kill the whole group/Job, reap within reap_timeout,
// classify and log. Returns false when the child could not be reaped.
bool EngineSupervisor::end_generation(const std::shared_ptr<EngineInstance>& instance, const KillReason reason) {
  set_state(SupervisorState::stopping);
  SupervisedProcess& process = *instance->process;
  const bool ended_on_its_own = process.try_reap().has_value();
  process.kill_group();  // spec §3.7a: always, even after the leader has ended
  const auto deadline = deadline_after(config_.policy.reap_timeout);
  std::optional<ExitStatus> status = process.try_reap();
  while (!status && remaining_until(deadline).count() > 0) {
    std::this_thread::sleep_for(kReapPoll);
    status = process.try_reap();
  }
  {
    std::lock_guard lock(mutex_);
    current_.reset();
    pending_kill_ = KillReason::none;
  }
  if (!status) {
    log_end(instance->number, "unreaped");
    std::lock_guard lock(mutex_);
    abandoned_.push_back(instance);  // kept alive: destroying it would wait for the child again
    return false;
  }
  process.release_resources(std::chrono::milliseconds(1000));
  const KillReason sent = ended_on_its_own ? KillReason::none : (reason == KillReason::none ? KillReason::protocol_failed : reason);
  log_end(instance->number, describe_exit(classify_exit(*status, sent)));
  return true;
}

// No restart policy yet: every unrequested end opens the circuit.
bool EngineSupervisor::schedule_restart(const std::chrono::milliseconds healthy_for, const bool reaped) {
  (void)healthy_for;
  (void)reaped;
  set_state(SupervisorState::circuit_open, nullptr);
  return false;
}

void EngineSupervisor::wait_in_circuit_open() {
  while (!stop_requested()) (void)wake_.wait(kNoMonitorHandle, std::chrono::milliseconds(-1));
}

void EngineSupervisor::shutdown_generation(const std::shared_ptr<EngineInstance>& instance) {
  set_state(SupervisorState::shutting_down);
  instance->process->kill_group();
  const auto deadline = deadline_after(config_.shutdown_term_wait);
  while (!instance->process->try_reap() && remaining_until(deadline).count() > 0) std::this_thread::sleep_for(kReapPoll);
  if (instance->process->try_reap()) instance->process->release_resources(std::chrono::milliseconds(0));
  std::lock_guard lock(mutex_);
  current_.reset();
}

void EngineSupervisor::finish() { set_state(SupervisorState::stopped, nullptr); }

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
```

- [ ] **Step 4: `EngineStdioProcess` becomes a `SupervisedProcess`; protocol, chat and router changes**

`secure_frontend/include/secure_frontend/engine_stdio_process.hpp`: include `"secure_frontend/supervised_process.hpp"`; change the class head to `class EngineStdioProcess final : public EngineLineTransport, public SupervisedProcess {`; mark `try_reap()` `override`; add to the public section:

```cpp
  // SupervisedProcess (spec §3.1): lifecycle calls from the supervisor thread only.
  [[nodiscard]] EngineLineTransport& transport() noexcept override { return *this; }
  [[nodiscard]] std::intptr_t monitor_handle() const noexcept override;
  void close_stdin() noexcept override;
  void signal_group_terminate() noexcept override;
  void kill_group() noexcept override;
  void release_resources(std::chrono::milliseconds bound) noexcept override;
  [[nodiscard]] long pid() const noexcept override;
  // Test seam: bytes read from the engine but not yet returned as a line.
  [[nodiscard]] std::size_t pending_bytes_for_tests() const noexcept { return pending_stdout_.size(); }
```

and the POSIX private member `int process_group_{-1};` after `process_id_`.

`secure_frontend/src/engine_stdio_process.cpp`: in the POSIX `start` success line (line 171) set `process_id_ = child; process_group_ = child;`. Add after `terminate`:

```cpp
std::intptr_t EngineStdioProcess::monitor_handle() const noexcept { return kNoMonitorHandle; }

void EngineStdioProcess::close_stdin() noexcept {
#if defined(_WIN32)
  close_handle(stdin_write_);
#else
  close_fd(stdin_write_);
#endif
}

void EngineStdioProcess::signal_group_terminate() noexcept {
#if !defined(_WIN32)
  if (process_group_ > 0) (void)kill(-process_group_, SIGTERM);
#endif
}

void EngineStdioProcess::kill_group() noexcept {
#if defined(_WIN32)
  if (job_handle_ != nullptr) (void)TerminateJobObject(static_cast<HANDLE>(job_handle_), kSupervisorKillExitCode);
#else
  // Spec §3.7a: the process-group id stays valid while any member lives, so
  // this reaches grandchildren even after the leader was reaped.
  if (process_group_ > 0) (void)kill(-process_group_, SIGKILL);
#endif
}

void EngineStdioProcess::release_resources(const std::chrono::milliseconds bound) noexcept {
  (void)bound;
#if !defined(_WIN32)
  process_group_ = -1;  // never signal a group id the kernel may hand out again
#endif
}

long EngineStdioProcess::pid() const noexcept {
#if defined(_WIN32)
  return process_handle_ != nullptr ? static_cast<long>(GetProcessId(static_cast<HANDLE>(process_handle_))) : -1;
#else
  return process_group_;
#endif
}
```

`secure_frontend/include/secure_frontend/engine_protocol_client.hpp`: after `closed()` (line 118) add

```cpp
  // The code that failed the protocol (set by every terminal failure), or nullopt.
  [[nodiscard]] std::optional<EngineProtocolErrorCode> failure_code() const noexcept { return failure_code_; }
```

and the private member `std::optional<EngineProtocolErrorCode> failure_code_;` after `stop_requested_`. In `secure_frontend/src/engine_protocol_client.cpp` `fail()` (lines 71-75) becomes:

```cpp
std::optional<EngineProtocolError> EngineProtocolClient::fail(const EngineProtocolErrorCode code, std::string message) {
  state_ = State::failed;
  failure_code_ = code;
  return EngineProtocolError{code, std::move(message)};
}
```

`secure_frontend/include/secure_frontend/chat_execution.hpp`, after `health()` (line 76):

```cpp
  // Eager READY (SP2): the supervisor consumed READY on `protocol` before the
  // first request; execute() then never waits for it. Call once, before execute().
  void adopt_ready(const EngineReady& ready) noexcept;
```

`secure_frontend/src/chat_execution.cpp`, after `health()`:

```cpp
void ChatExecutionService::adopt_ready(const EngineReady& ready) noexcept {
  engine_context_tokens_ = ready.max_context_tokens;
  received_ready_ = true;
}
```

`secure_frontend/include/secure_frontend/api_router.hpp`: replace lines 23-26 and 34-39 with

```cpp
// Optional engine state for /health. It must return one of the fixed literals
// "starting", "ready", "generating", "failed"; any other value fails closed
// (503, no engine field) so the body never echoes dynamic text. The optional
// recovery provider returns "", "restarting" or "exhausted"; a non-empty value
// is allowed only with "failed" and anything else fails closed the same way.
using EngineStateProvider = std::function<std::string_view()>;
```

```cpp
  // /health returns {"status":"ok"|"unavailable"} and, when engine_state is
  // set, also "engine":"<state>" and, when recovery_state returns a value,
  // "recovery":"<value>". The status code is 200 only when health_status
  // returns true. Per /health request the providers are called exactly once
  // each, in the order health_status, engine_state, recovery_state, so a
  // caller can take one snapshot in the first and report it from the others.
  ApiRouter(HealthStatusProvider health_status, ModelListProvider model_list, EngineStateProvider engine_state = {},
            EngineStateProvider recovery_state = {});
```

and the member `const EngineStateProvider recovery_state_;` after `engine_state_`. In `secure_frontend/src/api_router.cpp` replace lines 24-38 with:

```cpp
bool is_engine_state_literal(std::string_view state) {
  return state == "starting" || state == "ready" || state == "generating" || state == "failed";
}

bool is_recovery_literal(std::string_view value) { return value == "restarting" || value == "exhausted"; }

HttpResponse health_response(bool healthy, const EngineStateProvider& engine_state, const EngineStateProvider& recovery_state) {
  if (!engine_state) {
    return healthy ? json_response(200, "{\"status\":\"ok\"}\n") : json_response(503, "{\"status\":\"unavailable\"}\n");
  }
  const std::string_view state = engine_state();
  if (!is_engine_state_literal(state)) return json_response(503, "{\"status\":\"unavailable\"}\n");
  const std::string_view recovery = recovery_state ? recovery_state() : std::string_view{};
  if (!recovery.empty() && (state != "failed" || !is_recovery_literal(recovery))) {
    return json_response(503, "{\"status\":\"unavailable\"}\n");
  }
  std::string body{healthy ? "{\"status\":\"ok\",\"engine\":\"" : "{\"status\":\"unavailable\",\"engine\":\""};
  body.append(state);
  if (!recovery.empty()) {
    body.append("\",\"recovery\":\"");
    body.append(recovery);
  }
  body.append("\"}\n");
  return json_response(healthy ? 200 : 503, std::move(body));
}
```

and lines 92-102: the constructor initializes `recovery_state_(std::move(recovery_state))` and `dispatch` calls `health_response(healthy, engine_state_, recovery_state_)`.

- [ ] **Step 5: CMake**

After the `secure_frontend_chat_execution` library (`add_library(secure_frontend_chat_execution`, line 101 at `7ddcd755`) add:

```cmake
add_library(secure_frontend_engine_supervisor
    src/engine_supervisor.cpp
    src/supervisor_wake.cpp
)
```

After the `secure_frontend_chat_execution` link block (`target_link_libraries(secure_frontend_chat_execution`, line 224) add:

```cmake
target_compile_features(secure_frontend_engine_supervisor PUBLIC cxx_std_20)
target_include_directories(secure_frontend_engine_supervisor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(secure_frontend_engine_supervisor
    PUBLIC secure_frontend_chat_execution secure_frontend_engine_stdio_process)
```

Append `secure_frontend_engine_supervisor` to the `secure_frontend_set_warnings` foreach list (`foreach(_target ...` ending in `strata-secure-server)`, lines 261-269). The `strata-secure-server` link list is NOT changed here (Task 6b). In the tests block:

```cmake
    secure_frontend_add_test_executable(secure_frontend_engine_supervisor_tests tests/engine_supervisor_test.cpp
        secure_frontend_engine_supervisor secure_frontend_test_support)
    secure_frontend_test_paths(secure_frontend_engine_supervisor_tests)
    # UNIT: in-process fakes only (also smoke-run under wine). PROCESS: real
    # fake-engine children. LINUX / WINDOWS: platform-only process cases. Later
    # tasks append case names to these four set() lines only.
    set(SECURE_FRONTEND_SUPERVISOR_UNIT_CASES supervisor_health_literals_per_state)
    set(SECURE_FRONTEND_SUPERVISOR_PROCESS_CASES)
    set(SECURE_FRONTEND_SUPERVISOR_LINUX_CASES)
    set(SECURE_FRONTEND_SUPERVISOR_WINDOWS_CASES)
    if(WIN32)
        list(APPEND SECURE_FRONTEND_SUPERVISOR_PROCESS_CASES ${SECURE_FRONTEND_SUPERVISOR_WINDOWS_CASES})
    else()
        list(APPEND SECURE_FRONTEND_SUPERVISOR_PROCESS_CASES ${SECURE_FRONTEND_SUPERVISOR_LINUX_CASES})
    endif()
    secure_frontend_add_cases(secure_frontend_engine_supervisor_tests ${SECURE_FRONTEND_SUPERVISOR_UNIT_CASES} TIMEOUT 120)
    # Process cases are windows-runtime on Windows (Jobs, exit codes, pipes).
    secure_frontend_add_cases(secure_frontend_engine_supervisor_tests ${SECURE_FRONTEND_SUPERVISOR_PROCESS_CASES}
        TIMEOUT 120 LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})
```

Add `health_carries_recovery_only_with_failed` to SP1's `secure_frontend_add_cases(secure_frontend_api_router_tests ...)` call and `chat_uses_adopted_ready_without_waiting` to `secure_frontend_add_cases(secure_frontend_chat_execution_tests ...)`.

- [ ] **Step 6: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc 2>&1 | tail -3`
Expected: `100% tests passed, 0 tests failed out of <Task 5 total> + 3` (new: `secure_frontend_engine_supervisor_tests.supervisor_health_literals_per_state`, `secure_frontend_api_router_tests.health_carries_recovery_only_with_failed`, `secure_frontend_chat_execution_tests.chat_uses_adopted_ready_without_waiting`; `secure_frontend_api_router_tests.health_samples_status_before_state_once_each` passes with its SP2 mapping).

- [ ] **Step 7: Cross-compile and run wine** — compiles with `-Werror`; `ctest --preset windows-llvm-mingw-cross` → `100% tests passed, 0 tests failed out of <Task 5 wine total> + 3` (all three new cases are unit cases and unlabeled). Record.

- [ ] **Step 8: Handoff row**

```markdown
| V2, V11 (core) | `EngineSupervisor` library: one supervisor thread spawns, consumes READY eagerly, watches and kills; the serving thread borrows the generation via `Lease`. `health_for`: `starting` → 503, `ready`/`generating` → 200; router's optional `recovery` literal (`restarting`/`exhausted`) only with `engine=failed`. Fresh `EngineInstance` per spawn. Until the restart policy lands every end opens the circuit. SP1-bound values in `supervision_defaults.hpp`. Not yet wired into the server (Task 6b, after the SP1 merge). | `supervisor_health_literals_per_state`, `health_samples_status_before_state_once_each` (updated), `health_carries_recovery_only_with_failed`, `chat_uses_adopted_ready_without_waiting` | <recorded linux line>; wine <recorded> |
```

- [ ] **Step 9: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "secure_frontend: engine supervisor core with eager READY and leases; router recovery literal (V2, V11 core)" \
  -m "secure_frontend_engine_supervisor_tests.supervisor_health_literals_per_state, secure_frontend_api_router_tests.health_carries_recovery_only_with_failed, secure_frontend_chat_execution_tests.chat_uses_adopted_ready_without_waiting; health_samples_status_before_state_once_each updated for the SP2 mapping. The server is wired in Task 6b." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 7: Restart policy — exponential backoff, healthy window, circuit (V3)

**Files:**
- Modify: `secure_frontend/include/secure_frontend/restart_policy.hpp` (add `RestartDecision`, `RestartTracker`)
- Create: `secure_frontend/src/restart_policy.cpp`
- Create: `secure_frontend/tests/support/fake_supervised_process.hpp`
- Modify: `secure_frontend/include/secure_frontend/engine_supervisor.hpp` (member `RestartTracker tracker_` declared directly after `log_`)
- Modify: `secure_frontend/src/engine_supervisor.cpp` (constructor init list; `schedule_restart`)
- Modify: `secure_frontend/tests/engine_supervisor_test.cpp` (five cases, including Review Focus 6)
- Modify: `secure_frontend/CMakeLists.txt` (source; cases)
- Modify: `docs/AGENT_HANDOFF.md` (V3 row)

**Interfaces:**
- Consumes: `EngineSupervisor`, `SupervisorClock`, `SupervisedProcess`, `ProcessFactory`, `make_stdio_process_factory`, `EngineLaunchPlan` (Task 6a).
- Produces: `RestartDecision{circuit_open, delay, consecutive_failures}`, `RestartTracker(RestartPolicy)`, `RestartTracker::record_failure(healthy_for)`; test support `test_support::FakeChild` (`push_line`, `end(ExitStatus)`, `reapable`, `kills`, `writes`), `FakeChildTransport`, `FakeSupervisedProcess`, `FakeFactory` (`factory()`, `child(n)`, `spawned()`), `ManualClock` (`advance`, `delays`, `on_sleep`), `LogCapture` (`sink`, `contains`, `wait_for`), `wait_generation_ready(supervisor, n, timeout)`.

- [ ] **Step 1: Write the test support** — `secure_frontend/tests/support/fake_supervised_process.hpp`:

```cpp
#pragma once

#include "secure_frontend/engine_supervisor.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace test_support {

// The status a fake child shows after the supervisor's kill.
inline secure_frontend::ExitStatus fake_kill_status() {
#if defined(_WIN32)
  return {false, secure_frontend::kSupervisorKillExitCode};
#else
  return {true, 9U};
#endif
}

// Control block of one fake engine generation; the test keeps a copy.
struct FakeChild final {
  std::mutex mutex;
  std::condition_variable readable;
  std::deque<secure_frontend::EngineReadResult> reads;
  std::vector<std::string> writes;
  std::optional<secure_frontend::ExitStatus> exit;
  bool reapable{true};
  int kills{0};

  void push_line(std::string line) {
    std::lock_guard lock(mutex);
    reads.push_back({secure_frontend::EngineReadKind::line, std::move(line), {}});
    readable.notify_all();
  }
  // The child ends: stdout reaches EOF and `status` becomes reapable.
  void end(const secure_frontend::ExitStatus status) {
    std::lock_guard lock(mutex);
    if (!exit) exit = status;
    reads.push_back({secure_frontend::EngineReadKind::closed, {}, {}});
    readable.notify_all();
  }
};

class FakeChildTransport final : public secure_frontend::EngineLineTransport {
 public:
  explicit FakeChildTransport(std::shared_ptr<FakeChild> child) : child_(std::move(child)) {}
  bool write_line(const std::string_view line, std::string&) override {
    std::lock_guard lock(child_->mutex);
    if (child_->exit) return false;
    child_->writes.emplace_back(line);
    return true;
  }
  secure_frontend::EngineReadResult read_line(const std::chrono::milliseconds timeout) override {
    std::unique_lock lock(child_->mutex);
    if (timeout.count() <= 0) return {secure_frontend::EngineReadKind::timeout, {}, {}};
    if (!child_->readable.wait_for(lock, timeout, [this] { return !child_->reads.empty(); })) {
      return {secure_frontend::EngineReadKind::timeout, {}, {}};
    }
    auto result = child_->reads.front();
    if (result.kind != secure_frontend::EngineReadKind::closed) child_->reads.pop_front();  // EOF stays
    return result;
  }

 private:
  std::shared_ptr<FakeChild> child_;
};

class FakeSupervisedProcess final : public secure_frontend::SupervisedProcess {
 public:
  explicit FakeSupervisedProcess(std::shared_ptr<FakeChild> child) : child_(child), transport_(std::move(child)) {}
  secure_frontend::EngineLineTransport& transport() noexcept override { return transport_; }
  std::optional<secure_frontend::ExitStatus> try_reap() override {
    std::lock_guard lock(child_->mutex);
    if (!child_->exit || !child_->reapable) return std::nullopt;
    return child_->exit;
  }
  std::intptr_t monitor_handle() const noexcept override { return secure_frontend::kNoMonitorHandle; }
  // Like the real engine: EOF on stdin is a clean exit.
  void close_stdin() noexcept override { end_with({false, 0U}); }
  void signal_group_terminate() noexcept override {}
  void kill_group() noexcept override {
    std::lock_guard lock(child_->mutex);
    ++child_->kills;
    if (!child_->exit) child_->exit = fake_kill_status();
    child_->reads.push_back({secure_frontend::EngineReadKind::closed, {}, {}});
    child_->readable.notify_all();
  }
  void release_resources(std::chrono::milliseconds) noexcept override {}
  long pid() const noexcept override { return -1; }

 private:
  void end_with(const secure_frontend::ExitStatus status) noexcept {
    std::lock_guard lock(child_->mutex);
    if (!child_->exit) child_->exit = status;
    child_->reads.push_back({secure_frontend::EngineReadKind::closed, {}, {}});
    child_->readable.notify_all();
  }
  std::shared_ptr<FakeChild> child_;
  FakeChildTransport transport_;
};

// Generation n gets a fresh FakeChild; `script` runs at spawn time on the supervisor thread.
class FakeFactory final {
 public:
  using Script = std::function<void(FakeChild& child, std::uint64_t generation)>;
  explicit FakeFactory(Script script) : state_(std::make_shared<State>()) { state_->script = std::move(script); }
  [[nodiscard]] secure_frontend::ProcessFactory factory() const {
    auto state = state_;
    return [state](const std::uint64_t generation) -> secure_frontend::SpawnResult {
      auto child = std::make_shared<FakeChild>();
      if (state->script) state->script(*child, generation);
      {
        std::lock_guard lock(state->mutex);
        state->children.push_back(child);
      }
      return std::unique_ptr<secure_frontend::SupervisedProcess>(std::make_unique<FakeSupervisedProcess>(child));
    };
  }
  [[nodiscard]] std::shared_ptr<FakeChild> child(const std::uint64_t generation) const {
    std::lock_guard lock(state_->mutex);
    if (generation == 0U || generation > state_->children.size()) return nullptr;
    return state_->children[generation - 1U];
  }
  [[nodiscard]] std::uint64_t spawned() const {
    std::lock_guard lock(state_->mutex);
    return state_->children.size();
  }

 private:
  struct State {
    std::mutex mutex;
    std::vector<std::shared_ptr<FakeChild>> children;
    Script script;
  };
  std::shared_ptr<State> state_;
};

// Backoff delays are recorded and return at once; now() moves only by those
// delays and by advance(). Makes the §3.3 numbers exactly testable.
class ManualClock final : public secure_frontend::SupervisorClock {
 public:
  [[nodiscard]] time_point now() const override {
    std::lock_guard lock(mutex_);
    return now_;
  }
  [[nodiscard]] bool sleep_backoff(const std::chrono::milliseconds delay, secure_frontend::SupervisorWake&) override {
    std::function<void()> hook;
    {
      std::lock_guard lock(mutex_);
      delays_.push_back(delay);
      now_ += delay;
      hook = on_sleep_;
    }
    if (hook) hook();
    return true;
  }
  void advance(const std::chrono::milliseconds by) {
    std::lock_guard lock(mutex_);
    now_ += by;
  }
  [[nodiscard]] std::vector<std::chrono::milliseconds> delays() const {
    std::lock_guard lock(mutex_);
    return delays_;
  }
  void on_sleep(std::function<void()> hook) {
    std::lock_guard lock(mutex_);
    on_sleep_ = std::move(hook);
  }

 private:
  mutable std::mutex mutex_;
  time_point now_{};
  std::vector<std::chrono::milliseconds> delays_;
  std::function<void()> on_sleep_;
};

// Thread-safe capture of supervisor log lines.
class LogCapture final {
 public:
  [[nodiscard]] secure_frontend::SupervisorLog sink() const {
    auto state = state_;
    return [state](const std::string_view line) {
      std::lock_guard lock(state->mutex);
      state->lines.emplace_back(line);
    };
  }
  [[nodiscard]] bool contains(const std::string_view text) const {
    std::lock_guard lock(state_->mutex);
    return std::any_of(state_->lines.begin(), state_->lines.end(), [text](const std::string& line) { return line == text; });
  }
  [[nodiscard]] bool wait_for(const std::string_view text, const std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!contains(text)) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
  }

 private:
  struct State {
    std::mutex mutex;
    std::vector<std::string> lines;
  };
  std::shared_ptr<State> state_ = std::make_shared<State>();
};

inline bool wait_generation_ready(const secure_frontend::EngineSupervisor& supervisor, const std::uint64_t generation,
                                  const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (supervisor.generations_started() == generation && supervisor.snapshot().state == secure_frontend::SupervisorState::ready) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

}  // namespace test_support
```

- [ ] **Step 2: Write the failing tests** — in `secure_frontend/tests/engine_supervisor_test.cpp` add includes `"support/fake_supervised_process.hpp"`, `"support/test_tokenizer.hpp"`, `<mutex>`, `<memory>`, `<variant>`, and inside the namespace:

```cpp
secure_frontend::EngineLaunchPlan fake_plan(std::vector<std::string> arguments) {
  return {.command = {.executable = test_support::fake_engine_path(), .arguments = std::move(arguments), .environment = {}}};
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
  supervisor.start();
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
  supervisor.start();
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

void proc04_recovers_after_transient_crash() {
  const auto dir = test_support::make_temp_dir("transient");
  secure_frontend::SupervisorConfig config;
  config.policy.backoff_initial = 50ms;
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor(
      config, secure_frontend::make_stdio_process_factory(fake_plan({"--fail-first=1", "--counter-file=" + test_support::utf8_path(dir / "count")})),
      std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
  supervisor.start();
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
  supervisor.start();
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
  supervisor.start();
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
```

Register them in `main` (`{"proc04_restart_backoff_then_circuit_open", proc04_restart_backoff_then_circuit_open},` and so on, in SP1's `run_cases` list) and in CMake: append `proc04_restart_backoff_then_circuit_open proc04_healthy_window_resets_failure_count proc04_missing_engine_opens_circuit_without_spinning proc04_client_err_lines_open_circuit_until_restart` to the `set(SECURE_FRONTEND_SUPERVISOR_UNIT_CASES ...)` line and `proc04_recovers_after_transient_crash` to `set(SECURE_FRONTEND_SUPERVISOR_PROCESS_CASES ...)`.

- [ ] **Step 3: Run to verify they fail**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'proc04_(restart|healthy|recovers|missing|client_err)' --output-on-failure 2>&1 | grep -E "FAILED|tests passed"`
Expected: five failures — today every end opens the circuit at once (`backoff is exactly 1 s, then 2 s`, `generation 2 ready`, `exactly three attempts`, and for the ERR case `generation 2 ready` all fail).

- [ ] **Step 4: Implement the tracker and use it**

Append to `secure_frontend/include/secure_frontend/restart_policy.hpp` (inside the namespace):

```cpp
struct RestartDecision final {
  bool circuit_open{false};
  std::chrono::milliseconds delay{0};
  std::uint32_t consecutive_failures{0};
};

// Spec §3.3: a failure is any unrequested end, every §3.5 kill and any spawn
// failure. Delays grow initial * factor^(n-1), capped at backoff_max; the
// max_consecutive_failures-th failure opens the circuit for good.
class RestartTracker final {
 public:
  explicit RestartTracker(RestartPolicy policy) noexcept : policy_(policy) {}
  // `healthy_for` is the ended generation's continuous ready/generating time
  // (zero for spawn failures and generations that never became ready);
  // reaching healthy_window first resets the count to 0.
  [[nodiscard]] RestartDecision record_failure(std::chrono::milliseconds healthy_for) noexcept;
  [[nodiscard]] std::uint32_t consecutive_failures() const noexcept { return failures_; }

 private:
  RestartPolicy policy_;
  std::uint32_t failures_{0};
};
```

`secure_frontend/src/restart_policy.cpp`:

```cpp
#include "secure_frontend/restart_policy.hpp"

#include <algorithm>
#include <limits>

namespace secure_frontend {

RestartDecision RestartTracker::record_failure(const std::chrono::milliseconds healthy_for) noexcept {
  if (healthy_for >= policy_.healthy_window) failures_ = 0U;
  if (failures_ < std::numeric_limits<std::uint32_t>::max()) ++failures_;
  const std::uint32_t limit = std::max<std::uint32_t>(policy_.max_consecutive_failures, 1U);
  if (failures_ >= limit) return {true, std::chrono::milliseconds::zero(), failures_};
  const auto factor = static_cast<std::chrono::milliseconds::rep>(std::max<std::uint32_t>(policy_.backoff_factor, 1U));
  std::chrono::milliseconds delay = std::min(policy_.backoff_initial, policy_.backoff_max);
  for (std::uint32_t step = 1U; step < failures_; ++step) {
    if (delay.count() > policy_.backoff_max.count() / factor) {
      delay = policy_.backoff_max;
      break;
    }
    delay *= factor;
  }
  return {false, std::min(delay, policy_.backoff_max), failures_};
}

}  // namespace secure_frontend
```

In `engine_supervisor.hpp` add the member `RestartTracker tracker_;` directly after `SupervisorLog log_;`; in the constructor's init list add `tracker_(config_.policy)` after `log_(...)` (declaration order, `-Wreorder`). Replace `schedule_restart` in `engine_supervisor.cpp` with:

```cpp
bool EngineSupervisor::schedule_restart(const std::chrono::milliseconds healthy_for, const bool reaped) {
  (void)reaped;
  const RestartDecision decision = tracker_.record_failure(healthy_for);
  if (decision.circuit_open) {
    set_state(SupervisorState::circuit_open, nullptr);
    return false;
  }
  set_state(SupervisorState::backoff, nullptr);
  (void)clock_->sleep_backoff(decision.delay, wake_);  // woken early only by shutdown; run() re-checks
  return true;
}
```

Add `src/restart_policy.cpp` to `secure_frontend_engine_supervisor`.

- [ ] **Step 5: Run to verify they pass**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc 2>&1 | tail -3`
Expected: `100% tests passed, 0 tests failed out of <Task 6a total> + 5`.

- [ ] **Step 6: Cross-compile and run wine** — compiles with `-Werror`; `ctest --preset windows-llvm-mingw-cross` → `100% tests passed, 0 tests failed out of <Task 6a wine total> + 4` (four new unit cases run under wine; the process case is labeled). Record.

- [ ] **Step 7: Handoff row**

```markdown
| V3 | `RestartTracker`: 3 consecutive failures open the circuit; backoff 1 s → 2 s → … ≤ 60 s; 300 s healthy window resets the count; spawn failures count. Injectable `SupervisorClock` (`ManualClock` in tests). Three client-triggerable ERR lines in a row open the circuit until a restart (Review Focus 6, accepted). | `proc04_restart_backoff_then_circuit_open`, `proc04_healthy_window_resets_failure_count`, `proc04_recovers_after_transient_crash`, `proc04_missing_engine_opens_circuit_without_spinning` (Review Focus 2), `proc04_client_err_lines_open_circuit_until_restart` (Review Focus 6) | <recorded linux line>; wine <recorded> |
```

- [ ] **Step 8: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "supervisor: bounded restart with exponential backoff, healthy window and circuit (V3)" \
  -m "V3: proc04_restart_backoff_then_circuit_open, proc04_healthy_window_resets_failure_count, proc04_recovers_after_transient_crash. Review Focus 2: proc04_missing_engine_opens_circuit_without_spinning. Review Focus 6: proc04_client_err_lines_open_circuit_until_restart." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 8: Monitor — death between requests is seen without a request (V4)

**Files:**
- Modify: `secure_frontend/include/secure_frontend/engine_stdio_process.hpp` (POSIX member `int pidfd_{-1};`)
- Modify: `secure_frontend/src/engine_stdio_process.cpp` (open the pidfd after a successful POSIX start; close it on reap and in `cleanup`; `monitor_handle`)
- Modify: `secure_frontend/src/engine_supervisor.cpp` (`watch`)
- Modify: `secure_frontend/tests/engine_supervisor_test.cpp` (two cases), `secure_frontend/CMakeLists.txt`
- Modify: `docs/AGENT_HANDOFF.md` (V4 row)

**Interfaces:**
- Consumes: `SupervisorWake::wait(monitored, timeout)`, `SupervisorConfig::monitor_poll` (Task 6a).
- Produces: `EngineStdioProcess::monitor_handle()` returns the pidfd (POSIX, kernels with `pidfd_open`) or the process handle (Windows); otherwise `kNoMonitorHandle` and the supervisor polls every `monitor_poll` (100 ms).

- [ ] **Step 1: Write the failing tests** (`engine_supervisor_test.cpp`, add `#include "secure_frontend/engine_stdio_process.hpp"`; `proc04_monitor_detects_idle_crash` → append to the `SECURE_FRONTEND_SUPERVISOR_PROCESS_CASES` line, `proc04_monitor_fallback_without_pidfd` → the `SECURE_FRONTEND_SUPERVISOR_UNIT_CASES` line; both into SP1's `run_cases` list in `main`):

```cpp
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
  supervisor.start();
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
  supervisor.start();
  expect(supervisor.wait_for_state(SupervisorState::ready, 5s), "ready");
  const auto started = std::chrono::steady_clock::now();
  fakes.child(1U)->end({false, 0U});  // no monitor handle: only the fallback poll can see this
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 2s), "the fallback poll notices the exit");
  expect(std::chrono::steady_clock::now() - started < 500ms, "within a few poll intervals");
  supervisor.shutdown(5s);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R proc04_monitor --output-on-failure 2>&1 | grep FAILED`
Expected: both fail with `... is noticed ...` (the supervisor waits only on its wake object), and on Linux `proc04_monitor_detects_idle_crash` also fails `Linux: the monitor waits on a pidfd` (`monitor_handle()` returns `kNoMonitorHandle` until Step 3).

- [ ] **Step 3: Implement**

`secure_frontend/src/engine_stdio_process.cpp`, in the anonymous namespace (POSIX part):

```cpp
// A pidfd becomes readable when the child exits, so the supervisor can wait on
// the child itself (kernel 5.3+). -1 means "poll instead".
int open_pidfd(const pid_t pid) noexcept {
#if defined(__linux__) && defined(SYS_pidfd_open)
  return static_cast<int>(syscall(SYS_pidfd_open, pid, 0U));
#else
  (void)pid;
  return -1;
#endif
}
```

In POSIX `start`, the success line becomes `process_id_ = child; process_group_ = child; pidfd_ = open_pidfd(child); stdin_write_ = input[1]; stdout_read_ = output[0]; return std::nullopt;`. In `try_reap` (POSIX), right after `process_id_ = -1;` add `close_fd(pidfd_);`. In `cleanup` (POSIX) add `close_fd(pidfd_);`. Replace `monitor_handle`:

```cpp
std::intptr_t EngineStdioProcess::monitor_handle() const noexcept {
#if defined(_WIN32)
  return process_handle_ != nullptr ? reinterpret_cast<std::intptr_t>(process_handle_) : kNoMonitorHandle;
#else
  return pidfd_ >= 0 ? static_cast<std::intptr_t>(pidfd_) : kNoMonitorHandle;
#endif
}
```

`secure_frontend/src/engine_supervisor.cpp`, replace `watch`:

```cpp
KillReason EngineSupervisor::watch(EngineInstance& instance) {
  for (;;) {
    {
      std::lock_guard lock(mutex_);
      if (pending_kill_ != KillReason::none) return std::exchange(pending_kill_, KillReason::none);
    }
    if (stop_requested()) return KillReason::shutdown;
    if (instance.process->try_reap()) return KillReason::none;
    // Spec V4: wait on the child itself, so a death between requests is seen
    // at once; without a pidfd, poll every monitor_poll.
    const std::intptr_t handle = instance.process->monitor_handle();
    (void)wake_.wait(handle, handle == kNoMonitorHandle ? config_.monitor_poll : std::chrono::milliseconds(-1));
  }
}
```

- [ ] **Step 4: Run to verify they pass** — `ctest --preset linux-gcc 2>&1 | tail -3` → `100% tests passed, 0 tests failed out of <Task 7 total> + 2`.

- [ ] **Step 5: Cross-compile and run wine** — `-Werror` clean; wine `100% tests passed, 0 tests failed out of <Task 7 wine total> + 1` (the fallback unit case runs; the process case is labeled). Record.

- [ ] **Step 6: Handoff row**

```markdown
| V4 | The supervisor waits on the child: pidfd (Linux 5.3+; the test asserts `monitor_handle() != kNoMonitorHandle` on Linux, so the pidfd path is proven, not just the fallback), process handle (Windows), else a 100 ms `waitpid(WNOHANG)` poll. | `proc04_monitor_detects_idle_crash`, `proc04_monitor_fallback_without_pidfd` | <recorded linux line>; wine <recorded> |
```

- [ ] **Step 7: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "supervisor: monitor the engine between requests via pidfd / process handle (V4)" \
  -m "V4: proc04_monitor_detects_idle_crash, proc04_monitor_fallback_without_pidfd." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 9: Fatal verification lines kill and restart the engine (V5)

**Files:**
- Modify: `secure_frontend/include/secure_frontend/engine_protocol_client.hpp:37-49` (new error code), add `kEngineFatalPrefixes` and `is_engine_fatal_line`
- Modify: `secure_frontend/src/engine_protocol_client.cpp` (`await_ready` and `read_generation` check the prefixes right after `validate_incoming`)
- Modify: `secure_frontend/src/engine_supervisor.cpp` (`kill_reason_for`)
- Modify: `secure_frontend/tests/engine_supervisor_test.cpp`, `secure_frontend/CMakeLists.txt`
- Modify: `docs/AGENT_HANDOFF.md` (V5 row)

**Interfaces:**
- Produces: `EngineProtocolErrorCode::fatal_engine_line` (appended after `response_limit`), `kEngineFatalPrefixes` (`std::array<std::string_view, 4>`, exactly `upstream/serve/server.py:587-588`), `is_engine_fatal_line(std::string_view)`; `kill_reason_for` → `KillReason::fatal_line`.

- [ ] **Step 1: Write the failing test** (append to the `SECURE_FRONTEND_SUPERVISOR_PROCESS_CASES` line and SP1's `run_cases` list; `support/test_tokenizer.hpp` and `<variant>` are included since Task 7):

```cpp
void proc04_fatal_prefix_kills_engine() {
  // Protocol level: each prefix fails the generation; the trailing DONE is never consumed.
  for (std::size_t index = 0; index < secure_frontend::kEngineFatalPrefixes.size(); ++index) {
    auto child = std::make_shared<test_support::FakeChild>();
    test_support::FakeChildTransport transport(child);
    child->push_line("READY 4096 stop");
    secure_frontend::EngineProtocolClient protocol(transport);
    secure_frontend::EngineReady ready{};
    expect(!protocol.await_ready(1s, ready).has_value(), "READY");
    expect(!protocol.begin_generation(1, {1}).has_value(), "GEN");
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
    supervisor.start();
    expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "generation 1 ready");
    const auto outcome = secure_frontend::execute_with_lease(supervisor, request, tokenizer);
    expect(outcome && std::holds_alternative<secure_frontend::ChatExecutionError>(*outcome), "the request fails; no result after a fatal line");
    expect(test_support::wait_generation_ready(supervisor, 2U, 10s), "the engine is replaced");
    expect(log.contains("engine generation 1 ended: killed_by_supervisor(fatal_line)"), "killed as fatal_line, prefix " + std::to_string(index));
    supervisor.shutdown(5s);
  }
}
```

- [ ] **Step 2: Run to verify it fails** — `cmake --build --preset linux-gcc 2>&1 | grep -m1 error` → `kEngineFatalPrefixes is not a member of secure_frontend`. (With the symbols stubbed, the behavior today is `engine_error` / `killed_by_supervisor(protocol_failed)`.)

- [ ] **Step 3: Implement**

`engine_protocol_client.hpp`: add `#include <array>`; append `fatal_engine_line,` after `response_limit,` in `EngineProtocolErrorCode`; after the enum:

```cpp
// The engine's fatal verification lines (upstream/serve/server.py:587-588
// FATAL_PREFIXES), matched as prefixes. The engine lives on but is unusable;
// a trailing DONE is never accepted (upstream _pump, upstream/serve/server.py:775-791).
inline constexpr std::array<std::string_view, 4> kEngineFatalPrefixes{
    "ERR verify: timed out at layer ",
    "ERR verify batch: timed out at layer ",
    "ERR verify: an earlier window never finished",
    "ERR verify batch: an earlier window never finished",
};

[[nodiscard]] bool is_engine_fatal_line(std::string_view line) noexcept;
```

`engine_protocol_client.cpp`, after the anonymous namespace:

```cpp
bool is_engine_fatal_line(const std::string_view line) noexcept {
  return std::any_of(kEngineFatalPrefixes.begin(), kEngineFatalPrefixes.end(),
                     [line](const std::string_view prefix) { return line.substr(0, prefix.size()) == prefix; });
}
```

In `await_ready` and in `read_generation`, directly after the `validate_incoming` block, insert:

```cpp
    if (is_engine_fatal_line(response.line)) {
      return fail(EngineProtocolErrorCode::fatal_engine_line, "engine reported a fatal verification failure");
    }
```

`engine_supervisor.cpp`, in `kill_reason_for` add `case EngineProtocolErrorCode::fatal_engine_line: return KillReason::fatal_line;`.

- [ ] **Step 4: Run to verify it passes** — `ctest --preset linux-gcc 2>&1 | tail -3` → 100%, `out of <Task 8 total> + 1`.

- [ ] **Step 5: Cross-compile and run wine** — `-Werror` clean; wine stays at the Task 8 wine total (case labeled). Record.

- [ ] **Step 6: Handoff row**

```markdown
| V5 | The four upstream `FATAL_PREFIXES` fail the protocol (`fatal_engine_line`, fixed message, trailing `DONE` not accepted); the supervisor kills the group and restarts (`killed_by_supervisor(fatal_line)`). Other `ERR` lines keep SP0's per-request behavior; the failed generation is replaced. | `proc04_fatal_prefix_kills_engine` | <recorded linux line>; wine <recorded> |
```

- [ ] **Step 7: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "protocol: upstream fatal verify lines kill and restart the engine (V5)" \
  -m "V5: proc04_fatal_prefix_kills_engine (all four prefixes; trailing DONE refused)." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 10: READY timeout and drain failure kill and count (V6, V7)

V8 (an abort mid-generation is a generic `503`) needs the server wired to the supervisor, so its test moved to Task 6b.

**Files:**
- Modify: `secure_frontend/src/engine_supervisor.cpp` (`await_ready` deadline; `kill_reason_for` adds `abandoned`)
- Modify: `secure_frontend/include/secure_frontend/engine_protocol_client.hpp` (append `abandoned` to the error codes), `secure_frontend/src/engine_protocol_client.cpp:264` (`abandon` records it)
- Modify: `secure_frontend/tests/engine_supervisor_test.cpp` (V6, V7), `secure_frontend/CMakeLists.txt`
- Modify: `docs/AGENT_HANDOFF.md` (V6, V7 row)

**Interfaces:**
- Consumes: `RestartPolicy::ready_timeout` (Task 6a), `ChatExecutionLimits::{generation_timeout, stop_drain_timeout}` (SP0).
- Produces: `EngineProtocolErrorCode::abandoned` (set by `abandon()` when no earlier failure was recorded); `kill_reason_for` → `KillReason::drain_failed`; READY deadline → `KillReason::ready_timeout`.

- [ ] **Step 1: Write the failing tests**

`engine_supervisor_test.cpp` (both appended to the `SECURE_FRONTEND_SUPERVISOR_PROCESS_CASES` line and SP1's `run_cases` list):

```cpp
void proc04_ready_timeout_kills_and_counts() {
  secure_frontend::SupervisorConfig config;
  config.policy.ready_timeout = 200ms;
  config.policy.max_consecutive_failures = 2U;
  config.policy.backoff_initial = 20ms;
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor(config, secure_frontend::make_stdio_process_factory(fake_plan({"--hang-before-ready"})),
                                               std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
  supervisor.start();
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
  supervisor.start();
  expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "generation 1 ready");
  const auto tokenizer = test_support::byte_tokenizer();
  const secure_frontend::ChatCompletionRequest request{"fake-model", {{"user", "Hi"}}, 2U};
  const auto outcome = secure_frontend::execute_with_lease(supervisor, request, tokenizer);
  expect(outcome && std::holds_alternative<secure_frontend::ChatExecutionError>(*outcome), "the request fails after STOP is ignored");
  expect(test_support::wait_generation_ready(supervisor, 2U, 10s), "a new generation replaces the wedged one");
  expect(log.contains("engine generation 1 ended: killed_by_supervisor(drain_failed)"), "killed as drain_failed");
  supervisor.shutdown(5s);
}
```

- [ ] **Step 2: Run to verify the failures**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'proc04_(ready_timeout|drain)' --output-on-failure 2>&1 | grep -E "FAILED|Passed|Failed"`
Expected: `proc04_ready_timeout_kills_and_counts` fails (READY is waited for without a deadline); `proc04_drain_failure_kills_and_restarts` fails on the log line (it says `killed_by_supervisor(protocol_failed)`).

- [ ] **Step 3: Implement**

`engine_supervisor.cpp`, replace `await_ready`:

```cpp
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
    if (error->code != EngineProtocolErrorCode::timeout) return KillReason::protocol_failed;
    if (instance.process->try_reap()) return KillReason::none;
  }
}
```

(`remaining_until` takes the clock's `now()`, so a `ManualClock` that does not advance never times READY out in unit tests.)

`engine_protocol_client.hpp`: append `abandoned,` after `fatal_engine_line,`. `engine_protocol_client.cpp` line 264:

```cpp
void EngineProtocolClient::abandon() noexcept {
  state_ = State::failed;
  if (!failure_code_) failure_code_ = EngineProtocolErrorCode::abandoned;  // ignored STOP: drain failed (§3.5(3))
}
```

`kill_reason_for`: add `case EngineProtocolErrorCode::abandoned: return KillReason::drain_failed;`.

- [ ] **Step 4: Run to verify they pass** — `ctest --preset linux-gcc 2>&1 | tail -3` → 100% `out of <Task 9 total> + 2`.

- [ ] **Step 5: Cross-compile and run wine** — `-Werror` clean; wine stays at the Task 9 wine total (both cases labeled). Record.

- [ ] **Step 6: Handoff row**

```markdown
| V6, V7 | READY deadline `ready_timeout` (900 s default) kills and counts (`killed_by_supervisor(ready_timeout)`); an engine that ignores STOP is killed and replaced (`drain_failed`). (V8: Task 6b.) | `proc04_ready_timeout_kills_and_counts`, `proc04_drain_failure_kills_and_restarts` | <recorded linux line>; wine <recorded> |
```

- [ ] **Step 7: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "supervisor: READY timeout and drain failure kill, count and restart (V6, V7)" \
  -m "V6: secure_frontend_engine_supervisor_tests.proc04_ready_timeout_kills_and_counts. V7: proc04_drain_failure_kills_and_restarts." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 11: No leaks across generations; an unreaped child opens the circuit (V9, V10)

**Files:**
- Modify: `secure_frontend/src/engine_supervisor.cpp` (`schedule_restart`)
- Modify: `secure_frontend/tests/engine_supervisor_test.cpp` (two cases), `secure_frontend/CMakeLists.txt`
- Modify: `docs/AGENT_HANDOFF.md` (V9/V10 row)

**Interfaces:**
- Consumes: `EngineStdioProcess::pending_bytes_for_tests()`, `Lease::process()` (Task 6a); `test_support::children_of` (Task 3).
- Produces: rule "no spawn while a killed child is unreaped; past `reap_timeout` → `circuit_open`" (spec V10, K3).

- [ ] **Step 1: Write the tests** (`proc04_no_leak_across_restarts` → the `SECURE_FRONTEND_SUPERVISOR_PROCESS_CASES` line; `proc04_unreaped_child_opens_circuit` → the `SECURE_FRONTEND_SUPERVISOR_UNIT_CASES` line; both into SP1's `run_cases` list). Add `#include "secure_frontend/engine_stdio_process.hpp"`, `"support/server_harness.hpp"`, `<filesystem>`, `<system_error>`, and on POSIX `<unistd.h>`, on Windows `"secure_frontend/win32_prelude.hpp"`:

```cpp
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

void proc04_no_leak_across_restarts() {
  secure_frontend::SupervisorConfig config;
  config.policy.max_consecutive_failures = 100U;
  config.policy.backoff_initial = 1ms;
  config.policy.backoff_max = 1ms;
  secure_frontend::EngineSupervisor supervisor(config, secure_frontend::make_stdio_process_factory(fake_plan({})));
  supervisor.start();
  std::optional<std::size_t> baseline;
  for (std::uint64_t generation = 1U; generation <= 11U; ++generation) {  // 10 forced restarts
    expect(test_support::wait_generation_ready(supervisor, generation, 10s), "generation " + std::to_string(generation) + " ready");
    const std::size_t handles = open_handle_count();
    if (!baseline) baseline = handles;
    expect(handles == *baseline, "descriptor/handle count stable at generation " + std::to_string(generation) + ": " + std::to_string(handles) +
                                     " vs " + std::to_string(*baseline));
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
  supervisor.start();
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
}
```

- [ ] **Step 2: Run to verify** — `ctest --preset linux-gcc -R 'proc04_(no_leak|unreaped)' --output-on-failure`. Expected: `proc04_unreaped_child_opens_circuit` FAILS (`no respawn while the child is unreaped` — today the policy backs off and spawns generation 2). `proc04_no_leak_across_restarts` passes (V9's fix, fresh objects per generation, landed in Task 6a; if it fails, stop and report the leak it found).

- [ ] **Step 3: Implement** — replace `schedule_restart`:

```cpp
bool EngineSupervisor::schedule_restart(const std::chrono::milliseconds healthy_for, const bool reaped) {
  if (!reaped) {
    // Spec V10 / K3: never spawn while a killed child is unreaped; past
    // reap_timeout the circuit opens whatever the failure count.
    set_state(SupervisorState::circuit_open, nullptr);
    return false;
  }
  const RestartDecision decision = tracker_.record_failure(healthy_for);
  if (decision.circuit_open) {
    set_state(SupervisorState::circuit_open, nullptr);
    return false;
  }
  set_state(SupervisorState::backoff, nullptr);
  (void)clock_->sleep_backoff(decision.delay, wake_);  // woken early only by shutdown; run() re-checks
  return true;
}
```

- [ ] **Step 4: Run to verify** — `ctest --preset linux-gcc 2>&1 | tail -3` → 100% `out of <Task 10 total> + 2`.

- [ ] **Step 5: Cross-compile and run wine** — `-Werror` clean; wine `out of <Task 10 wine total> + 1` (the unreaped unit case runs). Record.

- [ ] **Step 6: Handoff row**

```markdown
| V9, V10 | Ten forced restarts leave one live engine, no zombie, a stable descriptor/handle count and an empty stdout buffer in each new generation. A killed child that cannot be reaped within `reap_timeout` (30 s) is abandoned and opens the circuit; nothing respawns while it lives. | `proc04_no_leak_across_restarts`, `proc04_unreaped_child_opens_circuit` | <recorded linux line>; wine <recorded> |
```

- [ ] **Step 7: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "supervisor: an unreaped child opens the circuit; no leaks across restarts (V9, V10)" \
  -m "V9: proc04_no_leak_across_restarts. V10: proc04_unreaped_child_opens_circuit." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 12: Group members die even after the leader is reaped (C1)

**Files:**
- Modify: `secure_frontend/src/engine_stdio_process.cpp` (POSIX branch of `terminate`, lines 285-313 at `7ddcd755`)
- Modify: `secure_frontend/tests/engine_child_test.cpp`, `secure_frontend/CMakeLists.txt` (`SECURE_FRONTEND_CHILD_LINUX_CASES`)
- Modify: `docs/AGENT_HANDOFF.md` (C1 row)

**Interfaces:**
- Consumes: `EngineStdioProcess::process_group_`, `try_reap()`, `pid()` (Tasks 5–6); `--spawn-grandchild --grandchild-ignore-term --grandchild-pidfile` (Task 1).
- Produces: `terminate()` always ends with `kill(-pgid, SIGKILL)` and forgets the group id only after success.

- [ ] **Step 1: Write the failing test** (Linux only; add `#include <csignal>` and `<sys/types.h>` under `#if !defined(_WIN32)`):

```cpp
#if !defined(_WIN32)
void proc05_group_killed_after_leader_reaped() {
  const auto dir = test_support::make_temp_dir("c1");
  const auto pidfile = dir / "grandchild.pid";
  secure_frontend::EngineStdioProcess engine;
  expect(!engine.start({.executable = test_support::fake_engine_path(),
                        .arguments = {"--spawn-grandchild", "--grandchild-ignore-term", "--grandchild-pidfile=" + test_support::utf8_path(pidfile)},
                        .environment = {}})
              .has_value(),
         "start");
  expect(engine.read_line(5s).line == "READY 4096 stop", "READY");
  const auto grandchild = test_support::read_pidfile(pidfile, 5s);
  expect(grandchild.has_value(), "the grandchild started");
  if (!grandchild) return;
  (void)kill(static_cast<pid_t>(engine.pid()), SIGKILL);  // only the leader
  expect(wait_reaped(engine, 5s).has_value(), "the leader is reaped first");
  expect(test_support::process_alive(*grandchild), "precondition: the grandchild outlived its leader");
  expect(engine.terminate(200ms), "terminate succeeds");
  expect(test_support::wait_until_gone(*grandchild, 2s), "a group member dies although the leader was already reaped");
  test_support::kill_process(*grandchild);
}
#endif
```

Register it in SP1's `run_cases` list in `main` under `#if !defined(_WIN32)` and append it to the `set(SECURE_FRONTEND_CHILD_LINUX_CASES ...)` line.

- [ ] **Step 2: Run to verify it fails** — `ctest --preset linux-gcc -R proc05_group_killed --output-on-failure | grep FAILED` → `a group member dies although the leader was already reaped` (today `terminate` returns at once when the leader is gone).

- [ ] **Step 3: Implement** — replace the POSIX branch of `terminate`:

```cpp
#else
  if (process_group_ < 0) return true;  // never started, or already fully cleaned up
  if (process_id_ >= 0) {
    if (kill(-process_group_, SIGTERM) != 0 && errno != ESRCH) return false;
    const auto end = std::chrono::steady_clock::now() + grace;
    while (std::chrono::steady_clock::now() < end && !try_reap()) usleep(10000);
  }
  // Spec §3.7(a): always SIGKILL the group, even when the leader was already
  // reaped: members that ignored SIGTERM or outlived the leader keep the group
  // id valid and must die too.
  (void)kill(-process_group_, SIGKILL);
  for (int step = 0; step < 50 && process_id_ >= 0; ++step) {
    if (try_reap()) break;
    usleep(10000);
  }
  if (process_id_ >= 0) return false;  // abandoned: still unreaped after the bound (header contract)
  close_fd(stdin_write_);
  close_fd(stdout_read_);
  process_group_ = -1;
  return true;
#endif
```

- [ ] **Step 4: Run to verify it passes** — `ctest --preset linux-gcc 2>&1 | tail -3` → 100% `out of <Task 11 total> + 1` (`secure_frontend_engine_stdio_process_tests.terminate_reaps_killed_child` still passes).

- [ ] **Step 5: Cross-compile** — `-Werror` clean (POSIX-only change; Windows untouched); wine unchanged.

- [ ] **Step 6: Handoff row**

```markdown
| C1 | POSIX `terminate()` always ends with `kill(-pgid, SIGKILL)`, also after the leader was reaped; the group id is forgotten only after success. | `proc05_group_killed_after_leader_reaped` (L) | <recorded linux line>; wine unchanged |
```

- [ ] **Step 7: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "posix launch: terminate always kills the whole process group (C1)" \
  -m "C1: proc05_group_killed_after_leader_reaped." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 13: Subreaper sweep catches `setsid` and double-fork escapes (C2)

**Files:**
- Create: `secure_frontend/src/descendant_sweep.hpp`, `secure_frontend/src/descendant_sweep.cpp`
- Modify: `secure_frontend/src/engine_supervisor.cpp` (constructor; `end_generation` after reap)
- Modify: `secure_frontend/CMakeLists.txt` (source in `secure_frontend_engine_supervisor`; the `set(SECURE_FRONTEND_SUPERVISOR_LINUX_CASES ...)` line)
- Modify: `secure_frontend/tests/engine_supervisor_test.cpp`
- Modify: `docs/AGENT_HANDOFF.md` (C2 row)

**Interfaces:**
- Produces: `become_child_subreaper()`, `list_own_children()`, `sweep_orphans(keep, bound)` (internal header); the supervisor becomes subreaper at construction when `SupervisorConfig::sweep_orphans` (default true) and sweeps after every generation ends.

- [ ] **Step 1: Write the failing test** (append to the `SECURE_FRONTEND_SUPERVISOR_LINUX_CASES` line; SP1's `run_cases` list under `#if !defined(_WIN32)`):

```cpp
#if !defined(_WIN32)
void proc05_setsid_grandchild_is_reaped() {
  const auto dir = test_support::make_temp_dir("c2");
  const auto pidfile = dir / "grandchild.pid";
  secure_frontend::SupervisorConfig config;
  config.policy.max_consecutive_failures = 1U;
  secure_frontend::EngineSupervisor supervisor(
      config, secure_frontend::make_stdio_process_factory(
                  fake_plan({"--spawn-grandchild", "--grandchild-setsid", "--grandchild-pidfile=" + test_support::utf8_path(pidfile)})));
  supervisor.start();
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
  test_support::kill_process(*grandchild);
  supervisor.shutdown(5s);
}
#endif
```

- [ ] **Step 2: Run to verify it fails** — `ctest --preset linux-gcc -R proc05_setsid --output-on-failure | grep FAILED` → `a setsid grandchild outside the engine's group is swept` (it re-parents to init today).

- [ ] **Step 3: Implement**

`secure_frontend/src/descendant_sweep.hpp`:

```cpp
#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

namespace secure_frontend {

// Linux (spec §3.7b): orphaned descendants of an engine re-parent to this
// process instead of init, so they can be found and reaped. No-op elsewhere.
bool become_child_subreaper() noexcept;
// Linux: this process's direct children (from every thread's
// /proc/self/task/<tid>/children). Empty elsewhere.
std::vector<long> list_own_children();
// Linux: SIGKILLs and reaps every direct child not in `keep`, repeating until
// none is left or `bound` passes. Returns how many were reaped. The production
// frontend has no children but engines (C7), so `keep` is the live leaders.
std::size_t sweep_orphans(const std::vector<long>& keep, std::chrono::milliseconds bound);

}  // namespace secure_frontend
```

`secure_frontend/src/descendant_sweep.cpp`:

```cpp
#include "descendant_sweep.hpp"

#include "secure_frontend/deadline.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#if defined(__linux__)
#include <csignal>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace secure_frontend {

bool become_child_subreaper() noexcept {
#if defined(__linux__)
  return prctl(PR_SET_CHILD_SUBREAPER, 1UL, 0UL, 0UL, 0UL) == 0;
#else
  return false;
#endif
}

std::vector<long> list_own_children() {
  std::vector<long> children;
#if defined(__linux__)
  std::error_code error;
  for (const auto& task : std::filesystem::directory_iterator("/proc/self/task", error)) {
    std::ifstream input(task.path() / "children");
    long child = 0;
    while (input >> child) children.push_back(child);
  }
  std::sort(children.begin(), children.end());
  children.erase(std::unique(children.begin(), children.end()), children.end());
#endif
  return children;
}

std::size_t sweep_orphans(const std::vector<long>& keep, const std::chrono::milliseconds bound) {
  std::size_t reaped = 0;
#if defined(__linux__)
  const auto deadline = deadline_after(bound);
  for (;;) {
    bool found = false;
    for (const long child : list_own_children()) {
      if (std::find(keep.begin(), keep.end(), child) != keep.end()) continue;
      found = true;
      (void)kill(static_cast<pid_t>(child), SIGKILL);
      int status = 0;
      if (waitpid(static_cast<pid_t>(child), &status, WNOHANG) == static_cast<pid_t>(child)) ++reaped;
    }
    if (!found || remaining_until(deadline).count() == 0) return reaped;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
#else
  (void)keep;
  (void)bound;
  return reaped;
#endif
}

}  // namespace secure_frontend
```

`engine_supervisor.cpp`: `#include "descendant_sweep.hpp"`; the constructor body becomes `{ if (config_.sweep_orphans) (void)become_child_subreaper(); }`; in `end_generation`, directly after the `if (!status) { ... }` block, insert:

```cpp
  // Spec §3.7(b): orphans (setsid, setpgid, double fork) re-parented to us are
  // killed and reaped; no engine is alive at this point.
  if (config_.sweep_orphans) (void)sweep_orphans({}, std::chrono::milliseconds(1000));
```

Add `src/descendant_sweep.cpp` to `secure_frontend_engine_supervisor`.

- [ ] **Step 4: Run to verify it passes** — 100% `out of <Task 12 total> + 1`.

- [ ] **Step 5: Cross-compile** — `-Werror` clean (the Windows build compiles the no-op branches); wine unchanged.

- [ ] **Step 6: Handoff row**

```markdown
| C2 | The frontend is the child subreaper (`PR_SET_CHILD_SUBREAPER`); after every generation it SIGKILLs and reaps every child that is not a live engine, so `setsid`/`setpgid`/double-fork escapes die. | `proc05_setsid_grandchild_is_reaped` (L) | <recorded linux line>; wine unchanged |
```

- [ ] **Step 7: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "supervisor: subreaper sweep reaps setsid and double-fork escapes (C2)" \
  -m "C2: proc05_setsid_grandchild_is_reaped." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 14: The engine leader dies with a SIGKILLed frontend (C3)

**Files:**
- Modify: `secure_frontend/src/posix_child_launch.hpp:38-67` (`PreparedExec::expected_parent`; `prepare` records `getpid()`) and `:80-82` (`exec_child` starts with `PR_SET_PDEATHSIG` + `getppid()` check)
- Modify: `secure_frontend/tests/server_test.cpp`, `secure_frontend/CMakeLists.txt` (the `set(SECURE_FRONTEND_SERVER_LINUX_CASES ...)` line)
- Modify: `docs/AGENT_HANDOFF.md` (C3 row)

**Interfaces:**
- Consumes: engines are forked only by a long-lived thread — the SP0 server's main thread now, the supervisor thread after Task 6b — so PDEATHSIG (which fires when the forking *thread* exits) fires only when the frontend dies. The test needs no supervisor: it waits for the grandchild's pidfile (written after READY), not for `/health`, so it runs against the SP0 server too.
- Produces: the first entry of `SECURE_FRONTEND_SERVER_LINUX_CASES` (Task 3 list, registered only when `NOT WIN32`).

- [ ] **Step 1: Write the failing test** — `server_test.cpp` (add `#include "support/test_paths.hpp"` already present; POSIX only):

```cpp
#if !defined(_WIN32)
void proc05_frontend_sigkill_kills_engine() {
  const auto dir = test_support::make_temp_dir("c3");
  const auto pidfile = dir / "grandchild.pid";
  test_support::ServerOptions options;
  // --stall-stdin keeps the engine alive after stdin EOF, so only PDEATHSIG can end it.
  options.engine_args = {"--spawn-grandchild", "--grandchild-pidfile=" + test_support::utf8_path(pidfile), "--stall-stdin"};
  auto server = ServerProcess::start(options);
  expect(server != nullptr && server->wait_until_listening(10s), "the server listens");
  if (!server) return;
  // The fake writes the grandchild's pidfile after READY: the engine is up
  // whether or not the server has consumed READY yet (SP0 waits lazily).
  const auto grandchild = test_support::read_pidfile(pidfile, 10s);
  expect(grandchild.has_value(), "the engine is past READY");
  const auto children = test_support::children_of(server->pid());
  expect(children.size() == 1U, "one engine");
  if (children.size() != 1U) return;
  server->stop(ServerProcess::Stop::kill);
  (void)server->wait_exit(5s);
  expect(test_support::wait_until_gone(children.front(), 2s), "the engine leader dies with a SIGKILLed frontend (PR_SET_PDEATHSIG)");
  if (grandchild) {
    // Spec §3.7 residual risk: without a delegated cgroup the engine's own
    // descendants survive a frontend SIGKILL. Logged, not asserted.
    std::cout << "residual risk (spec 3.7): grandchild " << (test_support::wait_until_gone(*grandchild, 1s) ? "gone" : "survived")
              << " after a frontend SIGKILL without a delegated cgroup\n";
    test_support::kill_process(*grandchild);
  }
}
#endif
```

Register it in SP1's `run_cases` list in `main` under `#if !defined(_WIN32)`; in CMake the Task 3 line becomes `set(SECURE_FRONTEND_SERVER_LINUX_CASES proc05_frontend_sigkill_kills_engine)` (later Linux-only server cases are appended to it, Windows-only ones to `SECURE_FRONTEND_SERVER_WINDOWS_CASES`).

- [ ] **Step 2: Run to verify it fails** — `ctest --preset linux-gcc -R proc05_frontend_sigkill --output-on-failure | grep FAILED` → `the engine leader dies with a SIGKILLed frontend` (the stalled engine survives today).

- [ ] **Step 3: Implement** — in `posix_child_launch.hpp` add `#include <csignal>` and, under `#if defined(__linux__)`, `#include <sys/prctl.h>`; add the member `pid_t expected_parent{-1};` to `PreparedExec` and `out.expected_parent = getpid();` at the top of `prepare`. At the very top of `exec_child`, before `setpgid`:

```cpp
#if defined(__linux__)
  // Spec §3.7(c): the leader dies with the frontend. PDEATHSIG fires when the
  // forking *thread* exits, which is why only the long-lived supervisor thread
  // forks engines. If the parent already died between fork() and here,
  // getppid() differs and the child exits instead of running orphaned.
  if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) fail(report_fd);
  if (getppid() != prepared.expected_parent) _exit(127);
#endif
```

- [ ] **Step 4: Run to verify it passes** — 100% `out of <Task 13 total> + 1`; the test prints the residual-risk line (record it in the handoff row). The harness destructor then kills the surviving grandchild (it re-parented to the subreaper test process), so nothing is left on the dev host.

- [ ] **Step 5: Cross-compile** — `-Werror` clean; wine unchanged.

- [ ] **Step 6: Handoff row**

```markdown
| C3 | Pre-exec `PR_SET_PDEATHSIG(SIGKILL)` + `getppid()` recheck; forks only from the supervisor thread. Residual risk (spec §3.7): without a delegated cgroup, the engine's own descendants survive a frontend SIGKILL — logged by the test as "<recorded line>". | `proc05_frontend_sigkill_kills_engine` (L) | <recorded linux line>; wine unchanged |
```

- [ ] **Step 7: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "posix launch: PR_SET_PDEATHSIG ties the engine to the frontend (C3)" \
  -m "C3: proc05_frontend_sigkill_kills_engine (leader asserted; grandchild logged as the spec 3.7 residual risk)." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 17: Windows — console events reach only the frontend; the Job kills all; no WER stall (C6)

**Files:**
- Modify: `secure_frontend/src/engine_stdio_process.cpp` (Windows `start`: `CREATE_NEW_PROCESS_GROUP`, `JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION`, `SetErrorMode`)
- Modify: `secure_frontend/src/engine_process.cpp` (same Job and creation flags)
- Modify: `secure_frontend/tests/engine_supervisor_test.cpp` (one W case), `secure_frontend/CMakeLists.txt`
- Modify: `docs/AGENT_HANDOFF.md` (C6 row, completed by Task 15)

**Interfaces:**
- Consumes: `make_stdio_process_factory`, `execute_with_lease` (Task 6a); fake `--abort-in-gen`, which itself runs with `SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX` (Task 1).
- Produces: the Windows engine runs in its own console process group inside a Job with `JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION`; the frontend's error mode (inherited by the engine) suppresses WER dialogs. Task 15's console handler and its two server W cases build on this.

- [ ] **Step 1: Write the W test** (compiled only on Windows):

The two server-level C6 cases (`proc05_windows_ctrl_break_stops_all_descendants`, `proc05_windows_terminate_frontend_kills_job`) need the console handler and the orderly shutdown wired into `main`, so they are written and registered in Task 15 (after the SP1 merge); the launch changes below are what they exercise.

`engine_supervisor_test.cpp` (append to the `SECURE_FRONTEND_SUPERVISOR_WINDOWS_CASES` line; SP1's `run_cases` list under `#if defined(_WIN32)`):

```cpp
#if defined(_WIN32)
void proc04_windows_abort_does_not_stall() {
  secure_frontend::SupervisorConfig config;
  config.policy.backoff_initial = 20ms;
  test_support::LogCapture log;
  secure_frontend::EngineSupervisor supervisor(config, secure_frontend::make_stdio_process_factory(fake_plan({"--abort-in-gen"})),
                                               std::make_shared<secure_frontend::SteadySupervisorClock>(), log.sink());
  supervisor.start();
  expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "ready");
  const auto tokenizer = test_support::byte_tokenizer();
  const auto started = std::chrono::steady_clock::now();
  (void)secure_frontend::execute_with_lease(supervisor, {"fake-model", {{"user", "Hi"}}, 2U}, tokenizer);
  expect(log.wait_for("engine generation 1 ended: abort", 5s), "the abort is observed, not stalled on a WER dialog");
  expect(std::chrono::steady_clock::now() - started < 5s, "within 5 s");
  supervisor.shutdown(5s);
}
#endif
```

CMake: the Task 6a line becomes `set(SECURE_FRONTEND_SUPERVISOR_WINDOWS_CASES proc04_windows_abort_does_not_stall)`.

- [ ] **Step 2: RED on Windows (cross-build + wine record)** — `cmake --build --preset windows-llvm-mingw-cross 2>&1 | tail -2` compiles; `WINEPREFIX=$PWD/out/wineprefix wine out/build/windows-llvm-mingw-cross/secure_frontend_engine_supervisor_tests.exe --case proc04_windows_abort_does_not_stall; echo "exit $?"` — record. Expected on real Windows before the fix: an abort that is not caught by the Job limit can stall on WER. Authoritative in MSYS2.

- [ ] **Step 3: Implement** — `secure_frontend/src/engine_stdio_process.cpp`, Windows `start`. Replace line 124 with:

```cpp
  HANDLE job = CreateJobObjectW(nullptr, nullptr); JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  // Spec §3.7: an unhandled exception in any Job process ends it at once
  // (no WER wait), and closing the Job kills every member.
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
```

and replace the `CreateProcessW` call (lines 146-150, the "Start suspended" comment through `&startup.StartupInfo, &info);`) with:

```cpp
  // Spec §3.7: a crashing engine must never wait on a Windows Error Reporting
  // dialog; the error mode is inherited by the child. Set once per process.
  static const UINT previous_error_mode = SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
  (void)previous_error_mode;
  // Start suspended so the child is inside the Job before it runs a single
  // instruction: no early descendant can escape the kill-on-close Job (PROC-05).
  // CREATE_NEW_PROCESS_GROUP: console Ctrl-C/Ctrl-Break reach only the
  // frontend, whose handler runs the orderly shutdown (Task 15).
  const BOOL created = CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, TRUE,
                                      CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
                                          CREATE_NEW_PROCESS_GROUP,
                                      environment.data(), nullptr, &startup.StartupInfo, &info);
```

`secure_frontend/src/engine_process.cpp` (the SP0 twin), Windows `start`: line 174 becomes

```cpp
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
```

and lines 182-185 (the "Start suspended" comment and the `if (!CreateProcessW(` head) become

```cpp
  static const UINT previous_error_mode = SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
  (void)previous_error_mode;
  // Start suspended so the child is inside the Job before it runs a single
  // instruction: no early descendant can escape the kill-on-close Job (PROC-05).
  // A new console process group: console events reach only the frontend.
  if (!CreateProcessW(command.executable.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                      CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP, environment.data(), nullptr,
                      &startup, &info)) {
```

(the body of that `if` — `CloseHandle(job); return ProcessError{...};` — is unchanged).

- [ ] **Step 4: GREEN locally** — Linux: `ctest --preset linux-gcc 2>&1 | tail -3` → 100%, total unchanged from Task 14 (the case is Windows-only). Cross: `-Werror` clean; wine total unchanged (labeled); record the wine run of `proc04_windows_abort_does_not_stall`.

- [ ] **Step 5: Handoff row**

```markdown
| C6 | Windows: `CREATE_NEW_PROCESS_GROUP` (console events reach only the frontend, whose `SetConsoleCtrlHandler` runs the orderly shutdown, Task 15); Job adds `JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION`; `SetErrorMode(SEM_NOGPFAULTERRORBOX)` before spawn. The two server-level C6 cases land in Task 15. | `proc04_windows_abort_does_not_stall` (W); Task 15: `proc05_windows_ctrl_break_stops_all_descendants`, `proc05_windows_terminate_frontend_kills_job` | Linux <recorded line, unchanged>; wine: <recorded>; **authoritative in MSYS2 — pending push** |
```

- [ ] **Step 6: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "windows launch: engine in its own console group; Job dies on unhandled exceptions; no WER stall (C6)" \
  -m "C6: secure_frontend_engine_supervisor_tests.proc04_windows_abort_does_not_stall (W, authoritative in the MSYS2 job). The server-level C6 cases follow in Task 15." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 20: The child starts with default signal dispositions and an empty mask (N4)

**Files:**
- Modify: `secure_frontend/src/posix_child_launch.hpp` (`BlockAllSignals`, `reset_signal_state`, called before `execve`)
- Modify: `secure_frontend/src/engine_stdio_process.cpp:166-168`, `secure_frontend/src/engine_process.cpp:213-221` (fork under `BlockAllSignals`)
- Modify: `secure_frontend/tests/engine_child_test.cpp`, `secure_frontend/CMakeLists.txt` (the `SECURE_FRONTEND_CHILD_LINUX_CASES` line)
- Modify: `docs/AGENT_HANDOFF.md` (N4 row)

**Interfaces:**
- Produces: `posix_launch::BlockAllSignals` (RAII; blocks every signal in the forking thread), `posix_launch::reset_signal_state()` (child only: every disposition but SIGKILL/SIGSTOP to `SIG_DFL`, then an empty mask).

- [ ] **Step 1: Write the failing test** (Linux; includes `<pthread.h>`; append to the `SECURE_FRONTEND_CHILD_LINUX_CASES` line and SP1's `run_cases` list under `#if !defined(_WIN32)`):

```cpp
#if !defined(_WIN32)
void proc02_child_signal_state_is_default() {
  struct sigaction ignore {};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  struct sigaction previous {};
  (void)sigaction(SIGPIPE, &ignore, &previous);  // what install_process_defaults() does in the frontend
  sigset_t block;
  sigemptyset(&block);
  sigaddset(&block, SIGUSR1);
  sigset_t old_mask;
  (void)pthread_sigmask(SIG_BLOCK, &block, &old_mask);
  secure_frontend::EngineStdioProcess child;
  const bool started = !child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value();
  (void)pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
  (void)sigaction(SIGPIPE, &previous, nullptr);
  expect(started, "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  const auto dump = test_support::request_dump(child, 5s);
  expect(dump && dump->sig_blocked == "0000000000000000", "the engine starts with an empty signal mask, saw " + (dump ? dump->sig_blocked : ""));
  expect(dump && dump->sig_ignored == "0000000000000000", "and nothing ignored (default SIGPIPE), saw " + (dump ? dump->sig_ignored : ""));
  (void)child.terminate(250ms);
}
#endif
```

(`#include "support/fake_engine_dump.hpp"` in `engine_child_test.cpp`.)

- [ ] **Step 2: Run to verify it fails** — `ctest --preset linux-gcc -R proc02_child_signal --output-on-failure | grep FAILED` → `SigBlk` shows SIGUSR1 (`0000000000000200`) and `SigIgn` shows SIGPIPE (`0000000000001000`).

- [ ] **Step 3: Implement** — `posix_child_launch.hpp` (add `#include <csignal>` and `#include <pthread.h>`):

```cpp
// Spec §3.12, parent side: every signal is blocked in the forking thread from
// just before fork() until fork() returns, so no handler (the shutdown
// self-pipe) can run in the child between fork and exec.
class BlockAllSignals final {
 public:
  BlockAllSignals() noexcept {
    sigset_t all;
    sigfillset(&all);
    (void)pthread_sigmask(SIG_SETMASK, &all, &previous_);
  }
  ~BlockAllSignals() { (void)pthread_sigmask(SIG_SETMASK, &previous_, nullptr); }
  BlockAllSignals(const BlockAllSignals&) = delete;
  BlockAllSignals& operator=(const BlockAllSignals&) = delete;

 private:
  sigset_t previous_{};
};

// Spec §3.12, child side (async-signal-safe): SIG_IGN survives execve, so put
// every disposition back to SIG_DFL (the engine gets the default SIGPIPE, not
// the frontend's ignore), then clear the mask inherited from the parent.
inline void reset_signal_state() noexcept {
  struct sigaction defaults {};
  defaults.sa_handler = SIG_DFL;
  sigemptyset(&defaults.sa_mask);
  for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
    if (signal_number == SIGKILL || signal_number == SIGSTOP) continue;
    (void)sigaction(signal_number, &defaults, nullptr);
  }
  sigset_t none;
  sigemptyset(&none);
  (void)sigprocmask(SIG_SETMASK, &none, nullptr);
}
```

In `exec_child`, immediately before `execve(...)`: `reset_signal_state();`. In `EngineStdioProcess::start` (POSIX) replace lines 166-168 (`const pid_t child = fork();` … `if (child == 0) posix_launch::exec_child(...)`) with:

```cpp
  pid_t child = -1;
  {
    posix_launch::BlockAllSignals blocked;
    child = fork();
    if (child == 0) posix_launch::exec_child(prepared, input[0], output[1], /*null_stderr=*/true, errors[1]);
  }
  if (child < 0) { for (int* pipe_fd : {input, output, errors}) { close_fd(pipe_fd[0]); close_fd(pipe_fd[1]); } return ProcessError{ProcessErrorCode::spawn_failed, "fork failed"}; }
```

In `EngineProcess::start` (`secure_frontend/src/engine_process.cpp`, POSIX) replace lines 213-221 (from `const pid_t child = fork();` through the closing brace of `if (child == 0) { ... }`) with:

```cpp
  pid_t child = -1;
  {
    posix_launch::BlockAllSignals blocked;
    child = fork();
    if (child == 0) {
      // Inherited stdio is kept; every other descriptor is closed before execve.
      posix_launch::exec_child(prepared, -1, -1, /*null_stderr=*/false, error_pipe[1]);
    }
  }
  if (child < 0) {
    close(error_pipe[0]); close(error_pipe[1]);
    return ProcessError{ProcessErrorCode::spawn_failed, "fork failed"};
  }
```

- [ ] **Step 4: Run to verify** — 100% `out of <Task 17 total> + 1`.

- [ ] **Step 5: Cross-compile** — `-Werror` clean; wine unchanged.

- [ ] **Step 6: Handoff row**

```markdown
| N4 | Fork with every signal blocked; the child resets every disposition to `SIG_DFL` and clears the mask before `execve`. | `proc02_child_signal_state_is_default` (L) | <recorded linux line>; wine unchanged |
```

- [ ] **Step 7: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "posix launch: default signal dispositions and an empty mask in the engine (N4)" \
  -m "N4: proc02_child_signal_state_is_default." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 21: Only stdio descriptors and handles reach the child — canaries (N5)

**Files:**
- Modify: `secure_frontend/tests/engine_child_test.cpp` (one L case, one W case), `secure_frontend/CMakeLists.txt`
- Modify: `docs/AGENT_HANDOFF.md` (N5 row)

**Interfaces:**
- Consumes: the existing descriptor sweep and Windows handle list (SP0); `request_dump` (Task 2).
- Produces: no production change (spec N5 "Fix: existing sweep; canaries"); both tests must pass on first run — if one fails, stop and report the leak.

- [ ] **Step 1: Write the tests**

Linux (`#include <netinet/in.h>`, `<sys/socket.h>`, `<fcntl.h>`):

```cpp
#if !defined(_WIN32)
void proc02_canary_descriptors_absent() {
  // What a careless launcher would leak: a non-CLOEXEC fd, a listening socket, an open token file.
  const auto dir = test_support::make_temp_dir("n5");
  const auto token = dir / "token";
  secure_frontend_test::write_file(token, "secret-token", 0600);
  const int token_fd = open(token.c_str(), O_RDONLY);
  const int null_fd = open("/dev/null", O_RDONLY);
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const bool listening = listener >= 0 && bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 1) == 0;
  expect(token_fd >= 0 && null_fd >= 0 && listening, "canaries opened");
  for (const int fd : {token_fd, null_fd, listener}) expect(fcntl(fd, F_GETFD) == 0, "precondition: canary " + std::to_string(fd) + " is inheritable");
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value(), "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  const auto dump = test_support::request_dump(child, 5s);
  expect(dump && dump->fds == std::vector<int>{0, 1, 2}, "the engine sees only stdio: no canary descriptor");
  (void)child.terminate(250ms);
  for (const int fd : {token_fd, null_fd, listener}) (void)close(fd);
}
#endif
```

Windows:

```cpp
#if defined(_WIN32)
template <typename Function>
Function resolve(const wchar_t* module, const char* name) {
  const HMODULE handle = GetModuleHandleW(module);
  if (handle == nullptr) return nullptr;
  return reinterpret_cast<Function>(reinterpret_cast<void (*)()>(GetProcAddress(handle, name)));
}

struct HandleEntry {
  HANDLE HandleValue;
  ULONG_PTR HandleCount;
  ULONG_PTR PointerCount;
  ULONG GrantedAccess;
  ULONG ObjectTypeIndex;
  ULONG HandleAttributes;
  ULONG Reserved;
};
struct HandleSnapshot {
  ULONG_PTR NumberOfHandles;
  ULONG_PTR Reserved;
  HandleEntry Handles[1];
};

void proc02_canary_handles_absent() {
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  const auto dir = test_support::make_temp_dir("n5w");
  HANDLE file = CreateFileW((dir / "token").wstring().c_str(), GENERIC_WRITE, 0, &inherit, CREATE_ALWAYS, 0, nullptr);
  HANDLE event = CreateEventW(&inherit, TRUE, FALSE, nullptr);
  WSADATA data{};
  (void)WSAStartup(MAKEWORD(2, 2), &data);
  const SOCKET socket_handle = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0);
  (void)SetHandleInformation(reinterpret_cast<HANDLE>(socket_handle), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  const std::vector<HANDLE> canaries{file, event, reinterpret_cast<HANDLE>(socket_handle)};
  for (HANDLE canary : canaries) {
    DWORD flags = 0;
    expect(canary != nullptr && canary != INVALID_HANDLE_VALUE && GetHandleInformation(canary, &flags) && (flags & HANDLE_FLAG_INHERIT) != 0U,
           "precondition: the canary is inheritable");
  }
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value(), "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  // Parent-side inspection (spec §3.11): snapshot the child's handle table,
  // duplicate each entry and compare objects; assert canary absence, not a count.
  using QueryFn = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  using CompareFn = BOOL(WINAPI*)(HANDLE, HANDLE);
  const auto query = resolve<QueryFn>(L"ntdll.dll", "NtQueryInformationProcess");
  const auto compare = resolve<CompareFn>(L"kernelbase.dll", "CompareObjectHandles");
  HANDLE process = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(child.pid()));
  expect(process != nullptr && query != nullptr && compare != nullptr, "inspection APIs are available (Windows 10 1607+)");
  if (process != nullptr && query != nullptr && compare != nullptr) {
    std::vector<unsigned char> buffer(1U << 16U);
    ULONG needed = 0;
    LONG status = query(process, 51 /* ProcessHandleInformation */, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
    if (status != 0 && needed > buffer.size()) {
      buffer.resize(needed + 4096U);
      status = query(process, 51, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
    }
    expect(status == 0, "handle snapshot of the engine");
    if (status == 0) {
      const auto* snapshot = reinterpret_cast<const HandleSnapshot*>(buffer.data());
      const HandleEntry* entries = snapshot->Handles;
      for (ULONG_PTR index = 0; index < snapshot->NumberOfHandles; ++index) {
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(process, entries[index].HandleValue, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) continue;
        for (HANDLE canary : canaries) expect(!compare(duplicate, canary), "a parent canary handle reached the engine");
        CloseHandle(duplicate);
      }
    }
    CloseHandle(process);
  }
  (void)child.terminate(250ms);
  CloseHandle(file);
  CloseHandle(event);
  (void)closesocket(socket_handle);
}
#endif
```

Register: append `proc02_canary_descriptors_absent` to the `SECURE_FRONTEND_CHILD_LINUX_CASES` line and `proc02_canary_handles_absent` to the `SECURE_FRONTEND_CHILD_WINDOWS_CASES` line; both into SP1's `run_cases` list under the matching `#if`.

- [ ] **Step 2: Run** — `ctest --preset linux-gcc 2>&1 | tail -4` → 100% `out of <Task 20 total> + 1`.

- [ ] **Step 3: Cross-compile and record wine** — `-Werror` clean; `WINEPREFIX=$PWD/out/wineprefix wine out/build/windows-llvm-mingw-cross/secure_frontend_engine_child_tests.exe --case proc02_canary_handles_absent; echo "exit $?"` — record (wine's `ProcessHandleInformation` is not authoritative). wine ctest total unchanged (labeled).

- [ ] **Step 4: Handoff row**

```markdown
| N5 | Canaries: a non-CLOEXEC fd, a listening socket and an open token file never reach the engine (Linux: `FDS 0,1,2`); Windows: an inheritable file, event and socket are absent from the engine's handle table (parent-side `NtQueryInformationProcess(ProcessHandleInformation)` + `CompareObjectHandles`). No production change. | `proc02_canary_descriptors_absent` (L), `proc02_canary_handles_absent` (W) | <recorded linux line>; wine: <recorded>; W authoritative in MSYS2 |
```

- [ ] **Step 5: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "test: canary descriptors and handles never reach the engine (N5)" \
  -m "N5: proc02_canary_descriptors_absent (L), proc02_canary_handles_absent (W, MSYS2-authoritative)." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 22: Windows stdin pipe — owner-only DACL, random name, verified client, overlapped writes (N6, R1, K2)

**Files:**
- Modify: `secure_frontend/src/engine_stdio_process.cpp` (Windows: lines 103-120 of `start`; `write_line` line 197; helpers)
- Modify: `secure_frontend/include/secure_frontend/engine_stdio_process.hpp` (Windows member `std::wstring stdin_pipe_name_;`, test seams)
- Modify: `secure_frontend/CMakeLists.txt:255-259` (link `advapi32 bcrypt` to `secure_frontend_engine_stdio_process` on `WIN32`)
- Modify: `secure_frontend/tests/engine_child_test.cpp` (N6, R1), `secure_frontend/tests/engine_supervisor_test.cpp` (K2)
- Modify: `docs/AGENT_HANDOFF.md` (row; remove the "default DACL" Known-gaps bullet)

**Interfaces:**
- Consumes: `supervision_defaults::kStdinWriteDeadline` (1 s), `kStdinWriteChunk` (64 KiB) (Task 6a); `kill_reason_for(transport_write_failed) == write_timeout` (Task 6a).
- Produces: Windows `EngineStdioProcess::stdin_pipe_name_for_tests()`, `stdin_handle_for_tests()`; pipe `\\.\pipe\secure_frontend-<pid>-<32 hex>` with `D:P(A;;GA;;;<user SID>)`, `FILE_FLAG_OVERLAPPED` on the frontend end, 64 KiB buffer; a write chunk with no progress for 1 s is cancelled (`CancelIoEx`) and fails with "engine stdin is not writable".

- [ ] **Step 1: Write the W tests**

`engine_child_test.cpp` (Windows; `#include <aclapi.h>`):

```cpp
#if defined(_WIN32)
std::vector<unsigned char> current_token_user() {
  HANDLE token = nullptr;
  (void)OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
  DWORD size = 0;
  (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> buffer(size);
  (void)GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
  CloseHandle(token);
  return buffer;  // a TOKEN_USER
}

void proc02_stdin_pipe_dacl_is_owner_only() {
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value(), "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD status = GetSecurityInfo(static_cast<HANDLE>(child.stdin_handle_for_tests()), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                                       nullptr, nullptr, &dacl, nullptr, &descriptor);
  expect(status == ERROR_SUCCESS && dacl != nullptr, "the pipe has an explicit DACL");
  if (status == ERROR_SUCCESS && dacl != nullptr) {
    ACL_SIZE_INFORMATION size{};
    expect(GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation) && size.AceCount == 1U, "exactly one ACE");
    void* ace = nullptr;
    if (GetAce(dacl, 0, &ace)) {
      const auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
      const auto user = current_token_user();
      expect(allowed->Header.AceType == ACCESS_ALLOWED_ACE_TYPE, "an allow ACE");
      expect(EqualSid(const_cast<DWORD*>(&allowed->SidStart), reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid) != FALSE,
             "for the current user only");
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    expect(GetSecurityDescriptorControl(descriptor, &control, &revision) && (control & SE_DACL_PROTECTED) != 0, "protected: nothing inherited");
    LocalFree(descriptor);
  }
  const std::wstring& name = child.stdin_pipe_name_for_tests();
  expect(name.size() > 33U && name.find_last_of(L'-') == name.size() - 33U, "the name ends with a 128-bit random suffix");
  (void)child.terminate(250ms);
}

void proc03_windows_pipe_second_open_fails() {
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value(), "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  HANDLE second = CreateFileW(child.stdin_pipe_name_for_tests().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  const DWORD error = second == INVALID_HANDLE_VALUE ? GetLastError() : 0U;
  if (second != INVALID_HANDLE_VALUE) CloseHandle(second);
  expect(second == INVALID_HANDLE_VALUE && (error == ERROR_PIPE_BUSY || error == ERROR_ACCESS_DENIED),
         "a second open of the engine stdin pipe fails (busy or denied), error " + std::to_string(error));
  (void)child.terminate(250ms);
}
#endif
```

`engine_supervisor_test.cpp` (append to the `SECURE_FRONTEND_SUPERVISOR_WINDOWS_CASES` line and SP1's `run_cases` list under `#if defined(_WIN32)`):

```cpp
#if defined(_WIN32)
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
  supervisor.start();
  expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "ready");
  const auto tokenizer = test_support::byte_tokenizer();
  // 70,000 '0' bytes are 70,000 prompt tokens of id 239 (three digits plus a
  // comma each): the GEN line is about 280 KB, more than 4x the 64 KiB pipe
  // buffer, and this engine never reads stdin, so a chunk must stall past the
  // 1 s no-progress deadline.
  const secure_frontend::ChatCompletionRequest request{"fake-model", {{"user", std::string(70000U, '0')}}, 2U};
  const auto started = std::chrono::steady_clock::now();
  const auto outcome = secure_frontend::execute_with_lease(supervisor, request, tokenizer);
  expect(outcome && std::holds_alternative<secure_frontend::ChatExecutionError>(*outcome), "the request fails");
  expect(std::chrono::steady_clock::now() - started < 5s, "the blocked write is cancelled at its deadline");
  expect(log.wait_for("engine generation 1 ended: killed_by_supervisor(write_timeout)", 5s), "killed as write_timeout");
  supervisor.shutdown(5s);
}
#endif
```

Register: append `proc02_stdin_pipe_dacl_is_owner_only proc03_windows_pipe_second_open_fails` to the `SECURE_FRONTEND_CHILD_WINDOWS_CASES` line and `proc04_windows_write_timeout_kills_engine` to the `SECURE_FRONTEND_SUPERVISOR_WINDOWS_CASES` line (and SP1's `run_cases` lists under `#if defined(_WIN32)`); after the supervisor `secure_frontend_add_cases` calls add
`if(WIN32)` / `set_tests_properties(secure_frontend_engine_supervisor_tests.proc04_windows_write_timeout_kills_engine PROPERTIES TIMEOUT 300)` / `endif()` (tokenizing and writing a 280 KB GEN line on a slow runner; the case itself ends in seconds).

- [ ] **Step 2: RED (Windows)** — `cmake --build --preset windows-llvm-mingw-cross 2>&1 | grep -m1 error` → `stdin_pipe_name_for_tests is not a member`. Before the fix on real Windows the pipe has the default DACL and the write blocks without a deadline (authoritative in MSYS2).

- [ ] **Step 3: Implement** — Windows includes in `engine_stdio_process.cpp`: `#include <bcrypt.h>`, `#include <sddl.h>`, `"secure_frontend/supervision_defaults.hpp"`. Windows helpers in the anonymous namespace:

```cpp
// 128 random bits for the pipe name (spec §3.11). Task 29 switches this to SP1's fill_random().
std::optional<std::wstring> random_pipe_suffix() {
  std::array<unsigned char, 16> bytes{};
  if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return std::nullopt;
  static constexpr wchar_t kDigits[] = L"0123456789abcdef";
  std::wstring text;
  for (const unsigned char byte : bytes) {
    text.push_back(kDigits[byte >> 4U]);
    text.push_back(kDigits[byte & 0x0fU]);
  }
  return text;
}

// D:P(A;;GA;;;<current user SID>): protected, one ACE, current user only.
// The caller LocalFree()s the result.
PSECURITY_DESCRIPTOR owner_only_descriptor() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;
  DWORD size = 0;
  (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<std::byte> buffer(size);
  const bool have_user = size != 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
  CloseHandle(token);
  if (!have_user) return nullptr;
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) return nullptr;
  const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
  LocalFree(sid);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return nullptr;
  return descriptor;
}
```

Replace lines 103-120 of the Windows `start` (the P7 comment block through the client `CreateFileW`) with:

```cpp
  // Spec §3.11: an owner-only (protected DACL, current user), single-instance
  // pipe with a 128-bit random name that rejects remote clients. The frontend
  // end is overlapped so writes have a deadline (§3.10). After our own client
  // open the pipe's client pid must be ours, which closes the window in which
  // another process could connect first.
  const auto suffix = random_pipe_suffix();
  PSECURITY_DESCRIPTOR descriptor = owner_only_descriptor();
  if (!suffix || descriptor == nullptr) {
    if (descriptor != nullptr) LocalFree(descriptor);
    return ProcessError{ProcessErrorCode::platform_failure, "stdio pipe setup failed"};
  }
  SECURITY_ATTRIBUTES pipe_attrs{sizeof(pipe_attrs), descriptor, FALSE};
  const std::wstring pipe_name = L"\\\\.\\pipe\\secure_frontend-" + std::to_wstring(GetCurrentProcessId()) + L"-" + *suffix;
  parent_stdin = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                  PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                  static_cast<DWORD>(supervision_defaults::kStdinWriteChunk), 0, 0, &pipe_attrs);
  LocalFree(descriptor);
  if (parent_stdin == INVALID_HANDLE_VALUE) parent_stdin = nullptr;
  if (parent_stdin != nullptr) {
    child_stdin = CreateFileW(pipe_name.c_str(), GENERIC_READ, 0, &attrs, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (child_stdin == INVALID_HANDLE_VALUE) child_stdin = nullptr;
  }
  ULONG client_pid = 0;
  if (child_stdin != nullptr && (!GetNamedPipeClientProcessId(parent_stdin, &client_pid) || client_pid != GetCurrentProcessId())) {
    CloseHandle(child_stdin);
    child_stdin = nullptr;
  }
```

At the success line (`process_handle_ = info.hProcess; job_handle_ = job; stdin_write_ = parent_stdin; stdout_read_ = parent_stdout; return std::nullopt;`, line 157 at `7ddcd755`) also set `stdin_pipe_name_ = pipe_name;`. Replace the Windows branch of `write_line` (line 197, with the comment above it on line 196) with:

```cpp
  if (!stdin_write_) { error = "engine stdin is closed"; return false; }
  // Spec §3.10: overlapped writes in chunks; a chunk that makes no progress
  // within kStdinWriteDeadline is cancelled and the write fails, so a hung
  // engine can never hang the serving thread (the supervisor then kills it).
  HANDLE pipe = static_cast<HANDLE>(stdin_write_);
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr) { error = "engine stdin write failed"; return false; }
  bool ok = true;
  std::size_t offset = 0;
  while (ok && offset < record.size()) {
    const auto chunk = static_cast<DWORD>(std::min<std::size_t>(record.size() - offset, supervision_defaults::kStdinWriteChunk));
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    (void)ResetEvent(event);
    DWORD written = 0;
    if (!WriteFile(pipe, record.data() + offset, chunk, nullptr, &overlapped) && GetLastError() != ERROR_IO_PENDING) {
      error = "engine stdin write failed";
      ok = false;
      break;
    }
    if (WaitForSingleObject(event, static_cast<DWORD>(supervision_defaults::kStdinWriteDeadline.count())) != WAIT_OBJECT_0) {
      (void)CancelIoEx(pipe, &overlapped);
      (void)GetOverlappedResult(pipe, &overlapped, &written, TRUE);  // the cancelled I/O must finish before `overlapped` goes away
      error = "engine stdin is not writable";
      ok = false;
      break;
    }
    if (!GetOverlappedResult(pipe, &overlapped, &written, FALSE) || written == 0) {
      error = "engine stdin write failed";
      ok = false;
      break;
    }
    offset += written;
  }
  CloseHandle(event);
  return ok;
```

Header (Windows): `std::wstring stdin_pipe_name_;` and

```cpp
#if defined(_WIN32)
  // Test seams (spec N6, R1): the stdin pipe's name and the frontend's end of it.
  [[nodiscard]] const std::wstring& stdin_pipe_name_for_tests() const noexcept { return stdin_pipe_name_; }
  [[nodiscard]] void* stdin_handle_for_tests() const noexcept { return stdin_write_; }
#endif
```

CMake (in the `if(WIN32)` block at lines 255-259): `target_link_libraries(secure_frontend_engine_stdio_process PUBLIC advapi32 bcrypt)`. Update `docs/security/engine-protocol-limits.md` row "Windows stdin writes" in Task 28 (K6).

- [ ] **Step 4: GREEN locally** — Linux `ctest --preset linux-gcc` unchanged: 100%, total unchanged from Task 21 (all three cases are Windows-only). Cross: `-Werror` clean; record wine for the three W cases; wine ctest total unchanged (labeled).

- [ ] **Step 5: Handoff** — delete the "The default DACL of the Windows stdin named pipe is used" Known-gaps bullet; add:

```markdown
| N6, R1, K2 | Windows stdin pipe: protected DACL with one ACE for the current user, `\\.\pipe\secure_frontend-<pid>-<128-bit random>`, single instance, remote clients rejected, `GetNamedPipeClientProcessId` must be our pid; frontend end `FILE_FLAG_OVERLAPPED`, 64 KiB chunks, 1 s no-progress deadline then `CancelIoEx` → write failure → `killed_by_supervisor(write_timeout)`. | `proc02_stdin_pipe_dacl_is_owner_only`, `proc03_windows_pipe_second_open_fails`, `proc04_windows_write_timeout_kills_engine` (W; ~280 KB GEN line, 2 s generation timeout, CTest timeout 300 s) | Linux <recorded line, unchanged>; wine: <recorded>; **authoritative in MSYS2 — pending push** |
```

- [ ] **Step 6: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "windows launch: owner-only random stdin pipe, verified client, overlapped writes with a deadline (N6, R1, K2)" \
  -m "N6: proc02_stdin_pipe_dacl_is_owner_only. R1: proc03_windows_pipe_second_open_fails. K2: proc04_windows_write_timeout_kills_engine. All W, authoritative in the MSYS2 job." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 23: Argv round-trips literally — Windows twin and end-to-end through the server (A1, A2)

**Files:**
- Modify: `secure_frontend/tests/engine_child_test.cpp` (A1, W), `secure_frontend/tests/server_test.cpp` (A2, B), `secure_frontend/CMakeLists.txt`
- Modify: `docs/AGENT_HANDOFF.md` (A1/A2 row)

**Interfaces:**
- Consumes: portable `ARGV` dump and `--echo-cmdline`, `--dump-file` (Tasks 1–2). Runs before the SP1 merge against the SP0 server (the engine is spawned at startup and writes `--dump-file` before READY, so no supervisor is needed); Task 19 later extends the A2 case with `--gpu`.
- Produces: no production change (spec A1 fix "make ARGV dump portable" landed in Task 2; A2 "—"): both tests must pass on first run; a failure is a quoting defect — stop and report.

- [ ] **Step 1: Write the tests**

`engine_child_test.cpp` (Windows; append to the `SECURE_FRONTEND_CHILD_WINDOWS_CASES` line and SP1's `run_cases` list under `#if defined(_WIN32)`):

```cpp
#if defined(_WIN32)
void proc01_windows_argv_round_trip() {
  const std::vector<std::string> tricky{"a \"quoted\" word", "C:\\path with space\\", "trailing\\", "back\\\\slashes\\\"",
                                        "%PATH%",           "^&|<>()",               "line\nbreak", "\xE2\x9C\x93 \xF0\x9F\x98\x80",
                                        "",                 "-flag=value"};
  std::vector<std::string> arguments{"--echo-cmdline"};
  arguments.insert(arguments.end(), tricky.begin(), tricky.end());
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = arguments, .environment = {}}).has_value(), "start");
  expect(child.read_line(5s).line.rfind("CMDLINE ", 0) == 0, "the raw command line is reported first");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  const auto dump = test_support::request_dump(child, 5s);
  std::vector<std::string> expected{test_support::utf8_path(test_support::fake_engine_path()), "--serve"};
  expected.insert(expected.end(), arguments.begin(), arguments.end());
  expect(dump && dump->argv == expected, "CommandLineToArgvW in the child yields exactly the argv the frontend was given");
  (void)child.terminate(250ms);
}
#endif
```

`server_test.cpp` (B; add `#include "support/fake_engine_dump.hpp"`):

```cpp
void proc01_server_passes_literal_argv() {
  const auto dir = test_support::make_temp_dir("a2");
  const auto dump_file = dir / "dump.txt";
  const std::string weird = "--weird=a b;$(touch x)|&`\"q\"'\\ \xE2\x9C\x93";
  test_support::ServerOptions options;
  options.engine_args = {"--dump-file=" + test_support::utf8_path(dump_file), weird, "--max-context", "8192"};
  auto server = ServerProcess::start(options);
  expect(server != nullptr && server->wait_until_listening(10s), "the server must listen");
  if (!server) return;
  const auto dump = test_support::read_dump_file(dump_file, 10s);
  expect(dump.has_value(), "the engine reported its argv");
  if (dump) {
    auto model = server->root() / "models" / "fake-model.gguf";
    model.make_preferred();
    const std::vector<std::string> expected{test_support::utf8_path(test_support::fake_engine_path()), "--serve",
                                            "--dump-file=" + test_support::utf8_path(dump_file), weird, "--max-context", "8192",
                                            "--native", test_support::utf8_path(model)};
    expect(dump->argv == expected, "every value arrives literally as one argv entry, in order, with no shell");
  }
#if !defined(_WIN32)
  expect(test_support::children_of(server->pid()).size() == 1U, "exactly one child process: no shell in between");
#endif
}
```

Register `proc01_server_passes_literal_argv`: append it to the `set(SECURE_FRONTEND_SERVER_CASES ...)` line and SP1's `run_cases` list in `server_test.cpp`.

- [ ] **Step 2: Run** — `ctest --preset linux-gcc 2>&1 | tail -4` → 100% `out of <Task 22 total> + 1`.

- [ ] **Step 3: Cross-compile and record wine** — `-Werror` clean; `WINEPREFIX=$PWD/out/wineprefix wine out/build/windows-llvm-mingw-cross/secure_frontend_engine_child_tests.exe --case proc01_windows_argv_round_trip; echo "exit $?"` — record; ctest wine total unchanged (both cases labeled).

- [ ] **Step 4: Handoff row**

```markdown
| A1, A2 | Windows argv twin: quotes, backslashes (incl. trailing), `%PATH%`, `^&|<>()`, newline, UTF-8, empty string round-trip through the frontend's quoting and `CommandLineToArgvW`. End to end: `--engine-arg` values with shell metacharacters and the inventory `--native` reach the engine as literal argv entries with no shell (Task 19 adds the typed `--gpu` to this case). No production change. | `proc01_windows_argv_round_trip` (W), `proc01_server_passes_literal_argv` (B) | <recorded linux line>; wine: <recorded> |
```

- [ ] **Step 5: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "test: engine argv round-trips literally, Windows twin and end to end through the server (A1, A2)" \
  -m "A1: proc01_windows_argv_round_trip (W). A2: proc01_server_passes_literal_argv." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 24: PROC-03 on Linux — other uid denied; same-uid reopen recorded as accepted risk (R2, R3)

**Files:**
- Modify: `secure_frontend/tests/engine_child_test.cpp` (two L cases), `secure_frontend/CMakeLists.txt` (link `secure_frontend_engine_process`; `SKIP_RETURN_CODE 77`)
- Modify: `.github/workflows/secure-frontend.yml` (header comment: the `sudo -n -u nobody` use; lines 42-43: env for the Linux ctest step)
- Modify: `docs/security/threat-model.md` ("Engine process isolation": PROC-03 bullet)
- Modify: `docs/AGENT_HANDOFF.md` (R2/R3 row)

**Interfaces:**
- Produces: CI env `STRATA_TEST_SECOND_UID_SUDO=1` enabling `proc03_other_uid_cannot_open_engine_fd` (uses `sudo -n -u nobody` on GitHub's Linux runners); locally it skips with a reason. **Never run it locally with the variable set** (spec §6: no second-user test outside CI without the owner).

- [ ] **Step 1: Write the tests** (append both to the `SECURE_FRONTEND_CHILD_LINUX_CASES` line and SP1's `run_cases` list under `#if !defined(_WIN32)`; includes `"secure_frontend/engine_process.hpp"`, `<cstdlib>`, `<fcntl.h>`, `<unistd.h>`):

```cpp
#if !defined(_WIN32)
std::optional<int> run_as_nobody(const std::vector<std::string>& command) {
  std::vector<std::string> arguments{"-n", "-u", "nobody"};
  arguments.insert(arguments.end(), command.begin(), command.end());
  secure_frontend::EngineProcess process;
  if (process.start({.executable = "/usr/bin/sudo", .arguments = arguments, .environment = {}}).has_value()) return std::nullopt;
  return process.wait_for(10s);
}

void proc03_other_uid_cannot_open_engine_fd() {
  const char* enabled = std::getenv("STRATA_TEST_SECOND_UID_SUDO");
  if (enabled == nullptr || std::string(enabled) != "1") {
    test_support::skip("needs a second uid: CI sets STRATA_TEST_SECOND_UID_SUDO=1 where `sudo -n -u nobody` is allowed");
  }
  if (run_as_nobody({"/bin/true"}) != 0) test_support::skip("`sudo -n -u nobody` is not permitted on this runner");
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value(), "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  const std::string target = "/proc/" + std::to_string(child.pid()) + "/fd/0";
  const auto opened = run_as_nobody({"/bin/sh", "-c", "exec 3>\"$0\"", target});
  expect(opened.has_value() && *opened != 0, "another uid cannot open the engine's stdin through /proc");
  (void)child.terminate(250ms);
}

void proc03_same_uid_reopen_is_accepted_risk() {
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value(), "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  const std::string target = "/proc/" + std::to_string(child.pid()) + "/fd/0";
  const int injected = open(target.c_str(), O_WRONLY | O_CLOEXEC);
  // Spec §3.13: the same uid CAN reopen the engine's stdin through /proc and
  // inject protocol lines. Recorded accepted risk (threat model, release
  // record): the upstream engine cannot be made non-dumpable and its protocol
  // has no nonce without engine changes. If this starts failing, a mitigation
  // landed: update the threat model and flip this test.
  expect(injected >= 0, "same-uid reopen of the engine's stdin still works (accepted risk, spec 3.13)");
  if (injected >= 0) {
    const std::string line = "DUMP\n";
    expect(write(injected, line.data(), line.size()) == static_cast<ssize_t>(line.size()), "an injected line is accepted by the pipe");
    (void)close(injected);
    const auto reply = child.read_line(5s);
    expect(reply.kind == secure_frontend::EngineReadKind::line && reply.line.rfind("FDS ", 0) == 0, "the engine acts on the injected line");
  }
  (void)child.terminate(250ms);
}
#endif
```

CMake: `target_link_libraries(secure_frontend_engine_child_tests PRIVATE secure_frontend_engine_process)`; after the child `secure_frontend_add_cases` call:

```cmake
    if(NOT WIN32)
        set_tests_properties(secure_frontend_engine_child_tests.proc03_other_uid_cannot_open_engine_fd PROPERTIES SKIP_RETURN_CODE 77)
    endif()
```

- [ ] **Step 2: Run** — `ctest --preset linux-gcc 2>&1 | tail -5` → `100% tests passed, 0 tests failed out of <Task 23 total> + 2`, with `secure_frontend_engine_child_tests.proc03_other_uid_cannot_open_engine_fd` skipped (`proc05_cgroup_kill_when_delegated` arrives in Task 16, after the SP1 merge). R2 has no fix ("—"); R3 asserts current behavior. If `proc03_same_uid_reopen_is_accepted_risk` fails here, stop and report (something on this host already blocks same-uid reopen).

- [ ] **Step 3: CI and threat model** — in `.github/workflows/secure-frontend.yml`, append to the header comment block (after the line `# runs with STRATA_NATIVE_EXPERTS=OFF, so no llama.cpp clone (FetchContent) occurs.`):

```yaml
# Privilege use, exactly: the Linux ctest step sets STRATA_TEST_SECOND_UID_SUDO=1
# so proc03_other_uid_cannot_open_engine_fd (SP2 R2) runs `sudo -n -u nobody`
# to try to open the engine's /proc/<pid>/fd/0 as another uid. Nothing else in
# this workflow uses sudo, and the test never runs it outside CI.
```

and the Linux job's ctest step (lines 42-43) becomes:

```yaml
      - run: ctest --preset ${{ matrix.preset }}
        working-directory: secure_frontend
        env:
          # PROC-03: proc03_other_uid_cannot_open_engine_fd uses `sudo -n -u nobody` (allowed on GitHub runners).
          STRATA_TEST_SECOND_UID_SUDO: "1"
```

In `docs/security/threat-model.md`, "Engine process isolation", after the bullet that starts "The child is reached only by private stdio", add:

```markdown
- PROC-03 on Linux means "other user context": another uid cannot open the
  engine's stdio (`proc03_other_uid_cannot_open_engine_fd`, run in CI). The
  **same** uid can reopen the engine's stdin through `/proc/<pid>/fd/0` and
  inject protocol lines; the upstream engine cannot be made non-dumpable after
  exec and its protocol carries no nonce without engine changes. This is an
  **accepted risk** for SP2 and must appear in the release record
  (`proc03_same_uid_reopen_is_accepted_risk` pins today's behavior). On Windows
  the stdin pipe is owner-only, single-instance and client-verified (N6, R1).
```

- [ ] **Step 4: Cross-compile** — `-Werror` clean (Linux-only code compiled out); wine unchanged.

- [ ] **Step 5: Handoff row**

```markdown
| R2, R3 | Linux PROC-03 = other user context: `nobody` cannot open `/proc/<engine>/fd/0` (CI via `sudo -n -u nobody`, env `STRATA_TEST_SECOND_UID_SUDO=1`; skipped locally). Same-uid reopen and injection is the recorded accepted risk (threat model; release record). | `proc03_other_uid_cannot_open_engine_fd` (L, CI only), `proc03_same_uid_reopen_is_accepted_risk` (L) | <recorded linux line> (1 skipped); wine unchanged |
```

- [ ] **Step 6: Commit**

```bash
git add secure_frontend .github/workflows/secure-frontend.yml docs/security/threat-model.md docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "test: PROC-03 other-uid denial in CI; same-uid reopen recorded as accepted risk (R2, R3)" \
  -m "R2: proc03_other_uid_cannot_open_engine_fd (CI only). R3: proc03_same_uid_reopen_is_accepted_risk; threat model records the accepted risk." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 6b: Server wiring — the supervisor runs the engine; `starting` → `503`; abort is a generic `503` (V2, V11, V8)

> **WAITS FOR SP1 MERGE — SP1 Task 5 replaces secure_server_main.cpp with a table-driven `kFlags` parser; re-anchor on that file.**
>
> **Mapping onto SP1:** this task adds no option, so no `kFlags` row and no schema key. In SP1's `secure_server_main.cpp` the engine is built from the validated `FrontendConfig` (`config.engine.executable`, the argument vector from `build_engine_arguments(..., config.engine.args)`, `chat_execution_limits(config)` from SP1 Task 7) by a block that starts at the `EngineStdioProcess` declaration and ends at the `std::cout.flush()` after "Secure server listening"; Step 3 replaces exactly that block. Read `options.profile` as SP1's `config.engine.profile` (mapped to `NativeChatProfile` the way SP1's file already does), `*options.engine` as `config.engine.executable`, `*options.model_id` as `config.engine.model_id.value()`, `*config` (the listener `Config`) as SP1's `listener_config(config)`, and set `supervisor_config.chat_limits = secure_frontend::chat_execution_limits(config);`. `policy.ready_timeout` stays `supervision_defaults::kReadyTimeout` until Task 29 feeds it from `limits.engine.ready_timeout_s`. The usage text lives in SP1's `server_usage()` (`src/server_cli.cpp`): Step 3's usage sentence replaces its "The server starts exactly one local --serve child ..." sentence there.

**Files:**
- Modify: `secure_frontend/src/secure_server_main.cpp` (supervisor wiring; SP0 lines `:74-77` usage and `:275-331` wiring — after the SP1 merge, the block named above) and SP1's `src/server_cli.cpp` `server_usage()` (one sentence)
- Modify: SP1's `secure_frontend/tests/support/server_fixture.hpp` and its e2e chat cases (Task 29 Step 6, applied here)
- Modify: `secure_frontend/tests/server_test.cpp` (V2, V11 and V8 cases)
- Modify: `secure_frontend/CMakeLists.txt` (`strata-secure-server` links `secure_frontend_engine_supervisor`; server cases)
- Modify: `docs/AGENT_HANDOFF.md` (V2/V11 row completed, V8 row; remove the "READY is awaited lazily" Known-gaps bullet; the SP0 Health row stays as history)

**Interfaces:**
- Consumes: everything Task 6a produced; harness (Task 3).
- Produces: the production server's engine is owned by `EngineSupervisor`; `/health` takes one `snapshot()` per request; a chat outside `ready` is an immediate generic `503`; parse-before-acquire (EXT-01).

- [ ] **Step 1: Write the failing tests**

`secure_frontend/tests/server_test.cpp` — add inside the anonymous namespace:

```cpp
void proc04_ready_is_eager_and_health_starting_is_503() {
  test_support::ServerOptions options;
  options.engine_args = {"--split-ready", "--ready-delay-ms=1500"};
  auto server = ServerProcess::start(options);
  expect(server != nullptr && server->wait_until_listening(10s), "the server must listen");
  if (!server) return;
  const auto starting = server->get("/health");
  expect(starting && starting->status == 503 && starting->body == test_support::kHealthStarting,
         "while the model loads /health is 503 with engine=starting, saw: " + describe(starting));
  expect(server->wait_for_health(test_support::kHealthReady, 10s), "READY is consumed with no request: /health becomes 200 ready");
  const auto ready = server->get("/health");
  expect(ready && ready->status == 200, "ready is 200: " + describe(ready));
}

void proc04_chat_during_restart_is_generic_503() {
  test_support::ServerOptions options;
  options.engine_args = {"--hang-before-ready"};
  auto server = ServerProcess::start(options);
  expect(server != nullptr && server->wait_until_listening(10s), "the server must listen");
  if (!server) return;
  const auto started = std::chrono::steady_clock::now();
  const auto chat = server->post_chat(test_support::kChatBody, 5s);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(chat && chat->status == 503 && chat->body == test_support::kGeneric503, "a chat while starting is a generic 503: " + describe(chat));
  expect(elapsed < 1s, "it is answered at once, never after a READY wait");
}

// V8 (spec "Fix: —"): moved here from Task 10 because it needs the wired supervisor.
void proc04_abort_in_generation_is_generic_503() {
  test_support::ServerOptions options;
  options.engine_args = {"--abort-in-gen"};
  auto server = ServerProcess::start(options);
  expect(server != nullptr && server->wait_until_listening(10s) && server->wait_for_health(test_support::kHealthReady, 10s), "ready");
  if (!server) return;
  const auto chat = server->post_chat(test_support::kChatBody, 10s);
  expect(chat && chat->status == 503 && chat->body == test_support::kGeneric503,
         "an engine abort mid-generation is a generic 503 with no exit code: " + describe(chat));
  expect(server->wait_for_health(test_support::kHealthReady, 15s), "the supervisor recovers with a new generation");
  expect(server->stderr_text().find("engine generation 1 ended: abort") != std::string::npos, "only the frontend's own log names the abort");
}
```

`main` becomes:

```cpp
int main(int argc, char* argv[]) {
  return secure_frontend_test::run_cases(argc, argv,
                                         {{"harness_server_lifecycle_selftest", harness_server_lifecycle_selftest},
                                          {"proc04_ready_is_eager_and_health_starting_is_503", proc04_ready_is_eager_and_health_starting_is_503},
                                          {"proc04_chat_during_restart_is_generic_503", proc04_chat_during_restart_is_generic_503},
                                          {"proc04_abort_in_generation_is_generic_503", proc04_abort_in_generation_is_generic_503}},
                                         failures);
}
```

(The cases Tasks 14 and 23 already added — `proc05_frontend_sigkill_kills_engine` under `#if !defined(_WIN32)`, `proc01_server_passes_literal_argv` — stay in the list as they are.) In CMake append `proc04_ready_is_eager_and_health_starting_is_503 proc04_chat_during_restart_is_generic_503 proc04_abort_in_generation_is_generic_503` to the `set(SECURE_FRONTEND_SERVER_CASES ...)` line.

- [ ] **Step 2: Run to verify the failures**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R 'proc04_(ready_is_eager|chat_during_restart|abort_in_generation)' --output-on-failure 2>&1 | grep -E "FAILED|Passed"`
Expected: `proc04_ready_is_eager_and_health_starting_is_503` fails (`/health` while starting is `200 {"status":"ok","engine":"starting"}`, SP0); `proc04_chat_during_restart_is_generic_503` fails on `it is answered at once` (the chat waits for READY); `proc04_abort_in_generation_is_generic_503` fails on `the supervisor recovers` (SP0 never restarts the engine).

- [ ] **Step 3: Wire the supervisor into the server** (WAITS FOR SP1 MERGE: apply to SP1's file as the mapping note above says)

`secure_frontend/src/secure_server_main.cpp`: add `#include "secure_frontend/engine_supervisor.hpp"`. Replace usage lines 74-75 (`"The server starts exactly one local --serve child with no shell and an empty "` / `"environment allowlist. ...`) with:

```cpp
            "The server supervises one local --serve child at a time (it waits for READY, "
            "restarts a failed engine with backoff, then stops trying until it is restarted) "
            "with no shell and an empty environment allowlist. It serves authenticated GET /health, GET /v1/models, "
```

Replace lines 275-331 (from `secure_frontend::EngineStdioProcess engine;` through `std::cout.flush();`) with:

```cpp
  const auto protocol_flavor = options.profile == secure_frontend::NativeChatProfile::glm53
      ? secure_frontend::EngineProtocolFlavor::lighttransport_glm53
      : secure_frontend::EngineProtocolFlavor::strata_0143;
  secure_frontend::SupervisorConfig supervisor_config;
  supervisor_config.profile = options.profile;
  supervisor_config.flavor = protocol_flavor;
  secure_frontend::EngineLaunchPlan plan{
      .command = {.executable = *options.engine, .arguments = std::move(*engine_arguments), .environment = {}}};
  // The supervisor thread spawns, watches and restarts the engine (spec §3.1).
  secure_frontend::EngineSupervisor supervisor(supervisor_config, secure_frontend::make_stdio_process_factory(std::move(plan)));

  const std::string authority = host_header(*config);
  secure_frontend::HttpPolicy policy{.allowed_hosts = {authority}, .same_origin_origins = {"http://" + authority},
                                     .cors_allowed_origins = {}, .cors_enabled = false};
  // /health takes one supervisor snapshot per request: the router calls the
  // bool provider first, then the engine and recovery providers, once each
  // (api_router.hpp), and the listener is serial.
  secure_frontend::HealthSnapshot health = secure_frontend::health_for(secure_frontend::SupervisorState::stopped);
  const secure_frontend::ApiRouter router(
      [&supervisor, &health] {
        health = supervisor.snapshot();
        return health.healthy;
      },
      [models] { return models; }, [&health] { return health.engine; }, [&health] { return health.recovery; });
  auto server = secure_frontend::HttpSocketServer::bind(
      *config, std::move(policy), [verifier](const std::string_view supplied) { return verifier->matches(supplied); },
      [&router, &supervisor, &tokenizer, &options](const std::string_view method, const std::string_view target,
                                                     const std::string_view body) {
        if (method == "GET") return router.dispatch(method, target);
        if (method != "POST" || target != "/v1/chat/completions") return json_error(404, "Not Found");
        // Parse first: a request the frontend refuses (tools, functions, MCP,
        // stream, unknown fields) is answered with no engine or process action (EXT-01).
        const auto request = secure_frontend::parse_chat_completion_request(body);
        if (!request || request->model != *options.model_id) return json_error(400, "Bad Request");
        // Outside `ready` there is no lease: an immediate generic 503, never a wait (spec §3.4).
        const auto outcome = secure_frontend::execute_with_lease(supervisor, *request, *tokenizer);
        if (!outcome) return json_error(503, "Service Unavailable");
        const auto* completion = std::get_if<secure_frontend::ChatExecutionResult>(&*outcome);
        if (completion == nullptr) return json_error(503, "Service Unavailable");
        const auto response = secure_frontend::render_chat_completion_response(*request, completion->text);
        if (!response) return json_error(503, "Service Unavailable");
        return secure_frontend::HttpResponse{.status = 200,
                                              .headers = {{"Content-Type", "application/json; charset=utf-8"}, {"Cache-Control", "no-store"}},
                                              .body = *response};
      },
      nullptr);
  if (!server) { std::cerr << "Unable to bind secure listener.\n"; return EXIT_FAILURE; }
  supervisor.start();
  std::cout << "Secure server listening on http://" << authority << " (authenticated; Ctrl-C to stop)\n";
  std::cout.flush();
```

- [ ] **Step 4: CMake**

Append `secure_frontend_engine_supervisor` to the `target_link_libraries(strata-secure-server PRIVATE ...)` list (line 233 at `7ddcd755`). The server test binary links nothing new: it reaches the supervisor only through the server executable it launches.

- [ ] **Step 5: Run to verify it passes**

Eager READY makes a chat `503` until READY is consumed, so SP1's e2e cases that post a chat right after the listener answers now race it: apply Task 29 Step 6 (`ServerFixture::wait_engine_ready()` and its calls) as part of this task.

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc 2>&1 | tail -3`
Expected: `100% tests passed, 0 tests failed out of <Task 24 total> + 3` (new: `secure_frontend_server_tests.proc04_ready_is_eager_and_health_starting_is_503`, `...proc04_chat_during_restart_is_generic_503`, `...proc04_abort_in_generation_is_generic_503`). V8 has no fix of its own: if it fails after the wiring, stop and report.

- [ ] **Step 6: Cross-compile and run wine** — compiles with `-Werror`; `ctest --preset windows-llvm-mingw-cross` stays at the Task 24 wine total (server cases are labeled). Record.

- [ ] **Step 7: Handoff** — delete the Known-small-gaps bullet that starts "READY is awaited lazily on the first chat request", complete the V2/V11 row from Task 6a and add V8:

```markdown
| V2, V11 | The production server runs the engine through `EngineSupervisor` (Task 6a): READY is consumed eagerly; `/health` `starting` → `503` (reverses SP0's `200`, true now that READY is eager), `ready`/`generating` → `200`, optional `recovery` only with `engine=failed`; one snapshot per `/health`. A chat outside `ready` is an immediate generic `503`; parse before acquire. | `proc04_ready_is_eager_and_health_starting_is_503`, `proc04_chat_during_restart_is_generic_503` | <recorded linux line>; wine <recorded> |
| V8 | An abort mid-generation answers the client with the fixed generic `503`, logs `abort`, and the supervisor recovers. No production change of its own. | `proc04_abort_in_generation_is_generic_503` | <recorded linux line>; wine <recorded> |
```

- [ ] **Step 8: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "secure_server: the supervisor owns the engine; /health starting is 503 again (V2, V11, V8)" \
  -m "V2: secure_frontend_server_tests.proc04_ready_is_eager_and_health_starting_is_503. V11: proc04_chat_during_restart_is_generic_503. V8: proc04_abort_in_generation_is_generic_503. Re-anchored on SP1's secure_server_main.cpp." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 15: SIGTERM/SIGINT stop the frontend in order; only engines are children (C4, C7; C6 server cases)

> **Merge note from the parallel-safe run (binding):** the branch's `EngineSupervisor::shutdown_generation` already differs from the code below: it retires the instance with `retire(instance, reaped)` (ended instances are destroyed only on the supervisor thread — Task 6a/7 ruling, spec §3.1) and, when the leader was reaped and `sweep_orphans` is set, runs `sweep_orphans({}, 250 ms)` (Task 13 ruling, spec §3.7(b)). Do NOT replace it with `current_.reset()`. Merge the §3.7 EOF → SIGTERM → SIGKILL sequence and the `engine generation <n> ended: killed_by_supervisor(shutdown)` log line onto the current function, keeping both. Also carry the pre-6b items: a Lease must not outlive the supervisor; a failed SupervisorWake must make start() refuse; exceptions escaping run() go to circuit_open; SupervisorWake::notify() from signal handlers must save/restore errno; and decide CREATE_NO_WINDOW for the engine (a console-wide Ctrl-Break still reaches it — Task 17 ruling).

Runs after Task 6b (its server tests need the supervisor in `main`). Only Step 6 changes `secure_server_main.cpp`.

**Files:**
- Create: `secure_frontend/include/secure_frontend/shutdown_signal.hpp`, `secure_frontend/src/shutdown_signal.cpp`
- Modify: `secure_frontend/include/secure_frontend/http_socket_server.hpp:45-51`, `secure_frontend/src/http_socket_server.cpp:226-243` (`shutdown_listener`)
- Modify: `secure_frontend/include/secure_frontend/engine_supervisor.hpp` (`wake()`, `set_on_stopped`, members), `secure_frontend/src/engine_supervisor.cpp` (`stop_requested`, `shutdown`, `shutdown_generation`, `finish`, `set_on_stopped`, `note_shutdown_started`)
- Modify: `secure_frontend/src/secure_server_main.cpp` (signals, on-stopped hook, loop exit)
- Modify: `secure_frontend/CMakeLists.txt` (source; the `SECURE_FRONTEND_SERVER_LINUX_CASES` and `SECURE_FRONTEND_SERVER_WINDOWS_CASES` lines)
- Modify: `secure_frontend/tests/server_test.cpp` (five Linux cases; the two Windows C6 cases moved here from Task 17)
- Modify: `docs/AGENT_HANDOFF.md` (C4/C7 row; C6 row completed)

**Interfaces:**
- Produces: `install_shutdown_signals(SupervisorWake&)`, `shutdown_signalled()`, `notify_shutdown_complete()`; `HttpSocketServer::shutdown_listener()`; `EngineSupervisor::wake()`, `EngineSupervisor::set_on_stopped(std::function<void()>)`; shutdown order (spec §3.7): EOF (unless a lease is out) → wait 2 s → SIGTERM group → wait 1 s → always SIGKILL group → reap → sweep → listener shutdown → `main` returns 0; past 5 s `_exit(1)`.

- [ ] **Step 1: Write the failing tests** (`server_test.cpp`; the five POSIX cases are appended to the `SECURE_FRONTEND_SERVER_LINUX_CASES` line and the two Windows ones to the `SECURE_FRONTEND_SERVER_WINDOWS_CASES` line; all go into SP1's `run_cases` list in `main` under the matching `#if`; add `<filesystem>`, `<optional>`, `<thread>`, `<system_error>`):

```cpp
#if !defined(_WIN32)
void stops_all_descendants_on(const ServerProcess::Stop how, const std::string& name) {
  const auto dir = test_support::make_temp_dir("c4");
  const auto pidfile = dir / "grandchild.pid";
  test_support::ServerOptions options;
  options.engine_args = {"--spawn-grandchild", "--grandchild-pidfile=" + test_support::utf8_path(pidfile)};
  auto server = ServerProcess::start(options);
  expect(server != nullptr && server->wait_until_listening(10s) && server->wait_for_health(test_support::kHealthReady, 10s), name + ": ready");
  if (!server) return;
  const auto grandchild = test_support::read_pidfile(pidfile, 5s);
  const std::vector<long> before = server->descendants();
  expect(before.size() == 2U, name + ": engine and grandchild running, saw " + std::to_string(before.size()));
  const auto started = std::chrono::steady_clock::now();
  server->stop(how);
  const auto code = server->wait_exit(6s);
  expect(code == 0, name + ": the frontend exits 0, saw " + (code ? std::to_string(*code) : std::string("still running")));
  expect(std::chrono::steady_clock::now() - started <= 5s, name + ": within the 5 s bound");
  for (const long pid : before) expect(test_support::wait_until_gone(pid, 1s), name + ": descendant " + std::to_string(pid) + " is gone");
  expect(server->port_refuses(), name + ": the port refuses connections");
  if (grandchild) test_support::kill_process(*grandchild);
}

void proc05_frontend_sigterm_stops_all_descendants() { stops_all_descendants_on(ServerProcess::Stop::term, "SIGTERM"); }
void proc05_frontend_sigint_stops_all_descendants() { stops_all_descendants_on(ServerProcess::Stop::interrupt, "SIGINT"); }

void proc05_frontend_has_only_engine_children() {
  auto server = ServerProcess::start({});
  expect(server != nullptr && server->wait_until_listening(10s) && server->wait_for_health(test_support::kHealthReady, 10s), "ready");
  if (!server) return;
  const auto children = test_support::children_of(server->pid());
  expect(children.size() == 1U, "the frontend has exactly one child, saw " + std::to_string(children.size()));
  if (children.size() == 1U) {
    std::error_code error;
    const auto exe = std::filesystem::read_symlink("/proc/" + std::to_string(children.front()) + "/exe", error);
    expect(!error && exe == std::filesystem::canonical(test_support::fake_engine_path()), "and it is the engine, started directly (no shell)");
  }
}

void proc05_sigterm_during_generation_exits_cleanly() {
  test_support::ServerOptions options;
  options.engine_args = {"--hang-in-gen"};
  auto server = ServerProcess::start(options);
  expect(server != nullptr && server->wait_until_listening(10s) && server->wait_for_health(test_support::kHealthReady, 10s), "ready");
  if (!server) return;
  const auto engine = test_support::children_of(server->pid());
  std::optional<test_support::HttpResult> chat;
  std::thread client([&server, &chat] { chat = server->post_chat(test_support::kChatBody, 10s); });
  std::this_thread::sleep_for(300ms);  // the GEN is in flight and the engine never answers
  server->stop(ServerProcess::Stop::term);
  const auto code = server->wait_exit(6s);
  client.join();
  expect(code == 0, "exit 0 with a request in flight");
  expect(!chat || (chat->status == 503 && chat->body == test_support::kGeneric503),
         "the in-flight client gets a generic 503 or a closed connection, never a result: " + describe(chat));
  for (const long pid : engine) expect(test_support::wait_until_gone(pid, 1s), "no engine is left");
}

void proc05_double_sigterm_is_idempotent() {
  auto server = ServerProcess::start({});
  expect(server != nullptr && server->wait_until_listening(10s) && server->wait_for_health(test_support::kHealthReady, 10s), "ready");
  if (!server) return;
  const auto engine = test_support::children_of(server->pid());
  server->stop(ServerProcess::Stop::term);
  std::this_thread::sleep_for(50ms);
  server->stop(ServerProcess::Stop::term);
  expect(server->wait_exit(6s) == 0, "a second SIGTERM does not break the orderly shutdown");
  for (const long pid : engine) expect(test_support::wait_until_gone(pid, 1s), "no engine is left");
}
#endif
```

Windows (C6, moved from Task 17; they exercise Task 17's console group and Job flags together with this task's console handler):

```cpp
#if defined(_WIN32)
void proc05_windows_ctrl_break_stops_all_descendants() {
  const auto dir = test_support::make_temp_dir("c6");
  test_support::ServerOptions options;
  options.engine_args = {"--spawn-grandchild", "--grandchild-pidfile=" + test_support::utf8_path(dir / "grandchild.pid")};
  auto server = ServerProcess::start(options);
  expect(server != nullptr && server->wait_until_listening(10s) && server->wait_for_health(test_support::kHealthReady, 10s), "ready");
  if (!server) return;
  expect(test_support::read_pidfile(dir / "grandchild.pid", 5s).has_value(), "grandchild started");
  expect(server->active_processes() == 3U, "server, engine and grandchild are in the harness Job");
  server->stop(ServerProcess::Stop::ctrl_break);  // to the server's console group only
  const auto code = server->wait_exit(6s);
  expect(code == 0, "CTRL_BREAK runs the orderly shutdown and the frontend exits 0");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (server->active_processes() != 0U && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
  expect(server->active_processes() == 0U, "no descendant is left (Job ActiveProcesses 0)");
}

void proc05_windows_terminate_frontend_kills_job() {
  auto server = ServerProcess::start({.engine_args = {"--spawn-grandchild"}, .extra_args = {}, .extra_env = {}, .engine = {}});
  expect(server != nullptr && server->wait_until_listening(10s) && server->wait_for_health(test_support::kHealthReady, 10s), "ready");
  if (!server) return;
  const auto before = server->descendants();
  expect(before.size() == 2U, "engine and grandchild running");
  server->stop(ServerProcess::Stop::kill);  // TerminateProcess: no handler runs
  (void)server->wait_exit(5s);
  for (const long pid : before) expect(test_support::wait_until_gone(pid, 2s), "the engine Job (kill-on-close) takes every descendant down");
}
#endif
```

- [ ] **Step 2: Run to verify they fail** — `ctest --preset linux-gcc -R 'proc05_(frontend_sig|frontend_has|sigterm_during|double)' --output-on-failure | grep -E "FAILED|Passed"`. Expected: the four signal cases fail with `exits 0, saw 143` / `saw 130` (default signal death). `proc05_frontend_has_only_engine_children` has no fix (spec C7 "—") and passes; if it fails, stop and report.

- [ ] **Step 3: Signal routing** — `secure_frontend/include/secure_frontend/shutdown_signal.hpp`:

```cpp
#pragma once

#include "secure_frontend/supervisor_wake.hpp"

namespace secure_frontend {

// Spec §3.7: SIGTERM/SIGINT (POSIX) and CTRL_C/CTRL_BREAK/CTRL_CLOSE (Windows
// console) set a process-wide flag and fire `wake`, so the supervisor thread
// runs the orderly shutdown. Install once, after the supervisor exists.
void install_shutdown_signals(SupervisorWake& wake) noexcept;
[[nodiscard]] bool shutdown_signalled() noexcept;
// Windows: lets a console handler that is holding the process open return.
void notify_shutdown_complete() noexcept;

}  // namespace secure_frontend
```

`secure_frontend/src/shutdown_signal.cpp`:

```cpp
#include "secure_frontend/shutdown_signal.hpp"

#include <atomic>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <cerrno>
#include <csignal>
#endif

namespace secure_frontend {
namespace {

std::atomic<SupervisorWake*> g_wake{nullptr};

#if defined(_WIN32)
std::atomic<bool> g_signalled{false};
HANDLE g_done = nullptr;

BOOL WINAPI on_console_event(const DWORD type) {
  if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT) return FALSE;
  g_signalled.store(true);
  if (auto* wake = g_wake.load()) wake->notify();
  // Keep the process alive while the orderly shutdown runs (CTRL_CLOSE ends it when we return).
  if (g_done != nullptr) (void)WaitForSingleObject(g_done, 5000);
  return TRUE;
}
#else
volatile std::sig_atomic_t g_signalled = 0;

void on_signal(int) {
  const int saved_errno = errno;
  g_signalled = 1;
  if (auto* wake = g_wake.load()) wake->notify();  // one write(2): async-signal-safe
  errno = saved_errno;
}
#endif

}  // namespace

void install_shutdown_signals(SupervisorWake& wake) noexcept {
  g_wake.store(&wake);
#if defined(_WIN32)
  if (g_done == nullptr) g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  (void)SetConsoleCtrlHandler(on_console_event, TRUE);
#else
  struct sigaction action {};
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;  // no SA_RESTART: a blocked accept() on the main thread returns EINTR
  (void)sigaction(SIGTERM, &action, nullptr);
  (void)sigaction(SIGINT, &action, nullptr);
#endif
}

bool shutdown_signalled() noexcept {
#if defined(_WIN32)
  return g_signalled.load();
#else
  return g_signalled != 0;
#endif
}

void notify_shutdown_complete() noexcept {
#if defined(_WIN32)
  if (g_done != nullptr) (void)SetEvent(g_done);
#endif
}

}  // namespace secure_frontend
```

Add `src/shutdown_signal.cpp` to `secure_frontend_engine_supervisor`.

- [ ] **Step 4: Listener shutdown** — in `http_socket_server.hpp` after `listener_failed()`:

```cpp
  // Thread-safe. Makes a blocked serve_one() return and every later call fail
  // with listener_failed(): POSIX shutdown(2) wakes accept() and stops new
  // connections; Windows closes the socket. Used by the orderly shutdown (SP2 C4).
  void shutdown_listener() noexcept;
```

In `http_socket_server.cpp` add `#include <atomic>`; in `State` add `std::atomic<bool> listener_closed{false};` and make the destructor:

```cpp
  ~State() {
#ifdef _WIN32
    if (!listener_closed.exchange(true)) close_socket(socket);
    if (winsock_started) WSACleanup();
#else
    close_socket(socket);
#endif
  }
```

and add after `listener_failed()`:

```cpp
void HttpSocketServer::shutdown_listener() noexcept {
  if (!state_) return;
#ifdef _WIN32
  if (!state_->listener_closed.exchange(true)) (void)closesocket(state_->socket);
#else
  state_->listener_closed.store(true);
  (void)::shutdown(state_->socket, SHUT_RDWR);
#endif
}
```

- [ ] **Step 5: Ordered shutdown in the supervisor** — header: add to the public section

```cpp
  // The wake object install_shutdown_signals() fires.
  [[nodiscard]] SupervisorWake& wake() noexcept { return wake_; }
  // Called on the supervisor thread after every engine process is gone (main
  // passes HttpSocketServer::shutdown_listener). Set before start().
  void set_on_stopped(std::function<void()> on_stopped);
```

private: `void note_shutdown_started() noexcept;`, members `std::function<void()> on_stopped_;`, `bool main_acknowledged_{false};`, `std::chrono::steady_clock::time_point shutdown_deadline_{};`. Source (`#include "secure_frontend/shutdown_signal.hpp"`):

```cpp
void EngineSupervisor::set_on_stopped(std::function<void()> on_stopped) { on_stopped_ = std::move(on_stopped); }

bool EngineSupervisor::stop_requested() const noexcept { return shutdown_requested_.load() || shutdown_signalled(); }

void EngineSupervisor::note_shutdown_started() noexcept {
  if (shutdown_deadline_ == std::chrono::steady_clock::time_point{}) {
    shutdown_deadline_ = std::chrono::steady_clock::now() + supervision_defaults::kShutdownBound;
  }
}

void EngineSupervisor::shutdown(const std::chrono::milliseconds bound) {
  shutdown_requested_.store(true);
  wake_.notify();
  if (!thread_.joinable()) return;
  {
    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(lock, bound, [this] { return state_ == SupervisorState::stopped; })) {
      std::_Exit(EXIT_FAILURE);  // spec §3.7: past the bound the frontend exits 1
    }
    main_acknowledged_ = true;
    changed_.notify_all();
  }
  thread_.join();
}

// Spec §3.7: EOF first (the engine exits 0 on EOF) unless a request still uses
// the engine's stdin; then SIGTERM to the group; then always SIGKILL the group.
void EngineSupervisor::shutdown_generation(const std::shared_ptr<EngineInstance>& instance) {
  note_shutdown_started();
  bool lease_outstanding = false;
  {
    std::lock_guard lock(mutex_);
    lease_outstanding = state_ == SupervisorState::generating;
    state_ = SupervisorState::shutting_down;
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
  bool ended = false;
  if (!lease_outstanding) {
    process.close_stdin();
    ended = wait_reaped(config_.shutdown_stdin_wait);
  }
  if (!ended) {
    process.signal_group_terminate();
    ended = wait_reaped(config_.shutdown_term_wait);
  }
  process.kill_group();
  if (!ended) ended = wait_reaped(config_.shutdown_term_wait);
  if (ended) {
    process.release_resources(std::chrono::milliseconds(500));
    log_end(instance->number, describe_exit(classify_exit(*process.try_reap(), KillReason::shutdown)));
  } else {
    log_end(instance->number, "unreaped");
  }
  std::lock_guard lock(mutex_);
  current_.reset();
}

void EngineSupervisor::finish() {
  note_shutdown_started();
  set_state(SupervisorState::shutting_down, nullptr);
  if (config_.sweep_orphans) (void)sweep_orphans({}, std::chrono::milliseconds(500));
  if (on_stopped_) on_stopped_();
  set_state(SupervisorState::stopped);
  notify_shutdown_complete();
  // Spec §3.7: main must finish within the bound; otherwise the frontend exits 1.
  std::unique_lock lock(mutex_);
  if (!changed_.wait_until(lock, shutdown_deadline_, [this] { return main_acknowledged_; })) std::_Exit(EXIT_FAILURE);
}
```

- [ ] **Step 6: `main` stops in order**

> **WAITS FOR SP1 MERGE — SP1 Task 5 replaces secure_server_main.cpp with a table-driven `kFlags` parser; re-anchor on that file.**
>
> **Mapping onto SP1:** no option is added, so no `kFlags` row and no schema key. In SP1's `secure_server_main.cpp`, Task 6b already put `supervisor.start();` after the listener is bound and the serve loop at the end of the serving function; apply the two replacements below to those same statements. Where SP1's loop returns a status from a helper instead of `run`, keep its structure and only add the `supervisor.stop_requested()` exit and the `supervisor.shutdown(kShutdownBound)` calls shown here (Task 29 later reads `kShutdownBound` from SP1's `config_defaults.hpp`).

In `secure_server_main.cpp` add `#include "secure_frontend/shutdown_signal.hpp"`; replace `supervisor.start();` with:

```cpp
  supervisor.set_on_stopped([&server] { server->shutdown_listener(); });
  secure_frontend::install_shutdown_signals(supervisor.wake());
  supervisor.start();
```

and replace the serve loop (the final `for (;;) { ... }` of `run`) with:

```cpp
  for (;;) {
    std::string error;
    const bool served = server->serve_one(&error);
    if (supervisor.stop_requested()) break;
    if (!served && server->listener_failed()) {
      std::cerr << "listener stopped: " << error << '\n';
      supervisor.shutdown(secure_frontend::supervision_defaults::kShutdownBound);
      return EXIT_FAILURE;
    }
    // Any other failure was one client's; keep serving.
  }
  supervisor.shutdown(secure_frontend::supervision_defaults::kShutdownBound);
  return EXIT_SUCCESS;
```

Register the cases: the Task 3 lines become `set(SECURE_FRONTEND_SERVER_LINUX_CASES proc05_frontend_sigkill_kills_engine proc05_frontend_sigterm_stops_all_descendants proc05_frontend_sigint_stops_all_descendants proc05_frontend_has_only_engine_children proc05_sigterm_during_generation_exits_cleanly proc05_double_sigterm_is_idempotent)` and `set(SECURE_FRONTEND_SERVER_WINDOWS_CASES proc05_windows_ctrl_break_stops_all_descendants proc05_windows_terminate_frontend_kills_job)` (plus any names later tasks already appended), and the same names go into SP1's `run_cases` list under `#if !defined(_WIN32)` / `#if defined(_WIN32)`.

- [ ] **Step 7: Run to verify they pass** — `ctest --preset linux-gcc 2>&1 | tail -3` → 100% `out of <Task 6b total> + 5`.

- [ ] **Step 8: Cross-compile** — `-Werror` clean (console handler compiles); wine total unchanged (the Windows server cases are labeled); `WINEPREFIX=$PWD/out/wineprefix wine out/build/windows-llvm-mingw-cross/secure_frontend_server_tests.exe --case proc05_windows_ctrl_break_stops_all_descendants; echo "exit $?"` — record (wine's console events are not authoritative; MSYS2 is).

- [ ] **Step 9: Handoff row**

```markdown
| C4, C7 | SIGTERM/SIGINT → self-pipe → supervisor: EOF, 2 s, SIGTERM group, 1 s, SIGKILL group always, reap, sweep, `shutdown_listener()`; `main` returns 0; past 5 s `_exit(1)`. The production binary's only children are engines. | `proc05_frontend_sigterm_stops_all_descendants`, `proc05_frontend_sigint_stops_all_descendants`, `proc05_frontend_has_only_engine_children` (C7), `proc05_sigterm_during_generation_exits_cleanly` (Review Focus 3), `proc05_double_sigterm_is_idempotent` (Review Focus 5) | <recorded linux line>; wine <recorded> |
```

and complete Task 17's C6 row with `proc05_windows_ctrl_break_stops_all_descendants`, `proc05_windows_terminate_frontend_kills_job` (W; wine: <recorded>; **authoritative in MSYS2 — pending push**).

- [ ] **Step 10: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "secure_frontend: SIGTERM/SIGINT run an orderly shutdown that leaves no descendant (C4, C7)" \
  -m "C4: proc05_frontend_sigterm_stops_all_descendants, proc05_frontend_sigint_stops_all_descendants. C7: proc05_frontend_has_only_engine_children. Review Focus: proc05_sigterm_during_generation_exits_cleanly, proc05_double_sigterm_is_idempotent. C6 (W): proc05_windows_ctrl_break_stops_all_descendants, proc05_windows_terminate_frontend_kills_job." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 16: cgroup v2 `cgroup.kill` when a delegated cgroup is detected (C5)

> **WAITS FOR SP1 MERGE — SP1 Task 5 replaces secure_server_main.cpp with a table-driven `kFlags` parser; re-anchor on that file.**
>
> **Mapping onto SP1:** the only `secure_server_main.cpp` change is one statement, `plan.use_delegated_cgroup = true;`, placed in SP1's file where Task 6b builds the `EngineLaunchPlan` (before the `EngineSupervisor` is constructed). Delegated-cgroup use is detected, not configured, so there is no `kFlags` row and no schema key. Everything else in this task (`cgroup_scope`, `posix_child_launch.hpp`, the factory, the tests) is independent of SP1.

**Files:**
- Create: `secure_frontend/include/secure_frontend/cgroup_scope.hpp`, `secure_frontend/src/cgroup_scope.cpp`
- Modify: `secure_frontend/include/secure_frontend/engine_process.hpp:13-17` (`EngineCommand::cgroup_procs`)
- Modify: `secure_frontend/src/posix_child_launch.hpp` (`PreparedExec::cgroup_fd`, `prepare` returns `bool`, `exec_child` joins the cgroup first)
- Modify: `secure_frontend/src/engine_stdio_process.cpp`, `secure_frontend/src/engine_process.cpp` (check `prepare`; close the parent's copy of the fd), `secure_frontend/include/secure_frontend/engine_stdio_process.hpp` (POSIX member `std::optional<std::filesystem::path> cgroup_dir_;`)
- Modify: `secure_frontend/include/secure_frontend/engine_supervisor.hpp` (`EngineLaunchPlan::use_delegated_cgroup`), `secure_frontend/src/engine_supervisor.cpp` (factory)
- Modify: `secure_frontend/src/secure_server_main.cpp` (`plan.use_delegated_cgroup = true;`)
- Modify: `secure_frontend/tests/engine_supervisor_test.cpp`, `secure_frontend/tests/server_test.cpp` (C3 asserts the grandchild when delegated), `secure_frontend/CMakeLists.txt` (source in `secure_frontend_engine_stdio_process`; case; `SKIP_RETURN_CODE`; `secure_frontend_server_tests` links `secure_frontend_engine_stdio_process`)
- Modify: `docs/AGENT_HANDOFF.md` (C5 row)

**Interfaces:**
- Produces: `EngineCgroups::detect()` (in `include/secure_frontend/cgroup_scope.hpp`; rejects the root cgroup, an unwritable `cgroup.procs`, a cgroup whose `cgroup.subtree_control` is non-empty — the cgroup v2 no-internal-process rule — and kernels without `cgroup.kill`), `EngineCgroups::prepare_generation(n)` → `std::optional<path>` (the child cgroup's `cgroup.procs`), `EngineCgroups::root()`, `kill_cgroup(dir)`, `remove_cgroup(dir, bound)`; `EngineCommand::cgroup_procs{}`; `EngineLaunchPlan::use_delegated_cgroup{false}`.

- [ ] **Step 1: Write the test** (append to the `SECURE_FRONTEND_SUPERVISOR_LINUX_CASES` line and SP1's `run_cases` list under `#if !defined(_WIN32)`; after the supervisor `secure_frontend_add_cases` calls add
`if(NOT WIN32)` / `set_tests_properties(secure_frontend_engine_supervisor_tests.proc05_cgroup_kill_when_delegated PROPERTIES SKIP_RETURN_CODE 77)` / `endif()`):

```cpp
#if !defined(_WIN32)
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
  supervisor.start();
  expect(test_support::wait_generation_ready(supervisor, 1U, 10s), "ready");
  const auto grandchild = test_support::read_pidfile(pidfile, 5s);
  expect(grandchild.has_value(), "grandchild started");
  {
    auto lease = supervisor.acquire();
    if (lease) supervisor.report_fatal(*lease, secure_frontend::KillReason::protocol_failed);
  }
  expect(supervisor.wait_for_state(SupervisorState::circuit_open, 5s), "the generation ended");
  if (grandchild) {
    expect(test_support::wait_until_gone(*grandchild, 2s), "cgroup.kill ends a setsid grandchild with no sweep");
    test_support::kill_process(*grandchild);
  }
  expect(!std::filesystem::exists(cgroups->root() / "strata-engine-1"), "the generation's cgroup is removed");
  supervisor.shutdown(5s);
}
#endif
```

In `server_test.cpp` replace the `if (grandchild) { ... }` block of `proc05_frontend_sigkill_kills_engine` with:

```cpp
  if (grandchild) {
    if (secure_frontend::EngineCgroups::detect()) {
      expect(test_support::wait_until_gone(*grandchild, 2s), "with a delegated cgroup the engine's descendants die too");
    } else {
      // Spec §3.7 residual risk: without a delegated cgroup the engine's own
      // descendants survive a frontend SIGKILL. Logged, not asserted.
      std::cout << "residual risk (spec 3.7): grandchild " << (test_support::wait_until_gone(*grandchild, 1s) ? "gone" : "survived")
                << " after a frontend SIGKILL without a delegated cgroup\n";
    }
    test_support::kill_process(*grandchild);
  }
```

(`server_test.cpp` includes `"secure_frontend/cgroup_scope.hpp"`; in CMake add `target_link_libraries(secure_frontend_server_tests PRIVATE secure_frontend_engine_stdio_process)` after its `secure_frontend_test_paths` line.)

- [ ] **Step 2: Run** — `cmake --build --preset linux-gcc 2>&1 | grep -m1 error` → `cgroup_scope.hpp: No such file or directory`. After Step 3 on this dev host the case prints `SKIP: no delegated cgroup v2 ...` (record it). The RED/GREEN for C5 is observable only inside a delegated scope; if the owner approves running a user scope, the command is `systemd-run --user --scope -p Delegate=yes ctest --preset linux-gcc -R proc05_cgroup --output-on-failure` (no root needed) — do not run it without that approval.

- [ ] **Step 3: Implement**

`secure_frontend/include/secure_frontend/cgroup_scope.hpp`:

```cpp
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace secure_frontend {

// Linux cgroup v2 (spec §3.7d): used only when this process already runs in a
// delegated cgroup it can write to (systemd `Delegate=yes`, `systemd-run --user
// --scope`); never created with privileges. Off everywhere else.
class EngineCgroups final {
 public:
  // nullopt unless /proc/self/cgroup names a v2 cgroup whose cgroup.procs this
  // process can write and below which it can create a child with cgroup.kill (5.14+).
  [[nodiscard]] static std::optional<EngineCgroups> detect();
  // Creates <root>/strata-engine-<generation> (0700) and returns its cgroup.procs.
  [[nodiscard]] std::optional<std::filesystem::path> prepare_generation(std::uint64_t generation) const;
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

 private:
  explicit EngineCgroups(std::filesystem::path root) : root_(std::move(root)) {}
  std::filesystem::path root_;
};

// Writes "1" to <directory>/cgroup.kill: every member dies, escapes included.
bool kill_cgroup(const std::filesystem::path& directory) noexcept;
// Waits up to `bound` for "populated 0" in cgroup.events, then removes the directory.
bool remove_cgroup(const std::filesystem::path& directory, std::chrono::milliseconds bound) noexcept;

}  // namespace secure_frontend
```

`secure_frontend/src/cgroup_scope.cpp`:

```cpp
#include "secure_frontend/cgroup_scope.hpp"

#include "secure_frontend/deadline.hpp"

#include <fstream>
#include <string>
#include <thread>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace secure_frontend {

std::optional<EngineCgroups> EngineCgroups::detect() {
#if defined(__linux__)
  std::ifstream input("/proc/self/cgroup");
  std::string line;
  std::optional<std::string> relative;
  while (std::getline(input, line)) {
    if (line.rfind("0::", 0) == 0) relative = line.substr(3);
  }
  if (!relative || relative->size() < 2U || relative->front() != '/') return std::nullopt;  // "/" is the root cgroup: never ours
  const std::filesystem::path root = std::filesystem::path("/sys/fs/cgroup") / relative->substr(1);
  if (access((root / "cgroup.procs").c_str(), W_OK) != 0) return std::nullopt;
  // cgroup v2 "no internal processes": a cgroup that already enables
  // controllers for its children (non-empty cgroup.subtree_control) cannot
  // also hold this process next to per-generation child cgroups. Not ours to use.
  {
    std::ifstream subtree(root / "cgroup.subtree_control");
    if (!subtree) return std::nullopt;
    std::string controllers;
    std::getline(subtree, controllers);
    if (controllers.find_first_not_of(" \t") != std::string::npos) return std::nullopt;
  }
  const auto probe = root / ("strata-probe-" + std::to_string(getpid()));
  if (mkdir(probe.c_str(), 0700) != 0) return std::nullopt;
  const bool has_kill = access((probe / "cgroup.kill").c_str(), W_OK) == 0;
  (void)rmdir(probe.c_str());
  if (!has_kill) return std::nullopt;
  return EngineCgroups(root);
#else
  return std::nullopt;
#endif
}

std::optional<std::filesystem::path> EngineCgroups::prepare_generation(const std::uint64_t generation) const {
#if defined(__linux__)
  const auto directory = root_ / ("strata-engine-" + std::to_string(generation));
  if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) return std::nullopt;
  return directory / "cgroup.procs";
#else
  (void)generation;
  return std::nullopt;
#endif
}

bool kill_cgroup(const std::filesystem::path& directory) noexcept {
#if defined(__linux__)
  const int fd = open((directory / "cgroup.kill").c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool written = write(fd, "1", 1) == 1;
  (void)close(fd);
  return written;
#else
  (void)directory;
  return false;
#endif
}

bool remove_cgroup(const std::filesystem::path& directory, const std::chrono::milliseconds bound) noexcept {
#if defined(__linux__)
  const auto deadline = deadline_after(bound);
  for (;;) {
    std::ifstream events(directory / "cgroup.events");
    std::string key;
    std::string value;
    bool populated = true;
    while (events >> key >> value) {
      if (key == "populated") populated = value != "0";
    }
    if (!populated) return rmdir(directory.c_str()) == 0;
    if (remaining_until(deadline).count() == 0) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
#else
  (void)directory;
  (void)bound;
  return false;
#endif
}

}  // namespace secure_frontend
```

(`errno` needs `<cerrno>`; add it.) Add `src/cgroup_scope.cpp` to `secure_frontend_engine_stdio_process`. Add `#include "secure_frontend/cgroup_scope.hpp"` to `engine_stdio_process.cpp`, `engine_supervisor.cpp` and `tests/engine_supervisor_test.cpp`.

`engine_process.hpp`: `EngineCommand` gains, after `environment`:

```cpp
  // Linux, delegated cgroup v2 only: the child writes itself into this
  // cgroup.procs before execve, so even its escaping descendants stay inside.
  // `{}`: existing designated initializers that omit it keep compiling (-Werror).
  std::optional<std::filesystem::path> cgroup_procs{};
```

`posix_child_launch.hpp`: `PreparedExec` gains `int cgroup_fd{-1};` and a destructor `~PreparedExec() { if (cgroup_fd >= 0) (void)close(cgroup_fd); }` (the forked child never runs it: it execs or `_exit`s). `prepare` returns `bool` and ends with:

```cpp
  if (command.cgroup_procs) {
    out.cgroup_fd = open(command.cgroup_procs->c_str(), O_WRONLY | O_CLOEXEC);
    if (out.cgroup_fd < 0) return false;
  }
  return true;
```

(change the signature to `[[nodiscard]] inline bool prepare(PreparedExec& out, const EngineCommand& command, const bool insert_serve)`; a `prepare_generation` that returned nullopt leaves `cgroup_procs` empty and the generation runs without a cgroup).

`exec_child`, first statement after the PDEATHSIG block:

```cpp
  // Spec §3.7(d): join the generation's delegated cgroup before anything runs.
  if (prepared.cgroup_fd >= 0 && write(prepared.cgroup_fd, "0", 1) != 1) fail(report_fd);
```

Callers. `engine_stdio_process.cpp`, POSIX `start`, line 161 becomes:

```cpp
  if (!posix_launch::prepare(prepared, command, /*insert_serve=*/true)) {
    return ProcessError{ProcessErrorCode::platform_failure, "engine cgroup setup failed"};
  }
```

`engine_process.cpp`, POSIX `start`, line 210 becomes:

```cpp
  if (!posix_launch::prepare(prepared, command, /*insert_serve=*/false)) {
    return ProcessError{ProcessErrorCode::platform_failure, "engine cgroup setup failed"};
  }
```

(Each `prepared` owns the cgroup fd; its destructor closes the parent's copy when `start` returns. The forked child never runs it: it execs or `_exit`s.) In `EngineStdioProcess::start` (POSIX) the success line (Task 8's `process_id_ = child; process_group_ = child; pidfd_ = open_pidfd(child); stdin_write_ = input[1]; stdout_read_ = output[0]; return std::nullopt;`) becomes:

```cpp
  process_id_ = child; process_group_ = child; pidfd_ = open_pidfd(child); stdin_write_ = input[1]; stdout_read_ = output[0];
  cgroup_dir_ = command.cgroup_procs ? std::optional<std::filesystem::path>(command.cgroup_procs->parent_path()) : std::nullopt;
  return std::nullopt;
```

`kill_group` becomes:

```cpp
void EngineStdioProcess::kill_group() noexcept {
#if defined(_WIN32)
  if (job_handle_ != nullptr) (void)TerminateJobObject(static_cast<HANDLE>(job_handle_), kSupervisorKillExitCode);
#else
  if (cgroup_dir_) (void)kill_cgroup(*cgroup_dir_);  // escape-proof when delegated
  // Spec §3.7a: the process-group id stays valid while any member lives.
  if (process_group_ > 0) (void)kill(-process_group_, SIGKILL);
#endif
}
```

`release_resources` (POSIX) becomes:

```cpp
void EngineStdioProcess::release_resources(const std::chrono::milliseconds bound) noexcept {
#if !defined(_WIN32)
  if (cgroup_dir_) {
    (void)remove_cgroup(*cgroup_dir_, bound);
    cgroup_dir_.reset();
  }
  process_group_ = -1;  // never signal a group id the kernel may hand out again
#else
  (void)bound;
#endif
}
```

`engine_supervisor.hpp`: `#include "secure_frontend/cgroup_scope.hpp"`; `EngineLaunchPlan` gains `bool use_delegated_cgroup{false};  // Linux: cgroup.kill when a delegated cgroup is detected`. `make_stdio_process_factory`:

```cpp
ProcessFactory make_stdio_process_factory(EngineLaunchPlan plan) {
  std::optional<EngineCgroups> cgroups;
  if (plan.use_delegated_cgroup) cgroups = EngineCgroups::detect();
  return [plan = std::move(plan), cgroups](const std::uint64_t generation) -> SpawnResult {
    EngineCommand command = plan.command;
    if (cgroups) command.cgroup_procs = cgroups->prepare_generation(generation);
    auto process = std::make_unique<EngineStdioProcess>();
    if (const auto error = process->start(command); error.has_value()) return SpawnFailure{error->message};
    return std::unique_ptr<SupervisedProcess>(std::move(process));
  };
}
```

`secure_server_main.cpp` (WAITS FOR SP1 MERGE; SP1's file): directly after the `plan` is built and before `secure_frontend::EngineSupervisor supervisor(...)` add `plan.use_delegated_cgroup = true;`.

- [ ] **Step 4: Run** — `ctest --preset linux-gcc 2>&1 | tail -4` → `100% tests passed, 0 tests failed out of <Task 15 total> + 1` with `secure_frontend_engine_supervisor_tests.proc05_cgroup_kill_when_delegated` listed under "did not run (Skipped)" on this host.

- [ ] **Step 5: Cross-compile** — `-Werror` clean; wine total unchanged.

- [ ] **Step 6: Handoff row**

```markdown
| C5 | Delegated cgroup v2 detected (writable own cgroup with `cgroup.kill` and an empty `cgroup.subtree_control`, never created with privileges): each generation joins `strata-engine-<n>` before `execve`; cleanup writes `cgroup.kill`, waits for `populated 0`, removes it. Off unless detected. | `proc05_cgroup_kill_when_delegated` (L; SKIPPED here: no delegated cgroup), C3 test now asserts the grandchild when delegated | <recorded linux line> (1 skipped); wine unchanged |
```

- [ ] **Step 7: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "posix launch: cgroup.kill per generation when a delegated cgroup v2 is detected (C5)" \
  -m "C5: proc05_cgroup_kill_when_delegated (skipped with reason when not delegated). proc05_frontend_sigkill_kills_engine asserts the grandchild when delegated." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 18: Typed environment allowlist; `--state-dir`; env tests fixed (N1, N2, N3)

Runs after the SP1 merge (Step 4 changes `secure_server_main.cpp`; Steps 1–3 do not, but the server case `state_dir_must_be_owner_only` needs Step 4).

**Files:**
- Create: `secure_frontend/include/secure_frontend/engine_environment.hpp`, `secure_frontend/src/engine_environment.cpp`
- Modify: `secure_frontend/include/secure_frontend/engine_supervisor.hpp` (`EngineLaunchPlan::environment{}`), `secure_frontend/src/engine_supervisor.cpp` (factory renders per generation)
- Modify: `secure_frontend/include/secure_frontend/engine_stdio_process.hpp`, `secure_frontend/src/engine_stdio_process.cpp` (`set_generation_temp_dir`; `release_resources` removes `<state>/engine-tmp/<n>` after the reap)
- Modify: `secure_frontend/src/secure_server_main.cpp` (`--state-dir`, `--engine-lib-dir`, usage, environment)
- Modify: `secure_frontend/tests/support/server_harness.{hpp,cpp}` (`ServerOptions::state_dir`; passes `--state-dir`)
- Modify: `secure_frontend/tests/engine_supervisor_test.cpp` (N1), `secure_frontend/tests/engine_stdio_process_test.cpp` (N2), `secure_frontend/tests/engine_process_test.cpp` (N3; one case added to SP1's runner), `secure_frontend/tests/server_test.cpp` (Review Focus 1)
- Modify: `secure_frontend/CMakeLists.txt` (source in `secure_frontend_engine_supervisor`; cases; engine_process test cases)
- Modify: `docs/AGENT_HANDOFF.md` (N1–N3 row)

**Interfaces:**
- Produces: `EngineEnvironment{lib_dirs, temp_dir, cuda_cache_dir, hipblaslt_tuning{}, watchdog_s{}, watchdog_io_s{}}`, `WindowsSystemValues`, `EnvironmentBlock`, `render_posix_environment`, `render_windows_environment`, `render_engine_environment`, `query_windows_system_values`, `create_owner_only_directory`, `is_owner_only_directory`, `clear_directory_contents`, `kNeverForwardedEnvironment`; `EngineLaunchPlan::environment{}` (`std::optional<EngineEnvironment>`, `temp_dir` is the per-generation root: generation n gets `<temp_dir>/<n>`, removed by `release_resources()` once generation n is reaped — in `end_generation` and `shutdown_generation` — and `engine-tmp/*` is cleared at startup); `EngineStdioProcess::set_generation_temp_dir(path)`; server options `--state-dir PATH` (required; owner-only), `--engine-lib-dir PATH` (repeatable, absolute; default: the engine's directory); `test_support::ServerOptions::state_dir{}`.
- Rendered names (spec §3.9). POSIX: `PATH=/usr/bin:/bin`, `LD_LIBRARY_PATH` (if lib dirs), `TMPDIR`, `CUDA_CACHE_PATH`, optional `STRATA_HIPBLASLT_TUNING`, `STRATA_WATCHDOG_S`, `STRATA_WATCHDOG_IO_S`. Windows: `PATH=<lib dirs>;<SystemRoot>\System32`, `TEMP`, `TMP`, `CUDA_CACHE_PATH`, `SystemRoot`, `WINDIR`, `SystemDrive`, `NUMBER_OF_PROCESSORS`, the same optional three. Nothing is copied from the parent environment. GPU selection is argv (Task 19), never environment.

- [ ] **Step 1: Write the failing tests**

`engine_supervisor_test.cpp` (append to the `SECURE_FRONTEND_SUPERVISOR_PROCESS_CASES` line and SP1's `run_cases` list; includes `"secure_frontend/engine_environment.hpp"`, `"support/fake_engine_dump.hpp"`, `<cstdlib>`, `<algorithm>`, `<filesystem>`, `<thread>`):

```cpp
void set_parent_variable(const char* name, const char* value) {
#if defined(_WIN32)
  (void)_putenv_s(name, value);
#else
  (void)setenv(name, value, 1);
#endif
}

void proc02_child_env_is_exact_allowlist() {
  // A hostile parent environment: none of it may reach the engine.
  set_parent_variable("STRATA_TEST_TOKEN_ENV", "secret-token-value");
  set_parent_variable("STRATA_API_KEY", "k");
  set_parent_variable("HOME", "/home/someone");
  set_parent_variable("LD_PRELOAD", "/nonexistent/libstrata-test.so");
  set_parent_variable("HTTPS_PROXY", "http://proxy.invalid:3128");
  set_parent_variable("CUDA_VISIBLE_DEVICES", "0");
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
  expect(dump->env == expected, "the child environment is exactly the rendered allowlist");
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
  // Fix 6: the directory goes away once the generation is reaped, exactly as
  // end_generation / shutdown_generation do it (kill, reap, release_resources).
  (*process)->kill_group();
  for (int step = 0; step < 500 && !(*process)->try_reap(); ++step) std::this_thread::sleep_for(10ms);
  (*process)->release_resources(1000ms);
  expect(!std::filesystem::exists(state / "engine-tmp" / "1"), "the generation's temp directory is removed after the reap");
}
```

`engine_stdio_process_test.cpp`, in `child_receives_exact_argv_and_env` inside `if (dump) { ... }` add (N2):

```cpp
    expect(dump->env == std::vector<std::string>{"STRATA_T=1"}, "the environment is exactly the allowlist passed in (N2)");
```

`engine_process_test.cpp` (N3; SP1 Task 2 already gave it `secure_frontend_test::run_cases` and a counted `expect`): replace `run_fake_engine` (the SP0 version plus `7ddcd755`'s diagnostic line) with

```cpp
int run_fake_engine(const int argc, char** argv) {
  if (argc != 3 || std::string(argv[2]) != "literal; exit 73") return 41;
  const char* const allowed = std::getenv("STRATA_ALLOWED_TEST");
  if (allowed == nullptr || std::string(allowed) != "present") return 42;
  // The names a real parent has (HOME/PATH on POSIX; PATH, SystemRoot, USERPROFILE on Windows): none may leak.
  for (const char* name : {"HOME", "PATH", "SystemRoot", "USERPROFILE"}) {
    if (std::getenv(name) != nullptr) return 43;
  }
  return 0;
}
```

add

```cpp
void proc02_engine_process_env_checks_windows_names() {
  expect(std::getenv("PATH") != nullptr, "precondition: the parent has PATH");
#if defined(_WIN32)
  expect(std::getenv("SystemRoot") != nullptr && std::getenv("USERPROFILE") != nullptr, "precondition: the parent has SystemRoot and USERPROFILE");
#endif
  starts_without_a_shell_and_uses_only_allowlisted_environment();
}
```

and add `{"proc02_engine_process_env_checks_windows_names", proc02_engine_process_env_checks_windows_names},` as the last entry of SP1's `run_cases` list in its `main` (the two `--fake-engine`/`--fake-sleeper` role checks stay first). CMake, after SP1's `secure_frontend_add_cases(secure_frontend_engine_process_tests ...)` call: `secure_frontend_add_cases(secure_frontend_engine_process_tests proc02_engine_process_env_checks_windows_names TIMEOUT 120 LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME})`.

`server_test.cpp` (Review Focus 1; B — the POSIX variants are compiled out on Windows):

```cpp
void state_dir_must_be_owner_only() {
  const auto base = test_support::make_temp_dir("rf1");
  std::vector<std::pair<std::string, std::filesystem::path>> variants{{"missing", base / "missing"}};
#if !defined(_WIN32)
  const auto open_dir = base / "world-writable";
  std::filesystem::create_directory(open_dir);
  (void)chmod(open_dir.c_str(), 0777);
  const auto good = base / "good";
  std::filesystem::create_directory(good);
  (void)chmod(good.c_str(), 0700);
  const auto link = base / "link";
  std::filesystem::create_directory_symlink(good, link);
  variants.emplace_back("world-writable", open_dir);
  variants.emplace_back("symlink", link);
#endif
  for (const auto& [name, state] : variants) {
    const auto record = base / (name + ".record");
    test_support::ServerOptions options;
    options.state_dir = state;
    options.engine_args = {"--record-file=" + test_support::utf8_path(record)};
    auto server = ServerProcess::start(options);
    expect(server != nullptr, name + ": launched");
    if (!server) continue;
    expect(server->wait_exit(10s) == 1, name + ": the server refuses to start");
    expect(server->stderr_text().find("--state-dir must be an existing owner-only directory.") != std::string::npos, name + ": fixed error");
    expect(!std::filesystem::exists(record), name + ": no engine was spawned");
  }
}
```

(append `state_dir_must_be_owner_only` to the `set(SECURE_FRONTEND_SERVER_CASES ...)` line and SP1's `run_cases` list; include `<sys/stat.h>` under `#if !defined(_WIN32)`).

- [ ] **Step 2: Run to verify the failures** — `cmake --build --preset linux-gcc 2>&1 | grep -m1 error` → `engine_environment.hpp: No such file or directory` (and `ServerOptions` has no `state_dir`). N2 and N3 are "existing test fixed" rows: after Step 3 they pass at once; if either fails, stop and report the leak.

- [ ] **Step 3: Implement the allowlist** — `secure_frontend/include/secure_frontend/engine_environment.hpp`:

```cpp
#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// Spec §3.9: the engine's environment is a closed type rendered by the
// frontend. Every value is generated or comes from validated configuration
// (SP1); nothing is copied from the parent environment by name.
// The optional members carry `{}` so designated initializers that name only
// the required three (as the tests and the server do) compile under -Werror.
struct EngineEnvironment final {
  std::vector<std::filesystem::path> lib_dirs;              // POSIX LD_LIBRARY_PATH; Windows head of PATH
  std::filesystem::path temp_dir;                           // TMPDIR | TEMP + TMP
  std::filesystem::path cuda_cache_dir;                     // CUDA_CACHE_PATH (replaces ~/.nv/ComputeCache; HOME is unset)
  std::optional<std::filesystem::path> hipblaslt_tuning{};  // STRATA_HIPBLASLT_TUNING (under the install root)
  std::optional<unsigned> watchdog_s{};                     // STRATA_WATCHDOG_S
  std::optional<unsigned> watchdog_io_s{};                  // STRATA_WATCHDOG_IO_S
};

// Windows values from GetSystemWindowsDirectoryW / GetSystemInfo, never from the environment.
struct WindowsSystemValues final {
  std::string system_root;   // e.g. C:\Windows
  std::string system_drive;  // e.g. C:
  unsigned processors{1};
};

using EnvironmentBlock = std::map<std::string, std::string, std::less<>>;

// Names the engine never receives (spec §3.9); tests assert their absence. The
// token variable named by --token-env and every other STRATA_* are excluded too.
inline constexpr std::array<std::string_view, 16> kNeverForwardedEnvironment{
    "STRATA_API_KEY", "LD_PRELOAD",  "LD_AUDIT",    "CUDA_LAUNCH_BLOCKING", "HOME",       "USERPROFILE",
    "APPDATA",        "HTTP_PROXY",  "HTTPS_PROXY", "ALL_PROXY",            "http_proxy", "https_proxy",
    "all_proxy",      "CUDA_VISIBLE_DEVICES", "CUDA_DEVICE_ORDER", "HIP_VISIBLE_DEVICES"};

[[nodiscard]] EnvironmentBlock render_posix_environment(const EngineEnvironment& environment);
[[nodiscard]] EnvironmentBlock render_windows_environment(const EngineEnvironment& environment, const WindowsSystemValues& system);
[[nodiscard]] WindowsSystemValues query_windows_system_values();
// The native rendering (POSIX or Windows).
[[nodiscard]] EnvironmentBlock render_engine_environment(const EngineEnvironment& environment);

// POSIX: creates `path` (and parents) and makes it 0700; true only if it is
// then an owner-only directory. Windows: creates it (DACL checks are SP1's).
[[nodiscard]] bool create_owner_only_directory(const std::filesystem::path& path);
// POSIX: a real directory (not a symlink), owned by the effective uid, no group/other bits.
// Windows: a directory that is not a symlink.
[[nodiscard]] bool is_owner_only_directory(const std::filesystem::path& path);
// Removes every entry below `directory` (a symlink entry itself, never its
// target), keeping `directory`. Used at startup on <state>/engine-tmp: temp
// directories of generations that ended with a previous frontend never leak.
bool clear_directory_contents(const std::filesystem::path& directory) noexcept;

}  // namespace secure_frontend
```

`secure_frontend/src/engine_environment.cpp`:

```cpp
#include "secure_frontend/engine_environment.hpp"

#include <system_error>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace secure_frontend {
namespace {

std::string native(const std::filesystem::path& path) {
  auto copy = path;
  copy.make_preferred();
  const auto text = copy.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string join(const std::vector<std::filesystem::path>& paths, const char separator) {
  std::string joined;
  for (const auto& path : paths) {
    if (!joined.empty()) joined += separator;
    joined += native(path);
  }
  return joined;
}

void add_optional(EnvironmentBlock& block, const EngineEnvironment& environment) {
  if (environment.hipblaslt_tuning) block.emplace("STRATA_HIPBLASLT_TUNING", native(*environment.hipblaslt_tuning));
  if (environment.watchdog_s) block.emplace("STRATA_WATCHDOG_S", std::to_string(*environment.watchdog_s));
  if (environment.watchdog_io_s) block.emplace("STRATA_WATCHDOG_IO_S", std::to_string(*environment.watchdog_io_s));
}

}  // namespace

EnvironmentBlock render_posix_environment(const EngineEnvironment& environment) {
  EnvironmentBlock block;
  block.emplace("PATH", "/usr/bin:/bin");
  if (!environment.lib_dirs.empty()) block.emplace("LD_LIBRARY_PATH", join(environment.lib_dirs, ':'));
  block.emplace("TMPDIR", native(environment.temp_dir));
  block.emplace("CUDA_CACHE_PATH", native(environment.cuda_cache_dir));
  add_optional(block, environment);
  return block;
}

EnvironmentBlock render_windows_environment(const EngineEnvironment& environment, const WindowsSystemValues& system) {
  EnvironmentBlock block;
  std::string path = join(environment.lib_dirs, ';');
  if (!path.empty()) path += ';';
  path += system.system_root + "\\System32";
  block.emplace("PATH", path);
  block.emplace("TEMP", native(environment.temp_dir));
  block.emplace("TMP", native(environment.temp_dir));
  block.emplace("CUDA_CACHE_PATH", native(environment.cuda_cache_dir));
  block.emplace("SystemRoot", system.system_root);
  block.emplace("WINDIR", system.system_root);
  block.emplace("SystemDrive", system.system_drive);
  block.emplace("NUMBER_OF_PROCESSORS", std::to_string(system.processors));
  add_optional(block, environment);
  return block;
}

WindowsSystemValues query_windows_system_values() {
  WindowsSystemValues values;
#if defined(_WIN32)
  wchar_t buffer[MAX_PATH + 1]{};
  const UINT length = GetSystemWindowsDirectoryW(buffer, MAX_PATH + 1);
  if (length > 0 && length <= MAX_PATH) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    values.system_root.assign(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) (void)WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), values.system_root.data(), size, nullptr, nullptr);
  }
  values.system_drive = values.system_root.substr(0, 2);
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  values.processors = info.dwNumberOfProcessors;
#endif
  return values;
}

EnvironmentBlock render_engine_environment(const EngineEnvironment& environment) {
#if defined(_WIN32)
  return render_windows_environment(environment, query_windows_system_values());
#else
  return render_posix_environment(environment);
#endif
}

bool is_owner_only_directory(const std::filesystem::path& path) {
#if defined(_WIN32)
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  return !error && std::filesystem::is_directory(status);
#else
  struct stat info {};
  if (lstat(path.c_str(), &info) != 0) return false;
  return S_ISDIR(info.st_mode) && info.st_uid == geteuid() && (info.st_mode & 077) == 0;
#endif
}

bool create_owner_only_directory(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::create_directories(path, error);
  if (error) return false;
#if !defined(_WIN32)
  if (chmod(path.c_str(), 0700) != 0) return false;
#endif
  return is_owner_only_directory(path);
}

bool clear_directory_contents(const std::filesystem::path& directory) noexcept {
  try {
    std::error_code error;
    bool cleared = true;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
      std::error_code remove_error;
      std::filesystem::remove_all(entry.path(), remove_error);  // a symlink entry is removed, not followed
      cleared = cleared && !remove_error;
    }
    return cleared && !error;
  } catch (...) {
    return false;
  }
}

}  // namespace secure_frontend
```

Add `src/engine_environment.cpp` to `secure_frontend_engine_supervisor`.

`engine_supervisor.hpp`: `#include "secure_frontend/engine_environment.hpp"`; `EngineLaunchPlan` becomes:

```cpp
struct EngineLaunchPlan final {
  EngineCommand command;                             // command.environment is replaced when `environment` is set
  std::optional<EngineEnvironment> environment{};    // spec §3.9; temp_dir is the per-generation root
  bool use_delegated_cgroup{false};                  // Linux: cgroup.kill when a delegated cgroup is detected
};
```

`engine_supervisor.cpp`, `make_stdio_process_factory`:

```cpp
ProcessFactory make_stdio_process_factory(EngineLaunchPlan plan) {
  std::optional<EngineCgroups> cgroups;
  if (plan.use_delegated_cgroup) cgroups = EngineCgroups::detect();
  return [plan = std::move(plan), cgroups](const std::uint64_t generation) -> SpawnResult {
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
      command.environment = render_engine_environment(environment);
    }
    if (cgroups) command.cgroup_procs = cgroups->prepare_generation(generation);
    auto process = std::make_unique<EngineStdioProcess>();
    if (const auto error = process->start(command); error.has_value()) {
      std::error_code ignored;
      if (temp_dir) std::filesystem::remove_all(*temp_dir, ignored);  // nothing ran in it
      return SpawnFailure{error->message};
    }
    if (temp_dir) process->set_generation_temp_dir(*temp_dir);
    return std::unique_ptr<SupervisedProcess>(std::move(process));
  };
}
```

(add `#include <filesystem>` and `<system_error>`).

`engine_stdio_process.hpp`: add to the public section after `pid()`

```cpp
  // The generation's private temp directory (Task 18). release_resources()
  // removes it, which the supervisor calls only after the leader is reaped.
  void set_generation_temp_dir(std::filesystem::path directory) { generation_temp_dir_ = std::move(directory); }
```

and the private member `std::optional<std::filesystem::path> generation_temp_dir_;` (both platforms; `#include <filesystem>`, `<optional>`). `engine_stdio_process.cpp`: `release_resources` becomes (it keeps Task 16's cgroup cleanup):

```cpp
void EngineStdioProcess::release_resources(const std::chrono::milliseconds bound) noexcept {
#if !defined(_WIN32)
  if (cgroup_dir_) {
    (void)remove_cgroup(*cgroup_dir_, bound);
    cgroup_dir_.reset();
  }
  process_group_ = -1;  // never signal a group id the kernel may hand out again
#else
  (void)bound;
#endif
  if (generation_temp_dir_) {
    // Called after the reap (end_generation, shutdown_generation): nothing of
    // this generation can still be writing here.
    try {
      std::error_code ignored;
      std::filesystem::remove_all(*generation_temp_dir_, ignored);
    } catch (...) {
    }
    generation_temp_dir_.reset();
  }
}
```

- [ ] **Step 4: Server options and harness**

> **WAITS FOR SP1 MERGE — SP1 Task 5 replaces secure_server_main.cpp with a table-driven `kFlags` parser; re-anchor on that file.**
>
> **Mapping onto SP1:** the two options become schema keys and `kFlags` rows — `--state-dir` → `roots.state` (string, absolute; required to serve: `main` refuses without it, so SP1's config-only tests stay valid; the owner-only check below stays in `main`, run on the validated path before anything is spawned; SP1's e2e `ServerFixture` gains a 0700 `state` directory and passes it, Task 29 Step 3) and `--engine-lib-dir` → `engine.lib_dirs` (string array, `FlagKind::append`, each absolute). Apply Task 29 Step 3's `roots.state` and `engine.lib_dirs` rows (schema table, `FrontendConfig` fields, `validate()` reads, `describe()` lines, `kFlags` rows) now instead of the `Options`/`parse_options` edits shown below; the `run`-side code below then reads `config.roots.state` for `*options.state_dir` and `config.engine.lib_dirs` for `options.engine_lib_dirs`. The usage lines are generated by SP1's `server_usage()` from `kFlags`, so the usage edits below are dropped; the "fixed environment" sentence replaces the "empty environment" sentence in `server_usage()`'s closing paragraph. The CUDA cache stays `<state>/cache/cuda` here; Task 29 moves it under `roots.cache` when that key is set.

`secure_server_main.cpp`: include `"secure_frontend/engine_environment.hpp"`. `Options` gains `std::optional<std::filesystem::path> state_dir;` and `std::vector<std::filesystem::path> engine_lib_dirs;`. In `parse_options`, before the final `else`:

```cpp
    } else if (argument == "--state-dir") {
      if (options->state_dir.has_value()) { *error = "duplicate --state-dir"; return false; }
      options->state_dir = std::filesystem::path{std::move(value)};
    } else if (argument == "--engine-lib-dir") {
      std::filesystem::path directory{std::move(value)};
      if (!directory.is_absolute()) { *error = "--engine-lib-dir must be an absolute directory"; return false; }
      options->engine_lib_dirs.push_back(std::move(directory));
```

and directly after the `--profile glm53 requires at least one --engine-arg` check:

```cpp
  if (!options->state_dir) {
    *error = "--state-dir is required";
    return false;
  }
```

Usage: add after the `--engine-arg` lines

```cpp
            "  --state-dir PATH          Existing owner-only directory for engine state: the engine's\n"
            "                            working directory, temp and CUDA cache live below it\n"
            "  --engine-lib-dir PATH     Absolute library directory for the engine; repeat as needed\n"
            "                            (default: the engine executable's directory)\n"
```

and replace `"with no shell and an empty environment allowlist. It serves ..."` with `"with no shell and a fixed environment (PATH, the library path, temp and CUDA cache directories below --state-dir; on Windows also SystemRoot, WINDIR, SystemDrive, NUMBER_OF_PROCESSORS); nothing is inherited from this server's environment. It serves ..."`.

In `run`, directly before `secure_frontend::EngineLaunchPlan plan{...}`:

```cpp
  // --state-dir holds the engine's cwd, temp and CUDA cache (spec §3.9, §3.12).
  // It must already exist and be owner-only; checked before anything is spawned.
  if (!secure_frontend::is_owner_only_directory(*options.state_dir)) {
    std::cerr << "--state-dir must be an existing owner-only directory.\n";
    return EXIT_FAILURE;
  }
  const auto state_dir = std::filesystem::absolute(*options.state_dir);
  for (const auto& directory : {state_dir / "engine", state_dir / "engine-tmp", state_dir / "cache", state_dir / "cache" / "cuda"}) {
    if (!secure_frontend::create_owner_only_directory(directory)) {
      std::cerr << "Unable to prepare --state-dir.\n";
      return EXIT_FAILURE;
    }
  }
  // No engine runs yet: temp directories a previous frontend left are stale.
  if (!secure_frontend::clear_directory_contents(state_dir / "engine-tmp")) {
    std::cerr << "Unable to prepare --state-dir.\n";
    return EXIT_FAILURE;
  }
  std::vector<std::filesystem::path> lib_dirs = options.engine_lib_dirs;
  if (lib_dirs.empty()) lib_dirs.push_back(std::filesystem::absolute(*options.engine).parent_path());
```

and, after the plan is built and before `secure_frontend::EngineSupervisor supervisor(...)`: `plan.environment = secure_frontend::EngineEnvironment{.lib_dirs = std::move(lib_dirs), .temp_dir = state_dir / "engine-tmp", .cuda_cache_dir = state_dir / "cache" / "cuda"};`.

Harness: `ServerOptions` gains `std::optional<std::filesystem::path> state_dir{};  // default: <root>/state, created 0700` (the `{}` keeps the designated initializers of Tasks 17 and 27 compiling). In `ServerProcess::start`, before building `arguments`:

```cpp
  const std::filesystem::path state_dir = options.state_dir.value_or(server->root_ / "state");
  if (!options.state_dir) secure_frontend_test::make_private_directory(state_dir);  // 0700 (SP1 fixture rule)
```

and append `"--state-dir", utf8_path(state_dir)` to the `arguments` initializer.

- [ ] **Step 5: Run to verify** — `ctest --preset linux-gcc 2>&1 | tail -4` → 100% `out of <Task 16 total> + 3` (new: `secure_frontend_engine_supervisor_tests.proc02_child_env_is_exact_allowlist`, `secure_frontend_engine_process_tests.proc02_engine_process_env_checks_windows_names`, `secure_frontend_server_tests.state_dir_must_be_owner_only`; 1 skipped).

- [ ] **Step 6: Cross-compile and run wine** — `-Werror` clean; wine total unchanged (new cases labeled). Record.

- [ ] **Step 7: Handoff** — add the row; in "Current security behavior" replace the empty-allowlist sentence with the rendered list:

```markdown
| N1, N2, N3 | `EngineEnvironment` → the child gets exactly: POSIX `PATH=/usr/bin:/bin`, `LD_LIBRARY_PATH`, `TMPDIR=<state>/engine-tmp/<gen>`, `CUDA_CACHE_PATH=<state>/cache/cuda` (+ optional `STRATA_HIPBLASLT_TUNING`, `STRATA_WATCHDOG_S`, `STRATA_WATCHDOG_IO_S`); Windows `PATH=<lib dirs>;%SystemRoot%\System32`, `TEMP`, `TMP`, `CUDA_CACHE_PATH`, `SystemRoot`, `WINDIR`, `SystemDrive`, `NUMBER_OF_PROCESSORS`. Never the token variable, `STRATA_API_KEY`, other `STRATA_*`, `LD_PRELOAD`/`LD_AUDIT`, `CUDA_LAUNCH_BLOCKING`, `HOME`/`USERPROFILE`/`APPDATA`, proxies, or GPU-selection variables. New required `--state-dir` (owner-only; SP1 key `roots.state`), `--engine-lib-dir` (`engine.lib_dirs`). Per-generation temp `<state>/engine-tmp/<n>` is removed after the generation is reaped; `engine-tmp/*` is cleared at startup. **Not GPU-verified** (offline GPU box, the owner). | `proc02_child_env_is_exact_allowlist`, `child_receives_exact_argv_and_env` (now asserts env), `proc02_engine_process_env_checks_windows_names`, `state_dir_must_be_owner_only` (Review Focus 1) | <recorded linux line> (1 skipped); wine unchanged |
```

- [ ] **Step 8: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "engine launch: typed environment allowlist below an owner-only --state-dir (N1, N2, N3)" \
  -m "N1: proc02_child_env_is_exact_allowlist. N2: child_receives_exact_argv_and_env asserts the environment. N3: proc02_engine_process_env_checks_windows_names. Review Focus 1: state_dir_must_be_owner_only." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 19: GPU selection by typed `--gpu` argv (spec §3.9)

> **WAITS FOR SP1 MERGE — SP1 Task 5 replaces secure_server_main.cpp with a table-driven `kFlags` parser; re-anchor on that file.**
>
> **Mapping onto SP1:** `--gpu LIST` → schema key `engine.gpu` (string; `validate()` runs `parse_gpu_list` and reports `engine.gpu: must be a comma-separated list of distinct device indices 0-255`, never the value) plus the `kFlags` row `{"--gpu", "engine.gpu", FlagKind::string}`; `FrontendConfig::engine` gains `std::vector<unsigned> gpus`. Apply Task 29 Step 3's `engine.gpu` row now instead of the `Options`/`parse_options` edit below, and pass `config.engine.gpus` where the code below passes `options.gpus`. SP1 Task 4 moved the argument loop of `engine_arguments.cpp` into `validate_engine_user_arguments` (which `validate()` and `build_engine_arguments` both call): apply Step 3's `kOwnedFlags` change unchanged, put the GLM-branch change into that function's GLM branch (`if (name == "--serve" || name == "--gpu")`), and put the `gpus` parameter, the GLM `unsupported_mode` check and the `--gpu` rendering into `build_engine_arguments` after its `if (auto rejected = validate_engine_user_arguments(...)) return *rejected;` line. `--gpu` via `engine.args` / `--engine-arg` is then refused at validation time, before anything is spawned. The usage line comes from `kFlags`; the `--engine-arg` help sentence in `server_usage()` lists `--gpu` among the frontend-owned flags.

**Files:**
- Modify: `secure_frontend/include/secure_frontend/engine_arguments.hpp:25-45`, `secure_frontend/src/engine_arguments.cpp` (owned `--gpu`, `parse_gpu_list`, rendering)
- Modify: `secure_frontend/src/secure_server_main.cpp` (`--gpu LIST`, usage)
- Modify: `secure_frontend/tests/engine_arguments_test.cpp`, `secure_frontend/CMakeLists.txt` (CTest `strata_secure_server_rejects_malformed_gpu`)
- Modify: `docs/AGENT_HANDOFF.md` (row)

**Interfaces:**
- Produces: `parse_gpu_list(std::string_view)` → `std::optional<std::vector<unsigned>>` (decimal 0–255, no sign/space/leading zero, distinct, 1–16 entries); `build_engine_arguments(profile, selected, models_root, user_arguments, const std::vector<unsigned>& gpus = {})` renders `--gpu i,j` before `--native` (Strata profile), refuses `--gpu` from `--engine-arg` for BOTH profiles (`owned_flag`; the GLM branch refused only `--serve` before), refuses a typed GPU list for the GLM profile (`unsupported_mode`).

- [ ] **Step 1: Write the failing tests** (`engine_arguments_test.cpp`; add both to SP1's `run_cases` list in `main` and to its `secure_frontend_add_cases(secure_frontend_engine_arguments_tests ...)` call):

```cpp
void engine_args_render_typed_gpu_and_deny_gpu_passthrough() {
  const auto result = secure_frontend::build_engine_arguments(kStrata, manifest(), "/models", {"--max-context", "8192"}, {0U, 2U});
  const auto* args = std::get_if<std::vector<std::string>>(&result);
  expect(args != nullptr, "typed GPUs build");
  if (args) {
    const std::vector<std::string> expected{"--max-context", "8192", "--gpu", "0,2", "--native",
                                            std::filesystem::path("/models/qwen/model-00001-of-00004.gguf").make_preferred().string()};
    expect(*args == expected, "--gpu comes from the typed list, before --native");
  }
  for (const std::string bad : {"--gpu", "--gpu=0"}) {
    const auto r = secure_frontend::build_engine_arguments(kStrata, manifest(), "/models", {bad, "1"});
    const auto* e = std::get_if<secure_frontend::EngineArgumentsError>(&r);
    expect(e != nullptr && e->code == secure_frontend::EngineArgumentsErrorCode::owned_flag, "--gpu is owned by the frontend: " + bad);
  }
  const auto glm = secure_frontend::build_engine_arguments(kGlm, manifest(), "/models", {"--model", "/x"}, {0U});
  expect(std::holds_alternative<secure_frontend::EngineArgumentsError>(glm), "GPU selection is refused for the uncharacterized GLM CLI");
  // The GLM branch passes unknown arguments through, but --gpu is frontend-owned for every profile.
  for (const std::string bad : {"--gpu", "--gpu=0"}) {
    const auto r = secure_frontend::build_engine_arguments(kGlm, manifest(), "/models", {"--model", "/x", bad, "1"});
    const auto* e = std::get_if<secure_frontend::EngineArgumentsError>(&r);
    expect(e != nullptr && e->code == secure_frontend::EngineArgumentsErrorCode::owned_flag, "GLM: --gpu via --engine-arg is refused: " + bad);
  }
}

void gpu_list_rejects_malformed_values() {
  for (const std::string bad : {"0,0", "-1", "1,,2", " 1", "1 ", "256", "01", "", ",", "1,", "+1", "a",
                                "0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16"}) {
    expect(!secure_frontend::parse_gpu_list(bad).has_value(), "--gpu must reject '" + bad + "'");
  }
  expect(secure_frontend::parse_gpu_list("0") == std::vector<unsigned>{0U}, "a single device");
  expect(secure_frontend::parse_gpu_list("3,1,255") == std::vector<unsigned>{3U, 1U, 255U}, "order is kept");
}
```

CMake, next to the GLM CTest:

```cmake
    add_test(NAME strata_secure_server_rejects_malformed_gpu
        COMMAND strata-secure-server --models-root models --tokenizer-pack tok --engine engine
                --model-id m --token-env STRATA_UNUSED_TOKEN --gpu 0,0)
    # Matches SP1's "engine.gpu: must be ..." issue line (and the SP0 parser's "--gpu must be ..." wording).
    set_tests_properties(strata_secure_server_rejects_malformed_gpu PROPERTIES
        PASS_REGULAR_EXPRESSION "must be a comma-separated list of distinct device indices")
```

- [ ] **Step 2: Run to verify they fail** — `cmake --build --preset linux-gcc 2>&1 | grep -m1 error` → `parse_gpu_list is not a member`; after stubbing, `--gpu` from `--engine-arg` passes through today for both profiles (`GLM: --gpu via --engine-arg is refused` fails too).

- [ ] **Step 3: Implement** — `engine_arguments.hpp`: add `#include <optional>` and `<string_view>`; extend the comment ("`--gpu` is owned too: GPUs are selected only by the typed list, rendered `--gpu i,j` before `--native`; the engine sets CUDA_DEVICE_ORDER/CUDA_VISIBLE_DEVICES (or HIP) itself, generate.cpp:1765-1786; for the GLM profile a GPU list is unsupported"); declare

```cpp
// Parses the operator's --gpu value: comma-separated decimal device indices
// 0-255, no sign, spaces or leading zeros, distinct, 1-16 entries.
[[nodiscard]] std::optional<std::vector<unsigned>> parse_gpu_list(std::string_view value);

[[nodiscard]] EngineArgumentsResult build_engine_arguments(EngineArgumentsProfile profile, const ModelManifest& selected,
                                                           const std::filesystem::path& models_root,
                                                           const std::vector<std::string>& user_arguments,
                                                           const std::vector<unsigned>& gpus = {});
```

`engine_arguments.cpp`: add `<algorithm>`, `<charconv>`; `kOwnedFlags` becomes `constexpr std::array<std::string_view, 3> kOwnedFlags{"--native", "--serve", "--gpu"};`; add

```cpp
std::optional<std::vector<unsigned>> parse_gpu_list(const std::string_view value) {
  if (value.empty() || value.size() > 64U) return std::nullopt;
  std::vector<unsigned> devices;
  std::size_t begin = 0;
  for (;;) {
    const auto comma = value.find(',', begin);
    const auto item = value.substr(begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin);
    if (item.empty() || item.size() > 3U || (item.size() > 1U && item.front() == '0')) return std::nullopt;
    unsigned device = 0;
    const auto [end, error] = std::from_chars(item.data(), item.data() + item.size(), device);
    if (error != std::errc{} || end != item.data() + item.size() || device > 255U) return std::nullopt;
    if (std::find(devices.begin(), devices.end(), device) != devices.end() || devices.size() == 16U) return std::nullopt;
    devices.push_back(device);
    if (comma == std::string_view::npos) return devices;
    begin = comma + 1U;
  }
}
```

change the definition's signature to take `const std::vector<unsigned>& gpus`. In the GLM branch of the argument loop (lines 29-36) replace the `--serve` check with:

```cpp
    if (profile == EngineArgumentsProfile::lighttransport_glm53) {
      // GLM CLI is not characterized: pass through unchanged; only the
      // launcher-owned --serve and the frontend-owned --gpu (GPUs are chosen
      // only by the typed list, spec §3.9) are refused.
      if (name == "--serve" || name == "--gpu") {
        return EngineArgumentsError{EngineArgumentsErrorCode::owned_flag, "engine argument is owned by the frontend"};
      }
      arguments.push_back(argument);
      continue;
    }
```

and replace `if (profile == EngineArgumentsProfile::lighttransport_glm53) return arguments;` (line 50) with:

```cpp
  if (profile == EngineArgumentsProfile::lighttransport_glm53) {
    if (!gpus.empty()) {
      return EngineArgumentsError{EngineArgumentsErrorCode::unsupported_mode, "GPU selection is not supported by this engine profile"};
    }
    return arguments;
  }
```

and directly before `arguments.emplace_back("--native");`:

```cpp
  if (!gpus.empty()) {
    std::string list;
    for (const unsigned device : gpus) {
      if (!list.empty()) list += ',';
      list += std::to_string(device);
    }
    arguments.emplace_back("--gpu");
    arguments.push_back(std::move(list));
  }
```

`secure_server_main.cpp`: `Options` gains `std::vector<unsigned> gpus;`; in `parse_options`:

```cpp
    } else if (argument == "--gpu") {
      const auto devices = secure_frontend::parse_gpu_list(value);
      if (!options->gpus.empty() || !devices) {
        *error = "--gpu must be a comma-separated list of distinct device indices 0-255, given once";
        return false;
      }
      options->gpus = *devices;
```

pass `options.gpus` as the last argument of `build_engine_arguments`.

Extend Task 23's end-to-end case `proc01_server_passes_literal_argv` (`server_test.cpp`) with the typed list: after `options.engine_args = {...};` add `options.extra_args = {"--gpu", "0,1"};`, and in its `expected` vector insert `"--gpu", "0,1",` directly before `"--native"`.

Usage gains

```cpp
            "  --gpu LIST                GPUs for the engine as device indices (e.g. 0 or 0,1); passed\n"
            "                            as the engine's --gpu argument, never through the environment\n"
```

and the `--engine-arg` text lists `--gpu` among the rejected flags.

- [ ] **Step 4: Run to verify** — `ctest --preset linux-gcc 2>&1 | tail -4` → 100% `out of <Task 18 total> + 3` (new: `secure_frontend_engine_arguments_tests.engine_args_render_typed_gpu_and_deny_gpu_passthrough`, `...gpu_list_rejects_malformed_values`, `strata_secure_server_rejects_malformed_gpu`; 1 skipped).

- [ ] **Step 5: Cross-compile and run wine** — wine `out of <Task 18 wine total> + 3` (all three are unlabeled and run). Record.

- [ ] **Step 6: Handoff row**

```markdown
| §3.9 GPU | `--gpu LIST` (typed, 0–255, distinct, ≤ 16; SP1 key `engine.gpu`) renders the engine's own `--gpu` argv; `--gpu` is a frontend-owned flag for `--engine-arg` in both profiles; GLM profile refuses GPU lists; no GPU variable is ever in the engine environment. | `engine_args_render_typed_gpu_and_deny_gpu_passthrough` (incl. GLM `--gpu` passthrough refused), `gpu_list_rejects_malformed_values` (Review Focus 4), CTest `strata_secure_server_rejects_malformed_gpu` | <recorded linux line> (1 skipped); wine <recorded> |
```

- [ ] **Step 7: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "engine arguments: GPUs selected by a typed --gpu list; --gpu is frontend-owned (spec 3.9)" \
  -m "Tests: engine_args_render_typed_gpu_and_deny_gpu_passthrough, gpu_list_rejects_malformed_values (Review Focus 4), strata_secure_server_rejects_malformed_gpu." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 25: Child cwd is `<state_dir>/engine`; identity never above the frontend; elevation is never requested (G1, G3)

> **WAITS FOR SP1 MERGE — SP1 Task 5 replaces secure_server_main.cpp with a table-driven `kFlags` parser; re-anchor on that file.**
>
> **Mapping onto SP1:** no new option. The one `secure_server_main.cpp` statement, `plan.command.working_directory = state_dir / "engine";`, goes into SP1's file next to Task 18's `plan.environment = ...` line, with `state_dir` being the validated `config.roots.state` (Task 18's mapping). Everything else is launch code and tests, independent of SP1.
>
> **Owner-acknowledged deviations (Global Constraints):** G1 is asserted relatively (never MORE elevated, never a HIGHER integrity level than the frontend; same uid/gid on POSIX), because CI runs the test binary itself elevated. G3 is UNVERIFIED in CI (elevated runners cannot see error 740) and under wine (no UAC): its code ships, its case is runnable by hand, but it is not registered with CTest and is not a skipped test; the handoff lists it as a validation-kit item for the owner's offline Windows box.

**Files:**
- Modify: `secure_frontend/include/secure_frontend/engine_process.hpp` (`EngineCommand::working_directory{}`)
- Modify: `secure_frontend/src/posix_child_launch.hpp` (`PreparedExec::working_directory`; `chdir` in `exec_child`)
- Modify: `secure_frontend/src/engine_stdio_process.cpp`, `secure_frontend/src/engine_process.cpp` (Windows `lpCurrentDirectory`; Windows 740 → `spawn_failed`)
- Modify: `secure_frontend/src/secure_server_main.cpp` (`plan.command.working_directory = state_dir / "engine";`)
- Create: `secure_frontend/tests/fake_engine_elevated.rc`, `secure_frontend/tests/fake_engine_elevated.manifest`
- Modify: `secure_frontend/CMakeLists.txt` (`secure_frontend_set_warnings` scopes its flags to C++; Windows-only `strata-supervision-fake-engine-elevated`, with `/MANIFEST:NO` under MSVC)
- Modify: `secure_frontend/tests/engine_child_test.cpp` (G1 B, registered; G3 W, in the case list only)
- Modify: `docs/AGENT_HANDOFF.md` (G1/G3 row; G3 validation-kit item)

**Interfaces:**
- Produces: `EngineCommand::working_directory{}` (empty = inherit; POSIX `chdir` pre-exec, Windows `lpCurrentDirectory`); Windows `start` (both `EngineStdioProcess` and `EngineProcess`) returns `ProcessError{spawn_failed, "engine requires elevation; refused"}` on `ERROR_ELEVATION_REQUIRED` (740) and never retries via `runas`.

- [ ] **Step 1: Write the tests**

`engine_child_test.cpp` (G1: append to the `set(SECURE_FRONTEND_CHILD_CASES ...)` line and SP1's `run_cases` list; G3: SP1's `run_cases` list under `#if defined(_WIN32)` ONLY — no CMake list, so CTest never runs it):

```cpp
#if defined(_WIN32)
std::map<std::string, std::string, std::less<>> own_token_ids() {
  std::map<std::string, std::string, std::less<>> ids;
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return ids;
  const auto user = current_token_user();
  LPWSTR sid = nullptr;
  if (!user.empty() && ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid, &sid)) {
    const std::wstring wide(sid);
    ids["sid"] = std::string(wide.begin(), wide.end());  // SID strings are ASCII
    LocalFree(sid);
  }
  TOKEN_ELEVATION elevation{};
  DWORD size = sizeof(elevation);
  ids["elevated"] = GetTokenInformation(token, TokenElevation, &elevation, size, &size) && elevation.TokenIsElevated != 0 ? "1" : "0";
  size = 0;
  (void)GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
  std::vector<unsigned char> label(size);
  if (size != 0 && GetTokenInformation(token, TokenIntegrityLevel, label.data(), size, &size)) {
    PSID level = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
    ids["integrity"] = std::to_string(*GetSidSubAuthority(level, static_cast<DWORD>(*GetSidSubAuthorityCount(level) - 1U)));
  }
  CloseHandle(token);
  return ids;
}
#endif

void proc06_child_ids_and_cwd() {
  const auto state = test_support::make_temp_dir("g1");
  const auto cwd = state / "engine";
  std::filesystem::create_directories(cwd);
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}, .working_directory = cwd}).has_value(),
         "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  const auto dump = test_support::request_dump(child, 5s);
  expect(dump.has_value(), "DUMP");
  if (dump) {
    std::error_code error;
    expect(std::filesystem::equivalent(dump->cwd, cwd, error), "the engine runs in <state_dir>/engine (its cwd-relative files land there)");
#if defined(_WIN32)
    const auto parent = own_token_ids();
    expect(dump->ids.count("sid") == 1 && dump->ids.at("sid") == parent.at("sid"), "same user SID");
    // Relative by owner decision (Global Constraints): CI runs this binary
    // elevated, so an elevated engine under an elevated frontend is allowed;
    // an engine MORE elevated than the frontend never is.
    expect(!(dump->ids.at("elevated") == "1" && parent.at("elevated") == "0"),
           "never more elevated than the frontend: no elevation is requested");
    expect(std::stoul(dump->ids.at("integrity")) <= std::stoul(parent.at("integrity")), "integrity never above the frontend's");
#else
    expect(dump->ids.at("uid") == std::to_string(getuid()) && dump->ids.at("gid") == std::to_string(getgid()) &&
               dump->ids.at("euid") == std::to_string(geteuid()) && dump->ids.at("egid") == std::to_string(getegid()),
           "the same uid/gid as the frontend: nothing gained");
#endif
  }
  (void)child.terminate(250ms);
}

#if defined(_WIN32)
// G3 — UNVERIFIED in CI (the runners are elevated, so requireAdministrator
// needs no elevation and error 740 cannot occur) and under wine (no UAC). Not
// registered with CTest. Validation kit, owner's offline Windows box, from a
// NON-elevated prompt:
//   secure_frontend_engine_child_tests.exe --case proc06_elevation_required_is_spawn_failed
void proc06_elevation_required_is_spawn_failed() {
  const bool elevated = own_token_ids()["elevated"] != "0";
  expect(!elevated, "run this case from a non-elevated prompt: an elevated token never sees error 740");
  if (elevated) return;
  secure_frontend::EngineStdioProcess child;
  const auto error = child.start({.executable = std::filesystem::path(STRATA_FAKE_ENGINE_ELEVATED_PATH), .arguments = {}, .environment = {}});
  expect(error && error->code == secure_frontend::ProcessErrorCode::spawn_failed && error->message == "engine requires elevation; refused",
         "ERROR_ELEVATION_REQUIRED is spawn_failed and is never retried via runas");
}
#endif
```

(`#include <map>`; on Windows `#include <sddl.h>`. `proc06_elevation_required_is_spawn_failed` gets no CTest registration and no `SKIP_RETURN_CODE`: it is a validation-kit step, not a skipped test.)

`secure_frontend/tests/fake_engine_elevated.manifest`:

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level="requireAdministrator" uiAccess="false"/>
      </requestedPrivileges>
    </security>
  </trustInfo>
</assembly>
```

`secure_frontend/tests/fake_engine_elevated.rc`:

```
// Test-only copy of strata-supervision-fake-engine whose manifest demands elevation (SP2 G3).
1 24 "fake_engine_elevated.manifest"
```

CMake: change `secure_frontend_set_warnings` so its flags apply to C++ only (the RC compiler must not receive them):

```cmake
function(secure_frontend_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4>)
        if(SECURE_FRONTEND_WERROR)
            target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/WX>)
        endif()
    else()
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>")
        if(SECURE_FRONTEND_WERROR)
            target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Werror>)
        endif()
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endfunction()
```

and after `secure_frontend_engine_child_tests` is defined:

```cmake
    if(WIN32)
        enable_language(RC)
        add_executable(strata-supervision-fake-engine-elevated tests/supervision_fake_engine_main.cpp tests/fake_engine_elevated.rc)
        target_include_directories(strata-supervision-fake-engine-elevated PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_features(strata-supervision-fake-engine-elevated PRIVATE cxx_std_20)
        target_link_libraries(strata-supervision-fake-engine-elevated PRIVATE shell32 advapi32)
        if(MSVC)
            # The .rc embeds the requireAdministrator manifest; without /MANIFEST:NO the
            # linker generates a second one and the informational windows-msvc job
            # fails with a duplicate-manifest link error.
            target_link_options(strata-supervision-fake-engine-elevated PRIVATE /MANIFEST:NO)
        endif()
        secure_frontend_set_warnings(strata-supervision-fake-engine-elevated)
        add_dependencies(secure_frontend_engine_child_tests strata-supervision-fake-engine-elevated)
        target_compile_definitions(secure_frontend_engine_child_tests PRIVATE
            "STRATA_FAKE_ENGINE_ELEVATED_PATH=\"$<TARGET_FILE:strata-supervision-fake-engine-elevated>\"")
    endif()
```

- [ ] **Step 2: Run to verify G1 fails** — `cmake --build --preset linux-gcc 2>&1 | grep -m1 error` → `EngineCommand has no member named working_directory`; with the field and no `chdir`, the cwd assertion fails.

- [ ] **Step 3: Implement**

`engine_process.hpp`, `EngineCommand` gains after `cgroup_procs`:

```cpp
  // The child's working directory (spec §3.12): the engine writes its
  // cwd-relative `pack/full` default and `strata-stall-<pid>.dmp` there. Empty: inherit.
  // `{}`: existing designated initializers that omit it keep compiling (-Werror).
  std::filesystem::path working_directory{};
```

`posix_child_launch.hpp`: `PreparedExec` gains `std::string working_directory;`; `prepare` sets `out.working_directory = command.working_directory.string();`; in `exec_child`, after the `dup2` block and before the descriptor sweep:

```cpp
  if (!prepared.working_directory.empty() && chdir(prepared.working_directory.c_str()) != 0) fail(kReportFd);
```

Windows `EngineStdioProcess::start`: Task 17's `CreateProcessW` call becomes

```cpp
  const std::wstring working_directory = command.working_directory.wstring();
  const BOOL created = CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, TRUE,
                                      CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
                                          CREATE_NEW_PROCESS_GROUP,
                                      environment.data(), working_directory.empty() ? nullptr : working_directory.c_str(),
                                      &startup.StartupInfo, &info);
```

and the failure line after it (`if (!created) { CloseHandle(parent_stdin); ... with_last_error("CreateProcessW failed")}; }`, line 153 at `7ddcd755`) becomes

```cpp
  if (!created) {
    const DWORD error = GetLastError();  // before CloseHandle can overwrite it
    CloseHandle(parent_stdin); CloseHandle(parent_stdout); CloseHandle(job);
    // Spec §3.6/G3: an engine that asks for elevation is a spawn failure; it is never retried via runas.
    if (error == ERROR_ELEVATION_REQUIRED) return ProcessError{ProcessErrorCode::spawn_failed, "engine requires elevation; refused"};
    return ProcessError{ProcessErrorCode::spawn_failed, "CreateProcessW failed (Win32 error " + std::to_string(error) + ")"};
  }
```

`secure_frontend/src/engine_process.cpp`, Windows `start`: Task 17's `if (!CreateProcessW(...)) { CloseHandle(job); return ...; }` block becomes

```cpp
  const std::wstring working_directory = command.working_directory.wstring();
  if (!CreateProcessW(command.executable.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                      CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP, environment.data(),
                      working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &info)) {
    const DWORD error = GetLastError();
    CloseHandle(job);
    // Spec §3.6/G3: never retried via runas.
    return ProcessError{ProcessErrorCode::spawn_failed,
                        error == ERROR_ELEVATION_REQUIRED ? "engine requires elevation; refused" : "CreateProcessW failed"};
  }
```

In `secure_server_main.cpp` (WAITS FOR SP1 MERGE; SP1's file, see the mapping note), after `plan.environment = ...;` (still before the `EngineSupervisor` is constructed) add `plan.command.working_directory = state_dir / "engine";`.

- [ ] **Step 4: Run to verify** — `ctest --preset linux-gcc 2>&1 | tail -4` → 100% `out of <Task 19 total> + 1` (new: `secure_frontend_engine_child_tests.proc06_child_ids_and_cwd`; 2 skipped). `ctest --preset linux-gcc -N | grep -c proc06_elevation_required` prints `0`.

- [ ] **Step 5: Cross-compile and record wine** — `-Werror` clean, and `strata-supervision-fake-engine-elevated.exe` is produced (the RC compiler receives no C++ warning flags); record `WINEPREFIX=$PWD/out/wineprefix wine out/build/windows-llvm-mingw-cross/secure_frontend_engine_child_tests.exe --case proc06_child_ids_and_cwd; echo "exit $?"` (G3 is not run under wine: no UAC); wine ctest total unchanged (G1 labeled).

- [ ] **Step 6: Handoff row**

```markdown
| G1, G3 | The engine's cwd is `<state_dir>/engine` (POSIX `chdir` pre-exec, Windows `lpCurrentDirectory`); same uid/gid (POSIX) and same SID, never MORE elevated, never a higher integrity level than the frontend (Windows; relative check by owner decision). `ERROR_ELEVATION_REQUIRED` (740) is `spawn_failed`, never `runas`. | `proc06_child_ids_and_cwd` (B); G3: `proc06_elevation_required_is_spawn_failed` (W) — **UNVERIFIED in CI and wine; validation-kit item, not a CTest test** | <recorded linux line> (2 skipped); wine: <recorded> |
```

In the handoff's Phase 5 / SP7 GPU validation-kit item add the Windows line: "SP2 G3 (unverified in CI and wine): on the offline Windows box, from a NON-elevated prompt, run `secure_frontend_engine_child_tests.exe --case proc06_elevation_required_is_spawn_failed` and record the result; it must pass (error 740 → `spawn_failed`, no `runas`)."

- [ ] **Step 7: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "engine launch: cwd under the state dir; elevation is a spawn failure, never runas (G1, G3)" \
  -m "G1: secure_frontend_engine_child_tests.proc06_child_ids_and_cwd (relative identity check, owner decision). G3: proc06_elevation_required_is_spawn_failed (W) is a validation-kit step, unverified in CI/wine, not registered. secure_frontend_set_warnings now scopes its flags to C++ so .rc files build; /MANIFEST:NO for the .rc target under MSVC." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 26: `PR_SET_NO_NEW_PRIVS` on by default with a documented switch; refuse root/elevated start (G2, G4)

> **WAITS FOR SP1 MERGE — SP1 Task 5 replaces secure_server_main.cpp with a table-driven `kFlags` parser; re-anchor on that file.**
>
> **Mapping onto SP1:** `--allow-new-privileges` → schema key `engine.allow_new_privileges` (boolean, default false) plus the `kFlags` row `{"--allow-new-privileges", "engine.allow_new_privileges", FlagKind::boolean}`; `FrontendConfig::engine` gains `bool allow_new_privileges`. Apply Task 29 Step 3's row now instead of the `Options`/`parse_options` edit below; the `run` code reads `config.engine.allow_new_privileges` where it reads `options.allow_new_privileges`. The usage line comes from `kFlags` (the three explanatory lines below go into the key's schema description). The privileged-start refusal goes into SP1's `main` directly after its `ServerCommand::help` branch and before `load_server_config` (so `--check` and `init` are refused as root too). The test harness already runs the server with a non-elevated token on Windows (Task 3); this task adds no harness change and no override flag.

**Files:**
- Create: `secure_frontend/include/secure_frontend/privilege_check.hpp`, `secure_frontend/src/privilege_check.cpp`
- Modify: `secure_frontend/include/secure_frontend/engine_process.hpp` (`EngineCommand::no_new_privileges`), `secure_frontend/src/posix_child_launch.hpp` (`prctl(PR_SET_NO_NEW_PRIVS)` pre-exec)
- Modify: `secure_frontend/include/secure_frontend/engine_supervisor.hpp` (`kNoNewPrivilegesHint`, member `nnp_hint_logged_`), `secure_frontend/src/engine_supervisor.cpp` (`end_generation` logs the hint)
- Modify: `secure_frontend/src/secure_server_main.cpp` (`--allow-new-privileges`, usage, privileged-start check)
- Modify: `secure_frontend/tests/engine_child_test.cpp` (G2), `secure_frontend/tests/server_test.cpp` (hint), `secure_frontend/tests/engine_supervisor_test.cpp` (G4), `secure_frontend/CMakeLists.txt`
- Modify: `docs/security/deployment.md` ("Linux prerequisites and operational checks"), `docs/AGENT_HANDOFF.md` (row; GPU validation-kit item)

**Interfaces:**
- Produces: `EngineCommand::no_new_privileges{false}` (default false at the type, so existing designated initializers compile; the server sets `supervision_defaults::kNoNewPrivilegesDefault` = true unless `--allow-new-privileges`); `kNoNewPrivilegesHint` (fixed log line naming `/dev/nvidia-uvm`, `nvidia-modprobe -u -c=0`, `nvidia-persistenced`, `--allow-new-privileges`), logged once when a generation ends before READY with no supervisor kill and NNP on; `ProcessPrivileges{posix_root, windows_elevated}`, `current_process_privileges()`, `privileged_start_error(const ProcessPrivileges&)` → fixed text or nullopt.

- [ ] **Step 1: Write the failing tests**

`engine_child_test.cpp` (append to the `SECURE_FRONTEND_CHILD_LINUX_CASES` line and SP1's `run_cases` list under `#if !defined(_WIN32)`; includes `"support/server_harness.hpp"`):

```cpp
#if !defined(_WIN32)
std::string own_no_new_privs() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("NoNewPrivs:", 0) == 0) return line.substr(line.find_first_not_of(" \t", 11));
  }
  return "?";
}

void proc06_child_has_no_new_privs() {
  for (const bool requested : {true, false}) {
    secure_frontend::EngineStdioProcess child;
    expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}, .no_new_privileges = requested})
                .has_value(),
           "start");
    expect(child.read_line(5s).line == "READY 4096 stop", "READY");
    const auto dump = test_support::request_dump(child, 5s);
    const std::string expected = requested || own_no_new_privs() == "1" ? "1" : "0";
    expect(dump && dump->ids.at("nnp") == expected, std::string("NoNewPrivs is ") + expected + (requested ? " when requested" : " when not requested"));
    (void)child.terminate(250ms);
  }
  // The production default is ON; --allow-new-privileges turns it off.
  for (const bool allow : {false, true}) {
    const auto dir = test_support::make_temp_dir("g2");
    test_support::ServerOptions options;
    options.engine_args = {"--dump-file=" + test_support::utf8_path(dir / "dump.txt")};
    if (allow) options.extra_args = {"--allow-new-privileges"};
    auto server = test_support::ServerProcess::start(options);
    expect(server != nullptr && server->wait_until_listening(10s), "server listens");
    const auto dump = test_support::read_dump_file(dir / "dump.txt", 10s);
    const std::string expected = !allow || own_no_new_privs() == "1" ? "1" : "0";
    expect(dump && dump->ids.at("nnp") == expected, allow ? "--allow-new-privileges leaves NoNewPrivs off" : "the server sets NoNewPrivs by default");
  }
}
#endif
```

`server_test.cpp` (append to the `SECURE_FRONTEND_SERVER_LINUX_CASES` line and SP1's `run_cases` list under `#if !defined(_WIN32)`):

```cpp
#if !defined(_WIN32)
void proc06_nnp_startup_hint_names_nvidia_uvm_fix() {
  test_support::ServerOptions options;
  options.engine_args = {"--exit-before-ready=1"};
  auto server = ServerProcess::start(options);
  expect(server != nullptr && server->wait_until_listening(10s), "the server listens while the engine fails");
  if (!server) return;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (server->stderr_text().find("/dev/nvidia-uvm") == std::string::npos && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(20ms);
  }
  const std::string log = server->stderr_text();
  expect(log.find("/dev/nvidia-uvm") != std::string::npos && log.find("nvidia-modprobe") != std::string::npos &&
             log.find("--allow-new-privileges") != std::string::npos,
         "an engine that dies before READY under NNP gets the nvidia-uvm hint");
  test_support::ServerOptions allowed = options;
  allowed.extra_args = {"--allow-new-privileges"};
  auto second = ServerProcess::start(allowed);
  expect(second != nullptr && second->wait_until_listening(10s), "second server listens");
  std::this_thread::sleep_for(1500ms);
  expect(second && second->stderr_text().find("/dev/nvidia-uvm") == std::string::npos, "no hint when NNP is off");
}
#endif
```

`engine_supervisor_test.cpp` (append to the `SECURE_FRONTEND_SUPERVISOR_UNIT_CASES` line and SP1's `run_cases` list; `#include "secure_frontend/privilege_check.hpp"`):

```cpp
void proc06_refuses_privileged_start() {
  expect(secure_frontend::privileged_start_error({.posix_root = true, .windows_elevated = false}).has_value(), "root is refused");
  expect(secure_frontend::privileged_start_error({.posix_root = false, .windows_elevated = true}).has_value(), "an elevated token is refused");
  expect(!secure_frontend::privileged_start_error({.posix_root = false, .windows_elevated = false}).has_value(), "an unprivileged user starts");
#if !defined(_WIN32)
  expect(secure_frontend::current_process_privileges().posix_root == (geteuid() == 0), "the probe reads the effective uid");
#endif
}
```

- [ ] **Step 2: Run to verify they fail** — compile errors first (`no_new_privileges`, `privilege_check.hpp`); with stubs, `proc06_child_has_no_new_privs` reports `NoNewPrivs is 1 when requested` failing and the hint test finds no hint.

- [ ] **Step 3: Implement NNP, the hint and the switch**

`engine_process.hpp`, `EngineCommand` gains after `working_directory`:

```cpp
  // Linux: PR_SET_NO_NEW_PRIVS before execve (spec §3.12). The server turns it
  // on by default (owner decision 2026-10-13); --allow-new-privileges turns it off.
  bool no_new_privileges{false};
```

`posix_child_launch.hpp`: `PreparedExec` gains `bool no_new_privileges{false};`, `prepare` copies it; in `exec_child`, after the `chdir` line:

```cpp
#if defined(__linux__)
  if (prepared.no_new_privileges && prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0) fail(kReportFd);
#endif
```

`engine_supervisor.hpp`:

```cpp
// Logged once when a generation ends before READY on its own while
// PR_SET_NO_NEW_PRIVS is set: the NVIDIA driver may need to exec setuid
// nvidia-modprobe to create /dev/nvidia-uvm*, which NNP blocks (spec open question 5).
inline constexpr std::string_view kNoNewPrivilegesHint =
    "engine ended before READY while PR_SET_NO_NEW_PRIVS is set; if this host relies on nvidia-modprobe to create "
    "/dev/nvidia-uvm, create the device nodes at boot (as root: nvidia-modprobe -u -c=0, or enable nvidia-persistenced) "
    "or start the frontend with --allow-new-privileges";
```

private member `bool nnp_hint_logged_{false};`. In `end_generation`, directly after the `log_end(instance->number, describe_exit(...))` line:

```cpp
  if (config_.no_new_privileges_hint && !nnp_hint_logged_ && !instance->chat && sent == KillReason::none) {
    nnp_hint_logged_ = true;
    log_(kNoNewPrivilegesHint);
  }
```

`secure_server_main.cpp`: `Options` gains `bool allow_new_privileges{false};`; in `parse_options` next to `--allow-remote`:

```cpp
    if (argument == "--allow-new-privileges") {
      if (options->allow_new_privileges) { *error = "duplicate --allow-new-privileges"; return false; }
      options->allow_new_privileges = true;
      continue;
    }
```

usage:

```cpp
            "  --allow-new-privileges    Linux: do not set PR_SET_NO_NEW_PRIVS on the engine (default:\n"
            "                            set). Only for hosts whose NVIDIA driver must run setuid\n"
            "                            nvidia-modprobe to create /dev/nvidia-uvm on first use\n"
```

after the `plan` lines in `run`:

```cpp
  plan.command.no_new_privileges = secure_frontend::supervision_defaults::kNoNewPrivilegesDefault && !options.allow_new_privileges;
#if defined(__linux__)
  supervisor_config.no_new_privileges_hint = plan.command.no_new_privileges;
#endif
```

These lines go directly before `secure_frontend::EngineSupervisor supervisor(...)` (as do the `plan.use_delegated_cgroup`, `plan.environment` and `plan.command.working_directory` lines from Tasks 16, 18 and 25): the supervisor takes the plan and config by value when it is built.

- [ ] **Step 4: Implement the privileged-start refusal** — `secure_frontend/include/secure_frontend/privilege_check.hpp`:

```cpp
#pragma once

#include <optional>
#include <string_view>

namespace secure_frontend {

// Spec §3.12 / G4: the frontend runs as the invoking unprivileged user.
struct ProcessPrivileges final {
  bool posix_root{false};        // effective uid 0
  bool windows_elevated{false};  // TokenElevation.TokenIsElevated
};

[[nodiscard]] ProcessPrivileges current_process_privileges();
// Fixed text when start must be refused, nullopt otherwise.
[[nodiscard]] std::optional<std::string_view> privileged_start_error(const ProcessPrivileges& privileges) noexcept;

}  // namespace secure_frontend
```

`secure_frontend/src/privilege_check.cpp`:

```cpp
#include "secure_frontend/privilege_check.hpp"

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <unistd.h>
#endif

namespace secure_frontend {

ProcessPrivileges current_process_privileges() {
  ProcessPrivileges privileges;
#if defined(_WIN32)
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    privileges.windows_elevated = GetTokenInformation(token, TokenElevation, &elevation, size, &size) && elevation.TokenIsElevated != 0;
    CloseHandle(token);
  }
#else
  privileges.posix_root = geteuid() == 0;
#endif
  return privileges;
}

std::optional<std::string_view> privileged_start_error(const ProcessPrivileges& privileges) noexcept {
  if (privileges.posix_root || privileges.windows_elevated) {
    return "Refusing to run as root or with an elevated token; start strata-secure-server as an unprivileged user.";
  }
  return std::nullopt;
}

}  // namespace secure_frontend
```

Add `src/privilege_check.cpp` to `secure_frontend_engine_supervisor`. In `secure_server_main.cpp` `main`, after `if (help) { ... }`:

```cpp
  if (const auto refused = secure_frontend::privileged_start_error(secure_frontend::current_process_privileges())) {
    std::cerr << *refused << '\n';
    return EXIT_FAILURE;
  }
```

The harness needs no change: since Task 3 it always starts the server with a LUA-restricted, verified non-elevated token on Windows (Safer `NORMALUSER` fallback; otherwise it stops and asks the owner). There is no test-only override of the refusal anywhere. If the MSYS2 job shows the server refusing to start under the harness, stop and report to the owner.

Docs — `docs/security/deployment.md`, under "Linux prerequisites and operational checks", add:

```markdown
- The engine runs with `PR_SET_NO_NEW_PRIVS` (default). On hosts where the
  NVIDIA driver creates `/dev/nvidia-uvm*` on first CUDA use by executing the
  setuid `nvidia-modprobe`, NNP blocks that: create the nodes at boot instead
  (as root: `nvidia-modprobe -u -c=0`, or enable `nvidia-persistenced`). The
  frontend logs a hint naming this fix when an engine dies before READY under
  NNP. `--allow-new-privileges` turns NNP off; it exists for the offline GPU
  validation kit and such hosts, and is not the default.
- `strata-secure-server` refuses to start as root (Windows: with an elevated
  token). Containers must run it as an unprivileged user.
- `--state-dir` must be an existing directory owned by the service user with
  no group/other permissions; the engine's working directory, temp and CUDA
  cache live below it.
```

Handoff: in Phase 5 / SP7 (the GPU validation-kit item) add: "SP2 inputs for the kit: check that `/dev/nvidia-uvm` exists before starting; run once with the default (`PR_SET_NO_NEW_PRIVS` on) and, only if CUDA init fails, once with `--allow-new-privileges`; record which works; confirm the SP2 environment allowlist is sufficient for CUDA and ROCm (spec §2, not yet verified)."

(Registration is in Step 1's three list lines.)

- [ ] **Step 5: Run to verify** — `ctest --preset linux-gcc 2>&1 | tail -4` → 100% `out of <Task 25 total> + 3` (2 skipped).

- [ ] **Step 6: Cross-compile and run wine** — `-Werror` clean; wine `out of <Task 25 wine total> + 1` (`proc06_refuses_privileged_start` runs). Record.

- [ ] **Step 7: Handoff row**

```markdown
| G2, G4 | Linux: `PR_SET_NO_NEW_PRIVS` pre-exec, **on by default** (owner decision); `--allow-new-privileges` turns it off (documented for the GPU validation kit); a one-time log hint names the `/dev/nvidia-uvm` / `nvidia-modprobe -u -c=0` / `nvidia-persistenced` fix when an engine dies before READY under NNP. The frontend refuses euid 0 / an elevated Windows token with a fixed error (no override flag; the Task 3 harness runs the server non-elevated, MSYS2 to confirm). | `proc06_child_has_no_new_privs` (L), `proc06_nnp_startup_hint_names_nvidia_uvm_fix` (L), `proc06_refuses_privileged_start` (B unit) | <recorded linux line> (2 skipped); wine <recorded> |
```

- [ ] **Step 8: Commit**

```bash
git add secure_frontend docs/security/deployment.md docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "secure_frontend: no_new_privs on by default with a documented switch; refuse root/elevated start (G2, G4)" \
  -m "G2: proc06_child_has_no_new_privs, proc06_nnp_startup_hint_names_nvidia_uvm_fix. G4: proc06_refuses_privileged_start." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 27: Extension attempts spawn nothing and never reach the child (X1, X2)

**Files:**
- Modify: `secure_frontend/tests/server_test.cpp` (three cases), `secure_frontend/CMakeLists.txt`
- Modify: `docs/AGENT_HANDOFF.md` (X1/X2 row)

**Interfaces:**
- Consumes: parse-before-acquire (Task 6b), the allowlist (Task 18), the deny-list (SP0 + Task 19), `--record-file` (created at fake start), `--dump-file`.
- Produces: no production change (spec X1/X2 have no fix column); the tests must pass on first run. The model inventory has no manifest fields (IDs come from file names), so there is no manifest extension to attempt; record that in the handoff row.

- [ ] **Step 1: Write the tests** (append all three to the `set(SECURE_FRONTEND_SERVER_CASES ...)` line and SP1's `run_cases` list; `#include <fstream>`, `<iterator>`). Runs after the SP1 merge: under SP1's `validate()` a rejected option or `engine.args` entry exits 1 before anything is spawned, which is what `ext02_rejected_before_spawn` asserts.

```cpp
void ext01_extension_fields_spawn_nothing() {
  const auto dir = test_support::make_temp_dir("x1");
  const auto record = dir / "record.txt";
  test_support::ServerOptions options;
  options.engine_args = {"--record-file=" + test_support::utf8_path(record)};
  auto server = ServerProcess::start(options);
  expect(server != nullptr && server->wait_until_listening(10s) && server->wait_for_health(test_support::kHealthReady, 10s), "ready");
  if (!server) return;
  const auto processes_before = server->active_processes();
  const std::vector<std::string> bodies{
      R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"tools":[{"type":"function","function":{"name":"x"}}]})",
      R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"tool_choice":"auto"})",
      R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"functions":[{"name":"x"}]})",
      R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"mcp_servers":[{"url":"mcp://x"}]})",
      R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"stream":true})",
      R"({"model":"fake-model","messages":[{"role":"tool","content":"run"}]})"};
  for (const auto& body : bodies) {
    const auto response = server->post_chat(body);
    expect(response && response->status == 400 && response->body == test_support::kBadRequest, "refused with the fixed response: " + describe(response));
  }
  std::ifstream recorded(record);
  const std::string content((std::istreambuf_iterator<char>(recorded)), std::istreambuf_iterator<char>());
  expect(content.find("GEN") == std::string::npos, "the engine received no GEN");
  expect(server->active_processes() == processes_before, "no process started or ended");
}

void ext02_extension_env_never_reaches_child() {
  const auto dir = test_support::make_temp_dir("x2");
  test_support::ServerOptions options;
  options.engine_args = {"--dump-file=" + test_support::utf8_path(dir / "dump.txt")};
  options.extra_env = {{"STRATA_FOO", "1"}, {"STRATA_ENABLE_MCP", "1"}, {"LD_PRELOAD", "/nonexistent/libstrata-test.so"},
                       {"CUDA_LAUNCH_BLOCKING", "1"}};
  auto server = ServerProcess::start(options);
  expect(server != nullptr && server->wait_until_listening(10s), "the server listens");
  const auto dump = test_support::read_dump_file(dir / "dump.txt", 10s);
  expect(dump.has_value(), "the engine reported its environment");
  if (!dump) return;
  for (const auto& entry : dump->env) {
    for (const std::string_view banned :
         {"STRATA_FOO=", "STRATA_ENABLE_MCP=", "LD_PRELOAD=", "CUDA_LAUNCH_BLOCKING=", "STRATA_HARNESS_TOKEN="}) {
      expect(entry.rfind(banned, 0) != 0, "never reaches the engine: " + std::string(banned));
    }
  }
}

void ext02_rejected_before_spawn() {
  const auto dir = test_support::make_temp_dir("x2b");
  const std::vector<std::pair<std::string, test_support::ServerOptions>> attempts{
      {"--vision via --engine-arg", {.engine_args = {"--vision=/m.gguf"}, .extra_args = {}, .extra_env = {}, .engine = {}}},
      {"--batch via --engine-arg", {.engine_args = {"--batch", "2"}, .extra_args = {}, .extra_env = {}, .engine = {}}},
      {"--gpu via --engine-arg", {.engine_args = {"--gpu=0"}, .extra_args = {}, .extra_env = {}, .engine = {}}},
      {"unknown option", {.engine_args = {}, .extra_args = {"--enable-mcp", "1"}, .extra_env = {}, .engine = {}}}};
  int index = 0;
  for (auto [name, options] : attempts) {
    const auto record = dir / ("record-" + std::to_string(index++) + ".txt");
    options.engine_args.push_back("--record-file=" + test_support::utf8_path(record));
    auto server = ServerProcess::start(options);
    expect(server != nullptr, name + ": launched");
    if (!server) continue;
    expect(server->wait_exit(10s) == 1, name + ": rejected at startup");
    expect(!std::filesystem::exists(record), name + ": no engine was ever started");
  }
}
```

- [ ] **Step 2: Run** — `ctest --preset linux-gcc 2>&1 | tail -4` → 100% `out of <Task 26 total> + 3` (2 skipped).

- [ ] **Step 3: Cross-compile and record wine** — `-Werror` clean; wine total unchanged (server cases labeled).

- [ ] **Step 4: Handoff row**

```markdown
| X1, X2 | Tool/function/MCP/stream fields and a `tool` role are answered with the fixed `400 {"error":"Bad Request"}` before any lease: no GEN, no process change. `STRATA_*`, `LD_PRELOAD`, `CUDA_LAUNCH_BLOCKING` in the server's environment never reach the engine; `--vision`/`--batch`/`--gpu` via `--engine-arg` and unknown options fail before any spawn. The inventory has no manifest fields to abuse (IDs come from file names). | `ext01_extension_fields_spawn_nothing`, `ext02_extension_env_never_reaches_child`, `ext02_rejected_before_spawn` | <recorded linux line> (2 skipped); wine unchanged |
```

- [ ] **Step 5: Commit**

```bash
git add secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "test: extension fields spawn nothing; extension env and flags never reach the engine (X1, X2)" \
  -m "X1: ext01_extension_fields_spawn_nothing. X2: ext02_extension_env_never_reaches_child, ext02_rejected_before_spawn." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 28: Documentation truth pass — sweep cap, limits doc, threat model, handoff (K4, K6)

**Files:**
- Modify: `secure_frontend/src/posix_child_launch.hpp:33-34` (K4 comment)
- Modify: `docs/security/engine-protocol-limits.md:17` (and new rows)
- Modify: `docs/security/threat-model.md:128-144` ("Engine process isolation")
- Modify: `docs/security/deployment.md` ("Release evidence checklist": accepted risks)
- Modify: `docs/secure-frontend-ci.md` (counts, labels)
- Modify: `secure_frontend/README.md:190` (environment sentence)
- Modify: `docs/AGENT_HANDOFF.md` (Phase 2 item 3; Known small gaps; Verification performed; SP2 table complete)

**Interfaces:**
- Consumes: every earlier task's row.
- Produces: no code behavior change. "Review" rows (K4, K6): the check is the reviewer reading the diff plus a full green run.

- [ ] **Step 1: K4 comment** — replace lines 33-34 of `posix_child_launch.hpp` with:

```cpp
// Upper bound for the fallback descriptor sweep. It is reached only where
// close_range(2) is missing (kernel < 5.9 or a non-Linux POSIX system) AND
// RLIMIT_NOFILE exceeds it; a sweep to an unbounded limit could take seconds
// per spawn. Every descriptor the frontend opens is O_CLOEXEC, so the sweep is
// defence in depth, not the primary guarantee.
inline constexpr long kSweepCap = 65536;
inline constexpr int kSweepDefault = 1024;
```

- [ ] **Step 2: Limits doc** — replace line 17 of `docs/security/engine-protocol-limits.md` with the first row below and append the others:

```markdown
| Windows stdin writes | The engine reads stdin on a reader thread with `read(0)` / `_read(0)` (`generate.cpp:8860-8900`), which normally drains the pipe | The frontend's end of the stdin pipe is overlapped: writes go in 64 KiB chunks and a chunk with no progress for 1 s is cancelled (`CancelIoEx`); the write fails and the supervisor kills the engine (`write_timeout`). POSIX polls each chunk for 1 s the same way. Windows behavior is confirmed only by the `windows-msys2-clang` job |
| READY wait | Upstream waits `STRATA_ENGINE_READY_S` (900 s) | The supervisor consumes READY eagerly within `ready_timeout` (900 s); `/health` is `503 starting` until then; a miss kills the engine and counts a failure |
| Fatal lines | `ERR verify: timed out at layer `, `ERR verify batch: timed out at layer `, `ERR verify: an earlier window never finished`, `ERR verify batch: an earlier window never finished` (upstream `FATAL_PREFIXES`); the engine lives on but is unusable | Prefix match fails the request with a generic `503`, kills the engine and restarts it; a trailing `DONE` is never accepted |
| Exit codes | QUIT/EOF → 0; `ERR` paths → 1; CUDA fault → `_Exit(1)`; watchdog → `abort()` (SIGABRT; Windows 3 or `0xC0000409`) | Classified only for the frontend's own log line (`engine generation <n> ended: <kind>`); every end counts as a failure |
| GPU selection | `--gpu LIST` makes the engine set `CUDA_DEVICE_ORDER`/`CUDA_VISIBLE_DEVICES` (or HIP) itself (`generate.cpp:1765-1786`) | The frontend renders `--gpu` from its typed `--gpu` option; no GPU variable is ever in the engine's environment |
```

- [ ] **Step 3: Threat model** — in "Engine process isolation" replace the bullet that starts "The child inherits an empty environment allowlist" with:

```markdown
- The child gets a fixed, frontend-rendered environment and nothing from the
  parent: POSIX `PATH=/usr/bin:/bin`, `LD_LIBRARY_PATH`, `TMPDIR`,
  `CUDA_CACHE_PATH`; Windows `PATH`, `TEMP`, `TMP`, `CUDA_CACHE_PATH`,
  `SystemRoot`, `WINDIR`, `SystemDrive`, `NUMBER_OF_PROCESSORS`; optional
  `STRATA_HIPBLASLT_TUNING`, `STRATA_WATCHDOG_S`, `STRATA_WATCHDOG_IO_S`. Never
  the token variable, `STRATA_API_KEY` or other `STRATA_*`, `LD_PRELOAD`,
  `LD_AUDIT`, `CUDA_LAUNCH_BLOCKING`, `HOME`/`USERPROFILE`/`APPDATA`, proxies,
  or GPU-selection variables (GPUs are typed `--gpu` argv). Not yet verified
  with a real CUDA/ROCm engine. It inherits no listening socket, credential
  descriptor/handle or token; it starts with default signal dispositions, an
  empty signal mask, `<state_dir>/engine` as its cwd and, on Linux,
  `PR_SET_NO_NEW_PRIVS`. Its stdout is the bounded strict protocol pipe; stderr
  is discarded to the platform null device so diagnostics cannot corrupt
  protocol framing or leak prompts into logs.
```

replace "Restart loops are bounded with backoff." with:

```markdown
A single supervisor thread owns the engine: it restarts a failed engine after
1 s, 2 s, … (at most 60 s), opens a circuit on the third consecutive failure
(closed only by restarting the frontend), and resets the count after 300 s of
healthy running. Linux cleanup is process-group SIGKILL, a child-subreaper
sweep after every generation, `PR_SET_PDEATHSIG`, and `cgroup.kill` when a
delegated cgroup v2 is detected. **Residual risk:** a frontend killed with
SIGKILL on Linux without a usable cgroup takes the engine leader with it, but
the engine's own descendants survive (the upstream engine is not known to
spawn any).
```

and append to the "Privilege elevation is forbidden" bullet: " The frontend refuses to start as root or with an elevated Windows token; an engine that requires elevation is a spawn failure and is never started via `runas` (G3 is verified only on a non-elevated Windows prompt, a validation-kit step; CI runners are elevated)." Append to the restart-policy paragraph above: " Because an `ERR` answer leaves the generation unusable, three requests in a row whose input makes the engine answer `ERR` open the circuit until the frontend is restarted; this is accepted (Review Focus 6, `proc04_client_err_lines_open_circuit_until_restart`)."

- [ ] **Step 4: Release record, CI doc, README** — `docs/security/deployment.md`, "Release evidence checklist", add: "Unresolved accepted risks to state: PROC-03 same-uid reopen of engine stdin on Linux (spec SP2 §3.13); PROC-05 frontend SIGKILL without a delegated cgroup leaves engine descendants (spec SP2 §3.7); the SP2 environment allowlist and NNP default not yet verified on a GPU." In `docs/secure-frontend-ci.md`, in the paragraph SP1 wrote about the cross build's `.exe` files and per-case registration, append:

```markdown
SP2 adds seven `.exe` files: the test-only `strata-supervision-fake-engine` and
`strata-supervision-fake-engine-elevated`, and five test binaries
(`secure_frontend_{fake_engine,deadline,engine_child,engine_supervisor,server}_tests`),
each case registered as `<binary>.<case>` through SP1's `secure_frontend_add_cases`.
Tests that need real Windows semantics (Job Objects, handles, DACLs, tokens,
console events, crash exit codes, every server-level test) carry the CTest label
`windows-runtime`; the `windows-llvm-mingw-cross` test preset excludes that
label, so wine runs only the portable cases, and only the `windows-msys2-clang`
job runs the Windows rows. Two tests skip with a stated reason when their
precondition is absent (`proc03_other_uid_cannot_open_engine_fd` needs
`STRATA_TEST_SECOND_UID_SUDO=1`, set in CI; `proc05_cgroup_kill_when_delegated`
needs a delegated cgroup v2). G3 (`proc06_elevation_required_is_spawn_failed`)
is not a CTest test: the runners are elevated, so it is a validation-kit step
for a non-elevated Windows prompt.
```

In `secure_frontend/README.md` replace the sentence at line 190 that starts "The engine child receives an empty environment allowlist" with: "The engine child receives a fixed environment rendered by the frontend (see `docs/security/threat-model.md`, Engine process isolation); nothing is inherited from the server's environment, so pass every engine setting as a typed option or `--engine-arg`." (keep the rest of that paragraph).

- [ ] **Step 5: Handoff** — Phase 2 item 3 becomes "**Done on branch `sp2-portable-supervision` for Linux (all L and B rows green locally); Windows (W) rows pending the `windows-msys2-clang` job.**"; delete every remaining Known-small-gaps bullet tagged SP2 that a task above resolved (lazy READY, duplicated deadline loops, default pipe DACL, `terminate` contract, sweep cap, `current_executable()`); change the section title "SP2 deliverables (… in progress)" to drop "in progress" and add the K4/K6 row:

```markdown
| K4, K6 | `kSweepCap` documented; limits doc (reader thread on `read(0)`/`_read(0)`, overlapped writes, READY wait, fatal lines, exit codes, GPU argv), threat model (allowlist, restart policy, PROC-03/PROC-05 residual risks, NNP, refusal of root/elevated, three client-triggerable ERR lines open the circuit), release-record accepted risks, CI doc. | review | <recorded linux line> (2 skipped); wine <recorded> |
```

Add to "Verification performed" one dated line per command actually run in this task (Step 6), with the exact result lines, and the sentence: "Windows (W) rows and every `windows-runtime` test are unverified until the `windows-msys2-clang` job is green after the owner pushes; nothing here was run on a GPU."

- [ ] **Step 6: Full verification**

Run: `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc 2>&1 | tail -6`
Expected: `100% tests passed, 0 tests failed out of <Task 27 total>` with exactly the two skipped tests named above.
Run: `cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross 2>&1 | tail -2 && ls out/build/windows-llvm-mingw-cross/*.exe | wc -l && ctest --preset windows-llvm-mingw-cross 2>&1 | tail -3`
Expected: builds with `-Werror`; the SP1-merged `.exe` count + 7; `100% tests passed, 0 tests failed out of <Task 27 wine total>`.
Run: `pgrep -f strata-supervision-fake-engine || echo none` → `none` (no test left a fake behind).
Run (repo root): `.venv-dev/bin/pytest -q | tail -2` — unchanged pass count (`tools/secure` only).
Run: `git status --short` — only this task's files.

- [ ] **Step 7: Commit**

```bash
git add secure_frontend docs
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "docs: SP2 truth pass; sweep cap, limits, threat model, release risks, handoff (K4, K6)" \
  -m "K4: kSweepCap comment. K6: engine-protocol-limits (reader thread, overlapped writes), threat model (allowlist, restart policy, PROC-03/05 residual risks), release record, CI doc, handoff." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

### Task 29: Post-SP1 integration — SP2 options as schema keys, config-driven policy, one defaults header, one CSPRNG

> **WAITS FOR SP1 MERGE** (runs last). Steps 3 and 6 are applied early by the tasks that need them — Tasks 18, 19 and 26 each land their own Step 3 row, and Task 6b lands Step 6 — so here they are verified and completed, not redone; every other step is new work.

**Files:**
- Modify (SP1's files): `secure_frontend/src/frontend_config.cpp` (`kSchema` rows, `validate()` reads, `describe()` lines), `secure_frontend/include/secure_frontend/frontend_config.hpp` (`Roots::state`, `EngineLaunch::{lib_dirs, gpus, allow_new_privileges}`), `secure_frontend/src/server_cli.cpp` (`kFlags` rows), `secure_frontend/include/secure_frontend/config_defaults.hpp` (supervision constants), `secure_frontend/include/secure_frontend/subsystem_limits.hpp`, `secure_frontend/src/subsystem_limits.cpp` (`supervisor_config`), `secure_frontend/tests/support/server_fixture.hpp` (`roots.state`, `wait_engine_ready`), `secure_frontend/tests/server_cli_test.cpp`, `secure_frontend/tests/subsystem_limits_test.cpp`
- Delete: `secure_frontend/include/secure_frontend/supervision_defaults.hpp` (folded into `config_defaults.hpp`)
- Modify (SP2's files): `restart_policy.hpp`, `engine_supervisor.hpp`, `engine_supervisor.cpp`, `engine_stdio_process.cpp` (constants from `config_defaults`; the pipe name from `fill_random`), `engine_environment.{hpp,cpp}` (`engine_environment_for`), `secure_server_main.cpp` (config-driven supervisor and environment), `tests/engine_supervisor_test.cpp`
- Modify: `secure_frontend/CMakeLists.txt` (`secure_frontend_engine_stdio_process` links `secure_frontend_secrets` instead of `bcrypt`; `secure_frontend_subsystem_limits` links `secure_frontend_engine_supervisor`; three new cases), `docs/AGENT_HANDOFF.md` (integration row), `secure_frontend/README.md` (key table rows)

**Interfaces:**
- Consumes (SP1): `kSchema`/`ConfigKey`, `DraftReader` (`read.string`, `read.strings`, `read.boolean`, `read.present`), `absolute_root`, `FrontendConfig`, `kFlags`/`FlagSpec`/`FlagKind`, `config_defaults::*`, `chat_execution_limits(const FrontendConfig&)`, `fill_random(unsigned char*, std::size_t)` (`token_generator.hpp`), `ServerFixture`.
- Produces: schema keys `roots.state` (string; `serve` refuses to start without it), `engine.lib_dirs` (string array, each absolute), `engine.gpu` (string, `parse_gpu_list`), `engine.allow_new_privileges` (boolean); `kFlags` rows `--state-dir`, `--engine-lib-dir`, `--gpu`, `--allow-new-privileges`; `Roots::state` (`std::optional<std::filesystem::path>`), `EngineLaunch::lib_dirs`, `EngineLaunch::gpus`, `EngineLaunch::allow_new_privileges`; `SupervisorConfig supervisor_config(const FrontendConfig&)` (`policy.ready_timeout` from `limits.engine.ready_timeout_s`); `EngineEnvironment engine_environment_for(const std::filesystem::path& state_dir, const std::optional<std::filesystem::path>& cache_root, std::vector<std::filesystem::path> lib_dirs)` (CUDA cache under `roots.cache` when set); `ServerFixture::wait_engine_ready()`.

- [ ] **Step 1: Write the failing tests**

`tests/server_cli_test.cpp` (SP1's binary; add to its `run_cases` list and to the first `secure_frontend_add_cases(secure_frontend_server_cli_tests ...)` call):

```cpp
void cli_sp2_flags_map_to_schema_keys() {
  const char* argv[] = {"strata-secure-server", "--state-dir", "/srv/state", "--engine-lib-dir", "/opt/a",
                        "--engine-lib-dir", "/opt/b", "--gpu", "0,1", "--allow-new-privileges"};
  const auto parsed = secure_frontend::parse_server_command_line(static_cast<int>(std::size(argv)), argv);
  const auto* invocation = std::get_if<secure_frontend::ServerInvocation>(&parsed);
  expect(invocation != nullptr, "the SP2 options parse");
  if (invocation == nullptr) return;
  const auto& entries = invocation->flags.entries;
  const auto text = [&entries](const std::string_view key) {
    const auto found = entries.find(key);
    return found != entries.end() && std::holds_alternative<std::string>(found->second.value) ? std::get<std::string>(found->second.value)
                                                                                              : std::string{"<missing>"};
  };
  expect(text("roots.state") == "/srv/state", "--state-dir sets roots.state");
  expect(text("engine.gpu") == "0,1", "--gpu sets engine.gpu");
  const auto libs = entries.find("engine.lib_dirs");
  expect(libs != entries.end() && std::holds_alternative<std::vector<std::string>>(libs->second.value) &&
             std::get<std::vector<std::string>>(libs->second.value) == std::vector<std::string>{"/opt/a", "/opt/b"},
         "--engine-lib-dir appends to engine.lib_dirs");
  const auto nnp = entries.find("engine.allow_new_privileges");
  expect(nnp != entries.end() && std::holds_alternative<bool>(nnp->second.value) && std::get<bool>(nnp->second.value),
         "--allow-new-privileges sets engine.allow_new_privileges");
  for (const char* key : {"roots.state", "engine.lib_dirs", "engine.gpu", "engine.allow_new_privileges"}) {
    expect(secure_frontend::find_config_key(key) != nullptr, std::string("schema key ") + key);
  }
  const char* bad[] = {"strata-secure-server", "--gpu", "0,0"};
  const auto bad_parsed = secure_frontend::parse_server_command_line(3, bad);
  const auto* bad_invocation = std::get_if<secure_frontend::ServerInvocation>(&bad_parsed);
  const auto validated = bad_invocation != nullptr ? secure_frontend::load_server_config(*bad_invocation, nullptr)
                                                   : secure_frontend::ConfigValidationResult{secure_frontend::ConfigErrors{}};
  const auto* errors = std::get_if<secure_frontend::ConfigErrors>(&validated);
  bool gpu_issue = false;
  for (const auto& issue : errors != nullptr ? *errors : secure_frontend::ConfigErrors{}) {
    gpu_issue = gpu_issue || (issue.key_path == "engine.gpu" && issue.reason.find("0,0") == std::string::npos);
  }
  expect(gpu_issue, "a malformed engine.gpu is a validation issue that never echoes the value");
}
```

`tests/subsystem_limits_test.cpp` (SP1 Task 7's binary, which has `config_with(...)`; add to its `run_cases` list and its `secure_frontend_add_cases(secure_frontend_subsystem_limits_tests ...)` call; link `secure_frontend_engine_supervisor` into it):

```cpp
void supervisor_policy_follows_config() {
  const auto config = config_with({{"limits.engine.ready_timeout_s", "7"}});
  const secure_frontend::SupervisorConfig supervisor = secure_frontend::supervisor_config(*config);
  expect(supervisor.policy.ready_timeout == std::chrono::seconds(7), "limits.engine.ready_timeout_s drives RestartPolicy::ready_timeout");
  expect(supervisor.chat_limits.ready_timeout == std::chrono::seconds(7), "and the chat limits agree");
  const secure_frontend::RestartPolicy defaults{};
  expect(defaults.ready_timeout == std::chrono::seconds(secure_frontend::config_defaults::kEngineReadyTimeoutS),
         "the policy default is SP1's 900 s constant, not a second copy");
  expect(supervisor.policy.max_consecutive_failures == secure_frontend::config_defaults::kMaxConsecutiveFailures &&
             supervisor.policy.backoff_initial == secure_frontend::config_defaults::kBackoffInitial,
         "the other policy values come from config_defaults.hpp");
}
```

`tests/engine_supervisor_test.cpp` (append to the `SECURE_FRONTEND_SUPERVISOR_UNIT_CASES` line and the `run_cases` list):

```cpp
void engine_environment_uses_cache_root_when_set() {
  const std::filesystem::path state{"/srv/state"};
  const auto plain = secure_frontend::engine_environment_for(state, std::nullopt, {"/opt/engine"});
  expect(plain.cuda_cache_dir == state / "cache" / "cuda", "without roots.cache the CUDA cache stays below the state dir");
  expect(plain.temp_dir == state / "engine-tmp", "the per-generation temp root is <state>/engine-tmp");
  expect(plain.lib_dirs == std::vector<std::filesystem::path>{"/opt/engine"}, "lib dirs pass through");
  const auto cached = secure_frontend::engine_environment_for(state, std::filesystem::path{"/var/cache/strata"}, {});
  expect(cached.cuda_cache_dir == std::filesystem::path{"/var/cache/strata"} / "cuda", "roots.cache holds the CUDA cache when set");
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m2 error`
Expected: FAIL — `'supervisor_config' is not a member of 'secure_frontend'` and `'engine_environment_for' is not a member of 'secure_frontend'` (and `kMaxConsecutiveFailures` is not in `config_defaults`). `cli_sp2_flags_map_to_schema_keys` is a regression pin: it passes as soon as it compiles if Tasks 18, 19 and 26 landed their Step 3 rows; if it fails, land the missing row from Step 3 now.
Run: `grep -rln 'supervision_defaults' secure_frontend/src secure_frontend/include secure_frontend/tests | wc -l`
Expected: non-zero (the header still exists and is used).

- [ ] **Step 3: SP2 options as schema keys and `kFlags` rows** (the rows Tasks 18, 19 and 26 applied early; complete any that is missing)

`src/frontend_config.cpp`, `kSchema` (grow the `std::array` size by 4), after the `roots.cache` row:

```cpp
    {"roots.state", T::string, false, 0, 0, 0, false, "", "--state-dir",
     "Absolute, existing owner-only directory for engine state: the engine's cwd, per-generation temp and (without roots.cache) the CUDA cache. Required to serve."},
```

after the `engine.args` row:

```cpp
    {"engine.lib_dirs", T::string_array, false, 0, 0, 0, false, "", "--engine-lib-dir (repeatable)",
     "Absolute library directories for the engine (default: the engine executable's directory)."},
    {"engine.gpu", T::string, false, 0, 0, 0, false, "", "--gpu",
     "GPU device indices for the engine, e.g. 0 or 0,1 (distinct, 0-255, at most 16); rendered as the engine's --gpu argument, never the environment."},
    {"engine.allow_new_privileges", T::boolean, false, 0, 0, 0, false, "", "--allow-new-privileges",
     "Linux: do not set PR_SET_NO_NEW_PRIVS on the engine (default: set). Only for hosts whose NVIDIA driver must run setuid nvidia-modprobe to create /dev/nvidia-uvm on first use."},
```

`include/secure_frontend/frontend_config.hpp`: `Roots` gains `std::optional<std::filesystem::path> state;` (last member); `EngineLaunch` gains, after `args`:

```cpp
  std::vector<std::filesystem::path> lib_dirs;  // engine.lib_dirs, each absolute
  std::vector<unsigned> gpus;                   // engine.gpu, parsed by parse_gpu_list
  bool allow_new_privileges{false};             // engine.allow_new_privileges
```

`validate()`: after `const auto cache = absolute_root(read, errors, "roots.cache");` add `const auto state = absolute_root(read, errors, "roots.state");`; after the `engine.args` checks add:

```cpp
  std::vector<std::filesystem::path> lib_dirs;
  for (const auto& text : read.strings("engine.lib_dirs")) {
    const std::filesystem::path directory{text};
    if (text.empty() || text.find('\0') != std::string::npos || !directory.is_absolute()) {
      add(errors, "engine.lib_dirs", ConfigIssueCode::invalid_value, "every entry must be an absolute directory");
      break;
    }
    lib_dirs.push_back(directory);
  }
  std::vector<unsigned> gpus;
  if (const auto gpu_text = read.string("engine.gpu")) {
    if (const auto parsed = parse_gpu_list(*gpu_text)) {
      if (profile == EngineProfile::glm53) {
        add(errors, "engine.gpu", ConfigIssueCode::conflicting_values, "is not supported with engine.profile glm53");
      } else {
        gpus = *parsed;
      }
    } else {
      add(errors, "engine.gpu", ConfigIssueCode::invalid_value,
          "must be a comma-separated list of distinct device indices 0-255, at most 16");
    }
  }
  const bool allow_new_privileges = read.boolean("engine.allow_new_privileges");
```

and the constructors at the end become `Roots roots{*models, *tokenizer, secret, logs, cache, state};` and `EngineLaunch engine{profile, *executable, std::move(*model_id), args, std::move(lib_dirs), std::move(gpus), allow_new_privileges};` (after SP1 Task 9 the root and executable types are SP1's canonical types; keep SP1's spelling and only append the new arguments). `describe()` gains, after the `roots.cache` and `engine.args` lines respectively:

```cpp
  line("roots.state", path_or_unset(c.roots.state));
```

```cpp
  std::string libs;
  for (const auto& directory : c.engine.lib_dirs) libs += (libs.empty() ? "" : ",") + quoted(directory.string());
  line("engine.lib_dirs", "[" + libs + "]");
  std::string gpu_list;
  for (const unsigned device : c.engine.gpus) gpu_list += (gpu_list.empty() ? "" : ",") + std::to_string(device);
  line("engine.gpu", gpu_list.empty() ? std::string{"(unset)"} : quoted(gpu_list));
  line("engine.allow_new_privileges", c.engine.allow_new_privileges ? "true" : "false");
```

(`#include "secure_frontend/engine_arguments.hpp"` for `parse_gpu_list`, if SP1's file does not already include it.) `src/server_cli.cpp`, `kFlags` (grow the `std::array` size by 4), after the `--secret-root` row:

```cpp
    {"--state-dir", "roots.state", FlagKind::string},
    {"--engine-lib-dir", "engine.lib_dirs", FlagKind::append},
    {"--gpu", "engine.gpu", FlagKind::string},
    {"--allow-new-privileges", "engine.allow_new_privileges", FlagKind::boolean},
```

`secure_server_main.cpp` (SP1's): the serve path refuses to start without the state root, before anything is bound or spawned:

```cpp
  if (!config.roots.state) {
    std::cerr << "roots.state (--state-dir) is required to serve.\n";
    return EXIT_FAILURE;
  }
```

`tests/support/server_fixture.hpp` (SP1's e2e fixture): the constructor creates `dir / "state"` with `make_private_directory`; `base_flags()` appends `"--state-dir", (dir / "state").string()` and `base_literals()` adds `{"roots.state", json_literal((dir / "state").string())}`, so every SP1 e2e case starts a server that can serve.

- [ ] **Step 4: Fold `supervision_defaults.hpp` into `config_defaults.hpp`; the supervisor follows the config**

`include/secure_frontend/config_defaults.hpp`, inside `namespace secure_frontend::config_defaults`, append (add `#include <chrono>` and `<cstddef>`):

```cpp
// Supervision (SP2). Restart policy (spec §3.3, owner decision 2026-10-13);
// the READY timeout is kEngineReadyTimeoutS above, not a second copy.
inline constexpr std::uint32_t kMaxConsecutiveFailures = 3U;
inline constexpr std::chrono::milliseconds kBackoffInitial{1'000};
inline constexpr std::uint32_t kBackoffFactor = 2U;
inline constexpr std::chrono::milliseconds kBackoffMax{60'000};
inline constexpr std::chrono::milliseconds kHealthyWindow{300'000};
inline constexpr std::chrono::milliseconds kReapTimeout{30'000};
inline constexpr std::chrono::milliseconds kMonitorFallbackPoll{100};  // kernels without pidfd_open
inline constexpr std::chrono::milliseconds kReadyPollSlice{100};       // READY waits stay responsive to shutdown
inline constexpr std::chrono::milliseconds kShutdownStdinWait{2'000};
inline constexpr std::chrono::milliseconds kShutdownTermWait{1'000};
inline constexpr std::chrono::milliseconds kShutdownBound{5'000};
inline constexpr std::chrono::milliseconds kStdinWriteDeadline{1'000};
inline constexpr std::size_t kStdinWriteChunk = 65'536U;
inline constexpr bool kNoNewPrivilegesDefault = true;
```

Then `git rm secure_frontend/include/secure_frontend/supervision_defaults.hpp` and, in every file `grep -rln supervision_defaults secure_frontend` lists, replace `#include "secure_frontend/supervision_defaults.hpp"` with `#include "secure_frontend/config_defaults.hpp"` and the qualifier `supervision_defaults::` with `config_defaults::`. In `restart_policy.hpp` the one changed member is:

```cpp
  std::chrono::milliseconds ready_timeout{std::chrono::seconds(config_defaults::kEngineReadyTimeoutS)};
```

`include/secure_frontend/subsystem_limits.hpp` (SP1's) gains `#include "secure_frontend/engine_supervisor.hpp"` and

```cpp
// The supervisor's configuration from the validated config: chat limits as
// chat_execution_limits(), READY deadline from limits.engine.ready_timeout_s,
// every other policy value from config_defaults.hpp.
[[nodiscard]] SupervisorConfig supervisor_config(const FrontendConfig& config);
```

`src/subsystem_limits.cpp`:

```cpp
SupervisorConfig supervisor_config(const FrontendConfig& config) {
  SupervisorConfig supervisor;
  supervisor.chat_limits = chat_execution_limits(config);
  supervisor.policy.ready_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(config.limits.engine.ready_timeout);
  supervisor.profile = config.engine.profile == EngineProfile::glm53 ? NativeChatProfile::glm53 : NativeChatProfile::qwen;
  supervisor.flavor = config.engine.profile == EngineProfile::glm53 ? EngineProtocolFlavor::lighttransport_glm53
                                                                     : EngineProtocolFlavor::strata_0143;
  return supervisor;
}
```

and `secure_frontend_subsystem_limits` links `secure_frontend_engine_supervisor` (CMake: `target_link_libraries(secure_frontend_subsystem_limits PUBLIC secure_frontend_engine_supervisor)`). In SP1's `secure_server_main.cpp` the block Task 6b wrote that fills `supervisor_config` field by field becomes `secure_frontend::SupervisorConfig supervisor_config = secure_frontend::supervisor_config(config);` followed by the `no_new_privileges_hint` line from Task 26.

- [ ] **Step 5: CUDA cache under `roots.cache`; pipe names from SP1's CSPRNG**

`engine_environment.hpp`, after `render_engine_environment`:

```cpp
// The engine environment for a validated config: temp root <state>/engine-tmp,
// CUDA cache <cache root>/cuda when roots.cache is set, else <state>/cache/cuda.
[[nodiscard]] EngineEnvironment engine_environment_for(const std::filesystem::path& state_dir,
                                                       const std::optional<std::filesystem::path>& cache_root,
                                                       std::vector<std::filesystem::path> lib_dirs);
```

`engine_environment.cpp`:

```cpp
EngineEnvironment engine_environment_for(const std::filesystem::path& state_dir, const std::optional<std::filesystem::path>& cache_root,
                                         std::vector<std::filesystem::path> lib_dirs) {
  EngineEnvironment environment;
  environment.lib_dirs = std::move(lib_dirs);
  environment.temp_dir = state_dir / "engine-tmp";
  environment.cuda_cache_dir = cache_root ? *cache_root / "cuda" : state_dir / "cache" / "cuda";
  return environment;
}
```

In SP1's `secure_server_main.cpp`, Task 18's directory loop creates `environment.cuda_cache_dir` instead of the fixed `state_dir / "cache" / "cuda"` (SP1 already created `roots.cache` 0700 when set), and `plan.environment = secure_frontend::engine_environment_for(state_dir, config.roots.cache, std::move(lib_dirs));` replaces Task 18's `EngineEnvironment{...}` literal.

`engine_stdio_process.cpp` (Windows): replace `random_pipe_suffix()` with

```cpp
// 128 random bits for the pipe name (spec §3.11), from SP1's fail-closed CSPRNG.
std::optional<std::wstring> random_pipe_suffix() {
  std::array<unsigned char, 16> bytes{};
  if (!fill_random(bytes.data(), bytes.size())) return std::nullopt;
  static constexpr wchar_t kDigits[] = L"0123456789abcdef";
  std::wstring text;
  for (const unsigned char byte : bytes) {
    text.push_back(kDigits[byte >> 4U]);
    text.push_back(kDigits[byte & 0x0fU]);
  }
  return text;
}
```

drop `#include <bcrypt.h>` from it, add `#include "secure_frontend/token_generator.hpp"`, and in CMake replace `bcrypt` in `target_link_libraries(secure_frontend_engine_stdio_process PUBLIC advapi32 bcrypt)` with `target_link_libraries(secure_frontend_engine_stdio_process PUBLIC advapi32 secure_frontend_secrets)` (SP1's `secure_frontend_secrets` links `bcrypt` itself).

- [ ] **Step 6: SP1's e2e chat cases wait for READY** (landed in Task 6b; verify)

Eager READY makes `/health` `503 starting` and a chat `503` until READY is consumed, so an SP1 e2e case that posts a chat right after the listener answers would race it. `tests/support/server_fixture.hpp` (SP1's) gains, in `ServerFixture`:

```cpp
  // SP2: READY is consumed eagerly; until then a chat is a 503. Poll /health
  // until the engine is ready before any case sends a chat.
  [[nodiscard]] bool wait_engine_ready(const std::chrono::milliseconds timeout = std::chrono::seconds(10)) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (contains(request("GET", "/health", bearer()), "\"engine\":\"ready\"")) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }
```

(`#include <thread>`), and every SP1 e2e case that sends `POST /v1/chat/completions` and expects a completion calls `expect(f.wait_engine_ready(), "engine ready before the chat");` first. List them with `grep -n '"/v1/chat/completions"' secure_frontend/tests/*.cpp`; a case that asserts a 503 for a READY timeout (`ready_timeout_is_configurable` with `ready_timeout_s=1`) does not wait — with the config-driven READY deadline from Step 4 its engine is killed at 1 s and the chat is a 503 either way.

- [ ] **Step 7: Run to verify**

Run: `grep -rln 'supervision_defaults' secure_frontend/src secure_frontend/include secure_frontend/tests | wc -l` → `0`.
Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc 2>&1 | tail -4` → `100% tests passed, 0 tests failed out of <Task 28 total> + 3` (new: `secure_frontend_server_cli_tests.cli_sp2_flags_map_to_schema_keys`, `secure_frontend_subsystem_limits_tests.supervisor_policy_follows_config`, `secure_frontend_engine_supervisor_tests.engine_environment_uses_cache_root_when_set`; 2 skipped).
Run: `cmake --build --preset windows-llvm-mingw-cross 2>&1 | tail -2 && ctest --preset windows-llvm-mingw-cross 2>&1 | tail -3` → `-Werror` clean; `out of <Task 28 wine total> + 3` (all three are unit cases). `grep -n BCryptGenRandom secure_frontend/src/engine_stdio_process.cpp` prints nothing.

- [ ] **Step 8: Handoff and README** — add the row; in `secure_frontend/README.md`'s configuration key table (SP1 Task 24 generates it from `kSchema`; regenerate it the way that task says) the four new keys appear.

```markdown
| SP1 integration | SP2 options are SP1 schema keys with `kFlags` rows: `--state-dir` → `roots.state` (required to serve), `--engine-lib-dir` → `engine.lib_dirs`, `--gpu` → `engine.gpu`, `--allow-new-privileges` → `engine.allow_new_privileges`. `limits.engine.ready_timeout_s` drives `RestartPolicy::ready_timeout` (`supervisor_config`). The CUDA cache moves under `roots.cache` when set. `supervision_defaults.hpp` is folded into `config_defaults.hpp`. The Windows pipe name uses SP1's `fill_random`. SP1's e2e chat cases poll `/health` until `engine=ready`. | `cli_sp2_flags_map_to_schema_keys`, `supervisor_policy_follows_config`, `engine_environment_uses_cache_root_when_set` | <recorded linux line> (2 skipped); wine <recorded> |
```

- [ ] **Step 9: Commit**

```bash
git add -A secure_frontend docs/AGENT_HANDOFF.md
git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "secure_frontend: SP2 options as SP1 config keys; config-driven READY deadline; one defaults header; SP1 CSPRNG for pipe names" \
  -m "Tests: secure_frontend_server_cli_tests.cli_sp2_flags_map_to_schema_keys, secure_frontend_subsystem_limits_tests.supervisor_policy_follows_config, secure_frontend_engine_supervisor_tests.engine_environment_uses_cache_root_when_set. roots.state, engine.lib_dirs, engine.gpu, engine.allow_new_privileges; CUDA cache under roots.cache; supervision_defaults.hpp folded into config_defaults.hpp; SP1 e2e chat cases wait for engine=ready." \
  -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: <session link>"
```

---

## Self-review (done while writing; updated after the pre-flight review)

**1. Spec coverage (every §4 row → task; CTest names are `<binary>.<case>`):**

| Spec row | Task | Test(s) |
| --- | --- | --- |
| F1 | 1 | `fake_engine_modes_selftest` |
| F2 | 2 | `fake_engine_dump_is_parseable` |
| F3 | 3 (harness), 7 (`ManualClock` / backoff timer) | `harness_server_lifecycle_selftest` |
| K1, K3, K5 | 4 | `deadline_zero_timeout_is_immediate_everywhere`; K3/K5 review |
| V1 (PROC-04) | 5 | `proc04_exit_status_is_classified` |
| V2, V11 (PROC-04) | 6a (core), 6b (server) | `proc04_ready_is_eager_and_health_starting_is_503`, `proc04_chat_during_restart_is_generic_503` (+ `supervisor_health_literals_per_state`) |
| V3 (PROC-04) | 7 | `proc04_restart_backoff_then_circuit_open`, `proc04_healthy_window_resets_failure_count`, `proc04_recovers_after_transient_crash` |
| V4 (PROC-04) | 8 | `proc04_monitor_detects_idle_crash` (asserts the Linux pidfd path) (+ `proc04_monitor_fallback_without_pidfd`) |
| V5 (PROC-04) | 9 | `proc04_fatal_prefix_kills_engine` |
| V6, V7 (PROC-04) | 10 | `proc04_ready_timeout_kills_and_counts`, `proc04_drain_failure_kills_and_restarts` |
| V8 (PROC-04) | 6b | `proc04_abort_in_generation_is_generic_503` |
| V9, V10 (PROC-04) | 11 | `proc04_no_leak_across_restarts`, `proc04_unreaped_child_opens_circuit` |
| C1 (PROC-05) | 12 | `proc05_group_killed_after_leader_reaped` |
| C2 (PROC-05) | 13 | `proc05_setsid_grandchild_is_reaped` |
| C3 (PROC-05) | 14 (+16) | `proc05_frontend_sigkill_kills_engine` |
| C4, C7 (PROC-05) | 15 | `proc05_frontend_sigterm_stops_all_descendants`, `proc05_frontend_sigint_stops_all_descendants`, `proc05_frontend_has_only_engine_children` |
| C5 (PROC-05) | 16 | `proc05_cgroup_kill_when_delegated` |
| C6 (PROC-05/04) | 17 (launch, supervisor case), 15 (server cases) | `proc04_windows_abort_does_not_stall`; `proc05_windows_ctrl_break_stops_all_descendants`, `proc05_windows_terminate_frontend_kills_job` |
| N1, N2, N3 (PROC-02) | 18 | `proc02_child_env_is_exact_allowlist`, `child_receives_exact_argv_and_env`, `proc02_engine_process_env_checks_windows_names` |
| §3.9 typed `--gpu` (owner decision) | 19 | `engine_args_render_typed_gpu_and_deny_gpu_passthrough`, `gpu_list_rejects_malformed_values`, `strata_secure_server_rejects_malformed_gpu` |
| N4 (PROC-02) | 20 | `proc02_child_signal_state_is_default` |
| N5 (PROC-02) | 21 | `proc02_canary_descriptors_absent`, `proc02_canary_handles_absent` |
| N6, R1, K2 (PROC-02/03/04) | 22 | `proc02_stdin_pipe_dacl_is_owner_only`, `proc03_windows_pipe_second_open_fails`, `proc04_windows_write_timeout_kills_engine` |
| A1, A2 (PROC-01) | 23 (+19 adds `--gpu` to A2) | `proc01_windows_argv_round_trip`, `proc01_server_passes_literal_argv` |
| R2, R3 (PROC-03) | 24 | `proc03_other_uid_cannot_open_engine_fd`, `proc03_same_uid_reopen_is_accepted_risk` |
| G1 (PROC-06) | 25 | `proc06_child_ids_and_cwd` (relative identity check, owner-acknowledged deviation) |
| G3 (PROC-06) | 25 | `proc06_elevation_required_is_spawn_failed` — UNVERIFIED in CI and wine; validation-kit item in the handoff, not a CTest test (owner-acknowledged deviation) |
| G2, G4 (PROC-06) | 26 | `proc06_child_has_no_new_privs` (+ `proc06_nnp_startup_hint_names_nvidia_uvm_fix`), `proc06_refuses_privileged_start` |
| X1, X2 (EXT-01, EXT-02) | 27 | `ext01_extension_fields_spawn_nothing`, `ext02_extension_env_never_reaches_child`, `ext02_rejected_before_spawn` |
| K4, K6 | 28 | review + full run |
| SP1 integration (spec §2) | 29 | `cli_sp2_flags_map_to_schema_keys`, `supervisor_policy_follows_config`, `engine_environment_uses_cache_root_when_set` |
| §3.1 thread model | 6a (supervisor), 14 (fork from a long-lived thread) | V2/V4/C3 tests |
| §3.7 shutdown order and 5 s bound | 15 | C4 tests, Review Focus 3 and 5 |
| §8 acceptance 1–5 | 28 Step 6, 29 Step 7 + MSYS2 after push | — |

Owner decisions: restart 3 / 1 s→60 s / 300 s / 900 s / reap 30 s (Task 6a constants — `config_defaults.hpp` after Task 29 —, Task 7 tracker, Task 10 READY deadline, Task 11 reap); typed `--gpu` in the deny-list for both profiles, no GPU env (Tasks 18–19); subreaper default + `cgroup.kill` only when delegated (Tasks 13, 16); overlapped stdin writes now (Task 22); NNP default ON with `--allow-new-privileges` and the `/dev/nvidia-uvm` hint, refuse root/elevated ON with no test override (Tasks 3, 26); G1 relative and G3 validation-kit (Global Constraints, Task 25).

**2. Placeholder scan:** no "TBD"/"TODO"/"similar to"; every code step carries code, including the `engine_process.cpp` twins in Tasks 5, 17, 20 and 25 and Task 16's `prepare` callers. `<recorded ...>` in handoff rows and the `<Task n total> + k` / N₀ / W₀ counts are deliberate: the executor writes the line it actually observed (SP1 Task 2's per-case totals cannot be known while writing).

**3. Type consistency:** `EngineInstance` (not the spec's `EngineGeneration`, which already names the protocol result struct in `engine_protocol_client.hpp`) is used in Tasks 6a–11; `Lease::{chat, protocol, process, generation}`; `SupervisorConfig` fields are named identically in Tasks 6a–29; `KillReason::{none, fatal_line, ready_timeout, drain_failed, write_timeout, protocol_failed, shutdown}` everywhere; `EngineProtocolErrorCode::{fatal_engine_line, abandoned}` appended in Tasks 9 and 10 and mapped in `kill_reason_for`; `EngineLaunchPlan{command, environment{}, use_delegated_cgroup{false}}` grows in Tasks 16 and 18 with the factory rewritten each time in full; `EngineCommand` gains `cgroup_procs{}` (16), `working_directory{}` (25), `no_new_privileges{false}` (26) in that declaration order. Designated initializers: every NEW member of an existing aggregate (`EngineCommand`, `EngineLaunchPlan`, `EngineEnvironment`'s optional members, `test_support::ServerOptions`) has a `{}` (or `{false}`) default member initializer, so the existing designated initializers that omit them — `{.executable = ..., .arguments = ..., .environment = {}}`, `{.lib_dirs = ..., .temp_dir = ..., .cuda_cache_dir = ...}`, `{.engine_args = ..., .extra_args = {}, .extra_env = {}, .engine = {}}` — compile under `-Werror` with g++ 11.4 (`-Wmissing-field-initializers`) and clang 23 (`-Wmissing-designated-field-initializers`). (The earlier claim that "every designated initializer in the tests follows" the declaration order was true but insufficient: order alone does not silence the missing-initializer warning.) CMake case lists `SECURE_FRONTEND_{SERVER,CHILD,SUPERVISOR}_*_CASES` are defined in Tasks 3/5/6a and only appended to later; every registration goes through SP1's `secure_frontend_add_cases`, and every `set_tests_properties` uses the full `<binary>.<case>` name.

**4. Review Focus:** all six pinned — Task 18 (`state_dir_must_be_owner_only`), Task 7 (`proc04_missing_engine_opens_circuit_without_spinning`, `proc04_client_err_lines_open_circuit_until_restart`), Task 15 (`proc05_sigterm_during_generation_exits_cleanly`, `proc05_double_sigterm_is_idempotent`), Task 19 (`gpu_list_rejects_malformed_values`, `strata_secure_server_rejects_malformed_gpu`).

**5. Dependencies after the reorder:** the parallel-safe tasks use no server wiring: Task 10's V8 server case moved to Task 6b, Task 17's two server cases moved to Task 15, Task 14's C3 case waits for the grandchild pidfile instead of `/health` (it runs against the SP0 server), and Task 23's A2 case drops `--gpu` (Task 19 adds it back). Code anchors that an earlier task changes are quoted (Task 22's success line, Task 25's `CreateProcessW` after Task 17, Task 16's success line after Task 8).

**6. Pre-flight review fixes (controller-ruled), one line each:**
1. Base hash `0c783345` → `7ddcd755`; `engine_stdio_process.cpp` citations +3 (`running()` 175-190, `read_line` 213/214/219/234/252-256, `TerminateJobObject` 284/155, `start` reset 93, POSIX success 171, fork 166-168, pipe block 103-120, success 157, `write_line` 197, POSIX `terminate` 285-313); every direct `wine …exe` call is prefixed `WINEPREFIX=$PWD/out/wineprefix`.
2. Every new aggregate member has a `{}` default initializer (`EngineCommand::cgroup_procs`, `EngineCommand::working_directory`, `EngineLaunchPlan::environment`, `EngineEnvironment`'s three optionals, `ServerOptions::state_dir` and all other `ServerOptions` members); Global Constraints rule added and the self-review's designated-initializer claim corrected (item 3).
3. Task 5: Windows `terminate()` waits for the Job kill and then calls `try_reap()`, so `exit_status()` is set and "a supervisor kill classifies as ours" holds on Windows.
4. Task 8: on Linux the V4 test asserts `monitor_handle() != kNoMonitorHandle` (pidfd path proven); Task 19: the GLM branch refuses `--gpu` via `--engine-arg` (tested), and after the SP1 merge the change goes into `validate_engine_user_arguments`.
5. Task 22: K2 uses `generation_timeout = 2 s`, a 70,000-token prompt of id 239 (three digits) giving a ~280 KB GEN line (> 4× 64 KiB), and keeps a 300 s CTest timeout.
6. Task 18: `<state>/engine-tmp/<n>` is removed by `release_resources()` after the reap (`end_generation`, `shutdown_generation`), `engine-tmp/*` is cleared at startup (`clear_directory_contents`), the factory comment no longer claims removal of the previous generation's directory, and the N1 test checks the removal.
7. Task 1: the fake calls `SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX)` first on Windows; Task 3: the harness destructor kills every descendant it has seen and reaps re-parented orphans (Linux child subreaper; Windows Job terminated).
8. Task 16: `detect()` rejects a cgroup with a non-empty `cgroup.subtree_control`; Task 25: `/MANIFEST:NO` for the `.rc` target under MSVC; Task 24: the workflow header comment documents the `sudo -n -u nobody` use for R2.
9. Task 3: harness token is 43 base64url characters; fixture dirs are 0700 and files 0600 (`make_private_directory`, `write_file`, `write_tokenizer_pack`, Task 18's state dir).
10. The restricted-token launch moved from Task 26 to Task 3: LUA token via `CreateRestrictedToken` + `CreateProcessAsUserW`, user grant on every test directory (`grant_current_user` in `make_temp_dir`), non-elevation asserted (harness and self-test), Safer `NORMALUSER` fallback, else stop and ask the owner; no `--allow-elevated-for-tests` flag anywhere.
11. Owner-acknowledged deviations recorded in Global Constraints and Task 25: G1 is a relative check (never MORE elevated / higher integrity than the frontend); G3 is unverified in CI and wine and is a handoff validation-kit item, not a skipped test (the controller's "G3 (refuse elevated start)" is read as spec G3, "elevation never requested / 740 → `spawn_failed`", since that is the row CI and wine cannot observe; G4's refusal stays a registered unit test). Only two tests may skip.
12. SP1 coordination: (a) Global Constraints paragraph makes SP1 Task 2 a shared pre-task on `main` (exact signature `secure_frontend_add_cases(<binary> <case>... [ARGS ...] [LABELS ...] [TIMEOUT s])` — positional cases, no `CASES` keyword; the wine preset's `filter.exclude.label` is the preset form of `-LE windows-runtime`); Tasks 1–5 and every later registration use SP1's helpers and `<binary>.<case>` names; `cmake/SecureFrontendCases.cmake` and `case_main.hpp` are gone (skips use `test_support::skip`). (b) SP2's fake is `tests/supervision_fake_engine_main.cpp` / `strata-supervision-fake-engine` everywhere. (c) Task 6 split into 6a (core, unit tests, `adopt_ready`; no `secure_server_main.cpp`) and 6b (server wiring); 6b, 15's Step 6, 16, 18's Step 4, 19, 25 and 26 carry the WAITS FOR SP1 MERGE banner with a mapping paragraph (`kFlags` rows `--state-dir`→`roots.state`, `--engine-lib-dir`→`engine.lib_dirs`, `--gpu`→`engine.gpu`, `--allow-new-privileges`→`engine.allow_new_privileges`). (d) New Task 29 "Post-SP1 integration" with tests: schema keys + `kFlags` rows, CUDA cache under `roots.cache`, `limits.engine.ready_timeout_s` → `RestartPolicy::ready_timeout`, `supervision_defaults.hpp` folded into `config_defaults.hpp`, pipe names from `fill_random`, SP1 e2e chat cases poll `/health` until `engine=ready`. (e) Tasks reordered: 1–5, 6a, 7–14, 17, 20–24, then 6b, 15, 16, 18, 19, 25–28, 29. (f) Review Focus 6 "three client-triggerable ERR lines open the circuit until a restart", pinned by `proc04_client_err_lines_open_circuit_until_restart` (Task 7).
- Also fixed: File map puts `cgroup_scope.hpp` under `include/secure_frontend/`; the header Interfaces block lists `query_windows_system_values`, `kNeverForwardedEnvironment`, `EngineCgroups`, `kNoNewPrivilegesHint` (and `clear_directory_contents`, `set_generation_temp_dir`); the prose-only `engine_process.cpp` twin steps (Tasks 5, 17, 20, 25) and Task 16's `prepare` callers now carry real code; Global Constraints note that clang is not installed locally, the cross build's clang catches most clang-only diagnostics, and CI's `linux-clang` is authoritative.

**Known judgement calls for the implementer:** counts are relative (`<Task n total> + k`); a total that differs only because SP1 registered a case differently is not a failure if the named tests all exist and pass. Windows-only code is written against MinGW-w64 headers; if a declaration is missing (for example `GetNamedPipeClientProcessId` behind `_WIN32_WINNT`, or `SaferComputeTokenFromLevel` in `winsafer.h`), resolve it with `GetProcAddress` exactly as `proc02_canary_handles_absent` does, never by raising `_WIN32_WINNT` silently. A WAITS task whose mapping note does not fit SP1's merged file exactly re-anchors by the quoted statement and keeps the behavior its tests pin; if that is impossible, stop and report.
