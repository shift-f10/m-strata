#pragma once

#include "secure_frontend/acquire_reason.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/manifest_trust.hpp"
#include "secure_frontend/model_index_model.hpp"
#include "secure_frontend/models_store.hpp"

#include <atomic>
#include <functional>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

// §4.8 "is it loaded?". Server discovery does not exist yet (Controller note 5).
class LoadedVersions {
 public:
  virtual ~LoadedVersions() = default;
  [[nodiscard]] virtual bool is_held(std::string_view name, std::string_view tag, std::string_view dir) = 0;
};

class NoServerDiscovery final : public LoadedVersions {
 public:
  [[nodiscard]] bool is_held(std::string_view, std::string_view, std::string_view) override { return false; }
};

[[nodiscard]] RelativePath staging_dir_path(std::string_view key);

// §4.4: (2) the regular files under `dir` are exactly the manifest's files plus
// `manifest_file_name`, which holds the exact manifest bytes; no link, junction,
// hard link, device or FIFO; (1) when hash_files, every file's size and SHA-256;
// (3) qwen layout + SP1's load_local_tokenizer_pack (byte_bpe) on <pack>/tokenizer; a gemma4
// manifest's launch.tokenizer_pack must hold vocab.json, merges.json, token_type.json and
// tokenizer.json and load in spm_bpe mode (Gemma 4 plan 1 Task 4).
// A tree that cannot be listed (or exceeds the portable path limits) is publish_failed.
// `interrupted` (§8.3, final fix F1) stops the re-hash between chunks with AcquireError interrupted.
[[nodiscard]] std::optional<AcquireError> verify_tree(const CanonicalRoot& root, const RelativePath& dir,
                                                      const TrustedManifest& manifest, std::string_view manifest_file_name,
                                                      bool hash_files, const std::atomic<bool>* interrupted = nullptr);

enum class InstallDecisionKind { fresh, already_installed, update };
struct InstallDecision final {
  InstallDecisionKind kind;
  const IndexEntry* installed{nullptr};
};
// §4.7: lower sequence → manifest_downgrade unless allowed; equal sequence with
// another digest → manifest_conflict; equal digest → already installed.
[[nodiscard]] std::variant<InstallDecision, AcquireError> decide_install(const ModelIndex& index, const TrustedManifest& manifest,
                                                                         bool allow_downgrade);

struct PublishOptions final {
  // SECURE_FRONTEND_TEST_HOOKS builds only (§10.1 fault injection between steps 4 and 5):
  // returning true stops publish before the index is written.
  std::function<bool()> stop_after_version_rename;
};

struct PublishResult final {
  IndexEntry entry;
  bool adopted{false};
  std::vector<RetiredEntry> still_retired;  // held by the server (or still open); deleted by a later run
};

// §4.5 steps 1-6 for .staging/<key>. Caller holds .lock. hash_files=false only
// when every file was verified in this run (§3.7). `key` must be the manifest's
// staging key (§4.4 item 2: MANIFEST hashes to <k>).
[[nodiscard]] std::variant<PublishResult, AcquireError> publish_staged(const CanonicalRoot& models, std::string_view key,
                                                                       const TrustedManifest& manifest, LoadedVersions& loaded,
                                                                       const StoreEnvironment& env, bool hash_files,
                                                                       const PublishOptions& options = {});
// §4.5 crash recovery: a version directory named for this manifest, not the live
// one of its name:tag (absent from the index, or retired — a downgrade), whose
// manifest.json has this digest, is re-hashed and adopted. nullopt when there is none, or when
// its files do not verify (not adoptable: publish then reports "target exists").
// An interrupt (§8.3) during the re-hash is AcquireError interrupted, never "not adoptable".
[[nodiscard]] std::optional<std::variant<PublishResult, AcquireError>> adopt_orphan(const CanonicalRoot& models,
                                                                                    const TrustedManifest& manifest,
                                                                                    LoadedVersions& loaded,
                                                                                    const StoreEnvironment& env,
                                                                                    const std::atomic<bool>* interrupted = nullptr);
// §4.5 step 6 / rm --retired: deletes retired versions the server does not hold; rewrites the index.
// A retired row naming a live directory is dropped without touching the directory.
[[nodiscard]] std::optional<AcquireError> delete_unheld_retired(const CanonicalRoot& models, LoadedVersions& loaded,
                                                                const StoreEnvironment& env);
// §4.8 rm: refuses a held version unless force; writes the index first (the entry moved to
// `retired`), then deletes bottom-up and drops the retired row. A failed delete stays retired
// (Controller note 5) for a later pull/import/rm --retired.
[[nodiscard]] std::variant<IndexEntry, AcquireError> remove_installed(const CanonicalRoot& models, std::string_view name,
                                                                      std::string_view tag, LoadedVersions& loaded, bool force,
                                                                      const StoreEnvironment& env);

// What pull/import report: the active entry, or "already installed".
struct InstallOutcome final {
  IndexEntry entry;
  bool already_installed{false};
  bool adopted{false};
  std::vector<RetiredEntry> still_retired;
};

}  // namespace secure_frontend
