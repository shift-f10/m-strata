# SP7 — release, packaging, service verbs, full doctor

Date: 2026-10-22. Status: **approved for planning by owner directive** ("continue, do not
stop per block, keep going one after the other" — controller calls marked D-n below; the
owner can reverse any of them with one line, same as SP5's C-n rulings).
Parents: PX spec `2026-10-13-product-experience-and-upstream-tracking-design.md` §5–8 and
its SP7 row; SP4 design (model update half, trust primitives); SP5 M1 (`serve`, `version`,
`key`).

## 1. What SP7 delivers

1. `strata service enable|disable|status` — opt-in per-user auto-start (PX §6).
2. `strata uninstall` — stop server, remove service unit/task, token, config; models kept
   by default (PX §5 table).
3. `strata doctor` (full) — the PX §5 row plus the §8 CUDA/MEMLOCK checks; fully offline.
4. Program-update release side: signed release manifest schema + `strata update` client
   completing the exit-12 placeholder (PX §7), including `update --rollback`.
5. Packaging: Linux tarball + `.deb` builders, Windows installer staging, `SHA256SUMS` +
   detached signature, SBOM (PX §8).
6. `scripts/secure_distribution_check.py --release` — quarantines the upstream Python
   service, installer and update scripts at package time.
7. Offline GPU validation kit (`docs/validation/sp7-offline-kit.md`) — the owner-box
   checklist that proves an artifact bundle runs without network.

Out of scope (unchanged): network installers, drivers/DKMS, elevation, system services,
background anything, paid services. `upstream/` and `engines/maya-ref/` stay read-only.

## 2. Decisions (D-1…D-10)

- **D-1 Split into two milestones.** M1 = the three runtime verbs (`service`,
  `uninstall`, full `doctor`) — pure product code, fully host-provable. M2 = the release
  side (signature verification, `strata update`, manifest schema, packaging builders,
  SBOM, `--release` check, offline kit). M1 merges before M2 plans.
- **D-2 Signature scheme: vendored self-contained Ed25519.** Follows the
  `secure_frontend_sha256` precedent: a small dependency-free verifier
  (`secure_frontend_ed25519`, RFC 8032 verification path only), signed against the RFC's
  official test vectors in unit tests. The release-side *signer* is pure-Python RFC 8032
  reference code under `scripts/release/` (no pip installs needed). Public key pinned at
  build time (`STRATA_RELEASE_PUBKEY_HEX` compile definition, default empty = update
  refuses); `update.source` stays an untrusted URL list — trust comes from the pinned key,
  never from TLS alone.
- **D-3 Release manifest schema (signed envelope).** `strata-release-v1`: version,
  channel, per-component rows (frontend, engine, cuda-runtime), each with filename,
  size, SHA-256, and a notes URL; envelope = manifest bytes + `\n--STRATA-SIGNATURE-V1\n`
  + base64 Ed25519 sig over the manifest bytes (minisign-style framing, our own). Key
  digest also recorded in `docs/PROVENANCE.md` per release.
- **D-4 Install layout (PX §7 picked):** `<root>/<version>/` side-by-side directories with
  a `current` pointer. Linux: `current` is a symlink switched by atomic rename
  (`ln -s <new> current.tmp && mv -T`). Windows: a `current.txt` pointer file plus
  Start-menu/PATH repoint of the user-facing entry (no launcher exe). `update --rollback`
  flips the pointer to the kept previous version. The running program must not be the
  update target: `strata update` refuses if `current` is not resolvable to its own exe
  path (plain single-directory installs get a clear "not a versioned install" error).
- **D-5 Service verbs.** Linux writes exactly the hardened unit of PX §6
  (`NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome=read-only`, `ReadWritePaths`
  of our three dirs, `PrivateTmp`, `RestrictAddressFamilies`, bounded `Restart`) to
  `~/.config/systemd/user/strata.service`, then `systemctl --user enable --now`; no
  user-namespace systemd → non-zero exit explaining what to fix, never a fallback to
  system scope, never `loginctl enable-linger` (explained only). Windows: Task Scheduler
  task at logon for the current user, not elevated, with upstream's two settings —
  *(split in M1 T6 per PX §6: we take "Priority level: Normal" (`<Priority>6</Priority>`)
  and refuse "run with highest privileges" (`RunLevel LeastPrivilege`) — upstream's
  privilege half conflicts with never-elevating, and PX §6 is the owner's text and
  wins.)* Task XML is the golden text; runtime proof waits for the owner's box, logic
  compile-proven by the cross gate. `status` reads unit/task state read-only.
  `ExecStart` always points at the resolved `current` target.
- **D-6 Uninstall safety.** *(amended in M1 T4 — verified fact: no serve lockfile
  exists on this base, and we do not add a pidfile.)* Finds a running `serve` via the
  loopback health probe: `GET /health` on the configured listener; any HTTP status
  line — a 401 included — means the server answers. Server answers and our service
  unit/task is installed → stop it first through the service manager; answers with no
  unit → refuse (stop the foreground `serve` with Ctrl+C first); silent → proceed.
  Removes: service unit/task, token file, config file, state dir. Keeps by default:
  models, logs (printed where they are). `--purge` removes models only after a typed
  second confirmation (`--yes` cannot satisfy it). Never touches any path outside the
  allowed roots; refuses if `current`/program dir is not where install metadata says it
  is (the metadata itself is M2's; the gate is in place and passes honestly in M1).
- **D-7 `doctor` full check set (offline only).** GPU/driver/CUDA runtime: probe
  `nvidia-smi` presence + driver version parse vs minimum (values from the engine
  profile — *(M1 T3 note: the shipped manifest/profile carries no such fields, so M1
  compiles the expectations in per profile: driver minimum "580" per upstream
  `INSTALL.md`, unlimited memlock, CUDA runtime `libcudart.so.12/13`)*), CUDA runtime
  libs presence next to the program; config validity (reuses the
  SP1 typed loader, reporting every rejected key by name); token file permissions;
  data/state/runtime dir permissions and canonicality (CFG rules); listen port free;
  model digests on `--deep` only (reuses `verify`); `LimitMEMLOCK` current value vs the
  engine profile's need. Exit 0 all pass; else non-zero and every failure prints the
  human fix string, installing nothing (PX §8).
- **D-8 Packaging builders (`scripts/release/`).** `build_release.sh` (Linux): release
  preset build → bundle dir `strata-<ver>-linux-x86_64/` with `bin/strata`,
  `bin/strata-secure-server`, engine build outputs, `VERSION`, `SHA256SUMS`,
  `manifest.json` (+ signature when `STRATA_SIGNING_KEY` names a key file; unsigned
  otherwise, and the check script then marks it `unsigned: not releasable`), `.deb` via
  `dpkg-deb` when present (the admin path; tarball remains the user path). CUDA runtime
  libraries are copied only when present (agent boxes may lack them; the offline kit
  proves the full bundle on the owner's box). Windows: staging script produces the
  installer input tree + `installer.nsi` (NSIS), and an NSIS toolchain under
  `.toolchains/` is attempted; if unavailable locally, content-staging is verified by
  tests and the actual `makensis` run is an owner-box step recorded in the kit.
- **D-9 SBOM and release evidence (amended at M2 planning: the evidence machinery already
  exists and wins).** `scripts/secure_distribution_check.py` already requires
  `secure-release-manifest.json` (profile `secure-native`, `network_default offline`,
  privacy flags all-false, non-empty components) and `sbom.spdx.json` (**SPDX**, not
  CycloneDX — correcting the first draft), and already quarantines `upstream/`,
  `maya-ref`, the legacy Python service/installer/update scripts, and any stray `.py`.
  M2 keeps that schema exactly, generates both files from the two submodule pins +
  vendored inventory (`scripts/release/sbom.py`, no new runtime deps), and adds on top:
  `SHA256SUMS` completeness, and verification of the signed D-3 release envelope against
  the pinned key (unsigned artifacts are marked `unsigned: not releasable`).
- **D-10 Config surface.** One new typed key, `update.source` (HTTPS URL list, already
  reserved in PX §4), plus parity-table disposition rows for it and for `STRATA_*`
  touchpoints the doctor reads; both tables grow their meta-test automatically.

## 3. Proof model (unchanged rules)

Host gates per task (linux-gcc ctest, release build, cross build, pytest) synchronous in
lane; `-Werror`; TDD RED first; systemd verb tests assert the exact unit text and command
composition with a fake systemctl (PATH shim) — no reliance on a live user manager;
Windows service logic compile-proof only until the owner's hardware; `doctor` and
`uninstall` fully host-testable with fake engine + temp roots. Signature tests use RFC
8032 vectors plus forged-envelope/tamper negatives. Kit runs (GPU/offline) remain the
owner's, hashed into `docs/PROVENANCE.md`.
