#include "bundle_cases.hpp"
#include "secure_frontend/bundle_text.hpp"
#include "support/expect.hpp"
#include "support/test_support.hpp"

#include <string>
#include <vector>

namespace {

using secure_frontend_test::expect;

secure_frontend::AcquireManifest sample() {
  secure_frontend::AcquireManifest m;
  m.files = {secure_frontend::ManifestFile{"weights/b.gguf", secure_frontend::FileRole::weights, 5, std::string(64, 'b'),
                                           {"https://h.example/b"}, {}},
             secure_frontend::ManifestFile{"pack/a.txt", secure_frontend::FileRole::pack, 5, std::string(64, 'a'), {}, {}},
             secure_frontend::ManifestFile{"Pack/Z.txt", secure_frontend::FileRole::pack, 5, std::string(64, 'c'), {}, {}}};
  return m;
}

void sha256sums_render_sorted_bytewise() {
  const std::string manifest_digest(64, 'f');
  const std::string expected = std::string(64, 'c') + "  files/Pack/Z.txt\n" + std::string(64, 'a') + "  files/pack/a.txt\n" +
                               std::string(64, 'b') + "  files/weights/b.gguf\n" + manifest_digest + "  manifest.json\n";
  expect(secure_frontend::render_sha256sums(manifest_digest, sample()) == expected,
         "bytewise order: files/Pack < files/pack < files/weights < manifest.json");
}

void sha256sums_parse_is_strict() {
  const std::string good = std::string(64, 'a') + "  files/x\n" + std::string(64, 'b') + "  manifest.json\n";
  expect(secure_frontend::parse_sha256sums(good).has_value(), "well-formed");
  for (const std::string& bad : {std::string(64, 'a') + " files/x\n", std::string(64, 'A') + "  files/x\n",
                                 std::string(64, 'a') + "  files/x", std::string(64, 'a') + "  files/x\r\n",
                                 std::string(64, 'b') + "  manifest.json\n" + std::string(64, 'a') + "  files/x\n",
                                 std::string(64, 'a') + "  files/x\n" + std::string(64, 'a') + "  files/x\n", std::string{}}) {
    expect(!secure_frontend::parse_sha256sums(bad).has_value(), "rejected sums text");
  }
}

void sums_agreement_rules() {
  const std::string manifest_digest(64, 'f');
  const auto lines = *secure_frontend::parse_sha256sums(secure_frontend::render_sha256sums(manifest_digest, sample()));
  expect(secure_frontend::sums_agree(lines, manifest_digest, sample()), "rendered sums agree");
  expect(!secure_frontend::sums_agree(lines, std::string(64, 'e'), sample()), "manifest digest differs");
  auto changed = sample();
  changed.files[0].sha256 = std::string(64, 'd');
  expect(!secure_frontend::sums_agree(lines, manifest_digest, changed), "a file digest differs");
  auto fewer = lines;
  fewer.pop_back();
  expect(!secure_frontend::sums_agree(fewer, manifest_digest, sample()), "a line is missing");
}

void missing_list_names_sourceless_files() {
  expect(secure_frontend::render_missing(sample()) == "Pack/Z.txt\npack/a.txt\n", "sorted sourceless paths");
  const auto parsed = secure_frontend::parse_missing("Pack/Z.txt\npack/a.txt\n");
  expect(parsed && parsed->size() == 2, "parse");
  expect(!secure_frontend::parse_missing("a\n\nb\n").has_value(), "empty line rejected");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"sha256sums_render_sorted_bytewise", sha256sums_render_sorted_bytewise},
      {"sha256sums_parse_is_strict", sha256sums_parse_is_strict},
      {"sums_agreement_rules", sums_agreement_rules},
      {"missing_list_names_sourceless_files", missing_list_names_sourceless_files},
      {"dl02_import_bundle_publishes_same_as_pull", bundle_cases::dl02_import_bundle_publishes_same_as_pull},
      {"dl03_bundle_digest_mismatch_publishes_nothing", bundle_cases::dl03_bundle_digest_mismatch_publishes_nothing},
      {"bundle_rejects_traversal_path", bundle_cases::bundle_rejects_traversal_path},
      {"bundle_rejects_unlisted_file_in_files_dir", bundle_cases::bundle_rejects_unlisted_file_in_files_dir},
      {"bundle_sha256sums_must_match_manifest", bundle_cases::bundle_sha256sums_must_match_manifest},
      {"bundle_rejects_hardlinked_file", bundle_cases::bundle_rejects_hardlinked_file},
      {"export_refuses_bundle_forbidden_license", bundle_cases::export_refuses_bundle_forbidden_license},
      {"export_refuses_fat32_target_for_large_file", bundle_cases::export_refuses_fat32_target_for_large_file},
      {"export_rehashes_and_rejects_bit_rot", bundle_cases::export_rehashes_and_rejects_bit_rot},
      {"verify_writes_nothing", bundle_cases::verify_writes_nothing},
      {"import_from_read_only_bundle_directory", bundle_cases::import_from_read_only_bundle_directory},
      {"bundle_incomplete_and_marker_rules", bundle_cases::bundle_incomplete_and_marker_rules},
      {"export_onto_shared_writable_target", bundle_cases::export_onto_shared_writable_target},
      {"export_refuses_when_one_file_license_forbids_bundle", bundle_cases::export_refuses_when_one_file_license_forbids_bundle},
      {"bundle_directory_must_not_be_a_volume_root", bundle_cases::bundle_directory_must_not_be_a_volume_root},
      {"make_bundle_tool_output_opens_verifies_and_imports", bundle_cases::make_bundle_tool_output_opens_verifies_and_imports},
      {"pull_bundle_resumes_after_interrupt", bundle_cases::pull_bundle_resumes_after_interrupt},
      {"pull_bundle_incomplete_without_pack_sources", bundle_cases::pull_bundle_incomplete_without_pack_sources},
      {"pull_bundle_resume_rehashes_files_from_earlier_run", bundle_cases::pull_bundle_resume_rehashes_files_from_earlier_run},
      {"import_works_without_downloader", bundle_cases::import_works_without_downloader},
      {"cli_verify_export_import_round_trip", bundle_cases::cli_verify_export_import_round_trip},
      {"failed_export_says_to_remove_target", bundle_cases::failed_export_says_to_remove_target},
      {"import_blocked_by_orphan_says_how_to_clear_it", bundle_cases::import_blocked_by_orphan_says_how_to_clear_it},
      {"import_license_change_needs_interactive_yes", bundle_cases::import_license_change_needs_interactive_yes},
      {"import_with_unreadable_installed_version_needs_interactive_yes",
       bundle_cases::import_with_unreadable_installed_version_needs_interactive_yes},
      {"import_over_corrupt_orphan_reports_target_exists", bundle_cases::import_over_corrupt_orphan_reports_target_exists},
#if defined(_WIN32)
      {"bundle_rejects_junction_entry_windows", bundle_cases::bundle_rejects_junction_entry_windows},
#else
      {"bundle_rejects_symlink_entry", bundle_cases::bundle_rejects_symlink_entry},
      {"bundle_walk_fails_closed_on_overlong_or_odd_names", bundle_cases::bundle_walk_fails_closed_on_overlong_or_odd_names},
      {"bundle_at_mount_root_is_read_but_never_written", bundle_cases::bundle_at_mount_root_is_read_but_never_written},
#endif
  }, secure_frontend_test::failures);
}
