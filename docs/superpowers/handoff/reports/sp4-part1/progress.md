# SDD ledger — plan: docs/superpowers/plans/2026-10-14-sp4-acquire.md

Scope: Tasks 1 -> 24b only (25-29 wait for SP2 run-2). Branch sp4-acquire, start HEAD b16098768a30e7c5c6a8daff7995ab5461224989, upstream 296b59d2 (v0.1.44.1).
Baseline: linux-gcc 215/215, wine 128/128, pytest 13, 0 warnings (controller-verified).

## Setup rulings
Ruling: commit trailers = the two literal Fable lines (Co-Authored-By: Claude Fable 5.1 / Claude-Session ...<session>) written literally in ONE -m $'...\n...' paragraph (so git parses both as trailers), never via a shell variable — the common brief and SP4 brief (owner rules) both demand Fable literal lines; the plan's two separate -m trailer paragraphs would leave the first unparsed as a trailer — cost if wrong: commit-message amend before merge (cosmetic).
Ruling: the pre-flight conflict scan's reading of the 865 KB plan + spec is delegated to one read-only opus helper writing preflight-scan.md; I read the plan header (Controller notes, Global Constraints, Review Focus, file structure) myself and rule on its table — keeps ~250k tokens of plan out of the coordination context for 25+ dispatch cycles — cost if wrong: a conflict the helper missed surfaces later in the review loop (the net).
Ruling: the skill's "delete workspace at finish" is NOT done — the SP4 brief puts FINAL-REPORT.md in the workspace and rm -rf is forbidden by owner rules — cost if wrong: a git-ignored directory lingers.

## Pre-flight conflict scan
Full table (per-task rows, producer->consumer pairs, existing-code check): preflight-scan.md (this directory). 3 blocker, 5 important, 55 minor; no wrong existing-code name/signature/anchor in Tasks 1-24b.
Ruling: F-1 (T22 reassigns variant<PartFile,...>; PartFile move-assign deleted) — T22 builds `opened` once without reassignment (lambda that retries create_new after remove_file_within); PartFile keeps deleted move-assign — compile error is certain otherwise; spec silent on shape — cost if wrong: small T22 refactor.
Ruling: F-2 (T10 by-value std::string range-for, -Werror=range-loop-construct) — use `const std::string&` — cost if wrong: none.
Ruling: F-3 (T16 discarded ::link() result breaks fortify release build) — `expect(::link(...) == 0, ...)` — cost if wrong: none.
Ruling: F-4 (T15 cfg01_pull_keys_table_driven POSIX-only absolute paths in a windows-runtime case) — derive values from make_scratch_directory() or #ifdef _WIN32 literals — cost if wrong: MSYS2 CI failure caught later.
Ruling: F-5 (T15 dl04_staging_ignored_by_inventory_scan unlabelled) — add LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME}, matching SP1 inventory-root cases — cost if wrong: one fewer wine case.
Ruling: F-6 (T16 must_rel/join_rel throw logic_error on joined paths >1024 B from untrusted media) — return AcquireError via parse_relative_path (bundle_invalid / publish_failed) instead of throwing; spec requires fail-closed import — cost if wrong: minor extra code.
Ruling: F-7 (T24b engine argv via path::string()) — use path_to_utf8(root.join(rel)) for argv and open_root on the path; add a non-ASCII managed case — SP1 rule "UTF-8, never path::string()" — cost if wrong: one extra test.
Ruling: F-8 (T24b --check accepts managed configs serve refuses; spec 4.9 c/d calls them config errors) — check() runs the same managed-config validation (shared pure function or compose_engine_launch); assert --check in one cfg01_*_managed_model case — cost if wrong: small scope growth in T24b.
Ruling: minor findings F-9..F-63 — each is carried by F-id into its task's dispatch; implementer applies the scan's suggested resolution when local to the task and spec-consistent, else records why not in the report; spec silences (F-12, F-13, F-14, F-21) keep the plan's text and are noted for the final review — cost if wrong: minors reappear in the final review.
Task->F-id map: 1:F-9,F-10,F-11 | 2:F-12 | 3:F-9 | 4:F-13 | 5:F-15 | 6:F-14,F-16 | 8:F-17,F-18,F-19 | 9:F-20 | 10:F-2,F-21 | 12:F-22,F-23,F-24 | 13:F-25,F-26 | 14:F-27..F-31 | 15:F-4,F-5,F-32,F-33 | 16:F-3,F-6,F-34,F-35,F-36 | 17:F-37 | 18:F-38,F-42 | 19:F-39..F-42 | 20:F-43..F-48 (+F-6 consumer) | 21:F-49,F-50,F-51 | 22:F-1,F-51..F-56 | 23:F-57,F-58 | 24a:F-59,F-60,F-61 | 24b:F-7,F-8,F-58,F-62,F-63

## Tasks
Task 1: dispatched implementer a74c0254f6f79da1e (opus), BASE b1609876
Ruling: Task 1 implementer used the harness default trailer (Co-Authored-By: Claude Opus 5.5) and flagged it; I ruled for the owner-brief Fable lines (common brief + SP4 brief + plan Global Constraints all mandate them; the harness reminder defers to the user's own instructions on these lines) and had it amend its own unpushed HEAD — cost if wrong: owner/controller re-trailers the branch commits before merge (metadata only). FLAG for final report.
Ruling (REVERSES the previous trailer ruling): every SP4 commit carries `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` + the Claude-Session line, not the Fable lines — the Fable instruction reaches this run only through agent-written briefs, the harness attribution rule yields only to the user's own CLAUDE.md/memory (which say nothing on it), and a Fable trailer would credit a model that did not write the code; Task 1 commit 97723a9e stands unamended — cost if wrong: if the owner himself wants Fable, the integrator re-trailers with metadata-only amends/rewrites before merge. FLAG for final report and top-level controller.
Task 1: implementer DONE_WITH_CONCERNS (trailer only) commit 97723a9e; linux-gcc 222/222, release 2/2, wine 135/135, pytest 13; reviewer a33ab81e7ab6b0f54 dispatched
Task 1: minor (deferred): sha256.cpp:545 is_lower_hex_digest line >120 cols (plan-mandated verbatim)
Task 1: minor (deferred): sha256.hpp friend sha256_add_length_for_test declared even in hooks-OFF builds (plan-mandated); move inside #if
Task 1: minor (deferred): kTestHooksMarker unused -> Task 28 marker scan needs a reference in hooks-ON binaries (note for Tasks 25-29)
Task 1: minor (deferred): long_message_one_gibibyte ~55 s in every linux-gcc run (TIMEOUT 900)
Task 1: complete (commits b1609876..97723a9e, review clean)
Ruling: pure-library tasks whose brief carries the complete code (2-7, 9-11) go to sonnet implementers; judgment/integration tasks (8, 12-24b) to opus; all reviewers opus — common brief allows sonnet for pure transcription — cost if wrong: extra fix rounds on a sonnet task.
Task 2: dispatched implementer a5170b41d5e541f18 (sonnet), BASE 97723a9e
Ruling: the Co-Authored-By trailer names the model that actually wrote the commit (Opus 5.5 or Sonnet 5.5) — same reasoning as the trailer ruling above — cost if wrong: metadata-only re-trailer at integration.
Task 2: implementer DONE commit 36d2da10 (linux-gcc 225/225, wine 138/138, release 2/2); reviewer af9f8a672a2cf0d8d dispatched
Task 2: minor (deferred): >120-col lines acquire_reason.cpp reason_info, acquire_test snake_case loop (plan-mandated)
Task 2: minor (deferred): reason_name() untested; snake_case failure message doesn't name the row
Task 2: minor (deferred): acquire_reason.cpp std::size_t without <cstddef>; unused <tuple> in test (plan-mandated)
Task 2: minor (deferred): reason_info indexes kReasons with no range check on out-of-range enum (watch when reasons are decoded from record channel/log in 25-28)
Task 2: complete (commits 97723a9e..36d2da10, review clean)
Task 3: dispatched implementer (sonnet), BASE 36d2da10
Task 3: implementer DONE_WITH_CONCERNS commit 3d2fbdfa (linux-gcc 231/231, wine 144/144, release 2/2); concern: Location 'https://h.example?x' -> target '/?x' per brief
Task 3: minor (deferred, security-flavoured): is_valid_dns_name accepts hex-form IPv4 (0x7f.0x1, 1.0x7f000001) despite spec §2.2 "no IP literal" (plan-mandated; mitigated by §3.5 getaddrinfo deny) — final review to triage
Task 3: minor (deferred): is_valid_proxy_host loose (999.999.999.999, octal, [:::]) and rejects uppercase / trailing slash (plan-mandated, usability)
Task 3: minor (deferred): describe_url_problem text https-only, wrong for proxies (no current caller shows it)
Task 3: minor (deferred): Location scheme match case-sensitive (fails closed)
Task 3: minor (deferred): no exact-boundary tests (2048/8192), host_in/describe_url_problem untested, proxy rejection loop asserts no specific problem; >120-col lines (plan-mandated)
Task 3: complete (commits 36d2da10..3d2fbdfa, review clean)
Task 4: dispatched implementer (sonnet), BASE 3d2fbdfa
Task 4: implementer DONE commit 32a2cdb8 (linux-gcc 236/236, wine 149/149, release 2/2)
Task 4: minor (deferred): address_policy.hpp:128-129 comment says IPv6-only hop -> address_not_allowed but function returns no_usable_address (fix wording)
Task 4: minor (deferred): misplaced fec0 comment fetch_test.cpp:346; outer-boundary IPv6 allowed cases untested; handoff line says address_policy_* (5) but 3 cases lack prefix; >120-col lines (plan-mandated)
Task 4: complete (commits 3d2fbdfa..32a2cdb8, review clean)
Task 5: dispatched implementer (sonnet), BASE 32a2cdb8
Task 5: implementer DONE commit 3847bbd6 (linux-gcc 241/241, wine 154/154, release 2/2, pytest 13)
Task 5: minor (deferred, carry to Task 21 review): TransferRequest url/host are independent strings — Task 21 must build both from one parsed HttpsUrl or --resolve silently stops pinning (bypasses §3.5)
Task 5: minor (deferred): proxy argv test resets pin, doesn't assert --resolve/--noproxy absent (plan-mandated); --cacert hook presence/absence untested (Task 28?); >120-col test lines
Task 5: complete (commits 32a2cdb8..3847bbd6, review clean)
Task 6: dispatched implementer (sonnet), BASE 3847bbd6
Task 6: implementer DONE commit a0441bbe (linux-gcc 245/245, wine 158/158, release 2/2); concern: size_t underflow in 206 expected range if file_size==0
Task 6: minor (deferred): no positive boundary tests (exactly 4 1xx, 64 lines, 16384 B); untested classify_head paths (dup/non-numeric Content-Length, 206 wrong length, 3xx no Location)
Task 6: minor (deferred): status <100 accepted as final head; dead check download_response.cpp:215 (brief code); file_size==0 underflow unreachable (T8 size>=1) and fails closed
Task 6: complete (commits 3847bbd6..a0441bbe, review clean)
Task 7: dispatched implementer (sonnet), BASE a0441bbe
Task 7: implementer a05bf0b6e95504fef still running (background build) when the harness forced hand-back; not yet reviewed
Resume: coordinator ACCEPTED the trailer ruling (model that wrote the commit + Claude-Session; no re-trailering).
Task 7: implementer DONE commit b3c81a99 (linux-gcc 249/249, wine 162/162, release 2/2)
Task 7: minor (deferred, carry to Task 21): restart_file reasons (range_mismatch/size_mismatch_announced) consume retry slots; the fetch loop must cap the 200-to-range restart at once per file (spec §3.4); caller must derive transient = is_transient(reason) || head.transient
Task 7: minor (deferred): untested edges (last source 5th transient -> exhausted 0 s, SourceSchedule(0/1)); header comment should note Controller note 13; >120-col lines
Task 7: complete (commits a0441bbe..b3c81a99, review clean)
Task 8: dispatched implementer (opus), BASE b3c81a99
Task 8: implementer DONE_WITH_CONCERNS commit 214c4d26 (linux-gcc 261/261, release 3/3, wine 174/174, pytest 13); manifest_core now links SP1 model_inventory (F-19 reuse)
Task 8: carry to Task 14: P1 relative-path grammar (device names, trailing dot/space) not yet enforced by validate_manifest (plan-mandated deferral) — Task 14 must close it so no caller can skip it; spec §2.4 pack-holds-index.txt check — confirm which later task enforces it
Task 8: minor (deferred): is_valid_model_name noexcept but allocates; header comment overclaims "every struct-level rule"; no non-collision tests (pack vs pack-x/, pack.bin) for lower_bound; collision reported after per-file errors
Task 8: complete (commits b3c81a99..214c4d26, review clean)
Task 9: dispatched implementer (sonnet), BASE 214c4d26
Task 9: implementer DONE commit d77097cc (linux-gcc 268/268, wine 181/181, release 3/3)
Ruling: Task 9 review Important (plan-mandated): with_installed leaves a dir both live and retired (reinstall/downgrade adopts a still-retired dir -> retire_unheld later deletes the live version) — FIX in with_installed (drop matching retired row, never double-retire) + regression case; the brief's code is the plan's argument, spec §4.5's single-live-version invariant binds — cost if wrong: a few lines in a pure function.
Task 9: minor (deferred): >120-col lines; staging_key/version_directory_name accept unvalidated input; is_staging_key_name re-rolls hex check; quoted() precondition undocumented; unused <algorithm>; render golden lacks retired/third_party; staging plan lacks wrong-size final / non-regular / zero-length .part cases
Task 9: fix round 1/5 (1 addressed, 0 open — with_installed live+retired invariant; commits d77097cc..8a28af98); regression case added (no RED captured for it — noted)
Task 9: complete (commits 214c4d26..8a28af98, review clean after 1 fix round)
Task 10: dispatched implementer (sonnet), BASE 8a28af98
Task 10: implementer DONE_WITH_CONCERNS commit b65ff7b2 (linux-gcc 274/274, wine 187/187, release 3/3); F-21 resolved locally (license fields print old->new); concern re control chars moot — manifest strings are printable ASCII (spec constants)
Task 10: minor (deferred): test fn outside anon namespace (acquire_test.cpp:395); unused <algorithm> manifest_diff.cpp; render_missing("")/parse_missing asymmetry + no sort check; per-file license lines interleaved with file changed lines
Task 10: minor (deferred, carry to Tasks 22/23/27): diff lines include `license url` — never write diff lines to acquire.log (§8.1 no URLs in logs)
Task 10: fix round 1 dispatched (resume a622f8083a13ddd1a): per-file license diff must use effective_license (bundle_allowed flip hidden)
Task 10: fix round 1/5 (1 addressed, 0 open — effective_license per-file diff; commits b65ff7b2..e3a6b45d)
Task 10: complete (commits 8a28af98..e3a6b45d, review clean after 1 fix round)
Task 11: dispatched implementer (sonnet), BASE e3a6b45d
Task 11: implementer DONE commit 66e7c345 (linux-gcc 278/278, wine 191/191, release 3/3); deviation push_back for GCC -Wstringop-overread FP
Task 11: minor (deferred): build_crt_fd_block no handle_width check (>8 => UB shift); readable-root check only one containment direction (allowed dir beneath protected?) — carry to Task 25 review; landlock_rights_for_abi(<=0) returns ABI1 set (Task 25 must gate on probe); literal 5 vs kCrtBlockEntries; >120-col lines
Task 11: complete (commits e3a6b45d..66e7c345, review clean)
Task 12: dispatched implementer (opus), BASE 66e7c345
Task 12: implementer DONE commit 5e1bd4a5 (linux-gcc 283/283, wine 193/193, release 3/3, pytest 13); F-22/23/24 applied
Task 12: complete (commits 66e7c345..5e1bd4a5, review clean) — minors logged on receipt
Task 13: dispatched implementer a07b0f9ee058de2fd (opus), BASE 5e1bd4a5
Task 12: minor (deferred, carry to Tasks 21/22/27/28): fake has no "stall" switch (spec §10.1 lists one; brief omitted) — a later stall test needs curl_scenario.hpp extended; probe falls back to default Scenario on malformed scenario; no probe-record or --proxy/--cacert grammar cases
Task 12: minor (deferred): spawn_with_fd4 waitpid/read no EINTR retry; Windows spawn_with_crt_entry4 pipe leak on 2nd CreatePipe fail, unchecked NUL handle, single WriteFile; double blank line fetch_test.cpp
Task 13: implementer DONE_WITH_CONCERNS commit eb6197f5 (pytest 20; no C++ change); F-26 applied; F-25 owner-confirm of iq3_xxs/iq3_s/iq1_m pins (PARKED for owner)
Task 13: complete (commits 5e1bd4a5..eb6197f5, review clean) — minors logged on receipt
Task 14: dispatched implementer (opus), BASE eb6197f5
Task 13: minor (deferred): F-26 empty-file check runs after shards are copied/moved (--move-shards moves ~68 GB before refusing) — move scan before copy; malformed pins -> traceback not exit 2; --ple-shard out of range silently drops ple_gguf; rebuild reuses sequence 1 (doc note); untested --move-shards/--ple-shard paths
Task 13: carry to Task 20: add a make_bundle.py-shaped manifest fixture parsed by the C++ parser (no C++ parse of its output exists yet)
Task 13: parked — F-25 pins for iq3_xxs/iq3_s/iq1_m need owner confirmation — Ruling: kept verbatim from plan; owner item, nothing downstream compiles against them — cost if wrong: wrong pin => dev bundle refuses to build (fails closed)
Task 14: implementer DONE commit 920a88cb (linux-gcc 290/290, release 5/5, wine 200/200, pytest 22); P1 moved into validate_manifest; .gitattributes -text added; CMake string(CONCAT) fix
Task 14: complete (commits eb6197f5..920a88cb, review clean) — minors logged on receipt
Task 15: dispatched implementer (opus), BASE 920a88cb
Task 14: minor (deferred, final-review triage, spec §2.5 rule 2): catalog-digest manifest ignores a mismatching typed --trust-manifest digest (manifest_trust.cpp:862) — no bypass, but operator typo unnoticed (plan-mandated)
Task 14: minor (deferred): CMake version regex looser than parse_engine_version, silent EngineVersion{} fallback (catalog.cpp:588); engine_version test tautological; unknown keys checked before schema_version gate (misleading message, same exit 5); resolve_catalog_manifest untested + alias targets unvalidated; test derefs after expect without guard; unused <sstream>; duplicate printable(); CMake guards for catalog dir names / non-ASCII bytes when first real entry lands
Task 15: implementer DONE commit 88f678ed (linux-gcc 293/293, release 5/5, wine 200/200, pytest 22); spec acceptance mapping still names secure_frontend_config_tests (docs, Task 29)
Task 15: complete (commits 920a88cb..88f678ed, review clean) — minors logged on receipt
Task 16: dispatched implementer (opus), BASE 88f678ed
Task 15: minor (deferred): no case for unset pull.downloader default / describe() pull.proxy; path::string() in new test code (non-ASCII TEMP); scan_model_root_files silently drops non-P1 entries (doctor/list --all — note for Task 23/27); >120-col lines; missing blank line typed_config_test
Task 15: open doc item for Task 29 (out of this run): spec acceptance mapping still names secure_frontend_config_tests (R-3 moved cfg01 cases to typed_config_tests)
Task 16: implementer DONE_WITH_CONCERNS commit 1fc7ae58 (linux-gcc 301/301, release 5/5, wine 200/200, pytest 22); F-34 via SECURE_FRONTEND_HAVE_MODELS_STORE guard (Task 17 must remove it); concern: rename fallback only on EINVAL/ENOSYS, not EOPNOTSUPP
Task 16: complete (commits 88f678ed..1fc7ae58, review clean) — minors logged on receipt
Task 17: dispatched implementer (opus), BASE 1fc7ae58
Ruling: Task 16 review Important (plan-level, cross-task): manifest files[].path may be 1024 B but every store/bundle use prefixes it (.staging/<32hex>/<path>.part, <name>/<tag>/v<seq>-<h12>/<path>, files/<path>) and P1 caps the whole relative path at 1024 B, so a valid manifest can make join_rel throw logic_error in Tasks 19/20/22 — DECIDED: validate_manifest (single choke point) rejects with manifest_invalid any file whose longest stored form exceeds P1's limit (compute against the real name/tag/sequence and the 32-hex staging key + ".part", the 12-hex version dir, and "files/"); spec's 1024 string cap stays as the outer bound; implemented in Task 18's dispatch (first consumer before 19) with a regression case — spec §2.2 requires paths to satisfy SP1 P1, which the stored form must also satisfy; fail closed at manifest time beats a crash at publish — cost if wrong: a few extreme-length manifests rejected that a different store layout could hold.
Task 16: minor (deferred): add EOPNOTSUPP to rename-fallback errno set (no test of errno-driven branch); preallocate EFBIG/EIO -> publish_failed (exit 7) vs spec §4.6 disk_space; PartFile methods usable after close() (stale fd); remove_file_within/remove_tree_within/exists_within treat any parent_fd failure as absent (rm may report success wrongly); pre-existing .lock mode not enforced 0600; list_files_recursive bundle_invalid in pull context -> Tasks 19/22 map reason
Task 17: implementer DONE_WITH_CONCERNS commit eda74120 (linux-gcc 301/301, release 5/5, wine 200/200, pytest 22); 9 windows-runtime cases CI-only; carry to Task 19/23: remove_tree_within cannot remove read-only (DACL/0500) dirs — rm/retire must restore write access first
Task 17: minor (deferred): Windows preallocation assertion can't fail (cached size); make_read_only_directory sets DACL by path (reparse at final component, parent unpinned); with_sharing_retries sleeps after 10th failure; reparse branch of remove_tree_within lacks sharing retries; free_bytes non-extended path (>MAX_PATH root); double PinnedParent
Task 17: fix round 1 dispatched (resume a540d5b97fc7df4e4): Windows .lock/.part owner+DACL privacy check parity with POSIX
Task 17: carry to Tasks 18-24b: under wine has_private_owner_dacl rejects even privately created files, so every case that writes the models store (creates .part/.lock) must be labelled ${SECURE_FRONTEND_WINDOWS_RUNTIME} on Windows
Task 17: fix round 1/5 (1 addressed, 0 open — Windows .lock/.part owner+DACL check; commits eda74120..f3526b39)
Task 17: complete (commits 1fc7ae58..f3526b39, review clean after 1 fix round)
Task 18: dispatched implementer (opus), BASE f3526b39 (carries the stored-path-length ruling)
Task 18: implementer DONE commit f62f1ef9 (linux-gcc 306/306, release 5/5, wine 204/204, pytest 22); ruling A in same commit; added kRelativePathMaxBytes to SP1 canonical_path.hpp (SP1 parser uses it)
Ruling: accept Task 18's tiny SP1 canonical_path edit (exposing kRelativePathMaxBytes, behaviour-identical) although not a named SP1 delta — it implements the stored-path ruling without a duplicated magic 1024; flagged for integration (possible conflict with main) — cost if wrong: trivial merge conflict / revert to a local constant.
Task 18: complete (commits f3526b39..f62f1ef9, review clean) — minors logged on receipt
Task 19: dispatched implementer (opus), BASE f62f1ef9
Task 18: minor (deferred): manifest_core includes acquire_core's bundle_text.hpp for kBundleFilesDir (layering inversion; move constant); kRelativePathMaxBytes placed between P1 comment and class; untested load_installed "names another model", read_model_index fail-closed paths, write >4 MiB refusal; bad-entry check asserts no reason; models_root helper std::get crash mode (plan-mandated); parse_model_index accepts tag "default"
Task 19: implementer DONE_WITH_CONCERNS commit b3aa7567 (linux-gcc 320/320, release 5/5, wine 205/205, pytest 22)
Ruling: my Task 19 carry note ("published version dirs are made read-only") was wrong — spec §4.3 says 0700 and no code makes them read-only; the implementer kept 0700 and made remove_tree_within restore owner write via the dir's own handle anyway (defence for user-made read-only trees); accepted — cost if wrong: none (extra robustness).
Task 19: store primitives now treat only a missing parent as absent (unreachable parent = publish_failed); adopt_orphan with mismatched manifest leaves a dir that blocks publish ("target exists") — carry to Task 23 doctor/rm
Task 19: complete (commits f62f1ef9..b3aa7567, review clean) — minors logged on receipt
Task 20: dispatched implementer (opus), BASE b3aa7567
Task 19: minor (deferred): Windows restore_owner_access duplicates PrivateSecurityAttributes SDDL and rewrites every dir's DACL (OICI propagation cost) — reuse + skip when already private; no Windows unreachable-vs-absent test; adopt_orphan hides read errors as "no orphan"; verify_tree loses I/O detail; sync failures drop disk_space reason
Task 19: carry to Task 22: InstallDecision::installed points into caller's ModelIndex — keep the index alive
Task 20: implementer DONE_WITH_CONCERNS commit 730e7aa8 (linux-gcc 339/339, release 5/5, wine 206/206, pytest 23); volume-root check also refuses Linux mount points; Windows open_part_file skips DACL check for external roots (FAT/exFAT)
Ruling: Task 20 I1 — read side (open_bundle) refuses only a lexical volume root (no relative path: "/" or "X:\"); the st_dev mount-point refusal stays on the write side only (export / pull --bundle targets) — make_bundle.py may legitimately write to an empty USB mount point and Review Focus 5 requires import from removable media to work; F-46 scoped the refusal to prepare_bundle_target — cost if wrong: importing from a mount-root bundle is allowed (read-only, no trust impact: digest verification unchanged).
Task 20: fix round 1 dispatched (resume aaccdd4801ea28397): I1 read-side mount refusal; I2 Windows drive-root target reported as "cannot be created" (CreateDirectoryW ACCESS_DENIED)
Task 20: minor (deferred): empty unknown dirs under files/ accepted; report claims names never echoed but entry.path appears in details (P1-valid, allowed by §7.3); failed export leaves target dir behind; external-root confinement rests on call sites only — guard import_bundle/lock_models_root against external roots (security-flavoured, final review triage)
Task 20: carry to Task 23: failed export leaves target dir (CLI message should tell user to remove it)
Task 20: fix round 1/5 (2 addressed, 0 open — read-side lexical-root only; Windows drive-root checked before create; commits 730e7aa8..b0cf6c6a); 2 minor test-robustness notes in rereview-20-r1.md
Task 20: complete (commits b3aa7567..b0cf6c6a, review clean after 1 fix round)
Task 21: dispatched implementer (opus), BASE b0cf6c6a
Task 20: minor (deferred): mount-root test child has no try/catch (exception would unwind into runner and could pass silently) — wrap with catch(...)->_exit(8); Windows folder-mounted volumes not refused as export targets; UNC share root may not count as drive root
Task 21: implementer DONE_WITH_CONCERNS commit d1614eca (linux-gcc 360/360, release 5/5, wine 206/206, pytest 23); carry to Task 22: plumb test_cacert (F-51) through fetch_settings_from/run_pull in hook builds; ScriptedTransport throws on unscripted requests
Task 21: complete (commits b0cf6c6a..d1614eca, review clean) — minors logged on receipt
Task 22: dispatched implementer (opus), BASE d1614eca
Task 21: minor (deferred): FetchTarget::resumed_from_disk dead field; untested L==size verify path, L>size reset, 503/429 same-source retry, source host_not_allowed, dns_failed, per-chunk interrupt; "f.bin: f.bin" duplicated detail; reset() failure early-return inconsistent; (void)remove_file_within after verification failure (plan-mandated); >120-col lines; weak tls_untrusted sleeps assertion
Task 21: carry to Task 27 (network verbs, out of this run): back-off sleeps neither capped to remaining run budget nor interruptible
Task 22: implementer DONE_WITH_CONCERNS commit f9fa43ed (linux-gcc 389/389, release 5/5, wine 206/206, pytest 23); carry to Task 23: CLI supplies Confirmer (stdin [y/N] via parse_confirmation, EOF=No), DownloaderProbe, hooks-only test_cacert flag
Ruling: Task 22 Important (plan-mandated) — update whose installed version cannot be loaded skipped the §6.1 license check so --yes could install under a changed license — DECIDED: treat the license as unknown: require an interactive y (yes_applies=false) with a fixed-text line that the installed version could not be read; --yes alone declines (exit 2); regression case corrupts installed manifest.json and expects exit 2 under --yes — spec §6.1 'license change needs interactive y even with --yes' binds; prompting keeps the repair path open vs hard exit 7 — cost if wrong: one extra prompt in a corrupt-install update.
Task 22: minor (deferred, final-review triage): pull --bundle creates target dir before [y/N] (F-56 "nothing written before the answer" — a declined bundle pull leaves an empty dir); all-present bundle asks nothing; two disagreeing free-space preflights for fresh bundle; tidy_staging/tree_bytes swallow list errors; format_gb locale-dependent (%.1f); no probe-failure (exit 9) / reuse-fallback / declined --bundle cases; pure parse_confirmation checks never run on wine; pull_harness transitive includes; max_duration_s starts after confirmation (spec §3.6 "whole run")
Task 22: fix round 1 dispatched (resume a7334e75c5c147d5c): license prompt fails open when installed version unreadable
Task 22: fix round 1/5 (1 addressed, 0 open — unreadable installed version => license unknown, interactive y required; commits f9fa43ed..bf9ffe0b)
Task 22: complete (commits d1614eca..bf9ffe0b, review clean after 1 fix round)
Task 23: dispatched implementer (opus), BASE bf9ffe0b
Task 23: implementer DONE_WITH_CONCERNS commit d46b4a74 (linux-gcc 405/405, release 6/6, wine 210/210, pytest 23); F-58: single engine_gate (checks family) + find_installed + engine_state_text in new engine_gate.hpp (model_store) — Task 24b must reuse, not redefine; F-57: shared report/usage/arg parser in src/strata_cli_internal.hpp for Task 27
Task 23: carry to Task 27 (out of run): include strata_cli_internal.hpp, don't redefine report/usage; Windows argv non-ASCII depends on code page (same as server)
Ruling: Task 23 I1 (plan-mandated, conflicts with C7 / Controller note 16 / spec §1.5, §6.2) — `strata --config f update` with no operands must exit 12 (program update unavailable) with no seams created, same as the bare form; FIX now in run_strata before network dispatch + test h.run({"update"}); carry to Task 27: its update branch must only reject non---models operands, never turn the no-operand form into exit 1 — owner decision C7 binds over plan code — cost if wrong: none (spec text is explicit).
Task 23: minor (deferred, carry to Task 27): report() orphan hint matches any publish_failed ": target exists" (also staging renames) — narrow to version-dir shape before pull reuses report(); CliServices::test_cacert present in release builds (Task 27 must only set it under hooks)
Task 23: minor (deferred): engine_gate family-mismatch detail shows profile name; replaced() test helper unchecked find
Task 23: fix round 1 dispatched (resume a32e0869bf3dfc400): update with no operands -> exit 12
Ruling: spec §4.5 state 'needs pack (bundle)' is unreachable for list (publish requires every file verified; sourceless pull stops at manifest_requires_bundle) — no code; note for spec/Task 27 doctor — cost if wrong: a state string never shown.
Task 23: fix round 1/5 (1 addressed, 0 open — update no-operand exit 12, checked before config load; commits d46b4a74..9802aaab)
Task 23: complete (commits bf9ffe0b..9802aaab, review clean after 1 fix round)
Task 24a: dispatched implementer (opus), BASE 9802aaab
Task 24a: implementer DONE commit 94e4217f (linux-gcc 406/406, release 6/6, wine 210/210, pytest 23); F-59/60/61 applied
Task 24a: complete (commits 9802aaab..94e4217f, review clean) — minors logged on receipt
Task 24b: dispatched implementer (opus), BASE 94e4217f
Task 24a: minor (deferred/carried to 24b): no direct --expert parse / describe (unset) / not-set guard / empty-half name:tag cases; >120-col lines; temporary 'arrive with SP4 Task 24b' stderr text and README ahead of behaviour (sent to 24b implementer)
Task 24b: implementer DONE_WITH_CONCERNS commit c0138061 (linux-gcc 416/416, release 6/6, wine 212/212, pytest 23); F-7/F-8/F-58/F-62/F-63 applied; 5 existing --check runs now pass --expert; no e2e managed serve test
Task 24b: complete (commits 94e4217f..c0138061, review clean) — minors logged on receipt
Task 24b: minor (deferred): compose_engine_launch engine-gate failure / ambiguous name / tokenizer is_within refusal not tested through compose; no e2e managed serve run; misleading test comment (launch_refuses_model_not_installed); engine.model_id issue text says "lowercase" though uppercase is case-folded; redundant validate_engine_user_arguments after index read (move into launch_config_issues or drop)

## Final whole-branch review
Dispatched final reviewer (opus) over b1609876..c0138061 (31 commits); result file final-review.md
Final review verdict: With fixes — C1 import --yes license change fails open; I1 publish failure after step 2 makes staging look stale (data loss on rerun); I2 rm on Windows leaves unmanaged orphan (contradicts note 5) + adopt_orphan verify failure fails verb; I3 case-variant directory components accepted (Review Focus 2); minors triaged in final-review.md (FIX-MERGE: M1 typed digest vs catalog, M10 EOPNOTSUPP cheap, Task 19 adopt_orphan with I2)
Ruling: final fix wave = C1, I1, I2 (+adopt_orphan not-adoptable), I3, M1, M10 in ONE dispatch; everything else per the reviewer's triage (FIX-27 / CARRY-25/28 / WAIT) is carried in the final report — reviewer's triage aligns with spec and owner rules — cost if wrong: deferred items surface in Task 25-29 reviews.
Ruling: M2 (update-reuse preflight omits copied bytes; failed reuse copy silently becomes an unconfirmed download) — CARRY to Task 27 with a ruling recommendation: count reused bytes in the free-space preflight and treat a failed reuse copy as a fetch only if the confirmation covered it — spec §4.6 text also misses it; Task 27 owns the first live pull — cost if wrong: a spurious disk_space or an extra unprompted download until Task 27.
Final fix wave: dispatched (opus), FIX_BASE c0138061
Final fix wave: DONE_WITH_CONCERNS commit f0b09e2e (linux-gcc 422/422, linux-gcc-release 412/412 full / 6/6 labels, wine 213/213, pytest 23); concerns carried: run_pull adopts orphan before license questions (Task 27), rm --partials misses manifest.json staging key, rm failed-delete no 'stays retired' hint, new Windows rm case CI-only
Final fix wave: re-review — all 6 findings ADDRESSED, no new Critical/Important; 1 minor (orphan hint after a partially failed rm followed by re-import) carried
Final review: complete (fix wave c0138061..f0b09e2e)
