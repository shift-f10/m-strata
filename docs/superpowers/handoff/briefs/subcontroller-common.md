# Sub-controller brief — common rules (Secure Strata / multi-strata)

> **Read `docs/superpowers/handoff/2026-10-16-controller-resume.md` (in the repository) before this file.** It is the controller-level state and the owner's rules; this brief and your lane brief are read-only in `docs/superpowers/handoff/briefs/` and are superseded by the lane brief the controller gives you at launch (e.g. the submodule pin is now v0.1.44.1 `296b59d2`, not the v0.1.43 line below).

You are a sub-controller. You run ONE implementation plan end to end with the
`superpowers:subagent-driven-development` skill, in the worktree named in your
plan-specific brief, and report back to the top-level controller. You do not
talk to the owner; the top-level controller does.

## First actions, in order
1. Invoke the Skill tool: `superpowers:subagent-driven-development`. Follow it
   exactly (workspace script, ledger, task-brief script, review-package script,
   implementer → task reviewer → fix rounds → scoped re-review, final
   whole-branch review with ONE fix dispatch). Narrate at most one line between
   tool calls.
2. `cd` into your worktree and confirm `git rev-parse HEAD` equals the HEAD your
   plan brief states, `git submodule status` shows ` 7695859aa13afb248bdb4d36f9b2e95707db079d upstream (v0.1.43)`
   with no leading `+`/`-` (NOTE: the pin is now ` 296b59d2… upstream (v0.1.44.1)`; the v0.1.43 line above is stale — your plan brief is authoritative), and `.venv-dev` / `.toolchains` exist (symlinks,
   shown as untracked — never add them).
3. Read the plan once, then the spec it names. Do the pre-flight conflict scan
   the skill requires and write the table to the ledger.

## Project facts you need
- Dependency-free C++20 security frontend in `secure_frontend/`; upstream Strata
  is the pinned, NEVER-modified git submodule `upstream/`. Root `CMakeLists.txt`
  is ours and is not in your plan's scope.
- Tooling: `export PATH="$(git rev-parse --show-toplevel)/.venv-dev/bin:$PATH"`
  (cmake 4.4.3, ninja, pytest). Presets in `secure_frontend/CMakePresets.json`:
  `linux-gcc`, `linux-clang`, `windows-llvm-mingw-cross` (cross-compile +
  wine smoke), `windows-msys2-clang` (CI only). Routine Python tests:
  `"$(git rev-parse --show-toplevel)/.venv-dev/bin/pytest" -q` from the
  worktree root → 13 passed today.
- Wine: every direct `wine …` and every `ctest --test-dir out/build/windows-llvm-mingw-cross …`
  is prefixed with `WINEPREFIX=$PWD/out/wineprefix WINEDLLOVERRIDES=mscoree,mshtml=`
  (from `secure_frontend/`). wine is a smoke test; the `windows-msys2-clang` CI
  job is authoritative. Record wine results in commit messages as the plan says.
- Test registration (shared pre-task, already on your branch): every test
  binary dispatches `--case <name>` through `secure_frontend_test::run_cases`
  (`secure_frontend/tests/support/test_support.hpp`), registered with
  `secure_frontend_add_cases(<binary> <case>... [ARGS ...] [LABELS ...] [TIMEOUT s])`
  → one CTest test per case named `<binary>.<case>`; `LABELS ${SECURE_FRONTEND_WINDOWS_RUNTIME}`
  for real-Windows-only cases; wine preset excludes label `windows-runtime`.
  Baseline before your Task 1: linux-gcc 111/111, wine 102/102.
 - **Per-task gate discipline (2026-10-21, after both SP3 lanes):** per task build
   BOTH `linux-gcc` and `linux-gcc-release` (hooks OFF, `-O3`), not just linux-gcc +
   cross — `-O3` catches classes both lanes missed until the merge gate
   (`-Werror=unused-result` in M1, `-Werror=dangling-pointer` in M2), since CI no
   longer runs. Full-suite gates run synchronously in-turn, never background-and-pause.
- Known parked item: socket-server tests use fixed ports 20000–20099 and
  collide under `ctest -j`; presets/CI run serially. If a task of yours
  touches that fixture, prefer ephemeral port 0.
- Dev host umask is 0002 → use `make_scratch_directory`/`make_private_directory`,
  never `std::filesystem::create_directories`, for test dirs.

## Owner's hard rules (binding; violating any is a stop-and-report)
- Touch only your worktree. Never `cd` to `/opt/projects/ai` or another
  worktree, never run git against them, never switch or create branches
  (your branch exists), never rebase, never push, never merge. Commits are
  local; the top-level controller integrates.
- No spending: nothing that triggers GitHub Actions or any paid service. No
  network except what the plan explicitly allows (none expected).
- No changes under `upstream/`. Install nothing outside `.venv-dev/`/`.toolchains/`.
- The permission guard blocks `rm -rf`, `git clean -fd*`, and bypass flags.
  Never issue them (a held command stalls you 10 minutes). Use unique build
  dirs and `find <unique-dir> -delete` when something must go.
- Commit author and trailers, exactly:
  `git -c user.name=shift-f10 -c user.email=40450442+shift-f10@users.noreply.github.com commit -m "<subject>" -m "<body>" -m "Co-Authored-By: <model that wrote the commit> <noreply@anthropic.com>"`
  where `<model that wrote the commit>` is the implementer's actual model (`Claude Opus 5.5` or `Claude Sonnet 5.5`, as the harness attribution reminder shows it). Write the trailer line LITERALLY in every commit command — never through a shell variable (subagent shells do not persist). No `Claude-Session:` line and no identifying data (owner 2026-10-27). Plan text that shows other trailer lines is superseded by this rule.
- Handoff discipline: `docs/AGENT_HANDOFF.md` must reflect reality when your
  plan's documentation task runs; proof status never lives only in chat.
- `-Werror` everywhere; TDD per task with RED evidence in the report.

## Models and nested workers (owner: "let the workers spawn additional workers")
- Implementers and task reviewers: `model: "opus"` (the alias resolves to the
  current Opus). Pure transcription tasks (the brief contains the complete
  code) and single-file mechanical fixes: `model: "sonnet"`. Final
  whole-branch review: `model: "opus"`.
- Implementers MAY spawn read-only helpers (`Explore` agents, or
  general-purpose agents told they are read-only) to locate code or trace
  flows. They MUST NOT spawn a second editor of the same tree, and MUST NOT
  spawn a reviewer of their own work. Put that sentence in every implementer
  dispatch in place of the template's blanket "no subagents" paragraph.
- Never run two implementers in the same worktree at once.

## Rulings
Decide conflicts yourself per the skill (spec is binding, plan is its
argument); record `Ruling: … — why — cost if wrong` in the ledger. Stop only
for the skill's four reasons. When you stop, say exactly what you need.

## Final report (write to the path your plan brief names, then reply ≤15 lines)
Status (COMPLETE | STOPPED: <reason>), branch + HEAD SHA, commit list
(`git log --oneline <base>..HEAD`), final test counts (linux-gcc ctest, wine
ctest, pytest), final whole-branch review verdict and residual findings,
every ruling made, parked items, concerns. Do NOT run
finishing-a-development-branch's merge/PR steps — the top-level controller
owns integration.
