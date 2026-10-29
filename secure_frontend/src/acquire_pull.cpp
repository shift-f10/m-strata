#include "secure_frontend/acquire_pull.hpp"

#include "secure_frontend/bundle_io.hpp"
#include "secure_frontend/bundle_text.hpp"
#include "secure_frontend/catalog.hpp"
#include "secure_frontend/manifest_diff.hpp"
#include "secure_frontend/model_index_store.hpp"
#include "secure_frontend/staging_plan.hpp"

#include <cstdio>
#include <fstream>
#include <set>
#include <utility>
#include <variant>
#include <vector>

namespace secure_frontend {
namespace {

PullOutcome failed(AcquireError error) {
  const auto exit = exit_class(error.reason);
  return PullOutcome{exit, std::move(error), std::nullopt, false, false};
}

std::variant<TrustedManifest, AcquireError> resolve_request(const PullRequest& request, const ManifestRules& rules) {
  if (request.manifest_file) {
    std::ifstream input(*request.manifest_file, std::ios::binary);
    if (!input) return AcquireError{AcquireReason::usage_error, "--manifest file cannot be read"};
    std::string bytes(kManifestMaxBytes + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    if (bytes.size() > kManifestMaxBytes) return AcquireError{AcquireReason::manifest_invalid, "manifest: is larger than 1 MiB"};
    return trust_manifest_bytes(bytes, request.trust_digest, rules);
  }
  const auto reference = request.reference ? parse_model_reference(*request.reference) : std::nullopt;
  if (!reference) return AcquireError{AcquireReason::usage_error, "expected <name[:tag]>"};
  return resolve_catalog_manifest(*reference, rules);
}

std::string runnable_text(const AcquireManifest& m) {
  switch (m.profile) {
    case ManifestProfile::raw: return "no (test profile raw)";
    case ManifestProfile::gguf: return "no (GGUF files to prepare on this machine)";
    case ManifestProfile::mtp: return "no (an MTP draft layer's tensors, built on this machine)";
    case ManifestProfile::glm53: return "no (needs the glm engine, SP6)";
    case ManifestProfile::gemma4: return "no (engine not available: the Gemma 4 engine is not installed)";
    case ManifestProfile::deepseek:
    case ManifestProfile::raon1: return "no (engine not available: the unified engine is not installed)";
    case ManifestProfile::qwen: break;
  }
  if (compare_versions(m.min_version, built_engine_version()) > 0) {
    return "no (needs engine strata >= " + render_engine_version(m.min_version) + ")";
  }
  if (has_sourceless_files(m)) return "yes, once the files without a source are added from a bundle";
  return "yes";
}

std::string hosts_text(const AcquireManifest& m) {
  std::string text;
  for (const auto& host : m.hosts) text += (text.empty() ? "" : ", ") + host;
  return text;
}

// §1.3 step 2: everything the operator sees before any connection.
void print_summary(Confirmer& ui, const TrustedManifest& tm, const std::uint64_t free_bytes, const std::uint64_t to_fetch,
                   const std::string& confinement, const std::string_view volume) {
  const auto& m = tm.parsed.manifest;
  ui.say("model: " + model_ref_text(m.name, m.tag) + " version " + m.version + " (sequence " + std::to_string(m.sequence) + ", " +
         std::string{origin_name(tm.origin)} + ")");
  ui.say("manifest sha256: " + tm.parsed.sha256);
  ui.say("license: " + m.license.id + " " + m.license.url + (m.license.bundle_allowed ? " (bundling allowed)" : " (bundling not allowed)"));
  if (!m.license.notice.empty()) ui.say("notice: " + m.license.notice);
  ui.say("hosts: " + hosts_text(m));
  ui.say("files:");
  for (const auto& f : m.files) {
    ui.say("  " + std::to_string(f.size) + "  " + f.sha256 + "  " + f.path + (f.sources.empty() ? "  (bundle only)" : ""));
  }
  ui.say("total: " + format_gb(total_bytes(m)) + " (" + std::to_string(total_bytes(m)) + " bytes); to download: " + format_gb(to_fetch));
  ui.say("free space on " + std::string{volume} + ": " + format_gb(free_bytes));
  ui.say("The source host sees your IP address and which files you download.");
  ui.say("runnable with this release: " + runnable_text(m));
  ui.say("confinement: " + confinement);
}

std::string resume_hint(const PullRequest& request, const AcquireManifest& m) {
  const std::string bundle = request.bundle_dir ? " --bundle" : "";
  if (request.manifest_file) return "run the same strata pull --manifest" + bundle + " command again to resume";
  if (request.bundle_dir) return "run the same strata pull " + model_ref_text(m.name, m.tag) + " --bundle command again to resume";
  return "run 'strata pull " + model_ref_text(m.name, m.tag) + "' again to resume";
}

// After staging was touched: the resume hint, and for pull --bundle what was left behind.
PullOutcome fail_with_hint(const PullRequest& request, const AcquireManifest& m, PullServices& s, AcquireError error) {
  if (reason_info(error.reason).resumable) {
    s.ui.say(resume_hint(request, m));
  } else if (request.bundle_dir) {
    s.ui.say("the partial bundle directory was left in place; delete it, or fix the cause and run the same pull --bundle again");
  }
  return failed(std::move(error));
}

std::uint64_t tree_bytes(const CanonicalRoot& root, const RelativePath& dir) {
  std::uint64_t total = 0;
  if (auto listed = list_files_recursive(root, dir); std::holds_alternative<std::vector<StagedEntry>>(listed)) {
    for (const auto& e : std::get<std::vector<StagedEntry>>(listed)) total += e.size;
  }
  return total;
}

// A publish that failed after §4.5 step 2 leaves manifest.json, not MANIFEST, in the key: the
// staged manifest's name is MANIFEST when present, else manifest.json (final review I1).
std::string_view staged_manifest_name(const CanonicalRoot& root, const RelativePath& dir) {
  return exists_within(root, join_rel(dir, kStagingManifestName)) ? kStagingManifestName : kVersionManifestName;
}

// The parsed manifest of a staging key whose staged manifest hashes to the key; else nullopt (stale).
std::optional<AcquireManifest> staged_manifest(const CanonicalRoot& root, const RelativePath& dir, const std::string_view key) {
  auto bytes = read_small_file(root, join_rel(dir, staged_manifest_name(root, dir)), kManifestMaxBytes);
  if (!std::holds_alternative<std::string>(bytes)) return std::nullopt;
  const auto& text = std::get<std::string>(bytes);
  if (staging_key(to_hex(sha256(text))) != key) return std::nullopt;
  ManifestRules any;
  any.max_total_bytes = ~std::uint64_t{0};
  auto parsed = parse_manifest(text, any);
  if (!std::holds_alternative<ParsedManifest>(parsed)) return std::nullopt;
  return std::get<ParsedManifest>(std::move(parsed)).manifest;
}

// §3.7, after the [y/N] answer (F-56): delete keys whose MANIFEST does not hash to the
// key; offer to delete other keys of the same name:tag (never answered by --yes).
std::optional<AcquireError> tidy_staging(const CanonicalRoot& root, const RelativePath& staging_root, const TrustedManifest& tm,
                                         PullServices& s) {
  const auto& m = tm.parsed.manifest;
  const std::string own = staging_key(tm.parsed.sha256);
  auto listed = list_directory(root, staging_root);
  if (std::holds_alternative<PathError>(listed)) return std::nullopt;
  std::vector<StagingKeyInfo> infos;
  for (const auto& entry : std::get<std::vector<DirectoryEntry>>(listed)) {
    if (entry.type != EntryType::directory || !is_staging_key_name(entry.name)) continue;  // .cwd is never a key
    const auto dir = join_rel(staging_root, entry.name);
    const auto other = staged_manifest(root, dir, entry.name);
    if (!other) {
      if (auto failure = remove_tree_within(root, dir)) return failure;
      s.ui.say("stale_staging_discarded: " + entry.name);
      continue;
    }
    if (entry.name == own) continue;
    infos.push_back(StagingKeyInfo{entry.name, other->name, other->tag, tree_bytes(root, dir)});
  }
  const auto abandoned = abandoned_keys(infos, own, m.name, m.tag);
  if (abandoned.empty()) return std::nullopt;
  std::uint64_t bytes = 0;
  for (const auto& key : abandoned) bytes += key.bytes;
  const std::string ref = model_ref_text(m.name, m.tag);
  if (s.ui.confirm("Delete " + format_gb(bytes) + " of partial downloads for other versions of " + ref + "? [y/N]", false)) {
    for (const auto& key : abandoned) {
      if (auto failure = remove_tree_within(root, join_rel(staging_root, key.key))) return failure;
    }
  } else {
    s.ui.say("kept " + format_gb(bytes) + " of partial downloads; delete them later with: strata rm --partials " + ref);
  }
  return std::nullopt;
}

// §3.2: the downloader is validated once, after confirmation, before the first transfer.
class ProbeOnce final {
 public:
  explicit ProbeOnce(DownloaderProbe& probe) : probe_(probe) {}
  [[nodiscard]] std::optional<AcquireError> ensure() {
    if (!result_) result_ = probe_.probe();
    return *result_;
  }

 private:
  DownloaderProbe& probe_;
  std::optional<std::optional<AcquireError>> result_;
};

// Final fix F1 (§8.3): an interrupt inside a whole-file hash or copy is reported as-is, never
// mistaken for a file that does not verify (which would discard or re-fetch it).
const AcquireError* interrupt_of(const std::variant<std::uint64_t, AcquireError>& result) {
  const auto* failure = std::get_if<AcquireError>(&result);
  return failure != nullptr && failure->reason == AcquireReason::interrupted ? failure : nullptr;
}

struct Work final {
  std::size_t file_index;
  FileStep step;
  std::uint64_t existing;
  bool reuse;
};

std::optional<AcquireError> ensure_parent_of(const CanonicalRoot& root, const RelativePath& path) {
  const auto slash = path.generic().rfind('/');
  if (slash == std::string::npos) return std::nullopt;
  return ensure_directory(root, must_rel(path.generic().substr(0, slash)));
}

// F-1: PartFile cannot be move-assigned, so the variant is built once: a fresh .part
// replaces any leftover; an existing one is reopened at its length.
std::variant<PartFile, AcquireError> open_part(const CanonicalRoot& root, const RelativePath& part, const bool resume,
                                               const StoreEnvironment& env) {
  if (resume) return open_part_file(root, part, PartOpen::open_existing, env);
  if (auto failure = remove_file_within(root, part)) return *failure;
  return open_part_file(root, part, PartOpen::create_new, env);
}

// Brings one file of `m` to <staging>/<path>, verified in this run: re-hash, resume, reuse or fetch.
std::optional<AcquireError> acquire_file(const CanonicalRoot& root, const RelativePath& staging, const TrustedManifest& tm,
                                         const Work& work, const std::optional<InstalledModel>& installed, const CanonicalRoot& models,
                                         FetchContext& context, ProbeOnce& probe, PullServices& s) {
  const auto& f = tm.parsed.manifest.files[work.file_index];
  const auto final_path = join_rel(staging, f.path);
  const auto part = must_rel(final_path.generic() + std::string{kPartSuffix});
  if (auto failure = ensure_parent_of(root, final_path)) return failure;
  FileStep step = work.step;
  if (step == FileStep::rehash_verified_candidate) {
    // §3.7: a suffix-less file from an earlier run is re-hashed before it is trusted.
    Sha256 hasher;
    auto counted = hash_into(root, final_path, hasher, f.size, &s.interrupted);
    if (const auto* failure = interrupt_of(counted)) return *failure;  // the file stays for the next run
    if (std::holds_alternative<std::uint64_t>(counted) && std::get<std::uint64_t>(counted) == f.size && to_hex(hasher.finish()) == f.sha256) {
      return std::nullopt;
    }
    if (auto failure = remove_file_within(root, final_path)) return failure;
    step = FileStep::fetch_from_zero;
  }
  Sha256 hasher;
  std::uint64_t existing = 0;
  if (step == FileStep::rehash_then_resume || step == FileStep::rehash_complete_part) {
    // Re-hash the .part's bytes up to its LENGTH (never its preallocation) before the writer opens it.
    auto counted = hash_into(root, part, hasher, f.size, &s.interrupted);
    if (const auto* failure = interrupt_of(counted)) return *failure;  // never restart a resumable .part from zero
    existing = std::holds_alternative<std::uint64_t>(counted) ? std::get<std::uint64_t>(counted) : 0;
    if (existing == 0) hasher = Sha256{};
    if (existing == f.size) {
      if (to_hex(Sha256{hasher}.finish()) == f.sha256) return rename_within(root, part, final_path, s.store);
      hasher = Sha256{};
      existing = 0;  // a complete .part that does not verify is fetched again from byte 0
    }
  }
  if (existing == 0 && work.reuse && installed) {
    auto copied = copy_verified(models, join_rel(installed->version_dir, f.path), root, final_path, f, s.store, s.progress,
                                AcquireReason::digest_mismatch, &s.interrupted);
    if (const auto* failure = interrupt_of(copied)) return *failure;  // the half copy was removed; nothing is fetched
    if (std::holds_alternative<std::uint64_t>(copied)) return std::nullopt;  // §6.1: unchanged files are copied, not downloaded
  }
  if (f.sources.empty()) return AcquireError{AcquireReason::manifest_requires_bundle, f.path};
  if (auto failure = probe.ensure()) return failure;
  auto opened = open_part(root, part, existing > 0, s.store);
  if (auto* failure = std::get_if<AcquireError>(&opened)) return *failure;
  auto& file = std::get<PartFile>(opened);
  if (file.size() != existing) {  // the .part changed between hashing and opening: start over
    if (auto failure = file.truncate_to_zero()) return failure;
    hasher = Sha256{};
    existing = 0;
  }
  // §4.6: KEEP_SIZE / FileAllocationInfo; size() (the resume offset) is unchanged.
  if (auto failure = file.preallocate(f.size)) return failure;
  FetchTarget target{part, final_path, file, hasher, existing > 0};
  return fetch_file(context, tm.parsed.manifest, work.file_index, target);
}

bool is_space(const char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

char lower(const char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

UpdateReview review_update(const CanonicalRoot& models, const IndexEntry& installed, const AcquireManifest& candidate, Confirmer& ui) {
  UpdateReview review;
  auto loaded_model = load_installed(models, installed);
  if (auto* model = std::get_if<InstalledModel>(&loaded_model)) {
    review.installed.emplace(std::move(*model));
    review.diff = diff_manifests(review.installed->manifest.manifest, candidate);
    // §6.1 on the console only: the lines carry license URLs, never written to acquire.log (§8.1).
    for (const auto& line : review.diff->lines) ui.say(line);
  } else {
    // Fail closed: the installed license is UNKNOWN, so §6.1's license question is asked
    // (never answered by --yes). Fixed text plus the reason name only (§8.1).
    ui.say("the installed version of " + model_ref_text(candidate.name, candidate.tag) + " cannot be read (" +
           std::string{reason_name(std::get<AcquireError>(loaded_model).reason)} + "); its license is unknown");
  }
  return review;
}

bool confirm_license_change(const UpdateReview& review, const AcquireManifest& candidate, Confirmer& ui) {
  const std::string ref = model_ref_text(candidate.name, candidate.tag);
  if (!review.installed) {
    return ui.confirm("The license of the installed version of " + ref + " is unknown; install " + candidate.license.id + "? [y/N]", false);
  }
  if (!review.diff || !review.diff->license_changed) return true;
  return ui.confirm("The license of " + ref + " changes (" + review.installed->manifest.manifest.license.id + " -> " +
                        candidate.license.id + "). Continue? [y/N]",
                    false);
}

bool parse_confirmation(std::string_view answer) noexcept {
  while (!answer.empty() && is_space(answer.front())) answer.remove_prefix(1);
  while (!answer.empty() && is_space(answer.back())) answer.remove_suffix(1);
  const auto equals = [answer](const std::string_view word) noexcept {
    if (answer.size() != word.size()) return false;
    for (std::size_t i = 0; i < word.size(); ++i) {
      if (lower(answer[i]) != word[i]) return false;
    }
    return true;
  };
  return equals("y") || equals("yes");
}

std::string format_gb(const std::uint64_t bytes) {
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%.1f GB", static_cast<double>(bytes) / 1e9);
  return buffer;
}

FetchSettings fetch_settings_from(const PullSettings& settings) {
  FetchSettings fetch;
  fetch.proxy = settings.proxy;
  fetch.allow_private_addresses = settings.allow_private_addresses;
  fetch.connect_timeout_s = static_cast<std::uint32_t>(settings.connect_timeout.count());
  fetch.stall_timeout_s = static_cast<std::uint32_t>(settings.stall_timeout.count());
  fetch.connections = settings.connections;
  return fetch;
}

PullOutcome run_pull(const PullRequest& request, const CanonicalRoot& models, const PullSettings& settings, PullServices& s) {
  ManifestRules rules;
  rules.max_total_bytes = settings.max_total_bytes;  // total_budget_exceeded (exit 6) before anything is written
  auto resolved = resolve_request(request, rules);
  if (auto* failure = std::get_if<AcquireError>(&resolved)) return failed(*failure);
  const auto tm = std::get<TrustedManifest>(std::move(resolved));
  const auto& m = tm.parsed.manifest;
  const std::string ref = model_ref_text(m.name, m.tag);
  s.progress.manifest_selected(ref, tm.parsed.sha256);
  const bool to_bundle = request.bundle_dir.has_value();
  if (!to_bundle && has_sourceless_files(m)) {
    s.ui.say("this manifest pins files that have no download source (the Qwen pack, D5); install it from a bundle:");
    s.ui.say("  1. strata pull " + ref + " --bundle <dir>   (online machine; fetches every file that has a source)");
    s.ui.say("  2. python3 -I tools/secure/make_bundle.py ...   (developer machine; builds the missing pack files)");
    s.ui.say("  3. strata import <dir>");
    return failed(AcquireError{AcquireReason::manifest_requires_bundle, ref});
  }

  // Destination: roots.models (under .lock for the whole run, §4.8), or the bundle directory.
  std::optional<ModelsLock> lock;
  std::optional<BundleTarget> bundle;
  std::optional<UpdateReview> review;  // an update: the installed version (if readable) and the diff
  if (to_bundle) {
    auto prepared = prepare_bundle_target(*request.bundle_dir, tm, true, s.store);
    if (auto* failure = std::get_if<AcquireError>(&prepared)) return failed(*failure);
    bundle.emplace(std::get<BundleTarget>(std::move(prepared)));
  } else {
    auto locked = lock_models_root(models);
    if (auto* failure = std::get_if<AcquireError>(&locked)) return failed(*failure);
    lock.emplace(std::get<ModelsLock>(std::move(locked)));
    auto read = read_model_index(models);
    if (auto* failure = std::get_if<AcquireError>(&read)) return failed(*failure);
    // Task 19 carry: InstallDecision::installed points into this index; the entry is copied
    // out while `index` is alive and the pointer is not used afterwards.
    const ModelIndex index = std::get<ModelIndex>(std::move(read));
    auto decision = decide_install(index, tm, request.allow_downgrade);
    if (auto* failure = std::get_if<AcquireError>(&decision)) return failed(*failure);
    const auto& d = std::get<InstallDecision>(decision);
    const std::optional<IndexEntry> current = d.installed ? std::optional<IndexEntry>{*d.installed} : std::nullopt;
    if (d.kind == InstallDecisionKind::already_installed) {
      if (!request.verify) {
        s.ui.say("already installed: " + ref);
        return PullOutcome{ExitCode::success, std::nullopt, current, true, false};
      }
      auto loaded_model = load_installed(models, *current);
      if (auto* failure = std::get_if<AcquireError>(&loaded_model)) return failed(*failure);
      if (auto failure = verify_tree(models, std::get<InstalledModel>(loaded_model).version_dir, tm, kVersionManifestName, true,
                                     &s.interrupted)) {
        return failed(*failure);
      }
      s.ui.say("verified: " + ref);
      return PullOutcome{ExitCode::success, std::nullopt, current, true, false};
    }
    if (auto adopted = adopt_orphan(models, tm, s.loaded, s.store, &s.interrupted)) {
      if (auto* failure = std::get_if<AcquireError>(&*adopted)) return failed(*failure);
      s.ui.say("adopted an earlier, interrupted publish of " + ref);
      return PullOutcome{ExitCode::success, std::nullopt, std::get<PublishResult>(*adopted).entry, false, false};
    }
    if (d.kind == InstallDecisionKind::update) review = review_update(models, *current, m, s.ui);
  }
  const CanonicalRoot& root = bundle ? bundle->root : models;

  // Plan (§3.7), reading only: nothing is written or deleted before the [y/N] answer (F-56).
  const auto staging_root = must_rel(std::string{kStagingDirName});
  const std::string key = staging_key(tm.parsed.sha256);
  const auto staging = staging_dir_path(key);
  std::vector<StagedEntry> staged;
  // A stale own key (MANIFEST that is not this manifest) is deleted by tidy_staging; plan as empty.
  if (!request.discard_partial && exists_within(root, staging) && staged_manifest(root, staging, key)) {
    auto listed = list_files_recursive(root, staging);
    if (auto* failure = std::get_if<AcquireError>(&listed)) return failed(*failure);
    staged = std::get<std::vector<StagedEntry>>(std::move(listed));
  }
  const auto plan = plan_staging(m, staged);
  std::set<std::size_t> reusable;
  if (review && review->diff) reusable.insert(review->diff->reusable_files.begin(), review->diff->reusable_files.end());
  std::vector<Work> work;
  std::vector<RelativePath> stale_bundle_files;  // files/<path> from an earlier run that no longer verify
  std::uint64_t to_fetch = 0;
  std::uint64_t needed = 0;
  for (const auto& p : plan.files) {
    const auto& f = m.files[p.file_index];
    if (bundle && f.sources.empty()) continue;  // §2.7: listed in MISSING
    if (bundle) {
      const auto done = must_rel(std::string{kBundleFilesDir} + "/" + f.path);
      if (exists_within(root, done)) {
        // §3.7: a run boundary is a trust boundary, also for files/ of an earlier pull --bundle run.
        Sha256 hasher;
        auto counted = hash_into(root, done, hasher, f.size, &s.interrupted);
        if (const auto* failure = interrupt_of(counted)) return failed(*failure);  // before the [y/N]: nothing was touched
        if (std::holds_alternative<std::uint64_t>(counted) && std::get<std::uint64_t>(counted) == f.size &&
            to_hex(hasher.finish()) == f.sha256) {
          continue;
        }
        stale_bundle_files.push_back(done);
      }
    }
    const bool reuse = reusable.contains(p.file_index) && p.step == FileStep::fetch_from_zero;
    if (!reuse) {
      if (p.step == FileStep::fetch_from_zero || p.step == FileStep::rehash_then_resume) to_fetch += f.size - p.existing_bytes;
      needed += f.size - p.existing_bytes;
    }
    work.push_back(Work{p.file_index, p.step, p.existing_bytes, reuse});
  }

  // Preflight (§4.6), summary, confirmation (§1.3).
  auto space = s.store.free_bytes(root);
  if (auto* failure = std::get_if<AcquireError>(&space)) return failed(*failure);
  const std::uint64_t free_bytes = std::get<std::uint64_t>(space);
  if (free_bytes < needed + kFreeSpaceMarginBytes) {
    return failed(AcquireError{AcquireReason::disk_space, "need " + format_gb(needed) + " plus 1 GiB, have " + format_gb(free_bytes)});
  }
  print_summary(s.ui, tm, free_bytes, to_fetch, s.probe.confinement(), bundle ? "the bundle volume" : "roots.models");
  if (review && !confirm_license_change(*review, m, s.ui)) return failed(AcquireError{AcquireReason::declined, ref});
  if (!work.empty() && !s.ui.confirm("Download " + format_gb(to_fetch) + " from " + hosts_text(m) + "? [y/N]", request.yes)) {
    return failed(AcquireError{AcquireReason::declined, ref});
  }

  // Staging hygiene, now that the operator said yes.
  if (auto failure = ensure_directory(root, staging_root)) return failed(*failure);
  if (!bundle) {
    if (auto failure = make_read_only_directory(root, must_rel(std::string{kStagingDirName} + "/" + std::string{kStagingCwdName}))) {
      return failed(*failure);
    }
  }
  if (request.discard_partial) {
    if (auto failure = remove_tree_within(root, staging)) return failed(*failure);
  }
  if (auto failure = tidy_staging(root, staging_root, tm, s)) return failed(*failure);
  // Final review I1: the key survived tidy_staging, so its manifest.json hashes to the key; undo
  // step 2 (same directory, no replace) before the plan removes names that are not the manifest's.
  if (exists_within(root, staging) && staged_manifest_name(root, staging) == kVersionManifestName &&
      exists_within(root, join_rel(staging, kVersionManifestName))) {
    if (auto failure = rename_within(root, join_rel(staging, kVersionManifestName), join_rel(staging, kStagingManifestName), s.store)) {
      return failed(*failure);
    }
  }
  if (!request.discard_partial) {
    for (const auto& path : plan.remove) {
      if (auto failure = remove_file_within(root, join_rel(staging, path))) return failed(*failure);
    }
  }
  for (const auto& path : stale_bundle_files) {
    if (auto failure = remove_file_within(root, path)) return failed(*failure);
  }

  // Acquire every file.
  if (auto failure = ensure_directory(root, staging)) return failed(*failure);
  if (!exists_within(root, join_rel(staging, kStagingManifestName))) {
    if (auto failure = write_new_file(root, join_rel(staging, kStagingManifestName), tm.parsed.bytes)) return failed(*failure);
  }
  const RunBudget budget(s.clock.now(), settings.max_duration);  // time_budget_exceeded (exit 3, resumable)
  FetchSettings fetch = fetch_settings_from(settings);
#if defined(SECURE_FRONTEND_TEST_HOOKS)
  fetch.test_cacert = request.test_cacert;  // F-51
#endif
  FetchContext context{s.transport, s.resolver, s.clock, budget, fetch, s.interrupted, s.progress, s.store, root};
  ProbeOnce probe(s.probe);
  const std::optional<InstalledModel> none;
  const auto& installed = review ? review->installed : none;
  for (const auto& w : work) {
    if (s.interrupted.load()) return fail_with_hint(request, m, s, AcquireError{AcquireReason::interrupted, ref});
    if (auto failure = acquire_file(root, staging, tm, w, installed, models, context, probe, s)) {
      return fail_with_hint(request, m, s, *failure);
    }
    if (bundle) {
      const auto& f = m.files[w.file_index];
      const auto into = must_rel(std::string{kBundleFilesDir} + "/" + f.path);
      if (auto failure = ensure_parent_of(root, into)) return fail_with_hint(request, m, s, *failure);
      if (auto failure = rename_within(root, join_rel(staging, f.path), into, s.store)) return fail_with_hint(request, m, s, *failure);
    }
  }
  s.progress.notice("verifying");
  if (s.interrupted.load()) return fail_with_hint(request, m, s, AcquireError{AcquireReason::interrupted, ref});

  if (bundle) {
    // Every sourced file is in files/: the staging area is no longer needed (finish_bundle
    // also removes it when the bundle is complete).
    if (auto failure = remove_tree_within(root, staging_root)) return fail_with_hint(request, m, s, *failure);
    if (auto failure = finish_bundle(root, tm)) return fail_with_hint(request, m, s, *failure);
    const bool complete = exists_within(root, must_rel(kBundleMarkerName));
    if (!complete) {
      s.ui.say("bundle incomplete: the files without a source are listed in MISSING");
      s.ui.say("complete it on a developer machine with: python3 -I tools/secure/make_bundle.py ...; then strata import <dir>");
    } else {
      s.ui.say("bundle complete; install it with: strata import <dir>");
    }
    return PullOutcome{ExitCode::success, std::nullopt, std::nullopt, false, complete};
  }
  // Every file was verified in this run (fetched, re-hashed or copied while hashing), §3.7.
  auto published = publish_staged(models, key, tm, s.loaded, s.store, false);
  if (auto* failure = std::get_if<AcquireError>(&published)) return failed(*failure);
  const auto& result = std::get<PublishResult>(published);
  for (const auto& retired : result.still_retired) {
    s.ui.say("retired (still loaded): " + model_ref_text(retired.name, retired.tag) + " " + retired.dir);
  }
  s.ui.say("installed " + ref + "; start it with: strata run " + ref);
  return PullOutcome{ExitCode::success, std::nullopt, result.entry, false, false};
}

}  // namespace secure_frontend
