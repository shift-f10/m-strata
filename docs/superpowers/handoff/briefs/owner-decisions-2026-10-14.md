# Owner decisions — 2026-10-14 07:43 UTC (the owner: "approve all")

All recommendations below were approved as written. They are binding for plans and implementation.

## Earlier today (already applied)
- Upstream bump to Niko1221/Strata v0.1.44.1 merged (main c07f2429).
- SP2 Task 22: engine-stdin writes get an overall bound (the request's remaining deadline) in SP2's second run; Task 28 documents it.
- Layout: upstream as pinned submodule; PROVENANCE is the single update runbook.

## A. SP6 — GLM engine port (from docs/superpowers/research/2026-10-14-sp6-glm-engine-sources.md §6)
A1. Base engine: EVALUATE BOTH (project-maya v1.1.8 @5c970a86 and lighttransport glm53f @4894c996) on the owner's NVIDIA box before choosing; lean Maya.
A2. Windows GLM is in SP6 scope.
A3. MSVC v143 (VS 2022) accepted as the nvcc host compiler for the Windows GLM build.
A4. AMD/HIP out of scope for SP6 v1 (NVIDIA only).
A5. Quant formats: GGUF IQ/K types + Maya's own S/M/L quants; EXL3 later.
A6. Maya's serve/, maya.py, dashboard, updater, MCP client and quant tooling are out of scope.
A7. DFlash2 (CC BY-NC-ND weights) is never supported, fetched or documented, including via STRATA_GLM_DFLASH.
A8. Single GPU first; multi-GPU layer split later; glm53f remote TP permanently out.
A9. Tracking: git submodule, no local patches (fork on GitHub if a patch is unavoidable).
A10. SP2 env allowlist for Maya: slots off (STRATA_GLM_SLOTS=0 or STRATA_GLM_NO_REUSE=1), STRATA_GLM_DFLASH excluded, no tuning knobs pass.
A11. Maya's default loop guard is OFF for our chat route.

## B. SP3 — API concurrency & streaming (docs/superpowers/specs/2026-10-14-sp3-api-concurrency-streaming-design.md §11 decisions 1–12)
B1. max_tokens omitted → auto = min(max_completion_tokens, ctx − n − 8); SP1 schema addendum (0 = auto, range 0–16384).
B2. max_tokens above the limit → clamp; 400 only when the prompt alone fills the context.
B3. Invalid UTF-8 output → U+FFFD; incomplete tail dropped on length/cancel.
B4. Generation timeout → partial text, finish_reason "length".
B5. Stream framing → chunked transfer coding.
B6. Sampling → neutral values are no-ops; bounded temperature/top_p/seed passed to GEN (option e).
B7. /v1/completions out. B8. /v1/messages out of SP3 (SP5 candidate). B9. Ollama /api/* out.
B10. CORS off by default; exact-origin opt-in via listener.cors_origins.
B11. Defaults: 8 workers; queue derived min(4, max(0, workers−2)); 300 s queue timeout; per-remote-address cap derived min(4, workers). *Amended 2026-10-23 (owner: "Raise it"): with `listener.web_ui` the per-remote-address cap derives min(6, workers); a phone's page load had lost `app.js` to the cap of 4.*
B12. Loopback local-DoS residual after the early-auth fix: accepted, recorded in the threat model.
Plans: TWO — M1 (concurrency + hardening), M2 (streaming).

## C. SP4 — verified pull / import / update (docs/superpowers/specs/2026-10-14-sp4-acquire-design.md §12 D1–D7)
C1 (D1). Transport: system curl as a confined child (Linux: PR_SET_NO_NEW_PRIVS + Landlock read-allow system dirs, no write; Windows: restricted/low-integrity token in the Job); we verify every byte (SHA-256) and count bytes ourselves.
C2 (D2). Manifest trust: built-in digest-pinned catalog; third-party manifests only via --trust-manifest sha256:<hex> per run; no remote catalog in v1 (PX pull.manifest_sources dropped).
C3 (D3). Curl floor 8.4.0 on Windows, 7.81.0 on Linux; "confinement: reduced" announced and logged when Landlock is unavailable.
C4 (D4). v1 catalog: Qwen3.8 GSQ-RCO iq2_xs, iq3_xxs, iq3_s + Coder iq1_m (Apache-2.0). GLM entries move to SP6.
C5 (D5). Derived packs: unresolved between (A) owner-hosted packs after a determinism check and (C) a C++ pack builder; SHIP (D) import-from-developer-built-bundle meanwhile. The plan implements (D) and leaves a seam for A/C.
C6 (D6). `run` on a missing model → the same in-process pull flow after the same confirmation prompt (run is conditionally networked; PRIV-01 list updated accordingly).
C7 (D7). Bundle trust = catalog digest or --trust-manifest; no signature. Ed25519 + SHA-512 + program-update verification move to SP7; `strata update` (program) exits 12 until then.
SP2 second-run seam (binding, from SP4 §3.2): keep_fds lifted via F_DUPFD_CLOEXEC ≥ kLiftFloor before the 0..3 dup2 sequence; `int pre_exec(const void*, std::span<const int> lifted) noexcept` run after the sweep: dup2(record,4) → PR_SET_NO_NEW_PRIVS → landlock_restrict_self → close(ruleset) → chdir; non-zero → 'H' marker + errno on kReportFd(3) → downloader_confinement_failed exit 9. Windows: primary_token, extra_inherited_handles, crt_fd_block (n=5, packed, entry 4 = record handle).
