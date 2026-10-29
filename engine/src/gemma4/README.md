# Gemma 4 26B-A4B in the one engine (`strata_gemma4`)

The Gemma 4 family library of `multiengine.exe` (target `strata`): `main()`'s family switch reads the `--native`
GGUF's architecture and hands a `gemma4` file to `strata::gemma4::run` (`include/strata/gemma4/engine.hpp`), as it
hands a `qwen35moe` file to `strata_qwen35`. It speaks upstream's stdio protocol (`strata_0143`), so the
frontend's `gemma4` profile drives it like the Qwen engine. NVIDIA only, and only with `STRATA_NATIVE_EXPERTS`
(default ON). Formerly the standalone `engines/gemma4` (spec `docs/superpowers/specs/2026-10-22-gemma4-engine-design.md`);
the expert tiering follows the Strata paper since 2026-10-26
(`docs/superpowers/specs/2026-10-26-gemma4-paper-tiering-design.md`).

## Build and test

Part of the engine's CMake build (`engine/CMakeLists.txt` adds `src/gemma4` inside its CUDA block). Host code
builds with `/W4 /WX`, device code with `-Werror=all-warnings`. `-DSTRATA_BUILD_GEMMA4_TESTS=ON` (or
`STRATA_BUILD_TESTS`) adds the family's tests (`engine/tests/gemma4`): `g4_host_tests` (GGUF, model guard, CLI,
sampler), `g4_lane_tests` (the CPU lane on the shared expert code at Gemma 4's geometry), `g4_kernel_tests` (label
`gpu`), and with `-DG4_MODEL_FILE=<gguf> -DG4_PYTHON=<python>` the model checks `g4_model.paths_agree` (prompt vs
decode), `g4_model.prompt_paths` (the streamed MMQ prompt vs the grouped kernel, a real 293-token prompt) and
`g4_model.serve_smoke` (labels `gpu;model`), which run `multiengine.exe`; with `-DG4_MTP_FILE=<drafter gguf>` also
`g4_model.mtp_window` (windows bitwise the decode steps), `g4_model.mtp_greedy` (greedy with the drafter = without),
`g4_model.mtp_serve` (the serve protocol, speed and acceptance) and `g4_model.mtp_serve_exact` (served greedy answers
the same tokens with and without the drafter, the lane off and the slots pinned).

## Model

Exactly the Gemma 4 26B-A4B text shape (30 layers, 2816 wide, 128 experts top-8), in Q8_0, BF16 and F32 tensors -
the owner's `gemma-4-26B-A4B-it-UD-Q8_K_XL.gguf`. The loader refuses any other architecture, shape, quant type or
unknown tensor with a fixed message (`g4_model.cpp`).

## Flags

| Flag | Meaning |
| --- | --- |
| `--native GGUF` | the model file (frontend-owned: the server composes it) |
| `--serve` | the stdio protocol (inserted by the server) |
| `--gpu i[,j]` | CUDA devices in PCI order (the first is used) |
| `--max-context N` | 1-262144 (default 32768); the full layers' f16 KV costs 20 KiB a token |
| `--chunk N` | prompt tokens per forward pass, 1-8192 (used up to 2048, the paper's chunk; default 512 - each doubling costs a few expert slots a layer) |
| `--kv f16` | the cache precision (`f32` is refused in v1) |
| `--expert-vram GiB` | the VRAM given to expert slots (default: the free VRAM less 640 MiB) |
| `--expert-slots N` | slots per layer instead of the budget |
| `--cpu-lane on\|off` | decode misses on the CPU lane (default on); off: the GPU reads them over PCIe in place |
| `--pcie-frac F` | 0-1: the share of a layer's misses the copy engine stages for the GPU (default 0, see below) |
| `--cpu-threads N` | the CPU expert pool's workers (default: one per physical core but the host's) |
| `--expert-profile P` | fill the VRAM expert slots from Strata's `profile.bin` (default: experts 0..S-1 of each layer) |
| `--expert-profile-save P` | write what the tier learned there, in the same format: on exit and between requests (every 10 min at most) |
| `--no-prompt-borrow` | the streamed prompt's buffers in VRAM of their own instead of lent by the expert cache (A/B) |
| `--no-graph` | a decode step as plain launches instead of one captured graph |
| `--hybrid-rows N` | accepted, no effect |
| `--mtp GGUF` | MTP speculative decoding with this `gemma4-assistant` drafter (unsloth's `mtp-gemma-4-26B-A4B-it-Q8_0.gguf`); checked against the model before load |
| `--spec N` | the verify window's rows, 2-8 (1 + the drafts a step; default 4: llama.cpp's 3 drafts); nothing without `--mtp` |
| `--spec-min-p P` | 0-1: drafting stops at a draft whose top-1 probability (the softmax of the drafter's top 10) is below P (default 0); nothing without `--mtp` |

Test-only (refused with `--serve`): `--tokens ids [--n N] [--force ids] [--dump-full-logits F]
[--dump-layers F]`, `--bench-pp N`, `--bench-tg N`, `--check` (with `--mtp`, the drafter's summary too), `--settle`
(every expert swap lands after the prompt), `--spec-check N` (with `--mtp`: N greedy tokens, each window's logits
compared bit for bit with the same tokens' single decode steps).

## How it runs (the paper's design, 3.1-3.6)

- **RAM holds every routed expert**: one pinned, mapped range a layer, each expert the GGUF's own bytes as a native
  blob `[gate rows | up rows | down rows]` (no repack).
- **VRAM holds the dense part, the KV caches and the expert cache** (Strata's `ExpertCache`, slots byte-sized per
  layer) - filled at start from a usage profile (`--expert-profile`; `--expert-profile-save` writes the next one),
  moved toward the most-routed experts after each prompt chunk, after a prompt and every 8 decoded tokens, on a copy
  stream nobody waits for (an evicted expert is read from RAM at once; an incoming one is used once its copy lands).
- **A decode step's experts on three workers** (`g4_lane.cpp`): the GPU publishes the routing and the experts'
  input to mapped memory; a CPU thread spinning on it runs the engine's window dispatcher - the cached experts on
  the GPU at once, `--pcie-frac` of the misses copied into VRAM staging by the copy engine and computed when they
  land, the rest on the CPU pool (ggml-cpu's dot products), whose rows come back through mapped memory.
- **A decode step is one captured CUDA graph**; the token and its position are device data.
- **Prompts in chunks** (up to 2,048 tokens): a chunk of 64 or more streams every layer's uncached experts from the
  arena into a ring of three VRAM staging buffers on a copy stream while the GPU computes the experts before them -
  the Q8_0 layers on llama.cpp's MMQ (the slots read in place), the BF16 layers (1, 29) on the grouped kernel from
  the ring (`STRATA_G4_NO_MMQ=1`: the grouped kernel reading the misses over PCIe in place, the A/B arm). The
  chunk's buffers - its rows, the MMQ buffers, the ring - are borrowed from the expert cache's top slots for the
  prompt and refilled after it, as the tier's swaps (Strata's prefill borrow). Dense weights dequantized through
  cuBLAS SGEMM; attention as cuBLAS batched GEMMs with a masked softmax.
- Protocol: `INFO ...`, `READY <ctx> stop`; `GEN <max_new> [k=v ...] <ids>` -> `PP`, `T <id>`, `DONE ...` (stops by
  itself on `<eos>` and `<turn|>`; fields 6 and 7 are the drafts accepted and offered, 0 0 without MTP); `STOP`;
  `QUIT`. A new prompt reuses the held tokens' common prefix when the held tail past it fits the ring's spare rows.
- **MTP** (`--mtp`, spec §12; `g4_mtp.{hpp,cu}`, `g4_spec.{hpp,cpp}`): llama.cpp's draft-mtp with shared memory. The
  drafter (4 layers, 1024 wide, all in VRAM, 428 MiB taken before the expert slots) embeds a token with the target's
  table, joins it with the target's final hidden state and attends Q-only over the target's caches (sliding layers
  over layer 28, full over 29) at the token's position; a step's drafts are one captured graph, the token chained on
  the device. The target then reads `[token, drafts]` as one **verify window**: the decode step's kernels over the rows
  (`matvec_rows` reads each weight once; one attention per row; the experts grouped across the rows, each distinct
  missed expert computed once on the CPU lane for all its rows), so every row's logits are bitwise the decode step's
  (`--spec-check`). Acceptance is llama.cpp's `common_sampler_sample_and_accept_n` with the request's own sampler, so
  sampled requests keep their exact plain-decode draws; the engine keeps the tokens plain decode keeps.

Measured on the RTX 4080 SUPER (16 GB, PCIe 4.0; 53-54 of 128 expert slots a layer with the borrowed prompt
buffers): a 2,048-token prompt 704 tok/s at 512-token chunks and 2,192 at 2,048, then 96 decode steps at 86.2 and
71.6 tok/s; a cold 48-step decode from a learned profile 90.0 tok/s against 43.2 without (the same workload); lane
off 41.9. The copy engine's share is 0 by default: staging decode misses was slower here at every share (half
45.4, all 36.5 tok/s, plain launches) - the pool reads RAM faster than the link copies, and both share the RAM's
bandwidth.

## Validation

`docs/validation/gemma4/2026-10-22-engine-parity-and-speed.md`: llama.cpp parity at its own backend-to-backend
floor (the engine before the paper's tiering). Tools: `engine/tests/gemma4/g4_llama_dump.cpp` (test-only llama.cpp
side), `g4_parity.py`, `g4_paths_agree.py`, `g4_serve_smoke.py`.
