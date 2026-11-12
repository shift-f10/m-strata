# Calibration improvements implementation

Owner authorization: implement the documented backlog once the active calibration finishes.
Completion was confirmed by the owner and status endpoint on 2026-11-11. Preserve the saved
result and existing card settings. No real-model benchmark or service restart is needed for
development; use synthetic fixtures and compile a runtime package under out.

## Compatibility and boundaries

- Keep the existing calibration API and automatic calibration path compatible. New manual
  controls use an optional `benchmark` object. Existing requests without it retain legacy behavior.
- Ordinary calibration holds accuracy tradeoffs and INT8/Q8 fixed. Explicit comparison runs
  report alternative speed settings; never silently change the card or apply their winner.
- Persist bounded local summaries/checkpoints/history, never real conversations or generated
  answers. Custom prompts are explicit input and are not persisted; their digest binds resume.
- Reject stale resume/restore identities. A cancelled, timed-out, contaminated or quality-failed
  run must not replace a valid calibration. Report unsupported measurements as unavailable.
- Quality checks are deterministic workload acceptance checks, not a general correctness proof.
- No upstream edits, new dependencies, commits, pushes, branch changes or real GPU claims.

## Work areas

1. Experiment policy: request validation, quick/thorough plans, combination refinement,
   repeat/variation policy, budgets and resume-aware sweep. New calibration_experiment files.
2. Workloads and readings: fixed chat/coding/long/tool cases, quality acceptance, bounded SSE
   validation, per-generation metrics and host snapshots. New calibration_workload files and
   existing ServeMonitor accessors. No launcher edits in this area.
3. Frontend: explicit controls, progress/ETA, results, history/restore, comparison warnings.
4. Controller integration: routes, launcher, trial measurements, checkpoints/history, library
   identity, skipped settings, memory checks, diagnostics, documentation and final build gates.

The controller owns cross-area integration and reviews each change. Each area records RED/GREEN
evidence. Final gates: full serial Linux suite, Release gates and relevant cases, cross-build,
host UI checks and native package build. Preserve all existing data in publication.

## Completion

Implemented and verified on 2026-11-11. The verification record is
`docs/validation/2026-11-11-calibration-improvements.md`; handoff includes exact gate counts,
compatibility caveats and package/publication status. Changes remain local and uncommitted.
