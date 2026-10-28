# SP6 research (2026-10-14): GLM-5.3 engine sources, lighttransport `glm53f` vs Project Maya

Read-only comparison for the SP6 brainstorm and spec. Nothing was built, run,
or downloaded from either source; both were cloned blobless into a scratch
directory and read at their pinned revisions.

| Name used here | Source | Pinned revision |
|---|---|---|
| **upstream** | `Niko1221/Strata` (our `upstream/` submodule) | tag `v0.1.43`, `7695859a` |
| **glm53f** | `https://github.com/lighttransport/Strata.git`, branch `glm53f` | `4894c9967f5754a2052d7b05f10a01b6071bb6e4` (2026-10-12) |
| **maya** | `https://github.com/mw00/project-maya.git` | tag `v1.1.8`, `5c970a863ce2fdd87b6e23ffb4822f6d76522b1c` (2026-10-14) |

Paths are relative to the root of the named repository. `[inference]` marks a
conclusion that was not verified directly. "Ours" means this repository.

**Branch movement since the pin:** `origin/glm53f` is 2 commits ahead of
`4894c996` as of 2026-10-14: `24af2c28` "Add guarded opt-in GLM prefill and
bounded drafting experiments" and `7a33c9c7` "Fix Q2_K LUT shuffle immediates
for Clang and Debug builds". `v1.1.8` is maya's current `HEAD` (0 commits after
it).

---

## 1. The three codebases at a glance

### 1.1 Lineage

Both GLM sources derive from Strata, in different ways.

**glm53f is a git fork that shares Strata's history and keeps merging it.**
- The merge base with lighttransport `origin/main` is `d6708a4a` (2026-09-30,
  "Engine 0.1.29 …"). lighttransport `main` is a stale mirror of upstream at
  0.1.29; its tags stop at `v0.1.29`.
- Upstream `main` was merged into the branch three times. Examples:
  `5800fef1` (2026-10-08) "Merge upstream Strata main (0.1.41) into the GLM
  branch" and `60c5d58e` (2026-10-11) "Merge upstream Strata main (235
  commits) into the GLM branch".
- The newest upstream commit merged in is `61b3fb5d` (2026-10-10). It is 72
  commits behind `v0.1.43`; `7695859a` (v0.1.43) is present in the clone but
  is not an ancestor of the pin.
- `CMakeLists.txt:10` reads `project(strata VERSION 0.1.42 …)`.
- `LICENSE` is byte-identical to upstream's. `README.md:1` begins
  "Strata-GLM53F: GLM-5.3-Flash on desktops and budget servers" and keeps
  upstream's README below it.
- The GLM work is effectively one author's. The first GLM commit is `c9229b3a`
  (2026-10-01, "glm: add GLM-5.3-Flash inference and benchmarks"), and Syoyo
  Fujita has 78 commits on `7695859a..HEAD`. Niko1221's 418 commits in that
  range are upstream commits that arrived through the merges.

**maya is a fresh-history rewrite of an older Strata.**
- `README.md:167-169`: "Maya's engine started from Strata's and was rewritten
  for GLM-5.3-Flash … and its server and dashboard grew from Strata's."
- `CMakeLists.txt:11` reads `project(strata VERSION 0.1.27 …)`.
  `docs/GLM5-FLASH.md:4-5` is "Written 2026-09-29 against engine 0.1.27
  (`main`)".
- The Strata name is kept: `namespace strata`, binary `strata`
  (`CMakeLists.txt:384`), `STRATA_GLM_*` env vars.
- The git history is new. The root commit is `a813352` (2026-10-07, mw00),
  801 files and +145,287 lines. Strata's commits are absent; `7695859a` is not
  a valid object in the clone.
- 88 of maya's 853 tracked files are byte-identical to upstream v0.1.43
  (`include/strata/core/layout.hpp` and `src/core/layout.cpp` among them), 199
  share a path but differ, and 566 are maya-only.
- `src/program/generate.cpp` is 7,355 lines in maya against 13,437 in upstream.
- `LICENSE` is byte-identical to upstream's. `LICENSE:3` carries only
  "Copyright (c) 2026 Niko1221 and the Strata contributors". There is no mw00
  line.

### 1.2 Size, languages, build, backends

LOC counts come from `git ls-files` plus `wc -l`. upstream and glm53f exclude
`third_party/`.

| | upstream v0.1.43 | glm53f @4894c996 | maya v1.1.8 |
|---|---|---|---|
| Tracked files | 2,976 | 8,353 (4,108 `.json`; 4,222 under `docs/`, mostly bench data) | 853 |
| C++ `.cpp` / `.hpp` | 315 / 200 files; 178k / 50k LOC | 378 / 236 files; 198k / 54k LOC | 117 / 103 files; 64k / 11k LOC |
| CUDA `.cu` | 63 files, 36k LOC | 76 files, 43k LOC | 76 files, 45k LOC |
| C | headers | 13 `.c` (ucomm, `src/host/glm_*.c`) | vendored llama.cpp reference only (`ref/`) |
| Python | 338 files, 77k LOC | 1,012 files, 127k LOC | 94 files, 29k LOC |
| Other | SYCL (`sycl/`) | Vulkan `.comp` shaders (`vulkan/shaders/{kda,kda_fused,mhc,mla}.comp`) | one hand-written `serve/web/app.js` (2.1k LOC); no JS deps |
| Build | CMake ≥ 3.24 | CMake ≥ 3.24 (`CMakeLists.txt:9`) | CMake |
| GLM engine binary | none | `strata-glm-decode` (`CMakeLists.txt:1604`, CUDA or HIP only); plus `strata-glm-tp-worker` (CPU, :1403) and `strata-glm-vulkan` (`vulkan/CMakeLists.txt:18`) | `strata` (`CMakeLists.txt:384`), GLM path selected by `--glm-pack` (`src/program/generate.cpp:3102, 3328`) |
| CUDA arch floor | default 120; < 75 fatal unless `STRATA_EXPERIMENTAL_SM60` (`CMakeLists.txt:155-180`); warns for sm_120 with CUDA < 13.0 (:186-190) | same guard, but sm_70 allowed for GLM (`CMakeLists.txt:160-191`); docs build sm_120 and sm_70 with CUDA 12.9 (`docs/GLM53_V100.md:317`) | default 120, < 75 refused (`CMakeLists.txt:66-82`); README:78 claims V100 (7.0), which `maya.py:1028-1033` reaches with `CMAKE_CUDA_ARCHITECTURES=native` |
| HIP | `STRATA_ENABLE_HIP` gfx1100/gfx1201 (`CMakeLists.txt:71`); `STRATA_HIP_GFX906` | same options; GLM `.cu` compiled as HIP with `-ffp-contract=off` (`CMakeLists.txt:505-507`); run on RX 9070 XT (`docs/GLM_B550_REAP50.md`) | `cmake/hip_backend.cmake`: gfx1100/gfx1201 validated; `docs/AMD_MAYA.md`: Linux ROCm 7, **text only**; no EXL3 on HIP (`maya.py:963-967`) |
| Other GPU | SYCL (Intel Arc, experimental) | Vulkan GLM runner, RADV only, "Windows execution [has] not been tested" (`docs/GLM_VULKAN.md:6`) | none |
| CPU-only inference | no | no; `strata-glm-decode` needs CUDA/HIP (`CMakeLists.txt:1601-1605`) | no; the `strata` engine needs CUDA/HIP (`CMakeLists.txt:157`) |
| Configure-time network | FetchContent of llama.cpp `3cf03257…` when `STRATA_NATIVE_EXPERTS=ON` and no `STRATA_GGML_DIR` | same (`CMakeLists.txt:1339-1342`) | same (`CMakeLists.txt:805-814`); also git-applies `third_party/ggml/mmq-fused-swiglu.patch` (:868-908) |

### 1.3 OS support and the Windows build

- **upstream:** Windows 10/11 and Linux (`README.md:61`). On Windows, `setup.py`
  finds `vcvars64.bat` (`setup.py:1390-1397`), so MSVC is the nvcc host. This
  matches our recorded caveat (`docs/AGENT_HANDOFF.md`, "CUDA engine builds on
  Windows still need MSVC as the nvcc host compiler").
- **glm53f:** the GLM engine is effectively Linux-only.
  - `strata_ucomm` (C11, `sys/socket.h`, `accept4`) is built whenever
    `STRATA_NATIVE_EXPERTS` is ON (`CMakeLists.txt:1398-1402`), with no `WIN32`
    guard.
  - `strata-glm-decode` links it unconditionally (`CMakeLists.txt:1605`).
  - `CMakeLists.txt:313` calls ucomm "Linux-only".
  - `src/program/glm_decode.cpp:53-63` wraps its POSIX headers in
    `#ifdef __linux__`. `getrusage` and `external_worker` are guarded too.
  - The constructs that break MSVC are:
    - `setenv` at :9773 and :9807.
    - `__builtin_popcount` at :5010.
    - An unconditional `#include` of `ucomm.h` at :33 and a call to
      `ucomm_init` at :7570.
    - Unguarded POSIX headers in `tools/ucomm/src/ucomm_boot.c:6-16`
      (`sys/socket.h`, `netdb.h`, `arpa/inet.h`, `poll.h`, `unistd.h`).
  - A Windows build of `strata-glm-decode` fails as shipped (verified by
    reading the code, not by building).
  - The fix is a moderate set of source patches, not only CMake changes:
    - a ucomm stub or Windows port;
    - `setenv` → `_putenv_s`;
    - `__builtin_popcount` → `std::popcount`.
- **maya:** Windows is "Experimental" (`docs/WINDOWS.md:5-8`).
  - Toolchain: VS 2022 Build Tools (MSVC v143) plus CUDA 12.8. CUDA 13 works for
    RTX 20 and newer, and one user reports CUDA 13.4 on Windows 11 with an RTX 4090.
  - `maya.py:1107-1119` finds `vcvars64.bat` and builds with Ninja, so MSVC is
    the nvcc host.
  - CUDA links `cudart_static` on Windows (`CMakeLists.txt:87-91`).
  - There are no CMake presets, and the build is driven by `maya.py`.
  - HIP on Windows uses AMD's HIP SDK or TheRock wheels (`maya.py:511`).

### 1.4 Python and service parts (all outside our trust boundary)

- **upstream:** `serve/` (Python service), `setup.py`, `chat.py`, `tools/`. We
  keep these untouched and quarantined at package time.
- **glm53f:** upstream's `serve/` with GLM additions. `serve/server.py:1954`
  rejects non-text input and :3445 rejects tool calls. There are about 100
  `tools/glm_*` scripts, `tools/sim/glm_sim.py`, and
  `tools/glm_external_draft_worker.py`, which talks HTTP to a loopback
  llama-server (:57-62).
- **maya:** the following parts.
  - `serve/server.py`: stdlib `ThreadingHTTPServer` (:43, :3145), default bind
    `127.0.0.1` (:3353), OpenAI- and Anthropic-compatible. It runs the engine as
    `subprocess.Popen([exe, "--serve", …], stdin=PIPE, stdout=PIPE)` (:420).
  - `serve/update.py:85-89`: GitHub "releases/latest" check, at most every 6 h,
    only when the dashboard asks; `MAYA_UPDATE_CHECK=0` disables it.
  - `serve/server.py:1064-1066`: fetches http(s) image URLs.
  - `serve/mcp.py:275-277, 366`: calls user-configured MCP servers.
  - `serve/server.py:3196-3205`: a UDP `connect` to `10.255.255.255` to learn the
    LAN address (no packet is sent).
  - `maya.py` (2,863 lines) installs, downloads from HF (`maya.py:93`, SHA-256
    checked) and builds.
  - `tools/maya_quant/`: GPTQ pipeline (torch, transformers, safetensors).
  - There is no requirements file. The runtime pip list is `setup.py:29`, and
    wheels are bundled in `third_party/wheels`.

### 1.5 Licenses and bundled third-party notices

| Component | upstream | glm53f | maya |
|---|---|---|---|
| Top-level `LICENSE` | MIT, "Niko1221 and the Strata contributors" | identical to upstream | identical to upstream (`LICENSE:3`) |
| ggml | `third_party/ggml/LICENSE`, MIT, ggml authors, `3cf03257…` | same | same (`third_party/ggml`, `3cf03257…`); plus vendored llama.cpp sources in `ref/upstream-llama.cpp/`. There is no LICENSE copy in that directory, but `ref/upstream-llama.cpp/README.md:3-4` gives the provenance (llama.cpp `272aad8b`, 2026-09-30) and cites MIT |
| exllamav3 (EXL3) | — | `third_party/exl3/LICENSE`, MIT, (c) 2025 Turboderp (cited `exl3.cu:1`) | `third_party/exllamav3/LICENSE`, MIT, (c) 2025 Turboderp |
| beellama.cpp | — | — | `third_party/beellama.cpp/LICENSE`, MIT, "The ggml authors, Anbeeld" (basis of the DFlash2 forward) |
| Non-MIT in tree | `sycl/include/dpct/LICENSE.TXT` Apache-2.0 WITH LLVM-exception | same (from upstream) | `serve/web/fonts/OFL.txt` SIL OFL 1.1 (dashboard font); bundled wheels without license files (`[inference]` pygments BSD-2, typing_extensions PSF, rest MIT) |
| **Non-MIT weights** | — | — | **DFlash2 drafter: CC BY-NC-ND 4.0, confirmed.** |

**Flag:** `maya.py:303-306` reads
`DFLASH = {"repo": "Anbeeld/GLM-5.3-Flash-DFlash2-GGUF", "revision": "f3569acb…", … "license": "CC BY-NC-ND 4.0"}`,
with the comment "(the repo's: non-commercial use only …)".
- The same notice appears at `README.md:130, 179-180`, `docs/SETTINGS.md:165`
  and `docs/SETUP.md:57`.
- The drafter is downloaded only when the user picks `--draft dflash2`
  (`maya.py:2664`). The engine loads it with `--dflash <gguf>`.
- The engine code that runs it (`src/core/glm_dflash.cu`, 1,754 LOC) is MIT.
  Only the weights are NC-ND.
- **SP6 must never fetch, bundle, or default to these weights.**

Model weights: GLM-5.3-Flash is MIT per `maya/README.md:177`. Maya's quants
inherit that license (`tools/maya_quant/MODEL_CARD.md:2, 79`). glm53f publishes
`lte-ai/GLM-5.3-Flash-Strata-Q2Q3-Experts` (`docs/GLM_RELEASE_Q2Q3.md`); its
license was not checked here (see §7).

---

## 2. GLM-5.3-Flash support, component by component

**maya's `docs/GLM5-FLASH.md` is a port study.** It was written 2026-09-29
against Strata 0.1.27, before maya's code existed. It has the following parts:
- §1 model config.
- §2 the existing engine.
- §3 gap analysis:
  - 3.2: GDN → KDA.
  - 3.3: QSA → NoPE-MLA + DSA, "the largest piece".
  - 3.4: HC → mHC.
  - 3.5: MoE. Its "hard wall" is that the canonical Q2_0 CPU kernel hardcodes
    Qwen shapes, so GLM needs native packs.
  - 3.6: MTP, vision and tokenizer.
  - 3.7: quantization.
- §4 an mmap study: experts exceed 64 GB at every useful quant, so "retention is
  the whole game".
- §5 performance estimates.
- §6 a phased plan: Python oracle `ref/glm.py`, then kernels, then runner, then
  pack and serve.
- §8 parity: the runner matches an f64 oracle with argmax 512/512, and a
  real-model cross-check against llama.cpp found "no structural divergence".

The study concludes that "About two thirds of the engine's machinery applies
unchanged". It is the best available map from Strata's structure to GLM's.
glm53f has no equivalent single study; its docs are about 40 `docs/GLM*.md`
measurement and design notes.

**Model geometry.**
- maya: `Glm5Geometry` (`include/strata/core/glm_model.hpp:64-80`), filled from
  GGUF `glm5-next.*` metadata. Values: 45 layers, `n_embd` 4096, 64 heads,
  vocab 154,880, q/kv LoRA 1536/512, qk_nope/v 256/256, KDA head 128,
  d_conv 4, indexer 32 heads/128 key/top-k 2048/pool 4, hc 4 with 20 Sinkhorn
  iterations, 288 experts with 8 used, expert FF 2048, 1 shared expert, dense
  FF 12,288 on 3 lead layers.
- glm53f: architecture `glm5next`/`glm5-next` (`include/strata/core/model.hpp:86-90`),
  with geometry parsed from GGUF metadata (:96-150) and an HF `text_config`
  path (:46-63).
- glm53f's decoder requires NoPE, 1 expert group, sigmoid gating, normalized
  weights and 1 shared expert (`src/program/glm_decode.cpp:6219-6223`).
- glm53f's docs: 34 KDA + 11 sparse-MLA layers and 3 dense + 42 MoE layers
  (`docs/README_GLM53_FLASH.md:65`).
- The two agree on the shape.

| Component | glm53f @4894c996 | maya v1.1.8 |
|---|---|---|
| Config / geometry | GGUF metadata + HF `text_config`; validates hc=4, MLA KV heads 0/1, NoPE, 1 group, sigmoid (`model.hpp:96-170`, `glm_decode.cpp:6219-6223`) | GGUF `glm5-next.*` metadata, both `glm5-next` and `glm5next` spellings (`generate.cpp:3726-3730`); `Glm5Geometry` |
| KDA linear attention | CUDA/HIP `src/kernels/cuda/glm.cu:390-426`, chunked prefill `glm_prefill.hpp:65-77`; Vulkan `kda*.comp`; CPU reference `glm_reference.hpp:71`; pin commit makes ordered KDA rows bit-exact on CUDA | CUDA/HIP `src/kernels/cuda/glm_kda.cu`, `include/strata/kernels/glm_kda.hpp`; opt-in CUDA graphs `STRATA_GLM_KDA_GRAPH=1` (`docs/GLM-KDA-GRAPHS.md`, +2.35 % measured) |
| MLA / DSA sparse attention | indexer pool/score/select + MLA `glm.cu:435-560`; prefill `glm_prefill.hpp:83-103`; Vulkan `mla.comp` | `glm_dsa.hpp/.cu` (indexer + absorbed MLA over 512-wide latent), `glm_kv_stream.hpp` (latent KV streaming) |
| Hyper-connections (mHC, Sinkhorn) | `glm.cu:250-372`, fused read `glm_decode.cpp:3083-3140`; Vulkan `mhc.comp` | `glm_hc.hpp/.cu` |
| MoE routing | sigmoid + correction bias, iterative top-k, normalize, scale (`glm.cu:139-208`); group-limited routing rejected | `router_sigmoid.hpp/.cu` (noaux_tc, ×2.5), `glm_ffn` clamped SwiGLU |
| Routed experts placement | GPU caches/tiers; CPU pool (`strata_kernels_cpu`, AVX2/AVX-512 "Q23" Q2_K/Q3_K + canonical); `--decode-experts=gpu\|cpu` | tiered VRAM → pinned RAM → SSD mmap; CPU expert lane `src/kernels/cpu/native_expert.cpp` via ggml-cpu; prefill via ggml MMQ `src/prefill/moe_mmq.cu` |
| MTP / speculation | GLM nextn draft layer, depth 1..7 (`glm_decode.cpp:7269-7376, 9373`); n-gram lookup; **external drafting** = `posix_spawnp` + `socketpair(AF_UNIX)` (`include/strata/spec/external_worker.hpp:77-92`) to a Python worker that drives a loopback llama-server | NextN block (`glm_model.hpp:518-524`, `fast_mtp`; the comment at :66-68 saying MTP is "deferred" is stale); `src/spec/*` suffix drafter/controller; **DFlash2** drafter `src/core/glm_dflash.cu` (weights CC BY-NC-ND) |
| Multi-sequence | `--batch N` (`glm_batch_serve.inc`: `BGEN`/`BT`/`BDONE`) | `--parallel 1-8` (`glm_multi.cu`; `GEN rid=…`, `T <rid> <id>`) |
| Multi-GPU | `--gpu-devices=0,1` (prefill split) | layer split up to 16 GPUs (`--layer-split`, `--gpu-stages`, `--split-device`; `docs/SETTINGS.md:6`); no tensor parallel, no NCCL |
| Cross-host | **`--remote-tp=HOST:PORT` / env `STRATA_GLM_REMOTE_TP`** to `strata-glm-tp-worker` over ucomm (TCP bootstrap, UDP or InfiniBand data) | none; "remote experts" means another local CUDA device (`include/strata/core/remote_experts.hpp:15`) |
| Vision | none (server rejects non-text) | separate process `tools/vision/strata_vision.cpp` (llama.cpp mtmd + glm5next patch) over stdio; engine takes embeddings via `GENI`; HIP is CPU-only |
| Tokenizer | Python only, `tools/strata_tokenizer.py:66, 98` (`glm4`/`chatglm-bpe`); engine takes ids. Ours was transcribed from it. | Python only, `tools/strata_tokenizer.py` (imported `serve/server.py:3366`); engine takes ids |
| Quant formats (engine) | GGUF types in `native_mmvq_supported` (`native_mmvq.cu:2474-2478`): Q4_0, Q5_0/1, Q8_0, Q2_K–Q6_K, IQ2_XXS/XS/S, IQ3_XXS/S, IQ4_NL/XS, IQ1_M, type 42; Unsloth UD-Q2_K_XL base; "Q23" Q2_K/Q3_K expert sidecar packs; REAP-50 / 224-expert prunes; EXL3 dirs (`glm_exl3.hpp`). No FP8 or NVFP4. | GGUF decode kernels IQ1_S/M, IQ2_*, IQ3_*, IQ4_XS, Q2_K, Q3_K (`docs/QUANT-PLAN.md:155-160`); MMQ instances incl. q2_0, q8_0, q2_k–q6_k (`CMakeLists.txt:919`); EXL3 (`exl3.hpp`, `exl3.cu`, CPU ref `src/kernels/cpu/exl3_*.cpp`), "experimental", NVIDIA only. `QUANT-PLAN.md:208-209` "Not started" is stale. Own S/S24/M/L quants via `tools/maya_quant` |
| Sampling | **greedy only**: `INFO … sampling=greedy` (`glm_decode.cpp:9436`); non-`GEN <n> <ids>` lines rejected "only greedy text is supported" (:9518-9523) | per-request keys on `GEN` (temperature, top_p, top_k, min_p, seed, think_budget, logit_bias, …; `generate.cpp:2880-2970`) |
| Published performance | 2×V100 + 2×Xeon 6240: ~29 tok/s decode (`README.md`, `docs/GLM53_V100.md`); RTX 5060 Ti + 128 GB: 23.1 tok/s (`docs/GLM_RELEASE_Q2Q3.md`) | per-quant speed table in README (not re-checked here) |
| Tests in the published tree | CPU: `glm_names_test`, `glm_env_test`, `glm_expert_pack_test` (`CMakeLists.txt:323-329`), `q23_test`, `q23_pool_test`, `canon_expert_test`, `flow_pool_test` (:1808-1826), ucomm loopback; ~40 Python `tools/test_glm_*.py`. GPU: `glm_parity`, `glm_prefill_parity`, … (:426-443); model/quality labels (`docs/GLM_TESTS.md`) | `CMakeLists.txt:53`: "The test suite and the micro benchmarks live in tests/ and bench/micro/, which the published source leaves out"; `tests/` tracks 26 C++/CUDA test sources, but `tests/CMakeLists.txt` and `bench/micro/` are unpublished, so the tests are not wired up and `STRATA_BUILD_TESTS` defaults OFF. CI (`.github/workflows/tests.yml`) runs the Python unit tests plus `tools/check_hip_compat.py`. |

---

## 3. Engine ↔ frontend contract

### 3.1 Our current contract (for reference)

- `secure_frontend/src/engine_protocol_client.cpp` and
  `include/secure_frontend/engine_protocol_client.hpp:64-90` implement it.
- **READY:** `READY <context> [fields…]`. Field `stop` enables `STOP`, other
  fields are ignored, and `INFO …` lines before READY are skipped
  (engine_protocol_client.cpp:124-137).
- **Request:** `GEN <max_new> <ids>`.
- **Responses:**
  - `T <id>`.
  - `DONE …`, where field 5 = `nonfinite` fails the request (:226-238).
  - `ERR` / `ERR <msg>`.
  - Anything else fails the protocol (:243).
- **`PP` heartbeat:** accepted only for the `lighttransport_glm53` flavor, in the
  forms `PP <read> <total>` or `PP <read> <total> <ms> <tok/s>` (:208-224).
- **Context rule:** we apply upstream's `n + max_new + 8 <= context`
  (`chat_execution.cpp:60-64`; upstream `src/program/generate.cpp:10172`).
- **Launch:** `engine_arguments.cpp` adds `--serve`.
  - Strata profile: we append `--native <path>` and reject `--native`,
    `--serve`, `--batch`, `--slots`, `--vision`.
  - GLM profile: everything passes through except `--serve` and NUL
    (:29-35, :50).
  - The child environment is `{}` (SP2 research).

### 3.2 glm53f `strata-glm-decode --serve`

- **Invocation is positional, not flags.** Usage is
  `strata-glm-decode --serve <shard> <context> <dense-MiB> <threads> [expert-MiB=0] [prefill-batch=8] [eos-id=-1] [gpu-budget-MiB=12288] [lookup-depth=0] [gpu-decode-experts=0]`
  (`glm_decode.cpp:9337-9342`). More positional slots follow (stream, mtp,
  draft-depth, affinity, devices, caches, graphs, window, adapt, pages, … up to
  argv 29/30 for `--batch N`).
  - There is **no `--max-context`, `--native`, `--pack`, `--gpu` or
    quant-selection flag**.
  - The context is argv[3], and quant follows from the GGUF and the pack.
- **READY:** `INFO engine=glm-experimental sampling=greedy` then
  `READY <ctx> stop prefill-includes-reset` (`glm_decode.cpp:9436-9437`).
  **Compatible with our parser as built** (INFO skipped, extra field ignored).
- **GEN:** only the exact shape `GEN <max_new> <ids>`. The rule is
  `tokens.size() + max_new > ctx`, which is rejected (:9518-9526). Our `+8` rule
  is stricter, so it is safe.
- **Responses:**
  - `PP <done> <total> <ms> <tok/s>` (:9540). This is the 4-field form our
    `lighttransport_glm53` flavor accepts.
  - `T <id>`.
  - `DONE <gen> <prompt> <prefill_ms> <decode_ms> <length|stop|cancel> 0 0 0`
    (:9600-9601).
  - Cancel prints `DONE … cancel 0 0 0` (:9616).
  - `ERR <msg>`. A failed reset prints `ERR …` and calls `_Exit(1)`
    (:9608-9611), so supervision must treat it as a crash.
- **Commands:**
  - `STOP` (:9451-9456).
  - `QUIT` (:9458, :9483).
  - `TUNE …` only if env `STRATA_GLM_ALLOW_TUNE` is set (:9488).
  - With `--batch`: `BGEN`, `BT`, `BDONE`, `BADM`, `YIELDED`
    (`glm_batch_serve.inc:152-188`).
- **Network: yes, inside the engine binary.**
  - Remote TP is enabled by env `STRATA_GLM_REMOTE_TP` (serve mode,
    `glm_decode.cpp:9424-9431`) or `--remote-tp=` (CLI modes).
  - The decoder is ucomm rank 0 (`glm_decode.cpp:7569-7575`), and rank 0 does
    `socket` + `SO_REUSEADDR` + `bind(INADDR_ANY)` + `listen` + `accept4`
    (`tools/ucomm/src/ucomm_boot.c:97-106`). That is a listener on all
    interfaces.
  - The data plane is UDP `sendto` (`ucomm_udp.c:177, 499-517`) or InfiniBand
    via `dlopen("libibverbs")` (`ibvew.c:12`). Expert weights cross the wire
    (`glm_tp_worker.cpp:298`).
  - `[inference]` The bootstrap is unauthenticated.
  - External drafting spawns a local process over `socketpair(AF_UNIX)`
    (`external_worker.hpp:77-92`), and its Python worker makes HTTP calls.
  - No curl, HTTP, Winsock or download code exists in the C/C++ engine.
- **What our GLM argument profile would need (glm53f):**
  - Model the positional argv as typed fields, with shard and context owned by
    the frontend from the manifest, instead of pass-through.
  - Deny `--remote-tp*`, `--speculative=external`, `--draft-worker-config`,
    `--batch`, and the `--check-*`, `--dump-*`, `--profile*`, `--eval-*`,
    `--bench` and `--routing-trace` families.
  - Keep `STRATA_GLM_REMOTE_TP*` and `STRATA_GLM_ALLOW_TUNE` out of the child
    environment. The `{}` env does this today; SP2's typed allowlist must keep
    doing it.
  - `[inference]` A build-time option to drop ucomm from the binary would remove
    the listener entirely, but that requires patching the source.

### 3.3 maya `strata --serve --glm-pack …`

- **Flags, upstream-style:**
  - `--serve` (`generate.cpp:3236`).
  - `--glm-pack <dir>` (:3102).
  - `--native <preset>` (:3187; a preset, not upstream's model path).
  - `--pack`, `--max-context`, `--max-new`.
  - About 120 flags in total. The rest are listed in §2 and the agent notes:
    `--parallel`, `--mtp*`, `--dflash`, `--spec*`, `--kv*`, `--layer-split`,
    `--expert-cache*`, `--expert-profile-save*`, `--prompt-cache*`, `--vision`,
    `--control-vector*`, `--dump-*`, sampling defaults.
  - There is no `--gpu`; maya.py sets `CUDA_VISIBLE_DEVICES`/`HIP_VISIBLE_DEVICES`.
  - Many knobs are env vars (`STRATA_GLM_*`, `docs/SETTINGS.md`). Under our `{}`
    env they all take defaults.
- **READY:** `READY <ctx> stop pause`, or `READY <ctx> stop pause multi=<n>`
  with `--parallel` (`generate.cpp:2749-2750`). This is compatible with our
  parser. The comment at :2747 ("stop" is NOT advertised) is stale.
- **GEN:** `GEN <max_new> [key=value …] <ids>` (:2880-2970). Our key-less form
  is accepted. Unknown commands get `ERR unsupported command …` (:3033).
  The ids list is the last whitespace-separated field (the ":2746" comment "the
  ids are the LAST token" means this). Each GEN carries the full prompt, and
  prefix reuse is internal (`prep_prompt`, :1555-1565). This fits our stateless
  full-prompt `GEN`.
- **Context rule:** the single-sequence GLM path has **no up-front
  `n + max_new + 8` check**. `run_request` (:1690-1707) checks only the token
  range and emptiness. Past the context, the behavior depends on the path:
  - A prompt longer than the context gets `ERR glm prefill: the prompt (…)
    exceeds the context` (`src/kernels/cuda/glm_prefill.cu:2912`;
    `src/core/glm_model.cu:945`).
  - Plain decode fails in `model.forward` past the context
    (`glm_window.cu:1216`), which prints `ERR` **after** some `T` lines have
    already been sent (`generate.cpp:1860-1864`).
  - The MTP and DFlash paths stop quietly with `finish=length`
    (`glm_fast_path.cu:5537`; `glm_dflash.cu:1525`).
  - `+8` appears only in multi mode (:2232, :2265, :2328) and in the legacy
    Qwen loop (:5952).
  - Our stricter frontend rule (§3.1) keeps a request out of all of these cases.
- **Responses:**
  - `T <id>` (:1742), or `T <rid> <id>` in multi mode (:2016).
  - **`PP %s<done> <total> <ms> <tok/s> <reuse>`** (:1580). This is **5
    numeric fields**, and multi mode adds a tag prefix. Our GLM flavor accepts
    only 2 or 4.
  - **`STAT tok_s=… ms_tok=… vram_hit=… …` every 8 tokens** when serving on
    the fast path (`if (o.serve && model.fast() && produced % 8 == 0)`, :1794;
    printed :1799). The cadence is a literal, and no flag or env var turns it
    off. The fast path is the default unless `STRATA_GLM_SLOW` is set
    (`glm_model.cu:2790`). **Our client fails on it**
    (`engine_protocol_client.cpp:243`).
  - **`NOTE <text>` lines.**
    - The loop guard prints them mid-generation (:1756, :1780).
    - Conversation-slot handling prints them too (:1245; `note()` callers
      :1299, :1365, :1382, :1400).
    - Our client fails these at `engine_protocol_client.cpp:243`.
    - `INFO` before READY (:1018) is fine, because our parser skips it (:125).
  - `DONE <produced> <prompt> <prompt_ms> <decode_ms> <finish> <spec_hits> <spec_steps> <reuse> <hits> <lookups>`
    (:1875). Field 5 is `finish`, compatible.
  - `ERR <msg>`, and also `LP`, `VLENT`, `VRECLAIMED` for the commands that
    trigger them.
- **Commands:** `STOP`, `STOP <rid>`, `PAUSE [keep]`, `WRAP`, `LOGP`,
  `GENI` (vision), `VLEND`/`VRECLAIM`, `QUIT`. A reader thread handles them
  mid-request (:1086-1110).
- **Network:** the C++/CUDA/HIP engine sources (excluding `ref/`) have **zero**
  hits for socket, connect, listen, getaddrinfo, curl, http(s)://, WSAStartup or
  Winsock. The `bind(` matches are weight binding, and "download" means a
  device-to-host copy. All of maya's network code is in Python (§1.4) and the
  CMake FetchContent.
- **What our GLM argument profile would need (maya):**
  - A new protocol flavor that accepts `STAT` and `NOTE` (bounded, private,
    discarded) and the 5-field `PP`, and rejects tags/rids.
  - **Conversation slots must be forced off** (or patched out). With our `{}`
    env, maya writes conversation state to disk by default:
    - `slot_max = env_num("STRATA_GLM_SLOTS", 4)` when the fast path is on and
      `STRATA_GLM_NO_REUSE` is unset (`generate.cpp:1174`).
    - The directory is `STRATA_GLM_SLOT_DIR` or `<glm_pack>/slots`
      (:1253-1256, created with `create_directories`).
    - `slot_put` (:1320-1345) writes the model snapshot and DSA caches as
      `slot-*.bin`, plus a `.meta` file holding the **conversation token ids**
      (`meta_write` :1204-1216, called at :1342).
    - It triggers for interleaved conversations of at least 1,024 tokens, up to
      16 GB (:1156-1158, :1175-1176).
    - The directory is emptied at start unless `STRATA_GLM_SLOT_KEEP=1`
      (:1182, :3049-3053).
    - To turn it off, set `STRATA_GLM_SLOTS=0` or `STRATA_GLM_NO_REUSE=1`. The
      SP2 env allowlist must force one of them, or SP6 patches the default.
  - **The loop guard changes model output by default.**
    - `STRATA_GLM_LOOP_THINK=256` and `STRATA_GLM_LOOP_ANSWER=1024`
      (:1726-1727) force the next token when a loop is detected (:1750, :1771).
    - It is announced by `NOTE` lines.
    - Keep it or disable it: this is an owner or spec decision.
  - Keep `STRATA_GLM_DFLASH` out of the child environment. It is a flag-free way
    to turn DFlash on (`glm_model.cu:1323`); `--dflash` itself sets it.
  - **`_Exit(3)` watchdog.** `start_serve_watchdog(180)` (:2744) calls
    `std::_Exit(3)` after 180 s without progress (:726-747). Supervision must
    treat this as a crash, the same as glm53f's `_Exit(1)`.
  - Owned `--serve`, `--glm-pack` (manifest path) and `--max-context` (manifest
    or config).
  - Deny `--parallel` > 1 (rid protocol), `--vision` (GENI), and `--dflash`
    (NC-ND weights).
  - Deny file writers: `--expert-profile-save*`, `--prompt-cache-root`,
    `--dump-*`, `--tokens-file`. Deny `--control-vector*` unless manifest-owned.
  - Pass through tuning flags as typed bounded values (layer split, expert-cache
    sizes, kv mode, MTP).
  - The SP2 env allowlist is a **safety control** for maya (slots, DFlash, loop
    guard), not only a performance one. SP2 owns that list.
    - The engine reads 227 distinct `STRATA_*` env variables.
    - Several flags work by setting env vars internally (`--dflash` sets
      `STRATA_GLM_DFLASH`; `--prefill` works the same way). Any env var we let
      through can therefore switch on behavior that a flag would.
- **No hard blocker was found in maya's engine.** It needs no Python at runtime
  and spawns no processes. Its only dynamic load is `LoadLibraryA` of dbghelp
  (`generate.cpp:702`). The engine code is all MIT, and the CUDA floor is sm_75.

---

## 4. Portability for our targets

Targets: the owner's NVIDIA test box on Linux and Windows. The dev host has no
GPU and no nvcc.

| | glm53f | maya |
|---|---|---|
| CUDA floor | sm_75 (sm_70 allowed for GLM); sm_120 warns below CUDA 13.0; V100 docs use CUDA 12.9 | sm_75 in CMake (sm_70 via `native`); CUDA 12.x; Blackwell needs 12.8+; CUDA 13 for RTX 20+ (`README.md:81, 139`) |
| Linux NVIDIA | primary, measured (V100, RTX 5060 Ti) | primary, measured |
| Windows NVIDIA | **not buildable as shipped**; needs source patches: a ucomm Windows stub or port, `setenv` → `_putenv_s`, `__builtin_popcount` → `std::popcount` (§1.3). No GLM Windows docs. | experimental, documented: MSVC v143 as nvcc host, CUDA 12.8/13.x, Ninja; one user report on Windows 11 |
| MSVC as nvcc host | would be required (as upstream) once the Linux-only parts were guarded | already the path (`maya.py:1107-1119`) |
| HIP/ROCm | gfx1201 measured under ROCm (Linux) | Linux ROCm 7, text only |
| Smoke-testable without a GPU | CPU-only targets with `STRATA_NATIVE_EXPERTS=ON`: `glm_names_test`, `glm_env_test`, `glm_expert_pack_test` (C11, no model), `q23_test`, `q23_pool_test`, `canon_expert_test`, `flow_pool_test`, `strata-glm-tp-worker`, ucomm loopback; Python tokenizer tests | CPU-only targets: `strata-gguf`, `strata-dequant`, `strata-plan`, `strata_kernels_cpu`, `strata_spec`; 26 tracked test sources in `tests/`, but their CMake wiring is unpublished (`[inference]` a local wiring file is needed to run them); Python unit tests (CI runs these) |
| Offline build | needs `STRATA_GGML_DIR` pointing at a local llama.cpp `3cf03257…` checkout, else FetchContent | same, plus a git-applied patch at configure |

`[inference]` On the dev host, either source can produce CPU compile and
unit-test evidence plus tokenizer evidence only. Neither can run a GLM forward
without a GPU. glm53f's published CPU tests cover more GLM-specific code
(names, env, expert pack, Q23 kernels) than maya's published tree does.

---

## 5. Tracking strategy

**Option A: pinned submodule in its own directory**, for example
`engines/glm/<source>/` as `upstream/` is. The spec
(`docs/superpowers/specs/2026-10-13-product-experience-and-upstream-tracking-design.md`,
"GLM engine" bullet) leaves subtree vs submodule to SP6. A submodule keeps the
never-edited rule and makes a bump a gitlink change. It cannot carry local
patches. glm53f's Windows break and ucomm listener, and maya's missing test
wiring, would need either an owner-approved fork or wrappers outside the
submodule.

**Option B: subtree import** under `engines/glm/` with a PROVENANCE section of
local changes. This allows patches and makes bumps merge work.

| | glm53f | maya |
|---|---|---|
| Pin unit | commit SHA on a moving branch (no GLM tags; tags stop at `v0.1.29`) | release tags (44 tags `v1.0.0`–`v1.1.8` in 8 days) |
| Churn | GLM-specific: Syoyo 12 / 47 / 10 commits in W40 / W41 / W42 (to Oct 12); branch total including upstream merges 1,295 / 853 in W40 / W41; +2 commits since pin | 331 commits 2026-10-07 → 10-14 (27–97 per day), 24 authors, mw00 210 |
| Relation to `upstream/` | shares history; merges upstream `main` periodically (0.1.42 now). `[inference]` a diff against our pinned upstream is computable, so a reviewer can isolate the GLM delta | no shared history, based on 0.1.27; upstream fixes do not flow in automatically; the delta against upstream is not computable by git |
| Fork-friendliness | one long-lived branch on a fork of a fork, mixing upstream merges and GLM commits; a bump drags in upstream changes too | single linear repo, small tree, frequent tags; but tests and benches are deliberately unpublished |
| A "bump" | fetch branch → new SHA → re-read `glm_decode.cpp` serve/argv section and `ucomm` linkage → update arg/protocol tables → run CPU tests + GPU kit on owner box → PROVENANCE | fetch tag → re-read `generate.cpp` serve section, flag list, `STRATA_GLM_*` env list → update flavor/pass-through table → build + GPU kit → PROVENANCE |

`[inference]` At either source's current pace, SP6 should pin and bump
deliberately (for example per reviewed release), not follow `HEAD`.

---

## 6. Recommendation

**Lean towards maya's engine sources as the SP6 base, without committing yet.**
That means the `strata` binary's `--glm-pack` path, C++/CUDA only (not
`serve/`, `maya.py` or the quant tooling), pinned to a release tag. Keep glm53f
as the second reference: its parity and CPU tests, its Q23 packs and its
upstream-merged lineage serve as a cross-check.

This is a reasoned judgment, not a measured result; neither engine was built or
run. The fact-check of 2026-10-14 weakened the case without reversing it: every
item below is an integration cost, not a blocker. Because of that, **"evaluate
both on the owner's GPU box before choosing" is a close, equally valid
alternative** (owner decision 1).

Reasons, with their caveats:

1. **Windows.** The owner targets Windows and Linux.
   - maya documents and drives an MSVC-hosted CUDA build on Windows. That build
     is marked experimental, with one user report.
   - glm53f's `strata-glm-decode` does not build for Windows as shipped.
   - *Caveat:* the glm53f fix is a moderate source patch, not a rewrite (§1.3):
     a ucomm stub or port, `setenv` → `_putenv_s`, and `__builtin_popcount` →
     `std::popcount`. It would mean carrying local patches.
2. **No network code in the engine binary.**
   - maya's C++/CUDA has no sockets, HTTP or process spawning; its only dynamic
     load is dbghelp.
   - glm53f's decode binary contains an unauthenticated remote-TP listener
     bound to `INADDR_ANY`. It is switched on by `STRATA_GLM_REMOTE_TP` or
     `--remote-tp=` (`glm_decode.cpp:9424-9431, 9704-9707`;
     `ucomm_boot.c:98-116`).
   - glm53f's listener can be kept off by policy (argument denial plus the env
     allowlist), or removed with the same ucomm stub that the Windows fix needs.
3. **Contract closeness, with three deltas.**
   - maya keeps upstream's flag-style CLI (`--serve`, `--max-context`, `--pack`)
     and its line protocol. A GEN carries the full prompt, which fits our
     stateless `GEN`, and sampling is set per request. Our typed pass-through
     table maps onto it.
   - *Caveat:* maya still needs a new protocol flavor for three deltas: `STAT`
     every 8 tokens with no off switch, the 5-field `PP`, and `NOTE` lines.
     Each of them fails our client today.
   - glm53f's positional argv and greedy-only sampling need a bespoke launcher
     model.
   - *Both* engines can end the process with `_Exit`: glm53f with `_Exit(1)` on
     a failed reset, maya with `_Exit(3)` from a 180 s watchdog. SP2
     supervision must handle this for either base.
4. **A written port study.** `docs/GLM5-FLASH.md` explains each component's
   mapping from Strata, which helps our reviewers audit the port.
5. **Feature coverage.** Sampling, MTP, multi-GPU layer split and EXL3 are
   present. Vision and DFlash2 exist but should stay off.

**Defaults SP6 must override if maya is the base** (§3.3):

- **Conversation state on disk.** With our `{}` env, maya keeps up to 4
  conversation slots and writes them to disk. Each slot is model snapshots and
  DSA caches (`slot-*.bin`) plus a `.meta` file holding the conversation token
  ids, under `<glm_pack>/slots`, up to 16 GB. They are written for interleaved
  conversations of at least 1,024 tokens. That is user content persisted
  without being asked for. SP2 must force `STRATA_GLM_SLOTS=0` or
  `STRATA_GLM_NO_REUSE=1`, or SP6 patches the default.
- **Output-altering loop guard.** It is on by default
  (`STRATA_GLM_LOOP_THINK=256`, `STRATA_GLM_LOOP_ANSWER=1024`), forces tokens
  when it detects a loop, and announces itself with `NOTE`.
- **Env surface.** The engine reads 227 distinct `STRATA_*` variables, and
  `STRATA_GLM_DFLASH` turns on the NC-ND drafter path without a flag. For maya,
  the SP2 env allowlist is a safety control, not only a performance one.

Top risks:

- **Maturity and churn.** maya is one week old with a fresh history and about
  44 tags.
- **Tests not wired up.** `tests/` tracks 26 C++/CUDA test sources, but their
  CMake wiring and `bench/micro/` are unpublished. CI is Python-only, and
  several docs contradict the code (stale MTP and EXL3 notes, and a sm_70 claim
  against a sm_75 guard). We would need our own GPU validation kit (SP7).
- **Disk persistence.** maya persists conversation-derived KV state and token
  ids to disk by default (above).
- **Changed output.** maya's default loop guard alters model output.
- **Protocol deltas.** `STAT`, the 5-field `PP` and `NOTE` need a new protocol
  flavor. Past the context, maya can print `ERR` after some `T` lines have
  already been sent, or stop quietly with `length`; our stricter `+8` rule
  avoids both.
- **Upstream drift.** maya's base is 0.1.27. Upstream fixes since then do not
  flow in; glm53f's do.
- **Our existing GLM work targets glm53f.** The `lighttransport_glm53` profile,
  flavor, help text and the `strata-glm-decode` name are glm53f-specific. A
  maya base means a second flavor or a renamed one.

**glm53f as the base is defensible** if the owner accepts carrying local source
patches. One ucomm stub would close both the Windows break and the network
listener. Then greedy-only sampling and the positional CLI are the remaining
costs.

**What would flip the lean:** either GPU-box results that favor glm53f, or an
owner decision that local patches to the engine source are acceptable. The
second makes glm53f's two main gaps cheap to close.

### Owner decisions (asked 2026-10-14)

1. **Which base:** maya, glm53f, or **evaluate both engines on the GPU box
   before choosing the base**. Given the fact-check, the third option is a
   strong default.
2. Is Windows GLM in SP6 scope? If yes, glm53f needs a moderate source patch
   (ucomm stub, `_putenv_s`, `std::popcount`).
3. Accept MSVC (VS 2022 v143) as the nvcc host for the Windows GLM build. Both
   sources need it.
4. Is AMD/HIP in scope for SP6, or NVIDIA only?
5. Which quant formats SP6 supports: GGUF IQ/K types only, or also EXL3 (NVIDIA
   only, experimental) and maya's own S/M/L quants or glm53f's Q23 packs.
6. Confirm that maya's `serve/`, `maya.py`, dashboard, updater, MCP client and
   quant tooling are out of scope, because our frontend owns the listener and
   acquisition.
7. Confirm that DFlash2 (CC BY-NC-ND weights) is never supported, fetched or
   documented as an option, including through `STRATA_GLM_DFLASH`.
8. Is multi-GPU (maya's layer split, glm53f's `--gpu-devices`) in scope? Is
   remote TP (glm53f) permanently out?
9. Submodule (no local patches; fork if needed) or subtree with recorded local
   changes. If local patches are acceptable, glm53f's main gaps become cheap to
   close.
10. **Which `STRATA_GLM_*` env knobs SP2's allowlist forces or excludes for
    maya:** slots off (`STRATA_GLM_SLOTS=0` or `STRATA_GLM_NO_REUSE=1`),
    `STRATA_GLM_DFLASH` excluded, and the loop guard kept or disabled. Also
    whether any tuning knobs may pass at all.
11. Is maya's default loop guard (forced tokens on detected loops) acceptable
    behavior for our chat route, or must it be off?

### Decisions taken (2026-10-14)

A1. Base engine: EVALUATE BOTH (project-maya v1.1.8 @5c970a86 and lighttransport glm53f @4894c996) on the owner's NVIDIA box before choosing; lean Maya.
A2. Windows GLM is in SP6 scope.
A3. MSVC v143 (VS 2022) accepted as the nvcc host compiler for the Windows GLM build.
A4. AMD/HIP out of scope for SP6 v1 (NVIDIA only).
A5. Quant formats: GGUF IQ/K types + Maya's own S/M/L quants; EXL3 later.
A6. Maya's serve/, maya.py, dashboard, updater, MCP client and quant tooling are out of scope.
A7. DFlash2 (CC BY-NC-ND weights) is never supported, fetched or documented, including via STRATA_GLM_DFLASH.
A8. Single GPU first; multi-GPU layer split later; glm53f remote TP permanently out.
A9. Tracking: git submodule, no local patches (fork on GitHub if a patch is unavoidable).
A10. SP2 env allowlist for Maya: slots off (STRATA_GLM_SLOTS=0 or STRATA_GLM_NO_REUSE=1), STRATA_GLM_DFLASH excluded, no tuning knobs pass.
A11. Maya's default loop guard is OFF for our chat route.

---

## 7. Open questions not settled from the sources

- Whether maya's CMake configures with `STRATA_BUILD_TESTS=ON` without the
  unpublished `tests/CMakeLists.txt` (`CMakeLists.txt:1038`), and how much
  wiring the 26 tracked test sources need.
- The license of glm53f's published weights
  (`lte-ai/GLM-5.3-Flash-Strata-Q2Q3-Experts`), and of maya's HF quants beyond
  the model card's "MIT" claim. Neither was queried.
- The licenses of maya's bundled Python wheels (no license files in the tree).
- Real performance and correctness on the owner's NVIDIA box for either engine.
  Nothing was built.
- Whether either maintainer would accept upstream PRs for Windows guards, a
  no-network build option, quieter protocol output (`STAT`/`NOTE`), or
  slots-off defaults.

Answered by the 2026-10-14 fact-check (kept here for traceability):

- maya's single-sequence context bound: there is no up-front `+8` check (§3.3).
- maya's "ids are the LAST token" means the ids list is the last field of the
  `GEN` line. The full prompt is sent, and reuse is internal (§3.3).
- maya's `STAT` cannot be disabled; its cadence is the literal
  `produced % 8 == 0` (§3.3).
- `ref/upstream-llama.cpp/` provenance: llama.cpp `272aad8b`, MIT, per its
  `README.md:3-4` (§1.5).
- glm53f's two post-pin commits add only opt-in `STRATA_GLM_*` knobs and do not
  change the serve contract.
