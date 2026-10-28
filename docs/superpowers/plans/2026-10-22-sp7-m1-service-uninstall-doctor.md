# SP7 M1 plan — service verbs, uninstall, full doctor

Spec: `docs/superpowers/specs/2026-10-22-sp7-release-packaging-service-design.md`
(D-1, D-5, D-6 amended, D-7). Base: `main` @ spec commit. Branch `sp7-m1-service-doctor`,
worktree `.worktrees/sp7m1`. Rules per `briefs/subcontroller-common.md`: per-task gates
(linux-gcc build + serial ctest, linux-gcc-release build, windows-llvm-mingw-cross build,
`pytest -q`) synchronous in-turn; TDD RED first; `-Werror`; no pushes.

Context facts (verified at planning): `strata_cli.cpp` grammar table at ~:528; `doctor`
currently routes through `run_network_verb` (SP1 base checks); NO server lockfile exists
(D-6 amendment); token/config/state roots come from SP1 typed config (`paths.*`);
`api_client` can do the loopback probe; engine profile carries GPU/driver expectations
from the manifest (SP4).

## New seams (binding)

- Lib `secure_frontend_service_ops` (`src/service_ops.{hpp,cpp}`, namespace
  `secure_frontend::service`): `render_systemd_unit(const ServiceUnit&)` → exact unit text;
  `plan_enable/plan_disable/plan_status(ServiceEnv, CommandRunner&)` where `CommandRunner`
  is `std::function<CmdResult(const std::vector<std::string>&)>` (tests use a recording
  runner + a PATH shim `systemctl`; production wires argv exec via the existing process
  helpers). Windows side: `render_schtasks_create(const ServiceUnit&)` text only.
- Lib `secure_frontend_doctor` (`src/doctor_checks.{hpp,cpp}`): each D-7 check is a pure
  `check_x(ProbeBundle&) -> CheckResult{name, ok, human_fix}`; `ProbeBundle` injects
  filesystem stats, `/proc` reads, PATH probes, port bind, config load result — every
  check unit-tested with fakes; the verb composes them.
- Lib `secure_frontend_uninstall` (`src/uninstall_plan.{hpp,cpp}`): `plan_uninstall(...)`
  → ordered removal list (paths only under allowed roots) + server-stop decision
  (`none | via_service_manager | foreground_running_refuse`), from the health probe and
  unit-file presence. Execution reuses it; `--purge` requires a typed confirmation token
  that `--yes` cannot satisfy.

## Tasks

1. **service_ops: unit rendering.** RED: exact-text golden test of the hardened unit
   (Directives per D-5 verbatim: NoNewPrivileges, ProtectSystem=strict,
   ProtectHome=read-only, ReadWritePaths of the three roots, PrivateTmp,
   RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6, Restart=on-failure, RestartSec=5,
   ExecStart=resolved current target `serve`); negative: paths with spaces/newlines
   rejected. GREEN: implement.
2. **service enable/disable/status.** RED: with fake CommandRunner — enable = write unit
   atomically (0644) then `systemctl --user enable --now strata`; missing user manager
   (runner reports failure on `--user is-enabled` probe) → unit NOT written, non-zero,
   explanation incl. `loginctl enable-linger` note (never run). disable =
   `disable --now` then remove ONLY our exact unit path; foreign unit path → refuse.
   status = one human line each for enabled/active + rendered task text on the Windows
   branch. GREEN: implement; PATH-shim `systemctl` e2e in a temp HOME.
3. **doctor full checks.** RED (unit, faked ProbeBundle): gpu/driver/CUDA-runtime
   presence + driver-minimum vs profile, config-validity named-key report (reuse SP1
   loader result), token file mode ≠ 0600/0400 → fail, dir canonicality/perms (CFG
   rules), port free by bind probe, LimitMEMLOCK vs profile need, `--deep` digest hook
   (test with tiny fake model). Human-fix strings never contain commands that install.
   GREEN: implement + compose in the verb; keep `--downloader`; exit 0 iff all pass.
4. **uninstall plan + execution.** RED: health-probe decision table (no listener →
   proceed; 401/200 + our unit file present → stop via service manager first; listener
   with no unit → refuse with Ctrl-C message, exit non-zero); removal list = unit file,
   token, config, state dir; models/logs kept + printed paths; `--purge` typed-confirm
   gate; all paths under allowed roots (escape attempts → refuse). GREEN: implement +
   e2e with fake engine server up/down.
5. **CLI wiring.** Grammar entries `service {enable|disable|status}`, `uninstall
   [--purge]`; usage text updated; exit codes; `doctor` flags `--deep`. e2e: each verb
   against temp roots (pattern: `server_cli_test.cpp`).
6. **Windows branch compile-proof.** `render_schtasks_create` golden test (host-runnable
   text assertions), windows-runtime-labelled e2e for the Task Scheduler runner path;
   cross preset stays warning-free. No Windows runtime claims.
7. **Truth pass.** `docs/AGENT_HANDOFF.md` deliverables row; spec amendments folded in;
   handoff report scaffold. Final full battery + merge gate by the controller on a
   scratch merge tree, per §3.3.

## Interfaces to SP7 M2 (do not build here)

Ed25519, release manifest, `strata update` body, packaging, SBOM, `--release` check,
offline kit — M2 plans after M1 merges. The `current`-target resolution helper from Task 1
(`resolve_current_target()`) is the seam M2's update will write through later.
