#include "secure_frontend/acquire_publish.hpp"

#include "secure_frontend/model_index_store.hpp"
#include "secure_frontend/model_index_write.hpp"
#include "secure_frontend/tokenizer_pack.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <utility>

namespace secure_frontend {
namespace {

constexpr std::array<std::string_view, 4> kPackRequired{"index.txt", "dense.bin", "native_experts.txt", "conversions.json"};
constexpr std::array<std::string_view, 4> kTokenizerRequired{"vocab.json", "merges.txt", "token_type.json", "tokenizer.json"};
// Gemma 4 plan 1 Task 4 (spec §6.2): an spm_bpe pack keeps its merges in merges.json.
constexpr std::array<std::string_view, 4> kGemma4TokenizerRequired{"vocab.json", "merges.json", "token_type.json",
                                                                   "tokenizer.json"};

std::string where(const AcquireManifest& m) { return model_ref_text(m.name, m.tag); }

// Gemma 4 plan 1 Task 4: launch.tokenizer_pack holds the four spm_bpe files and loads in that mode.
std::optional<AcquireError> check_gemma4_layout(const CanonicalRoot& root, const RelativePath& dir, const AcquireManifest& m) {
  if (!m.launch.tokenizer_pack) return AcquireError{AcquireReason::pack_layout_invalid, where(m) + ": launch.tokenizer_pack"};
  std::set<std::string> listed;
  for (const auto& f : m.files) listed.insert(f.path);
  const std::string tokenizer = *m.launch.tokenizer_pack;
  for (const auto name : kGemma4TokenizerRequired) {
    if (!listed.contains(tokenizer + "/" + std::string{name})) {
      return AcquireError{AcquireReason::pack_layout_invalid, tokenizer + "/" + std::string{name}};
    }
  }
  // As below: every file is under `dir` and validate_manifest bounds the stored form.
  auto opened = open_root(root.join(join_rel(dir, tokenizer)));
  if (std::holds_alternative<PathError>(opened)) return AcquireError{AcquireReason::pack_layout_invalid, tokenizer};
  if (!std::holds_alternative<TokenizerPack>(load_local_tokenizer_pack(std::get<CanonicalRoot>(opened), TokenizerMode::spm_bpe))) {
    return AcquireError{AcquireReason::pack_layout_invalid, tokenizer + ": tokenizer pack does not load"};
  }
  return std::nullopt;
}

std::optional<AcquireError> check_layout(const CanonicalRoot& root, const RelativePath& dir, const AcquireManifest& m) {
  switch (m.profile) {
    case ManifestProfile::qwen: break;
    case ManifestProfile::gemma4: return check_gemma4_layout(root, dir, m);
    // glm53: shard-set rule only, until SP6. deepseek/raon1 (unified-engine): shard-set rule only
    // too — their layout check arrives with the merged engine (plan 4).
    case ManifestProfile::glm53:
    case ManifestProfile::deepseek:
    case ManifestProfile::raon1:
    case ManifestProfile::raw:
    case ManifestProfile::mtp:
    case ManifestProfile::gguf: return std::nullopt;  // the shard-set rule only (validate_manifest)
  }
  if (!m.launch.pack || !m.launch.tokenizer_pack) return AcquireError{AcquireReason::pack_layout_invalid, where(m) + ": launch.pack"};
  std::set<std::string> listed;
  for (const auto& f : m.files) listed.insert(f.path);
  const std::string pack = *m.launch.pack;
  const std::string tokenizer = *m.launch.tokenizer_pack;
  for (const auto name : kPackRequired) {
    if (!listed.contains(pack + "/" + std::string{name})) return AcquireError{AcquireReason::pack_layout_invalid, pack + "/" + std::string{name}};
  }
  for (const auto name : kTokenizerRequired) {
    if (!listed.contains(tokenizer + "/" + std::string{name})) {
      return AcquireError{AcquireReason::pack_layout_invalid, tokenizer + "/" + std::string{name}};
    }
  }
  // Every file is under `dir` and validate_manifest bounds the stored form (Task 18 ruling), so this join is P1-valid.
  auto opened = open_root(root.join(join_rel(dir, tokenizer)));
  if (std::holds_alternative<PathError>(opened)) return AcquireError{AcquireReason::pack_layout_invalid, tokenizer};
  if (!std::holds_alternative<TokenizerPack>(load_local_tokenizer_pack(std::get<CanonicalRoot>(opened), TokenizerMode::byte_bpe))) {
    return AcquireError{AcquireReason::pack_layout_invalid, tokenizer + ": tokenizer pack does not load"};
  }
  return std::nullopt;
}

bool names_live(const ModelIndex& index, const RetiredEntry& retired) {
  const auto* live = find_model(index, retired.name, retired.tag);
  return live != nullptr && live->dir == retired.dir;
}

// §4.5 step 6. A retired row that names the live directory (only a hand-edited index can
// hold one; with_installed never writes it) is dropped and its directory never touched.
std::optional<AcquireError> retire_unheld(const CanonicalRoot& models, ModelIndex& index, LoadedVersions& loaded,
                                          std::vector<RetiredEntry>* still) {
  bool changed = false;
  for (const auto& retired : std::vector<RetiredEntry>(index.retired)) {
    if (!names_live(index, retired)) {
      if (loaded.is_held(retired.name, retired.tag, retired.dir)) {
        if (still) still->push_back(retired);
        continue;
      }
      // remove_tree_within restores owner write access on read-only directories (0500 / FRFX)
      // and reports a directory it cannot reach as a failure, never as absent.
      if (remove_tree_within(models, version_dir_path(retired.name, retired.tag, retired.dir))) {
        if (still) still->push_back(retired);  // e.g. Windows: the engine still has a file open
        continue;
      }
    }
    index = without_retired(std::move(index), retired.name, retired.tag, retired.dir);
    changed = true;
  }
  if (changed) return write_model_index(models, index);
  return std::nullopt;
}

// §4.5 step 1 (F-40): every directory of the staged tree, deepest first, then the key itself.
std::optional<AcquireError> sync_tree_directories(const CanonicalRoot& models, const RelativePath& dir, const AcquireManifest& m) {
  std::set<std::string> subdirs;
  for (const auto& f : m.files) {
    for (auto slash = f.path.rfind('/'); slash != std::string::npos && slash > 0; slash = f.path.rfind('/', slash - 1)) {
      subdirs.insert(f.path.substr(0, slash));
    }
  }
  for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) {
    if (auto failure = sync_directory(models, join_rel(dir, *it))) return AcquireError{AcquireReason::publish_failed, failure->detail};
  }
  if (auto failure = sync_directory(models, dir)) return AcquireError{AcquireReason::publish_failed, failure->detail};
  return std::nullopt;
}

PublishResult entry_result(const TrustedManifest& manifest, const std::string& dir, const bool adopted) {
  const auto& m = manifest.parsed.manifest;
  return PublishResult{IndexEntry{m.name, m.tag, m.version, m.sequence, dir, manifest.parsed.sha256, manifest.origin}, adopted, {}};
}

}  // namespace

RelativePath staging_dir_path(const std::string_view key) { return must_rel(std::string{kStagingDirName} + "/" + std::string{key}); }

std::optional<AcquireError> verify_tree(const CanonicalRoot& root, const RelativePath& dir, const TrustedManifest& manifest,
                                        const std::string_view manifest_file_name, const bool hash_files,
                                        const std::atomic<bool>* interrupted) {
  const auto& m = manifest.parsed.manifest;
  auto listed = list_files_recursive(root, dir);
  if (auto* failure = std::get_if<AcquireError>(&listed)) {
    // Task 16 carry: bundle_invalid is the bundle-reading reason; here the tree is ours.
    return AcquireError{AcquireReason::publish_failed, where(m) + ": " + failure->detail};
  }
  std::set<std::string> expected;
  for (const auto& f : m.files) expected.insert(f.path);
  expected.insert(std::string{manifest_file_name});
  std::set<std::string> found;
  for (const auto& entry : std::get<std::vector<StagedEntry>>(listed)) {
    if (!entry.regular || !expected.contains(entry.path)) {
      return AcquireError{AcquireReason::publish_failed, where(m) + ": unexpected entry " + entry.path};
    }
    found.insert(entry.path);
  }
  if (found != expected) return AcquireError{AcquireReason::publish_failed, where(m) + ": staged files incomplete"};
  auto bytes = read_small_file(root, join_rel(dir, manifest_file_name), kManifestMaxBytes);
  if (std::holds_alternative<AcquireError>(bytes) || std::get<std::string>(bytes) != manifest.parsed.bytes) {
    return AcquireError{AcquireReason::publish_failed, where(m) + ": staged manifest differs"};
  }
  if (hash_files) {
    for (const auto& f : m.files) {
      Sha256 hasher;
      auto counted = hash_into(root, join_rel(dir, f.path), hasher, f.size, interrupted);
      if (const auto* failure = std::get_if<AcquireError>(&counted); failure != nullptr && failure->reason == AcquireReason::interrupted) {
        return *failure;
      }
      if (std::holds_alternative<AcquireError>(counted) || std::get<std::uint64_t>(counted) != f.size ||
          to_hex(hasher.finish()) != f.sha256) {
        return AcquireError{AcquireReason::digest_mismatch, f.path};
      }
    }
  }
  return check_layout(root, dir, m);
}

std::variant<InstallDecision, AcquireError> decide_install(const ModelIndex& index, const TrustedManifest& manifest,
                                                           const bool allow_downgrade) {
  const auto& m = manifest.parsed.manifest;
  const auto* installed = find_model(index, m.name, m.tag);
  if (installed == nullptr) return InstallDecision{InstallDecisionKind::fresh, nullptr};
  if (installed->manifest_sha256 == manifest.parsed.sha256) return InstallDecision{InstallDecisionKind::already_installed, installed};
  if (m.sequence == installed->sequence) return AcquireError{AcquireReason::manifest_conflict, where(m)};
  if (m.sequence < installed->sequence && !allow_downgrade) {
    return AcquireError{AcquireReason::manifest_downgrade,
                        where(m) + " sequence " + std::to_string(m.sequence) + " < installed " + std::to_string(installed->sequence)};
  }
  return InstallDecision{InstallDecisionKind::update, installed};
}

std::variant<PublishResult, AcquireError> publish_staged(const CanonicalRoot& models, const std::string_view key,
                                                         const TrustedManifest& manifest, LoadedVersions& loaded,
                                                         const StoreEnvironment& env, const bool hash_files,
                                                         const PublishOptions& options) {
  const auto& m = manifest.parsed.manifest;
  // §4.4 item 2 (F-39): MANIFEST must hash to <k>; verify_tree checks it holds the manifest's bytes.
  if (key != staging_key(manifest.parsed.sha256)) {
    return AcquireError{AcquireReason::publish_failed, where(m) + ": staging key does not match the manifest digest"};
  }
  const auto staging = staging_dir_path(key);
  if (auto failure = verify_tree(models, staging, manifest, kStagingManifestName, hash_files)) return *failure;
  // Step 1: files were fsynced when they were completed; fsync every directory (F-40).
  if (auto failure = sync_tree_directories(models, staging, m)) return *failure;
  // Step 2: MANIFEST -> manifest.json (same directory, no replace), then fsync.
  if (auto failure = rename_within(models, join_rel(staging, kStagingManifestName), join_rel(staging, kVersionManifestName), env)) {
    return *failure;
  }
  if (auto failure = sync_directory(models, staging)) return AcquireError{AcquireReason::publish_failed, failure->detail};
  // Steps 3-4.
  const std::string dir = version_directory_name(m.sequence, manifest.parsed.sha256);
  const auto parent = must_rel(m.name + "/" + m.tag);
  if (auto failure = ensure_directory(models, parent)) return *failure;
#if defined(SECURE_FRONTEND_TEST_HOOKS)
  if (env.fail_version_rename()) return AcquireError{AcquireReason::publish_failed, "fault injected at publish step 4"};
#endif
  if (auto failure = rename_within(models, staging, version_dir_path(m.name, m.tag, dir), env)) return *failure;
  // A failure from here to step 5 leaves an orphan that adopt_orphan recovers (§4.5).
  if (auto failure = sync_directory(models, parent)) return AcquireError{AcquireReason::publish_failed, failure->detail};
  if (auto failure = sync_directory(models, must_rel(kStagingDirName))) return AcquireError{AcquireReason::publish_failed, failure->detail};
#if defined(SECURE_FRONTEND_TEST_HOOKS)
  if (options.stop_after_version_rename && options.stop_after_version_rename()) {
    return AcquireError{AcquireReason::publish_failed, "fault injected between publish steps 4 and 5"};
  }
#else
  (void)options;
#endif
  // Step 5.
  auto read = read_model_index(models);
  if (auto* failure = std::get_if<AcquireError>(&read)) return *failure;
  PublishResult result = entry_result(manifest, dir, false);
  auto index = with_installed(std::get<ModelIndex>(std::move(read)), result.entry);
  if (auto failure = write_model_index(models, index)) return *failure;
  // Step 6.
  if (auto failure = retire_unheld(models, index, loaded, &result.still_retired)) return *failure;
  return result;
}

std::optional<std::variant<PublishResult, AcquireError>> adopt_orphan(const CanonicalRoot& models, const TrustedManifest& manifest,
                                                                      LoadedVersions& loaded, const StoreEnvironment& env,
                                                                      const std::atomic<bool>* interrupted) {
  (void)env;
  using Outcome = std::variant<PublishResult, AcquireError>;
  const auto& m = manifest.parsed.manifest;
  const std::string dir = version_directory_name(m.sequence, manifest.parsed.sha256);
  const auto path = version_dir_path(m.name, m.tag, dir);
  const auto manifest_path = join_rel(path, kVersionManifestName);
  if (!exists_within(models, manifest_path)) return std::nullopt;
  auto bytes = read_small_file(models, manifest_path, kManifestMaxBytes);
  if (std::holds_alternative<AcquireError>(bytes) || to_hex(sha256(std::get<std::string>(bytes))) != manifest.parsed.sha256) {
    return std::nullopt;  // compared by digest, not by directory name
  }
  auto read = read_model_index(models);
  if (auto* failure = std::get_if<AcquireError>(&read)) return Outcome{*failure};
  auto index = std::get<ModelIndex>(std::move(read));
  if (const auto* current = find_model(index, m.name, m.tag); current != nullptr && current->dir == dir) return std::nullopt;
  // Final review I2: a tree that does not verify is not adoptable; publish then reports
  // "target exists" (the orphan hint), never a verb failure with digest_mismatch.
  if (auto failure = verify_tree(models, path, manifest, kVersionManifestName, true, interrupted)) {
    if (failure->reason == AcquireReason::interrupted) return Outcome{*failure};
    return std::nullopt;
  }
  PublishResult result = entry_result(manifest, dir, true);
  // with_installed drops a retired row naming `dir` (a downgrade to a retired version): one live directory.
  index = with_installed(std::move(index), result.entry);
  if (auto failure = write_model_index(models, index)) return Outcome{*failure};
  if (auto failure = retire_unheld(models, index, loaded, &result.still_retired)) return Outcome{*failure};
  return Outcome{std::move(result)};
}

std::optional<AcquireError> delete_unheld_retired(const CanonicalRoot& models, LoadedVersions& loaded, const StoreEnvironment& env) {
  (void)env;
  auto read = read_model_index(models);
  if (auto* failure = std::get_if<AcquireError>(&read)) return *failure;
  auto index = std::get<ModelIndex>(std::move(read));
  return retire_unheld(models, index, loaded, nullptr);
}

std::variant<IndexEntry, AcquireError> remove_installed(const CanonicalRoot& models, const std::string_view name,
                                                        const std::string_view tag, LoadedVersions& loaded, const bool force,
                                                        const StoreEnvironment& env) {
  (void)env;
  auto read = read_model_index(models);
  if (auto* failure = std::get_if<AcquireError>(&read)) return *failure;
  auto index = std::get<ModelIndex>(std::move(read));
  const auto* entry = find_model(index, name, tag);
  if (entry == nullptr) return AcquireError{AcquireReason::model_unknown, model_ref_text(name, tag)};
  const IndexEntry removed = *entry;
  if (loaded.is_held(removed.name, removed.tag, removed.dir) && !force) {
    return AcquireError{AcquireReason::models_busy, model_ref_text(name, tag) + " is loaded by the running server; stop it first"};
  }
  // Controller note 5 / final review I2: the entry moves to `retired` before anything is deleted,
  // so a delete that fails part-way (Windows: the engine has a file open) leaves a retired
  // version that the next pull/import/rm --retired removes, never an unmanaged orphan.
  index = without_model(std::move(index), name, tag);
  const bool listed = std::any_of(index.retired.begin(), index.retired.end(), [&](const RetiredEntry& r) {
    return r.name == removed.name && r.tag == removed.tag && r.dir == removed.dir;
  });
  if (!listed) index.retired.push_back(RetiredEntry{removed.name, removed.tag, removed.dir, removed.manifest_sha256});
  if (auto failure = write_model_index(models, index)) return *failure;
  // Restores owner write access on read-only directories; an unreachable directory is an error, not "absent".
  if (auto failure = remove_tree_within(models, version_dir_path(removed.name, removed.tag, removed.dir))) return *failure;
  // Deleted: drop the retired row, as retire_unheld does (§4.5 step 6).
  index = without_retired(std::move(index), removed.name, removed.tag, removed.dir);
  if (auto failure = write_model_index(models, index)) return *failure;
  return removed;
}

}  // namespace secure_frontend
