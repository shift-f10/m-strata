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

- **(a) Recommended: approve each plugin.** The Plugins tab (section 8) lists every plugin found, with its name,
  publisher, version, licence, the library's SHA-256 and what it declares (architectures, flags, downloads).
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

### 8. Plugin kinds and the Plugins tab (the owner, 2026-10-30)

The owner: "we also need to have a plugin management tab to manage these", then "now that I think about it, we should
make the email, websearch, debugging all plugins as well". So a plugin has a **kind**, and one tab manages them all.

**The rule for every kind: a plugin never runs inside the server.**
- An engine family runs in the engine's process, as today.
- Every other kind runs as a child process of the server, under the sandboxing the server already gives its children:
  the job object or process group, no new privileges, a closed environment, and stdio only.
- So a plugin can never reach the API key, the listener, the config file or another plugin's secrets.
- The server asks for a plugin's work over a small, declared protocol, and stays the security boundary.

| Kind | What it is | First-party plugin made from today's code |
| --- | --- | --- |
| `engine` | a model family (sections 1-8) | flint first (the proof), then the other families over time |
| `tool` | an MCP server the chat's tool loop calls (`tools/list`, `tools/call`) | **web search**: `strata-search` already is one; its SearXNG settings move into its manifest |
| `notifier` | told of the server's events (download finished, calibration finished or failed, engine stopped, server started) as one JSON line each on stdin | **email alerts**: the SMTP sender (Schannel / OpenSSL) moves out of the server into `strata-email` |
| `diagnostics` | a manifest only, no code: switches on what a `--debugging` build does today | **debugging**: verbose logs (`logging.requests`, `logging.engine_output`), the start-up warnings, the window's and TUI's notice, and Settings > Troubleshooting |

Every manifest declares its **settings**: name, kind, label, help, bounds and default, as card options are declared.
- The Plugins tab draws a plugin's settings, and the server saves them under `plugins.<id>.*` in `config.json`.
- A secret, such as the SMTP password, is declared as one. The server keeps it in the data folder's key store (as the
  alerts password is kept today), never shows it again, and hands it only to that plugin's process.
- A `tool` or `notifier` declares the network it needs (for example the SearXNG address, or the SMTP host and port).
  The owner sees that before approving it.

**The Plugins tab** (a top-level tab, beside Settings):
- **List:** every plugin found, by kind. For each: its name, publisher, version and licence, its state (on, off,
  waiting for approval, changed since approval, refused, with the reason), and what it declares.
- **Actions:** Approve (pins the SHA-256), Turn on / Turn off, its settings, and Remove. Remove asks first; it takes
  the plugin's folder away and keeps the models that the plugin's downloads fetched.
- **The same in the TUI:** a Plugins view with the same list and keys.

**First-party plugins** are the ones `make` builds from this repository: email, search, debugging, and the families once
they are plugins.
- `make` writes their SHA-256 into `engine/build.json` (the owner-only install folder). The server trusts a
  first-party plugin whose SHA-256 matches its build record, and needs no separate approval for it.
- Each is still off until its settings are filled in, and debugging stays off unless turned on.
- A plugin from anyone else needs the owner's approval (decision 2).

**Building:**
- `make --with email,search` builds those plugins into the out folder instead of compiling them into the server.
- `--debugging` ships the debugging plugin turned on.
- The installers' questions stay: they choose which plugins to build in.
- A build without a plugin can have it added later: drop in its folder and approve it in the Plugins tab.

### 9. The authoring guide (`docs/ENGINE_PLUGINS.md`)

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

### 10. Tool and notifier plugins: settings, secrets, running (the owner, 2026-10-30)

The owner's decisions 6-8 (below): their settings move into the plugins (`plugins.<id>.*`), third-party tool and
notifier plugins run in this step, and key-signed trust follows it. This section is the contract for both kinds, and for
search and email made from today's code.

**Settings.** A `tool` or `notifier` block declares its `settings`, at most 16. Each has:
- `name`: a lowercase letter, then lowercase letters, digits or `_`, at most 32 characters;
- `kind`: `text`, `url`, `secret`, `integer`, `boolean`, `choice` or `text_list`;
- `label` and `help` (printable text), and `required` (true or false);
- `default` (of its kind; never for a secret), `min` and `max` (an integer's), `choices` (a choice's, or the allowed
  items of a text list), and `max_items` (a text list's, at most 16).

Where the values live:
- Every value but a secret is in `config.json` as `plugins.<id>.<name>`. The config reader takes `plugins.<id>` objects
  of scalars and lists of strings, bounded, and the server checks each value against the manifest when it starts.
- A secret is in `roots.secret` as `plugin-<id>-<name>`, kept as the API key is (DPAPI on Windows, 0600 elsewhere). It
  is never in `config.json`, never in an answer (the tab sees only "set" or "not set"), and goes only to that plugin's
  own process.
- A plugin whose required setting is missing, or whose value its manifest refuses, is refused with the setting's name and
  the rule, and the tab shows it.

The routes:
- `POST /v1/strata/plugins` lists each plugin's settings with their values (a secret's as set or not).
- `POST /v1/strata/plugins/settings {"id", "values": {name: value or null}}` saves them, checked against the manifest. A
  null puts a setting back to its default, or removes a secret. A change applies when the server restarts.
- The Plugins tab draws each plugin's settings and saves them; the TUI's Plugins view lists them and edits them.

**Running.** A `tool` or `notifier` block says how its program runs:
- `program`: one of its files;
- `arguments`: each a fixed text or one `{setting}`; never a secret's;
- `environment`: variable name to `{setting}`, at most 16. A secret goes only here. The names `PATH`, `SYSTEMROOT`,
  `TEMP`, `TMP`, `HOME` and any name starting with `STRATA_` are refused;
- `network`: in words, what it connects to (`{setting}` allowed), shown in the approval question.

The server runs it as it runs strata-search today:
- a child in the server's job object (Windows) or process group (Linux), with no new privileges;
- the closed environment the downloads' children get, plus the declared variables;
- stdio only, its working folder `<roots.state>/plugins/<id>`;
- its stderr in the server's log, one line each, `[plugin <id>] ...`, cut at 400 characters (Debugging on only).

**Tools.** A tool plugin is an MCP server on stdio: JSON-RPC, one message a line, `initialize`, `tools/list` and
`tools/call`, as strata-search speaks it today.
- The tool hub starts each tool plugin that is on at the first chat that needs a tool, and lists its tools.
- A chat is offered every listed tool, at most 16 in all. A name a second plugin also lists is left out and logged.
- The model's calls are routed by name.
- The search loop becomes the tool loop. `tools.max_rounds` (1-8, default 8; `search.max_rounds` moves there) bounds an
  answer's rounds.
- A result is cut at 20000 characters (upstream's hub cuts it there; built 2026-10-30), and the page shows each call,
  as it shows searches today.
- A tool block may carry a `note` (one paragraph, `{today}` filled in). It joins the chat's system message while that
  plugin's tools are offered: search's says today's date and to search for anything recent.
- A program that fails is started again by a later chat, after a minute, as today.
- The tab's "Try" sends one call with the arguments typed and shows the result. The page's test search becomes this.

**Notifiers.** A notifier plugin's program reads one JSON line per event on stdin:
- events: `{"event": "<name>", "subject": "...", "body": "...", "server": "<this PC's name>", "time": "<RFC 3339>"}`,
  for the alert events of today (`server_started`, `download_finished`, `download_failed`, `calibration_finished`,
  `calibration_failed`, `engine_stopped`);
- the tab's "Send a test": `{"test": true, "server": "..."}`, answered by one line on stdout, `{"ok": bool, "reply":
  "<text>"}`, within 60 seconds;
- the host starts each notifier plugin that is on when the server starts, and starts one that ended again after a minute;
- events wait in a queue of at most 16, and an event raised while the program is down is dropped and logged;
- a notifier with an `events` setting (a text list of event names) gets only those; one without gets all;
- (built 2026-10-30) a notifier may answer an event with `{"event", "ok", "reply"}`; the log shows it, and the test's
  answer is the line without an `event`.

**Search, made a tool plugin.**
- `plugins/search/`: `plugin.json` and `strata-search(.exe)`.
- Settings: `url` (url, required), `results` (1-10, default 5), `timeout_s` (5-120, default 30).
- Arguments: `--url {url} --results {results} --timeout {timeout_s}`. Network: "the SearXNG instance at {url}".
- `make --with search` builds it there and records it in `build.json`.
- Search is no longer a build option (`SECURE_FRONTEND_WEB_SEARCH` goes): every server has the tool host. Settings'
  Web search section moves into the Plugins tab.

**Email, made a notifier plugin.**
- `strata-email(.exe)` is the SMTP client over TLS (Schannel on Windows, OpenSSL elsewhere), the per-kind limit and the
  test, moved out of the server: the server holds no SMTP client any more (built 2026-10-30:
  `SECURE_FRONTEND_EMAIL_ALERTS` and `SECURE_FRONTEND_WEB_SEARCH` stay, each building only its plugin's program).
- Settings: `host`, `port` (default 465), `security` (`ssl` or `starttls`), `username`, `from`, `to` (1-5 addresses),
  `events`, and `password` (a secret, given to the program as `SMTP_PASSWORD`).
- Network: "the mail server {host}:{port}".
- The page's IONOS fill-in becomes the manifest's `presets` (a label and values the tab fills in).

**Moving the old rows.** At the first start after the change:
- `search.url`, `.results` and `.timeout_s` become `plugins.search.*`, `search.max_rounds` becomes `tools.max_rounds`,
  and `search.enabled` becomes the search plugin's on or off;
- `alerts.email.*` becomes `plugins.email.*`, `alerts.email.enabled` the email plugin's on or off, and `smtp-password`
  in `roots.secret` becomes `plugin-email-password`;
- `config.json` is rewritten owner-only with the rows moved, and the server says so once in its log. `--check` names the
  rows it would move and writes nothing.

**Third-party tools and notifiers** run like these two once approved. The approval question says that the code runs on
this PC with the server's rights, and what network it says it uses.

### 11. Trust by key (decision 8; the owner, 2026-10-30)

A plugin signed by a key the server trusts runs without being approved one by one; unsigned plugins keep approval with
SHA-256 pinning, and make's build record keeps trusting what make built.

**The signature.** `plugins/<id>/plugin.sig` beside `plugin.json`: the release envelope's form (`release_manifest.hpp`) -
a signed statement, the line `--STRATA-SIGNATURE-V1`, and the base64 Ed25519 signature over exactly the statement's
bytes. The statement is text, one item a line, each line ending in LF:

    m-strata.plugin-signature/1
    key <the signing key's public key, 64 lowercase hex>
    id <the plugin's id>
    platform <windows or linux>
    manifest <plugin.json's SHA-256>
    file <a file the manifest lists for that platform> <its SHA-256>     (one line each, sorted by name)

It holds what the trust pins today (the manifest and each file of this platform), so a signature is for one platform's
folder; a folder for the other platform is signed again. The server reads at most 16 KiB of it.

**Keys.** A key is an Ed25519 key pair. The server trusts:
- the install's own key, whose public half make writes into `engine/build.json` (`signing_key`);
- each publisher key the owner adds in the Plugins tab or the TUI's Plugins view (`k`, or `t` on a plugin signed with a
  key not yet trusted; `POST /v1/strata/plugins/keys`), kept in `plugins.json` with a name (at most 16).
A key is trusted only from those two places, never from a plugin or a request.

**The order of trust.** For each plugin: (1) make's build record, if its SHA-256s match; (2) its signature, if it
verifies under a trusted key and its statement names this plugin, this platform and its SHA-256s now; (3) the owner's
approval, if its SHA-256s are still the approved ones. Otherwise it waits (or, approved once, it changed). A plugin
trusted by its signature starts on, as an approved one does, and the tab says who signed it. A signature that does not
verify, names an untrusted key, or does not match the folder is shown with why, and the plugin falls through to (3).

**The private key** stays out of the install and the data folder: `%APPDATA%\M-Strata\plugin-signing-key.json` on
Windows, `$XDG_CONFIG_HOME/m-strata/plugin-signing-key.json` (else `~/.config/...`) elsewhere, or `--signing-key PATH`.
On Windows the seed is protected with DPAPI for this user; elsewhere the file is 0600 in a 0700 folder. With
`--signing-passphrase` it is also sealed with a passphrase (scrypt, n=2^14, r=8, p=1; the seed XORed with the first 32
bytes of the derived key, the last 32 an HMAC-SHA256 key over the salt and the sealed seed, so a wrong passphrase is
refused); `install.bat -SigningPassphrase` and `install.sh --signing-passphrase` pass it on, they ask whether to when
this PC has no key yet, and a key already sealed is passed on as sealed. make creates the key at its first build when
there is none, signs each first-party plugin it stages, and
writes the public key into the build record; `make.py --sign-plugin FOLDER` signs a plugin made later with the same key,
and `make.py --signing-key-show` prints its public key for another install's owner to add.

**The code.** No new crypto dependency (the project's rule since the release trust chain, PROVENANCE "Vendored crypto"):
the server verifies with `secure_frontend_ed25519`, its own RFC 8032 verifier (verification only), and make signs with
`scripts/release/sign.py`, the RFC 8032 section 6 reference code. This replaces decision 8's "through Monocypher":
Monocypher would have been a second Ed25519 implementation beside a tested one. The reference signer is not
constant-time; it signs on the owner's own PC, where no one times it.

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

0. **Done:** the engine ABI and the launcher's `--family` (5beb6a12).

1. **The contract.**
   - The ABI header and the launcher's `--family` (ABI, id and SHA-256 checks). Done: 5beb6a12.
   - The manifest schema and its validator. Done for every kind but the engine block (5d0fa754): that block declares
     what a descriptor holds, so it follows phase 2's first step.
   - The fake plugin and its tests.
2. **The descriptors.**
   - `FamilyDescriptor` for the built-ins, with no behaviour change (every existing suite stays green). Begun: the
     engine each family talks to, its config rows, launching and calibration (`family_descriptor.hpp`); the cards' and
     options' knobs next.
   - Then manifest-driven descriptors for plugins: cards, options, arguments, calibration, Monitor, capabilities.
3. **Prompt format.** The declarative template renderer, if decision 1 is (a), with its refusal tests.
4. **Downloads.** Plugin entries in Download models, `prepare: plugin`, and the MTP and picture files.
5. **Trust and the Plugins tab.** Discovery of every kind, approval and pinning, first-party trust through the build
   record, the Plugins tab and the TUI's view, and the threat model. Done: 5d0fa754, 764392bc, c64018f8.
5b. **Feature plugins** (section 10). Done:
   - debugging as a diagnostics plugin (b7892340), the TUI's Plugins view (c64018f8);
   - plugin settings and secrets (103f059c);
   - the tool host, and search as a tool plugin, its rows moved (21b2915f, 77e64b37);
   - the notifier host, and email as a notifier plugin (strata-email), its rows moved (ebb10d21);
   - make and the installers build them as plugins; third-party tools and notifiers run once approved (the same two).
5c. **Key-signed trust** (decision 8, section 11): after 5b. Done: 7b9cd67d, then 0628a468 and 8121d685.
6. **The SDK and the guide.**
   - `strata_family_runtime` and the skeleton family.
   - `make.py --sdk` / `--plugin`.
   - `docs/ENGINE_PLUGINS.md`.
7. **The proof.** Flint as a plugin, or the first new family.

## Decisions (the owner, 2026-10-30)

1. Prompt format: **(a) a declarative template in the manifest** ("Declarative template"). No model-provided template
   runs.
2. Trust: **(a) approve each plugin, with SHA-256 pinning** ("Approve each"), in the Plugins tab (section 8).
3. Proof: **(a) re-package flint as a plugin first** ("Re-package flint").
4. First-party plugins: **trusted through the build record** ("Trusted by build"). Third-party ones need approval.
5. Order: **the platform and the Plugins tab first** ("Platform + tab first"), proven with debugging as a manifest-only
   plugin; then search and email; then the engine-family phases, ending with flint.

6. Settings: **moved into the plugins** ("Move to plugins.<id>.*"): each manifest declares them, the Plugins tab
   draws them, and `config.json` keeps them as `plugins.<id>.*`; the old rows are moved once (section 10).
7. Third-party tool and notifier plugins: **run in this step** ("Now"), once approved.
8. Trust by key (the owner's proposal, then "let's do that and implement it after you finish the current task"): the
   install makes a key pair; make signs the plugins it builds (a signature over the manifest and each file's SHA-256);
   the server trusts plugins signed by a key it lists (the install's own, and any publisher the owner adds); later
   plugins are signed with the private key, which stays out of the install and the data folder (DPAPI or 0600,
   optionally a passphrase). Ed25519 through Monocypher (vendored, recorded in PROVENANCE; built with the repository's own
   verifier and the RFC's reference signer instead, section 11). Unsigned plugins keep
   approval with SHA-256 pinning. Built after 5b.

**One folder for every kind** (2026-10-30, with decisions 4 and 5). Every plugin is `<install>/plugins/<id>/` with
`plugin.json` (schema `m-strata.plugin/1`, its `kind` one of engine, tool, notifier, diagnostics). An engine plugin's
library lives there too, and the launcher finds it as `<engine folder>/../plugins/<id>/multiengine_<id>`. The
`family.json` and `engine/families/<id>/` of sections 1-3 read as `plugin.json` and `plugins/<id>/`.
