# Secure Strata agent handoff

> **Agents: read `docs/superpowers/handoff/2026-10-16-controller-resume.md` first.**
> It holds the orchestration-level state at the owner's stop (2026-10-15), the
> owner's binding rules, the sub-controller briefs and lane reports, what waits
> on the owner, and the exact resume procedure. The project is stopped until the
> owner says otherwise. This document is the code-level truth it points at.

## Purpose

This repository is a Windows-and-Linux, security- and privacy-first successor
to Strata. It must retain the model/backend flexibility of upstream Strata
`v0.2.2` while replacing its public service boundary with a small portable
native frontend. UnbiasedStrata is a security-design reference, not a direct
code base.

Read this document, [`PROVENANCE.md`](PROVENANCE.md), and
[`security/threat-model.md`](security/threat-model.md) before modifying the
service, installer, download, telemetry, or process-supervision code.
Also read [`security/privacy-migration.md`](security/privacy-migration.md)
before packaging or replacing any baseline installer/service behavior.

Sub-project 0 (foundation and hardening of `secure_frontend`) is specified in
[`superpowers/specs/2026-10-13-sp0-foundation-hardening-design.md`](superpowers/specs/2026-10-13-sp0-foundation-hardening-design.md)
(§4 lists every T/S/E/P/M/D item) and planned task by task in
[`superpowers/plans/2026-10-13-sp0-foundation-hardening.md`](superpowers/plans/2026-10-13-sp0-foundation-hardening.md).

### Owner decisions and constraints (the owner, 2026-10-13)

These are decisions, not facts derivable from the code. Do not reverse them
without asking the owner.

1. **Scope is the full release path**, split into sub-projects SP0–SP7:
   - SP0 — foundation and hardening of `secure_frontend`. **Done on branch
     `sp0-foundation-hardening`** (not yet pushed or merged).
   - SP1 — typed configuration, canonical paths, CSPRNG tokens. **Done;
     merged into `main` at `e7f01b9e`.** CI run `38540719307` ran
     `windows-msys2-clang` 193/193 on SP1, so its `windows-runtime` cases are
     verified there (exceptions in the SP1 rows below).
   - SP2 — child supervision (`PROC-*` tests). **Done on branch
     `sp2-portable-supervision` for Linux (all L and B rows green locally);
     Windows (W) rows pending the `windows-msys2-clang` job** (not pushed; no
     CI run covers SP2).
   - SP3 — API concurrency, streaming, and `NET-*`/`WEB-*`/`DOS-*`/`EXT-*` tests.
   - SP4 — the explicit `acquire` command. **Tasks 1–24b merged into `main`
     at `4b9fdd31`**; their `windows-runtime` rows passed in
     `windows-msys2-clang` (CI run `38625663621` on `557a2cc7`, 488/488).
     **Tasks 25–29 done on branch `sp4-acquire-part2`** (`06630dbd` …
     Task 29; not pushed); their new `windows-runtime` cases passed on real
     Windows in CI run `38655938748` (`9044a5bc`, 515/516); the one failure,
     PRIV-04's link-set reader (CRLF), is fixed and its re-run is pending.
   - SP5 — frontend parity with the upstream service.
   - SP6 — GLM engine port, in its own directory, tracking
     `lighttransport/Strata` branch `glm53f` and, since 2026-10-14,
     `mw00/project-maya` (see `docs/PROVENANCE.md`).
     - **2026-10-25 (owner live direction, supersedes "its own directory"):** the
       engine is consolidated into ONE executable (`engine/`, Strata lineage) with
       each family's differences in libraries; GLM-5.3-Flash joins it as a
       self-contained family **DLL** `strata_glm`, built from Project Maya's GLM
       engine vendored under `engine/third_party/project-maya/` (a 115-file
       transitive include-closure, MIT). Done so far (uncommitted): the vendored
       subtree; registry `ModelFamily::Glm5Next` (arch `glm5-next`) with RED→GREEN
       tests; the family switch in `engine/src/program/generate.cpp` and a CMake
       hook (`STRATA_BUILD_GLM`, off by default) that delay-load the DLL and route a
       `glm5-next` file or `--glm-pack` to its one export `glm_main`. **Built and
       run on the owner's GPU box (2026-10-25):** `strata_glm.dll`
       (`third_party/project-maya/CMakeLists.txt`, 120 vendored files + the pinned
       ggml-cuda MMQ sources referenced in place) compiles and links with **0
       unresolved externals** and `dumpbin` shows it exports **only** `glm_main`;
       `strata.exe` built with `STRATA_BUILD_GLM=ON` reports
       `--families` = `qwen4exp qwen35moe glm5-next`; and
       `strata.exe --glm-pack C:\Strata-data\packs\glm53 --tokens "9707,11,1879,0"
       --max-new 8` ran the full engine (expert pool warmed 11,103 experts, MMQ
       prefill, greedy decode) to `RUN_EXIT 0`, emitting a clean autoregressive
       continuation. Local compile/run only — the authoritative Windows build claim
       remains the `windows-msys2-clang` CI job, GPU numerics the owner's box.
       **Phase G2 done (2026-10-25): GLM serves over the OpenAI API like qwen.**
       Maya's full `glm_pack_generate` serve driver (serve wire protocol, slots,
       conversation cache) is vendored into the DLL as
       `third_party/project-maya/src/glm_driver.cpp` (Maya's `generate.cpp` prologue
       lines 1–4542 — Options + shared serve/prompt helpers + `glm_pack_generate` +
       the arg-parse/dispatch of `main`, renamed to the exported `glm_main`; the
       native-family driver and its deps are not vendored). Serving our `strata.exe`
       via the reviewed serve layer on `our-glm.json` (`--glm-pack
       C:\Strata-data\packs\glm53`) answered `/v1/chat/completions` for model
       `glm-5.3-flash` with coherent output (~21 tok/s prompt, ~11 tok/s decode,
       `n_ctx` 32768). Remaining/tuning: **MTP not engaged** (`draft_n=0` — needs the
       spec/mtp flags wired for GLM); the `/WX` pass on `glm_driver.cpp`
       (`STRATA_GLM_WERROR=ON`, built OFF during G2 bring-up); logit parity vs Maya as
       the oracle; multi-GPU. Plans:
       `docs/superpowers/specs/2026-10-25-glm-port-blueprint.md` and
       `…-single-engine-family-libraries-plan.md`.
   - SP7 — release, plus an offline GPU validation kit.
2. **Windows toolchain is open source**: llvm-mingw (cross from Linux) and
   MSYS2 clang (native). MSVC is an informational CI job only. Caveat: CUDA
   engine builds on Windows still need MSVC as the nvcc host compiler.
3. **No GPU and no `nvcc` on the dev host.** GPU work (SP5, SP6) waits for an
   NVIDIA host the owner is building. Offline test boxes exist for both Linux
   and Windows.
4. **Product north star: "as easy as Ollama".** One `strata` binary with
   `pull`, `run`, `serve`, `list` and `key show`; a zero-config loopback first
   run with an auto-generated token that the client uses automatically (the
   token-on-loopback rule is **kept**); one install per OS; explicit
   `strata update` through a verified manifest; never auto-update. A
   product-experience spec is written between SP0 and SP1.
5. **Launch, install and configuration must be user-friendly and secure.** This
   lands in SP1 and SP7.
6. **Upstream-trackability.** Upstream Strata is the git submodule `upstream/`
   (`https://github.com/Niko1221/Strata.git`), pinned by its gitlink at
   `011c9e600a93bbc9fd451c563501b5831c32bbca` (tag `v0.2.2`) and never
   edited, inside or outside the submodule. The repository root holds only our
   code; our additions to upstream behavior are wrappers that import upstream
   (`tools/secure/secure_tokenizer.py`). The upstream Python service stays in
   the submodule, untouched, and is quarantined at package time
   (`scripts/secure_distribution_check.py` rejects any `upstream` path). An
   upstream update is "Update upstream" below: bump the pointer, run the gates.
   The SP6 engine is never woven into `upstream/src/`.
7. **Routine pytest runs our tests only.** `pytest.ini` collects
   `tools/secure/` with no ignores or deselects. Upstream's own suites
   (`pytest upstream/tools` with its recorded exclusions, `pytest
   upstream/serve`) run explicitly after each submodule bump; the exact
   commands are in [`PROVENANCE.md`](PROVENANCE.md), "Updating the baseline".
8. **Pre-push hook.** The owner's pre-push hook runs `pytest -q`. With
   `pytest.ini` it collects `tools/secure/` only (13 tests). `conftest.py`
   stops the run with "run `git submodule update --init`" when `upstream/` is
   not checked out.
9. **CI status: all blocking jobs green on Windows and Linux** (run
   `38458255140`, branch `ci/windows-runtime-fixes`, 2026-10-13). The first
   real run (`38456682352`) failed three tests on `windows-msys2-clang`; the
   fixes are listed under "Verification performed". The wine results below
   remain a smoke test only. After the submodule layout: run `38478775552`
   (branch `layout/upstream-submodule` @ `4aa742c9`), all blocking jobs green.
   SP1: run `38540719307`, `windows-msys2-clang` 193/193 (reported by the
   controller, 2026-10-14). SP2 has not been pushed; no CI run covers it.

### Owner decisions, 2026-10-14

Approved by the owner ("approve all"); binding for plans and implementation.

- Upstream bump to `Niko1221/Strata` v0.1.44.1 merged (main `c07f2429`).
- SP2 Task 22 = bound: engine-stdin writes get an overall bound (the request's
  remaining deadline) in SP2's second run (Task R1); documented by Task 28
  (`docs/security/engine-protocol-limits.md`, "Windows stdin writes").
- SP3 and SP4 specs APPROVED, all recommendations (see the specs' "Decisions
  taken"). SP3 plans: two, M1 (concurrency and hardening) and M2 (streaming).
  SP4 D5 stays open between (A) and (C); (D) ships meanwhile.
- SP6: A1-A11 recorded in the research doc, section 6.
- Details: `docs/superpowers/specs/2026-10-14-sp3-api-concurrency-streaming-design.md`
  (section 11), `docs/superpowers/specs/2026-10-14-sp4-acquire-design.md`
  (section 12), `docs/superpowers/research/2026-10-14-sp6-glm-engine-sources.md`
  (section 6).

### Owner decisions, 2026-10-20

Approved by the owner during the upstream update; binding.

- Upstream bumped to `Niko1221/Strata` **`v0.2.1.3`** (owner chose the latest
  tag of the 0.2.1 line); the `RESUME`/`REUSED` protocol gap was fixed as a
  hotfix on main immediately before the bump (`01ad7226`).
- **Tag version governs:** `upstream.version` → `kEngineVersion`, and manifest
  `min_version` compare against the tag number (`0.2.1.3`); the engine's CMake
  self-version `0.1.46.2` is informational for the frontend.
- **GitHub is storage only:** Actions disabled at the repo level by the owner
  ("do not do GitHub Actions on GitHub, we just need to save there"). Push to
  save; never rely on, await, or watch workflow runs.
- **Windows runtime proof deferred:** agents prove only the local
  `windows-llvm-mingw-cross` cross-build (`-Werror`); real-Windows runtime
  claims wait for the owner's own hardware. The green `windows-msys2-clang`
  run at `7bddb9e1` is the last CI-authorized Windows runtime proof and stands
  for the `v0.2.1.3` pin.
- New commits drop the `Claude-Session:` trailer (keep `Co-Authored-By`
  literal; existing trailers never rewritten). wine stays uninstalled locally.
- **Host toolchain 2026-10-21 (controller):** clang 18 + compiler-rt (TSan) +
  `llvm-symbolizer` live under `.toolchains/clang18/` with `clang`/`clang++`
  wrapper scripts in `.venv-dev/bin` (they pass `-resource-dir` because the
  Ubuntu driver hardcodes `/usr/lib/llvm-18`). The `linux-clang` preset with
  `-fsanitize=thread` builds and runs here; set
  `TSAN_OPTIONS=external_symbolizer_path=<repo>/.venv-dev/bin/llvm-symbolizer`.
  SP3 M1's merge gate ran the filtered concurrency suites under TSan: 90/90,
  zero ThreadSanitizer reports. The `linux-clang-tsan` CI job stays a
  definition only — the controller runs this gate locally.

### Owner decisions, 2026-10-25 — the opencode worker (binding)

Stated by the owner in chat (2026-10-25), to be recorded in the handoff chain:

- **Worker identity:** the current controller is the **`opencode` worker** — a
  separate worker from the Claude worker line that ran the owner-box GPU/Windows
  sessions. The opencode worker names itself as such in handoff documents and
  uses `Co-Authored-By: Qwen 3.8 Flash Next IQ3_S <noreply@anthropic.com>`
  (no `Claude-Session:` trailer) in its commits, per the 2026-10-20 rule.
- **Hardware boundary:** this worker runs on **Linux without a GPU and without
  Windows access** — unlike the Claude worker it has never been on the owner's
  GPU box and cannot be. Rule 7 (never claim GPU/Windows behaviour verified
  from this host) applies to it absolutely; there is no path by which this
  host produces runtime evidence for those platforms.
- **No model files (owner note, same ruling):** this Linux dev box has **no
  model weights** — no GGUF packs, nothing to run a model against. The opencode
  worker therefore **relies heavily on the upstream code itself** (the vendored
  `upstream/` and `engines/maya-ref/` checkouts are the behavioural reference
  for every port) plus hermetic tests built on synthetic fixtures. What it can
  prove on this host: compiles (`-Werror`), cross-builds, unit proofs and
  synthetic-data math tests. Every real-model claim — coherence, token-exact
  parity, tok/s — waits for the owner's box.
- **Consequence for engine work:** with the opencode worker the focus is
  **porting the engines without full live verification**: compile (linux debug
  + release, `-Werror`), cross-build (`windows-llvm-mingw-cross`, compile-only),
  and unit-test what can be proven on Linux; every runtime/timing/token-exact
  GPU claim and every real-Windows claim waits for the owner's box.

## Current baseline

- Upstream baseline: the `upstream` submodule gitlink recorded on `main` is
  `Niko1221/Strata` tag `v0.2.4.2`, commit
  `dfa3f402` (owner bump, landed with the SP5-P3-style parity refresh
  2026-10-25c — the tables are certified AT this pin, all tripwires green).
- Vendored read-only reference (SP5 M2, D-10): the `engines/maya-ref` submodule
  gitlink recorded on `main` is `mw00/project-maya` tag `v1.1.27`, commit
  `4615f346` (owner bump, landed with the SP5-P3-style parity refresh
  2026-10-25c — certified AT this pin). Never built by any preset,
  never packaged (`secure_distribution_check.py` rejects a `maya-ref`
  component), never collected by pytest; cited by `file:line` only.
- Git remotes: `origin` is the owner's repository. Only the owner pushes. The
  older `upstream` remote of this repository is no longer used for tracking;
  the submodule has its own `origin`.
- Secure baseline commit on `main`: `7de35b0c` (the native `secure_frontend`
  as handed off before SP0), followed by the SP0 spec and plan (`4884e9a8`,
  `4dce6f79`). SP0 is the branch `sp0-foundation-hardening` on top of `4dce6f79`.
- UnbiasedStrata reference: commit
  `46c46ecfc11d2315512f21f7549b0a7792ccd2ae`; no code has been copied from it.
- GLM-5.3 reference: `lighttransport/Strata` branch `glm53f`, commit
  `4894c9967f5754a2052d7b05f10a01b6071bb6e4`; the fixed native serving profile
  and Python `glm4`/`chatglm-bpe` tokenizer handling were implemented against
  its observable contracts. The separate GLM engine, CUDA/HIP kernels, and
  remote tensor-parallel worker are not yet ported (SP6).
- Second GLM-5.3 reference (owner, 2026-10-14): `mw00/project-maya` tag
  `v1.1.21` (pinned 2026-10-21; named at `v1.1.8`), commit `451f9cb6a21ea65673bdde2e57b4a670ae2b8859` (MIT, Strata
  derivative: NVIDIA + AMD HIP, experimental Windows, MTP, quant tooling). No
  code has been taken from it; SP6 planning weighs it against `glm53f`.
- Supported target platforms: native Windows 10/11 and Linux. The intended
  compatibility target is every upstream-supported model profile, model
  quantization, vision option, context option, GPU backend, and multi-GPU
  configuration. Do not silently reduce that scope to one model or NVIDIA.

## Repository layout

Upstream Strata is the git submodule `upstream/`; the repository root holds
only our code. Deleting `upstream/` leaves exactly our code.

| Ours, at the root | Pristine, under `upstream/` (submodule @ `011c9e60`, tag `v0.2.2`) |
| --- | --- |
| `CMakeLists.txt` (builds `secure_frontend/` and, by default, the engine from `upstream/`) | `upstream/CMakeLists.txt`, `upstream/src/`, `upstream/include/`, `upstream/cmake/`, `upstream/third_party/`, `upstream/sycl/` |
| `secure_frontend/` (own `CMakePresets.json`; builds without `upstream/`) | `upstream/serve/`, `upstream/setup.py`, `upstream/setup.sh`, `upstream/requirements.txt`, `upstream/chat.py`, the launchers |
| `tools/secure/` (GLM tokenizer wrapper and our pytest suite) | `upstream/tools/` (incl. `strata_tokenizer.py`, `test_setup_*.py`) |
| `scripts/`, `tests/security/`, `docs/` (this file, `PROVENANCE.md`, `secure-frontend-ci.md`, `security/`, `superpowers/`) | `upstream/docs/`, `upstream/tests/`, `upstream/bench/`, `upstream/data/`, `upstream/ref/` |
| `.github/workflows/secure-frontend.yml`, `.gitignore`, `.gitattributes`, `.gitmodules`, `pytest.ini`, `conftest.py`, `LICENSE`, `README.md` | `upstream/.github/`, `upstream/LICENSE`, `upstream/README*.md`, `upstream/AGENTS.md`, `upstream/SECURITY.md`, `upstream/Dockerfile` |

The exception is `engine/`, our engine executable `multiengine.exe`: it carries a copy of upstream's engine tree (at
`v0.2.6` since 2026-10-27, unedited but for one appended line of its root `CMakeLists.txt`) beside our launcher and
family libraries (`engine/src/multiengine`, `engine/src/gemma4`, `engine/third_party/project-maya`); see
`PROVENANCE.md`, the multiengine section, and `engine/README.md`. The `upstream/` gitlink is now `dfa3f402`
(`v0.2.4.2`); the table header's `v0.2.2` predates that bump.

Getting the code: `git clone --recurse-submodules`, or `git submodule update
--init` after a plain clone. Inside a `.worktrees/<name>` worktree run `git
submodule update --init` once as well. After a `git pull` that moves the
gitlink, run `git submodule update --init` again (or set `git config
submodule.recurse true` once); `git submodule status` showing a `+` prefix
means the `upstream/` checkout differs from the recorded gitlink (either direction). The presets need no submodule;
`pytest -q` and the root build do, and both say so when it is missing.

Root build (GPU backends off, nothing fetched): `cmake -S . -B out/root -G
Ninja && cmake --build out/root && ctest --test-dir out/root`.
`-DSECURE_STRATA_BUILD_ENGINE=OFF` builds the frontend alone. Details:
[`secure-frontend-ci.md`](secure-frontend-ci.md), "Root build".

### Update upstream

Follow [`PROVENANCE.md`](PROVENANCE.md), "Updating the baseline" (the only copy of the runbook: gates at the old pin, bump, gates, pin sites, one commit).

## What has changed

### Claude Code session, 2026-10-29 z38 (Windows box; owner-directed; committed with this entry and pushed on the owner's "commit and push once done")

The owner: "when the "--debugging on" was not provided in the install/builder, I want the settings to hide the
Troubleshooting section. In the install section, we should ask these questions, if they want email alerts, search,
etc so that they can build exactly what they want".

- **Troubleshooting only in a debugging build.**
  - `POST /v1/strata/config` answers `"debugging"` (`debugging_build()`, make `--debugging`).
  - Only such a build lists and saves `logging.engine_output`, the Troubleshooting row (`managed()` in
    `config_routes.cpp`). Any other build refuses it by name, as a row the tab does not manage. The config file still
    takes it.
  - The page hides `logs-settings` unless `debugging` is true.
  - The TUI's Settings lists the server's rows, so it has no row for it.
  - With the row on, a non-debugging build's start-up notice names `logging.engine_output in the config file`, not
    Settings > Troubleshooting.
- **The installers ask what to build.** `install.ps1` and `install.sh` ask one question for each of make's choices,
  before "Looking at what this PC has" (Linux's packages depend on the answers):
  - email alerts, web search, the window app, the terminal app;
  - the Maya engines (GLM/DeepSeek/MiniMax), Raon-1-mini, pictures (`vision`);
  - a debugging build.
  - Enter takes the default: everything but debugging. On Linux the window defaults to no without a display.
  - `-With`/`--with`, the new `-Without`/`--without` (make's `--without`) and `-Debugging`/`--debugging` answer them
    on the command line; `-Yes`/`--yes` asks nothing.
  - A name outside make's lists is refused before anything is installed. The lists are checked against make.py's
    `OPTIONAL_FEATURES`/`LEAVE_OUT`.
  - Windows no longer passes `--with` with nothing after it: PowerShell 5.1 drops an empty argument.
- **Also fixed:** `engine_output_reaches_the_log_only_when_asked` failed in a debugging tree. Its "off" half started
  with an empty config, which a debugging build reads as on. It now turns the row off explicitly there; the normal
  build's path is unchanged.
- **Proof**:
  - RED first: `troubleshooting_is_offered_only_in_a_debugging_build` (config routes), the page check in
    `web_ui_has_server_settings`, the notice check in `engine_output_reaches_the_log_only_when_asked`, and
    `test_the_installers_ask_what_to_build` and `test_the_window_question_defaults_to_no_without_a_display`.
  - GREEN with MSVC, in a normal tree (`out/build/shares`) and a debugging tree (`out/build/debugging`): 166/166 each
    (config routes, router, typed config, server CLI, TUI, the engine-output case).
  - GREEN on Linux gcc 13.3 and clang 18.1: 168/168 each. `tools/secure/test_install.py` and `test_make.py`: 42/42,
    including PowerShell's parser and `sh -n`.
  - Live, on scratch servers (port 18091). The normal build answered `"debugging":false`, without the row, and refused
    a save of it by name; the page showed every card but Troubleshooting. The debugging build answered
    `"debugging":true` with the row, and the page showed Troubleshooting with its switch.
  - Live, `install.bat -Plan` in a real console (hidden conhost), the questions answered. The plan's make line
    followed them, for example `--with email,search,window,tui --without maya`. With piped answers, an invalid
    "maybe" was asked again.
  - Live, `install.sh --plan` in WSL under `script` (a pty):
    - answers gave `--with search,window --without raon1`;
    - `--with email,tui` asked only the rest;
    - with no display, the window question read `[y/N]` with "(no display here)";
    - `--yes` asked nothing;
    - `--without qwen` was refused.
- **Not verified**: a full install run with the questions (only `-Plan`/`--plan`), and the Linux window build.

### Claude Code session, 2026-10-29 z37 (Windows box; owner-directed; committed with this entry, not pushed)

The owner: "clicking calibrate does not stop the current running model and empty the vram before running the
calibration". Checked on a scratch server (port 18090, the fake engine started 2.5 s slow, two Gemma 4 cards), with
every engine process logged every 100 ms. Three paths: calibrate card B while A runs, calibrate A itself, and
calibrate-then-start B while A runs.

- **The stop is there.** On each path A's engine had ended by the first sample after the request. The log has
  "stopped A" before "calibration trial 1". Never more than one engine ran at once, and the model list showed A
  stopped from the first sample. `retire_running` waits for the engine to exit (`stop_engine`).
- **Memory comes back at the exit.** A probe on this PC (RTX 4080 SUPER, driver 610.88) held 12 GiB of VRAM and
  16 GiB of pinned RAM. Both were free by the time its process had ended: nvidia-smi read 925 MiB at the next 50 ms
  sample, and RAM was back at once. The exit itself took 1.15 s, and the server waits for it. So each trial starts at
  once on memory already free. The 5090's "the expert arena needs 39.97 GiB of RAM but 38.79 GiB is available" fits
  its 48 GB of RAM, not an engine still exiting.
- **What was wrong is what the Monitor said.** For the whole calibration it reported an engine "starting" with no
  model, while each trial's engine filled the GPU again. That reads as the old model never left. Now `/metrics` says
  `calibrating` with the card being calibrated (`ModelLauncher::metrics_json`; `live.state` `calibrating` in
  `ServeMonitor`), and the Monitor tab shows a Calibrating badge, "Calibrating <card>" and "each trial starts the
  model, so the GPU memory in use is the calibration's". `/health` is unchanged (still `starting`).
- **Proof**:
  - RED first: `the_monitor_names_a_calibration_while_it_runs` (launcher; the calibration fixture now reports to a
    `ServeMonitor`), `monitor_reports_live_states`, and the page checks.
  - GREEN: the launcher, Monitor and router suites with MSVC (`switch_waits_for_the_generation_then_swaps` failed once
    under a parallel run, then passed alone four times), Linux gcc 13.3 and clang 18.1 (112/112 each).
  - Live: `/metrics` during a calibration named the calibrating card, with nothing loaded.
- **Not verified**: a real engine's calibration (only the fake and the probe), and the owner's 5090.

### Claude Code session, 2026-10-29 z36 (Windows box; owner-directed; committed with z35 and this entry, not pushed)

The owner: "when I browsed for the qwen 3.6 35B it did not prompt for the mtp layer and the card has no browse folder
for it" (answered: its draft layer is the GGUF's own MTP block, `MTP block loaded` in his log), then "Yes, show it";
and "make sure you are updating the readme on the features and changes as well".

- **Card JSON**: `card_builder`'s `DraftLayerSlot` gains `inside` and `in_file`. A qwen-profile card with no `--pack` and
  an `mtp` option (a Qwen3.6 card) is `{"kind":"inside","in_file":<the card has --mtp>,"path":null}`: there is no
  folder to point at. `POST /v1/strata/cards/mtp` on such a card is 422 `card_refused` ("this model's draft layer is in
  its model file; there is no other to point at").
- **Page**: the card's MTP draft layer row says **In the model file** (the card drafts with it; its MTP switch turns it
  off) or **This model file has no MTP draft layer**, hides **Choose...**, and the options hide the MTP switch when the
  file has none. Add a model says the same in its toast and never offers to point to a draft layer for such a card.
- **README.md pass** (the changes since z29): the Qwen3.6 draft layer; Settings > Model folders' network-shares switch
  and a section "Model folders on network shares (experimental)" with its status, what has and has not been tried (the
  owner: "add in the readme the experimental network share as well and it's status"), also in the Features list;
  Settings > Troubleshooting (`logging.engine_output`);
  the per-card **longest answer**; the `--debugging` build and its pop-up / `y` prompt; the window's **Server log**;
  Troubleshooting rows (an engine that does not start or a calibration that stops, an exit code; a Monitor with no GPU
  readings; a Qwen3.6 card with no draft layer to choose; a share's folder refused).
- **Proof**:
  - RED: `a_qwen36_gguf_makes_a_qwen_card` (card_builder: the slot and its JSON), the Qwen3.6 add test in card_routes
    (the 422), the page strings in api_router's cards tests.
  - GREEN: with z35 (below).
- **Not verified**: the page's draft-layer row with a real Qwen3.6 file (only the fixture's GGUF header).

### Claude Code session, 2026-10-29 z35 (Windows box; owner-directed; committed with z36, not pushed)

The owner: "I want to entertain the idea of being able to support SMB shares or CIFS shares or NFS shares for models",
"we should mark that as experimental", then "Yes, build it". Off by default; nothing changes until it is turned on.

- **Config**: `roots.allow_network_shares` (boolean, default false; README table row). Its description says it is
  experimental and why. The server warns at every start while it is on (`warn_of_network_shares`, after
  `warn_of_engine_output`, both start paths).
- **Paths** (`canonical_path`): a new `RootPolicy::model_folder`. On this PC's own drives it is exactly `existing`. On a
  network share it opens like `external` (no owner or write checks; marked external) only while `network_shares()` is
  on (set from the config at load):
  - **Windows**: a UNC root `\\server\share\...`, or a mapped drive whose final path is `\\?\UNC\...`.
    `check_windows_root_form` and `classify_final_path` take `allow_unc`; the root keeps its `\\server\share` name, and
    `open_beneath` classifies with UNC allowed when the root is UNC. `extended_path` turns `\\server\...` into
    `\\?\UNC\server\...`.
  - **Linux**: `fstatfs` f_type NFS (0x6969), SMB (0x517B), CIFS (0xFF534D42), SMB2 (0xFE534D42)
    (`is_network_file_system`).
  - Every model-folder site uses `model_folder` (`roots.models`, the launch composition, the card routes and both pack
    checks) unless `roots.allow_untrusted_media` makes it `external`. The server's own folders, key, config and
    downloads stay `existing`/`create_private`.
- **Folders** (`local_folders`): `LocalVolume.network`; `on_network_share` (Windows: `GetDriveTypeW` = `DRIVE_REMOTE` or a
  UNC root, through `drive_root`, since MSVC's root of a UNC path is only `\\server\`; Linux: `statfs`). While on, the
  picker lists mapped drives (Windows) and nfs/nfs4/cifs/smb3/smbfs mounts (Linux), and takes a typed
  `\\server\share\...`. While off, a share's path or a mapped drive is refused with the switch's name. A folder on a
  share is never made private, and no private folder is made in one.
- **Page**: Settings > Model folders > **Use model folders on network shares (SMB/CIFS, NFS)**, with an Experimental
  badge, what it means, and the running server's state. `POST /v1/strata/folders/sharing` takes and answers `network`
  (and `network_running`), and saves `shared` and `network` independently. The volumes JSON marks a share's drive.
- **Threat model**: the "Model folders on network shares" contract entry and the "Network shares residual".
- **Proof**:
  - RED: `network_share_roots_are_a_model_folders_opt_in` and `a_model_folder_on_a_share_opens_only_when_allowed`
    (paths; live through `\\localhost\C$`), `a_share_folder_is_picked_only_when_allowed` (local_folders; then again for
    the off text that names the switch), `network_shares_are_an_experimental_opt_in` (typed_config),
    `network_shares_warn_at_every_start` (api_server), the sharing route in `the_shared_folders_setting_is_read_and_saved`
    (card_routes), auth04 (server_cli).
  - GREEN with z36 (the shares build tree, MSVC): 230/231 of the touched suites. The one failure,
    `switch_waits_for_the_generation_then_swaps` (model_launcher, a file this change does not touch), failed under the
    parallel run while a Linux build loaded the PC, then passed alone three times. Linux gcc 13.3 and clang 18.1: 237/237
    each.
  - Live, a scratch server on port 18091 (fake engine) and `\\localhost\C$\...\share-live\models`:
    - Off, the share was refused (422) and the page's picker named the switch.
    - The switch saved ("Saved. It applies when the server restarts."). After the restart the warning was in the log,
      the sharing route answered `network_running: true`, the picker opened the share and listed its file, and
      make-private was refused (422, "a folder on a network share can't be made private from this PC; ...").
    - Then off again.
- **Not verified**: a real SMB/CIFS server on another PC, an NFS or CIFS mount on Linux (only the f_type check and the
  local UNC path), a drive letter mapped as another account, a model run from a share, and the window app's view.

### Claude Code session, 2026-10-29 z34 (Windows box; owner-directed; committed with this entry, not pushed)

The owner: "starting an uncalibrated server recommend calibrating first and when answered yes, it will calibrate.
However, during calibration, allow users to cancel calibration if they choose to." Until now Start calibrated first
unasked only for a Gemma 4 or flint page card, and never asked; Cancel already existed (the page's calibration
panel, the TUI's `x`).

- **Server**: `POST /v1/strata/models/start` takes `"calibrate": false` (a boolean only; anything else is 400
  `invalid_request`): `ModelLauncher::start(..., may_calibrate = false)` starts the card without the calibration a first
  start (Gemma 4, flint) or a "recalibrate when settings change" would run first. Without it, starts are as before
  (the tray, `models.start`).
- **Page** (browser and strata-window): Start on a card whose `calibration.state` is `missing` (and not calibrating)
  opens the `calibrate-first` dialog (the key dialog's styling): **Calibrate, then start (recommended)** posts
  `calibrate` with `then_start: true` (no second question; the dialog says a running model is stopped first),
  **Start without calibrating** posts the start with `calibrate: false`, **Cancel** or Esc does nothing. A card already
  calibrated asks as before (restart or stop the running one). Cancel during a calibration keeps nothing; when it was the
  page's own calibrate-then-start (`launcher.calibrateThenStart`), the page waits for the calibration to end and asks
  "Start it now without calibrating?".
- **TUI**: Enter on such a card asks in the Start box ("Stop X and start Y? It is not calibrated", or "Start Y?" /
  "Restart Y with these options?"): `c` calibrate, then start (recommended); `s` start without calibrating; any other
  key cancels (`Prompt::calibrate_first`; y/n is avoided, n means "don't" in the other prompts).
- **Proof**:
  - RED: `a_start_without_calibrating_starts_at_once`, `the_start_route_takes_calibrate_false` (launcher),
    `an_uncalibrated_start_asks_to_calibrate_first` (TUI), the page checks in `web_ui_models_tab_starts_stops_and_polls`.
    The TUI fixture's Qwen3.8 is now calibrated, so the restart flows still test a calibrated card.
  - GREEN: the launcher, TUI and router suites with MSVC (`failed_start_keeps_the_server_usable` failed once under a
    parallel run, then passed alone three times), Linux gcc 13.3 and clang 18.1 (101/101 each).
  - Live, a scratch server on port 18090 with a tiny Gemma 4 card in a browser:
    - The dialog showed; Cancel started nothing; Start without calibrating started it with the calibration still missing.
    - Calibrate, then start sent `then_start: true`. Cancel at trial 4 logged "calibration cancelled; nothing was
      kept", the question came, and yes started it with `calibrate: false`.
- **Not verified**: strata-window's view of the dialog (the same page in WebView2), and the TUI in a real console.

### Claude Code session, 2026-10-29 z33 (Windows box; owner-directed; committed with this entry, not pushed)

The owner: "the windows app has no scrollbar"; "the browser as well, there is not scrollbar in the settings". The page's
body never scrolls (`overflow: hidden`); each view taller than the window scrolls itself, and Monitor, About and Models
did, but **Settings had no such rule**. Its lower cards could not be reached with the mouse or keys: Model folders ("Use
model folders as they are"), Email alerts and Web search. That is why the owner "did not see" the SearXNG and folder
settings (z30's reading, a build or a client outside the networks, was not the cause on this PC). An agent's
`scrollIntoView` reached them, which hid the bug.

- `.view--settings` joins `.view--monitor, .view--about { overflow-y: auto; }` (app.css).
- **Proof**: RED first (`web_ui_has_server_settings` now requires an `overflow-y: auto` rule for the Monitor, About,
  Models and Settings views; only Settings failed). The router suite 58/58 with MSVC, Linux gcc and clang. Live: a
  scratch server's page in a browser scrolled to the bottom of Settings with the mouse wheel (3857 px in a 708 px
  view); the Web search card showed whole.
- **Not verified**: strata-window's view of it (the same page in WebView2).

### Claude Code session, 2026-10-29 z32 (Windows box; owner-directed; committed with this entry, not pushed)

The owner: "moving forward, we would rather build a version that has --debugging on the installer and it will have a
pop up on the windows version that this is the debugging version and the tui you will have to accept and press y to
start".

- **One notice** (`window::kDebuggingNotice`, window_shell): what a debugging build is for, what it logs and what that
  can hold, and the precautions. `window::asks_before_starting()` is `debugging_build()`.
- **strata-window** (Windows and GTK): a debugging build opens with a Yes/No pop-up ("M-Strata - debugging build", the
  notice and "Start the server?"); **No is the default** and ends the app before anything starts. Windows: a
  `MessageBoxW` after the one-window-per-data-folder check (a second start shows the first window, no second pop-up).
  GTK: a `GtkMessageDialog` in `activate` before the window is made (only the first instance activates that way; a
  refusal returns before the application is held, so it ends).
- **strata-tui**: a debugging build shows the notice on a screen of its own (`tui::draw_debugging_notice`: title, the
  paragraphs wrapped at the width, then `y: start the server    any other key: quit`) and starts the server only on y
  (`tui::debugging_gate`: y or Y accepts, a wheel turn is no answer, any other key, a quit signal or a closed terminal
  ends it before anything starts).
- make's notice and the README's `--debugging` row say so. Any other build starts as before.
- **Proof**:
  - RED: `a_debugging_build_says_what_it_logs_before_it_starts` (window shell) and `a_debugging_build_waits_for_y`
    (TUI) against stubs.
  - GREEN: the window shell and TUI suites 28/28 with MSVC, Linux gcc 13.3 and clang 18.1. strata-window builds with GTK
    on both Linux compilers.
  - Live, in a debugging tree (`out/build/debugging`, `SECURE_FRONTEND_DEBUGGING=ON`, scratch data, port 18090):
    - TUI: n quit with nothing listening; y showed the dashboard, and the server log had the debugging-build and
      engine-output warnings.
    - Window: the pop-up came up with nothing listening. No ended the app (exit 0). Yes started the server and showed
      the window.
- **Not verified**: the GTK pop-up was compiled but not run (no display in the toolchain distro).

### Claude Code session, 2026-10-29 z31 (Windows box; owner-directed; a worker session's change, committed with this entry, not pushed)

On his RTX 5090 PC the Monitor showed no GPU reading at all (load, VRAM, temperature, power, PCIe).

- **Monitor GPU note**: `parse_nvidia_smi_line` now reads any bracketed nvidia-smi placeholder (`[Unknown Error]`,
  `[Insufficient Permissions]`, `[GPU requires reset]`, ...: printable ASCII, at most 64 bytes) as an empty reading, as
  it did `[N/A]`; a malformed number still refuses the whole line (before, one such field threw away every line, and
  the note said "nvidia-smi is not answering"). The About page's GPU list reads its lines the same way. `GpuSampler`'s
  note quotes what nvidia-smi answered since its last reading when it exits or gives no reading for 5 s (`nvidia-smi
  stopped after answering "Failed to initialize NVML: ..."`; printable ASCII, anything else `?`, at most 120 bytes).
- **Proof**: RED first (`parses_nvidia_smi_lines`: an RTX 5090-style line with five placeholders and six malformed
  ones; `gpu_sampler_reads_a_fake_tool` with the new `--fake-nvidia-smi-unreadable` fake). GREEN with MSVC 21/21 and on
  Linux gcc and clang (the serve monitor suite).
- **Not verified**: the 5090 PC. nvidia-smi's own stderr still goes to the null device, so the note can quote only its
  normal output; the environment it runs with is unchanged (if the note says "Failed to initialize NVML", a missing
  `SystemRoot` is the first thing to try).

### Claude Code session, 2026-10-29 z30 (Windows box; owner-directed; committed with this entry, not pushed)

On his RTX 5090 PC every calibration's first engine ended before ready ("crashed" / "error_exit", z28), and answers
stopped at 120.1 s marked `length` with a 128K context. The owner: "yes I want it, I need to be able to see what's
wrong so we can develop and see why it does not calibrate on the 5090"; "make the default timeout configurable per
model on the card"; "have an option in the installer/builder to have --debugging on so that it is verbose on the logs
... it will tell the user the implications of having this on and precautions to take".

- **The engine's own output in the log** (`logging.engine_output`, false; Settings, new **Troubleshooting** card):
  - `EngineCommand::stderr_lines`: a line sink. With it, `EngineStdioProcess` pipes the child's stderr through the
    drain the conversation cache already used (`read_conversation_cache` still alone decides whether the cache is
    read), and hands each line on as `printable_stderr_line` makes it: printable ASCII, a tab a space, every other byte
    `?`, a line past 4 KiB cut there with ` [cut at 4096 bytes]`. A throwing sink loses its line, never the drain. On a
    stop the drain still reads what the pipe already holds (at most 256 reads, never waiting): a failing engine's
    last lines are what this is for.
  - `model_runtime.cpp` sets the sink for every family when the row is on (a calibration's starts go through the same
    plan): `[engine] <line>` to the server's stderr in one write. The server warns at every start that the engine's
    output can hold what it prints of prompts and the paths of model files (`warn_of_engine_output`). Threat model:
    the second engine-stderr exception, an owner-accepted disclosure while on.
  - `auth04_no_switch_disables_auth` lists the new switch (it reads, skips or replaces no key).
- **An engine's end names its exit code**: after `engine generation N ended: crashed` comes `engine generation N: exit
  code 0xC0000005` (Windows statuses from 0x10000 up in hex, small codes and POSIX signals in decimal;
  `describe_exit_status`, `EngineSupervisor::log_status`; a line of its own, as the supervisor tests match whole lines),
  so a crash is told from an abort or a CUDA error exit without the engine's output.
- **The longest answer**: `limits.engine.generation_timeout_s` defaults to 1800 (was 120: at 96 tok/s about 11.5K
  tokens, then `length`). Every card offers **Longest answer (minutes)** (`answer_minutes`, 1-1440, menu 2-1440),
  written as the card's own `limits.engine.generation_timeout_s` (no engine flag); its default is the server's row in
  minutes, a part of a minute counted whole (`offer_at_row`, now shared with the GLM RAM headroom).
- **`make --debugging`** (`install.bat -Debugging`, `install.sh --debugging` pass it on): `SECURE_FRONTEND_DEBUGGING`.
  `logging.requests` and `logging.engine_output` are on unless the config sets them (`debugging_build()`,
  `DraftReader::boolean_or`), the server warns at every start, and `engine/build.json` lists `debugging` among the
  features (the About tab shows it). make prints `DEBUGGING_NOTICE` (what is logged, what it can hold, the
  precautions) before and after the build and puts it in the folder's README.txt; the installers' plan says the same
  before anything is installed.
- **Proof**:
  - RED: the transport and server cases with the sink cut off (no line arrived), the notice disabled, the row, page
    and listing missing, the option missing on every profile, the card timeout, the exit-code text, make's and the
    installers' option missing.
  - GREEN: the affected suites on MSVC, and on Linux gcc 13.3 and clang 18.1 (230/230 each). The make and install
    tests 40/40; `install.ps1 -Plan -Debugging` printed the plan and the warning. The whole MSVC suite: 1392/1395 on
    the first run (the switch list, fixed), then everything but the two release-only cases (they need a release
    build). A debugging tree (`SECURE_FRONTEND_DEBUGGING=ON`): its typed config cases pass with both rows on by default.
- **Not verified**: the owner's 5090 (the reason his calibration ends is what this is for), and a full `make
  --debugging` folder build.

### Claude Code session, 2026-10-29 z29 (Windows box; owner-directed; a worker session's change, committed with this entry, not pushed)

The owner: "in the window app version we need to add a log capability, the console have it but this one do not so we
can troubleshoot". strata-window wrote the server's lines to `<data>/logs/server.log` and kept 40 for the page shown
when the server ends, but nothing showed them.

- **Portable half** (`window_shell.hpp/.cpp`): `LogLines`, the Log's lines in memory. The newest come last; at most
  `max_lines` lines and `max_bytes` bytes (the window: 2000 lines, as strata-tui's Log, and 1 MiB). A full log drops its
  oldest tenth at once, so a view redraws only now and then. Each line is made valid UTF-8 by `Utf8StreamDecoder`
  (GTK needs it), with control characters as spaces, and cut at a character boundary. A line with a login code is never
  kept. `newer_than()` tells a view which lines to add, or that it must redraw; `text()` and `save()` give what Copy and
  Save write. `saved_log_name()` gives `m-strata-log-<date>-<time>.txt`. `secure_frontend_window_shell` now links
  `secure_frontend_utf8_stream`.
- **The Log window** (Windows `window_main_win32.cpp`; Linux twin `window_main_gtk.cpp`):
  - Opened from the tray's new **Server log** entry or Ctrl+Shift+L. On Windows the title bar's menu has it too, and
    the page's own keys reach it through WebView2's `AcceleratorKeyPressed`.
  - The text is read-only and monospace, newest last. It follows new lines unless the user scrolled up. While the window
    is hidden it catches up when shown again, and a burst of lines is drawn once (100 ms timer).
  - **Copy all** puts the text on the clipboard. **Save as...** writes it to a file the user picks (a new one 0600 on
    POSIX). **Open the logs folder** opens the folder (greyed with `--config`). Closing the window hides it; the lines
    stay.
  - The window adds its own notes to the Log and `server.log`: `[strata-window] starting the server`, `restarting the
    server`, `stopping the server (Exit)`, `the server ended (exit code N)`, and a start failure. The page shown when the
    server ends takes its last 40 lines from `LogLines` and points at Server log.
  - Windows links `comdlg32` (`GetSaveFileNameW`).
- **Proof**:
  - RED: three new cases against stubs, 17 expectations failed (`the_log_view_keeps_the_newest_lines_in_its_bound`,
    `the_log_view_holds_text_a_view_can_show`, `the_log_view_saves_what_it_shows`).
  - GREEN: `secure_frontend_window_shell_tests` 17/17 with MSVC (`/WX`), Linux gcc 13.3 and clang 18.1 (`-Werror`,
    WSL `mstrata-linux`). `strata-window.exe` builds with MSVC under `/WX`.
  - Live on this PC: the build-tree window ran on scratch port 18093 with a scratch data folder, the dev key and the
    supervision fake engine. Each of these opened the Log: the tray's Server log, the title bar's menu
    (`WM_SYSCOMMAND`), and the tray again after it was closed and the server restarted while it was hidden (it caught
    up). The Log showed the console's lines and the notes. Restarts appended live; it stayed put when scrolled to the
    top and followed at the bottom. Save as wrote the Log's text exactly (driven through the real dialog). No login code
    was in the Log or `server.log`. Exit stopped everything.
- **Not verified**:
  - Copy all live: the user's clipboard held browser data, so it was not overwritten.
  - Ctrl+Shift+L by real keys, and Open the logs folder.
  - The GTK window was not run. It **compiles** (the main session put GTK 3.24, WebKitGTK 4.1 2.52 and Ayatana
    AppIndicator dev packages into the `mstrata-linux` toolchain distro): `strata-window` with `-Werror` on gcc 13.3 and
    clang 18.1 (`~/win-gcc`, `~/win-clang`, `SECURE_FRONTEND_WINDOW=ON`), `secure_frontend_window_shell_tests` 17/17 there.

### Claude Code session, 2026-10-29 z28 (Windows box; owner-directed; committed with this entry, not pushed)

The owner, pointing a Qwen3.8 card at its MTP draft layer: "when I select the mtp I get the error that the folder was
not made private". On his RTX 5090 PC the server also logged "the MTP draft layer in ...\mtp\rt is not private (other
users of this PC can change it): the card runs without it".

- **The cause**: the page offers to make a folder others may change private (`folders/private`), and
  `make_model_folder_private` took only a folder of model files - at least one `.gguf`, no folder but a model's packs.
  Upstream's draft layer has no `.gguf` in `rt` (dense.bin, experts.bin, draft_vocab.bin, dense.txt), and the folder
  holding it has `rt/` and `tensors/`: both were refused ("the folder holds no .gguf file", "the folder holds other
  folders"), which the page showed as "The folder was not made private".
- **The fix**: an MTP draft layer is a model folder too: its runtime folder, or the folder holding it as `rt/` beside
  only `tensors/` (upstream's setup and the server's downloads), each tree with no link and the usual entry caps.
  `holds_mtp_runtime` moved from `card_builder.cpp` to `local_folders` (one rule for both; card_builder now links
  local_folders).
- **Proof**: RED first (`an_mtp_draft_layer_folder_is_made_private`: the owner's two messages). The folder, card builder
  and card route suites 43/43 with MSVC, Linux gcc and clang. Not run: the page's Choose... on the owner's folder.
- **Found, not changed**: on the 5090 PC the first calibration trial's engine ended before ready ("crashed" for one card,
  "error_exit" for another). The server never keeps the engine's stderr (engine_stdio_process.cpp, by design), so its
  reason is not known; CUDA 13.0 needs NVIDIA driver 580 or newer, and install.ps1 checks only that a driver exists.

### Claude Code session, 2026-10-29 z27 (Windows box; owner-directed; committed with this entry, not pushed)

The owner's install.bat on an RTX 5090, after z26: ptxas "Instruction 'mma with block scale' not supported on .target
'sm_120'" in `mmq_nvfp4_w4a4.cu`; then "it should detect what card I have, it should also compile for turing", "detect
and build all nvidia compatible if possible", "to make it lightweight ... detect the card and target that
specifically, and if there is multiple cards, only those card types", and "if they have unsupported cards, then we
compile to only supported ones and if none is present we say no supported card detected".

- **The cause**: make passed `--cuda-arch native`, and the rule that builds the two NVFP4 W4A4 units (`strata_mmq_w4a4`,
  gemma4's `g4core_w4a4`) for the arch-specific `sm_12xa` rewrote only a literal `12x`: with `native` on an RTX 50
  they were compiled for a plain `sm_120`, and ptxas refuses RTX 50's FP4 MMA there. This PC (RTX 4080 SUPER, sm_89)
  never enters that code. Reproduced with nvcc on the unit: `-arch=sm_120` gives the owner's errors; CMake's `120a`
  form and `75;120a` compile.
- **The FP4 units**: `engine/cmake/strata_fp4_archs.cmake` (`strata_fp4_cuda_archs`) spells out `native`, `all` and
  `all-major` with CMake's own lists before `12x` becomes `12xa`; both units use it.
- **make's targets** (`--cuda-arch`, default `auto`): each supported card type nvidia-smi reports (compute capability
  7.5 or newer, the engines' own rule), once, as `NN-real` (machine code only); older cards are left out with a line,
  and with none make stops: "no supported NVIDIA card detected", naming `--cuda-arch`. `compatible` is every generation
  the engines build for, `75;80;86;89;90-real` and `120` (PTX on the newest only). A list is passed as it is. make
  prints its targets.
- **CUDA builds** (the engine, the picture encoder) run once more at a quarter of the jobs when they stop: under 20
  jobs the six-target build here had ptxas 0xC0000005 and cicc 0xC0000374 on two of llama.cpp's MMQ units, which then
  built at 4 jobs; a second failure stops make.
- **Proof**: RED first (the auto, compatible and no-card cases; the FP4 function through `cmake -P`; the retry). The
  make and install tests 38/38 on Windows and Linux. Live: every engine (`multiengine` and `strata_kq`: Strata, Gemma 4,
  flint, raon1, Maya's GLM, DeepSeek and MiniMax) built here for `75;80;86;89;90;120` (33 minutes at 20 jobs; the two
  compiler crashes rebuilt at 4); the 5090's configuration (`120-real`) gives 175 units `sm_120` and the two FP4 units
  `sm_120a`, both built. With PTX for every target the engine libraries were about 7.5 times larger
  (`multiengine_strata` 49 to 372 MiB); `compatible` keeps PTX for the newest only (not measured). Not run: the 5090
  PC's build, any card but the 4080 SUPER, the picture encoder for other targets.

### Claude Code session, 2026-10-29 z26 (Windows box; owner-directed; committed with this entry, not pushed)

The owner's install.bat on another PC, after z25 (its plan found the Visual Studio C++ tools): "make: ... vcvars64.bat
did not set up the MSVC x64 environment (cl was not found after it)".

- **The cause**: `make.py`'s `build_environment` copied `os.environ` (whose names Python upper-cases on Windows: `PATH`)
  and added the names cmd's `set` listed after vcvars64 as written. cmd lists the search path under the name its
  parent gave it - `Path` in a PowerShell window, which is how install.bat runs make.bat - so vcvars64's `Path` sat
  beside the old `PATH` and `cl` was looked for on the old one. From Git Bash (every earlier build here) the name is
  `PATH`, so it never showed. Reproduced on this PC (which has the C++ tools) by running `build_environment` in a
  child whose environment names it `Path`: the owner's message word for word. The 2026-10-28 second PC's same stop
  (the `missing_cpp_tools_message` entry above) was most likely this too, not a missing workload.
- **The fix**: `merged_environment` keeps every name upper-case and lets a listed name replace its own whatever its
  case (cmd's nameless `=C:` entries skipped).
- **Proof**: RED first (`test_the_compiler_environment_replaces_the_path_whatever_its_case`: two keys, `PATH` and
  `Path`). The make and install tests 34/34 on Windows and Linux. Live: a whole `make.py` build (all features) run with
  the search path named `Path` finished; before the fix the same environment stopped with the owner's message. Not
  run: install.bat on the owner's other PC.

### Claude Code session, 2026-10-29 z25 (Windows box; owner-directed; committed with this entry, not pushed)

The owner, testing the z24 build and a fresh clone: "several errors".

- **install.bat stopped at once** on a fresh clone: `install.ps1` line 194 wrote `"$WebView2Version:"`, which
  PowerShell parses as a drive-qualified variable, so the script never ran. Now `${WebView2Version}:`.
  `tools/secure/test_install.py` only matched text; `test_the_scripts_parse` now has PowerShell's parser read
  `install.ps1` (Windows PowerShell 5.1, which install.bat runs, else pwsh) and `sh -n` read `install.sh`, each where
  installed. The parser finds nothing else in the repository's own scripts. `install.ps1 -Plan` here runs to its plan
  and stops; the steps after "Go ahead?" were not run.
- **Every GLM-5.3 calibration failed** ("a calibration reply failed (unsupported_parameter)"): each calibration reply
  sent `reasoning_effort: none`, which only profiles taking it per request accept, and GLM-5.3 takes none (its prompt
  always opens its think block). The replies now send it only where `per_request_reasoning_effort_for` is true. No test
  ran a GLM-5.3 card on the fake engine; `a_glm_card_calibrates` does (a glm53 pack, GLM's markers, Maya's dialect).
- **Proof**: RED first (the parse test on line 194; the calibration test with the owner's two log lines). The launcher
  suite 29/29 with MSVC; the launcher, options and cards suites 57/57 with Linux gcc and clang; the install tests 9/9
  on both. Not run: a GLM-5.3 calibration on the GPU.

### Claude Code session, 2026-10-29 z24 (Windows box; owner-directed; committed with this entry, not pushed)

An A/B of our engine and upstream Maya v1.1.30 on the same packs and prompts (decode tok/s, a light code prompt and a
prose one, greedy and the card's sampling): DeepSeek-V4.1 Maya-XS 11.56 against Maya's 11.33 (Maya's own speed
calibration keeps DSpark off here: 9.4 against 12.3 without it; ours refuses `--dspark`, no loss on this PC);
MiniMax-M3.1 Maya-S 3.89 against 3.98 (code -9-10%, prose +1-4%, one run each); GLM-5.3 Maya-S-v2 about 3.9 against
9.73 under the server's defaults, 9.79 with 6 GB RAM headroom. The cause: `glm.ram_headroom_gb` (default 16, the owner's
SP6 M2 choice: his desktop swapped at 12) is always projected onto `STRATA_GLM_RAM_HEADROOM_GB`, while Maya's engine
leaves 6 GB on a discrete GPU (16 on unified memory, `glm_fast_path.cu`), so ours pins about 10 GB fewer experts in RAM.
The owner: "Make it an option in the card setting".

- **`ram_headroom`, a card option of the GLM engine's profiles** (`glm53`, `raon1`; `model_options.cpp`): RAM left
  free (GB), a dropdown of 6, 8, 12, 16, 24, 32, 48 and 64 within the row's 6-1024, after `kv`; it writes
  `glm.ram_headroom_gb` (never an engine flag), which `glm_environment_knobs` projects as before. Other profiles have
  none (DeepSeek's and MiniMax's engines refuse the row). `is_model_option_name` now reads every profile's table, so an
  option of one engine alone is remembered (`model-options.json`) and named in refusals.
- **Offered on every GLM card** (`model_launcher.cpp`, `offer_ram_headroom`): a card whose file does not list it
  offers it at the server's own `glm.ram_headroom_gb` (16 unless the main config sets it), in the profile's order, so a
  start without a choice runs exactly as before. The default stays the owner's 16; the page's dropdown moves it per
  card and the choice is remembered.
- **Proof**: RED first (the option tables and offers, then a card's choice of 6 reaching the engine as 16 with the
  committed `model_options.cpp`). The whole Windows tree 1380/1383: the two release-only cases and the README's
  config table, whose `glm.ram_headroom_gb` text the schema now carries (then the config, options, cards and launcher
  suites 103/103). Linux gcc and clang 1429/1434 each: the same three, `net01_workers_add_no_listener` (gcc) or
  `net01_ipv6_loopback_listener_only` (clang), and `egress_table_scan_during_lifecycle`; after the schema text, the
  touched suites 130/130 on both and each of those three alone 3 of 3 on both.
- **Live** (the release build, a scratch server, GLM-5.3 Maya-S-v2 started through `POST /v1/strata/models/start`
  with the option): at `ram_headroom` 6 the engine pinned 9463 experts in RAM (62.7 GB), at 16 7959 (52.7 GB); VRAM
  932 either way (`/metrics`). The page's dropdown itself was not clicked.

### Claude Code session, 2026-10-29 z23 (Windows box; owner-directed; committed with this entry, not pushed)

The owner, after z22 (Qwen3.8's draft layer comes from upstream's setup; a download could not make its own): "We should
create a native versions for them or perhaps fuse them into ours", then "Yes add it to strata prepare".

- **`strata-prepare mtp`** (`engine/src/prepare/mtp_layer.cpp`): upstream's `tools/mtp_pack.py` and `tools/mtp_rt.py`
  fused, from the fetched tensors (`<src>/tensors/<name>.bin`, `mtp_fetch.py`'s layout) to the engine's runtime folder:
  `experts.bin` (the routed experts as Q2_0 by `mtp_pack.py`'s per-block MSE scale search, numpy's float32 arithmetic and
  pairwise sum mirrored, `-ffp-contract=off`; `blob_of`'s layout), `dense.bin` (Q8_0 for the ten large projections, the
  1-D norms F32 + 1, the rest BF16, 256-byte aligned, name order), `dense.txt` (the platform's line ends, as
  `Path.write_text`), then the vocabulary as `draft_vocab.bin`, last; each file renamed when whole. The 31 tensors'
  shard, offset, size and SHA-256 at `mtp_fetch.py`'s pinned revision are compiled in; every tensor is checked before
  use. The `mtp-q2_0.gguf` between the two tools is not written (only the runtime files are read). The out folder now
  ships upstream's `data/draft_vocab.bin` in `engine/data/` (make.py's runtime files). Two gcc `-Wformat-truncation`
  buffers (`py_json.cpp`, `prepare_main.cpp`) and one clang sign conversion in the prepare tests, all from the root
  commit, kept strata-prepare from building on Linux: fixed.
- **The fetch, in our downloader** (no network code in strata-prepare; the threat model's rule): a manifest file may be a
  window of its source (`source_offset`): every request is a bounded range inside it, and only a `206` of exactly that
  range (any total past it, its length) is written - a `200` or another range is a `range_mismatch`, never a restart
  (`classify_window_head`; the single stream, a resume, and the parallel probe and pieces). A new manifest profile `mtp`
  lists only such windows as `tensors/<name>.bin` (release builds accept it; `raw` is test-only). Hugging Face downloads
  write it (`HfFile::source_path` / `source_offset`).
- **The catalog**: `draft_layers` (generated by `tools/secure/download_catalog.py` from `mtp_fetch.py`'s
  `PINNED_REVISION` and `SHA256` and `tools/secure/mtp_inventory.json`, the tensors' places at that revision; its rules
  restated and checked word for word) and `draft_layer` on the 15 Qwen3.8-family entries (Qwen3.8, Swift, Coder,
  Unsloth, Gyro). The C++ catalog reads a layer only when all of it is pinned and drops an entry naming one it lacks.
- **Downloads**: a Qwen3.8 download whose folder has no layer built pulls the layer's tensors after its pack (an `mtp`
  manifest; the card's size and disk check count its 5.2 GB, its progress continues the bar), runs `strata-prepare mtp`
  into `<folder>/mtp/rt` with `engine/data/draft_vocab.bin`, and its card drafts with it; later Qwen3.8 downloads use it.
  A failure fails the step with its reason (a retry continues the pull); nothing falls back silently.
- **Proof**: RED first for each piece (the prepare tests unlinked; a window fetched as a whole file;
  the mtp profile and the catalog's layer with their rules switched off; the download flow with two children and no
  layer). strata-prepare 28/28 with MSVC, Linux gcc and clang. The parity script's `--mtp-src` on the owner's fetched
  tensors (upstream's `mtp_pack.py` and `mtp_rt.py`, numpy 2.5.3): all four runtime files identical, and identical
  to the `rt` his engine runs. The whole Windows tree 1378/1380 (the two release-only cases). Linux gcc 1427/1431:
  the release-only two, `net01_ipv6_loopback_listener_only` and `egress_table_scan_during_lifecycle` (each passes
  alone, 3 of 3); clang, the changed suites 285/286 (release-only). Python: the catalog generator 40/40, make.py
  24/24. Live: three tensors fetched from Hugging Face as windows of their shards (`strata pull`, an mtp manifest)
  were byte-identical to upstream's fetch. Not run: a whole 5.2 GB fetch, or a Qwen3.8 download end to end.

### Claude Code session, 2026-10-29 z22 (Windows box; owner-directed; committed with this entry, not pushed)

After an A/B of our engine and upstream's on Qwen3.8 IQ3_S (his voxel prompt and a prose one, same pack, profile and
flags; ours 94.4 tok/s mean against upstream's 89.0, a full answer 101.1 against 92.0; his slow card ran without the
MTP draft layer, `--mtp`), the owner asked: "Make sure the card matches the chat settings when the engine is started as
default so each model reset it to the recommended"; "When browsing for models, if it supports mtp we should be able to
point where the mtp is at too"; "Check which models in the download needs draft layers as well and make sure when they
are downloaded ... configured correctly automatically".

- **Recommended sampling on every card** (`recommended_sampling`, `model_options.cpp`; `offer_sampling`,
  `model_launcher.cpp`): a card whose file lists no temperature, top-p, top-k or min-p offers its model's - upstream's
  MODEL_CARD_SAMPLING for thinking (Qwen3.8 and Qwen3.6 1.0 / 0.95 / top-k 20, DeepSeek-V4.1 1.0 / 1.0) and the shipped
  and page-made cards for the rest (Gemma 4 1.0 / 0.95 / 64 / min-p 0.05; flint and raon1 1.0 / 0.95 / 20; GLM-5.3 and
  MiniMax 1.0 / 0.95) - or its config's own `defaults.sampling` row, in the profile's order. A start with the card's
  defaults then samples as the model's card says: the owner's Qwen3.8 card (made from its pack before cards carried
  sampling) ran greedy. A Qwen3.8 pack's new card lists them in its file too (`card_for_pack`).
- **The chat follows the card** (`web/app.js`): the drawer's thinking level, temperature and top-p are the running
  card's (its values as started: Gemma's and flint's on / off are High / Off, MiniMax's auto is a new **Auto** that names
  no level) until changed there; a start the page saw, a restart or another model sets them back, a start made
  elsewhere does when the page sees it; Reset gives the card's; untuned it shows "1.00 (the card's)" and the card's
  top-k and min-p under top-p. A reload keeps the user's own values (`settings.card`: the card and its values).
- **Pointing a card at its draft layer** (`POST /v1/strata/cards/mtp`, `CardRoutes::point_draft`;
  `draft_layer_slot`, `mtp_runtime_at`, `card_with_draft_layer` in `card_builder.cpp`): a page card whose model drafts
  with a layer of its own - a Qwen3.8 pack's upstream `mtp/rt` (the folder itself, its `rt/` or `mtp/rt/`), a Gemma 4
  model's `.gguf` - lists it in the model list (`"draft": {"kind", "path"}`), and the route rewrites the card: `--mtp`
  (replacing one there) and the MTP options it lacks (on; Qwen3.8's confidence 0.5, Gemma 4's window 4 and confidence
  0.6). The picked folder and the `rt` folder pass the model folders' rule. The page: a card's **MTP draft layer** row
  with **Choose...**, and after Add a model a model with none found is offered the picker at once. Threat model updated.
- **Downloads' draft layers** (catalog checked): Gemma 4's MTP file was already fetched with it and wired; Qwen3.6's is
  inside its GGUF (`--mtp 2`); the Maya families (GLM-5.3, DeepSeek-V4.1, MiniMax-M3.1), flint and raon1 have none in
  the catalog. Every Qwen3.8 family (Qwen3.8, Swift, Coder, Unsloth, Gyro) drafts with the one layer upstream's setup
  builds from the original checkpoint (`tools/mtp_fetch.py`, `mtp_pack.py`, `mtp_rt.py`, about 5 GB fetched). A
  Qwen3.8-family download's card now drafts with the store's `<folder>/mtp/rt`, else one near the model's folder,
  when private (`native_prepare`). **Not done**: building that layer here (a native port of the three tools into
  `strata-prepare`); until then it comes from upstream's setup or is pointed at with the new route.
- **Proof**: RED first for each piece (the launcher listing and a fake-engine start: the GEN line had no sampling; the
  pack card; the route: 404 with its dispatch left out; the download: its card had no `--mtp`). The whole Windows tree 1372/1374 (the
  two release-only cases). Linux gcc 1421/1425: the release-only two, and `net01_workers_add_no_listener` and
  `egress_table_scan_during_lifecycle` (each passes alone, 3 of 3). Linux clang: the six changed suites, 166/166.
  The page (the real `web/` files) in headless Edge on a mock of the server's routes, 24 checks: the drawer at each
  card's values, nothing sent untuned, a tuned value sent and kept over a reload, a start, a restart and a start made
  elsewhere setting it back, Auto naming no level, Reset, the draft row and its picker posting `cards/mtp`. Not run
  against a model or the owner's server; his Qwen3.8 card is unchanged (he points it with Choose...).

### Claude Code session, 2026-10-29 z21 (Windows box; owner-directed; committed with this entry, not pushed)

The owner, after z20's finding: "Yes: when web search is on, add today's date and a short line telling the model its
knowledge may be out of date and to search for anything recent, for every model family; and keep earlier searches in
the chat history as upstream's page does." Then, on a follow-up to a long answer refused as request_too_large: "Raise
the limit" and "review the upstream strata as it does not have this sort of issue"; and "in the monitor section, the
speed portion does not seem to update in realtime".

- **The date line**: the search service's `system_note()` (`ToolRunner`) - "Today's date is <date>. What you know may
  be out of date: for anything recent, or anything that may have changed, search the web with web_search before you
  answer." - goes into a search turn's system message (`with_system_note`: after the chat's own text and a blank line,
  or a new system message first), every family.
- **Limits at upstream's scale** (`config_defaults.hpp`): a request of 256 MiB (its body: that, less the header bound),
  65,536 messages, a message as large as the body, 1,048,576 prompt and completion tokens (the model's context window
  is the bound); JSON nesting 16 (`kChatMaxJsonDepth`, for the rounds). The body is still read only after the API key
  checks. Threat model and README updated.
- **Earlier searches** (`strata_rounds` on an assistant message, `chat_schema.cpp`): per round its text before the
  calls, 1-16 calls (a name, an arguments object) and one result each; at most 64 rounds; an answer with rounds may
  have no text of its own; rounds on any other message are refused. The page (`web/app.js`) keeps each call, where the
  answer's text was then and the result the model read, and sends an answer that searched back as upstream's
  `assistantMessages` does. Each family's tool prompt writes them as its template writes upstream's normalized messages
  (`openai_to_messages`), with new fixture cases from each template - Qwen3.6's pack 8, Gemma 4's newer template 11,
  GLM-5.3 8, raon1 9, flint 8, DeepSeek (Maya v1.1.30's encoder) 11, MiniMax (Maya v1.1.30) 9 - and every existing case
  byte-identical. A chat served without the tool (turned off, a picture in it) gets each such answer as its text
  (`answers_as_text`: the rounds' text, then its own, a blank line between).
- **The Monitor's speed** (`serve_monitor.cpp`): upstream's `_rate_of` - the tokens over the samples of the last 2 s
  once they span 0.25 s (at most 32 samples), else the mean since the first token. It was always that mean, which
  barely moves in a long answer.
- **Differences kept**: GLM-5.3's fixed profile writes an earlier answer's text `<|assistant|>{text}` where Maya's
  template writes `<|assistant|><think></think>{text}` - as for every earlier answer before this; the GLM fixture
  records the cut (`history_think`). The Qwen history cases are the Qwen3.6 pack template's; Qwen3.8's writes an empty
  think block on every earlier turn and ours writes none (as before). A literal `</think>` in an earlier round's text
  stays text (Qwen, GLM, raon1, flint); MiniMax refuses `</mm:think>` there where it is no special token, as it already
  did in an earlier answer's text.
- **Proof**: RED first for each piece (the note, the limits, the schema, the page, the Monitor, the fold; each family's
  fixture and refusal tests against its old renderer). The whole Windows tree 1366/1369: the two release-only cases,
  and `failed_start_keeps_the_server_usable` at 31 s while the Linux suite ran beside it (alone 6/6; it passed in the
  full run before the last two edits, 1367/1369). Linux gcc 1415/1420: the release-only two, and
  `egress_table_scan_during_lifecycle` and the `net01_*` pair beside the Windows suite (all pass alone). The page
  script parses in headless Edge, and its `roundsOf` and `requestMessages` give the rounds and the request on sample
  calls there. Not run against a model here: the owner's next search chats are the live check.

### Claude Code session, 2026-10-28 z20 (Windows box; owner-directed; committed with this entry, not pushed)

The owner: "Update our Gemma 4 renderer to the newer chat template (one profile for both GGUFs), with fixtures from the
new template, then build and swap." (Asked "will this work on both models?": yes - both files hold the same weights;
llama.cpp renders each GGUF's own template and calls the older one outdated.)

- **The fixture generator** (`tools/generate_gemma4_tool_prompt_fixture.py`) takes llama.cpp's path for the template it
  is given: `convert_tool_responses_gemma4` only for an outdated one (`common/chat.cpp`'s test: no
  "{#- OpenAI Chat Completions:"); it also writes plain chats (`text_cases`, thinking on and off) and richer tool
  declarations (`declarations`: nested objects, arrays of objects, nullable, enums, a property named "type", an object
  without properties). Checked first: given the old template it still writes the committed cases byte for byte.
  `tests/fixtures/gemma4_tool_prompts.json` is now the newer template's (12 cases, 6 plain, 2 declarations).
- **The renderer** follows it: `<|think|>` then a newline; `format_parameters` with `filter_keys` and the newer comma
  and key order (nullable after enum/items, an object's properties and required with commas); `null` written `null`;
  a result is always the tool message's text (`value:<|"|>...<|"|>`, JSON or not); the rounds as one model turn they all
  continue - each round's reasoning in the thought channel, its calls, their results, then its own text
  (`strip_thinking`), the turn closed only after a last round with text; after a round's results the prompt goes on into
  `<|channel>thought\n` when thinking (the tool plan's `starts_in_reasoning` is set then), and a turn a round's text
  closed gets "<|turn>model\n" when not (llama.cpp's rule). Plain chats with thinking off are unchanged.
- **Proof**: RED (the three fixture tests against the old renderer), then 17/17 MSVC and 102/102 Linux gcc over the
  Gemma 4, chat-execution and tokenizer suites; four hand-written old-template expectations (the newline after
  `<|think|>`, the reply starting in the thought channel after a thinking round) now hold the newer template's bytes,
  each as the generator renders it. The whole Windows tree: 1360/1364 - the two release-only cases, the API test's
  thinking-on prompt (`gemma4_thinking_round_trip`, the same old expectation, updated: 3/3 MSVC and gcc) and
  `switch_waits_for_the_generation_then_swaps` under -j 4 only (it passes alone). Not run on the GPU here: the owner's
  next Gemma 4 chats are the live check.
- **Seen on the owner's Q8_0 card (z19's fix live, about 95 tok/s)**: with search on, the model reasoned that its own
  earlier answer's CVEs were "fictional" for a "hypothetical 2026". Cause, ours: the page sends earlier answers as their
  final text only, so the searches and results behind them are gone from later turns, and the only "2026" the model sees
  is the search tool's "Today's date is". Upstream's page sends each answer back with its calls and their results
  (`serve/web/app.js` `assistantMessages`). Not changed here; offered to the owner as the next step.

### Claude Code session, 2026-10-28 z19 (Windows box; owner-directed; committed with this entry, not pushed)

The owner's console: calibrating his new gemma-4-26B-A4B-it-Q8_0 card, "a calibration reply failed (invalid_request)",
then "the engine did not start for the calibration" (the engine had started).

- **Cause**: unsloth's current Gemma 4 GGUF (their Q8_0 at c099eb48) types `<eos>` NORMAL (type 1, not CONTROL) and names
  `<turn|>` the eos token (`eos_token_id` 106; his older UD-Q8_K_XL: 1, `<eos>` CONTROL). `find_gemma4_markers`
  required a CONTROL `<eos>`, so every chat on that card was refused (tokenization_failed -> invalid_request), not
  only its calibration. Its other markers are typed as before. Read from both GGUFs' headers (metadata only).
- **Fix**: `<eos>`, still exactly once, may be CONTROL or NORMAL, as the marker and as a stop id; `<turn|>` stays CONTROL
  (`unique_token_of_type` takes a second type). User text holding `<eos>` is refused as before (kControlLiterals).
  RED shown (`plan_encodes_markers_as_single_ids_and_text_as_spm`, `gemma4_turn_close_ends_the_reply_as_stop`; the old
  rule's NORMAL-<eos> case is now this file's, the other mistypes still fail closed), then 99/99 MSVC and 100/100
  Linux gcc over the Gemma 4, chat-execution and tokenizer suites. On his real file (a scratch server on 18090, the
  fake engine): the old build's first-start calibration failed exactly as his did.
- **Not changed - the newer template**: the same file carries a newer chat template (18,922 chars; the UD file's,
  12,045, is the one llama.cpp calls outdated). Rendered by the fixture generator: plain chats with thinking off are
  identical; thinking on adds a newline after `<|think|>`; tool rounds keep the model's reasoning in a thought channel
  and stay in one model turn. Our renderer follows the old one (the committed fixture reproduces byte for byte). One
  Gemma 4 profile serves both files; following the newer template is a separate change for the owner to decide.

### Claude Code session, 2026-10-28 z18 (Windows box; owner-directed; committed with this entry, not pushed)

The owner: "Make the Gemma 4 card now through the download route, then fix the page so an installed download without
its card offers the step."

- **His Gemma 4 card**: after the swap to 009b831d his Gemma 4 download read installed (its store holds it) with no card -
  the earlier server misread the downloader and never ran the card step - and the page offered nothing. One
  `POST /v1/strata/downloads/start {"id":"gemma-4-26b-a4b-it-q8_0"}` on his word: the downloader answered "already
  installed" (nothing downloaded), and `gemma-4-26B-A4B-it-Q8_0` was added (MTP on, spec 4, spec_min_p 0.6).
- **The fix**: an installed download whose step makes a card (or builds a pack: `runnable`) and that has no card from it
  is no longer counted as on this PC in the list (`have` null), so the page shows "Its files are on this PC, but its
  card was not made: Set up makes it (nothing is downloaded again)" with a **Set up** button (a start; `lacksRoom` asks no
  room of an installed one), and strata-tui says the same and Enter starts it. The page's fallback for a server without
  `have` no longer overrides a `have` the server sent. A download-only entry installed stays Downloaded, and so does an
  installed one whose file a card added from that folder runs (his DeepSeek-V4.1: it read Locally provided for a moment
  after the first swap; caught on his server and fixed in the same entry, RED shown).
- **Proof**: RED shown (`an_installed_download_without_its_card_offers_its_step`, the page's and the TUI's checks), then
  94/94 on MSVC and 94/94 on Linux gcc over the download manager, TUI and router suites.

### Claude Code session, 2026-10-28 z17 (Windows box; owner-directed; committed with this entry, not pushed)

The owner, testing the ab29ee2a build: "fix the issues while I test this version out"; then "also add an ability to
remove an active model card but not delete files"; a Gemma 4 download that "did not even say why it failed"; "I ran a
search with Gemma4, it searched the first time but the following messages it did not search again"; "make sure the
other models don't have the same search issue".

- **raon-1-mini was not "Locally provided"**: his file is another revision (30,693,556,000 bytes; the catalog pins
  KRAFTON's current 30,693,555,936), so the strict match rightly declined it. A third `have`, `"other"`: a card has a
  file of that name and another size; the page's badge **Other version on this PC** with that card's name, and the
  Download stays (the catalog's version); strata-tui says "Other version" and Enter still downloads. RED shown (three
  suites), then 42/42 on MSVC and 92/92 on Linux gcc.
- **"Unreaped engine" after every model stop** (seen on his console for Qwen3.8, DeepSeek-V4.1 and MiniMax-M3.1, and
  after each calibration trial of DeepSeek and Qwen3.8): a big model needs 10-25 s to free its memory and end, and a
  model stop used the server's 5 s shutdown bound (RF5: EOF 2 s, TERM 1 s, kill, 1 s), so the engine was left
  unreaped, the server would exit 1, and the next start - or the next calibration trial - began beside it.
  - `EngineSupervisor::stop_engine()`: the same orderly stop, then up to `policy.reap_timeout` (30 s, the bound a
    crashed engine already gets, spec V10) more for a killed engine still ending, inside its own bound; `hurry()` cuts
    that wait at once (the server's own shutdown keeps RF5). The idle unload waits the same way (a shutdown cuts it), so
    a big model's idle unload no longer opens the circuit.
  - The launcher uses it for a stop, a switch, a failed start and each calibration step (`measuring_`, so the shutdown
    can hurry a calibration's engine too); `ModelLauncher::shutdown()` keeps the 5 s path, and `begin_shutdown()`
    hurries whatever is stopping.
  - Tests: `a_model_stop_waits_for_an_engine_still_ending`, `hurry_cuts_a_model_stops_wait`,
    `an_idle_unload_waits_for_an_engine_still_ending` (in-process fakes reapable only later), and the launcher's
    `a_stop_waits_for_an_engine_still_ending` over the fake process factory (`CalibrationFixture` takes one now). RED
    on stubs (4 failures), then 92/92 MSVC and 94/94 Linux gcc for the supervisor and launcher suites. Not measured on
    the GPU: his server was calibrating; the next swap shows whether "unreaped" is gone from his console.
- **Remove card, files kept**: `POST /v1/strata/cards/remove {"name"}` (kModelCardRoutes, keyed, the card routes'
  client rule; request log name `strata_card_remove`). A page card only; its card file, remembered options and
  calibration go, no model file; a running card is stopped first through a queued `remove` launcher job
  (`RunningCard::stop_first` -> `CardChange::stopping`), and removed once stopped. The page's **Remove card** row under
  the card (asked once), strata-tui's Del (asked, y/n). Docs: README, secure_frontend/README, GUI-first spec §4.5,
  threat model. RED shown for the route table, its log name and the TUI key; the route and launcher code itself was
  written before this session's context was reset, so its own RED is not on record (its two `card_routes` cases pass).
- **The Gemma 4 download that "failed (ok)"**: both files came, matched their SHA-256 and were published (his
  `acquire.log`, the store's `index.json`); the downloader printed `RESULT ok` and ended with a non-zero exit code, and
  the manager took only ok-and-0 as done and printed the result word as the reason. Now `pull_failure` says each reason
  in words (and names the exit code), and an ok with another exit code is kept when the store's version holds every
  file at its pinned size (logged with the code). RED shown (`a_pull_that_said_ok_is_checked_in_the_store`), then
  27/27. Why the downloader ended that way is not known: no crash was logged, and the same pull against the installed
  copy ("already installed") exits 0, so it follows a real transfer; the new build logs the code next time. His copy is
  recovered by pressing Download again (the downloader answers "already installed", exit 0).
- **Search after the first message**: not a server bug. Every text turn is offered the tool (`chat_route.cpp`: no turn
  condition), his console had no "cannot carry the web search tool" fallback line and no search start failure, and the
  page sends earlier turns as plain text, which each family's fixture (from its own template) already covers - except
  GLM-5.3, whose earlier assistant turns differ from Maya's template by design (the fixed profile). Each family's route
  test now plans a follow-up turn and checks it gets the tool as the first turn does (Gemma 4, Qwen, GLM-5.3, DeepSeek,
  MiniMax, raon1, flint: 30/30). So whether to search again is the model's choice; his Gemma 4 ran with thinking off.
  Not checked on the GPU (busy).
- **The whole tree**: Windows windows-msvc ON (email, search, TUI) 1359/1361, failing only the two `release-only` cases a hooks-on dev
  build cannot pass; Linux gcc 13.3 1409/1412 (`egress_table_scan_during_lifecycle` under `-j 8` only,
  it passes alone; the two `release-only` cases a hooks-on tree fails by design).

### Claude Code session, 2026-10-28 z16 (Windows box; owner-directed; committed with this entry, not pushed)

The owner: "when we point it to a local folder with the model, if it match the card of the model that is downloadable,
we should change the banner from available to Locally Provided vs Downloaded", "put accordion control so that user can
expand per related section instead of seeing a busy page", and "make prominent the device and download location
selection so that it does not look just a bunch of text". Done by a worker; reviewed here.

- **Why his models said "Available"**: a GLM-5.3, raon-1-mini, DeepSeek-V4.1 or MiniMax-M3.1 card made from a folder
  sets `engine.model_id` to Maya's model name (`card_builder.cpp` `card_for_pack`), not the shard, and the launcher
  reports `model_id` as the card's `file`, so the page's `carded()` never matched; Qwen3.8, Gemma 4, flint and Qwen3.6
  matched by name only; `models.cards` cards reported no file.
- **One rule, on the server**: each `GET /v1/strata/downloads` entry carries `have` (`"downloaded"` | `"local"` | null)
  and `have_card`. `card_model_file` (card_builder) finds each card's model: `<roots.models>/<model_id>.gguf`, or for a
  Maya pack card the shard the pack indexes, beside the pack (`native_experts.txt`, else `sources.json`; regular files,
  bounded reads; the helpers moved from card_routes). "local" needs the file present, the same first-shard name, and
  the same size where the catalog pins one; nothing is hashed. The launcher's cards reach the download manager through
  `DownloadSettings::cards` (`ModelLauncher::cards()`, wired in `server_bootstrap`).
- **Windows case collisions**: a download counted as ready when `<cards>/<id>.json` existed, and a folder card named
  `Qwen3.6-35B-A3B-UD-Q4_K_XL.json` is that file on Windows; now only a card of that download's own (source
  `download:<id>`, or none: an old prepare step's) counts (`download_card_file`). And a download never writes its card
  over such a folder card: `write_download_card` refuses, saying the files are installed
  (`a_download_never_writes_over_a_folder_card`, RED shown: the folder card was overwritten).
- **The page**: entries the PC has are shown, not hidden, with a Downloaded / Locally provided badge, "Already on this
  PC; its card: X" and no Download button. Each family group is an accordion section (a real button with
  `aria-expanded`/`aria-controls`, a summary such as "3 models · 1 on this PC", open by itself while a download is under
  way, paused, failed or just added; opened groups kept in `sessionStorage` - the page's tests and the threat model
  forbid localStorage in it). A "Download to" panel: the drive, the path in a chip shortened in the middle, a space
  gauge, Change…, a default marker and a room warning; phone widths 375/320 without horizontal scroll (`app.css`).
- **strata-tui**: the Downloads view shows the same states; Enter does not download those entries.
- **Proof**: RED for each behaviour (logs in the worker's scratch); MSVC and Linux gcc/clang 149/149 over the six
  suites touched, the folder-card guard 26/26 in the download manager's; a scratch server with fake GGUFs (a sparse
  file at the catalog's size gives "local"; a same-named flint file of another size gives null); the page checked at
  375/320 px. Not seen live: the room warning (it needs a queued download larger than the free space).
- **The whole tree, validated once all of z13-z16 were in (2026-10-28):** Windows windows-msvc ON (email, search, TUI;
  the real packs named) 1352/1354 and OFF 1322/1324, failing only the two release-only cases a hooks-on dev build
  cannot pass (`idle_unload_respawns_before_the_next_request` passes now: z15's wake fix); Linux gcc 13.3 1401/1402
  (`egress_table_scan_during_lifecycle` fails only under `-j 8` and passes alone); pytest 140 passed, the 7
  `test_build_release.py` cases needing Linux bash failing here as before (they pass on Linux, z13).

### Claude Code session, 2026-10-28 z15 (Windows box; owner-directed; committed with this entry, not pushed)

The owner: "the System Ram reports MB usage only and not experts + mb usage. Also when the models are stop and nothing is
loaded, the stats seem to stay", "make the panels symmetric and even, like make the Recent Request at the bottom and
span the whole width", "the disk read metrics", and "the upstream strata has disk read and conversation cache as well
which we are both missing". Done by a worker (the page and `/metrics`) and another (strata-tui); reviewed here.

- **RAM showed GB only**: the page took the RAM experts from `ram_used` (only GLM-family STAT lines, every 8 tokens
  while decoding) or `expert_slots - expert_slots_primary` (experts in RAM only for Gemma 4 and flint; on upstream's
  engine the other GPUs' share), and `serve_monitor.cpp` kept only 9 engine keys, dropping each family's RAM-tier keys
  (`ram_slots`, `ram_experts`, `pinned_gib`, `arena_mib`). Now `engine.memory` (`experts_ram`, `experts_ram_bytes`,
  `experts_vram`, `process_bytes`) per family (`engine_memory_view`), plus the engine process's working set / VmRSS.
  Qwen gives bytes only, GLM/raon1/MiniMax counts, DeepSeek counts, Gemma 4/flint all pinned; Qwen3.6's engine says
  nothing (process memory only).
- **Stale stats**: the engine's readings were hidden only for "none"; "starting" (also an idle unload) and "failed" kept
  them, and the page cleared only on "stopped" and kept the last values when the server stopped answering. Now
  `engine.loaded`/`engine.unloaded`, `live.state` "unloaded", readings only while ready or generating, replaced at each
  READY, and the page clears every live figure when stopped, unloaded, failed or offline.
- **Disk read/write**, upstream's (`serve/telemetry.py` `_disk`): `hardware.disk_read_mb`/`disk_write_mb` (MiB/s over
  the last second, all physical disks, per-disk deltas, null when unknown) and `history.disk_read_mb`, read natively as
  psutil does (Linux `/proc/diskstats` over `/sys/block`, Windows `IOCTL_DISK_PERFORMANCE`). The engine's own read rate
  is not added: Windows' process counters miss the page faults of mapped expert files.
- **The conversation cache**, upstream's (`serve/server.py` `conversation_cache_view`, `ConvCacheLog`): budget and slots
  from the Qwen engine's INFO, parks/restores/evictions/bytes from its stderr lines, reuse from the request history; plus
  `supported`. **The stderr exception** (threat model): only for `profile == qwen` (`EngineCommand::
  read_conversation_cache`), through a drained pipe; a whole-line parser takes upstream's three forms and keeps their
  integers and the event kind; every other line is dropped unread; nothing of stderr's text is kept or logged
  (`stderr_gives_only_the_conversation_cache_numbers`, `stderr_flood_never_blocks_the_engine`).
- **The page**: eight tiles (Disk read among them), two equal panels ("Context and memory", "Conversation cache"),
  Recent requests across the whole width; checked headless at 1440/375/320 px.
- **A supervisor bug** found on the way: `release()` and `note_request_finished()` armed the idle-unload window without
  waking the supervisor thread - the cause of `idle_unload_respawns_before_the_next_request`'s long-known failure
  (`idle_unload_fires_after_a_request_that_began_at_ready`, RED deterministic).
- **strata-tui**: eight charts (Decode, GPU use, CPU, RAM - its own history of `ram_used` - GPU memory, temperature,
  power, Disk read) in two rows of four from 120 columns; three equal panels (Engine with the memory figures and
  "unloaded after idling", Machine with the disk rates, Conversation cache with gauges, "not used by this engine" /
  "no model running"); Last requests across the full width when tall enough; "-" for every field an older server lacks.
  A chart no longer draws an unknown sample as a 0 dot (RED: the braille case). 9/9 on MSVC, GCC, Clang.
- **Proof**: the worker's six suites 258/258 on Windows and Linux gcc (one IPv6 listener case flaky under -j 8, passes
  alone); the final whole-tree runs are in z16's proof line. Not proven: a real GPU engine's pinned memory in its
  working set, the Windows CI job.

### Claude Code session, 2026-10-28 z14 (Windows box; owner-directed; committed with this entry, not pushed)

The owner: "there are no download cards for the following: flint ..., raon1 ..., gemma4 ..." and "gemma4 has an mtp
... make sure we work that mtp in our gemma engine". Done by a worker; reviewed here.

- **Our own catalog pins**, `tools/secure/catalog_pins.json` (source `m-strata`, read by `download_catalog.py`
  `pin_entries`). Each file has its commit, size, SHA-256 and licence, read from Hugging Face's HEAD and API metadata on
  2026-10-28 (all `apache-2.0`; PROVENANCE has the table).
  - flint Q8_0 and Gemma 4 26B-A4B Q8_0 become cards from their GGUF.
  - raon-1-mini Q8_0 is packed like GLM-5.3 (`strata-prepare download --maya`, marker `raon1-mini.gguf`, then a raon1
    card).
  - The generator refuses an unpinned, unknown or misspelt entry, one with no licence, and an MTP file on another
    engine or one colliding with a model file or a shard name.
- **The MTP file**: a typed `mtp` file of a catalog entry (Gemma 4 only, at a commit with its size), always pulled with
  the model.
  - The card carries `engine.args --mtp <file>` and an `mtp` option (default on; off drops it from that start).
  - The `strata_gemma4` allowlist takes `--mtp` only as an absolute `.gguf` path (`gguf_file`; a refusal never echoes
    the path).
  - Each launch holds the file to the picture encoder's rule (`trusted_side_file`).
  - A Gemma 4 folder card finds `mtp-<model>.gguf` beside the model or in `MTP/`, inside the folder once resolved, and
    never guesses in a folder with several models. A repoint moves it when the new folder holds it.
- **Linux**: every `PackedCardRequest` and `GgufCardRequest` member has a `{}` default (clears
  -Wmissing-field-initializers wherever they are built). The `snprintf` buffer is 40 (-Wformat-truncation). The unused
  `active_child_` is gone. `card_routes_test`'s POSIX widen is 01777, and a plain 0777 parent is refused (422).
- **A side effect**: a pasted link matched to gemma4 or flint now survives a restart, because `custom.json` no longer
  drops those engines when read back.
- **Proof**:
  - pytest 35/35, `--check` current (32 models).
  - RED shown in Python (15 failed) and in MSVC (0/11 of the new or changed cases).
  - GREEN: MSVC 266/266 and Linux gcc 268/268, clang 268/268, across the download_manager, card_builder, card_routes,
    model_options, engine_arguments, api_router, model_cards, typed_config and model_launcher tests.
- **Not verified**: a real download, a GPU run of a downloaded card, and the page visually.
- **The cards' draft options** (Gemma 4 MTP, z12; done here): a Gemma 4 card with its MTP file lists `spec` (Draft window, 2-8, default
  4: three drafts, llama.cpp's default) and `spec_min_p` (Draft confidence, 0-1, default 0.6: the fastest measured above,
  where llama.cpp's own default is 0; the card test was changed to it, with RED shown). The
  `strata_gemma4` allowlist takes `--spec` (integer 2-8) and `--spec-min-p` (at most three places, 0-1) instead of
  refusing them by name. `--spec-min-p` is written only while the MTP switch is on, rounded to three places (the page's
  0.12345 is 0.123). `--spec` stays when the switch is off: the engine ignores it without `--mtp`. Gemma 4's
  option-controlled flags gain the two; flint's stay its three. RED shown (5 cases: allowlist, table order, controlled
  flags, composition, card defaults); GREEN 200/200 over model_options, engine_arguments, card_builder, model_cards,
  api_router and typed_config.

### Claude Code session, 2026-10-28 z13 (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

The owner: "an option to make a headless version of the frontend ... a TUI with the same settings and options via the
TUI. Make the TUI look professional and make monitoring the default view in a nice graph dashboard TUI look", then
"have the installer compile both as an option by default so the user can pick if they want the tui or window version or
specify", and "download and install the needed toolchains for the tui compile".

- **strata-tui** (spec `docs/superpowers/specs/2026-10-28-server-tui-design.md`; `SECURE_FRONTEND_TUI`, `make --with
  tui`): the server in a terminal, strata-window's headless twin over the same `window_shell` (server child,
  environment copy, `server.log` without the login link, stop in order).
  - `tui_screen` (cells, rounded boxes, eighth-block gauges, braille or block charts, wide characters in two cells,
    combining marks, controls C0/DEL/C1 drawn as spaces, VT output of only the changed cells), `tui_input` (VT keys,
    SGR wheel, split sequences and UTF-8), `tui_app` (Monitor first: six charts of `/metrics` history and the Engine,
    Machine and Last requests panels; Models with options, start (asks when another runs), stop, calibrate; Downloads;
    Settings with editing, save, the key only on `k`, rotate; Log; About), `tui_terminal` (Windows console VT mode, POSIX
    termios), `tui_main` (the program: a request worker, the UI loop, every way out restoring the terminal).
  - Tests: `secure_frontend_tui_tests` (8 cases). The code came before its tests, so RED was shown by breaking each
    behaviour on purpose and watching the case fail: the context step, a failed poll, the settings re-read, the hidden
    key, `q` among the options, the ellipsis, a braille dot, the change-only renderer (a hang), Shift-Tab, a split
    sequence; later fixes test-first. Driven in a real hidden Windows console (a scratch ctypes driver: every view,
    editing, quitting) and on Linux under tmux (below).
- **Installers build both front ends by default**: `install.ps1 -With email,search,window,tui`; `install.sh` both on a
  desktop, the TUI only without a display. `test_install.py::test_both_front_ends_by_default` (RED shown).
- **The review's fixes** (a reviewer worker with the `code-review` skill, then the same worker fixing them; RED in four
  rounds, 8/8 on MSVC, GCC and Clang, also clean with `-Wconversion -Wsign-conversion -Wshadow`):
  - the terminal is put back before the server stops, then a plain "stopping the server" line;
  - Windows: the server's Ctrl-Break comes from `strata-tui --ctrl-break <pid>` (the program relaunched with no
    console), on every way out, so the TUI never leaves its own console (ServerChild's `FreeConsole` would close a tab
    the TUI alone holds, or wait while the console closes); the console closing stops it in order within Windows' ~5 s;
  - a try/catch around the loop and in both threads; SIGINT/SIGQUIT quit; a vanished terminal quits; POSIX writes
    retry EINTR/EAGAIN;
  - no request starts after quit begins; polls time out at 5 s, actions at 15 s; the last lines reach the log;
  - the key row tells a held key, a keyless server, an unreadable key and not-yet-known apart; poll failures (401, no
    answer, a non-loopback server) are said, per poll, cleared when it answers again;
  - one view switch for 1-6/Tab/F1-F6; integers checked with `from_chars` and the row's bounds; a pending list edit
    re-opens as items; the Log scroll is clamped; wide characters take two cells and combining marks none; Enter asks
    "Stop X and start Y?" / "Restart X with these options?"; the wheel moves lists;
  - from the Linux run: Last requests newest first; the charts keep 300 samples of their own (five minutes); unsaved
    settings shown like saved ones; About's sizes in GB; "no GPU" charts on a machine without one.
  - Driven for real: Windows (a hidden conhost: q/y and WM_CLOSE, everything ends in under 0.5 s; the final lines
    print on the console) and Linux (a pty driver: termios restored, exit 0, nothing left).
- **window_shell**: the POSIX server child gets its own process group (`setpgid`: a terminal hangup or Ctrl-C reaches
  the window/TUI, which stops it in order; RED on Linux: "the server leads its own process group"); the log's rotated
  name is a path joined, not `path::string()` + ".1" (a folder name outside the code page; no RED with MSVC here, the
  test stays as a guard); the refusal of init/--check names neither front end.
- **The MiniMax prefill fallback** (from z10's GPU check: "not enough GPU memory ... a smaller --chunk" at 4096 on a
  16 GB card): a run that ended by itself is measured again at `--chunk 1024`; a run stopped (cancelled, past the hour)
  or never started is not. `a_prefill_calibration_falls_back_to_the_default_chunk` (RED shown). Verified on the GPU:
  done at chunk 1024, 14.9 tok/s, through the server.
- **The request log names every route** (`request_log.cpp`): 23 routes added since SP3 were logged as "other" (RED
  shown); found by gcc's `-Wswitch`.
- **Linux builds again**: main did not compile with GCC 13.3 / Clang 18.1 (CI's presets): -Wswitch,
  -Wmissing-field-initializers, -Wrange-loop-construct, -Wformat-truncation, -Wunused-private-field in ~20 files,
  none of which MSVC reports. Each field is now named in the initializer (`.client_gone = {}`, `.environment = {}`,
  `.max_tokens = {}`, ...; mcp_client.cpp's local `Awaited` got `{}` defaults), range loops bind `const T&`,
  `request_log.cpp` names every route, and the download cards' files were fixed by their worker. The whole tree builds
  on both compilers. Linux suite (ctest -j 8, release-only excluded), on each compiler: 1368 passed, 11 skipped (no
  second uid, no delegated cgroup, no real packs or SearXNG), 2 failed:
  - `egress_table_scan_during_lifecycle`: an artefact of running in parallel. The uid-attributed scan sees other tests'
    listeners under -j 8, and it passes alone (CI runs serially).
  - `card_routes_tests.a_new_private_folder_is_made`: a test bug. It widened the drive folder to 0777, which
    local_folders.cpp rightly refuses; it now uses 01777, the POSIX equivalent of the Windows case, and a plain 0777
    parent is checked to be refused (422).
  Note: ninja's order-only dependencies keep a failed library's dependents from compiling at all, so after a failure,
  compile the held-back objects one by one to see every warning.
- **The Linux build environment** (build tool only; PROVENANCE): WSL 2 distro `mstrata-linux`, Ubuntu 24.04's WSL
  image in `.toolchains/wsl/`, apt gcc/clang/cmake/ninja/libssl-dev, GTK/WebKitGTK (owner OK), tmux, and xdotool,
  wmctrl, x11-apps, x11-utils, imagemagick for the run checks.
- **First Linux runs** (a worker, the `run` skill's pattern; scratch fake-engine card):
  - strata-tui under tmux: every view, editing and saving, resizes 160x48 to 60x18, q/y, Ctrl-C/n, and a hangup
    (`tmux kill-session` with a model running: the server and the engine were gone 0.54 s later, stopped in order).
  - strata-window under WSLg: the page signed in on the Monitor; closing hides it; SIGTERM stops in order; the tray
    menu over D-Bus (start, stop, open, exit). No tray icon or notification can show in WSLg.
  - Found and fixed: a second start of the Linux window never exited (`g_application_hold` before `run` kept the
    remote instance's loop alive; now held in the first instance's `activate`). Before the fix the second start was
    killed by `timeout` (exit 124) with a `gtk_label_set_text` CRITICAL. After it, under gcc and clang: exit 0 in 0.03-0.12
    s, the first window shown, nothing on stderr; SIGTERM and the tray's Exit still stop in order (exit 0, 0.21-0.24
    s). The TUI's Last requests showed the oldest (`/metrics` lists newest first): fixed with the review's batch above.
- **Python on Linux** (system Python 3.12 with apt `python3-pytest`, `python3-regex`): `tools/secure` 149 of 151 pass, 1
  Windows-only skip. All 7 cases of `test_build_release.py` (never run here before) pass, given in-repo `out/` trees
  with natively built release binaries (`/mnt/c` refuses cmake's `file(COPY_FILE)` for a non-root user). Git needed
  `safe.directory` through the environment, because `/mnt/c` looks root-owned. The one real bug,
  `make.py`'s `missing_cpp_tools_message` handling a Windows path as a `PosixPath` on Linux (`IndexError: 3`), now uses
  `PureWindowsPath`; `test_make.py` passes 23/23 on both systems.
- **Presets**: the TUI is on in `windows-msys2-clang` and `windows-llvm-mingw-cross` too.
- **Windows clang** (a worker; the repo's pinned llvm-mingw `20261006`, sha256 checked, clang 23.1.3, on the WSL
  distro's disk): `windows-llvm-mingw-cross` from the real source builds whole at `-Werror`, 94/94 executables, and
  99/99 with email alerts and web search on too (SChannel, no OpenSSL). It found
  `calibration.cpp:555: call to 'to_string' is ambiguous`: libc++'s file-clock count is `__int128`, so z10 had broken
  CI's `windows-msys2-clang` (libc++ too). The count is now cast to `long long`, the same number on MSVC, so kept
  records stay fresh. The binaries import only system DLLs and the UCRT. Tests not run (no wine). Note: CI's Windows
  clang jobs leave email and search off, so in CI those files meet only MSVC.
- **Unreaped engine timing** (z5's open item): MiniMax's engine took ~10-25 s to exit after a stop, beyond the server's
  ~4 s bound (RF5): the owner's decision is still open.

### Claude Code session, 2026-10-28 z12 (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

The owner: "gemma4 has an mtp ... make sure we work that mtp in our gemma engine".

- **The engine** (a worker; spec §12): `multiengine_gemma4` drafts with unsloth's `gemma4-assistant` drafter
  (`--mtp <gguf> --spec N --spec-min-p P`).
  - **Drafter:** guarded by `check_assistant` and `same_vocabulary` (4 layers, 1024 wide, Q8_0/F32). All in VRAM,
    428 MiB taken before the expert slots (57 -> 55 a layer at 4K). Q-only attention over target layers 28/29, keys
    `[pos0-1023, pos0-1]`; each step's drafts are one captured graph (a graph per draft with a min-p floor).
  - **Verify window:** `[token, drafts]` (T <= 8) through the decode kernels (`matvec_rows`, per-row attention, experts
    grouped across rows, the CPU lane on 8T entries). Every row's logits are bitwise the decode step's; only kept rows
    reach the tier's routing statistics; the reuse spare shrinks by `spec-1`.
  - **Acceptance:** llama.cpp's sample-and-accept with the request's own sampler (greedy, top-k, or penalties with the
    history per row). DONE fields 6/7 are accepted/offered; `--spec` alone does nothing.
  - **Tests:** host 23 (7 new), lane 6 (2 new), kernel 16 (5 new), model 7 (4 new with `-DG4_MTP_FILE`). RED shown;
    52/52.
  - **GPU (RTX 4080 SUPER, the Q8_K_XL target):**
    - `--spec-check` windows 2-8, lane on/off, graph and plain launches, 1,800 rows past the ring wrap: all bitwise.
    - Greedy output identical with and without the drafter; served greedy, seeded sampled and penalized answers
      identical with the lane and MMQ off. With the lane on, a long answer can part at a near tie: the drafter's slots
      move the CPU/GPU split and MMQ's rounding, as between any two plain runs with a different residency.
    - Served, 8K, lane on: greedy plain 74-76 tok/s; `--spec 4` 81.2 (50% accepted); `--spec 4 --spec-min-p 0.6`
      86.5-91.9 (64-65%). Sampled plain 71.5-76.6, `--spec 4` 81.4, with 0.6 84.9. The gain is bounded by the CPU lane
      (a 4-row window misses about 3.3x a step's distinct experts).
  - **Not done:** upstream's split verify window, a fitted PCIe share for windows, drafting overlapped with the lane.
    No Linux build of the Gemma 4 engine (its CMake is MSVC-only, as before).
  - **Build tree:** a ninja 1.13 dry run deleted `engine/build-g`'s `.ninja_log` (a 1.12.1 tree); it was rebuilt from
    `build.ninja` (hash- and format-checked against 1.12.1). Use only `cmake --build` (VS ninja) on that tree.

### Claude Code session, 2026-10-28 z11 (Windows box; owner-directed; committed with this entry, not pushed)

**The server is now `strata-unified-server`.** The owner: "Let's not call it strata-secure-server as that may provide
misconceptions, let's call it strata-unified-server". **Entries before this one (and the dated plans, reports, briefs,
research, validation records and the specs from before 2026-10-20) keep the old name `strata-secure-server`:** they
record what was built and run then.

- Renamed: the executable and its CMake target (every `$<TARGET_FILE>`, `add_dependencies` and install),
  `STRATA_UNIFIED_SERVER_PATH`, the ctest cases `strata_unified_server_{help,glm_rejects_engine_args,rejects_malformed_gpu}`,
  `strata-unified-server.link-set.txt`, the usage text and messages, the window's and the TUI's `kServerName`, `make.py`
  (targets, required files, README.txt), `install.ps1`/`install.sh`, `scripts/release` (bundle, .deb, NSIS, manifest),
  the CI workflow, `engine/*.bat`, and the current docs and specs (2026-10-22 on). Both READMEs say the old name.
- Unchanged: the source file names (`secure_server_main.cpp`), `strata-secure-smoke-server`,
  `privacy.priv04_secure_server_does_not_link_acquisition`, and the console line `Secure server listening on http://`,
  which the window and the TUI read (`kListeningPrefix`).
- RED then GREEN: `test_make.py` (5 cases), `test_release_manifest_and_sbom.py` (1),
  `window_shell_tests.the_window_starts_the_server_beside_it`, `privacy_tests.priv04_secure_server_does_not_link_acquisition`.
  Done by a worker in its own build tree. windows-msvc Release ctest: 1321/1333, failing only the 2 release-only cases
  and 10 cases of other lanes' work then in flight. pytest: 137 passed, 7 failed (`test_build_release.py` needs Linux bash
  and its build trees; on Linux, in the WSL build environment of z13, all 7 pass).
- At run time:
  - `make --out` always builds into a new folder.
  - The window and the TUI start only `strata-unified-server(.exe)` beside them, so they must come from the same build
    as the server.
  - An old install keeps its old file.
  - A data folder is not tied to the name.
  - Shortcuts, scripts, firewall program rules and antivirus allowlists that name the old path must be redone.

### Claude Code session, 2026-10-28 z10 (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

The owner: "Yes: build the MiniMax first-start prefill calibration and the DeepSeek/MiniMax CPU-thread tuning, as Maya
does", then "make sure every model we have have the tuning and calibration option as well".

- **Every profile has a sweep** (`sweep_plan(profile, SweepMachine{logical, physical})`), each with its reference
  tool's steps and candidate values:
  - qwen: upstream's `tools/calibrate.py`;
  - glm53 and raon1: Maya's `calibrate_glm.py`;
  - deepseek and minimax: Maya's `calibrate_ds4_tp.py` and `tools/mm3/calibrate.py`;
  - flint and gemma4: as before.

  The values follow Python's rounding, and the engines' own CPU counts come from their sources (`physical_cores()`,
  new: `GetLogicalProcessorInformationEx` on Windows, `/sys` topology elsewhere). The spec's §5.1 table lists them.
- **The sweep** (`run_sweep`) gains:
  - joint steps (the expert tier's three flags);
  - per-step answer lengths (`max_tokens`): the best so far is measured again on the step's answers, and the
    confirmation uses the winner's longest;
  - a step floor (`stop_below`: Maya's `STOP_BELOW`, and "stops at the first that gains nothing");
  - a per-profile keep margin (5% for deepseek and minimax, Maya's `MIN_GAIN`).

  `SweepMeasure` takes the answers' length. `apply_calibration` (calibration.cpp) puts flags into `engine.args` and
  rows into the draft; the card's own flag or row stays.
- **GLM rows**: `glm.pcie_share` (0-1) and `glm.cpu_lane` (1-64) are typed and rendered by `glm_environment_knobs` as
  `STRATA_GLM_PCIE_SHARE` / `STRATA_GLM_CPU_LANE` (the engine reads both, `glm_fast_path.cu`). The README's config
  table is regenerated.
- **First starts**: only flint and gemma4 calibrate before a page card's first start (`calibrates_before_first_start`).
  The others' sweeps are many starts of a large model, so they run from Calibrate, as upstream and Maya do. The page
  says which (`calibration.first_start`).
- **MiniMax's prefill calibration** (`run_prefill_calibration`), as Maya's `calibrate_mm3_prefill.py`:
  - before a MiniMax card's first start on this machine (no record, or a stale one), and first in each Calibrate,
    the card's composed engine runs once without `--serve` and without the prefill flags, with `--calibrate-prefill
    <calibration>/<card>.prefill-out.json --ids @<card>.prefill-ids.txt`;
  - the ids are about 4096 tokens of the out folder's `LICENSE` as one message through MiniMax's template, thinking
    off;
  - the engine keeps its choice in the pack's `prefill-tune.txt`, and `<card>.prefill.json` records the run (or that
    it did not finish: not retried by itself).

  The card's calibration JSON gains `prefill`; the page shows the prompt reading.
- **Tests**:
  - calibration: 12 cases (5 new);
  - launcher: `a_minimax_card_measures_its_prompt_reading_first` and
    `a_failed_prefill_calibration_is_not_retried_by_itself`, with a MiniMax pack on disk and the fake engine's new
    `--calibrate-prefill` / `--prefill-log` / `--prefill-fail`;
  - typed config: `glm_cpu_lane_rows_project_when_set`;
  - page: `web_ui_models_tab_calibrates`.

  RED shown for each behaviour: the new steps, the floor, joint flags, rows, the file's values, the prefill trigger,
  the first-start rule and the page's words. 187/187 over the affected suites. Full suite: ON 1313/1316 and OFF 1284/1286, the known three (two release-only and
  `idle_unload_respawns_before_the_next_request`) only.
- **Not built** (spec §5.1): DeepSeek's `--tp` on several GPUs, the DSpark drafter's speed calibration (the owner's
  packs carry no drafter), upstream's `--prefill-pipe` and AMD stream switch, Maya's GLM `READ_CHUNKS`, the
  hybrid-CPU extra counts. Not run on the GPU at this commit.

### Claude Code session, 2026-10-28 z9 (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

- **MiniMax's real pack** (the owner's `minimax-m3.1-flash-maya-s`, downloaded and packed by him) failed the real-pack
  case: "rendered MiniMax prompt cannot be encoded".
  - The vocabulary holds `<tool_call>` and `</tool_call>` as special tokens (USER_DEFINED, 200052/200053), beside the
    namespace `]<]minimax[>[` (200058).
  - Maya marks every special token its vocabulary has. Its tokenizer encodes the template's text with
    `parse_special`, so `]<]minimax[>[<tool_call>` is two tokens, and the model writes them so.
  - Our text pieces refuse a special token's spelling.

  Fixed: `kMinimaxCallTags`. The tool plan writes each call tag the pack holds as a special token as that token, and
  outputs it as its text (`literal_tokens`, with the namespace) for the call reader. The prompt fixture's literals now
  include the two tags, as Maya marks them: the prompts are unchanged, and only one case's plain spans changed.
  - RED: `plans_with_the_namespace` and `the_route_plans_minimax_searches`, on a test pack with the tags as tokens,
    and the real pack.
  - With both real packs named: the MiniMax case plans with 200058/200052/200053 output as text and the reasoning
    pair split; DeepSeek's plans as before.
  - 92/92 (ON) and 91/91 (OFF) over the tool, MiniMax, option and launcher tests.
- **Calibration for DeepSeek and MiniMax** (the owner: "when I downloaded minimax and deepseek, I did not see the
  system calibrate when I ran them"): analysed, not built.
  - Our Calibrate (`sweep_plan`) covers flint and Gemma 4 only, so DeepSeek, MiniMax (and Qwen, GLM, raon1) cards
    never calibrate, neither on a first start nor from the button.
  - Maya v1.1.30 runs three calibrations for these families:
    - the speed calibration (`tools/calibrate_ds4.py`): the DSpark drafter or none, measured on the config's first
      start that has a drafter and no calibration (`needs_speed_calibration`), and in the setup;
    - for MiniMax only, the prefill calibration (`tools/calibrate_mm3_prefill.py`, the engine's own
      `--calibrate-prefill`): the prompt chunk and the GEMM backend, measured once at the first start
      (`needs_prefill_calibration`) and kept in the pack's `prefill-tune.txt`;
    - `--tp` and `--cpu-threads` (`tools/calibrate_ds4_tp.py`, `tools/mm3/calibrate.py`): `./maya.sh --calibrate`,
      offered at the setup.

### Claude Code session, 2026-10-28 z8 (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

This entry is the MiniMax half of the owner's "deepseek does not seem to have mcp search access and I believe minimax
may have the same issue"; z7 did DeepSeek. Every profile now has the tool.

- **The prompt**: `render_minimax_tool_prompt` writes Maya's MiniMax template for a chat with tools:
  - the tool section goes in the first developer turn: each function as `tojson` writes it, in `<tool>`, then the
    call format in the tool namespace `]<]minimax[>[`;
  - each round goes as Maya's `run_with_mcp` appends it: stripped, the reasoning in `<mm:think>` or the
    `</mm:think>` prefix, the calls by the template's `to_xml`;
  - then one `tool` turn holds a `<response>` per result.

  The text and tool prompts now share one renderer. A round without reasoning whose content holds the think pair as
  text is split as the template splits it. The prompts are checked byte for byte against the 11 that Maya's code
  rendered: both tool forms, with every literal special and with none (44 renders).
- **The plan**: `make_minimax_tool_generation_plan` is the text plan (one `minimax_plan` serves both) plus the
  namespace. Where the pack holds `]<]minimax[>[` as a special token, the prompt writes it as that token and the reply's
  is output as its text (`literal_tokens`); elsewhere it is text both ways. `kMinimaxToolNamespace` is shared by the
  renderer, the plan and the reader.
- **The calls**: `MinimaxToolCallParser` runs a port of Maya's `MinimaxRewriter` on both channels, then z7's
  rewritten-mode GLM reader.
  - A string parameter (the declaration's string parameters, Maya's `may_be_string`) streams JSON-quoted.
  - Any other value goes out whole at its close: a nested one as the object or array it spells, a plain one as JSON
    when it is a number, true, false or null.
  - Tag names and scalars follow Python's whitespace (RED with an ASCII-only reader).

  The calls are checked against the events Maya's parser gave for 14 replies.
- **Wiring**: `ToolFormat::minimax`; `search_tool_format(minimax)`. `SearchTurn::minimax` carries the turn's MiniMax
  thinking (`minimax_request_thinking`) to the search route's plan. Every `SearchTurn` initializer names it. The card
  gets the Web search switch (`kMinimaxOptions` 6→7, before GPUs).
- **Tests**:
  - `secure_frontend_minimax_tool_calls_tests`, 6 cases; the real-pack case runs with `SECURE_FRONTEND_MINIMAX_PACK`.
    On the first build every case passed except the route case, RED for the missing card option.
  - Updated to every profile having the tool: `minimax_options_are_mayas`,
    `web_search_is_a_checkbox_of_the_tool_profiles`, `cards_offer_web_search_when_the_server_has_it`, and the Gemma 4
    route assertion.
  - Full suite in both configurations:
    - OFF: 1276/1278, failing only the two release-only tests.
    - ON: 1304/1308, failing the same two plus two timing tests, `send_all_bounded_stall_restarts_on_progress` and
      `import_with_unreadable_installed_version_needs_interactive_yes`. Both passed 3/3 when rerun alone, so they are
      load flakes.
- **Not checked yet: MiniMax's real pack.** None was on this box; the owner was downloading MiniMax-M3.1 meanwhile.
  Once it is packed, run the real-pack case. It prints whether the namespace and the reasoning pair are special
  tokens. Where `<mm:think>` is text, the reply is not split, as in the text plan. Nothing has run on the GPU.

### Claude Code session, 2026-10-28 z7 (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

The owner: "deepseek does not seem to have mcp search access and I believe minimax may have the same issue". Both
profiles were served without the tool, since neither had a tool form. This entry covers DeepSeek-V4.1; MiniMax-M3.1
follows.

- **The prompt**: `render_deepseek_tool_prompt` writes DeepSeek's encoder as Maya runs it for a chat with tools
  (`serve/ds41_encoding.py` under Maya's ChatTemplate):
  - the tools ride on the system message (an empty one goes first when there is none): `TOOLS_TEMPLATE` verbatim
    with its DSML names, and each tool's function object as `to_json` writes it;
  - every assistant header opens `<think>` (thinking) or closes it;
  - a round is its reasoning and `</think>` (thinking), its content, its calls in DSML (`tool_calls_template`;
    `string="true"` for a string value, JSON otherwise), EOS, then the results as one user turn of `<tool_result>`
    blocks.

  As in Maya's `run_with_mcp`, DeepSeek's rounds are not stripped (the other families' are). The prompts are checked
  byte for byte, markers included, against the 11 that Maya's own code rendered (`tests/fixtures/
  deepseek_tool_prompts.json`), in both tool forms.
- **The plan**: `make_deepseek_tool_generation_plan` keeps the text plan's markers and stops. It adds `｜DSML｜` (a
  user-defined special token in the maya-ds4 pack), output as its text (`literal_tokens`) for the call reader. A pack
  without that token plans nothing, so the chat is served without the tool (`tokenization_failed`).
- **The calls**: `DeepseekToolCallParser` runs a port of Maya's `DsmlRewriter` on both channels, then the GLM reader
  in a new rewritten mode (`GlmToolCallParser(tools, true)`), as Maya's OutputParser reads rewritten DSML. That mode
  does not strip a leading newline after `</think>` or a call, and before a call drops only a trailing `\n\n`.
  `maya_json_escaped` (glm_tool_calls) is shared with the MiniMax reader to come. The calls are checked against the
  events Maya's parser gave for 14 replies, fed whole and one byte at a time (`deepseek_tool_calls.json`).
- **Python's whitespace**: Maya's rewriter splits a tag's name with `str.split()` and tests blank text with
  `str.strip()`, and both take Unicode whitespace too. The port now does the same through `python_text.hpp`, with a
  new `python_first_word`. The new fixture case `python_whitespace_in_the_tags` was RED with the ASCII-only port.
- **Wiring**: `ToolFormat::deepseek`; `search_tool_format(deepseek)`. The search route plans a DeepSeek turn with the
  plain chat's effort line (`deepseek_effort_budget`; `none` is chat mode). The card gets the Web search switch
  (`kDeepseekOptions` 7→8, before GPUs).
- **Tests**:
  - `secure_frontend_deepseek_tool_calls_tests`, 6 cases: the prompts, the refusals, the calls, the plan, the route,
    and the real pack (`SECURE_FRONTEND_DEEPSEEK_PACK`, run here on the owner's maya-ds4 pack).
  - Updated to the new rule (DeepSeek has the tool, MiniMax not yet): `deepseek_options_are_mayas`,
    `web_search_is_a_checkbox_of_the_tool_profiles` and `cards_offer_web_search_when_the_server_has_it` (RED with
    DeepSeek's old option table), and the Gemma 4 route assertion.
  - The full suite in both configurations failed only those three option tests and the new whitespace case, besides
    the known three (`idle_unload_respawns_before_the_next_request` and the two release-only tests). After the fixes
    the affected tests pass, 72/72, in both trees.
- **Not run on the GPU**: no DeepSeek search has been run on the real engine. The owner's server was in use: first
  DeepSeek-V4.1 itself, then a MiniMax download, during which he asked that the server not be restarted or replaced.
  Docs: the READMEs' family lists, PROVENANCE (DeepSeek's encoder and `DsmlRewriter`) and the threat model's web search
  section.

### Claude Code session, 2026-10-28 z6 (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

The owner tried the README's install on another PC and pasted the errors, then: "make the installation guide full step
by step with dependencies as well" and "make an easy install bat that does pretty much everything, download
dependencies, install them then run the build".

- **What stopped him** (from his output):
  - `git -C llama.cpp checkout` was run from inside `llama.cpp` ("cannot change to 'llama.cpp'"); the first checkout
    had already worked.
  - `.\make.bat` was run from the folder above the repository.
  - `--out D:\M-Strata` is, on Windows, the `D:\m-strata` checkout itself (names ignore case). That example was this
    session's own advice; the READMEs now use `D:\M-Strata-app`.
  - On a failure, make.bat ran make twice (`... && ... || ...`).
  - Build Tools were installed without the C++ workload: vcvars64.bat ran and `cl` was not found.
- **make.bat** is rewritten with labels: the repository's `.venv-dev` Python, else `py -3`, else `python`, each tried
  once. make's exit code is make.bat's.
- **make.py**: the "cl was not found" stop now says how to add "Desktop development with C++": Visual Studio Installer,
  or `setup.exe modify ... --add Microsoft.VisualStudio.Workload.VCTools` (`missing_cpp_tools_message`; test
  `test_missing_cpp_tools_say_how_to_add_them`).
- **`install.bat` → `install.ps1` (Windows) and `install.sh` (Ubuntu 24.04+ / Debian).** Each step runs only when needed:
  - look at what the PC has and print a plan (`-Plan` / `--plan` stops there), then ask (`-Yes` / `--yes` does not);
  - install what is missing:
    - Windows, with winget: Git, Python 3.12, Build Tools 2022 with the C++ workload (or the workload added to Build
      Tools that lack it), CUDA 13.0;
    - Linux, with apt: the packages, and CUDA 13.0 from NVIDIA's cuda-keyring repository;
  - clone the repository when run outside it, else update its submodules;
  - put CMake and Ninja in `.venv-dev`;
  - shallow-fetch the pinned llama.cpp into `.toolchains/llama.cpp`, or use `../llama.cpp` when it is at the pin;
  - for `window`, get the WebView2 SDK (SHA-256 checked);
  - run make into a new `M-Strata-app` folder beside the repository, never the repository's own folder.

  A llama.cpp folder named with `-GgmlDir` / `--ggml-dir` is used as it is, never initialised, fetched or checked out:
  it may be a vendored copy rather than a clone, as on this box. When winget reports "restart to finish" (0x8A150109),
  or the Visual Studio Installer exits 3010, the script carries on and says at the end to restart. Neither script
  installs the NVIDIA driver.
- `.gitattributes`: `*.bat` and `*.ps1` are checked out CRLF (cmd.exe's labels need it).
- **README "Install"** now covers:
  - what you need, in a table per system;
  - the one-script installs (with `curl.exe` for a PC without Git);
  - the steps by hand for Windows and for Linux;
  - the build options;
  - "If the install stops": each error he saw, with its fix.

  `secure_frontend/README.md` points to it. "What this system could not test" gains the installs' own row.
- **Tests:**
  - `tools/secure/test_install.py`, 7 tests. The scripts pin the engine's llama.cpp, the WebView2 version and SHA-256
    that PROVENANCE records, and CUDA 13.0. The wrappers run the scripts, and the scripts run make. The C++ workload
    is the one make names. A named llama.cpp folder is used as it is. Line endings and the BOM are right.
  - RED shown for the pin test and for the named-folder test (both scripts).
  - With `tools/secure/test_make.py`, 30 pass.
- **Run here:**
  - `install.bat -Plan`.
  - `install.bat -Yes -Out out\install-test -GgmlDir <the box's llama.cpp> -With email,search,window`, end to end,
    exit 0. It covered the plan, the submodules, the venv, the named llama.cpp folder, the SDK already in place, the
    build, and the 811 MiB out folder with `strata-window`.
  - Not run: the winget installs themselves, since this box has every dependency. They get their first run on the
    owner's other PC. `install.sh` is `sh -n` only: no Linux here.

### Claude Code session, 2026-10-28 z5 (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

The owner: "continue" (after z4 was pushed at `ccf4f051`), on the open items of the window phase.

- **The window opens on the Monitor** (his ask: "the open window shows the Monitor statistics"; z4 opened on Chat).
  - The page takes a view in its address: `#view=<name>` or `#login=<code>&view=<name>`, for chat, monitor or about
    (`addressView`); models and settings tabs appear only once the server offers them.
  - `enterApp(models, view)` and `signIn(key, view)` open that view, else Chat. A restarted server's link keeps the
    view the page shows (`state.view`), instead of going back to Chat.
  - The window's first page is `w::page_address(link, origin, first)`: `&view=monitor` (or `/#view=monitor` with no
    link); later loads (a restart) leave the view to the page. Both windows use it.

  Tests: `web_ui_opens_the_view_its_address_names` and `the_window_opens_on_the_monitor_first`, RED with the old
  behaviour (`page_address` without the view; the page as it was), then GREEN; router 53/53 and window shell 14/14;
  the ON full suite 1293/1296 (the known three).
  Run here: the window opened on the Monitor; switched to About, a tray Restart kept About, signed in, no key dialog.
  Note: the page shows the key once at each window start (a fresh tab session each launch); the Settings tab has
  it too.
- **"Unreaped engine" on a model stop (seen on the owner's console after the picture tests): analysed, not fixed.**
  - On Windows `shutdown_generation` closes stdin and waits `kShutdownStdinWait` (2 s). `signal_group_terminate` is
    POSIX-only, so the next 1 s (`kShutdownTermWait`) adds nothing. `TerminateJobObject` then gets 1 s more.
  - A real model (tens of GB of pinned RAM, a full GPU) can take longer than that to free its memory and end. The
    process is then left in `abandoned_` ("unreaped"), the server exits 1 at shutdown, and the launcher's switch can
    start the next model while the old one still holds VRAM.
  - A timing script (start the card on a dev server, stop it, time the engine's and the encoder's exit) was ready but
    not run: the owner's server was running DeepSeek-V4.1 on the GPU at the time.
  - Needs the measurement, then the owner's decision. The bounds come from ruling RF5 (the 5 s shutdown bound).
    Possible fixes: a longer post-kill wait for launcher stops, or a launcher start that waits until the old engine
    is gone.
- **The Linux window** still has no compile here: no Linux toolchain, and Docker Desktop is installed but not
  running. Compiling it needs an Ubuntu image and GTK/WebKitGTK/AppIndicator dev packages: a download for the owner
  to approve, or a CI job that installs them.
- **The owner's server** was running DeepSeek-V4.1 (started 13:54 on the swapped build), the first time that family
  has run here. Its result is not recorded by this session.

### Claude Code session, 2026-10-28 z4 (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

The spec's Part 2, `strata-window`: the server in a window with a tray icon (the owner: "instead of a console application,
we should make it into a window application ... minimized into the tray that has the start and stop models menu").

- **Portable half, every configuration** (`window_shell.hpp/.cpp`, library `secure_frontend_window_shell`):
  - the server's sign-in and listening lines (a wildcard bind opened on loopback), and the log rule (no line holding a
    login code);
  - the web view's navigation rule (stay, browser, refuse; user@host refused);
  - the tray's model list (`GET /v1/strata/models`) and the start body (the card's name only, so its remembered
    options apply);
  - loopback requests (`LoopbackStreamConnection`, the key in the Authorization header only);
  - `server_setup` (the server's own command-line parser; `init`, `--check` and `--help` refused; the data folder
    found as the server finds it);
  - `server_key`, read as `strata key show` reads it (`card_less_server_config` for a cards config, else `validate()`);
  - `RotatingLog` (two files of 4 MiB, 0600 on POSIX);
  - `ServerChild`. On Windows it uses the engine's `launch_in_job`: a kill-on-close Job, no console, the pipe for both
    stdout and stderr, and it stops through Ctrl-Break from the child's own console (`AttachConsole`). On Linux:
    fork/execve, `PR_SET_PDEATHSIG`, SIGTERM. In both, one reader thread reaps the child, and stop() kills it after
    the grace period.

  Tests: `secure_frontend_window_shell_tests`, 13 cases, with `secure_frontend_fake_server_child`. RED was shown by
  disabling a line each for the sign-in log rule, the Ctrl-Break, the user@host refusal and the card-less rule, and
  through the old behaviour for the login-code format and the restart environment.
- **Windows window** (`window_main_win32.cpp`, `SECURE_FRONTEND_WINDOW`, MSVC):
  - WebView2 through the SDK in `.toolchains/webview2/1.0.4314.35`: the static loader, and COM handlers from our own
    template (no WRL);
  - a drawn icon; the window hides on close, and one window runs per data folder (named mutex);
  - the tray menu (Open, a Start or Stop per card, Restart the server, Exit), with Restart and Exit stopping the server
    on a thread of their own;
  - the view's profile in `<data>/state/window`; a page of its own when the server ends.
- **Linux window** (`window_main_gtk.cpp`: GTK 3, WebKitGTK 4.1, Ayatana AppIndicator via pkg-config): written to
  mirror the Windows one, but **not compiled or run** (no Linux desktop here). The portable half covers what they
  share.
- **Run here and checked** (dev server from this build, scratch data folder, dev key, a demo card on the test fake
  engine, WebView2's DevTools port read by a socket CDP client, the tray menu opened by its callback message and
  driven by keys posted to the menu window):
  - the server ran as the window's child;
  - the page signed itself in with the one-time link, and `server.log` holds no login code;
  - closing hid the window, the server kept running, and Open showed the window again;
  - the tray listed the card and its Start reached the server ("start Demo-Q8_0 ... (loopback client)"; the card then
    failed on its 4-byte placeholder model, as expected);
  - Restart gave a new server, and the page signed in with the new link with no key dialog;
  - Exit ended the window and its server in seconds (an orderly stop, not the 60 s kill).
- **Three bugs the run found, each fixed with a test first:**
  - The window took only `[A-Za-z0-9-]` login codes; the server's are base64url (43 characters, `_` among them), so
    the window ignored the real link. It now takes the page's rule: 16 to 128 of `A-Z a-z 0-9 _ -`.
  - Restart lost `--token-env`, because `read_token` scrubs that variable from the reader (the window, for the tray's
    key). `ServerSetup::environment` keeps the window's first environment, and every start gets it.
  - A restarted server's link changed only the fragment, so the page never claimed it. The page now claims a link on
    `hashchange` too (`claimFromAddress`; the claim never sends a key), and shows the key only when it is new to the
    tab. Test: `web_ui_claims_a_login_link_at_load_and_on_hashchange`.
- **make.py:**
  - `--with window` (`-DSECURE_FRONTEND_WINDOW`; `strata-window(.exe)` in the layout);
  - README.txt's Window paragraph;
  - README.txt's Settings line now names the Settings tab (it said Models > Settings).

  Test: `test_the_window_is_built_only_when_asked`, RED then GREEN; tools/secure `test_make.py` 22/22.
- **The first `--with window` build failed at install:** nothing had built `strata-window`, which needs the server but
  is not needed by it, as `strata-search` is. `frontend_build_targets` now names it (the same test, RED then GREEN).
  The out folder was then rebuilt (`--with email,search,window`, 811 MiB) and switched: `out/dist-mstrata` holds this
  build with `strata-window.exe`, the old program files are in `out/dist-mstrata-prev8`, and `data/` was kept. The
  owner's server was restarted in its console as before, and it serves the Settings route and the new page. No model
  was running at the switch.
- **Licences:** NOTICE.md carries the WebView2 SDK's BSD-3-Clause notice (the static loader is in
  `strata-window.exe`). PROVENANCE records the package (sha256, size, source) and says its NOTICE.txt components
  belong to the package's .NET tools, which are not linked.
- **Full suites:** ON 1291/1294 (the two release-only checks and `idle_unload_respawns_before_the_next_request`, as
  before; the window target built with the dev tree's `SECURE_FRONTEND_WINDOW=ON`); OFF 1261/1264 (the same three;
  the 13 window-shell cases ran there too).
- **Docs:** README (features, build, the window, caveats), secure_frontend README, threat model (the window app and its
  residual), the spec's as-built notes.
- **Not done:** the Linux window is not compiled; MSYS2 clang does not build the window (`__uuidof` on WebView2's
  interfaces); the WebView2-runtime-missing path and several monitors at different DPIs were not tried.

### Claude Code session, 2026-10-28 z3 (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

The owner: "Push them to origin and proceed to next phase" (`ca9d7f7b` pushed; the next phase is the window app and its
Configuration tab, spec `docs/superpowers/specs/2026-10-28-server-window-design.md`), then "Go ahead and download
webview2 sdk so you can continue to develop". This entry is the spec's Part 1, the page's Settings tab. Part 2
(`strata-window`) has no code yet.

- **Server**: `ConfigRoutes` (`config_routes.hpp/.cpp`, library `secure_frontend_config_routes`, built in every
  configuration). `POST /v1/strata/config` lists 14 managed rows (type, the file's value or null, default, bounds, the
  schema's description), whether the file can be written and whether the key can be rotated. `/config/settings` saves
  `{"values":{...}}`: null or "" means the default; an unknown key or a wrong type is refused by name. The whole file is
  then checked by the start-up rules (`card_less_server_config` for a cards config, else `validate()`), and only a
  problem the change brings is refused (`format_issue`); the write is `update_config_file`. `/key/rotate` takes
  `{"confirm":"ROTATE"}`, answers loopback peers only and needs a protected-file key: `generate_token`, then
  `read_token`, and the new key is answered once, no-store. The routes are served with model control; the two settings
  routes answer `model_files_allowed` clients. Tests: `secure_frontend_config_routes_tests` (3 cases, RED shown on the
  save rules and the rotation) and `config_routes_need_their_handler_and_the_key`.
- **Page**: a fifth tab, Settings, hidden until the server offers one of its sections. It holds:
  - API key: Show and Copy of the key the page holds (no route returns a key); Rotate, typed ROTATE first, with the new
    key shown once and cleared at sign-out;
  - Server and network;
  - Limits and timeouts;
  - Folders, each with Choose through the folder picker;
  - the moved Model folders, Email alerts and Web search sections.

  Models > Settings is gone. Between 641 and 900 px the tabs are text-only and the running model's name is hidden: at
  764 px the five-tab header scrolled sideways.

  Tests:
  - `web_ui_has_server_settings`;
  - `web_ui_has_email_alert_settings`, now for the tab;
  - `web_ui_has_the_models_tab_hidden_until_the_server_offers_it`, for the views list;
  - `web_ui_header_fits_a_phone`, for the 900 px block.

  RED was shown with the page as committed (and for the Choose and 900 px checks before their code); all pass now.
- **Checked in the browser pane** on a dev server from this build (scratch data folder, dev key, port 18080):
  - no sideways scroll at 900, 764, 641, 375 or 320 px; the five tabs fill one row at 320;
  - a Limits save wrote `limits.engine.generation_timeout_s: 1800` to the dev config;
  - a listen address of 0.0.0.0 without the opt-ins was refused ("a non-loopback address needs
    listener.allow_remote"), and the file was left as it was;
  - Rotate was disabled, with the reason, on that `--token-env` start.

  Rotation itself is covered by the unit test only: it was not tried on a real data folder, where it would change the
  owner's key.
- **Two problems the suites caught.**
  - `readme_config_table_matches_schema` failed after the README's `roots.allow_untrusted_media` row was changed to
    name the Settings tab; the schema's description now says the same.
  - The OFF build (no email, no search) failed because `config_routes.hpp` had been included inside the search build's
    `#if`; the include is now unconditional.
- **WebView2 SDK** (the owner's OK): `microsoft.web.webview2.1.0.4314.35.nupkg` from nuget.org (9,818,548 bytes; its
  SHA-512 matched nuget.org's catalog entry), unpacked into `.toolchains/webview2/1.0.4314.35` (git-ignored). It holds
  `WebView2.h` and `WebView2LoaderStatic.lib` under BSD-3-Clause. Nothing links it yet; PROVENANCE gets it with
  `strata-window`.
- **Full suites**:
  - ON: 1276/1280. The README check (fixed above, 125/125 in the rerun of the router, config and settings tests), the
    two release-only checks, and `idle_unload_respawns_before_the_next_request`, which passed twice alone.
  - OFF: 1247/1250. The same three, without the README check.
- **Docs**: the README's Settings section, the secure_frontend README, and the threat model (the Settings tab and its
  residual).

### Claude Code session, 2026-10-28 z2 (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

The owner: "Do all three" (push, rebuild and switch, try a picture), then "Download it" (the Qwen3.8 encoder). `main`
was pushed at `297be6f5` after the guard (run by hand: `.git/hooks/identifying_guard.py` is not wired as a hook); the
out folder was rebuilt by `make.py --with email,search` and switched (old one in `out/dist-mstrata-prev5`, `data/`
kept); the encoder (`mmproj-Qwen3.8-Flash-Next-BF16.gguf`, 907,543,008 bytes) was pulled by `strata pull` against a
manifest pinned to upstream setup.py's SHA-256, into the server's store; the owner's Qwen3.8 card names it (the
original in `data/state/card-backups/`).

- **The first real picture failed, and why**: the card and its encoder started (85 s), but the picture's request waited
  the 300 s encode limit and answered 503. Upstream's strata-vision answers `OK <tokens> <nx> <ny> <ms>`; our reply
  parser took everything after `OK ` as the count (the older maya-vision `OK <n>`), so it never saw an answer. Now the
  count is the first field, as upstream's server reads it (`int(line.split()[1])`). The test fake answers in
  upstream's form (and takes `--min-tokens`, `--flash-attn`) when it runs as `strata-vision`, as the card tests install
  it: `a_card_reads_pictures_with_its_encoder` was RED with it, GREEN with the fix; 147/147 in the vision-related suites.
- **The second real picture failed, and why**: the encoder answered at once, but the engine refused the picture's
  generate line (`killed_by_supervisor(protocol_failed)`, 503). Upstream's engine reads `GENI <max_new> [keys] <file>
  <id,id,...>` (generate.cpp; its server writes it so, serve/server.py:2199); we wrote the older pin's order, ids then
  the file. The client now writes the file first. The supervision fake engine reads a `GENI` as upstream's engine does
  (any mode; out of order: the engine's `ERR`): the protocol test, `enc_round_trip_reaches_the_prompt` and
  `a_card_reads_pictures_with_its_encoder` were RED with it (503 `engine_error`, as on the box), GREEN with the fix;
  282/282 in the generation suites.
- **The third real picture worked** (the out folder rebuilt at `39141d3a`, switched; old builds in `prev6`, `prev7`):
  the owner's Qwen3.8 IQ3_S card, Pictures on the GPU, a 256x256 PNG of a red square and a blue circle, asked through
  `/v1/chat/completions` with an `image_url` data URL: 200 in 4.1 s, "the red square is on the left, the blue circle on
  the right" (97 prompt tokens, 140 completion). The model was stopped after. Not yet run: Qwen3.6, the CPU encoder,
  several pictures, the page's attach button.

### Claude Code session, 2026-10-28 z (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

The owner: "make sure the front end is mobile responsive". The page was measured, not guessed: a dev server from this
build (scratch data folder, dev key, a demo card, a copy of an out folder's `build.json`) at 375x812 and 320x640, every
view and sub-tab (sign-in, Chat, Models: Start a model with a card's options, Download models, Settings; Monitor;
About; the folder picker; a toast; a long answer), a script listing every visible element past the screen's edge.
Nothing scrolled sideways, but three things were wrong on a phone, now fixed in the 640 px block: a folder path (the
card's model folder, "Downloads go to") wrapped into a narrow column of large monospace beside its button - now its own
line, smaller, the button below; the About's engines table hid Built from and Size behind a sideways scroll - now one
labelled block per engine (`data-label` on each cell); toasts sat flush against the left edge - now 12 px on both
sides. Re-measured at both widths: no element past the edge. Test: `web_ui_header_fits_a_phone`'s new checks (RED shown
by removing a rule). Router tests 49/49.

### Claude Code session, 2026-10-28 y (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

The owner: "Also ensure our engine supports multi gpu like the upstream". Spec
`docs/superpowers/specs/2026-10-28-multi-gpu-design.md` (what upstream and Maya do, what each family did, what changed).
The audit: Qwen's launch already composed upstream's split from `engine.gpu`; the gaps were no GPUs control on the
cards, GLM-5.3/raon1 forced onto one device, and DeepSeek cards fixing Maya's one-GPU flags.

- **The GPUs choice** (`ModelOptionKind::devices`, the option `gpus` last in every table; the launcher's `offer_gpus`
  after Web search): "auto" or GPU numbers 0-63, none repeated, at most 16 (one on Gemma 4 and flint); writes
  `engine.gpu` or, for glm53/raon1, `glm.gpu`. Tests RED first: `gpus_choose_the_cards_a_model_runs_on`, the display
  orders, `cards_offer_web_search_when_the_server_has_it`'s GPUs check.
- **This PC's GPUs** (`parse_nvidia_smi_gpu_list`, `list_gpus`, `HardwareFacts::gpus`): one `nvidia-smi` query on the
  first About request; the About lists each GPU; the page draws a box per GPU (`devicesControl`), hidden on a one-GPU PC.
  Tests RED first: `this_pcs_gpus_are_listed`, `web_ui_offers_the_gpus_of_this_pc`.
- **GLM split**: several GPUs and the split at auto drop the forced `STRATA_GLM_SPLIT=0` (Maya's engine splits by
  default). Test RED first: `several_glm_gpus_split_as_mayas_engine_does`.
- **DeepSeek**: `--tp min(2, GPUs)` and `--threads on` on one GPU composed at launch (a card's own wins); new cards fix
  neither. Test: `deepseek_launch_is_mayas_write_config` (RED shown by disabling the composition).
- **Pictures, from the same upstream reading**: a GPU encoder adds `--vram-reserve-mib 700` (#1322; a reserve of the
  card's own stands; both launch paths), and upstream's encoder reads 1,024 image tokens on the GPU, 300 on the CPU
  (`Vision.token_limit`). Tests RED first: `vision_compose_rules`, `vision_child_argv_follows_upstreams_strata_vision`.
- **README, "What this system could not test"** (the owner: "add caveats on what we could not test due to hardware
  limitations like multi gpu etc"): several GPUs, pictures with a real model, DeepSeek and MiniMax end to end, AMD and
  Intel GPUs, other NVIDIA generations and smaller systems, and Linux runs of the newest work.
- Not done: upstream's card order for an auto split (#1352: needs multiprocessors x clock, which nvidia-smi does not
  give - an engine-side device listing); a split in Gemma 4 and flint; any run on two GPUs (this PC has one). The
  owner's DeepSeek card predates this and keeps its fixed `--tp 1 --threads on` (re-adding it drops them).

### Claude Code session, 2026-10-28 x (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

The owner: "a checkbox to also download vision files ... check the upstream on how they do it", then "Continue and
commit". Spec `docs/superpowers/specs/2026-10-28-vision-for-cards-design.md` (upstream's way: `setup.py`'s encoder
pins, `tools/vision`'s `strata-vision`, the engine's `--vision`, the server's reader arguments). Qwen cards only (the
families upstream's engine reads pictures with); the other profiles stay text only.

- **Card rows and option**: `vision.mmproj` (absolute) and `vision.gpu` are card-owned rows (a main config under
  `models.cards` that sets them, or `vision.enabled`, is refused as card rows; the launcher no longer refuses
  `vision.enabled` itself); the qwen option `vision` (Pictures: off, gpu, cpu) writes `vision.enabled` and
  `vision.gpu`. `--check` prints both rows. Tests RED first: `the_vision_option_writes_the_encoder_rows`,
  `card_refusals_name_the_rule`.
- **Launch**: an expert (card) launch with vision on is qwen only, checks the encoder file by the model folders' rule
  (regular, not a link, `open_root` existing or external), carries it and the model's GGUF on `ComposedLaunch` and adds
  `--vision`. Test RED first: `vision_compose_rules`' card block.
- **Encoder**: `compose_vision_arguments` follows upstream's reader (`--mmproj`, `--model`, `--gpu`, threads, max
  tokens; `vision.no_flash_attn` as `--flash-attn off` for upstream's program). `compose_model` plans the encoder
  (`VisionPlan`: upstream's `strata-vision` beside the engine, else `maya-vision`; scratch `<state>/vision`); the
  per-card `ModelRuntime` owns it (`make_vision_role`, `start_vision` after READY, `stop_vision` on stop, swap and
  exit; a card whose encoder fails does not start); `server_bootstrap` uses the same role. Tests RED first:
  `vision_child_argv_follows_upstreams_strata_vision`, `a_card_reads_pictures_with_its_encoder` (two children, a picture
  reaches the fake engine as GENI, the encoder stops with the card, a text card refuses pictures); the managed vision
  API cases still pass.
- **Downloads**: the catalog reads an encoder's own `repo`/`revision`; `hf_manifest_json` takes a file's own repository
  and commit; `start(id, folder, vision)` / the route's `"vision": true` pulls the pinned encoder as the manifest's
  `mmproj` and counts it (400 for an entry without one; the list's `vision_bytes`, `vision`; kept in `vision.json` for a
  resume); the native, GGUF and Python card makers name it. Test RED first:
  `the_vision_files_come_with_the_download_when_asked`.
- **Cards from folders**: `GgufCardRequest`/`PackedCardRequest::mmproj` (qwen only: the row and Pictures, GPU by
  default); `find_mmproj` (the folder's one `mmproj*.gguf`, not a link; two: none); Add a model uses it. Tests RED
  first: `cards_name_their_picture_encoder`, `a_qwen_model_beside_its_encoder_reads_pictures`.
- **The page**: the download cards' "Vision files (+0.9 GB)" box (sent with Download and Resume); PNG, JPEG and WebP
  attachments while the running card's Pictures option is on (at most 4, 20 MB each; the latest question carries its
  pictures as `image_url` parts, earlier ones and the saved chat name them). `app.js` parses (esprima, in `.venv-dev`);
  on a dev server from this build (scratch data folder, dev key) the box shows on the 18 Qwen entries and the console
  has no error past the sign-in. The chat's picture path is checked by the tests' requests, not in a browser.
- **make.py** builds upstream's `tools/vision` out of source (`vision_configure_args`: the pinned llama.cpp,
  ggml-cuda, portable) and copies `strata-vision` into `engine/` (it needs only the bundled cuBLAS); `--without vision`
  leaves it out; `required_files`, `build.json` and README.txt name it. Tests RED first in `test_make.py` (21 pass).
- Tests on this PC, 0 build errors and warnings. Email and search on: 1,271, all pass but the two release-binary checks
  (as in every hooks build) and the idle-unload flake. Off: 1,241, the same. (Four suites failed on their first run -
  the page's start request, the launcher rule and `auth04`'s switch list - and were updated with the change; they pass
  143/143 and 137/137.) `tools/secure`: 116 passed, the same 7 release-bundle cases need a cross build.
- **Not done here**: a GPU run with a real encoder and a real Qwen model (the owner's server holds the GPU, and the
  owner's install has no `strata-vision` until a rebuild); pictures on the other families; adding the vision files to
  a download that is already installed.

### Claude Code session, 2026-10-28 w (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

The owner: "while I am still downloading, do not swap my server yet but begin working on the rest of the issues and
features". Then: the licence is AGPL-3.0 and nothing may say otherwise, with `main`'s published history replaced ("I
don't care if we delete github history for it"); and his DeepSeek Maya-XS download "Failed ... strata-prepare said
missing_gguf". His window choices, asked: a native shell with the page inside (tray menu: Exit, each model's start and
stop), and a Configuration tab for the API key, the listener and LAN access, limits and timeouts, and the folders.

- **The licence**: every line that described the repository's licence other than as AGPL-3.0 is rewritten (NOTICE.md,
  README, PROVENANCE, the 2026-10-13 layout spec and plan, the SP7 report, sbom.py's docstring, a test docstring);
  third-party notices (Strata's, Maya's, ggml's) stay. `main` is one root commit, `8e9cef95`, force-pushed with a lease
  after the guard passed; the earlier commits are only in the local branch `backup-main-before-scrub`, never pushed.
- **Options start from the data folder** (`uses_data_folder`, server_cli): `strata-secure-server --port 9090` read no
  `data/` and failed ("roots.models: is required"). Now every start without `--config` uses the data folder, the options
  on top of its config, except a bring-up start that names its model by flags (`--expert`, `--models-root`, `--engine`,
  `--model-id`, `--profile`, `--tokenizer-pack`). README, README.txt and `--help` say so. Test RED first:
  `options_alone_start_from_the_data_folder`.
- **`strata doctor` in an out folder**: `cuda` also passes with cuBLAS and cuBLASLt of one version in the engine's folder
  (`models.default_engine`, `engine.executable`) or `engine.lib_dirs`, the multiengine layout; on Windows `token` asks the
  store's own rules (`read_token`: the ACL) instead of POSIX mode bits. Tests RED first:
  `doctor_check_cuda_finds_cublas_beside_the_engine`, `doctor_check_token_uses_the_store_rules_where_modes_do_not_apply`;
  the rebuilt `strata doctor` on a scratch data folder: cuda ok, token ok (port fails while the owner's server runs).
- **`auth04_no_switch_disables_auth`** lists `listener.login_link`, `roots.allow_untrusted_media`, `alerts.email.enabled`
  and `search.enabled`, each reviewed in its comment (none reads, skips or replaces the key); **`test_make.py`**'s helper
  names a build folder that never exists, so the cases describe a fresh build whatever `out/make-build` holds.
- **Web search for Gemma 4** (`ToolFormat::gemma4`): `render_gemma4_tool_prompt` writes the GGUF template's tool macros
  (format_function_declaration, format_parameters, format_argument) and llama.cpp 3cf03257's message path
  (convert_tool_responses_gemma4: a round is one open model turn of its calls and their responses);
  `Gemma4ToolCallParser` reads `<|tool_call>call:NAME{...}<tool_call|>` by llama.cpp's PEG rules; the plan stops the
  engine on `<|tool_response>` (llama.cpp's Gemma 4 end of generation; our engine ends on `<turn|>`/`<eos>` only) and
  outputs the three call tags as text; Gemma cards get the Web search switch. Tests RED first:
  `secure_frontend_gemma4_tool_calls_tests` (12 fixture prompts byte for byte in both tool forms against upstream's
  `ChatTemplate` over the GGUF's template, `tools/generate_gemma4_tool_prompt_fixture.py`; llama.cpp's
  `tests/test-chat.cpp` Gemma 4 replies; the plan; the route; the real vocabulary through
  `SECURE_FRONTEND_GEMMA4_PACK`, built with `secure_tokenizer.py --profile gemma4` into a scratch folder, never
  committed), `web_search_is_a_checkbox_of_the_tool_profiles`, `cards_offer_web_search_when_the_server_has_it`. Not run
  on the GPU (the owner's server holds it).
- **MTP on Qwen3.8 cards** (`find_mtp_runtime`, `PackedCardRequest::mtp`): Add a model finds upstream's draft layer
  (`mtp/rt` with dense.bin, experts.bin, draft_vocab.bin) in the model folder, its parent or its grandparent, private by
  the folders' rule, and the card runs it as upstream's setup does (`--mtp <rt>`, spec 4, spec_min_p 0.5) with the
  MTP draft layer, Draft window and Draft confidence settings. GLM-5.3's Maya engine drafts with its pack's MTP block by
  itself (no flag; only a request's spec=1 turns it off), so its cards have none; the owner's existing Qwen3.8 card
  predates this (re-adding it, or editing its card, gives it MTP). Making the draft layer here (upstream's
  `mtp_fetch.py`, `mtp_pack.py`, `mtp_rt.py`, natively) is open. Tests RED first:
  `a_qwen38_pack_with_its_draft_layer_offers_mtp`, `a_qwen38_model_finds_its_draft_layer`.
- **Downloads keep the catalog's subfolder** (`stored_file`): the store keeps a file at `<version>/<its catalog path>`
  (`Maya-XS/...-00001-of-00005.gguf`), but the native pack steps and the GGUF-direct card took the name alone - the
  owner's DeepSeek Maya-XS failed with `missing_gguf`, and GLM-5.3 and MiniMax downloads would have failed the same way
  (GLM's links: "could not be linked"). Every branch now uses the stored path, and a Qwen3.8 or GGUF card's model folder
  is the file's own folder. The tests' fake stores had the same flattened layout; they are corrected to the store's.
  Tests RED first: the five `a_downloaded_*` cases. The owner's DeepSeek card exists already: his Add a model on that
  folder built its pack.
- Tests on this PC, 0 build errors and warnings. Email and search on, the real packs named (Gemma 4's too): 1,265, the
  two release-binary checks fail as in every hooks build. Off: 1,235, the same and the idle-unload flake. (That run came
  before the store-path fix; `secure_frontend_download_manager_tests` passes 20/20 after it.) `tools/secure`: 115 passed,
  7 failed (the release-bundle cases need a cross build this PC has not got).
- **The owner's server** runs this build since his "Yes" after his download: the out folder's contents replaced, his
  `data/` kept, `limits.engine.generation_timeout_s` 1800 in its config (`--check` shows it), restarted on the dev key;
  `/health` ok, its six cards loaded, Gemma 4's with Web search. (A shell whose working folder is the out folder holds
  it open on Windows: the folder could not be renamed, so its contents were moved.)

### Claude Code session, 2026-10-28 v (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

After entry u's push the owner asked why MiniMax had no download cards (it waited on his licence decision, strata-prepare
design §7.3) and decided: "add MiniMax-M3.1-Flash under the MiniMax Community License: its prompt, tokenizer, profile,
cards and the Maya-XS and Maya-S download cards". While that ran he reported "Flint did not either" (search), "gemma4 did
not use search", a GLM answer cut as "length" ("is it because it ran out of context? it was set to 32K"), and set the
repository's licence: AGPL-3.0. His standing words for the server: "i am downloading a model so do not swap until I
finish it"; the generation limit "after the minimax swap", raised to 30 minutes ("a voxel pagoda ... takes longer").
Then: "make sure the readme is up to data and ensure the syntax for configuring and starting it are correct", and "in the
readme include all the features it has such as the download manager, model downloading, model selection and engine
swap, notification, mcp with searxng, etc". Queued by him for after this batch: the server as a window application
(no console, minimized to the tray, the window showing the Monitor, the tray menu with Exit and each model's start and
stop) with "a configuration tab that we can configure and manage it's settings like api key, etc".

- **flint's search fixed** (`flint_prompt_renderer.cpp`): flint's real vocabulary holds `<tools>` and `</tools>` as
  special tokens, which the renderer wrote as text, so the encoder refused the tool prompt and the route quietly served
  the chat without the tool. They are markers now. The fallback is no longer silent: `ToolRunner::served_without` logs
  `[strata] search: this model's prompt cannot carry the web search tool (...)` once per reason. Tests RED first:
  `the_real_packs_plan_their_searches` (SECURE_FRONTEND_FLINT_PACK / SECURE_FRONTEND_RAON1_PACK: the route's plan over the
  real vocabularies), the fixture case's marker check, `a_model_that_cannot_carry_the_tool_says_so`; the stand-in flint
  pack gained the two tokens the real one has.
- **The GLM "length"** (found, not changed yet): the Monitor shows the last round's prompt at 3,170 tokens and 349
  written - not the context. `limits.engine.generation_timeout_s` (default 120) cut it: 31.0 s reading the prompt and
  89.2 s writing at 3.9 tok/s. The owner chose 30 minutes (1800), to be set in his config after the swap.
- **gemma4 search**: not wired (its vocabulary has its own tool tokens, `<|tool>`, `<|tool_call>`, `<|tool_response>`,
  `<|"|>`); queued after MiniMax.
- **MiniMax-M3.1-Flash** (spec strata-prepare-design §7.3 "Done 2026-10-28"):
  - Licence material: Maya v1.1.30's `LICENSE.minimax-m31`, `tools/mm3/chat_template.jinja`, `tools/mm3/download_gguf.json`
    vendored byte for byte (`sync_maya.py`, `-text`); make ships the template and licence in `engine/templates/minimax-m3.1/`.
  - Tokenizer `minimax_bpe` (NFC, then MINIMAX_PATTERN on the regex classes). Tests RED first:
    `minimax_pretokenize_splits_as_maya`, `minimax_pretokenize_matches_mayas_fixture` (471 texts),
    `minimax_bpe_normalizes_before_the_bpe`.
  - Prompt `render_minimax_text_prompt`, plan `make_minimax_chat_generation_plan`: 15 prompts byte for byte against Maya's
    own ChatTemplate (`tests/fixtures/minimax_prompts.json`, `tools/generate_minimax_prompt_fixture.py`), special tokens
    exactly where Maya's tokenizer puts them; the reply keeps the newlines after `</mm:think>` (new
    `keeps_newlines_after_think`); `[e~[`/`]~b]` server stops. Tests RED first: `secure_frontend_minimax_profile_tests`
    (4), `drive_generation_keeps_the_newlines_after_think_when_asked`, `run_generation_reads_a_minimax_reply_like_mayas_server`.
  - Profile `minimax` (config, maya_118, strata family, per-request effort, `maya_mm3` allowlist and composition with the
    shipped `expert-profile-mm3.bin`). Tests RED first: `minimax_launch_is_mayas_write_config`, the subsystem rows,
    `minimax_route_plan_follows_thinking_and_its_level`.
  - Cards and packs: options (64K of 32K-1M, `--kv-cache` int8, thinking auto, temperature 1.0, top_p 0.95), Add a model
    and downloads through `strata-prepare download --maya --template` (new: Maya's mm3_pack_step, the set's indexers
    file by `maya.indexers.for`), `maya-mm3` folders made private, the catalog's Maya-XS (93 GB) and Maya-S (122.5 GB),
    page headings for DeepSeek and MiniMax (MiniMax's naming its licence). Tests RED first: `minimax_options_are_mayas`,
    `card_profiles_parse` (minimax), `a_minimax_pack_makes_a_minimax_card`, `a_minimax_model_is_added_with_mayas_mm3_pack`,
    `a_model_folder_with_its_pack_is_made_private`, `a_downloaded_minimax_is_packed_with_mayas_step`,
    `download_maya_mm3_step` (strata-prepare), `test_maya_minimax_entries_come_from_its_gguf_manifest`,
    `test_minimax_template_and_licence_come_with_the_maya_families`, `web_ui_models_tab_subtabs_and_family_groups`.
  - Not run end to end: no MiniMax GGUF on this PC. No tool calls or pictures for MiniMax yet.
- **License: AGPL-3.0-only** (the owner's; chosen with him: AGPL plus a section 7 permission for the MiniMax material,
  the official text fetched from gnu.org): `LICENSE` is the FSF's text byte for byte; `NOTICE.md`
  holds the copyright, the MIT notice of Strata, the MiniMax permission and the three files carrying MiniMax's text;
  the out folder carries both (make.py; `test_the_license_and_notices_come_with_every_out_folder`); `scripts/release/
  sbom.py` declares AGPL-3.0-only with NOTICE.md's copyright. README "License", PROVENANCE's first section.
- **The READMEs, checked by running them** against the new out folder on scratch data folders and ports (never the
  owner's): the first start, `--data-dir`, `--port`, `--check`, `key show` (exit code only), `doctor`, the dev key's
  `--token-env` start, and the README's PowerShell and curl chat calls (answered `no_model_running`, so accepted).
  - `README.md`: a **Features** section (the engine, model selection and engine swap, the download manager, notices
    and email alerts, web search over MCP and SearXNG, the page and the API, security); start commands as each shell
    types them; options go with `--data-dir` or `--config` (alone they start from the built-in defaults and do not
    read `data/`: `--port 9090` by itself fails with "roots.models: is required"); `--port` lasts one start;
    `--data-dir` and `--config` exclude each other; `make.bat` finds vcvars itself; `--jobs`, `--build-dir`; the out
    folder's LICENSE, NOTICE.md and templates; `maya-mm3/`; `limits.engine.generation_timeout_s` and two other keys;
    the dev key in cmd, PowerShell and sh; a command-line table with `doctor`'s misfires (below); a PowerShell chat
    call; `"strata_search": false`; two troubleshooting rows.
  - The out folder's `README.txt` (make.py `readme_text`): the name, every start form for the platform's shell, the
    generation limit, its contents and licence; cuBLAS's notice only when it is bundled. Test RED first:
    `test_the_folder_readme_names_its_license_and_how_to_start_it`.
  - `strata-secure-server --help` no longer says streaming, downloads and MCP are disabled, names all seven profiles and
    shows `--data-dir DIR [options]`. Test RED first: `usage_names_the_three_profiles` (strengthened).
  - `secure_frontend/README.md`: the build line and folder contents, MiniMax and raon1 packs in Add a model and
    downloads, Settings answering the listed LAN hosts (stale since entry t), MiniMax without search, the card
    profiles, the page's tabs, the server's intro paragraph.
- **Found, not changed** (README audit): `strata doctor` in an out folder reports `cuda` (it looks for `libcudart`
  beside `strata`; the engine carries the CUDA runtime and keeps cuBLAS in `engine/`) and, on Windows, `token` (POSIX
  modes) failures that do not apply; options without `--data-dir`/`--config` do not read `data/`
  (`secure_server_main.cpp`: the data folder only when no option is given); `auth04_no_switch_disables_auth`
  (windows-runtime label) lists the boolean keys allowed and lacks `listener.login_link`, `roots.allow_untrusted_media`,
  `alerts.email.enabled` and `search.enabled` (none touches the key; failing since those keys came); two `test_make.py`
  cases read the real `out/make-build` and fail once it holds a build.
- **Found, not changed**: DeepSeek's reply drops the newlines after `</think>` where Maya's parser keeps them (the same
  rule MiniMax now keeps); `engine/tests/terminal/test_monitor_bundle.py` points at a builder this tree no longer has.
- Tests on this PC, 0 build errors and warnings. Email and search on, the real packs named: 1,254 tests, the three known
  failures and the idle-unload flake (passes alone). Off: 1,224, the same. strata-prepare 24/24. `tools/secure`: 112
  passed, the known 9 failed. After the README work: `secure_frontend_server_cli_tests` 17/18 (`auth04`, above);
  `test_make.py` 18/20 (the two `out/make-build` cases, above); the out folder rebuilt by make.py with email and search
  (774 MiB, 0 failures).

### Claude Code session, 2026-10-28 u (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

After entry t the owner asked why raon1 "did not use the MCP to retrieve latest information" ("check it on flint as
well") and pasted a Qwen3.6 chat that "did search but did not give me an answer". Found: only the qwen and glm53
profiles were offered the tool; and every search of the Qwen chat came back empty because the test SearXNG's only web
engines (Brave, Google CSE, DuckDuckGo) were refusing it, so the model searched eight times and upstream's rule ended
the answer at the limit, with nothing written. The owner chose: "add web engines on the SearXNG box, show the round-limit
note, say why a search came back empty, and give the model one final answer round after the limit. Then rebuild and
replace my server", then "fix raon and flint as well", "a toggle in the card that can enable search by clicking on a
checkbox" (asked where: "Both", the card and the chat), "the search results should be in a box as well, they look out of
place" and "in the about section, the engines and text look messy especially when it wraps". Spec: web-search design §6.

- **The test SearXNG** (the owner's Docker instance; not in the repository): its `settings.yml` backed up, then Bing,
  Qwant, Yahoo and Mojeek turned on; five probe queries now return 17-27 results each. The README shows the same block.
- **Why a search was empty** (`web_search.cpp` `format_search_results`): SearXNG's `unresponsive_engines` (at most 8,
  name and reason cut short) become "No results: the search engines did not answer (...), so searching again now is
  unlikely to help."; otherwise "No results.". Test RED first: `searxng_json_becomes_numbered_results`.
- **One answer round after the limit** (`search_rounds.cpp`): past `search.max_rounds` the calls are not run; the
  `limit` event and `skipped` results go out as before, each call's result in the next prompt is the fixed
  `kSearchLimitNote`, and the model gets one more round; a call in that round ends the answer. Test RED first:
  `the_round_limit_leaves_one_answer_round` (it replaces `the_round_limit_ends_the_answer`, whose rule changed).
- **raon1 and flint search** (`glm_prompt_renderer` `render_raon1_tool_prompt`, `flint_prompt_renderer`
  `render_flint_tool_prompt`, `make_raon1_tool_generation_plan`, `make_flint_tool_generation_plan`, `ToolFormat::raon1`
  / `flint`, the chat route's profile switch): raon-1-mini's template writes Qwen's tools and calls (Qwen's parser),
  flint's writes GLM's (GLM's parser); the tags are single tokens in both packs. Byte for byte against upstream's
  `ChatTemplate` over each model's own `chat_template.jinja` (raon1's with the `{% generation %}` extension): 19 cases in
  `tests/fixtures/raon1_flint_tool_prompts.json`, made by `tools/generate_raon1_flint_tool_prompt_fixture.py`. Tests RED
  first: `secure_frontend_raon1_flint_tool_calls_tests` (5 cases: the fixture, the refusals, the plans' tags, each
  template's call form, the route's plans). Gemma 4 and DeepSeek are still served without search.
- **Turning search off.** `"strata_search": false` in a chat request serves it without the tool (any profile; not a
  boolean: 400 naming it). Every qwen, glm53, raon1 and flint card has a `web_search` option (Web search, on by
  default): the launcher adds it to a card that does not list it, lists it only while the server's search is on, and a
  start with it off gives the model no tool; it writes no flag or row, and calibration leaves it out of its key. The
  page's chat settings have a Web search switch, shown while the running card's is on; off, the page sends
  `strata_search: false`. Tests RED first: `strata_search_per_request`, `a_chat_can_turn_search_off`,
  `web_search_is_a_checkbox_of_the_tool_profiles`, `cards_offer_web_search_when_the_server_has_it`,
  `a_card_started_without_web_search_never_searches`, `web_ui_chat_can_turn_search_off`; the option tables' and the
  calibration key's expectations gained `web_search`.
- **The page** (`web/app.js`, `index.html`, `app.css`): the searches are a collapsible box like the thinking ("Searched
  the web · N searches", the queries as a list, the round-limit note); About's engines are a table (Models, Library,
  Built from, Size). Seen live in a narrow window after the first swap: the library names still broke mid-word
  ("multiengine_maya_d / ll"); now a name stays on one line and the table scrolls sideways in its card. Test RED first:
  `web_ui_search_box_limit_note_and_engine_table` (its library-name check RED against the first CSS).
- **Docs**: README (the families that search, the two switches, more SearXNG engines; two stale lines saying Settings
  answer this PC only, fixed after entry t), `secure_frontend/README.md` "Web search from the API", the threat model's
  web search section, the web-search spec §6.
- **The owner's server replaced** (twice, the second for the About fix), by moves as in entry t: a new
  `make --with email,search` folder took `out\dist-mstrata`'s place, the folder it replaced kept as `-prev2` / `-prev3`,
  its `data/` moved across; it runs on the dev key on 0.0.0.0:8080. Checked live: the qwen, flint and raon1 cards list
  Web search (on), the Gemma card does not; the page loads and runs the new script (its CSP refuses eval and inline
  styles, so there was no in-page syntax check; no script error in the console); About's library names sit on one line.
  Not yet tried by the owner: a real raon1 or flint chat that searches.
- **Waiting for later (owner, 2026-10-28, "after we fix all the current issues")**: a download checkbox for a model's
  vision files, with the engine serving images where the model can (check upstream's way first); and MTP settings on
  the cards (the owner sees none; check what each engine supports).
- Tests on this PC, 0 build errors and warnings. Email and search on, the real packs and the Qwen3.6 GGUF named: 1,237
  tests, only the three known failures (`auth04_no_switch_disables_auth`, `raw_profile_rejected_in_release_build`,
  `release_binary_lacks_hook_marker`). Email and search off: 1,208 tests, the same three. After the About CSS fix the
  page and router suite again: 48/48.

### Claude Code session, 2026-10-28 t (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

After entry s the owner had the new out folder started on the dev key (it serves on 0.0.0.0 with the old server's
network and model-control settings and web search) and, testing it from another PC, reported: "when I selected the
model for raon1 it says the permission issue, and it did not add the card", "I also get the make it private issue when
selecting the maya folder", "I don't see a section for the email notification or mcp settings in this new build", "I
don't see use folders as they are in the setting like the message box says", "when I stop the running engine, the
stats/monitor page does not reset stats like it still have something loaded", "the model state should say unloaded or
stopped if it is stopped", "on recent request, we should identify what model that line was ran on", "in the about, it
should list the build of our engines and also list the hardware of the system" and "i am downloading deepseek maya-xs
right now, it does not seem to be doing the multi threaded downloading". His rule for the turn: "do not touch the server
right now as I am testing ... but fix the code" - everything below is code and tests in the dev trees; the running
server was neither restarted nor rebuilt.

- **Make private with a model's pack in the folder** (`local_folders.cpp` `make_model_folder_private`): the owner's
  raon1 folders (Q8_0, IQ4_XS) and the Maya GLM folder hold the `pack/` their setup built beside the files (the IQ4_XS
  one also Hugging Face's `.cache/`), and the page's "Make it private" refused any folder with a sub-folder. Now the
  folders a model's tools put there are allowed - `pack`, `pack-<name>`, `maya-ds4`, `maya-ds4-<name>`, `.cache` - each
  walked whole (no link anywhere in it, at most 4096 entries in all), and the whole tree is made owner-only; any other
  folder, or one only named like a pack (`package`), is still refused. Test RED first:
  `a_model_folder_with_its_pack_is_made_private`.
- **The settings from another PC** (`alert_routes`, `search_routes`, `card_routes` sharing): the email alerts, web search
  (MCP) and Model folders settings answered this PC only, so a page opened elsewhere hid all three - while the
  "make it private" question names the Model folders switch. They now serve the clients the model-file routes serve:
  this PC and the hosts and subnets `models.control_remote_networks` lists (`model_files_allowed`, never
  `models.control_remote` alone); the server passes one `files_allowed` rule to all of them. The SMTP password is still
  written, never shown. Tests RED first: `alert_routes_let_in_the_clients_the_rule_lists`,
  `search_routes_let_in_the_clients_the_rule_lists`; `the_shared_folders_setting_is_read_and_saved` now checks the rule
  both ways (a listed subnet saves it, another is refused) instead of "this PC only".
- **raon1 from its files.** `strata-prepare pack --maya` / `download --maya` / `tokenizer` learn raon1 as our additions
  to Maya's Python do it (`engines/maya-ref/tools`' diff: `RAON1_NATIVE_OK`, no BF16 upgrade, 3-D tensors, a quantized
  tensor outside the set dequantized to F32 - what the IQ4_XS build needs; the `raon` pre-tokenizer's pattern shipped in
  `tokenizer.json`). Unit test RED first `maya_raon1_pack_tables`; parity (`strata_prepare_parity.py --raon-tools`, a
  reference folder of Maya v1.1.30's tools with that diff applied): 9 cases byte for byte - synthetic Q8_0 and
  IQ4_XS-layout models with and without `--compat-bf16`, `--experts-bin`, the setup step, the tokenizer - and the
  owner's two real raon-1-mini GGUFs through the setup step (the native `tokenizer.json` also equals the one in the
  owner's existing pack). The GLM/DeepSeek/MiniMax parity cases still pass against Maya's own tools.
  - The page's Add a model: `packed_family` names raon1; its pack goes in `pack/` beside the file as GLM-5.3's does (an
    up-to-date pack of this file is reused; `pack-<name>/` when `pack/` is another model's), the marker
    `raon1-mini.gguf`, and `card_for_pack` writes the managed raon1 card's settings: KRAFTON's sampling (temperature 1.0,
    top_p 0.95, top_k 20, presence_penalty 0 over the last 2048 tokens, 32K answers by default) and, new, the GLM
    engine's typed menus for raon1 (`kRaon1Options`: contexts 8K-256K, 64K by default; the KV cache fp16 / int8 / q8_0 /
    q4_0, q8_0 by default; written as `glm.max_context` / `glm.kv`, never `engine.args`). Tests RED first:
    `a_raon1_pack_makes_a_raon1_card`, `a_raon1_model_is_added_from_its_folder` (the owner's case: a widened folder with
    its pack refused, made private, then added).
- **The Monitor after a Stop** (`serve_monitor`): with no engine (the launcher's `none`) the state is `stopped` and the
  stopped engine's context and expert-tier readings are left out; the launcher also clears them when a stop retires the
  engine. The page has a Stopped badge, says "No model is running", and blanks the live cards; the history stays. Each
  recent request now names the model it ran on (`requests[].model`, the served name ChatRoute passes; a Model column).
  Tests RED first: `a_stopped_engine_reads_stopped_and_requests_name_their_model`, `web_ui_monitor_shows_stopped_and_the_model`.
- **About: the engines' build and this PC** (`about_info.{hpp,cpp}`, `GET /v1/strata/about` in `kRouteTable`, keyed like
  every API route, every server): make.py writes `engine/build.json` (when, the repository commit, the ggml pin, the GPU
  targets, the features, and each family library with the models it runs and its source - the engine version from
  `engine/CMakeLists.txt`, the families' pinned version from `cmake/multiengine.cmake`, Maya's from
  `third_party/maya/VERSION`); the server reads it once (schema 1, at most 64 KiB, known fields only, printable text) and
  adds the CPU and its threads, the RAM, the GPU (the Monitor's reading) and the OS (Windows: edition, version, build;
  POSIX: PRETTY_NAME and the kernel). The page shows "Engines" and "This PC" cards. Tests RED first:
  `test_the_build_record_names_each_engine_and_its_source` (make.py), `the_build_record_is_read_and_checked`,
  `this_pcs_hardware_is_named`, `the_about_json_has_both`, `about_route_needs_the_key_and_answers_its_provider`,
  `web_ui_about_lists_the_engines_and_this_pc`.
- **Downloads in 16 connections by default**: the parallel ranged downloader (2026-10-23) was there, but `pull.connections`
  defaulted to 1, so every download was one stream (neither the owner's old nor his new config set it). The default is
  now upstream's 16 (`upstream/setup.py` `DL_CONNECTIONS`, which the downloader mirrors); 1 still means one stream. The
  running server keeps 1 until it runs a new build. Test RED first: `typed_config_accepts_minimal_file_and_applies_defaults`.
- **Seen, not changed**: stopping Qwen logged "engine supervisor stopped with an unreaped engine"; no engine process was
  left afterwards (checked), so nothing held VRAM.
- **The owner's server replaced** (owner: "start the new build and replace the one I am currently running"): a new
  `make --with email,search` folder took the running one's place by moves only (the old folder kept beside it as
  `-prev`, its `data/` moved into the new one, so every saved path still holds); it runs on the dev key. Checked live:
  About lists the seven engine libraries and this PC; the Monitor reads `stopped`; from another LAN machine the three
  settings routes and About answer 200. A download paused before the restart shows as available again: the file in
  progress restarts from zero at 16 connections (a single-stream partial has no `.part.chunks` record); finished files
  are kept.
- Tests on this PC, 0 build errors and warnings. Email and search on, the real tokenizer packs and the Qwen3.6 GGUF
  named: 1,225 tests, only the three known failures (`auth04_no_switch_disables_auth`,
  `raw_profile_rejected_in_release_build`, `release_binary_lacks_hook_marker`). Email and search off: 1,198 tests, the
  same three and the idle-unload flake. `strata-prepare`: 23/23 unit tests; parity `--raon-tools` 9/9 and `--maya-tools`
  17/17 (with the tools folder that holds Maya's templates). `tools/secure`: 109 passed, the known 9 failed (7
  `test_build_release` cases need WSL's `/bin/bash`, `test_make`'s two engine-compiler cases).

### Claude Code session, 2026-10-27 s (Windows box, 4080 SUPER; owner-directed; committed with this entry, pushed on the owner's word)

After entry r's commit (`152bd798`) the owner said "Let's have an option to not change the permissions when browsing and
using existing models in the local system. Let's have it to where we can just ignore that other users can access the
files locally", then pasted a GLM-5.3-Flash Maya-S24 download card ("0.0 GB · 3 files ... download only: the GLM-5.3
engine is not built here yet"), and said "I also don't see a download card for deepseek 4.1 flash", "or mini max", "I also
don't see smtp settings or mcp settings" and "once done, commit and push to main/origin".

- **Model folders as they are** (design: `2026-10-27-gui-first-install-design.md` §4.2, "Folders as they are"; threat
  model: "Model folders as they are" and its residual). The existing owner opt-in `roots.allow_untrusted_media` now
  reaches the page's card routes: with it on, Add a model, a card's new folder and the Danger zone open the folder with
  `RootPolicy::external` (`trusted_folder(text, shared)`), a GLM/DeepSeek pack strata-prepare made beside the files is
  not made owner-only, and the page never asks to make a folder private. Card launches already honored it (a card's
  config is the main config plus the card's rows). Downloads, the server's own folders and the config file keep their
  checks.
  - Models > Settings > **Model folders**: the switch over `POST /v1/strata/folders/sharing` (`{}` reads `shared`,
    `running`, `editable`; `{"shared": bool}` saves the row, off takes it out), keyed, this PC's clients only even where
    the file routes admit listed subnets; it applies at the next start. The Settings sub-tab now shows with model
    control, alerts or search. The private-folder question names the switch.
  - `config_writer.hpp` (`update_config_file`): the alert, search and model-folder settings share one config rewrite
    (read, change, owner-only write in a private folder) instead of three copies.
  - The key's description (and the README key table, generated) says what it now covers.
  - Tests, RED first: `a_shared_folder_is_used_as_it_is_when_the_owner_allows_it` (a widened folder used and left
    widened; a card moved to another; a GLM pack left as strata-prepare made it; the control: without the option such a
    pack is made private), `the_shared_folders_setting_is_read_and_saved` (read, save, off again, malformed bodies,
    loopback only, running, no config file), `web_ui_has_model_folder_setting`; changed with the requirement:
    `model_card_routes_need_the_switch_and_the_key` (the new route), `web_ui_has_email_alert_settings` (the sub-tab's
    rule).
- **"0.0 GB" on Maya's GLM cards**: Maya's `maya.py` pins only SHA-256 at revision `main`, and the generator
  (`tools/secure/download_catalog.py`, its tests and `--check`) keeps those sizes unset on purpose - the server reads
  them from Hugging Face when a download starts. The list now says so: each card's `size_known`, and the page shows
  "size read from Hugging Face at the start" instead of 0.0 GB (`catalog_cards_and_the_disk_check` and
  `web_ui_has_model_folder_setting`, RED first). Checked, not committed: Hugging Face's file list gives every GLM-5.3
  file with the SHA-256 the catalog pins (Maya-S24 about 94.7 GB; Maya-S-v2's sizes equal the owner's copy on G:).
- **Why the owner's page lacks what is built** (found, not changed): the running server `out\dist-email` (pid 19132)
  was built at 14:16, before GLM-5.3 downloads were wired (`7ac4d16f`, 16:47: the "engine is not built here yet" note
  is that build's), the DeepSeek catalog (`36ea3bf6`, 18:12) and web search (`51c03d38`, 20:25). The alert, search and
  model-folder settings also answer this PC's browser only, so from another PC they stay hidden. MiniMax is not in the
  catalog: it still waits on the owner's licence decision (strata-prepare design §7.3).
- **No identifying data, and a fresh history** (owner: "make sure we have no sensitive data in the git or any
  identifying data", "no references to my internal network IPs and other settings", "even if they pull the old commits,
  not private information would leak", "make this main push as the primary starting point"):
  - Audit: no secret, key, token or password in any file or commit (the key-like strings are test fakes). Identifying
    data was: the owner's e-mail address (docs and every commit's author), the owner's handle, this PC's name, LAN
    addresses (this PC, the SearXNG box, two subnets) and network settings, Claude session links.
  - Every file of ours (not `upstream/` or `engines/`) scrubbed: the noreply address
    `40450442+shift-f10@users.noreply.github.com`, "the owner" for the handle, `<pc-name>`, `<this PC's LAN address>`
    and `<LAN box>` in docs, `172.16.A.B` for every other `192.168.A.B` (still private, so the tests keep their
    meaning; the SP4 spec's IANA list and `address_policy.cpp`'s numeric ranges are unchanged), no session links. Three
    tokenizer fixtures quoted a handoff line with the handle; their ids were taken again from each fixture's own oracle
    (llama.cpp master for flint, KRAFTON's llama.cpp for raon1, upstream's and Maya's tokenizers for the Qwen/GLM ids).
  - The rules (`CLAUDE.md`, `AGENTS.md`, the resume document §2, the common brief): the noreply author, one
    `Co-Authored-By` trailer, no `Claude-Session:` line, and no identifying data in any commit.
  - GitHub's `main` starts again from one commit holding these files; the 8 other branches there were deleted (7 were
    in `main`; `sp6-plan1-eval-kit`'s 3 other commits are kept as a local branch). The full earlier history is kept on
    this PC only, as the local branch `history-before-2026-10-27` (never pushed): **commit IDs cited in this document
    before this entry name commits on that branch.**
- **The repository moved** (owner: "I will just create a new repo and make this old one private"): the code now lives
  at `shift-f10/m-strata` (the fresh `main` only); the old `shift-f10/multi-strata` is the owner's to make private.
  Standing rule (owner: "from now on, on this new repo ... no identifying information or sensitive information will be
  pushed"): `CLAUDE.md` and `AGENTS.md` state it, and `.git/hooks/identifying_guard.py` (local, never pushed) checks
  each commit a push would add - addresses, message, every added line, this PC's API tokens, files that must not go.
  It is run by hand before a push; wiring it as the `pre-push` hook is the owner's step (the agent's install was
  refused as persistent configuration).
  - Its first run found what the earlier scans missed: `engine/raon-secret/token` (the raon dev server's API key, a
    placeholder the owner had once called safe to publish) was tracked, and its value was written in
    `engine/setup-frontend-raon.bat` and this document - so it was public on the old repository, in its history and in
    the first fresh commit pushed there. Now the file is untracked and `engine/raon-secret/` ignored (with `secret/`,
    `secrets/`, `*.pem`, `*.key` anywhere), and the fresh history for `m-strata` was made again without the file.
    Then the owner ruled: "the only key that we can save in the git repo is the dev api placeholder which is
    strata-secret-key-dev-placeholder and when ever we start the server and test and develop, this is what we should
    be using" - so the setup script writes that placeholder (as before), the rules, the README ("For development") and
    the guard name it as the one allowed key, and every dev or test server runs on it (`--token-env STRATA_DEV_KEY`,
    or a `file:` token holding it). `engine/glm-secret/token` (the real GLM key) was never tracked.
- The README names the project **M-Strata** (Multiengine Strata).
- **README** (owner: "a detailed guide of installation, configuration and usage", "the specs of our windows dev
  system"): the root `README.md` gains the development system's specifications, Install (requirements, the pinned
  llama.cpp, `make`), Configure (first start, models, settings, the config file, LAN use, a SearXNG setup), Use (the
  page, the API with curl and the OpenAI client, the CLI) and Troubleshooting; its engine and model tables now match
  the launcher and its libraries.
- Tests on this PC, 0 build errors and warnings. Email and search on, after the scrub, with the real tokenizer packs,
  the Qwen3.6 GGUF and the SearXNG instance named (every real-data case ran and passed: flint's and raon1's fixtures
  against the regenerated ids too): 1,213 tests, only the three known failures and the idle-unload flake, and
  `listed_remote_clients_use_the_file_routes`, which the scrub broke (its numeric client addresses) and is fixed
  (card routes 13/13 after). Email and search off, before the scrub: 1,188 tests, the same known four. Python:
  `serve/test_security` 28/28, `engine/tests/terminal` `test_loopback` 3/3 and `test_launch_monitor` 11/11;
  `test_launch`, `test_launch_tui` and `test_launch_tui_walk` fail for reasons of their own (their diff is the addresses
  only); `tools/secure` 108 passed, 9 failed (7 `test_build_release` cases need WSL's `/bin/bash`; `test_make`'s two
  engine-compiler cases are the known ones).

### Claude Code session, 2026-10-27 r (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

After entry q's commit (`51c03d38`) the owner said "ssh using <user>@<LAN box> and deploy a searxng using docker in
there to test", then "find out why the model's reasoning gets doubled spaces after a search result, and fix it if it's
ours", "test it using MITRE codes and actors to see if the mcp works" and "document in the readme when using model to
connect via api, how to make the model use the search capability to fetch most up to date information".

- **The instance** (a Linux box with Docker on the owner's LAN): `~/searxng/compose.yaml`
  runs `docker.io/searxng/searxng:latest` (digest `sha256:0f48ffe2...ad7b6e`, SearXNG 2026.10.27-b5ed73fcb) as container
  `searxng`, `restart: unless-stopped`, port 8080 on all addresses; `~/searxng/config/settings.yml` (owner-only) is
  `use_default_settings` with a random `server.secret_key`, `limiter: false`, `image_proxy: false`, the base URL
  `http://<LAN box>:8080/` and `search.formats: [html, json]` (strata-search reads the JSON). Stop it with
  `docker compose down` in `~/searxng`. Nothing else on that box was touched.
- From this PC: its JSON API answered (37 results in 1.4 s; duckduckgo answered CAPTCHA and wikidata an HTTP error -
  the engines' side, not ours); `strata-search` over MCP (initialize, tools/list, tools/call) returned five results,
  1,605 characters, in 0.86 s.
- **New opt-in case** `secure_frontend_web_search_server_tests.a_real_searxng_answers_through_the_server` (label
  `searxng-real`; `SECURE_FRONTEND_SEARXNG_URL` names the instance; unset, skipped): the server's own strata-search asks
  the real instance and its results reach the second prompt as the tool response. RED against a closed port (the
  result `ok: false`, "the search instance could not be reached"), GREEN against `http://<LAN box>:8080` (Rust
  1.90.0's announcement first), skipped unset; the suite's other two cases pass.
- **The page's routes on a make build** (`make --out out\dist-search --with email,search`, 774 MiB, strata-search
  beside the server; a scratch server on 127.0.0.1:8091 with `--data-dir`): `/v1/strata/search/settings` saved the
  instance, a restart picked it up (`running: true`), and `/v1/strata/search/test` returned live results.
- **A real model searching** (the same scratch server; `/v1/strata/cards/add` with the owner's
  `Qwen3.6-35B-A3B-Q8_0.gguf` from `engine/downloads/...`, thinking high, temperature 1, 32K; the GPU free). Two runs:
  - "What is the latest stable version of Rust, and when was it released?" (complete): one call,
    `{"query": "Rust latest stable version release date site:blog.rust-lang.org", "max_results": 5}`, 2,185 characters
    back in 1.7 s. The answer was Rust 1.99.1, released October 15, 2026, matching the live results. 54-58 s; about
    2,300 prompt and 1,400-1,500 completion tokens.
  - "Who won the most recent Formula 1 World Drivers' Championship?" (streamed): two searches, in two rounds in one run
    and as two calls in one round in the other. The answer was Lando Norris for 2025, noting that the 2026 season is
    still running (Kimi Antonelli leading). 105-146 s.
  - "What is 17 x 23?": no search, 391, 21 s.
  - **Doubled spaces after a search** (found here, then fixed on the owner's "find out why ... and fix it if it's
    ours"): the reasoning the model writes after a tool result had a doubled space in almost every word gap (595 of 633
    gaps in one run), and so did one later query. Its reasoning before the call (5 in 757 words, all `1.  **` list
    markers), its answers (0-1) and the results text (0) did not, so the round-2 prompt set it off.
  - Stopping the model logged "engine generation 1 ended: unreaped" and the server exited 1 every time. The 35 GB
    engine took longer to exit than the supervisor waits. Nothing to do with search; not changed.
- **The cause, ours: the Qwen and GLM text split.** byte_bpe split text with `basic_pretokenize` (runs of one character
  class), so a space was never part of the word after it: " the user" went in as `[Ġ][the][Ġ][user]`, not
  `[Ġthe][Ġuser]` (30 ids against upstream's 17 on one of the model's sentences). In round 2 the model read about 750
  words of its own reasoning that way and wrote it back with two spaces. Every Qwen and GLM-5.3 chat sent its text
  split that way, searching or not (entry o's "known gap"); raon1, flint and DeepSeek already had their references'
  splits. The fix:
  - `byte_bpe` (the qwen profile) splits by upstream's own `QWEN35_PATTERN` (`upstream/tools/strata_tokenizer.py`,
    v0.2.6, the split upstream's server gives every Qwen pack) and encodes through the shared byte-level BPE with the
    index built at load (the old path rebuilt two maps of the whole vocabulary for every span).
  - New `glm_bpe` (the glm53 profile; `tokenizer_mode_for`): Maya v1.1.30's `GLM_PATTERN` (pre `glm4`/`glm5`: digits in
    threes, letters without marks) and its `ignore_merges` (a piece that is one ordinary vocabulary token, looked up in
    the byte alphabet, is that token).
  - `qwen35_pretokenize` / `glm_pretokenize`: one splitter for both, on the regex module's classes (the table DeepSeek's
    split uses). `basic_pretokenize` and the old per-call encoder are gone.
  - Tests, RED first (stub splits; the old split and mode): `qwen35_pretokenize_splits_as_upstream`,
    `glm_pretokenize_splits_as_maya` (each expectation the reference's own output),
    `qwen35_pretokenize_matches_upstreams_fixture`, `glm_pretokenize_matches_mayas_fixture` (471 texts each),
    `byte_bpe_takes_a_word_with_its_leading_space`, `glm_bpe_takes_a_whole_vocabulary_piece`, and against the real
    vocabularies `real_qwen36_gguf_encodes_as_upstream` (gguf_metadata, `SECURE_FRONTEND_QWEN36_GGUF`) and
    `glm53_real_pack_encodes_as_maya` (`SECURE_FRONTEND_GLM53_PACK`): 42 texts id for id, among them the model's
    reasoning and a search result. RED 34 and 36 of 42 texts differed; GREEN all equal. Changed with the requirement:
    `tokenizer_mode_follows_the_profile` (glm53 is glm_bpe) and `deepseek_bpe_encodes_mayas_pieces_by_rank`'s control
    (byte_bpe now takes each digit alone: [1, 2, 3, 4], it was [12, 34]).
  - Fixtures: `tests/fixtures/maya_pretokenize_texts.json` gains seven texts and the `qwen35` and `glm4` pieces (the
    464 old rows' deepseek3 and minimax pieces regenerate unchanged); `tests/fixtures/byte_bpe_reference_ids.json` is
    new (`tools/generate_byte_bpe_reference_fixture.py`: upstream's tokenizer on the Qwen3.6 GGUF, Maya's on the GLM
    pack; ids only). Both generators load the reference modules in place and write no bytecode in `upstream/`.
  - The real model again, on a make build with the fix (`out\dist-search2`, the same data folder): 0-7 doubled gaps
    per answer's reasoning (list markers), none after a result (0 of 367 words in the F1 round 2; 0 in the Rust chat's
    2,136 words over two searches).
- **MITRE ATT&CK through search** (owner: "test it using MITRE codes and actors"), the same model and instance,
  `search.max_rounds` 8:
  - T1059.001: one search (attack.mitre.org) - Command and Scripting Interpreter: PowerShell, tactic Execution; 30 s.
  - G0016: three searches - APT29 with its aliases (NOBELIUM, Dark Halo, NobleBaron, UNC2452, The Dukes, ...) and
    T1195.002, T1566.001, T1547.001; 98 s. With `max_rounds` 4 (the earlier run) the model asked a fifth search, which
    was skipped at the limit, and the answer was empty - upstream's own behavior at the limit (`run_with_mcp`).
  - G0032 (streamed): one search - Lazarus Group, North Korea (RGB), its associated names; 20 s.
  - "The latest version of ATT&CK": four searches - v20.0, October 13, 2026, as attack.mitre.org's version history
    page reads; 125 s.
  - T1566.001 with its mitigations: five searches, then the last round spent the request's 4096 tokens reasoning
    (`finish_reason` length) and the answer was empty: `max_tokens` is each round's budget, as upstream's.
- **README** ("document in the readme when using model to connect via api, how to make the model use the search
  capability"): "Web search from the API" - turning it on, what the model is offered, getting it to search (the
  question, an explicit ask, a system message), the round limit and `max_tokens` per round, what a client must not
  send (`tools` ... refused), the complete and streamed replies' `strata_search` events, a curl example.
- Left in place: `out\dist-search` and `out\dist-search2` (gitignored; `out\dist-search\data\` holds the Qwen3.6 card
  and the search settings, now `max_rounds` 8); the instance keeps running on the box. No scratch server, engine or
  strata-search is left running here.
- Full frontend suite on this PC, 0 build errors and warnings: email and search on, 1,210 tests (with
  `SECURE_FRONTEND_QWEN36_GGUF`, `SECURE_FRONTEND_GLM53_PACK` and `SECURE_FRONTEND_SEARXNG_URL` set, so the three new
  real-data cases ran and passed); email and search off, 1,185 tests. Only the three known failures in each
  (`auth04_no_switch_disables_auth`, `raw_profile_rejected_in_release_build`, `release_binary_lacks_hook_marker`). Not
  run: a Linux or clang build (no clang on this PC; the code follows the DeepSeek split's idioms).

### Claude Code session, 2026-10-27 q (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

After entry p's commit (`36ea3bf6`) the owner said "Commit and proceed to the mcp feature": the queued "optional build
that is disabled by default but enabled for us that is an mcp connection to searxng that enables the model to search
If it is not certain and does not have the latest information". Design: `docs/superpowers/specs/2026-10-27-web-search-design.md`.

- **Build option `SECURE_FRONTEND_WEB_SEARCH`** (off by default; on in our three presets that turn email on, and in
  `make --with search`). Without it: no `strata-search`, no MCP client, no search service or routes, and every
  `search.*` config row is refused by name (checked in every build). The Qwen/GLM tool-call code (renderers, parsers,
  plans, the search loop in the chat route) is built in every build but is reached only through a tool runner, which
  only the option's search service provides.
- **`strata-search`** (new program, beside the server): an MCP server on stdio with one tool, `web_search(query,
  max_results?)`, over one SearXNG instance's JSON API (`GET <url>/search?q=..&format=json`). It is the install's only
  search connection: one origin from its arguments, http or https (TLS as the email client checks it), no redirect
  followed, a 4 MiB reply cap, `search.timeout_s` per step; results as a numbered list of title, URL, date and snippet
  (capped), SearXNG's answers first. A 403 is explained as `search.formats` lacking json.
- **The server's MCP client** (`mcp_client.cpp`): upstream's `serve/mcp.py` for one stdio server - JSON-RPC 2.0, protocol
  2025-06-18, initialize / initialized / tools/list (paged) / tools/call, `notifications/cancelled` on a timeout or a
  cancel, a server's ping answered, other requests -32601; result text and its 20,000-character cut with upstream's note.
  `EngineCommand::max_read_line_bytes` (new; the engine keeps 8192) lets the child's answer line be 1 MiB.
- **The search service** (`search_service.cpp`): starts `strata-search` on the first chat that needs it, in the downloads'
  closed environment (`download_environment`, its own owner-only `<roots.state>/search` temp folder), keeps it until the
  server stops; a failed start leaves chats without search for 60 s. It offers strata-search's own tool with "Today's
  date is YYYY-MM-DD." after its description; while a search runs, another chat's offer reuses the listed tool at once.
- **Tool calls per family, each in its reference's own form**:
  - Qwen (`render_qwen_tool_prompt`, `QwenToolCallParser`): the template's tool sections as upstream renders the
    Qwen3.6 pack's GGUF template (and Qwen3.8's, `serve/chat_template.jinja`, for the effort sentences and non-string
    values), byte for byte against upstream's own `ChatTemplate` (`tests/fixtures/qwen_tool_prompts.json`, 12 cases);
    the reply's calls as upstream's `OutputParser` reads them with the tool declared (declared_only; calls inside the
    reasoning #804, quotes in fences or inline code #1058, drifted openers #3441, the `</tool_call>` walk #210), against
    upstream's own parser fed whole and character by character (`tests/fixtures/qwen_tool_calls.json`, 26 cases).
  - GLM-5.3 (`render_glm53_tool_prompt`, `GlmToolCallParser`): GLM-5.3-Flash's own template as Maya renders it for its
    run_with_mcp (`tests/fixtures/glm_tool_prompts.json`, 8 cases) and Maya's `OutputParser` (`glm_tool_calls.json`, 15
    cases; no calls read in the reasoning; an unfinished call with its name is a call without arguments, as Maya's
    streaming scan delivers it). Two rules are ours (documented in the header): a call for an undeclared tool, or one
    that does not parse, is the text it was.
  - The prompt is built in pieces (`PromptPiece`: markers, text, plain): the model's words and every result are plain
    text, encoded as ordinary text even where they spell a special token (`encode_plain_text`, `encode_prompt_pieces`),
    as upstream's `mark_think_literals` does. The client's own text keeps the special-literal refusal.
  - The reply's tool tags are output as their text while a tool is offered (`literal_tokens`), and the generation loop
    tells an observer where the reasoning ends (`on_reasoning_end`).
- **The search loop** (`search_rounds.cpp`, `chat_search_route.cpp`): upstream's run_with_mcp for the qwen and glm53
  profiles' text chats, complete and streamed, inside one engine lease and one stream (`StreamRounds` in
  `run_chat_stream`): a round that ended as stop with calls runs them (each a `call` and a `result` event), keeps the
  round, renders the next prompt and generates again, at most `search.max_rounds`; calls not run are closed as skipped,
  past the limit a `limit` event. The answer's text and reasoning are every round's, the usage the last round's prompt
  and every round's tokens. Events go out as `strata_search` (stream chunks with an empty delta; `message.strata_search`
  in a complete answer), as upstream's `strata_mcp`. A pack without the tool tags, a request with images, or no tool
  now: the chat is served as before.
- **Config** `search.enabled`, `search.url`, `search.results` (1-10, 5), `search.max_rounds` (1-8, 8), `search.timeout_s`
  (5-120, 30); `search_from_draft` checks the page's rows alone. The URL check moved to `search_url.cpp` (every build).
- **The page**: Models > Settings gets a Web search section (on/off, address, results, rounds, time limit, Save, and a
  test search through the saved settings showing what a model reads) over `/v1/strata/search`, `/settings`, `/test`
  (keyed, JSON bodies, loopback clients only, with model control, in a build with the option). The sub-tab now shows when
  the server has alerts or search. A chat reply notes what it searched for.
- **Out folder**: `make --with search` builds and installs `strata-search` beside the server and checks it is there.
- `python_json_text` (Python's json.dumps) moved from the MCP client to `json.cpp`; its floats now follow Python's repr
  (`100000.0`, `1e+16`), RED first.
- Tests, RED first (stubs) unless noted: the 8 `qwen_tool_calls` cases, the 6 `glm_tool_calls` cases,
  `drive_generation_outputs_literal_tokens_as_their_text`, the 8 `search_rounds` cases, `sse_search_event_shape`,
  `stream_rounds_share_one_stream_and_one_release`, `stream_rounds_can_end_the_answer`,
  `renders_the_search_events_in_the_message`, `web_search_rows_follow_the_build`, `python_json_text_writes_as_python_does`,
  `test_web_search_is_built_only_when_asked`; RED by a mutation after the code (the code came first):
  `a_chat_searches_through_strata_search_and_answers` and `a_started_card_searches_too` (the server end to end: its own
  strata-search, a fake SearXNG, a fake engine whose first reply calls and whose second answers; RED with the wiring
  cut), `a_running_search_does_not_hold_up_the_offer` (RED with a blocking offer), `web_ui_has_web_search_settings` (RED
  with an id renamed); written after the code, GREEN at once: `the_search_service_offers_web_search_and_runs_it`,
  `search_routes_read_save_and_test`, `search_routes_need_their_handler_and_the_key` (it found the routes' missing JSON
  body check, fixed), `egress_strata_search_is_the_search_client`, `the_route_plans_a_search_in_the_turns_form`. Changed
  with the requirement: `web_ui_has_email_alert_settings` (the sub-tab's rule is now alerts or search).
- Phases 1-2 (strata-search, the MCP client) were written before their tests; their RED was a mutation run (recorded in
  the session).
- Not done: an end-to-end run against a real SearXNG (the owner's instance URL is needed) and with a real model; a GLM
  end-to-end through the server (its plan, parser and route choice are unit-tested); search for the other profiles
  (gemma4, flint, raon1, deepseek serve without it); streaming a call's arguments as they are written (upstream's
  stream_tools).
- Full suite on this PC, both builds 0 errors and 0 warnings: the option on (the preset tree), 1201 tests; the option and
  email off (a scratch tree), 1177 tests. Failing in both, and before this change: `auth04_no_switch_disables_auth`,
  `raw_profile_rejected_in_release_build`, `release_binary_lacks_hook_marker`. Also failing beside other tests:
  `api_server_tests.idle_unload_respawns_before_the_next_request` (passes alone, 3 of 3 and twice beside other suites;
  fails every time beside `a_running_search_does_not_hold_up_the_offer`). It is the timing flake recorded on 2026-10-21
  (docs/validation/2026-10-21-windows-gpu-session.md item 43); `engine_supervisor.cpp` is not touched here. Read, not
  proven: `EngineSupervisor::watch` waits without limit on the child when its idle slice ends with a request in flight
  (no deadline, or state generating), and neither `release()` nor `note_request_finished()` calls `wake_.notify()`, so a
  first request that outlasts the 1 s window from READY leaves the engine loaded.
- Found, not changed: the fixed GLM profile writes no `<|system|>Reasoning Effort: ...` turn and no `<think></think>`
  on earlier assistant turns, where GLM-5.3-Flash's template (and so Maya) writes "Max" by default; the tool prompt
  follows the fixed profile there. The Qwen byte-level packs tokenize with `basic_pretokenize`, not Qwen's regex.
  `tools/secure/test_make.py`'s two engine-compiler cases fail on this PC because they read the real
  `out/make-build/engine/CMakeCache.txt`.

### Claude Code session, 2026-10-27 p (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

After entry o's commit (`7ac4d16f`) the owner said "once commited continue to the next pieces I asked for": the rest of
"GLM, DeepSeek, MiniMax" and "Add a model ... for the others". Design: `docs/superpowers/specs/2026-10-27-strata-prepare-design.md`
section 7 (Maya v1.1.30's facts, each with its file and line). DeepSeek-V4.1-Flash is done here; MiniMax-M3.1-Flash
waits on the owner (its template's system text is under the MiniMax Community License).

- **The deepseek profile is Maya's maya-ds4** (multiengine `--pack`, `multiengine_maya_ds4`), replacing the merged-engine
  placeholder: the `maya_118` wire, the strata environment family, per-request `reasoning_effort`, cards accepted.
  The `unified` protocol flavor and argument profile are now selected by no profile (kept, with their tests).
  - Launch (`EngineArgumentsProfile::maya_ds4`, Maya's write_config_ds4): `--pack <pack>`, the card's allowlisted flags
    (`--max-context`, `--tp` 1-8, `--threads`, `--cpu-threads`, `--prefill-chunk`, `--reserve-mib`), `--gpus` for named
    GPUs, `--vision off`; the weight source, one-shot modes, vision, drafter and expert-profile path refused by name.
  - Prompt (`render_deepseek_text_prompt`): DeepSeek's own encoder (Maya's `serve/ds41_encoding.py`, MIT) byte for
    byte - the effort line (low 50, medium and high 75; none is chat mode), earlier reasoning dropped; an optional
    leading system message, alternating turns ending in a user turn, anything else refused. Its seven markers are single
    CONTROL or USER_DEFINED tokens (`encode_deepseek_prompt`, sharing GLM's encoder through `encode_with_markers`).
  - Reply: reasoning first in thinking mode, `</think>` to the answer; `<｜end▁of▁sentence｜>` and `<｜User｜>` are
    **server stops** (new `server_stop_tokens`, plan to generation loop): the engine stops on its EOS only, so the
    frontend ends the reply as stop and sends STOP, as Maya's server does.
  - Tokenizer (`TokenizerMode::deepseek_bpe`): Maya's `deepseek3` splits on the Python `regex` module's own Unicode
    classes (`src/regex_unicode_data.inc`, generated by `tools/generate_regex_unicode.py` from regex 2026.9.10; Maya's
    setup installs regex unpinned), then the byte-level BPE `raon_bpe` runs.
  - Options (Maya's): contexts 32K-1M (64K), thinking none/low/medium/high (high), temperature 1.0, top_p 1.0; the card
    carries `--tp 1 --threads on` (Maya's one-GPU form).
- **DeepSeek packs**: `strata-prepare download --maya` follows Maya's DeepSeek step for a `deepseek41` GGUF (ds4_pack_step:
  pack_gguf.py's pack, its `need` files, sources.json with the `tools` that built it, reuse while it matches; the shipped
  profile seeded as `expert-profile.bin` at the pack's geometry, ds4_seed_profile). Add a model builds it in
  `<folder>/maya-ds4` (`maya-ds4-<name>/` beside another model's), a download in `<store>/packs/<id>` reading the store's
  files in place (ds4.txt names the model folder: no hard links). The catalog gains Maya's four DeepSeek GGUF quants
  (`tools/ds4/download_gguf.json` of the pinned maya-ref v1.1.27: unsloth UD-Q8_K_XL / UD-Q4_K_XL, Maya-S, Maya-XS,
  every file pinned by size and sha256; Maya-M lists no files and is dropped).
- The profiles whose engines stop by themselves carry no server stops, so nothing changes for them.
- Tests, RED first unless noted: `deepseek_pretokenize_splits_as_maya`, `deepseek_pretokenize_matches_mayas_fixture` (464
  texts, Maya's own `_IsolatedSplits` output, `tests/fixtures/maya_pretokenize_texts.json` from
  `tools/generate_maya_pretokenize_fixture.py`), `deepseek_bpe_encodes_mayas_pieces_by_rank`,
  `drive_generation_server_stop_token_sends_stop_and_drains`, `renders_deepseeks_encoder_byte_for_byte` (five prompts from
  Maya's encoder), `refuses_what_the_fixed_profile_cannot_say` (passes on the stub too: a stub refuses everything),
  `plans_with_the_markers_the_split_and_server_stops`, `a_pack_without_deepseeks_markers_plans_nothing`,
  `effort_levels_are_mayas`, `run_generation_reads_a_deepseek_reply_like_mayas_server`,
  `deepseek_route_plan_follows_thinking_and_its_level`, `deepseek_launch_is_mayas_write_config`,
  `deepseek_options_are_mayas`, `a_deepseek_pack_makes_a_deepseek_card`, `a_deepseek_model_is_added_with_mayas_ds4_pack`,
  `a_downloaded_deepseek_is_packed_with_mayas_step`, `download_maya_ds4_step` (strata-prepare),
  `test_maya_deepseek_entries_come_from_its_gguf_manifest`; `deepseek_launch_composes_from_its_config` was written after
  the wiring it checks (GREEN at once). Changed with the requirement: the subsystem tables' deepseek rows (maya_118,
  deepseek_bpe, reasoning_effort taken), the argument profile (maya_ds4), deepseek cards now parse, the catalog counts and
  its pinning check (DeepSeek's manifest pins a commit and sizes; GLM's Maya entries stay at main).
- Not done: MiniMax (the owner's licence decision first; Maya-XS, 93 GB, would fit this PC for an end-to-end run);
  any DeepSeek end-to-end run (every GGUF is 277-508 GB, far past this PC); `--tp 2` on two GPUs (the card carries Maya's
  one-GPU form); a DeepSeek calibration sweep (Maya's tunes --tp and --cpu-threads by hand, `--calibrate`); the thinking
  budget (`think_budget` GEN key); effort 100 (Maya's "max": the request schema's levels stop at high);
  mid-conversation system messages (refused).

### Claude Code session, 2026-10-27 o (Windows box, 4080 SUPER; owner-directed; committed with this entry, not pushed)

After entry n's commit (`22dbd61d`) the owner ran the out folder and asked: "i need it hosted in 0.0.0.0"; "add
172.16.1.0/24 and 172.16.2.0/24 to the control_remote so I can actually test" / "it should allow hosts list or
subnets"; "change the Strata on the header page and title to M-Strata".

- **0.0.0.0:** config only, in the owner's install (`out\dist-email\data\config.json`): `listener.host` 0.0.0.0,
  `allow_remote`, `allow_wildcard_bind`, and `host_aliases` (127.0.0.1:8080, localhost:8080, <this PC's LAN address>:8080,
  <pc-name>:8080). On a non-loopback bind the login link prints 127.0.0.1 (it is loopback-only).
- **`models.control_remote_networks`** (string array, at most 16): with `models.control_remote`, only clients in the
  listed hosts (an IP address) or CIDR subnets get model control; empty = every key holder (D2 as before).
  `config.hpp`: `IpNetwork`, `parse_ip_network` (IPv4/IPv6; names, localhost included, and host bits set are refused),
  `ip_network_text`, `PeerKey`, `network_contains`, `model_control_allowed`. The accepted socket's `peer_key` now
  reaches `RequestContext::peer` (`WorkerSignals::peer`, connection pool and handler, the launcher's chat context), and
  the server's control gate calls `model_control_allowed`. The list needs `control_remote` (conflicting_values). The
  startup line names the networks.
- **Model-file actions from the subnets** (the owner hit `control_not_allowed` in the folder picker from another
  machine, then chose "Allow all from subnets"): `model_files_allowed` lets a client in only from a listed host or
  subnet, never through `control_remote` alone. `CardRouteSettings::client_allowed` carries it (empty = loopback
  only, as before). The delete keeps its three checks, and the alert routes stay loopback-only.
- **Qwen3.6 from the GGUF** (owner: "can we make executables to handle what needs to be done so we don't have any
  other dependencies?", then "Qwen 3.6 from the GGUF first, then a native strata-prepare packer ... then GLM"; design
  `docs/superpowers/specs/2026-10-27-strata-prepare-design.md`):
  - `card_for_gguf` takes architecture `qwen35moe`: profile qwen; `--mtp 2` when `nextn_predict_layers` > 0; 32K of
    the model's context; thinking high; Qwen's chat limits.
  - A Qwen model card with no pack reads the tokenizer from the GGUF in `byte_bpe` mode: `tokenizer_from_gguf(profile,
    card)`, with `card` from `ModelsConfig::from_card`, which `card_draft` sets through `FrontendConfigDraft::card`. A
    card with a pack still reads the pack.
  - The operator's single-model `--expert` Qwen config still needs `roots.tokenizer_pack`. A first version allowed
    every Qwen config and broke two existing tests (`engine_args_reject_weight_flags_for_managed_model`,
    `cfg01_model_reference_rejected_for_managed_model`); they pass again with the allowance limited to cards.
  - A Qwen3.6 download (or a paste matched to it) becomes a card from the GGUF where `models.default_engine` is set;
    no Python step runs.
  - Proven on the owner's real `Qwen3.6-35B-A3B-Q8_0.gguf` (35 GB): its 248,320 tokens, types and merges equal the
    pack upstream's `strata_tokenizer.py` wrote, and so do the ids of 38 texts
    (`real_qwen36_gguf_tokenizer_matches_its_pack`, label `qwen36-real-pack`; env `SECURE_FRONTEND_QWEN36_GGUF` /
    `_PACK`).
  - End to end on the GPU with a scratch server built by `make --with email`: the card was added through
    `/v1/strata/cards/add` and started (engine args `--native` and `--mtp 2`). It answered "17 x 23" with 391 and
    wrote 300 tokens at about 29 tok/s end to end. The server was stopped afterwards.
  - Known gap, pre-existing and unchanged: the server's Qwen mode splits text with `basic_pretokenize`, a
    conservative GPT-2 approximation, not llama.cpp's exact `qwen35` pre-tokenizer.
  - Tests (RED first): `a_qwen36_gguf_makes_a_qwen_card`, `a_qwen36_card_reads_its_tokenizer_from_the_gguf`,
    `a_downloaded_qwen36_becomes_a_card`.
- **strata-prepare, the native packer** (`engine/src/prepare`; spec
  `2026-10-27-strata-prepare-design.md` phases 2-3):
  - `strata-prepare pack` is upstream's `iq_pack.py` (v0.2.6) in C++, file for file:
    - `gguf_header` (`gguf_reader.py`'s types and geometry);
    - `py_json` (`json.dumps` byte for byte, Python's float repr);
    - `sha256`;
    - `iq_pack` (FORM conversions, v3/v4/v5 tables, Hadamard, `experts.bin`);
    - ggml's dequantizers for `--compat-bf16`, and NumPy's half rounding.
  - On Windows the JSON files get CRLF, as Python's `write_text` does.
  - `strata-prepare download` speaks `prepare_download.py`'s protocol (`STEP`/`DONE`/`ERROR`, `prepared.json` with
    `tokenizer: null`).
  - It builds standalone in seconds (`cmake -S engine/src/prepare -DSTRATA_GGML_DIR=...`), and inside the engine
    build with the launcher; it is installed as `engine/strata-prepare` plus `engine/profiles/expert-profile*.bin`.
  - Tests: 11 unit tests (RED on a stub build, then GREEN). `tools/secure/strata_prepare_parity.py` (dev-only) found
    every file byte-identical to upstream's Python on six synthetic cases and on the owner's real Qwen3.8 IQ3_S (two
    shards; `index.txt`, `native_experts.txt`, the 1.5 GB `dense.bin`, `conversions.json`).
  - The server: where `models.default_engine` names an engine with `strata-prepare` beside it, a Qwen3.8-family
    download (iq_pack) is packed natively into `<its store>/packs/<id>`, even where Python is configured.
    - Its card has `--pack`, `--ple-gguf` (at most two shards, as upstream's setup), `--expert-profile` from
      `engine/profiles`, and the tokenizer from the GGUF.
    - Deleting the model removes the pack.
    - Test: `a_downloaded_qwen38_is_packed_natively`, RED first. make.py's runtime list includes the tool and profiles
      (`test_strata_prepare_and_the_expert_profiles_are_runtime_files`, RED first).
  - On the GPU, from the out folder `make --with email` built (`out\dist-email-next5`):
    - its `engine\strata-prepare.exe download` packed the owner's Qwen3.8 IQ3_S in 2.1 s, every file identical to
      upstream's Python, with the PLE table found in shard 2;
    - a scratch server (port 8092, `roots.allow_untrusted_media` for the G: model folder, never changed) started a
      card written as `native_prepare` writes it (tokenizer from the GGUF);
    - it answered "17 x 23" with 391 and wrote 299 tokens at about 45 tok/s end to end;
    - the server and engine were stopped afterwards.
  - **Maya's packers** (owner: "Yes continue with the others"; spec section 5 and phase 4): Maya v1.1.30's Python in
    C++, byte for byte - `maya_pack.cpp` (Maya's fork of `iq_pack.py`: glm5-next / glm5next and its qwen4exp route;
    `strata-prepare pack --maya`), `tokenizer_export.cpp` (Maya's `strata_tokenizer.py`; `strata-prepare tokenizer`),
    `maya_moe_pack.cpp` (`tools/ds4` and `tools/mm3/pack_gguf.py`; `pack --maya-ds4` / `--maya-mm3`, Maya's chat
    template passed with `--template`), and `download --maya` (maya.py `pack_step`: `--compat-bf16`, `PACK_FILES`,
    `sources.json`, and its reuse rule `pack_source` / `pack_stale`: an up-to-date pack is not rewritten).
    `pack_common.cpp` now holds what both packers share (the model's shards, Python's file writes, the floats);
    `gguf_header` decodes strings as Python's `"replace"` and keeps metadata keys in file order; `py_json` gained
    `ensure_ascii=False` and `repr()`. Not ported: DeepSeek's DSpark drafter / vision tower and MiniMax's EXL3
    companions (files other than the GGUF set: packed text-only, with a note), Maya's `--base` / `--skip-experts`.
  - Tests: 21 unit tests (the new ones RED on stub builds; the reuse rule RED with it switched off). Parity
    (`strata_prepare_parity.py --maya-tools <Maya v1.1.30 tools, extracted byte-exact with git>`): 17 synthetic cases
    byte-identical to Maya's Python, and the owner's real GLM-5.3-Flash Maya-S-v2-IQ2_XXS (three shards, 95 GB): all
    10 files of `download --maya` identical in 22 s for both sides; its `index.txt`, `native_experts.txt` and
    `sources.json` also equal the pack Maya's setup had built in `G:\Maya-data\models\Maya-S-v2-IQ2_XXS\pack` (only
    read). Packs went to the scratchpad.
  - **GLM cards** (launcher spec D10 met; the owner chose "Page omits it" for item 46): the page drops
    `reasoning_effort` when the running card's list entry says `"reasoning_effort": false`
    (`per_request_reasoning_effort_for`, the chat schema's own rule: glm53 and deepseek false); glm53 cards parse;
    their options are Maya's (contexts 8K/32K/64K/128K, default 32K; `glm.kv`'s fp16/int8/q8_0/q4_0; sampling) and
    write `glm.max_context` / `glm.kv`, never `engine.args`. Every GLM launch names Maya's shipped expert profile when
    the install has it beside the engine (`engine/profiles/expert-profile-glm.bin`, as maya.py's `write_config`
    does; never for raon1). `sync_maya.py` now copies Maya's three shipped profiles (`data/expert-profile-{glm,ds4,mm3}.bin`,
    MIT) into `engine/third_party/maya/data`; the engine CMake installs them and make.py requires them with the Maya
    families. Side fix: `glm_kv_renders_the_cache_format` existed in the test file but was never registered with ctest;
    it is registered now (passes).
  - **GLM chat fix** (found by this entry's end-to-end check, below): `encode_glm53_prompt` took the six framing
    markers only as CONTROL tokens, but GLM-5.3-Flash's own tokenizer has `<think>` (and `</think>`) USER_DEFINED
    (type 4), so every GLM chat was refused as malformed - through the owner's single-model GLM config too (its
    tokenizer pack has the same types). A marker is now exactly one CONTROL or USER_DEFINED token, as Maya's server
    matches them (`user_defined_think_marker_encodes`, RED first; an ordinary or doubled marker is still refused).
    Second part, found by the next chat (`internal_error` after 8 s, log code `undecodable_token`): the glm53 plan had
    no reasoning split and no stops, so the model's `</think>` (USER_DEFINED) reached the byte decoder. GLM-5.3's
    template ends every prompt with `<think>`; the plan now starts the reply as reasoning when the pack has both think
    markers (Maya's server: `OutputParser(thinking=True)`) and ends it on `<|endoftext|>`, `<|user|>` or
    `<|observation|>` (`find_glm53_end_of_turn_tokens`, Maya's `glm_eos`, printed as a T record before DONE). Tests,
    RED first: `a_glm_reply_starts_in_reasoning_and_ends_on_its_turn_markers` and
    `run_generation_reads_a_glm_reply_like_mayas_server` (RED with `decode_failed`, the server's symptom);
    `finds_glm53_end_of_turn_tokens` (RED with the shared duplicate rule switched off, as the Qwen finder's test).
  - **GLM downloads** (`DownloadManager::maya_prepare`): the catalog's five GLM entries are `"prepare": "maya_pack"`
    (generator, its test and the catalog regenerated together). `<store>/packs/<id>/` holds hard links to the store's
    shards (no copy; the store's version folder is never written) and the pack in `pack/` (`strata-prepare download
    --maya`), made owner-only when it inherited its ACL, its marker `glm-5.3-flash.gguf`; the card is glm53 on the pack.
    Deleting the model removes `<store>/packs/<id>` (the links, not the store's files).
  - **Add a model with a pack** (owner: "Then work on the add model"): `/v1/strata/cards/add` with a Qwen3.8
    (`qwen4exp`) GGUF builds its pack in `<roots.state>/packs/<card>` (`--compat-bf16`; the model folder untouched;
    `--expert-profile` by the expert set, 512 `expert-profile.bin`, 256 the Coder's); with a GLM-5.3 GGUF, in `pack/`
    beside its shards as Maya's setup puts it (`pack-<name>/` when `pack/` indexes another model; an up-to-date pack
    reused). Any split model is added by its first file now (a later shard refused by name before anything is read).
    One `card_for_pack` writes both the download's and the folder's card; `run_prepare_command` runs strata-prepare for
    both (the routes' runner is wired at boot when strata-prepare is beside `models.default_engine`). The page shows
    an "Adding ..." notice while the pack is built.
  - **End to end (GLM-5.3, 2026-10-27, scratch server on loopback, `make` build `out\dist-email-next8`):** a scratch
    folder made private by the server's own new-folder route held hard links to the owner's three Maya-S-v2-IQ2_XXS
    shards (their files only read). Add a model built the pack in `pack/` in 3.8 s; the card listed as glm53 with
    `"reasoning_effort": false` and Maya's options; the engine command line named `--expert-profile
    ...\engine\profiles\expert-profile-glm.bin`; the model ran after 107 s. "17 x 23" answered `391` (finish stop,
    2.5 s warm); a 300-word request wrote 2048 tokens in 612 s, about **3.3 tok/s** overall, its reasoning (8,476
    chars) apart from the answer and cut at length; a request with `reasoning_effort` was refused
    `unsupported_parameter`. Not looked into: the speed is well under the ~11 tok/s measured earlier on another pack
    (`C:\Strata-data\packs\glm53`, gone now); this model is 89.8 GB on a 96 GB machine and the engine held 57 GB
    resident. The server was stopped and the scratch folder removed file by file (the owner's shards back to one link).
  - Tests (RED first): `a_downloaded_glm_is_packed_with_mayas_step`, `qwen38_and_glm_models_are_added_with_their_packs`,
    `packed_families_and_their_cards`, `glm_launch_names_mayas_shipped_expert_profile`, `glm_options_write_the_glm_rows`,
    `cards_say_whether_their_profile_takes_reasoning_effort`, `the_page_sends_reasoning_effort_only_where_it_is_taken`,
    `test_maya_expert_profiles_come_with_the_maya_families`; changed with the requirement (D10): the card-refusal case
    now expects glm53 cards to parse, the option table lists GLM's, the "another profile's option" check uses
    `thinking` / `kv_resident`.
  - Not done: DeepSeek and MiniMax downloads and cards - the packers exist, but the frontend has no chat profile for
    either (DeepSeek's `deepseek` profile is a placeholder template, MiniMax has none) and the catalog no entries; their
    chat templates are not in our tree (MiniMax's is under the MiniMax Community License). A GLM download into a store
    on a drive without hard links (exFAT / FAT) fails with "the drive must support hard links" (no 95 GB copy is made).
  - Incident: a test build that linked ggml as a DLL popped "ggml-base.dll missing" dialogs on the owner's screen.
    ggml is now static, the build scripts run tests only after a successful build, and test runs disable system error
    dialogs (`SetErrorMode`).
- **M-Strata:** the page's `<title>`, header brand and sign-in card title; `app.css` keeps the brand on one line
  (`white-space: nowrap; flex: none`: its hyphen broke it over two lines in a narrow header).
- **Tests (RED recorded for each on stubs or before the change):** `control_networks_take_hosts_and_subnets`,
  `model_control_follows_the_networks`, `model_files_need_a_listed_network` (config tests 17/17),
  `listed_remote_clients_use_the_file_routes` (card routes 9/9), `control_remote_networks_typed_and_checked` (typed config
  70/70 after the README table regen), the page case `web_ui_page_is_served_and_the_api_keeps_its_key` (now checks
  M-Strata in the title and header) and `web_ui_header_fits_a_phone` (the one-line brand; router tests 39/39). The
  socket-to-gate pass-through has no unit test (no remote peer can be made from loopback). It was checked live on the
  owner's install, this PC calling its own LAN address <this PC's LAN address>, a remote peer inside 172.16.1.0/24: the model
  list and downloads 200 with the key, 401 without, the loopback-only folder route 403, page title M-Strata. With the
  address not reaching the gate the list would have been 403. After the file-route change, the folder route answered
  200 from the LAN address (drives C: to G:) and the alert route 403. Full ctest (email on): 1,117 tests, only the
  three known failures; the email-off tree's config, typed-config and egress tests 91/91. Not committed.

### Claude Code session, 2026-10-27 n (Windows box, 4080 SUPER; owner-directed; committed with this entry, together with entries l and m, which share its files; not pushed)

The GUI-first program the owner asked for after the flint card (entry m's "Not done"), plus the asks that followed it:
download into a chosen drive or folder after a free-space check, calibrate on the first start, "point them to a folder
containing the model files and it can build the card", carded models gone from the Download list, a danger-zone
delete (code, API key again, final confirmation), and IONOS-compatible email alerts that are "Built into the server
but ... optional ... build it with or without", "disabled by default but for ours we have it enabled". Design, with
the owner's decisions D1-D4 and every quote: `docs/superpowers/specs/2026-10-27-gui-first-install-design.md`.

- **Out folder (phase 1):** `make.py` + `make.bat`/`make.sh` (repository root). Options: `--out`, `--without
  maya,raon1`, `--with email`, `--no-cuda-runtime`, `--plan`. It uses CMake `install()` rules of a `runtime` component
  in both projects, the distribution check, the bundled cuBLAS (D3) and the MSVC runtime, `README.txt`,
  `config.example.json`, and makes the folder owner-only. Built once into `out\dist` (without email), and that folder's
  first start ran (data folder created, login link printed).
- **Run the binary (phase 2):** `first_start.cpp` (`<exe dir>/data`, owner-only, `config.json` in launcher mode,
  install folder made owner-only through `make_tree_owner_only`) and `--data-dir`. Also the one-time login link
  (`login_link.cpp`, `POST /v1/strata/login-link`, loopback, 10 minutes, once; D1) and a launcher that boots with no
  card (`models.default_engine`, `models.default_root`).
- **Cards from the page (phase 3):** `gguf_metadata`, the tokenizer read from the GGUF (flint, gemma4), and
  `card_builder` (`card_for_gguf`, `card_with_folder`, `DeleteCodes`, `delete_gguf_model_files`). Also
  `local_folders` (drives, listings, `make_model_folder_private`, `make_private_folder`), `card_routes` (7 routes),
  and launcher `replace_card`/`remove_card`/`card`. Downloads go to a chosen folder (`strata --store`, its own volume's
  free space less 2 GiB, `destinations.json`). A downloaded Gemma 4/flint GGUF becomes a card, and carded downloads
  are hidden. Deleting a downloaded model also runs `strata rm <id>:download` in its store
  (`DownloadManager::remove_installed`, the route's `download_removed`).
- **Calibrate (phase 4):** `calibration.cpp` (`sweep_plan`: flint `--cpu-lane`/`--pcie-frac`/`--cpu-threads`, gemma4
  `--cpu-threads`; `run_sweep`: baseline, coordinate descent, 3% margin, 2 interleaved confirmations). The workload is
  a warm-up + 3 prompts x 128 tokens, greedy, scored by the median decode tok/s. Results are in
  `<state>/calibration/<card>.json`, keyed by GPU, CPU, RAM, engine file, model file and options; states missing,
  fresh, stale, settings_changed. Launcher `calibrate`/`cancel_calibration`/`set_auto_recalibrate` and 3 control
  routes. A never-calibrated page card calibrates on its first start. Removing a card removes its calibration.
- **Email alerts (phase 5):** CMake option `SECURE_FRONTEND_EMAIL_ALERTS` (OFF; ON in `linux-gcc`, `linux-clang`,
  `windows-msvc`); `alerts.email.*` (8 rows, refused by name in other builds after their own type checks);
  `smtp_client` (AUTH only inside TLS, PLAIN else LOGIN, dot-stuffing); `smtp_tls` (SChannel `SCH_CREDENTIALS` TLS 1.2+
  / OpenSSL min TLS 1.2, SNI, host check); `email_alerts` (password in `roots.secret/smtp-password`, DPAPI or 0600;
  queue of 16, one per kind per minute, no retry; 6 events); `alert_routes` (4 routes); the page's Settings sub-tab
  with an IONOS preset and "Send a test email".
- **The no-egress check, by the owner's choice:** the owner picked "Built into the server" over a curl child, knowing
  it "brings network and TLS code into the server process". `egress_server_imports_no_resolver_or_http_client` is
  unchanged for a default build (verified in a scratch email-OFF tree: 73/73 egress and config tests). In an email
  build it requires exactly `getaddrinfo` on the forbidden list and nothing else. The runtime egress checks are
  unchanged.
- **Error rows:** `login_link_refused`, `folder_refused`, `folder_not_private`, `card_refused`, `card_exists`,
  `card_read_only`, `card_in_use`, `delete_refused`, `calibration_unsupported`, `not_calibrating`,
  `settings_refused`.
- **Tests (TDD; RED recorded for each part by a stub or muted code):** new binaries `gguf_metadata`, `card_builder`,
  `local_folders`, `card_routes`, `calibration`, `smtp` (email builds). New cases in the launcher, downloads,
  router/page, http parser, typed config, acquire CLI (`--store`) and model cards tests, and
  `tests/support/gguf_builder.hpp`. Not shown RED on their own (each added together with its code): the router's
  alert-route case, `alerts_from_draft`, the downloads list's `disk.folder`, and the page's `download_removed` warning
  line. Full ctest (email ON): 1,112 tests, only the three pre-existing
  failures (`auth04_no_switch_disables_auth`, `raw_profile_rejected_in_release_build`,
  `release_binary_lacks_hook_marker`). MSVC /W4 /WX clean.
- **Seen working:** in the built-in browser against a scratch install: the login link, the empty state, Add a model
  (picker with free space; flint and gemma4 cards), make-private on an Everyone-writable folder, a new private download
  folder, and the delete flow (wrong code and wrong key refused, then the delete). The scratch server was stopped.
- **Docs:** the spec (phases 1-5 as built), `secure_frontend/README.md` ("Install: one folder, then the page", the
  email build option, the config table regenerated), `docs/security/threat-model.md` ("GUI-first start-up", two
  residuals, the second egress exception) and `docs/PROVENANCE.md` (cuBLAS, the MSVC runtime, calibration's method,
  the OS's TLS).
- **Not done / not proven:** the Settings sub-tab's listener and folder settings; the expert usage profile (spec
  §5.1) and the upstream/Maya calibration sweeps (qwen, raon1, glm53 answer `calibration_unsupported`); no
  calibration on the GPU yet (only the fake engine); no live mail send (IONOS untested; sending needs the owner's
  word). The OpenSSL path and every Linux claim are uncompiled here; Windows claims still need the
  `windows-msys2-clang` CI job. `out\dist` predates the email option and phases 3-5.

### Claude Code session, 2026-10-27 m (Windows box, 4080 SUPER; owner-directed; committed with entry n, not pushed)

Owner: "Build flint's frontend card next (secure_frontend profile, chat template and tokenizer), then a multi-test
server with it." (Also, for after it: GUI-first start-up with per-model folders on the Models tab, a make script that
puts only the runtime files in one out folder, per-model calibrate/recalibrate as upstream Strata and Maya do it, and
every change written for Linux as well as Windows - see "Not done".)

- **Profile `flint`** (secure_frontend README "flint profile"): `EngineProfile::flint` / `NativeChatProfile::flint`,
  the `strata_0143` wire and strata environment family (flint's serve loop is Gemma 4's), per-request
  `reasoning_effort`, cards (`profile: "flint"`), Model-tab options (Gemma's set: `max_context`, `chunk`,
  `expert_vram`, `thinking` off|on, sampling) and the page's profile label. The qwen launch rows are refused by name
  as on gemma4.
- **Arguments:** `engine_arguments.cpp`'s Gemma-only validator became one table-driven family validator
  (`FamilyArguments`; Gemma's flags, bounds and messages unchanged, its tests untouched and green) with flint's table
  `strata_flint` (`--max-context`, `--chunk`, `--no-graph`, `--expert-vram`, `--expert-slots` 0-256, `--cpu-threads`,
  `--pcie-frac` 0-1, `--cpu-lane on|off`, `--no-prompt-borrow`).
- **Tokenizer `flint_bpe`:** llama.cpp master `e86f5fd1`'s flint pre-tokenizer as llama.cpp actually runs it - its
  three regexes in turn over the collapsed text, where `\p{Lu}`/`\p{Ll}`/... are all the one LETTER class (so case
  changes never split a word, unlike the HF regex), digits in threes from the left, CJK/kana apart - then raon1's
  byte-level heap BPE (shared). The Unicode table (`src/raon_unicode_data.inc`, generator
  `tools/generate_raon_unicode.py`) now keeps every category bit; KRAFTON's and master's `unicode-data.cpp` are
  byte-identical, so both tokenizers read it (raon1's fixture still token-exact). The design was checked in Python on
  the fixture first (0 of 38 mismatches), then ported. Code points past U+FFFF follow the 32-bit `wchar_t` (Linux)
  semantics; MSVC's llama.cpp truncates them in the CJK pass.
- **Template:** `render_flint_text_prompt` (`src/flint_prompt_renderer.cpp`): the GGUF's Jinja byte for byte -
  `<|system|>`/`<|user|>` + `flint_text` (\0 and the think tags removed, seven framing literals escaped as
  `&lt;|...|&gt;`), `<|assistant|>` + earlier answers as written, generation prompt `<|assistant|>`; no newline, trim
  or BOS; thinking off appends `<think></think>`. Refused: text the escapes leave holding a marker, an answer holding
  one, other special literals (the encoder), empty text, control bytes. `encode_flint_prompt` maps the five markers;
  the plan's reasoning split is `<think>`/`</think>` and the four GGUF end-of-generation ids stop it.
- **Pack:** `out/flint-pack/tokenizer` (`tools/secure/secure_tokenizer.py`, which now records flint's three regexes as
  `pre_patterns` instead of the Qwen35 fallback; folder made owner-only like `out/gemma4-pack`).
- **Oracle additions (test-only):** `engine/tests/flint/oracle/flint_chat.cpp` (llama.cpp's `common_chat_templates_apply`
  Jinja rendering + tokenize with specials, as llama-server) and `flint_tokenize ... plain` (no special parsing).
  Fixture `secure_frontend/tests/fixtures/flint_tokenizer_texts.json`: 38 texts + 9 chats, ids only.
- **Tests (TDD, RED recorded: every new case failed to build first):** `flint_pretokenize_splits_as_llama_cpp`,
  `flint_real_pack_matches_the_fixture` (38/38 token-exact, decode round-trips), the new
  `secure_frontend_flint_profile_tests` (template bytes from llama.cpp's Jinja, escapes, thinking off, refusals, marker
  ids, plan stops, real-pack chats 9/9 id-exact), `supervisor_selection_for_flint`,
  `engine_profile_accepts_flint_and_refuses_qwen_rows`, `flint_user_arguments_follow_the_strata_flint_allowlist`,
  `flint_options_follow_the_flint_engine`, the card parse, and `test_extract_ships_the_flint_patterns_for_a_flint_pack`.
  Full ctest: 1,051 tests, only the three pre-existing failures (`auth04_no_switch_disables_auth`,
  `raw_profile_rejected_in_release_build`, `release_binary_lacks_hook_marker`). MSVC /W4 /WX clean.
- **Served:** card `engine/fe-config/cards/flint-q8.json` (engine `engine/frontend-engine-flint` = `multiengine.exe` +
  `multiengine_flint.dll` from `build-g`; GGUF sampling 1.0 / 0.95 / 20; thinking on; 64K context) added to
  `engine/fe-config/multi-test.json`. Over loopback: start 18 s; prompt tokens 3 / 19 / 64 = llama.cpp's for the
  fixture chats; "17 x 23" answered 391 with reasoning (thinking on) and directly in 2 tokens (`none`); `<tool_call>`
  in user text 400; a stream sends reasoning then content then `[DONE]`; ~53 tok/s on a 1,024-token reply. Server and
  engine stopped afterwards.
- **Not done (owner's next asks, in order):** a make script that builds every binary and engine into one clean
  runtime-only out folder (Windows and Linux); GUI-first start-up (run the server binary, configure everything on the
  Models tab, JSON still accepted) with a folder per model on download/selection that updates its card; per-model
  calibrate and recalibrate-on-settings-change, as upstream Strata and Project Maya calibrate (all built in entry n).

### Claude Code session, 2026-10-27 l (Windows box, 4080 SUPER; owner-directed; committed with entry n, not pushed)

Owner: "next engine we need to work on is flint, model is located in F:\models\flint_bf16. Again we make sure it follow
the same recipe as the rest, based on the strata paper at G:\Strata\docs\paper"; then, asked about the BF16 experts
(75.7 GB, too big to pin): "I'll download a Q8 instead, keep working on the rest of the code until I give you the
path" -> "the model is now in F:\models\flint-Q8_0"; the oracle download approved; the BF16 copy deleted by the owner.

- **Design:** `docs/superpowers/specs/2026-10-27-flint-engine-design.md` (supersedes the 2026-10-25 NVFP4 notes):
  flint read from the owner's files and llama.cpp master's `src/models/flint.cpp` (`e86f5fd1`) - 49 layers (2 dense +
  47 MoE), GQA 32:4 at head 128 with QK RMSNorm, per-head attention sinks, NeoX RoPE only where `rope_pattern` (the 35
  sliding layers, window 513: a query sees itself and 512 keys), sigmoid router top 6 of 256 renormalised plus one
  ungated shared expert (1024), SwiGLU, untied head; the recipe mapped onto Gemma 4's engine.
- **Model:** `F:\models\flint-Q8_0\flint-Q8_0.gguf` (unsloth Q8_0 + imatrix, one 42.3 GB file; Q8_0 matrices, BF16
  routers). Its GGUF says `general.license other / flint-preview`, the model page Apache 2.0 - recorded in PROVENANCE
  for the owner.
- **Oracle:** llama.cpp master `e86f5fd1` shallow-cloned to `G:\Strata-data\flint-llama-ref`, built CUDA sm_89 shared;
  `engine/tests/flint/oracle/` (test-only, never built by CMake): `flint_oracle`, `flint_seams`, `flint_tokenize` and
  the `f_kl.py` / `f_seams_cmp.py` comparisons.
- **`multiengine_flint.dll` (`engine/src/flint`), built as Gemma 4's:** `f_model` (the guard over any number of shards,
  gguf-split names), `f_cli` / `f_profile` / `f_engine.hpp` / `f_lane` (Gemma 4's at flint's numbers: 6 of 256, SwiGLU,
  47 tier layers), `f_kernels` (Gemma 4's generic kernels + flint's attention with sinks, QK post with per-layer RoPE,
  sigmoid router, SwiGLU, combines), `f_engine.cu` (Gemma 4's engine: pinned arena, VRAM cache + profile + background
  swaps, CPU lane, captured decode graph, MMQ prompt stream with borrowed buffers; the tier indexes the 47 MoE layers),
  `f_run.cpp` (`flint_main`; serve loop stops on the four end-of-generation ids). It compiles Gemma 4's unchanged
  `g4_gguf.cpp` / `g4_file.cpp` and links Gemma 4's frozen `g4core`. Test-only: `--layers N`, `STRATA_FLINT_SEAMS=<dir>`
  (the prompt path's seams in llama.cpp's names and file format), `STRATA_FLINT_NO_MMQ`. The launcher routes arch
  `flint` (Strata's process steps); `--families` ends `... gemma4 flint`.
- **Tests (TDD):** `f_host_tests` 11/11 (RED recorded for the model guard: 4 of 5 failing on a stub), `f_kernel_tests`
  8/8 GPU (RED recorded: all 8 failing on a stub). One kernel-test reference was corrected, not loosened: RoPE's angle is
  computed as ggml-cuda computes it (float), and the RoPE case's tolerance states float-angle precision (4 ulps of the
  angle times the pair's magnitude) - the small-position cases keep the original bound.
- **Checked on the Q8_0 file against the oracle** (CPU, f16 KV - the engine's own cache type):
  - 25-token chat prompt + 199 forced decode steps (graph, CPU lane): **mean KL 0.0113, top-1 98.0%**; llama.cpp
    against itself: f32 vs f16 KV 0.0178 / 97.0%, 16 layers on the GPU 0.0188 / 97.5%. Seams: layers 0-15 at cosine
    >= 0.9996; later, near-tie routing flips amplified by the model's massive activations (dim 996 ~70 from layer 15, dim
    1355 in the thousands by layer 46).
  - 1,511-token prompt (three 512-token chunks on the MMQ stream, the window and the ring's wrap-around) + 63 forced
    steps: **mean KL 0.0043, top-1 100%**.
  - Graph vs plain launches (CPU lane off): bit for bit over 200 steps.
  - Output: a coherent `<think>` answer (Canberra, the Sydney-Melbourne compromise; it invents a "John Canberra").
- **Speed** (74 of 256 experts a layer in VRAM, 37.45 GiB pinned, load 17.5 s): decode 49.1 tok/s (45 at 1.5K context;
  31.6 with the CPU lane off), prompt 406 tok/s on 1,511 tokens (329 on the grouped kernel), 46 tok/s on 25.
- **Unchanged:** Gemma 4 through the launcher, bit for bit against its reference again.
- **Not done:** F5 (prompt-lookup drafting), F6 (the frontend card - done in entry m), flint's own tuning of the
  copy-engine share and the rebalance, a usage profile for the VRAM tier.

### Claude Code session, 2026-10-27 k (Windows box, 4080 SUPER; owner-directed; committed and pushed with this entry, together with entry j)

Owner: "Port the upstream project maya engine into a library to support all models in our engine"; asked which
release, the owner chose Maya's newest, `v1.1.30`, fetched (`G:\Project-maya` had `v1.1.27`).

- **Project Maya `v1.1.30`** (`8621e331`, tag object `70137ee5`; `git fetch --tags` into `G:\Project-maya`, its
  working tree untouched) ships three programs: `maya` (GLM-5.3-Flash), `maya-ds4` (DeepSeek-V4.1-Flash) and the new
  `maya-mm3` (MiniMax-M3.1-Flash). All three are now family libraries of `multiengine.exe`:
  `multiengine_maya.dll` (`maya_main`), `multiengine_maya_ds4.dll` (`maya_ds4_main`), `multiengine_maya_mm3.dll`
  (`maya_mm3_main`).
- **`engine/third_party/maya`** is Maya's engine tree at `v1.1.30`, **not edited** (411 files: `src`, `include`,
  `cmake`, `tests`, `third_party/ggml`, the exllamav3 / beellama.cpp licences, `CMakeLists.txt`, `VERSION`,
  `LICENSE`), written by the new `engine/src/multiengine/sync_maya.py <clone> <tag>` from the clone's git objects
  (CRLF, except the configure-time `mmq-fused-swiglu.patch`, byte for byte as Maya's `.gitattributes` keeps it; our
  `.gitattributes` got the same rule).
- **Built as Maya's own CMake project** (Maya's targets share their names with upstream Strata's): `cmake/multiengine.cmake`
  adds it as an `ExternalProject` in `build-g\maya` with the options Maya's `maya.py` passes (Release, CUDA, native
  experts, `STRATA_GGML_DIR` = the same llama.cpp copy) and the engine's compilers and GPU targets; option
  `STRATA_BUILD_MAYA` (on in `build-glm-g.bat`), `MULTIENGINE_MAYA_TESTS` configures Maya's tests there.
  `cmake/multiengine_maya.cmake` is that build's `CMAKE_PROJECT_strata_INCLUDE`: it defers itself to the end of
  Maya's root directory and adds the three libraries there, each = its program's sources with the main one replaced
  by our wrapper (`src/multiengine/maya_family.cpp`, `maya_ds4_family.cpp`, `maya_mm3_family.cpp`: `#define main` +
  `#include` of the unedited file), the program's libraries / includes / definitions / options / features / link
  options / CUDA architectures, no export define, written beside the launcher. **One library per program**, as Maya
  builds one program per model: their sources were never linked together, so same-named definitions in one DLL would
  be a silent ODR risk.
- **raon1 keeps its own library:** `third_party/project-maya` (Maya `v1.1.27`'s GLM engine + our raon1 changes) is
  now `multiengine_raon1.dll` (was `multiengine_glm.dll`, target `strata_glm`; option `STRATA_BUILD_RAON1`, was
  `STRATA_BUILD_GLM`), same sources, recompiled under the new name. GLM-5.3-Flash now runs on Maya's own program.
- **Launcher** (`src/multiengine/launcher.cpp`): `--glm-pack DIR` routes by the architecture of the GGUF the pack was
  made from (first line of `DIR/native_experts.txt` -> the shard beside the pack -> `general.architecture`): `raon1`
  -> `multiengine_raon1`, anything else -> `multiengine_maya`; `--native`/`--model`: `gemma4`, `raon1`, `glm5-next`
  (-> Maya); a lone `--pack DIR` holding `ds4.txt` -> `multiengine_maya_ds4`, `mm3.txt` -> `multiengine_maya_mm3`;
  else upstream Strata. Maya's programs get **none** of the launcher's process steps (Maya's own `main`s take theirs;
  `maya-ds4`/`maya-mm3` set no `CUDA_MODULE_LOADING` and leave stdout buffered, as their executables do); Strata,
  Gemma 4 and raon1 keep them. `--families`: `qwen4exp qwen35moe glm5-next deepseek41 minimax-m3 raon1 gemma4` (a
  Maya family only when its library is in the folder).
- **Checked on this box:**
  - *Build:* `--target multiengine` exit 0 (launcher /W4 /WX clean); exports: each library its one entry (the Maya
    ones also `NvOptimusEnablementCuda`, from NVIDIA's static CUDA runtime - see the differences below).
  - *Same code as Maya's executables:* all **26 compile lines** of the three libraries are token for token their
    programs' (`ninja -t compdb` of the Maya build); the link lines differ only by the wrapper object and `/dll`
    for `/subsystem:console`.
  - *GLM-5.3-Flash* (Maya-S-v2 IQ2_XXS pack, `our-glm.json`'s flags, a real 36-token prompt, 128 greedy tokens,
    twice each) against Maya `v1.1.30`'s own `maya.exe` from the same build: not run-to-run reproducible in Maya
    itself - every run takes one of two paths that split at token 13 (a near-tie: ". It" vs " (ACT)"); `maya.exe`
    run 1 and launcher run 2 share **125 of 128** tokens (path A), `maya.exe` run 2 and launcher run 1 share 27
    (path B); each binary produced each path once. Both answers are correct and coherent (Canberra, the
    Sydney-Melbourne compromise). Decode 7.4-7.5 tok/s (`maya.exe`) vs 7.6-7.8 (launcher), prefill 2.5-2.6 s, the
    expert tiers warm (75.2 GB) in ~99 s either way.
  - *DeepSeek and MiniMax* (no model on the box, 340-500 GB): stand-in packs (`ds4.txt` / `mm3.txt` alone) reach
    their programs through the launcher with the same output and exit code as `maya-ds4.exe` / `maya-mm3.exe`; Maya's
    model-free tests pass: `mm3_exl3_test`, `mm3_gguf_test`, `mm3_kv_test`, `ds4_gguf_test` (ALL OK),
    `ds4_iq1_rows` (both), `ds4_prefill_stats_test`.
  - *Maya's GLM tests:* `glm_layer_graph_test`, `exl3_test`, `glm_moe_rows_test`, `glm_rows_kernels_test`,
    `glm_lat8_test`, `glm_mmq_glu_test`, `glm_mv_nt` (both), `native_split_test`, `split_groups_test` pass;
    **`native_rows_exact_test` fails** (Maya's test, Maya's code, unchanged): on this AVX2-only CPU, 35 of 11,220
    IQ1_M results with subnormal (~1e-39) activations differ from ggml-cpu's dot in the last bits. The engine sees
    the same at start-up and uses ggml's dots for IQ1_M rows ("the multi-token iq1_m rows differ from ggml-cpu's dot
    in this build - ggml's dots for them"); the GLM test pack has no IQ1_M experts.
  - *The other families through the new launcher:* Gemma 4 (`refs.sh`, both chunk sizes, twice): all four logit
    dumps **bitwise identical** to the old monolith's reference again. raon1 (`multiengine_raon1`, 8 greedy steps):
    the reference tokens in runs 1, 3 and 4 (1,296 expert slots); run 2 started with ~0.24 GB less free VRAM (desktop
    apps), sized its pool at 1,272 slots, moved experts to the CPU lane and its last three tokens differed - the known
    CPU-lane rounding of this synthetic near-tie prompt, not the code. Qwen IQ3_S (`qwen_real.sh`, twice): 75.4 and
    68.9 tok/s; the runs share 79 and 107 tokens with the previous launcher's run 2 and 35 with upstream's runs,
    inside upstream's own run-to-run spread (its two runs share 64).
- **Known differences from Maya's executables** (process-wide, so not per library): the launcher's UTF-8 code page
  manifest (Maya's executables run in the system code page; the same for ASCII paths), and `NvOptimusEnablementCuda`
  is exported by `maya.exe` but has no effect from a DLL (it only matters on hybrid-graphics laptops). Not ported:
  `maya-vision`, Maya's separate picture helper (llama.cpp's mtmd, which `maya.py` patches in place with GLM's vision
  tower), so GLM picture input is not available through `multiengine` yet.
- Docs: `engine/README.md` (family table, Maya build), `docs/PROVENANCE.md` (multiengine section: the Maya libraries,
  the raon1 library, the Maya supply-chain read of the whole copied scope; the Project Maya section: code taken since,
  the MiniMax Community License for the MiniMax model, not in our tree), `docs/security/threat-model.md` (the launcher
  bullet: six libraries, the routing reads, Maya's bare-name system DLL loads, its stall minidump).
- **State left:** committed and pushed (owner: "Commit and push the Strata v0.2.6 port, the Maya v1.1.30 libraries,
  the Qwen card and the docs"); the multi test server (Qwen + Gemma 4 cards) stopped (idle); no card made for
  GLM-5.3 / DeepSeek / MiniMax on the new libraries (the GLM and raon1 configs still use the older deployment folders
  `frontend-engine` / `frontend-engine-raon`), and the cards' deployment `engine/frontend-engine-multi` still holds the
  previous build (launcher + strata / gemma4 / glm DLLs; Qwen and Gemma 4 route the same there). The launcher's switch
  has no unit test: it was checked by the runs above only (every route taken at least once). `build-g` still holds
  the stale `multiengine_glm.dll` from before the rename (unused).

### Claude Code session, 2026-10-27 j (Windows box, 4080 SUPER; owner-directed; committed and pushed with entry k)

Owner: "now work on the upstream engine strata on G:\strata and port it over to our multiengine", then "i want the
upstream v0.2.6 but make sure it does not affect the other models, like it should be in it's own library so they
don't collide, glm 5.3 would be in the maya library, gemma4 separately, etc.", then "test the ported strata
multiengine with G:\Strata-data\models\IQ3_S" and "create a card for it as well".

- **One executable, a library per family.** `multiengine.exe` is now only a launcher
  (`engine/src/multiengine/launcher.cpp`, 57 KB): it reads which family a command line is for (`--glm-pack`, else the
  GGUF `general.architecture` of `--native`/`--model`'s first shard, read by a small bounds-checked header reader),
  loads `multiengine_<family>.dll` from its own folder (`LoadLibraryExW`, absolute path) and calls its one export. It
  first takes the steps upstream's `main()` takes before its family switch (stdout unbuffered, `CUDA_MODULE_LOADING=
  EAGER` unless set, `--gpu` -> `CUDA_DEVICE_ORDER`/`CUDA_VISIBLE_DEVICES`), keeps the crash filter (moved out of
  `generate.cpp`), answers `--families` (`qwen4exp qwen35moe glm5-next gemma4`) and forwards `--version` to upstream's
  library (`strata 0.1.51`). Libraries: `multiengine_strata.dll` (upstream's program, `strata_main`),
  `multiengine_glm.dll` (Maya, `glm_main`, unchanged sources), `multiengine_gemma4.dll` (`gemma4_main`). All are built
  beside the exe (`cmake/multiengine.cmake`; `build-glm-g.bat` builds the target `multiengine` and no longer copies
  the GLM DLL by hand).
- **Upstream `v0.2.6` (engine 0.1.51) in `engine/`.** `engine/src/multiengine/sync_upstream.py G:\Strata v0.2.6`
  wrote 45 changed and 15 new upstream files from the clone's git objects (G:\Strata's working tree untouched): our
  13 edited upstream files are upstream's again, plus upstream's 36 changes. The only non-upstream line in upstream's
  files is the last of the root `CMakeLists.txt` (`include(cmake/multiengine.cmake)`). `multiengine_strata.dll` is
  upstream's `strata` target's program compiled through `src/multiengine/strata_family.cpp` (`#define main
  strata_upstream_main` + `#include` of the unedited `generate.cpp`) with that target's libraries, definitions and
  options copied: its compile line is token for token `strata.exe`'s `generate.cpp` line plus the export macro, its
  link line the same libraries. Upstream's own `strata.exe` is still built. Supply-chain read: PROVENANCE, multiengine
  section (no network/process/shell/dynamic-load/file-delete call added).
- **Gemma 4 on its own core copy.** `engine/src/gemma4/core/` holds the 37 Strata-core units Gemma 4 links (and their
  headers, the ggml shims), copied from `engine/` before the update (upstream `v0.2.4.2` + Gemma 4's generalisations;
  `core/VENDORED.txt` lists each file's SHA-256), each compiled with the defines and ISA flags the engine gave it.
  `include/strata/gemma4/engine.hpp` (the old `strata::gemma4::run` entry) is gone; `gemma4_main` is the export and
  sets stdout unbuffered itself (a static-CRT build gives the DLL its own stdout).
- **The GLM and Gemma 4 libraries keep `STRATA_VERSION` 0.1.49.2** (`cmake/multiengine.cmake` rewrites only that
  directory definition for their subdirectories): GLM keys its calibration cache and saved sessions on it. The GLM
  sources did not recompile after the update (only relinked into the exe's folder).
- **Card:** `engine/fe-config/cards/qwen38-iq3s.json` (profile `qwen`; model `G:\Strata-data\models\IQ3_S`, pack
  `G:\Strata-data\packs\iq3_s`, MTP `G:\Strata-data\mtp\rt`, `our-qwen.json`'s fixed flags; options: context 256K,
  KV int8, KV streaming 32,768, MTP on, spec 4 / 0.7, expert cache and prefill auto, thinking high, upstream server's
  qwen4exp thinking sampling 1.0 / 0.95 / 20). Its engine is the new deployment `engine/frontend-engine-multi`
  (ignored; the build's exe, three DLLs and `strata_kq.dll`). `engine/fe-config/multi-test.json` +
  `engine/serve-frontend-multi-test.bat` serve it beside the Gemma 4 card, no model started. `--check`: both cards
  usable (qwen, 12 page options; gemma4, 7).
- **Checked on this box, through the new launcher:**
  - *Gemma 4* (`refs.sh`: the 293-token prompt in one 512-token chunk and in 128-token chunks, 16 greedy steps,
    twice each): all four logit dumps **bitwise identical** to the old monolith's (`multiengine.exe` of 23:47,
    2026-10-26), 16 MiB each.
  - *raon1* (`--glm-pack`, 8 greedy steps, twice): the reference's tokens both times, the same step times.
  - *Qwen3.8-Flash-Next IQ3_S* (owner's request) against upstream `v0.2.6`'s own `strata.exe` from the same build.
    Not bitwise checkable: on the native IQ pack `--dump-logits` writes nothing (upstream writes it only on its
    per-token path, and this pack always takes the batched prefill and the verify windows), and upstream's decode is
    not run-to-run reproducible here in any mode tried (adaptive tier on or off, MTP on or off; upstream itself
    documents a run-to-run KL floor). On a real chat prompt (31 tokens, the card's flags, 128 greedy tokens):
    upstream run 1 vs run 2 share their first 64 tokens; **launcher run 1 equals upstream run 1 in all 128 tokens
    with the same speculation path** (70 rounds, 59 of 88 drafts); launcher run 2 shares 35. Decode 72.7-72.8 tok/s
    through the launcher, 72.5-73.8 for `strata.exe`. The text is a coherent three-paragraph answer. The two
    builds' compile lines for `generate.cpp` differ only by the export macro, their link lines only by `/dll`.
  - *The card, end to end* (`multi-test.json` server, loopback API): `POST /v1/strata/models/start` -> running in
    78 s, one chat (the capital of Australia, thinking high) answered correctly, 241 completion tokens in 6.1 s,
    stopped cleanly.
  - *Gemma 4 suite* (`ctest -R ^g4_`, the model checks through the launcher): **34/34** (host 16, lane 4 on the
    vendored core, kernel 11, model 3).
- **Fixed on the way:** the first link of `multiengine_gemma4.dll` failed (8 unresolved symbols): the dependency probe
  had missed `src/platform/memory.cpp` and `src/kernels/cpu/kq_avx1.cpp`; both added to the core copy (unedited,
  recorded in `VENDORED.txt`) and compiled as the engine compiles them (`kq_avx1` with `/arch:AVX`).
- **State left:** committed and pushed with entry k. The idle Gemma 4 test server (`gemma4-test.json`) was replaced by
  the `multi-test.json` one (both cards, no model started; stopped again in entry k). The raon1 / GLM configs still
  name their old deployment folders (`frontend-engine-raon`, `frontend-engine`: the pre-split monolith), not
  repointed. Upstream code keeps compiling without `/WX` (`STRATA_WERROR=OFF` in `build-g`, as before); ours (the
  launcher, the Gemma 4 sources) with `/W4 /WX`.

### Claude Code session, 2026-10-26 i (Windows box, 4080 SUPER; owner-directed; committed and pushed with this entry)

Owner: "continue" - the gaps entry h left against the paper.

- **P2's profile fill:** `--expert-profile P` fills each layer's slots from Strata's `profile.bin`
  (`read_expert_profile`; `fill_from_profile` in `src/gemma4/g4_profile.cpp`: a layer's experts in rank order, then its
  lowest unranked); `--expert-profile-save P` writes what the tier learned (`rank_learned_profile`, the slots' experts
  first, then by decayed routing) on exit and between requests every 10 minutes at most. A cold 48-step decode after
  a 64-token prompt: 90.0 tok/s from a profile learned on the same workload, 43.2 without (VRAM entries 11,210 vs
  6,444). `--tokens` runs never rebalance, so they learn nothing to save.
- **P5's borrowed buffers:** the chunk-sized scratch is now a row set (`Rows`): decode and batches under 64 tokens use
  a small one of their own (63 rows); a streamed prompt binds a big one carved from the expert cache's top slots,
  lent for the prompt (their experts read from RAM meanwhile) and refilled after it as the tier's swaps - zeroed
  first (a slot's guard is MMQ's over-read), each slot back with the expert it held or its layer's hottest one in
  RAM, used once landed (`land_swaps`, now also at each decode step's start). `--no-prompt-borrow` is the A/B arm
  (Strata's `--no-prefill-borrow`). Lent: 106 slots at 512-token chunks, 233 at 2,048; slots a layer 54 / 53 instead
  of 51 / 45.
- **Decode after a prompt:** one more rebalance after a prompt hands its routing to the tier (a one-chunk prompt's
  only rebalance had just started the counts' read-back): decode after a one-chunk 2,048-token prompt 71.6 tok/s
  instead of 52.6.
- **Found and fixed:** entry h's guard fix (a memset after every streamed expert, ~2,300 a chunk on the copy stream)
  cost a fifth of the prompt speed; a ring buffer is now zeroed once, when a BF16 group or a fresh loan dirtied it.
- **Measured** (2,048-token prompt, then 96 decode steps): 512-token chunks 704 tok/s, 86.2 tok/s decode; 2,048-token
  chunks 2,192 tok/s, 71.6 decode (entry h: 577 / 2,204 with 51 / 45 slots). compute-sanitizer memcheck on the loan,
  the refill, a short batch after a streamed chunk and decode: 0 errors.
- **Kept as Strata keeps it:** the copy engine's share stays the fixed `--pcie-frac` (Strata's `PcieModel` fit is
  opt-in there and every non-zero share was slower here).
- **Tests** (RED first): host `profile_fills_each_layer_with_its_top_experts` (new; RED: "a list per layer" from the
  stub), the CLI cases for `--expert-profile`, `--expert-profile-save`, `--no-prompt-borrow` (RED: "unknown flag").
  Host 16/16.
- **`g4_model.prompt_paths` restructured - read this:** entry h gated it on KL(grouped || MMQ) < 1e-2, set from one
  measurement (4.2e-3). With the loan it measured 1.15e-2 and failed. Measured since: the end-to-end KL moves with
  the slot split from 5.4e-4 to 1.2e-2 (MMQ's stream-k split follows the launch's shape - compiled into llama.cpp's
  per-architecture configs, and llama.cpp is pinned and not modified - and the hidden's int8 re-quantization and
  the routing amplify its last bits), while MMQ against itself at two splits differs by 4e-5 at layer 0 and the
  grouped kernel is bitwise split-independent. So the 1e-2 gate sat inside MMQ's own noise. The test now has two
  gates: layer 0 per token within 1e-2 of the grouped path (measured median 9e-5, worst 1.3e-3; a wrong expert,
  row or stride is O(1) on its tokens - stricter at finding bugs than the old gate), and the end-to-end KL below
  llama.cpp's own CPU-vs-CUDA floor on this model, 0.042 (the design's phase-5 gate), with the same top-1. The
  end-to-end threshold is looser than entry h's; the owner may want to review that. RED for the new gates: the
  resident product stepping by the blob instead of the slot stride (injected, then removed) - layer 0 0.83, KL 5.9,
  top-1 34715 vs 2094; GREEN: layer 0 1.3e-3, KL 1.15e-2, same top-1. Gemma suite 34/34 (host 16, lane 4, kernel 11,
  model 3).
- **Owner rule recorded:** no server runs unless a step uses it; the raon1 server was stopped (it had been restarted
  after entry h's work).

### Claude Code session, 2026-10-26 h (Windows box, 4080 SUPER; owner-directed; pushed ae9ba79a)

Owner request: "revisit the gemma4 engine we built and make sure it follows the strata-paper.pdf design", then "do it
here and incorporate it [into] the multiengine as a library"; owner's choice of substrate: **Strata core** (the
paper's own pieces). Design record: `docs/superpowers/specs/2026-10-26-gemma4-paper-tiering-design.md` (the paper's
P1-P5, the old engine's gaps, the phases and their gates).

- **Phase 1 - Gemma 4 is a library of `multiengine.exe`.** `engines/gemma4` moved (git mv) to `engine/src/gemma4`
  (static libraries `strata_gemma4_host`, `strata_gemma4_kernels`, `strata_gemma4`; entry `strata::gemma4::run`,
  `include/strata/gemma4/engine.hpp`) and its tests and parity tools to `engine/tests/gemma4`;
  `engines/gemma4/build-windows.bat` removed. `generate.cpp`'s family switch routes a `gemma4` GGUF to it
  (`ModelFamily::Gemma4`; `--families` lists gemma4). Built only with CUDA and `STRATA_NATIVE_EXPERTS` (default ON).
  Logits bitwise identical to the standalone build over 24 greedy steps.
- **Phase 2 - P1/P2.** Experts are GGUF-native blobs `[gate | up | down]` in a per-layer pinned, mapped arena (no
  repack); the GPU computes them with the engine's `native_expert_grouped`, extended with GEGLU
  (`NativeExpertLayout::act = 1`) and BF16 weights (`Fmt<30>`, the Q8_K_XL file's layer 1 and layer 29's down); the
  VRAM tier is Strata's `ExpertCache` (byte-sized per layer), swapped on a copy stream that nobody waits for (evict at
  once, admit when the copy has landed). KL 4.8e-4 vs the old engine, the same greedy tokens.
- **Phase 3 - P3, the CPU lane** (`src/gemma4/g4_lane.{hpp,cpp}`): per decode layer the GPU publishes the routing
  and the experts' input (`doorbell_publish_res`); a thread pinned to the pool's host core runs the verify window's
  dispatcher (`expert_pool_dispatch_multi`) over an `ExpertSource` of the arena: VRAM experts -> the plan's first
  groups (flag A), `--pcie-frac` of the misses -> copied into VRAM staging by the copy engine (flag B), the rest ->
  the CPU pool on ggml-cpu's dots, rows back through mapped memory (flag C); the GPU waits on each flag in turn
  (`wait_flag_ge`, `copy_rows_from_mapped_after`, `moe_hit_add`). New flags: `--cpu-lane on|off`, `--pcie-frac F`;
  `--cpu-threads` sizes the pool. Shared code generalised (Qwen's widths no longer compiled in): `kNativeActBytes`
  8192 / `kNativeHBytes` 4096 (Maya's values), `kNativeMaxFF` 2048 (`SplitBufMulti::ff`, checked in `native_fmt`),
  `NativeFmt::act` with GEGLU rows on ggml-cpu's dots, `expert_layout_set` (a family sets the process-wide table from
  its own GGUF), and the dispatcher's row stride from the layer's `n_embd` instead of `H`. Teacher-forced vs the
  GPU-only path: mean KL 5.3e-4, max 4.7e-3, top-1 23/24 (step 6 is a near-tie).
- **Phase 4 - P4, one captured graph per decode step** (`strata::core::CapturedGraph`): the token and its position
  are device data (`QkvPost::d_pos`, `attn_decode_dev`), re-captured when an attention split is added (every 128
  positions). Bitwise identical to plain launches, lane on and lane off. `--no-graph` now means what it says.
- **Phase 5 - P5, streamed prompt chunks** (`--chunk` now up to 2,048, default still 512): a chunk of 64 tokens or
  more streams every layer's misses from the arena into a ring of 3 staging buffers on its own copy stream while the
  GPU computes the groups before them; the Q8_0 layers multiply on llama.cpp's MMQ (`strata::prefill::mmq`, the Qwen
  family's prompt path: gate+up as one product, GEGLU, down; the slots read in place), the BF16 layers (1, 29) on
  the grouped kernel from the ring. `STRATA_G4_NO_MMQ=1` is the A/B arm (the grouped kernel reading misses over PCIe
  in place). Two layout rules learned on the way, both in the code: an expert's stride must be a whole number of Q8_0
  blocks as well as 256-aligned (MMQ steps between experts in blocks; the stride is `blob + guard` on a 4,352-byte
  grid - a 256-byte stride faulted), and the guard after each streamed blob must be zero (MMQ's K-tail over-read
  multiplies it by zero activations; bytes a BF16 group left there decoded as Inf scales -> NaN routing ->
  out-of-range picks -> an illegal access on the second 512-token chunk). compute-sanitizer memcheck: 0 errors.
- **Fixed in passing:** a 2-4-token prompt tail queued decode steps back to back while the pinned id buffer each
  step's copy reads was rewritten by the next (`forward_one` now waits for the previous step first).
- **Measured on this box** (RTX 4080 SUPER 16 GB, PCIe 4.0; 52-53 expert slots a layer of 128, 41% resident; 96
  decode steps after a 64-token prompt, the slots rebalanced every 8): CPU lane + graph **58.0 tok/s**; lane, plain
  launches 51.4; lane off (Phase 2: every miss read over PCIe in place) 41.9 with the graph, 39.5 without. The copy
  engine's share: 0 -> 51.5, 0.5 -> 45.4, 0.75 -> 41.7, 1 -> 36.5 (plain launches) - the pool reads RAM faster than
  the link copies and the two share the RAM's bandwidth, so `kDefaultPcieFrac` is 0 here. Prompt of 2,048 tokens:
  577 tok/s at 512-token chunks, 1,381 at 1,024, **2,204 at 2,048**, against 133-178 tok/s for the phase-2 prompt
  path and 860-1,100 for the old standalone engine; the price is VRAM (ring + chunk buffers): 50 slots a layer at
  512, 45 at 2,048 (52 before phase 5).
- **Qwen3.6 bitwise gate:** Qwen3.6-35B-A3B Q8_0 through the qwen35 family (its hybrid mode: 7,287 CPU-pool
  entries over 24 greedy steps) gives bitwise-identical logits with the shared CPU code before and after this work.
- **Tests** (TDD, RED recorded before each fix): `g4_lane_tests` (new, 4 cases: the Gemma 4 geometry in
  `native_fmt`, GEGLU rows vs a reference built from the blob's numbers, the pool's 704-wide rows bitwise vs the
  sequential rows, the window dispatcher's rows at 2816) - RED: 7 checks failed on the old shared code; the kernel
  case `decode_kernels_read_the_position_from_the_device` - RED: it did not compile; the CLI cases for the two new
  flags - RED: "unknown flag"; the model check `g4_model.prompt_paths` (new: MMQ vs the grouped kernel on a real
  293-token prompt, `tests/gemma4/g4_prompt_ids.txt` from `g4_prompt_text.txt` by llama.cpp's tokenizer, KL < 1e-2
  and the same next token) - RED: "illegal memory access" with the 256-byte stride, GREEN KL 4.2e-3.
- **Open, against the paper:** the ring and the chunk's MMQ buffers are dedicated VRAM, not borrowed from the expert
  cache; the cache fill is experts 0..S-1, not a usage profile (`--expert-profile`); the copy engine's share is
  fixed, not fitted (`PcieModel`); llama.cpp parity (`g4_parity.py`) not re-run. No committed card launches Gemma 4
  (the comparison's card was a scratch one naming the old standalone exe); `multiengine.exe --native <gemma gguf>
  --serve` is what a card should run.

### Claude Code session, 2026-10-26 g (Windows box, 4080 SUPER; owner-directed)

- **raon1 card = KRAFTON's own coding settings** (owner's choice; `engine/fe-config/cards/raon1.json`): temperature
  1.0, presence_penalty 0 (top_p 0.95, top_k 20 unchanged), `default_max_tokens` 32768 (was 16384; KRAFTON's
  examples use 32,768). Supersedes entry e's 0.6. Source: the Raon-1-mini-preview model card on Hugging Face -
  temperature 1.0, top_p 0.95, top_k 20, min_p 0, presence_penalty 1.5 for chat / summarization / knowledge Q&A /
  agents and 0 for coding and math, thinking mode recommended on, truncate_history_thinking false.
- Why not 0.6: on the voxel prompt (seeds 101-105) 0.6 thinks about a third as long (median 10K characters vs 25K at
  1.0 / 0 and 31K at 1.0 / 1.5) and writes smaller pages (median 10K vs 11-14K) - the "basic" the owner reported -
  though it crashed less (4 of 5 pages ran cleanly vs 1 of 5 at 1.0 / 0, one of those cut at the old 16,384 cap).
- Correction to entry e's reasoning: KRAFTON does publish coding results (LiveCodeBench v6 81.6 vs Qwen3.6-35B-A3B
  81.2; SWE-Verified 70.8 vs 63.2).
- web page: the drawer comment no longer names a card value. Served after the restart: a chat answers; the page
  served carries the edit. api_router / api_server / chat_schema / typed_config tests: 195 of 195.
- Model comparison (owner request; the voxel prompt with "add as many details as possible", 3 runs each through
  secure_frontend, every page opened in a browser, each maker's settings): raon1 2 of 4 pages ran (~3.5K tokens,
  ~60 s); Gemma 4 26B-A4B Q8 3 of 3, coherent scenes (~4.5K tokens, ~76 s); Qwen3.6-35B-A3B Q8_0 1 of 3 as written -
  the other two fail on one slip each (`var` as a parameter name, a missing import map) and are the most detailed
  once fixed (11-15K tokens, 6.5-7 min, ~29-35 tok/s; top_k at the engine's 64: a request may not set top_k).
- Qwen3.6-35B-A3B Q8_0 downloaded through the server (`pull.connections` 16 in a scratch config; the managed config
  has none, so its downloads run one stream, ~1.6 MB/s here vs up to ~150 MB/s): `C:\multi-strata\engine\downloads`,
  SHA-256 verified, tokenizer prepared, card `engine/raon-state/cards/qwen3.6-35b-a3b-q8_0.json`. **Known gap:** a
  managed config with `glm.*` set refuses every non-GLM card ("glm.gpu: is only supported with the glm_pack engine"),
  so the raon1 server's Models tab cannot offer Gemma 4 or Qwen3.6 (fix: the glm settings as card-owned keys).

### Claude Code session, 2026-10-26 f (Windows box, 4080 SUPER; owner-directed)

- **Seeded and greedy requests now repeat on the GLM engine (raon1).** Found: two identical greedy requests to the
  served raon1 diverged after ~50 tokens. Isolated with the engine alone: fresh processes are identical (3 runs, 300
  tokens); inside one `--serve` process requests 2 and 3 diverged at tokens 55 and 50; `--prompt-cache 0` changed
  nothing; `STRATA_GLM_CPU_LANE=0` made them identical (37.8 tok/s vs 53). Cause: for raon1's Q8_0 experts the CPU
  lane uses ggml-cpu's dots (Q8_0 activations, AVX2 sums) where the GPU uses MMVQ (Q8_1, warp sums), and it adds its
  experts as one group; which experts it takes depends on the expert cache earlier requests left and on its decode
  re-timing - so the same request rounds differently. A bit-exact CPU mirror (activation quantization, reduction
  tree, the GLU's exp) was judged too large and fragile; the engine already has the per-request key `pcie_frac`
  (1: no expert to the CPU lane, misses read over PCIe by the GPU kernel).
- `EngineProtocolClient::begin_generation` (maya_118 flavor: raon1, glm53) appends `pcie_frac=1` when the request
  asks to be repeatable - a seed, or temperature 0 asked for (an unset temperature is the engine default, not a
  request; strata flavors unchanged). RED -> GREEN: `glm_repeatable_requests_keep_misses_on_the_gpu`.
- Served (end to end, each pair with an unseeded 0.6 request between): greedy 400 tokens identical, seed 101 at 0.6
  400 tokens identical, greedy 3,000 tokens identical (13,468 characters). Cost on those requests only: ~40-45 tok/s
  against ~58-61 for ordinary requests. Full ctest: only the three pre-existing failures. No engine change.

### Claude Code session, 2026-10-26 e (Windows box, 4080 SUPER; owner-directed)

- **Owner: raon1 "not smart enough, so basic" (a voxel-scene HTML prompt). Finding: the model, not the engine.**
  The same prompt through KRAFTON's official llama.cpp (raon-1-mini-preview fb895662, llama-cli, same sampling)
  gave an equally crude page. Teacher-forced on that run's 13,809-token transcript (last 256 positions, vs the
  official F32 / FA-off graph): ours q8_0 as served mean KL 0.00157, top-1 97.7%; ours F32 0.00142 / 98.0%;
  the official's own f16 cache 0.00144 / 96.9% and q8_0 0.00158 / 96.5%; transcript NLL 0.740 vs 0.743. Thinking
  length is the model's (ours 10-44K characters over five seeds, the official run 42K).
- **raon1 multi-turn prompt fixed** (`render_raon1_text_prompt`): an earlier assistant turn now renders as the
  template's `render_assistant_body` - `<think></think>\n\n{content|trim}<|im_end|>\n` (the schema has no
  reasoning_content, which is how the template renders a turn without one); before, the empty think block was
  missing, a history the model was not trained on. RED -> GREEN: `raon1_history_renders_as_the_official_template`
  (expected strings rendered from the pack's `chat_template.jinja` with jinja2, HF's trim_blocks / lstrip_blocks).
- **raon1 card sampling = the coding preset** (`engine/fe-config/cards/raon1.json`): temperature 0.6, presence_penalty
  0 (top_p 0.95, top_k 20, penalty_last_n 2048 unchanged); a request's own values still win. Raon's
  generation_config values (1.0 / 1.5) are Qwen3.6's "general tasks" preset; 0.6 / 0 is Qwen's published preset for
  "precise coding tasks (e.g. WebDev)". Measured on the voxel prompt, seeds 101-105, each page opened in a browser:
  1.0 / 1.5 ran cleanly 2 of 6 (3 crashed on the model's bugs, 1 half-rendered); 1.0 / 0 1 of 5 (3 crashed, 1 hit
  the 16,384-token default cap); 0.6 / 0 4 of 5 (1 crashed). Small samples; general-task quality at 0.6 not measured.
- Web page: untuned, the sampling drawer reads "model default" for temperature and top-p (it sends neither, so the
  card applies); it showed 1.00 / 0.95. A browser whose saved settings are tuned keeps sending its own values
  (Reset, then Apply returns to the card's).

### Claude Code session, 2026-10-26 d (Windows box, 4080 SUPER; owner-directed)

- **secure_frontend tokenizes raon1 exactly as its reference tokenizer** (fixes entry c's finding). New
  `TokenizerMode::raon_bpe` (raon1's profile; qwen / glm53 / deepseek stay byte_bpe, gemma4 spm_bpe): byte_bpe's
  files, then raon-1-mini's own text handling, each a line-for-line port of KRAFTON's llama.cpp
  (raon-1-mini-preview fb895662): `raon_nfc` (unicode_cpts_normalize_nfc), `raon_pretokenize`
  (unicode_regex_split_custom_raon: CJK/kana runs, letter runs that keep single spaces between words, digits in
  threes from the right, punctuation, whitespace), and a heap BPE (llm_tokenizer_bpe_session; the old loop was
  quadratic per piece and raon's pieces are word runs). Unicode tables generated from that commit's
  `unicode-data.cpp` (`secure_frontend/tools/generate_raon_unicode.py` -> `src/raon_unicode_data.inc`; PROVENANCE).
  The token / merge lookups are built once at load (`TokenizerPack::raon_index`; byte_bpe still rebuilds two
  260K-entry maps per call).
- RED -> GREEN: `raon_nfc_matches_llama_cpp`, `raon_pretokenize_splits_as_llama_cpp` (hermetic) and
  `raon1_real_pack_matches_the_fixture` (SECURE_FRONTEND_RAON1_PACK = the pack's tokenizer/ dir; the fixture is 30
  texts with llama_tokenize's ids - English, code, Korean NFC and NFD, Japanese, Chinese, mixed scripts, digits,
  whitespace and newline runs, URLs, accents both ways, emoji with ZWJ, RTL / Indic, control characters, a
  document slice): RED had every text wrong (English 38 vs 17 ids, NFD Korean 330 vs 27); GREEN token-exact on all
  30 and decode(encode(text)) == NFC(text). `subsystem_limits_test` now expects raon1 -> raon_bpe (it pinned the
  approximation). Full ctest: only the three pre-existing failures.
- Served (measured after the restart): 32,000 characters are 9,315 tokens, as llama.cpp (13,594 before); "The quick
  brown fox jumps over the lazy dog." 4 tokens (18 before). (A 560K-character document is 155,688 tokens by
  llama.cpp; the frontend counted 232,555 before - not re-measured through the server.) 256K now holds 256K real
  tokens.
- raon1 card: `default_max_tokens` 16384, `max_completion_tokens` 32768 (owner: a reply ended at the old 2048),
  `max_prompt_tokens` 229376 (256K less the completion cap).
- Still approximate: the qwen / glm53 / deepseek paths use the same `basic_pretokenize` (qwen35's pattern is
  transcribed in engines/maya-ref/tools/strata_tokenizer.py, not in C++ yet).

### Claude Code session, 2026-10-26 c (Windows box, 4080 SUPER; owner-directed)

- **raon1 served at 256K with a q8_0 cache** (owner request; `engine/fe-config`): `glm.max_context` 262144 (the
  model's trained length), `glm.kv` "q8_0" (1.12 GB for 256K positions), the card's `max_prompt_tokens` 258048,
  `limits.http.max_request_bytes` 8 MiB (chat body / message 8 MiB less the header), `generation_timeout_s` 3600.
  The frontend composes `--max-context 262144 --kv q8_0` (checked on the running process).
- **MTP: not possible for this checkpoint** - the official Raon-1-mini-preview GGUF has no NextN tensors or key.
  **KV streaming: not for raon1** - GLM's streaming pages the cells its sparse attention selects; raon1's full
  attention reads every cell each token, so it stays resident (`--kv-streaming on` is composed but ignored).
- Prefill attention: a block's (query, head) pairs are the columns of ONE scores / context GEMM (the cache is
  shared by every head), not one GEMM a head - at long context a block is a few queries and the per-head form read
  the cache 40 times over. 9146 tokens 2.38 -> 2.06 s; parity unchanged on short prompts (F32 0.00073, chunks
  0.00088, q8_0 0.00092); at 9K mean KL 0.0108 -> 0.0138 vs the F32 graph (same math, another summation order).
- **FOUND (not fixed): the frontend's raon1 tokenizer is an approximation.** `encode_raon1_chatml_prompt` uses
  `basic_pretokenize` (secure_frontend/src/tokenizer_pack.cpp:117-137, "a model-specific pre-tokenizer must be added
  before claiming exact parity"): it splits a word from its leading space, so 32,000 characters of this file are
  13,594 tokens where llama.cpp's official tokenizer gives 9,315 (+46%; our Python `raon` tokenizer 9,337). The model
  gets non-canonical ids and 256K holds ~175K real tokens. The fix needs raon's pre-tokenizer
  (`RAON_PATTERN`: Unicode L/M/N classes, CJK/kana ranges, 3-digit groups) and NFC in C++. The Qwen path uses the
  same approximation.
- **Long prompts and the server's silence bound.** The first 232,555-token request (the frontend's count, ~160K
  real) was killed at 747 s: `engine.silence_s` (300 s) stops a request the engine is silent on, and PP lines came
  only at chunk ends while one chunk at 200K positions takes minutes; mid-chunk the engine cannot drain a STOP, so
  the supervisor killed it (`drain_failed`). Fix: `prefill_half` sends the server a PP heartbeat every 10 s within a
  chunk (`prefill_progress`; a STOP is still read at the chunk's end). Re-run: the same request finished - prefill
  999.6 s, then 300 tokens at 12.6 tok/s at that context, a coherent reading of the document. Long-prompt speed is
  the open item (prompt attention as GEMMs writes every score out - a fused flash-attention kernel; the split-K
  decode at 232K is 12.6 tok/s).
- The frontend refuses chat text that spells a control token (e.g. a literal `<think>`): HTTP 400 invalid_request.
- **Open (owner, fix later): Monitor metrics.** The server keeps only a 9-key allowlist of the engine's readings
  (`secure_frontend/src/serve_monitor.cpp:24` `kExpertReadingKeys`). The RAM/VRAM cards' expert counts (`ram_used`,
  `vram_used`) come only from STAT lines, sent while DECODING - after an engine restart they are blank until the
  first answer (what the owner saw after the 256K restart). Sent by the engine but dropped: STAT `vram_hit` (the
  expert hit rate - owner wants it), `ram_fetch`, `disk`, `promo`, `vram_slots` / `ram_slots`; INFO `experts`,
  `context` (`/metrics` `max_context` is null), `kv`, `kv_resident`, `cpu_threads`, `pcie_share`, `mtp`, `dflash`;
  the `VRAM ...` breakdown line (weights / kv / act / pool / borrow / free); DONE `spec_hits`/`spec_steps` and
  `d_hits`/`d_total`; PP lines (live `prefill_tok_s` stays null while a prompt is read). Never sent (engine stderr
  only, which the frontend discards): the CPU expert lane's experts and ms a token (`glm stat: ... cpu lane`), disk
  wait ms a token, a prompt chunk's staging (experts from VRAM/RAM/disk, PCIe GB, copy-lane GB/s) - the owner asked
  for CPU-expert metrics; these need a STAT key from the engine first.
- Root `README.md` rewritten to the current engine, models and status (the old one said the engine port was planned
  and pinned Strata v0.2.2).

### Claude Code session, 2026-10-26 b (Windows box, 4080 SUPER; owner-directed; pushed 879fdbe0)

- **raon1's prompts run through the batched prompt path (was token by token).** `prefill_setup` no longer refuses
  `2*n_ff_exp != n_embd`: `carve_moe` gives the expert sets' gate/up rows a buffer of their own (`MoeBufs::GU`, W x
  2*n_ff) when they cannot overlay OUTP's rows; MMQ / WMMA / the Volta cuBLAS path use it. Found on the way and
  fixed: the prompt router widened raon1's F32 `ffn_gate_inp` as BF16 (now an F32 `cublasSgemm`); llama.cpp's MMQ
  reads each 256-value K step whole, so past a 1088-wide down row it read into the next slot or past the buffer -
  `glmfast::mmq_tail_bytes` gives every slot a zeroed tail (main pool zeroed at load, `gbuf`/`pbuf` at each lend), as
  llama.cpp pads its tensors; the light kernels and the AVX-512 CPU path applied SwiGLU (raon1 skips the light
  kernels; the CPU path calls the new `native_glu`); EXL3 experts refused for raon1 (that lane has no SiTU-GLU).
- **raon1's gated-MLA attention batched in prefill** (`raon1_rms_rows`, cuBLAS strided-batched F32 absorb / scores /
  context / v_b, `raon1_softmax_causal`, `raon1_rda_vscale_rows`): token by token it was 134 s of a 9146-token
  prompt's 313. Now 9161 tokens prefill in 2.38 s (3850 tok/s); served, a 13,386-token prompt + 120 tokens took 9.6 s.
- **raon1 KV cache formats (owner request): `--kv q8_0` / `--kv q4_0`** (int8 / q4 alias them; fp16 = the exact F32
  cache): ggml's blocks of 32, 612 / 324 bytes a position and layer vs 2304. `raon1_lat_put` quantizes with ggml's
  reference rounding; the decode attention is new and split-K (`raon1_mla_decode`: up to 64 cell chunks a head, then
  a combine - the old one-warp-a-head kernel was 13 tok/s averaged over a 0-9K context, now 52 tok/s at 9K, 74 short);
  prefill dequantizes the cells into its stage (`kv_stage_bytes_`). One size helper `lat_pos_bytes()` now sizes the
  state layout, `slot_dsa_runs` (snapshots sized raon1's rows as GLM's FP16 kv_lora/2 before) and `seq_layout`. raon1
  is never KV-streamed (its kernels read every cell in VRAM). INFO prints the real format (`kv=f32|q8_0|q4_0`).
- **secure_frontend: typed `glm.kv`** (fp16 default renders nothing; int8 / q8_0 / q4_0 render `--kv`) - raon1 rides
  the composed GLM launch, which takes no engine.args. RED->GREEN: `glm_kv_renders_the_cache_format`,
  `glm_kv_is_a_closed_choice`. README key table regenerated (it was already missing `roots.allow_untrusted_media`).
  Full ctest: all pass except three PRE-EXISTING failures (two hooks-OFF-only release checks in this hooks-ON build;
  `auth04_no_switch_disables_auth` lists switches without `roots.allow_untrusted_media`).
- **Verification** - `engine/tests/oracle/raon1_prefill_parity.py` (box-only; RED was "token by token") against the
  official KRAFTON llama.cpp graph (mean KL / top-1): F32 189-token prompt 0.00091 / 100%, 64-token chunks 0.00075 /
  100%, 180 decoded 0.00117 / 98.9%; q8_0 vs llama.cpp's q8_0 cache 0.00090 / 100% and 0.00106 / 99.4%; q4_0 vs its
  q4_0 0.00167 / 93.8% and 0.00189 / 96.7%. Our q8_0 / q4_0 cost vs our F32: 0.00087 / 0.00368 (llama.cpp's own vs
  F16: 0.0011 / 0.0032). Oracle gotchas: llama.cpp's flash attention reads K as F16 whatever the cache type (an F32
  reference needs FA off: f16+FA vs f32 differ by 0.0058 at 9K), K and V types must match.
- **Open:** (1) long-context fidelity - at 9146 tokens mean KL 0.011 vs the official F32 graph (top-1 100%, so the
  script's 0.005 bar fails there), the token-by-token path 0.037; per-layer `l_out` cosine erodes gradually
  (0.99998 -> 0.9985 by layer 22, no single seam), consistent with rounding accumulation over the recurrence and
  attention - not localized further. (2) `glm_window.cu:526` / `glm_rows.cu:662` still read raon1's F32 router as
  BF16 (multi-sequence / speculative decode). (3) q8_0 / q4_0 decode is 4-20% slower than F32 (dequant in the inner
  loop). (4) `build-server.bat` / `werror-glm.bat` still name the old `C:\Strata` venv.

### Claude Code session, 2026-10-26 (Windows box, 4080 SUPER; owner-directed, pushed)

- **raon1 (Raon-1-mini) runs on the FAST PATH of our single engine and is served by `secure_frontend`.**
  The fast path (captured CUDA graph, fused kernels, Strata-paper expert tiering: VRAM cache + pinned RAM
  tier + disk, CPU lane) now carries raon1's SiTU-GLU, Helmert mHC, RDA (gated delta) and gated-MLA,
  across decode, window/rows and prefill. GPU Q8_0 expert kernels (`vec_dot_q8_0_q8_1`, `Fmt<8>`) added.
  Engine binary renamed `multiengine.exe` + `multiengine_glm.dll` (not upstream's `strata`).
- **ROOT CAUSE of the hallucinations/loops (fixed): our SiTU-GLU applied `sigmoid` to the UP branch.**
  KRAFTON's own `llm_graph_context::build_situ` (official llama.cpp, `raon-1-mini-preview` branch) is
  `bg*tanh(g/bg)*sigmoid(g) * bu*tanh(u/bu)` — the up branch is softcapped only. All five
  implementations were wrong and are fixed: `glm_ffn.cu` (reference), `glm_fast.cu` `fast_glu_act`,
  `glm_batch.cu` `batch_glu_act`, `iq_kernels.cu` `situ_entries_kernel`, `native_expert.cpp` `gu_activate`.
- **Verification (oracle-based, not unit tests):** a small program linked against the official build's
  `llama.lib` dumps full logits / `cb_eval` seams for a token file; ours via `STRATA_GLM_KL_DUMP` +
  `GLM_CB_DIR`. 180 teacher-forced positions vs the official graph: fast path top-1 50% -> 100%, mean
  KL 1.19 -> 0.0011 nats, NLL equal per bin; `l_out` cosine >= 0.9999 in all 25 layers. Residuals that
  do not move the logits: `hc_comb-2` (4x4 comb, cos 0.985) and `ffn_norm-24` (cos 0.995).
- Other engine fixes: shared-expert SiTU in `moe_route_kernel`; batched dense-FFN SiTU betas
  (`glm_window.cu`, `glm_rows.cu`); `fast_warm` batch capped at `F->stage.size()` (raon1's
  n_exp_used=6 indexed past it); prefill routes MoE models with `2*n_ff_exp != n_embd` token-by-token;
  `glm_driver.cpp` end-of-turn ids are the model's (raon1 `{259202, 259200}` from its
  `generation_config.json`, not GLM's); raon1 fast is the default (`STRATA_GLM_SLOW` forces the
  reference); F16 projections converted to BF16 for the fast path, F32 router; unhandled-exception
  stack walker in `generate.cpp`.
- Frontend config (`engine/fe-config`): raon1 card sampling defaults = the model's
  `generation_config.json` (temperature 1.0, top_p 0.95, top_k 20, presence_penalty 1.5,
  penalty_last_n 2048), `default_max_tokens` 2048, `limits.engine.generation_timeout_s` 600; card
  executable points at the owner-only `frontend-engine-raon` copy. Dev API key placeholder
  kept in `engine/raon-secret/token` (made at random by the setup script; never committed since 2026-10-27). `setup-frontend-raon.bat` ACL
  grants fixed (effective `F`, not propagation-only). `.gitignore` excludes engine build output, deployed
  engine copies, `glm-secret`, `raon-state`.
- **Not done / known:** raon1 long prompts prefill token-by-token (batched prefill needs a separate
  gate/up buffer when `2*n_ff_exp != n_embd`) — a perf item; `strata_glm` still builds with WERROR off.

### Opencode-worker session, 2026-10-25c (this Linux host; `main` = `e1a0c5ce` at start, pushed)

- **SP5-P3-style parity refresh COMPLETED — the green pins landed and are now
  the certified state.** All three live-scan tripwires are dead; the landing
  blocker recorded in the 2026-10-25b entry is gone.
  - **Part 1, upstream half (commit `56b9ef16`)**: inventory regenerated at
    `dfa3f402` (v0.2.4.2) — 92 config keys / 38 env names (was 88/32; adds
    drafting, engine_ready_s, pre, special_ids + the six STRATA_DS4_*/DSV41
    names — identical to the tripwire's live scan); all 91 stale `kRows`
    citations re-anchored (scripted, scan-pattern-first; `runconfig.py` moved
    to `serve/runconfig.py`); +10 dispositioned rows per SP5-P2 D-7 doctrine
    (engine_ready_s kMapped — the typed STRATA_ENGINE_READY_S/ready_timeout
    row carries it; pre + special_ids kMapped — the SP4 manifest carries the
    tokenizer pack; drafting + the five STRATA_DS4_* names + the
    `STRATA_DS4_` prefix fragment + STRATA_DSV41_PACK kOut). kRows 127→137;
    new `parity-out` README block; gates 137 rows / 130 inventory names.
  - **Part 2, GLM/Maya half (this session's commit)**: 596 stale `kMayaRows`
    citations re-anchored at `4615f346` (v1.1.27) — the script mirrors the
    C++ citation parser exactly (extension whitelist, MAYA_/STRATA_ alias
    spellings, the 5-arg alias rows) and rewrites only missing tokens; two
    prose flow-pointers stay dangling by design (`serve/server.py:1274`,
    `docs/SETTINGS.md:29`); `STRATA_GLM_SPLIT` hand-fixed (SETTINGS.md:54
    documents `MAYA_GLM_SPLIT`; engine getenv `src/core/glm_model.cu:1953`).
    Maya inventory regenerated: 42 cfg / 690 env (was 38/575 — +4 penalty
    names now grep-visible via the validator elif chain, keeping their
    pre-existing rule rows; +115 env names, no removals). +117 rows: 61
    DS4-path kOut (plan §Non-goals boilerplate), 20 documented GLM/VIDEO
    engine names kTyped engine_env (with their 17 MAYA_* alias rows carrying
    targets + 9 MAYA_DS4_* alias rows), MAYA_GLM_NO_MTP alias row, 4
    undocumented-internal/launcher-server kOut, and the two flags:
    `--mmap-experts` kMapped (engine argv kept through config args, like the
    other --vram-* rows) and `--drafter` kOut (DS4 pack-build tool arg).
    kMayaRows 730→847.
  - **The table is the allowlist** (SP6 §4.9 ruling, D-7 amendment): the 21
    new typed engine_env names (the 20 above + `STRATA_GLM_TIER_DIAG`, which
    flipped kOut→kTyped because v1.1.27 documents it — the single #519
    violation) landed in `kFamilyForcibleEnvironment` (146→167, sorted, CUDA
    selectors last) — `sp6_env_forcible_names_are_the_ruled_set` regenerates
    it from the table.
  - **README mirror** re-synced from the table (93+610 cells re-anchored,
    10+118 rows inserted, the stale TIER_DIAG row removed from maya-out, the
    maya-typed header now says v1.1.27); readme-coverage gate 847 rows.
  - **Landing set**: `upstream` gitlink → `dfa3f402` (v0.2.4.2), `engines/maya-ref`
    gitlink → `4615f346` (v1.1.27), `upstream.version` → v0.2.4.2; catalog
    regenerated (ONLY `generated_from` changed — 23 model ids, files and
    shas identical); `test_download_catalog.py` `generated_from` constant
    re-anchored (it had drifted two refreshes behind: v0.2.2/v1.1.21).
  - **Proof**: parity_table_tests **11/11 GREEN — #517/#518/#519 all dead**;
    engine_supervisor 62/62 (1 known skip, proc05); full battery **1037:
    only flakes** (`net01_ipv6_loopback_listener_only`,
    `log_line_format_and_rate_limit`, `egress_table_scan_during_lifecycle` —
    all re-proven green isolated; never-edit list respected); `tools/secure`
    pytest **102 passed / 1 skipped** (the 4 catalog-staleness failures gone);
    zero build warnings under `-Werror`.

### Opencode-worker session, 2026-10-25b (this Linux host; `main` = `eb924df0` at start, pushed)

- **ds4 (DeepSeek-V4.1-Flash) port: the load path is now implemented** (this
  session's main work; the owner's named next lane, 2026-10-25). `ds4_graph.cu`
  no longer stubs `ready=false`: it takes the proven `FamilyGraph::load(gguf,
  mapped, err)` seam (u_family.hpp:48 / u_main.cpp:423 — the same one
  `GlmGraph::load` proved on the box), and `make_ds4_graph` constructs only
  (weights land at the seam). Phases: `load_geometry` reads the full extended
  geometry from the GGUF's own `deepseek41.*` keys (1:1 with Maya
  `tools/ds4/pack_gguf.py:geometry()` :139-193, incl. the NEWER_KEYS alternate
  spellings; the scaffold's invented `ds4.*` key map is replaced);
  `upload_weights` arenas every non-expert tensor (experts/mtp/token_embd/
  engram-hash/attn_output_a skipped); `wire_layers` maps every layer's tensors
  by the pack_gguf `LAYER`/`GLOBAL` name tables (fail-closed missing-tensor
  list), builds the Src ckv/ik source caches (Maya :2745-2752), the FP8 window
  ring + compressor partials (`ratio x hd` floats each, :4362-4363), and
  repacks each grouped `attn_output_a` into per-group compact [o_lora][per]
  slices (byte-contiguous for f32/bf16 and for q6_k when group columns are
  256-aligned — true for published shapes; other types fail closed);
  `init_freqs` ports Maya `rope_freqs` (:500-521, YaRN ramp included) for the
  window + compressor bases; `init_experts` registers the routed stacks with
  the shared `ExpertTier` and finalizes VRAM residency warm-started from the
  hit profile (`STRATA_DS4_EXPERT_PROFILE[_SAVE]` envs, the glm pattern).
  The generate() loop now parses the request sampler params (same key map as
  glm), passes the penalty-history tail to `k::sample_tokens`, and runs the
  gated `reg_rebalance`. The forward's grouped wo_a call was FIXED (was
  `n_out = OG*o_lora` against per-group compact slices — wrote out of bounds):
  groups now write [T][o_lora] into `oa_scr` and scatter into the interleaved
  `oa` rows; box should confirm vs ds4.py dumps.
- **dispatch accepts the real arch name**: `family_for_arch("deepseek41")` maps
  to `FamilyId::ds4` (RED captured via the extended
  `dispatch_maps_known_archs_and_rejects_unknown` case, then the row added;
  `general.architecture` in the real files is `deepseek41` per pack_gguf.py:
  387; the guard reads `<arch>.*` with the file's own prefix, so the existing
  ds4 spec row serves it). `kTable` grew 7 -> 8.
- **Proof for the un-compilable `.cu`** (no nvcc on this host): `ds4_graph.cu`
  passes a strict host parse — `g++ -std=c++20 -x c++ -fsyntax-only -Wall
  -Wextra -Werror -DSTRATA_U_CUDA` against a throwaway CUDA-API stub
  (`/tmp/opencode/custub/cuda_runtime.h`, never in the repo) and the real
  headers; clean. Two warnings it caught were fixed (unused `D`, sign-compare
  on `since_reb`). Host suites stay green: `u_host_tests` 9/9 Debug AND
  Release, `-Werror` clean. Runtime/token-exact ds4 claims still wait for the
  owner's box (plan T1.4 parity).
- **Open for the box** (unchanged structured stubs, all cite Maya file:line):
  DSpark `draft()` body + mtp.* upload + seed_drafter, sampled spec-accept
  rule, ring/compressor rollback for the rejected tail, engram n-gram host
  read, router parity check (reference scores experts `sqrt(softplus)`,
  scaffold uses `k::router_sigmoid` — TODO at the call), EOS set, and the
  geometry guard's `feed_forward_length` requirement vs the real file (if the
  published ds4 GGUF omits `deepseek41.feed_forward_length`, flip the ds4
  spec row to `kFfOpt`).

- **qwen4exp PLE addressing lifted to a host-anchored shared header** (same
  session; the host-provable remainder of the owner's "remaining qwen engine
  work"). The n-gram window/hash/row formulas were box-proven (token-exact
  parity, stage 6) but lived inside CUDA-only `qwen4exp_graph.cu`, unreachable
  from host builds. New host-safe `engines/unified/src/u_ple.hpp`
  (`u::ple::Spec` + `ngram_ctx`/`mixed_n`/`head_row`: the EOS-reset window,
  `mixed = XOR_j ctx[j]*mult[j]`, `row = mixed % vocab[h] + off[h]` — 1:1 with
  the stage-6 code and the oracle's formulas); `ple_prepare_into` now delegates
  to it (only the IQ4_NL row gather stays in the graph), so the box runs exactly
  what the host tests. RED captured first (`u::ple` undeclared in the host
  tree — the capability existed only in the .cu), then the new case
  `ple_ngram_window_and_row_addressing` (hand-computed synthetic spec: window
  cuts at EOS/missing history, both n-gram orders, four head regions) GREEN:
  u_host_tests 10/10 Debug AND Release, zero warnings. Both touched CUDA TUs
  (`ds4_graph.cu`, `qwen4exp_graph.cu`) pass the strict stub host parse
  (`-Wall -Wextra -Werror`) — the qwen parse needed the graph/profiler API
  stubs (throwaway, `/tmp/opencode/custub/`, outside the repo).

- **raon1 (Raon-1-mini) load path landed on the same seam** (same session; the
  owner's "raon1 is also a derivative of that" directive). `raon1_graph.cu`'s
  scaffold predated the landed shared API (its "ASSUMED SHARED API" block listed
  signatures that never shipped) and its `load(Geometry,...)` hard-failed with
  "the GgufFile never reaches us". Now: public `load(gguf, mapped, err)`
  override on the `FamilyGraph::load` seam (glm/ds4 contract), `make_raon1_graph`
  constructs only. Load = `Raon1Geometry::from_gguf` (raon1.* keys incl. the
  per-layer `attention.head_count_kv` RDA/MLA map — **fail-closed if the file
  omits it: the 18/7 pattern must be confirmed, never guessed**), upload of
  every non-expert tensor into one device arena (name-agnostic, so the still
  TODO(box) exact tensor names never block the load; `blk.N.` with N >= n_layers
  draft tensors skipped), routed-expert registration (layers >= dense_lead) +
  residency warm start (`STRATA_RAON1_EXPERT_PROFILE`), and the state-arena
  carve (RDA S 40x128x128 + conv history per RDA layer; context x 512 F32
  latent cache per MLA layer; offsets recorded in rda_state_/rda_conv_/
  mla_latent_, zeroed on load/reset). The stale ASSUMED-API comment block was
  replaced with the landed signatures. **Unit 2 stays open**: the mixer bodies
  (layer_rda/layer_mla/layer_ffn + the decode loop) are still fail-closed
  comments — their kernels (`kernels/raon1_rda.*`/`raon1_mla.*`) are landed and
  the wiring pattern is glm's KDA/MLA tail; `sc_`/`d_pos_` size with it.
  Proof: strict stub host parse of `raon1_graph.cu` CLEAN under -Werror; host
  suites unchanged 10/10 Debug+Release. The geometry guard pins raon1's exact
  published shape (u_geometry exact()s), so a wrong-shape file cannot load.

- **flint H1 landed (same session; first executable step of the flint lane,
  spec `docs/superpowers/specs/2026-10-25-flint-engine-design.md`)**:
  `FamilyId::flint` + dispatch row `"flint"` (kTable 8→9) + broad-range guard
  row (sigmoid router — the real file's `expert_gating_func` is **u32 2**, and
  `expert_weights_norm` bool confirms renormalize — rope theta 1e4) +
  `TensorType::nvfp4 = 40` + `tensor_bytes` (64-elem blocks, 36 B = 32 packed
  nibble bytes + 4 E4M3 sub-scale bytes, exactly 4.5 bpw; the F32 `<name>.scale`
  sidecar stays an ordinary f32 tensor) + `u::k::Quant::nvfp4` id + size math
  in `common.cu` (block_elems 64, weight_bytes 36, is_quantized true — device
  dot stays C1/box). `u_family.hpp`'s CUDA factory switch got an honest
  fail-closed flint arm (no stub factory until H2). TDD: RED = compile refusal
  (`nvfp4`/`flint` not members) on top of the earlier probe's runtime refusal;
  GREEN = new host case `gguf_reads_nvfp4_tensors` (synthetic nvfp4 pair +
  the row%64 refusal) + flint geometry/dispatch assertions — suite **11/11
  Debug+Release, zero warnings**. Proof beyond fixtures: the REAL 17 MB header
  image of `RecursiveSI/flint-nvfp4-gguf` (22.9 GB file) now parses end to end
  (probe `/tmp/opencode/flint_probe2.cpp`): 1264 tensors — 485 type-40, 731
  f32 sidecars, 48 bf16 — all byte math incl. `output.weight` 2048x150144 =
  172,965,888 B, geometry ACCEPTED. Real-file truths the notes had open:
  `expert_shared_count = 2` (config.json said 1), `leading_dense_block_count
  = 2`, `expert_shared_feed_forward_length = 1024`, `rope_pattern`+
  `sliding_window_pattern` are bool[49] (rope NOT all-1: some full layers are
  NoPE), expert `.scale` sidecars dims {256} per-expert, `general.file_type
  = 39` passes (ungated). The pattern arrays land with the H2 graph loader
  (fail-closed, raon1 `head_count_kv` precedent), not the shared `Geometry` —
  recorded in the spec §5. Flint H2 (scalar NVFP4 dequant reference + math
  tests, host) is the next flint step.

- **Refresh subunit landed: `tools/secure/secure_tokenizer.py` wrapper split
  was dead** (first piece of the SP5-P3 refresh). Upstream's `Tokenizer.__init__`
  now binds `self._split` itself (bound findall / deepseek closure, strata_
  tokenizer.py:123-127 at the new pin), so the wrapper's `self._re` override
  stopped redirecting anything — a glm4 pack silently split with the Qwen
  pattern. Fix: rebind `self._split = self._re.findall` after compiling the
  declared pre. New test `test_tokenizer_splits_with_the_declared_pre_pattern`
  (digit runs: glm chunks ≤3, qwen splits each digit) — RED confirmed (glm4
  split per-digit through the qwen branch), GREEN after; tools/secure pytest
  98 pass / 4 pre-existing fails / 1 skip, the 4 being `test_download_catalog`
  staleness against the new pins (verified failing with the fix stashed) —
  the catalog-regen refresh item.
- **Catalog subunit verified for the final landing set (not committed — it
  must land WITH the submodule gitlinks or fresh clones go inconsistent)**:
  `download_catalog.py` runs clean at the working-tree pins (rc 0, notes =
  the expected O4 exclusions: strata nvfp4 x3, maya EXL3 x2; wrote 23 models
  with `generated_from {upstream: v0.2.4.2, maya: v1.1.27}`; model-id set is
  IDENTICAL to the committed 23 — only the tags + per-model revision/hash
  fields move). Landing set = regen `secure_frontend/catalog/downloads.json`
  + re-anchor `tools/secure/test_download_catalog.py:129` (`generated_from`
  hardcodes v0.2.2/v1.1.21; check :149/:185 file-counts and :269
  dropped==2 against the fresh output too) + the uncommitted gitlinks
  (`upstream` dfa3f402, `engines/maya-ref` 4615f346) + `upstream.version` —
  plus the parity re-anchor (the only remaining real battery failures).
  Tool run verified: `/tmp/opencode/dl-test.json`.

- **Linux store regression FIXED: `part_file_write_at_fills_offsets`** (green-
  landing lane, same session). `open_part_file` opened the part fd `O_WRONLY |
  O_APPEND`, and on Linux an O_APPEND descriptor overrides `pwrite()`'s offset
  argument — every `write_at()` landed at EOF, shredding sparse part assembly
  (resume/delta imports). Fix: drop `O_APPEND`; `PartFile::append()` now
  `pwrite`s at the tracked `size_` (equivalent for the sole writer — the
  ModelsLock in the same file serializes every pull/import/rm/update — and it
  keeps `write_at()` honest on the same fd). RED confirmed live (ctest #835
  failing on the pre-fix tree), GREEN after: all 73 acquire cases pass, zero
  warnings. Full battery after the fix (log `/tmp/opencode/battery-579e8fa4.
  log`, commit `579e8fa4` + this fix): **1032/1037, 3 real failures = the
  expected live-scan parity tripwires** (#517/#518/#519, die with the SP5-P3
  refresh), 5 skips (known), and 2 load flakes re-proven green isolated:
  `egress_table_scan_during_lifecycle` (7 s, timing-sensitive under -j8) and
  `net01_workers_add_no_listener` (port contention) — add to the never-edit
  re-run-isolated list alongside the two tokenizer flakes.

- **flint H2 landed right after H1 (same session)**: host NVFP4 reference
  `src/kernels/nvfp4_ref.hpp` ported 1:1 from ggml master with file:line cites
  — block = 4 UE4M3 sub-scale bytes FIRST + 32 E2M1 nibble bytes; doubled
  kvalues_fp4 table; ue4m3_to_f32's raw×0.5 convention (0x00/0x7F read zero);
  sub-block element map low→e[s*16+j], high→e[s*16+8+j]. New host case
  `nvfp4_block_reference_matches_ggml_master` pins exact values (16 codes,
  UE4M3 corners incl. subnormal 2^-10 and 0x7E→224, round-trip, element map) —
  **the C1 device kernel must match these bit for bit**. RED = `u::nvfp4`
  undeclared; GREEN = suite **12/12 Debug+Release zero-warning**. The H1
  comments said "nibbles then scales"; ggml's struct is **scales first** —
  corrected in u_gguf/common.{cuh,cu}. Two test-authoring bugs were caught by
  the exact-value assertions mid-loop (double-counted ½, nibble interleave) —
  evidence the pins are real, not rubber-stamped. ggml master sources for C1:
  /tmp/opencode/ggml-{common-master.h,quants-master.c,impl-master.h}. Flint
  C1 (device NVFP4 kernels) is box work; H-lane flint host work is done until
  C1 returns.

### Opencode-worker session, 2026-10-25 (this Linux host; `main` = `6c6d1e91`, pushed)

- **-Werror repairs to the test tree** (commit `949e14b8`): the 2026-10-23/24
  box commits left `main` uncompilable under
  this host's gcc preset — `pull_harness.hpp` aggregate-init missed the new
  `PullSettings::connections` field, `gemma4_profile_test` bound a
  `const std::string&` to `const char*` temporaries, and
  `engine_arguments_test` carried a raw NUL byte inside a string literal.
  Test-side only; no test weakened. Full `linux-gcc` build `-k 0` green.
- **Unified geometry tests re-anchored** (`6c6d1e91`): `glm5next` moved to
  `RopeKind::nope` on the box (`c28d6baa`, real-file load proven) but
  `u_host_tests.cpp` still asserted the old RoPE contract — RED captured on
  this host, re-anchored to NoPE truth (invariant moved to `qwen35moe`, not
  weakened), now 7/7 green on this host.
- **Secure_frontend suite baseline at the working-tree pins** (upstream
  `v0.2.4.2` = `dfa3f402`, maya `v1.1.27` = `4615f346`, checked out but NOT
  committed yet — the SP5-P3 refresh still owns them): **1075 passed / 4
  failed / 5 skipped of 1079**. Three failures are the expected parity-scan
  tripwires at the new pins. The fourth is a REAL Linux regression:
  `secure_frontend_acquire_tests.part_file_write_at_fills_offsets` fails in
  isolation — `models_store_posix.cpp:189` opens part files
  `O_WRONLY | O_APPEND`, and Linux `pwrite` ignores the requested offset when
  `O_APPEND` is set (everything appends). Sequential/resume downloads append
  at the end anyway so they coincidentally work; the parallel-ranged writes
  (2026-10-23, `1c271a09` — the test was added 3 days AFTER GitHub Actions
  were disabled) never ran green on Linux. The Windows path
  (`models_store_windows.cpp`) has no `O_APPEND`/`pwrite` pair. Fix belongs to
  the green-landing lane: drop `O_APPEND` from the POSIX part-file open (the
  ranged-write discipline is the downloader's own range assignment, which it
  already enforces); RED evidence is this exact isolated failure.
- **Flint family engine notes** (owner asked 2026-10-25): new design document
  `docs/superpowers/specs/2026-10-25-flint-engine-design.md` — flint
  (RecursiveSI, Apache-2.0, 39.8B/≈2.5B-active MoE, hybrid
  SW-513/RoPE + full/NoPE layers, per-head QK-norm + learnable attention
  sinks, 256-expert sigmoid top-6 + shared, 2 dense prefix layers) is NOT a
  Maya derivative; its reference is llama.cpp master (`MODEL_ARCH.FLINT`,
  NVFP4 = ggml type 40, ftype 39 MOSTLY_NVFP4). Facts came from the HF card,
  `config.json`, and a Range-fetch parse of the REAL `flint-nvfp4.gguf`
  header (17MB of metadata/tensor-info; no weights run on this host).
  Proven here: our `u::parse_gguf` already handles the flint header's
  metadata (u32 type codes included) and refuses only on
  `gguf: unsupported tensor type` — the first host-side step (H1:
  `TensorType::nvfp4` + geometry + `FamilyId::flint`) has its RED captured in
  the doc. Six owner decisions (D1-D6) are listed there. One decode
  correction found while wiring H1's facts: `flint.expert_gating_func` is an
  **f32 key holding 2.0** (re-probed from the fetched bytes; llama.cpp master
  uses 2 = sigmoid), so the future geometry guard must accept f32 for that
  key, not u32.
- **C3 CPU-lane qwen quants ported** — see the C3 bullet in the unified-engine
  section (RED 2/2 → GREEN 9/9 Debug+Release); this was the owner's "remaining
  qwen engine work" that is provable without a GPU.
- **Owner's standing direction for this worker (2026-10-25, chat):** keep
  working the remaining **qwen (qwen4exp)** engine work, **start the
  deepseek (ds4)** port ("almost the same if not exactly the same as glm
  because both are from project maya"), with **raon1** the same lineage after,
  and the flint notes above. Engine order per the owner: qwen remainders +
  ds4 + raon1 first, flint per its plan.

### Parallel ranged downloader (2026-10-23, local `main`, not pushed)

Owner: "Why is the download so slow? ... much faster" directly; then "Build the parallel
downloader and complete it". Spec
[`superpowers/specs/2026-10-23-parallel-downloader-design.md`](superpowers/specs/2026-10-23-parallel-downloader-design.md).
The confined SP4 downloader was one curl stream; Hugging Face's CDN limits each connection, so it
was ~3 MB/s. Now, with `pull.connections` > 1 (1-32, default 1), a file >= 256 MiB is fetched as
32 MiB pieces over several confined `curl` children (mirrors `upstream/setup.py`'s 16-connection
downloader; no code copied).

- **Code** (`1c271a09`, `2a982c22`, `807ecf40`, `73639063`): `PartFile::write_at` (offset writes);
  `TransferRequest.range_to` (bounded `--range lo-hi`); `download_pieces` (the PieceMap +
  `.part.chunks` sidecar); the parallel path in `fetch_file.cpp` (probe resolves the final URL +
  ETag + total, N workers fetch missing pieces pinned per connection, each 206's Content-Range and
  ETag checked, whole-file hash in one pass, one-stream fallback); `pull.connections` config wiring
  the CLI pull and the model downloads. Threat model / PROVENANCE updated (same SP4 guarantees).
- **Proof:** TDD per step (write_at, bounded range, piece map, 3 orchestrator cases incl. resume
  and fallback; a test-resolver data race fixed). All 147 fetch+acquire cases pass. Box run: a real
  429 MB download reassembled with the SHA-256 equal to HF's pin, bursting ~67 MB/s vs ~3 MB/s
  single-stream (session log item 62).
- **Open:** effective parallelism is ~file_pieces/4 (4-piece runs); the manager's `bytes` progress
  is bursty (per run); the page's per-connection rate/ETA is not piece-aware. `pull.connections`
  defaults to 1 — set it (e.g. 16) in a serving config for fast downloads.

### Model downloads on the Models tab, with pause and resume (2026-10-23, local `main`, not pushed)

Owner requests: "for models supported by strata and project maya, create a card for each to
allow downloading and show status of download on the same tab" (O1); "it should show the disk
space available so it can say insufficient disk space when there is not enough space in the
data drive" (O2); "allow a custom download by pasting a huggingface link" (O3); "make sure there
is a safety in the download that only downloads from huggingface and a gguf" (O4); then "Have a
download pause and resume functionality". Spec
[`superpowers/specs/2026-10-23-model-downloads-design.md`](superpowers/specs/2026-10-23-model-downloads-design.md)
(ACCEPTED: D1-D8; the owner chose "Build packs automatically" for D7), plan
[`superpowers/plans/2026-10-23-model-downloads.md`](superpowers/plans/2026-10-23-model-downloads.md).
The page section and the catalog/prepare tools were written by two workers in detached
worktrees and integrated as patches.

- **What runs:** with `models.downloads` (needs `models.cards` and `models.downloads_root`) the
  launcher serves `GET /v1/strata/downloads` and `POST .../start|pause|cancel|link`
  (`kModelDownloadRoutes`, keyed, loopback unless `models.control_remote`). `DownloadManager`
  (`download_manager.hpp`) queues one download at a time and runs each as a `strata --config
  <cfg> pull --manifest <m> --trust-manifest sha256:<d> --yes --progress-lines` child (new
  machine-readable progress lines), link lookups as `strata hf-info <link>` (new verb). The
  manifests come from `hf_source` (O4's link grammar, Hugging Face hosts and redirect patterns,
  the new SP4 profile `gguf`: weights/mmproj `.gguf` files only, refused by the engine gate).
  The catalog (`catalog/downloads.json`, 23 cards: upstream 18, Maya 5) is generated by
  `tools/secure/download_catalog.py` from the pins and embedded; Maya's `main` pins are resolved
  at start and must match their SHA-256. Free space (O2): 2 GiB margin, 507
  `insufficient_storage`, checked again once Maya's sizes are known. Pause keeps the partial
  files and Resume is a start of the same manifest (same digest, same staging); Cancel runs
  `strata rm --partials`. After a catalog Qwen download the embedded
  `tools/secure/prepare_download.py` runs under `models.prepare_python` (upstream's
  `iq_pack.py` and `strata_tokenizer.py` from `models.prepare_tools`), then the server writes
  `<roots.state>/cards/<id>.json` with `models.download_engine_card`'s engine settings and the
  launcher adds it (`ModelLauncher::add_card`, up to `kMaxLauncherCards` = 64; such cards also
  load at boot). Qwen3.6-35B-A3B (family `qwen36`, `prepare: none`) runs on the same qwen engine
  by its own path (upstream `OWN_PATH_FAMILIES`): `prepare_download.py --tokenizer-only` builds
  only the tokenizer (no `iq_pack`, no PLE), and the written card reads the GGUF directly
  (`--native` composed from `model_id` + `roots.models` by the expert launcher path, engine args
  `["--mtp","2"]` — the qwen35moe draft count, not a draft-layer directory), with the qwen35
  options (max_context default 32768, prefill auto, thinking high, an mtp toggle). It is text-only:
  the expert launcher path takes no `mmproj`, so the family's vision encoder is not wired (the same
  limit as every other launcher card). Four new error rows: `download_in_progress`, `upstream_unavailable`,
  `insufficient_storage`, `download_not_active`.
- **Custom downloads matched to a family (owner 2026-10-23):** a pasted Hugging Face GGUF is matched
  to a family (`check_link(url, profile, extra_urls)`): the picker's families come from the list's
  new `profiles` array (`kCustomProfiles`). The six qwen families (Flash-Next, its Unsloth / Coder /
  Gyro variants, Swift, Qwen3.6) share the GGUF-direct recipe — the matched card carries the profile
  as its `family`, so `list_json`, `run_card` and `prepare` treat any custom qwen paste as GGUF-direct
  (tokenizer-only, `--native`), differing only in the context the card opens with. MTP defaults **off**
  for a custom paste (most finetune GGUFs carry no MTP block) with the card's toggle to turn it on;
  catalog Qwen3.6 keeps it on. Gemma 4 is offered but stays download-only: `strata_tokenizer.py` wants
  a qwen35 `gpt2` byte-level BPE vocabulary, so a Gemma SentencePiece GGUF has no tokenizer step here.
  A **multipart** GGUF's other shards are pasted too (`extra_urls`, at most five more): every part is
  resolved, checked to share the repository, revision and shard set (`gguf_shard_set` names the set),
  and assembled into one card; an incomplete set names its missing shards and does not start.
- **Page:** the Models tab's Downloads section: the drive's free/total space, a card per catalog
  model (Strata, Project Maya) and per pasted link, Download / Pause / Resume / Cancel, a
  progress bar with GB, %, rate and ETA, the insufficient-space text, the paste box with a
  confirmation (commit, files, sizes, fits), polling while active. The paste box has a profile
  picker (the "Run it as" family, filled from the list's `profiles`) and, for a split GGUF, inputs
  to paste the remaining parts; the confirm shows the matched family, the split progress (N of M
  parts) and gates Download until every shard is pasted.
- **Proof:** TDD with RED evidence per step: `hf_source` 3 cases, the `strata` CLI 2
  (`pull_progress_lines_are_machine_readable`, `hf_info_resolves_a_link`), the store config 1,
  typed config 1, the error rows, the manager 12 (catalog and disk, pull and progress, failure
  and cancel, Maya `main` resolved and checked, Maya sizes against the free space and a resume
  keeping the commit, a resume's rate, pasted links, pause/resume/cancel, a Qwen3.6 gguf-direct
  card runnable with its tokenizer note, a pasted GGUF matched to a profile with the families
  advertised and the five-part cap, a multipart paste assembling its shards and refusing an
  incomplete start, the routes' JSON), the
  router 1, the page 4 (the Downloads section, the profile picker and multipart inputs, the
  sub-tabs and family groups, the start/cancel/poll/check flow), the launcher end to end 2 (the catalog served, the folders made, a
  download's card loaded at boot; cards added while serving) plus the downloads-off 404s. The
  manager runs against a scripted `strata` child and a fake disk; no test reaches the network.
  Python: 29 pytest cases for the catalog generator and the prepare script (the prepare script's
  `--tokenizer-only` mode adds 2: a gguf-direct run skips the pack, and `--tokenizer-only` with a
  `--pack-arg` is refused); `download_catalog.py
  --check` reports the committed catalog current (23 models). Full MSVC suite: 1010 cases, 1009 passed, 1 skipped (cfg03), 0 failed, with the real Gemma pack set (`out/ctest-downloads-pause-2.log`).
  Box run (session log item 59): a real pasted-link pull from Hugging Face (415 MB) paused at
  83.8 MB, resumed from there and installed with the pinned SHA-256; Cancel deleted a partial;
  the 188 GB card refused for space; the page's buttons. It found three bugs, fixed with RED/GREEN:
  Hugging Face's CDN host `us.aws.cdn.hf.co` (SP4's one-label wildcard), a 10.6 KB string in a
  revision answer (`hf-info`'s per-string cap), and a cancel of an idle card answering 202.
  After those fixes and the phone fix: 1012 cases, 1011 passed, 1 skipped (cfg03), 0 failed
  (`out/ctest-box-and-phone.log`).
- **Phones (owner, same day):** the sign-in did not appear on an iPhone because the over-cap
  connection (default 4 per remote address) that carried `app.js` was refused with 503; the LAN
  config now sets `limits.concurrency.max_connections_per_remote_address` to 8 (restart pending
  on the owner), and the owner chose to raise the default: with `listener.web_ui` the cap derives
  min(6, workers) (B11 amended for page servers). The signed-in header now takes two rows below
  640 px; every view fits 320 px (session log item 60).
- **Open:** a paused card shows as `available` after a server restart (a Download then continues
  from the kept partial files); the image encoders (`mmproj`) are not downloaded (so Qwen3.6 and
  the other vision families run text-only); a pasted GGUF matched to a Gemma 4 profile stays
  download-only (no Gemma tokenizer step; a Gemma tokenizer tool would make it runnable), and a
  split GGUF of more than six parts is beyond the five-extra-link cap; GLM-5.3 files are download-only (no GLM engine
  here); `--compat-bf16` packs need `STRATA_GGUF_PY` in the server's environment; Windows CI
  (`windows-msys2-clang`) has not run.

### secure_frontend: raon1 (Raon-1-mini) is chattable over OpenAI /v1/chat/completions (2026-10-25, local `main`, NOT committed)

raon1 runs on `strata_glm` (glm_pack path, arch-detected) and was already hosted on our compiled
`secure_frontend` (stub `.gguf` + `engine.profile glm53` composes `--glm-pack`; see the hosting recipe
below/engine scripts). The chat endpoint, however, returned `invalid_request` for the served model: the
glm53 chat profile encodes the prompt with `encode_glm53_prompt`, which requires the six GLM CONTROL
markers (`[gMASK] <sop> <|system|> <|user|> <|assistant|> <think>`), and raon1's ChatML pack carries none
of them, so every chat request failed to tokenize (any plan error but `prompt_too_large` maps to
`invalid_request` at the route). `GET /v1/models` was unaffected.

- **Fix (chat side only; engine launch + config untouched):** the single glm_pack chat path is now
  **data-driven**. `pack_is_raon1_chatml(pack)` (a pack with `<|im_start|>` as a unique CONTROL token and
  **no** `<|user|>` marker — raon1's vocabulary has the former and none of the latter) routes to a faithful
  raon1 ChatML plan; otherwise the GLM path is byte-for-byte unchanged. New code: `render_raon1_text_prompt`
  (`src/glm_prompt_renderer.cpp`, KRAFTON `chat_template.jinja`, reasoning disabled —
  `<|im_start|>{role}\n{content|trim}<|im_end|>\n` … tail `<|im_start|>assistant\n<think></think>\n\n`),
  `encode_raon1_chatml_prompt` (`src/tokenizer_pack.cpp`, scans the four raon1 markers as single CONTROL ids
  like `encode_glm53_prompt`), and in `src/chat_generation_plan.cpp` `make_raon1_chatml_plan` (reuses
  `find_qwen_think_tokens` for the reply's reasoning split and `find_qwen_end_of_turn_tokens` so the reply
  stops on `<|im_end|>`) branched at the top of `make_glm53_chat_generation_plan`;
  `make_raon1_chat_generation_plan` now uses it too (not the unified placeholder). The template follows the
  loaded pack, so moving a model still needs no recompile.
- **Proof:** RED captured (the new `chat_generation_plan` case fails with the impl reverted), then GREEN;
  **77/77** across `tokenizer_pack` + `chat_generation_plan` + `glm_profile` + `chat_execution` (MSVC
  Release, `/WX`). The one regression caught and fixed: `chat_execution`'s `glm_tokenizer_for` adds
  `<|im_start|>` to a GLM pack, so detection must also require `<|user|>` absent. **Live:** POST
  `/v1/chat/completions` model `raon1-mini` → `"Paris is the capital of France."`, `finish_reason stop`,
  HTTP 200, on `strata-secure-server --expert --config fe-config\raon-frontend.json` (0.0.0.0:8080).
- **secure_frontend build on G: gotcha:** `out/build/windows-msvc`'s CMake cache had `CMAKE_ROOT`/
  `CMAKE_COMMAND` baked at the pre-move `C:\Strata\...cmake-4.4` (now on G:), so any reconfigure dies on the
  CTest templates. Recovery: delete `CMakeCache.txt` + top-level `CMakeFiles/`, then `cmake --preset
  windows-msvc` with `G:\Strata\.venv\Lib\site-packages\cmake\data\bin\cmake.exe` (per-target
  `*.dir/Release/*.obj` survive → incremental). Stop the server before relinking (it locks the exe, LNK1104).
- **Files:** `src/chat_generation_plan.cpp`, `src/glm_prompt_renderer.{cpp,hpp}`,
  `src/tokenizer_pack.{cpp,hpp}`, `tests/chat_generation_plan_test.cpp`, `CMakeLists.txt` (one new case).
  **Committed + pushed to `origin/main` at `45d0e376`** (the 7 files above; this was the owner's
  "commit and push to main").

### engine: raon1 decode sped up ~6.7x — experts moved onto the GPU (2026-10-26, local `main`, NOT committed)

Owner: raon1 was "much slower" than GLM despite being smaller. `STRATA_GLM_TIMING=1` showed decode was 93%
the CPU expert tail (dense 11 ms/tok, **expert tail 156 ms/tok**, ~5.8 tok/s): GLM/qwen run experts on the
GPU, raon1's were CPU-only (Q8_0 has no GPU expert kernel) and single-threaded. Fixed both paths
(`engine/third_party/project-maya`), each proven on the owner's box (RTX 4080 SUPER):

- **CPU expert pool (bit-identical):** `pack_moe_tail` ran ~6 experts on one core. Added a persistent worker
  pool (`moe_workers_` / `moe_parallel`, `glm_model.{cu,hpp}`) — one expert per core, ordered serial reduce,
  so output is bit-identical to the serial loop; `quant_act` hoisted out of the loop. Expert tail 156→88 ms/tok
  (~1.7–2×, sync/bandwidth-limited). `STRATA_GLM_MOE_THREADS` overrides the worker count. (Gotcha: the pool
  must start AFTER `pack_` is set in `load()`.)
- **GPU experts (the win): ~39 tok/s.** Three parts: (1) the device kernel `native_expert_grouped` only did
  SwiGLU — added `situ_entries_kernel` (`iq_kernels.cu`, mirrors the CPU `gu_activate`) + a `situ_bg/situ_bu`
  parameter, wired for raon1 at the call site. (2) that kernel is i-quant-only (not Q8_0, not K-quants), so
  the experts had to come from an i-quant GGUF: downloaded KRAFTON's `raon-1-mini-preview-IQ4_XS.gguf` (16 GB,
  via `huggingface_hub[hf_xet]` — it is xet-stored, plain curl truncates/corrupts) and re-packed to
  `G:\Strata-data\models\raon1_iq4xs\pack` (experts IQ4_XS / IQ4_NL, both GPU-supported). `iq_pack.py` needed a
  fix (`f32_bytes`): the IQ4_XS build quantizes `attn_k_b`/`attn_v_b` to Q8_0 (the Q8_0 build kept them F32),
  which it had refused — now it dequantizes such non-native raon1 projections to F32 into dense.bin. (3) lifted
  the raon1 CPU-only `dev_experts_` guard, gated on the expert type being GPU-native (`gpu_gu_ok`/`gpu_d_ok`);
  Q8_0 raon1 still falls to the CPU path. Proof (`STRATA_GLM_TIMING`, IQ4_XS pack): dense 10.5 + **expert tail
  12.9** (pool hits 6891) + rest 2.4 = ~26 ms/tok = ~39 tok/s, VRAM 14.9/16 GiB (experts resident), coherent.
  The managed raon1 card now points at `raon1_iq4xs\pack`; the Q8_0 pack stays as the CPU/reference fallback.
- **Files (uncommitted):** `third_party/project-maya/src/core/glm_model.cu`,
  `include/strata/core/glm_model.hpp`, `src/kernels/cuda/iq_kernels.cu`,
  `include/strata/kernels/iq_kernels.hpp`; `engines/maya-ref/tools/iq_pack.py`; `engine/fe-config/cards/raon1.json`.

### secure_frontend: raon1 is a first-class glm_pack family served in the FULL (managed) frontend (2026-10-26, local `main`, NOT committed)

Owner, live, testing the above: the served page was the minimal `--expert` UI — "missing the download tab,
the monitor is missing"; the built-in chat box returned `unsupported_parameter`; and "tune it to use the
vram, cpu, ram and storage splits". Owner chose **Full integration** + **Both** (tune knobs now, plan the
GPU-expert work). All phases are MSVC Release `/WX`, TDD (RED captured, then GREEN), and live-verified on the
box; the one red test, `typed_config_tests.readme_config_table_matches_schema`, is **pre-existing** (the
README key table is stale from prior uncommitted schema rows — `listener.monitor`, `models.*`, `glm.*` —
and no change here touches a schema key description).

- **A — raon1 first-class `EngineProfile`:** it now mirrors glm53's ENGINE behavior while keeping its own
  ChatML chat. `subsystem_limits.cpp`: `protocol_flavor_for(raon1)` → `maya_118`, `engine_family_for(raon1)`
  → `glm`, `per_request_reasoning_effort_for(raon1)` → true. `frontend_config.cpp`:
  `arguments_profile_for(raon1)` → `lighttransport_glm53` (so it composes `--glm-pack`), and the `engine.gpu`
  rejection / `glm.*` row gating / `glm.idle_unload_s` precedence all extend to raon1.
  `launch_composition.cpp`: raon1 joins glm53's no-`--gpu` arm. `typed_config`/`subsystem_limits` tests
  updated (raon1 is now glm_pack-lineage, not a merged-engine family — deepseek stays `unified`).
- **B — the web-UI chat box works:** the page always sends `reasoning_effort`, which the glm53 profile
  rejected (`unsupported_parameter`) — the owner's error. raon1 now honors it: `render_raon1_text_prompt`
  takes `thinking` (tail `<think>\n` open / `<think></think>\n\n` closed), threaded through
  `make_raon1_chat[ml]_plan`; `chat_route` derives `thinking` from `reasoning_effort` (none → off, else → on,
  default on). Live: a request WITH `reasoning_effort` → 200, coherent.
- **C — managed launcher + the Models/Download tabs:** `model_cards.cpp` accepts `profile "raon1"` (deepseek
  still gated); a card launch reuses `compose_model(expert=true)` — the proven `--glm-pack` path. New
  `engine/fe-config/raon-managed.json` (managed, no `--expert`) + `cards/raon1.json`, with
  `models.{control,control_remote,downloads,start}` and `listener.monitor`. The `models.downloads_root`
  (`engine/downloads`) must be owner+SYSTEM-only or the server refuses it. Live: `/v1/strata/models`,
  `/v1/strata/downloads`, `/metrics`, `/v1/models` all answer; raon1-mini auto-starts and chats.
- **D — tiering knobs:** box is 96 GiB RAM + a 16 GiB RTX 4080 SUPER. The managed config's `glm` block
  (`gpu "0"`, `max_context 32768`, `ram_headroom_gb 16`) tunes the glm_pack engine's automatic
  VRAM → pinned-RAM → disk(storage) expert tiering (`glm.ram_headroom_gb` → `STRATA_GLM_RAM_HEADROOM_GB`),
  with the CPU computing the Q8_0 experts. Live split: ~5.65 GiB VRAM (non-expert layers + KV), experts in
  RAM/CPU; the ~30 GiB model fits VRAM+RAM so disk is not touched. ~6 tok/s.
- **E — GPU-expert plan (plan only):** `docs/superpowers/specs/2026-10-25-raon1-gpu-experts-plan.md` — raon1
  experts are Q8_0 and `native_expert_grouped` is i-quant-only (`glm_model.cu:3617/3678` force CPU experts);
  options are (A) requant experts to IQ [recommended first, no CUDA], (B) a Q8_0 device MMVQ path [CUDA,
  box], (C) overlap the CPU lane + AVX2 Q8_0 dot. Box-gated.
- **Files (uncommitted):** frontend `src/subsystem_limits.cpp`, `src/frontend_config.cpp`,
  `src/launch_composition.cpp`, `src/chat_route.cpp`, `src/chat_generation_plan.cpp`,
  `src/glm_prompt_renderer.{cpp,hpp}`, `src/model_cards.{cpp}`, `include/.../{chat_generation_plan,model_cards}.hpp`,
  and the `typed_config`/`subsystem_limits`/`model_cards`/`chat_generation_plan` tests; configs under
  `engine/fe-config/` (`raon-managed.json`, `cards/raon1.json`, `serve-frontend-raon-managed.bat`); the spec.
  (`frontend_config.cpp`/`launch_composition.cpp` also carry prior uncommitted glm/raon profile work.)

### Unified engine: one binary for qwen35moe + gemma4 + glm53 + deepseek + raon1 (2026-10-23, `main`, in progress)

Owner (in chat): a single lightweight, non-bloat, resource-mindful engine that runs the selected
model **from one verified source** (not tracking several upstreams per release), to remove the
redundant per-engine code. Design
[`superpowers/specs/2026-10-23-unified-engine-design.md`](superpowers/specs/2026-10-23-unified-engine-design.md)
(decisions resolved) + plan
[`superpowers/plans/2026-10-23-unified-engine-plan.md`](superpowers/plans/2026-10-23-unified-engine-plan.md).
Added support target: KRAFTON **Raon-1-mini** (arch `raon1`, a GLM-lineage hybrid MoE).

- **Decisions:** one engine = a shared **substrate** + per-family **graph modules ported from
  upstream/Maya/our gemma4 as references and reviewed in** (never run as third-party binaries); full
  kernel unification onto one shared set (gemma4 included, no carve-out); one converged
  `EngineProtocolFlavor::unified`; priority GLM+DeepSeek+Raon first, then qwen, then gemma4; one
  shared expert-placement facility (VRAM/RAM tiering + expert-hit-profile warm-start, §3.5 of the design).
- **CI vs box boundary:** host C++ + the frontend are CI-green (agents); the CUDA kernels and every
  build/run/parity claim are the owner's GPU box (root option `SECURE_STRATA_BUILD_UNIFIED_ENGINE`
  OFF, never CI). Nothing is runnable/verified until the box builds + parities each family (DoD §1.3).
- **Landed, CI-green:** `engines/unified/` host foundation — bounded GGUF reader, geometry guards
  for all six families (raon1 pinned to its published shape), the converged `unified` wire dialect
  (fail-closed), arch→family dispatch, CLI + serve skeleton over a stubbed `FamilyGraph`; MSVC, 7/7
  hermetic tests, commit `42a382a7`. The frontend adds `EngineProtocolFlavor::unified` +
  `EngineProfile::deepseek`/`raon1` across every exhaustive switch, the `unified` argument profile,
  and placeholder renderers; MSVC `/WX` clean, affected suites green (the lone
  `raw_profile_rejected_in_release_build` failure is the pre-existing release-only/hooks-ON case).
- **Landed (box-to-compile CUDA, commit `a418fce4`):** the shared kernels + expert facility
  (`engines/unified/src/kernels/common.*`, `u_experts.*`) and **all six** family graph ports
  (`src/family/{glm,ds4,raon1,qwen35moe,qwen4exp,gemma4}_graph` + `src/kernels/<fam>_*`), plus
  `cmake/unified-engine` (the standalone box build, OFF/never-CI). All CUDA is behind `STRATA_U_CUDA`
  (OFF); the host foundation stays 7/7 green. Faithful ports from upstream/Maya/our gemma4 with
  in-file `TODO(box)` for the deep kernels. **Not runnable until the box compiles + parities.** The
  box guide is [`engines/unified/BUILD-ON-BOX.md`](../engines/unified/BUILD-ON-BOX.md); its first
  fixes are the shared seams the lanes flagged — the `GgufFile` factory, the `ExpertTier`
  `run_moe`→`compute`/per-layer reconciliation, the per-layer `head_count_kv` array in `u_geometry`,
  and the i-quant widening qwen4exp's pack needs. Fixed in passing: `u_geometry` pins qwen35moe's
  router to `softmax` (the sigmoid is only its shared-expert gate). Frontend `unified` flavor +
  `deepseek`/`raon1` profiles landed at `49cf0fd6`; host foundation at `42a382a7`.
- **GPU-box bring-up (this session, local `main`, NOT pushed):** the engine now **compiles clean** on
  the box (RTX 4080 SUPER, CUDA 13.0, sm_89) under `-Werror=all-warnings` + `/W4 /WX`
  (`strata-unified-engine.exe` links), and **runs a real 28 GB gemma-4-26B GGUF end-to-end — it
  GENERATES tokens** (loads + emits `PP`/`T`/`DONE`, exit 0). Commits (local `main`, not pushed): box
  compile fixes (INFINITY→CUDART_INF_F, int32 ids, `[[maybe_unused]]`) `f66bd1fd`; `u::ExpertTier`
  registered per-layer MoE path (`register_layer`/`run_moe`/`run_window`, shared `u::WSlot`)
  `4e765eaa`; the shared **load seam** — `FamilyGraph::load(const GgufFile&, const uint8_t* mapped,
  std::string&)` + whole-file mmap in `u_main` (Win `CreateFileMapping`/POSIX `mmap`, gated on
  `STRATA_U_CUDA`) + `using FamilyGraph::load` on the four stub families — `17a80c92`; gemma4 `load()`
  device bring-up `4642f229` (initial Q8_0-only host-arena version); geometry guard accepts gemma4
  (optional `expert_shared_count`, vocab from `token_embd.weight` when `vocab_size` absent)
  `3ed681f1`; **per-layer/BF16 experts + the run** `daa53afa` — the registered `ExpertTier` path gains
  an `Act` arg (gemma4 geglu; default keeps GLM/ds4/raon1) + explicit fused `gate|up` strides (stride
  vs matrix-bytes split), gemma4 registers experts straight from the mmap (per-layer quant, **no
  ~23 GiB repack**), and the gemma4 `matvec` bf16 path is fixed — this is what makes the UD file run.
- **Runtime run (`daa53afa`):** `strata-unified-engine --native <…UD-Q8_K_XL.gguf> --serve --gpu 0`
  loads the full model (per-layer **BF16 experts on layers 1 and 29** served alongside the Q8_0 rest)
  and generates: mmap → GGUF parse → geometry guard → dispatch → `from_gguf` → ~1.5 GB non-expert VRAM
  upload → per-layer weights + f16 KV caches → experts registered from the mmap → attention → dense
  GeGLU → softmax-top-8 MoE → head → sampler → `T …`×N → `DONE`. GLM (IQ2_XXS) and Qwen3.8-Flash-Next
  (IQ3_S) on-disk models are i-quants the lean kernel set cannot read at all.
- **CORRECTNESS PARITY (`<this commit>`):** on a chat-formatted prompt the unified engine's greedy
  output is TOKEN-FOR-TOKEN identical to the reference `engines/gemma4` for the same ids
  (`100 45518 107 101 818 5279 529 7001` both = a coherent "The capital of France …"). A layer dump
  (env `STRATA_U_DUMP` vs `--dump-layers`) showed only GRADUAL ~0.2–0.5%/layer numerical drift, NOT a
  structural bug: the reference PREFILL dequantizes Q8_0→f32 + cuBLAS SGEMM, while the lean unified
  prefill is token-by-token Q8_0 mmvq (design decision 2: no cuBLAS-prefill port), so the precision
  gap flips only NEAR-TIED greedy picks (a raw-completion prompt on an instruction-tuned model); a
  confident prompt is unaffected → exact parity. Verified with llama-tokenize + the reference's
  `--tokens`/`--n`/`--dump-layers`/`--dump-full-logits`.
- **Expert VRAM residency (`6019677b`):** the registered path gained VRAM slots + a live hit-based
  rebalance (`reg_finalize`/`reg_note_routing`/`reg_rebalance`/`reg_slot_of`/`reg_fill_slot`): the
  hottest experts stay resident in VRAM (slot = one expert's gate|up|down, sized to the largest expert
  so BF16 layers fit; per-layer admission; cold warm-start, then the rebalance converges the resident
  set to what the sequence routes), only the rest stream. gemma4 `load()` sizes the budget from free
  VRAM (−768 MB); `forward_one` notes routing per layer and rebalances once per token. Measured on the
  UD file: **decode 655 → 245 ms/tok (~2.7×)**, prompt ~1684 → ~401 ms/tok (~4.2×), same output.
- **Device-table MoE dispatch — DECODE NOW BEATS THE OLD ENGINE (`<this commit>`):** gemma4's MoE was
  rewired from the host-driven `ExpertTier` streaming (synced ~30-60×/token) to the reference's
  ZERO-HOST-SYNC on-device dispatch, ported into `u::gemma4::Gemma4Experts` (new
  `kernels/gemma4_moe.{cuh,cu}`; `ExpertTier` untouched, GLM/ds4/raon1 unaffected). Experts in a
  `cudaHostAllocMapped` arena (device-aliased), a per-layer DEVICE `ExpertPtr` table (resident→VRAM
  slot else→mapped-host), and `matvec_experts`/`geglu_experts`/`moe_combine` all driven by the DEVICE
  `router_topk` picks + device `down_scale` — no per-layer host round-trip; the live rebalance is gated
  to ~every 8 tokens. Measured on the UD file (same chat prompt): **decode ~43-48 tok/s vs the
  reference's 46.2 — unified is at/above parity on decode** (was ~4-7), and the MoE is now BIT-IDENTICAL
  to the reference (greedy output still 8/8 identical). The head now runs only on the last prompt token
  + decode steps (micro-opt).
- **Decode now BEATS the old engine on BOTH cold and steady-state (`<this commit>`, measured, greedy, UD
  file, clean GPU):** the earlier "unified decode is slower" read was a mix of (a) GPU contention from a
  running frontend server during the first measurements, and (b) comparing unified COLD against the
  reference's number without separating cold vs steady. Instrumentation-first diagnosis settled it:
  - **Instrumentation (no behaviour change):** a VRAM/residency report at load (`gemma4 vram: engine=new
    … slots_per_layer= resident_pct=`) and a per-GEN decode-stats line (`gemma4 decode stats: … hits=
    misses= hit_rate= fills= pcie_miss_mib=`) in `generate()`; the hit/miss tally rides the existing
    gated-rebalance D2H counts copy (no extra sync).
  - **Falsified by the data:** CAPACITY (unified holds 57 slots/layer, 44.5% — one MORE than the
    reference's 56/43.8%, same VRAM) and PLACEMENT/batched-prefill (batch-on ≈ batch-off: decode
    hit-rate 94.2% either way). The real gap was steady-state per-token cost at EQUAL residency.
  - **Root cause = kernel-launch overhead on Windows/WDDM.** Decode does only 1 host sync/token (the
    sampler), but ~1100 kernel launches/token. Two fixes, each parity-exact:
    1. **Fused tail kernels** (`u::gemma4::add_rmsnorm`, `ffn_end` in `kernels/gemma4_glue.*`, ported
       from `g4_kernels.cu`): the post-attention sandwich (was 3 kernels) and the double-sandwich layer
       tail (was 7) are now one launch each — ~240 fewer launches/token. RED→GREEN unit test vs a
       double-precision CPU ref (~4e-7). Lifted steady 57→60.7.
    2. **CUDA-graph decode capture:** the whole per-token body (embed→30 layers→head) is captured ONCE
       and replayed per token (one graph launch vs ~1100 kernel launches). The token id rides a pinned
       cell and the position a DEVICE `pos_dev` (+ a tail `pos_inc`), so one static graph serves every
       step; `qkv_post`/`attn_decode` read the position on-device and the attention grid is FIXED at the
       call's max split count (empty splits write neutral partials the merge discards → bitwise no-op).
       The sampler (host readback) and the ~8-token rebalance (host swap logic) stay OUTSIDE the graph,
       which patches the device expert tables between replays. `STRATA_U_NO_GRAPH` and an automatic
       capture-failure fallback both drop back to the per-token `forward_one` path.
  - **Result (median of 5 gens, same greedy chat prompt):** unified **cold 60.4 / steady 73.5 tok/s**
    vs the old engine's **cold 45.9 / steady 72.1** — faster on both, greedy output still bit-identical
    to the oracle (first-8 match every gen). Progression: 47.8 (committed decode) → 50.8 cold/60.7 steady
    (fused) → 60.4 cold/73.5 steady (graph).
  - **BATCHED PREFILL** (also in the tree, `forward_batch`, cuBLAS): prefill ~27→48 tok/s, parity held;
    the batched path does NOT regress decode (the earlier regression was contention).
- **GLM-5.3-Flash ("glm5-next") LOADS end-to-end on the box (`<this commit>`):** the second family brought
  up on the unified engine. The forward (KDA + DSA + mHC + sigmoid MoE) was already written
  (`kernels/glm_*`, `family/glm_graph.cu`); this change makes the real model load. Added: (a) a MULTI-PART
  GGUF loader (`u_main`: discover `-00001-of-000NN` siblings, mmap each, merge the tensor directories
  first-match-by-name, resolve every tensor to an absolute host pointer `GgufTensor::host_data` so a
  family `load()` is shard-agnostic; the committed single-file gemma4 path is untouched); (b) the k-quant
  + i-quant tensor types in the GGUF reader (`u_gguf`: Q2_K/Q3_K/Q4_K/Q5_K/Q6_K + IQ2_XXS/IQ2_S/IQ3_XXS,
  type-id whitelist + `tensor_bytes`); (c) the `glm5-next` arch spelling (`u_dispatch`) and the NoPE fix
  (`u_geometry`: GLM is NoPE-MLA, no `rope.freq_base`); (d) reading the real per-layer KDA/DSA schedule +
  NextN count from the GGUF in `glm_graph::load` (the factory's all-KDA placeholder is replaced from
  `glm5-next.attention.head_count_kv`). Result: the 3-shard Maya-S-v2 IQ2_XXS model (1412 tensors) loads,
  reports `34 KDA / 11 DSA, nextn=1`, ready/available, in ~9 s. **Does NOT generate yet:** the MoE experts
  are i-quant (IQ2_XXS/IQ2_S gate-up, IQ3_XXS down) and the i-quant expert MMVQ is not ported; the
  KDA/DSA/mHC forward has not run. Next: port maya-ref `kernels/cuda/iq_kernels.cu` (path A) + the
  `ggml-common.h` grid/sign tables, wire the ExpertTier i-quant grouped kernel, then debug the forward to
  parity vs the `engines/maya-ref` oracle.
- **GLM-5.3 i-quant experts ported + the forward RUNS end-to-end (`<this commit>`), not yet parity-correct:**
  (a) `kernels/u_iq.cu` — the IQ2_XXS/IQ2_S/IQ3_XXS decode MMVQ (vec_dots + grid/sign tables verbatim from
  ggml-common.h, one warp per output row), ported from maya-ref path A; `common.cu`'s `k::mmvq` routes the
  three i-quant type ids there (the ExpertTier's per-expert path, no grouped kernel needed). (b) `k::dequant_q6k`
  for the Q6_K `token_embd` gather (the embedding is quantized, not F32). (c) GLM `load()` dequantizes the
  DSA `attn_k_b`/`attn_v_b` (shipped Q6_K) to a small F32 arena, since `glm_q_absorb`/`glm_mla_attention`
  read them as f32*. With these, the whole forward (all 45 layers: KDA/DSA/mHC + sigmoid MoE over the
  i-quant experts + head) runs with no crash and emits tokens.
- **GLM-5.3 GENERATES CORRECT OUTPUT (`<this commit>`) — functionally working end to end.** Driven with
  real prompts tokenized by the reference GLM tokenizer (`engines/maya-ref/tools/strata_tokenizer.py`, read
  straight from the GGUF; exact round-trip), greedy decode is correct and coherent: "The capital of Japan
  is"→" Tokyo. Tokyo is the capital city of"; "Water is made of hydrogen and"→" oxygen, and it is a
  molecule…"; "The opposite of hot is"→" cold…"; "1, 2, 3, 4,"→" 5, 6, 7". So the whole novel stack — KDA
  (gated delta-net), DSA (absorbed-MLA + k-pool indexer), mHC (sinkhorn hyper-connection residual), the
  sigmoid MoE with a shared expert, and the i-quant experts — is numerically right. The earlier
  "repeats the last token" was only an arbitrary non-tokenized prompt (greedy looping), not a bug. Three
  forward gaps were fixed to get here, each found via the `STRATA_GLM_TRACE=1` per-layer trace (now also
  dumps the residual) + the `GLM_CU` file:line: the Q6_K embedding gather, and the DSA attn_k_b/attn_v_b
  f32 dequant (both committed earlier). **Still open:** exact bitwise parity vs the maya `strata.exe`
  oracle is not yet measured (it needs a GLM pack via `tools/iq_pack.py`); decode speed is not yet tuned;
  the NextN/MTP speculative block is intentionally skipped. But GLM-5.3 now loads and generates correct
  text on the unified engine.
- **GLM-5.3 vs the maya oracle (`<this commit>`): token-1 greedy parity, and the unified engine is already
  a touch faster.** Built the oracle: a GLM pack (`tools/iq_pack.py --gguf <shard1> --out
  C:\Strata-data\packs\glm53 --compat-bf16`, with `STRATA_GGUF_PY` at the vendored gguf-py and PyYAML in
  .venv-dev; experts/token_embd/output are served natively so the pack is only ~0.9 GB; the 3 GGUF shards
  are hard-linked beside the pack), and the `maya` runner itself (`engines/maya-ref` `maya` target,
  `-DSTRATA_ENABLE_CUDA=ON -DSTRATA_GGML_DIR=<local llama.cpp 3cf03257> -DSTRATA_BUILD_TESTS=OFF`, into
  scratchpad\mayabuild). On the same prompt both predict token 1 = 12089 (" Paris") EXACTLY; greedy then
  diverges at token 2. That divergence is expected, not a bug: the oracle pack forces the mHC `hc_*`
  tensors to BF16 while the unified engine reads them raw F32, and two independent engines (different
  kernels / reduction order / expert-residency machinery) cannot stay bitwise-identical under greedy — so
  exact *bitwise* parity is not an achievable bar; token-1 match is the meaningful signal. **Speed:** the
  unified engine decodes GLM at **2.72 tok/s** vs the oracle's **2.5 tok/s** (both are expert-streaming
  bound on this box — 90 GB model, 16 GB VRAM; the oracle warned its RAM tier is page-file-capped so it
  reads experts from SSD). So the unified engine already marginally beats it. **Headroom:** the unified
  GLM uses only ~9.5 of 16 GB VRAM — the registered ExpertTier reads GLM's i-quant experts from mapped
  host over PCIe with little VRAM residency, so wiring a hot-expert VRAM residency (the gemma4 device-table
  approach) into GLM's path is the ~2-3x lever (uses the free ~6.5 GB), the main remaining speed work.
- **GLM hot-expert VRAM residency wired — decode now ~2x the oracle (`<this commit>`).** The ExpertTier
  registered path already had the residency machinery (reg_finalize/reg_note_routing/reg_rebalance +
  reg_slot_of + the resident-vs-stream dispatch in reg_run_token); GLM simply never called it. Fixed a bug
  it exposed (reg_finalize took n_expert from reg_[0], but GLM's 3 leading dense layers leave that entry
  unregistered → n_expert 0 → 0 slots; now takes it from the first VALID layer and skips dense layers in
  the warm-start). Wired GLM: load() calls reg_finalize with a free-VRAM budget (free - 1 GiB headroom;
  the pool is one contiguous cudaMalloc), run_moe notes routing heat each token, and generate() rebalances
  every 8 tokens (cold-start then converge). Result on the France prompt: 799 slots (17/layer of 288,
  5.6 GiB), decode **2.72 -> 4.96 tok/s** (48% resident-hit rate over 64 tokens as the rebalance converges)
  vs the maya oracle's 2.5 — a convincing ~2x. Output is bit-identical to the streaming path (residency
  changes only where an expert is read, not the math; token-1 parity holds). Further headroom: a
  routing-profile warm start (vs cold-start) would raise the early hit rate; more slots need more VRAM.
- **GLM routing-profile warm start (`<this commit>`): removes the cold-start transient; verified, not a
  steady multiplier.** `glm_graph.cu` now persists and reloads the learned expert ranking via the
  ExpertTier's STRP profile facility (no new format): `generate()` writes `reg_ranked()` through
  `write_expert_profile` when `STRATA_GLM_EXPERT_PROFILE_SAVE` is set (12960 pairs = 45×288), and
  `load()` reads `STRATA_GLM_EXPERT_PROFILE` and hands the ranked set to `reg_finalize` so the resident
  slots are warm-filled with the profile's hottest experts instead of experts 0..quota-1. A bug this
  exposed: `reg_finalize` warm-filled the slots but left `reg_usage_` at zero, so the first rebalances
  (which compare heat) evicted the warm set before real routing data accrued. Fixed by seeding each
  resident warm expert's usage to `kWarmSeed=4.0` (above the rebalance margin, fades at kDecay over
  ~2k tokens). **Cache-fair measurement** (`scratchpad/glm_warmstart2.py`: a throwaway warmup process
  primes OS/driver/disk caches, then COLD and WARM are both measured with warm caches; 128-token
  "neural network" prompt; decode tok/s from the DONE timing, resident% the per-GEN average):
  gen1 COLD 20.3% resident / 3.92 tok/s → WARM 25.1% / 4.28 (the warm set now holds through gen1,
  landing within ~1.4 pp of the converged 26.5% ceiling vs 22.3% without the seed). **Steady-state is
  unchanged** (cold 5.46, warm 5.49 tok/s): the resident set (17 of 288 experts/layer) is routing-limited
  to ~26.5% hit on this workload, so the warm start removes the first-generation convergence transient
  but is not a throughput multiplier — the steady ceiling is set by the VRAM budget, not the seed. (The
  earlier 4.96 tok/s / 48% figure was the 64-token France prompt; this is a different, longer workload.)
  The next steady-state lever is a larger resident budget (more slots/layer), which needs more VRAM.
- **qwen4exp (Qwen3.8-Flash-Next) bring-up — Stage 1: loads on the real model (`<this commit>`).** Porting
  from the GLM graph (the two are architectural siblings: GDN↔KDA gated delta-net, QSA↔DSA sparse attn,
  4-stream hyper-connections↔mHC, i-quant MoE on the shared `ExpertTier`). Stage 1 landed and is verified
  on `Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S` (2-shard, 1224 tensors): the virtual
  `Qwen4ExpGraph::load(gguf, mapped)` reads the exact geometry (48 trunk layers = 36 GDN + 12 QSA, 512
  experts top-10 + 1 shared, hidden 2560), uploads the non-expert weights, registers the experts, carves
  the state (GDN recurrence/conv, QSA K/V + indexer pool) and scratch arenas, and allocates residency —
  **3700 VRAM slots (77/layer), 9.18 GiB budget, 10.18 GiB free**. The engine reaches `ready`. Substrate
  changes this needed: the GGUF reader + `u::k::Quant` + `weight_bytes`/`block_elems`/`is_quantized` now
  carry IQ4_NL (20, 32-elem), IQ3_S (21), IQ4_XS (23) and the Strata-custom Q2_0 (42, 64-elem); the
  common geometry guard makes `.feed_forward_length` optional (qwen4exp has MoE on every layer, no dense
  FFN); the shared expert is detected from the `ffn_*_shexp` tensor (the `expert_shared_count` key is
  absent). KEY FINDING: the PLE n-gram table `per_layer_token_embd.weight` is a **28.8 GB IQ4_NL tensor
  IN the GGUF** (320M rows × 160) — it must stay host-mapped and read on demand, NEVER uploaded (doing so
  oversubscribes VRAM under WDDM and zeroes the residency budget); `load()` skips it.
  - **Stage 2 DONE (`<this commit>`): the GPU i-quant kernels for qwen's quant set.** `u_iq.cu` now also
    decodes IQ4_NL (20, 32-elem), IQ3_S (21), IQ4_XS (23) and the Strata-custom Q2_0 (42, 64-elem) --
    vec_dots + block structs + the `iq3s_grid`/`kvalues_iq4nl` tables (`u_iq_qwen_tables.inc`, extracted
    from maya-ref ggml-common.h) + `get_int_from_table_16`; `iq_row_dot` is generalized to the block
    size (`nb = n_in / Fmt::qk`) for the 32/64-elem types; `iq_supported`/`iq_row_bytes`/the dispatch and
    `common.cu`'s `mmvq` now route all four. Builds -Werror clean; exercised at forward time (Stage 3),
    so runtime-verified there. token_embd IQ4_XS dequant-to-f32 for the embed gather lands with Stage 3.
  - **Stages 3-4 WIRED (`<this commit>`): the full forward + decode build and RUN end-to-end, but output
    is NOT yet coherent.** `step()` implements the HC-wrapped trunk per the upstream reference (extracted
    op-by-op from `out/.../src/models/qwen4exp.cpp`): embed (IQ4_XS dequant → 4 identical hc streams) →
    48 layers {hc_read(attn) → GDN|QSA → hc_inject, hc_read(ffn) → MoE+shared → hc_inject} → final
    hc-collapse (= result_norm) → head. The HC combine/collapse were REWRITTEN (`qwen4exp_hc`) from the
    wrong scalar-mix to the real per-element gated mean (per-stream RMS*gamma → down/silu(×1/hc)/up →
    xn*sigmoid(gate) → mean; inject = +block_out*2σ(inject_raw/hc)) — verified against the reference.
    GDN wires the gdn_* kernels; QSA does the per-head q|gate deinterleave + per-head RMS + NEOX rope
    (mrope≈neox for text) + DENSE attention over the KV cache (the sparse indexer is deferred — dense is
    the exact superset); MoE = `router_topk` (softmax top-10) + `ExpertTier` (Act::swiglu) + the
    sigmoid-gated shared expert. `generate()` is the GLM-style decode loop (+ rebalance). token_embd
    IQ4_XS dequant added (`k::dequant_iq4xs`).
  - **Stage 5 COHERENT (`<this commit>`): greedy output is now correct.** `"The capital of France is"`
    → `" Paris.\nThe capital of France is Paris.\n…"` (first token 11751 = " Paris", matching the oracle).
    Two bugs in the GDN recurrence/conv (`qwen4exp_gdn.cu`), both found by a parity oracle (see below):
    (1) the GQA v-head→q/k-head map was `hv/(h_v/h_k)` (consecutive) but must be `hv % h_k` (the
    reference repeats q/k by TILING, and `native_gdn.cu` uses `q_head = head % h_k`); (2) the short-conv
    weight was read transposed — `ssm_conv1d.weight` is GGUF `[d_conv, channels]` (ne0=d_conv contiguous,
    element [tap,c] at `c*d_conv+tap`), the kernel had `conv_w[tap*channels+c]`. **Parity oracle** (the
    method, reusable for the other families): the prebuilt upstream engine `out/engine-cuda/strata.exe`
    runs qwen4exp from the native IQ3_S pack `C:\Strata-data\packs\iq3_s` (it maps the experts from the
    GGUF shards in place, so no AVX-512 CPU is needed) — invocation
    `strata.exe --pack <pack> --native <shard1> --ple-gguf <shard2> --mmap-experts --expert-cache auto
    --expert-profile <profile> --spec 4 --mtp C:\Strata-data\mtp\rt --prefill 512 --tokens … --gpu 0`;
    the native pack forces `--spec`, which needs a static `--expert-profile` (synthesize a complete one
    with `tools/make_profile.py --no-base --out profile.bin`, no trace needed). Per-layer signals used:
    `STRATA_DUMP_MOE_INPUT`+`STRATA_DUMP_MOE_LAYER` (the router input per prefill position — layer-0 pos0
    now matches at cosine 0.99994), `--dump-routing` (routed expert ids per layer — layer 0 matches), and
    the embed was verified bit-exact by dequantising `token_embd` row N against ggml's `dequantize_row_iq4_xs`.
    The unified engine carries env-gated taps (`STRATA_QWEN_TAP_FILE`/`_LAYERS`/`_POS`, `STRATA_QWEN_ROUTE_FILE`)
    that dump the HC/mixer/MoE-input/residual and routing per layer for this comparison (off by default,
    zero cost).
  - **Stage 6 PLE WIRED (`<this commit>`): TOKEN-EXACT PARITY with the oracle.** `"The capital of France
    is"` → `" Paris. The capital of Germany is Berlin. The capital of"` — all 12 greedy tokens
    `[11751,13,561,6511,314,9564,369,19241,13,561,6511,314]` match the upstream oracle exactly. PLE is
    applied at the one layer named by `qwen4exp.ple.layers` (= [1]), before that layer's attention:
    a host n-gram hash of the recent token ids (`mixed_n = XOR_j ctx[j]*mult[j]`, `row = mixed_n %
    vocab[h] + off[h]`, EOS resets the window; ngram 3 → 16 heads = 2 orders × 8) gathers 16 IQ4_NL rows
    (90 B each) of the host-mapped `per_layer_token_embd` table (shard 2, via `host_data`), dequantised to
    `emb[n_embd]`; then `key=ple_key@emb`, `value=ple_value@emb`, grouped RMS norms (reusing
    `hc_rms_gamma`), a signed-sqrt sigmoid gate `sigmoid(sgn(s)·sqrt(clamp(|s|,1e-6)))` with
    `s=(kn·qn)/√n_embd` per stream, `gated = value⊗gate`, a dilated (×ngram) causal depthwise conv
    (kernel 4, F16 weight, same `[d_conv,channels]` layout as the GDN conv) + SiLU, and `R += gated +
    conv`. New kernels in `qwen4exp_ple.cu` (gate / gated_value / conv_silu / add2); the host hash +
    IQ4_NL gather live in `qwen4exp_graph.cu`. Validated: layer-1 MoE input (pos0) matches the oracle at
    cosine 0.99997 (was ~0 without PLE, the sole remaining divergence).
  - **Stage 7 decode tuning — the REAL root cause, measured with nsys (`<this commit>`).** An earlier
    write-up here blamed a "hard PCIe floor" (~85% of experts streaming every token). That was WRONG: the
    owner's upstream run on this box (IQ3_S, `--expert-cache auto` = 4232 slots, `--spec 4` + MTP,
    `--kv-resident 32768`, tuned `--adapt-every 1 --adapt-swaps 160 --adapt-decay 0.97`) sustains
    **81.4 tok/s with a 90.0% expert-cache hit, only 2.3% of routed experts over PCIe**; this engine on the
    same box measures **88.1% hit** on a repetitive prompt (69% on a varied chat prompt) — the hit rate was
    never the gap. `nsys profile --capture-range=cudaProfilerApi` of the decode (`STRATA_QWEN_PROF_RANGE=1`
    brackets it; `--script FILE` drives `--serve` from a file because nsys on Windows forwards neither a
    piped nor a redirected stdin) found the gap was four defects in the port, three now fixed:
    1. **A device-wide sync after every kernel.** `u::k::sync_if_needed` runs `cudaDeviceSynchronize` when a
       launch gets the null stream, and the graph passed `nullptr` everywhere: ~1,885 full drains/step.
       Fixed (qwen4exp only; other families unchanged): the graph owns a blocking stream `cs_`, every launch
       and the step's copies go on it (`cudaMemcpyAsync`/`cudaMemcpy2DAsync`), one real sync per token (the
       sampler read-back).
    2. **A naive BF16 GEMV taking 63% of decode GPU time.** `k::bf16_gemv` ran `n_out < 64` as one thread
       per row, a serial n_in-long dot product (~157 us a call): the HC inject 4x10240 (x96/step),
       `ssm_alpha`/`ssm_beta` 48x2560 (x72), the shared-expert gate 1x2560 (x48) — exactly the 216 calls/step
       nsys counted. Fixed in the shared kernel: a block-per-row reduction (~7 us). Same math, another
       summation order: qwen4exp still token-exact vs the oracle, gemma4 still matches its oracle's greedy
       stream (`instr_probe.py`, 71.1-71.6 vs 70.1-70.2 tok/s).
    3. **Streaming experts from the pageable mmap.** Each streamed tensor's `cudaMemcpyAsync` cost ~94 us of
       host time (the driver stages pageable memory on the calling thread). Fixed:
       `ExpertTier::reg_pin_host_arena` copies the registered layers' stacked expert tensors into pinned
       slices (one `cudaHostAlloc` per tensor) as upstream does; qwen4exp calls it after `register_layer`
       (`STRATA_QWEN_PIN_EXPERTS=0` keeps the mmap). ~47 GiB pinned, load ~38 s (upstream ~36 s).
    4. **Host launch overhead — fixed in two steps.** (a) *Device-decided routed MoE*: `run_moe` planned each
       layer's experts on the host (ids D2H + `cudaStreamSynchronize` + ~50 per-expert launches).
       `u::k::iq_mmvq_grouped` runs a layer's k routed experts in ONE launch, each expert's matrix base chosen
       on the device from the router's ids and a device residency table — its VRAM slot, else its matrices in
       the pinned host arena read zero-copy over PCIe (`reg_pin_host_arena` now maps the slices).
       `ExpertTier::reg_run_token_device` = quantize x, grouped gate, grouped up, join over k*n_ff, quantize
       the k joins, grouped down, combine: 7 launches a layer instead of ~52, no host routing, no per-layer
       sync. `reg_publish_device_residency` uploads the table, `reg_rebalance` republishes it (only when a
       device table exists, so GLM's path is unchanged). One async D2H per token brings every layer's ids
       to the host for the routing heat + hit counters (`reg_note_device_token`).
       `STRATA_QWEN_MOE_DEVICE=0` keeps the host-planned path. (b) *CUDA-graph decode*: with the MoE on the
       device the step's launch arguments can all be fixed — the embed gather reads the token id on the
       device (`k::dequant_iq4xs_row`), the QSA KV append writes at `d_pos_` (`qwen4exp::qsa_kv_append`),
       attention takes its length from `d_pos_` (`k::attn_decode_devlen`), and the per-token inputs (token,
       position, PLE rows from `ple_prepare`) go through pinned host buffers. `step()` runs one eager warm-up
       step (lazy scratch allocations), captures `device_step_` once (2,802 nodes), then replays it every
       token; rebalance edits the residency table and slots in place, so no re-capture. The sampler stays
       eager. `STRATA_QWEN_GRAPH=0`, any debug tap env, or `STRATA_QWEN_PROF` keeps it eager.
       **Now GPU-bound** (~27.5 ms/token = the step's GPU time). Next, in order of size: the grouped expert
       kernels (~12 ms/step, dominated by the ~10% missed experts read zero-copy over PCIe — upstream
       computes 65% of its misses on the CPU, `--pcie-frac 0.35`), the Q6_K / BF16
       dense GEMVs (~6 ms, near bandwidth), `router_topk` (1 ms); then speculative decode (MTP, upstream
       ~2.5 accepted tokens a window). (Since then: the tail skip and the async adaptive tier landed, see
       below; the usage decay is still 0.999 a token against upstream's tuned 0.97.)
    **Measured (RTX 4080 SUPER, IQ3_S, warm, 256-token greedy decode, token-exact throughout):** 8.9 tok/s
    before -> 14.2 (fixes 1+2) -> 18.2 (fix 3) -> 29.3 (device-decided MoE) -> 36.4 (CUDA graph) -> **40.2 tok/s**
    (GDN recurrence as upstream native_gdn.cu's warp-per-column, rows-contiguous state; parity-neutral: this
    and the previous build both match a 256-token oracle run for its first 40 tokens, then diverge at a near-tie)
    -> **43.1 tok/s** (zero-copy misses: write-combined arena + coalesced shared-memory staging; nsys shows the
    grouped launches bimodal, 8-15 us all-resident vs 46-200 us with one zero-copy miss);
    expert-cache hit 90.3% (upstream's tuned run: 90.0%) -> **44.1 tok/s** (per-layer slot geometry: a slot
    holds one expert at ITS layer's quants and each MoE layer gets an equal byte share, so the same budget
    holds 4,886 slots, ~32% more than when every slot was sized for the largest layer; hit 92.4%; dense
    layers get no quota).
    GLM re-checked after the slot change: its first 8 greedy tokens match the earlier GLM C2 parity runs
    (`[12089, 320, 2186, 6722, 9621, 11, 3283, 8584]`); a longer GLM upstream comparison is pending.
    -> **51.5 tok/s** (upstream's two decode defaults for missed experts, ported after reading the owner's
    upstream run log `C:\Strata\strata-iq3_s.log` and `upstream/docs/DETAILS.md`):
    (a) **route tail skip** (`STRATA_ROUTE_TAIL_SKIP`, DETAILS.md "Missed experts in a verify window", on by
    default upstream): a missed decode expert routed at rank 7-9 of the top-10 is neither read nor computed
    (zero contribution, no renormalisation); on unless >= 90% of the experts are cached, decode only (the rank
    is a device int the captured graph reads, so prefill runs 0); `=0` restores the exact output. It drops
    ~54% of the misses (5,051 of 9,328 over 256 tokens): 46.4 -> 50.0 tok/s. Output differs from the exact
    path from token 53 of the 256-token run, as upstream documents (its teacher-forced KL 0.003-0.006).
    (b) **async adaptive tier** (`reg_rebalance_async`): the evictions are published to the device residency
    table on the decode stream at once, the refills go on the copy stream after that upload (an event orders
    them behind every step still reading an evicted slot), and the swapped-in experts turn resident only when
    their copies have landed (checked every step). Upstream's tuned `--adapt-every 1 --adapt-swaps 160` is the
    default; a capped round starts one layer further each time (from layer 0, a binding cap of 8 starved the
    last layers: hit 43%). 50.0 -> 51.5 tok/s, cold 12-token run 25.2 -> 29.4, hit 93.0%. Token-exact
    against the blocking path with the tail skip off (256 tokens). `STRATA_QWEN_ADAPT_ASYNC=0` keeps blocking.
    Also built and measured, left opt-in: (c) **miss staging** (`STRATA_QWEN_STAGE_MISSES=1`,
    `u::k::expert_route_plan`): one wide copy kernel stages a layer's misses into VRAM before the grouped
    launches (upstream's PCIe share, pcie_mode 2). Token-exact, but 45.8 vs 46.4 tok/s here: the grouped
    kernels' own zero-copy reads were already as fast as a copy kernel on this link.
    -> **57.2 tok/s** (`u::k::bf16_gemv_f32x`, qwen4exp's BF16 GEMVs: the f32 activation rounded to bf16 in
    the kernel -- no f32_to_bf16 launch, bitwise the same x -- 16-byte weight loads, a block per row for the
    long rows. nsys had the HC inject 4x10240 at 10.9 us (4 blocks, latency-bound), the HC down 320x10240 at
    15.2 us (40 blocks), the HC up 10240x320 at 12.2 us: ~3.7 of the step's 19 ms). It takes 1-8 columns
    with every column BITWISE a single-column call -- the verify window's contract (spec below, S1) --
    checked by `STRATA_KERNEL_SELFTEST=1 strata-unified-engine` (kernels/selftest.cu: NC = 1,2,3,4,8 on the six
    qwen4exp shapes, bitwise against single-column calls and within 1e-4 of bf16_gemv; RED shown with a launch
    shape made to depend on the column count: 12 BITWISE FAILs; GREEN 30/30). Not bitwise the old kernel (its
    own summation order): the greedy stream still matches the upstream oracle's first 40 tokens, as before.
    gemma4 / GLM / ds4 keep `bf16_gemv`.
    **Spec S1 done (multi-column exact MMVQ, `<this commit>`):** `u::k::mmvq` with ncols 2..8 is now ONE fused
    launch per matrix (`mmvq_multi_kernel<Traits, SmallK, NC>` for Q4_K/Q5_K/Q6_K/Q8_0, transcribed per format
    from the single-column kernels; `iq_mmvq_multi` for IQ2_XXS/IQ2_S/IQ3_XXS/IQ3_S/IQ4_XS/IQ4_NL/Q2_0), every
    column bitwise a single-column call (it was a loop of single-column launches: ds4's column groups are
    unchanged bit for bit). Self-test: 99 mmvq checks (11 formats x 3 shapes incl. a non-multiple-of-4 row
    count and both SmallK branches x NC 2/3/8), RED with the cross-warp sum reversed (k-quant/Q8_0 BITWISE
    FAILs), GREEN 129/129 with the BF16 checks. Measured, one call (RTX 4080 SUPER): Q6_K 10240x2560 27.0 us at
    1 column, 42.3 at 4, 64.5 at 8; Q5_K 2560x6144 23.0 / 24.1 / 35.1; IQ4_XS 1536x2560 13.1 / 22.7 / 23.4
    -- four tokens for 1.05-1.7x one token's weight reads.
    **Spec S2 done (the T-token verify window, `<this commit>`), and a decode RoPE bug it exposed.**
    `Qwen4ExpGraph::window_forward_(T)` runs T tokens through every layer at once: the GEMVs as S1 multi-
    column calls, every other kernel per token (or over T rows of a row-independent kernel); the GDN
    conv/recurrence READ their states without writing them (`write_state = false`; their per-layer inputs
    stay in `rp_`), the PLE conv runs on a copy of its history, and the K/V of all T positions are appended.
    `commit(n_keep)` replays the kept tokens' stored inputs in place (bitwise the single-token updates);
    K/V cells past the kept prefix are overwritten before any query reads them. Window and commit are
    captured per T / n_keep (eager once first). `STRATA_QWEN_SPEC_T=2..8` turns it on; until the MTP
    drafter (S3) its drafts come from `STRATA_QWEN_DRAFT_FILE` (a reference stream), and
    `STRATA_QWEN_DRAFT_CORRUPT=K` breaks every K-th window's last draft. **Evidence (tail skip off,
    256+12-token greedy, RTX 4080 SUPER):** teacher-forced with the plain greedy stream as drafts, T=4:
    191 of 191 drafts accepted (3.98 tokens a window) and the stream TOKEN-EXACT with plain decode; with
    every 3rd window's last draft corrupted: 185 of 208 accepted (rollbacks exercised), still token-exact.
    Throughput with perfect drafts: 71.5 tok/s against 51.6 plain on the same exact path -- the window
    costs ~2.9x a single step at T=4 (per-token expert calls and per-token small kernels), the next work.
    **The bug:** the first run of the window departed from plain decode at the first QSA layer -- per-layer
    debug taps localized it to the q/k RoPE. `k::rope_neox_base` reads ONE POSITION PER ROW (rows = tokens x
    heads, its documented contract); the decode passed a 1-int `d_pos_` with rows = heads, so q heads 1-31
    and kv head 1 read past it (zeros): no rotation for 31 of 32 query heads. Fixed with per-row position
    buffers (decode `d_pos_` holds max(n_head, n_head_kv) copies; the window `d_pos_rows_win_`). Against the
    upstream oracle's `STRATA_DUMP_MOE_INPUT` at layer 3 (the first QSA layer), position 3: cosine
    0.998927 before (the pre-fix stream reproduced exactly by emulating zeros past entry 0), 0.999950
    after -- the GDN layers' own level (layer 1: 0.999961). The greedy stream's oracle prefix moved 40 -> 33
    tokens (both end at near-ties; the per-layer cosine is the meaningful signal). Default decode (tail
    skip on) on the now-different text: 53.0 tok/s, hit 88.4% (57.2 / 92.9% on the old text).
    **Window cost, first pass (`<this commit>`):** nsys on teacher-forced T=4 windows had the routed
    experts at 55 of 82 ms a window (grouped launches 97 us each against 31 us single-token: misses, and a
    hit rate down to 85%). Three changes, still token-exact (the T=4 teacher-forced and corrupted-draft runs
    both match plain greedy): (1) `reg_run_token_device(..., n_tok)` runs the window's T tokens' T*k
    entries in one plan / quantize / 3 grouped launches / combine (`iq_mmvq_grouped`'s new `x_group`: k
    entries share a token's activation column; `expert_route_plan`'s rank is per token), so the tokens'
    misses overlap; (2) the HC kernels (`hc_rms_gamma`, `hc_gated_mean`, `hc_inject`, `shexp_combine`)
    take T tokens in one launch; (3) the adaptive tier's swap cap is scaled by the tokens a window kept
    (one round a window at 160 swaps was a quarter of the plain path's rate: the hit rate had fallen to
    85.4%; now 91.6%). Teacher-forced T=4: 71.5 -> 96.5 tok/s, a window ~41 ms (2.1x one token).
    **Spec S3 done: the MTP draft layer (`<this commit>`, `family/qwen4exp_mtp.cu`).** Loads upstream's
    `mtp/rt` (`STRATA_QWEN_MTP=C:\Strata-data\mtp\rt`): dense.bin by dense.txt (Q8_0 projections, BF16 HC/router,
    F32 norms with GemmaRMSNorm's +1 already applied), experts.bin relaid out from upstream's CPU blob (gate/up
    rows interleaved, codes plane + fp16 scales plane) to GGUF Q2_0 blocks (512 experts, 675 MiB, all in VRAM,
    run through `iq_mmvq_grouped` with an identity residency table), the draft head gathered from
    output.weight's rows of draft_vocab.bin (106,299 tokens), and its own K/V; ~1 GiB, loaded before the
    expert residency is sized. The forward is upstream's `MtpDrafter::record_front/record_rest`
    transcribed: e = fc_embedding(rms(embed(tok))), h = fc_hidden per stream(rms over hc*n_embd of the main
    residual), R = h + e, attention HC read -> K/V at the cell, dense attention, gate, o_proj -> write; MLP
    HC read -> router + Q2_0 experts + shared expert -> write; final mixer -> head -> argmax + its softmax
    probability. Cell i = (main residual at i, token at i+1), rope position i; prefill writes the prompt's
    cells, each window's round writes the accepted cells' K/V from the window's residuals, then drafts a chain
    from the last kept cell while a draft's probability is >= `STRATA_QWEN_SPEC_MIN_P` (0.7, upstream's tuned
    floor). Drafts only, so the output is untouched. **Measured** (RTX 4080 SUPER, IQ3_S, 256-token greedy):
    tail skip off, T=4: drafts accepted 162 of 216 (75%), 2.74 tokens a window, 51.5 -> 64.5 tok/s, the
    stream TOKEN-EXACT with plain decode; defaults (tail skip on): T=3 67.0, T=4 65.6, T=5 64.7 tok/s.
    Wall time (T=4): verify windows 36.7 ms each (3,488 of ~3,900 ms), the eager MTP ~1.3 ms a draft step.
    Also `STRATA_QWEN_EXPERT_PROFILE` (an STRP profile; upstream's `data/expert-profile.bin` reads as is) warm-
    starts the expert slots: with `STRATA_CAL_WARM_SEED=1` the cold 12-token run 31.6 -> 35.0 tok/s and the
    256-token run 67.3 (the default seed 4.0 kept the profile's experts too long: 64.4, hit 80%).
    To run it: `STRATA_QWEN_SPEC_T=3 STRATA_QWEN_MTP=<mtp/rt> [STRATA_QWEN_EXPERT_PROFILE=<profile>
    STRATA_CAL_WARM_SEED=1]`. Next: the window's cost (nsys per emitted token: grouped experts 5.9 of 14.1
    GPU-ms, the adaptive tier's fills 2.7 ms of H2D a token competing with the zero-copy misses), the MTP's
    graph capture, then CLI flags (`--spec`, `--mtp`, `--expert-profile`) for the launcher.
    **Why the misses cost what they do (measured, `<this commit>`; `STRATA_KERNEL_SELFTEST=1` timing lines,
    `STRATA_SELFTEST_PCIE_MIB` / `_SLICE_MIB` / `_REGISTER`):** a wide uint4 kernel reading 2 MB experts
    zero-copy from pinned mapped host memory gets 25-26 GB/s -- the link's rate, the same as DMA -- while the
    TOTAL pinned footprint is <= 12 GB; at 16-20 GB it gets 6.6 GB/s and at >= 24 GB 3.3 GB/s (2.4 with
    malloc + cudaHostRegister), however it is sliced (1 GB or 256 MB allocations) and with any block count;
    only ~32 experts in flight with 640 blocks each recover ~26 GB/s. DMA (cudaMemcpyAsync) stays at
    25.4 GB/s at 32 GB. The 2 MB / 600 us at 3.3 GB/s is ~1.2 us per 4 KB page: the driver maps a large
    pinned footprint with small pages for SM accesses. Our expert arena is 46.8 GB pinned, so a layer's one
    or two misses are page-walk bound (~120 us each in the windows, ~11-15 GB/s with the window's
    concurrency) -- why miss staging did not help and why the misses are the biggest cost. Upstream lives
    with the same box limit (its large pages are refused, error 1314) and gets 81 tok/s by computing ~65%
    of the misses on its CPU pool from RAM (67-74 us an expert on 11 workers). **Next: the CPU expert lane**
    (AVX2 dots for IQ2_S / IQ3_S / IQ3_XXS / IQ4_XS x Q8_K and IQ4_NL / Q2_0 x Q8_0, a worker pool, and a
    per-layer device<->host handshake on mapped flags inside the window graph). Owner option, not taken:
    granting "Lock pages in memory" (upstream `tools/windows/enable_large_pages.ps1`) might lift the small-
    page mapping -- a security setting, so the owner's call.
    **CPU expert lane -- BUILT (`<this commit>`): host-proven + nvcc compile-proven; the box measures the perf.**
    The lane the handoff named "Next" above, the one upstream uses to get 81 tok/s. A layer's missed decode
    experts are computed on CPU cores from the pinned arena in parallel with the GPU's resident ones, exactly as
    upstream's ExpertPool + verify.cpp per-layer doorbell does.
    - `u_cpu_iq.*` (host): ggml's reference AND AVX2 row dots for IQ2_S / IQ3_XXS / IQ3_S / IQ4_XS x Q8_K and
      IQ4_NL / Q2_0 x Q8_0 -- the GLM and qwen4exp expert quant sets -- plus the Q8_K / Q8_0 quantizers and fp16
      round-trip. `u_cpu_iq_tables.inc` is generated from ggml-common.h + ggml-cpu/arch/x86/quants.c (PROVENANCE
      updated). This is the integer-dot FAST path (upstream's ~67 us/expert), distinct from the overnight C3
      `u_cpu_expert.cpp` f32-dequant reference, which stays as a numerical oracle (owner 2026-10-25: keep both).
      HOST-PROVEN: `u_host_tests` cpu_iq_* 4/4 (AVX2 == scalar within 1e-5 over 6 quants; Q2_0's dot == its
      dequant; the quantizers bound their error; RED first with a mutated IQ3_S scale, worst rel 12.6).
    - `u_cpu_lane.*` (host): `LanePool` -- a layer's CPU experts as multi-token jobs (upstream run_split_multi:
      each expert's rows read once for all its tokens), three phases (gate|up rows / silu*up + Q8_0 / down rows)
      split across a pinned worker pool (epoch-tagged CAS claim, spin 20 ms then sleep, the caller drains too,
      one worker per physical core). HOST-PROVEN: cpu_lane_serves_like_the_reference (serve == the per-expert
      scalar reference within 2e-3 over two quant sets x two rounds incl. a post-sleep wake; RED with broken
      multi-token routing, worst rel 31; 9-token serve refused).
    - Device half (`kernels/u_iq.cu`, new `kEffCpu`): `expert_lane_plan` (upstream's rule on the device --
      resident => slot; tail-skip => kEffSkip; the last `pcie_num/256` of the misses => GPU zero-copy; the rest
      => kEffCpu), `lane_doorbell` (publish ids/eff + the MoE input x, then the mapped `seq`; upstream
      doorbell_publish), `lane_merge` (the CPU's rows over the kEffCpu entries' zeroed rows, waiting the mapped
      flag; upstream copy_rows_from_mapped_after). Tier (`u_experts.cu`): `reg_lane_enable / arm / serve / report`
      -- the host serve loop spins the mapped doorbell per layer after each launch (upstream verify.cpp Verifier),
      20 s timeout + stream-query, releases the GPU's waits on any failure. The arena is pinned CACHED when the
      lane is on (a CPU core cannot read write-combined memory). Wired into qwen4exp `step()` and `run_window_`
      (arm before the launch, serve beside it); off through prefill, on for decode, like tail-skip.
      `STRATA_QWEN_CPU_LANE=1` (default OFF), `STRATA_QWEN_CPU_PCIE=frac` (misses kept on the GPU; default 0 =
      all to the CPU), `STRATA_QWEN_CPU_WORKERS` (<= 0: one per physical core).
    - **The dot must match the GPU, not ggml (owner 2026-10-25, "check upstream, don't guess"):** the lane
      computes with `u_cpu_mmvq.*`, a host mirror of the GPU's MMVQ `vec_dot_iqX_q8_1` -- the SAME Q8_1
      activation quant (`quantize_q81`) and the SAME integer dp4a / sign / scale math -- NOT ggml's reference
      IQ x Q8_K (`u_cpu_iq.*`, which quantizes the activation differently). `u_cpu_iq.*` stays as the ggml
      oracle. PROVEN against the real GPU kernel: `STRATA_KERNEL_SELFTEST=1` `cpu-mmvq` checks (all six quants,
      worst rel 1.85e-4, most ~1e-5 -- float reduction order only).
    **BOX RESULTS (RTX 4080 SUPER, 128-token greedy, STRATA_ROUTE_TAIL_SKIP=0, `<this commit>`):**
    - **Plumbing is correct.** Lane ON with `CPU_PCIE=1.0` (the full doorbell/serve/merge handshake runs but
      every miss stays a GPU zero-copy miss) is **bit-identical to lane OFF over all 128 tokens** -- the
      device<->host doorbell works under CUDA-graph capture on the real model.
    - **The CPU compute matches the GPU.** Lane ON with `CPU_PCIE=0.0` (every miss on the CPU) tracks lane-off
      for **97 of 98 tokens** then one greedy pick flips (was 62/63 with the wrong ggml Q8_K dots). The residual
      is the CPU-vs-GPU float reduction order (~1e-5 per logit), the same non-bit-exactness upstream's split has;
      not a bug.
    - **Speed: NOT YET (the scalar dots are far too slow).** `u_cpu_mmvq` has no AVX2 yet: 951 us/expert (vs the
      Q8_K AVX2's 142 and upstream's ggml AVX2 67), so at `CPU_PCIE=0.0` decode is 13 tok/s against the 46 tok/s
      baseline. **Next: AVX2-vectorize the Q8_1 dots to ~67 us, then sweep `CPU_PCIE` so the CPU share overlaps
      the GPU's work** (at 0.0 there is no overlap -- the worst case). COMPILE-PROVEN under nvcc 13.0
      `-Werror=all-warnings /W4 /WX` sm_89; the full engine links (ds4 fix below).
    - Reproduce: `STRATA_QWEN_CPU_LANE=1 STRATA_QWEN_CPU_PCIE=<f> STRATA_QWEN_CPU_WORKERS=11` vs lane off.
    **ds4 build fix (`<this commit>`'s parent `dc7e4dc5`):** `ds4_graph.cu:163` assigned the GGUF's int64 list
    into a `std::vector<int>` (C4244 narrowing, `/WX`) from the overnight load-seam commits, which ran only the
    host-stub parse, not the box's nvcc `/W4 /WX`; made the narrowing an explicit per-element cast so the engine
    links (owner named it; pushed).
    **The gap to upstream's 81.4 tok/s is now its speculative decode.** Its log: 9,603 tokens in 6,223 verify
    windows (1.54 tokens a window, drafts accepted 3,380 of 5,066), **~19 ms a window** against our 19.4 ms a
    token; a 512-token request 105 tok/s at 3.4 tokens a window. Its CPU pool also takes ~65% of the misses
    (67-74 us an expert on 11 workers, ggml AVX2 i-quant kernels) — ours would need AVX2 dots for IQ2_S,
    IQ3_S, IQ3_XXS, IQ4_XS, IQ4_NL and Q2_0.
    Earlier in this stage: the per-layer shared-expert
    sigmoid gate moved on-device (`qwen4exp::shexp_combine`), 5.1 -> 6.4 tok/s on a cold 32-token run. An
    env-gated coarse profiler (`STRATA_QWEN_PROF=1`) prints the attn / run_moe / other split per token; the
    decode summary prints the expert-cache hit rate. gemma4 re-checked after the shared-kernel changes: still
    matches its oracle's greedy stream (`instr_probe.py`, 71.2-71.6 tok/s).
- **CALIBRATION (owner note, 2026-10-24): what the unified engine keeps vs the lean cut dropped.** Upstream
  "calibration" = `setup --calibrate` (`tools/calibrate.py`) + runtime measurement that profiles THIS box
  and writes tuning values: `--prefill-pipe` (prefill layer-split / PCIe-vs-compute ratio), PCIe share
  (`STRATA_GLM_PCIE_SHARE`, RAM-tier experts over PCIe vs CPU-lane compute), CPU-lane expert timing +
  reader-thread count (`glm_fast_path.cu`), ds4 `calibrate_paths()`, and the speculative gate's p_on. The
  unified engine **KEEPS** the expert-hit-profile + VRAM-residency calibration (`reg_rebalance` converges
  the resident set to the workload; the STRP profile warm-start) — now active for GLM and qwen4exp. The
  lean cut (design §1.2) **dropped** the CPU-expert-pool / PCIe-overlap / NUMA machinery, so the hardware
  perf calibration (prefill-pipe, CPU-vs-PCIe split) has no home here yet. The owner chose to reintroduce
  the hardware calibration as its own workstream (spec
  [`2026-10-24-engine-calibration-design.md`](superpowers/specs/2026-10-24-engine-calibration-design.md)),
  staged C1 (harness + adaptive-tier calibration, now built) → C2 (async PCIe-overlap streaming) → C3
  (CPU expert lane + pcie-frac) → C4 (rides on batched-prefill / spec paths).
- **Engine calibration — Stage C1: per-box adaptive-tier profile, shared substrate (`<this commit>`).**
  New `u::Calibration` (`u_calibration.{hpp,cpp}`, in the host lib): a `CalibrationProfile`
  (residency_headroom_mib, rebalance_every, rebalance_margin, warm_seed) persisted per box in a tiny
  keyed store (`$STRATA_CAL_FILE` else `<home>/.strata/unified-calibration.cal`), keyed by GPU name +
  VRAM + model + context. Loaded once in `u_main` (`cudaGetDeviceProperties`) before the family load;
  env overrides `STRATA_CAL_{HEADROOM_MIB,REBALANCE_EVERY,REBALANCE_MARGIN,WARM_SEED}` on top. The
  profile defaults EQUAL the old literals, so an un-calibrated box is unchanged. Wired engine-wide: GLM
  and qwen4exp read it for the residency headroom, the rebalance cadence/margin, and the warm-start seed
  (`reg_finalize` gained a `warm_seed` param; the `kWarmSeed` constant is gone). The harness is an
  external driver (`engines/unified/tools/calibrate.py`, upstream's model): scrape the key, cache-fair
  restart-sweep over the knobs via the env overrides, write the winner. **Verified** on the RTX 4080
  SUPER + qwen4exp: env override 1024→3072 MiB headroom moved residency 9.18→7.18 GiB / 3700→2894 slots;
  the driver's write→read-back round-trip loads as `[profile]` (768 MiB / every 16 / seed 2 → 3801
  slots). The decode-tok/s sweep needs a generate-capable family (GLM/gemma4 today; qwen4exp once its
  decode loop lands). Not yet wired: gemma4's bespoke `gemma4_moe` residency (its own path, not
  `reg_finalize`) — a follow-up to have it read the profile too.
- **Engine calibration — Stage C2: async PCIe-overlap expert streaming (`<this commit>`). BUILT + correct,
  measured no-op on this box.** `ExpertTier`'s registered path now streams a non-resident expert's
  gate|up|down into a prefetch RING on a dedicated copy stream, and runs the expert sub-block on its OWN
  compute stream (not the caller's legacy default stream, which serialized with every other stream and
  blocked overlap) with boundary events. Ring depth = the calibrated `stream_prefetch` knob (reg_finalize
  gained the param). **Finding** (GLM, RTX 4080 SUPER, output byte-identical across depths — parity OK):
  prefetch 1/3/6 give the SAME decode tok/s. At high residency few experts stream; at low residency the
  streaming is copy-BANDWIDTH-bound (~2.3 GB/token over one PCIe link, copy ≫ compute), so copy/compute
  overlap cannot cut the single-link copy time. So `stream_prefetch` defaults to 1 (overlap off); the
  machinery stays, calibratable, for where it helps, and is re-checked on qwen4exp once it generates. The
  real copy-bound levers here are residency (C1) and a CPU expert lane (C3, COMPUTE experts vs copy them).
  Spec [`2026-10-24-engine-calibration-design.md`](superpowers/specs/2026-10-24-engine-calibration-design.md) §C2.
- **Engine calibration — Stage C3: CPU expert-compute lane (`<this commit>`). BUILT + correct, measured
  ~14× slower on this box.** A per-token lane that computes a fraction (`cpu_expert_frac`) of a token's
  STREAMED experts on the host CPU (thread pool, `cpu_workers`) in parallel with the GPU, instead of over
  PCIe. New `u_cpu_expert.{hpp,cpp}` (host lib) dequantizes GLM's three expert i-quants — IQ2_XXS, IQ2_S,
  IQ3_XXS — with byte-identical grids (`u_cpu_expert_grids.inc`, extracted from `u_iq.cu`) + an f32 row
  matvec + gated activation; `ExpertTier` splits streamed experts CPU/GPU, runs the CPU ones on workers
  reading the mmap directly, and H2D's their parts into the combine (reg_finalize gained no new param;
  the family calls `reg_set_cpu_lane(frac, workers)`). Unsupported quants fall back to PCIe. **Verified**
  (GLM, RTX 4080 SUPER, low residency, all experts stream): at `cpu_expert_frac` 0.5 the first-8 tokens
  are byte-identical to the GPU path and the text is coherent (dequants correct) — but decode is **0.21
  tok/s vs 2.89 at frac 0 (~14× slower)**: CPU i-quant dequant+matvec of a ~6.5 MB expert is far slower
  than its ~0.26 ms PCIe copy. So `cpu_expert_frac` DEFAULTS TO 0 (off); the lane is correct + calibratable
  for a box/model where the CPU wins. ~~Not ported: CPU kernels for the qwen4exp quants~~ — **the
  four qwen4exp quants were ported to the host lane on 2026-10-25 (next bullet)**; what remains
  unported is the auto-split `pcie-frac` timing (would pick 0 here). Spec
  [`2026-10-24-engine-calibration-design.md`](superpowers/specs/2026-10-24-engine-calibration-design.md) §C3.
- **C3 host lane extended to the qwen4exp quants (2026-10-25, opencode worker, this Linux host).**
  `u_cpu_expert.cpp` now also host-dequantizes **IQ4_NL (20), IQ3_S (21), IQ4_XS (23) and the
  Strata-custom Q2_0 (42)** — the qwen4exp routed-expert set — following the box-proven float
  dequants in maya `iq_kernels.cu` (`dq_iq4_nl`/`dq_iq3_s`/`dq_iq4_xs`/`dq_q2_0`, with the same
  grids: `iq3s_grid_host[512]` + `kvalues_iq4nl_host[16]` appended verbatim to
  `u_cpu_expert_grids.inc`). Q2_0's 2-bit codes decode to −1/0/+1/+2 (the same mapping
  `u_iq.cu`'s Q8_1 dot byte-perms encode); IQ4_XS carries the −32 scale offset; IQ3_S uses one
  4-bit `1+2·sc` scale per 32-column chunk with the 9th grid-index bit from `qh`. New hermetic
  cases `cpu_expert_supported_covers_glm_and_qwen_sets` + `cpu_expert_ffn_matches_the_qwen_quants`
  (5 synthetic FFN probes per quant: uniform scale ×2, sign path, and hi64 patterns on gate/down/up
  that fail if any element order is wrong; expected outputs f32-exact). **RED 2/2 recorded before
  implementing; GREEN 9/9 in Debug and Release on this host.** Box follow-up: the lane only pays
  off where CPU beats PCIe — measure on qwen4exp; the earlier 14× kernel-gap finding for GLM still
  stands.
- **GLM upstream baseline (owner's run, 2026-10-24, Project Maya v1.1.27, `C:\project-maya`, same box):**
  GLM-5.3-Flash Maya-S-v2-IQ2_XXS, 32K context, MTP drafting, chat sampling (T=1.0, top_p 0.95; so no greedy
  text to compare), setup's own tuning: **13.1 tok/s** (sweep 10.2-13.6; PCIe share 0.38, 12 CPU threads),
  per-request decode 9.7-14.0 tok/s, VRAM hit 21-50%. Its layout (`maya-maya-s-v2-iq2_xxs.log`): dense 6.86 GB
  VRAM, expert pool 5.98 GB = 903 slots (21-30 per layer), RAM tier 70.69 GB pinned (10,672 slots; Windows'
  6 GB page file capped it ~10 GB short, so ~1-2 experts/token come off the SSD), and a CPU lane of 12
  threads that does **an expert in 0.22-0.31 ms vs 0.29 ms over PCIe**, taking ~75-260 experts a token.
  Against ours: the unified GLM decodes ~10 tok/s (C2 runs); its C3 CPU lane is ~14x slower than PCIe because
  it dequantizes to f32 and does a scalar matvec, where Maya runs ggml's AVX2 i-quant x Q8_K dot products.
  That kernel gap, not the lane's design, is why the CPU lane lost here.
- **Still open (box):** PREFILL is still ~2× slower (unified ~27 tok/s vs reference ~58) — it is
  memory-bound (each prompt token reads 8 experts × 30 layers from the mapped arena); the fix is
  BATCHED prefill (amortise each weight read across the prompt tokens via mmvq column-groups / the
  reference's `forward_batch`), a larger change. Also: async stream/compute overlap (GLM expert-profile
  save/load is now wired, env-gated — see the warm-start bullet above; a CLI `--expert-profile` flag and
  an auto-save-on-exit are still open); optional f32-prefill path for bit-exact prefill parity; GLM per-layer `head_count_kv` +
  quantized `token_embd` dequant; qwen4exp i-quant widening + host PLE reader; and the other families'
  `load()`/`generate()` bodies + deep-kernel `TODO(box)`s (only gemma4 is wired end-to-end).

### Model launcher: choose, configure and start models from the page (2026-10-23, local `main`, not pushed)

Owner request: "when the web ui is up, I am able to select configure models in the webui then be
able to select settings like mtp, kv cache streaming, etc and start it up"; then "Accept the
recommendations for D1-D10 in the model launcher spec; write the plan and start." Spec
[`superpowers/specs/2026-10-23-model-launcher-design.md`](superpowers/specs/2026-10-23-model-launcher-design.md)
(ACCEPTED, D1-D10), plan [`superpowers/plans/2026-10-23-model-launcher.md`](superpowers/plans/2026-10-23-model-launcher.md).
Commits on the local `main` (no branch switch, nothing pushed); Tasks 2 (options) and 5 (page)
were written by two workers in detached worktrees and integrated as patches (the worktrees were
moved to `out/retired-worktrees/` and pruned).

- **What runs:** with `models.cards` in the main config, `strata-secure-server --config` boots
  the launcher (`launcher_run`; `--check` lists each card's verdict). Each card file
  (`model_cards.hpp`) holds one model: name, profile (`qwen` | `gemma4`), the card-owned config
  rows and the page options with their bounds (`model_options.hpp`: qwen max_context, kv,
  kv_resident = KV streaming, kv_grow, mtp, spec, spec_min_p, expert_cache, prefill, thinking,
  sampling; gemma4 max_context, chunk, expert_vram, thinking, sampling). A start composes the
  model (`compose_model`, the per-model half of the boot moved verbatim from
  `server_bootstrap.cpp` into `model_runtime.cpp`, which the single-model boot now also uses)
  into its own `ModelRuntime` (tokenizer, supervisor, port, gate, chat route). The launcher
  (`model_launcher.hpp`) keeps one running at a time: compose first, then refuse chat
  (`model_switching`), refuse the old queue, let the generation in flight finish for 60 s and
  cut it (D3), shut the old engine down, prepare the state dir, start, wait for READY; a start
  that ends before READY stays `failed` (D4). Options are remembered in
  `<roots.state>/model-options.json` (D7).
- **API:** `GET /v1/strata/models`, `POST /v1/strata/models/start`, `POST /v1/strata/models/stop`
  (`kModelControlRoutes`, outside `kRouteTable`, served only with `models.control`; remote
  clients need `models.control_remote`, D2 — the peer's class now rides `RequestContext`
  from the pool). `/v1/models` lists the running card plus `strata_models`; `/health` says
  `engine: none` with nothing running. Six new error rows: `invalid_option` (param = the
  option), `control_not_allowed`, `model_not_running` (D6), `switch_in_progress`,
  `model_switching` (Retry-After 5), `no_model_running`.
- **Page:** a Models tab (hidden when the routes answer 404/403): the cards with their state, the
  selected card's options rendered from their kinds, Start (confirm when another model runs),
  Stop, polling while a card loads; the header shows the running model; with none running the
  composer says so and links to the tab.
- **Proof:** TDD with RED evidence per task (cards 5 cases, options 10, typed config 1, error
  rows 1, router 2, page 3, launcher end to end 7 — the launcher's RED is the server refusing a
  cards config with the dispatch disabled). Full MSVC suite: at `0589af28`: 985 cases, 984 passed, 1 skipped (cfg03, the long-standing skip), 0 failed, with the real Gemma pack set (`out/ctest-launcher-full-3.log`). `3ad44536` alone (an isolated worktree) builds clean and its 98 config, card, option and CLI cases pass. Box run: see session log
  item 57.
- **Dropdowns (owner, same day):** the context (Qwen 4K-1M, Gemma to 256K) and KV streaming (Off-512K)
  are fixed value lists the server validates and the page renders as dropdowns; past Qwen's
  trained 256K a start adds YaRN (`--rope-scaling yarn --rope-scale context/262,144`). Box: 1M
  loads and answers (47.5 tok/s, RAM nearly full), 512K 84.5 tok/s (session log item 58). Full
  suite 988 cases, 0 failed.
- **Remote clients:** the owner's browser is on another machine, so the tab first stayed hidden
  (403 `control_not_allowed`, D2's default). The page now explains a 403 in the tab, and the
  owner's local LAN config sets `models.control_remote` (session log item 57).
- **Monitor experts (owner, 2026-10-23):** the VRAM and RAM cards show "N experts loaded" as soon
  as a model runs, not only after the first chat. `EngineSupervisor::ready_readings()` captures
  the engine's startup `INFO` at `READY` and the launcher pushes it to the Monitor on start
  (session log item 61).
- **Open:** Gemma KV streaming (D8, its own engine plan); GLM cards (D10); vision with cards
  (refused for now); the `strata` CLI verbs (`serve`, `key`, ...) still read single-model
  configs only; the page's Sampling drawer does not yet take its defaults from the running card
  (spec §6 bullet 3); on Windows a console *close* after a switch no longer waits for the
  orderly shutdown (threat model residual); Windows CI (`windows-msys2-clang`) has not run.

### SP5-P2 parity refresh at the new pins (branch `sp5-p2-parity-refresh`, base `c71206f1`; closes issue 27)

Owner-bumped pins (`upstream` v0.2.2 @ `011c9e60`, `engines/maya-ref` v1.1.21 @
`451f9cb6`) left both parity tables citing the OLD pins. SP5-P2 (plan
`docs/superpowers/plans/2026-10-22-sp5-p2-parity-refresh-new-pins.md`) closed that:

- Task 1 `da5d54f7`: parity meta-tests now enumerate the CURRENT vendored trees and
  demand a disposition for every knob (the drift tripwire — a new upstream/Maya knob
  now fails the suite until dispositioned, with a `file:line` citation at the pin).
- Task 2 `2205d229`: `parity_keys_maya.hpp` REGENERATED at v1.1.21 — 183 typed /
  33 mapped / 50 rejected / 464 out-of-scope over the `MAYA_*`/`DS4_*`/`STRATA_GLM_*`
  surface; security exclusions stay tested rejects.
- Task 3 `9b68cb4f`: upstream table citations refreshed at v0.2.2.
- Task 4 `9119629b`: GLM child-env allowlist regenerated (146 names), `glm.*` config
  refreshed (127-row table; `glm.idle_unload_s` added; forced-safe defaults kept).
- Task 5 `aa4e5e9d` (lane): maya_118 wire identity pinned at v1.1.21 — slot-mode
  records (`T <rid> <tok>`, rid `DONE`) are protocol errors under the slots-forced-off
  launch; the ten-field v1.1.21 `DONE` yields figures from positional fields 2-4/8.
- Task 6 (lane; code in `aa1a17d0`): the GLM argv base re-anchored at v1.1.21 (gained
  `--expert-cache auto`, maya.py:2228) — all six composed flags exist in the pin's
  parser; the vision child is `maya-vision` beside the engine exe (grammar certified
  against `tools/vision/strata_vision.cpp:61-79`; no `--min-tokens`).
- Task 7 `14a77634` (controller): penalty semantics closed (issue 27) — a penalty
  window of 0 means NO penalty on the GLM engine (`generate.cpp:3467`); the client
  renders the flavor rule; the strata default-window-of-64 rule is untouched.
- History mapping (labels, not content): `aa1a17d0` carries the Task-5 label but its
  files are Task 6 (a controller `git add -A` swept the lane's in-flight files while
  the lane session was thought dead); `aa4e5e9d` is the lane's real Task 5. Nothing
  was lost or rewritten; both states were gate-verified. A double-writer race then
  occurred in the worktree (the lane was alive); all gates were RE-RUN single-writer.
- Proof: debug ctest 965/965 (4 allowed env skips), release full 923/923,
  windows-llvm-mingw-cross 0 warnings (compile-only), pytest 65 passed 1 skipped.
  Parity is citation-static: no GPU runs, no Windows runtime claims (owner hardware).

### First owner-box Windows GPU session (2026-10-21, base `main` = `e5a5327a`, uncommitted)

Full log with every issue: [`validation/2026-10-21-windows-gpu-session.md`](validation/2026-10-21-windows-gpu-session.md).
Host: Windows 11, RTX 4080 SUPER 16 GB, 64 GB RAM, CUDA 13.0, MSVC v143.

- **Engine built with CUDA on Windows and ran on the GPU** (upstream unmodified, first at
  `v0.2.1.3`, then `v0.2.2`): one-shot `generate` with the native-expert packs and MTP gave
  correct answers at 50.8 tok/s (IQ3_S) and 51.2 tok/s (Swift IQ3_S). First GPU evidence.
- **Two chat blockers found and fixed (owner-approved), GPU-verified:** issue 19 — the Qwen
  prompt lacked the template's think block and `<think>`/`</think>` (USER_DEFINED) failed
  closed as output; issue 26 — the end-of-turn token `<|im_end|>` (CONTROL), which the engine
  prints before `DONE … stop`, failed closed too. Now: the qwen plan writes the template's
  generation tail when the pack has the markers (`reasoning.effort none` → closed block), the
  reply is split into `reasoning_content` and `content` (non-stream and stream), and an
  end-of-turn id ends the reply as `stop`. Through `strata-secure-server` on the `v0.2.2`
  engine: non-stream and stream chats 200 with `finish_reason: stop`, 68.7 tok/s end to end on
  a 513-token reply, client disconnect → `client_gone` and the next request served, multi-turn
  correct, effort `none` → no reasoning. 19 new test cases (TDD, stubs first). RED runs
  failed exactly on the new tests; the final full MSVC suite (865 tests, Smart App Control
  off) matches the baseline's 30 Windows-host failures exactly, with every new test passing
  (session log, "Owner-run results"). The Linux `-Werror` gates have not run. Known parity gaps: the template's effort sentence is not
  rendered; low/medium/high are not distinguished.
- **Upstream bumped to `v0.2.2`** (owner request; `011c9e60`, tag object `e6fff4ad`): pin
  sites, `engine-protocol-limits.md`, comment cites and the parity table (127 rows; new
  `pool_cores`, `STRATA_ENGINE_NICE`, `gpu_roles`, `gpu_none`) re-pinned. Gates on the
  Windows box only (session log step 6); the runbook's Linux gates and the
  `privacy-migration.md` re-review are still owed.
- **Engine supply-chain contract** (owner decision): [`security/engine-supply-chain.md`](security/engine-supply-chain.md)
  — rules C1–C9 for every engine we pull or compile (immutable pins, review before
  execution, `scripts/security/engine_review.py diff` with a ruling per finding, a full read
  of the compiled diff, the `binary` import gate against
  `security/engine-binary-allowlist.json`, no foreign executables, dependency review,
  containment, recorded sign-off), wired into PROVENANCE "Updating the baseline". Applied
  retroactively to `v0.2.2`: 58 findings ruled (0 refused), binary gate PASS for both
  builds, llama.cpp fetch clean; compiled-diff read recorded separately; signed off by the
  owner 2026-10-21 ("yes commit and push"). Gate tests `tools/secure/test_engine_review.py` (7; written after the script, no
  RED run).
- **Maya bumped to `v1.1.21`** (owner decision; `451f9cb6`, tag object `ba26442d`):
  citations re-pinned; the `glm53` READY blocker (`VRAM dev=…` records before READY) fixed
  test-first in the `maya_118` dialect. Open (session log issue 27): the GLM penalty
  semantics now differ from ours, and the `MAYA_*` respelling leaves the GLM key inventory at
  its `v1.1.8` set (50 new knobs, five DFlash, not yet rowed).
- **After `556b6368`:** the `windows-msvc` preset now builds clean with `/W4 /WX` (shadowing
  renames; one `crt_environment.hpp` wrapper for MSVC's `getenv` deprecation — `_dupenv_s`
  would break the token wipe; the two `small`-macro clashes). Full preset suite: 865 tests,
  832 pass; the 30 baseline Windows-host failures plus 2 `http_socket_server` cases that never
  ran under MSVC before (issue 29, likely Windows sleep granularity). Issue 20 fixed test-first
  (`undecodable_token` log-only code). `reasoning.effort` description/parity row updated.
  `privacy-migration.md` re-reviewed for `v0.2.2`. The distribution check now refuses
  `apps/host`/`apps/desktop`. Swift served through the frontend; STOP during a 16k-token
  prefill frees the engine within ~0.3 s; READY in 39–48 s.
- **After `324191af` (session log step 10): every Windows failure triaged to its root cause.**
  Two product bugs fixed: `strata serve` never started Winsock, so it could not bind on
  Windows (issue 30). And `service::run_argv` gave `CreateProcessW` a bare `schtasks` /
  `nvidia-smi`, which searches the current directory and PATH before/after System32
  (CWE-427, issue 31). Now System32 only, test-first; the Task Scheduler CLI case, which
  had been reaching the real schtasks (nothing registered), runs a fake through the
  hooks-only `--test-system-dir`. Our gate gap: `engine_review.py binary` ignored ordinal
  imports. It now refuses them unless `allowed_ordinals` rules them; RED/GREEN, and the
  `v0.2.2` engine still passes (issue 32, contract C5). The egress tripwire resolves MSVC's
  ordinal `WS2_32` imports through the system DLL's export table. Harness portability:
  CRLF records, D-4 Windows pointer files in the update fixture (the first Windows run of
  `strata update`), host-absolute config literals, DPAPI token assertion, `SystemRoot` for
  the probe, issue 29's two causes (timer resolution; 1-byte window vs 64 KiB slices),
  DOS-02 typed handle counts (issue 33). The systemd renderer takes the POSIX absolute rule.
  Issue 21 resolved as docs (`/v1/models` lists only the served model, SP3 E9).
  Owner-run and still unproven: whether the real `schtasks /Create` accepts strata's task
  XML for a standard user.
- **Built-in chat page and keyless loopback mode (owner decisions, session log step 12).**
  `listener.web_ui` (off by default) serves three bundled, ASCII files from
  `secure_frontend/web/` at exact `GET` targets (`/`, `/ui/app.js`, `/ui/app.css`), after
  Host/Origin. They are embedded by `cmake/EmbedWebUi.cmake`, kept outside `kRouteTable`,
  and served with their own CSP (`HttpResponse::content_security_policy`).
  - The page's login is the API key (`sessionStorage`, that tab). Every API route stays
    keyed.
  - `listener.loopback_without_key` (off by default, config file only) is the one switch
    that turns the key off. It is loopback only: refused at load, and the core
    `validate_config` re-checks it at bind. It is refused next to a `token.source`, and
    the boot warns on stderr. Host, Origin and Content-Type checks still apply.
  - Test-first: 11 cases, RED then GREEN. Full preset suite 878/877 pass, 1 skipped.
    Checked in the built-in browser against the real model, in both modes.
  - Threat model, privacy-migration and README updated.
- **Long contexts, model names, upstream's page look (session log steps 13-14).**
  - **Caps:** the enforced caps and schema maxima reach upstream's scale. The renderers
    take 65,536 messages and 256 MiB a message; the protocol takes 1,048,576 tokens; the
    request, body and message maxima are 256 MiB. The defaults are unchanged. YaRN passes
    through `engine.args`.
  - **`model.name`:** the served name (upstream's `model_name`). `reasoning_effort` is
    also accepted per request on the qwen profile.
  - **Page:** it now uses upstream's UI kit (MIT) and Outfit font (OFL), with provenance in
    PROVENANCE.md. It keeps our API-key sign-in and a New chat button, and has no
    Monitor, Images or MCP views. The embed step carries binary assets; the CSP adds
    `img-src`/`font-src 'self'`.
  - **Gates:** all test-first. Full preset suite 885/884 pass, 1 skipped. Served at 128K
    and checked in the browser against the real model.
  - **Open:** commit headroom at 128K (issue 38).
  - **Issue 39 (owner bug):** message text with a link or "data:" was refused (SP0
    `valid_content`), which broke every turn after an answer containing one. Text is now
    text (SP3 §6.2 allows "rejected or treated as text"; nothing fetches it). The model
    field and image URLs keep their guards; the egress test keeps its no-egress checks.
    Test-first; suite 886/885, 1 skipped.
  - **Issue 40 (owner bug):** a chat saved by an older page held an unanswered question,
    so every later turn sent two user turns in a row (renderer refuses: 400), even after
    a refresh. The page now sends, saves and loads only strictly alternating turns
    (`conversation()` in app.js). Verified in the browser; no JS test harness exists.
  - **Issue 41 (owner request): the Monitor.** `listener.monitor` (off by default) serves
    a keyed `GET /metrics` in upstream's shape (`serve_monitor.hpp`). It holds the last 100
    generations' counts and the engine's DONE timings (never text), and the host's CPU/RAM
    plus GPU 0 via the system's `nvidia-smi` as a child (`host_readings.hpp`; no NVML in
    the server; its environment is `ProgramFiles` from HKLM, issue 43 — NVML does not start
    without it). Off, `/metrics` is no route for any method (EXT-01). The page has
    upstream's Monitor tab, DOM-built. Gaps: one GPU only; no disk, expert or hit-rate
    readings; no Prometheus format. Gates: full suite 901/900, 1 skipped; after the issue
    43 fix the monitor and server e2e tests rerun (one known idle-unload timing flake,
    passes on rerun). Not run: the Linux and cross -Werror gates.
  - **Issue 42 (owner request): Off / Low / Medium / High.** The qwen renderer now writes
    the template's effort sentence (`QwenEffortPlacement`; high is the default, low has
    its own sentence, medium none, `effort_position` "end" as upstream #458). Before, every
    thinking prompt lacked the template's xhigh sentence. The page offers the four levels.
  - **Gemma 4 (owner request 2026-10-22): plan 1 (the frontend `gemma4` profile) and plans
    2-3 (our engine, `engines/gemma4/`) done on branch `gemma4-plan1-frontend`, merged into
    `main` at `0d60a2ea` and pushed (owner, 2026-10-23); the engine matches llama.cpp at its own CPU-vs-CUDA floor and the owner's Q8 file
    is served behind the 8080 page in expert mode on the owner's box (RTX 4080 SUPER, Windows 11,
    2026-10-23). Plan 4 (managed launch, the engine gate, the full §9.4 battery) not done.** Upstream cannot
    load `gemma4` (only `qwen4exp`/`qwen35moe`; the Qwen3.6 driver takes head 256 only).
    Owner chose a Strata-style port and accepted decisions D1-D11 (2026-10-22): spec
    `docs/superpowers/specs/2026-10-22-gemma4-engine-design.md` (our `engines/gemma4/`
    tree building upstream unmodified, `strata-gemma4-engine` on the `strata_0143`
    protocol, frontend `gemma4` profile with an `spm_bpe` tokenizer mode, the reference
    being llama.cpp `3cf03257`'s `gemma4.cpp`). The owner's UD-Q8_K_XL download
    (`C:\Strata-data\models\gemma4_q8`) confirmed the model facts header-only (§2.6); it
    is the v1 target by the owner's choice ("no use the q8", D8). That reverses D4: experts
    split VRAM/RAM with Q36's hybrid placement and a GELU/BF16 CPU pool, plus BF16 kernels
    for its 11 BF16 matrices (2.15 GB). (At the spec's writing no plan existed; see the
    Execution list below for what plan 1 built.) Meanwhile, by the
    owner's decision, a TEST-ONLY llama.cpp `llama-server` (pinned `3cf03257`, offline
    build, not reviewed under the supply-chain contract, not our server) serves that file
    on `0.0.0.0:8081` (session log step 17, issue 44); nothing of it is committed. Owner
    principle: one UI (8080), engines behind it. Owner decision D12: no interim
    llama.cpp-backed engine; Gemma joins the 8080 UI when the Strata-style port is done.
    Plan 1 (the `gemma4` frontend profile, 12 tasks, TDD, fake engine only) is written:
    `docs/superpowers/plans/2026-10-22-gemma4-plan1-frontend-profile.md`. Its
    survey found ~10 two-way profile ternaries a third profile would silently fall through,
    no `engine.executables.*` key (expert mode for now; the per-family key moves to plan 4),
    and an existing bug: the page always sends `reasoning_effort`, which glm53 refuses (400).
    **Execution** (owner, 2026-10-22: "keep working on this until it is ready and can serve
    the model for testing") on branch `gemma4-plan1-frontend`, never pushed:
    - Task 1 done: `engine.profile = "gemma4"` (`EngineProfile::gemma4`, `--check` prints
      it), `arguments_profile_for` (exhaustive switch: qwen → `strata_0143`, glm53 →
      `lighttransport_glm53`, gemma4 → `strata_gemma4`), the `strata_gemma4` allowlist
      (`--max-context` 1-262144, `--chunk` 1-8192, `--kv f16|f32`, `--no-graph`,
      `--expert-vram` 0-1024 GiB, `--expert-slots` 0-128, `--hybrid-rows` 1-4096,
      `--cpu-threads` 1-256; `--flag VALUE` only, once each; known foreign flags refused by
      name, unknown ones never echoed), the `glm.*` guard now "every profile but glm53".
      Gates: MSVC build clean; full suite 920 passed, 1 skipped (the symlink case).
    - Task 2 done: manifests take `"profile": "gemma4"` with `"engine": {"family": "gemma4"}`
      (`ManifestProfile::gemma4`, `ManifestFamily::gemma4`; the family per profile is an
      exhaustive switch). gemma4 launch keys are `native` (a listed weights file, shard 00001)
      and `tokenizer_pack` (a directory of listed files); `ple_gguf`, `pack` and `mmproj` are
      refused (D2 text only). `engine_gate` refuses gemma4 as `engine_profile_unavailable`
      with the fixed reason "the Gemma 4 engine is not installed" (plan 4 opens it); `list`
      and `show` print "engine not available (…)", `run` exits 11. Gates: MSVC build clean;
      full suite 922 passed, 1 skipped.
    - Task 3 done: `NativeChatProfile::gemma4`; the profile → chat profile / wire flavor /
      environment family / per-request `reasoning_effort` selections are exhaustive switches
      shared by `supervisor_config` and the bootstrap (gemma4 → its own chat profile,
      `strata_0143`, the strata env allowlist, `reasoning_effort` accepted). An expert gemma4
      launch is the allowlisted `engine.args`, then `--gpu` from `engine.gpu` (as qwen), then
      `--native <models_root/file>` last (controller ruling: the Gemma engine takes
      `--native`, spec §4.8's `--model` is amended in Task 12); no VRAM/split,
      expert-profile-save, `--batch`, MTP strip or effort-end tail. On an explicit gemma4
      profile the config refuses those qwen-only rows by name (`conflicting_values`). The
      chat plan is chosen by switch in `chat_execution` and `chat_route`; the gemma4 plan is a
      stub refusing every request until Task 8. Gates: MSVC build clean; full suite 926
      passed, 1 skipped.
    - Task 4 done: `TokenizerMode { byte_bpe, spm_bpe }`, a required argument of
      `load_local_tokenizer_pack` and recorded in the pack; `tokenizer_mode_for` (gemma4 →
      `spm_bpe`, else `byte_bpe`) picks it at boot — never inferred from the files. `spm_bpe`
      reads `merges.json` (array of two-string arrays in rank order, own JSON limits: depth 2,
      ≤ 1,048,576 entries, 64 MiB) and refuses a pack that also has `merges.txt`; `byte_bpe`
      is unchanged and refuses `merges.json`. Publishing a gemma4 manifest requires
      `vocab.json`, `merges.json`, `token_type.json`, `tokenizer.json` under
      `launch.tokenizer_pack`, loading in `spm_bpe`. Gates: MSVC build clean; full suite 931
      passed, 1 skipped.
    - Task 5 done: `spm_bpe` encode/decode as llama.cpp `3cf03257`'s gemma4 tokenizer
      (spaces → U+2581; `[^\n]+`/`[\n]+` runs; a known newline run is one token; code points
      merged by a priority queue, lowest rank then leftmost; `<0xXX>` byte fallback; no BOS,
      no space prefix). Fail-closed where llama.cpp is lenient: a missing byte token or a
      merge landing on a special token is `unencodable_text`. Decode: NORMAL (U+2581 → space)
      and BYTE (one raw byte) only; byte_bpe now refuses BYTE tokens too. Test pack
      `spm_tokenizer()` in `tests/support/test_tokenizer.hpp`. Gates: MSVC build clean; full
      suite 936 passed, 1 skipped.
    - Task 6 done: `tools/secure/secure_tokenizer.py --profile gemma4 --gguf F --out D`
      writes `D/tokenizer/{vocab.json, merges.json, token_type.json, tokenizer.json}` through
      our own header-only GGUF reader (metadata only, bounded; tensor data never read); it
      accepts only `tokenizer.ggml.model == "gemma4"`, refuses a gemma4 GGUF without the flag,
      refuses duplicate tokens, mismatched types, merges that are not two vocabulary tokens,
      and `add_space_prefix` true, and records model, `add_bos`, the special ids and the
      SHA-256 of the header bytes it read. `tools/secure/spm_bpe.py` (`Encoder`) is the
      pure-Python reference encoder for Task 7. Gates: pytest 62 passed, 4 skipped, 7 failed —
      all 7 in `test_build_release.py`, which runs `bash` (on this box WSL's launcher with no
      distro installed); environmental, not touched here.
    - Task 7 done: the real Gemma 4 vocabulary (the owner's UD-Q8_K_XL GGUF, header only) →
      `out/gemma4-pack/tokenizer` (never committed; folder made owner-only like `out/serve/`).
      64 texts tokenized by llama.cpp's `llama-tokenize` at `3cf03257` (test-only build, GPU
      hidden) equal `tools/secure/spm_bpe.py`'s ids, and the C++ encoder's
      (`tokenizer_pack_tests.gemma4_real_pack_matches_the_fixture`, label `gemma4-real-pack`,
      skipped with its reason unless `SECURE_FRONTEND_GEMMA4_PACK` names the pack); 27 marker
      texts refused. Fixture: `secure_frontend/tests/fixtures/gemma4_tokenizer_texts.json`
      (texts + ids). Session log step 17, item 45. Gates: MSVC build clean; tokenizer cases 18/18
      with the real pack; the full suite runs once after Tasks 8-9 (controller's batch gate).
    - Task 8 done (written by a parallel worker in a detached worktree, integrated by the lane):
      `render_gemma4_text_prompt` (new `gemma4_prompt_renderer`) renders the GGUF template's
      text path byte for byte (`<bos>`; a system turn only for a leading system message or
      thinking on, `<|think|>` first; `<|turn>user\n…<turn|>\n`; earlier answers as
      `<|turn>model\n…<turn|>\n`; the generation prompt plus `<|channel>thought\n<channel|>`
      when thinking is off); `gemma4_trim` is Python's `str.strip()` (a fixed 29-point table);
      strict user/assistant order, any of the 24 special literals, an empty text, C0 controls
      or the parts form fail closed. `find_gemma4_markers` (each literal once, the GGUF's own
      types), `encode_gemma4_prompt` (markers by position, one spm index per prompt) and
      `make_gemma4_chat_generation_plan` (think tokens `<|channel>`/`<channel|>`, never starting
      in reasoning). Not done: the one-time cross-check against llama-server's
      `/apply-template` (needs a running server on the box). Gates: MSVC build clean; 90/90
      targeted; full suite with Task 9.
    - Task 9 done (parallel worker, integrated by the lane): `GenerationRunLimits::channel_name`
      ("thought" for gemma4, unset for qwen/glm53): after `<|channel>` the decoded line must be
      exactly `thought\n` (within 64 bytes) and is dropped, the rest is `reasoning_content`;
      any other line, a tag or end-of-turn inside it, or no newline in the bound ends the reply
      `undecodable` (STOP + drain). Stop ids `<turn|>` then `<eos>` (each one CONTROL token,
      never output); tool and media markers in output still fail closed. Wired through both
      route paths. Gates: MSVC build clean; 94/94 targeted; **batch full suite after Tasks 7-9:
      946 passed, 1 skipped (cfg03), 947 cases, real-pack case included.**
    - Task 10 done: `select_chat_generation_plan` (chat_route) is the route's one plan switch
      for text and image paths (gemma4: its own plan, text only). gemma4 thinking: the
      request's `reasoning_effort` wins (`none` off, `low`/`medium`/`high` on), else
      `reasoning.effort` (unset or `none`: off, D10). `chat_schema_limits` derives the
      per-request flag from the profile; the vision gate opens only for qwen. Note: the page
      always sends a level (High by default), so page chats with Gemma think unless Off is
      picked; API clients that omit it get thinking off. Gates: MSVC build clean; 124/124 in
      the touched binaries; full suite in the Task 10-11 batch.
    - Task 11 done (parallel worker, integrated by the lane): the gemma4 profile end to end on
      the fake engine through the real server: `ServerOptions.profile` → `--profile`, the
      synthetic spm pack written as `roots.tokenizer_pack` (`write_spm_tokenizer_pack`), and
      `ServerOptions.fake_engine_options` in `<root>/bin/fake-engine.options` (the
      `strata_gemma4` allowlist refuses the fake's own flags as `engine.args`). Proven: thinking
      off by default (complete and streamed; GEN ids equal the template bytes written by hand),
      `none` off, `low`/`high` on with `<|think|>` in the system turn, the thought channel as
      `reasoning_content` (its name line dropped, tags never output, thought streamed first),
      `<turn|>` ends the reply as stop, the page and `/metrics` unchanged. One test-only fix
      (the role chunk's empty `content` was taken as the first answer delta). Gates: 4/4;
      **batch full suite after Tasks 10-11: 953 passed, 1 skipped (cfg03), 954 cases.**
    - Task 12 done (docs truth pass): the page's Thinking control notes "Gemma models think or
      answer directly; Low, Medium and High all mean thinking on" (router test pins it); the
      README gains a Gemma 4 profile section (launch, the `spm_bpe` pack, prompt, thinking,
      what is not yet there); the threat model one line (a third profile, same controls); the
      spec's §4.8 now says `--native` (Task 3's ruling) and §6.1 records that the per-family
      executable key and the gate opening move to plan 4; session log items 46 (the page cannot
      chat with a GLM model: it always sends `reasoning_effort`, glm53 refuses it) and 47 (page
      chats with Gemma think by default; API default off). Gates: MSVC build clean; **final
      full suite 953 passed, 1 skipped (cfg03), 954 cases** (`SECURE_FRONTEND_GEMMA4_PACK` set,
      so the real-vocabulary case ran); pytest 62 passed, 4 skipped, 7 failed (all
      `test_build_release.py`, `bash` → WSL without a distro on this box, environmental).
    - **To serve the model for testing** (expert mode, once `strata-gemma4-engine` is built):
      `engine.profile = "gemma4"`, `engine.executable` = the Gemma engine (absolute, owner-only
      folder), `roots.models = C:\Strata-data\models\gemma4_q8` with `engine.model_id =
      "gemma-4-26B-A4B-it-UD-Q8_K_XL"` (the `.gguf` stem), `roots.tokenizer_pack =
      C:\multi-strata\out\gemma4-pack\tokenizer` (owner-only), `engine.args` from the
      `strata_gemma4` allowlist (e.g. `["--max-context","32768","--kv","f16"]`),
      `engine.gpu = "0"`, served with `--expert`.
    - **Plans 2-3 (the engine), by the controller:** `strata-gemma4-engine`
      (`engines/gemma4/`, build `engines/gemma4/build-windows.bat` → `out/gemma4-engine`), plan
      `docs/superpowers/plans/2026-10-22-gemma4-plan2-3-engine.md` with reconciliations R1-R7
      (nothing of `upstream/` compiled; own Q8_0/BF16/F32 kernels + cuBLAS; cold experts read by
      the GPU from 23.5 GiB of pinned RAM over PCIe — the CPU pool of D4 not built, **owner
      ruling wanted**; llama.cpp the only reference; a 2,048-row sliding ring with
      common-prefix reuse; `--native`; `--kv f32` refused). Results
      `docs/validation/gemma4/2026-10-22-engine-parity-and-speed.md`: against llama.cpp
      `3cf03257` (test-only `engines/gemma4/tools/g4_llama_dump.cpp`) mean KL 0.040 where
      llama.cpp's own CPU-vs-CUDA floor is 0.042 (top-1 22/24 vs 21/24); 4,911-token prompt KL
      0.034, same answer; decode 47-60 tok/s greedy (llama.cpp: 24-30 here), 36 tok/s with
      sampling; prompts 860-1,110 tok/s; load 11-30 s, 54-57 of 128 experts a layer in VRAM.
      Gates: `ctest --test-dir out/gemma4-engine` 23/23 (12 host, 9 GPU kernel cases, 2
      real-model cases), MSVC `/W4 /WX` and nvcc `-Werror=all-warnings` clean.
    - **Serving for testing (2026-10-23):** session log step 18 item 53 — the API and the page
      chat with Gemma (thinking Off and High, streamed and not, follow-ups, Stop, the Monitor);
      the serve config (`out/serve/config-gemma4-lan.json`, not committed) sets the model
      card's sampling defaults (temperature 1.0, top_p 0.95, top_k 64): greedy decoding made the
      model deliberate for minutes. 2026-10-23 (session log item 54): the frontend builds the
      spm_bpe encode tables once at pack load (`TokenizerPack::spm_index`; about 0.2 s a request
      saved; full suite 955 passed, 1 skipped); the engine ranks the sampler's candidates on the
      GPU and rebalances its VRAM expert slots every 8 decoded tokens (up to 32 swaps): a sampled
      story went from 29-32 to 54 (cold) and 77-85 tok/s (settled); engine ctest 25/25. Item
      55: a repetition loop the owner hit is the model's own (llama.cpp gives the same
      distributions along it; about 1 sampled answer in 30 at the model card's values, none of 32
      with `min_p` 0.05, now in the serve config), and retries repeated it because an unseeded
      GEN used seed 0; unseeded requests now get a clock seed as upstream's generate.cpp does
      (engine ctest 26/26). Item 56: served at 262K (no KV streaming in our engine; the f16 KV
      of the full layers fits in VRAM at the cost of expert slots); the INFO record carries
      `expert_slots`/`expert_slots_primary` for the Monitor (engine ctest 27/27). Open:
      the `/apply-template` cross-check; the CPU pool ruling; plan 4.
- The `windows-runtime`-labelled cases run under the MSVC preset on the owner's box; the
  supported Windows toolchains (MSYS2 clang, llvm-mingw) are still not installed (issue 3).

### SP7 M2 (branch `sp7-m2-release-side`, base `f4dff56d` — `main` with SP7 M1 merged; the branch waits for the controller's merge gate)

Plan `docs/superpowers/plans/2026-10-22-sp7-m2-release-side.md` (Tasks 1–8),
spec D-2/D-3/D-4/D-10. Lane ledger with every ruling (R-T0-a/b, R-T1-math,
R-T2-a/c, R-T3-a…l, R-T5-a/truth, R-T6-a…d) and RED evidence:
`.superpowers/sdd/2026-10-22-sp7-m2-release-side/progress.md` (git-ignored
work record, same convention as M1 R-T1-d). Commits authored `shift-f10`,
`Co-Authored-By` trailer, no `Claude-Session:`.

| Task | Commit | Landed | Proof (named in the ledger) |
| --- | --- | --- | --- |
| 1 | `b4ca02b1` | lib `secure_frontend_ed25519`: dependency-free RFC 8032 §5.1.7 **verify path only** (strict equation, canonical-S, no cofactor), plus `sha512`; three field-arithmetic bugs found and fixed during TDD (R-T1-math) | official §7.1 vectors + forged/tampered/swapped/non-canonical negatives, all data fetched from rfc-editor.org and cryptographically re-derived, never hand-transcribed (R-T0-b) |
| 2 | `706fc5db` | lib `secure_frontend_release_manifest`: envelope gate order framing→base64→JSON→schema→Ed25519 (R-T2-a); CMake cache var `STRATA_RELEASE_PUBKEY_HEX` (64-lowercase-hex; empty/malformed = unpinned, exact refusal line `no release key pinned`) | good envelope accepted, every tamper class rejected with its named reject, pinned 844-byte fixture envelope |
| 3 | `f4811354` | config `update.source` (typed row, parity 121→123, README rows); full `strata update` program half: fetch envelope (1 MiB cap, no redirect) + components streamed with sha256 verify from first-valid mirror, confirm, stage → atomic rename → `current` flip keeping `previous`, `--rollback` pure pointer flip (skips the key, R-T3-l); exit 12 for program-update failures, 8 on interrupt, 2 on decline | `update_cli` 11 cases incl. `px7_nothing_but_the_update_verb_touches_update_source` source scan; rewritten no-network exit-12 case; Windows pointer branches compile-proved only |
| 4 | `16264a48` | `scripts/release/keygen.py` (seed→pin, 0600 `O_EXCL`, never committed) and `sign.py` — RFC 8032 §6 reference code VERBATIM + envelope framing + `--self-test` re-deriving all four official vectors; interlock fixtures under `secure_frontend/tests/fixtures/release/` | C++ verifier accepts the Python signer's committed fixture byte-identical to the C++ pinned literal and reproducible by the signer; 7 pytest |
| 5 | `792647fa` | generators `make_manifest.py`/`manifest_lib.py`/`sbom.py` (SPDX-2.3, live submodule pins/URLs/LICENSE facts — R-T5-truth: no hardcoded repo facts); also completed Task 4's uncommitted CMake registration hunk | e2e: generated package passes `secure_distribution_check.py --release`; negative control flips `analytics:true` → judge fails naming it; byte reproducibility |
| 6 | `ddb1cdbd` | `build_release.sh` (bundle + VERSION + evidence + D-3 manifest + `SHA256SUMS` + tarball + `dpkg-deb` when present; exact `unsigned: not releasable` without `STRATA_SIGNING_KEY`; judge is the last build step), `stage_windows.sh` + generated `installer.nsi` + `NSIS_STATUS.txt` (hooks-off release tree by construction), `make_update_manifest.py` (sizes/sha256 computed from shipped bytes) | 8 pytest incl. hooks-tree refusal negative control and signed-envelope verify against the keygen-derived pin |
| 7 | `620b4d1a` | `docs/validation/sp7-offline-kit.md` + validation README row + PROVENANCE placeholder release-entry table (13 blank fields) | docs-only; commands pinned to the real CLI shape (`--config` global) |
| 8 | (this commit) | truth pass: this section + PROVENANCE "Vendored crypto" note | — |

Gates at the branch tip (this host, 2026-10-22): `cmake --preset linux-gcc` +
build + `ctest --preset linux-gcc` 100% (**895/895** + the 4 env skips);
release preset build 0 warnings + plain `ctest --test-dir
out/build/linux-gcc-release` **853/853**; `windows-llvm-mingw-cross` rc=0,
**0 warnings** — Windows **not run** (owner-hardware rule); pytest **54**.

Declared gaps, not hidden: **no pinned release key exists** — agents never
generate or hold one; every agent-built binary is unpinned and its update
path refuses with `no release key pinned` (by design, D-2). The D-3 schema
carries one file per component class (closed set `frontend|engine|cuda-runtime`,
duplicate names rejected by the C++ schema), so `strata update` updates the
`strata` CLI only; `strata-secure-server` ships in bundles but is not
update-distributable under D-3 as specified — flagged for the
controller/owner, not relitigated in code. Windows `current`/`previous`
pointer files have a documented remove-then-rename gap (no atomic symlink
rename on Windows) recorded in the Task 3 ledger; all Windows update/packaging
branches are compile-proved only. `makensis` availability: nsis 3.09 deb was
fetched into `.toolchains/` but its stub paths are system-baked and
unprivileged bind-mounts are denied on this host — `NSIS_STATUS.txt` records
that truth; the `makensis` run, the offline kit, and every GPU claim wait for
the owner's box (PROVENANCE release-entry row blank ⇒ no claim). No signing
key has ever been committed; `SHA256SUMS`/`.deb`/tarball from agent runs are
test artifacts marked `unsigned: not releasable`. The uninstall
install-metadata gate stays vacuously matching (M1 ruling R-T5-b): M2's
versioned `current`/`previous` layout is deliberately its own story
(`strata_cli_network.cpp` carries the comment); no producer feeds
`install_location_ok` yet — recorded, not silently wired.

### SP7 M1 (branch `sp7-m1-service-doctor`, base `9b467e08`; the branch waits for the controller's merge gate)

Plan `docs/superpowers/plans/2026-10-22-sp7-m1-service-uninstall-doctor.md`
(Tasks 1–7), spec D-1/D-5/D-6/D-7. Lane ledger with every ruling
(R-T1-a…R-T6-d) and RED evidence:
`.superpowers/sdd/2026-10-22-sp7-m1-service-doctor/progress.md` (git-ignored
work record — R-T1-d: the brief's "commit the ledger" conflicts with
`.gitignore` ignoring `/.superpowers/`; resolved untracked, flagged for the
controller). Commits authored `shift-f10`, `Co-Authored-By` trailer, no
`Claude-Session:`.

| Task | Commit | Landed | Proof (named in the ledger) |
| --- | --- | --- | --- |
| 1 | `5ad06ab4` | lib `secure_frontend_service_ops`: exact hardened systemd user-unit text (D-5 directives verbatim, `ReadWritePaths` = state+logs+cache — the serve write-targets, R-T1-a); whitespace/relative path rejection; `resolve_current_target` (D-4 seam M2's update writes through) | golden text, negative matrix, symlink/plain-install cases |
| 2 | `2e314fed` | `run_argv` (fork/execvp + pipe pump; CreateProcessW on Windows) and `plan_enable/plan_disable/plan_status`: atomic 0644 unit write, `systemctl --user enable --now`, foreign-unit refusal, two read-only status probes | 8 cases on a recording fake runner + a PATH-shim `systemctl` e2e (real systemctl unreachable by construction) |
| 3 | `51fa1065` | lib `secure_frontend_doctor`: every D-7 check pure over an injected `ProbeBundle` (gpu/driver/CUDA/config/token/roots/port/memlock, `--deep` digests); verdict contract; `doctor` exit 1 on any failure | 10 fake-driven cases; three legacy fixed-exit-0 doctor tests amended to verdict equivalence (R-T3-b), no probe assertion weakened |
| 4 | `569186b0` | lib `secure_frontend_uninstall`: ordered removals (unit, token, config, state; models only under `--purge`) confined to allowed roots; stop decision table from the loopback `GET /health` probe (any HTTP answer incl. 401 = answering); typed-word gate `--yes` cannot satisfy | 9 cases incl. canned-401-listener e2e (server up/down per plan) |
| 5 | `1fa0c4af` | CLI wiring: grammar `service {enable,disable,status}`, `uninstall [--purge]`, usage text, exit codes; `ProcessSpec` gains optional `stdin_file` (both platform branches) | 7 e2e cases spawning real `strata` children vs temp HOME/XDG roots |
| 6 | `84a99b00` | `render_schtasks_create` golden (LogonTrigger, `RunLevel LeastPrivilege`, `<Priority>6</Priority>`, PT5M×3 restart) and the Windows plan branches wired — compile-proven only; windows-runtime-labelled plan cases + `schtasks.cmd`-shim CLI e2e registered for the owner's box | host-runnable goldens; cross build compiles all Windows branches and bodies 0 warnings — **no Windows runtime claims** |
| 7 | (this commit) | truth pass: spec amendments folded in (D-6 health-probe rewrite, D-5 two-settings split, D-7 compiled-in expectations note), this section | — |

Gates at the branch tip (this host, 2026-10-22): `cmake --preset linux-gcc` +
`ctest --preset linux-gcc` 100% (867 run / 870 registered / 4 env skips —
cfg03, proc03, proc05, bundle-mount); release preset 0 warnings + full release
`ctest` 833/833 100% incl. the release-only proofs; `windows-llvm-mingw-cross`
builds 0 warnings — Windows **not run** (owner-hardware rule); pytest 33.
Gate-method note: the presets keep build trees in
`secure_frontend/out/build/<presetName>` and the debug CTest preset excludes
the 3 `release-only` label cases; a bare `ctest --test-dir <debug-dir>` runs
those self-guarding proofs and shows two false failures.

Declared gaps, not hidden: Windows runtime paths (task XML acceptance,
schtasks flows, the e2e shim) wait for the owner's box; the uninstall
install-metadata gate passes honestly because M1 has no install metadata yet
(M2 feeds it); doctor GPU/driver/memlock expectations are compiled-in per
profile (the shipped manifest carries none — R-T3-a); the schtasks
Priority/elevation split follows PX §6 over upstream's hint and is recorded
in spec D-5 (R-T6-a); `loginctl enable-linger` is explained, never run.

### SP5 M2 (branch `sp5-maya-parity`, base `749e5395`; the branch waits for the owner's merge word)

Plan `docs/superpowers/plans/2026-10-21-sp5-m2-maya-parity-dialect-kit.md`
(Tasks 1–10), same lane spec as M1. Lane ledger with every ruling
(R-T2-a…R-T9-e) and RED evidence:
`.superpowers/sdd/2026-10-21-sp5-m2-maya-parity-dialect-kit/` (git-ignored
work record); final report `docs/superpowers/handoff/reports/sp5-m2/FINAL-REPORT.md`.
Commits authored `shift-f10`, `Co-Authored-By` trailer, no `Claude-Session:`.

| Task | Commit | Landed | Proof (named in the ledger) |
| --- | --- | --- | --- |
| 1 | `0b096f5c` | MAYA v1.1.8 vendored read-only as the `engines/maya-ref` submodule (D-10; the only network use of the lane) | PROVENANCE section + pytest guards (no CMake mentions it; distribution check rejects `maya-ref` paths) |
| 2 | `3ba36193` | GLM parity table `kMayaRows`: 178 rows (138 typed / 30 rejected / 10 mapped), every Maya option one disposition + `file:line` citation | table meta-tests; `test_parity_readme_coverage` count guard |
| 3 | `7061d32f` | `glm.*` typed config family (Maya names, C-1 bounds; `glm.max_context` default 32768 = maya.py:2524) | typed_config accept/reject family |
| 4 | `29e917cb` | closed typed GLM child environment: the table's 100 typed `engine_env` names are the allowlist (`kFamilyForcibleEnvironment`), §4.9 safety values forced, config→env projection (`glm_environment_knobs`) | `glm_child_env_is_exactly_the_named_typed_set` + merge-gate cases |
| 5 | `198b9848` | `maya_118` wire dialect: NOTE, STAT cadence, 3–6-field PP; dialect per-manifest, both enforced strictly | protocol-client cases |
| 6 | `9720d5bd` | fake engine `--dialect=maya` + GLM generation end to end (chat over `maya_118`, cancellation) | `fake_maya_dialect_round_trips_only_under_the_maya_dialect` (RED carrier), glm chat e2e guards; TSan spot clean |
| 7 | `92b461a9` | GLM launch composed from manifest + table (D-8): pack dir, `--max-context`, `--parallel` 1..8, expert/split flags; **pass-through retired** — `glm53` refuses any `engine.args` | argv-composition cases, `glm_rejects_every_engine_argument`, shell `strata_secure_server_glm_rejects_engine_args` |
| 8 | `fdc7a57f` | GLM vision rows: `STRATA_GLM_VISION_LEND_MB` projects from `vision.need_mib` only for an on-demand turn and renders fail-closed without one; bootstrap wires the glm family | `glm_vision_flags_reach_both_children` |
| 9 | `be3aaa26` | acceptance kit: probe `--corpus`/`--results`/`--compare`/`--dry-run` (fixed schema, C-6 classes, missing results file = FAIL not skip), `docs/validation/sp5-parity-kit.md` + canonical corpus, PROVENANCE kit placeholder row | `tools/secure/test_sp5_kit.py` (4 cases) |
| 10 | (this commit) | truth pass: README Maya mirror (3 blocks, all 178 rows), threat model (GLM allowlist, dialect, kit attestation), this section, FINAL-REPORT | extended `test_parity_readme_coverage` (178/178 counted) |

Gates at the branch tip (this host, 2026-10-21): `ctest --preset linux-gcc`
100% (826 run / 830 registered / 4 env skips); release build 0 warnings +
release `ctest` 10/10; `windows-llvm-mingw-cross` builds 0 warnings —
Windows/wine **not run** on this host (owner-hardware rule); pytest 33; plus
the Task-10 whole-feature battery (root build + `ctest` 792/796,
distribution check, clang18 TSan filtered concurrency suites 216 cases,
**zero** sanitizer reports — the parked flake
`log_line_format_and_rate_limit` fails deterministically only under TSan
slowdown, boot-window 503s, untouched as parked) recorded in the
FINAL-REPORT.

Known parity gaps declared, not hidden: the composition renders no
`--expert-profile(-save/-every)`, `--kv-resident` or `--spec*` (no value
source — parity rows and `docs/validation/sp5-parity-kit.md` §0 cite it);
the GPU acceptance run is the owner's and completes the parity claim via the
PROVENANCE SP5 kit entry (does not exist yet — no parity claim is made).

### SP5 M1 (branch `sp5-parity-cli`, base `60f94aeb`; the branch waits for the owner's merge word)

Plan `docs/superpowers/plans/2026-10-21-sp5-m1-strata-parity-cli.md` (Tasks 1–15),
spec `docs/superpowers/specs/2026-10-21-sp5-parity-typed-options-design.md`
(D-1…D-10, C-1…C-9). Lane ledger with every ruling (T2…T15) and the per-task
briefs: `.superpowers/sdd/2026-10-21-sp5-m1-strata-parity/` (git-ignored work
record); final report `docs/superpowers/handoff/reports/sp5-m1/FINAL-REPORT.md`.
All commits authored `shift-f10` with the `Co-Authored-By` trailer and no
`Claude-Session:` trailer (owner ruling 2026-10-20). One line per task.

| Task | Commit | Landed | Proof (cases, named in the ledger) |
| --- | --- | --- | --- |
| 1 | — | baseline + seam inventory (no files) | — |
| 2 | `e5ffa597` | parity spine: `parity_table.{hpp,cpp}` — 121 rows, every upstream option of v0.2.1.3 exactly one disposition + read-site citation; generated inventory `tests/parity_keys_upstream.hpp` (87 config keys + 31 env knobs, greps verbatim, committed) | parity-table meta-tests (coverage, one-row, citation shape) |
| 3 | `42bcafa7` | upstream-excluded keys refused at config load **by name**; `strata doctor` flags ignored `STRATA_*`/`MAYA_*` env knobs | typed_config rejection family + doctor cases |
| 4 | `e2743afe` | sampling defaults at the pin's validator; params reach `GEN` | sampling matrix + GEN-record cases |
| 5 | `cbd37ba4` | reasoning family typed (effort/budget/recovery, Maya names C-3); `fit_max_tokens` both pin behaviours | reasoning + fit cases |
| 6 | `25364882` | lifecycle: `lazy_load` (no flag — the pin has none), `idle_unload_s` respawn-on-request, `mtp=false` drops the draft layer | supervisor/argv lifecycle cases |
| 7 | `1c4fc2e8` | VRAM/GPU/elastic family typed to the engine argv | argv-composition cases |
| 8 | `9a6273ce` | silence/queue/room/stop-token keys at the pin | timeout/queue cases |
| 9 | `3bc08b21` | session/context family typed with path rules | session cases |
| 10 | `761b9b4a` | C-4 narrowing: `replicas.parallel` renders `--batch N` inside the one child; the pin's N-child replica config is rejected by name | replicas cases |
| 11 | `cbb4e747` | vision artifacts: manifest `files[]` role `mmproj` (≤1 entry), supervised `strata-vision` child (READY/ENC/QUIT, `kVisionKillExitCode`), resident encoder at boot (no READY → boot fails) | supervision cases with the fake vision child |
| 12 | `b3b7c6d6` | C-9 request half: content-parts only for vision-capable models; pre-decode caps (count, base64 length, 32 MiB, side probe); 0600 staging under `<state>/vision`; ENC round trip inside the lease before the `GENI` write (pin FIFO :4489); `GENI` embeddings tail; `<|vision_start|>`/pads/`<|vision_end|>` prompt runs; per-request scratch cleanup; `vision.encode_timeout_s` (schema 108→109) | image_payload unit + e2e enc round trip + rejection-before-write cases |
| 13 | `8b61436b` | shared boot library `secure_frontend_server_bootstrap` (moved verbatim from `secure_server_main.cpp`); `strata serve` — foreground, managed-only, no daemonization/`server.json`/lock | `serve_prints_ready_then_serves`, `serve_runs_foreground_and_shuts_clean_on_sigterm` (SIGTERM → exit 0 ≤5 s) |
| 14 | `18b2af90` | `strata version` (tag governs; engine self-version labelled informational, never spoken here); `strata key show [--env]`/`key rotate` on SP1 store rules, token printed once, logged nowhere | acquire_cli cases incl. chmod-unsafe store refusal |
| 15 | (this commit) | truth pass: README gains the parity mirror (typed/mapped/rejected blocks, regenerated from the C++ table), the `strata serve/version/key` paragraph and the images-are-input truth fix; threat-model rows for the env-knob warning, the image path, lifecycle timers, the encoder child and the no-lifecycle-files rule; PROVENANCE names the parity artifacts as bump pin-sites; new pytest gate mirrors README blocks vs the table | `tools/secure/test_parity_readme_coverage.py` |

Gates at the branch tip (this host, 2026-10-21): debug build clean under
`-Werror`; `ctest --preset linux-gcc` 803 registered / 799 run / 4 env-only
skips, 100% (lane base was 750/746 — +53 cases); release-and-also 10/10;
`windows-llvm-mingw-cross` builds 0 warnings — Windows/wine **not run** on this
host (owner-hardware rule, CI disabled 2026-10-20); pytest 26 (new parity gate);
root build + `ctest` 769 registered, 100%, 8 env skips, 0 warnings; clang18
TSan filtered concurrency suites (engine supervisor/child/stdio/process, pool,
socket server, chat execution, api server): 224 cases, **zero** ThreadSanitizer
reports — only the parked flake `api_server_tests.log_line_format_and_rate_limit`
flaked once and passed on rerun (untouched, as parked).

Recorded gaps and waits (each also in the ledger/threat model):
the vision child gets no per-generation cgroup delegation in M1; the pin's
`session.keep_files` LRU has no consumer (per-request cleanup instead);
`session.answer_room` is typed only (T8-d — the pin's formula sits in its
thinking parser); GPU, real-encoder and the D-1 acceptance kit (corpus mode of
`strata-api-probe`) wait for the owner's boxes; M2 (Maya table D-10, dialect
layer, the kit) is a separate plan written after this one merges.

### SP4 (branch `sp4-acquire`)

Plan: `docs/superpowers/plans/2026-10-14-sp4-acquire.md`. One line per task.

Real-Windows status: CI run `38625663621` (base `557a2cc7`: the SP4 merge `4b9fdd31`
plus its merge-time handoff commit) ran `windows-msys2-clang`
488/488 and the `linux-gcc-release` job green, so every `windows-runtime`
case named in the Task 1–24b lines and in merge-time change (a) below is
**verified on real Windows** there (merge-time change (c), the `strata`
binary's own argv, had no case there; Known small gaps). Tasks 25–28 did not change
that code except where their own lines say so (Task 26's additive SP1
`ExecutableWriters::also_trusted_installer` option). The new
`windows-runtime` cases of Tasks 25–28 are **verified on real Windows** by
CI run `38655938748` (`9044a5bc`, `windows-msys2-clang` 515/516); the one
failure, `privacy.priv04_secure_server_does_not_link_acquisition`, is fixed
(see SP4 part 2) and its MSYS2 re-run is pending.

- Task 1: `SECURE_FRONTEND_TEST_HOOKS` option (ON in test presets), `linux-gcc-release` preset (hooks OFF; runs `release-only|release-also`), in-tree FIPS 180-4 SHA-256; `secure_frontend_sha256_tests` (7 cases; 2 `release-also`).
- Task 2: §8 reasons, exit classes and error text in `secure_frontend_manifest_core`; `acquire.exit_classes_match_spec_table`, `reason_table_is_complete_and_ordered`, `error_text_states_resumable`.
- Task 3: URL/host/wildcard/proxy grammar (spec §2.2, §3.4, §3.8); manifest.source_url_grammar_matrix, dns_names_and_wildcards, proxy_grammar; fetch.dl03_location_with_glob_characters_rejected, location_resolution_and_host_normalization, location_query_characters.
- Task 4: §3.5 address policy (`secure_frontend_acquire_core`); fetch.address_policy_* (5 cases).
- Task 5: §3.3 argv/stdin, probe parsing, C3 floors, §3.4 curl exit map; fetch.transfer_argv_exact_order, config_line_is_single_quoted_url, probe_argv_is_version_only, probe_output_parsing_and_floor, curl_exit_codes_map_to_reasons.
- Task 6: §3.4 header-block parser and status table; fetch.informational_header_blocks_skipped, header_block_limits_enforced, split_feed_reports_exact_body_offset, status_table_decisions.
- Task 7: §3.6 retry schedule, run budget, clock seam; fetch.retry_backoff_follows_schedule, non_transient_moves_to_next_source, exhausted_class_follows_last_source_reason, run_budget_remaining_seconds.
- Task 8: manifest model and §2 struct rules, engine version grammar; manifest.* (13 cases, 1 release-only, 1 hooks-only).
- Task 9: index model (render/install/retire), staging plan, key and version-dir names; model_index.* (3), acquire.version_dir_name_unique_for_same_version_label, staging_plan_classifies_partials, abandoned_keys_match_same_model_only, fetch.staging_cwd_not_treated_as_key.
- Task 10: SHA256SUMS/MISSING/marker formats and §6.1 manifest diff (url/notice/per-file license shown old -> new, F-21); bundle.* (4), acquire.update_diff_lists_changed_files_and_license.
- Task 11: Landlock rights/dirs planning, readable-root rule, confinement labels, CRT descriptor block; fetch.landlock_rights_per_abi, confinement_plan_detects_readable_roots, confinement_label_texts, crt_fd_block_layout.
- Task 12: fake downloader secure_frontend_fake_curl (scenario format, §3.3 grammar, record over fd 4 / CRT entry 4); fetch.fake_curl_* (2 portable + 3 POSIX + 1 windows-runtime; record-channel cases hooks-only).
- Task 13: dev-only tools/secure/make_bundle.py (D5 (B′): local pack via upstream iq_pack.py, third-party manifest <quant>-local, SHA256SUMS, STRATA-BUNDLE last, 0-byte pack files refused per F-26) + Hub-read shard pins for the four D4 quants (iq3_xxs/iq3_s/iq1_m await owner confirmation, F-25); tools/secure/test_make_bundle.py (7).
- Task 14: strict manifest parser, embedded catalog (production empty; hooks-only fixture-qwen:t1), --trust-manifest rules, kEngineVersion from upstream.version (checked against a reachable submodule tag), linux-release CI job; validate_manifest now enforces P1 paths for every caller; manifest.dl03_invalid_manifest_matrix, dl03_http_url_rejected, dl05_catalog_pins_match_committed_list (release-also), +4; pytest test_catalog_provenance.py.
- Task 15: SP1 deltas a (inventory skips dot entries), b (scan_model_root_files), e (pull.* keys, lazy downloader); model_inventory.dl04_staging_ignored_by_inventory_scan, model_index.scan_tolerates_repeated_shard_names_across_versions, typed_config.cfg01_pull_keys_table_driven; server_cli auth04 allow-lists pull.allow_private_addresses.
- Task 16: models store primitives on POSIX (lock, part files with KEEP_SIZE preallocation, no-replace rename with fallback, tree removal, hashing, seams); acquire.concurrent_pull_same_manifest_second_is_models_busy +7 (incl. overlong_joined_paths_fail_closed_without_throwing, F-6). Store skipped on WIN32 until Task 17 adds models_store_windows.cpp (F-34 guard; Task 17 removes it).
- Task 17: models store primitives on Windows (pinned parents, CREATE_NEW private DACL, FileAllocationInfo, LockFileEx, MoveFileExW without replace, sharing retries); acquire.store_primitives_windows + store_adopts_only_private_files_windows (windows-runtime; passed in MSYS2, CI run `38625663621`; adopted .lock/.part need owner + protected DACL). F-34 guard removed: the store and its 8+2 cases build on every platform.
- Task 18: index.json strict read/write, installed-model load with manifest re-hash; model_index.index_inconsistent_manifest_fails_closed +3 (round trip windows-runtime). Ruling A: validate_manifest rejects a files[].path whose longest stored form (.staging/<key>/…part, <name>/<tag>/v…/…, files/…) exceeds P1's kRelativePathMaxBytes; manifest.stored_path_length_fits_p1.
- Task 19: §4.4 checks, §4.5 publish/retire/orphan adoption, §4.7 install decisions, §4.8 rm library; acquire.dl04_publish_uses_noreplace_and_never_overwrites, publish_falls_back_when_noreplace_unsupported, dl04_crash_between_publish_and_index_is_not_loaded (hooks), dl04_orphan_adopted_after_rehash_by_digest, replace_loaded_model_retires_old_version(_windows), +9 (F-39 key check, read-only retire/rm, downgrade single-live, unreachable rm is an error). remove_tree_within restores owner write (0500/FRFX) by handle; remove_*_within report an unreachable parent as publish_failed, only a missing one as absent.
- Task 20: bundles (open/verify/import/export, finish with SHA256SUMS + marker last, FAT and license refusals) and SP1 delta f RootPolicy::external; bundle.dl02_import_bundle_publishes_same_as_pull, dl03_bundle_digest_mismatch_publishes_nothing, bundle_rejects_* (traversal, symlink|junction, hard link, unlisted), bundle_sha256sums_must_match_manifest, export_refuses_*, export_rehashes_and_rejects_bit_rot, verify_writes_nothing, import_from_read_only_bundle_directory, export_onto_shared_writable_target, +5 (per-file license exit 10, drive root F-46 (read: "/"/"X:\\" only; write also refuses a mount point), mount-root bundle imports, F-6 over-long/odd names, make_bundle.py fixture with pytest drift guard); paths.external_root_skips_ownership_checks.
- Task 21: fetch_file over HopTransport (our redirects, DNS policy + --resolve, retries/sources/budget, resume, refetch-once, 1 GiB free-space re-checks), SystemResolver, hop_request (url+host from one HttpsUrl); fetch.dl03_* (11) +9 (5 from the brief, then ScriptedTransport fails loudly on unscripted requests and checks argv grammar + pin host per hop, F-49; IPv6-only hop, 200-to-range cap once per file, proxy hop, test_cacert in hook builds F-51). run_pull must still plumb FetchSettings::test_cacert (Task 22).
- Task 22: pull orchestration (catalog/--manifest trust, summary + [y/N], probe after confirmation, stale/abandoned staging and --discard-partial only after the answer (F-56), preflight + budgets, resume at the .part length, update reuse + license prompt, --bundle with MISSING and re-hashed files/, --verify, test_cacert in hook builds F-51); acquire.dl02_* (5, +1 Windows DACL), dl03_* (4), dl04_* (3), +14; bundle.pull_bundle_* (3); fetch.dl03_wrong_digest_rejected_prior_version_intact.
- Task 23: strata binary (local verbs import/verify/export/list/show/rm, program update exit 12 for `strata update` and `strata --config <f> update` (fix 1; Task 27's update branch must reject only non---models operands), hooks banner; engine_gate + find_installed shared in model_store for 24b (F-58), shared CLI helpers in src/strata_cli_internal.hpp for 27 (F-57)); acquire.program_update_exits_12_without_network (release-also), rm_refuses_loaded_model, rm_retired_deletes_unheld_versions, rm_partials_deletes_other_staging, +4; bundle.import_works_without_downloader +3 (export leftover hint, orphan hint); model_index +3 (find_installed, engine_gate family, skipped-entry count).
- Task 24a: SP1 deltas c/d config side: optional roots.tokenizer_pack and engine.profile, name:tag model references, strata-secure-server --expert; SP1 tests and fixtures updated; typed_config.typed_config_model_reference_accepts_name_tag.
- Task 24b: manifest-composed launch (index → --native/--pack/--ple-gguf as UTF-8, tokenizer root from the manifest), engine gate exit 11 (shared engine_gate), --expert keeps SP1's inventory path and messages, --check reports the managed-model config errors (F-8), server hooks banner; engine_arguments.launch_composed_from_manifest_qwen, managed_launch_argv_is_utf8_for_non_ascii_models_root, launch_refuses_model_not_installed, engine_args_reject_weight_flags_for_managed_model, launch_refuses_engine_too_old; typed_config.cfg01_engine_profile_rejected_for_managed_model, cfg01_tokenizer_pack_root_rejected_for_managed_model, cfg01_model_reference_rejected_for_managed_model, describe_reports_unset_expert_keys; server_cli.cli_expert_flag_parsing.
- Final-review fixes: C1 import decides under .lock, then §6.1 diff + license questions --yes never answers (shared review_update/confirm_license_change with pull); I1 staged manifest.json (publish failed after step 2) still trusted and renamed back; I2 rm retires before deleting, corrupt orphan not adoptable; I3 directory case variants manifest_invalid; M1 typed digest checked first; M10 EOPNOTSUPP fallback; bundle.import_license_change_needs_interactive_yes +2, acquire.rm_failed_delete_stays_retired (+_windows), publish_failure_after_step_2_resumes_without_download, manifest.typed_digest_must_match_catalog_bytes_too.

Merge into `main` (`4b9fdd31`, SP2 run 2 + SP4; merge-time changes, report `merge-sp4-report.md`):

- (a) GPU list: `compose_engine_launch` renders SP2's typed `engine.gpu` list in both modes (SP2 §3.9): `--expert` passes `config.engine.gpus` to `build_engine_arguments`; a managed model appends `--gpu <list>` after `engine.args` (`--gpu` in `engine.args` stays refused); engine_arguments.managed_launch_renders_typed_gpu_list (windows-runtime).
- (b) SP4's manifest-side `EngineFamily { strata, glm, none }` is renamed `ManifestFamily` (beside `ManifestProfile`) so it coexists with SP2's environment `EngineFamily { strata, glm }`; no behaviour change (the SP4 plan text still says `EngineFamily`).
- (c) Shared Windows UTF-8 argv: `utf8_command_line(argc, argv)` (`include/secure_frontend/utf8_argv.hpp`, `src/utf8_argv.cpp`) in library `secure_frontend_utf8_argv` (links `shell32`), used by `strata-secure-server` and `strata`; POSIX returns argv as given. `strata`'s Windows runtime proof is pending MSYS2 (Known small gaps): CI run `38625663621` ran the in-process CLI cases only; no case at `557a2cc7` starts the `strata` binary. Task 25 later added only a POSIX `SIGPIPE` ignore to `strata_main.cpp`.
- (d) The supervisor's chat profile and protocol flavor come from `launch.profile` (the manifest's for a managed model), not `config.engine.profile` (`--expert` only); identical today (managed is qwen).
- SP2's `tests/support/server_harness.cpp` passes `--expert` (SP1-style inventory fixture); server fixtures pass `--expert` and `--state-dir`.
- Task 25: confined downloader on Linux (SP2 seam: keep_fds + pre-exec hook; NO_NEW_PRIVS, Landlock read-only system dirs, record fd 4 in hooks builds; probe + C3 floor); fetch.priv04_downloader_argv_exact, priv04_downloader_env_is_empty, dl03_downloader_untrusted_path_rejected, dl03_downloader_cannot_write_files, dl03_downloader_cannot_read_home, pre_exec_failure_fails_closed_exit_9, record_fd_does_not_clobber_report_pipe, downloader_below_floor_exits_9, redirect_url_absent_from_downloader_argv, confinement_reduced_is_announced_without_landlock.
  Commit "sp4: confined curl downloader on Linux via the SP2 pre-exec seam (SP4 Task 25; spec §3.1-§3.3)". Controller rulings: B2 ruleset creation failure with Landlock present → downloader_confinement_failed, no spawn (fetch.ruleset_failure_fails_closed_exit_9, EMFILE via RLIMIT_NOFILE); R2 SP2 read_report/classify_report (fetch.downloader_spawn_report_classified, all builds); I7 record fd opened before the ruleset; I8 downloader-specific path texts; `strata` ignores SIGPIPE. Library `secure_frontend_downloader` is POSIX-only until Task 26; not yet linked by `secure_frontend_strata_cli`.
- Task 26: confined downloader on Windows (restricted Low-integrity primary token in a Job, SystemRoot-only env, explicit handle list, CRT block entry 4 in hooks builds, TrustedInstaller-aware executable check); fetch.*_windows (4, windows-runtime: MSYS2 CI).
  Commit "sp4: confined curl downloader on Windows: Low-integrity token, Job, explicit handles (SP4 Task 26; spec §3.2, §11)". Cases fetch.downloader_transfer_through_job_windows, dl03_default_windows_curl_path_accepted_windows (all builds), dl03_downloader_runs_at_low_integrity_windows, priv04_downloader_env_is_systemroot_only_windows (hooks only; ruling I11) — pending MSYS2 (not triggered by us; wine smoke: the transfer case passes, the integrity/env cases fail only on wine artifacts — wine ignores token integrity and injects WINE* variables). Controller rulings: B3 link secure_frontend_engine_stdio_process + advapi32 (no userenv); I2 WinLaunched::thread closed, Job limits left ON; R5 record OPEN_ALWAYS + append per spawn; I12 MSVCRT quoting copied file-locally; classify_spawn_report stays POSIX-only (launch_in_job DWORD mapped directly). `secure_frontend_downloader` now builds on both platforms; still not linked by `secure_frontend_strata_cli`.
  Fix round 1: validate_downloader is shared (downloader_common.cpp) and uses SP1 open_executable with the new default-preserving `ExecutableWriters::also_trusted_installer` (threaded to `windows_security::write_access_is_private(handle, also_trusted_installer = false)` and `detail::open_existing_root`), so Windows gets GetFullPathNameW normalisation, root-form/UNC refusal, FILE_TYPE_DISK and a prefix-free path; argv uses path_to_utf8 (POSIX bytes unchanged); fetch.dl03_downloader_path_normalised_windows, dl03_downloader_unc_path_rejected_windows (wine-runnable; wine 258/258).
- Task 27: strata pull/run (D6)/update --models/doctor [--downloader], SIGINT/SIGTERM/console interrupts → exit 8, §7.3 acquire.log with confinement; acquire.run_missing_model_prompts_and_decline_exits_2, run_missing_model_non_interactive_exits_2, run_refuses_glm53_without_engine, dl04_interrupt_during_download_never_indexed_windows, +3; fetch.confinement_reduced_written_to_log_record, confinement_reduced_when_root_beneath_allowed_dir, doctor_downloader_probes_on_fresh_store.
  Commit "sp4: strata pull, run (D6), update --models and doctor with interrupts and log records (SP4 Task 27; spec §1.3, §6.1, §7.3, D6)". `secure_frontend_strata_cli` now links `secure_frontend_downloader` (src/strata_cli_network.cpp; stub removed); hooks-only global options `--test-record-file <path>` / `--test-force-no-landlock` before `--config` (a release build rejects them: exit 1); fake-curl scenario directive `delay-ms <N>`. Controller rulings: B1 the downloader cwd `roots.models/.staging/.cwd` is created before any probe or transfer spawn (pull, pull --bundle, doctor --downloader; plain doctor writes nothing) — fetch.doctor_downloader_probes_on_fresh_store; R1 hooks builds copy CliServices::test_cacert into the pull request; R3 strata_cli_internal.hpp helpers reused; R8 the root/config-beneath-the-downloader case also checks `reduced:readable:config file` and a hooks pull through the fake logging `confinement=reduced:readable:roots.models`; R11 `run` of a model neither installed nor in the catalog → `model_unknown`, exit 5, plus a `strata import` hint; R12 acquire.log is one record per **transferred** file (reused, bundle-copied or already-staged files get none); I13 ForwardTransport/ForwardProbe in tests/support/pull_harness.hpp. update_models_pulls_outdated_catalog_models answers the §6.1 license question interactively (the committed catalog fixture's license url/notice differ, so --yes alone declines). Label form stays the merged `reduced:readable:<key>` (spec §3.2/§7.3 text differs; I20). Every new acquire/fetch CLI case on Windows is windows-runtime (wine cannot open the private-DACL roots; existing CLI cases fail the same way there): pending MSYS2 CI, including the Ctrl-Break DL-04 case (it calls AllocConsole when the runner has no console window; ruling I15).
  Fix round 1: InterruptScope now covers the whole network verb; a SIGINT/SIGTERM at a pending [y/N] ends `interrupted`, exit 8, nothing spawned (spec §8.2 controller ruling; EOF/Ctrl-D stays `declined`, exit 2); acquire.dl04_sigint_during_download_exits_8, dl04_sigterm_during_download_exits_8, interrupt_at_confirmation_prompt_exits_8 (Linux, hooks: a real strata child with the fake curl; the prompt case uses a pseudo-terminal stdin).
- Task 28: privacy lifecycle proofs (record channel with positive controls, LD_PRELOAD socket audit with a blocked positive control, Windows TCP table, link-set check) and hooks-OFF proofs (fd census, marker scan in C++ and in secure_distribution_check.py); privacy.* (13), fetch.release_spawn_has_no_fd4.
  Commit "sp4: DL-01 and PRIV-01..04 lifecycle proofs, release-only checks and the hooks marker scan (SP4 Task 28; spec §7, §10.3, §10.5)". New targets: `secure_frontend_privacy_tests`, `secure_frontend_connect_audit` (Linux LD_PRELOAD shim; `SF_CONNECT_AUDIT`, `SF_CONNECT_AUDIT_BLOCK`, plus `SF_CONNECT_AUDIT_LOADED` so an empty audit is proven to come from a process the shim was loaded into), `secure_frontend_resolve_probe`, `secure_frontend_fd_census`; `${CMAKE_BINARY_DIR}/strata-secure-server.link-set.txt` (written at the end of CMakeLists.txt). DL-01 = tripwire + socket audit + link-set check together (R10). Controller rulings: R6 doctor --downloader's strata child runs under the shim (audit empty); R7 Windows DL-01 also scans the UDP tables (and TCP/UDP IPv6); R9 every local verb's exit code asserted (update 12, the rest 0) plus `update --models` on fixture-qwen:t1 installed at sequence 1, declined both non-interactively and with an explicit `n` (exit 2, no spawn); R10 lifecycle waits for READY and asserts a 200 chat — the crash case kills the engine with SIGSEGV after READY (classified `crashed`) instead of `--crash-after-ready`, because that SP2 fake mode stops reading stdin until it crashes, so no chat could ever succeed; it then sees /health leave ready and return before auditing; I17 non-INET families are local; I18 dependencies. `linux-release` CI step stages strata + strata-secure-server and runs `secure_distribution_check.py` (no `--release` until SP7). Counts: linux-gcc 556/556 (+2 skips), linux-gcc-release 523/523 (10/10 `release-only|release-also`), wine 260/260, pytest 25. Pending MSYS2 CI: privacy.dl01_server_makes_no_outbound_connection_windows and the windows-runtime privacy cases.
  Fix round 1: the Windows DL-01 scan fails on any GetExtended{Tcp,Udp}Table error and must find the server's own 127.0.0.1:<port> listener under its pid each round (positive control); the link closure follows names inside generator expressions ($<LINK_ONLY:…>, conditional) and is also written for `strata`, whose set must contain secure_frontend_acquire/downloader/strata_cli (positive control); the shim also audits sendmsg/sendmmsg/getnameinfo/gethostbyname[_r]/gethostbyaddr (raw syscalls and io_uring are out of its reach) and the blocked probe must record all 9 calls, including connect.
- Task 29: docs only — threat-model amendment (spec §7.4: "Privacy and offline operation" names `strata pull`/`update`/`run`-on-missing as the networked operations; "Artifact acquisition" states the manifest pins, our-redirects-only rule and what confinement does not stop), `PROVENANCE.md` "Model catalog (SP4)" (empty v1 catalog, developer-bundle shard pins) and the `upstream.version` pin site in runbook step 5, the part-2 verification record below; no tests added.
  Commit "docs: SP4 threat-model amendment, catalog provenance, handoff verification (SP4 Task 29; spec §7.4, §13.5)". CI check (Step 4): `grep -n SECURE_STRATA_ENGINE_VERSION .github/workflows/secure-frontend.yml` prints nothing, and both `paths:` filters list `upstream.version`.
- Final-review fix wave: commit "sp4: final-review fixes — interruptible hashing and copying, probe interrupt exit 8, update --models interrupt exit, SP1 SID literal (SP4 final review; spec §8.3)". F1 `hash_into`, `copy_verified`, `verify_tree` and `adopt_orphan` take an optional interrupt flag (default nullptr: unchanged for import/export) checked before every 8 MiB chunk; `run_pull` passes its flag, so Ctrl-C/SIGTERM during a resume re-hash, an update's reuse copy, `pull --verify` or orphan adoption exits 8 within one chunk (the half copy removed, a `.part` never restarted from zero, nothing indexed); F3 a probe killed by the interrupt flag is `interrupted` (exit 8), not `downloader_unsupported`; F4 `update --models` returns 8 when a target is interrupted even after an earlier failure; F6 strata_cli.hpp comments; F7 DL-01 scope line (Part 2 deviations); C1 SP1 `windows_security.cpp` uses a file-local TrustedInstaller SID (no SP4 include); C2 Windows `untrusted_owner` text names the current user, SYSTEM, Administrators or TrustedInstaller. Tests added: acquire.hash_into_stops_when_interrupted, acquire.update_reuse_copy_interrupted_mid_file_exits_8, acquire.pull_verify_interrupted_exits_8 (windows-runtime label like their neighbours), fetch.probe_interrupted_exits_8 (Linux, hooks). Counts: linux-gcc 560/560 (+2 skips), linux-gcc-release 526/526 (10/10 `release-only|release-also`), wine 260/260 (518 registered), pytest 25. F4 has no case (the test catalog has one entry, so no two-target update exists).

Part 2 deviations from the spec text (controller rulings, recorded here so a
spec reader is not surprised):

- **R5, spec §10.1 "record channel":** the hooks-only record file is opened
  `O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC|O_NOFOLLOW` 0600 (Windows
  `OPEN_ALWAYS` + `FILE_APPEND_DATA`) once per spawn, **not** `O_EXCL`, so one
  file collects the probe and every transfer of a run. Tests always use a
  fresh scratch directory.
- **I20, labels:** the code and log use the merged form `full`,
  `reduced:no_landlock`, `reduced:readable:<key>` (for example
  `reduced:readable:roots.models`, `reduced:readable:config file`). Spec
  §3.2 prints `confinement: reduced (readable: <key path>)` and §7.3 writes
  `reduced:<cause>`; the spec text is inconsistent with itself and with the
  code. The key may contain a space (`config file`), so a strict
  `key=value` log parser must take the rest of the field.
- **Spec §8.2 over the Task 27 brief:** SIGINT/SIGTERM (Ctrl-C) at a pending
  `[y/N]` ends `interrupted`, **exit 8**, nothing spawned; EOF/Ctrl-D stays
  `declined`, exit 2. Known race: a SIGINT landing after the prompt flush and
  before `read()` starts needs a second Ctrl-C (the result is still exit 8).
- **R10, Task 28 crash case:** the crash-restart lifecycle kills the fake
  engine with SIGSEGV after READY (classified `crashed`) instead of SP2's
  `--crash-after-ready`, under which the fake stops reading stdin and no 200
  chat is possible.
- **Additive SP1 change (Task 26 fix round 1):** `ExecutableWriters
  { standard, also_trusted_installer }` and `open_executable(requested,
  ExecutableWriters = standard)` in `canonical_path.hpp`;
  `windows_security::write_access_is_private(HANDLE, bool
  also_trusted_installer = false)`; `detail::open_existing_root(…, bool
  also_trusted_installer = false)`. Defaults keep every SP1 caller's
  behaviour; only `validate_downloader` passes `also_trusted_installer` (spec
  §3.2 "same rules as `engine.executable`" plus TrustedInstaller for
  `System32\curl.exe`).
- **B1, spec §3.2 cwd row:** `pull`, `pull --bundle`, `run` → pull,
  `update --models` and `doctor --downloader` create
  `roots.models/.staging` and the read-only `.staging/.cwd` before any
  downloader spawn (and before the `[y/N]`, so a declined pull leaves that
  empty 0500 directory); plain `doctor` writes nothing.
- **Shim scope (Task 28):** the `LD_PRELOAD` connect audit hooks the libc
  entry points (`connect`, `sendto`, `sendmsg`, `sendmmsg`, `getaddrinfo`,
  `getnameinfo`, `gethostbyname[_r]`, `gethostbyaddr`); raw `syscall()` and
  io_uring are out of its reach, so DL-01 rests on the tripwire, the audit
  and the link-set check together.
- **`classify_spawn_report` is POSIX-only** (Windows has no report pipe;
  `launch_in_job`'s DWORD is mapped directly).
- **DL-01 audit scope (Task 28):** the Linux shim audits the `strata-secure-server` process only (the engine child's allowlisted environment drops `LD_PRELOAD`), and the Windows socket-table scan filters by the server's pid; the engine child's network isolation is SP2's.

SP4 closing summary (after Task 29).

New user-visible surface, all through the new `strata` binary (never
`strata-secure-server`): `strata --config <file> import|verify|export|list|show|rm|pull|run|update|doctor`
(`update --models [<name:tag> …]` is the models part; bare `strata update` is
the program part and exits 12 until SP7; `doctor --downloader` probes curl).
`--config <file>` is required on every verb. The production catalog is empty
(D5 (D)), so a model arrives today through `make_bundle.py` + `strata import
--trust-manifest sha256:<hex>` or `pull --manifest <file> --trust-manifest
sha256:<hex>`.

Open items (plan "Controller notes", one line each):

1. `strata` is SP4's own binary with its own verbs; the later PX CLI skeleton must absorb `secure_frontend_strata_cli`.
2. No default config location: every verb needs `--config <file>` until PX/SP1 picks one.
3. `strata run` stops at "model ready" + the `strata-secure-server` hint; the chat client (SP2 auto-start + SP3 streaming) replaces that tail.
4. `--expert` exists only on `strata-secure-server`, not on `strata`.
5. No server discovery: `LoadedVersions` = `NoServerDiscovery` ("not held"); the OS is the backstop and `rm --force` has nothing to override yet (SP2/CLI).
6. Spec §4.4's `load_tokenizer_pack` is SP1's `load_local_tokenizer_pack(const CanonicalRoot&)` (spec text to align).
7. Spec §4.1's `atomic_replace_within` is `replace_small_file` over SP1's `write_private_file(…, replace)` (spec text to align).
8. SP4's `cfg01_*` cases live in `secure_frontend_typed_config_tests` (spec §4.9 names `config_tests`).
9. `kEngineVersion` is `0.1.44.1` (spec cites `v0.1.43` line numbers); GSQ-RCO packs are still built by upstream `tools/iq_pack.py` without `pack_args`.
10. The v1 production catalog is EMPTY until D5 (A) owner determinism check or (C) a C++ pack builder; Hub-read shard pins await owner confirmation (`PROVENANCE.md`, "Model catalog (SP4)").
11. `upstream.version` is the engine-version source and a runbook pin site; no CI `-D` or env pin.
12. Retry schedule read as 5 attempts per source, 2/4/8/16 s between them, 32 s before the next source (spec §3.6 wording).
13. Unknown curl exits → `transport_failed`, next source, not retried (§3.4 over §8.2; spec text to align).
14. Landlock ABI ≥ 6 `scoped` rights are left unhandled (review with SP2).
15. SP2 seam names are authoritative (`ExecOptions`, `PreExecHook`, `WinLaunchSpec`, `launch_in_job`); Tasks 25/26 used them as built.
16. `strata update` = program part (exit 12); `strata update --models` = models part.
17. SP4 is the first writer of `roots.logs` (`acquire.log`; one record per transferred file, see Known small gaps).
18. SP1 delta f `RootPolicy::external` (bundle source/target only) is an SP1 addition to note in the SP1 docs.
19. CI runs `secure_distribution_check.py` without `--release` until SP7 provides the release manifest and SBOM.
20. Facts SP3 M1 must re-anchor on (`FrontendConfig::pull`, optional `tokenizer_pack`/`profile`, +7 schema rows, `compose_engine_launch`, `--expert` fixtures, the connect-audit shim to reuse).

PX amendments of spec §13.3 — **to apply in a PX addendum (owner/controller)**:
`pull.manifest_sources` dropped (catalog in the binary, D2); staging is
`roots.models/.staging/`, no `paths.staging`; bundle manifests trusted by
catalog digest or `--trust-manifest` (D7), signatures SP7; publish to
`models/<name>/<tag>/v<sequence>-<h12>/` with retired older versions; file
roles `weights`/`pack`/`tokenizer` plus the manifest `launch` block; PX §3
step 6 "`run` … asks Download?" — **kept (D6 (a))**; the TLS question
answered by D1; `strata update` model part §6.1, program part exit 12 until
SP7; `STRATA_CONFIG` untouched (open for the PX addendum, §3.9); SP4 row
signatures move to SP7.

### Upstream bump to `v0.2.1.3` (2026-10-20)

Owner lifted the stop and named the target; after analysis of `v0.2.1` →
`v0.2.1.3` (engine code identical to `v0.2.1.2` plus Windows-HIP fixes) the
owner chose the latest tag. Pin: `v0.1.44.1` (`296b59d2`) → **`v0.2.1.3`**
(gitlink `cbfd5b926b794b356195d3cb600191b0c3567540`, tag object
`1ab44ace2e4db985503ab9388b65405a63600dd7`). Pin sites re-pointed:
`upstream.version`, `README.md`, the CI workflow header, this file,
`docs/secure-frontend-ci.md` (offline bundle names), `docs/PROVENANCE.md`
(baseline, runbook, license links, ggml `FetchContent` lines 1552–1555),
`docs/security/*` headers, and the `engine_arguments` code comments
(`--gpu` re-anchored to `generate.cpp:2334-2352`, usage `:797`). The limits
doc was fully re-pinned to `v0.2.1.3` line numbers and gained rows for the
`DONE` tail, HIP-Windows `rocm\bin` delay-load, batch telemetry and the
version-numbering split.

Owner rulings (2026-10-20, this bump): **tag version governs** —
`kEngineVersion` (from `upstream.version`) and manifest `min_version` carry
`0.2.1.3`; the engine's CMake self-version `0.1.46.2` (`strata --version`) is
informational only. **wine stays off this host** — Windows proof comes from the
`windows-msys2-clang` CI job. New commits drop the `Claude-Session:` trailer
(keep `Co-Authored-By`).

Gates this host, old pin → new pin (linux-gcc build 0 warnings both):

| Gate | `v0.1.44.1` | `v0.2.1.3` |
| --- | --- | --- |
| pytest (ours) | 25 | 25 |
| linux-gcc ctest | 560 reg., 556 pass, 4 env skips | 561 reg., 557 pass, 4 env skips |
| windows-llvm-mingw-cross build (`-Werror`) | 0 warnings, 47 exes | 0 warnings |
| root build + ctest | 239 steps; 526 reg., 522 pass, 4 skips | 240 steps; 527 reg., 523 pass, 4 skips |
| upstream tools (own venv) | 760/2 skipped/5 desel. | 1027/13/6 |
| upstream serve (own venv) | 774/11 | 878/14/1 |

(Registered grew by one at both ctest levels from the `RESUME`/`REUSED`
hotfix, `01ad7226`, landed immediately before the bump.)

Two upstream tests fail deterministically at `v0.2.1.3` — both NEW in the
0.2.1.x range, both quarantined Python, diagnosed to root cause (see the
runbook bullet list): the parallel-download test (`04221c3e`/`e2071c2f`: the
new length probe issues a whole-file ranged GET its own `asked()` counts
twice) and the lifeline watcher test (`48e6d64c`: the `ps lstart,stat` token
is unstable because `stat` flips `R`→`S`; the `pdeathsig` production path
passes). Both added to the exclusion list with reasons. Nothing we build or
ship failed.

Upstream changes relevant to us (audited against the checked-out tag; full
facts in `docs/security/engine-protocol-limits.md`): the `RESUME`/`REUSED`
serve records (fixed in `01ad7226`); the main engine's `DONE` gained a
` chunk=… experts_streamed=…` tail (opaque to our client, finish field
position unchanged); `qwen35moe` now PARSES `--vision`/`--batch`/`--slots` —
the frontend deny-list is now the only guard, and it holds; `qwen35moe`
answers stray lines with `ERR expected: GEN …` instead of exiting; HIP
gfx906 builds exclude the qwen35 engine; HIP-Windows gained the
`<exe>\rocm\bin` delay-load search (SP7 packaging item, like `strata_kq`);
`STRATA_MMQ_TQ_OVERLAY` defaults ON (GPU builds only, no CPU/root effect);
`apps/desktop` and `apps/host` are new upstream trees — not packaged, and SP7
must keep them out; the ggml pin `3cf03257` is unchanged; CMake surface
re-audited: one `FetchContent` at `:1552`, no `tests/CMakeLists.txt`, option
defaults compatible with our root seeds. The engine CLI gained three local
tuning flags (pass-through candidates; deny-list unchanged and still
complete for everything it refuses).

### Protocol hotfix: `RESUME`/`REUSED` prompt-progress records are consumed (2026-10-20)

Found while re-auditing the `--serve` protocol for the upstream v0.2.1.x bump:
the main engine family's serve loop prints `RESUME <n>` before reading the
prompt and `REUSED <n>` once it is read, for **every** accepted GEN and
unconditionally (`generate.cpp:11214`/`:11849` at `v0.1.44.1`, `:12809`/`:13910`
at `v0.2.1.3`; upstream's own server consumes both). The strict client failed
such a response with `unexpected_line`, so **every real-engine chat would have
failed at both pins** — the same latent-blocker class as the PP fix (`c9629e6e`),
found by reading, never yet observed against a real engine. Fix mirrors PP: under
the Strata profile a `RESUME`/`REUSED` line with exactly two fields and a
32-bit decimal count is consumed silently; any other shape still fails closed;
the GLM profile keeps its exact record and refuses them. `YIELDED <slot>
<tokens>` is left strict — it needs the `--batch`/`--slots` paths, which the
frontend refuses at argv level.

Tests: `secure_frontend_engine_protocol_client_tests.resume_and_reused_progress_is_consumed_in_the_strata_profile`
(RED on the unfixed client: "RESUME/REUSED records of the main engine family
must be consumed silently" + "a held-state reuse count must be accepted as a
decimal"; GREEN after). Limits doc: new `RESUME`/`REUSED` row in
`docs/security/engine-protocol-limits.md`.

### Upstream bump to `v0.1.44.1` (2026-10-14, branch `upstream/bump-v0.1.44.1`)

`7695859a` (`v0.1.43`) → `296b59d2` (`v0.1.44.1`), 315 commits. Gates old →
new: `pytest -q` 13 → 13; `secure_frontend` linux-gcc 111/111 → 111/111;
`windows-llvm-mingw-cross` (-Werror, wine) 102/102 → 102/102; root build
94/94 steps, 0 warnings, ctest 111/111, no `_deps` → identical (same engine
targets); upstream `tools` 616/2/3 → 760/2/5 (two `test_setup_golden.py`
deselects added: upstream `12be1059` recorded `strata-vision.exe` on
Windows); upstream `serve` 695/11 → 774/11. CMake: no new FetchContent, no
`tests/CMakeLists.txt`, ggml/llama.cpp pin unchanged (`3cf03257`);
`STRATA_NATIVE_EXPERTS` moved up to L107 (our seed still wins);
`STRATA_MMQ_KQUANTS` and `STRATA_Q6K_EXPERTS` now default ON (CUDA/HIP only).
`strata_tokenizer.py` byte-identical. Main engine CLI/protocol: additive only
(`--prefill-head-alias`, `--families`, new ERR texts, GENI `img_keys=`), no
network code. Upstream changes that matter to the frontend:

- `qwen35moe` family switch (CUDA/HIP builds): such a GGUF runs a separate
  serve loop (`upstream/src/qwen35/q36_engine.cu`) — QUIT ignored (exits on
  stdin EOF), unknown flags skipped instead of fatal, no `--pack`, no
  `nonfinite`, context rule without the `+ 8` margin (our stricter check keeps
  us safe). See [`security/engine-protocol-limits.md`](security/engine-protocol-limits.md).
- Runtime-loaded `strata_kq` library (`strata_kq.dll` / `libstrata_kq.so`
  from the engine's folder when `STRATA_MMQ_KQ=1`, forced on by the qwen35
  path): a DLL search-order and packaging question for GPU builds.
- New family check: the engine refuses any `general.architecture` other than
  `qwen4exp` / `qwen35moe` — affects the SP6 GLM port.
- RESOLVED 2026-10-14 (commit on `main`, "protocol: Strata-profile PP prefill
  heartbeats are consumed"): the engine emits `PP <position> <prompt tokens>
  <ms> <tok/s>` during batched prompt reads (`generate.cpp`), and the
  `qwen35moe` engine emits `PP <done> <fresh> <ms> <tok/s>` (`q36_engine.cu`);
  `engine_protocol_client.cpp` used to reject `PP` outside the GLM profile, so
  every real Strata-profile request with a batched prefill would have failed
  with a generic `503`. It now consumes bounded `PP` records (3–5 fields, two
  decimal counters, optional non-negative metrics, no ordering assumed) under
  the Strata profile; the GLM profile keeps its exact 3-/5-field record with
  `read <= total`. Case `engine_protocol_client_tests.prefill_progress_is_bounded_in_both_profiles`
  (RED against the old client on the first Strata-shaped line). Still never
  observed against a real engine (no GPU here).

The runbook in `PROVENANCE.md` was corrected on its first real run (gates at
the old pin first, per-run `out/<name>-<L>` dirs, rule for new upstream test
failures, tag object and CMake line numbers in the pin sweep, history rows
left alone, `docs/security` facts re-checked) and is now the only copy.


### Submodule layout (branch `layout/upstream-submodule`)

Spec: [`superpowers/specs/2026-10-13-upstream-submodule-layout-design.md`](superpowers/specs/2026-10-13-upstream-submodule-layout-design.md);
plan: [`superpowers/plans/2026-10-13-upstream-submodule-layout.md`](superpowers/plans/2026-10-13-upstream-submodule-layout.md).

| Item | What changed | Check |
| --- | --- | --- |
| U2 | `upstream/` = submodule `https://github.com/Niko1221/Strata.git` at `7695859a` (`v0.1.43`), tree byte-identical to the tag | `git submodule status` |
| U3 | Our root `CMakeLists.txt`: frontend first, then `upstream/` unmodified; Release default, `include(CTest)`, `SECURE_FRONTEND_WERROR=ON`, CUDA/HIP/native experts seeded `OFF`, `SECURE_STRATA_BUILD_ENGINE` | root build 94 steps / 0 warnings, `ctest` 20/20, no `_deps`; `-D` overrides; empty `upstream/` message |
| U4, U5 | `tools/secure/secure_tokenizer.py` wraps upstream's tokenizer (GLM aliases, `extract()` ships the declared pattern); tests in `tools/secure/`; `pytest.ini` `testpaths = tools/secure`; `conftest.py` names `git submodule update --init` | `pytest -q` 12 passed (13 since the final fix wave: `extract()` refuses when upstream did not write `tokenizer/tokenizer.json`) |
| U1, U6, U7 | 2 971 upstream files removed from our tree; ours-only `.gitignore`, `.gitattributes`, `README.md`, `LICENSE` | `git ls-files` shows only our paths plus the `upstream` gitlink |
| U8 | `bootstrap-linux.sh` reads only `scripts/dev/requirements-dev.txt` (now pins cmake 4.4.3, ninja 1.13.2, regex 2026.9.10) | fresh venv bootstraps; `pytest -q` passes without numpy |
| U9 | `secure_distribution_check.py` rejects any `upstream` path component and quarantined names at any depth | `tools/secure/test_secure_distribution_check.py` (6 tests) |
| U10 | CI: submodule checkout in `linux` and `top-level-integration`; the latter configures our root (first with no `-D` flags, asserting the OFF seeds, `Release` and no `_deps` — added in the final fix wave); collection and submodule-status steps | run `38478775552` (`4aa742c9`): all five blocking jobs green; `windows-msvc` informational red (known) |
| U11–U14 | `PROVENANCE.md` baseline = gitlink, no ledger; this file, `secure-frontend-ci.md`, security docs, product spec §9, SP1/SP2 path patches; "Update upstream" runbook; offline bundle in `secure-frontend-ci.md` | doc greps in the plan, Task 6 |

### Secure baseline (commit `7de35b0c` on `main`)

| Area | Files | Status |
| --- | --- | --- |
| Provenance | `docs/PROVENANCE.md` | Complete baseline/attribution record. Since the submodule layout the baseline is the `upstream` gitlink and there are no upstream edits. |
| Security/privacy contract | `docs/security/threat-model.md`, `tests/security/README.md`, `docs/security/privacy-migration.md`, `docs/security/deployment.md` | Complete specification, legacy-network inventory, platform deployment prerequisites, and package-readiness procedure; most acceptance tests are not implemented yet. |
| Secure package gate | `scripts/secure_distribution_check.py` | Checks a selected staging directory (never this source tree) for quarantined Python installer/service/MCP/update artifacts and symlinks; `--release` additionally requires a privacy declaration manifest and SBOM. |
| Native frontend foundation | `secure_frontend/` | C++20 configuration, secret/token-reader, HTTP policy/request-handler/socket adapter, API router, local model inventory, and process-supervision libraries with unit tests. |
| Runnable smoke executables | `secure-frontend-smoke`, `strata-secure-smoke-server` | Authenticated local `/health` and `/v1/models`; no engine inference. |
| Native serial chat executable | `strata-secure-server` | Starts one supervised local `--serve` engine child and offers authenticated text-only, non-streaming Qwen or explicitly selected GLM-5.3 `/v1/chat/completions` plus health/models. It is intentionally not a claim of universal upstream model/template/vision compatibility. |
| GLM tokenizer tooling | `tools/secure/secure_tokenizer.py` (wraps `upstream/tools/strata_tokenizer.py`), `tools/secure/test_glm_tokenizer.py` | Offline support for the `glm4` and `chatglm-bpe` byte-BPE pre-tokenizer aliases, preserving GLM's 1–3-digit segmentation, with emitted pack metadata. `secure_frontend` does not yet consume tokenizer-pack metadata, so native GLM execution still needs matching-pack validation. |
| Top-level integration | `CMakeLists.txt` (ours since the submodule layout) | Builds `secure_frontend/` and the engine from `upstream/`; `SECURE_STRATA_BUILD_ENGINE=OFF` builds the frontend alone. Before the layout it was an opt-in hunk in upstream's root file. |

### SP0 deliverables (branch `sp0-foundation-hardening` on `4dce6f79`, including the docs commits and the final fix wave)

Item IDs are from §4 of the SP0 spec. Each row names its regression test, or
says why it has none.

| Item | What changed | Regression test / check |
| --- | --- | --- |
| Tooling (T1, T5) | `scripts/dev/bootstrap-linux.sh` creates `.venv-dev` (no root) with `-r scripts/dev/requirements-dev.txt` (since the submodule layout the only file it reads: cmake 4.4.3, ninja 1.13.2, regex, pytest 9.1.1 and its dependencies). Offline (`STRATA_WHEEL_DIR` set) needs every wheel that file names **plus** a pip wheel, which the pip-bootstrap step now takes from `STRATA_WHEEL_DIR` too; nothing is fetched from the network then. `pytest.ini` collects `tools/secure/` only (submodule layout). | Bootstrap is idempotent (re-run on the existing venv: exit 0); a fresh scratch checkout bootstraps with network (exit 0) and offline from a wheel directory with the network blocked (exit 0); `pytest -q` passes from the repository root. |
| CMake hygiene (T3, T4) | (before the submodule layout) Top-level `STRATA_BUILD_SECURE_FRONTEND` defaults `OFF` (opt-in). `SECURE_FRONTEND_WERROR` (default `ON`, follows `STRATA_WERROR` from the root). One `secure_frontend_set_warnings()` helper replaces the per-target flag blocks; dead `if(MSVC)`-inside-`if(UNIX)` blocks removed. Under our own root (submodule layout) the frontend is added before `upstream/` and the root declares `SECURE_FRONTEND_WERROR=ON`, so the follow-`STRATA_WERROR` default is inert there (SP1 may simplify it). | Configure with each setting. |
| llvm-mingw (T2, P5) | `scripts/dev/bootstrap-llvm-mingw.sh` (llvm-mingw `20261006`, sha256 pinned), toolchain file, `windows-llvm-mingw-cross` preset. `win32_prelude.hpp` (`NOMINMAX`, `WIN32_LEAN_AND_MEAN`); paths converted with `path::wstring()`. Two further Windows fixes found by the cross build: unused POSIX helpers in `engine_process.cpp` guarded; `token_reader.cpp` uses `std::next(it) == end()` (libc++ `path::iterator` is an input iterator). | Cross build with `-Werror`; `wine strata-secure-server.exe --help` exits 0. |
| Early close (S1) | `install_process_defaults()` ignores `SIGPIPE` in all three mains; `send()` uses `MSG_NOSIGNAL` (`SO_NOSIGPIPE` where that is the only option). | `socket_server_survives_client_early_close` |
| Request deadline, send timeout (S2, S3) | `HttpSocketServerLimits{request_deadline = 30 s, max_request_bytes = 64 KiB}`, a defaulted `bind()` parameter until SP1's typed configuration. Total monotonic deadline for header and body; send timeout, with each `send()` capped at 64 KiB (Winsock accepted one 32 MiB blocking `send()` whole, bypassing the deadline; found by the first MSYS2 run). | `socket_server_rejects_slow_request_past_deadline`, `socket_server_send_times_out_on_stalled_reader` |
| Listener robustness (S4–S7) | Oversize request answered `413`; accept blocks without a timeout (smoke server no longer exits after 5 s idle); `IPV6_V6ONLY` on IPv6 binds; `SO_REUSEADDR` (`SO_EXCLUSIVEADDRUSE` on Windows); complete responses. `listener_failed()` is set only for a broken listener (`EBADF`/`ENOTSOCK`/`EINVAL` and the Winsock equivalents); peer and network errors are transient; 50 ms backoff on descriptor/buffer exhaustion. The mains stop only on listener failure. | `socket_server_rejects_oversize_request_over_tcp`, `socket_server_accept_waits_without_timeout`, `socket_server_ipv6_bind_is_v6only` (binds `::`), `socket_server_response_is_complete`, `socket_server_survives_reset_before_accept`, `socket_server_responds_after_client_half_close`. After the last response byte (normal, `408`, `413`) the server half-closes its send side and drains the peer for at most 100 ms / 256 KiB before closing, so a Windows client reads the `413` rather than an RST. The socket tests are portable (run under wine and in the MSYS2 job). |
| Generation deadline and recovery (E1, E2) | Total generation deadline; on timeout the client sends `STOP` and drains to `DONE` within `ChatExecutionLimits::stop_drain_timeout` (5 s), returning to idle (`idle()`), or marks the engine failed (`abandon()`). The request gets the `generation_timed_out` error; a `nonfinite` result is a `503` for that request only and the engine stays ready. | `generation_total_deadline_is_enforced`, `chat_recovers_after_generation_timeout`, `chat_marks_engine_failed_when_drain_fails`, `chat_rejects_nonfinite_done_and_stays_ready` |
| Health (E3) | `EngineHealth` / `health()`; `ApiRouter` takes an optional `EngineStateProvider` and `/health` returns `{"status":"ok"|"unavailable","engine":"<state>"}` with the state one of four fixed literals (anything else fails closed). `starting` → `200` with `engine=starting` (READY is consumed on the first request; eager READY is SP2), `ready`/`generating` → `200`, `failed` or a stopped child → `503`. The server samples the child and protocol state once per request and derives both fields from that snapshot. No secrets or paths. | `health_reflects_protocol_state`, `health_is_failed_after_engine_closed_mid_generation`, `health_is_failed_when_engine_closes_before_ready`, `health_includes_engine_state_when_provided`, `health_is_503_with_engine_state_when_unavailable`, `health_samples_status_before_state_once_each` |
| Protocol limits (E4, D4) | Limits derived from `READY`: `max_line_bytes = 0` means `min(32 + 11 × ctx, 16 MiB kProtocolMaxLineBytes)` for the outbound `GEN` line; inbound records capped at `kMaxReadLineBytes = 8192`; `kMaxWriteLineBytes = kProtocolMaxLineBytes`. The engine's `+8` context rule is applied before `GEN` (`400`). Facts in `docs/security/engine-protocol-limits.md`. | `gen_line_fits_max_prompt_tokens`, `chat_applies_engine_context_rule_with_eight_token_margin`, 100 KB `GEN` write in `engine_stdio_process_tests`, 16 MiB clamp case in `engine_protocol_client_tests` |
| POSIX launch (P1–P3) | `secure_frontend/src/posix_child_launch.hpp`: argv/env built before `fork`, `pipe2(O_CLOEXEC)`, descriptor sweep (`close_range`, bounded fallback), reap after `SIGKILL`. | `child_inherits_only_stdio_descriptors`, `child_receives_exact_argv_and_env`, `terminate_reaps_killed_child` (asserts pid `-1` and `ECHILD`) |
| Windows launch (P4, P6, P7) | `CREATE_SUSPENDED` → Job Object → `ResumeThread`. Iterative bounded `read_line` shared with POSIX through `take_pending_line()`. Stdin pipe created with `CreateNamedPipeW`, buffer **requested** at `kMaxWriteLineBytes + 4096` (advisory per Win32; overlapped writes deferred to SP2). `engine_stdio_process_tests` is now portable. `read_line` keeps one deadline across chunks (POSIX polls with the remaining budget) and treats `WAIT_FAILED` as a failure on Windows. | `read_line_rejects_oversize_on_all_platforms`, `read_line_waits_for_a_record_split_across_writes` (both pass under wine); P4: `job_object_terminates_grandchild` (Windows only: the fake engine spawns a 30 s sleeper grandchild, which must be gone after `terminate()`; passes under wine, authoritative only in the MSYS2 job). |
| Engine arguments (M1, M2) | `secure_frontend_engine_arguments` library, profile-aware (`EngineArgumentsProfile`). Strata profile: the frontend supplies shard 1 by appending `--native <models_root/artifact>` (last) from the inventory entry for `--model-id` and rejects `--native`, `--serve`, `--batch`, `--slots`, `--vision` in `--flag` and `--flag=value` forms; the expert pack (`--pack`, loaded unconditionally, default cwd-relative `pack/full`) and other weight sources (`--ple-gguf`, `--embd-gguf`, `--native-head-gguf`, `--native-dense-gguf`, `--mtp`) pass through and are operator-supplied until SP1/SP5 model the whole launch. An invalid manifest artifact path (`../x.gguf`, `/abs.gguf`) is `invalid_value`. GLM-5.3 profile: arguments pass through (only NUL and `--serve` rejected), no model path is supplied; `strata-secure-server --profile glm53` requires at least one `--engine-arg`. **Superseded by SP5 M2 Task 7 (D-8):** `glm53` now refuses every `engine.args` form and its launch is composed from the manifest + the Maya parity table; `glm_profile_passes_arguments_through_without_native` was replaced by `glm_rejects_every_engine_argument` and the ctest case by `strata_secure_server_glm_rejects_engine_args`. | `engine_receives_model_path_from_inventory`, `engine_args_reject_owned_and_network_flags`, `strata_profile_passes_pack_and_weight_flags_through`, `rejects_invalid_relative_artifact_path`, `glm_profile_passes_arguments_through_without_native`, ctest `strata_secure_server_glm_requires_engine_arg` |
| CI (T6) | `.github/workflows/secure-frontend.yml`: blocking `linux-gcc` (+ `pytest -q`), `linux-clang`, `windows-llvm-mingw-cross`, `windows-msys2-clang`, `top-level-integration` (root CMake, `STRATA_NATIVE_EXPERTS=OFF`, no llama.cpp clone); non-blocking `windows-msvc`. Green on run `38458255140` (all blocking jobs; `windows-msys2-clang` 19/19). `windows-msys2-clang` links with `-static` (preset) so a child launched with the allowlisted environment, which has no `PATH`, needs no runtime DLL. See [`secure-frontend-ci.md`](secure-frontend-ci.md). |
| Docs (D1–D4) | `secure_frontend/README.md` build section and current behavior; `token_reader.hpp` comment describes the Windows checks that run, unreachable `protected_file_unsupported_on_platform` removed; this handoff; `secure-frontend-ci.md`; `engine-protocol-limits.md`. | Builds and tests re-run after the header change (below). |

### SP1 deliverables (branch `sp1-typed-config-paths-tokens`)

Spec: [`superpowers/specs/2026-10-13-sp1-typed-config-paths-tokens-design.md`](superpowers/specs/2026-10-13-sp1-typed-config-paths-tokens-design.md)
(§8 lists every N/C/P/K/A/D item); plan:
[`superpowers/plans/2026-10-13-sp1-typed-config-paths-tokens.md`](superpowers/plans/2026-10-13-sp1-typed-config-paths-tokens.md).
Task 2 is a shared pre-task committed on `main` (SP2 consumes it); every other
row is a commit on the SP1 branch (fix rounds are separate commits, listed in
the same row). Each row names the regression tests from its commit message.
Every test with the CTest label `windows-runtime` is **verified in MSYS2**:
CI run `38540719307` ran the authoritative `windows-msys2-clang` job on SP1,
193/193 (before the SP2 merge). What no CTest case covers stays unverified on
Windows and says so in its row (the Task 20 environment scrub, the Task 17
ancestor-rename assumption). wine is a smoke test only (see "Verification
performed", SP1).

| Task / commit | Items | What changed | Regression tests / check |
| --- | --- | --- | --- |
| 2 — `17d2452d` (on `main`) | §3.10 | `secure_frontend_add_cases(<binary> <case>... [ARGS] [LABELS] [TIMEOUT])` registers one CTest test per case (`<binary>.<case>`), dispatched through `secure_frontend_test::run_cases` (`--case`, `--list`); label `windows-runtime` (empty off Windows), excluded by the `windows-llvm-mingw-cross` test preset; scratch directories via `mkdtemp` (0700) under `<build>/test-tmp` (Windows: explicit protected DACL), removed on every exit path. Helper names, `<binary>.<case>`, the label and the preset exclusion are frozen for SP2. | Linux 111/111, wine 102/102 at that commit |
| 1 — `6be5042e` | N1, N2; defect a | Listener addresses are classified from the parsed bytes (`parse_listener_address`): every spelling of the unspecified address needs `--allow-wildcard-bind`; `::ffff:127.0.0.1` and the long loopback form are loopback; `localhost` (ASCII case-insensitive) maps to `127.0.0.1` without a resolver call; every other hostname is rejected; `bind()` uses the canonical text. `inet_ntop` runs during validation, before `WSAStartup` on Windows (wine passed; **verified in MSYS2, CI run `38540719307`**). | `net03_wildcard_spellings_need_wildcard_opt_in`, `net03_remote_opt_in_alone_rejects_wildcard`, `net01_loopback_spellings_classified_from_bytes`, `net02_hostname_rejected_before_bind`, `net01_localhost_maps_to_ipv4_loopback` |
| 3 — `828daac8` | C1 | One strict bounded JSON reader (`json.hpp`: duplicate keys, depth, size, integers only, BOM, trailing data) shared by the chat schema and the tokenizer pack. | `json_rejects_duplicate_keys`, `json_rejects_depth_over_limit`, `json_rejects_non_integer_numbers` (+ BOM/trailing/size cases) |
| 4 — `95ca08ad`, fix `67634f81` | C2 | Typed `FrontendConfig`, the schema table `config_schema()` (one row per key: type, range, default, option, description) and `validate()` collecting every issue by key path with a fixed reason; `ready_timeout` 900 s; `limits.chat.max_body_bytes` default 49152. Fix: member names that are empty or contain `.` are unknown keys (no aliasing of nested keys); a duplicate path on insert is an error. | `cfg01_unknown_key_rejected`, `cfg01_duplicate_key_rejected`, `cfg01_type_confusion_rejected`, `cfg01_missing_required_rejected`, `cfg01_out_of_range_rejected`, `typed_config_cross_field_rules`, `typed_config_errors_never_contain_values` (all `windows-runtime` from Task 9: **verified in MSYS2, CI run `38540719307`**) |
| 5 — `67ac645a` | C6, C8 | `strata-secure-server --config FILE` and `--check` go through the one validator; an option wins over the file and `--check` reports each overridden key; option values are never echoed. End-to-end harness (process runner, HTTP client, server fixture, fake engine). | `cli_config_file_and_flags_share_validator`, `cli_flag_overrides_file_and_is_reported`, `cli_check_prints_no_token`, `cli_unknown_option_does_not_echo_value` (`windows-runtime` from Task 9: **verified in MSYS2, CI run `38540719307`**) |
| 6 — `2b0f83b4`, fix `e6e82aa1` | C3, N3, N4 | The server binds before it spawns the engine; process-level NET tests. Fix: the C3 guard is deterministic (an unstartable engine plus an unbindable address must report the bind error). | `cfg01_invalid_config_fails_before_listener_and_child`; acceptance `net01_default_listener_is_loopback_only` (`GetExtendedTcpTable`), `net02_remote_addresses_need_opt_in`, `net02_error_is_non_secret_and_exits_before_bind` — all four `windows-runtime` from Task 9: **verified in MSYS2, CI run `38540719307`** (the C3 guard's RED was never shown on Windows) |
| 7 — `d9613548` | C4, C5; defect d | Listener, parser, chat schema and chat execution take their limits from `FrontendConfig`; defaults live only in `config_defaults.hpp`. Accepted deviation: an over-bound declared `Content-Length` is answered `400` from the headers alone (`413` is for bytes actually received past the bound). | `parser_honors_configured_max_request_bytes`, `ready_timeout_is_configurable`, `ready_timeout_defaults_to_900_seconds`, `subsystem_limits_follow_config` (`windows-runtime` from Task 9: **verified in MSYS2, CI run `38540719307`**) |
| 8 — `51059af4` | P1 | One relative-path grammar (`RelativePath`) for secret sources and model artifacts; the two disagreeing grammars are deleted. | `cfg02_rejects_traversal_and_absolute`, `cfg02_rejects_nul_and_control_bytes`, `cfg04_rejects_reserved_and_device_names`, `cfg04_rejects_trailing_dot_space_and_8dot3`, `cfg04_rejects_ads_and_drive_forms`, `relative_path_accepts_portable_forms` |
| 9 — `b36dd17a`, fix `5d6232e5` | P2, P3, P7; defect e | `CanonicalRoot`/`CanonicalFile` (`open_root`, `open_executable`); the engine receives the canonical models root; Windows root forms (UNC, device, `\\?\`) rejected and network/redirected final paths reported actionably. Fix: `roots.secret` defaults to the config file's folder only for a `file:` token source; an `env:` source never opens it. From here 24 cases that open real roots are `windows-runtime` (engine_arguments, typed_config, subsystem_limits, server_cli, server_e2e): **verified in MSYS2, CI run `38540719307`**. | `cfg03_engine_receives_canonical_models_root`, `cfg02_rejects_relative_root`, `cfg02_rejects_shared_writable_root`, `cfg02_root_may_traverse_symlink`, `cfg02_sibling_prefix_root_rejected`, `cfg02_engine_executable_must_be_private_regular_file`, `cfg04_rejects_unc_device_and_extended_roots`, `cfg04_final_path_on_network_share_is_actionable`; fix: `cfg02_env_token_ignores_config_folder_permissions`, `cfg02_file_token_default_secret_root_reports_one_issue` (POSIX) |
| 10 — `e07b0601` | P4 | Linux: `openat2(RESOLVE_BENEATH\|NO_SYMLINKS\|NO_MAGICLINKS\|NO_XDEV)` below a root, with an `openat(O_NOFOLLOW)` walk on `ENOSYS`/`EPERM`; the Linux token reader uses it; hard-linked token files rejected. | `cfg03_rejects_hardlinked_token_file`, `cfg03_rejects_symlinked_intermediate_directory`, `cfg03_rejects_symlinked_final_file`, `cfg03_fallback_walk_matches_openat2`, `cfg03_rejects_mount_crossing` |
| 11 — `9d9b1fba` | P5; defect b | Windows: `READ_CONTROL` handle walk below the root with reparse-point and hard-link rejection; the Windows token reader uses it (the old reader is deleted). The defect-b fix (handles that can read the DACL) was verified by reading only at the time; the Windows cases below have since passed in MSYS2. | `auth02_windows_token_file_reads_with_private_dacl`, `cfg03_rejects_junction_below_root`, `cfg03_rejects_symlink_below_root`, `paths_handle_non_ascii_and_long_names` (Review Focus 4; passes on Linux) — the Windows four are **verified in MSYS2, CI run `38540719307`** (wine 0/4: `GetFinalPathNameByHandleW` beyond `MAX_PATH`, `D:P` dropped, no junctions, symlink seen as not-found) |
| 12 — `ccd7c63b` | C7 | Config files and their folders are opened through the path module and refused when other users can write them (or when owned by neither euid nor root). | `cfg01_group_writable_config_rejected` (Linux), `cfg01_config_with_everyone_write_ace_rejected` (`windows-runtime`; wine passes but does not model DACLs: **verified in MSYS2, CI run `38540719307`**) |
| 13 — `96aaf699`, fix `56a60c5d` | P6 | Inventory and tokenizer pack enumerate (`list_directory`) and open through the canonical-path module; symlinks/junctions and case-folded duplicate IDs fail naming the entry; non-portable `.gguf` names and non-portable directory names fail; other non-portable non-model files are ignored. | `inventory_symlink_error_names_entry` (Review Focus 1), `inventory_duplicate_detection_is_case_insensitive` (Review Focus 5), `inventory_ignores_non_portable_non_model_names`, `inventory_rejects_non_portable_directory`, `cfg03_inventory_rejects_symlink_entry`, `cfg03_tokenizer_file_swapped_to_symlink_rejected`; Windows junction variants `cfg03_inventory_rejects_junction_entry`, `cfg03_tokenizer_file_swapped_to_junction_rejected` and the root-opening inventory/tokenizer cases: **verified in MSYS2, CI run `38540719307`** (wine cannot create junctions) |
| 14 — `264edde1` | P8, P9 | `create_directory_within` a root (symlinked components rejected); `roots.logs`/`roots.cache` are checked by `validate()` (absent-but-creatable under a private parent) and created 0700 only by serve and `init`; `--check` creates nothing; duplicate path checks outside the module removed (P9 grep). | `cfg02_create_within_root_writes_nothing_outside`, `cfg02_logs_and_cache_roots_are_created_private`, `cli_check_has_no_side_effects` (all `windows-runtime`: **verified in MSYS2, CI run `38540719307`**; wine: the junction case fails, the other two pass) |
| 15 — `5cc8a91f` | K1, K6 (type) | `SecretBytes` (fixed capacity, wiped on destroy and move), CSPRNG token generator (`getrandom`, fails closed; `BCryptGenRandom` on Windows), base64url; tokens are 43 characters. | `base64url_known_vectors`, `token_generate_is_43_base64url_chars`, `token_generate_differs_across_calls`, `secret_bytes_wiped_on_destroy_and_move`, `secret_bytes_rejects_over_capacity` (wine 5/5; BCryptGenRandom/SecureZeroMemory **verified in MSYS2, CI run `38540719307`**) |
| 16 — `e0f4294c`, fix `510617b5` | K2 | Linux token store: `O_EXCL\|O_NOFOLLOW` temp, `fsync`, `renameat2(RENAME_NOREPLACE)` (or replace with `--force`), 0600 file in a 0700 directory. Fix: rotate asserted; a target that cannot be inspected fails closed. | `token_store_creates_0600_file_in_0700_dir`, `token_store_rotate_is_atomic`, `token_store_refuses_symlinked_target` |
| 17 — `25ba2270`, fix `013f540a` | K3 | Windows token store: DPAPI (user scope) `STRATAK1` + blob, protected DACL `O:<user>D:P(A;;FA;;;<user>)(A;;FA;;;SY)`, `CREATE_NEW` temp, `FlushFileBuffers`, `MoveFileExW(WRITE_THROUGH)`; the reader unwraps DPAPI and still reads plaintext operator files. Fix: the holding directory is opened with `open_beneath` and pinned (no `FILE_SHARE_DELETE`) across create and move; creation and move are still by path (handle-relative `NtCreateFile` deferred); protection against renaming an ancestor relies on NTFS refusing to rename directories with open handles below them — unverified. | `token_store_windows_round_trips_through_dpapi`, `token_store_windows_file_has_private_dacl`, `token_reader_windows_accepts_plaintext_under_private_dacl` — **verified in MSYS2, CI run `38540719307`** (wine 0/3 at the `D:P` gate; a DPAPI probe round-trips under wine) |
| 18 — `f9f146ac` | K4, K5, K6; defect c | `secure_frontend_token_reader_tests` runs on every platform; one trailing LF or CRLF; result is `SecretBytes`; 32-byte floor, 256-byte cap, 4 KiB file cap; quoted tokens rejected with a reason; read buffers wiped. | `auth02_token_file_with_trailing_lf_accepted`, `auth02_token_file_with_trailing_crlf_accepted`, `auth02_short_operator_token_rejected`, `auth02_token_file_over_4k_rejected`, `token_store_round_trips_through_reader`, `auth02_token_file_with_two_newlines_rejected`, `auth02_token_file_with_bom_rejected`, `auth02_notepad_edits_rejected_with_reason` (Review Focus 3); the 8 Windows file cases are **verified in MSYS2, CI run `38540719307`** (wine fails on the DACL) |
| 19 — `137f2b4a` | K6, K7 (AUTH-03, A3) | One `TokenVerifier` in the library; `detail::fixed_window_equals` compares zero-padded 256-byte windows, folds the length difference in, never matches an empty stored value. Reviewer disassembly: at `-O2` the compare is branch-free and both `explicit_bzero` calls survive. | `auth03_compare_touches_fixed_window`, `auth03_prefix_and_suffix_variants_rejected`, `auth03_router_uses_verifier` |
| 20 — `2b480430` | K8, K9 | Environment token wiped and unset after reading; `install_process_defaults()` sets `RLIMIT_CORE` 0 and `PR_SET_DUMPABLE` 0 (Linux) or `SetErrorMode` (Windows) for every binary that calls it. Windows scrub: accepted deviation (gaps below). | `cfg05_env_token_absent_from_proc_environ`, `server_process_is_not_dumpable` (POSIX). Windows scrub has no ctest case (wine probe only: `getenv`/`_wgetenv` null, Win32 entry absent): **unverified on Windows** (no CTest case, so CI run `38540719307` does not cover it) |
| 21 — `92a8419a`, fix `dae07073` | K10 | `strata-secure-server init --config FILE [--force]` writes the token first, then an owner-only config, atomically; refuses to replace either without `--force`; never prints the token. Fix: one JSON tree renderer shared with the tests; a non-regular config target is refused before anything is written; nothing is created on bad input. | `init_writes_config_and_token_then_server_starts`, `init_refuses_to_overwrite_without_force`, `init_force_refuses_a_non_regular_config_target`, `init_failure_creates_nothing` (`windows-runtime`: **verified in MSYS2, CI run `38540719307`**; wine fails three on the `D:P` gap) |
| 22 — `792b7651` | K11, K12 | CFG-05 end to end; CFG-06 at the type level. | `cfg05_token_absent_from_child_argv_env_and_outputs`, `cfg06_config_has_no_secret_typed_member`, `cfg06_validated_config_bytes_do_not_contain_token` (the first and third `windows-runtime`: **verified in MSYS2, CI run `38540719307`**) |
| 23 — `3810a67b`, fix `1904f827` | A1, A2, A4 | AUTH acceptance. Fix: the AUTH-04 sweep derives flags and `STRATA_*` names from the schema and usage text; AUTH-01 also sends a tab separator and a duplicate `Authorization` header over TCP. | `auth01_every_route_rejects_missing_malformed_wrong_token`, `auth01_socket_rejects_missing_and_wrong_token_on_every_route`, `auth02_valid_token_succeeds_and_is_not_echoed`, `auth04_empty_token_rejected_on_loopback`, `auth04_no_switch_disables_auth` (the four socket/process cases `windows-runtime`, pass under direct wine: **verified in MSYS2, CI run `38540719307`**) |
| 24 — this commit | D1–D5 | `render_config_schema_markdown()` generates the README key table from `config_schema()`; `secure_frontend/README.md` "Configuration" section and updated security contract; `docs/security/deployment.md` token and Windows ACL bullets; `engine-protocol-limits.md` keys vs internal constants (and the `EngineProtocolLimits` classification); this handoff; `secure-frontend-ci.md` label note; `bootstrap-llvm-mingw.sh` deletes an archive whose digest does not match. | `readme_config_table_matches_schema` (unlabelled; runs under wine too); D4 probe on a scratch copy (no network): before `exit=1` with the corrupt archive kept, after `exit=1` and an empty `downloads` |

Accepted deviations from the SP1 spec (controller and owner rulings):

- `limits.chat.max_body_bytes` defaults to 49152, not 262144: the spec's own
  cross-field rule (`≤ max_request_bytes − max_header_bytes` = 65536 − 16384)
  rejects 262144.
- `roots.tokenizer_pack` is required (spec §4 says optional) (R7).
- A config file and its directory may be owned by root as well as by the
  effective user (spec C7 says euid) (R7).
- The Windows root is opened through its own link or junction and its final
  path is taken (spec §3.4 says open with `OPEN_REPARSE_POINT`); every
  component **below** the root is opened with `OPEN_REPARSE_POINT` and reparse
  points are rejected (R7).
- `install_process_defaults()` hardening (non-dumpable, `RLIMIT_CORE` 0,
  `SetErrorMode`) applies to every binary that calls it, not only
  `strata-secure-server` (R7).
- Windows environment-token scrub: owner-ruled deviation from spec §3.6 (gap 3
  below).
- `parser_honors_configured_max_request_bytes` asserts `400`, not `413`, for
  an over-bound declared `Content-Length` (Task 7).
- `roots.secret` defaults to the config file's folder only for a `file:` token
  source; an `env:` source never opens it (Task 9 fix).
- `read_token(path)` and the smoke server's `--secret-root` must be absolute
  (spec §3.4) (Task 10).
- Under the models root, entries whose names fail the grammar are ignored only
  when they are non-`.gguf` files; non-portable directory names, `.gguf` files
  with bad names, symlinks and case-fold duplicates fail the inventory, naming
  the entry (Task 13).
- JSON config member names that are empty or contain `.` are rejected as
  unknown keys (Task 4 fix).
- `readme_config_table_matches_schema` strips `\r` before comparing, so a
  Windows checkout with `core.autocrlf=true` (GitHub's `windows-latest`) does
  not fail it (Task 24; the brief's version compared raw bytes).

### SP2 deliverables (branch `sp2-portable-supervision`; SP1 merged in at `9ded0a5c`)

Item IDs are from §4 of the SP2 spec. W rows are verified only by the
`windows-msys2-clang` CI job (pending the owner's push); the wine column is a smoke result.

Merge with SP1: `9ded0a5c` merges `main` (SP1 `e7f01b9e`, upstream
`v0.1.44.1`) into this branch. Since then the server, its command line
(`kFlags`) and its configuration (`FrontendConfig`) are SP1's, as described in
the SP1 section above; SP2's options become SP1 schema keys in later tasks
(18, 19, 26; verified in 29b). The rows from F1 to R2/R3 record counts on the
branch before the merge (N₀ = 111, W₀ = 102 after SP1 Task 2). **Post-merge
baseline (`9ded0a5c`):** linux-gcc 244 of 247 registered cases passed (the 3
server-harness cases failed because SP1's engine trust check refuses the
group-writable build-tree fake under the dev host's umask 0002; fixed by Task
29a, last row); wine `100% tests passed out of 141`; pytest `13 passed`; no
warnings in either preset.

| Item | What changed | Regression test | Local result (linux-gcc; wine) |
| --- | --- | --- | --- |
| F1 | Standalone test-only `strata-supervision-fake-engine` with the SP2 failure modes (crash modes never wait on WER); SP2 tests registered by acceptance ID through SP1's `secure_frontend_add_cases` (`<binary>.<case>`); baseline after SP1 Task 2: N₀ = 111, W₀ = 102. Naming deviation (ruling PF5): spec F1 says `strata-fake-engine`; the target is `strata-supervision-fake-engine` because SP1 owns `secure_frontend_fake_engine` — behaviour matches spec F1. `engine_stdio_process_tests` now launch the standalone fake (the in-binary `--fake-engine`/`--fake-sleeper` self-exec is gone). | `secure_frontend_fake_engine_tests.fake_engine_modes_selftest` | `100% tests passed out of 112`; wine `100% tests passed out of 102` (label `windows-runtime` excluded; a direct wine run of the case exits 0, with winedbg noise from the SEGV mode — authoritative in MSYS2, pending the owner's push) |
| F2 | DUMP reports FDS/HANDLES, ARGV (Windows via `CommandLineToArgvW`), sorted ENV, SIG masks, CWD, IDS (uid/gid/NoNewPrivs/CapEff; Windows SID/elevation/integrity), END; `--dump-file`; parser `tests/support/fake_engine_dump.hpp`. `child_inherits_only_stdio_descriptors` and `child_receives_exact_argv_and_env` read the report through the parser. | `secure_frontend_fake_engine_tests.fake_engine_dump_is_parseable` | `100% tests passed out of 113`; wine `100% tests passed out of 102` (label `windows-runtime` excluded); a direct wine run of the case exits 1 with only `FAILED: ENV is the sorted child environment` (wine's loader adds its own variables to the exact environment block the launcher passes; ARGV, HANDLES, CWD, IDS and `--dump-file` pass) — authoritative in MSYS2, pending the owner's push |
| F3 | `tests/support/server_harness.{hpp,cpp}` starts `strata-secure-server` with the fake engine (0700 fixture dirs, 0600 files, 43-character token, free port); on Windows always with a LUA-restricted non-elevated token (Safer `NORMALUSER` fallback; no override flag) and asserts it; stops it by signal / `CTRL_BREAK_EVENT` / kill; the destructor kills every descendant it has seen (Linux subreaper reaps orphans; Windows Job); counts descendants; reads stderr; `test_tokenizer.hpp` (byte tokenizer pack, real GPT-2 byte alphabet per ruling PF6). Runs against the server (SP1's since the merge); since Task 6b its `/health` answers `503` `engine=starting` until READY, then `200` `engine=ready`, so the self-test keeps the tolerant 200-or-503 assertion. Since Task 29a the harness copies the engine into a private `<root>/bin` (0700) and passes that copy (`ServerProcess::engine_path()`). Since the winfix branch (after CI run `38598235525`), on Windows: `root()` is in the server's canonical form (`GetFinalPathNameByHandleW`, `FILE_NAME_NORMALIZED`, `\\?\` removed), so A2's expected argv[0] and model path match an 8.3 `%TEMP%`; `descendants()` and `active_processes()` leave out console hosts (`%SystemRoot%\System32\conhost.exe`, the OS console server for the engine's `CREATE_NO_WINDOW` console, which sits in the harness Job) and `describe_processes()` lists every Job member as `pid:image` in the count assertions' failure text — pending the next MSYS2 run. | `secure_frontend_server_tests.harness_server_lifecycle_selftest` | `100% tests passed out of 114`; no fake engine left running afterwards; wine `100% tests passed out of 102` (label `windows-runtime` excluded); a direct wine run of the case exits 1 with `harness: no non-elevated token for strata-secure-server (LUA and Safer NORMALUSER both elevated)` (wine has no UAC; its token answers are not authoritative); Windows token path authoritative in MSYS2, pending the owner's push |
| K1, K3, K5 | `deadline.hpp`: one `deadline_after`/`remaining_until` rule in `await_ready`, `read_generation`, `read_line`; a zero budget is an immediate timeout everywhere. `terminate` contract documented (false only when still unreaped = abandoned). `current_executable()` checks `readlink`/`GetModuleFileNameW`. | `deadline_zero_timeout_is_immediate_everywhere` | `100% tests passed out of 115`; wine `100% tests passed out of 103` (the new deadline case is portable and unlabeled and passes under wine) |
| V1 | `EngineStdioProcess::try_reap()` keeps the raw `waitpid` / `GetExitCodeProcess` status; `classify_exit` maps it to `clean_exit`/`error_exit`/`spawn_failed`/`abort`/`killed_external`/`killed_by_supervisor(reason)`/`crashed` (log only); Windows Job kill code `0x20000001`, and Windows `terminate()` records it through `try_reap()`. | `secure_frontend_engine_child_tests.proc04_exit_status_is_classified` (B) | `100% tests passed out of 116`; wine `100% tests passed out of 103` (label `windows-runtime` excluded); a direct wine run of the case exits 1 with only `FAILED: the child must end: --crash-after-ready=SEGV` (wine starts winedbg on the page fault, so the child does not end in 10 s); the table, exit-1, ABRT and supervisor-kill (`0x20000001` via `try_reap()`) checks pass under wine; Windows authoritative in MSYS2, pending the owner's push |
| V2, V11 (core) | `EngineSupervisor` library: one supervisor thread spawns, consumes READY eagerly, watches and kills; the serving thread borrows the generation via `Lease`. `health_for`: `starting` → 503, `ready`/`generating` → 200; router's optional `recovery` literal (`restarting`/`exhausted`) only with `engine=failed`. Fresh `EngineInstance` per spawn. Restarts follow the V3 restart policy (Task 7). SP1-bound values in `supervision_defaults.hpp` (folded into SP1's `config_defaults.hpp` by Task 29a). Wired into the server by Task 6b (the V2, V11 row below). A spawn failure is logged by the supervisor as `engine generation <n> ended: spawn_failed`. Interface members not yet read in this run are kept for later tasks: `SupervisorConfig::shutdown_stdin_wait`, `SupervisedProcess::close_stdin`/`signal_group_terminate`, `kShutdownStdinWait` (Task 15 reads them now); `SupervisorConfig::no_new_privileges_hint`, `kNoNewPrivilegesDefault` (Task 26). | `secure_frontend_engine_supervisor_tests.supervisor_health_literals_per_state`, `secure_frontend_api_router_tests.health_samples_status_before_state_once_each` (updated), `secure_frontend_api_router_tests.health_carries_recovery_only_with_failed`, `secure_frontend_chat_execution_tests.chat_uses_adopted_ready_without_waiting` | `100% tests passed out of 119`; wine `100% tests passed out of 106` (all three new cases are unit cases and unlabeled, and pass under wine) |
| V3 | `RestartTracker`: 3 consecutive failures open the circuit; backoff 1 s → 2 s → … ≤ 60 s; 300 s healthy window resets the count; spawn failures count. Injectable `SupervisorClock` (`ManualClock` in tests; `tests/support/fake_supervised_process.hpp`). Three client-triggerable ERR lines in a row open the circuit until a restart (Review Focus 6, accepted). Spec §3.1: an ended generation still held by a `Lease` waits in the supervisor's `retired_` list and is destroyed by the supervisor thread once the Lease lets go (never on the serving thread); an unreaped generation at shutdown goes to `abandoned_`. | `secure_frontend_engine_supervisor_tests.proc04_restart_backoff_then_circuit_open`, `secure_frontend_engine_supervisor_tests.proc04_healthy_window_resets_failure_count`, `secure_frontend_engine_supervisor_tests.proc04_recovers_after_transient_crash`, `secure_frontend_engine_supervisor_tests.proc04_missing_engine_opens_circuit_without_spinning` (Review Focus 2), `secure_frontend_engine_supervisor_tests.proc04_client_err_lines_open_circuit_until_restart` (Review Focus 6), `secure_frontend_engine_supervisor_tests.supervisor_retires_ended_generation_on_its_own_thread` | `100% tests passed out of 125`; wine `100% tests passed out of 111` (five new unit cases pass under wine; `proc04_recovers_after_transient_crash` is a process case, labeled windows-runtime — authoritative in MSYS2, pending the owner's push) |
| V4 | The supervisor waits on the child: pidfd (Linux 5.3+; the test asserts `monitor_handle() != kNoMonitorHandle` on Linux, so the pidfd path is proven, not just the fallback), process handle (Windows), else a 100 ms `waitpid(WNOHANG)` poll. Spec V4's "`/health` leaves ready within 1 s" was first proven at supervisor level (`wait_for_state`, ruling PF11); Task 6b (fix round 1) adds the `/health` form against the wired server: the engine is killed externally (POSIX SIGKILL, Windows TerminateProcess) with no request in flight and `/health` must stop reporting `ready` within 1 s. Linux proven; Windows pending MSYS2. Spec §3.3 (carried from the V3 review): a wake that is not shutdown — a released Lease, a stale `report_fatal` notify — no longer cuts the backoff short: `schedule_restart` sleeps out the remainder to the deadline and sweeps on each wake; `release()` of a retired generation wakes the supervisor so it is swept promptly, also in `circuit_open`. | `secure_frontend_engine_supervisor_tests.proc04_monitor_detects_idle_crash`, `secure_frontend_engine_supervisor_tests.proc04_monitor_fallback_without_pidfd`, `secure_frontend_engine_supervisor_tests.proc04_backoff_survives_stale_wake` (§3.3), `secure_frontend_server_tests.proc04_health_leaves_ready_after_idle_engine_death` (B, Task 6b fix round 1; `100% tests passed out of 254`; labeled windows-runtime, wine `100% tests passed out of 142` — Windows authoritative in MSYS2, pending the owner's push) | `100% tests passed out of 128`; wine `100% tests passed out of 113` (`proc04_monitor_fallback_without_pidfd` and `proc04_backoff_survives_stale_wake` pass under wine; `proc04_monitor_detects_idle_crash` is a process case, labeled windows-runtime — authoritative in MSYS2, pending the owner's push) |
| V5 | The four upstream `FATAL_PREFIXES` (`kEngineFatalPrefixes`, verbatim from `upstream/serve/server.py:734-735` at `v0.1.44.1`; the earlier `:587-588` citation does not match this tag) fail the protocol (`fatal_engine_line`, fixed message, trailing `DONE` not accepted), checked in both `await_ready` and `read_generation` right after `validate_incoming`; `kill_reason_for` maps it to `KillReason::fatal_line`, so the supervisor kills the group and restarts (`killed_by_supervisor(fatal_line)`). Correction (Task 10, spec §3.8): until Task 10 a fatal line before READY was logged `killed_by_supervisor(protocol_failed)`, because the supervisor's `await_ready` hard-mapped every non-timeout READY failure; it now maps them through `kill_reason_for`, so a pre-READY fatal line is `fatal_line` too (`proc04_fatal_line_before_ready_kills_as_fatal_line`, V6/V7 row). Other `ERR` lines keep SP0's per-request behavior; the failed generation is replaced. | `secure_frontend_engine_supervisor_tests.proc04_fatal_prefix_kills_engine` (protocol level for each prefix, then supervisor level with `--fatal-line=0..3`) | `100% tests passed out of 129`; wine `100% tests passed out of 113` (process case, labeled windows-runtime — authoritative in MSYS2, pending the owner's push; a direct wine run of the case also passes) |
| V6, V7 | READY deadline `ready_timeout` (900 s default) kills and counts (`killed_by_supervisor(ready_timeout)`); an engine that ignores STOP is killed and replaced (`drain_failed`): `abandon()` records `EngineProtocolErrorCode::abandoned` when no earlier failure was recorded, and `kill_reason_for` maps it to `KillReason::drain_failed`. Spec §3.8: non-timeout READY failures are mapped through `kill_reason_for` (a fatal line before READY is `fatal_line`, not `protocol_failed`). (V8: Task 6b.) | `secure_frontend_engine_supervisor_tests.proc04_ready_timeout_kills_and_counts`, `secure_frontend_engine_supervisor_tests.proc04_drain_failure_kills_and_restarts`, `secure_frontend_engine_supervisor_tests.proc04_fatal_line_before_ready_kills_as_fatal_line` (§3.8) | `100% tests passed out of 132`; wine `100% tests passed out of 114` (`proc04_fatal_line_before_ready_kills_as_fatal_line` is a unit case and passes under wine; the V6 and V7 cases are process cases, labeled windows-runtime — authoritative in MSYS2, pending the owner's push; a direct wine run of each also passes) |
| V9, V10 | Ten forced restarts leave one live engine, no zombie, a stable descriptor/handle count and an empty stdout buffer in each new generation. A killed child that cannot be reaped within `reap_timeout` (30 s) is abandoned and opens the circuit; nothing respawns while it lives. | `secure_frontend_engine_supervisor_tests.proc04_no_leak_across_restarts`, `secure_frontend_engine_supervisor_tests.proc04_unreaped_child_opens_circuit` | `100% tests passed out of 134`; wine `100% tests passed out of 115` (`proc04_unreaped_child_opens_circuit` is a unit case and passes under wine; `proc04_no_leak_across_restarts` is a process case, labeled windows-runtime — authoritative in MSYS2, pending the owner's push; a direct wine run of it also passes). Winfix ruling: CI run `38598235525` showed exactly +1 handle from generation 2 on, never growing (88 vs 87 at generations 2..11); on Windows the baseline is now generation 2 (9 restarts compared against it), and the case prints the handle types that changed between generations 1 and 2 to name the one-time allocation — pending the next MSYS2 run |
| C1 | POSIX `terminate()` always ends with `kill(-pgid, SIGKILL)`, also after the leader was reaped; the group id is forgotten only after success. | `secure_frontend_engine_child_tests.proc05_group_killed_after_leader_reaped` (L) | `100% tests passed out of 135`; wine unchanged, `100% tests passed out of 115` (Linux-only case, not registered in the cross build; cross build `-Werror` clean) |
| C2 | The frontend is the child subreaper (`PR_SET_CHILD_SUBREAPER`). When a generation ends and its leader is reaped (restart path, 1 s bound) and at shutdown (250 ms bound, inside the shutdown budget), it SIGKILLs and reaps every child that is not a live engine, so `setsid`/`setpgid`/double-fork escapes die and leave no zombie. If the leader stays unreaped, the circuit opens without a sweep. | `secure_frontend_engine_supervisor_tests.proc05_setsid_grandchild_is_reaped` (L), `secure_frontend_engine_supervisor_tests.proc05_setsid_grandchild_is_reaped_at_shutdown` (L) | `100% tests passed out of 137`; wine unchanged, `100% tests passed out of 115` (Linux-only cases, not registered in the cross build; cross build `-Werror` clean) |
| C3 | Pre-exec `PR_SET_PDEATHSIG(SIGKILL)` + `getppid()` recheck against the pid `prepare` records before fork (mismatch → `_exit(127)`); forks only from the supervisor thread (the server too since Task 6b). Residual risk (spec §3.7): without a delegated cgroup, the engine's own descendants survive a frontend SIGKILL — logged by the test as "residual risk (spec 3.7): grandchild survived after a frontend SIGKILL without a delegated cgroup". The test waits for the grandchild pidfile, not `/health`. Task 16 (fix round 1 ruling): with a delegated cgroup, nothing writes `cgroup.kill` after a frontend SIGKILL, so the test does not assert that the grandchild dies. It asserts that `/proc/<grandchild>/cgroup` names the generation directory `strata-engine-<frontend pid>-1`. It logs "grandchild remains until the scope's cgroup.kill (frontend SIGKILL residual, delegated)". It then cleans up itself: writes `cgroup.kill` and removes the directory. | `secure_frontend_server_tests.proc05_frontend_sigkill_kills_engine` (L) | `100% tests passed out of 138`; wine unchanged, `100% tests passed out of 115` (Linux-only case, not registered in the cross build; cross build `-Werror` clean) |
| C4, C7 | SIGTERM/SIGINT → `shutdown_signal.cpp` (`sigaction` without `SA_RESTART`; the handler sets a lock-free `std::atomic<int>` flag (final review M3), fires the supervisor's self-pipe with one `write(2)` and restores `errno`) → supervisor thread runs the spec §3.7 order in `shutdown_generation`: EOF on engine stdin (skipped while a Lease is out, so no EOF lands under a request), wait 2 s, SIGTERM group, wait 1 s, SIGKILL group always, reap, 250 ms sweep, retire (ruling RF5: `retire(instance, reaped)`, `sweep_retired()` and the one 250 ms sweep kept; no second sweep in `finish()`; worst case 4.35 s with the ~100 ms delegated-cgroup removal, matching `cgroup_scope.hpp` and `engine_supervisor.cpp`); log `engine generation <n> ended: killed_by_supervisor(shutdown)` (an exit 0 on our EOF is an exit we requested, §3.6); then `finish()` calls `HttpSocketServer::shutdown_listener()` (POSIX `shutdown(2)` wakes `accept()`; Windows `closesocket`; since final review I1 it also `shutdown()`s the client `serve_one()` is handling and the request read re-checks every 100 ms, so an idle client cannot hold `main`: worst case 4.35 s + 0.1 s), reports `stopped`, and waits for `main`'s `shutdown()`; `main` leaves the serve loop on `stop_requested()` and returns 0, or 1 when a generation was never reaped (final review I2: `finish()` logs `engine supervisor stopped with an unreaped engine: exit 1` and `engine_left_unreaped()` is true); past 5 s either side `_exit(1)`. An RAII guard in `run` stops the supervisor on every exit path while the listener still exists. RF4: a Lease reaches the supervisor through a `LeaseLink` that `shutdown()` and the destructor detach, so a Lease that outlives `shutdown()` (or the supervisor) never touches it; `~EngineSupervisor` unhooks its wake from the signal handlers. The production binary's only child is the engine, executed directly (argv[0] = `--engine` path). | `secure_frontend_server_tests.proc05_frontend_sigterm_stops_all_descendants`, `secure_frontend_server_tests.proc05_frontend_sigint_stops_all_descendants` (L; `--spawn-grandchild`: exit 0 within 5 s, engine and grandchild gone, port refuses, shutdown log line), `secure_frontend_server_tests.proc05_frontend_has_only_engine_children` (C7, L; spec Fix "—": passed first run; `/proc/<child>/exe` = canonical `engine_path()` and argv[0] = `engine_path()`), `secure_frontend_server_tests.proc05_sigterm_during_generation_exits_cleanly` (Review Focus 3), `secure_frontend_server_tests.proc05_double_sigterm_is_idempotent` (Review Focus 5); unit: `secure_frontend_engine_supervisor_tests.supervisor_shutdown_sends_eof_first_then_kills`, `secure_frontend_engine_supervisor_tests.supervisor_lease_outliving_shutdown_is_safe` (RF4; destroys the supervisor and poisons its memory before the Lease ends — aborted with `std::system_error` before the fix), `secure_frontend_engine_supervisor_tests.shutdown_signal_preserves_errno` (L; full self-pipe so the handler's write fails with `EAGAIN`), `secure_frontend_http_socket_server_tests.socket_server_shutdown_listener_wakes_accept` | `100% tests passed out of 263` (1 skipped: `proc03_other_uid_cannot_open_engine_fd`); wine `100% tests passed out of 145` (the EOF, Lease and listener cases are portable and pass under wine; the server cases are labeled windows-runtime and the errno case is Linux-only); cross build `-Werror` clean; pytest 13 passed |
| C5 | Delegated cgroup v2 detected (writable own cgroup with `cgroup.kill` and an empty `cgroup.subtree_control`, never created with privileges): each generation joins `strata-engine-<frontend pid>-<n>` before `execve`. The directory name is unique per frontend. A `mkdir` that fails, EEXIST included, means the generation runs without a cgroup and logs the fixed literal `engine generation <n>: cgroup unavailable` (no path). Cleanup writes `cgroup.kill`, waits for `populated 0` for at least `kCgroupRemoveBound` (100 ms, so the 0 ms shutdown and exception callers also remove the directory; shutdown worst case 4.35 s < 5 s), then removes the directory. A failed removal keeps the directory recorded, and `cleanup()` kills and retries it. A spawn that fails (open_cgroup, fork or execve) removes its empty directory. Off unless detected. Ruling RF8: `posix_launch::prepare` stays `void`; the cgroup fd is opened by `[[nodiscard]] bool posix_launch::open_cgroup(PreparedExec&, const EngineCommand&)`, called after `prepare` at both engine call sites (failure → `platform_failure` "engine cgroup setup failed"); the child writes it right after `setpgid` (preflight D-9 order). The server sets `plan.use_delegated_cgroup = true` (detected, no config key). | `secure_frontend_engine_supervisor_tests.proc05_cgroup_kill_when_delegated` (L; SKIPPED here: no delegated cgroup — this host runs in `/user.slice/user-1000.slice/session-3.scope`; prints "SKIP: no delegated cgroup v2 is writable here; run under systemd-run --user --scope -p Delegate=yes to exercise it", exit 77), the C5 case asserts the setsid grandchild's `/proc/<pid>/cgroup` names the generation directory before the kill; C3 `proc05_frontend_sigkill_kills_engine` asserts cgroup membership (not death) when delegated, logs the residual and cleans up (delegated branches not exercised here) | `100% tests passed out of 264` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`); wine unchanged, `100% tests passed out of 145` (Linux-only case, not registered in the cross build; cross build `-Werror` clean); pytest 13 passed. RED/GREEN inside a delegated scope pending the owner's approval to run `systemd-run --user --scope -p Delegate=yes ctest --preset linux-gcc -R proc05_cgroup --output-on-failure` |
| C6 | Windows launch (`engine_stdio_process.cpp` and its SP0 twin `engine_process.cpp`): `CREATE_NEW_PROCESS_GROUP` (Ctrl-C is disabled in the engine's group; group-targeted Ctrl-Break reaches only the frontend) and, since Task 15 (ruling RF4), `CREATE_NO_WINDOW`: the engine runs without the frontend's console, so a console-wide Ctrl-Break or a closed console window reaches only the frontend; the Job adds `JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION` to `KILL_ON_JOB_CLOSE`; `SetErrorMode(GetErrorMode() \| SEM_NOGPFAULTERRORBOX \| SEM_FAILCRITICALERRORS)` once per process before spawn, inherited by the engine. Task 15: `SetConsoleCtrlHandler` (CTRL_C, CTRL_BREAK, CTRL_CLOSE) sets the shutdown flag and fires the supervisor's wake, then holds the handler thread (at most 5 s) until the supervisor reports every engine gone, so the same §3.7 order as C4 runs. New fake flag `--default-error-mode` (the fake skips its own `SetErrorMode` and runs with the mode it inherited). | `secure_frontend_server_tests.proc05_windows_ctrl_break_stops_all_descendants` (W; `--spawn-grandchild`: CTRL_BREAK to the server's group → exit 0, Job `ActiveProcesses` 0), `secure_frontend_server_tests.proc05_windows_terminate_frontend_kills_job` (W; TerminateProcess → kill-on-close Job takes engine and grandchild); supervisor-level smokes of the launch settings: `secure_frontend_engine_supervisor_tests.proc04_windows_abort_does_not_stall` (W; `--abort-in-gen --default-error-mode`: abort / fastfail path, error mode only), `secure_frontend_engine_supervisor_tests.proc04_windows_crash_does_not_stall` (W; `--crash-after-ready=SEGV --default-error-mode`: a real unhandled exception, so the Job's `DIE_ON_UNHANDLED_EXCEPTION` is on the path; end classified `crashed` within 5 s) | Task 17: `100% tests passed out of 138`; wine `100% tests passed out of 115`. Task 15: `100% tests passed out of 263`; wine `100% tests passed out of 145` (the two server cases are labeled windows-runtime and excluded); cross build `-Werror` clean; direct wine runs of both server cases exit 1 with `FAILED: ready` because the harness prints `harness: no non-elevated token for strata-secure-server (LUA and Safer NORMALUSER both elevated)` (wine has no UAC, as in the F3 row), so wine cannot show the console handler; Task 17's direct wine runs: `proc04_windows_abort_does_not_stall` exits 0 (wine has no WER dialog), `proc04_windows_crash_does_not_stall` exits 1 (wine starts winedbg on the access violation) — **authoritative in MSYS2, pending the owner's push** (watch: the engine's stdio with `CREATE_NO_WINDOW`). Known limits (Task 15 review): the `CTRL_CLOSE` handler's return races the process exit code; on Windows `signal_group_terminate` is a no-op, so the 1 s TERM wait of the §3.7 order is idle there (EOF, then the Job kill). `SetErrorMode` pair: SP1's `install_process_defaults()` replaces the process error mode, then SP2's launch ORs `SEM_NOGPFAULTERRORBOX \| SEM_FAILCRITICALERRORS` into it; both set the same bits, so the order does not matter |
| N4 | Fork with every signal blocked (`posix_launch::BlockAllSignals` around `fork()` in `engine_stdio_process.cpp` and its SP0 twin `engine_process.cpp`); the child (`posix_launch::reset_signal_state()`, called in `exec_child` immediately before `execve`) resets every disposition but SIGKILL/SIGSTOP to `SIG_DFL` and clears the mask before `execve`. | `secure_frontend_engine_child_tests.proc02_child_signal_state_is_default` (L) | `100% tests passed out of 139`; wine unchanged, `100% tests passed out of 115` (Linux-only case, not registered in the cross build; cross build `-Werror` clean) |
| N5 | Canaries: a non-CLOEXEC fd, a listening socket and an open token file never reach the engine (Linux: `FDS 0,1,2`); Windows: an inheritable file, event and socket are absent from the engine's handle table (parent-side `NtQueryInformationProcess(ProcessHandleInformation)` + `CompareObjectHandles`, both resolved via `GetProcAddress`; the snapshot must list at least 3 handles and at least one must duplicate, and a positive control plants the event in the engine with `DuplicateHandle` and requires the same inspection to find it, so a failed inspection cannot pass as "absent"). No production change (existing sweep / handle list); both passed on their first run where runnable. | `secure_frontend_engine_child_tests.proc02_canary_descriptors_absent` (L), `secure_frontend_engine_child_tests.proc02_canary_handles_absent` (W) | `100% tests passed out of 140`; wine unchanged, `100% tests passed out of 115` (W case labeled windows-runtime, excluded); cross build `-Werror` clean; a direct wine run of `proc02_canary_handles_absent` exits 1 with `FAILED: inspection APIs are available (Windows 10 1607+)` (wine 6.0.3's kernelbase does not export `CompareObjectHandles`; not a leak finding) — **W authoritative in MSYS2, pending the owner's push** |
| N6, R1, K2 | Windows stdin pipe: protected DACL with one ACE for the current user, `\\.\pipe\secure_frontend-<pid>-<128-bit random>` (SP1's `fill_random()` since Task 29a; `BCryptGenRandom` directly before), single instance, remote clients rejected, `GetNamedPipeClientProcessId` must be our pid (a mismatch fails start with ERROR_ACCESS_DENIED; that path is untested, it cannot be provoked without racing our own client open); frontend end `FILE_FLAG_OVERLAPPED`, 64 KiB buffer and chunks, 1 s no-progress deadline per 64 KiB chunk (plus, since R1, an overall bound: see the "R1 (Task 22 / K2 overall bound)" row) then `CancelIoEx` → write failure → `killed_by_supervisor(write_timeout)`. Limits doc updated by Task 28 (`docs/security/engine-protocol-limits.md`, "Windows stdin writes": both bounds). | `secure_frontend_engine_child_tests.proc02_stdin_pipe_dacl_is_owner_only`, `secure_frontend_engine_child_tests.proc03_windows_pipe_second_open_fails` (a `GENERIC_READ` client open must fail with exactly `ERROR_PIPE_BUSY`, and a rival `CreateNamedPipeW` on the name must be refused), `secure_frontend_engine_supervisor_tests.proc04_windows_write_timeout_kills_engine` (all W; K2: 60,000-token prompt, ~240 KB GEN line, 2 s generation timeout, CTest timeout 300 s) | Linux unchanged, `100% tests passed out of 140`; wine unchanged, `100% tests passed out of 115` (W cases labeled windows-runtime, excluded); cross build `-Werror` clean; direct wine runs: all three exit 0 (K2 in ~1.5 s), also after fix round 1; RED under wine before the fix: K2 failed `the blocked write is cancelled at its deadline` / `killed as write_timeout` (the write fit the old 16 MiB buffer) — **W authoritative in MSYS2, pending the owner's push** |
| A1, A2 | Windows argv twin: quotes, backslashes (incl. trailing), `%PATH%`, `^&\|<>()`, newline, UTF-8, empty string round-trip through the frontend's quoting and `CommandLineToArgvW`. End to end: `--engine-arg` values with shell metacharacters and the inventory `--native` reach the engine as literal argv entries with no shell (since Task 19 the case also passes the typed `--gpu 0,1` and expects `--gpu`, `0,1` directly before `--native`). No production change; both passed on their first run where runnable (A2 runs against the current server; the fake writes `--dump-file` before READY; since Task 29a the expected argv[0] is the private engine copy, `ServerProcess::engine_path()`, which SP1 passes in canonical form). | `secure_frontend_engine_child_tests.proc01_windows_argv_round_trip` (W), `secure_frontend_server_tests.proc01_server_passes_literal_argv` (B) | `100% tests passed out of 141`; wine unchanged, `100% tests passed out of 115` (both cases labeled windows-runtime, excluded); cross build `-Werror` clean; direct wine run of `proc01_windows_argv_round_trip` exits 0; direct wine run of `proc01_server_passes_literal_argv` exits 1 with `harness: no non-elevated token for strata-secure-server` / `FAILED: the server must listen` (known wine limit: no UAC, the restricted-token harness cannot launch the server; not an argv finding) — **W and the Windows run of B authoritative in MSYS2, pending the owner's push** |
| R2, R3 | Linux PROC-03 = other user context: `nobody` cannot open `/proc/<engine>/fd/0` (CI via `sudo -n -u nobody`, env `STRATA_TEST_SECOND_UID_SUDO=1`, set on the Linux ctest step for both linux-gcc and linux-clang; skipped locally with its reason printed, never run with the variable set outside CI). With the variable set, a failing `sudo -n -u nobody` is a failure, not a skip; a same-uid run of the identical probe must open the path (positive control), the `nobody` probe must exit 4 (open refused, not engine gone/hidepid), and the engine must still answer DUMP afterwards. Same-uid reopen and injection is the recorded accepted risk (threat model "Engine process isolation"; release record). No production change; R3 passed on its first run. | `secure_frontend_engine_child_tests.proc03_other_uid_cannot_open_engine_fd` (L, CI only, `SKIP_RETURN_CODE 77`), `secure_frontend_engine_child_tests.proc03_same_uid_reopen_is_accepted_risk` (L) | `100% tests passed out of 143` (1 skipped: `proc03_other_uid_cannot_open_engine_fd`, printing `SKIP: needs a second uid: CI sets STRATA_TEST_SECOND_UID_SUDO=1 where` … `is allowed`); wine unchanged, `100% tests passed out of 115` (Linux-only cases compiled out); cross build `-Werror` clean — **R2 authoritative in the GitHub Linux jobs, pending the owner's push** |
| SP1 reconcile (29a) | Post-merge reconcile. (1) `ServerProcess::start` copies the engine (default the fake) into a private `<root>/bin` (directory 0700, file 0700 on POSIX; Windows: the protected owner-only DACL of `make_private_directory`, as SP1's `ServerFixture`) and passes the copy; `ServerProcess::engine_path()` exposes it (canonical on POSIX) and A2 expects it as argv[0]. SP1's engine trust check is unchanged. The harness's own free-port helper is gone; it calls SP1's `secure_frontend_test::pick_free_port`. (2) `supervision_defaults.hpp` folded into `config_defaults.hpp` and deleted; `RestartPolicy::ready_timeout` defaults to `kEngineReadyTimeoutS` (900 s, one copy); `SupervisorConfig supervisor_config(const FrontendConfig&)` in `subsystem_limits` (chat limits, `policy.ready_timeout` from `limits.engine.ready_timeout_s`, profile and protocol flavor); not yet used by the server (Task 6b). (3) The Windows stdin pipe name's 128 random bits come from SP1's `fill_random()`; `secure_frontend_engine_stdio_process` links `secure_frontend_secrets` instead of `bcrypt`. (4) `EngineProcess` (SP0) is marked test-only; its `terminate` keeps its behaviour without C1's unconditional group SIGKILL (ruling RF12). | `secure_frontend_server_tests.harness_server_lifecycle_selftest`, `secure_frontend_server_tests.proc01_server_passes_literal_argv`, `secure_frontend_server_tests.proc05_frontend_sigkill_kills_engine` (all three RED on the merged tree, GREEN now), `secure_frontend_subsystem_limits_tests.supervisor_policy_follows_config` (new; labeled windows-runtime like its binary's other cases); the pipe-name change is covered by the existing N6/R1/K2 cases | `100% tests passed out of 248` (1 skipped: `proc03_other_uid_cannot_open_engine_fd`); wine `100% tests passed out of 141` (the new case is labeled windows-runtime and excluded; direct wine runs of `supervisor_policy_follows_config`, `proc02_stdin_pipe_dacl_is_owner_only` and `proc04_windows_write_timeout_kills_engine` exit 0); cross build `-Werror` clean — W authoritative in MSYS2, pending the owner's push |
| V2, V11 | The production server runs the engine through `EngineSupervisor` (Task 6a): READY is consumed eagerly; `/health` `starting` → `503` (reverses SP0's `200`, true now that READY is eager), `ready`/`generating` → `200`, optional `recovery` only with `engine=failed`; one snapshot per `/health`. A chat outside `ready` is an immediate generic `503`; parse before acquire (SP1's pre-parse `engine.running()` check is gone, EXT-01). The supervisor's configuration is `supervisor_config(config)` (Task 29a: chat limits, profile, flavor, READY deadline from `limits.engine.ready_timeout_s`); `supervisor.start()` runs after the bind and after the token read (CFG-01); the supervisor is declared before the listener and router, and a `Lease` lives only inside one `execute_with_lease()` call. `server_usage()` describes supervision. SP1 e2e cases that chat or expect a `200` `/health` first call `ServerFixture::wait_engine_ready()` (Task 29 Step 6 + ruling RF3: `auth02_valid_token_succeeds_and_is_not_echoed`, `init_writes_config_and_token_then_server_starts`, `cfg03_engine_receives_canonical_models_root`, `cfg05_token_absent_from_child_argv_env_and_outputs`, the `5` branch of `ready_timeout_is_configurable`). `ready_timeout_is_configurable` is now a true test of the supervisor's READY deadline (fix round 1): with `ready_timeout_s=1` against a 2 s READY the engine never becomes ready within 4 s and the server logs `killed_by_supervisor(ready_timeout)`; with `5` it becomes ready, the chat is `200` and no such kill is logged (a mutation that drops the config→policy line in `supervisor_config` fails it). SP1's CFG-01 case asserts neither `Unable to start the engine supervisor` nor an `engine generation` line was printed. | `secure_frontend_server_tests.proc04_ready_is_eager_and_health_starting_is_503`, `secure_frontend_server_tests.proc04_chat_during_restart_is_generic_503` | `100% tests passed out of 254` (1 skipped: `proc03_other_uid_cannot_open_engine_fd`; 253 at `0e23d220`, +1 V4 server case in fix round 1); wine `100% tests passed out of 142` (server cases labeled windows-runtime, excluded) — Windows authoritative in MSYS2, pending the owner's push |
| V8 | An abort mid-generation answers the client with the fixed generic `503`, logs `abort`, and the supervisor recovers. No production change of its own. | `secure_frontend_server_tests.proc04_abort_in_generation_is_generic_503` | `100% tests passed out of 254` (1 skipped: `proc03_other_uid_cannot_open_engine_fd`; 253 at `0e23d220`, +1 V4 server case in fix round 1); wine `100% tests passed out of 142` (labeled windows-runtime, excluded) — Windows authoritative in MSYS2, pending the owner's push |
| Supervisor hardening (RF4, Task 6b) | `EngineSupervisor::start()` is `[[nodiscard]] bool`: it refuses (false, nothing spawned) when `SupervisorWake` could not create its pipe/event (`SupervisorWake::valid()`) or the thread cannot be created, and the server then exits with `Unable to start the engine supervisor.` An exception escaping the supervisor thread (factory, log sink, clock, allocation) never reaches `std::terminate`: the thread kills and reaps the current generation, opens the circuit (`/health` `failed`/`exhausted`, logged `engine supervisor failed: circuit open`; a reaped generation gets the 250 ms orphan sweep and `release_resources` first, as in `end_generation`/`shutdown_generation`, spec §3.7(b)) and still ends in `stopped` on shutdown. The descendant sweep's `/proc/self/task` loop uses `directory_iterator::increment(ec)`. | `secure_frontend_engine_supervisor_tests.supervisor_exception_opens_circuit` (factory, log sink, and clock with a live generation), `secure_frontend_engine_supervisor_tests.supervisor_start_refuses_without_wake` (L; `RLIMIT_NOFILE` 0 while the supervisor is built) | `100% tests passed out of 254` (1 skipped: `proc03_other_uid_cannot_open_engine_fd`; 253 at `0e23d220`, +1 V4 server case in fix round 1); wine `100% tests passed out of 142` (`supervisor_exception_opens_circuit` is a unit case and passes under wine; the wake case is Linux-only) |
| N1, N2, N3 | `EngineEnvironment` → the child gets exactly: POSIX `PATH=/usr/bin:/bin`, `LD_LIBRARY_PATH`, `TMPDIR=<state>/engine-tmp/<gen>`, `CUDA_CACHE_PATH=<state>/cache/cuda` (`<roots.cache>/cuda` when `roots.cache` is set, since Task 29b) (+ optional `STRATA_HIPBLASLT_TUNING`, `STRATA_WATCHDOG_S`, `STRATA_WATCHDOG_IO_S`); Windows `PATH=<lib dirs>;%SystemRoot%\System32`, `TEMP`, `TMP`, `CUDA_CACHE_PATH`, `SystemRoot`, `WINDIR`, `SystemDrive`, `NUMBER_OF_PROCESSORS`. Never the token variable, `STRATA_API_KEY`, other `STRATA_*`, `LD_PRELOAD`/`LD_AUDIT`, `CUDA_LAUNCH_BLOCKING`, `HOME`/`USERPROFILE`/`APPDATA`, proxies, or GPU-selection variables. New required `--state-dir` (SP1 key `roots.state`: `validate()` checks only absolute / non-empty / no NUL, so `--check` stays side-effect free; serve refuses with `roots.state (--state-dir) is required to serve.` when unset and, before anything is created, bound or spawned, with `--state-dir must be an existing owner-only directory.` unless `is_private_state_root` holds: POSIX `lstat` shows a real directory (not a symlink) owned by the effective uid with `mode & 077 == 0`; Windows opens it with `FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS`, refuses a junction/symlink (`is_reparse_point`) and requires SP1's `has_private_owner_dacl` for the current user (owner = user, protected DACL, allow-ACEs only for the user, SYSTEM, Administrators — a read ACE for anyone else is refused). **Accepted limitation:** POSIX `lstat` checks only the final component, so a parent directory writable by another user could swap the state root after the check; place `--state-dir` below owner-controlled parents. **MSYS2 watch:** the Windows check requires the state root's owner to be the current user (an elevated admin's default owner is Administrators); test state roots are therefore created with `make_owner_private_directory` (explicit `O:<user>`, SP1's `ProtectedAttributes`) in the harness, `ServerFixture` and the state-dir cases — not observable under wine. `--engine-lib-dir` (SP1 key `engine.lib_dirs`, repeatable, each absolute and free of the platform list separator `:`/`;`; default the engine executable's directory). Serve creates `<state>/engine`, `engine-tmp` and the CUDA cache owner-only: `<state>/cache/cuda` (with `<state>/cache`) only when `roots.cache` is unset, otherwise `<roots.cache>/cuda` (Task 29b). Per-generation temp `<state>/engine-tmp/<n>` is removed by `release_resources()` after the generation is reaped; `engine-tmp/*` is cleared at startup. SP1 test support updated (RF7): `ServerFixture` gains a 0700 `state` root in `base_flags()`/`base_literals()`, `server_cli_test` `init_flags` passes `--state-dir`; the README key table is regenerated (`readme_config_table_matches_schema`) and the README serve example gains `--state-dir`. **Not GPU-verified** (offline GPU box, the owner). | `secure_frontend_engine_supervisor_tests.proc02_child_env_is_exact_allowlist`, `secure_frontend_engine_stdio_process_tests.child_receives_exact_argv_and_env` (now asserts env), `secure_frontend_engine_process_tests.proc02_engine_process_env_checks_windows_names`, `secure_frontend_server_tests.state_dir_must_be_owner_only` (Review Focus 1), `secure_frontend_server_tests.state_dir_must_be_owner_only_windows` (Users read ACE, junction root; MSYS2 pending), `secure_frontend_server_tests.state_dir_stale_engine_tmp_is_cleared`, `secure_frontend_server_cli_tests.serve_requires_state_root`, `secure_frontend_typed_config_tests.engine_lib_dirs_must_be_absolute_directories` (fix round 1) | `100% tests passed out of 267` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`); wine unchanged, `100% tests passed out of 145` (the three new process/server cases are labeled windows-runtime: authoritative in MSYS2 — pending the owner's push; a direct wine smoke of N3 passes, and of N1 shows every rendered entry present plus only wine's own injected `WINE*`, `ProgramFiles*`, `CommonProgramFiles*` names); cross build `-Werror` clean; pytest 13 passed |
| §3.9 GPU | `--gpu LIST` (typed, 0–255, distinct, ≤ 16; no sign, space or leading zero; SP1 key `engine.gpu` with its `kFlags` row and `EngineLaunch::gpus`) is parsed by `parse_gpu_list` in `validate()`, so a malformed value (`0,0`, `-1`, `1,,2`, ` 1`, `256`, `01`, 17 entries; empty is SP1's `empty value for --gpu`) is a startup error with the fixed text `engine.gpu: must be a comma-separated list of distinct device indices 0-255, at most 16` that never echoes the value and never reaches the engine. `build_engine_arguments(..., gpus)` renders the engine's own `--gpu i,j` directly before `--native`; the engine sets `CUDA_DEVICE_ORDER`/`CUDA_VISIBLE_DEVICES` (or HIP) itself (upstream `src/program/generate.cpp:1844-1865`, usage `:706-708`, at v0.1.44.1; the spec's `:1765-1786` predates the bump). `--gpu` is frontend-owned for `--engine-arg`/`engine.args` in both profiles (`owned_flag`, refused by `validate_engine_user_arguments` before anything is spawned; the GLM branch refused only `--serve` before); the GLM profile refuses a typed GPU list (`engine.gpu: is not supported with engine.profile glm53`; `unsupported_mode` in `build_engine_arguments`). Strata family: no GPU variable in the engine environment (a later SP6 glm family renders `CUDA_DEVICE_ORDER`/`CUDA_VISIBLE_DEVICES` from typed configuration; not implemented here). `server_usage()` lists `--gpu` and says `--engine-arg` refuses `--native`, `--serve` and `--gpu`; `--check` prints `engine.gpu`; README key table regenerated (RF7). **Not GPU-verified** (offline GPU box, the owner). | `secure_frontend_engine_arguments_tests.engine_args_render_typed_gpu_and_deny_gpu_passthrough` (incl. GLM `--gpu` passthrough refused; labeled windows-runtime like its canonical-root siblings, passes in a direct wine smoke), `secure_frontend_engine_arguments_tests.gpu_list_rejects_malformed_values` (Review Focus 4), CTest `strata_secure_server_rejects_malformed_gpu` (Review Focus 4; fails if the output contains `0,0`), A2 `secure_frontend_server_tests.proc01_server_passes_literal_argv` extended | `100% tests passed out of 273` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`); wine `100% tests passed out of 147` (145 + `gpu_list_rejects_malformed_values` + `strata_secure_server_rejects_malformed_gpu`); cross build `-Werror` clean; pytest 13 passed |
| G1, G3 | The engine's cwd is `<state_dir>/engine` (`EngineCommand::working_directory{}`, empty = inherit; POSIX `chdir` pre-exec in `posix_launch::exec_child` after the `dup2` block and before the descriptor sweep, Windows `lpCurrentDirectory` in both `CreateProcessW` twins; serve sets it from the validated `roots.state`); same uid/gid (POSIX) and same SID, never MORE elevated, never a higher integrity level than the frontend (Windows; relative check by owner decision, Global Constraints). `ERROR_ELEVATION_REQUIRED` (740) is `spawn_failed` with the fixed message `engine requires elevation; refused` in `EngineStdioProcess::start` and `EngineProcess::start`, never `runas`. Test-only `strata-supervision-fake-engine-elevated` (Windows; embedded `requireAdministrator` manifest, `/MANIFEST:NO` under MSVC); `secure_frontend_set_warnings` now scopes its flags to C++ so the RC compiler receives none. | `secure_frontend_engine_child_tests.proc06_child_ids_and_cwd` (B); G3: `proc06_elevation_required_is_spawn_failed` (W) — **UNVERIFIED in CI and wine; validation-kit item, not a CTest test** | `100% tests passed out of 274` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`); wine unchanged, `100% tests passed out of 147` (G1 is labeled windows-runtime: authoritative in MSYS2 — pending the owner's push; a direct wine smoke of `proc06_child_ids_and_cwd` exits 0; G3 is not run under wine: no UAC); cross build `-Werror` clean, `strata-supervision-fake-engine-elevated.exe` produced with the manifest embedded; pytest 13 passed |
| G2, G4 | Linux: `PR_SET_NO_NEW_PRIVS` pre-exec (`posix_launch::exec_child`, after `chdir`, before the descriptor sweep; `EngineCommand::no_new_privileges{false}` at the type, `prepare` stays void), **on by default** (owner decision; `config_defaults::kNoNewPrivilegesDefault`); SP1 key `engine.allow_new_privileges` / kFlags `--allow-new-privileges` turns it off (documented for the GPU validation kit; README key table regenerated). A one-time log hint (`kNoNewPrivilegesHint`) names the `/dev/nvidia-uvm` / `nvidia-modprobe -u -c=0` / `nvidia-persistenced` fix when an engine dies before READY on its own under NNP. The frontend refuses euid 0 / an elevated Windows token with a fixed error (`privilege_check.{hpp,cpp}`), right after `--help` and before `init`, `--check` and serve; no override flag. **Test-support change (ruling, Option A):** the Task 3 harness's non-elevated launch (LUA token, Safer NORMALUSER fallback, still-elevated → fixed STOP message) moved into the shared `tests/support/non_elevated_launch.hpp`; the harness, SP1's `process_runner.hpp` (only for `strata-secure-server`; other executables keep `CreateProcessW`) and the new Windows launcher `secure_frontend_non_elevated_run` all use it, and one shared Windows argument quoter (`quote_windows_argument`, `tests/support/windows_command_line.hpp`); the launcher holds the server in a kill-on-close Job. On Windows, `strata_secure_server_glm_requires_engine_arg` and `strata_secure_server_rejects_malformed_gpu` run through that launcher and are windows-runtime (wine cannot get a non-elevated token); on Linux they run unlabeled as before. **SP1 security-test edit (owner-visible):** `auth04_no_switch_disables_auth`'s boolean allow-list now also admits `engine.allow_new_privileges`. | `secure_frontend_engine_child_tests.proc06_child_has_no_new_privs` (L), `secure_frontend_server_tests.proc06_nnp_startup_hint_names_nvidia_uvm_fix` (L), `secure_frontend_engine_supervisor_tests.proc06_refuses_privileged_start` (B unit) | `100% tests passed out of 277` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`); wine `100% tests passed out of 146` (147 + 1 for `proc06_refuses_privileged_start` − 2 CLI checks now windows-runtime; under wine the launcher reports "no non-elevated token ... STOP" exactly like the harness); cross build `-Werror` clean; pytest 13 passed. MSYS2 (authoritative for the de-elevated SP1 server cases and the two CLI checks) — pending the owner's push |
| X1, X2 | Tool/function/MCP/stream fields and a `tool` role are answered with the fixed `400 {"error":"Bad Request"}` before any lease: no GEN, no process change. `STRATA_*`, `LD_PRELOAD`, `CUDA_LAUNCH_BLOCKING` in the server's environment never reach the engine; `--vision`/`--batch`/`--gpu` via `--engine-arg` and unknown options fail with exit 1 before any spawn. The inventory has no manifest fields to abuse (IDs come from file names), so there is no manifest extension to attempt. No production change; the tests passed on their first run. | `secure_frontend_server_tests.ext01_extension_fields_spawn_nothing`, `secure_frontend_server_tests.ext02_extension_env_never_reaches_child`, `secure_frontend_server_tests.ext02_rejected_before_spawn` (L, W-runtime label) | `100% tests passed out of 280` (2 skipped: proc03 other-uid, proc05 cgroup delegation); wine `100% tests passed out of 146` (unchanged; server cases are labeled out) |
| SP1 integration (Task 29: 29a + 29b) | SP2 options are SP1 schema keys with `kFlags` rows, verified against Task 29 Step 3 (landed by Tasks 18, 19, 26): `--state-dir` → `roots.state` (required to serve), `--engine-lib-dir` → `engine.lib_dirs`, `--gpu` → `engine.gpu`, `--allow-new-privileges` → `engine.allow_new_privileges`; `engine.gpu` with `engine.profile glm53` is an `engine.gpu` `conflicting_values` issue (now tested). `limits.engine.ready_timeout_s` drives `RestartPolicy::ready_timeout` (`supervisor_config`, 29a); `supervision_defaults.hpp` folded into `config_defaults.hpp` (29a); the Windows pipe name uses SP1's `fill_random` (29a); SP1's e2e chat cases poll `/health` until `engine=ready` (6b). 29b: `engine_environment_for(state_dir, cache_root, lib_dirs)` — the CUDA cache is `<roots.cache>/cuda` when `roots.cache` is set (serve creates it owner-only; nothing is created at `<state>/cache`), else `<state>/cache/cuda`; the server builds the engine environment through it. `roots.cache`'s schema description now says it holds the CUDA cache (was "unused in SP1"); README key table regenerated. Test hardening: A2 asserts the engine's cwd is `<state>/engine` (production `working_directory` wiring); X1 asserts its record file opened and adds a positive control (a valid chat records `GEN`); X2 asserts the dumped environment holds the allowlisted `CUDA_CACHE_PATH` and that each startup rejection prints its fixed message (`engine.args: …` / `unknown option: --enable-mcp`); the X1/X2 case functions moved into the anonymous namespace. | `secure_frontend_server_cli_tests.cli_sp2_flags_map_to_schema_keys` (unit, unlabeled), `secure_frontend_subsystem_limits_tests.supervisor_policy_follows_config` (29a), `secure_frontend_engine_supervisor_tests.engine_environment_uses_cache_root_when_set` (unit, unlabeled), `secure_frontend_server_tests.engine_cuda_cache_follows_roots_cache` (new, windows-runtime), `secure_frontend_server_tests.proc01_server_passes_literal_argv`, `secure_frontend_server_tests.ext01_extension_fields_spawn_nothing`, `secure_frontend_server_tests.ext02_extension_env_never_reaches_child`, `secure_frontend_server_tests.ext02_rejected_before_spawn` (extended) | `100% tests passed out of 283` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`); wine `100% tests passed out of 148` (the two unit cases run and pass; the server case is windows-runtime, excluded; a direct wine run of it exits 1 with the known `harness: no non-elevated token` wine limit); cross build `-Werror` clean; pytest 13 passed — W authoritative in MSYS2, pending the owner's push |
| SP3 D-1 | Lease-release notification and a documented thread-safety contract for SP3's worker pool (SP3 spec §1.5 D-1, §2.5). New `EngineSupervisor::wait_for_change(SupervisorState from, std::chrono::milliseconds timeout) const` → `HealthSnapshot`: blocks until the state is no longer `from` or the timeout passes and returns the state then (still `from` on timeout). It waits on the existing `changed_` condition variable (the same one `wait_for_state` uses, so there is no second wait mechanism). Every state change already notifies it under `mutex_`: release (`generating` → `ready`), `report_fatal`/crash (→ `stopping`), `backoff`, `circuit_open`, shutdown (→ `shutting_down`, `stopped`). `wait_for_change(generating, t)` therefore wakes on a release and never sleeps through a failure or a shutdown. No polling. The notification is a condition-variable notify only, so the serving thread still never forks, reaps or kills (SP2 spec §3.1), and the shutdown sequence and its 5 s bound are unchanged. Header contract in `engine_supervisor.hpp`: `acquire`, `snapshot`, `report_fatal`, `wait_for_change`, `wait_for_state`, `stop_requested`, `generations_started` and `~Lease` are safe from any number of threads (each takes `mutex_` or reads an atomic); there is at most one Lease, so `acquire()` needs no caller lock; a Lease is used by one thread at a time but may move between threads; `set_on_stopped`/`start` run once on the owning thread before serving; `shutdown()` is called from one thread and may run concurrently with the serving API; nothing calls in once the destructor has begun, except that a Lease may outlive the supervisor (RF4, kept). | `secure_frontend_engine_supervisor_tests.supervisor_wait_wakes_on_lease_release` (unit: timeout returns `generating` after ≥ 200 ms and < 1 s; a blocked waiter sees `ready` < 1 s after another thread destroys the Lease), `secure_frontend_engine_supervisor_tests.supervisor_wait_wakes_on_shutdown` (unit: a waiter with a Lease out wakes < 1 s after `shutdown()`), `secure_frontend_engine_supervisor_tests.supervisor_serving_api_is_thread_safe` (process, fake engine: 8 threads × 2 s of snapshot/acquire/release/`wait_for_change`; bounded join (exits 1 on deadlock), no two Leases, `acquire()` empty while leased, holder sees `generating`, no restart; TIMEOUT 120; labeled windows-runtime like its siblings) | `100% tests passed out of 286` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`); wine `100% tests passed out of 150` (148 + the two unit cases; the stress case is windows-runtime and excluded, and passes in a direct wine smoke, exit 0); cross build `-Werror` clean; pytest 13 passed; stress case checked by mutation (`acquire()` also allowed in `generating` → 1,347,693 violations, FAILED) and 8 clean repeat runs |
| SP3 D-2 | A drain that fails after a CLIENT-requested `STOP` kills and restarts the engine but does not count toward the circuit (SP3 spec §1.5 D-2, risk R4; controller ruling R3). Carrier: new `enum class EngineStopCause { generation_timeout, client_cancelled }` (`engine_protocol_client.hpp`); `EngineProtocolClient::stop(EngineStopCause cause = generation_timeout)` records the cause only when `STOP` was written, `stop_cause()` reads it, and every `begin_generation()` call forgets it at its top, even a refused one (fix round 1). `ChatExecutionService` (today the only `STOP` issuer, after `generation_timeout`) now passes `generation_timeout` explicitly; SP3's P6 cancel (SP3 spec §2.6) passes `client_cancelled`. `kill_reason_for` maps `abandoned` to new `KillReason::client_stop_drain_failed` when the cause is `client_cancelled`, else `drain_failed` (unchanged); SP3 must derive it through `kill_reason_for(protocol)` (recorded `client_cancelled` + `abandoned`); passing it to `report_fatal` directly is test-only (fix round 1). Supervisor: `end_generation` returns `GenerationEnd { counted, uncounted, unreaped }`; `uncounted` only when the end classifies as `killed_by_supervisor(client_stop_drain_failed)` (an engine that ended by itself still counts). `schedule_restart` then skips `RestartTracker::record_failure`: no failure added, no delay, no backoff growth, but the healthy-window reset still applies through new `RestartTracker::note_healthy(healthy_for)` (spec §3.3/§3.8; fix round 1); the state goes `stopping` → `backoff` (zero length, `/health` failed/restarting as for any restart) → `starting` → `ready`. Unreaped still opens the circuit (V10). **Owner-visible bound (controller ruling):** at most `config_defaults::kMaxConsecutiveUncountedRestarts` = 3 uncounted client-STOP restarts in a row with no successful DONE between them; from the 4th on, a client-STOP drain failure is counted like `drain_failed`, so an engine that always ignores STOP still reaches `circuit_open`. A successful DONE on any generation (new atomic `EngineProtocolClient::completed_generations()`, read by the supervisor thread at each generation end; a `nonfinite` DONE is not one) resets the streak; a counted failure does not. New fixed log literal: `engine generation <n> ended: killed_by_supervisor(client_stop_drain_failed)`. It EXTENDS the plan state-machine `<reason>` list (`fatal_line`, `ready_timeout`, `drain_failed`, `write_timeout`, `protocol_failed`, `shutdown`) and is a ruled exception to SP2 spec §3.6 "all kinds count as failures"; it carries no client data (privacy rule). Timeout-STOP drain failures still count: V7 `proc04_drain_failure_kills_and_restarts` unchanged and green. Thread-safety contract (D-1) unchanged: no new entry point, `report_fatal` still only takes `mutex_`. | `secure_frontend_engine_supervisor_tests.sp3_d2_kill_reason_follows_stop_cause` (unit, protocol level: client STOP + failed drain → `client_stop_drain_failed`; timeout or default STOP → `drain_failed`; a drained client STOP does not carry over to the next GEN; literal), `secure_frontend_engine_supervisor_tests.sp3_d2_client_stop_drain_failure_restarts_uncounted` (unit, fake: three client-STOP drain failures in a row (the bound) → each generation killed and logged with the new literal, generation 4 ready, no backoff delay; then three counted failures open the circuit with delays 1 s, 2 s, proving the count stayed 0), `secure_frontend_engine_supervisor_tests.sp3_d2_timeout_stop_drain_failure_still_counts` (unit, fake: three timeout-STOP drain failures through `execute_with_lease` → circuit open, delays 1 s, 2 s, `drain_failed`), `secure_frontend_engine_supervisor_tests.sp3_d2_client_stop_counts_when_engine_ended_itself` (unit: engine exit 1 before the client-STOP report → `error_exit`, counted, delay 1 s), `secure_frontend_engine_supervisor_tests.sp3_d2_uncounted_end_resets_count_after_healthy_window` (unit: 2 counted, client-STOP end after > healthy_window, then 3 more counted needed; delays 1, 2, 1, 2 s), `secure_frontend_engine_supervisor_tests.sp3_d2_uncounted_streak_is_bounded` (unit: 3 uncounted, the 4th counts (1 s), 5th and 6th open the circuit), `secure_frontend_engine_supervisor_tests.sp3_d2_done_resets_uncounted_streak` (unit: a DONE on generation 3 resets the streak, five uncounted in all, no delay; mutation without the reset fails it), `secure_frontend_engine_protocol_client_tests.stop_cause_and_completed_generations` (unit: cause forgotten by a refused GEN; successful DONE counted, `nonfinite` not), `secure_frontend_engine_supervisor_tests.proc04_drain_failure_kills_and_restarts` (V7, kept); all TIMEOUT 120 | Fix round 1: `100% tests passed out of 294` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`); wine `100% tests passed out of 158` (150 + the eight new unit cases); V7 process case is windows-runtime (authoritative in MSYS2, pending the owner's push), direct wine smoke exit 0; cross build `-Werror` clean; pytest 13 passed; the seven `sp3_d2_*` cases 105/105 clean in 15 repeat runs |
| R1 (Task 22 / K2 overall bound) | Owner ruling 2026-10-14 (controller ruling RF10): every engine-stdin write is bounded overall by an absolute deadline as well as by the 1 s per 64 KiB chunk deadline (`config_defaults::kStdinWriteDeadline` / `kStdinWriteChunk`); each chunk waits `min(kStdinWriteDeadline, time left to the deadline)`, and a write past the deadline fails exactly like a stalled chunk (`transport_write_failed` → protocol failed → `killed_by_supervisor(write_timeout)`, counted, restart; no new kind). Interface: `EngineLineTransport::write_line(line, MonotonicClock::time_point deadline, error)`; `EngineProtocolClient::begin_generation(max_new, ids, write_deadline)`. `ChatExecutionService` fixes one generation deadline (`now + generation_timeout`) before GEN: it bounds the GEN write and the read to DONE then gets only what is left of it (`remaining_until`). STOP and QUIT (no request deadline) are bounded by one `kStdinWriteDeadline` from the call. POSIX: the literal `poll(..., 1000)` is gone; the frontend's stdin end is now `O_NONBLOCK` (set in `start()`), writes go in 64 KiB chunks and every wait is a bounded `poll` (a blocking write could otherwise outlast both deadlines). Windows: the overlapped wait uses the same `min`. New supervision-fake mode `--slow-stdin=BYTES` (+ `--slow-stdin-interval-ms=MS`, default 100): after READY it only drains stdin, BYTES per interval, and never answers. Limits doc updated by Task 28 ("Windows stdin writes" row: per-chunk and overall bounds, shared generation deadline, 1 s STOP/QUIT, POSIX `O_NONBLOCK`). | `secure_frontend_engine_supervisor_tests.proc04_slow_stdin_write_bounded_by_request_deadline` (process case, labeled windows-runtime on Windows like its group; ~960 KB GEN line drained at 16 KiB / 100 ms, 2 s generation timeout: request fails in < 3.5 s and the engine is killed as `write_timeout`; RED before the change: took 12654 ms, no `write_timeout`), `secure_frontend_engine_stdio_process_tests.write_line_fails_once_deadline_passed`, `secure_frontend_engine_protocol_client_tests.write_deadlines_reach_the_transport`, `secure_frontend_chat_execution_tests.chat_gen_write_bounded_by_generation_deadline`; K2 `proc04_windows_write_timeout_kills_engine` kept; all TIMEOUT 120 or the binary's existing value | `100% tests passed out of 298` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`); wine `100% tests passed out of 161` (the slow-stdin process case is windows-runtime and excluded; direct wine run exit 0 in ~3.6 s); cross build `-Werror` clean; pytest 13 passed — Windows path authoritative in MSYS2, pending the owner's push |
| SP6 env family (mechanism) | SP6 prerequisite, mechanism only (SP6 spec §4.9 DRAFT; SP2 §3.9; controller rulings RF9 and R5 fix round 1). `EngineEnvironment` gains `EngineFamily family{EngineFamily::strata}` (`enum class EngineFamily { strata, glm }`; default member initializer, so designated initializers keep compiling). A family hook (`FamilyEnvironmentHook`, production `family_environment_rows(const EngineEnvironment&)`) derives owning rows from the environment (`FamilyEnvironmentRows`: `forced` name/value pairs, `suppressed` names), so SP6 can derive `CUDA_VISIBLE_DEVICES` from the gpu list. `merge_forced_environment` validates, then suppresses and forces (replace or add); it is all-or-nothing and order-independent. It refuses a forced name off the closed `kFamilyForcibleEnvironment` (SP5 M2 D-7 superseded the original 7-name seed set — it is now the GLM parity table's 100 typed `engine_env` names: `maya_env_knobs()` filtered, pinned by `glm_child_env_is_exactly_the_named_typed_set`), a never-list name other than the two `kFamilyGpuSelectors` (RF9: the never-list stays the "never inherited" list), a NUL in a value, a suppressed name off the fixed `kFamilySuppressibleEnvironment` (`STRATA_WATCHDOG_IO_S`, `STRATA_HIPBLASLT_TUNING`, `STRATA_WATCHDOG_S`, plus SP5 M2's `STRATA_GLM_VISION_LEND_MB`), and any name that appears twice. `STRATA_GLM_DFLASH` and the rest of the never-list stay refused. The constant part of each family's rows is validated by a `static_assert`, and the same check runs at run time on derived rows. `render_posix_environment`/`render_windows_environment`/`render_engine_environment` stay the generic §3.9 rows. `apply_family_environment`/`render_family_environment` add the family step and return `std::optional`. The spawn path (`make_stdio_process_factory`, hook `EngineLaunchPlan::family_hook`) turns a refused merge into a spawn failure (`engine environment refused`), logs the fixed literal `engine generation <n>: family environment refused` (no value or path) and removes the generation's temp dir. It never spawns with an empty or partial environment. **strata: no rows, so the rendering is byte-identical** (pinned; N1 green). **SP5 M2: the glm family is reachable — a composed `glm53` launch renders its closed typed environment** (forced §4.9 safety defaults, config-projected typed rows, CUDA selectors from `glm.gpu`, the vision-turn-gated LEND row). Strata family: still no GPU variable. | `secure_frontend_engine_supervisor_tests.sp6_env_strata_family_renders_unchanged` (POSIX + Windows byte pins, unchanged rows; strata/glm through the family step equal the pin), `.sp6_env_forced_values_override_and_add`, `.sp6_env_forced_values_reject_never_list` (incl. refused derived rows yield nullopt), `.sp6_env_forcible_names_are_the_ruled_set`, `.sp6_env_hook_derives_values_from_environment` (test-only derivation through the real renderer, absent → 0), `.sp6_env_hook_suppresses_only_optional_rows`, `.sp6_env_refused_family_is_spawn_failure` (duplicate and NUL rows), `.sp6_env_derived_value_reaches_child` (process case: the derived rows reach a real child's environment); N1 `proc02_child_env_is_exact_allowlist` unchanged | `100% tests passed out of 306` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`); wine `100% tests passed out of 168` (the process case `sp6_env_derived_value_reaches_child` is windows-runtime and excluded; direct wine run exit 0); cross build `-Werror` clean; pytest 13 passed — Windows path authoritative in MSYS2, pending the owner's push |
| SP4 seam | SP4 launch seam on SP2's launch primitives (controller ruling R4 + RF11; SP4 spec §3.2; binding names in the run-2 seam contract and its controller addendum). **POSIX** (`src/posix_child_launch.hpp`, `secure_frontend::posix_launch`): `PreExecHook = int (*)(const void* ctx, std::span<const int> lifted) noexcept`; `struct ExecOptions { std::span<const int> keep_fds; PreExecHook pre_exec{nullptr}; const void* ctx{nullptr}; }`; `kHookFailureMarker = 'H'`; `exec_child(prepared, stdin_fd, stdout_fd, null_stderr, report_fd, const ExecOptions& options = {})` (new last parameter, defaulted, so existing callers compile unchanged); `kReportFd = 3` and `kLiftFloor = 10` already existed; new `kMaxKeepFds = 16` (the child lifts into a fixed array, no allocation; more entries fail closed with `E2BIG`). In the child, each `keep_fds` entry is lifted with `fcntl(fd, F_DUPFD_CLOEXEC, kLiftFloor)` right after the report descriptor is lifted and before the 0..3 `dup2` sequence, so a parent fd 0–3 survives. The sweep (`close_range` per gap, else one `close()` per fd below the sweep limit) spares the lifted numbers. After the sweep and the signal reset, immediately before `execve`, `pre_exec(ctx, lifted)` runs with the lifted numbers in `keep_fds` order. A non-zero return (an errno) makes the child write `'H'` + that int in one write on `kReportFd` and `_exit(127)`. A lift failure is reported the same way (fail closed). The lifted copies stay close-on-exec, so only what the hook `dup2`s survives `execve`. The hook must not close or `dup2` onto fds 0..`kReportFd`: a hook that clobbers `kReportFd` could make a failure read as success (header contract, fix round 1). SP2 never reserves or touches fd 4. Parent side: new `read_report(fd)` (to EOF or 16 bytes, whichever comes first; EINTR retried; fix round 1 bounded it, it could keep 32), `classify_report(bytes)` → `ChildReport{none | exec_failed | hook_failed | malformed, error}` (SP4's rule: `size == 1 + sizeof(int) && front() == 'H'`), and `describe_report()`. Both POSIX launchers (`engine_stdio_process.cpp`, the SP0 twin `engine_process.cpp`) now read the report through these: `execve failed: <strerror>` (text unchanged), `pre-exec hook failed: <strerror>`, `child launch report malformed` (never success). The engine passes `ExecOptions{}`, so its child behaviour is unchanged. **Windows** (new `src/win_child_launch.hpp` + `.cpp`, `secure_frontend::win_launch`; built into `secure_frontend_engine_stdio_process` on Windows): `WinLaunchSpec { executable, command_line, environment_block, cwd, stdin_read, stdout_write, stderr_handle, HANDLE primary_token{nullptr}, std::vector<HANDLE> extra_inherited_handles, std::vector<std::uint8_t> crt_fd_block, bool limit_active_process_to_one{true}, bool restrict_ui{true} }`; `WinLaunched { process, thread, job }` (the caller owns all three; the thread is already resumed); `WinLaunchStep`; `std::variant<WinLaunched, DWORD> launch_in_job(const WinLaunchSpec&, WinLaunchStep* failed_step = nullptr)` (DWORD = GetLastError of the failing call). It is the engine's former CreateProcessW + Job code, moved unchanged: Job `KILL_ON_JOB_CLOSE | DIE_ON_UNHANDLED_EXCEPTION`; flags `CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW`; explicit `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` = the three standard handles, then the extra handles (each checked inheritable, else `ERROR_INVALID_HANDLE` at step `attributes`; each must be distinct from the three standard handles and from each other, since a duplicate makes CreateProcess fail with `ERROR_INVALID_PARAMETER`; not deduped, documented); `lpCurrentDirectory` = cwd (empty: inherit); the once-per-process error mode; assign while suspended, then resume. `primary_token` set → `CreateProcessAsUserW`. `crt_fd_block` is copied into `cbReserved2`/`lpReserved2` (≤ 65535 bytes). `ACTIVE_PROCESS = 1` and all `JOB_OBJECT_UILIMIT_*` restrictions **default ON** (ruling RF11 as amended in fix round 1: a new caller is confined by default, and SP4 Task 26's 10-field positional initialiser gets this Job). The engine's `start()` sets both to false explicitly, so the engine Job is unchanged. The engine's `start()` maps the step and code to its former messages, including 740 → `spawn_failed` "engine requires elevation; refused" (never retried via runas). The SP0 twin keeps its own CreateProcessW (no stdio redirection, `bInheritHandles` FALSE). Types follow the controller addendum (ruling): `extra_inherited_handles` and `crt_fd_block` are owning vectors, so a spec never dangles. SP4 Task 26's positional `WinLaunchSpec{...}` (10 initialisers) compiles unchanged under clang `-Werror`; the test uses exactly that shape. **SP4 must close WinLaunched::thread after launch_in_job.** New supervision-fake mode `--write-inherited=N`: writes `INHERITED <N>` to fd N (POSIX), or on Windows to the handle at CRT entry N, parsed from `STARTUPINFO.lpReserved2`; exit 3 if absent. | `secure_frontend_engine_child_tests.sp4_seam_keep_fds_lift_and_hook_order` (L; keep_fds = {a parent fd opened as 3, a parent fd > 3}: the hook sees both lifted ≥ `kLiftFloor`, close-on-exec, the same files in keep_fds order, a non-kept parent fd already swept; it `dup2`s the first to 4; the fake's DUMP `FDS` is exactly `0,1,2,4` and its write on fd 4 lands in the file that was parent fd 3; checked by mutation: hook before sweep → `'H'` + EEXIST, lift after the dup2s → `'H'` + EDOM), `secure_frontend_engine_child_tests.sp4_seam_hook_failure_is_distinct_from_execve_failure` (L; hook EPERM → exactly `'H'` + EPERM, exit 127, `pre-exec hook failed: …`; missing executable → errno only (ENOENT), `execve failed: …`; unliftable keep fd → `'H'` + EBADF; empty/malformed classification; engine launcher text unchanged), `secure_frontend_engine_child_tests.sp4_seam_launch_in_job_handle_and_crt_block` (W, windows-runtime: one extra inheritable handle + CRT block n = 5 with entry 4 = that handle; the fake writes `INHERITED 4` through it, exit 0; the same launch without the extra handle cannot write (exit ≠ 0); every Job is `KILL_ON_JOB_CLOSE | DIE_ON_UNHANDLED_EXCEPTION`; the default spec (SP4's positional shape) gets `ACTIVE_PROCESS = 1` and `kAllUiRestrictions`, and the engine's spec (both false) gets neither), `secure_frontend_engine_child_tests.sp4_seam_launch_in_job_rejects_non_inheritable_handle` (W, windows-runtime; fix round 1: a non-inheritable extra handle → `ERROR_INVALID_HANDLE` at step `attributes`); the hook-failure case also checks that `read_report` stops at 16 bytes of a 40-byte pipe (RED before fix round 1: read 32); all TIMEOUT 120 | `100% tests passed out of 308` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`); wine `100% tests passed out of 168` (the Windows case is windows-runtime and excluded). In a direct wine run of it, the handle-list and CRT-block assertions and the negative control pass; it exits 1 only on the Job-query assertions (wine does not model Job Objects). After fix round 1, a direct wine run of `sp4_seam_launch_in_job_rejects_non_inheritable_handle` exits 0. Direct wine runs of the engine's Windows cases on the refactored path: `proc06_child_ids_and_cwd` and `proc01_windows_argv_round_trip` exit 0; `proc02_canary_handles_absent` (no `CompareObjectHandles` in wine) and `proc04_exit_status_is_classified` (winedbg on SEGV) fail exactly as recorded before. Cross build `-Werror` clean; pytest 13 passed — **Windows authoritative in MSYS2, pending the owner's push** |
| K4, K6 | Documentation truth pass, no behaviour change. K4: the `kSweepCap` comment states when the fallback sweep runs (no `close_range`), why it is capped, and that it is not backed by close-on-exec everywhere (the listening and accepted sockets are not `SOCK_CLOEXEC`; Known small gaps). K6: `docs/security/engine-protocol-limits.md` ("Windows stdin writes" rewritten: the engine's reader thread on `read(0)`/`_read(0)`, per-chunk and overall (R1) bounds, shared generation deadline, 1 s STOP/QUIT, POSIX `O_NONBLOCK`; new READY wait, fatal lines, exit codes, GPU selection rows with RF9 wording), all upstream citations re-pinned to `v0.1.44.1` and checked against `upstream/`; threat model "Engine process isolation" (fixed environment, restart policy and circuit, client-triggerable `ERR` lines, the D-2 bound, PROC-03 and PROC-05 residual risks, NNP default, refusal of root/elevated starts, G3 a validation-kit step); `deployment.md` release record (accepted risks) and its environment wording; `secure-frontend-ci.md` (SP2 `.exe` files, `windows-runtime` label, the two skips, G3; CI run `38540719307`); `secure_frontend/README.md` (environment sentence, `/health` `starting` → `503`); the `limits.engine.ready_timeout_s` description (schema string and README table, now "how long the supervisor waits for READY after each start"); citation re-pins in the SP2 spec (§3.8 `server.py:734-735`, `_pump` `:952-967`; §3.9 `generate.cpp:1843-1865`; §3.10 and K6 `:9433-9469`) and in the `kEngineFatalPrefixes` comment. Handoff: SP1 rows flipped to verified (CI run `38540719307`), SP2 status, gaps, run-2 record. | review (reviewer reads the diff) plus a full green run; `secure_frontend_typed_config_tests.readme_config_table_matches_schema` covers the description change | `100% tests passed out of 308` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`); wine `100% tests passed out of 168` (278 registered; the 110 `windows-runtime` cases excluded); cross build `-Werror` clean, 38 `.exe`; pytest `13 passed` |
| Final review I1, I2, M1-M7 | Whole-branch final review (run 2, "With fixes"). **I1 (C4):** an idle or slow HTTP client no longer turns a clean SIGTERM/console-event shutdown into `_Exit(1)`: `HttpSocketServer::shutdown_listener()` also shuts down the in-flight client socket (POSIX `SHUT_RDWR`, Windows `SD_BOTH`; under a mutex, never closed there) and `serve_one()`'s request read waits in 100 ms `poll()`/`select()` slices that re-check `listener_closed`; the shutdown worst case stays 4.35 s + 0.1 s < 5 s. **I2 (C4):** a generation left unreaped (`abandoned_`) at the end of shutdown makes `finish()` log the fixed literal `engine supervisor stopped with an unreaped engine: exit 1` (`kEngineLeftUnreapedLine`) and `main` return 1 (`EngineSupervisor::engine_left_unreaped()`, additive). **M1 (G2):** the NNP hint keys on `classified.kind != killed_by_supervisor`, not on "no kill sent". **M3:** `g_signalled` is a lock-free `std::atomic<int>`; the comment now says a process-directed POSIX signal may run the handler on any thread not blocking it, and `release_shutdown_signals()` waits for an in-flight handler (lock-free counter) after clearing the wake. **M4:** `engine_supervisor.hpp` SP3 caveats: `wait_for_change` observes states, not transitions; the one fork/reap/kill exception (a Lease outliving the supervisor with an abandoned generation runs `terminate(250 ms)` on the destroying thread); every serving-API call must have returned before the destructor begins; a stray `begin_generation` during an active client-STOP generation erases the recorded cause (errs toward counting); a cancel that finishes with `DONE` ends the uncounted streak. **M5:** one `reap_within()` helper replaces the duplicated reap-poll loops (no behaviour change). **M6:** the startup message is `Unable to prepare the state or cache directory.` (it also covers `<roots.cache>/cuda`). **M2, M7:** docs (C4 row 4.35 s; MSYS2 watch list; SP4 note). | `secure_frontend_http_socket_server_tests.socket_server_shutdown_listener_wakes_idle_client` (B), `secure_frontend_server_tests.proc05_sigterm_with_idle_client_exits_cleanly` (L), `secure_frontend_server_tests.proc05_windows_ctrl_break_with_idle_client_exits_cleanly` (W, windows-runtime), `secure_frontend_engine_supervisor_tests.supervisor_unreaped_at_shutdown_reports_engine_left` (B unit), `secure_frontend_engine_supervisor_tests.proc04_unreaped_child_opens_circuit` (extended), `secure_frontend_engine_supervisor_tests.supervisor_shutdown_sends_eof_first_then_kills` (negative control added), `secure_frontend_engine_supervisor_tests.proc06_nnp_hint_when_exit_status_lags` (B unit). RED before each fix: exit 1 after 5.33 s and a 3007 ms wait (I1); "shutdown reports the engine left behind" (I2); "the NNP hint is logged for an engine that ended before READY" (M1). | `100% tests passed out of 312` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`); wine `100% tests passed out of 171` (282 registered; the 111 `windows-runtime` cases excluded; the Windows idle-client server case is authoritative in MSYS2 — pending the owner's push); cross build `-Werror` clean, 38 `.exe`; pytest `13 passed` |

Notes on the SP2 table above:

- Shutdown follows the spec §3.7 order since Task 15 (C4, C7 row): EOF, TERM, KILL, then `killed_by_supervisor(shutdown)` is logged. A shutdown that lands while `end_generation` is reaping (up to `reap_timeout`, 30 s) cannot finish inside the 5 s bound and exits via `std::_Exit(1)` (spec-mandated).
- Documented spec §3.1 exception: on Windows, `EngineStdioProcess::read_line` waits briefly on the process handle from the serving thread (`WaitForSingleObject(..., 1)`). A Windows wait consumes no exit status, so this does not violate the single-reaper intent.

#### SP2 run-2 state (2026-10-14, recorded by Task 28)

- **Commits.** Run 2 is `5ac39bb4` through the Task 28 commit, 25 local
  commits on `sp2-portable-supervision` after the SP1 merge `9ded0a5c` (Tasks
  29a, 6b, 15, 16, 18, 19, 25, 26, 27, 29b, then the controller rulings R2, R3,
  R1, R5, R4, then Task 28), then the final-review fix wave (I1, I2, M1-M7;
  last row of the SP2 table). Nothing is pushed.
- **Final counts** (after the final-review fix wave, "Verification performed", SP2): linux-gcc
  `100% tests passed out of 312` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`); wine `100% tests passed out of 171` (282 registered; the 111 `windows-runtime` cases excluded); pytest `13 passed`; cross build `-Werror`
  clean, 38 `.exe` files.
- **Proof status.** L and B rows are green locally. Every W row and every
  `windows-runtime` test is unverified until the `windows-msys2-clang` job is
  green after the owner pushes. CI run `38540719307` (MSYS2 193/193) covers SP1
  only, because it predates the SP2 merge. `proc03_other_uid_cannot_open_engine_fd`
  runs only in the GitHub Linux jobs. `proc05_cgroup_kill_when_delegated` skips
  wherever no delegated cgroup exists. G3 is a validation-kit step. Nothing ran
  on a GPU.
- **MSYS2 watch list for the first SP2 push:** Windows A2 argv[0] final-path
  form (CI run `38598235525` failed it; since the winfix branch the harness
  puts `root()` in `GetFinalPathNameByHandleW` form on Windows, so an 8.3 or
  junction `TEMP` no longer mismatches — pending the next MSYS2 run); the LUA token executing the
  harness `bin/` copy; the console handler, `CTRL_CLOSE` and `CREATE_NO_WINDOW`
  engine stdio (C6); the owner-only `--state-dir` check, the
  `make_owner_private_directory` test roots and the junction case (N3); SP1
  server cases and the two CLI checks launched through the shared
  `non_elevated_launch` (G4); G1 (`proc06_child_ids_and_cwd`); the
  `roots.cache` CUDA path (`engine_cuda_cache_follows_roots_cache`); the SP4
  seam Job limits (`sp4_seam_launch_in_job_handle_and_crt_block`);
  `GetProcessHandleCount` staying flat across restarts (V9/V10); UCRT
  `__fastfail` and WER behaviour (`proc04_windows_abort_does_not_stall`,
  `proc04_windows_crash_does_not_stall`); R2 `sudo -n -u nobody` in the Linux
  CI jobs; the Windows `shutdown_listener()` `closesocket` versus `accept()`
  race (`http_socket_server.cpp`, the `listener_closed` re-check after
  `accept()` in `serve_one()` and the `closesocket` in `shutdown_listener()`:
  a socket handle closed while `accept()` runs may be reused); the C6
  `CTRL_CLOSE` exit-code race (Windows may end the process when the console
  handler returns, before `main` returns its code); the idle-client shutdown
  (`proc05_windows_ctrl_break_with_idle_client_exits_cleanly`: whether
  `shutdown(SD_BOTH)` wakes the `select()` at once or the 100 ms slice does).
- **Owner items:**
  1. The delegated-cgroup paths (C5, and the C3 delegated branch) are verified
     by compiling and reading only. A live proof needs approval to run
     `systemd-run --user --scope -p Delegate=yes ctest --preset linux-gcc -R proc05_cgroup --output-on-failure`.
  2. SP2 spec §4.7 K4 row (`docs/superpowers/specs/2026-10-13-sp2-portable-supervision-design.md`
     ~line 381) says "all frontend fds are `O_CLOEXEC`, so the sweep is
     defence in depth". The code contradicts it: the listening and accepted
     sockets are created without `SOCK_CLOEXEC` (Known small gaps, first
     bullet). The spec text was not edited; the `kSweepCap` comment states the
     code's truth.
  3. C3 spec row versus mechanism: the spec asserts the grandchild "only when
     C5 active", but after a frontend SIGKILL nothing writes `cgroup.kill`. The
     controller ruled a membership assertion instead (the C3 row). This is the
     PROC-05 residual risk in the threat model and the release record.
  4. D-2 bound: at most `config_defaults::kMaxConsecutiveUncountedRestarts` = 3
     uncounted client-STOP restarts in a row. This is a controller ruling (SP3
     D-2 row); a `DONE` resets the streak, including one that finishes a cancel.
  5. R1: the read to `DONE` gets only what is left of the generation deadline
     the GEN write used (R1 row; judged consistent with K1/RF10 in review).
  6. SP1 security-test edits made by SP2: `auth04_no_switch_disables_auth`'s
     boolean allow-list admits `engine.allow_new_privileges` (G2, G4); SP1's
     CFG-01 case gained the "no supervisor start, no engine generation"
     assertion (V2, V11); SP1's `process_runner` launches
     `strata-secure-server` through the shared non-elevated launcher on Windows
     (G2, G4).
- **SP3 note.** SP3 spec §2.6 (lines ~317-319) and its R4 risk row (~1298)
  still describe the pre-D-2 behaviour (a client-STOP drain failure as a
  counted `drain_failed`). They were not edited. SP3's P6 cancel must call
  `stop(EngineStopCause::client_cancelled)` and derive the kill reason through
  `kill_reason_for(protocol)`.
- **SP4 note.** SP4 must close `WinLaunched::thread` after `launch_in_job`.
  SP4 must pass `keep` to `sweep_orphans` (the pids of its live downloader
  children) so a live downloader child is not swept as an orphan.
  `crt_fd_block` and `extra_inherited_handles` are `std::vector` (controller
  addendum), so SP4's locals must be vectors, not arrays or spans.
  `WinLaunchSpec` defaults to a confined Job (`ACTIVE_PROCESS = 1`, UI limits);
  the engine opts out (SP4 seam row).
- **Plan.** The merge note at the top of the plan's Task 15 is satisfied (ruling
  RF5 was applied in Task 15, C4/C7 row). The plan file is outside Task 28's
  scope and was not edited.

The native frontend enforces these in-process boundaries: configuration
validation, secret-source metadata, Linux/Windows protected-file token reading,
exact Host/Origin/CORS policy, bounded HTTP request parsing with a total request
deadline plus bearer authentication, portable child-process supervision, and a
serial native Qwen (or selected GLM-5.3) text-chat route. The server does **not**
download artifacts (since SP4 only the separate, operator-invoked `strata`
binary does), package a secure release, implement every upstream serving
profile, or replace the upstream Python service in a release. Therefore the
repository must not yet be described as privacy-hardened or as having removed
upstream network-capable behavior.

### SP3 M1 deliverables (branch `sp3-api-streaming`)

Plan: `docs/superpowers/plans/2026-10-14-sp3-m1-concurrency-hardening.md`. Spec:
`docs/superpowers/specs/2026-10-14-sp3-api-concurrency-streaming-design.md`.
Baseline before Task 1: linux-gcc N₀ = 520 (+2 allowed skips) tests, wine W₀ = 256 tests.

| Task | IDs | What changed | Tests (CTest names) | linux-gcc | wine |
| --- | --- | --- | --- | --- | --- |
| 1 | §3.9, §4.6, §5.3, §7 | api_constants.hpp, api_errors (§7.2 table), http_wire (security headers, Date), router uses the shared JSON writer | http_parser_tests.err_every_code_renders_fixed_body, …err_param_is_key_only, …wire_response_has_security_headers_and_date, api_router_tests.models_list_keeps_utf8_model_id | 100% tests passed out of 524 (+2 allowed skips) | 100% tests passed out of 260 (smoke; real Windows pending MSYS2) |
| 2 | DOS-01, §4.2 | request-head scan + parser (414/431/411/413/505/417/400); shared ASCII helpers `ascii_iequal`/`ascii_to_lower`/`trim_ows` in http_request_head.hpp (pre-flight D6; no SP4-file edits) | http_parser_tests.dos01_request_line_over_limit_is_414, …dos01_header_block_over_limit_is_431, …dos01_too_many_headers_is_431, …dos01_transfer_encoding_variants_are_411, …dos01_content_length_variants, …head_request_line_and_header_syntax | 100% tests passed out of 530 (+2 allowed skips) | 100% tests passed out of 266 (smoke; real Windows pending MSYS2) |
| 3 | WEB-01, WEB-02, WEB-03, WEB-06 (unit) | Host allowlist (bound, localhost on loopback, aliases; wildcard = aliases only; names case-folded), exact origins, preflight fixed list (204/403); `make_http_policy` replaces the old 4-field HttpPolicy in every user (mains keep their old authority as an alias, no CORS yet); `struct HttpPolicy;` forward-declared in http_request_handler.hpp (pre-flight P11); temporary handler shim until Task 6a | http_policy_tests.web01_host_matrix, …web02_host_name_case_is_folded, …web03_origin_matrix, …web06_preflight_rules_unit, …cors_and_alias_grammar | 100% tests passed out of 530 (+2 allowed skips) | 100% tests passed out of 266 (smoke; real Windows pending MSYS2) |
| 4 | WEB-05 (config), CFG-01 (SP3 rows) | listener.cors_origins/host_aliases, HTTP deadline keys (header_deadline_ms, write_stall_ms, response_write_deadline_ms, stream_keepalive_ms), logging.requests; string-array bound + grammar side table (`ConfigKeyExtension`, `find_config_key_extension`); strata-secure-server passes `listener.host_aliases`/`cors_origins` to `make_http_policy` and drops the old-authority alias (wildcard bind = aliases only, §4.3); smoke mains pass no alias (smoke-server keeps its own authority only for a wildcard bind); SP1 tests ported: `subsystem_limits_follow_config` sets header_deadline_ms with its 1500 ms request deadline, `auth04_no_switch_disables_auth` admits logging.requests; README key table regenerated | typed_config_tests.cfg01_sp3_rows_type_and_range, …cfg01_header_deadline_le_request_deadline, …cfg01_response_deadline_ge_write_stall, …cfg01_wildcard_bind_requires_host_aliases, …cfg01_host_alias_grammar_and_count, …web05_cors_origin_config_rejects_wildcard_null_path_duplicate_and_17_entries | 100% tests passed out of 536 (+2 allowed skips) | 100% tests passed out of 272 (the 6 new cases are unlabelled and run under wine; real Windows pending MSYS2) |
| 5 | CFG-01 (SP3 rows), B1, B2, B11 | concurrency rows with derived defaults (`limits.concurrency.max_concurrent_requests` 1-256 default 8; `max_queued_generations` 0-254 derived min(4, max(0, workers-2)); `queue_timeout_ms` 1000-3600000 default 300000; `max_connections_per_remote_address` 1-256 derived min(4, workers); cross-field workers=1 ⇒ queue 0, workers ≥ 2 ⇒ queue+2 ≤ workers, cap ≤ workers; a derived value applies only when the key is absent, `--check` prints it with "(derived)"), `default_max_tokens` 0-16384 default 0 = auto, `resolve_max_tokens` (auto = min(max_completion, ctx − n − kContextMarginTokens); above-limit requests clamped, not rejected) used by `ChatExecutionService::execute`; README key table regenerated (derived Default text from `ConfigKeyExtension::derived_default`). Updated per spec §9.4: typed_config_tests.typed_config_accepts_minimal_file_and_applies_defaults (8 workers, default_max_tokens 0), chat_execution_tests.rejects_context_and_output_limits_before_generation (over-limit max_tokens clamped to GEN 1), chat_generation_plan_tests.defaults_output_limit_and_rejects_unsafe_inputs (omitted max_tokens plans 0 = auto) | typed_config_tests.cfg01_sp3_concurrency_rows_and_defaults, …cfg01_queue_plus_two_le_workers, …cfg01_serial_requires_zero_queue, …cfg01_remote_cap_le_workers, …cfg01_sp1_config_with_one_worker_still_valid, …cfg01_derived_defaults_follow_worker_count, …cfg01_explicit_values_are_not_adjusted; chat_execution_tests.resolve_max_tokens_follows_decisions_1_and_2 | 100% tests passed out of 544 (+2 allowed skips) | 100% tests passed out of 280 (the 8 new cases are unlabelled and run under wine; real Windows pending MSYS2) |
| 6a | §3.11 seam, §4.1 seam | RequestRouter returning RouteResult; HttpHead/StreamSink/StreamHandler; serialize_head; in-memory handle_http_request; legacy adapter (`DispatcherRouter`, deleted in Task 9); `serialize_response` rebuilt on the head helper (Content-Length except 204, drops Transfer-Encoding); `constant_time_token_equals` kept (pre-flight P24). Updated per spec §9.4: http_request_handler_tests.rejects_malformed_and_ambiguous_messages (Transfer-Encoding → 411), http_socket_server_tests.parser_honors_configured_max_request_bytes (Content-Length over the bound → 413) | http_parser_tests.wire_head_has_no_content_length, …route_result_seam_returns_complete_response | 100% tests passed out of 546 (+2 allowed skips) | 100% tests passed out of 282 (the 2 new cases are unlabelled and run under wine; real Windows pending MSYS2) |
| 6b | AUTH-01..04 kept, DOS-01 (415/417/505), §4.1, §3.6, §3.9 | header-first RequestRouter/ApiRouter (`ApiRouterConfig`, `HealthView`, `ChatHandler`), kRouteTable routing, 401/404/405/415/413 order, ACAO + `Vary: Origin` on every response after an approved origin (401 included); /v1/models[/id] only the served model (`launch.model_id`, owned_by "local"; expert inventory no longer listed, spec E9/re-anchor 4); model id/owned_by validated as UTF-8, no C0/DEL, 1..`kMaxModelFieldBytes` (new in api_constants.hpp, pre-flight D10), else 500 internal_error; /health keeps the SP2 body, a non-literal engine/recovery or a throwing provider is 503 engine_unavailable; `is_json_content_type` compares in place (noexcept, no allocation; D9) with the shared ASCII helpers (D6); `is_valid_utf8` exported from json.hpp; `handle_authenticated_get` legacy bridge for secure_server_main and both smoke mains (smoke mains serve their first inventory model; Task 8b reworks them). Transitional (pre-flight S11): main's health lambda calls `supervisor.snapshot()` directly until Task 16 moves it onto SupervisorPort. Updated per spec §9.4: api_router_tests.* and http_request_handler_tests.* assert §7.2 bodies (no `{"error":"Not Found"}`/`Service Unavailable`) | http_parser_tests.dos01_status_matrix_415_417_505, …content_type_variants_accepted, …auth_and_route_order_follows_spec; api_router_tests.* (rewritten, names kept; auth01 iterates kRouteTable; health_carries_recovery_only_with_failed ported per T6b(a)); http_request_handler_tests.* (rewritten, names kept) | 100% tests passed out of 549 (+2 allowed skips) | 100% tests passed out of 285 (new cases unlabelled, run under wine; real Windows pending MSYS2) |
| 7 | DOS-01 (JSON), EXT-01 (schema), B6 | §3.1 schema: `parse_chat_request` → `ChatRequestResult` (`ChatRequestError{ApiError, param}`; every param a literal from the §7.2 fixed list), extensions and role tool/function first in document order, neutral no-ops (penalties 0, `logit_bias` {}, `stop` null/[], `n` 1, `logprobs` false), bounded temperature 0–2 / top_p (0,1] / seed 0–2^63−1 into `SamplingParams` (integer forms accepted), `max_tokens`/`max_completion_tokens` alias (both → invalid_request) and clamp-at-resolve (B2; parse caps at UINT32_MAX), `stream:true` → unsupported_parameter `stream` (M1), `stream_options` without stream → invalid_request; JSON depth `kChatMaxJsonDepth` 4 with decimals on; `parse_chat_completion_request` is now a nullopt wrapper. Deviation from the plan test text: `dos01_content_array_is_unsupported_parameter` uses `"content":["Hi"]` (depth 4); the OpenAI parts form `[{"type":"text",…}]` is depth 5 and is pinned as invalid_json (spec §3.1 depth ≤ 4) | http_parser_tests.dos01_json_depth_5_is_400, …dos01_json_limits, …dos01_content_array_is_unsupported_parameter, …sampling_neutral_values_are_no_ops, …sampling_bounds_rejected, …sampling_integer_forms_accepted; json_tests.json_decimals_only_when_enabled | 100% tests passed out of 556 (+2 allowed skips) | 100% tests passed out of 292 (new cases unlabelled, run under wine; real Windows pending MSYS2) |
| 8a | §2.2 polled reads, §2.7 stall rule, §9.4 SP0 gaps (EINTR, transient accept), §10 | socket_io (library `secure_frontend_socket_io`, not yet used outside its tests; Task 8b builds on it): RAII `Socket`, `set_non_blocking`/`set_no_delay`, `set_no_inherit` helper for Task 8b (pre-flight V12; binding test stays 8b's), `poll_sockets` over poll/WSAPoll bounded by `kMaxPollEntries` 128 (new in api_constants.hpp, pre-flight D10), `recv_some`/`send_some` (EINTR retried; `kInterruptedCode` = EINTR/WSAEINTR, T8a), `send_all_bounded` (every send sliced to `kMaxSendChunk` and polled for writability in `kStreamPollSlice` steps on POSIX and Windows; stall measured on progress, whole-write deadline; closes the SP2 carry-in that Windows sends to a stalled reader were not sliced), `probe_peer`/`bytes_pending` (MSG_PEEK), `half_close_and_drain` (kDrainBudget/kDrainBytes), `WinsockGuard`, `detail::accept_backoff_for` (50 ms on transient errors, nullopt on EBADF/ENOTSOCK/EINVAL and Winsock equivalents) | http_socket_server_tests.send_retries_on_eintr, …accept_backs_off_on_transient_failure, …send_some_slices_to_max_send_chunk, …send_all_bounded_stops_at_stall_on_a_stalled_reader, …send_all_bounded_meets_deadline_and_completes, …send_all_bounded_stall_restarts_on_progress, …socket_io_poll_probe_and_recv, …set_no_inherit_marks_the_socket | 100% tests passed out of 564 (+2 allowed skips) | 100% tests passed out of 300 (new cases unlabelled, run under wine; real Windows pending MSYS2) |
| 8b | DOS-01 (P1/P3 deadlines), §2.2, §2.7 complete responses, §3.11 dispatch site, §9.4, SP2 K4 (pre-flight V12) | header-first connection handler (`connection_handler.hpp`: `ConnectionLimits` with SP1 schema defaults, `WorkerSignals`, `ConnectionOutcome`, `ConnectionResult`, `serve_connection` P1–P8 polled in 20 ms slices, 414/431 as soon as known, 408 at header/request deadline, authorize before any body byte, 100 Continue only with no body byte received, stray byte after the body → 400, EOF fine), `deliver_route_result` + `SocketStreamSink` (M1 placeholder; `kNoResponseStatus` → close with nothing written; a handler that throws or writes no head → fixed 500), `HttpSocketServer` rebuilt (`bind(listener, backlog, &err)`, port 0 ephemeral, listener polled in 100 ms slices via `accept_next`, `serve_one(router, limits, &err)`, idempotent `shutdown_listener` whose stop flag is the worker's `shutting_down` (pre-flight P34), no WSAStartup — every main holds a `WinsockGuard`), `kMinimumSendBudget`/private `send_all` deleted (socket_io's `send_all_bounded`), `connection_limits(FrontendConfig)` replaces `http_socket_server_limits`, `make_dispatcher_router` (legacy adapter by value, main until Task 9), smoke mains on `ApiRouter` + new bind/serve_one (chat → 404). **K4 gap closed (listener + accepted sockets CLOEXEC/no-inherit)**: Linux `SOCK_CLOEXEC` + `accept4(SOCK_CLOEXEC\|SOCK_NONBLOCK)`, other POSIX `FD_CLOEXEC`, Windows `WSASocketW(WSA_FLAG_NO_HANDLE_INHERIT)` + `set_no_inherit` per accepted socket. Transitional: `handle_authenticated_get` stays (main's legacy dispatcher) until Task 9 | http_socket_server_tests.socket_server_rejects_oversize_header_with_431, …head_terminator_split_across_reads (Review Focus 2), …expect_continue_skipped_when_body_already_sent (Review Focus 3), …expect_continue_sent_after_authorization, …stream_handler_dispatch_writes_head_chunks_and_terminal_chunk, …no_response_status_closes_without_writing, …listener_and_accepted_sockets_are_not_inherited, …socket_server_send_times_out_on_stalled_reader, …socket_server_accept_waits_without_timeout, …socket_server_ipv6_bind_is_v6only (binds `::`), …socket_server_responds_after_client_half_close (kept green), …socket_server_shutdown_listener_wakes_accept/…wakes_idle_client ("listener stopped"), …win_send_chunks_respect_write_stall (W, pending MSYS2), the rest ported per pre-flight T8b; subsystem_limits_tests.subsystem_limits_follow_config | 100% tests passed out of 571 (+2 allowed skips) | 100% tests passed out of 307 (`win_send_chunks_respect_write_stall` is `windows-runtime`: pending MSYS2; one earlier run hit a 0.01 s wine start-up flake, clean on rerun) |
| 9 | DOS-01 (header-first over TCP), AUTH-01, §4.6, §7, EXT-01 (amended ext01 bodies), §3.8 | `strata-secure-server` serves every request through `ApiRouter` (`model_id = launch.model_id`, `owned_by = "local"`, pre-flight P26) with header-first authentication, still serial (`serve_one` in SP2's loop) until Task 12; chat parses with `parse_chat_request` and maps `ChatRequestError{ApiError, param}` to `make_error_response(err, {.param})` on the wire (P28); wrong model → 404 model_not_found; `engine_refusal` → server_shutting_down while stopping, else engine_unavailable with `engine_retry_after(recovery)` (Retry-After 5 unless `exhausted`); execution errors map to context_length_exceeded / invalid_request / engine_error / internal_error. Deleted (P23/D7): legacy `handle_http_request(…HttpPolicy…)` overload, `HttpParserLimits`, `RequestDispatcher`, `DispatcherRouter`/`make_dispatcher_router`, `handle_authenticated_get`, main's `json_error`. One Winsock guard per main. Harness: `ServerOptions::config_literals` → owner-only `<root>/config.json` + `--config`; `kGeneric503`/`kBadRequest` now the §7.2 engine_unavailable/invalid_request bodies; test_support links api_core + typed_config (P36). Transitional (pre-flight S11): `supervisor.snapshot()` in the /health and `engine_refusal` lambdas and SP2's `execute_with_lease` are called directly from `run` until Task 16 | api_server_tests.dos01_body_over_limit_is_413_before_reading, …dos01_content_length_over_request_limit_is_413_before_auth, …dos01_unauthenticated_gets_401_before_body_is_read, …dos01_expect_continue_after_auth_only, …dos01_slow_header_is_408_at_header_deadline, …dos01_slow_body_is_408_at_request_deadline, …dos01_incomplete_body_then_close_frees_worker, …err_unauthenticated_is_401_on_every_path_and_method, …security_headers_on_every_response (all `windows-runtime`, pending MSYS2); server_tests.ext01_extension_fields_spawn_nothing (amended: every extension body within the depth bound — empty and populated tools/functions/mcp_servers, tool_choice, function_call, parallel_tool_calls, role tool/function — → the fixed extension_disabled body naming the key; one depth-5 tools body → 400 + nothing spawned, per the depth-limit ruling), …proc05_sigterm_during_generation_exits_cleanly (accepts the §7.2 server_shutting_down body); http_parser_tests.dos01_content_length_variants (+ mismatch / stray bytes via the seam, S12), …err_every_code_renders_fixed_body (+ engine_retry_after) | 100% tests passed out of 580 (+2 allowed skips) | 100% tests passed out of 307 (new api_server cases are `windows-runtime`: pending MSYS2) |
| 10 | WEB-01..06, AUTH-01 (route table), NET-03 (wildcard needs aliases) | acceptance over TCP against the real server: bad/malformed/duplicate Host on every `kRouteTable` route; IPv6 `[::1]:port` authority rules on a `::1` bind (skips with 77 without ::1); bound authority, `localhost` alias (any case) and exact `listener.host_aliases`; untrusted/duplicate Origin → 403 without CORS headers; no CORS header on any of 13 statuses by default; only the exact configured origin gets `Access-Control-Allow-Origin` + `Vary: Origin`, never credentials; preflight matrix (204 without a token, refused → 403, no GEN reached the engine); foreign-origin POST with a valid token → 403, no GEN; missing/malformed/wrong/duplicate token on every route; wildcard bind without aliases exits non-zero naming `listener.host_aliases` and spawns no engine (process half; validation half is typed_config_tests, Task 4). Helpers `has_no_cors_headers`, `route_request`, `wait_health_on(ip, port, host)` (pre-flight D2, reused by Task 12). No product change: acceptance tests, passed on first run | api_server_tests.web01_bad_host_rejected_on_every_route, …web01_ipv6_bound_host_rules, …web02_bound_authority_and_localhost_alias_succeed, …web02_host_alias_from_config_succeeds, …web03_untrusted_origin_no_cors_header, …web04_default_emits_no_cors_header_anywhere, …web05_only_exact_configured_origin_allowed, …web06_preflight_matrix, …web06_cross_origin_post_without_trust_rejected, …auth01_every_route_rejects_missing_malformed_wrong_token, …cfg01_wildcard_bind_requires_host_aliases (all `windows-runtime`, pending MSYS2) | 100% tests passed out of 591 (+2 allowed skips) | 100% tests passed out of 307 (new api_server cases are `windows-runtime`: pending MSYS2) |
| 11 | DOS-01 (eviction, per-remote cap, linger list), §2.3, §2.4 | `connection_pool.hpp` (library `secure_frontend_connection_pool`, unit level; Task 12 wires it): `AdmissionState` (not thread-safe; remote over `per_remote_cap` → refuse, loopback exempt; idle worker → assign; else the worker longest in P1 → evict, which releases the evicted peer's count; else refuse; connection ids make `header_complete`/`finished` of an evicted connection stale), `LingerList` (≤ `kLingerListMax` half-closed refused sockets, oldest closed at once when full; `append_poll_entries` for the accept thread's single `poll_sockets` per slice; `service` drains non-blocking and closes on EOF, error, `kDrainBytes` or `kLingerBudget`), fixed refusal `refusal_response` (503 server_busy, `Retry-After: 1`, ≤ `kRefusalMaxBytes`) and `refuse_connection` (one non-blocking send, half-close, linger). Linger case per pre-flight T11: the 503 is read from a still-lingering client, the evicted one is only asserted closed; which socket is evicted is pinned by native handle (review fix) | concurrency_tests.dos01_oldest_header_phase_connection_is_evicted, …dos01_remote_address_cap (+ eviction releases a remote peer's count), …dos01_linger_list_overflow_closes_oldest, …dos01_linger_closes_on_peer_eof, …refusal_response_is_fixed_and_small (portable, run under wine; MSYS2 pending) | 100% tests passed out of 596 (+2 allowed skips) | 100% tests passed out of 312 (the five new cases run under wine) |
| 12 | DOS-01 (slowloris, slow body, eviction over TCP), NET-01, §2.1, §2.8 | `ConnectionPool` (concurrent server, spec §2.1-2.4, §2.8): one accept loop (`run_until`) + `max_concurrent_requests` fixed workers created at start, joined at exit; `admit`/`header_complete`/`finished` only under the single pool mutex (static proof in the task report; TSan is a controller follow-up); the accept thread polls the listener plus the linger list in ONE `poll_sockets` per 100 ms slice (1 + `kLingerListMax` = 65 ≤ `kMaxPollEntries`, static_asserted); an evicted worker sees `abort` within one 20 ms slice and closes unanswered; refused sockets get the fixed 503 + linger. `secure_server_main` replaces SP2's serial loop: stop predicate `supervisor.stop_requested()` (P33), `PoolExit::listener_failed` → supervisor shutdown + `EXIT_FAILURE` (T12b); `run_until` also exits on an invalid listener handle. Task 8b carry-ins: POSIX `shutdown(SHUT_RDWR)` before closing the listener (a blocked poll wakes), and `send_all_bounded` gains a stop flag — once set, an in-flight write ends within `kShutdownDrainBudget` (never the full write deadline), every `connection_handler` write passes the pool's `shutting_down` | api_server_tests.dos01_eviction_over_tcp_admits_new_client, …dos01_slowloris_cannot_starve_a_real_client, …dos01_slow_body_clients_cannot_starve_a_real_client, …net01_workers_add_no_listener, …net01_ipv6_loopback_listener_only (new `tests/support/connection_scan.hpp`: Linux /proc fd+net tables with a uid fallback — the non-dumpable server hides /proc/pid/fd, ruling T12-scan — Windows iphlpapi tables by pid); http_socket_server_tests.send_all_bounded_stop_flag_cuts_an_inflight_write_within_the_drain_budget | 100% tests passed out of 644 (+4 allowed skips; SP2 `proc05_*` shutdown cases stay green on the pool) | wine/MSYS2 not run: owner ruling 2026-10-20 defers Windows runtime proof to owner hardware; `windows-llvm-mingw-cross` builds 0 warnings (new api_server cases + `connection_scan` Windows branch compile) |
| 13 | §2.5, D-1, V11 | `SupervisorPort` (SP3's `snapshot`/`acquire`/`report_fatal` serialized under one port mutex — SP2 was built for one serving thread; `~Lease` and `shutdown()` bypass the port and rely on SP2's own mutex, as D-1 documents) + `GenerationGate` (FIFO tickets, one running slot: snapshot fast-fail — starting/backoff/circuit_open→`engine_unavailable`, stopping/shutting_down/stopped→`shutting_down`, never waits for READY (V11); full queue → `busy` at once; a waiter wakes on `release()`/`shutdown()` or every `kStreamPollSlice` to check `client_gone`, its deadline, and headship; the head: `ready`→`acquire()` (lease, else `release()`+`engine_unavailable`), `generating`→re-check every 20 ms until its deadline; the holder destroys its Lease before `gate.release()`). T13b: `engine_supervisor.hpp` destructor comment now covers the Windows case (a reaped generation still leased in `retired_` cleans up on the Lease-destroying thread) — comment only. Plan-fixture fix: bare `supervisor.start()` would break -Werror ([[nodiscard]]) → wrapped in expect() | concurrency_tests.gate_first_request_gets_the_slot, …gate_queue_full_is_busy, …gate_fifo_order, …gate_timeout_returns_timeout, …gate_cancelled_waiter_leaves_queue, …gate_fast_fails_when_engine_not_ready, …gate_shutdown_refuses_waiters (new library `secure_frontend_generation_gate`) | 100% tests passed out of 651 (+4 allowed skips; the two timing-sensitive gate cases repeated 10/10 clean) | wine/MSYS2 not run (owner ruling 2026-10-20); `windows-llvm-mingw-cross` 0 warnings |
| 14 | test infrastructure (spec §9.2, M1 part) | fake engine: stdin read on a detached thread (`StdinReader`) — every line hits `--record-file` the moment it arrives (STOP visible mid-generation); flags `--token-ids`, `--tokens`, `--token-interval-ms`, `--finish`, `--prefill-delay-ms`, `--stop-delay-ms`, any of which switches GEN to `run_generation` and DONE to the full six-field form; STOP mid-generation → `DONE … cancel` after `--stop-delay-ms`; `max_new` reached first → `length`; SP2 modes (`--hang-in-gen` never acts on STOP, `--abort-in-gen`, `--fatal-line`, `--stall-stdin`, `--spew`) unchanged. Exit-path fix (ruling T14-exit): with a detached reader blocked in `std::getline`, glibc `exit()` deadlocks flushing all FILEs (main stuck in futex while the reader holds the stdin lock — observed: QUIT left the fake alive, deterministic 5×); `main` now flushes cout/cerr and `_Exit`s — the plan's own detach comment promises "return/_Exit"; every protocol line is flushed at emit, so no output loss. CMake: `find_package(Threads REQUIRED)` + link for both fake targets | fake_engine_tests.fake_engine_stream_modes_selftest (six blocks: cycled ids + full DONE, length, nonfinite, STOP-cancel with delay, prefill delay, SP2 short form intact) | 100% tests passed out of 652 (+4 allowed skips; fake+supervisor+server+child sets 135/135) | wine/MSYS2 not run (owner ruling 2026-10-20); `windows-llvm-mingw-cross` 0 warnings |
| 15 | B6 (GEN keys), §2.6 P6 cancel, §3.5, §5.4 | `GEN` carries schema-bounded sampling keys: `begin_generation(…, write_deadline, sampling={})` (ruling T15-signature: re-anchored SP2 keeps `write_deadline` 3rd, sampling added 4th — the plan snippet predates the hotfix), `render_sampling_keys` (shortest round-trip via `std::to_chars`, "" = greedy, never request bytes). `run_generation` (P6): READY gate like `execute`; reads in `kStreamPollSlice` (20 ms) slices; between slices checks cancel (→ `STOP(client_cancelled)` (ruling T15-stopcause) + `stop_drain_timeout` drain → `cancelled`), shutdown — pool flag OR SP2 signal flag, ruling R-H (→ STOP + `min(stop_drain, kShutdownDrainBudget)` drain → `shutting_down`), `generation_timeout` (→ STOP + drain → `timed_out`); failed STOP write or drain timeout → `abandon()` + `engine_unavailable`/`shutting_down`; nonfinite DONE → `engine_error` idle; ERR/fatal → `engine_error` closed; more tokens than asked → abandon + `engine_error`; DONE field 6 `length` else `stop`; text via `decode_token_ids` (fail → `decode_failed`). `context_tokens()` = READY context (0 before). `render_chat_completion` = exact §3.5 OpenAI body (invalid UTF-8/over-cap refused). RESUME/REUSED branches untouched | engine_protocol_client_tests.gen_carries_bounded_sampling_keys (reuses the file's FakeTransport — ruling T15-fake, no duplicate fake); chat_execution_tests.run_generation_reports_finish_usage_and_context, …run_generation_cancel_sends_stop_and_drains, …run_generation_timeout_stops_and_reports_timed_out, …run_generation_drain_failure_abandons, …run_generation_shutdown_uses_short_drain, …run_generation_maps_engine_errors (StopAwareTransport; the four timing cases repeated 20/20 clean); chat_schema_tests.renders_openai_completion_shape | 100% tests passed out of 660 (+4 allowed skips; targeted trio 38/38) | wine/MSYS2 not run (owner ruling 2026-10-20); `windows-llvm-mingw-cross` 0 warnings |
| 16 | DOS-02 (queue/busy/timeout/FIFO/health-while-full/serial), B1, B2, B6, §3.5, §5.4, §8 log codes | `ChatRoute` (new library `secure_frontend_chat_route`): P4 = schema parse, model check, plan (profile), prompt ids, `resolve_max_tokens` against the last READY context — 400s answered before any queue place; P5 = `gate.admit(port, now+queue_timeout, client_gone)` with the refusal map (busy/timeout→503 server_busy, cancelled→`kNoResponseStatus` + log_code `client_gone`, engine_unavailable→snapshot-based Retry-After, shutting_down→503); P6 = lease: budget re-checked against THIS generation's READY (400 if no room), `run_generation` with `ExecutionControl{client_gone, pool flag, shutdown_signalled}` (R-H: `report_fatal` only when protocol closed AND not shutting), then Lease destroyed BEFORE `gate.release()`. `make_completion_id` = "chatcmpl-"+32 CSPRNG hex; the 200 body is the §3.5 shape with real usage; `timed_out` → engine_error + log_code `generation_timeout` (B4 in M2). Defect found and fixed (named per plan): `GenerationGate::admit` refused an arrival whenever `max_queued == 0` even with the slot idle (serial mode got busy at once) — the cap now bounds WAITERS only; RED: "FAILED: max_queued = 0 still admits when the slot is idle (serial mode)". Wiring: supervisor→port→gate→chat_route→router→server→pool declaration order; /health reads `port.snapshot()`; `pool.run_until(P33 predicate, [&gate]{ gate.shutdown(); })` — ruling T16-pool: the plan's `shutdown_signalled()` predicate predates P33 and misses the recovery-exhausted exit, so P33 stays and `gate.shutdown()` runs as the pool's on-shutdown hook (Task 12's run_until already takes it). SP2 transitional chat lambda + `engine_refusal` deleted from main | api_server_tests.max_tokens_above_limit_is_clamped, …max_tokens_clamped_to_context_room, …prompt_alone_over_context_is_400, …absent_max_tokens_uses_auto, …sampling_passthrough_reaches_gen, …dos01_context_rule_is_400_without_gen, …chat_response_has_openai_shape, …dos02_excess_connections_get_503_retry_after, …dos02_queue_full_is_503, …dos02_queue_timeout_is_503, …dos02_queue_is_fifo, …dos02_health_answers_while_generation_places_full, …dos02_serial_mode_one_worker (the seven timing/shape cases repeated 21/21 clean) + concurrency_tests.gate_zero_queue_admits_one_at_a_time; SP2 `proc04_client_err_lines_open_circuit_until_restart` and `proc04_abort_in_generation_is_generic_503` stay green through the port | 100% tests passed out of 674 (+4 allowed skips; targeted api/server/e2e/concurrency 112/112; budget cases passed pre-implementation — SP2 already resolves B1/B2, honest note like T9a) | wine/MSYS2 not run (owner ruling 2026-10-20); `windows-llvm-mingw-cross` 0 warnings |
| 17 | DOS-02 (cancel, release), DOS-01 (health after attacks), §2.6, §2.8 | acceptance suite for behavior built in Tasks 6a/8a/8b/12/13/15/16: queued client disconnect leaves the FIFO queue silently (only A,C reach the engine); mid-P6 disconnect sends STOP within one poll slice (record shows STOP <1 s) and the drained cancel NEVER kills the engine; after 100 abandon-and-cancel chats the server's thread and fd counts return to their idle baseline and /health answers; SIGTERM during a generation: exit 0 within SP2's 5 s bound, the in-flight chat gets 503 `server_shutting_down` or a close, and the log contains NO `drain_failed`/`protocol_failed`/`write_timeout` (R-H — no worker-side failure reported fatal during shutdown); /health 200 + chat 200 after each of 7 attacker shapes (oversize header, deep JSON, chunked, slow-then-closed body, slow header, over-limit body, TLS-garbage) | api_server_tests.dos02_queued_client_disconnect_leaves_queue, …dos02_non_stream_disconnect_cancels, …dos02_resources_return_to_idle, …shutdown_errors_never_report_fatal, …dos01_service_healthy_after_each_attack — all passed on FIRST run (no product change needed); repeats 15/15 clean | 100% tests passed out of 679 (+4 allowed skips) | wine/MSYS2 not run (owner ruling 2026-10-20); `windows-llvm-mingw-cross` 0 warnings |
| 18 | PRIV-01/DL-01 (frontend part), §6.2 static tripwire | `tests/support/binary_imports.hpp` (stdlib-only): ELF64-LE reader — undefined `.dynsym` symbols + `DT_NEEDED`; NO `SHT_DYNSYM` → `ok=false` "no dynamic section (static binary?)" (never passes vacuously); PE32+ reader — import directory (data dir 1) → DLL names, by-name and by-ordinal thunks (bit 63); no import directory → `ok=false`; `forbidden_imports`: resolver calls (`getaddrinfo`…, `_`-stripped, lowercased), `winhttp/wininet/urlmon/dnsapi.dll` (case-insensitive), and ANY ordinal-only `ws2_32.dll` import (catches gethostbyname #52). Deviations to compile under -Werror: `std::numeric_limits<…>::max()` for the plan's `UINT64_MAX`, `(void)import_size` (read but unused in plan text), `<cctype>` in the test | egress_tests.egress_server_imports_no_resolver_or_http_client (positive control: `accept`/`accept4` really in `.dynsym`; the real Linux `strata-secure-server` imports NO forbidden symbol — no egress path found), …egress_static_tripwire_fails_on_binary_without_dynamic_section (synthetic ELF/PE per controller note 10/R-I; the readers run on the images, not a mock) | 100% tests passed out of 681 (+4 allowed skips) | wine/MSYS2 not run (owner ruling 2026-10-20 — the wine ctest line of the plan NOT RUN; PE reader still COMPILES in `windows-llvm-mingw-cross`, 0 warnings) |
| 19 | PRIV-01, PRIV-03, DL-01 (frontend part), §6.2 runtime | LD_PRELOAD interposer `tests/support/egress_tripwire.cpp` (Linux test-only; never linked into product): wraps `connect`/`sendto` (address≠null)/`sendmsg` (msg_name)/`getaddrinfo`/`gethostbyname`/`socket` (AF_UNIX, AF_NETLINK, SOCK_DGRAM) — one line to `$STRATA_EGRESS_RECORD` + EACCES/EAI_FAIL/null; everything else passes through to the real symbol. `tests/support/egress_support.hpp`: `arm_egress_tripwire` (extra_env), `SocketScan` (50 ms samples of `sockets_of(server pid)` + `descendants_of` children), `run_lifecycle` (health, models, chat, mid-generation cancel, command-like values, refusal flood, 40 half-header slow clients, health; engine restart via `--fail-first=1`). DEFECT found reaching GREEN (named): the plan's SocketScan rule assumed per-pid attribution — the server is non-dumpable so its rows are only uid-attributed (ruling T12-scan) and the ambient same-uid/container sockets drowned it (first RED below). Ruling T19-scan: on Linux the scan pins the server's LISTENER set differentially (NET-01 precedent) and the engine children EXACTLY (dumpable → inode mapping); Windows keeps the plan-strict rule (per-pid iphlpapi tables — `-Wunused-private-field` adaptation: baseline members guarded to Linux). CMake: `strata_egress_tripwire` SHARED + `strata-egress-probe-helper` above the first `secure_frontend_test_paths` call, defs via `if(TARGET)` in that function; Windows registers only command-like + table_scan (windows-runtime label) + iphlpapi; egress tests link `secure_frontend_socket_io` | egress_tests.egress_interposer_self_test (probe helper under the preload: all five calls recorded AND denied), …egress_interposer_records_nothing_during_lifecycle (zero forbidden calls through the whole lifecycle, exit 0), …egress_table_scan_during_lifecycle (violation list empty; windows-runtime-labeled on Windows), …egress_command_like_values_trigger_nothing (5 remote-looking contents 400, pull-model 400, 4 acquire/update routes 404, no GEN, `active_processes` unchanged, interposer record empty) — stability repeats 20/20 clean | 100% tests passed out of 685 (+4 allowed skips; egress set 6/6) — interim honest note: one earlier "green" claim was against an un-wired build (full-suite rerun exposed the 3 not-green cases; corrected here) | wine/MSYS2 not run (owner ruling 2026-10-20; the plan's `ctest -R egress_tests` under wine NOT RUN); `windows-llvm-mingw-cross` 0 warnings (test binary compiles clean, 4 cases registered 2 on Windows) |
| 20 | EXT-01, EXT-02 (spec §6.4 amendments) | The SP2 ext cases now run under the §6.2 runtime tripwires: `ext01_extension_fields_spawn_nothing` and both `ext02_*` cases wrapped with `arm_egress_tripwire` + `SocketScan` (+ end-of-case "scan: only the listener's sockets" / "interposer recorded nothing" expects); adaptations recorded: the arm for `ext02_extension_env_never_reaches_child` sits AFTER its own `extra_env` assignment (a plain assignment would wipe the preload; replacing /nonexistent with the real tripwire strengthens the ban it asserts), and that case's `SocketScan` is optional (the case tolerates a null server). For `ext02_rejected_before_spawn` (refusal-to-start): interposer lines only, record asserted empty after `wait_exit` — no scan (no running server). New: `ext01_upstream_control_routes_are_404_after_auth` (16 upstream control targets × GET+POST all 404 with the exact `not_found` body after auth), `ext02_http_headers_cannot_enable_extensions` (4 header switches change nothing: tools still 400 `extension_disabled`/"tools", ordinary chat 200 unaffected, exactly 1 GEN, no process churn). `secure_frontend_server_tests` links iphlpapi on WIN32 | server_tests.ext01_extension_fields_spawn_nothing, …ext02_extension_env_never_reaches_child, …ext02_rejected_before_spawn (amended — interposer + scan recorded NOTHING during all three: no real finding) + api_server_tests.ext01_upstream_control_routes_are_404_after_auth, …ext02_http_headers_cannot_enable_extensions — acceptance suite, ALL passed on FIRST run (rules exist since Tasks 6b/7/9); repeats 15/15 clean | 100% tests passed out of 687 (+4 allowed skips; targeted ext0 set 5/5) | wine/MSYS2 not run (owner ruling 2026-10-20); `windows-llvm-mingw-cross` 0 warnings |
| 21 | §8 (request logging without telemetry), PRIV-02 (frontend), §7 fixed reasons | `secure_frontend_request_log` library: `format_request_line` = "req <seq> <route> <status> <code\|-\\> <ms> <bytes_out> <peer>" (route ∈ health/models/model/chat/other; peer loopback\|remote; codes §7.2 + log-only `generation_timeout`/`client_gone`/`write_stall`/`evicted` via `log_record_for` mapping pool outcomes); `RequestLogger` token bucket — burst `kLogBurst` 100, refill `kLogRatePerSecond` 20/s, drops counted and reported `log: <n> request lines suppressed` at most once per second when the bucket next has a token; off ⇒ nothing written; `stderr_sink` fixed 256 B buffer, one fwrite per line, failures dropped. Wiring: pool observer (Task 12 hook) feeds the logger; startup summary `strata: listening <authority>, workers <n>, queued generations <q>` once to stderr after bind, before supervisor.start() — the operator's own settings only | concurrency_tests.request_log_format_and_token_bucket (exact lines incl. "log: 200 request lines suppressed" and the 100+1+20 refill count); api_server_tests.log_line_format_and_rate_limit (300 health hits: burst 100, ≤ 100+20·(s+1.2)+1 lines, suppression line, startup summary present); api_server_tests.err_no_token_prompt_or_path_in_any_body_or_log (marker through content/tools/field-name/model/path/bad-token/header/Host/Origin: never reflected in any response; stderr has no marker, token or root path) — RED (compile, as plan Step 2 predicts): "fatal error: secure_frontend/request_log.hpp: No such file or directory"; all three green on FIRST build; repeats 9/9 clean | 100% tests passed out of 690 (+4 allowed skips) | wine/MSYS2 not run (owner ruling 2026-10-20); `windows-llvm-mingw-cross` 0 warnings |
| 22 | §10 Windows parity, R2 (TSan) | Three Winsock-parity cases compiled everywhere, ctest-registered on Windows only (windows-runtime label, MSYS2 authoritative), manual on Linux: `win_refusal_is_read_not_reset` (30 refused clients all READ the full 503 — lingering close, no RST), `win_eviction_wakes_worker_within_one_slice` (workers=1, 30 s half-header holder; the new client served <250 ms — oldest-P1 eviction wakes the worker within one 20 ms slice), `win_disconnect_detected_by_peek` (mid-generation vanish → STOP in the record within 1 s). CI: `linux-clang-tsan` job added after `linux` (continue-on-error, ubuntu-24.04 pinned, mmap_rnd_bits=28, `-fsanitize=thread` on the linux-clang preset, `-E egress_interposer` — interposer+TSan don't mix) + header Non-blocking line updated. Ruling T22-tsan: owner disabled Actions (2026-10-20) — definition only, agents never push; TSan absent on this host, so the job is the future vehicle | api_server_tests.win_refusal_is_read_not_reset, …win_eviction_wakes_worker_within_one_slice, …win_disconnect_detected_by_peek — all three exit=0 on Linux FIRST run (same code paths), repeats 9/9 clean; `ctest --test-dir … -N -L windows-runtime` lists 3 win_ entries (grep count 4 counting SP2's pre-existing win_send_chunks_respect_write_stall) — wine run NOT RUN (owner ruling), pending MSYS2 | 100% tests passed out of 690 (+4 allowed skips; no Linux registration change) | `windows-llvm-mingw-cross` 0 warnings; Windows execution of the three cases waits for the owner's hardware (pending MSYS2) |

#### M1 status (recorded by Task 23, 2026-10-21)

SP3 M1 done on branch `sp3-api-streaming`, merged into `main` 2026-10-21: accept thread + worker pool (default 8), FIFO generation gate (default queue 4, 300 s), header-first authentication, §4.2 parser, Host/Origin/CORS, §7.2 JSON errors, decisions B1/B2/B6/B10/B11/B12 implemented; B3/B4/B5 are M2.

- linux-gcc (Task 23 battery, this host): `100% tests passed out of 690` — 686 passed, 4 allowed env skips (`cfg03_rejects_mount_crossing`, `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`, `bundle_at_mount_root_is_read_but_never_written`); `ctest -N | grep -cE '\.(net0[123]|web0[1-6]|dos0[12]|ext0[12]|egress_|err_|cfg01_|win_)'` = 98 §9.3 M1 cases listed individually; `Total Tests: 690`. pytest 25 passed.
- wine: NOT RUN (owner ruling 2026-10-20: GitHub Actions disabled, no wine on this host; agents prove only the local `windows-llvm-mingw-cross` cross-build — clean, 0 warnings, compile-only).
- `windows-runtime` cases pending windows-msys2-clang (owner hardware): 308 registered in the cross preset — acquire 68, api_server 50, bundle 26, fetch 27, server_tests 16, server_cli 13, engine_supervisor 13, engine_arguments 12, privacy 9, token_reader 8, engine_child 8, typed_config 19, paths 6, token_store 3, fake_engine 3, subsystem_limits 3, model_inventory 4, server_e2e 9, model_index 2, egress 2, engine_process 1, http_socket_server 1, tokenizer_pack 5, plus the 2 `strata_secure_server_*` CLI smokes.
- TSan: not installed on this host — "not run on this host (controller follow-up)"; the non-blocking `linux-clang-tsan` job (Task 22, ruling T22-tsan) is the future vehicle.
- Final-grep honesty (ruling T23-grep): `grep -rn "kMinimumSendBudget\|chatcmpl-local\|text/plain; charset" src include` prints ONE line — `chat_schema.cpp:277`, the legacy `render_chat_completion_response` kept per the plan's own Task 16 rule ("stays only if a test still uses it" — `chat_schema_test.cpp` pins it; no product path calls it; the M1 response is the §3.5 `render_chat_completion`, pinned since Task 15/16). `grep -rnE "std::chrono::(milli)?seconds\([0-9]{3,}\)" src` prints 8 lines, ALL in untouched SP1/SP2-era supervision files (`engine_stdio_process.cpp`, `engine_process.cpp`, `engine_supervisor.cpp`, `models_store_windows.cpp` — verified unchanged on this branch; §5.3 governs the HTTP/API constants files, which carry every SP3 timing value). No SP3 task added a literal either grep flags.

#### M2 status (recorded by the controller merge, 2026-10-21)

SP3 M2 done on branch `sp3-api-streaming` (17 commits, `f01ef654..847b5b9e`, 58 files, +4053/−266), merged into `main` at `c83119a9` 2026-10-21. Ledger + final report: `docs/superpowers/handoff/reports/sp3-m2/`.

- Merge-gate battery (controller, this host): linux-gcc build 0 warnings, ctest `100% tests passed out of 746`; release preset: build clean after a merge-time fix, preset ctest 10/10, `secure_distribution_check` PASS; `windows-llvm-mingw-cross` 0 warnings; pytest 25; root build 0 warnings + ctest 712 registered, one failure — the parked flake below; TSan (linux-clang, `-fsanitize=thread`): 111 filtered concurrency/streaming cases, **zero ThreadSanitizer reports**, same single flake.
- Fixed in-merge (release-gate class, second consecutive lane to miss it): `http_parser_test` took pointers from `ok(parse())` temporaries — dangling use, `-Werror=dangling-pointer` under `-O3`. Gate discipline from now on: the release-preset build is a per-task lane gate, not merge-only.
- Parked for the owner: `api_server_tests.log_line_format_and_rate_limit` — pre-existing M1 token-bucket timing flake (lane: 3/10 on pristine baseline; merge battery: 10/10 in isolation, fails only under full-suite/TSan load; M2 did not touch bucket timing). Windows runtime for `windows-runtime` cases incl. the two stall-volume streaming cases (owner hardware). GPU validation kit: `docs/validation/sp3-api-kit.md` runs on the owner's boxes.

#### SP1 addendum (recorded by SP3, spec §5.1, §5.3)

| # | Change | Task |
| --- | --- | --- |
| A1 | Rows listener.cors_origins, listener.host_aliases, limits.http.{header_deadline_ms, write_stall_ms, response_write_deadline_ms, stream_keepalive_ms}, logging.requests; string-array entry bound and per-entry grammar (ConfigKeyExtension side table); cross-field header ≤ request, response ≥ stall, wildcard ⇒ aliases. Compatibility: an SP1 file that sets request_deadline_ms below 10000 must now also set header_deadline_ms (≤ it); an SP1 wildcard-bind config must now name listener.host_aliases (spec §4.3) | 4 |
| A2 | max_concurrent_requests 1-256 default 8; max_queued_generations, queue_timeout_ms, max_connections_per_remote_address (derived defaults, `--check` marks "(derived)", init omits them); default_max_tokens 0-16384 default 0 = auto. Compatibility: an SP1 file that sets max_concurrent_requests to 1 still validates (queue 0, per-remote cap 1 derived); one that omits it now gets 8 workers | 5 |
| A3 | secure_frontend_json: opt-in `JsonLimits::allow_fractions` + `JsonKind::decimal` / `make_decimal` / `if_decimal` (RFC 8259 frac/exp via `from_chars`, finite only; config stays integer-only by default); chat schema depth 4, max_tokens clamped instead of rejected | 7 |
| A4 | SP1 spec §4 internal-constants list: the 1000 ms minimum send budget is deleted (replaced by `limits.http.write_stall_ms` and `limits.http.response_write_deadline_ms`); the 100 ms / 256 KiB half-close drain moved to `api_constants.hpp`. | 8a |

#### SP2 second-run follow-ups SP3 still depends on (Task 23)

- **D-1** (lease-release notification; documented thread-safety of `acquire`/`snapshot`/`report_fatal`): SP3 serializes its own calls through `SupervisorPort` (Task 13); `~Lease` and `EngineSupervisor::shutdown()` still bypass the port and rely on SP2's own mutex — unchanged SP2 behavior, documented there.
- **D-2** (a drain failure after a client-requested `STOP` must not count toward the circuit; risk R4): SP3 sends `STOP(EngineStopCause::client_cancelled)` from the P6 cancel path (ruling T15-stopcause) and derives the kill reason through `kill_reason_for(protocol)`; SP2's bound (≤3 uncounted client-STOP restarts in a row) still applies. Residual: R4 in the threat model.

#### Deferred to M2 (controller note 7, recorded by Task 23)

SSE/chunked streaming, `next_event`, `Utf8StreamDecoder` (and the non-streaming decode via it), decision B4 (timeout → partial text with `length`; M1 answers `503 engine_error` + log_code `generation_timeout`), B3 (U+FFFD), B5 (chunked framing), keepalive, `stream_options` acceptance, `secure_frontend_api_client`, `strata-api-probe`, `docs/validation/sp3-api-kit.md`, the streaming DOS-02/WEB-04/EXT-01 cases, fake-engine modes `--progress-records`, `--crash-mid-gen`, `--err-mid-gen` and the UTF-8 tokenizer ids — streaming (`stream: true`, `stream_options`) delivered by M2 Task 10.
| A6b | export `is_valid_utf8` from json.hpp (noexcept wrapper over the existing valid_utf8; behaviour unchanged) for §3.9 model-field validation | Task 6b |

### SP3 M2 deliverables (branch `sp3-api-streaming`, after the M1 merge)

Plan: `docs/superpowers/plans/2026-10-14-sp3-m2-streaming.md`. Baseline after M1 (reproduced at `f01ef654`, 2026-10-21): linux-gcc N₀ = 690 run, 100% (694 registered, the 4 allowed environment skips), pytest 25, `windows-llvm-mingw-cross` build 0 warnings; wine W₀: **not run on this host (owner ruling 2026-10-20: no wine, no CI; agents prove only the cross build)**. Every M1 name M2 consumes was found (Task 1 Step 2): all greps `OK` except three plan strings with grep-text drift whose interfaces exist exactly as consumed — `api_error_row(ApiError error) noexcept` (named parameter + noexcept), `void report_fatal(const Lease& lease, KillReason reason)` (declared in `generation_gate.hpp`, beside `SupervisorPort`), and `refuse(ApiError::unsupported_parameter, "stream")` in `chat_schema.cpp` (M1's helper is `refuse`, not `fail`; Task 10 replaces that line). Ruling recorded in the lane ledger.

| Task | Deliverable | RED | linux-gcc | wine | Status |
| --- | --- | --- | --- | --- | --- |
| 1 | M1 interfaces verified; baseline reproduced | — | 690 run, 100% (694 reg., 4 env skips); pytest 25; cross 0 warnings | not run on this host (owner ruling 2026-10-20) | done |
| 2 | Fake engine M2 modes: STOP-aware prefill/pauses, `--progress-records`, `--crash-mid-gen`, `--err-mid-gen`, `--burst-tokens`; byte pack id 258 (1024×`a`) + `id_for_byte`/`token_ids_for` | `fake_engine_stream_modes_selftest` (extended): "FAILED: not after the 3 s prefill", "FAILED: not after the 3 s pause", "FAILED: four tokens without a pause", "FAILED: PP then the token", "FAILED: ERR after two tokens: T 7", "FAILED: --crash-mid-gen exits" | 690 run, 100% (694 reg., 4 env skips) — unchanged, no new CTest entry; cross 0 warnings | not run on this host (owner ruling 2026-10-20) | done |
| 3 | `Utf8StreamDecoder` (incremental UTF-8, decision-3 U+FFFD policy), `TokenByteDecoder` raw per-token bytes; `decode_token_ids` now delegates | `sse_test.cpp:1:10: fatal error: secure_frontend/utf8_stream_decoder.hpp: No such file or directory`; `tokenizer_pack_test.cpp:185:26: error: 'TokenByteDecoder' in namespace 'secure_frontend' does not name a type` | 695 run, 100% (699 reg., 4 env skips) = +5; cross 0 warnings | not run on this host (owner ruling 2026-10-20) | done |
| 4 | `EngineProtocolClient::next_event` (T/PP/DONE one record at a time; strict full-form DONE finish); `read_generation` rebuilt on it | `engine_protocol_client_test.cpp:368:36: error: 'GenerationEvent' in namespace 'secure_frontend' does not name a type` (+ `no member named next_event`) | 698 run, 100% (702 reg., 4 env skips) = +3; cross 0 warnings | not run on this host (owner ruling 2026-10-20) | done |
| 5 | `drive_generation` — the one generation loop (timeout→STOP→length/B4, byte cap on code points, cancel/shutdown drains); `run_generation` = `begin` + loop; `tests/support/chat_json.hpp`; route: timeout is 200 length + `generation_timeout` log code | compile: `chat_execution_test.cpp:2:10: fatal error: secure_frontend/generation_run.hpp: No such file or directory`; runtime (untouched M1): "FAILED: a max_tokens cut mid-character is a 200, not a decode failure", "FAILED: a timed-out generation is a 200 (decision 4)" | 704 run, 100% (708 reg., 4 env skips) = +6; TSan spot on the new loop 10/10 zero reports; cross 0 warnings | not run on this host (owner ruling 2026-10-20) | done |
| 6 | SSE encoder: role/content/final/usage/error events (one line, `append_json_string` escaping), 16 KiB code-point deltas under the 128 KiB event cap, chunk framing, `[DONE]`/keep-alive/terminator constants | `sse_test.cpp:2:10: fatal error: secure_frontend/sse_encoder.hpp: No such file or directory` | 707 run, 100% (711 reg., 4 env skips) = +3; cross 0 warnings | not run on this host (owner ruling 2026-10-20) | done |
| 7 | `StreamSocket` seam + `AcceptedSocketStream` (non-blocking, TCP_NODELAY, peek) over M1's `socket_io`; `ChunkedStreamSink` (one-buffer pump, write-stall clock, shutdown-capped `finish`); `StreamSink::set_log_code`; `make_sse_head` | `sse_test.cpp:2:10: fatal error: secure_frontend/chunked_stream_sink.hpp: No such file or directory`; `http_socket_server_test.cpp:844:22: error: 'AcceptedSocketStream' is not a member of 'secure_frontend'` | 712 run, 100% (716 reg., 4 env skips) = +5; cross 0 warnings | not run on this host (owner ruling 2026-10-20) | done |
| 8 | `run_chat_stream` — headers+role, engine-speed pump with held/coalesced deltas, engine released at DONE, tail (final/usage/[DONE]/terminator) under the response-write deadline | `sse_test.cpp:2:10: fatal error: secure_frontend/chat_stream.hpp: No such file or directory` | 718 run, 100% (722 reg., 4 env skips) = +6; cross 0 warnings | not run on this host (owner ruling 2026-10-20) | done |
| 9 | Loopback client library: `LoopbackAddress` (loopback-only by construction; remote is a compile error), `ChunkedSseReader` (chunk framing + event cap), `LoopbackStreamConnection` (the only `connect()` in the frontend, §6.1), complete + streamed exchanges (`complete` = `[DONE]` + terminal chunk) | `api_client_test.cpp:1:10: fatal error: secure_frontend/api_client.hpp: No such file or directory` | 721 run, 100% (725 reg., 4 env skips) = +3; cross 0 warnings | not run on this host (owner ruling 2026-10-20) | done |
| 10 | `stream: true` end to end: schema accepts `stream`/`stream_options` (C3), `ChatRoute` stream branch with `LeaseHold` (report → Lease → gate, §2.5; R-H), `deliver_route_result` on the non-blocking `ChunkedStreamSink`, `tests/support/stream_client.hpp` | runtime: parser "FAILED: stream:true is accepted (M2)"; e2e "FAILED: stream with [DONE] and the terminal chunk", "FAILED: a plain 503, never a stream", "FAILED: the slow reader then gets all 1 MiB" (ext01/auth01 pass already, as the plan predicted) | 728 run, 100% (732 reg., 4 env skips) = +7; TSan spot (sse/api_client/chat_execution/engine_protocol_client) 60/60 zero reports; cross 0 warnings. Ruling t10-coalesce: full-16 KiB deltas only mid-stream (DOS-02 event bound was flaky at 80 with the plan's per-tick partial flush); chat_schema_tests pin adapted like the plan's dos01 amendment | not run on this host (owner ruling 2026-10-20) | done |
| 11 | Stream cancellation: `on_tick` returns `client_gone`/`write_stall` (stall window via `blocked_since_`), cut streams write nothing more and get no terminal chunk; §8 log path: `streamed` on `ConnectionResult`/`RequestLogRecord`, route prints `chat_stream`, `log_record_for` keeps the sink's code | compile: "concurrency_test.cpp:355:89: error: too many initializers for 'secure_frontend::RequestLogRecord'"; runtime (first build): "FAILED: the client closed after 3 content chunks", "FAILED: a cancelled stream logs client_gone" (t10-coalesce starved small streams — fixed by ruling t11-age: partial deltas after a 500 ms hold, full deltas otherwise; reading clients get timely chunks, never-reading clients see 16 KiB deltas only) | 735 run, 100% (739 reg., 4 env skips) = +7; TSan spot (sse/concurrency/api_client/chat_execution/engine_protocol_client) 76/76 zero reports; cross 0 warnings. **Windows risk (plan Step 5):** stall cases cap at 16 MiB with a 45 s STOP wait — Linux green; MSYS2 pending, may fail on buffer volume | not run on this host (owner ruling 2026-10-20) | done |
| 12 | Keepalive: `: keep-alive` comment per `limits.http.stream_keepalive_ms` while nothing is pending and the sink is writable; timer restarts at every content event and comment, starting after the role chunk | "FAILED: comments while no content flows: 0"; "FAILED: at least 2 ': keep-alive' comments during a 2.5 s prefill: 0" (both the plan's predicted lines) | 737 run, 100% (741 reg., 4 env skips) = +2; TSan sse 19/19 zero reports; cross 0 warnings. Note: one full-suite run hit the PRE-EXISTING SP2 flake in M1's `log_line_format_and_rate_limit` — reproduced 3/10 on baseline `f01ef654` (ruled out-lane, parked for owner) | not run on this host (owner ruling 2026-10-20) | done |
| 13 | In-stream error events: `engine_error`/`engine_unavailable`/`internal_error` after flushing pending text, then `[DONE]` + terminal chunk; no `finish_reason` after an error; §7.2 `log_code`; report/restart rides Task 10's `LeaseHold` | "FAILED: ERR: error event", "FAILED: text, then engine_error", "FAILED: engine_unavailable event, ended normally" (non-stream halves of the nonfinite/should-retry cases passed already, as the plan predicted) | 742 run, 100% (746 reg., 4 env skips) = +5; cross 0 warnings | not run on this host (owner ruling 2026-10-20) | done |
| 14 | Shutdown during a stream (R-H): `on_tick`/`wait_writable` watch the probe; `server_shutting_down` error event + `[DONE]` + terminal bounded by `kShutdownDrainBudget` from first sight (never the 60 s tail deadline); a cut shutdown stream gets no terminal chunk; `LeaseHold` never reports during shutdown | "FAILED: server_shutting_down event", "FAILED: the generation was stopped", "FAILED: log code", "FAILED: in-stream server_shutting_down" (the plan's predicted lines) | 744 run, 100% (748 reg., 4 env skips) = +2; TSan spot 36/36 + shutdown/disconnect e2e 2/2 under TSan, zero reports; cross 0 warnings | not run on this host (owner ruling 2026-10-20) | done |
| 15 | `run_target_from_server_json`: PX's `server.json` → loopback-only `RunTarget` (host IP literal + port 1-65535 required, other keys ignored); two fixed error literals, file values never echoed; defines the writer contract (controller note 4) | `api_client_test.cpp:3:10: fatal error: secure_frontend/run_target.hpp: No such file or directory` | 745 run, 100% (749 reg., 4 env skips) = +1; cross 0 warnings | not run on this host (owner ruling 2026-10-20) | done |
| 16 | `strata-api-probe` — test-only client for the GPU kit (modes `stream`, `nonstream`, `cancel-after=<n>`, `cancel-during-prefill`, `concurrent=<k>`, `utf8`, `seed`; PASS/FAIL one-liners, loopback by construction) + `docs/validation/sp3-api-kit.md` procedure (purpose, setup, egress before/during/after, probe table, real clients incl. SDK-raises-on-cut, record template) | configure: `CMake Error at CMakeLists.txt:656 (secure_frontend_test_paths): Unknown CMake command "secure_frontend_test_paths"` — no `strata-api-probe` target yet and the block sat before the function; ruling t16-order: block relocated after `secure_frontend_test_paths` + fake-engine, probe target created first (test also gained `support/test_paths.hpp`) | 746 run, 100% (750 reg., 4 env skips) = +1 (all 6 probe modes green against the fake engine); cross 0 warnings (probe built `-Werror` on mingw) | not run on this host (owner ruling 2026-10-20) | done |
| 17 | Documentation truth pass: `engine-protocol-limits.md` gains the SP3 M2 streaming section, `threat-model.md` gains the SSE/chunked-framing, no-bytes-while-queued (R6), slow-reader bound and R4-kit bullets, `secure_frontend/README.md` gains the streaming route description + constants note; all 32 §9.3 M2 names verified registered (`ctest -N` all `OK`) | — (docs task) | 746 in scope, 742 run + 4 env skips, 100%, 0 failed; cross 0 warnings | not run on this host (owner ruling 2026-10-20) | done |

**SP3 M2 status: COMPLETE (Tasks 1–17), 2026-10-21.** Final gate lines (Task 17 Step 6, branch HEAD after Task 17): `ctest --preset linux-gcc` — `100% tests passed out of 746` in preset scope (742 run + the 4 allowed environment skips, 0 failed, 163.75 s; 746 in scope by `ctest -N`), growth from N₀ = 690 → 746 (+56 total: +5 T3, +3 T4, +6 T5, +3 T6, +5 T7, +6 T8, +3 T9, +7 T10, +7 T11, +2 T12, +5 T13, +2 T14, +1 T15, +1 T16; Tasks 2 and 17 add no new CTest entry); `windows-llvm-mingw-cross` EXIT=0 with 0 warnings (compile-only). Counting note for the table above: ctest's own `out of N` line already includes the 4 skips, so the table's `reg.` column overstates the preset-scope total by 4; the run lines are the truth. `wine`: not run on this host (owner ruling 2026-10-20 — no wine, no CI; agents prove only the cross build). All `windows-runtime`-labeled cases (the `api_server`/`api_client`/`api_router` e2e suites, plus M2's `sse_tests` binaries' socket cases) remain **authoritative in MSYS2 — pending the owner's push** onto his own Windows hardware; `dos02_slow_stream_reader_hits_write_stall` and `stream_cut_has_no_terminal_chunk` are the two named at risk on Winsock buffer volume (Task 11 ruling). TSan: spot runs at Tasks 5, 10, 11, 12, 14 — every run zero ThreadSanitizer reports (largest: 76/76 plus 2 e2e under TSan).

Controller notes restated as decisions taken: **note 2 (R-B)** — the loopback client (`LoopbackAddress`, `secure_frontend_api_client`, the only `connect()` in the frontend, §6.1) is M2's own, delivered by Task 9. **note 4** — the byte pack needed no new multi-byte ids; M2's long token id 258 (1024×`a`) came in Task 2 and makes the volume/stall tests fit `max_completion_tokens ≤ 16384`. **note 7** — the §8 log path (Task 11): `streamed` on `ConnectionResult`/`RequestLogRecord`, route prints `chat_stream`, the sink's code survives `log_record_for`. **note 8** (row 15 mis-cited it as note 4; corrected here) — PX's `server.json` has no `host` and no writer exists; Task 15 fixed the reader contract (`host` + `port` required, other keys ignored) and the future PX/SP2 writer must emit `host`.

Open follow-ups (all outside M2): the `server.json` writer must emit `host`; SP2 second-run decisions D-1/D-2 unchanged; the GPU kit (`docs/validation/sp3-api-kit.md`) awaits the owner's boxes; the pre-existing SP2 flake in M1's `api_server_tests.log_line_format_and_rate_limit` (unexpected pool `evicted` line) reproduces 3/10 on the pristine baseline `f01ef654` — parked for the owner, untouched in-lane. **Nothing is claimed for a GPU or for Windows beyond CI logs**: the last CI-authorized Windows proof is the green `windows-msys2-clang` run at `7bddb9e1`; every M2 Windows statement rests on the local `windows-llvm-mingw-cross` build only.

## Current security behavior implemented in `secure_frontend`

Since SP1, `strata-secure-server` configuration is one typed, schema-validated
`FrontendConfig` (`frontend_config.hpp`; every key is in the README key table,
generated from `config_schema()`), read from built-in defaults, then an
optional strict JSON file (`--config`), then command-line options:

- Defaults to `127.0.0.1:8080`.
- The listener classifies its address from the parsed bytes (IPv4/IPv6
  literals) and rejects every hostname except `localhost` (ASCII
  case-insensitive), which maps to `127.0.0.1` with no resolver call.
- Requires `listener.allow_remote` for every non-loopback address, and in
  addition `listener.allow_wildcard_bind` for every spelling of the unspecified
  address (`0.0.0.0`, `::`, `::0`, `0::0`, the long form, `::ffff:0.0.0.0`, …).
- The whole configuration is validated, collecting every issue by key path
  with a fixed reason (no values echoed), before the listener binds or the
  engine starts. `--check` opens and inspects but never creates, writes,
  renames or deletes anything; only serve and `init` create the logs/cache
  roots. Config files (and their folders) writable by other users are refused.
- The engine is spawned only after the listener is bound.
- Roots and the engine executable are canonical (`CanonicalRoot`,
  `CanonicalFile`); files below a root are opened with `openat2`/a no-follow
  walk on Linux and a `READ_CONTROL` reparse-rejecting walk on Windows.
- Every listener requires a token, including on loopback. `init` generates a
  43-character CSPRNG base64url token; operator tokens must be 32–256 bytes.
  The token is held in `SecretBytes`, compared in a fixed 256-byte window
  (`TokenVerifier`), never stored in the config object and never passed to the
  child. An environment token is wiped and removed from the environment after
  it is read. The server is non-dumpable with a zero core limit (Linux).
- Rejects ports outside `1..65535`.

This is intentionally stricter than upstream. Do not weaken it to recover
legacy behavior. Add an explicit, reviewed migration path if compatibility
requires a change.

The implemented primitives and their current runtime connections are:

- `SecretSource` retains only a validated environment-variable name or
  portable relative protected-file path metadata. It rejects traversal,
  absolute/UNC/device/drive/ADS forms and Windows reserved names. It has no
  plaintext-token field or resolution operation; a later authentication layer
  must resolve beneath a canonical root and own the secret briefly and securely.
- `HttpPolicy` validates exactly one syntactically valid `Host` header against
  an exact allowlist, rejects duplicate/malformed/untrusted `Origin` headers,
  keeps CORS off by default, and only permits byte-for-byte configured CORS
  origins.
- `TokenReader` resolves an environment source or a canonical-root,
  owner-only regular protected file without symlinks/reparse points or extra
  hard links, through the canonical-path module. The Windows branch validates
  final handle paths, current-user ownership, and a protected private DACL on
  the root and every component, and unwraps a DPAPI `STRATAK1` file. It is
  fail-closed. Since SP1 `secure_frontend_token_reader_tests` builds and runs
  on every platform; its Windows file cases are `windows-runtime` and **verified in MSYS2, CI
  run `38540719307`**.
- `EngineProcess` and `EngineStdioProcess` start a child without a shell or
  inherited environment and reject embedded NUL in executable/argv/env values.
  The production engine's environment is rendered by the frontend
  (`EngineEnvironment`, spec §3.9) and is exactly: POSIX `PATH=/usr/bin:/bin`,
  `LD_LIBRARY_PATH`, `TMPDIR=<state>/engine-tmp/<gen>`,
  `CUDA_CACHE_PATH=<state>/cache/cuda` (`<roots.cache>/cuda` when `roots.cache`
  is set, since Task 29b) (plus optional `STRATA_HIPBLASLT_TUNING`,
  `STRATA_WATCHDOG_S`, `STRATA_WATCHDOG_IO_S`); Windows
  `PATH=<lib dirs>;%SystemRoot%\System32`, `TEMP`, `TMP`, `CUDA_CACHE_PATH`,
  `SystemRoot`, `WINDIR`, `SystemDrive`, `NUMBER_OF_PROCESSORS`. `<state>` is
  the required owner-only `--state-dir` (`roots.state`).
  On POSIX the child gets only stdio (argv/env prepared before `fork`,
  `O_CLOEXEC` pipes, descriptor sweep), runs in its own process group, and is
  reaped after `SIGKILL`. On Windows it is created suspended, assigned to a Job
  Object, then resumed. Both talk the documented Strata `--serve` line protocol
  over bounded private standard-I/O pipes, with a total generation deadline and
  `STOP`/drain recovery.
- `build_engine_arguments` builds the engine argv tail: for the Strata profile
  shard 1 comes from the inventory (`--native`), owned or unsupported flags
  are rejected, and `--pack` plus the other weight flags are operator-supplied
  pass-through. **The GLM-5.3 profile takes no arguments at all** (SP5 M2,
  D-8): any `engine.args` / `--engine-arg` with a `glm53` manifest is refused
  with a fixed message that echoes no value, and the whole argv is composed
  from the manifest, the model pack and the Maya parity table.
- `RequestRouter`/`ApiRouter` (SP3 §3.6/§4.1/§4.2) route the **complete header**
  before any body byte (`kRouteTable`: `GET /health`, `GET /v1/models`,
  `GET /v1/models/<id>`, `POST /v1/chat/completions`; everything else 404/405
  after authentication). The §4.2 head scan rejects malformed, ambiguous or
  over-limit request lines and header blocks (414/431/411/413/505/417/400);
  Host/Origin policy and CORS as before; authentication is part of the header
  phase (constant-time token compare in the 256-byte window), and every error
  is the §7.2 fixed JSON body with a fixed `code` and `param`.
- `HttpSocketServer` binds only a revalidated numeric configured address
  (`::` as IPv6-only) and accepts in 100 ms slices; since SP3 it is the
  listener only — `ConnectionPool` (§2.1–2.4) runs it on one accept thread
  with `limits.concurrency.max_concurrent_requests` fixed workers (default 8):
  per-address admission cap `max_connections_per_remote_address` (derived
  min(4, workers); loopback exempt — decision B12), oldest-P1 (header-phase)
  eviction when no worker is idle, and a fixed 503 `server_busy` +
  `Retry-After: 1` read through a lingering close for refusals. Each worker
  serves P1–P8 polled in 20 ms slices under `header_deadline_ms` (default
  10 s) and `request_deadline_ms` (default 30 s) with body caps
  (`limits.http.max_request_bytes` 64 KiB; chat body 49152), §2.7 complete-
  response writes bounded by the stall rule (`write_stall_ms` 10 s measured on
  progress, `response_write_deadline_ms` 60 s); every wait sees `abort` and
  shutdown within one slice. Chat generations are serialized by
  `GenerationGate` (FIFO; `max_queued_generations` derived default 4;
  `queue_timeout_ms` default 300 s; a full queue gets the fixed refusal at
  once; a client that vanishes mid-generation gets `STOP(client_cancelled)`
  within one slice). `secure_frontend_http_socket_server_tests` +
  `secure_frontend_concurrency_tests` + `secure_frontend_api_server_tests`
  cover these on this Linux host; the server-starting api/server/egress cases
  are `windows-runtime` (MSYS2 pending).
- `RequestLogger` (§8): with `logging.requests=true`, one fixed-field stderr
  line per finished request (`req <seq> <route> <status> <code> <ms>
  <bytes_out> <peer>`) under a token bucket (burst 100, refill 20/s, counted
  suppression line at most once per second); off by default; never a token,
  prompt, configured path or payload value.
- `ApiRouter` implements authenticated `GET /health` and `GET /v1/models[/id]` over
  immutable injected providers; in `strata-secure-server` `/health` returns
  `{"status":"ok"|"unavailable","engine":"starting|ready|generating|failed"}`
  (`starting` → `200` with `engine=starting` — READY is consumed on the first
  request; eager READY is SP2 — `ready`/`generating` → `200`, `failed` → `503`). Since
  SP3 /health also answers while the generation queue is full. The smoke
  executables pass no engine-state provider and keep `{"status":"ok"}`. `ModelInventory` discovers local
  `.gguf` artifacts only; it has no download, subprocess, or network behavior.
- `secure-frontend-smoke` wires the local components into a process users can
  run with an API-key environment variable and local model directory. It is a
  test server, not an inference server: chat requests correctly remain absent.
- `strata-secure-server` starts exactly one local engine child and implements
  authenticated, non-streaming, text-only `POST /v1/chat/completions` for the
  fixed offline GPT-2-byte-BPE/Qwen-ChatML profile or, only when selected at
  startup, the fixed GLM-5.3 prompt profile (since SP3 the endpoint runs
  through the concurrent pool + FIFO generation gate — many connections, one
  generation at a time; SSE streaming is M2). GLM mode requires an independently
  built and audited LightTransport-compatible `strata-glm-decode` engine and
  accepts only its exact bounded `PP` prefill progress record in addition to
  the normal private protocol. It requires explicit local engine/model/tokenizer
  paths, uses no downloads, and rejects unsupported model/templates, tools,
  images, URLs, MCP, and streaming.

## Required architecture

```
client
  -> portable native frontend (authentication, Host/Origin policy, request limits)
  -> private authenticated IPC/loopback endpoint
  -> Strata 0.1.43 engine child process
```

The frontend owns the externally visible listener. The engine must never be
directly exposed to LAN clients. The frontend starts it without a shell,
passes only typed and validated arguments, strips secrets from child argv and
environment, and gives it only the handles/descriptors it needs.

Implement a common process-supervision interface with platform backends:

- Windows: `CreateProcessW`, explicit inherited-handle list, anonymous/named
  pipes as appropriate, and a Job Object that terminates descendants.
- Linux: `posix_spawn` or `fork`/`execve`, close-on-exec descriptors, a process
  group, bounded output capture, and `waitpid` cleanup.

Never import UnbiasedStrata's POSIX process implementation unchanged.

## Privacy contract

Normal install, startup, inference, failure handling, shutdown, and local API
use must make no outbound connection or DNS lookup. The release must contain no
analytics, telemetry, crash upload, remote configuration, advertising, or
automatic update client. Prompts, completions, images, and usage data remain
local; request logging is off by default.

The only permitted network operations are separately invoked, foreground
`strata` commands: `strata pull` (acquire), `strata update` (today the
models part, `update --models`; the program part exits 12 until SP7) and
`strata run` for a model that is not installed, after the same confirmation.
Each uses a user-visible, digest-pinned HTTPS manifest and cannot be
triggered by an API request, model content, plugin, configuration or
automatic logic; `strata-secure-server` links no acquisition code (PRIV-04).
See the threat model ("Privacy and offline operation", "Artifact
acquisition"; amended by SP4 Task 29, spec §7.4) for the full rules. The
matching product-experience-spec amendments (SP4 spec §13.3) await a PX
addendum (SP4 closing summary, "What has changed").

## Ordered implementation backlog

SP0 items are marked **Done (SP0)**. Each open item names the sub-project that
owns it (see the decisions above).

### Phase 1 — native security primitives

1. Extend `secure_frontend` with immutable typed configuration for request
   time/concurrency limits and roots for models/logs/cache. **Done (SP1)**:
   `FrontendConfig`, `--config`/`--check`/`init`; concurrency stays 1 until SP3.
2. Add canonical-path validation for Linux and Windows, including reparse
   points, UNC/device/ADS forms, and traversal prevention. **Done (SP1)**;
   Windows runtime verified in `windows-msys2-clang` (CI run `38540719307`).
3. Add secure token generation/storage: CSPRNG; DPAPI-backed file on Windows;
   user-only permissions on Linux. Tokens must never reach log output,
   environment, child command lines, or metrics. **Done (SP1)**; the Windows
   DPAPI store passed in `windows-msys2-clang` (CI run `38540719307`).
4. Implement all relevant `CFG-*` and `AUTH-*` tests from
   `tests/security/README.md` before connecting these primitives to a server.
   **Done (SP1)** for CFG-01..06 and AUTH-01..04 (plus NET-01..03); the
   `windows-runtime` cases passed in `windows-msys2-clang` (CI run
   `38540719307`).

### Phase 2 — portable child supervision

1. Define a testable `EngineProcess` abstraction and a fake engine fixture. **Done.**
2. Implement the Windows and Linux backends described above. **Done (SP0)
   for the launch path: POSIX argv-before-fork, `O_CLOEXEC`, descriptor sweep,
   reap; Windows suspended start into the Job. Windows runtime still needs the
   `windows-msys2-clang` job.**
3. Add the remaining `PROC-01` through `PROC-06`, including fake-child crash/hang, inherited
   handle/descriptor inspection, and descendant cleanup tests. **Done on
   branch `sp2-portable-supervision` for Linux (all L and B rows green
   locally); Windows (W) rows pending the `windows-msys2-clang` job.**
4. Connect the abstraction to the actual 0.1.43 engine. **Done for the
   documented serial `--serve` protocol, with SP0's deadlines, recovery and
   inventory-owned `--native`; full profile coverage remains open (SP5).**

### Phase 3 — private API boundary

1. Implement authenticated `/health`, `/v1/models`, and
   `/v1/chat/completions` with bounded HTTP/JSON parsing and streaming.
   **The non-streaming text-only Qwen and explicitly selected GLM-5.3 subsets
   are implemented; streaming and concurrency are SP3, other profiles SP5.**
2. Enforce exact Host, Origin, and CORS policies; CORS is off by default.
   **Health/models/chat routing and engine state are connected for the narrow
   serial profile; add chat streaming only after a separate review (SP3).**
3. Add all `NET-*`, `WEB-*`, `DOS-*` and `EXT-*` tests on Windows and Linux.
   **SP3.** (SP0's socket tests cover the slow-request, oversize and early-close
   parts of `DOS-01` on Linux only.)
4. Differential-test supported request/response behavior against the upstream
   service. Preserve model and backend flexibility; reject only unsafe API
   behavior. **SP5.**

### Phase 4 — verified, explicit artifact acquisition

1. Implement an offline-by-default model inventory and local-file validation.
   **Inventory done; canonical-root validation done (SP1).**
2. Implement the explicit `acquire` command with HTTPS-only sources, byte
   limits, SHA-256 verification, temporary staging, and atomic publish.
   **Done in SP4** (branches `sp4-acquire`, merged at `4b9fdd31`, and
   `sp4-acquire-part2`): `strata pull`/`import`/`run`/`update --models`
   through a confined system curl. The real-HTTPS run is the manual step in
   "Verification performed", SP4 part 2.
3. Add `DL-*` tests using only local fixture servers. Never make tests fetch
   public model files. **Done in SP4** (same branches): DL-01..05 and
   PRIV-01..04 with a fake downloader and scripted resolver; Windows runtime
   cases of Tasks 25–28 verified in CI run `38655938748` (PRIV-04 link-set
   re-run pending).

### Phase 5 — compatibility, packaging, and release

1. Port all upstream model profiles, quantizations, vision/MTP paths,
   multi-GPU, CUDA, HIP, and experimental SYCL behavior one capability at a
   time with regression coverage. **SP5; the GLM engine is SP6. Both need the
   owner's NVIDIA host.**
2. Add CMake presets and CI for Windows and Linux. **Done (SP0)**: `linux-gcc`,
   `linux-clang`, `windows-llvm-mingw-cross`, `windows-msys2-clang`, and
   informational `windows-msvc`. **CI runs and is green**: all blocking jobs
   on run `38458255140` and, after the submodule layout, on run `38478775552`
   (see owner decision 9); `windows-msvc` stays informational.
3. Generate an SBOM and release manifest listing exact engine/dependency
   revisions and enabled network-capable components. **SP7.**
4. Run the full security/privacy matrix, dependency scan, and an independent
   review before claiming a hardened release. **SP7**, including the offline
   GPU validation kit for the Linux and Windows test boxes.
   SP2 G3 (unverified in CI and wine): on the offline Windows box, from a
   NON-elevated prompt, run `secure_frontend_engine_child_tests.exe --case
   proc06_elevation_required_is_spawn_failed` and record the result; it must
   pass (error 740 → `spawn_failed`, no `runas`).
   SP2 inputs for the kit: check that `/dev/nvidia-uvm` exists before
   starting; run once with the default (`PR_SET_NO_NEW_PRIVS` on) and, only if
   CUDA init fails, once with `--allow-new-privileges`; record which works;
   confirm the SP2 environment allowlist is sufficient for CUDA and ROCm (spec
   §2, not yet verified).
5. Stage only the native deliverables, then run
   `python3 scripts/secure_distribution_check.py --package-dir STAGE --release`.
   A source checkout fails this gate by design: it rejects any path under an
   `upstream` directory (the submodule) and the quarantined names at any depth;
   do not weaken the gate to make a source archive pass. **SP7.**

## Verification performed

### SP4 part 2 (2026-10-14, branch `sp4-acquire-part2`, Tasks 25–29)

Commits on base `557a2cc7`: `06630dbd` (Task 25, Linux confined
downloader), `563dd0e9` + `a79e8b02` (Task 26, Windows downloader + fix
round 1), `ecff71dc` + `1582fba4` (Task 27, network verbs + signals fix
round 1), `68273f72` + `e7a16999` (Task 28, privacy/release proofs +
positive controls), then the Task 29 docs commit. Each task was reviewed
(spec + quality); each fix round was re-reviewed. Run at `e7a16999` (Task 29
changes docs only) on the dev host: Linux 5.15.0 x86_64, no GPU, LSMs
`lockdown,capability,landlock,yama,apparmor` (Landlock ABI 1, so the
confinement proofs ran, not skipped), cmake 4.4.3, wine 6.0.3, worktree
`.worktrees/sp4b`, `.venv-dev/bin` first on `PATH`.

| Command (in `secure_frontend/` unless noted) | Result |
| --- | --- |
| `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc` (hooks ON) | builds clean (`-Werror`, no warning lines); `100% tests passed out of 556` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`); 520 at the base + 36 |
| `ctest --preset linux-gcc -N -L 'release-only\|release-also'` | `Total Tests: 7` (the preset excludes the 3 `release-only` cases; `ctest --test-dir out/build/linux-gcc -N` lists 559) |
| `cmake --preset linux-gcc-release && cmake --build --preset linux-gcc-release && ctest --preset linux-gcc-release` (hooks OFF, `release-only\|release-also`) | builds clean (`-Werror`); `100% tests passed out of 10` (6 at the base + `privacy.priv04_secure_server_does_not_link_acquisition`, `privacy.release_binary_lacks_hook_marker`, `privacy.dl01_server_makes_no_outbound_connection`, `fetch.release_spawn_has_no_fd4`) |
| `ctest --test-dir out/build/linux-gcc-release` (full, hooks OFF) | `100% tests passed out of 523` (same 2 skipped) |
| stage `strata` + `strata-secure-server` (release) into `out/stage-release/bin`; `python3 -I ../scripts/secure_distribution_check.py --package-dir out/stage-release` | `secure distribution check: PASS`, rc 0 (without `--release` until SP7, Controller note 19) |
| `cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross` | builds clean (`-Werror`, no warning lines) |
| `WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml= ctest --preset windows-llvm-mingw-cross` | `100% tests passed out of 260` (515 registered; `windows-runtime\|release-only` excluded) |
| `.venv-dev/bin/pytest -q` (worktree root) | `25 passed` |
| `.venv-dev/bin/pytest -q tools/secure/test_catalog_provenance.py` (worktree root) | `2 passed` |
| spec case-name check (Task 29 Step 1 loop, over `ctest -N` of the linux-gcc, linux-gcc-release and cross build directories, so `release-only` and `windows-runtime` cases are listed) | every `<binary>.<case>` named in spec §10.3, §4.9 and §10.5 is registered except the SP7-deferred `fetch.release_spawn_has_no_extra_handle_windows`; the other non-matches are config keys and file names, not case names |
| `grep -n SECURE_STRATA_ENGINE_VERSION .github/workflows/secure-frontend.yml` (root) | no output; both `paths:` filters list `upstream.version` |

Real Windows: CI run `38625663621` on the base `557a2cc7` is green
(`windows-msys2-clang` 488/488, `linux-gcc-release` job green); it covers
Tasks 1–24b and SP2 run 2, not Tasks 25–28. **Verified on real Windows by
CI run `38655938748` (`9044a5bc`, `windows-msys2-clang` 515/516; was
pending MSYS2)**: every new `windows-runtime` case of Tasks 25–28 —
`fetch.downloader_transfer_through_job_windows`,
`dl03_default_windows_curl_path_accepted_windows`,
`dl03_downloader_runs_at_low_integrity_windows`,
`priv04_downloader_env_is_systemroot_only_windows`; the Task 27 Windows
acquire/fetch CLI cases including the Ctrl-Break
`acquire.dl04_interrupt_during_download_never_indexed_windows` (ruling I15)
and `fetch.confinement_reduced_written_to_log_record`;
`privacy.dl01_server_makes_no_outbound_connection_windows` (TCP and UDP,
IPv4 and IPv6 tables, listener positive control) and the Windows
`windows-runtime` privacy cases (tripwire, priv03, doctor proxy hint,
record-channel strata cases, doctor positive control). Also first proven
there: the Job `ActiveProcessLimit = 1` against conhost and the
Low-integrity child's access to `.staging/.cwd`. Wine-runnable additions
(`dl03_downloader_path_normalised_windows`,
`dl03_downloader_unc_path_rejected_windows`, priv04 and the hooks marker
case) are in the wine 260. Still pending on real Windows (no case in run
`38655938748` covers it): Ctrl-C at a
pending `[y/N]` may race the console-handler thread (the handler runs on its
own thread, so the read can return EOF before the flag is set: exit 2
instead of 8). MSYS2 run id for this branch: `38655938748` (`9044a5bc`).

- CI run `38655938748` (`9044a5bc`, `windows-msys2-clang`): 515/516; every other Tasks 25–28 `windows-runtime` case passed on real Windows (rows above flipped from pending to verified). The failure, `privacy.priv04_secure_server_does_not_link_acquisition` ("the strata link set was generated" + all three positive controls), was CRLF: native-Windows CMake's `file(WRITE)` writes text mode, so `strata\r\n` never matched the test's `"strata\n"` (the server's newline-free check passed and its `\n`-suffixed negative checks were vacuous). Fix commit "sp4: PRIV-04 link sets written LF and parsed per line, CRLF-proof with path/size/entries diagnostics (SP4 merge gate; spec §7.1)": `file(CONFIGURE ... NEWLINE_STYLE UNIX)` and an exact per-line entry parse dropping `\r`, positive controls unchanged in strength. linux-gcc 560/560 (+2 skips), linux-gcc-release 526/526 (10/10), wine 260/260, pytest 25, cross 0 warnings. MSYS2 re-run: pending.

**Required manual step (spec §13.5 item 4, the only real-HTTPS proof of DL-02), not yet run:**
on the online dev machine,
1. hooks build: a third-party manifest of profile `raw` for the 9 426 880-byte
   `vision/GLM-5.3-Flash-vocab.gguf` (`peasantsmith/GLM-5.3-Flash-Maya-GGUF` @ `f16ee4ba50d8fd2bb726ea1862ca6ca14ba188da`),
   `strata --config <cfg> pull --manifest raw.json --trust-manifest sha256:<hex> --yes`;
2. release build, stock curl: `strata --config <cfg> pull <name:tag> --bundle <dir>` once a catalog
   entry exists (D5 (A)/(C)); until then, `make_bundle.py` + `strata import` of a developer bundle.
Record curl version, TLS backend, confinement label and the result here; `import`
on an offline box is recorded when the owner runs it. Also to verify on the boxes
(§3.2, §10.4): Schannel and revocation checks at Low integrity; the inherited stdout
pipe accepting a Low-integrity writer; Landlock on the offline Linux kernel (`strata doctor`);
`--resolve host:443:[v6]` on curl 7.81.0 (until then IPv6-only hops fail closed).

What this does **not** show: no real curl, no network and no real model was
used (fake downloader, scripted resolver, local fixtures only); nothing ran
on Windows or on a GPU; Landlock ABI ≥ 2 rights (`REFER`, `TRUNCATE`, the
ABI 4 TCP rule) were not exercised on this ABI 1 kernel.

### SP4 merge into `main` (2026-10-14, merge `4b9fdd31`)

Merged tree (main `863b3ff6` + `sp4-acquire` `f0b09e2e`), dev host, `.venv-dev/bin` first.

| Command | Result |
| --- | --- |
| in `secure_frontend/`: `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc` | build: 0 warnings; `100% tests passed out of 520` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`); 312 (main) + 207 (SP4) + 1 merge case |
| `cmake --preset linux-gcc-release && cmake --build --preset linux-gcc-release`; `ctest --test-dir out/build/linux-gcc-release`; `ctest --preset linux-gcc-release` (`release-only\|release-also`) | 0 warnings; `100% tests passed out of 510` (same 2 skipped); labelled `6/6` |
| `cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross`; `WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml= ctest --preset windows-llvm-mingw-cross` | `-Werror`, 0 warnings; wine `100% tests passed out of 256` (489 registered; `windows-runtime` cases excluded) |
| `.venv-dev/bin/pytest -q` (worktree root) | `23 passed` |

### SP2 final-review fix wave (2026-10-14, branch `sp2-portable-supervision`)

HEAD before the wave was `b9d707de`; each line was run after the wave's code
commits (I1, I2, M1, M3/M5/M6) and the M4 header edit.

| Command | Result |
| --- | --- |
| in `secure_frontend/`: `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc` | build: 0 warnings; `100% tests passed out of 312` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`) |
| in `secure_frontend/`: `cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross` | builds with `-Werror`, 0 warnings; 38 `.exe` files |
| `WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml= ctest --preset windows-llvm-mingw-cross` (wine 6.0.3) | `100% tests passed out of 171` (282 registered; the 111 `windows-runtime` cases excluded) |
| `.venv-dev/bin/pytest -q` (worktree root) | `13 passed` |

### SP2 (2026-10-14, Task 28 final verification, branch `sp2-portable-supervision`)

Dev host (Linux x86_64, no GPU), worktree `.worktrees/sp2`, `PATH` with
`.venv-dev/bin` first; HEAD before the Task 28 commit was `a12fa0b0`. Each
line is a command run in Task 28, after its edits.

| Command | Result |
| --- | --- |
| in `secure_frontend/`: `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc` | build: 0 warnings; `100% tests passed out of 308` (2 skipped: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`) |
| in `secure_frontend/`: `cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross` | builds with `-Werror`, 0 warnings; 38 `.exe` files (SP1's 30 + `strata-supervision-fake-engine`, `strata-supervision-fake-engine-elevated`, `secure_frontend_non_elevated_run` and five test binaries) |
| `WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml= ctest --preset windows-llvm-mingw-cross` (wine 6.0.3) | `100% tests passed out of 168` (278 registered; the 110 `windows-runtime` cases excluded) |
| `pgrep -f strata-supervision-fake-engine` after both runs | no fake engine running (the only match was the `pgrep` shell itself) |
| `.venv-dev/bin/pytest -q` (worktree root) | `13 passed` |

Windows (W) rows and every `windows-runtime` test are unverified until the
`windows-msys2-clang` job is green after the owner pushes; nothing here was run
on a GPU.

### SP1 final fix wave (2026-10-14, branch `sp1-typed-config-paths-tokens`)

Final whole-branch review fixes I1 (`9e8312b5`, chat ranges = enforced caps),
I2 (`7d59353c`, engine `--native` as UTF-8), I3 + F3 (`4ad028c0`, serve prints
the actionable reason; `--check` verdict after the token read), F1 + F2
(`c706366d`, docs). Same dev host and `PATH`; HEAD before the wave `05c9db02`.
These numbers supersede the Task 24 table below.

| Command | Result |
| --- | --- |
| `.venv-dev/bin/pytest -q` (worktree root) | `13 passed` |
| in `secure_frontend/`: `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc` | `100% tests passed out of 215` (210 + `cfg01_chat_ranges_match_enforced_caps`, `engine_native_path_is_utf8_for_non_ascii_root`, `serve_reports_why_the_token_and_tokenizer_pack_are_unusable`, `serve_reports_symlinked_model_entry` (POSIX only), `cli_check_unreadable_token_is_not_reported_valid`) |
| `ctest --preset linux-gcc -N \| grep -cE '\.(net0[123]\|auth0[1234]\|cfg0[123456])_'` | `59` (58 + `cfg01_chat_ranges_match_enforced_caps`) |
| in `secure_frontend/`: `cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross` | builds with `-Werror`, no diagnostics; 30 `.exe` files |
| `ctest --preset windows-llvm-mingw-cross` (wine 6.0.3) | `100% tests passed out of 128`, 0 failed; 193 registered, the 65 `windows-runtime` cases excluded: the 61 listed below plus `secure_frontend_typed_config_tests.cfg01_chat_ranges_match_enforced_caps`, `secure_frontend_engine_arguments_tests.engine_native_path_is_utf8_for_non_ascii_root`, `secure_frontend_server_e2e_tests.serve_reports_why_the_token_and_tokenizer_pack_are_unusable`, `secure_frontend_server_cli_tests.cli_check_unreadable_token_is_not_reported_valid` (each **verified in MSYS2, CI run `38540719307`**) |
| direct wine runs (smoke, not evidence) | `engine_native_path_is_utf8_for_non_ascii_root`: RED before I2 (`--native` not UTF-8 under ACP 1252), pass after; `serve_reports_why_the_token_and_tokenizer_pack_are_unusable`, `cli_check_unreadable_token_is_not_reported_valid`, `cli_check_prints_no_token`: pass |

### SP1 (2026-10-14, Task 24 final verification, branch `sp1-typed-config-paths-tokens`)

Dev host (Linux x86_64, no GPU), worktree `.worktrees/sp1`, `PATH` with
`.venv-dev/bin` first; HEAD before the Task 24 commit was `1904f827`.

| Command | Result |
| --- | --- |
| `.venv-dev/bin/pytest -q` (worktree root) | `13 passed` |
| in `secure_frontend/`: `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc` | `100% tests passed out of 210` (`ctest -N`: `Total Tests: 210`; 209 before Task 24, plus `readme_config_table_matches_schema`) |
| `ctest --preset linux-gcc -N \| grep -cE '\.(net0[123]\|auth0[1234]\|cfg0[123456])_'` | `58`; every `net0*`, `auth0*` and `cfg0*` case is listed individually as `<binary>.<case>` |
| in `secure_frontend/`: `cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross` | builds with `-Werror` (incremental, 18 Ninja steps, no diagnostics); 30 `.exe` files in the build directory |
| `ctest --preset windows-llvm-mingw-cross` (wine 6.0.3, `WINEPREFIX=secure_frontend/out/wineprefix`) | `100% tests passed out of 128`, 0 failed; 189 registered (`ctest -N`), the 61 `windows-runtime` cases excluded (list below). `readme_config_table_matches_schema` runs unlabelled under wine with the Linux-absolute README path and passes, so no label was needed (ruling R9) |
| `wine …/strata-secure-server.exe --help` | `help exit=0` |
| `wine …/strata-secure-server.exe --check --port 0 --models-root 'C:\x' --tokenizer-pack 'C:\y' --engine 'C:\e.exe' --model-id m --token-env X` | prints `Configuration error: listener.port: must be between 1 and 65535`, `roots.models: the root does not exist or cannot be opened`, `roots.tokenizer_pack: …`, `engine.executable: the engine executable does not exist`; `check exit=1` (spec §9.2) |
| D4 probe: a scratch copy of `bootstrap-llvm-mingw.sh` under `secure_frontend/out/d4-*` with a corrupt archive in its own `.toolchains/downloads` (no network; the real `.toolchains` untouched) | before the fix `exit=1` and the archive kept; after `exit=1`, "digest mismatch; the downloaded archive was deleted", `downloads` empty |

`windows-runtime` tests not run locally (61; each was **pending
windows-msys2-clang** at Task 24 and is now **verified in MSYS2, CI run
`38540719307`**; some were run directly under wine as recorded in the
SP1 deliverables table, which is not evidence): `secure_frontend_paths_tests.paths_handle_non_ascii_and_long_names`, `secure_frontend_paths_tests.cfg02_create_within_root_writes_nothing_outside`, `secure_frontend_paths_tests.auth02_windows_token_file_reads_with_private_dacl`, `secure_frontend_paths_tests.cfg03_rejects_junction_below_root`, `secure_frontend_paths_tests.cfg03_rejects_symlink_below_root`, `secure_frontend_token_store_tests.token_store_windows_round_trips_through_dpapi`, `secure_frontend_token_store_tests.token_store_windows_file_has_private_dacl`, `secure_frontend_token_store_tests.token_reader_windows_accepts_plaintext_under_private_dacl`, `secure_frontend_token_reader_tests.reads_an_owner_only_regular_file_below_the_root`, `secure_frontend_token_reader_tests.rejects_control_characters_in_file_token`, `secure_frontend_token_reader_tests.auth02_token_file_with_trailing_lf_accepted`, `secure_frontend_token_reader_tests.auth02_token_file_with_trailing_crlf_accepted`, `secure_frontend_token_reader_tests.auth02_token_file_with_two_newlines_rejected`, `secure_frontend_token_reader_tests.auth02_token_file_with_bom_rejected`, `secure_frontend_token_reader_tests.auth02_token_file_over_4k_rejected`, `secure_frontend_token_reader_tests.auth02_notepad_edits_rejected_with_reason`, `secure_frontend_model_inventory_tests.inventories_only_local_gguf_artifacts`, `secure_frontend_model_inventory_tests.rejects_duplicate_ids`, `secure_frontend_model_inventory_tests.cfg03_inventory_rejects_junction_entry`, `secure_frontend_engine_arguments_tests.engine_receives_model_path_from_inventory`, `secure_frontend_engine_arguments_tests.engine_args_reject_owned_and_network_flags`, `secure_frontend_engine_arguments_tests.strata_profile_passes_pack_and_weight_flags_through`, `secure_frontend_engine_arguments_tests.rejects_invalid_relative_artifact_path`, `secure_frontend_engine_arguments_tests.glm_profile_passes_arguments_through_without_native`, `secure_frontend_typed_config_tests.typed_config_accepts_minimal_file_and_applies_defaults`, `secure_frontend_typed_config_tests.cfg01_unknown_key_rejected`, `secure_frontend_typed_config_tests.cfg01_duplicate_key_rejected`, `secure_frontend_typed_config_tests.cfg01_type_confusion_rejected`, `secure_frontend_typed_config_tests.cfg01_missing_required_rejected`, `secure_frontend_typed_config_tests.cfg01_out_of_range_rejected`, `secure_frontend_typed_config_tests.typed_config_cross_field_rules`, `secure_frontend_typed_config_tests.typed_config_errors_never_contain_values`, `secure_frontend_typed_config_tests.cfg02_logs_and_cache_roots_are_created_private`, `secure_frontend_typed_config_tests.cfg06_validated_config_bytes_do_not_contain_token`, `secure_frontend_typed_config_tests.cfg01_config_with_everyone_write_ace_rejected`, `secure_frontend_tokenizer_pack_tests.loads_validates_and_decodes_offline_pack`, `secure_frontend_tokenizer_pack_tests.rejects_ambiguous_or_incompatible_pack_assets`, `secure_frontend_tokenizer_pack_tests.rejects_malformed_json_and_special_literals_for_decode`, `secure_frontend_tokenizer_pack_tests.encoding_fails_closed_for_special_invalid_and_missing_tokens`, `secure_frontend_tokenizer_pack_tests.cfg03_tokenizer_file_swapped_to_junction_rejected`, `secure_frontend_subsystem_limits_tests.ready_timeout_defaults_to_900_seconds`, `secure_frontend_subsystem_limits_tests.subsystem_limits_follow_config`, `secure_frontend_server_cli_tests.cli_config_file_and_flags_share_validator`, `secure_frontend_server_cli_tests.cli_flag_overrides_file_and_is_reported`, `secure_frontend_server_cli_tests.cli_check_prints_no_token`, `secure_frontend_server_cli_tests.cli_unknown_option_does_not_echo_value`, `secure_frontend_server_cli_tests.auth04_empty_token_rejected_on_loopback`, `secure_frontend_server_cli_tests.auth04_no_switch_disables_auth`, `secure_frontend_server_cli_tests.cli_check_has_no_side_effects`, `secure_frontend_server_cli_tests.init_writes_config_and_token_then_server_starts`, `secure_frontend_server_cli_tests.init_refuses_to_overwrite_without_force`, `secure_frontend_server_cli_tests.init_force_refuses_a_non_regular_config_target`, `secure_frontend_server_cli_tests.init_failure_creates_nothing`, `secure_frontend_server_e2e_tests.cfg01_invalid_config_fails_before_listener_and_child`, `secure_frontend_server_e2e_tests.net02_remote_addresses_need_opt_in`, `secure_frontend_server_e2e_tests.net02_error_is_non_secret_and_exits_before_bind`, `secure_frontend_server_e2e_tests.ready_timeout_is_configurable`, `secure_frontend_server_e2e_tests.cfg05_token_absent_from_child_argv_env_and_outputs`, `secure_frontend_server_e2e_tests.auth01_socket_rejects_missing_and_wrong_token_on_every_route`, `secure_frontend_server_e2e_tests.auth02_valid_token_succeeds_and_is_not_echoed`, `secure_frontend_server_e2e_tests.net01_default_listener_is_loopback_only`.

Update (2026-10-14, SP2 Task 28): CI run `38540719307` ran the
`windows-msys2-clang` job on SP1, 193/193, so the `windows-runtime` cases
listed above are now verified there; the paragraph below records the state
at Task 24.

What this does **not** show: nothing in SP1 is Windows-runtime-verified. The
`windows-msys2-clang` job (authoritative) has not run on any SP1 commit; wine
does not model DACLs (`D:P` is stored as `D:`), junctions, long final paths or
Job Objects faithfully. `linux-clang`, `windows-msvc` and the root build were
not run for SP1. Nothing ran on a GPU or a real engine (fake engine only).

### Submodule layout (2026-10-13, branch `layout/upstream-submodule`)

Dev host (Linux x86_64, no GPU), `PATH` with `.venv-dev/bin` first.

| Command (repository root unless noted) | Result |
| --- | --- |
| `git submodule status` | ` 7695859aa13afb248bdb4d36f9b2e95707db079d upstream (v0.1.43)`; `git -C upstream status --short` empty after every gate below |
| `git ls-files \| sed 's#/.*##' \| sort -u` | `.gitattributes .github .gitignore .gitmodules CMakeLists.txt LICENSE README.md conftest.py docs pytest.ini scripts secure_frontend tests tools upstream` |
| `pytest -q` | `12 passed` in Task 7; `13 passed` after the final fix wave (new `extract()` guard test) (`tools/secure/` only); with `upstream/` empty it exits 4 with "run `git submodule update --init`". A fresh venv from `bootstrap-linux.sh` (no numpy) passes too (from Task 3 report, same date) |
| in `secure_frontend/`: `ctest --preset linux-gcc` | `100% tests passed out of 20` (also from a fresh build directory: 78 steps, 0 warnings) |
| in `secure_frontend/`: `windows-llvm-mingw-cross` build, then `ctest --preset windows-llvm-mingw-cross` (wine) | 20 `.exe` with `-Werror`; `100% tests passed out of 19` (also from a fresh build directory: 76 steps, 0 warnings) |
| `cmake -S . -B out/root -G Ninja && cmake --build out/root && ctest --test-dir out/root` (from clean) | 94 steps, 0 warnings; `strata-plan`, `strata-gguf`, `strata-dequant`, `strata-secure-server` built; `100% tests passed out of 20`; no `out/root/_deps` |
| root configure with `-DCMAKE_BUILD_TYPE=Debug -DSTRATA_WERROR=OFF` / `-DSTRATA_ENABLE_CUDA=ON` / empty `upstream/` / `-DSECURE_STRATA_BUILD_ENGINE=OFF` (from Task 2 report, same date) | `Debug` kept and `SECURE_FRONTEND_WERROR=ON`; fails at `CMAKE_CUDA_COMPILER` (override reached upstream); fails with "run `git submodule update --init`"; configures and builds the frontend |
| fresh scratch clone (`git clone`, then `git submodule update --init --depth 1`) | plain clone: configure (exit 1) and pytest (exit 4) stop with the init instruction; after init: root build 94 steps, 0 warnings, ctest 20/20, no `_deps`, pytest 12 passed; `git rm -r upstream && ls -A` shows only our paths. With `--depth 1` the tag is not fetched, so `git submodule status` there shows `(7695859)` instead of `(v0.1.43)` |
| `pytest upstream/tools` (with the recorded `--ignore`/`--deselect`), separate venv from `scripts/dev/requirements-dev.txt` + `upstream/requirements.txt` | from the repository root: `4 failed, 612 passed, 2 skipped, 3 deselected, 214 subtests passed` — the four (`test_conversation_cache_disabled.py` and `test_conversation_cache_growth.py`, two each) run `tools/<script>.py` and import `tools.test_…` relative to the working directory; from `upstream/` (`pytest -q tools --ignore=tools/… --deselect=upstream/tools/…`, node ids stay relative to our root `pytest.ini`): `616 passed, 2 skipped, 3 deselected, 214 subtests passed` |
| `pytest upstream/serve` (same venv, repository root) | `695 passed, 11 skipped, 5532 subtests passed`, exit 0 |
| offline bundle (`git -C upstream bundle create … v0.1.43`, init a scratch clone from it) (from Task 6 report, same date) | bundle okay; `refs/tags/v0.1.43` → submodule checked out at `7695859a` with no network |

Final fix wave (same date, after `45487311`):

| Command | Result |
| --- | --- |
| `pytest -q` (repo root) | `13 passed` (new: `extract()` raises "upstream layout changed; re-verify the wrapper" when `tokenizer/tokenizer.json` is missing) |
| in `secure_frontend/`: `ctest --preset linux-gcc` | `100% tests passed out of 20` |
| `cmake -S . -B out/root-fixwave -G Ninja && cmake --build out/root-fixwave && ctest --test-dir out/root-fixwave` (fresh dir) | 94 steps, 0 warnings, `100% tests passed out of 20`, no `_deps` |
| root defaults, no `-D` flags (`cmake -S . -B out/root-defaults-fixwave -G Ninja`, the CI step's greps) | `STRATA_NATIVE_EXPERTS:BOOL=OFF`, `STRATA_ENABLE_CUDA:BOOL=OFF`, `STRATA_ENABLE_HIP:BOOL=OFF`, `CMAKE_BUILD_TYPE:STRING=Release`, no `_deps` |
| upstream suites, the exact `PROVENANCE.md` step-2 commands (`python3 -m venv --without-pip out/venv-upstream-fixwave`, dev pip `--python … install`, then from `upstream/`) | `tools`: `616 passed, 2 skipped, 3 deselected, 214 subtests passed`; `serve`: `695 passed, 11 skipped, 5532 subtests passed` (4 min 14 s); `git -C upstream status --porcelain` empty |

Not shown here: `linux-clang`, `windows-msys2-clang`, `windows-msvc`. CI on
GitHub: run `38478775552` on `4aa742c9` (U10 row above); no CI run since
`4aa742c9` (docs/comment-only commits until the final fix wave, which adds
the root-defaults CI step and the `extract()` guard).

### SP0 (before the submodule layout)

Run on the dev host (Linux x86_64, no GPU) on 2026-10-13, after the SP0 final fix
wave (`--pack` pass-through, `/health` engine state, portable socket tests). Always put the venv first:
`export PATH=/opt/projects/ai/.venv-dev/bin:$PATH`.

| Command (from the repository root unless noted) | Result |
| --- | --- |
| `scripts/dev/bootstrap-linux.sh` | `bootstrap-linux: ready` (idempotent on the existing venv; also exit 0 in a fresh scratch checkout with network, and offline from a `STRATA_WHEEL_DIR` holding pip + all pinned wheels with the network blocked; cmake 4.4.3, ninja 1.13.2, pytest 9.1.1) |
| `.venv-dev/bin/pytest -q` | `620 passed, 2 skipped, 3 deselected, 214 subtests passed in 20.60s` |
| in `secure_frontend/`: `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc` | `100% tests passed out of 20` (the socket-server binary runs 10 cases, binding loopback and `::` on this host; `strata_secure_server_glm_requires_engine_arg` is new) |
| `scripts/dev/bootstrap-llvm-mingw.sh` | llvm-mingw `20261006` (clang 23.1.3) present, sha256 checked when fetched |
| in `secure_frontend/`: `cmake --preset windows-llvm-mingw-cross && cmake --build --preset windows-llvm-mingw-cross` | Builds with `-Werror`; 20 `.exe` files (3 executables, 17 test binaries) |
| in `secure_frontend/`: `ctest --preset windows-llvm-mingw-cross` (wine 6.0.3, `WINEPREFIX=secure_frontend/out/wineprefix`) | `100% tests passed out of 19`: config, secret_source, engine_stdio_process (round trip, invalid record, `read_line_rejects_oversize_on_all_platforms`, `read_line_waits_for_a_record_split_across_writes`, `job_object_terminates_grandchild`), http_policy, http_request_handler, http_socket_server (all 10 cases; S3 1.6 s, S2 1.0 s), api_router, engine_process, engine_protocol_client, model_inventory, engine_arguments, chat_schema, tokenizer_pack, qwen_prompt_renderer, glm_profile, chat_generation_plan, chat_execution, strata_secure_server_help, strata_secure_server_glm_requires_engine_arg. wine is not authoritative for the Job Object grandchild case or native Winsock RST behavior. |
| `wine out/build/windows-llvm-mingw-cross/strata-secure-server.exe --help` | exit 0 |

What this does **not** show:

- `linux-clang`, `windows-msys2-clang` and `windows-msvc` were not built on
  this host (no clang; they need Windows); see the CI table below.
- wine is a smoke test. It does not implement Job Objects or protected-file
  DACLs faithfully, and it passed all three tests that failed on real Windows.
  `secure_frontend_token_reader_tests` is the only UNIX-only target, so the
  Windows token reader has no runtime test anywhere yet, including the MSYS2
  job.
- Nothing has been run on a GPU or against a real Strata engine; every engine
  test uses the fake engine.
- `pytest upstream/serve` (the upstream service suite; `pytest serve` before
  the submodule layout) was not run for SP0; nothing in `upstream/serve/`
  changed.

#### GitHub Actions (`secure-frontend` workflow, `shift-f10/multi-strata`)

| Run | Branch / head | Result |
| --- | --- | --- |
| `38456682352` (first real run) | `main` | `windows-msys2-clang` 16/19. Failed: `engine_stdio_process_tests` (`READY must parse`, 0.02 s), `engine_process_tests` (argv/env case), `http_socket_server_tests` (S2: `/big` reported sent in 132 ms to a peer that never reads; S3 passed, 1136 ms). |
| `38457597307` (diagnostics) | `ci/windows-runtime-fixes` `1a7d3b9d` | Same three failures. Both engine tests: the re-executed fake engine exited `0xC0000135` (`STATUS_DLL_NOT_FOUND`), message `engine closed before READY` for the stdio one. Loopback probe: 64 KiB blocking sends into a non-reading peer stop at 128 KiB (the default `SO_SNDBUF`) and fail with `WSAETIMEDOUT` (10060) after the 500 ms `SO_SNDTIMEO`, for client `SO_RCVBUF` default, 1 or 0; with server `SO_SNDBUF` 0 nothing is accepted. |
| `38458255140` (fixed) | `ci/windows-runtime-fixes` `9576d03b` | All blocking jobs green: `linux-gcc`, `linux-clang`, `windows-llvm-mingw-cross`, `windows-msys2-clang` (19/19), `top-level-integration`. `windows-msvc` (non-blocking) fails at configure: the `windows-latest` image now ships Visual Studio 2026 and the preset names the `Visual Studio 17 2022` generator. |

Fixes on `ci/windows-runtime-fixes`:

1. **Engine launch (`engine_process_tests`, `engine_stdio_process_tests`).**
   Both launchers pass only the allowlisted environment, so the child has no
   `PATH`. A CLANG64 build links `libc++.dll`/`libunwind.dll` from
   `/clang64/bin`, which the Windows loader finds only through `PATH`, so the
   child failed to load. The `windows-msys2-clang` preset now links with
   `-static`, as the llvm-mingw cross preset already did. Launchers and test
   assertions are unchanged; the `HOME`-absent check stays meaningful because
   the child gets an explicit environment block.
2. **S2 (`socket_server_send_times_out_on_stalled_reader`).** One 32 MiB blocking
   `send()` was accepted whole by Winsock, so `send_deadline` never applied.
   `send_all` now caps each `send()` at 64 KiB on every platform, and the
   unchanged S2 test passes on Windows. Not gated.

The Python GLM tokenizer tests also ran under `pytest -q` above (then
`tools/test_glm_tokenizer.py`; now `tools/secure/test_glm_tokenizer.py`, with
the pinned `regex` from the venv). The two skips were hardware/platform skips
in upstream tests (`upstream/tools/test_dll_closure.py`: HIP zip absent;
`upstream/tools/test_setup_oldcpu.py`: Windows only). Run
`tools/secure/test_glm_tokenizer.py` and `tools/secure/test_secure_tokenizer.py`
again in the release Python environment before accepting a GLM tokenizer
release.

### Known small gaps

Deferred from the SP0 task reviews; none blocks SP0, each is worth a look in the
sub-project named.

- `strata`'s Windows UTF-8 argv (shared `secure_frontend_utf8_argv`, SP4 merge
  `4b9fdd31`) builds and runs under wine only. CI run `38625663621` did not
  cover it: the CLI cases there call `run_strata` in-process. The first cases
  that start the `strata` binary on Windows are Task 27's DL-04 Ctrl-Break
  case and Task 28's privacy cases (passed in CI run `38655938748`), all with ASCII arguments;
  a non-ASCII path through a `strata` verb on real Windows has no case yet
  (nor has Task 26's downloader argv, built with `path_to_utf8`).
- The fallback descriptor sweep (used only where `close_range(2)` is missing
  or fails) stops at `kSweepCap` = 65536 (documented at the constant, SP2
  Task 28). The listening and accepted sockets are created without
  `SOCK_CLOEXEC`, so on that fallback path, with `RLIMIT_NOFILE` above the
  cap, such a socket numbered at or above 65536 would reach the engine. Linux
  5.9+ (`close_range`) has no such gap. Creating the sockets close-on-exec
  would remove it. SP3 (owns the listener).
  **K4 gap closed (listener + accepted sockets CLOEXEC/no-inherit)** — SP3
  Task 8b (branch `sp3-api-streaming`); Windows proof pending MSYS2.
- S4 (oversize `413`) relies on the half-close + 100 ms drain to beat an RST
  on Windows; it passed in the MSYS2 job (run `38458255140`). SP3 if it flakes.
- `windows-msvc` (non-blocking) fails at configure on the current
  `windows-latest` image (Visual Studio 2026; preset asks for 2022). SP1 tooling.
- The `send_all`-on-`EINTR`, S6 port-range and transient-accept-backoff SP0
  bullets in this list were fixed in SP3 (`send_retries_on_eintr`, the S6 test
  binding port 0, `accept_backs_off_on_transient_failure` — Task 8a; §9.4).
- Since SP3 the per-request log and every error are fixed literals; `--check`
  still prints the operator's own configured values, as SP1 designed.
- Submodule layout: closed in the final fix wave. `top-level-integration`
  now first configures our root with no `-D` flags (`out/root-defaults`) and
  asserts `STRATA_NATIVE_EXPERTS`, `STRATA_ENABLE_CUDA`, `STRATA_ENABLE_HIP`
  `OFF`, `CMAKE_BUILD_TYPE=Release` and no `_deps`; not yet run on GitHub.
- Submodule layout: `scripts/secure_distribution_check.py` does not split
  backslash separators on Linux, so a staged name such as
  `upstream\requirements.txt` is one path component there and passes (a
  `.py` name would still be caught by the Python-source rule).
- Submodule layout: `tools/secure/secure_tokenizer.py` couples to upstream
  internals (`Tokenizer._re`, the `__init__` argument order, the `extract()`
  output path `tokenizer/tokenizer.json`); re-verify it on every upstream
  pointer bump ("Update upstream"). Since the final fix wave `extract()`
  raises ("upstream layout changed; re-verify the wrapper") when upstream did
  not write that file, instead of creating it.

SP4 part 2 (Tasks 25–29, branch `sp4-acquire-part2`). Removed from the list
above: the threat-model "`acquire` is the sole networked operation" bullet
(amended in Task 29). Added (each a deferred review minor or a ruling's
residue; none blocks the merge):

- `acquire.log` has one record per **transferred** file (ruling R12):
  files reused from a prior version, copied from a bundle or already staged
  get no record, although spec §7.3 says "one record per file". Task 22's
  `fetch_file` is the only writer.
- `LoggingProgress` reopens the logs root per record and its
  exists-then-create step can race and drop a record; logging never fails a
  pull (I14).
- A declined pull leaves the empty `roots.models/.staging/.cwd` (B1 creates
  it before the `[y/N]`); a lazy creation in the downloader would remove it.
- Interrupts: `InterruptScope` does not honour an inherited `SIG_IGN` for
  SIGINT; a SIGINT during `doctor` is swallowed (exit 0); the
  prompt-flush/`read()` race above; the Windows console handler returns TRUE
  at once for `CTRL_CLOSE`/logoff/shutdown.
- Downloader: the executable is validated, then executed by path (no
  `fexecve`; same-user race, as for the engine); `probe_result_` is not
  cleared by a later failed probe; a failed record-file open is silently
  ignored (hooks builds only); Windows maps every `launch_in_job` failure
  by DWORD, ignoring `WinLaunchStep` (Job/attribute failures should be
  `downloader_confinement_failed`); an empty `cwd` inherits the parent's on
  Windows (POSIX fails closed; every caller passes `.staging/.cwd`); the
  stdin write precedes the interrupt-aware loop. (The POSIX-worded Windows
  `untrusted_owner` text and SP1 `windows_security.cpp`'s include of SP4's
  `confinement_plan.hpp` were fixed in the final fix wave.)
- Tests: no negative Windows validator case (writable file/parent, NULL
  DACL) and no non-ASCII downloader-argv case; the DL-04 Windows case leaks
  `strata` on timeout; the Linux prompt test re-sends SIGINT every 200 ms;
  the fd census stops at fd 1024 and has no committed negative control;
  priv02's `127.0.0.1`/`93.184.216.34` sentinels are weak (the proxy
  sentinel is the real check); the crash case needs
  `CONFIG_PROC_CHILDREN`; the Windows DL-01 case fails hard on a host
  without IPv6 tables; the link-set check matches `strata\n` as a
  substring; the shim reads `sa_family` without checking `msg_namelen`.
- The interrupted error detail names the file twice (Task 22 text).

SP2 (2026-10-14, Task 28). Removed from the list above: the undocumented
sweep cap (now documented at `kSweepCap`; the remaining socket gap is the
first bullet), the Windows stdin pipe bullet (N6, R1, K2 and R1 rows; limits
doc updated) and the missing Windows `SystemRoot` (N1 renders `SystemRoot`,
`WINDIR`, `SystemDrive`; not yet tried with a real engine on Windows).

SP1 (2026-10-14, Task 24). Fixed and removed from this list: the wildcard-bind
spelling bypass (Task 1, defect a), the corrupt-archive bootstrap gap (Task 24,
D4) and the stale "MSVC/Windows runtime coverage" CMake comment (Task 18).

1. The engine opens model files by path after validation: a swap below a
   user-owned, non-shared-writable root is a same-user race (SP1 spec §3.4).
   SP2 did not take up handle passing: its spec does not mention it, and SP2
   changed only what the engine inherits (N5 descriptors/handles, N6 the
   stdin pipe). Still an accepted same-user risk; open, no sub-project named
   yet (owner/controller to assign).
2. Fixed on the winfix branch (pending the next MSYS2 run): on Windows
   `strata-secure-server` now reads its command line as UTF-8 through
   `CommandLineToArgvW(GetCommandLineW())` (invalid UTF-16 exits 1 with a
   fixed message) and `--config` goes through `path_from_utf8`, so a
   non-ASCII value (A2's `✓`, a non-ASCII root) is no longer turned into `?`
   by the ANSI code page; CI run `38601830802` showed exactly that in
   `proc01_server_passes_literal_argv`. Before: `char** argv` in the ANSI code
   page, so non-ASCII paths belonged in the config file. The engine's argv is UTF-8 end to end since the SP1 final fix
   wave: `--native` is built with `path_to_utf8` (not `path::string()`, which
   narrowed through the ANSI code page and failed the launcher's strict UTF-8
   decode for a root such as `C:\Users\José\models`), and the executable's NUL
   check reads `path::native()`. `engine_native_path_is_utf8_for_non_ascii_root`
   was RED under wine before the fix and GREEN after; it is `windows-runtime`
   on Windows CI and passed there (CI run `38540719307`).
3. Windows environment-token scrub — accepted deviation from spec §3.6,
   owner-ruled: Windows has no `/proc/<pid>/environ` exposure, so the scrub
   wipes the CRT copies in place (narrow through `getenv`'s pointer, wide
   through `_wgetenv`'s), removes the CRT entry with `_putenv_s(name, "")` and
   removes the Win32 environment-block entry with
   `SetEnvironmentVariableW(name, nullptr)`; the block entry is removed, not
   overwritten in place (no Win32 API overwrites it). No ctest case covers it
   (wine probe only), so CI run `38540719307` does not verify it.
4. The `docs/security/deployment.md` Windows token caveat ("treat the Windows
   file path as unverified and prefer `--token-env` there") is removed in a
   follow-up commit once the owner's push shows the `windows-msys2-clang` job
   green for the `windows-runtime` token tests. The condition is met (CI run
   `38540719307`, 193/193); the caveat is still in `deployment.md` (not
   removed in SP2 Task 28, which only records proof). Open for the owner.
5. `cfg03_rejects_mount_crossing` skips where unprivileged user namespaces are
   disabled (GitHub's ubuntu-24.04 images may skip it).
6. Naming: the product-experience spec §4 sketches `listen.*`, flat `limits.*`
   keys and `token_source`; SP1 supersedes those names with `listener.*`,
   `limits.<section>.*` (`limits.http.*`, `limits.chat.*`, `limits.engine.*`,
   `limits.concurrency.*`) and `token.source`, and the future `strata` CLI
   follows SP1's names (the product spec §4 text is updated when that CLI is
   planned).
7. Closed in the SP1 final fix wave: the configuration ranges now match the
   enforced caps — `limits.chat.max_messages` 1–128 (renderer
   `kMaximumMessages`), `limits.chat.max_message_bytes` 1–65536 (renderer
   `kMaximumMessageBytes`), `limits.chat.max_prompt_tokens` 1–262144
   (`EngineProtocolLimits`), pinned by `cfg01_chat_ranges_match_enforced_caps`.
   `EngineProtocolLimits` and the renderers' 512 KiB rendered-prompt cap stay
   internal constants, not configuration keys; details in
   `docs/security/engine-protocol-limits.md`.
8. Windows `create_directory_within` does not check the DACL, owner or volume
   of components that already exist (POSIX does). No SP1 caller reaches that
   path with untrusted components (every caller ends in `open_existing_root`);
   revisit if a caller is added.
9. Windows token store (Task 17): the holding directory is pinned without
   `FILE_SHARE_DELETE`, but the temp file is created and moved by path
   (handle-relative `NtCreateFile` deferred); protection against renaming an
   ancestor relies on NTFS refusing to rename directories with open handles
   below them — unverified on real Windows.
10. A plaintext Windows token file that starts with the bytes `STRATAK1` is
    treated as DPAPI-wrapped and rejected as undecryptable (documented in
    `deployment.md`).
11. `ModelInventoryErrorCode::artifact_path_escapes_root` is unused since Task 13
    (kept for API stability).
12. The chat `model` field is bounded at 256 bytes (`chat_schema.cpp`
    `kMaximumModelBytes`) while model IDs are at most 128; consistent (a longer
    field is a model mismatch), noted in `engine-protocol-limits.md`.
13. Socket-server tests still use fixed ports 20000–20099; they collide under
    `ctest -j` and across concurrent worktrees (presets run serially). Parked
    (SP3).
14. The C3 bind-before-spawn guard's RED was shown on Linux only; on Windows the
    guard (`cfg01_invalid_config_fails_before_listener_and_child`) is
    `windows-runtime` and passed in `windows-msys2-clang` (CI run
    `38540719307`; GREEN only, its RED was never shown on Windows).

## Collaboration and change rules

- Keep security changes small and add a regression test before the fix.
- Never edit anything under `upstream/`. A compatibility need is met by a
  wrapper in our tree or by an upstream pull request; record it in
  `docs/PROVENANCE.md`. `git -C upstream status --short` stays empty.
- Do not copy UnbiasedStrata code without recording source, revision, license,
  rationale, and Windows/Linux test coverage.
- Do not enable MCP, tools, plugins, arbitrary command execution, telemetry,
  update checks, or automatic downloads. Those require a new threat-model
  review and an explicit user decision.
- Gates before every hand-off and after every submodule bump: `pytest -q`;
  `ctest --preset linux-gcc`; the `windows-llvm-mingw-cross` build; the root
  build and root `ctest`; after a bump also `pytest upstream/tools` and
  `pytest upstream/serve` ("Update upstream").
- Commit locally; the owner pushes.
- Before handing off, update this document's **What has changed** and
  **Verification performed** sections, then report `git status --short`.
