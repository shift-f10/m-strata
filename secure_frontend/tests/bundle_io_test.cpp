#include "bundle_cases.hpp"
#include "secure_frontend/bundle_io.hpp"
#include "secure_frontend/model_index_store.hpp"
#include "support/acquire_fixtures.hpp"
#include "support/expect.hpp"

#include <fstream>
#include <map>

#if !defined(_WIN32)
#include <iostream>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace bundle_cases {
namespace {

namespace fx = secure_frontend_test::acquire;
using secure_frontend::AcquireReason;
using secure_frontend::must_rel;
using secure_frontend_test::expect;

// F-47: free space is a seam, so the cases never depend on the build volume having 1 GiB spare.
class Roomy : public secure_frontend::StoreEnvironment {
 public:
  std::variant<std::uint64_t, secure_frontend::AcquireError> free_bytes(const secure_frontend::CanonicalRoot&) const override {
    return std::uint64_t{1} << 50U;
  }
};

// Answers exactly what --yes answers (C1: import now asks under the lock).
class YesOnly final : public secure_frontend::Confirmer {
 public:
  void say(std::string_view) override {}
  bool confirm(std::string_view, const bool yes_applies) override { return yes_applies; }
};

std::variant<secure_frontend::InstallOutcome, secure_frontend::AcquireError> import_yes(
    const secure_frontend::OpenedBundle& bundle, const secure_frontend::CanonicalRoot& models, const bool allow_downgrade,
    secure_frontend::LoadedVersions& loaded, const secure_frontend::StoreEnvironment& env, secure_frontend::ProgressSink& progress) {
  YesOnly ui;
  return secure_frontend::import_bundle(bundle, models, allow_downgrade, loaded, env, progress, ui, true);
}

struct Installed final {
  std::filesystem::path models_path;
  secure_frontend::CanonicalRoot models;
  secure_frontend::InstalledModel model;
};

Installed install(const std::vector<fx::PayloadFile>& payload, const bool bundle_allowed = true) {
  const auto path = secure_frontend_test::make_scratch_directory();
  auto root = fx::models_root(path);
  const auto manifest = fx::trusted(fx::manifest_text("m", "t", 1, payload, true, bundle_allowed), secure_frontend::ModelOrigin::third_party);
  fx::stage(root, manifest, payload);
  fx::FakeLoaded loaded;
  const Roomy env;
  const auto published = std::get<secure_frontend::PublishResult>(
      secure_frontend::publish_staged(root, secure_frontend::staging_key(manifest.parsed.sha256), manifest, loaded, env, true));
  auto model = std::get<secure_frontend::InstalledModel>(secure_frontend::load_installed(root, published.entry));
  return {path, std::move(root), std::move(model)};
}

std::filesystem::path exported(const Installed& installed) {
  const auto target = secure_frontend_test::make_scratch_directory() / "bundle";
  secure_frontend::ProgressSink quiet;
  const Roomy env;
  const auto failure = secure_frontend::export_model(installed.models, installed.model, target, env, quiet);
  expect(!failure.has_value(), "export: " + (failure ? secure_frontend::format_error(*failure) : std::string{}));
  return target;
}

std::optional<std::string> digest_of(const Installed& installed) { return installed.model.manifest.sha256; }

std::variant<secure_frontend::OpenedBundle, secure_frontend::AcquireError> open(const std::filesystem::path& dir,
                                                                               const std::optional<std::string>& trust) {
  secure_frontend::ProgressSink quiet;
  return secure_frontend::open_bundle(dir, trust, secure_frontend::ManifestRules{}, quiet);
}

AcquireReason reason_of(const std::variant<secure_frontend::OpenedBundle, secure_frontend::AcquireError>& result) {
  if (!std::holds_alternative<secure_frontend::AcquireError>(result)) {
    expect(false, "expected the bundle to be refused");
    return AcquireReason::usage_error;
  }
  return std::get<secure_frontend::AcquireError>(result).reason;
}

// Every regular file with its bytes, and every directory (key ends in '/'), so a new
// empty directory or file written into the tree is noticed too (F-48).
std::map<std::string, std::string> snapshot(const std::filesystem::path& dir) {
  std::map<std::string, std::string> files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
    const auto relative = entry.path().lexically_relative(dir).generic_string();
    if (entry.is_directory()) files[relative + "/"] = {};
    if (entry.is_regular_file()) files[relative] = secure_frontend_test::read_file(entry.path());
  }
  return files;
}

}  // namespace

void dl02_import_bundle_publishes_same_as_pull() {
  const auto source = install(fx::qwen_payload());
  const auto bundle = exported(source);
  expect(secure_frontend_test::read_file(bundle / "STRATA-BUNDLE") == "strata-bundle 1\n", "marker written last, exact");
  expect(!std::filesystem::exists(bundle / ".staging") && !std::filesystem::exists(bundle / "MISSING"), "a complete bundle");
  const auto opened = open(bundle, digest_of(source));
  expect(std::holds_alternative<secure_frontend::OpenedBundle>(opened), "bundle opens with the typed digest");
  const auto target_path = secure_frontend_test::make_scratch_directory();
  const auto target = fx::models_root(target_path);
  fx::FakeLoaded loaded;
  secure_frontend::ProgressSink quiet;
  const Roomy env;
  const auto outcome = import_yes(std::get<secure_frontend::OpenedBundle>(opened), target, false, loaded, env, quiet);
  expect(std::holds_alternative<secure_frontend::InstallOutcome>(outcome), "import publishes");
  const auto index = std::get<secure_frontend::ModelIndex>(secure_frontend::read_model_index(target));
  expect(index.models.size() == 1 && index.models.front().dir == source.model.entry.dir &&
             index.models.front().manifest_sha256 == source.model.entry.manifest_sha256,
         "same version directory and digest as the original publish");
  const auto again = import_yes(std::get<secure_frontend::OpenedBundle>(opened), target, false, loaded, env, quiet);
  expect(std::get<secure_frontend::InstallOutcome>(again).already_installed, "re-import: already installed");
  expect(snapshot(source.models_path / "m" / "t" / source.model.entry.dir) == snapshot(target_path / "m" / "t" / source.model.entry.dir),
         "byte-identical version directories");
  expect(!std::filesystem::exists(target_path / ".staging" / secure_frontend::staging_key(source.model.entry.manifest_sha256)),
         "staging consumed by publish");
}

void dl03_bundle_digest_mismatch_publishes_nothing() {
  const auto source = install(fx::qwen_payload());
  const auto bundle = exported(source);
  secure_frontend_test::write_file(bundle / "files" / "pack" / "dense.bin", "dense X");  // other bytes than the manifest pins
  const auto opened = open(bundle, digest_of(source));
  secure_frontend::ProgressSink quiet;
  const auto verified = secure_frontend::verify_bundle_files(std::get<secure_frontend::OpenedBundle>(opened), quiet);
  expect(verified && verified->reason == AcquireReason::bundle_invalid, "verify reports bundle_invalid (exit 4)");
  const auto target_path = secure_frontend_test::make_scratch_directory();
  const auto target = fx::models_root(target_path);
  fx::FakeLoaded loaded;
  const Roomy env;
  const auto outcome = import_yes(std::get<secure_frontend::OpenedBundle>(opened), target, false, loaded, env, quiet);
  expect(std::holds_alternative<secure_frontend::AcquireError>(outcome) &&
             std::get<secure_frontend::AcquireError>(outcome).reason == AcquireReason::bundle_invalid, "import refuses");
  expect(std::get<secure_frontend::ModelIndex>(secure_frontend::read_model_index(target)).models.empty(), "nothing published");
  expect(!std::filesystem::exists(target_path / "m"), "no version directory");
  // Same size, other bytes: caught by the digest, not the size.
  secure_frontend_test::write_file(bundle / "files" / "pack" / "dense.bin", "dense!");
  const auto same_size = import_yes(std::get<secure_frontend::OpenedBundle>(opened), target, false, loaded, env, quiet);
  expect(std::holds_alternative<secure_frontend::AcquireError>(same_size) &&
             std::get<secure_frontend::AcquireError>(same_size).reason == AcquireReason::bundle_invalid, "same-size corruption refused");
  expect(!std::filesystem::exists(target_path / "m"), "still no version directory");
}

void bundle_rejects_traversal_path() {
  auto payload = fx::qwen_payload();
  const auto source = install(payload);
  const auto bundle = exported(source);
  std::string manifest = secure_frontend_test::read_file(bundle / "manifest.json");
  manifest.replace(manifest.find("pack/index.txt"), 14, "../escape.txtx");
  secure_frontend_test::write_file(bundle / "manifest.json", manifest);
  const auto digest = secure_frontend::to_hex(secure_frontend::sha256(manifest));
  const auto opened = open(bundle, digest);
  expect(std::holds_alternative<secure_frontend::AcquireError>(opened) && reason_of(opened) == AcquireReason::manifest_invalid,
         "a manifest path with .. never reaches the file system");
  expect(!std::filesystem::exists(bundle.parent_path() / "escape.txtx"), "nothing outside the bundle");
}

void bundle_rejects_unlisted_file_in_files_dir() {
  const auto source = install(fx::qwen_payload());
  const auto bundle = exported(source);
  secure_frontend_test::write_file(bundle / "files" / "pack" / "extra.bin", "x");
  expect(reason_of(open(bundle, digest_of(source))) == AcquireReason::bundle_layout, "unknown entry under files/");
  std::filesystem::remove(bundle / "files" / "pack" / "extra.bin");
  secure_frontend_test::write_file(bundle / "NOTES.txt", "ignored");
  expect(std::holds_alternative<secure_frontend::OpenedBundle>(open(bundle, digest_of(source))), "unknown top-level entries are ignored");
  std::filesystem::remove(bundle / "files" / "pack" / "dense.bin");
  expect(reason_of(open(bundle, digest_of(source))) == AcquireReason::bundle_layout, "a manifest file absent from files/");
}

void bundle_sha256sums_must_match_manifest() {
  const auto source = install(fx::qwen_payload());
  const auto bundle = exported(source);
  const std::string good = secure_frontend_test::read_file(bundle / "SHA256SUMS");
  std::string sums = good;
  sums[0] = sums[0] == '0' ? '1' : '0';
  secure_frontend_test::write_file(bundle / "SHA256SUMS", sums);
  expect(reason_of(open(bundle, digest_of(source))) == AcquireReason::bundle_invalid, "SHA256SUMS disagreeing with the manifest");
  std::filesystem::remove(bundle / "SHA256SUMS");
  expect(reason_of(open(bundle, digest_of(source))) == AcquireReason::bundle_invalid, "SHA256SUMS absent");
  secure_frontend_test::write_file(bundle / "SHA256SUMS", good);
  expect(std::holds_alternative<secure_frontend::OpenedBundle>(open(bundle, digest_of(source))), "restored");
}

void bundle_rejects_hardlinked_file() {
  const auto source = install(fx::qwen_payload());
  const auto bundle = exported(source);
  std::filesystem::create_hard_link(bundle / "files" / "pack" / "dense.bin", bundle.parent_path() / "outside-link");
  expect(reason_of(open(bundle, digest_of(source))) == AcquireReason::bundle_layout, "a hard-linked payload file");
}

#if defined(_WIN32)
void bundle_rejects_junction_entry_windows() {
  const auto source = install(fx::qwen_payload());
  const auto bundle = exported(source);
  const auto outside = secure_frontend_test::make_scratch_directory();
  std::filesystem::rename(bundle / "files" / "pack" / "tokenizer", outside / "tokenizer");
  expect(secure_frontend_test::create_junction(bundle / "files" / "pack" / "tokenizer", outside / "tokenizer"), "junction");
  expect(reason_of(open(bundle, digest_of(source))) == AcquireReason::bundle_layout, "a junction under files/");
}
#else
void bundle_rejects_symlink_entry() {
  const auto source = install(fx::qwen_payload());
  const auto bundle = exported(source);
  const auto real = bundle.parent_path() / "dense.real";
  std::filesystem::rename(bundle / "files" / "pack" / "dense.bin", real);
  std::filesystem::create_symlink(real, bundle / "files" / "pack" / "dense.bin");
  expect(reason_of(open(bundle, digest_of(source))) == AcquireReason::bundle_layout, "a symlink under files/");
  std::filesystem::remove(bundle / "files" / "pack" / "dense.bin");
  std::filesystem::rename(real, bundle / "files" / "pack" / "dense.bin");
  std::filesystem::rename(bundle / "manifest.json", bundle.parent_path() / "manifest.real");
  std::filesystem::create_symlink(bundle.parent_path() / "manifest.real", bundle / "manifest.json");
  expect(reason_of(open(bundle, digest_of(source))) == AcquireReason::bundle_layout, "a symlinked manifest.json");
}

// Fix round 1 (I1): a bundle directory that is itself a mount point (a USB stick's
// mount root, a loop/ISO mount) is read like any other directory; only writing a
// bundle onto a mount root is refused. Simulated with a tmpfs in a user+mount namespace.
void bundle_at_mount_root_is_read_but_never_written() {
  const auto source = install(fx::qwen_payload());
  const auto bundle = exported(source);
  const auto scratch = secure_frontend_test::make_scratch_directory();
  const auto usb = scratch / "usb";
  const auto empty_mount = scratch / "empty";
  secure_frontend_test::make_private_directory(usb);
  secure_frontend_test::make_private_directory(empty_mount);
  const auto models_path = secure_frontend_test::make_scratch_directory();
  const auto digest = digest_of(source);
  const auto manifest = source.model.manifest;
  const auto origin = source.model.entry.origin;
  const uid_t uid = ::geteuid();
  const gid_t gid = ::getegid();
  const pid_t child = ::fork();
  if (child == 0) {
    if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) ::_exit(77);
    std::ofstream("/proc/self/setgroups") << "deny";
    std::ofstream("/proc/self/uid_map") << uid << ' ' << uid << " 1";
    std::ofstream("/proc/self/gid_map") << gid << ' ' << gid << " 1";
    if (::mount("none", usb.c_str(), "tmpfs", 0, "mode=0700") != 0) ::_exit(77);
    if (::mount("none", empty_mount.c_str(), "tmpfs", 0, "mode=0700") != 0) ::_exit(77);
    std::error_code ec;
    std::filesystem::copy(bundle, usb, std::filesystem::copy_options::recursive, ec);
    if (ec) ::_exit(2);
    const auto opened = open(usb, digest);
    if (!std::holds_alternative<secure_frontend::OpenedBundle>(opened)) ::_exit(3);
    secure_frontend::ProgressSink quiet;
    if (secure_frontend::verify_bundle_files(std::get<secure_frontend::OpenedBundle>(opened), quiet)) ::_exit(4);
    auto models = secure_frontend::open_root(models_path);
    if (!std::holds_alternative<secure_frontend::CanonicalRoot>(models)) ::_exit(5);
    fx::FakeLoaded loaded;
    const Roomy env;
    if (!std::holds_alternative<secure_frontend::InstallOutcome>(import_yes(
            std::get<secure_frontend::OpenedBundle>(opened), std::get<secure_frontend::CanonicalRoot>(models), false, loaded, env, quiet))) {
      ::_exit(6);
    }
    const auto prepared = secure_frontend::prepare_bundle_target(empty_mount, secure_frontend::TrustedManifest{manifest, origin}, false, env);
    const auto* refused = std::get_if<secure_frontend::AcquireError>(&prepared);
    if (refused == nullptr || refused->reason != AcquireReason::bundle_target_not_empty ||
        refused->detail != "the bundle directory is a drive or mount root") {
      ::_exit(7);
    }
    ::_exit(0);
  }
  int status = 0;
  ::waitpid(child, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 77) {
    std::cout << "SKIPPED: no unprivileged mount namespace on this host\n";
    return;
  }
  expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "a bundle at a mount root opens, verifies and imports; an empty mount root is refused as an export target (child exit " +
             std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")");
}

void bundle_walk_fails_closed_on_overlong_or_odd_names() {
  const auto source = install(fx::qwen_payload());
  const auto bundle = exported(source);
  auto deep = bundle / "files";
  for (int i = 0; i < 5; ++i) {  // files/ + 5 x 251 bytes > P1's 1024, every component valid
    deep /= std::string(250, static_cast<char>('a' + i));
    secure_frontend_test::make_private_directory(deep);
  }
  const auto overlong = open(bundle, digest_of(source));  // must return, never throw
  expect(std::holds_alternative<secure_frontend::AcquireError>(overlong) && reason_of(overlong) == AcquireReason::bundle_invalid,
         "an over-long tree on bundle media fails closed");
  std::filesystem::remove_all(bundle / "files" / std::string(250, 'a'));
  expect(std::holds_alternative<secure_frontend::OpenedBundle>(open(bundle, digest_of(source))), "clean again");
  for (const char* odd : {"bad:name", "CON", "trailing.", "x\x01y"}) {
    secure_frontend_test::write_file(bundle / "files" / "pack" / odd, "x");
    const auto refused = open(bundle, digest_of(source));
    expect(std::holds_alternative<secure_frontend::AcquireError>(refused) && reason_of(refused) == AcquireReason::bundle_layout,
           std::string{"a non-portable name under files/ is an unknown entry: "} + odd);
    std::filesystem::remove(bundle / "files" / "pack" / odd);
  }
}
#endif

void export_refuses_bundle_forbidden_license() {
  const auto source = install(fx::qwen_payload(), false);
  const auto target = secure_frontend_test::make_scratch_directory() / "bundle";
  secure_frontend::ProgressSink quiet;
  const Roomy env;
  const auto failure = secure_frontend::export_model(source.models, source.model, target, env, quiet);
  expect(failure && failure->reason == AcquireReason::license_forbids_bundle, "exit 10");
  expect(failure && secure_frontend::exit_class(failure->reason) == secure_frontend::ExitCode::license, "exit class 10");
  expect(!std::filesystem::exists(target), "before any byte is written");
}

void export_refuses_when_one_file_license_forbids_bundle() {
  // Top-level license allows bundling; one file's own license does not (effective_license).
  std::string text = fx::manifest_text("m", "t", 1, fx::qwen_payload(), true, true);
  const std::string anchor = "{\"path\":\"pack/dense.bin\",";
  text.replace(text.find(anchor), anchor.size(),
               anchor + "\"license\":{\"id\":\"Other\",\"url\":\"https://fixture.example/l2\",\"bundle_allowed\":false,\"notice\":\"\"},");
  const auto manifest = fx::trusted(text, secure_frontend::ModelOrigin::third_party);
  expect(manifest.parsed.manifest.files[3].license.has_value(), "the per-file license parsed");
  const auto target = secure_frontend_test::make_scratch_directory() / "bundle";
  const Roomy env;
  const auto prepared = secure_frontend::prepare_bundle_target(target, manifest, false, env);
  expect(std::holds_alternative<secure_frontend::AcquireError>(prepared) &&
             std::get<secure_frontend::AcquireError>(prepared).reason == AcquireReason::license_forbids_bundle,
         "one file's effective license forbids the bundle");
  expect(!std::filesystem::exists(target), "nothing written");
}

void export_refuses_fat32_target_for_large_file() {
  class Fat final : public Roomy {
   public:
    std::variant<secure_frontend::FileSystemKind, secure_frontend::AcquireError> file_system_kind(
        const secure_frontend::CanonicalRoot&) const override { return secure_frontend::FileSystemKind::fat; }
    std::uint64_t fat_max_file_bytes() const override { return 10; }  // stands in for 4 GiB - 1
  };
  const auto source = install(fx::qwen_payload());
  const auto target = secure_frontend_test::make_scratch_directory() / "bundle";
  secure_frontend::ProgressSink quiet;
  const Fat env;
  const auto failure = secure_frontend::export_model(source.models, source.model, target, env, quiet);
  expect(failure && failure->reason == AcquireReason::bundle_target_fat32, "bundle_target_fat32");
  expect(!std::filesystem::exists(target / "files"), "no payload copied");
}

void export_rehashes_and_rejects_bit_rot() {
  const auto source = install(fx::qwen_payload());
  secure_frontend_test::write_file(source.models.join(must_rel("m/t/" + source.model.entry.dir + "/pack/index.txt")), "index ROT");
  const auto target = secure_frontend_test::make_scratch_directory() / "bundle";
  secure_frontend::ProgressSink quiet;
  const Roomy env;
  const auto failure = secure_frontend::export_model(source.models, source.model, target, env, quiet);
  expect(failure && failure->reason == AcquireReason::digest_mismatch, "bit rot is not propagated");
  expect(!std::filesystem::exists(target / "STRATA-BUNDLE"), "no marker on a failed export");
  expect(!std::filesystem::exists(target / "files" / "pack" / "index.txt") &&
             !std::filesystem::exists(target / "files" / "pack" / "index.txt.part"),
         "the rotten file is neither published into files/ nor left as .part");
  // Same size, other bytes: only the digest notices.
  secure_frontend_test::write_file(source.models.join(must_rel("m/t/" + source.model.entry.dir + "/pack/index.txt")), "INDEX ");
  const auto second = secure_frontend_test::make_scratch_directory() / "bundle";
  const auto again = secure_frontend::export_model(source.models, source.model, second, env, quiet);
  expect(again && again->reason == AcquireReason::digest_mismatch, "same-size bit rot is not propagated");
}

void export_onto_shared_writable_target() {
  const auto source = install(fx::qwen_payload());
  const auto target = secure_frontend_test::make_scratch_directory() / "fat";
  secure_frontend_test::make_private_directory(target);
#if !defined(_WIN32)
  ::chmod(target.c_str(), 0777);  // what an exFAT/FAT mount with dmask=000 reports
#endif
  secure_frontend::ProgressSink quiet;
  const Roomy env;
  const auto failure = secure_frontend::export_model(source.models, source.model, target, env, quiet);
  expect(!failure.has_value(), "export onto a shared-writable (removable-media) target: " +
                                   (failure ? secure_frontend::format_error(*failure) : std::string{}));
  expect(std::holds_alternative<secure_frontend::OpenedBundle>(open(target, digest_of(source))), "and the bundle opens");
  const auto again = secure_frontend::export_model(source.models, source.model, target, env, quiet);
  expect(again && again->reason == AcquireReason::bundle_target_not_empty, "a second export into it is refused");
}

void bundle_directory_must_not_be_a_volume_root() {
  const auto volume = std::filesystem::absolute(secure_frontend_test::make_scratch_directory()).root_path();
  const auto opened = open(volume, std::nullopt);
  expect(std::holds_alternative<secure_frontend::AcquireError>(opened) && reason_of(opened) == AcquireReason::bundle_layout,
         "a drive or mount root is never read as a bundle (§5.1)");
  const auto manifest = fx::trusted(fx::manifest_text("m", "t", 1, fx::qwen_payload()), secure_frontend::ModelOrigin::third_party);
  const Roomy env;
  const auto prepared = secure_frontend::prepare_bundle_target(volume, manifest, true, env);
  expect(std::holds_alternative<secure_frontend::AcquireError>(prepared) &&
             std::get<secure_frontend::AcquireError>(prepared).reason == AcquireReason::bundle_target_not_empty &&
             std::get<secure_frontend::AcquireError>(prepared).detail == "the bundle directory is a drive or mount root",
         "nor written as one");
}

void verify_writes_nothing() {
  const auto source = install(fx::qwen_payload());
  const auto bundle = exported(source);
  const auto before = snapshot(bundle);
  const auto opened = open(bundle, digest_of(source));
  secure_frontend::ProgressSink quiet;
  expect(!secure_frontend::verify_bundle_files(std::get<secure_frontend::OpenedBundle>(opened), quiet), "verifies");
  expect(snapshot(bundle) == before, "verify writes nothing");
}

void import_from_read_only_bundle_directory() {
  const auto source = install(fx::qwen_payload());
  const auto bundle = exported(source);
  const auto before = snapshot(bundle);
#if !defined(_WIN32)
  // F-48: modes are restored even when a check throws, so scratch cleanup still works.
  struct RestoreModes final {
    std::filesystem::path dir;
    ~RestoreModes() {
      ::chmod(dir.c_str(), 0700);
      std::error_code ec;
      for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator();
           it.increment(ec)) {
        ::chmod(it->path().c_str(), 0700);
      }
    }
  } restore{bundle};
  std::vector<std::filesystem::path> entries;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(bundle)) entries.push_back(entry.path());
  for (auto it = entries.rbegin(); it != entries.rend(); ++it) {  // children before parents
    ::chmod(it->c_str(), std::filesystem::is_directory(*it) ? 0555 : 0444);
  }
  ::chmod(bundle.c_str(), 0777);  // and removable-media modes on the bundle root
#endif
  const auto opened = open(bundle, digest_of(source));
  expect(std::holds_alternative<secure_frontend::OpenedBundle>(opened), "a read-only (or shared-writable) bundle opens");
  if (!std::holds_alternative<secure_frontend::OpenedBundle>(opened)) return;
  secure_frontend::ProgressSink quiet;
  expect(!secure_frontend::verify_bundle_files(std::get<secure_frontend::OpenedBundle>(opened), quiet), "and verifies");
  const auto target = fx::models_root(secure_frontend_test::make_scratch_directory());
  fx::FakeLoaded loaded;
  const Roomy env;
  const auto outcome = import_yes(std::get<secure_frontend::OpenedBundle>(opened), target, false, loaded, env, quiet);
  expect(std::holds_alternative<secure_frontend::InstallOutcome>(outcome),
         "import from read-only media: " + (std::holds_alternative<secure_frontend::AcquireError>(outcome)
                                                ? secure_frontend::format_error(std::get<secure_frontend::AcquireError>(outcome))
                                                : std::string{"ok"}));
  expect(snapshot(bundle) == before, "import never writes into the bundle (checked by content, so also as root)");
}

void bundle_incomplete_and_marker_rules() {
  const auto source = install(fx::qwen_payload());
  const auto bundle = exported(source);
  std::filesystem::remove(bundle / "STRATA-BUNDLE");
  expect(reason_of(open(bundle, digest_of(source))) == AcquireReason::bundle_incomplete, "no marker: incomplete");
  secure_frontend_test::write_file(bundle / "STRATA-BUNDLE", "strata-bundle 2\n");
  expect(reason_of(open(bundle, digest_of(source))) == AcquireReason::bundle_layout, "unknown marker version");
  secure_frontend_test::write_file(bundle / "STRATA-BUNDLE", "strata-bundle 1\n");
  secure_frontend_test::write_file(bundle / "MISSING", "pack/dense.bin\n");
  expect(reason_of(open(bundle, digest_of(source))) == AcquireReason::bundle_incomplete, "MISSING present: incomplete");
  std::filesystem::remove(bundle / "MISSING");
  expect(reason_of(open(bundle, std::nullopt)) == AcquireReason::manifest_untrusted, "no catalog digest, no typed digest");
  expect(reason_of(open(bundle, std::string(64, 'a'))) == AcquireReason::manifest_untrusted, "a different typed digest");
}

// Task 13 carry: tests/fixtures/bundle/make-bundle-iq2_xs is byte-for-byte what
// tools/secure/make_bundle.py writes (pinned by tools/secure/test_make_bundle.py):
// pretty-printed manifest, third-party "<quant>-local" tag, sourceless pack files.
void make_bundle_tool_output_opens_verifies_and_imports() {
  const auto& args = secure_frontend_test::extra_arguments();
  expect(args.size() == 1, "fixture directory argument");
  if (args.size() != 1) return;
  const std::filesystem::path fixture{args[0]};
  const auto bundle = secure_frontend_test::make_scratch_directory() / "usb-qwen-iq2_xs";
  secure_frontend_test::make_private_directory(bundle);
  for (const auto& entry : std::filesystem::recursive_directory_iterator(fixture)) {
    const auto to = bundle / entry.path().lexically_relative(fixture);
    if (entry.is_directory()) secure_frontend_test::make_private_directory(to);
    else secure_frontend_test::write_file(to, secure_frontend_test::read_file(entry.path()));
  }
  const auto digest = secure_frontend::to_hex(secure_frontend::sha256(secure_frontend_test::read_file(bundle / "manifest.json")));
  expect(reason_of(open(bundle, std::nullopt)) == AcquireReason::manifest_untrusted, "not in the catalog: --trust-manifest required");
  const auto opened = open(bundle, digest);  // strata import <dir> --trust-manifest sha256:<digest>
  expect(std::holds_alternative<secure_frontend::OpenedBundle>(opened),
         "the tool's bundle opens: " + (std::holds_alternative<secure_frontend::AcquireError>(opened)
                                            ? secure_frontend::format_error(std::get<secure_frontend::AcquireError>(opened))
                                            : std::string{"ok"}));
  if (!std::holds_alternative<secure_frontend::OpenedBundle>(opened)) return;
  const auto& b = std::get<secure_frontend::OpenedBundle>(opened);
  expect(b.manifest.origin == secure_frontend::ModelOrigin::third_party && b.manifest.parsed.manifest.name == "qwen3.8-flash-next" &&
             b.manifest.parsed.manifest.tag == "iq2_xs-local",
         "third-party <quant>-local manifest");
  secure_frontend::ProgressSink quiet;
  expect(!secure_frontend::verify_bundle_files(b, quiet), "verifies");
  const auto target = fx::models_root(secure_frontend_test::make_scratch_directory());
  fx::FakeLoaded loaded;
  const Roomy env;
  const auto outcome = import_yes(b, target, false, loaded, env, quiet);
  expect(std::holds_alternative<secure_frontend::InstallOutcome>(outcome),
         "imports: " + (std::holds_alternative<secure_frontend::AcquireError>(outcome)
                            ? secure_frontend::format_error(std::get<secure_frontend::AcquireError>(outcome))
                            : std::string{"ok"}));
  const auto index = std::get<secure_frontend::ModelIndex>(secure_frontend::read_model_index(target));
  expect(index.models.size() == 1 && index.models.front().name == "qwen3.8-flash-next" && index.models.front().tag == "iq2_xs-local" &&
             index.models.front().manifest_sha256 == digest && index.models.front().origin == secure_frontend::ModelOrigin::third_party,
         "indexed as the tool's manifest");
}

}  // namespace bundle_cases
