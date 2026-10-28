# SP5-P2 plan — parity refresh at upstream v0.2.2 / Maya v1.1.21 (issue 27)

Spec basis: SP5 spec D-7 (full-surface parity, security exclusions as tested rejects) and
the parity-table rules; owner session log `docs/validation/2026-10-21-windows-gpu-session.md`
issue 27 ("regeneration is an open item"). Pins: `engines/maya-ref` @ `451f9cb6` (v1.1.21),
`upstream` @ `011c9e60` (v0.2.2). Branch `sp5-p2-parity-refresh`, worktree `.worktrees/sp5p2`.
Standard lane rules: per-task synchronous gates, TDD RED first, `-Werror`, no pushes,
read-only submodules, ledger at `.superpowers/sdd/2026-10-22-sp5-p2-parity-refresh/progress.md`.

Verified recon (controller, 2026-10-22): maya_118 wire CORE survives v1.1.21 — `PP`
5-field with optional tag prefix (`PP %s%zu …` :1757), `T <tok>` (:1956) AND a slot mode
`T <rid> <tok>` (:2295), `STAT key=value` extended keys (:2014), `DONE` now carries
~9 fields (:2111). `STRATA_GLM_*` env names still honored (:1086, :1270); the docs
rename everything `MAYA_*` (SETTINGS.md, 144 mentions) and the engine gained many `DS4_*`
knobs (~554 getenv sites total). Table header in `parity_keys_maya.hpp` already records
the freeze-at-v1.1.8 state.

Baselines at dispatch: debug ctest 955 runs / 4 allowed env skips / 100%; release full
913/913; cross 0 warnings; pytest 65 passed 1 skipped.

## Tasks

1. **Meta-test hardening (drift tripwire) — RED first.** The parity meta-test must
   enumerate the CURRENT vendored trees and require a disposition for EVERY knob found
   (argv options, `getenv` names, SETTINGS.md keys) for both tables; today a new v1.1.21
   knob passes unnoticed. Expect RED = the test fails listing hundreds of undispositioned
   v1.1.21 keys. Keep the citation requirement (file:line at the current pin).
2. **Maya inventory regeneration.** New `parity_keys_maya.hpp` content at v1.1.21: every
   config/env knob gets a disposition per D-7 (typed row / security reject with a tested
   rejection / mapped-away with citation / out-of-scope with reason). MAYA_* and DS4_*
   naming handled at the engine's names; STRATA_GLM_* rows kept where the engine still
   reads them. Header rewritten to "generated at 451f9cb6 (v1.1.21)" — the freeze note
   goes away when Task 1's enumeration passes.
3. **Upstream table refresh at v0.2.2.** Same regeneration pass for `parity_keys_upstream.hpp`
   at `011c9e60` (v0.2.2 knobs vs v0.2.1.3 citations); update citations; dispositions
   re-reviewed (the owner's v0.2.2 privacy re-review commit 324191af is the reference).
4. **Typed GLM env allowlist + glm.* config refresh.** The closed child-env allowlist
   regenerated from the new table (bounded set, forced-safe defaults preserved: slots off
   per sheet S5, watchdog 0 per M5, headroom 16 per M2). New typed config rows only for
   knobs the owner's parity intent demands (D-7: typed or rejected, never pass-through).
5. **maya_118 dialect refresh.** `DONE` extended fields consumed fail-closed per the
   v1.1.21 shape; `T <rid> <tok>` slot form: REJECTED (slots stay env-forced off — a slot
   line arriving under our launch is a protocol error, tested); PP tag-prefix consumed.
   Fake-engine Maya mode updated to the v1.1.21 cadence; GLM e2e (stream/non-stream/
   cancel/vision rows) re-green.
6. **Launch composition + vision binary.** glm53 manifest-composed launch updated for
   v1.1.21 argv (renamed/added flags; retired ones dropped); vision role now targets the
   `maya-vision` binary name (issue 27) — manifest role, VisionSupervisor wiring, tests.
7. **Penalty semantics (issue 27).** Our sampling-penalty mapping vs v1.1.21's engine
   semantics (`src` greps for penalty/presence/frequency): document + test the mapping or
   reject the knob with a named-row rationale.
8. **Kit + docs truth pass.** SP5 kit docs (v1.1.8 references → v1.1.21, corpus unaffected
   unless dialect rows changed), PROVENANCE refresh rows (both pins with dates), AGENT_
   HANDOFF, validation README statuses.

## Non-goals

No engine building (Plan 2 of SP6 remains queued); no GPU runs; the SP5 GPU kit stays
owner-side; DS4_* knobs that belong to the DeepSeek path get out-of-scope/reject rows
with citations, not typed config.
