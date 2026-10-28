# SP5 M1 — Strata parity table, lifecycle, and CLI verbs — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the secure frontend speaks the full option surface of the pinned upstream Strata `v0.2.1.3` — every config key and env knob typed, rejected by name, or mapped away with a cited rule — and lands the orphaned `serve`, `version`, `key show|rotate` verbs.

**Architecture:** one checked **parity table** (every upstream option name → disposition → destination → `file:line` citation at the pin) drives meta-tests that make silent drift impossible; typed values flow through the existing machinery — `config_schema()`/`ConfigKeyExtension` for config, `engine_arguments` for launch args, `EngineFamily`/`FamilyEnvironmentRows` for child env, `EngineSupervisor` restart policy for lifecycle — and the fake engine proves what reaches the engine.

**Tech Stack:** C++20, stdlib + OS only; CMake/Ninja from `.venv-dev/`; presets `linux-gcc`, `linux-gcc-release`, `windows-llvm-mingw-cross`; pytest for tooling tests.

**Spec:** `docs/superpowers/specs/2026-10-21-sp5-parity-typed-options-design.md` (owner-approved 2026-10-21; decisions D-1…D-10, calls C-1…C-8). This is plan **M1**. M2 (Maya table, dialect, acceptance kit) gets its own plan after M1 merges.

## Global Constraints

- C++20; `-Werror` on every build. Per-task gates (all must pass before the task commit): `cmake --preset linux-gcc && cmake --build --preset linux-gcc` (0 warnings), `ctest --preset linux-gcc` (serial; ports 20000–20099), `cmake --build --preset linux-gcc-release` (release build is a per-task gate — C rule from 2026-10-21), `cmake --build --preset windows-llvm-mingw-cross` (0 warnings), `pytest -q` (25).
- No wine, no CI, no pushes by the lane; `upstream/` never modified — it is the read-only parity reference. No new dependencies.
- Every parity row cites the pinned upstream source (`upstream/serve/server.py:NNNN` etc.) — copy kinds/ranges/defaults from the pin, never invent (C-1); where the pin has no bound, the row states the added security bound.
- Config keys never logged; error shapes §7.2 (`api_errors`); unknown-key load errors already exist (CFG-01) — reject rows must name the key.
- Commit author `shift-f10 <40450442+shift-f10@users.noreply.github.com>`, literal `Co-Authored-By: <real model> <noreply@anthropic.com>`, **no** `Claude-Session:` trailer.
- Ledger at `.superpowers/sdd/2026-10-21-sp5-m1-strata-parity/progress.md`; briefs hand-written (no `scripts/task-brief` on this host).
- Baseline before Task 1 (main `41f595bf`): ctest 746 run 100% (+4 env skips), pytest 25, cross 0 warnings, release preset 10/10.
- Spec wrinkle resolved here as **C-9**: C-8 ("request fields stay SP3's") keeps one carve-out required by D-9 — `content` array with `image_url` parts is accepted only when the model's manifest has a vision artifact; otherwise the existing `unsupported_parameter content` stands.

## Review Focus

1. **A renamed or new upstream key silently ignored** — every name the pin reads must be typed, rejected-by-name, or mapped-away; a name in neither bucket is a defect (`parity_table_test` meta-tests).
2. **Env knobs honored from the process environment** — `STRATA_*`/`MAYA_*` in env changes nothing except a `doctor` warning line (AUTH/PRIV rule: one documented source per value).
3. **Idle unload racing a running generation** — the unload timer never fires while a generation runs or a lease is held; respawn keeps `server.json`-visible readiness semantics.
4. **Oversized or malformed `image_url` payloads** — base64 length and decoded bytes bounded **before** decode; artifacts written only into private scratch with 0600.
5. **Off-by-one bounds at config edges** — min/max/default taken verbatim from the pin's validator lines; each family tests both boundary values.

---

## Task 1: Baseline and seam inventory

**Files:** none (read + ledger).

**Interfaces:**
- Produces: ledger section "M1 seams" with exact names: `config_schema()`, `ConfigKey`, `ConfigKeyExtension`, `engine_arguments` compose site, `EngineFamily`/`FamilyEnvironmentRows`/`FamilyEnvironmentHook`, `EngineSupervisor` restart policy, `resolve_max_tokens`, sampling GEN keys, `verb_doctor` (`strata_cli_network.cpp:361`), `FileRole`, `ManifestLaunch`.

- [ ] **Step 1:** reproduce the baseline (commands from Global Constraints); record counts in the ledger.
- [ ] **Step 2:** grep each seam name above, note file:line in the ledger. Any seam missing → say so before Task 2.

---

## Task 2: The parity table, the key inventory, and the meta-tests

**Files:**
- Create: `secure_frontend/include/secure_frontend/parity_table.hpp`, `secure_frontend/src/parity_table.cpp`, `secure_frontend/tests/parity_table_test.cpp`
- Create (generated once, committed): `secure_frontend/tests/parity_keys_upstream.hpp`

**Interfaces:**
- Produces:
  ```cpp
  enum class ParityDisposition { typed, rejected, mapped_away };
  enum class ParityDestination { frontend_behavior, config_only, engine_arg, engine_env, manifest_artifact, request_rule, none };
  struct ParityRow { std::string_view key; ParityDisposition disposition; ParityDestination destination; std::string_view citation; };
  std::span<const std::string_view> upstream_config_keys() noexcept;   // from the generated inventory
  std::span<const std::string_view> upstream_env_knobs() noexcept;
  const ParityRow* strata_parity_row(std::string_view key) noexcept;   // nullptr = table gap
  ```
- `parity_keys_upstream.hpp` holds `inline constexpr std::string_view kUpstreamConfigKeys[]` and `kUpstreamEnvKnobs[]`, generated from the pin with the commands in Step 1; its header comment records the command and the pin sha.

- [ ] **Step 1: Generate the inventory** (pinned submodule, v0.2.1.3):
  ```bash
  cd upstream && grep -rhoE 'cfg\.get\("[a-z_0-9]+"' serve/*.py | sort -u | sed 's/cfg.get("//;s/"//'   # config keys (expect ~87)
  grep -rhoE 'STRATA_[A-Z0-9_]+' serve/*.py | sort -u                                                     # env knobs (expect ~35)
  ```
  Write both lists into `parity_keys_upstream.hpp` verbatim (no edits, no additions).
- [ ] **Step 2: RED** — `parity_table_test.cpp`:
  ```cpp
  void every_upstream_key_has_exactly_one_disposition();   // each kUpstreamConfigKeys[i]: exactly one strata_parity_row hit
  void every_env_knob_is_dispositioned();                  // same for kUpstreamEnvKnobs; env knobs share rows by name
  void typed_rows_name_a_destination();                    // typed ⇒ destination != none; rejected/mapped ⇒ citation non-empty
  ```
  Run `ctest --preset linux-gcc -R parity_table` → FAIL (table empty / symbol missing).
- [ ] **Step 3: Implement** the table with **one row per key, all dispositions assigned now** (from Appendix A of the spec + the pin's validators). Rows are data: `{key, disposition, destination, "serve/server.py:NNNN"}`. Tasks 4–10 wire destinations; the row's existence and disposition do not change later.
- [ ] **Step 4:** meta-tests GREEN; also wire `secure_frontend_add_cases(parity_table_tests …)` in CMake. Full gate set; commit `parity: every upstream option of v0.2.1.3 has a dispositioned row with a citation`.

---

## Task 3: Rejected keys fail at load by name; `doctor` warns on env knobs

**Files:**
- Modify: `secure_frontend/src/frontend_config.cpp` (pre-scan before schema parse), `secure_frontend/src/strata_cli_network.cpp` (`verb_doctor`), tests `typed_config_test.cpp`, new doctor checks in existing CLI tests.

**Interfaces:**
- Consumes: `strata_parity_row()`, `upstream_config_keys()`.
- Produces: `std::optional<std::string> find_rejected_key(const JsonValue& root)` — first key whose row is `rejected` (checked before unknown-key detection so the message names the key and its rule).

- [ ] **Step 1: RED** —
  ```cpp
  void each_rejected_upstream_key_errors_at_load_naming_it();  // table-driven: for every rejected row, config with that key set fails; message contains the key name
  void doctor_warns_on_strata_env_knob();                      // env STRATA_HTTP_BACKLOG=128 → one "ignores STRATA_* env" doctor line; value not applied
  ```
- [ ] **Step 2:** verify RED; implement `find_rejected_key` driving off the parity table; doctor scan of `STRATA_`/`MAYA_`-prefixed env names (fixed literal line: `doctor: ignoring STRATA_*/MAYA_* environment; configure server.json instead`).
- [ ] **Step 3:** GREEN + full gates; commit `config: upstream-excluded keys rejected by name; doctor flags env knobs`.

---

## Task 4: Sampling defaults at the pin

**Files:** Modify `frontend_config` schema (`defaults.sampling.*`), `chat_generation_plan.cpp` (defaults→GEN), `sampling_params.hpp`; tests `chat_schema_test.cpp`, `chat_generation_plan_test.cpp`.

**Interfaces:**
- Produces: `struct SamplingDefaults { std::optional<double> temperature, top_p, min_p, presence_penalty, repetition_penalty, frequency_penalty; std::optional<int> top_k, penalty_last_n, seed; }` parsed from config; GEN request carries exactly upstream-named keys with the pin's bounds.

- [ ] **Step 1: RED** — `sampling_defaults_cover_the_pinned_validator()` (table: temperature ≥0, top_p 0<x≤1, top_k 1..64, min_p 0≤x≤1, presence/frequency ≥0, repetition >0, penalty_last_n int≥0, seed positive int — assert accept at each bound, reject outside; cite `serve/server.py:3275-3318` in the test comment); `sampling_defaults_reach_gen_request()` (fake engine sees `min_p`, `penalty_last_n`).
- [ ] **Step 2:** RED verified; extend schema rows (kinds/bounds copied from the cited lines — C-1); request-level override still wins (existing behaviour, re-assert).
- [ ] **Step 3:** GREEN + gates; commit `sampling: defaults match v0.2.1.3 validator and reach GEN`.

---

## Task 5: Reasoning family and `fit_max_tokens`

**Files:** Modify `frontend_config` (new `reasoning.*` rows), `engine_arguments.cpp`, `engine_environment.cpp` (strata-family rows), `chat_generation_plan.cpp`; tests `typed_config_test.cpp`, `chat_generation_plan_test.cpp`, `engine_arguments` test.

**Interfaces:**
- Produces: config `reasoning.effort` (`none|low|high|max`), `reasoning.budget_tokens` (int ≥0, pin default 0=none), `reasoning.close_retry` (bool), `reasoning.loop_recovery` (`off|retry`), `reasoning.effort_position` (`start|end`), `reasoning.anthropic_thinking` (`model|on_request`), `engine.fit_max_tokens` (bool, default true per pin).

- [ ] **Step 1: RED** —
  ```cpp
  void reasoning_config_reaches_engine_launch_and_env();   // per pin: which go to args, which to env — cite each line
  void fit_max_tokens_false_returns_400_like_the_pin();    // over-context max_tokens + fit_max_tokens=false → §7.2 400 (llama.cpp behaviour); default true keeps the SP3 clamp (B-decision unchanged)
  ```
- [ ] **Step 2:** implement; each parity row for the six reasoning keys points at the wired destination.
- [ ] **Step 3:** GREEN + gates; commit `reasoning: effort/budget/recovery typed; fit_max_tokens honours the pin's two behaviours`.

---

## Task 6: Lifecycle — `lazy_load`, `idle_unload_s`, `mtp`

**Files:** Modify `frontend_config` (`engine.lazy_load`, `engine.idle_unload_s`, `engine.mtp`), `engine_supervisor.cpp` (idle timer + respawn hook), `chat_execution.cpp`/`chat_route.cpp` (wake-on-request), `engine_arguments.cpp` (`mtp` toggles the engine's MTP flag per the pin); tests `engine_supervisor_test.cpp`, `api_server_test.cpp`.

**Interfaces:**
- Produces: `EngineSupervisor::note_request_started()/note_request_finished()` (no-ops while idle disabled), `idle_unload_deadline()`; respawn uses the existing restart policy with the *same* composed args (no config reload).

- [ ] **Step 1: RED** —
  ```cpp
  void idle_unload_tears_down_then_respawns_on_next_request();  // fake engine exits after the supervisor's shutdown; next POST /v1/chat answers after READY again
  void idle_unload_never_fires_during_a_generation();           // Review Focus 3
  void lazy_load_starts_without_load_flag();                    // composed args carry the pin's lazy flag (cite server.py line); first GEN triggers load
  void mtp_false_drops_the_mtp_arg();                           // default true keeps today's launch
  ```
- [ ] **Step 2:** implement the timer in the supervisor's wait loop (20 ms slices like the pool; single clock source), guarded by the running-generation flag; idle=0 disables (pin default).
- [ ] **Step 3:** GREEN + gates; TSan spot on the new timer (host toolchain); commit `lifecycle: lazy_load, idle_unload_s, mtp parity with the pin`.

---

## Task 7: VRAM / GPU / elastic family (args + typed env rows)

**Files:** Modify `frontend_config` (`engine.vram_reserve_mib`, `engine.min_free_vram_mib`, `engine.vram_elastic`, `engine.vram_segment_mib`, `engine.elastic`, `engine.elastic_idle_s`, `engine.elastic_mib`, `engine.gpu_order`, `engine.gpu_policy`, `engine.layer_split`, `engine.split_skip_if_fits`, `engine.hip_ordinal`, `engine.uma`), `engine_arguments.cpp`, `engine_environment.cpp` (new strata `FamilyEnvironmentRows` entries); tests `engine_arguments_test.cpp`, env-echo fake.

- [ ] **Step 1: RED** —
  ```cpp
  void vram_reserve_mib_becomes_the_pinned_flag();   // --vram-reserve-mib (upstream/serve/runconfig.py:42)
  void gpu_and_elastic_rows_reach_the_engine_env();  // env-echo fake lists exactly the named, typed names; unknown never sent
  void bounds_are_the_pins_at_each_edge();           // MiB caps: 0 and the row's max accepted; max+1 rejected
  ```
- [ ] **Step 2:** implement rows + arg/env wiring; every key cites its pin line in the parity table (update destinations).
- [ ] **Step 3:** GREEN + gates; commit `engine: vram/gpu/elastic family typed to args and child env`.

---

## Task 8: Timeouts, queues, and request-shape keys

**Files:** Modify `frontend_config` (`engine.silence_s`, `engine.queue_timeout_s`, `engine.timeout_s`, `engine.max_queue`, `engine.max_n`, `engine.max_tokens`, `engine.min_tokens`, `engine.answer_room`, `engine.repeat_stop_tokens`, `model.aliases`), stall watchdog in `engine_protocol_client.cpp` / supervisor, `generation_gate.cpp` (max_queue default source), `chat_route`/model-matching site for aliases, GEN keys for `repeat_stop_tokens`; tests as above.

- [ ] **Step 1: RED** —
  ```cpp
  void engine_silence_s_sets_the_stall_watchdog();     // fake engine goes quiet past the configured silence → the SP3 stall path fires at the new bound
  void queue_and_answer_room_limits_are_enforced();    // max_queue wired to the gate's queued cap (0 = serial allowed, SP3 T16 fix stays); answer_room honoured at admission like max_tokens room rules
  void repeat_stop_tokens_reach_gen();                 // env-echo/GEN-shape fake sees them
  void aliases_answer_like_the_primary_model();        // POST with a configured alias → 200; a name in neither model_name nor aliases → the SP3 model error; aliases never match empty/partial (§4 rules)
  ```
- [ ] **Step 2:** implement; defaults copied from the pin (cite `engine_silence_s` default 300 at `serve/runconfig.py:39`).
- [ ] **Step 3:** GREEN + gates; commit `engine: silence/queue/room/stop-token keys at the pin`.

---

## Task 9: Session and context family

**Files:** Modify `frontend_config` (`session.slot_save_path`, `session.expert_profile_save`, `session.expert_profile_save_every_s`, `session.tool_call_recovery`, `session.env_to_user`, `session.codex_compaction_cache`, `session.codex_thread_titles`, `session.keep_files`), path-rule helpers (existing canonical/path guards), `engine_arguments`/`engine_environment` where the pin routes them; tests.

- [ ] **Step 1: RED** —
  ```cpp
  void slot_save_path_is_a_bounded_private_path();      // canonical path rules; world-writable or escaping path → invalid_value; reaches env/arg per pin citation
  void expert_profile_flags_reach_launch();             // --expert-profile-save/-every per the engine arg form the pin composes
  void codex_and_tool_call_rows_are_dispositioned();    // rows for routes we do not serve: rejected-with-rule (named error at load) or mapped-away — matches the parity table exactly (re-read rows)
  ```
- [ ] **Step 2:** implement to match the Task-2 rows (if implementation evidence contradicts a row's disposition, update the row and the citation in the same commit — table is the source of truth, tests follow).
- [ ] **Step 3:** GREEN + gates; commit `session: slots, expert profiles, recovery keys typed with path rules`.

---

## Task 10: Replicas — the accepted narrowing (C-4)

**Files:** Modify `frontend_config` (`replicas.parallel`), `engine_arguments.cpp` (`--parallel N` per pin); README + limits doc (narrowing statement); tests.

- [ ] **Step 1: RED** —
  ```cpp
  void parallel_reaches_the_engine_within_one_child();   // replicas.parallel 1..8 → engine arg; bounds from the pin (cite)
  void multi_child_replicas_reject_with_the_ruling();    // replicas.count (if the pin exposes it as config) > 1 → §7.2 error naming the narrowing; the row is rejected-with-rule and the message cites README
  ```
- [ ] **Step 2:** implement; README §7.2/limits sentence: "N-child replica scheduling is not implemented in SP5; `--parallel` within one engine is."
- [ ] **Step 3:** GREEN + gates; commit `replicas: engine-internal parallel typed; N-child narrowing documented and rejected by name`.

---

## Task 11: Vision artifacts — manifest role, supervised child, launch proof

**Files:** Modify `acquire_manifest.hpp/.cpp` (`FileRole::mmproj`, manifest `files[]` role + `ManifestLaunch.mmproj`), a new supervised role wiring in `engine_supervisor` composition (reuse SP2 machinery; distinct exit-code space), config `vision.enabled`, `vision.max_side`, `vision.max_n`, `vision.need_mib`, `vision.no_flash_attn`, `vision.resident`, `vision.threads`; tests `acquire_manifest_test.cpp`, `engine_supervisor_test.cpp` (fake vision child binary `fake_vision_child` in tests/support).

- [ ] **Step 1: RED** —
  ```cpp
  void manifest_accepts_mmproj_role_and_rejects_duplicates();   // role name "mmproj", one entry max
  void vision_child_launches_with_pinned_args_when_enabled();   // fake vision child echoes argv; args match the pin's vision block form (cite serve/server.py:3378-3418)
  void vision_child_respects_resource_limits();                 // PROC/DOS rows: kill on oversize log; restart once then circuit (SP2 policy)
  ```
- [ ] **Step 2:** implement; the vision child is a supervised role like the engine (own lifecycle, same confinement).
- [ ] **Step 3:** GREEN + gates; commit `vision: mmproj manifest role and supervised vision child launch at the pin`.

---

## Task 12: Chat `image_url` content path (C-9 carve-out)

**Files:** Modify `chat_schema.cpp` (content array with `{type:"text"}` / `{type:"image_url"}` parts accepted **only** when the active model's manifest has `mmproj`; else existing `unsupported_parameter content` stands), new `image_payload.{hpp,cpp}` (base64 length pre-check, decoded-bytes cap, private-scratch write 0600), `chat_route`/GEN or ENC exchange with the vision child (fake answers `ENC <path>` with a canned embedding line), tests `chat_schema_test.cpp`, `api_server_test.cpp`.

- [ ] **Step 1: RED** —
  ```cpp
  void image_url_parts_accepted_only_for_vision_models();          // manifest with mmproj: 200 path; without: today's 400 unsupported_parameter content
  void oversized_or_bad_base64_rejected_before_decode();           // length cap first: fixed §7.2 400 naming "image_url"; no file written
  void enc_round_trip_reaches_the_prompt();                        // fake vision child embedding appears in the GEN prompt stream (fake echoes)
  ```
- [ ] **Step 2:** implement caps from the row table (cite the pin's vision caps; added security bounds stated on the row).
- [ ] **Step 3:** GREEN + gates + TSan spot; commit `vision: image_url path with pre-decode caps and ENC round trip`.

---

## Task 13: `strata serve` — foreground serve

**Files:** Modify `strata_cli.cpp` verb table, new `verb_serve` in `strata_cli*.cpp` reusing `frontend_init` + the supervised path minus daemonization/`server.json`/lock; tests `server_cli_test.cpp`.

- [ ] **Step 1: RED** —
  ```cpp
  void serve_runs_foreground_and_shuts_clean_on_sigterm();  // same config validation as run; no server.json written; no lock; SIGTERM → exit 0 in ≤5 s (SP3 I1 shutdown path)
  void serve_prints_ready_then_serves();                    // loopback POST answers; ready line fixed literal
  ```
- [ ] **Step 2:** implement; `--help` text updated (fixed literals).
- [ ] **Step 3:** GREEN + gates; commit `cli: strata serve (foreground) lands`.

---

## Task 14: `strata version` and `strata key show|rotate`

**Files:** `strata_cli.cpp` (`verb_version`, `verb_key` reusing SP1 token store); tests.

- [ ] **Step 1: RED** —
  ```cpp
  void version_names_frontend_and_engine_versions();   // frontend version + upstream.version (tag governs; engine self-version labelled informational) — matches AGENT_HANDOFF bump rule
  void key_show_requires_local_token_and_never_logs(); // show prints the stored token only when the store is readable by this uid; rotate regenerates + prints once; nothing lands in any log file
  ```
- [ ] **Step 2:** implement with SP1 store rules (0600, CSPRNG, constant-time unchanged).
- [ ] **Step 3:** GREEN + gates; commit `cli: strata version and strata key show|rotate land`.

---

## Task 15: Docs truth pass, whole-feature battery, handoff

**Files:** Modify `secure_frontend/README.md` (config reference auto-renders new keys via `render_config_schema_markdown()`; verbs; narrowing sentences), `docs/AGENT_HANDOFF.md`, `docs/security/threat-model.md` (new rows: env-knob warning, image path, lifecycle timer), `docs/PROVENANCE.md` (touch table if any).

- [ ] **Step 1:** README regenerated sections; grep pass: every `rejected` row appears in README §"excluded keys"; every typed key appears in the config table (a small Python test in the existing pytest suite asserts the rendered markdown contains each key from `parity_keys_upstream.hpp` with the right section — no vacuous pass: it counts ≥ 80 rows).
- [ ] **Step 2:** full battery on the final tree: linux-gcc ctest (all), release preset, cross, pytest, TSan filtered suites (host clang), root build + ctest.
- [ ] **Step 3:** ledger close-out, FINAL-REPORT.md in the workspace, handoff rows for Tasks 1–15; commit `docs: SP5 M1 truth pass and handoff`.

---

## Sequencing notes

Tasks 2→3 are the spine (table before behaviour). 4–10 are independent families (parallelizable). 11→12 strictly ordered. 13–14 independent. 15 last. M2 (Maya table, dialect layer, acceptance kit) is a separate plan written after M1 merges — it starts by vendoring the pinned Maya source per D-10.
