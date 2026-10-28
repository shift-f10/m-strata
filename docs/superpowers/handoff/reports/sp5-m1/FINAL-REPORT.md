# SP5 M1 (Strata parity table, lifecycle, CLI verbs) — FINAL REPORT

Lane: `sp5-parity-cli`, worktree `/home/opencode/projects/multi-strata/.worktrees/sp5`, base `60f94aeb` (main). Upstream pin `cbfd5b92` (tag `v0.2.1.3`) — read-only; **zero** upstream files touched on the branch.
Status: **all 15 plan tasks complete**. 14 commits — one per task (Task 1 lands no files): `e5ffa597, 42bcafa7, e2743afe, cbd37ba4, 25364882, 1c4fc2e8, 9a6273ce, 3bc08b21, 761b9b4a, cbb4e747, b3b7c6d6, 8b61436b, 18b2af90`, plus this docs commit (HEAD). Every commit authored `shift-f10 <40450442+shift-f10@users.noreply.github.com>`, carries `Co-Authored-By: Qwen 3.8 Flash Next IQ3_S <noreply@anthropic.com>`, and — per the owner's 2026-10-20 ruling — no `Claude-Session:` trailer.

## What was built (task → deliverable, one line each)

1. baseline reproduced; every seam the plan names located and inventoried (no files);
2. the parity spine: `parity_table.{hpp,cpp}` — 121 rows, every option the pin reads (87 config keys + 31 env knobs, greps verbatim into the committed `tests/parity_keys_upstream.hpp`) exactly one disposition (`typed` / `rejected` / `mapped_away`) with a `serve/<file>.py:<line>` citation; meta-tests pin coverage, one-row-per-option and citation shape;
3. excluded keys refused at config load **by name**; `strata doctor` warns about ignored `STRATA_*`/`MAYA_*` env knobs;
4. sampling defaults byte-exact to the pin's validator; parameters reach `GEN`;
5. reasoning family typed (effort/budget/recovery; Maya's names kept, C-3); `fit_max_tokens` honours both pin behaviours;
6. lifecycle: `engine.lazy_load` (typed, no flag — the pin has none), `engine.idle_unload_s` (respawn on request), `mtp=false` drops the draft layer;
7. VRAM/GPU/elastic family typed and composed into the engine argv;
8. silence/queue/room/stop-token keys at the pin (queue/timeout trio re-destined `mapped_away` per Ruling T8);
9. session/context family typed with path rules;
10. C-4 narrowing: `replicas.parallel` typed, rendered `--batch N` inside the one child; the pin's N-child replica config rejected by name;
11. vision artifacts: manifest `files[]` role `mmproj` (at most one), a new supervised `strata-vision` child (READY/ENC/QUIT, own kill exit code), resident encoder started at boot — no READY means boot fails;
12. the C-9 request path: content-parts accepted only for vision-capable models; caps answered before any decode (count → base64 length → strict decode → 32 MiB → side probe); 0600 staging under `<state>/vision`; `ENC` round trip inside the lease before the `GENI` write (pin FIFO :4489); `GENI` embeddings tail; `<|vision_start|>`/pads/`<|vision_end|>` prompt runs (marker literals refused in user content); per-request scratch cleanup; `vision.encode_timeout_s` (schema 108→109);
13. `strata serve`: the supervised boot moved **verbatim** into library `secure_frontend_server_bootstrap`, foreground, managed-only, no daemonization and no lifecycle files (tree-walk proof; SIGTERM → exit 0 ≤5 s);
14. `strata version` (the tag governs; the engine's self-version is labelled informational and never spoken by this binary) and `strata key show [--env] | key rotate` on SP1's exact store machinery (token printed once, lives in exactly one file, a rotated-out token in zero files);
15. truth pass: README parity mirror (typed/mapped/rejected blocks regenerated from the C++ table) + verb paragraph + the images-are-an-input fix; threat-model rows (env knobs, image path, lifecycle timers, encoder child, no lifecycle files); PROVENANCE bump runbook extended with the parity pin-sites; new pytest gate `tools/secure/test_parity_readme_coverage.py` re-checks README blocks against the table (121 rows, never vacuous).

## Gates (final tree)

- `ctest --preset linux-gcc`: **100% tests passed out of 803** (799 run + the 4 allowed environment skips). Lane growth 750 → 803 (+53 cases); every task's exact denominator is in the ledger.
- release-and-also (`ctest --preset linux-gcc-release`): **10/10**, build 0 warnings.
- `windows-llvm-mingw-cross`: EXIT=0, **0 warnings**, every task. wine: **not run on this host** (owner ruling 2026-10-20; CI disabled — Windows runtime waits for the owner's hardware; last CI-authorized proof `windows-msys2-clang` at `7bddb9e1`).
- root build + `ctest --test-dir out/root`: 769 registered, 100%, 8 env skips, 0 warnings.
- TSan (clang18 from `.toolchains/clang18`, `setarch -B -R`, pinned `llvm-symbolizer`): filtered concurrency suites — engine supervisor/child/stdio/process, connection pool, socket server, chat execution, api server — **224 cases, zero ThreadSanitizer reports**; only the parked flake (below) flaked once and passed on rerun. Task 12 also spot-ran its two new supervisor cases under TSan: clean.
- pytest: **26** (25 + the new parity-mirror gate). `secure_distribution_check` PASS at the lane baseline; the lane is C++/docs/tests only.
- every C++ build: `-Werror`, 0 warnings; debug builds clean at each task boundary.

## Rulings (full text in the lane ledger `.superpowers/sdd/2026-10-21-sp5-m1-strata-parity/` + task briefs)

- T2: env knobs get their own dispositioned rows (the plan's key-only table would have left 31 knobs unaddressed); effort vocabulary kept as Maya's (C-3).
- T6: `lazy_load` gets no flag (the pin has none); idle unload respawns on the next request, matching the pin.
- T8: `max_queue`/`queue_timeout`/`timeout_s` re-destined `mapped_away` (our SP3 admission already owns the behaviour); `answer_room` typed-only (T8-d — the pin's formula lives in its thinking parser, not in our §7.2 room rule).
- T10 (C-4): `replicas.parallel` is the only replica surface typed; it renders `--batch N` exactly like the pin; the N-child scheduler stays rejected by name.
- T11: NEW `VisionSupervisor` role beside the engine supervisor (READY/ENC/QUIT, `kVisionKillExitCode`), encoder exe = `strata-vision` sibling of the engine exe (the pin's config-named vision exe stays rejected), manifest role `mmproj` ≤1 entry, resident=true starts at boot and no READY fails the boot.
- T12 (a..l): data-URL images only (http(s) image URLs refused by name, never fetched); schema gate = `vision_capable` (enabled AND manifest mmproj); encode inside the lease (pin FIFO); image requests flip to plan-after-encode; GENI via `GenerationSettings.embeddings`; `kChatMaxJsonDepth` 4→6 for the parts grammar (both SP3 depth pins moved with the rule, DOS property intact); chat model grammar mirrors `ModelId` `name[:tag]` (T12-l — the served managed id was otherwise unaddressable; junk still 400/404 with no engine contact).
- T13 (a..h): `serve` = managed only (`--expert` stays a `strata-secure-server` flag); boot moved verbatim into the bootstrap library; no daemon/`server.json`/lock (PX writer has never existed; pin has none).
- T14 (a..f): no separate frontend version — the tag governs, so `version` reads the one configure-time source; `key` reuses SP1's store rules as the security verdict.
- T15: README blocks are a mirror regenerated from the C++ table (the table stays the authority); the parity artifacts are named as bump pin-sites in the PROVENANCE runbook.

## Parked for the owner

- Pre-existing SP2 flake `api_server_tests.log_line_format_and_rate_limit` — untouched all lane; reproduced once under TSan at the final gate, passed on rerun (no sanitizer report; not a new finding).
- GPU, real encoder, and the D-1 acceptance kit (corpus mode of `strata-api-probe` against plain upstream `serve` and Maya's server) run only on the owner's boxes — M2.
- Windows runtime for every `windows-runtime`-labeled case (incl. the new serve/vision process cases) — owner hardware; CI stays disabled.
- Recorded gaps (docs + threat model): vision child has no per-generation cgroup delegation in M1; `session.keep_files` LRU consumer is out of M1 (per-request cleanup instead); `session.answer_room` typed-only.
- The lane was never pushed (agents never push; GitHub is storage). Merge of `sp5-parity-cli` onto `main` is the merge gate's decision.

Nothing is claimed for a GPU or for Windows runtime beyond the local cross-build and the disabled-CI history.
