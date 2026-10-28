# Sub-controller brief — SP4 part 2: confined downloader, `strata` verbs end-to-end (Tasks 25–29)

Read `subcontroller-common.md` (same directory) first; it binds. Then this file.

- **Plan:** `docs/superpowers/plans/2026-10-14-sp4-acquire.md` — Tasks **25, 26, 27, 28, 29** only (1–24b are merged on main; their ledger is NOT yours — see below).
- **Spec:** `docs/superpowers/specs/2026-10-14-sp4-acquire-design.md` (APPROVED; C1–C7 inline). SP2's launch seam is specified in §3.2.
- **Owner decisions file:** `/tmp/claude-1000/-opt-projects/ee5cedea-4504-4508-8cef-d9fc7bb1a693/scratchpad/owner-decisions-2026-10-14.md` section C.
- **Worktree:** `/opt/projects/ai/.worktrees/sp4b`, branch `sp4-acquire-part2`, created from `main` — **HEAD at launch: `557a2cc7`** (= main with SP2 run 2, the Windows argv fix and SP4 Tasks 1–24b all merged at 4b9fdd31). Verify `git submodule status` shows ` 296b59d2… upstream (v0.1.44.1)`.
- **Baseline on this tree (controller-verified):** linux-gcc 520/520 (+2 skips: proc03_other_uid_cannot_open_engine_fd, proc05_cgroup_kill_when_delegated), linux-gcc-release 510/510 full and 6/6 with the release labels, wine 256/256, pytest 23, 0 warnings on all presets. MSYS2 real Windows for this exact tree: CI run on main 557a2cc7, in progress at launch — the controller forwards the result (previous main 863b3ff6 was 282/282).
- **Merge-time facts (merge 4b9fdd31):** SP4's manifest-side `EngineFamily` is now `ManifestFamily` (SP2 owns `EngineFamily`); `compose_engine_launch` renders SP2's typed `engine.gpu` list (expert) / appends `--gpu` (managed); supervisor profile/flavor come from `launch.profile`; the Windows UTF-8 argv helper is the shared library `secure_frontend_utf8_argv` (links shell32) already used by BOTH `strata-secure-server` and `strata` — do not re-add one.
- **Ledger:** start a FRESH ledger at the workspace `scripts/sdd-workspace` prints for this plan (the part-1 ledger lives in the retired part-1 worktree; the controller carries its parked items below). First line names the plan; add a second line `Scope: Tasks 25–29 (1–24b merged at 4b9fdd31)`.
- **Final report path:** `/opt/projects/ai/.worktrees/sp4b/.superpowers/sdd/2026-10-14-sp4-acquire/FINAL-REPORT-part2.md`

## First action after the skill's setup: the plan's seam check
Run Task 25's seam-check script verbatim (eight names in `src/posix_child_launch.hpp` / `src/win_child_launch.hpp`). SP2 run 2 landed every name (controller-verified on the merged tree; the grep output is in the retired SP2 report `task-R4-report.md`). If any grep fails, STOP and report — do not improvise the seam.

## SP2 run-2 facts the plan could not know (binding carry-ins; from the SP2 final report)
- `WinLaunched::thread` is a real open handle: the launcher's consumer (SP4's downloader supervisor) MUST close it (the engine path already does). Leaving it open shows up as a per-launch handle leak in `proc04_no_leak_across_restarts`-style checks.
- `extra_inherited_handles` and `crt_fd_block` are `std::vector` (HANDLE / bytes), not spans or fixed arrays — the controller addendum, not the plan excerpt, is final where they differ.
- `WinLaunchSpec` Job limits default ON and the engine opts out; the downloader keeps them ON.
- Pass `keep` to `sweep_orphans` so a live downloader child is never swept by the frontend's descriptor/process sweep.
- `pre_exec` non-zero → `'H'` + errno on `kReportFd` (3); the launcher surfaces it distinctly from an execve failure → SP4 maps it to `downloader_confinement_failed`, exit 9 (spec §8).
- Windows `strata-secure-server` argv is now UTF-8 from the wide command line (`CommandLineToArgvW`, links shell32; commit 863b3ff6). The `strata` binary's Windows entry MUST do the same (reuse the helper if it is factored; if it is still private to `secure_server_main.cpp`, factor it into a shared source in Task 27/28 rather than duplicating it) so `--config <path>` and model ids with non-ASCII characters arrive literally.
- SP1's `process_runner` and the server fixtures launch through the shared non-elevated launcher; the `strata` verbs' child launches go through the same path.
- Harness facts proven on real Windows (CI run 38604191725, 282/282): Job-member counts exclude exact `%SystemRoot%\System32\conhost.exe` (CREATE_NO_WINDOW spawns one); harness `root()` uses the server's resolved long-path form, never 8.3 names.

## Plan-specific reminders
- NEVER run upstream's pack tooling or download model files on this host; no real network in tests (fake downloader with scenarios, record channel fd 4 / CRT entry 4 exists only in hook builds; positive controls required). The optional real-curl check is documentation only.
- Landlock: dev host kernel 5.15 (ABI 1) — confinement proofs run here; SKIP with a fixed reason only when the kernel has no Landlock at all. `linux-gcc-release` (hooks OFF) must stay green: `-L "release-only|release-also"`.
- `cfg01_*` cases stay in `secure_frontend_typed_config_tests`; `upstream.version` is the engine-version source; `--config <file>` required on every `strata` verb; catalog ships EMPTY (D5(D)); developer bundles via `make_bundle.py` + `--trust-manifest`.
- Commits: one per task; `-c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com`; trailer lines LITERAL in every commit command: `Co-Authored-By: <Claude Opus 5.5 | Claude Sonnet 5.5> <noreply@anthropic.com>` and `Claude-Session: <session link>`.
- Handoff: Task 28/29 docs update `docs/AGENT_HANDOFF.md` (SP4 section + Known small gaps), `docs/security/threat-model.md`, `docs/PROVENANCE.md`, `docs/secure-frontend-ci.md`; the SP2 and winfix rows there are current — keep them.
- Expected counts grow per task; reconcile and ledger differences rather than forcing.
- After Task 29: final whole-branch review over 25–29 (one fix dispatch + scoped re-review), final report `COMPLETE (Tasks 25–29)`, stop. The controller merges.
