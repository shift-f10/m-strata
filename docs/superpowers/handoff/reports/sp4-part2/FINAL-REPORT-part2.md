# SP4 part 2 — final report

**Status: COMPLETE (Tasks 25–29)**
Branch `sp4-acquire-part2`, worktree /opt/projects/ai/.worktrees/sp4b, base 557a2cc7, **HEAD 9044a5bc**, tree clean. Not pushed, merged or rebased.

## Commits (`git log --oneline 557a2cc7..HEAD`)
- 9044a5bc sp4: final-review fixes — interruptible hashing and copying, probe interrupt exit 8, update --models interrupt exit, SP1 SID literal (SP4 final review; spec §8.3)
- 53dc2c7f docs: SP4 threat-model amendment, catalog provenance, handoff verification (SP4 Task 29; spec §7.4, §13.5)
- e7a16999 sp4: Task 28 fix round 1 — positive controls for the Windows socket tables, the link-set check and every audited libc call (SP4 Task 28; spec §10.1, §10.3)
- 68273f72 sp4: DL-01 and PRIV-01..04 lifecycle proofs, release-only checks and the hooks marker scan (SP4 Task 28; spec §7, §10.3, §10.5)
- 1582fba4 sp4: strata signals end to end and interrupt at the confirmation prompt (SP4 Task 27 fix round 1; spec §8.2, §8.3)
- ecff71dc sp4: strata pull, run (D6), update --models and doctor with interrupts and log records (SP4 Task 27; spec §1.3, §6.1, §7.3, D6)
- a79e8b02 sp4: Task 26 fix round 1 — downloader validation on SP1 open_executable (TrustedInstaller option), UTF-8 argv (SP4 Task 26; spec §3.2)
- 563dd0e9 sp4: confined curl downloader on Windows: Low-integrity token, Job, explicit handles (SP4 Task 26; spec §3.2, §11)
- 06630dbd sp4: confined curl downloader on Linux via the SP2 pre-exec seam (SP4 Task 25; spec §3.1-§3.3)

## Final test counts
These are the fix agent's numbers at 9044a5bc. Task 29 independently re-ran the suites at e7a16999.

| Run | Result |
|---|---|
| linux-gcc ctest | 560/560 (+2 known skips: proc03_other_uid_cannot_open_engine_fd, proc05_cgroup_kill_when_delegated) |
| linux-gcc-release | 526/526 full; 10/10 with `-L "release-only|release-also"`; release staging marker scan PASS |
| wine (windows-llvm-mingw-cross) | 260/260; cross build 0 warnings |
| pytest | 25 |

Warnings: 0 on every preset.

Real Windows: CI run 38625663621 on base 557a2cc7 is 488/488. It is cited in the handoff only for code that Tasks 25–29 did not change. Every new Tasks 25–28 windows-runtime case is **pending MSYS2**: the Windows downloader cases, the Ctrl-Break DL-04 case, the Windows DL-01 TCP/UDP table cases and the new acquire interrupt cases.

## Reviews
- Every task had a spec and quality review.
- Fix rounds:
  - T26: 1 round (SP1-rule validation, duplicated ACL check, UTF-8 argv).
  - T27: 1 round (Linux signal test; Ctrl-C at the [y/N] prompt exits 8).
  - T28: 1 round (positive controls for the Windows tables, the link set and every shim hook).
  - T25 and T29: clean on first review.
- **Final whole-branch review:** "With fixes".
  - No Critical findings.
  - Important F1: Ctrl-C was ignored for minutes during whole-file hashing and copying. Fixed.
  - Important F2: the Windows downloader runtime has never run. This is a gate we cannot clear, because we trigger no CI.
  - One fix wave (9044a5bc) fixed F1, F3, F4, F6 and F7, plus two cleanups:
    - C1: SP1 no longer includes an SP4 header (a local SID literal is used instead).
    - C2: Windows downloader owner text.
  - The scoped re-review found every finding addressed and no new breakage.
- **Residual findings, all left:**
  - No in-process interrupt case for the two re-hash paths; the unit case and the shared helper cover them.
  - Plain `doctor` swallows SIGINT (exit 0).
  - F4 has no test; the catalog has only one entry.
  - F5: Windows Ctrl-C at [y/N] may race the console-handler thread. Noted as pending MSYS2.
  - All other deferred minors were triaged "leave" by the final reviewer: covered by a ruling or the handoff, test hygiene only, or docs wording only.

## Merge gate for the controller
Run MSYS2 CI on `sp4-acquire-part2` before or at the merge. None of the following has run yet:
- the Windows downloader runtime: Low-integrity token, `launch_in_job`, `System32\curl.exe` accepted through TrustedInstaller, the Job process limit vs conhost, access to `.staging/.cwd`;
- the Ctrl-Break and [y/N] interrupt paths;
- the DL-01 table checks.

All of these fail closed, but if any one fails, every Windows pull fails.

The handoff leaves the `strata` UTF-8 argv row open, because no test at 557a2cc7 launches the `strata` binary. Confirm it from the CI log if you have evidence otherwise.

## Rulings I made (in order; each with its cost if wrong)
- Ruling: final report kept at the brief's path inside this workspace, so the workspace is NOT deleted at finish — the controller's brief names that path — cost if wrong: a stale git-ignored directory to delete by hand.
- Ruling: B1 downloader cwd never created for doctor/--bundle — Task 27 `fill_kit` ensures `roots.models/.staging` (ensure_directory) then `.staging/.cwd` (make_read_only_directory, as acquire_pull does) before any downloader is built for probe or transfer — spec §3.2 cwd row binding — cost if wrong: one extra directory creation on doctor.
- Ruling: B2 Landlock ruleset-creation failure on a Landlock kernel → Task 25 returns AcquireError{downloader_confinement_failed} without spawning (exit 9); never execs curl unconfined after announcing `full`; add a hooks test if a seam exists cheaply — spec §3.2 + R7 — cost if wrong: pulls fail on kernels with odd Landlock (fail-closed, visible).
- Ruling: B3 Task 26 links secure_frontend_downloader (WIN32) to secure_frontend_engine_stdio_process + advapi32 (userenv only if actually used) — launch_in_job lives there — cost if wrong: none (server already links it).
- Ruling: B4 Task 27 fetch_cli_test.cpp includes support/fake_curl_support.hpp — compile fix — none.
- Ruling: R1 Task 27 do_pull copies services.test_cacert into PullRequest::test_cacert under SECURE_FRONTEND_TEST_HOOKS; no new CLI option — keeps F-51 seam alive — cost if wrong: trivial.
- Ruling: R2 Task 25 reuses SP2 posix_launch::read_report/classify_report; classify_spawn_report(string_view) stays as the public thin mapping (hook_failed→confinement_failed; exec ENOENT→missing; EACCES→untrusted_path; other/malformed→unsupported) — no duplicated logic — cost if wrong: small refactor.
- Ruling: R3 Task 27 reuses strata_cli_internal.hpp helpers (parse_args, usage, report, trust_option, loaded_for, store_for) instead of re-creating them — DRY, same behaviour — cost if wrong: small.
- Ruling: R4 Task 25 confinement() plans with std::filesystem::canonical(executable, ec), falling back to lexically_normal(absolute) only when canonical fails — spec §3.2 canonical parent — cost if wrong: none.
- Ruling: R5 record file opened O_APPEND|O_CREAT|O_NOFOLLOW|O_CLOEXEC once per spawn (Windows OPEN_ALWAYS), not O_EXCL; Task 29 records this as a spec §10.1 text deviation in the handoff — one file serves probe+transfer — cost if wrong: low (tests use fresh scratch).
- Ruling: R6 Task 28 positive control priv01_positive_control_doctor_downloader_probe_is_recorded also runs under the connect-audit shim on Linux and asserts an empty audit (extra ARG $<TARGET_FILE:secure_frontend_connect_audit>) — spec §10.3 PRIV-01 — cost if wrong: one extra ARG.
- Ruling: R7 Task 28 DL-01 Windows also enumerates GetExtendedUdpTable (UDP_TABLE_OWNER_PID) for the pid — spec §10.3 — cost if wrong: trivial.
- Ruling: R8 Task 27 extends fetch.confinement_reduced_when_root_beneath_allowed_dir: config inside the downloader dir → `reduced:readable:config file`, and a hooks pull through the fake with roots.logs set asserts `confinement=reduced:readable:…` in acquire.log — spec §10.3 "announced and logged" — cost if wrong: a longer test.
- Ruling: R9 Task 28 priv01_local_verbs_never_spawn_downloader asserts each verb's expected exit code and adds one `update --models` against an outdated fixture catalog model answered No — spec PRIV-01 "update declined" — cost if wrong: test effort only.
- Ruling: R10 Task 28 lifecycle cases call wait_engine_ready() and assert a 200 chat; crash case polls /health until a restart is observed (not-ready then ready) before auditing — tests must prove what they claim — cost if wrong: flakier timing; use generous timeouts.
- Ruling: R11 Task 27 verb_run unknown model → report(io, AcquireError{AcquireReason::model_unknown, ref}) + `strata import` hint line — spec §8.1/§8.2 — trivial.
- Ruling: R12 acquire.log keeps "one record per transferred file"; Task 29 notes the narrower reading in handoff Known small gaps — Task 22 code untouched — cost if wrong: log completeness gap for reused/bundle files.
- Ruling: informational items folded into dispatches — I2 close WinLaunched::thread (binding); I5 strata_main.cpp edit adds only SIGPIPE ignore, keeps utf8 argv; I7 fix record-fd comment; I8 downloader-specific fixed error text; I9 check optional before deref; I13 Forward/ForwardProbe adapters into tests/support/pull_harness.hpp + one verb_run tail helper; I17 shim treats non-INET families as local; I18 add_dependencies complete; I20 keep merged label `reduced:readable:<key>` and note spec-text inconsistency in handoff; I21 real handoff headings, add a part-2 verification record; I12 accept quoting copy; I15 Windows Ctrl-Break case recorded as pending MSYS2 CI; I1/I3/I4/I6/I11/I16/I19/I22/I23 no action.
- Ruling: classify_spawn_report stays POSIX-only (Windows has no report pipe; Task 26 maps launch_in_job DWORD failures directly) — interface amended rather than adding a dead Windows symbol — cost if wrong: one declaration to add later.
- Ruling: I2 plan-mandated duplication — fix it: extend the shared SP1 helper additively (e.g. an allow_trusted_installer parameter defaulting to false, or a principal-set overload) so every existing caller is unchanged; spec binding ("same rules" + TrustedInstaller) outranks the plan's copy and the "never edit SP1 files" constraint is honoured in spirit by an additive, default-preserving change — cost if wrong: one small SP1-file touch the controller must notice at merge.
- Ruling: I1 — validate_downloader builds on SP1's open/normalise/classify path (GetFullPathNameW, classify_final_path rejecting UNC/unsupported roots, FILE_TYPE_DISK, prefix-stripped result), with additive SP1 exposure only where needed — spec §3.2 "same rules as engine.executable" — cost if wrong: small SP1 surface growth.
- Ruling: I1 plan-mandated missing test — add a Linux hooks case (strata child, fake curl delay-ms, SIGINT after records → exit 8, resume hint, no index.json, no surviving curl; plus SIGTERM) — spec §8.3 and TDD constraint outrank the plan's omission — cost if wrong: one slower test.
- Ruling: Ctrl-C at the [y/N] prompt (brief: EOF → No, exit 2; spec §8.2: SIGINT → interrupted, exit 8) — follow the spec: SIGINT/SIGTERM at the prompt exits 8 (`interrupted`), spawning nothing; Ctrl-D/EOF remains No (exit 2) — spec is binding — cost if wrong: an exit-code difference on a user abort at the prompt.
- Ruling: R10 crash case uses SIGSEGV to the fake engine after READY (not the brief's --crash-after-ready=9, under which the fake stops reading stdin so a 200 chat is impossible), asserting the supervisor's "engine generation 1 ended: crashed" and /health not-ready→ready — a real crash + observed restart satisfies R10 — cost if wrong: crash path differs from the supervisor's own crash flag.
- Ruling: UDP DL-01 check fails on any non-loopback binding incl. 0.0.0.0 (R7 as written) — serve opens no UDP sockets — cost if wrong: a red MSYS2 case to relax.
- Ruling: I1–I3 plan-mandated vacuity — fix all three: spec §10.1 demands positive controls; proofs must not pass vacuously — cost if wrong: modest extra test code.
- Ruling: strata UTF-8 argv handoff row left OPEN despite the coordinator's note (no test at 557a2cc7 launches the strata binary; in-process run_strata only) — honest evidence over a broad claim; controller to confirm from the CI log — cost if wrong: one row stays pending longer.
- Ruling: fetch.release_spawn_has_no_extra_handle_windows treated as SP7-deferred (as the docs record) — release Windows hook-free proof needs the SP7 release job — cost if wrong: a spec §10.5 case missing on Windows until SP7.
- Ruling: F1 fix via option (a): thread the interrupted flag into hash_into/copy_verified chunk loops → AcquireReason::interrupted (exit 8), with a test — exit-8 contract (§8.3) and prompt response; acquire_pull.cpp is SP4's own Task 22 code, not SP1/SP2 — cost if wrong: small per-chunk atomic load.
- Ruling: F2 cannot be done by us (no CI triggers allowed); surfaced to the controller as a merge gate: run MSYS2 on sp4-acquire-part2 before merge — cost if wrong: Windows pulls fail closed until fixed.
- Ruling: final fix dispatch also takes F3, F4, F6, F7 and the two recommended deferred cleanups (SP1 windows_security.cpp local TrustedInstaller SID literal instead of including SP4 confinement_plan.hpp; Windows-worded untrusted_owner text). F5 tracked with the MSYS2 run (needs real Windows). F8: ledger correction recorded here; no doc change.

## Parked items / concerns
- **No task hit the 5-round breaker.** Nothing was parked at the cap.
- **SP1 files touched**, additively and keeping existing behaviour (Task 26 plus the final fix):
  - `canonical_path*`: `ExecutableWriters::also_trusted_installer`.
  - `windows_security.*`: `write_access_is_private(…, also_trusted_installer=false)`, `open_existing_root(…, false)`, and a local TrustedInstaller SID literal.
  - The controller should note these at merge.
- **Not touched:** no SP2 file and nothing under `upstream/`.
- **Owner pointer from the final review:** PROVENANCE and `qwen_bundle_pins.json` (part-1 data) show the same shard-2 digest (`316b46f3…`) for all three quants and for the Coder repo. It is already marked "owner to confirm"; it is worth checking specifically.
- **Deviations from the spec text** (documented in the handoff):
  - Record file opened append/create, not `O_EXCL`.
  - `acquire.log` writes one record per *transferred* file.
  - The label form `reduced:readable:<key>` contains a space in `config file`.
  - `fetch.release_spawn_has_no_extra_handle_windows` is deferred to SP7.
- **Workspace kept:** the ledger, briefs, reports and review packages stay in this git-ignored workspace, because this report lives there.

## Addendum: merge-gate fix (2026-10-15)
CI run 38655938748 on 9044a5bc was green except for MSYS2, which passed 515/516. The one failure was `privacy.priv04_secure_server_does_not_link_acquisition`. Every other windows-runtime case from Tasks 25–28 passed on real Windows, including all of the downloader and dl01 cases.

- **Root cause:** native-Windows CMake writes `file(WRITE)` output in text mode, so the link-set files had CRLF line endings. The test matched `"name\n"`, which can never match a CRLF line. One side effect: the "server does not link X" checks had been passing vacuously on Windows.
- **Fix:** commit **13f297a1**, on top of 9044a5bc.
  - The link sets are now written with LF on every host (`file(CONFIGURE … @ONLY NEWLINE_STYLE UNIX)`).
  - The test parses the files line by line, ignores a trailing `\r`, and checks exact membership.
  - On failure it prints the path, whether the file exists, its size, the entry count and the first entries.
  - The positive controls are stricter than before, and none is skipped on Windows.
- **Handoff:** records the run, and flips the cases that passed in it to "verified by 38655938748". PRIV-04 and the Ctrl-C at `[y/N]` race stay pending.
- **Local tests at 13f297a1:**
  - linux-gcc 560/560 (+2 skips)
  - linux-gcc-release 526/526, and 10/10 on the release labels
  - wine 260/260
  - pytest 25
- **Review:** the scoped re-review found the fix ADDRESSED with no new breakage. Two cosmetic minors were left: the diagnostics show only the first 6 entries, and a failed `file_size` prints the maximum integer.
- **Still needed:** an MSYS2 re-run of 13f297a1 to confirm the fix on real Windows.

New HEAD: **13f297a1**.
