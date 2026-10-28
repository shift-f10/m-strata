#pragma once
// Cases of secure_frontend_fetch_tests defined outside fetch_test.cpp.
namespace fetch_cases {
// fetch_file_test.cpp (Task 21)
void dl03_truncated_body_rejected();
void dl_parallel_assembles_pieces();
void dl_parallel_falls_back_without_ranges();
void dl_parallel_resumes_from_sidecar();
void dl03_oversize_body_rejected_mid_stream();
void dl03_redirect_to_http_rejected();
void dl03_redirect_off_allowlist_rejected();
void dl03_redirect_limit();
void redirect_query_only_in_config_line();
void dl03_private_address_rejected();
void dl03_mixed_public_private_answer_rejected();
void dl03_range_ignored_restarts_from_zero();
void dl03_content_range_mismatch_rejected();
void dl03_content_encoding_gzip_fails_digest();
void dl03_stalled_source_times_out();
void time_budget_exceeded_is_resumable();
void tls_untrusted_moves_to_next_source();
void digest_mismatch_after_resume_refetches_once();
void free_space_rechecked_while_writing();
void ipv6_only_hop_is_address_not_allowed();
void range_ignored_twice_is_range_mismatch_then_restarts();
void proxy_hop_skips_address_policy();
#if defined(SECURE_FRONTEND_TEST_HOOKS)
void test_cacert_reaches_argv_in_hook_builds();
#endif
// fetch_pull_test.cpp (Task 22)
void dl03_wrong_digest_rejected_prior_version_intact();
#if !defined(_WIN32)
// fetch_spawn_test.cpp (Task 25)
void downloader_spawn_report_classified();
#if defined(SECURE_FRONTEND_TEST_HOOKS)
void priv04_downloader_argv_exact();
void priv04_downloader_env_is_empty();
void redirect_url_absent_from_downloader_argv();
void dl03_downloader_untrusted_path_rejected();
void downloader_below_floor_exits_9();
void dl03_downloader_cannot_write_files();
void dl03_downloader_cannot_read_home();
void pre_exec_failure_fails_closed_exit_9();
void probe_interrupted_exits_8();
void ruleset_failure_fails_closed_exit_9();
void record_fd_does_not_clobber_report_pipe();
void confinement_reduced_is_announced_without_landlock();
#endif
#endif
#if defined(_WIN32)
// fetch_spawn_windows_test.cpp (Task 26)
void downloader_transfer_through_job_windows();
void dl03_default_windows_curl_path_accepted_windows();
void dl03_downloader_path_normalised_windows();
void dl03_downloader_unc_path_rejected_windows();
#if defined(SECURE_FRONTEND_TEST_HOOKS)
void dl03_downloader_runs_at_low_integrity_windows();
void priv04_downloader_env_is_systemroot_only_windows();
#endif
#endif
// fetch_cli_test.cpp (Task 27)
void confinement_reduced_written_to_log_record();
#if !defined(_WIN32)
void doctor_downloader_probes_on_fresh_store();
#if defined(SECURE_FRONTEND_TEST_HOOKS)
void confinement_reduced_when_root_beneath_allowed_dir();
#endif
#endif
#if !defined(_WIN32)
// fetch_release_test.cpp (Task 28, release-only)
void release_spawn_has_no_fd4();
#endif
}  // namespace fetch_cases
