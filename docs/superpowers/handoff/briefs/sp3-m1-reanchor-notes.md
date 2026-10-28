# SP3 M1 re-anchor notes (for the SP3 sub-controller at launch) — from the SP4 plan pre-flight clash table, 2026-10-14 09:35 UTC

SP3 M1 executes AFTER SP2 run-2 AND SP4 (Tasks 1–24) are merged to main. Its Task 0 anchor check will meet these SP4-era changes; rule on them before Task 1:

1. `.github/workflows/secure-frontend.yml`: SP4 adds `upstream.version` (committed root file) as the engine-version source; configure never fails for a missing tag, only on mismatch. The TSan job needs NO extra `-D`. Header comment lines may text-conflict — keep both sentences.
2. `secure_frontend/CMakePresets.json`: SP4 turned `SECURE_FRONTEND_TEST_HOOKS` ON in the test presets and added `linux-gcc-release` (hooks OFF, `-L "release-only|release-also"`); the TSan job inherits hooks ON (harmless). Do not re-add.
3. `include/secure_frontend/frontend_config.hpp`: SP4 added a `pull` member and ctor parameter and made `tokenizer_pack`/`profile` optional; SP3 M1 adds `logging` + its ctor parameter and rewrites `ConcurrencyLimits`. Re-anchor the ctor edit on the SP4 shape; any SP3 code dereferencing `tokenizer_pack`/`profile` must handle the optional.
4. `src/frontend_config.cpp`: `kSchema` size is written relatively in both plans ("current count + N"); SP4 rows `pull.*` sit after `engine.args`; SP3 appends its 10 rows after those. `validate`/`describe` edits are additive.
5. `src/secure_server_main.cpp`: SP4 removed `selected` and composes the launch via `compose_engine_launch`; the served model id is `launch.model_id`. SP3 M1's `ApiRouterConfig{model_id=config.engine.model_id, owned_by=selected->owned_by}` must become `model_id = launch.model_id` and take `owned_by` from the launch/inventory record SP4 exposes.
6. `tests/support/server_harness` and SP3's process tests: SP4 requires `--expert` on `strata-secure-server` for weight flags in `engine.args`; SP3's server launches must pass what the SP4-era fixtures pass (read `tests/support` on main at launch).
7. `cfg01_*` cases live in `secure_frontend_typed_config_tests` (ruling R-3 of the SP4 review) — SP3 M1 already uses that binary; keep it.
8. Egress: SP4 lands a connect-audit `LD_PRELOAD` shim first; SP3 M1's interposer task reuses/extends it rather than adding a second shim; SP3's static-import tripwire stays.
9. `secure_frontend/README.md` key table is generated — regenerate, never hand-merge. `docs/AGENT_HANDOFF.md`: both plans add sections and edit "Known small gaps" — additive, text-merge carefully.
10. The `strata` binary exists after SP4 (its verbs only); SP3 M1 does not create one; the product-spec CLI (`serve`, `version`, `key`) is still unowned — flag to the controller when SP3 M1 finishes.
