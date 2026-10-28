# Strata engine — architecture map, and what our engine reuses (2026-10-25)

Owner directive: study how the Strata engine (`C:\Strata`) works, map it, and build our engine from it. Project
Maya (`C:\project-maya`) is a fork of Strata — reuse the similarities, cut code. `C:\Strata` and
`C:\project-maya` are the real, working engines (the GLM + qwen configs the owner runs live there). This map is
read from that source, not inferred. Depth is marked: [studied] = read the code; [inventory] = known by
file/role, not yet read line by line (next passes).

## 0. The fork relationship (the consolidation opportunity) [studied]

`C:\Strata\src` and `C:\project-maya\src` have the SAME directory layout except the model-specific dirs:

| dir | role | Strata | Maya |
|---|---|---|---|
| core, kernels, prefill, spec, program, ngram, plan, platform, artifact | the engine | yes | yes (shared base) |
| dsv41 / ds4 | DeepSeek family | `dsv41` | `ds4` (renamed in the fork) |
| qwen35 | qwen tools | yes | — (dropped) |

Git lineage diverged (Strata 0.2.4.2, Maya v1.1.27), no shared live branch, but the codebase is one lineage.
**So ~90% is a shared base; the per-model code is the thin differing layer.** Our engine should BE that shared
base (reused, not reimplemented) plus a thin per-model layer. The "lean cut" (engines/unified) re-wrote the
shared base from scratch — that is the waste to undo.

## 1. How a model is run (the recipe) [studied — strata-iq3_s.json]

`serve/server.py` (a thin HTTP server) launches `engine\strata.exe` (built from `src/program/generate.cpp`) with:
- `--pack C:\Strata-data\packs\iq3_s` — Strata's repacked expert/tokenizer bundle (the GPU-resident + streamed
  expert store; tokenizer lives here too).
- `--native <shard1>.gguf --ple-gguf <shard2>.gguf` — the GGUF weights; PLE (per-layer embedding) in shard 2.
- `--expert-profile data/expert-profile.bin --expert-cache auto` — warm the VRAM residency set + size it.
- `--spec 4 --mtp C:\Strata-data\mtp\rt --spec-min-p 0.70` — speculative decode: 4-token verify windows, the
  MTP drafter, accept while prob >= 0.70.
- `--pcie-frac 0.35` — of a layer's MISSED experts, 35% read by the GPU over PCIe, the rest computed on the CPU
  expert lane (the `ExpertPool`). THE CPU/GPU split.
- `--adapt-every 1 --adapt-swaps 160 --adapt-decay 0.97` — the adaptive residency tier (swap hot experts into
  VRAM each step).
- `--max-context 262144 --kv int8 --kv-resident 32768` — KV cache: int8, 32k resident.
This is the proven qwen3.8-flash-next IQ3_S config (~81 tok/s). GLM/maya runs with the analogous Maya config.

## 2. Source map

### src/program (the CLI + serve + decode orchestration) [structure studied, bodies inventory]
- `generate.cpp` (17k lines): `Options` (the full `--flag` surface, ~400 lines), arg parse, device/pack/model
  load, the serve loop, the decode driver. Key hooks: `drive_pool` / `drive_pool_multi` / `drive_pool_split`
  — the callbacks the GPU verify loop calls to run a layer's CPU experts on the `ExpertPool`. Plus the serve
  conversation cache, checkpointing, watchdog/stall reporting, pcie-auto-split.

### src/core (the engine) [partly studied]
- Model: `weights.cpp` (602, load the GGUF/pack weights), `graph.cpp` (146), `layer.cpp` (1638 — the transformer
  layer variants per arch: attention/GDN/MoE/PLE), `layout.cpp`, `arch_defaults.cpp` (GPU-arch env tuning, NOT
  model family), `nv_defaults.cpp`.
- Experts [studied]: `expert_cache.cpp` (VRAM residency slots + the adaptive tier), `expert_source.cpp` (the
  plan: resident / tail-skip / GPU-PCIe / CPU per missed expert, `expert_pool_dispatch_multi`), `peer_experts.cpp`
  / `remote_experts.cpp` / `remote_expert_opt.cu` (multi-GPU / helper experts).
- Decode [studied]: `verify.cpp` (`Verifier::run` — the spec verify-window loop + the per-layer mapped doorbell
  that drives the CPU pool), `mtp.cpp` (the MTP draft layer).
- Session: `session.cpp`, `conversation_*` (the chat/agent conversation cache + checkpoints).
- Device/mem: `device.cu`, `pinned.cu`, `vmm.cpp` (VMM arena), `native_dense.cpp`, `native_head.cpp`.
- Mains: `device_main.cpp`, `load_main.cpp`, `concurrent_main.cpp`, `overlap_main.cpp` (serve orchestration).

### src/kernels (the compute, 158 files) [inventory]
- `cuda/` — the GPU kernels: attention/flash, MoE grouped MMVQ (i-quant x Q8_1), quantize, elementwise, the
  doorbell/wait/copy-rows kernels (verify_kernels.cu, elementwise.cu), RoPE, sampling.
- `cpu/` [studied] — the CPU expert lane: `pool.cpp` (`ExpertPool` worker pool), `native_expert.cpp` (the dot
  dispatch), `iq_avx2/avx512/avx1.cpp` (multi-token i-quant dots, 2-2.4x ggml), `q2_avx2.cpp` (Q2_0),
  `expert.cpp` (act_quant), `expert_layout.cpp` (NativeFmt + ISA probes), tq/kq/nvfp4 (other quant families).
  Arithmetic is ggml-cpu's (`ggml_get_type_traits_cpu`) + these faster kernels; ggml-cpu linked from a forked
  llama.cpp (adds the custom Q2_0). See 2026-10-25-cpu-expert-lane-port-from-upstream.md.

### src/prefill (26) [inventory] — the prefill pipeline (layer-split / PCIe-vs-compute, `arm_cpu_share`).
### src/spec (8) [inventory] — speculative decode (the suffix drafter, draft-kv planning) beside mtp + verify.
### src/dsv41 (Strata) / src/ds4 (Maya) — the DeepSeek family's own graph/kernels (the renamed fork dir).
### src/qwen35 (Strata only) — qwen tooling (dump/compare vs llama.cpp).
### src/ngram, plan, platform, artifact — small: n-gram PLE helpers, plan tables, platform/affinity, artifacts.

## 3. The expert system end to end [studied] — the engine's core trick

A MoE layer's experts don't all fit in VRAM. Per layer, per decode step/window:
1. The router picks k experts per token. `expert_source.cpp` plans each DISTINCT expert: resident VRAM slot →
   compute on GPU; not resident → a miss. Of the misses, tail-skip (the lowest-weight, STRATA_ROUTE_TAIL_SKIP),
   the last `pcie_num/256` read by the GPU zero-copy over PCIe, the REST handed to the CPU `ExpertPool`.
2. The GPU rings a mapped doorbell (ids/x/seq); the host serve loop (`drive_pool*` → `ExpertPool::run_split_multi_native`)
   computes the CPU experts from the pinned/packed weights, in parallel with the GPU's resident+PCIe work, and
   writes the rows to mapped memory; the GPU's `copy_rows_from_mapped` merges them; the combine applies the
   router weights.
3. The adaptive tier (`--adapt-*`) swaps hot experts into VRAM over time; `--expert-profile` warm-starts it.
This overlap (GPU resident + GPU PCIe + CPU lane) + spec decode (verify windows batch several tokens' misses)
is how Strata reaches ~81 tok/s where our from-scratch engine sits at ~46.

## 4. Building our engine from this (the plan)

- Our engine = Strata's shared base (core + kernels + prefill + spec + program), REUSED, consolidated with Maya
  where the fork diverged, plus a thin per-model layer. Not a re-implementation.
- Concretely for the lightweight target: start from Strata/Maya source, strip to the families we want
  (qwen3.8-flash-next, GLM), keep the proven expert/residency/spec/CPU-lane machinery, link ggml-cpu, and
  delete our hand-written reinventions (`engines/unified` u_cpu_iq / u_cpu_mmvq / the bespoke dots, and more as
  the map's deeper passes show what else duplicates the shared base).
- Order (owner's rule): reuse the proven code → build → prove it runs GLM + qwen like the configs here → test/tune
  → optimize only after.
- **gemma4 too (owner, 2026-10-25):** its model graph is unique, but it benefits DIRECTLY from the shared base —
  the serve loop, expert residency + CPU lane, spec/MTP, kernels. So gemma4 rides the same substrate as the
  others; only its per-model layer stays bespoke. Nothing is walled off from the proven base.

## 6. Deepening pass 1 (2026-10-25) [studied]

**The model is GEOMETRY-DRIVEN, not a per-arch switch — the key design.** `layer.cpp`'s main per-layer function
branches `if (is_qsa_layer(g, layer)) qsa_layer(...) else gdn_layer(...)` (line ~1517-1548); qwen3.8-flash-next
is QSA (full attention) every 4th layer, GDN (gated-delta-net linear attention) otherwise, decided from
`ModelGeometry g` (built from the GGUF metadata). There is NO `arch == "qwen"` switch in core/. So a "model
family" = (a) its GGUF geometry parse → `ModelGeometry`, and (b) which LAYER-KERNELS it composes. The engine is
a library of layer-type kernels assembled by geometry.

**The kernel library (`src/kernels/cuda`, the compute surface our engine inherits):**
- attention: `qsa`, `qsa_decode_attn`, `qsa_prompt_attn`, `qsa_select`, `qsa_scores*`, `native_qsa*`, `dflash2`
  (flash), `native_flash_attn`, `rope`/`native_rope`.
- linear attention: `gdn`, `fused_gdn`, `native_gdn`, `native_gdn_preprocess`, `gr`, `fused_gr`, `native_gr_*`.
- MoE: `native_moe`, `s2_expert_grouped`, `router_top10`, `route_prior`, `shared_expert`, the `s_gemv*`/`s2_gemv*`
  matvecs, `iq_kernels` + `native_mmvq` (i-quant MMVQ), `quantize_act`, `dequant_*`, `bf16_gemv`.
- PLE: `ple`, `native_ple_postops`. KV cache: `kv_q8`, `kv_q4`, `kv_stream`. Sampling: `sampler`. Spec/doorbell:
  `verify_kernels`, `elementwise` (the mapped doorbell/copy-rows). `cvec` (control vectors).
- CPU lane mirror in `src/kernels/cpu` (section 2, studied).

So: qwen3.8-flash-next composes {gdn, qsa, native_moe + router + shared_expert, ple, kv, iq/mmvq}; GLM its set;
DeepSeek adds MLA (`dsv41`/`ds4`); gemma4 its own attention/FFN — all on the SAME serve/expert/spec/KV substrate.

**Consolidation sizing — the precise Strata↔Maya file diff [studied]:** of 287 Strata src sources, **94 differ**
from Maya, 147 Strata-only, 47 Maya-only; the differences cluster in `kernels/cuda` (37), `kernels` (18),
`core` (13), `kernels/cpu` (8), `prefill` (6), `spec` (4). The two share the architecture but have DIVERGED
(release lines 0.2.4.2 vs v1.1.27) — "use the similarities" means picking ONE lineage as the base (Strata: newer,
fuller, has `qwen35`) and pulling the other's model-specific code (Maya's GLM graph), not deduping byte-identical
files. A per-file substance diff (trivial version/comment churn vs real GLM changes) is the remaining sizing step.

## 7. Deepening pass 2 (2026-10-25) [studied]

**The orchestration flow (`generate.cpp` `main`, line 2309):** parse ~400 `Options` → load `--pack` + `--native`
GGUF → build `ModelGeometry g` (3640) → `SessionState ss` (3976, the KV/recurrent state) → construct the
`Verifier` (the spec/decode engine; `ver`/`ver_b` for pipelined layer-split stages, `set_commit_async`) → the
decode loop drives `Verifier::run(T, tokens, pool, ...)` per verify window, where `pool` = `drive_pool` /
`drive_pool_multi` / `drive_pool_split` (the callbacks that run a layer's CPU experts on the `ExpertPool`, and
also write the routing trace) → sample → emit. Prompt goes through the same windows (or a batched prompt path).
The whole thing is glue over the core components already mapped; the Verifier loop itself is `verify.cpp`
(studied: the per-layer mapped doorbell + the CPU pool serve).

**The ModelGeometry parse** lives in the GGUF-reading path (`expert_source.cpp` `from_gguf` + `weights.cpp`),
not a tidy function; `layer.cpp` / `mtp.cpp` / `session.cpp` build SUB-geometries for components (the MTP draft
layer, etc.). Reading the GGUF arch metadata → geometry is what our loader must replicate per family. [the exact
field-by-field parse is the one remaining line-level read, best done while writing our loader]

**The consolidation reality — the shared files have DIVERGED, heavily [studied].** The 94 shared-name files that
differ are NOT near-identical: `core/expert_source.cpp` differs by **5,512 lines**, `core/mtp.cpp` 1,434,
`core/layer.cpp` 423, `core/expert_cache.cpp` 367, `core/device.cu` 230. Strata and Maya share the ARCHITECTURE
(same components, same design, same file names) but months of independent development on two release lines
(0.2.4.2 vs v1.1.27) rewrote the internals. **So "use the similarities, cut code" is NOT a file-level dedupe of
near-twin files.** It is: pick ONE lineage as the base — **Strata** (newer, fuller, already runs qwen via
`qwen35` + the IQ3_S config, and as the upstream general engine very likely GLM too) — strip it to the families
we want, and consult Maya as the reference for any GLM-specific behavior Strata lacks or does differently. The
"cut" is cutting families/features we don't need out of the Strata base, not merging Strata⇄Maya twins.

## 5. The one remaining line-level read (do it while building, not before)
- The exact GGUF-metadata → `ModelGeometry` field parse per family (in `expert_source.cpp`/`weights.cpp`) and the
  `--pack` binary layout — both are implementation detail best absorbed when writing our loader against them.
  Everything architectural (run flow, orchestration, geometry-driven layer dispatch, the kernel library, the
  expert/residency/CPU-lane/spec substrate, the Strata↔Maya divergence) is now mapped from source.
