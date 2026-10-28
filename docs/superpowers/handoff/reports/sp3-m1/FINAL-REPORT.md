# SP3 M1 sub-controller — final report (Tasks 12–23)

Date: 2026-10-21. Branch `sp3-api-streaming` (base `8520deee`), HEAD `11f22e47`, worktree
`.worktrees/sp3`. **Local only — never pushed** (owner ruling; GitHub is storage, Agents never push).
12 commits, 43 files, +3344/−121; `upstream/` untouched; all paths under `secure_frontend/`, `docs/`,
`.github/`.

## Status: COMPLETE

| Task | Commit | One-line result |
| --- | --- | --- |
| 12 | `66b13d88` | Concurrent accept thread + 8-worker pool, oldest-P1 eviction, refusal+linger; NET-01 re-proved (differential listener rule, ruling T12-scan/net01); P33 stop predicate |
| 13 | `04a079d0` | `SupervisorPort` (D-1 serialization) + FIFO `GenerationGate` (one running slot, 20 ms slice wakeups, Lease destroyed before release) |
| 14 | `2132c197` | Fake engine stdin reader thread + M1 modes (tokens/interval/finish/prefill/stop delays); `_Exit` fix for glibc exit-vs-detached-reader deadlock (T14-exit) |
| 15 | `3c3c8808` | `GEN` bounded sampling keys (T15-signature), cancellable `run_generation` (STOP `client_cancelled`, T15-stopcause), exact §3.5 OpenAI body |
| 16 | `96dd42da` | `ChatRoute` P4→P5→P6; **defect fixed**: gate refused arrivals when `max_queued==0` even with the slot idle (cap now bounds waiters only) |
| 17 | `8f5fbc81` | DOS-02/§2.6/§2.8 acceptance: 5 cases, ALL passed FIRST run (no product change); repeats 15/15 |
| 18 | `23eecf33` | Static egress tripwire (ELF64 + PE32+ import readers; never vacuous without a dynamic section); real server imports nothing forbidden |
| 19 | `d4422808` | LD_PRELOAD interposer + /proc & iphlpapi scans + command-like sweep; **defect fixed**: plan's per-pid scan rule impossible vs the non-dumpable server (ruling T19-scan: listener-differential + exact child inodes); repeats 20/20 |
| 20 | `9e0f582f` | SP2 ext cases re-run under both tripwires (logged NOTHING — no finding); +control-routes-404 and header-switches-inert acceptance (first-run pass) |
| 21 | `f0e17ee3` | `secure_frontend_request_log`: §8 fixed-field lines, token bucket (100 burst / 20 s), stderr startup summary; RED = missing-header compile; nothing-secret sweep green |
| 22 | `9b53c1bf` | 3 Winsock-parity cases (Linux manual exit=0 first run; registered on Windows, windows-runtime label) + non-blocking `linux-clang-tsan` job (T22-tsan: definition-only per Actions-disable ruling) |
| 23 | `11f22e47` | Docs truth pass: handoff M1 status + A4 + D-1/D-2 + Deferred-to-M2 + security-behavior rewrite; threat-model B12/R4/R5/R8; engine-protocol-limits "SP3 M1"; README routes/§7.2/headers/limits/Constants; CI doc. Ruling T23-grep: the plan's two "expect empty" greps do NOT hold on the tree (1 legacy renderer kept per the plan's own Task 16 rule + its pinned test; 8 chrono literals in untouched SP1/SP2 supervision files) — recorded honestly, no code change |

## Final battery (Task 23)
- `ctest --preset linux-gcc`: **100% tests passed out of 690** (686 passed, 4 allowed env skips:
  `cfg03_rejects_mount_crossing`, `proc03_other_uid_cannot_open_engine_fd`,
  `proc05_cgroup_kill_when_delegated`, `bundle_at_mount_root_is_read_but_never_written`);
  §9.3 case-family grep = 98; `Total Tests: 690`.
- `windows-llvm-mingw-cross` **clean rebuild, EXIT=0, 0 warnings** (`-Werror`, compile-only).
- pytest 25 passed. README config-table pin green (36/36 typed_config).
- NOT RUN per owner ruling 2026-10-20: wine, MSYS2, GitHub Actions (the cross ctest preset hides
  `windows-runtime` — 308 such cases pending windows-msys2-clang on owner hardware); TSan not installed
  on this host ("not run on this host (controller follow-up)" — the `linux-clang-tsan` job is the vehicle).

## Process honesty
One interim "green" claim in Task 19 was measured against an un-wired build (session truncation lost a
CMake edit); the next full-suite run exposed it, it was fixed and recorded in `d4422808` and the ledger.
Henceforth every gate number was taken from a rerun after the last edit. Whole-branch review
(`8520deee..HEAD`) found no further defect: lifetimes/order in `run()` verified, tripwire targets inside
the test-only block, trailers exact (12× `Co-Authored-By: Qwen 3.8 Flash Next IQ3_S`, no
`Claude-Session:` per owner ruling), zero out-of-scope files.

## Awaits the owner
1. `windows-msys2-clang` run on owner hardware (308 windows-runtime cases incl. Task 19/20/22 additions).
2. GPU-box items (SP3 §9.5): R4 STOP-during-prefill measurement, M2 kit (`strata-api-probe`, validation doc — deferred to M2).
3. Controller follow-up: the non-blocking TSan job (once CI posture allows).
4. Merge decision per the merge gate — nothing was pushed, merged or branched.
