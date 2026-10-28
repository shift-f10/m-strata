#pragma once

// Web search (docs/superpowers/specs/2026-10-27-web-search-design.md §2): the SearXNG instance search.url names - read
// by the config in every build (search.* is checked even where it is refused), used by strata-search to connect.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

// The instance a URL names: http(s)://host[:port][/path], no user, query or fragment; base_path "" or "/a/b" (no
// trailing slash). The host is a DNS name, an IPv4 address or a bracketed IPv6 one (kept without its brackets).
struct SearchOrigin final {
  bool tls{false};
  std::string host;
  std::uint16_t port{0};
  std::string base_path;
};
[[nodiscard]] std::optional<SearchOrigin> parse_search_url(std::string_view url);

}  // namespace secure_frontend
