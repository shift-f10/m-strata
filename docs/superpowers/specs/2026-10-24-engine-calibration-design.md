# Engine calibration — per-box hardware tuning for the unified engine (every family)

> **STATUS: DRAFT — owner-requested 2026-10-24.** The owner's note: *"the upstream engines all have
> calibration features, make sure we don't lose that … We need this on every engine."* The owner then
> approved scoping it as its own workstream, written up here **before** the heavy stages, and starting
> with Stage C1. Research was fanned out to two read-only workers over `upstream/` (the `tools/calibrate.py`
> driver + persistence) and `upstream/src` + `engines/maya-ref/src` (runtime consumption + the machinery
> each knob drives). This doc records what calibration IS, the machinery each knob needs, and a staged
> plan. C1 is being built on the owner's go-ahead; C2–C4 await it.

## Why this exists

The upstream Strata / maya-ref engines ship a per-PC **hardware calibration** that measures the specific
box and tunes the streaming-MoE / prefill / speculative paths to it. The unified engine's lean cut
(design `2026-10-23-unified-engine-design.md` §1.2) dropped the CPU expert pool, the PCIe-overlap
machinery, batched/layer-split prefill and the speculative path — and with them, the places most of
that calibration plugged into. The owner wants the calibration **feature** back, as **shared substrate**
(measured once per box, consumed by every family: gemma4, glm5next, qwen4exp, ds4, raon1, qwen35moe),
not re-dropped per family.

## The critical finding — calibration is not standalone

`tools/calibrate.py` (`setup --calibrate`) measures six knobs. **Each knob is inert unless its
performance subsystem exists.** Reintroducing "calibration" therefore means reintroducing the machinery
first, then the thin tuner on top. The map (anchors from the two research passes):

| Knob | What it tunes | Machinery it requires | Present in unified? |
|---|---|---|---|
| Adaptive VRAM tier — `--adapt-every` / `--adapt-swaps` / `--adapt-decay` (`generate.cpp:690,724,695`) | rebalance cadence / swap count / usage decay of the resident-expert set | the resident-expert cache + rebalance | **YES** — `u::ExpertTier` residency + `reg_rebalance` + the STRP profile |
| PCIe share — `--pcie-frac` (`generate.cpp:684`, `PcieAuto :2181`) | share of missed experts the GPU streams over PCIe vs the CPU pool computes | async PCIe-overlap stream **and** a CPU expert-compute lane | no (blocking `cudaMemcpy` fetch, `u_experts.cu:221`; no CPU lane) |
| CPU pool — `--pool-workers` (`generate.cpp:518`) | CPU threads computing non-resident experts | CPU expert lane: thread pool + **native CPU quant kernels per type** + RAM tier + answer buffer | no (dropped by lean cut) |
| Prefill pipe — `--prefill-pipe` (`generate.cpp:638`, `prefill.cpp:2228-2307`) | prompt-chunk pipeline ratio across a layer split | a ≥2-stage **layer split** + pipelined prefill handoff | no (single-GPU, per-token prefill) |
| Spec floor — `--spec-min-p` (`generate.cpp:705`) | speculative draft-window length | an **MTP/draft head + verify pipeline** | no (deferred in every family) |
| HIP shared-stream `STRATA_SH_STREAM`; hipBLASLt/rocBLAS GEMM tables (`gemm.cu`, shipped per-arch) | HIP stream split; GEMM kernel selection | HIP backend | no — CUDA engine, **inert** |

Runtime *probes* that are **not** persisted calibration (do not wire into the profile): SSD read-rate
(`drive_read_gbps`, `STRATA_SSD_GBPS`, `generate.cpp:1383`), the PLE direct-read probe, and the
arena-derived "pinned share" (`set_pinned_share`, `generate.cpp:7521`).

**Persistence model (upstream, to mirror):** `calibrate.py` sweeps candidate values on canned
prompts, keeps a winner only if it beats the default by `MIN_GAIN` over `SWEEP_ROUNDS` alternating
rounds, and writes `{settings, env}` into a per-box config plus a `calibration[hardware_key]` record
(`setup.py:6455-6493`), where `hardware_key` = GPU + CPU + RAM + model + context signature. The profile
is **reapplied automatically on update** (`setup.py:9117-9126`). The unified engine will keep the same
shape: a per-box profile, keyed by a hardware signature, measured once, reloaded on start.

## The workstream (staged; shared `u::` substrate, every family)

**Stage C1 — the calibration facility + adaptive-tier calibration (THIS stage; low risk, every family).**
Build the shared per-box calibration profile (measure → persist keyed by hardware → reload on start →
reapply) and wire it to the one perf subsystem the engine already has: the adaptive expert tier. See
"C1 design" below. Delivers the calibration feature immediately for all six families with **no new
heavy machinery**.

**Stage C2 — async PCIe-overlap expert streaming + its calibration (BUILT; measured no-op on this box).**
Done: the registered expert path now streams a non-resident expert's gate|up|down into a prefetch RING
on a dedicated copy stream, and runs the expert sub-block on its OWN compute stream (not the caller's
legacy default stream, which implicitly serialized with every other stream and blocked any overlap),
with boundary events (caller→compute at entry, compute→caller at exit). The ring depth is the calibrated
knob `stream_prefetch`. **Finding (GLM, RTX 4080 SUPER, parity-verified byte-identical across depths):
prefetch depth 1/3/6 gives the SAME decode tok/s.** At high residency few experts stream (nothing to
overlap); at low residency the streaming is copy-BANDWIDTH-bound (~2.3 GB/token over one PCIe link when
all experts stream, copy ≫ compute), so overlapping copy with compute cannot reduce the single-link copy
time. Overlap only pays when copy and compute are balanced — not here. So `stream_prefetch` DEFAULTS TO 1
(overlap off); the machinery stays, calibratable, for hardware/models where it helps, and is re-checked
on qwen4exp once it generates. The compute-stream separation (depth-independent) is kept (a small gain,
off the legacy default stream). **The real copy-bound levers on this box are residency (C1 — keep more
experts in VRAM so fewer stream) and a CPU expert lane (C3 — COMPUTE some experts instead of copying
them).** C2 is still the prerequisite that would make a future `pcie-frac` meaningful.

**Stage C3 — CPU expert-compute lane + `cpu_expert_frac` (BUILT; correct; measured ~14× SLOWER here).**
Done: a per-token lane that computes a fraction (`cpu_expert_frac`) of a token's STREAMED experts on the
host CPU (a small thread pool, `cpu_workers`) in parallel with the GPU, instead of over PCIe.
`u_cpu_expert.{hpp,cpp}` (host lib) dequantizes GLM's three expert i-quants (IQ2_XXS, IQ2_S, IQ3_XXS)
with byte-identical codebook grids (`u_cpu_expert_grids.inc`, extracted from `u_iq.cu`) + an f32 row
matvec + the gated activation; `ExpertTier` splits the streamed experts CPU/GPU, runs the CPU ones on
worker threads reading the mmap directly, and H2D's their f32 parts into the combine. Unsupported quants
fall back to PCIe. **Finding** (GLM, RTX 4080 SUPER, low residency so all experts stream): output is
correct — at `cpu_expert_frac` 0.5 the first-8 tokens are byte-identical to the GPU path and the text is
coherent (the dequants are right) — but decode is **0.21 tok/s vs 2.89 at frac 0, ~14× slower**. CPU
i-quant dequant+matvec of these small experts (~6.5 MB each) is far slower than a ~0.26 ms PCIe copy, so
offloading to the CPU loses. `cpu_expert_frac` DEFAULTS TO 0 (off); the lane is correct and calibratable,
for hardware/models where the CPU wins (a much faster CPU, larger experts, or a PCIe-starved box). The
auto-split `pcie-frac` calibration (time cpu_c_ms vs the PCIe cost → choose the fraction) is the remaining
piece; it would pick 0 here. NOT ported: native CPU kernels for the qwen4exp quants
(IQ3_S/IQ4_NL/IQ4_XS/Q2_0) and k-quants — a mechanical extension once a box shows the CPU can win.

**Stage C4 — rides on larger roadmap features.** `--prefill-pipe` once batched/layer-split prefill
exists; `--spec-min-p` once the MTP/spec path exists. Calibrate each when its feature lands.

**Skipped on CUDA:** the hipBLASLt/rocBLAS GEMM tables (HIP-only, shipped per-arch, inert here). A
cuBLASLt algorithm-pick tuning could be a minor separate item later if wanted.

## C1 design — `u::Calibration` (shared substrate)

### The profile
A small POD the whole engine reads, holding the adaptive-tier knobs that are currently hard-coded in
each family's `load()`/`generate()`:

```
struct CalibrationProfile {
  // adaptive expert tier (ExpertTier residency + rebalance) — the only subsystem calibratable today
  std::int64_t residency_headroom_mib = 1024;  // VRAM left free below the resident-expert budget
  int          rebalance_every        = 8;      // decode tokens between reg_rebalance() calls
  float        rebalance_margin        = 1.0f;   // the swap-in gain threshold
  double       warm_seed               = 4.0;    // kWarmSeed: protective heat for a warm-started expert
  int          version                 = 1;
};
```

These replace the literals in `glm_graph.cu` / `qwen4exp_graph.cu` / `gemma4_graph.cu` (headroom
`1024<<20`, the `% 8` rebalance cadence, `reg_rebalance(1.0f, …)`, `kWarmSeed = 4.0`). Every family
reads them from the profile, so calibration is genuinely engine-wide.

### Hardware key + persistence
- `hardware_key()` = GPU name + total VRAM (from `cudaGetDeviceProperties`) + the model file's basename
  + the serving context. One box + one model + one context = one record (mirrors upstream's key).
- Profile store: a JSON file in the engine's data dir (e.g. `~/.strata/unified-calibration.json`),
  `{ "<hardware_key>": { ...profile... }, ... }`. Written atomically (tmp + rename), read on start.
  Absent/mismatched key ⇒ the struct defaults (today's values) ⇒ **no behaviour change** until a
  calibration runs. A `version` bump invalidates old records.
- Env overrides (for tests / power users), each taking precedence over the file:
  `STRATA_CAL_HEADROOM_MIB`, `STRATA_CAL_REBALANCE_EVERY`, `STRATA_CAL_REBALANCE_MARGIN`,
  `STRATA_CAL_WARM_SEED`. A `STRATA_CAL_FILE=<path>` points at an alternate store.

### Measurement — the sweep harness
C1 ships the harness as an **external driver** (`engines/unified/tools/calibrate.py`), mirroring
upstream's own model (`tools/calibrate.py` is a Python driver that restarts the engine per value — the
only way to make each measurement cache-fair, since residency/caches are fixed at load). It scrapes the
engine's `hardware_key` from its `calibration: key=…` log line, sweeps the C1 knobs via the
`STRATA_CAL_*` env overrides (each candidate in a fresh process, a throwaway warm-up GEN first), keeps
the winner by **decode tok/s**, and writes the record to the store. An in-engine `--calibrate` subcommand
is a fast-follow (it would need a tier re-finalize/reset path to sweep without a reload). The knobs and
method:
- `residency_headroom_mib`: sweep {1536, 1024, 768, 512} — smaller headroom ⇒ more resident slots ⇒
  higher hit rate, until fragmentation/OOM. Keep the smallest that loads cleanly and is fastest.
- `rebalance_every`: sweep {4, 8, 16, 32} — how often the resident set re-converges vs the per-call cost.
- `warm_seed`: sweep {0, 2, 4, 8} — gen-1 hit-rate vs over-freezing (the lever measured in the GLM
  warm-start work: cache-fair gen-1 resident 20.3%→25.1% at seed 4).
- `rebalance_margin`: hold at 1.0 for C1 (second-order); expose but don't sweep.
Each point: load once, warm one GEN, measure N GENs on a canned prompt; rotate order across rounds to
cancel drift; keep a winner only if it beats the default by a `MIN_GAIN` (≈3%) median — the
`calibrate.py` discipline. The sweep uses the same cache-fair method as `scratchpad/glm_warmstart2.py`
(a throwaway warmup process first). Runtime budget: a few minutes per model.

### Wiring
- `u::Calibration::load(model_path, device)` → `CalibrationProfile`, called once in `u_main.cpp` after
  the device is chosen, before the family graph's `load()`. The profile is threaded into each family
  (a new argument, or a `u::Calibration&` the graph queries) so `reg_finalize` uses
  `headroom_mib`/`warm_seed` and the decode loop uses `rebalance_every`/`rebalance_margin`.
- `ExpertTier` already exposes `reg_finalize(budget, ranked)` and `reg_rebalance(margin)`; C1 feeds them
  the profile values instead of literals. `kWarmSeed` becomes a `reg_finalize` parameter (or a tier
  setter) so the seed is calibratable.
- `--calibrate` is a `u_main` subcommand that runs the sweep and writes the store, then exits.

### Non-goals for C1
No CPU lane, no PCIe-overlap, no prefill-pipe, no spec floor, no HIP/GEMM tables (those are C2–C4 /
skipped). C1 does not change any math — only *where/when* experts are made resident, which is already
non-semantic (output is bit-identical regardless of residency). The profile defaults equal today's
constants, so an un-calibrated box behaves exactly as now.

## Open questions (owner)
1. Profile store location — `~/.strata/` vs the model's own dir vs a repo-ignored path. (C1 assumes an
   engine data dir; easy to change.)
2. Whether `--calibrate` is driven by the frontend/launcher (a "Tune" action, as upstream's
   `native-control.mjs` does) or stays a CLI step for now. (C1 ships the CLI; the launcher hook is a
   follow-up.)
3. C3 (CPU lane) is a large, opt-in effort — confirm it's wanted before building, and on which CPUs it
   should default on.
