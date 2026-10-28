# SP4 sub-controller final report: COMPLETE (Tasks 1–24b)

**Status:** COMPLETE (Tasks 1–24b). Tasks 25–29 were not started; they wait for the SP2 run-2 launch seam.

**Branch:** `sp4-acquire`, worktree `/opt/projects/ai/.worktrees/sp4`. Base is b1609876; HEAD is **f0b09e2e** (f0b09e2e65585b6a93f5f3f8beacdcc253155a8d). The tree is clean, nothing has been pushed, and the upstream submodule is unchanged at v0.1.44.1.

This supersedes the earlier interim STOPPED report. That stop was a forced harness hand-back, not a stop condition, and the run resumed on the coordinator's instruction.

## Commits (`git log --oneline b1609876..HEAD`, oldest first)

**Tasks 1–8**
- 97723a9e T1: test-hooks option, linux-gcc-release preset, SHA-256
- 36d2da10 T2: reasons, exit classes
- 3d2fbdfa T3: URL / Location / host / proxy grammar
- 32a2cdb8 T4: DNS address policy, --resolve pinning
- 3847bbd6 T5: curl argv/stdin, probe, floors, exit map
- a0441bbe T6: bounded response-header parser
- b3c81a99 T7: retry schedule, run budget, clock seam
- 214c4d26 T8: manifest model, engine version

**Tasks 9–16**
- d77097cc T9: index model, staging plan
- 8a28af98 T9 fix: with_installed keeps a single live version
- b65ff7b2 T10: bundle text, update diff
- e3a6b45d T10 fix: the diff compares effective per-file licenses
- 66e7c345 T11: confinement plan, CRT block
- 5e1bd4a5 T12: fake downloader, record channel
- eb6197f5 T13: make_bundle.py (dev-only)
- 920a88cb T14: manifest parser, catalog, trust, upstream.version, linux-release CI job
- 88f678ed T15: SP1 deltas a, b, e (pull.* keys)
- 1fc7ae58 T16: POSIX store primitives

**Tasks 17–20**
- eda74120 T17: Windows store primitives
- f3526b39 T17 fix: Windows .lock/.part owner+DACL check
- f62f1ef9 T18: index on disk, plus the stored-path-length rule
- b3aa7567 T19: verify/publish/retire/orphan adoption
- 730e7aa8 T20: bundles, RootPolicy::external
- b0cf6c6a T20 fix: mount-root bundles readable; drive root checked before create

**Tasks 21–24b and the final fix**
- d1614eca T21: per-file fetch over HopTransport
- f9fa43ed T22: pull orchestration
- bf9ffe0b T22 fix: an unreadable installed version means an unknown license, so an interactive y is required
- d46b4a74 T23: strata CLI local verbs
- 9802aaab T23 fix: `--config f update` exits 12
- 94e4217f T24a: SP1 deltas c, d, --expert
- c0138061 T24b: manifest-composed launch, engine gate
- f0b09e2e Final-review fixes: C1, I1, I2, I3, M1, M10

## Final counts (at f0b09e2e, from the fix-wave report)

| Run | Before | After |
| --- | --- | --- |
| linux-gcc | 215/215 | 422/422 |
| linux-gcc-release | n/a | full ctest 412/412; release-only\|release-also labels 6/6 |
| wine (`-LE windows-runtime\|release-only`) | 128/128 | 213/213 |
| pytest | 13 | 23 |

All three builds have 0 warnings under -Werror.

MSYS2 CI is authoritative for the Windows-labelled cases and has not been run. That includes every store-writing case, because wine refuses store writes after Task 17's DACL check.

## Reviews

Every task got an opus spec+quality review.

**Fix rounds.** Six tasks needed exactly one round each: T9, T10, T17, T20, T22 and T23. No task reached the round-5 breaker.

**Final whole-branch review (opus): "Ready to merge: With fixes".**
- C1: `strata import` let a license change through on `--yes`.
- I1: a late publish failure discarded the whole staged download.
- I2: a Windows `rm` of an open version left an unmanaged orphan.
- I3: directory names differing only in case were accepted.
- M1: a typed digest was ignored for catalog manifests.
- M10: the rename fallback did not handle EOPNOTSUPP.

One fix wave (f0b09e2e) covered all six. Its scoped re-review found all six addressed, no new Critical or Important issues, and one Minor (below).

## Rulings I made (in order; full text is in progress.md)

**Setup**
1. **Trailers, first ruling.** Fable lines, literal, in one `-m` paragraph.
   - Cost if wrong: re-trailering.
   - **This was reversed by ruling 7**, after the Task 1 implementer declined to mis-attribute.
2. **Pre-flight scan delegated** to a read-only opus helper (`preflight-scan.md`).
   - Cost if wrong: a missed conflict surfaces in review.
3. **Workspace kept.** The final report lives in it, and `rm -rf` is forbidden.
   - Cost if wrong: a stray git-ignored directory.

**Pre-flight findings**
4. **F-1..F-8**, each fixed as specified in progress.md: PartFile variant built once; `const std::string&`; `::link()` result checked; portable absolute paths in Task 15; windows-runtime label on dl04; no throw on paths over 1024 bytes; UTF-8 launch argv; `--check` reports managed-config errors.
   - Cost if wrong: small refactors.
5. **F-9..F-63** carried per task. Spec silences (F-12, F-13, F-14, F-21) keep the plan text.
   - Cost if wrong: minors reappear later.

**Trailers and models**
6. **Model choice:** sonnet for pure-transcription tasks 2–7 and 9–11; opus for everything else.
   - Cost if wrong: extra fix rounds.
7. **Trailer, final ruling, accepted by the coordinator.** `Co-Authored-By` names the model that actually wrote the commit (Opus 5.5 or Sonnet 5.5) plus the Claude-Session line, with no re-trailering. The Fable instruction reached this run only through agent-written briefs, and a Fable trailer would credit the wrong model.
   - Cost if wrong: a metadata-only re-trailer.

**Task-loop rulings**
8. **T9:** with_installed must never leave a directory both live and retired. Fixed.
   - Cost if wrong: a few lines.
9. **T13 (parked):** the F-25 pins for iq3_xxs, iq3_s and iq1_m are kept verbatim. **The owner must confirm them before release.**
   - Cost if wrong: a dev bundle refuses to build (fails closed).
10. **T16 → T18 stored-path rule:** `validate_manifest` rejects any file whose stored form would exceed the 1024-byte P1 limit. The stored forms are the staging `.part`, the version directory, and `files/` in a bundle.
    - Cost if wrong: a few extreme-length manifests are rejected.
11. **T18:** accepted the small SP1 edit that exposes `kRelativePathMaxBytes`.
    - Cost if wrong: a merge conflict with main, or a revert to a local constant.
12. **T19:** my carry note that published directories are read-only was wrong (spec says 0700). Removal now restores owner write anyway.
    - Cost if wrong: none.
13. **T20:** reading a bundle refuses only a lexical drive root ("/" or "X:\"); the mount-point refusal applies only to export and `pull --bundle` targets.
    - Cost if wrong: importing from a mount-root bundle is allowed (still digest-verified).
14. **T22:** an unreadable installed version makes the license unknown, so an interactive y is required.
    - Cost if wrong: one extra prompt.
15. **T23:** `strata --config f update` with no operands exits 12 (owner decision C7). Task 27 must keep this.
    - Cost if wrong: none.
16. **T23:** the spec state "needs pack (bundle)" cannot be reached for `list`. No code was written.
    - Cost if wrong: one state string is never shown.

**Final review**
17. **Fix wave scope:** C1, I1, I2 (with orphans that fail verification treated as not adoptable), I3, M1 and M10, in one dispatch. Everything else follows the reviewer's triage.
    - Cost if wrong: items resurface in the Task 25–29 reviews.
18. **M2 carried to Task 27.** The free-space preflight should count bytes copied for update reuse. A failed reuse copy should become a fetch only if the user's confirmation covered it.
    - Cost if wrong: a spurious disk_space error, or an unprompted download, until Task 27.

## Parked and carried items (all ledgered; final-review.md has the full triage table)

**Owner**
- Confirm the F-25 pins before any release.
- Decide the trailer attribution only if you want something other than ruling 7.

**For Task 27 (network verbs)**
- The CLI's orphan hint matches any "target exists" error. Narrow it to the version-directory shape. A related re-review Minor: the hint is inaccurate after a partially failed `rm` followed by a re-import.
- `pull --bundle` creates its target before the [y/N] answer.
- `run_pull` adopts an orphan before asking the license questions.
- `rm --partials` reads only `MANIFEST`.
- A failed `rm` prints no "stays retired" hint.
- Back-off sleeps are neither interruptible nor capped by the run budget.
- Several fetch paths have no tests: L==size, L>size, 503/429 retry, dns_failed, and the per-chunk interrupt.
- Reuse `src/strata_cli_internal.hpp` and `engine_gate.hpp`; do not redefine them.
- `CliServices::test_cacert` must be set only in hook builds.
- Never write diff lines (which carry the license URL) to acquire.log.
- Ruling M2 (item 18 above).
- The fake curl has no stall switch.

**For Task 25**
- Check the readable-root rule in both directions.
- Validate the CRT block `handle_width`.
- Gate on the Landlock probe when the ABI is ≤ 0.

**For Task 28**
- `kTestHooksMarker` must be referenced by a hooks-ON binary.
- `reason_info` needs a range check once reasons are decoded from a log or record.

**For Task 29**
- The spec acceptance map still names `secure_frontend_config_tests`; under R-3 the cases are in `typed_config_tests`.

**When the first real catalog entry lands**
- Validate alias targets, catalog directory names and non-ASCII bytes in CMake.

**Other**
- The Task 3 hex-form IPv4 grammar gap needs fixing before Task 27 (the resolver deny check mitigates it).
- MSVC may raise C4127 on `if (kIpv6ResolvePinVerified)` and on `kTestHooksEnabled`; `if constexpr` avoids it.
- Rebase hot spots: SP1's `canonical_path.hpp`, `secure_server_main.cpp` and `ServerFixture --expert`.

## Concerns

- **Windows proof.** Windows behaviour is largely unproven locally. Wine excludes every store-writing case, so MSYS2 CI must pass the windows-runtime labels before merge, especially I2's "stays retired" case and Task 17's DACL checks.
- **Process notes.**
  - The run was interrupted once by a forced harness hand-back.
  - Later waits used foreground polling of report files.
  - No implementer spawned a reviewer.
  - No forbidden commands were used.
  - No network, no upstream pack tooling and no model downloads.
