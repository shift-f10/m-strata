# SP5 M2 (Maya parity table, `glm.*` config, GLM env family, `maya_118` dialect, launch composition, acceptance kit) — FINAL REPORT

Lane: `sp5-maya-parity`, worktree `/home/opencode/projects/multi-strata/.worktrees/sp5m2`, base `749e5395`.
Upstream pin `cbfd5b92` (tag `v0.2.1.3`) — read-only, **zero** upstream files touched.
MAYA reference vendored read-only: `engines/maya-ref/` at `5c970a86…` (tag `v1.1.8`) — cited by
`file:line` only, built by **no** preset, guarded by pytest (no CMake mentions it; the distribution
check rejects any staged `maya-ref` path).

Status: **all 10 plan tasks complete**. One commit per task — `0b096f5c, 3ba36193, 7061d32f, 29e917cb,
198b9848, 92b461a9 (Task 7), 9720d5bd (Task 6), fdc7a57f, be3aaa26` — plus this docs commit (HEAD).
(Task 7 ran before Task 6 per the plan's sequencing note: "enabled by whichever lands second".)
Every commit authored `shift-f10 <40450442+shift-f10@users.noreply.github.com>`, carries
`Co-Authored-By: Qwen 3.8 Flash Next IQ3_S <noreply@anthropic.com>`, and — per the owner's 2026-10-20
ruling as adapted for this lane host — no `Claude-Session:` trailer.

## What was built (task → deliverable, one line each)

1. MAYA v1.1.8 vendored read-only as the `engines/maya-ref` submodule (D-10; the lane's only network
   use); PROVENANCE section + guards that no preset, build or package path can touch it;
2. the GLM parity table: `kMayaRows` — 178 rows (138 typed / 30 rejected / 10 mapped), every MAYA
   v1.1.8 config key, env knob and launcher flag exactly one disposition with a `file:line` citation;
3. `glm.*` typed config family in Maya's own names (`glm.gpu/gpus`, `parallel`, `layer_split`,
   `max_context` 1..1048576 default 32768 = maya.py:2524, `expert_*`, …) — bounds copied from the
   reference (C-1); `glm.*` refused when the profile is explicitly `qwen`;
4. closed typed GLM child environment (D-7): the table's 100 typed `engine_env` names are the
   allowlist (`kFamilyForcibleEnvironment`), SP6 §4.9 safety values forced, `glm_environment_knobs`
   projects typed rows, `glm_child_env_is_exactly_the_named_typed_set` pins the exact set;
5. `maya_118` wire dialect: `NOTE` after `GEN`, `STAT` cadence, 3–6-field `PP`; the dialect is
   per-manifest and enforced in the supervisor — unknown/out-of-order records still kill;
6. fake engine `--dialect=maya` + GLM generation end to end (chat over `maya_118`, cancellation with
   the READY-advertised `stop`), adversarial proof: the strata dialect kills the maya-mode child;
7. GLM launch composed from manifest + table (D-8): pack dir = parent of the selected artifact,
   `--max-context`, `--parallel` 1..8 enforced at composition, expert/split/gpu flags; the
   `glm53` **pass-through is retired** — every `engine.args` form refused with a fixed message that
   echoes no value;
8. GLM vision rows: `STRATA_GLM_VISION_LEND_MB` projects from `vision.need_mib` only for an
   on-demand turn (`vision.enabled && !resident`) and renders fail-closed without one; the bootstrap
   wires `family=glm` + gpus + knobs + the vision-turn gate for a composed `glm53` launch;
9. the acceptance kit: `strata-api-probe` corpus mode (`--corpus/--results/--command-line/
   --engine-version`; fixed schema `case,class,RESULT,detail`; C-6 classes text/shape/
   divergent-by-decision; missing results file = FAIL, never a skip via `--compare`) and `--dry-run`
   (renders the exact owner-box commands); `docs/validation/sp5-parity-kit.md` + the canonical
   corpus; PROVENANCE gains the SP5 kit placeholder row — **no GLM parity claim until the owner run**;
10. truth pass: README Maya mirror (3 blocks regenerated from the C++ table, gate extended to count
    all 178 rows non-vacuously), threat-model additions (GLM closed env allowlist, dialect rule, kit
    attestation rule), AGENT_HANDOFF SP5 M2 section + the two superseded pass-through/env-set
    passages, this report.

## Gates (final tree, dev host)

- `ctest --preset linux-gcc`: **100% tests passed out of 826** (830 registered; the 4 allowed
  environment skips). Lane growth 803 → 830 (+27 cases); per-task denominators in the ledger.
- release build + `ctest --preset linux-gcc-release`: **10/10**, build 0 warnings.
- `windows-llvm-mingw-cross`: EXIT=0, **0 warnings** at every task. wine/Windows runtime: **not run**
  (owner-hardware rule; CI disabled 2026-10-20; last CI-authorized Windows proof `windows-msys2-clang`
  at `7bddb9e1`).
- root build (`out/root-sp5m2`) + `ctest --test-dir`: **792 run / 796 registered, 100%**, 0 warnings.
- TSan (clang18 wrappers, `-fsanitize=thread -fno-pie`, pinned `llvm-symbolizer`, build
  `secure_frontend/out/build/tsan-b`): filtered concurrency suites — engine supervisor/child/stdio/
  process, connection pool, socket server, chat execution, api server — **216 cases, zero
  ThreadSanitizer reports**; the only failure is the parked flake below (1.25 s, no sanitizer report).
  Task 6 also spot-ran its three new cases under TSan earlier: clean.
- pytest: **33** (26 at M1 tip; M2 adds +3 — the maya-ref distribution guards from Task 1 — and the
  4-case kit suite `tools/secure/test_sp5_kit.py`).
- distribution check: script OK; `test_secure_distribution_check` green; any `maya-ref` path rejected.
- every C++ build: `-Werror`, 0 warnings.

## Rulings (full text in the lane ledger `.superpowers/sdd/2026-10-21-sp5-m2-maya-parity-dialect-kit/`)

- R-T2: the Maya table covers config keys AND env knobs AND launcher flags; tokenizer/`model_name`
  precedence and DFLASH licence rejects recorded there.
- R-T3: `glm.*` refused at config only when the profile explicitly says `qwen` (a manifest's own
  profile decides); `glm.layer_split` grammar-only validation until composition bounds it.
- R-T4: the allowlist is exactly the table's 100 typed env names (forcible set derived from the
  table); knob channel `EngineEnvironment.glm_knobs` is internal, never a user surface.
- R-T5: `maya_118` keeps the strata dialect's kill-on-unknown rule; flavor selection sites deferred
  to Task 7 and pinned there; NOTE/STAT fail-closed bounds.
- R-T7: engine_gate keeps `glm53` managed-OFF until SP6 (the composition runs on the `--expert`
  path today); `glm.max_context` is the typed context row; composed-flag gaps declared (§0 of the
  kit doc), retired pass-through surfaces recorded with their replacements.
- R-T8: the LEND row's typed source is `vision.need_mib`; the shared `vision.resident` default stays
  TRUE (no per-profile config fork — Maya's on-demand default is a table fact, not a default flip);
  projection AND render both gate the turn (fail closed for a smuggled knob row).
- R-T9: compare is shipped code so "missing results file" FAILs mechanically; the kit pytest's
  unbuilt-binary case is an environment skip like the lane's 4 skips (the FAIL-not-skip property is
  about results files); the canned test harness was bent to the product's strict HTTP/1.1 client,
  not the reverse.

## Parked for the owner

- The GPU acceptance run: `docs/validation/sp5-parity-kit.md` is ready; the PROVENANCE SP5 kit entry
  (command lines, engine versions, pack + results hashes) completes the GLM parity claim — until
  then no parity claim is made (threat model records the rule).
- The parked flake `api_server_tests.log_line_format_and_rate_limit`: untouched all lane; passes in
  every linux-gcc run; under the TSan build it now fails deterministically from sanitizer slowdown
  (early `GET /health` sees 503 while the fake engine is still booting) — timing only, zero
  sanitizer reports; left exactly as parked.
- Composed-flag gaps the kit omits on both sides: `--expert-profile(-save/-every)`, `--kv-resident`,
  `--spec*` have no typed value source yet; `--parallel` bounded 1..8 at composition.
- Windows runtime for every `windows-runtime` case — owner hardware; real GLM engine work is SP6.
- The lane was never pushed (agents never push). Merge of `sp5-maya-parity` onto `main` is the
  merge gate's decision.

Nothing is claimed for a GPU or for Windows runtime beyond the local cross-build and the
disabled-CI history.
