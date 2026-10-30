# Engine family plugins (2026-10-30) - design, approved by the owner (decisions at the end)

The owner (2026-10-30): "now let's work on making our multiengine plugin capable where for example a new model comes out
that is not yet supported, our engine should have a published way so that we or another user can use those guidelines
and create a library that we can just add later on and it will seamlessly integrate along with cards, settings, etc."
and "it should have download cards as well for the model, mtp files if any, etc". The same ask, 2026-10-28: "make our
engine pluginable so that people can author their own engine and slots directly with ours with minimal or no changes
but configuration file only ... create an authoring guide as well for agents and developers for it so that there is a
blueprint of what it needs and what needs to be built."

**Goal.** A new model family arrives as one folder - its engine library and a manifest - dropped into an install's
`engine/families/<id>/`. Once the owner approves it, the server shows its download entries (the model, its MTP or draft
files, its picture encoder), makes cards for its GGUFs, offers its card options and settings, calibrates it, and chats
with it. Neither the launcher, the server nor the page changes. A published SDK and authoring guide say how to build
one.

**Not a goal.** Running code the owner did not approve; letting a plugin widen what the server allows (its environment
rows, the wire dialects, control-token refusal); other GPU makers (the engine stays CUDA); hot-loading without a server
restart.

## Today (2026-10-30 survey)

**The engine** (`engine/src/multiengine/launcher.cpp`):
- The launcher loads one library per family at run time (`LoadLibraryExW` / `dlopen` beside the exe) and calls one
  `extern "C" int <name>_main(int, char**)`. The names differ per family: `strata_main`, `gemma4_main`, `flint_main`,
  `glm_main`, `maya_*_main`.
- The family is guessed from the argv's shape: the GGUF architecture of `--native`, `native_experts.txt` of
  `--glm-pack`, marker files of `--pack`. The table is fixed at compile time (`family_for`, `MULTIENGINE_HAVE_*`). An
  unknown architecture falls through to upstream's engine.
- There is no ABI version, no size check and no signature check.
- Each family re-implements the stdio wire protocol: READY / GEN / T / DONE / STOP / QUIT, plus INFO / STAT.
- The shared expert tiering (`strata_core`, `strata_engine`, `strata_kernels_cpu`, `strata_prefill`, MMQ) is built as
  internal static libraries, with no installed headers and no SDK. The one versioned plugin ABI in the tree is
  `strata_kq` (`mmq_kq_abi.hpp`: `STRATA_KQ_ABI_VERSION`, the argument-block size checked).

**The server.** Every place it branches on `EngineProfile` (`frontend_config.hpp:109`) falls into one of three groups.
- **Data a manifest can declare** (about two thirds):
  - GGUF architecture to family (`card_builder.cpp:109-125`) and the metadata keys read;
  - card option tables (`model_options.cpp:142-312`) and how each option becomes a flag, row or environment variable;
  - the argument allowlists (`engine_arguments.cpp:54-132`);
  - calibration steps and margins (`calibration.cpp:158-213`);
  - Monitor reading keys (`serve_monitor.cpp:31-36`);
  - capabilities (vision, MTP, web search) and download entries (`catalog/downloads.json` already has the schema:
    files, `mtp`, `mmproj`, `draft_layer`, `prepare`);
  - the wire dialect, tokenizer mode and thinking defaults (`subsystem_limits.cpp`).
- **Code from a closed, reviewed set:**
  - prompt renderers (no Jinja, by rule: `chat_generation_plan.hpp:24-26` "selecting a tokenizer pack alone must never
    cause an unreviewed model-provided template to execute");
  - tool-call parsers (streaming state machines) and pre-tokenizer splits (generated tables);
  - wire dialects and argv composition (`build_engine_arguments`).
- **Security** (`docs/security/threat-model.md:413-450`, `:516-535`):
  - each family has an allowlist, a dialect, a closed environment and a renderer that refuses control-token literals;
  - the server checks only `multiengine.exe` (`open_executable`), never the family libraries.

## The plugin

One folder, `engine/families/<id>/`, owner-only like every runtime folder:

```
engine/families/acme3/
  family.json                 the manifest (schema m-strata.family/1)
  multiengine_acme3.dll       Windows (libmultiengine_acme3.so on Linux)
  strata-prepare-acme3.exe    optional: builds a pack for its models
  LICENSE, NOTICE             its own
```

### 1. The engine ABI (`engine/sdk/include/strata/family_abi.h`, C, versioned)

```c
#define STRATA_FAMILY_ABI_VERSION 1
typedef struct StrataFamilyInfo {
  uint32_t size;          /* sizeof(StrataFamilyInfo): later versions only append */
  uint32_t abi;           /* STRATA_FAMILY_ABI_VERSION * 100000 + minor, as strata_kq's */
  const char* id;         /* "acme3": the manifest's id */
  const char* version;    /* the library's own version */
} StrataFamilyInfo;
STRATA_FAMILY_EXPORT const StrataFamilyInfo* strata_family_info(void);
/* The engine: argv as the server composed it (UTF-8), the wire dialect on stdin/stdout; the exit code is the process's. */
STRATA_FAMILY_EXPORT int strata_family_main(int argc, char** argv);
```

- **The launcher.** It gains `--family <id>`, which the server always passes for a plugin card. It loads
  `families/<id>/multiengine_<id>` and refuses the library unless all of these hold:
  - `strata_family_info()` exists, and its `size` and `abi` major match;
  - its `id` is the folder's;
  - its SHA-256 is the one the server passed on the command line.
  
  The launcher's process steps (unbuffered stdout, `CUDA_MODULE_LOADING`, `--gpu` to `CUDA_VISIBLE_DEVICES`, the crash
  trace) apply to every plugin, so a plugin never repeats them.
- **Built-in families.** They keep their argv routes. Over time they also export the standard symbols, so `--family`
  works for them too, and the argv sniffing can retire.

### 2. The SDK (`engine/sdk/`, installed by `make.py --sdk DIR`)

The SDK is a CMake package (`find_package(StrataFamilySDK)`) with headers and static libraries for:
- **`strata_family_runtime`:** the wire protocol written once. A family implements a small C++ interface, and the
  runtime runs READY, GEN, T, DONE, STOP, QUIT, INFO and STAT exactly as the server's `strata_0143` dialect expects,
  so no plugin can get the protocol subtly wrong:
  - `load(args)` returns the context size and INFO;
  - `prefill(ids)` reports its progress;
  - `step(sampling)` returns the next token;
  - `stop()` and `stats()`.
- **The shared expert tiering** (the Strata paper's design, which every engine here follows): the VRAM slot cache, the
  pinned RAM tier, the CPU expert lane, disk reads, the batched prompt path, MTP verification and the MMQ kernels. These
  are the same targets the built-in families link, so a plugin gets the same speed and calibration knobs for free.
- **A skeleton family** (`engine/sdk/template/`): it builds, loads and answers. It is the starting point the authoring
  guide walks through.

### 3. The manifest (`family.json`)

Every block is declared data, checked against a published schema. The parts that are code name an entry from a closed,
reviewed list.

```json
{
  "schema": "m-strata.family/1",
  "id": "acme3", "name": "Acme-3", "version": "0.1.0", "publisher": "Example Labs", "license": "Apache-2.0",
  "engine": {
    "library": {"windows": "multiengine_acme3.dll", "linux": "libmultiengine_acme3.so"},
    "abi": 1, "dialect": "strata_0143",
    "prepare": null
  },
  "models": {"gguf_architectures": ["acme3"], "context_key": "acme3.context_length", "mtp_key": "acme3.nextn_predict_layers"},
  "tokenizer": {"source": "gguf", "mode": "byte_bpe", "pre": "qwen2"},
  "chat": {
    "template": {"bos": "<|bos|>", "system": "<|system|>{content}<|end|>\n", "user": "<|user|>{content}<|end|>\n",
                 "assistant": "<|assistant|>", "assistant_end": "<|end|>\n"},
    "thinking": {"open": "<think>", "close": "</think>", "default": "on", "efforts": ["none", "low", "high"]},
    "tools": "hermes_json",
    "stop": ["<|end|>"]
  },
  "arguments": [
    {"flag": "--ctx", "value": "integer", "min": 4096, "max": 1048576},
    {"flag": "--expert-vram", "value": "gib_or_auto"},
    {"flag": "--cpu-threads", "value": "integer", "min": 1, "max": 256},
    {"flag": "--pcie-frac", "value": "fraction"}
  ],
  "options": [
    {"name": "max_context", "kind": "integer", "label": "Context length", "default": 65536,
     "choices": [8192, 32768, 65536, 131072], "flag": "--ctx"},
    {"name": "expert_vram", "kind": "integer_or_auto", "label": "Expert VRAM", "default": "auto", "flag": "--expert-vram"}
  ],
  "calibration": {
    "before_first_start": true, "keep_margin": 0.03,
    "steps": [{"flag": "--cpu-threads", "values": ["{cores}", "{cores*2/3}", "{cores/2}"]},
              {"flag": "--pcie-frac", "values": ["0", "0.25", "0.5", "0.75"]}]
  },
  "monitor": {"experts_vram": "vram_slots", "experts_ram": "pinned_gib"},
  "capabilities": {"vision": false, "web_search": true, "mtp": "file"},
  "downloads": [
    {"id": "acme3-30b-q8_0", "name": "Acme-3 30B Q8_0", "about": "...",
     "repo": "example/acme3-30b-GGUF", "revision": "<40 hex>",
     "files": [{"path": "acme3-30b-Q8_0.gguf", "size": 32000000000, "sha256": "<64 hex>"}],
     "mtp": {"path": "MTP/acme3-30b-mtp.gguf", "size": 460000000, "sha256": "<64 hex>",
             "repo": "example/acme3-30b-GGUF", "revision": "<40 hex>"},
     "mmproj": null, "prepare": "none"}
  ]
}
```

What each block drives:
- **`models`:** Add a model and a finished download make a card for any GGUF whose architecture is listed. The context
  and MTP keys fill the card's limits and its draft layer.
- **`tokenizer`:** the GGUF's own vocabulary (or a pack's). `mode` and `pre` come from the closed list of tokenizers this
  server implements (byte_bpe with qwen2 / llama3 / gpt2 splits, spm, ...). Control-token refusal always applies.
- **`chat`:** see "Prompt format" below. `tools` names a reviewed tool-call format (hermes_json, qwen_xml, gemma4,
  glm_xml, deepseek_dsml, minimax_xml, or none). `thinking` drives the card's Thinking option and how a reply's
  reasoning is split off.
- **`arguments`:** the plugin's allowlist. Only these flags, with these value kinds, ever reach its engine. The value
  kinds are the server's closed set: integer, fraction, gib_or_auto, choice, gguf_file, ...
- **`options`:** the card's options, as the built-in tables declare them. Each option targets a declared flag, or a
  value map to flags. The page and the TUI draw them as they draw any card's options.
- **`calibration`:** the sweep, as the built-in plans are written. Candidates may use `{cores}` (physical cores) and
  `{threads}`. The keep margin is set here, and so is whether the sweep runs before the first start.
- **`monitor`:** which of the engine's STAT/INFO keys mean the expert tiers, from the Monitor's fixed key list.
- **`capabilities`:** whether cards offer pictures (with an `mmproj`), web search, and an MTP file or draft layer.
- **`downloads`:** entries in the built-in catalog's schema. Each pins its repository, revision, every file's size and
  SHA-256, the MTP file, the picture encoder and the draft layer. They show under Download models with the plugin's
  name. `prepare` is `none`, a built-in pack step, or `plugin` (the family's own `strata-prepare-<id>`, run as
  strata-prepare's download protocol: STEP / DONE / ERROR).

### 4. Prompt format (decision 1)

A new model nearly always ships a Jinja chat template in its GGUF. Two ways to support one without new server code:

- **(a) Recommended: a declarative template.** The manifest declares the segments: BOS, each role's prefix and suffix,
  the assistant's generation prompt, the thinking markers and the stop tokens. One new reviewed renderer in the server
  draws any declared template. It escapes and refuses control-token literals exactly as the hand-written renderers do,
  and tool calls use a format from the reviewed list. The rule that no model-provided template ever runs stays.
  Formats that need logic beyond segments (Gemma 4's, DeepSeek's) stay hand-written; a plugin can name one of those.
- **(b) The GGUF's own Jinja template, sandboxed.** It is the simplest for authors, since nothing needs declaring. It
  needs a Jinja engine in the server, and it overturns today's rule: a downloaded file's template would decide the exact
  tokens the engine sees.

### 5. The server (`FamilyDescriptor`)

- **One descriptor per family.** A `FamilyDescriptor` holds every declared block above. The built-in families get
  descriptors from today's tables, so their behaviour does not change; plugins get theirs from their manifests.
- **The profile becomes an id.** `EngineProfile` becomes a family id (the built-ins keep theirs). Each switch the
  survey found reads the descriptor instead:
  - card_builder, model_options, engine_arguments and calibration;
  - subsystem_limits, serve_monitor and download_catalog;
  - the page's labels, which the server will send.
- **Built-in families keep their code.** Their hand-written renderers, parsers, pre-tokenizers and argv composition stay
  as they are. A plugin selects from them by name, or uses the declarative template.
- **Composing a plugin card's engine arguments.** The server writes `--family <id> --family-sha256 <hex>`, the model
  (`--native <gguf>` or `--pack <dir>`), then each option's flag from the allowlist, then the calibration's flags.

### 6. Trust (decision 2)

A plugin is code that runs as the user, with the model files and the GPU. That is a trust decision, recorded in
`docs/security/threat-model.md`.

- **(a) Recommended: approve each plugin.** Settings > Engines lists every plugin found under `engine/families/` with
  its name, publisher, version, licence, the library's SHA-256 and what it declares (architectures, flags, downloads).
  None is used until the owner approves it; approval pins the library's and the manifest's SHA-256 in `config.json`.
  - A plugin whose library or manifest changed is refused until it is approved again.
  - Its folder must be owner-only, as `open_executable` already requires of the engine.
  - The launcher checks the pinned SHA-256 again before it loads the library.
- **(b) Any plugin in the folder loads.** This is simpler, but anyone who can write into the engine folder decides what
  runs.

**What a manifest can never change**, whichever trust model is chosen:
- the environment allowlist (`kFamilyForcibleEnvironment`) and the wire dialects (a plugin picks one);
- control-token refusal, and the limits (prompt size, answer size, timeouts);
- the API key and the listener;
- the download rules: pinned revisions and SHA-256, owner-only folders, and the space check.

### 7. Building and shipping

- `make.py --plugin DIR` builds an out-of-tree plugin against the SDK into the out folder's `engine/families/<id>/`.
- The layout check accepts `engine/families/<id>/` holding exactly the manifest's files.
- `build.json` lists each plugin with its version and SHA-256, and the About tab shows them.
- `make.py --sdk DIR` installs the SDK for authors who build outside this repository.

### 8. The authoring guide (`docs/ENGINE_PLUGINS.md`)

Written for developers and agents, as a blueprint:
1. What a family is.
2. The ABI and the SDK.
3. The protocol, with a transcript.
4. The expert tiering to reuse, with the Strata paper's rules.
5. Each manifest block, with the schema and its checks.
6. Tokenizer and prompt-format choices.
7. Calibration.
8. Downloads and packs.
9. Testing: the SDK's protocol conformance test and a fake-model run.
10. Packaging and approval.
11. A checklist.

The skeleton family is its worked example.

## Proof (decision 3)

- **(a) Recommended: re-package flint as a plugin.** flint is the smallest family: GGUF in, no pack, the strata_0143
  dialect, a hand-written renderer. As `engine/families/flint/`, with a manifest and no flint code in the launcher's
  table or the server's switches, it must download, card, calibrate and chat exactly as today. That proves the contract
  on a real model before any new family needs it.
- **(b) A new family directly.** Build the first plugin for a model nothing here runs yet. It shows the whole path at
  once, but a bug could be in the contract or in the new engine.

Either way, the tests are:
- a fake plugin built from the supervision fake engine, which loads, refuses a changed SHA-256, cards a GGUF of its
  architecture, offers its options, calibrates and answers;
- a manifest that tries to widen an allowlist, the environment or a dialect is refused by name;
- the page lists the plugin's downloads and cards with its name.

## Phases (each its own commit, TDD, Windows and Linux)

1. **The contract.**
   - The ABI header and the launcher's `--family` (ABI, id and SHA-256 checks).
   - The manifest schema and its validator.
   - The fake plugin and its tests.
2. **The descriptors.**
   - `FamilyDescriptor` for the built-ins, with no behaviour change (every existing suite stays green).
   - Then manifest-driven descriptors for plugins: cards, options, arguments, calibration, Monitor, capabilities.
3. **Prompt format.** The declarative template renderer, if decision 1 is (a), with its refusal tests.
4. **Downloads.** Plugin entries in Download models, `prepare: plugin`, and the MTP and picture files.
5. **Trust.** Settings > Engines, approval and pinning, the launcher's SHA-256 check, and the threat model.
6. **The SDK and the guide.**
   - `strata_family_runtime` and the skeleton family.
   - `make.py --sdk` / `--plugin`.
   - `docs/ENGINE_PLUGINS.md`.
7. **The proof.** Flint as a plugin, or the first new family.

## Decisions (the owner, 2026-10-30)

1. Prompt format: **(a) a declarative template in the manifest** ("Declarative template"). No model-provided template
   runs.
2. Trust: **(a) approve each plugin in Settings, with SHA-256 pinning** ("Approve each").
3. Proof: **(a) re-package flint as a plugin first** ("Re-package flint").
