# SP5 M2 — Maya parity table, engine dialect, acceptance kit — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the `glm53` profile speaks Project Maya `v1.1.8`'s real option surface (typed, cited, allowlisted) and its wire dialect, and one acceptance kit exists that proves both profiles' parity once, on the owner's NVIDIA box.

**Architecture:** extend the M1 machinery — the dispositioned parity table grows a second reference (Maya, vendored read-only per D-10, inventory generated from the vendored tree); typed values flow through the same `config_schema` / `engine_arguments` / `engine_environment` seams (the `glm` family rows exist but are "not reachable from config; SP6 fills" — M2 fills them from Maya, not from A10, per D-7); the protocol client grows a third `EngineProtocolFlavor` for the Maya framing (explicit contract, never auto-detected); the kit is a corpus mode on `strata-api-probe` plus a procedure doc.

**Tech Stack:** C++20, stdlib + OS only; CMake/Ninja from `.venv-dev/`; presets `linux-gcc`, `linux-gcc-release`, `windows-llvm-mingw-cross`; pytest for tooling; `git submodule` for the Maya reference only.

**Spec:** `docs/superpowers/specs/2026-10-21-sp5-parity-typed-options-design.md` (D-1…D-10, C-1…C-9). M1's plan (`2026-10-21-sp5-m1-strata-parity-cli.md`) defines the table/verbs machinery this plan extends.

## Global Constraints

- C++20; `-Werror` everywhere. Per-task gates, synchronous in-turn: linux-gcc build (0 warnings) + `ctest --preset linux-gcc` serial + **linux-gcc-release build** + `windows-llvm-mingw-cross` build (0 warnings; the Maya submodule is never built by any preset — it must not even configure) + `pytest -q`.
- Baseline at Task 1 (main `9ab45319`): ctest **803 run / 100%** (+4 env skips, 807 registered); release preset 10/10; pytest **26**; cross 0 warnings.
- `upstream/` stays untouched. The Maya reference (`engines/maya-ref/` submodule, tag `v1.1.8`, commit `5c970a863ce2fdd87b6e23ffb4822f6d76522b1c`) is **read-only**: never compiled, never linked, excluded from packaging and `secure_distribution_check.py`, cited by `file:line` only (D-10, §9).
- Kind/bound/default copy from the vendored Maya source with citations (C-1). Config leaf names follow **Maya's own names** (C-3: `thinking_budget`, not `reasoning_budget_tokens`).
- D-7 supersedes A10 for scope: every documented `STRATA_GLM_*` knob gets a typed row; the SP6 §4.9 forced defaults (slots off, loop guards, split) stay forced *defaults* — the table may set values, nothing unknown is ever sent. `STRATA_GLM_DFLASH` is a **rejected** row on the A7 licence ground (C-5), cited as such.
- The dialect is an explicit wire contract selected by engine profile — never sniffed from untrusted output (existing comment at `engine_protocol_client.hpp:108-115` stays true).
- No CI, no wine, no pushes by the lane; commit trailers as always (`shift-f10`, literal `Co-Authored-By: <real model>`, no `Claude-Session:`). Never weaken a test; the parked flake stays untouched.
- Ledger: `.superpowers/sdd/2026-10-21-sp5-m2-maya-parity-dialect-kit/progress.md`.

## Review Focus

1. **A Maya knob silently absent from the table** — the inventory meta-test regenerates from the vendored tree; `SETTINGS.md` family rows are part of the inventory (Task 2), so a renamed knob (`thinking_budget`) silently missing is a build failure.
2. **An unknown engine-env name reaching the GLM child** — allowlist stays closed; typed rows send *named, bounded* values only; the env-echo fake asserts the exact set.
3. **A dialect record parsed by the wrong profile** — flavor is set at launch from the profile; unit tests pin each record kind per flavor (NOTE on strata flavor = fail-closed as today).
4. **Kit attesting to nothing** — every corpus case carries its equivalence class and both sides' full command lines; the results file schema is fixed and its absence is a FAIL, never a skip.
5. **GLM config accepted on the wrong profile** — `glm.*` keys on a strata-profile config error by name (and vice versa where the pin differs).

---

## Task 1: Vendor the pinned Maya reference

**Files:** `.gitmodules` (new submodule `engines/maya-ref/`), `docs/PROVENANCE.md` ("Project Maya" section: pin sha, purpose, read-only rule), `scripts/secure_distribution_check.py` + packaging lists (exclude `engines/maya-ref/`), pytest config (ignore `engines/maya-ref/` collection — its `conftest`-bearing tree must not pollute `pytest -q`).

**Interfaces:**
- Produces: the vendored tree at a fixed sha; path constant in docs: `engines/maya-ref/serve/server.py:NNNN` citations become checkable locally.

- [ ] **Step 1:** `git submodule add https://github.com/mw00/project-maya engines/maya-ref && git -C engines/maya-ref checkout 5c970a863ce2fdd87b6e23ffb4822f6d76522b1c` (owner-authorized D-10); verify `git -C engines/maya-ref status --porcelain` empty and `git submodule status` shows the sha with tag `v1.1.8`.
- [ ] **Step 2: RED** — extend the distribution-check pytest: a staged tree containing `engines/maya-ref/` must be *skipped* by packaging, and a repo grep test asserts no CMake file references `engines/maya-ref` (never built). Run → FAIL (no rule yet).
- [ ] **Step 3:** implement exclusions; full gates — confirm no preset configures the submodule (cross + release builds unchanged); commit `provenance: Project Maya v1.1.8 vendored read-only (D-10)`.

---

## Task 2: Maya key inventory + the GLM parity table + meta-tests

**Files:** extend `parity_table.{hpp,cpp}`, `tests/parity_table_test.cpp`; new generated `secure_frontend/tests/parity_keys_maya.hpp` (same pattern as M1's: header comment records generation commands + vendored sha).

**Interfaces:**
- Produces:
  ```cpp
  std::span<const std::string_view> maya_config_keys() noexcept;   // serve/*.py cfg.get/maya.py + shared-settings keys
  std::span<const std::string_view> maya_env_knobs() noexcept;     // STRATA_GLM_* family from docs/SETTINGS.md:26-101 + calibration writes + server env
  const ParityRow* maya_parity_row(std::string_view) noexcept;
  ```

- [ ] **Step 1:** generate the inventory from the vendored tree (commands written in the header): `grep -rhoE 'cfg\.get\("[a-z_0-9]+"' engines/maya-ref/serve/*.py engines/maya-ref/maya.py | sort -u`; SETTINGS.md family names by parsing its table rows (a short shell pipeline, recorded verbatim); shared-settings keys from `clean_shared_defaults` (`serve/server.py:3223-3260`).
- [ ] **Step 2: RED** — `every_maya_key_has_exactly_one_disposition()`, `every_maya_env_knob_is_dispositioned()`, `dflash_is_rejected_on_licence()`, `updater_and_installer_rows_are_rejected()` (`MAYA_UPDATE_CHECK`, `MAYA_RESTART_ON_UPDATE`, `installer`, `serve/update.py` ground: PRIV-01). FAIL first.
- [ ] **Step 3:** assign all dispositions now: server keys typed per Maya citations; ~100 env knobs typed (D-7) except `STRATA_GLM_DFLASH` (rejected, A7 licence) and the updater/installer family (rejected, PRIV-01); engine-flag launcher names mapped-away (manifest composition). Meta-tests GREEN; full gates; commit `parity: Maya v1.1.8 inventory and dispositioned GLM table`.

---

## Task 3: `glm.*` config surface (Maya names, profile-gated)

**Files:** `frontend_config` (new `glm.*` ConfigKeys: `glm.thinking_budget` default 32768, `glm.reasoning_effort` enum none|low|high|max, `glm.fit_max_tokens`, `glm.experimental_speed_projection`, plus the lifecycle/gpu/layer_split/tokenizer/model-name keys Task 2 typed); tests `typed_config_test.cpp`.

- [ ] **Step 1: RED** —
  ```cpp
  void glm_keys_parse_with_mayas_kinds_and_defaults();   // bounds verbatim from vendored citations (C-1)
  void glm_keys_rejected_on_strata_profile_by_name();    // Review Focus 5
  void thinking_budget_default_is_mayas_32768();         // C-3: Maya's name and value, not upstream's
  ```
- [ ] **Step 2:** implement (schema extension already supports enums/lists from M1); rows' destinations updated to `config_only`/`engine_env`/`engine_arg` as wired in Tasks 4–7.
- [ ] **Step 3:** GREEN + gates; commit `config: glm.* typed surface at Maya's names`.

---

## Task 4: GLM engine-env rows — the closed typed allowlist

**Files:** `engine_environment.cpp` (replace "glm currently renders strata rows" with generated glm rows), `frontend_config` → env projection; tests: env-echo fake per profile.

- [ ] **Step 1: RED** —
  ```cpp
  void glm_child_env_is_exactly_the_named_typed_set();   // configure 6 knobs → env holds exactly those + forced defaults; never any other STRATA_*/MAYA_* name
  void forced_defaults_stay_forced_unless_the_table_moves_them();  // slots off etc. per SP6 §4.9; a typed row may set a value; unknown names never appear
  void dflash_never_reaches_the_child();                 // rejected row: config accepts nothing for it; child env never carries it (C-5)
  ```
- [ ] **Step 2:** implement rows generated from the parity table (one source of truth: the table drives the env-name list at compile time or via tested constant array).
- [ ] **Step 3:** GREEN + gates; commit `engine: GLM child env closed typed allowlist from the parity table`.

---

## Task 5: The Maya wire dialect in the protocol client

**Files:** `engine_protocol_client.{hpp,cpp}` (new flavor value `EngineProtocolFlavor::maya_118`), record handlers for `NOTE`, Maya `STAT` cadence, 5-field `PP` (already tolerant — pin the exact Maya grammar from `engines/maya-ref/serve/server.py` citations); tests `engine_protocol_client_test.cpp`.

**Interfaces:**
- Consumes: `EngineProtocolFlavor`, `next_event`, `ProgressEvent` (M1/SP3).
- Produces: flavor selected by engine profile at client construction (existing wiring site in chat execution/supervisor composition); `NOTE <text>` → consumed as `ProgressEvent` (never client-visible), bounded like PP/RESUME; unknown-to-flavor records still fail closed.

- [ ] **Step 1: RED** —
  ```cpp
  void maya_flavor_consumes_note_and_progress();        // canned NOTE/STAT-cadence/5-field PP lines: no protocol error, invisible to output
  void strata_flavor_still_fails_closed_on_note();      // cross-flavor strictness unchanged
  void maya_pp_grammar_is_bounded();                    // 5-field form cited to the vendored line; malformed → unexpected_line
  ```
- [ ] **Step 2:** implement; update the flavor doc-comment (explicit contract, three values).
- [ ] **Step 3:** GREEN + gates; commit `engine: maya_118 wire dialect (NOTE, STAT cadence, 5-field PP)`.

---

## Task 6: Fake engine Maya mode + end-to-end on the GLM profile

**Files:** `tests/supervision_fake_engine_main.cpp` (new `--dialect maya`), `tests/chat_generation_plan_test.cpp` / `api_server_test.cpp` e2e through the glm profile launch shape.

- [ ] **Step 1: RED** —
  ```cpp
  void glm_profile_generation_completes_over_maya_dialect();   // stream + non-stream: same TokenEvent/DoneEvent stream as the strata dialect for identical token payloads
  void maya_dialect_cancellation_round_trips();                // STOP path unchanged under the dialect
  ```
- [ ] **Step 2:** implement the fake mode (emits exactly the vendored-grammar records per profile); wire a glm-profile test fixture (fake glm engine argv from Task 7's composition — sequence note: may land after Task 7; the e2e test is enabled by whichever lands second).
- [ ] **Step 3:** GREEN + gates + TSan spot on the dialect parsing; commit `test: fake engine Maya dialect; GLM generation end to end`.

---

## Task 7: GLM launch composition — manifest replaces Maya's launcher

**Files:** `engine_arguments.cpp` (`lighttransport_glm53` profile: compose `--glm-pack <manifest pack>`, `--max-context` (typed, pin default 32768), `--prefill auto`, `--kv-streaming on`, `--parallel` from `replicas.parallel`, `--expert-profile` per manifest) — these are the Task-2 mapped-away launcher-flag rows' destinations; tests argv assertions vs `engines/maya-ref/maya.py:1816-1823,1865` citations.

- [ ] **Step 1: RED** — `void glm_launch_argv_matches_mayas_launcher_form();` (ordered argv equality against the citations; nothing from §4 can appear: no `--serve`, no free `--flag` owned forms) and `void glm_context_and_parallel_bounds_enforced();`.
- [ ] **Step 2:** implement; the old pass-through `--engine-arg` path for glm53 is retired: **require** the table (update SP1-era tests that asserted pass-through with named-error assertions; the parity table's mapped-away rows cite this retirement).
- [ ] **Step 3:** GREEN + gates; commit `engine: GLM launch composed from manifest + table (pass-through retired)`.

---

## Task 8: GLM vision argv + env rows (behaviour deferred to the kit)

**Files:** glm vision rows' destinations (no_flash_attn/resident → argv per citations; VLEND-family env names already typed in Task 4); `engine_arguments` glm vision argv; tests.

- [ ] **Step 1: RED** — `void glm_vision_flags_reach_both_children();` (engine env carries the typed VLEND family only when vision configured; encoder argv per form; resident policy per pin default).
- [ ] **Step 2:** implement.
- [ ] **Step 3:** GREEN + gates; commit `vision: GLM-side vision argv and env rows`.

---

## Task 9: The acceptance kit — corpus mode + procedure

**Files:** `secure_frontend/tools/api_probe` (corpus mode: `--corpus <file>`; per-case class `text|shape|divergent-by-decision`; writes a fixed results schema: `case,class,RESULT,detail` + both servers' full command lines + engine versions at top), `docs/validation/sp5-parity-kit.md` (purpose; owner-box setup: build upstream `v0.2.1.3` engine + reference server, build Maya engine from `engines/maya-ref/` per the SP6-designed recipe `docs/superpowers/specs/2026-10-14-sp6-glm-engine-design.md §5`; run reference side `serve` and our `strata serve` with identical composed flags; record both command lines; run probe corpus both sides; hash results into PROVENANCE SP5 entry), `tools/secure/test_sp5_kit.py` (fixture corpus + canned responses → exact PASS/FAIL lines + schema), `docs/PROVENANCE.md` placeholder row.

- [ ] **Step 1: RED** — kit pytest: fixture corpus with one `text`, one `shape`, one `divergent-by-decision` case against canned outputs → one results line each, class recorded, missing results file = FAIL not skip.
- [ ] **Step 2:** implement corpus mode + doc; a `--dry-run` mode renders the exact commands for the owner box (no GPU code on the dev host).
- [ ] **Step 3:** GREEN + gates; commit `kit: SP5 parity corpus mode + owner-box procedure`.

---

## Task 10: Docs truth pass, whole-feature battery, handoff

**Files:** README (glm config mirror — extend `tools/secure/test_parity_readme_coverage.py` to the Maya table: no vacuous count), threat model (GLM env allowlist, dialect records, kit attestation), `docs/AGENT_HANDOFF.md` SP5 M2 section, `docs/superpowers/handoff/reports/sp5-m2/FINAL-REPORT.md`, PROVENANCE (kit row, maya section touch note).

- [ ] **Step 1:** README regenerated; mirror test counts both tables (≥121 + Maya rows); grep pass: every rejected Maya row appears in README §excluded.
- [ ] **Step 2:** full battery on the final tree (linux-gcc ctest, release, cross, pytest, TSan filtered suites, root build + ctest, distribution check).
- [ ] **Step 3:** ledger close-out + FINAL-REPORT; commit `docs: SP5 M2 truth pass and handoff`.

---

## Sequencing

1 → 2 spine. 3 → 4 ordered; 5 → 6 ordered; 7 before 6's glm fixture (both before 8); 9 after 6+7 (kit exercises the dialect and composed launch); 10 last. The **GPU acceptance run itself is not a lane task** — the kit lands ready; the owner runs it; results + hash go into PROVENANCE in a follow-up controller commit like the SP3 kit.
