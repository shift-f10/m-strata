# Sub-controller brief — SP1: typed config, paths, tokens

Read `subcontroller-common.md` (same directory as this file) first; it binds.

- **Plan:** `docs/superpowers/plans/2026-10-13-sp1-typed-config-paths-tokens.md` (24 tasks)
- **Spec:** `docs/superpowers/specs/2026-10-13-sp1-typed-config-paths-tokens-design.md`
- **Worktree:** `/opt/projects/ai/.worktrees/sp1`, branch `sp1-typed-config-paths-tokens`
- **HEAD at launch:** `17d2452d` (= `main` after the layout merge and the shared pre-task). Verify before anything else.
- **Final report path:** `/opt/projects/ai/.worktrees/sp1/.superpowers/sdd/2026-10-13-sp1-typed-config-paths-tokens/FINAL-REPORT.md`

## Task 2 is DONE — skip it
The plan says Task 2 (test registration) runs first on `main`. It has already
been executed, reviewed (Approved) and merged; its commit is in your HEAD.
Write `Task 2: complete (shared pre-task, merged to main)` into your ledger
before dispatching anything, and never re-dispatch it. The plan's rebase step
for Task 2 is also done. Run **Task 1, then Tasks 3–24 in number order**.
Task 1 registers its five `net0*` cases with Task 2's helper.

Parked findings from Task 2's review that SP1 tasks should pick up where the
code is touched anyway (carry these into the relevant dispatches):
- `make_private_directory` accepts an existing directory without checking its
  mode/DACL — Task 9 (root checks) territory.
- `make_scratch_directory` falls back to `/tmp` when `SECURE_FRONTEND_TEST_TMP`
  is unset (manual runs only).

## Plan-specific reminders
- Global Constraints of the plan apply verbatim (strict JSON config, `token.source`
  `file:`/`env:` only, `localhost` → `127.0.0.1` without resolver, tokens < 32
  bytes rejected, `limits.chat.max_body_bytes` default 49152, `--check` has no
  side effects, SP1 touches only `secure_frontend/`, `docs/`, `scripts/dev/`).
- The `strata` CLI binary is NOT in SP1; SP1 exposes `--config`, `--check`,
  `init --config FILE [--force]` on `strata-secure-server`.
- The Windows environment-scrub deviation (Task 20) is owner-ruled; Task 24
  records it in the handoff.
- Expected counts grow per task; the plan states them. When a count differs,
  reconcile and ledger it rather than forcing it.
