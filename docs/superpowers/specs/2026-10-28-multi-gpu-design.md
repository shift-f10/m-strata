# Several GPUs, as upstream runs them (2026-10-28)

The owner: "Also ensure our engine supports multi gpu like the upstream". This records what upstream Strata and Project
Maya do with several GPUs, what each of our engine libraries and launches did, and what changed.

## Upstream's way

- **Upstream Strata** (`upstream/serve/server.py`): the config's `"gpu"` names one card or several (`gpu_list`), numbered
  as nvidia-smi numbers them (`CUDA_DEVICE_ORDER=PCI_BUS_ID`, `CUDA_VISIBLE_DEVICES`). Several cards are a layer split:
  `--layer-split auto` (or the first layer of each later card), unless `--peer-device` makes the second card an
  expert-cache tier; `split_skip_if_fits` runs an auto split on the first card when it holds everything. For an auto
  split it orders the cards so the faster one runs the last stage (`ordered_gpus`, #1352: multiprocessors x clock, a
  VRAM guard, `"gpu_order": "as_given"`).
- **Maya's GLM-5.3 engine** (`glm_model.cu` `load_pack_place`): the layer split is the default on a multi-GPU host;
  `STRATA_GLM_SPLIT=0` forces one device, a list pins the boundaries.
- **Maya's DeepSeek-V4.1** (`maya.py` `write_config_ds4`): `--tp min(2, GPUs)` (two equal GPUs per tensor-parallel group)
  and `--threads on` only on one GPU; **MiniMax-M3.1**: `--gpus` (its tensor parallel is EXL3-only).
- **Pictures** (`setup.py` `VISION`, `server.py` #1322): a GPU encoder keeps 700 MiB free beside the engine (an explicit
  `--vram-reserve-mib 700`) and reads up to 1,024 image tokens; a CPU encoder takes no reserve and 300 tokens.

## Where we stood

| Family | Engine | Launch before this change |
| --- | --- | --- |
| Qwen3.8, Qwen3.6 | upstream's engine: `--gpu` list, `--layer-split`, `--split-skip-if-fits` | composed from `engine.gpu`, `engine.layer_split`, `engine.split_skip_if_fits`, as upstream |
| GLM-5.3, raon1 | Maya's GLM engine (raon1 on our port of it): its layer split | `glm.gpu` as `CUDA_VISIBLE_DEVICES`, but `STRATA_GLM_SPLIT=0` forced unless `glm.layer_split` was pinned: one device |
| DeepSeek-V4.1 | maya-ds4: `--gpus`, `--tp` | `--gpus` composed; the cards fixed `--tp 1 --threads on` |
| MiniMax-M3.1 | maya-mm3: `--gpus` | `--gpus` composed |
| Gemma 4, flint | our engines: one GPU (`--gpu` uses the first) | `--gpu` |

No card offered a choice of GPUs: the rows existed, the page had no control for them.

## What changed

1. **A GPUs choice on every card** (the option `gpus`, kind `devices`): "auto" or GPU numbers 0-63 such as "0,1", none
   repeated, at most 16 (one on Gemma 4 and flint). It writes `engine.gpu` (qwen, deepseek, minimax, gemma4, flint) or
   `glm.gpu` (glm53, raon1); auto writes nothing. The launcher offers it on every card, after Web search.
2. **This PC's GPUs**: `nvidia-smi --query-gpu=index,name,memory.total` once, on the first About request, run as the
   Monitor runs it; the About lists them (`hardware.gpus`) and the page draws one box per GPU, hidden on a one-GPU PC.
3. **GLM-5.3 and raon1 split as Maya's engine does**: several GPUs named and the split left at auto, the forced single
   device is dropped; one GPU or none keeps it; a pinned split wins.
4. **DeepSeek's `--tp` and `--threads` follow the GPUs**: composed at each launch as `write_config_ds4` writes them;
   new cards no longer fix them; a card that names either keeps its own.
5. **Pictures**: upstream's 700 MiB reserve for a GPU encoder (a reserve of the card's own stands) and the encoder's
   image-token default by kind.

## Not done here

- Upstream's card order for an auto split (#1352) needs each card's multiprocessors and clock, which nvidia-smi does not
  give: an engine-side listing (as upstream's `strata-device --list-devices`) is the way. Until then the order is the
  card's.
- Gemma 4 and flint stay one-GPU engines; a layer split in them is engine work.
- Every multi-GPU claim needs a box with two GPUs; this PC has one.
