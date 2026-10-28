# Gemma 4 on the Strata paper's expert tiering, as a family library of the one engine (2026-10-26)

Owner request (2026-10-26): "revisit the gemma4 engine we built and make sure it follows the
G:\Strata\docs\paper\strata-paper.pdf design", then "do it here and incorporate it [into] the multiengine as a
library". Owner decision on the substrate (asked, answered): **Strata core** - the paper's own implementation -
not Maya's GLM fast path. Standing directives: experts run exactly as the paper describes, for every family
(memory: strata-paper-expert-tiering); reuse upstream before writing anything new (use-upstream-not-reinvent).

## 1. The paper's design (Strata-Paper.pdf, sections 3.1-3.6 and findings 4, 8-10)

- **P1 - RAM holds every routed expert** in one pinned (page-locked) arena; on Windows pinned per layer range,
  and a PCIe read never spans two ranges (finding 13).
- **P2 - VRAM holds the dense part, the KV cache and an adaptive expert cache** filling the rest: filled at
  start from a usage profile, then hot experts swapped in during decode without anyone waiting (evict at once,
  admit when the copy lands - finding 10); slots sized in bytes per layer (finding 8).
- **P3 - per layer, three workers at once.** The GPU runs mixer and router and writes the chosen ids into a
  pinned doorbell; a CPU thread spinning on it splits the experts: cached -> the GPU; a share of the misses ->
  DMA copies issued by that CPU thread on the copy engine (the GPU computes its cached experts first, the copied
  ones when they land - finding 9); the rest -> the CPU cores in place in RAM with ggml-cpu's vec_dot; the CPU
  writes its rows into mapped memory and the GPU sums everything.
- **P4 - the whole pass is one captured CUDA graph**, so the host never waits on a synchronizing driver call.
- **P5 - prompts in 2,048-token chunks**: uncached experts copied from the pinned arena over PCIe while the GPU
  computes the previous ones, multiplied on the GPU; the buffers are borrowed from the expert cache.
- (MTP speculation and the SSD n-gram tier are Qwen3.8-Flash-Next's; Gemma 4 26B-A4B has neither.)

## 2. The Gemma 4 engine as it was (engines/gemma4, plans 2-3; mapped 2026-10-26)

| | had | gap |
| --- | --- | --- |
| P1 | one mapped `cudaHostAlloc` per layer (23.5 GiB), experts repacked to an int8 + fp16 plane | per-layer ranges already; not GGUF-native blobs |
| P2 | the same slot count every layer, filled with experts 0..S-1; rebalanced on the compute stream with host syncs (after each prompt chunk, every 8 decoded tokens) | no profile; swaps stall decode |
| P3 | none: cold experts read zero-copy over PCIe by the GPU's own matvec | no doorbell, no CPU lane, no copy-engine DMA |
| P4 | none: ~690 launches a token, host syncs per token | no graph |
| P5 | chunks of <= 1,024; experts dequantized to FP32, cuBLAS SGEMM, read zero-copy; dedicated scratch | no streaming, no overlap |

Its model graph (norms, sliding + full GQA attention, the dense GEGLU FFN beside the MoE, the softmax top-8
router, the soft-capped tied head, the sampler) is validated against llama.cpp (mean KL 0.040, llama.cpp's own
CPU/CUDA floor 0.042) and is kept.

## 3. The design

**Library.** `src/gemma4` is a static library of `multiengine.exe` (`strata::gemma4::run`, the family switch at
the top of `main`, `--families` lists `gemma4`) - the pattern of `strata_qwen35`. Phase 1 moved the engine as it
was: logits bitwise identical to the standalone build over 24 greedy steps.

**Experts through Strata's own pieces**, extended rather than bolted on:

- Blobs are GGUF-native `[gate rows | up rows | down rows]` (`kernels::NativeExpertLayout` / `cpu::NativeFmt`):
  Gemma's fused `ffn_gate_up_exps` slice (704 gate then 704 up rows) followed by its `ffn_down_exps` slice -
  the GGUF's own bytes, no repack (paper 3.6: "keeps every tensor exactly as the GGUF stores it").
- GPU: `native_expert_grouped` (q8_1 activations, llama.cpp's dot products) - extended with a GELU-tanh gated
  activation (GEGLU) beside SwiGLU, and BF16 (type 30) weights for the UD-Q8_K_XL file's layer 1 and layer 29 down.
- CPU: `ExpertPool::run_split_multi_native` (ggml-cpu `vec_dot`, which has Q8_0 and BF16) - its split buffers
  sized at run time from the layer's `NativeFmt` (they were fixed at Qwen's 2560 / 640: Gemma is 2816 / 704), and
  the same GEGLU variant.
- Host plan: `ExpertDispatch` + `expert_pool_dispatch_multi` + `GpuPlanSink` (VRAM groups, the DMA share issued
  by the dispatch thread with `fetch`, the CPU share), with the process-wide `cpu::ExpertLayout` filled from
  Gemma's GGUF and its row stride taken from the layout instead of the compile-time `H`.
- GPU plan side: the doorbell kernels (`doorbell_publish_res`, `wait_flag_ge`, `copy_i32_from_mapped`,
  `copy_rows_from_mapped`, `moe_hit_add`) in Verifier's post-layer order; Gemma's combine applies the router
  weight times `ffn_down_exps.scale`.
- Arena: an `ExpertSource` over per-layer pinned, mapped ranges filled from the GGUF; the VRAM tier byte-sized
  per layer, profile-filled (`--expert-profile`), swapped on a copy stream with per-layer landing (no host wait).
- Graph: one `CapturedGraph` per decode token; every per-step value (token id, position, ring slot) is device
  data, never a kernel argument (graph.hpp note 2).
- Prompt: 2,048-token chunks; the layer's missed experts streamed through a pinned ring on a copy stream while
  the GPU computes, multiplied with Strata's MMQ (`mmq::Context`, Q8_0) instead of FP32 SGEMM.

Every change to shared code must leave the other families' results bitwise unchanged (checked on Qwen3.6, the
one other MoE model on the box, whose experts run through the same pool and grouped kernel).

## 4. Phases and their gates

| phase | delivers | gate |
| --- | --- | --- |
| 1 | the library in multiengine (done 2026-10-26) | bitwise vs the standalone; host 15/15, kernel 10/10; paths_agree, serve_smoke |
| 2 | P1 native blobs + P2 profile fill, async swaps | KL vs llama.cpp <= its floor; decode never waits on a swap |
| 3 | P3 doorbell, CPU lane (ggml-cpu), DMA share | same; Qwen3.6 bitwise unchanged; tok/s vs phase 1 |
| 4 | P4 one captured graph per token | bitwise vs phase 3 |
| 5 | P5 streamed prompt, MMQ | KL as phase 2; prompt tok/s vs phase 1 |
| - | secure_frontend: gemma4 cards run multiengine.exe | serve smoke through the server |

## 5. Where it stands (2026-10-26, RTX 4080 SUPER 16 GB, PCIe 4.0 x16, 12 pool threads)

- **Phases 1-5 built.** Phase 2: KL 4.8e-4 vs the old engine, the same greedy tokens (the old engine was at
  llama.cpp's floor; the llama.cpp parity itself was not re-run). Phase 3: Qwen3.6-35B-A3B Q8_0 (hybrid mode, 7,287
  CPU-pool entries in 24 steps) bitwise unchanged by the shared-code changes; teacher-forced vs the GPU-only path mean
  KL 5.3e-4. Phase 4: bitwise vs plain launches, lane on and off.
- **Decode** (96 steps after a 64-token prompt, ~52 of 128 slots a layer): 58.0 tok/s (lane + graph) vs 39.5 for
  phase 2; the copy engine's share is 0 by default here (every share above it was slower: the pool reads RAM faster
  than the link copies).
- **Prompt** (2,048 tokens): 577 tok/s at 512-token chunks, 1,381 at 1,024, 2,204 at 2,048, against 133-178 for the
  phase-2 path (the grouped kernel reading the misses over PCIe in place, once per tile of entries) and 860-1,100 for
  the old standalone engine. Every layer's misses stream: the Q8_0 layers multiply on MMQ, the BF16 ones (1, 29) on
  the grouped kernel from the ring. Grouped vs MMQ on a real 293-token prompt: KL 4.2e-3, the same next token.
- **Closed the same day (entry i of the handoff):**
  - P5's borrowed buffers: a streamed prompt's chunk rows, MMQ buffers and ring are carved from the cache's top slots
    for the prompt (106 slots at 512-token chunks, 233 at 2,048) and refilled after it as the tier's swaps (Strata's
    prefill borrow; `--no-prompt-borrow` is the A/B arm). Slots a layer 54 / 53 instead of 51 / 45; a 2,048-token
    prompt 704 / 2,192 tok/s.
  - P2's profile fill: `--expert-profile` fills each layer's slots from Strata's `profile.bin`;
    `--expert-profile-save` writes what the tier learned (on exit, and between requests every 10 minutes at most).
    A cold 48-step decode: 90.0 tok/s from a learned profile, 43.2 without (the same workload).
  - A prompt's routing reaches the tier before decode (one more rebalance after the prompt): decode after a
    one-chunk 2,048-token prompt 71.6 tok/s instead of 52.6.
- **Kept as Strata keeps it:** the copy engine's share is a fixed `--pcie-frac` (Strata's default `pcie_num`; its
  fitted `PcieModel` is opt-in, STRATA_PCIE_BALANCE, and needs per-layer GPU timestamps). Every non-zero share was
  slower on this box, so wiring the fit would only confirm 0 here.
