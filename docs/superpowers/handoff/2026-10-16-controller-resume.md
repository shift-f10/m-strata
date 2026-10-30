# Secure Strata — controller resume document (written 2026-10-16, at the owner's stop)

**Read this first if you are the agent resuming this project in a new chat.** It records what was built, how the work was run, every rule the owner (the owner) set, the exact state of every branch, what is waiting on the owner, and how to pick up. The code-level truth is `docs/AGENT_HANDOFF.md`; this file is the orchestration-level truth. Everything referenced here is committed in this repository; nothing needed to resume lives only in a chat transcript or a temp directory.

Owner's last instruction (2026-10-15 01:35 UTC): *"Close these out then push, then we stopped here until I tell you to."* Nothing may be started until the owner says so.

---

## 1. What this project is

Secure Strata (`shift-f10/multi-strata`, local checkout `/opt/projects/ai`) is a dependency-free C++20 **security frontend** around the upstream **Strata** inference engine (Niko1221/Strata, pinned as the never-modified git submodule `upstream/` at **v0.1.44.1**, gitlink `296b59d2`). The frontend owns the network edge, authentication, configuration, model acquisition, and supervision of the engine child; the engine does inference on an NVIDIA GPU. Goal: a hardened, privacy-first release for Linux and Windows, built and verified entirely without a GPU on the dev host, then validated once on the owner's offline NVIDIA box (Linux and Windows).

Decomposition (approved order): **SP0** foundation/hardening → **SP1** typed config, paths, tokens → **SP2** portable supervision → **SP3** API concurrency (M1) and streaming (M2) → **SP4** explicit verified pull/import/update → **SP5** frontend parity → **SP6** GLM-5.3 engine port → **SP7** release + offline GPU validation kit.

### Status at the stop

| Sub-project | State | Where |
| --- | --- | --- |
| Layout (upstream as submodule), shared pre-task (per-case CTest) | merged | main |
| SP0 foundation | merged | main |
| SP1 typed config / paths / tokens (24 tasks) | merged, real-Windows verified | main |
| SP2 portable supervision (30 tasks, two runs) | merged, real-Windows verified | main (merge 8333eccf) |
| Windows fixes after SP2 (harness 8.3 paths, conhost, handle baseline, **UTF-8 argv product fix**) | merged | main (863b3ff6) |
| SP4 part 1, Tasks 1–24b (SHA-256, manifests, store, index, bundles, fetch engine, `strata` verbs) | merged | main (merge 4b9fdd31) |
| **PP-line protocol fix** (Strata-profile prefill heartbeats accepted) | merged | main (c9629e6e) |
| SP4 part 2, Tasks 25–29 (confined curl downloader Linux + Windows, verbs end to end, privacy proofs) | merged, real-Windows verified | main (merge bd3b53a6) |
| **SP3 M1** concurrency + HTTP hardening (26 tasks: 0–5, 6a, 6b, 7, 8a, 8b, 9–23) | **Tasks 0–23 complete, reviewed and merged** | main (merge `b7fb97c9`); lane `sp3-api-streaming` saved at `11f22e47` |
| SP3 M2 streaming (17 tasks) | **complete and merged** | main (merge `c83119a9`); lane saved at `847b5b9e`; plan `docs/superpowers/plans/2026-10-14-sp3-m2-streaming.md`; ledger `docs/superpowers/handoff/reports/sp3-m2/` |
| SP6 GLM engine | DRAFT spec + research; **decision sheet adopted by the controller 2026-10-22** (no-block directive; owner may reverse any value — §5 item 1); **plan now writable, queued after SP7** | specs/research below |
| SP5, SP7 | SP5: **COMPLETE** (M1 `600e371c` + M2 `073af122`); SP7: **M1 merged** (service verbs, uninstall, full doctor), **M2 planned & queued** (release side: Ed25519, signed manifest, `strata update`, packaging, SBOM, offline kit) | spec `docs/superpowers/specs/2026-10-21-sp5-parity-typed-options-design.md`; SP7 spec/plans/ledgers `docs/superpowers/specs/2026-10-22-sp7-*`, `docs/superpowers/plans/2026-10-22-sp7-m1|m2-*`, `docs/superpowers/handoff/reports/sp7-m1/`; kits `docs/validation/README.md` |

**`main` = `bd3b53a6` = `origin/main`.** CI run 38660065273 on it is green on every blocking job: linux-gcc, linux-clang, linux-gcc-release (shipped configuration), windows-llvm-mingw-cross, top-level-integration, and **windows-msys2-clang on real Windows 516/516**. The `windows-msvc` job is informational and red by design (MSVC is not a supported toolchain for the frontend).

**2026-10-20 — the stop is lifted and the state has moved.** The owner directed the upstream update: **`main` = `7bddb9e1` = `origin/main`**, upstream `v0.1.44.1` → **`v0.2.1.3`** (gitlink `cbfd5b92`), with the `RESUME`/`REUSED` protocol hotfix `01ad7226` immediately before it; every gate green at the new pin, host and (last time) CI. Owner also disabled GitHub Actions (remote = storage; see the 2026-10-20 owner-decisions block in `docs/AGENT_HANDOFF.md`), so the CI statements below describe history unless the owner re-enables. SP3 M1 Task 12 resumes from this base.

**2026-10-21 — SP3 M1 merged; SP3 M2 in flight.** M1 Tasks 12–23 ran on the re-anchored lane (`8520deee` re-anchor merge of main, then 12 commits to `11f22e47`) and were merged into **`main` = `b7fb97c9`** with the full gate battery on the merged tree: linux-gcc 690 run 100% (+4 env skips), linux-gcc-release preset 10/10 + `secure_distribution_check` PASS, windows-llvm-mingw-cross 0 warnings, pytest 25, root build + ctest 652 passed + 4 env skips 0 failed, and — first time — a controller-run **TSan** pass (clang-18 installed under `.toolchains/clang18` with wrappers in `.venv-dev/bin`; filtered concurrency suites 90/90, zero ThreadSanitizer reports). One lane defect fixed in-merge (release-preset `-O3` `-Werror=unused-result` in the new egress tripwire). Egress interposers ruled to stay separate (SP3 `egress_tripwire` for NET/EXT, SP4 connect-audit shim for DL). M1 ledger + final report archived in `docs/superpowers/handoff/reports/sp3-m1/`. M2 (17 tasks) launched 2026-10-21 on branch `sp3-api-streaming` re-created at merged main (base `f01ef654`), same worktree path; ledger `.superpowers/sdd/2026-10-14-sp3-m2-streaming/progress.md` records the host adaptations.

**2026-10-21 (later) — SP3 M2 merged; SP3 complete.** M2 ran 17 commits (`5d086223..847b5b9e`) with three §3.4 forced hand-backs, all recovered from the ledger; process fix: full-suite gates run synchronously in-turn, never pause-and-wait (the hand-backs were the lane ending its turn beside a backgrounded gate). Merged at **`main` = `c83119a9`**: linux-gcc 746/746, release preset clean after a merge-time dangling-pointer fix in `http_parser_test` (real UB, `-Werror=dangling-pointer` under `-O3` — second consecutive lane to miss the release build; it is now a per-task lane gate, see `briefs/subcontroller-common.md`), cross 0 warnings, pytest 25, root ctest 712 with one flake, TSan 111 cases **zero reports** with the same single flake. That flake (`log_line_format_and_rate_limit`) is pre-existing M1 token-bucket timing sensitivity (3/10 on pristine per the lane; 10/10 in isolation here; M2 did not touch bucket timing) — parked for the owner. `docs/superpowers/handoff/reports/sp3-m2/` holds the ledger. **Next milestone needs an owner decision: SP5, SP6 (blocked on the decision sheet, §5.1), or SP7.**

**2026-10-22 — SP5 designed, planned, and M1 merged.** Owner brainstorm resolved ten decisions (spec `2026-10-21-sp5-parity-typed-options-design.md`, D-1…D-10: full option-surface parity with the pinned upstream `v0.2.1.3` and Project Maya `v1.1.8` as the GLM reference — D-7 supersedes the 2026-10-14 A10 "no tuning knobs" ruling for scope; DFLASH stays rejected on the A7 licence (C-5); D-10 vendors Maya as a pinned read-only source in M2). Builder-check questions resolved with the owner; plan `2026-10-21-sp5-m1-strata-parity-cli.md` approved. M1 lane (`sp5-parity-cli`, 14 commits `a38704bf`, one §3.4 recovery after a cancellation mid-Task-4) merged at **`main` = `600e371c`**: the meta-tested parity table (every upstream key/env knob dispositioned + cited), rejected-keys-name-at-load + doctor env warning, families typed (sampling/reasoning/vram-gpu-elastic/timeouts-aliases/session/replicas-narrowing), lifecycle `lazy_load`/`idle_unload_s`/`mtp`, vision input path (`mmproj` manifest role, `VisionSupervisor`, data-URL-only carve-out C-9 with pre-decode caps), `strata serve`/`version`/`key`. Battery: 803/803 ctest, release 10/10, cross 0, pytest 26, distcheck PASS, root 769/769, TSan 117 cases zero race reports (same single parked flake). **SP5 M2 (Maya table, dialect layer, acceptance kit) needs its plan written next; SP6 decision sheet and the §5 parked items still wait on the owner.**

**2026-10-22 (later) — SP5 M2 merged; SP5 COMPLETE.** M2 plan `2026-10-21-sp5-m2-maya-parity-dialect-kit.md` approved; lane `sp5-maya-parity` ran 10 clean commits to `452954e1`, merged at **`main` = `073af122`**: Maya `v1.1.8` vendored read-only (`engines/maya-ref` @ `5c970a86`, zero CMake references — never built), the 178-row GLM parity table (all knobs dispositioned per D-7; DFLASH + updater rejected), `glm.*` config at Maya's names, closed typed GLM child-env allowlist, `EngineProtocolFlavor::maya_118`, manifest-composed GLM launch (pass-through retired), fake-engine Maya dialect e2e, and the SP5 acceptance kit (`docs/validation/sp5-parity-kit.md`, probe `--corpus/--compare/--dry-run`) with the PROVENANCE row a placeholder until the owner-box run. Battery: 825/826 ctest, release 10/10, cross 0, pytest 33, distcheck PASS, TSan 117 cases zero race reports, root 792/792. Flake data updated (§5 item 8). **Next: owner runs the acceptance + GPU kits (index at `docs/validation/README.md`); SP6 needs its decision sheet answered; SP7 unstarted.**

**2026-10-22 — SP7 designed; M1 merged.** Under the owner's "do not stop per block" directive the controller designed SP7 (spec `2026-10-22-sp7-release-packaging-service-design.md`, D-1…D-10 marked for one-line owner reversal; D-6 amended at planning — no serve lockfile exists; D-9 amended at M2 planning — the `--release` evidence schema already exists and is **SPDX**) and also **took up the SP6 decision sheet at its sheet values** (§5 item 1). The parked flake was **fixed** (test-side only; RED 3/3 TSan → GREEN 12/12 + first fully green full suite; `2af092d8`). M1 lane `sp7-m1-service-doctor` ran 7 clean commits to `161523fe` (one controller intervention: a hung RED case with no `TIMEOUT` registration was killed, self-corrected next task), merged at **`main` = `f4dff56d`**: `service enable|disable|status` (hardened unit golden text, never-elevate, no system-scope fallback, R-T6-a PX privilege split), `uninstall` (health-probe stop decision, typed purge word), full offline `doctor` (10 checks, `--deep` digests, human-fix lines with no install commands), schtasks XML golden (compile-proof only; Windows runtime still owner-side). Battery: linux-gcc 867/867, release 833/833, cross 0, pytest 33, distcheck PASS, TSan 133 cases zero reports, root 833/833. Ledger at `docs/superpowers/handoff/reports/sp7-m1/`. **SP7 M2 (release side) and SP6 Plan 1 plans are committed and queued — M2 dispatches immediately, SP6 Plan 1 after it.**

Local gates on `main` (dev host): linux-gcc **560/560** (+2 environment skips: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`), linux-gcc-release 10/10 labelled (526/526 full), wine 260/260, pytest 25, 0 warnings under `-Werror` on every preset.

**2026-10-22 — SP5-P2 parity refresh merged; pins certified.** After SP7 M2 (`e5a5327a`) the OWNER moved the pins (`upstream` → v0.2.2 @ `011c9e60`, `engines/maya-ref` → v1.1.21 @ `451f9cb6`) and shipped the chat page + Monitor himself; the Monitor "experts loaded" fix (his report) landed at `40628b15` (INFO/STAT capture → GenerationMeter → `/metrics engine.experts` whitelist → page lines; 955/955 + 913/913). Issue 27 (tables frozen at the old pins) became plan `2026-10-22-sp5-p2-parity-refresh-new-pins.md`; lane `sp5-p2-parity-refresh` ran Tasks 1–5 (controller takeover mid-Task-5; Tasks 6–8 controller-done) and merged at **`main` = `bf308b56`**: drift tripwire (meta-tests enumerate the CURRENT vendored trees), Maya table REGENERATED at v1.1.21 (183 typed/33 mapped/50 rejected/464 out), upstream citations at v0.2.2, GLM env allowlist + `glm.*` refreshed (146 names/127 rows), maya_118 wire certified (slot-mode rejects, ten-field DONE figures), launch base + `maya-vision` certified at the pin, penalty window-0 flavor rule. Battery: linux-gcc 965/965, release full 923/923, cross 0, pytest 65+1 skip, root 923/923, TSan filtered suites 122/123 with **zero ThreadSanitizer reports** (the one failure the parked load-sensitive `dos02_slow_reader`, 3/3 in isolation). **Next: SP6 Plan 1 → SP6 Plan 2; §5 parked items and the D-3 update-distribution gap still wait on the owner.**

**2026-10-23 — Gemma 4 merged and pushed (owner: "commit, merge and push to main").** Owner
asks of 2026-10-22: port Gemma 4 26B-A4B as a Strata-style engine behind the one 8080 UI (spec
`2026-10-22-gemma4-engine-design.md`, D1-D12) and "keep working on this until it is ready and can
serve the model for testing". Branch `gemma4-plan1-frontend` (plan 1 Tasks 1-12 by the controller,
a lane and two worktree workers; plans 2-3, `engines/gemma4/` `strata-gemma4-engine`, by the
controller) merged at **`main` = `0d60a2ea`** (tree identical to the tested branch tip
`d7557542`): frontend full suite 955 passed, 1 skipped (cfg03); engine ctest 26/26; parity with
llama.cpp `3cf03257` at its own CPU-vs-CUDA floor; served behind the page on the owner's box in
expert mode (session log steps 17-18, items 44-55; `docs/validation/gemma4/`). **Waiting on the
owner:** the CPU expert pool ruling (plans 2-3 R2), plan 4 (managed manifests, the gate opening,
the full acceptance battery), the glm53 page issue (session log item 46).

**2026-10-23 — Model launcher built on the local `main` (not pushed).** Owner: choose, configure
(MTP, KV streaming, ...) and start models from the page; then "Accept the recommendations for
D1-D10 in the model launcher spec; write the plan and start. Work on this to completion without
asking me anything else". Spec `2026-10-23-model-launcher-design.md` (ACCEPTED), plan
`2026-10-23-model-launcher.md`; commits `02ad1b02`, `3ad44536`, `07c4f8d9`, `0589af28` and the
docs commit after them, on the checked-out `main` (no branch could be switched to; **the owner
decides whether to push**). Two worktree workers (options, page) handed over patches. Full MSVC
suite 985 cases: 984 passed, 1 skipped (cfg03). Box: both cards (Qwen IQ3_S, Gemma Q8) started,
switched with different options and chatted from the page and the API (session log item 57);
left serving on `0.0.0.0:8080` with the Gemma card started. **Waiting on the owner:** push;
`models.control_remote` for the LAN; Gemma KV streaming (D8); GLM cards (D10).

**2026-10-23 — Model downloads on the Models tab, with pause and resume (local `main`, not
pushed).** Owner: O1-O4 (a card per Strata/Maya model with its status, the data drive's free
space and an insufficient-space refusal, a pasted Hugging Face link, Hugging Face and `.gguf`
only), D7 "Build packs automatically", then "Have a download pause and resume functionality".
Spec `2026-10-23-model-downloads-design.md` (ACCEPTED), plan `2026-10-23-model-downloads.md`.
Every transfer is a confined `strata pull` child; the server still links no network code (the
threat model's one accepted exception). Box run (a real Hugging Face pull, pause/resume, the
space refusal, the page) and enabling `models.downloads` on the owner's LAN server: session log
item 59. **Waiting on the owner:** push (`391997ee` onward).

### Is it deployable? (owner asked 2026-10-14; answer still holds)
No. The frontend side is strongly evidenced; the system is not. It has **never run against a real engine or GPU** (every test uses a fake engine). Streaming does not exist and the serving path is single-threaded until SP3 lands. The downloader exists but the catalog ships **empty** by owner decision (D5(D)), so v1 acquisition is import-from-developer-bundle. No packaging, installer, validation kit, or GLM engine. The first real answer comes from one session on the GPU box (§8).

---

## 2. Owner rules (binding on every agent; violating one is stop-and-report)

1. **Touch only what you are told to.** The owner's standing rule across all his projects: only systems and paths he names. Never run `acid start`/`acid stop` or `az`. Never flip repository visibility. Never trigger paid services ("do not do anything that will trigger spending"). GitHub Actions on this **public** repo are free and are used freely.
2. **`upstream/` is never modified.** Bumps are submodule pin changes recorded in `docs/PROVENANCE.md` with the runbook there.
3. **Nothing installed outside** `.venv-dev/` (pip cmake 4.4.3, ninja, pytest) and `.toolchains/` (llvm-mingw). Both are git-ignored and symlinked into every worktree; never `git add` them.
4. **Sub-agents never push, merge, rebase, or switch branches**; the controller integrates. Commits are local to the lane branch.
5. **Commit authorship and trailers**, exactly: `git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit …` with one trailer line written **literally** in every commit command (never via a shell variable): `Co-Authored-By: <model that wrote the commit> <noreply@anthropic.com>`. Sub-agent commits name the implementer's actual model (`Claude Opus 5.5` / `Claude Sonnet 5.5`); commits the controller makes itself name the controller's model (`Claude Fable 5.1`). *(Owner 2026-10-27: the GitHub noreply address as the author, no `Claude-Session:` line, no session links, and no identifying data in any commit - names, handles, e-mail addresses, PC or host names, LAN addresses or network settings.)* **Existing trailers are never rewritten** (owner: "keep trailers as-is", no force-push).
6. **The permission guard** blocks `rm -rf`, `git clean -fd*`, bypass flags, and sometimes rebase-conflict edits. Use unique directories and `find <dir> -delete`; retire worktrees by `mv` into the scratchpad then `git worktree prune` (git refuses to remove worktrees containing a submodule).
7. **Handoff discipline:** `docs/AGENT_HANDOFF.md` is updated as part of every change; proof status never lives only in chat. Never claim GPU or Windows behaviour verified from this host: Windows claims come only from the `windows-msys2-clang` CI job (wine is smoke); GPU claims come only from the owner's box. *(Owner update 2026-10-20: Actions are disabled — GitHub is storage; Windows runtime proof deferred to the owner's own hardware, and the green `windows-msys2-clang` run at `7bddb9e1` is the last CI-authorized Windows proof. Agents prove only the local `windows-llvm-mingw-cross` cross-build.)*
8. **`-Werror` everywhere; TDD per task with RED evidence in the report.** Tests never get weakened to pass; a wrong expectation is changed only with a written ruling.
9. **Commit and push after verified tests** (owner memory rule) — once the gates pass, the controller commits and pushes without being asked. (Suspended by the stop order until the owner resumed; since 2026-10-20 the push is **save-only** — no CI watching, no awaiting runs.)
10. **Workers are checked** (owner standing rule, 2026-10-30: "use workers with skills if you have to but you need to check their work and you also need to check with them every ten minutes to make sure they are not stuck", then "make that a standing rule"). A worker's result is verified (its tests rerun, its diff read) before it is reported or committed, and every running worker is checked on about every ten minutes; one that is stuck is stopped or redirected.

*(Owner update 2026-10-25 — the opencode worker: the current controller is the **`opencode` worker**, not the Claude worker line. It runs on Linux with **no GPU and no Windows** — unlike the Claude worker it was never on the owner's GPU box and cannot be. It identifies itself as the opencode worker in handoff documents; its commits carry `Co-Authored-By: Qwen 3.8 Flash Next IQ3_S <noreply@anthropic.com>`, no `Claude-Session:`. With this worker the focus is **porting the engines without full live verification**: builds, cross-builds and unit proofs only; every GPU/Windows runtime claim still waits for the owner's hardware, exactly as rule 7 has always required — but now with no possible exception from this host. The box also has **no model files** — ports rely on the upstream code (the vendored `upstream/` + `engines/maya-ref/` checkouts) as the behavioural reference, and tests run on synthetic fixtures only.)*

---

## 3. How the work was run (the agent instructions)

### 3.1 Controller model
One **controller** (this document's author, Claude Fable 5.1 in the original session) orchestrates with the `superpowers` skills: `brainstorming` → spec, `writing-plans` → plan, `subagent-driven-development` → execution, `finishing-a-development-branch` → integration. The owner scaled it ("keep working and speed up development using workers"; "let the workers spawn additional workers") into:

- **Sub-controllers**, one per plan, each a `general-purpose` agent on **Opus** (alias `opus`), running the full `subagent-driven-development` loop in **its own git worktree** under `.worktrees/<lane>` on **its own branch**: implementer → task reviewer → fix rounds (≤5) → scoped re-review → final whole-branch review with one fix dispatch → final report. They decide conflicts themselves (spec binding, plan its argument), record `Ruling: … — why — cost if wrong` in a ledger, and stop only for the skill's four reasons.
- **Implementers/reviewers** on `opus`; pure transcription or single-file mechanical fixes on `sonnet`. Implementers may spawn **read-only** helpers (Explore / read-only general-purpose) but never a second editor of the same tree nor a reviewer of their own work. Never two implementers in one worktree.
- **The controller** owns: pre-flight reviews of plans, cross-plan rulings, the trial-merge gate, pushes, CI watching, owner communication, memory. It does not implement except for small hotfixes (the PP-line fix was one).

### 3.2 Briefs (verbatim copies in `docs/superpowers/handoff/briefs/`)
Every sub-controller received two files: `subcontroller-common.md` (project facts, tooling, owner rules, models, report format) and a plan-specific brief naming plan, spec, worktree, HEAD at launch, controller-verified baseline counts, scope, carry-ins from other lanes, and the final-report path. **Keep this pattern**: exact values live in the brief file, the dispatch prompt only points at it. Briefs preserved: `subcontroller-sp1.md`, `subcontroller-sp2.md`, `subcontroller-sp2-run2.md`, `subcontroller-sp4.md`, `subcontroller-sp4b.md`, `subcontroller-sp3m1.md` (the one to reuse), `winfix-brief.md`, `merge-sp4-brief.md`; plus `owner-decisions-2026-10-14.md`, `sp2-run2-handoff-items.md`, `sp3-m1-reanchor-notes.md`, `sp3-plans-preflight-review-1.md`.

Note: `subcontroller-common.md` still says the submodule pin is v0.1.43 in one line; the plan-specific brief (v0.1.44.1, `296b59d2`) is authoritative.

### 3.3 Integration gate (what the controller does after a lane reports COMPLETE)
1. `git worktree add --detach <scratchpad>/merge-<lane> main`; `git merge --no-ff --no-commit <branch>`; resolve conflicts (an Opus "merge resolver" agent with a brief did the 9-file SP4 merge — see `reports/fixes/merge-sp4-report.md`); `git submodule update --init -- upstream`; symlink `.venv-dev`/`.toolchains`.
2. Gates on the merged tree: `cmake --preset linux-gcc && cmake --build --preset linux-gcc && ctest --preset linux-gcc`; `linux-gcc-release` preset (hooks OFF, `-L "release-only|release-also"`); `windows-llvm-mingw-cross` build (0 warnings) and `WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml= ctest --preset windows-llvm-mingw-cross` from `secure_frontend/`; `pytest -q` from the root. Linux suites run **serially** (socket tests use fixed ports 20000–20099).
3. Commit the merge with gates in the message; `git merge --ff-only` in the main checkout; `git push origin main`; `gh run watch -R shift-f10/multi-strata` (always `-R`: plain `gh` resolves to the `upstream` remote); download the `windows-msys2-clang` log on red and open a fix lane. CI runs on **any branch push** (path-filtered), so a lane branch can be pushed for a real-Windows check before merging. *(Since 2026-10-20 Actions are disabled: skip the `gh run watch` and log-download parts; the push is the save, and the `windows-llvm-mingw-cross` build gate stands in for Windows until the owner runs real Windows himself.)*
4. Retire: `mv .worktrees/<lane> <scratchpad>/stale-worktrees/…`, `git worktree prune`, `git branch -d`, `git push origin --delete <branch>`.

### 3.4 Known harness behaviour
Sub-controllers were repeatedly **force-handed-back** mid-run by the harness (SP4 at Task 7, SP4 part 2 at Task 26, SP3 M1 at Tasks 6a, 8b, 11). Their in-flight implementer kept running and its report arrived at the controller instead. Recovery that worked every time: `SendMessage` to the sub-controller's agent id with "resume exactly from your ledger; reconcile children first; never redispatch the in-flight implementer; here is its report". The ledger (`progress.md`) is the recovery map; all are preserved under `reports/`.

---

## 4. Decisions already taken (do not re-litigate)

Owner, 2026-10-14 ("approve all and continue building") — full text in `briefs/owner-decisions-2026-10-14.md`, written into the specs:
- **SP6 (A1–A11):** evaluate both Maya (project-maya v1.1.8 @ `5c970a86`) and glm53f (@ `4894c996`) on the NVIDIA box, lean Maya; Windows in scope; MSVC v143 only as nvcc host for the engine; NVIDIA only; GGUF IQ/K + Maya quants; Maya serve/dashboard/updater/MCP/quant tooling out; **DFlash2 (CC BY-NC-ND) never**; single GPU first; engine as submodule, no local patches; env allowlist forces slots off and excludes DFLASH, no tuning knobs; loop guard off.
- **SP3 (B1–B12):** `max_tokens` omitted → auto; clamp; U+FFFD + tail drop; timeout → partial text `length`; chunked; neutral no-op + bounded temperature/top_p/seed; `/v1/completions`, `/v1/messages`, Ollama API out; CORS off with exact-origin opt-in `listener.cors_origins`; 8 workers, queue min(4, max(0, w−2)), 300 s, per-remote cap min(4, w); loopback residual accepted.
- **SP4 (C1–C7):** confined system-curl child; built-in digest catalog + `--trust-manifest`; curl floors 8.4.0 Win / 7.81.0 Linux; Qwen GSQ-RCO iq2_xs/iq3_xxs/iq3_s + Coder iq1_m but **catalog ships EMPTY** under D5(D) import-from-developer-bundle; `run` on a missing model = in-process pull after confirmation; no signature in v1 (Ed25519/SHA-512 → SP7); `update` exits 12.
- Task 22 (SP2) stdin-write bound; upstream bump to v0.1.44.1; rebase-conflict resolution approved.

Controller rulings of note (each ledgered with cost-if-wrong in the lane reports): D-2 uncounted client-STOP restarts bounded at `kMaxConsecutiveUncountedRestarts = 3`; `WinLaunchSpec` Job limits default ON, engine opts out; `EngineFamily` (SP2 env) vs `ManifestFamily` (SP4) rename; `compose_engine_launch` renders the typed `engine.gpu` list; Windows Job counts exclude `%SystemRoot%\System32\conhost.exe`; Windows handle-leak baseline taken at generation 2; link-set files written LF; Strata-profile `PP` heartbeats accepted (3–5 fields, two decimal counters, optional metrics, no ordering), GLM keeps its strict record.

Owner-visible behaviour changes already on the **SP3 branch** (not yet on main): 411 for Transfer-Encoding, 413 for oversized Content-Length, 404 `model_not_found`, 503 `server_shutting_down`, `invalid_json` for content-part arrays and deep tools bodies, `max_tokens` auto + clamp, `request_deadline_ms < 10000` requires `header_deadline_ms`, wildcard bind requires `host_aliases`, default 8 workers, `/v1/models` lists only the served model, listener and accepted sockets no-inherit (closes SP2 gap K4).

---

## 5. Waiting on the owner (none blocks the GPU session)

1. **SP6 decision sheet** (presented 2026-10-14 08:41; **taken up by the controller 2026-10-22 under the owner's standing "do not stop per block" directive** — the sheet values below are adopted as controller rulings, owner can reverse any one with a single line, exactly like SP5's C-n and SP7's D-n rulings; SP6's plan is now writable): S1 default pull `maya-s`; S2 include Maya-S24; S3 glm53f → keep pin + transcripts only; S4 fallback order; S5 `STRATA_GLM_SLOTS=0` only (prefix-reuse side channel noted); M1 Maya shipped flags as constants; M2 amend A10 for one typed `STRATA_GLM_RAM_HEADROOM_GB` default 16; M3 hand-built release engine accepted v1; M4 dev host = confined CPU configure+build only; M5 `STRATA_WATCHDOG_S=0`; M6 no-network-imports gate; M7 glm53f load-only.
2. **Delegated-cgroup live proof** on the dev host needs `systemd-run --user --scope -p Delegate=yes` (SP2 C5/C3; verified by reading only).
3. **Confirm the F-25 digest pins** for iq3_xxs, iq3_s, iq1_m before release (SP4 part 1), and **check `docs/PROVENANCE.md`**, which shows the **same shard-2 digest for all three Qwen quants and the Coder repo** — possibly a shared shard, possibly wrong; verify at source before trusting any pull.
4. Ratify two SP2 spec-vs-code rulings: C3 grandchild → membership assertion; K4 sockets (now closed on the SP3 branch).
5. SP1 `deployment.md`: the Windows token caveat can be removed.
6. The product-spec CLI verbs `serve`, `version`, `key` have **no owner** (SP4 created `strata` with its verbs only); assign to SP3 M2 or SP5.
7. Whether to resume SP3 M1 before or after the GPU session. *(Resolved: resumed 2026-10-20; M1 and M2 both merged 2026-10-21.)*
8. Pre-existing test flake `secure_frontend_api_server_tests.log_line_format_and_rate_limit` — **RESOLVED 2026-10-22** (controller fix, test-side only; full RED/GREEN record in `docs/validation/README.md`): readiness-gate boot polls (`health 503`) and pool eviction lines (`other 0 evicted`) are accepted as the real second line shape, and the wave (6x80) guarantees bucket overflow under TSan slowdown; no invariant weakened. RED 3/3 TSan, GREEN 12/12 TSan + first fully green full suite (826/826). The two stall-volume `windows-runtime` streaming cases plus the GPU kit runs (index: `docs/validation/README.md`) still wait on the owner's hardware.

---

## 6. Exact resume procedure (when the owner says go)

1. Read this file, then `docs/AGENT_HANDOFF.md`, then the memory file if you are the same assistant (`/home/<user>/.claude/projects/-opt-projects/memory/project_multi_strata.md`).
2. Verify state: `git -C /opt/projects/ai status --short` clean; `git log --oneline -1 main` = `bd3b53a6`; `git worktree list` shows `.worktrees/sp3` at `9d30088b`; `git submodule status` = ` 296b59d2… upstream (v0.1.44.1)`; `.venv-dev` and `.toolchains` present (symlinked in the worktree).
3. **Resume SP3 M1:** launch a new Opus sub-controller with `briefs/subcontroller-common.md` + `briefs/subcontroller-sp3m1.md` plus this instruction: *"Resume from the ledger at `.worktrees/sp3/.superpowers/sdd/2026-10-14-sp3-m1-concurrency-hardening/progress.md` (copy in `reports/sp3-m1/`): Tasks 0–11 are complete at `9d30088b`; start with Task 12 from BASE `9d30088b`. Task 12 must prove: every admission call runs under the pool mutex; an evicted worker sees its abort within 20 ms; listener + linger entries stay within 128 poll entries. Carry the 37 rulings and 36 deferred minors listed in FINAL-REPORT.md. The task briefs 12–23 are already in the workspace."* If the lane workspace is gone, the sub-controller regenerates briefs from the plan with `scripts/task-brief`. Expect the forced hand-back; recover per §3.4.
4. Merge M1 via §3.3. Expected conflicts with main: none large (main did not move on SP3's surface after c9629e6e), but **consolidate the two egress interposers** (SP3 Task 19's own `LD_PRELOAD` interposer vs SP4's connect-audit shim) — ruled to be done at merge. Then push, CI, and update `AGENT_HANDOFF.md` to state the SP3 behaviour changes.
5. **SP3 M2** (streaming): plan on main; brief pattern as M1; it greps M1's seam names `RouteResult`, `StreamSink`, `HttpHead`, `StreamHandler`, `serialize_head`, `deliver_route_result`, `SocketStreamSink`.
6. **SP6 plan 1** only after the owner answers the sheet (§5.1); then SP5/SP7 specs via `brainstorming`.
7. Also queued: PX addendum reconciling `http.*` → `listener.*` names and `STRATA_CONFIG`; parked test-infrastructure items (fixed ports 20000–20099 collide under `ctest -j`; `make_private_directory` accepts an existing dir unchecked; `/tmp` fallback without `SECURE_FRONTEND_TEST_TMP`; stale `test-tmp` dirs after SIGKILLed runs; wine 0.01 s start-up flake).

---

## 7. Repository map (what to read for what)

- `docs/AGENT_HANDOFF.md` — code-level truth: every deliverable, proof status, known gaps. Updated by every lane.
- `docs/PROVENANCE.md` — upstream pin, bump runbook, digests. `upstream.version` (root) is the engine-version source.
- `docs/security/threat-model.md`, `docs/security/engine-protocol-limits.md` (engine wire facts incl. the `PP` row), `docs/secure-frontend-ci.md`, `docs/product-spec*` (PX), `tests/security/README.md` (acceptance IDs NET/AUTH/DOS/WEB/CFG/DL/PRIV/PROC/EXT).
- Specs: `docs/superpowers/specs/2026-10-13-sp0-*`, `…-sp1-*`, `…-sp2-portable-supervision-design.md`, `2026-10-14-sp3-api-concurrency-streaming-design.md` (APPROVED, D-1/D-2 re-anchored), `2026-10-14-sp4-acquire-design.md` (APPROVED), `2026-10-14-sp6-glm-engine-design.md` (DRAFT), `2026-10-13-upstream-submodule-layout-design.md`.
- Plans: `docs/superpowers/plans/…sp3-m1-concurrency-hardening.md`, `…sp3-m2-streaming.md`, `…sp4-acquire.md`, earlier SP0/SP1/SP2.
- Research: `docs/superpowers/research/2026-10-14-sp6-glm-engine-sources.md` (Maya vs glm53f facts; project-maya is the second SP6 source per the owner).
- This directory `docs/superpowers/handoff/`: `briefs/` (§3.2), `reports/` (SDD final reports and ledgers for SP1, SP2 run 1 and 2, SP4 part 1 and 2, SP3 M1 to Task 11, the Windows fix lane, the SP4 merge resolver, the SP0 ledger).
- Code: `secure_frontend/` (presets in `CMakePresets.json`: `linux-gcc`, `linux-clang`, `linux-gcc-release`, `windows-llvm-mingw-cross`, `windows-msys2-clang` CI-only; tests register per case through `secure_frontend_add_cases`, label `windows-runtime` for real-Windows-only cases). Binaries: `strata-unified-server`, `strata` (SP4 verbs). CI: `.github/workflows/secure-frontend.yml`, Linux jobs pinned `ubuntu-24.04`.

---

## 8. First GPU-box session checklist (the first real test of the system)

1. Build the upstream engine there (CUDA; Windows needs MSVC v143 as nvcc host — the frontend itself stays llvm-mingw/MSYS2). Hand-place engine, model shards and tokenizer pack (catalog is empty by design; or build a developer bundle with `make_bundle.py` and `strata import --trust-manifest`).
2. `strata-unified-server --config <file> --expert` with `--pack` and the weight flags in `engine.args`; one `POST /v1/chat/completions`. The `PP` heartbeat fix (c9629e6e) is the known first-request blocker that was fixed by reading; confirm it live.
3. Check `qwen35moe` family behaviour if that model is used (different serve loop: no QUIT, unknown flags skipped, no `--pack`, no `+8` margin — `engine-protocol-limits.md`).
4. Run the cgroup-delegation proof if the box is systemd (`systemd-run --user --scope -p Delegate=yes`) and the SP2 `proc05_cgroup_kill_when_delegated` case.
5. Measure STOP latency during a long prefill (SP3 risk R4) and the READY time against `ready_timeout_s` (900 s default).
6. Record every outcome in `docs/AGENT_HANDOFF.md` ("Verification performed", GPU column) — the first time any GPU row can be marked verified.

---

## 9. Lessons that cost time (do not repeat)

- Never `git merge … | tail` inside an `&&` chain (the pipe hides the failure).
- `git submodule deinit` in **any** worktree removes the shared `.git/config` submodule entry (repair: `git submodule init` in the main checkout).
- Fresh worktrees need `git submodule update --init -- upstream` (clones from the shared module store, no network) and the two symlinks.
- Windows CMake `file(WRITE)` writes CRLF; Windows `main(argc, argv)` is ANSI (now fixed with the shared `secure_frontend_utf8_argv` library); `CREATE_NO_WINDOW` spawns a `conhost.exe` inside the Job; `%TEMP%` on runners is an 8.3 path.
- A sub-controller's "COMPLETE" is not a merge gate: the trial merge found semantic conflicts (dropped GPU list, duplicate `EngineFamily`) that only compile-and-test exposed.
- The harness lists finished subagents as present until the session ends; `ps` is the truth for live processes.
- A worker whose vitals (commits/dirty/ledger mtime) are static for 20 minutes is NOT proven dead — the SP5-P2 lane was thinking while a watchdog flagged it; replacing it produced a DOUBLE WRITER in the worktree (a controller `git add -A` then swept the live lane's in-flight files into a mislabeled commit — pushed, so only label-mappable after the fact). Before replacing: check the harness session log with the CORRECT timestamp pattern, and never `git add -A` in a worktree that may have another writer.
