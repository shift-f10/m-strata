# Sub-project 0 — Foundation and hardening of `secure_frontend`

Date: 2026-10-13. Status: design for review. Parent effort: Secure Strata
full release path (sub-projects 0–7), see `docs/AGENT_HANDOFF.md`.

## 1. Purpose

Sub-project 0 makes the existing `secure_frontend/` work trustworthy before
any new capability is built on it. It has three outcomes:

1. A reproducible build-and-test loop on the Linux dev host and in CI, with an
   open-source Windows toolchain, so every later sub-project can prove its work.
2. Every defect found by the 2026-10-13 audit fixed, each behind a named
   regression test that fails before the fix and passes after it.
3. The repository's integration, CI, and documentation telling the truth about
   what is and is not verified.

It adds **no new security features**. Typed limits configuration, the
canonical-path module, CSPRNG token generation, streaming, `PROC-*` tests by
ID, and `acquire` belong to later sub-projects.

## 2. Constraints carried from the parent effort

- Dev host: Linux, g++ 11.4, make, Python 3.10, no root. No cmake, ninja,
  clang, MinGW, nvcc, or hipcc installed. Loopback bind is permitted. wine
  6.0.3 is present.
- Target platforms: Linux and Windows 10/11, both NVIDIA for the GPU engine.
  The offline test box exists in both OS forms.
- The frontend has no third-party dependencies; keep it that way.
- Nothing is pushed by agents. Commits are local; the owner pushes.
- `docs/AGENT_HANDOFF.md` is updated as part of this sub-project, not after.
- One change per concern; a regression test lands before each fix
  (`AGENTS.md`, handoff "Collaboration and change rules").

## 3. Decisions

### 3.1 Build tooling — project-local venv

`scripts/dev/bootstrap-linux.sh` creates `.venv-dev/` and installs pinned
`cmake` and `ninja` wheels plus `requirements.txt`. It is idempotent, needs no
root, and works from pre-downloaded wheels for the offline box. All CMake and
CTest commands in docs and CI assume this venv (or an equivalent system
install). No hand-written Makefile is added.

Rejected: system `apt` install (needs root); a parallel Makefile (drifts from
CMake).

### 3.2 Windows toolchain — open source first, MSVC informational

- **Required Windows toolchain:** llvm-mingw (clang + mingw-w64 runtime).
  `scripts/dev/bootstrap-llvm-mingw.sh` downloads one exact pinned release
  (version and SHA-256 recorded in the script), verifies the digest, and
  extracts it under `.toolchains/` (gitignored). A CMake toolchain file
  `secure_frontend/cmake/toolchains/llvm-mingw-x86_64.cmake` enables cross
  compilation from Linux. A `windows-llvm-mingw-cross` preset builds all
  libraries, tests, and the three executables.
- **Local Windows smoke under wine:** the cross-built `--help` and
  configuration-rejection paths of the three executables run under wine. wine
  is a smoke harness only; Job Objects, DACLs, and handle inheritance are not
  trusted under wine and are documented as such.
- **CI (`.github/workflows/secure-frontend.yml`):**
  - `ubuntu-latest` × `linux-gcc` — blocking.
  - `ubuntu-latest` × `linux-clang` — blocking.
  - `ubuntu-latest` × `windows-llvm-mingw-cross` — blocking (compile + link of
    everything Windows).
  - `windows-latest` × MSYS2 `CLANG64` environment — **blocking**: builds with
    clang/mingw-w64 and runs CTest on real Windows.
  - `windows-latest` × `windows-msvc` — **non-blocking** (`continue-on-error:
    true`), compile and CTest, informational only.
  - A `top-level-integration` job configures the repository root with
    `-DSTRATA_BUILD_SECURE_FRONTEND=ON` and builds only the frontend targets,
    so the integration path is exercised. Workflow path filters include the
    top-level `CMakeLists.txt`.
- MinGW-specific portability (no `/W4 /WX`, `-municode` or explicit
  `wmain`/`main` choice, `ws2_32`/`advapi32` link lines, `NOMINMAX`,
  `WIN32_LEAN_AND_MEAN`) is part of this sub-project.

Reason: CUDA on Windows requires MSVC as host compiler for the **engine**, but
the frontend contains no CUDA. Keeping the frontend MSVC-free removes the only
proprietary toolchain from its path; the MSVC job stays visible for anyone who
must build frontend and engine with one toolchain.

### 3.3 Socket server — correctness now, concurrency later

SP0 fixes the ways one client can kill or stall the server. It does **not**
add a worker pool, connection limits, or `DOS-02`; those depend on the typed
limits configuration of sub-project 1 and are implemented in sub-project 3.
Hardcoding limits now would be undone there.

## 4. Scope — defects and their regression tests

Each row is one task: write the test, watch it fail, fix, watch it pass.
Test names below are the CTest names to add.

### 4.1 Tooling and integration

| Item | Change | Verification |
| --- | --- | --- |
| T1 | `scripts/dev/bootstrap-linux.sh`; `docs/secure-frontend-ci.md` and `secure_frontend/README.md` updated to the venv flow. | `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc` passes on the dev host, including `secure_frontend_http_socket_server_tests`. |
| T2 | `scripts/dev/bootstrap-llvm-mingw.sh`, toolchain file, `windows-llvm-mingw-cross` preset. | Cross build produces `.exe` for all three executables and all test binaries; `wine strata-secure-server.exe --help` exits 0. |
| T3 | Top-level `option(STRATA_BUILD_SECURE_FRONTEND ... OFF)`; `STRATA_BUILD_SECURE_FRONTEND_TESTS` keeps defaulting to `STRATA_BUILD_TESTS`. | Root configure without the option builds no frontend target; with `ON` it builds them. AGENTS.md opt-in rule satisfied. |
| T4 | Remove dead `if(MSVC)` inside `if(UNIX)` blocks (CMakeLists.txt ~292–295, ~338–339). Frontend `-Werror`/`/WX` becomes `option(SECURE_FRONTEND_WERROR ON)`; when built from the root it defaults to `STRATA_WERROR`. | Configure with each setting; flags appear/disappear in `compile_commands.json`. |
| T5 | `pytest.ini` (or `[tool.pytest.ini_options]`) that collects only `test_*.py` modules under `serve/`, `tools/`, and the frontend's Python tests; argv-driven `tools/*_test.py` scripts are excluded. | `pytest -q` from the root passes inside the bootstrap venv, so the the owner pre-push hook passes without `ECC_SKIP_PREPUSH`. Any module that still needs an uninstalled package is either added to `requirements.txt` (if upstream already lists it) or skipped with a reason. |
| T6 | CI workflow as in §3.2. | Workflow YAML validated with `actionlint` or `gh workflow view` after push by the owner; first green run recorded in the handoff. |

### 4.2 Socket server (`http_socket_server.cpp`)

| Item | Defect | Fix | Regression test |
| --- | --- | --- | --- |
| S1 | `send()` without `MSG_NOSIGNAL`; `SIGPIPE` not ignored; early-closing client kills the process. | `MSG_NOSIGNAL` on Linux, `SO_NOSIGPIPE` where available, and `signal(SIGPIPE, SIG_IGN)` in a shared `install_process_defaults()` called by all three mains. | `socket_server_survives_client_early_close`: client connects, sends a full request, closes before reading; server thread returns an error, process alive, next request served. |
| S2 | No send timeout; a client that never reads blocks `send_all` forever. | `SO_SNDTIMEO` equal to the request deadline; `send_all` fails cleanly. | `socket_server_send_times_out_on_stalled_reader`. |
| S3 | No total request deadline; 5 s per `recv` lets one byte every 4.9 s hold the server for minutes. | Monotonic deadline for header+body (constant `kRequestDeadline = 30 s` in SP0, promoted to config in SP1); `recv` timeout recomputed from the remaining budget. Chat generation time is governed separately by `ChatExecutionLimits` and does not count against this deadline. | `socket_server_rejects_slow_request_past_deadline`: dripping client is cut off at the deadline ± tolerance. |
| S4 | Oversize request handling only tested in-memory. | None beyond the test unless it fails. | `socket_server_rejects_oversize_request_over_tcp`: 64 KiB + 1 bytes → bounded `413`/close, server alive. |
| S5 | Listening socket inherits `SO_RCVTIMEO`, so `accept` times out and `smoke_main.cpp:81` exits after ~5 s idle. | Timeouts set on the accepted socket only; `serve_one` distinguishes "idle" from "client error"; all mains keep serving on client errors and stop only on listener failure or shutdown signal. | `socket_server_accept_waits_without_timeout`: no client for 7 s, server still accepting. |
| S6 | `::` binds without `IPV6_V6ONLY`; no `SO_REUSEADDR`. | Set `IPV6_V6ONLY` for IPv6 binds; set `SO_REUSEADDR` (`SO_EXCLUSIVEADDRUSE` on Windows). | `socket_server_ipv6_bind_is_v6only` (skipped if host lacks IPv6). |
| S7 | Response completeness not asserted. | None unless the test fails. | Existing health test extended to read to EOF and check `Content-Length` matches body. |

### 4.3 Engine protocol and chat execution

| Item | Defect | Fix | Regression test |
| --- | --- | --- | --- |
| E1 | Generation timeout is per line, not total (`engine_protocol_client.cpp:164`). | Total generation deadline; per-line read uses the remaining budget. | `generation_total_deadline_is_enforced` with a fake engine emitting one token every 100 ms past the total. |
| E2 | After timeout, protocol stays `generating`; `ready()` is true; every later chat is `503` until restart. | On timeout send `STOP`, drain to `DONE`/`ERR` under a bounded drain deadline, return to `ready`; if drain fails, state `failed`. `ChatExecutionService` surfaces a generic `503` for that request only. | `chat_recovers_after_generation_timeout`: second request succeeds. `chat_marks_engine_failed_when_drain_fails`. |
| E3 | `/health` only checks the child is alive. | `/health` reports `engine: ready|generating|failed|stopped` and returns `503` for `failed`/`stopped`. No secrets, no paths. | `health_reflects_protocol_state`. |
| E4 | `max_line_bytes` 8192 caps the `GEN` line to roughly 1.2–1.6 K tokens while limits advertise 32 768 prompt tokens. | An agent reads the upstream `--serve` implementation in `src/` and records its real line/token limits in `docs/security/engine-protocol-limits.md`. `EngineProtocolLimits` and `ChatExecutionLimits` are set to exactly what the engine accepts; if the engine accepts multi-line or chunked prompts, use that; never advertise more than the engine takes. | `gen_line_fits_max_prompt_tokens`: a prompt at the advertised max encodes into a `GEN` line that passes the limit. |

### 4.4 Process launch hygiene

| Item | Defect | Fix | Regression test |
| --- | --- | --- | --- |
| P1 | Heap allocation between `fork` and `execve`; not async-signal-safe. | Build `argv`/`envp` as `std::vector<char*>` before `fork`; child performs only `dup2`, `close`, `setpgid`, `execve`, `_exit`. Prefer `posix_spawn` with `posix_spawn_file_actions` where it covers the needs. | Review-only item plus `child_receives_exact_argv_and_env` with a fake engine that dumps both (reused by SP2). |
| P2 | `pipe()` without `O_CLOEXEC`; no descriptor sweep; unrelated fds leak into the engine. | `pipe2(O_CLOEXEC)`; explicit `dup2` of the three stdio fds; `close_range(3, ~0U)` with a `/proc/self/fd` fallback. | `child_inherits_only_stdio_descriptors`: fake engine lists `/proc/self/fd`; expects exactly 0,1,2. |
| P3 | After `SIGKILL`, `EngineStdioProcess::terminate` does not reap. | `waitpid` with bounded wait after kill; zombies impossible. | `terminate_reaps_killed_child`: no zombie in `/proc` after terminate. |
| P4 | Windows: process created running, then assigned to the Job; early descendants escape. | `CREATE_SUSPENDED` → `AssignProcessToJobObject` → `ResumeThread`. | Compiles in cross build; runtime assertion in the MSYS2 CI job via a fake engine that spawns a grandchild which must die with the Job. |
| P5 | Windows compile errors: `to_wide(path.u8string())` type mismatch; `std::min` vs `<windows.h>` `min` macro. | `NOMINMAX` + `WIN32_LEAN_AND_MEAN` before every Windows include (central `win32_prelude.hpp`); convert paths via `std::filesystem::path::wstring()`. | Cross build and MSYS2 job compile with `-Werror`. |
| P6 | Windows `read_line`: busy loop, recursive per 1 KiB, no size bound. | Iterative loop with `max_line_bytes` enforced identically to POSIX; `WaitForSingleObject`/overlapped or bounded `PeekNamedPipe` poll with the remaining deadline. | `read_line_rejects_oversize_on_all_platforms` (shared test body). |
| P7 | `WriteFile` has no timeout on Windows. | Overlapped write with the deadline, or document the bounded-write contract if the pipe buffer makes it unnecessary. | Covered by P6 test file on Windows. |

### 4.5 Model-id linkage and engine argument policy

| Item | Defect | Fix | Regression test |
| --- | --- | --- | --- |
| M1 | `--model-id` only checked against the inventory; the engine loads whatever `--engine-arg` says. | The frontend derives the engine's model-path argument from the inventory entry for `--model-id` and appends it itself. An agent reads the engine CLI in `src/` to find the exact flag and records it in `docs/security/engine-protocol-limits.md`. | `engine_receives_model_path_from_inventory` via the argv-dumping fake engine. |
| M2 | Only `--serve` and NUL are rejected in `--engine-arg`. | A deny-list of engine flags the frontend owns or that open network listeners (model/weights path, any host/port/listen/bind/http flag, as found in `src/`). Everything else still passes through; full typed pass-through is sub-project 5. | `engine_args_reject_owned_and_network_flags`. |

### 4.6 Documentation hygiene

| Item | Change |
| --- | --- |
| D1 | `secure_frontend/README.md`: remove "placeholder until real routes" and "presets supplied by the parent project"; document venv flow and the wine smoke caveat. |
| D2 | `token_reader.hpp:42–48` comment updated; `protected_file_unsupported_on_platform` removed if unreachable, or kept with an accurate comment. |
| D3 | `docs/AGENT_HANDOFF.md`: merge the duplicate GLM-tokenizer rows; rewrite "What has changed" and "Verification performed" for SP0; record the exact build command via the scripts rather than an elided `g++ ...` line; state the Windows status honestly (cross-compiled and MSYS2-tested when CI is green; MSVC informational). |
| D4 | `docs/security/engine-protocol-limits.md` created by E4/M1. |

## 5. Out of scope (explicit)

Typed limits/roots configuration; canonical-path module; CSPRNG token
generation or DPAPI; streaming; concurrency, connection limits, `DOS-02`;
`PROC-*`, `NET-*`, `WEB-*`, `DOS-*`, `EXT-*` tests by ID; engine restart with
backoff; `acquire`; any engine (`src/`) change. If an SP0 fix needs an engine
change, stop and report; record any such change in `docs/PROVENANCE.md`.

## 6. Execution model

- Orchestrator: this session. Implementation and review: Opus 4.8 subagents,
  one task at a time, each followed by a spec-compliance review and a code
  quality review by separate agents.
- Each task: regression test first (fails), fix, test passes, full `ctest`
  green, then one local commit with the test and fix together.
- The orchestrator reruns `ctest` itself before accepting a task.
- Before any task touches `src/` or installs anything outside `.venv-dev/`,
  `.toolchains/`, and the repo tree, work stops and the owner decides.

## 7. Acceptance for sub-project 0

1. On the dev host: bootstrap scripts run from clean; `ctest --preset
   linux-gcc` is green, including the socket-server tests; the cross build
   produces all Windows binaries; the wine smoke passes.
2. In CI after the owner pushes: `linux-gcc`, `linux-clang`,
   `windows-llvm-mingw-cross`, MSYS2 `CLANG64`, and `top-level-integration`
   green; `windows-msvc` reported but non-blocking.
3. Every defect S1–S7, E1–E4, P1–P7, M1–M2 has a named test that exists in
   CTest and is documented as its regression test in the commit message.
4. `pytest -q` passes at the repository root inside the venv.
5. `docs/AGENT_HANDOFF.md` describes the state accurately; `git status
   --short` is clean except for `.venv-dev/` and `.toolchains/`, both
   gitignored.
6. Nothing is claimed as GPU-verified or Windows-runtime-verified beyond what
   the CI logs show.
