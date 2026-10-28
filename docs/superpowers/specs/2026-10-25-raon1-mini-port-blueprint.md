# raon1-mini — build blueprint (2026-10-25)

Next engine family after GLM (owner: raon1-mini, then flint). Grounded in the real model + the unified-engine's
raon1 reference, not guessed.

## What raon1-mini is (GGUF arch `raon1`)
Model: `C:\Strata-data\models\raon1_q8\raon-1-mini-preview-Q8_0.gguf` (30.7 GB, single Q8_0 shard).
Shape (from `raon1.*` GGUF metadata + the unified-engine's `engines/unified/src/family/raon1_graph.hpp`):
28.8B total / 4.3B active MoE; 25 layers; hidden 2560; 40 heads; head_dim 128; dense intermediate 12032; MoE
intermediate 1088; 128 experts, 6 routed + 1 shared; **sigmoid router + renormalize**; leading_dense_block_count = 1
(layer 0 dense FFN); vocab 259840; context 262144; hyper-connections (count 4). Attention is a
**hybrid**: 18/25 layers **RDA** (Raon Delta Attention, NoPE), 7 **Gated-MLA** (latent/MLA, QK 192 = 128 nope + 64
**rope** / V 128, q/kv LoRA 576 / 512). Ships a DFlash-style draft block. **(All confirmed from the GGUF — see
"Confirmed from the model" below.)**

## Key finding: raon1 ≈ GLM-5.3-Flash — reuse the GLM engine we just vendored
raon1 has **no upstream engine** (not in Strata or Maya). But it is architecturally a near-twin of GLM, and its
reference (`raon1_graph.hpp`) is explicitly "modeled on Maya's `glm_model.cu`, reusing the GLM-shaped kernels":
- **RDA = GLM's KDA** gated-delta linear attention (`glm_kda`, already vendored in `strata_glm`).
- **Gated-MLA = GLM's absorbed-MLA** latent attention (`glm_dsa` MLA path, already vendored).
- **MoE** (128 exp, 6+1, sigmoid+renorm) rides the same expert tiering + `router_sigmoid` (vendored).
So raon1 is built by REUSING `strata_glm`'s kernels with a raon1 model graph — NOT reinventing, NOT the
deprecated `engines/unified` base. The deprecated `engines/unified/src/family/raon1_graph.{hpp,cu}` is a useful
**reference** for the exact raon1↔GLM parameterization (it did this mapping), alongside Maya's `glm_model.cu`.

## Deltas from GLM (what the raon1 graph must change)
- Geometry: Raon's dims (above), pinned from `raon1.*` metadata.
- Per-layer hybrid: an attention-type array (18 RDA + 7 Gated-MLA) — like GLM's qsa/gdn layer pattern.
- NoPE everywhere (GLM's rope dims zeroed / skipped).
- Sigmoid router + renormalize + shared expert (GLM already has this shape).
- Hyper-connections (`raon1.hyper_connection.*`), leading-dense layer 0, the Raon LoRA ranks.
- The DFlash draft block (MTP) — same facility as GLM.

## Plan (into the ONE engine)
1. Registry: add `ModelFamily::Raon1` (arch `raon1`) to `engine/include/strata/artifact/model_family.hpp`,
   RED→GREEN in `tests/core/model_family_test.cpp` (done alongside this doc).
2. raon1 runs in the **same family DLL as GLM** (`strata_glm` — a Maya-lineage family DLL): add a raon1 model
   graph reusing the vendored `glm_kda` / `glm_dsa` / MoE kernels with Raon geometry + the hybrid pattern.
   The family switch in `generate.cpp` routes `raon1` (and `--raon-pack`/`--native` raon1) to the DLL entry,
   which dispatches by arch (glm5-next vs raon1).
3. Verify per-layer against an oracle (memory `engine-parity-oracle`): raon1 has no upstream engine, so the
   oracle is the model's reference / the unified-engine raon1 graph's validated values; the GLM kernels
   themselves are already parity-tested. GPU/speed claims only from the owner's box.
4. Then tune; then flint (RecursiveSI/flint) next.

## Design detail (implementation level)

**Family placement.** raon1 runs in the **same family DLL as GLM** (`strata_glm`, a Maya-lineage family DLL),
reusing its vendored kernels: RDA = `glm_kda`, Gated-MLA = `glm_dsa` (MLA path), MoE = `router_sigmoid` +
`shared_expert` + the expert tier. The DLL's entry dispatches by GGUF arch (`glm5-next` → Glm5Model, `raon1` →
a Raon1 runner). The Raon1 runner is a near-clone of Maya's `glm_model.cu` forward with Raon geometry + the layer
pattern; reference for the exact mapping: `engines/unified/src/family/raon1_graph.{hpp,cu}` (which already did it).

**Geometry** (`Raon1Geometry`, confirmed from `raon1.*` GGUF metadata): n_embd 2560, n_layers 25,
n_head 40, head_dim 128, n_vocab 259840, n_ff_dense 12032, n_ff_exp 1088, n_expert 128, n_exp_used 6, n_shared 1,
dense_lead 1 (layer 0 dense FFN). MLA dims: q_lora 576, kv_lora 512, **qk 192 = qk_nope 128 + qk_rope 64**, v 128.
RDA: d_conv 4 (`ssm.conv_kernel`), gate lower-bound −5 (`kda.gate_lower_bound`), kda.head_dim 128,
ssm.time_step_rank 128. Router: sigmoid (`expert_gating_func=2`) + renormalize (`expert_weights_norm=True`),
`expert_weights_scale` 1.0. rms_eps 1e-5; latent_norm rms_eps 1e-6. **RoPE on the MLA layers only** (64 dims,
`rope.dimension_count=64`); **RDA layers are NoPE**. Activation **`situ`** (situ_beta 4.0, situ_linear_beta 25.0),
NOT plain silu. Hyper-connections: count 4, eps 1e-5, magnitude 2.0. Tokenizer gpt2/BPE, pre `raon`.

**Per-layer hybrid — GLM's exact convention, confirmed from the model.** The `raon1.attention.head_count_kv[il]`
array (25 entries) drives the MIXER: `0` → **RDA** (linear/recurrent), nonzero → **Gated-MLA** — identical to GLM's
`is_recr = (n_head_kv==0)` in `glm_model.hpp`. Confirmed array:
`[0,0,0,1, 0,0,0,1, 0,0,0,1, 0,0,0,1, 0,0,0,1, 0,0,0,1, 1]` → **18 RDA** {0,1,2,4,5,6,8,9,10,12,13,14,16,17,18,20,21,22}
+ **7 MLA** {3,7,11,15,19,23,24} (pattern: 3 RDA → 1 MLA, repeating; final layer 24 also MLA). **Independently**,
the FFN tier is set by `leading_dense_block_count=1`: layer 0 = dense FFN. So **layer 0 = RDA mixer + dense FFN**;
the mixer split is over all 25 layers, NOT "1..24". The raon1 forward is GLM's forward with this array + Raon dims.

**The three mixers (per layer):**
- `layer_rda` (RDA, reuse `glm_kda`): proj → short conv (d_conv 4) → decay gate (lower-bound −5) → gated-delta
  recurrence over a per-head state → RMS out-gate → output proj. **NoPE** (linear attention). State:
  `rda_state_[il]` (+ `rda_conv_[il]`).
- `layer_mla` (Gated-MLA, reuse `glm_dsa` MLA path): absorbed-MLA latent attention, q/kv LoRA 576/512,
  QK 192 (**qk_nope 128 + qk_rope 64 — decoupled RoPE on the 64 rope dims, DeepSeek/GLM-MLA style**), V 128.
  State: `mla_latent_[il]` (the cached latent per token). latent_norm rms_eps 1e-6.
- `layer_ffn`: dense MLP (**`situ` activation**, situ_beta 4.0 / situ_linear_beta 25.0, ff 12032) for
  layer < dense_lead; else routed MoE (sigmoid router, top-6, renormalize) + the shared expert, on the expert tier.
  The same `situ` activation is used in the expert MLPs. (`situ` ≈ a β-scaled SiLU/SwiGLU; confirm exact form on box.)

**Load.** Read `raon1.*` metadata (reference: the unified `from_gguf`), upload non-expert tensors, register the
routed experts with the shared tier (+ residency), carve the RDA/MLA state arenas. The DFlash-style draft block
(nextn) maps to GLM's MTP if the GGUF ships one.

**Confirmed from the model (2026-10-25, `G:\Strata-data\models\raon1_q8\raon-1-mini-preview-Q8_0.gguf`, 731
tensors).** All geometry above is read from `raon1.*` GGUF KV — not guessed. Three corrections vs the first draft:
(1) **not NoPE everywhere** — the MLA layers carry decoupled RoPE (64 dims); only the RDA layers are NoPE;
(2) activation is **`situ`** (situ_beta/linear_beta), not plain silu; (3) the hybrid is over **all 25 layers**
(layer 0 has an RDA mixer), with the exact `head_count_kv` array above. Router confirmed sigmoid + renorm.
**Residual box TODOs (need the weights/parity oracle, not just KV):** the exact `situ` formula (β-SiLU vs β-SwiGLU
clamp) and whether the 64 rope dims are interleaved or split; the RDA conv/gate/beta tensor layout vs `glm_kda`;
and the DFlash/MTP draft tensors (if any) in the 731-tensor list. raon1 has no upstream engine, so the oracle is
the model's own reference (memory `engine-parity-oracle`); the GLM kernels it reuses are already parity-tested.

**Switch + run.** `ModelFamily::Raon1` (done); `generate.cpp` routes a `raon1` file to the DLL; the runner
dispatches raon1 vs glm5-next by arch. GPU/speed claims only from the owner's box.

## Tensor-level implementation spec (grounded in the real GGUF + the vendored GLM code, 2026-10-25)
Dumped raon1's 731 tensors and compared to the vendored GLM pack's. raon1 is a **real GLM variant, not a config
tweak** — reuse is at the KERNEL/infra level, with raon1-specific wiring.

**Architecture decision: a SEPARATE `Raon1Model` in `strata_glm`, NOT an extension of `Glm5Model`.** `Glm5Model`'s
`from_gguf` hard-refuses raon1 (lines ~602 `key_length==kv_lora`, ~609 `qk_nope==v_head`) and its forward is wired
for GLM's DSA (sparse indexer, factored KDA gate, NoPE, no MLA gate). GLM now runs end-to-end (smoke-tested on G:
2026-10-25) and must not regress. So raon1 gets its own `Raon1Model`/`Raon1Geometry` that REUSES the shared kernels
and infra (expert cache/tier, `router_sigmoid`, `glm_hc`, the SSM/MLA building blocks, pack loader, sampler) with
its own forward. The DLL entry (`glm_main`) dispatches by GGUF arch: `glm5-next`→`Glm5Model`, `raon1`→`Raon1Model`.

**Per-tensor → kernel map (from the dump):**
- *Global:* `token_embd` Q8_0 [2560,259840], `output` Q8_0, `output_norm` F32. (vocab 259840.)
- *RDA mixer (`is_recr`, 18 layers), ssm_\*:* `attn_q/k/v` [2560,5120], `attn_output` [5120,2560], `attn_norm`;
  `ssm_a` [40] (per-head decay), `ssm_beta` [2560,40], `ssm_conv1d_{q,k,v}` [4,1,5120] (3 short convs, d_conv 4),
  `ssm_dt.bias` [5120], `ssm_f_a` [2560,128]/`ssm_f_b` [128,5120] (rank-128 dt factor = `ssm.time_step_rank`),
  `ssm_g` [2560,5120] (FULL output gate), `ssm_norm` [128], **`ssm_rda` [2560,40]** (raon-specific per-head proj).
  **Δ vs GLM KDA:** GLM has factored `ssm_g_a`/`ssm_g_b` and NO `ssm_rda`; raon1 has full `ssm_g` + `ssm_rda`.
  → adapt `glm_kda`: full-rank gate path + the extra rda term.
- *MLA mixer (7 layers {3,7,11,15,19,23,24}):* `attn_q_a` [2560,576]/`attn_q_a_norm` [576]/`attn_q_b` [576,7680]
  (40 heads × 192), `attn_kv_a_mqa` [2560,**576 = kv_lora 512 + rope 64**]/`attn_kv_a_norm` [512], `attn_k_b`
  [128,512,40], `attn_v_b` [512,128,40], **`attn_gate` [2560,5120]** (Gated-MLA output gate), `attn_output`, `attn_norm`.
  **Δ vs GLM DSA:** raon1 has NO indexer tensors (full latent attention, not sparse), ADDS `attn_gate`, and ADDS
  decoupled RoPE (64 dims carried in `kv_a_mqa`, DeepSeek-MLA split). → from `glm_dsa`: drop the indexer/selection,
  add the 64-dim decoupled RoPE on q_b/kv_a, add the sigmoid/silu output gate.
- *MoE FFN (layers ≥1):* `ffn_gate_inp` [2560,128], `exp_probs_b.bias` [128], `ffn_{gate,up,down}_exps`
  [.,.,128], `ffn_{gate,up,down}_shexp`, `ffn_norm`. **Identical shape to GLM** → reuse `router_sigmoid`
  (sigmoid + renorm, 6/128) + shared expert + expert tier UNCHANGED. Dense FFN (layer 0): `ffn_{gate,up,down}` +
  `ffn_norm` → reuse `glm_ffn`. **Activation = `situ`** (situ_beta 4.0 / situ_linear_beta 25.0) — new activation
  in both the dense and expert MLPs (replaces GLM's clamped SwiGLU); confirm exact form vs the model reference.
- *Hyper-connections:* `hc_attn_{base,fn,scale}` [17]/[10240,17]/[4] and `hc_ffn_*` — **identical to GLM** → reuse
  `glm_hc` (4 streams) UNCHANGED.

**Loading:** weights are **Q8_0** (28.8B → ~115 GB at F32, won't fit). The plain `load()` refuses non-F32, so raon1
needs a **pack** (like GLM: dense dequant to F32 + routed experts stay quantized on the native/expert-tier path).
→ extend `tools/iq_pack.py` to accept raon1's tensor layout (the ssm_\*/attn_gate/no-indexer set), producing
`index.txt` + `dense.bin` + native experts; then reuse `Raon1Model::load_pack`.

**Build order (TDD, -Werror, on G:\Strata):**
1. `include/strata/core/raon1_model.hpp`: `Raon1Geometry` (+ `qk_rope`, `situ_beta`, `situ_linear_beta`, `has_mla_gate`
   fields) and `Raon1Model` skeleton; `Raon1Geometry::from_gguf` reads `raon1.*` with the confirmed key mapping.
   RED→GREEN geometry test against the dump. **(first module — this turn)**
2. Adapt the KDA kernel for the full gate + `ssm_rda`; add a RED kernel test vs a reference vector.
3. Adapt the MLA kernel: drop indexer, add decoupled RoPE(64) + output gate; RED test.
4. `situ` activation in `glm_ffn`/experts; RED test.
5. `Raon1Model` forward wiring (hybrid dispatch via `is_recr`, HC, MoE) + pack loader (iq_pack raon1 support).
6. Dispatch `raon1` in `glm_main`; build on G:\Strata; verify logits per-layer vs the model reference (oracle:
   memory `engine-parity-oracle`); then a generate smoke test like GLM's. GPU/speed claims only from the box.

## Status
Arch + geometry + **full tensor-level spec** confirmed against the real GGUF (2026-10-25); GLM base proven
end-to-end on G: (smoke test). Architecture decided: **separate `Raon1Model` reusing the GLM kernel library**, GLM
untouched. Implementation NOT a config tweak — the RDA gate+rda, MLA gate+RoPE+no-indexer, `situ` activation, and a
raon1 pack (Q8_0) are real work (implementer-lane sized).

## Authoritative reference + validation oracle (ik_llama.cpp PR #2733, found 2026-10-25)
**`ikawrakow/ik_llama.cpp` PR #2733 (Bl4ckyou) "Model: add Raon-1 mini preview runtime + DFlash2"** is a complete,
upstream raon1 implementation — so raon1 ALREADY RUNS there. It is both the exact spec AND the validation oracle
(build that PR, run the Q8_0 GGUF, diff logits per layer against our port). Diff saved at
`scratchpad/raon1_pr.diff`. New graph builder `src/graphs/build_raon1.cpp`; activation in `llama-build-context.cpp`;
RDA gates in `llama-delta-net.cpp`/`llama-kda.cpp`. Confirmed from the source:

- **SiTU-GLU (exact):** `situ(x, β) = ggml_softcap(x, 1/β, β) · sigmoid(x) = β·tanh(x/β)·sigmoid(x)`; the GLU output is
  `situ(gate, situ_beta=4.0) · situ(up, situ_linear_beta=25.0)`. Used in the dense FFN, the shared expert, and the
  routed experts. This is NOT GLM's clamped-SiLU swiglu — raon1's hand-rolled CUDA FFN/expert sites need a `situ`
  variant (β·tanh(x/β)·sigmoid(x) per branch, two different β), not a parameter tweak.
- **Gated-MLA = NoPE, absorbed (corrects the earlier "decoupled RoPE" wording):** `raon1_mla_attention` uses
  `wk_b`/`wkv_a_mqa`/`wv_b` (absorbed). `head_k = qk_nope(128) + qk_rope(64) = 192`, `n_nope = head_k - n_rot = 128`,
  `n_extra = n_rot = 64`. The 64 `qk_rope` channels are concatenated as **extra pass-through K/Q channels and are
  NOT rotated** (PR: "7x NoPE gated MLA"; comment: "real extra K/Q channels"). KV-cache row width = `kv_lora + n_extra
  = 576`. Sigmoid **output gate** (`attn_gate`). **No indexer.** So our geometry VALUES (qk 192, qk_rope 64,
  qk_nope 128, v_head 128; `qk_nope == v_head`) are correct; only the word "RoPE" was wrong — it's NoPE.
- **RDA (gated delta-net):** 3 separate q/k/v 1-D convs (kernel 4); low-rank decay `ssm_f_a`/`ssm_f_b`; a full-rank
  output gate `ssm_g`; gates from `ssm_beta` + a delta from `ssm_rda`: `erase = softplus(-beta)`,
  `write = softplus(-(beta+delta))` (so `write/erase = exp(logsigmoid(beta+delta) - logsigmoid(beta))`, making the
  standard delta rule exact). The gates pre-scale `v_conv`, then the existing fused delta-net scan runs
  (`S' = S(I - erase·k kᵀ) + write·v kᵀ`). NB: `ssm_g` (RDA) and `attn_gate` (MLA) are the same logical gate tensor,
  named per layer type.
- **MoE:** sigmoid router + renormalize (`expert_weights_norm`) + one always-on shared expert; `situ` activation.
- **Converter key semantics (confirmed):** `head_count_kv[i] = 0 if (i+1)∈rda_layers else 1`; `key_length =
  kv_lora + qk_rope (576)`; `value_length = kv_lora (512)`; `key_length_mla = qk_nope + qk_rope (192)`;
  `value_length_mla = v_head (128)`; `rope.dimension_count = qk_rope (64)`; `ssm.time_step_rank = rda_decay_rank`.

## Feasibility + completion reality (read the real forward, 2026-10-25)
Reading `strata_glm`'s forward changed the plan. The GLM decode forward is **`src/core/glm_fast_path.cu` (7,368
lines)** + `glm_model.cu` (3,848) — a heavily optimized pipeline (EXL3 / native-quant / Q8_1 expert paths,
multi-GPU split, CPU lane, KV streaming). raon1's deltas are **pervasive, not 3 localized functions**:
- `situ` activation would replace the clamped-SwiGLU (`g.swiglu_exp`) threaded through ~12 expert/FFN sites
  (`native_gu_rows_split`, `moe_gate_up`, `x3_down_prep`, `swiglu_q8`, the EXL3 helper, dense + shared paths).
- the MLA deltas (output gate, decoupled RoPE, no indexer) sit inside the DSA path; the KDA deltas (full gate +
  `ssm_rda`) inside the recurrent path; both thread through the state + pipelining (`is_recr` at ~15 sites).

**Revised architecture (supersedes "separate Raon1Model"):** EXTEND `Glm5Model` to accept arch `raon1`, with every
raon1-specific branch guarded by an `is_raon1` flag that is false for GLM (so GLM's proven path is byte-identical
and cannot regress). This reuses the entire forward + loader + expert tier instead of cloning 7k lines — matching
the standing "don't reinvent" directive. Build it incrementally (geometry → loader-accepts-raon1 → situ → MLA
deltas → KDA deltas → pack), each guarded and each validated before the next.

**No validated oracle on the box.** raon1 has no upstream engine, and the in-repo `engines/unified` raon1
(`src/kernels/raon1_rda.cu`, `raon1_mla.cu`, dated 2026-10-23) PREDATES and OMITS the `situ` activation + the
metadata confirmed from the shipped GGUF today — so it is partial *reference math* for RDA/MLA, NOT a validated
oracle. Final numerical correctness therefore needs the model's OWN reference outputs (a HF/transformers Raon-1
run, or the official reference), produced on the box and diffed per layer (memory `engine-parity-oracle`). Until
that exists, no raon1 output can be claimed correct — only "it loads and generates".

**Honest scope:** this is an implementer lane (pervasive guarded edits through an optimized 7k-line CUDA forward +
the expert kernels, a raon1 pack, and an on-box parity bring-up), not a single-pass completion. Driven
incrementally; each landed piece is guarded + tested so the proven GLM engine stays intact throughout.

**Build step 1 DONE (2026-10-25):** the geometry module landed and TDD-proven —
`third_party/project-maya/include/strata/core/raon1_geometry.hpp` + `src/core/raon1_geometry.cpp`
(`Raon1Geometry` + `from_gguf`, auto-globbed into `strata_glm`), with `tests/core/raon1_geometry_test.cpp`
(synthetic raon1 GGUF, 20 assertions). RED→GREEN confirmed under MSVC on `G:\Strata`'s toolchain: RED =
`LNK2019 unresolved Raon1Geometry::from_gguf`, GREEN = all pass; `/W4 /WX` clean (−Werror). Geometry members
default to 0/unset so the reader must parse every key (a no-op reader fails the test).
Immediate follow-ups: wire the test into the engine CMake test target (currently proven via a standalone
`cl` compile) and update `docs/AGENT_HANDOFF.md`.

**Build step 2 DONE (2026-10-25): loader accepts raon1 + builds on G:.** `Glm5Geometry` gained raon1 fields +
a `from_gguf` arch-`raon1` branch (reusing the tested `Raon1Geometry` reader), all guarded by `is_raon1`.
`strata_glm.dll` builds on the G: toolchain (`engine/build-glm-g.bat`; the venv's `Scripts\cmake.exe` broke in the
move — use `…\site-packages\cmake\data\bin\cmake.exe`). GLM path untouched.

**Build step 3 IN PROGRESS: SiTU activation.** `glm_situ_glu` kernel added to `glm_ffn.cu`/`glm_ffn.hpp`
(`situ(x,b)=b*tanh(x/b)*sigmoid(x)`, GLU = `situ(gate,situ_beta)·situ(up,situ_linear_beta)`), and wired (guarded
by `is_raon1`) at the 3 reference-path FFN sites in `glm_model.cu` (dense, routed-expert fallback, shared expert).
STILL TODO for situ: `pack_moe_tail`/`pack_moe_tail_device` (the quantized expert path raon1 actually uses with a
pack), and the fused `glm_fast_path.cu` sites (the `glm-fast` decode engine). Then MLA (absorbed, 64-extra NoPE,
output gate, no indexer — the reference path at `glm_model.cu` refuses a rope tail today), RDA (softplus gates +
`ssm_rda`), the raon1 pack, `glm_main` dispatch, `secure_frontend` card, and validation vs the ik_llama oracle.
