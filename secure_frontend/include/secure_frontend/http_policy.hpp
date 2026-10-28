#pragma once

#include "secure_frontend/config.hpp"
#include "secure_frontend/http_request_handler.hpp"
#include "secure_frontend/http_request_head.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// Computed once at startup from the validated listener (spec §4.3, §4.4).
// allowed_hosts and cors_origins hold exact, lower-case serializations.
struct HttpPolicy final {
  std::vector<std::string> allowed_hosts;
  std::vector<std::string> same_origins;   // "http://" + each allowed host
  std::vector<std::string> cors_origins;
};

// Allowed hosts: the bound authority (none for a wildcard bind), plus
// `localhost:<port>` for a loopback bind, plus every alias lower-cased.
[[nodiscard]] HttpPolicy make_http_policy(const IpAddress& bind, std::uint16_t port,
                                          const std::vector<std::string>& host_aliases,
                                          const std::vector<std::string>& cors_origins);

enum class PolicyVerdict { allowed, malformed, forbidden };
// `hosts` holds every Host field value. None, several or an invalid one is
// malformed (400); a valid one outside the allowlist is forbidden (403). The
// value is compared after ASCII lower-casing (names only; ports have no letters).
[[nodiscard]] PolicyVerdict check_host(const HttpPolicy& policy, const std::vector<std::string_view>& hosts);

struct OriginDecision final {
  PolicyVerdict verdict;
  std::optional<std::string> allow_origin;   // set only for a configured CORS origin
};
// Origins are compared byte-for-byte (no folding). Absent: allowed; several
// or unknown (including `null`): forbidden.
[[nodiscard]] OriginDecision check_origin(const HttpPolicy& policy, const std::vector<std::string_view>& origins);

inline constexpr std::string_view kPreflightAllowHeaders =
    "authorization, content-type, x-stainless-arch, x-stainless-lang, x-stainless-os, x-stainless-package-version, "
    "x-stainless-retry-count, x-stainless-runtime, x-stainless-runtime-version, x-stainless-timeout";
// nullopt: not a preflight. Otherwise a 204 or a 403 response (never reaches auth or a route).
[[nodiscard]] std::optional<HttpResponse> evaluate_preflight(const HttpPolicy& policy, const RequestHead& head);

// Config grammars (SP1 addendum rows listener.host_aliases / listener.cors_origins).
[[nodiscard]] bool is_valid_host_alias(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_cors_origin(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_host_header(std::string_view value);

}  // namespace secure_frontend
