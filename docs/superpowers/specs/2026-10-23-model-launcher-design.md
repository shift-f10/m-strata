# Model launcher in the web UI — choose a configured model, set its options, start it

Date: 2026-10-23. **Status: ACCEPTED — the owner took the recommendation of every decision
D1-D10 (§10) on 2026-10-23: "Accept the recommendations for D1-D10 in the model launcher spec;
write the plan and start."** Plan: `docs/superpowers/plans/2026-10-23-model-launcher.md`, which
fixes the card's file shape (§3.2: the card-owned config keys sit in a `config` object parsed by
the main config's reader) and the API's JSON.

Owner request (2026-10-23): "work on being able to switch models using the webui as the next
step", then: "what I want is that when the web ui is up, I am able to select configure models in
the webui then be able to select settings like mtp, kv cache streaming, etc and start it up."

## 0. Summary

Today `strata-secure-server` binds **one** model at boot: the launch (executable, argv,
environment), the chat profile, the tokenizer pack, the chat route's settings and the served
name are all fixed in `server_bootstrap.cpp`, and changing any of them means editing a config file
and restarting the server from a terminal.

This spec makes the server a **launcher**: the operator configures a small set of **model cards**
on disk (one JSON file per model: which engine, which weights, which tokenizer, the fixed
arguments, and which options the page may change within which bounds). With the page up, the
operator opens a **Models** tab, picks a card, sets its options — MTP on/off, KV precision, KV
streaming, context length, expert cache, thinking default — and presses **Start**; the server
stops whatever engine runs, composes the chosen engine's argv from the card plus the typed
options (validated server-side, never free text), loads that model's tokenizer and profile, and
starts it; **Stop** unloads it. Chat uses whichever model is running. One engine at a time (one
GPU, 16 GB).

The security boundary is kept: the page never supplies a path, an executable, an environment
variable or a free-form argument; it chooses a card by name and option values from the card's own
typed, bounded list, and every value goes through the same validators and allowlists the config
path uses today.

## 1. Goals and non-goals

### 1.1 Goals

1. A **Models** tab in the page: every configured card with its state (stopped / starting /
   running / failed), the running one marked; per-card option controls; **Start** and **Stop**.
2. **Options per engine** (§4), typed and bounded by the card: for the Qwen engine (upstream
   Strata) MTP on/off with its window, KV precision, KV streaming (resident cells), context,
   expert cache, prompt chunking; for the Gemma engine context, chunk, expert VRAM; for both the
   thinking default and the sampling defaults.
3. **Switching** is safe: a switch never cuts a generation in half without saying so (§5.2), never
   serves a request with one model's tokenizer on another model's engine, and leaves the server
   usable if the new engine fails to start (it reports the failure; the operator can start
   another card).
4. The server can **boot with no engine** and wait for the page (§10 D5), so the GPU is free until
   the operator chooses.
5. The OpenAI surface stays coherent: `/v1/models` lists the running model (and, as an extension
   field, the others with their state), and chat requests address the running model (§5.4).

### 1.2 Non-goals (v1)

| Non-goal | Why |
| --- | --- |
| Adding, editing or deleting cards from the page | cards name executables and weight files: they stay operator files on disk (D1) |
| Downloads from the page | the SP4 acquire path (`strata pull`) stays the CLI's |
| Two models loaded at once | one GPU; a second engine would not fit beside the first |
| Free-form engine arguments from the page | the allowlist rule of SP1/SP5: only typed options |
| KV streaming for the Gemma engine | the engine has none yet; it is its own engine plan (D8) |
| GLM-5.3 cards | the page cannot chat with glm53 today (session log item 46); a GLM card follows that fix |

## 2. Today's single-model wiring (what changes)

`server_bootstrap.cpp` composes `ComposedLaunch` (`compose_engine_launch`), loads the tokenizer
pack in the profile's mode, builds `SupervisorConfig` (profile, flavor), the
`EngineLaunchPlan` (argv, the closed environment, the state cwd), one `EngineSupervisor`
(`make_stdio_process_factory(plan)`), one `ChatRoute` (holding `const TokenizerPack&` and a
`ChatRouteConfig` with the model id, profile, thinking defaults, sampling defaults) and the
`ApiRouter` (model id, aliases). The supervisor already knows an **`unloaded`** state
(`engine.lazy_load`, `engine.idle_unload_s`): it can tear its child down and respawn it from the
same factory. The launcher generalises that: the factory and the per-model bindings become
replaceable while nothing is leased.

## 3. Model cards

### 3.1 Where they live

The main config gains a `models` block (D1):

| Key | Type | Meaning |
| --- | --- | --- |
| `models.cards` | string array | absolute paths of card files (owner-only, like the config) |
| `models.start` | string | the card started at boot; empty: boot with no engine (D5) |
| `models.control` | boolean | the page and API may start/stop cards (default false) |
| `models.control_remote` | boolean | ... also from non-loopback clients (default false; D2) |

`--expert` keeps today's single-model config working unchanged; the launcher is what runs when
`models.cards` is set (the two are mutually exclusive, refused by name).

### 3.2 A card

```json
{
  "schema_version": 1,
  "name": "Qwen3.8-Flash-Next",
  "profile": "qwen",
  "config": {
    "engine": {"executable": "C:\\multi-strata\\out\\serve\\engine\\strata.exe",
               "model_id": "Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002",
               "lib_dirs": ["C:\\multi-strata\\out\\serve\\engine", "C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\v13.0\\bin\\x64"],
               "args": ["--pack", "C:\\Strata-data\\packs\\iq3_s", "--ple-gguf", "...", "--mtp", "C:\\Strata-data\\mtp\\rt"]},
    "roots": {"models": "C:\\Strata-data\\models", "tokenizer_pack": "C:\\Strata-data\\packs\\iq3_s\\tokenizer"},
    "defaults": {"sampling": {"temperature": 0.7, "top_p": 0.95}},
    "reasoning": {"effort": "high"}
  },
  "options": {
    "max_context": {"default": 131072, "max": 131072},
    "kv": {"default": "int8", "choices": ["fp16", "int8", "q4_0", "k8v4"]},
    "kv_resident": {"default": 32768, "min": 0, "max": 131072},
    "mtp": {"default": true},
    "spec": {"default": 4}
  }
}
```

(The `config` object holds only card-owned keys of the main config schema, read by the same
reader; the full list is in the plan, Task 1.)

- `engine.args` are the operator's **fixed** arguments, validated by the profile's existing rules
  at load (`validate_engine_user_arguments`: owned flags, unsupported modes, the `strata_gemma4`
  allowlist); a flag that an option controls (`--kv`, `--kv-resident`, `--max-context`, ...) is
  refused in `args` by name, so the page's value is the only one.
- `options` lists **only** the options the page may change, each with a default and the card's
  own bounds inside the engine's hard bounds (§4); an option not listed is not shown.
- The card file is validated with the main config's machinery (strict JSON, unknown keys refused,
  every path absolute and owner-only, executables by SP1's rules). A bad card is refused by name
  at boot; the others still load (D9).

## 4. Options per engine (v1)

Each option maps to one flag the frontend composes; the hard bounds are the engine's.

| Option | Qwen engine (upstream Strata, `strata_0143`) | Gemma engine (`strata_gemma4`) |
| --- | --- | --- |
| `max_context` | `--max-context N` (the frontend's 1,048,576 cap) | `--max-context N` (1-262,144) |
| `kv` | `--kv fp16\|int8\|q4_0\|k8v4` | `f16` only (fixed; `f32` refused in v1) |
| `kv_resident` (**KV streaming**) | `--kv-resident N`, 0 = off, else ≥ 20,480 and ≤ context | not available (D8) |
| `kv_grow` | `--kv-grow` on/off | — |
| `mtp` | on: the card's `--mtp <dir>` stays; off: the draft flags are stripped (today's `engine.mtp=false` rule) | — (no MTP block in the file) |
| `spec`, `spec_min_p` | `--spec N`, `--spec-min-p P` (bounds read from the engine in plan Task 2), only with MTP on | — |
| `expert_cache` | `--expert-cache auto\|N` | `--expert-vram GiB` (0-1024) or auto |
| `prefill` / `chunk` | `--prefill auto\|N` | `--chunk N` (1-8,192) |
| `thinking` | the default level (none/low/medium/high) | off / on (D10 of the Gemma spec) |
| `sampling` | temperature, top_p, top_k, min_p | the same |

The page renders each listed option as a control (switch, number with bounds, choice list) from a
server-supplied description, so a new option is a server change only.

## 5. Server design

### 5.1 The launcher

A `ModelLauncher` owns: the card registry (loaded at boot), the **active binding** (card name,
profile, tokenizer pack, `ChatRouteConfig`, served name, the composed launch), and the one
`EngineSupervisor`, whose factory it replaces on a switch. `ChatRoute` and `ApiRouter` read the
active binding through the launcher (a shared, immutable snapshot per request: a request validated
against binding *B* runs on the engine of *B* or fails with a fixed error, never on another).

### 5.2 A start (switch)

1. Validate the request: card name known; every option value typed and inside the card's bounds;
   control allowed for this client (D2). Refusals are fixed messages naming the option.
2. Compose the launch (the card's fixed args + the options' flags), load the tokenizer pack in
   the profile's mode — **before** touching the running engine, so a bad card never takes the
   running model down.
3. Close the gate to new generations (new chat requests get `503 model_switching` with
   `Retry-After`); wait for the in-flight generation to finish, bounded (D3); then stop the
   engine (the supervisor's `unloaded` path).
4. Install the new binding and factory; start the supervisor; wait for READY (the engine's own
   load time: 11-30 s for the Gemma Q8 file on the owner's box).
5. Open the gate. The page polls the state; a failure (exit before READY, timeout) leaves the
   launcher in `failed` with the engine's fixed reason, the old model **not** restarted (D4).

One switch at a time; a second start while one runs is refused (`409 switch_in_progress`).

### 5.3 API (authenticated; only when `models.control`)

| Route | Body | Answer |
| --- | --- | --- |
| `GET /v1/strata/models` | — | every card: name, profile, state, the option descriptions (type, default, bounds, choices), the running card's current values |
| `POST /v1/strata/models/start` | `{"name": "...", "options": {...}}` | `202` and the new state; or a fixed refusal |
| `POST /v1/strata/models/stop` | — | `202`; the GPU is freed |

Each start and stop is logged (stderr, fixed text: card name, option names and values, the
client's loopback/remote class — never the key).

### 5.4 The OpenAI surface

`/v1/models` lists the running model (the OpenAI shape) and, as an extension field
`strata_models`, the cards and their states. A chat request whose `model` names another card
gets `409 model_not_running` naming it (D6); with no model running every chat request gets
`503 no_model_running`.

## 6. The page

- A **Models** tab beside Chat and Monitor: one row per card (name, engine, state), the running one
  first; selecting a card shows its options as controls with their bounds; **Start** (with a
  confirmation when another model is running and would be stopped) and **Stop**; a progress line
  while loading (the supervisor's state, the Monitor's VRAM figure rising).
- The chat header shows the running model's name; with none running, the chat box says so and
  links to the Models tab.
- The Sampling drawer's defaults come from the running card.

## 7. Security review items

- No path, executable, environment variable or free-form argument ever comes from the page or API;
  a start names a card and typed option values only.
- Model control is off unless configured, and loopback-only unless `models.control_remote` (D2);
  every start/stop is logged.
- Option values are validated twice: against the card's bounds and by the engine profile's own
  argument rules when the argv is composed.
- A start cannot take the running model down before the new card has validated and its tokenizer
  loaded (§5.2 step 2).
- Thrash: one switch at a time; while switching, chat is refused rather than queued for minutes.
- Threat model and PROVENANCE lines are added; the Job/process-group confinement is unchanged (one
  supervised child at a time).

## 8. Testing

- Hermetic: card parsing and refusals (each rule by name); option → flag composition per profile
  (every option, every bound, the args-conflict rule); the launcher state machine on the fake
  engine (start, switch with a generation in flight, failed start keeps the server usable, stop,
  one switch at a time, requests during a switch get 503); the routes (auth, control off,
  remote refusal, malformed bodies); the page (the tab, the controls rendered from descriptions,
  the confirmation).
- On the box: switch Qwen IQ3_S ↔ Gemma Q8 from the page with different options (MTP on/off, KV
  int8 vs k8v4, KV streaming 32K at 128K context); chat after each; the Monitor follows.

## 9. Plan outline

1. Cards: schema, loader, validation (frontend config machinery), `models.*` keys.
2. Options: per-profile option descriptions and the composition into argv (Qwen and Gemma).
3. The launcher: replaceable binding + factory, the switch state machine, the gate's switching
   mode; `ChatRoute`/`ApiRouter` read the active binding.
4. The API routes and `/v1/models` extension.
5. The page: the Models tab, option controls, start/stop, states.
6. Box: two cards (Qwen IQ3_S, Gemma Q8), switching from the page; docs truth pass.

## 10. Owner decisions (recommendations in **bold**)

| # | Decision | Options |
| --- | --- | --- |
| D1 | Where models are configured | **one card file per model, listed in the main config (`models.cards`)**; or inline in the main config |
| D2 | Who may start/stop models | **API-key holders on loopback; remote only with `models.control_remote`**; or any key holder |
| D3 | A start while a generation runs | **wait for it, up to 60 s, then cut it as `cancel`**; or refuse the start until idle; or cut at once |
| D4 | A start that fails | **stay stopped and show the error (the old model is not restarted)**; or fall back to the previous card |
| D5 | Boot | **`models.start` names a card, or empty = boot with no engine and wait for the page**; or always start the first card |
| D6 | A chat request naming another card | **409 `model_not_running` (the page switches explicitly)**; or switch automatically (Ollama-style) |
| D7 | Remember the page's last options per card | **yes, in an owner-only file under `roots.state`, used as the next defaults**; or no memory (card defaults each time) |
| D8 | KV streaming for the Gemma engine | **its own engine plan after v1 (the option appears when the engine has it)**; or build it first |
| D9 | One bad card file | **refuse that card by name, load the rest**; or refuse to boot |
| D10 | GLM-5.3 cards | **after the page's glm53 chat fix (item 46)**; or in v1 together with that fix |
