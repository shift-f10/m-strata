# Gemma 4 Plans 2-3 — `strata-gemma4-engine` — Implementation Plan

**Goal:** our own engine executable, `strata-gemma4-engine`, that loads the owner's
`gemma-4-26B-A4B-it-UD-Q8_K_XL.gguf` (spec D8) on the 16 GB RTX 4080 SUPER, speaks the
`strata_0143` stdio protocol the frontend already uses, and produces llama.cpp's outputs
(spec §9.3) — so that plan 4 can put Gemma behind the 8080 UI (spec goal 0, D12).

**Owner request (2026-10-22):** "keep working on this until it is ready and can serve the
model for testing". Plans 2 and 3 are executed together in this one plan because the box is
the dev host: every GPU step runs where the GPU is.

**Spec:** `docs/superpowers/specs/2026-10-22-gemma4-engine-design.md` (§2 the model, §4 engine
design, §5 kernels, §8 security, §9 testing, D1-D12).

## Reconciliation with the spec (binding corrections, each recorded for the owner)

- **R1 — no upstream libraries linked.** Spec §3.2/§3.3 planned to build upstream's
  `strata_kernels`/`strata_mmq`/`strata_kernels_cpu` (needs `STRATA_NATIVE_EXPERTS=ON` and a
  ggml checkout) and copy their fused SwiGLU kernels into GELU variants. The v1 file has
  three weight types only (Q8_0, BF16, F32, spec §2.6), so the engine carries its own small
  kernels for exactly those (matvec, dequantise) and uses cuBLAS (part of the CUDA toolkit
  upstream's engine already loads) for the prompt GEMMs. Nothing from `upstream/` is
  compiled; the compiled third-party surface shrinks to CUDA's own runtime and cuBLAS.
  Other quant types are refused at load with the tensor named (spec §5.5's "refuse rather
  than convert").
- **R2 — cold experts read from pinned RAM by the GPU.** Spec §4.2/D4 places every expert in
  pinned host RAM with VRAM slots rebalanced by routing counts (kept), and computes the
  non-resident experts on a GELU/BF16 CPU pool. v1 computes them on the GPU reading the
  pinned RAM directly (mapped, zero-copy, over PCIe 4.0 x16) — one code path, no per-layer
  host sync. The CPU pool is the measured follow-up (Task 9): the box's DDR4-3600 dual
  channel (≈40 GB/s) is faster than PCIe reads (≈20 GB/s), so the pool is expected to help
  decode; it is added only if the measurement says so. **Owner ruling wanted.**
- **R3 — the reference engine (plan 2) is the same executable.** llama.cpp at `3cf03257` is
  the reference (spec §9.2); our kernels are each checked against a CPU computation in
  `g4_kernel_tests`, and the whole forward pass against llama.cpp's logits and per-layer
  outputs (`g4_llama_dump`, test-only, D7). A second, slow CUDA engine is not built.
- **R4 — sliding ring = window + max(chunk, 1024) rows** (spec §4.3 said window + 8). The
  prompt path writes a whole chunk's keys before attending, so the ring must hold
  `window + chunk - 1` rows; the extra rows also let a new prompt reuse the held state when
  it diverges within the last `ring - window` tokens (the thought channel is stripped from
  history, so a follow-up turn diverges where the last answer's thinking began). Reuse =
  the longest common prefix when the held tail past it fits the ring's spare rows, else none
  (spec D6 said strict prefix; this is strictly more reuse with the same exactness).
- **R5 — the model flag is `--native <gguf>`** (spec §4.8 said `--model`): upstream's q36
  engine accepts `--native`, the frontend already composes it for the strata family, and
  `--native` is frontend-owned (operators cannot pass it).
- **R6 — `--kv f32` is refused by the engine in v1** (D3 chose fp16; the allowlist keeps the
  value for later). `--hybrid-rows` is accepted and unused until the CPU pool exists.
- **R7 — layout.** `engines/gemma4/CMakeLists.txt` is a standalone project (no wrapper under
  `cmake/`: nothing of upstream is added, so there is no target-name clash to wrap).

## Global constraints

- `upstream/` is never modified; nothing of it is compiled. Host code: MSVC `/W4 /WX`;
  device code: `nvcc -Werror all-warnings`. CUDA 13.0, `sm_89`, static cudart, cuBLAS DLL from
  the toolkit's `bin\x64` (already in the Qwen setup's `engine.lib_dirs`).
- No network code, no process launch, no file writes in serve mode; `--dump-*` and
  `--tokens` are test-only and refused with `--serve`. Diagnostics go to stderr; stdout
  carries protocol records only.
- Fail closed on every malformed input: GGUF (bounded parser, every tensor's type, shape,
  offset and size checked against the file), GEN lines (ids in range, context bound).
- TDD where hermetic (`g4_host_tests`: parser, geometry guard, CLI, protocol parsing,
  sampler, ring arithmetic) and kernel-by-kernel on the box (`g4_kernel_tests`: every
  kernel against a CPU reference on random data).

## Tasks

1. **Build skeleton** — CMake project, `strata-gemma4-engine`, `g4_host_tests`,
   `g4_kernel_tests`; a build script `engines/gemma4/build-windows.bat`.
2. **GGUF reader** (`g4_gguf.{hpp,cpp}`) — bounded GGUF v3 parser; tensor table checked
   against the file size. Tests: synthetic files (valid, truncated, bad magic, oversized
   counts and strings, misaligned/overlapping/out-of-file tensors, unknown types).
3. **Model guard** (`g4_model.{hpp,cpp}`) — spec §4.1 geometry from `gemma4.*` keys and the
   tensor table of §2.2, fail closed with fixed messages; the EOS ids from the vocabulary by
   literal. Tests: a synthetic gemma4 header (and each refusal).
4. **CLI and protocol** (`g4_cli`, `g4_protocol`) — argument parser (spec §4.8 + R5/R6), GEN
   parsing (Q36's grammar), sampler (Q36's `sample` semantics). Tests: hermetic.
5. **Kernels** (`g4_kernels.cu`) — Q8_0/BF16/F32 matvec (single and grouped-by-expert),
   RMSNorm variants, embedding, q/k/v norm + the two RoPEs + cache write, decode attention
   (split-K, head 256/512, window), prompt attention (cuBLAS + masked softmax), router top-8,
   GELU, soft-cap, dequantise. Tests: each against a CPU reference.
6. **Engine** (`g4_engine.cu`) — load (repack Q8_0 into int8 + scale planes), placement
   (dense in VRAM, experts in pinned RAM, VRAM slots), decode and prompt forward passes,
   session and prefix reuse (R4), rebalance.
7. **Serve loop** (`g4_serve.cpp`) — READY / GEN / PP / T / DONE / STOP / QUIT as upstream's q36
   engine; INFO records for the Monitor.
8. **Parity on the box** — `g4_llama_dump` (from upstream's `tools/qwen35/q36_llama_dump.cpp`,
   experts on llama.cpp's CPU backend), per-layer error and teacher-forced KL / greedy
   agreement; results in `docs/validation/gemma4/`.
9. **Speed** — decode and prompt tok/s at 32K, slot share; CPU pool decision (R2).

## Status (2026-10-22, on the owner's box)

- Tasks 1-8 done; Task 9 measured, the CPU pool not built (R2, owner ruling wanted).
  Results: `docs/validation/gemma4/2026-10-22-engine-parity-and-speed.md`.
- Gates: `ctest --test-dir out\gemma4-engine` 23/23 (12 host, 9 GPU kernel cases against CPU
  references, 2 real-model checks); MSVC `/W4 /WX` and nvcc `-Werror=all-warnings` clean.
  RED evidence: the host cases failed against stubs before each module; the kernel cases all
  failed against empty launchers (`out\g4-kernels-red.log`); the non-finite guard's case failed
  before its fix (`out\g4-nonfinite-red.log`).
- Found on the real file: layer 29 mixes Q8_0 gate_up experts with BF16 down experts, so the
  expert blob layout is per matrix (`expert_layout(gu, dn)`).
- Test-tolerance ruling (kernel case `qkv_post_matches_cpu_for_both_layer_kinds`): the reference
  RoPE is now computed in double precision and the tolerance includes float RoPE's own angle
  rounding (about pos x 2.4e-7 rad, 2 ulp); before, the reference was float with a different
  `pow` and the case failed at one value near position 2,047 on some random draws. The kernel
  now takes `theta_scale` from the host's `powf`, as ggml-cuda's RoPE launcher does.
- 2026-10-23 (owner: "move sampling to the GPU and cache the Gemma encode index, test-first"):
  `g4k::topk` ranks the sampler's 64 candidates on the GPU (`topk_matches_a_cpu_ranking`) and
  `g4::sample_sorted` draws from them exactly as `sample` does
  (`sorted_candidates_sample_like_the_full_sampler`); penalties keep the host path. Measured,
  the sampler cost only about 1.9 ms a token; the decode rebalancing was the bottleneck and now
  runs every 8 tokens with up to 32 swaps (`kDecodeRebalanceEvery`/`kDecodeRebalanceSwaps`),
  reading all layers' routing counts in one copy: a sampled story went from 29-32 to 54-85
  tok/s. Gates: 25/25 (13 host, 10 GPU kernel, 2 real-model).
- 2026-10-23: an unseeded GEN gets a seed from the clock (`g4::request_seed`, upstream
  generate.cpp:13784) instead of the q36 engine's fixed 0, so a retry is a new draw (session log
  item 55). Gates: 26/26.
