#pragma once
// Cases of secure_frontend_acquire_tests defined outside acquire_test.cpp.
namespace acquire_cases {
// acquire_store_test.cpp (Task 16)
void concurrent_pull_same_manifest_second_is_models_busy();
void part_file_create_append_resume_truncate();
void part_file_write_at_fills_offsets();
void rename_within_never_replaces();
void rename_fallback_never_replaces();
void remove_tree_removes_links_without_following();
void hash_into_rejects_hard_links_and_counts_bytes();
void hash_into_stops_when_interrupted();
void store_environment_seams_report_space_and_fs_kind();
void overlong_joined_paths_fail_closed_without_throwing();
// acquire_publish_test.cpp (Task 19)
void dl04_publish_uses_noreplace_and_never_overwrites();
void publish_falls_back_when_noreplace_unsupported();
#if defined(SECURE_FRONTEND_TEST_HOOKS)
void dl04_crash_between_publish_and_index_is_not_loaded();
#endif
void dl04_orphan_adopted_after_rehash_by_digest();
void replace_loaded_model_retires_old_version();
void layout_check_requires_pack_files_and_valid_tokenizer();
void gemma4_layout_check_requires_an_spm_tokenizer_pack();  // Gemma 4 plan 1 Task 4
void staged_tree_must_be_exactly_the_manifest();
void install_decisions_follow_sequence_rules();
void publish_requires_staging_key_of_manifest();
void overlong_staging_tree_is_publish_failed();
void retire_removes_read_only_published_version();
void downgrade_adopts_retired_version_keeping_one_live();
void rm_refuses_held_version_and_removes_read_only_tree();
#if !defined(_WIN32)
void rm_reports_unreachable_version_as_failure();
void rm_failed_delete_stays_retired();
#endif
#if defined(_WIN32)
void replace_loaded_model_retires_old_version_windows();
void rm_open_version_stays_retired_windows();
#endif
#if defined(_WIN32)
// acquire_store_windows_test.cpp (Task 17)
void store_primitives_windows();
void store_adopts_only_private_files_windows();
#endif
// acquire_pull_test.cpp (Task 22)
void dl02_correct_digest_publishes_atomically();
void dl02_byte_limit_enforced_while_streaming();
void dl02_resume_with_range_completes_and_verifies();
void resume_offset_ignores_preallocation();
void dl02_published_files_are_private();
#if defined(_WIN32)
void dl02_published_files_have_private_dacl_windows();
#endif
void dl02_preflight_refuses_without_space();
void dl03_untrusted_manifest_digest_rejected();
void dl03_downgrade_rejected();
void dl03_sequence_conflict_rejected();
#if defined(SECURE_FRONTEND_TEST_HOOKS)
void dl03_third_party_cannot_shadow_catalog();
void pull_test_cacert_reaches_argv_in_hook_builds();
void publish_failure_after_step_2_resumes_without_download();
#endif
void dl04_interrupt_during_download_never_indexed();
void dl04_interrupt_during_verification();
void dl04_stale_staging_from_other_manifest_discarded();
void abandoned_partials_kept_with_yes();
void abandoned_staging_reported_and_offered_for_deletion();
void all_sources_failed_exits_3_and_keeps_part();
void enospc_mid_stream_keeps_part_and_exits_6();
void update_reuses_unchanged_files();
void update_reuse_copy_interrupted_mid_file_exits_8();
void pull_verify_interrupted_exits_8();
void license_change_requires_interactive_yes();
void pull_without_bundle_refuses_sourceless_files();
void confirmation_answers_parsed_strictly();
void already_installed_needs_no_network();
void pull_budgets_map_to_exit_classes();
void discard_partial_runs_only_after_confirmation();
void unreadable_installed_license_requires_interactive_yes();
// acquire_cli_test.cpp (Task 23)
void program_update_exits_12_without_network();
void cli_requires_config_and_known_verb();
void list_and_show_report_installed_models();
void store_option_names_another_store();  // GUI-first start-up (spec 2026-10-27 §4.3)
void list_all_reports_retired_orphaned_and_staging();
void rm_refuses_loaded_model();
void rm_retired_deletes_unheld_versions();
void rm_partials_deletes_other_staging();
// SP5 M1 Task 14 (D-4): strata version / strata key show|rotate.
void version_names_frontend_and_engine_versions();
void key_show_requires_local_token_and_never_logs();
#if defined(SECURE_FRONTEND_TEST_HOOKS)
void list_outdated_uses_catalog_sequence();
#endif
// acquire_network_cli_test.cpp (Task 27)
void pull_cli_prints_summary_and_installs();
void pull_progress_lines_are_machine_readable();
void hf_info_resolves_a_link();
void run_installed_model_needs_no_network();
void run_refuses_glm53_without_engine();
void engine_gate_refuses_gemma4_until_the_engine_exists();  // Gemma 4 plan 1 Task 2
#if defined(SECURE_FRONTEND_TEST_HOOKS)
void run_missing_model_non_interactive_exits_2();
void run_missing_model_prompts_and_decline_exits_2();
void update_models_pulls_outdated_catalog_models();
#endif
#if !defined(_WIN32) && defined(SECURE_FRONTEND_TEST_HOOKS)
void dl04_sigint_during_download_exits_8();
void dl04_sigterm_during_download_exits_8();
void interrupt_at_confirmation_prompt_exits_8();
#endif
#if defined(_WIN32) && defined(SECURE_FRONTEND_TEST_HOOKS)
void dl04_interrupt_during_download_never_indexed_windows();
#endif
}  // namespace acquire_cases
