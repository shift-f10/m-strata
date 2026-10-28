# M-Strata (Multiengine Strata)

**M-Strata** - Multiengine Strata - is our own inference engine for large mixture-of-experts (MoE) language models on a single consumer GPU, plus a
security-first server in front of it. The engine keeps a model's hot experts in VRAM and the rest in pinned RAM
(read from disk when RAM runs out), and computes some of them on the CPU while the GPU works: the expert tiering
described by Strata (Niko1221). It is built from Strata's and Project Maya's sources with llama.cpp/ggml's quantized
kernels, extended for the model families below. Nothing here is released yet.

The project's live status is [`docs/AGENT_HANDOFF.md`](docs/AGENT_HANDOFF.md) (newest change first); this page
summarizes it as of 2026-10-28. All GPU numbers are from the development system below.

- [Features](#features)
- [What is in the repository](#what-is-in-the-repository)
- [Models](#models)
- [Development system](#development-system)
- [What this system could not test](#what-this-system-could-not-test)
- [Install](#install)
- [Configure](#configure)
- [Use](#use)
- [Troubleshooting](#troubleshooting)
- [Current progress and open items](#current-progress-and-open-items)
- [License](#license)
- [Sources and credits](#sources-and-credits)

## Features

**The engine**

- **One engine for many model families:** `multiengine` loads one library per family (Qwen3.8, Qwen3.6, GLM-5.3,
  DeepSeek-V4.1, MiniMax-M3.1, Raon-1-mini, Gemma 4, flint); the server starts the right one for the model you pick.
- **Expert tiering for MoE models larger than VRAM:** the hot experts stay in a VRAM cache, the rest in pinned RAM
  (read from disk when RAM runs out), and some are computed on the CPU while the GPU works, as Strata describes.
- Batched prompt processing, speculative decoding (the model's MTP draft layer and prompt lookup on Qwen), several KV
  cache formats, KV streaming for long contexts, and contexts up to 1M tokens where the model allows it.
- **`strata-prepare`** builds the packs Qwen3.8, GLM-5.3, DeepSeek-V4.1, MiniMax-M3.1 and Raon-1-mini need, natively
  (no Python), with the same bytes as upstream's and Project Maya's tools.
- **Pictures** on Qwen3.8 and Qwen3.6: upstream's picture encoder, `strata-vision` (llama.cpp's mtmd), runs beside the
  engine and turns each picture into the embeddings the engine reads, on the GPU (about 1 GB of VRAM) or the CPU.
- **Several GPUs**, as upstream and Project Maya run them: Qwen splits its layers across the GPUs you pick, GLM-5.3 and
  Raon-1-mini split theirs (the GLM engine's split), DeepSeek-V4.1 runs tensor-parallel pairs, MiniMax-M3.1 takes the
  list; Gemma 4 and flint run on one GPU.

**Model selection and engine swap**

- **Model cards:** every model you add or download becomes a card on the page's **Models** tab, with the options its
  engine takes: context length, KV cache format, thinking level, temperature, top-p and the other sampling defaults,
  and web search. The page remembers each card's last options; **Defaults** resets them.
- **Start, Stop and swap:** Start runs the card you pick; starting another card swaps the engine. The new model and its
  tokenizer are checked before the running one is touched, the answer in progress may finish (up to 60 s), then the old
  engine stops and the new one loads; a start that fails never takes the running model down. One model runs at a
  time, and Stop gives its GPU and RAM back.
- **GPUs:** on a PC with several GPUs, each card has a box per GPU (as nvidia-smi numbers them); none ticked is the
  first GPU. About lists this PC's GPUs.
- **Calibrate** tunes the settings that depend on this PC by timing real engine starts, once per card (today flint's
  CPU expert lane, PCIe share and CPU threads, and Gemma 4's CPU threads; the other engines tune themselves).
- **Add a model** from files already on this PC (a folder and its `.gguf`); the pack is built when the family needs one.
  A Qwen model whose folder holds its picture encoder (one `mmproj*.gguf`) gets a card that reads pictures, with a
  **Pictures** option (GPU, CPU or off).
  Each card shows its folder (move the files and point the card there); the **danger zone** deletes a model's files,
  card and calibration after three confirmations.

**Download manager**

- **A built-in catalog** of the models this project runs (Qwen3.8-Flash-Next with its Coder, Gyro, Swift and Unsloth
  quants, Qwen3.6, GLM-5.3, DeepSeek-V4.1 and MiniMax-M3.1 in Project Maya's packs), every file pinned by its SHA-256,
  or **any Hugging Face link** to a `.gguf` (a split model's whole set), shown for confirmation first.
- Into a **drive or folder you choose**, its free space shown and checked first (plus a 2 GiB margin), or a new private
  folder.
- **16 connections at once** for a large file (`pull.connections`, 1-32), a progress line with speed and time left,
  **Pause** and **Resume** (it continues from the pieces already fetched), **Cancel** (deletes the partial files), and a
  queue: one download runs, the next starts when it ends.
- **Vision files:** a Qwen entry's checkbox also fetches its picture encoder (about 0.9 GB, pinned like the model);
  its card then reads pictures.
- When the files are verified, the model is packed if it needs it and appears as a card, ready to start.
- The transfer is a separate confined process (`strata pull`): HTTPS to Hugging Face only; the server itself has no
  network code.

**Notifications**

- On the page: a notice when a download is queued, paused, cancelled, finished, failed or ready, and when a model is
  running, stopped or did not start.
- **Email alerts** (a build with `email`): over SSL/TLS or STARTTLS, for downloads finished or failed, calibrations
  finished or failed, the engine stopped unexpectedly, and the server started. "Fill in IONOS" presets one provider,
  "Send a test email" checks it, and the password is stored apart and never shown again (DPAPI on Windows).

**Web search through MCP and SearXNG**

- A build with `search` has `strata-search`, an MCP server the server runs as a private child, which asks **your own
  SearXNG instance**. Qwen3.6, Qwen3.8, GLM-5.3, Raon-1-mini, flint and Gemma 4 models get a `web_search` tool, each in its own
  template's form, and decide when to search.
- Up to `search.max_rounds` searches per answer (8 by default), then the model answers from what it found; the chat
  shows each search and its results in a box, and the API returns them as `strata_search` events.
- On or off per card, per chat (the chat settings' switch) and per request (`"strata_search": false`); a test search
  in the Settings tab shows what a model reads; a search that found nothing says why.

**The page and the API**

- **Chat** with Markdown, the model's reasoning in its own block, text-file and picture attachments (PNG, JPEG, WebP,
  while the running card reads pictures), saving a chat as Markdown, and
  the sampling drawer (thinking, temperature, top-p, max tokens, seed). **Monitor:** GPU load, VRAM, temperature, power,
  PCIe, CPU and RAM, decode and prefill speed, the context fill, the experts in each tier, and the recent requests (never
  their text). **About:** the engines this folder was built with and this PC's hardware. Light and dark; fits a phone.
- **OpenAI-compatible API:** `/v1/chat/completions` (streamed or not, with usage and `reasoning_effort`),
  `/v1/models`, `/health` and `/metrics`.

**Security**

- An API key on every route, shown once through a one-time login link; `strata key show` and `key rotate`.
- Loopback by default: serving the LAN, wildcard binds and model control from other PCs are separate opt-ins (control
  can be limited to the hosts and subnets you list), with Host and Origin checks and a strict page policy.
- The engine runs as a private child with no shell and a fixed environment. The server tokenizes every prompt itself
  and refuses user text that spells a model's control tokens. Model folders are made private (or used as they are, if
  you choose).

## What is in the repository

| Part | What it is |
| --- | --- |
| [`engine/`](engine/) | **The engine.** `multiengine.exe` is a launcher over one library per model family: `multiengine_strata.dll` (upstream Strata's engine, `v0.2.6`: Qwen3.8 and Qwen3.6), `multiengine_maya.dll`, `multiengine_maya_ds4.dll` and `multiengine_maya_mm3.dll` (Project Maya `v1.1.30`: GLM-5.3, DeepSeek-V4.1, MiniMax-M3.1), `multiengine_raon1.dll`, `multiengine_gemma4.dll` and `multiengine_flint.dll`. `strata-prepare.exe` builds the packs some families need. Shared parts: the expert tiers (VRAM slot cache, pinned RAM tier, CPU expert lane, disk reads), the batched prompt path, speculative decoding (MTP), CUDA kernels and llama.cpp's MMQ. Build notes: [`engine/README.md`](engine/README.md). |
| [`secure_frontend/`](secure_frontend/) | **The server.** A dependency-free C++20 program (`strata-secure-server`) that owns the listener and runs the engine as a private child over standard I/O: an API key on every listener (loopback by default; remote and wildcard binds are separate opt-ins), Host/Origin checks, an OpenAI-compatible `/v1/chat/completions` with streaming, `/v1/models`, health and metrics, a built-in web page (chat, monitor, models), model cards the page adds, starts, stops and calibrates, Hugging Face downloads, optional email alerts and optional web search for the model. It tokenizes prompts itself and refuses user text that spells a model's control tokens. The reference for every setting is [`secure_frontend/README.md`](secure_frontend/README.md). |
| [`make.py`](make.py), `make.bat`, `make.sh` | Build everything into one folder that holds only what runs. |
| [`engines/gemma4/`](engines/gemma4/), [`engines/unified/`](engines/unified/) | Earlier standalone engines whose pieces now live in `engine/`. |
| `serve/`, `strata_cli/`, `strata_control/` | Strata's Python serve layer, kept for reference and older scripts; `secure_frontend` replaces it. |
| [`upstream/`](upstream/) | Upstream Strata as an unmodified git submodule, the reference we track. |
| [`engines/maya-ref/`](engines/maya-ref/) | Project Maya as a read-only reference submodule; never built. |

## Models

| Model | Engine library | Status |
| --- | --- | --- |
| **Qwen3.8-Flash-Next** (IQ3_S and other quants) | `multiengine_strata.dll` | **Works** from a page card (its pack built by `strata-prepare`): about 45 tok/s. **Pictures work** with its picture encoder on the GPU: a test picture answered correctly in about 4 s. |
| **Qwen3.6-35B-A3B** (Q8_0) | `multiengine_strata.dll` | **Works** from a page card made straight from the GGUF: about 29 tok/s; web search checked end to end. |
| **GLM-5.3-Flash** (Maya packs) | `multiengine_maya.dll` | **Runs** from a page card (Maya's pack built beside the shards); speed not tuned yet. |
| **Raon-1-mini** (KRAFTON, Q8_0) | `multiengine_raon1.dll` | **Works, verified** against KRAFTON's llama.cpp (mean KL 0.0009, top-1 100% on short prompts); 74 tok/s short, 52 tok/s at 9K context; 256K context. |
| **Gemma 4 26B-A4B** (UD-Q8_K_XL) | `multiengine_gemma4.dll` | **Works:** KL 0.040 against llama.cpp, 47-60 tok/s. |
| **flint** (RecursiveSI, Q8_0) | `multiengine_flint.dll` | **Works:** about 49-53 tok/s decode, checked against llama.cpp's flint. |
| DeepSeek-V4.1-Flash | `multiengine_maya_ds4.dll` | Built and wired into the server (prompt, tokenizer, cards, downloads); not run end to end (its GGUFs are 277-508 GB). |
| MiniMax-M3.1-Flash (Maya-XS, Maya-S) | `multiengine_maya_mm3.dll` | Built and wired into the server (prompt, tokenizer, cards, downloads; MiniMax Community License, see below); not run end to end yet (Maya-XS is 93 GB). |

## Development system

Everything above was built and measured on one Windows desktop:

| Part | Specification |
| --- | --- |
| CPU | AMD Ryzen 9 3900X, 12 cores / 24 threads |
| Memory | 96 GB DDR4-3600 (2 x 32 GB + 2 x 16 GB) |
| GPU | NVIDIA GeForce RTX 4080 SUPER, 16 GB GDDR6X, PCIe 4.0 x16, 320 W power limit (compute capability 8.9) |
| Motherboard | MSI PRO B550M-VC WIFI |
| Storage | 5 SSDs: 2 x 1 TB NVMe, 512 GB NVMe, 250 GB SATA, 128 GB SATA (models spread over several drives) |
| Operating system | Windows 11 Pro 64-bit (build 26200) |
| Toolchain | Visual Studio 2022 Build Tools (MSVC), CUDA Toolkit 13.0, NVIDIA driver 610.88, CMake and Ninja (pip), Python 3.12 |

A model larger than VRAM keeps its experts in RAM (and reads the rest from disk): with 96 GB, a Qwen3.8 IQ3_S
(about 84 GB) or a GLM-5.3-Flash Maya-S pack (about 96 GB) runs with most experts in RAM. Smaller systems work, more
slowly, when the drive is fast NVMe.

## What this system could not test

One Windows desktop with one NVIDIA GPU cannot check everything the code supports. These parts are built and covered
by the server's tests (with fake engines where a GPU would be needed), but have **not been run on real hardware**:

| Area | What is untested, and why |
| --- | --- |
| **Several GPUs** | The GPUs choice on the cards, Qwen's layer split, GLM-5.3's and Raon-1-mini's split, DeepSeek-V4.1's tensor-parallel pairs and MiniMax-M3.1's GPU list are composed as upstream and Project Maya compose them, and checked argument by argument, but never run: the development system has one GPU. Upstream's card order for a split (the faster card last) is not done yet. |
| **Pictures (vision)** | Checked on Qwen3.8-Flash-Next IQ3_S with its encoder on the GPU (a red square and a blue circle, named and placed correctly, about 4 s). Not yet run: Qwen3.6, the encoder on the CPU, several pictures in one question, and pictures through the page's attach button (the check used the API). |
| **DeepSeek-V4.1 and MiniMax-M3.1** | Built and wired, not run end to end yet. Their GGUFs are large (DeepSeek's 277-508 GB, MiniMax's from 93 GB): with 96 GB of RAM most of their experts would come from disk, so a run here shows whether they work, not how fast they can be. |
| **Other GPU makers** | Only NVIDIA CUDA builds are made and run. Upstream's AMD (HIP) and Intel (SYCL) paths are in the tree but neither built nor tested here. |
| **Other NVIDIA generations** | Every speed and every run is from one RTX 4080 SUPER (compute capability 8.9). Other generations build with `--cuda-arch` but have not been run here; nor have GPUs with less than 16 GB, or PCs with less than 96 GB of RAM. |
| **Linux** | Every change is written for Linux and Windows, but the newest work (the GUI-first install, the newer engines, web search, pictures, several GPUs) has been built and run on Windows only. |

## Install

### Requirements

- **Windows 10/11 x64** (the tested system) or **Linux x86_64** - every change is written for both.
- An **NVIDIA GPU** with CUDA support and as much **RAM** as you can give the model (see above); fast NVMe storage for
  the model files.
- To build: **Visual Studio 2022 Build Tools** with the C++ workload (Windows) or GCC 13+ / Clang 17+ (Linux), the
  **CUDA Toolkit** (13.0 tested), **Python 3.10+** (the build script uses the standard library only), **CMake 3.28+**
  and **Ninja** (`pip install cmake ninja` is enough), and **Git**. On Linux, email alerts also need OpenSSL's
  development files.

### Get the code

```sh
git clone --recurse-submodules https://github.com/shift-f10/m-strata.git
git clone https://github.com/ggml-org/llama.cpp.git llama.cpp
git -C llama.cpp checkout 3cf03257f219afbe7334045ff7c6a06ac68c627d
```

The engine builds ggml from that pinned llama.cpp checkout (`--ggml-dir`). After a plain clone, run
`git submodule update --init`.

### Build an out folder

On Windows, from any prompt in the repository folder (`make.bat` finds Visual Studio's x64 build tools itself; an "x64
Native Tools Command Prompt for VS 2022" works too):

```bat
make.bat --out D:\SecureStrata --ggml-dir C:\src\llama.cpp --with email,search
```

On Linux:

```sh
./make.sh --out /opt/secure-strata --ggml-dir ~/src/llama.cpp --with email,search
```

| Option | What it does |
| --- | --- |
| `--out DIR` | The folder to create (it must not exist yet, or be empty). |
| `--ggml-dir DIR` | The pinned llama.cpp checkout (or set `STRATA_GGML_DIR`). |
| `--cuda-root DIR` | The CUDA toolkit (default: `CUDA_PATH` / `CUDA_HOME`). |
| `--cuda-arch native` | GPU targets: `native` (this PC's GPU) or a list such as `"86;89;120"`. |
| `--with email,search` | Optional features, off otherwise: email alerts, and web search for the models. |
| `--without maya,raon1,vision` | Leave those engine families, or the picture encoder (`vision`), out (strata, Gemma 4 and flint are always built). |
| `--no-cuda-runtime` | Do not bundle cuBLAS (the PC running it then needs a CUDA toolkit). |
| `--jobs N` | Parallel compile jobs (default: the CPU count). |
| `--build-dir DIR` | Where the builds are kept (default `out/make-build`). |
| `--plan` | Print the commands only. |

The first build compiles the CUDA engine and takes a while; later builds are incremental (`out/make-build`). The out
folder holds only what runs: `strata-secure-server`, `strata`, `strata-search` (with `--with search`), `engine/` (the
launcher, one library per family, `strata-prepare`, `strata-vision` (upstream's picture encoder, built from
`upstream/tools/vision` with the same pinned llama.cpp), the bundled cuBLAS, the expert profiles and, with the Maya
families, `templates/minimax-m3.1/`), `README.txt`, `config.example.json`, `LICENSE` and `NOTICE.md`. It is made
owner-only. Copy or move the whole folder anywhere.

## Configure

### First start

1. Run `strata-secure-server` (`strata-secure-server.exe` on Windows) in the out folder, with no arguments.
2. It creates `data/` beside itself (the config, the API key, model cards and state, all owner-only), listens on
   `http://127.0.0.1:8080`, and prints a **one-time login link** (`http://127.0.0.1:8080/#login=...`, valid for 10
   minutes, once, from this PC only).
3. Open the link in a browser on the same PC. The page shows your **API key once** and signs you in. Every later
   start prints a fresh link.
4. To see the key again later: `strata --config data/config.json key show` (`key rotate` makes a new one).

Other ways to start it, from the out folder (in PowerShell type `.\strata-secure-server`, on Linux
`./strata-secure-server`; the same for `strata`):

```text
strata-secure-server --port 9090                         # this folder's data/, on port 9090 for this start only
strata-secure-server --data-dir D:\StrataData            # the data folder somewhere else (created at its first start)
strata-secure-server --config data/config.json           # run from a config file (data/config.json is one)
strata-secure-server --check                             # validate data/config.json, print the settings in effect, exit
strata-secure-server --help                              # every option
```

Options such as `--port`, `--host` or `--token-env` apply to that start on top of the data folder's `config.json`; to
keep a port, set `listener.port` there. A relative `--data-dir` is taken from the current folder. `--data-dir` and
`--config` exclude each other; `config.example.json` shows the config file's shape. (A start that names its model by
flags, `--expert`, `--models-root`, `--engine`, `--model-id`, is the older bring-up mode and reads no data folder.)

### Add a model

On the page, open **Models**:

- **Add a model** uses files already on this PC: pick the folder and the model's `.gguf` (a split model: its first
  part). Gemma 4, flint and Qwen3.6 GGUFs become cards straight away. Qwen3.8, GLM-5.3, DeepSeek-V4.1, MiniMax-M3.1 and
  Raon-1-mini GGUFs have their pack built first by `strata-prepare` (seconds to minutes; a pack already beside the file
  and still matching it is reused). A Qwen3.8 model whose data folder holds upstream's MTP draft layer (`mtp/rt`, from
  upstream's setup) runs it, and its card shows the MTP settings (draft layer on/off, draft window, draft confidence).
  A Qwen3.8 or Qwen3.6 model with its picture encoder beside it (one `mmproj*.gguf` in the folder) reads pictures.
- **Download** fetches a model from the built-in catalog (every file pinned by its SHA-256) or a Hugging Face link
  into a drive or folder you choose; the free space is checked first. A large file comes over 16 connections at once
  (`pull.connections`, 1-32; 1 is one stream). A Qwen entry's **Vision files** box also fetches its picture encoder.
  When it is verified, it is packed if needed and appears as a card.
- **Start a model** starts and stops a card, with its options (context length, cache format, thinking, sampling).
  **Calibrate** tunes the engine settings that depend on this PC, once per card; it reruns after a settings change if
  you ask it to.
- A model folder must normally be private (other users of the PC cannot change it); the page offers to make it so -
  a folder of model files, with the packs its tools put beside them (`pack/`, `maya-ds4/`, `maya-mm3/`) and Hugging Face's
  `.cache/`. If you would rather use folders as they are, see Model folders below.

### Settings

The **Settings** tab holds the server's own settings. They are saved to `data/config.json` and apply when the server
restarts. They show in a browser on this PC, and on the other PCs `models.control_remote_networks` lists (the same
clients that may add and delete models).

- **API key:** the key this page signed in with, hidden until you press Show, and Copy. "Rotate the key" (type ROTATE
  first; only in a browser on this PC) makes a new key in `data/secret`, shows it once to copy, and works after the
  server restarts - every app then needs the new key. A server started with `--token-env` (a development start) has no
  key file to rotate.
- **Server and network:** the listen address and port, the two opt-ins for serving other PCs, the host names clients
  use, and which PCs or subnets may manage models.
- **Limits and timeouts:** the longest answer and model start (seconds), the largest prompt and answer (tokens), and
  the connections per download.
- **Folders:** where downloads go, and the folder Add a model opens first.

Each group's Save checks the whole file by the rules the server checks at start, so a setting that would stop the
server from starting (for example listening on `0.0.0.0` without the opt-ins) is refused with the reason, and the file
is left as it was. An empty box puts the setting back to its default.

- **Model folders:** "Use model folders as they are, even if other users of this PC can access them" lets the server
  use shared folders without changing their permissions (then other users of the PC can read or change those files).
- **Email alerts** (a build with `email`): the SMTP server, port and security (SSL/TLS on 465 or STARTTLS on 587),
  user name, From and To, and which events to send (downloads, calibrations, engine stops, server start). "Fill in
  IONOS" presets one provider; the password is stored on its own and never shown again; "Send a test email" checks it.
- **Web search** (a build with `search`): on/off, the address of your SearXNG instance, results per search, searches
  per answer and a time limit, and a test search that shows what a model reads.

### The config file

`data/config.json` is a plain JSON file; every key, its range and default are in the table in
[`secure_frontend/README.md`](secure_frontend/README.md). The ones most people touch:

| Key | Meaning |
| --- | --- |
| `listener.host`, `listener.port` | Where the server listens (`127.0.0.1:8080` by default). |
| `listener.allow_remote`, `listener.allow_wildcard_bind` | Opt-ins for listening beyond this PC. |
| `listener.host_aliases` | The `Host` names clients may use (for example this PC's LAN name and address with the port). |
| `models.control_remote`, `models.control_remote_networks` | Let other machines start and stop models, from listed hosts or subnets only. |
| `roots.allow_untrusted_media` | Use model folders as they are (the Model folders switch). |
| `search.enabled`, `search.url`, `search.results`, `search.max_rounds`, `search.timeout_s` | Web search. |
| `alerts.*` | Email alerts (set from the page). |
| `limits.engine.generation_timeout_s` | How long one generation may run before it is stopped: 120 s by default, up to 86400. A slow model writing a long answer needs more (1800 is half an hour). |
| `limits.chat.max_prompt_tokens`, `limits.chat.max_completion_tokens` | The largest prompt and answer a request may ask for (the card's context still bounds them). |
| `pull.connections` | Connections per large download (16 by default, 1-32). |

### Use it from other machines on your network

The API key is always required. To serve your LAN, set in `data/config.json`, then restart:

```json
"listener": {
  "host": "0.0.0.0", "port": 8080,
  "allow_remote": true, "allow_wildcard_bind": true,
  "host_aliases": ["127.0.0.1:8080", "localhost:8080", "<this PC's LAN address>:8080", "<this PC's name>:8080"]
}
```

`models.control_remote` with `models.control_remote_networks` (for example `["172.16.1.0/24"]`) also lets those
machines manage models from the page, and use the Settings tab (all but the key's rotation, which is this PC's alone).

### Web search through SearXNG

The model can search the web through your own [SearXNG](https://github.com/searxng/searxng) instance when it is unsure
of a fact or needs recent information. A small Docker setup on any machine you control:

```yaml
# compose.yaml
services:
  searxng:
    image: docker.io/searxng/searxng:latest
    container_name: searxng
    restart: unless-stopped
    ports: ["8080:8080"]
    volumes: ["./config:/etc/searxng", "./cache:/var/cache/searxng"]
```

```yaml
# config/settings.yml
use_default_settings: true
server:
  secret_key: "<a long random string>"
  limiter: false
  base_url: "http://<that machine>:8080/"
search:
  formats: [html, json]   # the server reads the JSON
```

Run `docker compose up -d` there, then in Settings > Web search turn it on, enter
`http://<that machine>:8080`, save, restart the server, and use "Test search". A Qwen, GLM-5.3, raon1, flint or Gemma 4 model
then gets the `web_search` tool on every text chat; what it searches for leaves your PC through that instance. Each such
card has a **Web search** switch (on by default; off, that model starts without the tool), and while it is on the chat
settings have one too (off, the page's chats ask for no search). After the last allowed search the model is told to
answer from what it found.

If searches come back with "the search engines did not answer", the engines SearXNG asks are refusing it (CAPTCHAs,
rate limits). Turn on more of them in `config/settings.yml`, then restart the container:

```yaml
engines:
  - name: bing
    disabled: false
  - name: qwant
    disabled: false
  - name: yahoo
    disabled: false
  - name: mojeek
    disabled: false
    inactive: false
```

## Use

### The page

Open `http://127.0.0.1:8080/` and sign in with the API key (or the login link). **Chat** talks to the running model
(with its reasoning shown separately, and a note of what it searched for), **Monitor** shows the GPU, CPU, memory and
speed and the recent requests with the model each ran on (Stopped when no model runs), **Models** manages models and
settings, and **About** lists the engines this folder was built with (`engine/build.json`, which `make` writes) and
this PC's CPU, memory, GPU and system.

### The API (OpenAI-compatible)

The base URL is `http://127.0.0.1:8080/v1` and the API key is the bearer token; `model` is the card's name
(`GET /v1/models` lists the running one).

```sh
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H "Authorization: Bearer $STRATA_API_KEY" -H "Content-Type: application/json" \
  -d '{"model": "Qwen3.6-35B-A3B-Q8_0", "max_tokens": 8192,
       "messages": [{"role": "user", "content": "What is MITRE ATT&CK technique T1059.001?"}]}'
```

The `curl` line is for a POSIX shell (Linux, Git Bash). In Windows PowerShell:

```powershell
$key = '<your API key>'
$body = '{"model": "Qwen3.6-35B-A3B-Q8_0", "max_tokens": 8192, "messages": [{"role": "user", "content": "Hello"}]}'
(Invoke-RestMethod http://127.0.0.1:8080/v1/chat/completions -Method Post -ContentType 'application/json' `
  -Headers @{ Authorization = "Bearer $key" } -Body $body).choices[0].message.content
```

```python
from openai import OpenAI

client = OpenAI(base_url="http://127.0.0.1:8080/v1", api_key="<your API key>")
stream = client.chat.completions.create(
    model="Qwen3.6-35B-A3B-Q8_0", max_tokens=8192, stream=True,
    messages=[{"role": "user", "content": "What is the latest stable Rust release?"}])
for chunk in stream:
    if chunk.choices and chunk.choices[0].delta.content:
        print(chunk.choices[0].delta.content, end="", flush=True)
```

- `stream: true` streams server-sent events; `stream_options: {"include_usage": true}` adds the usage at the end.
- `reasoning_effort` sets how much a model that thinks reasons (`low`, `medium`, `high`; the card shows its
  choices); `none` answers directly.
- With web search on, the model searches by itself when it needs to. Ask for current facts, or say "search the web";
  give it room (`max_tokens` 8192 or more), since each search round has its own budget. The searches it made come
  back as `strata_search` events; `"strata_search": false` answers that request without searching. Do not send
  `tools`: the server refuses it. Details:
  [`secure_frontend/README.md`, "Web search from the API"](secure_frontend/README.md#web-search-from-the-api).
- **Pictures:** while the running card's Pictures option is on, a user message's `content` may be a list of parts:
  `{"type": "text", "text": "..."}` and `{"type": "image_url", "image_url": {"url": "data:image/png;base64,..."}}`
  (PNG, JPEG or WebP; up to `vision.max_n` per request, each side up to `vision.max_side`). An `http(s)` picture
  address is refused, never fetched.
- `/health` and `/metrics` (with the Monitor on) also take the key.

### For development

Every server started for development and tests uses the public development API key
`strata-secret-key-dev-placeholder`, the only key this repository holds. Set it in a variable and pass its name with
`--token-env` (it replaces the data folder's key for that start; the folder's own key is kept):

```bat
set STRATA_DEV_KEY=strata-secret-key-dev-placeholder
strata-secure-server --data-dir dev-data --port 18080 --token-env STRATA_DEV_KEY
```

```powershell
$env:STRATA_DEV_KEY = 'strata-secret-key-dev-placeholder'
.\strata-secure-server --data-dir dev-data --port 18080 --token-env STRATA_DEV_KEY
```

```sh
export STRATA_DEV_KEY=strata-secret-key-dev-placeholder
./strata-secure-server --data-dir dev-data --port 18080 --token-env STRATA_DEV_KEY
```

`engine/setup-frontend-raon.bat` writes the same key to the raon dev server's key file. The key is public: never use it
for a real install, which makes its own key at the first start.

### The command line

| Command | What it does |
| --- | --- |
| `strata --config data/config.json key show` | Prints the API key (`--env` prints it as `OPENAI_API_KEY=...`). |
| `strata --config data/config.json key rotate` | Makes a new key; restart the server and update every app that used the old one. |
| `strata --config data/config.json doctor` | Checks the GPU, the driver, cuBLAS beside the engine (or in `engine.lib_dirs`), the config, the key file, the folders and the port (`port` fails while the server runs). |
| `strata --config data/config.json list` | Lists the downloaded models in the store. |
| `strata --help`, `strata-secure-server --help` | Every verb and option. |

## Troubleshooting

| What you see | Why, and what to do |
| --- | --- |
| No Settings, email or web search on the page | Those sections answer this PC and the machines `models.control_remote_networks` lists; open the page from one of them. A build without `--with email,search` has no email or search sections at all. |
| A new model family, download or setting is missing | The running server is an older build: rebuild the out folder and restart from it. |
| "Other users of this PC can change files in this folder" | Make the folder private when the page asks, or turn on Settings > Model folders. |
| A GLM download shows its size as "read from Hugging Face at the start" | Project Maya's catalog pins each file's SHA-256 but not its size; the server reads the size when the download starts. |
| A web-search answer is empty | It ran out of tokens while reasoning (`finish_reason` `length`): raise `max_tokens`. If every search found nothing, the search box says why; see "Web search through SearXNG" for more engines. |
| An answer stops after about two minutes, marked "Max tokens" (`length`), with room left in the context | It reached `limits.engine.generation_timeout_s` (120 s by default). Raise it in `data/config.json` (for example `"limits": {"engine": {"generation_timeout_s": 1800}}`) and restart the server. |
| The engine takes long to stop with a big model | Freeing tens of GB of pinned RAM takes time; the server waits, then reports it. |

## Current progress and open items

- **Done recently:** the GUI-first install (one out folder, setup on the page, a folder per model, calibration,
  downloads into a chosen folder, the danger-zone delete); optional email alerts; the engine as a launcher over one
  library per family (upstream Strata `v0.2.6`, Project Maya `v1.1.30`, raon1, Gemma 4, flint); native `strata-prepare`
  packers; GLM-5.3, DeepSeek-V4.1, MiniMax-M3.1 and Qwen3.6 in the server; optional web search over MCP and SearXNG for
  Qwen, GLM, raon1, flint and Gemma 4, with a switch on each card and in the chat; Qwen's and GLM's prompts tokenized exactly as
  upstream's and Maya's tokenizers do; pictures on Qwen cards (upstream's `strata-vision` beside the engine, the Vision
  files download, picture attachments in the chat; checked on Qwen3.8 IQ3_S on the GPU).
- **Open:** DeepSeek and MiniMax end-to-end runs; GLM speed tuning; making Qwen3.8's MTP draft layer here (upstream's
  three Python tools, natively; today it comes from upstream's setup); pictures on the other families; web search
  for DeepSeek and MiniMax; several GPUs checked on a two-GPU PC, upstream's card order for a layer split (the faster
  card last), and a split for Gemma 4 and flint;
  long-prompt speed and long-context drift on raon1; Linux runs of the newest work; the server as a window with a
  tray icon (model start and stop, the Monitor, a configuration tab).

## License

M-Strata is free software under the **GNU Affero General Public License, version 3** ([`LICENSE`](LICENSE),
AGPL-3.0-only). Running a modified version as a network service means offering its users the source. [`NOTICE.md`](NOTICE.md) holds the copyright, the MIT notice of the Strata code this builds on, and
an additional permission (AGPL section 7) to combine it with MiniMax-M3.1-Flash's template and model, which stay under
the MiniMax Community License. Third-party code keeps its own license (below).

## Sources and credits

| Source | What we use | Revision | License |
| --- | --- | --- | --- |
| [Niko1221/Strata](https://github.com/Niko1221/Strata) | The engine this project's engine is built from (`engine/` carries its tree), its Python serve layer, and the expert-tiering design. Also the unmodified submodule `upstream/`. This project started as a fork of it. | engine tree `v0.2.6` (`4b9e05a2`); submodule `v0.2.4.2` (`dfa3f402`) | MIT, Copyright (c) 2026 Niko1221 and the Strata contributors (its notice kept in [`NOTICE.md`](NOTICE.md)) |
| [mw00/project-maya](https://github.com/mw00/project-maya) | Its GLM-5.3, DeepSeek-V4.1 and MiniMax-M3.1 engines (`engine/third_party/maya`), its packers and tokenizer (ported), and the read-only submodule `engines/maya-ref`. | engine tree `v1.1.30` (`8621e331`); submodule `v1.1.27` (`4615f346`) | MIT, Copyright (c) 2026 Peasant Smith and the Project Maya contributors, Copyright (c) 2026 Niko1221 and the Strata contributors |
| [MiniMaxAI/MiniMax-M3.1-Flash](https://huggingface.co/MiniMaxAI/MiniMax-M3.1-Flash) | Its chat template's text (through Maya's copy, `engine/third_party/maya/tools/mm3/chat_template.jinja`; shipped in `engine/templates/minimax-m3.1/`), which our MiniMax prompt writes; the model itself is downloaded, never in the tree. Commercial use needs "Built with MiniMax M3.1-Flash" shown and a notice to MiniMax. | revision `54e4e69` | MiniMax Community License, Copyright (c) 2026 MiniMax (`engine/third_party/maya/LICENSE.minimax-m31`) |
| [ggml-org/llama.cpp](https://github.com/ggml-org/llama.cpp) / ggml | Linked into the engine: ggml and ggml-cuda's quantized matrix kernels (MMQ), from the pinned checkout; and its `mtmd` library, which upstream's `strata-vision` (`upstream/tools/vision`, built as it is) encodes pictures with. Also the test oracle for Gemma 4 and flint. | commit `3cf03257f219afbe7334045ff7c6a06ac68c627d` | MIT |
| [robertkarl/UnbiasedStrata](https://github.com/robertkarl/UnbiasedStrata) | Security-design ideas only (a smaller, offline-verifiable runtime; a reduced attack surface). No code. | commit `46c46ecfc11d2315512f21f7549b0a7792ccd2ae` | MIT |
| [lighttransport/Strata](https://github.com/lighttransport/Strata/tree/glm53f) | GLM-5.3 compatibility reference (the GLM serving profile and tokenizer behaviour). | branch `glm53f`, commit `4894c9967f5754a2052d7b05f10a01b6071bb6e4` | MIT |
| [ikawrakow/ik_llama.cpp](https://github.com/ikawrakow/ik_llama.cpp) PR #2733 and KRAFTON's llama.cpp (`raon-1-mini-preview`) | raon1 references: the model's math and pre-tokenizer, and the official graph our raon1 output is checked against (a test oracle, not shipped). | - | MIT |
| [mstorsjo/llvm-mingw](https://github.com/mstorsjo/llvm-mingw) | Build tool only (the open-source Windows cross toolchain); nothing from it is in this tree. | release `20261006` | ISC for its scripts; bundled components keep their own licenses |

The full attribution record is [`docs/PROVENANCE.md`](docs/PROVENANCE.md) and the security model is
[`docs/security/threat-model.md`](docs/security/threat-model.md).
