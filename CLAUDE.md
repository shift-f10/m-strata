# Secure Strata — agent entry point

**Before doing anything in this repository, read, in this order:**

1. `docs/superpowers/handoff/2026-10-16-controller-resume.md` — the orchestration-level state: what is built, the owner's binding rules, how work is run (sub-controllers, briefs, the merge gate), decisions already taken, what waits on the owner, the exact resume procedure, and the first GPU-box checklist. The project is **stopped by the owner** until he says otherwise; do not start work on your own.
2. `docs/AGENT_HANDOFF.md` — the code-level truth: every deliverable, its proof status, known gaps. It is updated as part of every change.
3. `docs/PROVENANCE.md` and `docs/security/threat-model.md` before touching code.

Sub-controllers and implementers also read `docs/superpowers/handoff/briefs/subcontroller-common.md` and their lane brief.

## Non-negotiable rules (full text in the resume document, §2)

- Touch only what the owner names. No paid services. `upstream/` is never modified.
- Agents never push, merge, rebase, switch branches, or install outside `.venv-dev/` and `.toolchains/`.
- Commit author `shift-f10 <40450442+shift-f10@users.noreply.github.com>` (the GitHub noreply address; owner 2026-10-27); one literal trailer line in every commit, `Co-Authored-By: <model> <noreply@anthropic.com>` - no `Claude-Session:` line and no session links (owner 2026-10-27). Existing trailers are never rewritten.
- **Standing rule (owner, 2026-10-27): nothing identifying or sensitive is ever committed or pushed.** No e-mail address but the noreply one, no names or handles, no PC or host names, no LAN addresses or network settings (subnets, host aliases, SSH targets); no secrets, keys, tokens or passwords, no `secret/` files, no `out/` or `data/` configs, no logs, no session links. The one exception: the development API key `strata-secret-key-dev-placeholder` may be in the repository, and every server started for development or tests uses it (never a real install's key). Scan staged changes before each commit; the local `.git/hooks/pre-push` guard checks every pushed commit and is never bypassed. Push only `main`, only on the owner's word; the local branch `history-before-2026-10-27` is never pushed.
- No `rm -rf`, `git clean -fd*`, or bypass flags. Retire worktrees by moving them out, then `git worktree prune`.
- `-Werror` everywhere; TDD with RED evidence; a test is never weakened to pass.
- Windows claims come only from the `windows-msys2-clang` CI job; GPU claims only from the owner's box. Update `docs/AGENT_HANDOFF.md` with every change.
- **Standing rule (owner, 2026-10-30): workers are checked.** Workers (sub-agents, with skills) may be used where they help, but their work is checked before it is reported or committed, and each running worker is checked on about every ten minutes to make sure it is not stuck.
