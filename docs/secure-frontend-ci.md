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

Each test case is its own CTest test (`<binary>.<case>`). Cases that need real
Windows semantics carry the label `windows-runtime`; the
`windows-llvm-mingw-cross` test preset excludes it under wine, and
`windows-msys2-clang` runs everything.

The Windows toolchain is open source: llvm-mingw and MSYS2 clang. MSVC is kept
only as an informational job. Caveat: CUDA engine builds on Windows still need
MSVC as the nvcc host compiler; that concerns the engine (`upstream/src/`), not
this frontend.

## Getting the code

    git clone --recurse-submodules https://github.com/shift-f10/m-strata.git
    # or, after a plain clone:
    git submodule update --init

The presets need no submodule. `pytest -q` and the root build do; both stop
with "run `git submodule update --init`" when `upstream/` is empty.

After a `git pull` that moves the gitlink, run `git submodule update --init`
again (or set `git config submodule.recurse true` once). A `+` at the start of
the `git submodule status` line means the checked-out `upstream/` commit differs
from the recorded gitlink (behind, ahead or elsewhere).

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

Since SP1 every test binary, including `secure_frontend_token_reader_tests`,
is built and run on Windows; the Windows protected-file reader's cases carry
`windows-runtime`, so they run only in the `windows-msys2-clang` job (run
`38540719307`: `windows-msys2-clang` 193/193 on SP1, before the SP2 merge).
Counts at the end of SP1 (Task 24): see `docs/AGENT_HANDOFF.md`,
"Verification performed", SP1.
`secure_frontend_http_socket_server_tests` is portable and runs the Windows
listener in that job. `engine_process_tests` and `engine_stdio_process_tests`
start and terminate through the Job on MSYS2; the grandchild assertion
(`job_object_terminates_grandchild`) runs only there (it also passes under wine,
which is not authoritative for Job Objects). `secure_frontend_http_socket_server_tests`
and `secure_frontend_engine_stdio_process_tests` carry a 300 s ctest `TIMEOUT`, and
every CI job has `timeout-minutes: 60`.

SP2 adds eight `.exe` files (38 in all): the test-only
`strata-supervision-fake-engine` and `strata-supervision-fake-engine-elevated`,
the test launcher `secure_frontend_non_elevated_run` (starts
`strata-unified-server` with a non-elevated token on Windows), and five test
binaries (`secure_frontend_{fake_engine,deadline,engine_child,engine_supervisor,server}_tests`),
each case registered as `<binary>.<case>` through SP1's `secure_frontend_add_cases`.
Tests that need real Windows semantics (Job Objects, handles, DACLs, tokens,
console events, crash exit codes, every server-level test) carry the CTest label
`windows-runtime`; the `windows-llvm-mingw-cross` test preset excludes that
label, so wine runs only the portable cases, and only the `windows-msys2-clang`
job runs the Windows rows (not yet run on SP2: pending the owner's push). Two
tests skip with a stated reason (exit 77) when their precondition is absent
(`proc03_other_uid_cannot_open_engine_fd` needs
`STRATA_TEST_SECOND_UID_SUDO=1`, set on the Linux ctest step in CI;
`proc05_cgroup_kill_when_delegated` needs a delegated cgroup v2; no CI step
creates one). G3
(`proc06_elevation_required_is_spawn_failed`) is not a CTest test: the runners
are elevated, so it is a validation-kit step for a non-elevated Windows prompt.

SP3 M1 adds two test binaries — `secure_frontend_concurrency_tests` (pool,
admission, linger list, FIFO gate, request logger) and
`secure_frontend_egress_tests` (static import tripwire plus, on Linux only, the
runtime `LD_PRELOAD` interposer `libstrata_egress_tripwire.so` and
`strata-egress-probe-helper`; both artifacts are test-only and are never
linked into a product binary). The three `win_*` Winsock-parity cases in
`secure_frontend_api_server_tests` carry `windows-runtime` (MSYS2 pending). A
non-blocking `linux-clang-tsan` job (ruling T22-tsan) rebuilds the
`linux-clang` preset with `-fsanitize=thread` and runs
`ctest -R '(concurrency_tests|api_server_tests|http_socket_server_tests)'
-E 'egress_interposer'` (the interposer and TSan do not mix; static import
checks are a separate job) after setting `vm.mmap_rnd_bits=28` — but per the
owner's 2026-10-20 ruling GitHub Actions are disabled and agents never push,
so no CI has run since; the job is the future vehicle for the TSan follow-up
(clang/TSan are not installed on the dev host). The Windows runtime proof of
every SP3 case waits for `windows-msys2-clang` on the owner's hardware.

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
warnings, `ctest` 111/111 at `v0.1.44.1` before the SP1 merge (the frontend's
tests; upstream registers none at `v0.1.44.1` because `STRATA_BUILD_TESTS` is
`OFF`); the per-case count after SP1 is recorded in `docs/AGENT_HANDOFF.md`.

## Python tests

`pytest -q` from the repository root runs only `tools/secure/` (see
`pytest.ini`): the GLM tokenizer wrapper tests and the secure distribution
check tests, 13 in all; the owner's pre-push hook runs it. `conftest.py` stops
the run with "run `git submodule update --init`" when `upstream/` is empty.
Upstream's own suites run only after a submodule bump ("Updating upstream").

## Updating upstream

The runbook (gates at the old pin, bump, gates incl. upstream's own suites with their recorded exclusions and expected counts, pin sites, one commit) lives only in [`PROVENANCE.md`](PROVENANCE.md), "Updating the baseline".

## Offline checkout

An offline box cannot fetch the submodule. The offline kit carries a git bundle
of the pinned upstream commit next to the bundle of our repository:

    git -C upstream bundle create ../out/upstream-v0.2.2-011c9e60.bundle v0.2.2
    git bundle verify out/upstream-v0.2.2-011c9e60.bundle

On the offline box, after cloning our repository from its bundle:

    git submodule init
    git config submodule.upstream.url /path/to/upstream-v0.2.2-011c9e60.bundle
    git -c protocol.file.allow=always submodule update
    git submodule status   # 011c9e600a93bbc9fd451c563501b5831c32bbca upstream (v0.2.2)

The bundle holds tag `v0.2.2` (tag object
`e6fff4ad12908de74952abe53b2bade45a68aa0f`, commit
`011c9e600a93bbc9fd451c563501b5831c32bbca`).

## Workflow

`.github/workflows/secure-frontend.yml` defines these jobs:

| Job | Runs on | Submodule | Blocking |
| --- | --- | --- | --- |
| `linux` (matrix `linux-gcc`, `linux-clang`) | ubuntu-24.04 (pinned; `ubuntu-latest` becomes Ubuntu 26 on 2026-10-19); `linux-gcc` also checks that `pytest -q` collects `tools/secure/` only and runs it | yes (pytest) | yes |
| `windows-cross` (`windows-llvm-mingw-cross`) | ubuntu-24.04 (pinned; `ubuntu-latest` becomes Ubuntu 26 on 2026-10-19); uploads the `.exe` files | no | yes |
| `windows-msys2` (`windows-msys2-clang`) | windows-latest, MSYS2 CLANG64; runs `ctest` | no | yes |
| `windows-msvc` | windows-latest | no | no |
| `top-level-integration` | ubuntu-24.04 (pinned; `ubuntu-latest` becomes Ubuntu 26 on 2026-10-19); first configures **our** root with no `-D` flags (`out/root-defaults`, configure only) and asserts `STRATA_NATIVE_EXPERTS`/`STRATA_ENABLE_CUDA`/`STRATA_ENABLE_HIP` `OFF`, `CMAKE_BUILD_TYPE=Release` and no `_deps`; then configures it with CUDA/HIP/native experts off, builds `strata-unified-server`, `secure_frontend_config_tests`, `strata-plan`, runs `ctest -R secure_frontend_config_tests`, checks no `out/root/_deps` | yes | yes |

The `linux-release` job runs `secure_distribution_check.py` without `--release` until SP7 provides the release manifest and SBOM that `--release` requires; the test-hooks marker scan runs in every mode and covers `strata` and `strata-unified-server` (SP4 Controller note 19).

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

Path filters: the workflow runs on changes to `secure_frontend/**`,
`CMakeLists.txt`, `scripts/**`, `pytest.ini`, `conftest.py`, `tools/**`,
`upstream` (the gitlink), `.gitmodules` and the workflow file. `tools/**` is
kept because our own `tools/secure/` tests live there (layout spec U10).

Status (2026-10-13): all blocking jobs green on run `38458255140` (branch
`ci/windows-runtime-fixes`), before the submodule layout, and on run
`38478775552` (branch `layout/upstream-submodule` @ `4aa742c9`), the first run
after it: all five blocking jobs green, `top-level-integration` building our
root with the `upstream/` engine. The
`windows-msys2-clang` preset links with `-static`, so a child started with the
allowlisted environment (SP0's had no `PATH`; SP2's `PATH` is the engine
library directories plus `%SystemRoot%\System32`) needs no MSYS2 runtime DLL. `windows-msvc`
currently fails at configure (the image ships Visual Studio 2026, the preset
names 2022); it is non-blocking. Run `38540719307` (SP1, reported by the
controller) ran `windows-msys2-clang` 193/193. No CI run covers SP2 yet.
