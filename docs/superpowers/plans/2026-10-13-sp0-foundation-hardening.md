# SP0 Foundation and Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the existing `secure_frontend/` trustworthy: reproducible build/test on Linux and an open-source Windows toolchain, every 2026-10-13 audit defect fixed behind a named regression test, and documentation that tells the truth.

**Architecture:** No new subsystems. Each defect is fixed inside the module that owns it (`http_socket_server`, `engine_protocol_client`, `chat_execution`, `engine_stdio_process`, `engine_process`), plus two small new pieces: a `process_defaults` function (SIGPIPE immunity) and an `engine_arguments` library (model-id → `--native` linkage and the engine-flag deny-list). Tooling lives under `scripts/dev/` and `secure_frontend/cmake/`.

**Tech Stack:** C++20 (g++ 11.4 locally, clang in CI), CMake 4.4.3 + Ninja 1.13.2 from `.venv-dev/`, llvm-mingw 20261006 (clang 23.1.3) for Windows cross builds, wine 6.0.3 for smoke only, Python 3.10 + pytest 9 for the repo-level `pytest -q`, GitHub Actions (ubuntu-latest, windows-latest with MSYS2).

**Spec:** `docs/superpowers/specs/2026-10-13-sp0-foundation-hardening-design.md`

## Global Constraints

- No third-party C++ dependencies in `secure_frontend/`; standard library and OS APIs only (spec §2).
- No change under `src/`, `include/`, `serve/`, `setup.py`. If a fix needs one, stop and report (spec §5, §6).
- Install nothing outside `.venv-dev/`, `.toolchains/`, and the repository tree; no `sudo`, no `apt` (spec §2, §6).
- Agents never push. Commits are local, one per task, with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` as the last trailer (spec §2).
- Regression test first; it must fail before the fix and pass after (spec §4).
- Warnings are errors: `-Wall -Wextra -Wpedantic -Werror` (GCC/Clang/MinGW) and `/W4 /WX` (MSVC) stay on for every frontend target unless `SECURE_FRONTEND_WERROR=OFF` (spec §4.1 T4).
- Test style: no framework. Each test file defines `int failures` and `void expect(bool, const std::string&)` in an anonymous namespace, one `void` function per case, `main` returns `failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE`. Test binaries that need a child re-exec themselves (`--fake-engine` scanned across all argv).
- Build and test commands always run with `export PATH=/opt/projects/ai/.venv-dev/bin:$PATH` and from `/opt/projects/ai/secure_frontend` unless stated. Build dir for local work: `out/build/linux-gcc` (preset `linux-gcc`).
- Never print tokens, prompts, or engine paths in errors or logs (handoff privacy contract).
- Engine facts (from `src/program/generate.cpp` at v0.1.43): stdin lines are unbounded; `READY <ctx> stop`; GEN is `GEN <max_new> <id,id,...>` on one line; prompt limit is `n + max_new + 8 <= --max-context` (default 4096), violation → `ERR ...` and the engine stays alive; STOP always yields `DONE ... cancel`; model path is `--native <shard.gguf>` or `--pack DIR`; the engine has no network flags; binary is `strata`.

## Review Focus

1. A client that half-closes right after sending a full request (shutdown(SHUT_WR)) must still receive its response and must not kill the server — pinned in Task 4 (`socket_server_survives_client_early_close` sends, closes, server stays alive) and Task 6 (`socket_server_response_is_complete` reads to EOF).
2. An IPv6 loopback listener (`::1`) with `Host: [::1]:<port>` must serve normally and must not also accept IPv4 — pinned in Task 6 (`socket_server_ipv6_bind_is_v6only`).
3. A `DONE ... nonfinite` record (NaN guard) must produce a 503 for that request and leave the engine usable — pinned in Task 7 (`chat_rejects_nonfinite_done_and_stays_ready`).
4. The engine exiting mid-generation (stdout closed) must mark health failed and return 503, never a partial text — pinned in Task 8 (`health_is_failed_after_engine_closed_mid_generation`).
5. `--engine-arg --native=/other/model.gguf` (equals form) and `--engine-arg=--pack` must hit the deny-list exactly like the spaced form — pinned in Task 12 (`engine_args_reject_owned_and_network_flags`).

---

### Task 1: Dev bootstrap script, pytest scope, ignore rules

**Files:**
- Create: `scripts/dev/bootstrap-linux.sh`
- Create: `pytest.ini`
- Modify: `.gitignore` (append)
- Modify: `docs/secure-frontend-ci.md` (replace "how to build locally" section)
- Test: manual commands below (shell-level task; no C++ test)

**Interfaces:**
- Produces: `.venv-dev/bin/{cmake,ninja,python,pytest}` for every later task.

- [ ] **Step 1: Write the failing check**

Run from `/opt/projects/ai`: `test -x scripts/dev/bootstrap-linux.sh && .venv-dev/bin/pytest -q`
Expected: FAIL (`bootstrap-linux.sh` missing; or pytest shows `ERROR tools/batch_groups_cache_test.py` etc.).

- [ ] **Step 2: Create `scripts/dev/bootstrap-linux.sh`**

```bash
#!/usr/bin/env bash
# Creates the project-local developer environment for the secure frontend on
# Linux without root: a venv holding the pinned cmake/ninja/pytest wheels from
# requirements.txt. Idempotent. Works offline when the wheels are already in
# the pip cache or in $STRATA_WHEEL_DIR.
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
venv="$repo/.venv-dev"
python="${PYTHON:-python3}"

if [[ ! -x "$venv/bin/python" ]]; then
  "$python" -m venv --without-pip "$venv"
fi
if [[ ! -x "$venv/bin/pip" ]]; then
  # Debian/Ubuntu images often lack ensurepip; bootstrap pip from the system pip.
  if "$python" -m pip --version >/dev/null 2>&1; then
    "$python" -m pip download --no-deps -q -d "$venv/.pip-bootstrap" pip
    "$venv/bin/python" "$venv/.pip-bootstrap"/pip-*.whl/pip install --no-index --find-links "$venv/.pip-bootstrap" pip
  else
    echo "bootstrap-linux: neither ensurepip nor a system pip is available" >&2
    exit 1
  fi
fi
pip_args=(-q -r "$repo/requirements.txt" pytest)
if [[ -n "${STRATA_WHEEL_DIR:-}" ]]; then
  pip_args=(--no-index --find-links "$STRATA_WHEEL_DIR" "${pip_args[@]}")
fi
"$venv/bin/pip" install "${pip_args[@]}"
echo "bootstrap-linux: ready. Use: export PATH=$venv/bin:\$PATH"
"$venv/bin/cmake" --version | head -1
"$venv/bin/ninja" --version
```

Run: `chmod +x scripts/dev/bootstrap-linux.sh && scripts/dev/bootstrap-linux.sh`
Expected: prints `cmake version 4.4.3` and `1.13.2...`. (If `.venv-dev` already exists from the orchestrator's manual setup, the script must still exit 0.)

- [ ] **Step 3: Create `pytest.ini`**

```ini
# Collect only real test modules. Upstream tools/*_test.py are argv-driven
# scripts (they parse sys.argv), not pytest tests, and must not be collected.
[pytest]
testpaths = serve tools
python_files = test_*.py
addopts = -q
```

Run: `.venv-dev/bin/pytest -q 2>&1 | tail -3`
Expected: no `ERROR` lines. If a `test_*.py` module still fails at import because an upstream dependency is missing from `requirements.txt`, do NOT add packages; add `--ignore=<path>` to `addopts` with a comment naming the missing module, and report it in the handoff.

- [ ] **Step 4: Append to `.gitignore`**

```
# secure frontend developer environment (scripts/dev/bootstrap-*.sh)
/.venv-dev/
/.toolchains/
/secure_frontend/out/
/secure_frontend/build/
```

- [ ] **Step 5: Update `docs/secure-frontend-ci.md`**

Replace any "install cmake/ninja" instruction with:

```markdown
## Building locally (Linux)

    scripts/dev/bootstrap-linux.sh          # once; creates .venv-dev with pinned cmake/ninja/pytest
    export PATH=$PWD/.venv-dev/bin:$PATH
    cd secure_frontend
    cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc

The venv needs no root. For an offline machine, pre-download the wheels from
`requirements.txt` into a directory and run the script with
`STRATA_WHEEL_DIR=/path/to/wheels`.
```

- [ ] **Step 6: Verify**

Run: `cd secure_frontend && cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -3`
Expected: `100% tests passed, 0 tests failed out of 18`.
Run: `cd /opt/projects/ai && .venv-dev/bin/pytest -q | tail -2`
Expected: a passed count and no errors.

- [ ] **Step 7: Commit**

```bash
git add scripts/dev/bootstrap-linux.sh pytest.ini .gitignore docs/secure-frontend-ci.md
git commit -m "dev: bootstrap venv for cmake/ninja/pytest; scope pytest to real test modules

T1/T5 of SP0. pytest.ini keeps upstream argv-driven tools/*_test.py out of
collection so 'pytest -q' passes from the repo root.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: CMake hygiene — opt-in integration, warnings option, dead blocks

**Files:**
- Modify: `CMakeLists.txt` (top-level, ~lines 1684-1691)
- Modify: `secure_frontend/CMakeLists.txt` (lines 1-7 header, 225-265 flags block, 292-295 and 338-344 dead blocks, every test target's flag lines)
- Test: configure checks below

**Interfaces:**
- Produces: CMake function `secure_frontend_set_warnings(<target>)` used by every later task that adds a target.

- [ ] **Step 1: Write the failing check**

Run from `/opt/projects/ai`: `cmake -S . -B /tmp/claude-1000/-opt-projects/ee5cedea-4504-4508-8cef-d9fc7bb1a693/scratchpad/root-default -G Ninja -DSTRATA_ENABLE_CUDA=OFF -DSTRATA_ENABLE_HIP=OFF 2>&1 | tail -2; grep -c secure_frontend /tmp/claude-1000/-opt-projects/ee5cedea-4504-4508-8cef-d9fc7bb1a693/scratchpad/root-default/build.ninja`
Expected now: a non-zero count (frontend built by default). After the fix: 0. (If the root configure fails for an unrelated engine reason, note it and verify with `cmake -LA` that `STRATA_BUILD_SECURE_FRONTEND:BOOL=OFF` instead.)

- [ ] **Step 2: Top-level option default OFF**

In `CMakeLists.txt` replace:
```cmake
option(STRATA_BUILD_SECURE_FRONTEND "Build the portable hardened frontend foundation" ON)
```
with:
```cmake
# Opt-in (AGENTS.md: new features are opt-in; the default build stays byte-identical).
option(STRATA_BUILD_SECURE_FRONTEND "Build the portable hardened frontend foundation" OFF)
```

- [ ] **Step 3: Warnings option and helper in `secure_frontend/CMakeLists.txt`**

After the header block (line 7) add:
```cmake
if(DEFINED STRATA_WERROR)
    set(_secure_frontend_werror_default ${STRATA_WERROR})
else()
    set(_secure_frontend_werror_default ON)
endif()
option(SECURE_FRONTEND_WERROR "Treat warnings as errors in the secure frontend" ${_secure_frontend_werror_default})

function(secure_frontend_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(SECURE_FRONTEND_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(SECURE_FRONTEND_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endfunction()
```
Delete the whole `if(MSVC) ... else() ... endif()` block at lines 225-265 and replace it with one call per target:
```cmake
foreach(_target IN ITEMS
        secure_frontend_config secure_frontend_token_reader secure_frontend_http_policy
        secure_frontend_http_request_handler secure_frontend_api_router secure_frontend_engine_process
        secure_frontend_engine_protocol_client secure_frontend_engine_stdio_process
        secure_frontend_http_socket_server secure_frontend_model_inventory secure_frontend_chat_schema
        secure_frontend_tokenizer_pack secure_frontend_qwen_prompt_renderer secure_frontend_glm_prompt_renderer
        secure_frontend_chat_generation_plan secure_frontend_chat_execution
        secure-frontend-smoke strata-secure-smoke-server strata-secure-server)
    secure_frontend_set_warnings(${_target})
endforeach()
```
In the tests block, replace every `if(MSVC) target_compile_options(X PRIVATE /W4 /WX) else() target_compile_options(X PRIVATE -Wall -Wextra -Wpedantic -Werror) endif()` with `secure_frontend_set_warnings(X)`. Delete the dead nested `if(UNIX)` inside `if(MSVC)` at 292-295 and 338-339 (they disappear with this rewrite; make sure the UNIX-gated test targets still get `secure_frontend_set_warnings` inside their `if(UNIX)` block).

- [ ] **Step 4: Verify**

Run: `cd secure_frontend && rm -rf out/build/linux-gcc && cmake --preset linux-gcc >/dev/null && grep -c -- "-Werror" out/build/linux-gcc/build.ninja`
Expected: a count ≥ 35 (every target).
Run: `cmake --preset linux-gcc -DSECURE_FRONTEND_WERROR=OFF >/dev/null && grep -c -- "-Werror" out/build/linux-gcc/build.ninja`
Expected: `0`.
Run: `cmake --preset linux-gcc -DSECURE_FRONTEND_WERROR=ON >/dev/null && cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -1`
Expected: 18/18 passed.
Run the Step 1 root check → `0`; then `cmake -S . -B <same> -DSTRATA_BUILD_SECURE_FRONTEND=ON -DSTRATA_ENABLE_CUDA=OFF -DSTRATA_ENABLE_HIP=OFF && grep -c secure_frontend <same>/build.ninja` → non-zero.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt secure_frontend/CMakeLists.txt
git commit -m "build: secure frontend is opt-in; SECURE_FRONTEND_WERROR option; one warnings helper

T3/T4 of SP0. STRATA_BUILD_SECURE_FRONTEND defaults OFF per AGENTS.md. The
40-line per-target flag block becomes secure_frontend_set_warnings(); the dead
if(MSVC)-inside-if(UNIX) blocks are gone. NOMINMAX/WIN32_LEAN_AND_MEAN are
defined for every Windows target.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: llvm-mingw cross toolchain, Windows compile fixes, wine smoke

**Files:**
- Create: `scripts/dev/bootstrap-llvm-mingw.sh`
- Create: `secure_frontend/cmake/toolchains/llvm-mingw-x86_64.cmake`
- Create: `secure_frontend/include/secure_frontend/win32_prelude.hpp`
- Modify: `secure_frontend/CMakePresets.json` (add `windows-llvm-mingw-cross` configure/build/test presets)
- Modify: `secure_frontend/src/engine_stdio_process.cpp:90` (u8string), every file including `<windows.h>` / `<winsock2.h>` (use the prelude)
- Modify: `secure_frontend/CMakeLists.txt` (link `ws2_32` for every target that uses sockets when cross compiling; `engine_process` and `engine_stdio_process` need no extra libs)
- Test: cross build + `wine` smoke

**Interfaces:**
- Produces: preset `windows-llvm-mingw-cross`; header `secure_frontend/win32_prelude.hpp` that every Windows include site uses from now on.

- [ ] **Step 1: Write the failing check**

Run: `scripts/dev/bootstrap-llvm-mingw.sh && cd secure_frontend && cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross`
Expected: FAIL (script/preset missing; after the preset exists, compile errors in `engine_stdio_process.cpp` for `u8string` and in `http_socket_server.cpp` for `std::min`).

- [ ] **Step 2: Create `scripts/dev/bootstrap-llvm-mingw.sh`**

```bash
#!/usr/bin/env bash
# Fetches ONE pinned llvm-mingw release (open-source clang + mingw-w64 runtime)
# into .toolchains/ for cross-compiling the secure frontend to Windows from
# Linux. The digest below was recorded on first download (2026-10-13); the
# script refuses any file that does not match it.
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
version="20261006"
name="llvm-mingw-${version}-ucrt-ubuntu-22.04-x86_64"
archive="${name}.tar.xz"
sha256="5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4"
url="https://github.com/mstorsjo/llvm-mingw/releases/download/${version}/${archive}"
dest="$repo/.toolchains"
mkdir -p "$dest/downloads"

if [[ -x "$dest/$name/bin/x86_64-w64-mingw32-clang++" ]]; then
  echo "bootstrap-llvm-mingw: $name already present"
else
  if [[ ! -f "$dest/downloads/$archive" ]]; then
    if [[ -n "${STRATA_TOOLCHAIN_DIR:-}" && -f "$STRATA_TOOLCHAIN_DIR/$archive" ]]; then
      cp "$STRATA_TOOLCHAIN_DIR/$archive" "$dest/downloads/$archive"
    else
      curl -sSL --fail -o "$dest/downloads/$archive" "$url"
    fi
  fi
  echo "$sha256  $dest/downloads/$archive" | sha256sum -c -
  tar -xJf "$dest/downloads/$archive" -C "$dest"
fi
ln -sfn "$name" "$dest/llvm-mingw"
"$dest/llvm-mingw/bin/x86_64-w64-mingw32-clang++" --version | head -1
```

Run: `chmod +x scripts/dev/bootstrap-llvm-mingw.sh && scripts/dev/bootstrap-llvm-mingw.sh`
Expected: `clang version 23.1.3 ...` and `.toolchains/llvm-mingw` symlink exists. (The archive is already in `.toolchains/downloads/`, so no network is needed.)

- [ ] **Step 3: Create `secure_frontend/cmake/toolchains/llvm-mingw-x86_64.cmake`**

```cmake
# Cross-compile the secure frontend to x86_64 Windows with llvm-mingw.
# Usage: cmake --preset windows-llvm-mingw-cross  (expects ../.toolchains/llvm-mingw)
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT DEFINED LLVM_MINGW_ROOT)
    set(LLVM_MINGW_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.toolchains/llvm-mingw")
endif()
get_filename_component(LLVM_MINGW_ROOT "${LLVM_MINGW_ROOT}" ABSOLUTE)
set(CMAKE_C_COMPILER   "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER  "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-windres")
set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_ROOT}/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Static runtime so wine and a bare Windows box need no extra DLLs.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
# Let ctest run the .exe files under wine when it is present on the build host.
find_program(SECURE_FRONTEND_WINE wine)
if(SECURE_FRONTEND_WINE)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${SECURE_FRONTEND_WINE}")
endif()
```

- [ ] **Step 4: Add presets to `secure_frontend/CMakePresets.json`**

Add to `configurePresets`:
```json
    {
      "name": "windows-llvm-mingw-cross",
      "displayName": "Windows x86_64 (llvm-mingw cross from Linux)",
      "inherits": "secure-frontend-base",
      "generator": "Ninja",
      "toolchainFile": "${sourceDir}/cmake/toolchains/llvm-mingw-x86_64.cmake",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" }
    }
```
Add to `buildPresets`: `{ "name": "windows-llvm-mingw-cross", "configurePreset": "windows-llvm-mingw-cross" }`.
Add to `testPresets`: `{ "name": "windows-llvm-mingw-cross", "configurePreset": "windows-llvm-mingw-cross", "output": { "outputOnFailure": true } }`.

- [ ] **Step 5: Create `secure_frontend/include/secure_frontend/win32_prelude.hpp`**

```cpp
#pragma once
// Include this instead of <windows.h>/<winsock2.h> directly. It fixes the
// macro hygiene that otherwise breaks std::min/std::max and pulls in the
// socket headers in the order Winsock2 requires.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif
```
In every source and test file with `#ifdef _WIN32` includes of `<windows.h>`, `<winsock2.h>`, or `<ws2tcpip.h>` (grep: `grep -rl "include <windows.h>\|include <winsock2.h>" secure_frontend/`), replace those includes with `#include "secure_frontend/win32_prelude.hpp"`.

- [ ] **Step 6: Fix `engine_stdio_process.cpp` u8string**

Replace `auto executable = to_wide(command.executable.u8string());` with:
```cpp
  std::optional<std::wstring> executable = command.executable.wstring();
```
(`std::filesystem::path::wstring()` is already UTF-16 on Windows; keep the `if (!executable)` check as-is since `optional` is always engaged here, or simplify to a plain `std::wstring` and delete the check.) Also add `#include <cstdint>` to this file (it uses `std::int64_t`).

- [ ] **Step 7: Cross build and wine smoke**

Run: `cd secure_frontend && cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross 2>&1 | tail -5`
Expected: `strata-secure-server.exe`, `strata-secure-smoke-server.exe`, `secure-frontend-smoke.exe`, and the test `.exe` files in `out/build/windows-llvm-mingw-cross/`. Fix any further MinGW-only errors (missing `ws2_32` link, `ssize_t`, format strings) inside `secure_frontend/` only.
Run: `wine out/build/windows-llvm-mingw-cross/strata-secure-server.exe --help >/dev/null; echo exit=$?`
Expected: `exit=0`.
Run: `ctest --preset windows-llvm-mingw-cross 2>&1 | tail -4`
Expected: the pure-logic tests (config, secret_source, http_policy, http_request_handler, api_router, engine_protocol_client, model_inventory, chat_schema, tokenizer_pack, qwen/glm renderers, chat_generation_plan, chat_execution, `strata_secure_server_help`) pass under wine. Tests that drive sockets or child processes may fail under wine; record which in the commit message — they are validated by the MSYS2 job in Task 13, not by wine.
Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -1` → 18/18.

- [ ] **Step 8: Commit**

```bash
git add scripts/dev/bootstrap-llvm-mingw.sh secure_frontend/cmake secure_frontend/CMakePresets.json secure_frontend/include/secure_frontend/win32_prelude.hpp secure_frontend/src secure_frontend/tests secure_frontend/CMakeLists.txt
git commit -m "build: llvm-mingw cross toolchain for Windows; fix the two Windows compile errors

T2/P5 of SP0. Pinned llvm-mingw 20261006 (sha256 recorded in the script),
toolchain file, windows-llvm-mingw-cross preset, win32_prelude.hpp with
NOMINMAX/WIN32_LEAN_AND_MEAN, path::wstring() instead of u8string(). Wine
smoke: <list what passed/failed under wine>.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Socket server survives a client that closes early (S1)

**Files:**
- Create: `secure_frontend/include/secure_frontend/process_defaults.hpp`
- Create: `secure_frontend/src/process_defaults.cpp`
- Modify: `secure_frontend/src/http_socket_server.cpp` (`send_all`)
- Modify: `secure_frontend/src/secure_server_main.cpp`, `smoke_server_main.cpp`, `smoke_main.cpp` (call `install_process_defaults()` first thing in `main`)
- Modify: `secure_frontend/CMakeLists.txt` (add `process_defaults.cpp` to `secure_frontend_http_socket_server`)
- Test: `secure_frontend/tests/http_socket_server_test.cpp`

**Interfaces:**
- Produces: `void secure_frontend::install_process_defaults() noexcept;` — idempotent; on POSIX sets `SIGPIPE` to `SIG_IGN`; on Windows a no-op (Winsock has no SIGPIPE).

- [ ] **Step 1: Refactor the test file to a reusable server fixture, then add the failing test**

Replace the body of `tests/http_socket_server_test.cpp` with the fixture below plus the existing health case rewritten on top of it (keep `serves_a_loopback_request_without_outbound_connections` passing). Everything after this task adds cases to this file.

```cpp
#include "secure_frontend/http_socket_server.hpp"
#include "secure_frontend/process_defaults.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
#include "secure_frontend/win32_prelude.hpp"
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
int failures = 0;
void expect(bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kBad = INVALID_SOCKET;
void close_socket(NativeSocket s) { closesocket(s); }
int send_bytes(NativeSocket s, const std::string& b) { return send(s, b.data(), static_cast<int>(b.size()), 0); }
int recv_bytes(NativeSocket s, char* out, int n) { return recv(s, out, n, 0); }
#else
using NativeSocket = int;
constexpr NativeSocket kBad = -1;
void close_socket(NativeSocket s) { close(s); }
int send_bytes(NativeSocket s, const std::string& b) { return static_cast<int>(send(s, b.data(), b.size(), MSG_NOSIGNAL)); }
int recv_bytes(NativeSocket s, char* out, int n) { return static_cast<int>(recv(s, out, static_cast<std::size_t>(n), 0)); }
#endif

struct Fixture final {
  std::optional<secure_frontend::HttpSocketServer> server;
  std::string error;
  std::string host{"127.0.0.1"};

  explicit Fixture(secure_frontend::HttpSocketServerLimits limits = {}, std::string bind_host = "127.0.0.1")
      : host(std::move(bind_host)) {
    for (std::uint16_t port = 20000; port < 20100 && !server.has_value(); ++port) {
      const bool v6 = host.find(':') != std::string::npos;
      const std::string authority = (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
      secure_frontend::Config config{host, port, true, true, false, false};
      secure_frontend::HttpPolicy policy{{authority}, {"http://" + authority}, {}, false};
      server = secure_frontend::HttpSocketServer::bind(
          config, std::move(policy),
          [](std::string_view token) { return secure_frontend::constant_time_token_equals(token, "test-token"); },
          [](std::string_view method, std::string_view target, std::string_view) {
            if (method == "GET" && target == "/health") {
              return secure_frontend::HttpResponse{200, {{"Content-Type", "application/json; charset=utf-8"}}, "{\"status\":\"ok\"}\n"};
            }
            return secure_frontend::HttpResponse{404, {{"Content-Type", "application/json; charset=utf-8"}}, "{\"error\":\"Not Found\"}\n"};
          },
          &error, limits);
    }
  }
  std::string authority() const {
    const bool v6 = host.find(':') != std::string::npos;
    return (v6 ? "[" + host + "]" : host) + ":" + std::to_string(server->port());
  }
  std::string health_request() const {
    return "GET /health HTTP/1.1\r\nHost: " + authority() + "\r\nAuthorization: Bearer test-token\r\n\r\n";
  }
  NativeSocket connect_client() const {
    const bool v6 = host.find(':') != std::string::npos;
    NativeSocket client = socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == kBad) return kBad;
    int rc = -1;
    if (v6) {
      sockaddr_in6 a{}; a.sin6_family = AF_INET6; a.sin6_port = htons(server->port());
      inet_pton(AF_INET6, host.c_str(), &a.sin6_addr);
      rc = connect(client, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    } else {
      sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(server->port());
      inet_pton(AF_INET, host.c_str(), &a.sin_addr);
      rc = connect(client, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    }
    if (rc != 0) { close_socket(client); return kBad; }
    return client;
  }
  // Sends one full health request and returns the whole response (read to EOF).
  std::string round_trip() {
    std::string response;
    bool served = false;
    std::thread worker([&] { served = server->serve_one(&error); });
    const NativeSocket client = connect_client();
    if (client == kBad) { worker.join(); return {}; }
    (void)send_bytes(client, health_request());
    char chunk[1024];
    for (;;) { const int n = recv_bytes(client, chunk, sizeof(chunk)); if (n <= 0) break; response.append(chunk, static_cast<std::size_t>(n)); }
    close_socket(client);
    worker.join();
    if (!served) response.clear();
    return response;
  }
};

void serves_a_loopback_request_without_outbound_connections() {
  Fixture f;
  expect(f.server.has_value(), "loopback listener should bind: " + f.error);
  if (!f.server) return;
  const std::string response = f.round_trip();
  expect(response.rfind("HTTP/1.1 200 OK", 0) == 0, "authorized loopback request should receive HTTP 200");
}

// S1: a client that sends a request and closes before reading must not kill
// the process (SIGPIPE) and must not stop the listener from serving the next client.
void socket_server_survives_client_early_close() {
  Fixture f;
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  std::thread worker([&] { (void)f.server->serve_one(&f.error); });
  const NativeSocket client = f.connect_client();
  expect(client != kBad, "client should connect");
  if (client == kBad) { worker.join(); return; }
  // A body large enough that the response cannot be absorbed by the socket
  // buffer before we close; the server's send() then hits a closed peer.
  (void)send_bytes(client, f.health_request());
  close_socket(client);
  worker.join();
  // Still alive here means no SIGPIPE killed us. The next client must be served.
  const std::string response = f.round_trip();
  expect(response.rfind("HTTP/1.1 200 OK", 0) == 0, "listener must serve the next client after an early close");
}
}  // namespace

int main() {
  secure_frontend::install_process_defaults();
  serves_a_loopback_request_without_outbound_connections();
  socket_server_survives_client_early_close();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
```

Note: `HttpSocketServerLimits` and the extra `bind` parameter are introduced in Task 5. For THIS task, write the fixture without the `limits` parameter (delete the `limits` argument and the extra `bind` argument), then Task 5 adds them back. The test must compile at every task boundary.

To make S1 fail deterministically before the fix, the server has to write to a closed peer. Add this helper case instead of relying on timing: in `socket_server_survives_client_early_close`, before `send_bytes`, call `setsockopt(client, SOL_SOCKET, SO_RCVBUF, ...)` with a 1-byte buffer and make the dispatcher response body 256 KiB for target `/big` (add a `/big` branch to the fixture dispatcher: `return HttpResponse{200, {{"Content-Type","text/plain"}}, std::string(256U * 1024U, 'x')};`), request `/big`, then close immediately. With a 1-byte receive buffer the server's `send()` blocks until the peer closes and then fails with `EPIPE`.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R http_socket_server --output-on-failure`
Expected: FAIL — the test process dies with SIGPIPE (`Exception: SegFault`/`Subprocess aborted` or exit by signal 13) or `FAILED: listener must serve the next client`.

- [ ] **Step 3: Implement `process_defaults`**

`include/secure_frontend/process_defaults.hpp`:
```cpp
#pragma once

namespace secure_frontend {

// Process-wide defaults every frontend executable (and every test binary
// that opens sockets) installs first thing in main(): a peer that closes a
// socket early must produce an error return from send(), never a SIGPIPE
// that terminates the service. Idempotent. No effect on Windows.
void install_process_defaults() noexcept;

}  // namespace secure_frontend
```
`src/process_defaults.cpp`:
```cpp
#include "secure_frontend/process_defaults.hpp"

#if !defined(_WIN32)
#include <csignal>
#endif

namespace secure_frontend {

void install_process_defaults() noexcept {
#if !defined(_WIN32)
  struct sigaction action {};
  action.sa_handler = SIG_IGN;
  sigemptyset(&action.sa_mask);
  (void)sigaction(SIGPIPE, &action, nullptr);
#endif
}

}  // namespace secure_frontend
```
In `http_socket_server.cpp` `send_all`, POSIX branch:
```cpp
#if defined(MSG_NOSIGNAL)
    const ssize_t result = send(socket, bytes.data() + sent, remaining, MSG_NOSIGNAL);
#else
    const ssize_t result = send(socket, bytes.data() + sent, remaining, 0);
#endif
```
and on Apple-like platforms without `MSG_NOSIGNAL`, set `SO_NOSIGPIPE` on the accepted socket in `serve_one` right after `accept`:
```cpp
#if defined(SO_NOSIGPIPE)
  { const int one = 1; (void)setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); }
#endif
```
CMake: `add_library(secure_frontend_http_socket_server src/http_socket_server.cpp src/process_defaults.cpp)`.
Mains: first statement of `main()` in `secure_server_main.cpp`, `smoke_server_main.cpp`, `smoke_main.cpp`: `secure_frontend::install_process_defaults();` with `#include "secure_frontend/process_defaults.hpp"`.

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc --output-on-failure | tail -3`
Expected: 18/18 passed.

- [ ] **Step 5: Commit**

```bash
git add secure_frontend
git commit -m "socket server: survive a client that closes early (S1)

Regression test: socket_server_survives_client_early_close. install_process_defaults()
ignores SIGPIPE; send() uses MSG_NOSIGNAL (SO_NOSIGPIPE where that is the only option).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Send timeout and a total request deadline (S2, S3)

**Files:**
- Modify: `secure_frontend/include/secure_frontend/http_socket_server.hpp`
- Modify: `secure_frontend/src/http_socket_server.cpp`
- Test: `secure_frontend/tests/http_socket_server_test.cpp`

**Interfaces:**
- Produces:
```cpp
struct HttpSocketServerLimits final {
  std::chrono::milliseconds request_deadline{std::chrono::seconds(30)};  // header+body, total
  std::size_t max_request_bytes{64U * 1024U};
};
static std::optional<HttpSocketServer> bind(const Config&, HttpPolicy, BearerTokenValidator,
                                            RequestDispatcher, std::string* error,
                                            HttpSocketServerLimits limits = {});
```
  All existing callers compile unchanged (defaulted last parameter). SP1 replaces the struct with typed configuration.

- [ ] **Step 1: Add the failing tests**

Append to the test file (and add the `limits` parameter back into `Fixture` as shown in Task 4):

```cpp
// S3: one byte every 400 ms must not hold the listener past the deadline.
void socket_server_rejects_slow_request_past_deadline() {
  Fixture f({.request_deadline = std::chrono::milliseconds(1000)});
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  bool served = true;
  const auto start = std::chrono::steady_clock::now();
  std::thread worker([&] { served = f.server->serve_one(&f.error); });
  const NativeSocket client = f.connect_client();
  if (client == kBad) { worker.join(); expect(false, "client should connect"); return; }
  const std::string request = f.health_request();
  for (std::size_t i = 0; i < request.size() - 1; ++i) {  // never send the final byte
    if (send_bytes(client, request.substr(i, 1)) <= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(4)) break;
  }
  worker.join();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  close_socket(client);
  expect(!served, "a dripping client must be rejected, not served");
  expect(elapsed >= std::chrono::milliseconds(900) && elapsed < std::chrono::milliseconds(2500),
         "listener must give up at the deadline, not per-recv");
}

// S2: a peer that never reads must not block send_all forever.
void socket_server_send_times_out_on_stalled_reader() {
  Fixture f({.request_deadline = std::chrono::milliseconds(1000)});
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  bool served = true;
  std::thread worker([&] { served = f.server->serve_one(&f.error); });
  const NativeSocket client = f.connect_client();
  if (client == kBad) { worker.join(); expect(false, "client should connect"); return; }
  const int tiny = 1;
  (void)setsockopt(client, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&tiny), sizeof(tiny));
  (void)send_bytes(client, "GET /big HTTP/1.1\r\nHost: " + f.authority() + "\r\nAuthorization: Bearer test-token\r\n\r\n");
  const auto start = std::chrono::steady_clock::now();
  worker.join();  // must return without us ever reading
  const auto elapsed = std::chrono::steady_clock::now() - start;
  close_socket(client);
  expect(!served && elapsed < std::chrono::seconds(3), "send to a stalled reader must time out at the deadline");
}
```
Add both to `main()`.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset linux-gcc 2>&1 | tail -3`
Expected: compile error (`HttpSocketServerLimits` undeclared). Add the struct and parameter (Step 3 header only), rebuild, run `ctest -R http_socket_server --output-on-failure`: expected FAIL on both new cases (slow request served or takes > 2.5 s; stalled reader blocks until the 5 s recv timeout cannot apply to send, so the test hangs or exceeds 3 s).

- [ ] **Step 3: Implement**

Header: add `#include <chrono>` and the struct above; add the defaulted parameter to `bind`.
Source changes in `http_socket_server.cpp`:
- `State` gains `HttpSocketServerLimits limits;` set in `bind`.
- Replace `set_timeout(socket)` with:
```cpp
bool set_socket_timeouts(const NativeSocket socket, const std::chrono::milliseconds remaining) {
  const auto clamped = std::clamp<long long>(remaining.count(), 1, 3600000);
#ifdef _WIN32
  const DWORD ms = static_cast<DWORD>(clamped);
  return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms)) == 0 &&
         setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms)) == 0;
#else
  timeval tv{static_cast<time_t>(clamped / 1000), static_cast<suseconds_t>((clamped % 1000) * 1000)};
  return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
         setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}
```
- In `bind`: do NOT set any timeout on the listening socket (Task 6 relies on this; it is harmless to do it now).
- In `serve_one`: after `accept`, compute `const auto deadline = std::chrono::steady_clock::now() + state_->limits.request_deadline;`. Before every `recv`, compute `remaining = deadline - now`; if `remaining <= 0` → send a `408 Request Timeout` (add `case 408: return "Request Timeout";` to `reason_phrase`), set `*error = "request deadline exceeded"`, return false; else `set_socket_timeouts(client, remaining)`. Before `send_all`, set the timeouts once more from the remaining budget (minimum 1 s so a legitimate large response can still go out: `std::max(remaining, 1000ms)`).
- Use `state_->limits.max_request_bytes` everywhere `kMaximumRequestBytes` was used.

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc --output-on-failure | tail -3`
Expected: 18/18 (the socket test binary now runs four cases).

- [ ] **Step 5: Commit**

```bash
git add secure_frontend
git commit -m "socket server: total request deadline and send timeout (S2, S3)

Regression tests: socket_server_rejects_slow_request_past_deadline,
socket_server_send_times_out_on_stalled_reader. HttpSocketServerLimits
{request_deadline=30s, max_request_bytes=64KiB} is a defaulted bind() parameter
until SP1 introduces typed configuration.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: Oversize over TCP, idle accept, IPv6-only, response completeness, mains loop (S4–S7)

**Files:**
- Modify: `secure_frontend/include/secure_frontend/http_socket_server.hpp` (add `listener_failed()`)
- Modify: `secure_frontend/src/http_socket_server.cpp`
- Modify: `secure_frontend/src/smoke_main.cpp:81`, `smoke_server_main.cpp` loop, `secure_server_main.cpp` loop
- Test: `secure_frontend/tests/http_socket_server_test.cpp`

**Interfaces:**
- Produces: `[[nodiscard]] bool HttpSocketServer::listener_failed() const noexcept;` — true once `accept()` failed with a non-transient error (anything other than `EINTR`, `ECONNABORTED`, `EAGAIN`/`EWOULDBLOCK`, `EMFILE`/`ENFILE`); mains stop their loop only then.

- [ ] **Step 1: Add the failing tests**

```cpp
// S4: 64 KiB + 1 of headers over TCP → bounded 400/413, listener stays usable.
void socket_server_rejects_oversize_request_over_tcp() {
  Fixture f({.request_deadline = std::chrono::seconds(5), .max_request_bytes = 64U * 1024U});
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  bool served = true;
  std::thread worker([&] { served = f.server->serve_one(&f.error); });
  const NativeSocket client = f.connect_client();
  if (client == kBad) { worker.join(); expect(false, "client should connect"); return; }
  std::string request = "GET /health HTTP/1.1\r\nHost: " + f.authority() + "\r\nX-Pad: ";
  request.append(64U * 1024U + 1U, 'a');
  request += "\r\n\r\n";
  (void)send_bytes(client, request);
  std::string response; char chunk[512];
  for (;;) { const int n = recv_bytes(client, chunk, sizeof(chunk)); if (n <= 0) break; response.append(chunk, static_cast<std::size_t>(n)); }
  close_socket(client);
  worker.join();
  expect(!served, "oversize request is a client error");
  expect(response.rfind("HTTP/1.1 4", 0) == 0, "oversize request must get a 4xx, got: " + response.substr(0, 20));
  expect(f.round_trip().rfind("HTTP/1.1 200 OK", 0) == 0, "listener must serve the next client after an oversize request");
}

// S5: no client for 7 s must NOT make serve_one return (the old 5 s SO_RCVTIMEO on the listener did).
void socket_server_accept_waits_without_timeout() {
  Fixture f;
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  bool returned = false;
  std::thread worker([&] { (void)f.server->serve_one(&f.error); returned = true; });
  std::this_thread::sleep_for(std::chrono::seconds(7));
  expect(!returned, "accept must block while idle; serve_one returned: " + f.error);
  expect(!f.server->listener_failed(), "idle is not a listener failure");
  // Unblock the worker with a real request so the thread can join.
  const NativeSocket client = f.connect_client();
  if (client != kBad) { (void)send_bytes(client, f.health_request()); char c[64]; (void)recv_bytes(client, c, sizeof(c)); close_socket(client); }
  worker.join();
}

// S6: '::1' must be v6-only and must serve with a bracketed Host.
void socket_server_ipv6_bind_is_v6only() {
  Fixture f({}, "::1");
  if (!f.server) { std::cerr << "SKIP ipv6: " << f.error << '\n'; return; }
  expect(f.round_trip().rfind("HTTP/1.1 200 OK", 0) == 0, "IPv6 loopback must serve with bracketed Host");
  // An IPv4 connect to the same port must be refused (no dual-stack mapping).
  NativeSocket v4 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(f.server->port()); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  const int rc = connect(v4, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
  close_socket(v4);
  expect(rc != 0, "an IPv6 listener must not accept IPv4 clients");
}

// S7: Content-Length matches the body and the whole body arrives.
void socket_server_response_is_complete() {
  Fixture f;
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  const std::string response = f.round_trip();
  const auto header_end = response.find("\r\n\r\n");
  expect(header_end != std::string::npos, "response must have a header block");
  if (header_end == std::string::npos) return;
  const auto cl = response.find("Content-Length: ");
  expect(cl != std::string::npos && cl < header_end, "Content-Length must be present");
  if (cl == std::string::npos) return;
  const std::size_t declared = std::stoul(response.substr(cl + 16, response.find("\r\n", cl) - cl - 16));
  expect(response.size() - header_end - 4 == declared, "body length must equal Content-Length");
}
```
Add all four to `main()`.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R http_socket_server --output-on-failure`
Expected: compile error (`listener_failed` missing) → add the declaration returning `false` for now → FAIL on S5 (`serve_one returned: accept failed`) and S6 (`IPv6 listener must not accept IPv4 clients` if the kernel default is dual-stack; if your kernel has `bindv6only=1` the case passes already — note that in the commit) and possibly S4.

- [ ] **Step 3: Implement**

In `bind`:
```cpp
  { const int one = 1;
#ifdef _WIN32
    (void)setsockopt(state->socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof(one));
#else
    (void)setsockopt(state->socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    if (family == AF_INET6) {
      if (setsockopt(state->socket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&one), sizeof(one)) != 0) {
        if (error != nullptr) *error = "listener socket setup failed";
        return std::nullopt;
      }
    }
  }
```
Remove the timeout call from the listener socket. In `serve_one`, after a failed `accept`:
```cpp
  if (client == kInvalidSocket) {
#ifdef _WIN32
    const int code = WSAGetLastError();
    const bool transient = code == WSAEINTR || code == WSAECONNABORTED || code == WSAEWOULDBLOCK || code == WSAEMFILE;
#else
    const int code = errno;
    const bool transient = code == EINTR || code == ECONNABORTED || code == EAGAIN || code == EWOULDBLOCK || code == EMFILE || code == ENFILE;
#endif
    if (!transient) state_->listener_failed = true;
    if (error != nullptr) *error = transient ? "accept interrupted" : "accept failed";
    return false;
  }
```
`State` gains `bool listener_failed{false};`; add `bool HttpSocketServer::listener_failed() const noexcept { return state_ && state_->listener_failed; }`.
Oversize: when the loop breaks because the bound is exceeded, respond `413` (`case 413: return "Content Too Large";`) instead of 400 and keep returning false.
Mains:
- `smoke_main.cpp:81` → 
```cpp
  for (;;) {
    if (!server->serve_one(&error) && server->listener_failed()) { std::cerr << "listener stopped: " << error << '\n'; return EXIT_FAILURE; }
  }
```
- `secure_server_main.cpp` and `smoke_server_main.cpp` loops: same shape (break only on `listener_failed()`).

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc --output-on-failure | tail -3`
Expected: 18/18 (socket test now ~8 cases, ~10 s because of the idle case).

- [ ] **Step 5: Commit**

```bash
git add secure_frontend
git commit -m "socket server: oversize 413, blocking accept, IPV6_V6ONLY, complete responses; mains stop only on listener failure (S4-S7)

Regression tests: socket_server_rejects_oversize_request_over_tcp,
socket_server_accept_waits_without_timeout, socket_server_ipv6_bind_is_v6only,
socket_server_response_is_complete. Fixes secure-frontend-smoke exiting after 5 s idle.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 7: Total generation deadline and STOP/drain recovery (E1, E2)

**Files:**
- Modify: `secure_frontend/include/secure_frontend/engine_protocol_client.hpp` (add `idle()`)
- Modify: `secure_frontend/src/engine_protocol_client.cpp:144-175` (`read_generation`)
- Modify: `secure_frontend/include/secure_frontend/chat_execution.hpp` (`ChatExecutionLimits::stop_drain_timeout`, `ChatExecutionErrorCode::generation_timed_out`)
- Modify: `secure_frontend/src/chat_execution.cpp` (timeout handling)
- Test: `secure_frontend/tests/engine_protocol_client_test.cpp`, `secure_frontend/tests/chat_execution_test.cpp`

**Interfaces:**
- Produces: `bool EngineProtocolClient::idle() const noexcept;` (state is exactly `idle`); `ChatExecutionLimits::stop_drain_timeout{std::chrono::seconds(5)}`; `ChatExecutionErrorCode::generation_timed_out`; `read_generation(timeout, ...)` where `timeout` is the TOTAL budget for the whole generation.
- Consumes: `FakeTransport` idiom from the existing tests.

- [ ] **Step 1: Add the failing tests**

In `engine_protocol_client_test.cpp`, give `FakeTransport` a per-read delay and add:
```cpp
  std::chrono::milliseconds delay_per_read{0};
  // in read_line(): if (delay_per_read.count() > 0) std::this_thread::sleep_for(delay_per_read);
```
(add `#include <thread>`), then:
```cpp
// E1: the timeout is a total budget, not a per-line allowance.
void generation_total_deadline_is_enforced() {
  FakeTransport transport;
  transport.reads = {line("READY 64 stop")};
  for (int i = 0; i < 20; ++i) transport.reads.push_back(line("T 1"));
  transport.reads.push_back(line("DONE 20 1 1.0 1.0 length"));
  transport.delay_per_read = std::chrono::milliseconds(100);
  EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  (void)client.await_ready(std::chrono::seconds(1), ready);
  (void)client.begin_generation(20, {1});
  secure_frontend::EngineGeneration generation;
  const auto start = std::chrono::steady_clock::now();
  const auto error = client.read_generation(std::chrono::milliseconds(350), generation);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  expect(error.has_value() && error->code == EngineProtocolErrorCode::timeout, "20 lines at 100 ms each must exceed a 350 ms total budget");
  expect(elapsed < std::chrono::milliseconds(900), "the client must stop reading at the total deadline");
  expect(client.generating() && !client.idle(), "a timeout leaves the generation active for STOP/drain");
}
```
In `chat_execution_test.cpp` add a `std::chrono::milliseconds delay_per_read{0}` to its `FakeTransport` the same way, and:
```cpp
// E2: after a timeout the service sends STOP, drains to DONE, and serves the next request.
void chat_recovers_after_generation_timeout() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt + "OK");
  const auto o = token_id_for(tokenizer, "O");
  FakeTransport transport;
  transport.reads = {line("READY 512 stop")};
  for (int i = 0; i < 10; ++i) transport.reads.push_back(line("T " + std::to_string(o)));
  transport.reads.push_back(line("DONE 10 1 1.0 1.0 cancel"));   // the drain after STOP
  transport.reads.push_back(line("T " + std::to_string(o)));      // the next request
  transport.reads.push_back(line("DONE 1 1 1.0 1.0 length"));
  transport.delay_per_read = std::chrono::milliseconds(60);
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::ChatExecutionService service(protocol, {.generation_timeout = std::chrono::milliseconds(200), .stop_drain_timeout = std::chrono::seconds(2)});
  const auto first = service.execute(valid_request(10U), tokenizer);
  const auto* first_error = std::get_if<secure_frontend::ChatExecutionError>(&first);
  expect(first_error != nullptr && first_error->code == secure_frontend::ChatExecutionErrorCode::generation_timed_out, "a slow generation must time out");
  expect(std::count(transport.writes.begin(), transport.writes.end(), "STOP") == 1, "exactly one STOP must be sent after the timeout");
  expect(protocol.idle(), "the protocol must be idle again after the drain");
  transport.delay_per_read = std::chrono::milliseconds(0);
  const auto second = service.execute(valid_request(1U), tokenizer);
  expect(std::holds_alternative<secure_frontend::ChatExecutionResult>(second), "the next request must succeed after recovery");
}

void chat_marks_engine_failed_when_drain_fails() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt + "O");
  const auto o = token_id_for(tokenizer, "O");
  FakeTransport transport;
  transport.reads = {line("READY 512 stop")};
  for (int i = 0; i < 10; ++i) transport.reads.push_back(line("T " + std::to_string(o)));
  // No DONE ever arrives: the drain must give up.
  transport.delay_per_read = std::chrono::milliseconds(60);
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::ChatExecutionService service(protocol, {.generation_timeout = std::chrono::milliseconds(150), .stop_drain_timeout = std::chrono::milliseconds(300)});
  const auto result = service.execute(valid_request(10U), tokenizer);
  const auto* error = std::get_if<secure_frontend::ChatExecutionError>(&result);
  expect(error != nullptr && error->code == secure_frontend::ChatExecutionErrorCode::engine_protocol_failed, "a failed drain is a protocol failure");
  expect(protocol.closed(), "a failed drain must close the protocol so health reports it");
}

// Review Focus 3: DONE ... nonfinite is a 503 for this request only.
void chat_rejects_nonfinite_done_and_stays_ready() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt + "O");
  const auto o = token_id_for(tokenizer, "O");
  FakeTransport transport;
  transport.reads = {line("READY 512 stop"), line("DONE 0 1 1.0 1.0 nonfinite"), line("T " + std::to_string(o)), line("DONE 1 1 1.0 1.0 length")};
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::ChatExecutionService service(protocol);
  const auto first = service.execute(valid_request(1U), tokenizer);
  const auto* error = std::get_if<secure_frontend::ChatExecutionError>(&first);
  expect(error != nullptr && error->code == secure_frontend::ChatExecutionErrorCode::engine_protocol_failed, "nonfinite must not produce text");
  expect(protocol.idle(), "nonfinite does not kill the engine; the protocol stays idle");
  const auto second = service.execute(valid_request(1U), tokenizer);
  expect(std::holds_alternative<secure_frontend::ChatExecutionResult>(second), "the engine is usable after a nonfinite request");
}
```
Add `#include <algorithm>` and `#include <thread>`; register the cases in both `main()`s.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset linux-gcc 2>&1 | grep -m3 error`
Expected: compile errors for `idle`, `stop_drain_timeout`, `generation_timed_out`. Add only the declarations (`idle()` returning `state_ == State::idle`, the new limit field, the new enum value), rebuild, run `ctest -R "protocol_client|chat_execution" --output-on-failure`: FAIL on all four new cases.

- [ ] **Step 3: Implement**

`read_generation`:
```cpp
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return EngineProtocolError{EngineProtocolErrorCode::timeout, "engine generation read timed out"};
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const auto response = transport_.read_line(remaining);
    ... (unchanged handling)
```
In the DONE handling of `read_generation`, parse the finish field (5th whitespace-separated field after `DONE`); if it is `nonfinite`, set `state_ = State::idle` and return `EngineProtocolError{EngineProtocolErrorCode::engine_error, "engine reported a non-finite result"}` WITHOUT calling `fail()` (the engine is alive and will reset on the next GEN). All other finishes keep the existing behavior.

`ChatExecutionService::execute` after `read_generation` returns a `timeout` error:
```cpp
  if (error->code == EngineProtocolErrorCode::timeout) {
    if (const auto stop_error = protocol_.stop(); stop_error.has_value()) {
      return ChatExecutionError{ChatExecutionErrorCode::engine_protocol_failed, "engine could not be stopped"};
    }
    EngineGeneration drained;
    const auto drain_error = protocol_.read_generation(limits_.stop_drain_timeout, drained);
    if (drain_error.has_value()) {
      // read_generation already closed the protocol on terminal failures; a second
      // timeout means the engine ignores STOP, so close it ourselves via quit().
      if (drain_error->code == EngineProtocolErrorCode::timeout) (void)protocol_.quit();
      return ChatExecutionError{ChatExecutionErrorCode::engine_protocol_failed, "engine did not drain after STOP"};
    }
    return ChatExecutionError{ChatExecutionErrorCode::generation_timed_out, "generation exceeded the time limit"};
  }
```
If `quit()` cannot be used from the `generating` state (check `quit()`'s state guard), add a private `fail()` path: give `EngineProtocolClient` a public `void abandon() noexcept;` that sets `state_ = State::failed` and use it instead of `quit()`. Either way `protocol.closed()` must be true afterwards.

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc --output-on-failure | tail -3`
Expected: 18/18.

- [ ] **Step 5: Commit**

```bash
git add secure_frontend
git commit -m "engine protocol: total generation deadline; chat recovers via STOP/drain (E1, E2)

Regression tests: generation_total_deadline_is_enforced,
chat_recovers_after_generation_timeout, chat_marks_engine_failed_when_drain_fails,
chat_rejects_nonfinite_done_and_stays_ready. A timeout no longer wedges every
later request into 503.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 8: `/health` reflects protocol state (E3)

**Files:**
- Modify: `secure_frontend/include/secure_frontend/chat_execution.hpp` (add `EngineHealth`, `health()`)
- Modify: `secure_frontend/src/chat_execution.cpp`
- Modify: `secure_frontend/src/secure_server_main.cpp` (health provider)
- Test: `secure_frontend/tests/chat_execution_test.cpp`, `secure_frontend/tests/api_router_test.cpp` (only if the router does not already return 503 for a false provider — read it first)

**Interfaces:**
- Produces:
```cpp
enum class EngineHealth { starting, ready, generating, failed };
[[nodiscard]] EngineHealth ChatExecutionService::health() const noexcept;
```
  `starting` before READY; `ready` when `protocol_.idle()`; `generating` when `protocol_.generating()`; `failed` when `protocol_.closed()`.

- [ ] **Step 1: Add the failing tests**

```cpp
void health_reflects_protocol_state() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt + "O");
  const auto o = token_id_for(tokenizer, "O");
  FakeTransport transport;
  transport.reads = {line("READY 512 stop"), line("T " + std::to_string(o)), line("DONE 1 1 1.0 1.0 length")};
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::ChatExecutionService service(protocol);
  expect(service.health() == secure_frontend::EngineHealth::starting, "before READY the engine is starting");
  (void)service.execute(valid_request(1U), tokenizer);
  expect(service.health() == secure_frontend::EngineHealth::ready, "after a completed request the engine is ready");
}

// Review Focus 4: the engine closing stdout mid-generation is a failure, not text.
void health_is_failed_after_engine_closed_mid_generation() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt + "O");
  const auto o = token_id_for(tokenizer, "O");
  FakeTransport transport;
  transport.reads = {line("READY 512 stop"), line("T " + std::to_string(o)),
                     {secure_frontend::EngineReadKind::closed, {}, {}}};
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::ChatExecutionService service(protocol);
  const auto result = service.execute(valid_request(2U), tokenizer);
  expect(std::holds_alternative<secure_frontend::ChatExecutionError>(result), "a closed engine must not yield partial text");
  expect(service.health() == secure_frontend::EngineHealth::failed, "health must report failed after the engine closed");
}
```

- [ ] **Step 2: Run to verify they fail** — compile error → add the enum and `health()` returning `starting` → FAIL on both.

- [ ] **Step 3: Implement**

```cpp
EngineHealth ChatExecutionService::health() const noexcept {
  if (protocol_.closed()) return EngineHealth::failed;
  if (!received_ready_) return EngineHealth::starting;
  if (protocol_.generating()) return EngineHealth::generating;
  return EngineHealth::ready;
}
```
(`received_ready_` already exists as a private member.) In `secure_server_main.cpp` the health provider becomes:
```cpp
  const secure_frontend::ApiRouter router(
      [&engine, &chat] { return engine.running() && chat.health() != secure_frontend::EngineHealth::failed; },
      [models] { return models; });
```
Confirm in `api_router.cpp` that a false provider yields `503` with a body that contains no engine details; if it yields 200 with `"status":"degraded"`, change it to 503 and update `api_router_test.cpp` accordingly.

- [ ] **Step 4: Run to verify they pass** — `ctest --preset linux-gcc | tail -1` → 18/18.

- [ ] **Step 5: Commit**

```bash
git add secure_frontend
git commit -m "health: report engine protocol state, 503 on failure (E3)

Regression tests: health_reflects_protocol_state,
health_is_failed_after_engine_closed_mid_generation.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 9: Limits sized from the engine's READY context (E4)

**Files:**
- Modify: `secure_frontend/include/secure_frontend/engine_protocol_client.hpp` (`EngineProtocolLimits` semantics)
- Modify: `secure_frontend/src/engine_protocol_client.cpp` (`await_ready`, `begin_generation`)
- Modify: `secure_frontend/include/secure_frontend/engine_stdio_process.hpp` (`kMaxLineBytes` split into read/write caps)
- Modify: `secure_frontend/src/engine_stdio_process.cpp` (`write_line`, `read_line`)
- Modify: `secure_frontend/src/chat_execution.cpp` (context check uses `+ 8`)
- Create: `docs/security/engine-protocol-limits.md`
- Test: `secure_frontend/tests/engine_protocol_client_test.cpp`, `secure_frontend/tests/chat_execution_test.cpp`, `secure_frontend/tests/engine_stdio_process_test.cpp`

**Interfaces:**
- Produces:
```cpp
struct EngineProtocolLimits final {
  // 0 means "derive from READY": 32 + 11 * max_context_tokens (10 digits + separator per id).
  std::size_t max_line_bytes{0};
  std::size_t max_prompt_tokens{262144};
  std::size_t max_generated_tokens{262144};
  std::uint32_t max_new_tokens{262144};
};
// EngineStdioProcess
static constexpr std::size_t kMaxReadLineBytes = 8192;              // engine → frontend records are short
static constexpr std::size_t kMaxWriteLineBytes = 16U * 1024U * 1024U;  // GEN line; bounded above by the protocol client
```
  `ChatExecutionService` rejects a request when `prompt_tokens + max_tokens + 8 > engine_context_tokens_` with `context_limit_exceeded` (the engine's own rule).

- [ ] **Step 1: Add the failing tests**

`engine_protocol_client_test.cpp`:
```cpp
// E4: a prompt at the engine's advertised context must fit the GEN line.
void gen_line_fits_max_prompt_tokens() {
  FakeTransport transport;
  transport.reads = {line("READY 4096 stop"), line("DONE 0 4000 1.0 1.0 length")};
  EngineProtocolClient client(transport);  // default limits: derive from READY
  secure_frontend::EngineReady ready{};
  (void)client.await_ready(std::chrono::seconds(1), ready);
  std::vector<std::uint32_t> prompt(4000U, 4294967295U);  // worst case: 10-digit ids
  expect(!client.begin_generation(1, prompt).has_value(), "4000 ten-digit ids must fit the derived GEN line limit");
  expect(transport.writes.size() == 1U && transport.writes.front().size() > 40000U, "the GEN line must actually carry every id");
}
```
`chat_execution_test.cpp`:
```cpp
void chat_applies_engine_context_rule_with_eight_token_margin() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt);
  // Count the prompt tokens by running one successful request first.
  FakeTransport probe; probe.reads = {line("READY 4096 stop"), line("DONE 0 1 1.0 1.0 length")};
  secure_frontend::EngineProtocolClient probe_protocol(probe);
  secure_frontend::ChatExecutionService probe_service(probe_protocol);
  (void)probe_service.execute(valid_request(1U), tokenizer);
  const std::string& gen = probe.writes.front();
  const std::size_t prompt_tokens = static_cast<std::size_t>(std::count(gen.begin(), gen.end(), ',')) + 1U;
  // ctx = prompt + max_new + 7 → exceeds by one; ctx = prompt + max_new + 8 → fits.
  for (const std::size_t margin : {7U, 8U}) {
    FakeTransport t; t.reads = {line("READY " + std::to_string(prompt_tokens + 1U + margin) + " stop"), line("DONE 0 1 1.0 1.0 length")};
    secure_frontend::EngineProtocolClient p(t);
    secure_frontend::ChatExecutionService s(p);
    const auto r = s.execute(valid_request(1U), tokenizer);
    const bool rejected = std::holds_alternative<secure_frontend::ChatExecutionError>(r);
    expect(rejected == (margin == 7U), "context rule must be prompt + max_new + 8 <= ctx (margin " + std::to_string(margin) + ")");
  }
}
```
`engine_stdio_process_test.cpp`: in `test_round_trip`, after READY, write a 100 000-byte line and expect success:
```cpp
  std::string big(100000U, '1'); std::string werr;
  expect(transport.write_line("GEN 1 " + big, werr), "a 100 KB GEN line must be writable: " + werr);
  // The fake engine answers any GEN with T 7 / DONE.
  secure_frontend::EngineGeneration g{};
```
(Place it so the protocol client state stays consistent: do this with a raw `write_line` BEFORE constructing the `EngineProtocolClient`, and read the fake's reply with `transport.read_line` twice.)

- [ ] **Step 2: Run to verify they fail** — `gen_line_fits_max_prompt_tokens` fails with `generation command exceeds line limit`; the margin test fails on margin 8 (current check presumably has no `+ 8`); the stdio test fails with `protocol line is invalid`.

- [ ] **Step 3: Implement**

- `await_ready`: after parsing `READY <ctx>`, if `limits_.max_line_bytes == 0` set `effective_max_line_bytes_ = 32U + 11U * static_cast<std::size_t>(ready.max_context_tokens);` else `effective_max_line_bytes_ = limits_.max_line_bytes;` (new private member, default 8192 until READY). `begin_generation` compares against `effective_max_line_bytes_`.
- `EngineStdioProcess::write_line`: cap at `kMaxWriteLineBytes`; `read_line`: cap at `kMaxReadLineBytes` (both platforms — the Windows oversize check lands in Task 11, but use the new constant name now).
- `chat_execution.cpp`: wherever the context check is, make it `if (prompt_tokens + request.max_tokens + 8U > engine_context_tokens_) return context_limit_exceeded`.
- Create `docs/security/engine-protocol-limits.md`:

```markdown
# Engine protocol limits (Strata v0.1.43, `src/program/generate.cpp`)

Facts the frontend relies on; verified against the engine source on 2026-10-13.

| Fact | Engine behavior | Frontend consequence |
| --- | --- | --- |
| stdin line length | Unbounded `std::string`; no truncation, no error | Frontend caps its own GEN line at 32 + 11 × ctx bytes and never relies on the engine to reject |
| Prompt form | One line: `GEN <max_new> [k=v ...] <id,id,...>`; no multi-line form | Non-streaming prompt only; one request in flight |
| Context rule | `n + max_new + 8 <= --max-context` else `ERR ...` and the engine stays alive | Frontend rejects with 400 before GEN using the same rule |
| READY | `READY <max_context> stop` | ctx parsed from READY sizes every limit; a fixed 32768 is never advertised |
| STOP | Honored only after the GEN has been picked up; always followed by `DONE ... cancel` | Frontend sends STOP only after a generation has started; drains to DONE |
| `nonfinite` | `DONE ... nonfinite`; next request restarts from token 0 | 503 for that request; protocol stays idle |
| Model path | `--native <shard1.gguf>` or `--pack DIR`; no positional args | Frontend owns `--native` (Task 12) |
| Network | None in the engine; no host/port/bind/http flags | Deny-list covers only frontend-owned flags |
| Unsupported modes | `--batch` (BT/BDONE), `--vision` (GENI) | Rejected in `--engine-arg` until the frontend implements their protocol |
```

- [ ] **Step 4: Run to verify they pass** — 18/18.

- [ ] **Step 5: Commit**

```bash
git add secure_frontend docs/security/engine-protocol-limits.md
git commit -m "engine protocol: size limits from READY context, +8 context rule, 16 MiB write cap (E4)

Regression tests: gen_line_fits_max_prompt_tokens,
chat_applies_engine_context_rule_with_eight_token_margin, 100 KB GEN line in
engine_stdio_process_tests. Facts recorded in docs/security/engine-protocol-limits.md.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 10: POSIX launch hygiene — pre-fork argv, O_CLOEXEC, fd sweep, reap (P1–P3)

**Files:**
- Modify: `secure_frontend/src/engine_stdio_process.cpp` (POSIX `start`, `terminate`)
- Modify: `secure_frontend/src/engine_process.cpp` (POSIX `start`, same pattern)
- Test: `secure_frontend/tests/engine_stdio_process_test.cpp` (extend the fake engine)

**Interfaces:**
- Produces: fake engine protocol extension used by SP2 later — on `DUMP`, the fake prints `FDS <comma-separated open fds>` and `ARGV <argc> <argv joined by 0x1F>` then continues.

- [ ] **Step 1: Add the failing tests**

Extend `fake_engine()`:
```cpp
    } else if (line == "DUMP") {
      std::string fds;
      for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        if (!fds.empty()) fds += ',';
        fds += entry.path().filename().string();
      }
      std::cout << "FDS " << fds << '\n';
      std::cout << "ARGV";
      for (int i = 0; i < g_argc; ++i) std::cout << '\x1f' << g_argv[i];
      std::cout << '\n' << std::flush;
```
(store `argc/argv` in file-scope `g_argc`/`g_argv` from `main`.) Note the directory iterator itself holds one fd while iterating; accept `0,1,2` plus at most one extra fd whose number is > 2 **and** that is not present when listing twice — simpler: have the fake call `std::filesystem::directory_iterator` and then filter out the iterator's own fd by reading `/proc/self/fd/<n>` link targets and dropping the one pointing at `/proc/<pid>/fd`.

Tests:
```cpp
void child_inherits_only_stdio_descriptors(const std::filesystem::path& self) {
  // Open two extra descriptors in the parent without CLOEXEC; a correct launcher must not leak them.
  int leak_a = open("/dev/null", O_RDONLY); int leak_b = open("/dev/null", O_RDONLY);
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = self, .arguments = {"--fake-engine"}, .environment = {}}).has_value(), "fake engine must start");
  std::string err;
  (void)transport.read_line(std::chrono::seconds(2));  // READY
  expect(transport.write_line("DUMP", err), "DUMP must write");
  const auto fds = transport.read_line(std::chrono::seconds(2));
  (void)transport.read_line(std::chrono::seconds(2));  // ARGV
  expect(fds.kind == secure_frontend::EngineReadKind::line && fds.line == "FDS 0,1,2", "child must see only stdio, saw: " + fds.line);
  close(leak_a); close(leak_b);
  (void)transport.terminate(std::chrono::milliseconds(250));
}

void child_receives_exact_argv_and_env(const std::filesystem::path& self) {
  secure_frontend::EngineStdioProcess transport;
  const std::string weird = "literal; $(echo x) \"quoted\" \xE2\x9C\x93 \n-less";
  expect(!transport.start({.executable = self, .arguments = {"--fake-engine", weird}, .environment = {{"STRATA_T", "1"}}}).has_value(), "fake engine must start");
  std::string err;
  (void)transport.read_line(std::chrono::seconds(2));
  (void)transport.write_line("DUMP", err);
  (void)transport.read_line(std::chrono::seconds(2));  // FDS
  const auto argv = transport.read_line(std::chrono::seconds(2));
  const std::string expected = std::string("ARGV\x1f") + self.string() + "\x1f--serve\x1f--fake-engine\x1f" + weird;
  expect(argv.line == expected, "argv must arrive literally, no shell: " + argv.line);
  (void)transport.terminate(std::chrono::milliseconds(250));
}

void terminate_reaps_killed_child(const std::filesystem::path& self) {
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = self, .arguments = {"--fake-engine", "--ignore-term"}, .environment = {}}).has_value(), "fake engine must start");
  (void)transport.read_line(std::chrono::seconds(2));
  const pid_t pid = transport.pid_for_tests();
  expect(transport.terminate(std::chrono::milliseconds(200)), "terminate must succeed even when SIGTERM is ignored");
  std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
  std::string content((std::istreambuf_iterator<char>(stat)), {});
  expect(content.empty() || content.find(") Z ") == std::string::npos, "no zombie may remain after terminate");
}
```
In the fake: `--ignore-term` → `signal(SIGTERM, SIG_IGN)` before the loop. Add `[[nodiscard]] int pid_for_tests() const noexcept { return process_id_; }` (POSIX only) to `EngineStdioProcess` under a `// test seam` comment. Note the `weird` string contains `\n`; `EngineCommand` must still accept it in argv (only NUL is forbidden) — if `validate()` rejects newlines, change the test string to drop `\n` and note it.

- [ ] **Step 2: Run to verify they fail** — `FDS 0,1,2,<n>,<m>` (leaked), argv passes already (keep it as the regression anchor), reap test shows `Z`.

- [ ] **Step 3: Implement (both `engine_stdio_process.cpp` and `engine_process.cpp` POSIX paths)**

Before `fork()`:
```cpp
  std::vector<std::string> argument_storage;
  argument_storage.reserve(command.arguments.size() + 2);
  argument_storage.push_back(command.executable.string());
  argument_storage.push_back("--serve");  // (engine_process.cpp: no --serve)
  argument_storage.insert(argument_storage.end(), command.arguments.begin(), command.arguments.end());
  std::vector<char*> argv; for (auto& a : argument_storage) argv.push_back(a.data()); argv.push_back(nullptr);
  std::vector<std::string> environment_storage;
  for (const auto& [k, v] : command.environment) environment_storage.push_back(k + "=" + v);
  std::vector<char*> envp; for (auto& e : environment_storage) envp.push_back(e.data()); envp.push_back(nullptr);
  const std::string executable_path = command.executable.string();
```
Pipes: `pipe2(fd, O_CLOEXEC)` for all three; the child's ends are `dup2`'d onto 0/1/2 (dup2 clears CLOEXEC on the target), so nothing else survives `execve`. Child body (async-signal-safe only):
```cpp
  if (child == 0) {
    if (setpgid(0, 0) != 0) { const int e = errno; (void)!write(errors[1], &e, sizeof(e)); _exit(127); }
    const int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd < 0 || dup2(input[0], 0) < 0 || dup2(output[1], 1) < 0 || dup2(null_fd, 2) < 0) { const int e = errno; (void)!write(errors[1], &e, sizeof(e)); _exit(127); }
#if defined(__linux__) && defined(SYS_close_range)
    (void)syscall(SYS_close_range, 3U, ~0U, 0);
#else
    for (int fd = 3; fd < 1024; ++fd) if (fd != errors[1]) close(fd);
#endif
    execve(executable_path.c_str(), argv.data(), envp.data());
    const int e = errno; (void)!write(errors[1], &e, sizeof(e)); _exit(127);
  }
```
Keep `errors[1]` alive for the error report: with `close_range` it would be closed, so write the errno through a pipe that is `dup2`'d to a fixed fd ≥ 3 first (e.g. `dup2(errors[1], 3)` then `close_range(4, ~0U)`), or use `close_range(3, errors[1]-1)` + `close_range(errors[1]+1, ~0U)`. Include `<sys/syscall.h>`.
`terminate`: after `kill(-pgid, SIGKILL)`, loop `waitpid(process_id_, &status, WNOHANG)` for up to 500 ms (10 ms steps); on reap set `process_id_ = -1`; return true only when reaped.

- [ ] **Step 4: Run to verify they pass** — 18/18.

- [ ] **Step 5: Commit**

```bash
git add secure_frontend
git commit -m "process launch: argv/env built before fork, O_CLOEXEC pipes, fd sweep, reap after SIGKILL (P1-P3)

Regression tests: child_inherits_only_stdio_descriptors, child_receives_exact_argv_and_env,
terminate_reaps_killed_child. The fake engine gained DUMP for SP2's PROC-* tests.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 11: Windows launch hygiene — suspended start into the Job, bounded read_line, write deadline (P4, P6, P7)

**Files:**
- Modify: `secure_frontend/src/engine_stdio_process.cpp` (Windows `start`, `read_line`, `write_line`)
- Modify: `secure_frontend/src/engine_process.cpp` (Windows `start`)
- Modify: `secure_frontend/tests/engine_stdio_process_test.cpp` (portable `current_executable()`, Windows fake)
- Modify: `secure_frontend/CMakeLists.txt` (un-gate `engine_stdio_process_tests` from `if(UNIX)`; keep the Linux-only cases behind `#if !defined(_WIN32)`)
- Test: cross build; runtime in Task 13's MSYS2 job

**Interfaces:**
- Consumes: `current_executable()` helper pattern from `engine_process_test.cpp:28-46`.

- [ ] **Step 1: Make the test portable and add the shared oversize case**

Copy `current_executable()` into `engine_stdio_process_test.cpp`; replace `test_round_trip("/proc/self/exe")` with `test_round_trip(current_executable())`. Wrap the three Task 10 cases and `terminate_reaps_killed_child` in `#if !defined(_WIN32)`. Add a case that runs everywhere:
```cpp
void read_line_rejects_oversize_on_all_platforms(const std::filesystem::path& self) {
  secure_frontend::EngineStdioProcess transport;
  expect(!transport.start({.executable = self, .arguments = {"--fake-engine", "--spew"}, .environment = {}}).has_value(), "fake engine must start");
  // --spew: after READY the fake writes 9000 'x' bytes with no newline.
  (void)transport.read_line(std::chrono::seconds(2));  // READY
  const auto result = transport.read_line(std::chrono::seconds(2));
  expect(result.kind == secure_frontend::EngineReadKind::failure, "an oversized engine line must be a failure on every platform");
  (void)transport.terminate(std::chrono::milliseconds(250));
}
```
Fake: `--spew` → after READY, `std::cout << std::string(9000, 'x') << std::flush;` then block on `std::cin`.
In CMake move `secure_frontend_engine_stdio_process_tests` out of `if(UNIX)`.

- [ ] **Step 2: Verify on Linux and cross-compile**

Run: `cmake --build --preset linux-gcc && ctest --preset linux-gcc -R stdio --output-on-failure` → passes (POSIX already bounds lines).
Run: `cmake --build --preset windows-llvm-mingw-cross 2>&1 | tail -3` → compiles.

- [ ] **Step 3: Implement the Windows fixes**

`start` (both files): add `CREATE_SUSPENDED` to the flags; after `AssignProcessToJobObject(job, info.hProcess)` succeeds call `ResumeThread(info.hThread)` then `CloseHandle(info.hThread)`; on assignment failure `TerminateProcess` the still-suspended child.
`read_line` Windows branch, iterative and bounded:
```cpp
  if (!stdout_read_) return {EngineReadKind::closed, {}, {}};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<char, 1024> bytes{};
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(static_cast<HANDLE>(stdout_read_), nullptr, 0, nullptr, &available, nullptr)) return {EngineReadKind::closed, {}, {}};
    if (available != 0) {
      DWORD read = 0;
      if (!ReadFile(static_cast<HANDLE>(stdout_read_), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read == 0) return {EngineReadKind::closed, {}, {}};
      pending_stdout_.append(bytes.data(), read);
      if (pending_stdout_.size() > kMaxReadLineBytes && pending_stdout_.find('\n') == std::string::npos) return {EngineReadKind::failure, {}, "engine emitted an oversized line"};
      const auto newline = pending_stdout_.find('\n');
      if (newline != std::string::npos) { /* same extraction as the fast path at the top of read_line */ }
      continue;
    }
    if (std::chrono::steady_clock::now() >= deadline) return {EngineReadKind::timeout, {}, {}};
    WaitForSingleObject(static_cast<HANDLE>(process_handle_), 1);  // returns at once if the child died; else a 1 ms pause
  }
```
Factor the newline extraction into a private `std::optional<EngineReadResult> take_pending_line()` used by both the fast path and both platform loops.
`write_line` Windows: open the stdin pipe with `FILE_FLAG_OVERLAPPED` (use `CreateNamedPipeW` + `CreateFileW` for the stdin pipe instead of `CreatePipe`) and do `WriteFile` + `GetOverlappedResultEx(..., 1000, FALSE)`; on timeout `CancelIoEx` and fail with "engine stdin is not writable". If the overlapped rewrite is judged too large for this task, document the bounded-write contract instead: the child's stdin pipe buffer is 64 KiB minimum (`CreateNamedPipeW` `nOutBufferSize`), set it to `kMaxWriteLineBytes + 4096` so a single GEN line never blocks, and state that in a comment plus `docs/security/engine-protocol-limits.md`.

- [ ] **Step 4: Verify** — `cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -1` → 18/18; `cmake --build --preset windows-llvm-mingw-cross` → compiles with `-Werror`; `wine out/build/windows-llvm-mingw-cross/secure_frontend_engine_stdio_process_tests.exe; echo $?` → record the result (wine may not implement Job Objects; a wine failure here is not a plan failure, the MSYS2 job is).

- [ ] **Step 5: Commit**

```bash
git add secure_frontend
git commit -m "windows launch: suspended start into the Job, bounded iterative read_line, bounded stdin writes (P4, P6, P7)

engine_stdio_process_tests is now portable (current_executable()) and runs on
Windows CI; read_line_rejects_oversize_on_all_platforms is the shared case.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 12: Model-id → `--native` linkage and the engine-flag deny-list (M1, M2)

**Files:**
- Create: `secure_frontend/include/secure_frontend/engine_arguments.hpp`
- Create: `secure_frontend/src/engine_arguments.cpp`
- Create: `secure_frontend/tests/engine_arguments_test.cpp`
- Modify: `secure_frontend/CMakeLists.txt` (new library + test; link into `strata-secure-server`)
- Modify: `secure_frontend/src/secure_server_main.cpp` (use it; `--engine-arg` no longer required to be non-empty)

**Interfaces:**
- Produces:
```cpp
namespace secure_frontend {
enum class EngineArgumentsErrorCode { owned_flag, unsupported_mode, invalid_value, model_not_found };
struct EngineArgumentsError final { EngineArgumentsErrorCode code; std::string message; };
using EngineArgumentsResult = std::variant<std::vector<std::string>, EngineArgumentsError>;

// Builds the engine argv tail (everything after `--serve`): the user's
// pass-through arguments, validated, followed by `--native <models_root/relative_artifact_path>`
// for the selected manifest. Rejects any user argument that names a flag the
// frontend owns (--native, --pack, --serve) or a mode whose protocol the
// frontend does not speak (--batch, --slots, --vision), in both `--flag value`
// and `--flag=value` spellings. Values containing NUL are rejected.
[[nodiscard]] EngineArgumentsResult build_engine_arguments(const ModelManifest& selected,
                                                           const std::filesystem::path& models_root,
                                                           const std::vector<std::string>& user_arguments);
}
```

- [ ] **Step 1: Write the failing tests** (`tests/engine_arguments_test.cpp`)

```cpp
#include "secure_frontend/engine_arguments.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace {
int failures = 0;
void expect(bool c, const std::string& m) { if (!c) { ++failures; std::cerr << "FAILED: " << m << '\n'; } }

secure_frontend::ModelManifest manifest() {
  secure_frontend::ModelManifest m; m.id = "qwen-local"; m.relative_artifact_path = "qwen/model-00001-of-00004.gguf"; return m;
}

void engine_receives_model_path_from_inventory() {
  const auto result = secure_frontend::build_engine_arguments(manifest(), "/models", {"--max-context", "8192"});
  const auto* args = std::get_if<std::vector<std::string>>(&result);
  expect(args != nullptr, "valid pass-through must build");
  if (!args) return;
  const std::vector<std::string> expected{"--max-context", "8192", "--native", std::filesystem::path("/models/qwen/model-00001-of-00004.gguf").string()};
  expect(*args == expected, "frontend must append --native from the inventory, last");
}

void engine_args_reject_owned_and_network_flags() {
  for (const std::string bad : {"--native", "--native=/x.gguf", "--pack", "--pack=dir", "--serve", "--batch", "--batch=2", "--slots", "--vision"}) {
    const auto r = secure_frontend::build_engine_arguments(manifest(), "/models", {bad, "value"});
    const auto* e = std::get_if<secure_frontend::EngineArgumentsError>(&r);
    expect(e != nullptr, "must reject " + bad);
    if (e) expect(e->code == secure_frontend::EngineArgumentsErrorCode::owned_flag || e->code == secure_frontend::EngineArgumentsErrorCode::unsupported_mode, "code for " + bad);
  }
  const auto nul = secure_frontend::build_engine_arguments(manifest(), "/models", {std::string("--gpu\0 0", 8)});
  expect(std::holds_alternative<secure_frontend::EngineArgumentsError>(nul), "NUL in an argument must be rejected");
  const auto empty = secure_frontend::build_engine_arguments(manifest(), "/models", {});
  expect(std::holds_alternative<std::vector<std::string>>(empty), "no pass-through arguments is valid: the frontend supplies the model");
}
}  // namespace

int main() {
  engine_receives_model_path_from_inventory();
  engine_args_reject_owned_and_network_flags();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
```
CMake: `add_library(secure_frontend_engine_arguments src/engine_arguments.cpp)` with the usual include dir, `cxx_std_20`, `secure_frontend_set_warnings`, link `PUBLIC secure_frontend_model_inventory`; test target `secure_frontend_engine_arguments_tests` + `add_test`; add the library to `strata-secure-server`'s links and to the warnings `foreach`.

- [ ] **Step 2: Run to verify it fails** — link/compile error (header missing).

- [ ] **Step 3: Implement**

```cpp
#include "secure_frontend/engine_arguments.hpp"

#include <array>
#include <string_view>

namespace secure_frontend {
namespace {
constexpr std::array<std::string_view, 3> kOwnedFlags{"--native", "--pack", "--serve"};
constexpr std::array<std::string_view, 3> kUnsupportedModes{"--batch", "--slots", "--vision"};

std::string_view flag_name(const std::string_view argument) {
  const auto equals = argument.find('=');
  return equals == std::string_view::npos ? argument : argument.substr(0, equals);
}
}  // namespace

EngineArgumentsResult build_engine_arguments(const ModelManifest& selected, const std::filesystem::path& models_root,
                                             const std::vector<std::string>& user_arguments) {
  std::vector<std::string> arguments;
  arguments.reserve(user_arguments.size() + 2);
  for (const auto& argument : user_arguments) {
    if (argument.find('\0') != std::string::npos) return EngineArgumentsError{EngineArgumentsErrorCode::invalid_value, "engine argument contains NUL"};
    const auto name = flag_name(argument);
    for (const auto owned : kOwnedFlags) if (name == owned) return EngineArgumentsError{EngineArgumentsErrorCode::owned_flag, "engine argument is owned by the frontend"};
    for (const auto mode : kUnsupportedModes) if (name == mode) return EngineArgumentsError{EngineArgumentsErrorCode::unsupported_mode, "engine mode is not supported by the frontend"};
    arguments.push_back(argument);
  }
  const auto path = validate_local_artifact_path(selected.relative_artifact_path);
  if (!std::holds_alternative<std::string>(path)) return EngineArgumentsError{EngineArgumentsErrorCode::invalid_value, "selected model has an invalid artifact path"};
  arguments.emplace_back("--native");
  arguments.push_back((models_root / std::get<std::string>(path)).string());
  return arguments;
}
}  // namespace secure_frontend
```
In `secure_server_main.cpp`: find the selected manifest (not just `any_of`), call `build_engine_arguments`, fail startup with `"Engine arguments rejected."` on error, pass the result as `EngineCommand::arguments`; remove the `"at least one --engine-arg is required"` rule and update `usage()` to say `--engine-arg` is optional and that `--native` is supplied by the frontend.

- [ ] **Step 4: Run to verify it passes** — `ctest --preset linux-gcc | tail -1` → 19/19.

- [ ] **Step 5: Commit**

```bash
git add secure_frontend
git commit -m "engine arguments: frontend owns --native from the inventory; deny owned and unsupported flags (M1, M2)

Regression tests: engine_receives_model_path_from_inventory,
engine_args_reject_owned_and_network_flags. --model-id now selects what the engine loads.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 13: CI matrix — llvm-mingw cross, MSYS2 CLANG64 runtime, MSVC informational, top-level integration (T6)

**Files:**
- Modify: `.github/workflows/secure-frontend.yml` (rewrite)
- Modify: `secure_frontend/CMakePresets.json` (add `windows-msys2-clang` preset: Ninja, `CC=clang`, `CXX=clang++`)
- Test: `actionlint` is not installed; validate with `python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/secure-frontend.yml'))"` from the venv, and by the owner pushing.

- [ ] **Step 1: Write the failing check** — `grep -c msys2 .github/workflows/secure-frontend.yml` → `0`.

- [ ] **Step 2: Rewrite the workflow**

```yaml
name: secure-frontend

on:
  push:
    paths: ["secure_frontend/**", "CMakeLists.txt", "scripts/dev/**", ".github/workflows/secure-frontend.yml"]
  pull_request:
    paths: ["secure_frontend/**", "CMakeLists.txt", "scripts/dev/**", ".github/workflows/secure-frontend.yml"]
  workflow_dispatch:

permissions:
  contents: read

jobs:
  linux:
    name: ${{ matrix.preset }}
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false
      matrix:
        preset: [linux-gcc, linux-clang]
    steps:
      - uses: actions/checkout@v4
      - run: scripts/dev/bootstrap-linux.sh
      - run: echo "$GITHUB_WORKSPACE/.venv-dev/bin" >> "$GITHUB_PATH"
      - run: cmake --preset ${{ matrix.preset }}
        working-directory: secure_frontend
      - run: cmake --build --preset ${{ matrix.preset }} --parallel
        working-directory: secure_frontend
      - run: ctest --preset ${{ matrix.preset }}
        working-directory: secure_frontend

  windows-cross:
    name: windows-llvm-mingw-cross (compile+link on Linux)
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: scripts/dev/bootstrap-linux.sh && scripts/dev/bootstrap-llvm-mingw.sh
      - run: echo "$GITHUB_WORKSPACE/.venv-dev/bin" >> "$GITHUB_PATH"
      - run: cmake --preset windows-llvm-mingw-cross
        working-directory: secure_frontend
      - run: cmake --build --preset windows-llvm-mingw-cross --parallel
        working-directory: secure_frontend
      - uses: actions/upload-artifact@v4
        with:
          name: windows-x86_64-llvm-mingw
          path: secure_frontend/out/build/windows-llvm-mingw-cross/*.exe

  windows-msys2:
    name: windows-msys2-clang (runtime tests on Windows)
    runs-on: windows-latest
    defaults:
      run:
        shell: msys2 {0}
    steps:
      - uses: actions/checkout@v4
      - uses: msys2/setup-msys2@v2
        with:
          msystem: CLANG64
          update: false
          install: mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja
      - run: cmake --preset windows-msys2-clang
        working-directory: secure_frontend
      - run: cmake --build --preset windows-msys2-clang --parallel
        working-directory: secure_frontend
      - run: ctest --preset windows-msys2-clang
        working-directory: secure_frontend

  windows-msvc:
    name: windows-msvc (informational, non-blocking)
    runs-on: windows-latest
    continue-on-error: true
    steps:
      - uses: actions/checkout@v4
      - run: cmake --preset windows-msvc
        working-directory: secure_frontend
      - run: cmake --build --preset windows-msvc --parallel
        working-directory: secure_frontend
      - run: ctest --preset windows-msvc
        working-directory: secure_frontend

  top-level-integration:
    name: root CMake with STRATA_BUILD_SECURE_FRONTEND=ON
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: scripts/dev/bootstrap-linux.sh
      - run: echo "$GITHUB_WORKSPACE/.venv-dev/bin" >> "$GITHUB_PATH"
      - run: cmake -S . -B out/root -G Ninja -DSTRATA_ENABLE_CUDA=OFF -DSTRATA_ENABLE_HIP=OFF -DSTRATA_BUILD_SECURE_FRONTEND=ON -DSTRATA_BUILD_SECURE_FRONTEND_TESTS=ON
      - run: cmake --build out/root --target strata-secure-server secure_frontend_config_tests --parallel
      - run: ctest --test-dir out/root -R secure_frontend_config_tests
```
Add the `windows-msys2-clang` preset (inherits base, Ninja, `CC=clang`/`CXX=clang++`) with build/test presets. If the root configure fails without CUDA/HIP for engine reasons, restrict the integration job to `cmake -S . -B out/root ... 2>&1 | tee` and `grep -q "secure_frontend" out/root/build.ninja` and say so in a comment.

- [ ] **Step 3: Validate YAML** — `.venv-dev/bin/python -c "import yaml; yaml.safe_load(open('.github/workflows/secure-frontend.yml')); print('ok')"` → `ok`. Local presets: `cmake --list-presets` in `secure_frontend` lists all five.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/secure-frontend.yml secure_frontend/CMakePresets.json
git commit -m "ci: llvm-mingw cross, MSYS2 clang runtime on Windows, MSVC informational, root integration (T6)

Blocking: linux-gcc, linux-clang, windows-llvm-mingw-cross, windows-msys2-clang,
top-level-integration. Non-blocking: windows-msvc. Path filters include the
root CMakeLists.txt.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 14: Documentation truth pass and handoff update (D1–D4)

**Files:**
- Modify: `secure_frontend/README.md`
- Modify: `secure_frontend/include/secure_frontend/token_reader.hpp:42-48`
- Modify: `docs/AGENT_HANDOFF.md`
- Modify: `docs/secure-frontend-ci.md`
- Verify: `docs/security/engine-protocol-limits.md` exists (Task 9)

- [ ] **Step 1: Failing check** — `grep -n "placeholder until real routes\|supplied by the parent project" secure_frontend/README.md` → matches exist. `grep -c "GLM tokenizer tooling" docs/AGENT_HANDOFF.md` → `2`.

- [ ] **Step 2: README** — delete the two stale sentences; add a "Build" section pointing to `scripts/dev/bootstrap-linux.sh`, the five presets, and this caveat verbatim:

```markdown
Windows binaries are cross-compiled with llvm-mingw and smoke-tested under
wine on Linux. wine does not implement Job Objects or protected-file DACLs
faithfully; Windows runtime behavior is validated only by the `windows-msys2-clang`
CI job on a real Windows runner. The `windows-msvc` job is informational.
```

- [ ] **Step 3: token_reader.hpp** — read `token_reader.cpp:169-417`; if `protected_file_unsupported_on_platform` is never returned, delete the enum value and the comment; otherwise rewrite the comment to describe the DACL/final-path checks that actually run.

- [ ] **Step 4: Handoff** — in `docs/AGENT_HANDOFF.md`:
  - Merge the two "GLM tokenizer tooling" rows into one.
  - Replace "What has changed in this working tree" with a table of SP0 deliverables (tooling, CMake hygiene, llvm-mingw, S1–S7, E1–E4, P1–P7, M1–M2, CI, docs), each naming its regression test.
  - Replace "Verification performed so far" with: the exact commands (`scripts/dev/bootstrap-linux.sh`, `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc`), the test count, the cross-build result, which tests pass under wine, and "CI status: awaiting the first push by the owner; Windows runtime unverified until `windows-msys2-clang` is green".
  - In "Ordered implementation backlog" mark the SP0 items done and leave SP1–SP7 untouched.
  - Add the `pytest -q` + pre-push-hook note.

- [ ] **Step 5: Full verification**

```bash
cd /opt/projects/ai && .venv-dev/bin/pytest -q | tail -1
cd secure_frontend && cmake --build --preset linux-gcc && ctest --preset linux-gcc | tail -1
cmake --build --preset windows-llvm-mingw-cross | tail -1 && wine out/build/windows-llvm-mingw-cross/strata-secure-server.exe --help >/dev/null && echo wine-ok
cd .. && git status --short
```
Expected: pytest passes; 19/19; cross build ok; `wine-ok`; `git status --short` empty.

- [ ] **Step 6: Commit**

```bash
git add secure_frontend/README.md secure_frontend/include/secure_frontend/token_reader.hpp docs/AGENT_HANDOFF.md docs/secure-frontend-ci.md
git commit -m "docs: SP0 truth pass; handoff records what is verified and what is not (D1-D4)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-review (done while writing)

- **Spec coverage:** T1→Task 1, T2→Task 3, T3/T4→Task 2, T5→Task 1, T6→Task 13; S1→4, S2/S3→5, S4–S7→6; E1/E2→7, E3→8, E4→9; P1–P3→10, P4/P6/P7→11, P5→3; M1/M2→12; D1–D4→14 (+ Task 9 creates D4). Acceptance §7.1–7.6 are checked in Task 14 Step 5 and by the owner's push.
- **Type consistency:** `HttpSocketServerLimits` (Task 5) is used by Tasks 4 (introduced as a forward note), 5, 6. `EngineHealth` (Task 8) is used in `secure_server_main.cpp` only. `idle()` (Task 7) is used by Tasks 7, 8. `kMaxReadLineBytes`/`kMaxWriteLineBytes` (Task 9) are used by Task 11. `build_engine_arguments` (Task 12) consumes `ModelManifest` and `validate_local_artifact_path` from `model_inventory.hpp`.
- **Review Focus:** all five pinned (Tasks 4/6, 6, 7, 8, 12).
- **Known judgement calls for the implementer:** Task 4 ordering of the `limits` parameter (compile at every boundary); Task 7 `quit()` vs `abandon()` from the generating state; Task 11 overlapped-write vs documented pipe-buffer contract; Task 13 root configure without GPUs.
