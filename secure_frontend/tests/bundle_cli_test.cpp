// SP4 Task 23 (spec §5): strata import / verify / export through the CLI.
#include "bundle_cases.hpp"
#include "secure_frontend/model_index_store.hpp"
#include "support/cli_harness.hpp"
#include "support/expect.hpp"

namespace bundle_cases {
namespace {

using secure_frontend_test::expect;
using secure_frontend_test::cli::CliHarness;

// Every write fails once a file holds `limit` bytes (disk_space, exit 6).
class FailingWrites final : public secure_frontend::StoreEnvironment {
 public:
  [[nodiscard]] std::optional<std::uint64_t> fail_writes_after_bytes() const override { return 0; }
};

namespace fx = secure_frontend_test::acquire;

// Publishes `text` (payload `salt`) into h's roots.models; the entry.
secure_frontend::IndexEntry install_text(CliHarness& h, const std::string& text, const std::string_view salt) {
  const auto root = h.models();
  const auto manifest = fx::trusted(text, secure_frontend::ModelOrigin::third_party);
  fx::stage(root, manifest, fx::qwen_payload(salt));
  fx::FakeLoaded nobody;
  const secure_frontend::StoreEnvironment env;
  return std::get<secure_frontend::PublishResult>(
             secure_frontend::publish_staged(root, secure_frontend::staging_key(manifest.parsed.sha256), manifest, nobody, env, true))
      .entry;
}

std::uint32_t live_sequence(const CliHarness& h) {
  return std::get<secure_frontend::ModelIndex>(secure_frontend::read_model_index(h.models())).models.front().sequence;
}

}  // namespace

// Final review C1 (§6.1): import obeys the same --yes limits as pull. A license change, or an
// installed version that cannot be read, needs an interactive y; --yes alone declines (exit 2).
void import_license_change_needs_interactive_yes() {
  std::string v2 = fx::manifest_text("m", "t", 2, fx::qwen_payload("2"));
  v2.replace(v2.find("\"Apache-2.0\""), 12, "\"MIT\"");
  CliHarness online;
  const auto entry = install_text(online, v2, "2");
  const auto bundle = online.scratch / "b";
  expect(online.run({"export", "m:t", bundle.string()}) == 0, "export v2");
  const std::string trust = "sha256:" + entry.manifest_sha256;
  CliHarness offline;
  (void)secure_frontend_test::cli::install(offline, 1, "1");
  expect(offline.run({"import", bundle.string(), "--trust-manifest", trust, "--yes"}) == 2, "--yes, non-interactive: declined (exit 2)");
  expect(live_sequence(offline) == 1, "v1 stays live");
  expect(offline.out.find("license: Apache-2.0 -> MIT") != std::string::npos, "the manifest diff is printed: " + offline.out);
  expect(offline.out.find("The license of m:t changes") != std::string::npos, "the license question is asked");
  expect(offline.run({"import", bundle.string(), "--trust-manifest", trust, "--yes"}, "n\n", true) == 2 && live_sequence(offline) == 1,
         "an interactive n declines");
  expect(offline.run({"import", bundle.string(), "--trust-manifest", trust}, "y\ny\n", true) == 0 && live_sequence(offline) == 2,
         "an interactive y (license) and y (import) accept it: " + offline.out + offline.err);
  expect(offline.run({"import", bundle.string(), "--trust-manifest", trust}) == 0 && offline.out.find("already installed") != std::string::npos &&
             offline.out.find("[y/N]") == std::string::npos,
         "already installed, without --yes: exit 0 before any question: " + offline.out);
}

void import_with_unreadable_installed_version_needs_interactive_yes() {
  CliHarness online;
  const auto entry = secure_frontend_test::cli::install(online, 2, "2");
  const auto bundle = online.scratch / "b";
  expect(online.run({"export", "m:t", bundle.string()}) == 0, "export v2");
  const std::string trust = "sha256:" + entry.manifest_sha256;
  CliHarness offline;
  const auto v1 = secure_frontend_test::cli::install(offline, 1, "1");
  const auto rotten = offline.models_path / "m" / "t" / v1.dir / "manifest.json";
  std::filesystem::permissions(rotten, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
  secure_frontend_test::write_file(rotten, "rotten");
  expect(offline.run({"import", bundle.string(), "--trust-manifest", trust, "--yes"}) == 2 && live_sequence(offline) == 1,
         "--yes does not answer an unknown installed license: exit 2, v1 stays live");
  expect(offline.out.find("installed version of m:t cannot be read") != std::string::npos, "the reason is printed: " + offline.out);
  expect(offline.run({"import", bundle.string(), "--trust-manifest", trust, "--yes"}, "y\n", true) == 0 && live_sequence(offline) == 2,
         "an interactive y proceeds");
}

// Final review I2: an orphan with this manifest's digest whose files do not verify is not
// adoptable: import reports "target exists" and the orphan hint, never digest_mismatch.
void import_over_corrupt_orphan_reports_target_exists() {
  CliHarness online;
  const auto entry = secure_frontend_test::cli::install(online);
  const auto bundle = online.scratch / "b";
  expect(online.run({"export", "m:t", bundle.string()}) == 0, "export");
  CliHarness offline;
  const auto root = offline.models();
  const std::string dir = "m/t/" + entry.dir;
  auto payload = fx::qwen_payload();
  payload[3].bytes = "corrupt!";
  for (const auto& f : payload) {
    const auto file = secure_frontend::must_rel(dir + "/" + f.path);
    (void)secure_frontend::ensure_directory(root, secure_frontend::must_rel(file.generic().substr(0, file.generic().rfind('/'))));
    (void)secure_frontend::write_new_file(root, file, f.bytes);
  }
  (void)secure_frontend::write_new_file(root, secure_frontend::must_rel(dir + "/manifest.json"),
                                        fx::manifest_text("m", "t", 1, fx::qwen_payload()));
  expect(offline.run({"import", bundle.string(), "--trust-manifest", "sha256:" + entry.manifest_sha256, "--yes"}) == 7,
         "publish over a corrupt orphan: publish_failed (exit 7): " + offline.err);
  expect(offline.err.find("digest_mismatch") == std::string::npos && offline.err.find("target exists") != std::string::npos &&
             offline.err.find("delete it by hand") != std::string::npos,
         "the orphan hint, not digest_mismatch: " + offline.err);
}

void import_works_without_downloader() {
  CliHarness online;
  const auto entry = secure_frontend_test::cli::install(online);
  const auto bundle = online.scratch / "usb-bundle";
  expect(online.run({"export", "m:t", bundle.string()}) == 0, "export");
  CliHarness offline;  // pull.downloader = <scratch>/missing/curl
  expect(offline.run({"import", bundle.string(), "--trust-manifest", "sha256:" + entry.manifest_sha256, "--yes"}) == 0,
         "import needs no downloader");
  expect(std::get<secure_frontend::ModelIndex>(secure_frontend::read_model_index(offline.models())).models.size() == 1, "installed");
}

void cli_verify_export_import_round_trip() {
  CliHarness online;
  const auto entry = secure_frontend_test::cli::install(online);
  const auto bundle = online.scratch / "b";
  expect(online.run({"export", "m:t", bundle.string()}) == 0, "export");
  expect(online.run({"export", "m:t", bundle.string()}) == 7, "export into a non-empty directory: bundle_target_not_empty (exit 7)");
  CliHarness offline;
  expect(offline.run({"verify", bundle.string()}) == 5, "verify without trust: manifest_untrusted (exit 5)");
  expect(offline.run({"verify", bundle.string(), "--trust-manifest", "sha256:" + entry.manifest_sha256}) == 0, "verify: exit 0");
  expect(offline.run({"import", bundle.string(), "--trust-manifest", "sha256:" + entry.manifest_sha256}) == 2,
         "non-interactive import without --yes is declined (exit 2)");
  expect(offline.run({"import", bundle.string(), "--trust-manifest", "sha256:" + entry.manifest_sha256}, "", true) == 2,
         "interactive EOF is No (exit 2)");
  expect(offline.run({"import", bundle.string(), "--trust-manifest", "sha256:" + entry.manifest_sha256}, "y\n", true) == 0,
         "interactive y imports");
  expect(offline.run({"import", bundle.string(), "--trust-manifest", "sha256:" + entry.manifest_sha256, "--yes"}) == 0 &&
             offline.out.find("already installed") != std::string::npos, "re-import: already installed");
}

// Task 20 carry: a failed export leaves the target directory; the CLI says to remove it.
void failed_export_says_to_remove_target() {
  CliHarness h;
  (void)secure_frontend_test::cli::install(h);
  const FailingWrites failing;
  h.services.store = &failing;
  const auto bundle = h.scratch / "half";
  expect(h.run({"export", "m:t", bundle.string()}) == 6, "a write failure mid-export: disk_space (exit 6)");
  expect(std::filesystem::exists(bundle), "the target directory stays behind");
  expect(h.err.find("remove the target directory") != std::string::npos, "the error text says to remove it before retrying: " + h.err);
  expect(h.run({"export", "m:t", (h.scratch / "fresh").string()}) == 6 && h.run({"export", "m:t", bundle.string()}) == 7 &&
             h.err.find("remove the target directory") == std::string::npos,
         "a target that was not empty before is not ours to remove: no hint");
}

// Task 19 carry: a version directory the index does not name, whose manifest differs,
// blocks publish ("target exists"); the CLI says how to clear it.
void import_blocked_by_orphan_says_how_to_clear_it() {
  CliHarness online;
  const auto entry = secure_frontend_test::cli::install(online);
  const auto bundle = online.scratch / "b";
  expect(online.run({"export", "m:t", bundle.string()}) == 0, "export");
  CliHarness offline;
  const auto dir = "m/t/" + entry.dir;
  (void)secure_frontend::ensure_directory(offline.models(), secure_frontend::must_rel(dir));
  (void)secure_frontend::write_new_file(offline.models(), secure_frontend::must_rel(dir + "/manifest.json"), "{}\n");
  expect(offline.run({"import", bundle.string(), "--trust-manifest", "sha256:" + entry.manifest_sha256, "--yes"}) == 7,
         "publish over an orphan directory: publish_failed (exit 7): " + offline.err);
  expect(offline.err.find("target exists") != std::string::npos && offline.err.find("strata list --all") != std::string::npos &&
             offline.err.find("delete it by hand") != std::string::npos,
         "the error says how to clear the orphan: " + offline.err);
}

}  // namespace bundle_cases
