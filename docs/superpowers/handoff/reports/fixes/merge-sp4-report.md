# Merge sp4-acquire into main: report

Merge commit: **4b9fdd31** (detached HEAD in `scratchpad/merge-sp4`; parents 863b3ff6 and f0b09e2e). Not pushed. Submodule `upstream` is unchanged at 296b59d2 (v0.1.44.1). The `.venv-dev` and `.toolchains` symlinks were not added.

## Conflicted files: conflict and resolution
1. **secure_frontend/CMakeLists.txt** (3 hunks)
   - engine_arguments cases: kept both sides (SP2's `gpu_list_rejects_malformed_values` and SP4's launch cases with the `secure_frontend_launch`/`secure_frontend_strata_cli` link), plus one new case (see semantic fixes).
   - server_cli cases: kept both (`cli_sp2_flags_map_to_schema_keys` and `cli_expert_flag_parsing`).
   - `strata_secure_server_glm_requires_engine_arg`: SP2's `${SECURE_FRONTEND_SERVER_CLI_LAUNCH}` (the non-elevated launcher on Windows) plus SP4's `--expert`.
   - Added rule 4's shared library `secure_frontend_utf8_argv` (`src/utf8_argv.cpp`, `include/secure_frontend/utf8_argv.hpp`). It links shell32 on WIN32. Both `strata-secure-server` and `strata` link it, so the server keeps its shell32 link through the library.
2. **secure_frontend/README.md**: the only conflict was inside the generated key table. The generator is `render_config_schema_markdown()`, enforced by `typed_config_tests.readme_config_table_matches_schema`. The merged table (SP2 engine rows, then SP4 pull rows) matches the generator byte for byte, and that case passes. No prose conflicts.
3. **src/frontend_config.cpp**
   - `kSchema` is now 43 entries: 36 from SP2 + 7 `pull.*` = 43, and a count of the merged rows confirms it.
   - `engine.lib_dirs`, `engine.gpu` and `engine.allow_new_privileges` stay after `engine.args`, followed by the `pull.*` rows.
   - The constructor composes both sides: `Roots{*models, tokenizer (optional, SP4), secret, logs, cache, state}` and `EngineLaunch{..., lib_dirs, gpus, allow_new_privileges}`.
   - In `describe`, both sides' added lines are kept.
4. **src/secure_server_main.cpp**
   - Includes: `launch_composition.hpp` plus `privilege_check.hpp`. `model_inventory.hpp` is dropped as SP4 dropped it.
   - `run(config, expert)` keeps SP2's `state_dir_is_usable`/`prepare_state_dir`, environment allowlist, no_new_privs and supervisor-owned launch, and uses SP4's `compose_engine_launch` with `launch.model_id`, `launch.engine_args` and `launch.served_models`.
   - The Windows `utf8_command_line()` moved to the shared source. On Windows, main calls `secure_frontend::utf8_command_line(argc, argv)`; the POSIX path is unchanged.
5. **src/server_cli.cpp**: usage text. SP4's managed/--expert paragraph is followed by SP2's sentence that `--engine-arg` refuses `--native`, `--serve` and `--gpu`.
6. **tests/engine_arguments_test.cpp**: all cases and registrations from both sides are kept, plus the new GPU case.
7. **tests/server_cli_test.cpp**
   - The init-then-serve start keeps SP2's `wait_engine_ready` and adds `--expert`.
   - In the "only switches" security assertion, the allowed set is now SP2's `engine.allow_new_privileges` plus SP4's `pull.allow_private_addresses`. The assertion is not weakened: each side's exact allowed set is kept, and the merged set is their union.
8. **tests/server_e2e_test.cpp**: SP2's ready_timeout body is kept and the server start adds `--expert`.
9. **tests/support/server_fixture.hpp**: `base_flags()` gets `--expert` and `--state-dir`.

## Semantic fixes (compile errors or cases that failed only after the merge)
- **`EngineFamily` was defined twice** (a compile error).
  - SP2: `engine_environment.hpp`, `{strata, glm}`, the environment family.
  - SP4: `acquire_manifest.hpp`, `{strata, glm, none}`, the manifest family.
  - I renamed SP4's enum to `ManifestFamily` in acquire_manifest.{hpp,cpp}, engine_gate.cpp, manifest_json.cpp, manifest_test, model_index_test and engine_arguments_test.
  - Ruling: rename SP4's enum, not SP2's — SP2's name is already recorded in AGENT_HANDOFF's SP6 row and its `none` value has no meaning for the environment, while `ManifestFamily` sits next to SP4's own `ManifestProfile` — cost if wrong: one mechanical rename. The SP4 plan text still says `EngineFamily` (docs only).
- **The managed launch dropped SP2's typed `engine.gpu` list.** SP4's `compose_engine_launch` called `build_engine_arguments` without `gpus`, which would have silently ignored `--gpu` on the merged server.
  - The `--expert` path now passes `config.engine.gpus`.
  - The managed path appends `--gpu <list>` after the user args. `--gpu` in engine.args is still refused by `validate_engine_user_arguments`.
  - New case `engine_arguments_tests.managed_launch_renders_typed_gpu_list` (labelled windows-runtime like its SP4 siblings).
  - Ruling: managed launches honour engine.gpu, because SP2 spec §3.9 makes the typed list the only GPU path and nothing in SP4 removes it — cost if wrong: one `--gpu` pair in managed argv.
- **Supervisor profile and protocol flavor** now come from `launch.profile`, not `config.engine.profile`, which is `--expert`-only and unset for a managed model. This also uses SP4's otherwise-unused `glm` variable, which `-Werror` rejected. Behaviour today is identical, since managed is always qwen.
- **SP2's `tests/support/server_harness.cpp`** (an SP1-style inventory fixture added on main) failed 19 server/engine_child cases with "listener must answer", because SP4 serve requires `--expert` for an inventory model. It now passes `--expert`. No assertion was changed.

## Counts
- linux-gcc: **520/520** (2 skips: `proc03_other_uid_cannot_open_engine_fd`, `proc05_cgroup_kill_when_delegated`).
  - Reconciliation: 312 (main) + (422 − 215) (SP4 additions) = 519, plus 1 new merge case = 520.
  - No cases were removed or renamed, and there were no duplicates.
- linux-gcc-release: unfiltered `ctest --test-dir out/build/linux-gcc-release` **510/510** (same 2 skips). The preset, which filters to `release-only|release-also`, and the explicit `-L` run are each **6/6**.
- windows-llvm-mingw-cross: build is `-Werror` clean (0 warnings); wine **256/256**.
- pytest **23 passed**.
- 0 warnings everywhere (`-Werror` builds).

## Not done / concerns
- `docs/AGENT_HANDOFF.md` was taken as git auto-merged it. No merge row was added for the GPU, family-rename and utf8-argv changes, because the brief scoped me to resolution only. The controller should add one.
- Windows runtime proof of `strata`'s new UTF-8 argv (non-ASCII paths) is pending MSYS2. Wine only shows that it builds and runs.
- Windows-runtime rows are pending MSYS2 (the commit says so).
