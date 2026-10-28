# qwen4exp speculative decode (MTP draft + verify window) — design

Date: 2026-10-24. Engine: `engines/unified` (`strata-unified-engine`), family qwen4exp. Status: design; nothing built.

## Why

The owner's upstream run (`C:\Strata\strata-iq3_s.log`, same RTX 4080 SUPER box, IQ3_S) decodes **81.4 tok/s** over
9,603 tokens: 6,223 verify windows (1.54 tokens a window, drafts accepted 3,380 of 5,066) at **~19 ms a window**.
A 512-token request runs 105 tok/s at 3.4 tokens a window. The unified engine decodes 51.5 tok/s at 19.4 ms **a
token**: its single-token step already costs what upstream's whole window does. The window is how upstream reads
the dense weights (~7 ms of our 19 ms: Q6_K / Q5_K / Q4_K / IQ / BF16 GEMVs, near bandwidth) **once for T tokens**.
So parity needs the same machinery, not more single-token tuning.

## Upstream's contract (read from `upstream/include/strata/core/verify.hpp` and `mtp.hpp`)

- **Verify window**: T tokens at positions p0..p0+T-1 (the last accepted token plus T-1 drafts) go through all 48
  layers in one captured graph; the head's argmax comes out for every row. Row t's argmax is **bit for bit** what
  greedy single-token decode would produce after token t: every kernel is either the single-token kernel applied
  per token, or a multi-column kernel whose per-column arithmetic is the single-column kernel's (`native_mmvq.cu`
  P3: each format's dot split into a per-block weight `load` and a per-column `apply`, accumulated, summed across
  warps and reduced in the single-column order). A draft is accepted exactly when greedy decode would have
  produced it, so the output is unchanged by speculation.
- **State**: the window appends K/V for all T positions and leaves the GDN state untouched; `commit(n_keep)` makes
  the first n_keep tokens permanent (GDN conv history + recurrent state advanced by replaying those tokens from
  inputs the window stored; the PLE history set to its snapshot after token n_keep-1). K/V cells past the accepted
  prefix are overwritten when those positions are processed again, before any query reads them.
- **MTP drafter** (`mtp/rt`: `dense.bin` + `dense.txt`, `experts.bin`, `draft_vocab.bin`): one extra layer. Cell i
  pairs the main model's final multi-stream residual R at position i with the token at i+1 (rope position i):
  `e = fc_embedding(rms(embed(tok)) * (1+w))`, `h = fc_hidden(rms_10240(R) * (1+w))`, `R' = h + e` -> attention HC
  -> attention (own K/V, dense over every cell) -> MLP HC -> MoE (512 experts, top-10, Q2_0, all in VRAM, 708 MB) +
  shared expert -> inject -> final mixer -> head over the draft vocabulary (106,299 rows of `output.weight`). Its
  own residual feeds the next draft step. Projections are Q8_0, HC and router BF16, norms F32 with the +1 applied.
  `experts.bin` is upstream's CPU blob layout of the Q2_0 blocks (codes plane + fp16 scales plane, gate/up rows
  interleaved): a lossless relayout back to GGUF Q2_0 blocks at load lets our existing GPU Q2_0 path run it.
- Drafts per window are adaptive (`--spec 4`, `--spec-min-p 0.70`: a draft step continues while the drafter's top
  probability clears the floor).

## Design for the unified engine

Stages, each landing on its own with its own evidence:

**S1. Multi-column exact GEMV kernels.** `u::k::mmvq` today loops single-column launches for ncols > 1. Add fused
NCOLS = 2..8 kernels for every format the qwen4exp dense path uses (Q6_K, Q5_K, Q4_K, the IQ types, BF16, F32),
transcribed from the single-column kernels with the weight load hoisted out of the column loop, so each column is
bitwise the single-column result. Evidence: a GPU self-check comparing every format's NCOLS kernel against NCOLS
single-column calls (bitwise), plus the bandwidth gain at T = 2..4.

**S2. The T-token verify window (no drafter yet).** `Qwen4ExpGraph::window_(T, tokens, p0)`: embeds and PLE rows for
T tokens; per layer HC read/write per token, the GEMVs through S1, GDN on a scratch copy of the layer's conv +
recurrent state (the kernels already loop over T; ~72 MB copied a window), QSA K/V appended at T positions and
causal attention for T queries, router + device-decided MoE for T x k entries (one grouped launch over T*k, tail
skip off in the window for the exactness check), head + argmax for T rows. `commit(n_keep)`: replay the kept
tokens' GDN updates from the stored per-token inputs (or copy the scratch state when n_keep == T), advance PLE and
positions. One captured graph per T. Evidence, without any drafter: **teacher forcing** — drive windows with the
greedy single-token output as the drafts; every draft must be accepted and the stream must equal the single-token
greedy stream token for token, and the window time per T gives the ceiling.

**S3. The MTP drafter.** Loader for `mtp/rt` (Q8_0 / BF16 / F32 dense, Q2_0 experts relaid out, draft-vocab head
gathered from `output.weight`), its own K/V, the draft chain with the min-p floor. Evidence: acceptance on the
owner's prompts against upstream's (89/86/85% at steps 1-3 per `mtp.hpp`; 3,380 of 5,066 on the long run), the
output unchanged against plain greedy (the window decides every token), and tok/s.

**S4. Tuning**: draft depth / floor, tail skip inside windows (upstream: skip a missed expert only if every token
of the window routes it at rank >= 7), the adaptive tier between windows, graph per T.

## Not in scope

Sampling (temperature / top-p) in the window, prompt-lookup drafts, batching, a layer split.
