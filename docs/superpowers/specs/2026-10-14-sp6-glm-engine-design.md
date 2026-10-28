# Sub-project 6 — GLM-5.3 engine in-tree (evaluation, tracking, contract)

Date: 2026-10-14. **Status: DRAFT — awaiting owner review.** Nothing in this
document is approved. The binding inputs are the owner decisions A1–A11 of
2026-10-14 ("approve all", 07:43 UTC); §9 lists only what those decisions
leave open, each with a recommendation, and the body is written as if every
recommendation were accepted. Revision: review round 1 fixes (controller
rulings R-1..R-11 of `.superpowers/reports/sp6-spec-review-1.md` applied).
Parent effort: Secure Strata full release path (SP0–SP7), see
[`docs/AGENT_HANDOFF.md`](../../AGENT_HANDOFF.md).

Inputs, cited by short name:

- **A1–A11** — owner decisions of 2026-10-14, section A (SP6).
- **R** — [SP6 research](../research/2026-10-14-sp6-glm-engine-sources.md)
  (fact-checked 2026-10-14). `R §3.3` etc.
- **MAYA** — `mw00/project-maya` tag `v1.1.8`, commit
  `5c970a863ce2fdd87b6e23ffb4822f6d76522b1c`, read-only. Paths are relative to
  its root (`generate.cpp` = `src/program/generate.cpp`).
- **G53F** — `lighttransport/Strata` branch `glm53f`, commit
  `4894c9967f5754a2052d7b05f10a01b6071bb6e4`, read-only
  (`glm_decode.cpp` = `src/program/glm_decode.cpp`).
- **PX** — [product-experience spec](2026-10-13-product-experience-and-upstream-tracking-design.md)
  (APPROVED): §5 manifests, §11 SP6 row, the GLM-engine tracking bullet.
- **SP1** — [typed config spec](2026-10-13-sp1-typed-config-paths-tokens-design.md)
  (APPROVED): config schema, `limits.engine.*`, `secure_frontend_add_cases`.
- **SP2** — [supervision spec](2026-10-13-sp2-portable-supervision-design.md)
  (APPROVED): §3.5 kill triggers, §3.6 exit classification, §3.9 environment
  allowlist, typed `--gpu`.
- **SP3** — [API concurrency/streaming spec](2026-10-14-sp3-api-concurrency-streaming-design.md)
  (decisions B1–B12 approved): B6 sampling keys on `GEN`, §9.5 offline kit.
- **SP4** — [acquire spec](2026-10-14-sp4-acquire-design.md) (decisions C1–C7
  approved): §2.2 manifest schema (`profile`, `engine.family`), §2.4 `launch`
  keys, §2.7/D5 derived packs, §4.4 layout checks; D4 moved GLM entries here.
- **PROV** — [`docs/PROVENANCE.md`](../../PROVENANCE.md): the pinned-submodule
  runbook, "GLM-5.3 compatibility reference", "Project Maya".
- **TM / AT** — [`docs/security/threat-model.md`](../../security/threat-model.md),
  [`tests/security/README.md`](../../../tests/security/README.md).
- **EPL** — [`docs/security/engine-protocol-limits.md`](../../security/engine-protocol-limits.md).

Upstream facts at our pin (`upstream/` = `v0.1.44.1`, `296b59d2`) that shape
this spec (handoff "Upstream bump to v0.1.44.1", EPL):

- Upstream's `strata` refuses every GGUF whose `general.architecture` is not
  `qwen4exp` or `qwen35moe`. A GLM-5.3 GGUF (`glm5-next` / `glm5next`) can
  therefore never run on the upstream engine; GLM needs its own binary.
- Upstream `strata` loads `strata_kq` (`libstrata_kq.so` / `strata_kq.dll`)
  at run time from its own folder. SP6 checks the GLM engine for the same
  pattern (§6.4).
- Upstream gained `--families`; it is irrelevant to the GLM engine, which is
  built from a different source tree.

## 1. Goals, non-goals, definition of done

### 1.1 Goals

1. **Evaluate both GLM sources on the owner's NVIDIA box and pick the base
   (A1).** The first deliverable is a reproducible, offline-capable evaluation
   kit (§2) and its decision rule (§2.6). The lean is MAYA.
2. **Track the chosen engine as a pinned, unmodified git submodule under
   `engines/glm/` (A9)**, built by an optional root CMake component that is
   OFF by default and never built in CI (§3).
3. **Speak the chosen engine's wire protocol fail-closed** with a new
   protocol flavor, a typed argument profile and a typed environment
   allowlist that forces conversation slots off, the loop guard off and
   DFlash off (A7, A10, A11) (§4).
4. **Ship GLM-5.3 models through SP4's catalog**: Maya's S/M/L quants as
   `glm53` manifests, with pack layout checks and a bundle builder (A5) (§5).
5. **Close the engine-specific security questions** (network code, file
   writes, environment surface, runtime library loads, crash/watchdog,
   Windows DLL search) with controls and tests (§6).
6. Linux **and Windows** (A2), MSVC v143 as the nvcc host on Windows (A3),
   NVIDIA only (A4).

### 1.2 Non-goals

| Non-goal | Source |
| --- | --- |
| AMD/HIP, Intel, Vulkan GLM builds | A4 |
| EXL3 quants (`turboderp/GLM-5.3-Flash-exl3`) | A5 ("later") |
| Maya's `serve/`, `maya.py`, dashboard, updater, MCP client, `tools/maya_quant/` | A6 |
| DFlash2 in any form: weights, `--dflash`, `STRATA_GLM_DFLASH`, documentation as an option | A7 |
| Multi-GPU layer split (`--layer-split`, `STRATA_GLM_SPLIT`, `--split-device`) | A8 ("later") |
| glm53f remote tensor parallelism (`--remote-tp`, `STRATA_GLM_REMOTE_TP`, `strata-glm-tp-worker`) | A8 ("permanently out") |
| Local patches inside the engine submodule | A9 (a needed patch means an owner-approved GitHub fork) |
| Any `STRATA_GLM_*` tuning knob reaching the engine | A10 |
| Maya's loop guard on the chat route | A11 |
| Vision (`--vision`, `GENI`, mmproj files), `--parallel` > 1 (`rid` protocol), `PAUSE`/`WRAP`/`LOGP` | not needed by SP3's text chat route; each needs a protocol the frontend does not speak |
| Maya-M-Derisked (a third-party weight edit in a separate repository) | not one of Maya's S/M/L quants (A5) |
| A C++ pack builder, owner-hosted packs | SP4 D5, unresolved; SP6 ships the developer-built bundle path (§5.4) |
| GPU jobs in CI | no GPU runner exists; every GPU claim comes from the owner's box |

### 1.3 Definition of done

The GLM engine runs only on the owner's NVIDIA box, so "done" has four
parts, and nothing is called GPU-verified before part 4:

1. **Plan 1 merged (agents, dev host):** see §1.4; every item with the
   hermetic tests of §8 green on `linux-gcc`, `windows-llvm-mingw-cross`
   (wine) and, after the owner pushes, the MSYS2 `windows-msys2-clang` job.
2. **Evaluation (owner):** the battery of §2.4 run on Linux and Windows
   (MAYA in full, G53F load-only unless §9 M7 says otherwise); results files
   committed under `docs/validation/glm-eval/`; the decision rule of §2.6
   applied and the base recorded by the owner in the handoff.
3. **Plans 2 and 3 merged (agents, after part 2):** §1.4.
4. **Acceptance (owner):** the battery re-run against the integrated build
   (root option ON on Linux; `cmake/glm-engine` standalone on Windows) with a
   catalog model imported through SP4, plus SP3's API kit, on both OSes;
   results committed. Only then does the handoff say "GLM verified on <GPU>,
   <OS>".

### 1.4 Three plans

| Plan | When | Contents |
| --- | --- | --- |
| **1 — kit** | before the box run | **first tasks:** the tokenizer prerequisite fix (§5.5: `ignore_merges` for `glm4`/`glm5`, `glm5` accepted, in `tools/secure/secure_tokenizer.py` and the C++ tokenizer) and the `glm53` output post-processing (§4.10); then the evaluation kit and `strata-engine-probe` (§2), both protocol flavors (§4.2, §4.7), both argument profiles (§4.8), the `glm` environment family (§4.9), the legacy `--profile glm53` gating (§4.7), a pinned `numpy` in `scripts/dev/requirements-dev.txt` (today it pins only `regex`), and the SP1 config addenda `engine.glm.max_context` and `engine.glm.ram_headroom_gb` (M2) with the error `gpu_list_too_long_for_glm` |
| **2 — integration** | after the owner records the base | submodule `engines/glm/<name>` (§3.2), `cmake/glm-engine/` and the root option with its configure guards (§3.3), PROV baseline and bump runbook (§3.4), the audit tool and its committed snapshot (§3.4), notices (§3.5) |
| **3 — artifacts** | after plan 2 (needs `kGlmEngineVersion`) | manifest schema 2 and the `glm53` layout check (§5.2), catalog entries (§5.3), `make_bundle.py --profile glm53` (§5.4), SP4 inventory changes (§5.6), the SP1 addendum `engine.executables.glm` (§4.1; needed once `strata serve` launches a catalog model) |

## 2. Evaluation kit (A1)

### 2.1 Shape

The kit lives in our tree at `tools/glm-eval/` (scripts and fixed prompts)
plus one test-only C++ binary, `strata-engine-probe`, built with the frontend
in every preset and never installed. The probe launches an engine through the
same launch code the product uses (SP2's launcher, environment family §4.9,
argument profile §4.8, protocol client §4.2/§4.7), so the battery exercises
our real client, not a script's reimplementation of it. If SP2 has not merged
when the probe is built, the probe uses SP0's `EngineStdioProcess` with the
§4.9 environment rendered by the same function, and the results file records
`launcher: sp0`.

| File | Runs on | Purpose |
| --- | --- | --- |
| `tools/glm-eval/pins.txt` | — | the pinned commits, one per line: `maya 5c970a863ce2fdd87b6e23ffb4822f6d76522b1c v1.1.8`, `glm53f 4894c9967f5754a2052d7b05f10a01b6071bb6e4 -`, `llama.cpp 3cf03257f219afbe7334045ff7c6a06ac68c627d -` |
| `tools/glm-eval/fetch-sources.sh` | online Linux dev machine | clones the three repositories, checks out the pins, writes one `git bundle` per repository (`maya.bundle`, `glm53f.bundle`, `llama.cpp.bundle`), downloads binary-only wheels for the pack tooling (`pip download --only-binary=:all:` of numpy and `regex` at the versions pinned in `scripts/dev/requirements-dev.txt`; plan 1 adds the numpy pin) and writes `SHA256SUMS` into an output directory. It executes nothing it fetched |
| `tools/glm-eval/download-model.sh` | the box (Linux) or an owner-approved online machine (§9 M4) | downloads one Maya quant's shards from the pinned Hugging Face revision (§5.3) with `curl --fail --location --proto =https`, verifies each SHA-256 against `pins-models.txt` |
| `tools/glm-eval/build-pack.sh` | the box (Linux), or an owner-approved machine (M4); never the dev host (R-1) | builds the pack with MAYA's own `tools/iq_pack.py` exactly as §5.4 does (Python 3, the carried wheels, `gguf-py` from the llama.cpp bundle) and writes a model directory laid out like an SP4 version directory plus `SHA256SUMS` |
| `tools/glm-eval/pins-models.txt` | — | `<quant> <file> <size> <sha256>` for each shard of the quants in §5.3, copied from the LFS pointers at the pinned revision and cross-checked against MAYA `maya.py:104-200` |
| `tools/glm-eval/build-linux.sh` | box, Linux | builds one engine from its bundle, offline (§2.3) |
| `tools/glm-eval/build-windows.ps1` | box, Windows (VS 2022 x64 developer PowerShell) | the same for Windows |
| `tools/glm-eval/run-battery.sh` / `run-battery.ps1` | box | runs §2.4 for one engine and one model directory; writes the results file (§2.5) |
| `tools/glm-eval/prompts/p01.json` … `p08.json` | — | the fixed conversations (§2.4.1) |
| `strata-engine-probe` | box (built by the frontend presets) | one case per invocation; transcript and metrics out |

The scripts call only `git`, `cmake`, `ninja`, `curl` (download only),
`sha256sum` / `Get-FileHash`, `nvidia-smi`, `ss` / `netstat`, `nm` /
`objdump` / `dumpbin`, `python3` (pack building only) and the probe. The
battery itself needs no Python. The pack step needs Python 3 with numpy and
`regex` (MAYA `tools/strata_tokenizer.py:22` imports it) and llama.cpp's
`gguf-py`; it runs on the box's Linux side (or an owner-approved machine),
and its output directory is then used from both OSes.

### 2.2 Getting sources and models onto the box offline

1. On an online Linux dev machine: `tools/glm-eval/fetch-sources.sh <out>`
   (git objects and wheels only; nothing is executed).
2. Carry `<out>` to the box (disk or USB); verify `sha256sum -c SHA256SUMS`
   (Linux) or `run-battery.ps1 -VerifyOnly`; any mismatch stops the kit.
3. Shards: `download-model.sh <quant>` on the box while it is online, or on an
   owner-approved machine and carried over (default quant `maya-s`); then
   `build-pack.sh` on the box's Linux side.
4. The build scripts clone from the bundles into a fresh directory and check
   `git rev-parse HEAD` against `pins.txt`. A dirty tree
   (`git status --porcelain` not empty) or a wrong commit stops the build.

### 2.3 Build procedure

Common to both OSes and both engines:

- Configure from a fresh build directory `out/glm-eval-<engine>-<os>-<n>`,
  never reused.
- `STRATA_GGML_DIR=<llama.cpp clone at 3cf03257>`; FetchContent therefore
  never runs. After the build `<build>/_deps` must not exist.
- `STRATA_ENABLE_CUDA=ON`, `STRATA_ENABLE_HIP=OFF`, `STRATA_NATIVE_EXPERTS=ON`,
  `STRATA_BUILD_TESTS=OFF`, `STRATA_WERROR=OFF`, `CMAKE_BUILD_TYPE=Release`,
  `CMAKE_CUDA_ARCHITECTURES=native` (the box builds for its own GPU; the
  release list is SP7's).
- Linux: the whole build runs under `unshare --user --map-root-user --net`
  (no network namespace available → the results file records
  `network_isolated: false`). Windows: the owner disconnects the network and
  types `-Offline` to confirm; recorded as `network_isolated: owner-asserted`.
- Recorded: `nvcc --version`, host compiler version (`gcc --version` or
  `cl` banner and `MSVC_TOOLSET_VERSION`), `cmake --version`, driver version
  (`nvidia-smi`), the binary's SHA-256, and the configure log.

| | MAYA | G53F |
| --- | --- | --- |
| Target | `strata` (MAYA `CMakeLists.txt:384`); copied to `strata-glm-engine[.exe]` | `strata-glm-decode` (G53F `CMakeLists.txt:1604`) |
| Extra seeds | `STRATA_PORTABLE=ON` (AVX2 baseline, static MSVC runtime, MAYA `CMakeLists.txt:23`) | none |
| Extra checks | the configure log must not contain `does not apply to` or `git was not found` (MAYA `CMakeLists.txt:886-907`: the `mmq-fused-swiglu.patch` fallback silently builds a different prefill path); `<build>/strata-ggml-cuda/.strata-stamp` must exist | none |
| Windows | VS 2022 v143 developer prompt, Ninja; MAYA links `CUDA::cudart_static` on Windows (`CMakeLists.txt:87-91`) | attempted and recorded; expected to fail as shipped (R §1.3: `ucomm` POSIX headers, `setenv`, `__builtin_popcount`) |
| Arm | full | load-only (§2.4, §9 M7) |

### 2.4 Battery

Every case runs through `strata-engine-probe` with the product launch path:
owned argv (§4.8), environment family (§4.9), the engine's flavor (§4.2 or
§4.7). Each case writes the full engine stdout transcript (one line per
record, prefixed with milliseconds since spawn) to
`<results>/transcripts/<case>.txt`. Engine stderr goes to
`<results>/stderr/<case>.txt` and is never parsed for decisions.

Configuration for all cases unless stated: MAYA argv exactly as §4.8.1
(`--max-context 32768`, plus the frontend-owned constants `--prefill auto
--kv-streaming on --expert-profile <maya source>/data/expert-profile-glm.bin`);
G53F argv context `32768`; one GPU (`gpu: [0]`); `limits.engine.*` at SP1
defaults (`ready_timeout_s` 900, `generation_timeout_s` 120,
`stop_drain_timeout_s` 5).

**G53F arm: load-only** (§9 M7). The kit prepares Maya quants only; the G53F
reference model (Unsloth UD-Q2_K_XL, ≈ 100 GB, licence unchecked; G53F
`docs/README_GLM53_FLASH.md:9, 89`) is not fetched. G53F runs B-01, B-02,
B-03 and a B-04 attempt with the Maya-S shard set (R §2: both accept
`glm5-next`/`glm5next`), recording whether it reaches READY. Every other case
is `not_run` for G53F unless the owner adds the reference model (then B-05,
B-07, B-08, B-11…B-13, B-16, B-17, B-22 run with it).

#### 2.4.1 Fixed prompts

Eight conversations, committed as JSON in SP3's request shape, rendered by our
GLM prompt renderer and tokenized by our tokenizer (the probe prints the ids
it sends):

- `p01` "What is the capital of France? Answer with one word." — **known
  answer** `Paris`;
- `p02` CJK plus emoji;
- `p03` digit runs of length 1–12 (exercises the `glm4` three-digit rule);
- `p04` a 40-line C++ function to explain;
- `p05` system "You are a calculator. Reply with the number only." and three
  turns ending "What is 17 + 25?" — **known answer** `42`;
- `p06` a deterministic 4 000-token document (generated from a fixed word list
  and seed by the probe, so no text file is shipped);
- `p07` the same at 16 000 tokens;
- `p08` a request designed to produce a long repetitive answer ("count from
  1 to 400, one number per line").

#### 2.4.2 Cases

| ID | What | Pass criteria (gate column in §2.6) | Recorded |
| --- | --- | --- | --- |
| B-01 BUILD | §2.3 build, Linux and Windows | binary built from the pinned, clean tree; no `_deps`; MAYA patch stamp present | toolchain versions, build time, binary SHA-256 |
| B-02 STATIC-NET (our policy, §9 M6) | `nm -D --undefined-only` (Linux) / `dumpbin /imports` (Windows) on the engine binary itself | no import of `socket`, `connect`, `bind`, `listen`, `accept`, `accept4`, `getaddrinfo`, `sendto`, `recvfrom`; no `ws2_32.dll`, `wininet.dll`, `winhttp.dll`. On Linux this sees the binary's direct imports only, not those of `libcudart`/`libcublas`; the gate expresses TM's "engine has no network code" rule, which the engine's authors never promised | full import list |
| B-03 STATIC-LOAD | `strings`/import scan for `dlopen`, `LoadLibrary*`; `ldd` / `dumpbin /dependents` | every runtime-loaded or dependent library is listed in the results; any library outside the system, CUDA (`libcudart`, `libcublas`, `libcublasLt`, `cudart`/`cublas64_*`/`cublasLt64_*`) and the C/C++ runtime is a finding | load list |
| B-04 READY | spawn; wait for READY | READY within 900 s; `INFO` lines only before it; READY matches the flavor grammar; READY context equals the configured context; MAYA's `INFO` line (`generate.cpp:1018-1020`) shows `dflash=0` (A7 evidence) | time to READY, `INFO` fields (`mtp=`, `vram_gb=`, `ram_gb=`) |
| B-05 TRANSCRIPT | `p01`–`p05` greedy, `max_new` 1 024 | every line accepted by the flavor; zero `NOTE`; `DONE` finish `stop` or `length` | transcripts, `STAT` count; per prompt whether a `stop` output lacked `</think>` (observed / not observed, §4.10) |
| B-05a KNOWN-ANSWER | the B-05 outputs of `p01` and `p05`, post-processed exactly as §4.10 (split on the `</think>` id, trailing end-of-turn id dropped) and decoded by our tokenizer | decoding succeeds; the answer contains `Paris` (p01) and `42` (p05) | the answer texts (fixed prompts, no user data); any type-3/4 id removed by §4.10 rule 3 |
| B-06 SAMPLING | `p01` with `temperature=0.7 top_p=0.9 top_k=0 seed=42` twice, then `seed=43` (MAYA only; G53F is greedy-only) | lines accepted; same seed → identical ids | whether `seed=43` differs |
| B-07 DETERMINISM | `p05` greedy twice in one process, once after a restart | recorded only | identical yes/no, first divergence index |
| B-08 CTX-EDGE | engine restarted with context 4096; prompt `p06` truncated so `n + max_new + 8 = 4096` with `max_new` 32 | completes with `stop` or `length`; no `ERR` | — |
| B-09 CTX-REJECT | same engine; `n + max_new + 8 = 4097` | the client refuses before writing `GEN` (probe records zero outbound `GEN` bytes) | — |
| B-10 CTX-RAW | probe raw mode, which bypasses our rule and exists only in `SECURE_FRONTEND_TEST_HOOKS=ON` builds; `n + max_new = 4096 + 64` | recorded only | the engine's actual behavior (expected per R §3.3: `ERR glm prefill: … exceeds the context` for a long prompt; `ERR` after `T` lines or a quiet `length` for decode overrun) |
| B-11 STOP-DECODE | `p08`; `STOP` after the 32nd `T`; 20 trials | `DONE … cancel` within 2 000 ms on every trial; no `T` after `DONE` | p50/p95/max latency |
| B-12 STOP-PREFILL | `p07`; `STOP` after the first `PP` | `DONE … cancel` within 5 000 ms (`stop_drain_timeout_s`) | latency |
| B-13 QUIT | `QUIT` when idle | process exits 0 within 60 s | exit time (pinned-memory teardown) |
| B-14 KILL | external `SIGKILL` / `TerminateProcess` mid-generation, through `strata serve` | SP2 classifies `killed_external`, restarts, READY again | restart time. **`not_run` until SP1's `engine.executables.glm` and SP2's supervisor have merged** (like B-21) |
| B-15 STALL-BOUND | watchdog off (`STRATA_WATCHDOG_S=0`, §4.9); `generation_timeout_s` set to 5 for this case; `p07` with `max_new` 4 096 | the probe observes the frontend's timeout `STOP` and then either `DONE … cancel` within `stop_drain_timeout_s` or, failing that, the kill: with `launcher: sp2` SP2's `drain_failed` path; with `launcher: sp0` (no SP2 yet) the probe abandons the protocol (`abandon()`) and kills the process group itself, and records `branch: probe-kill`; no exit code 3 from the engine. The case passes only on an observed timeout `STOP`; if the generation finishes before 5 s, it is `not_run` and repeated with `p07` twice concatenated | which branch (cancel / kill) |
| B-16 PERF | `p01`, `p06`, `p07`, greedy, `max_new` 256, 3 runs each | recorded only | prefill tok/s = `n × 1000 / prompt_ms`, decode tok/s = `produced × 1000 / decode_ms` (both from `DONE`), medians |
| B-17 MEMORY | sampled at 1 Hz during B-16 | recorded only | peak VRAM (`nvidia-smi --query-gpu=memory.used`), peak engine RSS (`VmRSS`, `VmPin` / `PeakWorkingSetSize`), minimum host available memory (`MemAvailable` / `\Memory\Available MBytes`), with `STRATA_GLM_RAM_HEADROOM_GB` as rendered (§9 M2) |
| B-18 NET-RUNTIME | `ss -tunap` / `netstat -ano` filtered by engine pid, every 1 s through B-04…B-16 | zero sockets owned by the engine | snapshots |
| B-19 FILES | (path, size, mtime) snapshot of the model directory, the engine install directory and the engine cwd (`<state_dir>/engine`, SP2 §3.12) before B-04 and after B-16 | no file created, changed or deleted in the model or install directory, and no `slots/` anywhere; any new file in the cwd is a finding | the diff |
| B-20 MODPROBE | SP2's `nvidia-modprobe` check with `PR_SET_NO_NEW_PRIVS` on (Linux) | READY reached with NO_NEW_PRIVS (SP2 owner decision) | — |
| B-21 SERVE | `strata serve` with the model, then SP3's `strata-api-probe` cases `stream`, `nonstream`, `cancel-after=16`, `cancel-during-prefill`, `utf8`, `seed` (SP3 §9.5) | SP3's pass criteria; responses carry only the post-`</think>` answer, or the full text on stop without `</think>` (§4.10); records whether that case was observed | — |
| B-22 CROSS-CHECK | G53F and MAYA greedy on `p01`–`p05` from the same reference model | recorded only; `not_run` in the load-only arm | per prompt the first index where the two id streams differ |
| B-23 CORE (Linux) | `/proc/sys/kernel/core_pattern`; the engine's `RLIMIT_CORE` (`/proc/<pid>/limits`, inherited 0 from SP1 §3.6) | recorded; a piped `core_pattern` (for example `systemd-coredump`) is a finding for the owner, because a pipe handler may ignore `RLIMIT_CORE` 0 | values |
| B-24 TOKENIZER | our tokenizer's ids for `p01`–`p08` against MAYA's `tools/strata_tokenizer.py` on the pack's `tokenizer/`. MAYA's Python runs only on the Linux side (`build-pack.sh`, R-1), which writes the reference ids to `<model dir>/reference-ids.json`; the Windows probe compares its ids against that same file | identical id lists | first difference per prompt |

B-21 needs the integrated build (§1.3 part 4); in the evaluation (part 2) it
is `not_run`.

### 2.5 Results file

One JSON file per engine and OS: `docs/validation/glm-eval/<YYYY-MM-DD>-<engine>-<linux|windows>.json`,
plus the `transcripts/` and `stderr/` directories beside it. Transcripts hold
token ids of the fixed prompts and engine metrics only; no user data exists
on the box run.

```
{
  "format": "strata-glm-eval", "version": 1,
  "engine": {"name": "maya" | "glm53f", "commit": "<40 hex>", "tag": "v1.1.8" | null,
             "binary_sha256": "<64 hex>", "launcher": "sp2" | "sp0",
             "arm": "full" | "load-only"},
  "frontend_commit": "<40 hex>",
  "host": {"os": "...", "os_build": "...", "cpu": "...", "ram_gib": <int>,
           "gpu": "...", "gpu_vram_mib": <int>, "driver": "...", "cuda_toolkit": "...",
           "host_compiler": "...", "cmake": "...", "network_isolated": true | false | "owner-asserted"},
  "model": {"quant": "maya-s", "shard_sha256": ["<64 hex>", ...], "pack_sha256sums": "<64 hex>"},
  "cases": [{"id": "B-04", "status": "pass" | "fail" | "recorded" | "not_run",
             "metrics": {"<name>": <number or string>}, "note": "<≤ 512 chars, no prompt text>"}],
  "gates": {"G1": "pass" | "fail" | "not_run", ...}
}
```

The probe writes `cases[]`; `run-battery` fills `engine`, `host`, `model` and
computes `gates` from the cases with the table in §2.6. A pytest
(`tools/secure/test_glm_eval_results.py`) validates every committed results
file against this shape.

### 2.6 Decision rule

Gates (each per engine, per OS):

| Gate | Cases | Meaning |
| --- | --- | --- |
| G1 | B-01 | builds offline from the pinned, unmodified source |
| G2 | B-02, B-18 | our no-network-code policy (TM; §9 M6): no network import, no socket at run time |
| G3 | B-04, B-05, B-05a, B-06 (MAYA), B-08, B-09, B-24 | READY, generation and context behavior conform to the flavor; the known answers are right; our tokenizer matches the engine's |
| G4 | B-11, B-12, B-13 | STOP and QUIT within bounds |
| G5 | B-19 | no writes outside the allowed set |
| G6 | B-15 (evaluation); B-14 and B-15 (acceptance, §1.3 part 4) | a stalled generation is bounded by the frontend; crashes are classified and recovered |

An engine is **eligible** on an OS when G1–G6 pass there. The rule:

1. **MAYA eligible on Linux and Windows → MAYA is the base.** (The lean, A1.)
2. **MAYA not eligible on some OS** → the owner decides with §9 decision 4;
   the failing gates and their cases go into the handoff with the results.
3. **G53F as shipped cannot become eligible.** Its decode binary links
   `strata_ucomm` unconditionally (G53F `CMakeLists.txt:1605`), which imports
   `socket`/`bind`/`listen`/`accept4` (R §3.2), so it fails G2 on Linux, and
   it does not build for Windows (G1). Under A9 it can be eligible only from
   an owner-approved fork that removes `ucomm` from `strata-glm-decode` and
   adds the Windows guards. In the load-only arm the evaluation records G1,
   G2 and whether G53F loads a Maya quant, which is the data §9 decision 4 (c)
   needs; with the reference model (M7) it also gives a measured cross-check.
4. **Both eligible** (only possible after such a fork) → MAYA, unless the
   owner reads B-16/B-22 differently; the metrics are not a gate.

The owner records the result in `docs/AGENT_HANDOFF.md` ("SP6 base: <engine>,
results <files>"). Plans 2 and 3 start only after that line exists.

### 2.7 What the dev host can do, and what waits for the box

Execution of MAYA or G53F code on the dev host is limited by R-1: only a
**confined, CPU-only configure and build** of the engine, only inside a
reviewed plan task, run as `env -i PATH=/usr/bin:/bin unshare --user
--map-root-user --net` (no network, no `HOME`), with the source a read-only
copy of the verified bundle and the build directory the only writable path.
No pack tooling (`iq_pack.py`, `strata_tokenizer.py`, `gguf-py`), nothing
that needs model files and no engine binary runs there. The residual — we
run a one-week-old repository's CMake scripts and compilers' inputs — is §9
M4.

| Item | Dev host (no GPU, no nvcc) | Box |
| --- | --- | --- |
| Flavor parsers, argument profiles, environment family, manifest/layout checks | full hermetic tests (§8) with synthetic transcripts transcribed from the sources' `printf` formats | captured transcripts from B-05…B-12 replace or extend the synthetic ones |
| Tokenizer prerequisite fix (§5.5) | `secure_tokenizer.py` and C++ tests against fixtures generated by our fixed `secure_tokenizer.py` | B-24 confirms against MAYA's tokenizer on the real vocabulary |
| `tokenizer.ggml.pre` of the pinned shards | never | not the box either: runs on the owner-approved online machine (§9 M4) — a ranged read of the GGUF header (first 16 MiB of shard 1, `curl --range`) parsed by **our** `tools/secure/gguf_header.py` under `python3 -I`; data, not code |
| Pack building and its determinism (§5.4) | never (R-1) | `build-pack.sh` on the box's Linux side, or an owner-approved machine |
| Configure guards (no CUDA → clear failure) | `secure_frontend_glm_configure_tests.*` (our CMake only) | — |
| Engine audit (`tools/secure/glm_engine_audit.py`, §3.4) | full (reads source text, executes nothing) | — |
| CPU-only engine configure + build (MAYA `strata-gguf`, `strata-dequant`; `[to confirm]` whether MAYA configures with `STRATA_ENABLE_CUDA=OFF` and `STRATA_NATIVE_EXPERTS=OFF`) | confined as above, as a toolchain smoke test; the built binaries are not run | — |
| Anything that runs a GLM forward, STOP latency, memory, Windows CUDA | impossible | B-01…B-24 |

## 3. Repository layout and tracking (A9)

### 3.1 Layout

```
engines/glm/maya/            git submodule (only after §2.6 picks MAYA; see §3.2)
cmake/glm-engine/            ours: standalone wrapper project that builds the engine
  CMakeLists.txt
  CMakePresets.json          presets glm-linux-cuda, glm-windows-msvc-cuda
tools/glm-eval/              ours: the evaluation kit (§2)
tools/secure/glm_engine_audit.py     ours: bump audit (§3.4)
tools/secure/make_bundle.py          SP4's bundle builder (never shipped), extended for glm53 (§5.4)
docs/validation/glm-eval/    results files (§2.5)
docs/licenses/glm-engine.md  notices (§3.5)
```

`engines/glm/` holds only submodules; every file of ours is outside it. If the
evaluation picks a G53F fork, the path is `engines/glm/glm53f/` and every
`maya` below reads `glm53f`.

### 3.2 Submodule and pins

- **During evaluation (§1.3 part 2): no submodule.** The kit works from the
  verified bundles of §2.2. PROV keeps both sources as reference pins (its
  "GLM-5.3 compatibility reference" and "Project Maya" sections, unchanged).
- **After the decision:** `git submodule add https://github.com/mw00/project-maya.git engines/glm/maya`,
  checked out at the annotated or lightweight tag `v1.1.8`
  (`5c970a863ce2fdd87b6e23ffb4822f6d76522b1c`), no branch tracking. If a
  newer tag is chosen at integration time, the §3.4 runbook is run for it
  first. PROV's "Project Maya" section becomes "GLM engine baseline" with the
  gitlink, tag and tag object, in the same shape as the upstream "Baseline"
  section, and keeps PROV's existing sentence that the DFlash2 drafter
  weights (CC BY-NC-ND 4.0) are never bundled or fetched; the G53F section
  stays a reference pin (see §9 decision 3). The data file
  `data/expert-profile-glm.bin` used by §4.8.1 is recorded there with its
  SHA-256 at the tag.
- **No local patches.** `git -C engines/glm/maya status --porcelain` is empty;
  a needed change is offered upstream and, if unavoidable, carried on an
  owner-approved fork on GitHub whose URL replaces the submodule URL (A9).
- G53F has no GLM tags (R §5); a fork, if ever needed, must carry a tag, so
  the "pinned by tag" rule holds.
- PX's `glm-upstream` / `maya-upstream` git remotes are not added: the
  submodule URL is the tracking mechanism (amendment, §10.3).

### 3.3 CMake integration

Upstream and MAYA (and G53F) all declare `project(strata)` and the same
target names (`strata`, `strata-gguf`, `strata_core`, `strata_kernels`,
`strata_kernels_cpu`, …; MAYA `CMakeLists.txt:131-384`). CMake target names
are global, so the GLM engine can never be `add_subdirectory`'d into the root
project next to `upstream/`. It is built as a separate CMake project:

- **`cmake/glm-engine/CMakeLists.txt`** (ours) is a standalone project
  (`project(secure_strata_glm_engine LANGUAGES C CXX)`) that:
  1. fails with `engines/glm/maya is empty: run git submodule update --init engines/glm/maya`
     if `engines/glm/maya/CMakeLists.txt` is missing;
  2. runs `check_language(CUDA)`; without a CUDA compiler (or with the
     test-only cache variable `SECURE_STRATA_GLM_ASSUME_NO_CUDA=ON`) it fails
     with `the GLM engine needs the NVIDIA CUDA toolkit (nvcc 12.8 or newer); it is never built without CUDA`;
  3. requires `SECURE_STRATA_GLM_GGML_DIR`, a llama.cpp git checkout whose
     `git rev-parse HEAD` is `3cf03257f219afbe7334045ff7c6a06ac68c627d`;
     otherwise it fails with `SECURE_STRATA_GLM_GGML_DIR must name a llama.cpp checkout at 3cf03257 (the GLM engine is built offline)`;
  4. requires every entry of `SECURE_STRATA_GLM_CUDA_ARCHITECTURES` (default
     `native`) to be `native` or a number ≥ 75 (optionally `-real` /
     `-virtual`), else `the GLM engine needs compute capability 7.5 or newer`
     (MAYA's own guard says the same, `CMakeLists.txt:66-82`; ours fails
     before the engine's configure runs);
  5. on Windows requires `MSVC` with `MSVC_TOOLSET_VERSION` 143, else
     `the GLM engine on Windows is built with MSVC v143 (VS 2022) as the nvcc host: configure cmake/glm-engine from a VS 2022 x64 developer prompt`;
  6. requires `find_package(Git)` (MAYA applies `mmq-fused-swiglu.patch` to a
     build-tree copy with git, `CMakeLists.txt:886-907`), else
     `git is needed to apply the engine's pinned ggml patch`;
  7. seeds the engine's cache and adds it:
     `STRATA_ENABLE_CUDA=ON`, `STRATA_ENABLE_HIP=OFF`, `STRATA_PREFILL_MMQ=OFF`,
     `STRATA_NATIVE_EXPERTS=ON`, `STRATA_GGML_DIR=${SECURE_STRATA_GLM_GGML_DIR}`,
     `STRATA_BUILD_TESTS=OFF`, `STRATA_WERROR=OFF`, `STRATA_PORTABLE=ON`,
     `CMAKE_CUDA_ARCHITECTURES=${SECURE_STRATA_GLM_CUDA_ARCHITECTURES}`, then
     `add_subdirectory(${repo}/engines/glm/maya maya)`;
  8. adds a post-build step on target `strata` that fails unless
     `${CMAKE_BINARY_DIR}/strata-ggml-cuda/.strata-stamp` exists (MAYA
     writes the patched copy under the top-level binary directory,
     `CMakeLists.txt:873, 898`, which is the wrapper's) and copies
     `$<TARGET_FILE:strata>` to
     `${CMAKE_BINARY_DIR}/bin/strata-glm-engine${CMAKE_EXECUTABLE_SUFFIX}`
     and `engines/glm/maya/data/expert-profile-glm.bin` to
     `${CMAKE_BINARY_DIR}/bin/expert-profile-glm.bin` (read-only data, §4.8.1).
     The copies never touch the submodule; the rename removes the clash with
     our `strata` CLI. On MSVC Release, MAYA adds `/Zi` and `/DEBUG`
     (`CMakeLists.txt:15-17`), so a `.pdb` appears beside the binary; it is
     never copied to `bin/` and SP7 never ships it.
- **Root option:** `option(SECURE_STRATA_BUILD_GLM_ENGINE "Build the GLM-5.3 engine from engines/glm/maya (NVIDIA CUDA only)" OFF)`.
  When ON, the root `CMakeLists.txt` adds `ExternalProject_Add(glm_engine
  SOURCE_DIR ${CMAKE_SOURCE_DIR}/cmake/glm-engine BINARY_DIR ${CMAKE_BINARY_DIR}/glm-engine
  DOWNLOAD_COMMAND "" UPDATE_COMMAND "" INSTALL_COMMAND "" CMAKE_CACHE_ARGS <the SECURE_STRATA_GLM_* variables, CMAKE_BUILD_TYPE>)`.
  On Windows with a non-MSVC root compiler (our llvm-mingw / MSYS2 clang
  builds) the root fails at configure with the step-5 message: the owner
  builds `cmake/glm-engine` standalone from a VS 2022 prompt. Before the
  ExternalProject, the root runs the step-1 and step-2 checks itself so a
  missing CUDA fails at root configure time, not at build time.
- **CI:** never builds the engine. The configure guards run as CTest cases on
  the dev host and in CI (§8.1, `secure_frontend_glm_configure_tests.*`).
  `SECURE_STRATA_BUILD_GLM_ENGINE` stays OFF in every CI job.
- **Version:** at configure time of the root (option ON or OFF), if the
  submodule is present, `kGlmEngineVersion` = `git -C engines/glm/maya describe --tags --exact-match`
  with the leading `v` removed (`1.1.8`); a checkout not exactly at a tag
  fails unless `SECURE_STRATA_GLM_ENGINE_VERSION` is set. No submodule →
  `kGlmEngineVersion` is absent and every `glm53` manifest is
  `engine_profile_unavailable` (SP4 §2.3, exit 11).

### 3.4 Bumping the GLM engine

PROV gets a section "Updating the GLM engine" that follows the upstream
runbook's pattern (gates at the old pin first, per-run `out/<name>-<L>`
directories, one commit). Steps:

1. Gates at the old pin: `pytest -q`; `linux-gcc` and
   `windows-llvm-mingw-cross` configure/build/ctest; record counts.
2. `git -C engines/glm/maya fetch --tags origin && git -C engines/glm/maya checkout --detach <new tag>`.
3. `python3 tools/secure/glm_engine_audit.py engines/glm/maya > out/glm-audit-<new tag>.txt`
   and diff it against the committed `docs/security/glm-engine-audit.txt`.
   The audit prints, sorted and deduplicated, from the engine's C/C++/CUDA
   sources (`src/`, `include/`; never `ref/` or `serve/`):
   - every string literal matching `"STRATA_[A-Z0-9_]*"` or `"GLM_[A-Z0-9_]*"`,
     whatever function receives it. `getenv` is not the only reader: MAYA's
     local wrappers `env_num` and `env_on` read `STRATA_GLM_SLOTS`,
     `_SLOT_MIN`, `_SLOT_GB`, `_SLOT_DAYS`, `_LOOP_THINK`, `_LOOP_ANSWER`,
     `_CKPTS`, `_CKPT_EVERY`, `_MULTI_*` and `_MLA_WMMA`/`_MLA_F16` (241
     literals at `v1.1.8` against 227 `getenv` hits). The audit also lists
     the names of functions **and lambdas** that wrap `getenv` (any function or
     lambda body that calls `getenv` with a parameter: today the lambda
     `env_num` at `generate.cpp:1173` and the function `env_on` at
     `glm_prefill.cu:143`), so a new wrapper shows up as a diff;
   - every flag compared in the `--serve` argument parser (`a == "--…"`);
   - every `printf` format that starts with `READY`, `T `, `PP `, `STAT `,
     `NOTE `, `DONE `, `ERR`, `INFO`, `LP `, `VLENT`, `VRECLAIMED`;
   - every hit of `socket(`, `connect(`, `listen(`, `accept`, `getaddrinfo`,
     `WSAStartup`, `curl`, `http://`, `https://`;
   - every hit of `dlopen`, `LoadLibrary`, `GetProcAddress`;
   - every hit of `ofstream`, `fopen(` with a mode containing `w` or `a`,
     `CreateFile`, `create_directories`, `remove`, `rename`;
   - every `_Exit(`, `abort(`, `exit(` with its argument;
   - the CMake `FetchContent_Declare` blocks, `option(` lines, the CUDA arch
     guard, the MSVC Release flags, and the `third_party/*/LICENSE` file list;
   - for the Python pack tooling that runs on the box (R-1) —
     `tools/iq_pack.py`, `tools/strata_tokenizer.py`, `tools/gguf_reader.py`,
     `tools/_paths.py` — the SHA-256 of each file, its `import` lines, and
     every hit of `subprocess`, `os.system`, `urllib`, `http`, `socket`,
     `requests`, `open(` with a write mode;
   - the SHA-256 of `data/expert-profile-glm.bin`.
   Every difference is reviewed against §4 and §6 and either needs no action
   (stated in the commit body) or changes the flavor, profile, environment
   family, notices or this spec.
4. The three configure guards and the hermetic tests at the new pin.
5. **The box battery (§2.4) on Linux and Windows against the new pin.** A
   bump is not merged before its results files exist; until then it stays
   on a branch marked "awaiting box".
6. Update PROV (baseline, tag object), `docs/security/glm-engine-audit.txt`,
   `docs/licenses/glm-engine.md` if third-party files changed, and the
   catalog's `engine.min_version` only if a manifest needs the new engine.
7. One commit: `engines/glm: bump maya to <new tag>`, body listing gates
   (old → new counts), the audit differences and their disposition, and the
   results files.

The upstream runbook (`upstream/` bumps) gains one step: re-run the
configure guards, because `cmake/glm-engine` and upstream share the llama.cpp
pin `3cf03257` today; if upstream moves its pin, the GLM engine keeps its
own (MAYA `CMakeLists.txt:805-814`), and the kit's `pins.txt` names the commit
the engine needs, not upstream's.

### 3.5 License notices

`docs/licenses/glm-engine.md` (ours) lists, for the binary
`strata-glm-engine`, with the full text copied from the submodule at the
pinned tag (SP7 packages it next to the binary):

| Component | File at the tag | License | Why it is in the binary |
| --- | --- | --- | --- |
| Project Maya engine | `LICENSE` | MIT, "Copyright (c) 2026 Niko1221 and the Strata contributors" | the engine |
| ggml (llama.cpp `3cf03257`) | `third_party/ggml/LICENSE` and the llama.cpp checkout's `LICENSE` | MIT, the ggml authors | ggml-cpu and the MMQ sources are compiled in |
| exllamav3 | `third_party/exllamav3/LICENSE` | MIT, (c) 2025 Turboderp | `src/kernels/cuda/exl3.cu` and `src/kernels/cpu/exl3_cpu.cpp` derive from it and are compiled in (EXL3 quants are not supported by us; the code is present) |
| beellama.cpp | `third_party/beellama.cpp/LICENSE` | MIT, the ggml authors, Anbeeld | `src/core/glm_dflash.cu` derives from it and is compiled in; unreachable under §4.8/§4.9 |

Not shipped and not listed: `ref/upstream-llama.cpp/` (not compiled),
`serve/web/fonts/OFL.txt`, `third_party/wheels/`. **DFlash2 weights (CC
BY-NC-ND 4.0) are never fetched, bundled, cataloged or described as an
option**; our documentation states only that the engine runs without a
drafter. The audit (§3.4) fails review if a new `third_party/*/LICENSE`
appears without a row here.

## 4. Engine ↔ frontend contract

### 4.1 Profiles, flavors, families

| Manifest `profile` | `engine.family` | Executable | Argument profile | Protocol flavor | Environment family | Reachable from |
| --- | --- | --- | --- | --- | --- | --- |
| `qwen` | `strata` | upstream `strata` | `strata_0143` | `strata_0143` | `strata` (SP2 §3.9) | release |
| `glm53` | `glm` | `strata-glm-engine` (MAYA) | `maya_glm53` (§4.8.1) | `maya_glm53` (§4.2) | `glm` (§4.9) | release |
| — (evaluation only) | — | `strata-glm-decode` (G53F) | `lighttransport_glm53` (§4.8.2) | `lighttransport_glm53` (§4.7) | `glm` without the MAYA-forced values (§4.9) | `strata-engine-probe` (test-only) and the legacy `strata-secure-server --profile glm53`, both only in `SECURE_FRONTEND_TEST_HOOKS=ON` builds (§4.7) |

`EngineArgumentsProfile` and `EngineProtocolFlavor` each gain the value
`maya_glm53`. The executable is chosen by `engine.family` from the install
layout (SP7) or, until then, from config `engine.executables.glm` (an SP1
addendum: an absolute file path with SP1's `engine.executable` rules). A
`glm53` manifest never launches upstream `strata` and a `qwen` manifest never
launches `strata-glm-engine`: the family-to-executable map is a fixed table,
tested (§8.1). Upstream would refuse a GLM GGUF anyway (family check at
`v0.1.44.1`); MAYA has no family check, so the frontend's binding is the
control.

### 4.2 MAYA flavor grammar (`maya_glm53`)

All lines are ASCII printable, ≤ the inbound cap (8 192 bytes), split on
single spaces exactly as today. `u32` is a decimal without sign or leading
`+`; `metric` is SP0's `nonnegative_decimal_metric`; `int` is an optional
`-` followed by digits.

**Before READY:** `INFO <text>` lines are skipped (MAYA prints one,
`generate.cpp:1018`); anything else but READY fails.

**READY:** `READY <ctx:u32> <field>…` (MAYA `generate.cpp:2749-2750`).
Accepted only when the fields are exactly `stop pause` in that order. `multi=`
(only with `--parallel`, which §4.8 denies) fails with `unexpected_line`.
`ctx` must equal the `--max-context` value the frontend passed, else
`engine_context_mismatch` (a startup failure, SP2 counts it). `stop` sets
`supports_stop`; `pause` is recorded and never used.

**During a generation** (after `GEN`, until `DONE` or `ERR`):

| Line | Exact form (MAYA source) | Rule | Disposition |
| --- | --- | --- | --- |
| `T <id:u32>` | `generate.cpp:1742` | as today; `T <rid> <id>` (multi mode, `:2016`) fails `malformed_line` | token |
| `PP <done:u32> <total:u32> <ms:metric> <tok_s:metric> <reuse:u32>` | `"PP %s%zu %zu %.1f %.1f %zu\n"` at two sites, `generate.cpp:1580` (`reuse + done`) and `:1648` (chunk index `i`); the `%s` tag is empty outside multi mode | exactly 6 space-separated fields; `total` equals the prompt length the client sent (MAYA prints `prompt.size()`, and `prompt` is built from exactly the parsed ids, `:1694-1701`); `done ≤ total`; `reuse ≤ total`; allowed only before the first `T` of the generation; at most `total + 1` PP lines per generation | dropped (private heartbeat); the count is kept for the probe |
| `STAT <key>=<value> …` (today 12 keys: `tok_s ms_tok vram_hit ram_fetch disk promo vram_used vram_slots ram_used ram_slots ram_gb vram_gb`) | `generate.cpp:1794-1805` | 1–32 `key=value` fields; each key matches `[a-z_]{1,32}` and occurs once; each value is a `metric` or an `int`; the number of `STAT` lines seen so far ≤ ⌊tokens seen so far ÷ 8⌋ (MAYA prints one after every 8th `T`, `produced % 8 == 0`). The key set is not pinned, so an added counter in a later tag does not break generation; the audit (§3.4) still reports it | dropped; the last one is kept in memory for the probe only, never logged by the server |
| `NOTE <text>` | `generate.cpp:1756, 1780` (loop guard), `:1245`, `:1299`, `:1365`, `:1382`, `:1400` (slots) | `text` non-empty; at most 4 per generation | dropped; the server logs the fixed literal `engine note` with a per-generation count, never the text. Under §4.9 no `NOTE` source is active, so B-05 requires zero |
| `DONE <produced:u32> <prompt:u32> <prompt_ms:metric> <decode_ms:metric> <finish> <spec_hits:u32> <spec_steps:u32> <reuse:u32> <hits:u32> <lookups:u32>` | `generate.cpp:1875` | exactly 11 fields; `finish` ∈ {`stop`, `length`, `cancel`} (the only values MAYA assigns); `produced` equals the number of `T` lines; `prompt` equals the prompt length sent (same `prompt.size()` as `PP`) | terminal; idle |
| `ERR` / `ERR <text>` | 45 `printf("ERR…")` sites | as today | terminal; protocol failed (SP2 kills and restarts) |
| anything else | — | — | `unexpected_line`, protocol failed |

A `nonfinite` finish does not exist in MAYA; the SP0 `nonfinite` branch stays
`strata_0143`-only. Lines that arrive while idle (between `DONE` and the next
`GEN`) are read at the next `read_generation` and fail as `unexpected_line`;
B-05 confirms none occur.

The `strata_0143` flavor keeps rejecting `STAT` and `NOTE`; the
`lighttransport_glm53` flavor rejects `STAT`, `NOTE` and the 6-field `PP`.
`T`, `PP`, `DONE` and READY stay strict, and any unknown line prefix keeps
failing the protocol (TM "Engine process isolation": malformed engine output
fails closed).

Only one value is left for the box to confirm: whether `STAT`'s `int` fields
are ever negative (the grammar allows a sign either way). The grammar is
fail-closed, so any other wrong reading shows up as a G3 failure in B-05,
never as silent acceptance.

### 4.3 Commands the frontend sends to MAYA

`GEN`, `STOP`, `QUIT` only. MAYA's reader thread handles `STOP` mid-request
(R §3.3; `generate.cpp:2793` ignores a stray `STOP` between requests) and
`QUIT` ends the loop (`:2788`). `PAUSE`, `WRAP`, `LOGP`, `GENI`, `VLEND`,
`VRECLAIM` are never sent; `begin_generation` builds the only `GEN` the
client writes.

### 4.4 `GEN` and sampling (SP3 B6)

Form: `GEN <max_new> [key=value …] <id,id,…>`; MAYA takes the last field
without `=` as the id list (`generate.cpp:2898-2902`).

- No sampling keys → MAYA's start-up sampler, which is greedy
  (`Options::greedy = true`, `generate.cpp:108`; no sampling flag is passed,
  §4.8).
- SP3 B6 passes bounded `temperature` (0–2), `top_p` (0 < p ≤ 1) and `seed`
  (0–2⁶³−1). SP3 owns their validation and number formatting (C locale,
  no exponent). For the `maya_glm53` flavor:
  - `temperature=0` makes MAYA greedy (`:2906-2909`), matching OpenAI.
  - On the sampled path (any `temperature` > 0 or any `top_p`), the client
    always sends `top_p=<the request's value, or 1>` and `top_k=0`. MAYA's
    sampled path otherwise applies its own defaults `top_p` 0.95 and `top_k`
    20 (`generate.cpp:110-111`); OpenAI's default `top_p` is 1. `top_k=0`
    selects MAYA's widest list, which is still capped at 64 candidates
    (`sampler.hpp:16-17`): a residual difference from OpenAI sampling,
    documented in the API notes. `top_k` from API clients stays rejected
    (SP3 B6).
  - Key order on the line: `temperature`, `top_p`, `top_k`, `seed`.
  - `min_p`, `think_budget`, `think_end`, `logit_bias`, `pcie_frac`,
    `cpu_threads`, `read_chunks`, `rid`, `spec*` are never sent.
- `lighttransport_glm53`: `begin_generation` with any sampling key returns
  `invalid_argument` before writing (G53F rejects non-greedy `GEN` with `ERR`,
  `glm_decode.cpp:9518-9523`).

### 4.5 Context rule

MAYA's single-sequence path has no up-front `n + max_new + 8` check (R §3.3).
The frontend enforces the existing rule unchanged for every flavor:
`n + max_new + 8 ≤ ctx`, with `ctx` from READY (equal to the configured
context, §4.2), before `GEN` is written (`chat_execution.cpp`, test
`chat_applies_engine_context_rule_with_eight_token_margin`). SP3 B1/B2 clamp
`max_tokens` against the same `ctx`. This keeps every request out of MAYA's
three past-context behaviors (prefill `ERR`, `ERR` after some `T`, quiet
`length`).

The context is config `engine.glm.max_context` (SP1 addendum): integer
4 096–262 144, default 32 768, rendered as `--max-context <n>`. MAYA's own
default is 4 096 (`generate.cpp:107`); a larger context takes VRAM from the
expert tiers (MAYA README), which B-17 measures.

### 4.6 Exit and crash semantics

MAYA's serve watchdog (`start_serve_watchdog(180)`, `generate.cpp:726-747,
2744`) ends the engine with `std::_Exit(3)` after `STRATA_WATCHDOG_S` seconds
without progress, and on Windows first writes a minidump to the working
directory through `dbghelp.dll` (`:700-716`, called only from that watchdog,
`:743`). The `glm` family renders **`STRATA_WATCHDOG_S=0`**, which turns it
off (`limit <= 0` returns, `:729`) — §9 M5. A stalled generation is still
bounded by the frontend: SP1's `generation_timeout_s` (120), then `STOP`,
then `stop_drain_timeout_s` (5), then SP2's kill on a failed drain (SP2
§3.5 trigger 3). B-15 observes that path. SP2's §3.6 table is used unchanged:

| Observed (GLM family) | SP2 `ExitKind` | Engine source |
| --- | --- | --- |
| exit 3 | Linux `error_exit` (other exit code); Windows `abort` | not expected with the watchdog off; the UCRT `abort()` code is also 3 |
| exit 1 after an `ERR` line (G53F) | `error_exit` | failed reset → `ERR` then `_Exit(1)` (`glm_decode.cpp:9608-9611`) |
| exit 2 before READY | `error_exit` | MAYA argument errors return 2 (`generate.cpp:3307-3309`); with §4.8 this means a frontend bug or an engine CLI change |

All kinds count as failures under SP2's restart policy (3 failures, backoff
1 s → 60 s, 300 s window, then circuit open). MAYA's start-up takes minutes
(README: pinning host memory); B-04 and B-14 measure whether 900 s
`ready_timeout` and the restart budget fit, and §10.2 records the risk.

### 4.7 G53F flavor (`lighttransport_glm53`, evaluation only)

Unchanged from today (`engine_protocol_client.cpp`): `INFO` before READY,
`READY <ctx> stop prefill-includes-reset` (`glm_decode.cpp:9436-9437`; extra
fields ignored as today), `PP` in the 3- or 5-token forms, `T <id>`,
`DONE … <finish> 0 0 0`, `ERR`. Additions: READY `ctx` must equal argv
context (`engine_context_mismatch`), and no sampling keys (§4.4). It is never
reachable from the release `strata` CLI. Because the G53F binary contains a
listener (§6.1), the legacy `strata-secure-server --profile glm53` is gated
**now** (plan 1), not when SP1 retires that binary: in builds with
`SECURE_FRONTEND_TEST_HOOKS=OFF` it fails before spawn with the fixed message
`--profile glm53 launches an evaluation-only engine and needs a test-hooks build`;
`strata-engine-probe` is itself test-only. After the decision its fate is §9
decision 3.

### 4.8 Argument profiles

#### 4.8.1 `maya_glm53`

The frontend owns the whole argv for a managed model:

```
strata-glm-engine --serve --glm-pack <canonical version dir>/<launch.glm_pack> --max-context <engine.glm.max_context>
                  --prefill auto --kv-streaming on --expert-profile <engine dir>/expert-profile-glm.bin
```

The last three are **frontend-owned constants** (§9 M1): the configuration
MAYA's own setup writes (`maya.py:1814-1817`, `KV_STREAMING = "on"` at `:89`,
and `--expert-profile` with `data/expert-profile-glm.bin` at `:312,
1864-1865`). Without an expert profile MAYA downgrades `--prefill auto` to a
fixed 2 048-token chunk (`generate.cpp:3434-3436`). The profile is read-only
data from the submodule at the pinned tag, copied beside the binary by
`cmake/glm-engine` (§3.3 step 8) and recorded with its SHA-256 in PROV; the
kit passes `<maya source>/data/expert-profile-glm.bin`.

MAYA parses only `--flag value` (space-separated, `next()`,
`generate.cpp:3094-3098`) and treats an unknown argument as fatal
(`:3307-3309`), so the frontend always renders two argv elements and never
`--flag=value`.

Operator arguments (`--engine-arg`) are accepted only in PX's `serve --expert`
bring-up mode, and only from this allow-list; everything else is rejected
before spawn with `owned_flag` (the owned `--serve`, `--glm-pack`,
`--max-context`, `--prefill`, `--kv-streaming`, `--expert-profile`, plus
`--native`, `--pack`, `--gpu`) or `unsupported_mode` (all others):

| Allowed flag | Value | Bound |
| --- | --- | --- |
| `--vram-reserve-mib` | integer | 0–262 144 |

Explicitly denied (each has a test row, §8.1), grouped by reason:

| Reason | Flags |
| --- | --- |
| frontend-owned | `--serve`, `--glm-pack`, `--max-context`, `--prefill` (the prompt chunk size, `generate.cpp:3209-3213`; owned as `auto`), `--kv-streaming`, `--expert-profile`, `--pack`, `--native` (a preset name in MAYA, `:3187`), `--gpu` (no such flag in MAYA; GPU selection is §4.9) |
| protocol the frontend does not speak | `--parallel`, `--vision`, `--prefill-until`, `--tokens`, `--tokens-file`, `--max-new`, `--stream-token` |
| tuning knobs (A10; the flag sets a `STRATA_GLM_*` variable internally, `generate.cpp:3330-3337`) | `--kv`, `--kv-resident` |
| DFlash2 (A7) | `--dflash` |
| multi-GPU (A8) | `--layer-split`, `--split-device`, `--expert-cache-device1`, `--expert-cache-device2`, `--expert-cache-device3`, `--expert-cache-remote-placement` |
| file writers | `--expert-profile-save`, `--expert-profile-save-every`, `--prompt-cache`, `--prompt-cache-every`, `--prompt-cache-root`, every `--dump-*` |
| diagnostics and test modes | `--check-logits`, `--spec-oracle`, `--spec-corrupt`, `--graph-only`, `--cpu-oracle-q8-0`, `--stage-timing`, `--sync-every-layer`, `--logits-stride` |
| sampling defaults (sampling is per request, §4.4) | `--greedy`, `--temperature`, `--top-k`, `--top-p`, `--seed` |
| everything else in MAYA's ≈ 120 flags | not allowed (allow-list) |

SP5's typed pass-through table may extend the allow-list later; A10's "no
tuning knobs" covers the environment, and for managed models the flag-side
equivalent is that no operator argument passes (the constants above are
fixed, not tunable).

#### 4.8.2 `lighttransport_glm53` (evaluation)

G53F's serve CLI is positional (`glm_decode.cpp:9337-9342`). The profile
becomes typed instead of pass-through:

```
strata-glm-decode --serve <shard 1 path> <context> <dense-MiB> <threads> [expert-MiB] [prefill-batch] [eos-id] [gpu-budget-MiB] [lookup-depth] [gpu-decode-experts]
```

`--serve`, the shard path (from the model directory) and the context (from
the probe's `--max-context`) are frontend-owned. The operator supplies 2–8
values for the remaining positions, each a decimal integer within: dense-MiB
0–1 048 576; threads 1–1 024; expert-MiB 0–1 048 576; prefill-batch 1–4 096;
eos-id −1 or 0–154 879; gpu-budget-MiB 0–1 048 576; lookup-depth 0–16;
gpu-decode-experts 0–1. Any argument starting with `-` (beyond the owned
`--serve`), and any argument past position 10, is rejected, which keeps out
`--batch`, `--remote-tp*`, `--speculative=external`, `--draft-worker-config`
and the `--check-*` / `--dump-*` / `--profile*` / `--eval-*` / `--bench` /
`--routing-trace` families (R §3.2). The lookup-depth and eos-id bounds are
marked `[to confirm on the box]`; a too-tight bound fails closed.

### 4.9 Environment family `glm` (A10, A11; SP2 §3.9)

SP2's `EngineEnvironment` gains a family. The `glm` family renders exactly:

| Variable | Value | Why |
| --- | --- | --- |
| POSIX `PATH` | `/usr/bin:/bin` | SP2 |
| POSIX `LD_LIBRARY_PATH` | SP2 `lib_dirs` | CUDA runtime and cuBLAS are shared on Linux (MAYA links `CUDA::cudart`, `CUDA::cublas`) |
| `TMPDIR` / Windows `TEMP`, `TMP` | `<state_dir>/engine-tmp/<gen>` | SP2 (the working directory stays SP2 §3.12's `<state_dir>/engine`) |
| `CUDA_CACHE_PATH` | `<cache_dir>/cuda` | SP2 |
| `CUDA_DEVICE_ORDER` | `PCI_BUS_ID` | MAYA has no `--gpu` flag (R §3.3), so GPU selection is environment for this family only |
| `CUDA_VISIBLE_DEVICES` | the single integer of SP1's `gpu` list | A8 single GPU; a `gpu` list with more than one entry is a config error for a `glm53` model (`gpu_list_too_long_for_glm`); an absent list renders `0` |
| `STRATA_GLM_SPLIT` | `0` | forces one device even if a driver exposes more (MAYA's split is the default on multi-GPU hosts, `glm_model.cu:1717-1735`) |
| `STRATA_GLM_SLOTS` | `0` | A10: conversation slots off; with `slot_max` 0 MAYA neither creates nor empties the slot directory (`generate.cpp:1174, 1252-1256`) |
| `STRATA_GLM_LOOP_THINK` | `0` | A11: loop guard off while thinking; `0` turns the check off (`generate.cpp:1724-1727`, `looping()` returns 0 for `w ≤ 0`) |
| `STRATA_GLM_LOOP_ANSWER` | `0` | A11: loop guard off in the answer (same lines) |
| `STRATA_WATCHDOG_S` | `0` | §4.6, §9 M5: MAYA's watchdog off; replaces SP2's optional `watchdog_s` rendering for this family |
| `STRATA_GLM_RAM_HEADROOM_GB` | config `engine.glm.ram_headroom_gb`, integer 6–1 024, default 16 | §9 M2 (needs an A10 amendment): RAM MAYA leaves free when sizing its pinned tier (`glm_fast_path.cu:973, 1337`; MAYA default 6; MAYA `docs/SETTINGS.md:28` reports a desktop that stayed responsive at 16 and swapped at 12). If the owner declines M2, this row is removed and the variable joins the never-list |
| Windows `SystemRoot`, `WINDIR`, `SystemDrive`, `NUMBER_OF_PROCESSORS`, `PATH` (`lib_dirs`, then `%SystemRoot%\System32`) | SP2 | SP2 |

Never present, asserted absent by test with the parent environment setting
each of them: everything on SP2's never-list; `STRATA_GLM_DFLASH` (A7; MAYA
reads it at load, `glm_model.cu:1702`); `STRATA_GLM_NO_REUSE` (see §9
decision 5); `STRATA_GLM_SLOT_DIR`, `STRATA_GLM_SLOT_KEEP`,
`STRATA_GLM_SLOT_MIN`, `STRATA_GLM_SLOT_GB`, `STRATA_GLM_SLOT_DAYS`;
`STRATA_GLM_USAGE`, `STRATA_GLM_TRACE`, `STRATA_QSA_DUMP`,
`STRATA_QSA_DUMP_LAST`, `STRATA_GLM_SCORE`, `STRATA_GLM_SCORE_DUMP`,
`STRATA_GLM_SCORE_REF`, `STRATA_GLM_SCORE_TOPK`, `GLM_CB_DIR` (file writers,
§6.2); `STRATA_GLM_DEVS`, `STRATA_GLM_DEV1` (multi-GPU); `STRATA_GLM_SLOW`,
`STRATA_GLM_KV_INT8`, `STRATA_GLM_KV_RESIDENT`, `STRATA_GLM_CKPTS`,
`STRATA_GLM_CKPT_EVERY`, `STRATA_GLM_MULTI_BENCH`, `STRATA_GLM_MULTI_BOUNDARY`,
`STRATA_GLM_MULTI_ROOM`, `STRATA_GLM_MULTI_SPEC`, `STRATA_GLM_MLA_WMMA`,
`STRATA_GLM_PROFILE_WEIGHT`, `STRATA_GLM_KDA_GRAPH` and every other
`STRATA_*` (A10: no tuning knob); `STRATA_WATCHDOG_IO_S` (SP2 renders it for
upstream; MAYA's sources at `v1.1.8` contain no `WATCHDOG_IO` literal, so the
`glm` family omits it); `STRATA_HIPBLASLT_TUNING` (A4); `HIP_VISIBLE_DEVICES`;
`STRATA_GLM_REMOTE_TP`, `STRATA_GLM_REMOTE_TP_*`, `STRATA_GLM_ALLOW_TUNE`
(G53F).

The variable names above are confirmed from MAYA's source at `v1.1.8`
(`getenv`, `env_num` and `env_on` sites cited); what the box confirms is
their **effect**: B-19 shows no `slots/` directory appears, B-05 shows zero
`NOTE` lines (p08 is the loop-prone prompt), B-17 shows one GPU in use and
the available memory, B-04's `INFO` line shows `dflash=0`. A `STRATA_GLM_*`
name that changes in a later tag is caught by the audit (§3.4).

The forced `STRATA_GLM_*` values (`SPLIT`, `SLOTS`, `LOOP_THINK`,
`LOOP_ANSWER`) and `STRATA_WATCHDOG_S=0` are safety settings, authorized by
A10/A11 (slots off, loop guard off) and A8 (single GPU); they are constants,
not knobs. `STRATA_GLM_RAM_HEADROOM_GB` is the only typed value and exists
only if the owner amends A10 (M2).

For the G53F evaluation path the family renders SP2's base rows and the two
CUDA selectors only (G53F does not read the MAYA variables; nothing else is
added).

### 4.10 Output post-processing for `glm53` (chat execution)

Two kinds of token ids in a MAYA generation are not ordinary text, and our
`decode_token_ids` refuses control (type 3) and user-defined (type 4) ids
(`tokenizer_pack.cpp:294-296`), which `chat_execution.cpp:118` turns into
`output_decode_failed`. Without the rules below every GLM chat response
would fail:

1. **End of turn.** MAYA prints the end-of-turn token as a `T` line and then
   `DONE … stop` (`generate.cpp:1742` prints every token; the EOS check that
   sets `finish = "stop"` follows at `:1807-1811`; GLM's EOS ids are
   `{154820, 154827, 154829}`, `glm_eos` at `:1035`). The frontend keeps the
   fixed table `kGlmEndOfTurnIds = {154820, 154827, 154829}`; the `glm53`
   layout check (§5.2 rule 7) requires each to be a type-3 or type-4 id in
   `token_type.json` and to appear as a value in `tokenizer.json`
   `special_ids`, so a pack that disagrees with the table is refused. On
   `finish = stop`, chat execution drops **exactly one** trailing id when it
   is in the table; any other end-of-turn id elsewhere in the output is
   handled by rule 3. The protocol client stays token-pure; the flavor does
   not rewrite ids.
2. **Reasoning.** Our renderer opens `<think>` (`glm_prompt_renderer.cpp`),
   so the output is reasoning, the user-defined `</think>` id (type 4, MAYA
   `strata_tokenizer.py:118-121`), then the answer. The `</think>` id is
   looked up once from the pack's vocabulary by its literal (exactly one
   entry whose literal is `</think>`, and it is type 4 — MAYA also marks
   `<think>`, `<tool_call>`, `<tool_response>` as type 4,
   `strata_tokenizer.py:120`; else `invalid_tokenizer_pack`). **Until SP5's
   thinking handling lands, the chat route strips the reasoning:**
   - output contains `</think>` → content is the decoded ids after the first
     `</think>`; the ids before it are never decoded or returned;
   - `finish = stop` and **no** `</think>` → the **whole** decoded output
     (after rules 1 and 3) is the content; never an empty `200`. MAYA closes
     thinking itself only under `think_budget`, the loop guard or the server's
     wrap (`generate.cpp:1043, 1135-1139, 1788-1790`), none of which we use,
     so whether the model always emits `</think>` before ending its turn is
     `[to confirm on the box]` (B-05 and B-21 record whether a
     stop-without-`</think>` output was observed);
   - `length`, timeout or cancel with no `</think>` (still thinking) → empty
     content, `finish_reason` per SP3's mapping (`length` for both length and
     timeout, B4). The think phase counts against `max_tokens` and
     `generation_timeout_s` like any other output.

   Streaming (SP3): content deltas are emitted only after `</think>` has been
   seen; until then the stream sends SP3's keep-alive comments
   (`limits.http.stream_keepalive_ms`, every 15 s while no content chunk has
   been sent; SP3's write-stall bound applies only to pending output). If the
   generation ends with `stop` and no `</think>`, the whole buffered text is
   sent as one content chunk before the terminal chunk.
   Why strip rather than pass the reasoning through: OpenAI
   chat clients render `content` as the answer, and raw reasoning text in it
   would read as the answer, double the visible output and expose a format
   SP5 has not yet designed; dropping it is the conservative default and
   loses nothing SP5 cannot add back as a separate field.
3. **Any other type-3/4 id in the content** (after `</think>`, other than
   the one trailing end-of-turn id) is removed before decoding, and chat
   execution counts it in the fixed log literal `engine emitted control
   token`; decoding never fails because of such ids. With tools disabled
   (EXT-01) MAYA has no reason to emit tool tags; B-05 records whether any
   appear.

The G53F evaluation path applies the same rules (its probe output is decoded
the same way; G53F prints EOS as `T` before `DONE` too, R §3.2).

## 5. Model artifacts (A5)

### 5.1 What MAYA loads

- `--glm-pack <dir>`: MAYA's pack. The engine finds the GGUF shards in the
  pack's parent directory (`maya.py:1622-1623`, "the engine finds the GGUF
  files at `<pack>/..`").
- Pack files (`maya.py:313-314` `PACK_FILES`, MAYA `tools/iq_pack.py`):
  `index.txt`, `native_experts.txt`, `dense.bin` (≈ 1 GB of small float
  weights, `docs/SETUP.md:22`), `compat-bf16.json` (written with
  `--compat-bf16`, which `maya.py` always passes, `:1652-1655`), and
  `tokenizer/{vocab.json, merges.txt, token_type.json, tokenizer.json,
  chat_template.jinja}`. `experts.bin` is optional and never built by us
  (the engine then reads experts from the GGUF, `iq_pack.py:344-345`).
- `sources.json` is a `maya.py` staleness stamp, not read by the engine; we
  never produce it.

### 5.2 Manifest schema 2 for `glm53`

SP4 §2.2 reserves `glm53` with only `launch.native` in schema 1 and says SP6
bumps the schema. Schema 2 is schema 1 plus:

| `launch` key (`glm53`) | Required | Value | Argument |
| --- | --- | --- | --- |
| `native` | yes | shard `00001` | none (layout anchor; MAYA finds shards via the pack) |
| `glm_pack` | yes | the directory `pack` that is a **direct child of the shard directory** | `--glm-pack <path>` |
| `tokenizer_pack` | yes | `<glm_pack>/tokenizer` | frontend `--tokenizer-pack <path>` |

Schema-2 rules: `engine.family` must be `glm` for `glm53`;
`engine.min_version` is compared with `kGlmEngineVersion` (§3.3); a schema-1
`glm53` manifest is accepted by `pull`/`import` and refused by `run`/`serve`
(`engine_profile_unavailable`). Schema 2 changes nothing for `qwen` or `raw`.
`files[].role`: shards `weights`, pack files `pack`, tokenizer files
`tokenizer`. No file path may contain a `slots` component or end in
`experts.bin`; no `vision` or `draft` role (schema 1 already rejects those).

Layout check for `glm53` (extends SP4 §4.4, `pack_layout_invalid` on any
failure):

1. The shard set rule of SP4 (one directory, complete `00001..MMMMM`).
2. `glm_pack` is `<dir of launch.native>/pack` exactly.
3. The pack holds `index.txt`, `native_experts.txt`, `dense.bin`,
   `compat-bf16.json` and the five tokenizer files listed in §5.1.
4. The first line of `native_experts.txt` contains `absolute offsets in
   <basename of launch.native>,` (`iq_pack.py:438-445` writes `src.name`).
5. `tokenizer/tokenizer.json` has `"pre"` equal to `glm4`, `glm5` or
   `chatglm-bpe` (MAYA's `strata_tokenizer.py` accepts `qwen35`, `glm4`,
   `glm5`, `:94-95`, and writes `pre` into `tokenizer.json`, `:328-333`).
6. `load_tokenizer_pack(<glm_pack>/tokenizer)` succeeds.
7. Each id of `kGlmEndOfTurnIds` (§4.10) is type 3 or 4 in `token_type.json`
   and a value in `tokenizer.json` `special_ids`; the vocabulary has exactly
   one entry whose literal is `</think>`, and it is type 4.

### 5.3 Catalog entries (SP4 §2.6, D4 moved them here)

Source: `peasantsmith/GLM-5.3-Flash-Maya-GGUF` at revision
`f16ee4ba50d8fd2bb726ea1862ca6ca14ba188da` (SP4 facts table; `license:mit`,
not gated). Sizes are `maya.py` `download_gb` (decimal GB) at `v1.1.8`.

| `name:tag` | Folder / file pattern at the revision | Shards | Download | Disk after import (shards + pack ≈ 1 GB) | `engine.min_version` |
| --- | --- | --- | --- | --- | --- |
| `glm-5.3-flash:maya-s` | `Maya-S-v2-IQ2_XXS/GLM-5.3-Flash-Maya-S-v2-IQ2_XXS-0000N-of-00003.gguf` | 3 | 96.5 GB | ≈ 97.5 GB | `1.1.8` |
| `glm-5.3-flash:maya-s24` (§9 decision 2) | `Maya-S24/GLM-5.3-Flash-Maya-S24-IQ2_XXS_S-0000N-of-00003.gguf` | 3 | 94.7 GB | ≈ 95.7 GB | `1.1.8` |
| `glm-5.3-flash:maya-m` | `Maya-M/GLM-5.3-Flash-Maya-M-IQ2_S-0000N-of-00003.gguf` | 3 | 116.0 GB | ≈ 117 GB | `1.1.8` |
| `glm-5.3-flash:maya-l` | `Maya-L/GLM-5.3-Flash-Maya-L-IQ3_S-0000N-of-00004.gguf` | 4 | 156.3 GB | ≈ 157 GB | `1.1.8` |
| `glm-5.3-flash:default` | alias of `maya-s` (§9 decision 1) | | | | |

- **Digests:** each shard's SHA-256 is taken from the LFS pointer at the
  pinned revision and must equal an entry of `maya.py`'s `sha256` table at
  `v1.1.8` (`maya.py:104-200`; shard 1 has two accepted digests there, the
  `glm5-next` and the older `glm5next` metadata spelling — the manifest pins
  the one the revision serves). A mismatch stops the catalog commit. PROV
  records repo, revision and every digest in the same commit (DL-05).
- **Pack files** are listed by size and digest **without `sources`** (SP4
  §2.7) and completed by the bundle builder (§5.4).
- **License:** `license.id` `MIT`, `license.url`
  `https://huggingface.co/peasantsmith/GLM-5.3-Flash-Maya-GGUF`,
  `bundle_allowed` `true`, `notice` "GLM-5.3-Flash weights (Z.ai) and Project
  Maya quantization, MIT."
- **Not in the catalog:** `Maya-M-Derisked` (third-party weight edit, separate
  repository), EXL3, the vision mmproj and vocab files, any drafter. Other
  GGUF IQ/K quants of GLM-5.3-Flash (A5) are usable only as third-party
  manifests (`--manifest … --trust-manifest sha256:<hex>`, SP4 D2) that pass
  the §5.2 layout check; nothing in SP6 vouches for them.

Memory guidance printed by `pull` (from MAYA's README, not measured by us):
"RAM and VRAM together should exceed the model size for good speed; 32 GB
RAM is the published minimum; a fast NVMe SSD is required." B-17 records
measured VRAM and host memory per quant on the owner's box; those numbers
replace the published guidance in `pull`'s message once recorded.

### 5.4 Pack building and determinism

SP4 D5 is unresolved; SP4 ships option (D), import from a developer-built
bundle, and SP6 uses the same path:

- `tools/secure/make_bundle.py <dir> --profile glm53` (never shipped; runs
  on the owner's box or an owner-approved machine, never on the dev host,
  R-1/§9 M4) runs **MAYA's** `tools/iq_pack.py` from the submodule at the
  pinned tag, unmodified, with `--gguf <shard 1>`, `--out pack`,
  `--compat-bf16`, and `STRATA_GGUF_PY=<llama.cpp 3cf03257>/gguf-py`.
  `iq_pack.py` makes the source path absolute (`pathlib.Path(a.gguf).absolute()`,
  `:350`) and records it in `compat-bf16.json` (`"source"`, `:263-266`), so
  the builder always runs at one **canonical path**: on Linux it enters a
  private mount namespace (`unshare --user --map-root-user --mount`) and
  bind-mounts the bundle's shard directory at `/strata-pack/files`, then runs
  with `--gguf /strata-pack/files/<basename of shard 1>`. Pack building is
  Linux-only. `regex` (`strata_tokenizer.py:22`) is already pinned in
  `scripts/dev/requirements-dev.txt`; plan 1 adds a pinned numpy there.
- The builder then runs `tools/secure/secure_tokenizer.py`'s `extract` on the
  same shard into a scratch directory and requires `vocab.json`,
  `merges.txt` and `token_type.json` byte-identical to the pack's, and the
  same `pre` in `tokenizer.json`; a difference stops the bundle.
- Each produced file is checked against the manifest's digest; then
  `SHA256SUMS` and `STRATA-BUNDLE` are written (SP4 §2.7).
- This is pack tooling, not MAYA's quant tooling (`tools/maya_quant/`, A6);
  the audit (§3.4) covers its four Python files at every bump.

**Determinism question** (needed before any owner-hosted pack, SP4 D5 (A)):
build the pack twice for `maya-s` on one machine and once on a second
machine, each at the canonical path, and compare SHA-256 of every pack file.
`compat-bf16.json` is reproducible **only** because of the canonical path; a
pack built elsewhere differs in that file. Not yet known: whether `dense.bin`
BF16 conversion and the tokenizer JSON key order are stable across numpy
versions. The experiment needs ≈ 97 GB of download and ≈ 250 GB of disk; it
runs on the box or an owner-approved machine (§2.7), is recorded in the
handoff, and its outcome feeds SP4 D5.

### 5.5 Tokenizer

**Prerequisite bug fix (first tasks of plan 1, R-3).** The frontend
tokenizes (MAYA takes ids), and our GLM tokenization is wrong today in two
ways, for the MAYA path and the G53F path alike:

1. **Merges.** llama.cpp sets `ignore_merges = true` for `glm4`/`glm5`: a
   pre-token piece that is an exact vocabulary entry is emitted whole, and
   only other pieces go through BPE (MAYA `tools/strata_tokenizer.py:72-75,
   94-97`). `tools/secure/secure_tokenizer.py` transcribed only the regex and
   keeps applying merges; the C++ tokenizer (`tokenizer_pack.cpp`) does the
   same.
2. **Pre-tokenizer.** The C++ tokenizer uses a generic split
   (`basic_pretokenize`, commented "a model-specific pre-tokenizer must be
   added before claiming exact parity"), and `secure_tokenizer.py` knows
   `glm4`/`chatglm-bpe` but not `glm5`.

The fix: `secure_tokenizer.py` accepts `glm5` beside `glm4` and the
`chatglm-bpe` alias and implements `ignore_merges` for them; the C++
tokenizer gets the same regex (numeric runs capped at three digits, the
same letter/space/punctuation classes) and `ignore_merges`, selected by
`tokenizer.json` `pre`; an unknown `pre` is `invalid_tokenizer_pack`. Which
`pre` the pinned shards declare (`tokenizer.ggml.pre`) is confirmed by a
ranged read of the GGUF header of each quant's shard 1 (§2.7) and recorded
in PROV.

Decoding: GLM output contains end-of-turn and `</think>` ids that our
decoder refuses by type; §4.10 defines how chat execution removes them before
decoding (part of the same plan-1 prerequisite work).

- Golden fixtures: `tools/secure/gen_glm_tokenizer_fixtures.py` (dev-only)
  writes `secure_frontend/tests/fixtures/glm-tokenize.txt` (input, pieces,
  ids over a small committed byte-BPE vocabulary that contains some whole
  pieces) for 200 generated strings (digits, CJK, emoji, mixed whitespace,
  punctuation, combining marks) using the fixed `secure_tokenizer.py`,
  after the fix has been reviewed line by line against MAYA's
  `strata_tokenizer.py`. The C++ test compares piece by piece and id by id.
  On the box, B-24 checks our ids against MAYA's tokenizer on the real
  vocabulary; its outputs become captured fixtures.
- Prompt rendering stays our fixed GLM profile (`glm_prompt_renderer.cpp`,
  ends `<|assistant|><think>`). The pack's `chat_template.jinja` is data that
  the layout check requires and nothing executes. A dev-host check
  (`secure_frontend_glm_profile_tests.renders_like_maya_chat_template_fixture`)
  compares our rendering of `p01`–`p05` with expected strings derived by
  reading MAYA's template at `v1.1.8`, committed as fixtures.

### 5.6 What SP4's inventory must learn

1. Schema 2 and the `glm53` `launch` keys, roles and layout check (§5.2).
2. `kGlmEngineVersion` gating per family (§3.3).
3. The `glm53` → `glm` family → executable binding (§4.1).
4. `pull` / `show` print the memory guidance of §5.3 for `glm53` models.
5. `doctor` (SP7) reports a model directory that contains a `pack/slots`
   directory as a finding (left by a non-frontend MAYA run on the same
   files).

## 6. Security review items

### 6.1 Network code

- **MAYA:** the C++/CUDA engine sources outside `ref/` contain no socket,
  connect, listen, getaddrinfo, curl, HTTP or Winsock code (R §3.3). All of
  MAYA's network code is Python (`serve/`, `maya.py`) or configure-time
  FetchContent; we ship neither, and §2.3/§3.3 make the configure offline.
  Controls: the audit's network grep at every bump (§3.4); B-02 on the
  binary; B-18 at run time (gate G2). The "no network code in the engine
  binary" rule is **our** threat-model policy (TM "Engine process
  isolation"), not a property MAYA promises; making it an eligibility gate is
  §9 M6.
- **G53F:** `strata-glm-decode` links `strata_ucomm` unconditionally; rank 0
  binds `INADDR_ANY` and listens, unauthenticated (R §3.2). Policy (argument
  and environment denial) can keep it unused, but the binary contains a
  listener, so **the G53F binary is rejected as a product engine** (G2). It is
  run only by the evaluation probe on the owner's box, with
  `STRATA_GLM_REMOTE_TP*` absent and `--remote-tp*` unreachable (§4.8.2).
  Eligibility needs a fork that compiles `ucomm` out (§2.6).

### 6.2 File writes by the engine

| Writer (MAYA) | Trigger | Under §4.8/§4.9 |
| --- | --- | --- |
| conversation slots `<glm_pack>/slots/slot-*.bin` and `.meta` (token ids) | default on, `STRATA_GLM_SLOTS` ≠ 0 (`generate.cpp:1174, 1253-1256, 1320-1345`) | **off** (`STRATA_GLM_SLOTS=0`) |
| expert profile | `--expert-profile-save` | denied flag |
| prompt cache | `--prompt-cache*` | denied flags |
| dumps | `--dump-*`, `STRATA_QSA_DUMP`, `STRATA_GLM_SCORE_DUMP`, `GLM_CB_DIR`, `STRATA_GLM_TRACE`, `STRATA_GLM_USAGE` | denied flags / never-listed variables |
| Windows stall minidump `maya-stall-<pid>.dmp` (`MiniDumpWithThreadInfo`) in the working directory | only from the watchdog (`generate.cpp:700-716`, called at `:743`) | **unreachable**: `STRATA_WATCHDOG_S=0` (§4.6, §9 M5) |

The working directory stays SP2 §3.12's `<state_dir>/engine` (owner-only);
SP6 changes nothing about it. A stall minidump would hold thread stacks and
so possibly prompt token ids (PRIV-02); with the watchdog off it cannot be
written, and B-19 checks that no file appears in the working directory.
Linux core dumps: the engine inherits SP1's `RLIMIT_CORE = 0` (SP1 §3.6);
B-23 records whether the box pipes cores to a handler such as
`systemd-coredump`, which may ignore that limit.

The model directory stays read-only in practice: B-19 checks that nothing is
created in it (gate G5). SP4's version directories are `0700` owned by the
service user, so the engine *could* write there; making them read-only to the
engine (separate user or Landlock) is SP7's sandbox work (§10.1).

### 6.3 Environment surface

MAYA reads 227 distinct `STRATA_*` variables (R §3.3), and several flags work
by setting variables internally (`--dflash` sets `STRATA_GLM_DFLASH`;
`--kv`/`--kv-resident` set `STRATA_GLM_KV_*`, `generate.cpp:3330-3337`). The
`glm` family is therefore an exact allow-list (§4.9): four forced
`STRATA_GLM_*` values, `STRATA_WATCHDOG_S=0`, the one typed
`STRATA_GLM_RAM_HEADROOM_GB` (M2), SP2's base set, two CUDA selectors,
nothing copied from the parent. Test `glm_family_env_is_exact_allowlist`
fails on any extra variable. The audit (§3.4) diffs every `"STRATA_…"` and
`"GLM_…"` literal at every bump, whichever wrapper reads it.

### 6.4 Runtime library loading

- Upstream's `strata_kq` pattern (a library loaded by path from the engine's
  folder) does not exist in MAYA (based on Strata 0.1.27). MAYA's only
  dynamic load is `LoadLibraryA("dbghelp.dll")` on Windows for the stall
  minidump (`generate.cpp:702`), reachable only from the watchdog, which the
  `glm` family turns off. G53F loads `dxgi.dll` and `dbghelp.dll` by name and
  `libibverbs` via `dlopen` (ucomm).
- Windows resolves a bare-name `LoadLibraryA` through the standard search
  order, which starts with the **application directory**. Control: the engine
  install directory is not writable by the service user (SP7 installer;
  B-03 records the loaded-module paths, which must be System32, the engine
  directory, or a `lib_dirs` entry). The working directory is SP2's private
  `<state_dir>/engine`.
- cuBLAS (`cublas64_*.dll`, `cublasLt64_*.dll`) is a load-time dependency on
  Windows and `libcublas*.so` on Linux; SP7 decides whether they ship beside
  the engine or come from the CUDA toolkit through `lib_dirs`. Either way they
  are verified like the engine binary (SHA-256 in the release manifest).
- The audit's `dlopen`/`LoadLibrary` grep (§3.4) catches any new pattern.

### 6.5 Crash and watchdog

MAYA's `_Exit(3)` watchdog is off (§4.6); a stall is bounded by the
frontend's generation timeout, `STOP`, drain and kill. `_Exit(1)` (G53F reset
failure) and any other exit end the process without unwinding; the supervisor
treats every exit as a failure (SP2 §3.6), kills the whole group/Job (no
grandchildren expected; MAYA spawns no processes, R §3.3) and applies the
restart policy. No crash material leaves the box (PRIV-01).

### 6.6 Other items

- **Pinned host memory:** MAYA pins all free RAM but `STRATA_GLM_RAM_HEADROOM_GB`
  (default 6) for its expert tier (README; `docs/SETTINGS.md:28`). Pinned
  memory cannot be swapped, so on a shared box this can starve the frontend
  and the OS. The body assumes §9 M2 (a typed headroom, default 16); B-17
  records the minimum available memory either way.
- **Cross-request state in RAM:** with slots off, MAYA still keeps in-memory
  prefix reuse and up to 4 checkpoints (~150 MB each, `generate.cpp:1414-1431`)
  across requests. One API client can infer from latency whether another
  client recently sent the same prefix (a timing side channel among holders
  of the same token). Recorded in TM; §9 decision 5, to be revisited when
  multiple keys or per-application tokens land (SP1's deferred item).
- **Loop guard off (A11):** without it, a looping answer runs to
  `max_tokens` or `generation_timeout_s`; both are bounded by SP3.
- **Windows Error Reporting:** SP2's `SEM_NOGPFAULTERRORBOX` and
  `JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION` apply.
- **Python and build scripts we run:** nothing of MAYA's Python runs in the
  product. Its pack tooling runs on the owner's box (or an owner-approved
  machine) at build-pack time only, and its CMake scripts run wherever the
  engine is built (box; dev host only confined, R-1). Both are covered by the
  audit; the residual is §9 M4.

## 7. Build and validation on the box

### 7.1 Toolchain floors

| Item | Linux | Windows |
| --- | --- | --- |
| GPU | NVIDIA, compute capability ≥ 7.5 (RTX 20 and newer) | same |
| CUDA toolkit | 12.8 or newer (sm_120 needs 12.8; MAYA README) | 12.8 or newer; 13.x accepted for RTX 20+ (MAYA `docs/WINDOWS.md`) |
| nvcc host compiler | the distribution GCC that the toolkit release supports | MSVC v143 (VS 2022 Build Tools), checked by `MSVC_TOOLSET_VERSION` 143 (A3) |
| CMake / Ninja | ≥ 3.24 (MAYA `CMakeLists.txt:10`) / any | same |
| Git | required (MAYA's ggml patch) | same |
| Driver | the version the toolkit requires; recorded | same |

`CMAKE_CUDA_ARCHITECTURES` is `native` on the box; the list for a
redistributable binary is SP7's (it changes build time and binary size).

### 7.2 What CI does

- No GPU job and no engine build. `SECURE_STRATA_BUILD_GLM_ENGINE` is OFF.
- The configure guards (§3.3 steps 1–5) run as CTest cases in every preset:
  each configures `cmake/glm-engine` into a scratch directory with the
  condition forced and expects the exact message (§8.1).
- `pytest -q` runs the audit tool's tests, the results-file schema test and
  the bundle-builder tests (fake `iq_pack.py`).

### 7.3 Offline validation kit entries SP7 packages

SP7's offline GPU validation kit includes, for GLM:

1. `tools/glm-eval/` (scripts, prompts, `pins.txt`, `pins-models.txt`).
2. `strata-engine-probe` built for the box's OS.
3. The source bundles and `SHA256SUMS` from `fetch-sources.sh`.
4. A runbook page `docs/validation/sp6-glm-kit.md`: §2.2, §2.3, §2.4 with
   the exact commands, and where to put the results files.
5. SP3's `strata-api-probe` for B-21.
6. The binary-only wheels (numpy, `regex`) for `build-pack.sh`.

The release engine binaries themselves are built by hand on the owner's box
(no CI runner has nvcc or a GPU), §9 M3: SP7 records each binary's SHA-256,
the exact `cmake/glm-engine` preset and cache values, and the toolchain
versions of §7.1 in the release record.

## 8. Testing strategy

### 8.1 Hermetic tests on the dev host

All C++ cases are `secure_frontend_<binary>_tests.<case>` registered with
`secure_frontend_add_cases`; they run in `linux-gcc`, the wine
`windows-llvm-mingw-cross` run and the MSYS2 job. Written test-first (TDD):
each lands failing, then the code.

**Fixtures.** `secure_frontend/tests/fixtures/engine-transcripts/`:

- `maya-v1.1.8-synthetic/*.txt` — transcribed from MAYA's `printf` formats
  (§4.2 citations) with chosen values: a READY; a generation with 3 `PP`, 24
  `T`, 3 `STAT`, `DONE … stop`; one with `NOTE`; one with `STOP` →
  `DONE … cancel`; one with a `STAT` carrying a 13th, unknown key (accepted);
  and malformed variants (tagged `PP`, `T <rid> <id>`, `STAT` before the 8th
  `T`, `STAT` with a duplicated key, an upper-case key or 33 keys, `PP` after
  `T`, `DONE` with 10 fields, `finish` `nonfinite`, READY with `multi=2`, an
  unknown line prefix).
- `glm53f-4894c996-synthetic/*.txt` — from `glm_decode.cpp:9436-9437, 9540,
  9600-9616`.
- `maya-v1.1.8-captured/`, `glm53f-4894c996-captured/` — copied from the
  results' `transcripts/` after the box run; the replay case iterates every
  file in each directory.

**Cases.**

| Binary | Cases |
| --- | --- |
| `engine_protocol_client` | `maya_flavor_runs_generation_with_pp_stat_and_note`, `maya_flavor_rejects_multi_mode_tags_and_rids`, `maya_flavor_bounds_stat_cadence_to_every_eighth_token`, `maya_flavor_accepts_bounded_unknown_stat_keys`, `maya_flavor_rejects_malformed_stat`, `maya_flavor_pp_total_must_equal_prompt_length`, `maya_flavor_rejects_pp_after_first_token`, `maya_flavor_note_is_bounded_and_never_returned`, `maya_flavor_done_requires_eleven_fields_and_known_finish`, `maya_flavor_done_counts_must_match`, `maya_flavor_ready_requires_stop_pause_and_rejects_multi`, `maya_flavor_rejects_unknown_line_prefix`, `ready_context_must_equal_configured_context`, `maya_flavor_sampled_gen_always_sends_top_p_and_top_k_zero`, `maya_flavor_greedy_gen_sends_no_keys`, `glm53f_flavor_refuses_sampling_keys`, `strata_flavor_rejects_stat_and_note`, `glm53f_flavor_rejects_six_field_pp`, `replays_recorded_transcripts` |
| `engine_arguments` | `maya_profile_owns_serve_pack_context_and_constants` (`--prefill auto`, `--kv-streaming on`, `--expert-profile <engine dir>/expert-profile-glm.bin`), `maya_profile_renders_space_separated_values`, `maya_profile_rejects_each_denied_flag` (table over §4.8.1, both spellings), `maya_profile_expert_mode_allowlist_bounds`, `maya_profile_managed_model_takes_no_operator_arguments`, `glm53f_profile_types_positional_arguments`, `glm53f_profile_rejects_named_flags_and_extra_positions` (replaces `glm_profile_passes_arguments_through_without_native`) |
| `engine_environment` (SP2) | `glm_family_env_is_exact_allowlist`, `glm_family_forces_slots_loop_guard_split_and_watchdog_off`, `glm_family_never_passes_dflash_tuning_or_writer_vars` (parent sets every never-listed name), `glm_family_renders_single_cuda_visible_device`, `glm_family_rejects_more_than_one_gpu`, `glm_family_ram_headroom_is_typed_and_bounded` (M2), `glm53f_family_has_no_strata_glm_values` |
| `engine_supervisor` (SP2) | `glm_stall_is_bounded_by_generation_timeout_then_kill` (fake engine `--hang-in-gen`), `glm_ready_context_mismatch_counts_as_start_failure`, `glm_working_directory_is_sp2_engine_dir` |
| `secure_server` (legacy binary, plan 1) | `glm53_profile_refused_without_test_hooks` (label `release-only`), `glm53_profile_allowed_with_test_hooks` |
| `engine_launch` (SP4 §4.9 c) | `family_selects_executable_glm_vs_strata`, `glm53_manifest_never_launches_upstream_engine` |
| `manifest` (SP4) | `glm53_schema2_launch_keys_validated`, `glm53_schema1_manifest_refused_by_run`, `glm53_engine_family_must_be_glm`, `glm53_pack_must_be_child_pack_of_shard_dir`, `glm53_rejects_slots_paths_and_experts_bin` |
| `model_inventory` / layout (SP4) | `glm53_layout_requires_pack_and_tokenizer_files`, `glm53_native_experts_header_must_name_shard_one`, `glm53_tokenizer_pre_must_be_glm4_glm5_or_alias` |
| `tokenizer_pack` (plan 1, first) | `glm_pretokenizer_matches_python_fixtures`, `glm_ignore_merges_emits_whole_vocab_pieces`, `glm_ids_match_python_fixtures`, `chatglm_bpe_alias_selects_glm4`, `glm5_pre_is_accepted`, `unknown_pre_is_rejected` |
| `glm_profile` | `renders_like_maya_chat_template_fixture` |
| `chat_execution` (plan 1) | `glm53_drops_one_trailing_end_of_turn_on_stop`, `glm53_keeps_non_trailing_and_non_stop_ids_out_of_decode`, `glm53_strips_reasoning_before_think_close`, `glm53_stop_without_think_close_returns_full_text`, `glm53_length_without_think_close_returns_empty_content`, `glm53_stream_stop_without_think_close_flushes_full_text`, `glm53_removes_other_control_ids_and_never_fails_decode`, `glm53_stream_emits_only_after_think_close` (SP3 streaming path) |
| `tokenizer_pack` / layout | `glm53_layout_requires_end_of_turn_ids_and_single_think_close` |
| `api_server` (SP3, fake engine `--wire=maya`) | `glm_maya_stream_hides_stat_pp_and_note`, `glm_maya_sampling_keys_reach_gen` |
| `glm_configure` (CTest `add_test` cases named `secure_frontend_glm_configure_tests.<case>`, registered by a helper next to `secure_frontend_add_cases`) | `requires_submodule`, `requires_cuda_toolkit` (`SECURE_STRATA_GLM_ASSUME_NO_CUDA=ON`), `requires_pinned_ggml_dir` (missing; wrong commit), `rejects_cuda_arch_below_75`, `windows_requires_msvc_v143` (MSYS2 job only, label `windows-runtime`), `root_option_off_by_default` |

The fake engine (SP2 F1) gains `--wire=maya`: READY `stop pause`, `PP` in
MAYA's 6-field form, a `STAT` after every 8th `T`, optional `--note-at=<k>`,
DONE in the 11-field form.

**Python (pytest):** `tools/secure/test_secure_tokenizer.py` and
`test_glm_tokenizer.py` gain `glm5` and `ignore_merges` cases (plan 1,
first); `tools/secure/test_glm_engine_audit.py` (a tiny fixture tree with
one of each grep target, including a literal read through an `env_num`-style
wrapper and a Python file with a `subprocess` call; output sorted and stable),
`tools/secure/test_glm_eval_results.py` (schema of §2.5 over
`docs/validation/glm-eval/*.json` and fixtures),
`tools/secure/test_make_bundle_glm.py` (a fake `iq_pack.py` records its argv
and the mount it sees: `--gguf /strata-pack/files/<shard 1>`, `--compat-bf16`,
`--out pack`; tokenizer cross-check passes and fails as designed; skipped
where unprivileged user namespaces are unavailable).
`strata-engine-probe`'s raw mode (B-10) is compiled only with
`SECURE_FRONTEND_TEST_HOOKS=ON`; `secure_frontend_engine_probe_tests.raw_mode_absent_without_test_hooks`
(label `release-only`) checks it.

### 8.2 GPU-box battery (manual acceptance)

The battery of §2.4, with the results file of §2.5, is the acceptance test
for every GPU claim: in the evaluation (§1.3 part 2), at integration (part 4)
and at every bump (§3.4 step 5). Results are committed; CI validates their
shape only. The handoff's "Verification performed" quotes the results file
names, never a summary without them.

## 9. Owner decisions

Only what A1–A11 leave open. The body assumes each recommendation.

1. **Default pull (`glm-5.3-flash:default`).** Options: `maya-s`, `maya-s24`,
   `maya-m`. **Recommend `maya-s`** (MAYA's own default; smallest download
   that MAYA marks as its main compact quant). Cost: owners of 24 GB cards
   get ~14% slower decode than with S24 (MAYA README) unless they pull S24.
2. **Include Maya-S24 in the catalog.** A5 names S/M/L; S24 is MAYA's variant
   of S for cards of 24 GB or less. **Recommend yes** (one more pin; MAYA's
   setup picks it automatically for ≤ 24 GB cards, `maya.py:968-974`).
3. **G53F after the evaluation.** Options: (a) keep its flavor, profile and
   probe path as a long-term Linux cross-check rerun at each MAYA bump; (b)
   keep only the PROV reference pin and the captured transcripts, and delete
   the `lighttransport_glm53` flavor/profile and the legacy
   `strata-secure-server --profile glm53` path once SP1 retires that binary.
   **Recommend (b)**: less code that speaks to a binary with a listener; the
   evaluation's results files stay. (The legacy path is already gated to
   test-hooks builds by plan 1, §4.7.)
4. **Fallback if MAYA is not eligible (especially on Windows).** Options: (a)
   ship GLM on Linux only and record Windows GLM as a known gap; (b) an
   owner-approved fork of MAYA carrying the minimal fix (A9), with an
   upstream PR in parallel; (c) an owner-approved fork of G53F with a `ucomm`
   stub, `_putenv_s` and `std::popcount` (R §1.3), then re-evaluate.
   **Recommend (b) when the failing gate has a small, reviewable fix, else
   (a)**; (c) last, because it also needs the listener removed and drags in
   G53F's upstream merges at every bump.
5. **In-memory prefix reuse.** A10 allows `STRATA_GLM_SLOTS=0` or
   `STRATA_GLM_NO_REUSE=1`. Options: (a) `STRATA_GLM_SLOTS=0` only — no disk
   state, in-RAM reuse and checkpoints kept (faster multi-turn chat; a
   latency side channel among clients sharing the token, §6.6); (b) both —
   no cross-request state at all, every request re-reads its whole prompt.
   **Recommend (a)**, with the side channel recorded in TM; the frontend has
   one shared token, so every client is already fully trusted with every
   other client's API access. Revisit when multiple keys or per-application
   tokens land (SP1's deferred item).

Decisions raised by review round 1 (controller rulings):

- **M1 — MAYA's shipped configuration as frontend constants.** Options: (a)
  pass `--prefill auto --kv-streaming on --expert-profile <engine
  dir>/expert-profile-glm.bin` always (what MAYA's own setup writes); (b)
  pass none (MAYA defaults: fixed 2 048-token prefill chunk without a
  profile, `generate.cpp:3434-3436`). **Recommend (a)** (§4.8.1): the
  battery then measures the configuration MAYA's users run.
- **M2 — RAM headroom.** Options: (a) accept MAYA's 6 GB default (no
  variable passes, A10 as written); (b) amend A10 to allow one typed,
  bounded value `STRATA_GLM_RAM_HEADROOM_GB` from config
  `engine.glm.ram_headroom_gb` (6–1 024). **Recommend (b) with default 16**
  (MAYA `docs/SETTINGS.md:28`: a desktop swapped at 12 and stayed responsive
  at 16; pinned memory cannot be swapped out).
- **M3 — Hand-built release engine.** No CI runner has nvcc or a GPU, so the
  Linux and Windows GLM binaries for a release are built by hand on the
  owner's box. **Recommend accepting this for v1**, with SP7 recording the
  binary SHA-256, the exact build recipe (preset, cache values) and the
  toolchain versions; a reproducible MSVC+CUDA build host is deferred.
- **M4 — Running MAYA code off the box.** **Recommend:** on the dev host only
  a confined, CPU-only configure and build inside reviewed plan tasks (no
  network, no `HOME`, read-only source, §2.7); MAYA's pack tooling and
  anything needing model files only on the owner's box or a machine the
  owner approves by name. Residual: we still run a one-week-old repository's
  build scripts on the dev host.
- **M5 — MAYA's watchdog.** Options: (a) render `STRATA_WATCHDOG_S=0` (off)
  for the `glm` family and rely on SP2's generation timeout, `STOP`, drain
  and kill; (b) keep it on, which makes the Windows minidump of thread
  stacks reachable in SP2's working directory and would need a per-run
  temp-directory amendment to SP2. **Recommend (a)** (§4.6); SP2's working
  directory is unchanged.
- **M6 — B-02 as a gate.** The static "no network imports" check is our TM
  policy, not a MAYA promise. **Recommend keeping it an eligibility gate
  (G2)**: it is the only check that sees a listener which no test path
  happens to start.
- **M7 — G53F reference model.** Options: (a) G53F arm load-only (build,
  static checks, a READY attempt on a Maya quant); (b) the owner adds the
  ≈ 100 GB Unsloth UD-Q2_K_XL model G53F is documented with (licence not
  checked) for a full cross-check. **Recommend (a)**; G53F cannot be eligible
  without a fork anyway (§2.6).

## 10. Out of scope, risks, amendments, global constraints

### 10.1 Out of scope / deferred

AMD/HIP and other GPU APIs (A4); multi-GPU layer split (A8, later); G53F
remote TP (A8, never); EXL3 (A5, later); vision; `--parallel`; DFlash2
(never); MAYA's Python service, setup, dashboard, updater, MCP client and
quant tooling (A6); a C++ pack builder and owner-hosted packs (SP4 D5);
redistributable CUDA arch lists, shipping cuBLAS, release signing (SP7);
running the engine as a separate user or under Landlock so the model
directory is read-only to it (SP7 sandbox); thinking-text handling in API
responses (SP5); upstream PRs to MAYA for quieter output (`STAT`/`NOTE`
switches) or slots-off defaults (worth offering; not required).

### 10.2 Risks

- **MAYA's maturity and churn.** One week old, fresh history, ≈ 44 tags in 8
  days, test wiring unpublished, several docs contradicting the code (R
  §6). The box battery is the only correctness evidence we will have; each
  bump costs a box run (§3.4 step 5).
- **There is no ready fallback.** G53F as shipped fails G1 (Windows) and G2
  (listener); if MAYA fails a gate, every way forward is a fork or a scope
  cut (§9 decision 4).
- **Wire-format drift fails closed.** `STAT` cannot be switched off and its
  `STAT` key set is open (bounded), but `T`, `PP`, `DONE` and READY are
  strict and unknown prefixes fail; a format change there in a MAYA tag
  breaks generation until the flavor is updated. The audit (§3.4) makes this
  a review item at bump time rather than a field failure.
- **Resource behavior on the box.** Minutes-long start-up and nearly all
  free RAM pinned: a slow or memory-tight box could hit `ready_timeout` or
  trip SP2's circuit after three restarts. B-04, B-14 and B-17 measure it;
  the remedy may need M2 or an owner change to SP1/SP2 defaults.
- **Tokenizer correctness.** Our GLM tokenization is wrong, and every GLM
  response would fail to decode (end-of-turn and `</think>` ids), until the
  plan-1 prerequisite fixes land (§5.5, §4.10); B-24 is the first check against MAYA's own
  tokenizer on the real vocabulary.
- **Running a young repository's build and pack scripts** (M4).
- **Pack reproducibility unknown** (§5.4); until it is shown, every model
  needs a developer-built bundle.
- **Upstream drift.** MAYA is based on Strata 0.1.27; upstream fixes after
  that do not reach the GLM engine.

### 10.3 Amendments this spec requires (reviewed with it)

| Document | Text | Amendment |
| --- | --- | --- |
| PX §11 | "SP6: `engines/glm/` tracking `glm53f`; GLM manifest entries" | "SP6: evaluation of MAYA and G53F; `engines/glm/<chosen>` as a pinned submodule; GLM manifest entries" |
| PX tracking bullet | "`engines/glm/`, imported as a subtree (or a pinned submodule — SP6 picks) from `glm-upstream`/`glm53f`" and the `glm-upstream` / `maya-upstream` remotes | pinned submodule (A9) under `engines/glm/<name>`; no extra remotes; the submodule URL tracks |
| SP2 §3.9 | `EngineEnvironment` (one rendering) | gains a family field (`strata`, `glm`); the `glm` family renders the §4.9 table |
| SP2 §3.9 | "any other `STRATA_*`" is never present | the `glm` family forces `STRATA_GLM_SPLIT=0`, `STRATA_GLM_SLOTS=0`, `STRATA_GLM_LOOP_THINK=0`, `STRATA_GLM_LOOP_ANSWER=0` (authorized by A10/A11, A8) and `STRATA_WATCHDOG_S=0` (M5) instead of SP2's optional `watchdog_s`; plus `STRATA_GLM_RAM_HEADROOM_GB` only if M2 amends A10 |
| SP2 §3.9 | "Rejected: frontend-set `CUDA_VISIBLE_DEVICES`" | holds for the `strata` family; the `glm` family selects its single GPU by environment because MAYA has no `--gpu` flag (§4.9) |
| SP2 §3.12 | working directory `<state_dir>/engine` | **no change** (R-2) |
| SP2 §3.6 | exit-kind table | no change (§4.6) |
| SP3 | chat response content | for `glm53`, content is the answer after `</think>`; reasoning is stripped until SP5 (§4.10) |
| SP1 | config schema | adds `engine.glm.max_context` (4 096–262 144, default 32 768), `engine.executables.glm` (until SP7's install layout) and, if M2, `engine.glm.ram_headroom_gb` (6–1 024, default 16). No `limits.engine.watchdog_s` key is added |
| A10 (owner) | "no tuning knobs pass" | only if M2 (b): one typed, bounded `STRATA_GLM_RAM_HEADROOM_GB` |
| SP4 §2.2–§2.4, §4.4 | schema 1, `glm53` with `launch.native` only | schema 2 (§5.2), layout check for `glm53` |
| PROV | "Project Maya" and "GLM-5.3 compatibility reference" | unchanged during evaluation; after the decision, "GLM engine baseline" (with the DFlash2 exclusion and the expert-profile digest) + "Updating the GLM engine" (§3.2, §3.4) |
| TM | engine isolation | the `glm` environment family; the no-network-code rule as an eligibility gate (M6); the G53F listener exclusion; the in-RAM prefix-reuse side channel; the watchdog off and its minidump unreachable |
| EPL | Strata-only | a MAYA section with the §4.2 grammar and the context-rule note |

### 10.4 Global constraints

- No third-party dependency in the frontend; the engine is a separate
  process built from its own submodule.
- Engine code stays in its submodule, unmodified (A9); our build logic lives
  in `cmake/glm-engine/`; nothing under `engines/glm/` or `upstream/` is
  edited.
- `-Werror` for our code on every preset; the engine builds with its own
  `STRATA_WERROR=OFF` and its warnings are not ours to fix.
- TDD: every item lands with its failing-first test; one commit per item
  naming the item and its tests.
- Hermetic tests only on the dev host and in CI; no GPU, no network, no
  model weights.
- **Nothing is claimed GPU-verified from the dev host.** Every GPU statement
  in the handoff names the results file it comes from.
- `docs/AGENT_HANDOFF.md` and the roadmap are updated with every SP6 item.
