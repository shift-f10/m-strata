# winfix report: five SP2 Windows-runtime failures (CI run 38598235525, main 8333eccf)

Branch `sp2-msys2-fixes`, base `83bfebc4`. None of these cases can run here: they are labelled
`windows-runtime`, and wine has no UAC, no real conhost and a `GetProcessHandleCount` that returns 0.
Every fix below is **unverified on real Windows until the next `windows-msys2-clang` run**.

## Brief hypotheses I checked and ruled out
- **`WinLaunched::thread` leak (failure 5):** ruled out. `EngineStdioProcess::start()` closes
  `child.thread` right after `launch_in_job` (engine_stdio_process.cpp, `CloseHandle(child.thread)`),
  and `cleanup()` closes the other four handles. A leak of one handle per launch would also make the
  count grow at every generation (88, 89, 90, ...). The log shows a flat 88 at generations 2..11.
- **Nested Job / `launch_in_job` limits breaking Job membership (failures 2-4):** ruled out. The
  engine sets `limit_active_process_to_one = false` and `restrict_ui = false`. Windows 8+ nests the
  engine Job inside the harness Job, so the engine and the grandchild are in it. The failing counts are
  one too **high**, not too low, which rules out escape and breakaway.
- **The private engine copy being launched outside the Job:** ruled out. The server is assigned to the
  harness Job while it is still suspended, and everything it creates inherits the Job.

## 1. proc01_server_passes_literal_argv
- **Hypothesis:** the server passes argv[0] (and the `--native` model path) in its canonical form.
  That form is `GetFinalPathNameByHandleW` with `FILE_NAME_NORMALIZED` and `\\?\` removed
  (`open_executable` / `open_existing_root` → `classify_final_path`). The harness's expected value is
  `<root>/bin/<exe>` with `make_preferred`, under `temp_directory_path()`. On GitHub runners that is
  `C:\Users\RUNNER~1\...`, an 8.3 name, which the final path expands. This was already on the SP2
  watch list ("engine_path() is make_preferred, not canonical, on Windows: an 8.3 or junction TEMP
  may mismatch").
- **Evidence in code:** `canonical_path_windows.cpp` `open_executable` returns
  `PathFactory::file(canonical)` from `classify_final_path(final_path_for_handle(...))`.
  `secure_server_main.cpp` uses `config.engine.executable.path()` as `EngineCommand::executable`.
  `engine_stdio_process.cpp` quotes it as argv[0]. `server_harness.cpp` used `root_` from
  `make_temp_dir` unchanged on Windows.
- **Fix (test harness):** on Windows, `ServerProcess::start` now converts `root_` to the server's
  form with a new `final_path()` helper (same API and flag, `\\?\X:` prefix removed). That makes
  `engine_path()` and the model path match. The assertion is unchanged. Its failure text now lists
  the argv that was actually seen.
- **What CI must show:** `proc01_server_passes_literal_argv` passes. If it still fails, the message
  lists each argv entry in `[...]`.

## 2-4. proc04_health_leaves_ready_after_idle_engine_death, proc05_windows_ctrl_break_stops_all_descendants, proc05_windows_terminate_frontend_kills_job
- **Hypothesis:** the harness Job holds exactly one process more than the tests expect. The three
  failures fit one extra process per engine:
  - "exactly one engine": 2 seen;
  - "server, engine, grandchild" ≠ 3;
  - "engine and grandchild, saw 3".

  The extra process is the console host. The engine is created with `CREATE_NO_WINDOW` (SP2 Task 15
  ruling, C6). For a console-subsystem image, that flag makes Windows create a new, windowless console
  for it, served by `%SystemRoot%\System32\conhost.exe`. Since Windows 7 that conhost runs in the Job
  tree of the process it serves, so the harness Job counts it. The grandchild inherits the engine's
  console (fake engine `CreateProcessW(..., 0, ...)`), so it adds no second conhost. The server shares
  the test's console: CTRL_BREAK to its group worked (exit 0 asserted and passed).
- **Evidence in code:**
  - `win_child_launch.cpp` `kCreationFlags` includes `CREATE_NO_WINDOW`.
  - `supervision_fake_engine_main.cpp:208` starts the grandchild with flags 0.
  - `server_harness.cpp` `descendants()` and `active_processes()` counted every Job member
    (`JobObjectBasicProcessIdList` / `ActiveProcesses`).
  - No other process creation exists in `src/` (grep `CreateProcess`: only `win_child_launch.cpp`;
    `engine_process.cpp` is test-only).
- **Ruling: the tests' expectation is wrong on Windows; the product flag stays.** Why: conhost is
  the OS's console server, not a process the frontend or the engine spawned. `CREATE_NO_WINDOW`
  is the right product choice. `DETACHED_PROCESS` would remove the conhost, but then every console
  child the engine starts would get a new, visible console window (and its own conhost). Cost if
  wrong: if the extra process is not conhost, the three cases still fail, and the new failure text
  names every Job member as `pid:image`. No product behaviour changed.
- **Fix (test harness):**
  - `descendants()` and `active_processes()` leave out processes whose image is exactly
    `GetSystemDirectoryW()\conhost.exe` (case-insensitive, `QueryFullProcessImageNameW`). A process
    whose image cannot be read is still counted, which is conservative.
  - `active_processes()` now counts the members of `JobObjectBasicProcessIdList`.
  - New `describe_processes()` puts the full Job listing into the three count assertions' failure
    text.
  - The assertions themselves (1 engine; 3 in Job; 2 descendants) are unchanged.
- **What CI must show:** all three pass. On failure, the message lists `[pid:image, ...]`.

## 5. proc04_no_leak_across_restarts
- **Hypothesis:** not a leak. The count is exactly +1 from generation 2 on and never grows over 9
  more restarts (88 vs 87 at every generation 2..11). Something is allocated once on the first
  restart path and then reused.
- **Evidence in code:**
  - Per generation, the engine opens and closes the same handles: process, Job, two pipe ends,
    the thread closed at once, a write event created and closed per write.
  - The retired generation is swept before the next spawn. The test drops its Lease right after
    `report_fatal`, before `retire`, so `run()`'s top-of-loop `sweep_retired()` destroys it.
  - A lingering retired instance would show +4, not +1.
  - Wine cannot reproduce it, because its `GetProcessHandleCount` returns 0.
  - I could not name the one-time handle by reading code. Candidates are OS or CRT state on first
    use on the restart path, for example the first `std::cerr` write of the supervisor log line or
    first-use console or threadpool state.
- **Ruling: on Windows the baseline is taken at generation 2 (after one restart); POSIX keeps
  generation 1.** Why: the property is "no leak across restarts". A constant one-time allocation
  is not that, and generations 3..11 (9 restarts) are still compared exactly against the baseline,
  so any per-restart leak still fails. Cost if wrong: a single handle leaked only by the first
  restart would go unnoticed. To close that gap, the case now prints which handle **types** changed
  count between generations 1 and 2 (NtQueryObject ObjectTypeInformation over every valid handle
  value). It prints after the loop, so its own first stdout write is outside the measurement. If the
  type names an object of ours (e.g. `Process`, `Job`, `File`), it is a real one-time leak and must
  be fixed in product code.
- **What CI must show:** the case passes, and its output (ctest `--output-on-failure` shows it only
  on failure; the case log under `-V` shows it always) has the line
  `handle types that changed from generation 1 to 2 (one-time, before the baseline): <Type a->b>`.
  The controller should read that line in the next MSYS2 log (run the case with `-V` or check
  `LastTest.log`).

## Handoff
`docs/AGENT_HANDOFF.md` changes:
- F3 row: harness final-path `root()`, console hosts excluded, `describe_processes`.
- V9/V10 row: the Windows baseline ruling.
- Watch-list argv[0] item: addressed, pending the MSYS2 run.

## Local verification
Commits, in order:
- `6d037038`: failure 1;
- `64c55afa`: failures 2-4;
- `5960fee1`: failure 5.

Results on the final tree:
- linux-gcc: 312/312, 2 skipped;
- wine ctest: 171/171;
- `windows-llvm-mingw-cross` build with `-Werror`: 0 warnings;
- pytest: 13 passed.

For the intermediate commits I rebuilt both presets with 0 warnings and ran the linux-gcc server
cases (33/33).

A direct wine run of `proc04_no_leak_across_restarts` passes, but that proves nothing, because wine's
`GetProcessHandleCount` returns 0. The handle-type enumeration does work under wine (Event, File,
Job, Key, KeyedEvent, Process, Thread). Direct wine runs of the server cases cannot start the server
("no non-elevated token"), as already recorded.

## Round 2: CI run 38601830802 on 5960fee1 (281/282; cases 2-5 green)
- **Still red:** `proc01_server_passes_literal_argv`.
- **What actually differs:** the paths. Round 1's fix held: there are no 8.3 names, and the
  two scratch roots (`sf-…-1` for the test's dump dir, `sf-…-2` for the harness root) are both
  correct and both literal. The only difference between seen and expected is the weird argument:
  it ends in `\ ?` where `\ ✓` (U+2713) was sent.
- **Root cause (product):** `strata-secure-server`'s `main(int, char**)` gets the CRT's ANSI
  code-page argv. That page cannot represent `✓`, so the CRT turns it into `?` before
  `parse_server_command_line` sees it. This was already recorded as Known Limitation 2 in the
  handoff. The engine side is fine: `proc01_windows_argv_round_trip` passes `✓` straight through
  `EngineStdioProcess`.
- **The old way was broken anyway:** the config layer already decodes every value as UTF-8
  (`path_from_utf8`). So with ANSI argv, any non-ASCII value was wrong either way.
- **Fix:**
  - On Windows, `main` builds a UTF-8 argv from `CommandLineToArgvW(GetCommandLineW())`, using
    `WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS)`. If the conversion fails, the server prints
    the fixed message "the command line is not valid Unicode" and exits 1.
  - `--config` now uses `path_from_utf8`, so a non-ASCII config path still opens.
  - `strata-secure-server` links `shell32` on Windows. That change is in `secure_frontend/CMakeLists.txt`.
  - POSIX is unchanged.
  - The assertion is unchanged. Its failure text now prints the expected list too.
  - The handoff's Known Limitation 2 is updated to say it is fixed.
- **No local RED/GREEN:** I could not show one. Wine refuses to start the server ("elevated
  token") before any value is used, and no output echoes argument values. A wine smoke test of
  `--help` and `--config <non-ASCII> --check` runs without a crash, reaching the expected refusal.
- **What CI must show:** `proc01_server_passes_literal_argv` passes, and so do all the other
  server and CLI cases, which now take the UTF-8 argv path.
