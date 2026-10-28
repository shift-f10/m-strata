# Unified engine — implementation plan

Plan for [`../specs/2026-10-23-unified-engine-design.md`](../specs/2026-10-23-unified-engine-design.md)
(read it first; owner decisions resolved 2026-10-23). Thesis: **one lightweight, non-bloat,
resource-mindful engine that runs the selected model, from one verified source** — a shared substrate
written once + per-family graph modules **ported from upstream/Maya/our gemma4 as references and
reviewed in**, in our tree `engines/unified/`.

## What is verifiable where (the hard boundary)
- **CI / dev host (agents): host-only C++ + the frontend.** The engine's host code (GGUF reader,
  geometry readers, the serve protocol, dispatch, CLI, session bookkeeping) compiles and runs
  hermetic tests on `linux-gcc` and `windows-llvm-mingw-cross` (wine) with the CUDA paths behind
  `#if STRATA_U_CUDA` / stubbed. The frontend changes build + test under MSVC. **No GPU is used.**
- **Owner's GPU box: the CUDA kernels + every runtime/perf/parity claim.** The engine's CMake is a
  root option `SECURE_STRATA_BUILD_UNIFIED_ENGINE` (OFF, never in CI); the box builds it (CUDA 13,
  sm_75+) and runs the parity + generation battery. Nothing is "runnable"/"verified" until the box
  does this (DoD, design §1.3). Agents write the CUDA ports (reviewed, from references, as gemma4
  was); the box compiles + parities them.

So the agents drive everything to a **box-buildable, CI-green tree**; the owner builds + runs it.

## Layout (`engines/unified/`, ours — reviewed like the frontend, not a submodule)
```
engines/unified/
  src/u_main.cpp            CLI -> dispatch by general.architecture -> serve loop
  src/u_dispatch.{hpp,cpp}  arch key -> family graph module (static table, guarded)
  src/u_gguf.{hpp,cpp}      one bounded GGUF reader (from gemma4's g4_gguf as reference)
  src/u_geometry.{hpp,cpp}  per-family geometry structs parsed from <arch>.* keys
  src/u_protocol.{hpp,cpp}  the converged "unified" wire dialect (host; the superset grammar)
  src/u_session.{hpp,cpp}   session/KV bookkeeping, sampling glue (host side)
  src/u_experts.{hpp,cu}    the shared expert-tiering + expert-hit-profile facility (§3.5)
  src/kernels/              shared generic kernels (matmul/router/rope/norms) + per-family graph .cu
  src/family/glm.cu ds4.cu raon1.cu qwen35moe.cu qwen4exp.cu gemma4.cu   (ported graphs)
  tests/                    hermetic host tests (CI) + GPU parity tests (label gpu, box)
  tools/                    parity harnesses (per-family, vs the reference engine)
cmake/unified-engine/       standalone wrapper project (SP6 §3.3 / gemma4 pattern), OFF, never CI
docs/validation/unified-engine/   box parity + acceptance results
```

## Plan 0 — foundation (CI-green, no GPU; agents)
- **T0.1 Tree + CMake wrapper.** `engines/unified/` + `cmake/unified-engine/` standalone project;
  root option `SECURE_STRATA_BUILD_UNIFIED_ENGINE` (OFF). Host libs build without CUDA (CUDA behind
  `STRATA_U_CUDA`). Hermetic test target on CI.
- **T0.2 GGUF reader + geometry.** Port gemma4's bounded `g4_gguf` as `u_gguf`; `u_geometry` reads
  `general.architecture` + the per-family `<arch>.*` keys into typed structs (glm5next, ds4, raon1,
  qwen35moe, qwen4exp, gemma4) with strict guards. Hermetic tests from committed GGUF-header fixtures.
- **T0.3 Converged protocol (host).** `u_protocol`: the superset dialect (READY/GEN/T/DONE +
  PP/STAT/NOTE/RESUME as a strict union), fail-closed. Golden host tests of the grammar.
- **T0.4 Dispatch.** `u_dispatch`: `general.architecture` -> family module via a fixed, guarded table
  (trusts only the GGUF arch key). Host test: each arch maps to its module; unknown fails closed.
- **T0.5 Frontend (MSVC, CI).** `EngineProtocolFlavor::unified` + `EngineArgumentsProfile::unified`
  (strict superset grammar in `engine_protocol_client.cpp`); add `EngineProfile::deepseek` and
  `::raon1` across **every** exhaustive switch (`subsystem_limits.cpp`, `model_options.cpp`,
  `acquire_manifest.hpp`, `engine_gate.cpp`) + prompt renderers + tokenizer modes (raon1 vocab);
  `protocol_flavor_for` returns `unified` for the merged-engine profiles. RED→GREEN api_router/page
  tests. Keep the "startup-selected, never inferred" invariant.
- **T0.6 Session/CLI host side.** `u_main` arg parse (owned flags only), `u_session` bookkeeping, the
  serve loop skeleton calling a `FamilyGraph` interface whose CUDA impls are stubbed off-GPU.

## Plan 1 — the Maya-lineage graphs (box-verified; agents write, owner parities)
- **T1.1 Shared generic kernels.** Port Maya's `strata_kernels` generics (native MMVQ/MMQ, router
  top-k + sigmoid, RoPE, norms, sampler) into `src/kernels/`, reviewed; the one set all families call.
- **T1.2 Expert facility (§3.5).** `u_experts`: VRAM/RAM tiering + the expert-hit profile
  (load/warm-start hottest-first + `--expert-profile-save`) + live rebalance. Shared by all families.
- **T1.3 GLM graph** (`family/glm.cu`): port from `engines/maya-ref/src/core/glm_model.cu` — KDA
  linear attention, DSA/MLA, sigmoid-routed MoE, NextN/DFlash — onto the shared kernels + `u_experts`.
- **T1.4 DeepSeek graph** (`ds4.cu`): port from `src/ds4/ds4_model.cu` — MLA + index + DSpark.
- **T1.5 Raon-1-mini graph** (`raon1.cu`): new but GLM-shaped — RDA linear (reuse KDA), Gated MLA
  (QK192/V128, LoRA 576/512, NoPE), sigmoid MoE (128, 6+1), DFlash. Geometry from `raon1.*` keys.
- **T1.6 Box parity.** Per family: logits/KL vs its reference engine + a short generation battery on
  the owner's box; results under `docs/validation/unified-engine/`. Only then is that family "runs".

## Plans 2–3 (later): qwen35moe/qwen4exp (GDN onto the KDA path; iq_pack reconcile), then gemma4
(dense + sliding-window + soft-cap, re-expressed from `engines/gemma4/` as reference).

## Testing & DoD
Hermetic host + frontend tests green on CI per task (RED→GREEN). Per-family box parity + the frontend
API/page acceptance (design §1.3) gate "verified". Lean: port only what we serve (design §1.2); no
live multi-upstream tracking — references are pulled + reviewed deliberately.
