# Sub-project 4 — Explicit, verified model acquisition (`strata pull`, `import`, `export`)

Date: 2026-10-14. **Status: APPROVED 2026-10-14 — owner accepted every recommendation (see §12 "Decisions taken")**. Nothing in
this document is approved. The owner could not be interviewed, so every
genuinely open choice is in §12 with a recommendation, and the body is written
as if each recommendation were accepted. Revision: review rounds 1–3 fixes
(controller rulings R-1..R-8, the N10 seam ruling and the I-3 test-hook build
ruling applied). Parent effort: Secure Strata full
release path (SP0–SP7), see [`docs/AGENT_HANDOFF.md`](../../AGENT_HANDOFF.md)
Phase 4.

Inputs, cited by short name:

- **PX** — [product-experience spec](2026-10-13-product-experience-and-upstream-tracking-design.md)
  (APPROVED): `pull`/`import`/`list`/`rm`/`show`/`update` surface, §3 first
  run, §5 model management, §7 update, §11 sub-project mapping.
- **SP1** — [typed config, paths, tokens spec](2026-10-13-sp1-typed-config-paths-tokens-design.md)
  (APPROVED, being implemented): `roots.models`, the path module (§3.4, P1
  grammar), the strict JSON reader (C1), `EngineLaunch`, `secure_frontend_add_cases`
  (§3.10). SP4 consumes these; changes SP4 needs are listed as **SP1 deltas**
  (§4.9), executed after SP1 merges, not as changes to SP1 now.
- **SP2** — [portable supervision spec](2026-10-13-sp2-portable-supervision-design.md)
  (APPROVED): child launch without a shell (`posix_child_launch.hpp`, the Job
  Object launcher), typed environment allowlist.
- **TM** — [`docs/security/threat-model.md`](../../security/threat-model.md)
  "Artifact acquisition", "Privacy and offline operation".
- **AT** — [`tests/security/README.md`](../../../tests/security/README.md):
  DL-01..05 and PRIV-01..04.
- **PROV** — [`docs/PROVENANCE.md`](../../PROVENANCE.md), "Project Maya".
- **UP** — upstream `v0.1.43` acquisition path, read-only: `upstream/setup.py`
  (`HF_REVISIONS` :65-70, `MODELS` :203-232, `UNSLOTH_*_SHARDS` :234-254,
  `FAMILIES` :258-300, `download()` :1455-1540, pack step :5517-5536, launch
  arguments :5571-5573), `upstream/tools/iq_pack.py` (602 lines),
  `upstream/tools/strata_tokenizer.py` (368 lines; :307-335),
  `upstream/tools/gguf_reader.py` (142 lines), `upstream/tools/_paths.py`.

Facts gathered for this draft on 2026-10-14 (read-only Hugging Face API reads
and `HEAD` requests, no download):

| Fact | Value |
| --- | --- |
| Upstream download mechanism | Python `urllib`, `User-Agent: strata-setup`, `HEAD` then ranged `GET` into `<dst>.part`, rename on size match (UP :1455-1540). Proxies come implicitly from `HTTP(S)_PROXY`; `HF_ENDPOINT` selects a mirror; a pinned revision falls back to `main` on 404 (UP :88). |
| Upstream digest checks | Only the Unsloth shards and the MTP tensors are SHA-256-checked; GSQ-RCO/Swift/Coder shards are checked by size and GGUF tensor directory only (UP :1542-1551). |
| Upstream derived artifacts | `--pack <data>/packs/<tag>` is **built locally** from shard 1 by `tools/iq_pack.py` (needs numpy and llama.cpp's `gguf-py`: `iq_pack.py:52`, `_paths.py:16`): `index.txt`, `dense.bin`, `native_experts.txt` (written last), `conversions.json`, optional `experts.bin`, and `tokenizer/` (`vocab.json`, `merges.txt`, `token_type.json`, `tokenizer.json`, and `chat_template.jinja` only when the GGUF has one, `strata_tokenizer.py:333`). The MTP draft is built from ranged reads of `Qwen/Qwen3.8-Flash-Next` (UP :5537-5555). `--expert-profile` comes from `upstream/data/expert-profile[-coder].bin` (UP :5572). Upstream has no GLM tooling. |
| Hugging Face redirect | `https://huggingface.co/<repo>/resolve/<sha>/<file>` answers `302` with `Location: https://us.aws.cdn.hf.co/xet-bridge-us/…?…X-Xet-Signature…` (signed, expiring query) and headers `x-linked-size` and `x-linked-etag` (the file's SHA-256, in double quotes). |
| `ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF` | `license:apache-2.0`; UP pin `ed59f92082b1e93c0e96d60a8b11aab089b52f09`; `IQ2_XS/…-00001-of-00002.gguf` 39 225 954 592 B sha256 `92cee27ae5bbadcd732416a0f7a7f0acc092399dbbe8f5a5efa707c2ec0a49d7`, `…-00002-of-00002.gguf` 28 800 138 432 B sha256 `316b46f3a2dbd68c900f43136ab9449f9dcc3725dfd8c794847c204bc161e113` |
| `ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-Coder-GGUF` | `license:apache-2.0`; UP pin `5348543e0147355ac9cbcb031184a3546350988e` |
| `ukisai/Swift-1.5-…-GSQ-RCO-GGUF`, `unsloth/Qwen3.8-Flash-Next-GGUF` | `license:other` (Swift Open License 1.0; Unsloth); current `main` differs from the UP pins |
| `peasantsmith/GLM-5.3-Flash-Maya-GGUF` (SP6 input, not in SP4's catalog) | `license:mit`, not gated, `main` = `f16ee4ba50d8fd2bb726ea1862ca6ca14ba188da`; four variants of 3–4 shards (94.7–156.3 GB); Maya-S shard 1 is 47 835 993 056 B. Maya's setup also builds a ~1 GB pack inside a **writable** model folder (`project-maya` `docs/SETUP.md` at `5c970a86`). |
| Maya drafter | `Anbeeld/GLM-5.3-Flash-DFlash2-GGUF`, `license:cc-by-nc-nd-4.0`. Never fetched or bundled by this project (PROV). |

## 1. Goals, non-goals, user stories

### 1.1 Goals

1. **`strata pull <name[:tag]>`** — the networked model operation: fetch
   exactly the files a digest-pinned manifest names, over HTTPS, from hosts the
   manifest allows, verify every byte by SHA-256 before anything becomes
   visible, publish atomically under `roots.models`, resume after
   interruption. Satisfies DL-02..04.
2. **`strata import <bundle>` / `export` / `pull --bundle` / `verify`** — the
   offline path: an online machine produces a bundle; the offline NVIDIA box
   verifies and publishes it with **the same verification and publish code**
   as `pull`. `import` ships first (PX §5).
3. **A model index** (`roots.models/index.json`) that is the single source of
   truth for installed models, plus `list`, `show`, `rm` (PX §2 rows owned by
   SP4), and **manifest-composed engine launch** (PX §5 "Launch
   composition").
4. **`update` semantics** for models (explicit diff, never automatic). The
   program update needs signatures, which are SP7 (§6.2); SP4's `update` for
   the program exits 12.
5. SP4 **contributes cases to** DL-01..04, PRIV-01, PRIV-03 and parts of
   PRIV-02/PRIV-04 and DL-05 (§10.3 names the owning SP of the rest: PRIV-02
   prompt/completion logging is SP1/SP3 logging; PRIV-04 release-manifest
   inspection and DL-05 SBOM are SP7). The TM amendment that names the
   networked verbs lands with SP4 (§7.4).

### 1.2 Non-goals

- No automatic, background, scheduled or server-initiated download, update or
  version check — ever (TM, PX §1).
- No model discovery or search, no mutable `latest`, no remote catalog (D2).
- No gated or authenticated sources: the config holds no secret and SP4 reads
  no environment variable value (§3.9; `doctor`'s proxy check is presence-only).
- No archives in v1 (TM "Initial releases should distribute verified files,
  not arbitrary archives"); bundles are directories (§5).
- No local model conversion (upstream `iq_pack.py`, `mtp_*`, Maya's pack
  builder); derived artifacts are D5.
- No GLM catalog entries: GLM manifest entries are SP6 (PX §11). SP4's schema
  accepts profile `glm53` with only `launch.native`; SP6 bumps the schema.
- No `push`, `create`, `cp` (PX §2 deferred); no vision files, MTP draft or
  any drafter in v1 manifests.
- No signatures (Ed25519 and SHA-512 are SP7, §6.2).

### 1.3 User story — online, Ollama terms

```
$ strata pull qwen3.8-flash-next:iq2_xs
```

1. `strata` resolves `qwen3.8-flash-next:iq2_xs` in the **built-in catalog**
   (§2.5). Nothing has touched the network.
2. It prints, before any connection: `name:tag`, catalog version and
   sequence, the manifest SHA-256 in full, license id and URL, whether the
   license allows bundling, every source **host** (never a full URL), every
   file's relative path, byte size and full SHA-256, the total, the free space
   on the `roots.models` volume, the sentence `The source host sees your IP
   address and which files you download.`, and whether the model is runnable
   with this release (§2.3, §2.7).
3. `Download 68.0 GB from huggingface.co? [y/N]` — default No; non-interactive
   stdin is No unless `--yes` (PX §3 step 6).
4. Files stream through the confined downloader child into
   `roots.models/.staging/<key>/`, hashed while they arrive; progress shows
   file, bytes, rate. Ctrl-C stops cleanly (exit 8) and prints `run 'strata
   pull qwen3.8-flash-next:iq2_xs' again to resume`.
5. A rerun resumes each partial file with an HTTP range after re-hashing the
   bytes already on disk.
6. When every file is verified: atomic rename into
   `roots.models/qwen3.8-flash-next/iq2_xs/v<sequence>-<12 hex>/`, then
   atomic index update. Output ends with `strata run qwen3.8-flash-next:iq2_xs`.
7. `strata run` composes the engine arguments from the manifest's `launch`
   block (§2.4).

**Honest limitation (D5):** this story is complete only once D5 (A) or (C)
gives the Qwen `--pack` files a source. The engine needs a pack that upstream
builds locally from shard 1 with Python; until D5 is decided the pack files
are pinned in the manifest without a download source (§2.7), so step 2 ends
with `manifest_requires_bundle` (exit 5) and the `--bundle` hint, and **with
D5 options (B)/(B′), v1 networked `pull` yields no model a user can run**;
a runnable install then comes only from the bundle path of §1.4.

If the user instead types `strata run qwen3.8-flash-next:iq2_xs` and the model
is missing, D6 decides what happens (recommendation: the same in-process pull
flow with the same confirmation, PX §3 step 6).

### 1.4 User story — air-gapped NVIDIA box

On the online dev machine:

```
$ strata pull qwen3.8-flash-next:iq2_xs --bundle /media/usb/qwen-iq2_xs
```

This downloads and verifies straight into a bundle directory (no install, no
second copy). Alternatively `strata export qwen3.8-flash-next:iq2_xs
/media/usb/qwen-iq2_xs` exports an installed model, re-hashing every file
while copying. Any writable local file system works except FAT/FAT32 when a
file is ≥ 4 GiB (always the case for these shards; refused with
`bundle_target_fat32`); exFAT, NTFS and ext4 are the expected choices. Under D5 (B) a developer completes the bundle's pack
files with the dev-only `tools/secure/make_bundle.py` (§2.7).

On the offline box (same `strata` release, so the same catalog), with no
downloader installed or configured:

```
> strata verify E:\qwen-iq2_xs        (optional: checks the copy, writes nothing)
> strata import E:\qwen-iq2_xs
> strata run qwen3.8-flash-next:iq2_xs
```

`import` accepts the bundle only if its `manifest.json` digest is in this
binary's catalog or equals `--trust-manifest sha256:<hex>` (D7), copies each
file into staging while hashing it, and publishes exactly as `pull` does. For
manual cross-checks, `SHA256SUMS` works with `sha256sum -c SHA256SUMS` (Linux)
and `Get-FileHash -Algorithm SHA256` (Windows); `strata show --digests
<model>` prints the same lines for an installed model.

### 1.5 Command surface owned by SP4

| Command | Network | Writes | Exit |
| --- | --- | --- | --- |
| `pull <name[:tag]> [--yes] [--bundle <dir>] [--allow-downgrade] [--verify] [--discard-partial]` | yes (downloader child only) | `roots.models` or `<dir>` | §8.3 |
| `pull --manifest <file> --trust-manifest sha256:<hex> [same options]` | yes | same | §8.3 |
| `import <bundle> [--trust-manifest sha256:<hex>] [--yes] [--allow-downgrade]` | no | `roots.models` | §8.3 |
| `verify <bundle> [--trust-manifest sha256:<hex>]` | no | nothing | 0 / 4 / 5 |
| `export <name[:tag]> <dir>` | no | `<dir>` | §8.3 |
| `list [--all] [--outdated]` | no | nothing | 0 / 7 |
| `show <name[:tag]> [--digests]` | no | nothing | 0 / 5 |
| `rm <name[:tag]> [--force] [--retired]` / `rm --partials <name:tag>` | no | `roots.models` | 0 / 7 |
| `update` (models part, §6.1) | only after confirmation, as `pull` | as `pull` | §8.3 |
| `update` (program part) | no | nothing | 12 (§6.2) |

`--verify` re-hashes an installed model whose manifest digest matches instead
of reporting `already installed`. `--discard-partial` deletes this manifest's
staging before starting. `--manifest` takes a **local file path only**; there
is no URL form. All are `strata` CLI verbs; none is reachable from `strata
serve`, `strata-secure-server`, the HTTP API, a model or a config key (§7.1,
PRIV-03). `run` is conditionally networked only through D6.

## 2. Manifest format

### 2.1 Encoding and limits

Strict JSON read by SP1's shared reader `secure_frontend_json` (C1): UTF-8, no
BOM, duplicate and unknown keys are errors, integers only. Manifest-specific
bounds (internal constants): `kManifestMaxBytes` 1 MiB, `kManifestMaxDepth` 6,
`kManifestMaxFiles` 4096, `kManifestMaxSources` 8 per file,
`kManifestMaxHosts` 16; strings ≤ 1024 bytes except source URLs (≤ 2048) and
`license.notice` (≤ 512). Every string is printable ASCII `0x20–0x7E`, so
nothing a manifest says can inject terminal escape sequences when printed.

**Identity of a manifest = SHA-256 of its exact bytes.** Nothing is
re-serialized before hashing; the bytes are copied verbatim into staging, into
the version directory and into bundles.

### 2.2 Schema (version 1)

```json
{
  "format": "strata-model-manifest",
  "schema_version": 1,
  "name": "qwen3.8-flash-next",
  "tag": "iq2_xs",
  "version": "2026.10.14.1",
  "sequence": 1,
  "profile": "qwen",
  "engine": { "family": "strata", "min_version": "0.1.43" },
  "license": {
    "id": "Apache-2.0",
    "url": "https://huggingface.co/ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF",
    "bundle_allowed": true,
    "notice": "Qwen3.8-Flash-Next by Qwen; GSQ-RCO quantization by ISTA-DASLab (Apache-2.0)."
  },
  "hosts": ["huggingface.co"],
  "redirect_hosts": ["*.aws.cdn.hf.co", "cas-bridge.xethub.hf.co"],
  "files": [
    {
      "path": "weights/Qwen3.8-Flash-Next-GSQ-RCO-IQ2_XS-00001-of-00002.gguf",
      "role": "weights",
      "size": 39225954592,
      "sha256": "92cee27ae5bbadcd732416a0f7a7f0acc092399dbbe8f5a5efa707c2ec0a49d7",
      "sources": [
        "https://huggingface.co/ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF/resolve/ed59f92082b1e93c0e96d60a8b11aab089b52f09/IQ2_XS/Qwen3.8-Flash-Next-GSQ-RCO-IQ2_XS-00001-of-00002.gguf"
      ]
    }
  ],
  "launch": {
    "native": "weights/Qwen3.8-Flash-Next-GSQ-RCO-IQ2_XS-00001-of-00002.gguf",
    "ple_gguf": "weights/Qwen3.8-Flash-Next-GSQ-RCO-IQ2_XS-00002-of-00002.gguf",
    "pack": "pack",
    "tokenizer_pack": "pack/tokenizer"
  }
}
```

(The example shows one file; the real manifest lists both shards and every
pack file, and passes the cross-field rules below.)

| Key | Type / grammar | Rule |
| --- | --- | --- |
| `format` | `"strata-model-manifest"` | exact |
| `schema_version` | integer | `1`; else `manifest_unsupported_version` |
| `name`, `tag` | `validate_model_id` grammar (`[A-Za-z0-9][A-Za-z0-9._-]{0,127}`, not a Windows device name, no trailing `.`), ASCII-lowercased on input | tag `default` is reserved for catalog aliases (§2.5); the case-folded name `index.json` is reserved (it is a file in `roots.models`) |
| `version` | `YYYY.MM.DD.N`, `N` 1..999 | human label only |
| `sequence` | integer 1..2^31−1 | strictly increasing per `name:tag`; used by the downgrade check (§4.7) and the version directory name (§4.3) |
| `profile` | `"qwen"` \| `"glm53"` \| `"raw"` | selects the `launch` key set and the layout check. `raw` exists **only in builds with `SECURE_FRONTEND_TEST_HOOKS=ON`** (release builds reject it as `manifest_invalid`): no `launch` keys, no shard-set or layout rule, never runnable; it lets a third-party test manifest name any small real file for the manual HTTPS check (§10.4) |
| `engine.family` | `"strata"` \| `"glm"` \| `"none"` | `qwen` ⇒ `strata`, `glm53` ⇒ `glm`, `raw` ⇒ `none` (and `min_version` `0.0.0`) |
| `engine.min_version` | `MAJOR.MINOR.PATCH` (each 0..9999, an optional fourth `.N` component allowed) | compared with `kEngineVersion` (§2.3) |
| `license.id` | SPDX id or `LicenseRef-<name>` | shown before transfer |
| `license.url` | `https://` URL | shown, never fetched |
| `license.bundle_allowed` | bool | `false` ⇒ `export` and `pull --bundle` refuse (`license_forbids_bundle`); a release never embeds the file |
| `license.notice` | string ≤ 512 | shown before transfer and covered by the same confirmation (`--yes` included); a license **change** on update needs an interactive `y` (§6.1) |
| `hosts` | 1..16 DNS names | hosts `sources` may name: lowercase LDH labels, ≥ 2 labels, no IP literal, no port |
| `redirect_hosts` | 0..16 entries | extra hosts a redirect may reach. An entry may start with `*.`, which matches **exactly one** extra leftmost label; the part after `*.` must itself have ≥ 2 labels (`*.aws.cdn.hf.co` matches `us.aws.cdn.hf.co`, not `aws.cdn.hf.co` or `a.b.aws.cdn.hf.co`; `*.co` is invalid) |
| `files[].path` | SP1 P1 relative-path grammar, ≤ 1024 bytes | unique after ASCII case-folding; no component starting with `.`; the case-folded names `manifest.json`, `MANIFEST`, `STRATA-BUNDLE`, `SHA256SUMS`, `MISSING` are reserved at the top level; no component may end in `.part` (case-folded), the in-progress suffix (§3.7) |
| `files[].role` | `"weights"` \| `"pack"` \| `"tokenizer"` | layout check input; `vision` and `draft` are reserved and rejected in schema 1 |
| `files[].size` | integer 1..2^40 | exact byte count and byte limit |
| `files[].sha256` | 64 lowercase hex | digest of the whole file |
| `files[].sources` | 0..8 `https://` URLs | tried in order (§3.6). Host in `hosts`; port absent or 443; no userinfo, query or fragment; characters limited to RFC 3986 unreserved, `/` and `%XX` — in particular no `[`, `]`, `{`, `}`, space, `"` or `\` (curl globbing and config quoting, §3.3). 0 sources ⇒ the file arrives only by `import` (§2.7) |
| `files[].license` | optional object, same shape as top-level `license` | overrides it for this file |
| `launch` | object, keys per profile (§2.4) | every value names a listed file or a directory prefix of listed files |

Cross-field rules: the size total ≤ `pull.max_total_bytes`; every `weights`
file named `…-NNNNN-of-MMMMM.gguf` belongs to a complete set `00001..MMMMM` in
**one directory** (the engine finds sibling shards next to shard 1, UP :1553);
`launch.native` is the `00001` shard. Profile `raw` (test-hook builds) is
exempt from the shard-set and `launch` rules.

### 2.3 Engine-version and profile gating

`kEngineVersion` is derived at configure time from the `upstream` submodule:
`git -C upstream describe --tags --exact-match` with the leading `v` removed
(today `0.1.43`; the `upstream/bump-v0.1.44.1` branch would give `0.1.44.1`).
If the submodule HEAD is not exactly at a tag (a developer checkout between
tags) or the tree has no git metadata, configure fails unless the CMake cache
variable `SECURE_STRATA_ENGINE_VERSION` is set explicitly; it never guesses
from `git describe` distances. There is no `glm` engine
version until SP6.

`pull`/`import` accept a manifest this release cannot run and say so before
transfer; the index records it. `run`/`serve` refuse it with
`engine_profile_unavailable` or `engine_too_old` (exit 11).

### 2.4 `launch` keys and the arguments they imply

| Profile | Key | Argument | Value |
| --- | --- | --- | --- |
| `qwen` | `native` (required) | engine `--native <path>` | shard `00001` |
| `qwen` | `pack` (required) | engine `--pack <dir>` | directory holding `index.txt` |
| `qwen` | `ple_gguf` (optional) | engine `--ple-gguf <path>` | the shard holding `per_layer_token_embd`; upstream passes it only for ≤ 2 shards (UP :5571) |
| `qwen` | `tokenizer_pack` (required) | frontend `--tokenizer-pack <dir>` | `<pack>/tokenizer` |
| `glm53` | `native` (required) | defined by SP6 | shard `00001`; the only `glm53` key in schema 1 |
| `raw` (test-hook builds only) | none | none | `run` refuses it (`engine_profile_unavailable`) |

Paths are joined to the **canonical** version directory (SP1 P7). With a
managed model the operator pass-through of weight flags is removed (SP1 delta
c, §4.9); PX §5's `serve --expert` mode keeps `--engine-arg` for bring-up
only. `--expert-profile` (UP :5572) is not manifest data: the file ships with
the engine install (SP7) and SP5's typed pass-through table selects it
(`expert-profile-coder.bin` for the Coder family); SP4 neither carries nor
validates it. MTP (`--mtp`, `--spec`) is absent until a manifest can deliver
the draft (D5).

### 2.5 Where manifests come from (D2)

1. **Built-in catalog (default and only source).** Manifests live as
   `secure_frontend/catalog/<name>/<tag>.json`; the build embeds their bytes
   and a table `{name, tag, sequence, sha256}` in the `strata` binary. CMake
   computes each digest with `file(SHA256 …)`; a committed
   `secure_frontend/catalog/CATALOG.sha256` (`<hex>  <name>/<tag>.json` per
   line) must match, else a test fails, so a changed manifest needs a reviewed
   pin change. `pull` makes **no network request to find or fetch a
   manifest**.
2. **Third-party manifest, per invocation.** `--manifest <local file>
   --trust-manifest sha256:<hex>`: the file's digest must equal the given one;
   both are printed before transfer. Trust is not remembered. A third-party
   `name:tag` equal to a catalog entry is rejected
   (`manifest_shadows_catalog`).
3. **Bundles** carry `manifest.json`, accepted only under rule 1 or 2 (D7).

Aliases: the catalog may map tag `default` to one real tag per name; the model
installs under the real tag.

### 2.6 v1 catalog (D4)

| `name:tag` | Source @ revision | Files | License, bundle |
| --- | --- | --- | --- |
| `qwen3.8-flash-next:iq2_xs` (also `:default`) | `ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF` @ `ed59f92082b1e93c0e96d60a8b11aab089b52f09` | 2 shards (facts table) + pack/tokenizer files (§2.7) | Apache-2.0, `true` |
| `qwen3.8-flash-next:iq3_xxs`, `:iq3_s` | same @ same | 2 shards each + pack/tokenizer | Apache-2.0, `true` |
| `qwen3.8-flash-next-coder:iq1_m` | `ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-Coder-GGUF` @ `5348543e0147355ac9cbcb031184a3546350988e` | 2 shards + pack/tokenizer | Apache-2.0, `true` |

GLM-5.3 (Maya) entries are SP6's (PX §11); the DFlash2 drafter is never a
catalog entry. Every pin (repo, revision, file digests) is recorded in PROV in
the commit that adds the manifest (DL-05 contribution); digests come from the
Hub's LFS pointers (`x-linked-etag`, quotes stripped) **and** are re-verified
by downloading each file once on the dev machine before commit.

### 2.7 Files without sources (derived artifacts until D5 is decided)

A catalog manifest may pin a file by size and digest with no `sources` (the
Qwen pack files under D5 (B)). Then:

- `pull <model>` without `--bundle` refuses before any connection with
  `manifest_requires_bundle` (exit 5) and prints the steps below.
- `pull <model> --bundle <dir>` fetches every file that has sources into
  `<dir>/files/`, writes `manifest.json` and a `MISSING` list, and writes
  **neither** `SHA256SUMS` **nor** `STRATA-BUNDLE`; `import` then says
  `bundle_incomplete`.
- The dev-only `tools/secure/make_bundle.py <dir>` (never shipped; needs the
  dev venv with numpy and llama.cpp `gguf-py`, which upstream's pack tools
  import) runs upstream's pinned `iq_pack.py` on the bundle's shard 1, checks
  each produced file against the manifest digest, then writes `SHA256SUMS` and
  `STRATA-BUNDLE`. Under (B′) it instead writes a third-party manifest (§12 D5).
- `strata import <dir>` verifies and publishes as usual.

## 3. Fetch

### 3.1 Transport (D1)

We have no TLS library and add none. Recommended: **run the platform `curl`
as a confined child process** (§3.2), with an argument vector and a config
fed on stdin that we build, an empty environment, and **our own** framing,
byte limits and SHA-256. Verification, not transport, is the integrity
boundary (TM: downloaded bytes are untrusted until their digest is verified).

That does not make curl harmless: **curl parses attacker-controlled bytes
(TLS, HTTP/2, headers) and runs as the user**, so a curl memory-safety bug
reached by a malicious server is same-user code execution. Controls: (1) curl
is **confined** (§3.2); (2) the attack surface is narrowed by `--proto =https`
(no FTP, SOCKS, file, …), no redirects followed by curl, `--globoff`, no
decompression, no cookie engine; (3) a version floor plus the
`pull.downloader` override (D3) as hygiene. **What confinement does not stop**
(also stated in §7.4 and §13.2):

- **Network.** Landlock network rules need ABI ≥ 4 (Linux 6.7); this dev host
  (5.15) has ABI 1 and the offline box's kernel is unknown, so usually there
  is **no network restriction at all**, and even at ABI ≥ 4 only ports are
  limited, not hosts. Windows Low integrity restricts no network access. A
  compromised curl can therefore connect to any host, including LAN and
  loopback services — an SSRF pivot past §3.5's address policy, which only
  governs the requests *we* ask for.
- **Linux metadata.** Landlock does not mediate `chmod`, `chown`, `utime` or
  extended attributes, and below ABI 3 not `truncate(2)` by path: a
  compromised curl can change modes or truncate files it can name (denial of
  service; model files are caught by `--verify`, the token and config by
  their readers' checks).
- **Windows reads.** Low integrity blocks write-up but **not read-up**: a
  compromised curl can read the user's files, including the config and the
  DPAPI-wrapped token (which it can unwrap as the same user), and send them
  to any host.

Mitigations that do hold: every byte we accept is digest-verified, so a
compromised downloader cannot plant a model; the intended request is pinned
to a checked address (`--resolve`); the child has no credentials in its
environment or argv and, on Linux with Landlock, cannot read the home
directory, config, token or models. Deferred, to evaluate for SP7: an
AppContainer for the Windows downloader with file access denied by default
(and network limited to `internetClient`); a seccomp-bpf filter on Linux that
allows `connect` only to the pinned address and port (independent of the
Landlock ABI).

### 3.2 Downloader child: launch and confinement

The downloader is launched with SP2's launch primitives, extended by a seam
that **SP2's second run must provide** (dependency, ruled by the controller).
SP4 does not copy the launcher. Exact signature required:

- **POSIX** (`posix_child_launch.hpp`). Today `exec_child` dup2s the
  child's stdin/stdout/stderr onto 0–2, owns **fd 3 as `kReportFd`** (the
  pre-exec errno report pipe, `posix_child_launch.hpp:33,146-159`), sweeps
  from `kReportFd + 1`, then `execve`s, reporting any failure through fd 3.
  The seam adds a launch-options struct with:
  - `std::span<const int> keep_fds` — at the start of `exec_child`, **before**
    the 0..3 `dup2` sequence, each entry is lifted with
    `fcntl(fd, F_DUPFD_CLOEXEC, kLiftFloor)` (so a parent fd numbered 0–3 is
    not overwritten; `kLiftFloor` is above 4), and the sweep spares the
    lifted numbers;
  - `int (*pre_exec)(const void* ctx, std::span<const int> lifted) noexcept`
    and `const void* ctx`, called **after** the sweep and **before**
    `execve`, receiving the lifted descriptor numbers in `keep_fds` order. It
    returns 0 or an `errno` value; on non-zero, `exec_child` writes a
    one-byte marker `'H'` followed by the errno to **`kReportFd` (fd 3)** —
    i.e. `fail(kReportFd)`, not the lifted `report` copy, which the sweep has
    already closed — and `_exit`s. An `execve` failure keeps SP2's existing
    report (errno without the marker). The parent maps a marked report to
    `downloader_confinement_failed` and an unmarked one to the existing
    downloader reason (`ENOENT` → `downloader_missing`, `EACCES` →
    `downloader_untrusted_path`, other → `downloader_unsupported`); exit 9
    either way, and EOF on fd 3 still means a successful `execve` — the child
    never `execve`s curl unconfined after the frontend announced
    `confinement: full`.

  SP4's `ctx` (built before `fork`) holds the Landlock ruleset fd position in
  `keep_fds`, the optional record fd position (test-hook builds only), and
  the cwd path. The hook makes only async-signal-safe calls, in this order,
  returning `errno` at the first failure: `dup2(lifted_record, 4)` (clears
  `FD_CLOEXEC` on 4) and `close(lifted_record)` if a record fd exists;
  `prctl(PR_SET_NO_NEW_PRIVS, 1)`; `landlock_restrict_self(lifted_ruleset,
  0)`; `close(lifted_ruleset)`; `chdir(cwd)`. Landlock does not mediate
  `chdir`, which is why it may follow `landlock_restrict_self`. With
  confinement `reduced: no_landlock` there is no ruleset fd and the Landlock
  steps are skipped (announced before confirmation, §3.2 table). Release
  builds pass only the ruleset fd, so curl starts with exactly fds 0–2 (fd 3
  is close-on-exec, fd 4 is never created).
- **Windows** (`engine_stdio_process.cpp` launcher): three extra inputs,
  `HANDLE primary_token` (used with `CreateProcessAsUserW`; the restricted
  Low-integrity token of the Privilege row below — a restricted version of the
  caller's own token, which needs no `SeAssignPrimaryTokenPrivilege`),
  `std::span<const HANDLE> extra_inherited_handles` (added to the
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` of the `STARTUPINFOEX`, which is a set:
  order there carries no meaning), and `std::span<const uint8_t>
  crt_fd_block` copied into `STARTUPINFO.cbReserved2`/`lpReserved2`. In
  test-hook builds the record handle is the only extra handle and the block
  is the Microsoft CRT descriptor-inheritance layout, packed with no padding:
  a 4-byte `int` count `n = 5`, then `n` flag bytes, then `n` `HANDLE`
  values, so `cbReserved2 = 4 + n·(1 + sizeof(HANDLE))` (49 on x64).
  Entries: 0 stdin pipe and 1 stdout pipe with `FOPEN|FPIPE` (`0x01|0x08`),
  2 the null device with `FOPEN|FDEV` (`0x01|0x40`), 3 unused (flag 0,
  `INVALID_HANDLE_VALUE`, mirroring Linux's reserved fd 3), 4 the record
  handle with `FOPEN` (`0x01`). The fake does not rely on its CRT: it calls
  `GetStartupInfoW`, parses this block and writes to entry 4. The handle was
  opened by the frontend with `FILE_APPEND_DATA` and keeps that access in the
  child — integrity levels are checked when a handle is opened, not when an
  inherited handle is used — so the Low-integrity fake can write it. A real
  `curl.exe` ignores CRT descriptors 3 and 4. stdin stays exactly one line
  for transfers; the probe gets an **empty, already-closed stdin pipe**, so
  the entries never shift. Release builds pass no extra handle and an empty
  block (`cbReserved2 = 0`).

| Requirement | Linux | Windows |
| --- | --- | --- |
| Executable | `pull.downloader`, validated like SP1's `engine.executable` (absolute, canonical, regular file, file and parent not group/other-writable); `PATH` never searched | same rules, where the allowed writers for the downloader **also include the TrustedInstaller service SID** (`S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464`), and ACEs flagged `INHERIT_ONLY_ACE` (which grant nothing on the object itself, as on `System32`) are ignored, so the stock `System32\curl.exe` is accepted |
| Environment | empty | exactly `SystemRoot`, its value taken from `GetSystemWindowsDirectoryW()` (never from our own environment) |
| cwd | `roots.models/.staging/.cwd/` (empty, 0500) | same, empty, private DACL read-only |
| Privilege | `PR_SET_NO_NEW_PRIVS` in the hook | restricted primary token (`CreateRestrictedToken`, `DISABLE_MAX_PRIVILEGE`) at **Low** integrity (`S-1-16-4096`): no write-up to the user's medium-integrity files; read-up remains (§3.1) |
| Filesystem | Landlock: ABI probed with `landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)`. Handled rights = every right the probed ABI defines (ABI 1: all `READ_*`, `WRITE_FILE`, `EXECUTE`, `REMOVE_*`, `MAKE_*`; ABI 2 adds `REFER`; ABI 3 adds `TRUNCATE`; ABI 5 adds `IOCTL_DEV`). Rules grant only `READ_FILE`, `READ_DIR` and `EXECUTE` beneath those of `/usr`, `/lib`, `/lib64`, `/lib32`, `/bin`, `/sbin`, `/etc` **that exist** (a rule is added only for an existing directory, opened `O_PATH`), the canonical parent directory of `pull.downloader`, and `READ_FILE` on `/dev/null` and `/dev/urandom`; **no write right anywhere**. Consequently `roots.models`, the secret root and the config file are neither readable nor writable **provided none of them lies beneath an allowed directory**; before the spawn the frontend checks this (canonical paths, separator-boundary prefix test), and if one does — a config under `/etc`, or a `pull.downloader` placed in the home directory so that its parent covers the config or a root — it prints `confinement: reduced (readable: <key path>)`, records it in the log record and continues (the same D3 (i) rule as missing Landlock). With `--resolve` pinning the host and no proxy, curl performs **no name resolution** (so needs no resolver files); with `pull.proxy` set by name, it resolves the proxy name through `/etc` (allowed). stdout is an inherited pipe (already-open descriptors are unaffected). Whether every curl build needs nothing outside this set (e.g. `/proc/self`, `/run/systemd/resolve` when a proxy name is resolved) is **to verify** on the dev host and the offline box. | Low integrity; Job with `JOB_OBJECT_LIMIT_ACTIVE_PROCESS` = 1, `KILL_ON_JOB_CLOSE`, UI restrictions |
| Network | Landlock ABI ≥ 4: `LANDLOCK_ACCESS_NET_CONNECT_TCP` allowed only to port 443 (or the `pull.proxy` port); `BIND_TCP` handled with no rule | — |
| Unavailable | Landlock absent or disabled (probe returns an error): D3 decides; recommended: run with `confinement: reduced` printed before the confirmation prompt, written into the §7.3 log record, and reported by `doctor` | token or integrity-level creation failure: fail closed (`downloader_confinement_failed`) |
| Output | stdout pipe → our parser; stderr → null device (curl messages can contain URLs) | same |
| Inherited descriptors / handles | release builds: exactly stdin, stdout, stderr — no fd above 2 at `execve` (fd 3 is SP2's close-on-exec report pipe; no fd 4); test-hook builds: plus the record descriptor as fd 4 | release builds: exactly the three standard handles in the explicit handle list, empty CRT block; test-hook builds: plus the record handle, CRT entry 4 |

To verify on the Windows test box before release (§10.4): Schannel TLS at Low
integrity; certificate-revocation checks at Low integrity (CryptNet URL cache
access, which lives in the user profile); the inherited stdout pipe's DACL
admitting a Low-integrity writer.

**Probe spawn.** Every spawn of the downloader is one of two kinds:
(1) a **probe**: argv exactly `<curl> -q --version`, no stdin config, same
confinement and inheritance as a transfer, output parsed for `curl X.Y.Z` and
the TLS backend name; it opens no connection; (2) a **transfer** (§3.3). Every
transferring verb — `pull`, the models part of `update`, and D6 `run` —
validates `pull.downloader` (§3.9 lazy validation) and runs one probe **after
the user confirms and immediately before the first transfer** (below the D3
floor → `downloader_version`, exit 9); a declined or non-interactive-No run
therefore spawns nothing. `doctor --downloader` also validates
`pull.downloader` and runs one probe to report version, TLS backend and
confinement level, and warns that a custom `pull.downloader` built to read its
CA bundle from `SSL_CERT_FILE` will fail with `tls_failed` under the empty
environment; plain `doctor` spawns nothing. §7.1 and the PRIV-01 tests exempt
the probe explicitly.

### 3.3 Exact invocation (one request, one hop)

Argument vector (no URL in argv, so `/proc/<pid>/cmdline` shows none):

```
<curl> -q --config - --silent --include --suppress-connect-headers --globoff
       --proto =https --max-redirs 0 --tlsv1.2
       --connect-timeout <pull.connect_timeout_s>
       --speed-limit 1024 --speed-time <pull.stall_timeout_s>
       --max-time <remaining run budget, s>
       --user-agent strata --header "Accept-Encoding: identity"
       (--noproxy * | --proxy <pull.proxy>)
       [--resolve <host>:443:<address>]   (only without a proxy, §3.5)
       [--range <offset>-]                (only when resuming)
```

Config written to curl's stdin, then stdin closed: exactly one line
`url = "<validated URL>"\n`. The URL grammar (§2.2, §3.4) contains no `"` or
`\`, so no escaping is needed.

- `-q` first (no `.curlrc`); `--globoff` so `[]{}` are never expanded (they
  are also rejected by the grammar — belt and braces).
- No `-L`: curl never follows a redirect; **we** parse `Location` (§3.4).
- No `--compressed`, `--cookie`, `--netrc`, `--insecure`; no `--cacert` in
  release builds (§10.5 test hook only).

### 3.4 Response handling and redirects

We parse header blocks curl writes before the body (`--include`): each block
≤ 16 KiB and ≤ 64 lines, header names matched case-insensitively, status line
`HTTP/1.0|1.1 NNN …` or `HTTP/2 NNN`. **Blocks with a `1xx` status (`100`,
`103 Early Hints`) are skipped**; at most 4 such blocks, then
`response_malformed`. Proxy `CONNECT` responses are suppressed by curl.

| Status | Action |
| --- | --- |
| `200` | Valid only when no `--range` was sent; if one was, the server ignored it: discard the partial file and restart this file from byte 0, once per file. `Content-Length`, if present, must equal the remaining expected size, else `size_mismatch_announced` before any byte is written. |
| `206` | Valid only with `--range N-`; `Content-Range` must be exactly `bytes N-<size−1>/<size>`, else `range_mismatch`. |
| `301 302 303 307 308` | One `Location`: absolute `https://` URL or absolute path (resolved against the current host); ≤ 8 KiB; host matches `hosts` ∪ `redirect_hosts`; port absent or 443; no userinfo or fragment; path and query limited to RFC 3986 unreserved, `/`, `%XX`, and in the query also `=`, `&`, `+`, `,`, `;`, `:`, `@`, `!`, `$`, `'`, `(`, `)`, `*`; **never `[`, `]`, `{`, `}`, space, `"`, `\`** (`redirect_not_allowed` otherwise). The query (signed CDN URLs) is never printed, logged or stored. The next hop re-runs §3.5. At most 3 hops, else `redirect_limit`. |
| `416` | `range_mismatch` (we never ask past the size). |
| `429`, `5xx` | transient (§3.6) |
| other | `http_status` with the number only |

Body bytes go to the `.part` file **as they arrive**, hashed incrementally;
one byte past the manifest `size` kills curl's process group / Job and fails
the file with `size_exceeded`. Fewer bytes than expected when curl exits is
`size_short` (transient).

Curl exit-code mapping (curl's stderr is discarded, so only the code is used):

| curl exit | Reason | Retry |
| --- | --- | --- |
| 0 | per status table | — |
| 5, 6 | `dns_failed` (with a proxy: proxy or target name) | transient |
| 7 | `connect_failed` | transient |
| 18, 52, 55, 56, 92 | `size_short` / transient transfer error | transient |
| 28 | `stalled` (or `time_budget_exceeded` when the run budget is used up) | transient / no |
| 35 | `tls_failed` (handshake; on Windows also Schannel's offline-revocation failure, typical behind an intercepting proxy) | transient |
| 60, 58, 59, 77, 83, 90, 91 | `tls_untrusted` (certificate, pinning, CA store) | **not** retried on the same source |
| 1, 2, 3, 4, 43, 48 | `downloader_unsupported` (our argv or the build is wrong) | no |
| any other | `transport_failed` | next source |

### 3.5 DNS and address policy

Without a proxy, before each hop we resolve the host with `getaddrinfo`
(`AI_ADDRCONFIG`) and reject the hop if **any** returned address is in a
denied range; then we pin the first address with `--resolve` (an IPv6 address written as `[addr]`; that this form is accepted by every curl at or above the D3 floor, 7.81.0 included, is **to verify** — until verified, IPv6-only hops fail closed with `address_not_allowed`), so curl connects
to exactly the address we checked (no rebinding window). TLS still validates
the certificate against the host name. Denied unless
`pull.allow_private_addresses = true`:

| IPv4 | IPv6 |
| --- | --- |
| `0.0.0.0/8`, `10.0.0.0/8`, `100.64.0.0/10`, `127.0.0.0/8`, `169.254.0.0/16`, `172.16.0.0/12`, `192.0.0.0/24`, `192.0.2.0/24`, `192.168.0.0/16`, `198.18.0.0/15`, `198.51.100.0/24`, `203.0.113.0/24`, `224.0.0.0/4`, `240.0.0.0/4` | `::/128`, `::1/128`, `::/96` (IPv4-compatible), `::ffff:0:0/96`, `64:ff9b::/96` and `64:ff9b:1::/48` (NAT64), `100::/64`, `2001::/32` (Teredo), `2001:db8::/32`, `2002::/16` (6to4), `fc00::/7`, `fe80::/10`, `ff00::/8` |

With `pull.proxy` set, the proxy resolves the target; the check cannot see the
target address and is skipped for it (risk §13.2). The proxy may be private.

### 3.6 Retries, sources, budgets

- Transient failures: resume the same file from the bytes on disk, up to **5
  attempts** per source with back-off 2, 4, 8, 16, 32 s; then the next entry
  in `sources`. When every source has failed, the run's exit class follows
  the **last** source's reason (§8.2 "class"): 3 for transport reasons
  (partial bytes kept, a rerun may succeed after network or proxy changes),
  4 for verification reasons (the file's partial bytes are deleted). Each
  attempt
  starts again at the manifest's source URL; signed redirect URLs are never
  reused.
- Non-transient failures move to the next source without retrying.
- One file at a time, in manifest order.
- Budgets: `pull.max_total_bytes` (manifest total, before transfer);
  `pull.max_duration_s` (whole run, passed as the remaining `--max-time`);
  `pull.stall_timeout_s` (below 1024 B/s); `pull.connect_timeout_s`.

### 3.7 Staging, resume and abandoned partials

- Staging key `<k>` = first 32 hex of the manifest SHA-256;
  `roots.models/.staging/<k>/` holds `MANIFEST` (exact manifest bytes) and the
  files. A key directory whose `MANIFEST` does not hash to `<k>` is deleted
  and `stale_staging_discarded` is printed.
- Per file: `<path>.part` while incomplete; `<path>` once verified **in this
  run**.
- New run: a `.part` of length `L < size` is re-hashed, then resumed with
  `--range L-`; `L == size` is hashed; `L > size` is deleted. A suffix-less
  file from an earlier run is re-hashed before it is trusted — a run boundary
  is a trust boundary for staged bytes (no persisted hash state).
- Digest mismatch after a resumed fetch: delete, refetch once from byte 0; a
  second mismatch is `digest_mismatch` (exit 4) and the file is deleted.
- **Abandoned partials.** Before transfer, `pull` lists other staging keys
  whose `MANIFEST` has the same `name:tag` (older or other manifests), prints
  their bytes and asks `Delete <N> GB of partial downloads for other versions
  of <name:tag>? [y/N]`. `--yes` does **not** answer this prompt (deleting
  is never implied); non-interactive runs keep them and print the
  `strata rm --partials <name:tag>` command that deletes them.
  `--discard-partial` deletes this manifest's own staging first. Key
  enumeration skips `.cwd` (and every name that is not 32 lowercase hex), so
  the downloader's cwd is neither "stale" nor "abandoned". `list --all` and
  `doctor` report total staging usage per key.

### 3.8 Proxies (design statement)

`pull.proxy` is the only proxy source; absent ⇒ `--noproxy *`. Grammar
`http://<host>:<port>` or `https://<host>:<port>` (DNS name or IP literal,
port 1..65535), no userinfo, path or query. No proxy authentication in v1 (it
would put a secret in config). Environment proxy variables and WinINET/WPAD
are never read. `doctor` performs a **presence-only** check: if any of
`HTTPS_PROXY`, `https_proxy`, `ALL_PROXY` is set in its own environment and
`pull.proxy` is not, it prints a hint; it never reads or prints the value.

### 3.9 Configuration and environment

SP4 reads **no environment variable value**; the only environment access is
`doctor`'s presence-only proxy check (§3.8), which tests whether a name exists
and never reads its value. PX §4 says `STRATA_CONFIG` is read; SP1
§3.2 says the only variable read is the one named by `token.source`. SP1 is
the implemented truth; the conflict is open for a later PX addendum owned by
the controller, and SP4 depends on neither.

SP4 registers these keys in SP1's single schema list (same validator, same
`--check` output, same every-key CFG-01 tables):

| Key | Type | Default | Range / rule |
| --- | --- | --- | --- |
| `pull.downloader` | executable path | Linux `/usr/bin/curl`; Windows `GetSystemDirectoryW()` + `\curl.exe` | file rules §3.2; **validated lazily**: when a transferring verb runs (after confirmation) or `doctor --downloader` runs, never at `serve` start (a recorded CFG-01 exception in SP1's schema list, so a box without curl can still `serve` and `import`) |
| `pull.proxy` | string | absent | §3.8 |
| `pull.allow_private_addresses` | bool | `false` | §3.5 |
| `pull.max_total_bytes` | integer | 1 099 511 627 776 (1 TiB) | 1 MiB .. 16 TiB |
| `pull.max_duration_s` | integer | 172 800 (48 h) | 60 .. 604 800 |
| `pull.stall_timeout_s` | integer | 120 | 10 .. 3600 |
| `pull.connect_timeout_s` | integer | 30 | 1 .. 300 |

## 4. Verification and placement

### 4.1 Interfaces consumed

From SP1: `CanonicalRoot roots.models` and its handle; the P1 grammar; the
link-refusing handle walk (`openat2(RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|
RESOLVE_NO_MAGICLINKS|RESOLVE_NO_XDEV)` / fallback; Windows per-component
`OPEN_REPARSE_POINT`); `create_directory_within`; `secure_frontend_json`;
`validate_model_id`; `secure_frontend_add_cases`. From SP2: the launch
primitives (§3.2). SP4 adds `atomic_replace_within(root, rel, bytes)` (temp
file in the same directory with `O_CREAT|O_EXCL|O_NOFOLLOW` 0600, `fsync`,
`renameat`, `fsync(dir)`; Windows `CREATE_NEW` + protected DACL,
`FlushFileBuffers`, `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`) if SP1 does
not already expose it generically.

### 4.2 SHA-256

Our own FIPS 180-4 implementation, `secure_frontend_sha256` (no copied code):
streaming `update()`/`finish()`, 64-bit length counter, portable C++, 8 MiB
read chunks. A SHA-NI path is deferred (§13.1).

### 4.3 On-disk layout

```
roots.models/                           0700 / private DACL
  index.json                            0600, atomic replace
  .lock                                 0600, exclusive lock for the whole verb (§4.8)
  .staging/                             0700
    .cwd/                               0500, empty: the downloader's cwd
    <k>/                                one per manifest being acquired
      MANIFEST                          exact manifest bytes
      <files[].path>.part | <files[].path>
  <name>/<tag>/v<sequence>-<h12>/       0700; <h12> = first 12 hex of the manifest sha256
    manifest.json                       exact manifest bytes, 0600
    <files[].path>                      0600
```

The version directory name is derived from `sequence` and the manifest
digest, never from the human `version`, so two manifests can never map to one
directory. `<model-id>` in PX terms is `<name>/<tag>` on disk. Names starting
with `.` can never be model names (`validate_model_id`).

### 4.4 Checks before publish

1. Every listed file: size equals `size`, SHA-256 equals `sha256`.
2. The regular files under `.staging/<k>/` are exactly the manifest's files
   plus `MANIFEST`, which must hash to `<k>`; no symlink, junction, hard link
   (`st_nlink > 1` / `nNumberOfLinks > 1`), device or FIFO.
3. Layout for `qwen`: the `pack` directory has `index.txt`, `dense.bin`,
   `native_experts.txt`, `conversions.json`, and `tokenizer/{vocab.json,
   merges.txt, token_type.json, tokenizer.json}`; `chat_template.jinja` is
   required only when the manifest lists it (upstream writes it only if the
   GGUF carries a template). Then the frontend's `load_tokenizer_pack` runs on
   `<pack>/tokenizer` (`pack_layout_invalid` on failure). `glm53`: shard-set
   rule only, until SP6.

### 4.5 Publish and index (crash order)

All under `.lock` (§4.8).

1. Close every handle under `.staging/<k>/`; `fsync` each file and directory.
2. Rename `.staging/<k>/MANIFEST` → `.staging/<k>/manifest.json` (same
   directory, no-replace) and `fsync`.
3. `create_directory_within(roots.models, "<name>/<tag>")`.
4. Rename `.staging/<k>` → `<name>/<tag>/v<sequence>-<h12>`:
   `renameat2(…, RENAME_NOREPLACE)` (fallback `renameat` after an `fstatat`
   that finds no target, still under `.lock`); Windows
   `MoveFileExW(src, dst, MOVEFILE_WRITE_THROUGH)` (directories cannot be
   replaced). `fsync` both parents.
5. `atomic_replace_within(roots.models, "index.json", …)` naming the new
   version and moving the previous version of the same `name:tag`, if any, to
   the index's `retired` list.
6. Retired versions are deleted **only when no server holds them** (§4.8 asks
   the server, as `rm` does); otherwise they stay listed as `retired` and are
   removed by the next `pull`/`import`/`rm --retired` that finds them unheld.
   `doctor` and `list --all` report them with their size.

A crash between 4 and 5 leaves a version directory the index does not name:
it is **never loaded**. A later `pull`/`import` of a manifest whose digest
equals that directory's `manifest.json` digest (compared by digest, not by
directory name) re-hashes every file and adopts it instead of downloading;
`doctor`/`list --all` report it as `orphaned`.

`index.json` (strict JSON, ≤ 4 MiB) is `{"format": "strata-model-index",
"schema_version": 1, "models": [...], "retired": [...]}`. `models` entries
have exactly:

| Key | Type | Meaning |
| --- | --- | --- |
| `name`, `tag` | model-id grammar | as in the manifest |
| `version` | `YYYY.MM.DD.N` | display only |
| `sequence` | integer | downgrade check |
| `dir` | `v<sequence>-<h12>` | version directory under `<name>/<tag>/` |
| `manifest_sha256` | 64 lowercase hex | digest of `<dir>/manifest.json` |
| `origin` | `"catalog"` \| `"third_party"` | how it was trusted (§2.5) |

`retired` entries have `name`, `tag`, `dir`, `manifest_sha256`. At most one
`models` entry per `name:tag`. Reading an entry re-hashes its `manifest.json`
(≤ 1 MiB) and fails closed on mismatch (`index_inconsistent`). `list` shows
name, tag, version, size, origin and `runnable` / `needs engine <family> ≥
<version>` / `needs pack (bundle)`.

### 4.6 Disk space

- Preflight: free bytes on the `roots.models` volume (`fstatvfs` on the root
  handle / `GetDiskFreeSpaceExW`) ≥ (manifest total − bytes already staged
  under `<k>` − bytes adoptable from an unchanged installed version, §6.1) +
  `kFreeSpaceMarginBytes` (1 GiB), else `disk_space` (exit 6) before any
  connection. Retired versions not yet deleted count as used.
- Each `.part` is preallocated without changing its length:
  `fallocate(FALLOC_FL_KEEP_SIZE)` (unsupported file system: skipped);
  Windows `SetFileInformationByHandle(FileAllocationInfo)`; failure is
  `disk_space` before that file's transfer.
- Every 1 GiB written, free space is re-checked against the margin; `ENOSPC` /
  `ERROR_DISK_FULL` keeps the `.part`, kills curl and exits 6 (resumable).
- `pull --bundle` and `export` apply the same checks to the target volume.

### 4.7 Downgrade and replacement

A manifest whose `sequence` is lower than the installed one for the same
`name:tag` is refused (`manifest_downgrade`, exit 5) unless
`--allow-downgrade`; equal sequence with a different digest is
`manifest_conflict` (exit 5). Equal digest ⇒ `already installed` (exit 0, no
network) unless `--verify`.

### 4.8 Lock, `rm`, and "is it loaded?"

- `roots.models/.lock` is taken exclusively (`flock(LOCK_EX|LOCK_NB)` /
  `LockFileEx(EXCLUSIVE|FAIL_IMMEDIATELY)`) for the **whole** `pull`,
  `import`, `rm` and models `update`, from before staging is touched to after
  the index is written. A second such verb exits 7 with `models_busy`. (A
  48-hour pull therefore blocks `rm` of other models; accepted for v1.)
  `export` and `verify` take no lock and read through the index.
- "Loaded" is asked of the running server through SP2's server discovery
  (`server.json`) and its authenticated status route; with no server running,
  nothing is loaded.
- `rm`: refuse if the server holds the version unless `--force` after `stop`;
  write the index without the entry; delete the version directory bottom-up
  through the handle walk. `rm --retired` deletes unheld retired versions.

### 4.9 SP1 deltas (an SP1 addendum, executed after SP1 merges)

| Delta | Change | Test |
| --- | --- | --- |
| a | `enumerate_local_models` skips every directory entry whose name starts with `.` at the root (`.staging`, `.lock`) | `secure_frontend_model_inventory_tests.dl04_staging_ignored_by_inventory_scan` |
| b | `list --all`/`doctor` use a new `scan_model_root_files(root)` that returns every regular file with its path and size, derives no IDs, and therefore tolerates the same shard names across current, retired and orphaned versions; `enumerate_local_models` keeps its fail-closed behavior and is no longer called by `/v1/models` or launch (they read the index) | `secure_frontend_model_index_tests.scan_tolerates_repeated_shard_names_across_versions` |
| c | `EngineLaunch{profile, executable, model_id, args}`: for a managed model, `profile` comes from the manifest and `engine.profile` in config is rejected unless `serve --expert` (`engine_profile_conflicts_with_manifest`); `engine.model_id` becomes the default model reference (`name[:tag]`, resolved through the index — PX's `default_model`); weight flags (`--native`, `--pack`, `--ple-gguf`, `--embd-gguf`, `--native-head-gguf`, `--native-dense-gguf`, `--mtp`) join the deny-list for `engine.args` outside `--expert` | `secure_frontend_engine_arguments_tests.launch_composed_from_manifest_qwen`, `….engine_args_reject_weight_flags_for_managed_model`, `secure_frontend_config_tests.cfg01_engine_profile_rejected_for_managed_model` |
| d | `roots.tokenizer_pack` is valid only with `serve --expert`; a managed launch uses the manifest's `<pack>/tokenizer`; setting both outside `--expert` is a config error (no silent ignore) | `secure_frontend_config_tests.cfg01_tokenizer_pack_root_rejected_for_managed_model` |
| e | `pull.*` keys (§3.9) join the schema list, with the recorded lazy-validation exception for `pull.downloader` | `secure_frontend_config_tests.cfg01_pull_keys_table_driven` |

## 5. `import`, `export`, bundles

### 5.1 Bundle format (version 1, a directory)

```
<bundle>/                    any directory the user names (not a drive root)
  STRATA-BUNDLE              exactly "strata-bundle 1\n"; written last
  manifest.json              exact manifest bytes
  SHA256SUMS                 "<64 hex>  <relative path>\n", LF, sorted bytewise by path,
                             covering manifest.json and every files/<path>
  files/<files[].path>       the payload
  MISSING                    only in an incomplete bundle (§2.7)
  .staging/<k>/              only while `pull --bundle` is running or interrupted
```

`SHA256SUMS` is for humans and standard tools and is **never a trust
source**: `import` checks it agrees with `manifest.json` (same paths, same
digests, else `bundle_invalid`) and trusts only the manifest digest (§2.5
rules 1–2) and the per-file digests in the manifest.

### 5.2 Reading a bundle

The bundle directory is canonicalized once and opened as a handle; every file
is opened through SP1's link-refusing walk from it, so a symlink, junction,
mount crossing, hard link, device or FIFO is `bundle_layout`. There is no
archive and every path comes from the manifest and passes P1, so traversal
("zip-slip") has no input. Unknown entries under `files/` are `bundle_layout`;
unknown top-level entries are listed and ignored. Each file is read once:
copied into `.staging/<k>/` while hashed; then §4.4–4.5 run unchanged.
`verify` reads and hashes without writing. `import` and `verify` never need
`pull.downloader`.

### 5.3 Writing a bundle

`export <model> <dir>` and `pull --bundle <dir>`:

- `<dir>` must not exist, be empty, or (for `pull --bundle` only) contain
  nothing but `.staging/<k>/` for **the same manifest** plus already-verified
  `files/`, `manifest.json` and `MISSING` from that run, in which case the
  pull resumes. Anything else is `bundle_target_not_empty`. Created 0700 /
  private DACL.
- Refused when any file's effective `bundle_allowed` is `false`
  (`license_forbids_bundle`, exit 10) before any byte is written.
- Target checks: free space (§4.6); FAT/FAT32 refused when any file is ≥ 4 GiB
  (`statfs` `f_type == MSDOS_SUPER_MAGIC`; Windows
  `GetVolumeInformationByHandleW` name `FAT`/`FAT32`).
- `export` re-hashes each installed file while copying (bit rot is not
  propagated). `pull --bundle` stages under `<dir>/.staging/<k>/` and renames
  each verified file into `files/`; `manifest.json` is written at the end of
  the run, and `SHA256SUMS` and finally `STRATA-BUNDLE` only when the bundle
  is complete (§2.7).
- On removable exFAT/FAT media the OS ignores modes; `import` never relies on
  source permissions.

## 6. `update`

### 6.1 Models

- Never automatic, never at start, never in the background, never prompted by
  the server. The catalog is in the binary, so "is there a newer manifest?" is
  answered offline: `strata list --outdated` lists installed catalog models
  whose catalog entry has a higher `sequence`.
- `strata update` (models part) and `strata pull <name:tag>` on an outdated
  model print a **manifest diff** and ask `[y/N]`:

| Diff line | Shown |
| --- | --- |
| version / sequence | old → new |
| profile, engine requirement | old → new |
| license id, `bundle_allowed`, notice | old → new; a license change requires an interactive `y` even with `--yes` |
| hosts / redirect hosts | added, removed |
| files | added, removed, changed (path, size old → new, sha256 old → new) |
| bytes to download | added or changed files only |

  Unchanged files (same path and digest) are copied from the installed
  version into staging, re-hashed while copying, instead of downloaded.
  Publish is §4.5 (new version, index switch, old version retired).
- Third-party models update only by an explicit `pull --manifest …
  --trust-manifest …`.

### 6.2 Program

PX §7 assigns the download/verify/swap mechanism to SP4. SP4 delivers the
reusable parts (confined downloader, response parser, SHA-256, staging,
atomic swap, diff printer). A program update also needs a **trust anchor for a
future release's manifest**, which a digest in the current binary cannot be.
That is a signature: **Ed25519 verification moves to SP7** with these
requirements recorded now: SHA-512 in-tree (Ed25519 needs it); RFC 8032
strict decoding (reject `S ≥ L`; reject non-canonical encodings of `A` and
`R`; decide and document small-order `A`/`R` handling and cofactored vs
cofactorless verification); RFC 8032 §7.1 vectors plus Wycheproof-style edge
vectors; constant time not required for verify-only. Until SP7 ships it,
`strata update` for the program prints `program update not available in this
release; install the new release manually` and exits 12, with no network
access.

## 7. Privacy (PRIV-01..04)

### 7.1 What never happens

- No network access in `serve`, `list`, `show`, `rm`, `import`, `export`,
  `verify`, `key`, `version`, `doctor`, at install, start, failure or
  shutdown, or in `run` when the model is installed. A downloader
  **transfer** spawn happens only in `pull`, the models part of `update`, and
  — if D6 (a) is chosen — `run` for a missing model after the same
  confirmation. The only other spawn is the confined, connection-less
  **probe** (`-q --version`, §3.2), run by those same verbs after
  confirmation and by `doctor --downloader`; it is exempted explicitly here
  and in the PRIV-01 tests.
- No telemetry, analytics, crash upload, usage reporting, update check,
  remote configuration or phone-home.
- The HTTP API, model output, config keys and environment cannot start an
  acquisition: the acquisition library is reachable only from the `strata`
  CLI verb dispatcher; `serve`'s object graph never receives it, and
  `strata-secure-server` does not link it. Because PX's single `strata` binary
  does link it, the real proof is at run time (fake-downloader marker and
  socket audit, §10.3), with the link-level check as a supplement for
  `strata-secure-server` only.

### 7.2 What a pull reveals

Fixed `User-Agent: strata` (no version, OS or machine data);
`Accept-Encoding: identity`; no cookies, `Authorization`, `Referer` or custom
headers. The source host and CDN see the user's IP address, the files and the
time; the confirmation text says so (§1.3).

### 7.3 What is printed and logged

Terminal: source **hosts**, relative paths, sizes, digests, progress,
fixed-reason errors. Never: full URLs, queries, `Location` values, proxy URLs,
resolved IP addresses, curl output. If operator logging is enabled (SP1
`logs` root; off by default), one record per file: time, verb, `name:tag`,
manifest sha256, relative path, size, sha256, host, result reason, and
`confinement` (`full` or `reduced:<cause>`, cause ∈ `no_landlock`,
`readable:<key path>`).

### 7.4 Threat-model amendment (lands with SP4, under review)

TM "Privacy and offline operation", replacing "The sole future networked
operation is an explicit, foreground `acquire` command": "The networked
operations are the explicit, foreground `strata pull` (acquire), `strata
update`, and — only for a model that is not installed and only after the same
confirmation — `strata run` (if adopted, D6). Each runs only after the
operator confirms the manifest-pinned hosts and digests it displays. `strata
import`, `export` and `verify` are local. None can be invoked by the HTTP API,
a model, a plugin, configuration or automatic logic." TM "Artifact
acquisition" gains: "Every file is SHA-256-pinned by a manifest that is
compiled into the release or pinned by an operator-typed digest; redirects are
followed only by the frontend, only over HTTPS and only to hosts the manifest
names. The downloader runs confined (on Linux with Landlock: no file writes
and reads only below system directories; on Windows: Low integrity, which
blocks writes but not reads of the user's files). Confinement limits but does
not remove what a compromised downloader can do: it can open network
connections to any host, including LAN and loopback services (Landlock
restricts only ports and only from ABI 4; Low integrity restricts no network),
read and send the user's files on Windows, and on Linux change modes, owners
and timestamps and — below Landlock ABI 3 — truncate files. Digest
verification still prevents it from planting a model." This closes the
AGENT_HANDOFF "Known small gaps" item.

## 8. Error model

### 8.1 Rules

- Every error is a fixed reason code plus fixed text. Variable parts are
  limited to `name:tag`, manifest-relative path, host name, HTTP status
  number, byte counts, digests and config key paths. Never: URL path or query,
  `Location`, proxy URL, IP address, token, config values, curl output, or OS
  messages containing paths outside `roots.models`.
- No partial content is ever indexed on error.
- Every error says `resumable: yes|no`.

### 8.2 Reasons

"Same source" says whether the attempt is retried on the same source (§3.6);
"class" is the run's exit code when this is the final reason; "rerun resumes"
says whether bytes already verified or staged are reused by the next run.

| Reason | Meaning | Same source | Class | Rerun resumes |
| --- | --- | --- | --- | --- |
| `manifest_invalid`, `manifest_unsupported_version` | schema or limits (key path named) | — | 5 | no |
| `manifest_untrusted`, `manifest_shadows_catalog`, `manifest_downgrade`, `manifest_conflict` | §2.5, §4.7 | — | 5 | no |
| `manifest_requires_bundle` | a file has no sources and `--bundle` was not given (§2.7) | — | 5 | no |
| `model_unknown` | not in catalog or index | — | 5 | no |
| `dns_failed`, `connect_failed`, `tls_failed`, `stalled`, `size_short`, `transport_failed` | §3.4, §3.6 | retried | 3 | yes |
| `range_mismatch`, `size_mismatch_announced` | §3.4 | file restarts from 0 | 3 | yes (other files) |
| `host_not_allowed`, `redirect_not_allowed`, `redirect_limit`, `response_malformed`, `address_not_allowed`, `tls_untrusted`, `http_status` | §3.4, §3.5 | next source | 3 | yes |
| `size_exceeded`, `digest_mismatch` | body longer than `size`; second digest mismatch | next source / deleted | 4 | no (file deleted) |
| `pack_layout_invalid`, `bundle_invalid` (digest) | §4.4, §5.1 | — | 4 | no |
| `bundle_layout`, `bundle_incomplete`, `bundle_target_fat32`, `bundle_target_not_empty` | §5 | — | 7 | no |
| `time_budget_exceeded` | `pull.max_duration_s` | — | 3 | yes |
| `total_budget_exceeded`, `disk_space` | §3.6, §4.6 | — | 6 | yes (`disk_space`) / no |
| `models_busy`, `publish_failed`, `index_write_failed` | §4.5, §4.8 | — | 7 | yes |
| `index_inconsistent` | §4.5 | — | 7 | no |
| `license_forbids_bundle` | §5.3 | — | 10 | no |
| `downloader_missing`, `downloader_version`, `downloader_untrusted_path`, `downloader_unsupported`, `downloader_confinement_failed` | §3.2, §3.4 | — | 9 | yes (after fixing) |
| `declined` | No, or non-interactive without `--yes` | — | 2 | — |
| `interrupted` | SIGINT/SIGTERM/console Ctrl-C/Ctrl-Break/close | — | 8 | yes |
| `engine_profile_unavailable`, `engine_too_old` | `run`/`serve` (§2.3) | — | 11 | — |
| `stale_staging_discarded` | notice, not an error | — | — | — |

### 8.3 Exit codes (`pull`, `import`, `export`, `verify`, `update`, `rm`)

| Code | Class |
| --- | --- |
| 0 | success, or already installed |
| 1 | usage or configuration error |
| 2 | declined |
| 3 | transport failure, including every source exhausted on a transport reason (resumable) |
| 4 | verification failure (`digest_mismatch`, `size_exceeded` as the last source's reason, `pack_layout_invalid`, bundle digest failures) |
| 5 | unknown model; manifest invalid, untrusted, conflicting, downgrade, or requires a bundle |
| 6 | disk space or total budget |
| 7 | filesystem, permission, lock busy, index, bundle layout or target |
| 8 | interrupted (resumable) |
| 9 | downloader missing, untrusted, unsupported, too old, or not confinable |
| 10 | license forbids bundling |
| 11 | engine cannot run this profile (`run`/`serve`) |
| 12 | program update unavailable (§6.2) |

Interruption: kill the downloader's group / Job, `fsync` the current `.part`,
release `.lock`, print the resume command, exit 8. A hard kill leaves the same
state minus unflushed bytes (re-hashed on resume).

## 9. Threats and controls

| Threat | Control | Test (§10) |
| --- | --- | --- |
| Malicious mirror/CDN alters bytes | Per-file SHA-256 from a compiled-in or operator-typed manifest digest; nothing visible before verification | `fetch.dl03_wrong_digest_rejected_prior_version_intact` |
| curl exploited by a malicious server (same-user code execution) | Confined child: Linux no writes and reads only below system directories (Landlock), no new privileges; Windows Low IL (no write-up; read-up and exfiltration over its socket remain, §3.1); empty env and cwd; `--proto =https`, no redirects in curl, `--globoff`, no decompression; version floor as hygiene (D3) | `fetch.dl03_downloader_cannot_write_files`, `fetch.dl03_downloader_cannot_read_home` (Linux), `fetch.dl03_downloader_runs_at_low_integrity_windows` |
| URL globbing makes curl fetch many URLs into one stream | `--globoff`; `[]{}` rejected in manifest URLs and `Location` | `fetch.priv04_downloader_argv_exact`, `manifest.dl03_invalid_manifest_matrix`, `fetch.dl03_location_with_glob_characters_rejected` |
| Truncation, cut connection | Exact `size`; `size_short` resumes; digest at end | `fetch.dl03_truncated_body_rejected`, `acquire.dl02_resume_with_range_completes_and_verifies` |
| Endless/oversize body, disk fill | Kill at `size`+1; preflight, preallocation, 1 GiB margin, re-checks; `pull.max_total_bytes`; manifest bounds | `fetch.dl03_oversize_body_rejected_mid_stream`, `acquire.dl02_preflight_refuses_without_space`, `acquire.enospc_mid_stream_keeps_part_and_exits_6` |
| Range ignored / wrong `Content-Range` splices bytes | `200` to a range ⇒ restart; exact `Content-Range`; digest | `fetch.dl03_range_ignored_restarts_from_zero`, `fetch.dl03_content_range_mismatch_rejected` |
| Header-block confusion (`100`/`103`) | 1xx blocks skipped, bounded | `fetch.informational_header_blocks_skipped` |
| Redirect to HTTP / other host / loop | We follow redirects: HTTPS, allowlist, 3 hops | `fetch.dl03_redirect_to_http_rejected`, `fetch.dl03_redirect_off_allowlist_rejected`, `fetch.dl03_redirect_limit` |
| DNS rebinding / SSRF to LAN | Resolve ourselves, deny special ranges if any address matches, pin with `--resolve` | `fetch.dl03_private_address_rejected`, `fetch.dl03_mixed_public_private_answer_rejected` |
| Signed redirect URL leaks | Not in argv (config on stdin), not printed, logged or stored; curl stderr discarded | `privacy.priv02_pull_output_and_logs_contain_no_url_query`, `fetch.redirect_url_absent_from_downloader_argv` |
| Manifest substitution | Compiled-in digests checked against committed `CATALOG.sha256`; typed digest for third-party; `SHA256SUMS` never trusted | `acquire.dl03_untrusted_manifest_digest_rejected`, `manifest.dl05_catalog_pins_match_committed_list` |
| Downgrade / replay | Monotonic `sequence`; explicit `--allow-downgrade`; conflicts refused | `acquire.dl03_downgrade_rejected`, `acquire.dl03_sequence_conflict_rejected` |
| Third-party shadows curated model | `manifest_shadows_catalog` | `acquire.dl03_third_party_cannot_shadow_catalog` |
| Traversal / links in bundles or manifests | No archives; P1 grammar; link-refusing walk | `bundle.bundle_rejects_traversal_path`, `bundle.bundle_rejects_symlink_entry`, `bundle.bundle_rejects_junction_entry_windows`, `bundle.bundle_rejects_hardlinked_file` |
| Reserved-name or case-fold collisions | Reserved names; case-fold uniqueness | `manifest.reserved_manifest_names_rejected`, `manifest.dl03_invalid_manifest_matrix` |
| Concurrent verbs corrupt staging | `.lock` for the whole verb | `acquire.concurrent_pull_same_manifest_second_is_models_busy` |
| TOCTOU on rename | User-owned 0700 root; handle-relative opens and `renameat2(RENAME_NOREPLACE)`; same-user attacker out of scope (TM; SP1 §3.4) | `acquire.dl04_publish_uses_noreplace_and_never_overwrites` |
| Partial data selected for loading | Index is the only source; staging, orphans and retired versions never loaded; scan skips `.staging` | `acquire.dl04_interrupt_during_download_never_indexed`, `acquire.dl04_crash_between_publish_and_index_is_not_loaded`, `model_inventory.dl04_staging_ignored_by_inventory_scan` |
| Deleting a version the server has open | Retire, delete only when unheld | `acquire.replace_loaded_model_retires_old_version`, `acquire.replace_loaded_model_retires_old_version_windows` |
| Stale partial bytes / abandoned staging | Staging keyed by manifest digest; same-`name:tag` partials offered for deletion; usage reported | `acquire.dl04_stale_staging_from_other_manifest_discarded`, `acquire.abandoned_staging_reported_and_offered_for_deletion` |
| Downloader substituted, argv injection | Validated absolute path, no `PATH`; argv vector, no shell; URL only in stdin config after validation; `-q` | `fetch.dl03_downloader_untrusted_path_rejected`, `fetch.priv04_downloader_argv_exact` |
| Proxy/environment hijack | Empty env; `--noproxy *` unless `pull.proxy` | `fetch.priv04_downloader_env_is_empty` |
| Content-encoding confusion | No `--compressed`; identity requested; raw bytes hashed | `fetch.dl03_content_encoding_gzip_fails_digest` |
| Terminal escape injection | Printable ASCII manifests; output escaping | `manifest.dl03_invalid_manifest_matrix` |
| Slow-loris source | speed limit, connect timeout, run budget | `fetch.dl03_stalled_source_times_out`, `fetch.time_budget_exceeded_is_resumable` |
| Redistribution against a license (CC BY-NC-ND) | Drafter never in catalog; role `draft` rejected; `bundle_allowed=false` blocks export | `bundle.export_refuses_bundle_forbidden_license` |
| Server/API triggers a download | Library not reachable from `serve`; runtime marker and socket audit | `privacy.dl01_*`, `privacy.priv03_*` |

## 10. Testing strategy

### 10.1 Seams

- **Fake downloader** `secure_frontend_fake_curl` (test-only executable),
  **copied into `<scratch>/downloader/`** (a subdirectory that contains no
  root, config or secret, so the read-allow rule for the downloader's parent
  directory does not reduce confinement) with its scenario directory next to
  it (it finds the scenario relative to its own path, since its environment is
  empty), so tests run in parallel under `ctest -j`. It accepts exactly the
  §3.3 grammar and the stdin config (anything else ⇒ exit 98 and a recorded
  failure), records argv, environment and stdin, and replays a scripted
  response per URL: header blocks (including `100`/`103`), status, headers,
  body from a fixture, and switches for truncate-at-N, stall, exceed-by-N,
  ignore-range, wrong `Content-Range`, `Content-Encoding: gzip`, redirects,
  exit codes, and "try to write a file" (for the confinement tests).
- **Record channel (non-vacuous under confinement).** The fake cannot write
  files (that is the point of §3.2), so it reports over an **inherited
  descriptor**: in builds with `SECURE_FRONTEND_TEST_HOOKS=ON` only, the
  frontend opens `<scratch>/downloader-record.log` (`O_WRONLY|O_APPEND|
  O_CREAT|O_EXCL|O_CLOEXEC`, created by the frontend itself, before the
  spawn) and passes it to the child as descriptor 4 — on Linux through
  `keep_fds` and `dup2` in the pre-exec hook, on Windows as an extra
  inherited handle named by entry 4 of the CRT descriptor block (§3.2) — for transfer
  and probe spawns alike; the fake writes one JSON line per invocation (kind
  probe/transfer, argv, environment, stdin config). Release builds have no
  such descriptor. Because the frontend creates the file only when it is
  about to spawn, the file's absence is itself the DL-01 marker, and every
  "never spawns" case has a **positive control** in the same harness:
  `privacy.dl01_positive_control_pull_spawn_is_recorded` runs `pull` on a
  fixture and must find a transfer record; `privacy.priv01_positive_control_doctor_downloader_probe_is_recorded`
  must find exactly one probe record.
- **Resolver seam**: scripted addresses per host.
- **Clock seam** for back-off and budgets; **free-space** and **file-system
  type** seams; a **fault-injection hook** between publish steps 4 and 5.
- Scratch roots per test under the build tree (SP1 §3.10); small synthetic
  fixtures; no public network, no real weights.

### 10.2 Test binaries

Registered with `secure_frontend_add_cases`; full names are
`secure_frontend_<binary>_tests.<case>`; in the tables below `fetch.x` means
`secure_frontend_fetch_tests.x`. Windows-semantics cases carry the
`windows-runtime` label and a `_windows` suffix.

| Binary | Scope |
| --- | --- |
| `sha256` | FIPS 180-4 vectors (empty, `abc`, 448-bit, 896-bit, one million `a`), chunk boundaries, > 2^32-byte length via a length-only hook |
| `manifest` | schema, limits, cross-field rules, wildcard hosts, reserved names, catalog embedding |
| `fetch` | argv/stdin construction, confinement, response parsing, redirects, DNS policy, exit-code mapping, retries, budgets |
| `acquire` | staging, resume, lock, verification, layout, publish, index, retire, rm, downgrade, update diff |
| `bundle` | import, export, verify, `pull --bundle` |
| `model_index` | index read/write, `scan_model_root_files` |
| `privacy` | DL-01 and PRIV-01..03 lifecycle checks with positive controls; `doctor_proxy_hint_is_presence_only` (variable set to a sentinel value; the sentinel never appears in output) |

### 10.3 Acceptance mapping

| ID | Owner of the rest | SP4 cases |
| --- | --- | --- |
| DL-01 | — | `privacy.dl01_serve_and_infer_never_spawn_downloader` (fake engine; no record file after start, `/health`, `/v1/models`, chat, stop — §10.1 record channel), its positive control `privacy.dl01_positive_control_pull_spawn_is_recorded`; `privacy.dl01_server_makes_no_outbound_connection` (Linux `LD_PRELOAD` shim recording `connect`/`getaddrinfo`/`sendto` to non-loopback); `privacy.dl01_server_makes_no_outbound_connection_windows` (`GetExtendedTcpTable`/`GetExtendedUdpTable` for the pid); `privacy.dl01_server_makes_no_outbound_connection` is labelled `release-also` and also runs hooks-OFF (§10.5) |
| DL-02 | — | `acquire.dl02_correct_digest_publishes_atomically`, `acquire.dl02_byte_limit_enforced_while_streaming`, `acquire.dl02_resume_with_range_completes_and_verifies`, `acquire.dl02_published_files_are_private`, `acquire.dl02_published_files_have_private_dacl_windows`, `acquire.dl02_preflight_refuses_without_space`, `bundle.dl02_import_bundle_publishes_same_as_pull`. **HTTPS itself** is exercised only by the real-curl checks: the §10.5 hook test where enabled and the required manual step in §13.5 item 4. |
| DL-03 | — | `manifest.dl03_http_url_rejected`, `fetch.dl03_wrong_digest_rejected_prior_version_intact`, `fetch.dl03_truncated_body_rejected`, `fetch.dl03_oversize_body_rejected_mid_stream`, `fetch.dl03_redirect_to_http_rejected`, `fetch.dl03_redirect_off_allowlist_rejected`, `fetch.dl03_redirect_limit`, `fetch.dl03_location_with_glob_characters_rejected`, `manifest.dl03_invalid_manifest_matrix` (unknown/duplicate key, bad hex, size 0, `..`, absolute, case-fold collision, control byte, `[` in URL, role `draft`, incomplete shard set, `launch` naming an unlisted file, single-label wildcard base), `acquire.dl03_untrusted_manifest_digest_rejected`, `acquire.dl03_downgrade_rejected`, `acquire.dl03_sequence_conflict_rejected`, `acquire.dl03_third_party_cannot_shadow_catalog`, `fetch.dl03_private_address_rejected`, `fetch.dl03_mixed_public_private_answer_rejected`, `fetch.dl03_range_ignored_restarts_from_zero`, `fetch.dl03_content_range_mismatch_rejected`, `fetch.dl03_content_encoding_gzip_fails_digest`, `fetch.dl03_stalled_source_times_out`, `fetch.dl03_downloader_untrusted_path_rejected`, `fetch.dl03_downloader_cannot_write_files`, `fetch.dl03_downloader_cannot_read_home`, `fetch.dl03_downloader_runs_at_low_integrity_windows`, `bundle.dl03_bundle_digest_mismatch_publishes_nothing` |
| DL-04 | — | `acquire.dl04_interrupt_during_download_never_indexed`, `acquire.dl04_interrupt_during_download_never_indexed_windows` (`GenerateConsoleCtrlEvent`), `acquire.dl04_interrupt_during_verification`, `acquire.dl04_crash_between_publish_and_index_is_not_loaded`, `model_inventory.dl04_staging_ignored_by_inventory_scan` (SP1 delta a), `acquire.dl04_stale_staging_from_other_manifest_discarded`, `acquire.dl04_publish_uses_noreplace_and_never_overwrites`, `acquire.dl04_orphan_adopted_after_rehash_by_digest` |
| DL-05 | SP7 (SBOM, release provenance) | `manifest.dl05_catalog_pins_match_committed_list`; pytest `tools/secure/test_catalog_provenance.py` (every catalog repo, revision and file digest appears in PROV) |
| PRIV-01 | SP2 (crash-restart lifecycle), SP7 (install) | `privacy.priv01_lifecycle_and_crash_restart_make_no_network_use`, `privacy.priv01_local_verbs_never_spawn_downloader` (`list`, `show`, `rm`, `import`, `export`, `verify`, plain `doctor`, `run` with the model installed, `update` declined: no record), `privacy.priv01_positive_control_doctor_downloader_probe_is_recorded` (`doctor --downloader`: exactly one probe record, argv `-q --version`, no connection in the socket audit) |
| PRIV-02 | SP1/SP3 (prompts, completions, images, credentials in logs) | `privacy.priv02_pull_output_and_logs_contain_no_url_query` (sentinel in a fake redirect query absent from stdout, stderr, logs), `privacy.priv02_errors_contain_no_proxy_or_address` |
| PRIV-03 | — | `privacy.priv03_api_payloads_resembling_pull_trigger_nothing` |
| PRIV-04 | SP7 (release manifest, linked components) | `fetch.priv04_downloader_argv_exact` (`-q` first, `--config -`, `--globoff`, `--user-agent strata`, no `-L`, `--noproxy *`), `fetch.priv04_downloader_env_is_empty`, `fetch.priv04_downloader_env_is_systemroot_only_windows`, `privacy.priv04_secure_server_does_not_link_acquisition` (CMake link-set check; supplement only, §7.1); hooks-OFF (§10.5): `fetch.release_spawn_has_no_fd4`, `manifest.raw_profile_rejected_in_release_build`, `privacy.release_binary_lacks_hook_marker`; SP7: `fetch.release_spawn_has_no_extra_handle_windows` |

Further SP4 cases:

| Binary | Cases |
| --- | --- |
| `fetch` | `confinement_reduced_when_root_beneath_allowed_dir` (config under an allowed directory, and a downloader parent covering a root: `reduced:readable:<key path>` announced and logged), `pre_exec_failure_fails_closed_exit_9` (injected Landlock failure: curl never runs, exit 9), `record_fd_does_not_clobber_report_pipe`, `downloader_below_floor_exits_9`, `probe_argv_is_version_only`, `staging_cwd_not_treated_as_key`, `confinement_reduced_written_to_log_record`, `informational_header_blocks_skipped`, `redirect_url_absent_from_downloader_argv`, `curl_exit_codes_map_to_reasons`, `retry_backoff_follows_schedule` (clock seam), `time_budget_exceeded_is_resumable`, `tls_untrusted_moves_to_next_source`, `dl03_default_windows_curl_path_accepted_windows`, `confinement_reduced_is_announced_without_landlock` |
| `acquire` | `program_update_exits_12_without_network`, `run_missing_model_prompts_and_decline_exits_2`, `run_missing_model_non_interactive_exits_2`, `abandoned_partials_kept_with_yes`, `rm_partials_deletes_other_staging`, `all_sources_failed_exits_3_and_keeps_part`, `concurrent_pull_same_manifest_second_is_models_busy`, `enospc_mid_stream_keeps_part_and_exits_6`, `version_dir_name_unique_for_same_version_label`, `replace_loaded_model_retires_old_version`, `replace_loaded_model_retires_old_version_windows`, `abandoned_staging_reported_and_offered_for_deletion`, `update_diff_lists_changed_files_and_license`, `update_reuses_unchanged_files`, `rm_refuses_loaded_model`, `rm_retired_deletes_unheld_versions`, `pull_without_bundle_refuses_sourceless_files`, `run_refuses_glm53_without_engine` |
| `manifest` | `reserved_manifest_names_rejected`, `model_name_index_json_rejected`, `part_suffix_path_rejected`, `raw_profile_rejected_in_release_build`, `engine_version_parsed_with_fourth_component` |
| `bundle` | `bundle_rejects_traversal_path`, `bundle_rejects_symlink_entry`, `bundle_rejects_junction_entry_windows`, `bundle_rejects_hardlinked_file`, `bundle_rejects_unlisted_file_in_files_dir`, `bundle_sha256sums_must_match_manifest`, `export_refuses_bundle_forbidden_license`, `export_refuses_fat32_target_for_large_file`, `export_rehashes_and_rejects_bit_rot`, `verify_writes_nothing`, `pull_bundle_resumes_after_interrupt`, `pull_bundle_incomplete_without_pack_sources`, `import_works_without_downloader` |
| `model_index` | `index_inconsistent_manifest_fails_closed`, `scan_tolerates_repeated_shard_names_across_versions` |
| SP1-delta cases | listed in §4.9 |

### 10.4 What needs the owner's machines

| Check | Where |
| --- | --- |
| Real curl against Hugging Face: (1) with a `SECURE_FRONTEND_TEST_HOOKS` build, a third-party manifest of profile `raw` (§2.2) for one small public file (the 9 426 880-byte `vision/GLM-5.3-Flash-vocab.gguf`); (2) with a release build and stock curl, `pull --bundle` of one catalog model (profile `qwen`) | online dev machine, manual, recorded in the handoff (§13.5 item 4) |
| In-box Windows `curl.exe`: Schannel at Low integrity inside the Job; Defender during publish rename | Windows test box and the MSYS2 CI job |
| Landlock on the offline Linux box's kernel | offline Linux box (`doctor`) |
| `import` of a real bundle and `run` (once D5 yields a pack) | offline Linux and Windows NVIDIA boxes, SP7 validation kit |

### 10.5 Test-hook builds and release-only cases

`SECURE_FRONTEND_TEST_HOOKS` is a CMake option, `OFF` by default. It compiles
in: the record channel (§10.1), the `raw` profile (§2.2), the publish
fault-injection hook, and an optional `--cacert <file>` plus loopback
allowance, active only when a test config sets it, so an opt-in pytest can run
real `curl` against a Python `ssl` server on `127.0.0.1` (inert for a real
Hugging Face pull, which never sets it).

| Configuration | Hooks | Runs | Purpose |
| --- | --- | --- | --- |
| `linux-gcc`, `linux-clang`, `windows-llvm-mingw-cross`, `windows-msys2-clang` (existing test presets) | `ON` | every case except those labelled `release-only` (`ctest --preset … -LE release-only`) | the acceptance run, incl. DL-01/PRIV-01 record-channel cases |
| `linux-gcc-release` (new, one extra configure + build in CI) | `OFF` | only cases labelled `release-only` or `release-also` (`ctest --preset linux-gcc-release -L "release-only\|release-also"`), then `scripts/secure_distribution_check.py --release` over its staged artifacts | proves the shipped configuration |

`release-only` cases (hooks-OFF only): `manifest.raw_profile_rejected_in_release_build`,
`fetch.release_spawn_has_no_fd4` (a tiny test executable configured as
`pull.downloader`, in `<scratch>/downloader/`, answers the probe with `curl
8.4.0` and exits 0 only if no descriptor above 2 is open — counted by probing
`fcntl(fd, F_GETFD)` for every fd from 3 up to 1024, since Landlock leaves no
read access to `/proc`; `doctor --downloader` must report the probe as
successful), `privacy.release_binary_lacks_hook_marker`.
`release-also` cases (both configurations): the socket audit
`privacy.dl01_server_makes_no_outbound_connection` (and its `_windows`
variant in the Windows presets), which needs no hook and so is DL-01's
evidence in the shipped configuration. `ctest -N` lists both labels in both
configurations (registered by `secure_frontend_add_cases(... LABELS
release-only)` / `LABELS release-also`); the hooks-ON presets exclude only
`release-only`. The DL-01/PRIV-01 record-channel cases need hooks and are not
built in the release configuration. **Deferred to the SP7 release kit:** the
Windows hooks-OFF check `fetch.release_spawn_has_no_extra_handle_windows` —
no Windows hooks-OFF configuration is added now; note that `cbReserved2 == 0`
alone would not prove that no extra handle was inherited, so the SP7 check
enumerates the child's handles. `scripts/secure_distribution_check.py
--release` checks that the shipped binary lacks the hooks' marker string.
**The shipped binary is always the hooks-OFF build.**

## 11. Windows parity

| Topic | Windows behavior |
| --- | --- |
| Downloader | `GetSystemDirectoryW()` + `\curl.exe` (Schannel, system store); TrustedInstaller allowed as a writer of that path (§3.2); same argv and stdin config; env `SystemRoot` only. No `curl.exe` (Windows 10 before 1803) → `downloader_missing` with the hint to set `pull.downloader`. |
| Confinement | Restricted token, Low integrity, Job (`KILL_ON_JOB_CLOSE`, `ACTIVE_PROCESS` 1, UI limits), explicit handle list; created suspended, assigned, resumed (SP2 primitive). |
| Interrupt | `SetConsoleCtrlHandler`: terminate the Job, flush, exit 8. |
| Private files/dirs | Explicit protected DACL `D:P(A;;FA;;;<UserSID>)(A;;FA;;;SY)` (SP1 K3). |
| Paths | `\\?\` produced internally for long paths; manifest paths ≤ 1024 bytes of printable ASCII in P1 grammar. |
| File writes | `CreateFileW` (`CREATE_NEW`, or `OPEN_EXISTING` to resume), `FILE_FLAG_SEQUENTIAL_SCAN`; `FileAllocationInfo` preallocation; `FlushFileBuffers` before rename. |
| Renames | Index: `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`; payload files and `MANIFEST`: `MoveFileExW(WRITE_THROUGH)` without replace; version directory: `MoveFileExW(WRITE_THROUGH)` after closing every handle under it. |
| Sharing violations | Antivirus/indexer handles: `ERROR_ACCESS_DENIED`/`ERROR_SHARING_VIOLATION` on rename or delete retried 10 times over 5 s, then `publish_failed` (resumable); a retired version the engine holds open stays retired (§4.5). |
| Lock | `LockFileEx(LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY)`. |
| Space / FS type | `GetDiskFreeSpaceExW`; `GetVolumeInformationByHandleW`. |
| Links in bundles | Per-component `OPEN_REPARSE_POINT` + `FileAttributeTagInfo` (SP1 P5). |

## 12. Owner decisions

The body assumes each recommendation.

**D1 — Transport.** (a) Confined `curl` child on Linux and Windows, our own
framing and SHA-256 (§3). (a′) `curl` on Linux, WinHTTP (in the OS, no
dependency, in-process — so no child confinement) on Windows. (b) Our own
HTTPS on OS TLS: Schannel on Windows, and on Linux OpenSSL/GnuTLS — a
third-party dependency, rejected by policy. (c) Upstream's Python downloader —
quarantined, needs Python, rejected. (d) No networked `pull` in v1 (`import`
only).
*Recommendation: (a)*, `import` first. Honest risk: curl is a large C parser
of attacker-supplied bytes running as the user; a curl bug is same-user code
execution, contained by the confinement of §3.2 (where Landlock exists) and
narrowed by `--proto =https`, no curl-followed redirects, `--globoff` and no
SOCKS/FTP. Cost: a runtime dependency on a system curl; one process per hop;
platform confinement code. (a′) trades the external binary for an in-process
TLS stack with no confinement.
**Decision (2026-10-14): (a) system curl as a confined child (Linux: `PR_SET_NO_NEW_PRIVS` + Landlock, read-allow system dirs, no write; Windows: restricted/low-integrity token in the Job); every byte verified by SHA-256 and counted by us.**

**D2 — Manifest trust for `pull`.** (a) Built-in catalog pinned by digest,
plus per-invocation `--trust-manifest`. (b) Remote catalog
(`pull.manifest_sources`, PX §4) signed with a key in the binary (needs SP7's
Ed25519). (c) Remote catalog over HTTPS only (rejected: TLS is not the
integrity boundary).
*Recommendation: (a).* Cost: new models or revisions need a new release or a
third-party manifest; PX §4 is amended (§13.3).
**Decision (2026-10-14): (a) built-in digest-pinned catalog; third-party manifests only via `--trust-manifest sha256:<hex>` per run; no remote catalog in v1 (`pull.manifest_sources` dropped).**

**D3 — Downloader floor and confinement fallback.** Floor options: (a) curl
≥ 8.4.0 everywhere — **hygiene, not the control**: the CVEs that made 8.4.0
notable (CVE-2023-38545 SOCKS5, CVE-2023-38546 cookies) are not reachable with
this argv (no SOCKS proxy, no cookie engine); confinement (§3.2) is the real
control. Whether current in-box Windows `curl.exe` is ≥ 8.4.0 is **unverified**
(to check on the Windows box). (b) ≥ 8.4.0 on Windows,
≥ 7.81.0 on Linux, because LTS distributions backport security fixes without
changing the version (Ubuntu 22.04 ships 7.81.0, and so does this dev host);
(c) 7.55.0 (the first in-box Windows build; known CVEs). In all cases
`pull.downloader` can point at a newer curl. Fallback when Landlock is
missing: (i) run with `confinement: reduced` announced before confirmation and
reported by `doctor`; (ii) refuse unless a new key
`pull.allow_unconfined_downloader = true`.
*Recommendation: (b) and (i).* Disagreement noted: the ruling asked for an
8.x floor; on Linux that rejects security-supported distro builds (including
this host's), so (b) keeps 8.4.0 only where version numbers track fixes.
Cost: (b) trusts distro backports; (i) leaves old kernels without filesystem
confinement.
**Decision (2026-10-14): (b) and (i): curl floor 8.4.0 on Windows, 7.81.0 on Linux; `confinement: reduced` announced and logged when Landlock is unavailable.**

**D4 — v1 catalog contents (Qwen only; GLM is SP6).** (a) GSQ-RCO
`iq2_xs`/`iq3_xxs`/`iq3_s` and Coder `iq1_m` (Apache-2.0). (b) (a) plus Swift
1.5 and Unsloth after reviewing "Swift Open License 1.0" and Unsloth's terms.
(c) `iq2_xs` only.
*Recommendation: (a).* Cost: four pins in PROV; each needs a pack (D5).
**Decision (2026-10-14): (a) GSQ-RCO `iq2_xs`, `iq3_xxs`, `iq3_s` plus Coder `iq1_m` (Apache-2.0); GLM entries move to SP6.**

**D5 — Derived artifacts (the Qwen `--pack`; also MTP). Unresolved.**
Upstream builds the pack locally from shard 1 with `iq_pack.py` (602 lines)
plus `gguf_reader.py` (142) and `strata_tokenizer.py` (368), needing numpy and
`gguf-py`. Whether the output is byte-identical across runs and machines has
**not been tested**; that experiment needs a ≈68 GB download and is to be run
by the owner or in SP7, not now.
- (A) Owner-hosted packs: after the determinism check, the owner publishes
  packs built with pinned tools (upstream commit, numpy and gguf-py versions
  recorded in PROV beside the pack digests) in an owner-controlled repository;
  manifests give them sources; `pull` alone yields a runnable model. Cost:
  account, hosting of a few GB per quant (without `experts.bin`), a
  reproducibility check per upstream bump.
- (B) Pack files pinned by digest without sources (§2.7), completed by the
  dev-only `make_bundle.py`; requires determinism.
- (B′) If not deterministic: `make_bundle.py` writes a third-party manifest
  under tag `<tag>-local` whose digest the developer passes to `import
  --trust-manifest`.
- (C) A native C++ pack builder in our tree. Re-costed: the non-`experts.bin`
  path is a GGUF reader, tensor-directory planning, float conversions
  (F32/F16/BF16 round-to-nearest-even) and the tokenizer export —
  ≈1.1 k lines of Python, estimated 1.5–2.5 k lines of C++ plus a
  differential test against upstream's output; dequantization of quantized
  types (gguf-py) is needed only for `--compat-bf16` (Unsloth, outside D4 (a))
  and would add much more. It must follow upstream's pack format on every
  bump.
- (D) Qwen-only v1 through `import` of developer-built bundles; networked
  `pull` ships for third-party manifests and for catalog entries once (A) or
  (C) exists.
**With (B) or (B′), v1 networked `pull` yields no model a user can run.**
*Recommendation:* no recommendation between (A) and (C) until the owner's
determinism check; ship (D) as the v1 behavior in the meantime, with (B) or
(B′) as its developer mechanism depending on the check.
**Decision (2026-10-14): unresolved between (A) and (C); ship (D) meanwhile — plan implements (D) with a seam for A/C.**

**D6 — Does `run` offer to pull a missing model?** PX §3 step 6 states the
intent. (a) `run` runs the same in-process `pull` flow after the same
confirmation prompt (Ollama parity); `run` becomes conditionally networked in
§7.1, the TM text and the PRIV-01 local-verb list (only "with the model
installed" is local). (b) `run` prints the `pull` (or `import`) command and
exits non-zero — an explicit PX amendment.
*Recommendation: (a).* Cost: one more verb that can reach the network; the
confirmation and non-interactive No rule are unchanged.
**Decision (2026-10-14): (a) `run` on a missing model runs the same in-process pull flow after the same confirmation prompt (conditionally networked; PRIV-01 list updated).**

**D7 — Trust for bundle manifests.** PX §5 says a bundle has a **signed**
manifest. (a) A bundle's manifest is trusted by catalog digest or
`--trust-manifest` (§2.5). (b) Signed bundle manifests (needs SP7's Ed25519
and key custody).
*Recommendation: (a)* — the offline box runs the same release, so the catalog
already pins the digest; PX §5 is amended (§13.3). Cost: third-party bundles
need the digest typed on the offline box.
**Decision (2026-10-14): (a) bundle trust = catalog digest or `--trust-manifest`, no signature; Ed25519 + SHA-512 + program-update verification move to SP7; `strata update` (program) exits 12 until then.**

**Decisions taken.** On 2026-10-14 the owner accepted every recommendation: D1 confined system curl; D2 built-in catalog plus `--trust-manifest`; D3 floor 8.4.0 Windows / 7.81.0 Linux with reduced-confinement fallback; D4 Qwen GSQ-RCO catalog; D5 unresolved between (A) and (C), (D) ships meanwhile; D6 `run` offers the pull; D7 bundle trust by digest, signatures in SP7. Binding SP2 second-run seam (from §3.2): `keep_fds` lifted via `F_DUPFD_CLOEXEC` ≥ `kLiftFloor` before the 0..3 `dup2` sequence; `int pre_exec(const void*, std::span<const int> lifted) noexcept` runs after the sweep: `dup2(record,4)` → `PR_SET_NO_NEW_PRIVS` → `landlock_restrict_self` → close(ruleset) → `chdir`; non-zero → `H` marker + errno on `kReportFd(3)` → `downloader_confinement_failed`, exit 9. Windows: `primary_token`, `extra_inherited_handles`, `crt_fd_block` (n=5, packed, entry 4 = record handle).

Design statements formerly listed as decisions (not open): proxy only via
`pull.proxy` (§3.8); private/special ranges denied by default with
`pull.allow_private_addresses` (§3.5); acquisition only in the `strata` CLI
(§7.1); directory bundles (§5.1); staging under `roots.models/.staging`
(§4.3); re-hash at run boundaries (§3.7); Ed25519 in SP7 (§6.2).

## 13. Out of scope, risks, amendments, constraints

### 13.1 Out of scope / deferred

An AppContainer for the Windows downloader (closes read-up and file exfiltration; needs `internetClient` and Schannel/CryptNet checks — to verify); remote or signed catalogs; signatures and the program update release side
(SP7); GLM manifests, `launch` keys and layout checks (SP6); parallel file
fetch; per-chunk digests; SHA-NI; user-configured mirrors; gated sources and
proxy authentication; archives; vision files, MTP draft and any drafter; local
model conversion unless D5 (C); SP5's typed pass-through of non-weight engine
flags (incl. `--expert-profile`); macOS; `push`/`create`/`cp`.

### 13.2 Risks

- **No runnable Qwen model from `pull` alone** until D5 (A) or (C).
- **curl** parses hostile input as the user; confinement depends on Landlock
  (Linux ≥ 5.13 with the LSM enabled) and on Schannel working at Low
  integrity (to be verified on the Windows box). Residual even when confined
  (§3.1): Linux — `chmod`/`chown`/`utime`/xattr changes, and `truncate` below
  Landlock ABI 3 (kernel 5.15 is ABI 1); Windows — read-up of the user's files
  (config, DPAPI-wrapped token); both OSes — unrestricted outbound network
  from a compromised downloader (any host, LAN and loopback included; Landlock
  network rules need ABI ≥ 4 and limit ports only), i.e. an SSRF pivot and an
  exfiltration path. Deferred to SP7 evaluation: Windows AppContainer;
  seccomp-bpf `connect` pinning on Linux. Curl versions differ in
  `--include` framing and exit codes; the fake covers our parser, real builds
  only the owner's machines.
- **Proxy hides the target address** from §3.5.
- **Hugging Face changes** redirect hosts (today `us.aws.cdn.hf.co`; regional
  variants expected): pulls fail closed (`redirect_not_allowed`) until a
  release updates `redirect_hosts`. A deleted pinned revision fails with
  `http_status 404` (no fallback to `main`, unlike UP :88).
- **Engine opens files by path** after verification (SP1's accepted
  same-user residual).
- **Disk**: an update can need room for both versions; a retired version held
  by the server keeps its space until released.
- **Lock granularity**: a long pull blocks `rm`/`import` of other models.
- **SHA-256 speed**: portable code ≈300–500 MB/s; re-hashing a resumed 39 GB
  shard takes minutes.

### 13.3 Amendments to PX that SP4 requires (for review with SP4)

| PX text | Amendment |
| --- | --- |
| §4 new key `pull.manifest_sources` | Dropped; the catalog is in the binary (D2). |
| §3 table "Download staging" and §4 `paths.staging` | Staging is `roots.models/.staging/` (same file system and root handle as the destination); no `paths.staging` key. |
| §5 "`import` … signed manifest … verifies signature" | Bundle manifests are trusted by catalog digest or `--trust-manifest` (D7); signatures are SP7. |
| §5 "`models/<name>/<tag>/`" | Published to `models/<name>/<tag>/v<sequence>-<h12>/`; the index names the active version; older versions are retired. |
| §5 roles (`native`, `pack`, `ple_gguf`, `embd_gguf`, `native_head_gguf`, `native_dense_gguf`, `mtp`, `tokenizer_pack`) | File roles are `weights`, `pack`, `tokenizer`; engine wiring is the manifest's `launch` block (§2.4). |
| §3 step 6 "`run` … asks Download?" | Kept if D6 (a); replaced by "prints the pull/import command" if D6 (b). |
| §5 "Open design question for SP4: TLS" | Answered by D1. |
| §7 `strata update` | Model part as §6.1; program part exits 12 until SP7 (§6.2). |
| §4 `STRATA_CONFIG` | Not touched by SP4; the PX/SP1 conflict is open for a controller-owned PX addendum (§3.9). |
| §11 SP4 row "TLS decision; … `update` download/verify/swap" | Signatures move to SP7. |

### 13.4 Global constraints

- No third-party dependency in the frontend; curl is a system executable
  invoked at run time, configured by `pull.downloader`, never linked.
- `-Werror` on every preset; TDD — every item lands with its failing-first
  test; one commit per item naming item and tests.
- No change under `upstream/`.
- Configuration only through SP1's schema and validator (§3.9); SP4 reads no
  environment variable value (presence-only proxy check in `doctor`, §3.8).
- Errors and logs never contain tokens, URLs with queries, redirect
  locations, proxy URLs, IP addresses or config values (§8.1).
- Hermetic tests only (fake downloader, scripted resolver, local fixtures).
- `docs/AGENT_HANDOFF.md` "What has changed" and "Verification performed" are
  updated with every SP4 item; the TM amendment (§7.4), the PX amendments
  (§13.3) and the SP1 addendum (§4.9) are reviewed with SP4.

### 13.5 Order of work and acceptance

Order: (1) SHA-256, manifest parser, catalog embedding, index,
`atomic_replace_within`, SP1 addendum (§4.9); (2) `import`, `verify`,
`export`, `list`/`show`/`rm`, retire, manifest-composed launch; (3) confined
downloader wrapper, fake curl, fetch layer, `pull`, `pull --bundle`, resume,
budgets, DNS policy; (4) privacy lifecycle tests and the TM amendment; (5)
models part of `update`; program `update` stub (exit 12).

Acceptance:

1. `ctest --preset linux-gcc -LE release-only` green (hooks ON); every case
   in §10.3, §4.9 and §10.5 except the SP7-deferred
   `fetch.release_spawn_has_no_extra_handle_windows` is listed by `ctest -N`; `ctest --preset
   linux-gcc-release -L "release-only|release-also"` green (hooks OFF) and `secure_distribution_check.py
   --release` passes on its artifacts.
2. `windows-llvm-mingw-cross` builds with `-Werror`; wine `ctest -LE
   "windows-runtime|release-only"` green; after the owner pushes,
   `windows-msys2-clang` with `ctest -LE release-only` green including every
   `windows-runtime` case.
3. `pytest -q` green, including `test_catalog_provenance.py`.
4. **Required manual step** (the only real-HTTPS proof of DL-02): on the
   online dev machine, (1) with a `SECURE_FRONTEND_TEST_HOOKS` build, a real
   `pull` of the small `raw`-profile third-party manifest (§10.4); (2) with a
   release build and the stock curl, a `pull --bundle` of one catalog model,
   recorded in the handoff; `import` on an offline box recorded when the owner
   runs it.
5. TM, PX and SP1 amendments reviewed; PROV lists every catalog pin; no claim
   beyond what was run.
