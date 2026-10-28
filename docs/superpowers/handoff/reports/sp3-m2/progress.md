# SP3 M2 (streaming) — lane ledger

Branch `sp3-api-streaming` recreated on merged main at `f01ef654` (M1 merge `b7fb97c9`).
Plan: `docs/superpowers/plans/2026-10-14-sp3-m2-streaming.md` (Tasks 1–17).
M1 context: `docs/superpowers/handoff/reports/sp3-m1/FINAL-REPORT.md` + `progress.md` (37 rulings).

## Status

- [x] Task 1 (verify M1 interfaces on the branch; baseline) — COMPLETE at HEAD f01ef654+1
- [x] Task 2 (fake engine M2 modes, STOP-aware sleeps, long token) — COMPLETE
- [x] Task 3 (Utf8StreamDecoder + TokenByteDecoder) — COMPLETE
- [x] Task 4 (EngineProtocolClient::next_event; read_generation rebuilt) — COMPLETE
- [x] Task 5 (drive_generation; run_generation on it; timeout→200 length) — COMPLETE
- [x] Task 6 (SSE encoder: events, deltas, cap, error event) — COMPLETE
- [x] Task 7 (StreamSocket seam, ChunkedStreamSink, set_log_code) — COMPLETE
- [x] Task 8 (run_chat_stream pump) — COMPLETE
- [x] Task 9 (loopback client library) — COMPLETE
- [x] Task 10 (wire stream:true; LeaseHold; deliver_route_result) — COMPLETE
- [x] Task 11 (cancellation: client_gone/write_stall arms) — COMPLETE
- [x] Task 12 (keepalive comments) — COMPLETE
- [x] Task 13 (in-stream error events) — COMPLETE
- [x] Task 14 (shutdown during stream) — COMPLETE
- [x] Task 15 (run_target_from_server_json) — COMPLETE
- [x] Task 16 (strata-api-probe + sp3-api-kit.md) — COMPLETE
- [x] Task 17 (docs truth) — COMPLETE — ALL 17 TASKS DONE

## Rulings (this lane)

Ruling (T1-grep): the plan's Task-1 Step-2 grep list reports 3 MISS strings but every consumed interface exists exactly as M2 needs it — (1) `const ApiErrorRow& api_error_row(ApiError error) noexcept` (named parameter and noexcept break the literal), (2) `void report_fatal(const Lease& lease, KillReason reason)` in `generation_gate.hpp` (named parameter), (3) `refuse(ApiError::unsupported_parameter, "stream")` in `chat_schema.cpp` (M1's helper name is `refuse`, not `fail`). Verified semantically; Task 10 replaces (3) anyway — why: spec/plan intent is the interface, not the literal; the plan's own rule is to check M1 names "as M1 built them" — cost if wrong: none.
Ruling (T1-host): wine W₀ line = "not run on this host (owner ruling 2026-10-20)" in every handoff row; the cross build (0 warnings, compile-only) is the Windows gate. TSan spot-runs allowed and expected on race/cancellation tasks.
Ruling (method): this lane runs implementer + adversarial self-review sequentially (no nested workers); briefs written from the plan text by hand (scripts/task-brief absent); every task: RED line in ledger BEFORE GREEN; one commit per task (+fix rounds); ledger updated at every boundary.

## Task log

Task 1: BASE f01ef654 (branch recreated on M1 merge b7fb97c9 + toolchain doc commit).
 - Base check: clean; branch sp3-api-streaming; `git merge-base --is-ancestor main HEAD` true; M1 commits visible.
 - Interface greps: 74/77 literal OK, 3 MISS resolved by ruling T1-grep; HttpLimits members write_stall/response_write_deadline/stream_keepalive present; `limits.chat.default_max_tokens` min 0 (B1); no LoopbackAddress/secure_frontend_api_client in the tree (R-B holds); result 0 after the ruling.
 - Baseline reproduced at f01ef654: ctest --preset linux-gcc 690 run, 100% (694 registered; 4 allowed env skips cfg03_mount_crossing, proc03_other_uid, proc05_cgroup_delegated, bundle_at_mount_root); pytest -q 25; windows-llvm-mingw-cross EXIT=0, 0 warnings. wine NOT RUN (owner ruling 2026-10-20). Matches the controller's merge-gate numbers exactly.
 - Handoff section "SP3 M2 deliverables" appended after the M1 section.
Task 1: complete (one commit; no RED — verification/docs task).
Task 2: BASE 5d086223.
 RED (runtime, fake_engine_stream_modes_selftest on 5d086223 after extending the case): "FAILED: not after the 3 s prefill", "FAILED: not after the 3 s pause", "FAILED: four tokens without a pause", "FAILED: PP then the token", "FAILED: ERR after two tokens: T 7", "FAILED: --crash-mid-gen exits" (M1's prefill/pause are uninterruptible sleep_ms; the new flags are unknown and ignored).
 Implementing per brief (Options+parse, sleep_or_stop, run_generation rewrite, test_tokenizer id 258 + id_for_byte/token_ids_for).
 GREEN: fake_engine_stream_modes_selftest Passed (1.36 s); targeted ctest -R 'fake_engine|engine_supervisor|server|tokenizer' 189/189 100%; full ctest --preset linux-gcc 690 run, 100% (694 registered, the 4 allowed env skips) — unchanged from baseline (no new CTest entry, as planned); windows-llvm-mingw-cross EXIT=0, 0 warnings. wine: not run on this host (owner ruling 2026-10-20).
 Edge note (adversarial review): STOP arriving during the post-last-token pause now yields finish=cancel with all tokens counted (M1 only checked STOP at the loop top); no existing test covered that window — suite green.
Task 2: complete (one commit; RED above).
Task 3: BASE 66e6cc6d.
 RED (compile, after adding tests+CMake+placeholder src; build EXIT=1): "tests/sse_test.cpp:1:10: fatal error: secure_frontend/utf8_stream_decoder.hpp: No such file or directory"; "tests/tokenizer_pack_test.cpp:185:26: error: 'TokenByteDecoder' in namespace 'secure_frontend' does not name a type" (plan expected "'TokenByteDecoder' is not a member of 'secure_frontend'" — same failure, this compiler's wording). Placeholder src/utf8_stream_decoder.cpp needed so CMake configure passes and the RED is compiler-level as the brief expects.
 GREEN: targeted '^secure_frontend_(sse|tokenizer_pack)_tests\.' 11/11 100%; full ctest --preset linux-gcc 695 run, 100% (699 registered, same 4 env skips) = previous + 5 exactly; windows-llvm-mingw-cross EXIT=0, 0 warnings. wine: not run on this host (owner ruling 2026-10-20).
Task 3: complete (one commit; RED above). decode_token_ids keeps old semantics except the limit check moved per-token (bytes.size()+text.size() > cap vs per-byte >= cap); no test pins the old boundary (checked).
Task 4: BASE 0900173e.
 RED (compile): "engine_protocol_client_test.cpp:368:36: error: 'GenerationEvent' in namespace 'secure_frontend' does not name a type" (+377:29 no member next_event, +378:30 TokenEvent is not a member) — plan expected "'GenerationEvent' is not a member", same failure.
 Ruling t4-test: plan's generating_client helper returns EngineProtocolClient by value — impossible (std::atomic member: never copyable/movable); helper inlined per test, begin_generation keeps M1's write_deadline arg. Assertions verbatim.
 Ruling t4-preserve: the brief's next_event sketch omits three things M1's tests pin — (1) RESUME/REUSED consumption (strata flavor) → surfaced as ProgressEvent (invisible, never fails a request; resume_and_reused_progress_is_consumed_in_the_strata_profile), (2) the two-profile PP acceptance block (prefill_progress_is_bounded_in_both_profiles; plan's GLM-only version would reject strata PP), (3) completed_generations_ fetch_add on DONE, never for nonfinite (stop_cause_and_completed_generations). All three carried into next_event verbatim.
 Ruling t4-done-strict: a full-form DONE (>=6 fields) whose finish is not stop|length|cancel|nonfinite is now malformed (protocol closed) — plan-required (next_event_finish_forms... expects "closed"); M1 was lax; no existing test relied on the laxity (full suite green).
 GREEN: targeted engine_protocol_client|chat_execution|engine_supervisor 84/84; full ctest 698 run, 100% (702 registered, same 4 env skips) = previous + 3; windows-llvm-mingw-cross EXIT=0, 0 warnings. wine: not run on this host (owner ruling 2026-10-20).
Task 4: complete (commit d4c34a93; RED above).
Task 5: BASE d4c34a93.
 GREEN: targeted 141/141; full ctest --preset linux-gcc 704 run, 100% (708 registered, same 4 env skips) = previous + 6; TSan spot 10/10 zero reports; cross 0 warnings. wine: not run on this host.
Task 5: complete (commit 3c121cc2; RED above).
Task 6: BASE 3c121cc2.
 RED (compile, after adding tests+CMake+placeholder src): "tests/sse_test.cpp:2:10: fatal error: secure_frontend/sse_encoder.hpp: No such file or directory" (plan's exact line). Note: a stray NUL byte from transcription slipped into sse_test.cpp's escape assertion; repaired (plan and brief are clean; whole-tree control-byte scan clean).
 GREEN: sse_tests 7/7; full ctest 707 run, 100% (711 registered, same 4 env skips) = previous + 3; windows-llvm-mingw-cross EXIT=0, 0 warnings. wine: not run on this host (owner ruling 2026-10-20).
Task 6: complete (commit pending this line; RED above).
Task 7: BASE df2d0fd3.
 RED (compile, tests+CMake+placeholder srcs first): "sse_test.cpp:2:10: fatal error: secure_frontend/chunked_stream_sink.hpp: No such file or directory"; "http_socket_server_test.cpp:844:22: error: 'AcceptedSocketStream' is not a member of 'secure_frontend'".
 GREEN: targeted sse|http_socket_server 41/41 (incl. stream_socket_peek_and_nonblocking_send on real loopback sockets); full ctest 712 run, 100% (716 registered, same 4 env skips) = previous + 5; windows-llvm-mingw-cross EXIT=0, 0 warnings. wine: not run on this host (owner ruling 2026-10-20).
Task 7: complete (commit e7d5292f; RED above).
Task 8: BASE e7d5292f.
 RED (compile): "tests/sse_test.cpp:2:10: fatal error: secure_frontend/chat_stream.hpp: No such file or directory" (plan's exact line). Ruling t8-test: scripted transport write_line / begin_generation adapted to M1's deadline-carrying signatures; assertions verbatim.
 Ruling t8-unused: the plan's StreamObserver stores settings_ and shutting_down_ it never reads — clang -Wunused-private-field + -Werror fails the cross build; observer carries only sink+identity until Tasks 11/12 read them (cross build then confirms: 0 warnings).
 GREEN: sse_tests 17/17; full ctest 718 run, 100% (722 registered, same 4 env skips) = previous + 6; windows-llvm-mingw-cross EXIT=0, 0 warnings. wine: not run on this host (owner ruling 2026-10-20).
Task 8: complete (commit cd8a2d61; RED above).
Task 9: BASE cd8a2d61.
 RED (compile, tests+CMake+placeholder srcs first): "tests/api_client_test.cpp:1:10: fatal error: secure_frontend/api_client.hpp: No such file or directory" (plan's exact line).
 Ruling t9-nsdmi: GCC -Werror=missing-field-initializers fires on the plan's designated inits ({.timeout=5s}) because on_item/extra_headers lack NSDMI; added `= {}` in the header (M1 style), tests untouched.
 GREEN: api_client_tests 3/3; full ctest 721 run, 100% (725 registered, same 4 env skips) = previous + 3; windows-llvm-mingw-cross EXIT=0, 0 warnings. wine: not run on this host (owner ruling 2026-10-20).
Task 9: complete (commit 10fe2d94; RED above).
Task 10: BASE 10fe2d94.
 RED (runtime; build compiles, ext01/auth01 pass already as the plan predicted): parser "FAILED: stream:true is accepted (M2)"; e2e "FAILED: stream with [DONE] and the terminal chunk", "FAILED: a plain 503, never a stream", "FAILED: the slow reader then gets all 1 MiB", "FAILED: EOS: stop,\"stream\":true", "FAILED: timeout is length (decision 4)", "FAILED: no CORS header on an SSE response".
 Ruling t10-coalesce: DOS-02 slow-reader hit "FAILED: coalesced into 16 KiB deltas" (81-83 events) with the plan's per-tick partial flush — Linux's kernel accepts ~130 KiB from a never-reading client, so per-tick small deltas accumulated. flush_ready now emits only FULL 16 KiB deltas mid-stream (§2.7 "held until the sink can take it, coalesced"); partial text flushes only in flush_all (the tail). Delta log: 53x16384 + small; after fix 3/3 repeat passes. Test never changed.
Task 10: complete (commit 1284633e; RED above). Also amended M1 chat_schema_tests.rejects_unsupported_and_unsafe_features (plan oversight; ruling t10-schema-pin): "stream":true entry replaced by two still-invalid forms.
Task 11: BASE 1284633e.
 RED (compile): "concurrency_test.cpp:355:89: error: too many initializers for 'secure_frontend::RequestLogRecord'"; ":357:10: error: 'struct secure_frontend::RequestLogRecord' has no member named 'streamed'". Runtime after impl v1: "FAILED: the client closed after 3 content chunks", "FAILED: the frontend sent STOP to the engine", "FAILED: a cancelled stream logs client_gone" - t10-coalesce starved small streams of mid-stream deltas.
 Ruling t11-age: partial (<16 KiB) deltas go out mid-stream only after kPartialFlushHold=500 ms; full deltas whenever one is ready. Reading clients get timely chunks (Task 11 cancel tests deterministic), never-reading clients see 16 KiB deltas only (DOS-02 <=80-event bound deterministic: 1 MiB crosses 16 KiB in ~10 ms). Reconciles the plan's Task 10 vs Task 11 expectations on this host.
 GREEN: targeted 32/32 (t8+t10 stream regressions included); full ctest 735 run, 100% (739 registered, same 4 env skips) = previous + 7; TSan spot sse|concurrency|api_client|chat_execution|engine_protocol_client 76/76 zero ThreadSanitizer reports; windows-llvm-mingw-cross EXIT=0, 0 warnings. wine: not run on this host (owner ruling 2026-10-20).
Task 11: complete (commit dee60cd3; RED above). Windows risk per plan Step 5 recorded in the handoff row.
Task 12: BASE dee60cd3.
 RED (runtime, both as the plan predicted): "FAILED: comments while no content flows: 0"; "FAILED: at least 2 ': keep-alive' comments during a 2.5 s prefill: 0".
 Ruling t12-flake (not fixed in-lane): the first Task 12 full suite showed api_server_tests.log_line_format_and_rate_limit failing "fixed fields only: req 23 other 0 evicted 0 0 loopback" (an unexpected pool eviction line). Reproduced on THIS host on the pristine baseline f01ef654 throwaway worktree: 3/10 standalone failures (M2 branch: 2/5) — pre-existing SP2 eviction-timing flake in an M1 test, untouched by the M2 serve path (health path byte-identical). Parked for the owner; lane does not touch M1 admission. Baseline worktree retired to /tmp/opencode/retired-flake-baseline + git worktree prune.
 GREEN: targeted 20/20; full ctest 737 run, 100% (741 registered, same 4 env skips) = previous + 2 (re-run after the flake hit: clean); TSan sse_tests 19/19 zero reports; windows-llvm-mingw-cross EXIT=0, 0 warnings. wine: not run on this host (owner ruling 2026-10-20).
Task 12: complete (commit dc2cdc65; RED above).
Task 13: BASE dc2cdc65.
 RED (runtime, plan lines): sse "FAILED: ERR: error event" (+ all four check() arms); e2e "FAILED: text, then engine_error", "FAILED: [DONE] and the terminal chunk follow", "FAILED: engine_unavailable event, ended normally". The non-stream halves of nonfinite/should-retry passed already (M1 Task 16 + Task 5), as the plan predicted.
 GREEN: targeted 24/24; full ctest 742 run, 100% (746 registered, same 4 env skips) = previous + 5 (the pre-existing M1 flake did not recur); windows-llvm-mingw-cross EXIT=0, 0 warnings. wine: not run on this host (owner ruling 2026-10-20).
Task 13: complete (commit ca3f2732; RED above).
Task 14: BASE ca3f2732.
 RED (runtime, plan lines): sse "FAILED: server_shutting_down event", "FAILED: the generation was stopped", "FAILED: log code"; e2e "FAILED: in-stream server_shutting_down".
 GREEN: targeted 22/22; full ctest 744 run, 100% (748 registered, same 4 env skips) = previous + 2; TSan spot (sse/concurrency) 36/36 zero reports AND api_server shutdown/disconnect e2e 2/2 under TSan with zero reports (server binary TSan-built); windows-llvm-mingw-cross EXIT=0, 0 warnings. wine: not run on this host (owner ruling 2026-10-20).
Task 14: complete (commit 56c2254d; RED above).
Task 15: BASE 56c2254d.
 RED (compile): "tests/api_client_test.cpp:3:10: fatal error: secure_frontend/run_target.hpp: No such file or directory" (plan's exact line).
 GREEN: api_client_tests 4/4; full ctest 745 run, 100% (749 registered, same 4 env skips) = previous + 1; windows-llvm-mingw-cross EXIT=0, 0 warnings. wine: not run on this host (owner ruling 2026-10-20).
Task 15: complete (commit 89cfd4c3; RED above).
Task 16: BASE 89cfd4c3.
 RED (configure): "CMake Error at CMakeLists.txt:656 (secure_frontend_test_paths): Unknown CMake command \"secure_frontend_test_paths\"" — no strata-api-probe target, and my block sat before the function (defined at 782). Ruling t16-order: relocated the api_client_tests block after secure_frontend_test_paths + fake-engine with the probe target created first; test also needed support/test_paths.hpp (make_temp_dir/utf8_path live there).
 GREEN: api_client_tests 5/5 (probe passes against the fake engine in all 6 modes); full ctest 746 run, 100% (750 registered, same 4 env skips) = previous + 1; windows-llvm-mingw-cross EXIT=0, 0 warnings (probe built -Werror). wine: not run on this host (owner ruling 2026-10-20).
Task 16: complete (commit 54f70516; RED above).
Task 17: BASE 54f70516. Docs-only task - no RED/GREEN test pair (verification steps instead).
 Step 1: all 32 spec §9.3 M2 case names OK in ctest -N (92 entries across sse/api_server/api_client).
 Steps 2-5: engine-protocol-limits.md (M1 tail claim replaced by an SP3 M2 streaming section + Prompt-form row fixed), threat-model.md (SSE no-terminal-chunk rationale, R6 no-bytes-while-queued, slow-reader bound, R4 = GPU-kit-only), README.md (streaming route description, keys, constants note +kMaxDeltaTextBytes/kMaxSseEventBytes, strata-api-probe test-only; M1 intro sentences updated), AGENT_HANDOFF.md (row 17 + COMPLETE close-out: final lines 746 in scope / 742 run + 4 env skips 100%; counting note: the table's reg. column double-counted the skips; controller notes 2/4/7/8 restated, row 15's note-4 mis-cite corrected to note 8; follow-ups: server.json writer host, SP2 D-1/D-2 unchanged, GPU kit on owner boxes, pre-existing log_line_format_and_rate_limit flake parked).
 GREEN (final run, Step 6): ctest --preset linux-gcc 100% out of 746 (742 run + 4 env skips, 0 failed); windows-llvm-mingw-cross EXIT=0, 0 warnings. wine: not run on this host (owner ruling 2026-10-20).
Task 17: complete (commit 847b5b9e).
- [x] Task 17 (docs truth) — COMPLETE — ALL 17 TASKS DONE
 RED (compile, Step 1): "tests/chat_execution_test.cpp:2:10: fatal error: secure_frontend/generation_run.hpp: No such file or directory" (plan's exact line).
 RED (runtime, Step 2 against untouched M1 path, api_server_tests target only): "FAILED: a max_tokens cut mid-character is a 200, not a decode failure" (M1: internal_error 500 — decode_token_ids rejects the mid-character cut), "FAILED: a timed-out generation is a 200 (decision 4)" (M1: 503 path). Both recorded per the brief.
 Implementing Steps 4-5 (generation_run.hpp/.cpp, run_generation rebuilt, route timeout→200 length, subsystem wiring).
 Ruling t5-stopcause: drive_generation's STOP records EngineStopCause::client_cancelled when cancel==client_gone, else generation_timeout — the SP2 kill-reason contract (KillReason::client_stop_drain_failed follows stop_cause) and M1's cancel path both pin this; the plan's bare stop() call would regress sp3_d2 semantics on the shared loop.
 Ruling t5-begin-deadline: ChatExecutionService::begin keeps M1's GEN write deadline (deadline_after(generation_timeout), ruling T15-signature); the brief's 3-arg begin_generation is M1-incompatible.
 GREEN (pending final full-suite count): targeted chat_execution|api_server|engine_supervisor|server 141/141 (includes 4 new drive/chat cases, amended timeout case, 2 new e2e cases flipped from the RED above); TSan spot run (linux-clang + -fsanitize=thread) on drive_generation/run_generation/chat_recovers cases: 10/10, zero ThreadSanitizer reports; windows-llvm-mingw-cross EXIT=0, 0 warnings.

## Host adaptations (binding, owner rulings 2026-10-20/21)

- No CI anywhere (Actions disabled; GitHub storage only). No wine, no MSYS2.
  Windows gate = local `windows-llvm-mingw-cross` compile+link, 0 warnings.
- TSan IS available on this host (controller-installed): `clang`/`clang++`
  wrappers in `.venv-dev/bin` (see docs/AGENT_HANDOFF.md "Host toolchain
  2026-10-21"); `TSAN_OPTIONS=external_symbolizer_path=<repo>/.venv-dev/bin/llvm-symbolizer`.
  The full TSan gate runs at the controller merge; lane may spot-run TSan builds.
- Commits: author `shift-f10 <40450442+shift-f10@users.noreply.github.com>`, literal
  `Co-Authored-By: <real model> <noreply@anthropic.com>` trailer, NO
  `Claude-Session:` trailer.
- Agents never push/merge/rebase/switch branches; controller pushes to save.
- Linux ctest runs SERIAL (fixed ports 20000–20099). umask 0002: use
  make_scratch_directory/make_private_directory. `scripts/task-brief` does not
  exist on this host — write task briefs by hand into this workspace dir.

## Baseline to reproduce before Task 2 (measured by controller at merge, 2026-10-21)

- ctest --preset linux-gcc: 690 run, 100% (694 registered, 4 allowed env skips:
  proc03_other_uid, proc05_cgroup_delegated, cfg03_mount_crossing, bundle_at_mount_root)
- linux-gcc-release: build clean, preset ctest 10/10, secure_distribution_check PASS
- windows-llvm-mingw-cross: 0 warnings
- pytest -q from worktree root: 25
- root build + ctest: 656 registered, 652 passed + 4 env skips, 0 failed
- TSan (controller): filtered concurrency suites 90/90, zero reports
