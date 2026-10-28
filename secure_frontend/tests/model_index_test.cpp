#include "secure_frontend/acquire_reason.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/catalog.hpp"
#include "secure_frontend/engine_gate.hpp"
#include "secure_frontend/manifest_json.hpp"
#include "secure_frontend/model_index_model.hpp"
#include "secure_frontend/model_index_store.hpp"
#include "secure_frontend/model_index_write.hpp"
#include "secure_frontend/sha256.hpp"
#include "support/test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

using secure_frontend::IndexEntry;
using secure_frontend::ModelIndex;
using secure_frontend::ModelOrigin;

IndexEntry entry(const std::string& name, const std::string& tag, const std::uint32_t sequence, const char digit) {
  const std::string digest(64, digit);
  return IndexEntry{name, tag, "2026.10.14." + std::to_string(sequence), sequence,
                    secure_frontend::version_directory_name(sequence, digest), digest, ModelOrigin::catalog};
}

void index_render_is_deterministic() {
  ModelIndex index;
  index = secure_frontend::with_installed(index, entry("zeta", "a", 1, 'b'));
  index = secure_frontend::with_installed(index, entry("alpha", "b", 2, 'c'));
  const std::string expected =
      "{\"format\":\"strata-model-index\",\"schema_version\":1,\"models\":["
      "{\"name\":\"alpha\",\"tag\":\"b\",\"version\":\"2026.10.14.2\",\"sequence\":2,\"dir\":\"v2-cccccccccccc\","
      "\"manifest_sha256\":\"" + std::string(64, 'c') + "\",\"origin\":\"catalog\"},"
      "{\"name\":\"zeta\",\"tag\":\"a\",\"version\":\"2026.10.14.1\",\"sequence\":1,\"dir\":\"v1-bbbbbbbbbbbb\","
      "\"manifest_sha256\":\"" + std::string(64, 'b') + "\",\"origin\":\"catalog\"}],\"retired\":[]}\n";
  expect(secure_frontend::render_model_index(index) == expected, "sorted, compact, one trailing newline");
}

void install_retires_previous_version() {
  ModelIndex index = secure_frontend::with_installed({}, entry("m", "t", 1, 'a'));
  index = secure_frontend::with_installed(index, entry("m", "t", 2, 'b'));
  expect(index.models.size() == 1 && index.models.front().sequence == 2, "one entry per name:tag");
  expect(index.retired.size() == 1 && index.retired.front().dir == "v1-aaaaaaaaaaaa", "old version retired");
  index = secure_frontend::without_retired(index, "m", "t", "v1-aaaaaaaaaaaa");
  expect(index.retired.empty(), "retired entry removed");
  index = secure_frontend::without_model(index, "m", "t");
  expect(index.models.empty() && secure_frontend::find_model(index, "m", "t") == nullptr, "model removed");
}

void reinstall_same_version_does_not_retire() {
  ModelIndex index = secure_frontend::with_installed({}, entry("m", "t", 1, 'a'));
  index = secure_frontend::with_installed(index, entry("m", "t", 1, 'a'));
  expect(index.models.size() == 1 && index.retired.empty(), "--verify re-publish of the same dir retires nothing");
}

void downgrade_to_retired_version_leaves_it_live_only() {
  ModelIndex index = secure_frontend::with_installed({}, entry("m", "t", 1, 'a'));
  index = secure_frontend::with_installed(index, entry("m", "t", 2, 'b'));
  index = secure_frontend::with_installed(index, entry("m", "t", 1, 'a'));
  expect(index.models.size() == 1 && index.models.front().dir == "v1-aaaaaaaaaaaa", "v1 is live again");
  expect(index.retired.size() == 1 && index.retired.front().dir == "v2-bbbbbbbbbbbb", "only v2 is retired; v1 is not");
  index = secure_frontend::with_installed(index, entry("m", "t", 2, 'b'));
  index = secure_frontend::with_installed(index, entry("m", "t", 1, 'a'));
  expect(index.retired.size() == 1, "repeated swaps leave no duplicate retired rows");
}

void make_dirs(const std::filesystem::path& directory) {
  if (directory.empty() || std::filesystem::exists(directory)) return;
  make_dirs(directory.parent_path());
  secure_frontend_test::make_private_directory(directory);
}

void put(const std::filesystem::path& path, const std::string_view bytes) {
  make_dirs(path.parent_path());
  secure_frontend_test::write_file(path, bytes);
}

// Spec §4.9 delta b.
void scan_tolerates_repeated_shard_names_across_versions() {
  const auto root_path = secure_frontend_test::make_scratch_directory();
  put(root_path / "m" / "t" / "v1-aaaaaaaaaaaa" / "weights" / "m-00001-of-00001.gguf", "one");
  put(root_path / "m" / "t" / "v2-bbbbbbbbbbbb" / "weights" / "m-00001-of-00001.gguf", "two!");
  put(root_path / ".staging" / std::string(32, 'c') / "weights" / "m-00001-of-00001.gguf.part", "x");
  auto opened = secure_frontend::open_root(root_path);
  const auto* root = std::get_if<secure_frontend::CanonicalRoot>(&opened);
  expect(root != nullptr, "test setup: the scratch root opens");
  if (root == nullptr) return;
  const auto scanned = secure_frontend::scan_model_root_files(*root);
  const auto* files = std::get_if<std::vector<secure_frontend::ModelRootFile>>(&scanned);
  expect(files != nullptr && files->size() == 3, "every regular file, duplicate names included");
  if (files != nullptr && files->size() == 3) {
    expect((*files)[0].path == ".staging/" + std::string(32, 'c') + "/weights/m-00001-of-00001.gguf.part" &&
               (*files)[0].size == 1,
           "staging is scanned");
    expect((*files)[1].path == "m/t/v1-aaaaaaaaaaaa/weights/m-00001-of-00001.gguf" && (*files)[1].size == 3, "sorted paths, sizes");
    expect((*files)[2].size == 4, "second version");
  }
}

const char* kTinyManifest =
    "{\"format\":\"strata-model-manifest\",\"schema_version\":1,\"name\":\"m\",\"tag\":\"t\",\"version\":\"2026.10.14.1\","
    "\"sequence\":1,\"profile\":\"glm53\",\"engine\":{\"family\":\"glm\",\"min_version\":\"0.0.1\"},"
    "\"license\":{\"id\":\"MIT\",\"url\":\"https://h.example/l\",\"bundle_allowed\":true},\"hosts\":[\"h.example\"],"
    "\"files\":[{\"path\":\"w.gguf\",\"role\":\"weights\",\"size\":1,"
    "\"sha256\":\"ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb\",\"sources\":[]}],"
    "\"launch\":{\"native\":\"w.gguf\"}}";

secure_frontend::CanonicalRoot models_root(const std::filesystem::path& path) {
  return std::get<secure_frontend::CanonicalRoot>(secure_frontend::open_root(path));
}

secure_frontend::IndexEntry install_tiny(const std::filesystem::path& root_path) {
  const std::string digest = secure_frontend::to_hex(secure_frontend::sha256(kTinyManifest));
  const std::string dir = secure_frontend::version_directory_name(1, digest);
  put(root_path / "m" / "t" / dir / "manifest.json", kTinyManifest);
  put(root_path / "m" / "t" / dir / "w.gguf", "a");
  return secure_frontend::IndexEntry{"m", "t", "2026.10.14.1", 1, dir, digest, secure_frontend::ModelOrigin::third_party};
}

void index_round_trip_through_disk() {
  const auto root_path = secure_frontend_test::make_scratch_directory();
  const auto root = models_root(root_path);
  const auto empty = secure_frontend::read_model_index(root);
  expect(std::holds_alternative<secure_frontend::ModelIndex>(empty) && std::get<secure_frontend::ModelIndex>(empty).models.empty(),
         "a missing index.json is an empty index");
  const auto entry = install_tiny(root_path);
  const auto index = secure_frontend::with_installed({}, entry);
  expect(!secure_frontend::write_model_index(root, index), "written atomically");
  const auto read = secure_frontend::read_model_index(root);
  expect(std::holds_alternative<secure_frontend::ModelIndex>(read) &&
             secure_frontend::render_model_index(std::get<secure_frontend::ModelIndex>(read)) == secure_frontend::render_model_index(index),
         "round trip");
  const auto installed = secure_frontend::load_installed(root, entry);
  expect(std::holds_alternative<secure_frontend::InstalledModel>(installed) &&
             std::get<secure_frontend::InstalledModel>(installed).version_dir.generic() == "m/t/" + entry.dir,
         "installed model loads, manifest re-hashed");
}

void index_inconsistent_manifest_fails_closed() {
  const auto root_path = secure_frontend_test::make_scratch_directory();
  const auto root = models_root(root_path);
  const auto entry = install_tiny(root_path);
  secure_frontend_test::write_file(root_path / "m" / "t" / entry.dir / "manifest.json", std::string{kTinyManifest} + " ");
  const auto installed = secure_frontend::load_installed(root, entry);
  expect(std::holds_alternative<secure_frontend::AcquireError>(installed) &&
             std::get<secure_frontend::AcquireError>(installed).reason == secure_frontend::AcquireReason::index_inconsistent,
         "a manifest.json that no longer hashes to the index entry fails closed");
}

void index_rejects_unknown_keys_and_bad_dirs() {
  using secure_frontend::AcquireReason;
  const std::string digest(64, 'a');
  const std::string entry = "{\"name\":\"m\",\"tag\":\"t\",\"version\":\"2026.10.14.1\",\"sequence\":1,"
                            "\"dir\":\"v1-aaaaaaaaaaaa\",\"manifest_sha256\":\"" + digest + "\",\"origin\":\"catalog\"}";
  const std::string prefix = "{\"format\":\"strata-model-index\",\"schema_version\":1,\"models\":[";
  const std::string suffix = "],\"retired\":[]}\n";
  const std::string good = prefix + entry + suffix;
  expect(std::holds_alternative<secure_frontend::ModelIndex>(secure_frontend::parse_model_index(good)), "good index");
  for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
           {"\"retired\":[]", "\"retired\":[],\"x\":1"}, {"v1-aaaaaaaaaaaa", "v2-aaaaaaaaaaaa"}, {"\"catalog\"", "\"other\""},
           {"strata-model-index", "strata-model-indexes"}, {"\"name\":\"m\"", "\"name\":\"../m\""}}) {
    std::string bad = good;
    bad.replace(bad.find(from), from.size(), to);
    const auto parsed = secure_frontend::parse_model_index(bad);
    expect(std::holds_alternative<secure_frontend::AcquireError>(parsed) &&
               std::get<secure_frontend::AcquireError>(parsed).reason == AcquireReason::index_inconsistent,
           "index_inconsistent for " + to);
  }
  const std::string twice = prefix + entry + "," + entry + suffix;
  expect(std::holds_alternative<secure_frontend::AcquireError>(secure_frontend::parse_model_index(twice)), "one entry per name:tag");
}

// Beyond the brief: version labels, retired dirs and hand-built entries are as strict as models dirs.
void index_rejects_bad_versions_and_retired_dirs() {
  using secure_frontend::AcquireReason;
  const std::string digest(64, 'a');
  const std::string prefix = "{\"format\":\"strata-model-index\",\"schema_version\":1,\"models\":[";
  const std::string model = "{\"name\":\"m\",\"tag\":\"t\",\"version\":\"2026.10.14.1\",\"sequence\":1,"
                            "\"dir\":\"v1-aaaaaaaaaaaa\",\"manifest_sha256\":\"" + digest + "\",\"origin\":\"catalog\"}";
  const auto retired = [&](const std::string& dir) {
    return "{\"name\":\"m\",\"tag\":\"t\",\"dir\":\"" + dir + "\",\"manifest_sha256\":\"" + digest + "\"}";
  };
  const auto is_inconsistent = [](const std::string& text) {
    const auto parsed = secure_frontend::parse_model_index(text);
    return std::holds_alternative<secure_frontend::AcquireError>(parsed) &&
           std::get<secure_frontend::AcquireError>(parsed).reason == AcquireReason::index_inconsistent;
  };
  expect(!is_inconsistent(prefix + model + "],\"retired\":[" + retired("v7-aaaaaaaaaaaa") + "]}\n"), "a retired v7 dir");
  for (const auto* dir : {"v/../../x-aaaaaaaaaaaa", "v-aaaaaaaaaaaa", "v0-aaaaaaaaaaaa", "v01-aaaaaaaaaaaa",
                          "v2147483648-aaaaaaaaaaaa", "vx-aaaaaaaaaaaa", "v1-aaaaaaaaaaab"}) {
    expect(is_inconsistent(prefix + model + "],\"retired\":[" + retired(dir) + "]}\n"), std::string{"retired dir "} + dir);
  }
  std::string bad_version = prefix + model + "],\"retired\":[]}\n";
  bad_version.replace(bad_version.find("2026.10.14.1"), 12, "2026.13.14.1");
  expect(is_inconsistent(bad_version), "version must be a YYYY.MM.DD.N label");
  const auto root_path = secure_frontend_test::make_scratch_directory();
  const auto root = models_root(root_path);
  auto entry = install_tiny(root_path);
  entry.dir = "../x";
  const auto installed = secure_frontend::load_installed(root, entry);
  expect(std::holds_alternative<secure_frontend::AcquireError>(installed), "a hand-built bad entry fails closed, never throws");
}

// SP4 Task 23 / F-58: one lookup for `strata` verbs and the managed launch (Task 24b).
void find_installed_matches_name_and_tag() {
  ModelIndex index = secure_frontend::with_installed({}, entry("m", "t", 1, 'a'));
  const auto ref = [](const std::string_view text) { return *secure_frontend::parse_model_reference(text); };
  const auto found = secure_frontend::find_installed(index, ref("m:t"));
  expect(std::holds_alternative<IndexEntry>(found) && std::get<IndexEntry>(found).tag == "t", "name:tag");
  expect(std::holds_alternative<IndexEntry>(secure_frontend::find_installed(index, ref("m"))), "name alone, one tag installed");
  const auto unknown = secure_frontend::find_installed(index, ref("m:u"));
  expect(std::holds_alternative<secure_frontend::AcquireError>(unknown) &&
             std::get<secure_frontend::AcquireError>(unknown).reason == secure_frontend::AcquireReason::model_unknown,
         "absent: model_unknown");
  index = secure_frontend::with_installed(index, entry("m", "u", 1, 'b'));
  const auto ambiguous = secure_frontend::find_installed(index, ref("m"));
  expect(std::holds_alternative<secure_frontend::AcquireError>(ambiguous) &&
             std::get<secure_frontend::AcquireError>(ambiguous).reason == secure_frontend::AcquireReason::usage_error,
         "several tags: usage_error");
}

// SP4 §2.3 / F-58: the single gate list/show (Task 23) and run/serve (Task 24b, 27) share; it honours family.
void engine_gate_honours_profile_family_and_version() {
  secure_frontend::AcquireManifest m;
  m.profile = secure_frontend::ManifestProfile::qwen;
  m.family = secure_frontend::ManifestFamily::strata;
  m.min_version = secure_frontend::built_engine_version();
  expect(!secure_frontend::engine_gate(m).has_value(), "qwen on strata at this release's version runs");
  m.family = secure_frontend::ManifestFamily::glm;
  const auto wrong_family = secure_frontend::engine_gate(m);
  expect(wrong_family && wrong_family->reason == secure_frontend::AcquireReason::engine_profile_unavailable, "family glm refused");
  m.family = secure_frontend::ManifestFamily::strata;
  m.profile = secure_frontend::ManifestProfile::glm53;
  const auto glm = secure_frontend::engine_gate(m);
  expect(glm && glm->reason == secure_frontend::AcquireReason::engine_profile_unavailable, "glm53 until SP6");
  m.profile = secure_frontend::ManifestProfile::qwen;
  m.min_version = *secure_frontend::parse_engine_version("9999.0.0");
  const auto old = secure_frontend::engine_gate(m);
  expect(old && old->reason == secure_frontend::AcquireReason::engine_too_old &&
             old->detail.find("needs engine strata >= 9999.0.0") != std::string::npos,
         "a newer min_version: engine_too_old");
  expect(secure_frontend::engine_state_text(m) == "needs engine strata >= 9999.0.0", "state text for list/show");
  m.min_version = secure_frontend::built_engine_version();
  expect(secure_frontend::engine_state_text(m) == "runnable", "runnable");
}

#if !defined(_WIN32)
// Task 15 carry: what scan_model_root_files skips (links, non-P1 names) is counted for list --all / doctor.
void scan_counts_skipped_entries() {
  const auto root_path = secure_frontend_test::make_scratch_directory();
  put(root_path / "m" / "t" / "v1-aaaaaaaaaaaa" / "w.gguf", "one");
  std::filesystem::create_directory_symlink(root_path / "m", root_path / "link");
  put(root_path / "m" / "bad:name", "x");
  const auto root = models_root(root_path);
  const auto scanned = secure_frontend::scan_model_root(root);
  const auto* scan = std::get_if<secure_frontend::ModelRootScan>(&scanned);
  expect(scan != nullptr && scan->files.size() == 1 && scan->skipped == 2, "one file; the link and the non-P1 name are counted");
  const auto files = secure_frontend::scan_model_root_files(root);
  expect(std::get<std::vector<secure_frontend::ModelRootFile>>(files).size() == 1, "scan_model_root_files unchanged");
}
#endif

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"index_render_is_deterministic", index_render_is_deterministic},
      {"install_retires_previous_version", install_retires_previous_version},
      {"reinstall_same_version_does_not_retire", reinstall_same_version_does_not_retire},
      {"downgrade_to_retired_version_leaves_it_live_only", downgrade_to_retired_version_leaves_it_live_only},
      {"scan_tolerates_repeated_shard_names_across_versions", scan_tolerates_repeated_shard_names_across_versions},
      {"index_round_trip_through_disk", index_round_trip_through_disk},
      {"index_inconsistent_manifest_fails_closed", index_inconsistent_manifest_fails_closed},
      {"index_rejects_unknown_keys_and_bad_dirs", index_rejects_unknown_keys_and_bad_dirs},
      {"index_rejects_bad_versions_and_retired_dirs", index_rejects_bad_versions_and_retired_dirs},
      {"find_installed_matches_name_and_tag", find_installed_matches_name_and_tag},
      {"engine_gate_honours_profile_family_and_version", engine_gate_honours_profile_family_and_version},
#if !defined(_WIN32)
      {"scan_counts_skipped_entries", scan_counts_skipped_entries},
#endif
  }, failures);
}
