#include "secure_frontend/acquire_reason.hpp"

#include <array>

namespace secure_frontend {
namespace {

using R = AcquireReason;
using E = ExitCode;
using S = SameSource;

constexpr std::array<ReasonInfo, 50> kReasons{{
    {R::manifest_invalid, "manifest_invalid", E::manifest, S::not_applicable, false},
    {R::manifest_unsupported_version, "manifest_unsupported_version", E::manifest, S::not_applicable, false},
    {R::manifest_untrusted, "manifest_untrusted", E::manifest, S::not_applicable, false},
    {R::manifest_shadows_catalog, "manifest_shadows_catalog", E::manifest, S::not_applicable, false},
    {R::manifest_downgrade, "manifest_downgrade", E::manifest, S::not_applicable, false},
    {R::manifest_conflict, "manifest_conflict", E::manifest, S::not_applicable, false},
    {R::manifest_requires_bundle, "manifest_requires_bundle", E::manifest, S::not_applicable, false},
    {R::model_unknown, "model_unknown", E::manifest, S::not_applicable, false},
    {R::dns_failed, "dns_failed", E::transport, S::retried, true},
    {R::connect_failed, "connect_failed", E::transport, S::retried, true},
    {R::tls_failed, "tls_failed", E::transport, S::retried, true},
    {R::stalled, "stalled", E::transport, S::retried, true},
    {R::size_short, "size_short", E::transport, S::retried, true},
    {R::transport_failed, "transport_failed", E::transport, S::next_source, true},
    {R::range_mismatch, "range_mismatch", E::transport, S::restart_file, true},
    {R::size_mismatch_announced, "size_mismatch_announced", E::transport, S::restart_file, true},
    {R::host_not_allowed, "host_not_allowed", E::transport, S::next_source, true},
    {R::redirect_not_allowed, "redirect_not_allowed", E::transport, S::next_source, true},
    {R::redirect_limit, "redirect_limit", E::transport, S::next_source, true},
    {R::response_malformed, "response_malformed", E::transport, S::next_source, true},
    {R::address_not_allowed, "address_not_allowed", E::transport, S::next_source, true},
    {R::tls_untrusted, "tls_untrusted", E::transport, S::next_source, true},
    {R::http_status, "http_status", E::transport, S::next_source, true},
    {R::size_exceeded, "size_exceeded", E::verification, S::next_source_file_deleted, false},
    {R::digest_mismatch, "digest_mismatch", E::verification, S::next_source_file_deleted, false},
    {R::pack_layout_invalid, "pack_layout_invalid", E::verification, S::not_applicable, false},
    {R::bundle_invalid, "bundle_invalid", E::verification, S::not_applicable, false},
    {R::bundle_layout, "bundle_layout", E::filesystem, S::not_applicable, false},
    {R::bundle_incomplete, "bundle_incomplete", E::filesystem, S::not_applicable, false},
    {R::bundle_target_fat32, "bundle_target_fat32", E::filesystem, S::not_applicable, false},
    {R::bundle_target_not_empty, "bundle_target_not_empty", E::filesystem, S::not_applicable, false},
    {R::time_budget_exceeded, "time_budget_exceeded", E::transport, S::not_applicable, true},
    {R::total_budget_exceeded, "total_budget_exceeded", E::space, S::not_applicable, false},
    {R::disk_space, "disk_space", E::space, S::not_applicable, true},
    {R::models_busy, "models_busy", E::filesystem, S::not_applicable, true},
    {R::publish_failed, "publish_failed", E::filesystem, S::not_applicable, true},
    {R::index_write_failed, "index_write_failed", E::filesystem, S::not_applicable, true},
    {R::index_inconsistent, "index_inconsistent", E::filesystem, S::not_applicable, false},
    {R::license_forbids_bundle, "license_forbids_bundle", E::license, S::not_applicable, false},
    {R::downloader_missing, "downloader_missing", E::downloader, S::not_applicable, true},
    {R::downloader_version, "downloader_version", E::downloader, S::not_applicable, true},
    {R::downloader_untrusted_path, "downloader_untrusted_path", E::downloader, S::not_applicable, true},
    {R::downloader_unsupported, "downloader_unsupported", E::downloader, S::not_applicable, true},
    {R::downloader_confinement_failed, "downloader_confinement_failed", E::downloader, S::not_applicable, true},
    {R::declined, "declined", E::declined, S::not_applicable, false},
    {R::interrupted, "interrupted", E::interrupted, S::not_applicable, true},
    {R::engine_profile_unavailable, "engine_profile_unavailable", E::engine, S::not_applicable, false},
    {R::engine_too_old, "engine_too_old", E::engine, S::not_applicable, false},
    {R::usage_error, "usage_error", E::usage, S::not_applicable, false},
    {R::program_update_unavailable, "program_update_unavailable", E::program_update, S::not_applicable, false},
}};

static_assert(static_cast<std::size_t>(R::program_update_unavailable) + 1U == kReasons.size(),
              "kReasons has one row per AcquireReason, in enum order");

}  // namespace

std::span<const ReasonInfo> reason_table() noexcept { return kReasons; }
const ReasonInfo& reason_info(const AcquireReason reason) noexcept { return kReasons[static_cast<std::size_t>(reason)]; }
std::string_view reason_name(const AcquireReason reason) noexcept { return reason_info(reason).name; }
ExitCode exit_class(const AcquireReason reason) noexcept { return reason_info(reason).exit; }

std::string format_error(const AcquireError& error) {
  const auto& info = reason_info(error.reason);
  std::string text = "error: ";
  text.append(info.name);
  if (!error.detail.empty()) text.append(": ").append(error.detail);
  text.append(info.resumable ? " (resumable: yes)" : " (resumable: no)");
  return text;
}

}  // namespace secure_frontend
