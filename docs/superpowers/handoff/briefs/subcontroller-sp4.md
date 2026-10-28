# Sub-controller brief — SP4: explicit, verified pull / import / update (Tasks 1–24b)

Read `subcontroller-common.md` (same directory) first; it binds. Then this file.

- **Plan:** `docs/superpowers/plans/2026-10-14-sp4-acquire.md` (30 tasks: 1–24b runnable now; 25–29 AFTER SP2 run-2 — do NOT start them)
- **Spec:** `docs/superpowers/specs/2026-10-14-sp4-acquire-design.md` (APPROVED; decisions C1–C7 inline)
- **Owner decisions file:** `/tmp/claude-1000/-opt-projects/ee5cedea-4504-4508-8cef-d9fc7bb1a693/scratchpad/owner-decisions-2026-10-14.md` section C
- **Worktree:** `/opt/projects/ai/.worktrees/sp4`, branch `sp4-acquire`, created from `main` — **HEAD at launch `b1609876`** (SP1 merged; upstream v0.1.44.1; SP3 plans on main). Verify `git submodule status` shows ` 296b59d2… upstream (v0.1.44.1)`.
- **Baseline on this tree (controller-verified):** linux-gcc 215/215, wine 128/128 (65 windows-runtime excluded, verified on real Windows by CI run 38540719307: MSYS2 193/193), pytest 13, 0 warnings.
- **Final report path:** `/opt/projects/ai/.worktrees/sp4/.superpowers/sdd/2026-10-14-sp4-acquire/FINAL-REPORT.md`

## Scope for this run — STOP after it
Tasks **1 → 24b in plan order** (the plan's dependency classes are current: SP1 is on main). Do NOT start 25–29 (they need the SP2 run-2 launch seam; SP2 run-2 is executing concurrently in `.worktrees/sp2` and will produce the names the plan's Tasks 25/26 grep for). After 24b: final whole-branch review (one fix dispatch + scoped re-review), final report with status `COMPLETE (Tasks 1–24b)`, stop. The controller merges and later launches Tasks 25–29.

## Plan-specific reminders
- The plan was pre-flight reviewed twice; its "Controller notes" (1–20) and the rulings in `/opt/projects/ai/.worktrees/plan-sp4/.superpowers/reports/sp4-plan-review-1.md` (R-1…R-7; the worktree may be gone — the same rulings are reflected in the plan text) are binding. Key ones: committed `upstream.version` is the engine-version source (configure fails only on MISMATCH with a reachable tag, guarded on `upstream/.git`); `cfg01_*` cases in `secure_frontend_typed_config_tests`; schema count written relatively; `linux-release` CI job in Task 14 (hooks OFF, `-L "release-only|release-also"`, `ubuntu-24.04`); catalog ships EMPTY under D5(D) — developer bundles via `make_bundle.py` + `--trust-manifest`; the `strata` binary is created by SP4 with its verbs only; `--config <file>` required on every verb.
- NEVER run upstream's pack tooling (`iq_pack.py`, `strata_tokenizer.py`, `gguf-py`) or download model files on this host; tests use the fake pack tool and the fake downloader. No real network in tests; the optional real-curl check is documentation only.
- Test hooks: `SECURE_FRONTEND_TEST_HOOKS` ON in the test presets, OFF in `linux-gcc-release`; record channel on fd 4 exists only in hook builds; positive controls required.
- SHA-256 is implemented in-tree with FIPS 180-4 vectors verbatim; the 2^30-byte vector may be slow — keep it as the plan labels it.
- Landlock: dev host kernel 5.15 (ABI 1) — confinement proofs run here; tests SKIP with a fixed reason only when the kernel has no Landlock at all.
- Commits: one per task, literal Fable trailer lines in every commit command (the plan has them; never a shell variable), `-c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com`.
- Handoff: the plan's docs tasks update `docs/AGENT_HANDOFF.md`, `docs/security/threat-model.md`, `docs/PROVENANCE.md` (pin site `upstream.version`), `docs/secure-frontend-ci.md`; keep hunks compact (SP2 run-2 edits the handoff concurrently on its branch).
- Expected counts grow per task; reconcile and ledger differences rather than forcing.
