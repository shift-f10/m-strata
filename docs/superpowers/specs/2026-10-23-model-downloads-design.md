# Model downloads on the Models tab — Hugging Face GGUF files, with progress and a disk check

Date: 2026-10-23. **Status: ACCEPTED — O1-O4 are the owner's words below; D1-D6 and D8 as recommended
("Yes, build it"); D7 the other way: "Build packs automatically".** Plan:
`docs/superpowers/plans/2026-10-23-model-downloads.md`.

Owner, 2026-10-23, while the launcher's dropdowns were being built:
- O1: "for models supported by strata and project maya, create a card for each to allow downloading
  and show status of download on the same tab."
- O2: "when downloading models, it should show the disk space available so it can say insufficient
  disk space when there is not enough space in the data drive"
- O3: "allow a custom download by pasting a huggingface link"
- O4: "make sure there is a safety in the download that only downloads from huggingface and a gguf"

## 1. What changes in the security contract

The threat model ("Privacy and offline operation") says network use happens only through a
foreground `strata pull`/`update`/`run` the operator confirms, and "None can be invoked by the HTTP
API". `privacy_test` enforces that `strata-unified-server` links no acquisition or downloader code.
O1/O3 ask for the opposite: the page starts a download. The design keeps as much as it can:

- **The server stays without network code.** It starts the existing `strata` program as a separate
  supervised child for each download (`strata hf-fetch ...`, a new verb), exactly like the engine:
  no shell, fixed argv, closed environment, the Job / process group. Inside it, SP4's confined
  downloader (the platform curl under a restricted token / Landlock, URL on stdin, each redirect
  checked, `--resolve` pinning) does the transfer. `privacy_test` keeps passing.
- **O4, enforced twice** (the page never decides): a source is `https://huggingface.co/<owner>/<repo>`
  at a resolved commit, a file path ending in `.gguf` (case-insensitive), nothing else; redirects
  may go only to Hugging Face's own download hosts (`*.huggingface.co`, `*.hf.co`,
  `cas-bridge.xethub.hf.co`); the server checks the request, and the child checks every URL and hop
  again. No other host, scheme, port, or file type can be fetched.
- **Every file has its size and SHA-256 before a byte is written** (the catalog's pins, or Hugging
  Face's own LFS metadata for a pasted link), checked against free space first (O2) and verified at
  the end; a mismatch deletes the file.
- **Off unless switched on:** `models.downloads` (default false), keyed, loopback clients only unless
  `models.control_remote` (the launcher's D2). Every download is logged (what, size, client class).

## 2. The catalog (O1)

Generated from the pinned references by a tool (`tools/secure/download_catalog.py`, reading
upstream `setup.py` `MODELS`/`REPO_PINS`/`HF_REVISIONS` and Maya's model table; facts only, no code),
committed as `secure_frontend/catalog/downloads.json`, embedded at build time:

| Family | Variants (GGUF only, per O4) | Source | Pins |
| --- | --- | --- | --- |
| Qwen3.8-Flash-Next GSQ-RCO | Q2_0 66.4 GB, IQ2_XS 68.0, IQ3_XXS 75.8, IQ3_S 83.6 | ISTA-DASLab | commit + size + SHA-256 per file |
| Swift 1.5 | IQ2_XS, IQ3_XXS, IQ3_S | ukisai | same |
| Coder | IQ1_M 58.4 | ISTA-DASLab | same |
| Unsloth | UD-IQ4_XS 93.7, UD-Q4_K_XL 111.3, UD-Q5_K_XL 158.3, UD-Q6_K_XL 169.2, Q8_0 188.2 | unsloth | same |
| Gyro | S 58.5, M 92.0 | agentionai | same |
| Qwen3.6-35B-A3B | three sizes (22.9-37.8) | unsloth | same |
| GLM-5.3 Flash (Maya) | Maya-S-v2, S24, M, L, M-Derisked (94.7-156.3 GB) | peasantsmith | SHA-256 per shard; **revision `main`, no sizes** |
| mmproj (vision) | Qwen 0.91 GB, GLM 1.14 GB | as above | as above |

Left out by O4: the NVFP4 packs and safetensors, EXL3, the Qwen MTP layer (safetensors, range-fetched).

## 3. Decisions (recommendations in **bold**)

| # | Decision | Options |
| --- | --- | --- |
| D1 | Contract change (§1) | **downloads from the page through a separate confined `strata` child, off unless `models.downloads`; the server keeps no network code**; or downloads stay CLI-only |
| D2 | Maya's unpinned GGUFs (`revision: main`, no sizes) | **resolve `main` to a commit and read sizes from Hugging Face when the catalog is generated, then pin both (a later upstream change is a catalog update)**; or leave the GLM cards out |
| D3 | A pasted link (O3) | **one `.gguf` file link (`/resolve/` or `/blob/` at a branch or commit; a split model's first shard brings its siblings); size and SHA-256 from Hugging Face's API at the resolved commit, shown, and confirmed before the transfer** |
| D4 | Free space (O2) | **the drive holding `roots.models`: free space on the tab; a download needs its size + 2 GiB to start (partial files count as already written); otherwise "Insufficient disk space: N GB needed, M GB free"** |
| D5 | Where files land | **`<roots.models>/downloads/<owner>/<repo>/<file>` (owner-only, `.part` then rename), so the expert inventory sees them** |
| D6 | One at a time | **one transfer, the rest queued; Cancel; a `.part` resumes after a restart** |
| D7 | Runnable after download | **v1 downloads and verifies; the card then says what a start still needs: Qwen GGUFs need upstream's `iq_pack.py` (Python with numpy) to build the native pack, and the GLM ones need the GLM engine (not built); a model card is written by the operator** — or build packs automatically (needs upstream's Python deps; its own plan) |
| D8 | Status | **queued / downloading (bytes, %, rate, ETA) / verifying / done / failed (fixed reason) / cancelled, per file and per card; the tab polls every 2 s while a transfer runs** |

## 4. API (keyed; only with `models.downloads`)

- `GET /v1/strata/downloads`: the catalog cards (name, family, variant, total size, files, state,
  progress, whether runnable here and what it needs), the free space, the transfer queue.
- `POST /v1/strata/downloads/start` `{"id": "<catalog id>"}` or `{"url": "<huggingface .gguf link>"}`
  (a pasted link first answers `{"confirm": {...size, sha256, commit...}}`; a second call with
  `"confirm": true` starts it).
- `POST /v1/strata/downloads/cancel` `{"id": ...}`.

## 5. Testing

Hermetic: the link grammar (O4: every refused host, scheme, port, path, extension, redirect), the
catalog's generated pins, the disk-space arithmetic and refusal text, the child's progress protocol
on a fake curl (the existing `fake_curl` harness), resume, cancel, a SHA-256 mismatch, the routes'
switch and key. On the box: one small catalog GGUF (the Qwen mmproj, 0.91 GB) and one pasted link
end to end, an oversized one refused for space.

## 6. Our own pins: flint, raon-1-mini, Gemma 4 and its MTP file (owner 2026-10-28)

Owner: "there are no download cards for the following: flint ..., raon1 ..., gemma4 ..." and "gemma4 has an mtp ...
make sure we work that mtp in our gemma engine".

- **The pins.** Neither upstream's `setup.py` nor Maya's tables name these models, so the catalog gains a third
  source: `tools/secure/catalog_pins.json`, ours (catalog `source` `m-strata`; the page shows it as "Built from"),
  read by the generator's `pin_entries`. Each file is pinned by commit, size and SHA-256 as read from Hugging Face's
  HEAD headers on 2026-10-28, each entry names the licence its model card names (stated on the card), and anything
  unpinned, unknown or misspelt drops the entry (reported). PROVENANCE lists the values.
- **flint Q8_0 and Gemma 4 26B-A4B Q8_0** (`prepare` `none`): a card is made from the GGUF the moment it is verified
  (§4.3 of the GUI-first spec; `models.default_engine` set). **raon-1-mini Q8_0** (`maya_pack`): packed as a raon1
  folder card is - the GGUF hard-linked into `<store>/packs/<id>/`, `strata-prepare download --maya` into `pack/`
  there, the marker `raon1-mini.gguf` - then its raon1 card.
- **Gemma 4's MTP draft file.** A typed optional `mtp` file of a catalog entry (the image encoder's shape, its
  repository and revision the entry's unless it names its own; Gemma 4 entries only, pinned to a commit with its
  size). It is always fetched with the model (0.46 GB), counted in the card's size and file count, pulled as a
  `weights` file of the same manifest, and handed to the card: `engine.args` `--mtp <the stored file>` and a boolean
  `mtp` option, on by default, listed only on a card that has the file; off drops `--mtp <file>` for that start. A
  Gemma 4 card made from a folder finds the folder's one `mtp-*.gguf` (beside the model or in `MTP/`; none or two:
  none), held to the model folders' rule. The Gemma engine's allowlist takes `--mtp` with an absolute `.gguf` path,
  and each launch checks the file as a card's picture encoder is checked (a regular file, not a link, in a folder no
  other user may change). A moved model folder takes an MTP file inside it along.
