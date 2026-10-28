# Flint engine — what it should be (design notes)

> **Superseded 2026-10-27** by `2026-10-27-flint-engine-design.md`: flint is built as the `multiengine_flint` family
> library on Gemma 4's pieces and the owner's Q8_0 file, not on `engines/unified` or the NVFP4 file. The architecture
> findings here were re-checked there (one correction: the license, see PROVENANCE's flint section).

Date: 2026-10-25. Author: opencode worker (Linux, no GPU, no model weights —
see `AGENT_HANDOFF.md` §"Owner decisions, 2026-10-25").
Status: **recon + notes only** — no engine code yet. The owner's order
(chat, 2026-10-25): "flint is a different model family … Ideally we want the
nvfp4 because it is native cuda." These notes record what the flint engine
must be, what was proven on this host, and what waits for the owner's box.

## 1. What flint is

- **RecursiveSI/flint** ("Recursive Flint", Hugging Face, fetched 2026-10-25):
  an Apache-2.0 MoE decoder for coding agents. ≈39.8B total, ≈2.5B active,
  262,144 ctx, vocab 150,144. Quantized GGUFs for NVIDIA (NVFP4) and Apple.
- GGUF artifacts: `unsloth/flint-GGUF` (standard GGML ladder: UD-IQ1_M,
  UD-IQ2_XXS, UD-Q4_K_XL ≈25.1GB, …, Q8_0, BF16) and
  `RecursiveSI/flint-nvfp4-gguf` — single `flint-nvfp4.gguf`, 22.9GB, NVFP4.
- **Not** a Project Maya derivative (unlike glm5next/ds4/raon1). Its reference
  implementation is **llama.cpp master** (`MODEL_ARCH.FLINT`, GGML NVFP4).
  Vendor it read-only like `engines/maya-ref`? — owner decision (D6).
- GGUF architecture string: **`flint`** (so `general.architecture == "flint"`).

## 2. Architecture truth (HF `config.json`, fetched 2026-10-25)

49 layers; `mlp_only_layers: [0, 1]` (2 dense FFN, then 47 MoE;
`decoder_sparse_step: 1`). hidden 2048; 32 Q heads / 4 KV heads;
head_dim 128 (key_length = value_length = 128); dense FFN 6144; MoE FFN 512;
shared expert FFN 1024; 256 experts, top-6; `router_score_function: sigmoid`,
`norm_topk_prob: true`; rms eps 1e-6; rope theta 10000.
Hybrid attention: `sliding_window: 513`; per-layer `layer_types` = 14
`full_attention` + 35 `sliding_attention`; **`no_rope_layer_indices` =
[0,1,5,9,…,45,48]** — the 14 full-attention layers are **NoPE**, and RoPE
applies exactly to the 35 sliding layers. `softmax_type: learnable` →
**per-head attention sinks** (32 floats/layer). Per-head QK RMSNorm
(Q/K norm, dims 128). No MTP tensors in the GGUF → no drafter weights.

## 3. GGUF truth (parsed from the real file's header bytes on this host)

Method: HTTP Range fetch of the first 17MB of `flint-nvfp4.gguf` (header +
metadata + tensor-info only — no weights run, 21.3 GiB stays on HF). Parsed
with a throwaway script; facts cross-checked against llama.cpp master
`gguf-py` (`constants.py`, `gguf_reader.py`, fetched 2026-10-25).

- GGUF version 3, 1264 tensors, 43 KV pairs. Metadata value type codes are
  **u32** (confirmed from the bytes; llama.cpp master's reader reads u32 too).
  **Our `u_gguf.cpp` already reads u32 type codes — zero reader change needed.**
- Keys (namespace `flint.`): `block_count 49`, `context_length 262144`,
  `embedding_length 2048`, `feed_forward_length 6144`,
  `attention.head_count 32`, `attention.head_count_kv 4`,
  `attention.key_length 128`, `attention.value_length 128`, `rope.freq_base
  10000.0`, `attention.layer_norm_rms_epsilon 1e-6`, `expert_count 256`,
  `expert_used_count 6`, `expert_gating_func 2` (sigmoid),
  `expert_feed_forward_length 512`, `expert_shared_feed_forward_length 1024`,
  `leading_dense_block_count 2`, `expert_shared_count 2`, `expert_weights_norm
  true`, `expert_weights_scale 1.0`, `attention.sliding_window 513`,
  `attention.sliding_window_pattern` **bool[49]**, `attention.rope_pattern`
  **bool[49]** (the pattern array of §2). Plus `general.file_type 39`,
  `general.quantization_version 2`,
  `general.tensor_extra.name` **str[485]** + `general.tensor_extra.prec_a4`
  **bool[485]** (all false; 485 = exactly the NVFP4 weight tensors — a
  per-tensor quantize-options record; treat as opaque metadata).
  Tokenizer: gpt2 BPE, `tokenizer.ggml.pre = "flint"`, 150144 tokens,
  149744 merges, bos 150000 / eos 150001, `add_bos_token false`, chat
  template strips `<think>`. Sampling: temp 1.0, top_p 0.95, top_k 20.
- Tensor types: **NVFP4 = ggml type 40** × 485 (all 2-D/3-D weights incl.
  `output.weight`), **bf16 (30)** × 48 (47 routers `blk.N.ffn_gate_inp.weight`
  (2048,256) + `token_embd.weight` (2048,150144)), **f32 (0)** × 731 (norms,
  `attn_q_norm`/`attn_k_norm` (128), `attn_sinks` (32), and the scale
  sidecars below).
- **NVFP4 layout**: block = 64 elements → 32 B of FP4 nibbles + 4 B of E4M3
  block scales (llama.cpp `(64, 4+32)`); 36 B per 64 el = exactly 4.5 bpw
  (verified against the file: `blk.24.ffn_gate_exps.weight` (2048,512,256)
  spans 150,994,944 B = 4,194,304 × 36). Each NVFP4 tensor has a paired
  **`<name>.scale` f32 sidecar** in the tensor directory — per-tensor (1,) or
  per-expert (256,) for the 3-D packs — the second-level scale.
- Experts are **3-D packed per layer**, same convention as glm5next:
  `ffn_gate_exps (2048,512,256)`, `ffn_up_exps (2048,512,256)`,
  `ffn_down_exps (512,2048,256)` (+ (256,) scale each). Shared expert:
  one 2-D `ffn_{gate,up,down}_shexp` triple per MoE layer (1024 rows) — note
  the GGUF says `expert_shared_count = 2` but the tensors show one; trust the
  tensors (D3).
- Dense prefix: `blk.0/blk.1` carry `ffn_{gate,up,down}.weight` (6144) +
  scales and no expert tensors (mirrors `leading_dense_block_count 2`).

**Proven on this host (2026-10-25):** feeding the real 17MB header image
through `u::parse_gguf` (caps untouched, `file_size` = 21.3 GiB) gives
`REFUSED: gguf: unsupported tensor type` — the metadata and tensor directory
otherwise pass all current limits. That refusal is the natural **RED** for
host step H1 below.

## 4. What the flint engine should be — mapping to the unified engine

flint needs **no new attention algebra** (no MLA/KDA/GDN): every feature is a
composition of pieces the engine already has, plus three genuinely new ones.

| flint feature | engine source to reuse |
|---|---|
| sigmoid top-6 routing, `expert_weights_norm`, packed 3-D experts, one shared expert | glm5next/ds4 MoE tier (`RouterKind::sigmoid`, `ffn_*_exp`/`_shexp`) |
| per-head QK RMSNorm | qwen35moe family |
| sliding-window attention (window 513) | gemma4 SW path |
| NoPE layers | glm5next NoPE (`RopeKind::nope`) — but **per layer** (new) |
| dense FFN prefix (2 layers) + MoE rest | `leading_dense_block_count` + optional-FFN geometry (qwen4exp pattern) |
| bf16 router, f32 norms | existing loaders/tiers |

New primitives (the flint-specific work):
1. **Per-layer patterns in `Geometry`**: `rope_pattern` +
   `sliding_window_pattern` bool[49] (today rope/SW are family-wide). Guard:
   arrays present, length == block_count.
2. **Attention sinks**: `attn_sinks` f32[head_count] added to the softmax row
   before the max (learnable-softmax); small kernel + geometry entry.
3. **NVFP4 (ggml type 40)**: `TensorType::nvfp4 = 40`,
   `tensor_bytes` (row % 64 == 0; blocks×36 + f32 sidecar), and the CUDA
   kernel. **Hardware note:** the owner's RTX 4080 SUPER is Ada (sm_89) — no
   FP4 tensor cores; NVFP4 there is a *dequant* matvec (the same kernel class
   as our i-quant paths). "Native cuda" FP4 MMA needs Blackwell (sm_100/120)
   and is later work. The 22.9GB file fits the 32GB card: weights ≈21.3 GiB +
   f16 KV ≈28KB/token on the 14 full layers (SW layers capped at 513) ⇒
   ~131K ctx fits; full 262K needs Q8_0 KV on the full layers (D2).
4. **Tokenizer**: BPE with `pre = "flint"` + `<think>`-stripping template —
   engine vs secure_frontend ownership (D5).

## 5. Staged plan

- **H1 — DONE (2026-10-25, host)**: `FamilyId::flint` + dispatch row `"flint"` +
  `Geometry` guard row (broad ranges, sigmoid + rope) + `TensorType::nvfp4 = 40`
  + `tensor_bytes` (row%64==0, 36 B per 64-elem block) + `u::k::Quant::nvfp4`
  (id + size math only; the device dot is C1). TDD: RED = compile refusal
  (`nvfp4`/`flint` not members) + the pre-H1 probe's runtime refusal; GREEN =
  new host case `gguf_reads_nvfp4_tensors` + flint geometry/dispatch assertions,
  suite 11/11. **The real 17 MB header image of the actual NVFP4 file now
  parses end-to-end** (probe `/tmp/opencode/flint_probe2.cpp`, image
  `flint-nvfp4-header2.bin`): 43 metadata keys, 1264 tensors — 485 type-40,
  731 f32 (the `.scale` sidecars), 48 bf16 — every byte math passes incl.
  `output.weight` 2048x150144 = 172,965,888 B; geometry ACCEPTED. The dump
  settles the open key questions: `expert_gating_func` is **u32 2** (not f32),
  `expert_shared_count = 2` and `expert_shared_feed_forward_length = 1024` and
  `leading_dense_block_count = 2` all present, `attention.rope_pattern` +
  `attention.sliding_window_pattern` are **bool[49]** arrays (rope_pattern is
  NOT all-1 — some full layers are NoPE), expert `.scale` sidecars are f32
  dims {256} (per-expert) for 3-D expert stacks, dims {1} elsewhere, and
  `general.file_type = 39` parses (reader does not gate it). Adjustment to the
  plan text: the two pattern arrays are NOT fields of the shared `Geometry`
  (the guard validates common dims only); the flint graph loader reads them
  fail-closed in H2, exactly the raon1 `head_count_kv` precedent.
- **H2 — DONE (2026-10-25, host)**: host NVFP4 reference `engines/unified/src/
  kernels/nvfp4_ref.hpp` ported 1:1 from ggml master (cited file:line inside):
  `block_nvfp4` = **4 UE4M3 sub-scale bytes FIRST + 32 E2M1 nibble bytes**
  (ggml-common.h:221-227), the doubled `kvalues_fp4` table (:1124-1128),
  `ggml_ue4m3_to_fp32` with its raw×0.5 convention + 0x00/0x7F zero sentinels
  and `ggml_fp32_to_ue4m3` (ggml-impl.h:512-567), and the sub-block element
  mapping low-nibble=e[s*16+j] / high-nibble=e[s*16+8+j] (ggml-quants.c:589-
  611). Host case `nvfp4_block_reference_matches_ggml_master` pins exact
  values: all 16 E2M1 codes, UE4M3 corners (subnormal 2^-10, raw-1.0 = 0x38,
  max 0x7E = 224, 0x7F-zero), f32_to_ue4m3 round-trip, and the element map —
  all exact binary math (the doubled table × halved scale cancels). **C1's
  device kernel must match these numbers bit for bit.** TDD: RED =
  `u::nvfp4` undeclared; GREEN = host suite 12/12 cases Debug+Release zero-
  warning. Two test-authoring bugs found and fixed during the loop (a
  double-counted ½ in the expectation, a nibble-interleave mistake in the
  synthetic block) — the reference itself was right against ggml the first
  time. Layout-order comments in u_gguf/common.cu corrected (scales first).
- **C1 (box, CUDA)**: NVFP4 dequant/matvec kernels (reference: llama.cpp
  master ggml NVFP4, MIT).
- **C2 (box)**: `flint_graph.cu` — hybrid attn (SW/full, per-layer NoPE,
  sinks, QK norm) + dense prefix + MoE tier reuse.
- **C3 (box)**: load the real 22.9GB file, parity vs llama.cpp, then perf.
  Spec decode later (no MTP tensors; drafter = separate decision).

## 6. Decisions for the owner

- **D1** first file: NVFP4 (recommended — the owner's stated preference, and
  the only single-file flint under ~25GB) vs unsloth's ladder (Q4_K_XL 25.1GB
  etc.).
- **D2** KV/context budget at first load-up: f16 KV capped ~131K vs Q8_0 KV
  for full-attention layers to reach 262K.
- **D3** `expert_shared_count = 2` vs one shexp tensor per layer: engine
  trusts tensors; report upstream.
- **D4** NVFP4 on sm_89 = dequant path (accepted; native-FP4 Blackwell path
  deferred).
- **D5** tokenizer ownership (engine `pre flint` vs frontend layer).
- **D6** vendor llama.cpp (read-only, like `maya-ref`) as the flint/NVFP4
  reference, or keep citing it from the web?
