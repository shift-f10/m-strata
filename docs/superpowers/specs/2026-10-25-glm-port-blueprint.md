# GLM port from Maya — build blueprint (2026-10-25)

Owner decision: GLM joins the single engine as a **self-contained family DLL** (`strata_glm`) beside the one
`strata.exe`. It statically bundles Maya's proven GLM engine + its divergent deps with hidden symbols and
exports ONE entry; qwen stays statically linked (it shares our Strata core). Standing rule: reuse Maya's
upstream source verbatim first; test/tune/lightweight only after it runs.

## Why a DLL (evidence)
Every shared header the GLM path touches diverged across the Strata↔Maya fork — `gguf_reader` +433,
`remote_experts` +140, `moe_mmq` +126, `iq_kernels` +117, `sampler` +69 lines — and our core `geometry.hpp`
does not exist in Maya at all (Maya uses its own `Glm5Geometry`). GLM's code is written against Maya's
divergent `strata::` core, so it cannot static-link into one exe beside our Strata core without ODR clashes.
A DLL gives each family its own symbol table: the GLM DLL links its own copies, exports only a C entry.

## The vendored engine — 115 Maya files (transitive quote-include closure, fixpoint, 0 unresolved)
Computed by `scratchpad/glm_closure.py`; manifest in `scratchpad/glm_vendor_manifest.txt`. Seeds: the 21
`glm_*` source/header files. The closure STOPS at a clean boundary: it pulls `Glm5Model` + the `glm_*`
kernels + Maya's divergent shared infra it needs (`gguf_reader`, `expert_source`/`expert_cache`, `graph`,
`pinned`, `platform/memory`, `exl3`, `iq_kernels`, `qsa`, `sampler`, `router_sigmoid`, `native_mmvq`,
`kv_stream`, the cpu lane, `prefill/{gemm,moe_mmq,moe_fused_rdna4}`) — but **not** Maya's generic
`session`/`weights`/`device` (GLM has its own `glm_memory`/`glm_fast_state`/`glm_layer_graphs`). 4 includes
resolve to Maya's **ggml-cuda internals** (`common.cuh`, `convert.cuh`, `mmq.cuh`, `quantize.cuh`): the DLL
builds against **Maya's** vendored `third_party/llama.cpp`, not Strata's (ggml may differ across the fork).

Vendor location: `engine/third_party/project-maya/{include,src}/...` (preserve Maya's layout; the dir already
holds Maya's LICENSE/NOTICE — MIT). This is the DLL's include root, placed FIRST so Maya's headers win.

## The seam — staged (minimal runner first, full driver later)
Maya drives GLM from `glm_pack_generate()` (generate.cpp:1008–4233, ~3225 lines: packs, serve wire protocol,
MTP, multi-GPU split, slots, checkpoints) — entangled with the 8555-line multi-family driver's `Options` and
helpers. Too much to lift cleanly up front.

- **Phase G1 (get it running + testable):** a small vendored `glm_entry.cpp` exposing
  `extern "C" int glm_main(int argc, char** argv)` that drives `Glm5Model` directly — modeled on Maya's
  `glm_model_test.cpp` (221 lines): parse model/pack + max_ctx + prompt tokens + max_new + sampler,
  `model.load(...)`, prefill, decode loop, emit tokens. Proves the single-exe GLM path and enables the
  parity oracle. No production driver surgery.
- **Phase G2 (production):** port `glm_pack_generate` (serve, packs, MTP, multi-GPU, slots) onto the same
  engine for feature/speed parity with Maya.

## The one exe (`strata`)
- Registry (`include/strata/artifact/model_family.hpp`): add `ModelFamily::Glm5Next`, arch `glm5-next`.
  **RED test first** in `tests/core/model_family_test.cpp` (glm5-next → Glm5Next; title/arch round-trip).
  `model_family_runs` true only once the DLL is wired; honest `model_family_not_supported` until then.
- Family switch (`src/program/generate.cpp`, guarded `STRATA_HAVE_GLM`): on a `glm5-next` `--native` arch
  (or a `--glm-pack`/`--glm` flag), **delay-load `strata_glm.dll`** and `return glm_main(argc, argv)`.
- `strata --families` lists `glm5-next`.

## CMake
`add_library(strata_glm SHARED <115 vendored sources>)` — own include root first
(`third_party/project-maya/include`), Maya's ggml include, `CMAKE_CUDA_ARCHITECTURES=89`, hidden default
visibility + the single exported entry, `Threads`. Option `STRATA_BUILD_GLM` (ON for our box build). The exe
delay-loads it (Windows `/DELAYLOAD:strata_glm.dll`, as it already does for the HIP DLLs).

## Verify, then tune
Run the GLM GGUF/pack (see memory `model-locations`) through `strata.exe` (CLI one-shot, Phase G1), parity-
check logits per-layer against Maya's GLM as the oracle (memory `engine-parity-oracle`). Windows build claim
only from the `windows-msys2-clang` CI job; GPU/speed claims only from the owner's box. Then Phase G2 + the
lightweight consolidation.

## Build order (lanes)
1. Vendor the 115-file subtree into `third_party/project-maya/` (deterministic, from the manifest).
2. Registry entry + RED test (tiny, TDD, host-only).
3. CMake `strata_glm` SHARED target; get it to compile under `-Werror` (local nvcc 13.0 compile-check).
4. `glm_entry.cpp` (G1 runner) + export; wire the family switch + delay-load; `strata --families`.
5. Verify (parity oracle) → G2 driver → tune → lightweight.
