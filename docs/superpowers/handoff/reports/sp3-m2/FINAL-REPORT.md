# SP3 M2 (streaming) — FINAL REPORT

Lane: `sp3-api-streaming`, worktree `/home/opencode/projects/multi-strata/.worktrees/sp3`, base `f01ef654` (main's docs-only advances intentionally NOT merged — the lane keeps its base per controller instruction).
Status: **all 17 plan tasks complete**. 17 commits, one per task: `5d086223, 66e6cc6d, 0900173e, d4c34a93, 3c121cc2, df2d0fd3, e7d5292f, cd8a2d61, 10fe2d94, 1284633e, dee60cd3, dc2cdc65, ca3f2732, 56c2254d, 89cfd4c3, 54f70516, 847b5b9e` (HEAD).

## What was built (task → deliverable, one line each)

1. baseline reproduced + every M1 interface grep verified (3 grep-string drifts, interfaces exact);
2. fake-engine M2 modes (`--progress-records`, `--crash-mid-gen`, `--err-mid-gen`, `--burst-tokens`, STOP-aware sleeps) + byte-pack long token id 258;
3. `Utf8StreamDecoder` + `TokenByteDecoder` (hold-split-release, B3 U+FFFD);
4. `next_event`/`read_generation` — incremental protocol reads;
5. `drive_generation` (the one loop; M1's `run_generation` rebuilt on it; timeout → 200 `length` (B4); `max_generated_bytes` code-point cap; STOP causes);
6. SSE encoder (role/content/final/usage/`[DONE]`, error events, 16 KiB code-point deltas, 128 KiB event cap);
7. `StreamSocket`/`AcceptedSocketStream` + `ChunkedStreamSink` (non-blocking, write-stall rule) + `StreamSink::set_log_code`;
8. `run_chat_stream` pump (coalescing, lease released at DONE, tail under the response-write deadline);
9. loopback client: `LoopbackAddress` (remote is a compile error), `ChunkedSseReader` (framing + cap), `LoopbackStreamConnection` — the only `connect()` in the frontend (§6.1);
10. `stream: true` end to end: schema (C3), `ChatRoute` stream branch, `LeaseHold` (§2.5 order, R-H), `deliver_route_result` on the non-blocking sink, `stream_client.hpp` helpers;
11. cancellation: `client_gone`/`write_stall` arms, cut streams write nothing more, §8 log path (`chat_stream` route, fixed codes);
12. keepalive comments (never split/queued ahead of pending content);
13. in-stream error events + `x-should-retry` (Task 10's LeaseHold carries report/restart);
14. shutdown during a stream bounded by `kShutdownDrainBudget` (R-H);
15. `run_target_from_server_json` — loopback-only, fixed messages, writer contract stated;
16. `strata-api-probe` (7 modes, test-only, loopback) + `docs/validation/sp3-api-kit.md`;
17. docs truth pass (limits doc, threat model, README, handoff close-out).

## Gates (final)

- `ctest --preset linux-gcc`: **100% tests passed out of 746** (742 run + the 4 allowed environment skips, 0 failed). N₀ 690 → 746 = +56. Counting note: ctest's own line already includes the skips; per-task deltas in the handoff table.
- `windows-llvm-mingw-cross`: EXIT=0, **0 warnings**, every task. wine: **not run on this host** (owner ruling 2026-10-20); Windows runtime claims wait for the owner's MSYS2 hardware; last CI-authorized proof is `windows-msys2-clang` at `7bddb9e1`.
- TSan spot runs (Tasks 5, 10, 11, 12, 14): largest 76/76 + shutdown/disconnect e2e 2/2 under TSan — **zero reports**.
- pytest 25, `secure_distribution_check` PASS (re-verified at the Task 1 baseline; lane changes are C++/docs only).

## Rulings (full text in the lane ledger, `.superpowers/sdd/2026-10-14-sp3-m2-streaming/progress.md`)

- t1-grep/t1-host: grep drift ≠ missing interfaces; wine lines are host rulings.
- t4/t5-test: plan test helpers were M1-incompatible in signature only (`EngineProtocolClient` non-copyable, absolute write deadlines); assertions verbatim.
- t4-preserve, t4-done-strict, t5-stopcause, t5-begin-deadline: RESUME/REUSED surfacing; two-profile PP block; DONE strictness; `client_cancelled` STOP cause; GEN keeps its write deadline.
- t8-unused: observer carries only members it reads (clang `-Wunused-private-field` + `-Werror`); settings arrive with Task 11/12/14.
- t9-nsdmi: NSDMI added to two `ChatRequestOptions` members (GCC `-Werror=missing-field-initializers` on the plan's designated init); tests untouched.
- **t10-coalesce / t11-age** (the one design change): the plan's per-tick partial delta flush made DOS-02's ≤80-event bound flaky on Linux (81–83); mid-stream ticks now send full 16 KiB deltas, partials after a 500 ms hold — reading clients get timely chunks (all cancel tests deterministic), never-reading clients see full deltas only. Tests never changed.
- t10-schema-pin: the plan missed M1's `chat_schema_tests.rejects_unsupported_and_unsafe_features` pinning `stream:true` rejection; its entry replaced by two still-invalid stream forms (same category as the plan's own dos01 amendment).
- t16-order: `secure_frontend_test_paths` used before definition; block relocated; probe target created first.

## Parked for the owner

- Pre-existing SP2 flake: `api_server_tests.log_line_format_and_rate_limit` (unexpected pool `evicted` line) — reproduces 3/10 on the pristine baseline `f01ef654`; untouched in-lane (evidence in ledger t12-flake).
- Windows runtime for all `windows-runtime`-labeled cases; `dos02_slow_stream_reader_hits_write_stall` + `stream_cut_has_no_terminal_chunk` named at risk on Winsock buffer volume (Task 11).
- GPU kit run (`docs/validation/sp3-api-kit.md`) on the owner's boxes; `server.json` writer must emit `host` (PX/SP2 follow-up); SP2 second-run D-1/D-2 unchanged.
- The lane was never pushed (agents never push); `GitHub is storage` — push to save when the owner says so. Base `f01ef654` kept; merging onto current main is the merge gate's decision.

Nothing is claimed for a GPU or for Windows runtime beyond CI logs and the local cross-build.
