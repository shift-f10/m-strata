# Our engine (ported from Strata)

The Strata inference engine, **ported into our repo** (`engine/`) and built into our own `multiengine.exe`. Not a
wrapper around Strata's binary — our own build from the source here (Strata lineage, security-reviewed serve
layer). Runs **qwen3.8-flash-next IQ3_S** end-to-end with the full stack: MTP speculative decode, the CPU expert
pool, the adaptive VRAM residency tier, and the GPU/PCIe/CPU expert split. Verified ~**56–57 tok/s**.

## One executable, a library per model family

`multiengine.exe` is only a launcher (`src/multiengine/launcher.cpp`). It reads which family a command line is for
(`--glm-pack`: the architecture of the GGUF the pack was made from; `--native` / `--model`: the GGUF
`general.architecture` of the first shard; `--pack` alone: a DeepSeek pack's `ds4.txt` or a MiniMax pack's `mm3.txt`),
loads that family's library from its own folder and hands it the whole command line. Each library is compiled from
its own sources, so no family, and no update of one, changes what another computes:

| Library | Family | Source |
|---|---|---|
| `multiengine_strata.dll` | qwen4exp (Qwen3.8-Flash-Next), qwen35moe (Qwen3.6) | upstream Strata **v0.2.6** (engine 0.1.51), `src/program/generate.cpp` unchanged |
| `multiengine_maya.dll` | glm5-next (GLM-5.3-Flash, `--glm-pack`) | Project Maya **v1.1.30**, its `maya` program, `third_party/maya` unchanged |
| `multiengine_maya_ds4.dll` | deepseek41 (DeepSeek-V4.1-Flash, `--pack` of a ds4 pack) | Project Maya v1.1.30, its `maya-ds4` program |
| `multiengine_maya_mm3.dll` | minimax-m3 (MiniMax-M3.1-Flash, `--pack` of an mm3 pack) | Project Maya v1.1.30, its `maya-mm3` program |
| `multiengine_raon1.dll` | raon1 (Raon-1-mini, `--glm-pack`) | our raon1 port onto Maya v1.1.27's GLM engine, `third_party/project-maya` |
| `multiengine_gemma4.dll` | gemma4 (Gemma 4 26B-A4B) | `src/gemma4` on its own copy of the Strata core (`src/gemma4/core`, v0.2.4.2 + Gemma's generalisations) |
| `multiengine_flint.dll` | flint (RecursiveSI's Flint, `--native` of its GGUF, any shard) | `src/flint`, ours, on Gemma 4's pieces (its GGUF reader, its frozen core library `g4core`) |

They share only the pinned ggml (`third_party/ggml/VERSION.txt`) and the CUDA toolkit. Everything under `src`,
`include`, `tests`, `cmake`, `data` and `third_party/ggml*` that is not one of ours above is upstream's file as
upstream ships it; the root `CMakeLists.txt` differs only by its last line (`include(cmake/multiengine.cmake)`).
To update upstream: `python -I engine/src/multiengine/sync_upstream.py G:\Strata <tag>` (it reads the tag from the
clone's git objects and writes the changed files; then build and check each family against its references).

`third_party/maya` is Project Maya's engine tree (`src`, `include`, `cmake`, `tests`, `third_party/ggml`, its
`CMakeLists.txt` and licences) exactly as Maya ships it, not edited. It is built as a CMake project of its own in
`build-g\maya`, configured as Maya's own setup configures it, because its targets share their names with upstream
Strata's; `cmake/multiengine_maya.cmake`, read inside that build, adds one library per Maya program, each compiled
with that program's own compile lines (token for token) and written beside the launcher. Maya's own executables
(`maya.exe`, `maya-ds4.exe`, `maya-mm3.exe`) build in the same place on request, to check the libraries against. To
update Maya: `python -I engine/src/multiengine/sync_maya.py G:\Project-maya <tag>`.

## Build

```
engine\build-glm-g.bat
```
Configures `engine\build-g` (Ninja + CUDA sm_89, ggml from `G:\Strata\third_party\llama.cpp`, the Maya and raon1
libraries on) and builds the target `multiengine`: the launcher and all six libraries, side by side in `build-g`
(Maya's build runs inside it, in `build-g\maya`). Upstream's own `strata` target still builds `strata.exe` as
upstream builds it. A deployment folder needs `multiengine.exe`, the `multiengine_*.dll` of the families it serves and
`strata_kq.dll` together.

## Run and test

**Serve + chat (the normal way to test):** launch the reviewed serve layer pointed at our engine (a Python
command, not a batch file):
```
cd C:\Strata
.venv\Scripts\python.exe -m serve.server --engine strata --config C:\multi-strata\engine\our-qwen.json --port 8080
```
Then the OpenAI/Anthropic API is on `http://localhost:8080` (model `qwen3.8-flash-next-iq3_s`):
```
curl http://localhost:8080/v1/chat/completions -H "Authorization: Bearer strata-secret-key" ^
  -H "Content-Type: application/json" ^
  -d "{\"model\":\"qwen3.8-flash-next-iq3_s\",\"messages\":[{\"role\":\"user\",\"content\":\"Hello\"}],\"max_tokens\":128}"
```
`our-qwen.json` is the launch recipe (our `multiengine.exe` + the qwen model/pack/mtp paths in `C:\Strata-data`,
spec decode, pcie-frac, adaptive tier) — the same settings as Strata's proven `strata-iq3_s.json`.

**One-shot (no server):**
```
engine\build-g\multiengine.exe --pack G:\Strata-data\packs\iq3_s --native G:\Strata-data\models\IQ3_S\...-00001-of-00002.gguf --ple-gguf ...-00002-of-00002.gguf --expert-profile G:\Strata\data\expert-profile.bin --expert-cache auto --spec 4 --mtp G:\Strata-data\mtp\rt --tokens "9707,11,1879,0" --max-new 64 --gpu 0
```

## Status

- DONE: engine source ported into our repo; our own build (`multiengine.exe`); runs + serves qwen (CLI + API).
- DONE (2026-10-27): upstream's engine at v0.2.6 as its own library, GLM (Maya) and Gemma 4 as theirs, the
  launcher between them.
- DONE (2026-10-27): Project Maya v1.1.30 as three libraries (GLM-5.3-Flash, DeepSeek-V4.1-Flash, MiniMax-M3.1-Flash),
  built from Maya's unedited tree; raon1 in a library of its own.
- DONE (2026-10-27): flint on the Strata paper's tier (`src/flint`, design
  `docs/superpowers/specs/2026-10-27-flint-engine-design.md`), checked against llama.cpp's flint; served by the
  frontend's `flint` profile (card `fe-config/cards/flint-q8.json`, in `fe-config/multi-test.json`).
- PENDING (per the plan): **lightweight consolidation** — drop the tests/bench/apps/sycl/metal we don't ship and
  trim the build to the `strata` target + its libraries; then tune.
- The serve layer here is reused from Strata (reviewed clean). Consolidating it into our own lean serve is part
  of the lightweight work.
