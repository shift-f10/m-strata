#pragma once

#include "secure_frontend/acquire_reason.hpp"
#include "secure_frontend/manifest_json.hpp"
#include "secure_frontend/model_index_model.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace secure_frontend {

struct TrustedManifest final {
  ParsedManifest parsed;
  ModelOrigin origin;
};

// "--trust-manifest sha256:<64 lowercase hex>" → the hex, or nullopt.
[[nodiscard]] std::optional<std::string> parse_trust_argument(std::string_view text);

// Spec §2.5 / D2 / D7: trusted iff the digest is in this binary's catalog, or
// equals the operator-typed digest (then it must not shadow a catalog name:tag).
[[nodiscard]] std::variant<TrustedManifest, AcquireError> trust_manifest_bytes(
    std::string_view bytes, const std::optional<std::string>& trusted_digest, const ManifestRules& rules);

// name[:tag] (tag "default" when absent) → the catalog manifest, or model_unknown.
[[nodiscard]] std::variant<TrustedManifest, AcquireError> resolve_catalog_manifest(const ModelReference& reference,
                                                                                   const ManifestRules& rules);

[[nodiscard]] AcquireError issue_to_error(const ManifestIssue& issue);

}  // namespace secure_frontend
