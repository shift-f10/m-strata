# SP2 run-2 handoff items (from FINAL-REPORT-run2.md, merged to main at 8333eccf on 2026-10-14)

Source of truth: `stale-worktrees/sp2-run2/.superpowers/sdd/2026-10-13-sp2-portable-supervision/{FINAL-REPORT-run2.md,progress.md,task-R4-report.md}`.

## For the SP4 Tasks 25–29 brief (binding carry-ins)
- Close `WinLaunched::thread` (SP2 left the thread handle open; SP4's launcher consumer must close it).
- `extra_inherited_handles` and `crt_fd_block` are `std::vector` (HANDLE / bytes), not spans/arrays.
- Pass `keep` to `sweep_orphans` so a live downloader child is not swept.
- Job limits in `WinLaunchSpec` default ON; the engine opts out — the downloader should keep them on.
- The two SP4 plan grep checks (Tasks 25/26 seam names) already hit on the merged tree; output in task-R4-report.md.
- `pre_exec` failure → 'H' marker + errno on kReportFd(3); launcher reports it distinctly (`downloader_confinement_failed`, exit 9 per SP4 spec).
- SP1 `process_runner` now launches through the shared non-elevated launcher — SP4's `strata` verbs should use the same path.

## For the SP3 M1 brief
- SP3 spec §2.6 and its R4 row describe pre-D-2 behaviour: now a drain failure after a client STOP restarts without counting, bounded at `config_defaults::kMaxConsecutiveUncountedRestarts = 3` consecutive, reset by a DONE. Re-anchor; spec text edit is a controller item (spec was not edited by SP2).
- Cancellation path: call `stop(EngineStopCause::client_cancelled)`; take the reason through `kill_reason_for(protocol)`.
- Lease-release notification: `wait_for_change` observes STATES, not transitions (D-1). acquire/snapshot/report_fatal/release are documented thread-safe (mutex_).
- Known gap carried to SP3: spec §4.7 K4 says every frontend fd is O_CLOEXEC; listening/accepted sockets are not — the descriptor sweep covers them. SP3's socket work should set CLOEXEC on listener and accepted sockets and close the gap in the spec.
- R1: the stdin write up to DONE shares the generation (request) deadline on top of 1 s / 64 KiB.
- Windows `send_all` to a stalled reader is not sliced (POSIX bound via SHUT_RDWR) — parked minor; SP3 M1's socket timeouts should slice writes on both platforms.
- Supervisor destructor wording gap (engine_supervisor.hpp ~:123, Windows reaped-but-leased generation runs cleanup on the destroying thread) — parked minor for SP3.

## For the SP6 plan-1 brief
- `EngineEnvironment` has a `family` field; `render_family_environment` is the single spawn path. Forcible set (7 names): STRATA_GLM_SLOTS, STRATA_GLM_LOOP_THINK, STRATA_GLM_LOOP_ANSWER, STRATA_WATCHDOG_S, CUDA_DEVICE_ORDER, CUDA_VISIBLE_DEVICES, +1 (see R5 ruling in progress.md); suppressible set 3 names. A refused family is a spawn failure. The glm table itself is still empty — SP6 plan 1 fills it.

## Owner items (pending the owner)
- All SP2 Windows-runtime rows are "pending MSYS2"; the watch list (14 rows) is in FINAL-REPORT-run2.md §Needs owner, item 1. Next CI run on main (8333eccf) exercises them.
- Delegated cgroup (C5 + C3 delegated branch) verified by reading only; live proof needs `systemd-run --user --scope -p Delegate=yes` approval on the dev host.
- Spec-vs-mechanism rulings to ratify: C3 grandchild → membership assertion (frontend SIGKILL writes no cgroup.kill); K4 sockets not CLOEXEC (gap recorded).
- SP1 test edits made by SP2: auth04 allow-list gains `engine.allow_new_privileges`; CFG-01 assertion; process_runner via non-elevated launcher; roots.cache / ready_timeout_s descriptions (README regenerated); ServerFixture state-dir + wait_engine_ready.
- SP1 deployment.md: Windows token caveat can now be removed (left for owner).
- G3 (elevation-requiring engine → spawn_failed) is a validation-kit item, unregistered in CTest.

## Controller follow-ups
- Edit SP3 spec §2.6 / R4 row to the D-2 bound before SP3 M1 launches (small doc commit on main).
- ~70 "minor (deferred)" lines in run-2 progress.md; final reviewer triaged, none blocks merge.
