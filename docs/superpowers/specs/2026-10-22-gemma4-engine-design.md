# Gemma 4 engine path (`gemma4` family) and frontend profile

Date: 2026-10-22. **Status: DECISIONS ACCEPTED; plan 1 (the frontend profile) and plans 2-3
(the engine, `engines/gemma4/`, with the reconciliations R1-R7 of
`docs/superpowers/plans/2026-10-22-gemma4-plan2-3-engine.md`) executed on branch
`gemma4-plan1-frontend`; the engine matches llama.cpp at its own backend floor and serves the
owner's Q8 file behind the 8080 page in expert mode (2026-10-23,
`docs/validation/gemma4/`). Plan 4 (managed manifests, `engine.executables.gemma4`, the gate
opening, the full acceptance battery) is not done.** The binding inputs are the
owner's decisions of 2026-10-22: "Port Gemma 4 into a Strata-style engine path: write the
design spec for a gemma4 family modelled on the Qwen3.6 path, plus the frontend profile",
and "Accept the recommendations for D1-D11 in the Gemma 4 spec" (§11). Where the plans
departed from this text they say so (plan 1's reconciliation list; plans 2-3's R1-R7: no
upstream libraries compiled, cold experts read over PCIe instead of a CPU pool (owner ruling
wanted), llama.cpp as the only reference, a 2,048-row ring with common-prefix reuse,
`--native`, `--kv f32` refused, no `cmake/` wrapper). The model facts were confirmed against the owner's download's
header on 2026-10-22 (§2.6); each further quant's header is read before it is relied on, and
a fact a header contradicts is corrected here and, if it changes a decision, goes back to the
owner.

Inputs, cited by short name:

- **G4** — the target model: `groxaxo/Huihui-gemma-4-26B-A4B-it-abliterated-GGUF`, Unsloth
  "UD" quants of `huihui-ai/Huihui-gemma-4-26B-A4B-it-abliterated`, itself a fine-tune of
  `google/gemma-4-26B-A4B-it`. Its `config.json` and `chat_template.jinja` (read 2026-10-22).
- **LC** — llama.cpp at `3cf03257f219afbe7334045ff7c6a06ac68c627d` (2026-09-20), the commit
  upstream already pins (`upstream/CMakeLists.txt:1564-1566`) and our build fetches
  (`out/engine-cuda-v0.2.2/_deps/strata_llamacpp-src`). It carries a complete Gemma 4
  implementation, which is this spec's **reference**: `src/models/gemma4.cpp` (498 lines),
  the `gemma4` tokenizer in `src/llama-vocab.cpp`, and the converter `conversion/gemma.py`
  (`Gemma4Model`, :633-810).
- **Q36** — upstream's Qwen3.6 path, `upstream/src/qwen35/q36_engine.cu` (3,979 lines) and
  `q36_kernels.cuh` (399), the pattern this engine copies. Line numbers are at upstream
  `v0.2.2` (`011c9e60`).
- **SP6** — [GLM engine design](2026-10-14-sp6-glm-engine-design.md), the precedent for a
  second engine (layout, wrapper CMake, contract table, review items).
- **SCC** — [engine supply-chain contract](../../security/engine-supply-chain.md).
- **TM** — [threat model](../../security/threat-model.md).

## 0. Summary

Upstream Strata cannot load a Gemma 4 GGUF: its only architectures are `qwen4exp` and
`qwen35moe` (`include/strata/artifact/model_family.hpp:21-25`), and `qwen35moe`'s driver
accepts only head width 256 (`q36_engine.cu:971`). This spec adds **our own engine
executable, `strata-gemma4-engine`**, built from a new source tree `engines/gemma4/` that
copies Q36's driver structure and reuses upstream's quantised matvec, GEMM and MMQ libraries
unmodified. It speaks the same stdio protocol as upstream's engine, so the frontend talks
to it through the existing `strata_0143` flavor. The frontend gains a `gemma4` profile: a
renderer for Gemma's template, a second tokenizer mode (SentencePiece-style BPE), Gemma's
thought-channel reasoning split and its stop tokens.

The v1 target is the owner's **UD-Q8_K_XL** file (27.9 GB, D8). Everything but the experts
(2.6 GB) sits in VRAM; the experts (25.2 GB) cannot, so v1 carries over Q36's **hybrid
placement**: every expert in pinned host RAM, a share of them in VRAM slots chosen and
rebalanced by routing counts, and the rest computed by a CPU expert pool (D4). Two
copy-and-modify items follow from that file: the CPU pool's SwiGLU becomes GELU, and the
**BF16** matrices Unsloth kept in layers 1 and 29 need BF16 kernels on the GPU and the CPU.
Estimated size: about 6,000-8,500 new lines across four plans (§1.4), most of it CUDA and
the CPU pool.

## 1. Goals, non-goals, definition of done

### 1.1 Goals

0. **One UI, engines behind it** (owner, 2026-10-22: "the point should be 1 UI, different
   engines to run the models, if anything one unified engine"). Gemma is served by
   `strata-secure-server` and its page on 8080 like every other model; the profile picks
   the engine (§6.1). No second UI or API server is built. A single engine for every model
   is not reachable: the owner's Qwen3.8 Flash Next is `qwen4exp`, which only upstream
   Strata runs, so at least Strata and this engine sit behind the one UI.
1. Run G4's **UD-Q8_K_XL** GGUF (D8) on the owner's box (16 GB RTX 4080 SUPER, 64 GB RAM,
   24 threads) through `strata-secure-server`, text chat, streamed and not, thinking on or
   off, with experts split between VRAM and RAM (§4.2). Smaller UD quants load when their
   types pass §5.5.
2. Output that matches the reference: teacher-forced KL against LC within the same gate
   upstream used for Qwen3.6 (`tools/qwen35/kl.py`: within 1.5x of llama.cpp's own
   backend-to-backend floor), and greedy agreement over a fixed prompt set (§9.3).
3. Keep every frontend property: the engine stays a supervised child with private stdio, the
   closed environment, the Job / process group, no network (§8); the frontend renders and
   tokenizes, the engine sees token ids only.
4. Windows (MSVC v143 as the nvcc host) and Linux, NVIDIA only, like SP6.

### 1.2 Non-goals (v1)

| Non-goal | Why |
| --- | --- |
| Images (`mmproj-*.gguf`, the 27-layer vision tower, `GENI`) | a second large port; text first (§11 D2) |
| Streaming experts from SSD (upstream's main-path `expert_source.cpp`) | the Q8 experts (25.2 GB) fit in RAM; hybrid VRAM + RAM is in scope (§4.2), SSD is not |
| MTP / speculative decoding, `--batch` slots | the 26B GGUF has no MTP block (§2.6); LC's drafter is a separate `gemma4-assistant` arch; the frontend refuses `--batch` today |
| Tools / function calls (`<|tool>`, `<|tool_call>`, `<|tool_response>`) | the frontend's extensions stay disabled (EXT-01) |
| Gemma 4's other sizes (E2B, E4B with per-layer embeddings and KV-shared layers; 31B dense) | only the 26B-A4B shape is loaded; the loader refuses `embedding_length_per_layer_input > 0`, `shared_kv_layers > 0` and a block count other than 30 (§4.1) |
| Any change under `upstream/` | project rule |
| A Python server, a catalog entry, a pack builder for the release | plan 4 adds catalog entries only if the owner asks (§11 D9) |

### 1.3 Definition of done

1. Plans 1-3 merged with their hermetic tests green on `windows-msvc` (and the Linux and
   cross gates the owner runs).
2. On the owner's box: the parity gates of §9.3 pass for the chosen quant(s), results
   committed under `docs/validation/gemma4/`.
3. Plan 4 merged and the acceptance battery (§9.4) run through `strata-secure-server` and
   the page. Only then does the handoff say "Gemma 4 verified on <GPU>, <OS>".

### 1.4 Four plans

| Plan | Where it runs | Contents |
| --- | --- | --- |
| **1 — frontend profile** ([plan](../plans/2026-10-22-gemma4-plan1-frontend-profile.md), written 2026-10-22) | dev host, hermetic | `gemma4` profile end to end on the fake engine: config/manifest/launch enums, the `spm_bpe` tokenizer mode (C++ and the Python pack tool), the renderer, the thought-channel split, stop tokens, the reasoning mapping, page wording (§6, §7) |
| **2 — reference engine** | dev host builds; box runs | `engines/gemma4/` tree and wrapper CMake (§3), the loader and geometry (§4.1), a straightforward CUDA forward pass (`strata-gemma4-ref`, like upstream's `strata-q36ref`), the logit-dump interface and the LC-side dump tool (§9.2) |
| **3 — fast engine** | box | the decode and prompt kernels (§5) including BF16 (§5.5), hybrid expert placement and the GELU CPU pool (§4.2, §5.4), graph capture, prefix reuse, the serve loop (§4.7); parity re-run against plan 2's reference and LC |
| **4 — integration** | box | launch composition, argument profile, the acceptance battery, docs (README, threat model, provenance, handoff) |

## 2. The model

### 2.1 Shape (G4 `config.json`, `text_config`)

| Field | Value |
| --- | --- |
| layers | 30: layers 5, 11, 17, 23, 29 (0-based) full attention, the rest sliding (`layer_types`; LC writes `gemma4.attention.sliding_window_pattern`) |
| hidden | 2816 (= 11 x 256) |
| heads | 16 query heads on every layer |
| KV heads | 8 on sliding layers, 2 on full layers (`gemma4.attention.head_count_kv` is a per-layer array) |
| head width | 256 sliding (`key_length_swa`), 512 full (`key_length`) |
| K/V projection | `attention_k_eq_v`: full layers have no `attn_v`; V is computed from the K projection (§2.3) |
| sliding window | 1024 |
| RoPE | sliding: NEOX, base 10,000, 256 dims; full: "proportional", base 1,000,000, see §2.4 |
| FFN | every layer: dense GELU MLP, width 2112, plus the MoE block (§2.3) |
| experts | 128, top-8, width 704, softmax gating renormalised over the picks, a per-expert output scale |
| norms | RMSNorm, eps 1e-6, weights used as stored (LC `norm_shift` 0.0) |
| activation | GELU-tanh (`gelu_pytorch_tanh`) |
| vocab | 262,144; embeddings tied (no `output.weight`, §2.6) |
| context | 262,144 trained |
| final logits | soft-capped: `30 * tanh(x / 30)` |
| attention scale | 1.0 (no 1/sqrt(d); LC `gemma4.cpp:11`) |
| EOS | ids 1 (`<eos>`) and 106 (`<turn|>`) |

### 2.2 Tensor names (LC `llama-arch.cpp`, `gemma4.cpp:49-140`)

Per layer `blk.N.`: `attn_norm`, `attn_q`, `attn_k`, `attn_v` (sliding layers only), `attn_q_norm`
[256 or 512], `attn_k_norm`, `attn_output`, `post_attention_norm`, `ffn_norm`, `ffn_gate`,
`ffn_up`, `ffn_down` [dense 2112], `post_ffw_norm_1`, `pre_ffw_norm_2`, `ffn_gate_inp` and
`ffn_gate_inp.scale` [2816], `ffn_gate_exps` + `ffn_up_exps` (or fused `ffn_gate_up_exps`),
`ffn_down_exps` and `ffn_down_exps.scale` [128], `post_ffw_norm_2`, `post_ffw_norm`,
optional `layer_output_scale` [1]. Global: `token_embd`, `output_norm`, `rope_freqs` [256].

In G4's UD-Q8_K_XL file (§2.6) the forms are: experts **fused** as
`ffn_gate_up_exps` [2816, 1408, 128] only; `attn_v` on sliding layers only (full layers:
`attn_k` [2816, 1024], `attn_q` [2816, 8192], `attn_output` [8192, 2816]); no
`output.weight` (tied); `layer_output_scale` present on every layer; `ffn_gate_inp` is F32
[2816, 128]; all norms and scales F32.

### 2.3 Forward pass (LC `gemma4.cpp:154-436`, the 26B-A4B subset)

For each token, with `x` the residual stream:

1. `x = embed(token) * sqrt(2816)`.
2. Per layer `l`:
   - `h = rms(x) * attn_norm`.
   - `q = rms_head(h @ Wq) * q_norm`, then RoPE (§2.4).
   - `k0 = h @ Wk`. `v0 = h @ Wv` on sliding layers, `v0 = k0` on full layers.
   - `k = RoPE(rms_head(k0) * k_norm)`; `v = rms_head(v0)` (no weight). The cache keeps
     both: K and V differ after their norms even where the projection is shared.
   - Attention, scale 1.0, causal; sliding layers see the last 1024 positions only. GQA:
     16 query heads over 8 (sliding) or 2 (full) KV heads.
   - `a = rms(attn @ Wo) * post_attention_norm`; `x1 = x + a`.
   - Dense branch: `d = rms( down( gelu(gate(rms(x1)*ffn_norm)) * up(...) ) ) * post_ffw_norm_1`.
   - Router: `logits = Wr @ ( rms(x1) / sqrt(2816) * gate_inp.scale )` (no learned norm
     weight); softmax over 128; top-8; weights renormalised to sum 1.
   - Experts on `rms(x1) * pre_ffw_norm_2`: for each pick `e`,
     `w_e * down_e( gelu(gate_e(.)) * up_e(.) ) * down_exps.scale[e]`; summed, then
     `rms(.) * post_ffw_norm_2`.
   - `f = rms(dense + experts) * post_ffw_norm`; `x = (x1 + f) * layer_output_scale` (scale
     1 when absent).
3. `logits = 30 * tanh( (rms(x) * output_norm) @ token_embd^T / 30 )`.

### 2.4 RoPE

- **Sliding layers:** NEOX over 256 dims (`rope.dimension_count_swa`), base 10,000 (`rope.freq_base_swa`),
  `theta_i = pos * 10000^(-2i/256)`, pairs `(i, i+128)`.
- **Full layers ("proportional", LC `conversion/gemma.py:734-761`):** the GGUF declares
  `rope.dimension_count = 512` and stores `rope_freqs` = 64 ones then 192 values of 1e30. NEOX
  over 512 dims pairs `(i, i+256)`; `theta_i = pos * 1e6^(-2i/512) / rope_freqs[i]`, so
  pairs 0-63 rotate and the rest (factor 1e30) stay put. That is HF's `cc000000ss000000`
  layout: 128 rotated dims whose frequencies are computed over the full 512. A kernel that
  rotates "the first 128 dims with exponent -2i/128" is **wrong** for this model. The
  engine reads `rope_freqs` and applies it; it does not hard-code 64.

### 2.5 Tokenizer and chat format

- **Tokenizer** (`tokenizer.ggml.model = "gemma4"`, LC `llama-vocab.cpp:528-537, 637-644,
  2110-2234, 3197`): BPE with ranked `merges` over **raw UTF-8 characters** (no GPT-2
  byte alphabet); a normaliser replaces each space with `▁` (U+2581); the pre-tokenizer
  only splits runs of `\n` from everything else (`[^\n]+|[\n]+`); a newline-only run that
  is itself a vocabulary token is that token; a character the merges cannot reach falls
  back to `<0xXX>` byte tokens; `add_bos` is always true; no space prefix. The converter marks
  `<|channel>`, `<channel|>`, `<|tool_call>`, `<tool_call|>`, `<|tool_response>`,
  `<tool_response|>`, `<|"|>` USER_DEFINED (`gemma.py:645-654`).
- **Chat template** (the GGUF's `tokenizer.chat_template`, 12,045 chars, lines 141-266 for
  the text path; confirmed 2026-10-22). No newline anywhere except those shown:
  ```
  <bos>
  <|turn>system\n[<|think|>]{system text | trim}<turn|>\n      (only with a system message or thinking on)
  <|turn>user\n{text | trim}<turn|>\n
  <|turn>model\n{strip_thinking(earlier answer)}<turn|>\n           (strip_thinking also trims)
  <|turn>model\n                                                    (generation prompt)
  <|channel>thought\n<channel|>                                       (only with thinking off)
  ```
  `<|think|>` is written only when thinking is on (template lines 161-162) and is followed
  directly by the system text. `trim` is Jinja's (Python `str.strip()`: leading and trailing
  Unicode whitespace); the renderer reproduces it exactly. Thinking on: the model writes
  `<|channel>thought\n…<channel|>` and then the answer. There are no effort levels;
  `enable_thinking` is on or off, default off.
- **Ids** (this GGUF): `<pad>` 0, `<eos>` 1, `<bos>` 2, `<unk>` 3, `<mask>` 4, `<|think|>` 98,
  `<|channel>` 100, `<channel|>` 101, `<|turn>` 105, `<turn|>` 106, `\n` 107, `\n\n` 108,
  `▁` 236743; tool markers 46-52, image/audio/video markers 255999-258884. 24 tokens are
  CONTROL (3) or USER_DEFINED (4); 256 `<0xXX>` byte tokens; 514,906 merges. The renderer and
  stop rules find every id by its literal (exactly once, of the expected type), never by number.

### 2.6 What the GGUF header confirmed (2026-10-22)

Read header-only (metadata and tensor directory, no weights) with a stdlib parser of our
own from the owner's download `C:\Strata-data\models\gemma4_q8\gemma-4-26B-A4B-it-UD-Q8_K_XL.gguf`
(27,866,180,544 bytes; GGUF v3, 658 tensors, 58 keys; `general.name` "Huihui Gemma 4 26B A4B
It Abliterated", `general.license` apache-2.0 with Google's Gemma 4 terms linked):

- **Confirmed as written in §2.1-§2.5:** `general.architecture = gemma4`; `block_count` 30;
  `embedding_length` 2816; `feed_forward_length` 2112; `head_count` 16; `head_count_kv` per
  layer (8, and 2 on full layers); `key_length`/`value_length` 512, `_swa` 256;
  `sliding_window` 1024 with the pattern of §2.1; `rope.dimension_count` 512 and `_swa` 256;
  `rope.freq_base` 1e6 and `_swa` 1e4; `rope_freqs` [256] F32; `expert_count` 128,
  `expert_used_count` 8, `expert_feed_forward_length` 704; `final_logit_softcapping` 30;
  `layer_norm_rms_epsilon` 1e-6; `shared_kv_layers` 0; `embedding_length_per_layer_input` 0;
  tied embeddings; no MTP tensors; `tokenizer.ggml.model = gemma4`, `add_bos_token` true,
  `add_space_prefix` false; `general.sampling` top_k 64, top_p 0.95, temp 1.0 (the model
  card's defaults, which `defaults.sampling.*` can mirror).
- **This file's types:** Q8_0 for 255 matrices, F32 for 392 small tensors, and **BF16 for
  11 matrices**: all of layer 1 (attention, dense MLP and both expert tensors) and layer
  29's `ffn_down` and `ffn_down_exps` (Unsloth's "UD" upcast of sensitive layers). Every
  704- and 2112-wide input is Q8_0 or BF16, both 32-block, so §5.5's rule holds.
- **Sizes** (header arithmetic, `gguf_sizes.py`): experts 25.22 GB; dense, attention and
  norms 1.85 GB; `token_embd` (also the tied head) 0.78 GB; 27.85 GB in all, of which BF16
  2.15 GB. One expert is about 6.6 MB in Q8_0 (gate+up+down), about 12.4 MB in layer 1's BF16.
- **Consequences:**
  - **This file is the v1 target** (owner, 2026-10-22: "no use the q8"; D8). It cannot sit
    in 16 GB of VRAM, so v1 places experts in VRAM and RAM (§4.2; D4 reversed), and its
    BF16 matrices need BF16 kernels on the GPU and in the CPU pool (§5.5). It is also the
    model LC runs for the reference side of the parity gates (§9.2).
  - Other UD quants mix types per tensor; any further quant's header is read before it is
    relied on.
  - Gemma's merges include runs of `\n` and `\t` (`'\n\n…\n \n'`), so a line-based
    `merges.txt` cannot hold them (§6.2).

## 3. Source layout and build

### 3.1 Layout

```
engines/gemma4/              ours: the engine (not a submodule; reviewed like the frontend)
  src/main.cpp               CLI -> run()
  src/g4_engine.cu           driver: load, place, session, forward, serve (from Q36's shape)
  src/g4_kernels.cuh         attention, norms, RoPE, router, GELU expert kernels
  src/g4_layout.hpp          geometry_from_gguf for gemma4 and the tensor table (our own guard)
  tools/g4_llama_dump.cpp    reference side: LC's logits/layers (test-only, never shipped)
  tools/g4_parity.py, g4_kl.py   parity gates (after tools/qwen35/parity.py, kl.py)
cmake/gemma4-engine/         standalone wrapper project (SP6 §3.3 pattern)
docs/validation/gemma4/      parity and acceptance results
```

### 3.2 Build (`cmake/gemma4-engine/CMakeLists.txt`)

Upstream declares `project(strata)` with global target names, so the engine cannot be added to
the root project next to `upstream/` (SP6 §3.3). The wrapper is its own project and:

1. Requires the CUDA toolkit, compute capability 7.5 or newer, MSVC v143 on Windows, and
   `SECURE_STRATA_G4_GGML_DIR` = a llama.cpp checkout at `3cf03257` (offline, as SP6).
2. Seeds upstream's cache (`STRATA_ENABLE_CUDA=ON`, **`STRATA_NATIVE_EXPERTS=ON`** — without
   it there is no MMQ and no ggml checkout, Q36 report §A — `STRATA_GGML_DIR`,
   `STRATA_BUILD_TESTS=OFF`, the arch list) and `add_subdirectory(${repo}/upstream upstream
   EXCLUDE_FROM_ALL)` **unmodified**, for its libraries only: `strata_kernels`,
   `strata_prefill`, `strata_mmq`, `strata_kq`, `strata_kernels_cpu` (the base the GELU/BF16
   CPU pool copy builds beside, §5.4) and the header-only `strata_artifact`.
3. Builds `strata-gemma4-engine` (and, test-only, `strata-gemma4-ref`) from
   `engines/gemma4/src` linking those, and copies `strata_kq` beside it (Q36 forces
   `STRATA_MMQ_KQ=1` and loads it at run time, `q36_engine.cu:3801-3803`).
4. Root option `SECURE_STRATA_BUILD_GEMMA4_ENGINE` (OFF), ExternalProject like SP6; never in CI.

### 3.3 Reuse, copy, new

| Upstream piece | Use | Reason |
| --- | --- | --- |
| `gguf_reader.hpp` | reuse | header-only reader; its `check_architecture` is not called (we write `g4_layout.hpp`) |
| `native_mmvq` / `native_quantize_q8_1` | reuse | dense matvec on the supported types |
| `iq_embed_rows` | reuse | embedding lookup (scaling by sqrt(2816) is our own kernel after it) |
| `prefill/gemm.hpp` (`Gemm::native`), `prefill/moe_mmq.hpp` | reuse | prompt GEMMs and MMQ expert products; `MMQ::swiglu` is a separate call, so our GELU kernel sits between the two products (`moe_mmq.cu:137-160`) |
| `q36_engine.cu` structure | copy, strip | anonymous-namespace driver (`:62-3733`); drop GDN/SSM, M-RoPE, vision, MTP, batch, Qwen constants (EOS 248044/248046, filler 198, image pad); **keep** hybrid placement |
| `place_experts`, `rebalance`, the prompt staging pair | copy | hybrid mode: all experts in pinned host RAM, S VRAM slots per layer, rebalanced by routing counts every 32 positions, double-buffered staging for prompt chunks (`:2796-3009`, `:2298`, `:1272-1288`, `:2888-3007`) |
| `k_attn_prep/part/merge` | copy, rewrite | head width as a template (256 and 512), window, scale 1.0, no sigmoid gate, the two RoPEs, V-norm |
| `route_topk<NE,K>` | copy | file-internal to `native_router.cu:154-207`; instantiate 128/8 |
| grouped expert decode kernels | copy, GELU | SwiGLU is fused at `iq_kernels.cu:2092, 2737, 2766, 3699/3705, 5107`; no activation parameter |
| CPU expert pool (`strata_kernels_cpu`) | copy, GELU + BF16 | computes the experts not in a VRAM slot; SwiGLU is hard-coded at `kernels/cpu/native_expert.cpp:282`, `expert.cpp:308,329,425,440,458,611`, `pool.cpp:1230`; it needs `STRATA_NATIVE_EXPERTS=ON` (§3.2) and a BF16 path |
| `strata_kq` runtime load | reuse | same DLL, same allowlist entry |

## 4. Engine design

### 4.1 Load and geometry (`g4_layout.hpp`)

Our own `geometry_from_gguf` for `gemma4.*` keys (§2.1, §2.6). Fail closed, with a fixed
message, on: an architecture other than `gemma4`; a block count other than 30;
`embedding_length_per_layer_input > 0`; `attention.shared_kv_layers > 0`; head widths other
than 256/512; window other than 1024; experts other than 128/top-8; a missing tensor; a quant
type the kernels do not take (§5.5); `rope_freqs` not of shape [256]. The tensor table of §2.2
binds each tensor once; `output.weight` absent means tied (the head reads `token_embd`).

### 4.2 Weights and placement

Dense matrices, attention, norms and `token_embd` (2.63 GB in the Q8 file) to VRAM as in Q36
(`load` 994-1024). Experts as `[gate|up|down]` blobs (Q36 `expert_blob`), placed by Q36's
`place_experts` (D4):

- **All in VRAM** when they fit with the caches and scratch (a smaller quant).
- **Hybrid** otherwise (the Q8 file): every expert in pinned host RAM (25.2 GB of the box's
  64 GB), `S` VRAM slots per layer filled from the VRAM left after the dense weights, the KV
  caches and scratch, and the rest computed by the CPU expert pool. `rebalance` moves slots
  toward the most-routed experts every 32 positions, as Q36 does. Graph capture is off in
  hybrid mode (Q36 `:2891`).

Budget on the owner's box (16 GB, about 15 GB usable): dense 2.6 GB + fp16 KV (§4.3) + about
1 GB scratch leaves about **10.5 GB for expert slots at 32K** (about 42% of the experts) and
about **8.5 GB at 128K** (about 34%). Each token routes 8 of 128 experts in each of 30 layers
(240 expert products); with a 34-42% resident share before rebalancing, and more after it
concentrates on the hot experts, the remainder (about 0.9-1.0 GB of Q8 weights a token
before rebalancing) is read by the CPU pool from RAM. The decode speed this gives is a
§9.4 measurement, not a promise here. `--expert-vram GiB` caps the slot budget (Q36's
flag); the loader refuses a host with less free RAM than the experts plus 2 GB.

### 4.3 KV caches

Two kinds per session:

- **Sliding layers (25):** a ring of `window + kVerify` rows (1024 + 8; Q36 keeps `kVerify` = 8
  rows for verification, `q36_engine.cu:1164`, and the report shows a ring smaller than that
  would let rejected rows clobber live ones), `[8 heads][1032][256]` for K and for V.
- **Full layers (5):** `[2 heads][ctx][512]` for K and for V.

Precision is the owner's call (§11 D3). Per token of context, full layers cost 40 KiB in
fp32 or 20 KiB in fp16; the sliding rings are fixed at about 420 MB (fp32) / 210 MB (fp16)
whatever the context.

| Context | fp32 full-layer KV | fp16 full-layer KV |
| --- | --- | --- |
| 32K | 1.3 GB | 0.66 GB |
| 128K | 5.4 GB | 2.7 GB |
| 262K | 10.7 GB | 5.4 GB |

With the Q8 file in hybrid mode, KV competes with expert slots, not with the whole model:
fp16 KV at 128K (2.9 GB with the rings) costs about 2.0 GB of slots against 32K. The loader
sizes KV first, then slots, and refuses a `--max-context` whose KV leaves fewer than 8 slots
per layer, naming the largest context that does not.

### 4.4 Decode (one token)

Per layer, in Q36's order (`Engine::layer`, `q36_engine.cu:1880-2075`) with Gemma's
pieces: `k_rms` attn_norm → Q/K/V matvecs → `g4_attn_prep<HD>` (q/k norms, V rms, the
layer's RoPE, write K/V to the cache or ring) → `g4_attn_part<HD,WINDOW>` +
`g4_attn_merge<HD>` (scale 1.0) → Wo → post-attention norm + residual
(a sandwich-norm kernel: Q36's `k_add_rms` assumes pre-norm only) → dense MLP (GELU) → router
(`route_topk<128,8>` on the scaled, unweighted rms input) → GELU grouped experts with the
per-expert scale → the two post-norms, sum, `post_ffw_norm`, residual, layer scale. Graph
capture as Q36 (`process(n==1)` replay, `:2443-2461`).

### 4.5 Prompt (T > 8)

Q36's batched path (`:1565-1572, 1944-1984, 2019-2049`): FP16 GEMMs for dense projections;
attention as strided-batched GEMMs plus a softmax with the causal **and window** mask for
sliding layers (Q36's `k_attn_softmax16` is causal-only, `:439-453`); experts through MMQ
(count/scan/scatter, gate+up product, **our GELU kernel**, down product, per-expert scale).
Chunking by Q36's rule (`:3819-3862`); a sliding layer's ring is filled from the last 1032
positions of each chunk.

### 4.6 Output and sampling

`head_rows` over the tied embedding (`token_embd` read as the output matrix; its quant type
must be one `native_mmvq` takes — Q8_0 in the Q8_K_XL file, §2.6; checked per quant), then the soft-cap kernel, then Q36's host
sampler (`:3019-3056`) and device argmax unchanged. Stop on ids 1 and 106 (read from the
GGUF's `eos_token_id` and the `<turn|>` token, not hard-coded).

### 4.7 Serve loop and protocol

Q36's serve loop (`:3159-3731`) with batch, `BGEN*`, `GENI` and vision removed: `READY <ctx>
stop`, `GEN <max_new> [k=v …] <ids>`, `T <id>`, `PP <done> <fresh> <ms> <tok/s>`,
`DONE <gen> <prompt> <prompt_ms> <decode_ms> <finish> 0 0 <reused> 0 0 0 0 0.0 <fresh> 0`,
`STOP`, `QUIT`, `ERR …`. Sampling keys as Q36 (`:3464-3484`). This is exactly what the
frontend's `strata_0143` flavor accepts today, so no new flavor is needed (the Monitor
reads `DONE`'s figures unchanged).

**Prefix reuse** (Q36 `match_len`/`pick_session`, `:3404-3429`): with no recurrent state a
held prefix can be truncated, but a sliding ring only holds the last 1032 positions, so a
reuse point must be at or after `held - 1032` for sliding layers to still be exact. v1 keeps
Q36's rule: reuse only when the held tokens are a strict prefix of the new prompt. The rings
then hold exactly the last 1032 positions of that prefix, which is what the next positions
need, and `DONE` reports the reused count. Truncating to a shorter common prefix (possible
for full layers, bounded by the ring for sliding ones) is a later optimisation.

### 4.8 Flags (argument profile `strata_gemma4`)

`--serve` (inserted by the frontend), `--native <gguf>` (amended 2026-10-22, plan 1 Task 3
ruling: the frontend appends the model as `--native` last, as for the strata engine; this
section first said `--model`), `--max-context N`, `--chunk N`,
`--kv f16|f32` (§11 D3), `--no-graph`, and the hybrid placement flags carried over from Q36:
`--expert-vram GiB`, `--expert-slots N`, `--hybrid-rows N`, `--cpu-threads N`. Everything
else is refused by the engine and not
composable by the frontend. Test-only (refused with `--serve`): `--tokens`, `--force`,
`--dump-full-logits <file>`, `--dump-layers <file>`, `--bench-pp`, `--bench-tg`.

## 5. Kernels

### 5.1 Attention (new, templated on head width 256/512)

`g4_attn_prep<HD>`: one block per head; HD 512 needs two passes or a 512-thread block
(Q36's is 256 threads with `buf[256]`, `:255, 261`). `g4_attn_part<HD>`: range `[max(0,
pos-1023), pos]` on sliding layers through the ring index, `[0,pos]` on full layers; split-K
as Q36. `g4_attn_merge<HD>`: no sigmoid gate (Q36 `:357`).

### 5.2 Norms

`k_rms` reused (weights as stored). New: `rms_head` with and without weight; the
sandwich step `x + rms(y)*w`; the scaled unweighted rms for the router; the embedding scale.

### 5.3 RoPE

Two NEOX variants: 256 dims base 1e4; 512 dims base 1e6 with `rope_freqs` (§2.4).

### 5.4 Router and experts

`route_topk<128,8>` (softmax, top-8, renormalise; Q36's copy is fixed 256/8,
`q36_kernels.cuh:314`). Decode: copies of the grouped expert kernels with
`gelu_tanh(g) * u` in place of `silu(g) * u` at each fused site, plus the per-expert scale
on the down output. Q36's fast paths that need shapes we do not have (`route_group` 256/8,
`:1798`; `k_sh_gu` Q8_0 and N ≤ 2048, `:1702-1704`) are not used; Gemma's dense MLP runs as
three `native_mmvq` calls plus a GELU kernel.

**CPU expert pool (hybrid mode).** A copy of upstream's pool (`kernels/cpu/native_expert.cpp`,
`expert.cpp`, `pool.cpp`) with GELU-tanh at each SwiGLU site, the per-expert down scale, and
Q8_0 and BF16 expert formats (the Q8 file's two); its results join the GPU's through Q36's
hybrid rows (`--hybrid-rows`, `--cpu-threads`). The prompt path keeps Q36's double-buffered
staging, so a chunk's non-resident experts are uploaded and run on the GPU rather than on
the CPU (Q36 `:2888-3007`).

### 5.5 Quant types

Dense: Q4_0, Q5_0, Q5_1, Q8_0, Q3_K-Q6_K, IQ2_XXS/XS/S, IQ3_XXS/S, IQ4_NL/XS, IQ1_M
(`native_mmvq.cu:2887-2891`). Experts: `iq_kernels.cu:767-777`, with `n_embd` a multiple
of 256 (2816 = 11 x 256) and `n_ff` divisible by the type's block and by 32
(`:4331-4332`). A K-quant (block 256) cannot be the input side of a 704- or 2112-wide
down projection, so those tensors must be block-32 types; LC enforces the same, and the
Q8_K_XL file complies (§2.6). **BF16 matrices are required** by the v1 file (layer 1 whole,
layer 29's `ffn_down` and `ffn_down_exps`, 2.15 GB) and Strata's kernels do not take them
(`upload_f32` handles F32/BF16/F16/Q8_0 only for small vectors, `:934-957`). Plan 3 adds:
a BF16 dense matvec (decode) and BF16 GEMM operands (prompt; cuBLAS takes BF16 natively),
BF16 grouped expert decode kernels, a BF16 path through the prompt expert products (MMQ has
none, so BF16 experts run as cuBLAS grouped GEMMs), and BF16 in the CPU pool. All are
memory-bound and straightforward; each is checked against LC per layer (§9.3). Converting
BF16 to Q8_0 at load is **not** done: Unsloth kept those layers at high precision on purpose,
and it would move the KL away from LC's. F16 matrices are refused.

## 6. Frontend: the `gemma4` profile

### 6.1 Profiles, families, flavors

| Manifest `profile` / `engine.profile` | `engine.family` | Executable | Argument profile | Protocol flavor | Environment family |
| --- | --- | --- | --- | --- | --- |
| `qwen` | `strata` | upstream `strata` | `strata_0143` | `strata_0143` | `strata` |
| `glm53` | `glm` | `strata-glm-engine` | `lighttransport_glm53` | `maya_118` | `glm` |
| **`gemma4`** | **`gemma4`** | **`strata-gemma4-engine`** | **`strata_gemma4`** (§4.8) | `strata_0143` | `strata` |

Touch points (each an enum value plus its tests): `EngineProfile`
(`frontend_config.hpp:105`), `ManifestProfile` (`acquire_manifest.hpp:17`,
`manifest_json.cpp:169-170`), `NativeChatProfile` (`chat_execution.hpp:23`),
`EngineArgumentsProfile` (`engine_arguments.hpp:18`), `EngineFamily`
(`engine_environment.hpp:19`), launch composition (`launch_composition.cpp:37-70`),
`subsystem_limits.cpp:52-56`, `server_bootstrap.cpp:191, 210`, `chat_route.cpp` plan
selection. Config: `engine.executables.gemma4` (an absolute path, SP1's executable rules).
The family-to-executable map is fixed and tested; a `gemma4` manifest never starts
upstream `strata` and vice versa.

**Amended 2026-10-22 (plan 1 reconciliation):** no per-family executable key exists today
(the server always spawns the one `engine.executable`), so plan 1 serves Gemma in **expert
mode**: `engine.profile = "gemma4"` with `engine.executable` naming `strata-gemma4-engine`.
`engine.executables.gemma4`, the family-to-executable map above and opening the engine gate
for `gemma4` manifests (refused today as `engine_profile_unavailable`, "the Gemma 4 engine is
not installed") move to plan 4. The rows above as built in plan 1: argument profile
`strata_gemma4` is an allowlist (§4.8, with `--native`); protocol `strata_0143`; environment
family `strata`; tokenizer mode `spm_bpe` (§6.2) chosen by the profile.

### 6.2 Tokenizer mode `spm_bpe`

`TokenizerPack` gains a mode chosen by the profile, never inferred from the files
(`chat_execution.hpp:20-22`'s rule). Today's `merges.txt` is one merge per line split on a
single space, with a trailing `\r` stripped (`tokenizer_pack.cpp:231-235`); Gemma's merges
contain `\n`, `\r`-free tabs and runs of `▁`, so a `gemma4` pack stores its merges as
`merges.json`, a JSON array of two-string arrays in rank order, read by the same bounded
JSON reader and validated the same way (each side a vocabulary token, no duplicates). The
pack is then `vocab.json`, `merges.json`, `token_type.json`; the `spm_bpe` mode refuses a
`merges.txt` and the byte-level mode refuses a `merges.json`. In `spm_bpe`: encode replaces spaces with
`▁`, splits newline runs, merges by rank over UTF-8 characters, maps a newline-only run to
its token when one exists, and falls back to `<0xXX>`; decode maps `▁` to a space and
`<0xXX>` to its byte (a partial character still goes through `Utf8StreamDecoder`). BOS is
prepended by the renderer. `tools/secure/secure_tokenizer.py` gains `gemma4`: it accepts
`tokenizer.ggml.model = gemma4` only with the `gemma4` profile and writes the same pack.
Tests: round trips over the corpus upstream uses (multi-byte, emoji, combining marks,
whitespace runs, control bytes) and token-for-token agreement with LC's tokenizer on a fixed
text set (the dump tool, §9.2).

### 6.3 Renderer `render_gemma4_text_prompt`

The template of §2.5, byte for byte, as `render_qwen_text_prompt` does for ChatML: a system
turn only with a system message or thinking on; `<|think|>` first in it when thinking is on;
roles `user` and `model`; earlier answers are sent without thought channels (the page never
sends them); the generation prompt, plus the empty thought channel when thinking is off.
Content containing any Gemma control literal (`<bos>`, `<eos>`, `<|turn>`, `<turn|>`,
`<|think|>`, `<|channel>`, `<channel|>`, the tool and image markers) fails closed, as ChatML
markers do. `encode_gemma4_prompt` maps the renderer's own markers to their single ids and
encodes everything else as text.

### 6.4 Reasoning split

The output's thought channel is `<|channel>thought\n…<channel|>`, then the answer.
`GenerationRunLimits::think_tokens` gets `<|channel>` / `<channel|>` (USER_DEFINED,
found exactly once each, as `find_qwen_think_tokens` does), and the split learns one rule:
right after the open token, the channel-name line must be exactly `thought\n` and is dropped;
any other name ends the reply as `undecodable` (fail closed). With thinking off the reply
starts as the answer (`starts_in_reasoning = false`, the prompt closed the channel).

### 6.5 Stop tokens and refusals

Stop ids: `<turn|>` and `<eos>` (each found exactly once as a CONTROL or USER_DEFINED
token). The tool markers, `<|image|>` and other control tokens in output fail closed as today.

### 6.6 Reasoning effort

The schema keeps `reasoning_effort` per request on `gemma4` too: `none` is thinking off,
`low`/`medium`/`high` are thinking on (the template has no levels). With no level and no
`reasoning.effort`, thinking is **off** (the template's default). The page keeps its four
buttons; its drawer notes that Gemma models think or do not, with no levels (§11 D10).

## 7. Model artifacts

v1 runs in expert mode from the local inventory (`engine.profile = "gemma4"`, the GGUF path,
a tokenizer pack built by `secure_tokenizer.py --profile gemma4`), like the IQ3_S Qwen setup
on the owner's box. A manifest schema entry (`profile: gemma4`, `engine.family: gemma4`) is
part of plan 1 so a later catalog entry needs no code. `mmproj-*.gguf` files are never
loaded. The model card's "abliterated / uncensored" lineage is the owner's choice of
weights; the engine and frontend treat it as any other model.

## 8. Security review items

- **Our code, not a third party's:** `engines/gemma4/` is reviewed like the frontend (TDD
  where hermetic, review per change). SCC still applies to the upstream and llama.cpp
  sources it compiles; `add_subdirectory(upstream)` compiles nothing beyond the libraries
  §3.2 names (`EXCLUDE_FROM_ALL`), all already in the reviewed surface at `v0.2.2`.
- **C5 binary gate** on `strata-gemma4-engine` with the existing allowlist: no network
  library or function, no process launch, `strata_kq` the one runtime load.
- **File writes:** none in serve mode; `--dump-*` are test-only and refused with `--serve`.
- **Environment:** the `strata` family's closed allowlist (CUDA cache and temp under the
  state directory); the engine reads no `STRATA_*` tuning knobs in serve mode (Q36's
  `STRATA_Q36_*` knobs are not carried over).
- **Protocol:** the existing `strata_0143` bounds (line caps, fail-closed records).
- **The test-only reference tool** `g4_llama_dump` compiles llama.cpp's `libllama` at
  `3cf03257`, which is not in today's compiled surface (about 99,000 lines in `src/`). It
  never ships and runs only on the box during validation (§11 D7).

## 9. Testing

### 9.1 Hermetic (dev host, every plan)

Frontend: tokenizer mode, renderer bytes, marker refusals, the split rule, stop tokens, the
profile and family tables, config and manifest rows, the e2e chat on the fake engine with a
`gemma4` profile (the fake gains the Gemma end-of-turn id). Engine: the layout guard and
tensor table on synthetic GGUF headers (like upstream `tests/core/model_family_test.cpp`),
the argument parser, the ring indexing (host-side unit test).

### 9.2 Reference dumps

`strata-gemma4-ref --tokens <ids> --force --dump-full-logits F --dump-layers L` (float32
`[step][262144]`, as Q36 `:3919-3970`) against `g4_llama_dump` (LC's per-layer outputs and
full logits, after upstream `tools/qwen35/q36_llama_dump.cpp`), same token ids. The reference
engine keeps the Q8 file's experts in host RAM and uploads each routed expert to the GPU when
it is used (slow and simple: no slots, no CPU pool), so its results do not depend on the
hybrid machinery the fast engine adds. LC runs the same file with its own CPU offload.

### 9.3 Parity gates (box)

1. Per-layer hidden-state error, reference engine vs LC, on 8 fixed prompts (`g4_parity.py`).
2. Teacher-forced KL over 4K tokens, fast engine vs LC: within 1.5x LC's own CUDA-vs-CPU
   floor (`g4_kl.py`, upstream's Q36 gate).
3. Greedy agreement on 32 prompts, 128 tokens each, thinking off and on.
4. The fast engine against the reference engine with the same gates (catches kernel bugs
   that LC comparison blurs).

### 9.4 Acceptance (box)

Through `strata-secure-server` and the page: chat streamed and not, thinking off/on, Stop,
New chat, long prompt (32K) past several windows, a prompt that reuses a held prefix, the
Monitor's figures, the SP3 API kit. Hybrid mode with the Q8 file at 32K and at 128K: the
slot count per layer, the resident share before and after rebalancing, prompt and decode
speed, VRAM and RAM in use; the same chats with `--expert-vram` lowered, to show the CPU
pool carrying more. Results under `docs/validation/gemma4/`.

## 10. Risks

| Risk | Mitigation |
| --- | --- |
| A subtle maths error (RoPE layout, norm order, router input) | the reference engine first (plan 2), per-layer parity against LC before any fast kernel |
| Head-512 attention is slow or register-bound | split the head across two passes; measure against LC's speed on the box |
| The sliding ring and verify/reuse rules | `window + kVerify` ring; strict-prefix reuse in v1 (§4.7) |
| G4's per-tensor quant types do not match our kernels | §2.6 header check before plan 2; refuse, with the tensor named, rather than convert |
| KV memory at long context | fp16 KV (§11 D3); the loader's budget check |
| GELU copies of fused kernels drift from upstream's fixes | the copied sites are listed (§3.3) and diffed on each upstream bump |
| Decode speed in hybrid mode is bound by the CPU pool reading about 0.9-1.0 GB of Q8 experts a token from RAM | `rebalance` concentrates slots on hot experts; `--cpu-threads`; measured in §9.4 with the resident share; a smaller quant (all in VRAM) remains the fast path if the owner wants speed over precision |
| BF16 kernels (GPU decode, prompt GEMMs, grouped experts, CPU pool) are new code in four places | each is small and memory-bound; per-layer parity on layer 1 and 29, the BF16 layers, against LC |
| Pinned host memory of 25 GB plus the server and the OS on a 64 GB box | the loader's free-RAM check (§4.2); the Qwen 128K run and this engine never run together |
| No GPU on the dev host | every engine claim comes from the box (project rule) |

## 11. Owner decisions

All eleven recommendations were accepted by the owner on 2026-10-22 ("Accept the
recommendations for D1-D11 in the Gemma 4 spec"). The owner then chose the downloaded
UD-Q8_K_XL file as the v1 target ("no use the q8"), which changes D8 and reverses D4, and
ruled out an interim engine (D12); the rows below and the body are written to the current
decisions.

| # | Decision | Accepted |
| --- | --- | --- |
| D1 | Where the engine code lives | `engines/gemma4/` in this repository (ours), building upstream unmodified (§3); not a GitHub fork of upstream |
| D2 | Images in v1 | no: text only; vision is its own plan later |
| D3 | KV precision | fp16 for both cache kinds from the start (halves the context cost; §4.3) |
| D4 | Experts outside VRAM | **yes** (changed 2026-10-22 with D8): Q36's hybrid placement — all experts in pinned RAM, VRAM slots rebalanced by routing, the rest on a GELU/BF16 CPU pool (§4.2, §5.4); no SSD streaming |
| D5 | MTP / speculative | no (the GGUF has no MTP block, §2.6) |
| D6 | Prefix reuse in v1 | strict-prefix only, as Q36 |
| D7 | The LC reference dump tool | build `libllama` at the existing pin, test-only, never shipped; run C3's automated scan and C5 on it instead of a line-by-line C4 read (it runs only on the box for validation) |
| D8 | Which quants are supported | **UD-Q8_K_XL** (27.9 GB, `C:\Strata-data\models\gemma4_q8`) is the v1 target, the file every gate of §9 runs on (owner, 2026-10-22: "no use the q8"); it needs hybrid placement (D4) and BF16 kernels (§5.5); smaller quants load if their types pass §5.5 |
| D9 | Catalog entries | none in v1 (expert mode); add later on request |
| D10 | Thinking default and the page | thinking off unless asked (the template's default); the page's Off/Low/Medium/High stay, all but Off meaning "on" for Gemma |
| D11 | Executable and family names | `strata-gemma4-engine`, family `gemma4`, profile `gemma4` |
| D12 | An interim engine before the port | **none** (owner, 2026-10-22: "Keep to the Strata-style port in the spec; Gemma joins the 8080 UI when that engine is done"). No llama.cpp-backed engine is built; the frontend profile (plan 1) and the engine (plans 2-3) reach the 8080 UI together in plan 4. llama.cpp stays the parity reference only (§9.2). |
