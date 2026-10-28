#include "secure_frontend/acquire_manifest.hpp"
#include "secure_frontend/acquire_reason.hpp"
#include "secure_frontend/manifest_diff.hpp"
#include "secure_frontend/model_index_model.hpp"
#include "secure_frontend/staging_plan.hpp"
#include "acquire_cases.hpp"
#include "support/expect.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using secure_frontend_test::expect;

using secure_frontend::AcquireReason;
using secure_frontend::ExitCode;
using secure_frontend::SameSource;

// A copy of spec §8.2, kept independent of the implementation's table.
struct Expected final {
  AcquireReason reason;
  const char* name;
  ExitCode exit;
  SameSource same;
  bool resumable;
};

const std::vector<Expected>& spec_table() {
  using R = AcquireReason;
  using E = ExitCode;
  using S = SameSource;
  static const std::vector<Expected> table{
      {R::manifest_invalid, "manifest_invalid", E::manifest, S::not_applicable, false},
      {R::manifest_unsupported_version, "manifest_unsupported_version", E::manifest, S::not_applicable, false},
      {R::manifest_untrusted, "manifest_untrusted", E::manifest, S::not_applicable, false},
      {R::manifest_shadows_catalog, "manifest_shadows_catalog", E::manifest, S::not_applicable, false},
      {R::manifest_downgrade, "manifest_downgrade", E::manifest, S::not_applicable, false},
      {R::manifest_conflict, "manifest_conflict", E::manifest, S::not_applicable, false},
      {R::manifest_requires_bundle, "manifest_requires_bundle", E::manifest, S::not_applicable, false},
      {R::model_unknown, "model_unknown", E::manifest, S::not_applicable, false},
      {R::dns_failed, "dns_failed", E::transport, S::retried, true},
      {R::connect_failed, "connect_failed", E::transport, S::retried, true},
      {R::tls_failed, "tls_failed", E::transport, S::retried, true},
      {R::stalled, "stalled", E::transport, S::retried, true},
      {R::size_short, "size_short", E::transport, S::retried, true},
      {R::transport_failed, "transport_failed", E::transport, S::next_source, true},
      {R::range_mismatch, "range_mismatch", E::transport, S::restart_file, true},
      {R::size_mismatch_announced, "size_mismatch_announced", E::transport, S::restart_file, true},
      {R::host_not_allowed, "host_not_allowed", E::transport, S::next_source, true},
      {R::redirect_not_allowed, "redirect_not_allowed", E::transport, S::next_source, true},
      {R::redirect_limit, "redirect_limit", E::transport, S::next_source, true},
      {R::response_malformed, "response_malformed", E::transport, S::next_source, true},
      {R::address_not_allowed, "address_not_allowed", E::transport, S::next_source, true},
      {R::tls_untrusted, "tls_untrusted", E::transport, S::next_source, true},
      {R::http_status, "http_status", E::transport, S::next_source, true},
      {R::size_exceeded, "size_exceeded", E::verification, S::next_source_file_deleted, false},
      {R::digest_mismatch, "digest_mismatch", E::verification, S::next_source_file_deleted, false},
      {R::pack_layout_invalid, "pack_layout_invalid", E::verification, S::not_applicable, false},
      {R::bundle_invalid, "bundle_invalid", E::verification, S::not_applicable, false},
      {R::bundle_layout, "bundle_layout", E::filesystem, S::not_applicable, false},
      {R::bundle_incomplete, "bundle_incomplete", E::filesystem, S::not_applicable, false},
      {R::bundle_target_fat32, "bundle_target_fat32", E::filesystem, S::not_applicable, false},
      {R::bundle_target_not_empty, "bundle_target_not_empty", E::filesystem, S::not_applicable, false},
      {R::time_budget_exceeded, "time_budget_exceeded", E::transport, S::not_applicable, true},
      {R::total_budget_exceeded, "total_budget_exceeded", E::space, S::not_applicable, false},
      {R::disk_space, "disk_space", E::space, S::not_applicable, true},
      {R::models_busy, "models_busy", E::filesystem, S::not_applicable, true},
      {R::publish_failed, "publish_failed", E::filesystem, S::not_applicable, true},
      {R::index_write_failed, "index_write_failed", E::filesystem, S::not_applicable, true},
      {R::index_inconsistent, "index_inconsistent", E::filesystem, S::not_applicable, false},
      {R::license_forbids_bundle, "license_forbids_bundle", E::license, S::not_applicable, false},
      {R::downloader_missing, "downloader_missing", E::downloader, S::not_applicable, true},
      {R::downloader_version, "downloader_version", E::downloader, S::not_applicable, true},
      {R::downloader_untrusted_path, "downloader_untrusted_path", E::downloader, S::not_applicable, true},
      {R::downloader_unsupported, "downloader_unsupported", E::downloader, S::not_applicable, true},
      {R::downloader_confinement_failed, "downloader_confinement_failed", E::downloader, S::not_applicable, true},
      {R::declined, "declined", E::declined, S::not_applicable, false},
      {R::interrupted, "interrupted", E::interrupted, S::not_applicable, true},
      {R::engine_profile_unavailable, "engine_profile_unavailable", E::engine, S::not_applicable, false},
      {R::engine_too_old, "engine_too_old", E::engine, S::not_applicable, false},
      {R::usage_error, "usage_error", E::usage, S::not_applicable, false},
      {R::program_update_unavailable, "program_update_unavailable", E::program_update, S::not_applicable, false},
  };
  return table;
}

void exit_classes_match_spec_table() {
  for (const auto& row : spec_table()) {
    const auto& info = secure_frontend::reason_info(row.reason);
    expect(info.name == row.name, std::string{"name of "} + row.name);
    expect(info.exit == row.exit, std::string{"exit class of "} + row.name);
    expect(info.same_source == row.same, std::string{"same-source rule of "} + row.name);
    expect(info.resumable == row.resumable, std::string{"resumable flag of "} + row.name);
    expect(secure_frontend::exit_class(row.reason) == row.exit, std::string{"exit_class of "} + row.name);
  }
  expect(secure_frontend::reason_table().size() == spec_table().size(), "every reason is in the spec table");
}

void reason_table_is_complete_and_ordered() {
  std::set<std::string_view> names;
  const auto table = secure_frontend::reason_table();
  for (std::size_t i = 0; i < table.size(); ++i) {
    expect(static_cast<std::size_t>(table[i].reason) == i, "row " + std::to_string(i) + " is in enum order");
    expect(names.insert(table[i].name).second, "unique name " + std::string{table[i].name});
    for (const char c : table[i].name) expect((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_', "snake_case");
  }
  expect(secure_frontend::exit_code_value(ExitCode::program_update) == 12, "exit code values are the enum values");
}

void error_text_states_resumable() {
  expect(secure_frontend::format_error({AcquireReason::digest_mismatch, "weights/a.gguf"}) ==
             "error: digest_mismatch: weights/a.gguf (resumable: no)",
         "detail and resumable: no");
  expect(secure_frontend::format_error({AcquireReason::interrupted, ""}) == "error: interrupted (resumable: yes)",
         "no detail");
}

void version_dir_name_unique_for_same_version_label() {
  const std::string first(64, 'a');
  std::string second = first;
  second[11] = 'b';
  expect(secure_frontend::version_directory_name(3, first) == "v3-aaaaaaaaaaaa", "v<sequence>-<12 hex>");
  expect(secure_frontend::version_directory_name(3, first) != secure_frontend::version_directory_name(3, second),
         "two manifests with the same version label never share a directory");
  expect(secure_frontend::staging_key(first) == std::string(32, 'a'), "staging key is 32 hex");
}

secure_frontend::AcquireManifest staging_manifest() {
  secure_frontend::AcquireManifest m;
  m.files = {secure_frontend::ManifestFile{"weights/a.gguf", secure_frontend::FileRole::weights, 100, std::string(64, 'a'), {}, {}},
             secure_frontend::ManifestFile{"pack/index.txt", secure_frontend::FileRole::pack, 10, std::string(64, 'b'), {}, {}},
             secure_frontend::ManifestFile{"pack/dense.bin", secure_frontend::FileRole::pack, 10, std::string(64, 'c'), {}, {}},
             secure_frontend::ManifestFile{"pack/extra.bin", secure_frontend::FileRole::pack, 10, std::string(64, 'd'), {}, {}}};
  return m;
}

void staging_plan_classifies_partials() {
  using secure_frontend::FileStep;
  const std::vector<secure_frontend::StagedEntry> staged{
      {"MANIFEST", 900, true},          {"weights/a.gguf.part", 40, true}, {"pack/index.txt.part", 10, true},
      {"pack/dense.bin.part", 11, true}, {"pack/extra.bin", 10, true},     {"pack/extra.bin.part", 3, true},
      {"stray.bin", 5, true},            {"weights/link", 0, false}};
  const auto plan = secure_frontend::plan_staging(staging_manifest(), staged);
  expect(plan.files.size() == 4, "one plan per manifest file");
  expect(plan.files[0].step == FileStep::rehash_then_resume && plan.files[0].existing_bytes == 40, "short .part resumes at L");
  expect(plan.files[1].step == FileStep::rehash_complete_part && plan.files[1].existing_bytes == 10, "full .part is hashed");
  expect(plan.files[2].step == FileStep::fetch_from_zero && plan.files[2].existing_bytes == 0, "oversize .part is deleted");
  expect(plan.files[3].step == FileStep::rehash_verified_candidate, "a suffix-less file is re-hashed before it is trusted");
  const std::vector<std::string> remove{"pack/dense.bin.part", "pack/extra.bin.part", "stray.bin", "weights/link"};
  expect(plan.remove == remove, "oversize part, shadowed part, strays and non-regular entries are removed");
  expect(plan.staged_bytes == 60, "40 + 10 + 10 staged bytes count toward the preflight");
}

void abandoned_keys_match_same_model_only() {
  const std::vector<secure_frontend::StagingKeyInfo> keys{
      {std::string(32, 'a'), "m", "t", 100}, {std::string(32, 'b'), "m", "t", 200}, {std::string(32, 'c'), "m", "u", 300}};
  const auto abandoned = secure_frontend::abandoned_keys(keys, std::string(32, 'a'), "m", "t");
  expect(abandoned.size() == 1 && abandoned.front().key == std::string(32, 'b'), "other manifests of the same name:tag only");
}

}  // namespace

void update_diff_lists_changed_files_and_license() {
  auto installed = staging_manifest();
  installed.version = "2026.10.14.1";
  installed.sequence = 1;
  installed.license = {"Apache-2.0", "https://h.example/l", true, "n"};
  installed.hosts = {"h.example"};
  auto candidate = installed;
  candidate.version = "2026.11.01.1";
  candidate.sequence = 2;
  candidate.license.id = "MIT";
  candidate.hosts = {"h.example", "g.example"};
  candidate.files[1].sha256 = std::string(64, 'e');
  candidate.files[1].size = 12;
  candidate.files.pop_back();
  candidate.files.push_back(secure_frontend::ManifestFile{"pack/new.bin", secure_frontend::FileRole::pack, 7,
                                                          std::string(64, '7'), {}, {}});
  const auto diff = secure_frontend::diff_manifests(installed, candidate);
  const std::vector<std::string> expected{
      "version: 2026.10.14.1 (sequence 1) -> 2026.11.01.1 (sequence 2)",
      "license: Apache-2.0 -> MIT",
      "hosts: + g.example",
      "file changed: pack/index.txt size 10 -> 12, sha256 " + std::string(64, 'b') + " -> " + std::string(64, 'e'),
      "file removed: pack/extra.bin",
      "file added: pack/new.bin (7 bytes, sha256 " + std::string(64, '7') + ")",
      "bytes to download: 19"};
  expect(diff.lines == expected, "diff lines in spec §6.1 order");
  expect(diff.license_changed, "a license change requires an interactive y");
  expect(diff.bytes_to_download == 19, "added + changed bytes only");
  expect((diff.reusable_files == std::vector<std::size_t>{0, 2}), "unchanged files are reused, not downloaded");

  // F-21: a url, notice or per-file license change shows old -> new, never a bare "changed".
  auto relicensed = installed;
  relicensed.license.url = "https://h.example/l2";
  relicensed.license.notice = "m";
  relicensed.files[0].license = secure_frontend::ManifestLicense{"BSD-3-Clause", "https://h.example/b", false, "x"};
  const auto license_diff = secure_frontend::diff_manifests(installed, relicensed);
  const std::vector<std::string> license_expected{
      "license url: https://h.example/l -> https://h.example/l2",
      "notice: n -> m",
      "bytes to download: 0"};
  const auto has = [&](const std::string& line) {
    return std::find(license_diff.lines.begin(), license_diff.lines.end(), line) != license_diff.lines.end();
  };
  expect(license_diff.license_changed, "a url or per-file license change requires an interactive y");
  expect(has(license_expected[0]) && has(license_expected[1]) && has(license_expected[2]), "license url and notice shown old -> new");
  // The override is compared with the effective license of the other side (top level here), field by field.
  expect(has("file weights/a.gguf license: Apache-2.0 -> BSD-3-Clause"), "override id vs effective top-level id");
  expect(has("file weights/a.gguf license url: https://h.example/l -> https://h.example/b"), "override url vs effective url");
  expect(has("file weights/a.gguf bundle_allowed: true -> false"), "effective bundle_allowed flip shown (appearing override)");
  expect(has("file weights/a.gguf notice: n -> x"), "override notice vs effective notice");
  expect(std::count_if(license_diff.lines.begin(), license_diff.lines.end(),
                       [](const std::string& l) { return l.rfind("file pack/", 0) == 0; }) == 0,
         "files without an override on either side add no per-file license lines");

  // Reverse: an override that allowed bundling vanishes while the top level forbids it.
  auto before = installed;
  before.license.bundle_allowed = false;
  before.files[0].license = secure_frontend::ManifestLicense{"Apache-2.0", "https://h.example/l", true, "n"};
  auto after = before;
  after.files[0].license.reset();
  const auto reverse = secure_frontend::diff_manifests(before, after);
  expect(reverse.license_changed, "a vanished override that flips effective bundle_allowed requires a y");
  expect((reverse.lines == std::vector<std::string>{"file weights/a.gguf bundle_allowed: true -> false", "bytes to download: 0"}),
         "vanishing override shows the effective bundle_allowed flip");

  // An override identical to the top-level license is not a change.
  auto same = installed;
  same.files[0].license = installed.license;
  const auto same_diff = secure_frontend::diff_manifests(installed, same);
  expect(!same_diff.license_changed && same_diff.lines == std::vector<std::string>{"bytes to download: 0"},
         "an override equal to the top-level license changes nothing effective");
  expect(license_diff.reusable_files.size() == 4, "a license change does not force a re-download");
}

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"exit_classes_match_spec_table", exit_classes_match_spec_table},
      {"reason_table_is_complete_and_ordered", reason_table_is_complete_and_ordered},
      {"error_text_states_resumable", error_text_states_resumable},
      {"version_dir_name_unique_for_same_version_label", version_dir_name_unique_for_same_version_label},
      {"staging_plan_classifies_partials", staging_plan_classifies_partials},
      {"abandoned_keys_match_same_model_only", abandoned_keys_match_same_model_only},
      {"update_diff_lists_changed_files_and_license", update_diff_lists_changed_files_and_license},
      {"concurrent_pull_same_manifest_second_is_models_busy", acquire_cases::concurrent_pull_same_manifest_second_is_models_busy},
      {"part_file_create_append_resume_truncate", acquire_cases::part_file_create_append_resume_truncate},
      {"part_file_write_at_fills_offsets", acquire_cases::part_file_write_at_fills_offsets},
      {"rename_within_never_replaces", acquire_cases::rename_within_never_replaces},
      {"rename_fallback_never_replaces", acquire_cases::rename_fallback_never_replaces},
      {"remove_tree_removes_links_without_following", acquire_cases::remove_tree_removes_links_without_following},
      {"hash_into_rejects_hard_links_and_counts_bytes", acquire_cases::hash_into_rejects_hard_links_and_counts_bytes},
      {"hash_into_stops_when_interrupted", acquire_cases::hash_into_stops_when_interrupted},
      {"store_environment_seams_report_space_and_fs_kind", acquire_cases::store_environment_seams_report_space_and_fs_kind},
      {"overlong_joined_paths_fail_closed_without_throwing", acquire_cases::overlong_joined_paths_fail_closed_without_throwing},
      {"dl04_publish_uses_noreplace_and_never_overwrites", acquire_cases::dl04_publish_uses_noreplace_and_never_overwrites},
      {"publish_falls_back_when_noreplace_unsupported", acquire_cases::publish_falls_back_when_noreplace_unsupported},
#if defined(SECURE_FRONTEND_TEST_HOOKS)
      {"dl04_crash_between_publish_and_index_is_not_loaded", acquire_cases::dl04_crash_between_publish_and_index_is_not_loaded},
#endif
      {"dl04_orphan_adopted_after_rehash_by_digest", acquire_cases::dl04_orphan_adopted_after_rehash_by_digest},
      {"replace_loaded_model_retires_old_version", acquire_cases::replace_loaded_model_retires_old_version},
      {"layout_check_requires_pack_files_and_valid_tokenizer", acquire_cases::layout_check_requires_pack_files_and_valid_tokenizer},
      {"gemma4_layout_check_requires_an_spm_tokenizer_pack", acquire_cases::gemma4_layout_check_requires_an_spm_tokenizer_pack},
      {"staged_tree_must_be_exactly_the_manifest", acquire_cases::staged_tree_must_be_exactly_the_manifest},
      {"install_decisions_follow_sequence_rules", acquire_cases::install_decisions_follow_sequence_rules},
      {"publish_requires_staging_key_of_manifest", acquire_cases::publish_requires_staging_key_of_manifest},
      {"overlong_staging_tree_is_publish_failed", acquire_cases::overlong_staging_tree_is_publish_failed},
      {"retire_removes_read_only_published_version", acquire_cases::retire_removes_read_only_published_version},
      {"downgrade_adopts_retired_version_keeping_one_live", acquire_cases::downgrade_adopts_retired_version_keeping_one_live},
      {"rm_refuses_held_version_and_removes_read_only_tree", acquire_cases::rm_refuses_held_version_and_removes_read_only_tree},
      {"dl02_correct_digest_publishes_atomically", acquire_cases::dl02_correct_digest_publishes_atomically},
      {"dl02_byte_limit_enforced_while_streaming", acquire_cases::dl02_byte_limit_enforced_while_streaming},
      {"dl02_resume_with_range_completes_and_verifies", acquire_cases::dl02_resume_with_range_completes_and_verifies},
      {"resume_offset_ignores_preallocation", acquire_cases::resume_offset_ignores_preallocation},
      {"dl02_published_files_are_private", acquire_cases::dl02_published_files_are_private},
      {"dl02_preflight_refuses_without_space", acquire_cases::dl02_preflight_refuses_without_space},
      {"dl03_untrusted_manifest_digest_rejected", acquire_cases::dl03_untrusted_manifest_digest_rejected},
      {"dl03_downgrade_rejected", acquire_cases::dl03_downgrade_rejected},
      {"dl03_sequence_conflict_rejected", acquire_cases::dl03_sequence_conflict_rejected},
      {"dl04_interrupt_during_download_never_indexed", acquire_cases::dl04_interrupt_during_download_never_indexed},
      {"dl04_interrupt_during_verification", acquire_cases::dl04_interrupt_during_verification},
      {"dl04_stale_staging_from_other_manifest_discarded", acquire_cases::dl04_stale_staging_from_other_manifest_discarded},
      {"abandoned_partials_kept_with_yes", acquire_cases::abandoned_partials_kept_with_yes},
      {"abandoned_staging_reported_and_offered_for_deletion", acquire_cases::abandoned_staging_reported_and_offered_for_deletion},
      {"all_sources_failed_exits_3_and_keeps_part", acquire_cases::all_sources_failed_exits_3_and_keeps_part},
      {"enospc_mid_stream_keeps_part_and_exits_6", acquire_cases::enospc_mid_stream_keeps_part_and_exits_6},
      {"update_reuses_unchanged_files", acquire_cases::update_reuses_unchanged_files},
      {"update_reuse_copy_interrupted_mid_file_exits_8", acquire_cases::update_reuse_copy_interrupted_mid_file_exits_8},
      {"pull_verify_interrupted_exits_8", acquire_cases::pull_verify_interrupted_exits_8},
      {"license_change_requires_interactive_yes", acquire_cases::license_change_requires_interactive_yes},
      {"pull_without_bundle_refuses_sourceless_files", acquire_cases::pull_without_bundle_refuses_sourceless_files},
      {"confirmation_answers_parsed_strictly", acquire_cases::confirmation_answers_parsed_strictly},
      {"already_installed_needs_no_network", acquire_cases::already_installed_needs_no_network},
      {"pull_budgets_map_to_exit_classes", acquire_cases::pull_budgets_map_to_exit_classes},
      {"discard_partial_runs_only_after_confirmation", acquire_cases::discard_partial_runs_only_after_confirmation},
      {"unreadable_installed_license_requires_interactive_yes", acquire_cases::unreadable_installed_license_requires_interactive_yes},
      {"program_update_exits_12_without_network", acquire_cases::program_update_exits_12_without_network},
      {"cli_requires_config_and_known_verb", acquire_cases::cli_requires_config_and_known_verb},
      {"list_and_show_report_installed_models", acquire_cases::list_and_show_report_installed_models},
      {"store_option_names_another_store", acquire_cases::store_option_names_another_store},
      {"list_all_reports_retired_orphaned_and_staging", acquire_cases::list_all_reports_retired_orphaned_and_staging},
      {"rm_refuses_loaded_model", acquire_cases::rm_refuses_loaded_model},
      {"rm_retired_deletes_unheld_versions", acquire_cases::rm_retired_deletes_unheld_versions},
      {"rm_partials_deletes_other_staging", acquire_cases::rm_partials_deletes_other_staging},
      {"version_names_frontend_and_engine_versions", acquire_cases::version_names_frontend_and_engine_versions},
      {"key_show_requires_local_token_and_never_logs", acquire_cases::key_show_requires_local_token_and_never_logs},
#if defined(SECURE_FRONTEND_TEST_HOOKS)
      {"list_outdated_uses_catalog_sequence", acquire_cases::list_outdated_uses_catalog_sequence},
#endif
      {"pull_cli_prints_summary_and_installs", acquire_cases::pull_cli_prints_summary_and_installs},
      {"pull_progress_lines_are_machine_readable", acquire_cases::pull_progress_lines_are_machine_readable},
      {"hf_info_resolves_a_link", acquire_cases::hf_info_resolves_a_link},
      {"run_installed_model_needs_no_network", acquire_cases::run_installed_model_needs_no_network},
      {"run_refuses_glm53_without_engine", acquire_cases::run_refuses_glm53_without_engine},
      {"engine_gate_refuses_gemma4_until_the_engine_exists", acquire_cases::engine_gate_refuses_gemma4_until_the_engine_exists},
#if defined(SECURE_FRONTEND_TEST_HOOKS)
      {"run_missing_model_non_interactive_exits_2", acquire_cases::run_missing_model_non_interactive_exits_2},
      {"run_missing_model_prompts_and_decline_exits_2", acquire_cases::run_missing_model_prompts_and_decline_exits_2},
      {"update_models_pulls_outdated_catalog_models", acquire_cases::update_models_pulls_outdated_catalog_models},
#endif
#if !defined(_WIN32) && defined(SECURE_FRONTEND_TEST_HOOKS)
      {"dl04_sigint_during_download_exits_8", acquire_cases::dl04_sigint_during_download_exits_8},
      {"dl04_sigterm_during_download_exits_8", acquire_cases::dl04_sigterm_during_download_exits_8},
      {"interrupt_at_confirmation_prompt_exits_8", acquire_cases::interrupt_at_confirmation_prompt_exits_8},
#endif
#if defined(_WIN32) && defined(SECURE_FRONTEND_TEST_HOOKS)
      {"dl04_interrupt_during_download_never_indexed_windows", acquire_cases::dl04_interrupt_during_download_never_indexed_windows},
#endif
#if defined(SECURE_FRONTEND_TEST_HOOKS)
      {"dl03_third_party_cannot_shadow_catalog", acquire_cases::dl03_third_party_cannot_shadow_catalog},
      {"pull_test_cacert_reaches_argv_in_hook_builds", acquire_cases::pull_test_cacert_reaches_argv_in_hook_builds},
      {"publish_failure_after_step_2_resumes_without_download", acquire_cases::publish_failure_after_step_2_resumes_without_download},
#endif
#if defined(_WIN32)
      {"dl02_published_files_have_private_dacl_windows", acquire_cases::dl02_published_files_have_private_dacl_windows},
#endif
#if !defined(_WIN32)
      {"rm_reports_unreachable_version_as_failure", acquire_cases::rm_reports_unreachable_version_as_failure},
      {"rm_failed_delete_stays_retired", acquire_cases::rm_failed_delete_stays_retired},
#endif
#if defined(_WIN32)
      {"replace_loaded_model_retires_old_version_windows", acquire_cases::replace_loaded_model_retires_old_version_windows},
      {"rm_open_version_stays_retired_windows", acquire_cases::rm_open_version_stays_retired_windows},
      {"store_primitives_windows", acquire_cases::store_primitives_windows},
      {"store_adopts_only_private_files_windows", acquire_cases::store_adopts_only_private_files_windows},
#endif
  }, secure_frontend_test::failures);
}
