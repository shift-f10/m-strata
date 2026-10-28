# SDD ledger — plan: docs/superpowers/plans/2026-10-13-sp1-typed-config-paths-tokens.md (shared pre-task only; SP1 proper runs in .worktrees/sp1 with its own ledger)
Ruling: Task 2 runs on branch shared/test-registration in .worktrees/pretask from f3872a86 (layout branch head), not on main at 7ddcd755 — main is behind the layout branch and the layout merge awaits the owner; the commit is merged to main after the layout merge, then sp1/sp2 rebase. Cost if wrong: one rebase of a single commit.
Task 2: BASE f3872a86; implementer dispatched (opus).
Task 2: implementer DONE_WITH_CONCERNS @ a602d8b1 (linux 111/111, wine 102/102 0 excluded, pytest 13). Ruling: concern 1 (2 socket cases collide under ctest -j8 — fixed port range 20000-20099) PARKED: pre-existing fixture behavior, presets/CI serial; SP1 should move the fixture to ephemeral port 0 or add RESOURCE_LOCK — cost if wrong: flaky parallel runs on the owner's box only. Concern 2 (Windows scratch under %TEMP%) is the brief's design — accepted. Reviewer dispatched.
Task 2: review Approved (0 Critical, 0 Important, 8 Minor; 18/18 binaries via run_cases, no registration drift, 110 names → 111 linux / 102 wine). Ruling: fix round 1 for Minors 1 (std::exit paths leave scratch dirs → catch(...) + atexit cleanup / fixtures throw), 2 (catch(...)), 3 (model_inventory_test create_directories → make_private_directory), 5 (capture GetLastError before LocalFree), 6 (errno/GetLastError + path in messages) — contained in test_support.hpp + 2 tests, shared header worth hardening now; cost if wrong: one amended commit. PARKED for SP1: Minor 4 (make_private_directory accepts existing dir unchecked; plan-mandated, no caller yet — SP1 Task 9 root checks territory), 7 (/tmp fallback when SECURE_FRONTEND_TEST_TMP unset — brief design), 8 (failure lines lack case name in all-cases mode — brief design).
Task 2: fix round 1 DONE @ 17d2452d (amended a602d8b1; linux 111/111, test-tmp empty, wine 102/102, pytest 13; failure-path probes recorded). Scoped re-review dispatched.
Task 2: fix round 1 re-review — all 5 findings ADDRESSED, no new breakage. Out-of-scope cosmetic: GetTokenInformation size==0 error source (test_support.hpp:88). Task 2: complete (shared pre-task, commit 17d2452d on shared/test-registration; awaiting merge to main after the layout merge).
Pre-task merged to main as 17d2452d (fast-forward) and pushed 2026-10-14; SP1 sub-controller continues this ledger in .worktrees/sp1.

## SP1 sub-controller (worktree .worktrees/sp1, HEAD 17d2452d verified, submodule clean)
Task 2: complete (shared pre-task, merged to main)
Ruling: pre-flight conflict scan of the 11086-line plan delegated to a read-only opus helper writing preflight-scan.md here; I read its table and rule on each row — why: keeps the controller context for 23 tasks of coordination — cost if wrong: a missed conflict surfaces in a task review loop instead.
Ruling: commit trailers are Fable 5.1 per common brief ($TRAILERS), overriding plan's SP1_TRAILERS Opus example — why: owner brief binds — cost if wrong: trailer amend.
Pre-flight scan: table in preflight-scan.md (A 55 rows, B 23 rows, C 29 rows; Tasks 1 and 3 compile+pass as written in scratch). Rulings:
Ruling: R1 Task 8 loop L5207 → `for (const std::string& text : …)` — -Werror=range-loop-construct on g++ 11.4 — cost if wrong: none (no behaviour change).
Ruling: R2 Task 9 keeps `is_below_or_equal_final_path` in token_reader.cpp until Task 11 deletes it; windows_security.cpp current_user_sid closes the token with CloseHandle/file-local RAII, not WindowsHandle — plan's deletion breaks the Windows cross build (spec §8 green every task) — cost if wrong: small cleanup in Task 11/14.
Ruling: R3 Task 10 moves `source_relative_path` under the Windows branch beside read_protected_file_windows instead of deleting it; Task 11 deletes it — same reason — cost if wrong: one leftover function caught by Task 14 grep.
Ruling: R4 Task 16 marks `fail`/`random_suffix` `[[maybe_unused]]` — -Werror=unused-function on llvm-mingw — cost if wrong: attribute noise.
Ruling: R5 Task 9 `cfg03_engine_receives_canonical_models_root` polls for a non-empty dump (≤10 s) or makes one chat request before reading — server binds before spawn after Task 6, race — cost if wrong: slightly slower test.
Ruling: R6 Task 22 CFG-06 tests kept (spec K12 names them; the type-level static_asserts are the real proof) and the runtime case reads the token via read_token from its source before scanning describe() so the scan can fail — cost if wrong: one extra line of test.
Ruling: R7 accept four plan-vs-spec deviations (tokenizer_pack root required; C7 config file/dir may be root-owned; Windows root opened through its link then final path taken; install_process_defaults hardening applies to every binary calling it); Task 24 records them in the handoff — each is safer or forced by existing CLI — cost if wrong: owner reverses one in a later SP.
Ruling: R8 implementers anchor on quoted text/function names; line numbers are hints. Task 24 replaces the whole deployment.md bullets "Generate a long, random API token…" and "Windows protected token files…" — stale numbers after earlier tasks — cost if wrong: doc touch-up.
Ruling: R9 wine runs Linux-absolute ARGS as planned; a case failing under wine only because of the path gets LABELS windows-runtime, recorded in commit + handoff; harness not rewritten — wine is smoke only — cost if wrong: a few cases skip under wine (MSYS2 still runs them).
Minors folded into briefs: kTestToken is 45 bytes; Task 13 uses make_private_directory; file is canonical_path_common.cpp not relative_path.cpp; duplicated UTF-8 validators/SDDL builders/render fns noted for reviews.
Task 1: BASE 17d2452d; implementer dispatched (opus).
Task 1: implementer DONE_WITH_CONCERNS @ f61d75f6 (linux 116/116, wine 107/107, pytest 13). Concern 1: trailer Opus 5.5 not Fable 5.1 → message-only amend in fix round. Concern 3: AGENT_HANDOFF.md:238-240 'literal or hostname' stale → deferred to Task 24 (D1). Reviewer dispatched (opus).
Task 1: review Approved (spec ✅, 0 Critical, 0 Important). ⚠️ items resolved: counts/wine from report (accepted; commit body records them); inet_ntop before WSAStartup → pending MSYS2 (note for Task 24 handoff).
Task 1: minor (deferred): parse_listener_address/IpAddress accessors have no direct unit test (consumed by Tasks 4, 6, 23)
Task 1: minor (deferred): config_test.cpp:403 wildcard canonical-text assertion accepts 0.0.0.0 or :: for every spelling
Task 1: minor (deferred): no test binds a hand-built Config with non-canonical host to exercise http_socket_server.cpp:328/333 verified->host
Task 1: minor (deferred): config.cpp:274 `!input.api_key->empty()` redundant with is_blank (brief-mandated)
Ruling: Task 1 commit f61d75f6 carried Opus 5.5 trailer (implementer's harness reminder); controller did a message-only amend → 6be5042e (tree identical) — owner brief mandates Fable 5.1 — cost if wrong: none (local, unpushed). implementer-rules.md now states the override explicitly.
Task 1: complete (commits 17d2452d..6be5042e, review clean)
Task 3: BASE 6be5042e; implementer dispatched (sonnet — brief has complete code, scan verified it compiles/passes).
Task 3: implementer DONE @ 828daac8 (linux 123/123, wine 114/114, pytest 13). Note: kept tokenizer_pack.cpp append_utf8 (still used). Reviewer dispatched (opus).
Note: SP2 sub-controller runs ctest concurrently in .worktrees/sp2; socket tests use fixed ports 20000-20099 → cross-worktree collisions possible; a socket-test failure that passes on rerun is treated as that, not a regression (ledger it). Stray pytest in /opt/projects/ai (not ours) left alone.
Task 3: review Approved (spec ✅, 0 Critical, 0 Important).
Task 3: minor (deferred): `-0` now accepted by shared reader (chat max_tokens, tokenizer vocab id) where old parsers rejected it; no test pins it
Task 3: minor (deferred): tokenizer error code shifts invalid_vocabulary→invalid_json for >kMax+1 entries and ids > INT64_MAX (fail closed; comment at tokenizer_pack.cpp:1023-1027 now inaccurate)
Task 3: minor (deferred): plan-mandated DOM parse raises peak memory for 64 MiB packs (~100 MB values vs ~4 MB streaming), bounded
Task 3: minor (deferred): three valid_utf8 copies (chat_schema, tokenizer_pack, json.cpp) + tokenizer append_utf8
Task 3: minor (deferred): chat_schema.cpp dead checks (:402 items>kMaximumMessages, :382 size precheck) and unused <limits>
Task 3: minor (deferred): json_test.cpp:1120-1125 dereferences members[2..3] without guards after failed size check
Task 3: minor (deferred): coverage gaps — exactly max_container_entries, valid surrogate pair, chat content-array depth failure
Task 3: complete (commits 6be5042e..828daac8, review clean)
Task 4: BASE 828daac8; implementer dispatched (opus).
Task 4: implementer DONE_WITH_CONCERNS @ 95ca08ad (linux 131/131, wine 122/122, pytest 13). quoted()→json_quoted() rename (ADL vs std::quoted) accepted; untested 128-char env-name cap and missing roots.secret path noted for reviewer. Reviewer dispatched (opus).
Task 4: review Needs fixes (spec ❌: 1 Important plan-mandated — config_file.cpp:33-36 collect() accepts dotted member names, so `{"listener":{"port":9000},"listener.port":9001}` silently last-wins).
Ruling: Task 4 dotted-member bypass is fixed despite plan text — spec §3.1 "duplicate keys are an error" binds; fix = reject any member name that is empty or contains '.' as unknown_key at its path, plus a defensive duplicate check on draft insert, with a regression extending cfg01_duplicate_key_rejected over every key — cost if wrong: a flat dotted form some later task expects would be rejected (none in plan per scan).
Task 4: minor (deferred): config_file.cpp:69 parse issues return before validate() — not all errors collected end to end (plan-mandated)
Task 4: minor (deferred): no test asserts several independent defects all reported in one validate()
Task 4: minor (deferred): duplicate/float JSON issues name "config file" not the key path (reader limitation)
Task 4: minor (deferred): frontend_config.cpp:224-230 remote/wildcard reported in stages (else-if)
Task 4: minor (deferred): DraftReader dereferences *find_config_key(path) unchecked (:86,99,107,117)
Task 4: minor (deferred): 128-char env-name cap (secret_source.cpp:45) untested
Task 4: minor (deferred): roots.secret defaulting / missing_required from CLI-only draft untested (cover in Task 5/6)
Task 4: fix round 1/5 (1 addressed, 0 open — dotted member names bypass duplicate check; commits 95ca08ad..67634f81)
Task 4: complete (commits 828daac8..67634f81, review clean)
Task 5: BASE 67634f81; implementer dispatched (opus).
Task 5: implementer DONE @ 67ac645a (linux 135/135, wine 126/126 — CLI cases pass unlabelled under wine, pytest 13). snapshot_tree long long cast (llvm-mingw __int128 to_string ambiguity) accepted. Reviewer dispatched (opus).
Task 5: review Approved (spec ✅, 0 Critical, 0 Important).
Task 5: minor (deferred): check() prints "Configuration is valid." before token read; unavailable token line on stdout (secure_server_main.cpp:518-524)
Task 5: minor (deferred): serve mode applies file/flag overrides silently (only --check reports)
Task 5: minor (deferred): --engine-arg replaces file engine.args list; undocumented in --help/tests
Task 5: minor (deferred, plan-mandated): replacement main dropped rationale comments (health router snapshot order; "keep serving")
Task 5: minor (deferred, plan-mandated): --help lost operator detail (--serve/--native/... rejections, glm53 needs, --pack) — restore or move to README key table (Task 24)
Task 5: minor (deferred): server_cli_test gaps (--port=V msg, '?' replacement, conflicting token flags, --check with file: source, stray-arg message; no unit test of parse_server_command_line)
Task 5: minor (deferred): snapshot_tree can pass vacuously on iterator error (assert !before.empty())
Task 5: minor (deferred): harness ignores socket()/posix_spawn_file_actions_addopen/CreateFileW returns
Task 5: minor (deferred): read_config_file uses plain ifstream (FIFO/device) — Task 12 trusted open
Task 5: complete (commits 67634f81..67ac645a, review clean)
Task 6: BASE 67ac645a; implementer dispatched (opus).
Task 6: implementer DONE_WITH_CONCERNS @ 2b0f83b4 (linux 139/139, wine 129/129 with net01 labelled windows-runtime — direct wine run passed, pytest 13). Concern: cfg01(b) unbindable-address case passes on old code too (engine SIGTERMed before marker write); RED shown only with LD_PRELOAD kill() shim. Reviewer dispatched (opus) with the concern.
Task 6: review Needs fixes (spec ❌ §8: C3 part (b) guard cannot fail on old code; Important 2: commit msg claims "failed before").
Ruling: Task 6 C3 part (b) is reworked per reviewer — engine.executable = a 0700 non-executable-format file under f.dir/bin, unbindable host 192.0.2.10 + allow_remote via config file; assert "Unable to bind" and NOT "Unable to start local engine"; RED re-shown by reverting only the secure_server_main.cpp hunk — spec §8 (failing-then-passing test) binds over the brief's test text — cost if wrong: one test differs from plan text. The false "failed before" line in 2b0f83b4 is corrected by the fix commit's message (no amend/rebase of an earlier commit) — cost if wrong: history carries one misleading line plus its correction.
Task 6: minor (deferred): cfg01 (b) does not re-probe can_connect; net02 cases assert only exit_code != 0; Windows listeners_on_port matches pid || port
Task 6: fix round 1/5 (2 addressed, 0 open — C3 guard deterministic; commit-message correction; commits 2b0f83b4..e6e82aa1). Windows RED of the guard not run (pending MSYS2).
Task 6: complete (commits 67ac645a..e6e82aa1, review clean)
Task 7: BASE e6e82aa1; implementer dispatched (opus).
Task 7: implementer DONE_WITH_CONCERNS @ d9613548 (linux 143/143, wine 133/133, pytest 13).
Ruling: Task 7 parser_honors_configured_max_request_bytes asserts 400 (not the brief's 413) for a 128 KiB POST at default limits — the listener rejects an over-bound Content-Length from headers with the parser's generic 400; spec C4 only requires "rejected at default"; changing the listener's status code is out of task scope — cost if wrong: a later listener change to emit 413 updates one assertion.
Task 7: reviewer dispatched (opus).
Task 7: review Approved (spec ✅, 0 Critical, 0 Important).
Ruling: Task 7 ⚠️ EngineProtocolLimits (262144 prompt/generated/new tokens, engine_protocol_client.hpp:62-64, constructed with {} in secure_server_main.cpp:111) — not in spec §4's internal list; carried to Task 24 (D5 must classify them as protocol facts or config-derived) and to the final review — cost if wrong: §9.5 gap found at final review.
Ruling: Task 7 ⚠️ chat_schema.cpp:579 kMaximumModelBytes=256 vs spec §4 "model id ≤ 128" — pre-existing, request-field bound (spec also lists router model field ≤ 1024), not a model-id format; carried to final review — cost if wrong: one constant.
Task 7: minor (deferred, plan-mandated): http_socket_server.cpp:852-853 positional HttpParserLimits{…} aggregate init of 4 same-typed fields; rebuilt per request
Task 7: minor (deferred): ChatExecutionService::limits_ and state->limits are not const members (§3.3 literal)
Task 7: minor (deferred): subsystem_limits_follow_config checks 8 of 17 mapped fields
Task 7: minor (deferred): no parser unit cases for configured max_headers/max_header_bytes/max_request_line_bytes
Task 7: minor (deferred): ready_timeout_is_configurable timing margin (2 s READY vs 1 s) could flake on slow hosts/wine
Task 7: minor (deferred): redundant backlog clamp silently clamps direct callers (http_socket_server.cpp:828)
Task 7: complete (commits e6e82aa1..d9613548, review clean)
Task 8: BASE d9613548; implementer dispatched (sonnet — complete code in brief; R1 loop fix carried).
Task 8: implementer DONE @ 51059af4 (linux 149/149, wine 139/139, pytest 13; R1 applied). Reviewer dispatched (opus).
Task 8: review Approved (spec ✅, 0 Critical, 0 Important).
Task 8: minor (deferred): is_windows_reserved_component is noexcept but allocates (canonical_path_common.cpp:243-244)
Task 8: minor (deferred): canonical_path.hpp uses std::move without <utility>; <cstdint> only used in .cpp; paths_test.cpp unused <cstdlib>
Task 8: minor (deferred): enumerate_local_models fails whole inventory on first invalid .gguf name (stricter grammar widens blast radius) — relevant to Task 13 Review Focus 1
Task 8: minor (deferred): coverage — 255/1024 boundary acceptance, overlong/surrogate/truncated UTF-8, message excludes input, path_from_utf8/path_to_utf8 round trip, consumer-level agreement (COM0, "x.", backslash → location())
Task 8: complete (commits d9613548..51059af4, review clean)
Task 9: BASE 51059af4; implementer dispatched (opus).
Task 9: implementer DONE @ b36dd17a (linux 157/157, wine 120/120 — 24 windows-runtime labelled, pytest 13; R2, R5 applied; make_private_directory unchanged — no test reuses loose dir). Concern: roots.secret defaulted to config dir is opened even for env: tokens (0775 dir fails); file: token gets two roots.secret issues. Reviewer dispatched (opus).
Task 9: review Needs fixes (spec ❌; 2 Important, both plan-mandated).
Ruling: Task 9 Important 1 (roots.secret defaulted to the config folder and opened even for env: token, frontend_config.cpp:270-291) is FIXED now: default applies only when token.source is file:, one roots.secret issue not two, readable message; tests env+0775 config dir → valid, file+0775 → exactly one roots.secret issue — spec §3.2/§4 (env source reads only the variable; token file lives below roots.secret) binds over brief code — cost if wrong: a file:-less deployment that relied on the config dir being checked loses a check it never needed.
Ruling: Task 9 Important 2 (absent logs/cache root now rejected via open_root(existing), frontend_config.cpp:279-280) accepted as INTERIM until Task 14 restores absent-but-creatable acceptance; carried into Task 14's dispatch with a required test "absent logs root under a private parent validates and --check creates nothing" — roots unused in SP1; Task 14 owns create_private — cost if wrong: Tasks 10-13 can't configure a not-yet-created logs/cache root (no test does).
Task 9: minor (deferred): NUL in root path reported as "empty" (canonical_path_posix.cpp:27)
Task 9: minor (deferred): executable folder owned by other user reported as shared_writable (posix :84-88); Windows open_executable maps untrusted_owner/unreadable/parent errors to shared_writable (canonical_path_windows.cpp:78-85)
Task 9: minor (deferred): Windows executable held open for write reports not_found (no FILE_SHARE_WRITE) (:67-69)
Task 9: minor (deferred): is_within noexcept but allocates (posix :92, windows :89)
Task 9: minor (deferred): no test that copied CanonicalRoot shares one handle / closes on last owner
Task 9: fix round 1/5 (3 addressed, 0 open — env token ignores config-folder default; one issue; message grammar; commits b36dd17a..5d6232e5)
Task 9: complete (commits 51059af4..5d6232e5, review clean; 1 interim ruling carried to Task 14)
Task 10: BASE 5d6232e5; implementer dispatched (opus).
Task 10: implementer DONE @ e07b0601 (linux 164/164 = 159+5, wine 120/120, pytest 13; R3 applied; mount-crossing test ran, not skipped).
Ruling: Task 10 read_token(path) overload now requires an absolute non-shared-writable root, so strata-secure-smoke-server --secret-root <relative> fails — accepted: spec §3.4 roots must be absolute; README documents an absolute /path/to/root; no script or test passes a relative one — cost if wrong: a dev using a relative smoke-server root must pass an absolute one.
Task 10: reviewer dispatched (opus).
Task 10: review Approved (spec ✅, 0 Critical, 0 Important). ⚠️ token file ≤ 4 KiB still 1 MiB (token_reader.cpp:591) → resolved: Task 18 brief owns kMaximumTokenFileBytes; carried into Task 18 dispatch/review. ⚠️ openat2 vs fallback indistinguishable in auto mode on 5.15 without strace → accepted (both modes tested explicitly).
Task 10: minor (deferred): walk fstat failure reported as cross_device (canonical_path_posix.cpp:420-422)
Task 10: minor (deferred): read_all too_large after growth mapped to read_failed not token_too_large (token_reader.cpp:638-639); unparseable location → read_failed (:631)
Task 10: minor (deferred): read_all 64 KiB stack buffer + string reallocations hold token bytes unwiped (canonical_path_common.cpp:270) → carry into Task 18 (SecretBytes)
Task 10: minor (deferred): automatic-mode ENOSYS/EPERM fallback decision untested (posix.cpp:436)
Task 10: complete (commits 5d6232e5..e07b0601, review clean)
Task 11: BASE e07b0601; implementer dispatched (opus).
Task 11: implementer DONE_WITH_CONCERNS @ 9d9b1fba (linux 165/165, wine 120/120, pytest 13; windows-runtime cases under wine 0/4 — wine limitations: GetFinalPathNameByHandleW >MAX_PATH, D:P dropped, no junctions, symlink as not-found; all fail closed, pending MSYS2). Concerns: header comment claims root opened with OPEN_REPARSE_POINT (it isn't, R7); final-path failure reported as escapes_root; test SID/SD helpers duplicate src (brief-mandated). Reviewer dispatched (opus).
Task 11: review Approved (spec ✅, 0 Critical, 0 Important). Named risk cleared: final_path_for_handle sizes its buffer dynamically (windows_security.cpp:56-68); wine long-path failure is a wine limitation. ⚠️ real-Windows behaviour of the 4 windows-runtime cases pending MSYS2 (record in handoff, Task 24).
Task 11: minor (deferred): failed final-path lookup / classify_final_path PathError collapsed to escapes_root (canonical_path_windows.cpp, brief code) — wrong message, fails closed; also hides network_location actionable message (Review Focus 2!) on below-root walks
Task 11: minor (deferred): final_path_for_handle no retry if path grows between calls (windows_security.cpp:62-65)
Task 11: minor (deferred, plan-mandated): token_reader.hpp comment says root opened with OPEN_REPARSE_POINT — contradicts R7
Task 11: minor (deferred): GetFileInformationByHandleEx return unchecked in open_beneath (Windows)
Task 11: minor (deferred): Windows walk reopens by full path per component (rename/junction race bounded to within root); hold parent handles without FILE_SHARE_DELETE as hardening
Task 11: minor (deferred): no non-ASCII profile coverage; remove_scratch_directories may leak >260-char trees on Windows
Task 11: complete (commits e07b0601..9d9b1fba, review clean)
Task 12: BASE 9d9b1fba; implementer dispatched (opus).
Task 12: implementer DONE @ ccd7c63b (linux 166/166, wine 120/120, Windows case passed in a direct wine run — pending MSYS2, pytest 13). Reviewer dispatched (opus).
Task 12: review Approved (spec ✅, 0 Critical, 0 Important). ⚠️ Windows DACL check pending MSYS2 (wine doesn't model DACLs) — handoff item.
Task 12: minor (deferred): config dir error reads "directory: the root must not be writable…" (config_file.cpp:100) — wording
Task 12: minor (deferred): no assertion that file_untrusted reasons exclude path; untrusted_owner and root-owned dir (R7) untested (need non-root setup)
Task 12: minor (deferred): grant_everyone_write ignores CreateWellKnownSid result (test_support.hpp:178); test leaves a 0777 scratch dir
Task 12: complete (commits 9d9b1fba..ccd7c63b, review clean)
Task 13: BASE ccd7c63b; implementer dispatched (opus).
Ruling: Task 13 — entries under the models root whose names fail the P1 grammar and are NOT .gguf files (and not directories that must be descended) are ignored as before, not an inventory failure; .gguf entries and symlinks keep failing with a message naming the entry (Review Focus 1) — spec P6 asks for enumeration through the module, not for failing on unrelated files; a stray README~1 or notes:txt should not take down every model — cost if wrong: a non-portable non-model file is silently skipped. Also: Task 13 uses make_private_directory, not create_directories (GC L27).
Task 13: implementer DONE @ 96aaf699 (linux 171/171 incl. inventory_ignores_non_portable_non_model_names for the ruling, wine 114/114; 2 junction cases fail under wine — wine can't create junctions, pending MSYS2). Concern: a non-portable-named directory is skipped with its contents. Reviewer dispatched (opus).
Task 13: review Needs fixes (spec ❌: non-portable DIRECTORY skipped with its .gguf contents — contrary to my ruling's "directories that must be descended" exclusion).
Ruling: Task 13 clarifies the pre-dispatch ruling — a non-portable directory name fails the inventory with invalid_artifact_path naming the entry (fail closed; models never vanish silently); only non-.gguf FILES with non-portable names are ignored. Fold in Minor 1 (Windows FindNextFileW must check ERROR_NO_MORE_FILES, matching POSIX readdir fix) and Minor 2 (list_directory failure message appends the fixed PathError reason) — same "models vanish/undiagnosable" class as Review Focus 1 — cost if wrong: an odd-named folder of non-model files now fails startup with a named, actionable message.
Task 13: minor (deferred): new tests don't remove scratch dirs (other_disk/outside left behind)
Task 13: minor (deferred): ModelInventoryErrorCode::artifact_path_escapes_root now unused (handoff note)
Task 13: minor (deferred): Windows list_directory lists by path (FindFirstFileExW), not by pinned handle — final review
Task 13: fix round 1/5 (4 addressed, 0 open — non-portable dir fails inventory; Windows FindNextFileW error; list failure reason; test cleanup; commits 96aaf699..56a60c5d)
Task 13: minor (deferred): older inventory/tokenizer tests (inventories_only_local_gguf_artifacts, rejects_duplicate_ids) may leak scratch dirs
Task 13: complete (commits ccd7c63b..56a60c5d, review clean)
Task 14: BASE 56a60c5d; implementer dispatched (opus).
Task 14: implementer DONE @ 264edde1 (linux 175/175, wine 114/114; cfg02_create_within_root_writes_nothing_outside fails under direct wine — no junctions, pending MSYS2; pytest 13; P9 grep empty). Concern: Windows create_directory_within skips permission checks on existing intermediate dirs (brief) — relevant to Tasks 16/17/21. Reviewer dispatched (opus).
Task 14: review Approved (spec ✅, 0 Critical, 0 Important). Task 9 interim ruling CLOSED (absent logs/cache validate; --check creates nothing — cli_check_has_no_side_effects). ⚠️ Windows runtime of 3 new cases pending MSYS2.
Task 14: minor (deferred, plan-mandated): Windows create_directory_within doesn't check DACL/owner/volume of existing components (POSIX does); no SP1 caller reaches it (all end in open_existing_root) — document in header/handoff (Task 24)
Task 14: minor (deferred): create_directory_within not all-or-nothing (earlier components left on failure)
Task 14: minor (deferred): Windows GetFileInformationByHandleEx unchecked (canonical_path_windows.cpp:387); path-string walk race (owner-only)
Task 14: minor (deferred): stale comment frontend_config.cpp:468 ("held open by the FrontendConfig" — logs/cache are plain paths)
Task 14: minor (deferred): Windows create_junction return unchecked (paths_test.cpp:648); chmod return unchecked (typed_config_test.cpp:788)
Task 14: complete (commits 56a60c5d..264edde1, review clean)
Task 15: BASE 264edde1; implementer dispatched (opus).
Task 15: implementer DONE @ 5cc8a91f (linux 180/180, wine 119/119, pytest 13; ASan/UBSan -O2/-O3 manual runs clean). Reviewer dispatched (opus).
Task 15: review Approved (spec ✅, 0 Critical, 0 Important). ⚠️ real-Windows BCryptGenRandom/SecureZeroMemory pending MSYS2.
Task 15: minor (deferred): destroy check only searches marker; use all_zero (secrets_test.cpp:407-410); std::get in differs test → get_if+expect
Task 15: minor (deferred): base64url_encode aliasing (out.clear() before reading bytes) — document precondition
Task 15: minor (deferred): getrandom returning 0 would spin forever (treat count==0 as failure); ULONG cast truncation in fill_random; non-glibc wipe fallback; unused <cstdlib>
Task 15: complete (commits 264edde1..5cc8a91f, review clean)
Task 16: BASE 5cc8a91f; implementer dispatched (opus).
Task 16: implementer DONE @ e0f4294c (linux 183/183, wine 119/119, pytest 13; R4 applied). Reviewer dispatched (opus).
Task 16: review Needs fixes (1 Important, plan-mandated: rotate test discards replace results — successful rotate never asserted).
Ruling: Task 16 rotate test asserts each replace succeeds and content changes (spec K2 "rotate replaces atomically" binds over the brief's (void) casts); fold in Minor 2: fstatat errors other than ENOENT fail closed (unsafe_location/write_failed) instead of proceeding — cost if wrong: none material.
Task 16: minor (deferred): fsync(dir) failure ignored but success returned (brief); linkat-fallback unlinkat failure ignored (second hard link possible, 0700 dir); replace-mode fstatat→renameat window (renameat doesn't follow); test stat()/chmod returns unchecked; root 0700 not asserted
Task 16: fix round 1/5 (2 addressed, 0 open — rotate success asserted; fstatat non-ENOENT fails closed; commits e0f4294c..510617b5)
Task 16: minor (deferred): EACCES regression check assumes non-root test user
Task 16: complete (commits 5cc8a91f..510617b5, review clean)
Task 17: BASE 510617b5; implementer dispatched (opus).
Task 17: implementer DONE @ 25ba2270 (linux 183/183, wine 119/119; 3 new windows-runtime cases 0/3 under direct wine — wine drops D:P, pending MSYS2; DPAPI probe round-trips under wine; pytest 13). Added GetFileAttributesW non-NOT_FOUND fail-closed (mirrors Task 16). Concerns 3 (reader rejects store's trailing \n) and 4 (plaintext read buffers unwiped) → Task 18 (K5 defect c, K6 buffers). Reviewer dispatched (opus).
Task 17: review Needs fixes (2 Important, plan-mandated: Windows writer doesn't open the holding dir via open_beneath nor pin it (FILE_SHARE_DELETE); K3 round-trip tests never compare the value).
Ruling: Task 17 fixes both — Windows store_token opens the parent through open_beneath(…, OpenKind::directory) like Linux and holds a handle without FILE_SHARE_DELETE across create+move; round-trip and plaintext tests compare the read value; fold Minors 1-2 (early return on GetNamedSecurityInfoW failure; DACL test uses has_private_owner_dacl / checks owner+ACEs) — spec "token lives below roots.secret" + K3 runtime proof (MSYS2 is first real run) bind over brief code — cost if wrong: Windows-only code unverifiable here; MSYS2 decides. Full handle-relative NtCreateFile deferred → handoff.
Task 17: minor (deferred): plaintext token starting with STRATAK1 treated as DPAPI (doc note); token_store links whole token_reader for kDpapiTokenMagic; redundant test link
Task 17: fix round 1/5 (4 addressed, 0 open — open_beneath + pinned dir w/o FILE_SHARE_DELETE + file-ID match + temp landing check; value comparisons; DACL test early return + owner/ACE checks; commits 25ba2270..013f540a). Windows-only, judged by reading; pending MSYS2.
Task 17: minor (deferred): SetFileInformationByHandle disposition return ignored (empty temp may remain); path-based DeleteFileW temp cleanup; ancestor-rename protection relies on unverified NTFS behaviour (handoff)
Task 17: complete (commits 510617b5..013f540a, review clean)
Task 18: BASE 013f540a; implementer dispatched (opus).
Task 18: implementer DONE @ f9f146ac (linux 192/192, wine 122/122; 8 file cases windows-runtime fail under direct wine on DACL — pending MSYS2; pytest 13). Carried items done: 4 KiB cap + test, wiped token read buffer, too_large mapping. Mains' std::string copies → Task 19; getenv wipe → Task 20. Reviewer dispatched (opus).
Task 18: review Approved (spec ✅, 0 Critical, 0 Important). ⚠️ 8 Windows file cases + DPAPI read pending MSYS2; grow-after-size-check path untested (needs racing writer) — accepted by reading.
Task 18: minor (deferred): BOM/newline messages say "token file" on the env path (token_reader.cpp)
Task 18: minor (deferred, plan-mandated): mkfifo return unchecked in rejects_a_non_regular_file; env trailing-\n rejection unpinned; quote check after charset loop
Task 18: complete (commits 013f540a..f9f146ac, review clean)
Task 19: BASE f9f146ac; implementer dispatched (opus).
Task 19: implementer DONE @ 137f2b4a (linux 195/195, wine 125/125, pytest 13; no std::string token copy left in mains). Reviewer dispatched (opus).
Task 19: review Approved (spec ✅, 0 Critical, 0 Important). Reviewer disassembled secret_bytes.cpp at -O2: 256-iteration branch-free compare, explicit_bzero kept (record in handoff).
Task 19: minor (deferred, plan-mandated): auth03_compare_touches_fixed_window counter is unconditional (folds to constant) — proves loop bound only
Task 19: minor (deferred): memcpy of size bytes into windows scales with length (spec design); header comment overstates; untested 256/256, 255 vs 256, make_bearer_validator(nullptr)
Task 19: complete (commits f9f146ac..137f2b4a, review clean)
Task 20: BASE 137f2b4a; implementer dispatched (opus).
Task 20: implementer DONE @ 2b480430 (linux 197/197, wine 125/125, pytest 13; Windows scrub probe under wine: getenv/_wgetenv null, Win32 entry absent — pending MSYS2). Reviewer dispatched (opus).
Task 20: review Approved (spec ✅, 0 Critical, 0 Important). ⚠️ Windows scrub has no ctest case (wine probe only) — pending MSYS2/handoff; no setenv of the token name precedes the read (accepted by inspection).
Task 20: minor (deferred): duplicate NAME= entries in initial env block — only first wiped; K9 dumpable check skipped as root (use prctl(PR_GET_DUMPABLE)); setrlimit/prctl failures silent; unrelated reformat token_reader.cpp:164
Task 20: complete (commits 137f2b4a..2b480430, review clean)
Task 21: BASE 2b480430; implementer dispatched (opus).
Task 21: implementer DONE @ 92a8419a (linux 199/199, wine 125/125; 2 init cases windows-runtime fail under direct wine on D:P — pending MSYS2; pytest 13). Concerns: render_config_document duplicates test helper tree builder (A49); --force not all-or-nothing (token rotated before config failure), empty 0700 dir may remain. Reviewer dispatched (opus).
Task 21: review Approved with 1 Important plan-mandated (JSON tree builder duplicated: config_file.cpp render_config_document vs tests/support/config_json.hpp render_config_json).
Ruling: Task 21 dedup now — production exposes one document builder over pre-encoded literals; render_config_document and the test helper both call it (helper keeps only its duplicate-injection mode) — rubric: verbatim logic duplication is Important and tests should prove what init emits; fold Minors 1-2 (with --force refuse up front when the config target exists and is not a regular file; check config file name before creating the directory) — cost if wrong: small refactor of a test helper.
Task 21: minor (deferred): partial failure after token+config written blocks retry without --force (no hint); coverage — init with env: source, token-only pre-exists, 0600/0700 modes, negative CLI cases (init w/o --config, --force w/o init, init --check)
Task 21: fix round 1/5 (3 addressed, 0 open — shared render_config_literals; --force refuses non-regular config target; validate before creating dir; commits 92a8419a..dae07073)
Task 21: minor (deferred): token-store failure after dir creation can leave config dir; config write failure after --force token rotation (other I/O) keeps new token; helper output now pretty-printed
Task 21: complete (commits 2b480430..dae07073, review clean)
Task 22: BASE dae07073; implementer dispatched (opus).
Task 22: implementer DONE @ 792b7651 (linux 204/204, wine 126/126, pytest 13; 3 acceptance tests passed first run; mutation check showed scans fail with token present; R6 applied — typed_config test links token_reader). Reviewer dispatched (opus).
Task 22: review Approved (spec ✅, 0 Critical, 0 Important). cfg05 windows-runtime label justified (only excludes wine; runs on Linux + MSYS2).
Task 22: minor (deferred, plan-mandated): CFG-06 static_asserts check only 5 top-level member types — add static_assert(std::is_copy_constructible_v<FrontendConfig>) to cover nested/future members
Task 22: minor (deferred): raw-bytes scan covers only inline bytes (describe() does the work); cfg05 uses --token-env flag path, not config token.source=env:; error-body scan samples 401/404/400 only; SF_CFG06_TOKEN never unset
Task 22: complete (commits dae07073..792b7651, review clean)
Task 23: BASE 792b7651; implementer dispatched (opus).
Task 23: implementer DONE @ 3810a67b (linux 209/209, wine 127/127; 4 labelled cases also pass under direct wine; pytest 13; all 5 acceptance passed first run; no production change). Reviewer dispatched (sonnet — test-only diff).
Note: Task 23 reviewer actually dispatched on opus (ledger line said sonnet).
Task 23: review Needs fixes (1 Important plan-mandated: AUTH-04 flag and STRATA_* sweeps hard-coded, not schema-derived).
Ruling: Task 23 derives the AUTH-04 flag sweep from config_schema() ConfigKey::flag (and the CLI flag table if reachable) and seeds the env probe with every STRATA_[A-Z_]+ name found in the schema plus the existing guesses; fold Minor 2 (socket-level AUTH-01 adds duplicate-header→400 and tab-separator→401 over TCP) — spec A4 "every flag, key and STRATA_* env name in the schema list", A1 "via router and socket" — cost if wrong: test-only change.
Task 23: minor (deferred): route list hand-picked (no route table; /does-not-exist probe limits drift); positive control on one route only; forbidden-word heuristic ("dev" vs "device")
Task 23: fix round 1/5 (2 addressed, 0 open — schema-derived AUTH-04 sweeps incl. STRATA_ENGINE_READY_S; TCP tab/duplicate cases; commits 3810a67b..1904f827)
Task 23: complete (commits 792b7651..1904f827, review clean)
Task 24: BASE 1904f827; implementer dispatched (opus) with task-24-handoff-inputs.md.
Task 24: implementer DONE @ 05c9db02 (linux 210/210, wine 128/128 of 189 registered (61 windows-runtime), pytest 13; 58 acceptance-named cases listed; wine --help 0 / --check bad 1; D4 tested on scratch copy). Found: max_prompt_tokens config ≤1048576 vs protocol 262144; max_messages config ≤1024 vs renderers hard-coded 128 — documented only → final review. README test strips \r (CRLF checkout). Reviewer dispatched (opus).
Task 24: review Approved (spec ✅, 0 Critical, 0 Important in scope). ⚠️ §9.5: renderers hard-code kMaximumMessages=128 while limits.chat.max_messages accepts ≤1024; limits.chat.max_prompt_tokens accepts ≤1048576 while EngineProtocolLimits caps 262144 (accepted-but-ineffective config).
Ruling: §9.5 accepted-but-ineffective ranges are fixed in the final fix wave by narrowing the schema ranges to the enforced caps (max_messages ≤ 128, max_prompt_tokens ≤ 262144) and regenerating the README table + handoff gap 7 — spec §9.5 and C2 ("validate() with ranges") bind; narrowing fails closed at validation; plumbing config into the renderers/protocol is larger and SP3's territory — cost if wrong: an operator cannot configure values the server never honoured anyway.
Task 24: minor (deferred): README openat2 flags omit RESOLVE_NO_MAGICLINKS; README init wording implies token always written; D5 drops router model field ≤1024 internal constant; README ownership omits root (R7)
Task 24: complete (commits 1904f827..05c9db02, review clean)
Final review: MERGE_BASE 17d2452d, HEAD 05c9db02; dispatched (opus).
Final review: With fixes — 0 Critical, 3 Important (I1 §9.5 ranges incl. max_message_bytes; I2 Windows engine argv path via ANSI .string(); I3 serve mode drops actionable token/tokenizer/inventory reasons), 3 Minor (M1 init explicit roots.secret not created / 0755 dir passes --check then init fails; M2 six valid_utf8 copies; M3 constant_time_token_equals wrapper).
Ruling: ONE fix wave carries I1 (narrow max_messages≤128, max_message_bytes≤65536, max_prompt_tokens≤262144; 512 KiB prompt cap documented internal), I2, I3 and folds F1 README init wording, F2 token_reader.hpp R7 comment, F3 check() verdict after token read, F4 handoff gaps 2/7 + counts — §9.5, Review Focus 1/3/4 bind — cost if wrong: narrower operator ranges; small message changes. M1, M2, M3 stay deferred (handoff/final report).
Final review triage: stale minors closed — T4 roots.secret defaulting (covered by T9), T5 ifstream (T12), T6 net02 exit code (stderr checked), T8 blast radius (T13 ruling), T10 read_all mapping/buffers (T18), T9 interim (T14). All other deferred minors stay deferred.
Final fix wave: BASE 05c9db02; fixer dispatched (opus) with final-fix-findings.md.
Final fix wave: DONE @ 04210cbf (5 commits 9e8312b5..04210cbf; linux 215/215, wine 128/128 of 193 (65 windows-runtime), pytest 13). I2 RED only showable under wine/MSYS2. Scoped re-review dispatched (opus).
Final fix wave: scoped re-review — all findings (I1, I2, I3, F1–F4) ADDRESSED, no new breakage. Controller rerun: linux-gcc 215/215, cross build 0 warnings, wine 128/128, pytest 13.
Final review residuals (parked): smoke_server_main.cpp:155,177 generic token/inventory lines; "Engine arguments rejected." without reason; 512 KiB rendered-prompt cap reachable when request/body limits raised >~512 KiB (documented internal); M1 init explicit roots.secret not created / 0755 dir passes --check then init fails; M2 six valid_utf8 copies; M3 constant_time_token_equals wrapper.
Ruling: plan workspace NOT deleted — the SP1 brief puts FINAL-REPORT.md inside it and the guard blocks rm -rf; the top-level controller removes it after integration — cost if wrong: one git-ignored directory remains.
SP1: COMPLETE @ 04210cbf.
Top-level controller verification @ 04210cbf (2026-10-14 09:10 UTC): linux-gcc 0 warnings, ctest 215/215; windows-llvm-mingw-cross 0 warnings, wine 128/128; pytest 13. PARKED minor: out/build/linux-gcc/test-tmp held one stale mkdtemp dir dated 02:05 (interrupted/SIGKILL-ed run skips atexit cleanup) — run_cases or the ctest fixture could sweep stale test-tmp entries at start; not a correctness issue. Merge commit e7f01b9e prepared in .worktrees/merge-sp1; merged-tree verification running.
