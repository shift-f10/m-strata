# Single engine, family libraries — grounded plan (2026-10-25)

Owner's spec: **one executable; each family's differences live in libraries** (not a dispatcher, not
per-family processes, not a .bat launcher). Base = Strata lineage; qwen first; fold in Maya's
meaningfully-different GLM and deepseek as libraries. Review-before-adopt; consolidate to lightweight.

This document is grounded in the actual ported code (`C:\multi-strata\engine`) and Project Maya
(`C:\project-maya`) — read, not guessed.

## Key finding: the single-exe + family-library architecture ALREADY EXISTS

Our ported engine **is** one executable (`strata`, `src/program/generate.cpp`) that links:

- **Shared libraries** (the common base, maintained once): `strata_core`, `strata_kernels`,
  `strata_engine`, `strata_prefill`, `strata_spec`, `strata_kernels_cpu`, `strata_mmq` / `strata_kq`.
- **Per-family engine-path libraries**: `strata_qwen35` (Qwen3.6-35B-A3B) is folded in today
  (`target_link_libraries(strata PRIVATE strata_qwen35)`, guarded by `STRATA_HAVE_QWEN35`).

It has a **family registry** — `include/strata/artifact/model_family.hpp`: the `ModelFamily` enum, the
GGUF-`general.architecture` → family map (`model_family_from_arch`), `model_family_runs`, and the honest
refusal `model_family_not_supported` — and **THE FAMILY SWITCH** at `generate.cpp:2393`, which reads the
`--native` shard's arch string and routes a `qwen35moe` file to the `strata_qwen35` path; everything else
runs the native Flash-Next path. `strata --families` (`generate.cpp:2338`) self-reports what the build runs.

**So the owner's spec is not a rebuild — it is extending this switch to cover our remaining families.**

## What the one exe spans today

| Family | GGUF arch | How it's in the exe | Runs? |
|---|---|---|---|
| Qwen3.8-Flash-Next | `qwen4exp` | native engine path | ✅ runs E2E (~56–57 tok/s, our build) |
| Qwen3.6-35B-A3B | `qwen35moe` | library `strata_qwen35`, in the switch | ✅ switched in (built; folded) |
| DeepSeek-V4.1-Flash | (ds4 packs) | `src/dsv41` (ported from Maya `ds4`) — **separate exe** `strata-dsv41`, `STRATA_BUILD_DSV41=OFF` | ❌ not in the switch |
| GLM-5.3-Flash | `glm5-next` | **not present** — lives only in Maya | ❌ not ported |

## The two pieces of "make it a single engine"

### A. Fold DeepSeek-V4.1 (dsv41) into the one exe — moderate, code is in-tree
`src/dsv41` (Maya's `maya-ds4`, MIT) is already in our tree with its own kernels lib
(`strata_dsv41_kernels`), but builds as a standalone `strata-dsv41` program (`ds4_main.cpp`), OFF by
default. CMake note: *"a family switch in `strata` comes after first light."*
Fold-in = refactor `ds4_main` into an engine-path library (as `q36_engine` is for qwen35), add a
`ModelFamily::DeepseekV41` + its arch label to the registry, and route it in the family switch.

### B. Port Maya's GLM as a family library — large, net-new port from upstream
GLM-5.3-Flash (arch `glm5-next`) is an MLA + sparse-indexer + KDA architecture, meaningfully different
from qwen's GDN/QSA. In Maya it is **~35,000 lines across 25 files** (`src/core/glm_*` ≈ 25.1k / 17 files;
`src/kernels/cuda/glm_*` ≈ 10.1k / 8 files) plus 6 `glm_*_parity.cpp` tests. Maya has **no** family
registry (it predates Strata's registry refactor) and its `core/` diverged from ours.
Port (per the standing rule: reuse upstream first, then test/tune) = bring Maya's `glm_*` engine path in
as a `strata_glm` library, reconcile the Maya↔Strata core divergence, add `ModelFamily::Glm5Next`
(`glm5-next`) to the registry, and route it in the switch. Reuse the shared libs; only GLM's unique path
is new.

## Lightweight consolidation (ongoing, with both of the above)
Build only the `strata` target + its libraries; drop tests/bench/apps/sycl/metal we don't ship; maintain
the shared libraries once. The compiled `secure_frontend` (api-key login + downloader) is the UI, not
Strata's `serve/web`.

## Sequencing (the one owner decision)
Both A and B are real work. A is lower-risk (in-tree, smaller) and gives a second switched-in family
quickly; B is the high-value, large port that brings GLM — the family the owner actually runs on Maya
today. Recommend A first (prove the fold-in seam on in-tree code), then B. Owner to confirm order.
