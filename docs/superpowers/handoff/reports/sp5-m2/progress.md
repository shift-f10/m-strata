# SP5 M2 (Maya parity table, dialect, acceptance kit) — lane ledger

Branch `sp5-maya-parity` at base `749e5395` (main). Upstream pin `cbfd5b92` (v0.2.1.3).
Plan: `docs/superpowers/plans/2026-10-21-sp5-m2-maya-parity-dialect-kit.md` (Tasks 1–10).
Spec: `docs/superpowers/specs/2026-10-21-sp5-parity-typed-options-design.md` (D-1…D-10, C-1…C-9).
M1 context: `docs/superpowers/handoff/reports/sp5-m1/` + plan of record.

## Status

- [x] Task 1 (vendor pinned Maya reference) — COMPLETE at commit `0b096f5c`.
  Submodule `5c970a86… engines/maya-ref (v1.1.8)` recorded, porcelain-clean, no
  nested fetch. Distribution check + pytest.ini + CMake-guard tests added
  (RED at :73/:98 recorded below). Gates: linux-gcc 0 warn, ctest 803/803 100%
  (4 env skips, unchanged registered), release build 0 warn, cross 0 warn,
  pytest 26→29; no CMakeCache references maya-ref.
- [x] Task 2 (Maya inventory + GLM table + meta-tests) — COMPLETE at `3ba36193`.
  Table 178 rows (138 typed / 30 rejected / 10 mapped), inventory 37+119,
  4 new meta-test cases (RED recorded). Gates: ctest 807/807 100% (4 skips,
  registered 811), release 0 warn, cross 0 warn, pytest 29.
  Rulings R-T2-a…R-T2-e recorded above.
  Inventories generated (commands recorded in tests/parity_keys_maya.hpp header):
  37 config keys (31 cfg.get grep + 6 shared-settings names), 119 env knobs
  (SETTINGS.md 26-101:103, whole file:105, serve grep:12, server env:9,
  WATCHDOG_S, 2 MAYA_*). Launcher/build internals excluded with Ruling T2-b
  (recorded in the header + below).
  RED (linux-gcc build 0 warnings, empty GLM table, four new cases):
  `every_maya_key_has_exactly_one_disposition` FAILED —
  "FAILED: Maya config key 'api_key' must have exactly one parity row" (first line);
  `every_maya_env_knob_is_dispositioned` FAILED —
  "FAILED: Maya env knob 'MAYA_RESTART_ON_UPDATE' must have exactly one parity row";
  `dflash_is_rejected_on_licence` FAILED — "FAILED: maya_parity_row('STRATA_GLM_DFLASH') must resolve";
  `updater_and_installer_rows_are_rejected` FAILED — "FAILED: maya_parity_row('MAYA_UPDATE_CHECK') must resolve".
  (One earlier build error — -Wcomment from a backslash-continued comment in the
  generated header — fixed; recorded commands reflowed to one line, still verbatim.)
  Submodule added: `5c970a863ce2fdd87b6e23ffb4822f6d76522b1c engines/maya-ref (v1.1.8)`
  porcelain-clean, no +/- prefix, .gitmodules has the entry; no nested submodule
  fetch (project-maya has no .gitmodules).
  RED (pytest, before implementation):
  `test_staged_maya_ref_tree_is_rejected` FAILED at test_secure_distribution_check.py:73
  — `AssertionError: ('engines/maya-ref/README.md', ['engines/maya-ref/serve: quarantined…'])`;
  `test_pytest_never_collects_the_engines_tree` FAILED at :98 —
  `pytest.ini must pin norecursedirs while engines/ is vendored`.
  (`test_no_cmake_file_references_maya_ref` passed — a guard, not new behaviour.)

- [x] Task 3 (`glm.*` config surface) — COMPLETE. Brief:
  task-03-glm-config-surface.md.
  RED (linux-gcc build 0 warnings; three cases registered, schema row absent):
  `glm_keys_parse_with_mayas_kinds_and_defaults` FAILED —
  "FAILED: the schema row exists: glm.thinking_budget";
  `glm_keys_rejected_on_strata_profile_by_name` FAILED —
  "FAILED: glm.thinking_budget is refused by name on the qwen profile: glm: unknown key;";
  `thinking_budget_default_is_mayas_32768` FAILED —
  "FAILED: Maya's thinking_budget default is 32768 (vendored serve/server.py:3440; C-3 keeps Maya's name)".
  GREEN: 8 schema rows (kSchema 109→117), GlmSettings/GlmReasoningEffort in
  frontend_config.hpp, validate() parsing + qwen-profile gate, describe() lines,
  README config-keys table regenerated (117 rows). auth04's closed boolean-switch
  set extended with the two GLM booleans (R-T3-d). Gates: ctest 810/810 100%
  (+3 cases, registered 814, 4 env skips), release build 0 warn, cross build
  0 warn, pytest 29. Tooling incident recorded as R-T3-e (no test, product or
  history damage; main workspace verified clean afterwards.
- [x] Task 4 (GLM child-env closed typed allowlist) — COMPLETE.
  RED (linux-gcc build clean, five cases on the current code):
  `glm_child_env_is_exactly_the_named_typed_set` FAILED — "forced default:
  STRATA_GLM_SLOTS" (+ LOOP/SPLIT/WATCHDOG, "the glm family renders the CUDA
  selectors", and every table name "…is on the allowlist: STRATA_GLM_AHEAD…");
  `forced_defaults_stay_forced_unless_the_table_moves_them` FAILED — the
  watchdog-IO row present + STRATA_WATCHDOG_S not forced "0";
  `sp6_env_forcible_names_are_the_ruled_set` FAILED — "the forcible list is
  exactly the ruled set" (7 vs 100) + per-name refusals;
  `sp6_env_strata_family_renders_unchanged` FAILED — "the glm hook now forces
  and suppresses its rows (D-7)";
  `dflash_never_reaches_the_child` — guard, passes before and after (like
  Task 1's CMake guard; recorded as such, not a TDD miss).
  GREEN: kFamilyForcibleEnvironment 7→100 (the SP6 §4.9 ruled set ∪ the
  table's 98 typed engine_env names; sorted; pinned table-driven by
  sp6_env_forcible_names_are_the_ruled_set); glm hook renders the five §4.9
  safety forced-defaults + suppresses HIPBLASLT_TUNING/WATCHDOG_IO_S +
  CUDA_DEVICE_ORDER=PCI_BUS_ID/CUDA_VISIBLE_DEVICES from glm_gpus (absent:
  "0"); EngineEnvironment gains glm_gpus/glm_knobs; a glm_knobs row replaces
  the forced default of the same name through the same merge gate;
  frontend_config gains glm_environment_knobs() (layer_split→SPLIT today).
  Gates: ctest 814/814 100% (+4 cases: 3 supervisor + 1 projection;
  registered 818; 4 env skips unchanged), release 0 warn, cross 0 warn,
  pytest 29.
- [x] Task 5 (maya_118 wire dialect) — COMPLETE.
  RED (enum value stubbed so RED lands as failing cases; 0 warnings):
  `maya_flavor_consumes_note_and_progress` FAILED — "NOTE/STAT/5-field PP are
  consumed silently: exactly the tokens survive" + 4× "a maya progress record
  surfaced as an error"; `maya_pp_grammar_is_bounded` FAILED — "the vendored
  PP forms are consumed: PP 512 4096 120 33.3 0" (+ 2 more good forms);
  `strata_flavor_still_fails_closed_on_note` passed at RED — a cross-flavor
  guard (the strictness it pins already existed; recorded as a guard).
  GREEN: flavor-aware PP acceptance (strata unchanged; lighttransport
  unchanged incl. the !=4/second!=0/read<=total strictness; maya_118 = 3-6
  fields, decimal counters, optional non-negative ms/tok-s, optional decimal
  reuse — engine prints generate.cpp:5546/:6111, pump server.py:771-783);
  NOTE consumed when non-empty (server.py:787 etc.), STAT consumed with the
  key=value shape check (:636-649); flavor doc comment now names three
  explicit contracts. Fixture fix during GREEN: my next_event sub-fixture
  lacked its READY line (assertions untouched — R-T5-a). Gates: ctest
  817/817 100% (+3 cases, registered 821, 4 env skips), release 0 warn,
  cross 0 warn, pytest 29. Flavor selection sites (server_bootstrap:208,
  subsystem_limits:53) still pick lighttransport_glm53 for glm53 — Task 7
  moves the GLM composition to maya_118 (R-T5-b).
- [x] Task 7 (GLM launch composition, pass-through retired) — COMPLETE (ran
  before Task 6 per the sequencing note).
  RED (Glm53LaunchArgs struct + defaulted param stubbed so RED lands as
  failing cases; 0 warnings): 7/7 new cases failed —
  `glm_launch_argv_matches_mayas_launcher_form` ("the argv equals Maya's
  launcher form" — pass-through returned empty argv), `glm_context_and_parallel_bounds_enforced`
  (bounds accepted), `glm_rejects_every_engine_argument` (pass-through still
  builds), `managed_glm_rows_refused_for_qwen_manifest` (compose succeeded),
  `glm_max_context_typed_and_glm53_args_refused` (schema row missing),
  `supervisor_flavor_is_the_maya_dialect_for_glm53` (std::get wrong-index —
  bare glm53 config still missing_required at RED), and shell
  `strata_secure_server_glm_rejects_engine_args` (regex not found — old
  behaviour accepted the args). A CMakeLists comment typo (// vs #) made the
  first configure fail; fixed before the recorded RED ran.
  GREEN: glm.max_context schema row (integer 1-1048576, default 32768 =
  Maya's DEFAULT_CONTEXT, maya.py:2524; kSchema 117→118) + GlmSettings
  max_context/rows_present + describe line + README row (readme_config_table
  pin green); validate_engine_user_arguments glm branch refuses EVERY
  engine.args row (not_accepted; NUL still first) and the "at least one
  --engine-arg" rule is deleted; build_engine_arguments composes
  --glm-pack <parent of the selected artifact> --max-context N --prefill auto
  --kv-streaming on [+ --parallel N for 2..8] (maya.py:1816-1818, :1875,
  :2742), bounds fail closed out_of_range, invalid artifact path
  invalid_value, non-empty gpus stays unsupported_mode; expert_launch passes
  Glm53LaunchArgs{glm.max_context, replicas.parallel}; compose_engine_launch
  managed refuses glm.* rows when the manifest profile is not glm53
  (R-T3-a, rows_present); flavor sites (server_bootstrap:208,
  subsystem_limits:53) now pick maya_118 for glm53 (R-T5-b closed).
  Plan-named SP1-era updates: old pass-through case and shell case retired
  with named-error replacements (R-T7-f). Gates: ctest 822/822 100% (+5
  cases, registered 826, 4 env skips), release build 0 warn, cross build 0
  warn, pytest 29.
- [x] Task 6 (fake engine Maya mode + GLM e2e) — COMPLETE (ran after Task 7
  per the sequencing note: "enabled by whichever lands second").
  Landing order (R-T6-a): the two plan-named cases
  (glm_profile_generation_completes_over_maya_dialect,
  maya_dialect_cancellation_round_trips, chat_execution_test) pin the GLM
  profile over the maya_118 dialect that Task 5 had already made legal —
  green on first run, they stand as guards (recorded honestly, R-T5-a
  precedent). The RED driver is the process case
  fake_maya_dialect_round_trips_only_under_the_maya_dialect
  (engine_supervisor_test, PROCESS group): at RED it failed "the unknown
  record kills the generation" + "the maya dialect serves the generation" —
  the fake had no dialect mode. GREEN (R-T6-b): the supervision fake grows
  --dialect=maya — NOTE right after GEN (generate.cpp:1245), 5-field PP per
  prefill chunk (:5546; the mode implies progress_records), STAT cadence
  every 3 tokens (:1799); T/DONE payloads unchanged; proof adversarial —
  the strict strata dialect kills the child protocol_failed on those
  records, maya_118 completes and logs no kill, and the fake carries Task
  7's composed GLM argv (--glm-pack/--max-context/--prefill/--kv-streaming).
  Case fixes while developing (my scripts, no assertion weakened): the
  cancellation script first lacked READY's stop advertisement (STOP is
  legal only when READY advertises it; real Maya READY carries stop/pause,
  generate.cpp:2750 — R-T6-c), and LogCapture::contains pins whole log
  lines (the fatal-case pattern). Gates: ctest 825/825 100% (+3 cases,
  registered 829, 4 env skips), release build 0 warn, cross build 0 warn,
  pytest 29. TSan spot (plan Step 3): clang++ -fsanitize=thread scratch
  build — protocol-client suite 0 ThreadSanitizer warnings, both maya chat
  cases and the supervisor fake case clean
  (TSAN_OPTIONS=external_symbolizer_path=<repo>/.venv-dev/bin/llvm-symbolizer).
- [x] Task 8 (GLM vision argv + env rows) — COMPLETE.
  RED: glm_vision_flags_reach_both_children (typed_config_test) failed its
  three GLM-env assertions — LEND projection, rendered presence with a turn,
  fail-closed suppression without one. The argv half (--no-flash-attn) and
  the no-vision half were already true from M1 (recorded as guards). GREEN:
  glm_environment_knobs projects STRATA_GLM_VISION_LEND_MB from
  vision.need_mib only when vision.enabled && !vision.resident (Maya's
  on-demand flow, server.py:3385/:3396); EngineEnvironment.glm_vision_turn
  gates the render — without a turn the row is erased from the knob channel
  and recorded suppressed (fail closed); STRATA_GLM_VISION_LEND_MB joins
  kFamilySuppressibleEnvironment (3→4); the bootstrap wires family=glm +
  glm_gpus + glm_knobs + glm_vision_turn for a composed glm53 launch
  (closes R-T4-c's "Task 7 extends the projection"). Gates: ctest 826/826
  100% (+1 case, registered 830, 4 env skips), release build 0 warn, cross
  build 0 warn, pytest 29.
- [x] Task 9 (acceptance kit: corpus mode + procedure) — COMPLETE.
  RED: tools/secure/test_sp5_kit.py — all 4 tests failed (no --corpus/
  --compare/--dry-run; usage exit 2). GREEN: strata-api-probe corpus mode
  writes/prints the fixed schema `case,class,RESULT,detail` with both
  servers' command lines and the engine versions in the header; classes
  text (determinism pair + sha256)/shape(expect)/divergent-by-decision
  (authorised_by required) — every case lands exactly one line, never a
  skip; --compare diffs the two sides (text sha equality, finish equality,
  recorded-pair) and a missing results file is a FAIL line with exit 1;
  --dry-run renders the exact owner-box commands. Kit doc
  docs/validation/sp5-parity-kit.md + canonical corpus
  docs/validation/sp5-parity-corpus.txt + PROVENANCE SP5 kit placeholder
  row (no parity claim until the owner run). Dev-host smoke: the canonical
  corpus parses (10 result lines, exit 1 vs closed port — expected).
  Gates: ctest 826/826 100% (unchanged; kit proof lives in pytest),
  release build 0 warn, cross build 0 warn, pytest 33 (29 + 4 new).
- [x] Task 10 (truth pass, whole-feature battery, handoff) — COMPLETE.
  README Maya mirror (3 blocks regenerated from kMayaRows: 138/10/30,
  generated by script not hand-typed); test_parity_readme_coverage extended:
  every Maya row in the block for its disposition, found==178 (non-vacuous
  by full count — the §excluded grep per rejected row is that check).
  Threat model: GLM closed 100-name env allowlist (replacing the stale
  "SP6 will render" sentence), dialect rule bullet (dialect relaxes framing,
  never the kill-on-unknown rule), kit-attestation rule (no parity claim
  before the PROVENANCE entry). AGENT_HANDOFF: new SP5 M2 section +
  superseded passages fixed (SP6 env-family 7-name row → 100-name set;
  glm53 pass-through row amended in place with its replacements).
  PROVENANCE maya touch note (citation target only; still zero code taken).
  Battery (final tree): linux-gcc ctest 826/826 100%; release build 0 warn +
  release ctest 10/10; cross 0 warn; root build (out/root-sp5m2) 0 warn +
  ctest 792/796 100% (4 env skips); pytest 33; distcheck script OK. TSan:
  fresh clang18 configure out/build/tsan-b (-fsanitize=thread -fno-pie;
  driver-side -no-pie needed for try_compile links — first configure died
  in FindThreads), filtered suites 216 run: 215 pass, ZERO ThreadSanitizer
  reports; the parked flake log_line_format_and_rate_limit fails 3/3 under
  TSan slowdown (early /health sees 503 in the fake-engine boot window —
  timing only, no sanitizer report). Ruling: recorded as observed, flake
  NOT touched; it passes every linux-gcc run.
## Rulings (this lane)

- **R-T2-a (README gate scope, Task 2):** `test_parity_readme_coverage` parsed
  EVERY `{"...}` row of parity_table.cpp and asserted 121; adding the GLM table
  made it see 299 and fail. Fixed by scoping the parse to the `kRows{{` array
  (121 assertion and every README-mirror assertion unchanged) and ADDING a GLM
  guard: `kMayaRows{{` must parse 178 rows. Strictness up, not down; Task 10
  replaces the guard with the full Maya mirror. — cost if wrong: a silently
  shrinking GLM table between Task 2 and Task 10 passes unnoticed; the count
  guard is what closes that.
- **R-T2-b (env inventory boundary, Task 2):** maya_env_knobs() = the documented
  runtime surface — SETTINGS.md family (26-101 ∪ whole file) ∪ serve/maya.py
  GLM-name writes ∪ server.py environ.get reads ∪ STRATA_WATCHDOG_S ∪
  MAYA_UPDATE_CHECK/MAYA_RESTART_ON_UPDATE. Excluded as launcher/build
  internals (recorded with cites in the header): MAYA_SH (maya.py:2781),
  MAYA_VERSION (serve/server.py:58 module constant), the eleven maya.py
  engine-BUILD env names, and the engine's undocumented src-internal getenv
  names (~180). The Task-4 child-env allowlist renders only table names, so a
  non-inventory name can never reach a child regardless. — cost if wrong: an
  operator knob outside the table exists and shows up as "unknown name" at
  wiring; SETTINGS.md is Maya's own documentation surface, so the risk is low.
- **R-T2-c (tokenizer/model_name typed for GLM, Task 2):** plan Task 3 names
  them as glm.* keys, so they are typed config_only (Maya's names, C-3);
  managed launches still compose identity/tokenizer from the SP4 manifest —
  Task 3 must make the manifest win where both exist. — cost if wrong: two
  sources name the tokenizer; Task 3 needs a precedence rule (documented there).
- **R-T2-d (licence vs window knobs, Task 2):** rejected on the A7 licence are
  exactly the DFLASH-named knob rows + the --dflash flag row; STRATA_GLM_WIN_*
  stay TYPED because the verify window also exists for MTP drafting
  (docs/SETTINGS.md:31,36,63 — "the MTP block... pipelined window", STRATA_GLM_NO_MTP/NO_SPEC
  are typed MTP knobs). D-7's letter ("every documented knob a typed row") and
  C-5's narrow ground agree under this reading. — cost if wrong: MTP-window
  tuning knobs carried that must have been licence-blocked: they set engine
  performance only, no network/no licence-weight fetching.
- **R-T2-e (path-valued engine knobs rejected, Task 2):** USAGE, ROUTE_LOG,
  CPU_CAL, SLOT_DIR, MTP_GGUF are rejected under §4's free-form
  log/cwd/model-path rule (they take arbitrary operator paths the engine
  writes/reads). Every other documented GLM knob is typed engine_env per D-7.
  — cost if wrong: parity loses five knobs the owner could argue are typed;
  the reject rows are explicit and cited, movable with a ruling.
- **R-T3-a (profile gate for glm.* rows, Task 3):** an explicit
  `engine.profile = "qwen"` refuses every present glm.* key by name with
  `conflicting_values` ("is only supported with engine.profile glm53"),
  mirroring the existing engine.gpu/glm53 rule (frontend_config.cpp:879) in
  the other direction. A config with NO engine.profile ACCEPTS the rows: a
  managed model's profile comes from the SP4 manifest, which the config
  loader cannot see — Task 7's launch composition must refuse glm.* for a
  qwen-manifest model (its own case + ruling there). — cost if wrong: an
  unenforced window between config and launch for managed qwen models; the
  Task-7 refusal closes it.
- **R-T3-b (glm.layer_split grammar only, Task 3):** validated as Maya's
  shape — "auto" or rising integers >=1 ("18", "16,32"; serve/server.py:
  1210-1213) — but NOT the engine.layer_split count rule against glm.gpu:
  the Task-1 fixtures accept "18,32" with glm.gpu "0,1" because a manifest
  may name the cards the config does not; the count check belongs to the
  Task-7 argv composition. — cost if wrong: one rejected-too-lenient config
  row until launch; launch refuses then.
- **R-T3-c (glm.tokenizer/glm.model_name rules, Task 3):** tokenizer takes
  `is_absolute_store_path` (same rule as session.slot_save_path); model_name
  is a 1-256-character non-empty string (bounds added, C-1). Both default
  unset — a managed launch keeps the manifest's identity/tokenizer (the
  R-T2-c precedence: manifest wins; the config rows exist for --expert-style
  parity and for Maya's own spelling, C-3).
- **R-T3-d (auth04 closed set, Task 3):** server_cli's security sweep names
  every schema boolean; the two new GLM booleans were ADDED to its pinned
  set (with cites server.py:3427/:3255) — the sweep stays closed, nothing
  weakened.
- **R-T3-e (tooling incident, Task 3, host):** several file-tool edits with
  RELATIVE paths applied to the MAIN workspace instead of the worktree
  (looked like silent reverts in the worktree). Recovered: the two files'
  main-workspace versions were the intended edits, copied into the worktree,
  `git checkout --` restored main to HEAD (verified porcelain-clean; this
  reverted ONLY my own unauthorized stray edits — nothing of the owner's).
  Lesson recorded: file tools take ABSOLUTE worktree paths only; shells keep
  using the workdir parameter. README table splices must end with exactly
  one trailing newline (the render's tail is `|\n`).
- **R-T4-a (allowlist supersedes the SP6 §4.9 ruled set, Task 4):**
  kFamilyForcibleEnvironment grows 7→100: SP6 §4.9's names ∪ the GLM table's
  98 typed engine_env names (97 STRATA_GLM_* + STRATA_WATCHDOG_S). D-7 (owner
  2026-10-21) supersedes A10, so SP6 §4.9's "never present" list no longer
  excludes the DOCUMENTED knobs (KV_INT8, CKPTS, MULTI_* and friends are now
  forceable because the table types them); rejected names (DFLASH C-5,
  updater/PRIV-01, the §4 path knobs) stay out — the closed list, the never-
  inherited list, the suppressible list and the duplicate/NUL refusals are all
  unchanged, so the gate only moves per the table. AGENT_HANDOFF's SP6 row
  (the 7-name set) and the threat model need a sentence at Task 10. — cost
  if wrong: none beyond D-7 itself, which is owner-decided; the table-driven
  sp6_env_forcible_names_are_the_ruled_set pin keeps list and table equal.
- **R-T4-b (env-echo fake, Task 4):** the three named cases assert the exact
  rendered block from render_family_environment — the value the spawn path
  consumes — and the existing process case
  sp6_env_derived_value_reaches_child already proves hook rows land in a real
  child's environment; Task 6's fake-engine Maya mode covers the per-profile
  env-echo end to end, so no second echo child is added now. — cost if
  wrong: a spawn-path regression between render and execve; covered today by
  the sp6 process case and next task by the dialect fake.
- **R-T4-c (knob channel, Task 4):** typed knob values reach the family
  through EngineEnvironment.glm_knobs (name/value strings) validated by the
  merge step against the closed list — no per-knob config keys this task
  (the plan's Task-4 interface names none). glm_environment_knobs() projects
  today's typed config (layer_split→STRATA_GLM_SPLIT); Task 7 extends the
  projection at the launch composition. The projection's own case
  (glm_environment_projection_moves_the_split_default) came with the
  implementation, not RED-first — the three plan-named cases carry the RED.
- **R-T5-a (fixture bug found during GREEN, Task 5):** the RED run of
  maya_flavor_consumes_note_and_progress also failed its next_event half —
  my sub-fixture never queued a READY line, so the client was never in the
  generating state. Fixed the fixture (added READY); no assertion was
  changed or weakened. The recorded RED text covers both halves.
- **R-T5-b (flavor selection stays for Task 7, Task 5):** server_bootstrap
  and subsystem_limits still map engine.profile glm53 →
  lighttransport_glm53 (SP6's engine). The maya_118 value is not selected by
  any site yet; Task 7's GLM launch composition decides the family and must
  move the selection with it — a glm53 model on the strata wire would be a
  dialect mismatch, so Task 7 carries a test pinning the selection. — cost
  if wrong: an unreachable enum value until Task 7; nothing can pick it.
- **R-T5-c (NOTE/STAT bounds, Task 5):** NOTE: `NOTE ` + at least one field;
  the text is never surfaced anywhere (only consumed). STAT: at least one
  `key=value` pair, non-empty key per pair; values are free text, never
  parsed, never surfaced. Line length is already bounded by the inbound
  record cap. Malformed → unexpected_line + protocol closed, matching the
  lane's fail-closed posture (stricter than Maya's own pump, deliberately).
- **R-T6-a (which cases carry the RED, Task 6):** the plan named
  glm_profile_generation_completes_over_maya_dialect and
  maya_dialect_cancellation_round_trips as RED, but the sequencing note let
  Task 5 land first — the client-side dialect they exercise already exists,
  so they pass on first run and stand as behavior guards (same posture as
  strata_flavor_still_fails_closed_on_note). The RED for this task is the
  process case fake_maya_dialect_round_trips_only_under_the_maya_dialect:
  the fake's --dialect=maya mode is what was missing, and the case fails
  RED in both halves (strict strata dialect sees no unknown line; the maya
  half completes only with strata framing today). Recorded rather than
  contrived: no existing test was weakened to manufacture a red.
- **R-T6-b (fake dialect mode shape, Task 6):** --dialect=maya layers
  Maya's cadence records over the same T/DONE payloads — NOTE right after
  GEN (generate.cpp:1245), 5-field PP per prefill chunk (:5546; maya also
  gets progress_records implied), STAT every 3rd token (:1799 key=value
  form). READY stays "READY <ctx> stop": the real Maya READY adds pause
  (and multi=) and the client's parser tolerates any non-empty tail flag
  (stop recognized), so the fake keeps the minimal honest form; the
  cancellation case proves the stop advertisement matters, not the extras.
  Proof is adversarial both ways: under strata_0143 the records must kill
  (protocol_failed); under maya_118 nothing is unknown. — cost if wrong: a
  fake dialect drifting from the vendored grammar; every emitted form is
  cited to the vendored engine.
- **R-T6-c (READY stop, Task 6):** EngineProtocolClient advertises
  supports_stop_ only from READY's `stop` flag; a cancellation test must
  script `READY <ctx> [stop...]` — my first script wrote a bare READY and
  the STOP leg failed with "engine could not be stopped" (client abandon).
  Fixed the script to Maya's real form (generate.cpp:2750), not the
  client. The byte-pack id offset (byte_for_id = id+65, test_tokenizer)
  explains why token ids 72/105 decode as non-printable bytes — the fake's
  default token id 7 (byte 72 = 'H') is the working choice.
- **R-T7-a ("manifest" at the GLM composition = the expert inventory model,
  Task 7):** engine_gate still refuses every non-qwen manifest from the
  managed path (engine_gate.cpp; pinned by launch_refuses_engine_too_old
  "glm53 until SP6"), so D-8's composed launch runs on the --expert path
  where the selected model comes from the inventory (enumerate_local_models).
  The pack is the directory holding the selected artifact (parent of
  relative_artifact_path under roots.models) — Maya's pack root keeps the
  tokenizer at <pack>/tokenizer (maya.py:1817). If SP6 opens managed glm53,
  the same composition serves the manifest's launch.native. — cost if wrong:
  a pack rooted at the artifact directory; Maya layouts share that shape.
- **R-T7-b (glm.max_context is a typed launch row, not a Maya config key,
  Task 7):** Maya keeps context as the --context setup flag whose result is
  written into args as --max-context (DEFAULT_CONTEXT 32768, maya.py:2524);
  the plan's Task-7 wording "--max-context (typed, pin default 32768)"
  authorizes the typed row. Name follows Maya's own flag spelling
  (max_context), bound 1-1048576 added (C-1). The VRAM-fitted clamping
  maya.py does after the choice is GPU-box work the kit judges, not typed
  here.
- **R-T7-c (--parallel bounds ride the composition, Task 7):**
  replicas.parallel keeps its schema 1-1024 (the strata side renders --batch
  past 8 per C-4); for glm53 the composition refuses >8 (Maya's argparse
  choices 1..8, maya.py:2742) with the fixed message — the schema cannot
  carry two bounds for one key. parallel 1 renders nothing (Maya's set_arg
  only writes the flag when a value is given, :1875; SP6's <=1 no-op agrees).
- **R-T7-d (flag rows without a value source today, Task 7):** the Task-2
  --expert-profile row says "per manifest" — a schema-1 glm53 manifest
  carries only launch.native (acquire_manifest.cpp:342-362), no
  expert-profile role, so the composition omits the flag; the remaining
  typed engine_arg rows (--kv-resident, --spec, --spec-min-p,
  --expert-profile-save/-every) have their destination at the composition
  site but no typed value source in the current family, and the plan's
  Task-7 list names none of them for M2. Deliberate known gap for the
  owner-box kit (Task 9 records both command lines); NOT silently dropped:
  the rows cite it and the FINAL-REPORT lists it.
- **R-T7-e (my own RED case fixed its error-code expectation, Task 7):**
  the glm.max_context bound assertions first expected invalid_value; the
  config reader emits ConfigIssueCode::out_of_range for range violations
  (the lane's own cfg01_out_of_range_rejected convention). Corrected my new
  case to the precise code during GREEN; nothing existing was touched or
  weakened.
- **R-T7-f (retired pass-through surfaces, Task 7):** plan Step 2 names the
  retirement — glm_profile_passes_arguments_through_without_native deleted
  (glm_rejects_every_engine_argument replaces every form it allowed), the
  config rule "glm53 requires at least one --engine-arg" deleted, the shell
  case renamed strata_secure_server_glm_rejects_engine_args with the
  named-error regex plus FAIL_REGULAR_EXPRESSION pinning that no value is
  echoed, and the GLM half of engine_args_render_typed_gpu_and_deny_gpu_
  passthrough re-pinned to not_accepted (empty-args + gpus still
  unsupported_mode: env carries the typed glm.gpu list, Task 4).
- **R-T8-a (LEND row source, Task 8):** the typed source of
  STRATA_GLM_VISION_LEND_MB (parity_table.cpp:349, server.py:3396) is
  vision.need_mib — Maya's on-demand turn lends exactly the encoder's VRAM
  need (images.py:156 arithmetic; the LEND knob takes the MiB figure). Only
  an ON-DEMAND turn lends: vision.resident true means the encoder holds its
  own VRAM (server.py:977 flow), so no row; unset need_mib means "the pin
  computes it" — we cannot compute driver-bound VRAM here, so nothing
  projects and the kit records the owner-box measurement. — cost if wrong:
  a mis-sized expert-pool carve on the owner box; the kit's LEND row is the
  measurement of record either way.
- **R-T8-b (shared vision.resident default stays TRUE, Task 8):** the plan's
  "resident policy per pin default" might suggest forking the default per
  profile (Maya's on-demand default is on-demand, server.py:3385). Rejected:
  one typed key, one default (KISS; the SP6 shared-key ruling from M1); the
  divergence is a parity-table fact the kit cites, not a config fork. The
  GLM gate reads the typed value either way.
- **R-T8-c (two gates, one name, Task 8):** projection (config→knob channel)
  AND render (engine_environment) both check the turn. Redundant by design:
  the channel is internal and reachable by future code, so the render fails
  closed even for a smuggled knob row — the test pins the smuggled case
  explicitly. The name joins kFamilySuppressibleEnvironment because a
  suppressed forced row must be suppressible to keep the merge gate
  total (engine_environment.hpp:115-117).
- **R-T8-d (encoder argv is shared surface, Task 8):** --no-flash-attn
  reaches the vision child through M1's compose_vision_arguments already
  (vision_supervisor.cpp:34, wired at server_bootstrap.cpp:304); the
  family-independent argv form is Maya's
  (server.py:983-991/:1158), so Task 8's half there is the guard pin, not
  new behaviour — recorded honestly like R-T5-a/R-T6-a.
- **R-T9-a (missing binary vs missing results file, Task 9):** the kit
  pytest finds the probe via STRATA_API_PROBE or the linux-gcc build dir;
  an unbuilt probe is an environment skip (the lane's 4-skip idiom), while
  WITHIN a scenario a missing/unparseable results file is a FAIL line with
  exit 1 — the plan's "FAIL not skip" property is about results files, and
  the compare mode enforces it as product surface, not on paper. — cost if
  wrong: a bare pytest run on an unbuilt tree silently loses kit coverage;
  the gate always builds first.
- **R-T9-b (the compare is shipped code, Task 9):** a paper diff invites
  "file missing → eyeball → skip"; probe --compare reads both results files
  and emits corpus,compare,FAIL,missing_results_file:<path> (exit 1) for an
  absent or header-less file. The pytest pins exactly that.
- **R-T9-c (my new harness bent to the product, not the reverse, Task 9):**
  the first canned server answered HTTP/1.0 and the client's strict
  HTTP/1.1-only head parser (api_client.cpp:141, by design — the product
  server is 1.1) returned protocol_error. Fixed the fixture (protocol_version
  HTTP/1.1 + Connection: close); no product code relaxed.
- **R-T9-d (corpus stays on the approved request schema, Task 9):** the
  probe injects its --model (the two sides have different model ids) and
  refuses stream:true (streaming is the SP3 kit's ground); the text class
  runs its own determinism pair, so determinism is proven per side before
  the cross-side text-for-text diff means anything (C-8).
- **R-T9-e (citations in the corpus, Task 9):** authorised_by values cite
  real rows of docs/superpowers/handoff/briefs/owner-decisions-2026-10-14.md
  (B1 auto max_tokens, B2 clamp) and the SP3 plan's EXT-01/§3.1 schema rule;
  where no B-number covers a divergence the corpus cites the spec section
  (SP3-Sec3.1) rather than inventing an id.

## Host adaptations (binding)

- No CI, no wine; GitHub storage only; lane never pushes (the controller saves the branch).
- Task 1's `git submodule add https://github.com/mw00/project-maya engines/maya-ref` at
  sha `5c970a863ce2fdd87b6e23ffb4822f6d76522b1c` is owner-authorized (D-10) — network use
  for that one fetch is fine; nothing else is fetched or installed.
- Per-task gates, ALL, synchronous in one foreground call with generous timeout, acted on
  in-turn (never background-and-pause): linux-gcc build (0 warn) + ctest serial +
  **linux-gcc-release build** + windows-llvm-mingw-cross build (0 warn) + pytest -q.
  No preset may ever configure/build engines/maya-ref.
- TSan: clang18 wrappers in .venv-dev/bin; TSAN_OPTIONS=external_symbolizer_path=<repo>/.venv-dev/bin/llvm-symbolizer.
- Commits: `shift-f10 <40450442+shift-f10@users.noreply.github.com>`, literal `Co-Authored-By: <real model> <noreply@anthropic.com>`, NO `Claude-Session:`.
- `upstream/` AND `engines/maya-ref/` are read-only references. Nothing outside the worktree.
  No rm -rf/git clean/bypass flags. Never weaken tests; parked flake
  `api_server_tests.log_line_format_and_rate_limit` untouched.
- Briefs hand-written to this workspace dir.

## Baseline to reproduce at Task 1 (measured by controller at merge 600e371c)

- ctest --preset linux-gcc: 803 run, 100% (807 registered; 4 allowed env skips)
- release preset ctest 10/10; pytest 26; cross 0 warnings; distcheck PASS.
