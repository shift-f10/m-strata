# Unified engine — building and bringing it up on the GPU box

This is the box-side guide for `engines/unified/` (the merged engine: qwen35moe + qwen4exp + gemma4 +
glm53 + deepseek + raon1 in one binary). Agents wrote the host foundation + the CUDA graph ports and
kept the host build CI-green; **the CUDA compiles and runs only here, on the NVIDIA box** (CLAUDE.md:
GPU claims only from the owner's box). Design:
[`../../docs/superpowers/specs/2026-10-23-unified-engine-design.md`](../../docs/superpowers/specs/2026-10-23-unified-engine-design.md);
plan: [`../../docs/superpowers/plans/2026-10-23-unified-engine-plan.md`](../../docs/superpowers/plans/2026-10-23-unified-engine-plan.md).

## What is written vs what the box must finish
- **Written + host-verified (CI, no GPU):** the host foundation (GGUF reader, geometry guards,
  converged `unified` wire dialect, arch→family dispatch, CLI + serve skeleton), and the frontend
  contract (`EngineProtocolFlavor::unified`, `EngineProfile::deepseek`/`raon1`). Host build 7/7.
- **Written, box-to-compile:** `src/kernels/common.*` (shared kernels), `src/u_experts.*`
  (ExpertTier), and the six `src/family/<fam>_graph.*` + `src/kernels/<fam>_*` graph ports. Faithful
  to the references, with `TODO(box)` where a deep kernel or a shared seam needs finishing. **Not yet
  compiled or parity-checked.**

## Build (box)
Requirements: CUDA 13, an NVIDIA GPU sm_75+ (cuBLAS), MSVC v143 on Windows, the pinned llama.cpp ggml
checkout (tag `3cf03257`, as Maya/gemma4 use) for the i-quant/MMQ work in the TODOs below.

```
cmake -S cmake/unified-engine -B out/unified-engine -DSTRATA_U_CUDA=ON ^
      -DCMAKE_CUDA_ARCHITECTURES=89   (your card's arch; 75;80;86;89;90 supported)
cmake --build out/unified-engine --config Release
```
`cmake/unified-engine` is a standalone wrapper (OFF by default / never CI); it sets `STRATA_U_CUDA=ON`
and globs `src/kernels/*.cu` + `src/family/*.cu`. The host build (`-DSTRATA_U_CUDA=OFF`) stays green
and compiles none of the CUDA.

## First fixes to link + run (the reconciliation seams every lane flagged; grep `TODO(box)`)
1. **`GgufFile` factory seam.** `make_<fam>_graph(const Geometry&, uint32_t, string&)` has no file;
   each graph's `load(gguf, …)` is written and waits for it. Thread the parsed `GgufFile` (and, for
   qwen4exp, the `--pack`/`--ple-gguf` paths) through `u::make_family_graph`'s `#if STRATA_U_CUDA`
   dispatch into each factory. `load()` fails closed until then.
2. **ExpertTier API.** The landed `u::ExpertTier` is `open(Config)` + `set_host_arena(base,bytes)` +
   `compute(layer, host-ids, weights, x, out, …)`. The glm/ds4/raon1 graphs were written against an
   assumed `register_layer`/`run_moe(device-ids)` shape — rename to `compute`, feed experts via the
   one host arena + blob offsets, copy the k routed ids to host. Extend `Config` to **per-layer**
   blob size/quant (qwen4exp's pack varies per layer).
3. **Per-layer attention schedule.** `u::Geometry` carries only scalars; GLM/Raon/qwen hybrids need
   the per-layer `attention.head_count_kv` array (0 ⇒ the linear/recurrent layer) to drive `is_recr`.
   Surface it in `u_geometry` and overwrite each graph's placeholder before any parity claim.
4. **i-quant widening (qwen4exp only).** Flash-Next's iq_pack experts are `IQ1_M/IQ2_XS/IQ3_S/…`,
   which `common.cuh`'s `k::Quant` (Q8_0/Q4_K/Q5_K/Q6_K/BF16/F32) does not carry. Port the i-quant
   MMVQ + grouped-MoE kernels (Maya `iq_kernels.cu`, `s2_expert_grouped.cu`, `prefill/moe_mmq.cu`)
   and widen `k::Quant`, or serve a re-quantized pack. Decode-only families don't need this.
5. **Deep per-family kernels** (each on a dense/simple baseline already in place): QSA sparse-select
   (qwen4exp), DSpark verify (ds4), DFlash/NextN verify (glm/raon), GDN state tiling, the MMQ prefill
   fast path. All are `TODO(box)` with reference `file:line`.

## Bring-up order + parity (DoD)
Per design §1.3, a family is "verified" only after a box parity gate + a short generation battery;
commit results under `docs/validation/unified-engine/`. Recommended order (owner's priority): **GLM →
DeepSeek → Raon** (Plan 1), then **qwen35moe → qwen4exp** (Plan 2), then **gemma4** (Plan 3). Each
graph lane left a parity ladder in-file (e.g. run attention `sel=all` dense → sparse; single-warp GDN
→ tiled; i-quant experts last). The engine links once all six family factories exist (they do).

## Running it (after a family parities)
Point a launcher card's `engine.executable` at the built `strata-unified-engine` and set the model's
profile; the frontend already speaks `unified` and selects the family by the model's manifest/GGUF
arch (startup-selected, never inferred). Or run standalone: `strata-unified-engine --native <gguf>
--serve --gpu 0 --max-context <n>` and drive it with the `unified` wire dialect.
