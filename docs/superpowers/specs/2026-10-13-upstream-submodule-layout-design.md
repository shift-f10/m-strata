# Upstream as a submodule — repository layout

Date: 2026-10-13. Status: **APPROVED by the owner 2026-10-13 21:05 UTC** (single root LICENSE with both notices; upstream tests run explicitly after pointer bumps only; no second submodule until SP6).
Parent effort: Secure Strata full release path, see `docs/AGENT_HANDOFF.md`.
Supersedes, once approved, the "Upstream tracking" mechanics in
`2026-10-13-product-experience-and-upstream-tracking-design.md` §9 (allowlist,
`upstream-delta.sh`, merge runbook).

## 1. Purpose

Today this repository is a fork: upstream Niko1221/Strata `v0.1.43` and our
code share one tree, and every upstream file we touch is tracked through an
allowlist in `docs/PROVENANCE.md`. This change makes the split structural:

1. Upstream lives, pristine, in a git submodule at `upstream/`, pinned at tag
   `v0.1.43` (commit `7695859aa13afb248bdb4d36f9b2e95707db079d`).
2. The repository root holds only our code. Deleting `upstream/` leaves exactly
   our code.
3. An upstream update is "bump the submodule pointer, run the gates". No
   merges, no allowlist, no touch table, no delta script.

It changes no behavior of `secure_frontend/` and no upstream byte.

## 2. Constraints

- Owner decision (binding): submodule at `upstream/`, URL
  `https://github.com/Niko1221/Strata.git`, pinned at `7695859a`.
- Upstream files are never edited, inside or outside the submodule. Our
  additions to upstream behavior are wrappers that import upstream.
- `secure_frontend/` and its `CMakePresets.json` stay where they are and work
  unchanged (in-flight SP1/SP2 plans edit only that tree).
- No GPU SDK and no network fetch in the default root build
  (`STRATA_NATIVE_EXPERTS=OFF`, so no llama.cpp FetchContent).
- Nothing is pushed by agents; commits are local; the owner pushes.
- `docs/AGENT_HANDOFF.md` and `docs/PROVENANCE.md` change in the same commit
  series, not after.

## 3. Spike result (2026-10-13, scratch only)

A scratch root (`CMakeLists.txt` of §4.3, a copy of `secure_frontend/`,
`upstream/` = a worktree of `7695859a`) was configured with Ninja and
`-DSTRATA_ENABLE_CUDA=OFF -DSTRATA_ENABLE_HIP=OFF -DSTRATA_NATIVE_EXPERTS=OFF`:

- Configure: clean, no warnings, no FetchContent, `_deps/` absent.
- `strata-secure-server` + `secure_frontend_config_tests` built;
  `ctest -R ^secure_frontend_config` → `100% tests passed out of 1`.
- Full build (`all`, 94 Ninja steps from clean, 0 warnings): upstream's engine targets
  (`strata-plan`, `strata-gguf`, `strata-dequant`, `strata_spec`,
  `strata_kernels_cpu`) are exactly the set a standalone configure of
  `7695859a` produces; root `ctest` → `100% tests passed out of 20`.
- `-DSTRATA_WERROR=ON` reaches upstream's targets (94/94 built).
- The exact §4.3 file: same results, `SECURE_FRONTEND_WERROR=ON`,
  `CMAKE_BUILD_TYPE=Release`; `-DSECURE_STRATA_BUILD_ENGINE=OFF` configures
  without `upstream/`; an empty `upstream/` stops with the
  `git submodule update --init` message.
- `secure_frontend/` presets unchanged: `linux-gcc` 20/20;
  `windows-llvm-mingw-cross` builds 20 `.exe` with `-Werror`; wine 19/19.
- `pytest -q` with `testpaths = tools/secure` and the §4.4 wrapper: 4 passed
  (spike: GLM tests only; with the U4 extract and U9 tests the routine count
  is 12, 13 after the final fix wave's `extract()` guard test);
  the submodule tree stayed clean (`git status` empty).

Verdict: upstream's root `CMakeLists.txt` works as a subdirectory without
edits. Its top-level assumptions and how our root handles each:

| Upstream site (v0.1.43 `CMakeLists.txt`) | Effect as a subdirectory | Our handling |
| --- | --- | --- |
| L10–11 `cmake_minimum_required(3.24)`, `project(strata VERSION 0.1.43)` | Works; `PROJECT_VERSION` feeds `STRATA_VERSION` in its directory only | Root requires 3.24 too |
| L54–56 `CMAKE_BUILD_TYPE` forced to `Release` in the cache when empty | Leaks to the whole build (incl. the frontend) | Root sets its own default before `add_subdirectory` |
| L51–53, L23, L26–29 `CMAKE_CXX_STANDARD`, `add_compile_definitions/options` | Directory-scoped; do not reach `secure_frontend/` | None needed |
| L96–99 `STRATA_BUILD_TESTS` default from `EXISTS tests/CMakeLists.txt` | OFF at `v0.1.43` (file absent) → no Catch2 fetch | None; documented |
| L101 `option(STRATA_WERROR OFF)` | `secure_frontend/CMakeLists.txt` defaults `SECURE_FRONTEND_WERROR` to `STRATA_WERROR` when defined: upstream first → frontend loses `-Werror` (spike-verified `OFF`) | Root declares `SECURE_FRONTEND_WERROR ON` and adds `secure_frontend` first |
| L104, 497, 524, 532, 606, 1636 `enable_testing()` in its own dir | Root `ctest` sees them only if the root enables testing | Root `include(CTest)` |
| L1210 `option(STRATA_NATIVE_EXPERTS ON)` → L1254 FetchContent llama.cpp | Network fetch by default | Root pre-seeds the cache `OFF` |
| L1252, L1263 `add_subdirectory(.../ggml ${CMAKE_BINARY_DIR}/ggml)` | Binary dir lands at `<build>/ggml` instead of `<build>/upstream/ggml`; works | None (only with native experts) |
| L1219–1226 `BUILD_SHARED_LIBS`, `GGML_*` `CACHE … FORCE` | Global cache; frontend libraries are static anyway | None |
| `sycl/` (L20, `return()`), its L164/L175 `CMAKE_BINARY_DIR` | Only with `STRATA_ENABLE_SYCL`; `return()` leaves upstream's dir only | Out of scope |
| `tools/vision/CMakeLists.txt` L61 `CMAKE_BINARY_DIR/bin` | Separate project built by `setup.py`, not added by the root | None |
| `install()` rules, `include(CTest)`, `CMAKE_SOURCE_DIR`, `PROJECT_SOURCE_DIR`, `CMAKE_CURRENT_LIST_DIR` | None present | — |

No `ExternalProject` or second configure is needed.

## 4. Decisions

### 4.1 Submodule at `upstream/`, pinned by gitlink

The gitlink is the baseline record. `.gitmodules` names
`https://github.com/Niko1221/Strata.git`, path `upstream`, no branch tracking.

Rejected: keep the fork + allowlist (every upstream merge risks our hunks and
needs a ledger); `git subtree` into `upstream/` (copies upstream history and
bytes into ours, merges stay); `ExternalProject`/second configure (the spike
shows `add_subdirectory` works, and a second configure hides upstream targets
from the root `ctest`); a vendored tarball (loses the provable commit link).

### 4.2 Our root `CMakeLists.txt` owns the build

Upstream's root `CMakeLists.txt` is used unmodified as a subdirectory. Our
root holds the options that used to be our hunk at the end of upstream's file.

Rejected: `add_subdirectory(upstream/<x>)` for a part of the engine (upstream
has one monolithic root; its targets are defined there).

### 4.3 Root `CMakeLists.txt` (as spiked)

```cmake
cmake_minimum_required(VERSION 3.24)
project(secure_strata LANGUAGES CXX)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  # FORCE: project() already made an empty cache entry (Task 2 review fix)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
include(CTest)
option(SECURE_STRATA_BUILD_ENGINE "Configure upstream's engine from upstream/" ON)
option(STRATA_BUILD_SECURE_FRONTEND_TESTS "Build the portable frontend tests" ${BUILD_TESTING})
option(SECURE_FRONTEND_WERROR "Treat warnings as errors in the secure frontend" ON)
# Upstream's defaults that would need a GPU SDK or the network; -D overrides.
set(STRATA_ENABLE_CUDA OFF CACHE BOOL "Build the CUDA targets")
set(STRATA_ENABLE_HIP OFF CACHE BOOL "Build the HIP targets")
set(STRATA_NATIVE_EXPERTS OFF CACHE BOOL "Native (IQ) experts through ggml-cpu (fetches llama.cpp)")
add_subdirectory(secure_frontend)
if(SECURE_STRATA_BUILD_ENGINE)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/upstream/CMakeLists.txt")
    message(FATAL_ERROR "upstream/ is empty: run `git submodule update --init`")
  endif()
  add_subdirectory(upstream)
endif()
```

`STRATA_BUILD_SECURE_FRONTEND` disappears: the frontend is ours and always
built from our root. `SECURE_STRATA_BUILD_ENGINE=OFF` gives a frontend-only
root build without the submodule.

### 4.4 GLM tokenizer: wrapper, not edit

`tools/secure/secure_tokenizer.py` puts `upstream/tools` on `sys.path`,
imports upstream's `strata_tokenizer`, and adds `GLM4_PATTERN`,
`pre_tokenizer_pattern()`, a `Tokenizer` subclass that recompiles `self._re`
from the declared `pre`, and an `extract()` that calls upstream's and rewrites
`pre_pattern`/`pre_pattern_source` in `tokenizer.json`. Import failure names
`git submodule update --init`. The test moves to
`tools/secure/test_glm_tokenizer.py`; the only change is the import:

```python
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from secure_tokenizer import (GLM4_PATTERN, QWEN35_PATTERN, Tokenizer,
                              bytes_to_unicode, pre_tokenizer_pattern)
```

Rejected: a package shim `tools/__init__.py` re-exporting
`upstream.tools.strata_tokenizer` (upstream's module does its own
`sys.path.insert` for `gguf_reader`, so it must be importable as a top-level
module anyway).

### 4.5 LICENSE and README

- `LICENSE`: this repository's license, the GNU AGPL-3.0 (the FSF's text).
  `NOTICE.md` keeps Strata's notice with the retained "Copyright (c) 2026
  Niko1221 and the Strata contributors" line, and says "`upstream/` is a git
  submodule of Niko1221/Strata under its own `upstream/LICENSE`." Reason: our
  root still carries material derived from Strata-family code (the GLM regex
  transcribed from lighttransport/Strata, whose `LICENSE` carries the Niko1221
  line), whose notice travels with it.
  Rejected: our notice only (drops an attribution we still owe).
- `README.md`: the current attribution header (credits table and "What is
  different here") with links rewritten to `upstream/...`, then a short pointer
  "Upstream Strata's README: `upstream/README.md`". The six translated READMEs
  leave with upstream. (Final fix wave: GitHub does not serve paths inside a
  submodule from the parent repository, so links into `upstream/` became
  pinned absolute URLs such as
  `https://github.com/Niko1221/Strata/blob/v0.1.43/README.md`.)

## 5. Target tree

| Ours, at the root | Pristine, under `upstream/` (submodule @ `7695859a`) |
| --- | --- |
| `.github/workflows/secure-frontend.yml` | `.github/` (FUNDING, ISSUE_TEMPLATE, PR template) |
| `.gitignore` (ours), `.gitattributes` (ours: `*.sh text eol=lf`), `.gitmodules` | `.gitignore`, `.gitattributes`, `.dockerignore` |
| `CMakeLists.txt` (ours, §4.3) | `CMakeLists.txt`, `cmake/`, `src/`, `include/`, `sycl/`, `third_party/` |
| `LICENSE` (§4.5), `README.md` (§4.5) | `LICENSE`, `README.md`, `README.{de,es,fr,ja,pt-BR,zh-CN}.md`, `SECURITY.md`, `AGENTS.md` |
| `secure_frontend/` (incl. `CMakePresets.json`, unchanged) | `serve/`, `setup.py`, `setup.sh`, `requirements.txt`, `chat.py` |
| `scripts/dev/`, `scripts/secure_distribution_check.py` | `START-HERE.bat`, `SETUP.bat`, `UPDATE.bat`, `BENCH.bat`, `update.sh`, `bench.sh` |
| `tools/secure/` (wrapper + test) | `tools/` (incl. `strata_tokenizer.py`, `test_setup_*.py`) |
| `tests/security/` | `tests/` (`core`, `cuda`, `data`, `hip`, `platform`, `prefill`, `program`) |
| `docs/AGENT_HANDOFF.md`, `docs/PROVENANCE.md`, `docs/secure-frontend-ci.md`, `docs/security/`, `docs/superpowers/` | `docs/` (all 35 upstream `*.md`, `benchmarks/`, `media/`, `monitoring/`, `paper/`) |
| `pytest.ini`, `conftest.py` | `bench/`, `data/`, `ref/`, `Dockerfile`, `docker-entrypoint.sh` |

`git rm -r --cached upstream && rm -rf upstream && ls -A` then shows only:
`.git .gitattributes .github .gitignore .gitmodules CMakeLists.txt LICENSE
README.md conftest.py docs pytest.ini scripts secure_frontend tests tools`
(plus git-ignored `.venv-dev/`, `.toolchains/`, `.worktrees/`).

## 6. Scope

One item per commit; each lands with its verification.

| Item | Change | Verification |
| --- | --- | --- |
| U1 | Remove upstream files from our tree: `git rm -r` of `.dockerignore AGENTS.md BENCH.bat Dockerfile README.de.md README.es.md README.fr.md README.ja.md README.pt-BR.md README.zh-CN.md SECURITY.md SETUP.bat START-HERE.bat UPDATE.bat bench.sh bench chat.py cmake data docker-entrypoint.sh include ref requirements.txt serve setup.py setup.sh src sycl third_party update.sh .github/FUNDING.yml .github/ISSUE_TEMPLATE .github/pull_request_template.md`, every `tools/*` except our `test_glm_tokenizer.py` (moved in U4), `tests/{core,cuda,data,hip,platform,prefill,program}`, and the 35 upstream `docs/*.md` plus `docs/{benchmarks,media,monitoring,paper}`. Root `CMakeLists.txt`, `.gitignore`, `.gitattributes`, `LICENSE`, `README.md` are rewritten in U3/U6/U7, not deleted. U1 and U2 are one commit (the tree is never without upstream). | `git diff --name-status 7695859a HEAD` after U1–U2 lists only our paths plus the `upstream` gitlink. |
| U2 | `git submodule add https://github.com/Niko1221/Strata.git upstream`; `git -C upstream checkout 7695859a`; commit gitlink + `.gitmodules`. | `git submodule status` → ` 7695859aa13afb248bdb4d36f9b2e95707db079d upstream (v0.1.43)`; `git -C upstream status --short` empty. |
| U3 | Our root `CMakeLists.txt` (§4.3). The upstream root hunk (`STRATA_BUILD_SECURE_FRONTEND*`) is gone with upstream's file. | Root configure+build with GPU off: `strata-plan`, `strata-gguf`, `strata-dequant`, `strata-secure-server` built; root `ctest` 20/20; `-DSECURE_STRATA_BUILD_ENGINE=OFF` configures with `upstream/` empty. |
| U4 | `tools/secure/secure_tokenizer.py` + `tools/secure/test_glm_tokenizer.py` (§4.4). Upstream `tools/strata_tokenizer.py` is back to pristine (it is in the submodule). | `pytest -q` → 12 passed (4 GLM + 2 extract + 6 U9 distribution-check; 13 after the final fix wave, which adds the `extract()` missing-file guard test); `git -C upstream status --short` empty after the run. |
| U5 | `pytest.ini`: `testpaths = tools/secure`, `python_files = test_*.py`, all `--ignore`/`--deselect` lines dropped (they named upstream tests). `conftest.py`: numpy `collect_ignore` dropped; replaced by a check that fails collection with "run `git submodule update --init`" when `upstream/tools/strata_tokenizer.py` is absent. | `pytest -q` from the root passes; with `upstream/` deinitialized, the message is shown. |
| U6 | `.gitignore` ours: `/.venv-dev/`, `/.toolchains/`, `/.worktrees/`, `/secure_frontend/out/`, `/secure_frontend/build/`, `/out/`, `__pycache__/`, `.pytest_cache/`. `.gitattributes` ours: `*.sh text eol=lf`. | `git status --short` clean after a full build, ctest, pytest and the cross build. |
| U7 | `README.md` and `LICENSE` per §4.5. | Every link in the README header resolves in the tree (`upstream/...` paths included). |
| U8 | `scripts/dev/bootstrap-linux.sh` stops reading upstream's `requirements.txt`; `scripts/dev/requirements-dev.txt` adds the pins it used from it: `cmake==4.4.3`, `ninja==1.13.2`, `regex==2026.9.10`. | Fresh `.venv-dev` from the script (and from `STRATA_WHEEL_DIR`) runs `pytest -q` and the presets. |
| U9 | `scripts/secure_distribution_check.py`: docstring says the source checkout holds upstream as a submodule; also reject any path with an `upstream` component and match `FORBIDDEN_PATHS` on any path suffix, not only at the root. Spike finding: a staging dir holding `upstream/requirements.txt` and `upstream/setup.sh` passes today. | New hermetic test under `tests/security/` (or `tools/secure/`): nested `upstream/requirements.txt` and `upstream/serve/index.html` fail; a stage with only `bin/strata-secure-server` passes. |
| U10 | CI: `actions/checkout@v4` with `submodules: true` in `linux` (pytest needs upstream), `top-level-integration`, and any job that configures the root; preset-only jobs (`windows-cross`, `windows-msys2`, `windows-msvc`) keep plain checkout. `top-level-integration` configures **our** root: `cmake -S . -B out/root -G Ninja -DSTRATA_ENABLE_CUDA=OFF -DSTRATA_ENABLE_HIP=OFF -DSTRATA_NATIVE_EXPERTS=OFF -DSTRATA_BUILD_SECURE_FRONTEND_TESTS=ON`, builds `strata-secure-server secure_frontend_config_tests strata-plan`, runs `ctest --test-dir out/root -R secure_frontend_config_tests --no-tests=error`. New step: `git submodule status` must show no `+`/`-`/`U` prefix. Path filters: `upstream` (gitlink) replaces `requirements.txt`; `tools/**` is kept (a superset: our `tools/secure/` tests live there). Header comment: network fetches add the submodule at depth 1 (≈19.7 MB pack, 50.7 MB, 2 976 files checked out; measured with `git pack-objects` on `7695859a`). | First green run after the owner pushes, recorded in the handoff. |
| U11 | `docs/PROVENANCE.md` rewritten: Baseline = the `upstream` submodule gitlink (`7695859a`, tag `v0.1.43`); "Upstream-owned files touched" section and touch table deleted; remaining exceptions: none. References/credits sections kept; links to `third_party/ggml/LICENSE` become `upstream/third_party/ggml/LICENSE`; GLM section names `tools/secure/secure_tokenizer.py`. | `grep -n 'allowlist\|touch' docs/PROVENANCE.md` returns no rule text. |
| U12 | Docs: `AGENT_HANDOFF.md` rule 6 and rule 7 rewritten (no allowlist; pytest exclusions gone; runbook = U13); paths citing upstream (`src/`, `serve/`, `tools/`, `include/`) prefixed `upstream/` in `AGENT_HANDOFF.md` (12 lines), `PROVENANCE.md` (7), `security/privacy-migration.md` (6), `secure-frontend-ci.md` (3), `security/engine-protocol-limits.md` (1), `README.md` (2); `secure-frontend-ci.md` documents `git submodule update --init` and the new integration job; product spec §9 gets a "superseded by" note. | `grep -rnE '(^|[ \`(])(src|serve|include|third_party)/' docs/AGENT_HANDOFF.md docs/security docs/secure-frontend-ci.md` finds only `upstream/…` or `secure_frontend/…` paths. |
| U13 | Runbook "update upstream": `git -C upstream fetch --tags origin`; `git -C upstream checkout <tag>`; `git add upstream`; run the gates (§8 items 1–5) plus `pytest upstream/tools` (with the former ignores: `test_iq_pack.py`, `test_setup_engine_hash.py`, the three `test_setup_choices.py` deselects) and `pytest upstream/serve`; re-check the SP5 pass-through table; one commit "upstream: bump to <tag>". | Written in `AGENT_HANDOFF.md` and `secure-frontend-ci.md`. |
| U14 | Offline kit: the kit vendors the submodule (a `git bundle` of the pinned commit or the checked-out `upstream/` tree with its commit id), since an offline box cannot fetch it. | Offline-kit doc names the bundle and its commit. |

### What happens to today's three upstream-file edits

| Edit | After |
| --- | --- |
| Root `CMakeLists.txt` hunk (`STRATA_BUILD_SECURE_FRONTEND*`) | Gone; its options live in our root (§4.3) |
| `.gitignore` appends | Our own `.gitignore` (U6) |
| `tools/strata_tokenizer.py` GLM aliases (35 lines) | `tools/secure/secure_tokenizer.py` wrapper (U4) |

The README header (the approved exception) becomes our whole README (U7).

### Impact on the in-flight SP1/SP2 plans

They edit `secure_frontend/` and its presets only, so their code tasks rebase
without conflict. Lines whose wording must change:

- SP1 plan L23, SP2 plan L29: "no change under the repository-root `src/`,
  `include/`, `serve/`, `setup.py` or the root `CMakeLists.txt`" → "no change
  under `upstream/` (the submodule) or our root `CMakeLists.txt`"; "upstream-
  owned files only from the `docs/PROVENANCE.md` allowlist" → delete.
- SP1 spec L41 ("touched only from the `PROVENANCE.md` allowlist") → delete.
- SP2 plan L8838: "unchanged pass count (nothing under `serve/` or `tools/`
  changed)" → "unchanged pass count (`tools/secure` only)".
- SP2 plan citations of `serve/server.py:587-588` and `_pump` (L671, L4967,
  L5020–5022) → `upstream/serve/server.py`; comment-only.
- SP1 plan L11010 (`pytest -q` at the top level) and the
  `top-level-integration` mentions (SP1 spec L407, SP2 spec L430) stay valid;
  the job's command changes in U10 only.
- SP1 plan L58/L199/L643 `CMakeLists.txt` mean `secure_frontend/CMakeLists.txt`;
  unaffected.
- The `.worktrees/sp1` and `.worktrees/sp2` checkouts need
  `git submodule update --init` once they rebase onto the restructure (pytest
  and the root build need `upstream/`; the presets do not).

## 7. Out of scope

Any upstream edit; `lighttransport/Strata` as a second submodule (SP6, see Q3);
SYCL, CUDA, HIP or native-expert builds from our root (they work through the
same `-D` switches but are not gated); our own `SECURITY.md`/`AGENTS.md`;
rewriting history to drop upstream objects from our repository; the
upstream-delta script (made unnecessary).

## 8. Execution and acceptance

Execution: one branch (`upstream-submodule`), items in order U1+U2, U3–U14,
one local commit each; the orchestrator reruns the gates before accepting
each item; the owner pushes. Best landed between SP1 and SP2 merges, or after
both, so neither plan rebases across a half-done tree.

Acceptance:

1. `ctest --preset linux-gcc` → 20/20 (or the then-current count).
2. `windows-llvm-mingw-cross` builds every `.exe` with `-Werror`; wine
   `ctest --preset windows-llvm-mingw-cross` → 19/19 (or then-current).
3. `pytest -q` at the root → our tests only, green.
4. Root configure+build with `-DSTRATA_ENABLE_CUDA=OFF -DSTRATA_ENABLE_HIP=OFF
   -DSTRATA_NATIVE_EXPERTS=OFF` builds the engine targets and the frontend;
   root `ctest` green; no `_deps/` directory.
5. CI green after the owner pushes (all blocking jobs incl. the new
   `top-level-integration` and the submodule-status step).
6. In a scratch clone: `git rm -r -q upstream && ls -A` shows only the paths
   listed in §5; `git -C upstream status --short` is empty in the real
   checkout after all gates.
7. `docs/PROVENANCE.md` and `docs/AGENT_HANDOFF.md` describe the submodule
   layout; no allowlist text remains.

## 9. Open questions

1. **License file placement.** One root `LICENSE` (the AGPL-3.0) and
   `NOTICE.md` with the retained Niko1221 line and a pointer to
   `upstream/LICENSE` (§4.5). Alternative: our notice only.
2. **Keep running upstream's `tools/test_setup_*` (and the rest of
   `upstream/tools`) in routine `pytest -q`?** Recommended: no. They test
   upstream's `setup.py`, which we do not ship; run `pytest upstream/tools`
   and `pytest upstream/serve` explicitly after each bump (U13).
3. **Submodule URL and a second submodule.** URL
   `https://github.com/Niko1221/Strata.git` as decided. Add
   `lighttransport/Strata` (`glm53f`) as a second submodule now for SP6?
   Recommended: not yet; SP6 decides its own path (`engines/glm/`) when it
   starts, so no unused fetch is added to every CI job today.
