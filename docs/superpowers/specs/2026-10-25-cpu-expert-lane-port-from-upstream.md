# CPU expert lane — port from upstream, don't reinvent (2026-10-25)

Owner directive (2026-10-25): build our lightweight engines FROM upstream's proven source, reuse ggml where
it already solves a thing, consolidate the shared pieces across engines, and only test/tune/optimize AFTER the
proven baseline works — never before. Applies to every unified-engine family except gemma4. This note is the
grounded understanding of upstream's CPU expert lane (read, not guessed) and the plan to port it, replacing the
hand-written reinventions (`u_cpu_iq`, `u_cpu_iq_avx2`, `u_cpu_mmvq`).

## How upstream's CPU expert lane works (src/kernels/cpu/, include/strata/kernels/cpu/)

**NativeFmt (`native_expert.hpp`, built by `expert_layout.cpp::native_fmt`)** — one layer's geometry: the two
ggml types (`gu_type`, `d_type`), their `vec_dot_type` activation formats (`gu_act`, `d_act`), `n_embd`/`n_ff`,
row-byte strides (`gu_row`, `d_row`), blob offsets (`up_off`, `down_off`), activation byte sizes, plus optional
NVFP4 tail and Hadamard-rotation fields. The expert blob is the three GGUF slices back to back:
`[gate rows | up rows | down rows]`.

**Quantize the activation** — `native_quant_act(f, x, dst)` turns x[n_embd] into the gate/up activation in
`gu_act` (ggml `from_float`, Hadamard-rotated first if the layer is folded); `native_quant_h(f, h, dst)` the
same for the down input in `d_act`. For the qwen4exp set `gu_act` = Q8_K (the 256-block i-quants) and `d_act`
= Q8_0 (IQ4_NL / Q2_0). **Q8_K/Q8_0, not Q8_1 — upstream's CPU does not match its GPU's Q8_1 bit for bit, and
does not try to.**

**Compute the rows** — the pool splits an expert's rows across threads and calls:
- `native_gu_rows(f, blob, act[], nt, ff[], r0, r1)`: `ff[t][r] = silu(gate_r . act[t]) * (up_r . act[t])`.
- `native_down_rows(f, blob, hq[], nt, out[], r0, r1)`: `out[t][r] = down_r . hq[t]`.

Both DISPATCH by type + ISA + token count (`nt`), to upstream's OWN tuned multi-token kernels, with ggml-cpu as
the universal fallback:
- i-quant gate/up (IQ2_XXS/IQ2_XS/IQ2_S/IQ3_XXS/IQ3_S/IQ4_XS): `iq512_gu_rows` (AVX-512) / `iq256_gu_rows`
  (AVX2) — `iq_avx2.cpp` / `iq_avx512.cpp`. Decode each 32-value chunk's weights ONCE per window, every token
  applies them (~2.0-2.4x ggml-cpu at 3+ tokens). `iq128_gu_rows` (AVX1) for old CPUs.
- IQ4_NL down: `iq4nl256_down_rows` (AVX2, Q8_0 acts).
- Q2_0 down (Strata-custom type 42): `q2_0_gguf_rows_multi_avx2` (`q2_avx2.cpp`).
- else / single token / no AVX2: ggml-cpu `ggml_get_type_traits_cpu(type)->vec_dot(n, &s, 0, row, 0, act, 0, 1)`.
- `#152` rule (`native_gu_mt_min`): a multi-token kernel only from `mt_min` tokens up, so a lone token's rows
  do not depend on how many tokens share its expert (bit-stability), ggml's per-token dot below that.

**The pool (`pool.cpp` `ExpertPool::run_split_multi_native`, `expert.cpp`)** — workers + the calling thread
(`host_works`) drain a batch through three phases per group of experts: phase 5 gate/up rows
(`native_gu_rows`), quantize the intermediate h (`native_quant_h`), phase 6 down rows (`native_down_rows`).
Epoch-tagged claim, spin-then-sleep, one worker per physical core.

**The GPU side (`core/verify.cpp`, `kernels/cuda/*`)** — the device plans resident/skip/CPU/PCIe per layer,
rings a mapped doorbell (ids/x/seq), the host serve loop runs the pool for the layer's CPU experts into mapped
`ymiss`, raises a flag, and the GPU's `copy_rows_from_mapped` merges those rows. (We already have our own
equivalents: `expert_lane_plan` / `lane_doorbell` / `lane_merge` + `reg_lane_serve`.)

## Port plan (reuse, adapt, keep, delete)

REUSE (copy upstream files into our tree, adapt the `strata::kernels::cpu` namespace + `strata/...` include
paths; MIT, Niko1221 + the Strata contributors / ggml authors — PROVENANCE):
- `iq_avx2.cpp` + `iq_avx2.hpp` (+ `iq_avx512.cpp`, `iq_avx1.cpp` for the other ISA tiers, optional first cut).
- `q2_avx2.cpp` (Q2_0).
- `expert_layout.cpp` (NativeFmt / `native_fmt` / `cpu_avx2_ok` / ISA probes / act-quant dispatch) + header.
- `expert.cpp` (`act_quant_q8_1`, the q8k quant) + header.
- `native_expert.cpp` (the dispatch) + header.
- `pool.cpp` (`ExpertPool`) + header — or keep our `LanePool` and have it call `native_gu_rows`/`native_down_rows`.
- Link ggml-cpu (the forked build with Q2_0; already builds on the box — the engine links it green) for the
  `vec_dot`/`from_float` fallback + quantizers.

KEEP (ours, the GPU orchestration): the device `expert_lane_plan` / `lane_doorbell` / `lane_merge` (`u_iq.cu`),
the tier serve loop (`reg_lane_serve` in `u_experts.cu`), the qwen4exp wiring. They drive upstream's CPU pool
instead of hand-written dots.

DELETE (reinventions of ggml/upstream): `u_cpu_iq.*`, `u_cpu_iq_avx2.cpp`, `u_cpu_iq_tables.inc`,
`u_cpu_mmvq.*`, and the hand dots inside `u_cpu_lane.*`. Keep only the pool/orchestration if not replaced by
upstream's `pool.cpp`.

ORDER (owner's rule): port → build → prove token stream on the box (accept the non-bit-exact split, as upstream
does) → test/tune (pcie-frac, workers) → optimize only if needed. Then generalize the same reuse to
glm5next / ds4 / raon1 / flint and consolidate the shared code. gemma4 stays its own.
