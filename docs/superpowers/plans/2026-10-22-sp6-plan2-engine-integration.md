# SP6 Plan 2 — GLM engine integration: submodule path, wrapper project, guards, audit

Spec: `docs/superpowers/specs/2026-10-14-sp6-glm-engine-design.md` §3 (all subsections
binding), §9 decisions as adopted (resume §5 item 1). Dispatch queued **after SP6 Plan 1**
(Plan 1 delivers `fetch-sources.sh` bundles the wrapper's step-3 GGML pin consumes, and
proves the flavor/launch seams). Branch `sp6-plan2-integration`, worktree `.worktrees/sp6p2`.
Standard per-task gates; TDD RED first; `-Werror`; no pushes.

## Controller decision (marked for one-line owner reversal)

- **C-SP6-A: one MAYA gitlink, at the spec's path.** SP5 D-10 already vendors the same
  repo at the same pin as `engines/maya-ref` (v1.1.8 @ `5c970a86`). Plan 2 **renames** that
  submodule to `engines/glm/maya` (`git mv` + `.gitmodules` + `git submodule sync`) instead
  of adding a second gitlink to the same commit. All path consumers update in the same
  commit: `docs/PROVENANCE.md` ("Project Maya" → "GLM engine baseline" per §3.2), the
  distribution check's `maya-ref` forbidden-part (keep BOTH parts forbidden for safety:
  `maya-ref` stays banned as a stale-path tripwire, `glm` under `engines/` is the engine
  path and likewise forbidden in packages), SP5 parity-table generator tests that grep the
  vendored tree, `docs/AGENT_HANDOFF.md`, validation README.

## Tasks

1. **Submodule re-path** (C-SP6-A) — atomic commit; every preset must still configure with
   zero references into `engines/glm/maya` (the engine is NEVER built by host presets;
   only `cmake/glm-engine` ever `add_subdirectory`s it, and only with the root option ON,
   which stays OFF everywhere on this host). Distribution check PASS both before/after.
2. **`cmake/glm-engine/` wrapper** (§3.3 exactly): standalone project; guards 1–6 with the
   fixed configure-failure messages verbatim; cache seeding (step 7); post-build stamp
   check + `strata-glm-engine`/`expert-profile-glm.bin` copies into `bin/` (step 8, never
   touching the submodule). Host-provable: hermetic configure tests
   `secure_frontend_glm_configure_tests.*` (new CTest suite, TIMEOUT-registered) covering
   empty-submodule message, `SECURE_STRATA_GLM_ASSUME_NO_CUDA=ON` message, bad
   `SECURE_STRATA_GLM_GGML_DIR` (wrong sha or missing), arch-list validation, git-present
   requirement. Real CUDA configure (owner box) NOT claimed here.
3. **Root option + ExternalProject** (§3.3): `SECURE_STRATA_BUILD_GLM_ENGINE` default OFF;
   root-side step-1/2 pre-checks; Windows-non-MSVC configure refusal; OFF stays the
   default in every preset — verify presets untouched by diff.
4. **`kGlmEngineVersion`** (§3.3 last bullet): root configure-time `git describe
   --tags --exact-match` for `engines/glm/maya`; not-exact-tag fails unless
   `SECURE_STRATA_GLM_ENGINE_VERSION` set; absent submodule → version absent → existing
   SP4 `engine_profile_unavailable` (exit 11) path for `glm53` manifests (test with the
   existing manifest machinery, no submodule present).
5. **Bump runbook + audit tool** (§3.4): PROVENANCE section "Updating the GLM engine"
   (old-pin gates first, `out/<name>-<L>` dirs, one commit);
   `tools/secure/glm_engine_audit.py` (reads engine source text, executes nothing;
   dev-host full coverage per §2.7) + its committed snapshot file + a pytest case
   asserting the snapshot matches the current pin's audit output (the drift gate).
6. **Notices** (§3.5): `docs/licenses/glm-engine.md`; DFlash2 drafter weights sentence
   preserved (CC BY-NC-ND never bundled/fetched); `data/expert-profile-glm.bin` recorded
   with SHA-256 at the tag in PROVENANCE.
7. **Truth pass:** AGENT_HANDOFF rows; validation README unchanged (still owner-box runs);
   spec context lines amended where merged reality differs (the maya-ref rename is folded
   back into §3.1/§3.2 text with a dated note).

## Waits for the owner's box

`glm-linux-cuda` / `glm-windows-msvc-cuda` real configures and builds (nvcc ≥12.8;
VS 2022 v143 on Windows), the §2.6 battery verdict formally recorded, engine binary
signing/packaging with SP7 M2's release tooling.
