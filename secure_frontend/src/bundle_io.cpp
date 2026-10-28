#include "secure_frontend/bundle_io.hpp"

#include "secure_frontend/acquire_pull.hpp"
#include "secure_frontend/bundle_text.hpp"
#include "secure_frontend/model_index_write.hpp"

#include <set>
#include <system_error>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include "windows_security.hpp"
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace secure_frontend {
namespace {

constexpr std::size_t kSumsMaxBytes = std::size_t{8} * 1024U * 1024U;
constexpr std::string_view kBundleStagingDir = ".staging";

AcquireError err(const AcquireReason reason, std::string detail) { return AcquireError{reason, std::move(detail)}; }

// files/<path>: manifest paths are P1-valid and validate_manifest guarantees the
// "files/" form fits P1 too (Task 18 stored-path ruling), so this never throws.
RelativePath files_path(const std::string& manifest_path) { return must_rel(std::string{kBundleFilesDir} + "/" + manifest_path); }

std::optional<AcquireError> ensure_parent(const CanonicalRoot& root, const RelativePath& path) {
  const auto& generic = path.generic();
  const auto slash = generic.rfind('/');
  if (slash == std::string::npos) return std::nullopt;
  return ensure_directory(root, must_rel(generic.substr(0, slash)));
}

enum class Created { created, existed, failed };

// §5.3 "Created 0700 / private DACL" in one step (F-45: no umask-mode window).
// Removable media may ignore the mode; nothing relies on it.
Created create_target_directory(const std::filesystem::path& dir) {
#if defined(_WIN32)
  windows_security::PrivateSecurityAttributes security(true);
  if (CreateDirectoryW(windows_security::extended_path(std::filesystem::absolute(dir).wstring()).c_str(), security.get()) != 0) {
    return Created::created;
  }
  return GetLastError() == ERROR_ALREADY_EXISTS ? Created::existed : Created::failed;
#else
  if (::mkdir(dir.c_str(), 0700) == 0) return Created::created;
  return errno == EEXIST ? Created::existed : Created::failed;
#endif
}

// §5.1 "not a drive root", lexically: "/" or "X:\". The only refusal on the read side:
// a bundle whose directory is a mount point (USB stick, loop/ISO, NFS) is read normally.
bool is_drive_root(const std::filesystem::path& path) { return !path.has_relative_path(); }

// Write side only (export, pull --bundle): also refuse a POSIX mount point (the
// directory's parent is on another device), so a bundle never takes over a whole
// volume. Fails closed when it cannot tell.
bool is_mount_root(const CanonicalRoot& root) {
#if defined(_WIN32)
  (void)root;
  return false;
#else
  struct stat parent {};
  if (::fstatat(root.handle(), "..", &parent, 0) != 0) return true;
  return static_cast<std::uint64_t>(parent.st_dev) != root.device_id();
#endif
}

constexpr std::string_view kVolumeRootDetail = "the bundle directory is a drive or mount root";

}  // namespace

std::variant<std::uint64_t, AcquireError> copy_verified(const CanonicalRoot& from_root, const RelativePath& from,
                                                        const CanonicalRoot& to_root, const RelativePath& to, const ManifestFile& file,
                                                        const StoreEnvironment& env, ProgressSink& progress,
                                                        const AcquireReason on_mismatch, const std::atomic<bool>* interrupted) {
  auto opened = open_beneath(from_root, from, OpenOptions{OpenKind::regular_file, FileTrust::any, true});
  if (std::holds_alternative<PathError>(opened)) {
    // A bundle source is untrusted media (§5.2); an installed file that cannot be opened is a store problem.
    return err(from_root.external() ? AcquireReason::bundle_layout : AcquireReason::index_inconsistent,
               file.path + ": not a single-link regular file");
  }
  const auto& source = std::get<OpenedFile>(opened);
  if (source.size() != file.size) return err(on_mismatch, file.path + ": size differs from the manifest");
  if (auto failure = ensure_parent(to_root, to)) return *failure;
  const auto part = must_rel(to.generic() + std::string{kPartSuffix});
  if (auto failure = remove_file_within(to_root, part)) return *failure;
  auto created = open_part_file(to_root, part, PartOpen::create_new, env);
  if (auto* failure = std::get_if<AcquireError>(&created)) return *failure;
  auto& out = std::get<PartFile>(created);
  const auto abandon = [&](AcquireError error) -> std::variant<std::uint64_t, AcquireError> {
    out.close();
    (void)remove_file_within(to_root, part);
    return error;
  };
  if (auto failure = out.preallocate(file.size)) return abandon(*failure);
  Sha256 hasher;
  std::string buffer(kSha256ReadChunkBytes, '\0');
  progress.file_started(file.path, file.size);
  while (true) {
    if (interrupted != nullptr && interrupted->load()) return abandon(err(AcquireReason::interrupted, file.path));
    auto read = source.read_some(buffer.data(), buffer.size());
    if (std::holds_alternative<PathError>(read)) {
      return abandon(err(from_root.external() ? AcquireReason::bundle_layout : AcquireReason::index_inconsistent,
                         file.path + ": read failed"));
    }
    const auto n = std::get<std::size_t>(read);
    if (n == 0) break;
    if (out.size() + n > file.size) return abandon(err(on_mismatch, file.path + ": longer than the manifest size"));
    const std::string_view chunk{buffer.data(), n};
    hasher.update(chunk);
    if (auto failure = out.append(chunk)) return abandon(*failure);
    progress.file_bytes(file.path, out.size(), file.size);
  }
  if (out.size() != file.size || to_hex(hasher.finish()) != file.sha256) return abandon(err(on_mismatch, file.path));
  if (auto failure = out.sync()) return abandon(*failure);
  out.close();
  if (auto failure = rename_within(to_root, part, to, env)) {
    (void)remove_file_within(to_root, part);
    return *failure;
  }
  return file.size;
}

std::variant<OpenedBundle, AcquireError> open_bundle(const std::filesystem::path& dir, const std::optional<std::string>& trust_digest,
                                                     const ManifestRules& rules, ProgressSink& progress) {
  // Every access below is a read through the held handle; nothing is ever written here.
  auto opened = open_root(dir, RootPolicy::external);
  if (std::holds_alternative<PathError>(opened)) return err(AcquireReason::bundle_layout, "the bundle directory cannot be opened");
  auto root = std::get<CanonicalRoot>(std::move(opened));
  if (is_drive_root(root.path())) return err(AcquireReason::bundle_layout, std::string{kVolumeRootDetail});
  auto top = list_directory(root);
  if (std::holds_alternative<PathError>(top)) return err(AcquireReason::bundle_layout, "the bundle directory cannot be listed");
  std::set<std::string, std::less<>> names;
  for (const auto& entry : std::get<std::vector<DirectoryEntry>>(top)) {
    names.insert(entry.name);
    const bool known = entry.name == kBundleMarkerName || entry.name == kBundleManifestName || entry.name == kBundleSumsName ||
                       entry.name == kBundleMissingName || entry.name == kBundleFilesDir;
    // Fixed text only: entry names on untrusted media may hold anything (§7.3).
    if (!known) progress.notice("ignored an unknown top-level entry in the bundle");
  }
  if (names.contains(kBundleMissingName) || !names.contains(kBundleMarkerName)) {
    return err(AcquireReason::bundle_incomplete, "the bundle is incomplete (MISSING present or STRATA-BUNDLE absent)");
  }
  auto marker = read_small_file(root, must_rel(kBundleMarkerName), 64);
  if (std::holds_alternative<AcquireError>(marker) || std::get<std::string>(marker) != kBundleMarkerContent) {
    return err(AcquireReason::bundle_layout, "STRATA-BUNDLE is not a version 1 bundle marker");
  }
  auto manifest_bytes = read_small_file(root, must_rel(kBundleManifestName), kManifestMaxBytes);
  if (std::holds_alternative<AcquireError>(manifest_bytes)) return err(AcquireReason::bundle_layout, "manifest.json cannot be read");
  auto trusted = trust_manifest_bytes(std::get<std::string>(manifest_bytes), trust_digest, rules);
  if (auto* failure = std::get_if<AcquireError>(&trusted)) return *failure;
  auto manifest = std::get<TrustedManifest>(std::move(trusted));
  auto sums_text = read_small_file(root, must_rel(kBundleSumsName), kSumsMaxBytes);
  const auto sums = std::holds_alternative<std::string>(sums_text) ? parse_sha256sums(std::get<std::string>(sums_text)) : std::nullopt;
  if (!sums || !sums_agree(*sums, manifest.parsed.sha256, manifest.parsed.manifest)) {
    return err(AcquireReason::bundle_invalid, "SHA256SUMS does not agree with manifest.json");
  }
  // F-6: names on the media are untrusted; an over-long tree fails closed (bundle_invalid), never throws.
  auto listed = list_files_recursive(root, must_rel(kBundleFilesDir));
  if (auto* failure = std::get_if<AcquireError>(&listed)) {
    return failure->reason == AcquireReason::bundle_invalid ? *failure : err(AcquireReason::bundle_layout, failure->detail);
  }
  std::set<std::string> expected;
  for (const auto& f : manifest.parsed.manifest.files) expected.insert(f.path);
  std::set<std::string> found;
  for (const auto& entry : std::get<std::vector<StagedEntry>>(listed)) {
    // A non-portable name is never in the manifest; reported by fixed text, not by the name.
    if (!std::holds_alternative<RelativePath>(parse_relative_path(entry.path))) {
      return err(AcquireReason::bundle_layout, "files/ holds an entry that is not in the manifest");
    }
    if (!entry.regular) return err(AcquireReason::bundle_layout, "files/" + entry.path + ": link, junction, hard link, device or FIFO");
    if (!expected.contains(entry.path)) return err(AcquireReason::bundle_layout, "files/" + entry.path + ": not in the manifest");
    found.insert(entry.path);
  }
  if (found != expected) return err(AcquireReason::bundle_layout, "files/ is missing manifest files");
  return OpenedBundle{std::move(root), std::move(manifest)};
}

std::optional<AcquireError> verify_bundle_files(const OpenedBundle& bundle, ProgressSink& progress) {
  for (const auto& f : bundle.manifest.parsed.manifest.files) {
    progress.file_started(f.path, f.size);
    Sha256 hasher;
    auto counted = hash_into(bundle.root, files_path(f.path), hasher, f.size);
    if (std::holds_alternative<AcquireError>(counted) || std::get<std::uint64_t>(counted) != f.size || to_hex(hasher.finish()) != f.sha256) {
      return err(AcquireReason::bundle_invalid, f.path);
    }
    progress.file_bytes(f.path, f.size, f.size);
  }
  return std::nullopt;
}

std::variant<InstallOutcome, AcquireError> import_bundle(const OpenedBundle& bundle, const CanonicalRoot& models, const bool allow_downgrade,
                                                         LoadedVersions& loaded, const StoreEnvironment& env, ProgressSink& progress,
                                                         Confirmer& ui, const bool yes) {
  auto lock = lock_models_root(models);
  if (auto* failure = std::get_if<AcquireError>(&lock)) return *failure;
  auto read = read_model_index(models);
  if (auto* failure = std::get_if<AcquireError>(&read)) return *failure;
  const auto& index = std::get<ModelIndex>(read);  // InstallDecision::installed points into it
  const auto& manifest = bundle.manifest;
  auto decision = decide_install(index, manifest, allow_downgrade);
  if (auto* failure = std::get_if<AcquireError>(&decision)) return *failure;
  const auto& d = std::get<InstallDecision>(decision);
  if (d.kind == InstallDecisionKind::already_installed) return InstallOutcome{*d.installed, true, false, {}};  // §8.3: asks nothing
  // Final review C1: §6.1's --yes limits hold for import as for pull (Task 22 fix 1), decided
  // under the lock before anything is copied.
  const auto& m = manifest.parsed.manifest;
  const std::string ref = model_ref_text(m.name, m.tag);
  if (d.kind == InstallDecisionKind::update && !confirm_license_change(review_update(models, *d.installed, m, ui), m, ui)) {
    return err(AcquireReason::declined, ref);
  }
  if (!ui.confirm("Import " + ref + " into roots.models? [y/N]", yes)) return err(AcquireReason::declined, ref);
  if (auto adopted = adopt_orphan(models, manifest, loaded, env)) {
    if (auto* failure = std::get_if<AcquireError>(&*adopted)) return *failure;
    auto& result = std::get<PublishResult>(*adopted);
    return InstallOutcome{result.entry, false, true, result.still_retired};
  }
  auto space = env.free_bytes(models);
  if (auto* failure = std::get_if<AcquireError>(&space)) return *failure;
  if (std::get<std::uint64_t>(space) < total_bytes(manifest.parsed.manifest) + kFreeSpaceMarginBytes) {
    return err(AcquireReason::disk_space, "roots.models needs " + std::to_string(total_bytes(manifest.parsed.manifest)) + " bytes plus 1 GiB");
  }
  // F-47: a .staging/<k> left by an interrupted pull of this same manifest is discarded:
  // the bundle holds every file, and import copies each one exactly once.
  const auto key = staging_key(manifest.parsed.sha256);
  const auto staging = staging_dir_path(key);
  if (auto failure = remove_tree_within(models, staging)) return *failure;
  if (auto failure = ensure_directory(models, staging)) return *failure;
  if (auto failure = write_new_file(models, join_rel(staging, kStagingManifestName), manifest.parsed.bytes)) return *failure;
  for (const auto& f : manifest.parsed.manifest.files) {
    auto copied = copy_verified(bundle.root, files_path(f.path), models, join_rel(staging, f.path), f, env, progress,
                                AcquireReason::bundle_invalid);
    if (auto* failure = std::get_if<AcquireError>(&copied)) {
      (void)remove_tree_within(models, staging);
      return *failure;
    }
  }
  // Every file was hashed while copied in this run (§3.7), so publish does not re-hash.
  auto published = publish_staged(models, key, manifest, loaded, env, false);
  if (auto* failure = std::get_if<AcquireError>(&published)) return *failure;
  auto& result = std::get<PublishResult>(published);
  return InstallOutcome{result.entry, false, false, result.still_retired};
}

std::variant<BundleTarget, AcquireError> prepare_bundle_target(const std::filesystem::path& dir, const TrustedManifest& manifest,
                                                               const bool allow_resume, const StoreEnvironment& env) {
  const auto& m = manifest.parsed.manifest;
  // Effective per-file license (§5.3), before any byte is written.
  if (!bundle_allowed(m)) return err(AcquireReason::license_forbids_bundle, model_ref_text(m.name, m.tag));
  // Before anything is created: CreateDirectoryW on an existing drive root fails with
  // ERROR_ACCESS_DENIED, not ERROR_ALREADY_EXISTS (fix round 1, I2).
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(dir, ec);
  if (!ec && is_drive_root(absolute.lexically_normal())) return err(AcquireReason::bundle_target_not_empty, std::string{kVolumeRootDetail});
  if (create_target_directory(dir) == Created::failed && !std::filesystem::is_directory(dir, ec)) {
    return err(AcquireReason::bundle_target_not_empty, "the bundle directory cannot be created");
  }
  auto opened = open_root(dir, RootPolicy::external);
  if (std::holds_alternative<PathError>(opened)) return err(AcquireReason::bundle_target_not_empty, "the bundle directory cannot be opened");
  BundleTarget target{std::get<CanonicalRoot>(std::move(opened)), false};
  if (is_drive_root(target.root.path()) || is_mount_root(target.root)) {  // canonical: also catches a link to a root
    return err(AcquireReason::bundle_target_not_empty, std::string{kVolumeRootDetail});
  }
  auto listed = list_directory(target.root);
  if (std::holds_alternative<PathError>(listed)) return err(AcquireReason::bundle_target_not_empty, "the bundle directory cannot be listed");
  const auto& entries = std::get<std::vector<DirectoryEntry>>(listed);
  if (!entries.empty()) {
    bool resumable = allow_resume;
    for (const auto& e : entries) {
      resumable = resumable && (e.name == kBundleStagingDir || e.name == kBundleFilesDir || e.name == kBundleManifestName ||
                                e.name == kBundleMissingName);
    }
    if (resumable && exists_within(target.root, must_rel(kBundleManifestName))) {
      auto bytes = read_small_file(target.root, must_rel(kBundleManifestName), kManifestMaxBytes);
      resumable = std::holds_alternative<std::string>(bytes) && std::get<std::string>(bytes) == manifest.parsed.bytes;
    }
    if (resumable && exists_within(target.root, must_rel(kBundleStagingDir))) {
      auto keys = list_directory(target.root, must_rel(kBundleStagingDir));
      if (std::holds_alternative<PathError>(keys)) {
        resumable = false;
      } else {
        for (const auto& k : std::get<std::vector<DirectoryEntry>>(keys)) resumable = resumable && k.name == staging_key(manifest.parsed.sha256);
      }
    }
    if (!resumable) return err(AcquireReason::bundle_target_not_empty, "the bundle directory is not empty");
    target.resumed = true;
  }
  auto kind = env.file_system_kind(target.root);
  if (auto* failure = std::get_if<AcquireError>(&kind)) return *failure;
  if (std::get<FileSystemKind>(kind) == FileSystemKind::fat) {
    for (const auto& f : m.files) {
      if (f.size > env.fat_max_file_bytes()) return err(AcquireReason::bundle_target_fat32, f.path + " is 4 GiB or larger");
    }
  }
  auto space = env.free_bytes(target.root);
  if (auto* failure = std::get_if<AcquireError>(&space)) return *failure;
  if (std::get<std::uint64_t>(space) < total_bytes(m) + kFreeSpaceMarginBytes && !target.resumed) {
    return err(AcquireReason::disk_space, "the bundle volume needs " + std::to_string(total_bytes(m)) + " bytes plus 1 GiB");
  }
  return target;
}

std::optional<AcquireError> finish_bundle(const CanonicalRoot& target, const TrustedManifest& manifest) {
  const auto& m = manifest.parsed.manifest;
  if (!exists_within(target, must_rel(kBundleManifestName))) {
    if (auto failure = write_new_file(target, must_rel(kBundleManifestName), manifest.parsed.bytes)) return failure;
  }
  bool complete = true;
  for (const auto& f : m.files) complete = complete && exists_within(target, files_path(f.path));
  if (auto failure = remove_file_within(target, must_rel(kBundleMissingName))) return failure;
  if (!complete) return write_new_file(target, must_rel(kBundleMissingName), render_missing(m));
  if (auto failure = write_new_file(target, must_rel(kBundleSumsName), render_sha256sums(manifest.parsed.sha256, m))) return failure;
  if (auto failure = remove_tree_within(target, must_rel(kBundleStagingDir))) return failure;
  return write_new_file(target, must_rel(kBundleMarkerName), kBundleMarkerContent);  // written last (§5.1)
}

std::optional<AcquireError> export_model(const CanonicalRoot& models, const InstalledModel& model, const std::filesystem::path& target,
                                         const StoreEnvironment& env, ProgressSink& progress) {
  const TrustedManifest manifest{model.manifest, model.entry.origin};
  auto prepared = prepare_bundle_target(target, manifest, false, env);
  if (auto* failure = std::get_if<AcquireError>(&prepared)) return *failure;
  const auto& root = std::get<BundleTarget>(prepared).root;
  for (const auto& f : model.manifest.manifest.files) {
    auto copied = copy_verified(models, join_rel(model.version_dir, f.path), root, files_path(f.path), f, env, progress,
                                AcquireReason::digest_mismatch);
    if (auto* failure = std::get_if<AcquireError>(&copied)) return *failure;
  }
  return finish_bundle(root, manifest);
}

}  // namespace secure_frontend
