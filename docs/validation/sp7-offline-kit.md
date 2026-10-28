# SP7 offline release validation kit (owner box)

Everything here runs on the **owner's own hardware**. Agents prove only the
local build gates; no Windows-runtime, GPU, or installed-release claim exists
until this kit's results are recorded in `docs/PROVENANCE.md` (the placeholder
row is there; keep it blank until you run).

One release = one kit run: build → air-gap carry → validate offline → record.

## 0. Prerequisites

- The GPU box: NVIDIA driver ≥ 580, CUDA runtime `libcudart.so.12` or `.13`
  obtainable, unlimited `LimitMEMLOCK` for the service user.
- A local model pack and tokenizer pack already installed (never fetched —
  the release is offline by default and must stay that way).
- The signing seed from `scripts/release/keygen.py --out-key ...` held
  **offline**; its pinned public key hex is the one the release binary must
  be built with.

## 1. Build the release artifact (build host)

```
cmake --preset linux-gcc-release                       # secure_frontend/
cmake out/build/linux-gcc-release \
     -DSTRATA_RELEASE_PUBKEY_HEX=<64-hex pinned key>   # sticks in the cache
STRATA_SIGNING_KEY=<path-to-seed-file> \
  scripts/release/build_release.sh --version <V> --out dist
```

An unsigned build prints the exact line `unsigned: not releasable` — it is a
test artifact, never a release. Record immediately after the run: the source
commit (`git rev-parse HEAD`), the version, the pinned public key hex, and
`sha256sum dist/strata-<V>-linux-x86_64.tar.gz dist/strata-secure_<V>_amd64.deb`.

## 2. Carry and unpack with the network off (GPU box)

Carry the tarball physically. With the network namespace closed:

```
unshare -rn tar -xzf strata-<V>-linux-x86_64.tar.gz
cd strata-<V>-linux-x86_64
sha256sum -c SHA256SUMS        # must be all OK; this is the digest re-verify gate
```

`sbom.spdx.json` and `secure-release-manifest.json` sit in the bundle: the
SBOM must name both engine pins (tags and 40-hex commits) with their licenses.

## 3. `doctor` and `doctor --deep`

```
bin/strata --config <file> doctor
bin/strata --config <file> doctor --deep
```

Expected: exit 0 with no installs or downloads. `doctor` must find the CUDA
runtime **next to the program** (copied only when it was present at build
time — an absent line here is a finding), the driver minimum (580), the
memlock setting, config validity, and port state. `--deep` adds model digest
verification. Any failure prints the human fix string; fix on the build host
and re-run §1, never patch the bundle in place.

## 4. `serve` and a real local chat

```
bin/strata --config <file> serve   # foreground, engine child, 127.0.0.1 only
```

With a second shell: `curl http://127.0.0.1:<port>/health`, then
`/v1/models`, then one non-streaming `POST /v1/chat/completions` with the
installed model id. Confirm the engine never opens an outbound connection
(`unshare -rn` around the server if you want the strict proof).

## 5. Program update, both directions

Serve the *next* release bundle over a local HTTPS mirror (operator-built,
self-hosted — the client only accepts `update.source` HTTPS URLs and pins
every hop). The mirror layout is fixed: the signed envelope at
`<base>/envelope`, every component file at `<base>/<file>` (plain names).

```
bin/strata --config <c> update             # shows the offer; verifies the envelope
                                           # against the BUILD-TIME pinned key
bin/strata --config <c> update --yes       # stages, verifies each component sha256,
                                           # flips current (previous kept)
bin/strata --config <c> version            # must report <Vnew>, previous = <V>
bin/strata --config <c> update --rollback  # pure pointer flip back; no network, no crypto
bin/strata --config <c> version            # back to <V>
```

Also record two refusals (each must be a refusal, not an install): an
envelope signed by an unpinned key is rejected (`update:` signature line),
and a tampered component fails its sha256 and leaves `current` untouched
(`update: failed; current version <V> untouched`).

## 6. Record the results

Keep a results file (every command, every exit code, every fixed output
line). Commit its SHA-256 — plus the artifact SHAs, the pinned key hex, the
tested commit, and the verdict — as the release-entry row in
`docs/PROVENANCE.md`. Agents cite that row, never the run output.

**No offline-release, GPU, or update-chain claim is made until that row
exists.**
