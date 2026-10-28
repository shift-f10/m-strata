#pragma once
// SP4 Task 20 (spec §5): bundle open/verify/import/export cases, defined in bundle_io_test.cpp.

namespace bundle_cases {

void dl02_import_bundle_publishes_same_as_pull();
void dl03_bundle_digest_mismatch_publishes_nothing();
void bundle_rejects_traversal_path();
void bundle_rejects_unlisted_file_in_files_dir();
void bundle_sha256sums_must_match_manifest();
void bundle_rejects_hardlinked_file();
void export_refuses_bundle_forbidden_license();
void export_refuses_fat32_target_for_large_file();
void export_rehashes_and_rejects_bit_rot();
void verify_writes_nothing();
void import_from_read_only_bundle_directory();
void bundle_incomplete_and_marker_rules();
void export_onto_shared_writable_target();
// Beyond the plan's list: effective per-file license (exit 10), drive/mount roots (F-46),
// the make_bundle.py output shape (Task 13 carry).
void export_refuses_when_one_file_license_forbids_bundle();
void bundle_directory_must_not_be_a_volume_root();
void make_bundle_tool_output_opens_verifies_and_imports();
// bundle_pull_test.cpp (Task 22): pull --bundle.
void pull_bundle_resumes_after_interrupt();
void pull_bundle_incomplete_without_pack_sources();
void pull_bundle_resume_rehashes_files_from_earlier_run();
// bundle_cli_test.cpp (Task 23): strata import / verify / export.
void import_works_without_downloader();
void cli_verify_export_import_round_trip();
void failed_export_says_to_remove_target();
void import_blocked_by_orphan_says_how_to_clear_it();
// Final review C1 / I2.
void import_license_change_needs_interactive_yes();
void import_with_unreadable_installed_version_needs_interactive_yes();
void import_over_corrupt_orphan_reports_target_exists();
#if defined(_WIN32)
void bundle_rejects_junction_entry_windows();
#else
void bundle_rejects_symlink_entry();
// F-6 consumer: over-long joined paths and non-portable names on bundle media fail closed.
void bundle_walk_fails_closed_on_overlong_or_odd_names();
// Fix round 1 (I1): read side accepts a mount-root bundle; write side refuses a mount root.
void bundle_at_mount_root_is_read_but_never_written();
#endif

}  // namespace bundle_cases
