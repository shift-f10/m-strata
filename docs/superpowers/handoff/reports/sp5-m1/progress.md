# SP5 M1 (Strata parity table, lifecycle, CLI verbs) — lane ledger

Branch `sp5-parity-cli` at base `60f94aeb` (main). Upstream pin `cbfd5b92` (v0.2.1.3) — the read-only parity reference.
Plan: `docs/superpowers/plans/2026-10-21-sp5-m1-strata-parity-cli.md` (Tasks 1–15).
Spec: `docs/superpowers/specs/2026-10-21-sp5-parity-typed-options-design.md` (D-1…D-10, C-1…C-9 — binding).

## Status

- [x] Task 1 (baseline + seam inventory) — done at 60f94aeb, no commit (files: none)
- [x] Task 2 (parity table + key inventory + meta-tests) — GREEN, gates green
- [x] Task 3 (rejected keys fail at load by name; doctor env warning) — GREEN, gates green
- [x] Task 4 (sampling defaults at the pin) — GREEN, gates green
- [x] Task 5 (reasoning family + fit_max_tokens) — GREEN, gates green
- [x] Task 6 (lifecycle: lazy_load, idle_unload_s, mtp) — GREEN, gates green
- [x] Task 7 (VRAM/GPU/elastic family typed rows) — GREEN, gates green
- [x] Task 8 (timeouts/queues/request-shape keys) — GREEN, gates green
- [x] Task 9 (session/context family) — GREEN, gates green
- [x] Task 10 (replicas, C-4 narrowing) — GREEN, gates green
- [x] Task 11 (vision artifacts) — GREEN, gates green (commit cbb4e747)
- [x] Task 12 (ENC exchange + chat image_url path) — GREEN, gates green (commit b3b7c6d6)
- [x] Task 13 (`strata serve` foreground) — GREEN, gates green (commit 8b61436b)
- [x] Task 14 (strata version / key show|rotate) — GREEN, gates green (commit 18b2af90)
- [x] Task 15 (docs truth pass + whole-branch review) — GREEN, gates green

## Task 15 record

- commit `a38704bf` "docs: SP5 M1 truth pass and handoff" (plan message verbatim; branch tip).
- README: parity mirror inserted (three marker-guarded blocks: typed 56 / mapped 28 /
  rejected 37 rows, regenerated from src/parity_table.cpp by a throwaway generator —
  the C++ table stays the authority); `strata serve/version/key` paragraph added; the
  stale truth sentence "deliberately rejects tools, images, URLs, MCP..." fixed (images
  are an input path for vision-capable models, data-URL only; http(s) image URLs refused
  by name); narrowing sentences (C-4 batch, answer_room typed-only, lazy_load no flag,
  vision cgroup gap, keep_files consumer out).
- New pytest gate tools/secure/test_parity_readme_coverage.py: parses the 121 parity
  rows + the 118-name inventory, asserts every row appears in its README block by
  disposition and the mirror counts >= 80 (found = 121); config-table floor >= 100.
  First run: 26 passed.
- docs/security/threat-model.md: bullets added under Browser/HTTP (image path), Config
  and filesystem (env-knob warning; no-lifecycle-files serve; vision scratch rules +
  whitespace boot guard), Engine isolation (vision child stdio-only + cgroup-gap
  recorded, lifecycle timers are frontend decisions).
- docs/PROVENANCE.md: bump runbook step 5 now names the SP5 parity artifacts as
  line-number pin sites (inventory header greps, per-row citations, README mirror gate).
- docs/AGENT_HANDOFF.md: SP5 M1 section (task table 1-15 with commits, final gate
  numbers, recorded gaps) inserted at the top of "What has changed".
- docs/superpowers/handoff/reports/sp5-m1/FINAL-REPORT.md: written (lane story, final
  gates, rulings index, parked list).
- Gates on the final tree: debug ctest 803/799-run/4-skips 100% (unchanged since T14 —
  README-only C++ exposure re-checked via the readme gate, rc=0); release 10/10;
  windows-llvm-mingw-cross 0 warnings (Windows/wine NOT run on this host); root build +
  ctest 769 registered 100% / 8 env skips / 0 warnings; clang18 TSan filtered
  concurrency suites 224 cases ZERO ThreadSanitizer reports (parked flake
  log_line_format_and_rate_limit flaked once, passed rerun — untouched); pytest 26.
- Whole-branch review (60f94aeb..HEAD, 13 commits pre-docs): all authored shift-f10,
  13/13 Co-Authored-By Qwen trailer, zero Claude-Session trailers, zero upstream/ files
  touched, no TODO/FIXME added, aggregate 85 files +6484/-441 all inside secure_frontend
  (+ this docs commit outside it). Move-verbatim proof for the server_bootstrap split was
  diff-verified at Task 13.

## Lane closed

- All 15 plan tasks GREEN; branch tip waits for the owner's merge word (agents never push).
- FINAL-REPORT.md at docs/superpowers/handoff/reports/sp5-m1/FINAL-REPORT.md.

## Task 14 record

- commit `18b2af90` "cli: strata version and strata key show|rotate land" (plan message verbatim).
- RED first: acquire_cli cases `version_names_frontend_and_engine_versions` +
  `key_show_requires_local_token_and_never_logs` — both Failed; the version case's first
  line pinned the missing verb directly ("strata version runs without --config" →
  usage + exit 1). (One intermediate sweep compiled the TEST with two mistakes of mine —
  perms masks, increment() — and its ctest run hit stale binaries after an
  undefined-reference link failure; fixed the test, added the missing
  secure_frontend_server_cli link for frontend_init's generate_token, re-captured RED
  from a clean build, then GREEN.)
- Rulings T14-a..f live in task-14.md. Shape: no separate frontend version exists — the
  tag governs, so `strata version` prints three fixed lines from the ONE configure-time
  source (built_engine_version_text = upstream.version): frontend version, upstream pin,
  and the engine self-version labelled informational WITHOUT a number (upstream's own
  0.1.46.2 lives in upstream/CMakeLists.txt, which configure deliberately never reads).
  Bare `strata version` short-circuits before the --config rule (spec writes it bare);
  the --config form validates then answers identically (asserted same-bytes).
  `key show [--env]`/`key rotate`: env: source refuses (exit 1, names env:); the read is
  SP1's read_token (store rules = the "readable by this uid" verdict; a chmod-0644 store
  is refused, exit 7, nothing printed); rotate = frontend_init's generate_token(force)
  (CSPRNG + atomic replace + 0600) then the same print path; --env prints the PX
  OPENAI_API_KEY= line; token printed exactly once; tree-walk proof: rotated-out token
  in ZERO files, live token in exactly ONE (the store). Grammar: key{--env}, version{},
  kUsage gains both lines.
- Gates: debug 803 registered / 799 run / 4 env skips, 100% (was 801/797; +2); release
  10/10; windows-llvm-mingw-cross clean, 0 warnings (Windows/wine runtime NOT run on
  this host); pytest 25. No new threads: no TSan spot.

## Task 13 record

- commit `8b61436b` "cli: strata serve (foreground) lands" (plan message verbatim).
- RED first: server_cli_test cases `serve_prints_ready_then_serves` +
  `serve_runs_foreground_and_shuts_clean_on_sigterm` — both Failed; the child's stderr
  showed the missing verb directly: "strata: unknown verb", exit 1.
- Rulings T13-a..h live in task-13.md. Shape: serve = managed only (no --expert — that
  stays a strata-secure-server flag), no options/operands; the supervised boot moved
  VERBATIM from secure_server_main.cpp into NEW library secure_frontend_server_bootstrap
  (src/server_bootstrap.cpp: server_run/server_check + file-local helpers), the server's
  main slimmed to argv/init/check dispatch; strata_cli gains grammar {serve}, verb_serve
  (calls server_run(config, false)) and the kUsage line; install_process_defaults also
  runs at the top of server_run (idempotent, T13-c). Ready literals are the boot's own
  (T13-d): stdout "Secure server listening on http://<authority> (authenticated;
  Ctrl-C to stop)" + stderr "strata: listening <authority>, workers N, queued generations M"
  — both asserted by name in the test (my first expectation mixed the two streams; fixed
  before GREEN against the real literals, not by weakening the case).
- Test plumbing (T13-f/g): SECURE_FRONTEND_SERVER_ARGS gains $<TARGET_FILE:strata>
  (ARGS[2]); ServerFixture gains cli_executable/run_cli/start_cli + shared
  wait_until_listening; acquire_fixtures gains qwen_payload_pack (real byte_tokenizer
  trio, no mmproj) via the shared use_real_tokenizer_trio; serve test proves the broken-
  config validator text first (T13-h), SIGTERM->exit 0 <=5 s on POSIX only (T13-e), and
  the scratch-tree walk refuses any server.json/.lock (T13-f: PX writer still does not
  exist, pin has none).
- Gates: debug 801 registered / 797 run / 4 env skips, 100% (was 799/795; +2); release
  10/10; windows-llvm-mingw-cross clean (Windows/wine runtime NOT run on this host);
  pytest 25. No new threads: no TSan spot (brief).

## Task 12 record

- commit `b3b7c6d6` "vision: image_url path with pre-decode caps and ENC round trip"
  (plan message verbatim). The request half of D-9: schema parts carve-out, pre-decode caps,
  staging, the ENC round trip, GENI, vision-marker prompts, per-request scratch cleanup.
- RED (cases written and failing before any implementation): chat_schema
  `image_url_parts_accepted_only_for_vision_models` + `image_part_caps_and_shape_are_checked_before_decode`;
  NEW image_payload_test.cpp `image_payload_stages_valid_images_with_the_pin_key` + 2 more;
  chat_generation_plan `vision_marker_runs_join_the_prompt_ids` (+ glm closes-images case);
  engine_protocol_client GENI case; engine_supervisor `vision_encode_round_trip_at_the_pin`
  + `vision_encode_failures_take_the_pinned_paths` (fake child modes badenc/silentenc/dieenc);
  api_server `enc_round_trip_reaches_the_prompt` + `image_rejections_name_image_url_before_any_write`.
  RED sweep `ninja -k 0`: 72 error lines naming stage_image_payload/StagedImage/ImagePayloadLimits,
  ChatMessage::parts + ChatSchemaLimits vision members, the begin() embeddings settings member,
  VisionSupervisor::encode/VisionEncoded/VisionEncodeError, and the 6-argument
  make_qwen_chat_generation_plan. (Captured in-session; exact lines live with the RED tree.)
- Rulings T12-a…T12-l live in task-12.md (data-URL-only M1; schema gate = vision_capable;
  cap order shape->length->decode->32 MiB->dim probe; 0600 scratch under <state>/vision as
  the child cwd; no LRU, per-request delete; encode inside the lease before the GENI write
  (pin FIFO :4489); marker runs in the qwen prompt; encoder-dead -> 503, ERR -> 400 naming
  image_url; typed row vision.encode_timeout_s 0..86400 d300 (schema 108 -> 109, README
  re-rendered + Vision paragraph extended); boot guard: vision.enabled + state path with
  whitespace = boot fails (#480). T12-k: kChatMaxJsonDepth 4 -> 6 for the parts grammar;
  two SP3 depth pins moved with the rule (stronger schema-level 400s, DOS property intact).
  T12-l: chat model grammar mirrors ModelId name[:tag] — managed ids were unaddressable;
  grammar pins moved, refusal property intact (pull:x -> model_not_found 404, no GEN).
- Implementation shape: chat_schema parts/caps + image_data_url_shape; image_payload.{hpp,cpp}
  (strict base64, PNG/JPEG/GIF/BMP dim probe, sha256(bytes)[:32] key, VisionService
  stage/encode/combine/discard); VisionSupervisor gains exchange_ mutex + atomics, encode()
  with lazy wake (:2646) and shutdown-after-stop() semantics; GENI in engine_protocol_client
  (+ embeddings validation); GenerationSettings.embeddings; qwen renderer parts + marker runs
  (+ three vision-marker literals refused in user content); tokenizer pack: five special
  markers, vision ones required only when present; chat_route stage->gate->encode->plan->
  GENI order with RAII scratch cleaner (combined file follows the generation: non-stream
  after DONE, stream inside the closure); main: optional<VisionSupervisor>+optional<VisionService>
  built pre-route, spawned post-bind, stopped before the engine supervisor; <state>/vision
  created 0700 at boot.
- Gates: debug build clean under -Werror; ctest 799 registered / 795 run / 4 env-only skips,
  100% pass (was 788/784; +11 cases); release ctest 10/10; windows-llvm-mingw-cross clean,
  0 warnings (Windows/wine runtime NOT run on this host — CI disabled, owner-hardware rule);
  pytest 25 passed; TSan spot (out/build/tsan, setarch -B -R): both new vision supervisor
  cases race-free.
- Gaps recorded: session.keep_files consumer stays out (no LRU in M1, T12-d — row already
  marked); vision child gets no per-generation cgroup delegation (T11 gap, unchanged);
  GPU/real-encoder behaviour only from the owner-box kit.

## Task 11 record

- commit `cbb4e747` "vision: mmproj manifest role and supervised vision child launch proof"
  (plan message verbatim). Eight schema rows (100 -> 108 keys): vision.enabled (bool false —
  gates the family), vision.max_side (1..16384 d2048 — images.py:102 `or 2048`, cap added
  C-1), vision.max_n (1..64 d4 — images.py:103 `or 4`, cap added), vision.need_mib
  (1..1048576 unset — images.py:156 override; the driver-bound VRAM gate itself stays
  owner-box, cf T7-c), vision.no_flash_attn (bool false — spec §5.1 family, NOT read by this
  pin: added; the kit proves the child flag), vision.resident (bool true — the pin's eager
  encoder server.py:2511/:2552; false = the first picture starts it, Task 12), vision.threads
  (0..256 d0 — :2562 truthy->--threads, 0 = unset, cap added), vision.ready_timeout_s
  (0..86400 d300 — the STRATA_VISION_READY_S counterpart, :327; 0 = wait for ever, :291 —
  the shape engine.silence_s took in Task 8). VisionConfig on FrontendConfig; describe 8
  lines; README table 108 rows + "Vision (SP5 M1, spec D-9)" paragraph; auth04 boolean
  allowlist gains vision.enabled/no_flash_attn/resident (behaviour switches, none touches
  auth — same ruling pattern as Tasks 5-9).
- Parity spine fixes (same commit, per the Task-2-row rule): the "threads" row's old
  citation "(M1 Task 7: engine.threads)" was WRONG — :2562 sits inside the vision
  reader_args, the ONLY pin read of threads; citation now names vision.threads, destination
  engine_arg kept (the flag lands on the encoder child's argv). STRATA_VISION_READY_S row
  re-cited :327 + typed row vision.ready_timeout_s. max_side/max_n/need_mib rows kept their
  Task-2 images.py citations (Task 12 consumes max_side/max_n at image_url).
- Manifest schema 1 extension (Ruling T11-bb): files[].role "mmproj" (AT MOST ONE entry —
  role_name/parse/validate; "vision"/"draft" stay reserved-and-rejected; the role is listed
  like any artifact so pull/bundle/download paths unchanged); ManifestLaunch gains optional
  mmproj; raw rejects launch.mmproj (no launch keys), glm53 keeps "glm53 has only
  launch.native in schema 1" (Maya vision is the M2 kit, D-9/D-10), qwen: launch.mmproj must
  name a listed mmproj file.
- Compose rules (Ruling T11-aa): ComposedLaunch gains vision_mmproj/vision_model (absolute
  UTF-8, joined via in_version exactly like --native; carried whenever the manifest declares
  mmproj, enabled or not); vision.enabled without mmproj -> LaunchFailure ExitCode::manifest
  naming the artifact; engine.lazy_load + mmproj model -> refused at compose (the pin's
  config conflict, runconfig.py:97-98 "lazy loading is text-only, and this model reads
  images" — pin wins over our lazy default); --expert + vision.enabled -> LaunchFailure by
  name (no manifest, no mmproj); engine gets --vision when enabled and the operator does not
  own the flag (the pin's #1322 section gate :2926-2931); the pin's auto --vram-reserve-mib
  700 (:2933-2935) NOT composed — GPU-encoder fact from the owner box, knob typed in Task 7.
- Supervision (Ruling T11-w): NEW VisionSupervisor role (vision_supervisor.{hpp,cpp}), not a
  second EngineSupervisor — the pin's child speaks READY/ENC/QUIT (:2599-2612 READY,
  :2755-2792 ENC/QUIT), not GEN/T/DONE with chat leases. Reuses ProcessFactory/
  SupervisedProcess/EngineStdioProcess/RestartPolicy/RestartTracker. READY gate: FIRST
  stdout line must start with "READY" (prefix, not our engine's "READY <ctx> <mode>"
  grammar); oversize line (shared 8192-byte bound) / closed stdout / death / timeout end the
  generation; kill_group + bounded reap + record_failure(healthy_for 0); restart ONCE then
  circuit (VisionLimits default max_consecutive_failures 2, plan's line). stop(): QUIT
  (:2792), 2 s drain, kill else reap. compose_vision_arguments renders the pin's
  reader_args order :2556-2568.
- Ruling (T11-x): distinct exit-code space per plan — exit_status.hpp kVisionKillExitCode =
  0x20000002; EngineCommand gains defaulted kill_exit_code, EngineStdioProcess captures it in
  start() and Windows kills (terminate()/kill_group TerminateJobObject) use it.
- Ruling (T11-ff): EngineStdioProcess::start's literal --serve insertion is the ENGINE
  protocol; the pin's vision reader_args carry NO --serve, so EngineCommand.insert_serve
  (default true, POSIX path rides posix_launch::prepare's existing insert_serve seam) and
  the vision role starts with false. Found while writing the launch-proof test — the fake's
  argv record would have shown a foreign --serve.
- Ruling (T11-y): encoder exe = OUR distribution's strata-vision (Windows: strata-vision.exe)
  BESIDE the engine executable, discovered at boot; missing + vision.enabled -> boot fails
  with fixed text. The pin's config-named vision exe (:7674/:7934 component folder) stays
  kRejected (spec §4 exe row); our confinement model owns engine binaries.
- Ruling (T11-z): boot policy — resident (the pin's eager default): start after the engine is
  up; if the READY gate exhausts the policy, BOOT FAILS (vision.enabled is a promise of
  images). resident=false: no boot start; the Task 12 request path wakes it; circuit-open
  later means image requests fail while text serving continues (Task 12).
- Ruling (T11-ee): vision child environment = the empty allowlist (no GPU vars, SP2 §3.9
  family rule), working_directory = prepared state dir (image scratch arrives with Task 12),
  no_new_privileges like the engine; NO per-generation cgroup delegation on this child in M1
  (recorded gap for AGENT_HANDOFF at Task 15; the SP2 cgroup seam is EngineSupervisor-shaped).
- Ruling (T11-dd): engine.max_tokens/min_tokens ALSO reach the vision argv as
  --max-tokens/--min-tokens when set (:2564-2567 — their only pin home), on top of Task 8's
  request floor/ceiling; compose_vision_arguments proves it.
- Ruling (T11-gg): vision.ready_timeout_s 0 = wait for ever — the pin's own timeout rule
  (:291), so the READY loop ends only on a line or the child's end.
- RED evidence (compile): manifest_test.cpp "no member mmproj/... FileRole::mmproj is not a
  member" via acquire_fixtures.hpp:67; engine_arguments_test.cpp:247/248 "ComposedLaunch has
  no member named vision_mmproj/vision_model"; typed_config_test.cpp:1437 "FrontendConfig
  has no member vision"; engine_supervisor_test.cpp "secure_frontend/vision_supervisor.hpp:
  No such file or directory". GREEN runtime iterations fixed only test-support shape (5th
  aggregate init m.launch, test moved below its text helpers, base_manifest() for valid
  SHAs, distinct second-mmproj path, value-capturing vision_factory) — no test weakened.
- Test support: strata-fake-vision-child (tests/fake_vision_child_main.cpp; records argv per
  spawn, READY, --mode=oversize|diedie; ENC/QUIT protocol ready for Task 12);
  STRATA_FAKE_VISION_CHILD_PATH via secure_frontend_test_paths; fx::qwen_payload_vision +
  manifest_text launch_mmproj + cli::install(vision) for the compose fixtures. 5 new cases:
  manifest_accepts_mmproj_role_and_rejects_duplicates, vision_child_launches_with_pinned_
  args_when_enabled, vision_child_respects_resource_limits, vision_rows_match_the_pin,
  vision_compose_rules.
- Gates: linux-gcc 0 warnings; serial ctest 788 registered / 784 run / 4 env skips, 100%
  (the parked flake log_line_format_and_rate_limit passed both full runs this task,
  untouched); release 0 warnings + 10/10; windows-llvm-mingw-cross 0 warnings, exit 0
  (Windows/wine runtime: not run on this host); pytest 25. New process cases are Linux-run
  like the SP2 process family; no new threading (supervisor is single-owner-thread; Task 12
  adds the serving-thread path + its TSan spot).

## Task 10 record

- commit `761b9b4a` "replicas: engine-internal parallel typed; N-child narrowing documented
  and rejected by name" (plan message verbatim). One schema row (99 -> 100 keys):
  replicas.parallel (integer 1..1024, default 1 — the pin's one-at-a-time); ReplicasConfig on
  FrontendConfig; engine_arguments ParallelBatchArgs + append_parallel_batch_arguments
  rendering the pin's EXACT shape: --batch N (serve/server.py:2975 — "parallel" in, --batch
  out; llama.cpp's historical name), <=1 nothing (:2968), operator --batch or --slots wins
  (:2961), values past PARALLEL_MAX 8 (:2954) ride on — the engine warns and adjusts,
  recommend never force (:2969-2974); both compose sites call it; describe 1 line; README
  table 100 rows + a new "Replicas (SP5 narrowing, spec C-4)" paragraph in Configuration;
  config_file's fixed excluded_key reason now cites README ("the accepted narrowing is in
  README, section Configuration"); parity parallel citation extended to :2955-2975.
- RED evidence: compile failures — engine_arguments_test.cpp:467/469 "ParallelBatchArgs has
  not been declared / append_parallel_batch_arguments is not a member";
  typed_config_test.cpp:1379 "FrontendConfig has no member named replicas". Then runtime
  RED: the replicas dict slipped past the rejected-name scan (see T10-b).
- Ruling (T10-a): replicas.parallel's rendering is --batch N, not --parallel (the plan said
  "--parallel N per pin" — the pin's parallel_args returns ["--batch", str(n)], :2975).
  Bounds: pin's CLI bound 1..1024 (:7845) adopted as the added typed cap (C-1); the pin's
  config path itself has no cap (warn-and-adjust past 8), so 9..1024 stay valid and reach
  the engine; 0 rejected (the pin's off is 1/absent, :2957).
- Ruling (T10-b): the typed section name `replicas` collides with the rejected flat pin name
  (the scheduler dict, replicas.py:44). The Task-3 scan's "schema sections win" rule is
  sharpened: a section name wins ONLY if the object speaks the typed namespace (every member
  names a schema path under it, typed_section_object); `replicas: {"count": 4}` uses none —
  it is the pinned dict and fails with the C-4 rule by name (each_rejected sweep +
  multi_child_replicas_reject_with_the_ruling). One existing generated expectation updated
  with a citation: the schema-driven typo sweep's `replicas.parallel_typo` now expects the
  excluded_key "replicas" instead of unknown_key (still rejected at load by name — code and
  name now carry the narrowing rule, nothing weakened).
- INCIDENTS/fixes: none besides T10-b (found by GREEN gates, not by weakening anything).
- Gates: linux-gcc 0 warnings; serial ctest 783 registered 100% (4 env skips) — the parked
  flake log_line_format_and_rate_limit fired ONCE mid-suite under load ("fixed fields only:
  req 84/99" = its token-bucket signature), passed in isolation and on the confirming full
  re-run; untouched per owner rule. release 0 warnings + 10/10; windows-llvm-mingw-cross 0
  warnings (wine/Windows runtime: not run on this host); pytest 25. No new threading.

## Task 9 record

- commit `3bc08b21` "session: slots, expert profiles, recovery keys typed with path rules"
  (plan message verbatim). New `session.` namespace, 8 schema rows (91 -> 99 keys):
  session.slot_save_path (string), session.expert_profile_save (string),
  session.expert_profile_save_every (integer 0..1440 default 10 — docstring :2996, cap added
  C-1), session.tool_call_recovery / env_to_user / codex_thread_titles / codex_compaction_cache
  (booleans, default false — strict bool at the pin :8106/:8110, `is True` :8043-:8044),
  session.keep_files (integer 1..1024 default 64 — images.py:188 `or 64`, bounds added C-1).
  New SessionPolicy on FrontendConfig; is_absolute_store_path (pin's slot_save_dir shape
  :3472-3475 + our absolute rule, engine.lib_dirs precedent); engine_arguments
  ExpertProfileSaveArgs + append_expert_profile_save_arguments (the pin's guard: blank path
  or operator-owned --expert-profile-save returns args unchanged BEFORE either flag, :3001;
  the every rides the save, never stands alone); both compose sites call it; describe 8 lines;
  README regenerated (99 rows); auth04 allowlist +4 session booleans.
- Parity: slot_save_path engine_arg -> config_only (T9-a — implementation evidence: every pin
  use is svc-side (:8096/:3698-3715), no argv append exists); keep_files frontend_behavior ->
  config_only (T9-e — consumer is the pin's ImageService, D-9 out); expert pair citations
  extended to the append lines :3000-3005.
- RED evidence: compile failures — engine_arguments_test.cpp:466-470 "ExpertProfileSaveArgs/
  append_expert_profile_save_arguments is not a member"; typed_config_test.cpp:1250+
  "FrontendConfig has no member named session" (session_rows_match_the_pin et al).
- INCIDENTS/fixes (GREEN phase, nothing weakened): (1) GCC parsed my single-expression
  `return ValidatedConfig{std::shared_ptr<const FrontendConfig>(new FrontendConfig(10 args))};`
  with a bogus cascade ("expected primary-expression before '{'"); splitting the shared_ptr
  into a named local compiles clean — the ctor call itself was always fine (diagnostic quirk,
  cosmetic change only); (2) my operator-guard expect counted --expert-profile-save in the
  COMPOSED list, but the operator's flag lives in the user-args input — the assertion now
  checks the composed list stays empty (stronger, same intent); (3) test-local `using
  secure_frontend::ParityDestination` — both enums live in secure_frontend::parity; fixed.
- Ruling (T9-a): slot_save_path never reaches the engine argv at the pin — slot_save_dir
  (:3469-3481) feeds the pin frontend's POST /slots save/restore file store (:3698-3715); our
  route table has no /slots (spec §3.6). Typed + path-validated now (the pin's own file-store
  feature stays out; owner decision if wanted). Plan's "reaches env/arg per pin citation"
  wrong at the pin — pin wins.
- Ruling (T9-b): path rows reject relative paths (no config-cwd concept; added bound C-1,
  engine.lib_dirs :794-797 style), keep the pin's non-empty + no-control-chars rules, and do
  NOT touch the filesystem at load (the pin's makedirs 0700 is a runtime act — recorded in
  the row; the "private" in the plan's test name = that 0700 intent).
- Ruling (T9-c): expert pair defaults: every = 10 (the pin's docstring default), 0 = at-QUIT
  only kept verbatim; cap 1440 added. No profile-shape comparison logic (learned-vs-base is
  the ENGINE's job for llama-server; the pin's :3006-3010 shape check has no frontend role here).
- Ruling (T9-d): the four session booleans are typed config_only — their pin-side consumers
  (OutputParser recovery, frontend._env_to_user, Codex routes #923/#924) have no counterpart
  in our stack; they validate + describe only, defaults false like the pin.
- Ruling (T9-e): keep_files destination corrected frontend_behavior -> config_only; the
  consumer is the pin's sd-cli ImageService scratch retention (D-9 out). If the Task 12
  image_url path needs scratch retention, revisit then; gap noted for Task 15.
- Gates: linux-gcc 0 warnings; serial ctest 776 run 100% (registered 780, 4 env skips;
  parked flake did not fire); release 0 warnings + 10/10; windows-llvm-mingw-cross 0 warnings
  (wine/Windows runtime: not run on this host); pytest 25. No new threading — no TSan spot.

## Task 8 record

- commit `9a6273ce` "engine: silence/queue/room/stop-token keys at the pin". 6 schema rows
  (85 -> 91 keys): engine.silence_s (integer 0..86400, default 300 — runconfig.py:39,
  "0: wait"; cap added, C-1), engine.answer_room (boolean true — serve/server.py:8014
  `is not False`), engine.repeat_stop_tokens (integer 0..65536, default 256 — REPEAT_STOP_TOKENS
  :197, :8115-8118; cap added), engine.max_tokens / engine.min_tokens (integer 1..1048576,
  unset — added bounds per the Task-2 rows, C-1), model.aliases (string array <= 16 entries,
  grammar validate_model_id + added 64-char cap — runconfig.py:34, set_aliases :3764-3777).
  ChatExecutionLimits +silence_timeout/+repeat_stop_tokens; GenerationRunLimits +silence/
  +repeat_stop_tokens; the shared pump (generation_run.cpp) gets the silence watchdog (valid
  event = any token/progress/DONE; a stall reuses the timeout cause -> timed_out + length,
  B4 path) and the repeat run-length detector (StopCause::repeat -> RunEnd::length; the run
  is kept, everything after the STOP is dropped; :4908, notice :5088); drive_limits carries
  both to BOTH pumps (chat_route.cpp:162 streams through drive_limits too); subsystem_limits
  maps the config rows; resolve_max_tokens +minimum_tokens/+maximum_tokens trailing params
  (floor after the fit=false room rule, ceiling last); chat_route answers aliases exactly
  (:62) and passes floor/ceiling; api_router lists aliases in /v1/models and finds them at
  GET /v1/models/<alias>; main dedups aliases and drops one equal to the served id
  (set_aliases :3774-3777); auth04 allowlist +engine.answer_room; describe 6 lines;
  README regenerated (91 rows). Parity: aliases config_only -> frontend_behavior,
  answer_room frontend_behavior -> config_only (T8-d), repeat_stop_tokens engine_arg ->
  frontend_behavior (T8-f), max_queue/queue_timeout/timeout_s/STRATA_ADMIT_MAX_WAIT_S
  kTyped -> kMapped to the existing SP3 rows (T8-a/b).
- RED evidence: compile failures — chat_execution_test.cpp:391-401 "too many arguments to
  function resolve_max_tokens(...)" (6 expects), ReadyService brace-init with silence_timeout/
  repeat_stop_tokens; typed_config_test.cpp:1171+ EngineLaunch "no member silence /
  answer_room / repeat_stop_tokens / max_tokens / min_tokens / aliases"; then GREEN caught
  the api rows (server refused the unknown keys pre-rows — the two api cases were vacuous
  until the rows landed, failed at GEN-assert level after, passed after the wiring).
- INCIDENTS/fixes (GREEN phase, not weakened): (1) my pump tests were first inserted BEFORE
  the ReadyService/StopAwareTransport fakes (compile) — relocated; (2) designated-init order
  in the repeat test's ChatExecutionLimits literal — reordered to declaration order; (3) the
  schema's shared list rule rejects repeated entries, so the aliases accept-case was a bad
  test — recorded as Ruling T8-h and the duplicate is now a reject-case (assertion added,
  not removed); (4) dos02_slow_reader and stream_cut (StalledStream fixture) failed once the
  repeat rule defaulted ON: their filler-token streams (1024/16384 identical ids) legitimately
  hit the pin's 256 bound — both fixtures set engine.repeat_stop_tokens 0 (config only, all
  assertions identical; Ruling T8-i). Parked flake did not fire.
- Ruling (T8-a): no new queue rows — the pin's max_queue (:7833, bound :7835) and
  queue_timeout (:7837, bound :7839) are already carried by limits.concurrency.
  max_queued_generations / queue_timeout_ms (SP3); parity rows re-destined kMapped,
  citations updated. STRATA_ADMIT_MAX_WAIT_S likewise.
- Ruling (T8-b): timeout_s (images.py:226 generation-job timeout at the pin) is carried by
  the existing limits.engine.generation_timeout_s; row re-destined kMapped (no new row).
- Ruling (T8-c): engine.silence_s 0 = wait (the pin's), still bounded by the total generation
  deadline; the watchdog counts only VALID engine events (pin: "nothing valid", :2861-2868).
- Ruling (T8-d): answer_room is typed + described but NOT enforced: the pin's formula
  (:4704-4716) lives in its client-side thinking/reasoning parser, which our §7.2 room rule
  has no counterpart for (Task 5 rows are config-only). Enforcement gap recorded for the
  Task 15 docs-truth pass; destination config_only.
- Ruling (T8-e): engine.max_tokens/min_tokens re-scoped as request output ceiling/floor at
  the §7.2 resolve site (the pin's own keys bound the vision encoder, :2564-2567 — the
  re-scope is an added frontend bound, C-1). Floor applies AFTER the fit=false room refusal
  check (our bound never turns a request into a refusal); ceiling caps last, after the
  completion cap and context room; the pin's §7.2 refusals stay untouched.
- Ruling (T8-f): repeat_stop_tokens destination corrected engine_arg -> frontend_behavior:
  the pin's detector is frontend-side (:4908), not a GEN key (the plan said "reaches GEN" —
  wrong at the pin). Implemented in the shared pump; 0 = off (:8115); finish "length" (:5088).
- Ruling (T8-g): model.aliases entry grammar = SP1's one model-ID grammar (validate_model_id)
  + added 64-char cap (C-1); list max 16 = kMaxListEntries (added); matching is exact in
  chat and in /v1/models/<id> (§4 never partial/empty); an alias equal to the served id is
  dropped at wiring (set_aliases :3775).
- Ruling (T8-h): model.aliases rejects repeated entries — the schema's shared list rule
  (host_aliases/cors_origins precedent); the pin dedups silently at :3774, we reject
  (stricter, consistent); wiring still dedups defensively.
- Ruling (T8-i): dos02_slow_reader_never_backpressures_engine and StalledStream set
  engine.repeat_stop_tokens 0 — their streams are 1024/16384 of one filler token id, which
  the pin's default-ON runaway rule would cut; the tests target reader backpressure and the
  write-stall cut, so the rule is off in those fixtures only (harness config change, no
  assertion altered).
- Gates: linux-gcc 0 warnings; serial ctest 771 run 100% (registered 775, 4 env skips);
  release 0 warnings + 10/10; windows-llvm-mingw-cross 0 warnings (wine/Windows runtime:
  not run on this host); pytest 25. Silence/repeat ride the existing single-threaded pump —
  no new threading, no TSan spot.

## Task 7 record

- commit: `engine: vram/gpu/elastic family typed to the engine argv` (the plan's message
  said "and child env"; corrected per Ruling T7-a — this family composes NO env).
  13 schema rows (72 -> 85 keys): engine.vram_reserve_mib (unset = the engine's own 700),
  engine.min_free_vram_mib (int-or 0, :8083), engine.vram_elastic / engine.vram_segment_mib
  (unset = 512) / engine.split_skip_if_fits, engine.elastic / engine.elastic_idle_s (120)
  / engine.elastic_mib (2048), engine.gpu_order (auto|as_given), engine.gpu_policy (POLICIES),
  engine.layer_split ("auto" + the pin's rising-grammar check incl. the count=gpus-1 rule
  when several GPUs), engine.hip_ordinal (0..127), engine.uma; GpuOrder/GpuTurnPolicy enums;
  EngineLaunch 13 fields; describe 13 lines; engine_arguments VramGpuElasticArgs +
  append_vram_gpu_elastic_arguments (reserve unless operator owns it; elastic branch nests
  the segment; the split pair gated on several GPUs without --peer-device, never doubling
  operator flags) + effective_gpu_list (hip_ordinal wins over <=1 GPU, :3025; GLM exempt);
  launch_composition renders both paths from effective gpus and appends the family; parity
  destinations corrected (elastic trio/min_free/uma engine_arg -> frontend_behavior, Rulings
  T7-c/d); README regenerated (85 rows); auth04 allowlist +4 booleans.
- Existing expectations updated with citations (behaviour change per pin, not weakened):
  proc01_server_passes_literal_argv and managed_launch_renders_typed_gpu_list now carry
  "--layer-split auto" with 2 GPUs (server.py:2896-2899, :2938).
- RED evidence: compile failures — engine_arguments_test.cpp:384 "VramGpuElasticArgs is not
  a member", :387/:390/:396 "append_vram_gpu_elastic_arguments is not a member", :449-451
  "effective_gpu_list is not a member"; then runtime RED for gpu_and_elastic_rows and the
  two typed_config cases until the family composed/validated.
- INCIDENTS/fixes (caught by GREEN gates): (1) my own new test compared iterators from
  three separate compose({}) temporaries in one expect (UB) — rewritten with named values;
  (2) with_literals values for string rows need their JSON quotes (my accepts/rejects fixed).
- Ruling (T7-a): NO engine-ENV rows for this family — the pin reaches it via argv
  (--vram-reserve-mib runconfig.py:42; --vram-elastic/--vram-segment-mib :2943-2948;
  --layer-split :2938; --split-skip-if-fits :2942), runtime POST /v1/vram (:3832) and
  svc-side driver gates (:8083, :3105); SP2 §3.9 keeps the strata environment
  GPU-variable-free (the engine renders CUDA/HIP from --gpu itself, generate.cpp:2334-2352).
  Plan files-touched deviation (engine_environment.cpp untouched); plan test name
  "…reach_the_engine_env" became "…reach_the_engine_argv".
- Ruling (T7-b): engine.gpu_order enum auto|as_given; "auto" reordering needs driver speed
  scores (gpu_speed_scores/strata-device) — no probe on this host (owner GPU rule); both
  values render the configured order today; owner-box follow-up for Task 15 handoff.
- Ruling (T7-c): engine.min_free_vram_mib typed; its pre-load gate is a driver check —
  same no-probe limit; enforcement recorded for the owner's box.
- Ruling (T7-d): elastic/elastic_mib/elastic_idle_s/uma/gpu_policy typed now, consumed by
  the vision child / GPU-turn arbitration at Tasks 11-12 (image generation stays out, D-9).
- Ruling (T7-e): layer_split is a string row; the pin's JSON-list form (:2878) is
  wrong_type for us (C-2 style, cf T5-b); grammar is the pin's (:2875-2892).
- Ruling (T7-f): hip_ordinal reaches the engine through --gpu (pin used HIP_VISIBLE_DEVICES
  :3224; our design owns the device list).
- Ruling (T7-h): added caps: MiB rows 0/1..1048576, elastic_idle_s 1..86400 whole seconds,
  hip_ordinal 0..127; the pin's "0 == empty" falsy defaults (elastic_mib/idle) are NOT
  copied — typed accepts 1..cap.
- Gates: linux-gcc 0 warnings; serial ctest 768 run 100% (registered 772, 4 env skips;
  parked flake did not fire); release 0 warnings + 10/10; windows-llvm-mingw-cross 0
  warnings (wine/Windows runtime: not run on this host); pytest 25. No new threading ->
  no TSan spot this task.

## Task 6 record

- commit: `lifecycle: lazy/idle unload respawn on request; mtp=false drops the draft layer`.
  3 schema rows (69 -> 72 keys; engine.lazy_load bool false, engine.idle_unload_s int 0..86400
  default 0, engine.mtp bool default TRUE — draft_layer.py is_off needs an explicit false);
  EngineLaunch.lazy_load/idle_unload(seconds)/mtp; describe 3 lines; SupervisorConfig.lazy_start/
  idle_unload via subsystem_limits supervisor_config(); EngineSupervisor: SupervisorState::unloaded,
  KillReason::idle_unload, idle window armed at READY and on Lease release, cleared on acquire and
  request bracket (watch() checks it — state flips to unloaded under the lock so no Lease slips
  in; pidfd waits bounded while armed), unload_generation reuses the orderly stop (stdin EOF
  first), tracker banks health (not a failure), supervisor parks (wait_asleep) and wakes on
  wake_for_request(); note_request_started/finished bracket in ChatRoute::handle (RAII);
  GenerationGate: refusal_for(unloaded)=nullopt and the driver loop wakes + stays queued through
  the spawn it woke (woke_cold flag); engine_arguments build_engine_arguments gains trailing
  mtp_on=true and strips MTP_FLAGS verbatim {--mtp v, --spec-min-p v, --batch-mtp} (pin consumes a
  value only when the next token is not a flag; --spec stays), draft_layer_removed +
  strip_draft_layer_arguments exported; launch_composition sets ComposedLaunch.mtp_removed (expert
  via draft_layer_removed(config.engine.args), managed strips its composed args; GLM profile
  exempt); secure_server_main prints the pin's fixed line (server.py:2925); README regenerated
  (72 rows); parity rows updated (see rulings); auth04 allowlist += engine.lazy_load, engine.mtp.
- Tests (6 new): supervisor idle-teardown/respawn, idle-never-during-generation, lazy-defers-first-
  spawn (fake infra, ManualClock idle window), engine_arguments mtp_false_drops_the_mtp_arg,
  typed_config lifecycle_rows_match_the_pin, api_server idle_unload_respawns_before_the_next_request
  (1 s window: /health ok+"starting" proves the teardown, the supervisor stderr line
  `engine generation 1 ended: unloaded` + one GEN in the final record prove the fresh generation).
- RED evidence: compile failures — engine_supervisor_test.cpp:149 "SupervisorConfig has no member
  idle_unload", :158 "no member idle_unload_deadline", :160 "unloaded is not a member of
  SupervisorState", :163 "no member wake_for_request"; engine_arguments_test.cpp:356/365 "too many
  arguments to build_engine_arguments", :375 "draft_layer_removed is not a member"; plus
  typed_config/api_server compile failures for engine.mtp/lazy_load members and the lifecycle rows.
- INCIDENTS/fixes (caught by GREEN gates): (1) parked flake log_line_format_and_rate_limit failed
  the first full serial run; isolated re-runs 3/3 + two further full serial runs 100% — fingerprint
  match, untouched. (2) engine_arguments_test.cpp missed <algorithm> (std::none_of): libstdc++
  transitive include hid it, the windows-llvm-mingw-cross build caught it. (3) wake_for_request's
  first design split into two flags (asleep_/wake_pending_); wait_asleep only consumed one — the
  supervisor unit test caught the missed wake; single latch now. (4) the api harness rewrites the
  fake engine's --record-file at every spawn, so cross-generation GEN counting was wrong; switched
  to the supervisor stderr proof.
- Ruling (T6-b, pre-recorded): lazy_load composes NO engine flag — the pin's lazy skips the child
  start (server.py:7925 -> :966-993); parity row lazy_load destination engine_arg ->
  frontend_behavior (implementation evidence in the same commit).
- Ruling (T6-c, pre-recorded): wake-on-request is bounded by limits.concurrency.queue_timeout (the
  pin waits indefinitely in run()); a lazy first request that outlives the queue deadline gets the
  §5 503 timeout — owner-visible difference recorded for Task 15 docs.
- Ruling (T6-e): unloaded reports /health ok + "starting" (a load is pending) — the closed literal
  set from SP1 stays untouched (api_router's literal validator unchanged).
- Ruling (T6-f): the gate queues through a cold spawn only when it woke it (woke_cold); a crash-
  recovery `starting` still refuses fast, as SP2 established. lazy+vision conflict rule deferred
  to Task 11 (runconfig.py:97-98).
- Ruling (T6-g): engine.idle_unload_s is whole seconds (pin num>=0, "0 or empty: never"); the
  86400 cap is added (C-1). Parity kRows 120 -> 121: `mtp` gets an extra rule row (the Task-2
  inventory greps miss draft_layer.is_off; the row keeps the spine complete).
- Gates: linux-gcc 0 warnings; serial ctest 764 run 100% (registered 768, 4 env skips); release
  0 warnings + 10/10; cross 0 warnings (wine/Windows runtime: not run on this host); pytest 25.
  TSan spot (manual out/build/tsan, -fsanitize=thread -fno-pie; runs need `setarch $(uname -m)
  -B -R` on this kernel — "unexpected memory mapping" otherwise): 6 supervisor cases
  (3 new lifecycle + serving-api-thread-safe + wake-on-release + shutdown-eof) all exit 0,
  0 ThreadSanitizer warnings.

## Task 5 record

- commit: `reasoning: effort/budget/recovery typed; fit_max_tokens honours the pin's two behaviours`.
  7 schema rows (62 -> 69 keys; reasoning.effort/budget_tokens/close_retry/loop_recovery/
  effort_position/anthropic_thinking + engine.fit_max_tokens default true); ReasoningConfig + 4 enums
  in frontend_config.hpp; FrontendConfig::reasoning; EngineLaunch.fit_max_tokens; describe 8 lines;
  chat_execution resolve_max_tokens gains trailing fit_max_tokens=true (false + explicit over-room
  -> nullopt 400, pin :4526-4544; max_completion cap still clamps B2); chat_route passes
  config_.fit_max_tokens at both admission sites; ChatRouteConfig.fit_max_tokens wired in main;
  engine_arguments compose_effort_end_arguments (binary-literal probe with overlap-window scan +
  tokenizer single-"system" encode, pin :2836-2856; fixed pin notices); secure_server_main appends
  the composed --tail-role-token when effort_position=end; parity row effort_position destination
  config_only -> engine_arg; README regenerated (69 rows).
- RED evidence: compile failures — chat_execution_test.cpp:375 "too many arguments to function
  resolve_max_tokens(...)"; engine_arguments_test.cpp:316/324/335 "compose_effort_end_arguments is
  not a member"; typed_config_test.cpp:1011 "FrontendConfig has no member named reasoning" — then
  runtime RED for api_server: temporarily reverting the chat_route fit args gave "FAILED: 400 like
  the pin's over-room refusal (:4536-4543)" + "FAILED: no GEN is written when fit_max_tokens is
  false"; GREEN after restore.
- INCIDENTS/fixes (caught by GREEN gates): (1) kSchema declared +9 rows but 7 added -> two
  zero-initialized phantom rows rendered into the README table; fixed the count (43+7+3+9+7).
  (2) engine_arguments lib now calls encode_text -> CMake target_link_libraries added
  secure_frontend_tokenizer_pack PUBLIC (link failures surfaced). (3) ASan exposed a latent
  use-after-free when config_of(with_literals(...)) is passed a temporary (dangling view into the
  destroyed result): fixed in MY Task-4 and Task-5 cases (results kept alive). Pre-existing
  occurrences elsewhere in typed_config_test left untouched (out of task scope; flagged here).
  (4) auth04_no_switch_disables_auth (SP1 auth allowlist) names every boolean row: extended per its
  own per-milestone allowlist pattern (see ruling T5-d) — the forbidden-name scans unchanged.
- Ruling (T5-a): the plan's effort vocabulary "none|low|high|max" contradicts the pin: config-level
  reasoning_effort is validated at clean_shared_defaults serve/server.py:7341-7343 to
  none|low|medium|high. Pin wins (C-1); "max" is rejected by our row too.
- Ruling (T5-b): the plan wrote loop_recovery "(off|retry)": the pin's vocabulary is false|"stop"|
  "recover"|true (:8129-8136). Typed row names the three string words off|stop|recover (default
  off); the pin's bool forms are wrong_type for us (typed-namespace strictness, C-2 style).
- Ruling (T5-c): no chat_generation_plan.cpp change — the fit decision belongs to admission
  (chat_route resolve_max_tokens sites), and the other reasoning rows are service-object values the
  pin never sends in argv/env (evidence: grep over the pin's arg/env composition). Files-touched
  deviation from the plan text, evidence-driven.
- Ruling (T5-d): auth04 allowlist extension — engine.fit_max_tokens and reasoning.close_retry are
  file-only behaviour booleans (no CLI flag, no auth surface); the test's own design expects each
  milestone to justify new boolean rows by name; forbidden-in-path/flag/env-name scans untouched.
- Wire destinations proven in tests with citations: effort_position end -> engine arg only; the
  other six reach nothing in argv/env (pin lines cited in engine_arguments_test).
- Gates: linux-gcc 0 warnings; serial ctest 758 run 100% (registered 762, 4 env skips; no parked-
  flake hit this run); release 0 warnings + 10/10; cross 0 warnings (wine/Windows runtime: not run
  on this host); pytest 25.

## Task 4 record

- commit: `sampling: defaults match v0.2.1.3 validator and reach GEN` (sampling_params.hpp
  SamplingParams extended to the 9 pin keys + SamplingDefaults; frontend_config schema +9 rows
  (43+7+3+9=62 keys; ConfigValueType::decimal; ConfigKey gained minimum_decimal/maximum_decimal/
  default_decimal(NaN=unset)/minimum_exclusive with NSDMIs so pre-Task-4 positional rows stay
  valid; DraftReader decimal()/optional_integer(); validate() fills FrontendConfig::sampling;
  describe() 9 lines "(unset)" when absent); config_file.cpp allow_fractions + 
  walk_fraction_outside_decimal; chat_generation_plan merge_sampling (request wins, unset stays
  unset, seed widening cast exact — rows validated positive/32-bit); engine_protocol_client
  render_sampling_keys rewritten to the pin wire rules; chat_route ChatRouteConfig::sampling_default
  + stream_response include_usage param; secure_server_main wires config.sampling.)
- RED evidence (re-captured after the mid-task cancellation via surgical revert of the merge,
  the render rewrite and the schema rows; originals lost with the cancelled turn):
  engine_protocol: "FAILED: the pin's key names and default window: GEN 16 temperature=0 top_p=1
  1,2,3" + "top_k clamp: " + "penalty alias + default window: " + "explicit window: ";
  typed_config: "FAILED: schema row exists: defaults.sampling.temperature" (+ per-row unknown-key
  lines, segfaulted after null-deref on `set` in RED-only state); chat_plan: "FAILED: the config
  default fills top_k" + "the config default fills seed". GREEN after restore+fixes.
- INCIDENT (caught by GREEN gate, test never weakened): cfg01_type_confusion_rejected failed for
  EVERY integer key — the global allow_fractions=true let "8080.0"/"1e3" parse as doubles that then
  hit wrong_type instead of the contracted "config file" malformed_document. Fix keeps the parser
  permissive but adds the schema-aware walk_fraction_outside_decimal gate before excluded-name and
  unknown-key detection (same precedence the old parse-time failure had).
- Ruling (T4-a): the plan's Task-4 citation "serve/server.py:3275-3318" for sampling is wrong at
  the pin: the wire rules live in `sampling_keys` (serve/server.py:1360-1394) and the config
  bounds in `clean_shared_defaults` (serve/server.py:7331-7367). Cite the real lines.
- Ruling (T4-b): existing expectation render_sampling_keys(temperature=1,top_p=1)==" temperature=1
  top_p=1" changed to " temperature=1": the pin omits top_p=1 (the engine default), and pin parity
  is the task. The test's other asserts are untouched; the SP3 "bounded keys" intent stands.
- Ruling (T4-c): typed_config_test's local switches (valid_sample/wrong_type_samples) extended for
  the decimal enum (midpoint valid sample; wrong-type sweep "1.5"/true/[1]/null) — an adaptation to
  the new type, not a weakening.
- Wire rules implemented verbatim from the pin (:1360-1394): temperature only >0; top_p only <1;
  top_k: skip <0, else 1..64 as-is else 64; min_p only 0<x<=1; penalty_repeat !=1, penalty_freq
  !=0, penalty_present !=0; any penalty on -> penalty_last_n value>0 else 64; seed only >0; in that
  order. Number text = shortest round-trip (std::to_chars).
- Gates: linux-gcc build 0 warnings; serial ctest run A 753/754 (parked flake
  log_line_format_and_rate_limit failed — isolated re-runs 4/5, fingerprint match; re-run B:
  754/754 100%, registered 758, 4 env skips); release 0 warnings + 10/10; cross 0 warnings
  (wine/Windows runtime: not run on this host); pytest 25. README config table regenerated
  (62 rows + header; readme_config_table_matches_schema green).

Ruling (T4-d): decimal config rows are read as double; integer draft values widen exactly. A
decimal row receiving an integer literal (e.g. seed-style "1") is fine at the reader; the
fraction gate only rejects fraction/exponent SYNTAX outside decimal rows — identical to the pin's
behavior (clean_shared_defaults coerces int to float ranges; the pin's wire only ever sees floats).
Why: keeps SP1's cfg01 contract byte-for-byte while letting the 6 decimal rows through. Cost if
wrong: over-rejection of valid configs — the typed_config accept rows cover each bound.

## Task 3 record

- find_rejected_key (config_file.hpp/.cpp): document-order JsonValue walk; typed schema
  key/section paths never match (typed wins over the flat name — engine.lib_dirs stays valid).
  parse_config_text pre-scans before unknown-key detection; new ConfigIssueCode::excluded_key,
  fixed reason names the SP5 parity table + threat model; no value echoed. Server and strata
  both load through this path.
- doctor: engine_env_knob_present() scans the process env for STRATA_/MAYA_ prefixes
  (POSIX environ; Windows GetEnvironmentStringsW); fixed line
  "doctor: ignoring STRATA_*/MAYA_* environment; configure server.json instead" once.
- RED evidence: typed_config: "FAILED: config with excluded key 'api_monitor' must fail at
  load naming the key: api_monitor: unknown key;"; privacy: "FAILED: exactly one fixed
  env-knob line, got 0: landlock: ABI 4". GREEN after.
- New cases: each_rejected_upstream_key_errors_at_load_naming_it (+ nested + typed-name
  collision sub-asserts), doctor_warns_on_strata_env_knob (windows-runtime label like its
  proxy-hint sibling; Linux run green; Windows: not run on this host).
- Gates: linux-gcc build 0 warnings; ctest 751/751 (registered 755), serial; release 0 warnings
  + 10/10; cross 0 warnings; pytest 25.

Ruling: rejected-name matching is by member name at any depth, but a path the schema knows
(key or section) wins — spec §4 rejects "raw env dict env"/lib_dirs etc. while our own typed
engine.lib_dirs/defaults.sampling stay valid. Why: C-2 typed namespace conventions + D-3
by-name rejects; cost if wrong: false rejections of typed keys (caught by the collision
sub-assert).

## Task 2 record

- Generated inventory: 87 config keys + 31 env knobs (grep over upstream/serve/*.py at
  cbfd5b92; spec's "~35" was an estimate — grep is authoritative; diff-checked verbatim).
- Table: 120 rows = 87 keys + 31 knobs + 2 extra rule rows (mcpServers rejected @mcp.py:648;
  vram_reserve_mib typed @runconfig.py:42). Meta-tests: every_upstream_key_has_exactly_one_disposition,
  every_env_knob_is_dispositioned, typed_rows_name_a_destination (+ unique-name + rule-citation checks).
- RED evidence: `FAILED: config key 'aliases' must have exactly one parity row`
  (ctest #404, empty-table stub; 2 of 3 cases failed before the table).
- GREEN: 3/3; full suite 749 run 100% (registered 753); release 10/10 + 0 warnings;
  cross 0 warnings; pytest 25.
- First full run had the parked flake fail (1/749); second serial run 749/749; the flake
  passed 9/10 in isolation afterwards. Same fingerprint as the owner-parked
  api_server_tests.log_line_format_and_rate_limit; not touched, not weakened.
- Windows runtime/wine/CI: not run on this host.

Ruling: images.py at the pin is SD image *generation* (sd-cli), not vision input; the flat
cfg.get grep mixes block keys (vision dict, images dict, mcp entries, replica entries).
Generation-only names (sigmas, steps, out_dir, images, reload, quant, native, model_name,
tokenizer, arch, backend, api_key) are mapped_away with the governing rule cited; the three
names the plan re-uses as vision input caps (max_n, max_side, need_mib) are typed as vision.*
with the pin's images-block read site cited and "cap lives on the images block" noted.
Why: C-1/D-9 binding; cost if wrong: table rows contradict the pin.

Ruling: the plan's Task 11 citation "serve/server.py:3378-3418" for the vision block form is
wrong at the pin — that range is stop_strings/StopMatcher; Vision.reader_args is
server.py:2557-2570 (exe/--mmproj/--model/--gpu/--threads/--max-tokens/--min-tokens).
Tasks 11-12 cite the pin's real lines. Why: C-1; cost if wrong: false citations.

Ruling: env knobs get their own table rows (31) because their names differ from the config
keys; "typed" on an env row means its semantics become a typed config row; raw env input is
never honored (spec §4) — Task 3's doctor warning proves that. Rejected rows must cite a spec
rule ("spec §4…" or "spec C-4…") — enforced in the meta-test. Why: D-3 auditable rejects;
cost if wrong: vacuous reject rows.

Ruling: `upstream_config_keys()`/`upstream_env_knobs()` live in src/parity_table.cpp and
include the tests/-generated header by relative include — the plan places the generated file
under tests/ and the accessors in the library. Why: plan interfaces block; cost if wrong: none.

## Baseline reproduced at Task 1 (HEAD 60f94aeb, this host)

- build linux-gcc: 0 warnings (`grep '(warning|error):'` empty)
- ctest --preset linux-gcc: **746 run, 100%**, 4 skipped (cfg03_rejects_mount_crossing,
  proc03_other_uid_cannot_open_engine_fd, proc05_cgroup_kill_when_delegated,
  bundle_at_mount_root_is_read_but_never_written); 750 registered; 164.9 s
- linux-gcc-release: build 0 warnings; ctest **10/10**
- windows-llvm-mingw-cross: build 0 warnings (compile-only; wine/MSYS2/CI: not run on this host)
- pytest -q: **25 passed**
- Windows runtime / wine / CI steps for every task below: "not run on this host" (owner ruling 2026-10-20).

## M1 seams (Task 1 inventory; all present)

- `config_schema()` — include/secure_frontend/frontend_config.hpp:41; schema array `kSchema` src/frontend_config.cpp:271 (definition above)
- `ConfigKey` — frontend_config.hpp:28; `ConfigKeyExtension` — :44; `find_config_key_extension` :51, `kSchemaExtensions` src/frontend_config.cpp:279
- engine_arguments compose site — `build_engine_arguments` include/secure_frontend/engine_arguments.hpp:52
- `EngineFamily` — include/secure_frontend/engine_environment.hpp:19; `FamilyEnvironmentRows` :64; `FamilyEnvironmentHook` :71; rows fn :86; `merge_forced_environment` :94; supervisor default hook engine_supervisor.hpp:287
- `EngineSupervisor` restart policy — `RestartTracker tracker_` engine_supervisor.hpp:243; `RestartPolicy` include/secure_frontend/restart_policy.hpp:12; generation-end class :224
- `resolve_max_tokens` — chat_execution.hpp:73, chat_execution.cpp:35; call sites chat_execution.cpp:94, chat_route.cpp:70
- sampling GEN keys — GEN line built src/engine_protocol_client.cpp:211 (`"GEN " + max_new + render_sampling_keys(...)`); `render_sampling_keys` :97 (keys temperature/top_p/seed, SP3 B6); `SamplingParams` include/secure_frontend/sampling_params.hpp:12; request parsing src/chat_schema.cpp:223
- `verb_doctor` — src/strata_cli_network.cpp:323; dispatch :361
- `FileRole` — include/secure_frontend/acquire_manifest.hpp:19 (`weights, pack, tokenizer`)
- `ManifestLaunch` — acquire_manifest.hpp:37

## Rulings (this lane)

(none yet)

## Host adaptations (binding)

- No CI, no wine, no MSYS2; GitHub storage only; lane never pushes.
- Per-task gates (ALL, synchronously in-turn — never background-and-pause):
  linux-gcc build (0 warnings) + `ctest --preset linux-gcc` (serial, ports 20000–20099) +
  **linux-gcc-release build** + windows-llvm-mingw-cross build (0 warnings) + `pytest -q`.
- TSan available: `clang`/`clang++` wrappers in `.venv-dev/bin` (see AGENT_HANDOFF
  "Host toolchain 2026-10-21"); TSAN_OPTIONS=external_symbolizer_path=<repo>/.venv-dev/bin/llvm-symbolizer.
- Commits: author `shift-f10 <40450442+shift-f10@users.noreply.github.com>`, literal `Co-Authored-By: <real model> <noreply@anthropic.com>`, NO `Claude-Session:` trailer.
- Never push/merge/rebase/switch branches; nothing outside this worktree; `upstream/` read-only;
  no rm -rf / git clean / bypass flags; no installs.
- umask 0002 → make_scratch_directory/make_private_directory; system_clock is ns-based.
- Task briefs: hand-written into this workspace dir (no scripts/task-brief here).

## Baseline to reproduce at Task 1 (measured by controller at merge c83119a9 lineage)

- ctest --preset linux-gcc: 746 run, 100% (750 registered; 4 allowed env skips:
  proc03_other_uid, proc05_cgroup_delegated, cfg03_mount_crossing, bundle_at_mount_root)
- release preset ctest 10/10; pytest 25; cross 0 warnings.
- Known parked flake: api_server_tests.log_line_format_and_rate_limit (pre-existing M1
  token-bucket timing; fails only under heavy load; never weaken or delete it).
