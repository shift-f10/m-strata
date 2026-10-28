# strata-prepare: native packs, no Python - design (2026-10-27)

## 0. What the owner asked

- "can we make executables to handle what needs to be done so we don't have any other dependencies?" (about the
  Models tab's "download only: the prepare step is not configured (models.prepare_python, models.prepare_tools)").
- "Go ahead: Qwen 3.6 from the GGUF first, then a native strata-prepare packer for the Qwen 3.8 family (byte-for-byte
  against upstream's Python), then GLM."
- "make sure you plan for deepseek in maya tools as well, I bet it's covered as well"
- "Yes continue with the others", then "Then work on the add model" (GLM, DeepSeek and MiniMax after Qwen3.8; then
  "Add a model" for a GGUF already on the PC).
- The page's GLM chat (item 46, launcher spec D10, asked 2026-10-27): "Page omits it" - the page sends
  `reasoning_effort` only to a card whose profile takes it.

## 1. Where things stand

- The engines are built (multiengine: strata v0.2.6, Maya v1.1.30 GLM/DeepSeek/MiniMax, raon1, Gemma 4, flint). What a
  download lacks is the step that turns its GGUF files into what the engine is started with. Today that is
  `tools/secure/prepare_download.py` under a configured Python, calling upstream's `iq_pack.py` (784 lines) and
  `strata_tokenizer.py` (528 lines); the clean out folder ships no Python, so every Qwen download is "download only".
- **Qwen3.6 (done first, 2026-10-27):** upstream runs it with `--native <gguf> --mtp 2` and no pack. Its card is now
  built from the GGUF header (`card_for_gguf`, architecture `qwen35moe`) and its tokenizer read from the GGUF (the
  `qwen` profile joined `tokenizer_from_gguf`); checked on the owner's 35 GB Q8_0 file: the GGUF's 248,320 tokens,
  types and merges equal the pack upstream's Python wrote, and so do the ids of 38 texts. No prepare step remains.
- **Qwen3.8 family** (qwen, swift, coder, gyro, unsloth): upstream runs `--pack <pack> --native <shard1>` (plus
  `--ple-gguf` for at most two shards, `--expert-profile`). The pack is `iq_pack.py`'s: `index.txt`,
  `native_experts.txt`, `dense.bin`, `conversions.json`, `compat-bf16.json` (with `--compat-bf16`, Unsloth and Gyro),
  optionally `hadamard.txt` and `experts.bin`, and `tokenizer/`.
- The engine reads packs in C++ (expert_layout.cpp, core/weights.cpp) and has a header-only GGUF reader
  (`include/strata/artifact/gguf_reader.hpp`) with every block geometry `iq_pack.py` knows; nothing writes a pack.

## 2. The tool

`strata-prepare` (C++20, Windows and Linux), in `engine/src/prepare/`, built by the engine's CMake and installed with
the `runtime` component beside `multiengine` (`out/engine/`). No Python, no network, no third-party code beyond what
the engine already builds (the GGUF reader; ggml's dequantizers for `--compat-bf16`).

```
strata-prepare pack --gguf <shard 1> --out <pack folder> [--compat-bf16] [--experts-bin]
```

- Writes exactly what `iq_pack.py --gguf --out [--compat-bf16] [--experts-bin]` writes, byte for byte:
  `index.txt`, `native_experts.txt` (v3/v4/v5), `dense.bin`, `conversions.json`, `compat-bf16.json`, `hadamard.txt`,
  `experts.bin` + `experts.bin.src.json`. The same refusals with the same reasons, the same publish order (the
  completion marker `native_experts.txt` last, temporary names renamed).
- **Not ported:** `--base` (a Q2_0 canonical pack sharing dense.bin: setup's AVX-512 path, not ours), the tokenizer
  export (the server reads the tokenizer from the GGUF since the Qwen3.6 step), `--skip-experts`.
- **Byte-exact details that need care:** index rows in shard order then each shard's tensor order (Python dict
  insertion order); `json.dumps` formatting (`indent=1` / `indent=2`, `", "` / `": "`, Python's float `repr` - the
  shortest round-trip form, `1e-05` style exponents, `0.0`); BF16 by ggml's round-to-nearest-even (gguf-py's
  `quants.quantize(..., BF16)` mirrors it); F16 by IEEE round-to-nearest-even (numpy's `astype(float16)`); dequantized
  values as gguf-py computes them (ggml's C dequantizers, which gguf-py's numpy versions follow).
- Progress for the server on stdout, one line each, the protocol `prepare_download.py` already speaks: `STEP pack`,
  `DONE`, or `ERROR <reason>` with a non-zero exit.

The Maya families (section 5), each a port of Maya v1.1.30's own Python, byte for byte:

```
strata-prepare pack --maya --gguf <shard 1> --out <pack> [--compat-bf16] [--experts-bin]     GLM-5.3 (and Maya's qwen4exp route)
strata-prepare tokenizer --gguf <gguf> --out <dir>                                            Maya's strata_tokenizer.py
strata-prepare pack --maya-ds4 --model <dir> [--out] [--template <file>] [--engram <dir>] [--shards ...]   DeepSeek-V4.1
strata-prepare pack --maya-mm3 --model <dir> [--out] --template <file> [--indexers <file>] [--shards ...]  MiniMax-M3.1
strata-prepare download --maya --gguf <shard 1> --out <pack>      maya.py pack_step: the pack, PACK_FILES, sources.json
```

- `download --maya` is Maya's setup step: `iq_pack.py --compat-bf16` (Maya's fork), the `PACK_FILES` check
  (`pack_incomplete`), `sources.json` (`stamp_pack`: the shards' names, sizes and `mtime_ns`), and Maya's reuse rule - a
  whole pack of this model (`pack_source`) whose `sources.json` still matches the shards (`pack_stale`) is used as it
  is, not rewritten (`STEP pack`, `STEP sources`, `DONE`; prepared.json's `tokenizer` is `<pack>/tokenizer`).
- Shared code: `pack_common` (the shards as both packers read them - upstream's `check_split`, Maya's none; Python's
  `write_text` CRLF on Windows / `newline="\n"`; the float conversions), `gguf_header` (strings decoded as
  `decode("utf-8", "replace")`, metadata keys in file order), `py_json` (`ensure_ascii=False`, `repr()` of a str).
- **Not ported:** what comes from files other than the GGUF set - DeepSeek's DSpark drafter and vision tower (made by
  `drafter_pack.py` / `vision_pack.py` from DeepSeek's checkpoint) and MiniMax's EXL3 companions: such a model is
  packed text-only and the log says so. Maya's `--base` and `--skip-experts` (its setup never passes them). The
  chat templates DeepSeek and MiniMax read (`tools/ds4` / `tools/mm3/chat_template.jinja`) are passed with
  `--template`: they are not in our tree (the MiniMax one is under the MiniMax Community License, PROVENANCE).

## 3. Proof

- Unit tests on synthetic GGUFs (each refusal, each conversion, v3/v4/v5 tables, split shards).
- **Parity, the owner's bar:** a dev-only script runs upstream's `iq_pack.py` (with the repository's dev Python) and
  `strata-prepare` on the same GGUFs and compares every file byte for byte: synthetic GGUFs written with gguf-py
  (every FORM conversion, `--compat-bf16` over the quant types, a split model with a layer across shards), and the
  owner's real Qwen3.8 IQ3_S (`G:\Strata-data\models\IQ3_S`, two shards). Python stays a test tool only.

## 4. The server

- A Qwen3.8-family download runs `<engine folder>/strata-prepare pack ...` instead of the Python script (same
  progress lines), then writes the card: `--pack`, the first shard as the model (`--native`, from roots.models),
  `--ple-gguf` when there are at most two shards, `--expert-profile` from the bundled profiles; the tokenizer from the
  GGUF. `models.prepare_python` / `models.prepare_tools` keep working where an operator set them.
- The out folder bundles upstream's and Maya's expert profiles (`expert-profile.bin`, `expert-profile-coder.bin`,
  `expert-profile-glm.bin`, `expert-profile-ds4.bin`; 60-192 KB each; their licences in PROVENANCE).

## 5. GLM-5.3, DeepSeek and MiniMax (Maya v1.1.30)

None of Maya's packs is downloaded ready-made; each is built locally from GGUFs by a Python script, and no C++ in
Maya's or upstream's tree writes one (research of 2026-10-27; `M:` = `git -C G:\Project-maya show v1.1.30:`).

| Family (program) | Engine input | Built by | Port as |
|---|---|---|---|
| GLM-5.3 (maya, `--glm-pack`) | `<gguf dir>/pack`: index.txt, native_experts.txt, dense.bin, tokenizer/ (M:maya.py:640) | Maya's fork of `iq_pack.py --compat-bf16` (492 lines; numpy, gguf-py) + Maya's `strata_tokenizer.py` (GLM pre-tokenizer) | `strata-prepare pack --family glm`: Maya's table (`BF16_PROJECTIONS`, `GLM5NEXT_BF16`, `GLM5NEXT_NATIVE_OK`, arch whitelist, 3-D/4-D tensors indexed as 2-D, NextN tensors dropped, trunk = block_count - nextn_predict_layers) on the same writer; tokenizer/ written natively |
| DeepSeek-V4.1-Flash (maya-ds4, `--pack`) | `<model>/maya-ds4`: ds4.txt, tensors.txt, engram.bin, tokenizer/ (M:maya.py:4088; engine reads at ds4_model.cu:354, :411) | `tools/ds4/pack_gguf.py` (619 lines, standard library only): header parse, LAYER/GLOBAL name maps, deepseek41.* geometry, Engram/PLE keys from a side set, experts split per expert, `wo_a` per group | `strata-prepare ds4`: a direct port (an index of the GGUFs plus engram.bin; no tensor data copied) |
| MiniMax-M3.1-Flash (maya-mm3, `--pack`) | `<model>/maya-mm3`: mm3.txt, tensors.txt, tokenizer/ (M:maya.py:4509) | `tools/mm3/pack_gguf.py` (381 lines; reuses ds4's GGUF reader, pulls numpy through imports) + a `*-maya-indexers.gguf` side file | `strata-prepare mm3`, after ds4 (it shares ds4's reader and maps) |

- Upstream's own DeepSeek (`tools/dsv41/pack_gguf.py`, 914 lines, a port of Maya v1.1.17) writes the same file family
  with extra dtypes and options; our multiengine runs DeepSeek on Maya's program, so Maya's builder is the reference.
- The download catalog (generated from Maya v1.1.27) had five GLM entries marked `"prepare": "unsupported"` (now
  `"maya_pack"`) and has no DeepSeek or MiniMax entries yet.
- Parity for each: Maya's script and `strata-prepare` on the same GGUFs, every file compared byte for byte (the owner's
  GLM file `G:\Maya-data\models\Maya-S-v2-IQ2_XXS`; DeepSeek and MiniMax on synthetic GGUFs unless their files are
  downloaded).

## 6. Phases (TDD; RED recorded for each)

1. Qwen3.6 from the GGUF (done).
2. `strata-prepare pack` for the Qwen3.8 family, parity-proven. *Done 2026-10-27: `engine/src/prepare` (standalone
   CMake, ggml-base static), 11 unit tests (RED on a stub build first), and the parity script found every file
   byte-identical to upstream's `iq_pack.py`:*
   - *six synthetic cases: plain, `--experts-bin`, a `--compat-bf16` refusal, `--compat-bf16` over 12 quant types (17
     of 25 conversions rounded), a split model (v4), Hadamard (v5);*
   - *the owner's real Qwen3.8 IQ3_S (two shards, 78 GB; a 1.5 GB dense.bin).*
3. The server runs it; the bundled expert profiles; Qwen3.8 downloads become cards. *Built 2026-10-27:
   `DownloadManager::native_prepare` (`strata-prepare download`, the pack in `<store>/packs/<id>`, card with
   `--pack` / `--ple-gguf` / `--expert-profile` and the tokenizer from the GGUF, the pack removed with the model);
   make.py and the engine CMake install `engine/strata-prepare` and `engine/profiles/`. Not yet: "Add a model" for a
   Qwen3.8 GGUF already on the PC (card_for_gguf still refuses `qwen4exp`).*
4. GLM (Maya's table), then DeepSeek (`ds4`), then MiniMax (`mm3`), each parity-proven against Maya's script, with
   their catalog entries. *Done 2026-10-27:*
   - *`maya_pack.cpp` (Maya's `iq_pack.py`), `tokenizer_export.cpp` (Maya's `strata_tokenizer.py`), `maya_moe_pack.cpp`
     (`tools/ds4` and `tools/mm3/pack_gguf.py`), `download --maya`; 21 unit tests (RED on stub builds, and on the
     reuse rule switched off);*
   - *parity, every file byte-identical to Maya's Python: 17 synthetic cases (GLM with `--compat-bf16`, refused,
     `--experts-bin`, the setup step, unsloth's `glm5next`, a fail-closed refusal, three shards with a split layer;
     Maya's qwen4exp route; the tokenizer for glm4 / qwen35 / deepseek3; DeepSeek with the Engram tables three ways;
     MiniMax with and without its indexers file), and the owner's real GLM-5.3-Flash Maya-S-v2-IQ2_XXS (three shards,
     95 GB; all 10 files, 22 s for both sides; its index.txt, native_experts.txt and sources.json also equal the pack
     Maya's setup had built in the model's folder);*
   - *GLM downloads become cards: the catalog's five GLM entries are `"prepare": "maya_pack"`; the pack goes in
     `<store>/packs/<id>/pack` beside hard links to the store's shards (the store's version folder is never written);
     the card is glm53 (D10 met: GLM cards and Maya's options; the bundled `expert-profile-glm.bin` named on every GLM
     launch, as Maya's setup does). DeepSeek and MiniMax packs are not wired: the frontend has no chat profile for
     either yet (DeepSeek's is a placeholder, MiniMax has none) - a follow-up.*
5. "Add a model" for a GGUF already on the PC. *Done 2026-10-27 (committed 7ac4d16f):* a Qwen3.8 (`qwen4exp`) file has its pack built in
   `<roots.state>/packs/<card>` (`--compat-bf16`, so any quant packs; the model folder untouched; the expert profile by
   its expert set: 512 `expert-profile.bin`, 256 the Coder's), a GLM-5.3 file in `pack/` beside its shards as Maya's
   setup puts it (`pack-<name>/` when `pack/` holds another model's; an up-to-date pack reused), then the card, as a
   download's (one `card_for_pack` for both).
   *Raon-1-mini, 2026-10-28 (owner: "when I selected the model for raon1 ... it did not add the card"):* `pack --maya`
   and `download --maya` take architecture `raon1` as our additions to Maya's Python do (`engines/maya-ref/tools`:
   `RAON1_NATIVE_OK`, no BF16 upgrade, 3-D tensors, a quantized tensor outside the set dequantized to F32; the `raon`
   pre-tokenizer). Parity: `--raon-tools` (Maya v1.1.30's tools with that diff), 9 cases byte for byte including the
   owner's Q8_0 and IQ4_XS GGUFs. The pack goes in `pack/` beside the file as GLM-5.3's; the card is the managed raon1
   card's with the GLM engine's typed menus (contexts 8K-256K, the KV cache).

## 7. DeepSeek-V4.1-Flash and MiniMax-M3.1-Flash in the frontend (owner: "continue to the next pieces I asked for")

What is left of "GLM, DeepSeek, MiniMax" and "Add a model ... for the others": chat profiles, cards, options, launches,
downloads and Add a model for the two other Maya families. Reference: Maya v1.1.30 (`M:` as above); the facts below
were read there, file and line cited where they decide something.

### 7.1 What Maya does (both families)

- One engine each, already ours: `multiengine --pack <dir>` runs `multiengine_maya_ds4` for a pack holding `ds4.txt`
  and `multiengine_maya_mm3` for `mm3.txt` (launcher.cpp:261-262).
- Wire: GLM's `maya_118` dialect, except `READY <ctx> stop` (no `pause`, no `multi=`), INFO/VRAM/NOTE before READY, and
  `ERR` followed by `DONE ... cancel` on some errors (M:src/ds4/ds4_main.cpp:383, :1006-1007; M:src/mm3/mm3_main.cpp:287,
  :315-330). The engine prints the stop token as a `T` line before `DONE` (ds4_main.cpp:1125-1135; mm3_main.cpp:473-485)
  but stops only on its EOS (DeepSeek id 1; MiniMax the pack's `eos_token_id`). Maya's server stops on the turn markers
  itself and sends STOP (M:serve/server.py:1749-1764, :1019-1044); a STOP read after the engine's own DONE is cleared at
  the next GEN (ds4_main.cpp:846, mm3_main.cpp:312).
- Pack (GGUF route): `<model>/maya-ds4` (pack_gguf.py: ds4.txt, tensors.txt, engram.bin, tokenizer/ with
  `{"pre":"deepseek3"}`; `expert-profile.bin` seeded from `data/expert-profile-ds4.bin`, M:maya.py:2590-2616) and
  `<model>/maya-mm3` (mm3.txt, tensors.txt, tokenizer/ with `{"pre":"minimax"}`; the `*-maya-indexers.gguf` file is one
  of the shards).
- Tokenizer: Maya's `strata_tokenizer.py` - `deepseek3` is three "Isolated" splits (digits in threes, CJK/kana runs, a
  word pattern), `minimax` is NFC and GPT-4o's o200k regex; byte-level BPE, the lowest rank first, no ignore_merges.

### 7.2 DeepSeek-V4.1-Flash-1017 (profile `deepseek`)

- **Prompt** (DeepSeek's own encoder, MIT, M:serve/ds41_encoding.py): `<｜begin▁of▁sentence｜>`; with thinking,
  `<｜System｜>Reasoning Effort: N (range 1-100, the higher the value, the more thorough the reasoning)\n\n` then the
  system text; without, `<｜System｜>` + text only when there is a system message; `<｜User｜>U`; after a user turn
  `<｜Assistant｜>` + `<think>` (the last user turn, thinking) or `</think>`; an assistant turn `C<｜end▁of▁sentence｜>`
  (earlier reasoning dropped, Maya's default). Effort: low 50, medium and high 75 (Maya maps medium to high); none closes
  the think block (chat mode). Our renderer takes an optional leading system message, then user and assistant in turn,
  ending with a user; anything else is refused, as the GLM renderer does.
- **Reply**: reasoning first with thinking (the prompt ends in `<think>`), `</think>` to the answer; it ends on
  `<｜end▁of▁sentence｜>` or `<｜User｜>`, both through the new `server_stop_tokens` (the frontend sends STOP, as Maya).
- **Tokenizer**: a new `deepseek_bpe` mode - the `deepseek3` splits on exact Unicode classes, the BPE `raon_bpe` already
  runs. The classes are generated from Python's `regex` module (the one Maya's server uses, unpinned in its setup) into
  `regex_unicode_data.inc`, and the split is checked against Maya's own `_IsolatedSplits` on a fixture of texts.
- **Card**: profile deepseek on `<pack>`; options context 32K/64K/128K/256K/512K/1M (default 64K, Maya's), thinking
  none/low/medium/high (default high, Maya's DS4_EFFORT), temperature 1.0, top_p 1.0 (Maya's DS4_SAMPLING); per-request
  `reasoning_effort` taken. Launch: `--pack <pack> --max-context <n> --tp 1 --threads on --vision off` (Maya's
  write_config_ds4 for one GPU and no vision tower; the expert profile is the pack's own `expert-profile.bin`).
- **Packs**: `strata-prepare download --maya` learns the DeepSeek GGUF (Maya's `<model>/maya-ds4`, the profile seeded,
  sources.json); downloads of Maya's catalog (M:tools/ds4/download_gguf.json) and Add a model use it.
- Not on this PC: every DeepSeek GGUF is 277-508 GB, so no end-to-end run here; proof is the unit tests, the tokenizer
  fixture and the packer's parity.

### 7.3 MiniMax-M3.1-Flash (profile `minimax`, new)

- **Prompt** (Maya's `tools/mm3/chat_template.jinja`, MiniMax's own with Maya's thinking modes): `]~!b[]~b]system\n` +
  MiniMax's default system text + its thinking instructions + the mode line + `<model_id>8bb8149fe</model_id>` (+
  `<effort>L</effort>`) + `[e~[\n`; a client system message is the developer block; `]~b]user\nU[e~[\n`;
  `]~b]ai\n</mm:think>C[e~[\n`; the tail `]~b]ai\n` (adaptive thinking) or `]~b]ai\n</mm:think>` (off).
- **Reply**: reasoning first with thinking (`<mm:think>` dropped, `</mm:think>` to the answer); it ends on `[e~[` or
  `]~b]` (server-side stops).
- **Tokenizer**: `minimax_bpe` - NFC per plain segment, the o200k regex on the same generated classes.
- **Licence, the owner's decision**: the template's system text is MiniMax's, under the MiniMax Community License
  (non-commercial use and redistribution with its notice; commercial use needs "Built with MiniMax M3.1-Flash" and a
  notice to MiniMax). Our renderer would carry that text in our tree.
- Card: contexts 32K..1M (default 64K), KV bf16/int8/fp8/fp4 (default int8), thinking off/on, temperature 1.0, top_p
  0.95; launch `--pack <pack> --max-context <n> --profile <engine>/profiles/expert-profile-mm3.bin --kv-streaming on
  --vision off` (Maya's write_config_mm3 for a GGUF). Maya-XS (93 GB) would fit this PC for an end-to-end run.
- **Done 2026-10-28** (owner: "add MiniMax-M3.1-Flash under the MiniMax Community License"; handoff entry v):
  - Maya v1.1.30's `LICENSE.minimax-m31`, `tools/mm3/chat_template.jinja` and `tools/mm3/download_gguf.json` vendored
    byte for byte in `engine/third_party/maya` (`sync_maya.py`); the install ships the template and the licence in
    `engine/templates/minimax-m3.1/`.
  - `minimax_bpe`: NFC per plain span, then MINIMAX_PATTERN on the regex module's classes (`minimax_split`): the 471
    texts of `maya_pretokenize_texts.json` split as Maya splits them.
  - `render_minimax_text_prompt`: Maya's template for text chats, byte for byte against Maya's own ChatTemplate (15
    cases, `tests/fixtures/minimax_prompts.json`). The template's five literals are special tokens where the pack has
    them and text where it does not (Maya's parse_special); message text is plain (Maya's mark_literals). Thinking:
    adaptive with no effort (Maya's `auto`, the card's default), an effort (low, medium, high; the template also takes
    xhigh and max), or off (`thinking_mode` "disabled").
  - The reply: reasoning first unless off, `<mm:think>` dropped, `</mm:think>` to the answer, the newlines after it kept
    (Maya's parser has no lead rule for MiniMax); `[e~[` and `]~b]` are server stops.
  - The `minimax` profile: maya_118 wire, `maya_mm3` allowlist (context, `--kv-cache`, KV streaming and its window,
    chunk, CPU threads, VRAM kept free), the composition above with the shipped profile, `--gpus` for named GPUs.
  - Cards: thinking is auto/none/low/medium/high (auto writes no `reasoning.effort`). Add a model and downloads build
    the pack in `maya-mm3/` (`maya-mm3-<name>/`) or `<store>/packs/<id>` with `strata-prepare download --maya
    --template`, Maya's mm3_pack_step; the set's indexers file is the folder's `*-maya-indexers.gguf` whose
    `maya.indexers.for` names the set. The catalog carries Maya-XS and Maya-S.
  - Not done: no end-to-end run (no MiniMax GGUF on this PC; Maya-XS is 93 GB); MiniMax's tool calls and pictures.

### 7.4 Phases (TDD; RED recorded for each)

1. The generated Unicode classes and `deepseek_bpe`, against Maya's splitter. *Done 2026-10-27 (entry p): 464 fixture
   texts piece for piece.*
2. `server_stop_tokens` in the generation loop. *Done.*
3. DeepSeek's renderer, encoder and plan. *Done: five prompts byte for byte from Maya's encoder.*
4. The deepseek profile wired: dialect, child environment, tokenizer mode, effort, cards, options, launch. *Done.*
5. DeepSeek packs: `download --maya`, Add a model, downloads and the catalog. *Done (four catalog entries).*
6. MiniMax the same way, once the owner has decided on the licence. *Waiting on the owner.*
