# SP6 Plan 1 — GLM evaluation kit, tokenizer prerequisite, probe

Spec: `docs/superpowers/specs/2026-10-14-sp6-glm-engine-design.md` §1.4-plan-1, §2,
§4.7–4.10, §5.5; decision sheet adopted 2026-10-22 (resume §5 item 1). Dispatch queued
**after SP7 M2**. Branch `sp6-plan1-eval-kit`, worktree `.worktrees/sp6p1`. Standard
per-task gates; TDD RED first; `-Werror`; no pushes.

## Reconciliation with merged reality (binding corrections to the spec's context lines)

- The spec predates SP5 M2. The GLM wire flavor is **already shipped** as
  `EngineProtocolFlavor::maya_118` (NOTE, STAT cadence, 5-field PP) with fake-engine Maya
  mode and e2e coverage — Plan 1 does NOT rebuild §4.2; it reuses that seam and the
  retired-pass-through, manifest-composed GLM launch.
- Config addenda: `glm.max_context` (default 32768) and the GLM env family including
  `STRATA_GLM_RAM_HEADROOM_GB` are **already in** `frontend_config.cpp` / the GLM parity
  table (178 rows). Plan 1 only verifies they match §4.9 + sheet M2 (headroom default 16)
  and adds whatever is genuinely missing — no duplicate keys; names stay as merged.
- `engines/maya-ref` (submodule @ v1.1.8) already vendors MAYA source. Plan 2's
  `engines/glm/<name>` submodule task will be RE-PLANNED against that (likely reuse
  `engines/maya-ref` as the build input instead of a second gitlink to the same repo) —
  recorded as a Plan 2 decision now; nothing here changes submodules.
- `tools/secure/secure_tokenizer.py` already wraps `glm4`/`chatglm-bpe` (with
  `test_glm_tokenizer.py`); the §5.5 fix (ignore_merges, `glm5` accepted) is genuinely
  unimplemented (grep: no `ignore_merges` anywhere).
- **2026-10-22 SP5-P2 correction (binding):** the pins moved — `engines/maya-ref` is now
  **v1.1.21 @ `451f9cb6`** and the GLM parity table was REGENERATED there (183 typed /
  33 mapped / 50 rejected / 464 out; 146-name env allowlist; `glm.*` 127 rows incl.
  `glm.idle_unload_s`), penalty window-0 rule per flavor. Cite the NEW table surface, not
  "178 rows"; `glm.*` config verification above means: re-check §4.9/sheet M2 against the
  regenerated surface. The maya_118 wire is now certified at v1.1.21 (slot-mode rejects,
  ten-field DONE) — the fake engine already emits the v1.1.21 cadence.

## Tasks

1. **Tokenizer prerequisite fix (§5.5).** `tools/secure/secure_tokenizer.py`: honor
   `tokenizer.ggml.add_bos_token`/merges correctly — implement `ignore_merges` for
   `glm4`/`glm5` pre-tokenizer names, accept `glm5`; mirror in the C++ tokenizer-pack
   path (`tokenizer_pack.cpp`) where the same rule applies. pytest fixtures + ctest cases;
   B-24 (box) confirms against MAYA's real vocabulary later.
2. **`glm53` output post-processing (§4.10)** per spec, in the frontend decode path,
   hermetic tests with synthetic transcripts.
3. **Legacy `--profile glm53` gate (§4.7).** With `SECURE_FRONTEND_TEST_HOOKS=OFF`, fail
   before spawn with the exact line
   `--profile glm53 launches an evaluation-only engine and needs a test-hooks build`;
   test-hooks builds keep working. RED first.
4. **`strata-engine-probe` (§2.1).** Test-only C++ binary in every preset, never
   installed (distribution check stays PASS — verify), launches engines through the
   product launch path (manifest-composed GLM launch + `maya_118`/`lighttransport_glm53`
   flavors, §4.8 argument profiles), one case per invocation, transcript+metrics JSON out.
   Hermetic tests use the fake-engine Maya mode from SP5 M2.
5. **`tools/glm-eval/` kit (§2).** `pins.txt` (maya `5c970a86…` v1.1.8, glm53f
   `4894c996…`, llama.cpp `3cf03257…`), `fetch-sources.sh` (bundles + wheels +
   SHA256SUMS, executes nothing fetched), `download-model.sh`, `build-pack.sh`,
   `pins-models.txt` (built by ranged LFS-pointer reads at the pinned HF revisions +
   cross-check vs MAYA `maya.py:104-200`; data-only network use from this host),
   `build-linux.sh`, `build-windows.ps1`, `run-battery.sh/.ps1`, `prompts/p01..p08.json`
   (§2.4.1 verbatim), results schema §2.5. Shell/py tests: dry-run modes, schema
   validation, no-execution audit; pytest suite.
6. **requirements-dev pin:** add pinned `numpy` (wheel for the pack tooling);
   bootstrap note line.
7. **CPU-only confined engine configure+build smoke (§2.7).** Reviewed task:
   `env -i PATH=/usr/bin:/bin unshare --user --map-root-user --net` over a read-only copy
   of the MAYA bundle sources, build dir the only writable path, CUDA/NATIVE_EXPERTS OFF
   (resolving the spec's `[to confirm]`); binaries are never run. Record outcome
   (including whether MAYA configures at all) in the ledger + PROVENANCE context note.
8. **Truth pass:** `docs/validation/README.md` SP6 row → kit paths + "defined, not yet
   run"; AGENT_HANDOFF deliverables; spec §1.4/§2.1 context lines amended with the
   reconciliation facts above (amendments folded into the spec file with dates).

## Waits for the owner's box (not lane tasks)

`download-model.sh` + `build-pack.sh` runs, battery B-01…B-24, `tokenizer.ggml.pre`
ranged reads (owner-approved online machine), everything with a GLM forward. Results +
hashes go into PROVENANCE by the owner; controller records rows afterward.
