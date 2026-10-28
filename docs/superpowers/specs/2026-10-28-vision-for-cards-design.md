# Vision for model cards (2026-10-28)

The owner: "a checkbox to also download vision files ... check the upstream on how they do it", with the engine able to
read the pictures. This follows upstream Strata's way (`upstream/setup.py`, `upstream/serve/server.py`,
`upstream/tools/vision`) and reuses the image path this server already has for its single-model start
(`VisionSupervisor`, `VisionService`, `image_payload`, the chat route's GENI path).

## Upstream's way

- **The files.** Each family pins its picture encoder, an `mmproj` GGUF, by repository, revision, size and SHA-256
  (`setup.py`: `mmproj_hf`, `mmproj`, `MMPROJ` sums; Qwen3.8-Flash-Next's `mmproj-Qwen3.8-Flash-Next-BF16.gguf`,
  Qwen3.6's `mmproj-F16.gguf`). Setup downloads it when vision is chosen (`--vision yes|no|gpu|cpu`).
- **The encoder.** `strata-vision` (`tools/vision`, 343 lines over llama.cpp's mtmd at the engine's pinned commit) reads
  `--mmproj`, `--model` (the text model's first GGUF), `--gpu`, `--threads`, `--max-tokens`, `--min-tokens`,
  `--flash-attn`; prints `READY <n_embd>`, answers `ENC <image> <out>` with `OK <tokens> <nx> <ny> <ms>` or `ERR`, ends on
  `QUIT`.
- **The engine.** Started with `--vision`; a request's pictures reach it as the encoder's embeddings (`GENI`).
- **The server.** A `vision` section (`exe`, `mmproj`, `model`, `gpu`, `threads`) starts the encoder beside the engine.

## Scope of this change

Qwen cards first (Qwen3.8-Flash-Next packs and Qwen3.6 GGUFs), the families upstream's engine reads pictures with and our
`multiengine_strata.dll` carries. The other profiles stay text only (their engines here have no picture path wired).

1. **Build.** make.py builds upstream's `tools/vision` out of source (never editing `upstream/`) with ggml-cuda for the
   engine's GPU targets, into `engine/strata-vision(.exe)` beside the launcher; `--without vision` leaves it out.
2. **Downloads.** A catalog entry with an `mmproj` (23 have one, from upstream's and Maya's pins) shows a "Vision files"
   checkbox with its size; checked, the pull's manifest lists the mmproj as a second role (`mmproj`, its own repository
   and revision), and the space check counts it.
3. **Cards.** Two new card-owned rows: `vision.mmproj` (the encoder file, absolute) and `vision.gpu` (encode on the GPU).
   A qwen card with `vision.mmproj` lists a `vision` option - off, gpu (the default) or cpu - writing `vision.enabled`
   and `vision.gpu`. A download with its vision files, and Add a model on a folder holding one `mmproj*.gguf`, make such
   cards.
4. **Launch.** A card's launch with vision on adds `--vision` to the engine and hands the encoder the mmproj and the
   model's first GGUF; the per-card runtime owns the encoder (`strata-vision` beside the engine, started with the model,
   stopped with it) and opens the schema's image gate. The encoder gets `--gpu` from `vision.gpu`, and
   `vision.no_flash_attn` becomes upstream's `--flash-attn off`.
5. **The page.** The chat attaches pictures (PNG, JPEG, WebP) when the running card reads them, sent as OpenAI
   `image_url` data URLs; the size and count limits are `vision.max_side` and `vision.max_n`.

## Security

Pictures arrive only as `data:` URLs (an http(s) image URL stays refused, never fetched); the encoder is a confined child
like the engine (no shell, a fixed environment, the scratch folder `<state>/vision` its working directory); the mmproj
must sit in the card's model folder or the download store and pass the model folders' privacy rule.
