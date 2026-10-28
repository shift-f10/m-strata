# strata-gemma4-engine — parity and speed on the owner's box (2026-10-22)

Gemma 4 plans 2-3 (`docs/superpowers/plans/2026-10-22-gemma4-plan2-3-engine.md`), Tasks 5-9.
Box: RTX 4080 SUPER 16 GB (driver 610.88, PCIe 4.0 x16), AMD Ryzen 9 3900X (12 cores, AVX2),
64 GB DDR4-3600, Windows 11 Pro. CUDA 13.0, MSVC v143. Model: the owner's
`C:\Strata-data\models\gemma4_q8\gemma-4-26B-A4B-it-UD-Q8_K_XL.gguf` (27,866,180,544 bytes).
Reference: llama.cpp at the pinned `3cf03257` through the test-only `g4_llama_dump`
(`engines/gemma4/tools/g4_llama_dump.cpp`, built by `engines\gemma4\tools\build-g4-llama-dump.bat` against the
test-only llama.cpp build of session-log step 17; routed experts on llama.cpp's CPU backend,
everything else on CUDA). Token ids came from a scratch encoder over the GGUF's own vocabulary
and merges (the frontend's encoder is plan 1's), rendered with the chat template, thinking off.
Nothing from the model file is committed: these files hold ids, statistics and timings only.

## Parity (spec §9.3)

Method: the same ids through both; llama.cpp's greedy continuation is fed to ours (teacher
forcing, `--force`), and every step's full logits are compared (`tools/g4_parity.py`): KL of
the reference distribution against ours, top-1 agreement, top-5 overlap; per-layer outputs
(`l_out-N` against `--dump-layers`) as relative RMS error per position. llama.cpp's own floor is
its CPU backend against its CUDA run on the same ids.

| Run | Steps | Mean KL | Max KL | Top-1 | Top-5 overlap |
| --- | --- | --- | --- | --- | --- |
| short chat (25 prompt ids), ours vs llama.cpp CUDA | 24 | 0.0399 | 0.334 | 22/24 | 112/120 |
| same ids, llama.cpp CPU vs llama.cpp CUDA (the floor) | 24 | 0.0422 | 0.631 | 21/24 | 111/120 |
| long prompt (4,911 ids: 10 prompt chunks, the 2,048-row sliding ring wrapped twice), ours vs llama.cpp CUDA | 24 | 0.0339 | 0.589 | 23/24 | 113/120 |

- Ours against llama.cpp sits at llama.cpp's own backend-to-backend floor (mean KL ratio 0.95;
  the spec's gate is 1.5x). On the steps inside the answer the KL is below 1e-4; the large ones
  are the junk past `<turn|>` (teacher-forced after the model ended its turn), where llama.cpp's
  two backends disagree with each other just as much.
- Per-layer median relative error: 0.45% after layer 0, 0.9% at layer 10, 2.4% at layer 20,
  5.6% at layer 28 (the floor: 0.19%, 1.0%, 2.8%, 4.9%). Files:
  `2026-10-22-parity-short-ours-vs-lc-cuda.txt`, `2026-10-22-parity-short-lc-cpu-vs-lc-cuda.txt`,
  `2026-10-22-parity-long-ours-vs-lc-cuda.txt`.
- Answers: "The capital of France is Paris." and, from the 4,911-token archive, "The secret
  password in record 0 is PELICAN-7." — the same ids as llama.cpp's greedy run in both.
- The engine's two paths agree (`tools/g4_paths_agree.py`): the prompt path (dequantised
  weights, cuBLAS GEMMs and attention) against the decode path (matvec kernels, split-K
  attention) after the same 30 ids: KL 9.0e-5, the same top-1.

Not yet run (spec §9.3): the 4K-token teacher-forced KL over natural text, the 32-prompt greedy
agreement, thinking-on prompts.

## Protocol (Task 7)

`tools/g4_serve_smoke.py` drives `--serve` over stdio as the frontend's `strata_0143` client
does: INFO then `READY 8192 stop`; GEN → PP, T, DONE (16 fields); a follow-up turn reused 32 held
ids (DONE's reused field); STOP mid-answer → `cancel`; an out-of-vocabulary id → one ERR line;
QUIT → exit 0. All checks passed (`2026-10-22-serve-smoke.txt`).

## Binary and environment (spec §8)

- `dumpbin /dependents`: KERNEL32, the MSVC runtime (MSVCP140, VCRUNTIME140, the api-ms-win-crt
  set) and `cublas64_13.dll` (the CUDA runtime is linked statically). `dumpbin /imports` has no
  process-launch, socket, WinINet/WinHTTP or shell function; `LoadLibraryExA`/`GetProcAddress` are
  the static CUDA runtime's own driver load (as in upstream's engine).
- A run with only the strata family's child environment (PATH = the CUDA `bin\x64` folder and
  System32, SystemRoot, WINDIR, SystemDrive, NUMBER_OF_PROCESSORS, TEMP/TMP and CUDA_CACHE_PATH
  under a state folder) loads and answers as above.

## Placement and speed (Task 9)

- Load: 11-29 s (warm or cold file cache); 56-57 VRAM expert slots per layer (43.8-44.5% of
  the experts), VRAM 15.8 of 16.4 GB in use (the desktop holds about 1.3 GB), 23.49 GiB of experts
  in pinned RAM. Context 4,096-8,192 in these runs.
- Decode: 46.9-49.5 tok/s on chat answers right after load; 80.6 tok/s on `--bench-tg 128` after
  a 2,048-token prompt had rebalanced the slots toward the routed experts. llama.cpp's
  `llama-server` on the same file (`--n-cpu-moe 19`, session-log step 17): 24-30 tok/s.
- Prompt: 864 tok/s for 2,048 ids (`--bench-pp`), 949 tok/s for the 4,911-id prompt; 26-42
  tok/s for 5-25 ids (each chunk reads every routed cold expert over PCIe once).
- At 32K context (54 slots a layer, 42.2%): a 30,000-id prompt in 27.0 s (1,113 tok/s), then
  77.1 tok/s decode (`--bench-pp 30000 --bench-tg 64`).
- Thinking on (system turn `<|think|>`): the answer starts `<|channel>thought\n`, the thought
  ends `<channel|>`, the answer ends `<turn|>` — the shape plan 1's split expects.
- 2026-10-23, after the serve run (owner: "move sampling to the GPU and cache the Gemma encode
  index"): the same 27-token story prompt, 225 tokens, seed 5, through `--serve`:
  - The sampler's top-k now runs on the GPU (`g4k::topk`, 64 candidates; the host draws from
    them); a no-op penalty forces the old host path (1 MB of logits a token, ranked on the
    host): the same 225 ids on both paths, 30.2 tok/s host against 29.2-32.1 tok/s GPU. The
    sampler was not the bottleneck: about 1.9 ms a token.
  - The bottleneck was the expert placement adapting too slowly in decode (8 slot swaps every
    32 tokens). With 32 swaps every 8 tokens (one batched read of all layers' routing counts):
    54.1 tok/s from a cold start, then 77.0 and 84.5 tok/s on the repeats (29.2, 30.2 and 32.1
    before). Logs: `out\g4-sampler-speed.log`, `out\g4-rebalance-speed.log`.
- Plan R2 (the CPU expert pool, spec D4): not built. Cold experts are read by the GPU from pinned
  RAM over PCIe and decode already runs at about twice llama.cpp's speed on this box; the pool is
  an owner decision, not needed to serve the model.
