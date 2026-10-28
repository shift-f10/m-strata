Research reports gathered 2026-10-13 for the post-SP0 specs. Each is a subagent's
final report, saved verbatim from the controller's transcript. Move to
docs/superpowers/research/ once the SP0 branch is merged.

- product-experience.md — Ollama CLI/install/update surface vs upstream Strata onboarding; gap table; token UX options (recommend A+B+C); service/auto-start options (recommend opt-in user unit / logon task).
- sp1-config-paths-tokens.md — config/limits inventory, secret/token paths, path-validation duplication, CFG/AUTH/NET test mapping, CSPRNG/DPAPI/canonical-path API notes, upstream config surface. 5 defects incl. NET-03 wildcard-spelling bypass (→ SP1 Task 1) and likely-broken Windows token reader (READ_CONTROL).
- sp2-supervision-proc.md — supervision today, fake engine coverage, PROC/EXT gap table, upstream restart/backoff + engine watchdog facts, typed env allowlist proposal, platform notes. Defects: PROC-05 leader-reaped-without-group-kill; env `{}` likely breaks real CUDA engine on Windows (SystemRoot).
- 2026-10-14-sp6-glm-engine-sources.md — SP6 source comparison: upstream v0.1.43 vs lighttransport glm53f @4894c996 vs mw00/project-maya v1.1.8 (lineage, GLM component table, serve-protocol/argv deltas, engine network code, Windows/HIP, tracking); recommends maya engine as base with glm53f as cross-check; 10 owner decisions.
