# Owner-box validation index

Everything in this repository that **cannot** be proven on the Linux dev host lives here,
in one place, with its current status. Agents prove only the local gates
(`linux-gcc`, release preset, `windows-llvm-mingw-cross` compile-only, pytest, TSan);
every claim below waits for the owner's own hardware (owner rulings 2026-10-20/21).

Session logs from the owner's box: [`2026-10-21-windows-gpu-session.md`](2026-10-21-windows-gpu-session.md)
(first Windows GPU run: engine works; the Qwen think-token and end-of-turn chat blockers fixed
and GPU-verified; upstream bumped to `v0.2.2`, Maya to `v1.1.21`).

Every engine bump also passes the [engine supply-chain contract](../security/engine-supply-chain.md)
(review before build, a ruling per scan finding, a full read of the compiled diff, the
built-binary import gate); its records live in `docs/security/engine-reviews/`.

## Windows runtime

- **What:** the `windows-runtime`-labelled ctest cases (300+ registered in the cross
  preset; excluded from Linux suites) plus the SP3/SP4 real-Windows proofs.
- **How:** native build on a Windows box per `docs/AGENT_HANDOFF.md` ("Update upstream"
  and CI-era job definitions, which remain runbooks though Actions are disabled).
- **Status:** last CI-authorized proof: green `windows-msys2-clang` at `7bddb9e1`
  (v0.2.1.3 pin). Newer `windows-runtime` cases added by SP3 M2 (two named cases with
  write-stall volume risk) and SP5 M1 (idle-unload/lifecycle, serve foreground, vision)
  have **no runtime proof yet**.

## GPU validation kits

1. **SP3 API kit** — `docs/validation/sp3-api-kit.md` + `strata-api-probe`
   (modes `stream`, `nonstream`, `cancel-after=<n>`, `cancel-during-prefill`,
   `concurrent=<k>`, `utf8`, `seed`). Status: defined, **not yet run**.
2. **SP5 parity acceptance kit** — `docs/validation/sp5-parity-kit.md` + probe corpus
   mode (delivered by SP5 M2, merged `073af122`). Runs the same corpus against plain upstream
   `serve` (v0.2.2 engine) and `strata serve`, plus the Maya `v1.1.21` side vs our
   `glm53` profile; results file is hashed into `docs/PROVENANCE.md` as the SP5
   acceptance entry (row exists as a placeholder — **no GLM parity claim until you run it**).
   Status: ready; **run is owner-side**.
3. **SP6 GLM eval kit** — `tools/glm-eval/*` per the SP6 design (decision sheet adopted
   2026-10-22, resume §5 item 1; kit planned in `docs/superpowers/plans/2026-10-22-sp6-plan1-eval-kit.md`).
   Status: plan queued after SP7 M2; **battery runs (B-01…B-24) and pack building are owner-box
   work**.
4. **SP7 offline release validation kit** — `docs/validation/sp7-offline-kit.md`:
   build the signed artifact (`scripts/release/build_release.sh`, pinned key at
   build time), air-gap carry, `unshare -rn` unpack, `SHA256SUMS` re-verify,
   `doctor`/`doctor --deep`, `serve` + local chat, `strata update` accept and
   `update --rollback` both directions, refusal proofs for unpinned-key and
   tampered-component. Results hash into the `docs/PROVENANCE.md` release-entry
   placeholder row. Status: ready (SP7 M2 delivered the tooling); **run is
   owner-side**; no offline-release/GPU/update-chain claim until the row exists.

## Parked for the owner (decisions, not runs)

- `secure_frontend_api_server_tests.log_line_format_and_rate_limit` — **FIXED 2026-10-22**
  (controller, test-side only, product untouched). Was: deterministic failure under TSan,
  intermittent under heavy load. Cause: readiness-gate `/health` polls are logged as
  `health 503` lines during a stretched boot, the pool logs `other 0 evicted` bookkeeping
  lines, and the old assertions assumed a single line shape and a burst that only
  overflows the token bucket on fast timing. Fix: field rule accepts the two real line
  shapes, wave raised to 6x80 so overflow is guaranteed at any plausible speed; every
  invariant (fixed field shape, rate bound, suppression reporting, startup summary) kept.
  RED: 3/3 deterministic under TSan before the fix. GREEN: 12/12 under TSan, full
  linux-gcc suite 826/826 (first fully green run in six batteries), release 10/10,
  cross 0 warnings, pytest 33.
- SP6 decision sheet (S1–S7, M1–M7), presented 2026-10-14 — **adopted by the controller
  2026-10-22** under the no-block directive (values in resume §5 item 1; any one reversible
  by the owner with a single line).

## How to record results

Run a kit, keep its results file, and commit a `docs/PROVENANCE.md` row: kit name, date,
host, commit sha tested, results file SHA-256, verdict. Agents then cite that row as the
authority for the claim — never the run output itself.
