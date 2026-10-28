# flint — build blueprint (2026-10-25)

> **Superseded 2026-10-27** by `2026-10-27-flint-engine-design.md` (the `multiengine_flint` library on Gemma 4's pieces,
> the owner's Q8_0 file, checked against llama.cpp master's flint).

Engine family after raon1. Grounded in flint's public HF config **and now the real GGUF** (confirmed below).

> **PRIORITY (owner, 2026-10-25): flint NVFP4 is now SECONDARY.** The box GPU is an **RTX 4080 Super (Ada, sm_89)**,
> which has **no hardware FP4** (NVFP4/FP4 tensor cores are Blackwell/sm_120). The engine's NVFP4 path is
> software dequant (E2M1→compute; CPU `nvfp4_avx2`, and a dequant-in-kernel on GPU), so the inline-NVFP4 model would
> run only via that slow software path on Ada. The owner will download a **different, non-NVFP4 flint quant**
> (standard GGUF — e.g. Q4_K/Q8_0 — which the engine's normal dequant handles). **raon1 (Q8_0) is the next engine to
> build; flint waits for the replacement model.** The confirmed `flint-nvfp4.gguf` at
> `G:\Strata-data\models\flint_nvfp4\` stays as the arch reference (and for a future Blackwell box).
>
> **The arch findings below transfer unchanged to the replacement model** — flint is flint regardless of quant:
> geometry, the 14-full/35-sliding + no-rope layer pattern, the per-head **attention sinks**, and the sigmoid-MoE
> (256/6 + 2 shared) are identical; ONLY the expert tensor dtype differs (standard GGUF quant instead of NVFP4).

## What flint is (HF `RecursiveSI/flint`, arch `flint`, FlintForCausalLM)
hidden 2048; **49 layers**; 32 attention heads; **4 KV heads (GQA)**; head_dim 128; intermediate 6144; vocab
150144; max_pos **262144**; silu; rms_eps 1e-6.
- **Attention — hybrid, standard-transformer style (NOT GLM's KDA/MLA):** GQA + **RoPE** (theta 10000, no scaling),
  with **14 `full_attention` + 35 `sliding_attention`** layers (`sliding_window` 513). The 14 full-attention layers
  are exactly the **14 no-RoPE layers** (`no_rope_layer_indices` [0,1,5,9,13,17,21,25,29,33,37,41,45,48]).
  **`softmax_type: learnable`.** attention_bias false.
- **MoE — GLM/qwen-shaped:** 256 experts, **6/tok**, moe_intermediate 512, **2 shared experts** (shared ff 1024,
  `expert_shared_count=2` — corrected from the GGUF), **sigmoid router + norm_topk_prob**, `mlp_only_layers` [0,1]
  dense, decoder_sparse_step 1.
- eos ids [150001,150003,150005,150018]; bos 150000.

## Key finding: flint reuses existing machinery; little is new
Unlike raon1 (≈GLM), flint is a **synthesis of pieces we already have** on the native (qwen4exp) engine core:
- **NVFP4 experts — ALREADY SUPPORTED.** `upstream/docs/NVFP4.md`: the engine serves NVFP4 routed experts
  (NVIDIA ModelOpt: 64-value E2M1 blocks + UE4M3 sub-scales + FP32 `weight_scale_2`), via
  `engine/src/kernels/cpu/nvfp4_avx2.cpp` + `GGML_TYPE_NVFP4` (GPU) + an `experts.bin` pack written by
  `tools/nvfp4_convert.py` + `tools/iq_pack.py`. Proven on Qwen3.8-Flash-Next NVFP4. The flint NVFP4 GGUF packs the
  same way (experts.bin holds the FP4 blocks + scales; the GGUF has no room for the scales).
- **MoE — reuse** `router_sigmoid` + `shared_expert` + `s2_expert_grouped` + the expert-tiering facility.
- **Attention — reuse** flash attention (`src/kernels/cuda/dflash2.cu`) + the RoPE kernels for the full-attention
  layers.

## What is genuinely NEW for flint (the build)
1. **Sliding-window attention** (window 513) for 35 of 49 layers — a windowed mask over the flash-attn path.
2. **Attention sinks** (the real meaning of `softmax_type: learnable`) — every layer ships
   `blk.N.attn_sinks.weight` F32[32] = one learned **per-head sink logit** (gpt-oss / StreamingLLM style): an extra
   always-available key whose logit joins the softmax denominator. NOT a temperature. The flash-attn path must add
   the per-head sink to the softmax normalization (llama.cpp added exactly this for gpt-oss — mirror it).
3. The **per-layer hybrid** array (14 full+RoPE-skipped / 35 sliding+RoPE) + the no-RoPE layer set.
4. GQA (32/4) + RoPE wiring + flint geometry; dense MLP on layers 0–1; eos set.

## Plan (into the ONE engine)
1. Registry: `ModelFamily::Flint` (arch `flint`), RED→GREEN in `tests/core/model_family_test.cpp` (done with this
   doc).
2. Build the flint model graph on the **native engine core** (it already has flash-attn + NVFP4 experts + MoE +
   expert tiering), adding the sliding-window mask, learnable softmax, and the hybrid/no-RoPE layer pattern. Likely
   a family path like qwen35's (a `strata_flint` lib switched in), reusing the native MoE/NVFP4/flash kernels.
3. NVFP4 packing: run the owner's flint NVFP4 GGUF through `tools/nvfp4_convert.py` + `tools/iq_pack.py` to get the
   pack + `experts.bin` (confirm the GGUF is ModelOpt-NVFP4 like the Qwen3.8 case).
4. Verify per-layer against the HF reference (flint has no engine; the NVFP4 kernels + flash/MoE are already
   parity-tested). GPU/speed only from the owner's box.

## Design detail (implementation level)

**Family placement.** flint is its own model graph (a `strata_flint` family, switched in like `strata_qwen35`),
built on the native engine core (prefill, KV cache, sampler, expert tiering), NOT the GLM/Maya DLL — its attention
is standard GQA, not GLM's KDA/MLA. It reuses the vendored sigmoid-MoE kernels (`router_sigmoid`, `shared_expert`,
`s2_expert_grouped`) and the existing NVFP4 expert path.

**Geometry** (confirmed from `flint.*` GGUF metadata, 2026-10-25, `G:\Strata-data\models\flint_nvfp4\flint-nvfp4.gguf`,
1264 tensors): n_layers 49, n_embd 2048, n_head 32, n_head_kv 4, head_dim 128 (`key_length=value_length=128`),
n_ff_dense 6144, n_vocab 150144, ctx 262144, rms_eps 1e-6, rope_base 10000. MoE: 256 experts, 6 routed + **2 shared**
(`expert_shared_count=2`), moe_ff 512, shared_ff 1024, dense layers {0,1} (`leading_dense_block_count=2`), sigmoid
(`expert_gating_func=2`) + renorm (`expert_weights_norm=True`), scale 1.0. **Per-layer arrays are in the GGUF:**
`flint.attention.sliding_window_pattern` and `flint.attention.rope_pattern` — both 49-entry bool arrays, **identical**,
`False` (= full attention / no rope) at layers **{0,1,5,9,13,17,21,25,29,33,37,41,45,48}** (14 layers), `True`
(= sliding-window 513 / rope) on the other 35. So the full-attention set == the no-rope set, as designed.

**Attention kernel (the main new work).** A GQA flash-attention over the KV cache with, per layer:
- **Window:** a `window` parameter — `0` = full attention (14 layers), `513` = sliding-window (35 layers). In the
  flash kernel this is a key-range clamp: key j attends only if `q_pos - j < window` (plus causal). One kernel,
  windowed by a parameter, not two kernels.
- **RoPE:** applied to Q/K except the 14 `no_rope_layer_indices` (which are exactly the full-attention layers).
  Reuse the engine's RoPE (theta 10000, no scaling); skip it on the no-rope set.
- **Attention sinks (`softmax_type: learnable`, confirmed):** each layer has `blk.N.attn_sinks.weight` F32[32]
  (one learned sink logit per head). In attention, the softmax denominator gains `exp(sink_h − max)` — i.e. a
  virtual key with a learned per-head logit, no value contribution. This is the gpt-oss sink; the engine's
  flash-attn (`dflash2`) must take an optional per-head sink vector and add it to the running max/denominator.
  The fixed scale stays 1/sqrt(head_dim). Gate the sink path behind the tensor's presence.
- GQA replication (32 q-heads over 4 kv-heads), standard. Base the dense GQA path on qwen35's dense attention; add
  the window clamp + the learnable scale.

**MoE.** Sigmoid router over 256 experts, top-6, `norm_topk_prob` (renormalize the 6 gates), plus the always-on
shared expert; dense MLP (silu, ff 6144) on layers {0,1}. Reuse `router_sigmoid` + `shared_expert` +
`s2_expert_grouped` + the expert-tiering facility (VRAM/RAM/CPU split) unchanged.

**NVFP4 experts — INLINE in the GGUF (confirmed, corrects the earlier experts.bin assumption).** The flint GGUF
stores experts as **485 `GGML_TYPE_NVFP4` tensors directly** (e.g. `blk.N.ffn_{gate,up,down}_exps.weight` NVFP4)
each with a companion **per-expert F32 `.scale`** tensor (`...exps.scale` [n_expert]). So there is **no `experts.bin`
sidecar and no `nvfp4_convert.py`/`iq_pack.py` step** — the engine reads the NVFP4 expert tensors straight from the
GGUF via the existing `GGML_TYPE_NVFP4` path (GPU) / `nvfp4_avx2` (CPU), with the `.scale` tensor as the per-expert
weight scale. **Box TODO:** confirm the engine's GGUF loader + MoE expert-tiering accept inline NVFP4 expert tensors
(the prior path fed them from `experts.bin`); if the tier expects the sidecar layout, add a thin inline-NVFP4 reader
rather than converting. Dtypes in the file: 485 NVFP4 (experts + likely attn projections), 48 BF16, 731 F32
(scales, norms, per-layer `attn_sinks`, router).

**Switch + run.** Registry `ModelFamily::Flint` (done); `generate.cpp` routes a `flint` `--native`/`--pack` to the
flint family. Verify logits per layer against the HF reference (flint has no engine; the MoE/NVFP4/flash kernels are
already parity-tested). GPU/speed claims only from the owner's box.

## Status
Arch mapped, registry entry in (RED→GREEN), implementation design done, and **geometry + per-layer arrays + the
NVFP4/sink layout now confirmed against the real GGUF** (2026-10-25, `G:\Strata-data\models\flint_nvfp4\flint-nvfp4.gguf`,
1264 tensors). Toolchain restored at **`G:\Strata`**. Two confirmed corrections vs the first draft: experts are
**inline NVFP4** (no experts.bin), and "learnable softmax" is **per-head attention sinks** (`attn_sinks.weight`).
The real build work: the flint attention kernel (sliding-window mask + per-head sinks + the 14/35 full/sliding and
no-rope pattern on a GQA flash path) and confirming inline-NVFP4 reads through the MoE tier. MoE + NVFP4 dtype +
GQA base are reuse. Ready to build on G:\Strata once the owner says go.
