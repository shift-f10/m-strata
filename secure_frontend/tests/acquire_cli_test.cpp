// SP4 Task 23 (spec §1.5, §4.5, §4.8, §6.2): the strata CLI's local verbs.
#include "acquire_cases.hpp"
#include "secure_frontend/catalog.hpp"  // SP5 M1 Task 14: the one version source
#include "secure_frontend/first_start.hpp"  // GUI-first start-up: a first start's config
#include "secure_frontend/model_index_store.hpp"
#include "secure_frontend/test_hooks.hpp"
#include "secure_frontend/token_reader.hpp"  // kDpapiTokenMagic: the Windows store's header
#include "support/cli_harness.hpp"
#include "support/config_json.hpp"  // SP5 M1 Task 14: the key-case config rewrite
#include "support/expect.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace acquire_cases {
namespace {

using secure_frontend_test::expect;
using secure_frontend_test::json_literal;
using secure_frontend_test::cli::CliHarness;

class HeldLoaded final : public secure_frontend::LoadedVersions {
 public:
  explicit HeldLoaded(std::string held) : held_(std::move(held)) {}
  bool is_held(const std::string_view name, const std::string_view tag, const std::string_view dir) override {
    return held_ == std::string{name} + "/" + std::string{tag} + "/" + std::string{dir};
  }

 private:
  std::string held_;
};

// Publishes m:t v2 while v1 is held, so v1 stays in the index's retired list.
void publish_v2_while_v1_held(CliHarness& h, const secure_frontend::IndexEntry& v1) {
  namespace fx = secure_frontend_test::acquire;
  const auto root = h.models();
  const auto manifest = fx::trusted(fx::manifest_text("m", "t", 2, fx::qwen_payload("2")), secure_frontend::ModelOrigin::third_party);
  fx::stage(root, manifest, fx::qwen_payload("2"));
  HeldLoaded held("m/t/" + v1.dir);
  const secure_frontend::StoreEnvironment env;
  (void)secure_frontend::publish_staged(root, secure_frontend::staging_key(manifest.parsed.sha256), manifest, held, env, true);
}

std::string replaced(std::string text, const std::string& from, const std::string& to) {
  text.replace(text.find(from), from.size(), to);
  return text;
}

}  // namespace

void program_update_exits_12_without_network() {
  CliHarness h;
  bool network_seam_used = false;
  h.services.transport = [&](const secure_frontend::FrontendConfig&) {
    network_seam_used = true;
    return std::unique_ptr<secure_frontend::HopTransport>{};
  };
  h.services.probe = [&](const secure_frontend::FrontendConfig&) {
    network_seam_used = true;
    return std::unique_ptr<secure_frontend::DownloaderProbe>{};
  };
  h.services.resolver = [&]() {
    network_seam_used = true;
    return std::unique_ptr<secure_frontend::Resolver>{};
  };
  // SP7 M2 Task 3 (ruling R-T3-j): the exit-12 placeholder is gone. A bare
  // `strata update` now takes the standard "--config is required" usage path,
  // and `strata --config <f> update` is the program verb refusing on the
  // unconfigured update.source — still exit 12, still with no network.
  expect(h.run_raw({"update"}) == 1 && h.err.find("--config") != std::string::npos, "bare update now follows every verb's config rule");
  const int code = h.run({"update"});
  expect(code == 12, "update with no update.source exits 12, got " + std::to_string(code));
  expect(h.out.find("strata update: update.source is not configured") != std::string::npos, "the configured-key refusal");
  expect(!network_seam_used, "no transport, downloader probe or resolver is created before the config refusals");
  expect(h.run({"update", "--models"}) != 12, "update --models is the models part, not the program update");
}

void cli_requires_config_and_known_verb() {
  CliHarness h;
  expect(h.run_raw({"list"}) == 1 && h.err.find("--config") != std::string::npos, "config is required (no default location yet)");
  expect(h.run_raw({"--config"}) == 1, "--config without a file and verb: exit 1");
  expect(h.run_raw({"--config", (h.scratch / "absent.json").string(), "list"}) == 1, "unreadable config: exit 1");
  expect(h.run({"frobnicate"}) == 1, "unknown verb: usage, exit 1");
  expect(h.run({"list", "--bogus"}) == 1, "unknown option: exit 1");
  expect(h.run({"show"}) == 1 && h.run({"rm", "--partials"}) == 1, "missing operand or option value: exit 1");
  expect(h.run({"verify", "b", "--trust-manifest", "sha256:XYZ"}) == 1, "malformed --trust-manifest: exit 1");
  expect(h.run_raw({"--help"}) == 0 && h.out.find("strata --config <file> <verb>") != std::string::npos, "--help prints usage, exit 0");
  if (secure_frontend::kTestHooksEnabled) expect(h.err.find("SECURE_FRONTEND_TEST_HOOKS_COMPILED_IN") != std::string::npos, "hooks banner");
}

// GUI-first start-up (spec 2026-10-27 §4.3): with a models.cards config, `strata --config <f> --store <folder> <verb>`
// uses that folder as the store instead of models.downloads_root (a download the page sent to another drive).
void store_option_names_another_store() {
  CliHarness h;
  const auto data = secure_frontend::data_folder_layout(h.scratch / "data");
  expect(std::holds_alternative<secure_frontend::FirstStartResult>(secure_frontend::ensure_data_folder(data, h.scratch / "bin" / "engine")),
         "a first start's config (models.cards, no card yet)");
  const auto other = h.scratch / "other";
  secure_frontend_test::make_private_directory(other);
  h.models_path = other;
  (void)secure_frontend_test::cli::install(h);   // m:t, into the other folder
  const std::string config = secure_frontend::path_to_utf8(data.config_file);
  const std::string folder = secure_frontend::path_to_utf8(other);
  expect(h.run_raw({"--config", config, "list"}) == 0 && h.out.find("m:t") == std::string::npos, "the downloads root holds nothing");
  expect(h.run_raw({"--config", config, "--store", folder, "list"}) == 0 && h.out.find("m:t") != std::string::npos,
         "--store lists the other folder: " + h.out + h.err);
  expect(h.run_raw({"--config", config, "--store", "relative", "list"}) == 1, "--store takes an absolute folder");
  expect(h.run_raw({"--config", config, "--store", folder}) == 1, "--store needs a verb after its folder");
  expect(h.run_raw({"--config", config, "--store", secure_frontend::path_to_utf8(h.scratch / "absent"), "list"}) == 1,
         "a folder that is not there is refused");
  expect(h.run({"--store", folder, "list"}) == 1 && h.err.find("models.cards") != std::string::npos,
         "only a models.cards config takes --store: " + h.err);
}

void list_and_show_report_installed_models() {
  CliHarness h;
  const auto entry = secure_frontend_test::cli::install(h);
  expect(h.run({"list"}) == 0, "list");
  expect(h.out.find("m:t") != std::string::npos && h.out.find("third_party") != std::string::npos && h.out.find("runnable") != std::string::npos,
         "name:tag, origin, state");
  expect(h.run({"show", "m:t"}) == 0 && h.out.find("--native m/t/" + entry.dir + "/weights/fx-00001-of-00002.gguf") != std::string::npos,
         "show prints the engine arguments the manifest implies, relative to roots.models");
  expect(h.out.find(h.models_path.string()) == std::string::npos, "never an absolute path outside the data dir");
  expect(h.run({"show", "m:t", "--digests"}) == 0 && h.out.find("  weights/fx-00001-of-00002.gguf") != std::string::npos,
         "--digests prints sha256sum lines");
  expect(h.run({"show", "nope:t"}) == 5, "unknown model: exit 5");
  expect(h.run({"show", "m"}) == 0, "a single installed tag is found by name alone");
  // A second model whose manifest needs a newer engine than this release (§2.3, §4.5 list state).
  {
    namespace fx = secure_frontend_test::acquire;
    const auto root = h.models();
    const auto manifest = fx::trusted(replaced(fx::manifest_text("m", "u", 1, fx::qwen_payload("u")), "\"min_version\":\"0.1.43\"",
                                               "\"min_version\":\"9.0.0\""),
                                      secure_frontend::ModelOrigin::third_party);
    fx::stage(root, manifest, fx::qwen_payload("u"));
    fx::FakeLoaded nobody;
    const secure_frontend::StoreEnvironment env;
    (void)secure_frontend::publish_staged(root, secure_frontend::staging_key(manifest.parsed.sha256), manifest, nobody, env, true);
  }
  expect(h.run({"list"}) == 0 && h.out.find("needs engine strata >= 9.0.0") != std::string::npos, "list: needs engine <family> >= <version>");
  // A glm53 / family glm model (SP6): the state names its family (F-58: the gate honours family).
  {
    namespace fx = secure_frontend_test::acquire;
    const std::string text =
        "{\"format\":\"strata-model-manifest\",\"schema_version\":1,\"name\":\"g\",\"tag\":\"t\",\"version\":\"2026.10.14.1\","
        "\"sequence\":1,\"profile\":\"glm53\",\"engine\":{\"family\":\"glm\",\"min_version\":\"0.0.1\"},"
        "\"license\":{\"id\":\"MIT\",\"url\":\"https://h.example/l\",\"bundle_allowed\":true},\"hosts\":[\"h.example\"],"
        "\"files\":[{\"path\":\"w.gguf\",\"role\":\"weights\",\"size\":1,"
        "\"sha256\":\"ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb\",\"sources\":[]}],"
        "\"launch\":{\"native\":\"w.gguf\"}}";
    const auto root = h.models();
    const auto manifest = fx::trusted(text, secure_frontend::ModelOrigin::third_party);
    fx::stage(root, manifest, {{"w.gguf", secure_frontend::FileRole::weights, "a"}});
    fx::FakeLoaded nobody;
    const secure_frontend::StoreEnvironment env;
    expect(std::holds_alternative<secure_frontend::PublishResult>(secure_frontend::publish_staged(
               root, secure_frontend::staging_key(manifest.parsed.sha256), manifest, nobody, env, true)),
           "test setup: glm53 model published");
  }
  expect(h.run({"list"}) == 0 && h.out.find("needs engine glm >= 0.0.1") != std::string::npos, "list: glm family named: " + h.out);
  expect(h.run({"show", "g:t"}) == 0 && h.out.find("needs engine glm >= 0.0.1") != std::string::npos, "show: same state");
  expect(h.run({"show", "m"}) == 1 && h.err.find("several tags installed") != std::string::npos, "an ambiguous name is a usage error");
}

void list_all_reports_retired_orphaned_and_staging() {
  CliHarness h;
  const auto v1 = secure_frontend_test::cli::install(h, 1, "1");
  publish_v2_while_v1_held(h, v1);
  (void)secure_frontend::ensure_directory(h.models(), secure_frontend::must_rel("x/t/v9-aaaaaaaaaaaa"));
  (void)secure_frontend::write_new_file(h.models(), secure_frontend::must_rel("x/t/v9-aaaaaaaaaaaa/w.gguf"), "orphan");
  (void)secure_frontend::ensure_directory(h.models(), secure_frontend::must_rel(".staging/" + std::string(32, 'b')));
  (void)secure_frontend::write_new_file(h.models(), secure_frontend::must_rel(".staging/" + std::string(32, 'b') + "/x.part"), "12345");
#if !defined(_WIN32)
  // Task 15 carry: scan_model_root_files skips links and non-portable names; list --all counts them.
  std::filesystem::create_directory_symlink(h.scratch, h.models_path / "link");
#endif
  expect(h.run({"list", "--all"}) == 0, "list --all");
  expect(h.out.find("orphaned x/t/v9-aaaaaaaaaaaa 6 bytes") != std::string::npos, "orphaned version reported with its size");
  expect(h.out.find("staging " + std::string(32, 'b') + " 5 bytes") != std::string::npos, "staging usage per key");
  expect(h.out.find("retired m/t/" + v1.dir + " ") != std::string::npos, "retired version reported with its size");
  expect(h.out.find("never loaded") != std::string::npos, "the orphan line says what it is and how to clear it");
#if !defined(_WIN32)
  expect(h.out.find("skipped 1 entry") != std::string::npos, "skipped (non-P1 / link) entries are counted, never followed");
#endif
}

void rm_refuses_loaded_model() {
  CliHarness h;
  const auto entry = secure_frontend_test::cli::install(h);
  h.services.loaded = [entry](const secure_frontend::FrontendConfig&) {
    return std::make_unique<HeldLoaded>("m/t/" + entry.dir);
  };
  expect(h.run({"rm", "m:t"}) == 7 && h.err.find("models_busy") != std::string::npos, "a loaded version is refused (exit 7)");
  expect(h.run({"rm", "m:t", "--force"}) == 0, "--force after stop");
  expect(std::get<secure_frontend::ModelIndex>(secure_frontend::read_model_index(h.models())).models.empty(), "removed from the index");
  expect(!std::filesystem::exists(h.models_path / "m" / "t" / entry.dir), "deleted bottom-up");
  expect(h.run({"rm", "m:t"}) == 5, "rm of a model no longer installed: model_unknown (exit 5)");
}

void rm_retired_deletes_unheld_versions() {
  CliHarness h;
  const auto v1 = secure_frontend_test::cli::install(h, 1, "1");
  publish_v2_while_v1_held(h, v1);
  expect(std::get<secure_frontend::ModelIndex>(secure_frontend::read_model_index(h.models())).retired.size() == 1, "retired");
  expect(h.run({"rm", "--retired"}) == 0, "rm --retired");
  expect(std::get<secure_frontend::ModelIndex>(secure_frontend::read_model_index(h.models())).retired.empty() &&
             !std::filesystem::exists(h.models_path / "m" / "t" / v1.dir),
         "unheld retired versions deleted");
}

void rm_partials_deletes_other_staging() {
  CliHarness h;
  namespace fx = secure_frontend_test::acquire;
  const auto manifest = fx::manifest_text("m", "t", 3, fx::qwen_payload("p"));
  const auto key = secure_frontend::staging_key(secure_frontend::to_hex(secure_frontend::sha256(manifest)));
  (void)secure_frontend::ensure_directory(h.models(), secure_frontend::must_rel(".staging/" + key));
  (void)secure_frontend::write_new_file(h.models(), secure_frontend::must_rel(".staging/" + key + "/MANIFEST"), manifest);
  const auto other = fx::manifest_text("n", "t", 1, fx::qwen_payload("q"));
  const auto other_key = secure_frontend::staging_key(secure_frontend::to_hex(secure_frontend::sha256(other)));
  (void)secure_frontend::ensure_directory(h.models(), secure_frontend::must_rel(".staging/" + other_key));
  (void)secure_frontend::write_new_file(h.models(), secure_frontend::must_rel(".staging/" + other_key + "/MANIFEST"), other);
  expect(h.run({"rm", "--partials", "m"}) == 1, "rm --partials needs <name:tag>");
  expect(h.run({"rm", "--partials", "m:t"}) == 0, "rm --partials");
  expect(!std::filesystem::exists(h.models_path / ".staging" / key) && std::filesystem::exists(h.models_path / ".staging" / other_key),
         "only m:t partials deleted");
}

// SP5 M1 Task 14 (D-4): `strata version` — the tag governs, so the answer comes
// from the one configure-time version source (upstream.version ->
// built_engine_version_text), and the engine's self-version is labelled
// informational without ever being spoken by this binary (Ruling T14-a).
void version_names_frontend_and_engine_versions() {
  CliHarness h;
  const std::string pin{secure_frontend::built_engine_version_text()};
  const int bare = h.run_raw({"version"});
  const std::string first = h.out;
  expect(bare == 0, "strata version runs without --config: " + h.err);
  expect(h.out.find("strata (frontend) version " + pin) != std::string::npos,
         "the frontend version names the governing pin: " + h.out);
  expect(h.out.find("upstream pin v" + pin + " (upstream.version)") != std::string::npos,
         "upstream.version is named as the pin: " + h.out);
  expect(h.out.find("the tag version governs") != std::string::npos, "the bump rule is stated: " + h.out);
  expect(h.out.find("engine self-version: informational only") != std::string::npos,
         "the engine self-version is labelled informational: " + h.out);
  expect(h.out.find("0.1.46") == std::string::npos, "the engine's self-number never comes from this binary: " + h.out);
  expect(h.run({"version"}) == 0 && h.out == first, "the --config form validates, then answers the same: " + h.out);
}

// SP5 M1 Task 14 (D-4, Rulings T14-c/d/e): key show/rotate on the SP1 store.
void key_show_requires_local_token_and_never_logs() {
  CliHarness h;
  // The harness base config carries env:STRATA_TEST_TOKEN — not a locally
  // generated store, so show refuses before reading anything.
  expect(h.run({"key", "show"}) == 1 && h.err.find("env:") != std::string::npos,
         "an env: source has no local token to show: " + h.err);
  // A file: token under roots.secret (the store key works on).
  secure_frontend_test::make_private_directory(h.scratch / "secret");
  secure_frontend_test::write_file(h.config,
                                   secure_frontend_test::render_config_json({{"schema_version", "1"},
                                                                            {"roots.models", json_literal(h.models_path.string())},
                                                                            {"roots.secret", json_literal((h.scratch / "secret").string())},
                                                                            {"token.source", json_literal("file:token")},
                                                                            {"engine.executable", json_literal((h.scratch / "bin" / "engine").string())},
                                                                            {"engine.model_id", json_literal("m")}}));
  expect(h.run({"key", "show"}) == 7 && h.out.empty(), "no token yet: show refuses, prints nothing: " + h.err);
  expect(h.run({"key", "rotate"}) == 0, "rotate creates the store: " + h.err);
  const auto first_line = h.out.find('\n');
  const std::string token1 = h.out.substr(0, first_line);
  expect(!token1.empty() && h.err.find("restart") != std::string::npos,
         "rotate prints the new token once and the restart notice: " + h.out);
  expect(std::count(h.out.begin(), h.out.end(), '\n') >= 1 && h.out.find(token1, first_line + 1) == std::string::npos,
         "the rotated token is printed exactly once");
  const auto store = h.scratch / "secret" / "token";
  expect(std::filesystem::exists(store), "the token file sits at roots.secret/token");
#if !defined(_WIN32)
  const auto bits = std::filesystem::status(store).permissions();
  expect((bits & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none,
         "the store stays 0600 (SP1 rules unchanged)");
#endif
  expect(h.run({"key", "show"}) == 0 && h.out == token1 + "\n", "show prints the stored token, once: " + h.out);
  // The store readable by THIS uid is the store-rule verdict (T14-c): a
  // group-readable file is refused without printing.
#if !defined(_WIN32)
  (void)::chmod(store.c_str(), 0644);
  const int unsafe = h.run({"key", "show"});
  expect(unsafe == 7 && h.out.find(token1) == std::string::npos,
         "an unsafe store is refused and the token is not printed (exit 7): " + h.out);
  (void)::chmod(store.c_str(), 0600);
#endif
  // --env prints the shell line (PX).
  expect(h.run({"key", "show", "--env"}) == 0 && h.out == "OPENAI_API_KEY=" + token1 + "\n",
         "--env prints the shell line: " + h.out);
  // rotate replaces atomically; the old token is gone from every file.
  expect(h.run({"key", "rotate"}) == 0, "rotate again");
  const std::string token2 = h.out.substr(0, h.out.find('\n'));
  expect(!token2.empty() && token2 != token1, "rotate generated a new token");
  int token1_files = 0;
  int token2_files = 0;
  std::error_code walk_ec;
  for (auto it = std::filesystem::recursive_directory_iterator(h.scratch, walk_ec);
       !walk_ec && it != std::filesystem::recursive_directory_iterator(); it.increment(walk_ec)) {
    if (!it->is_regular_file()) continue;
    const std::string body = secure_frontend_test::read_file(it->path());
    if (body.find(token1) != std::string::npos) ++token1_files;
    if (body.find(token2) != std::string::npos) ++token2_files;
  }
  expect(token1_files == 0, "the rotated-out token lives in no file anywhere (nothing logged)");
#if defined(_WIN32)
  // Windows stores the DPAPI blob (STRATAK1 + CryptProtectData, token_store.cpp), so
  // the plaintext lives in no file at all; show above proved the store round-trips.
  expect(token2_files == 0, "the live token's plaintext lives in no file: the store holds the DPAPI blob");
  expect(secure_frontend_test::read_file(store).rfind(std::string{secure_frontend::kDpapiTokenMagic}, 0) == 0,
         "the store is the DPAPI-protected form (STRATAK1 header)");
#else
  expect(token2_files == 1, "the live token lives in exactly one file: the store itself");
#endif
  // Grammar: bad subcommand and --env misuse are usage errors.
  expect(h.run({"key", "frobnicate"}) == 1, "key takes show or rotate");
  expect(h.run({"key", "rotate", "--env"}) == 1, "--env belongs to show only");
}

#if defined(SECURE_FRONTEND_TEST_HOOKS)
void list_outdated_uses_catalog_sequence() {
  CliHarness h;
  // An older catalog release's fixture-qwen:t1 (sequence 1); this binary's catalog has sequence 2.
  (void)secure_frontend_test::cli::install(h, 1, "", secure_frontend::ModelOrigin::catalog, "fixture-qwen", "t1");
  expect(h.run({"list", "--outdated"}) == 0, "list --outdated");
  expect(h.out.find("fixture-qwen:t1 sequence 1 -> 2") != std::string::npos, "answered offline from the embedded catalog");
}
#endif

}  // namespace acquire_cases
