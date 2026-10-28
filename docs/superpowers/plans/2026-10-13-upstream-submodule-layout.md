# Upstream as a Submodule — Repository Layout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move upstream Niko1221/Strata `v0.1.43` out of our tree into a pinned, unmodified git submodule at `upstream/`, so the repository root holds only our code and an upstream update becomes "bump the pointer, run the gates".

**Architecture:** A submodule at `upstream/` (gitlink `7695859aa13afb248bdb4d36f9b2e95707db079d`) replaces the in-tree upstream copy. Our own root `CMakeLists.txt` adds `secure_frontend/` first and then `upstream/` as an unmodified subdirectory, with cache defaults that keep GPU SDKs and the network out of the default build. The one upstream Python edit (GLM pre-tokenizer aliases) becomes a wrapper in `tools/secure/` that imports upstream's tokenizer; routine `pytest -q` runs only `tools/secure/`.

**Tech Stack:** git submodules (git 2.34.1 on the dev host), CMake 4.4.3 + Ninja 1.13.2 (from `.venv-dev`), C++20, Python 3 + pytest 9.1.1 + `regex`, GitHub Actions, llvm-mingw `20261006` + wine for the Windows cross build.

**Spec:** [`docs/superpowers/specs/2026-10-13-upstream-submodule-layout-design.md`](../specs/2026-10-13-upstream-submodule-layout-design.md) (approved; scope items U1–U14). Owner decisions on its open questions: one root `LICENSE` (the AGPL-3.0) and `NOTICE.md` (Strata's notice with the Niko1221 copyright line + a pointer to `upstream/LICENSE`); upstream's own tests are **not** in routine `pytest -q` (run `pytest upstream/tools` / `pytest upstream/serve` explicitly after a bump); no `lighttransport/Strata` submodule yet.

## Global Constraints

- Where it runs: the **main checkout `/opt/projects/ai`**, on branch `layout/upstream-submodule` created from `main` (Task 1). Not a worktree. Precondition: this plan file is already committed on `main` by the controller and `git status --short` is empty. `.worktrees/sp1` and `.worktrees/sp2` are left alone until Task 7 recreates them.
- Every command block starts with `cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"` and then uses `git -C "$R"` and `"$R/…"` paths (agent shells reset their working directory between calls).
- Tool PATH in every block that runs cmake/ctest/ninja/pytest: `export PATH="$R/.venv-dev/bin:$PATH"`.
- Submodule: path `upstream`, URL `https://github.com/Niko1221/Strata.git`, pinned at `7695859aa13afb248bdb4d36f9b2e95707db079d` (tag `v0.1.43`), no branch tracking.
- Upstream files are never edited, inside or outside the submodule. After every gate, `git -C "$R/upstream" status --porcelain` prints nothing and `git -C "$R" submodule status` has no `+`, `-` or `U` prefix.
- `secure_frontend/` code and `secure_frontend/CMakePresets.json` stay unchanged (the only `secure_frontend/` edit in this plan is three sentences of `secure_frontend/README.md` in Task 6).
- No GPU SDK and no network fetch in the default root build: our root seeds `STRATA_ENABLE_CUDA=OFF`, `STRATA_ENABLE_HIP=OFF`, `STRATA_NATIVE_EXPERTS=OFF` as cache defaults that `-D` overrides; `out/root/_deps` never exists.
- Network use is limited to: cloning the submodule from GitHub (Task 1, Task 7 scratch clone and worktree init), and PyPI for the fresh-venv check in Task 3. No `acid`, no `az`, nothing outside this repository.
- Baseline gate numbers (from `docs/AGENT_HANDOFF.md`): `ctest --preset linux-gcc` 20/20; `windows-llvm-mingw-cross` builds 20 `.exe` with `-Werror`; wine `ctest --preset windows-llvm-mingw-cross` 19/19; full root build from clean 94 Ninja steps, 0 warnings, root `ctest` 20/20.
- Routine `pytest -q` after Task 3: `testpaths = tools/secure`, **12 passed** (4 GLM + 2 extract + 6 distribution-check); **13** after the final fix wave (+1 `extract()` missing-file guard test). Later "12 passed" expectations in this plan are the per-task values before that wave.
- Deletions: only the paths this plan names (build/probe directories under `out/`, the `git rm` list in Task 4, the two worktree symlinks in Task 7). Never `rm -rf` the repository, `upstream/`, `.venv-dev/`, `.toolchains/` or `.worktrees/`.
- Commits: local only, one per task, with
  `git -C "$R" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "<subject>" -m "<body>" -m "$TRAILER"` where
  `TRAILER=$'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\nClaude-Session: <session link>'`
  (use the attribution lines your own harness gives if they differ). **Never push**; the controller/the owner pushes.
- `docs/AGENT_HANDOFF.md` and `docs/PROVENANCE.md` change in this same commit series (Task 6, verification record in Task 7), never after.

## Review Focus

1. **Plain clone or deinitialized submodule (empty `upstream/`).** A person who clones without `--recurse-submodules` must get the instruction `git submodule update --init` from both the root CMake configure and `pytest -q` (not an ImportError traceback), while `-DSECURE_STRATA_BUILD_ENGINE=OFF` and the `secure_frontend/` presets still work. Pinned in Task 2 Step 7 (CMake), Task 3 Step 11 (pytest), Task 7 Step 4 (fresh scratch clone).
2. **Quarantined content below the staging root.** A staging directory holding `upstream/requirements.txt`, `upstream/serve/index.html`, `vendor/strata/requirements.txt` or `pkg/serve/…` must fail `scripts/secure_distribution_check.py`; look-alike names (`upstream-notes.txt`, `requirements.txt.sig`, `observe/`) must still pass. Pinned in Task 3 Steps 1–4.
3. **Gates leaving the submodule dirty or moved** (build output, `__pycache__`, a stray checkout). After build, ctest and pytest, `git -C upstream status --porcelain` is empty and `git submodule status` has no `+`/`-`/`U`. Pinned in Task 3 Step 12, Task 5 (CI step), Task 7 Step 8.
4. **Operator `-D` overrides through our root.** `-DCMAKE_BUILD_TYPE=Debug` stays `Debug` (upstream forces `Release` only when empty), `-DSTRATA_WERROR=OFF` does not drop the frontend's `-Werror`, and `-DSTRATA_ENABLE_CUDA=ON` reaches upstream (our seeds are defaults, not `FORCE`). Pinned in Task 2 Step 6.
5. **A GLM pack extracted through the wrapper.** Upstream's pristine `extract()` writes the Qwen pattern into `tokenizer.json` for every pack; through `tools/secure/secure_tokenizer.py` a `glm4`/`chatglm-bpe` pack must ship `GLM4_PATTERN` and a `qwen35` pack must keep `QWEN35_PATTERN`. Pinned in Task 3 Steps 5–8 (`tools/secure/test_secure_tokenizer.py`).

---

## File map

| Path | After this plan | Task |
| --- | --- | --- |
| `.gitmodules`, `upstream` (gitlink) | new: the submodule | 1 |
| `CMakeLists.txt` | ours (spec §4.3), replaces upstream's modified root file | 2 |
| `tools/secure/secure_tokenizer.py` | new: GLM wrapper over `upstream/tools/strata_tokenizer.py` | 3 |
| `tools/secure/test_glm_tokenizer.py` | moved from `tools/test_glm_tokenizer.py`; import changed | 3 |
| `tools/secure/test_secure_tokenizer.py` | new: `extract()` through the wrapper | 3 |
| `tools/secure/test_secure_distribution_check.py` | new: U9 tests | 3 |
| `scripts/secure_distribution_check.py` | U9: any-depth matching, `upstream` component rejected | 3 |
| `pytest.ini`, `conftest.py` | ours-only collection; missing-submodule message | 3 |
| `scripts/dev/requirements-dev.txt`, `scripts/dev/bootstrap-linux.sh` | cmake/ninja/regex pins; bootstrap reads only our file | 3 |
| every upstream file at the root (192 pathspecs, 2 971 files) | removed (`git rm`) | 4 |
| `.gitignore`, `.gitattributes`, `README.md`, `LICENSE` | rewritten as ours | 4 |
| `.github/workflows/secure-frontend.yml` | submodule checkout, our root in `top-level-integration`, status checks | 5 |
| `docs/PROVENANCE.md`, `docs/secure-frontend-ci.md` | rewritten | 6 |
| `docs/AGENT_HANDOFF.md`, `docs/security/{privacy-migration,engine-protocol-limits,deployment}.md`, `secure_frontend/README.md`, product spec §9/§11, SP1/SP2 plan+spec lines | listed edits | 6 |
| `docs/AGENT_HANDOFF.md` "Verification performed" | restructure verification rows | 7 |

---

### Task 1: Add the `upstream/` submodule at the pinned commit (U2)

**Files:**
- Create: `.gitmodules`
- Create: `upstream` (gitlink → `7695859aa13afb248bdb4d36f9b2e95707db079d`)

**Interfaces:**
- Consumes: nothing.
- Produces: a checked-out `upstream/` holding pristine upstream `v0.1.43` (its `CMakeLists.txt`, `tools/strata_tokenizer.py`, `tools/gguf_reader.py`, `LICENSE`, `README.md`, `third_party/ggml/LICENSE`, `requirements.txt`). Every later task reads these paths. Branch `layout/upstream-submodule`.

- [ ] **Step 1: Create the branch from `main` in the main checkout**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
git -C "$R" status --short          # must print nothing
git -C "$R" switch -c layout/upstream-submodule main
git -C "$R" branch --show-current
```

Expected: `git status --short` prints nothing; then `Switched to a new branch 'layout/upstream-submodule'`; last line `layout/upstream-submodule`.

- [ ] **Step 2: Confirm the failing precondition (no submodule yet)**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
git -C "$R" submodule status; echo "exit $?"
test -e "$R/upstream" && echo "upstream exists" || echo "no upstream/"
```

Expected: no submodule lines, `exit 0`, `no upstream/`.

- [ ] **Step 3: Add the submodule and pin it**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
git -C "$R" submodule add https://github.com/Niko1221/Strata.git upstream
git -C "$R/upstream" checkout --detach 7695859aa13afb248bdb4d36f9b2e95707db079d
git -C "$R" add upstream .gitmodules
```

Expected: `Cloning into '/opt/projects/ai/upstream'...`, then `HEAD is now at 7695859a …`.

- [ ] **Step 4: Verify the pin, the URL and that the tree is pristine**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
git -C "$R/upstream" rev-parse HEAD
git -C "$R" submodule status
git -C "$R" config -f "$R/.gitmodules" --list
git -C "$R/upstream" status --short
test "$(git -C "$R/upstream" rev-parse 'HEAD^{tree}')" = "$(git -C "$R" rev-parse '7695859aa13afb248bdb4d36f9b2e95707db079d^{tree}')" && echo "tree identical to v0.1.43"
git -C "$R" diff --cached --name-status
```

Expected, in order:
```
7695859aa13afb248bdb4d36f9b2e95707db079d
 7695859aa13afb248bdb4d36f9b2e95707db079d upstream (v0.1.43)
submodule.upstream.path=upstream
submodule.upstream.url=https://github.com/Niko1221/Strata.git
tree identical to v0.1.43
A	.gitmodules
A	upstream
```
(`git -C upstream status --short` prints nothing; there is no `branch =` line in `.gitmodules`.) If `git submodule status` shows a leading `+`, Step 3's `git add upstream` was skipped: run it again.

- [ ] **Step 5: Existing gates are untouched**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
cd "$R" && pytest -q 2>&1 | tail -1
git -C "$R/upstream" status --porcelain
```

Expected: `620 passed, 2 skipped, 3 deselected, 214 subtests passed in …s` (pytest still collects the old in-tree `tools/` only; `upstream/` is not in `testpaths`), and no submodule output.

- [ ] **Step 6: Commit**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
TRAILER=$'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\nClaude-Session: <session link>'
git -C "$R" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "layout: add upstream Strata as the submodule upstream/ at v0.1.43 (U2)" \
  -m "git submodule add https://github.com/Niko1221/Strata.git upstream, pinned at 7695859aa13afb248bdb4d36f9b2e95707db079d (tag v0.1.43); its tree is byte-identical to v0.1.43. The in-tree upstream copy is removed in a later commit of this series." \
  -m "$TRAILER"
git -C "$R" log -1 --format='%h %s'
```

Expected: one commit, subject as above.

---

### Task 2: Our root `CMakeLists.txt` builds `secure_frontend/` and `upstream/` side by side (U3)

**Files:**
- Modify (full rewrite): `CMakeLists.txt`
- Modify: `.gitignore` (append `/out/`; the whole file is replaced in Task 4)

**Interfaces:**
- Consumes: `upstream/CMakeLists.txt` (Task 1).
- Produces: CMake options `SECURE_STRATA_BUILD_ENGINE` (BOOL, default `ON`), `STRATA_BUILD_SECURE_FRONTEND_TESTS` (default `${BUILD_TESTING}`), `SECURE_FRONTEND_WERROR` (default `ON`); cache seeds `STRATA_ENABLE_CUDA`, `STRATA_ENABLE_HIP`, `STRATA_NATIVE_EXPERTS` = `OFF`. The FATAL_ERROR text `upstream/ is empty: run \`git submodule update --init\`` (Task 7 and the docs cite it). Root build directory convention `out/root` (Tasks 4, 5, 7). The option `STRATA_BUILD_SECURE_FRONTEND` no longer exists.

- [ ] **Step 1: Confirm the failing precondition**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
head -12 "$R/CMakeLists.txt" | grep -n 'project('
grep -c 'add_subdirectory(upstream)' "$R/CMakeLists.txt"
```

Expected: `project(strata VERSION 0.1.43 LANGUAGES CXX)` (the root file is still upstream's) and `0`.

- [ ] **Step 2: Write our root `CMakeLists.txt` (whole file)**

```cmake
# Secure Strata root build: our secure_frontend/ plus upstream Strata's engine
# from the pinned, unmodified git submodule upstream/ (docs/AGENT_HANDOFF.md,
# "Repository layout"). Upstream's own CMakeLists.txt is used as-is.
cmake_minimum_required(VERSION 3.24)
project(secure_strata LANGUAGES CXX)

# Upstream forces CMAKE_BUILD_TYPE=Release into the cache when it is empty,
# which would leak to the whole build; choose the default here first.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  # FORCE: project() already made an empty cache entry (Task 2 review fix)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Upstream calls enable_testing() only in its own directories; the root ctest
# sees any test only when the root enables testing.
include(CTest)

option(SECURE_STRATA_BUILD_ENGINE "Configure upstream's engine from upstream/" ON)
option(STRATA_BUILD_SECURE_FRONTEND_TESTS "Build the portable frontend tests" ${BUILD_TESTING})
# Declared here, before upstream's option(STRATA_WERROR OFF) exists, so the
# frontend keeps -Werror whatever STRATA_WERROR is.
option(SECURE_FRONTEND_WERROR "Treat warnings as errors in the secure frontend" ON)

# Upstream's defaults that would need a GPU SDK or the network; -D overrides.
set(STRATA_ENABLE_CUDA OFF CACHE BOOL "Build the CUDA targets")
set(STRATA_ENABLE_HIP OFF CACHE BOOL "Build the HIP targets")
set(STRATA_NATIVE_EXPERTS OFF CACHE BOOL "Native (IQ) experts through ggml-cpu (fetches llama.cpp)")

# The frontend first: see SECURE_FRONTEND_WERROR above.
add_subdirectory(secure_frontend)

if(SECURE_STRATA_BUILD_ENGINE)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/upstream/CMakeLists.txt")
    message(FATAL_ERROR "upstream/ is empty: run `git submodule update --init`")
  endif()
  add_subdirectory(upstream)
endif()
```

- [ ] **Step 3: Ignore root build output**

Append to `.gitignore` (the file is replaced wholesale in Task 4):

```
# root build directories (our root CMakeLists.txt; see docs/secure-frontend-ci.md)
/out/
```

- [ ] **Step 4: Configure and build the whole root from clean**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
test ! -e "$R/out/root" && echo "clean start"
cmake -S "$R" -B "$R/out/root" -G Ninja 2>&1 | tail -1
grep -E '^(CMAKE_BUILD_TYPE|SECURE_FRONTEND_WERROR|STRATA_NATIVE_EXPERTS|STRATA_ENABLE_CUDA|STRATA_ENABLE_HIP|STRATA_BUILD_TESTS|SECURE_STRATA_BUILD_ENGINE):' "$R/out/root/CMakeCache.txt" | sort
cmake --build "$R/out/root" > "$R/out/root-build.log" 2>&1; echo "build exit $?"
grep -m1 -o '^\[1/[0-9]*\]' "$R/out/root-build.log"
grep -c 'warning:' "$R/out/root-build.log"
```

Expected:
```
clean start
-- Build files have been written to: /opt/projects/ai/out/root
CMAKE_BUILD_TYPE:STRING=Release
SECURE_FRONTEND_WERROR:BOOL=ON
SECURE_STRATA_BUILD_ENGINE:BOOL=ON
STRATA_BUILD_TESTS:BOOL=OFF
STRATA_ENABLE_CUDA:BOOL=OFF
STRATA_ENABLE_HIP:BOOL=OFF
STRATA_NATIVE_EXPERTS:BOOL=OFF
build exit 0
[1/94]
0
```
If `out/root` already exists, remove only it (`cmake -E rm -rf "$R/out/root"`) and rerun; the 94-step count holds only from clean.

- [ ] **Step 5: Engine and frontend targets exist, no FetchContent, root ctest green**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
find "$R/out/root" -type f \( -name strata-plan -o -name strata-gguf -o -name strata-dequant -o -name strata-secure-server \) -printf '%f\n' | sort
test ! -e "$R/out/root/_deps" && echo "no _deps"
ctest --test-dir "$R/out/root" 2>&1 | grep 'tests passed'
git -C "$R/upstream" status --porcelain
```

Expected:
```
strata-dequant
strata-gguf
strata-plan
strata-secure-server
no _deps
100% tests passed, 0 tests failed out of 20
```
and nothing from the submodule status.

- [ ] **Step 6: Review Focus 4 — `-D` overrides reach the right place**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
cmake -S "$R" -B "$R/out/probe-debug" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSTRATA_WERROR=OFF > "$R/out/probe-debug.log" 2>&1; echo "debug exit $?"
grep -E '^(CMAKE_BUILD_TYPE|SECURE_FRONTEND_WERROR|STRATA_WERROR):' "$R/out/probe-debug/CMakeCache.txt" | sort
cmake -S "$R" -B "$R/out/probe-cuda" -G Ninja -DSTRATA_ENABLE_CUDA=ON > "$R/out/probe-cuda.log" 2>&1; echo "cuda exit $?"
grep -m1 -o 'CMAKE_CUDA_COMPILER' "$R/out/probe-cuda.log"
```

Expected:
```
debug exit 0
CMAKE_BUILD_TYPE:STRING=Debug
SECURE_FRONTEND_WERROR:BOOL=ON
STRATA_WERROR:BOOL=OFF
cuda exit 1
CMAKE_CUDA_COMPILER
```
(The dev host has no CUDA toolkit, so upstream's `enable_language(CUDA)` fails: proof that the `-D` reached upstream. On a host with a toolkit the configure succeeds instead; then `grep '^STRATA_ENABLE_CUDA:BOOL=ON' out/probe-cuda/CMakeCache.txt` must match.)

- [ ] **Step 7: Review Focus 1 — empty `upstream/` gives the instruction; frontend-only still configures**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
git -C "$R" submodule deinit -f upstream
ls -A "$R/upstream" | wc -l
cmake -S "$R" -B "$R/out/probe-empty" -G Ninja > "$R/out/probe-empty.log" 2>&1; echo "empty exit $?"
grep -o 'upstream/ is empty: run `git submodule update --init`' "$R/out/probe-empty.log"
cmake -S "$R" -B "$R/out/probe-frontend" -G Ninja -DSECURE_STRATA_BUILD_ENGINE=OFF > "$R/out/probe-frontend.log" 2>&1; echo "frontend-only exit $?"
cmake --build "$R/out/probe-frontend" --target secure_frontend_config_tests > /dev/null 2>&1; echo "frontend-only build exit $?"
git -C "$R" submodule update --init upstream
git -C "$R" submodule status
```

Expected:
```
Cleared directory 'upstream'
Submodule 'upstream' (https://github.com/Niko1221/Strata.git) unregistered for path 'upstream'
0
empty exit 1
upstream/ is empty: run `git submodule update --init`
frontend-only exit 0
frontend-only build exit 0
Submodule 'upstream' (https://github.com/Niko1221/Strata.git) registered for path 'upstream'
Submodule path 'upstream': checked out '7695859aa13afb248bdb4d36f9b2e95707db079d'
 7695859aa13afb248bdb4d36f9b2e95707db079d upstream (v0.1.43)
```
(The re-init uses the clone kept in `.git/modules/upstream`; no network.)

- [ ] **Step 8: Remove the probe directories and check the tree**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
cmake -E rm -rf "$R/out/probe-debug" "$R/out/probe-cuda" "$R/out/probe-empty" "$R/out/probe-frontend"
cmake -E rm -f "$R/out/probe-debug.log" "$R/out/probe-cuda.log" "$R/out/probe-empty.log" "$R/out/probe-frontend.log"
git -C "$R" status --short
git -C "$R/upstream" status --porcelain
```

Expected: `git status --short` shows exactly ` M .gitignore` and ` M CMakeLists.txt` (`out/` is ignored); nothing from the submodule.

- [ ] **Step 9: Commit**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
TRAILER=$'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\nClaude-Session: <session link>'
git -C "$R" add CMakeLists.txt .gitignore
git -C "$R" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "build: our root CMakeLists.txt adds secure_frontend/ then upstream/ (U3)" \
  -m "Replaces upstream's root file and its STRATA_BUILD_SECURE_FRONTEND hunk. Default build type Release before upstream sees it; include(CTest); SECURE_FRONTEND_WERROR ON declared before upstream's STRATA_WERROR; CUDA/HIP/native experts seeded OFF as overridable cache defaults; SECURE_STRATA_BUILD_ENGINE=OFF builds the frontend alone. Full build from clean: 94 steps, 0 warnings, ctest 20/20, no _deps. Empty upstream/ stops with the git submodule update --init message." \
  -m "$TRAILER"
```

---

### Task 3: `tools/secure/` wrapper, ours-only pytest, dev requirements, distribution check at any depth (U4, U5, U8, U9)

**Files:**
- Create: `tools/secure/secure_tokenizer.py`
- Move+Modify: `tools/test_glm_tokenizer.py` → `tools/secure/test_glm_tokenizer.py` (import block only)
- Create: `tools/secure/test_secure_tokenizer.py`
- Create: `tools/secure/test_secure_distribution_check.py`
- Modify: `scripts/secure_distribution_check.py:1-7` (docstring), `:19-67` (constants comment + `forbidden_reason`)
- Modify (full rewrite): `pytest.ini`, `conftest.py`, `scripts/dev/requirements-dev.txt`
- Modify: `scripts/dev/bootstrap-linux.sh:1-7` (header), `:41` (`pip_args`)

**Interfaces:**
- Consumes: `upstream/tools/strata_tokenizer.py` (pristine: `QWEN35_PATTERN`, `bytes_to_unicode()`, `Tokenizer(tokens, merges, token_types=None, pre="qwen35", special_ids=None)`, `Tokenizer.from_gguf(path)` classmethod, `extract(gguf_path, out_dir) -> dict` writing `<out_dir>/tokenizer/tokenizer.json`), `upstream/tools/gguf_reader.py`.
- Produces: module `tools/secure/secure_tokenizer.py` exporting `GLM4_PATTERN: str`, `QWEN35_PATTERN: str`, `GLM4_PRE_NAMES: tuple[str, ...]`, `bytes_to_unicode() -> dict[int, str]`, `pre_tokenizer_pattern(pre: str) -> tuple[str, str]`, `class Tokenizer(strata_tokenizer.Tokenizer)`, `extract(gguf_path, out_dir) -> dict`, `main(argv: list[str] | None = None) -> int`. `scripts/secure_distribution_check.forbidden_reason(relative: str) -> str | None` (signature unchanged) and `inspect(package_root: Path, require_release_evidence: bool) -> list[str]` (unchanged). Routine `pytest -q` = 12 tests under `tools/secure/`. `scripts/dev/requirements-dev.txt` is the only requirements file `bootstrap-linux.sh` reads.

- [ ] **Step 1: Write the failing distribution-check tests (U9)**

Create `tools/secure/test_secure_distribution_check.py`:

```python
"""scripts/secure_distribution_check.py: quarantined content is rejected at any depth.

Hermetic: every case builds a throwaway staging directory under tmp_path.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_SPEC = importlib.util.spec_from_file_location(
    "secure_distribution_check", ROOT / "scripts" / "secure_distribution_check.py")
check = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(check)


def stage(tmp_path: Path, *files: str) -> Path:
    root = tmp_path / "stage"
    root.mkdir()
    for relative in files:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"x")
    return root


def flagged(errors: list[str], relative: str) -> bool:
    return any(error.startswith(relative.lower() + ":") for error in errors)


def test_nested_upstream_requirements_is_rejected(tmp_path):
    errors = check.inspect(stage(tmp_path, "upstream/requirements.txt"), False)
    assert flagged(errors, "upstream/requirements.txt"), errors


def test_nested_upstream_serve_asset_is_rejected(tmp_path):
    errors = check.inspect(stage(tmp_path, "upstream/serve/index.html", "upstream/setup.sh"), False)
    assert flagged(errors, "upstream/serve/index.html"), errors
    assert flagged(errors, "upstream/setup.sh"), errors


def test_forbidden_file_below_the_root_is_rejected(tmp_path):
    files = ["vendor/strata/requirements.txt", "vendor/strata/docs/MCP_SERVER.md", "opt/START-HERE.bat"]
    errors = check.inspect(stage(tmp_path, *files), False)
    for relative in files:
        assert flagged(errors, relative), (relative, errors)


def test_forbidden_directory_below_the_root_is_rejected(tmp_path):
    files = ["pkg/serve/index.html", "pkg/sycl/serve/app.js", "pkg/.strata-mcp/state.json"]
    errors = check.inspect(stage(tmp_path, *files), False)
    for relative in files:
        assert flagged(errors, relative), (relative, errors)


def test_native_only_stage_passes(tmp_path):
    assert check.inspect(stage(tmp_path, "bin/strata-secure-server"), False) == []


def test_lookalike_names_pass(tmp_path):
    files = ["share/doc/upstream-notes.txt", "bin/requirements.txt.sig", "bin/observe/index.html"]
    assert check.inspect(stage(tmp_path, *files), False) == []
```

- [ ] **Step 2: Run them to see the depth bug**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
cd "$R" && pytest -q tools/secure/test_secure_distribution_check.py 2>&1 | tail -1
```

Expected: `4 failed, 2 passed in …s` (the four rejection tests fail: today only root-level paths are matched).

- [ ] **Step 3: Fix `scripts/secure_distribution_check.py`**

Replace the module docstring (lines 2–7) with:

```python
"""Fail closed when a candidate is not a secure-native Strata distribution.

This is a packaging gate, not a malware scanner.  It checks the selected
staging directory; it intentionally does *not* scan this source checkout,
which holds upstream Strata as the git submodule `upstream/` for provenance.
Any staged path with an `upstream` component is rejected, and the quarantined
names below are matched at any depth, not only at the staging root.
"""
```

Replace lines 19–21 (the comment above `FORBIDDEN_PATHS`) with:

```python
# These paths are deliberate quarantine boundaries, documented in
# docs/security/privacy-migration.md.  Match whole path components (at any
# depth) rather than a substring so an unrelated README cannot cause a false
# positive.
```

Replace `forbidden_reason` (lines 51–67) with:

```python
def _contains_components(parts: list[str], sequence: list[str]) -> bool:
    """True when `sequence` occurs as consecutive whole components of `parts`."""
    width = len(sequence)
    return any(parts[i:i + width] == sequence for i in range(len(parts) - width + 1))


def forbidden_reason(relative: str) -> str | None:
    parts = relative.split("/")
    if "upstream" in parts:
        return "upstream source tree (the git submodule upstream/) is never part of a secure-native package"
    if any(parts[-len(p.split("/")):] == p.split("/") for p in FORBIDDEN_PATHS):
        return "quarantined legacy installer, updater, dependency bootstrap, or MCP documentation"
    if any(_contains_components(parts, directory.split("/")) for directory in FORBIDDEN_DIRECTORIES):
        return "quarantined legacy Python service/MCP/telemetry directory"
    if parts[-1] in FORBIDDEN_PYTHON_NAMES:
        return "quarantined legacy Python server, MCP, telemetry, or metrics module"
    # A secure-native artifact must not silently bring an arbitrary Python
    # runtime back into the service path.  Python may only be shipped after a
    # future review adds a specific allowlist and regression coverage.
    if parts[-1].endswith(".py"):
        return "Python source is not permitted in the secure-native package"
    return None
```

- [ ] **Step 4: Run them to verify the fix**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
cd "$R" && pytest -q tools/secure/test_secure_distribution_check.py 2>&1 | tail -1
mkdir -p "$R/out/stage-native/bin" && printf x > "$R/out/stage-native/bin/strata-secure-server"
python3 "$R/scripts/secure_distribution_check.py" --package-dir "$R/out/stage-native"; echo "exit $?"
cmake -E rm -rf "$R/out/stage-native"
```

Expected: `6 passed in …s`; `secure distribution check: PASS (/opt/projects/ai/out/stage-native)`; `exit 0`.

- [ ] **Step 5: Move the GLM test and add the extract tests (they must fail: no wrapper yet)**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
git -C "$R" mv tools/test_glm_tokenizer.py tools/secure/test_glm_tokenizer.py
```

In `tools/secure/test_glm_tokenizer.py` replace exactly this block:

```python
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from strata_tokenizer import (  # noqa: E402
    GLM4_PATTERN,
    QWEN35_PATTERN,
    Tokenizer,
    bytes_to_unicode,
    pre_tokenizer_pattern,
)
```

with (spec §4.4; nothing else in the file changes):

```python
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from secure_tokenizer import (GLM4_PATTERN, QWEN35_PATTERN, Tokenizer,  # noqa: E402
                              bytes_to_unicode, pre_tokenizer_pattern)
```

Create `tools/secure/test_secure_tokenizer.py`:

```python
"""extract() through tools/secure/secure_tokenizer.py ships the declared pre-tokenizer's pattern.

Upstream's extract() always writes the Qwen35 pattern; the wrapper must rewrite it for GLM packs.
The GGUF here is a tiny metadata-only v3 file written with struct: no weights, no numpy, no network.
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from secure_tokenizer import GLM4_PATTERN, QWEN35_PATTERN, bytes_to_unicode, extract, main  # noqa: E402

GGUF_STRING, GGUF_ARRAY = 8, 9


def _string(text: str) -> bytes:
    raw = text.encode("utf-8")
    return struct.pack("<Q", len(raw)) + raw


def _strings(values: list[str]) -> bytes:
    return struct.pack("<IQ", GGUF_STRING, len(values)) + b"".join(_string(v) for v in values)


def write_tiny_gguf(path: Path, pre: str) -> Path:
    tokens = list(bytes_to_unicode().values()) + ["12", "123"]
    entries = [
        ("tokenizer.ggml.model", GGUF_STRING, _string("gpt2")),
        ("tokenizer.ggml.pre", GGUF_STRING, _string(pre)),
        ("tokenizer.ggml.tokens", GGUF_ARRAY, _strings(tokens)),
        ("tokenizer.ggml.merges", GGUF_ARRAY, _strings(["1 2", "12 3"])),
    ]
    blob = struct.pack("<IIQQ", 0x46554747, 3, 0, len(entries))  # "GGUF", v3, 0 tensors
    for key, type_id, payload in entries:
        blob += _string(key) + struct.pack("<I", type_id) + payload
    path.write_bytes(blob)
    return path


def shipped(out_dir: Path) -> dict:
    return json.loads((out_dir / "tokenizer" / "tokenizer.json").read_text(encoding="utf-8"))


def test_extract_ships_the_glm_pattern_for_a_glm_pack(tmp_path):
    out_dir = tmp_path / "pack"
    cfg = extract(write_tiny_gguf(tmp_path / "glm.gguf", "chatglm-bpe"), out_dir)
    assert cfg["pre"] == "chatglm-bpe"
    assert shipped(out_dir)["pre_pattern"] == GLM4_PATTERN
    assert "CHATGLM4" in shipped(out_dir)["pre_pattern_source"]
    assert shipped(out_dir) == cfg


def test_cli_keeps_the_qwen_pattern_for_a_qwen35_pack(tmp_path):
    out_dir = tmp_path / "pack"
    assert main(["--gguf", str(write_tiny_gguf(tmp_path / "qwen.gguf", "qwen35")), "--out", str(out_dir)]) == 0
    assert shipped(out_dir)["pre_pattern"] == QWEN35_PATTERN
    assert "QWEN35" in shipped(out_dir)["pre_pattern_source"]
```

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
cd "$R" && pytest -q tools/secure/test_glm_tokenizer.py tools/secure/test_secure_tokenizer.py 2>&1 | tail -1
```

Expected: `2 errors in …s` (`ModuleNotFoundError: No module named 'secure_tokenizer'` for both modules).

- [ ] **Step 6: Write the wrapper `tools/secure/secure_tokenizer.py`**

```python
"""tools/secure/secure_tokenizer.py - GLM pre-tokenizer aliases over upstream's tokenizer, without editing it.

Upstream's `upstream/tools/strata_tokenizer.py` (the git submodule, never edited) knows only the `qwen35`
pre-tokenizer.  GLM-5.3 tokenizer packs declare `glm4` or the older `chatglm-bpe` alias, whose regex caps
numeric runs at three digits.  This module imports upstream's tokenizer and adds:

  * GLM4_PATTERN and pre_tokenizer_pattern(pre) -> (regex, provenance);
  * Tokenizer, a subclass that recompiles `_re` from the declared `pre`;
  * extract(), which calls upstream's extract() and rewrites `pre_pattern`/`pre_pattern_source` in the
    pack's tokenizer.json, because upstream ships the Qwen35 pattern for every pack.

The GLM regex was transcribed from lighttransport/Strata `glm53f` @ 4894c996 `tools/strata_tokenizer.py`
(see docs/PROVENANCE.md).  Upstream's module inserts its own directory into sys.path for gguf_reader, so it
is imported here as a top-level module from upstream/tools.
"""
from __future__ import annotations

import json
import pathlib
import sys

import regex

_UPSTREAM_TOOLS = pathlib.Path(__file__).resolve().parents[2] / "upstream" / "tools"
if not (_UPSTREAM_TOOLS / "strata_tokenizer.py").is_file():
    raise ImportError("upstream/tools/strata_tokenizer.py is missing: run `git submodule update --init` "
                      "from the repository root")
if str(_UPSTREAM_TOOLS) not in sys.path:
    sys.path.insert(0, str(_UPSTREAM_TOOLS))
import strata_tokenizer as _upstream  # noqa: E402

QWEN35_PATTERN = _upstream.QWEN35_PATTERN
bytes_to_unicode = _upstream.bytes_to_unicode

# llama.cpp's LLAMA_VOCAB_PRE_TYPE_CHATGLM4.  GLM-5.3 tokenizer packs identify
# this as either `glm4` or the older `chatglm-bpe` alias.  The three-digit cap
# is semantic: using the Qwen pattern here tokenizes every digit separately,
# while accepting arbitrarily long numeric runs changes BPE merge boundaries.
GLM4_PATTERN = (
    r"(?:'[sS]|'[tT]|'[rR][eE]|'[vV][eE]|'[mM]|'[lL][lL]|'[dD])"
    r"|[^\r\n\p{L}\p{N}]?\p{L}+"
    r"|\p{N}{1,3}"
    r"| ?[^\s\p{L}\p{N}]+[\r\n]*"
    r"|\s*[\r\n]+|\s+(?!\S)|\s+"
)
GLM4_PRE_NAMES = ("glm4", "chatglm-bpe")


def pre_tokenizer_pattern(pre: str) -> tuple[str, str]:
    """Return the fixed regex and provenance for a declared GGUF pre-tokenizer.

    Unknown pre-tokenizers retain the historic Qwen35 fallback so this utility
    remains usable for existing Strata packs.  A caller that needs exact
    compatibility must validate `pre` against its model profile before use.
    """
    if pre in GLM4_PRE_NAMES:
        return (GLM4_PATTERN,
                ".ref/llama.cpp src/llama-vocab.cpp (LLAMA_VOCAB_PRE_TYPE_CHATGLM4)")
    return (QWEN35_PATTERN,
            ".ref/llama.cpp src/llama-vocab.cpp L396 (LLAMA_VOCAB_PRE_TYPE_QWEN35)")


class Tokenizer(_upstream.Tokenizer):
    """Upstream's tokenizer with the pre-tokenizer regex chosen by the declared `pre`."""

    def __init__(self, tokens: list[str], merges: list[str], token_types: list[int] | None = None,
                 pre: str = "qwen35", special_ids: dict[str, int] | None = None):
        super().__init__(tokens, merges, token_types, pre, special_ids)
        self._re = regex.compile(pre_tokenizer_pattern(self.pre)[0])


def extract(gguf_path, out_dir) -> dict:
    """Upstream's extract(), then ship the declared pre-tokenizer's pattern in tokenizer.json."""
    cfg = _upstream.extract(gguf_path, out_dir)
    cfg["pre_pattern"], cfg["pre_pattern_source"] = pre_tokenizer_pattern(cfg["pre"])
    path = pathlib.Path(out_dir) / "tokenizer" / "tokenizer.json"
    path.write_text(json.dumps(cfg, ensure_ascii=False, indent=1), encoding="utf-8")
    return cfg


def main(argv: list[str] | None = None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Write a pack's tokenizer/ with the declared pre-tokenizer's pattern")
    ap.add_argument("--gguf", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    cfg = extract(args.gguf, args.out)
    print("tokenizer: pre=%s vocab=%d merges=%d" % (cfg["pre"], cfg["vocab_size"], cfg["n_merges"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 7: Run the tokenizer tests**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
cd "$R" && pytest -q tools/secure/test_glm_tokenizer.py tools/secure/test_secure_tokenizer.py 2>&1 | tail -1
```

Expected: `6 passed in …s`.

- [ ] **Step 8: Prove the wrapper, not upstream, supplies the GLM behavior**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
cd "$R" && python3 -c "
import sys; sys.path.insert(0, 'upstream/tools'); import strata_tokenizer as u
print('upstream has GLM4_PATTERN:', hasattr(u, 'GLM4_PATTERN'))"
git -C "$R/upstream" status --porcelain
```

Expected: `upstream has GLM4_PATTERN: False`, and nothing from the submodule (upstream's `.gitignore` ignores `__pycache__/`).

- [ ] **Step 9: Ours-only `pytest.ini` and `conftest.py` (whole files)**

`pytest.ini`:

```ini
# Routine `pytest -q` (and the owner's pre-push hook) runs our tests only:
# tools/secure/ (the GLM tokenizer wrapper and the secure distribution check).
# Upstream's own suites live in the upstream/ submodule and run explicitly
# after a submodule bump, with upstream's recorded exclusions; see
# docs/AGENT_HANDOFF.md, "Update upstream".
[pytest]
testpaths = tools/secure
python_files = test_*.py
```

`conftest.py`:

```python
"""Repository-root pytest configuration.

Routine `pytest -q` collects only tools/secure/ (pytest.ini).  Those tests import upstream's tokenizer through
tools/secure/secure_tokenizer.py, so the upstream/ submodule must be checked out; stop with the instruction
instead of an import traceback when it is not.
"""
from pathlib import Path

import pytest

UPSTREAM_TOKENIZER = Path(__file__).resolve().parent / "upstream" / "tools" / "strata_tokenizer.py"


def pytest_sessionstart(session):
    if not UPSTREAM_TOKENIZER.is_file():
        pytest.exit("upstream/ is not checked out (upstream/tools/strata_tokenizer.py is missing): "
                    "run `git submodule update --init` from the repository root", returncode=4)
```

- [ ] **Step 10: Dev requirements carry the pins; bootstrap reads only them**

`scripts/dev/requirements-dev.txt` (whole file):

```
# Developer tooling for the secure frontend (scripts/dev/bootstrap-linux.sh),
# pinned at the versions verified in .venv-dev on 2026-10-13. This is the only
# requirements file the bootstrap reads: upstream's requirements.txt lives in
# the upstream/ submodule and is not installed into .venv-dev.
# build tools for the presets and the root build
cmake==4.4.3
ninja==1.13.2
# tools/secure: the GLM tokenizer wrapper imports upstream's tokenizer, which needs regex
regex==2026.9.10
pytest==9.1.1
# pytest's dependencies
pluggy==1.6.0
iniconfig==2.3.1
packaging==26.3
pygments==2.21.0
exceptiongroup==1.3.1; python_version < "3.11"
tomli==2.5.0; python_version < "3.11"
typing_extensions==4.16.0; python_version < "3.13"
```

In `scripts/dev/bootstrap-linux.sh` replace lines 2–7:

```bash
# Creates the project-local developer environment for the secure frontend on
# Linux without root: a venv holding cmake/ninja pinned in requirements.txt and
# pytest (with its dependencies) pinned in scripts/dev/requirements-dev.txt.
# Idempotent. Offline: set STRATA_WHEEL_DIR to a directory holding every wheel
# both files name PLUS a pip wheel (pip-*.whl) for the venv's pip bootstrap;
# nothing is fetched from the network when it is set.
```

with:

```bash
# Creates the project-local developer environment for the secure frontend on
# Linux without root: a venv holding cmake, ninja, regex, pytest and pytest's
# dependencies, all pinned in scripts/dev/requirements-dev.txt (the only file
# read; upstream's requirements.txt in the upstream/ submodule is not ours).
# Idempotent. Offline: set STRATA_WHEEL_DIR to a directory holding every wheel
# that file names PLUS a pip wheel (pip-*.whl) for the venv's pip bootstrap;
# nothing is fetched from the network when it is set.
```

and replace line 41:

```bash
pip_args=(-q -r "$repo/requirements.txt" -r "$repo/scripts/dev/requirements-dev.txt")
```

with:

```bash
pip_args=(-q -r "$repo/scripts/dev/requirements-dev.txt")
```

- [ ] **Step 11: Full routine pytest; Review Focus 1 for pytest (empty `upstream/`)**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
cd "$R" && pytest -q 2>&1 | tail -1
cd "$R" && pytest -q --collect-only 2>/dev/null | grep '::' | grep -vc '^tools/secure/'
git -C "$R" submodule deinit -f upstream > /dev/null
cd "$R" && pytest -q > "$R/out/pytest-empty.log" 2>&1; echo "pytest exit $?"
grep -o 'run `git submodule update --init` from the repository root' "$R/out/pytest-empty.log"
git -C "$R" submodule update --init upstream > /dev/null
git -C "$R" submodule status
cmake -E rm -f "$R/out/pytest-empty.log"
```

Expected:
```
12 passed in …s
0
pytest exit 4
run `git submodule update --init` from the repository root
 7695859aa13afb248bdb4d36f9b2e95707db079d upstream (v0.1.43)
```

- [ ] **Step 12: Review Focus 3 — pytest leaves the submodule clean; bootstrap works on the existing and on a fresh venv**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
git -C "$R/upstream" status --porcelain
git -C "$R" submodule status | grep -cE '^[-+U]'
grep -c '\$repo/requirements.txt' "$R/scripts/dev/bootstrap-linux.sh"
"$R/scripts/dev/bootstrap-linux.sh"
F="$(mktemp -d)"; mkdir -p "$F/repo"; cp -r "$R/scripts" "$F/repo/scripts"
"$F/repo/scripts/dev/bootstrap-linux.sh" | tail -2
cd "$R" && "$F/repo/.venv-dev/bin/pytest" -q 2>&1 | tail -1
"$F/repo/.venv-dev/bin/python" -c "import importlib.util as u; print('numpy in fresh venv:', u.find_spec('numpy') is not None)"
cmake -E rm -rf "$F"
```

Expected: no submodule status output; `0`; `0` (the bootstrap no longer names the root `requirements.txt`); the existing venv prints `bootstrap-linux: ready. Use: export PATH=/opt/projects/ai/.venv-dev/bin:$PATH`, `cmake version 4.4.3`, `1.13.2`; the fresh venv prints `cmake version 4.4.3` and `1.13.2`; the fresh-venv pytest prints `12 passed in …s`; `numpy in fresh venv: False` (routine pytest needs nothing from upstream's `requirements.txt`).

- [ ] **Step 13: Commit**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
TRAILER=$'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\nClaude-Session: <session link>'
git -C "$R" add tools/secure pytest.ini conftest.py scripts/secure_distribution_check.py scripts/dev/requirements-dev.txt scripts/dev/bootstrap-linux.sh
git -C "$R" status --short
git -C "$R" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "tools/secure: GLM tokenizer wrapper over upstream/; ours-only pytest; dist check at any depth (U4, U5, U8, U9)" \
  -m "secure_tokenizer.py imports upstream/tools/strata_tokenizer.py unmodified and adds GLM4_PATTERN, pre_tokenizer_pattern, a Tokenizer subclass and an extract() that ships the declared pattern. test_glm_tokenizer.py moved (import only). pytest.ini: testpaths = tools/secure, no ignores; conftest stops with the git submodule update --init instruction when upstream/ is empty. requirements-dev.txt pins cmake/ninja/regex; bootstrap reads only it. secure_distribution_check rejects any upstream component and quarantined names at any depth. pytest -q: 12 passed." \
  -m "$TRAILER"
```

Expected `git status --short` before the commit: `R  tools/test_glm_tokenizer.py -> tools/secure/test_glm_tokenizer.py` (or `RM`), `A` for the three new files under `tools/secure/`, `M` for the five modified files.

---

### Task 4: Remove every upstream file from our tree; ours-only `.gitignore`, `.gitattributes`, `README.md`, `LICENSE` (U1, U6, U7)

**Files:**
- Delete (`git rm -r`): the 192 pathspecs below (2 971 tracked files: every path of upstream's `v0.1.43` tree except the five root files rewritten here)
- Modify (full rewrite): `.gitignore`, `.gitattributes`, `README.md`, `LICENSE`

**Interfaces:**
- Consumes: `upstream/` populated (Task 1), our root `CMakeLists.txt` (Task 2), `tools/secure/` (Task 3).
- Produces: the ours-only root. Top level (`git ls-files | sed 's#/.*##' | LC_ALL=C sort -u`): `.gitattributes .github .gitignore .gitmodules CMakeLists.txt LICENSE README.md conftest.py docs pytest.ini scripts secure_frontend tests tools upstream`. Our `.gitignore` entries (Tasks 5–7 rely on `/out/` and on `/.venv-dev`, `/.toolchains` matching the worktree symlinks).

The exact removal list (derived from `git ls-tree -r --name-only 7695859a` minus `CMakeLists.txt .gitignore .gitattributes LICENSE README.md`, grouped by directory). Upstream's `docs/` contributes 35 `*.md` files plus four directories; `tools/` contributes 113 entries (`tools/hip/`, `tools/vision/` and 111 files, incl. `strata_tokenizer.py`; our `tools/test_glm_tokenizer.py` already moved in Task 3):

```
.dockerignore
.github/FUNDING.yml
.github/ISSUE_TEMPLATE/
.github/pull_request_template.md
AGENTS.md
BENCH.bat
Dockerfile
README.de.md
README.es.md
README.fr.md
README.ja.md
README.pt-BR.md
README.zh-CN.md
SECURITY.md
SETUP.bat
START-HERE.bat
UPDATE.bat
bench.sh
bench/
chat.py
cmake/
data/
docker-entrypoint.sh
docs/A770_SPEED_SPEC.md
docs/AI_SETUP.md
docs/AMD_HIP.md
docs/AMD_HIP_PERFORMANCE.md
docs/BATCHED_DMA.md
docs/BATCHING.md
docs/COMMUNITY_BENCHMARKS.md
docs/DETAILS.md
docs/DISJOINT_EXPERT_CACHE.md
docs/EXCHANGE_ROTATION.md
docs/HOW_IT_WORKS.md
docs/INSTALL.md
docs/INTEL.md
docs/INTEL_ARC.md
docs/KV_PREFETCH.md
docs/LLAMA_SWAP.md
docs/LOGIT_BIAS.md
docs/MCP_SERVER.md
docs/MESSAGE_BOUNDARY_CACHE.md
docs/MMVQ_IL_TABLE.md
docs/MODELS.md
docs/MULTI_GPU.md
docs/NVIDIA_V100.md
docs/OLDER_GPUS.md
docs/ORCA.md
docs/ORCA_Q4_K_S.md
docs/PROMPT_CACHE_TAIL.md
docs/RESEARCH_RUNS.md
docs/SECOND_GPU.md
docs/STRIX_HALO.md
docs/TEST_REQUESTS.md
docs/TROUBLESHOOTING.md
docs/UNSLOTH_Q4.md
docs/UNSLOTH_Q6.md
docs/VRAM_ELASTIC.md
docs/benchmarks/
docs/media/
docs/monitoring/
docs/paper/
include/
ref/
requirements.txt
serve/
setup.py
setup.sh
src/
sycl/
tests/core/
tests/cuda/
tests/data/
tests/hip/
tests/platform/
tests/prefill/
tests/program/
third_party/
tools/_paths.py
tools/ab_engine.py
tools/agent_turns_bench.py
tools/batch_groups_cache_test.py
tools/batch_interleave_test.py
tools/batch_test.py
tools/bench_eviction.py
tools/calibrate.py
tools/cancel_park_test.py
tools/canonical_xcheck.py
tools/check_isa_guard.py
tools/collect_profile_traces.py
tools/conversation_cache_disabled.py
tools/conversation_cache_growth.py
tools/conversation_cache_http_smoke.py
tools/conversation_cache_isolation.py
tools/conversation_cache_parity.py
tools/conversation_cache_soak.py
tools/conversation_restore_speed.py
tools/draft_vocab.py
tools/early_close_test.py
tools/embd_bf16_pack.py
tools/gguf_reader.py
tools/gguf_writer.py
tools/h2d_bench.cu
tools/hip/
tools/iq_fixture.py
tools/iq_pack.py
tools/logit_bias_english_example.py
tools/logit_bias_http_probe.py
tools/make_profile.py
tools/make_profile_mix.py
tools/mtp_fetch.py
tools/mtp_pack.py
tools/mtp_rt.py
tools/needle_bench.py
tools/pack_index.py
tools/pack_layer.py
tools/parking_test.py
tools/ple_fp8_pack.py
tools/quantscope.py
tools/replay_cache_policy.py
tools/research_run.py
tools/serve_load.py
tools/session_pressure_test.py
tools/sm60_dp4a_check.cu
tools/split_mtp_batch_parity.py
tools/stop_resend_test.py
tools/strata_bench.py
tools/strata_inspect.py
tools/strata_mcp.py
tools/strata_pack.py
tools/strata_tokenizer.py
tools/test_bench_eviction.py
tools/test_calibrate.py
tools/test_check_isa_guard.py
tools/test_conversation_cache_disabled.py
tools/test_conversation_cache_growth.py
tools/test_conversation_cache_http_smoke.py
tools/test_conversation_cache_isolation.py
tools/test_conversation_cache_parity.py
tools/test_conversation_cache_soak.py
tools/test_conversation_restore_speed.py
tools/test_dll_closure.py
tools/test_docker_entrypoint.py
tools/test_draft_vocab.py
tools/test_effort_end_args.py
tools/test_iq_pack.py
tools/test_make_profile.py
tools/test_make_profile_mix.py
tools/test_mtp_fetch.py
tools/test_research_run.py
tools/test_setup_amd.py
tools/test_setup_bench_tips.py
tools/test_setup_choices.py
tools/test_setup_compute_mode.py
tools/test_setup_config.py
tools/test_setup_configs.py
tools/test_setup_draft_vocab.py
tools/test_setup_engine_hash.py
tools/test_setup_engine_keep.py
tools/test_setup_golden.json
tools/test_setup_golden.py
tools/test_setup_hotfix_tag.py
tools/test_setup_hybrid.py
tools/test_setup_llama_archive.py
tools/test_setup_lowram.py
tools/test_setup_modelscope.py
tools/test_setup_oldcpu.py
tools/test_setup_older_gpus.py
tools/test_setup_page_file.py
tools/test_setup_parallel.py
tools/test_setup_pcie.py
tools/test_setup_pins.py
tools/test_setup_prompts.py
tools/test_setup_remote_opt.py
tools/test_setup_risk.py
tools/test_setup_rocm_indexes.json
tools/test_setup_rocm_pins.py
tools/test_setup_rope.py
tools/test_setup_start_gpus_amd.py
tools/test_setup_strix_halo.py
tools/test_setup_sycl.py
tools/test_setup_unsloth.py
tools/test_setup_update.py
tools/test_setup_update_gpu.py
tools/test_shards.py
tools/test_strata_bench.py
tools/test_strata_inspect.py
tools/test_strata_mcp.py
tools/test_strata_tokenizer.py
tools/test_tokenizer_plain_spans.py
tools/vision/
update.sh
```

- [ ] **Step 1: Failing precondition — upstream files are still ours**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
comm -12 <(git -C "$R" ls-files | LC_ALL=C sort) <(git -C "$R" ls-tree -r --name-only 7695859aa13afb248bdb4d36f9b2e95707db079d | LC_ALL=C sort) | wc -l
```

Expected: `2976` (2 971 upstream files plus the five root files).

- [ ] **Step 2: Derive the pathspec list and check it equals the list above**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
git -C "$R" ls-tree -r --name-only 7695859aa13afb248bdb4d36f9b2e95707db079d \
  | grep -vxF -e CMakeLists.txt -e .gitignore -e .gitattributes -e LICENSE -e README.md \
  | awk -F/ '{ if ($1==".github"||$1=="docs"||$1=="tests"||$1=="tools") { if (NF>2) print $1"/"$2"/"; else print $0 } else if (NF>1) print $1"/"; else print $0 }' \
  | LC_ALL=C sort -u > "$R/out/upstream-rm-pathspecs.txt"
wc -l < "$R/out/upstream-rm-pathspecs.txt"
head -3 "$R/out/upstream-rm-pathspecs.txt"; tail -2 "$R/out/upstream-rm-pathspecs.txt"
```

Expected: `192`; first lines `.dockerignore`, `.github/FUNDING.yml`, `.github/ISSUE_TEMPLATE/`; last lines `tools/vision/`, `update.sh` — the same 192 lines printed above (compare by eye or paste the block above into a file and `diff`).

- [ ] **Step 3: `git rm` them**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
git -C "$R" --literal-pathspecs rm -r -q --pathspec-from-file="$R/out/upstream-rm-pathspecs.txt"
git -C "$R" diff --cached --name-only --diff-filter=D | wc -l
cmake -E rm -f "$R/out/upstream-rm-pathspecs.txt"
```

Expected: `2971`.

- [ ] **Step 4: Remove the ignored leftovers that kept upstream directories alive**

`git rm` leaves ignored `__pycache__/` directories behind. List, then remove only ignored content in the former upstream directories, then empty directories there:

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
git -C "$R" clean -ndX -- bench ref serve sycl tools/__pycache__ tools/hip
git -C "$R" clean -fdX -- bench ref serve sycl tools/__pycache__ tools/hip > /dev/null
for d in bench ref serve sycl tools/hip; do test -d "$R/$d" && find "$R/$d" -depth -type d -empty -delete; done
for d in bench ref serve sycl tools/hip; do test -e "$R/$d" && echo "still there: $d"; done; echo "leftover check done"
```

Expected dry-run lines (exactly the ignored caches present on the dev host; a host with fewer caches prints fewer):
```
Would remove bench/__pycache__/
Would remove bench/results/2026-10-09-community-rtx5090-9800x3d-swift-1m/__pycache__/
Would remove ref/__pycache__/
Would remove serve/__pycache__/
Would remove sycl/__pycache__/
Would remove tools/__pycache__/
Would remove tools/hip/__pycache__/
```
then only `leftover check done`.

- [ ] **Step 5: Write our `.gitignore` (whole file)**

```
# Secure Strata (ours). Upstream's own ignores live in upstream/.gitignore.

# developer environment (scripts/dev/bootstrap-*.sh); no trailing slash so the
# symlinks inside .worktrees/<name>/ are ignored too
/.venv-dev
/.toolchains

# parallel sub-project worktrees (superpowers finishing skill cleans these)
/.worktrees/
# superpowers execution state
/.superpowers/

# build output: secure_frontend/ presets and our root CMakeLists.txt
/secure_frontend/out/
/secure_frontend/build/
/out/

# Python
__pycache__/
.pytest_cache/
```

- [ ] **Step 6: Write our `.gitattributes` (whole file)**

```
# Shell scripts keep LF line endings wherever the repository is cloned.
*.sh text eol=lf
```

- [ ] **Step 7: Write our `LICENSE` and `NOTICE.md`**

`LICENSE` is the GNU Affero General Public License, version 3: the FSF's text, byte for byte
(https://www.gnu.org/licenses/agpl-3.0.txt). `NOTICE.md` holds our copyright line, Strata's license notice with its
retained "Copyright (c) 2026 Niko1221 and the Strata contributors" line, and:

```
`upstream/` is a git submodule of Niko1221/Strata under its own
`upstream/LICENSE`.
```

- [ ] **Step 8: Write our `README.md` (whole file)**

````markdown
# Secure Strata (multi-strata)

This repository (`shift-f10/multi-strata`) builds a security- and privacy-first frontend around [Niko1221/Strata](https://github.com/Niko1221/Strata/tree/v0.1.43) and is a mixture of the sources below. Upstream Strata is not copied into this tree: it is the git submodule [`upstream/`](upstream/), pinned at `v0.1.43` and never modified. The repository root holds only our code.

Get the code with the submodule: `git clone --recurse-submodules https://github.com/shift-f10/multi-strata.git`, or run `git submodule update --init` after a plain clone.

## Sources and credits

| Source | What we took from it | Revision | License |
| --- | --- | --- | --- |
| [Niko1221/Strata](https://github.com/Niko1221/Strata/tree/v0.1.43) | The engine, Python server, installer and docs, used unmodified from the git submodule `upstream/`. This project started as a fork of it. | tag `v0.1.43`, commit `7695859aa13afb248bdb4d36f9b2e95707db079d` (the `upstream` gitlink) | MIT, Copyright (c) 2026 Niko1221 and the Strata contributors ([`upstream/LICENSE`](upstream/LICENSE); the line is retained in our [`LICENSE`](LICENSE)) |
| [robertkarl/UnbiasedStrata](https://github.com/robertkarl/UnbiasedStrata/tree/46c46ecfc11d2315512f21f7549b0a7792ccd2ae) | Security-design ideas only (smaller, offline-verifiable runtime; reduced service attack surface). No code copied. | commit `46c46ecfc11d2315512f21f7549b0a7792ccd2ae` | MIT (per the GitHub license API, 2026-10-13; see [`docs/PROVENANCE.md`](docs/PROVENANCE.md)); no code taken, so it governs nothing here |
| [lighttransport/Strata](https://github.com/lighttransport/Strata/tree/glm53f) | GLM-5.3 compatibility reference: the native GLM-5.3 serving profile and `glm4` / `chatglm-bpe` tokenizer handling were implemented against its observable contracts (the tokenizer regex was transcribed from its `tools/strata_tokenizer.py` into our wrapper [`tools/secure/secure_tokenizer.py`](tools/secure/secure_tokenizer.py)). Its engine and kernels are not ported yet. | branch `glm53f`, commit `4894c9967f5754a2052d7b05f10a01b6071bb6e4` | MIT, Copyright (c) 2026 Niko1221 and the Strata contributors (per the GitHub API, 2026-10-13; see [`docs/PROVENANCE.md`](docs/PROVENANCE.md)) |
| [ggml-org/llama.cpp](https://github.com/ggml-org/llama.cpp) / ggml | Comes with upstream Strata, unchanged by us: `upstream/third_party/ggml` and the pinned llama.cpp checkout its build fetches (only with `STRATA_NATIVE_EXPERTS=ON`). | commit `3cf03257f219afbe7334045ff7c6a06ac68c627d` | MIT ([`upstream/third_party/ggml/LICENSE`](upstream/third_party/ggml/LICENSE)) |
| [mstorsjo/llvm-mingw](https://github.com/mstorsjo/llvm-mingw) | Build tool only: the open-source Windows cross toolchain, fetched by `scripts/dev/bootstrap-llvm-mingw.sh` with a pinned sha256. Nothing from it is in this tree. | release `20261006` | GitHub API reports `NOASSERTION` (Other); its `LICENSE` puts the llvm-mingw scripts under ISC, bundled components keep their own licenses (see [`docs/PROVENANCE.md`](docs/PROVENANCE.md)) |

## What is different here

- **Exists:** a dependency-free native C++20 security frontend that owns the listener and starts the unmodified upstream engine as a private child process over standard I/O. It does not yet replace the upstream Python service in a release. See [`secure_frontend/`](secure_frontend/).
- **Exists:** a bearer token is required on every listener, including loopback; the default bind is loopback, and remote and wildcard binds need separate opt-ins. See [`docs/security/threat-model.md`](docs/security/threat-model.md).
- **Planned:** the token is generated for the user and used by the client automatically, so it is never typed. Today it must be supplied. See the [product spec](docs/superpowers/specs/2026-10-13-product-experience-and-upstream-tracking-design.md).
- **Planned:** no telemetry, no update checks, no automatic downloads; network use only in an explicit, digest-verified `pull` or `update`. The native frontend has no download or outbound code today, but the upstream Python service is still in the `upstream/` submodule, so nothing here is privacy-hardened yet. See [`docs/AGENT_HANDOFF.md`](docs/AGENT_HANDOFF.md).
- **Exists:** Linux and Windows builds with an open-source Windows toolchain (llvm-mingw, MSYS2 clang) and CI on real Windows. Nothing has run on a GPU or against a real engine yet. See [`docs/secure-frontend-ci.md`](docs/secure-frontend-ci.md).
- **Exists:** security acceptance tests specified by ID, to run hermetically. Most are not automated yet. See [`tests/security/README.md`](tests/security/README.md).
- **Exists:** upstream stays trackable: it is a pinned, unmodified git submodule, and an update is a pointer bump plus the gates. See [`docs/PROVENANCE.md`](docs/PROVENANCE.md).
- **Exists:** a fixed GLM-5.3 serving profile and GLM tokenizer handling. The GLM engine port is planned. See [`docs/AGENT_HANDOFF.md`](docs/AGENT_HANDOFF.md).
- **Planned:** an Ollama-style `strata` CLI (`pull`, `run`, `serve`, `list`). It does not exist yet. See the [product spec](docs/superpowers/specs/2026-10-13-product-experience-and-upstream-tracking-design.md).

Nothing here is released. The full attribution record is [`docs/PROVENANCE.md`](docs/PROVENANCE.md); current status is in [`docs/AGENT_HANDOFF.md`](docs/AGENT_HANDOFF.md).

---

Upstream Strata's README: [`upstream/README.md`](upstream/README.md).
````

- [ ] **Step 9: Verify the ours-only tree, links and license**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
git -C "$R" add -A .gitignore .gitattributes LICENSE README.md
comm -12 <(git -C "$R" ls-files | LC_ALL=C sort) <(git -C "$R" ls-tree -r --name-only 7695859aa13afb248bdb4d36f9b2e95707db079d | LC_ALL=C sort) | tr '\n' ' '; echo
git -C "$R" ls-files | sed 's#/.*##' | LC_ALL=C sort -u | tr '\n' ' '; echo
git -C "$R" ls-files | grep -vE '^(\.github/workflows/secure-frontend\.yml|\.gitattributes|\.gitignore|\.gitmodules|CMakeLists\.txt|LICENSE|README\.md|conftest\.py|pytest\.ini|docs/(AGENT_HANDOFF\.md|PROVENANCE\.md|secure-frontend-ci\.md|security/.*|superpowers/.*)|scripts/.*|secure_frontend/.*|tests/security/.*|tools/secure/.*|upstream)$'; echo "non-ours paths: $?"
grep -oE '\]\([^)#]+' "$R/README.md" | sed 's/^](//' | grep -vE '^https?://' | LC_ALL=C sort -u | while read -r p; do test -e "$R/$p" && echo "ok $p" || echo "MISSING $p"; done
grep -cx 'GNU AFFERO GENERAL PUBLIC LICENSE' "$R/LICENSE"
grep -cx 'Copyright (c) 2026 Niko1221 and the Strata contributors' "$R/NOTICE.md"
git -C "$R" check-attr eol -- scripts/dev/bootstrap-linux.sh
git -C "$R" status --short --ignored | grep -v '^[MADR] ' | grep -v '^R'
```

Expected:
```
.gitattributes .gitignore CMakeLists.txt LICENSE README.md
.gitattributes .github .gitignore .gitmodules CMakeLists.txt LICENSE README.md conftest.py docs pytest.ini scripts secure_frontend tests tools upstream
non-ours paths: 1
ok LICENSE
ok docs/AGENT_HANDOFF.md
ok docs/PROVENANCE.md
ok docs/secure-frontend-ci.md
ok docs/security/threat-model.md
ok docs/superpowers/specs/2026-10-13-product-experience-and-upstream-tracking-design.md
ok secure_frontend/
ok tests/security/README.md
ok tools/secure/secure_tokenizer.py
ok upstream/
ok upstream/LICENSE
ok upstream/README.md
ok upstream/third_party/ggml/LICENSE
1
1
scripts/dev/bootstrap-linux.sh: eol: lf
```
followed only by ignored entries (`!! .pytest_cache/`, `!! .superpowers/`, `!! .toolchains/`, `!! .venv-dev/`, `!! .worktrees/`, `!! __pycache__/`, `!! out/`, `!! scripts/__pycache__/`, `!! secure_frontend/build/`, `!! secure_frontend/out/`, `!! tools/secure/__pycache__/`) — no `??` line. The five names on the first line are the files rewritten in this task (they share names with upstream's root files; their content is ours).

- [ ] **Step 10: Gates still pass on the ours-only tree**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
cd "$R" && pytest -q 2>&1 | tail -1
cmake --build "$R/out/root" 2>&1 | tail -1
ctest --test-dir "$R/out/root" 2>&1 | grep 'tests passed'
cd "$R/secure_frontend" && cmake --preset linux-gcc > /dev/null && cmake --build --preset linux-gcc > /dev/null && ctest --preset linux-gcc 2>&1 | grep 'tests passed'
git -C "$R/upstream" status --porcelain
```

Expected: `12 passed in …s`; `ninja: no work to do.`; `100% tests passed, 0 tests failed out of 20` twice; nothing from the submodule.

- [ ] **Step 11: Commit**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
TRAILER=$'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\nClaude-Session: <session link>'
git -C "$R" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "layout: remove the in-tree upstream copy; ours-only .gitignore, .gitattributes, README, LICENSE (U1, U6, U7)" \
  -m "git rm of 2971 files (every path of upstream v0.1.43 except the five root files rewritten here); upstream now lives only in the upstream/ submodule. README = our attribution header with upstream/ links and a pointer to upstream/README.md. LICENSE = the AGPL-3.0; NOTICE.md = Strata's notice with the retained Niko1221 line and a pointer to upstream/LICENSE. pytest -q 12 passed; root ctest 20/20; linux-gcc 20/20." \
  -m "$TRAILER"
git -C "$R" status --short
```

Expected: `git status --short` prints nothing.

---

### Task 5: CI — submodule checkout where needed, our root in `top-level-integration`, submodule status checks (U10)

**Files:**
- Modify (full rewrite): `.github/workflows/secure-frontend.yml`

**Interfaces:**
- Consumes: our root `CMakeLists.txt` options (Task 2), `pytest.ini` collection (Task 3), `scripts/dev/bootstrap-linux.sh` reading only `requirements-dev.txt` (Task 3).
- Produces: job ids unchanged (`linux`, `windows-cross`, `windows-msys2`, `windows-msvc`, `top-level-integration`); `top-level-integration`'s display name changes to `top-level-integration (our root CMake + upstream/ engine)`; two new step names, `pytest collects tools/secure only` and `upstream submodule pinned and unmodified` (Task 6 docs cite them).

- [ ] **Step 1: Failing precondition — today's workflow cannot build our root**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
grep -c 'submodules: true' "$R/.github/workflows/secure-frontend.yml"
grep -c 'STRATA_BUILD_SECURE_FRONTEND=ON' "$R/.github/workflows/secure-frontend.yml"
```

Expected: `0` (a checkout without the submodule hits the `upstream/ is empty` FATAL_ERROR) and `1` (an option that no longer exists).

- [ ] **Step 2: Write the workflow (whole file)**

```yaml
name: secure-frontend

# Blocking: linux-gcc, linux-clang, windows-llvm-mingw-cross, windows-msys2-clang,
# top-level-integration. Non-blocking: windows-msvc (informational).
# Upstream Strata is the git submodule upstream/ (gitlink 7695859a, tag v0.1.43).
# Jobs that run pytest or configure our root CMakeLists.txt check it out with
# `submodules: true` (depth 1: about a 19.7 MB pack, 50.7 MB and 2,976 files
# checked out); preset-only jobs build secure_frontend/ alone and do not.
# Network fetches, exactly: the GitHub actions used below; the upstream submodule
# (linux, top-level-integration); the pip wheels pinned in
# scripts/dev/requirements-dev.txt (cmake, ninja, regex, pytest and its
# dependencies) via scripts/dev/bootstrap-linux.sh, plus a pip wheel when the
# venv has no pip; MSYS2 packages via msys2/setup-msys2; and the pinned,
# sha256-checked llvm-mingw tarball (scripts/dev/bootstrap-llvm-mingw.sh).
# No models are downloaded and no GPU backend is enabled. Our root seeds
# STRATA_NATIVE_EXPERTS=OFF, so no llama.cpp clone (FetchContent) occurs.
on:
  push:
    paths: ["secure_frontend/**", "CMakeLists.txt", "scripts/**", "pytest.ini", "conftest.py", "tools/**", "upstream", ".gitmodules", ".github/workflows/secure-frontend.yml"]
  pull_request:
    paths: ["secure_frontend/**", "CMakeLists.txt", "scripts/**", "pytest.ini", "conftest.py", "tools/**", "upstream", ".gitmodules", ".github/workflows/secure-frontend.yml"]
  workflow_dispatch:

permissions:
  contents: read

jobs:
  linux:
    name: ${{ matrix.preset }}
    runs-on: ubuntu-latest
    timeout-minutes: 60
    strategy:
      fail-fast: false
      matrix:
        preset: [linux-gcc, linux-clang]
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: true
      - run: scripts/dev/bootstrap-linux.sh
      - run: echo "$GITHUB_WORKSPACE/.venv-dev/bin" >> "$GITHUB_PATH"
      - name: pytest collects tools/secure only
        if: matrix.preset == 'linux-gcc'
        run: |
          collected="$(.venv-dev/bin/pytest -q --collect-only)"
          printf '%s\n' "$collected"
          if printf '%s\n' "$collected" | grep '::' | grep -v '^tools/secure/'; then
            echo "pytest -q collected tests outside tools/secure/" >&2
            exit 1
          fi
      - name: pytest (repo root)
        if: matrix.preset == 'linux-gcc'
        run: .venv-dev/bin/pytest -q
      - run: cmake --preset ${{ matrix.preset }}
        working-directory: secure_frontend
      - run: cmake --build --preset ${{ matrix.preset }} --parallel
        working-directory: secure_frontend
      - run: ctest --preset ${{ matrix.preset }}
        working-directory: secure_frontend
      - name: upstream submodule pinned and unmodified
        run: |
          git submodule status
          if git submodule status | grep -qE '^[-+U]'; then
            echo "upstream/ is not at the recorded gitlink" >&2
            exit 1
          fi
          test -z "$(git -C upstream status --porcelain)"

  windows-cross:
    name: windows-llvm-mingw-cross (compile+link on Linux)
    runs-on: ubuntu-latest
    timeout-minutes: 60
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
          if-no-files-found: error

  windows-msys2:
    name: windows-msys2-clang (runtime tests on Windows)
    runs-on: windows-latest
    timeout-minutes: 60
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
    timeout-minutes: 60
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
    name: top-level-integration (our root CMake + upstream/ engine)
    runs-on: ubuntu-latest
    timeout-minutes: 60
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: true
      - run: scripts/dev/bootstrap-linux.sh
      - run: echo "$GITHUB_WORKSPACE/.venv-dev/bin" >> "$GITHUB_PATH"
      - run: cmake -S . -B out/root -G Ninja -DSTRATA_ENABLE_CUDA=OFF -DSTRATA_ENABLE_HIP=OFF -DSTRATA_NATIVE_EXPERTS=OFF -DSTRATA_BUILD_SECURE_FRONTEND_TESTS=ON
      - run: cmake --build out/root --target strata-secure-server secure_frontend_config_tests strata-plan --parallel
      # --no-tests=error stops a vacuous pass if the frontend tests stop registering.
      - run: ctest --test-dir out/root -R secure_frontend_config_tests --no-tests=error --output-on-failure
      - name: no FetchContent download
        run: test ! -e out/root/_deps
      - name: upstream submodule pinned and unmodified
        run: |
          git submodule status
          if git submodule status | grep -qE '^[-+U]'; then
            echo "upstream/ is not at the recorded gitlink" >&2
            exit 1
          fi
          test -z "$(git -C upstream status --porcelain)"
```

- [ ] **Step 3: Validate the YAML and the job/step shape**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
python3 - "$R/.github/workflows/secure-frontend.yml" <<'EOF'
import sys, yaml
wf = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
jobs = wf["jobs"]
print(sorted(jobs))
for name, job in sorted(jobs.items()):
    checkout = next(s for s in job["steps"] if str(s.get("uses", "")).startswith("actions/checkout"))
    print(name, "submodules" if (checkout.get("with") or {}).get("submodules") else "plain")
print([s.get("name") for s in jobs["top-level-integration"]["steps"] if s.get("name")])
EOF
```

Expected (`python3` here is the system interpreter, which has PyYAML on the dev host; `.venv-dev/bin/python` also has it):
```
['linux', 'top-level-integration', 'windows-cross', 'windows-msvc', 'windows-msys2']
linux submodules
top-level-integration submodules
windows-cross plain
windows-msvc plain
windows-msys2 plain
['no FetchContent download', 'upstream submodule pinned and unmodified']
```

- [ ] **Step 4: Run the CI shell steps locally from the main checkout**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
cd "$R" && collected="$(.venv-dev/bin/pytest -q --collect-only)"; printf '%s\n' "$collected" | grep '::' | grep -v '^tools/secure/'; echo "outside tools/secure: exit $? (1 = none)"
cd "$R" && cmake -S . -B out/root-ci -G Ninja -DSTRATA_ENABLE_CUDA=OFF -DSTRATA_ENABLE_HIP=OFF -DSTRATA_NATIVE_EXPERTS=OFF -DSTRATA_BUILD_SECURE_FRONTEND_TESTS=ON > /dev/null
cd "$R" && cmake --build out/root-ci --target strata-secure-server secure_frontend_config_tests strata-plan --parallel > /dev/null; echo "build exit $?"
cd "$R" && ctest --test-dir out/root-ci -R secure_frontend_config_tests --no-tests=error --output-on-failure 2>&1 | grep 'tests passed'
cd "$R" && test ! -e out/root-ci/_deps && echo "no _deps"
cd "$R" && git submodule status | grep -qE '^[-+U]'; echo "bad prefix: exit $? (1 = none)"; test -z "$(git -C upstream status --porcelain)" && echo "submodule clean"
cmake -E rm -rf "$R/out/root-ci"
```

Expected:
```
outside tools/secure: exit 1 (1 = none)
build exit 0
100% tests passed, 0 tests failed out of 1
no _deps
bad prefix: exit 1 (1 = none)
submodule clean
```

- [ ] **Step 5: Commit**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
TRAILER=$'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\nClaude-Session: <session link>'
git -C "$R" add .github/workflows/secure-frontend.yml
git -C "$R" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "ci: submodule checkout for pytest and the root build; top-level-integration configures our root (U10)" \
  -m "linux and top-level-integration check out upstream/ (depth 1); preset-only jobs keep a plain checkout. top-level-integration builds strata-secure-server, secure_frontend_config_tests and strata-plan from our root and checks no _deps. New steps: pytest collects tools/secure only; upstream submodule pinned and unmodified. Path filters: upstream gitlink, .gitmodules, conftest.py, scripts/** replace requirements.txt." \
  -m "$TRAILER"
```

---

### Task 6: Docs — PROVENANCE, handoff, CI doc, security docs, product spec §9, SP1/SP2 path patches (U11, U12, U13, U14)

**Files:**
- Modify (full rewrite): `docs/PROVENANCE.md`, `docs/secure-frontend-ci.md`
- Modify (listed edits): `docs/AGENT_HANDOFF.md`, `docs/security/privacy-migration.md`, `docs/security/engine-protocol-limits.md:1`, `docs/security/deployment.md:26-29`, `secure_frontend/README.md:71-77`, `docs/superpowers/specs/2026-10-13-product-experience-and-upstream-tracking-design.md` (§9 head, §11 row), `docs/superpowers/plans/2026-10-13-sp1-typed-config-paths-tokens.md:23`, `docs/superpowers/plans/2026-10-13-sp2-portable-supervision.md:29,675,4971,5024,5026,8940`, `docs/superpowers/specs/2026-10-13-sp1-typed-config-paths-tokens-design.md:41,393`, `docs/superpowers/specs/2026-10-13-sp2-portable-supervision-design.md:128,132`

Line numbers are as of `main` `41cbd6d3`; locate each edit by its exact old text (`grep -nF`), not by number.

**Interfaces:**
- Consumes: names from Tasks 1–5 (`upstream/`, `tools/secure/secure_tokenizer.py`, `SECURE_STRATA_BUILD_ENGINE`, CI step names, `out/root`).
- Produces: the "Repository layout" and "Update upstream" sections of `docs/AGENT_HANDOFF.md` (Task 7 appends verification rows to the same file).

- [ ] **Step 1: Failing precondition — docs still describe the fork**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
grep -c -i 'allowlist' "$R/docs/PROVENANCE.md"
grep -rnE '(^|[ `(])(src|serve|include|third_party)/' "$R/docs/AGENT_HANDOFF.md" "$R/docs/security" "$R/docs/secure-frontend-ci.md" | wc -l
```

Expected: `4` and `12` (on `41cbd6d3`: the allowlist text in PROVENANCE, and bare `src/`/`serve/` paths in `engine-protocol-limits.md:1`, `privacy-migration.md:30-33,53-54`, `deployment.md:27`, `secure-frontend-ci.md:20`, `AGENT_HANDOFF.md:61,134,375`).

- [ ] **Step 2: Rewrite `docs/PROVENANCE.md` (whole file)**

```markdown
# Provenance

## Baseline

Upstream [Niko1221/Strata][strata] is the git submodule `upstream/`, pinned by
its gitlink at the annotated release tag `v0.1.43`.

- Submodule (`.gitmodules`): path `upstream`, URL
  `https://github.com/Niko1221/Strata.git`, no branch tracking.
- Pinned commit (the gitlink): `7695859aa13afb248bdb4d36f9b2e95707db079d`
- Tag object: `16374b3e6dfe452a163c3a961ae5faa3b4aec84b`
- Check: `git submodule status` prints
  ` 7695859aa13afb248bdb4d36f9b2e95707db079d upstream (v0.1.43)`.

The gitlink is the baseline record. Nothing under `upstream/` is edited and no
upstream file exists outside it; the repository root holds only our code (see
[`AGENT_HANDOFF.md`](AGENT_HANDOFF.md), "Repository layout"). Where we need
different behavior we wrap upstream from our own tree
(`tools/secure/secure_tokenizer.py`) or offer the change upstream. Exceptions:
none.

Everything under `upstream/` is under upstream's license
([`upstream/LICENSE`](../upstream/LICENSE)). The root [`LICENSE`](../LICENSE)
is the AGPL-3.0; [`NOTICE.md`](../NOTICE.md) keeps Strata's notice with its "Copyright (c) 2026 Niko1221 and the
Strata contributors" line and a pointer to `upstream/LICENSE`.

An upstream update bumps the gitlink and runs the gates
([`AGENT_HANDOFF.md`](AGENT_HANDOFF.md), "Update upstream"); this section's
commit and tag change in the same commit.

History: until 2026-10-13 this repository was a full clone of upstream with our
code mixed in, plus a recorded list of approved edits to upstream files. The
[submodule layout](superpowers/specs/2026-10-13-upstream-submodule-layout-design.md)
replaced it; the three former edits became our root `CMakeLists.txt`, our
`.gitignore`, and the wrapper `tools/secure/secure_tokenizer.py`.

## Security reference

[robertkarl/UnbiasedStrata][unbiased] at
`46c46ecfc11d2315512f21f7549b0a7792ccd2ae` (`main`, resolved 2026-10-11
Pacific time) is a design reference for a smaller, offline-verifiable runtime
and reduced service attack surface.  It is **not** imported as a code
baseline.  Any future adaptation must record the source, license, and exact
revision of copied material, and must preserve Windows and Linux compatibility
requirements.

License: the GitHub API (`gh api repos/robertkarl/UnbiasedStrata/license`,
queried 2026-10-13, default branch) returns SPDX id `MIT`, name `MIT License`.
No code was taken from this repository, so its license governs nothing in
this tree.

## GLM-5.3 compatibility reference

[lighttransport/Strata][lighttransport] branch `glm53f`, resolved at commit
`4894c9967f5754a2052d7b05f10a01b6071bb6e4` on 2026-10-12 Pacific time, is
the requested reference for GLM-5.3 compatibility work. It is not imported as
a code baseline. The `glm4` / `chatglm-bpe` pre-tokenizer regex in our wrapper
`tools/secure/secure_tokenizer.py` (which imports upstream's unmodified
`upstream/tools/strata_tokenizer.py`) was transcribed from that revision's
`tools/strata_tokenizer.py` (which identifies the upstream llama.cpp
`LLAMA_VOCAB_PRE_TYPE_CHATGLM4` behavior); no engine source or model artifact
was copied. It is covered by tiny local byte-BPE regression fixtures
(`tools/secure/test_glm_tokenizer.py`: three-digit numeric boundary, alias
behavior, control-token parsing, UTF-8 round trips) and by
`tools/secure/test_secure_tokenizer.py` (the pattern shipped in a pack's
`tokenizer.json`). This is portable Python code, but its runtime test still
requires the pinned `regex` dependency (`scripts/dev/requirements-dev.txt`).
The GLM prompt template and native engine remain separately reviewed work; the
tokenizer port does not make a model executable or claim complete frontend
tokenizer parity.

License: the GitHub API (`gh api repos/lighttransport/Strata/license`,
queried 2026-10-13, default branch) returns SPDX id `MIT`, name `MIT License`.
The `LICENSE` file on branch `glm53f` (via the contents API, same date) carries
the copyright line "Copyright (c) 2026 Niko1221 and the Strata contributors".

## ggml / llama.cpp (bundled with upstream)

[ggml-org/llama.cpp][llamacpp] / ggml comes with the upstream baseline and is
unchanged by us (it is inside the `upstream/` submodule). It is present in two
forms:

- `upstream/third_party/ggml` — vendored headers;
  `upstream/third_party/ggml/VERSION.txt` records commit
  `3cf03257f219afbe7334045ff7c6a06ac68c627d`.
- The llama.cpp checkout fetched by `upstream/CMakeLists.txt`
  (`FetchContent_Declare(strata_llamacpp ...)`, lines 1255–1258) when
  `STRATA_NATIVE_EXPERTS=ON` and `STRATA_GGML_DIR` is unset, pinned to
  `GIT_TAG 3cf03257f219afbe7334045ff7c6a06ac68c627d`.

License: MIT, "Copyright (c) 2023-2026 The ggml authors"
([`upstream/third_party/ggml/LICENSE`](../upstream/third_party/ggml/LICENSE)).
Our root `CMakeLists.txt` seeds `STRATA_NATIVE_EXPERTS=OFF` and our CI
(`.github/workflows/secure-frontend.yml`) configures with
`-DSTRATA_NATIVE_EXPERTS=OFF`, so the llama.cpp checkout is never fetched by
default or in CI.

## llvm-mingw (build tool only)

[mstorsjo/llvm-mingw][llvmmingw] release `20261006` is the open-source
Windows cross toolchain fetched by `scripts/dev/bootstrap-llvm-mingw.sh`,
verified against sha256
`5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4`. It is a
build tool only; nothing from it is in this tree (it unpacks under the
git-ignored `/.toolchains/`).

License: the GitHub API (`gh api repos/mstorsjo/llvm-mingw/license`, queried
2026-10-13, default branch) returns SPDX id `NOASSERTION`, name `Other`. The
`LICENSE` file it returns states that the llvm-mingw scripts and wrappers are
licensed under the ISC license and that this covers only components provided
by llvm-mingw directly (the bundled LLVM, mingw-w64 and other components keep
their own licenses).

[strata]: https://github.com/Niko1221/Strata/tree/v0.1.43
[unbiased]: https://github.com/robertkarl/UnbiasedStrata/tree/46c46ecfc11d2315512f21f7549b0a7792ccd2ae
[lighttransport]: https://github.com/lighttransport/Strata/tree/glm53f
[llamacpp]: https://github.com/ggml-org/llama.cpp/tree/3cf03257f219afbe7334045ff7c6a06ac68c627d
[llvmmingw]: https://github.com/mstorsjo/llvm-mingw/releases/tag/20261006
```

- [ ] **Step 3: Rewrite `docs/secure-frontend-ci.md` (whole file)**

````markdown
# Secure frontend portability CI

`secure_frontend` is built as an independent CMake project. This is
deliberate: portability checks must not configure the inference engine, select
a CUDA/HIP/SYCL backend, or need a GPU. Upstream Strata (the engine) is the git
submodule `upstream/`; the presets below never read it. Our root
`CMakeLists.txt` builds `secure_frontend/` together with the engine from
`upstream/` (see "Root build").

The checked-in presets (`secure_frontend/CMakePresets.json`):

| Preset | Platform and compiler | Role |
| --- | --- | --- |
| `linux-gcc` | Linux, GCC, Ninja | Blocking CI; primary local preset |
| `linux-clang` | Linux, Clang, Ninja | Blocking CI |
| `windows-llvm-mingw-cross` | Linux host, llvm-mingw cross to Windows x86_64 | Blocking CI (compile + link with `-Werror`); locally, `ctest` runs the `.exe` files under wine as a smoke test only |
| `windows-msys2-clang` | Windows, MSYS2 CLANG64 clang | Blocking CI; the only job that runs the tests on real Windows |
| `windows-msvc` | Windows, Visual Studio 2022/MSVC x64 | Informational, non-blocking (`continue-on-error`) |

The Windows toolchain is open source: llvm-mingw and MSYS2 clang. MSVC is kept
only as an informational job. Caveat: CUDA engine builds on Windows still need
MSVC as the nvcc host compiler; that concerns the engine (`upstream/src/`), not
this frontend.

## Getting the code

    git clone --recurse-submodules https://github.com/shift-f10/multi-strata.git
    # or, after a plain clone:
    git submodule update --init

The presets need no submodule. `pytest -q` and the root build do; both stop
with "run `git submodule update --init`" when `upstream/` is empty.

## Building locally (Linux)

    scripts/dev/bootstrap-linux.sh          # once; creates .venv-dev from scripts/dev/requirements-dev.txt
    export PATH=$PWD/.venv-dev/bin:$PATH
    cd secure_frontend
    cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc

The venv needs no root. The script installs `-r
scripts/dev/requirements-dev.txt` only: cmake 4.4.3, ninja 1.13.2, regex
2026.9.10, pytest 9.1.1 and pytest's dependencies. Upstream's
`upstream/requirements.txt` is not installed. Offline use (`STRATA_WHEEL_DIR`
set) needs every wheel that file names **plus** a pip wheel (`pip-*.whl`); on a
fresh venv the pip bootstrap takes pip from `STRATA_WHEEL_DIR` too, so nothing
is fetched.

Windows cross build from the same Linux host:

    scripts/dev/bootstrap-llvm-mingw.sh     # pinned llvm-mingw 20261006, sha256 checked
    cd secure_frontend
    cmake --preset windows-llvm-mingw-cross
    cmake --build --preset windows-llvm-mingw-cross
    ctest --preset windows-llvm-mingw-cross # under wine; WINEPREFIX=secure_frontend/out/wineprefix

Windows binaries are cross-compiled with llvm-mingw and smoke-tested under
wine on Linux. wine does not implement Job Objects or protected-file DACLs
faithfully; Windows runtime behavior is validated only by the `windows-msys2-clang`
CI job on a real Windows runner. The `windows-msvc` job is informational.

The cross build produces 20 `.exe` files (three executables, seventeen test
binaries) and `ctest` registers 19 entries on Windows against 20 on Linux:
`secure_frontend_token_reader_tests` is the only UNIX-only target, so the Windows
protected-file reader has no runtime test even in the `windows-msys2-clang` job
yet. `secure_frontend_http_socket_server_tests` is portable and runs the Windows
listener in that job. `engine_process_tests` and `engine_stdio_process_tests`
start and terminate through the Job on MSYS2; the grandchild assertion
(`job_object_terminates_grandchild`) runs only there (it also passes under wine,
which is not authoritative for Job Objects). `secure_frontend_http_socket_server_tests`
and `secure_frontend_engine_stdio_process_tests` carry a 300 s ctest `TIMEOUT`, and
every CI job has `timeout-minutes: 60`.

## Root build

    cmake -S . -B out/root -G Ninja
    cmake --build out/root
    ctest --test-dir out/root

Our root `CMakeLists.txt` adds `secure_frontend/` first, then `upstream/`
unmodified. It seeds `STRATA_ENABLE_CUDA=OFF`, `STRATA_ENABLE_HIP=OFF` and
`STRATA_NATIVE_EXPERTS=OFF` as cache defaults (a `-D` overrides them), so the
default build needs no GPU SDK and fetches nothing (no `out/root/_deps`). It
defaults `CMAKE_BUILD_TYPE` to `Release` before upstream sees it and declares
`SECURE_FRONTEND_WERROR=ON`, so `-DSTRATA_WERROR=OFF` does not drop the
frontend's `-Werror`. `-DSECURE_STRATA_BUILD_ENGINE=OFF` builds only the
frontend and works with an empty `upstream/`. From clean: 94 Ninja steps, 0
warnings, `ctest` 20/20 (the frontend's tests; upstream registers none at
`v0.1.43` because `STRATA_BUILD_TESTS` is `OFF`).

## Python tests

`pytest -q` from the repository root runs only `tools/secure/` (see
`pytest.ini`): the GLM tokenizer wrapper tests and the secure distribution
check tests, 12 in all; the owner's pre-push hook runs it. `conftest.py` stops
the run with "run `git submodule update --init`" when `upstream/` is empty.
Upstream's own suites run only after a submodule bump ("Updating upstream").

## Updating upstream

1. `git -C upstream fetch --tags origin`
2. `git -C upstream checkout --detach <tag>`
3. `git add upstream` and update `docs/PROVENANCE.md` "Baseline" and
   `docs/AGENT_HANDOFF.md` "Current baseline" to the new commit and tag.
4. Run the gates: `pytest -q`; `ctest --preset linux-gcc`; the
   `windows-llvm-mingw-cross` build (and wine `ctest`); the root build and root
   `ctest` above.
5. Run upstream's own suites in a separate venv (upstream's pins win there):

       python3 -m venv out/venv-upstream
       out/venv-upstream/bin/pip install -q -r scripts/dev/requirements-dev.txt
       out/venv-upstream/bin/pip install -q -r upstream/requirements.txt
       out/venv-upstream/bin/pytest -q upstream/tools \
         --ignore=upstream/tools/test_iq_pack.py \
         --ignore=upstream/tools/test_setup_engine_hash.py \
         --deselect=upstream/tools/test_setup_choices.py::HipVision::test_the_cpu_encoder_is_built_once \
         --deselect=upstream/tools/test_setup_choices.py::HipVision::test_without_images_nothing_changes \
         --deselect=upstream/tools/test_setup_choices.py::CudaVision::test_the_encoder_is_built_native
       out/venv-upstream/bin/pytest -q upstream/serve   # about 4.5 minutes

   The two ignores need llama.cpp's `gguf` module and GitHub respectively; the
   three deselects are recorded upstream drift against its `setup.py`. Drop an
   exclusion once the new tag no longer needs it.
6. Re-check the SP5 pass-through table against the new engine's flags and
   protocol.
7. `git submodule status` shows no `+`/`-`/`U`; `git -C upstream status
   --short` is empty. One commit: `upstream: bump to <tag>`.

## Offline checkout

An offline box cannot fetch the submodule. The offline kit carries a git bundle
of the pinned upstream commit next to the bundle of our repository:

    git -C upstream bundle create ../out/upstream-v0.1.43-7695859a.bundle v0.1.43
    git bundle verify out/upstream-v0.1.43-7695859a.bundle

On the offline box, after cloning our repository from its bundle:

    git submodule init
    git config submodule.upstream.url /path/to/upstream-v0.1.43-7695859a.bundle
    git -c protocol.file.allow=always submodule update
    git submodule status   # 7695859aa13afb248bdb4d36f9b2e95707db079d upstream (v0.1.43)

The bundle holds tag `v0.1.43` (tag object
`16374b3e6dfe452a163c3a961ae5faa3b4aec84b`, commit
`7695859aa13afb248bdb4d36f9b2e95707db079d`).

## Workflow

`.github/workflows/secure-frontend.yml` defines these jobs:

| Job | Runs on | Submodule | Blocking |
| --- | --- | --- | --- |
| `linux` (matrix `linux-gcc`, `linux-clang`) | ubuntu-latest; `linux-gcc` also checks that `pytest -q` collects `tools/secure/` only and runs it | yes (pytest) | yes |
| `windows-cross` (`windows-llvm-mingw-cross`) | ubuntu-latest; uploads the `.exe` files | no | yes |
| `windows-msys2` (`windows-msys2-clang`) | windows-latest, MSYS2 CLANG64; runs `ctest` | no | yes |
| `windows-msvc` | windows-latest | no | no |
| `top-level-integration` | ubuntu-latest; configures **our** root with CUDA/HIP/native experts off, builds `strata-secure-server`, `secure_frontend_config_tests`, `strata-plan`, runs `ctest -R secure_frontend_config_tests`, checks no `out/root/_deps` | yes | yes |

The jobs that check out the submodule end with the step "upstream submodule
pinned and unmodified": `git submodule status` must show no `+`/`-`/`U` prefix
and `git -C upstream status --porcelain` must be empty. The
`top-level-integration` display name is now "top-level-integration (our root
CMake + upstream/ engine)"; a branch-protection rule that names the old check
("root CMake with STRATA_BUILD_SECURE_FRONTEND=ON") must be updated.

Network fetches are limited to the GitHub actions, the upstream submodule at
depth 1 in `linux` and `top-level-integration` (about a 19.7 MB pack, 50.7 MB
and 2,976 files checked out), the pip wheels pinned in
`scripts/dev/requirements-dev.txt` and, on a fresh venv, pip itself, MSYS2
packages through `msys2/setup-msys2`, and the pinned, sha256-checked
llvm-mingw tarball. No model is downloaded and no GPU backend is enabled. Do
not change the frontend preset jobs to configure the repository root: that
would exercise the upstream engine rather than the isolated,
security-sensitive portable frontend.

Status (2026-10-13): all blocking jobs green on run `38458255140` (branch
`ci/windows-runtime-fixes`), before the submodule layout; the first run after
it is recorded in `AGENT_HANDOFF.md` once the owner pushes. The
`windows-msys2-clang` preset links with `-static`, so a child started with the
allowlisted environment (no `PATH`) needs no MSYS2 runtime DLL. `windows-msvc`
currently fails at configure (the image ships Visual Studio 2026, the preset
names 2022); it is non-blocking.
````

- [ ] **Step 4: `docs/AGENT_HANDOFF.md` edits (each: replace the exact old text)**

H1 — owner decision 6. Replace the whole item that starts `6. **Upstream-trackability.**` and ends `   The SP6 engine is never woven into \`src/\`.` with:

```markdown
6. **Upstream-trackability.** Upstream Strata is the git submodule `upstream/`
   (`https://github.com/Niko1221/Strata.git`), pinned by its gitlink at
   `7695859aa13afb248bdb4d36f9b2e95707db079d` (tag `v0.1.43`) and never
   edited, inside or outside the submodule. The repository root holds only our
   code; our additions to upstream behavior are wrappers that import upstream
   (`tools/secure/secure_tokenizer.py`). The upstream Python service stays in
   the submodule, untouched, and is quarantined at package time
   (`scripts/secure_distribution_check.py` rejects any `upstream` path). An
   upstream update is "Update upstream" below: bump the pointer, run the gates.
   The SP6 engine is never woven into `upstream/src/`.
```

H2 — owner decision 7. Replace the whole item that starts `7. **pytest exclusions to keep visible**` and ends with `\`tools/test_setup_engine_hash.py\` (reaches GitHub; tests must be hermetic).` with:

```markdown
7. **Routine pytest runs our tests only.** `pytest.ini` collects
   `tools/secure/` with no ignores or deselects. Upstream's own suites
   (`pytest upstream/tools` with its recorded exclusions, `pytest
   upstream/serve`) run explicitly after each submodule bump; the exact
   commands are in "Update upstream" below.
```

H3 — owner decision 8. Replace the whole item that starts `8. **Pre-push hook.**` and ends `the full set runs in \`.venv-dev\`.` with:

```markdown
8. **Pre-push hook.** The owner's pre-push hook runs `pytest -q`. With
   `pytest.ini` it collects `tools/secure/` only (12 tests). `conftest.py`
   stops the run with "run `git submodule update --init`" when `upstream/` is
   not checked out.
```

H4 — "Current baseline", replace:

```markdown
- Upstream baseline: `Niko1221/Strata` tag `v0.1.43`, checked-out commit
  `7695859aa13afb248bdb4d36f9b2e95707db079d`.
- Git remotes: `upstream` points at `https://github.com/Niko1221/Strata.git`;
  `origin` is the owner's repository. Only the owner pushes.
```

with:

```markdown
- Upstream baseline: the `upstream` submodule gitlink, `Niko1221/Strata` tag
  `v0.1.43`, commit `7695859aa13afb248bdb4d36f9b2e95707db079d`
  (`git submodule status`).
- Git remotes: `origin` is the owner's repository. Only the owner pushes. The
  older `upstream` remote of this repository is no longer used for tracking;
  the submodule has its own `origin`.
```

H5 — insert this new section immediately before the line `## What has changed`:

````markdown
## Repository layout

Upstream Strata is the git submodule `upstream/`; the repository root holds
only our code. Deleting `upstream/` leaves exactly our code.

| Ours, at the root | Pristine, under `upstream/` (submodule @ `7695859a`, tag `v0.1.43`) |
| --- | --- |
| `CMakeLists.txt` (builds `secure_frontend/` and, by default, the engine from `upstream/`) | `upstream/CMakeLists.txt`, `upstream/src/`, `upstream/include/`, `upstream/cmake/`, `upstream/third_party/`, `upstream/sycl/` |
| `secure_frontend/` (own `CMakePresets.json`; builds without `upstream/`) | `upstream/serve/`, `upstream/setup.py`, `upstream/setup.sh`, `upstream/requirements.txt`, `upstream/chat.py`, the launchers |
| `tools/secure/` (GLM tokenizer wrapper and our pytest suite) | `upstream/tools/` (incl. `strata_tokenizer.py`, `test_setup_*.py`) |
| `scripts/`, `tests/security/`, `docs/` (this file, `PROVENANCE.md`, `secure-frontend-ci.md`, `security/`, `superpowers/`) | `upstream/docs/`, `upstream/tests/`, `upstream/bench/`, `upstream/data/`, `upstream/ref/` |
| `.github/workflows/secure-frontend.yml`, `.gitignore`, `.gitattributes`, `.gitmodules`, `pytest.ini`, `conftest.py`, `LICENSE`, `README.md` | `upstream/.github/`, `upstream/LICENSE`, `upstream/README*.md`, `upstream/AGENTS.md`, `upstream/SECURITY.md`, `upstream/Dockerfile` |

Getting the code: `git clone --recurse-submodules`, or `git submodule update
--init` after a plain clone. Inside a `.worktrees/<name>` worktree run `git
submodule update --init` once as well. The presets need no submodule;
`pytest -q` and the root build do, and both say so when it is missing.

Root build (GPU backends off, nothing fetched): `cmake -S . -B out/root -G
Ninja && cmake --build out/root && ctest --test-dir out/root`.
`-DSECURE_STRATA_BUILD_ENGINE=OFF` builds the frontend alone. Details:
[`secure-frontend-ci.md`](secure-frontend-ci.md), "Root build".

### Update upstream

1. `git -C upstream fetch --tags origin`, then `git -C upstream checkout
   --detach <tag>`, then `git add upstream`.
2. Update `PROVENANCE.md` "Baseline" and "Current baseline" above to the new
   commit and tag.
3. Gates: `pytest -q`; `ctest --preset linux-gcc`; the
   `windows-llvm-mingw-cross` build and wine `ctest`; the root build and root
   `ctest`.
4. Upstream's own suites in `out/venv-upstream` (exact commands in
   [`secure-frontend-ci.md`](secure-frontend-ci.md), "Updating upstream"):
   `pytest upstream/tools` with `--ignore` of `test_iq_pack.py` (needs
   llama.cpp's `gguf` module) and `test_setup_engine_hash.py` (reaches GitHub),
   and `--deselect` of the three recorded `test_setup_choices.py` drift cases;
   then `pytest upstream/serve`.
5. Re-check the SP5 pass-through table against the new engine's flags and
   protocol.
6. `git submodule status` shows no `+`/`-`/`U`; `git -C upstream status
   --short` is empty. One commit: `upstream: bump to <tag>`.

````

H6 — insert this new subsection immediately after the line `## What has changed` (before `### Secure baseline (commit \`7de35b0c\` on \`main\`)`):

```markdown
### Submodule layout (branch `layout/upstream-submodule`)

Spec: [`superpowers/specs/2026-10-13-upstream-submodule-layout-design.md`](superpowers/specs/2026-10-13-upstream-submodule-layout-design.md);
plan: [`superpowers/plans/2026-10-13-upstream-submodule-layout.md`](superpowers/plans/2026-10-13-upstream-submodule-layout.md).

| Item | What changed | Check |
| --- | --- | --- |
| U2 | `upstream/` = submodule `https://github.com/Niko1221/Strata.git` at `7695859a` (`v0.1.43`), tree byte-identical to the tag | `git submodule status` |
| U3 | Our root `CMakeLists.txt`: frontend first, then `upstream/` unmodified; Release default, `include(CTest)`, `SECURE_FRONTEND_WERROR=ON`, CUDA/HIP/native experts seeded `OFF`, `SECURE_STRATA_BUILD_ENGINE` | root build 94 steps / 0 warnings, `ctest` 20/20, no `_deps`; `-D` overrides; empty `upstream/` message |
| U4, U5 | `tools/secure/secure_tokenizer.py` wraps upstream's tokenizer (GLM aliases, `extract()` ships the declared pattern); tests in `tools/secure/`; `pytest.ini` `testpaths = tools/secure`; `conftest.py` names `git submodule update --init` | `pytest -q` 12 passed |
| U1, U6, U7 | 2 971 upstream files removed from our tree; ours-only `.gitignore`, `.gitattributes`, `README.md`, `LICENSE` | `git ls-files` shows only our paths plus the `upstream` gitlink |
| U8 | `bootstrap-linux.sh` reads only `scripts/dev/requirements-dev.txt` (now pins cmake 4.4.3, ninja 1.13.2, regex 2026.9.10) | fresh venv bootstraps; `pytest -q` passes without numpy |
| U9 | `secure_distribution_check.py` rejects any `upstream` path component and quarantined names at any depth | `tools/secure/test_secure_distribution_check.py` (6 tests) |
| U10 | CI: submodule checkout in `linux` and `top-level-integration`; the latter configures our root; collection and submodule-status steps | first green run after the owner pushes: pending |
| U11–U14 | `PROVENANCE.md` baseline = gitlink, no ledger; this file, `secure-frontend-ci.md`, security docs, product spec §9, SP1/SP2 path patches; "Update upstream" runbook; offline bundle in `secure-frontend-ci.md` | doc greps in the plan, Task 6 |
```

H7 — in the "Secure baseline" table replace these three cells/rows exactly:

- `Complete baseline/attribution record. SP0 added the upstream-touch allowlist and the record of every touch to an upstream-owned file.` → `Complete baseline/attribution record. Since the submodule layout the baseline is the \`upstream\` gitlink and there are no upstream edits.`
- `| GLM tokenizer tooling | \`tools/strata_tokenizer.py\`, \`tools/test_glm_tokenizer.py\` |` → `| GLM tokenizer tooling | \`tools/secure/secure_tokenizer.py\` (wraps \`upstream/tools/strata_tokenizer.py\`), \`tools/secure/test_glm_tokenizer.py\` |`
- `| Top-level integration | \`CMakeLists.txt\` | Builds \`secure_frontend\` through \`STRATA_BUILD_SECURE_FRONTEND=ON\` (default \`OFF\` since SP0). |` → `| Top-level integration | \`CMakeLists.txt\` (ours since the submodule layout) | Builds \`secure_frontend/\` and the engine from \`upstream/\`; \`SECURE_STRATA_BUILD_ENGINE=OFF\` builds the frontend alone. Before the layout it was an opt-in hunk in upstream's root file. |`

H8 — in the SP0 table, row "Tooling (T1, T5)", replace `with \`-r requirements.txt -r scripts/dev/requirements-dev.txt\`: cmake/ninja pinned in \`requirements.txt\`; pytest 9.1.1 and its dependencies pinned in \`scripts/dev/requirements-dev.txt\`.` with `with \`-r scripts/dev/requirements-dev.txt\` (since the submodule layout the only file it reads: cmake 4.4.3, ninja 1.13.2, regex, pytest 9.1.1 and its dependencies).`; in the same row replace `needs every wheel both files name` with `needs every wheel that file names`, and `\`pytest.ini\` collects \`tools/test_*.py\` only, with the exclusions listed above.` with `\`pytest.ini\` collects \`tools/secure/\` only (submodule layout).`

H9 — in the SP0 table, row "POSIX launch (P1–P3)", replace `\`src/posix_child_launch.hpp\`` with `\`secure_frontend/src/posix_child_launch.hpp\``.

H10 — Phase 5 item 5, replace:

```markdown
   The repository source tree intentionally fails this gate because it retains
   quarantined upstream files for provenance; do not weaken the gate to make a
   source archive pass. **SP7.**
```

with:

```markdown
   A source checkout fails this gate by design: it rejects any path under an
   `upstream` directory (the submodule) and the quarantined names at any depth;
   do not weaken the gate to make a source archive pass. **SP7.**
```

H11 — "Verification performed": replace the paragraph start `Run on the dev host (Linux x86_64, no GPU) on 2026-10-13, after the SP0 final fix` with:

```markdown
### SP0 (before the submodule layout)

Run on the dev host (Linux x86_64, no GPU) on 2026-10-13, after the SP0 final fix
```

and replace:

```markdown
- `pytest serve` (the upstream service suite) was not run for SP0; nothing in
  `serve/` changed.
```

with:

```markdown
- `pytest upstream/serve` (the upstream service suite; `pytest serve` before
  the submodule layout) was not run for SP0; nothing in `upstream/serve/`
  changed.
```

and replace:

```markdown
The Python GLM tokenizer module also runs under `pytest -q` above
(`tools/test_glm_tokenizer.py`, with the pinned `regex` from the venv). The two
skips are hardware/platform skips in upstream tests
(`tools/test_dll_closure.py`: HIP zip absent; `tools/test_setup_oldcpu.py`:
Windows only). Run `tools/test_glm_tokenizer.py` again in the release Python
environment before accepting a GLM tokenizer release.
```

with:

```markdown
The Python GLM tokenizer tests also ran under `pytest -q` above (then
`tools/test_glm_tokenizer.py`; now `tools/secure/test_glm_tokenizer.py`, with
the pinned `regex` from the venv). The two skips were hardware/platform skips
in upstream tests (`upstream/tools/test_dll_closure.py`: HIP zip absent;
`upstream/tools/test_setup_oldcpu.py`: Windows only). Run
`tools/secure/test_glm_tokenizer.py` and `tools/secure/test_secure_tokenizer.py`
again in the release Python environment before accepting a GLM tokenizer
release.
```

H12 — "Collaboration and change rules", replace:

```markdown
- Do not modify upstream engine/model code while developing the frontend unless
  a compatibility defect proves it necessary; record every such change in
  `docs/PROVENANCE.md`. Upstream-owned files outside the allowlist in the owner
  decisions are not touched at all.
```

with:

```markdown
- Never edit anything under `upstream/`. A compatibility need is met by a
  wrapper in our tree or by an upstream pull request; record it in
  `docs/PROVENANCE.md`. `git -C upstream status --short` stays empty.
```

and replace:

```markdown
- Gates before every hand-off and after every upstream merge: `pytest -q`;
  `ctest --preset linux-gcc`; the `windows-llvm-mingw-cross` build; after an
  upstream merge also `pytest serve`.
```

with:

```markdown
- Gates before every hand-off and after every submodule bump: `pytest -q`;
  `ctest --preset linux-gcc`; the `windows-llvm-mingw-cross` build; the root
  build and root `ctest`; after a bump also `pytest upstream/tools` and
  `pytest upstream/serve` ("Update upstream").
```

- [ ] **Step 5: Security docs, frontend README, product spec, SP1/SP2 patches (each: exact old → new)**

`docs/security/privacy-migration.md`:
- `| \`START-HERE.bat\` |` → `| \`upstream/START-HERE.bat\` |`
- `| \`setup.py\` |` → `| \`upstream/setup.py\` |`
- `| \`update.sh\`, \`UPDATE.bat\` |` → `| \`upstream/update.sh\`, \`upstream/UPDATE.bat\` |`
- `| \`serve/server.py\` image handling |` → `| \`upstream/serve/server.py\` image handling |`
- `| \`serve/mcp.py\` |` → `| \`upstream/serve/mcp.py\` |`
- `| \`serve/telemetry.py\`, \`serve/prometheus.py\` |` → `| \`upstream/serve/telemetry.py\`, \`upstream/serve/prometheus.py\` |`
- `| \`serve/web/\` |` → `| \`upstream/serve/web/\` |`
- `` `START-HERE.bat`, `setup.py`, `UPDATE.bat`, `update.sh`, `serve/server.py`, and`` → `` `upstream/START-HERE.bat`, `upstream/setup.py`, `upstream/UPDATE.bat`, `upstream/update.sh`, `upstream/serve/server.py`, and``
- `` `serve/mcp.py`. Future agents must update this inventory when adding or`` → `` `upstream/serve/mcp.py` (the baseline is the `upstream/` submodule). Future agents must update this inventory when adding or``

`docs/security/engine-protocol-limits.md` line 1: `# Engine protocol limits (Strata v0.1.43, \`src/program/generate.cpp\`)` → `# Engine protocol limits (Strata v0.1.43, \`upstream/src/program/generate.cpp\`)`

`docs/security/deployment.md`, replace:

```markdown
The normal mode rejects legacy service and installer/update artifacts,
including `serve/`, `sycl/serve/`, `setup.py`, `START-HERE.bat`, update
scripts, `requirements.txt`, MCP artifacts, and Python source. It also rejects
```

with:

```markdown
The normal mode rejects legacy service and installer/update artifacts at any
depth of the staging tree: a `serve` or `sycl/serve` directory, `setup.py`,
`START-HERE.bat`, update scripts, `requirements.txt`, MCP artifacts, Python
source, and anything under an `upstream` directory. It also rejects
```

`secure_frontend/README.md`, replace:

```markdown
the tool environment once (no root needed). It installs `-r requirements.txt
-r scripts/dev/requirements-dev.txt` into `.venv-dev`: cmake/ninja are pinned in
`requirements.txt`; pytest and its dependencies are pinned in
`scripts/dev/requirements-dev.txt`. Offline use (`STRATA_WHEEL_DIR` set) needs
every wheel those two files name **plus** a pip wheel (`pip-*.whl`): on a fresh
venv the pip bootstrap takes pip from `STRATA_WHEEL_DIR` as well.
```

with:

```markdown
the tool environment once (no root needed). It installs `-r
scripts/dev/requirements-dev.txt` into `.venv-dev`: cmake, ninja, regex, pytest
and pytest's dependencies are pinned there. Offline use (`STRATA_WHEEL_DIR`
set) needs every wheel that file names **plus** a pip wheel (`pip-*.whl`): on a
fresh venv the pip bootstrap takes pip from `STRATA_WHEEL_DIR` as well.
```

`docs/superpowers/specs/2026-10-13-product-experience-and-upstream-tracking-design.md`:
- Insert after the line `## 9. Upstream tracking` (with a blank line on each side):

```markdown
> **Superseded (2026-10-13)** by
> [`2026-10-13-upstream-submodule-layout-design.md`](2026-10-13-upstream-submodule-layout-design.md):
> upstream is the pinned, unmodified submodule `upstream/`. The approved-edit
> list, `scripts/dev/upstream-delta.sh`, the `upstream-delta` CI job, the merge
> runbook, the ledger and the tokenizer exception below no longer apply; an
> upstream update is a submodule bump plus the gates (`docs/AGENT_HANDOFF.md`,
> "Update upstream"). The GLM-engine and SP5-parity bullets still hold, with
> `src/` read as `upstream/src/`.
```

- §11 row: `| Tooling (any SP, recommend SP1) | \`scripts/dev/upstream-delta.sh\` + CI job; tokenizer exception move |` → `| Tooling (any SP, recommend SP1) | Done by the upstream-submodule layout: no delta script needed; tokenizer aliases moved to \`tools/secure/secure_tokenizer.py\` |`

`docs/superpowers/plans/2026-10-13-sp1-typed-config-paths-tokens.md` line 23, replace the whole line:

```markdown
- No change under the repository-root `src/` and `include/` (the engine), `serve/`, `setup.py` or the root `CMakeLists.txt` (`secure_frontend/src` and `secure_frontend/include` are this plan's own tree); upstream-owned files only from the `docs/PROVENANCE.md` allowlist. SP1 touches only `secure_frontend/`, `docs/`, `scripts/dev/`. If a step seems to need anything else: stop and report. No `rm -rf`. Install nothing outside `.venv-dev/` and `.toolchains/`.
```

with:

```markdown
- No change under `upstream/` (the submodule) or our root `CMakeLists.txt` (`secure_frontend/src` and `secure_frontend/include` are this plan's own tree). SP1 touches only `secure_frontend/`, `docs/`, `scripts/dev/`. If a step seems to need anything else: stop and report. No `rm -rf`. Install nothing outside `.venv-dev/` and `.toolchains/`.
```

`docs/superpowers/plans/2026-10-13-sp2-portable-supervision.md`:
- line 29, replace the whole line `- **Off limits:** no change under the repository-root \`src/\`, \`include/\`, \`serve/\`, \`setup.py\`, or the root \`CMakeLists.txt\` (\`secure_frontend/src\` and \`secure_frontend/include\` are the frontend and are in scope). Stop and report to the owner before any such change, before any second-user test outside CI, before any GPU run, and before any \`/health\` change beyond adding \`recovery\`.` with `- **Off limits:** no change under \`upstream/\` (the submodule) or our root \`CMakeLists.txt\` (\`secure_frontend/src\` and \`secure_frontend/include\` are the frontend and are in scope). Stop and report to the owner before any such change, before any second-user test outside CI, before any GPU run, and before any \`/health\` change beyond adding \`recovery\`.`
- line 675: `// The engine's fatal verification lines (upstream serve/server.py FATAL_PREFIXES).` → `// The engine's fatal verification lines (upstream/serve/server.py FATAL_PREFIXES).`
- line 4971: `exactly upstream \`serve/server.py:587-588\`` → `exactly \`upstream/serve/server.py:587-588\``
- line 5024: `// The engine's fatal verification lines (upstream serve/server.py:587-588` → `// The engine's fatal verification lines (upstream/serve/server.py:587-588`
- line 5026: `// a trailing DONE is never accepted (upstream _pump, server.py:775-791).` → `// a trailing DONE is never accepted (upstream _pump, upstream/serve/server.py:775-791).`
- line 8940: `Run (repo root): \`.venv-dev/bin/pytest -q | tail -2\` — unchanged pass count (nothing under \`serve/\` or \`tools/\` changed).` → `Run (repo root): \`.venv-dev/bin/pytest -q | tail -2\` — unchanged pass count (\`tools/secure\` only).`

`docs/superpowers/specs/2026-10-13-sp1-typed-config-paths-tokens-design.md`:
- line 41: delete the line `- Upstream-owned files are touched only from the \`PROVENANCE.md\` allowlist.`
- line 393 (now 392 after the deletion): `  task touches \`src/\`, an upstream-owned file outside the allowlist, or` → `  task touches anything under \`upstream/\` (the submodule) or our root \`CMakeLists.txt\`, or`

`docs/superpowers/specs/2026-10-13-sp2-portable-supervision-design.md`:
- line 128: `(\`serve/server.py:587-588\`, research §4)` → `(\`upstream/serve/server.py:587-588\`, research §4)`
- line 132: `(\`_pump\`, \`server.py:775-791\`)` → `(\`_pump\`, \`upstream/serve/server.py:775-791\`)`

- [ ] **Step 6: Verify the docs (U11, U12, U14)**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
grep -n -i 'allowlist\|touch' "$R/docs/PROVENANCE.md"; echo "provenance rule text: exit $? (1 = none)"
grep -rnE '(^|[ `(])(src|serve|include|third_party)/' "$R/docs/AGENT_HANDOFF.md" "$R/docs/security" "$R/docs/secure-frontend-ci.md"; echo "bare upstream paths: exit $? (1 = none)"
grep -n -i 'upstream-owned\|upstream-touch\|pre-allowlist\|allowlist is defined\|outside the allowlist\|git merge upstream' "$R/docs/AGENT_HANDOFF.md"; echo "handoff allowlist rule text: exit $? (1 = none)"
grep -c 'PROVENANCE.md` allowlist\|PROVENANCE.md\` allowlist' "$R/docs/superpowers/plans/2026-10-13-sp1-typed-config-paths-tokens.md" "$R/docs/superpowers/specs/2026-10-13-sp1-typed-config-paths-tokens-design.md"
grep -n 'serve/server.py' "$R/docs/superpowers/plans/2026-10-13-sp2-portable-supervision.md" "$R/docs/superpowers/specs/2026-10-13-sp2-portable-supervision-design.md" | grep -v 'upstream/serve/server.py'; echo "bare serve/server.py: exit $? (1 = none)"
grep -c '^> \*\*Superseded (2026-10-13)\*\*' "$R/docs/superpowers/specs/2026-10-13-product-experience-and-upstream-tracking-design.md"
for f in docs/PROVENANCE.md docs/AGENT_HANDOFF.md docs/secure-frontend-ci.md; do grep -oE '\]\([^)#]+' "$R/$f" | sed 's/^](//' | grep -vE '^https?://' | while read -r p; do test -e "$(dirname "$R/$f")/$p" || echo "MISSING in $f: $p"; done; done; echo "link check done"
```

Expected: `provenance rule text: exit 1 (1 = none)`; `bare upstream paths: exit 1 (1 = none)`; `handoff allowlist rule text: exit 1 (1 = none)` (the remaining "allowlist" words in the handoff are about the child environment and the Host header, not upstream files); `…sp1-typed-config-paths-tokens.md:0` and `…design.md:0`; `bare serve/server.py: exit 1 (1 = none)`; `1`; then only `link check done`.

- [ ] **Step 7: Exercise the offline-bundle procedure (U14) in a scratch clone**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
git -C "$R/upstream" bundle create "$R/out/upstream-v0.1.43-7695859a.bundle" v0.1.43
git bundle verify "$R/out/upstream-v0.1.43-7695859a.bundle" 2>&1 | tail -1
git bundle list-heads "$R/out/upstream-v0.1.43-7695859a.bundle"
S="$(mktemp -d)"
git clone -q --branch layout/upstream-submodule "$R" "$S/c"
git -C "$S/c" submodule init
git -C "$S/c" config submodule.upstream.url "$R/out/upstream-v0.1.43-7695859a.bundle"
git -C "$S/c" -c protocol.file.allow=always submodule update 2>&1 | tail -1
git -C "$S/c" submodule status
cmake -E rm -rf "$S"; cmake -E rm -f "$R/out/upstream-v0.1.43-7695859a.bundle"
```

Expected: `…/out/upstream-v0.1.43-7695859a.bundle is okay`; `16374b3e6dfe452a163c3a961ae5faa3b4aec84b refs/tags/v0.1.43`; `Submodule path 'upstream': checked out '7695859aa13afb248bdb4d36f9b2e95707db079d'`; ` 7695859aa13afb248bdb4d36f9b2e95707db079d upstream (v0.1.43)`. (The scratch clone uses the commits already made on the branch; it needs no network.)

- [ ] **Step 8: Gates unaffected**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
cd "$R" && pytest -q 2>&1 | tail -1
git -C "$R" status --short
```

Expected: `12 passed in …s`; `git status --short` lists exactly the modified docs (` M docs/AGENT_HANDOFF.md`, ` M docs/PROVENANCE.md`, ` M docs/secure-frontend-ci.md`, ` M docs/security/deployment.md`, ` M docs/security/engine-protocol-limits.md`, ` M docs/security/privacy-migration.md`, ` M docs/superpowers/plans/2026-10-13-sp1-typed-config-paths-tokens.md`, ` M docs/superpowers/plans/2026-10-13-sp2-portable-supervision.md`, ` M docs/superpowers/specs/2026-10-13-product-experience-and-upstream-tracking-design.md`, ` M docs/superpowers/specs/2026-10-13-sp1-typed-config-paths-tokens-design.md`, ` M docs/superpowers/specs/2026-10-13-sp2-portable-supervision-design.md`, ` M secure_frontend/README.md`).

- [ ] **Step 9: Commit**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
TRAILER=$'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\nClaude-Session: <session link>'
git -C "$R" add docs secure_frontend/README.md
git -C "$R" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "docs: submodule layout — PROVENANCE baseline = gitlink, handoff layout + Update upstream, CI doc, path patches (U11-U14)" \
  -m "PROVENANCE: baseline is the upstream gitlink; no ledger; GLM section names tools/secure/secure_tokenizer.py; ggml links under upstream/. AGENT_HANDOFF: decisions 6-8, current baseline, Repository layout, Update upstream runbook, What has changed. secure-frontend-ci.md: getting the code, root build, Python tests, updating upstream, offline bundle, jobs. Security docs and secure_frontend/README.md paths. Product spec section 9 superseded note. SP1/SP2 plan and spec lines: upstream/ instead of root src/include/serve/setup.py, allowlist lines removed." \
  -m "$TRAILER"
```

---

### Task 7: Full verification, record it, fast-forward `main`, recreate the SP1/SP2 worktrees

**Files:**
- Modify: `docs/AGENT_HANDOFF.md` ("Verification performed": new subsection)
- Worktrees: `.worktrees/sp1`, `.worktrees/sp2` (deleted and recreated; branches `sp1-typed-config-paths-tokens`, `sp2-portable-supervision` recreated from the new `main`)

**Interfaces:**
- Consumes: everything from Tasks 1–6.
- Produces: `main` fast-forwarded to `layout/upstream-submodule` (local only); clean worktrees at `.worktrees/sp1` and `.worktrees/sp2` on fresh branches from `main`, with `upstream/` initialized and the `.venv-dev`/`.toolchains` symlinks.

- [ ] **Step 1: Only our paths are tracked (the `git ls-files` dry run)**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
git -C "$R" ls-files | sed 's#/.*##' | LC_ALL=C sort -u | tr '\n' ' '; echo
git -C "$R" ls-files | grep -v '^upstream$' | sed 's#/.*##' | LC_ALL=C sort -u | tr '\n' ' '; echo
git -C "$R" ls-files -s upstream
comm -12 <(git -C "$R" ls-files | LC_ALL=C sort) <(git -C "$R" ls-tree -r --name-only 7695859aa13afb248bdb4d36f9b2e95707db079d | LC_ALL=C sort) | tr '\n' ' '; echo
```

Expected:
```
.gitattributes .github .gitignore .gitmodules CMakeLists.txt LICENSE README.md conftest.py docs pytest.ini scripts secure_frontend tests tools upstream
.gitattributes .github .gitignore .gitmodules CMakeLists.txt LICENSE README.md conftest.py docs pytest.ini scripts secure_frontend tests tools
160000 7695859aa13afb248bdb4d36f9b2e95707db079d 0	upstream
.gitattributes .gitignore CMakeLists.txt LICENSE README.md
```
(The last line: the only paths we share with upstream's `v0.1.43` tree are the five root files whose content is now ours.)

- [ ] **Step 2: Linux preset 20/20**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
cd "$R/secure_frontend" && cmake --preset linux-gcc > /dev/null && cmake --build --preset linux-gcc 2>&1 | tail -1 && ctest --preset linux-gcc 2>&1 | grep 'tests passed'
```

Expected: last build line `ninja: no work to do.` or a `[n/n]` link line; `100% tests passed, 0 tests failed out of 20`.

- [ ] **Step 3: Windows cross with `-Werror`, then wine 19/19**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
cd "$R/secure_frontend" && cmake --preset windows-llvm-mingw-cross > /dev/null && cmake --build --preset windows-llvm-mingw-cross > "$R/out/cross-build.log" 2>&1; echo "cross exit $?"
grep -c -- '-Werror' "$R/secure_frontend/out/build/windows-llvm-mingw-cross/build.ninja" | awk '{print ($1>0) ? "-Werror present" : "NO -Werror"}'
ls "$R/secure_frontend/out/build/windows-llvm-mingw-cross/"*.exe | wc -l
cd "$R/secure_frontend" && ctest --preset windows-llvm-mingw-cross 2>&1 | grep 'tests passed'
```

Expected: `cross exit 0`; `-Werror present`; `20`; `100% tests passed, 0 tests failed out of 19`.

- [ ] **Step 4: Root build from a fresh scratch clone (Review Focus 1 end to end)**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
S="$(mktemp -d)"
git clone -q --branch layout/upstream-submodule "$R" "$S/c"
ls -A "$S/c/upstream" | wc -l
cmake -S "$S/c" -B "$S/c/out/root" -G Ninja > "$S/empty.log" 2>&1; echo "plain clone configure exit $?"
grep -o 'run `git submodule update --init`' "$S/empty.log"
(cd "$S/c" && pytest -q > "$S/pytest-empty.log" 2>&1; echo "plain clone pytest exit $?")
git -C "$S/c" submodule update --init --depth 1 2>&1 | tail -1
cmake -S "$S/c" -B "$S/c/out/root" -G Ninja > /dev/null 2>&1 && cmake --build "$S/c/out/root" > "$S/build.log" 2>&1; echo "root build exit $?"
grep -m1 -o '^\[1/[0-9]*\]' "$S/build.log"; grep -c 'warning:' "$S/build.log"
ctest --test-dir "$S/c/out/root" 2>&1 | grep 'tests passed'
test ! -e "$S/c/out/root/_deps" && echo "no _deps"
(cd "$S/c" && pytest -q 2>&1 | tail -1)
test -z "$(git -C "$S/c/upstream" status --porcelain)" && echo "scratch submodule clean"
git -C "$S/c" rm -r -q upstream && ls -A "$S/c" | grep -vxE 'out|\.pytest_cache|__pycache__' | LC_ALL=C sort | tr '\n' ' '; echo
cmake -E rm -rf "$S"
```

Expected:
```
0
plain clone configure exit 1
run `git submodule update --init`
plain clone pytest exit 4
Submodule path 'upstream': checked out '7695859aa13afb248bdb4d36f9b2e95707db079d'
root build exit 0
[1/94]
0
100% tests passed, 0 tests failed out of 20
no _deps
12 passed in …s
scratch submodule clean
.git .gitattributes .github .gitignore .gitmodules CMakeLists.txt LICENSE README.md conftest.py docs pytest.ini scripts secure_frontend tests tools
```
(The `--depth 1` init fetches from GitHub, as CI does; if this git refuses the shallow fetch of a non-tip commit, rerun the init without `--depth 1` and note it. The last line is acceptance 6 of the spec: removing `upstream/` leaves exactly our code; the filter drops only the ignored `out/`, `.pytest_cache/` and `__pycache__/` this run created.)

- [ ] **Step 5: Root build in the main checkout from clean, and routine pytest**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
cmake -E rm -rf "$R/out/root"
cmake -S "$R" -B "$R/out/root" -G Ninja > /dev/null && cmake --build "$R/out/root" > "$R/out/root-build.log" 2>&1; echo "build exit $?"
grep -m1 -o '^\[1/[0-9]*\]' "$R/out/root-build.log"; grep -c 'warning:' "$R/out/root-build.log"
find "$R/out/root" -type f \( -name strata-plan -o -name strata-gguf -o -name strata-dequant -o -name strata-secure-server \) -printf '%f\n' | sort
ctest --test-dir "$R/out/root" 2>&1 | grep 'tests passed'
test ! -e "$R/out/root/_deps" && echo "no _deps"
cd "$R" && pytest -q 2>&1 | tail -1
```

Expected: `build exit 0`; `[1/94]`; `0`; `strata-dequant`, `strata-gguf`, `strata-plan`, `strata-secure-server`; `100% tests passed, 0 tests failed out of 20`; `no _deps`; `12 passed in …s`.

- [ ] **Step 6: Upstream's own suites from the submodule (the first "bump")**

The existing `.venv-dev` still holds upstream's former requirements (numpy etc.), so it runs them as before the restructure:

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)" && export PATH="$R/.venv-dev/bin:$PATH"
cd "$R" && pytest -q upstream/tools \
  --ignore=upstream/tools/test_iq_pack.py \
  --ignore=upstream/tools/test_setup_engine_hash.py \
  --deselect=upstream/tools/test_setup_choices.py::HipVision::test_the_cpu_encoder_is_built_once \
  --deselect=upstream/tools/test_setup_choices.py::HipVision::test_without_images_nothing_changes \
  --deselect=upstream/tools/test_setup_choices.py::CudaVision::test_the_encoder_is_built_native 2>&1 | tail -1
cd "$R" && pytest -q upstream/serve 2>&1 | tail -1; echo "serve exit ${PIPESTATUS[0]}"
```

Expected: `616 passed, 2 skipped, 3 deselected, 214 subtests passed in …s` (the SP0 count of 620 minus the 4 GLM tests that now live in `tools/secure/`); `pytest upstream/serve` ends with `… passed …` and `serve exit 0` (about 4.5 minutes). If either count or exit differs, stop and report the output; do not edit anything under `upstream/`.

- [ ] **Step 7: Clean tree check**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
git -C "$R" status --short
git -C "$R/upstream" status --porcelain
git -C "$R" submodule status
```

Expected: no output from the first two; ` 7695859aa13afb248bdb4d36f9b2e95707db079d upstream (v0.1.43)`.

- [ ] **Step 8: Record the verification in `docs/AGENT_HANDOFF.md` and commit**

Insert immediately after the line `## Verification performed` (before `### SP0 (before the submodule layout)` added in Task 6):

```markdown
### Submodule layout (2026-10-13, branch `layout/upstream-submodule`)

Dev host (Linux x86_64, no GPU), `PATH` with `.venv-dev/bin` first.

| Command (repository root unless noted) | Result |
| --- | --- |
| `git submodule status` | ` 7695859aa13afb248bdb4d36f9b2e95707db079d upstream (v0.1.43)`; `git -C upstream status --short` empty after every gate below |
| `git ls-files \| sed 's#/.*##' \| sort -u` | `.gitattributes .github .gitignore .gitmodules CMakeLists.txt LICENSE README.md conftest.py docs pytest.ini scripts secure_frontend tests tools upstream` |
| `pytest -q` | `12 passed` (`tools/secure/` only); a fresh venv from `bootstrap-linux.sh` (no numpy) passes too; with `upstream/` empty it exits 4 with "run `git submodule update --init`" |
| in `secure_frontend/`: `ctest --preset linux-gcc` | `100% tests passed out of 20` |
| in `secure_frontend/`: `windows-llvm-mingw-cross` build, then `ctest --preset windows-llvm-mingw-cross` (wine) | 20 `.exe` with `-Werror`; `100% tests passed out of 19` |
| `cmake -S . -B out/root -G Ninja && cmake --build out/root && ctest --test-dir out/root` (from clean) | 94 steps, 0 warnings; `strata-plan`, `strata-gguf`, `strata-dequant`, `strata-secure-server` built; `100% tests passed out of 20`; no `out/root/_deps` |
| root configure with `-DCMAKE_BUILD_TYPE=Debug -DSTRATA_WERROR=OFF` / `-DSTRATA_ENABLE_CUDA=ON` / empty `upstream/` / `-DSECURE_STRATA_BUILD_ENGINE=OFF` | `Debug` kept and `SECURE_FRONTEND_WERROR=ON`; fails at `CMAKE_CUDA_COMPILER` (override reached upstream); fails with "run `git submodule update --init`"; configures and builds the frontend |
| fresh scratch clone (`git clone`, then `git submodule update --init --depth 1`) | plain clone: configure and pytest stop with the init instruction; after init: root build 94 steps, ctest 20/20, pytest 12 passed; `git rm -r upstream && ls -A` shows only our paths |
| `pytest upstream/tools` (with the recorded `--ignore`/`--deselect`) | `616 passed, 2 skipped, 3 deselected, 214 subtests passed` |
| `pytest upstream/serve` | exit 0 |
| offline bundle (`git -C upstream bundle create … v0.1.43`, init a scratch clone from it) | bundle okay; `refs/tags/v0.1.43` → submodule checked out at `7695859a` with no network |

Not shown here: CI on GitHub (first run after the owner pushes; record its run id
in the U10 row above), `linux-clang`, `windows-msys2-clang`, `windows-msvc`.
```

If any observed value in Steps 1–7 differs from this table, write the observed value instead and say so in the commit body. Then:

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
TRAILER=$'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\nClaude-Session: <session link>'
git -C "$R" add docs/AGENT_HANDOFF.md
git -C "$R" -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit \
  -m "docs: record the submodule-layout verification" \
  -m "linux-gcc 20/20; cross 20 .exe with -Werror; wine 19/19; root build 94 steps, 0 warnings, ctest 20/20, no _deps; pytest -q 12 passed; scratch clone end to end; pytest upstream/tools 616 passed; pytest upstream/serve exit 0; offline bundle checked." \
  -m "$TRAILER"
git -C "$R" status --short
```

Expected: `git status --short` prints nothing.

- [ ] **Step 9: Fast-forward `main` to the branch (local only, never pushed)**

Run only after Steps 1–8 passed.

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
git -C "$R" switch main
git -C "$R" merge --ff-only layout/upstream-submodule | tail -1
git -C "$R" submodule status
test "$(git -C "$R" rev-parse main)" = "$(git -C "$R" rev-parse layout/upstream-submodule)" && echo "main = layout/upstream-submodule"
git -C "$R" status --short
```

Expected: `Switched to branch 'main'`; a fast-forward summary line; ` 7695859aa13afb248bdb4d36f9b2e95707db079d upstream (v0.1.43)`; `main = layout/upstream-submodule`; no status output. If `--ff-only` refuses, `main` moved since Task 1: stop and report (do not merge or rebase).

- [ ] **Step 10: Remove the old worktrees and their branches**

The old worktree branches sit at `7ddcd755`, an ancestor of `main`, so deleting them loses no commit. `git worktree remove` needs a clean worktree, and each old worktree holds two untracked symlinks (`.venv-dev`, `.toolchains`, untracked because the old `.gitignore` patterns had a trailing slash); remove the symlinks first (plain `rm` of a symlink never touches its target).

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
for b in sp1-typed-config-paths-tokens sp2-portable-supervision; do git -C "$R" merge-base --is-ancestor "$b" main && echo "$b is in main"; done
git -C "$R/.worktrees/sp1" status --short; git -C "$R/.worktrees/sp2" status --short
for w in sp1 sp2; do test -L "$R/.worktrees/$w/.venv-dev" && rm "$R/.worktrees/$w/.venv-dev"; test -L "$R/.worktrees/$w/.toolchains" && rm "$R/.worktrees/$w/.toolchains"; done
git -C "$R/.worktrees/sp1" status --short; git -C "$R/.worktrees/sp2" status --short; echo "old worktrees clean"
git -C "$R" worktree remove "$R/.worktrees/sp1"
git -C "$R" worktree remove "$R/.worktrees/sp2"
git -C "$R" branch -D sp1-typed-config-paths-tokens sp2-portable-supervision
git -C "$R" worktree list
```

Expected: `sp1-typed-config-paths-tokens is in main`, `sp2-portable-supervision is in main`; the first status calls print `?? .toolchains` and `?? .venv-dev` for each; then only `old worktrees clean`; `Deleted branch sp1-typed-config-paths-tokens (was 7ddcd755).`, `Deleted branch sp2-portable-supervision (was 7ddcd755).`; `git worktree list` shows only `/opt/projects/ai  <sha> [main]`. If a status shows anything besides those two symlinks, stop and report.

- [ ] **Step 11: Recreate both worktrees from the new `main`, with the submodule and symlinks**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
git -C "$R" worktree add -b sp1-typed-config-paths-tokens "$R/.worktrees/sp1" main
git -C "$R" worktree add -b sp2-portable-supervision "$R/.worktrees/sp2" main
for w in sp1 sp2; do
  ln -s "$R/.venv-dev" "$R/.worktrees/$w/.venv-dev"
  ln -s "$R/.toolchains" "$R/.worktrees/$w/.toolchains"
  git -C "$R/.worktrees/$w" submodule update --init 2>&1 | tail -1
done
for w in sp1 sp2; do
  echo "== $w"
  git -C "$R/.worktrees/$w" branch --show-current
  test "$(git -C "$R/.worktrees/$w" rev-parse HEAD)" = "$(git -C "$R" rev-parse main)" && echo "at main"
  git -C "$R/.worktrees/$w" submodule status
  git -C "$R/.worktrees/$w" status --short
  (cd "$R/.worktrees/$w" && .venv-dev/bin/pytest -q 2>&1 | tail -1)
done
git -C "$R" worktree list
```

Expected: `Preparing worktree (new branch 'sp1-typed-config-paths-tokens')` / `…'sp2-portable-supervision'`, `HEAD is now at <main sha> docs: record the submodule-layout verification`; each `submodule update --init` ends with `Submodule path 'upstream': checked out '7695859aa13afb248bdb4d36f9b2e95707db079d'` (a network clone per worktree); then for each worktree:
```
== sp1
sp1-typed-config-paths-tokens
at main
 7695859aa13afb248bdb4d36f9b2e95707db079d upstream (v0.1.43)
12 passed in …s
== sp2
sp2-portable-supervision
at main
 7695859aa13afb248bdb4d36f9b2e95707db079d upstream (v0.1.43)
12 passed in …s
```
with no `git status --short` output (the symlinks are ignored by `/.venv-dev` and `/.toolchains`), and `git worktree list` showing the main checkout plus `.worktrees/sp1 [sp1-typed-config-paths-tokens]` and `.worktrees/sp2 [sp2-portable-supervision]`. Note for later: a worktree that holds an initialized submodule can only be removed with `git worktree remove --force`.

- [ ] **Step 12: Final report (no commit; nothing to push)**

```bash
cd /opt/projects/ai && R="$(git rev-parse --show-toplevel)"
git -C "$R" log --oneline main -8
git -C "$R" status --short
```

Expected: the seven commits of this plan (Tasks 1–6 plus the Task 7 verification commit) on top of the controller's commit that added this plan, and an empty status. Report both to the controller; the owner pushes.

---

## Self-review

**Spec coverage.**
- U1 (remove upstream files) → Task 4 Steps 2–4 (192 pathspecs, 2 971 files), checked in Task 4 Step 9 and Task 7 Steps 1, 4.
- U2 (submodule) → Task 1; `git submodule status` and tree-identity check.
- U3 (root CMake) → Task 2 (file verbatim from spec §4.3 plus comments); 94 steps, 0 warnings, 20/20, `_deps` absent, frontend-only with empty `upstream/`.
- U4 (wrapper) → Task 3 Steps 5–8; U5 (pytest.ini/conftest) → Task 3 Steps 9, 11.
- U6 (`.gitignore`/`.gitattributes`) → Task 4 Steps 5–6, clean status in Task 4 Step 9 and Task 7 Step 7.
- U7 (README/LICENSE per §4.5 and decision 1) → Task 4 Steps 7–9 (link check, the AGPL text, the Niko1221 line in NOTICE.md).
- U8 (bootstrap/dev pins) → Task 3 Steps 10, 12 (existing and fresh venv).
- U9 (distribution check depth) → Task 3 Steps 1–4 (hermetic tests in `tools/secure/`, so routine pytest runs them).
- U10 (CI) → Task 5; the first green GitHub run is outside this plan (the owner pushes), recorded as pending.
- U11 (PROVENANCE) → Task 6 Step 2, grep in Step 6.
- U12 (handoff, path prefixes, CI doc, product spec note) → Task 6 Steps 3–6.
- U13 (runbook) → Task 6 (handoff "Update upstream", CI doc "Updating upstream"), exercised once in Task 7 Step 6.
- U14 (offline kit) → Task 6 Step 3 ("Offline checkout") and Step 7 (exercised).
- Spec §6 "Impact on SP1/SP2" → Task 6 Step 5 (every listed line, located by text since the SP2 plan's numbers moved after the spec was written: L671→675, L4967→4971, L5020–5022→5024–5026, L8838→8940), and Task 7 Steps 10–11 (worktrees re-created with the submodule initialized).
- Spec §8 acceptance 1–4, 6–7 → Task 7 Steps 1–7; acceptance 5 (CI green) needs the owner' push.

**Placeholder scan.** No TBD/TODO; every code step has full content; `<tag>` appears only inside the runbook text being written (a parameter of the procedure). Commit trailers are concrete (with a note to use the executor harness's own lines if they differ).

**Name consistency.** `secure_tokenizer` exports (`GLM4_PATTERN`, `QWEN35_PATTERN`, `GLM4_PRE_NAMES`, `bytes_to_unicode`, `pre_tokenizer_pattern`, `Tokenizer`, `extract`, `main`) match their uses in both test modules; `check.inspect(root, False)` matches `inspect(package_root, require_release_evidence)`; CMake option names (`SECURE_STRATA_BUILD_ENGINE`, `SECURE_FRONTEND_WERROR`, `STRATA_BUILD_SECURE_FRONTEND_TESTS`) and the FATAL_ERROR text match across Tasks 2, 5, 6, 7; CI step names match the docs; the test count 12 = 4 + 2 + 6 everywhere.

**Review Focus.** Each of the five lines has its test in the owning task: 1 → Task 2 Step 7, Task 3 Step 11, Task 7 Step 4; 2 → Task 3 Steps 1–4; 3 → Task 3 Step 12, Task 5 CI step and Step 4, Task 7 Step 7; 4 → Task 2 Step 6; 5 → Task 3 Steps 5–8.
