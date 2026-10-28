# SP5 — Parity with the engine's real option surface (design)

Date: 2026-10-21. Status: **owner decisions D-1…D-10 recorded, builder-check
resolved — awaiting your approval of the whole.**
Owner decisions recorded inline as `D-n`; controller choices as `C-n`.

## 1. Intent and why

SP3/SP4 made the secure frontend serve and manage models, but the frontend can
only configure what SP1/SP2/SP3 typed into `server.json`. The engines offer far
more: the pinned upstream Strata `v0.2.1.3` reads **87 run-config keys and ~35
`STRATA_*` env knobs**; Project Maya `v1.1.8` reads its own surface plus a
~100-name `STRATA_GLM_*` engine-env family. SP5 closes that gap: **the secure
frontend exposes the same options the current engines have**, as bounded typed
values — the owner's words: *"I want the same options as the current strata
has."* Parity is completeness of the table, not imitation of the engines'
freedom: unknown options are rejected, never passed through.

## 2. Decisions (owner, 2026-10-21)

- **D-1 Parity model, staged.** The typed table + CLI + fake-engine proofs gate
  every merge on the Linux dev host. A **conformance kit** (identical requests
  against the real engines) is the SP5 acceptance step, run **once** on the
  owner's NVIDIA box, results recorded into `docs/PROVENANCE.md` like the SP3
  GPU kit. Merges never wait on GPU hardware; final SP5 sign-off always does.
- **D-2 Scope = the full real surface.** Every option of the pinned references:
  upstream `v0.2.1.3` (Strata profile) and Project Maya `v1.1.8` (GLM profile).
  Nothing is curated away except the security exclusions of §4.
- **D-3 Security exclusions stay, as tested reject rows.** The approved threat
  model's exclusions are the only non-parity items; each appears explicitly in
  the table (key present in config → typed error) and is tested.
- **D-4 The orphaned verbs land in SP5:** `strata serve` (foreground serve),
  `strata version`, `strata key show|rotate`.
- **D-5 The `glm53` profile's parity reference is Project Maya** (`mw00/project-maya`,
  tag `v1.1.8`, commit `5c970a863ce2fdd87b6e23ffb4822f6d76522b1c`, already
  pinned in PROVENANCE). Today `glm53` passes engine arguments through
  untyped; SP5 replaces that with typed tables derived from Maya's real
  surface. SP6's decision sheet is NOT needed here — it governs building and
  porting the engine, not the option table.
- **D-6 Split into two milestones.** **M1 (Strata):** typed table for
  `v0.2.1.3` + verbs + reject rows, fake-engine gated. **M2 (GLM + kit):**
  Maya-`v1.1.8` table replacing `glm53` pass-through + the one conformance kit
  for both profiles.
- **D-7 Full GLM knobs supersede decision A10 (2026-10-14).** Owner,
  2026-10-21: *"we need the same knobs as the project maya has."* All ~100
  `STRATA_GLM_*` knobs become typed rows with Maya's kinds/defaults at the pin;
  A10's "no tuning knobs pass" no longer bounds scope. Safety values from
  SP6 §4.9 (`STRATA_GLM_SLOTS` off, loop guards, forced split) stay as
  **forced defaults** the table can move but the frontend never sends unknown.
  `C-5:` one tension is kept rather than overridden: `STRATA_GLM_DFLASH` stays
  **rejected on the A7 license ground** (not a knob-visibility question);
  flagged for the owner to confirm while reviewing.
- **D-8 One client speaks both engine dialects.** "Project maya has a
  strata-based engine, it's got to be possible to support both." SP5 M2 adds a
  dialect layer to `engine_protocol_client`: the `strata` (v0.2.1.3) and
  `maya-1.1.8` (`STAT` cadence, 5-field `PP`, `NOTE`) framings, selected by
  engine profile, proven with a fake-engine Maya mode. The GLM half of the
  acceptance kit stays in SP5: on the owner's box the reference side runs
  Maya's own server and our side runs the Maya engine (built from the pinned
  source by the SP6-designed recipe; the kit doc carries the build steps).
- **D-9 Vision split.** Image **input** (supervised vision child, `mmproj` as
  a manifest artifact role, `ENC` plumbing, chat `image_url` content) is typed
  and plumbed in SP5 (real encode behaviour proven only by the kit). Image
  **generation** (`/v1/images/generations`, `sd-cli`, file-hosting) is **out**
  — the route answers 404 like any unknown route, with a mapped-away row.
- **D-10 Maya is vendored as a pinned source.** M2 task 1 adds the pinned
  Maya source (commit `5c970a86…`, tag `v1.1.8`) as a read-only reference in
  the repo — never built on any lane, excluded from packaging and from the
  distribution check — so every GLM table row cites `file:line` a reviewer
  can open locally. (Pulls that piece of SP6 forward; nothing else moves.)

## 3. What parity means operationally

For every option of a reference engine, the frontend has exactly one of three
outcomes, and each outcome is a row in the profile's table:

1. **Typed** — a config key (or request field) with a bounded value whose
   *kind, range, default, and effect* are the reference's, taken from the
   pinned source at cited `file:line`. It reaches the engine either as a
   composed engine argument, as a typed child-env entry, or it changes
   frontend behaviour itself — chosen per row (a "where it lands" column).
2. **Rejected** — a security exclusion (§4). Presence in config = typed error
   naming the key; already the pattern for `mcp_servers` (EXT-02).
3. **Mapped-away** — the capability exists differently by design (e.g. engine
   path is fixed by the install, SP4 manifest composes launch args). Documented
   per row.

`C-1:` exact bounds/default/validation for every typed row are **copied from
the pinned reference source, not invented**; each plan task cites the
`file:line`. Where the reference itself enforces no bound, the table adds the
security bound (finite ranges, MiB/GB caps) and says so.

## 4. Security exclusions (D-3), identical for both profiles

- Command execution: `before_load`, `before_load_strict`,
  `before_load_ignore_failure`, `before_load_timeout_s`; MCP: `mcp`,
  `mcp_servers`, `mcpServers` and their `command/args/env/cwd/url/type/headers`.
- Free-form process control: `exe`, `args`, `user_args`, `env` (raw dict),
  `cwd`, `log`, `command`, `lib_dirs`, Maya's `--engine mock` scripts.
  (Engine identity comes from the SP4 manifest; child env is the typed
  allowlist — SP2 — now extended by these tables.)
- Web/desktop/telemetry: `api_monitor`, `open_browser`, `prevent_sleep`,
  `batch_telemetry`/`prometheus` endpoints (no new web surface), and **Maya's
  self-updater block** (`installer`, `MAYA_UPDATE_CHECK`, `MAYA_RESTART_ON_UPDATE`,
  `serve/update.py`) — PRIV-01 forbids update checks; the reject rows say so.
- Raw env knobs are **not honored** as environment input (product-spec rule,
  one documented source per value); `strata doctor` warns when any `STRATA_*`
  or `MAYA_*` name is set in the process environment. Their *semantics* are
  honored as typed config keys instead.
- `C-5` licence, not surface: `STRATA_GLM_DFLASH` (DFlash2 draft-model feature)
  stays **rejected** pending SP6's A7 licence question even under D-7 — confirm
  or move while reviewing.

These are the only non-parity items; a config containing any excluded key fails
at load with the §7.2 error shape naming it.

## 5. The tables (what M1/M2 must contain)

### 5.1 Strata profile — upstream `v0.2.1.3` (M1)

87 config keys + ~35 env knobs; complete enumeration with governing files in
Appendix A. Families: listen/http/token (already typed — re-checked against the
new pin), sampling defaults, **reasoning** (`reasoning_effort`,
`reasoning_budget_tokens`, `reasoning_close_retry`, `reasoning_loop_recovery`,
`effort_position`, `anthropic_thinking`, `fit_max_tokens`), **lifecycle/VRAM**
(`lazy_load`, `idle_unload_s`, `min_free_vram_mib`, `vram_reserve_mib`,
`vram_elastic`, `vram_segment_mib`, `elastic`, `elastic_idle_s`, `elastic_mib`,
`mtp`, `gpu`, `gpu_order`, `gpu_policy`, `layer_split`, `split_skip_if_fits`,
`hip_ordinal`, `uma`), **replicas** (`parallel`, `replicas`, `replica_cpus`,
`replica_load_tokens` — typed; the frontend implements scheduling parity with
`serve/replicas.py` semantics or rejects with a documented bound — see §8),
**engine timeouts/queues** (`engine_silence_s`, `queue_timeout`, `timeout_s`,
`max_queue`, `max_n`, `max_tokens`, `min_tokens`, `answer_room`),
**sessions/context** (`slot_save_path`, `expert_profile_save`,
`expert_profile_save_every`, `codex_compaction_cache`, `codex_thread_titles`,
`tool_call_recovery`, `repeat_stop_tokens`, `keep_files`, `env_to_user`),
**vision** (`vision`/`images` block: `max_n`, `max_side`, `need_mib`, `uma`,
`no_flash_attn`, `resident`, `max_tokens`) — typed plumbing proven against the
fake engine; real image behaviour is acceptance-kit evidence,
**misc** (`aliases`, `model_name`, `tokenizer`, `arch`, `backend`, `quant`,
`sigmas`, `steps`, `out_dir`, `reload`, `type`, `telemetry_read_key`,
`batch_telemetry` — mapped-away or rejected where §4 applies).

`C-2:` config key tree follows SP1 conventions: existing namespaces keep their
place (`listen.*`, `http.*`, `token_source`, `defaults.sampling.*`), new
families are `engine.*`, `reasoning.*`, `session.*`, `vision.*`, `replicas.*`.
Unknown keys remain errors (CFG-01..04).

### 5.2 GLM profile — Project Maya `v1.1.8` (M2)

Maya has **no editable-config table and no engine-flag denylist** (verified at
the pinned commit): its surface is (a) a smaller server config (`exe/args/cwd`
required, `host`, `api_key`, `cors_origins`, `gpu`, `layer_split`, `backend`,
`tokenizer`, `model_name`, `sampling` block incl. `experimental_speed_projection`,
`reasoning_effort`, **`thinking_budget`** (default 32768 — Maya's name where
upstream uses `reasoning_budget_tokens`), `fit_max_tokens`, `vision` block incl.
`resident`/`no_flash_attn`, shared-settings defaults), (b) a huge **engine-env
allowlist**: ~100 `STRATA_GLM_*` knobs documented in Maya's `docs/SETTINGS.md`
rows 26–101 and carried as raw `env`, and (c) launcher-managed engine flags
(`--glm-pack`, `--max-context`, `--prefill`, `--kv-streaming`, `--kv-resident`,
`--dflash`, `--expert-profile*`, `--parallel`, `--spec*`, `--dump-routing`).

The GLM table therefore: Maya's config keys typed with Maya's own kinds and
defaults (Appendix B cites them); the `STRATA_GLM_*` family typed as a
**named allowlist** — every documented knob a row, kind + bound + default from
`SETTINGS.md` at the pinned commit, **unknown engine-env names rejected** (the
denylist Maya lacks); launcher-managed flags become SP4 manifest-composed
launch (mapped-away rows). Excluded per §4: `installer`, updater env knobs,
`exe/args/cwd/log` free-form, MCP, mock engine. `C-3:` Maya's renames are kept
as Maya's names in the GLM table (`thinking_budget`, not
`reasoning_budget_tokens`) — parity follows the reference, not the cousin.

## 6. The CLI verbs (D-4)

- `strata serve` — foreground serve from `server.json` (same loader,
  validation, shutdown as `run`'s supervised path; no daemonization, no
  `server.json`/lock writing). This is the verb the conformance kit uses.
- `strata version` — frontend version + `upstream.version` (`v0.2.1.3`) +
  engine self-version shown as informational, matching the bump rule "tag
  version governs".
- `strata key show|rotate` — token store maintenance (SP1 store rules; token
  never logged; `show` requires it was generated locally).

## 7. The acceptance kit (D-1)

`docs/validation/sp5-parity-kit.md` + a corpus mode on the SP3
`strata-api-probe` (one tool, both milestones): a fixed corpus of HTTP
exchanges — deterministic seeded generations, sampling matrix, reasoning
on/off, length/timeout edges, error shapes, vision images (M2), streaming
already covered by the SP3 kit — run against **plain upstream `serve` at the
pin** (reference server, owner box) and against `strata serve`/`strata run`;
the kit diffs text-for-text on greedy/seeded cases, `finish_reason`, usage,
and error codes. GLM side: same corpus against Maya's own server at
`v1.1.8` with the Maya engine, vs our `glm53` profile. Output: one PASS/FAIL
line per case + a results file hashed into the PROVENANCE SP5 entry. Nothing
in the kit runs on the dev host except build/`--help` smoke; corpus semantics
against fake engines are *lane* tests, not the kit.

`C-6` Equivalence rule: naive byte-diff would fail on our **approved**
divergences (SP3 B1–B12: error shapes §7.2, neutralized extension fields,
`max_tokens` auto/clamp, `/v1/completions`·`/v1/messages` absent). Each corpus
case declares its equivalence class — `text` (greedy/seeded: text-for-text),
`shape` (finish_reason/usage/error-code mapping to our §7.2 with the cited
approved decision), or `divergent-by-decision` (the row names the B/ruling
that authorizes it). The kit starts the **reference** server with the same
pinned engine binary and the same composed flags our manifest produces
(threads/context/parallel change GPU token sequences — the run records both
command lines verbatim). `C-7:` the kit drives `strata serve` only; the
`run`↔`server.json` product loop (writer, lock, first-run) is explicitly not
SP5. `C-8:` parity is a **config/launch surface** claim: no new routes (no
`/settings` equivalent), request fields stay exactly SP3's approved schema —
request-flavouring rows like `anthropic_thinking`/`effort_position` type the
config that governs the fields we do serve.

## 8. Remaining ground the plan must settle (builder's, all decidable)

- **Replicas**: upstream `replicas.py` schedules N engine children per model;
  the frontend's gate is one-running-generation. Typed `replicas.*` values
  whose behaviour the frontend must *implement* (not just pass along): the
  plan either implements the scheduling subset in SP5 or types the keys and
  documents a narrowed, explicit behaviour (`C-4:` implement at least
  `parallel` within one engine, which the engine itself already does; N-child
  scheduling is accepted narrowed — say so in the spec and the row.)
- **Lifecycle** (`mtp`, `lazy_load`, `idle_unload_s`): the pinned source
  confirms upstream "unload" is process teardown + respawn — the frontend
  drives it through the supervisor; this is the bulk of M1's non-table code.
- **Dialect layer** (D-8): framed parser split by engine profile; the plan
  keeps `engine_protocol_client`'s PP/STAT/T/DONE core and adds a Maya
  framing table + fake-engine Maya mode; every delta (`STAT` cadence, 5-field
  `PP`, `NOTE`) gets a named test before the kit claims GLM parity.
- **Vision input plumbing** (D-9): `mmproj` as a manifest artifact role and
  the vision child as a supervised role reusing SP2 supervision; the plan
  decides the role name and the PROC/DOS limits rows.

## 9. Gates and invariants (unchanged owner rules)

`-Werror` everywhere; per-task gates: `linux-gcc` build+serial ctest, **release
preset build** (learned rule), `windows-llvm-mingw-cross` compile, pytest.
TSan available on this host (`.toolchains/clang18`); no wine, no CI, GitHub
storage-only; `upstream/` never modified; the pinned Maya reference (D-10) is
read-only, never built on any lane, and excluded from packaging and the
distribution check. `upstream/` untouched — the parity *reference*, not a
dependency. Windows runtime and GPU claims wait
for owner hardware. Docs (`AGENT_HANDOFF`, `PROVENANCE`, threat model, README)
updated per change.

## 10. Goals / non-goals / anti-goals

- **Goals:** complete per-profile tables (typed / rejected / mapped-away for
  every reference option, each with a test); the three verbs; one acceptance
  kit for both profiles.
- **Non-goals:** building/porting engines (SP6); installing/updates (SP7);
  a web UI of any kind; honoring raw env knobs; SP6 decision items.
- **Anti-goals:** a table that passes vacuously (a rejected row with no test,
  a typed row never reaching the engine); parity by re-adding raw pass-through;
  any behaviour claim from the dev host about GPU or Windows.

---

## Appendix A — Strata `v0.2.1.3` config keys (enumerated locally from the submodule, `serve/*.py` `cfg.get`)

87 keys; governing file per key recorded in the controller extraction
(`/tmp/opencode/sp5-strata-keys.txt` at design time; the plan re-derives it
from the pin). Names: aliases allowed_hosts answer_room anthropic_thinking
api_key api_monitor arch args backend batch_telemetry before_load
before_load_ignore_failure before_load_strict before_load_timeout_s
codex_compaction_cache codex_thread_titles command cors_origins cwd
effort_position elastic elastic_idle_s elastic_mib engine_silence_s env
env_to_user exe expert_profile_save expert_profile_save_every fit_max_tokens
gpu gpu_order gpu_policy headers hip_ordinal host idle_unload_s images
keep_files layer_split lazy_load lib_dirs log max_n max_queue max_side
max_tokens mcp mcp_servers min_free_vram_mib min_tokens model_name native
need_mib open_browser out_dir parallel prevent_sleep quant queue_timeout
reasoning_budget_tokens reasoning_close_retry reasoning_effort
reasoning_loop_recovery reload repeat_stop_tokens replica_cpus
replica_load_tokens replicas sampling sigmas slot_save_path
split_skip_if_fits steps telemetry_read_key threads timeout_s tokenizer
tool_call_recovery trusted_origins type uma url user_args vision.
Env knobs (~35, `STRATA_*`, enumerated from `serve/*.py`): ADMIT_MAX_WAIT_S
ADMIT_SHORT_FIRST ALLOWED_HOSTS API_KEY BATCH_MTP BATCH_TELEMETRY BATCH_TRACE
DEBUG DROP_CACHE_ON_EXIT ENGINE_READY_S ENGINE_STALL_S FAIL_ONCE HTTP_BACKLOG
KEEP_EMPTY_TURNS KEEP_LEFTOVERS LIFELINE MAX_BODY_MIB NO_PRESET OFFLINE
OWNER_ATTEST_KEY PARALLEL_SOLO REPLICA_START REQUEST_LINES RUNTIME_PROFILE
START_RETRIES SYCL_BIN TELEMETRY_KEY TOKENIZER VERIFY_TRACE VISION_ENCODE_S
VISION_READY_S (+ edit-view keys from `serve/runconfig.py`: sampling.*,
reasoning_budget_tokens, fit_max_tokens, anthropic_thinking, effort_position,
aliases, idle_unload_s, mtp, lazy_load, engine_silence_s, api_monitor,
open_browser, vram_reserve_mib→`--vram-reserve-mib`).

## Appendix B — Maya `v1.1.8` surface (researched read-only from the pinned commit; full report retained by the controller)

Server keys (`serve/server.py` @pin): `exe*` `args*` `cwd` `tokenizer`
`model_name` `gpu` `layer_split` `backend` `env` `lib_dirs` `log` `host`
`api_key` `cors_origins` `vision{exe,mmproj,model,gpu,threads,max_tokens,
no_flash_attn,resident}` `sampling{temperature,top_p,top_k,min_p,
presence_penalty,repetition_penalty,frequency_penalty,penalty_last_n,seed,
experimental_speed_projection}` `reasoning_effort` `thinking_budget`
`fit_max_tokens` `mcp/mcp_servers/mcpServers{command,args,env,cwd,url,type,
headers}` `installer{quant,models_dir,gguf,gguf_dir,draft,written,
speed_calibration}` (* required in Maya; rejected outright for us per §4).
Shared settings (POST /settings validator, `server.py:3223-3260`):
reasoning_effort temperature top_p min_p top_k seed max_tokens
experimental_speed_projection. Server env: STRATA_ENGINE_STALL_S(90)
STRATA_HTTP_BACKLOG(256) STRATA_MAX_BODY_MIB(256) STRATA_FAIR_SLICE_S
STRATA_ENGINE_QUIT_S(50) STRATA_ENGINE_WRAP_S(40) STRATA_GLM_THINK_RESERVE(1024)
STRATA_DEBUG STRATA_API_KEY. Engine-env family: ~100 `STRATA_GLM_*` names per
Maya `docs/SETTINGS.md:26-101` + calibration writes (`…PCIE_SHARE …CPU_LANE
…READ_CHUNKS …RAM_HEADROOM_GB=16 …RESERVE_MB …`). Engine flags managed by
`maya.py`: `--glm-pack --max-context(32768) --prefill auto --kv-streaming on
--kv-resident --dflash --expert-profile[-save][-every] --parallel --spec
--spec-min-p --dump-routing`. No runconfig/`/config` route; no engine-flag
denylist; renames: `thinking_budget` (vs upstream `reasoning_budget_tokens`),
`STRATA_ENGINE_STALL_S` replaces `engine_silence_s`.
