# Unified engine — one binary for qwen35moe + gemma4 + glm53 + deepseek + raon1

> **STATUS: DRAFT — §6 decisions resolved by the owner 2026-10-23; awaiting go-ahead to build.**
> Owner asked to scope a single merged engine binary running all these families as a design doc
> **before any code**. Research was fanned out to four read-only workers (Maya / upstream-qwen /
> gemma4 engines + the frontend engine interface) plus a model lookup for KRAFTON Raon-1-mini. The
> owner's decisions are in "Decisions resolved" below; §6 records them against the questions. No code
> has been written; building starts on the owner's go-ahead.

## Goals and decisions (owner, 2026-10-23)
**Three goals drive this (owner, in the owner's words): a lightweight, non-bloat, resource-mindful
engine that runs the selected model, from one verified source, not from multiple sources that each
change on every update.**
- (i) **Lightweight / non-bloat / resource-mindful:** one lean engine that runs the selected model
  and nothing more — **trimmed** to the families and features we actually serve (aggressive non-goals,
  SP6-style, §1.2), while keeping the resource-mindful parts that matter on a 16 GB card: the hybrid
  VRAM/RAM expert tiering and the expert-profile placement (§3.5).
- (ii) **Streamline — eliminate redundant code:** the substrate (serve loop, GGUF reader, protocol,
  sampling, session/KV, expert infra, generic kernels) is written and reviewed **once**, not copied
  across three+ engines.
- (iii) **One verified source, not live multi-upstream tracking:** the engine is a single codebase in
  **our reviewed tree**. Upstream Strata, Maya and our gemma4 are **references we port from
  deliberately and review**, not live dependencies that change under us with each release. We never
  run engine binaries pulled unvetted from several sources.

1. **Base: one engine = a shared substrate + per-family graph modules.** The **substrate** (serve
   loop, GGUF reader, protocol, sampling, session/KV, expert-cache/tiering, and the generic kernels —
   matmul, router, RoPE, norms) is written and reviewed **once**. Each family's **unique** graph
   (GLM's KDA/DSA, DeepSeek's MLA, qwen35moe's GDN, gemma4's dense+sliding, raon1's RDA+Gated-MLA) is
   a module that is **ported and reviewed** — copied from upstream/Maya/our gemma4 as the *reference*
   and reviewed into our tree, reusing the shared generic kernels — **streamlined, not rewritten from
   scratch, and not run as an opaque third-party binary**. This serves both goals: one substrate (no
   redundancy) and all-reviewed code. It **rules out** a thin launcher over N near-upstream engines
   run as-is — that keeps N redundant substrates and runs unverified code, against both goals.
2. **gemma4: unified like every other family** — re-expressed on the shared kernel set, **no
   carve-out**. Its 26/26-verified, llama.cpp-parity-checked cuBLAS engine (`engines/gemma4/`) is the
   **reference to port against** and the parity oracle, not shipped as-is. Full unification (decision
   1) applies to all families uniformly, gemma4 included.
3. **Protocol: one converged flavor** — add `EngineProtocolFlavor::unified`, a strict superset, used
   for every merged-engine profile (§3.3 option a).
4. **Priority: GLM + DeepSeek + Raon-1-mini first** (Plan 1, the Maya lineage — highest kernel reuse,
   proves the dispatch, delivers Raon soonest), then qwen (Plan 2), then gemma4 (Plan 3).
5. **Expert placement: one shared facility with profile warm-start** (§3.5, decided earlier the same day).

These set the plan below; building starts on the owner's go-ahead.

Builds on and does not supersede the accepted engine designs
[`2026-10-14-sp6-glm-engine-design.md`](2026-10-14-sp6-glm-engine-design.md) (GLM via MAYA) and
[`2026-10-22-gemma4-engine-design.md`](2026-10-22-gemma4-engine-design.md) (the standalone gemma4
engine). Read both first. Pins at the time of writing: upstream `v0.2.3.1`, Project Maya `v1.1.26`.

## 0. Summary and the one finding that shapes everything

The frontend already presents a **unified server** — one `strata-unified-server`, one page, one API —
over per-family engine *binaries*, bound by the `profile / family / flavor` table (SP6 §4.1). The
owner wants a stronger thing: **one engine binary** that loads and runs every family in one process.

**The finding that shapes the design:** none of the three engine codebases has a pluggable
architecture layer. In every one, a model family is a **hand-written per-family graph in CUDA**;
"multi-family" is achieved by a **shared substrate + a bespoke graph + launcher-level dispatch**, not
by composing a graph from config. In particular Maya is **two binaries from one tree** (`maya` for
GLM, `maya-ds4` for DeepSeek), not one binary serving two families
(`engines/maya-ref/CMakeLists.txt:463,471-473`). So "one binary for five families" is **new work**:
one dispatch binary that selects, by the GGUF's `general.architecture`, among five hand-written
graphs, all standing on one shared substrate. The win is not a smaller codebase; it is one process,
one weight/expert arena, one serve loop, one deployment, and one frontend contract.

**The five families cluster by graph shape, and the clustering picks the base:**

| Family | Arch key | Shape | Attention | Experts | Draft | Lineage |
| --- | --- | --- | --- | --- | --- | --- |
| Qwen3.8-Flash-Next | `qwen4exp` | MoE | **GDN linear + QSA sparse** (hybrid, +PLE, +hyper-conn) | iq_pack | MTP | upstream |
| Qwen3.6-35B-A3B | `qwen35moe` | MoE | **GDN linear** + periodic full | native GGUF | MTP | upstream |
| Gemma-4-26B-A4B | `gemma4` | **dense** + MoE/layer | sliding + full (softmax) | native | — | gemma4 (standalone) |
| GLM-5.3-Flash | `glm5next` | MoE | **KDA linear** + DSA/MLA | tiered | DFlash/NextN | MAYA |
| DeepSeek-V4.1-Flash | (ds4) | MoE | **MLA** + index | tiered | DSpark | MAYA |
| **Raon-1-mini** (new) | `raon1` | MoE | **RDA linear** (18/25) + **Gated MLA** (7/25), NoPE | tiered, sigmoid+shared | DFlash-style | **MAYA** |

Four of six graphs (both Qwens, GLM, Raon) are **linear-attention hybrids**; three (GLM, DeepSeek,
Raon) use **latent/MLA attention + sigmoid-routed MoE + a draft block** — i.e. the **MAYA lineage**,
and **Raon-1-mini is a near-twin of GLM**. Gemma4 is the outlier (dense, standalone cuBLAS engine).
This is why the recommended base is **Maya's substrate** (§3): its KDA linear-attention, MLA
latent-KV, sigmoid router, grouped-MoE/expert-tiering and DFlash kernels already cover the three MAYA
families and most of Raon, and its linear-attention path is the closest existing scaffold for qwen35moe's GDN.

## 1. Goals, non-goals, definition of done

### 1.1 Goals
1. **One engine binary** that, given a GGUF of any supported family, reads `general.architecture`,
   dispatches to that family's graph, and serves it over **one wire protocol** — behind one
   `engine.executable` the frontend already points every card at.
2. Preserve the capabilities we **serve** per family (qwen MTP + hybrid experts; gemma4 sliding/full +
   dense+MoE; the GLM/DeepSeek chat/serve path) and **add Raon-1-mini** (`raon1`) — trimmed of
   features we don't serve (goal i).
3. Keep the frontend's fail-closed contract: a typed argument profile, **one** protocol flavor, a
   typed environment allowlist; the family/tokenizer/renderer stay **startup-selected, never inferred
   from a model artifact** (a security invariant — `chat_execution.hpp:20-22`).
4. **One shared expert-placement facility** (VRAM/RAM(/disk) tiering + an expert-hit profile that
   warm-starts placement and can be saved per model) used by **every** family, replacing each
   engine's ad-hoc placement — in particular gemma4's arbitrary cold start (§3.5).
5. NVIDIA first; every GPU claim from the owner's box (CLAUDE.md). AMD/HIP where a lineage already has it.

### 1.2 Non-goals (v1)
- A generic, config-driven "architecture registry" that composes any graph from metadata — no engine
  has one and building one is a research project; v1 is five **hand-written** graphs behind a dispatch.
- Re-deriving model math from scratch: each family's graph is **ported from its reference**
  (upstream/Maya/our gemma4) and reviewed, reusing the shared generic kernels — streamlined, not
  reinvented.
- **Anything we don't serve, carried only because an upstream has it** (keeps the engine lean): EXL3
  quant paths, multi-GPU tensor parallelism, vision towers for text-only families (qwen35moe, gemma4
  and the Raon preview are text-only), exotic/unused quant layouts, and drafter variants we don't use.
- Live multi-upstream tracking: we do not add Strata/Maya/other engine binaries as dependencies that
  update under us; ports are deliberate and reviewed (goal iii).
- Merging the Python `serve/` orchestration layers — the frontend is the server; the engine stays
  "token ids in, text/ids out".

### 1.3 Definition of done (follows SP6's four-part, box-gated pattern)
1. **Dev-host + CI:** the dispatch, the per-family geometry readers, the frontend protocol
   convergence and the typed seams, with hermetic tests green on `linux-gcc`,
   `windows-llvm-mingw-cross` (wine) and, after the owner pushes, `windows-msys2-clang`. No GPU claim yet.
2. **Box bring-up (owner), per family:** each family loads its real GGUF in the merged binary and
   passes a parity gate (logits/KL vs its reference) + a short generation battery on the owner's
   NVIDIA box; results committed under `docs/validation/unified-engine/`.
3. **Frontend acceptance (owner):** a model of each family served end-to-end through
   `strata-unified-server` (the page + the API kit), one binary, both OSes.
4. Only then does the handoff say "unified engine verified on <GPU>, <OS>" per family.

### 1.4 Plans (indicative; sized in §5)
- **Plan 0 — base decision + skeleton:** confirm the base (§6 Q1); stand up the merged binary that
  reads `general.architecture` and dispatches to a stub per family; wire its single protocol flavor
  into the frontend; green on CI (no GPU).
- **Plan 1 — the MAYA families:** fold `maya` (GLM) and `maya-ds4` (DeepSeek) into the one binary
  (they already share the substrate); add **Raon-1-mini** (`raon1`) as a new graph reusing GLM's
  linear-attention/MLA/sigmoid-MoE/DFlash kernels. Box parity per family.
- **Plan 2 — the qwen families:** bring `qwen35moe` onto the substrate (its GDN linear-attention
  mapped onto the KDA/linear path; MoE onto the shared expert infra) and `qwen4exp`/Flash-Next.
- **Plan 3 — gemma4:** lift `engines/gemma4/`'s graph onto the shared substrate (or keep its cuBLAS
  kernels as a gemma4 module inside the binary); dense, no MoE, needs Gemma norms/soft-cap/sliding.
- **Plan 4 — frontend + catalog:** the `deepseek`/`raon1` profiles across the exhaustive switches,
  renderers, tokenizer modes, manifest/catalog entries, and the managed/download-card paths.

## 2. The engine landscape today

### 2.1 The `profile / family / flavor` contract (SP6 §4.1 + gemma4 + frontend worker)

`EngineProfile { qwen, glm53, gemma4 }` (`frontend_config.hpp:105`) is the launch-time enum; it fans
out through **exhaustive** switches to `EngineArgumentsProfile { strata_0143, lighttransport_glm53,
strata_gemma4 }`, the prompt renderer `NativeChatProfile`, the protocol flavor `EngineProtocolFlavor
{ strata_0143, lighttransport_glm53, maya_118 }`, the environment family `{ strata, glm }`, the
tokenizer mode `{ byte_bpe, spm_bpe }`, per-request reasoning-effort, and page options
(`subsystem_limits.cpp:69-119`, `model_options.cpp:545`). The engine **binary is one per-card config
key** (`engine.executable`, `model_runtime.cpp:103`): a merged binary plugs in with **no code change
for selection** — point every card's `engine.executable` at it. `deepseek` and `raon1` have **no
frontend profile yet**; each is a brand-new family to add across those exhaustive switches.

### 2.2 gemma4 engine (`engines/gemma4/`) — standalone, `strata_0143`
Clean-room CUDA/cuBLAS engine (~4,900 lines, 13 `g4_*` files), **no `upstream/` linkage** (diverged
from its spec §3 per plan R1/R7). 30-layer 26B-A4B MoE **plus a dense GELU MLP each layer**; 16 query
heads, GQA (8 KV sliding / 2 full), two head widths 256/512, 5 full layers (V-from-K), dual RoPE,
attn scale 1.0, logit soft-cap; text-only. Geometry **hard-coded** (`g4_model.hpp`). Tokenizer
frontend-side (`spm_bpe`). Hybrid expert placement (pinned RAM + VRAM slots + routing rebalance), f16
KV ring for sliding layers, cuBLAS prompt chunks. Speaks `strata_0143`. 26/26 gate; parity at
`docs/validation/gemma4/`. **Merge cost: low–moderate** (self-contained, protocol-compatible) but it
reuses **none** of the shared substrate (its own cuBLAS kernels), so folding it onto Maya's kernels is
rework; keeping its kernels as a gemma4 module inside the binary is the cheaper path.

### 2.3 upstream qwen engine (q36) — `strata_0143`
The qwen engine is upstream's `strata` binary; `main()` has a **family switch**
(`generate.cpp:2394-2407`) that routes arch `qwen35moe` to a self-contained engine in
`src/qwen35/q36_engine.cu` (~217 KB; reference `q36ref.cu`), while `qwen4exp` (Flash-Next) stays on
the main engine + the iq_pack path. **qwen35moe** = 40 layers, **hybrid Gated-DeltaNet** (30 linear
layers, no KV) + periodic **full attention** (10 layers, GQA 16/2, head_dim 256, NEOX RoPE, sigmoid
output gate) + **MoE** (256 experts top-8 + 1 shared) + an **MTP** draft block (`--mtp N` = verified
draft count). Tokenizer frontend-side (`byte_bpe`, `QWEN35_PATTERN`). fp32 KV + GDN recurrent state;
hybrid VRAM/RAM experts + CPU pool + PCIe streaming; CUDA-graph decode; in-engine sampling. **Merge
cost: high** — the graph is self-contained and documented (`q36ref.cu` is the spec), portable in days
to a week **only if** the target supplies quantized grouped-MoE matmul, a **fused linear-attention/GDN
primitive**, and sampling/sessions. Maya has a KDA/gated-delta linear-attention path (a promising
match for GDN — to validate) and the grouped-MoE + expert-tiering infra, so Maya is the better host;
the cost is the kernel-library dependency and the hybrid-offload subsystem, not the math.

### 2.4 Maya engine (`engines/maya-ref`) — `maya_118`, the recommended substrate
**Two binaries from one tree**: `maya` (GLM, `src/program/generate.cpp`) and `maya-ds4` (DeepSeek,
`src/ds4/ds4_main.cpp`), each a hand-written graph, selected at the Python launcher
(`maya.py` `FAMILIES=("glm","deepseek")`) and, inside `maya`, by `--glm-pack` vs the legacy
`qwen4exp` path. **No runtime graph registry.** The **shared substrate** (the reuse asset, and why
Maya is the base): the header-only GGUF reader (`strata_artifact`), the per-family "geometry struct
parsed from `<arch>.*` keys" pattern, the **MoE machinery** (grouped GPU expert GEMV, AVX-512/ggml-cpu
CPU expert lane, and the VRAM/RAM/disk **expert cache + profile tiering** — `expert_source.hpp`,
`expert_cache.*`), routers (`router_top10`, **`router_sigmoid`**), `sampler`, `rope`, KV-quant
kernels, the `strata_mmq`/`strata_prefill` batched prompt path, multi-GPU layer split, `--parallel`,
the **wire protocol**, and the Python `serve/` layer (which already carries **Qwen** chat/effort
handling; `server.py` default model is `qwen3.8-flash-next`). GLM's graph (KDA linear attention, DSA
sparse/latent attention, sigmoid-routed MoE, NextN/DFlash) and DeepSeek's (MLA + index + DSpark) are
bespoke hybrids. Build: `strata_artifact → strata_plan → strata_core → strata_kernels(+cpu) →
strata_engine → strata_prefill`; CUDA (sm_75+) or HIP (RDNA); pinned llama.cpp ggml (`3cf03257`); **no
CPU-only inference**. **Implication:** Maya is a strong base for the *substrate* but **not** a
pluggable engine — adding a family means a new bespoke graph (today, a new binary). Raon-1-mini reuses
GLM's kernels most directly; qwen35moe maps onto the dormant `qwen4exp`/`native` path + the KDA linear
primitive; gemma4 is the least-aligned (dense, no MoE).

### 2.5 frontend engine interface — the integration seam
The wire has **no family field**; the dialect is chosen at client construction and **never inferred
from engine output** (security invariant), and the three flavors **fail closed on each other's private
records** (three `PP` grammars; `RESUME`/`REUSED` is `strata_0143`-only; `NOTE`/`STAT` is
`maya_118`-only — `engine_protocol_client.cpp:396-468`). `READY`/`GEN`/`T`/`DONE` are family-agnostic.
One `EngineSupervisor` owns all generations; each is a fresh process + protocol client; the
environment is a closed type (nothing inherited by name). GPU selection is **family-split**: `strata`
family passes `--gpu` in argv; `glm` family forces `CUDA_VISIBLE_DEVICES` + the GLM env table.
Managed/download-card paths are **qwen-only** today (`engine_gate` admits only `ManifestProfile::qwen`;
`download_card_template` is qwen-only); operator model-cards already carry an arbitrary
`profile` + `engine.executable`, which is how all three profiles are reached.

## 3. Merge strategy

### 3.1 Base: Maya's substrate, one dispatch binary, five graphs
Build the merged engine as **a new serving binary inside the Maya tree** (or a thin tree that links
Maya's libraries), reusing `strata_artifact` + `strata_core` + `strata_kernels(+cpu)` +
`strata_prefill` + the expert-tiering + the serve loop. At `main()` it reads
`general.architecture` from the GGUF header (the pattern already in `maya.py`/`server.py` and the
per-family geometry readers) and dispatches to one of five graph modules:
`glm5next`, `ds4`, `raon1`, `qwen35moe`, `gemma4` (+ `qwen4exp`). Under **full kernel unification**
(decision 1) every graph is re-expressed on **one shared kernel set**: GLM and DeepSeek move from
Maya's two `main()`s onto the shared kernels; `raon1` is new but GLM-shaped; `qwen35moe`/`qwen4exp`
are ported from upstream; and `gemma4` is **re-expressed from `engines/gemma4/`'s cuBLAS reference
onto the shared kernels** (decision 2 — not lifted). Dispatch is **static per family** (no
config-built graph) — matching how every engine already works and keeping the frontend's "no behavior
from model data" invariant on the engine side too (the binary trusts only the GGUF arch key, which it
also guards).

### 3.2 Per-family graph plan and kernel reuse
- **GLM-5.3 (`glm5next`)** and **DeepSeek (ds4):** move the existing `maya`/`maya-ds4` graphs behind
  the dispatch. Mostly mechanical (one `main()`, one serve loop); the graphs and kernels are unchanged.
- **Raon-1-mini (`raon1`, new):** a new graph, but its pieces already exist in GLM — **RDA** linear
  attention ≈ GLM **KDA** gated-delta; **Gated MLA** ≈ GLM **DSA/MLA** latent attention (new dims: QK
  192 / V 128, LoRA 576/512, **NoPE** so the RoPE step is dropped); **sigmoid-routed MoE + shared
  expert** = `router_sigmoid` + the grouped-MoE/expert-tiering infra verbatim (128 experts, 6+1);
  **DFlash** draft reuses GLM's NextN/DFlash. New work is a `raon1.*` geometry reader, the RDA/Gated-MLA
  kernels where dims differ, and the pack/tokenizer. **Moderate** — the highest-reuse new family.
- **qwen35moe:** geometry reader + the **GDN** linear path (validate reuse of KDA/gated-delta) +
  periodic full attention + MoE on the shared expert infra + MTP. **High**, per §2.3.
- **qwen4exp (Flash-Next):** the dormant `qwen4exp`/`native` path in Maya is the scaffold; iq_pack
  experts vs the tiered cache is the main reconciliation.
- **gemma4:** dense MLP + sliding-window + Gemma norms/soft-cap, 128 routed experts. **Re-expressed
  on the shared kernels** (decision 2 — no cuBLAS-module carve-out), with `engines/gemma4/`'s
  26/26-verified cuBLAS engine as the port reference and the llama.cpp-parity oracle. Its routed
  experts join the shared placement facility (§3.5), gaining the expert-hit-profile warm start it
  lacks today. The standalone `engines/gemma4/` engine stays in-tree as the reference; the merge
  subsumes it. This is more rework than lifting it, but keeps one uniform kernel set (the owner's choice).

### 3.3 Protocol convergence (the single biggest frontend change)
A merged binary cannot emit both the `strata_0143` record set and the `maya_118` set while the client
is pinned to one flavor. Choose one:
- **(a) One flavor for the merged engine — CHOSEN (decision 3):** the binary emits a single superset
  dialect; add `EngineProtocolFlavor::unified` (and a matching `EngineArgumentsProfile`), make
  `protocol_flavor_for` return it for all merged-engine profiles, and implement its
  `await_ready`/`next_event` grammar as the union the binary actually emits. One grammar to test,
  fail-closed.
- **(b) Keep per-family flavors, one binary:** the binary emits each family's existing dialect; the
  frontend keeps choosing the flavor by profile. No new grammar, but the binary must switch its own
  output dialect per loaded family — more surface, and it reintroduces three grammars.
Either way `READY`/`GEN`/`T`/`DONE` are unchanged. The frontend keeps **per-family `EngineProfile`**
(prompt renderer, tokenizer mode, reasoning-effort genuinely differ) — do **not** collapse to one
profile + a runtime arch arg; that would move behavior selection toward model data (invariant break).

### 3.4 Tokenizer modes and GPU selection
Tokenizers stay frontend-side and startup-selected by profile: `byte_bpe` (qwen, glm, deepseek),
`spm_bpe` (gemma4), and **raon1** needs its vocab (259,840) + pre-tokenizer registered — likely a new
`TokenizerMode` or a parameterization of `byte_bpe`; confirm from the GGUF `tokenizer.ggml.*` on the
box. GPU selection must unify: today `strata` family uses argv `--gpu`, `glm` uses env. Pick **one**
mechanism for the merged binary and set `engine_family_for` to one family (or extend the `glm` env
table to cover it). Recommend argv `--gpu` for all (simpler, matches qwen/gemma4), moving GLM off the
env mechanism.

### 3.5 Shared expert placement and the expert-hit profile (owner decision, 2026-10-23)
Today each engine places MoE experts its own way, and gemma4's is the weakest: `place_experts`
(`engines/gemma4/src/g4_engine.cu`) fills the VRAM slots with experts 0..S-1 — an **arbitrary cold
start** — and only learns a good slot→expert map live, during decode, via routing-count `rebalance`;
prefill never benefits and there is no profile. The upstream and MAYA families instead ship a
committed **expert-hit profile** (`upstream/data/expert-profile*.bin`,
`engines/maya-ref/data/expert-profile-glm.bin` / `-ds4.bin`) that pre-places the hottest experts at
load, and the frontend already composes `--expert-profile` / `--expert-profile-save[-every]`
(`engine_arguments.cpp:333`, `launch_composition.cpp:73,232`) so a per-model profile is learned and saved.

The merged engine makes expert placement **one shared facility** for every family, rather than a new
optimization per engine:
- **Warm start:** at load, fill each layer's VRAM slots from a committed-or-saved profile, hottest
  first, so decode **and prefill** are fast from the first token (not after `rebalance` converges).
- **Save:** `--expert-profile-save[-every]` dumps the decayed routing scores as a per-model profile;
  the frontend already owns these flags for the qwen family and extends them to the merged binary.
- **Online correction:** the live `rebalance` stays, as the correction on top of the warm start.
- **One format, one path** for qwen35moe, qwen4exp, gemma4, glm53, deepseek and raon1 — so gemma4
  gains the warm start for free when its graph moves onto the substrate.

The standalone `engines/gemma4/` engine is **not** modified for this (owner decision); the facility is
built once in the merged engine and gemma4 inherits it through the merge. The frontend's gemma4
argument profile gains `--expert-profile` only when it points at the merged binary (today
`kGemma4Flags` omits it).

## 4. Build, platform, repository layout
- The merged binary lives in the Maya tree (it needs Maya's libraries + ggml pin). Expose it through
  an optional root CMake component, **OFF by default, never in CI** (SP6 §3.3 pattern), built on the
  owner's box. Name e.g. `strata-unified-engine`; the frontend's `engine.executable` points at it.
- `engines/gemma4/` either contributes its kernels as a module (add to the merged build) or stays a
  separate binary during transition (§6 Q3).
- NVIDIA sm_75+ (CUDA + cuBLAS); HIP (RDNA) where the constituent graphs already support it; no
  CPU-only inference (GPU backend required). Pinned llama.cpp ggml `3cf03257` as Maya uses.
- `upstream/` and `engines/maya-ref/` stay **unmodified submodules**; qwen35moe/qwen4exp port means
  **copying** their graph into our tree (as gemma4 did with Q36's structure), never patching the
  submodule — an owner-approved fork only if a submodule change is unavoidable (SP6 A9).

## 5. Risks and effort
- **Size.** Each bespoke graph is thousands of lines (gemma4 ~4,900; `glm_model.cu` ~2,700;
  `ds4_model.cu` ~12,000). The merge is **multi-month**: Plan 1 (GLM+DeepSeek fold + Raon) is the
  natural first milestone and the highest-value (adds Raon, proves the dispatch); qwen and gemma4
  follow. This is SP-scale, not a single change.
- **Is one binary worth it?** The frontend already gives a unified *server* over per-family binaries.
  The concrete wins of one *binary* are a shared weight/expert arena, one serve loop and one
  deployment — real for a box juggling families, but weigh against the merge cost. A cheaper
  intermediate (one dispatch binary that `exec`s/links the existing graphs, no kernel unification)
  captures most of the deployment win. **§6 Q2.**
- **GPU-only verification.** Nothing is "done" per family until the owner's box runs its parity gate;
  agents can only land the dispatch, readers, protocol and tests on CI.
- **Protocol fail-closed.** A converged flavor must be a strict superset; any slack reopens the
  "malformed engine output" threat the three-flavor split closes. Grammar tests are mandatory.
- **Divergent upstreams.** Folding GLM/DeepSeek behind one dispatch forks them from Maya's two-`main()`
  layout; future Maya bumps (v1.1.27+) get harder to track. Keep the graphs as verbatim-as-possible
  modules and record the bump runbook (SP6 §3.4 pattern).

## 6. Decisions (resolved 2026-10-23) and the one remaining detail
1. **Base (§3.1):** RESOLVED — Maya's substrate, one dispatch binary (see "Decisions resolved" #1).
2. **Depth (§5):** RESOLVED — **full kernel unification** (#1), not the lighter reuse path.
3. **gemma4 (§3.2):** RESOLVED — **re-express on the shared kernels** (#2), no cuBLAS-module carve-out.
4. **Protocol (§3.3):** RESOLVED — **one converged `unified` flavor** (#3).
5. **Raon-1-mini scope — the one detail left:** the preview is text-only and ships a DFlash draft.
   Default is to include speculative decoding from the start (it reuses GLM's NextN/DFlash), settled
   inside Plan 1; flip to "base graph parities first, drafting follows" if preferred.
6. **Priority (§1.4):** RESOLVED — **GLM + DeepSeek + Raon first** (#4), then qwen, then gemma4.
