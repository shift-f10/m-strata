#include "secure_frontend/manifest_trust.hpp"

#include "secure_frontend/catalog.hpp"
#include "secure_frontend/sha256.hpp"

namespace secure_frontend {
namespace {

std::variant<TrustedManifest, AcquireError> parse_trusted(const std::string_view bytes, const ManifestRules& rules,
                                                          const ModelOrigin origin) {
  auto parsed = parse_manifest(bytes, rules);
  if (const auto* issue = std::get_if<ManifestIssue>(&parsed)) return issue_to_error(*issue);
  return TrustedManifest{std::get<ParsedManifest>(std::move(parsed)), origin};
}

}  // namespace

AcquireError issue_to_error(const ManifestIssue& issue) {
  return AcquireError{issue.reason, issue.key_path + ": " + std::string{issue.message}};
}

std::optional<std::string> parse_trust_argument(const std::string_view text) {
  constexpr std::string_view kPrefix = "sha256:";
  if (!text.starts_with(kPrefix) || !is_lower_hex_digest(text.substr(kPrefix.size()))) return std::nullopt;
  return std::string{text.substr(kPrefix.size())};
}

std::variant<TrustedManifest, AcquireError> trust_manifest_bytes(const std::string_view bytes,
                                                                 const std::optional<std::string>& trusted_digest,
                                                                 const ManifestRules& rules) {
  const std::string digest = to_hex(sha256(bytes));
  // §2.5 rule 2: a typed digest must equal the file's, also for catalog bytes (a typo or the wrong file).
  if (trusted_digest && *trusted_digest != digest) return AcquireError{AcquireReason::manifest_untrusted, "manifest sha256 " + digest};
  if (find_catalog_by_digest(digest) != nullptr) return parse_trusted(bytes, rules, ModelOrigin::catalog);
  if (!trusted_digest) {
    return AcquireError{AcquireReason::manifest_untrusted, "manifest sha256 " + digest};
  }
  auto trusted = parse_trusted(bytes, rules, ModelOrigin::third_party);
  if (const auto* manifest = std::get_if<TrustedManifest>(&trusted)) {
    const auto& m = manifest->parsed.manifest;
    if (find_catalog_entry(m.name, m.tag) != nullptr) {
      return AcquireError{AcquireReason::manifest_shadows_catalog, model_ref_text(m.name, m.tag)};
    }
  }
  return trusted;
}

std::variant<TrustedManifest, AcquireError> resolve_catalog_manifest(const ModelReference& reference,
                                                                     const ManifestRules& rules) {
  const auto* entry = find_catalog_entry(reference.name, reference.tag.value_or("default"));
  if (entry == nullptr) {
    return AcquireError{AcquireReason::model_unknown, reference.name + (reference.tag ? ":" + *reference.tag : std::string{})};
  }
  return parse_trusted(entry->bytes, rules, ModelOrigin::catalog);
}

}  // namespace secure_frontend
