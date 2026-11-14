# Calibration improvements verification

Scope: the owner-authorized follow-up after calibration finished, including the false
settings_changed result. The implementation plan and original backlog are under
`docs/superpowers/plans/2026-11-11-calibration-*`.

## Implementation and boundaries

| Requirement | Implementation |
| --- | --- |
| Prompt, TTFT and decode measurements | ServeMonitor generation snapshots; prompt throughput excludes reused tokens. |
| Representative workloads | Chat, coding, long context, JSON/SSE multi-tool and explicit custom workloads. Longer performance cases are separated from short quality probes. |
| Interactions and variation | Thorough refinement, repeated candidates, interleaved baseline/winner confirmation and conservative variation gate. |
| Quality before speed | Fixed known answers and strict tool/stream completion checks. Custom text has structural checks only; none of these establish general model accuracy. |
| Explanatory reports | Exact settings, timings, context coverage, memory, failures, skipped knobs, variance, retained-result outcome and local history. |
| Accuracy tradeoffs | Explicit report-only comparison against accuracy defaults. Both math runtimes bind comparison resume; inactive sampled-only shortcuts are skipped. INT8/Q8 defaults are unchanged. |
| Bounded/resumable work | Quick/thorough, time budget, ETA, cancel and matching checkpoints younger than one day. Invalid resume preserves its original checkpoint. |
| Interference | Two post-stop CPU/RAM snapshots avoid attributing the prior trial to another workload. A bounded system NVIDIA query reads active compute processes and actual thermal flags. Missing evidence is reported; no applications are stopped or power settings changed. |
| Context headroom | Real selected-template/tokenizer counts fill the input budget. Less than 95% coverage cannot pass the context probe. The synthetic prompt is capped at 1 MiB, so very large contexts may remain unverified. |
| Cold/warm/sustained | Separate startup, first/warm TTFT, warm decode and thorough sustained decode. These are workload measurements, not a fixed-duration thermal soak. |
| Fixed/inactive settings | Plan diagnostics omit fixed flags and inactive MTP/sampled-only settings; model-specific bounds are revalidated before a trial. |
| Local history and rollback | Eight owner-private entries; validated restore changes the next launch only. Custom prompt text and generated answers never enter summaries/checkpoints/history. |
| Freshness | Resolved option defaults, remembered measured options, metadata inventories for runtime libraries and selected models/packs/tokenizers/shards, and a digest of fixed launch settings. Older records lacking that evidence are stale but retained. |

All artifact inventories are bounded metadata checks, not model-file content attestation.
GPU readings are before/after snapshots, not continuous proof of no interference or throttling.
The unchanged legacy automatic calibration path remains compatible; manual benchmarking is explicit.

## Regression evidence

- Completed-run freshness: three failing assertions before remembering measured values; passing
  afterward, including a later start with the same context and accepted tuning.
- Policy/workload/monitor lanes captured RED for timing separation, quality and contamination,
  context coverage, stream type/completion, host freshness, external runtime and model artifacts.
  Focused GREEN results were independently rebuilt in the final integration gates.
- End-to-end synthetic fixtures cover success, quality failure preserving prior tuning, cancelling
  and resuming completed trials, rejection after option changes without erasing the checkpoint,
  restore and removal, private custom-input handling, comparison never applying its result,
  changed runtime rejection, larger bounded calibration input and unchanged other route limits.
- The full regression pass exposed seven expectations: six UI checks and the former calibration
  body limit. The new phone rule was merged into the existing media block with every original
  layout rule preserved. Request checks now assert the complete legacy request and optional
  benchmark request. The calibration-only bound is tested at limit+1; other management routes
  retain 4 KiB. These are explicit feature-contract updates, not weakened failure assertions.
- Host-wide Python execution was unsuitable for Linux toolchain symlinks and the restricted temp
  folder. WSL ran 187 checks successfully; its twelve packaging failures were Git ownership
  detection on the Windows mount. With trust scoped to this repository and pinned sources for
  that command only, all fourteen packaging/manifest checks passed. No global Git setting changed.

## Final gates

- Linux Debug: 1551 passed, 11 environment-dependent skips, zero failures (1562 registered).
- Release: 30 release gates, 46 calibration cases and 7 benchmark cases; all passing.
- Windows LLVM-MinGW cross-build and the top-level integration build: successful under Werror.
- Python: 199 passed and 11 skips across the full run and corrected packaging rerun; host UI 4/4.
- Native Windows: compiled accurate and fast-math variants and the final frontend/UI package.
  The unchanged CUDA dependency warnings retain the previously recorded vendor exception;
  native frontend compilation continues to treat warnings as errors.
- Package: 58 manifest entries verified, two engine variants and 78 runtime files compiled in
  `out/dist-calibration-improvements`, then published to **`out/dist-tool-calling`** after the
  owner closed the running app. All 78 copied runtime files matched their staged SHA-256 values;
  a fresh before/after comparison confirmed **all 377 data files unchanged**. The earlier snapshot
  with live file locks was not used. Prior runtime backup:
  `out/backup-before-calibration-20261111-154630`. The app remains closed for owner testing.

Local logs are under out with the calibration-final-, calibration-wsl-, calibration-native-
calibration-package-verification and calibration-publication prefixes. No new real-model/GPU numerical test or Windows
runtime validation is claimed. See `docs/AGENT_HANDOFF.md` for the current publication status.
