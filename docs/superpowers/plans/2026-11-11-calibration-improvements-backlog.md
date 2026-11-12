# Calibration improvements backlog

Status: implemented, tested and compiled on 2026-11-11 after the owner confirmed calibration finished.
The implementation and verification record is in `2026-11-11-calibration-implementation.md`
and `docs/AGENT_HANDOFF.md`. The original requested scope is retained below.

## Requested follow-up

1. Measure prompt processing and time to first token separately from decode tokens/second.
2. Offer representative short-chat, long-context, coding and tool-calling workloads, rather
   than relying only on the current three short prompts.
3. Revisit combinations around the winning settings, particularly CPU threads and CPU/GPU
   work sharing; a single sequential sweep can miss interactions.
4. Repeat promising candidates, report measurement variation, and check output quality
   against an accuracy baseline. Speed alone must not qualify a configuration as better.
   Define meaningful quality checks; matching one greedy output is not proof of equivalence.
5. Explain results: exact changes, before/after speeds, memory use, failed trials and restore.
   Invalidate results when relevant engine libraries or other material inputs change.
6. Provide an explicitly enabled comparison mode for fast math and other accuracy tradeoffs.
   Ordinary calibration keeps those at the user's selected values. Retain INT8/Q8 defaults.

Suggested first milestone: representative workloads and separate prompt/decode measurements.

## Additional requested follow-up

The owner also approved adding the following items to the deferred implementation backlog.

- Quick and thorough modes with a time budget, estimated remaining work, cancellation and
  resumable completed trials. Resume only if the hardware/software/settings identity matches.
- Detect competing CPU/GPU activity, thermal throttling and memory pressure. Flag contaminated
  trials and offer a retry; never silently stop other applications or change power settings.
- Evaluate memory headroom at the selected context length. A fast short trial should not hide
  out-of-memory failures or severe paging during longer conversations.
- Report cold-load, warm-cache and sustained performance separately. Keep startup costs visible
  without letting them distort steady-state throughput comparisons.
- Explain settings that are fixed by a card, unsupported, inactive or irrelevant to its model,
  and avoid spending trials on candidates that do not change the effective engine settings.
- Preserve named prior results for comparison and rollback. Save benchmark summaries locally;
  never collect real conversation text automatically. Custom workloads require explicit opt-in.
- Exercise tool-call structure and completion, including multiple calls and streamed responses,
  in quality checks. A faster result that breaks tool use should fail that workload's acceptance.

The original run was allowed to finish before implementation began.
