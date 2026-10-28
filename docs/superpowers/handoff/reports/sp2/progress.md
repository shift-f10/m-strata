# SDD ledger — plan: docs/superpowers/plans/2026-10-13-sp2-portable-supervision.md

Spec: docs/superpowers/specs/2026-10-13-sp2-portable-supervision-design.md (APPROVED; NNP default ON)
Worktree: /opt/projects/ai/.worktrees/sp2, branch sp2-portable-supervision, launch HEAD 17d2452d (verified; submodule clean at 7695859a; .venv-dev/.toolchains symlinks present)
Scope this run: Tasks 1,2,3,4,5,6a,7,8,9,10,11,12,13,14,17,20,21,22,23,24 then final whole-branch review; STOP. 6b,15,16,18,19,25-29 wait for SP1 merge.

## Rulings
Ruling: Skip the plan's "rebase onto main before Task 1" — HEAD 17d2452d already IS main with the shared pre-task; rebasing is forbidden by the owner's rules — cost if wrong: none (Task 1 Step 0 verifies pre-task presence).
Ruling: Commit trailers use Claude Fable 5.1 + session URL from the common brief, not the plan's Opus trailer — owner/controller instruction overrides plan text — cost if wrong: trailer amend by integrator.
Ruling: Pre-flight conflict scan delegated to one read-only opus agent (plan is 518 KB; reading it whole would crowd the controller's context for 20 tasks) — table written to preflight-scan.md and summarized here — cost if wrong: a conflict the scan misses surfaces in task review instead.
Ruling: The final report lives inside this workspace (.superpowers/sdd/.../FINAL-REPORT.md) per the SP2 brief, so the workspace is NOT deleted at the end (also rm -rf is forbidden) — cost if wrong: one git-ignored directory left behind.

## Pre-flight scan (full table: preflight-scan.md, sections A-F; 2 CONFLICT rows, both Task 22 K2 size; section C: no hard dependency on out-of-scope tasks)
Ruling: PF1 Task 22 K2 prompt is std::string(60000U,'0') (~240 KB GEN line, >3x 64 KiB) instead of 70000 — plan's 70,000 exceeds the 65,536-byte per-message cap in qwen_prompt_renderer.cpp so write_timeout would never be reached; production cap NOT raised — cost if wrong: one MSYS2 CI round.
Ruling: PF2 Task 17 proc04_windows_abort_does_not_stall gets a fake flag `--default-error-mode` (fake skips its own SetErrorMode) so the test exercises the inherited frontend error mode and Job DIE_ON_UNHANDLED_EXCEPTION; the test is a supervisor-level smoke, not C6 proof (spec C6 server cases are Task 15) — cost if wrong: one extra fake flag.
Ruling: PF3 Task 17 handoff C6 row is written partial ("launch flags only; console handler + server cases pending Task 15"); code comment reworded to future tense — handoff must not overstate — cost if wrong: none.
Ruling: PF4 Task 24 anchors: insert the workflow comment after "# STRATA_NATIVE_EXPERTS=OFF, so no llama.cpp clone (FetchContent) occurs." and add env to the ctest step now at lines 58-59 (both matrix presets) — quoted text drifted — cost if wrong: workflow edit misplaced (no CI runs from here).
Ruling: PF5 Keep fake name strata-supervision-fake-engine (spec says strata-fake-engine); record naming deviation in F1 handoff row — SP1 owns the other fake — cost if wrong: later rename.
Ruling: PF6 Task 3 byte_symbol fixed to real GPT-2 printable ranges (0x21-0x7E, 0xA1-0xAC, 0xAE-0xFF) — latent fixture bug, 3 lines — cost if wrong: none.
Ruling: PF7 Dead surface (SupervisorConfig::shutdown_stdin_wait/no_new_privileges_hint, SupervisedProcess::close_stdin/signal_group_terminate, kShutdownStdinWait, kNoNewPrivilegesDefault) is kept — plan Interfaces bind later tasks 15/26 — cost if wrong: review churn.
Ruling: PF8 Task 4 RED may show more FAILED lines than Step 2 lists, valid if the named ones are among them — cost if wrong: none.
Ruling: PF9 Task 6a: proc04_missing_engine_opens_circuit_without_spinning accepted as UNIT; CMake comment "no surviving process" — cost if wrong: none.
Ruling: PF10 EngineSupervisor::shutdown std::_Exit(1) past the bound is kept (spec §3.7 mandates exit 1 past 5 s) — cost if wrong: reviewer churn.
Ruling: PF11 Spec V4 "/health leaves ready within 1 s" is proven at supervisor level (wait_for_state) in this run; the /health form waits for Task 6b — cost if wrong: V4 row marked partial until 6b.

## Progress
Task 1: dispatched (BASE 17d2452d, implementer a13f6fab092fd8fed, opus)
Task 1: minor (deferred): stale SP0 "Windows launch (P4,P6,P7)" handoff row still says 30 s sleeper from in-binary fake
Task 1: minor (deferred): comments name SP1's tests/fake_engine_main.cpp, not on branch until SP1 merges (brief-mandated)
Task 1: minor (deferred): selftest does not exercise --grandchild-setsid / --grandchild-ignore-term
Task 1: minor (deferred): POSIX grandchild inherits engine stderr (fd 2) — may matter to tests that wait for stderr EOF
Task 1: minor (deferred): test_paths.hpp unused <atomic>; Windows make_temp_dir std::exit skips scratch cleanup (brief-verbatim)
Task 1: minor (deferred): fake's stoi/stoul on malformed flag values abort instead of a clear exit(2)
Task 1: minor (deferred): stdio test_round_trip rewrite drops start/ready error message detail (brief-verbatim)
Task 1: complete (commits 17d2452d..0d7810fb, review clean)
Task 2: dispatched (BASE 0d7810fb)
Ruling: Owner's Fable 5.1 trailer overrides harness attribution reminders seen by implementers; Task 2 commit eeebbb94 amended (message only) — owner rule is binding — cost if wrong: none
Task 2: minor (deferred): --dump-file subtest does not pin FDS {0,1,2} (the deviation it motivated)
Task 2: minor (deferred): write_dump_file swallows stream/rename errors (plan-mandated)
Task 2: minor (deferred): parse_dump throws via stoi/stoul on bad FDS/HANDLES instead of nullopt (plan-mandated)
Task 2: minor (deferred): launcher-test failure messages lost observed values (plan-mandated)
Task 2: minor (deferred): DUMP line order (SIG before ARGV) differs from plan Interfaces list; parser order-independent
Task 2: concern: Windows ENV check fails under wine (wine injects vars); authoritative in MSYS2
Task 2: complete (commits 0d7810fb..a40bd520, review clean)
Task 3: dispatched (BASE a40bd520)
Task 3: minor (deferred): harness never sees descendants if server exits on its own before stop()/descendants() (remember_descendants in wait_exit loop) (plan-mandated scope)
Task 3: minor (deferred): stop() after wait_exit and destructor kills may hit recycled PIDs
Task 3: minor (deferred): descendant kill loop in pid order can leave a re-parented zombie unreaped (kill all, then reap pass)
Task 3: minor (deferred): self-test does not assert descendants() non-empty / gone after destruction
Task 3: minor (deferred): PF6 tokenizer round trip not pinned by a committed case
Task 3: minor (deferred): http_request std::stoi can throw; partial body on timeout (brief code)
Task 3: minor (deferred): Windows harness CreateProcess bInheritHandles=TRUE without handle list
Task 3: concern: wine has no UAC -> restricted-token check fails under wine (smoke); GenerateConsoleCtrlEvent return ignored; MSYS2 authoritative
Task 3: complete (commits a40bd520..0b78daa1, review clean)
Task 4: dispatched (BASE 0b78daa1)
Task 4: minor (deferred): K3 terminate comment describes POSIX timing only (plan-mandated text)
Task 4: minor (deferred): handoff K1 row uses bare case name, not <binary>.<case> (plan-mandated)
Task 4: minor (deferred): read_line zero guard spelled timeout.count()<=0 rather than via remaining_until (brief form)
Task 4: minor (deferred): deadline test's buffered-line precondition (fake single flush) not asserted
Task 4: complete (commits 0b78daa1..8b2c6505, review clean)
Task 5: dispatched (BASE 8b2c6505)
Task 5: minor (deferred): sentinel statuses (GetExitCodeProcess failure 0xFFFFFFFF -> crashed; POSIX ECHILD {false,255} -> error_exit) not marked as sentinels
Task 5: minor (deferred): warning-severity NTSTATUS (0x80000003) classifies error_exit, arguably crashed
Task 5: minor (deferred): K3 comment leaves Windows terminate(0ms)->false contract difference implicit; uneven reflow
Task 5: minor (deferred): classify table misses cheap rows (TERM unsent -> crashed; Windows sent!=none with 1/3; 0x20000001 unsent; exit 2)
Task 5: minor (deferred): note "owning thread only" on running() too (calls try_reap)
Task 5: ⚠️ resolved: spawn_failed has no classify_exit producer — the supervisor (6a) must log SpawnFailure as spawn_failed; carried into 6a dispatch. Requested exit-0 during shutdown is Task 15's (out of scope).
Task 5: complete (commits 8b2c6505..3f780e79, review clean)
Task 6a: dispatched (BASE 3f780e79)
Task 6a: minor (deferred): Lease holds raw EngineSupervisor* — Leases must not outlive the supervisor (6b wiring must honour)
Task 6a: minor (deferred): SupervisorWake construction failure swallowed (pipe2/CreateEvent) — start() should refuse (plan-mandated)
Task 6a: minor (deferred): Lease::process() exposes lifecycle interface to the serving thread (plan-mandated)
Task 6a: minor (deferred): committed supervisor test never starts the thread; acquire/stale-lease/shutdown-with-lease uncovered (plan scope)
Task 6a: minor (deferred): start() assigns thread_ outside the lock (two concurrent start() calls)
Task 6a: minor (deferred): factory/log_ exceptions escape run() -> std::terminate
Task 6a: minor (deferred): notify() from signal handler clobbers errno — Task 15 must save/restore
Task 6a: Ruling: EngineInstance may be destroyed on the serving thread when a Lease holds the last ref (Windows TerminateJob+wait, POSIX waitpid of an unreaped shutdown instance) — violates spec §3.1 letter; becomes routine once Task 7 loops. Task 7 implementer is told to retire ended instances on the supervisor thread (keep a retired ref until the Lease releases; push unreaped shutdown_generation instances to abandoned_) — spec §3.1 is binding — cost if wrong: small extra code in Task 7.
Task 6a: complete (commits 3f780e79..63918234, review clean)
Task 7: dispatched (BASE 63918234)
Task 7: minor (deferred): unreaped generation restarts instead of circuit_open ((void)reaped) — plan assigns to Task 11 (V10)
Task 7: minor (deferred): retirement tests cover only reaped+lease path; Lease outliving shutdown() -> dangling owner_ (6b/15 lifetime caveat)
Task 7: Ruling: stale SupervisorWake signal can make SteadySupervisorClock::sleep_backoff return early and skip the 1 s/2 s delay (spec §3.3); widens once Task 8 polls. Task 8 implementer fixes it: schedule_restart loops sleep_backoff until the deadline unless stop_requested(); release() may then notify wake_; wait_in_circuit_open calls sweep_retired() — spec §3.3 binding — cost if wrong: small extra change in Task 8.
Task 7: complete (commits 63918234..0e6ea993, review clean)
Task 8: dispatched (BASE 0e6ea993)
Task 8: minor (deferred): sweep in wait_in_circuit_open untested
Task 8: minor (deferred): SupervisorWake maps poll error / WAIT_FAILED to timeout — ends backoff early or spins on invalid handle (pre-existing)
Task 8: minor (deferred): idle-crash test infers supervisor pidfd via a separate probe process
Task 8: complete (commits 0e6ea993..417c518b, review clean)
Task 9: dispatched (BASE 417c518b)
Task 9: minor (deferred): no negative coverage for is_engine_fatal_line (substring-not-prefix, truncated prefix, plain ERR)
Task 9: minor (deferred): failure count not asserted directly in V5 supervisor case
Task 9: minor (deferred): fake duplicates kFatalPrefixes (independent oracle; defensible)
Task 9: Ruling: pre-READY fatal line is logged protocol_failed (engine_supervisor.cpp await_ready) and the V5 handoff row overstates it; spec §3.8 starting row lists fatal line as its own cause. Task 10 (which edits await_ready for the READY timeout) maps non-timeout READY failures via kill_reason_for(*instance.protocol), adds one unit case, and corrects the V5 row — spec §3.8 binding — cost if wrong: one line + one case.
Task 9: complete (commits 417c518b..5cea53b5, review clean)
Task 10: dispatched (BASE 5cea53b5)
Task 10: minor (deferred): no test pins shutdown responsiveness during a long await_ready
Task 10: minor (deferred): no test pins abandon() keeping an earlier failure code (write_timeout)
Task 10: minor (deferred): V6 asserts kill reason only for generation 1
Task 10: complete (commits 5cea53b5..60998bf7, review clean)
Task 11: dispatched (BASE 60998bf7)
Task 11: minor (deferred): V9 pending-stdout check cannot fail with fake_plan({}) (plan-mandated)
Task 11: minor (deferred): V9 has no fd/children check after shutdown
Task 11: minor (deferred): Windows GetProcessHandleCount failure ignored -> vacuous pass; strict ±0 may flake
Task 11: note: "unreaped" log literal is in the plan's state-machine log kinds (plan line 64), not the spec §3.6 table — documented, not a defect
Task 11: complete (commits 60998bf7..05516535, review clean)
Task 12: dispatched (BASE 05516535)
Task 12: minor (deferred): terminate guard `process_group_ < 0` vs siblings' `> 0`; use `<= 0`
Task 12: minor (deferred): engine_stdio_process.hpp terminate header comment stale (skips TERM once reaped; pid()==-1 after; idempotent)
Task 12: minor (deferred): standalone (non-supervisor) users may kill a reused pgid from the destructor long after reap (spec §3.7(a) accepted risk)
Task 12: minor (deferred): C1 test doesn't assert pid()==-1 / idempotent second terminate; unconditional kill_process of possibly-recycled grandchild pid
Task 12: complete (commits 05516535..aafde3e6, review clean)
Task 13: dispatched (BASE aafde3e6)
Ruling: PF12 Task 13 production list_own_children stays separate from the harness children_of loop (production code must not depend on tests/support; the loop is ~15 lines) — cost if wrong: a small duplication the final review may flag
Task 13: Ruling: review Important (plan-mandated) — shutdown_generation does not sweep though spec §3.7 says the shutdown sequence sweeps and §3.7(b) after every generation; fix (a): sweep_orphans({}, 250 ms) in shutdown_generation when reaped, plus handoff row wording; also take reviewer Minor 1 (ECHILD reap assertion) in the same round since it proves the 'reaps' claim — spec binding — cost if wrong: Task 15 may restructure the shutdown sweep
Task 13: minor (deferred): become_child_subreaper result discarded; empty /proc list and sweep timeout are silent (no log)
Task 13: minor (deferred): directory_iterator increment can throw filesystem_error on the supervisor thread
Task 13: minor (deferred): unreaped path skips the sweep entirely (could sweep with keep={leader})
Task 13: minor (deferred): descendant_sweep.cpp unused <string>; fake-based unit cases run the real /proc sweep
Task 13: fix round 1/5 (2 addressed, 0 open — shutdown sweep; ECHILD reap proof; commits 443957a0..38a61cda)
Task 13: complete (commits aafde3e6..38a61cda, review clean)
Task 14: dispatched (BASE 38a61cda)
Task 14: minor (deferred): getppid mismatch branch untested; prepare must run pre-fork (comment/assert)
Task 14: minor (deferred): prctl failure reported as "execve failed" (same as existing setpgid)
Task 14: minor (deferred): exec_child comment says only the supervisor thread forks; SP0 main thread does today
Task 14: complete (commits 38a61cda..593fc39a, review clean)
Task 17: dispatched (BASE 593fc39a)
Task 17: Ruling: review Important — comments + C6 row overstate: CREATE_NEW_PROCESS_GROUP disables Ctrl-C in the engine group and keeps group-targeted Ctrl-Break off it, but keyboard Ctrl-Break still reaches every process on the console. Fix round 1 rewords both twin comments and the C6 row accordingly; adding CREATE_NO_WINDOW (engine off the frontend's console) is NOT done here — it changes stdio/console-handle semantics that only MSYS2 can verify and belongs with Task 15's console handler; carried as an open item for Task 15 / the owner — spec C6 binding — cost if wrong: keyboard Ctrl-Break race until Task 15.
Task 17: fix round 1 also takes Minor: sibling case with --crash-after-ready=SEGV --default-error-mode to exercise DIE_ON_UNHANDLED_EXCEPTION; abort-test comment reworded "abort/fastfail path; error mode only"; copy the WER comment to the engine_process.cpp twin.
Task 17: minor (deferred): SetErrorMode set as a side effect of first start() in library code, duplicated in twins (plan-mandated)
Task 17: minor (deferred): plan front matter fake-flag list lacks --default-error-mode (plan doc, not code)
Task 17: concern: UCRT abort may __fastfail (0xC0000409) to WER regardless of error mode; a real stall fails at the 120 s CTest timeout; MSYS2 authoritative; fallback _set_abort_behavior(0,_CALL_REPORTFAULT) in the fake
Task 17: fix round 1/5 (2 addressed, 0 open — console-event wording; SEGV/DIE smoke + comments; commits 3759dee3..dc747455)
Task 17: complete (commits 593fc39a..dc747455, review clean)
Task 20: dispatched (BASE dc747455)
Task 20: minor (deferred): SP0 twin engine_process.cpp N4 path untested
Task 20: minor (deferred): parent-side mask restore not asserted by the N4 test
Task 20: minor (deferred): child uses sigprocmask vs parent pthread_sigmask (style)
Task 20: complete (commits dc747455..c3ca8ce7, review clean)
Task 21: dispatched (BASE c3ca8ce7)
Task 21: Ruling: review Important (plan-mandated) — W case proc02_canary_handles_absent has no positive control; never ran past wine's availability guard, so a broken enumeration would pass silently. Fix: NumberOfHandles >= 3 and >=1 successful duplicate; injected-handle presence control (DuplicateHandle event into engine, re-snapshot, compare finds it) via a shared engine_holds lambda; also take Minors 1-2 (STATUS_INFO_LENGTH_MISMATCH loop; close process handle on all paths) — a vacuous test cannot be N5 proof — cost if wrong: small test code.
Task 21: minor (deferred): WSAStartup result ignored / no WSACleanup; failure message doesn't name the canary; L case non-fatal expect after failed start
Task 21: fix round 1/5 (3 addressed, 0 open — positive control + checked snapshot; length-mismatch loop; process handle close; commits 6f61fa67..94965a24)
Task 21: complete (commits c3ca8ce7..94965a24, review clean)
Task 22: dispatched (BASE 94965a24)
Ruling: PF13 Task 22 handoff row states 'limits doc update pending Task 28' (engine-protocol-limits.md still says synchronous writes) — handoff must not hide stale docs — cost if wrong: none
Task 22: Ruling: review Important (plan-mandated) — R1 test opened GENERIC_WRITE on an outbound pipe and accepted ERROR_ACCESS_DENIED, so it passes without proving single-instance. Fix: GENERIC_READ open must give exactly ERROR_PIPE_BUSY, plus a CreateNamedPipeW same-name probe that must fail. Also take Minor 2 (distinct fixed message / SetLastError on client-pid mismatch). — a vacuous R1 test is not proof — cost if wrong: tightened W test may need adjusting in MSYS2.
Task 22: Ruling: spec §3.10 "remaining deadline" is implemented as a per-chunk (64 KiB) 1 s deadline (kStdinWriteDeadline), matching the plan's Interfaces and POSIX per-poll parity; no overall write bound (a slow drainer could hold the serving thread ~256 s on a 16 MiB line). Accepted for this run; Task 28's limits-doc row must state the per-chunk semantics precisely; overall bound is a candidate follow-up — cost if wrong: one more deadline in write_line later.
Task 22: minor (deferred): no test for client-pid mismatch path (say so in handoff); N6 test doesn't check ACE mask or suffix hex/uniqueness
Task 22: fix round 1/5 (3 addressed, 0 open — R1 exact PIPE_BUSY + rival probe; mismatch error code; handoff row; commits 799ea851..4857dfa8)
Task 22: complete (commits 94965a24..4857dfa8, review clean)
Task 23: dispatched (BASE 4857dfa8)
Task 23: minor (deferred): A2 one-child check POSIX-only (plan-mandated); child identity not tied to dump
Task 23: minor (deferred): A2 has metacharacters only in config values, not model id (plan-mandated)
Task 23: minor (deferred): A1 checks only CMDLINE prefix
Task 23: complete (commits 4857dfa8..28bd962a, review clean)
Task 24: dispatched (BASE 28bd962a)
Task 24: Ruling: review Importants (plan-mandated) — (1) with STRATA_TEST_SECOND_UID_SUDO=1 a failed sudo precondition still skips → misconfigured CI goes green; fix: env set ⇒ precondition failure is a FAILURE, env unset ⇒ skip; (2) R2 denial assertion accepts any non-zero exit (vacuous possibilities: engine dead, path unreachable, sudo failure); fix: same-uid positive control on the same path first, probe script with distinct exit codes (path missing vs open denied) and expect exactly "denied", and prove engine still alive after (DUMP → FDS). Also take Minor 4 (print errno on R3 failure). Implementer must not run sudo locally; the CI branch is verified by reading + compile only — spec R2 "skipped with reason elsewhere" means not in CI — cost if wrong: R2 may need a tweak after the first CI run.
Task 24: minor (deferred): R2 probes only fd/0 write, not fd/1 read; sudo hang past 10 s could outlive the case (root-owned pgid)
Task 24: fix round 1/5 (3 addressed, 0 open — CI-demanded R2 fails not skips; distinguishing probe + positive control + liveness; R3 errno; commits 4b2171ef..f05f3e52)
Task 24: complete (commits 28bd962a..f05f3e52, review clean)
Final review: dispatched (MERGE_BASE 17d2452d)
Final review: With fixes — no code defect; Important: (1) threat-model PROC-03 bullet + handoff Known-gaps deletion overstate unproven Windows N6/R1; (2) plan Task 15 shutdown_generation code would regress the Task 6a/7 retire ruling and the Task 13 shutdown sweep. Minors 1-3 handoff wording (stale V2/V11 row; current shutdown is immediate SIGKILL with no log; slow reap + shutdown → _Exit(1)); Minor 4 Windows read_line waits on process handle from the serving thread (documented exception).
Ruling: Final fix wave (ONE dispatch): threat-model sentence qualified "implemented; authoritative in MSYS2 — pending"; Known-gaps entry restored as "fixed in code, MSYS2 proof pending"; handoff wording for Minors 1-4; plan Task 15 gets a binding pre-amble note to merge onto the current shutdown_generation (keep retire(instance, reaped) + 250 ms sweep; add EOF→TERM→KILL and killed_by_supervisor(shutdown) log) — docs-only, no code — cost if wrong: doc churn.
Ruling: Pre-Task-6b carry list (not fixed in this run, no wiring yet): Lease must not outlive supervisor / dangling owner_ after shutdown(); failed SupervisorWake construction must make start() refuse; exceptions escaping run() (directory_iterator increment(ec); catch → circuit_open). Watch in first MSYS2 run: GetProcessHandleCount ±0 flake; UCRT __fastfail/WER. — wiring task owns them — cost if wrong: they surface in 6b review.
Ruling: Task 22 per-chunk 1 s write deadline vs spec §3.10 "remaining deadline" is escalated to the owner as a deviation needing owner acknowledgement (final reviewer disagrees with a controller-only acceptance); behaviour kept — cost if wrong: add an overall write deadline later.
Final review: fix wave dispatched (FIX_BASE f05f3e52)
Final review: fix wave re-review clean (6/6 addressed; commit a1ec5bc0). Controller rerun: linux-gcc 100% of 143 (1 skipped: R2), wine 100% of 115, pytest 13 passed.
Top-level controller verification @ a1ec5bc0 (2026-10-14 04:42 UTC): linux-gcc build 0 warnings, ctest 143/143 (1 skip R2), test-tmp empty, pytest 13; windows-llvm-mingw-cross -Werror clean, wine 115/115; scope: only secure_frontend/, docs/, plan + 7 lines .github/workflows (Task 24). HOLD for SP1 merge; Task 22 deadline deviation escalated to owner (recommendation: overall bound in second run).
OWNER RULING 2026-10-14 07:12 UTC (Task 22 deviation): BOUND — SP2 second run adds an overall bound on engine-stdin writes (the request's remaining deadline) on top of the 1 s per 64 KiB chunk deadline; Task 28 documents it in the limits doc. Also carried to the second run: lease release notification + documented thread-safety of acquire/snapshot/report_fatal (SP3 D-1); drain failure after a client-requested STOP restarts but does not count toward the circuit (SP3 D-2); pre-exec hook + token parameter seam on the launch primitives (SP4 N10).

# ===== RUN 2 (sub-controller, 2026-10-14) =====
Launch HEAD 9ded0a5c (merge of main/SP1 e7f01b9e into sp2-portable-supervision; verified), submodule 296b59d2 upstream (v0.1.44.1) clean, symlinks present. Baseline (controller-verified): linux-gcc 244/247 (3 server-harness failures), wine 141/141, pytest 13.
Scope run 2: Task 29 (reconcile) first, then 6b,15,16,18,19,25,26,27,28 + controller rulings R1 (Task 22 overall stdin bound), R2 (SP3 D-1), R3 (SP3 D-2), R4 (SP4 launch seam), R5 (SP6 env family); final review; STOP (no merge).
Ruling: Run-2 rules files updated in place (implementer-rules.md, reviewer-constraints.md: secure_server_main.cpp/SP1 files now in scope; baseline 244/247; seam-contracts-run2.md binding names); SP4 plan Tasks 25-26 excerpt copied to sp4-plan-tasks-25-26.md so no agent opens the plan-sp4 worktree — cost if wrong: none.
Ruling: Common brief's submodule line (7695859a v0.1.43) is superseded by the run-2 brief (296b59d2 v0.1.44.1) — the merge brought upstream v0.1.44.1 from main — cost if wrong: none.
Ruling: Workspace is kept at the end (holds first-run + run-2 reports; recursive deletion is forbidden by the guard) — cost if wrong: one git-ignored dir.
Preflight run 2: dispatched read-only opus scan -> preflight-scan-run2.md
Controller addendum (SP4 seam, binding, received mid-setup): Windows seam lives in NEW header src/win_child_launch.hpp, namespace win_launch; extra_inherited_handles is std::vector<HANDLE> appended after the 3 std handles in PROC_THREAD_ATTRIBUTE_HANDLE_LIST; primary_token optional HANDLE; crt_fd_block bytes (count, flag bytes, HANDLE array, packed) -> lpReserved2/cbReserved2; std::variant<WinLaunched, DWORD> launch_in_job(const WinLaunchSpec&, ...), WinLaunched holds process/thread handles; engine launch passes an empty spec. POSIX ExecOptions{keep_fds, pre_exec (PreExecHook), ctx}, exec_child(..., const ExecOptions&), kHookFailureMarker 'H'. Tests: POSIX fake child proves lift + hook order + marker path; Windows windows-runtime test proves handle list + CRT block reach the child. Own ledgered task (R4).
Ruling: R4 Windows types follow the controller addendum where it differs from the SP4 plan excerpt (addendum says the SP4 plan is being amended and its names are final): extra_inherited_handles = std::vector<HANDLE>; WinLaunched = {process, thread, job} (superset of plan's {process, job} and addendum's process/thread); keep_fds = std::span<const int> (addendum allows span or vector; SP4 plan uses span) — cost if wrong: a type tweak when SP4 Task 26 compiles against it.
Preflight run 2: complete — preflight-scan-run2.md (sections A-F; 10 CONFLICT rows D-1,4,5,8,11,12,13,14,15,18). Root cause of the 3 harness failures: SP1 engine trust check rejects the group-writable (umask 0002) build-tree fake the harness passes directly (server_harness.cpp:291-294; canonical_path_posix.cpp:222-230). Supervisor not yet wired (secure_server_main.cpp:117-169 bare EngineStdioProcess).
Ruling: RF1 run order 29a -> 6b -> 15 -> 16 -> 18 -> 19 -> 25 -> 26 -> 27 -> 29b -> R2 -> R3 -> R1 -> R5 -> R4 -> 28 (R tasks after the code they touch settles; R4 refactors launch code once after 16/25/26; 28 last so it documents R1 and flips SP1 MSYS2 rows) — brief's literal order puts 28 before the rulings, but 28 must document R1 — cost if wrong: re-anchoring churn.
Ruling: RF2 29a scope = harness copies the fake into a private <root>/bin (0700) and exposes engine_path() (A2 expectation updated); Task 29 Step 4 (fold supervision_defaults into config_defaults + supervisor_config(FrontendConfig) with its test); Step 5b (pipe name from SP1 fill_random); handoff reconcile (SP2 section after SP1, post-merge baseline, "SP0 server"->"server"); EngineProcess test-only comment; reconcile items of preflight B.3. 29b = remaining Steps 1/3/5a/6-9 verification + cli_sp2 test. Later briefs spelling supervision_defaults:: read config_defaults:: — cost if wrong: rename churn.
Ruling: RF3 6b applies Task 29 Step 6 and adds f.wait_engine_ready() to SP1 auth02_valid_token_succeeds_and_is_not_echoed, init_writes_config_and_token_then_server_starts, cfg03, cfg05 and the "5" branch of ready_timeout_is_configurable (eager READY makes /health 503 while starting) — cost if wrong: flaky SP1 cases.
Ruling: RF4 pre-6b carry list: 6b owns start() refusing on failed SupervisorWake, exceptions escaping run()/factory/log -> circuit_open, directory_iterator::increment(ec); 15 owns errno save/restore in signal notify, Lease must not outlive shutdown(), CREATE_NO_WINDOW ON for the engine (run-2 brief ruling) — cost if wrong: items slip between reviews.
Ruling: RF5 Task 15 finish(): keep sweep_retired(), retire(instance, reaped) and the 250 ms sweep; add EOF->TERM->KILL 2/1/1 s; drop 15's extra 500 ms sweep (shutdown_generation already sweeps) — worst case 4.25 s < 5 s (spec §3.1/§3.7) — cost if wrong: rare shutdown flake.
Ruling: RF6 roots.state: validate() checks absolute/non-empty/no NUL only (no fs access; --check side-effect free); run() does the owner-only check (POSIX lstat: not symlink, euid owner, mode&077==0; Windows also SP1 open_root DACL) with Task 18's fixed message — cost if wrong: one test expectation.
Ruling: RF7 Task 18 updates SP1 ServerFixture (state dir, base_flags, base_literals) and server_cli_test init_flags; 18/19/26 each regenerate the README key table; 26 extends SP1 auth04's boolean allow-list with engine.allow_new_privileges and calls it out in its commit + handoff row (owner-visible SP1 security-test edit) — cost if wrong: red SP1 tests.
Ruling: RF8 Task 16 keeps posix_launch::prepare void; adds [[nodiscard]] bool open_cgroup(PreparedExec&, const EngineCommand&) at both engine call sites (SP4 seam says prepare unchanged) — cost if wrong: SP4 Task 25 compile break.
Ruling: RF9 R5 family scoping: kNeverForwardedEnvironment stays "never inherited from the parent"; family forced values are frontend-rendered; N1 assertion applies to EngineFamily::strata; docs say "strata family: no GPU variable; glm (SP6) renders CUDA_DEVICE_ORDER/CUDA_VISIBLE_DEVICES from typed config" — cost if wrong: docs/code contradiction at SP6.
Ruling: RF10 R1 shape: absolute stdin-write deadline on the transport from ChatExecutionService's generation deadline; keep 1 s/64 KiB; POSIX literal poll(...,1000) replaced by the constant; miss -> transport_write_failed -> write_timeout — cost if wrong: one interface touch over 5 test transports.
Ruling: RF11 R4 shape: exec_child(..., const ExecOptions& = {}) keeps existing callers compiling; 'H'+errno read distinctly in both POSIX launchers; win_launch::launch_in_job takes the engine's flags/job limits/handle list/cwd/740 mapping; ACTIVE_PROCESS=1/UI limits only when the spec asks; engine job unchanged — cost if wrong: SP4 adapts one call.
Ruling: RF12 SP0 EngineProcess::terminate left as is except a test-only comment (wait_for reaps; a later group SIGKILL could hit a recycled pgid; no production user) — cost if wrong: drift persists in a test-only class.
Ruling: RF13 upstream citations re-pinned to v0.1.44.1 in 19/28 text (GPU env ~generate.cpp:1844-1852, usage :707; stdin reader ~:9467-9469; re-verify FATAL_PREFIXES serve/server.py:587-588) — cost if wrong: stale citations.
Ruling: RF14 Task 28 flips only SP1 rows covered by CI run 38540719307 (MSYS2 193/193); SP2 W rows stay pending (run predates the SP2 merge) — cost if wrong: handoff overstates.
Ruling: RF15 expected skips are 2 (R2 other-uid + cgroup delegation) not 1 where briefs 18/19 say "1 skipped"; expected linux total before R tasks ~272 — reconcile per task, never force — cost if wrong: none.
Task 29a: dispatched (BASE 9ded0a5c, opus)
Task 29a: implementer DONE_WITH_CONCERNS (5ac39bb4; linux 248/248 1 skip, wine 141, pytest 13); concerns: wine +0 (new case in windows-runtime-labelled binary), Windows engine_path root-based not canonical (MSYS2), rename notes carried
Task 29a: review Approved (spec compliant). ⚠️ items resolved by controller: Windows A2 argv[0] final-path form and LUA token executing the bin/ copy are MSYS2-only — added to the MSYS2 watch list (not a gap provable here).
Task 29a: minor (deferred): Windows engine_path() is make_preferred, POSIX canonical — A2 may mismatch an 8.3/junction TEMP on MSYS2
Task 29a: minor (deferred): handoff V2/C6 rows + SP2 note still say "after the SP1 merge" (Task 28 truth pass fixes)
Task 29a: minor (deferred): SetErrorMode pair (SP1 replaces, SP2 ORs) noted only in the task report, not the handoff (Task 28)
Task 29a: minor (deferred): harness uses throwing std::filesystem::canonical overload amid error_code style
Task 29a: complete (commits 9ded0a5c..5ac39bb4, review clean)
Task 6b: dispatched (BASE 5ac39bb4, opus)
Task 6b: implementer DONE_WITH_CONCERNS (0e23d220; linux 253 1 skip, wine 142, pytest 13). start() now [[nodiscard]] bool (Task 15 must use it); SP1 CFG-01 unbindable case gained a no-"engine generation" assertion (SP1 test edit, noted in commit + handoff); wait_engine_ready(optional auth).
Ruling: V4 "/health leaves ready within 1 s" server-level case (PF11 deferred it to 6b; the 6b brief omits it) is added in 6b's fix round 1 regardless of review outcome — spec V4 binding and the wiring now exists — cost if wrong: one extra case.
Task 6b: review Needs fixes — Important: ready_timeout_is_configurable vacuous after eager READY while handoff claims it proves the READY deadline (partly plan-mandated by Task 29 Step 6).
Ruling: 6b Important 1 fixed in the TEST (preferred), not by rewording: "1" branch asserts wait_engine_ready(4s) false + stderr killed_by_supervisor(ready_timeout); "5" branch asserts no ready_timeout line — handoff must not overstate (PF3/PF13 precedent) and the plan's "503 either way" is satisfied by the stronger form — cost if wrong: timing sensitivity in one e2e case.
Ruling: fix round 1 also takes Minor 1 (fail_closed runs sweep_orphans({},250ms) + release_resources when reaped — spec §3.7(b) sweep after every generation) and Minor 2 (CFG-01 assertion names the real message) — cost if wrong: small code.
Task 6b: minor (deferred): V11 case covers only initial start, not backoff (brief-verbatim)
Task 6b: fix round 1/5 (4 addressed, 0 open — ready_timeout test real; V4 /health server case; fail_closed sweep+release; CFG-01 message; commits 0e23d220..e28f6eeb)
Task 6b: minor (deferred): fail_closed sweep/release has no dedicated assertion; V4 noticed-time includes one /health request timeout (conservative)
Task 6b: complete (commits 5ac39bb4..e28f6eeb, review clean)
Task 15: dispatched (BASE e28f6eeb, opus)
Task 15: implementer DONE_WITH_CONCERNS (b0ab61ca; linux 263 1 skip, wine 145, pytest 13); additions: release_shutdown_signals, LeaseLink+detach_leases, accept-after-shutdown_listener as listener failure (WSAEINTR under wine), run() scope guard; SP0 twin with CREATE_NO_WINDOW no longer writes to console (tests check exit codes only)
Task 15: review Approved. ⚠️ Windows console handler/CTRL_CLOSE/CREATE_NO_WINDOW stdio — MSYS2 watch list.
Task 15: minor (deferred): g_signalled is volatile sig_atomic_t read cross-thread (formal data race; plan-mandated) — std::atomic<int> + lock-free static_assert
Task 15: minor (deferred): shutdown_signal.cpp comment wrongly says the handler only runs on the calling thread after join; release vs ~SupervisorWake race in multi-threaded test binaries
Task 15: minor (deferred): leader unreaped after SIGKILL+1 s -> abandoned_, yet main returns 0 (C4 "no descendant") — consider _Exit(1) or document
Task 15: minor (deferred): no test of the slow path (EOF-ignoring engine: TERM ~2 s, KILL ~1 s, total <5 s); budget proven by arithmetic only
Task 15: minor (deferred): CTRL_CLOSE return races the exit code; Windows signal_group_terminate no-op makes the 1 s TERM wait idle — record in C6 row (Task 28)
Task 15: minor (deferred): SP0 test-only twin with CREATE_NO_WINDOW loses console output (RF12 accepted)
Task 15: complete (commits e28f6eeb..b0ab61ca, review clean)
Task 16: dispatched (BASE b0ab61ca, opus)
Task 16: implementer DONE_WITH_CONCERNS (168e06ca; linux 264 2 skip, wine 145, pytest 13). Child writes cgroup after setpgid (preflight D-9); delegated RED/GREEN not run (needs owner approval for systemd-run scope — not requested; CI/owner box only).
Ruling: Task 16 C3 delegated branch — spec C3 row says the grandchild is "asserted only when C5 active", but after a frontend SIGKILL nothing writes cgroup.kill (the frontend is dead), so asserting death would fail. The delegated branch instead asserts the grandchild is a member of the generation's strata-engine-<n> cgroup (so the scope/operator cgroup.kill reaps it) and logs that its death needs that cleanup; the spec-vs-mechanism gap goes to the owner in the final report — cost if wrong: a stronger assertion needed later (e.g. a watchdog writing cgroup.kill).
Task 16: minor (deferred): failed spawn leaves an empty strata-engine-<n> cgroup directory
Task 16: review Needs fixes — Important 1 C3 delegated assertion impossible (ruled above); Important 2 (plan-mandated naming) strata-engine-<n> not unique per frontend + EEXIST silently reused → cross-instance cgroup.kill.
Ruling: Task 16 Important 2 fixed despite plan naming: directory strata-engine-<pid>-<n>, EEXIST = failure (generation runs without cgroup, logged), test reads the generation dir via an accessor — a cross-instance kill is a correctness/security defect; the spec does not fix the name — cost if wrong: a naming tweak.
Ruling: fix round 1 also takes Minors 1-4 (bounded ~100 ms cgroup removal inside the shutdown budget / no reset on failure; fixed-literal log on cgroup downgrade; rmdir on spawn-failure paths; C5 test asserts /proc/<gc>/cgroup membership) — cost if wrong: small code.
Task 16: minor (deferred): kill_cgroup/remove_cgroup noexcept but allocate (brief-mandated); public header includes cgroup_scope.hpp; stale strata-probe-<pid> makes detect fail closed (negligible)
Task 16: fix round 1/5 (6 addressed, 0 open — C3 membership; per-frontend dirs + EEXIST fail; bounded removal 4.35 s worst; downgrade log; spawn-failure rmdir; C5 membership; commits 168e06ca..9b50d1bd)
Task 16: minor (deferred): cleanup() retries removal another 100 ms after the 4.35 s budget when removal already failing; factory default log goes to stderr, not the supervisor's SupervisorLog
Task 16: complete (commits b0ab61ca..9b50d1bd, review clean). OWNER ITEM: delegated-cgroup paths (C5, C3 delegated branch) verified by compile+reading only; live proof needs the owner approval for systemd-run --user --scope -p Delegate=yes.
Task 18: dispatched (BASE 9b50d1bd, opus)
Task 18: implementer DONE_WITH_CONCERNS (58c21385; linux 267 2 skip, wine 145, pytest 13); README 'empty environment allowlist' line left for Task 28 (D-24); roots.state description mentions roots.cache before 29b
Task 18: review Needs fixes — Important: Windows --state-dir check uses open_root write_access_is_private (accepts readable-by-Users, SYSTEM/Admin owner, follows junctions); handoff calls it owner/protected-DACL. Reviewer flags RF6's wording ("SP1 open_root DACL") as the cause.
Ruling: RF6 amended — on Windows the owner-only check is a no-reparse open (FILE_FLAG_OPEN_REPARSE_POINT, reject is_reparse_point) + ws::has_private_owner_dacl(handle, current_user) (same fixed message); Windows variant of state_dir_must_be_owner_only (extra Users read ACE) labelled windows-runtime; comment/handoff name the real check — Review Focus 1 intent binds over my wording — cost if wrong: MSYS2 round on the Windows variant.
Ruling: fix round 1 also takes Minor 1 (CLI case: serve without roots.state refused; server case: stale engine-tmp/7 cleared at start), Minor 2 (reject platform list separator in engine.lib_dirs entries), Minor 3 as an accepted-limitation note in the handoff (lstat checks only the last component), Minor 6 (N1 case restores the env vars it sets) — cost if wrong: small code.
Task 18: minor (deferred → Task 28): README:287 "empty environment allowlist" stale
Task 18: minor (deferred → Task 29b): roots.state description says CUDA cache under roots.cache when set — 29b must make code match (Step 5a)
Task 18: fix round 1/5 (5 addressed, 1 open — new Important: Windows test state roots owned by Administrators under elevated CI, has_private_owner_dacl refuses → every Windows serve case breaks; commits 58c21385..768fdea8)
Task 18: fix round 2/5 (1 addressed, 0 open — owner-private test state roots via make_owner_private_directory; commits 768fdea8..8f009ecc)
Task 18: complete (commits 9b50d1bd..8f009ecc, review clean). MSYS2 watch: Windows owner-only state check + owner-private test roots + junction case.
Task 19: dispatched (BASE 8f009ecc, opus)
Task 19: implementer DONE_WITH_CONCERNS (f6c8b3ee; linux 273 2 skip, wine 147, pytest 13); GLM+engine.gpu conflicting_values refusal hand-checked only; spec §3.9 still cites old upstream lines (Task 28)
Task 19: review Approved.
Ruling: Task 19 render case keeps LABELS windows-runtime (wine +2 not +3) — it opens a canonical root like its SP1 siblings, which carry the label; a direct wine smoke passes — cost if wrong: CI wine never runs one case (MSYS2 does).
Task 19: minor (deferred → 29b): GLM + engine.gpu conflicting_values refusal untested — add to 29b's cli_sp2 case
Task 19: minor (deferred → 28): spec §3.9 line 241 still cites generate.cpp:1765-1786; citation :1844 vs :1843 off by one
Task 19: complete (commits 8f009ecc..f6c8b3ee, review clean)
Task 25: dispatched (BASE f6c8b3ee, opus)
Task 25: implementer DONE_WITH_CONCERNS (bd3debb8; linux 274 2 skip, wine 147, pytest 13); G1 Windows MSYS2-pending; G3 validation-kit only; /MANIFEST:NO MSVC unbuilt here
Task 25: review Approved. Trailer Fable 5.1 per owner ruling (reviewer Minor 4 is answered by existing ruling).
Task 25: minor (deferred → 29b): server-level engine cwd wiring untested — extend A2 proc01_server_passes_literal_argv to assert CWD == <state>/engine
Task 25: minor (deferred): elevated-fake CMake repeats fake target setup (plan-mandated); hand-spelled "Win32 error" format; no spawn_failed case for bad working_directory; stoul on DUMP integrity
Task 25: complete (commits f6c8b3ee..bd3debb8, review clean)
Task 26: dispatched (BASE bd3debb8, opus)
Task 26: implementer NEEDS_CONTEXT (uncommitted) — G4 refuse-elevated-start fires in every Windows test launching strata-secure-server without the Task 3 LUA token: 2 unlabeled add_test checks (glm_requires_engine_arg, rejects_malformed_gpu) + ~30 SP1 ServerFixture/process_runner cases.
Ruling: Task 26 Option A — extract the Task 3 harness's non-elevated (LUA-restricted, Safer fallback) launch into one shared test-support helper; SP1's process_runner uses it on Windows for strata-secure-server; the two add_test CLI checks are launched through it and labelled windows-runtime (wine total -2); refusal placement stays as the brief says (after help, before validation); no runtime override flag — global constraint "Elevation in tests (no override flag)" binds and already prescribes the LUA token for every server launch — cost if wrong: MSYS2 round if the LUA launch fails for SP1 cases (same exposure as F3 harness).
Task 26: implementer DONE_WITH_CONCERNS (34c02b5b; linux 277 2 skip, wine 146, pytest 13); shared tests/support/non_elevated_launch.hpp used by harness, SP1 process_runner (by exe name) and new secure_frontend_non_elevated_run launcher
Task 26: review Needs fixes — Important: third verbatim copy of the Windows argv quoter in non_elevated_run_main.cpp. Fix round 1 also takes Minors 1-3 (diagnostic on CreateProcessAsUserW failure; kill-on-close Job in the launcher; comment on exe-stem selection).
Task 26: minor (deferred): NNP-off hint check is a 1500 ms negative wait, no at-most-once assertion (brief-mandated); G4 main wiring untestable without root/elevation (brief)
Task 26: fix round 1/5 (4 addressed, 0 open — one shared quote_windows_argument; CreateProcessAsUserW diagnostic; launcher Job; exe-stem comment; commits 34c02b5b..b6524ec4)
Task 26: minor (deferred): shared quote_windows_argument has no dedicated unit test (A1 uses its own copy)
Task 26: complete (commits bd3debb8..b6524ec4, review clean). MSYS2 watch: SP1 server cases + 2 CLI checks under LUA token via shared non_elevated_launch.
Task 27: dispatched (BASE b6524ec4, sonnet — test-only, brief carries the code)
Task 27: implementer DONE (1210d89c; linux 280 2 skip, wine 146, pytest 13)
Task 27: review Approved.
Ruling: Task 27 minors (namespace placement; record-file opened + GEN positive control; non-empty env assertion; rejection-reason assertion) are batched into Task 29b, which edits server_test.cpp anyway — cost if wrong: none.
Task 27: minor (deferred): --gpu attempt covers only the --gpu=0 form
Task 27: complete (commits b6524ec4..1210d89c, review clean)
Task 29b: dispatched (BASE 1210d89c, opus)
Task 29b: implementer DONE_WITH_CONCERNS (28d40956; linux 283 2 skip, wine 148, pytest 13); added engine_cuda_cache_follows_roots_cache server case; SP1 roots.cache description updated (was 'unused in SP1')
Task 29b: review Needs fixes — Important: engine_cuda_cache_follows_roots_cache string-compares canonical server path vs raw temp path (8.3 TEMP on MSYS2). Fix round 1 also takes Minors 1 (derive cache_below_state from environment), 3 (owner-only assertion), 4 (stale N1 row).
Task 29b: minor (deferred): "Unable to prepare --state-dir." also covers roots.cache/cuda failure; hand-rolled JSON escaping in test config
Task 29b: fix round 1/5 (4 addressed, 0 open — CUDA path identity compare; cache parent from environment; owner-only assertion; N1 row; commits 28d40956..9541f36d)
Task 29b: complete (commits 1210d89c..9541f36d, review clean). Task 29 (29a+29b) complete.
Task R2: dispatched (BASE 9541f36d, opus)
Task R2: implementer DONE_WITH_CONCERNS (26ff9739; linux 286 2 skip, wine 150, pytest 13); wait_for_change(from, timeout) on existing changed_ cv.
Ruling: R2 thread-safety doc states the Task 15 guarantee (a Lease MAY outlive shutdown() safely via LeaseLink/detach_leases), not my brief's example "must not outlive" — the shipped, tested behaviour is stronger and SP3 relies on documented truth — cost if wrong: doc wording.
Task R2: review Approved.
Task R2: minor (deferred, SP3-facing doc): wait_for_change observes states not transitions (release+immediate acquire unseen) — add caveat
Task R2: minor (deferred, SP3-facing doc): ~Lease outliving supervisor with an abandoned instance runs terminate(250ms) on the serving thread — qualify "none forks/reaps/kills"
Task R2: minor (deferred, SP3-facing doc): destructor contract should say every serving-API call must have returned
Task R2: minor (deferred): stress case lacks concurrent shutdown(); not a race detector (no TSAN)
Task R2: complete (commits 9541f36d..26ff9739, review clean)
Task R3: dispatched (BASE 26ff9739, opus)
Task R3: implementer DONE_WITH_CONCERNS (f569a524; linux 290 2 skip, wine 154, pytest 13); new literal killed_by_supervisor(client_stop_drain_failed); OWNER/SP3 item: uncounted client-STOP restarts have zero backoff → repeated client cancels = repeated full reloads (SP3 risk R4); SP3 spec §2.6 'a counted failure' predates D-2; SP3 P6 must call stop(EngineStopCause::client_cancelled)
Task R3: review Needs fixes — Important 1: uncounted branch skips the §3.8 healthy-window reset (can open the permanent circuit sooner than before); Important 2 (ruling-level): a wedged engine that ignores STOP + clients that disconnect first restarts forever, circuit never opens, operator never sees exhausted.
Ruling: R3 Important 2 — bound the D-2 exemption: at most config_defaults::kMaxConsecutiveUncountedRestarts (3) consecutive uncounted client-STOP restarts with no successful DONE in between; past that, client-STOP drain failures count normally; a DONE resets the streak. Keeps D-2's intent (an isolated disconnect during a long prefill never opens the circuit) while a broken engine still reaches circuit_open — spec §3.3 circuit is binding over an unbounded exemption — cost if wrong: owner may prefer a different N or none (one constant). OWNER-VISIBLE.
Ruling: R3 fix round 1 also takes Minor 1 (document direct report_fatal(client_stop_drain_failed) as test-only; SP3 must derive via kill_reason_for), Minor 2 (reset stop_cause_ at top of begin_generation), Minor 4 (comment "not counted (SP3 D-2, risk R4)") — cost if wrong: none.
Task R3: minor (deferred → controller/SP3): SP3 spec §2.6 lines ~317-319 and R4 row ~1298 describe pre-D-2 behaviour; SP3 P6 must call stop(EngineStopCause::client_cancelled)
Task R3: fix round 1/5 (5 addressed, 0 open — healthy-window reset on uncounted end; bound kMaxConsecutiveUncountedRestarts=3 with DONE reset; direct report_fatal test-only; stop_cause_ reset at top; comment; commits f569a524..7c9e7717)
Task R3: minor (deferred): stray begin_generation during an active client-STOP generation erases the cause (errs toward counting; header note for SP3); a cancel-finish DONE resets the streak (say so in the owner-visible bound text)
Task R3: complete (commits 26ff9739..7c9e7717, review clean)
Task R1: dispatched (BASE 7c9e7717, opus)
Task R1: implementer DONE_WITH_CONCERNS (2086b58b; linux 298 2 skip, wine 161, pytest 13); read-to-DONE now uses remainder of the same generation deadline; POSIX stdin write end O_NONBLOCK; STOP/QUIT bounded by 1 s each
Task R1: review Approved. Read-to-DONE sharing the generation deadline judged consistent with K1/RF10 (recorded in handoff, owner-visible).
Task R1: minor (deferred): no test pins read_generation gets the remainder (FakeTransport ignores read timeout); one-line pipe2/nonblocking start statement hard to read
Task R1: complete (commits 7c9e7717..2086b58b, review clean)
Task R5: dispatched (BASE 2086b58b, opus)
Task R5: implementer DONE_WITH_CONCERNS (667d5ba2; linux 301 2 skip, wine 164, pytest 13).
Ruling: R5 forcible-name set must include the four names the controller brief lists (STRATA_GLM_SLOTS, STRATA_GLM_LOOP_THINK, STRATA_GLM_LOOP_ANSWER, STRATA_WATCHDOG_S) plus CUDA_DEVICE_ORDER/CUDA_VISIBLE_DEVICES, and the hook must be able to derive values from the EngineEnvironment (CUDA_VISIBLE_DEVICES comes from the configured gpu list per SP6 §4.9) — the controller ruling names exactly these; leaving them out forces SP6 to reopen SP2's mechanism — cost if wrong: SP6 trims the set.
Task R5: review Needs fixes — Imp1 forcible set lacks STRATA_GLM_SLOTS/LOOP_THINK/LOOP_ANSWER (R5 ruling); Imp2 hook cannot derive from EngineEnvironment; Imp3 renderer returns {} on refused merge (swallowed error once derived values exist); Imp4 mechanism cannot suppress generic rows (§4.9 drops STRATA_WATCHDOG_IO_S/STRATA_HIPBLASLT_TUNING for glm) and STRATA_GLM_SPLIT=0 is outside the ruled set.
Ruling: R5 Imp4 — the family hook may also SUPPRESS generic optional rows (names from a fixed suppressible set: STRATA_WATCHDOG_IO_S, STRATA_HIPBLASLT_TUNING, STRATA_WATCHDOG_S), and STRATA_GLM_SPLIT joins the forcible set (§4.9 forces =0); STRATA_GLM_RAM_HEADROOM_GB stays out (depends on SP6 M2, draft) — mechanism-only, keeps SP6 from reopening SP2 — cost if wrong: SP6 trims a name.
Ruling: R5 Imp3 — a refused family merge is a startup failure surfaced to the caller (optional/bool + fixed-literal log; the generation is not spawned with an empty environment) — no swallowed errors — cost if wrong: none.
Task R5: fix round 1/5 (6 addressed, 0 open — ruled forcible set of 7; derived owning rows; refused family = SpawnFailure; suppression set of 3; glm comment; strata pin unchanged; commits 667d5ba2..2a534292)
Task R5: minor (deferred → SP6): generic render_* remain public under natural names (only render_family_environment applies the family) — SP6 keeps one spawn path or narrows visibility; EngineLaunchPlan::family_hook test seam on production struct (defaults to production, unreachable from config)
Task R5: complete (commits 2086b58b..2a534292, review clean)
Task R4: dispatched (BASE 2a534292, opus)
Task R4: implementer DONE_WITH_CONCERNS (8565f499; linux 308 2 skip, wine 168, pytest 13); SP4 HANDOFF ITEMS: Task 26 must close WinLaunched::thread and set limit_active_process_to_one + restrict_ui (off by default to keep the engine Job unchanged); 740 returned as error code
Task R4: review Approved (no Critical/Important). Minor 1 flags fail-open defaults for SP4.
Ruling: RF11 amended — WinLaunchSpec limit_active_process_to_one and restrict_ui default to TRUE (confined by default); the engine path sets both false explicitly (Job unchanged). SP4 Task 26's 10-positional-field initializer then gets its spec §3.2 Job without a plan change — secure default over my earlier "only when asked" wording — cost if wrong: a future caller wanting an unconfined job must opt out.
Ruling: R4 fix round 1 (ruling-driven) also takes Minors 3 (header: hook must not close/dup2 onto 0..kReportFd), 4 (read_report bound/comment), 5 (non-inheritable extra handle case → ERROR_INVALID_HANDLE at attributes), 6 (document extras distinct from std handles); Minor 2 (WinLaunched::thread must be closed by SP4) stays a documented SP4 handoff item (addendum names thread) — cost if wrong: none.
Task R4: fix round 1/5 (6 addressed, 0 open — Job limits default on/engine opts out; hook fd contract; read_report bounded 16; non-inheritable handle case; distinct-handles doc; SP4 must close thread; commits 8565f499..a12fa0b0)
Task R4: complete (commits 2a534292..a12fa0b0, review clean). SP4 handoff: close WinLaunched::thread; Job defaults confined.
Task 28: dispatched (BASE a12fa0b0, opus)
Task 28: implementer DONE_WITH_CONCERNS (cb5b2822; linux 308 2 skip, wine 168, pytest 13); K4 brief comment false (listen/accept sockets lack SOCK_CLOEXEC) → true comment + SP3 known gap; FATAL_PREFIXES re-pinned server.py:734-735; text-only edits to ready_timeout_s description/README /health line/kEngineFatalPrefixes comment; SP1 deployment.md Windows token caveat left for owner
Task 28: review Needs fixes — Important: RF14 flip incomplete (AGENT_HANDOFF.md:733 SP1 final-fix-wave 4 windows-runtime cases still "pending", contradicting the flipped gap-2 row; README:28-29 "pending"). Fix round 1 also takes docs Minors 1-6 (CancelIoEx wording; threat-model socket exception; spec K4 O_CLOEXEC contradiction as owner item; Fatal-lines pre-READY wording; ERR→1 exit row; SP1 gap 1 handle-passing status).
Ruling: K4 brief text ("every frontend fd O_CLOEXEC") was false — implementer's true comment + SP3 known gap accepted; spec §4.7 K4 row contradiction recorded as owner item (spec edits limited to citation re-pins) — cost if wrong: none.
Task 28: fix round 1/5 (6 addressed, 1 open — AGENT_HANDOFF.md:519-520 token-reader Windows cases still 'pending windows-msys2-clang'; commits cb5b2822..179dfb2c)
Task 28: fix round 2/5 (1 addressed, 0 open — last SP1 pending flipped; commits 179dfb2c..b9d707de)
Task 28: minor (deferred): SP1 plan/spec historical text still says "pending MSYS2" (design-time templates, not status)
Task 28: complete (commits a12fa0b0..b9d707de, review clean)
Final review: dispatched (MERGE_BASE 9ded0a5c = run-2 base)
Final review: With fixes — Important: (1) idle HTTP peer turns SIGTERM into _Exit(1) (§3.7/C4); (2) unreaped engine at shutdown still exits 0 (deferred Task 15 minor promoted). Minors: NNP hint condition; C4 row 4.25→4.35 s; g_signalled atomic + comment; SP3-facing header caveats (R2 #62-64, R3 notes); reap-poll dup; state/cache message; Windows shutdown_listener race; sweep_orphans SP4 keep. Stale deferred entries already resolved at HEAD: 16 cgroup dir, 29a wording, 18 README + CUDA cache, 19 glm test + citations, 25 cwd.
Ruling: Final fix wave = ONE dispatch with final-fix-run2-brief.md (I1, I2, M1-M7); Windows race + SP4 sweep keep recorded as notes, not code — cost if wrong: a second look in SP3/SP4.
Final review: fix wave dispatched (FIX_BASE b9d707de, opus)
Final review: fix wave DONE_WITH_CONCERNS (b3b84cd4..2fa870b8; linux 312 2 skip, wine 171, pytest 13); M3 extended with in-handler counter waited on by release_shutdown_signals; I2 adds engine_left_unreaped() + kEngineLeftUnreapedLine (no _Exit in finish)
Final review: fix wave re-review clean (9/9 addressed; commits b9d707de..2fa870b8). Residual Minors parked:
Final review: parked — engine_supervisor.hpp:123 "one exception" omits Windows: a reaped generation still leased in retired_ at supervisor destruction runs cleanup() (TerminateJobObject+wait) on the destroying thread — Ruling: harmless (wait returns at once, Job kill-on-close); SP3 contract wording fix deferred to SP3's first touch of the header — cost if wrong: SP3 reads one incomplete sentence.
Final review: parked — Windows send_all to a stalled reader is not sliced (only the request read is); POSIX bound holds via SHUT_RDWR — Ruling: added to the MSYS2 watch list in this report; header comment overstates for Windows send — cost if wrong: Windows SIGTERM with a stalled reader may hit _Exit(1).
Controller verification @ 2fa870b8: linux-gcc 0 warnings, 100% of 312 (2 skips: proc03 other-uid, proc05 cgroup); windows-llvm-mingw-cross 0 warnings, wine 100% of 171; pytest 13; tree clean; trailers/author verified on last 3 commits.
Run 2: COMPLETE.
