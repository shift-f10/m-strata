# SP7 M1 (service verbs, uninstall, full doctor) — FINAL REPORT

Lane: `sp7-m1-service-doctor`, worktree `/home/opencode/projects/multi-strata/.worktrees/sp7m1`, base `9b467e08` (plan commit on `main`).
Upstream pin unchanged — read-only, **zero** upstream files touched.

Status: **all 7 plan tasks complete**. One commit per task — `5ad06ab4, 2e314fed, 51fa1065,
569186b0, 1fa0c4af, 84a99b00` — plus this docs commit (HEAD). Every commit authored
`shift-f10 <40450442+shift-f10@users.noreply.github.com>`, carries
`Co-Authored-By: Qwen 3.8 Flash Next IQ3_S <noreply@anthropic.com>`, and — per the owner's
2026-10-20 ruling as adapted for this lane host — no `Claude-Session:` trailer.

Lane ledger (RED one-liners, gate lines, every ruling R-T1-a…R-T6-d):
`.superpowers/sdd/2026-10-22-sp7-m1-service-doctor/progress.md` — **untracked**: `.gitignore`
ignores `/.superpowers/` deliberately and no lane ever tracked it (R-T1-d); flagged for the
controller since the lane brief said otherwise.

## What was built (task → deliverable, one line each)

1. `secure_frontend_service_ops` — exact hardened systemd user-unit text (D-5 directives
   verbatim; `ReadWritePaths` = state + logs + cache, the serve write-targets, R-T1-a),
   whitespace/relative-path rejection, and the `resolve_current_target` `current`-pointer
   seam (D-4) M2's update will later write through;
2. `run_argv` (fork/execvp + pipe pump; CreateProcessW on Windows) and
   `plan_enable/plan_disable/plan_status`: atomic 0644 unit write, `systemctl --user
   enable --now`, "no user manager" refuses before writing (linger explained, never run),
   disable removes **only** `~/.config/systemd/user/strata.service`, status = two read-only
   probes, one line each;
3. `secure_frontend_doctor` — every D-7 check pure over an injected `ProbeBundle`
   (gpu / driver / CUDA / config / token / roots / port / memlock, model digests only under
   `--deep`), fixed check order, `doctor: all checks passed` / `one or more checks failed`,
   exit 1 on any failure, fix lines install nothing (PX §8);
4. `secure_frontend_uninstall` — the D-6 planner: ordered removals (unit, token, config,
   state; models only under `--purge`) bounded by allowed roots, the health-probe stop
   table (no listener → proceed; answering + our unit + reachable manager → stop via the
   service manager; answering + no unit → refuse with the Ctrl+C line), and the typed-word
   gate `--yes` cannot satisfy; `server_answers_on` = loopback `GET /health`, any HTTP
   answer (a 401 included) means answering;
5. CLI wiring — grammar `service {enable|disable,status}`, `uninstall [--purge]`, usage
   text, exit codes, unit dir from `XDG_CONFIG_HOME`/`HOME` (R-T2-a), `ExecStart` through
   `resolve_current_target(self)`; 7 e2e cases spawn the real `strata` binary against temp
   HOME/XDG roots with a fake-systemctl PATH shim;
6. Windows branch, compile-proven only — `render_schtasks_create` golden XML ("At log on",
   `RunLevel LeastPrivilege`, `<Priority>6</Priority>`, PT5M×3 restart), the plan branches
   wired to `schtasks /Query|Create /F /XML|End|Delete /F`, task file pinned to
   `<config home>/strata/strata-task.xml` with the same foreign-path refusal,
   windows-runtime-labelled cases registered for the owner's box; **no Windows runtime
   claim made**;
7. truth pass — spec amendments folded in (D-6 rewritten to the health probe; D-5 records
   the upstream-two-settings split per PX §6; D-7 notes the compiled-in per-profile
   expectations), this report, and the `AGENT_HANDOFF.md` SP7 M1 section.

## Final gate battery (this host, 2026-10-22, branch tip)

| Gate | Result |
| --- | --- |
| `cmake --build --preset linux-gcc` | 0 warnings (`-Werror`) |
| `ctest --preset linux-gcc` (serial) | 100% — 870 registered / 867 run / 4 env skips (cfg03 mount-crossing, proc03 other-uid, proc05 cgroup-kill, bundle-at-mount-root) |
| `cmake --build --preset linux-gcc-release` | 0 warnings |
| release full `ctest` (unfiltered) | 100% — 833/833 incl. the release-only proofs, same 4 skips |
| `cmake --build --preset windows-llvm-mingw-cross` | 0 warnings — Windows **not run** (owner-hardware rule) |
| `.venv-dev/bin/pytest -q` | 33 passed |
| per-task RED evidence | in the ledger (compile-failure RED for new seams; behavioral RED for amendments) |

Gate-method note for the controller: the presets keep build trees in
`secure_frontend/out/build/<presetName>`; the debug CTest preset excludes the 3
`release-only` label cases — a bare `ctest --test-dir <debug-dir>` runs those two
self-guarding release proofs and shows two false failures. Use `ctest --preset <p>`.

## Rulings the controller should see

- **R-T1-d**: the ledger could not be committed (`.gitignore` ignores `/.superpowers/`);
  resolved untracked, brief text noted as inconsistent.
- **R-T1-c**: plain (non-versioned) installs honestly resolve to their own path; the
  "not a versioned install" refusal belongs to `strata update` (M2), not the service verbs.
- **R-T3-a**: driver/GPU/CUDA/memlock expectations are compiled in per profile — the
  shipped manifest carries none (plan's context line was wrong for this base; spec D-7
  amended).
- **R-T3-b**: three legacy doctor tests asserting a fixed exit 0 were amended to verdict
  equivalence — D-7 replaces that contract by design; no probe assertion weakened.
- **R-T6-a**: upstream's task-settings pair splits under PX §6 — Priority Normal taken,
  "run with highest privileges" refused (never elevates); the tension is recorded in spec
  D-5 rather than silently resolved. Windows memlock is reported not-applicable (R-T3-c).
- **R-T5-b**: the uninstall install-metadata gate passes honestly in M1 (no metadata
  exists yet; M2 feeds it).
- Test-support change shared with other lanes: `ProcessSpec` gained an optional
  `stdin_file` (both platform branches); `CommandRunner` fakes must be wrapped in
  `std::ref` (R-T2-e).

## Parked / owed to the owner and to M2

- Windows runtime acceptance (task XML import, schtasks flows, the `schtasks.cmd`-shim
  e2e, the windows-runtime-labelled cases): runs on the owner's Windows box — nothing here
  claims it ran.
- GPU-dependent doctor lines: proven with fakes here; real values on the owner's GPU box.
- `--purge` currently removes models (D-6 wording); the versions/`current` tree and
  downloads cache belong to the M2 layout and will join the purge list with it.
- The `current`-pointer write side (installer, `strata update`) is M2; M1 only resolves
  through it.
