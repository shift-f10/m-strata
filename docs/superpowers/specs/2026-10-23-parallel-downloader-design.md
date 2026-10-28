# Parallel ranged downloader — design

Date: 2026-10-23. **Status: PROPOSED — awaiting the owner's go-ahead to implement.**

Owner request (2026-10-23): "Why is the download so slow? When I am downloading them it's much
faster"; then "Do it" (make our downloader as fast as the direct tools), after confirming what
upstream uses.

## Problem

Our confined SP4 downloader (`secure_frontend/src/downloader_command.cpp`
`build_transfer_argv`, driven by `src/fetch_file.cpp`) fetches each file over **one `curl`
stream**. There is no rate cap (`--speed-limit 1024` is only a stall floor). Hugging Face's CDN
(`us.aws.cdn.hf.co`, AWS/Cloudflare + Xet) throttles throughput **per connection**, so a single
stream is several times slower than a parallel downloader on the same link. Box run 2026-10-23: a
415 MB pull averaged ~3 MB/s.

## What the upstreams use (reference)

Both pinned upstreams use a **parallel ranged downloader** in `upstream/setup.py` (0.2.0); Project
Maya (`engines/maya-ref/maya.py`) has no downloader of its own — it `import setup as S` and reuses
it. The relevant constants (`upstream/setup.py`):

- `DL_CONNECTIONS = 16` — connections for a big file; `STRATA_DL_CONNECTIONS` overrides, capped
  1–32; `=1` restores one stream.
- `DL_CHUNK = 32 << 20` — 32 MiB per piece (what the piece list records, and the most a failed
  request loses).
- `DL_RUN = 4` — pieces one request asks for (4 × 32 MiB = 128 MiB), so a fresh connection does not
  spend its first seconds in TCP slow-start per piece (their measurement: 8 connections 66 MB/s
  with 128 MiB runs vs less per-piece).
- `DL_MIN_PARALLEL = 256 << 20` — files below 256 MiB are one stream.
- `DL_CHUNK_TRIES = 5`, `DL_ROUNDS = 30`, `DL_BACKOFF = 1.0` — retry/round bounds.
- Pieces are written at their offset into the file; finished pieces are tracked in
  `<name>.part.chunks` so a stopped download resumes only the missing pieces.
- `_check_ranged` trusts a ranged answer only on `206` with `Content-Range pos-end/total`, a
  fitting `Content-Length`, and the **same ETag** as the first answer; `_NoRange` (no 206, another
  range, another total) falls back to one stream. Their comment states the rationale verbatim:
  *"One TCP stream often cannot fill a fast line … a CDN that limits each connection."*

We mirror this design within our security model (below). We do **not** adopt `huggingface_hub` or
`hf_transfer` — upstream itself uses only the standard library, and our downloader stays curl.

## The obstacle: streaming hash

The current fetch hashes **as it writes, in order**: `src/fetch_file.cpp:129`
`target.hasher.update(bytes)` on each sequential body chunk, and `verify_and_finish`
(`:40-41`) compares `Sha256{target.hasher}.finish()` to the manifest's `sha256`. Parallel pieces
arrive out of order, so on-the-fly hashing is impossible. The fetch must change to:

1. Write each piece at its byte offset into the `.part` file (not append).
2. After all pieces complete, make **one sequential pass over the finished file** to compute the
   SHA-256, then compare to the manifest (unchanged guarantee: digest verification still prevents a
   planted model). This is what upstream does.

`FetchTarget` (`fetch_file.hpp:70`) currently carries a running `Sha256& hasher` and a
`PartFile& file` whose `append()` is used. The parallel path needs `PartFile::write_at(offset,
bytes)` and drops the running hasher in favour of the final pass. The single-stream path keeps the
streaming hash (it is still the right thing for small files and `_NoRange` servers).

## Design

### Shape

Per file, `fetch_file` chooses:

- **One stream** (unchanged) when `file.size < kDownloadMinParallel` (256 MiB), the resolved
  server does not honour ranges (`_NoRange`), or parallelism is disabled
  (`models.download_connections = 1` / a pull setting). This keeps today's exact behaviour,
  streaming hash included, for the common small-file and fallback cases.
- **Parallel ranged** otherwise:
  1. **Resolve once.** Do one request against the manifest source URL and follow our redirects
     (the existing hop loop, `kMaxRedirectHops`, host ∪ redirect-host allowlist, DNS resolve +
     address pin per hop) to the final CDN URL, reading its head: status, `Accept-Ranges`/`206`
     capability, `Content-Length` (must equal `file.size`), and `ETag`. The signed CDN URL
     (`X-Amz-…` query) is the one every piece request reuses; it is valid for its (minutes-long)
     expiry and is **never logged** (as now). If the head says no ranges or a wrong length,
     fall back to one stream.
  2. **Plan pieces.** Split `[0, file.size)` into 32 MiB pieces. Load the piece manifest
     (below) to skip pieces already on disk.
  3. **Fan out.** Run up to N (default 16, `max 32`) confined `curl` children concurrently, each a
     normal `build_transfer_argv` request to the **final URL** with `--range <lo>-<hi>` for a run
     of up to 4 pieces (128 MiB). Each child still: `--proto =https`, `--max-redirs 0` (the final
     URL needs none), its own DNS resolve + address pin, the Job/confinement, the stall floor. The
     C++ side writes each child's body at the piece's offset via `PartFile::write_at`, bounds the
     bytes to the piece length (`size_exceeded` on overflow), checks disk space, and reports
     aggregate progress.
  4. **Verify a piece** only structurally during transfer (`206`, `Content-Range`, length, same
     `ETag` as the head — `_check_ranged`); the **cryptographic check is the final whole-file
     hash**, never per piece (the manifest pins only the whole-file digest).
  5. **Retry / rounds.** A failed piece is retried `DL_CHUNK_TRIES` times with backoff; a round of
     the remaining pieces repeats up to `DL_ROUNDS`, within the overall `pull.max_duration_s`
     budget. Any piece that will not complete fails the file (transient vs fatal as today).
  6. **Finish.** When every piece is on disk, make the final sequential SHA-256 pass and compare;
     then publish (`verify_and_finish`, minus the running hasher).

### Piece manifest and resume (pause/resume)

Beside `<name>.part`, keep `<name>.part.chunks`: the file's total size, the final URL's ETag, and a
bitmap of completed 32 MiB pieces (fixed-width, owner-only, written with the atomic writer every
`DL_SAVE_EVERY`). On resume:

- Same ETag + same total → re-fetch only the missing pieces (true parallel resume).
- Different ETag or total (the object changed) → discard and restart (as the single-stream path
  does on a range mismatch today).

This is what the **download manager's pause/resume** rides on: `DownloadManager::pause` kills the
`strata pull` child (which kills its N curl children via the Job), leaving `<name>.part` and
`<name>.part.chunks`; the next `start` re-runs the same manifest, and the pull continues from the
piece bitmap. `cancel` still runs `strata rm --partials`, which must also delete `.part.chunks`.

### Configuration

- `models.download_connections` (server config, 1–32, default 16; 1 = one stream) — the model
  downloads path. Mirrors upstream's `DL_CONNECTIONS`.
- The CLI `strata pull` reads the same from its config (`pull.connections`), so both paths speed
  up together. Chunk size, run length, retry/round bounds are fixed constants matching upstream
  (not config), to keep the surface small.

## Security review points (must hold)

1. **Allowlist per connection.** Every piece request goes to the resolved final URL whose host
   already passed `hosts ∪ redirect_hosts`; each child re-resolves DNS and pins its address exactly
   as today. No piece request may target a host outside the allowlist. The one resolve-redirect
   phase is the only place a `Location` is followed; piece requests use `--max-redirs 0`.
2. **Whole-file digest unchanged.** The manifest pins the whole-file SHA-256; verification is the
   final sequential pass over the reassembled file. A wrong offset write, a piece from a changed
   object (ETag guard), or any corruption fails the hash and leaves no accepted file. No per-piece
   trust is introduced.
3. **Bounded.** Connections capped at 32; bytes per piece bounded (`size_exceeded`); total bounded
   by `file.size` and `pull.max_total_bytes`; time bounded by `pull.max_duration_s`; rounds/tries
   bounded. Disk-space checks keep running.
4. **Confinement unchanged.** Each curl is the same confined child (Windows Low integrity / Linux
   Landlock), fixed argv, closed environment, the Job. N children instead of one; the Job still
   kills them all with the parent.
5. **No secret leakage.** The signed CDN URL's query is never logged or echoed (already the rule);
   the `.part.chunks` file holds only sizes, an ETag and a bitmap.
6. **Fail closed.** `_NoRange`, a wrong length, a changed ETag, or any structural surprise falls
   back to one stream or fails the file — never silently accepts mismatched bytes.

PROVENANCE and `docs/security/threat-model.md` (Artifact acquisition) get updated: the acquire path
gains parallel ranged transfers; the trust boundary (whole-file digest, allowlist, confinement) is
unchanged.

## Test plan (TDD, RED evidence per task)

The transport is already a seam (`HopTransport`, `fetch_file.hpp:29`), so parallelism is testable
without the network:

- A fake ranged transport that serves `206`/`Content-Range`/`ETag` for arbitrary ranges, and
  variants that: ignore ranges (`_NoRange` → one-stream fallback), change the ETag mid-download
  (restart), drop a piece N times (retry then round), and return a wrong length.
- Cases: parallel assembly of a multi-piece file hashes equal to the pinned digest; out-of-order
  pieces land at the right offsets; a changed ETag restarts; `_NoRange` falls back and still
  verifies; a corrupted piece fails the whole-file hash; resume from a partial `.part.chunks`
  fetches only the missing pieces; the connection cap and byte/time bounds hold; pause (kill)
  then resume continues from the bitmap.
- The download-manager tests (`download_manager_test.cpp`) gain a pause/resume case over a
  multi-piece transfer, and `rm --partials` removes `.part.chunks`.
- Full MSVC suite green; then a **box run**: a real multi-GB (or the 415 MB test) pull at N=16
  vs N=1, with the measured MB/s, pause/resume mid-download, and the final SHA-256 equal to the
  pin.

## Task breakdown

1. `PartFile::write_at(offset, bytes)` and the final whole-file SHA-256 pass; keep the streaming
   hash for the one-stream path. RED/GREEN in the fetch tests.
2. The resolve-once head phase: follow redirects to the final URL, read status/length/ETag/range
   capability; `_NoRange` detection.
3. The piece planner + `.part.chunks` manifest (load/save/bitmap, ETag + total guard).
4. The parallel orchestrator: bounded N confined children, offset writes, aggregate progress,
   retry/rounds within the budget; the one-stream fallback.
5. Config keys (`models.download_connections`, `pull.connections`); wire both the CLI pull and the
   model downloads path.
6. `rm --partials` deletes `.part.chunks`; download-manager pause/resume over pieces.
7. Docs (README downloader section, threat model, PROVENANCE) + the box run.

## Note

This is the single most security-sensitive code in the frontend (the confined acquire path shared
by the CLI and the page). It warrants the project's usual spec → plan → RED/GREEN-per-task → full
suite → box-run discipline, not a rushed change. Estimated: a focused multi-session piece of work
with a box run to confirm the speed-up and the resume behaviour.
