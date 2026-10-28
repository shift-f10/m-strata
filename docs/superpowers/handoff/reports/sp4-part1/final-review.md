# SP4 final whole-branch review: Tasks 1–24b (b1609876..c0138061)

Reviewer: Opus 5.5, read-only. I did the whole review myself and dispatched no subagents. I read the source files directly rather than only the diff package, in eight passes:

1. CMake, presets, CI and layering.
2. Manifest, catalog and trust.
3. Fetch and pull.
4. Bundles.
5. CLI and launch.
6. Store on POSIX and Windows, plus the index.
7. Tests: the Review Focus cases, labels and hygiene.
8. Tools, docs and the ledger triage.

I built nothing and ran nothing. Every finding below comes from reading code against the spec and the ledger.

## Strengths

- **Trust model has one choke point.** Every manifest goes through `trust_manifest_bytes`: the pull/catalog path, `--manifest` + `--trust-manifest`, and bundle `manifest.json`.
  - A manifest is trusted only by a catalog digest or by the operator-typed digest. Shadowing a catalog entry is refused.
  - `SHA256SUMS` is checked for agreement only and is never used as a trust source.
  - The index is read only as an owner-only file. Every installed `manifest.json` is re-hashed against the index before use, at load, launch, list, show and export.
- **Manifest validation is strict and centralised.** `parse_manifest` handles strict JSON, unknown keys and printable ASCII, then calls `validate_manifest`, which applies P1 to every caller.
  - The Task 18 stored-path-length ruling means none of the `must_rel`/`join_rel` joins built from a manifest can throw.
  - The shard-set, launch-key and reserved-name rules are all present.
- **Digest verification on every path.**
  - Fetch hashes while streaming and enforces the byte limit mid-stream.
  - A resumed `.part` is re-hashed up to its length, so preallocation is never mistaken for data (Review Focus 1 holds).
  - Suffix-less staged files are re-hashed at every run boundary.
  - Import, export and update-reuse go through one `copy_verified`, which writes `<to>.part`, hashes while copying and renames only when the bytes match.
  - Publish verifies the staged tree is exactly the manifest's files, with no links, hard links or devices, then runs the layout check and a real `load_local_tokenizer_pack`.
- **Publish crash ordering follows §4.5.**
  - The final rename never replaces an existing target. POSIX uses `renameat2(NOREPLACE)` and falls back to `fstatat`-absent + `renameat` under `.lock` (Review Focus 4). Windows calls `MoveFileExW` without `REPLACE_EXISTING`.
  - A crash between steps 4 and 5 leaves an orphan, which is adopted by digest after a re-hash.
  - `with_installed` keeps exactly one live version (the Task 9 fix).
  - Retired versions are deleted only when unheld, and a failed delete stays listed.
- **Redaction (§8.1) is disciplined.**
  - Error details carry only host, relative path, `name:tag`, status number, byte counts, digests and key paths.
  - The `Location` value and signed queries go only into curl's stdin config line.
  - Bundle entry names from untrusted media reach messages only after they pass P1. Otherwise the message is fixed text.
  - The `--manifest` path is never echoed.
- **Library layering holds.** `strata-secure-server` links only `secure_frontend_launch`, which pulls in `model_store`, `typed_config`, `engine_arguments` and `model_inventory`. It does not link `acquire`, `acquire_core`, `strata_cli` or any downloader code, so §7.1's link-level supplement is achievable in Task 28.
- **Hooks discipline is good.**
  - `SECURE_FRONTEND_TEST_HOOKS` is a single global definition, so there is no ODR split.
  - The `raw` profile, `--cacert`, fault injection and the fixture catalog are compiled only with hooks.
  - The release preset and CI job prove the hooks-OFF build.
  - `dl05_catalog_pins_match_committed_list` checks production and fixture separately and asserts that release builds embed no fixture.
- **Review Focus 3 and 5 are covered.** `parse_confirmation` is strict, and EOF or non-interactive input answers No. Import never writes into the bundle, so read-only media works.
- **The ledger is unusually complete.** Every ruling has a cost-if-wrong line, which made this triage possible.

## Issues

### Critical (Must Fix)

**C1. `strata import` lets a license change go through on `--yes` alone (§6.1, §2.2 `license.notice`).**
`src/strata_cli.cpp:110-139` (`verb_import`) and `src/bundle_io.cpp:195-238` (`import_bundle`).

- **What happens:**
  - Import shows the bundle's license and asks one `Import …? [y/N]`, which `--yes` answers.
  - It then calls `import_bundle`. That function decides `update` (higher sequence, or lower with `--allow-downgrade`) and publishes, which retires the installed version.
  - There is no manifest diff and no license comparison. A newer bundle whose license changed is installed under `--yes`, unattended.
  - If the installed version cannot be read, nothing is asked either.
- **Why it matters:**
  - §6.1 says a license change "requires an interactive `y` even with `--yes`". It is one of the explicit `--yes` limits.
  - The controller already ruled this exact gap Important and fixed it for `pull` (Task 22 fix 1).
  - `import` is reachable in this branch today, while `pull` is not yet wired. A reasonable person expects the same consent rule whichever way the update arrives: a bundle on a USB stick is how the air-gapped user (§1.4) updates.
- **How to fix:**
  1. Under the lock, compute `decide_install` before the confirmation. Split `import_bundle` into decide → confirm → copy/publish, or pass a `Confirmer` in.
  2. On `update`, print `diff_manifests` (terminal only), as `run_pull` does.
  3. If `diff->license_changed`, or the installed version is unreadable, ask a second question with `yes_applies=false`. On No, decline with exit 2.
  4. Add a regression test: an import of v2 with a changed license under `--yes`, non-interactive, exits 2 and v1 stays live; an interactive `y` accepts it.
  5. While there: when the bundle is already installed, report it before asking anything. Today `import` of an installed bundle with no `--yes` exits 2 "declined" instead of 0 "already installed" (§8.3).

### Important (Should Fix)

**I1. A publish failure after step 2 makes the whole staged download look stale; the next pull deletes it.**
`src/acquire_publish.cpp:162-171` (step 2 renames `MANIFEST` to `manifest.json` inside `.staging/<k>`) and `src/acquire_pull.cpp:103-112, 127-131, 357` (`staged_manifest` reads only `MANIFEST`).

- **What happens:**
  - Step 4 (staging → version dir) or a later step fails with `publish_failed`. On Windows this is realistic: an AV or indexer handle on a freshly written 39 GB shard can outlast the 5 s of sharing retries. On any platform, `target exists` triggers it once the user clears an orphan by hand.
  - `.staging/<k>/` is left holding `manifest.json` instead of `MANIFEST`.
  - On the rerun, `staged_manifest` returns nullopt, the plan treats the key as empty, and after the `[y/N]` answer `tidy_staging` deletes the key as `stale_staging_discarded`.
  - Every verified file is thrown away and the user downloads tens of GB again.
  - A crash between steps 2 and 4 has the same effect.
- **Why it matters:** §8.2 classes `publish_failed` as "rerun resumes: yes", and §4.5's crash story covers only the window between steps 4 and 5. A reasonable person whose pull failed at the very last step expects the rerun to finish in seconds.
- **How to fix (either works):**
  - Have `staged_manifest`, and the `MANIFEST` existence check at `acquire_pull.cpp:437`, accept `manifest.json` when `MANIFEST` is absent, provided the digest still equals the key. The rename back can be lazy: publish renames again anyway.
  - Or, on a failure in steps 3–4, rename `manifest.json` back to `MANIFEST` on a best-effort basis.
- **Test:** a hooks-only fault that fails `rename_within` at step 4 once, then a rerun that publishes with zero transfers.

**I2. `rm` of a model whose files are open (Windows) leaves an unmanaged orphan, not a retired version (contradicts Controller note 5).**
`src/acquire_publish.cpp:226-248` (`remove_installed`).

- **What happens:**
  - The index is written *without* the entry and *without* a retired row. Then the tree is deleted.
  - Server discovery does not exist (`NoServerDiscovery`), so on Windows the delete fails part-way on the engine's open files, with some files already gone.
  - Controller note 5's backstop says the version "stays `retired`". Instead it becomes an orphan:
    - `rm --retired` never touches it.
    - `strata list --all` reports it as orphaned, with a "delete it by hand" hint.
    - A later `pull`/`import` of the same manifest finds its `manifest.json` (if it survived) and runs `adopt_orphan` → `verify_tree`. That fails with `digest_mismatch` or a confusing `publish_failed: … staged files incomplete` on every retry, until the user deletes the directory by hand.
- **Why it matters:** this is Windows store parity, and the stated recovery path does not exist. Users will hit it because `rm` cannot know the server is running.
- **How to fix:**
  - Write the index with the entry moved to `retired`, using `without_model` plus a `RetiredEntry`, then attempt the delete. On success, drop the retired row through the existing `retire_unheld`. A failed delete then stays retired and is cleaned up by the next `pull`/`import`/`rm --retired`, exactly as note 5 says.
  - Separately, make `adopt_orphan` return "not adoptable" (nullopt) when `verify_tree` fails instead of failing the whole verb. The publish then reports `target exists` and `report()` prints the orphan hint.

**I3. Directory components that differ only in case are accepted, then fail at publish on Windows (Review Focus 2).**
`src/acquire_manifest.cpp:267-301`.

- **What happens:**
  - Only whole file paths are folded, plus a file-equals-directory check.
  - `Pack/extra.bin` beside `pack/index.txt` passes validation.
  - On Linux these are two directories. On Windows they are one directory with one spelling, so `verify_tree`'s exact-case listing reports an "unexpected entry" → `publish_failed`, after the whole download.
  - A bundle built or exported on Linux from such a manifest can never be imported on Windows.
- **Why it matters:** Review Focus 2 names exactly this example ("`Pack/x` beside `pack/y` after case-folding") as "must be `manifest_invalid`, not a publish-time collision on Windows". The test (`manifest_test.cpp:294-302`) covers only the `PACK` file-versus-directory variant.
- **How to fix:** while iterating files, map every folded directory prefix to its exact spelling and reject a second spelling with `manifest_invalid`. Add `Pack/x` + `pack/y` to `file_and_directory_prefix_collision_rejected`.

### Minor (Nice to Have)

**M1. A typed `--trust-manifest` digest that does not match is ignored when the bytes are a catalog manifest.** `src/manifest_trust.cpp:32`.

- §2.5 rule 2 says the file's digest "must equal the given one". A typo, or the wrong file, goes unnoticed. This is not a bypass, since catalog trust is stronger.
- **Fix:** if `trusted_digest` is given and differs from the digest, return `manifest_untrusted` before the catalog lookup. It is one line plus a case. Ledger Task 14 item; see the triage.

**M2. Update reuse: the preflight leaves out bytes that will be copied, and a failed copy silently becomes a download nobody confirmed.** `src/acquire_pull.cpp:220-222, 385-389`.

- Unchanged files are *copied* into staging, so they consume space. The spec's text (§4.6 subtracts "adoptable" bytes) misses that too, so this needs a controller ruling.
- When `copy_verified` fails (bit rot, or ENOSPC), the code falls through to fetching. The bytes fetched can then exceed the confirmed "Download X GB".
- **Fix:**
  - Count reused bytes in `needed`.
  - On a copy failure, print a fixed line naming the file, and either ask again or fail with the copy's reason.

**M3. `StreamConfirmer` prints "no (not interactive; pass --yes)" for questions `--yes` cannot answer** (license change, abandoned partials). `src/strata_cli.cpp:356`. The text tells the user to do something that will not help. Print a separate message when `yes_applies` is false.

**M4. `report()`'s orphan hint matches any `publish_failed: …: target exists`.** `src/strata_cli.cpp:60-64`.

- Staging renames inside pull (`.part` → final, files into a bundle's `files/`) produce the same suffix and would print a wrong "version directory" hint.
- Pull is not wired yet, so narrow the match to the version-dir shape before Task 27 reuses `report()`. Ledger Task 23 item.

**M5. `pull --bundle` creates the target directory, and with an all-present bundle writes `manifest.json`/`SHA256SUMS`/`STRATA-BUNDLE`, before or without any `[y/N]`.** `src/acquire_pull.cpp:299` and the `work.empty()` skip at `:411`.

- This breaks F-56 ("nothing written before the answer").
- It is not reachable until Task 27. Fix it before Task 27 wires `pull`: probe the target read-only first, create it after the answer, and ask even when nothing needs downloading.

**M6. Hex and octal IPv4 forms pass `is_valid_dns_name`** (`0x7f.0x1`, `1.0x7f000001`). `src/acquire_url.cpp:95-110`.

- The spec says "no IP literal". Without a proxy, the §3.5 deny list catches these after `getaddrinfo`. With `pull.proxy` set, nothing does.
- The manifest is still digest-trusted, so this is low risk.
- **Fix before Task 27:** reject a name whose labels all parse as `inet_aton` numbers (decimal, `0x…` or octal).

**M7. Confinement of external roots rests on the call sites only.**

- `lock_models_root`, `publish_staged`, `import_bundle` (models side) and `remove_installed` accept a `CanonicalRoot` with `external() == true`.
- An assertion-style guard costs a few lines and protects Task 27's new call sites. Return `publish_failed`, "internal: external root", if `root.external()`.

**M8. No top-level exception boundary in `strata`.** `src/strata_main.cpp:13`.

- A `std::filesystem` throw (for example `absolute()` on Windows inside `create_target_directory`), a `bad_alloc`, or a `logic_error` from `must_rel` ends in `std::terminate`.
- Wrap `run_strata` and map anything unexpected to a fixed internal-error line with exit 7.

**M9. Hints point at verbs this build does not have.**

- `installed …; start it with: strata run …` (`acquire_pull.cpp:483`, `strata_cli.cpp:138`).
- The README's "`strata pull` or `strata import`".
- Today `run`/`pull` print "not available in this build yet". This is acceptable while Tasks 25–29 are pending, but it must not ship as is.

**M10. `EOPNOTSUPP` is not in the rename-fallback errno set.** `src/models_store_posix.cpp:220`. It is cheap insurance for Review Focus 4 on unusual FUSE back ends, and safe because the fallback is under `.lock`. Ledger Task 16 item.

**M11. Duplicated logic across tasks.** Each copy will drift; consolidate before Task 27 adds more:

- ASCII lowercasing in 4 copies: `acquire_url.cpp`, `acquire_manifest.cpp`, `download_response.cpp`, `acquire_pull.cpp`.
- `printable()` in 2 copies.
- `ensure_parent` (bundle_io) duplicates `ensure_parent_of` (acquire_pull).
- "Hash this file and compare with a `ManifestFile`" appears 5 times (pull ×3, bundle_io, publish).
- 3 free-space preflights with three different formulas (`run_pull`, `import_bundle`, `prepare_bundle_target`).
- The import summary in `verb_import` is a weaker copy of `print_summary`: no "bundling allowed" and no per-file lines.
- `staged_manifest` duplicates the parse loop in `rm_partials`.

**M12. Test hygiene.**

- The pure `parse_confirmation` checks sit inside a `windows-runtime` case, so they never run on wine. Split them into an unlabelled case.
- `engine_version_read_from_upstream_version_file` largely re-states CMake's own substitution.
- Windows preallocation assertion cannot fail (ledger Task 17 item).
- The mount-root test child has no `catch(...)`, which in a forked child would unwind into the runner (ledger Task 20 item).

**M13. -Werror risk on presets not run locally.**

- MSVC `/W4 /WX` (informational only) may raise C4127 on `if (kIpv6ResolvePinVerified)` (`address_policy.cpp`) and `if (secure_frontend::kTestHooksEnabled)` in tests. Use `if constexpr` to remove the risk.
- The generated `catalog_data.cpp` puts `0x80..0xFF` byte values into a `char[]` initializer. That is a narrowing error on GCC/Clang and C4309 on MSVC the day a catalog manifest holds a non-ASCII byte. Manifest rules forbid such bytes, but emit `unsigned char`, or add a CMake guard (ledger Task 14 item).
- I found nothing else likely to fail on MSYS2 Clang. The llvm-mingw cross build already compiles every Windows TU with Clang `-Werror`. The POSIX-only TUs (`models_store_posix.cpp`, the fake-curl POSIX paths) are only GCC-proven locally; `linux-clang` CI covers them.

## Deferred-minor triage (from progress.md)

Legend:
- **FIX-MERGE**: fix before merging this branch.
- **FIX-27**: fix before or with Task 27 (network verbs), the first live caller.
- **CARRY-25/28**: the named later task must handle it.
- **WAIT**: can wait indefinitely.
- **CLOSED**: already resolved in-branch.

| Task | Item | Verdict | Why |
| --- | --- | --- | --- |
| 1 | >120-col line in sha256.cpp | WAIT | cosmetic |
| 1 | friend `sha256_add_length_for_test` declared in hooks-OFF builds | WAIT | harmless declaration; tidy when touched |
| 1 | `kTestHooksMarker` unused | CLOSED | the `strata` and server banners now reference it in hooks-ON builds |
| 1 | `long_message_one_gibibyte` ~55 s | WAIT | consider a `slow` label later |
| 2 | >120 cols, `<cstddef>`, unused `<tuple>`, `reason_name` untested | WAIT | cosmetic |
| 2 | `reason_info` has no range check | CARRY-25/28 | harmless while reasons come only from the enum; must be checked once reasons are decoded from the record channel or a log |
| 3 | hex IPv4 accepted as a host | FIX-27 | see M6; fails open only behind a proxy |
| 3 | proxy host grammar loose / rejects uppercase | WAIT | config-time usability; nothing fails open |
| 3 | `describe_url_problem` text https-only | WAIT | no current caller shows it for proxies |
| 3 | `Location` scheme case-sensitive | WAIT | fails closed |
| 3 | boundary tests | WAIT | coverage |
| 4 | comment wording, test placement | WAIT | cosmetic |
| 5 | url/host built independently | CLOSED | `hop_request` builds both from one `HttpsUrl` (Task 21) |
| 5 | proxy/`--cacert` argv tests | WAIT | coverage |
| 6 | boundary tests, status <100, dead check, size-0 underflow | WAIT | all fail closed |
| 7 | restart reasons consume retry slots; cap of one 200-to-range restart | CLOSED | `FileState::restarted_after_ignored_range` |
| 7 | untested edges, header note | WAIT | coverage |
| 8 | P1 grammar; pack-holds-`index.txt` | CLOSED | Task 14 moved P1 into `validate_manifest`; `check_layout` enforces the pack files |
| 8 | `is_valid_model_name` noexcept but allocates | WAIT | `bad_alloc` would mean `std::terminate`; drop `noexcept` when touched |
| 8 | non-collision tests, issue order | WAIT | but see I3 for the real gap |
| 9 | >120 cols, unvalidated helper inputs, missing staging-plan cases | WAIT | coverage and cosmetic |
| 10 | test fn outside namespace, unused include, MISSING asymmetry, line order | WAIT | cosmetic |
| 10 | diff lines carry the license URL; never log them | CARRY-27 | the comment at `acquire_pull.cpp:418` holds the line; Task 27's `acquire.log` writer must keep it |
| 11 | `build_crt_fd_block` handle_width unchecked | CARRY-25 | Task 25 is the first caller; add a check there |
| 11 | readable-root check one-directional | CARRY-25 | an allowed downloader parent *beneath* `roots.models` is not flagged; Task 25 must check both directions |
| 11 | `landlock_rights_for_abi(<=0)` | CARRY-25 | gate on the probe |
| 12 | fake curl has no `stall` switch | CARRY-27/28 | needed by a stall e2e test |
| 12 | `spawn_with_fd4` EINTR, Windows pipe leak | WAIT | test support only |
| 13 | `--move-shards` moves before the empty-file check | WAIT | dev-only; the shards end up in `out/files/weights`, not lost |
| 13 | traceback on bad pins, silent `--ple-shard`, untested paths | WAIT | dev-only |
| 13 | **parked** F-25 pins for iq3_xxs / iq3_s / iq1_m | WAIT for merge; owner before any release | catalog ships empty (D5 (D)); a wrong pin fails closed in `make_bundle.py` |
| 14 | typed digest ignored for a catalog manifest | FIX-MERGE | one line; spec literal (M1) |
| 14 | CMake regex looser than the parser; silent `EngineVersion{}` fallback | WAIT | fallback 0.0.0 makes every model `engine_too_old` (fails closed) |
| 14 | alias targets unvalidated, CMake guards for names / non-ASCII bytes | CARRY (first real catalog entry) | must land with the first production manifest (see M13) |
| 14 | engine_version test tautological, unused include, duplicate `printable` | WAIT | M11/M12 |
| 15 | missing cases, `path::string()` in tests, >120 cols | WAIT | coverage |
| 15 | `scan_model_root_files` drops non-P1 entries | CLOSED | Task 23 counts and reports `skipped` |
| 15 | **open doc item** spec acceptance names `secure_frontend_config_tests` | CARRY-29 | docs task |
| 16 | `EOPNOTSUPP` | FIX-MERGE (cheap) or WAIT | M10 |
| 16 | preallocate EFBIG/EIO → exit 7 | WAIT | the FAT check precedes it on bundle targets |
| 16 | `PartFile` usable after `close()` | WAIT | no caller does it |
| 16 | parent-absent semantics | CLOSED | Task 19 |
| 16 | pre-existing `.lock` mode not enforced | WAIT | `.lock` lives in the 0700 root |
| 16 | `bundle_invalid` in pull context | CLOSED | `verify_tree` maps it to `publish_failed` |
| 17 | Windows preallocation assertion cannot fail | WAIT | M12 |
| 17 | DACL set by path; sleep after the 10th retry; reparse branch lacks retries; double `PinnedParent` | WAIT | private root; perf and cosmetic |
| 17 | `free_bytes` on a root longer than MAX_PATH | WAIT | fails closed (`disk_space`); use `extended_path` when touched |
| 18 | `manifest_core` includes the acquire_core header for one constant | WAIT | header-only, no link edge; move `kBundleFilesDir` when touched |
| 18 | untested fail-closed paths; test helper `std::get`; index accepts tag `default` | WAIT | all fail closed at `load_installed` |
| 19 | Windows `restore_owner_access` duplicates SDDL, rewrites DACLs | WAIT | children carry protected DACLs, so little propagation |
| 19 | `adopt_orphan` hides read errors as "no orphan" | FIX-MERGE (with I2) | make verify failure "not adoptable" so the orphan hint appears |
| 19 | `verify_tree` I/O detail; sync drops `disk_space` | WAIT | |
| 20 | empty unknown directories under `files/` accepted | WAIT | no trust impact |
| 20 | P1-valid entry names echoed | WAIT | allowed by §7.3 (relative paths) |
| 20 | external-root confinement rests on call sites | FIX-27 | M7; cheap guard |
| 20 | mount-root test child has no try/catch | WAIT (cheap) | M12 |
| 20 | Windows folder-mounted volume / UNC root | WAIT | the user's explicit choice; UNC share roots are already refused lexically |
| 21 | dead `resumed_from_disk`, duplicated detail, `reset()` inconsistency, weak sleep assertion, >120 cols | WAIT | cosmetic |
| 21 | untested L==size, L>size, 503/429 retry, `host_not_allowed`, `dns_failed`, per-chunk interrupt | FIX-27 | coverage must exist before the code goes live |
| 21 | back-off sleeps not capped or interruptible | CARRY-27 | Ctrl-C during a 32 s back-off must stop at once |
| 22 | `--bundle` target created before the answer; all-present bundle asks nothing | FIX-27 | M5 (not reachable until 27) |
| 22 | two disagreeing free-space preflights; `tidy_staging` swallows list errors | WAIT | M11 |
| 22 | `format_gb` locale | WAIT | the binary never calls `setlocale` |
| 22 | missing probe-failure / reuse-fallback / declined `--bundle` cases | FIX-27 | with M2 and M5 |
| 22 | pure `parse_confirmation` checks never run on wine | WAIT (cheap) | M12 |
| 22 | `max_duration_s` starts after confirmation | WAIT | better UX than the spec letter; record as a spec note |
| 23 | `report()` orphan hint too broad | FIX-27 | M4 |
| 23 | `CliServices::test_cacert` present in release builds | CARRY-27 | Task 27 sets it under hooks only |
| 23 | engine_gate detail shows the profile name; `replaced()` unchecked | WAIT | cosmetic |
| 23 | `needs pack (bundle)` state unreachable | WAIT | spec note for Task 29 |
| 24a | no direct `--expert` parse / describe / name:tag edge cases; >120 cols | WAIT | `cli_expert_flag_parsing` and `describe_reports_unset_expert_keys` landed in 24b; the remainder is coverage |
| 24a | temporary "arrive with Task 24b" text | CLOSED | none remains in `src/` or `include/` |
| setup | commit-trailer ruling (model that wrote the commit) | n/a | metadata, accepted by the top-level controller; not a code item |
| 18 | SP1 `canonical_path.hpp` `kRelativePathMaxBytes` (not a named delta) | WAIT | flag for the rebase onto SP2 run-2 (likely trivial conflict) |

## Declined to judge

- **Downloader child, confinement, Landlock, Low-IL and the CRT block in use, `doctor`, `acquire.log` writing, signal-driven interrupt, network verbs, privacy proofs and docs:** Tasks 25–29, explicitly out of this branch. I reviewed only the planning code as input to those tasks.
- **Engine version from the committed `upstream.version` instead of configure-time `git describe` (spec §2.3):** controller ruling R-1, accepted.
- **Production catalog ships empty, plus the `make_bundle.py` (B′) path:** owner decision D5 (D) / Controller note 10.
- **`rm --force` has nothing to override and `NoServerDiscovery` answers "not held":** Controller note 5, an SP2 dependency. I judged only what the code does when the OS refuses the delete (I2).
- **`run` stops at "model ready" and `serve --expert` lives only on `strata-secure-server`:** Controller notes 3 and 4.
- **`transport_failed` → next source rather than retried:** Controller note 13.
- **Landlock ABI ≥ 6 `scoped` rights left unhandled:** Controller note 14, reviewed with SP2.
- **IPv6-only hops failing closed (`kIpv6ResolvePinVerified` false):** spec §3.5 marks the bracket form as "to verify".
- **The proxy path skips the address policy:** spec §3.5 / §13.2 accepted risk.
- **Retired-version deletion on Windows while the engine still runs (files it has not opened yet get deleted):** depends on server discovery (SP2); accepted by note 5.
- **A 48-hour pull blocks `rm`:** spec §4.8 accepts it for v1.
- **Program update exits 12 with no network:** spec §6.2 / C7 / Controller note 16. The implementation matches.
- **`describe()` / `--check` printing `pull.proxy`:** SP1's `--check` contract prints config values. §8.1 governs errors and logs.
- **The commit-trailer model attribution:** a metadata ruling already accepted by the coordinator.
- **`export` or `--bundle` into a directory inside `roots.models`:** odd but non-destructive (refused when non-empty, later visible as orphaned). The spec is silent and no reasonable user does it.
- **`verify` not running the pack-layout/tokenizer check:** §1.5 / §5.2 define `verify` as read-and-hash. `import` runs the layout check at publish.

## Recommendations

1. **Before merge:**
   - Fix C1.
   - Fix I1, I2 (with the `adopt_orphan` change) and I3.
   - Fix M1. Each fix is a few lines plus a regression case. Re-run linux-gcc, linux-gcc-release and the wine preset. MSYS2 CI must pass the Windows-labelled cases, especially I2's retired-on-failure behaviour.
2. **First items of Task 27's brief:** M4, M5, M6, M7, the M2 ruling, and the untested fetch paths. Do the M11 consolidation (shared summary/confirm helper used by both `pull` and `import`) at the same time, so the C1 logic exists once.
3. **Add to Task 25's brief:** the two-direction readable-root check, the `handle_width` check, and the ABI ≤ 0 gate.
4. **When the first real catalog entry lands:** validate catalog name/tag directory names, non-ASCII guard, alias targets in CMake (M13), and get owner confirmation of the F-25 pins.
5. **Rebase onto SP2 run-2:** the SP1 files touched outside the named deltas (`canonical_path.hpp` `kRelativePathMaxBytes`) are the likely conflict points. The `secure_server_main.cpp` launch rewrite and SP1's `ServerFixture --expert` are the facts SP3 M1 re-anchors on (Controller note 20).

## Assessment

**Ready to merge?** With fixes.

**Reasoning:** the security core holds end to end: trust choke point, streaming digest verification, no-replace publish, single live version, §8.1 redaction and the layering for `strata-secure-server`. One `--yes` limit fails open on a verb that is live now (`import` lets a license change through), so it must be fixed first. Three reasonable-person failures should be fixed alongside it because they are cheap now and expensive once Task 27 makes them reachable at scale: staged data lost after a late publish failure, `rm` on Windows leaving an unmanaged orphan, and case-variant directories passing validation.
