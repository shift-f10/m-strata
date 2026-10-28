# raon1 fast-path port blueprint — "work how the paper describes"

Goal: run raon1 on `strata_glm`'s **fast path** (`fast_mode_`) — the captured-CUDA-graph, fused-kernel,
expert-prefetch, CPU-lane, **DFlash-speculation** pipeline that GLM-5.3-Flash and (upstream) Qwen3.8-Flash-Next
run on, per `G:\Strata\docs\paper\Strata-Paper.pdf`. Today raon1 is forced to the reference `step_layers`
path (`glm_model.cu:3134` `fast_mode_ = … && !g_.is_raon1`); it already uses the paper's **core hybrid**
(GPU i-quant expert cache + CPU misses, IQ4_XS, ~39 tok/s), but not the fast path.

## The correctness oracle
The reference `step_layers` path produces CORRECT raon1 output (verified: "Paris", math, etc.). Every phase
below is validated by: fast-path output == reference-path output on the SAME greedy prompt (bit-for-bit token
ids), and finally per-seam logit parity vs KRAFTON's llama.cpp (`G:\Strata-data\krafton-llama-ref`,
`GLM_CB_DIR`). `-Werror`, nvcc 13.0 sm_89, measured on the owner's box only.

## raon1 vs glm5-next — the deltas the fast path bakes in (what to port)
The fast pipeline is `Glm5Model::fast_layers` (glm_fast_path.cu:5432) / `fast_token` (:5621). glm5-next math
is hardcoded at:
1. **Activation — SiTU vs SwiGLU.** Fast path uses `g.swiglu_exp` (clamped SwiGLU) in `gf::moe_gate_up`
   (:5412), `x3_down_prep` (:2269/:2507), the CPU lane `native_gu_rows_split` (:2291/:2461), the shared
   expert (:5320), and the helper (:4405). raon1 needs `situ(gate,situ_beta)*situ(up,situ_linear_beta)`,
   `situ(x,b)=b·tanh(x/b)·sigmoid(x)` (already in iq_kernels `situ_entries_kernel` for the native GPU expert
   path, and CPU `native_set_situ`). Port: a situ variant of each fast activation site, selected by
   `g.is_raon1` (or `g.situ_beta>0`).
2. **Hyper-connections — Helmert vs Sinkhorn.** Fast path: `gf::hc_post` (:5612) + `h.iters =
   g.sinkhorn_iters` (:5483), comb width `(2+hc)·hc=24`. raon1: Helmert comb, width `2·hc+(hc-1)²=17`,
   pre=sigmoid (no eps), post=magnitude·sigmoid (the reference `raon1_hc_gates`/`raon1_hc_mix`). Port: a
   raon1 HC path in the fast pre/comb/post.
3. **RDA mixer vs GLM KDA.** Fast path: `kda_all` (:6478) + the fused kda kernels (conv/gate/recurrence).
   raon1 RDA: `v *= exp(softplus(-beta)-softplus(-(beta+delta)))` (delta from ssm_rda), full-rank `ssm_g`
   out-gate (not g_a/g_b), L2-norm q/k, silu after conv, log-decay `lower_bound·sigmoid(-ssm_a·x)`. Port:
   raon1 RDA variants of the fast kda kernels (the reference path's raon1 RDA branch is the spec).
4. **Gated-MLA vs GLM MLA.** Fast path: `gf::mla` (:5220) with the sparse indexer. raon1: absorbed NoPE MLA,
   64 qk_rope channels concatenated un-rotated (kv row 576 = kv_lora 512 + 64), sigmoid `attn_gate` output
   gate, NO indexer, scale 1/√(qk_nope+qk_rope). Port: a raon1 MLA variant (the reference `raon1_mla_attention`
   / `raon1_build_qcur` / `raon1_gate_mul` are the spec); the fast path must skip indexer setup for raon1.

## Phasing (each builds + validates against the reference path before the next)
- **P1 SiTU** in the fast FFN/MoE kernels (most pervasive, lowest risk). Validate: with `fast_mode_` still off,
  a unit/compile check of the situ kernels; integration validated once P5 enables fast_mode_.
  **DONE 2026-10-26 — compiles clean on G: (BUILD_EXIT 0, no new warnings; strata_glm.dll copied to build-g/).**
  - GPU kernels: `fast_glu_act` (glm_fast.cu) + `batch_glu_act` (glm_batch.cu) — situ if `situ_bg>0`, else
    byte-identical SwiGLU. Threaded `situ_bg/situ_bu` through `swiglu_q8`/`moe_gate_up_kernel` (via `MoeDev.situ_bg/bu`,
    set in glm_fast_path.cu `fill`), `swiglu_f16`/`swiglu_rows`/`swiglu_rows_h16` (glm_batch.cu + glm_prefill.cu).
    Betas = `g.is_raon1 ? g.situ_beta/situ_linear_beta : 0.0f`, so non-raon1 is provably unchanged.
  - Guard: the fused MMQ `run_glu` write-back only knows SwiGLU, and `glu_on` defaults ON — so `glu` in
    glm_prefill.cu now also requires `!g.is_raon1`, keeping raon1 on the separate run+`swiglu_rows` (SiTU) path.
  - **CPU lane: already covered, no edit.** `lane_experts`/`win_cpu_merged` take the native arm for raon1
    (IQ4_XS loads as `NativeFmt`, so `Ly.exl3==false`) → `native_gu_rows_split` → `gu_activate`, which reads the
    file-scope situ betas set once at load by `native_set_situ(g_.situ_beta, g_.situ_linear_beta)` (glm_model.cu,
    runs regardless of fast/reference mode). The `x3_down_prep` exl3 arm is GLM-only, not raon1-reachable.
  - **Why no box run at P1:** `fast_mode_` stays gated off for raon1 until P5, so every P1 edit is dormant for
    raon1 now (raon1 reference uses step_layers, not these fast/prefill_half kernels); and for the models that DO
    use them (GLM/qwen) the change is byte-identical by construction (situ=0, `!is_raon1` keeps run_glu). Isolated
    reference-vs-fast token parity on the box is only meaningful at P5 once all 4 math pieces are in.
- **P2 Helmert HC** in the fast pre/comb/post.
  **DECODE DONE 2026-10-26 (compiles clean).** raon1's Raon-mHC is a Helmert construction (mix_dim 17), not
  glm5-next's Sinkhorn (fused hc_kernel, HC_MIX 24). Rather than fuse Helmert into those kernels, the decode read
  half is un-fused for raon1 and reuses the validated reference kernels: new `gf::raon1_hc_read` (pre-gated mix +
  rms_norm + q8) + `raon1_hc_gates` (Helmert gates) + the shared `gf::hc_post` (write) + a bf16 `gf::mv` for the fn
  projection; branched `if (g.is_raon1)` at both decode HC sites in glm_fast_path.cu. GLM path untouched.
  **P2 COMPLETE 2026-10-26 (all HC surfaces, compiles clean).** Shared `gf::raon1_hc` (un-fused Helmert) drops into
  decode (glm_fast_path.cu), window (glm_window.cu `hc_rows`) and rows (glm_rows.cu, `batch` forced off for raon1 so
  the per-row gf::hc path is used); prefill (glm_prefill.cu `hc_read`) uses a new batched `gb::raon1_hc_finish` (N=17
  fn projection + Helmert gates + the shared mix/norm tail). Writes (`hc_post` / `gb::hc_update`) are shared.
  GLM/qwen untouched. Compile-only until P5. CONFIRMED: P3/P4 span the same decode/window/rows/prefill spread.
- **P3 raon1 RDA** in the fast kda kernels.
  **DECODE + PREFILL done 2026-10-26 (compiles clean); WINDOW + ROWS deferred.** raon1 RDA = glm5-next KDA + two
  deltas: a V pre-scale (`raon1_rda_vscale`, delta = ssm_rda·x) and a full-rank `ssm_g` gate (vs factored g_a/g_b).
  Added Ly.ssm_rda (BF16) + Ly.ssm_g (Q8_0 WSlot) to the fast Layer + loading; `kda_prep` drops its g2 quarters when
  g_b is null (raon1 gets g2 from ssm_g). Wired in decode (glm_fast_path.cu) and prefill (glm_prefill.cu). Window +
  rows share `kda_token`, which still needs delta+g2 threaded through + the vscale — deferred (only hit by
  speculative/batched decode; basic P5 greedy = decode+prefill is covered). Compile-only until P5.
- **P4 raon1 gated-MLA** in `gf::mla` (+ skip the indexer for raon1).
  **DECODE + PREFILL done 2026-10-26 (compiles clean).** Made the fast setup raon1-aware (guard indexer loads, fix
  kv_a_mqa width, add attn_gate WSlot + MLA scratch + a gf::rms_norm helper + BF16-weight build_qcur/mla_attention
  variants); the F32 kw-wide latent cache was already allocated. fast_dsa (decode) and the glm_prefill.cu non-recr
  block (carve_dsa widened) now run raon1's full-attention gated-MLA (NoPE, absorbed, sigmoid gate, no indexer,
  reference kernels) and bypass the DSA indexer/KV-stream. WINDOW + ROWS MLA deferred with their KDA (speculative/
  batched only). Compile-only until P5; ensure KV streaming is off for raon1 when fast_mode_ is enabled.
- **P5** drop the `!g_.is_raon1` guard on `fast_mode_`; wire **DFlash** (the draft GGUF
  `dflash-raon-1-mini-preview-Q8_0.gguf`; `dflash_on_` needs `fast_mode_`). Build, validate fast==reference
  (greedy, bit-exact tokens), logit parity vs KRAFTON, measure tok/s.

## Risks / open questions
- The captured CUDA graph assumes fixed per-layer shapes; raon1's RDA/MLA buffer sizes must be stable across
  the verify window. The fast expert prefetch ("one layer early", :5276) assumes glm5-next routing — confirm it
  holds for raon1's sigmoid router (6 routed + 1 shared).
- The CPU lane (`lane_experts`/`win_cpu_merged`) must apply SiTU (the lane uses `native_gu_rows_split` +
  `x3_down_prep` with `g.swiglu_exp`) — P1 must cover the lane, not just the GPU kernels.
- DFlash draft/target vocab + the reduced head: confirm raon1's DFlash draft is compatible, else speculation
  stays off and raon1 gets the fast path without the 1.6–1.8× (still a win over the reference path).
- MTP: raon1 has `nextn 0` (no MTP head); DFlash is its only speculation route.

## Payoff
Captured-graph + fused kernels + (if the draft is compatible) DFlash speculation → the paper's full treatment;
plausibly ~39 → ~65+ tok/s. If the port proves too entangled with glm5-next's fused kernels, the fallback is
the reference path at ~39 tok/s, which already uses the core expert-cache hybrid.
