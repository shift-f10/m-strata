# SP2 portable supervision — FINAL REPORT, run 2 (post-SP1 integration + remaining tasks + controller rulings)

**Status:** COMPLETE.
**Branch and HEAD:** branch sp2-portable-supervision, HEAD 2fa870b8757c1534a6889388cad42d8a112872b9.
**Base:** 9ded0a5c, the controller's merge of main and SP1. It was not rewritten. Nothing was pushed, merged or rebased.

**Tasks done, in run order:**
1. 29a (reconcile)
2. 6b, 15, 16, 18, 19, 25, 26, 27
3. 29b
4. R2 (SP3 D-1)
5. R3 (SP3 D-2)
6. R1 (Task 22 overall stdin-write bound, the owner's ruling)
7. R5 (SP6 env family)
8. R4 (SP4 launch seam)
9. 28 (docs truth pass and handoff)
10. The final whole-branch review, with one fix wave and its scoped re-review.

Every SP2 plan task (1–29, 6a/6b) is now complete across runs 1 and 2.

## Final test counts (controller re-run at HEAD 2fa870b8)
- **linux-gcc:** builds with 0 warnings. `100% tests passed out of 312`. Two tests are skipped, both allowed:
  - `secure_frontend_engine_child_tests.proc03_other_uid_cannot_open_engine_fd` needs a second uid; it runs in CI only.
  - `secure_frontend_engine_supervisor_tests.proc05_cgroup_kill_when_delegated` needs a delegated cgroup v2.
- **windows-llvm-mingw-cross:** builds with -Werror and 0 warnings. Wine ctest (WINEPREFIX prefix): `100% tests passed out of 171`. Tests labelled windows-runtime are excluded.
- **pytest:** 13 passed.
- **Baseline at run start:** linux-gcc 244/247 (the 3 server-harness failures, fixed in 29a), wine 141/141, pytest 13.

## Commits (git log --oneline 9ded0a5c..HEAD, 32)
```
2fa870b8 docs(sp2): final-review fix wave handoff, SP3 contract caveats (final review M2, M4, M7)
2b7c2f00 fix(sp2): signal flag is a lock-free atomic, reap-poll helper, state/cache message (final review M3, M5, M6)
d6b87d38 fix(sp2): NNP hint keys on the exit classification, not on a sent kill (final review M1, G2)
9721a266 fix(sp2): an unreaped engine at shutdown makes the frontend exit 1 (final review I2, C4)
b3b84cd4 fix(sp2): idle HTTP client no longer turns SIGTERM shutdown into exit 1 (final review I1, C4)
b9d707de docs: SP2 truth pass fix round 2; last SP1 MSYS2 'pending' statements flipped (K6, RF14)
179dfb2c docs: SP2 truth pass fix round 1; complete the SP1 MSYS2 flip, sharpen limits and threat-model rows (K4, K6)
cb5b2822 docs: SP2 truth pass; sweep cap, limits, threat model, release risks, handoff (K4, K6)
a12fa0b0 launch: SP4 seam fix round 1 — Job options default on, engine opts out; bounded read_report; hook and handle-list contracts
8565f499 launch: SP4 seam — keep_fds lift, pre-exec hook with 'H' report; Windows launch_in_job with token, extra handles, CRT block
2a534292 engine environment: derived family rows, ruled forcible/suppressible sets, refused family is a spawn failure (R5 fix round 1)
667d5ba2 engine environment: family field and forced-values hook; strata unchanged (SP6 prerequisite)
2086b58b engine I/O: stdin writes bounded by the request's remaining deadline as well as per chunk (K2, owner ruling)
7c9e7717 supervisor: bound uncounted client-STOP restarts; keep the healthy-window reset (SP3 D-2 fix round 1)
f569a524 supervisor: drain failure after a client STOP restarts without counting (SP3 D-2)
26ff9739 supervisor: lease-release notification and documented thread-safety (SP3 D-1)
9541f36d integration: CUDA cache path compared by identity; cache parent derived from the environment (Task 29b fix round 1)
28d40956 integration: SP2 options verified as schema keys; CUDA cache under roots.cache; engine cwd pinned (Task 29b)
1210d89c test: extension fields spawn nothing; extension env and flags never reach the engine (X1, X2)
b6524ec4 test support: one Windows argument quoter; launcher Job and launch diagnostics (Task 26 fix round 1)
34c02b5b secure_frontend: no_new_privs on by default with a documented switch; refuse root/elevated start (G2, G4)
bd3debb8 engine launch: cwd under the state dir; elevation is a spawn failure, never runas (G1, G3)
f6c8b3ee engine arguments: GPUs selected by a typed --gpu list; --gpu is frontend-owned (spec 3.9)
8f009ecc test support: --state-dir roots owned by the user on Windows (Task 18 fix round 2)
768fdea8 state dir: Windows owner-only check via SP1's private-DACL helpers; startup and lib_dirs pins (Task 18 fix round 1)
58c21385 engine launch: typed environment allowlist below an owner-only --state-dir (N1, N2, N3)
9b50d1bd cgroup: per-frontend generation directories, bounded removal, downgrade log, spawn-failure cleanup (C5, C3; Task 16 fix round 1)
168e06ca posix launch: cgroup.kill per generation when a delegated cgroup v2 is detected (C5)
b0ab61ca secure_frontend: SIGTERM/SIGINT run an orderly shutdown that leaves no descendant (C4, C7)
e28f6eeb secure_server: READY-deadline and V4 /health proofs against the wired server; fail_closed sweeps (6b fix round 1)
0e23d220 secure_server: the supervisor owns the engine; /health starting is 503 again (V2, V11, V8)
5ac39bb4 integration: server harness launches a private engine copy; supervision defaults in config_defaults; pipe names from SP1's CSPRNG (Task 29a)
```
These tasks needed review fix rounds:
- One round each: 6b, 15, 16, 26, 29b, R3, R5, R4.
- Two rounds each: 18 and 28.

No task reached the 5-round breaker.

## Final whole-branch review
**Verdict: "With fixes". The fixes are done, and the scoped re-review is clean (9 of 9 items addressed).**
- **I1:** an idle HTTP client made a SIGTERM exit 1 through `_Exit`. Fixed: the in-flight client is shut under a mutex, and the request read is sliced into 100 ms waits.
- **I2:** an engine left unreaped at shutdown still gave exit 0. Fixed: the frontend now exits 1 and logs a fixed line.
- **M1–M7:**
  - the NNP hint condition;
  - the 4.35 s worst-case text;
  - an atomic signal flag, with a counter that is waited on when handlers are released;
  - caveats in the SP3-facing header contract;
  - a shared reap helper;
  - the state/cache message;
  - handoff and watch-list notes.

**Two residual minors, parked with rulings (no second fix wave):**
- On Windows, a reaped generation that is still leased at supervisor destruction runs cleanup on the destroying thread. The header's "one exception" wording leaves this out. SP3 should fix the wording.
- On Windows, `send_all` to a stalled reader is not sliced. This is now on the MSYS2 watch list.

**Triage:** the final reviewer went through all deferred minors. Only Task 15's "unreaped engine still exits 0" had to be fixed before merge; it was (I2). Several other deferred entries were already resolved at HEAD.

## Rulings I made (in ledger order; each line has why and the cost if wrong)
- Ruling: Run-2 rules files updated in place (implementer-rules.md, reviewer-constraints.md: secure_server_main.cpp/SP1 files now in scope; baseline 244/247; seam-contracts-run2.md binding names); SP4 plan Tasks 25-26 excerpt copied to sp4-plan-tasks-25-26.md so no agent opens the plan-sp4 worktree — cost if wrong: none.
- Ruling: Common brief's submodule line (7695859a v0.1.43) is superseded by the run-2 brief (296b59d2 v0.1.44.1) — the merge brought upstream v0.1.44.1 from main — cost if wrong: none.
- Ruling: Workspace is kept at the end (holds first-run + run-2 reports; recursive deletion is forbidden by the guard) — cost if wrong: one git-ignored dir.
- Ruling: R4 Windows types follow the controller addendum where it differs from the SP4 plan excerpt (addendum says the SP4 plan is being amended and its names are final): extra_inherited_handles = std::vector<HANDLE>; WinLaunched = {process, thread, job} (superset of plan's {process, job} and addendum's process/thread); keep_fds = std::span<const int> (addendum allows span or vector; SP4 plan uses span) — cost if wrong: a type tweak when SP4 Task 26 compiles against it.
- Ruling: RF1 run order 29a -> 6b -> 15 -> 16 -> 18 -> 19 -> 25 -> 26 -> 27 -> 29b -> R2 -> R3 -> R1 -> R5 -> R4 -> 28 (R tasks after the code they touch settles; R4 refactors launch code once after 16/25/26; 28 last so it documents R1 and flips SP1 MSYS2 rows) — brief's literal order puts 28 before the rulings, but 28 must document R1 — cost if wrong: re-anchoring churn.
- Ruling: RF2 29a scope = harness copies the fake into a private <root>/bin (0700) and exposes engine_path() (A2 expectation updated); Task 29 Step 4 (fold supervision_defaults into config_defaults + supervisor_config(FrontendConfig) with its test); Step 5b (pipe name from SP1 fill_random); handoff reconcile (SP2 section after SP1, post-merge baseline, "SP0 server"->"server"); EngineProcess test-only comment; reconcile items of preflight B.3. 29b = remaining Steps 1/3/5a/6-9 verification + cli_sp2 test. Later briefs spelling supervision_defaults:: read config_defaults:: — cost if wrong: rename churn.
- Ruling: RF3 6b applies Task 29 Step 6 and adds f.wait_engine_ready() to SP1 auth02_valid_token_succeeds_and_is_not_echoed, init_writes_config_and_token_then_server_starts, cfg03, cfg05 and the "5" branch of ready_timeout_is_configurable (eager READY makes /health 503 while starting) — cost if wrong: flaky SP1 cases.
- Ruling: RF4 pre-6b carry list: 6b owns start() refusing on failed SupervisorWake, exceptions escaping run()/factory/log -> circuit_open, directory_iterator::increment(ec); 15 owns errno save/restore in signal notify, Lease must not outlive shutdown(), CREATE_NO_WINDOW ON for the engine (run-2 brief ruling) — cost if wrong: items slip between reviews.
- Ruling: RF5 Task 15 finish(): keep sweep_retired(), retire(instance, reaped) and the 250 ms sweep; add EOF->TERM->KILL 2/1/1 s; drop 15's extra 500 ms sweep (shutdown_generation already sweeps) — worst case 4.25 s < 5 s (spec §3.1/§3.7) — cost if wrong: rare shutdown flake.
- Ruling: RF6 roots.state: validate() checks absolute/non-empty/no NUL only (no fs access; --check side-effect free); run() does the owner-only check (POSIX lstat: not symlink, euid owner, mode&077==0; Windows also SP1 open_root DACL) with Task 18's fixed message — cost if wrong: one test expectation.
- Ruling: RF7 Task 18 updates SP1 ServerFixture (state dir, base_flags, base_literals) and server_cli_test init_flags; 18/19/26 each regenerate the README key table; 26 extends SP1 auth04's boolean allow-list with engine.allow_new_privileges and calls it out in its commit + handoff row (owner-visible SP1 security-test edit) — cost if wrong: red SP1 tests.
- Ruling: RF8 Task 16 keeps posix_launch::prepare void; adds [[nodiscard]] bool open_cgroup(PreparedExec&, const EngineCommand&) at both engine call sites (SP4 seam says prepare unchanged) — cost if wrong: SP4 Task 25 compile break.
- Ruling: RF9 R5 family scoping: kNeverForwardedEnvironment stays "never inherited from the parent"; family forced values are frontend-rendered; N1 assertion applies to EngineFamily::strata; docs say "strata family: no GPU variable; glm (SP6) renders CUDA_DEVICE_ORDER/CUDA_VISIBLE_DEVICES from typed config" — cost if wrong: docs/code contradiction at SP6.
- Ruling: RF10 R1 shape: absolute stdin-write deadline on the transport from ChatExecutionService's generation deadline; keep 1 s/64 KiB; POSIX literal poll(...,1000) replaced by the constant; miss -> transport_write_failed -> write_timeout — cost if wrong: one interface touch over 5 test transports.
- Ruling: RF11 R4 shape: exec_child(..., const ExecOptions& = {}) keeps existing callers compiling; 'H'+errno read distinctly in both POSIX launchers; win_launch::launch_in_job takes the engine's flags/job limits/handle list/cwd/740 mapping; ACTIVE_PROCESS=1/UI limits only when the spec asks; engine job unchanged — cost if wrong: SP4 adapts one call.
- Ruling: RF12 SP0 EngineProcess::terminate left as is except a test-only comment (wait_for reaps; a later group SIGKILL could hit a recycled pgid; no production user) — cost if wrong: drift persists in a test-only class.
- Ruling: RF13 upstream citations re-pinned to v0.1.44.1 in 19/28 text (GPU env ~generate.cpp:1844-1852, usage :707; stdin reader ~:9467-9469; re-verify FATAL_PREFIXES serve/server.py:587-588) — cost if wrong: stale citations.
- Ruling: RF14 Task 28 flips only SP1 rows covered by CI run 38540719307 (MSYS2 193/193); SP2 W rows stay pending (run predates the SP2 merge) — cost if wrong: handoff overstates.
- Ruling: RF15 expected skips are 2 (R2 other-uid + cgroup delegation) not 1 where briefs 18/19 say "1 skipped"; expected linux total before R tasks ~272 — reconcile per task, never force — cost if wrong: none.
- Ruling: V4 "/health leaves ready within 1 s" server-level case (PF11 deferred it to 6b; the 6b brief omits it) is added in 6b's fix round 1 regardless of review outcome — spec V4 binding and the wiring now exists — cost if wrong: one extra case.
- Ruling: 6b Important 1 fixed in the TEST (preferred), not by rewording: "1" branch asserts wait_engine_ready(4s) false + stderr killed_by_supervisor(ready_timeout); "5" branch asserts no ready_timeout line — handoff must not overstate (PF3/PF13 precedent) and the plan's "503 either way" is satisfied by the stronger form — cost if wrong: timing sensitivity in one e2e case.
- Ruling: fix round 1 also takes Minor 1 (fail_closed runs sweep_orphans({},250ms) + release_resources when reaped — spec §3.7(b) sweep after every generation) and Minor 2 (CFG-01 assertion names the real message) — cost if wrong: small code.
- Ruling: Task 16 C3 delegated branch — spec C3 row says the grandchild is "asserted only when C5 active", but after a frontend SIGKILL nothing writes cgroup.kill (the frontend is dead), so asserting death would fail. The delegated branch instead asserts the grandchild is a member of the generation's strata-engine-<n> cgroup (so the scope/operator cgroup.kill reaps it) and logs that its death needs that cleanup; the spec-vs-mechanism gap goes to the owner in the final report — cost if wrong: a stronger assertion needed later (e.g. a watchdog writing cgroup.kill).
- Ruling: Task 16 Important 2 fixed despite plan naming: directory strata-engine-<pid>-<n>, EEXIST = failure (generation runs without cgroup, logged), test reads the generation dir via an accessor — a cross-instance kill is a correctness/security defect; the spec does not fix the name — cost if wrong: a naming tweak.
- Ruling: fix round 1 also takes Minors 1-4 (bounded ~100 ms cgroup removal inside the shutdown budget / no reset on failure; fixed-literal log on cgroup downgrade; rmdir on spawn-failure paths; C5 test asserts /proc/<gc>/cgroup membership) — cost if wrong: small code.
- Ruling: RF6 amended — on Windows the owner-only check is a no-reparse open (FILE_FLAG_OPEN_REPARSE_POINT, reject is_reparse_point) + ws::has_private_owner_dacl(handle, current_user) (same fixed message); Windows variant of state_dir_must_be_owner_only (extra Users read ACE) labelled windows-runtime; comment/handoff name the real check — Review Focus 1 intent binds over my wording — cost if wrong: MSYS2 round on the Windows variant.
- Ruling: fix round 1 also takes Minor 1 (CLI case: serve without roots.state refused; server case: stale engine-tmp/7 cleared at start), Minor 2 (reject platform list separator in engine.lib_dirs entries), Minor 3 as an accepted-limitation note in the handoff (lstat checks only the last component), Minor 6 (N1 case restores the env vars it sets) — cost if wrong: small code.
- Ruling: Task 19 render case keeps LABELS windows-runtime (wine +2 not +3) — it opens a canonical root like its SP1 siblings, which carry the label; a direct wine smoke passes — cost if wrong: CI wine never runs one case (MSYS2 does).
- Ruling: Task 26 Option A — extract the Task 3 harness's non-elevated (LUA-restricted, Safer fallback) launch into one shared test-support helper; SP1's process_runner uses it on Windows for strata-secure-server; the two add_test CLI checks are launched through it and labelled windows-runtime (wine total -2); refusal placement stays as the brief says (after help, before validation); no runtime override flag — global constraint "Elevation in tests (no override flag)" binds and already prescribes the LUA token for every server launch — cost if wrong: MSYS2 round if the LUA launch fails for SP1 cases (same exposure as F3 harness).
- Ruling: Task 27 minors (namespace placement; record-file opened + GEN positive control; non-empty env assertion; rejection-reason assertion) are batched into Task 29b, which edits server_test.cpp anyway — cost if wrong: none.
- Ruling: R2 thread-safety doc states the Task 15 guarantee (a Lease MAY outlive shutdown() safely via LeaseLink/detach_leases), not my brief's example "must not outlive" — the shipped, tested behaviour is stronger and SP3 relies on documented truth — cost if wrong: doc wording.
- Ruling: R3 Important 2 — bound the D-2 exemption: at most config_defaults::kMaxConsecutiveUncountedRestarts (3) consecutive uncounted client-STOP restarts with no successful DONE in between; past that, client-STOP drain failures count normally; a DONE resets the streak. Keeps D-2's intent (an isolated disconnect during a long prefill never opens the circuit) while a broken engine still reaches circuit_open — spec §3.3 circuit is binding over an unbounded exemption — cost if wrong: owner may prefer a different N or none (one constant). OWNER-VISIBLE.
- Ruling: R3 fix round 1 also takes Minor 1 (document direct report_fatal(client_stop_drain_failed) as test-only; SP3 must derive via kill_reason_for), Minor 2 (reset stop_cause_ at top of begin_generation), Minor 4 (comment "not counted (SP3 D-2, risk R4)") — cost if wrong: none.
- Ruling: R5 forcible-name set must include the four names the controller brief lists (STRATA_GLM_SLOTS, STRATA_GLM_LOOP_THINK, STRATA_GLM_LOOP_ANSWER, STRATA_WATCHDOG_S) plus CUDA_DEVICE_ORDER/CUDA_VISIBLE_DEVICES, and the hook must be able to derive values from the EngineEnvironment (CUDA_VISIBLE_DEVICES comes from the configured gpu list per SP6 §4.9) — the controller ruling names exactly these; leaving them out forces SP6 to reopen SP2's mechanism — cost if wrong: SP6 trims the set.
- Ruling: R5 Imp4 — the family hook may also SUPPRESS generic optional rows (names from a fixed suppressible set: STRATA_WATCHDOG_IO_S, STRATA_HIPBLASLT_TUNING, STRATA_WATCHDOG_S), and STRATA_GLM_SPLIT joins the forcible set (§4.9 forces =0); STRATA_GLM_RAM_HEADROOM_GB stays out (depends on SP6 M2, draft) — mechanism-only, keeps SP6 from reopening SP2 — cost if wrong: SP6 trims a name.
- Ruling: R5 Imp3 — a refused family merge is a startup failure surfaced to the caller (optional/bool + fixed-literal log; the generation is not spawned with an empty environment) — no swallowed errors — cost if wrong: none.
- Ruling: RF11 amended — WinLaunchSpec limit_active_process_to_one and restrict_ui default to TRUE (confined by default); the engine path sets both false explicitly (Job unchanged). SP4 Task 26's 10-positional-field initializer then gets its spec §3.2 Job without a plan change — secure default over my earlier "only when asked" wording — cost if wrong: a future caller wanting an unconfined job must opt out.
- Ruling: R4 fix round 1 (ruling-driven) also takes Minors 3 (header: hook must not close/dup2 onto 0..kReportFd), 4 (read_report bound/comment), 5 (non-inheritable extra handle case → ERROR_INVALID_HANDLE at attributes), 6 (document extras distinct from std handles); Minor 2 (WinLaunched::thread must be closed by SP4) stays a documented SP4 handoff item (addendum names thread) — cost if wrong: none.
- Ruling: K4 brief text ("every frontend fd O_CLOEXEC") was false — implementer's true comment + SP3 known gap accepted; spec §4.7 K4 row contradiction recorded as owner item (spec edits limited to citation re-pins) — cost if wrong: none.
- Ruling: Final fix wave = ONE dispatch with final-fix-run2-brief.md (I1, I2, M1-M7); Windows race + SP4 sweep keep recorded as notes, not code — cost if wrong: a second look in SP3/SP4.
- Final review: fix wave re-review clean (9/9 addressed; commits b9d707de..2fa870b8). Residual Minors parked:
- Final review: parked — engine_supervisor.hpp:123 "one exception" omits Windows: a reaped generation still leased in retired_ at supervisor destruction runs cleanup() (TerminateJobObject+wait) on the destroying thread — Ruling: harmless (wait returns at once, Job kill-on-close); SP3 contract wording fix deferred to SP3's first touch of the header — cost if wrong: SP3 reads one incomplete sentence.
- Final review: parked — Windows send_all to a stalled reader is not sliced (only the request read is); POSIX bound holds via SHUT_RDWR — Ruling: added to the MSYS2 watch list in this report; header comment overstates for Windows send — cost if wrong: Windows SIGTERM with a stalled reader may hit _Exit(1).

## Needs owner (the owner) / controller attention
1. **The MSYS2 CLANG64 run is authoritative for every SP2 Windows-runtime row.** None of these rows is marked verified. Watch list:
   - the Windows A2 argv[0] final-path form;
   - the LUA token executing the copy in the harness `bin/`;
   - console handler, CTRL_CLOSE and CREATE_NO_WINDOW stdio, including the CTRL_CLOSE exit-code race;
   - the owner-only `--state-dir` check, `make_owner_private_directory` roots, and the junction case;
   - SP1 server cases and the 2 CLI checks launched through the shared `non_elevated_launch`;
   - G1;
   - the roots.cache CUDA path;
   - SP4 seam Job limits;
   - `GetProcessHandleCount` exactly ±0;
   - UCRT `__fastfail` / WER;
   - the R2 sudo CI path;
   - Windows `shutdown_listener` closesocket racing `accept`;
   - Windows `send_all` to a stalled reader;
   - Windows idle-client SIGTERM.
2. **Delegated cgroup (C5, and C3's delegated branch) is verified by reading only.** A live proof needs approval to run `systemd-run --user --scope -p Delegate=yes`.
3. **Spec-vs-mechanism gaps:**
   - Spec C3 says "grandchild asserted when C5 active", but after a frontend SIGKILL nothing writes `cgroup.kill`. I ruled for a membership assertion instead.
   - Spec §4.7 K4 says "all frontend fds O_CLOEXEC", but the listening and accepted sockets are not. The sweep covers them. Recorded as a Known gap for SP3.
4. **Owner-visible controller rulings:**
   - **D-2 bound:** `kMaxConsecutiveUncountedRestarts = 3`. A DONE resets the streak.
   - **R1:** the read up to DONE now shares the generation deadline.
   - **SP4 Job defaults:** `WinLaunchSpec` Job limits are on by default, and the engine opts out.
   - **RF6 amended:** the Windows `--state-dir` check is `has_private_owner_dacl` plus a no-reparse open.
   - **Owner-visible SP1 test edits:**
     - auth04 boolean allow-list: adds `engine.allow_new_privileges`;
     - CFG-01 assertion;
     - the SP1 `process_runner` launches the server through the shared non-elevated launcher;
     - the SP1 `roots.cache` / `ready_timeout_s` descriptions, with the README table regenerated;
     - `ServerFixture` state-dir and `wait_engine_ready`.
5. **SP3 handoff:**
   - The SP3 spec §2.6 and the R4 row describe behaviour from before D-2. The spec was not edited.
   - SP3 must call `stop(EngineStopCause::client_cancelled)` and get the reason through `kill_reason_for(protocol)`.
   - SP3 must use `wait_for_change`, which observes states, not transitions.
6. **SP4 handoff:**
   - Close `WinLaunched::thread`.
   - `extra_inherited_handles` and `crt_fd_block` are `std::vector`.
   - Pass `keep` to `sweep_orphans` so a live downloader is not swept.
   - The two SP4 grep checks hit; their output is in task-R4-report.md.
7. **SP6 handoff:**
   - Fill the glm table: 7 names can be forced and 3 suppressed.
   - Keep the single spawn path through `render_family_environment`.
8. **SP1 deployment.md:** the Windows token caveat can now be removed. It was left for the owner.
9. **Trailers:** every commit carries the literal Fable 5.1 trailer and the session line, author shift-f10. The implementers ran on the opus/sonnet aliases.

## Parked items and concerns
- About 70 run-2 "minor (deferred)" lines are in progress.md. The final reviewer triaged them, and none blocks merge.
- G3 (an engine requiring elevation gives spawn_failed) is a validation-kit item. It is unregistered in CTest, as the owner acknowledged.
- Nothing was run on a GPU.
- I did not delete the SDD workspace. It holds the run-1 and run-2 reports and ledger, and recursive deletion is forbidden.
