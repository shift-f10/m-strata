# Windows GPU session — 2026-10-21 (owner box, first native run)

Running log of the first build-and-run on the owner's Windows GPU box, using the
model files in `C:\Strata-data`. Every issue hit is recorded here as it happens,
with what was done about it. This is a session log, not a PROVENANCE row.

## Host

| Item | Value |
| --- | --- |
| OS | Windows 11 Pro 10.0.26200 |
| CPU | AMD Ryzen 9 3900X (12 cores / 24 threads) |
| RAM | 63.9 GB |
| GPU | NVIDIA GeForce RTX 4080 SUPER, 16376 MiB, compute capability 8.9 |
| Driver | 610.88 (meets the ≥ 580 floor for the cuda13 build) |
| CUDA toolkit | 13.0 (`C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0`) |
| Host compiler | VS 2022 Build Tools, MSVC 14.44.35207 (v143) |
| CMake | 3.31.6-msvc6 (bundled with VS Build Tools only) |
| Python | 3.12 (`%LOCALAPPDATA%\Programs\Python\Python312`) |
| Repo | `C:\multi-strata`, `main` = `e5a5327a` (= `origin/main`) |
| Free disk on C: | 199 GB |

## Data in `C:\Strata-data`

| Path | What |
| --- | --- |
| `models\IQ3_S\` | Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S, 2 shards (54.8 GB + 28.8 GB), `.done` sha256 sidecars |
| `models\swift-IQ3_S\` | Swift-Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S, 2 shards (44.9 GB + 38.8 GB), `.done` sidecars |
| `packs\iq3_s\`, `packs\swift-iq3_s\` | `tools/iq_pack.py` native-expert packs (index v3, `dense.bin` 1.54 GB, `native_experts.txt`, tokenizer dir) |
| `mtp\` | MTP draft head (`mtp-q2_0.gguf`, `rt\` runtime blobs, raw tensors, manifests) |

## Issues

Numbered in the order found. Status: **open**, **worked around**, **fixed**, **note**.

1. **Submodules not checked out** — worked around. `upstream/` and `engines/maya-ref`
   were empty (`git submodule status` showed `-`), so the root `CMakeLists.txt` would
   stop at `upstream/ is empty`. Ran `git submodule update --init -- upstream`
   (checked out `cbfd5b92`, tag `v0.2.1.3`, matching the recorded gitlink).
   `engines/maya-ref` left uninitialised (not needed for the Qwen models).
2. **No CMake or Ninja on PATH** — worked around. Only the copies bundled inside VS 2022
   Build Tools exist; builds must run from a VS developer environment (`vcvars64.bat`).
3. **Supported frontend Windows toolchains absent** — open. The frontend's supported
   Windows toolchains are llvm-mingw (cross, from Linux) and MSYS2 clang; neither is on
   this box (`C:\msys64` missing, no `clang`/`gcc` on PATH). MSVC is documented as
   "informational, red by design" for the frontend. The engine needs MSVC v143 as the
   nvcc host anyway.
4. **Native-expert packs need `STRATA_NATIVE_EXPERTS=ON`** — note. Both packs are
   `iq_pack.py` *native experts* packs; the root build seeds `STRATA_NATIVE_EXPERTS=OFF`
   (upstream's default is ON). The engine has to be configured with it ON, which fetches
   the pinned llama.cpp checkout over the network at configure time.
5. **Model size vs memory** — note. Each model is ~83.6 GB on disk against 63.9 GB RAM +
   16 GB VRAM. The native expert blob is 50.3 GB (`native_experts.txt` total), dense pack
   1.54 GB, so it should fit with experts in RAM; upstream README notes Windows can pin
   only about half of RAM (`STRATA_PIN_BY_NEED=1`). Watch for pinning/paging limits.
6. **Docs disagree / stale** — note. `CLAUDE.md` and `AGENTS.md` give different Windows-claim
   rules (CI job vs owner hardware); the resume doc (2026-10-16) still names upstream
   `v0.1.44.1` / `296b59d2`, while the checkout pins `v0.2.1.3` / `cbfd5b92`.
7. **SP7 offline kit is Linux-only** — note. `docs/validation/sp7-offline-kit.md` uses
   `unshare`, `.deb`, `sha256sum`, `libcudart.so`; there is no Windows procedure, so this
   session follows the resume doc §8 checklist adapted to Windows instead.
8. **No git identity configured** — note. `git config user.name/user.email` are unset in
   this checkout; nothing is committed in this session.

9. **Frontend `windows-msvc` preset fails to build** — open (expected per
   `docs/secure-frontend-ci.md`: MSVC is informational). Configure OK; build exit 1
   (`out/frontend-msvc-build.log`). Distinct errors, all from `-Werror` → `/WX`:
   - C4456 (declaration hides previous local): `src/chat_schema.cpp:216` (`member`),
     `src/frontend_config.cpp:978` and `:1096` (`token`),
     `tests/supervision_fake_engine_main.cpp:623` (`line`).
   - C4996 (`getenv` / `_wgetenv` deprecated): `src/token_reader.cpp:82`, `:97`.
10. **`tests/manifest_test.cpp:349` does not compile on MSVC** — open, real portability bug.
    The local `secure_frontend::ManifestRules small;` collides with the Windows SDK macro
    `#define small char` (`rpcndr.h`, pulled in by `<windows.h>`), giving C2628/C2059/C2062.
    Renaming the variable fixes it; not changed here (test code, owner has not named it).
    mingw-w64's `rpcndr.h` has the same macro, so the MSYS2/llvm-mingw builds can hit it too
    if `<windows.h>` ever reaches that translation unit.

11. **Engine build is warning-heavy** — note. 1126 lines matching `warning` in
    `out/build-engine-cuda.log` (nvcc `#128-D` unreachable loop, `#177-D` unused function,
    MSVC C4459 shadowing in `upstream/src/kernels/cuda/iq_kernels.cu`, …). Upstream builds
    with `STRATA_WERROR=OFF`; not ours to fix (`upstream/` is never modified).
12. **Engine version string differs from the tag** — note, expected. `strata.exe --version`
    prints `strata 0.1.46.2`; that is upstream's engine `project(... VERSION 0.1.46.2)`
    (`upstream/CMakeLists.txt:11`) at app tag `v0.2.1.3`, not a wrong checkout.

13. **Native packs refuse to run without MTP speculation** — note. First one-shot
    `generate` (no `--spec`) exited 2 in 1 s with
    `... is a native (IQ) pack: it needs --native SHARD1, --spec T (T >= 2) and --prefill CHUNK`.
    So the MTP head in `C:\Strata-data\mtp\rt` is mandatory for these packs, not optional.
    Relevant to the frontend: `engine.mtp:false` strips `--mtp`/`--spec-min-p`, which would
    make a native-pack profile unlaunchable.
14. **No Python packages on the system interpreter** — worked around. `regex` (needed by
    upstream's tokenizer) was missing; created `.venv-dev/` and installed
    `scripts/dev/requirements-dev.txt` (the only install location the project rules allow).
    Upstream's own runtime requirements (numpy, jinja2, …) are deliberately not in that file,
    so upstream's Python server cannot run from `.venv-dev` as-is.
15. **Engine has no tokenizer** — note. `strata.exe generate` takes token IDs only
    (`--tokens` / `--tokens-file`); session helper `out/tok.py` wraps
    `upstream/tools/strata_tokenizer.py` for encode/decode with the pack's tokenizer dir.

16. **Session script bug: `cmd` splits on commas** — fixed (ours, `out/run-generate.bat`).
    Passing `--tokens "%~2"` through a `.bat` delivered only the first ID (`prompt : 248045`,
    0 tokens out) because `cmd` treats commas as argument separators. Switched to
    `--tokens-file`. Also: `cmd` here does not run `strata.exe` from the current directory
    (`NoDefaultCurrentDirectoryInExePath` behaviour); scripts use the full path.
17. **Expert arena only partly pinned; Windows commit limit nearly exhausted** — open
    (performance/stability risk). From the engine log:
    - `cudaHostRegister of the whole arena FAILED (out of memory); 47 slices pinned (45 GiB)`
    - `large pages refused for 50295996416 B (... VirtualAlloc error 1314)` — the account
      lacks `SeLockMemoryPrivilege` ("Lock pages in memory"); falls back to 4 KB pages.
    - `only 0.5 GiB of the Windows commit limit is left` → the CPU prefill share is not armed.
    Commit limit is 67.9 GB (63.9 GB RAM + ~4 GB page file); the 46.8 GiB arena plus the
    mapped model files use nearly all of it. A larger page file and/or the "Lock pages in
    memory" right are system-settings changes for the owner to decide; not changed here.
    Upstream README suggests `STRATA_PIN_BY_NEED=1` on Windows (not yet tried).

18. **Frontend refuses the default Windows folder permissions** — open (working as designed,
    but a first-run usability blocker). `strata-secure-server --check --expert` exit 1:
    `roots.models` / `roots.tokenizer_pack` / `engine.executable`: "must not be writable by
    other users". Any folder created under `C:\` inherits
    `NT AUTHORITY\Authenticated Users:(M)`, so `C:\Strata-data` and `C:\multi-strata\out`
    fail out of the box. Engine and tokenizer copied into an owner-only folder
    (`out/serve/`, inheritance removed, current user Full only). The 167 GB models root
    cannot reasonably be copied: its ACL needs the owner's decision. The product docs should
    say how to prepare a Windows models folder (e.g. the `icacls` lines) before first serve.

19. **Every Qwen3.8 chat through the secure frontend fails with 500 `internal_error`** —
    **fixed in the working tree** (see "Fix for issues 19 and 26"; GPU-verified, step 7;
    C++ RED/GREEN runs owner-side, pending). Was: open, blocking (main finding of this session). Non-streaming and streaming both fail
    in ~1.6–2 s, even for "Say hello in one word."; the stream sends the role chunk, then an
    `internal_error` chunk, then `[DONE]`.
    - Cause: `src/qwen_prompt_renderer.cpp:152` ends the prompt with
      `<|im_start|>assistant\n` and nothing else. The model's own template
      (`packs/*/tokenizer/chat_template.jinja`, `add_generation_prompt`) always continues
      with `<think>\n` (thinking on) or `<think>\n\n</think>\n\n` (`enable_thinking=false`).
      Without either, the model opens its reply with `<think>` itself.
    - `<think>` (248068) and `</think>` (248069) have token type 4 in `token_type.json`, and
      `TokenByteDecoder::bytes_for` (`src/tokenizer_pack.cpp:252-253`) refuses types 3/4 as
      output → `StopCause::undecodable` (`src/generation_run.cpp:129`) →
      `GenerationFailure::decode_failed` → 500 (`src/chat_route.cpp:228`,
      `src/chat_stream.cpp:157-160`).
    - Proof: the engine run directly on the frontend's exact prompt
      (`out/prompt-ids-frontend-style.txt`) emits `248068 198 ...` first
      (`out/confirm-think-token.log`).
    - `reasoning.effort` unset ("names none") does not render the empty think block, so
      there is no config workaround; user content cannot carry control literals.
    - Fix direction (not done here — product code, owner has not named it; needs TDD):
      render the generation prompt as the template does for the chosen reasoning level, and
      handle `<think>`/`</think>` in output (strip or route to a reasoning field) instead of
      failing the request. Compare upstream `serve/server.py`'s Qwen path for parity.
20. **The 500 carries no cause in the server log** — note (diagnosability). The request log
    line is `req 12 chat 500 internal_error 1956 460 loopback`; nothing names the
    undecodable token, so the cause had to be found by reading the source. A fixed, text-free
    log code (e.g. `undecodable_token`) would keep the privacy rule and save the next person
    the hunt.
21. **`/v1/models` lists one model** — **resolved (docs, step 10).** With `--expert`, the docs say `/v1/models`
    "lists the whole inventory", but only `Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002`
    is returned although `roots.models` also holds the Swift shard 1. The behaviour is the
    SP3 spec's (E9: only the served model, plus `model.aliases`; `server_bootstrap.cpp`). The
    README sentence and two "SP3 decides later" comments on SP4's `served_models` (computed,
    never read by the server) were stale and now say so.
22. **Engine outlives a force-killed frontend by a few seconds** — note, not a leak.
    After `Stop-Process -Force` on `strata-secure-server`, `strata.exe` was still listed for
    ~3 s, then gone with its VRAM freed.

23. **Second `small`-macro clash: `tests/http_socket_server_test.cpp:837`** — open, same
    portability bug as issue 10 (`int small = 4096;` next to winsock). With
    `SECURE_FRONTEND_WERROR=OFF` the full MSVC test tree builds 51 test executables; only
    `secure_frontend_manifest_tests` and `secure_frontend_http_socket_server_tests` fail to
    compile (`out/frontend-msvc-tests-old-build.log`).
24. **Smart App Control blocks locally built test binaries** — first decision: SAC stays
    on and the owner runs the C++ test binaries (`out/run-frontend-tests.ps1`); later the same
    day the owner **turned Smart App Control off** so the suites can run in full here.
    `VerifiedAndReputablePolicyState = 1` (enforce); Code Integrity events 3033/3077 show
    `ctest.exe` refused `secure_frontend_*_tests.exe` ("did not meet the Enterprise signing
    level requirements"), so every case reported `BAD_COMMAND`. The engine and
    `strata-secure-server.exe` built earlier were not blocked.
25. **Repo pytest on Windows: 7 host failures** — note. Old pin (`v0.2.1.3`, Maya `v1.1.8`
    checked out): `43 passed, 7 failed, 4 skipped`. All 7 in `tools/secure/test_build_release.py`:
    six run `bash scripts/release/*.sh` (WSL has no distribution: `execvpe(/bin/bash) failed`),
    one needs the Linux cross-gate build tree. With `engines/maya-ref` uninitialised, five more
    fail (`sbom.py` reads `engines/maya-ref/LICENSE`): `38 passed, 12 failed`.

26. **Every Qwen chat ends in 500 at the end-of-turn token** — **fixed in the working tree**
    with issue 19 (same class, hidden behind it; GPU-verified, step 7). With issue 19 fixed, the stream delivered
    `reasoning_content` and `content: "The capital of France is Paris."`, then an
    `internal_error` chunk (`out/serve-iq3s-v0.2.2.log`, v0.2.2 engine). In serve mode the
    engine prints the end-of-turn token as a `T` line before `DONE … stop`
    (`generate.cpp:14578-14590`, `eos_ids` 248044/248046 at :700); `<|im_end|>` is CONTROL
    (type 3), so `TokenByteDecoder` refuses it → undecodable → 500. Upstream's server treats
    `<|im_end|>`/`<|endoftext|>` as stop ids, never decoded or output (`serve/server.py:3770-3771`,
    `:5086-5091`). No fake-engine test emits an end-of-turn token, which is why it never showed.

27. **Maya `v1.1.21`** — **bumped (owner decision), READY blocker fixed; open items below.**
    `engines/maya-ref` `v1.1.8` → `v1.1.21` (commit `451f9cb6`, tag object `ba26442d`,
    124 commits), after a read-only audit:
    - **Blocker — fixed test-first:** the fast path prints one `VRAM dev=…` record per GPU
      part before `READY` (Maya `generate.cpp:1054-1062`; plus `NOTE experts: …` and a second
      `INFO` on a commit-capped Windows APU, :1065-1071). `await_ready` accepted only `INFO`
      there, so every glm53 start would have failed. `maya_118` now consumes a bounded `VRAM`
      record (STAT rule: at least one pair, every key non-empty before `=`) and a non-empty
      `NOTE` before READY; everything else still fails closed, and the other dialects still
      reject `VRAM`. Tests: `engine_protocol_client_tests.maya_flavor_accepts_vram_and_note_before_ready`,
      `engine_supervisor_tests.fake_maya_vram_records_reach_ready_only_under_the_maya_dialect`
      (the fake gains `--maya-vram`; the existing Maya round-trip test is unchanged). RED tree
      `out/frontend-msvc-tests-red27`; runs owner-side. The enum keeps the name `maya_118`
      (the records are additive; a rename would touch 26 sites).
    - Citations: 606 occurrences (212 unique) re-pinned to `v1.1.21`; pin statements, the
      threat-model dialect text and PROVENANCE (bump paragraph, both LICENSE copyright lines,
      `LICENSE.deepseek-v41`, the Outfit font) updated.
    - **Open — penalties:** the GLM engine now honours the penalty keys (ignored at `v1.1.8`,
      Maya `generate.cpp:3449-3470`); Maya renders `penalty_last_n` 0 as no penalties at all
      and −1 as the whole window (4096, `serve/server.py:754-783`, `:260`). Ours renders
      `penalty_last_n=64` for ≤ 0 whenever a penalty is on and bounds the key to 0–8192, so a
      configured 0 now penalises on the GLM side where Maya does not, and −1 is refused.
      Decide: follow Maya `v1.1.21` for glm53, or record a C-6 divergence.
    - **Open — env names:** Maya's user-facing settings are now spelled `MAYA_*`; the code
      still reads `STRATA_*`, and the engine copies `MAYA_X` over `STRATA_X` at start
      (`env_alias.hpp`, `generate.cpp:3591`). Our closed allowlist keeps every `MAYA_*` name
      out, so the five forced safety values hold (threat model updated). But the GLM key
      inventory (`parity_keys_maya.hpp`) can no longer be regenerated with its documented
      greps; it stays the `v1.1.8` set (119 env knobs, 37 config keys) until decided. The
      adapted inventory has 50 new knobs (five DFlash — must be rejected rows, plus
      DFlash-window-only ones to re-rule), new config key `idle_unload_s`, new server knob
      `MAYA_PACK_TEMPLATE`, and three documented non-`_GLM_` names.
    - Other findings, not acted on: binaries renamed `maya` / `maya-vision`
      (`server_bootstrap.cpp:293` hard-codes `strata-vision`); `VERSION` is empty at the tag
      (the kit must record tag + commit); Maya now renders GLM-5 packs with its own shipped
      template (`serve/server.py:4062-4073`; re-check our renderer with the kit corpus);
      the "Repeatable answers" doc says text-for-text kit pairs need `--expert-cache N` and
      `PROMOTE=0`/`CPU_RETIME=0`/`CPU_PLAN` pinned; `WARM_PROMPT` may print before READY
      (unverified).
    - License: still MIT; a new first copyright line "Peasant Smith and the Project Maya
      contributors"; new `LICENSE.deepseek-v41` (MIT, DeepSeek, `serve/ds41_encoding.py`); no
      new non-commercial component (DFlash2 CC BY-NC-ND unchanged, opt-in); the bundled Outfit
      font (SIL OFL 1.1) was never recorded in PROVENANCE.
    - Full audit kept in the session scratchpad (`audit-maya-v1.1.21.md`).
28. **Excluded upstream tests on Windows at `v0.2.2`** — note. Run alone: the three
    `test_setup_choices.py` exclusions pass, the five golden-config subtests and the
    parallel-download test still fail, the lifeline test skips (POSIX path). Whether an
    exclusion can be dropped is a Linux-host question; the runbook list is unchanged.

## Fix for issues 9, 10 and 23 (MSVC `/W4 /WX`, after the first commit)

The project's own `windows-msvc` preset (`SECURE_FRONTEND_WERROR=ON`, `/W4 /WX`) now builds:
- C4456/C4458/C4459 shadowing: inner names renamed (`chat_schema.cpp` parts loop `member` →
  `part_member`; `frontend_config.cpp` layer-split loops `token` → `piece`; the fake engine's
  loop record `line` → `record` — every use in the loop; `cli_harness.hpp` `out` → `escaped`;
  `run_cases(..., failure_count)`).
- C4996 (MSVC's deprecation of `getenv`/`_wgetenv`): one header,
  `include/secure_frontend/crt_environment.hpp`, with `crt_getenv`/`crt_wgetenv` that silence it
  in one place. `_dupenv_s` is not a substitute where it matters: `token_reader.cpp` wipes the
  CRT's own copy of the token in place (CFG-05), and `_dupenv_s` would return a copy.
- C2628/C2187: the Windows SDK's `#define small char` (`rpcndr.h`) — `small` locals renamed in
  `manifest_test.cpp` (`small_rules`) and `http_socket_server_test.cpp` (`small_buffer`).
- C4244 in `engine_child_test.cpp`: the ASCII SID string is narrowed with an explicit cast.
- Not touched: POSIX-only `getenv` uses that MSVC never compiles.

## Fix for issues 19 and 26 (owner-approved 2026-10-21)

Behaviour follows upstream `v0.2.1.3` (`serve/frontend.py`, `serve/server.py`), checked by a
read-only audit:

- **Prompt.** When the tokenizer pack has `<think>` and `</think>` (exactly one CONTROL or
  USER_DEFINED token each; `find_qwen_think_tokens`), the Qwen renderer ends the prompt as
  the template does: `<think>\n` (thinking, the default) or `<think>\n\n</think>\n\n` when
  `reasoning.effort` is `none`. A pack without them keeps the old plain prompt. With a think
  tail, content holding either literal fails closed, and the encoder accepts the markers
  only as that exact tail after the final `<|im_start|>assistant\n`.
- **Output.** `drive_generation` routes text before `</think>` to a reasoning channel and
  text after it to the answer, dropping only the newlines right after `</think>`. Neither tag
  is ever output; a `<think>` while answering opens a new reasoning span (upstream would leak
  it into `content`). A partial UTF-8 character before a tag becomes one U+FFFD in its own
  channel. One byte cap covers both channels. Other CONTROL/USER_DEFINED tokens still fail
  closed.
- **Response.** Non-stream: `message.reasoning_content` after `content`, only when non-empty.
  Stream: `delta.reasoning_content` events, in arrival order with the content deltas.
- **End of turn (issue 26).** The plan carries the pack's `<|im_end|>`/`<|endoftext|>` ids
  (`find_qwen_end_of_turn_tokens`, each exactly one CONTROL token). In `drive_generation` one
  ends the reply as `stop`: it is never output, a held partial character ends as one U+FFFD,
  anything before `DONE` is dropped, and no `STOP` is sent (the engine ends the turn itself).
  Without stop ids a CONTROL token still fails closed.
- **Files:** `tokenizer_pack.{hpp,cpp}`, `qwen_prompt_renderer.{hpp,cpp}`,
  `chat_generation_plan.{hpp,cpp}`, `generation_run.{hpp,cpp}`, `chat_execution.{hpp,cpp}`,
  `chat_schema.{hpp,cpp}`, `sse_encoder.{hpp,cpp}`, `chat_stream.cpp`, `chat_route.{hpp,cpp}`,
  `server_bootstrap.cpp`; tests in `qwen_prompt_renderer_test.cpp`, `tokenizer_pack_test.cpp`,
  `chat_generation_plan_test.cpp`, `chat_execution_test.cpp`, `chat_schema_test.cpp`,
  `sse_test.cpp` (19 new cases: 14 for issue 19, 5 for issue 26, registered in
  `secure_frontend/CMakeLists.txt`).
- **TDD.** For each issue the API surface came first with stubs that kept the old behaviour,
  then the tests, then the implementation. Builds (MSVC, `SECURE_FRONTEND_WERROR=OFF`, because
  of issues 9/10): RED for issue 19 `out/frontend-msvc-tests-red`, RED for issue 26
  `out/frontend-msvc-tests-red26`, GREEN (everything, final source) `out/frontend-msvc-tests-green`.
  No tree adds a warning in the changed files. The Linux `-Werror` gates (gcc/clang/cross)
  have not run.
- **Owner-run results (2026-10-21, `out/ctest-*.xml`; compare with `out/compare-ctest.ps1`):**
  - Baseline, old tree `out/frontend-msvc-tests`: 844 tests — 718 passed, 30 failed, 39
    blocked by Smart App Control (`BAD_COMMAND`), 57 not run (the two test binaries that do not
    compile under MSVC, issues 10/23). The 30 failures are Windows-host/MSVC ones unrelated to
    this work: the update CLI needs the symlink privilege (7), Task Scheduler (2), typed-config
    cases written with POSIX paths (4), Winsock disconnect timing (7 in `api_server_tests`),
    vision-child cases (4), the egress import check flagging `WS2_32` in the MSVC server (1),
    the systemd golden (1), `strata serve` foreground (2), the API probe (1), token store (1).
  - RED, issue 19 (`frontend-msvc-tests-red`): **13 failed — exactly the new tests**; the 14th,
    `drive_generation_keeps_other_special_tokens_fail_closed`, is a guard and passed, as meant.
  - RED, issue 26 (`frontend-msvc-tests-red26`): **5 failed — exactly the 5 new tests.**
  - GREEN, final issue 19/26 source (`frontend-msvc-tests-green`): **all 19 new tests pass**; no
    test that ran at the baseline fails for a code reason. One new failure,
    `acquire_tests.dl04_interrupt_during_download_never_indexed_windows`, is Smart App Control
    blocking the freshly built `secure_frontend_non_elevated_run.exe` the test starts the
    downloader through (Code Integrity event 3077). 158 cases did not run (blocked binaries:
    fetch, bundle, privacy, model_index, update_cli, service_ops, ed25519, release_manifest,
    sha256 and two server cases) — none on the chat path, but their state at `v0.2.2` is unproven.
  - Issue 27 (owner-run): RED (`frontend-msvc-tests-red27`, regex `maya`) — both new tests
    failed, the three existing Maya cases passed; the supervisor test's Maya half failed for
    the right reason ("the v1.1.21 startup reaches READY under maya_118"). GREEN
    (`frontend-msvc-tests-maya`) showed one more failure, in that test's strict half: a bug in
    the test, not the code — `LogCapture::contains` matches whole lines and the assertion
    passed a fragment (which also made the Maya half's negative check vacuous). Both now name
    the full line `engine generation 1 ended: killed_by_supervisor(protocol_failed)`; the
    corrected strict half was not re-run against the unfixed code.
  - **Final, run here after Smart App Control was turned off (nothing blocked):**
    baseline `old-full` (old tree, original source): 844 tests — 757 passed, 30 failed,
    57 not run. GREEN `green-full` (`frontend-msvc-tests-green` rebuilt from the final source:
    issues 19, 26, 27, the `v0.2.2` and Maya `v1.1.21` re-pins): 865 tests — **778 passed,
    30 failed, 57 not run. The 30 failures are exactly the baseline's (none new, none fixed);
    all 21 new tests pass**; the earlier `dl04` failure is gone (it was Smart App Control).
    Compare: `out/compare-ctest.ps1 -Old old-full -New green-full`.
- **Parity gaps left on purpose:** the template's effort sentence ("Reasoning effort is set to
  xhigh…", written as a system turn by default) is not rendered; `reasoning.effort`
  low/medium/high are not distinguished (all think); `<think>` in content is refused rather
  than tokenised as upstream does. The M1 `ChatExecutionService::execute` path (tests only,
  via `execute_with_lease`) and the GLM profile are unchanged. The `reasoning.effort` config
  description and the parity table row (`config_only`) still need updating to say it now
  selects the qwen think tail.

## Changes made to this machine

- `C:\Strata-data` ACL tightened (owner-approved in chat): inheritance removed; the current
  user, Administrators and SYSTEM keep Full, `BUILTIN\Users` keep Read & execute;
  `Authenticated Users: Modify` is gone. The previous ACLs are saved in
  `out/strata-data-acl-backup.txt` (79 entries). Restore exactly with
  `icacls C:\ /restore C:\multi-strata\out\strata-data-acl-backup.txt`.
- `git submodule update --init -- upstream` (issue 1).
- `.venv-dev/` created with `scripts/dev/requirements-dev.txt` (issue 14).
- `git submodule update --init -- engines/maya-ref` (first at its pin `v1.1.8`), then moved to
  `v1.1.21` (issue 27); `git -C engines/maya-ref status --porcelain` is empty.
- `upstream/` moved to `v0.2.2` (step 6); `upstream/` itself has no local edits. One slip,
  corrected: a session edit run from the wrong directory created an empty untracked
  `upstream/src/chat_generation_plan.cpp` (18:26, after the `v0.2.2` engine build had
  finished); it was removed and `git -C upstream status --porcelain` is empty again.
- Build and run output only under `out/` and `secure_frontend/out/` (both gitignored), including
  the two runbook venvs `out/venv-upstream-{old-v0.2.1.3,new-v0.2.2}`.
- Tracked files changed (nothing committed): the issue 19/26 fix and tests, the v0.2.2 pin
  sites and parity re-pin, and these docs.

## Steps and results

### 1. Engine build (CUDA) — PASS

- Script: `out/build-engine-cuda.bat` (vcvars64 → CMake/Ninja), mirroring upstream
  `setup.py` `build_engine()`: `-DSTRATA_ENABLE_CUDA=ON -DSTRATA_BUILD_TESTS=OFF
  -DSTRATA_NATIVE_EXPERTS=ON -DSTRATA_Q6K_EXPERTS=ON -DSTRATA_MMQ_KQUANTS=ON
  -DCMAKE_CUDA_ARCHITECTURES=89`, CUDA 13.0, Release, build dir `out/engine-cuda`
  (outside `upstream/`, which stays unmodified).
- llama.cpp `3cf03257` fetched by CMake FetchContent at configure time (network).
- Result: exit 0, 163/163 steps; `out/engine-cuda/strata.exe` (75.8 MB) and
  `strata_kq.dll` (10.9 MB). Log: `out/build-engine-cuda.log`.
- `strata.exe --version` → `strata 0.1.46.2`, exit 0.

### 2. Frontend build (`windows-msvc` preset) — FAIL (issues 9, 10)

- Configure exit 0, build exit 1. Logs: `out/frontend-msvc-configure.log`,
  `out/frontend-msvc-build.log`.

### 3. One-shot GPU generate, original IQ3_S — PASS

- Command: `out/run-generate.bat iq3_s out/prompt-ids.txt 48`, i.e. `strata.exe --pack
  C:\Strata-data\packs\iq3_s --native <shard1> --ple-gguf <shard2> --expert-profile
  upstream\data\expert-profile.bin --expert-cache auto --prefill auto --spec 4
  --spec-min-p 0.5 --mtp C:\Strata-data\mtp\rt --max-new 48 --tokens-file ...`
  with CUDA 13.0 `bin\x64` on PATH (for `cublas64_13.dll`).
- Prompt: "What is the capital of France? Answer in one sentence." (Qwen chat template,
  thinking off), 24 tokens.
- Output (decoded): `The capital of France is Paris.<|im_end|>` — correct. `generate`
  then keeps going to `--max-new` (repeats the turn); expected for the one-shot tool,
  which does not stop at end-of-turn.
- Wall time 46 s end to end (most of it loading the 46.84 GiB expert arena at 1.67 GiB/s).
- Expert cache: 4518 slots, 8.62 GiB VRAM; MTP draft head 799 MiB VRAM.
- Prefill 23 tokens in 532.5 ms (43.2 tok/s), time to first token 1928 ms.
- **Decode 48 tokens in 944.6 ms → 50.82 tok/s.** Speculation: 14 rounds, drafts
  accepted 34/41 (0.829), 3.43 tokens/round. (Upstream README quotes 53 tok/s for IQ3_S
  on an RTX 5070.)
- Log: `out/smoke-iq3s-generate.log`.

### 4. One-shot GPU generate, Swift IQ3_S — PASS

- `out/run-generate.bat swift out/prompt-ids-swift.txt 48` (no `--ple-gguf`: the engine
  found the PLE table in Swift's shard 1 by itself). Same prompt IDs as the original.
- Output: `The capital of France is Paris.<|im_end|>` — correct.
- Wall 47 s; prefill 23 tokens in 466.7 ms (49.3 tok/s), TTFT 1364 ms;
  **decode 48 tokens in 938.0 ms → 51.17 tok/s**; drafts accepted 35/41 (0.854),
  3.69 tokens/round. Same pinning/commit warnings as run 3 (issue 17).
- Log: `out/smoke-swift-generate.log`.

### 5. Secure frontend serve + HTTP chat, IQ3_S — FAIL (issue 19)

- Frontend built with MSVC **outside the supported toolchains**, as a smoke-only build:
  `-DSECURE_FRONTEND_WERROR=OFF -DBUILD_TESTING=OFF`, targets `strata-secure-server` and
  `strata` only, into `out/frontend-msvc-nowerror/` (exit 0). No source changed. Results
  below are evidence about this machine and the engine path, not a supported-build claim.
- Config `out/serve/config-iq3s.json` (`--expert`, profile `qwen`, `env:STRATA_API_TOKEN`
  with a 43-char random token kept in owner-only `out/serve/api-token.txt`, engine and
  tokenizer copied to owner-only `out/serve/`, state/cache under `out/serve/`).
  `--check --expert` → `Configuration is valid.` after issue 18 was resolved.
- Serve: listening on `127.0.0.1:8080`; `/health` 503 while loading, **200 after ~45 s**
  (engine READY over stdio, well inside `ready_timeout_s` 900).
- `GET /v1/models` without a token → **401 `invalid_api_key`** (auth enforced on loopback).
- `GET /v1/models` with the token → 200, one model (issue 21).
- `POST /v1/chat/completions` (non-stream and stream) → **500 `internal_error`**: issue 19.
  The engine did start generating (GEN accepted, first `T` line received), so the stdio
  protocol works up to the first token; the failure is in the frontend's output decoding.
- Log: `out/serve-iq3s.log`. The Swift model was not served: the same bug would block it.

### 6. Upstream bump `v0.2.1.3` → `v0.2.2` (owner request) — gates on this box

- `git -C upstream checkout --detach v0.2.2` (commit `011c9e60`, tag object `e6fff4ad`,
  242 commits); `git -C upstream status --porcelain` empty; `upstream.version` → `v0.2.2`.
  Read-only audit first: CMake, `--serve` protocol and `tools/strata_tokenizer.py` unchanged;
  one new engine flag (`--pool-cores`); parity inventory 87 → 88 keys, 31 → 32 env knobs.
- Pin sites, `engine-protocol-limits.md` (every cited line re-checked at v0.2.2, one row
  added for `--pool-cores`), frontend comment cites and the parity table (127 rows, README
  mirror re-rendered) updated. `docs/security/privacy-migration.md` still names `v0.2.1.3`: its
  re-review (new ranged-download path in `setup.py`, new `strata_control` modules) is not done.
- Gates (Windows host; the runbook's Linux gates were not run):

  | Gate | old `v0.2.1.3` | new `v0.2.2` |
  | --- | --- | --- |
  | Repo `pytest -q` (`.venv-dev`) | 43 passed, 7 failed, 4 skipped | 43 passed, 7 failed, 4 skipped (same 7 host failures, issue 25) |
  | Upstream `tools` suite (runbook deselects) | 1004 passed, 12 failed, 24 skipped, 6 deselected | 1027 passed, 11 failed, 30 skipped, 6 deselected (no new failure; `Resume::test_a_killed_download_continues` now passes) |
  | Upstream `serve` suite | 865 passed, 27 skipped, 1 deselected | 905 passed, 23 skipped, 1 deselected |
  | Engine CUDA build | PASS (`strata 0.1.46.2`) | PASS, `out/engine-cuda-v0.2.2` (`strata 0.1.47`, 163/163) |
  | Frontend C++ ctest (MSVC) | built, run pending (owner) | GREEN tree built, run pending (owner) |

  The 11 remaining upstream failures are Windows-host ones (MCP install/start-stop, SYCL build
  guard, MTP fetch). Logs: `out/upstream-{tools,serve}-{old-v0.2.1.3,new-v0.2.2}.log`.

### 7. Serve + HTTP chat with the fixes, `v0.2.2` engine, IQ3_S — PASS

- Server rebuilt from the fixed source (MSVC smoke build, test hooks OFF); engine
  `out/engine-cuda-v0.2.2` copied to owner-only `out/serve/engine/`. `/health` 200 after 39–44 s.
- Non-stream, "What is the capital of France? Answer in one sentence." → **200**,
  `content: "The capital of France is Paris."`, `reasoning_content` holding the think block,
  `finish_reason: "stop"`, 31 completion tokens, 2.3 s.
- Stream, a Fibonacci function → **200**: one `reasoning_content` delta, then the code as
  `content` deltas, the `stop` chunk and `[DONE]`; no tag in any event. 3.1 s.
- Stream with `include_usage`, ~300 words → 513 completion tokens in 7.5 s end to end
  (**68.7 tok/s** including prefill, MTP speculation on).
- Client disconnect after 2 s → log `chat_stream 200 client_gone`; the next request was served.
- Multi-turn with a system turn and assistant history → `content: "Sam"`, stop.
- `reasoning.effort: "none"` (`out/serve/config-iq3s-nothink.json`) → closed think block:
  `content: "The capital of France is Paris."`, no `reasoning_content`, 8 tokens, 2.2 s.
- Request log (`out/serve-iq3s-v0.2.2-fixed.log`): `chat 200`, `chat_stream 200` ×2,
  `chat_stream 200 client_gone`, `chat 200`. Outputs: `out/serve-v0.2.2-fixed-*.txt`,
  `out/serve-v0.2.2-nothink.txt`.

### 8. Engine supply-chain contract (owner request) — first application, to `v0.2.2`

- Contract: `docs/security/engine-supply-chain.md` (C1–C9), gate script
  `scripts/security/engine_review.py` (`diff`, `binary`), allowlist
  `docs/security/engine-binary-allowlist.json`, tests `tools/secure/test_engine_review.py`
  (7 passed). Wired into PROVENANCE "Updating the baseline" steps 2, 3 and 6.
- Order note: this bump built the engine and ran upstream's suites before the review
  (the contract did not exist yet); C2 forbids that from now on.
- C1: tags unsigned (`v0.2.1.3`, `v0.2.2`, Maya `v1.1.21`); both builds' fetched llama.cpp
  is `3cf03257`, clean, and it is the only fetch site in the build.
- C3: 58 findings (py-exec 15, py-load 12, js-exec 10, digest 6, deps 6, py-net 5, net 3,
  load 1), each read in context and ruled: 0 refused, 6 accepted (installer download
  repair and update-feed run in `setup.py`, an install-script test, the NVFP4 torch pin),
  the rest benign; no new network host in compiled, build or script code; no added binary
  file; no new third-party dependency. Records: `docs/security/engine-reviews/strata-v0.2.1.3-v0.2.2.{json,md}`.
- C5: `strata.exe` and `strata_kq.dll` (both pins) PASS: no network library imported or
  named, no process-launch import; loadable names `nvcuda`, `nvcudart_hybrid64`, `nvldumd`,
  `nvldumdx`, `dxgi`, `dbghelp`, `strata_kq`. The one `http://` string is the Windows
  manifest's XML namespace. Negative control: System32 `curl.exe` fails the gate.
- C4: the full read of the compiled and build diff (33 files, +2991/−110), by an agent
  reviewer, is recorded in `docs/security/engine-reviews/strata-v0.2.1.3-v0.2.2-compiled-read.md`:
  **0 concern, 12 note, 21 clean** — no new network/IPC, process launch, code loading,
  persistence, privilege or system-setting change, time/host-gated behaviour, encoded data or
  configure/build-time command; serve mode writes nothing new to stdout. Notes worth the
  owner's eye: the NUMA split is now on by default on Linux boxes with ≥ 2 NUMA nodes (its off
  switch `STRATA_NUMA_POOL` is not in our allowlist); a test-only `STRATA_TEST_SYSFS_ROOT`
  read survives in the release binary (Linux, read-only `/sys` redirect, only steers the NUMA
  decision — kept as "ambiguous", not cleared); the engine now asks Linux to drop the page
  cache of its own pack/MTP files (read-only); `tq_pascal_bench` joins a full CUDA build (not
  `--target strata`).

### 9. After the first commit (`556b6368`, not pushed)

- **MSVC `/W4 /WX`:** issues 9, 10, 23 fixed (see "Fix for issues 9, 10 and 23"). The
  `windows-msvc` preset builds clean from scratch with warnings as errors: 0 warnings, 53 test
  binaries (`manifest_tests` and `http_socket_server_tests` for the first time).
- **Full preset suite** (`ctest --preset windows-msvc`), final source of this step
  (`out/ctest-preset-final.xml`): 866 tests, 833 passed, 32 failed, 1 not run
  (`cfg03_rejects_symlink_below_root`, needs a symlink privilege). 30 failures are the
  baseline's; 2 are new because their binary never ran under MSVC before (issue 29); the 56
  newly compiled tests otherwise pass, and nothing regressed against the previous run.
- **Issue 20 fixed test-first:** a refused output token still answers the generic 500 (stream:
  an `internal_error` chunk), and the request log now carries the log-only code
  `undecodable_token` (`chat_route.cpp`, `chat_stream.cpp`). Test
  `api_server_tests.undecodable_output_logs_its_own_code`: RED (`req 3 chat 500 internal_error`,
  `req 4 chat_stream 200 internal_error`), then GREEN; the 64 related chat/SSE/API cases pass.
- **`reasoning.effort`:** config description and parity row (now `frontend behaviour`) say what
  it does for the qwen profile; README mirror and config table updated together.
- **`privacy-migration.md` re-reviewed for `v0.2.2`** (agent read): every row holds; 2 rows
  updated (`setup.py` download repair + signed-feed engine fetch; the update scripts now reach
  it), 2 added (the `strata update` feed client; `apps/host`/`apps/desktop`, whose LAN share and
  Cloudflare tunnel no longer require a key at `v0.2.2`). No literal URL host added.
- **Distribution check hardened test-first:** `apps/host` and `apps/desktop` (JavaScript, so the
  `.py` rule never caught them) are refused at any depth; RED then GREEN, 13 tests.
- **Swift model through the frontend** (`out/serve/config-swift.json`, no `--ple-gguf`): ready in
  48 s; "Name three primary colors" → `content: "Red, yellow, blue"` plus `reasoning_content`,
  stop, 2.6 s.
- **Issue 21 explained:** with `--expert`, `/v1/models` lists the configured `engine.model_id`
  only (seen for both models), not the whole inventory as the README says — a doc/behaviour
  mismatch, not investigated further.
- **Resume §8 item 5, measured** (`out/stop-latency.ps1`, Swift, `v0.2.2` engine): a 16,401-token
  prompt + 8 tokens in 6.25 s end to end (≥ 2,600 tok/s overall); a client that disconnects
  2 s into a fresh long prefill is logged `client_gone`, and the next short request finishes
  only 305 ms slower than an idle one (1,911 vs 1,606 ms) — STOP cuts the prefill within about
  0.3 s (budget `stop_drain_timeout_s` 5 s). READY: `/health` 200 after 39–48 s on every start
  (limit `ready_timeout_s` 900). Note: an identical prompt sent twice is reused by the engine
  (no second prefill), which a first attempt at this measurement ran into.

29. **Two `http_socket_server_tests` cases fail deterministically under MSVC on Windows** —
    **fixed in the test harness (step 10), two different causes, both measured.**
    `head_terminator_split_across_reads` and `send_all_bounded_stall_restarts_on_progress`
    (3 of 3 runs). First run ever under MSVC (the file did not compile before, issue 23); not
    caused by the `small` rename (that is in `connected_pair`, :835).
    (a) The head case: `Sleep(2)` takes 15.84 ms here (100 × measured), so the byte-paced
    header (~100 bytes) overran the fixture's 1 s header deadline. The test binary now asks
    for 1 ms timer resolution (`timeBeginPeriod(1)`, per-process since Windows 10 2004,
    `winmm`): 1.15 s fail → 0.42 s pass. (b) The stall case was **not** timing: it failed
    at 2.3 s, before its 10 s deadline, with `stalled`. Winsock takes a whole
    `kMaxSendChunk` (64 KiB) slice per non-blocking `send`, and the case's 1-byte client
    receive buffer throttles loopback TCP to ~120 KB/s, so one slice drains in ~540 ms, past
    the 200 ms bound (`out/tcp-window-gap.ps1`: 14 sender gaps ≥ 200 ms, max 540 ms; with a
    4 KiB buffer: none, max ~90 ms, 1.3 s in all). On Windows the case now uses a 4 KiB
    client buffer, which still keeps the 1 MiB payload waiting on the reader and still
    outlasts the bound (Linux shape unchanged). Both pass 3/3. Product note: on Windows the
    write-stall clock ticks per 64 KiB slice, so a response is cut when one slice takes
    longer than `write_stall_ms` (default 10 s, i.e. a client reading below ~6.5 KB/s);
    SSE events are far smaller than a slice. Not changed.

### 10. The 32 Windows failures, triaged one by one (after `324191af`)

Until now the 30 baseline failures were grouped by a guessed cause ("Windows-host"). Each
was read to its root cause. Two were product bugs, one a gap in our own supply-chain gate,
the rest test-harness portability. Each fix keeps the Linux shape of the test unchanged.

| Tests | Root cause | Kind | Fix |
|---|---|---|---|
| `server_cli_tests` serve × 2 | `strata.exe` never starts Winsock, so `strata serve` cannot bind on Windows (issue 30) | **product bug** | `WinsockGuard` in `strata_main.cpp` |
| `cli_verbs_tests` Task Scheduler × 1 | bare `schtasks` searched cwd/PATH; the case's `.cmd` shim never ran, the real schtasks did (issue 31) | **product security bug** | System32-only resolution, test-first |
| `egress_tests` imports × 1 | MSVC's `ws2_32.lib` links Winsock 1.1 by ordinal; the tripwire failed closed on every ordinal (issue 32) | gate gap (test) | resolve through the system `ws2_32.dll` export table |
| `api_server_tests` × 6 | the fake engine wrote its record in text mode (`STOP\r\n`); the cases match `\nSTOP\n` | harness | record in binary mode |
| `engine_supervisor_tests` vision × 4 | same, in the fake vision child (`SPAWN\r\n`) | harness | record in binary mode |
| `update_cli_tests` × 7 | the fixture built Linux symlinks; on Windows the product's D-4 pointers are `current.txt`/`previous.txt` | harness | fixture writes/reads the Windows pointers — first Windows runtime proof of `strata update` install, decline, digest/tamper refusal, mirror fallback and rollback |
| `typed_config_tests` × 4 | POSIX absolute literals (`/state/slots`) are drive-relative on Windows and correctly refused | harness | host-absolute literals (`C:/state/slots`) |
| `service_ops_tests` disable × 1 | the case never created its task directory (Win32 error 3) | harness | create it |
| `service_ops_tests` systemd golden × 1 | the systemd renderer used the host's absolute rule (a unit's paths are POSIX on any host) | product (pure function) | POSIX rule (leading `/`), identical on Linux |
| `acquire_tests` key show × 1 | Windows stores the token DPAPI-protected (`STRATAK1` + blob): plaintext is in no file | harness | Windows asserts 0 plaintext files and the DPAPI header |
| `api_client_tests` probe × 1 | the probe was started with an empty environment; Winsock cannot start without `SystemRoot` | harness | pass `SystemRoot` |
| `http_socket_server_tests` × 2 | issue 29 (a) timer resolution, (b) 1-byte window vs 64 KiB slices | harness | see issue 29 |
| `api_server_tests` DOS-02 idle × 1 | the process handle total includes the OS thread pool's own objects (issue 33) | harness | compare the per-request kinds exactly |

30. **`strata serve` cannot bind on Windows** — **fixed.** `secure_server_main.cpp` holds a
    `WinsockGuard` (spec §10: Winsock once per process, `bind()` never calls `WSAStartup`);
    `strata_main.cpp` did not, and `strata serve` reaches the same
    `HttpSocketServer::bind`, so `socket()` failed and the boot printed "Unable to bind secure
    listener." (the GPU sessions above used `strata-secure-server`). RED: the two
    `server_cli_tests` serve cases (they always failed here); GREEN after the guard. Note: the
    bootstrap captures the bind error text and does not print it (diagnosability; not changed).
31. **Bare tool names ran from the current directory or PATH (CWE-427)** — **fixed test-first.**
    `service::run_argv` handed `CreateProcessW` a bare `schtasks` (and `doctor`'s
    `nvidia-smi`), which searches strata's own directory, then the **current directory**,
    then System32, then PATH. The code comment said "searches PATH, like execvp". A
    `schtasks.exe` planted where a user runs `strata service enable` would run in their
    session. Now a bare name runs only as `GetSystemDirectoryW()\<name>.exe`, passed as
    `lpApplicationName`, or not at all. Both tools ship in System32 (checked on this box).
    New case `service_ops_tests.run_argv_never_runs_a_planted_tool`. RED: the copies planted on
    PATH and in the current directory both ran; the latter only once the case cleared
    `NoDefaultCurrentDirectoryInExePath=1`, which this session's harness sets and a user's
    shell does not. GREEN: 14/14 service and CLI cases.
    The Task Scheduler CLI case had been running the **real** `schtasks` on this machine all
    session: `CreateProcessW` never runs a `.cmd`, so its PATH shim was never found. Its
    `/Create` exited 1 every time, so nothing was registered (`schtasks /Query /TN strata`:
    not found). It now uses a test-only fake (`tests/fake_system_tool_main.cpp`) through a
    hooks-only `--test-system-dir`, like the other hooks-only options. **Unproven:** whether
    the real `schtasks /Create` accepts strata's task XML for a standard user. It exited 1
    with the cause unrecorded. Proving it registers a real logon task, so it is owner-run.
32. **The engine binary gate (C5) ignored imports by ordinal** — **fixed test-first** (our
    own gate). `engine_review.py`'s PE parser dropped ordinal-only thunks, so e.g.
    `CreateProcessW` imported from kernel32 by number would pass both the name rule and the
    text scan. Now every ordinal import fails unless the allowlist rules it
    (`allowed_ordinals`). RED: a synthetic PE importing kernel32 #291 passed the gate. GREEN:
    10 gate tests, and the gated `v0.2.2` engine (`strata.exe`, `strata_kq.dll`) still passes
    (it imports nothing by ordinal). Contract C5 text updated.
    The C++ egress tripwire (`tests/support/binary_imports.hpp`) had the opposite problem: it
    failed closed on every `WS2_32` ordinal, which MSVC links for the Winsock 1.1 functions.
    It now reads the export table of the system `ws2_32.dll` (the one the loader binds) and
    applies the name rule. An ordinal it cannot name still fails. New synthetic export-table
    cases go through the real reader.
33. **DOS-02 idle check on Windows** — harness, measured. Handles by type
    (`out/handle-types.ps1`, NtQuerySystemInformation): over 360 cancelled chats the server's
    File handles (sockets, pipes, files) stay at 27 and Process at 1. The total drifts +5/+6
    in Thread, Event and WaitCompletionPacket, the OS thread pool's own objects (the product
    uses no thread pool; its one per-request event, `write_line`'s, is closed on every path).
    A 480-chat run shows no per-request growth (175 → 180, 180, 180, 181). On Windows the
    case now compares the per-request kinds exactly (`request_handle_count`: File + Process,
    failing closed if none can be typed). Linux still compares every fd.
34. **Windows `send` granularity** — note (issue 29 b): the write-stall clock ticks per 64 KiB
    slice on Windows.
35. **`run_argv` (Windows) drains stdout to EOF before stderr** — note. A tool that filled the
    stderr pipe first would deadlock it; schtasks and nvidia-smi print a few lines. The POSIX
    side reads both alternately. Not changed.

36. **Latent use-after-free in three `typed_config_tests` cases** — **fixed (test bug).** The
    first full run after these fixes crashed once in
    `cfg01_derived_defaults_follow_worker_count` (SEGFAULT; 5/5 alone passed).
    `config_of(with_literals(…))` returned a view into a temporary that died at the end of
    the statement, and the next lines read through it. The file already recorded the same bug
    fixed once in the sampling case. Fixed there and in two sibling cases (`…one_worker…`,
    `…explicit_values…`): the result now outlives the reads.

- **Full preset suite after these fixes** (`ctest --preset windows-msvc`,
  `out/ctest-preset-harness2.xml`, clean `/W4 /WX` build): **867 tests, 866 passed, 0 failed,
  1 skipped** (`cfg03_rejects_symlink_below_root`, symlink privilege), in 242 s. The first run
  (`out/ctest-preset-harness.xml`) had the one crash of issue 36. Repo pytest: 55 passed (+3
  gate tests), the same 7 host failures (bash/WSL, cross tree), 4 skipped. RED evidence:
  `out/ctest-red-cwe427.*`, `out/pytest-red-ordinal.log`; the serve and harness REDs are the
  failing cases of `out/ctest-preset-final.xml`.

### 11. IQ3_S served on `0.0.0.0:8080` for the owner's own testing (owner request)

- Binary: `out/frontend-msvc-serve` (current source, `BUILD_TESTING=OFF`,
  `SECURE_FRONTEND_TEST_HOOKS=OFF`, `/WX`; no hooks marker in the exe), not the hooks-enabled
  preset tree. Config `out/serve/config-iq3s-lan.json`: the IQ3_S config with
  `listener.host` `0.0.0.0`, `allow_remote`, `allow_wildcard_bind`, and `host_aliases`
  `<this PC's LAN address>[:8080]`, `<pc-name>:8080`, `127.0.0.1:8080`, `localhost:8080`;
  `--check` valid. Started by `out/serve/serve-iq3s-lan.ps1` (token read from
  `api-token.txt`, never printed; log `out/serve-iq3s-lan.log`).
- Through `http://<this PC's LAN address>:8080` from this machine: READY after 24 s; `/v1/models` lists
  the one model; a chat answered "The capital of France is Paris." (`stop`, 2.4 s, reasoning
  split out). No token → 401 `invalid_api_key`, a foreign `Host` → 403. Requests from the LAN
  address are logged `remote`.
- Not proven: a connection from another machine. The Ethernet network is a **Public** profile
  with `NotifyOnListen` on, so Windows asks the owner to allow `strata-secure-server.exe`. No
  firewall rule was read (needs admin) or changed.

### 12. Built-in chat page and the keyless loopback mode (owner decisions, test-first)

The owner asked for a page to land on, with the API key as its login, and then for "an
explicit opt-in no-key mode for the page and API, loopback binds only". The secure server
was API-only by design (privacy-migration.md: no web UI; threat model: an unauthenticated
mode only as an explicit loopback-only switch with a warning). Both are now built that way:

- **`listener.web_ui`** (off by default): a chat page at `/`, `/ui/app.js` and
  `/ui/app.css`. These are three ASCII files in `secure_frontend/web/` (LF via
  `.gitattributes`), embedded at configure time by `cmake/EmbedWebUi.cmake`, which refuses
  non-ASCII and CR. They are public only at those exact `GET` targets, after the Host and
  Origin checks, and outside `kRouteTable` (which stays the authenticated API).
  - The page's responses carry their own CSP (self-only script, style and connect; no
    inline code; `form-action 'none'`; `frame-ancestors 'none'`) through a new
    `HttpResponse::content_security_policy` field. It replaces the fixed policy once; the
    reserved-header guard still drops a CSP set in `headers`.
  - The page uses the key as its login: checked against `/v1/models`, kept in
    `sessionStorage`, sent only as a same-origin bearer header with cookies off.
  - It streams the chat with the reasoning shown separately, has Stop, and renders text
    only.
- **`listener.loopback_without_key`** (off by default, config file only): no key for the
  API or the page.
  - At load it is refused for a non-loopback host, and refused next to a `token.source`
    (the file, `--token-file` or `--token-env`).
  - At bind the core `validate_config` accepts a keyless listener only with the opt-in
    *and* a loopback address. `HttpSocketServer::bind` re-applies that.
  - The boot prints a `strata: warning:` line, the ready line says
    `no API key: loopback only`, and `--check` says `token: not used`.
  - Host, Origin and Content-Type checks still apply. The page detects the mode (the
    model list answers without a key) and skips the login.
  - `server_cli_tests`' switch inventory now names it as the one switch that turns the
    key off, and checks that it has no flag and is off by default.
- **TDD:** 11 new cases.
  - Router (6): off by default; page served without a key; every API route still keyed;
    Host/Origin still checked; the files bundled and inert; the keyless router.
  - Core config (1), wire (1), typed config (1), and over a socket (2: the page plus the
    keyed API; keyless serving, with Host/Origin refusals and a `token.source` refusal).
  - RED: 10 failed for the right reason (`out/ctest-red-webui.*`). One test expectation
    was corrected: the parser already refuses `?` in a target with 400, not 401. GREEN:
    all 11 pass.
  - Full preset suite: **878 tests, 877 passed, 0 failed, 1 skipped**
    (`out/ctest-preset-webui.xml`). README table regenerated from the schema; README,
    threat model and privacy-migration row updated.
- **Browser check (built-in browser, this box, real model):**
  - Keyless loopback (`out/serve/config-iq3s-local-keyless.json`): `/` went straight to the
    chat ("No API key needed (loopback server)"). A question streamed back with the
    reasoning collapsed: `stop | 73 tokens, 19.2 tok/s | 3.8 s`. The console was clean.
  - Keyed LAN server (`config-iq3s-lan.json` plus `web_ui`, `0.0.0.0:8080`): `/` showed
    the key login. A wrong key got "That API key was not accepted." and the URL stayed
    clean. The dev key signed in, and Stop mid-essay was logged
    `chat_stream 200 client_gone`.
  - A reload stayed signed in, and the next chat answered at once. The only console lines
    were the two expected 401s.
  - Note: in keyed mode each page load makes one probe without a key (to detect keyless
    mode), which the request log shows as one `other 401`.
- **Left running for the owner:** the LAN server with the page, `http://<this PC's LAN address>:8080/`
  (terminal tab, `out/serve/serve-iq3s-lan.ps1`). From another machine, Windows Firewall
  must allow `strata-secure-server.exe` (Public profile; not changed here).

### 13. Long contexts to upstream's scale, and served model names (owner requests)

- **1M contexts** (owner: "Strata can go to 1M context using rope scaling yarn"). Upstream
  serves a 256 MiB body by default ("a million-token conversation") and documents YaRN runs
  at 421K, 714K and 1M (`--rope-scaling yarn --rope-scale F`, F = context / 262,144). Our
  frontend capped well below that, below the config too.
  - The prompt renderers allowed 128 messages, 64 KiB a message and **512 KiB a prompt**
    (≈128K tokens of text). `EngineProtocolLimits` allowed 262,144 prompt, new and
    generated tokens. The schema maxima mirrored those (spec §9.5).
  - Now: renderers 65,536 messages and 256 MiB a message (prompt: that plus 16 MiB of
    framing); protocol 1,048,576; schema maxima `max_prompt_tokens`,
    `max_completion_tokens` and `default_max_tokens` 1,048,576, `max_messages` 65,536,
    `max_message_bytes`, `max_body_bytes` and `max_request_bytes` 256 MiB.
  - **Defaults unchanged** (48 KiB body, 32K prompt), so an install opts in by config.
  - The GEN line for ~1M six-digit ids is ~7 MB, under the 16 MiB protocol cap. YaRN
    needs no code: `engine.args` passes `--rope-scaling`/`--rope-scale` through (only
    frontend-owned flags are refused).
  - Test-first: 5 cases RED then GREEN (renderers ×2, protocol defaults, config ranges ×2).
- **Served model names** (owner: "name the models better instead of the filename").
  Upstream serves `qwen3.8-flash-next` (`default_model_name`, its `model_name` key), not the
  file. New typed `model.name` (the alias grammar, ≤64 characters): `/v1/models` lists it,
  the chat page shows it, requests name it. Unset keeps the inventory file or `name:tag`.
  The `model_name` parity row is now typed (`frontend behaviour`). Test-first: RED (the file
  name was still served), GREEN.

### 14. The page with upstream's look; per-request thinking; served at 128K (owner requests)

- **Design** (owner: "the same look and feel as the upstream but I want the api login and
  new chat button"). Upstream's UI (`serve/web/`, MIT; the Outfit font SIL OFL) was read
  and its kit copied: tokens, components, icon sprite, favicon, font. The sprite's C2PA
  metadata blob is dropped, and the text is converted to ASCII and LF (provenance and
  SHA-256s in `docs/PROVENANCE.md`).
  - Markup and layout follow upstream: header tabs, status pill, theme switch, bubbles,
    Markdown and code blocks, the "Thinking" collapsible, the composer with attach,
    export, sampling, Stop and Send, the sampling drawer, and About.
  - Added: our API-key sign-in card and a labelled New chat button.
  - Dropped: Monitor, Images, MCP and server settings (routes this server does not have),
    the GitHub link, Top-k (the schema refuses it), and inline script (the theme
    pre-paint moved to `/ui/theme.js`).
  - The Markdown renderer builds DOM nodes only. The embed step now carries binary assets
    (the font); text assets are still ASCII and LF only. CSP adds `img-src 'self'` and
    `font-src 'self'`. `.gitattributes` keeps `web/` at LF and the fonts binary.
- **Per-request `reasoning_effort`** (the drawer's Thinking control; upstream sends it per
  message): `none|low|medium|high` on the qwen profile (`none` closes the think block).
  A profile without it refuses it by name. Test-first: schema and socket cases RED, then
  GREEN.
- **Tests:** the page cases now pin the 10 assets and their types, the absence of inline
  code and HTML sinks, the WOFF2 fonts, the symbols-only sprite, and the theme-only
  storage. Full preset suite **885 tests, 884 passed, 0 failed, 1 skipped**; repo pytest
  55 passed, the same 7 host failures, 4 skipped. The page's later script fixes (below)
  re-ran the 10 page cases.
- **Served at 128K** (`out/serve/config-iq3s-lan-128k.json`):
  - The engine runs `--max-context 131072 --kv int8 --kv-resident 32768`. READY in 45 s,
    and `/v1/models` lists `Qwen3.8-Flash-Next` (`model.name`).
  - Resources at idle: GPU 15.7 of 16 GB, RAM free ~4 GB, **commit 77.4 of 78.3 GB**
    (issue 17's headroom, now nearly gone).
- **Browser check** (built-in browser, real model):
  - Sign-in card, chat with Markdown (heading, 3-item list, `python` code block, thinking
    collapsed after the answer started): `Qwen3.8-Flash-Next - 220 tokens - 94.7 tok/s -
    5.1 s`. Thinking Off answered with no thinking block. Reload stayed signed in, and New
    chat cleared.
  - Stop, then a new question answered (the stored history alternates). The About facts
    were right, and the theme switched and was remembered.
  - No CSP violation in the console; the font, sprite and favicon all loaded.
  - Two page bugs found and fixed: a turn with no answer (stopped or refused) left the
    question in the history, so the next question was refused as two user turns in a
    row; and a speed of 40,000 tok/s for a 4-token reply that arrived in one chunk (a rate
    is now shown only from 16 tokens and 0.25 s).
37. **The first two thinking-off requests after a 128K start took 60-70 s** — observation.
    `"What is 17 times 23?"` with `reasoning_effort: none` (35 prompt tokens, 4 out): the
    engine at 99% GPU, no token for 62 s (the page's request, stopped), then 69.8 s (curl).
    After that the same request took 2.2 and 1.7 s. After a restart it took 2.0 s, and
    thinking on 2.8 s. Not reproduced. A plausible cause is cold experts read from disk
    under memory pressure (~4 GB free); not proven.
38. **128K on this box leaves almost no commit headroom** — open (owner). Commit charge
    77.4 of 78.3 GB at idle with the IQ3_S pack. Longer contexts (256K, or 1M with YaRN)
    need more pinned host memory for the KV cache (~13 KB a token), so on this box they
    need a larger page file or a smaller pack first (issue 17).

### 15. A link or "data:" in an answer broke the next turn (owner bug report)

39. **"The request is malformed or not supported. (invalid_request)" after the first
    answer** — **fixed, test-first.** SP0's `valid_content` refused any message text
    containing `http://`, `https://`, `file://`, `mcp://` or `data:` (case-insensitive,
    anywhere). The page sends the earlier answers back as history, so an answer with a
    link, or prose such as "Training data: …", made every later turn a 400. Reproduced
    with curl.
    - SP3 §6.2 (PRIV-03) allows such content to be "rejected or treated as text", and the
      server never fetches message text. So text is now text, verbatim, in every role.
      The model field keeps the scheme guard, and image URLs keep their own gate (data:
      only, never fetched).
    - Tests: new `chat_schema_tests.remote_looking_text_is_text`. Two cases change from
      "refused" to "kept as text": `http_parser_tests.dos01_json_limits`, and
      `egress_tests.egress_command_like_values_trigger_nothing`, which now asserts the five
      texts are served (200) and reach the engine exactly five times. Its egress checks are
      unchanged: no process started, no acquire or update route, no outbound attempt
      under the interposer.
    - RED (3 failing), GREEN; full preset suite **886 tests, 885 passed, 0 failed, 1
      skipped**.
    - Checked: the failing curl request now answers in 4.0 s. In the page, "the Python
      website URL" (`https://www.python.org`) then "which language?" (`Python`) both
      answered.

40. **The same error survived the issue 39 fix and a page refresh (owner report)** —
    **fixed in the page.** The page keeps the chat in sessionStorage, and a refresh
    restores it. A chat saved by the page before its step-14 history fix could hold a
    question that never got an answer (the one the issue 39 rule refused). Every later
    request then sent `user, assistant, user, user`, and the Qwen renderer takes only
    strict alternation, so every turn was a 400.
    - Reproduced in the built-in browser: that saved chat gave the owner's exact log line
      (`chat 400 invalid_request 2 488`) on every turn, across reloads.
    - Fix (app.js): one `conversation()` rule for what is sent, saved and loaded. Turns
      alternate; a newer question replaces an unanswered one; an assistant turn only
      follows a question. A loaded chat never ends on an unanswered question. The server
      rule is unchanged.
    - Checked: the same saved chat now loads as `user, assistant`. The next two
      questions answered (200, 200). Page and router tests: 18/18. No CSP violations.
      There is no JS test harness here: the browser reproduction was the RED, the
      same run after the fix the GREEN.

### 16. Upstream's Monitor tab and its four thinking levels (owner requests)

Owner: "it is missing the monitor section from the upstream", then "the upstream has
thinking off, low, medium and high".

41. **The page had no Monitor; this server had no `/metrics`.** Upstream's Monitor tab
    polls its own `GET /metrics` (serve/server.py `metrics()`), which reads the GPU through
    NVML loaded in-process (serve/telemetry.py). Added as **`listener.monitor`** (off by
    default, config file only), following privacy-migration.md's rule for upstream's
    metrics: "opt-in, token-protected, and documented".
    - **Route:** `GET /metrics` is a keyed `kRouteTable` route. While the switch is off it
      answers 404. The JSON has upstream's shape.
    - **Meter (`ServeMonitor`):** the chat route tells it when a generation starts and how
      it ended, on both paths, and `drive_generation` tells it each token and the engine's
      `DONE` figures. `DoneEvent::figures` now carries the record's prompt tokens,
      prompt_ms, decode_ms and reused count. It keeps the last 100 generations and totals,
      never any text.
    - **Host readings:** CPU and RAM come from the OS. GPU 0 comes from
      `System32\nvidia-smi.exe` (`/usr/bin/nvidia-smi` on Linux), run as its own child
      through `EngineStdioProcess`: fixed argv (`--loop-ms=1000`), no shell, empty
      environment, the Job. Its csv is parsed strictly and nothing from NVIDIA loads into
      the server. A one-second sampler keeps 60 samples a series.
    - **Page:** upstream's Monitor tab, ported from its `renderMonitor`: state badges,
      speed / GPU load / VRAM / GPU temp / power / PCIe / CPU cards with sparklines,
      context gauge, GPU memory / RAM / temperature bars, recent requests and totals. It
      is built from DOM nodes; upstream's uses `innerHTML`, which our page tests forbid.
      Left out: disk read, experts in VRAM, VRAM hit rate, conversation cache and MCP (no
      data here).
42. **The four levels were one level on our side, and our prompt lacked the template's
    effort sentence.** The page had Thinking Off / On. The server took
    none/low/medium/high but told the three thinking ones apart nowhere. The model's own
    template (`C:\strata-data\packs\iq3_s\tokenizer\chat_template.jinja`, same effort
    block as upstream's `serve/chat_template.jinja`) puts a fixed sentence at the top of
    the system turn: "Reasoning effort is set to xhigh. ..." for high, which is also the
    default, a low sentence for low, nothing for medium, and nothing with thinking off.
    So every thinking request we rendered differed from upstream's prompt.
    - Fixed: `render_qwen_text_prompt` takes a `QwenEffortPlacement` and writes the
      template's sentences byte for byte.
    - With `reasoning.effort_position = "end"`, and only when the engine took
      `--tail-role-token`, it renders upstream #458: the conversation as high, and a low or
      medium sentence in a short system turn before the answer (`render_prompt`,
      server.py:4557).
    - The route passes the request's level, else `reasoning.effort`, else high. The page
      offers Off / Low / Medium / High (High by default), and a saved "on" becomes high.
- **TDD:**
  - New cases: `serve_monitor_tests` ×7 (nvidia-smi parsing and fixed command, records,
    live states, bounded history, host readings, sampler against a fake tool, a new
    `--fake-nvidia-smi` mode of the test-only fake engine);
    `engine_protocol_client_tests.done_figures_reach_the_done_event`;
    `chat_execution_tests.drive_generation_feeds_the_monitor_meter`;
    `api_router_tests.metrics_route_needs_the_key_and_the_switch`;
    `typed_config_tests.monitor_row`;
    `api_server_tests.monitor_reports_generations_without_their_text` (a real socket:
    keyed, both chat paths recorded, the prompt text absent, valid JSON, 404 when off);
    `qwen_prompt_renderer_tests.renders_the_templates_effort_sentences`;
    `chat_generation_plan_tests.qwen_plan_writes_the_effort_sentence`.
  - RED: 14 failed for the right reason (stubs, an unknown config key, a 404 route, no
    sentence; `out/ctest-red-monitor.log`), then GREEN, 15/15.
  - `server_cli_tests.auth04_no_switch_disables_auth`'s switch inventory names
    `listener.monitor` (a keyed route; it reads no key).
  - The page assertions (`api_router_tests.web_ui_has_the_monitor_and_four_thinking_levels`)
    were written with the page, not before it; the browser run below is their behavioural
    check.
- **Found by the existing security tests (full run 1: 901 tests, 3 failed):**
  - `api_server_tests.ext01_upstream_control_routes_are_404_after_auth`: with the Monitor
    off, `POST /metrics` answered 405, which reveals the route. Fixed in the router (off,
    `/metrics` is no route for any method); RED first in
    `api_router_tests.metrics_route_needs_the_key_and_the_switch`. The EXT-01 test is
    unchanged.
  - `api_server_tests.web02_bound_authority_and_localhost_alias_succeed` walks every
    `kRouteTable` route on a default server. `/metrics` is opt-in, so that server now turns
    the Monitor on and the test covers `/metrics` under every authority too.
  - `typed_config_tests.readme_config_table_matches_schema`: I edited the README during the
    run. It passes after the rebuild.
  - Full run 2: **901 tests, 900 passed, 0 failed, 1 skipped**
    (`cfg03_rejects_symlink_below_root`, symlink privilege).
43. **The real `nvidia-smi` does not start with an empty environment.** On the restarted
    server the GPU cards were empty, with the note "nvidia-smi stopped". The tool printed
    "Failed to initialize NVML: Unknown Error" and exited 255; tried by hand, it needs
    `ProgramFiles` and nothing else. Fixed: the child's whole environment is `ProgramFiles`,
    read from `HKLM\...\CurrentVersion\ProgramFilesDir`, never from the server's own
    environment.
    - Tests: `serve_monitor_tests.nvidia_smi_environment_is_program_files_only`, RED then
      GREEN. `serve_monitor_tests.gpu_sampler_reads_the_real_tool_when_present` runs the
      server's exact command against the real tool where one exists; it was written after
      the fix, and would have caught the bug. Monitor tests 9/9.
    - `api_server|serve_monitor|server_e2e|egress` rerun: 103 tests, 102 passed.
      `idle_unload_respawns_before_the_next_request` (an idle-window timing test, Monitor
      off) failed once while the 128K server was running. It then passed 3 of 3 alone, and
      it passed in both full runs. That is a timing flake, not this change.
- **Checked on the owner box** (IQ3_S, 128K, 0.0.0.0, `listener.monitor` on, built-in
  browser):
  - The Monitor tab shows the RTX 4080 SUPER's load, VRAM 15.4 of 16 GB, temperature,
    power of 320 W, PCIe Gen4 x16 (idle Gen1), CPU over 24 threads, RAM 60.0 of 64 GB, and
    sparklines.
  - The same "hi" at each level, through the page's drawer: the engine reported prompts of
    13 tokens (Off), 65 (Low), 11 (Medium) and 90 (High). Those are the closed block, the
    low sentence, no sentence and the xhigh sentence. All four ended `stop`, with the
    engine's own timings.
  - The table and totals show the four requests; the context reads "117 / 128K"; no CSP
    violation or script error.

### 17. Gemma 4 on llama.cpp's own server, test-only (owner decision)

Gemma 4 cannot run on our server: upstream's engine accepts only `qwen4exp`/`qwen35moe`, and
our own Gemma 4 engine exists only as a spec
(`docs/superpowers/specs/2026-10-22-gemma4-engine-design.md`). The owner asked to test the
model anyway. On my question, "Yes: build llama.cpp's llama-server from the pinned 3cf03257
checkout (test-only, outside our secure server), run the Q8 Gemma file on 0.0.0.0:8081 with
that API key".

44. **Built and ran code that the supply-chain contract has not reviewed**, by the owner's
    explicit decision. llama.cpp's `libllama`, `common` and server (about 100,000 lines) were
    never in our compiled surface. C3/C4 were not run; C1 was (checkout at `3cf03257`, clean
    before and after). It is a test tool, not part of the product: nothing of it is committed,
    shipped or started by our server.
    - **Build** (`out/build-llama-server.bat` → `out/llama-server-3cf03257/bin/llama-server.exe`,
      60.9 MB, static):
      - CUDA 13.0, `sm_89`, Ninja + MSVC v143, built out of tree.
      - Every build-time fetch was off: the web UI (no npm build, no Hugging Face bundle, so
        the server has no page), OpenSSL (no HTTPS model downloads), CCCL, the OpenMP
        installer, llguidance. Tests and examples were off too.
      - The log shows no download; the UI step reports "embedded 0 assets".
    - **Run** (`out/serve/serve-gemma4-llamacpp.ps1`, log `out/serve-gemma4-llamacpp.log`,
      terminal tab "gemma4 llama-server 8081"):
      - Listens on `0.0.0.0:8081`, context 32768, one slot, flash attention.
      - Experts: layers 0-18 in RAM (memory-mapped), layers 19-29 and every non-expert
        tensor on the GPU (`-ngl 99 --n-cpu-moe 19`).
      - The key is our dev token, passed as `LLAMA_API_KEY` (not on the command line).
      - Off: `--no-agent` (no tools, no MCP/CORS proxy), `--no-slots`, `--no-webui`. Any
        `LLAMA_ARG_*` variable is cleared first.
      - It is **not** behind our frontend: no Host/Origin checks, no request limits, and
        `/health` answers without a key (llama.cpp's design).
    - **Firewall:** an inbound Allow rule for `llama-server` (Public profile; this network
      is Public) exists from Windows' first-run prompt. I neither created nor changed it.
    - **Checked:**
      - Loaded in 8 s; VRAM 14.97 of 16.4 GB.
      - Without the key: 401. `/v1/models`: `gemma-4-26b-a4b-it`.
      - Thinking off (`chat_template_kwargs: {"enable_thinking": false}`): a correct
        one-sentence answer, 59 tokens, 30.6 tok/s decode.
      - Thinking on: reasoning returned apart in `reasoning_content`, 24.2 tok/s.
      - A request that names neither thinks (it is llama.cpp's `--reasoning auto`).
      - `http://<this PC's LAN address>:8081/health` answers.
    - **llama.cpp's own warnings:** "detected an outdated gemma4 chat template, applying
      compatibility workarounds"; `<|tool_response>` and `</s>` retyped as control tokens.
      Neither is our concern for this test.
    - **To stop it:** Ctrl+C in its tab.
    - **What comes after it:** `/` on 8081 is a 404 (no UI was built). The owner wants one UI: the
      8080 server and its page, with engines behind it, not a page served by llama-server. Asked
      whether a thin llama.cpp-backed engine should carry Gemma behind 8080 in the meantime,
      the owner ruled it out ("Keep to the Strata-style port in the spec; Gemma joins the
      8080 UI when that engine is done"; spec D12). This llama-server remains a test tool only.

45. **The frontend's Gemma 4 tokenizer agrees with llama.cpp's on the real vocabulary**
    (Gemma 4 plan 1 Task 7, 2026-10-22). Nothing from the model is committed: the fixture
    `secure_frontend/tests/fixtures/gemma4_tokenizer_texts.json` holds texts and token ids only.
    - **Pack:** `tools/secure/secure_tokenizer.py --profile gemma4 --gguf
      C:\Strata-data\models\gemma4_q8\gemma-4-26B-A4B-it-UD-Q8_K_XL.gguf --out out/gemma4-pack`
      read the metadata only (15,777,400 header bytes, SHA-256 `9f281a1e…f1143b`; no tensor
      data) and wrote `out/gemma4-pack/tokenizer/` (262,144 tokens, 514,906 merges; 17 CONTROL,
      7 USER_DEFINED, 256 BYTE tokens) in under 2 s.
    - **Folder permission:** like `out/serve/` (issue 18), `out/gemma4-pack` was made
      owner-only (`icacls … /inheritance:r /grant:r <user>:(OI)(CI)F`, children reset to
      inherit); with the inherited `Authenticated Users: Modify` the loader refuses it ("must
      be an existing private directory"), as designed. Only that folder was changed.
    - **Reference ids:** `llama-tokenize` built test-only in the same out-of-tree build as step
      17 (`cmake --build out/llama-server-3cf03257 --target llama-tokenize`, 2 of 2 steps; no
      download), run as `llama-tokenize -m <gguf> -f <text file> --no-escape --ids --no-bos
      --log-disable` with `CUDA_VISIBLE_DEVICES=-1` (it loads the vocabulary only; with no
      device visible it never touched the GPU: "no CUDA-capable device is detected").
    - **Result:** 64 texts (ASCII prose, leading/trailing/double spaces, `\n`/`\n\n`/`\n\n\n`
      runs, `\r\n`, tabs, code, numbers, CJK, Korean, emoji with ZWJ and skin tone, combining
      marks, Arabic and Hebrew, NUL/C0/DEL, NBSP, U+2028/2029, U+3000, U+200B, long words,
      5,000 `x`, the template's own `system\n`/`user\n`/`model\n`/`thought\n`): llama.cpp's
      ids equal `tools/secure/spm_bpe.py`'s for every one (no disagreement); 27 texts holding
      a Gemma control literal (all 24 markers, and three injection attempts) are refused by our
      encoders. The C++ case `tokenizer_pack_tests.gemma4_real_pack_matches_the_fixture`
      (`SECURE_FRONTEND_GEMMA4_PACK=out\gemma4-pack\tokenizer`, label `gemma4-real-pack`)
      failed against the pre-spm_bpe tokenizer (`out/ctest-g4-t7-red.log`) and passes now
      (`out/ctest-g4-t7-green.log`, 21 s: the per-call index over 262,144 tokens and 514,906
      merges costs about 0.2 s an encode).

46. **The built-in page cannot chat with a GLM model** — open (found by the Gemma 4 plan 1
    survey, 2026-10-22; owner's call). The page always sends `reasoning_effort`
    (`secure_frontend/web/app.js`, `reasoning_effort: LEVELS[s.thinking] ? s.thinking : "high"`),
    and the glm53 schema refuses the field by name (400 `unsupported_parameter`). Plan 1 did not
    change glm53 (no GLM engine runs on this box yet). Options: the page omits the field for a
    profile without levels, or glm53 maps it to `glm.reasoning_effort`.

47. **Page chats with a Gemma model think by default** — note (Gemma 4 plan 1 Task 12). D10
    makes thinking off the default for a request that names no level, but the page always names
    one and its default is High, so a page chat thinks unless Off is picked; the drawer now says
    "Gemma models think or answer directly; Low, Medium and High all mean thinking on". API
    clients that omit `reasoning_effort` get thinking off. If the owner wants Off on the page
    for Gemma, the page needs to know the profile (today it does not).

### 18. Our own Gemma 4 engine: built, checked against llama.cpp, served on 8080 (owner request)

Owner: "keep working on this until it is ready and can serve the model for testing". Plan 1 (the
`gemma4` frontend profile) ran on branch `gemma4-plan1-frontend`; the engine of plans 2-3 was
written beside it: `engines/gemma4/` (`strata-gemma4-engine`), plan
`docs/superpowers/plans/2026-10-22-gemma4-plan2-3-engine.md`, results
`docs/validation/gemma4/2026-10-22-engine-parity-and-speed.md`.

48. **Plan reconciliations for the owner** (plan R1-R7):
    - R1: nothing of `upstream/` is compiled; the engine has its own Q8_0/BF16/F32 kernels and
      uses cuBLAS (from the CUDA toolkit) for prompt GEMMs.
    - R2: cold experts are read by the GPU from pinned RAM over PCIe; the CPU expert pool of
      D4 is not built (decode already runs at about twice llama.cpp's speed here). **Owner
      ruling wanted.**
    - R3: llama.cpp is the reference; no second, slow reference engine.
    - R4: a 2,048-row sliding ring and longest-common-prefix reuse within its spare rows.
    - R5: `--native <gguf>` (not `--model`). R6: `--kv f32` refused in v1. R7: no `cmake/` wrapper.
49. **Found on the real file:** layer 29's experts mix Q8_0 (gate_up) and BF16 (down); the first
    load stopped there, and the expert layout became per matrix.
50. **Parity** (llama.cpp `3cf03257` through the test-only `g4_llama_dump`, experts on its CPU
    backend): mean KL 0.040 against llama.cpp CUDA where llama.cpp's own CPU-vs-CUDA floor is
    0.042; top-1 22/24 (floor 21/24); on a 4,911-token prompt mean KL 0.034, top-1 23/24 and the
    same answer. The prompt and decode paths agree (KL 9e-5).
51. **Speed:** load 11-30 s, 54-57 of 128 experts a layer in VRAM, 23.5 GiB pinned; decode 47-50
    tok/s on chat answers, 77-81 tok/s after rebalancing; prompts 864-1,113 tok/s.
52. **Test-only tools** (never shipped): `g4_llama_dump` links the static libraries of step 17's
    llama.cpp build (`engines\gemma4\tools\build-g4-llama-dump.bat`); it compiles with `/W3 /WX` and
    `_CRT_SECURE_NO_WARNINGS` (upstream's tool calls `fopen`/`getenv`); the link reports LNK4098
    (LIBCMT against the /MD libraries), as the llama.cpp build itself does.
53. **Served on `0.0.0.0:8080` behind our server and page** (2026-10-23; expert mode, the
    no-hooks build of the branch `out/frontend-msvc-serve-gemma4`, config
    `out/serve/config-gemma4-lan.json`, script `out/serve/serve-gemma4-lan.ps1`, terminal tab
    "gemma4 secure server 8080"; none of it committed):
    - `engine.profile` gemma4, the engine copied to `out/serve/engine-gemma4/` (owner-only),
      `roots.models` `C:\Strata-data\models\gemma4_q8`, the pack `out/gemma4-pack/tokenizer`,
      `--max-context 32768`, `engine.gpu` 0, the CUDA `bin\x64` folder as `engine.lib_dirs`;
      `--check` passed. READY after the engine's load (about 30 s).
    - API (loopback, the dev key): `/v1/models` lists `Gemma-4-26B-A4B-it`; "What is the capital
      of France?" answers "The capital of France is Paris." (25 prompt + 8 completion tokens),
      streamed with the first token at 0.5 s; with `reasoning_effort` medium the thought comes
      apart in `reasoning_content` and the answer is right; a follow-up turn works.
    - Page (built-in browser, signed in with the dev key): High thinks (the Thinking block streams,
      then folds), Off answers directly (46 tokens), a follow-up after a thinking turn works, Stop
      ends a long thought ("Stopped"), the Monitor shows the GPU, VRAM 15.4 of 16 GB, RAM 33.7 of
      64 GB and the requests.
    - Sampling: with no temperature the frontend sends none and the engine decodes greedily, and
      the model then deliberated for minutes over a haiku's syllables. The config now sets the
      model card's defaults (`defaults.sampling` temperature 1.0, top_p 0.95, top_k 64, the
      GGUF's `general.sampling`); the same prompt then thought for 2,359 tokens and answered.
    - Speed with sampling: 35.6-36.3 tok/s decode (the host sampler copies 1 MB of logits and
      ranks them each token; greedy decodes at 47-60 tok/s). Prompt reading on short turns 39-116
      tok/s. A thinking turn followed by an Off turn reuses nothing: the system turn differs.
54. **Faster encoding and decoding** (owner: "move sampling to the GPU and cache the Gemma
    encode index, test-first, then restart the server"; 2026-10-23):
    - The frontend's spm_bpe encode tables are built once when the pack loads
      (`TokenizerPack::spm_index`, a private copy of the pack's data, shared by copies; a pack
      whose counts changed falls back to per-call tables). The real-vocabulary fixture case
      went from 21.2 s to 1.9 s for the same encodes (about 0.2 s a request saved). Full suite
      955 passed, 1 skipped.
    - The engine ranks the sampler's 64 candidates on the GPU. Measured honestly, that saved
      only about 1.9 ms a token: the 36 tok/s seen on the page came from slow expert
      rebalancing, not the sampler. Rebalancing every 8 tokens with up to 32 swaps took a
      sampled 225-token story from 29-32 tok/s to 54 (cold) and 77-85 tok/s (settled).
55. **A repetition loop on the page, and every retry repeating it** (owner report 2026-10-23:
    "what MITRE attack code is process injection" with thinking on looped "sometimes even
    sometimes even ...", twice, the same text both times):
    - Not the engine: the looping text teacher-forced through llama.cpp `3cf03257` gives the same
      distributions (mean KL 0.006 over 300 steps, the same top choice at 290), and the loop
      reproduces identically with the GPU and the host sampler and with decode rebalancing on or
      off (expert placement changes no token). It is this abliterated model sampled at
      temperature 1.0: over 32 runs each (two thinking prompts, 600 tokens), the model card's
      values looped once, llama-server's defaults (T 0.8, top_k 40, min_p 0.05) once, the model
      card's values with `min_p` 0.05 never (a repeat penalty of 1.1: 0 of 8).
    - Why the retry repeated it: a GEN without a seed used seed 0 in our engine (the q36 engine's
      rule), so the same prompt drew the same sample every time. Fixed as upstream's
      generate.cpp does (:13784): an unseeded request gets a seed from the clock
      (`g4::request_seed`, case `unseeded_requests_draw_fresh_seeds`, RED then GREEN); an explicit
      seed still reproduces. The same question asked twice now gives two different, correct
      answers (T1055 and its sub-techniques).
    - The serve config adds `min_p` 0.05 to the model card's defaults; the README recommends it.
56. **Served at 262K context; the Monitor shows the expert split** (owner: "restart it with
    262K context with kv streaming on", then "tell me the experts distribution"):
    - Our engine has no KV streaming (the upstream and GLM engines' option; the `strata_gemma4`
      allowlist refuses it). 262,144 tokens fit without it: the five full-attention layers' f16
      K/V take 5.37 GB of VRAM, at the cost of expert slots.
    - The engine's INFO record now also carries the main engine's `expert_slots` (all 3,840
      experts) and `expert_slots_primary` (those in VRAM slots), the keys the frontend's Monitor
      whitelist already reads, plus `context` (case `info_line_reports_the_expert_split`, RED
      then GREEN; engine ctest 27/27). The page's VRAM and RAM cards show "N experts loaded"
      after the first request.
    - The serve config: `--max-context 262144`, `limits.chat.max_prompt_tokens` and
      `max_completion_tokens` 262,144.

57. **The model launcher: choose, configure and start models from the page** (owner, 2026-10-23:
    "when the web ui is up, I am able to select configure models in the webui then be able to
    select settings like mtp, kv cache streaming, etc and start it up"; then "Accept the
    recommendations for D1-D10 ... write the plan and start"). Spec and plan under
    `docs/superpowers/`; commits `02ad1b02` (spec accepted, plan), `3ad44536` (Tasks 1-2: cards,
    `models.*`, options), `07c4f8d9` (Tasks 3-5: launcher, API, page; it also holds the fix that
    shows a restarted card as starting, which `0589af28`'s message lists by mistake), `0589af28`
    (box-run fixes). Local `main`, not pushed.
    - **Box setup (local files, not committed):** `out/serve/cards/qwen-iq3s.json`
      (Qwen3.8-Flash-Next IQ3_S on the upstream `v0.2.2` engine, 10 page options: context, KV
      precision, KV streaming, KV grow, MTP, draft window and confidence, expert cache, prompt
      chunk, thinking) and `out/serve/cards/gemma4-q8.json` (Gemma 4 26B-A4B Q8 on our engine, 7
      options: context, expert VRAM, thinking, sampling); `out/serve/config-launcher-lan.json`
      (the LAN server, `models.control` for loopback clients) and
      `config-launcher-local-keyless.json` (127.0.0.1:8091, no key: the page walk-through, so no
      key was typed into a browser). `--check`: both cards usable, configuration valid.
    - **The page (built-in browser, keyless instance):** the server booted with no engine
      (`/health` `engine: none`); the chat said "No model is running — open the Models tab";
      the Models tab listed both cards with their options and bounds. Gemma started from the page
      at 32K (the page showed the load and the GPU memory rising), then a chat: 337 tokens,
      64.3 tok/s. Stop from the page; Qwen started from the page with its defaults (MTP on, KV
      int8, KV streaming 32,768 resident cells at 131,072 context): 81 tokens, 87.9 tok/s.
    - **Switching through the API** (`out/launcher-box-run.log`; a chat during each load got 503):
      | start | load | reply | engine decode |
      | --- | --- | --- | --- |
      | Qwen, MTP off, KV k8v4, KV streaming 32,768 at 131,072 | 49.6 s | 175 tokens | 60.9 tok/s |
      | Qwen, MTP on, KV int8, no streaming, 32,768 context | 48.2 s | 180 tokens | 68.4 tok/s |
      | Gemma, 262,144 context (870 of 3,840 experts in VRAM) | 33.3 s | 147 tokens | 34.6 tok/s (first answer) |
      The load times include the old engine's orderly stop (no generation was in flight, so no
      drain wait) and the new engine's load; `/v1/models` then listed Gemma with `strata_models`.
    - **Found and fixed on the box** (`0589af28`): a chat naming the card being started, sent
      while its tokenizer loaded, got 409 `model_not_running` from the old model (now 503
      `model_switching`; re-check: six chats during a Qwen -> Gemma start, all 503
      `model_switching`); the Monitor kept Qwen's expert cache under Gemma (now dropped at each
      engine change; re-check: under Gemma at 32K it shows 3,840 experts, 1,620 in VRAM); top_p's
      open lower bound read "5e-324" on the page (now "above 0").
    - **Tests:** full MSVC suite at `0589af28`: 985 cases, 984 passed, 1 skipped (cfg03, the long-standing skip), 0 failed, with the real Gemma pack set (`out/ctest-launcher-full-3.log`). Every task's cases went RED first (see the commits).
    - **Left running for the owner:** `out/serve/serve-launcher-lan.ps1 -Start Gemma-4-26B-A4B-it`
      on `0.0.0.0:8080` (the no-hooks build). The script's start took the remembered 32K from the
      walk-through (D7 at work), so the card was restarted through the API at 262,144: it answers,
      870 of 3,840 experts in VRAM (`out/launcher-lan-start.log`). The remembered Qwen values are
      the walk-through's last (32K, no KV streaming, MTP on, thinking none); the page's Defaults
      button puts the card's own back (128K, KV streaming 32,768, thinking high). The Models tab
      answers loopback clients (`http://127.0.0.1:8080` on the box); from another machine it
      stays hidden until `"control_remote": true` is added to the config's `models` block (D2).
    - **Owner report the same morning: "I do not see the new tab".** The owner's browser is on
      another machine: the request log showed `strata_models 403 control_not_allowed ... remote`,
      and the page hid the tab on a 403 without a word. Two changes: the page now shows the tab
      on a 403 with the reason and the switch that allows other machines (api_router page case,
      RED then GREEN); and, with the owner's report as the ask, `"control_remote": true` is set in
      the local `out/serve/config-launcher-lan.json` (the key is still required for every call).
      Restarted with the Gemma card (262K, the remembered value); from the LAN address
      `GET /v1/strata/models` answers 200 as a remote client.

58. **Context and KV streaming as dropdowns; Qwen to 1M** (owner, 2026-10-23: "make the context length
    a drop down menu that tops out to 1M, and make the kv streaming into a dropdown selection also").
    - Both options now offer fixed values the server validates (a value off the list is 400
      `invalid_option`): Qwen's context 4K, 8K, 16K, 32K, 64K, 128K, 256K, 384K, 512K, 768K, 1M;
      Gemma's the same up to its trained 256K (its engine's limit); KV streaming Off, 20K, 32K, 64K,
      128K, 256K, 512K resident tokens. The card's min/max trim the lists. The page renders them as
      dropdowns with the server's labels and hides KV streaming values above the chosen context.
    - Past Qwen's trained 262,144 positions a start adds `--rope-scaling yarn --rope-scale
      <context / 262,144>` (factor 4 at 1M), upstream's own setup rule (its INSTALL and DETAILS
      docs; experimental there). A card that offers the context may not fix those flags.
    - Tests: model_options `context_and_kv_streaming_offer_fixed_values`,
      `past_trained_context_adds_yarn`, model_cards (a fixed `--rope-scale` refused), the page case
      `web_ui_listed_integers_are_dropdowns`, all RED then GREEN; two pinned expectations updated for
      the request (the context's description now carries its list; the qwen controlled flags include
      the rope flags) and one test value moved onto the list (KV streaming 200,000 -> 262,144). Full
      MSVC suite 988 cases, 987 passed, 1 skipped (cfg03), 0 failed (`out/ctest-dropdowns-full.log`).
    - **Box** (`out/qwen-1m-run.log`; the local Qwen card now offers up to 1M, prompt and
      completion caps 1,048,576; KV int8, KV streaming 32K, MTP on):
      | context | load | decode | RAM used (of 68.6 GB) | experts in VRAM |
      | --- | --- | --- | --- | --- |
      | 256K | 49 s | 96.5 tok/s | 62.1 GB | 4,116 |
      | 512K (YaRN 2) | 53 s | 84.5 tok/s | 65.8 GB | 3,883 |
      | 1M (YaRN 4) | 105 s | 47.5 tok/s (best of two; the first answer 7.4) | 67.4 GB | 3,412 |
      1M runs and answers coherently, but the box has about 1 GB of RAM left there; 512K is the
      comfortable ceiling on this machine. Long-context quality past 256K was not measured here
      (upstream reports needle and real-document runs to 1M with yarn 4).
    - Left serving: Gemma at 262K, as before.
59. **Model downloads on the Models tab, with pause and resume** (owner, 2026-10-23: O1-O4, D7
    "Build packs automatically", then "Have a download pause and resume functionality"; README
    "Model downloads", `docs/AGENT_HANDOFF.md`).
    - Built and tested without the network (a scripted `strata` child, a fake disk); the full
      suite in the handoff entry. `.venv-dev` got numpy 2.5.4 and regex 2026.9.10 for the
      prepare step (upstream's pack tools).
    - **Box** (a keyless loopback instance on 127.0.0.1:8092, `out/serve/config-downloads-local-keyless.json`,
      the no-hooks build `out/frontend-msvc-serve-downloads`; the store `C:\Strata-data\downloads`, 173.5 GB
      free of 511 GB):
      - Refusals: the 188.2 GB `qwen3.8-flash-next-unsloth-q8_0` 507 `insufficient_storage`; a link
        off Hugging Face and a `.safetensors` link 400 with their fixed reasons; an unknown id 404.
      - A pasted link, `Qwen/Qwen2.5-0.5B-Instruct-GGUF` `qwen2.5-0.5b-instruct-q2_k.gguf`: `strata
        hf-info` resolved `main` to `9217f5db`, 415,182,688 bytes, LFS SHA-256 `9ee36184...`. The first
        pull failed `redirect_not_allowed`: Hugging Face now answers the resolve URL with a 302 to
        `us.aws.cdn.hf.co`, three labels under `hf.co`, and SP4's wildcard covers one. Fix: the
        manifests also allow `*.cdn.hf.co` and `*.aws.cdn.hf.co` (`cdn_redirects_are_admitted`, RED
        then GREEN; SP4's rule unchanged). Then: about 3 MB/s; **Pause** at 83.8 MB (the 84,380,963-byte
        `.part` kept in the store's staging); **Resume** continued from there (not from zero) and the
        card was `installed` 123 s later; the stored file's SHA-256 equals the pin.
      - **Cancel:** `qwen2.5-0.5b-instruct-q4_0.gguf` (428,730,208 bytes) cancelled at about 10 MB:
        `cancelled`, 0 bytes, the partial file deleted (`strata rm --partials`).
      - **The page** (desktop and 375 px): Download, Pause ("Paused at ... (3%); Resume continues from
        there", Resume and Cancel), Resume (2.2 MB/s, the rate counted from the kept bytes), the
        installed and cancelled custom cards.
      - **Maya:** `glm-5.3-flash-maya-s24` failed at start with `unexpected_answer`: Hugging Face's
        revision answer for that repository carries a 10,648-byte string (the GGUF chat template)
        and `hf-info` capped strings at 4 KB. The cap is now the answer's own 1 MiB (the commit, paths
        and SHA-256s read from it keep their grammars; `hf_info_resolves_a_link` extended, RED then
        GREEN). The lookup then resolved `f16ee4ba`, 3 shards, 94.7 GB, each SHA-256 equal to its pin.
        Not downloaded (95 GB).
      - A cancel of a card that was not running answered 202 "cancelled": now 409
        `download_not_active` (RED then GREEN).
      - Not done here: a catalog Qwen download and its prepare step (58-188 GB each, the owner's
        call). Left on disk: the 415 MB test GGUF (a download-only card) and an empty staging folder
        from the failed first attempt (`strata rm --partials <id>:download` removes it).
60. **The page on a phone** (owner, 2026-10-23: "On mobile the api login is not showing up, we need to
    make sure it is mobile responsive"; then "I refreshed and still don't see the api login box in
    mobile on my iPhone").
    - **Cause:** not the layout. The request log of the LAN server shows each iPhone load (requests
      685-692 and 719-739) fetching every page file except `app.js` (the 74,247-byte answer), and no
      API call after it. `limits.concurrency.max_connections_per_remote_address` defaults to 4 per
      remote address; the phone opens up to six connections for the page's files, and the over-cap
      one is answered 503 `server_busy` without a log line. Without `app.js` no view is shown, the
      sign-in included. Loopback clients are exempt, which is why the page worked on the box itself.
    - **Fix for the LAN server:** `out/serve/config-launcher-lan.json` sets the key to 8 (the
      server's workers); `--check` passes. It needs a restart: this session's stop of the running
      server was refused by the permission check, so the owner restarts it.
    - **The default** (owner: "Raise it"): with `listener.web_ui` the per-remote-address cap now
      derives min(6, workers) instead of min(4, workers), so a page server works from a phone without
      the key (`cfg01_web_ui_raises_the_remote_cap`, RED then GREEN; the README table regenerated).
      This amends decision B11 for page servers only; without the page the cap stays min(4, workers).
    - **Layout:** signed in, the header (brand, four tabs, New chat, the pill, the theme button) needs
      about 480 px, so a 375 px phone laid the page out at 473 px and cut off its right side. Below
      640 px the header now takes two rows, the tabs sharing the full width
      (`web_ui_header_fits_a_phone`, RED then GREEN). Checked in the browser at 375 and 320 px: Chat,
      Models with Downloads, Monitor, About and the sampling drawer fit without sideways scrolling.
    - `out/serve/serve-launcher-lan.ps1` now runs a copy of the new no-hooks build
      (`out/serve/bin-lan`), so the restart brings the header fix too (downloads stay off there).

61. **The Monitor shows experts loaded as soon as a model runs** (owner, 2026-10-23: "under
    monitor it should show the number of experts not just the ram and vram"; "Every model should
    be showing the number of experts loaded along with memory used").
    - **Cause:** the page already renders "N experts loaded" on the VRAM and RAM cards, but the
      Monitor's expert tier (`ServeMonitor::experts_`) was fed only by `on_engine_stats`, which
      runs during a generation (`generation_run.cpp:210`). A freshly-started model therefore
      showed RAM/VRAM but no experts until the first chat. The engine already prints its split in
      the startup `INFO` (box, Gemma: `expert_slots=3840 expert_slots_primary=870`).
    - **Fix:** `EngineSupervisor` captures the protocol's `engine_readings()` at `READY`
      (`ready_readings()`); the launcher pushes them to the Monitor the moment the model is
      running (`model_launcher.cpp` run_start), so experts show before any chat. The GLM engine's
      live `STAT` cadence still updates them during generation.
    - **Tests:** `supervisor_captures_startup_readings_at_ready` (a `FakeChild` emits `INFO ...
      expert_slots=...`); `experts_show_as_soon_as_a_model_runs` (end to end: the supervision fake
      engine's new `--ready-experts=P,T` emits the startup INFO, the card starts, `GET /metrics`
      shows the experts with no chat). RED then GREEN. README Monitor section updated.
    - **Box:** verified live on the LAN Gemma that `/metrics` carries
      `experts:{expert_slots:3840,expert_slots_primary:870}` after a chat on the running (pre-fix)
      binary; the at-READY push lands when the serve binary is next rebuilt.

62. **Parallel ranged downloader** (owner, 2026-10-23: "Why is the download so slow? When I am
    downloading them it's much faster"; "What does the current upstreams use?"; "Do it"; "Build the
    parallel downloader and complete it"). Spec
    `docs/superpowers/specs/2026-10-23-parallel-downloader-design.md`.
    - **Cause:** our confined SP4 downloader fetched each file over one curl stream; Hugging Face's
      CDN limits throughput per connection, so a single stream is slow (~3 MB/s on the box). Both
      upstreams use a 16-connection ranged downloader (`upstream/setup.py` 0.2.0, `DL_CONNECTIONS`
      16, 32 MiB pieces, a `.part.chunks` resume; Maya reuses it via `import setup`).
    - **Built** (commits `1c271a09`, `2a982c22`, `807ecf40`, `73639063`), TDD per step: PartFile
      offset writes; a bounded `range_to`; the `download_pieces` sidecar (PieceMap serialize/parse,
      resume); the parallel orchestrator in `fetch_file.cpp` (a one-byte probe resolves the final
      CDN URL + ETag + total, N workers fetch the missing 32 MiB pieces as bounded `--range lo-hi`
      to that URL — pinned per connection, the 206 Content-Range and ETag checked, the whole file
      hashed in one pass and published, falling back to one stream without ranges or with a proxy);
      `pull.connections` (1-32, default 1) wires both the CLI pull and the model downloads (which
      run `strata pull` with the server config). A data race in the test resolver found via a
      0xC0000374 crash under ctest and fixed. `rm --partials` already removes the sidecar (it is in
      the staging tree).
    - **Box** (8092, `pull.connections` = 16): a real 429 MB pasted-link download
      (`qwen2.5-0.5b-instruct-q4_0.gguf`) over the parallel path completed and installed; the stored
      file's SHA-256 `7671c0c304e6ce5a7fc577bcb12aba01e2c155cc2efd29b2213c95b18edaf6ed` equals
      Hugging Face's pinned LFS oid (byte-correct reassembly). Throughput burst to ~67 MB/s per
      sample vs the earlier single-stream ~3 MB/s; effective parallelism for this size is ~4
      connections (14 pieces at 4 pieces/run), and sustained speed is bounded by the box's link.
    - **Open:** the download manager's `bytes` progress updates per piece-run completion (bursty,
      not smooth); per-connection ETA/rate on the page is not piece-aware.

### Not done in this session

- Resume doc §8 item 3 (`qwen35moe` family behaviour): not applicable to these packs (no
  `qwen35moe` model on the box); item 4 (cgroup delegation proof): Linux only. Item 5 was
  measured (step 9).
- The Maya open items (issue 27): penalty semantics, the `MAYA_*` inventory regeneration and
  its new rows, the `maya-vision` binary name. No GLM engine was built or run on this box.
- The supported Windows frontend builds (MSYS2 clang / llvm-mingw) and their
  `windows-runtime` ctest cases: toolchains not installed (issue 3).
- SP3 API kit, SP5 parity kit, SP7 offline kit: need a working chat path first.
- Upstream's own Python server as a baseline: its runtime requirements are not installed
  (issue 14).
