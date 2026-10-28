Controller rulings that bind Task 25 (supersede the brief where they differ):
- B2: If Landlock is expected (landlock_abi() >= 1 and not test_force_no_landlock) and ruleset creation (landlock_create_ruleset) fails, return AcquireError{AcquireReason::downloader_confinement_failed} with NO spawn (exit 9). Never execve curl unconfined after confinement() announced `full`. add_rule failures may stay (void) (they only narrow access). Add a covering test if a cheap hooks seam exists; otherwise justify in the report.
- R2: Reuse SP2's posix_launch::read_report and classify_report (src/posix_child_launch.hpp ~313-347) for the report pipe instead of a hand-written read loop/classifier. Keep public `classify_spawn_report(std::string_view)` as a thin mapping: hook_failed→downloader_confinement_failed; exec_failed ENOENT→downloader_missing; EACCES→downloader_untrusted_path; other errno or malformed→downloader_unsupported.
- R4: confinement() plans with std::filesystem::canonical(executable, ec); fall back to absolute(executable).lexically_normal() only if canonical fails.
- R5: Keep the plan's record-file open (O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC|O_NOFOLLOW once per spawn); no O_EXCL.
- I5: src/strata_main.cpp is in scope: add only std::signal(SIGPIPE, SIG_IGN); keep the existing utf8_command_line / secure_frontend_utf8_argv usage untouched.
- I7: in record_fd_does_not_clobber_report_pipe, fix the comment (on a Landlock kernel the ruleset fd takes 3) or open the record before the ruleset.
- I8: validate_downloader maps PathErrorCode to downloader-specific fixed text (no "engine executable" wording).
- I9: check the optional before dereferencing in dl03_downloader_untrusted_path_rejected.
- Carry-in: SP2 sweep_orphans is not called by this code; no action. Do not change SP2 files.
