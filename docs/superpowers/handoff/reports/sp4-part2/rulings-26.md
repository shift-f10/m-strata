Controller rulings that bind Task 26 (supersede the brief where they differ):
- B3: launch_in_job is compiled into secure_frontend_engine_stdio_process, not secure_frontend_engine_process. Link: if(WIN32) target_link_libraries(secure_frontend_downloader PUBLIC secure_frontend_engine_stdio_process advapi32); add userenv only if something actually uses it.
- I2 (binding SP2 carry-in): WinLaunched::thread is a real open handle — close it (e.g. wrap in the same RAII Handle as process and job). Keep WinLaunchSpec Job limits ON (defaults; do not opt out). extra_inherited_handles / crt_fd_block are std::vector — use the real header (src/win_child_launch.hpp) shapes, not the plan excerpt's spans; launch_in_job has an extra optional WinLaunchStep* parameter.
- R2 analogue: reuse Task 25's shared code (downloader_common.cpp, classify_spawn_report etc.) rather than duplicating logic.
- R5: record file opened OPEN_ALWAYS + append once per spawn (no exclusive create).
- I12: the MSVCRT quoting copy is accepted (SP2's quote is file-local); say so in a comment.
- I11: Interfaces text says "first three hooks-only"; follow the registration block (two unconditional, two hooks-only) and note it.
- classify_spawn_report stays POSIX-only (ruled after Task 25); do not add a Windows declaration — map launch_in_job DWORD failures directly per the brief.
