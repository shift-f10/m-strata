# Sub-controller brief — SP2 second run (post-SP1 integration + remaining tasks)

Read `subcontroller-common.md` (same directory) first; it binds. Then this file.

- **Plan:** `docs/superpowers/plans/2026-10-13-sp2-portable-supervision.md` (30 tasks incl. 6a/6b)
- **Spec:** `docs/superpowers/specs/2026-10-13-sp2-portable-supervision-design.md` (`no_new_privileges` DEFAULT ON)
- **Worktree:** `/opt/projects/ai/.worktrees/sp2`, branch `sp2-portable-supervision`
- **HEAD at launch:** `9ded0a5c` — a MERGE COMMIT that brings `main` (SP1 merged at e7f01b9e, upstream v0.1.44.1, CI runner pin, all approved specs) into the SP2 branch. Verify `git log --oneline -1` shows it and `git submodule status` shows ` 296b59d2… upstream (v0.1.44.1)`.
- **Existing ledger:** `.superpowers/sdd/2026-10-13-sp2-portable-supervision/progress.md` — first line names your plan; Tasks 1,2,3,4,5,6a,7–14,17,20–24 are `complete` (first run). Resume there; never re-dispatch them. Also read `FINAL-REPORT.md` in that directory (first-run rulings, the carried-forward list, deferred minors).
- **Final report path:** `/opt/projects/ai/.worktrees/sp2/.superpowers/sdd/2026-10-13-sp2-portable-supervision/FINAL-REPORT-run2.md`

## Baseline on this merged tree (controller-verified before launch)
linux-gcc 244/247 (3 known failures: server_tests.harness_server_lifecycle_selftest, proc01_server_passes_literal_argv, proc05_frontend_sigkill_kills_engine — the server harness passes pre-SP1 arguments; fixing them is Task 29's first step), wine 141/141, pytest 13, 0 warnings on both presets. SP1 is in: typed `FrontendConfig`, `config_schema()`, canonical paths/roots, token store/reader/verifier, `init`/`--check`, `install_process_defaults()` (RLIMIT_CORE 0, PR_SET_DUMPABLE), SP1's test binaries and 59 acceptance cases. Line numbers in the plan are from 7ddcd755 — locate everything by quoted text.

## Scope for this run — in this order
1. **Task 29 (post-SP1 integration) FIRST**, re-read against the real merged tree: the merge compiled and all tests passed, so Task 29 becomes "reconcile": SP2's supervisor wiring vs SP1's `secure_server_main.cpp` structure, `FrontendConfig`/`EngineLimits` vs SP2's supervisor config, the environment allowlist vs SP1's `token.source = env:` read, duplicated helpers (e.g. two `valid_utf8`, the SP0 `EngineProcess::terminate` lacking C1's group-SIGKILL — decide per the plan/spec), handoff tables merged (both SP1 and SP2 sections exist; make them consistent). Ledger each ruling.
2. Then the WAITS-FOR-SP1 tasks in plan order: **6b, 15, 16, 18, 19, 25, 26, 27, 28**, honoring the binding merge note at the top of the plan's Task 15 (Lease must not outlive the supervisor; failed SupervisorWake → start() refuses; exceptions escaping run() → circuit_open with `directory_iterator::increment(ec)`; errno save/restore in signal-handler notify(); decide CREATE_NO_WINDOW — ruling: set CREATE_NO_WINDOW for the engine).
3. **Owner/controller rulings to implement in this run (binding):**
   - **Task 22 BOUND (owner, 2026-10-14):** engine-stdin writes get an overall bound = the request's remaining deadline on top of the 1 s per 64 KiB chunk; Task 28 documents it in `docs/security/engine-protocol-limits.md`.
   - **SP3 D-1:** a lease-release notification (the serving side can wait for "lease released / state ready" without polling) and documented thread-safety of `acquire`/`snapshot`/`report_fatal`/`release` (they already take `mutex_` — document and test it).
   - **SP3 D-2:** a drain failure after a CLIENT-requested STOP restarts the engine but does NOT count toward the circuit.
   - **SP4 seam (from SP4 spec §3.2, APPROVED):** launch primitives gain `keep_fds` (lifted with `F_DUPFD_CLOEXEC` ≥ `kLiftFloor` before the 0..3 dup2 sequence; hook receives lifted numbers) and `int pre_exec(const void*, std::span<const int> lifted) noexcept` run after the descriptor sweep; non-zero → `'H'` marker + errno on `kReportFd` (3) → the launcher reports it distinctly from an execve failure; Windows `CreateProcessW` path gains `primary_token`, `extra_inherited_handles` and `crt_fd_block` (`lpReserved2`, packed, n entries, flag bytes). The engine launch passes none of these (defaults = today's behavior). Tests prove the seam with a fake child. Read SP4 spec §3.2 for the exact text.
   - **SP6 env family prerequisite (from SP6 spec, DRAFT but these items are controller-ruled):** `EngineEnvironment` gets a `family` field so a later `glm` family can force variable VALUES (`STRATA_GLM_SLOTS=0`, `STRATA_GLM_LOOP_THINK=0`, `STRATA_GLM_LOOP_ANSWER=0`, `STRATA_WATCHDOG_S=0`) and set `CUDA_VISIBLE_DEVICES` — implement only the mechanism (family enum + forced-values table hook) with the `strata` family unchanged; no GLM behavior yet.
4. Final whole-branch review over this run's commits (one fix dispatch + scoped re-review), final report, stop. The controller merges.

## Reminders
- The merge commit at HEAD was made by the controller; do not rewrite it. Commits: one per task, Fable trailers (literal lines in each commit command), `-c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com`.
- Expected counts grow per task; reconcile and ledger any difference rather than forcing.
- Windows-runtime claims only via the `windows-runtime` label and "pending MSYS2" notes; wine is smoke.
- `.worktrees/sp2/.superpowers/` is git-ignored; the first-run report and ledger live there — keep them.
