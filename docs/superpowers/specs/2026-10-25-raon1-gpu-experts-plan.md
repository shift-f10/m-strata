# raon1 GPU-expert bring-up plan (Phase E)

Status: **plan only** (owner chose "Both" — tune the tier knobs now, plan the GPU-expert work).
Owner's GPU box: RTX 4080 SUPER (sm_89), 16 GiB VRAM; the host has 96 GiB RAM.

## The constraint

raon-1-mini runs on the glm_pack engine (`strata_glm`, `Glm5Model`). Its MoE experts are **Q8_0**
(6 routed + 1 shared of 128, per layer). The engine forces experts to the CPU:

- `engines/.../project-maya/src/core/glm_model.cu:3617` —
  `dev_experts_ = !fast_mode_ && getenv("STRATA_GLM_CPU_EXPERTS")==nullptr && g_.n_exp_used<=8 && !g_.is_raon1;`
- `:3678` — `if (g_.is_raon1) dev_experts_ = false;` with the note that the GPU native-expert kernel
  (`native_expert_grouped`) is **i-quant-only**, so a Q8_0 expert has no device path.

So today VRAM holds the non-expert layers (attention, RDA, gated-MLA, dense FFN, norms, embeddings) +
the KV cache; the experts sit in pinned RAM and are computed on CPU (`src/kernels/cpu/native_expert.cpp`),
~6 tok/s decode. The four-tier split (VRAM + RAM + storage + CPU) is active and correct for this model
on this box (the ~30 GiB model fits VRAM+RAM, so storage/mmap is not touched); the experts are the
CPU-bound part. Getting them onto the GPU is the decode-speed win.

## Options

**A. Requantize the experts to a GPU-supported i-quant (no CUDA change).**
Repack raon1's experts from Q8_0 to an i-quant `native_expert_grouped` already dots (IQ4_XS / IQ3_S /
IQ2_S — the GLM/qwen4exp expert set in `src/kernels/cpu/u_cpu_iq.*` and the GPU `ggml-cuda` MMQ). Steps:
(1) `engines/maya-ref/tools/iq_pack.py` already has a raon1 arch and a `RAON1_NATIVE_OK` tuple — add the
expert tensors to the native/i-quant set and emit an IQ pack; (2) drop the `!g_.is_raon1` guard at
glm_model.cu:3617/3678 so `dev_experts_` can be true for a raon1 pack whose experts are IQ; (3) keep the
Q8_0 pack as the CPU-path reference. Pros: no CUDA kernel work; experts land in VRAM (the 4080 SUPER's 16
GiB holds many IQ experts) → large decode speedup. Cons: requant error vs Q8_0 (validate logits against
the Q8_0/CPU path and KRAFTON's llama.cpp oracle); a second pack to manage.

**B. Add a Q8_0 device path to the native-expert kernel (CUDA change, box).**
Give `native_expert_grouped` a Q8_0×Q8_1 MMVQ dot (mirror of the recent `u_cpu_mmvq` CPU work, commit
`81e92d28`, which already matched the GPU's Q8_1 activation math). Then `dev_experts_` can be true for the
existing Q8_0 pack. Pros: preserves Q8_0 numerics exactly; no new pack. Cons: real CUDA work under
`-Werror`/sm_89 on the box + GPU-vs-CPU parity testing; VRAM budget (Q8_0 experts are ~2× an IQ4 expert,
so fewer fit resident — the expert-cache tiers the rest to RAM/CPU anyway).

**C. Speed the CPU-expert lane and overlap it with the GPU (no requant, uses existing work).**
The qwen4exp CPU expert lane (commits `cbe74ef`/`81e92d28`: AVX2 i-quant/Q8_1 dots, a pinned worker pool,
a device doorbell that runs missed experts on CPU cores in parallel with the GPU's resident ones) is the
upstream mechanism for exactly this. Wire raon1 into `reg_lane_*` so its Q8_0 experts compute on CPU cores
overlapped with the GPU non-expert work, and vectorize the Q8_0 dot (today's scalar `u_cpu_mmvq` is ~951
µs/expert vs upstream's ~67 µs AVX2). Pros: no numerics change; reuses landed infrastructure. Cons: still
CPU compute (bounded by cores/memory bandwidth), less upside than A/B; the AVX2 Q8_0 dot is new code.

## Recommendation

**A first** (fastest path to VRAM experts and the biggest decode win, no CUDA risk), with the Q8_0 pack
retained as the numerical oracle; then **C** (overlap + AVX2) to make the CPU fallback and any RAM/disk
tier fast; **B** only if the owner wants Q8_0 exactness on the GPU. Each step is box-gated: no speed or
parity claim until the owner's box builds it and shows GPU-vs-CPU/KRAFTON-oracle logit parity (per the
project rule — GPU claims come only from the box).

## Validation (every option)

- Logit parity: diff raon1 logits (new path vs the Q8_0 CPU path, and vs KRAFTON's llama.cpp branch at
  `G:\Strata-data\krafton-llama-ref`) per the engine-parity-oracle method; worst-rel within the i-quant
  tolerance already used for qwen4exp (~1e-4).
- Decode tok/s on the 4080 SUPER vs the ~6 tok/s CPU baseline, reported from the box only.
- `-Werror=all-warnings /W4 /WX`, nvcc 13.0, sm_89.
