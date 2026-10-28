# SP2 portable supervision — FINAL REPORT (parallel-safe run)

**Status:** COMPLETE (parallel-safe set)
**Branch:** sp2-portable-supervision. **HEAD:** a1ec5bc0423213712211217f825b0b281820ca6e. **Base:** 17d2452d. Nothing was pushed, merged or rebased.
**Tasks done:** 1, 2, 3, 4, 5, 6a, 7, 8, 9, 10, 11, 12, 13, 14, 17, 20, 21, 22, 23 and 24, then the final whole-branch review with one fix wave.
**Not started (they wait for the SP1 merge):** 6b, 15, 16, 18, 19, 25, 26, 27, 28 and 29.

## Final test counts (controller re-run at HEAD)
- **linux-gcc:** `100% tests passed out of 143`. One test was skipped: `secure_frontend_engine_child_tests.proc03_other_uid_cannot_open_engine_fd`. It needs a second user, which this machine does not have, so it can only run in CI.
- **wine** (windows-llvm-mingw-cross, run with the WINEPREFIX prefix): `100% tests passed out of 115`. Cases labelled windows-runtime are excluded. The cross build is clean with -Werror.
- **pytest:** 13 passed.
- **Baselines before this run:** linux-gcc 111, wine 102.

## Commits (git log --oneline 17d2452d..HEAD)
```
a1ec5bc0 docs: SP2 handoff and threat model state only what is proven; Task 15 merge note
f05f3e52 test: PROC-03 R2 fails instead of skipping when CI demands it; probe distinguishes denial, with positive control (fix round 1)
4b2171ef test: PROC-03 other-uid denial in CI; same-uid reopen recorded as accepted risk (R2, R3)
28bd962a test: engine argv round-trips literally, Windows twin and end to end through the server (A1, A2)
4857dfa8 windows launch: R1 probe requires ERROR_PIPE_BUSY plus a rival server refusal; client-pid mismatch reports access denied (fix round 1)
799ea851 windows launch: owner-only random stdin pipe, verified client, overlapped writes with a deadline (N6, R1, K2)
94965a24 test: N5 handle canary gets a positive control and a checked snapshot (fix round 1)
6f61fa67 test: canary descriptors and handles never reach the engine (N5)
c3ca8ce7 posix launch: default signal dispositions and an empty mask in the engine (N4)
dc747455 windows launch: state exactly what the new process group does; add an unhandled-exception smoke (C6 fix round 1)
3759dee3 windows launch: engine in its own console group; Job dies on unhandled exceptions; no WER stall (C6, partial)
593fc39a posix launch: PR_SET_PDEATHSIG ties the engine to the frontend (C3)
38a61cda supervisor: sweep orphans at shutdown too; prove the sweep reaps (C2)
443957a0 supervisor: subreaper sweep reaps setsid and double-fork escapes (C2)
aafde3e6 posix launch: terminate always kills the whole process group (C1)
05516535 supervisor: an unreaped child opens the circuit; no leaks across restarts (V9, V10)
60998bf7 supervisor: READY timeout and drain failure kill, count and restart (V6, V7)
5cea53b5 protocol: upstream fatal verify lines kill and restart the engine (V5)
417c518b supervisor: monitor the engine between requests via pidfd / process handle (V4)
0e6ea993 supervisor: bounded restart with exponential backoff, healthy window and circuit (V3)
63918234 secure_frontend: engine supervisor core with eager READY and leases; router recovery literal (V2, V11 core)
3f780e79 engine process: keep and classify the child's exit status; Windows kill code 0x20000001 (V1)
8b2c6505 engine I/O: one deadline rule for READY, generation and line reads; terminate contract (K1, K3, K5)
0b78daa1 test: server harness with a non-elevated server token and no orphaned fakes (F3)
a40bd520 test: fake engine DUMP reports env, signals, cwd, ids and portable argv (F2)
0d7810fb test: standalone strata-supervision-fake-engine with SP2 failure modes on SP1's per-case registration (F1)
```

Six tasks needed one review fix round each before they were clean: 13, 17, 21, 22 and 24 had Important findings, and so did the final wave. No task reached the five-round limit.

## Final whole-branch review
**Verdict: "With fixes". The fixes are done.**

The reviewer found no code defect:
- The single-supervisor-thread rule (§3.1), object lifetimes and wake logic are sound.
- The timing-sensitive cases stayed stable when run in parallel (-j8) and repeated 5 times.

It found two Important problems, one in the docs and one in the plan:
- The threat model and handoff claimed Windows pipe hardening that MSYS2 has not proven yet.
- The plan's Task 15 code would undo two of my rulings: the one that retires instances on the supervisor thread, and the one that sweeps orphans at shutdown.

Commit a1ec5bc0 fixes both, along with 4 minor handoff wording issues. It changes docs only. The scoped re-review found all 6 items addressed and nothing new broken.

What is still open:
- **Before Task 6b** (these are also in the merge note at the top of the plan's Task 15):
  - A Lease must not outlive the supervisor. Today it can be left pointing at a destroyed supervisor after shutdown().
  - If SupervisorWake fails to construct, start() must refuse to start.
  - Exceptions that escape run() must lead to circuit_open. That includes using `directory_iterator::increment(ec)` so directory iteration cannot throw.
  - When notify() is called from a signal handler, it must save and restore errno.
  - Decide whether the engine gets CREATE_NO_WINDOW. Without it, a Ctrl-Break sent to the whole console still reaches the engine.
- **Check in the first MSYS2 run:**
  - The V9 test requires the GetProcessHandleCount value to be exactly unchanged. That strictness may be flaky.
  - UCRT `__fastfail` and Windows Error Reporting could still stall `proc04_windows_abort_does_not_stall`.
  - R2 runs for real for the first time in the GitHub Linux jobs. With the CI variable set, a sudo misconfiguration now fails the test instead of skipping it.
- **The two copies of the launch code have drifted:** the SP0 `EngineProcess::terminate` does not have C1's "always SIGKILL the whole group". Nothing in src/ uses it any more; only tests and the R2 runner do.
- **Deferred minor findings:** about 80, recorded as "minor (deferred)" lines in progress.md. The final reviewer went through them; none must be fixed before merge.

## Rulings I made (in order, each with its cost if wrong)
1. Skip the plan's "rebase onto main before Task 1". HEAD 17d2452d already is main and already contains the shared pre-task, and the owner's rules forbid rebasing. Cost if wrong: none.
2. Commit trailers use Claude Fable 5.1 plus the session URL, as the common brief says, not the plan's Opus trailer. Cost if wrong: the integrator has to amend the trailers.
3. The pre-flight conflict scan was handed to one read-only opus agent, because the plan is 518 KB. The table is in preflight-scan.md. Cost if wrong: a conflict the scan missed shows up in task review instead.
4. The final report stays inside the SDD workspace, and the workspace is not deleted. Cost if wrong: one git-ignored directory is left behind.
5. **PF1:** the Task 22 K2 prompt is 60,000 '0' characters instead of 70,000. 70,000 is over the renderer's 65,536-byte cap, so the test would never reach the write. The production cap was not raised. Cost if wrong: one MSYS2 CI round.
6. **PF2:** the fake gets a `--default-error-mode` flag, so `proc04_windows_abort_does_not_stall` exercises the error mode the engine inherits. This test is a smoke check, not proof of C6. Cost if wrong: one extra fake flag.
7. **PF3:** the C6 handoff row is written as partial, and the comment about Task 15 is in the future tense. Cost if wrong: none.
8. **PF4:** the Task 24 workflow edits use re-located anchors (after the line `# STRATA_NATIVE_EXPERTS=OFF ...`, and the ctest step now at lines 58-59, for both presets). Cost if wrong: a workflow edit lands in the wrong place.
9. **PF5:** the fake keeps the name `strata-supervision-fake-engine`, though spec F1 calls it `strata-fake-engine`. The deviation is recorded in the handoff. Cost if wrong: a later rename.
10. **PF6:** Task 3's byte_symbol now uses the real GPT-2 printable byte ranges. Cost if wrong: none.
11. **PF7:** members of the Interfaces block that nothing uses yet are kept, because later tasks need them. Cost if wrong: review churn.
12. **PF8:** Task 4's failing (RED) run may show more FAILED lines than the brief lists. Cost if wrong: none.
13. **PF9:** the missing-engine case is labelled UNIT. Cost if wrong: none.
14. **PF10:** `std::_Exit(1)` when shutdown runs past its 5 s bound is kept, as spec §3.7 says. Cost if wrong: review churn.
15. **PF11:** V4 is proven at supervisor level (wait_for_state). The /health form of the test comes with Task 6b. Cost if wrong: the V4 row stays partial until 6b.
16. The owner's Fable 5.1 trailer overrides the attribution reminders the implementers' harness gave them. The Task 2 commit was amended, message only. Cost if wrong: none.
17. **Task 6a, then 7:** ended EngineInstances are destroyed only on the supervisor thread, through retire, retired_ and abandoned_ (spec §3.1). Cost if wrong: a little extra code.
18. **Task 7, then 8:** a stale wake could skip the backoff delay. schedule_restart now sleeps until the deadline, release() notifies, and the circuit_open loop sweeps (spec §3.3). Cost if wrong: a small extra change.
19. **Task 9, then 10:** a fatal line before READY is now logged as fatal_line, through `kill_reason_for`, and the V5 row is corrected (spec §3.8). Cost if wrong: one line and one test case.
20. **PF12:** the production `list_own_children` stays separate from the harness's `children_of`. Cost if wrong: a small duplication.
21. **Task 13:** the orphan sweep also runs on the shutdown path (250 ms bound), and the test asserts the grandchild was reaped (ECHILD) (spec §3.7). Cost if wrong: Task 15 may restructure the sweep.
22. **Task 17:** the comments and C6 row were reworded. CREATE_NEW_PROCESS_GROUP keeps Ctrl-C and group-targeted Ctrl-Break away from the engine, but a Ctrl-Break sent to the whole console still reaches it. CREATE_NO_WINDOW was deferred to Task 15 or the owner. Cost if wrong: a race on a keyboard Ctrl-Break until Task 15.
23. **Task 21:** the Windows handle-canary test gets a positive control and a checked snapshot, because a vacuous test is not proof. Cost if wrong: a little test code.
24. **PF13:** the Task 22 handoff row says "limits doc update pending Task 28". Cost if wrong: none.
25. **Task 22:** the R1 test must open with GENERIC_READ and get exactly ERROR_PIPE_BUSY, and a rival server must be refused. The client-pid mismatch now reports access denied. Cost if wrong: the test may need adjusting in MSYS2.
26. **Task 22:** spec §3.10's "remaining deadline" is implemented as a 1 s deadline per 64 KiB chunk, with no overall bound. Accepted for this run; Task 28's limits doc must state it. Cost if wrong: one more deadline in write_line. **This is escalated to the owner; see below.**
27. **Task 24:** when CI requires R2 and the sudo precondition fails, the test now fails instead of skipping. The probe has a same-uid positive control and distinct exit codes, and it checks the engine is still alive afterwards. R3 prints errno when it fails. Cost if wrong: R2 may need a tweak after its first CI run.
28. **Final fix wave**, docs only:
    - The threat-model sentence and the Known-gaps entry are qualified as pending MSYS2.
    - The handoff wording is fixed.
    - A binding merge note is added at the top of the plan's Task 15.

    Cost if wrong: doc churn.
29. The pre-Task-6b list above is carried forward for the wiring task, not fixed in this run. Cost if wrong: these items surface in the 6b review.
30. The Task 22 per-chunk write deadline is escalated to the owner as a spec deviation, because the final reviewer disagrees with accepting it on my ruling alone. The behaviour is kept. Cost if wrong: an overall write deadline is added later.

## Needs owner (the owner) attention
1. **Task 22 deviation.**
   - Spec §3.10 says stdin writes wait "with the remaining deadline".
   - What was built is a 1 s deadline per 64 KiB chunk (the plan's kStdinWriteDeadline), with no overall bound.
   - Consequence: an engine that drains its stdin slowly could hold the serving thread for about 256 s on a 16 MiB line.
   - This needs your acknowledgement, or an overall bound. Task 28's limits doc must state it either way.
2. **Every Windows-runtime row is unproven** until the MSYS2 CLANG64 job runs after you push. That covers:
   - the Windows parts of F1–F3;
   - the Windows cases of V1, V3 and V4;
   - C6, which is partial anyway: launch flags only;
   - the N5 handle test;
   - N6, R1 and K2;
   - A1, and A2 on Windows;
   - the harness's restricted-token launch.

   Wine cannot exercise UAC, CompareObjectHandles, Windows Error Reporting or Job semantics.
3. **Trailer attribution.** Every commit carries "Co-Authored-By: Claude Fable 5.1", as the common brief requires, although the implementers ran on the opus alias (Opus 5.5). One Task 2 commit was amended, message only, to comply.
4. **Residual risks you accepted earlier, unchanged:**
   - §3.7: when the frontend is SIGKILLed and no cgroup is in use, descendants survive. The C3 test logs this.
   - §3.13: another process running as the same user can reopen the engine's stdin. R3 pins this.

## Parked items and concerns
- Spec V4 ("/health leaves ready within 1 s") is proven at supervisor level only. Spec C6 is partial; its server tests belong to Task 15.
- `docs/security/engine-protocol-limits.md` keeps its stale "Windows stdin writes" row until Task 28. The handoff says so.
- The fake-flag list in the plan's front matter does not include `--default-error-mode`, which Task 17 added.
- The merge note at the top of the plan's Task 15 should be removed once Task 15 is merged.
- The SDD workspace is kept, because it holds this report.
