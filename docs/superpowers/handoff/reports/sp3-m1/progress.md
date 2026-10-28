# SDD ledger — plan: docs/superpowers/plans/2026-10-14-sp3-m1-concurrency-hardening.md

Spec: docs/superpowers/specs/2026-10-14-sp3-api-concurrency-streaming-design.md (APPROVED; §1.5 D-1/D-2 and §2.6 re-anchored 2026-10-14 — spec wins over plan on cancellation).
Worktree /opt/projects/ai/.worktrees/sp3, branch sp3-api-streaming, launch HEAD 557a2cc7 (verified), submodule 296b59d2 upstream (v0.1.44.1) clean, .venv-dev/.toolchains symlinks present.
MERGE_BASE = 557a2cc7da02f966cc92ed80ee137e9cc27d4364
Baseline (controller-verified): linux-gcc 520/520 (+2 skips), linux-gcc-release 510/510, wine 256/256, pytest 23.

## Task 0 anchor check (controller, 2026-10-14)
All 18 plan Step-1 greps hit on 557a2cc7 (kSchema is `std::array<ConfigKey, 43>`; FrontendConfig ctor now ends `PullSettings pull_value`; README `config-keys:begin` in secure_frontend/README.md).

Ruling: re-anchor 1 (CI YAML) — `upstream.version` is the engine-version source; Task 22's TSan job adds no `-D`; keep both header-comment sentences on any text conflict. Controller note 11 APPROVED: Task 22 edits the YAML locally (no push; nothing triggers Actions from this lane) — why: brief says the TSan job is non-blocking and to run the TSan preset locally — cost if wrong: one revertable YAML hunk.
Ruling: re-anchor 2 (CMakePresets) — SECURE_FRONTEND_TEST_HOOKS is already ON in test presets and `linux-gcc-release` exists; no task re-adds them; the TSan preset inherits from the hooks-ON base — why: fact on main — cost if wrong: none (no edit).
Ruling: re-anchor 3 (frontend_config.hpp) — Task 4's `logging` member + ctor parameter is appended AFTER `pull` (ctor becomes `(..., EngineLaunch, PullSettings, LoggingSettings)`); Task 5's ConcurrencyLimits rewrite replaces the current one-field struct; any SP3 use of `roots.tokenizer_pack` / `engine.profile` must handle `std::nullopt` — why: SP4 shape on main — cost if wrong: merge-time conflict in a ctor line.
Ruling: re-anchor 4 (secure_server_main.cpp) — router config takes `model_id = launch.model_id` (the `const std::string& model_id = launch.model_id;` line); `selected` is never reintroduced; `owned_by = "local"` (the `ModelRecord::owned_by` default; the launch record carries no owned_by). Per spec R3/§9.4 `/v1/models` lists only the served model (launch.model_id), not `launch.served_models`' expert inventory — why: spec is binding and plan says "lists only the served model" — cost if wrong: expert-mode /v1/models shows one id instead of the inventory; one-line change.
Ruling: re-anchor 5 (server harness/fixture) — every SP3 process launch uses what `ServerFixture::base_flags()` / `server_harness.cpp` pass today: `--expert`, `--models-root`, `--tokenizer-pack`, `--engine`, `--state-dir` (0700 owner-private dir); implementers read tests/support before writing launches — cost if wrong: test-only rework.
Ruling: re-anchor 6 — cfg01_* cases in `secure_frontend_typed_config_tests` (plan note 1 already does this).
Ruling: re-anchor 7 (egress shim) — FACT DIFFERS: SP4's `secure_frontend_connect_audit` shim (SP4 Task 28) is NOT on main 557a2cc7; it is being built concurrently in .worktrees/sp4b. Task 19 therefore builds SP3's interposer as the plan specifies (`tests/support/egress_tripwire.cpp` → `libstrata_egress_tripwire.so`), with no name, env var (`SF_CONNECT_AUDIT*`) or CMake target overlapping SP4's, and keeps the static-import tripwire (Task 18). Flag to controller: consolidate the two shims at/after integration — why: cannot reuse code that does not exist in this tree, and must not touch sp4b — cost if wrong: two LD_PRELOAD shims in tests until consolidated.
Ruling: re-anchor 8 — README key table regenerated with the SP1 generator in Tasks 4/5/23, never hand-merged; controller regenerates again at merge.
Ruling: re-anchor 9 — no `strata` binary work in M1; product-spec CLI (`serve`, `version`, `key`) still unowned — flagged in the final report.
Ruling: commit trailers — plan text's `Co-Authored-By: Claude Fable 5.1` is superseded by the common brief: implementer's actual model (`Claude Opus 5.5` / `Claude Sonnet 5.5`), literal lines — why: owner rule — cost if wrong: none.
Ruling: plan note 13 / SP2 R1 — stdin write bound belongs to SP2 (shipped in run 2); M1 does not touch it.
Ruling: SP2 carry-ins bind: cancellation = `stop(EngineStopCause::client_cancelled)` with reason via `kill_reason_for(protocol)`; uncounted restart bound `config_defaults::kMaxConsecutiveUncountedRestarts`; `wait_for_change` observes states; listener + accepted sockets get CLOEXEC / `WSA_FLAG_NO_HANDLE_INHERIT` (Task 8a/8b) and the handoff records the K4 gap closed; write-stall timeout slices sends on BOTH platforms (Task 8a); fix the `engine_supervisor.hpp` destructor "one exception" comment to cover Windows in Task 13 (lease lifecycle) — why: SP2 final report, binding per brief.
Ruling: socket tests touching the server fixture prefer ephemeral port 0 (plan's per-case port table in 8b may be replaced by port 0 where the fixture allows).
Ruling: plan read strategy — plan is 9094 lines; controller read header/constraints/interfaces/Task 0 directly and delegated the full pre-flight conflict scan to a read-only Opus agent (table below); per-task text reaches implementers via task-brief — why: protects controller context over 26 tasks — cost if wrong: a cross-task conflict the scan missed is caught by task review.

## Pre-flight conflict scan
Full table (143 rows: 50 pair, 26 per-task, 17 spec, 39 tree, 11 reviewer; 79 conflicts 23H/33M/23L): preflight-scan.md in this workspace — it is part of this ledger. Implementers read the rows naming their task.

Ruling: ADOPT every proposed ruling in preflight-scan.md §1–§5 as written (P10–P50, T1–T23, S1–S17, V1–V39, D1–D10), with the refinements below — why: each was checked against tree 557a2cc7 and the spec by the scan; spec binds — cost if wrong: per-row costs as stated in the scan.
Ruling (S1/P45/V26, H): cancel uses `stop(EngineStopCause::client_cancelled)`, timeout `stop(EngineStopCause::generation_timeout)`; Task 15 adds the stop_cause assertion; Task 17 (not 16 — it owns disconnect acceptance) adds `api_server_tests.dos02_cancel_drain_failure_is_not_counted` (assert via stderr reason `client_stop_drain_failed` and that a following chat succeeds after restart) — why: spec §2.6/D-2 — cost if wrong: one test moves between tasks.
Ruling (P12/V1/P23, H): the task that changes an API ports every in-tree user of it, including `src/smoke_main.cpp`, `src/smoke_server_main.cpp`, `secure_server_main.cpp` and old tests — Task 3 (HttpPolicy), 6b (ApiRouter), 8b (HttpSocketServer/bind; smoke mains hold a WinsockGuard), 9 (legacy handle_http_request removal). Every task ends with a green full build on both presets — why: build must never be red between tasks — cost if wrong: none beyond scope.
Ruling (P11): Task 3 takes 6a's forward-declaration change (`struct HttpPolicy;` in http_request_handler.hpp, include moved to .cpp).
Ruling (P33/P34/T12, H): adopt both: 8b wires `HttpSocketServer::State::stopped` as `WorkerSignals.shutting_down` in `serve_one`; Task 12's `run_until` exits when the listener handle is invalid, main's stop predicate is `supervisor.stop_requested()`, and `PoolExit::listener_failed` → `supervisor.shutdown(kShutdownBound)` + `return EXIT_FAILURE`.
Ruling (V12, carry-in): CLOEXEC/no-inherit on listener + accepted sockets lands in Task 8b with case `http_socket_server_tests.listener_and_accepted_sockets_are_not_inherited`; handoff records K4 gap closed.
Ruling (T13b/V22): Task 13 also fixes the engine_supervisor.hpp destructor comment (comment-only).
Ruling (T22b): no TSan preset is added (the plan adds `-D CMAKE_*_FLAGS` on linux-clang in the CI job only); local TSan run is NOT possible on this host if no Linux clang with the TSan runtime exists — Task 22 implementer checks (`which clang clang++`) and runs it locally if available, else records "CI-only, not run locally" — why: brief asks local TSan "where the plan says so"; plan defines none locally — cost if wrong: TSan findings surface only on the owner's push.
Ruling (D3/D4): assert-nothing traffic in net01_workers_add_no_listener and run_lifecycle gets real assertions as proposed.
Ruling (D10): new constants kMaxModelFieldBytes, kMaxPollEntries, kLogLineMaxBytes go in api_constants.hpp in the task that first needs them; use kContextMarginTokens in Task 5.
Ruling (T9a, T19c): RED lines may be "acceptance, passed before implementation" where earlier tasks already deliver the behavior; record honestly in the commit body.


Note (coordinator, 2026-10-14): CI run 38625663621 on base 557a2cc7 all green — MSYS2 real Windows 488/488, linux-gcc-release green. Every windows-runtime row on the base is verified on real Windows; cite run 38625663621 in Task 23 (and any handoff edit) for "pending MSYS2" rows M1 does not change. New M1 windows-runtime rows stay "pending MSYS2".

## Task log
Task 0: fix round 1/5 (1 addressed, 0 open — SP3 block split SP2 closing paragraph; commits cf6df6dd..7f682eed)
Task 0: complete (commits 557a2cc7..7f682eed, review clean) — N0 = 520 (+2 skips), W0 = 256
Task 1: BASE 7f682eed, dispatched (opus)
Task 1: ⚠️(a) resolved by controller — chat_schema.cpp already uses SP1's shared append_json_string (no copy left); ⚠️(b) carried to Task 6a dispatch (P3: delete local ascii_iequal in http_wire.cpp).
Task 1: minor (deferred): http_parser_test.cpp:111 guard `wire.size() >= 3U` should be `>= 7U` before substr(size-7).
Task 1: minor (deferred): §3.9 "UTF-8 copied unchanged after validation" — router model fields are not UTF-8-validated; carried to Task 6b dispatch (validate in the router rewrite).
Task 1: minor (deferred): http_wire.hpp:9 comment says six security headers, kSecurityHeaders holds five (+Connection).
Task 1: complete (commits 7f682eed..8aea2175, review clean) — linux-gcc 524/524, wine 260/260
Task 2: BASE 8aea2175, dispatched (opus)
Ruling (Task 2 concern): the implementer moved SP4's `secure_frontend::ascii_lower` out of acquire_manifest.{hpp,cpp} into a new ascii_text.hpp. Reverse it: SP4-lane files (src/acquire*, include/secure_frontend/acquire*) are owned by the concurrent sp4b lane; SP3 must not edit them. SP3's export is renamed `ascii_to_lower` (http_request_head.hpp) so no symbol collides; ascii_text.hpp is removed — why: brief assigns acquire* to SP4; a two-line duplicate is cheaper than a cross-lane merge conflict — cost if wrong: one small duplicated helper; controller may consolidate after both lanes merge.
Task 2: ⚠️ resolved — (a) trailers verified Claude Opus 5.5 on 8aea2175/64d96fee/0cb16cd2; (b) Task 3 brief pins single-Host → 400 (policy test "duplicate Host is 400"); (c) see ruling.
Ruling (Task 2 two commits): keep 64d96fee + 0cb16cd2 as-is (no rebase/squash allowed in this lane); the pair's net diff touches no SP4 file — integrator may squash when merging if desired — cost if wrong: one intermediate commit touches acquire_manifest.* in history only.
Task 2: minor (deferred): http_request_head.cpp Content-Length accumulates 10 digits in size_t — wraps on 32-bit targets (both presets 64-bit).
Task 2: minor (deferred): D6 helper assertions folded into head_request_line_and_header_syntax (mislabel risk).
Task 2: complete (commits 8aea2175..0cb16cd2, review clean) — linux-gcc 530/530, wine 266/266
Task 3: BASE 0cb16cd2, dispatched (opus)
Task 3: ⚠️ resolved — commit body carries RED, linux-gcc and wine lines; trailer Opus 5.5.
Task 3: minor (deferred): tests/http_request_handler_test.cpp policy(bool) ignores its argument (dead param) — remove when 6a/6b port these cases.
Task 3: minor (deferred): is_valid_host_alias/is_valid_cors_origin are noexcept but call non-noexcept is_valid_host_header.
Task 3: minor (deferred → carried to Task 4): mains pass their old bind authority as a host alias (keeps non-spec 0.0.0.0:<port> for wildcard binds); Task 4 wires listener.host_aliases/cors_origins into make_http_policy in the mains and drops that alias.
Task 3: complete (commits 0cb16cd2..211f8287, review clean) — linux-gcc 530/530 (5 policy cases replaced 1:1), wine 266/266
Task 4: BASE 211f8287, dispatched (opus)
Ruling (Task 4 concern 1): accept that an SP1 config setting request_deadline_ms < 10000 without header_deadline_ms, or a wildcard bind without listener.host_aliases, is now rejected — why: spec §5.1 table states both cross-field rules explicitly and §9.3 names cfg01_header_deadline_le_request_deadline / cfg01_wildcard_bind_requires_host_aliases; the "every SP1 config stays valid" sentence (§5.1 l.787) is argued only for the concurrency keys — cost if wrong: such configs need one extra key; handoff A1 row records it; flag to owner in final report.
Ruling (Task 4 concern 2): SP1 test edits (subsystem_limits header_deadline_ms=1500; auth04 allow-list gains logging.requests as a non-auth switch) accepted as the behavior-change-in-same-task rule requires — reviewer checks auth04 still forbids any auth-disabling switch.
Ruling (Task 4 concern 4): kSchema rows placed at the brief's grouped positions (listener after backlog, http after max_request_line_bytes, logging.requests last) rather than all after pull.* — why: brief is explicit; hunks are away from SP4's pull.* rows except logging.requests — cost if wrong: one merge hunk to resolve.
Task 4: ⚠️ resolved — only one HttpLimits{/ListenerConfig{ construction site exists (frontend_config.cpp validate), updated.
Task 4: minor (deferred): smoke_server_main wildcard bind still passes "0.0.0.0:<port>" as alias (smoke binary, no typed config) — record or reject wildcard in that binary.
Task 4: minor (deferred): wildcard host without allow_wildcard_bind reports both listener.host and host_aliases issues (noise).
Task 4: minor (deferred): host_aliases duplicate check is byte-exact (Box.lan:1 vs box.lan:1 both validate; make_http_policy dedupes).
Task 4: complete (commits 211f8287..5134f4a6, review clean) — linux-gcc 536/536, wine 272/272, pytest 23
Task 5: BASE 5134f4a6, dispatched (opus)
Task 5: note — README intro 'serial inference listener' left for Task 12/23 truth pass (carry to Task 23).
Task 5: ⚠️ resolved — frontend_init.cpp:86 still writes only flags + schema_version.
Task 5: minor (deferred): no execute()-level test of omitted max_tokens → auto (only resolve_max_tokens unit test).
Task 5: minor (deferred): max_connections_per_remote_address row default_integer 0 below its min 1 (masked by derived default; add comment).
Task 5: minor (deferred): chat_applies_engine_context_rule_with_eight_token_margin name/comment describe the old rule.
Task 5: complete (commits 5134f4a6..e5fd2cad, review clean) — linux-gcc 544/544, wine 280/280, pytest 23
Task 6a: BASE e5fd2cad, dispatched (opus)
Harness forced handback after Task 5 (interim FINAL-REPORT). Coordinator: resume; rulings on re-anchor 7 and owner-visible behaviour changes ACKNOWLEDGED/ACCEPTED — handoff + README key table must state them (carry to Task 23).
Task 6a: implementer committed 9cc726cd (hand-back routed to coordinator; report file present). BASE e5fd2cad.
Ruling (Task 6a concern): Origin before preflight in the legacy adapter stands — spec §4.1 order is step 3 Host+Origin policy, step 4 CORS preflight; Task 3's shim order was the deviation — cost if wrong: none observable (unknown-origin preflight 403 either way).
Ruling (Task 6a §9.4 status changes): TE → 411 (was 400) in http_request_handler_tests.rejects_malformed_and_ambiguous_messages and CL over bound → 413 (was 400) in http_socket_server_tests.parser_honors_configured_max_request_bytes accepted — spec §4.2 and §9.4 require them, and the "update tests in the task that changes behavior" rule applies — cost if wrong: two assertions.
Task 6a: ⚠️ carried — D7 deletion of DispatcherRouter + legacy overload checked at Task 9; kNoResponseStatus=0 handling carried to Task 8b dispatch.
Task 6a: minor (deferred): seam tests miss short-body (CL 2, 1 byte) and "handle not called when decision.early".
Task 6a: minor (deferred): no test that serialize_head drops Server/X-Powered-By/caller Date.
Task 6a: minor (deferred): legacy path no longer has GET/POST/OPTIONS allowlist (correct per §4.1 step 6) but untested there.
Task 6a: complete (commits e5fd2cad..9cc726cd, review clean) — linux-gcc 546/546, wine 282/282
Task 6b: BASE 9cc726cd, dispatched (opus)
Task 6b: note — wine flake once in SP4's fetch_tests.status_table_decisions (passed on rerun 3/3); watch list.
Ruling (Task 6b SP1 edit): Task 6b may make one SP1 edit — exporting `is_valid_utf8` (noexcept wrapper over json.cpp's existing valid_utf8) from json.hpp for §3.9 — recorded as SP1-addendum row "A6b" (A3 stays reserved for Task 7's JSON decimals per plan note 2) — why: avoids a sixth private UTF-8 copy; behaviour-neutral — cost if wrong: one exported function in SP1.
Ruling (Task 6b deviation 1): /health keeps SP2's literal allowlist; the fail-closed body for an unknown literal is §7.2 engine_unavailable — accepted (production snapshots cannot reach it; brief sets this body for a throwing provider); noted for final review.
Task 6b: fix round 1/5 (2 addressed, 0 open — A6b addendum row; RecordingRouter copy/move deleted; commits a49ff6b2..47a97a71)
Task 6b: minor (deferred): transitional public `handle_authenticated_get` bridge (api_router.hpp) not in shared-interfaces — Task 8b/9 must delete it; prefix/exact match duplicated with authorize.
Task 6b: minor (deferred): smoke mains serve only first inventory model; empty inventory → /v1/models 500 (smoke binaries untested; Task 8b takes them over per P23).
Task 6b: minor (deferred): secure_server_main builds a second HttpPolicy unused on the serial path until Task 9.
Task 6b: complete (commits 9cc726cd..47a97a71, review clean after round 1) — linux-gcc 549/549, wine 285/285
Task 7: BASE 47a97a71, dispatched (opus)
Ruling (Task 7 depth vs content arrays): keep kChatMaxJsonDepth = 4 everywhere (no per-path depth exception). OpenAI part arrays (`content:[{"type":"text",...}]`, depth 5) answer 400 invalid_json; shallow array content (`["Hi"]`, `[]`) answers 400 unsupported_parameter `content` — why: spec §3.1 l.378-379 fixes depth ≤ 4 with "deepest legal value is messages[i].content at depth 3" and §5.3 maps depth to invalid_json; both outcomes are 400 with no engine action — cost if wrong: clients sending part arrays see invalid_json instead of unsupported_parameter until SP5; flag to owner.
Ruling (EXT-01 under the depth limit, carried to Tasks 9/20): realistic deep `tools`/`functions` bodies may answer 400 invalid_json rather than extension_disabled; EXT-01's binding property is "spawn nothing, 400". ext01 cases assert extension_disabled with `param` for shallow forms (e.g. `"tools":[]`, `"tool_choice":"auto"`, `"functions":[]`, role `tool`) and 400 + nothing spawned for deep forms — cost if wrong: test expectation adjustments in 9/20; flag to owner.
Task 7: ⚠️ carried — ChatRequestError→ErrorOptions{.param} wire mapping verified at Task 9/16.
Task 7: minor (deferred → carried to Task 15): temperature -0.0 stored as -0.0 → GEN would render temperature=-0; normalize zero.
Task 7: minor (deferred): decimal underflow/overflow reported as integer_out_of_range (fail-closed; misleading name).
Task 7: minor (deferred): extension-precedence test lacks an invalid member BEFORE the extension key.
Task 7: minor (deferred → M2): stream_options value not type-checked.
Task 7: minor (deferred): handoff A3 row Task column style "7" vs "Task 6b".
Task 7: complete (commits 47a97a71..c2d51628, review clean) — linux-gcc 556/556, wine 292/292
Task 8a: BASE c2d51628, dispatched (opus)
Task 8a: incident — implementer ran one read-only git log/branch in .worktrees/sp4b (its inherited cwd) before reading the rules; nothing changed there. Mitigation: every dispatch now states the inherited cwd is NOT the worktree. Report to coordinator.
Ruling (Task 8a set_no_inherit extra): keep — it is the V12 helper Task 8b uses; binding test + K4 note remain 8b's.
Coordinator FYI: main c9629e6e changed engine_protocol_client.cpp read_generation PP branch (Strata accepts bounded 3–5-field PP; test renamed prefill_progress_is_bounded_in_both_profiles). Carry to Task 15: do not alter the PP branch/case semantics; if touched, preserve Strata PP acceptance.
Task 8a: fix round 1/5 (4 addressed, 0 open — progress-based stall test; stderr line; stall<=0 doc; EINTR drain; commits 53ed7f73..701a725d)
Task 8a: complete (commits c2d51628..701a725d, review clean after round 1) — linux-gcc 564/564, wine 300/300
Task 8b: BASE 701a725d, dispatched (opus)
Harness forced handback #2 during Task 8b (implementer a40a043c… still running its own build; BASE 701a725d). Resume: wait for 8b, review from 701a725d.
Task 8b: note — wine start-up flakes (0.01 s failures before test code: json_tests.json_accepts_strict_document, http_socket_server_tests.oversize_header_with_431) passed on rerun; watch list with SP4 fetch flake.
Task 8b: ⚠️ resolved — test main keeps install_process_defaults() + WinsockGuard (checked by controller); Task 9 must delete handle_authenticated_get + DispatcherRouter/make_dispatcher_router (carried); Windows rows pending MSYS2.
Task 8b: minor (deferred → carried to Task 12): an in-flight response write is not cut by shutdown (only writes starting after shutdown use kShutdownDrainBudget); a stalled reader can hold main up to write_stall (10 s) > SP2's 5 s bound.
Task 8b: minor (deferred → carried to Task 12): POSIX close() doesn't wake an in-progress poll — shutdown(fd, SHUT_RDWR) before close restores immediate wake.
Task 8b: minor (deferred): secure_server_main never checks WinsockGuard::ok().
Task 8b: minor (deferred → M2): placeholder SocketStreamSink writes chunked framing regardless of the head's Transfer-Encoding.
Task 8b: minor (deferred): legacy handle_http_request copies HttpPolicy + 2 std::functions per call (transitional until Task 9).
Task 8b: complete (commits 701a725d..63b7ffe2, review clean) — linux-gcc 571/571, wine 307/307 (two 0.01 s start-up flakes passed on rerun)
Task 9: BASE 63b7ffe2, dispatched (opus)
Task 9: fix round 1/5 (4 addressed, 0 open — ext01 asserts extension_disabled for reachable bodies + depth-5 400; dup link; dead fwd decls; record_of comment; commits 7e441e27..29804f11). Residual: task-9-report.md line 12 stale wording (workspace scratch, not shipped) — minor (deferred).
Task 9: notes — wrong model id → 404 model_not_found (spec §3.1/§7.2); proc05_sigterm_during_generation_exits_cleanly also accepts 503 server_shutting_down (spec §3.10); S11 direct supervisor calls remain until Task 16.
Task 9: minor (deferred): api_server_test record_of/count_of [[maybe_unused]] until later tasks.
Task 9: complete (commits 63b7ffe2..29804f11, review clean after round 1) — linux-gcc 580/580, wine 307/307 (9 api_server cases windows-runtime → pending MSYS2)
Task 10: BASE 29804f11, dispatched (opus)
Coordinator FYI: main bd3b53a6 has SP4 part 2 (connect-audit shim secure_frontend_connect_audit, privacy tests). Task 19 keeps non-overlapping names; coordinator consolidates after merge. No rebase.
Task 10: minor (deferred): has_no_cors_headers checks only ACAO/ACAC, not every Access-Control-* header.
Task 10: minor (deferred): "no preflight reached a route" check is near-vacuous.
Task 10: minor (deferred): WEB-04 matrix omits 408 (spec says reachable over TCP), 500/503.
Task 10: minor (deferred): auth01 repeats route_request prefix/target logic; stderr diagnostic only when ok empty.
Task 10: complete (commits 29804f11..f7a150d0, review clean) — linux-gcc 591/591, wine 307/307
Task 11: BASE f7a150d0, dispatched (opus)
Task 11: review — Important: linger overflow test can't tell oldest from any; fix round 1 dispatched (resume implementer).
Task 11: ⚠️ carried to Task 12: every admit/header_complete/finished under the pool mutex; evicted worker's abort flag seen within 20 ms; listener + linger poll entries ≤ kMaxPollEntries.
Harness forced handback #3 during Task 11 fix round 1 (implementer a8579bf3… running; FIX_BASE 46ad217f).
Task 11: fix round 1/5 (5 addressed, 0 open — linger overflow pins natives[1..64]; mutex comment; remote eviction count; EOF case; constants; commits 46ad217f..9d30088b)
Task 11: complete (commits f7a150d0..9d30088b, review clean after round 1) — linux-gcc 596/596, wine 312/312
Task 12: BASE 9d30088b, dispatched (opus)
OWNER CLOSE-OUT: stopped at Task 11 (complete, 9d30088b). Task 12 implementer was dispatched just before the instruction; told to stop — it made no edits, no commit (tree clean). Task 12 NOT started.

## Re-anchor 2026-10-20 (new sub-controller, host adaptations)
Re-anchored at 8520deee (controller merge of main: upstream v0.2.1.3 cbfd5b92 + RESUME/REUSED hotfix). Baseline (controller-verified at 8520deee): linux-gcc 642 registered/638 passed/0 failed + 4 allowed env skips (proc03, proc05_cgroup_kill_when_delegated, cfg03_rejects_mount_crossing, bundle_at_mount_root — container restrictions), pytest 25, cross build 0 warnings.
Ruling (host): no CI, no wine, no clang/TSan on this host (owner ruling 2026-10-20): Windows gate = windows-llvm-mingw-cross compile (-Werror); runtime proof deferred to owner hardware; TSan rows "not run on this host (controller follow-up)". Commit trailers: Co-Authored-By only, NO Claude-Session (owner ruling overrides brief text). No nested-worker harness here: I run implementer + adversarial reviewer sequentially myself, RED/GREEN logged per task.
Carry-in: Task 15 must not disturb the RESUME/REUSED branch in engine_protocol_client.cpp (brought by the re-anchor); build run_generation on top of it. kEngineVersion now 0.2.1.3 (tag-governed).

## Task 12 (ConnectionPool) — 2026-10-20, commits 8520deee..66b13d88
Brief regenerated: task-12-brief.md (scripts/task-brief absent on this host).
RED (runtime, on 8520deee before implementation): dos01_eviction_over_tcp_admits_new_client "FAILED: a ninth client completes while eight workers are held in P1"; dos01_slowloris_cannot_starve_a_real_client "FAILED: a real chat completes under 16 one-byte-per-second header clients"; dos01_slow_body_clients_cannot_starve_a_real_client PASSED before implementation (Task 9's auth-before-body already delivers it — honest note per T9a). Carry-in test RED by mutation (drain-bound disabled): send_all_bounded_stop_flag... "FAILED: the stop flag cuts the write at the drain budget" (10.36 s stall) — restored, GREEN 2.34 s.
GREEN: all 14 touched cases; targeted set api_server|server|concurrency|http_socket_server 79/79 (SP2 proc05_* shutdown cases stay green on the pool); full ctest 644 passed / 0 failed / 4 env skips (648 registered); cross build 0 warnings; pytest 25.
Proofs: (a) admit/header_complete/finished under the single pool mutex — static: run_until admit under unique_lock; header_complete only inside the on_header_complete lambda's lock_guard; finished under lock after re-lock; AdmissionState/LingerList docs updated; TSan = controller follow-up (not run on this host). (b) evicted worker sees abort within one 20 ms slice: serve_connection checks signals.abort every kStreamPollSlice; pinned by dos01_eviction_over_tcp (wait_closed 100 ms). (c) listener+linger in one poll_sockets ≤ kMaxPollEntries: static_assert(1+kLingerListMax ≤ kMaxPollEntries) in connection_pool.cpp, entries.reserve(1+kLingerListMax).
Ruling (T12 file set): Task 12 also touches socket_io.{hpp,cpp}, connection_handler.cpp, http_socket_server.cpp, http_socket_server_test.cpp — why: the two Task 8b deferred minors carried into Task 12 (in-flight write ≤ kShutdownDrainBudget; POSIX shutdown(SHUT_RDWR) before listener close) are binding per FINAL-REPORT resume §4 — cost if wrong: none (behavior additive, defaults keep old call sites).
Ruling (T12-scan): the plan's connection_scan Linux scan (per-pid fd inodes) cannot see the server: strata-secure-server sets PR_SET_DUMPABLE(0) (SP0), so /proc/<pid>/fd is EACCES even for the same uid. Fallback: when the fd scan yields nothing, attribute /proc/<pid>/net (or /proc/net) rows by uid == the test's uid (server shares uid+netns; suites serial). Dumpable pids (fake engine, Task 19) keep exact inode mapping. Cost if wrong: another same-uid process's sockets join the scan; NET-01 asserted as a listener-SET DIFFERENTIAL (see T12-net01) so ambient listeners cannot false-fail; Task 19's scan inherits the same ruling.
Ruling (T12-net01): net01/net01_ipv6 assert the listening-socket SET unchanged before/after the traffic + the server's own listener present/loopback (the dev container has ambient same-uid listeners, e.g. 0.0.0.0:80 — an absolute "exactly one listener" cannot hold here). Why: NET-01's property is "workers add no listener" — cost if wrong: an ambient process changing listeners mid-case = flake (suites serial, minutes-long window tiny).
Ruling (T12-net01b): the plan's net01 traffic expectation (all 10 GETs 200) contradicts spec §2.3 under 20 near-simultaneous clients: 10 chats take all 8 workers past P1 (single lease until Task 16), so later clients — GETs included — are legitimately refused 503 at accept, and late arrivals can evict a not-yet-authenticated connection (closed without response = nullopt exchange). Asserted instead: every status ∈ {200,503}, no hangs, ≥1 200, sequential /health 200 after the burst, listeners unchanged (12/12 repeat clean; distribution logged by the case). Why: spec §2.3 binding — cost if wrong: Task 16's queue makes refusals rarer; assertion stays honest.
Ruling (T12-stop-predicate): main uses supervisor.stop_requested() (ledger ruling P33), not the plan snippet's shutdown_signalled(); run_until additionally exits on an invalid listener handle. T12b honored: listener_failed -> "listener stopped" + supervisor.shutdown(kShutdownBound) + EXIT_FAILURE.
Note (review, accepted): eviction of a worker whose handoff was never picked up closes the replaced connection unanswered (no serve started) — outcome-equivalent to spec §2.3's eviction, recorded.
Note (known window until Task 15/17, plan's own): a worker inside P6 generation when shutdown starts is joined when the generation ends naturally (SP2 fake engine completes; proc05_* green); Task 15 (run_generation + stop cause) and Task 17 close the window.
Minor (deferred): Worker::peer set but never read (plan-prescribed member); net01 burst distribution printed to stderr (diagnostic, shown by ctest only on failure).
Task 12: complete (commits 8520deee..66b13d88, review clean) — linux-gcc 644/0 failed/4 env skips, cross build 0 warnings, pytest 25. Windows runtime: owner hardware (2026-10-20).
Task 13: BASE 66b13d88, next.

## Task 13 (SupervisorPort + GenerationGate) — 2026-10-20, commits 66b13d88..04a079d0
Brief: task-13-brief.md. RED (compile, as plan Step 2 expects): CMake "Cannot find source file: src/supervisor_port.cpp" + generation_gate.hpp missing (test cases registered before the library existed). GREEN: all 7 gate cases; full ctest 651 passed / 0 failed / 4 env skips (655 registered); timing-sensitive gate_fifo_order + gate_timeout_returns_timeout repeated 10/10 clean; cross build 0 warnings; pytest 25.
Ruling (T13-plan-fixture): the plan's GateFixture calls bare `supervisor.start();` — start() is [[nodiscard]] and -Werror rejects it; wrapped in expect(). Same for the silent-factory fixture. Why: plan text is the argument, the tree's -Werror rule binds — cost if wrong: none.
T13b honored: engine_supervisor.hpp destructor comment extended to the Windows case (reaped generation still leased in retired_ cleans up on the Lease-destroying thread) — comment only, no code change.
Harness note: the controller hand-back message said "HEAD still 8520deee, ledger untouched" — git says otherwise (Task 12 committed at 66b13d88, ledger updated); per the recovery rule I trusted git + the ledger and did NOT redo Task 12. Task 13's interrupted session left uncommitted files; re-gated everything, then committed.
Task 13: complete (commits 66b13d88..04a079d0, review clean) — linux-gcc 651/0 failed/4 env skips, cross build 0 warnings, pytest 25.
Task 14: BASE 04a079d0.

## Task 14 (fake engine M1 modes) — 2026-10-20, commits 04a079d0..2132c197
Brief: task-14-brief.md. RED (runtime): fake_engine_stream_modes_selftest "FAILED: ids cycle until --tokens, full DONE" + cascade (length/nonfinite/STOP-cancel/prefill lines); the SP2-short-form block passed before implementation (honest: unchanged behavior, per T9a).
GREEN: 135/135 on fake+supervisor+server+child; full ctest 652/0 failed/4 env skips (656 registered); cross build 0 warnings; pytest 25.
Ruling (T14-exit): the detached StdinReader thread can be blocked inside std::getline when serve returns; glibc exit()'s stdio cleanup locks every FILE including the stdin the reader holds -> main futex-waits forever (diagnosed via /proc wchan/syscall: main futex_wait_queue, reader pipe_read; "QUIT ends the engine" failed 5/5 with stdin kept open by the harness; a manual repro only exited once the stdin write-end closed, giving EOF). Fix: main flushes cout+cerr then std::_Exit(rc) — exactly the "return/_Exit" the plan's own detach comment promises; lossless (protocol lines flushed at emit, record appends closed per line, dump output flushed by the explicit cout.flush). Cost if wrong: none — _Exit(rc) reports the same exit code; crash/abort paths bypass serve entirely.
CMake: find_package(Threads REQUIRED) + Threads::Threads on strata-supervision-fake-engine and -elevated (plan's contingency branch; SP2 never linked threads).
Note: --hang-in-gen now records stdin lines (STOP lands in --record-file) but never acts on them — plan-sanctioned ("reader thread is started only after READY...; hang-in-gen stops consuming the queue"); SP2 hang tests assert stdout silence only and stay green.
Task 14: complete (commits 04a079d0..2132c197, review clean) — linux-gcc 652/0 failed/4 env skips, cross build 0 warnings, pytest 25.
Task 15: BASE 2132c197.

## Task 15 (GEN sampling keys, run_generation, context_tokens, §3.5 shape) — 2026-10-20, commits 2132c197..3c3c8808
Brief: task-15-brief.md. RED (compile, as plan Step 2 expects): "error: 'render_chat_completion' is not a member of 'secure_frontend'" + "'SamplingParams' is not a member" + no-matching begin_generation with the sampling arg.
GREEN: engine_protocol_client/chat_execution/chat_schema trio 38/38; four timing-sensitive run_generation cases repeated 20/20 clean; full ctest 660/0 failed/4 env skips (664 registered); cross build 0 warnings; pytest 25.
Ruling (T15-signature): re-anchored SP2 gives begin_generation a 3rd write_deadline param (hotfix); the plan's 3-param sampling snippet predates it — kept write_deadline 3rd, sampling is a defaulted 4th param; run_generation passes deadline_after(generation_timeout) like execute. Why: re-anchor rulings bind; cost if wrong: none.
Ruling (T15-stopcause): cancel path sends STOP(EngineStopCause::client_cancelled) (timeout/shutdown use generation_timeout) — the hotfix enum exists so the supervisor's D-2 kill-reason mapping can distinguish a vanished-client drain failure; plan's bare stop() call is its pre-hotfix ancestor.
Ruling (T15-fake): reused the test file's existing FakeTransport for gen_carries_bounded_sampling_keys instead of the plan's near-duplicate RecordingTransport (old 2-param write_line signature would not compile on the re-anchored interface anyway); assertions identical. Test-side nit: {.cancelled = {}, .shutting_down = &shutting} — GCC -Werror=missing-field-initializers rejects skipping the first member in designated init.
Carry-ins honored: RESUME/REUSED branches untouched (resume tests green in full suite); chat_execution.cpp execute() path unchanged (Task 16 switches ChatRoute to run_generation).
Task 15: complete (commits 2132c197..3c3c8808, review clean) — linux-gcc 660/0 failed/4 env skips, cross build 0 warnings, pytest 25.
Task 16: BASE 3c3c8808.

## Task 16 (ChatRoute; DOS-02 queue) — 2026-10-20, commits 3c3c8808..96dd42da
Brief: task-16-brief.md. RED (runtime, 5/13 of the new e2e cases): sampling_passthrough "GEN carries the keys: GEN 2 256,..." (no keys on SP2 execute path), chat_response_has_openai_shape "FAILED: 32 lowercase hex"/"a fresh id per response"/"created, finish_reason and usage are real" (chatcmpl-local), dos02_queue_full_is_503 "FAILED: the sixth chat is refused at once", dos02_queue_timeout_is_503, dos02_queue_is_fifo. Honest note (like T9a): the four budget cases + dos01_context_rule + excess + health-while-full + serial PASSED pre-implementation — SP2's execute path already resolves B1/B2 and the Task 12 pool already refused worker-exhaustion; they lock behavior against the ChatRoute rewrite.
DEFECT found while reaching GREEN (named in commit): GenerationGate::admit returned busy whenever max_queued == 0 even with the slot idle — serial mode (workers=1, derived queue 0) could never chat; dos02_excess chat1 itself got 503 (diagnosed by capturing the response: server_busy, then /health 200/ready showed the slot was never held). Fix: max_queued bounds WAITERS only; an arrival finding slot+queue empty takes the slot. Unit RED (stash-proven): gate_zero_queue_admits_one_at_a_time "FAILED: max_queued = 0 still admits when the slot is idle (serial mode)". dos02_serial_mode_one_worker had been a false-pass pre-fix (chat1 refused fast); now genuinely asserts first-chat-holds-second-busy.
Ruling (T16-pool): plan's pool call update (run_until(shutdown_signalled, gate.shutdown)) predates P33; shutdown_signalled() misses the recovery-exhausted exit — kept supervisor.stop_requested() predicate + added gate.shutdown() as the on-shutdown hook (Task 12 run_until already takes it; hook runs before worker joins, so queued gate waiters wake as shutting_down).
Note: execute_with_lease + render_chat_completion_response remain in libraries (SP2 tests use them); main no longer references either. engine_refusal deleted from main.
GREEN: api/server/e2e/concurrency 112/112; dos02+shape repeats 21/21 clean; full ctest 674/0 failed/4 env skips (678 registered); cross build 0 warnings; pytest 25.
Task 16: complete (commits 3c3c8808..96dd42da, review clean) — linux-gcc 674/0 failed/4 env skips, cross build 0 warnings, pytest 25.
Task 17: BASE 96dd42da.

## Task 17 (acceptance: cancel/release/shutdown rules) — 2026-10-20, commits 96dd42da..8f5fbc81
5 acceptance cases; ALL passed on FIRST run (plan expects this; RED = "would fail if the owning task's behavior regressed"). No product change needed — noted honestly in the handoff row. Stability repeats 15/15 clean. Full ctest 679/0 failed/4 env skips (683 registered); cross build 0 warnings; pytest 25.
Task 17: complete (commits 96dd42da..8f5fbc81, review clean) — linux-gcc 679/0 failed/4 env skips, cross build 0 warnings, pytest 25.
Task 18: BASE 8f5fbc81.

## Task 18 (static egress tripwire) — 2026-10-20, commits 8f5fbc81..23eecf33
Brief: task-18-brief.md. RED (compile, plan Step 2): "fatal error: support/binary_imports.hpp: No such file or directory". GREEN 2/2: real strata-secure-server ELF imports NO forbidden symbol (positive control accept/accept4 proves the reader works); synthetic ELF/PE tripwire cases green. -Werror adaptations: numeric_limits for UINT64_MAX, (void)import_size, <cctype> in the test. Full ctest 681/0 failed/4 env skips (685 registered); cross build 0 warnings (wine ctest line NOT RUN — owner ruling 2026-10-20); pytest 25.
Task 18: complete (commits 8f5fbc81..23eecf33, review clean) — linux-gcc 681/0 failed/4 env skips, cross build 0 warnings, pytest 25.
Task 19: BASE 23eecf33.

## Task 19 (runtime egress tripwires) — 2026-10-20, commits 23eecf33..d4422808
Brief: task-19-brief.md. Files: egress_tripwire.cpp (LD_PRELOAD: connect/sendto/sendmsg/getaddrinfo/gethostbyname/socket AF_UNIX|AF_NETLINK|dgram → record + deny; rest pass-through), egress_probe_helper.cpp, egress_support.hpp (arm_egress_tripwire, SocketScan, run_lifecycle), egress_test.cpp +4 cases, CMake (targets above first test_paths call, if(TARGET) defs, Linux-only; Windows registers command-like + table_scan windows-runtime + iphlpapi).
RED (real, two-part): (1) before wiring, interposer cases reported skip (exit 77, no SKIP_RETURN_CODE → ctest "Failed", not green); (2) table_scan "FAILED: only the listener and its accepted connections...: server tcp local port 49374; server tcp local port 80; ... server unix local port 0; ..." — the plan's strict per-pid rule cannot run against the non-dumpable server (uid fallback, T12-scan) nor attribute container-ambient sockets.
DEFECT+Ruling T19-scan: SocketScan rewritten — server listener set differential baseline (NET-01 precedent, scan starts after the server is listening), engine children exact-inode only (dumpable; empty inodes = socket-less or dead → claim nothing), Windows keeps plan-strict (per-pid iphlpapi). -Werror nit: baseline_ / baseline_taken_ guarded to !_WIN32 (clang -Wunused-private-field).
PROCESS CORRECTION (honesty): mid-task I claimed "fully green 25/25" while the CMake target wiring had NOT been applied (session truncation lost the edit); the next full ctest (3 failures) exposed the stale state. After applying the plan's exact CMake block: egress 6/6, repeats 20/20, full ctest 685/0 failed/4 env skips (689 registered), cross 0 warnings, pytest 25. Lesson applied henceforth: gate numbers only from a rerun AFTER the last edit.
Task 19: complete (commits 23eecf33..d4422808, review clean) — linux-gcc 685/0 failed/4 env skips, cross build 0 warnings, pytest 25.
Task 20: BASE d4422808.

## Task 20 (EXT-01/EXT-02 under tripwires) — 2026-10-20, commits d4422808..9e0f582f
Brief: task-20-brief.md. Test-only. Wrapped server_tests.ext01_extension_fields_spawn_nothing + ext02_extension_env_never_reaches_child + ext02_rejected_before_spawn with arm_egress_tripwire/SocketScan (+file_text end asserts); refusal-to-start case gets interposer-only per plan. New api cases ext01_upstream_control_routes_are_404_after_auth + ext02_http_headers_cannot_enable_extensions; server_tests WIN32 iphlpapi.
ACCEPTANCE: all 5 passed FIRST run; tripwires logged nothing during SP2 ext cases (no real finding). Adaptations: arm AFTER the case's extra_env assignment (else wiped); optional<SocketScan> (null-server tolerance). Verified server_tests compile line carries STRATA_EGRESS_* defs (interposer genuinely armed). Repeats 15/15. Full ctest 687/0 failed/4 env skips (691 registered); cross 0 warnings; pytest 25.
Task 20: complete (commits d4422808..9e0f582f, review clean) — linux-gcc 687/0 failed/4 env skips, cross build 0 warnings, pytest 25.
Task 21: BASE 9e0f582f.

## Task 21 (request logging §8) — 2026-10-20, commits 9e0f582f..f0e17ee3
Brief: task-21-brief.md. New lib secure_frontend_request_log (plan text verbatim); main wiring: RequestLogger + pool observer (Task 12 hook already took a ConnectionObserver) + stderr startup summary before supervisor.start(). Tests: 1 unit (format + token bucket exact counts) + 2 e2e (rate-limit bounds, nothing-secret sweep). RED (compile): request_log.hpp missing. All green FIRST build; repeats 9/9. Full ctest 690/0 failed/4 env skips (694 registered); cross 0 warnings; pytest 25.
Task 21: complete (commits 9e0f582f..f0e17ee3, review clean) — linux-gcc 690/0 failed/4 env skips, cross build 0 warnings, pytest 25.
Task 22: BASE f0e17ee3.

## Task 22 (Windows parity + TSan job) — 2026-10-20, commits f0e17ee3..9b53c1bf
3 win_ acceptance cases (run_cases everywhere, ctest Windows-only windows-runtime label): all exit=0 on Linux FIRST run, repeats 9/9. Label listing verified via ctest --test-dir -N -L windows-runtime (preset filter windows-runtime|release-only hides them from --preset -N — pitfall noted). 3 new entries listed (4 win_ total incl. SP2's win_send_chunks). linux-clang-tsan job added per plan + header update (ruling T22-tsan: definition-only per owner's Actions-disable; never runs from agents). No product change. Full ctest 690/0/4; cross 0 warnings; pytest 25.
Task 22: complete (commits f0e17ee3..9b53c1bf, review clean) — linux-gcc 690/0 failed/4 env skips, cross build 0 warnings, pytest 25.
Task 23: BASE 9b53c1bf.

## Task 23 (final verification + docs truth pass) — 2026-10-21, commits 9b53c1bf..11f22e47
Brief: task-23-brief.md. Battery: ctest 100% out of 690 (4 env skips: cfg03/proc03/proc05/bundle_at_mount_root), §9.3 grep 98, Total 690, clean cross rebuild EXIT=0 0 warnings (only grep match = filename api_errors.cpp), pytest 25, git status only the 5 doc files. Ruling T23-grep: the plan's 'none left'/'no stray tuning numbers' expectations DO NOT HOLD on the tree — 1 chatcmpl-local hit (legacy render_chat_completion_response kept per the plan's own Task 16 rule + pinned test) and 8 chrono hits all in untouched SP1/SP2 engine files (outside §5.3); recorded honestly in the handoff, no code change (docs-only task; spec-compatible). wine NOT RUN (owner ruling). windows-runtime pending MSYS2: 308 cases (counted per-suite from ctest -N -L windows-runtime, 650 total registered in the cross preset). Docs: AGENT_HANDOFF (M1 status, A4, D-1/D-2, Deferred-to-M2, gaps cleanup, security-behavior rewrite incl. verified defaults 10s/30s/10s/60s), threat-model (3 boundary bullets + B12/R4/R5/R8), engine-protocol-limits ('SP3 M1' section), README (routes+§7.2 table+headers+limits+Constants note; readme_config_table test re-ran green 36/36), secure-frontend-ci (2 new binaries + linux-clang-tsan status).
Task 23: complete (commits 9b53c1bf..11f22e47, review clean) — linux-gcc 690/0 failed/4 env skips, cross build 0 warnings, pytest 25.
WHOLE-BRANCH REVIEW 8520deee..HEAD (2026-10-21, fresh reviewer role): 12 commits; 12 exact Co-Authored-By trailers, 0 Claude-Session; 0 upstream/ paths, 0 out-of-scope paths, no build artifacts; run() declaration order supervisor→port→gate→chat_route→router→server→StopSupervisor→request_log→pool verified (pool/logger destroyed before everything they use); tripwire/probe targets inside if(STRATA_BUILD_SECURE_FRONTEND_TESTS); run_until = P33 predicate + gate.shutdown() hook. No defect found — no fix dispatch. FINAL-REPORT.md rewritten. M1 LANE COMPLETE — HEAD 11f22e47, tree clean, nothing pushed.
