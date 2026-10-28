# Model launcher — Implementation Plan

**Status (2026-10-23):** Tasks 1-5 built test-first on the local `main` (Tasks 1-2 commit
`3ad44536`, Tasks 3-5 the commit after it); Task 6's box run and docs follow in the session log
(`docs/validation/2026-10-21-windows-gpu-session.md`, item 57) and `docs/AGENT_HANDOFF.md`.
Departures from the text below, all recorded where they live: the control routes sit in their
own table (`kModelControlRoutes`), not in `kRouteTable`, which existing tests pin as every
server's API; the single-model boot keeps its own path and shares `compose_model()` with the
launcher instead of becoming a one-card launcher; `spec` stays when `mtp` is off (upstream drafts
by prompt lookup without the draft layer, and a native pack needs `--spec`).

**Goal:** with the page up, the operator picks a configured model card, sets its typed options
(MTP, KV precision, KV streaming, context, expert cache, chunking, thinking, sampling) and starts
it; Stop unloads it; chat uses the running model. Spec
`docs/superpowers/specs/2026-10-23-model-launcher-design.md`, decisions D1-D10 accepted by the
owner on 2026-10-23 ("Accept the recommendations for D1-D10 in the model launcher spec; write the
plan and start. Work on this to completion without asking me anything else").

**Where:** commits on the local `main` (the checked-out branch; the owner's rules allow no branch
switch without his ask), never pushed by this work. Author `shift-f10 <40450442+shift-f10@users.noreply.github.com>`,
the two trailers of `CLAUDE.md`. Workers run in detached worktrees under `.worktrees/` and hand
over patches; only the controller commits.

**Gates per task:** MSVC `/W4 /WX` build clean (`cmake --build --preset windows-msvc`), the task's
RED cases seen failing for the right reason, then GREEN; the FULL suite
(`out/run-frontend-tests.ps1`) after Tasks 1+2, after Task 3, and at the end; pytest when Python
changes. A test is never weakened.

## Architecture

- **Cards** (`model_cards.{hpp,cpp}`): one JSON file per model, listed by the main config's
  `models.cards`. A card carries `name`, `profile`, a **config fragment** (`config`: only the
  card-owned keys of the main schema, parsed by `parse_config_text`, so every existing rule
  applies) and an **options** object (the page-settable options with the card's default and
  bounds). Launcher mode is on when `models.cards` is set; the main config then may not set a
  card-owned key (refused by name).
- **Options** (`model_options.{hpp,cpp}`): the engine's option table per profile (spec §4),
  parsing a card's `options`, resolving the page's values (or the remembered ones, D7) against the
  card's bounds, and applying them to a config draft (flags appended to `engine.args`,
  `engine.mtp`, `reasoning.effort`, `defaults.sampling.*`). A card's fixed `engine.args` may not
  hold a flag an option controls.
- **Effective config of a start:** `main draft` + `card draft` + `option edits` → `validate()` →
  `FrontendConfig` → **`prepare_model(config, expert=true)`** (the per-model half of today's
  `server_bootstrap.cpp`, factored out: compose the launch, load the tokenizer in the profile's
  mode, the supervisor config, the launch plan, the chat route settings) → `PreparedModel`.
- **Launcher** (`model_launcher.{hpp,cpp}`): owns the cards, the remembered options, the state
  (`stopped | starting | running | failed`, `switching`), and the **running model** — a heap
  object holding its own `EngineSupervisor`, `SupervisorPort` and `ChatRoute`, shared by
  `shared_ptr` with every request in flight. Single-model configs (today's `--expert` and managed
  mode) become a launcher with one fixed model and control off, so there is one code path.
- **Server-wide pieces stay single:** the listener, the `GenerationGate`, the `ServeMonitor`, the
  host sampler, the signal hook (now a launcher-owned `SupervisorWake`; a binding's supervisor
  stopping no longer shuts the listener — only a shutdown does).
- **API** (Task 4): `GET /v1/strata/models`, `POST /v1/strata/models/start`,
  `POST /v1/strata/models/stop` (keyed; 404 unless `models.control`; non-loopback clients 403
  unless `models.control_remote`). `RequestContext` gains the peer's `AddressScope`.

### JSON shapes (fixed here; the page is written against them)

`GET /v1/strata/models`:

```json
{"object": "list", "running": "Gemma-4-26B-A4B-it", "state": "running", "error": null,
 "data": [{"name": "Gemma-4-26B-A4B-it", "profile": "gemma4", "state": "running", "error": null,
           "options": [{"name": "max_context", "kind": "integer", "label": "Context length",
                        "help": "Tokens of context the engine allocates", "default": 32768,
                        "min": 4096, "max": 262144, "value": 32768},
                       {"name": "kv", "kind": "choice", "label": "KV cache precision", "help": "...",
                        "default": "int8", "choices": ["fp16", "int8"], "value": "int8"},
                       {"name": "mtp", "kind": "boolean", "label": "MTP draft layer", "help": "...",
                        "default": true, "value": true},
                       {"name": "expert_cache", "kind": "integer_or_auto", "label": "...",
                        "default": "auto", "min": 0, "max": 4096, "value": "auto"}]}]}
```

`state` per card: `stopped | starting | running | failed` (`failed` carries `error`, a fixed
text). Top-level `state` is the launcher's (`stopped` when nothing runs). Kinds: `boolean`,
`integer`, `decimal`, `choice`, `integer_or_auto`, `decimal_or_auto` (the value `"auto"` or a
number). `value` is what the next start uses (the remembered value, else the default).

`POST /v1/strata/models/start` body `{"name": "...", "options": {"max_context": 131072, ...}}`
(options optional; missing ones take `value`) → `202 {"state": "starting", "name": "..."}`;
refusals: `400 invalid_request` (fixed message naming the option), `404 model_not_found`,
`409 switch_in_progress`, `403 control_not_allowed`.
`POST /v1/strata/models/stop` → `202 {"state": "stopped"}`.
While a switch runs, chat answers `503 model_switching` (`Retry-After: 5`); with nothing
running, `503 no_model_running`; a request naming a card that is not running, `409
model_not_running` (D6). `/health` answers 200 `{"status": "ok", "engine": "none"}` with nothing
running. `/v1/models` lists the running model and adds `"strata_models": [{"id", "state"}]`.

## Tasks

### Task 1: Cards and the `models.*` keys
**Files:** `frontend_config.{hpp,cpp}` (schema rows `models.cards` string array of absolute paths,
`models.start` string, `models.control` bool, `models.control_remote` bool; `ModelsConfig`
in `FrontendConfig`; README table), new `model_cards.{hpp,cpp}`; tests new
`model_cards_test.cpp`, `typed_config_test.cpp`.
- `parse_model_card(text, dir)` / `read_model_card(file)` (bounded like the config file, owner
  only), `load_model_cards(files)` → `{cards, refused}` (D9: a bad card is refused by name, the
  rest load; two cards with one name refused), `card_draft(main, card, values)` → the merged
  draft (main's card-owned keys refused by name), `card_config(...)` → `ConfigValidationResult`.
- Card rules: `schema_version` 1; `name` (the served name; `model.name` inside the fragment is
  refused); `profile` `qwen` or `gemma4` (D10: `glm53` refused); the fragment's keys within the
  card-owned set: `engine.*` except `engine.lazy_load`/`engine.idle_unload_s`/`engine.profile`
  (set from `profile`), `model.aliases`, `roots.models`, `roots.tokenizer_pack`,
  `defaults.sampling.*`, `reasoning.*`, `limits.chat.max_prompt_tokens`,
  `limits.chat.max_completion_tokens`, `limits.chat.default_max_tokens`; `vision.*` refused
  (no vision in launcher v1); a card's `engine.args` holding an option-controlled flag refused by
  name.
- RED: `card_parses_and_merges_into_a_valid_config`, `card_refusals_name_the_rule`,
  `bad_card_is_refused_others_load`, `main_config_may_not_set_card_keys`,
  `models_keys_typed_and_described`.

### Task 2: Engine options (worker)
**Files:** new `model_options.{hpp,cpp}`; tests new `model_options_test.cpp`.
- The table of spec §4 per profile (`model_options_for(profile)`); `parse_card_options(profile,
  json)`; `resolve_option_values(profile, limits, requested_json, remembered)`;
  `apply_option_values(profile, values, draft)`; `option_controlled_flags(profile)`;
  `describe_options_json(...)` (the `options` array above).
- Qwen: `max_context` (`--max-context`, 1-1,048,576), `kv` (`--kv` fp16|int8|q4_0|k8v4),
  `kv_resident` (`--kv-resident N`, 0 = off, else 20,480-`max_context`; spec "KV streaming"),
  `kv_grow` (`--kv-grow`), `mtp` (false → `engine.mtp` false), `spec` (`--spec N`, only with MTP),
  `spec_min_p` (`--spec-min-p`), `expert_cache` (`--expert-cache auto|N`), `prefill`
  (`--prefill auto|N`), `thinking` (`reasoning.effort`), `temperature`/`top_p`/`top_k`/`min_p`
  (`defaults.sampling.*`). Gemma: `max_context` (1-262,144), `chunk` (1-8,192), `expert_vram`
  (`--expert-vram`, auto|0-1024 GiB), `thinking` (none|high: off|on), sampling as Qwen.
- RED: `option_tables_per_profile`, `card_options_parse_and_bound`, `page_values_resolve_within_bounds`,
  `options_apply_as_flags_and_keys`, `controlled_flags_listed`, `description_json_shape`.

### Task 3: The launcher (controller)
**Files:** `server_bootstrap.cpp` (split), new `model_launcher.{hpp,cpp}` (and
`prepare_model`), `api_router.{hpp,cpp}` (handlers read the launcher), `connection_pool.cpp` /
`request_router.hpp` (peer scope in `RequestContext`); tests new `model_launcher_test.cpp`
(fake engine), existing server e2e kept green.
- RED: `single_model_config_still_serves` (the existing e2e suite), `launcher_starts_and_stops_a_card`,
  `switch_waits_for_the_generation_then_swaps`, `failed_start_keeps_the_server_usable`,
  `requests_during_a_switch_get_503`, `one_switch_at_a_time`, `options_are_remembered`.

### Task 4: The control API (controller)
**Files:** `request_router.hpp` (route table), `api_router.cpp`, the launcher; tests
`api_router_test.cpp`, `api_server_test.cpp` (fake engine, two cards).
- RED: `model_control_routes_need_the_switch_and_the_key`, `remote_control_needs_its_switch`,
  `start_stop_and_list_through_the_api`, `chat_names_another_card_409`, `no_model_running_503`.

### Task 5: The page (worker)
**Files:** `web/index.html`, `web/app.js`, `web/app.css`; tests `api_router_test.cpp` page
assertions.
- A Models tab (hidden when `/v1/strata/models` answers 404): card list with state; the selected
  card's options rendered from `kind` (switch, number with min/max, select, number-or-auto);
  Start (confirm when another model runs), Stop; polling every 2 s while `starting`; the header
  shows the running model; with none running the chat box says so and links to the tab.

### Task 6: Box, docs, serve for the owner (controller)
- Two cards on the box: Qwen IQ3_S (upstream engine) and Gemma Q8 (our engine); switch from the
  page with different options (MTP on/off; KV int8/k8v4; KV streaming 32K at 128K context; Gemma
  at 32K and 262K); chat after each; the Monitor follows.
- README (launcher section, card format, the new keys), threat model, AGENT_HANDOFF, session log.
- Leave the server running in launcher mode for the owner (both cards, nothing started or the
  Gemma card started).
