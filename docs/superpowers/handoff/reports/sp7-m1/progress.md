# SP7 M1 — service verbs, uninstall, full doctor — lane ledger

Branch `sp7-m1-service-doctor`, worktree `.worktrees/sp7m1`, base `9b467e08`.
Plan: `docs/superpowers/plans/2026-10-22-sp7-m1-service-uninstall-doctor.md`.
Spec: `docs/superpowers/specs/2026-10-22-sp7-release-packaging-service-design.md` (D-1, D-5, D-6 amended, D-7 binding).

## Pre-flight conflict scan (plan vs spec vs code, at base)

| # | Apparent conflict | Resolution |
|---|---|---|
| 1 | Plan context "token/config/state roots come from SP1 typed config (`paths.*`)" — the typed config's members are `roots.*` (frontend_config.hpp `Roots`), not `paths.*`. | Use `config.roots.*` (models/secret/state/logs/cache) + the `--config` file path. Naming-only. |
| 2 | Plan context "engine profile carries GPU/driver expectations from the manifest (SP4)" — verified: `AcquireManifest` carries `profile`/`family`/`min_version` only, no GPU/driver fields. D-7 says "values from the engine profile". | R-T3-a: expectations live in a per-profile `ProfileExpectations` compiled into `secure_frontend_doctor`, seeded from the pinned upstream docs (driver ≥ 580, upstream/docs/INSTALL.md:29 at pin `cbfd5b92`; memlock unlimited per upstream/docs/DOCKER.md `--ulimit memlock=-1:-1`; CUDA runtime candidates `libcudart.so.13`/`libcudart.so.12`), injectable via `ProbeBundle`. |
| 3 | D-5 writes the unit at "~/.config/systemd/user/strata.service"; systemd's user manager actually reads `$XDG_CONFIG_HOME/systemd/user` (fallback `~/.config/systemd/user`). | R-T2-a: resolve the unit dir by systemd's own rule (`XDG_CONFIG_HOME`, else `HOME/.config`); the literal spec path is the fallback case. One documented env read, recorded R-T2-b as an exception to "the CLI reads no environment variable value" (the unit location cannot come from anywhere else; the value goes only into path construction, never printed). |
| 4 | Plan Task 2 RED: "runner reports failure on `--user is-enabled` probe → unit NOT written". A real `systemctl --user is-enabled strata` also fails on a RUNNING manager when the unit is not yet written (fresh enable). | R-T2-c: keep the plan's probe command; classify "manager missing" only when the runner's stderr contains the fixed systemd connect-failure text ("Failed to connect to bus"); other probe failures (unit unknown/disabled) proceed to write+enable. Fake runner + PATH shim both drive both branches deterministically. |
| 5 | PX §6: Windows task "not run with highest privileges"; upstream DETAILS.md:667-670 warns the two settings include "Run with highest privileges". D-5 keeps both phrasings. | R-T6-a (Task 6): render Priority=Normal and run level LIMITED (never elevate — PX §6 "Never elevates" + D-5 "not elevated" win); the refusal of the elevation half is explained in fixed text. |
| 6 | Plan: "production wires argv exec via the existing process helpers". The existing launch helpers (`posix_child_launch.hpp`, `win_child_launch`) are engine-specific (fd canaries, Job objects, env allowlists) and unusable for `systemctl`. | R-T2-d: `service_ops` owns a minimal argv executor (POSIX spawn/execvp path; Windows CreateProcessW branch compile-proven only). PATH shim works because it searches PATH. |
| 7 | D-6 says "state-dir lockfile … reuse"; the plan's own context corrects it: NO server lockfile exists (Ruling T13-f). | Follow the plan: the running `serve` is found by the loopback health probe only; no pidfile/lockfile is read or written. (Task 7 folds this amendment into the spec text.) |
| 8 | "the 3 roots" for `ReadWritePaths` (D-5) and "three allowed roots" (D-6) are not enumerated. | R-T1-a: ReadWritePaths = the write-targets of `serve`: `roots.state` (always; serve requires it) plus `roots.logs` and `roots.cache` when configured (golden test pins all three). R-T4-a: uninstall's allowed roots = exact unit path, `roots.secret` (token lives below it), `roots.state`, plus the exact `--config` file path; everything else refuses. |
| 9 | Common brief said wine smoke + baselines "111/111, 102/102" — stale; my lane brief's baseline is linux-gcc registered 830, 4 allowed env skips, release 10/10, pytest 33, cross build only (no wine runs). | Lane brief is authoritative (it already says the brief's pin note is stale). |

## Task log

### Task 1 — service_ops: unit rendering

Files: `src/service_ops.hpp/.cpp` (new lib `secure_frontend_service_ops`), `tests/service_ops_test.cpp`, CMake wiring (+5 cases).

- RED: `ctest -R service_ops` → 4/5 Failed against not-implemented stubs (`render_systemd_unit_exact_golden`, `render_systemd_unit_rejects_whitespace_paths`, `resolve_current_target_follows_current_pointer`, `resolve_current_target_plain_install_uses_self`); the rejects-relative_or_empty case passes trivially on an error-everything stub (expected).
- GREEN: same 5/5 pass.
- Gates: linux-gcc build 0 warnings; ctest 831/831 100% (4 allowed env skips: cfg03_mount_crossing, proc03_other_uid, proc05_cgroup_delegated, bundle_at_mount_root; registered 826→831). linux-gcc-release 0 warnings, 10/10. windows-llvm-mingw-cross build 0 warnings (test exe cross-compiled; cross tests never run). pytest 33 passed.
- R-T1-a (pre-flight #8): golden unit uses ReadWritePaths = roots.state + roots.logs + roots.cache (serve's only write-targets; models/secret stay read-only under ProtectHome=read-only). R-T1-b: `render_systemd_unit` returns `variant<string, ServiceError>` (the plan's negative case needs the error to surface); errors are fixed text, never echoing a path. R-T1-c: `resolve_current_target` resolves a versioned install (`<root>/<version>/bin/strata` + `current` symlink/current.txt naming that version) to the canonical versioned path; plain installs (or a version the pointer left) honestly report their own canonical path — D-4's "not a versioned install" refusal belongs to `strata update` (M2), not the service verbs.
- R-T1-d: the lane brief says "commit the ledger with each task commit", but repo `.gitignore` deliberately ignores `/.superpowers/` ("superpowers execution state") and no lane has ever tracked it. Resolution: the ledger lives untracked in the worktree (survives for resume); no force-add of an intentionally ignored path. Noted for the controller.

### Task 2 — service enable/disable/status

Files: `src/service_ops.{hpp,cpp}` (+`run_argv`, `CmdResult`, `CommandRunner`, `ServiceEnv`, `ServiceOutcome`, `plan_enable/plan_disable/plan_status`), tests (+8 cases), CMake (+8).

- RED: `ctest -R service_ops` → 7 of 8 new cases Failed against not-implemented stubs (enable-writes, missing-manager, failed-enable, disable-removes, status-lines, status-no-manager, e2e-path-shim); `service_disable_refuses_foreign_unit` passes trivially on an error-everything stub (expected, like Task 1's).
- GREEN: 13/13 service_ops cases. (One test-side fix during GREEN: `CommandRunner runner{fake}` COPIES the recorder so recorded argv vanished — switched to `std::ref(fake)`. The tested behavior contract (exact probe→write→enable ordering) was unchanged; only the fake's plumbing was wrong. R-T2-e.)
- Gates: linux-gcc build 0 warnings; ctest 839/839 100% (831→839 registered; four allowed env skips only). release 0 warnings, 10/10. windows-llvm-mingw-cross build 0 warnings — the Windows `run_argv` (CreateProcessW PATH search) compiles; cross tests never run. pytest 33.
- R-T2-a/b as pre-flight (#3/#6): unit dir = XDG_CONFIG_HOME else HOME/.config, plus /systemd/user (computed by the verb, Task 5); `run_argv` is the minimal argv executor (fork/execvp + pipe pump on POSIX, CreateProcessW on Windows).
- R-T2-c as pre-flight (#4): probe `systemctl --user is-enabled strata`; "no user manager" = exit<0 (unstartable) or stderr containing "Failed to connect to bus"; the fresh-install "No such file or directory" answer proceeds. Success path asserts: exactly probe → write(0644, atomic tmp+rename) → `enable --now`; missing manager writes nothing (no dir, no file) and the message carries the `loginctl enable-linger` note it never runs.
- R-T2-f: `plan_disable` refuses BEFORE running anything unless unit_path == user_unit_dir/strata.service (foreign system-scope paths and renamed files untouched); a failed `disable --now` removes nothing.
- R-T2-g: status = exactly two read-only probes, one line each ("service: enabled: …" / "service: active: …"); unreachable manager is said, not guessed.
- The Windows branch of the three plans returns "not wired yet (Task 6)" for now — honest, compiled, replaced in Task 6.
- e2e note (R-T2-e cont.): the Task 2 PATH-shim e2e runs plan_enable in-process with PATH=shim-dir-only (real systemctl unreachable by construction); the temp-HOME CLI e2e is Task 5's.


### Task 3 — doctor: the full D-7 check set

Files: new `src/doctor_checks.{hpp,cpp}` (library `secure_frontend_doctor`), `src/strata_cli_network.cpp` (verb_doctor composes checks + production probes, `--deep` flag), new `tests/doctor_checks_test.cpp` (10 cases), legacy doctor-expectation amendments in `tests/fetch_cli_test.cpp`, `tests/fetch_release_test.cpp`, `tests/privacy_test.cpp`, CMake (+10 cases, +1 lib).

- RED: `secure_frontend_doctor_tests` registered against not-implemented check stubs → 9 of 10 cases Failed (the legacy CLI doctor answered every new case with its old fixed verdict line; `doctor_human_fix_never_carries_install_commands` passed trivially).
- GREEN: 10/10 doctor cases; full battery green.
- Gates (authoritative re-run in `out/build/<preset>` with the CTest presets): linux-gcc build 0 warnings, `ctest --preset linux-gcc` 852 registered / 849 run / 100% passed / 4 allowed env skips (839→852 registered). linux-gcc-release build 0 warnings, full `ctest` 815/815 100% incl. the release-only proofs. windows-llvm-mingw-cross build 0 warnings (cross tests never run). pytest 33 passed.
- Gate-method correction for the controller: the presets put build trees in `secure_frontend/out/build/<presetName>` (CMakePresets.json line 12) and the debug CTest preset excludes the 3 `release-only` cases (CMakePresets.json ~line 115); a bare `ctest --test-dir <dir>` in the hooks-ON tree runs those two self-guarding release proofs and shows 2 false failures. Commands used: `cmake --preset <p>` / `cmake --build --preset <p>` / `ctest --preset <p>` from `secure_frontend/`.
- R-T3-a: GPU/driver expectations are a compiled-in per-profile `ProfileExpectations` (min driver "580" per upstream INSTALL.md, memlock unlimited, CUDA runtime libs libcudart.so.12/13) — the shipped manifest carries no such fields (the plan's Task-3 context line claiming otherwise is false for this base; checked `profiles/glm53.json`).
- R-T3-b: the three legacy doctor tests that asserted the fixed exit-0 contract are amended to verdict equivalence: `(exit==0) == output contains "doctor: all checks passed"`. D-7 replaces the old always-0 contract; no probe-behavior assertion was weakened (downloader-probe, privacy-presence and env-knob cases keep their exact expectations).
- R-T3-c: Windows memlock check passes as not-applicable (`memlock_applicable=false` in the bundle); R-T3-d: `roots.state` is only existence-checked when the config names it and the path can exist — a fresh install with no state dir is not a failure.
- Verdict contract (locked by golden-ish assertions): `doctor <name>: ok` / `doctor <name>: <human fix>` lines, check order gpu,driver,cuda,config,token,roots,port,memlock (+model digests under `--deep`), then `doctor: all checks passed` (exit 0) or `doctor: one or more checks failed` (exit 1). Fix lines never carry install commands (D-7).

### Task 4 — uninstall plan (planner + loopback health probe)

Files: new `src/uninstall_plan.{hpp,cpp}` (library `secure_frontend_uninstall`, LINKS typed_config; ws2_32 on WIN32), new `tests/uninstall_test.cpp` (9 cases incl. canned-HTTP-listener probe e2e), CMake (+lib, +9 cases).

- RED: `ctest -R uninstall` → 9/9 Failed against not-implemented stubs (every decision-table, purge-gate, escape, metadata case, plus the probe-up case the false-stub also failed).
- GREEN: 9/9 pass.
- Gates: linux-gcc build 0 warnings; `ctest --preset linux-gcc` 861 registered / 858 run / 100% / 4 allowed env skips. linux-gcc-release 0 warnings, full ctest 824/824 100%. windows-llvm-mingw-cross 0 warnings — the cross gate caught two real portability errors, fixed before commit: `path::native()` is `wstring` on Windows (containment now compares path COMPONENTS), and `SOCKET` is unsigned (`NativeSocket{-1}` narrowing + `<0` accept check guarded per platform). pytest 33.
- R-T4-a: stop table — no listener → `none`; listener + our unit + manager reachable → `via_service_manager`; listener + no unit → `foreground_running_refuse` with the Ctrl+C line; listener + unit but NO manager → refuse (can't run the service stop; R-T4-a is this fourth-row ruling — the plan's table left it open).
- R-T4-b: empty config_path or state_root → refuse ("run it with the --config you use"); the planner never guesses mandatory paths. Optional paths (unit not installed, env: token with no file) are simply skipped in the removal list.
- R-T4-c: refusal order is metadata → mandatory-path → allowed-root escapes → purge typed gate → stop table; the escape refusal echoes the offending path (user-typed config data, honest to show).
- D-6 kept-list: models + logs printed; `--purge` moves models to removals (last) but logs are always kept. Typed-gate deny text names both "type" and "--yes" (the CLI must obtain the typed word in Task 5; `--yes` sets only the ordinary no-prompt flag).
- Probe: `server_answers_on(IpAddress, port)` — connect + `GET /health`, any "HTTP/" reply (incl. 401) = listening; 2 s recv/send timeouts. e2e runs a real canned 401 listener on 127.0.0.1 and asserts false after close (fake engine server up/down, per plan).

### Task 5 — CLI wiring: service {enable,disable,status}, uninstall [--purge]

Files: `src/strata_cli.cpp` (usage lines + verb dispatch), `src/strata_cli_network.cpp` (verb_service, verb_uninstall, user_config_home, self_executable, run_network_verb dispatch), `src/service_ops.{hpp,cpp}` (+exported `user_manager_reachable`), new `tests/cli_verbs_test.cpp` (7 e2e cases spawning real `strata` children; POSIX-gated where systemd-shaped), `tests/support/process_runner.hpp` (ProcessSpec gains optional `stdin_file`, both platform branches), CMake (+test exec, +link uninstall lib).

- RED: `ctest -R cli_verbs` → 7/7 Failed against the CLI that answered "unknown verb" (usage, enable/status/disable roundtrip, no-manager, plain uninstall, purge refusal, purge typed-word, refuses-while-answering). (One test-side fix during RED: my canned-listener case deadlocked its own accept/join — added `shutdown(SHUT_RDWR)` before join; contract unchanged. R-T5-e.)
- GREEN: 7/7 pass (roundtrip asserts the golden hardened text lands at XDG_CONFIG_HOME/systemd/user/strata.service, the shim records `enable --now`/`disable --now`, disable+uninstall remove it; typed word "purge" on stdin gates models removal; canned 401 listener forces the Ctrl+C refusal with nothing removed).
- Gates: linux-gcc build 0 warnings; `ctest --preset linux-gcc` 868 registered / 865 run / 100% / 4 allowed env skips (861→868). linux-gcc-release 0 warnings, full ctest 831/831 100%. windows-llvm-mingw-cross 0 warnings after one cross-only fix (unused note() under !WIN32 guards). pytest 33.
- R-T5-a: `service status` exits 0 — it is a report, not a verdict; the two lines carry the unreachable-manager truth.
- R-T5-b: `install_location_ok` is true in M1 — there is no install metadata to compare against yet (the versioned `current` layout is M2's); the planner gate exists and M2 will feed it.
- R-T5-c: the verb passes allowed roots = {config file's parent, state, models, logs, user config home, secret root}; the planner skips empty roots in the containment check (unset optionals bound nothing).
- R-T5-d: `--yes` is not in the uninstall grammar at all — a usage error cannot satisfy the typed gate (plan's "--yes cannot satisfy" made literal). The typed word is read from stdin even when it is not a terminal (e2e feeds it by file); no word, no removal.
- Exit codes: verb failure → 1; usage mistakes → usage(1) with the updated kUsage (`doctor [--downloader] [--deep]`, `service enable|disable|status`, `uninstall [--purge]`).
- Note for Task 6: the service verbs call the three plans, whose Windows branches still answer "not wired yet (SP7 M1 Task 6)" — compiled, honest, replaced in Task 6; cli_verbs service cases stay NOT-WIN32-registered to match.

### Task 6 — Windows branch: schtasks golden + wired plan branches (compile-proof only)

Files: `src/service_ops.{hpp,cpp}` (`render_schtasks_create`, Windows branches of `plan_enable/plan_disable/plan_status`, `user_manager_reachable` branch, `ServiceEnv.task_xml_path`, `kTaskXmlFileName`; two helpers `[[maybe_unused]]` in Windows builds), `src/strata_cli_network.cpp` (APPDATA fallback in `user_config_home`, task-file paths in `verb_service`/`verb_uninstall` WIN32 branches), `tests/service_ops_test.cpp` (+2 host-runnable goldens, +5 `_WIN32`-guarded plan cases), `tests/cli_verbs_test.cpp` (+1 `_WIN32` e2e with a `schtasks.cmd` PATH shim), `tests/cli_verbs_test.cpp`+support (note/read_text made portable), CMake (+2 base cases, WIN32 blocks for plan cases and the windows-runtime-labelled e2e).

- RED: `render_schtasks_create` seam missing → compile-failure RED (`has not been declared`), then goldens green. The `_WIN32`-only plan/e2e cases cannot be RUN on Linux by definition; their RED/GREEN evidence is the cross gate: they compile against the new branches (cross build 0 warnings) and stay registered for the owner's Windows build (`LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME}`; on Linux that variable is empty and the cases are registered under `if(WIN32)` only, so the Linux battery is untouched).
- GREEN: 15/15 service_ops (13 + 2 goldens), 7/7 cli_verbs on Linux.
- Gates: linux-gcc build 0 warnings; `ctest --preset linux-gcc` 870 registered / 867 run / 100% / 4 allowed env skips. linux-gcc-release 0 warnings, full ctest 833/833 100%. windows-llvm-mingw-cross 0 warnings — now exercising the real Windows plan branches (CreateProcessW schtasks flows) plus the Windows test bodies. pytest 33.
- R-T6-a: task XML golden = LogonTrigger + RunLevel LeastPrivilege + Priority 6 (Normal, not the Below-normal default) + RestartOnFailure PT5M×3 + ExecutionTimeLimit PT0S. Upstream's SETTINGS.md pair is Priority-Normal + "run with highest privileges"; PX §6 never-elevates wins (the owner's spec predates and overrides upstream's hint): we take the Priority half and refuse elevation, and doctor marks the Windows large-pages/memlock matter not-applicable (R-T3-c). Tension recorded rather than silently resolved.
- R-T6-b: rejection for the task text adds XML metacharacters (& < ") to the systemd whitespace rule; absoluteness is portable text (leading `/` or `X:\`), so the golden renders identically from any host.
- R-T6-c: Windows task file = `<APPDATA-or-config-home>/strata/strata-task.xml`; `plan_disable` refuses foreign paths before running anything (mirrors the unit rule), tolerates `/End` on a not-running task, and removes only the exact file after a successful `/Delete /F`.
- R-T6-d: `schtasks` unreachable = argv exec failure (exit<0); "task does not exist" (exit>=1) is the fresh-install answer and proceeds (`/Create /F` is idempotent).
- No Windows runtime claims: nothing here has run on Windows. The labelled e2e + plan cases run on the owner's box.

### Task 7 — truth pass

Files: spec `2026-10-22-sp7-...-design.md` (D-6 rewritten to the health-probe decision table; D-5 records the R-T6-a settings split; D-7 notes the compiled-in per-profile expectations — R-T3-a), `docs/AGENT_HANDOFF.md` (new SP7 M1 section: task table, gate table, declared gaps), new `docs/superpowers/handoff/reports/sp7-m1/FINAL-REPORT.md`.

- Gates (final battery at HEAD~1, C++ state identical): `ctest --preset linux-gcc` 870 reg / 867 run / 100% / 4 allowed skips; release 0 warnings, full ctest 833/833; cross 0 warnings; pytest 33. Docs-only commit.
- Lane done: 7 commits on `sp7-m1-service-doctor` over base `9b467e08`: 5ad06ab4, 2e314fed, 51fa1065, 569186b0, 1fa0c4af, 84a99b00, 161523fe. Nothing pushed. Awaiting the controller merge gate.
