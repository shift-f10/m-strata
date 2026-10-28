# Engine supply-chain contract

Owner decision, 2026-10-21: whenever we pull, update or compile an engine, we make
sure it carries no deliberate or unintentional malicious code. This page is that
contract. Every rule is a gate: a bump that has not passed all of them is not
accepted, is not committed as the new pin, and nothing from it is shipped.

**What it can and cannot do.** No review proves the absence of malicious code. The
contract makes every pulled change reviewable, catches the usual signs mechanically,
puts a recorded human decision on each one, reads the compiled change in full, checks
what the built binary can reach, and keeps the engine contained at run time as defence
in depth. A determined, well-hidden change inside kernel arithmetic could still pass;
the rules below are written to make that hard and to leave a record of who looked.

## Scope

| Source | How it reaches the box | Gated surfaces |
| --- | --- | --- |
| `upstream/` (Niko1221/Strata) | git submodule, pinned gitlink | compiled + build (we build the engine); Python and scripts (our runbook runs upstream's own test suites) |
| llama.cpp / ggml | CMake `FetchContent` at the commit `upstream/CMakeLists.txt` pins | compiled + build |
| `engines/maya-ref/` (mw00/project-maya) | git submodule, read-only reference | none today: never compiled, never executed (`pytest.ini` excludes it). **Before SP6 compiles or runs any of it, the full contract applies to it.** |
| Python packages for the gates (`.venv-dev`, `out/venv-upstream-*`) | `pip install -r` from PyPI, pinned versions | gap: versions are pinned, hashes are not (C9) |

## Rules

- **C1 — Pin by immutable identity.** A pin is a full commit hash plus, for a tag, its
  tag-object hash, recorded in `docs/PROVENANCE.md`. Upstream's and Maya's release tags
  are **not signed** (checked 2026-10-21 for `v0.2.1.3`, `v0.2.2`, `v1.1.21`), so the
  reviewed commit hash *is* the identity; a moved tag never moves our pin. Build-time
  fetches are allowed only by full commit hash (`GIT_TAG <40 hex>`), and after a build
  the fetched checkout must equal it and be clean:
  `git -C <build>/_deps/strata_llamacpp-src rev-parse HEAD` and `status --porcelain`.
- **C2 — Review before execution.** Between fetching a new pin and passing C3 and C4,
  only read-only git commands may touch it: no configure, build, `setup.py`, test suite
  or script from the new tree. (The 2026-10-21 bump to `v0.2.2` built the engine and ran
  upstream's suites before this contract existed; its review was done afterwards and is
  recorded as such.)
- **C3 — Automated diff scan with a ruling per finding.**
  `scripts/security/engine_review.py diff --repo <submodule> --old <old tag> --new <new tag>
  --rulings docs/security/engine-reviews/<engine>-<old>-<new>.json --report …md` scans
  every added line for: network use, process launch, runtime code loading, persistence
  or cross-process access, build-time fetches and commands, embedded URLs, decoding of
  embedded data, long encoded blobs, and it lists every added or changed binary file,
  checksum file and dependency manifest. Each finding has a stable id. `--write-template`
  writes the rulings skeleton; every finding then needs `benign`, `accepted` (a real
  capability, judged acceptable, with why) or `refused` (blocks the bump), plus a reason
  written after reading the surrounding code. The gate exits non-zero while any finding
  is unruled or refused.
- **C4 — Every compiled and build line is read.** The diff of the surfaces we compile
  (`src include cmake CMakeLists.txt third_party` for upstream; the fetched dependency's
  sources when its pin moves) is read in full by a reviewer, file by file, for the points
  in C3 plus file-system writes outside the documented inputs, hidden or time/host-gated
  behaviour, privilege or system-setting changes, and protocol-injection risk. The notes go
  to `docs/security/engine-reviews/<engine>-<old>-<new>-compiled-read.md`, one verdict per
  file (`clean` / `note` / `concern`); a `concern` blocks until the owner rules on it.
- **C5 — Built-binary gate.** `engine_review.py binary --exe <engine binaries>
  --allowlist docs/security/engine-binary-allowlist.json`: every imported DLL must be in
  the allowlist; no network library (`ws2_32`, `winhttp`, `wininet`, `urlmon`, `dnsapi`,
  …) may be imported or named; no process-launch, remote-thread, service, registry-write
  or network function may be imported or named; an import by ordinal (no name for that
  rule to see) is refused unless the allowlist's `allowed_ordinals` rules it, naming what
  it binds (added 2026-10-21; the v0.2.2 engine imports nothing by ordinal); every DLL
  name embedded in the binary (what it can load at run time) must be allowlisted with a
  reason. A new name is a ruling in the
  bump's review file and an allowlist edit. Windows PE only today; the Linux (ELF) gate is
  owed (C9).
- **C6 — No foreign executables.** An added or changed binary file in the gated surfaces is
  refused unless the review rules it data that the engine reads (for example a profile
  table) and says why; prebuilt executables or libraries are never compiled in or loaded.
- **C7 — Dependency changes are reviewed.** A changed dependency manifest (requirements,
  `package.json`/lock, `Cargo.*`, …) is a C3 finding; a new third-party dependency gets its
  own review before anything installs it.
- **C8 — Containment stays on (defence in depth).** The engine runs as the frontend's
  child with the closed environment allowlist, private stdio, no inherited handles, in a
  Job object (Windows) / its own process group (POSIX), behind a loopback-only,
  token-authenticated listener (`threat-model.md`). Planned hardening: run the engine
  with no network at all (an AppContainer or firewall rule on Windows, `unshare -n` on
  Linux), since it needs none.
- **C9 — Record and sign off.** The bump commit's `docs/PROVENANCE.md` row cites the C3
  rulings file and the C4 notes by SHA-256, the C5 result, and the C1 fetch check. The
  owner signs off; an agent's review counts as the reviewer, never as the sign-off. Known
  gaps, each to be closed or explicitly accepted by the owner: pip installs are version-
  but not hash-pinned (`--require-hashes`); the C5 gate has no ELF mode; builds are not
  reproducible (a second build's hash is not compared).

## Procedure for a bump (inserted into `docs/PROVENANCE.md`, "Updating the baseline")

1. `git -C <submodule> fetch --tags origin`; resolve the new tag's commit and tag object;
   check whether the tag is signed (`git cat-file -p <tag>`).
2. C3: run the diff scan, write the template, read and rule every finding.
3. C4: read the compiled/build diff in full; write the notes file.
4. Only now: check out the new pin, build, and run the runbook's gates.
5. C5 on the built binaries; C1 fetch check on `_deps`.
6. C9: record the hashes and results in the PROVENANCE row; owner sign-off.

## Record

| Bump | C3 rulings | C4 read | C5 | C1 fetch | Sign-off |
| --- | --- | --- | --- | --- | --- |
| upstream `v0.2.1.3` → `v0.2.2` (2026-10-21, review done after the build, see C2) | `engine-reviews/strata-v0.2.1.3-v0.2.2.json`: 58 findings, 0 refused | `engine-reviews/strata-v0.2.1.3-v0.2.2-compiled-read.md`: 33 files, 0 concern, 12 note, 21 clean | PASS (`strata.exe`, `strata_kq.dll`, both pins) | llama.cpp `3cf03257`, clean, both builds | owner, 2026-10-21, in chat, answering "Sign off the v0.2.2 review and commit this work?": "yes commit and push" |
| Maya `v1.1.8` → `v1.1.21` (2026-10-21) | not required: never compiled or executed (scope) | — | — | — | — |
