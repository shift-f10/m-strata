# Sub-controller brief — SP3 M1: API concurrency and HTTP hardening (26 tasks)

Read `subcontroller-common.md` (same directory) first; it binds. Then this file.

- **Plan:** `docs/superpowers/plans/2026-10-14-sp3-m1-concurrency-hardening.md` (tasks 0–5, 6a, 6b, 7, 8a, 8b, 9–23).
- **Spec:** `docs/superpowers/specs/2026-10-14-sp3-api-concurrency-streaming-design.md` (APPROVED; B1–B12 inline; §1.5 D-1/D-2 and §2.6 were re-anchored on 2026-10-14 to what SP2 run 2 shipped — trust the spec text, not the plan, where they differ on cancellation).
- **Owner decisions file:** `/tmp/claude-1000/-opt-projects/ee5cedea-4504-4508-8cef-d9fc7bb1a693/scratchpad/owner-decisions-2026-10-14.md` section B.
- **Worktree:** `/opt/projects/ai/.worktrees/sp3`, branch `sp3-api-streaming`, created from `main` — **HEAD at launch: `557a2cc7`** (main with SP1, SP2 run 2, the Windows UTF-8 argv fix and SP4 Tasks 1–24b merged at 4b9fdd31). Verify `git submodule status` shows ` 296b59d2… upstream (v0.1.44.1)`.
- **Baseline on this tree (controller-verified):** linux-gcc 520/520 (+2 skips: proc03_other_uid_cannot_open_engine_fd, proc05_cgroup_kill_when_delegated), linux-gcc-release 510/510 full and 6/6 with the release labels, wine 256/256, pytest 23, 0 warnings on all presets. MSYS2 real Windows for this exact tree: CI run on main 557a2cc7, in progress at launch — the controller forwards the result (previous main 863b3ff6 was 282/282).
- **Merge-time facts (merge 4b9fdd31):** SP4's manifest-side `EngineFamily` is `ManifestFamily` (SP2 owns `EngineFamily`); `compose_engine_launch` renders the typed `engine.gpu` list; supervisor profile/flavor come from `launch.profile`; Windows UTF-8 argv is the shared library `secure_frontend_utf8_argv` used by both binaries; the harness and fixtures pass `--expert`.
- **Final report path:** `/opt/projects/ai/.worktrees/sp3/.superpowers/sdd/2026-10-14-sp3-m1-concurrency-hardening/FINAL-REPORT.md`
- **Concurrent lane:** SP4 part 2 (Tasks 25–29: confined downloader + `strata` verbs) runs in `.worktrees/sp4b` at the same time. It owns `src/acquire*`, `src/*downloader*`, `src/models_store*`, the `strata` binary's verbs, `docs/AGENT_HANDOFF.md` SP4 section. You own the HTTP/API/router/concurrency surface. Shared files you will both touch: `secure_frontend/CMakeLists.txt` (append-only hunks), `docs/AGENT_HANDOFF.md` (your own SP3 section + "Known small gaps" additive lines), `src/frontend_config.cpp` (`kSchema` relative count; your 10 rows append after SP4's `pull.*`), `README.md` key table (regenerate). Keep hunks compact; the controller merges.

## Task 0 anchor check — rule on these before Task 1 (from `sp3-m1-reanchor-notes.md`, now facts on main)
1. CI YAML: `upstream.version` is the engine-version source; the TSan job needs no extra `-D`; keep both header-comment sentences if they text-conflict.
2. `CMakePresets.json`: `SECURE_FRONTEND_TEST_HOOKS` ON in test presets, `linux-gcc-release` exists (hooks OFF, `-L "release-only|release-also"`); the TSan preset inherits hooks ON. Do not re-add.
3. `frontend_config.hpp`: SP4 added `pull` + ctor parameter; `tokenizer_pack`/`profile` are `std::optional`; SP2 added `engine.allow_new_privileges`, `EngineEnvironment.family`, `--gpu` typed list, `roots.state`/`roots.cache` semantics. Your `logging` member + `ConcurrencyLimits` rewrite re-anchor on that shape; any deref of the optionals must handle `nullopt`.
4. `secure_server_main.cpp`: the supervisor owns the engine (SP2); launch composed by `compose_engine_launch` (SP4); served model id = `launch.model_id`; `owned_by` from the launch/inventory record; Windows argv is UTF-8 via the shared wide-argv helper (863b3ff6 / merge) — your router config must take `model_id = launch.model_id` and must not reintroduce `selected`.
5. `tests/support/server_harness.*` / `server_fixture.hpp`: SP4 requires `--expert` for weight flags in `engine.args`; SP2 launches a private 0700 engine copy through the shared non-elevated launcher, sets `--state-dir`, excludes `conhost.exe` from Job counts on Windows, and uses the server's resolved long-path root. Read `tests/support` on main at launch and pass what the fixtures pass.
6. `cfg01_*` cases live in `secure_frontend_typed_config_tests`.
7. Egress: SP4 landed a connect-audit `LD_PRELOAD` shim — reuse/extend it; keep SP3's static-import tripwire.
8. README key table is generated — regenerate, never hand-merge.
9. The `strata` binary exists (SP4 verbs only); SP3 M1 does not create one; the product-spec CLI (`serve`, `version`, `key`) is still unowned — flag to the controller at the end.

## SP2 run-2 facts binding on SP3 (from the SP2 final report; detail in `sp2-run2-handoff-items.md`)
- Cancellation: `stop(EngineStopCause::client_cancelled)`; the reason is derived through `kill_reason_for(protocol)` → `KillReason::client_stop_drain_failed`; uncounted restart, bounded by `config_defaults::kMaxConsecutiveUncountedRestarts` (3) consecutive with no `DONE` between.
- `wait_for_change(from, timeout)` observes STATES, not transitions (`wait_for_change(generating, t)` wakes on lease release → `ready`). `acquire`/`snapshot`/`report_fatal`/`release` are thread-safe (one internal mutex).
- Known gap carried to you: spec §4.7 K4 says every frontend fd is `O_CLOEXEC`, but listening and accepted sockets are not (the descriptor sweep covers them). Your socket work sets CLOEXEC/`WSA_FLAG_NO_HANDLE_INHERIT` on listener and accepted sockets and records the gap closed in the handoff.
- Windows `send_all` to a stalled reader is not sliced (POSIX bound via `SHUT_RDWR`) — your write-stall timeout must slice writes on both platforms.
- Parked for you: `engine_supervisor.hpp` destructor comment ("one exception") omits Windows (a reaped generation still leased in `retired_` runs cleanup on the destroying thread) — fix the wording when you touch the lease lifecycle.
- Fixed socket ports 20000–20099 collide under `ctest -j`: if your tasks touch that fixture, move to ephemeral port 0 (plan permits).

## Plan-specific reminders
- Seam types from the plan are binding names (`RouteResult`, `StreamSink`, `HttpHead`, `StreamHandler` in `request_router.hpp`; `serialize_head`; `deliver_route_result`; `SocketStreamSink` placeholder) — SP3 M2 greps for them.
- TSan job is non-blocking in CI; still run the TSan preset locally where the plan says so.
- No network in tests; no spending. `-Werror` everywhere; TDD with RED evidence per task.
- Commits: one per task; `-c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com`; trailer lines LITERAL in every commit command: `Co-Authored-By: <Claude Opus 5.5 | Claude Sonnet 5.5> <noreply@anthropic.com>` and `Claude-Session: <session link>`.
- After Task 23: final whole-branch review (one fix dispatch + scoped re-review), final report `COMPLETE (M1)`, stop. The controller merges and launches M2.
