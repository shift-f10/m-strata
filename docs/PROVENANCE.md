# Provenance

## This repository's license

The repository's own code is under the GNU Affero General Public License version 3 only: `LICENSE` is the FSF's text,
byte for byte (https://www.gnu.org/licenses/agpl-3.0.txt, fetched 2026-10-28, SHA-256
`0d96a4ff68ad6d4b6f1f30f713b18d5184912ba8dd389f86aa7710db079abcb0`). `NOTICE.md` holds the copyright line, the MIT notice of Strata (whose code much of this derives from; MIT permits
that), and an additional permission under AGPL section 7 for the MiniMax-M3.1-Flash material, which keeps the MiniMax
Community License (the three files that carry its template text are listed there). Every third-party component keeps
its own license as recorded below; the out folder carries `LICENSE` and `NOTICE.md` (make.py).

## Baseline

Upstream [Niko1221/Strata][strata] is the git submodule `upstream/`, pinned by
its gitlink at the annotated release tag `v0.2.2`.

- Submodule (`.gitmodules`): path `upstream`, URL
  `https://github.com/Niko1221/Strata.git`, no branch tracking.
- Pinned commit (the gitlink): `011c9e600a93bbc9fd451c563501b5831c32bbca`
- Tag object: `e6fff4ad12908de74952abe53b2bade45a68aa0f`
- Check: `git submodule status` prints
  ` 011c9e600a93bbc9fd451c563501b5831c32bbca upstream (v0.2.2)`.
- Supply-chain review of `v0.2.1.3..v0.2.2` ([contract](security/engine-supply-chain.md), C9):
  rulings `security/engine-reviews/strata-v0.2.1.3-v0.2.2.json` (58 findings, 0 refused;
  SHA-256 `dfdf7e6b655977b1b2d4bbee1f824d3fb274f400a59cf8b5559efc5fea9510f7`), compiled-diff
  read `security/engine-reviews/strata-v0.2.1.3-v0.2.2-compiled-read.md` (0 concern; SHA-256
  `65e26faa115427e4181341341c30417a36f69cbab41d01fb12aaabd4f128aaa0`), binary gate PASS
  against `security/engine-binary-allowlist.json` (SHA-256
  `999d465a956e5a1087f41fbc10491240c6550a18ecf48912c1f2d2151200837f`), llama.cpp fetch
  `3cf03257` clean. Signed off by the owner on 2026-10-21. The review followed the build
  (the contract postdates it; C2).

The gitlink is the baseline record. Nothing under `upstream/` is edited and no
upstream file exists outside it; the repository root holds only our code (see
[`AGENT_HANDOFF.md`](AGENT_HANDOFF.md), "Repository layout"). Where we need
different behavior we wrap upstream from our own tree
(`tools/secure/secure_tokenizer.py`) or offer the change upstream. Exceptions:
none. `git -C upstream status --porcelain` is empty.

Everything under `upstream/` is under upstream's license
([`LICENSE` at `v0.2.2`](https://github.com/Niko1221/Strata/blob/v0.2.2/LICENSE);
offline: `upstream/LICENSE` in a checkout with the submodule initialized). The
root [`LICENSE`](../LICENSE) is this repository's license (above); [`NOTICE.md`](../NOTICE.md)
keeps Strata's notice with its "Copyright (c) 2026 Niko1221 and the Strata
contributors" line and points to `upstream/LICENSE`.

History: until 2026-10-13 this repository was a full clone of upstream with our
code mixed in, plus a recorded list of approved edits to upstream files. The
[submodule layout](superpowers/specs/2026-10-13-upstream-submodule-layout-design.md)
replaced it. The former integration hunk in upstream's root `CMakeLists.txt`
became our own root `CMakeLists.txt`; the appended `.gitignore` lines and the
README attribution header became our own `.gitignore` and `README.md`; the
`glm4` / `chatglm-bpe` edit to upstream's `tools/strata_tokenizer.py` became the
wrapper `tools/secure/secure_tokenizer.py`; and the root `pytest.ini` and
`conftest.py` are ours (they now collect `tools/secure/` only).

## Updating the baseline

An upstream update moves the gitlink; it is never a merge into our tree.

This is the only copy of the runbook; [`AGENT_HANDOFF.md`](AGENT_HANDOFF.md)
and [`secure-frontend-ci.md`](secure-frontend-ci.md) link here instead of
repeating it. `<old tag>` / `<old short sha>` / `<old tag object>` are the
current pin (today `v0.2.2` / `011c9e60` / `e6fff4ad`), `<new tag>` the
target. `<L>` is a label unique to the run (for example `old-<old tag>` and
`new-<new tag>`): build directories and venvs are never reused or deleted
between runs, so every run uses its own `out/<name>-<L>`.

Shell: `export PATH="$(git rev-parse --show-toplevel)/.venv-dev/bin:$PATH"`.
For direct wine calls (not the test preset, which sets it) prefix
`WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml=` from
`secure_frontend/`.

1. **Gates at the old pin first** (the gate list in step 3, with
   `<L>` = `old-<old tag>`), and record every count. Without this baseline a
   changed count cannot be attributed to the bump.
2. Fetch only: `git -C upstream fetch --tags origin`. Then the
   [engine supply-chain contract](security/engine-supply-chain.md) C1–C4 on
   `<old tag>..<new tag>` with read-only git: the diff scan with a ruling per
   finding (`scripts/security/engine_review.py diff`, rulings in
   `security/engine-reviews/`) and the full read of the compiled and build diff.
   Nothing from the new tree is configured, built, installed or run before both
   records exist. Then `git -C upstream checkout --detach <new tag>`.
3. Gates at the new pin (`<L>` = `new-<new tag>`), each with its working
   directory (plus, on any engine build made for the bump, the contract's C5
   binary gate — `engine_review.py binary` against
   `security/engine-binary-allowlist.json` — and the C1 check that the fetched
   `_deps` checkout equals the pinned commit and is clean):
   - `pytest -q` (repo root): our `tools/secure/` tests.
   - In `secure_frontend/`: `cmake --preset linux-gcc && cmake --build
     --preset linux-gcc && ctest --preset linux-gcc`, and the same three
     commands with the `windows-llvm-mingw-cross` preset (build with
     `-Werror`, then the wine `ctest`).
   - Root configure, build and `ctest` (repo root), from a fresh build
     directory, then confirm nothing was fetched:

         cmake -S . -B out/root-<L> -G Ninja && cmake --build out/root-<L> && ctest --test-dir out/root-<L>
         test ! -e out/root-<L>/_deps

   - Upstream's own suites in a separate venv (upstream's pins win there).
     `python3 -m venv` may lack `ensurepip`, so create the venv without pip
     and install into it with the dev venv's pip (repo root):

         python3 -m venv --without-pip out/venv-upstream-<L>
         .venv-dev/bin/pip --python out/venv-upstream-<L>/bin/python install \
           -r scripts/dev/requirements-dev.txt -r upstream/requirements.txt

     Then run them **from `upstream/`** (four `tools/` tests resolve
     `tools/...` against the working directory). Our root `pytest.ini` stays
     the rootdir, so `--deselect` ids keep the `upstream/tools/` prefix:

         ( cd upstream && ../out/venv-upstream-<L>/bin/pytest -q tools \
           --ignore=tools/test_iq_pack.py --ignore=tools/test_setup_engine_hash.py \
           --deselect upstream/tools/test_setup_choices.py::HipVision::test_the_cpu_encoder_is_built_once \
           --deselect upstream/tools/test_setup_choices.py::HipVision::test_without_images_nothing_changes \
           --deselect upstream/tools/test_setup_choices.py::CudaVision::test_the_encoder_is_built_native \
           --deselect upstream/tools/test_setup_golden.py::Golden::test_yes \
           --deselect upstream/tools/test_setup_golden.py::Golden::test_enter_for_every_question \
           --deselect upstream/tools/test_setup_download.py::ParallelDownload::test_a_big_file_comes_over_several_connections \
           && ../out/venv-upstream-<L>/bin/pytest -q serve \
           --deselect upstream/serve/test_lifeline.py::Lifeline::test_the_portable_watcher_ends_the_child_when_the_server_is_killed )

     The subshell keeps the shell at the repo root for the later steps.

     At `v0.2.2` the Linux counts are not recorded yet: the bump was made on
     the owner's Windows box (2026-10-21), where the same commands give
     `1027 passed, 11 failed, 30 skipped, 6 deselected` and `905 passed,
     23 skipped, 1 deselected` (at `v0.2.1.3` there: 1004/12 failed/24/6 and
     865/27/1; the 11 failures are Windows-host ones present at both tags —
     `docs/validation/2026-10-21-windows-gpu-session.md`, step 6). Run the
     Linux gate before treating the `v0.2.2` row as complete.
     At `v0.2.1.3`: `1027 passed, 13 skipped, 6 deselected` and `878 passed,
     14 skipped, 1 deselected`. At `v0.1.44.1`: `760 passed, 2 skipped,
     5 deselected` and `774 passed, 11 skipped` (at `v0.1.43`, with the first
     three deselects only: 616/2/3 and 695/11). Why each exclusion exists:
     - `test_iq_pack.py`: needs llama.cpp's `gguf` module.
     - `test_setup_engine_hash.py`: needs GitHub.
     - the three `test_setup_choices.py` ids: upstream drift against its
       `setup.py` (recorded at `v0.1.43`).
     - the two `test_setup_golden.py` ids: upstream `12be1059` re-recorded the
       golden configs on Windows; the `unsloth UD-Q4_K_XL` cases expect
       `strata-vision.exe`, which only matches on Windows.
     - `test_setup_download.py::ParallelDownload::test_a_big_file_comes_over_several_connections`
       (test new at `04221c3e`, retuned by `e2071c2f`, first runs at
       `v0.2.1.x`): the new length probe issues a whole-file ranged GET that
       the test's own `asked()` helper counts as a second piece-0 request
       (`[0, 0, 1, …]` where `[0, 1, …]` is expected); deterministic alone and
       under load. Upstream drift against its own test; quarantined Python.
     - `serve/test_lifeline.py::Lifeline::test_the_portable_watcher_ends_the_child_when_the_server_is_killed`
       (new at `48e6d64c`, first runs at `v0.2.1.3`): the watcher's liveness
       token is `ps -o lstart=,stat=`, and `stat` flips `R`→`S` as the child
       starts sleeping, so when the killed parent's pipe closes the token no
       longer matches and the watcher declines to end the child. A
       host-sensitive race in the new mechanism itself (the `pdeathsig` case —
       the Linux production path — passes); quarantined Python.

     **When the new tag adds a failing upstream test:** it is a finding, not
     something to force green. If it is in quarantined Python (`setup.py`,
     `serve/`, `tools/`) and the cause is upstream drift or the host, add a
     `--deselect` here with a one-line reason naming the upstream commit,
     record the new counts in this paragraph, and name it in the bump
     commit's body. A failure in anything we build or ship (engine, CMake,
     `strata_tokenizer.py`) stops the bump. Drop an exclusion once the new
     tag no longer needs it (run the excluded ids alone to check).
   - Re-audit upstream's top-level CMake assumptions (the table in
     [layout spec §3](superpowers/specs/2026-10-13-upstream-submodule-layout-design.md)):
     new `FetchContent`, changed option defaults, new options or targets, a
     `tests/CMakeLists.txt` that would flip `STRATA_BUILD_TESTS` on (and fetch
     Catch2), and that our root cache seeds (`STRATA_ENABLE_CUDA/HIP=OFF`,
     `STRATA_NATIVE_EXPERTS=OFF`) still land before the options they seed.
   - Re-check the engine's CLI flags and `--serve` protocol against
     `secure_frontend/src/engine_arguments.cpp` (deny list),
     `secure_frontend/src/engine_protocol_client.cpp` and
     [`security/engine-protocol-limits.md`](security/engine-protocol-limits.md)
     (`git -C upstream diff <old tag> <new tag> -- src/program/generate.cpp`),
     and the upstream internals `tools/secure/secure_tokenizer.py` couples to
     (listed in its module docstring).
4. `git -C upstream status --porcelain` is empty and `git submodule status`
   shows no `-`/`U` (`+` is expected until the pointer is staged). Worktrees
   share `.git/config`: a `git submodule deinit` in any worktree turns this
   into `-` everywhere; repair with `git submodule init` in the main checkout.
   Inspect the pointer with `git diff -- upstream` (a bare `upstream` is
   ambiguous with `upstream/...` branch names).
5. Update "Baseline" above (commit, tag object, tag), then find every other
   pin site:

       git grep -n '<old tag>\|<old short sha>\|<old tag object>' -- ':!docs/superpowers' ':!upstream'

   (escape the dots of `<old tag>`). Update each hit that states the
   **current** pin; leave verification-history rows (tables of past
   commands and results, dated sections) as they are. Known pin sites:
   `README.md` (the revision column and the pinned `blob/<tag>/...` links),
   the header comment of `.github/workflows/secure-frontend.yml`,
   [`AGENT_HANDOFF.md`](AGENT_HANDOFF.md) (owner decision 6, the "Repository
   layout" table header, "Current baseline"),
   [`secure-frontend-ci.md`](secure-frontend-ci.md) (the "Root build" note on
   `STRATA_BUILD_TESTS` and the offline-bundle commands, which carry the tag
   object), this file (the `[strata]` reference, the pinned `blob/<tag>/...`
   license links, the counts in step 3, and the ggml commit and
   `upstream/CMakeLists.txt` line numbers below — not found by the grep:
   re-derive them with `grep -n FetchContent_Declare -A3
   upstream/CMakeLists.txt`), and the `docs/security/` pages that name the
   baseline version. The SP5 M1 parity artifacts are line-number pin sites as
   well: `secure_frontend/tests/parity_keys_upstream.hpp` (its header comment
   carries the pin commit and tag, and its two grep commands must be re-run at
   the new pin), `secure_frontend/src/parity_table.cpp` (every row cites
   `serve/<file>.py:<line>` at the pin — re-pin each citation, then the
   parity meta-tests and the README mirror), and the `secure_frontend/README.md`
   parity section (regenerated from that table;
   `tools/secure/test_parity_readme_coverage.py` guards the mirror).
   Changing the version on a `docs/security/` page asserts
   its facts hold for the new tag: re-check each row against the new source
   first and add rows for new behavior.
   `upstream.version` (repository root, one line: the tag itself, today
   `v0.2.2`) is a pin site — bump it with the gitlink; configure fails
   when a reachable submodule tag disagrees with it. It is the source of the
   SP4 engine version `kEngineVersion` (`secure_frontend/CMakeLists.txt`); the
   `SECURE_STRATA_ENGINE_VERSION` cache override is for local experiments
   only and appears nowhere in `.github/workflows/secure-frontend.yml`.
6. `git add upstream docs/PROVENANCE.md` plus every file touched in step 5
   (at least `upstream.version`, `README.md`, `.github/workflows/secure-frontend.yml`,
   `docs/AGENT_HANDOFF.md`, `docs/secure-frontend-ci.md`); one commit:
   `upstream: bump to <new tag>`, whose body lists each gate with its counts
   (old → new), any exclusion added, and the upstream changes that matter to
   the frontend. The commit also carries the contract's review records
   (`docs/security/engine-reviews/<engine>-<old>-<new>.{json,md}` and the
   `-compiled-read.md` notes) and a row in the contract's Record table; the
   body cites their SHA-256 and the owner's sign-off (C9).

After a `git pull` that moves the gitlink, run `git submodule update --init`
(or set `git config submodule.recurse true` once). A `+` in front of the line
`git submodule status` prints means the checked-out `upstream/` commit differs
from the recorded gitlink (behind, ahead or elsewhere).

## Model catalog (SP4)

The `strata` binary embeds `secure_frontend/catalog/<name>/<tag>.json`
manifests, pinned by `secure_frontend/catalog/CATALOG.sha256`. **The v1
production catalog is empty** (SP4 D5 (D)): the Qwen `--pack` files have no
pinned digests until the owner's determinism check (D5 (A)) or a C++ pack
builder (D5 (C)). A catalog entry lands in the same commit as its pins here
(repository, revision, every file's size and SHA-256, re-verified by one
download on the dev machine); `tools/secure/test_catalog_provenance.py`
enforces that. The DFlash2 drafter (`Anbeeld/GLM-5.3-Flash-DFlash2-GGUF`,
CC BY-NC-ND 4.0) is never a catalog entry.

Developer-built bundles (D5 (B′), `tools/secure/make_bundle.py`) check the
downloaded shards against `tools/secure/qwen_bundle_pins.json`, read from the
Hugging Face tree API (`lfs.oid`, `lfs.size`) on 2026-10-14 at the revisions
upstream pins in `setup.py` `HF_REVISIONS`:

| Repository @ revision | File | Bytes | SHA-256 |
| --- | --- | --- | --- |
| `ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF` @ `ed59f92082b1e93c0e96d60a8b11aab089b52f09` | `IQ2_XS/…-IQ2_XS-00001-of-00002.gguf` | 39225954592 | `92cee27ae5bbadcd732416a0f7a7f0acc092399dbbe8f5a5efa707c2ec0a49d7` |
| same | `IQ2_XS/…-IQ2_XS-00002-of-00002.gguf` | 28800138432 | `316b46f3a2dbd68c900f43136ab9449f9dcc3725dfd8c794847c204bc161e113` |
| same | `IQ3_XXS/…-IQ3_XXS-00001-of-00002.gguf` | 47039860096 | `219ea929900dfa9ef091f3aa473fdba6874b65fcb36526d7d851ac9e95856d15` |
| same | `IQ3_XXS/…-IQ3_XXS-00002-of-00002.gguf` | 28800138432 | `316b46f3a2dbd68c900f43136ab9449f9dcc3725dfd8c794847c204bc161e113` |
| same | `IQ3_S/…-IQ3_S-00001-of-00002.gguf` | 54817524224 | `4c1eb2ceb4915e1192f4f386021897bde56a97f40a0bb78bb86465e0f7d2aca3` |
| same | `IQ3_S/…-IQ3_S-00002-of-00002.gguf` | 28800138432 | `316b46f3a2dbd68c900f43136ab9449f9dcc3725dfd8c794847c204bc161e113` |
| `ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-Coder-GGUF` @ `5348543e0147355ac9cbcb031184a3546350988e` | `IQ1_M/…-IQ1_M-00001-of-00002.gguf` | 29608446496 | `e11083ba855e7666b48ea3f2db6a9c3a20c18751a012cc24f948de91b7087fad` |
| same | `IQ1_M/…-IQ1_M-00002-of-00002.gguf` | 28800138432 | `316b46f3a2dbd68c900f43136ab9449f9dcc3725dfd8c794847c204bc161e113` |

`…` is `Qwen3.8-Flash-Next-GSQ-RCO`. These are Hub-reported digests, not yet
re-verified by a download here — **owner to confirm before any release**; `make_bundle.py` re-hashes every shard it uses.

## Security reference

[robertkarl/UnbiasedStrata][unbiased] at
`46c46ecfc11d2315512f21f7549b0a7792ccd2ae` (`main`, resolved 2026-10-11
Pacific time) is a design reference for a smaller, offline-verifiable runtime
and reduced service attack surface.  It is **not** imported as a code
baseline.  Any future adaptation must record the source, license, and exact
revision of copied material, and must preserve Windows and Linux compatibility
requirements.

License: the GitHub API (`gh api repos/robertkarl/UnbiasedStrata/license`,
queried 2026-10-13, default branch) returns SPDX id `MIT`, name `MIT License`.
No code was taken from this repository, so its license governs nothing in
this tree.

## GLM-5.3 compatibility reference

[lighttransport/Strata][lighttransport] branch `glm53f`, resolved at commit
`4894c9967f5754a2052d7b05f10a01b6071bb6e4` on 2026-10-12 Pacific time, is
the requested reference for GLM-5.3 compatibility work. It is not imported as
a code baseline. The `glm4` / `chatglm-bpe` pre-tokenizer regex in our wrapper
`tools/secure/secure_tokenizer.py` (which imports upstream's unmodified
`upstream/tools/strata_tokenizer.py`) was transcribed from that revision's
`tools/strata_tokenizer.py` (which identifies the upstream llama.cpp
`LLAMA_VOCAB_PRE_TYPE_CHATGLM4` behavior); no engine source or model artifact
was copied. It is covered by tiny local byte-BPE regression fixtures
(`tools/secure/test_glm_tokenizer.py`: three-digit numeric boundary, alias
behavior, control-token parsing, UTF-8 round trips) and by
`tools/secure/test_secure_tokenizer.py` (the pattern shipped in a pack's
`tokenizer.json`). This is portable Python code, but its runtime test still
requires the pinned `regex` dependency (`scripts/dev/requirements-dev.txt`).
The GLM prompt template and native engine remain separately reviewed work; the
tokenizer port does not make a model executable or claim complete frontend
tokenizer parity.

License: the GitHub API (`gh api repos/lighttransport/Strata/license`,
queried 2026-10-13, default branch) returns SPDX id `MIT`, name `MIT License`.
The `LICENSE` file on branch `glm53f` (via the contents API, same date) carries
the copyright line "Copyright (c) 2026 Niko1221 and the Strata contributors".

## Project Maya (second GLM-5.3 reference, planned for SP6)

[mw00/project-maya][maya] was named by the owner on 2026-10-14 as a second
source for the GLM-5.3 engine work, beside the lighttransport branch above.
Resolved on 2026-10-14 (`gh api repos/mw00/project-maya/commits/main`) at tag
`v1.1.8`, commit `5c970a863ce2fdd87b6e23ffb4822f6d76522b1c`. It is a Strata
derivative (its README: "Maya's engine started from Strata's and was rewritten
for GLM-5.3-Flash"), covering NVIDIA (1–16 GPUs) and AMD HIP, Linux and
experimental Windows (VS 2022 + CUDA 12.8), MTP decoding, an OpenAI- and
Anthropic-compatible server, and its own quantization tooling. Its
`docs/GLM5-FLASH.md` is a component-by-component port study against Strata.
**No code, kernel, model or quant has been taken from it yet.** When SP6 uses
it, this section records the exact files or contracts used, as the sections
above do, and the README sources table is updated in the same commit.

**Vendored 2026-10-22 (SP5 M2, owner decision D-10):** the pinned source is
now the git submodule `engines/maya-ref/` at gitlink
`5c970a863ce2fdd87b6e23ffb4822f6d76522b1c` (tag `v1.1.8`), a **read-only
reference**: the GLM parity table cites it by `file:line` so a reviewer can
open the exact read site locally. It is never compiled, linked, or configured
by any CMake preset (a pytest guard checks that no CMake file mentions it);
`secure_distribution_check.py` rejects any staged path with a `maya-ref`
component; `pytest.ini` keeps its own test tree out of collection. This
vendoring pulls forward only the reference-source piece of SP6 — no SP6
engine work happens in SP5, and nothing here comes out of this submodule
into our binaries. **SP5 M2 use:** the submodule is the citation target of
the GLM parity table (`kMayaRows`, 178 rows) and of the dialect/vision
citations across `secure_frontend` and the kit docs — the whole table is
verifiable by opening the cited `file:line` locally. Still: **no code,
kernel, model or quant has been taken from it**; SP6 import work keeps the
rules above.

**Bumped 2026-10-21 to tag `v1.1.21`** (owner request, on the owner's Windows
box): gitlink `451f9cb6a21ea65673bdde2e57b4a670ae2b8859`, tag object
`ba26442d5c8b833fe8be466277ca52b09bf1ec63` (124 commits; no file renamed or
deleted). The Maya citations across `secure_frontend` and the kit docs were
re-pinned to it. **Refreshed 2026-10-22 (SP5-P2, `sp5-p2-parity-refresh`):**
the GLM key inventory `parity_keys_maya.hpp` was REGENERATED at `v1.1.21`
(183 typed / 33 mapped / 50 rejected / 464 out-of-scope rows over the
`MAYA_*`/`DS4_*`/`STRATA_GLM_*` surface), the upstream table's citations
were refreshed at `v0.2.2`, and the meta-tests now enumerate the CURRENT
vendored trees and demand a disposition for every knob (the drift tripwire).
The changed GLM penalty semantics are closed: a penalty window of 0 means
NO penalty on the GLM engine (`src/program/generate.cpp:3467`), and the
client now renders that flavor rule (`render_sampling_keys`).
(`docs/validation/2026-10-21-windows-gpu-session.md`, issue 27, closed.) The
`maya_118`
dialect now also accepts the bounded `VRAM`/`NOTE` records `v1.1.21` prints
before READY. The tag's `VERSION` file is empty, so a built Maya server reports
no version; record the tag and commit instead.

**SP5 parity acceptance kit (placeholder, SP5 M2):** the kit procedure is
`docs/validation/sp5-parity-kit.md` (corpus: `docs/validation/sp5-parity-corpus.txt`);
the GPU run happens on the owner's box. When it runs, record here: the
release commit, both servers' full command lines, both engine versions, the
model pack SHA-256, and the SHA-256 of both results files and of the corpus.
**No GLM parity claim is made until this entry exists.**

License: the GitHub API (`gh api repos/mw00/project-maya`, queried 2026-10-14,
default branch) returns SPDX id `MIT`. At `v1.1.21` (read from the local tag,
2026-10-21) its `LICENSE` is still the MIT text and carries two copyright lines:
"Copyright (c) 2026 Peasant Smith and the Project Maya contributors" (new) and
"Copyright (c) 2026 Niko1221 and the Strata contributors". It bundles
`third_party/ggml` (MIT), `third_party/exllamav3` (MIT, Turboderp),
`third_party/beellama.cpp` (MIT), `LICENSE.deepseek-v41` (MIT, Copyright (c)
2023 DeepSeek; new at `v1.1.21`, covers `serve/ds41_encoding.py`) and the Outfit
font (SIL OFL 1.1, `serve/web/fonts/OFL.txt`; present at `v1.1.8` too but not
recorded here before); any SP6 import from those paths carries their notices.
Its optional DFlash2 drafter weights are CC BY-NC-ND 4.0 and are never
bundled or fetched by this project; a tree-wide search at `v1.1.21` finds no
other non-commercial or copyleft component.

**Code taken since (the multiengine section has the details):** 2026-10-25, Maya
`v1.1.27`'s GLM engine sources into `engine/third_party/project-maya` (now the raon1
library, with our raon1 changes); 2026-10-27, Maya **`v1.1.30`**'s whole engine tree,
unedited, into `engine/third_party/maya` (GLM-5.3-Flash, DeepSeek-V4.1-Flash and
MiniMax-M3.1-Flash). `v1.1.30` adds one licence: `LICENSE.minimax-m31`, the MiniMax
Community License (free for non-commercial use, conditions for commercial use), which
covers the MiniMax model and Maya's MiniMax chat template and fixtures (`tools/mm3`,
`serve`); none of those files is in our tree, and the engine code in `src/mm3` is
under Maya's MIT licence. The statements above that nothing was taken predate this.

## ggml / llama.cpp (bundled with upstream)

[ggml-org/llama.cpp][llamacpp] / ggml comes with the upstream baseline and is
unchanged by us (it is inside the `upstream/` submodule). It is present in two
forms:

- `upstream/third_party/ggml` — vendored headers;
  `upstream/third_party/ggml/VERSION.txt` records commit
  `3cf03257f219afbe7334045ff7c6a06ac68c627d`.
- The llama.cpp checkout fetched by `upstream/CMakeLists.txt`
  (`FetchContent_Declare(strata_llamacpp ...)`, lines 1564–1567) when
  `STRATA_NATIVE_EXPERTS=ON` and `STRATA_GGML_DIR` is unset, pinned to
  `GIT_TAG 3cf03257f219afbe7334045ff7c6a06ac68c627d`.

License: MIT, "Copyright (c) 2023-2026 The ggml authors"
([`third_party/ggml/LICENSE` at `v0.2.2`](https://github.com/Niko1221/Strata/blob/v0.2.2/third_party/ggml/LICENSE);
offline: `upstream/third_party/ggml/LICENSE`).
Our root `CMakeLists.txt` seeds `STRATA_NATIVE_EXPERTS=OFF` and our CI
(`.github/workflows/secure-frontend.yml`) configures with
`-DSTRATA_NATIVE_EXPERTS=OFF`, so the llama.cpp checkout is never fetched by
default or in CI.

## CPU expert lane: ggml tables/dots and upstream's pool design (ours, 2026-10-25)

The CPU expert lane computes a layer's missed MoE experts on the host CPU in parallel
with the GPU. Two kinds of borrowed material, both MIT, both reimplemented in our own
tree (nothing under `upstream/` is edited):

- **ggml i-quant tables and dots.** `engines/unified/src/u_cpu_iq_tables.inc` carries
  ggml's codebook and sign tables (the IQ2_S / IQ3_XXS / IQ3_S grids, the IQ2_XS
  ksigns/kmask, the IQ4_NL values, the Q2_XS even-sign table), copied verbatim — values
  unchanged, only the C++ types — from ggml's `ggml-common.h` and
  `ggml-cpu/arch/x86/quants.c` at the commit its file header records
  (`4615f346d9311dd1ce9f5738ab04446f023a6e42`). The scalar reference dots (`dot_ref`,
  `u_cpu_iq.cpp`) and the AVX2 dots (`detail::dot_avx2`, `u_cpu_iq_avx2.cpp`) follow
  ggml's `ggml_vec_dot_iq*_q8_K` / `_iq4_nl_q8_0` from the same `quants.c`. ggml is MIT,
  "Copyright (c) 2023-2026 The ggml authors" (see the ggml / llama.cpp section). The
  Strata-custom Q2_0 (type id 42) dot and its AVX2 form are ours. The tables were
  emitted by a session-only dev script (`gen_cpu_iq_tables.py`, not committed); the GPU
  path's `u_iq.cu` carries the same grids (`u_iq_qwen_tables.inc`) from the same source.

- **The pool + handshake structure.** `engines/unified/src/u_cpu_lane.*` (the worker
  pool as upstream's `run_split_multi`) and the device half — `expert_lane_plan`,
  `lane_doorbell`, `lane_merge` in `src/kernels/u_iq.cu` and `reg_lane_*` in
  `src/u_experts.cu` — are our reimplementation of upstream's expert pool and
  verify-window doorbell (`upstream/src/kernels/cpu/pool.cpp`,
  `upstream/src/core/verify.cpp` `Verifier::run`, and the doorbell / wait / copy-rows
  kernels in `upstream/src/kernels/cuda/elementwise.cu` and `verify_kernels.cu`). MIT,
  "Copyright (c) 2026 Niko1221 and the Strata contributors" (`upstream/LICENSE`).

## multiengine (`engine/`): upstream's engine at `v0.2.6` beside our family libraries (2026-10-27)

`engine/` is the source of our engine executable, `multiengine.exe`. Since 2026-10-27 it is a launcher
(`engine/src/multiengine/launcher.cpp`, ours) over one shared library per model family, each compiled from its own
sources (owner: "it should be in its own library so they don't collide"):

- **`multiengine_strata.dll` - upstream Strata's engine.** `engine/` carries upstream [Niko1221/Strata][strata]'s
  engine tree (`src`, `include`, `tests`, `cmake`, `data`, `third_party/ggml`, `third_party/ggml-tq`, the root
  `CMakeLists.txt` and the bench files the build reads) at tag **`v0.2.6`**: commit
  `4b9e05a2a806a3fe24f7b2560d98626c37dade35`, tag object `ff91dd77a13857ccc2f1bd67edc9a8ecd4ecdfa4`, engine version
  0.1.51. Every such file is upstream's file as upstream ships it (line endings aside), except the root
  `CMakeLists.txt`, which ends with our one `include(cmake/multiengine.cmake)`. The library is upstream's `strata`
  target's program (`src/program/generate.cpp`, not edited) compiled with that target's libraries, definitions and
  flags, its `main` renamed and exported (`engine/src/multiengine/strata_family.cpp`); upstream's own `strata.exe`
  is still built beside it. The tree is written by `engine/src/multiengine/sync_upstream.py` from a local clone's git
  objects (`G:\Strata`; its working tree is not touched). MIT, "Copyright (c) 2026 Niko1221 and the Strata
  contributors". Before 2026-10-27 `engine/` held upstream `v0.2.4.2` (`dfa3f402`, engine 0.1.49.2) with our edits in
  13 of its files (Gemma 4's generalisations, the GLM and Gemma 4 family switches, a crash filter); those edits now
  live in the family libraries and the launcher, and the upstream files are upstream's again.
- **`multiengine_maya.dll`, `multiengine_maya_ds4.dll`, `multiengine_maya_mm3.dll` - Project Maya's three programs**
  (`maya`: GLM-5.3-Flash; `maya-ds4`: DeepSeek-V4.1-Flash; `maya-mm3`: MiniMax-M3.1-Flash), since 2026-10-27.
  `engine/third_party/maya` carries [mw00/project-maya][maya]'s engine tree (`src`, `include`, `cmake`, `tests`,
  `third_party/ggml`, the root `CMakeLists.txt`, `VERSION`, `LICENSE` and the `LICENSE` files of
  `third_party/exllamav3` and `third_party/beellama.cpp`; since the later 2026-10-27 sync also Maya's three shipped
  expert profiles, `data/expert-profile-{glm,ds4,mm3}.bin`) at tag **`v1.1.30`**: commit
  `8621e33159f426da2d376be4ec012b0ce8fe2948`, tag object `70137ee561c2d22bd1f299c90110c7e62d6d1144` (fetched into
  `G:\Project-maya` on 2026-10-27 at the owner's word). Every file is Maya's as Maya ships it (line endings aside; the
  configure-time `mmq-fused-swiglu.patch` byte for byte, as Maya's `.gitattributes` keeps it), none edited, the
  `CMakeLists.txt` included; `engine/src/multiengine/sync_maya.py` writes the tree from the clone's git objects. The
  tree is built as a CMake project of its own (`engine/cmake/multiengine.cmake`, `ExternalProject`), with the options
  Maya's own `maya.py` passes; our `engine/cmake/multiengine_maya.cmake`, read inside that build as
  `CMAKE_PROJECT_strata_INCLUDE`, adds the three libraries in Maya's root directory, each compiled from its program's
  own sources with that program's libraries, definitions and flags (all 26 compile lines token for token the
  program's own, checked on the generated build) and its `main` renamed and exported
  (`engine/src/multiengine/maya_family.cpp`, `maya_ds4_family.cpp`, `maya_mm3_family.cpp`). MIT, "Copyright (c) 2026
  Peasant Smith and the Project Maya contributors" and "Copyright (c) 2026 Niko1221 and the Strata contributors";
  the EXL3 kernels carry exllamav3's MIT notice (turboderp), the copied `third_party/*/LICENSE` files travel with them.
  The models these programs run are the owner's choice and come under their own licences, none of them in this tree:
  GLM-5.3-Flash (MIT, zai-org), DeepSeek-V4.1-Flash (MIT, DeepSeek) and MiniMax-M3.1-Flash (the **MiniMax Community
  License**: free for non-commercial use, conditions for commercial use; Maya's `LICENSE.minimax-m31`, not copied,
  since no file it covers - Maya's `tools/mm3` and `serve` - is in our tree).
- **`multiengine_raon1.dll` - raon1 on our port of Maya's GLM engine,** vendored under `engine/third_party/project-maya`
  with its own CMake (MIT; see the Project Maya section): Maya `v1.1.27`'s GLM sources (110 of its 135 files are
  `v1.1.27`'s as Maya ships them) with the raon1 changes in 19 of them and 6 files of ours (`glm_driver.cpp`, the
  raon1 geometry and kernels, `NOTICE.md`). Until 2026-10-27 it was `multiengine_glm.dll` and ran GLM-5.3-Flash too;
  the launcher now gives it the raon1 packs only. It compiles only its own headers.
- **`multiengine_flint.dll` - our flint engine** (its own section below), on Gemma 4's GGUF reader and core library.
- **`multiengine_gemma4.dll` - our Gemma 4 engine** (next section) on its own copy of the Strata core,
  `engine/src/gemma4/core`: the 37 translation units and their headers it links, copied from `engine/` at upstream
  `v0.2.4.2` plus Gemma 4's generalisations (`core/VENDORED.txt` lists every file with its SHA-256), not edited
  after the copy.
- The raon1 and Gemma 4 libraries keep the engine version they were validated with (`STRATA_VERSION` 0.1.49.2,
  `cmake/multiengine.cmake`): the raon1 library keys its calibration cache and saved sessions on it. Maya's libraries
  carry Maya's own (`0.1.27`, its `project()` version), as Maya's executables do.
- The libraries share only the pinned ggml (`third_party/ggml/VERSION.txt`, `3cf03257`, unchanged in `v0.2.6` and the
  same pin in Maya `v1.1.30`; one llama.cpp source copy, `G:\Strata\third_party\llama.cpp`, builds both trees; see the
  ggml / llama.cpp section) and NVIDIA's CUDA toolkit.

**Supply-chain read of `v0.2.4.2..v0.2.6`** over that scope (2026-10-27, by this session; not the C9 contract review,
which governs the `upstream/` submodule): 51 files, +2,479 / -109, no deletions or renames. Built code: the
AVX-512 i-quant rows load their grid words by gathers where `cpu_gather_fast()` says gathers are cheap
(`iq_avx512.cpp`), a one-token 512-bit exact row path that checks its bits against ggml-cpu's dot at first use
(`native_expert.cpp`), the adaptive tier's measured per-session asynchronous choice (`adapt_async_default.hpp`, new;
its use in `generate.cpp`), and the version bump in `CMakeLists.txt`. The DeepSeek path (`src/dsv41`, built only with
`STRATA_BUILD_DSV41`, off) and the desktop app's Python tests changed too. Every added line was searched for network,
process, shell, dynamic-loading, file-writing and file-deletion calls: none. The `upstream/` submodule is not part of
this change; its gitlink is `dfa3f402` (`v0.2.4.2`), which the Baseline section above, still naming `v0.2.2`,
predates.

**Supply-chain read of Project Maya `v1.1.30`'s copied scope** (2026-10-27, by this session; the whole 411-file
scope, not only the `v1.1.27..v1.1.30` diff, since the DeepSeek and MiniMax programs are new to our build). Between
the tags the scope changed in 58 files (+19,659 / -409; the new `src/mm3`, MiniMax-M3.1-Flash's engine, is most of
it). Every source and header was searched for network, process, shell, dynamic-loading, file-writing and
file-deletion calls. Network, process and shell: none (no socket, connect, listen, `system`, `popen`,
`CreateProcess`, `ShellExecute` or exec call; the `send` in `ds4_model.cu` is a local lambda that rings the helper
GPU's doorbell, and `remote_experts.cpp` is a helper GPU's expert cache, not a remote host). Dynamic loading: system
libraries by name only (`advapi32.dll` for `RegGetValueW`, `dxgi.dll` for `CreateDXGIFactory1`, `dbghelp.dll` for
`MiniDumpWriteDump`, `kernel32` functions by `GetProcAddress`), as in `v1.1.27`. File writes: the conversation slots
(`slot-*` files in the pack's `slots` folder, or `STRATA_GLM_SLOT_DIR`; DeepSeek's in its own slot folder), the expert
profile `--expert-profile-save` names, the calibration and lane-time caches beside the pack, trace and dump files only
when an environment variable names them, and on a decode stall a minidump `maya-stall-<pid>.dmp` in the working
directory. File deletions: only those files and their `.tmp` siblings (a slot directory's scan removes only names
starting `slot-`). Nothing here is downloaded at build time: the configure step builds ggml from the local llama.cpp
copy (`STRATA_GGML_DIR`), and Maya's FetchContent fallback is never taken.

## flint engine (ours, 2026-10-27) and its test-only oracle

- `engine/src/flint/` (the `multiengine_flint` family library of `multiengine.exe`; design
  `docs/superpowers/specs/2026-10-27-flint-engine-design.md`) is our own code under this repository's licence, built as
  Gemma 4's engine is: `f_cli`, `f_profile`, `f_lane`, `f_engine.cu`, `f_run.cpp` and the generic half of `f_kernels.cu`
  are adapted copies of our Gemma 4 sources (`engine/src/gemma4`), and it compiles Gemma 4's unchanged `g4_gguf.cpp` /
  `g4_file.cpp` and links Gemma 4's frozen core library `g4core` (the Strata core copy described in the multiengine
  section; MIT, "Copyright (c) 2026 Niko1221 and the Strata contributors"). It compiles nothing from `upstream/`.
- The model's arithmetic follows llama.cpp master's flint graph, read at `e86f5fd1e9f584b0fcec71d7da9a92de44e8bcbf`
  (2026-10-27; `src/models/flint.cpp`, the window rule in `src/llama-hparams.h`, the sinks in
  `ggml/src/ggml-cpu/ops.cpp`'s soft_max, NeoX RoPE as `ggml-cuda/rope.cu` computes its angle; MIT, "Copyright (c)
  2023-2026 The ggml authors"). Nothing of it is copied into `engine/src/flint`: the attention-with-sinks, QK-norm/RoPE,
  sigmoid-router and SwiGLU kernels are written for this engine (Gemma 4's split-K attention and router as their
  base).
- **Test-only oracle (never built by default, never shipped):** `engine/tests/flint/oracle/` - `flint_oracle.cpp`,
  `flint_seams.cpp` (adapted from this session's raon1 oracle programs) and `flint_tokenize.cpp`, ours, linking the
  llama.cpp master checkout above, which the owner approved downloading (2026-10-27, a shallow clone kept outside this
  repository at `G:\Strata-data\flint-llama-ref`); `f_kl.py` and `f_seams_cmp.py` compare logit and seam files.
- **The frontend's flint profile** (`secure_frontend`: `TokenizerMode::flint_bpe` in `src/tokenizer_pack.cpp`,
  `src/flint_prompt_renderer.cpp`, the flint plan in `src/chat_generation_plan.cpp`) is our own code. The pre-tokenizer
  reproduces the behaviour of llama.cpp master `e86f5fd1`'s `unicode_regex_split` (`src/unicode.cpp`) over
  `LLAMA_VOCAB_PRE_TYPE_FLINT`'s three regexes (`src/llama-vocab.cpp`), written from reading that code, not copied
  (MIT, "Copyright (c) 2023-2026 The ggml authors"). Its Unicode tables are the generated
  `src/raon_unicode_data.inc` (now with every category bit), whose source, KRAFTON's `src/unicode-data.cpp`, is
  byte-identical to llama.cpp master's. The renderer follows the GGUF's own `tokenizer.chat_template` (RecursiveSI's
  template, read from the owner's file). `tools/secure/secure_tokenizer.py`'s `FLINT_PATTERNS` are the three regex
  strings from `llama-vocab.cpp`, recorded in the pack's `tokenizer.json` as data.
- `secure_frontend/tests/fixtures/flint_tokenizer_texts.json` holds texts we wrote (plus an 8,000-character slice of
  `docs/AGENT_HANDOFF.md`) and the token ids llama.cpp master gives them on the owner's file's vocabulary
  (`flint_tokenize ... plain`), and chats with the prompt llama.cpp's Jinja engine renders from the GGUF's template and
  its ids (`flint_chat`); ids and texts only, no vocabulary. `engine/tests/flint/oracle/flint_chat.cpp` (ours,
  test-only) links llama.cpp's `llama-common` library of the same checkout.
- The model file is the owner's choice and nothing of it is in our tree: `F:\models\flint-Q8_0\flint-Q8_0.gguf`
  (unsloth's Q8_0 of RecursiveSI's "Flint Preview"). Its GGUF declares `general.license = other`,
  `general.license.name = flint-preview`; RecursiveSI's model page (fetched 2026-10-27) names the Apache 2.0 license.
  The two disagree; the owner decides which applies to the file in use (the 2026-10-25 notes' "Apache-2.0" was taken
  from the page alone).

## Gemma 4 engine (ours, 2026-10-22) and its test-only reference tool

- `engine/src/gemma4/` (the `multiengine_gemma4` family library of `multiengine.exe`; until 2026-10-26 the
  standalone `engines/gemma4/`, `strata-gemma4-engine`, Gemma 4 plans 2-3) is our own code under this
  repository's licence, written for the owner's Gemma 4 26B-A4B file (spec
  `docs/superpowers/specs/2026-10-22-gemma4-engine-design.md`). It compiles nothing from
  `upstream/`. Since 2026-10-26 (design
  `docs/superpowers/specs/2026-10-26-gemma4-paper-tiering-design.md`) it runs on the Strata core's own pieces, and
  since 2026-10-27 on its own copy of them (`engine/src/gemma4/core`, upstream `v0.2.4.2`'s files plus Gemma 4's
  generalisations; MIT, see the multiengine section), so the ggml-cpu dot products and llama.cpp's MMQ it runs are
  built from the pinned llama.cpp (see the ggml / llama.cpp section). At build and run time it uses NVIDIA's CUDA
  13.0 toolkit from the box (the static CUDA runtime and `cublas64_13.dll`, the toolkit's own licence), as
  upstream's CUDA engine does.
- The model weights it loads are the owner's choice (spec §7); nothing from the GGUF is committed
  (the validation files under `docs/validation/gemma4/` hold token ids, statistics and timings).
- `engine/tests/gemma4/g4_llama_dump.cpp` (formerly `engines/gemma4/tools/`) is a TEST-ONLY tool, never built by default and never
  shipped: a copy of upstream's `tools/qwen35/q36_llama_dump.cpp` (at `v0.2.2`; MIT, "Copyright
  (c) 2026 Niko1221 and the Strata contributors", `upstream/LICENSE`) with the changes marked
  `gemma4:`. It links the static libraries of the test-only llama.cpp build at
  `3cf03257` (session log step 17; MIT, see the ggml / llama.cpp section) through
  `engine/tests/gemma4/build-g4-llama-dump.bat`. Spec D7: built at the existing pin, run only
  on the box for parity.

## raon-1-mini's tokenizer in secure_frontend (2026-10-26): KRAFTON's llama.cpp

raon-1-mini's reference tokenizer is KRAFTON's fork of llama.cpp,
[krafton-ai/llama.cpp](https://github.com/krafton-ai/llama.cpp/tree/fb895662d25868756a19253ca5e0688f99b18994)
branch `raon-1-mini-preview`, commit `fb895662d25868756a19253ca5e0688f99b18994` (MIT, "Copyright (c) 2023-2026 The
ggml authors", as llama.cpp).

- `secure_frontend/src/raon_unicode_data.inc` is GENERATED from that commit's `src/unicode-data.cpp` by
  `secure_frontend/tools/generate_raon_unicode.py`: llama.cpp's category flags (reduced to number / letter / accent
  mark), its whitespace set, and the canonical combining classes, decompositions and compositions. Unicode data in
  a derived, reduced form; the generator names its source in the file's header.
- `secure_frontend/src/tokenizer_pack.cpp`'s raon_bpe functions (`raon_code_points`, `raon_nfc_points` and its
  helpers, `raon_split`, `raon_encode_piece`) are ports of that commit's `unicode_cpts_from_utf8`,
  `unicode_cpts_normalize_nfc` (with `unicode_canonical_*`), `unicode_regex_split_custom_raon` and
  `llm_tokenizer_bpe_session::tokenize`, written for the frontend's types; each names its original.
- `secure_frontend/tests/fixtures/raon1_tokenizer_texts.json` holds texts and the ids that commit's `llama_tokenize`
  gave them (no model data). The model's vocabulary and merges are never committed.

## DeepSeek-V4.1 in secure_frontend (2026-10-27): Maya's tokenizer and DeepSeek's encoder

The references are Project Maya `v1.1.30` (MIT; read from `G:\Project-maya`'s git objects, never checked out) and the
DeepSeek encoder Maya vendors (`serve/ds41_encoding.py`, MIT, "Copyright (c) 2023 DeepSeek", Maya's
`LICENSE.deepseek-v41`).

- `secure_frontend/src/regex_unicode_data.inc` is GENERATED by `secure_frontend/tools/generate_regex_unicode.py`, which
  asks Python's `regex` module (2026.9.10, Apache-2.0; the one Maya's server tokenizes with, installed unpinned by Maya's
  setup) which code points `\p{L}`, `\p{M}`, `\p{N}`, `\p{P}`, `\p{S}`, the letter subcategories and `\s` match: Unicode
  Character Database properties (Unicode License v3) in a derived, reduced form; the file's header names the module.
- `tokenizer_pack.cpp`'s `deepseek_split` transcribes Maya's `tools/strata_tokenizer.py` `DEEPSEEK3_PATTERNS` and
  `_IsolatedSplits` for the frontend's types. `tests/fixtures/maya_pretokenize_texts.json` holds our texts and the pieces
  Maya's own functions gave them (`secure_frontend/tools/generate_maya_pretokenize_fixture.py`, run on a byte-exact copy
  of Maya's tools folder that is never committed).
- `tokenizer_pack.cpp`'s `qwen_shape_split` (2026-10-27) transcribes two patterns for the frontend's types:
  `QWEN35_PATTERN` from upstream's own `upstream/tools/strata_tokenizer.py` (v0.2.6; byte_bpe, the Qwen packs) and
  `GLM_PATTERN` with its `ignore_merges` rule from Maya v1.1.30's `tools/strata_tokenizer.py` (glm_bpe, GLM-5.3); both
  files say they transcribed them from llama.cpp's `llama-vocab.cpp`. No code is copied. The same fixture now also holds
  the pieces upstream's and Maya's patterns gave our texts (`qwen35`, `glm4`; the generator loads upstream's module in
  place, writing no bytecode there). `tests/fixtures/byte_bpe_reference_ids.json` holds our texts (those of
  `flint_tokenizer_texts.json` and four of our own) and the ids upstream's and Maya's tokenizers gave them from the
  owner's Qwen3.6-35B-A3B GGUF and GLM-5.3-Flash pack (`secure_frontend/tools/generate_byte_bpe_reference_fixture.py`):
  ids only, no vocabulary or model data.
- `secure_frontend/src/deepseek_prompt_renderer.cpp` and `kDeepseekMarkers` reproduce the prompt format of DeepSeek's
  encoder for plain text chats: its marker literals and its effort line's text; no code is copied. The test prompts in
  `tests/deepseek_profile_test.cpp` are that encoder's output for our messages.
- The DeepSeek catalog entries come from Maya's `tools/ds4/download_gguf.json` (the pinned `engines/maya-ref` v1.1.27;
  MIT), read as data by `tools/secure/download_catalog.py`. The model files are never committed.

## MiniMax-M3.1-Flash in secure_frontend (2026-10-28): the MiniMax Community License

Added on the owner's decision of 2026-10-28 ("add MiniMax-M3.1-Flash under the MiniMax Community License"). The
references are Project Maya `v1.1.30` (MIT; read from `G:\Project-maya`'s git objects, never checked out) and MiniMax's
own chat template, which Maya ships with one addition.

- `engine/third_party/maya/LICENSE.minimax-m31`, `tools/mm3/chat_template.jinja` and `tools/mm3/download_gguf.json` are
  Maya v1.1.30's files, byte for byte (`engine/src/multiengine/sync_maya.py`; `.gitattributes` keeps them `-text`):
  - `LICENSE.minimax-m31` (sha256 `8c4978e3e0fe0349ff929cc8450463cba3039c4c9fc144d8bd5382ab00df247a`): the MiniMax
    Community License, "Copyright (c) 2026 MiniMax". Use, copying and redistribution for non-commercial purposes with the
    notice kept; commercial use needs "Built with MiniMax M3.1-Flash" shown and a one-time notice to MiniMax (written
    authorization above USD 20 million a year in revenue); its appendix lists prohibited uses. It covers the template
    text below and the model; this repository's own code keeps its own licence.
  - `chat_template.jinja` (sha256 `726bb6b3dcfa107d918a8a4a5f646990678e8562875f7ee633dcd3e064b428ef`): MiniMax's template
    (MiniMaxAI/MiniMax-M3.1-Flash at revision `54e4e69`) with Maya's `thinking_mode` "disabled"; every MiniMax pack copies
    it (`strata-prepare pack --maya-mm3 --template`), and the install ships it beside the engine.
  - `download_gguf.json` (sha256 `b6d126a7b60638195ad6ed40125c696f13c4a8b1d1231df89c04af4469c8a207`): Maya's MiniMax GGUF
    quants (Maya-XS, Maya-S; peasantsmith/MiniMax-M3.1-Flash-Maya-GGUF at a pinned commit, every file's size and sha256),
    read as data by `tools/secure/download_catalog.py`. The model files are never committed.
- `secure_frontend/src/minimax_prompt_renderer.cpp` writes the template's own text verbatim (its default system text,
  the thinking instructions and modes, the model id line, the developer default), the model having been trained on it;
  no code is copied. `tests/fixtures/minimax_prompts.json` holds our messages and what Maya's own `ChatTemplate`,
  `effort_kwargs`, `mark_literals` and `unmark_literals` (`serve/frontend.py`, MIT) gave for them over that template
  (`secure_frontend/tools/generate_minimax_prompt_fixture.py`, run on a byte-exact copy of Maya's `serve` folder that is
  never committed); the template's sha256 is in the file.
- `tokenizer_pack.cpp`'s `minimax_split` transcribes Maya's `tools/strata_tokenizer.py` `MINIMAX_PATTERN` (MiniMax's
  tokenizer.json split, GPT-4o's o200k regex) for the frontend's types, after the NFC raon-1-mini's port already
  computes; `maya_pretokenize_texts.json`'s `minimax` pieces (already generated with Maya's own functions) are the check.
- `engine/src/prepare/prepare_main.cpp`'s MiniMax `download --maya` step follows Maya's `maya.py` `mm3_pack_step`
  (its `need` list, stamp_pack, the reuse rule) over the existing port of `tools/mm3/pack_gguf.py`; no code is copied.

## GUI-first start-up (2026-10-27): the out folder, calibration, email alerts

Spec `docs/superpowers/specs/2026-10-27-gui-first-install-design.md`. All of it is our own code under this
repository's licence; nothing is copied from `upstream/` or Project Maya.

- **The out folder** (`make.py`, `make.bat`, `make.sh`) copies the built binaries and, unless `--no-cuda-runtime`,
  NVIDIA's cuBLAS libraries from the CUDA toolkit on the build machine (`cublas64_*.dll` and `cublasLt64_*.dll`, or
  `libcublas.so.*` and `libcublasLt.so.*`), unmodified, into `engine/`. They are redistributed under the NVIDIA CUDA
  Toolkit End User License Agreement, whose Attachment A lists them as redistributable. The folder's `README.txt`
  says so, and they remain NVIDIA's. Nothing of the toolkit is committed. On Windows, CMake's
  `InstallRequiredSystemLibraries` also puts the Microsoft Visual C++ runtime DLLs (`msvcp140*.dll`,
  `vcruntime140*.dll`, `concrt140.dll`) beside each program: Microsoft's app-local deployment of the Visual C++
  Redistributable files, which the Visual Studio licence allows to be distributed with an application, copied
  unmodified from the build machine's Visual Studio.
- **Calibration** (`secure_frontend/src/calibration.cpp`) follows the method of upstream's `tools/calibrate.py` and
  Maya's `calibrate_glm.py`: a fixed greedy workload, the median decode tok/s, a 3% keep margin, and confirmation
  against the defaults. It shares the method only; no code or data from either is copied. The swept flags and values
  are our engines' own (`engine_arguments.cpp`).
- **Email alerts** (only with `SECURE_FRONTEND_EMAIL_ALERTS`, off by default): the SMTP client
  (`smtp_client.cpp`, RFC 5321 with STARTTLS per RFC 3207, AUTH per RFC 4954 with PLAIN per RFC 4616 or LOGIN) is
  ours. TLS comes from the operating system, never from this repository: on Windows SChannel through SSPI
  (`secur32`, `crypt32`; part of Windows); elsewhere the system's OpenSSL (`libssl`/`libcrypto`, Apache-2.0 for
  OpenSSL 3), found by CMake's `FindOpenSSL` and linked dynamically. Neither is vendored, and the out folder bundles
  neither. The IONOS settings the page fills in (`smtp.ionos.com`, 465 SSL/TLS or 587 STARTTLS, the full mailbox
  address as the user name) come from IONOS's help pages, read 2026-10-27.

## Web search (2026-10-27): upstream's MCP client and Qwen tool form, Maya's GLM tool form

Spec `docs/superpowers/specs/2026-10-27-web-search-design.md`; built only with `SECURE_FRONTEND_WEB_SEARCH` (the
format code is in every build). The references are upstream Strata as the `upstream/` submodule holds it (`v0.2.4.2`,
MIT, "Copyright (c) 2026 Niko1221 and the Strata contributors") and Project Maya `v1.1.30` (MIT, "Copyright (c) 2026
Peasant Smith and the Project Maya contributors"; read from `G:\Project-maya`'s git objects, never checked out).

- **`strata-search`** (`search_mcp_server.cpp`, `web_search.cpp`, `search_main.cpp`, `search_url.cpp`) is ours: the MCP
  server side, the HTTP/1.1 client and the result list. It speaks SearXNG's JSON API (`/search?format=json`) to an
  instance the operator runs; SearXNG (AGPL-3.0) is neither bundled nor linked. TLS is the operating system's, as for the
  email alerts (`smtp_tls.cpp`).
- **The MCP client** (`mcp_client.cpp`) transcribes the behaviour of upstream's `serve/mcp.py` client (the same file in
  Maya): its messages, its limits, `result_text` and the truncation note's wording, kept verbatim. No code is copied.
- **Qwen tool calls**: `render_qwen_tool_prompt`'s fixed text (the tools header, the call-format instructions with their
  `<IMPORTANT>` reminder) is the tool section of Qwen's chat template as the Qwen3.6 GGUF and Qwen3.8 ship it (Apache-2.0,
  Alibaba's Qwen team; the Qwen3.6 GGUF's carries Unsloth's fixes), kept verbatim because the model was trained on it.
  `qwen_tool_calls.cpp` ports upstream's `serve/frontend.py` (`OutputParser`'s call handling, `parse_tool_call`,
  `param_end`, `call_end`, `function_end`, `_track`) to C++ for our two channels.
- **GLM-5.3 tool calls**: `render_glm53_tool_prompt`'s fixed text is the tool section of GLM-5.3-Flash's chat template
  (MIT, zai-org), verbatim. `glm_tool_calls.cpp` ports Maya's `serve/frontend.py` (`parse_tool_call`'s GLM and Qwen
  forms, `call_name`, the call handling of `OutputParser` with its streaming scan's name rule).
- **Fixtures** (`tests/fixtures/qwen_tool_prompts.json`, `qwen_tool_calls.json`, `glm_tool_prompts.json`,
  `glm_tool_calls.json`) hold our inputs and what upstream's and Maya's own code gave for them
  (`secure_frontend/tools/generate_{qwen,glm}_tool_{prompt,call}_fixture.py`, run on upstream's submodule and on a
  byte-exact copy of Maya's `serve` folder that is never committed). The prompt fixtures embed the chat templates' tool
  text as rendered; the templates themselves are not committed (their SHA-256 is in each file). No model file is.
- **raon1 and flint tool calls (2026-10-28)**: `render_raon1_tool_prompt` writes the tool section of raon-1-mini's chat
  template (KRAFTON's, from its GGUF), which is Qwen's tool text above, verbatim, reusing it; `render_flint_tool_prompt`
  writes the tool section of flint's template (RecursiveSI's, from its GGUF; Apache 2.0 per its model page), verbatim.
  Their calls are read with the Qwen and GLM ports above. `tests/fixtures/raon1_flint_tool_prompts.json` holds our inputs
  and what upstream's `ChatTemplate` (`serve/frontend.py`) rendered from each template
  (`secure_frontend/tools/generate_raon1_flint_tool_prompt_fixture.py`, which adds the `{% generation %}` tag raon1's
  template uses); the templates are not committed (their SHA-256 is in the file).
- **Gemma 4 tool calls (2026-10-28)**: `render_gemma4_tool_prompt` writes what the tool macros of Gemma 4's chat template
  (Google's, from the UD-Q8_K_XL GGUF; its `general.license` apache-2.0 with Google's Gemma 4 terms linked) write -
  `format_function_declaration`, `format_parameters`, `format_argument` and the call and response framing - ported to
  C++ and checked byte for byte. The message path is llama.cpp's at the pinned `3cf03257` (MIT, "Copyright (c)
  2023-2026 The ggml authors"): `common/parsers/gemma4.cpp`'s `convert_tool_responses_gemma4` (folding a round's calls
  and results into one model turn) and its "<|turn>model" rule, ported; `gemma4_tool_calls.cpp` reads calls by that
  file's PEG rules and `common/chat-peg-parser.cpp`'s `gemma4_to_json`, reimplemented by hand; the `<|tool_response>`
  stop follows `src/llama-vocab.cpp`'s Gemma 4 end-of-generation list. `tests/gemma4_tool_calls_test.cpp` carries the
  replies of `tests/test-chat.cpp`'s Gemma 4 block with the calls llama.cpp expects for them.
  `tests/fixtures/gemma4_tool_prompts.json` holds our inputs and what upstream's `ChatTemplate` rendered from the GGUF's
  template over that path (`secure_frontend/tools/generate_gemma4_tool_prompt_fixture.py`); the template is not
  committed (its SHA-256 is in the file).

## Pictures on model cards (2026-10-28): upstream's strata-vision, built as it is

Spec `docs/superpowers/specs/2026-10-28-vision-for-cards-design.md`.

- **The encoder** is upstream Strata's `tools/vision` (`strata_vision.cpp`, its CMake file; the `upstream/` submodule at
  `v0.2.4.2`, MIT, "Copyright (c) 2026 Niko1221 and the Strata contributors"), built out of source by `make.py` and
  never changed, over llama.cpp's `mtmd` library at the engine's pinned checkout `3cf03257` (MIT, "Copyright (c)
  2023-2026 The ggml authors"). The out folder ships the program as `engine/strata-vision(.exe)`.
- **The server side is ours**: the card rows (`vision.mmproj`, `vision.gpu`), the Pictures option, the per-card
  encoder in `model_runtime.cpp`, the download's `mmproj` file and the cards that name it. The encoder's command line
  follows upstream's `serve/server.py` reader arguments (`--mmproj`, `--model`, `--gpu`, `--threads`,
  `--max-tokens`, `--flash-attn`), and the catalog's encoder pins (repository, revision, size, SHA-256) are upstream's
  `setup.py` pins (`mmproj_hf`, `MMPROJ`) as the catalog already carried them.
- **The picture encoder files** (`mmproj-*.gguf`) are downloaded from their model repositories at those pins, never
  in the tree; they keep their models' licences.

## Several GPUs (2026-10-28): upstream's and Maya's rules, ours to compose

Spec `docs/superpowers/specs/2026-10-28-multi-gpu-design.md`. Behaviour followed, no code copied: upstream Strata's
`serve/server.py` (`gpu_list`, `engine_args`' layer split, `Vision.token_limit`, #1322's GPU-encoder reserve) and
`setup.py`'s `VISION` table (the `upstream/` submodule, `v0.2.4.2`, MIT); Project Maya's `maya.py` `write_config_ds4`
(`--tp min(2, GPUs)`, `--threads on` on one GPU) and its GLM engine's default layer split (`glm_model.cu`
`load_pack_place`; MIT). The GPU list is read with NVIDIA's own `nvidia-smi`, as the Monitor reads the GPU.

## strata-prepare (ours, 2026-10-27): upstream's pack tools as a native program

`engine/src/prepare/` (spec `docs/superpowers/specs/2026-10-27-strata-prepare-design.md`) is our C++ under this
repository's licence, a port of upstream Strata's `tools/iq_pack.py` and `tools/gguf_reader.py` (tag `v0.2.6`; MIT,
"Copyright (c) 2026 Niko1221 and the Strata contributors"). Their function names and messages are kept so the two
read side by side.

- **Text kept verbatim for byte parity:** the files it writes keep upstream's formats, including upstream's own text
  ("generated by tools/iq_pack.py" in `index.txt`, `"tool": "tools/iq_pack.py"` in `conversions.json`), so every
  file is byte-identical to the Python tool's output.
- **Dequantizing (`--compat-bf16`):** it links ggml's own dequantizers (`ggml-base`, the pinned llama.cpp; MIT), the
  C code gguf-py's numpy versions are tested against.
- **F16 rounding:** `f16_bits` ports NumPy's `npy_floatbits_to_halfbits` (round to nearest, ties to even;
  `numpy/_core/src/npymath/halffloat.cpp`, BSD 3-Clause, "Copyright (c) 2005-2024, NumPy Developers"), so values
  narrow exactly as `astype(float16)` does.
- **SHA-256:** a plain FIPS 180-4 implementation, written here.
- **Proof:** the parity check `tools/secure/strata_prepare_parity.py` (dev-only; runs upstream's `iq_pack.py` with
  numpy and llama.cpp's gguf-py) found every file identical on synthetic models and on the owner's
  Qwen3.8-Flash-Next IQ3_S.
- **The out folder** carries upstream's `data/expert-profile.bin` and `data/expert-profile-coder.bin` (MIT, unmodified)
  in `engine/profiles/`, as the cards name them.
- **Project Maya's packers** (added 2026-10-27): `maya_pack.cpp`, `tokenizer_export.cpp` and `maya_moe_pack.cpp` port
  [mw00/project-maya][maya]'s `tools/iq_pack.py` (Maya's fork), `tools/strata_tokenizer.py`, `tools/ds4/pack_gguf.py`
  and `tools/mm3/pack_gguf.py` at tag `v1.1.30` (MIT; read from `G:\Project-maya`'s git objects), with
  `maya.py`'s `pack_step` / `stamp_pack` / `pack_source` / `pack_stale` for `download --maya`. Their function names,
  messages and the patterns `strata_tokenizer.py` ships (transcribed there from llama.cpp's `llama-vocab.cpp`) are
  kept verbatim for byte parity. No Maya file is copied by these: the chat templates DeepSeek and MiniMax packs read
  are passed in (`--template`) and are not in our tree - Maya's `tools/mm3/chat_template.jinja` is under the MiniMax
  Community License (see the Project Maya section). Proof: the same parity script (`--maya-tools`, Maya's Python
  extracted byte-exact) on synthetic models and on the owner's GLM-5.3-Flash Maya-S-v2-IQ2_XXS.
- **raon1 in the Maya packer** (added 2026-10-28): `maya_pack.cpp`'s raon1 branch (`kRaon1NativeOk`, no BF16 upgrade,
  3-D tensors, a quantized tensor outside the set dequantized to F32 by ggml's dequantizers) and `tokenizer_export.cpp`'s
  `raon` pre-tokenizer port our own additions to Maya's `tools/iq_pack.py` and `tools/strata_tokenizer.py` (the local
  diff in `engines/maya-ref`, never committed there); `RAON_PATTERN` is transcribed from ik_llama.cpp PR #2733
  (`LLAMA_VOCAB_PRE_TYPE_RAON`, MIT) as that diff has it. Proof: the parity script's `--raon-tools` (Maya v1.1.30's
  tools with that diff) on synthetic models and on the owner's raon-1-mini Q8_0 and IQ4_XS GGUFs.
- **Maya's expert profiles:** `engine/third_party/maya/data/expert-profile-{glm,ds4,mm3}.bin` (Maya v1.1.30, MIT,
  unmodified; 106,008 / 61,440 / 30,720 bytes) are written by `engine/src/multiengine/sync_maya.py` from the clone's git
  objects with the rest of Maya's tree and installed in `engine/profiles/`; the server names the GLM one on every
  GLM-5.3 launch, as Maya's setup writes it into every GLM config.

## The WebView2 SDK (strata-window on Windows, 2026-10-28)

`strata-window` (spec `docs/superpowers/specs/2026-10-28-server-window-design.md`, Part 2) shows the page in Microsoft
Edge WebView2. Its Windows build compiles against `WebView2.h` and links `WebView2LoaderStatic.lib` from the
[Microsoft.Web.WebView2][webview2] NuGet package, version `1.0.4314.35`. The package was fetched from nuget.org on
2026-10-28, with the owner's OK. It is 9,818,548 bytes, sha256
`90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4`, and its SHA-512 matched nuget.org's catalog entry.
It is unpacked under the git-ignored `/.toolchains/webview2/1.0.4314.35`; nothing from it is in this tree. CMake
reads it from `SECURE_FRONTEND_WEBVIEW2_DIR`, which defaults there.

- **Licence:** the package's `LICENSE.txt`, BSD-3-Clause ("Copyright (C) Microsoft Corporation"). The static loader
  becomes part of `strata-window.exe`, so [`NOTICE.md`](../NOTICE.md) carries that notice, as its terms require for
  binary redistribution.
- **Not linked:** the package's `NOTICE.txt` names Antlr3.Runtime and StringTemplate4 (BSD-3-Clause). They belong to
  the package's .NET WinRT projection tooling, which this build never uses or ships.
- **The runtime:** WebView2 itself (the Evergreen Runtime) is part of Windows 10 and 11 and is not shipped.
- **Our own code:** the COM handlers in `window_main_win32.cpp` are a small template of our own; the Windows
  Implementation Library and WRL are not used.

The Linux window links the system's GTK 3, WebKitGTK 4.1 and Ayatana AppIndicator dynamically (each under its own
LGPL licence), found by pkg-config; nothing of them is vendored or bundled.

[webview2]: https://www.nuget.org/packages/Microsoft.Web.WebView2

## llvm-mingw (build tool only)

[mstorsjo/llvm-mingw][llvmmingw] release `20261006` is the open-source
Windows cross toolchain fetched by `scripts/dev/bootstrap-llvm-mingw.sh`,
verified against sha256
`5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4`. It is a
build tool only; nothing from it is in this tree (it unpacks under the
git-ignored `/.toolchains/`).

License: the GitHub API (`gh api repos/mstorsjo/llvm-mingw/license`, queried
2026-10-13, default branch) returns SPDX id `NOASSERTION`, name `Other`. The
`LICENSE` file it returns states that the llvm-mingw scripts and wrappers are
licensed under the ISC license and that this covers only components provided
by llvm-mingw directly (the bundled LLVM, mingw-w64 and other components keep
their own licenses).

## Vendored crypto (SP7 M2, spec D-2)

The release trust chain deliberately adds **no third-party crypto
dependency**. `secure_frontend/src/ed25519_verify.{hpp,cpp}` (lib
`secure_frontend_ed25519`) is our own dependency-free implementation of the
RFC 8032 §5.1.7 **verification path only** (strict verification equation as
the RFC's §6 reference, canonical-S rejection per §8.4, no cofactor
multiplication — the C++ never signs), with `sha512.{hpp,cpp}` alongside.
The release-side signer `scripts/release/sign.py` embeds the RFC 8032 §6
reference code **verbatim** (only page headers removed) from the text fetched
from rfc-editor.org on 2026-10-22. Test provenance follows the same rule:
every vector in `secure_frontend/tests/fixtures/release/rfc8032-vectors.json`
was fetched from the RFC's own §7.1/§7.2 listings and cryptographically
re-derived with the RFC's own code before being committed — no hex in this
repository's crypto tests is hand-transcribed.

License of the source material: RFC 8032 (and RFCs generally) is published
under the IETF Trust's legal provisions (Simplified BSD-style for the code
components); no copyright restriction conflicts with this repository's AGPL-3.0.
Cited document: `RFC 8032, "Edwards-curve Digital Signature Algorithm
(EdDSA)"` (Josefsson & Liusvaara, January 2017), sections 5.1.7, 6, 7, 8.4
(<https://www.rfc-editor.org/rfc/rfc8032>).

## Built-in chat page: upstream's UI kit and font (2026-10-22, session log step 14)

The page served with `listener.web_ui` (`secure_frontend/web/`) takes upstream Strata's
look, by owner request. It is copied from the reviewed upstream pin (`v0.2.2`,
`serve/web/`, last changed in upstream commit `f2709679`) by `out/copy_upstream_ui.py`
(a session helper). The copy normalises line ends to LF and the three non-ASCII
characters in comments (em dash, ellipsis, degree sign) to ASCII. It drops the sprite's
embedded C2PA metadata block (an inert base64 provenance blob from the design tool) and
its inline style, points the font URLs at `/ui/fonts/`, and puts an attribution header
on each text file.

| File | Source | Licence | SHA-256 (as committed) |
| --- | --- | --- | --- |
| `web/tokens.css` | `serve/web/tokens.css` (adapted) | MIT | `b42489b95a279578244e1a4a080fc4c31169062652fced83b277ee6a295d5605` |
| `web/components.css` | `serve/web/components.css` | MIT | `c1d0cc2a77450817ee10b6cadaa7099c4b4afd20998bca45acdc14e820a666df` |
| `web/sprite.svg` | `serve/web/sprite.svg` (metadata dropped) | MIT | `55efc1936f17f21c645eac63579857bf565d21e30ac972b3d180e8bf7f022cf9` |
| `web/favicon.svg` | `serve/web/favicon.svg` | MIT | `84e586cd99715df7191c587e5b7289267f87e5035a0c4f5ab004d385809e0c1d` |
| `web/fonts/outfit-latin-wght.woff2` | `serve/web/fonts/` (byte-identical) | SIL OFL 1.1 | `6c18d579fd87c3776be068b762cbc83fde3acb543d49eabd3ade842eb987e887` |
| `web/fonts/outfit-latin-ext-wght.woff2` | `serve/web/fonts/` (byte-identical) | SIL OFL 1.1 | `0f53d1c03b3918d744a843b5039001ee31695ca1e255e3914188df81beb461e9` |
| `web/fonts/OFL.txt` | `serve/web/fonts/OFL.txt` | (the licence) | `c676351bf8576b9aba743cd5eaa8c0e7ee0d51f805d720447b4df4ddb6a2e416` |
| `web/LICENSE.strata-ui.txt` | upstream `LICENSE` | (the licence) | `949b9b0aada7476006469e58ee4ffb0e0f68c2d9137e90a356757b61a09d37f0` |

`web/index.html`, `web/app.css` and `web/app.js` are ours, modelled on upstream's
`index.html`, `app.css` and `app.js` (the markup and layout rules adapted, with the same
attribution; the script written for this server's routes). The Monitor tab (session log
step 16) follows the same rule: its markup (the state card, the context gauge and bars, the
requests table) and its layout rules are adapted from upstream's `index.html` and
`app.css`, and its rendering from upstream's `renderMonitor` in `app.js`, rewritten to
build DOM nodes (upstream's uses `innerHTML`) for this server's `/metrics`. Upstream's
conversation-cache and MCP cards and its disk and expert readings are left out (this
server has none of them). `web/theme.js` is ours. The
fonts are data, not code (contract C6 is about executables). They are WOFF2, checked by a
test, and the page's CSP allows fonts from this origin only. The licence texts are kept
beside the files and are not served.

The page's Models tab (the model launcher, 2026-10-23; spec
`docs/superpowers/specs/2026-10-23-model-launcher-design.md`) is ours: upstream's page has no
model launcher. It is written for this server's `/v1/strata/models` routes with the kit's
existing components and sprite icons (`i-layers`, `i-refresh`); no upstream file was copied for
it. The launcher's server code (`model_cards`, `model_options`, `model_runtime`,
`model_launcher`) is ours too. Each option's flag, bounds and units were read from the pinned
upstream engine's argument parser (`upstream/src/program/generate.cpp`, cited per option in
`secure_frontend/src/model_options.cpp`); nothing was copied from it.

The Models tab's Downloads section (model downloads, 2026-10-23; spec
`docs/superpowers/specs/2026-10-23-model-downloads-design.md`) is ours, and so is its server code
(`hf_source`, `download_catalog`, `download_manager`, the `gguf` manifest profile, `strata
hf-info` and `pull --progress-lines`). Its data come from the pins, never copied as code:
`tools/secure/download_catalog.py` reads upstream's `setup.py` tables (`MODELS`, `FAMILIES`,
`REPO_PINS`, `HF_REVISIONS`, ...) and Maya's `maya.py` `MODELS` with `ast`, without importing or
running them, restates their file rules and refuses to run when the pinned source no longer
contains those rules word for word; its output `secure_frontend/catalog/downloads.json`
(commits, sizes and SHA-256 values: facts, not code) is embedded at build. The prepare step
`tools/secure/prepare_download.py` is ours and embedded at build too; it runs upstream's
`tools/iq_pack.py` and `tools/strata_tokenizer.py` in place at run time (read only, from the
operator's `models.prepare_tools`), and nothing of them is copied into this tree.

The parallel ranged downloader (2026-10-23; `download_pieces`, the parallel path in
`fetch_file.cpp`) is ours. Its design mirrors upstream Strata's downloader (`upstream/setup.py`
0.2.0: ranged pieces over several connections, `DL_CONNECTIONS` 16, `DL_CHUNK` 32 MiB, a
`.part.chunks`-style resume), which we read for the algorithm and the constants; no code was
copied (upstream is Python/urllib, ours is the confined C++ curl path), and the SP4 security
model — the host allowlist, per-connection pinning, the whole-file digest, confinement — is
unchanged.

## Release evidence (SP7 M2 — placeholder)

The SP7 offline release validation kit is `docs/validation/sp7-offline-kit.md`
(build → air-gap carry → offline validate → record). When a release kit run
happens on the owner's box, fill this entry — **blank means no claim**:

| field | value |
|---|---|
| date | _blank_ |
| host (GPU box) | _blank_ |
| source commit tested | _blank_ |
| release version | _blank_ |
| pinned release public key (64-hex) | _blank_ |
| signing key fingerprint used at build | _blank_ |
| tarball SHA-256 (`strata-<V>-linux-x86_64.tar.gz`) | _blank_ |
| `.deb` SHA-256 (`strata-secure_<V>_amd64.deb`) | _blank_ |
| `sha256sum -c SHA256SUMS` verdict | _blank_ |
| `doctor` / `doctor --deep` verdicts | _blank_ |
| `serve` + local chat verdict | _blank_ |
| `update` accept / `update --rollback` verdicts | _blank_ |
| unpinned-key and tampered-component refusal verdicts | _blank_ |
| results file SHA-256 | _blank_ |

**No offline-release, GPU, or update-chain claim is made until this entry
exists.** Windows installer runtime waits for the owner's own hardware on the
same terms (`NSIS_STATUS.txt` records the toolchain state on agent boxes).

[strata]: https://github.com/Niko1221/Strata/tree/v0.2.2
[unbiased]: https://github.com/robertkarl/UnbiasedStrata/tree/46c46ecfc11d2315512f21f7549b0a7792ccd2ae
[lighttransport]: https://github.com/lighttransport/Strata/tree/glm53f
[maya]: https://github.com/mw00/project-maya/tree/451f9cb6a21ea65673bdde2e57b4a670ae2b8859
[llamacpp]: https://github.com/ggml-org/llama.cpp/tree/3cf03257f219afbe7334045ff7c6a06ac68c627d
[llvmmingw]: https://github.com/mstorsjo/llvm-mingw/releases/tag/20261006
