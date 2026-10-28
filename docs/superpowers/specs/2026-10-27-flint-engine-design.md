# flint as a family library of multiengine, on the Strata paper's recipe (2026-10-27)

Owner, 2026-10-27: "next engine we need to work on is flint, model is located in F:\models\flint_bf16. Again we make
sure it follow the same recipe as the rest, based on the strata paper at G:\Strata\docs\paper". Standing directives:
experts run as the paper describes, for every family (memory: strata-paper-expert-tiering); reuse upstream before
writing anything (use-upstream-not-reinvent); each family is its own library (the 2026-10-27 multiengine layout).

This supersedes `2026-10-25-flint-engine-design.md` and `2026-10-25-flint-port-blueprint.md`, which targeted the NVFP4
file and the retired `engines/unified` substrate; their architecture findings are re-checked below against the BF16
file.

## 1. The model (read from the owner's file)

`F:\models\flint_bf16\flint-BF16-0000{1,2,3}-of-00003.gguf` (unsloth's BF16 export of RecursiveSI/flint "Flint
Preview", 75 GB; shard 1 is metadata only, shards 2-3 hold 97 + 682 tensors). GGUF arch **`flint`**.

| | |
|---|---|
| layers | 49: 2 dense (`leading_dense_block_count` 2, FFN 6144) + 47 MoE |
| width | hidden 2048; attention 32 query heads, **4 KV heads** (GQA 8:1), head dim 128 (q proj 2048 -> 4096) |
| attention | per-head **QK RMSNorm** (128); **per-head attention sinks** (`attn_sinks` F32[32]); RoPE base 10000 on the 35 **sliding-window** layers (window **513**, llama.cpp's "standard" rule: query p sees key j when p - j < 513); the 14 **full-attention** layers {0,1,5,9,...,45,48} have **no RoPE** (`sliding_window_pattern` == `rope_pattern`) |
| MoE | 256 routed experts, top 6, **sigmoid** router (`expert_gating_func` 2) with the top 6 renormalized (`expert_weights_norm`), scale 1.0; expert FFN 512 (separate `gate`/`up`/`down` stacks); **one** shared expert of width 1024, added to the routed sum with no gate (`expert_shared_count` 2 in the metadata, one tensor triple in the file - llama.cpp builds the one) |
| activation | SiLU (SwiGLU) everywhere |
| head | `output_norm` + separate `output.weight` (not tied); vocab 150,144 |
| tokens | gpt2 BPE, pre-tokenizer `flint`; bos 150000, eos 150001, 4 end-of-generation ids; sampling 1.0 / 0.95 / 20 |
| context | 262,144 |
| weights | everything BF16 except the F32 norms and sinks; **routed experts 75.7 GB**, the dense part ~3.9 GB |
| no | MTP head, DFlash drafter, biases, logit softcap, router bias (`exp_probs_b`) |

The graph is llama.cpp master's `src/models/flint.cpp` (MIT; read at `e86f5fd1`, 2026-10-27): pre-norm blocks;
attention = `attn_norm` -> q/k/v -> QK RMSNorm -> RoPE where `rope_pattern` says -> attention over the KV cache
(sliding or full) with the sink -> `attn_output` -> residual; FFN = `ffn_norm` -> dense SwiGLU (layers 0-1) or sigmoid
MoE + shared expert -> residual. Neither upstream Strata (v0.2.6), Project Maya (v1.1.30) nor the pinned llama.cpp
(3cf03257) implements flint. Exact points read from that source:

- RoPE is **NeoX** style (`LLAMA_ROPE_TYPE_NEOX`: element i pairs with i + 64 of the 128-wide head), base 10000, no
  scaling, applied after the QK RMSNorm.
- The window rule (`llama-hparams.h`, `LLAMA_SWA_TYPE_STANDARD`): a key is masked when `query_pos - key_pos >= 513`, so
  a sliding layer's query sees itself and the 512 keys before it.
- The sink (`ggml_compute_forward_soft_max_f32`): scores scaled by 1/sqrt(128) (and masked); `max = max(max score,
  sink_h)`; the denominator adds `exp(sink_h - max)`; the sink is not scaled and has no value.
- flint is one of the four architectures whose MoE down projection rescales its input by a power of two
  (`build_amax_src1_f16`, `[TAG_MOE_AMAX_SCALE]`): the SwiGLU output can be large enough that a q8_1 block's FP16 sum
  of 32 values overflows. Exact in floating point (a power of two), so it changes no result where nothing overflows;
  the engine must keep the expert hidden activations out of FP16 or rescale the same way (checked in F2/F3).

## 2. The recipe (the paper, as Strata's HOW_IT_WORKS.md and the Gemma 4 design state it)

- **VRAM:** the dense part (attention, routers, shared expert, head), the KV cache, and an **adaptive expert cache**
  filling the rest - filled at start from a usage profile, then moved toward the most-routed experts during decode
  without anyone waiting.
- **RAM:** every routed expert, **pinned**. The **CPU computes the experts the GPU does not hold, in place, while
  the GPU computes the cached ones** (a doorbell per layer; a share of the misses can be DMA'd by the copy engine).
- **One captured CUDA graph per decode step.**
- **Long prompts in big chunks**, each layer's uncached experts streamed over PCIe while the GPU computes, buffers
  borrowed from the expert cache.
- **Guess, then check:** drafts verified by the big model (flint has no MTP; Strata's prompt-lookup drafting, kept
  only where its measured acceptance pays, is the paper's other drafter).
- The conversation is kept between requests (only new tokens are read).

Gemma 4 (`engine/src/gemma4`, `docs/superpowers/specs/2026-10-26-gemma4-paper-tiering-design.md`) already runs this
recipe on the Strata core's own pieces, and flint has the same skeleton (sliding + full attention layers, routed +
shared experts, QK norm). **flint is built as Gemma 4 was**, as `multiengine_flint.dll`.

## 3. What is reused, and what is new

Reused unchanged (Gemma 4's frozen core copy, `engine/src/gemma4/core`, compiled into the flint library as its own
objects, so the two libraries share source but no binary; a core file flint would have to change is copied into
`src/flint/core` first, so Gemma 4 never moves):

- P1 pinned expert arena per layer range; the expert layout over split `gate`/`up`/`down` stacks
  (`expert_layout_set`, the native split base).
- P2 `ExpertCache` + the profile fill (`--expert-profile`) and save (`--expert-profile-save`), swaps on a copy stream.
- P3 the CPU lane: `ExpertDispatch` + `expert_pool_dispatch_multi` + `GpuPlanSink`, the pinned doorbell, the pool's
  ggml-cpu dots (`ExpertPool::run_split_multi_native`; ggml has Q8_0 and BF16), the copy-engine share.
- GPU experts: `native_expert_grouped` (q8_1 activations, llama.cpp's dots; Q8_0 and BF16 weights, SwiGLU).
- P5 prompt chunks with the miss ring, Q8_0 experts on Strata's MMQ, BF16 ones on the grouped kernel.
- The serve loop shape and `Engine` interface of `g4_run.cpp` / `g4_engine.hpp` (the frontend's protocol).

From Gemma 4's family code, adapted (flint's own files under `src/flint/`, `f_*`):
- the model guard + tensor binding (`g4_model` -> `f_model`: flint's fixed shape, the two pattern arrays read
  fail-closed), the GGUF reader and file mapping (`g4_gguf`, `g4_file`, unchanged in substance), the CLI.
- the engine (`g4_engine.cu` -> `f_engine.cu`): layer loop, KV cache with a ring for sliding layers, the captured
  decode graph, the MoE tier wiring, rebalancing, top candidates.

Genuinely new (flint's kernels, `f_kernels.cu`, each checked against llama.cpp's graph):
1. **Attention with sinks**: the softmax of each head gains the head's sink logit - max over (scores, sink), the
   denominator adds exp(sink - max), no value for it; scores scaled by 1/sqrt(128), the sink not.
2. **GQA 32:4** at head dim 128 with QK RMSNorm (Gemma 4's heads are 256/512 wide).
3. **Per-layer RoPE / window from the GGUF arrays**: rotate only where `rope_pattern`, the 513-key window only where
   `sliding_window_pattern` (Gemma 4 rotates every layer with two bases and a 1024 window).
4. **Sigmoid router + renormalized top 6 + an ungated shared expert** (Gemma 4: softmax-style router with a per-expert
   scale and a GEGLU expert); the 2 dense SwiGLU layers.
5. The head: `output_norm` + untied `output` (no softcap); flint's end-of-generation ids.

New in the launcher: `general.architecture == "flint"` -> `multiengine_flint` (`flint_main`), with Strata's process
steps (as Gemma 4). New in the frontend (later phase): a flint card, profile and its chat template.

## 4. The expert format (owner's decision D1)

The recipe pins **every** routed expert in RAM. In BF16 that is 75.7 GB on a 96 GB machine whose Windows commit limit
already capped Maya's RAM tier 12 GB below free RAM: it does not fit pinned. Options:

- **D1-A (recommended): a Q8_0 flint GGUF made once from the BF16** by our own offline tool (`tools/flint_q8.py`:
  gguf-py reads the BF16 shards, ggml's Q8_0 rounding for the routed experts and the large matrices, routers, norms
  and sinks kept as they are, the metadata copied; written to a folder the owner picks). Routed experts 40.2 GB -
  pinned in full; ~25% of them fit the VRAM cache (vs ~13% in BF16); Q8_0 is near-lossless (8.5 bits a weight) and
  every piece of the tier already runs it (the raon1 and Gemma 4 precedent).
- **D1-B: run the BF16 file as it is**: exact weights, no conversion; the paper's low-RAM mode (RAM holds only what
  the GPU does not, the rest read from the SSD), roughly half D1-A's decode speed at best.
- **D1-C: a smaller quant from the BF16** (Q4_K / IQ4_XS): faster, lossy; better left until D1-A is verified.

Rough decode bound on this box (6 x 47 experts a token; RAM ~60 GB/s, PCIe 4.0 ~25 GB/s): BF16 moves 1.8 GB of experts
a token, Q8_0 0.94 GB.

## 5. Verification: the official graph as the oracle (owner's decision D2)

Fast-equals-our-reference says nothing about the real model (raon1's lesson). The oracle is **llama.cpp master's
flint**, built on this box like the raon1 oracle (`G:\Strata-data\krafton-llama-ref`): a ~100-line program over its
libraries writes the last N positions' logits and the per-layer seams (`cb_eval`). That needs llama.cpp master's
source (a source archive of roughly 30 MB, MIT), placed beside the other references (`G:\Strata-data\flint-llama-ref`);
its `llama-quantize` could also write D1-A's file, but our own tool keeps the conversion independent of the oracle.
Gates: teacher-forced logits on a real prompt - mean KL and top-1 against the oracle at the BF16 weights, and the same
against the oracle running our Q8_0 file (the quant's own cost measured separately from the engine's); the per-layer
seams to find the first wrong layer when it fails.

## 6. Phases (each ends checked on this box; TDD with RED evidence for every kernel and rule)

- **F0** the inputs: the oracle (D2), a token file from flint's tokenizer, and - when it lands - the owner's Q8 file
  (D1; its tensor types read and checked against what the tier runs).
- **F1** `src/flint` skeleton: model guard + binding (RED: the guard refuses a mutated header), the library
  (`flint_main`), the launcher route and `--families` (`flint`); `--check` prints the geometry.
  **Done 2026-10-27:** `f_model` (the guard over any number of shards, gguf-split names), `f_cli` / `f_profile` /
  `f_engine.hpp` (Gemma 4's, at flint's numbers), `f_run.cpp` (`flint_main`), the engine a stub that refuses to load;
  `f_host_tests` 11/11 (RED recorded for the model cases: 4 of 5 failing on a stub); the launcher routes `flint`;
  `--check` passes on the Q8_0 file and on the BF16 set (named by its second shard).
- **F2** a plain forward (no tiering, experts read in place, no graph): per-layer seams and logits against the
  oracle. The kernels of section 3's "new" list, each with a unit test against a double-precision host reference.
  **Done 2026-10-27, together with F3 and F4** (Gemma 4's engine carries the tier, the graph and the prompt stream, so
  flint's adaptation brought them at once): `f_kernels` (8 GPU cases, RED recorded, GREEN; the RoPE case's reference
  computes the angle as ggml-cuda does, in float, with a stated float-angle tolerance), `f_lane`, `f_engine.cu`, test-only
  seam dumps (`STRATA_FLINT_SEAMS`). On the Q8_0 file against the oracle (CPU, f16 KV, the engine's own cache type):
  25-token chat prompt + 199 forced decode steps - **mean KL 0.0113, top-1 98.0%** (llama.cpp's own f32-vs-f16 KV:
  0.0178 / 97.0%; its 16-layers-on-GPU run: 0.0188 / 97.5%); 1,511-token prompt (three chunks, MMQ stream, ring
  wrap-around, window) + 63 forced steps - **mean KL 0.0043, top-1 100%**. Layers 0-15 agree to cosine >= 0.9996; the
  later spread is near-tie routing flips amplified by the model's massive activations (dim 996 ~70 from layer 15, dim
  1355 in the thousands by layer 46), within llama.cpp's own spread. Graph vs plain launches: bit for bit over 200
  steps. Speed (RTX 4080 SUPER, 74 of 256 experts a layer in VRAM, 37.45 GiB pinned): decode 49 tok/s (45 at 1.5K
  context; 31.6 with the CPU lane off), prompt 406 tok/s at 1,511 tokens (329 without MMQ), load 17.5 s.
- **F3** the paper's tier: pinned arena, VRAM cache + profile, CPU lane, copy share - logits unchanged within the
  CPU lane's known rounding (Q8_1 MMVQ vs ggml-cpu Q8_0, measured), then speed. **Done (see F2).** The copy-engine
  share keeps Gemma 4's default (0) and the decode rebalance Gemma 4's 8/32: flint's own tuning is open.
- **F4** the captured decode graph (bitwise against plain launches) and the chunked prompt path (MMQ + ring).
  **Done (see F2).**
- **F5** guess-then-check: Strata's prompt-lookup drafting, kept where measured acceptance pays. **Not started.**
- **F6** the frontend: a flint card (profile, chat template, sampling 1.0 / 0.95 / 20), served end to end. **Done
  2026-10-27** (secure_frontend README "flint profile"): `EngineProfile::flint` on the `strata_0143` wire with the
  `strata_flint` argument allowlist; `TokenizerMode::flint_bpe` reproduces llama.cpp master's flint pre-tokenizer as
  it actually runs (its three regexes in turn over the collapsed text, where every `\p{L*}` class is one letter class,
  so case changes do not split words) - token-exact on 38 texts, and the plan id-exact on 9 chats rendered by llama.cpp's
  own Jinja engine (`engine/tests/flint/oracle/flint_chat.cpp`); `render_flint_text_prompt` is the GGUF's template
  byte for byte with its `flint_text` escapes; thinking off writes `<think></think>` after `<|assistant|>`.
  `engine/fe-config/cards/flint-q8.json` serves in the multi-test server: prompt token counts equal llama.cpp's, both
  thinking modes answer "391" to 17 x 23, a stream sends reasoning then content.

## 7. Decisions

- **D1 (owner, 2026-10-27): the owner downloads a Q8 flint GGUF** ("I'll download a Q8 instead, keep working on the
  rest of the code until I give you the path"), then: "the model is now in F:\models\flint-Q8_0" -
  `F:\models\flint-Q8_0\flint-Q8_0.gguf`, one file of 42.3 GB, unsloth's Q8_0 (made with an imatrix,
  `general.file_type` 7): every matrix Q8_0 (routed experts 40.2 GB, attention, dense FFN, shared expert, embedding,
  head), the routers BF16, norms and sinks F32. No conversion tool. The owner may delete the BF16 copy (asked
  2026-10-27; the parity gates run the oracle on the same Q8_0 file).
- **D2 (owner, 2026-10-27): the oracle download is approved.** llama.cpp master `e86f5fd1e9f584b0fcec71d7da9a92de44e8bcbf`
  (2026-10-27), shallow clone at `G:\Strata-data\flint-llama-ref`, built CUDA sm_89 with shared libraries.
- Stated, not asked: flint shares Gemma 4's frozen core source and copies a core file before changing it.
