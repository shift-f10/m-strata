#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace secure_frontend {

// Spec §8.3: exit codes of pull/import/export/verify/update/rm/run.
enum class ExitCode : int {
  success = 0, usage = 1, declined = 2, transport = 3, verification = 4, manifest = 5, space = 6,
  filesystem = 7, interrupted = 8, downloader = 9, license = 10, engine = 11, program_update = 12,
};

// Spec §8.2, in table order. usage_error and program_update_unavailable carry exit codes 1 and 12.
enum class AcquireReason : std::uint8_t {
  manifest_invalid, manifest_unsupported_version, manifest_untrusted, manifest_shadows_catalog, manifest_downgrade,
  manifest_conflict, manifest_requires_bundle, model_unknown,
  dns_failed, connect_failed, tls_failed, stalled, size_short, transport_failed,
  range_mismatch, size_mismatch_announced,
  host_not_allowed, redirect_not_allowed, redirect_limit, response_malformed, address_not_allowed, tls_untrusted,
  http_status,
  size_exceeded, digest_mismatch, pack_layout_invalid, bundle_invalid,
  bundle_layout, bundle_incomplete, bundle_target_fat32, bundle_target_not_empty,
  time_budget_exceeded, total_budget_exceeded, disk_space,
  models_busy, publish_failed, index_write_failed, index_inconsistent,
  license_forbids_bundle,
  downloader_missing, downloader_version, downloader_untrusted_path, downloader_unsupported,
  downloader_confinement_failed,
  declined, interrupted, engine_profile_unavailable, engine_too_old,
  usage_error, program_update_unavailable,
};

// Spec §8.2 "Same source": what a later attempt on the same source does.
enum class SameSource : std::uint8_t { not_applicable, retried, restart_file, next_source, next_source_file_deleted };

struct ReasonInfo final {
  AcquireReason reason;
  std::string_view name;
  ExitCode exit;
  SameSource same_source;
  bool resumable;  // §8.2 "Rerun resumes"
};

[[nodiscard]] std::span<const ReasonInfo> reason_table() noexcept;
[[nodiscard]] const ReasonInfo& reason_info(AcquireReason reason) noexcept;
[[nodiscard]] std::string_view reason_name(AcquireReason reason) noexcept;
[[nodiscard]] ExitCode exit_class(AcquireReason reason) noexcept;
[[nodiscard]] constexpr int exit_code_value(const ExitCode code) noexcept { return static_cast<int>(code); }

// A failure: a fixed reason plus at most the §8.1 variable parts (name:tag,
// manifest-relative path, host name, HTTP status number, byte counts, digests,
// config key paths). Never a URL path or query, a Location, a proxy URL, an IP
// address, a token, a config value, curl output, or a path outside roots.models.
struct AcquireError final {
  AcquireReason reason;
  std::string detail;
};

// "error: <reason>[: <detail>] (resumable: yes|no)"
[[nodiscard]] std::string format_error(const AcquireError& error);

}  // namespace secure_frontend
