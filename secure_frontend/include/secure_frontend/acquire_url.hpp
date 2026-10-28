#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace secure_frontend {

inline constexpr std::size_t kMaxSourceUrlBytes = 2048;
inline constexpr std::size_t kMaxLocationBytes = 8192;

// host: lowercase DNS name. target: path (and, for a Location only, "?query"),
// always starting with '/'. Never printed, logged or stored when it carries a query.
struct HttpsUrl final {
  std::string host;
  std::string target;
};

enum class UrlProblem {
  not_https, bad_host, port_not_allowed, userinfo, fragment, query_not_allowed, bad_character, missing_path, too_long,
};

// Lowercase LDH labels (1-63 bytes, no leading/trailing '-'), at least two
// labels, at most 253 bytes, no trailing dot, last label not all digits (so no IPv4 literal).
[[nodiscard]] bool is_valid_dns_name(std::string_view name) noexcept;
// A DNS name, or "*." followed by a DNS name (which itself has >= 2 labels).
[[nodiscard]] bool is_valid_redirect_pattern(std::string_view pattern) noexcept;
// Exact match, or "*.<base>" matching exactly one extra leftmost label.
[[nodiscard]] bool host_matches(std::string_view pattern, std::string_view host) noexcept;
[[nodiscard]] bool host_in(std::span<const std::string> patterns, std::string_view host) noexcept;

// Manifest source: https://<host>[:443]/<path>; path = RFC 3986 unreserved, '/', %XX;
// no userinfo, query or fragment; host already lowercase; <= 2048 bytes.
[[nodiscard]] std::variant<HttpsUrl, UrlProblem> parse_source_url(std::string_view text);
// Location header: an absolute https URL (host lowercased) or an absolute path
// resolved against current_host. Query allowed with = & + , ; : @ ! $ ' ( ) * in
// addition to the path set; never [ ] { } space " \ ; <= 8 KiB.
[[nodiscard]] std::variant<HttpsUrl, UrlProblem> parse_location(std::string_view value, std::string_view current_host);
[[nodiscard]] std::string render_url(const HttpsUrl& url);

// pull.proxy: http://<host>:<port> or https://<host>:<port>; host = LDH name
// (one label allowed), dotted IPv4 or [IPv6]; port 1..65535 without leading zero.
struct ProxySetting final {
  bool https{false};
  std::string host;
  std::uint16_t port{0};
};
[[nodiscard]] std::variant<ProxySetting, UrlProblem> parse_proxy(std::string_view text);
[[nodiscard]] std::string render_proxy(const ProxySetting& proxy);

[[nodiscard]] std::string_view describe_url_problem(UrlProblem problem) noexcept;

}  // namespace secure_frontend
