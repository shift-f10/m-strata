# SP5 GLM parity acceptance kit — GPU-box proof (lane spec §7, D-1)

This procedure runs **on the owner's GPU box**. It proves the SP5 M2 claim the
lane can never prove by itself: that `strata serve` on a `glm53` manifest and
**MAYA v1.1.21's own server** (same engine build, same composed flags) answer
the same fixed corpus the same way — text-for-text on greedy/seeded cases,
finish/usage on schema cases, and recorded shapes where an approved decision
authorizes a difference (C-6). GitHub Actions are disabled and the dev host
has no GPU: **nothing here runs on the dev host** except building the probe
and the `--dry-run` render. The GPU run's results are hashed into the
PROVENANCE SP5 entry, like the SP3 kit's.

Parity is a **config/launch-surface claim** (C-8): no new routes, and the
request fields stay exactly SP3's approved schema. The kit drives `strata
serve` only (C-7); the `run`↔`server.json` product loop is not SP5.

## 0. Declared parity gaps (not silently dropped)

The launch composition (Task 7) does not render these Maya server flags, so
the run must **omit them on both sides** for the flags to stay identical:
`--expert-profile`, `--expert-profile-save`, `--expert-profile-save-every`,
`--kv-resident`, `--spec`, `--spec-draft`, `--spec-draft-glue` — no typed
value source or manifest role exists for them yet (parity rows cite this;
FINAL-REPORT lists it). `--parallel` is bounded 1..8 by the composition.
Vision is typed and plumbed (SP5 M1) but its real encode behaviour is
kit evidence: run the corpus vision case only with the vision flow up on both
sides, or comment that corpus line out for a text-only run.

## 1. Setup (owner box)

1. **Reference engine + server:** build the upstream Strata reference at the
   pinned `v0.2.2` (the `upstream/` gitlink; sha in `docs/PROVENANCE.md`)
   and use its `serve` verb as the reference server's baseline plumbing.
2. **Maya engine:** build MAYA v1.1.21 from the vendored
   `engines/maya-ref/` submodule (gitlink `451f9cb6…`, tag `v1.1.21`) exactly
   as the SP6-designed recipe says: `docs/superpowers/specs/2026-10-14-sp6-glm-engine-design.md`
   §5 (manifest/pack layout §5.2, bundle path §5.4) plus MAYA's own
   `docs/SETUP.md`/`docs/SETTINGS.md` build notes. This submodule is a
   read-only reference in our build system: on the owner box it is built
   **outside** our presets, by hand.
3. **One model, both sides:** one Maya quant **pack**, built per §5.4
   (`make_bundle.py --profile glm53` / MAYA's `tools/iq_pack.py` recipe), is
   the single model of the run. Record its SHA-256.
4. **Probe:** build it **from the release commit** under test:
   `cmake --build --preset linux-gcc --target strata-api-probe`
   (test-only, never installed; it refuses any non-loopback address).

## 2. Render the exact commands

`--dry-run` prints the two corpus runs and the compare, so both command
lines are recorded verbatim before anything GPU-side starts (C-6 requires
the composed flags to match):

```
strata-api-probe --dry-run \
  --corpus docs/validation/sp5-parity-corpus.txt --token-file T --model M \
  --ref-port 8081 --serve-port 8080 \
  --ref-results ref-results.csv --serve-results ours-results.csv \
  --command-line '<the exact MAYA v1.1.21 serve command line>' \
  --command-line '<the exact strata serve command line + config>' \
  --engine-version 'strata v0.2.2 (reference)' \
  --engine-version 'MAYA v1.1.21 451f9cb6 <engine build id>'
```

Write the Maya version from the tag and commit as above: `VERSION` is empty
at tag `v1.1.21`, so its server reports no version of its own.

Start the reference side with the flag set the composition produces for the
`glm53` manifest (`--context` from `glm.max_context`, `--parallel` from
`glm.parallel`, the layer split, the same quant pack, no §0 flags), and
start ours with `strata serve` on that manifest. Both listen on loopback.

## 3. Corpus runs

Copy the **same** `sp5-parity-corpus.txt` to both runs. Each case class
(C-6) decides how its result is used:

- `text` — greedy/seeded: the probe runs the request twice (a determinism
  pair) and records `bytes=` and `sha256=` of the text; the compare step
  text-for-text diffs the hashes between the two sides.
- `shape` — the case declares `expect` (`status`/`finish`/`usage`) on the
  shared schema; both sides must satisfy it and agree on `finish`.
- `divergent-by-decision` — the case names `authorised_by` (B1, B2, EXT-01,
  …); both sides' shapes are RECORDED, not compared.

Per side:

```
strata-api-probe --port <P> --token-file T --model M \
  --corpus sp5-parity-corpus.txt --results <side>-results.csv \
  --command-line '<ref cmd>' --command-line '<ours cmd>' \
  --engine-version '<ref version>' --engine-version '<ours version>'
```

Exit 0 means no FAIL line; a FAIL anywhere is a failed kit, not a retry.

## 4. Compare and record

```
strata-api-probe --compare ref-results.csv ours-results.csv
```

Every line must read `PASS` (`sha256_equal` / `finish_equal` /
`recorded_both`); a missing or short results file is a `FAIL` line with exit
1 — never a skip. Then record in the **PROVENANCE SP5 entry**: the release
commit, both servers' full command lines, both engine versions, the model
pack SHA-256, and `sha256sum` hashes of both results files and of the
corpus file. That completes the GLM parity claim, exactly as the SP3 kit
entry completes the API claim.
