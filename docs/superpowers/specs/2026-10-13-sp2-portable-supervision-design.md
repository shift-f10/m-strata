# Sub-project 2 — Portable child supervision of the engine

Date: 2026-10-13. Status: APPROVED by the owner 2026-10-13 19:08 UTC — decisions: restart 3/1 s→60 s/300 s window/900 s READY; typed `--gpu`; subreaper default + cgroup.kill when delegated; overlapped stdin writes NOW; PR_SET_NO_NEW_PRIVS DEFAULT ON and refuse root/elevated start ON (owner overrode the orchestrator's default-off recommendation; validation kit must include an nvidia-modprobe check). Parent
effort: Secure Strata full release path (sub-projects 0–7), see
`docs/AGENT_HANDOFF.md` Phase 2. Fact base:
`docs/superpowers/research/sp2-supervision-proc.md` (cited below as "research
§n"). Siblings drafted in parallel: the SP1 spec (typed configuration, roots,
timeouts) and the product-experience spec (CLI `run` auto-start, `ps`, `stop`).

## 1. Purpose

SP0 made the launch path correct for one child that starts once. SP2 turns it
into supervision a release can rely on:

1. An `EngineSupervisor` owns the engine child for the frontend's lifetime:
   eager READY, exit-status capture, bounded restart with backoff, a circuit
   breaker, and a monitor that notices death between requests.
2. No descendant outlives the frontend, however the frontend stops.
3. The child gets a typed environment allowlist instead of `{}`, a reset
   signal state, a scoped cwd, an owner-only pipe, and no stray handles.
4. Every `PROC-01`..`PROC-06`, `EXT-01`, `EXT-02` row in
   `tests/security/README.md` has CTest tests named by acceptance ID.

SP2 changes no engine code (`upstream/src/`). The `run`/`ps`/`stop` CLI builds on this
supervisor but is not part of SP2.

## 2. Constraints

- SP0 §2 still holds (no root, g++ 11.4, wine as smoke only, no third-party
  frontend dependencies, nothing pushed by agents, handoff updated in-task,
  regression test before every fix).
- Windows runtime claims (Job Objects, DACLs, handle inheritance, console
  events, exit codes) come only from the blocking MSYS2 `CLANG64` CI job.
- No GPU claim. Whether a real CUDA/ROCm engine starts with the allowlist in
  §3.9 is a step on the offline GPU box that the owner runs; until then it is
  recorded as unverified.
- **Assumed from the SP1 spec:** typed, validated `ready_timeout` (default
  900 s, upstream `STRATA_ENGINE_READY_S`), the §3.3 policy numbers, grace
  values, install root and canonical engine `lib_dirs`, frontend-owned
  `state_dir`/`cache_dir`, optional `gpu` list, `hipblaslt_tuning` path and
  watchdog integers, and the CSPRNG. Until SP1 lands these are named
  constants in one header (as SP0 did for `kRequestDeadline`).
- **Assumed from the product-experience spec:** `strata run` polls `/health`
  until `ready`; `strata stop` sends SIGTERM / `CTRL_BREAK_EVENT` and waits;
  `strata ps` reads `/health`. SP2 adds no admin route.

## 3. Decisions

### 3.1 One supervisor thread owns the child

`EngineSupervisor` runs one long-lived thread that spawns, reaps, watches, and
kills every engine generation. It is the only code that calls `waitpid` (or
waits on the Windows process handle) for engine processes; the request path
never calls `running()`. The serving thread borrows the current generation
through a lease:

```
enum class SupervisorState { starting, ready, generating, stopping,
                             backoff, circuit_open, shutting_down, stopped };
struct EngineGeneration;            // process + protocol client + chat service
class EngineSupervisor {
 public:
  std::optional<Lease> acquire();   // only in `ready`; holds shared_ptr<EngineGeneration>
  void report_fatal(const Lease&, KillReason);   // fatal line, drain fail, write timeout
  HealthSnapshot snapshot() const;  // state + recovery literal, sampled once
  void shutdown(std::chrono::milliseconds bound);
};
```

Reasons: `PR_SET_PDEATHSIG` fires when the forking *thread* exits (research
§6), so forks must come from a thread that lives as long as the process; one
reaper never loses exit status to a racing `waitpid(WNOHANG)`; the serving
loop stays single-threaded for SP3. Rejected: checking `running()` from the
serving thread (death unseen while `accept` waits); reaping from both threads
(status lost, as today).

### 3.2 Fresh objects per child generation

Each spawn builds a new `EngineGeneration` (process, protocol client, chat
service) behind a `shared_ptr` that a lease keeps alive until its request ends.
Rejected: `reset()` APIs (`received_ready_`, `pending_stdout_`, state each
need one; a missed one leaks across generations, research §1).

### 3.3 Restart policy: exponential, bounded, then a circuit

Defaults (SP1 makes them typed config):

| Field | Default | Meaning |
| --- | --- | --- |
| `max_consecutive_failures` | 3 | the 3rd consecutive failure opens the circuit |
| `backoff_initial` | 1 s | delay after the 1st failure |
| `backoff_factor` | 2 | 1 s, 2 s, 4 s, … |
| `backoff_max` | 60 s | cap, reached only if the owner raises the failure count |
| `healthy_window` | 300 s | continuous `ready`/`generating` time that resets the counter to 0 |
| `ready_timeout` | 900 s | READY deadline per generation |
| `reap_timeout` | 30 s | bound for reaping a SIGKILLed child before the circuit opens |

A *failure* is any unrequested end of a generation, every §3.5 kill, and any
spawn failure, the initial start included. Circuit open is `failed` until the
operator restarts the frontend (`strata stop` + `run`, or the service
manager); there is no in-band reset route.

Rejected: upstream's fixed 15 s × 3 (`restart(tries=3)`, `RESTART_RETRY_S=15`,
research §4). Upstream restarts lazily and blocks the triggering request
through all tries; ours restarts in the background, so a 1 s first delay
recovers a one-off crash (watchdog abort, transient OOM) at no cost, a
deterministic fault (bad `--pack`) stops after three quick attempts, and
exponential growth keeps a raised failure count from looping tightly. Load
time (minutes) dominates all of these delays anyway.

### 3.4 Eager READY; `starting` becomes `503`

The supervisor thread consumes READY right after spawn, within
`ready_timeout`; `/health` keeps its four `engine` literals and `starting`
returns `503` (§3.8). Reconciling SP0: SP0 reverted `starting` to `200`
because READY was consumed lazily, so a healthy idle server sat in `starting`
until the first chat and `503` would have been false. With eager READY,
`starting` means the model is loading, `503` is true, and `strata run` can
wait for `ready`. Upstream always answers `200 "loaded"`; ours stays stricter.
A chat in `starting` gets an immediate generic `503`; it never waits.

### 3.5 Kill triggers

The supervisor kills the whole generation (group/Job SIGKILL, §3.7) and counts
a failure when:

1. a line matches one of upstream's `FATAL_PREFIXES` exactly as a prefix
   (`upstream/serve/server.py:734-735` at v0.1.44.1, research §4) — `"ERR verify: timed out at layer "`,
   `"ERR verify batch: timed out at layer "`, `"ERR verify: an earlier window
   never finished"`, `"ERR verify batch: an earlier window never finished"`.
   The engine lives on but is unusable; a trailing `DONE` is not accepted
   (`_pump`, `upstream/serve/server.py:952-967` at v0.1.44.1). Other `ERR` lines keep SP0 behavior;
2. READY does not arrive within `ready_timeout`;
3. an E1/E2 drain fails (SP0 marks `failed`; SP2 adds kill and restart);
4. a stdin write misses its deadline (§3.10).

### 3.6 Exit-status capture and classification

`EngineStdioProcess` keeps the raw `waitpid` status (POSIX) or
`GetExitCodeProcess` value (Windows) instead of discarding it. The supervisor
classifies it into a fixed literal used only in the frontend's own log line
(`engine generation <n> ended: <kind>`) and never sent to clients:

| Observed | `ExitKind` | Source in engine (research §4) |
| --- | --- | --- |
| exit 0 not requested | `clean_exit` | QUIT/EOF path |
| exit 1 | `error_exit` | `ERR` paths `return 1`; CUDA fault `std::_Exit(1)` (indistinguishable) |
| exit 127 with errno on report fd | `spawn_failed` | SP0 exec report |
| other exit code | `error_exit` | — |
| SIGABRT; Windows 3 or `0xC0000409` | `abort` | watchdog `std::abort()` (UCRT may fast-fail; verify on MSYS2) |
| SIGKILL not sent by us | `killed_external` | likely OOM killer |
| any signal / exit sent by us | `killed_by_supervisor(reason)` | §3.5 |
| other signal; Windows `0xC0000005` etc. | `crashed` | — |

`TerminateJobObject` uses customer code `0x20000001` instead of `1` so our
kill is never confused with an engine `return 1`. `ERROR_ELEVATION_REQUIRED`
(740) maps to `spawn_failed`; never retried via `runas`. All kinds count as
failures; the classification is diagnostic only.

### 3.7 Descendant cleanup (PROC-05)

**Linux.** (a) `terminate` always ends with `kill(-pgid, SIGKILL)`, even after
the leader is reaped — the pgid stays valid while any member lives (research
§6; fixes defect PROC-05(a)). (b) The supervisor process sets
`PR_SET_CHILD_SUBREAPER` once at construction; after every generation ends it
enumerates its own children (`/proc/self/task/*/children`), and SIGKILLs and
reaps every child that is not a live engine leader. This catches `setsid`,
`setpgid` and double-fork escapes (defect PROC-05(b)). The production binary
spawns no other children; a test asserts that. (c) Pre-exec
`PR_SET_PDEATHSIG(SIGKILL)` followed by `getppid()` compared to the pid
captured before `fork`; mismatch → `_exit(127)`. (d) cgroup v2: if the
frontend already runs in a delegated cgroup it can write to (detected, never
created with privileges), each generation goes into a child cgroup and
cleanup writes `cgroup.kill` (5.14+). Optional and off unless detected.

Residual risk (threat model): a frontend SIGKILLed on Linux without a usable
cgroup takes the engine leader with it (PDEATHSIG), but the engine's own
descendants survive; the upstream engine is not known to spawn any. Rejected
as default: cgroup-only (needs `Delegate=yes` / `systemd-run --user --scope`,
absent on many hosts); pidfd-only cleanup (cannot find descendants).

**Frontend signals.** SIGTERM/SIGINT handlers write a byte to a self-pipe the
supervisor thread polls; shutdown then closes engine stdin (engine exits 0 on
EOF), waits 2 s, SIGTERMs the group, waits 1 s, always SIGKILLs the group,
sweeps, and `shutdown()`s the listener so `main` exits 0; past a 5 s bound it
`_exit(1)`s. Windows: `SetConsoleCtrlHandler` (C, BREAK, CLOSE) runs the same
path; `CREATE_NEW_PROCESS_GROUP` keeps console Ctrl-C off the child.
Rejected: a `sigwait` thread (mask must precede every thread, POSIX-only).

**Windows.** Job gains `JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION`;
`SetErrorMode(SEM_NOGPFAULTERRORBOX)` before spawn (inherited), so a crash
never stalls on WER. Tests count via Job `ActiveProcesses`.

### 3.8 State machine

| State | Entered when | Leaves to | `/health` |
| --- | --- | --- | --- |
| `starting` | spawn succeeded | `ready` on READY; `stopping` on exit, fatal line, READY timeout | `503`, `engine=starting` |
| `ready` | READY parsed; generation done | `generating` on `acquire`; `stopping` on monitor-observed exit | `200`, `engine=ready` |
| `generating` | lease taken | `ready` on DONE/ERR; `stopping` on §3.5 trigger or exit | `200`, `engine=generating` |
| `stopping` | any unrequested end or kill | `backoff` once reaped + swept and failures < max; `circuit_open` if failures = max or reap exceeds `reap_timeout` | `503`, `engine=failed`, `recovery=restarting` |
| `backoff` | after `stopping` | `starting` when the delay expires (spawn); spawn failure counts and returns to `backoff`/`circuit_open` | `503`, `engine=failed`, `recovery=restarting` |
| `circuit_open` | consecutive failures reached max | none (operator restarts frontend) | `503`, `engine=failed`, `recovery=exhausted` |
| `shutting_down` | SIGTERM/SIGINT/console event, or `main` exit | `stopped` | `503`, `engine=failed` (listener closes promptly) |
| `stopped` | cleanup finished | — | not served |

A generation that ends after `healthy_window` in `ready`/`generating` resets
the counter to 0 before its own failure counts. `recovery` is a new optional
fixed literal, present only with `engine=failed`, carrying no exit code or
detail. Requests use one `snapshot()` (SP0's sample-once rule).

### 3.9 Typed environment allowlist (PROC-02, EXT-02)

`.environment = {}` is replaced by a closed type rendered by the frontend.
Nothing is copied from the parent environment by name; every value is
generated or comes from validated SP1 config.

```
struct EngineEnvironment {           // rendered to "NAME=value" before fork
  std::vector<path> lib_dirs;        // POSIX LD_LIBRARY_PATH; Windows head of PATH
  path temp_dir;                     // TMPDIR | TEMP + TMP  -> <state_dir>/engine-tmp/<gen>
  path cuda_cache_dir;               // CUDA_CACHE_PATH      -> <cache_dir>/cuda
  std::optional<path> hipblaslt_tuning;   // STRATA_HIPBLASLT_TUNING (under install root)
  std::optional<unsigned> watchdog_s, watchdog_io_s;  // STRATA_WATCHDOG_S / _IO_S
  // Windows only, values from GetSystemWindowsDirectoryW / GetSystemInfo:
  // SystemRoot, WINDIR, SystemDrive, NUMBER_OF_PROCESSORS; PATH tail %SystemRoot%\System32
};
```

POSIX `PATH` is the constant `/usr/bin:/bin` (dropped if the GPU run shows
it unneeded); `HOME` is unset (`CUDA_CACHE_PATH` replaces
`~/.nv/ComputeCache`). Never present, and asserted absent by test:
the token-env variable named by `--token-env`, `STRATA_API_KEY`, `LD_PRELOAD`,
`LD_AUDIT`, `CUDA_LAUNCH_BLOCKING` (engine warns it hangs verify windows), any
other `STRATA_*`, `HOME`, `USERPROFILE`, `APPDATA`, and proxy variables
(`HTTP_PROXY`, `HTTPS_PROXY`, `ALL_PROXY`, lower-case forms). The engine sets
`CUDA_MODULE_LOADING=EAGER` and arch defaults itself (research §4).

**GPU selection by typed `--gpu` argv, not environment.** The engine's
`--gpu LIST` sets `CUDA_DEVICE_ORDER=PCI_BUS_ID` and `CUDA_VISIBLE_DEVICES`
(or HIP) itself (`generate.cpp:1843-1865` at v0.1.44.1). The frontend renders `--gpu` from
the SP1 typed list (non-negative integers), adds `--gpu` to the owned-flag
deny-list for `--engine-arg`, and keeps all three GPU variables out of the
environment. Rejected: frontend-set `CUDA_VISIBLE_DEVICES` etc. (two places
select GPUs). Open question 2. The missing Windows `SystemRoot` is the likely
real-engine breakage today (research "Defects"); N1 lands before any Windows
GPU run.

### 3.10 Overlapped stdin writes on Windows: now

The frontend owns the stdin named pipe, so its end gets `FILE_FLAG_OVERLAPPED`;
writes wait with the remaining deadline and `CancelIoEx` on timeout (a §3.5
kill). The engine's reader thread (`read(0)`/`_read(0)`,
`generate.cpp:9433-9469` at v0.1.44.1) normally drains the pipe, but a hung engine must not
hang the serving thread. Rejected: trusting the advisory buffer size
(unprovable); `CancelSynchronousIo` from the supervisor (couples threads).
Open question 4.

### 3.11 Windows pipe hardening and handle enumeration

`CreateNamedPipeW` gets a `SECURITY_ATTRIBUTES` whose DACL is protected and
grants `GENERIC_ALL` only to the current token user SID (`D:P(A;;GA;;;<sid>)`).
The name gains a 128-bit random suffix from the SP1 CSPRNG. After the parent
opens the client end, `GetNamedPipeClientProcessId` must equal its own pid,
else spawn fails (closes the open-before-us squat window).

Handle tests enumerate **parent-side**: open the child with
`PROCESS_DUP_HANDLE`, snapshot its handles
(`NtQueryInformationProcess(ProcessHandleInformation)`), duplicate each, and
`CompareObjectHandles` (Win10 1607+) against canaries; assert canary absence,
not a count (loader handles vary). Rejected: child-side `NtQueryObject` name
queries (can hang on synchronous pipe handles, research §6).

### 3.12 Child pre-exec state (POSIX) and identity

The parent blocks all signals around `fork` (the self-pipe handler must not
run in the child); the child sets every disposition to `SIG_DFL`, then clears
the mask, then `execve`s, so the engine gets default SIGPIPE, not S1's ignore.
The child `chdir`s (Windows: `lpCurrentDirectory`) to the owner-only
`<state_dir>/engine`, because the engine writes its cwd-relative `pack/full`
default and `strata-stall-<pid>.dmp` there (research §4).

`PR_SET_NO_NEW_PRIVS` is set pre-exec (open question 5: the NVIDIA driver may
exec setuid `nvidia-modprobe` to create missing `/dev/nvidia-uvm*` nodes, which
NNP blocks). The frontend refuses to start as euid 0 or with an elevated
Windows token (fixed error; threat model "run as the invoking user").
Rejected for SP2: Landlock, restricted tokens (need final SP1 roots and a
real-engine trial).

### 3.13 PROC-03 scope on Linux

PROC-03 on Linux means "other user context". From research §6: "Same-uid
caveat for PROC-03: `open("/proc/<engine>/fd/0", O_WRONLY)` reopens the pipe →
injection possible; only other uid, non-dumpable target, or sandbox/LSM blocks
→ spec PROC-03 as 'other user context', record same-user accepted risk (engine
protocol can't carry a nonce without engine changes)." The upstream engine
cannot be made non-dumpable after exec without an engine change, so SP2
records the accepted risk in the threat model and release record.

## 4. Scope — requirements, fixes, and regression tests

Platform: **L** Linux-only, **B** both (Linux CTest + MSYS2 CTest), **W**
MSYS2-only (Windows runtime). "Fake" names the fake-engine mode. Test names are
CTest names.

### 4.1 Fixtures

| Item | Requirement / change | Test (platform) |
| --- | --- | --- |
| F1 | Fake engine becomes a standalone test-only executable `strata-fake-engine` (not installed) so the server binary can launch it. Modes: `--crash-after-ready=<SIG\|code>`, `--exit-before-ready=<code>`, `--hang-before-ready`, `--hang-in-gen` (ignores STOP), `--abort-in-gen`, `--fatal-line=<0..3>` (prints that prefix, then lives), `--fail-first=<n> --counter-file=<p>`, `--spawn-grandchild` (now POSIX too) with `--grandchild-setsid`, `--grandchild-ignore-term`, `--grandchild-pidfile=<p>`, `--record-file=<p>` (every stdin line, for EXT), Windows `--echo-cmdline`. Existing modes kept. | `fake_engine_modes_selftest` (B) |
| F2 | `DUMP` extended: `ENV` (sorted `NAME=value`), `SIG` (`SigBlk`/`SigIgn` from `/proc/self/status`), `CWD`, `IDS` (uid/gid/`NoNewPrivs`/`CapEff`; Windows token user SID, `TokenElevation`, integrity level), `HANDLES` (Windows: own pid only; inspection is parent-side, §3.11). Portable `ARGV`. | `fake_engine_dump_is_parseable` (B) |
| F3 | Harness library: start `strata-secure-server` as a test child with fake engine; wait for `/health`; stop it by SIGTERM/SIGINT/SIGKILL (POSIX) or `CTRL_BREAK_EVENT`/`TerminateProcess` (Windows); count descendants (`/proc/<pid>/task/*/children` recursively; Job `ActiveProcesses`); liveness by pidfile. Injectable `Clock` and `BackoffTimer` for supervisor unit tests. | used by rows below |

### 4.2 Supervisor (PROC-04)

| Item | Requirement | Fix | Regression test (platform, fake) |
| --- | --- | --- | --- |
| V1 | Exit status is captured and classified. | Keep raw status; §3.6 table; Windows kill code `0x20000001`. | `proc04_exit_status_is_classified` (B; `--exit-before-ready=1`, `--crash-after-ready=ABRT`, `=KILL`, `=SEGV`) |
| V2 | READY consumed eagerly; `/health` `503 starting` → `200 ready` with no request. | Supervisor thread; §3.4, §3.8. Supersedes SP0 `starting → 200`; the SP0 health tests are updated, not deleted. | `proc04_ready_is_eager_and_health_starting_is_503` (B; `--split-ready`) |
| V3 | Bounded restart with exponential backoff, then permanent circuit. | §3.3 policy with injected clock. | `proc04_restart_backoff_then_circuit_open` (unit, B: delays exactly 1 s, 2 s, then `circuit_open` on the 3rd failure); `proc04_healthy_window_resets_failure_count` (unit, B); `proc04_recovers_after_transient_crash` (B; `--fail-first=1`) |
| V4 | Death between requests is noticed without a request. | pidfd poll (fallback: 100 ms `waitpid(WNOHANG)` on kernels without `pidfd_open`); Windows wait on process handle. | `proc04_monitor_detects_idle_crash` (B; `--crash-after-ready=KILL`): `/health` leaves `ready` within 1 s with no chat |
| V5 | Fatal prefixes kill and restart. | Prefix match in the protocol client → `report_fatal`. | `proc04_fatal_prefix_kills_engine` (B; `--fatal-line=0..3`, each prefix) |
| V6 | READY timeout kills and counts. | §3.5(2). | `proc04_ready_timeout_kills_and_counts` (B; `--hang-before-ready`, `ready_timeout` 200 ms) |
| V7 | Drain failure kills and counts. | §3.5(3). | `proc04_drain_failure_kills_and_restarts` (B; `--hang-in-gen`) |
| V8 | Abort in generation is a generic `503`, then recovery. | — | `proc04_abort_in_generation_is_generic_503` (B; `--abort-in-gen`); body is the fixed literal, no exit code |
| V9 | No leaks across generations. | Fresh `EngineGeneration` (§3.2). | `proc04_no_leak_across_restarts` (B; 10 forced restarts: exactly one live engine, no zombie (`/proc`), frontend fd/handle count stable ±0, new generation's `pending_stdout_` empty) |
| V10 | A SIGKILLed child that cannot be reaped blocks respawn. | No spawn until reaped; past `reap_timeout` → `circuit_open`. | `proc04_unreaped_child_opens_circuit` (unit, B; fake process that never reaps) |
| V11 | Chat in `starting`/`backoff`/`circuit_open` is an immediate generic `503`. | `acquire()` returns empty outside `ready`. | `proc04_chat_during_restart_is_generic_503` (B; `--hang-before-ready`) |

### 4.3 Descendant cleanup (PROC-05)

| Item | Requirement | Fix | Regression test (platform, fake) |
| --- | --- | --- | --- |
| C1 | Group members die even after the leader is reaped. | Always `kill(-pgid, SIGKILL)` (§3.7a). | `proc05_group_killed_after_leader_reaped` (L; `--spawn-grandchild --grandchild-ignore-term`) |
| C2 | `setsid`/double-fork escapes die. | Subreaper sweep (§3.7b). | `proc05_setsid_grandchild_is_reaped` (L; `--spawn-grandchild --grandchild-setsid`) |
| C3 | Engine leader dies if the frontend is SIGKILLed. | `PR_SET_PDEATHSIG` + `getppid()` recheck, forked from supervisor thread. | `proc05_frontend_sigkill_kills_engine` (L; harness SIGKILL; asserts leader gone; grandchild asserted only when C5 active, else logged as the §3.7 residual risk) |
| C4 | SIGTERM/SIGINT stop the frontend in order. | Self-pipe → §3.7 shutdown. | `proc05_frontend_sigterm_stops_all_descendants`, `proc05_frontend_sigint_stops_all_descendants` (L; `--spawn-grandchild`): all gone within 5 s, port refuses connections, exit 0 |
| C5 | cgroup v2 when delegated. | Detect writable delegated cgroup; `cgroup.kill`. | `proc05_cgroup_kill_when_delegated` (L; skipped with reason when not delegated) |
| C6 | Windows: console events reach only the frontend; Job kills all; no WER stall. | `CREATE_NEW_PROCESS_GROUP`, `DIE_ON_UNHANDLED_EXCEPTION`, `SEM_NOGPFAULTERRORBOX`, console handler. | `proc05_windows_ctrl_break_stops_all_descendants`, `proc05_windows_terminate_frontend_kills_job` (W; `--spawn-grandchild`; `ActiveProcesses` 0 / pids gone); `proc04_windows_abort_does_not_stall` (W; `--abort-in-gen`, exit observed < 5 s) |
| C7 | Production binary has no non-engine children. | — | `proc05_frontend_has_only_engine_children` (L) |

### 4.4 Environment, signals, cwd, handles (PROC-02)

| Item | Requirement | Fix | Regression test (platform, fake) |
| --- | --- | --- | --- |
| N1 | Child environment equals the allowlist exactly. | §3.9 `EngineEnvironment`. | `proc02_child_env_is_exact_allowlist` (B; `DUMP ENV`; parent sets token var, `STRATA_API_KEY`, `HOME`, `LD_PRELOAD`) |
| N2 | `child_receives_exact_argv_and_env` asserts env. | Compare ENV dump to the expected block. | existing test fixed (B) |
| N3 | `engine_process` env test checks Windows-relevant names. | `run_fake_engine` checks `PATH`, `SystemRoot`, `USERPROFILE` instead of only `HOME`. | `proc02_engine_process_env_checks_windows_names` (B) |
| N4 | Signal mask empty, no ignored signals in child. | §3.12 pre-exec reset. | `proc02_child_signal_state_is_default` (L; `DUMP SIG`: `SigBlk`/`SigIgn` zero) |
| N5 | Only stdio descriptors/handles inherited. | Existing sweep; canaries. | `proc02_canary_descriptors_absent` (L; non-CLOEXEC fd, listening socket, open token file) ; `proc02_canary_handles_absent` (W; inheritable file, event, socket; §3.11 parent-side) |
| N6 | Windows pipe owner-only, random name, verified client pid. | §3.11. | `proc02_stdin_pipe_dacl_is_owner_only` (W; read back DACL: one ACE, current user) |

### 4.5 Argv, IPC reachability, privilege (PROC-01, PROC-03, PROC-06)

| Item | Requirement | Fix | Regression test (platform, fake) |
| --- | --- | --- | --- |
| A1 | Windows argv round-trips literally. | Make ARGV dump portable. | `proc01_windows_argv_round_trip` (W; `--echo-cmdline` with `"`, `\`, trailing `\`, `%PATH%`, `^&\|`, newline, UTF-8) |
| A2 | End-to-end argv through `build_engine_arguments` and the server, no shell. | — | `proc01_server_passes_literal_argv` (B; model id/config values with metacharacters; ARGV dump equal; only one child process) |
| R1 | Second open of the Windows stdin pipe fails. | §3.11. | `proc03_windows_pipe_second_open_fails` (W; `CreateFileW` on the name after start → `ERROR_PIPE_BUSY` or access denied) |
| R2 | Other user cannot reach engine stdio. | — | `proc03_other_uid_cannot_open_engine_fd` (L; needs a second uid, CI-only; skipped with reason elsewhere) |
| R3 | Same-uid reopen recorded as accepted risk. | §3.13; threat-model and release-record entry. | `proc03_same_uid_reopen_is_accepted_risk` (L; asserts current behavior so a future mitigation flips it visibly) |
| G1 | Child identity and cwd. | §3.12 `chdir`/`lpCurrentDirectory`. | `proc06_child_ids_and_cwd` (B; `DUMP IDS/CWD`: uid/gid equal parent, cwd = `<state_dir>/engine`; Windows: not elevated, same user SID) |
| G2 | `NoNewPrivs` set. | `PR_SET_NO_NEW_PRIVS` pre-exec (if approved). | `proc06_child_has_no_new_privs` (L) |
| G3 | Elevation never requested. | 740 → `spawn_failed`. | `proc06_elevation_required_is_spawn_failed` (W; fake copy with `requireAdministrator` manifest) |
| G4 | Frontend refuses root/elevated start. | Startup check, fixed error. | `proc06_refuses_privileged_start` (unit seam for euid/token check, B) |

### 4.6 Extension exclusion (EXT-01, EXT-02)

| Item | Requirement | Regression test (platform, fake) |
| --- | --- | --- |
| X1 | Tool/function/MCP/stream fields reject with the fixed response and no process or engine action. | `ext01_extension_fields_spawn_nothing` (B; `--record-file`: no `GEN`; descendant count stays 1) |
| X2 | Extension attempts via env, `--engine-arg`, unknown option, manifest field fail before spawn or never reach the child. | `ext02_extension_env_never_reaches_child` (B; parent `STRATA_FOO`, `LD_PRELOAD`, `CUDA_LAUNCH_BLOCKING` absent from ENV dump); `ext02_rejected_before_spawn` (B; zero children) |

### 4.7 SP2-tagged known gaps and docs

| Item | Change | Regression test / check |
| --- | --- | --- |
| K1 | One `remaining_until(deadline, clock)` helper used by `await_ready`, `read_generation`, and `read_line`; durations must be > 0 (SP1 rejects 0); a 0 that slips through means "already expired" on every path. | `deadline_zero_timeout_is_immediate_everywhere` (B) |
| K2 | Overlapped stdin writes (§3.10). | `proc04_windows_write_timeout_kills_engine` (W; `--hang-in-gen` with stdin reader paused) |
| K3 | `terminate` contract in the header: returns `false` only if the child is still unreaped after the bound; the object is then "abandoned" and the supervisor applies V10. | review + V10 |
| K4 | Comment on `kSweepCap` 65536: why bounded, when reached (no `close_range`, kernel < 5.9, and `RLIMIT_NOFILE` > cap), and that all frontend fds are `O_CLOEXEC`, so the sweep is defence in depth. | review |
| K5 | `current_executable()` checks `readlink` (`-1`, truncation) and `GetModuleFileNameW` length; fails the test loudly. | review |
| K6 | `docs/security/engine-protocol-limits.md`: replace "blocking `getline`" with the reader thread on `read(0)`/`_read(0)` (`generate.cpp:9433-9469` at v0.1.44.1); state overlapped writes done. Threat model: env allowlist (no longer "empty"), restart policy, PROC-03 and PROC-05 residual risks. Handoff: Phase 2 item 3, Known gaps, Verification. | review |

## 5. Out of scope

Engine changes (`upstream/src/`: protocol nonce, `PR_SET_DUMPABLE`); GPU verification
of the allowlist (offline box, the owner); Landlock, seccomp, restricted tokens,
write confinement beyond cwd/temp; a circuit-reset route; `run`/`ps`/`stop`
and service units (product-experience spec); config parsing (SP1);
concurrency, streaming, replicas (SP3); vision/batch; upstream's psutil stall
and silence checks beyond SP0's generation deadline; overlapped stdout.

## 6. Execution model

- As SP0 §6: one task per subagent, spec-compliance and code-quality reviews
  by separate agents, test first, full `ctest` green, one local commit per
  task; the orchestrator reruns `ctest` itself.
- Order (37 tasks; `+` joins rows into one task): F1, F2, F3 → K1, K3, K5 →
  V1 → V2 → V3 → V4 → V5, V6, V7, V8, V11 → V9, V10 → C1, C2, C3, C4, C7 →
  C5 → C6 → N1+N2+N3 → N4 → N5 → N6+R1+K2 → A1, A2 → R2+R3 → G1, G2, G3, G4
  → X1+X2 → K4+K6. W tasks are cross-built and wine-smoked locally and count
  as done only when the MSYS2 job is green.
- Stop for the owner before any `upstream/src/` change, any second-user test outside CI,
  any GPU run, or any `/health` change beyond adding `recovery`.

## 7. Review Focus

1. §3.1 thread model: one supervisor thread is the only reaper and forker;
   check every path that used to call `running()` from the request side.
2. §3.8 state machine and `/health` mapping, especially the SP0 reversal
   (`starting` → `503`) and the new `recovery` literal.
3. §3.9 allowlist: names, generated values, the never-list, and `--gpu` as
   argv; whether a real CUDA/ROCm engine needs anything else.
4. §3.7 residual risk on Linux frontend SIGKILL without cgroup, and §3.13
   same-uid PROC-03 accepted risk: are both acceptable for release?
5. §3.12 `PR_SET_NO_NEW_PRIVS` and refusing root/elevated start: do they break
   real installs (NVIDIA `nvidia-modprobe`, containers running as root)?

## 8. Acceptance for sub-project 2

1. Dev host: `ctest --preset linux-gcc` green with every L and B test by ID
   (`proc01_*`..`proc06_*`, `ext01_*`, `ext02_*`); skips only with a stated
   reason (second uid, delegated cgroup).
2. Cross build produces all binaries incl. `strata-fake-engine.exe`; wine
   smoke passes and is not cited for W rows.
3. CI after the owner pushes: MSYS2 `CLANG64` green for every W row (Job and
   grandchild, Ctrl-Break, pipe busy, pipe DACL, handle canaries, argv twin,
   overlapped write timeout, abort without WER stall); `linux-gcc`,
   `linux-clang`, cross, `top-level-integration` green; MSVC informational.
4. Every §4 row has its named test in CTest and in its commit message.
5. Limits doc, threat model and handoff are accurate, including both residual
   risks and the not-GPU-verified allowlist; nothing claimed beyond CI logs.

## 9. Open questions for the owner

1. **Restart numbers.** Recommend 3 consecutive failures, 1 s → 2 s (… ≤ 60 s),
   300 s healthy window, 900 s READY timeout, circuit reset only by restarting
   the frontend. Alternative: upstream's fixed 15 s × 3.
2. **GPU selection.** Recommend typed `--gpu` argv and no GPU variables in the
   environment. Alternative: frontend-rendered `CUDA_VISIBLE_DEVICES` etc.
3. **cgroup v2.** Recommend subreaper + reap by default, `cgroup.kill` only
   when delegation is detected, accepting the frontend-SIGKILL residual risk.
   Alternative: require a delegated cgroup for Linux release.
4. **Overlapped stdin writes.** Recommend now. Alternative: defer and bound
   writes with `CancelSynchronousIo` from the supervisor thread.
5. **`PR_SET_NO_NEW_PRIVS` and refusing root/elevated start.** Recommend both
   on, with a startup error naming the `/dev/nvidia-uvm` fix if CUDA init
   fails. Alternative: NNP behind an SP1 switch, default off, until the GPU
   box confirms it is harmless.
