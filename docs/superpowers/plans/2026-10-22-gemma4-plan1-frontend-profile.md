# Gemma 4 Plan 1 — the `gemma4` frontend profile — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `strata-secure-server` accepts `engine.profile = "gemma4"` and, against an engine that speaks the `strata_0143` stdio protocol, renders Gemma 4's chat template byte for byte, tokenizes with Gemma's SentencePiece-style BPE, splits the thought channel into `reasoning_content`, stops on Gemma's end-of-turn ids, and maps `reasoning_effort` to thinking on/off — proven end to end on the fake engine. No engine is built here (plans 2-3); Gemma reaches the 8080 UI in plan 4 (spec D12).

**Architecture:** a third chat profile beside `qwen` and `glm53`, threaded through the same seams SP6 used (config, manifest, launch composition, supervisor, chat plan, route), with every two-way profile ternary turned into an exhaustive `switch` first. The tokenizer gains an explicit mode chosen by the profile (never sniffed from the files). The reasoning split reuses `GenerationRunLimits::think_tokens` and gains one rule, the channel-name line.

**Tech Stack:** C++20, stdlib + OS only; MSVC `/W4 /WX` (`windows-msvc` preset) on the owner's box; `linux-gcc`, `linux-clang`, `windows-llvm-mingw-cross` where available; pytest (`.venv-dev`) for `tools/secure`.

**Spec:** `docs/superpowers/specs/2026-10-22-gemma4-engine-design.md` (§1.1 goal 0, §2.5, §2.6, §6, §7; decisions D1-D12). Facts below cite the 2026-10-22 survey of the frontend; line numbers are at `5531a53d`.

## Reconciliation with the code (binding corrections to the spec)

- **No per-family executable key exists.** The server always spawns the single
  `engine.executable` (`frontend_config.cpp:119, 926-934`; `server_bootstrap.cpp:240`);
  `engine.executables.glm` exists only in specs. Plan 1 therefore keeps **expert mode**:
  `engine.profile = "gemma4"` with `engine.executable` pointing at the Gemma engine. The
  spec's `engine.executables.gemma4` and the family-to-executable table move to plan 4
  (managed launch). Spec §6.1 is amended accordingly (Task 12).
- **glm53 is expert-only.** `engine_gate.cpp:8` refuses every manifest but qwen/strata and
  `compose_engine_launch` hardcodes qwen (`launch_composition.cpp:184, 235`). Plan 1 lets a
  `gemma4` manifest parse and validate, and keeps `engine_gate` refusing it
  (`engine_profile_unavailable`) until plan 4.
- **Two-way ternaries** select by profile at `frontend_config.cpp:942-947, 1339-1341`,
  `launch_composition.cpp:36-37, 41-42, 49, 70`, `server_bootstrap.cpp:191, 210, 217-222,
  232, 235-236`, `subsystem_limits.cpp:52, 55-56`, `chat_execution.cpp:80-82`,
  `chat_route.cpp:152-156`, `acquire_manifest.cpp:248-251`. A third value compiles and
  silently takes the GLM (or qwen) branch. Task 1 converts each to an exhaustive `switch`
  with no `default:`, so GCC/Clang `-Wswitch` (in `-Wall`) flags a missed profile.
- **The tokenizer loader is not told the profile** (`server_bootstrap.cpp:192`,
  `tokenizer_pack.cpp:210-243`). Task 4 adds the mode parameter.
- **The `glm.*` guard is `profile == qwen`** (`frontend_config.cpp:1178`); it would let
  `glm.*` rows through on `gemma4`. It becomes `!= glm53`.
- **The qwen effort-end tail** (`server_bootstrap.cpp:217-222`) is composed for any profile;
  it is gated to qwen.
- **Found, out of scope:** the page always sends `reasoning_effort` (`web/app.js:426`), which
  the glm53 schema refuses (400) — the built-in page cannot chat with a GLM model today.
  Recorded as a session-log issue for the owner; plan 1 does not change glm53.
- `tools/secure/make_bundle.py` is qwen-only (`:155-156`); bundles are not part of plan 1.

Rulings taken during execution (2026-10-22; each also in its task's commit message):

- **Task 2:** a gemma4 manifest's `launch.tokenizer_pack` is any directory of listed files (qwen's
  `<pack>/tokenizer` rule cannot apply: gemma4 has no `launch.pack`) — why: the frontend pack is
  the only launch directory — cost if wrong: a manifest could name a non-tokenizer directory; the
  publish layout check (Task 4) and the loader refuse it. The publish-time gemma4 layout check
  waits for Task 4's `spm_bpe` mode (the engine gate refuses gemma4 meanwhile).
- **Task 3 (controller):** the Gemma engine takes the model as `--native <gguf>` like the strata
  engine (spec §4.8 says `--model`; amended in Task 12) — why: upstream's q36 engine and the
  engine being written accept it, and `--native` stays frontend-owned — cost if wrong: one flag
  spelling in `build_engine_arguments`.
- **Task 3:** on an explicit gemma4 profile the config refuses by name the rows the qwen launch
  turns into engine flags (`engine.vram_reserve_mib`, `engine.vram_elastic`,
  `engine.vram_segment_mib`, `engine.layer_split`, `engine.split_skip_if_fits`,
  `session.expert_profile_save(_every)`, `replicas.parallel`, `engine.mtp`,
  `reasoning.effort_position`) — why: fail closed instead of silently ignoring them — cost if
  wrong: an operator deletes a row that had no effect.
- **Task 3:** `chat_route.cpp`'s plan ternary became a switch here (not in Task 10) — why: once
  `NativeChatProfile::gemma4` exists the ternary would send gemma4 requests to the GLM plan —
  cost if wrong: none.
- **Task 10:** the route's plan selection is a public `select_chat_generation_plan` (both plan
  sites) — why: the RED cases need it at unit level — cost if wrong: none. `chat_schema_limits`
  derives the per-request `reasoning_effort` flag from the configured profile (the bootstrap
  still re-derives it from the launch) — cost if wrong: none at serve time.
- **Task 12 (2026-10-23):** spec amended in place with dated notes: §4.8 `--native` (Task 3's
  ruling), §6.1 the per-family executable key and the engine gate opening → plan 4, the status
  line. No schema text changed after Task 1, so the README table needed no regeneration
  (`readme_config_table_matches_schema` passes).
- **Execution note (2026-10-22):** Tasks 8, 9 and 11 were written by parallel workers in
  detached worktrees and integrated here in plan order (`git apply --3way`); the full suite ran
  per batch (after 7-9, after 10-11, after 12) by the controller's gate change.

## Global Constraints

- TDD: every task's RED tests are written and seen failing for the right reason before the
  change; a test is never weakened. `-Werror`/`/WX` everywhere. `upstream/` is never modified.
- Per-task gates on this box: `windows-msvc` build (0 warnings), `ctest --preset windows-msvc`
  (serial, full), `pytest -q` for the Python task. Linux and cross gates run where the owner
  runs them; no Windows claim beyond the MSVC preset.
- No engine code and no GPU work in this plan. The fake engine (`supervision_fake_engine_main.cpp`,
  `--token-ids=…`) stands in for the Gemma engine; it already speaks `strata_0143`.
- The Gemma 4 GGUF on the box (`C:\Strata-data\models\gemma4_q8\…UD-Q8_K_XL.gguf`) is read
  header-only and only by Task 7; nothing from it is committed (its vocabulary is 32 MB and
  the model's licence terms apply). Fixtures commit token **ids and texts** only.
- Commits on a branch `gemma4-plan1-frontend`, never pushed by the lane; author
  `shift-f10 <40450442+shift-f10@users.noreply.github.com>`; the two literal trailers of `CLAUDE.md`. Update
  `docs/AGENT_HANDOFF.md` in the commit that changes behaviour.

## Review Focus

1. **A third profile taking a two-way ternary's wrong branch** — every site above becomes an
   exhaustive switch; a test per site proves `gemma4` takes its own path.
2. **The tokenizer mode inferred from files** — the mode comes from the profile only; a
   `gemma4` profile with a `merges.txt` pack, or a qwen profile with `merges.json`, fails at load.
3. **A Gemma control literal in client text reaching the prompt as a control id** — every
   marker in user, system or assistant text fails closed (renderer), and `encode_text`
   refuses type 3/4 literals as today.
4. **The thought channel** — the channel-name line is exactly `thought\n` and is dropped;
   anything else ends the reply as `undecodable`; tags are never output.
5. **qwen or glm behaviour leaking onto gemma4** — no qwen effort sentence, no effort-end
   tail, no `glm.*` rows, no MTP or VRAM flags on a `gemma4` launch.
6. **`trim` fidelity** — Jinja's `trim` is Python `str.strip()`: every code point with
   `str.isspace()` true, both ends; the C++ set is a fixed table tested against Python.
7. **Merges with `\n`/`\t`** — `merges.json` round-trips them; a line-based reader is never
   used for `gemma4`.

---

## Task 1: Exhaustive profile switches and the `gemma4` value in config

**Files:** `frontend_config.{hpp,cpp}`, `server_cli.cpp` (help text), `engine_arguments.{hpp,cpp}`;
tests `typed_config_test.cpp`, `engine_arguments_test.cpp`, `server_cli_test.cpp`.

**Interfaces:**
- Produces: `EngineProfile::gemma4`; `EngineArgumentsProfile::strata_gemma4`;
  `engine.profile` accepts `"gemma4"`; `--check` describes it; the `strata_gemma4` user-argument
  allowlist (spec §4.8): `--max-context N`, `--chunk N`, `--kv f16|f32`, `--no-graph`,
  `--expert-vram GiB`, `--expert-slots N`, `--hybrid-rows N`, `--cpu-threads N`; everything
  else refused by name.

- [x] **Step 1: RED**
  ```cpp
  void engine_profile_accepts_gemma4_and_describes_it();          // parse + --check line "engine.profile = \"gemma4\""
  void glm_keys_rejected_on_gemma4_profile_by_name();             // the :1178 guard becomes != glm53
  void gemma4_user_arguments_follow_the_strata_gemma4_allowlist(); // each allowed flag accepted with bounds; --mtp, --batch, --vision, unknown refused by name
  void every_profile_selects_its_own_argument_profile();          // qwen→strata_0143, glm53→lighttransport_glm53, gemma4→strata_gemma4
  ```
  plus the `server_cli_tests` help text naming `qwen, glm53 or gemma4`. Run → FAIL.
- [x] **Step 2:** add the values; convert `frontend_config.cpp:942-947` and `:1339-1341` to
  exhaustive switches; implement the allowlist in `engine_arguments.cpp` beside
  `lighttransport_glm53`'s; update the `engine.profile` schema row text and regenerate the
  README table (`out/regen-readme-table.ps1` pattern; `readme_config_table_matches_schema`).
- [x] **Step 3:** GREEN + gates; commit `config: engine.profile gemma4 and the strata_gemma4 argument profile`.

---

## Task 2: Manifest profile and family; the engine gate keeps refusing

**Files:** `acquire_manifest.{hpp,cpp}`, `manifest_json.cpp`, `engine_gate.cpp`,
`acquire_pull.cpp` (`runnable_text`), `strata_cli.cpp` (list/show text); tests
`manifest_test.cpp`, `model_index_test.cpp`, `acquire_cli_test.cpp`.

**Interfaces:**
- Produces: `ManifestProfile::gemma4`, `ManifestFamily::gemma4`; JSON `"profile": "gemma4"`
  with `"engine": {"family": "gemma4"}`; launch keys for gemma4 = qwen's text shape
  (`native`, `tokenizer_pack`; `mmproj` and `ple_gguf` refused, D2); `engine_gate` →
  `engine_profile_unavailable` for gemma4 with a fixed reason ("the Gemma 4 engine is not
  installed").

- [x] **Step 1: RED**
  ```cpp
  void manifest_accepts_gemma4_profile_with_gemma4_family();     // round trip; mismatched family refused by name
  void gemma4_launch_keys_are_text_only();                       // mmproj/ple_gguf/pack refused for gemma4
  void engine_gate_refuses_gemma4_until_the_engine_exists();     // list/show print "engine not available"
  ```
  extend `manifest_field_rules_matrix` and `launch_keys_follow_profile` rows. FAIL.
- [x] **Step 2:** implement; `acquire_manifest.cpp:248-251` becomes a switch; the `:342-375`
  launch-key switch gains a `gemma4` case that does not fall through to qwen's.
- [x] **Step 3:** GREEN + gates; commit `manifest: gemma4 profile and family (gated until the engine exists)`.

---

## Task 3: Launch, supervision and bootstrap wiring

**Files:** `launch_composition.cpp`, `subsystem_limits.cpp`, `server_bootstrap.cpp`,
`chat_execution.{hpp,cpp}` (`NativeChatProfile::gemma4`); tests
`engine_arguments_test.cpp`, `subsystem_limits_test.cpp`, `chat_execution_test.cpp`.

**Interfaces:**
- Produces: an expert `gemma4` launch composes its argv from the allowlist only (`--serve`
  is still inserted by `EngineStdioProcess`; no VRAM, profile-save, batch or MTP flags:
  `:49, :70` gated to qwen); GPUs as qwen
  (single `--gpu`, HIP override unchanged); `supervisor.profile = NativeChatProfile::gemma4`,
  `flavor = strata_0143`; environment family `strata`; the effort-end tail (`:217-222`) only
  for qwen; `per_request_reasoning_effort` true for qwen **and** gemma4.

- [x] **Step 1: RED**
  ```cpp
  void gemma4_expert_launch_composes_only_allowlisted_flags();  // no --expert-vram unless given; no MTP strip, no effort tail
  void supervisor_profile_and_flavor_for_gemma4();               // gemma4 → NativeChatProfile::gemma4 + strata_0143
  void executes_gemma4_only_when_explicitly_selected();          // chat_execution:80-82 switch; a gemma4 request never takes the glm53 plan
  ```
  FAIL.
- [x] **Step 2:** convert `launch_composition.cpp:36-37, 41-42, 49, 70`,
  `subsystem_limits.cpp:52, 55-56`, `server_bootstrap.cpp:191, 210, 217-222, 232, 235-236`
  and `chat_execution.cpp:80-82` to exhaustive switches; add the gemma4 cases (the plan
  call is a stub returning `invalid_request` until Task 8).
- [x] **Step 3:** GREEN + gates; commit `launch: gemma4 expert launch, supervisor profile and strata_0143 flavor`.

---

## Task 4: Tokenizer mode and the `merges.json` pack

**Files:** `tokenizer_pack.{hpp,cpp}`, `server_bootstrap.cpp:192`, `acquire_publish.cpp:39`
(qwen stays byte mode); tests `tokenizer_pack_test.cpp`.

**Interfaces:**
- Produces:
  ```cpp
  enum class TokenizerMode { byte_bpe, spm_bpe };
  struct TokenizerPack { …; TokenizerMode mode{TokenizerMode::byte_bpe}; };
  TokenizerPackLoadResult load_local_tokenizer_pack(const CanonicalRoot& root, TokenizerMode mode);
  ```
  `byte_bpe` reads `merges.txt` (unchanged) and refuses a pack that also has `merges.json`;
  `spm_bpe` reads `merges.json` — a JSON array of two-string arrays in rank order, parsed with
  its own `JsonLimits` (depth 2, entries ≤ `kMaximumVocabularyEntries` per container,
  64 MiB file cap as `read_required`) — and refuses a pack with `merges.txt`. Each side must be
  a vocabulary token; duplicates refused. The bootstrap passes the mode from `launch.profile`
  (gemma4 → `spm_bpe`, others → `byte_bpe`). Gemma's real pack fits the existing caps
  (`tokenizer_pack.cpp:21-22`: 1,048,576 vocabulary entries and merges, 64 MiB a file; the
  Q8 GGUF has 262,144 tokens and 514,906 merges).

- [x] **Step 1: RED**
  ```cpp
  void spm_pack_loads_merges_json_with_newline_and_tab_tokens();  // "\n\n \n", "\t \t", "▁▁ ▁" merges round-trip
  void mode_comes_from_the_profile_never_the_files();              // spm with merges.txt → error; byte with merges.json → error
  void merges_json_is_bounded_and_validated();                     // depth 3, non-string, unknown token, duplicate, oversize → typed errors
  ```
  FAIL.
- [x] **Step 2:** implement; error codes reuse `invalid_merge` / `invalid_json` with fixed messages.
- [x] **Step 3:** GREEN + gates; commit `tokenizer: spm_bpe mode and merges.json, chosen by the profile`.

---

## Task 5: `spm_bpe` encode and decode

**Files:** `tokenizer_pack.{hpp,cpp}` (`encode_text`, `TokenByteDecoder`, `decode_token_ids`);
tests `tokenizer_pack_test.cpp`; a small synthetic pack builder in
`tests/support/test_tokenizer.hpp` (`spm_tokenizer()`: `▁`, the 256 `<0xXX>` tokens as BYTE
type 6, a few merges, the Gemma markers as CONTROL/USER_DEFINED).

**Interfaces:**
- Produces, in `spm_bpe` mode (LC `llama-vocab.cpp:528-537, 637-644, 3186-3200`):
  encode — replace every U+0020 with U+2581; split into runs `[^\n]+` and `[\n]+`; a
  newline-only run that is a vocabulary token is that token; otherwise symbols are whole UTF-8
  code points, merged by lowest rank repeatedly; a final symbol not in the vocabulary becomes
  its bytes' `<0xXX>` tokens; no BOS (the renderer adds it); type 3/4 literals in text refused
  as today. Decode — `▁` → space, `<0xXX>` (token type 6, BYTE; the Q8 GGUF has 256 of them
  beside 17 CONTROL and 7 USER_DEFINED) → that byte (a split code point is resolved by
  `Utf8StreamDecoder` as today); CONTROL/USER_DEFINED fail closed as today. In `byte_bpe`
  mode a type-6 token stays refused (no GPT-2 pack has one).

- [x] **Step 1: RED**
  ```cpp
  void spm_encodes_spaces_as_u2581_and_merges_by_rank();
  void spm_newline_runs_are_single_tokens_when_known();
  void spm_unknown_characters_fall_back_to_byte_tokens();   // emoji, combining marks, C0 controls
  void spm_round_trips_the_upstream_corpus();               // the corpus of upstream tools/strata_tokenizer.py's self-test
  void spm_decoder_maps_u2581_and_byte_tokens();
  ```
  FAIL.
- [x] **Step 2:** implement (a heap-ordered merge like the byte path; no regex).
- [x] **Step 3:** GREEN + gates; commit `tokenizer: spm_bpe encode/decode (Gemma 4)`.

---

## Task 6: The Python pack tool and reference encoder

**Files:** `tools/secure/secure_tokenizer.py`, new `tools/secure/spm_bpe.py` (reference
encoder), tests `tools/secure/test_gemma4_tokenizer.py`.

**Interfaces:**
- Produces: `secure_tokenizer.py --profile gemma4 --gguf F --out D` accepts only
  `tokenizer.ggml.model == "gemma4"` (any other model with `--profile gemma4` is refused, and
  `gemma4` without the flag is refused) and writes `D/tokenizer/{vocab.json, merges.json,
  token_type.json, tokenizer.json}` (the last records `model`, `add_bos`, `add_space_prefix`,
  the special ids and the source GGUF's SHA-256 of its header bytes). `spm_bpe.Encoder` mirrors
  Task 5's rules in pure Python (the oracle for Task 7).

- [x] **Step 1: RED** — pytest: a synthetic GGUF header written by the test (stdlib `struct`,
  like `test_glm_tokenizer.py`) with a tiny gemma4 vocabulary; extraction writes the four files;
  `merges.json` keeps `\n`/`\t` merges; profile/model mismatches refused; the reference encoder
  matches hand-computed ids. FAIL.
- [x] **Step 2:** implement (the GGUF reader stays header-only, our own code).
- [x] **Step 3:** GREEN + `pytest -q`; commit `tools: gemma4 tokenizer packs and the spm_bpe reference encoder`.

---

## Task 7: Real-vocabulary agreement (box, no commit of model data)

**Files:** `secure_frontend/tests/fixtures/gemma4_tokenizer_texts.json` (texts + expected ids),
a test-only ctest case `tokenizer_pack_tests.gemma4_real_pack_matches_the_fixture`, labelled
`gemma4-real-pack` and skipped (with a printed reason) when the pack directory named by
`SECURE_FRONTEND_GEMMA4_PACK` is absent.

- [x] **Step 1:** build a pack from the owner's GGUF into `out/gemma4-pack/` with Task 6's tool
  (never committed).
- [x] **Step 2:** fixture texts: ASCII prose, leading/trailing/double spaces, `\n`, `\n\n`,
  `\n\n\n` runs, tabs, code, CJK, emoji, combining marks, RTL, the Gemma marker strings
  *as text* (must be refused), long words. Expected ids come from **llama.cpp's tokenizer at
  the pinned `3cf03257`**: `llama-tokenize -m <gguf> --ids --stdin` (it always loads the
  vocabulary only, `tools/tokenize/tokenize.cpp:146`), built test-only in
  `out/llama-server-3cf03257` like session-log step 17 (owner decision D7). They must equal
  the Python reference's; any disagreement stops the task.
- [x] **Step 3: RED → GREEN** — the C++ case against `out/gemma4-pack/` (FAIL before Task 5's
  code paths are right, PASS after); record the run in the session log; commit the fixture
  (texts and ids only) `tokenizer: gemma4 real-vocabulary fixture (ids checked against llama.cpp at 3cf03257)`.

---

## Task 8: Renderer, encoder and generation plan

**Files:** new `gemma4_prompt_renderer.{hpp,cpp}`; `tokenizer_pack.{hpp,cpp}`
(`encode_gemma4_prompt`, `find_gemma4_markers`); `chat_generation_plan.{hpp,cpp}`; CMake
library `secure_frontend_gemma4_prompt_renderer`; tests new `gemma4_profile_test.cpp`.

**Interfaces:**
- Produces:
  ```cpp
  std::optional<std::string> render_gemma4_text_prompt(const std::vector<ChatMessage>& messages, bool thinking);
  struct Gemma4Markers { std::size_t bos, eos, turn_open, turn_close, think, channel_open, channel_close; };
  std::optional<Gemma4Markers> find_gemma4_markers(const TokenizerPack& pack);   // each literal exactly once, expected type
  TokenizerEncodeResult encode_gemma4_prompt(const TokenizerPack& pack, std::string_view prompt);
  ChatGenerationPlanResult make_gemma4_chat_generation_plan(const ChatCompletionRequest&, const TokenizerPack&,
      std::size_t maximum_prompt_tokens, std::size_t default_max_tokens, const SamplingDefaults&, bool thinking);
  ```
  Bytes per spec §2.5 (template lines 141-266): `<bos>`; a system turn only with a leading
  system message or thinking on, `<|think|>` first when on, then the system text trimmed;
  `<|turn>user\n{trim}<turn|>\n`; `<|turn>model\n{trim}<turn|>\n` for earlier answers; the
  generation prompt `<|turn>model\n` plus `<|channel>thought\n<channel|>` when thinking is off.
  Strict alternation user/model after an optional leading system message; a non-leading
  system message, an empty or all-whitespace text, or any marker literal (`<bos>`, `<eos>`,
  `<pad>`, `<|turn>`, `<turn|>`, `<|think|>`, `<|channel>`, `<channel|>`, the tool, image,
  audio and video markers) fails closed. `trim` uses a fixed table equal to Python's
  `str.isspace()` set.

- [x] **Step 1: RED**
  ```cpp
  void renders_the_template_bytes_thinking_off();          // exact strings, with and without a system message
  void renders_the_template_bytes_thinking_on();
  void trims_like_jinja();                                 // U+00A0, U+3000, U+2028 … trimmed; inner whitespace kept
  void refuses_markers_order_and_empty_text();
  void plan_encodes_markers_as_single_ids_and_text_as_spm(); // on Task 5's synthetic pack
  ```
  `jinja2` is not in `.venv-dev` (checked 2026-10-22) and is not added for this: the expected
  strings are written from the template text (lines 141-266) and cross-checked once on the box
  against llama.cpp's own rendering (`llama-server`'s `/apply-template`, `server.cpp:276`,
  test-only, step 17's build), recorded in the session log. The GGUF's template text is the
  authority: llama.cpp reports this file's template as outdated and applies compatibility
  workarounds (step 17), so any difference is listed and ruled on, not copied.
  FAIL.
- [x] **Step 2:** implement; the plan sets `think_tokens = {channel_open, channel_close}`,
  `starts_in_reasoning = false` (both modes), `stop_tokens` per Task 9.
- [x] **Step 3:** GREEN + gates; commit `gemma4: renderer, marker encoder and generation plan`.

---

## Task 9: Thought-channel split and stop tokens

**Files:** `generation_run.{hpp,cpp}`, `chat_generation_plan.cpp`, `tokenizer_pack.cpp`
(`find_gemma4_end_of_turn_tokens`); tests `chat_execution_test.cpp`.

**Interfaces:**
- Produces: `GenerationRunLimits::channel_name` (`std::optional<std::string>`, `"thought"` for
  gemma4, empty for the others): after an open tag, the decoded text up to the first `\n` must
  equal it and is dropped; any other line (or none within 64 bytes) ends the reply as
  `RunEnd::undecodable` with STOP+drain. Stop ids: `<turn|>` and `<eos>`, each exactly once as
  CONTROL, never output; `<|tool_call>`/`<|tool_response>`/image markers in output fail closed.

- [x] **Step 1: RED**
  ```cpp
  void gemma4_thought_channel_goes_to_reasoning();          // <|channel> "thought\n" r <channel|> a → reasoning "r", text "a"
  void gemma4_wrong_channel_name_fails_closed();
  void gemma4_turn_close_ends_the_reply_as_stop();
  void gemma4_tool_markers_in_output_fail_closed();
  ```
  FAIL.
- [x] **Step 2:** implement in the shared loop (qwen/glm untouched: `channel_name` empty).
- [x] **Step 3:** GREEN + gates; commit `generation: Gemma 4 thought channel and end-of-turn ids`.

---

## Task 10: Route, schema and execution for gemma4

**Files:** `chat_route.{hpp,cpp}`, `chat_execution.cpp`, `server_bootstrap.cpp`; tests
`chat_schema_test.cpp`, `api_server_test.cpp` (unit-level router cases where possible).

**Interfaces:**
- Produces: `chat_route.cpp:152-156` and the vision path become profile switches (gemma4 text
  only; `vision_capable` false); `reasoning_effort`: `none` → thinking off, `low`/`medium`/`high`
  → on; with neither the request nor `reasoning.effort` set, thinking is **off** (D10);
  `ChatRouteConfig` gains `gemma4_thinking_default`.

- [x] **Step 1: RED**
  ```cpp
  void reasoning_effort_per_request_on_gemma4();      // schema admits it; glm53 still refuses (unchanged)
  void gemma4_thinking_defaults_to_off();              // GEN prompt ends in the closed channel
  void gemma4_route_never_takes_another_profiles_plan();
  ```
  FAIL.
- [x] **Step 2:** implement.
- [x] **Step 3:** GREEN + gates; commit `route: gemma4 plan selection and thinking default`.

---

## Task 11: End to end on the fake engine

**Files:** `tests/support/test_tokenizer.hpp` (`write_spm_tokenizer_pack`),
`tests/support/server_harness.{hpp,cpp}` (`ServerOptions.profile` → `--profile`, pack mode),
`tests/api_server_test.cpp`.

- [x] **Step 1: RED**
  ```cpp
  void gemma4_profile_chats_end_to_end();     // stream + not: answer text; GEN record's ids equal encode_gemma4_prompt of the rendered bytes
  void gemma4_thinking_round_trip();          // fake emits <|channel> thought\n … <channel|> answer <turn|>: reasoning_content + content
  void gemma4_monitor_and_page_unchanged();   // /metrics and the page work on the gemma4 profile (fake engine)
  ```
  FAIL.
- [x] **Step 2:** implement the harness options; no product change expected (if one is
  needed, it is a bug in Tasks 1-10 and goes back to that task).
- [x] **Step 3:** GREEN + full gates; commit `test: gemma4 profile end to end on the fake engine`.

---

## Task 12: Page note and the docs truth pass

**Files:** `secure_frontend/web/index.html` (drawer note under Thinking: "Gemma models think
or answer directly; Low, Medium and High all mean thinking on"), `api_router_test.cpp` page
assertion; `secure_frontend/README.md` (profile, pack format, reasoning mapping, regenerated
table); `docs/security/threat-model.md` (one line: a third chat profile, same controls);
spec amendments (§6.1 executable key → plan 4; reconciliation list above, dated);
`docs/AGENT_HANDOFF.md`; session log (the glm53 page issue, Task 7's run).

- [x] **Step 1: RED** — the page assertion for the note; README table check after the schema text change.
- [x] **Step 2:** write.
- [x] **Step 3:** GREEN + full gates; commit `docs: gemma4 profile truth pass`.

## Waits for later plans (not lane tasks)

- The Gemma engine itself (plans 2-3) and launching it from 8080 (plan 4: managed manifests,
  `engine.executables.gemma4`, the engine gate opening, the page chatting with the real model).
- Bundles for gemma4 (`make_bundle.py --profile gemma4`) and catalog entries (D9).
