#include "acquire_cases.hpp"
#include "secure_frontend/acquire_publish.hpp"
#include "secure_frontend/model_index_store.hpp"
#include "secure_frontend/model_index_write.hpp"
#include "support/acquire_fixtures.hpp"
#include "support/expect.hpp"

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#endif

#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace acquire_cases {
namespace {

namespace fx = secure_frontend_test::acquire;
using secure_frontend::AcquireReason;
using secure_frontend_test::expect;

class FallbackEnvironment final : public secure_frontend::StoreEnvironment {
 public:
  bool force_rename_fallback() const override { return true; }
};

secure_frontend::ModelIndex index_of(const secure_frontend::CanonicalRoot& root) {
  return std::get<secure_frontend::ModelIndex>(secure_frontend::read_model_index(root));
}

// Stages and publishes name:tag at `sequence`; the published entry.
secure_frontend::IndexEntry publish(const secure_frontend::CanonicalRoot& root, const secure_frontend::TrustedManifest& manifest,
                                    const std::vector<fx::PayloadFile>& payload, secure_frontend::LoadedVersions& loaded) {
  const secure_frontend::StoreEnvironment env;
  fx::stage(root, manifest, payload);
  auto result = secure_frontend::publish_staged(root, secure_frontend::staging_key(manifest.parsed.sha256), manifest, loaded, env, true);
  if (!std::holds_alternative<secure_frontend::PublishResult>(result)) throw std::runtime_error("fixture publish failed");
  return std::get<secure_frontend::PublishResult>(result).entry;
}

}  // namespace

void dl04_publish_uses_noreplace_and_never_overwrites() {
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  const auto manifest = fx::trusted(fx::manifest_text("m", "t", 1, fx::qwen_payload()), secure_frontend::ModelOrigin::third_party);
  fx::stage(root, manifest, fx::qwen_payload());
  const std::string dir = secure_frontend::version_directory_name(1, manifest.parsed.sha256);
  expect(!secure_frontend::ensure_directory(root, secure_frontend::must_rel("m/t/" + dir)), "a squatting directory at the target");
  expect(!secure_frontend::write_new_file(root, secure_frontend::must_rel("m/t/" + dir + "/keep"), "k"), "with content");
  fx::FakeLoaded loaded;
  const secure_frontend::StoreEnvironment env;
  const auto result = secure_frontend::publish_staged(root, secure_frontend::staging_key(manifest.parsed.sha256), manifest, loaded, env, true);
  expect(std::holds_alternative<secure_frontend::AcquireError>(result), "publish refuses to replace an existing version directory");
  expect(secure_frontend::exists_within(root, secure_frontend::must_rel("m/t/" + dir + "/keep")), "existing content untouched");
  expect(index_of(root).models.empty(), "nothing indexed");
}

void publish_falls_back_when_noreplace_unsupported() {
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  const auto manifest = fx::trusted(fx::manifest_text("m", "t", 1, fx::qwen_payload()), secure_frontend::ModelOrigin::catalog);
  fx::stage(root, manifest, fx::qwen_payload());
  fx::FakeLoaded loaded;
  const FallbackEnvironment env;
  const auto result = secure_frontend::publish_staged(root, secure_frontend::staging_key(manifest.parsed.sha256), manifest, loaded, env, true);
  expect(std::holds_alternative<secure_frontend::PublishResult>(result), "fstatat + renameat fallback publishes");
  const auto index = index_of(root);
  expect(index.models.size() == 1 && index.models.front().origin == secure_frontend::ModelOrigin::catalog, "indexed");
  expect(!secure_frontend::exists_within(root, secure_frontend::staging_dir_path(secure_frontend::staging_key(manifest.parsed.sha256))),
         "staging key moved, not copied");
  expect(std::holds_alternative<secure_frontend::InstalledModel>(secure_frontend::load_installed(root, index.models.front())),
         "manifest.json in place and consistent");
  // The fallback never overwrites either: a second staging of the same manifest meets the published directory.
  fx::stage(root, manifest, fx::qwen_payload());
  const auto again = secure_frontend::publish_staged(root, secure_frontend::staging_key(manifest.parsed.sha256), manifest, loaded, env, true);
  expect(std::holds_alternative<secure_frontend::AcquireError>(again) &&
             std::get<secure_frontend::AcquireError>(again).reason == AcquireReason::publish_failed,
         "fallback refuses an existing target");
  expect(std::holds_alternative<secure_frontend::InstalledModel>(secure_frontend::load_installed(root, index_of(root).models.front())),
         "published version untouched");
}

#if defined(SECURE_FRONTEND_TEST_HOOKS)
void dl04_crash_between_publish_and_index_is_not_loaded() {
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  const auto manifest = fx::trusted(fx::manifest_text("m", "t", 1, fx::qwen_payload()), secure_frontend::ModelOrigin::third_party);
  fx::stage(root, manifest, fx::qwen_payload());
  fx::FakeLoaded loaded;
  const secure_frontend::StoreEnvironment env;
  secure_frontend::PublishOptions crash;
  crash.stop_after_version_rename = [] { return true; };
  const auto crashed = secure_frontend::publish_staged(root, secure_frontend::staging_key(manifest.parsed.sha256), manifest, loaded, env,
                                                       true, crash);
  expect(std::holds_alternative<secure_frontend::AcquireError>(crashed), "fault injected between steps 4 and 5");
  expect(index_of(root).models.empty(), "the version directory is not named by the index: never loaded");
  const auto adopted = secure_frontend::adopt_orphan(root, manifest, loaded, env);
  expect(adopted.has_value() && std::holds_alternative<secure_frontend::PublishResult>(*adopted) &&
             std::get<secure_frontend::PublishResult>(*adopted).adopted,
         "a later run adopts the orphan by digest (dl04_orphan_adopted_after_rehash_by_digest covers the re-hash)");
  expect(index_of(root).models.size() == 1, "adopted into the index");
}
#endif

void dl04_orphan_adopted_after_rehash_by_digest() {
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  const auto payload = fx::qwen_payload();
  const auto manifest = fx::trusted(fx::manifest_text("m", "t", 1, payload), secure_frontend::ModelOrigin::third_party);
  const std::string dir = "m/t/" + secure_frontend::version_directory_name(1, manifest.parsed.sha256);
  for (const auto& f : payload) {
    const auto file = secure_frontend::must_rel(dir + "/" + f.path);
    (void)secure_frontend::ensure_directory(root, secure_frontend::must_rel(file.generic().substr(0, file.generic().rfind('/'))));
    (void)secure_frontend::write_new_file(root, file, f.bytes);
  }
  (void)secure_frontend::write_new_file(root, secure_frontend::must_rel(dir + "/manifest.json"), manifest.parsed.bytes);
  secure_frontend_test::write_file(root.join(secure_frontend::must_rel(dir + "/pack/dense.bin")), "corrupt!");
  fx::FakeLoaded loaded;
  const secure_frontend::StoreEnvironment env;
  const auto corrupt = secure_frontend::adopt_orphan(root, manifest, loaded, env);
  // Final review I2: a corrupt orphan is "not adoptable" (publish then reports target exists),
  // never a verb failure with digest_mismatch.
  expect(!corrupt.has_value(), "an orphan is re-hashed before adoption; a corrupt one is not adoptable");
  expect(index_of(root).models.empty(), "and nothing is indexed");
  secure_frontend_test::write_file(root.join(secure_frontend::must_rel(dir + "/pack/dense.bin")), payload[3].bytes);
  const auto good = secure_frontend::adopt_orphan(root, manifest, loaded, env);
  expect(good.has_value() && std::holds_alternative<secure_frontend::PublishResult>(*good), "an intact orphan is adopted");
  expect(!secure_frontend::adopt_orphan(root, fx::trusted(fx::manifest_text("m", "t", 2, payload), secure_frontend::ModelOrigin::third_party),
                                        loaded, env).has_value(),
         "no orphan with another digest");
}

void replace_loaded_model_retires_old_version() {
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  const secure_frontend::StoreEnvironment env;
  fx::FakeLoaded loaded;
  const auto v1 = fx::trusted(fx::manifest_text("m", "t", 1, fx::qwen_payload("1")), secure_frontend::ModelOrigin::third_party);
  fx::stage(root, v1, fx::qwen_payload("1"));
  const auto first = std::get<secure_frontend::PublishResult>(
      secure_frontend::publish_staged(root, secure_frontend::staging_key(v1.parsed.sha256), v1, loaded, env, true));
  loaded.held.insert("m/t/" + first.entry.dir);
  const auto v2 = fx::trusted(fx::manifest_text("m", "t", 2, fx::qwen_payload("2")), secure_frontend::ModelOrigin::third_party);
  fx::stage(root, v2, fx::qwen_payload("2"));
  const auto second = std::get<secure_frontend::PublishResult>(
      secure_frontend::publish_staged(root, secure_frontend::staging_key(v2.parsed.sha256), v2, loaded, env, true));
  auto index = index_of(root);
  expect(index.models.front().sequence == 2 && index.retired.size() == 1 && second.still_retired.size() == 1,
         "the held version is retired, not deleted");
  expect(secure_frontend::exists_within(root, secure_frontend::must_rel("m/t/" + first.entry.dir)), "still on disk");
  loaded.held.clear();
  expect(!secure_frontend::delete_unheld_retired(root, loaded, env), "deleted once unheld");
  index = index_of(root);
  expect(index.retired.empty() && !secure_frontend::exists_within(root, secure_frontend::must_rel("m/t/" + first.entry.dir)), "gone");
}

void layout_check_requires_pack_files_and_valid_tokenizer() {
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  auto payload = fx::qwen_payload();
  // F-41: two vocabulary entries against three token types; SP1's load_local_tokenizer_pack rejects the pack.
  payload[6].bytes = "{\"a\":0,\"b\":2}";
  const auto manifest = fx::trusted(fx::manifest_text("m", "t", 1, payload), secure_frontend::ModelOrigin::third_party);
  fx::stage(root, manifest, payload);
  fx::FakeLoaded loaded;
  const secure_frontend::StoreEnvironment env;
  const auto result = secure_frontend::publish_staged(root, secure_frontend::staging_key(manifest.parsed.sha256), manifest, loaded, env, true);
  expect(std::holds_alternative<secure_frontend::AcquireError>(result) &&
             std::get<secure_frontend::AcquireError>(result).reason == AcquireReason::pack_layout_invalid,
         "pack_layout_invalid (exit 4)");
  auto missing = fx::qwen_payload();
  missing.erase(missing.begin() + 3);  // no pack/dense.bin
  const auto incomplete = fx::trusted(fx::manifest_text("n", "t", 1, missing), secure_frontend::ModelOrigin::third_party);
  fx::stage(root, incomplete, missing);
  const auto refused = secure_frontend::publish_staged(root, secure_frontend::staging_key(incomplete.parsed.sha256), incomplete, loaded, env, true);
  expect(std::get<secure_frontend::AcquireError>(refused).reason == AcquireReason::pack_layout_invalid, "required pack files");
}

// Gemma 4 plan 1 Task 4 (spec §6.2, §7): a gemma4 manifest's tokenizer_pack must hold the
// spm_bpe files (vocab.json, merges.json, token_type.json, tokenizer.json) and load in that
// mode; a byte-level pack (merges.txt) is pack_layout_invalid.
void gemma4_layout_check_requires_an_spm_tokenizer_pack() {
  using secure_frontend::FileRole;
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  const auto manifest_for = [](const std::string& name, const std::vector<fx::PayloadFile>& files) {
    std::string out = "{\"format\":\"strata-model-manifest\",\"schema_version\":1,\"name\":\"" + name +
                      "\",\"tag\":\"t\",\"version\":\"2026.10.22.1\",\"sequence\":1,\"profile\":\"gemma4\","
                      "\"engine\":{\"family\":\"gemma4\",\"min_version\":\"0.0.1\"},"
                      "\"license\":{\"id\":\"Apache-2.0\",\"url\":\"https://fixture.example/l\",\"bundle_allowed\":true},"
                      "\"hosts\":[\"fixture.example\"],\"files\":[";
    for (std::size_t i = 0; i < files.size(); ++i) {
      const auto& f = files[i];
      out += (i ? "," : "") + std::string{"{\"path\":\""} + f.path + "\",\"role\":\"" +
             std::string{secure_frontend::role_name(f.role)} + "\",\"size\":" + std::to_string(f.bytes.size()) +
             ",\"sha256\":\"" + secure_frontend::to_hex(secure_frontend::sha256(f.bytes)) + "\",\"sources\":[]}";
    }
    return out + "],\"launch\":{\"native\":\"weights/g.gguf\",\"tokenizer_pack\":\"tokenizer\"}}\n";
  };
  const std::vector<fx::PayloadFile> spm{{"weights/g.gguf", FileRole::weights, "gguf"},
                                         {"tokenizer/vocab.json", FileRole::tokenizer, "{\"a\":0,\"b\":1,\"ab\":2}"},
                                         {"tokenizer/merges.json", FileRole::tokenizer, "[[\"a\",\"b\"]]"},
                                         {"tokenizer/token_type.json", FileRole::tokenizer, "[1,1,1]"},
                                         {"tokenizer/tokenizer.json", FileRole::tokenizer, "{}\n"}};
  fx::FakeLoaded loaded;
  const secure_frontend::StoreEnvironment env;
  const auto try_publish = [&](const std::string& name, const std::vector<fx::PayloadFile>& files) {
    const auto manifest = fx::trusted(manifest_for(name, files), secure_frontend::ModelOrigin::third_party);
    fx::stage(root, manifest, files);
    return secure_frontend::publish_staged(root, secure_frontend::staging_key(manifest.parsed.sha256), manifest, loaded, env, true);
  };
  const auto good = try_publish("g", spm);
  expect(std::holds_alternative<secure_frontend::PublishResult>(good), "an spm_bpe tokenizer pack publishes");
  auto byte_level = spm;
  byte_level[2] = {"tokenizer/merges.txt", FileRole::tokenizer, "a b\n"};
  const auto refused = try_publish("h", byte_level);
  expect(std::holds_alternative<secure_frontend::AcquireError>(refused) &&
             std::get<secure_frontend::AcquireError>(refused).reason == AcquireReason::pack_layout_invalid &&
             std::get<secure_frontend::AcquireError>(refused).detail == "tokenizer/merges.json",
         "a byte-level (merges.txt) pack on gemma4 is pack_layout_invalid, naming merges.json");
  auto both = spm;
  both.push_back({"tokenizer/merges.txt", FileRole::tokenizer, "a b\n"});
  const auto mixed = try_publish("i", both);
  expect(std::holds_alternative<secure_frontend::AcquireError>(mixed) &&
             std::get<secure_frontend::AcquireError>(mixed).reason == AcquireReason::pack_layout_invalid,
         "an spm pack that also carries merges.txt does not load in spm_bpe mode");
  auto broken = spm;
  broken[2].bytes = "[[\"a\",\"c\"]]";
  const auto unknown = try_publish("j", broken);
  expect(std::holds_alternative<secure_frontend::AcquireError>(unknown) &&
             std::get<secure_frontend::AcquireError>(unknown).reason == AcquireReason::pack_layout_invalid,
         "a merge naming an unknown token does not load");
}

void staged_tree_must_be_exactly_the_manifest() {
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  const auto manifest = fx::trusted(fx::manifest_text("m", "t", 1, fx::qwen_payload()), secure_frontend::ModelOrigin::third_party);
  fx::stage(root, manifest, fx::qwen_payload());
  const auto dir = secure_frontend::staging_dir_path(secure_frontend::staging_key(manifest.parsed.sha256));
  (void)secure_frontend::write_new_file(root, secure_frontend::join_rel(dir, "pack/extra.bin"), "x");
  const auto refused = secure_frontend::verify_tree(root, dir, manifest, "MANIFEST", true);
  expect(refused.has_value(), "an unlisted file in staging blocks publish");
  (void)secure_frontend::remove_file_within(root, secure_frontend::join_rel(dir, "pack/extra.bin"));
  secure_frontend_test::write_file(root.join(secure_frontend::join_rel(dir, "pack/index.txt")), "tampered");
  const auto tampered = secure_frontend::verify_tree(root, dir, manifest, "MANIFEST", true);
  expect(tampered && tampered->reason == AcquireReason::digest_mismatch, "every listed file is re-hashed when asked");
}

void install_decisions_follow_sequence_rules() {
  using secure_frontend::InstallDecisionKind;
  const auto v2 = fx::trusted(fx::manifest_text("m", "t", 2, fx::qwen_payload("2")), secure_frontend::ModelOrigin::third_party);
  secure_frontend::ModelIndex index;
  index = secure_frontend::with_installed(index, secure_frontend::IndexEntry{"m", "t", "2026.10.14.2", 2,
                                                                             secure_frontend::version_directory_name(2, v2.parsed.sha256),
                                                                             v2.parsed.sha256, secure_frontend::ModelOrigin::third_party});
  expect(std::get<secure_frontend::InstallDecision>(secure_frontend::decide_install(index, v2, false)).kind ==
             InstallDecisionKind::already_installed, "equal digest: already installed");
  const auto v1 = fx::trusted(fx::manifest_text("m", "t", 1, fx::qwen_payload("1")), secure_frontend::ModelOrigin::third_party);
  expect(std::get<secure_frontend::AcquireError>(secure_frontend::decide_install(index, v1, false)).reason == AcquireReason::manifest_downgrade,
         "lower sequence refused");
  expect(std::get<secure_frontend::InstallDecision>(secure_frontend::decide_install(index, v1, true)).kind == InstallDecisionKind::update,
         "--allow-downgrade");
  const auto other2 = fx::trusted(fx::manifest_text("m", "t", 2, fx::qwen_payload("x")), secure_frontend::ModelOrigin::third_party);
  expect(std::get<secure_frontend::AcquireError>(secure_frontend::decide_install(index, other2, true)).reason == AcquireReason::manifest_conflict,
         "equal sequence, different digest: conflict even with --allow-downgrade");
  const auto v3 = fx::trusted(fx::manifest_text("m", "t", 3, fx::qwen_payload("3")), secure_frontend::ModelOrigin::third_party);
  expect(std::get<secure_frontend::InstallDecision>(secure_frontend::decide_install(index, v3, false)).kind == InstallDecisionKind::update, "update");
  const auto fresh = fx::trusted(fx::manifest_text("n", "t", 1, fx::qwen_payload()), secure_frontend::ModelOrigin::third_party);
  expect(std::get<secure_frontend::InstallDecision>(secure_frontend::decide_install(index, fresh, false)).kind == InstallDecisionKind::fresh, "fresh");
}

// F-39 (spec §4.4 item 2): MANIFEST must hash to the staging key it sits under.
void publish_requires_staging_key_of_manifest() {
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  const auto manifest = fx::trusted(fx::manifest_text("m", "t", 1, fx::qwen_payload()), secure_frontend::ModelOrigin::third_party);
  fx::stage(root, manifest, fx::qwen_payload());
  const secure_frontend::StoreEnvironment env;
  const std::string other_key(32, 'a');
  expect(!secure_frontend::rename_within(root, secure_frontend::staging_dir_path(secure_frontend::staging_key(manifest.parsed.sha256)),
                                         secure_frontend::staging_dir_path(other_key), env),
         "a complete staging tree under another key");
  fx::FakeLoaded loaded;
  const auto result = secure_frontend::publish_staged(root, other_key, manifest, loaded, env, true);
  expect(std::holds_alternative<secure_frontend::AcquireError>(result) &&
             std::get<secure_frontend::AcquireError>(result).reason == AcquireReason::publish_failed,
         "publish refuses a key that is not the manifest's");
  expect(index_of(root).models.empty() && !secure_frontend::exists_within(root, secure_frontend::must_rel("m")), "nothing published");
  expect(secure_frontend::exists_within(root, secure_frontend::join_rel(secure_frontend::staging_dir_path(other_key), "MANIFEST")),
         "staging left as it was");
}

// Task 16 carry: an over-long tree in staging is a publish failure in pull context, not bundle_invalid.
void overlong_staging_tree_is_publish_failed() {
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  const auto manifest = fx::trusted(fx::manifest_text("m", "t", 1, fx::qwen_payload()), secure_frontend::ModelOrigin::third_party);
  fx::stage(root, manifest, fx::qwen_payload());
  const auto dir = secure_frontend::staging_dir_path(secure_frontend::staging_key(manifest.parsed.sha256));
  const std::string level(200, 'a');
  const auto deep = secure_frontend::join_rel(dir, level + "/" + level + "/" + level + "/" + level);  // 845 bytes
  expect(!secure_frontend::ensure_directory(root, deep), "deep chain inside staging");
  secure_frontend_test::make_private_directory(root.join(deep) / level);  // 1046 bytes joined
  const auto refused = secure_frontend::verify_tree(root, dir, manifest, "MANIFEST", false);
  expect(refused && refused->reason == AcquireReason::publish_failed, "over-long staging tree: publish_failed");
}

// Tasks 16/17 carry: a version directory made read-only (0500 / FRFX DACL) is still removed on retire.
void retire_removes_read_only_published_version() {
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  const secure_frontend::StoreEnvironment env;
  fx::FakeLoaded loaded;
  const auto v1 = fx::trusted(fx::manifest_text("m", "t", 1, fx::qwen_payload("1")), secure_frontend::ModelOrigin::third_party);
  const auto first = publish(root, v1, fx::qwen_payload("1"), loaded);
  const std::string v1_dir = "m/t/" + first.dir;
  for (const char* sub : {"/pack/tokenizer", "/pack", "/weights", ""}) {
    expect(!secure_frontend::make_read_only_directory(root, secure_frontend::must_rel(v1_dir + sub)), "read-only directory");
  }
  const auto v2 = fx::trusted(fx::manifest_text("m", "t", 2, fx::qwen_payload("2")), secure_frontend::ModelOrigin::third_party);
  fx::stage(root, v2, fx::qwen_payload("2"));
  const auto second = secure_frontend::publish_staged(root, secure_frontend::staging_key(v2.parsed.sha256), v2, loaded, env, true);
  expect(std::holds_alternative<secure_frontend::PublishResult>(second) &&
             std::get<secure_frontend::PublishResult>(second).still_retired.empty(),
         "nothing left retired");
  const auto index = index_of(root);
  expect(index.models.size() == 1 && index.models.front().sequence == 2 && index.retired.empty(), "index names v2 only");
  expect(!secure_frontend::exists_within(root, secure_frontend::must_rel(v1_dir)), "the read-only version is gone from disk");
  expect(std::holds_alternative<secure_frontend::InstalledModel>(secure_frontend::load_installed(root, index.models.front())),
         "the live version is intact");
}

// Task 9 carry: a downgrade adopts a retired version again; one live directory per name:tag,
// and retire never deletes the live directory.
void downgrade_adopts_retired_version_keeping_one_live() {
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  const secure_frontend::StoreEnvironment env;
  fx::FakeLoaded loaded;
  const auto v1 = fx::trusted(fx::manifest_text("m", "t", 1, fx::qwen_payload("1")), secure_frontend::ModelOrigin::third_party);
  const auto first = publish(root, v1, fx::qwen_payload("1"), loaded);
  loaded.held.insert("m/t/" + first.dir);
  const auto v2 = fx::trusted(fx::manifest_text("m", "t", 2, fx::qwen_payload("2")), secure_frontend::ModelOrigin::third_party);
  const auto second = publish(root, v2, fx::qwen_payload("2"), loaded);
  loaded.held.insert("m/t/" + second.dir);
  expect(index_of(root).retired.size() == 1, "v1 retired while held");
  expect(std::get<secure_frontend::InstallDecision>(secure_frontend::decide_install(index_of(root), v1, true)).kind ==
             secure_frontend::InstallDecisionKind::update,
         "--allow-downgrade");
  const auto adopted = secure_frontend::adopt_orphan(root, v1, loaded, env);
  expect(adopted.has_value() && std::holds_alternative<secure_frontend::PublishResult>(*adopted) &&
             std::get<secure_frontend::PublishResult>(*adopted).adopted,
         "the retired v1 directory is adopted again");
  auto index = index_of(root);
  expect(index.models.size() == 1 && index.models.front().dir == first.dir, "v1 live");
  expect(index.retired.size() == 1 && index.retired.front().dir == second.dir, "v2 retired (held); v1 not listed as retired");
  loaded.held.clear();
  expect(!secure_frontend::delete_unheld_retired(root, loaded, env), "retire runs");
  index = index_of(root);
  expect(index.retired.empty() && !secure_frontend::exists_within(root, secure_frontend::must_rel("m/t/" + second.dir)), "v2 deleted");
  expect(secure_frontend::exists_within(root, secure_frontend::must_rel("m/t/" + first.dir)), "v1 (live) kept");
  // A hand-edited index naming the live directory as retired too: retire drops the row, never the directory.
  index.retired.push_back(secure_frontend::RetiredEntry{"m", "t", first.dir, first.manifest_sha256});
  expect(!secure_frontend::write_model_index(root, index), "index with a live dir listed as retired");
  expect(!secure_frontend::delete_unheld_retired(root, loaded, env), "retire runs");
  index = index_of(root);
  expect(index.retired.empty() && index.models.size() == 1, "the stray retired row is dropped");
  expect(std::holds_alternative<secure_frontend::InstalledModel>(secure_frontend::load_installed(root, index.models.front())),
         "the live directory is untouched");
}

// §4.8 rm: refuses a held version unless --force; removes a read-only tree; unknown afterwards.
void rm_refuses_held_version_and_removes_read_only_tree() {
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  const secure_frontend::StoreEnvironment env;
  fx::FakeLoaded loaded;
  const auto v1 = fx::trusted(fx::manifest_text("m", "t", 1, fx::qwen_payload("1")), secure_frontend::ModelOrigin::third_party);
  const auto first = publish(root, v1, fx::qwen_payload("1"), loaded);
  const std::string dir = "m/t/" + first.dir;
  loaded.held.insert(dir);
  const auto busy = secure_frontend::remove_installed(root, "m", "t", loaded, false, env);
  expect(std::holds_alternative<secure_frontend::AcquireError>(busy) &&
             std::get<secure_frontend::AcquireError>(busy).reason == AcquireReason::models_busy,
         "held: models_busy");
  expect(index_of(root).models.size() == 1 && secure_frontend::exists_within(root, secure_frontend::must_rel(dir)), "untouched");
  expect(!secure_frontend::make_read_only_directory(root, secure_frontend::must_rel(dir + "/pack")), "read-only pack");
  expect(!secure_frontend::make_read_only_directory(root, secure_frontend::must_rel(dir)), "read-only version");
  const auto forced = secure_frontend::remove_installed(root, "m", "t", loaded, true, env);
  expect(std::holds_alternative<secure_frontend::IndexEntry>(forced) && std::get<secure_frontend::IndexEntry>(forced).dir == first.dir,
         "--force removes");
  expect(index_of(root).models.empty() && !secure_frontend::exists_within(root, secure_frontend::must_rel(dir)), "gone");
  const auto unknown = secure_frontend::remove_installed(root, "m", "t", loaded, false, env);
  expect(std::holds_alternative<secure_frontend::AcquireError>(unknown) &&
             std::get<secure_frontend::AcquireError>(unknown).reason == AcquireReason::model_unknown,
         "model_unknown afterwards");
}

#if !defined(_WIN32)
// Task 16 carry: a version directory that cannot be reached (its parent is now a symlink)
// is a failure, never reported as removed; the link target is never touched.
void rm_reports_unreachable_version_as_failure() {
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  const secure_frontend::StoreEnvironment env;
  fx::FakeLoaded loaded;
  const auto v1 = fx::trusted(fx::manifest_text("m", "t", 1, fx::qwen_payload("1")), secure_frontend::ModelOrigin::third_party);
  const auto first = publish(root, v1, fx::qwen_payload("1"), loaded);
  std::filesystem::rename(path / "m" / "t", path / "m" / "elsewhere");
  std::filesystem::create_directory_symlink("elsewhere", path / "m" / "t");
  const auto removed = secure_frontend::remove_installed(root, "m", "t", loaded, false, env);
  expect(std::holds_alternative<secure_frontend::AcquireError>(removed) &&
             std::get<secure_frontend::AcquireError>(removed).reason == AcquireReason::publish_failed,
         "an unreachable version directory is reported as a failure");
  std::error_code ignored;
  expect(std::filesystem::exists(path / "m" / "elsewhere" / first.dir / "manifest.json", ignored), "the link target is untouched");
}

// Final review I2 / Controller note 5: rm moves the entry to retired before deleting, so a
// delete that fails part-way leaves a retired version that rm --retired removes later.
void rm_failed_delete_stays_retired() {
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  const secure_frontend::StoreEnvironment env;
  fx::FakeLoaded loaded;
  const auto v1 = fx::trusted(fx::manifest_text("m", "t", 1, fx::qwen_payload("1")), secure_frontend::ModelOrigin::third_party);
  const auto first = publish(root, v1, fx::qwen_payload("1"), loaded);
  std::filesystem::rename(path / "m" / "t", path / "m" / "elsewhere");
  std::filesystem::create_directory_symlink("elsewhere", path / "m" / "t");
  const auto removed = secure_frontend::remove_installed(root, "m", "t", loaded, false, env);
  expect(std::holds_alternative<secure_frontend::AcquireError>(removed), "the delete fails");
  const auto after = index_of(root);
  expect(after.models.empty() && after.retired.size() == 1 && after.retired.front().dir == first.dir &&
             after.retired.front().manifest_sha256 == first.manifest_sha256,
         "the version is not live and stays retired (never an unmanaged orphan)");
  std::filesystem::remove(path / "m" / "t");
  std::filesystem::rename(path / "m" / "elsewhere", path / "m" / "t");
  expect(!secure_frontend::delete_unheld_retired(root, loaded, env), "rm --retired");
  expect(index_of(root).retired.empty() && !secure_frontend::exists_within(root, secure_frontend::must_rel("m/t/" + first.dir)),
         "a later rm --retired removes it");
}
#endif

#if defined(_WIN32)
// Final review I2 on Windows: the engine holds a file open; rm leaves the version retired.
void rm_open_version_stays_retired_windows() {
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  const secure_frontend::StoreEnvironment env;
  secure_frontend::NoServerDiscovery nobody;  // production answer today (Controller note 5)
  const auto v1 = fx::trusted(fx::manifest_text("m", "t", 1, fx::qwen_payload("1")), secure_frontend::ModelOrigin::third_party);
  const auto first = publish(root, v1, fx::qwen_payload("1"), nobody);
  const auto shard = root.join(secure_frontend::must_rel("m/t/" + first.dir + "/weights/fx-00001-of-00002.gguf"));
  HANDLE held = CreateFileW(shard.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  const auto removed = secure_frontend::remove_installed(root, "m", "t", nobody, false, env);
  expect(std::holds_alternative<secure_frontend::AcquireError>(removed), "the delete fails on the open file");
  expect(index_of(root).models.empty() && index_of(root).retired.size() == 1, "the version stays retired");
  CloseHandle(held);
  expect(!secure_frontend::delete_unheld_retired(root, nobody, env) && index_of(root).retired.empty() &&
             !secure_frontend::exists_within(root, secure_frontend::must_rel("m/t/" + first.dir)),
         "removed once released");
}

void replace_loaded_model_retires_old_version_windows() {
  const auto path = secure_frontend_test::make_scratch_directory();
  const auto root = fx::models_root(path);
  const secure_frontend::StoreEnvironment env;
  secure_frontend::NoServerDiscovery nobody;  // production answer today (Controller note 5)
  const auto v1 = fx::trusted(fx::manifest_text("m", "t", 1, fx::qwen_payload("1")), secure_frontend::ModelOrigin::third_party);
  fx::stage(root, v1, fx::qwen_payload("1"));
  const auto first = std::get<secure_frontend::PublishResult>(
      secure_frontend::publish_staged(root, secure_frontend::staging_key(v1.parsed.sha256), v1, nobody, env, true));
  // An engine holds shard 1 open without FILE_SHARE_DELETE.
  const auto shard = root.join(secure_frontend::must_rel("m/t/" + first.entry.dir + "/weights/fx-00001-of-00002.gguf"));
  HANDLE held = CreateFileW(shard.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  const auto v2 = fx::trusted(fx::manifest_text("m", "t", 2, fx::qwen_payload("2")), secure_frontend::ModelOrigin::third_party);
  fx::stage(root, v2, fx::qwen_payload("2"));
  const auto second = secure_frontend::publish_staged(root, secure_frontend::staging_key(v2.parsed.sha256), v2, nobody, env, true);
  expect(std::holds_alternative<secure_frontend::PublishResult>(second), "the new version publishes");
  expect(index_of(root).retired.size() == 1, "the open version stays retired (deletion failed after retries)");
  CloseHandle(held);
  expect(!secure_frontend::delete_unheld_retired(root, nobody, env) && index_of(root).retired.empty(), "removed once released");
}
#endif

}  // namespace acquire_cases
