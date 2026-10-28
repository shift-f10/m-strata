# SP7 M2 plan — signed release side: Ed25519, `strata update`, packaging, SBOM

Spec: `docs/superpowers/specs/2026-10-22-sp7-release-packaging-service-design.md`
(D-2, D-3, D-4, D-8, D-9 amended, D-10). Dispatch only after SP7 M1 merges. Branch
`sp7-m2-release-side`, worktree `.worktrees/sp7m2`. Standard per-task gates
(linux-gcc build+ctest, release build+ctest, cross build, pytest) synchronous in-turn;
TDD RED first; `-Werror`; no pushes; `upstream/` + `engines/maya-ref/` read-only.

Verified base facts (do not re-litigate): `strata update` is the exit-12 placeholder in
`strata_cli.cpp:37` with the guard at `run_network_verb`; network fetch MUST reuse the
SP4 confined downloader stack (`downloader.hpp`, `ConfinedDownloader`, probe-then-fetch,
cwd rule B1 in `strata_cli_network.cpp`); `secure_distribution_check.py --release` already
exists and requires `secure-release-manifest.json` + `sbom.spdx.json` (SPDX) and already
quarantines upstream/maya-ref/legacy Python — M2 makes those real, it does not redesign
them; the sha256 lib (`secure_frontend_sha256`) is the self-contained-crypto precedent.

## New seams (binding)

- Lib `secure_frontend_ed25519` (`src/ed25519_verify.{hpp,cpp}`): dependency-free RFC 8032
  **verify-only** path — `bool ed25519_verify(pubkey32, msg, sig64)`; constant-time-ish
  ref-style implementation; unit tests use the RFC 8032 official test vectors (section
  file content inlined in the test) plus forged-signature/tampered-message/pubkey-swap
  negatives. No signing in C++.
- Lib `secure_frontend_release_manifest` (`src/release_manifest.{hpp,cpp}`): parse +
  verify the D-3 envelope (`strata-release-v1` JSON + `--STRATA-SIGNATURE-V1` framing,
  base64 sig over manifest bytes); schema: version, channel, components[]
  {name(frontend|engine|cuda-runtime), file, size, sha256, notes_url}; pinned key from
  compile definition `STRATA_RELEASE_PUBKEY_HEX` (empty ⇒ every update refuses with
  `no release key pinned`; tests cover both).
- Config: typed `update.source` (HTTPS URL list; already reserved PX §4) + parity-table
  rows (D-10) so the meta-test absorbs it.
- Update install layout per D-4: `<root>/<version>/` + `current`; Linux atomic symlink
  rename; Windows `current.txt` + user-PATH/Start-menu repoint (compile-proved, runtime
  owner-side); `update --rollback` flips the pointer; refuse when the running exe is not
  under a resolvable `current` (single-dir install message).

## Tasks

1. **Ed25519 verifier.** RED: RFC 8032 vectors + negatives against an unimplemented stub.
   GREEN: implement. (Test vectors: SHA-512 comes from the existing sha256? NO — RFC 8032
   needs SHA-512: implement a self-contained SHA-512 inside the same lib, same
   no-dependency rule, with its own FIPS vector tests.)
2. **Release manifest envelope.** RED: verify good envelope; tamper byte ⇒ reject; wrong
   pinned key ⇒ reject; expired-looking field not needed; no pinned key ⇒ refuse with the
   exact line; malformed base64/JSON ⇒ reject. GREEN: implement; wire
   `STRATA_RELEASE_PUBKEY_HEX` through CMake (`secure_frontend_release_manifest` lib +
   cache var default empty).
3. **`strata update` (program half).** RED→GREEN: flow = fetch manifest+signature from
   each `update.source` via the confined downloader (never the system curl) → verify
   envelope → show current-vs-offered (version, components, sizes, digests — golden-ish
   stdout test) → confirm (`--yes` accepted) → download components into staging under the
   program root, verify every SHA-256 → install `<root>/<new-version>/` → atomic `current`
   flip → keep previous → print restart hint. Failure at any step: non-zero, previous
   `current` untouched. `update --rollback` test. Exit-12 placeholder removed from usage;
   the models half (`update --models`) unchanged.
4. **Key material + signer (release tooling, Python).** `scripts/release/keygen.py`
   (deterministic Ed25519 keypair, prints the pinned hex), `scripts/release/sign.py`
   (pure-Python RFC 8032 reference signer — self-test asserts it validates with the C++
   verifier via a cross-check test fixture committed under `tests/fixtures/`).
5. **Release manifest + SBOM generators.** `scripts/release/make_manifest.py` and
   `sbom.py` produce REAL `secure-release-manifest.json` (schema unchanged — the check
   script's existing assertions stay the contract) and `sbom.spdx.json` listing both
   submodule pins (`upstream` v0.2.1.3 @ `cbfd5b92…`, `engines/maya-ref` v1.1.8 @
   `5c970a86…`) with licenses, plus the frontend build info. pytest suite for both
   generators + a check-script e2e (build stage dir → generators → `--release` PASS).
6. **`build_release.sh` + `SHA256SUMS`.** Linux: release-preset build → bundle tree
   (binaries + VERSION + evidence files + CUDA libs copied only when present) → tarball →
   `.deb` via dpkg-deb when present; `SHA256SUMS` generated; signature attached only when
   `STRATA_SIGNING_KEY` names a key file, else check marks `unsigned: not releasable`.
   Windows: staging script + `installer.nsi`; NSIS under `.toolchains/` attempted,
   recorded either way; runtime install proof stays owner-side.
7. **Offline GPU validation kit.** `docs/validation/sp7-offline-kit.md`: the owner-box
   checklist (unpack artifact with network namespace off, `doctor`, `serve`, digest
   re-verify, `update --rollback` both directions); `docs/validation/README.md` gains the
   kit row; `docs/PROVENANCE.md` gains the placeholder release-entry row (key digest,
   artifact SHA, results hash — all blank until the owner runs it).
8. **Truth pass.** AGENT_HANDOFF rows for all of the above; PROVENANCE notes the vendored
   crypto decision (D-2); resume doc update by the controller at merge.

## Explicit non-goals

No signing key exists in-repo (owner generates/holds it); no real Windows installer run
until NSIS availability or the owner's box; no auto-update anywhere (PX §7 invariant
tests: nothing in startup paths, server paths, or model paths touches `update.source`).
