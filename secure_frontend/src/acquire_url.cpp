#include "secure_frontend/acquire_url.hpp"

#include <algorithm>
#include <optional>

namespace secure_frontend {
namespace {

constexpr std::string_view kScheme = "https://";

constexpr bool is_digit(const char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool is_lower(const char c) noexcept { return c >= 'a' && c <= 'z'; }
constexpr bool is_upper(const char c) noexcept { return c >= 'A' && c <= 'Z'; }
constexpr bool is_hex(const char c) noexcept { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
constexpr bool is_unreserved(const char c) noexcept {
  return is_digit(c) || is_lower(c) || is_upper(c) || c == '-' || c == '.' || c == '_' || c == '~';
}
constexpr bool is_query_extra(const char c) noexcept { return std::string_view{"=&+,;:@!$'()*"}.find(c) != std::string_view::npos; }

// Unreserved, '/', %XX; in a query also the query-extra set.
bool valid_characters(const std::string_view text, const bool query) noexcept {
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '%') {
      if (i + 2 >= text.size() || !is_hex(text[i + 1]) || !is_hex(text[i + 2])) return false;
      i += 2;
      continue;
    }
    if (is_unreserved(c) || c == '/') continue;
    if (query && is_query_extra(c)) continue;
    return false;
  }
  return true;
}

std::string lowercase(std::string_view text) {
  std::string out{text};
  for (char& c : out) {
    if (is_upper(c)) c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

// authority = host[:443]; fills `host`.
std::variant<std::string, UrlProblem> parse_authority(const std::string_view authority, const bool lower) {
  if (authority.find('@') != std::string_view::npos) return UrlProblem::userinfo;
  if (authority.empty() || authority.front() == '[') return UrlProblem::bad_host;
  std::string_view host = authority;
  if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
    if (authority.substr(colon + 1) != "443") return UrlProblem::port_not_allowed;
    host = authority.substr(0, colon);
  }
  std::string normalized = lower ? lowercase(host) : std::string{host};
  if (!is_valid_dns_name(normalized)) return UrlProblem::bad_host;
  return normalized;
}

// target = path[?query]; query only when allowed.
std::optional<UrlProblem> check_target(const std::string_view target, const bool allow_query) {
  if (target.find('#') != std::string_view::npos) return UrlProblem::fragment;
  const auto question = target.find('?');
  if (question != std::string_view::npos) {
    if (!allow_query) return UrlProblem::query_not_allowed;
    const auto query = target.substr(question + 1);
    if (query.find('?') != std::string_view::npos || !valid_characters(query, true)) return UrlProblem::bad_character;
  }
  if (!valid_characters(target.substr(0, question), false)) return UrlProblem::bad_character;
  return std::nullopt;
}

bool is_ldh_label(const std::string_view label) noexcept {
  if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
  return std::all_of(label.begin(), label.end(), [](const char c) { return is_lower(c) || is_digit(c) || c == '-'; });
}

bool is_valid_proxy_host(const std::string_view host) noexcept {
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
    const auto inner = host.substr(1, host.size() - 2);
    return !inner.empty() && std::all_of(inner.begin(), inner.end(), [](const char c) {
      return is_digit(c) || (c >= 'a' && c <= 'f') || c == ':' || c == '.';
    });
  }
  if (host.empty() || host.size() > 253) return false;
  std::size_t start = 0;
  while (true) {
    const auto dot = host.find('.', start);
    if (!is_ldh_label(host.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start))) return false;
    if (dot == std::string_view::npos) return true;
    start = dot + 1;
  }
}

}  // namespace

bool is_valid_dns_name(const std::string_view name) noexcept {
  if (name.empty() || name.size() > 253) return false;
  std::size_t labels = 0;
  std::size_t start = 0;
  std::string_view last;
  while (true) {
    const auto dot = name.find('.', start);
    const auto label = name.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
    if (!is_ldh_label(label)) return false;
    ++labels;
    last = label;
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return labels >= 2 && !std::all_of(last.begin(), last.end(), is_digit);
}

bool is_valid_redirect_pattern(const std::string_view pattern) noexcept {
  if (pattern.starts_with("*.")) return is_valid_dns_name(pattern.substr(2));
  return is_valid_dns_name(pattern);
}

bool host_matches(const std::string_view pattern, const std::string_view host) noexcept {
  if (!pattern.starts_with("*.")) return pattern == host;
  const auto base = pattern.substr(1);  // ".<base>"
  if (host.size() <= base.size() || !host.ends_with(base)) return false;
  const auto label = host.substr(0, host.size() - base.size());
  return !label.empty() && label.find('.') == std::string_view::npos;
}

bool host_in(const std::span<const std::string> patterns, const std::string_view host) noexcept {
  return std::any_of(patterns.begin(), patterns.end(), [host](const std::string& p) { return host_matches(p, host); });
}

std::variant<HttpsUrl, UrlProblem> parse_source_url(const std::string_view text) {
  if (text.size() > kMaxSourceUrlBytes) return UrlProblem::too_long;
  if (!text.starts_with(kScheme)) return UrlProblem::not_https;
  const auto rest = text.substr(kScheme.size());
  const auto slash = rest.find('/');
  const auto authority = rest.substr(0, slash);
  if (authority.find_first_of("?#") != std::string_view::npos) {
    return authority.find('#') != std::string_view::npos ? UrlProblem::fragment : UrlProblem::query_not_allowed;
  }
  auto host = parse_authority(authority, false);
  if (auto* problem = std::get_if<UrlProblem>(&host)) return *problem;
  if (slash == std::string_view::npos) return UrlProblem::missing_path;
  const auto target = rest.substr(slash);
  if (const auto problem = check_target(target, false)) return *problem;
  if (target == "/") return UrlProblem::missing_path;
  return HttpsUrl{std::get<std::string>(std::move(host)), std::string{target}};
}

std::variant<HttpsUrl, UrlProblem> parse_location(const std::string_view value, const std::string_view current_host) {
  if (value.size() > kMaxLocationBytes) return UrlProblem::too_long;
  std::string host;
  std::string target;
  if (value.starts_with(kScheme)) {
    const auto rest = value.substr(kScheme.size());
    const auto end = rest.find_first_of("/?#");
    auto parsed = parse_authority(rest.substr(0, end), true);
    if (auto* problem = std::get_if<UrlProblem>(&parsed)) return *problem;
    host = std::get<std::string>(std::move(parsed));
    target = end == std::string_view::npos ? std::string{"/"} : std::string{rest.substr(end)};
    if (target.front() == '?') target.insert(target.begin(), '/');
    if (target.front() == '#') return UrlProblem::fragment;
  } else if (value.starts_with('/') && !value.starts_with("//")) {
    host = std::string{current_host};
    target = std::string{value};
  } else {
    return UrlProblem::not_https;
  }
  if (const auto problem = check_target(target, true)) return *problem;
  return HttpsUrl{std::move(host), std::move(target)};
}

std::string render_url(const HttpsUrl& url) { return std::string{kScheme} + url.host + url.target; }

std::variant<ProxySetting, UrlProblem> parse_proxy(const std::string_view text) {
  ProxySetting proxy;
  std::string_view rest;
  if (text.starts_with("http://")) {
    rest = text.substr(7);
  } else if (text.starts_with("https://")) {
    proxy.https = true;
    rest = text.substr(8);
  } else {
    return UrlProblem::not_https;
  }
  if (rest.find('@') != std::string_view::npos) return UrlProblem::userinfo;
  if (rest.find_first_of("/?#") != std::string_view::npos) return UrlProblem::bad_character;
  const auto colon = rest.rfind(':');
  if (colon == std::string_view::npos || (rest.front() == '[' && colon < rest.find(']'))) return UrlProblem::port_not_allowed;
  const auto port_text = rest.substr(colon + 1);
  if (port_text.empty() || port_text.size() > 5 || port_text.front() == '0' ||
      !std::all_of(port_text.begin(), port_text.end(), is_digit)) {
    return UrlProblem::port_not_allowed;
  }
  unsigned long port = 0;
  for (const char c : port_text) port = port * 10U + static_cast<unsigned long>(c - '0');
  if (port == 0 || port > 65535U) return UrlProblem::port_not_allowed;
  const auto host = rest.substr(0, colon);
  if (!is_valid_proxy_host(host)) return UrlProblem::bad_host;
  proxy.host = std::string{host};
  proxy.port = static_cast<std::uint16_t>(port);
  return proxy;
}

std::string render_proxy(const ProxySetting& proxy) {
  return std::string{proxy.https ? "https://" : "http://"} + proxy.host + ":" + std::to_string(proxy.port);
}

std::string_view describe_url_problem(const UrlProblem problem) noexcept {
  switch (problem) {
    case UrlProblem::not_https: return "must be an https:// URL";
    case UrlProblem::bad_host: return "host must be a lowercase DNS name with at least two labels";
    case UrlProblem::port_not_allowed: return "port must be absent or 443";
    case UrlProblem::userinfo: return "must not contain userinfo";
    case UrlProblem::fragment: return "must not contain a fragment";
    case UrlProblem::query_not_allowed: return "must not contain a query";
    case UrlProblem::bad_character: return "contains a character outside the allowed set";
    case UrlProblem::missing_path: return "must name a file path";
    case UrlProblem::too_long: return "is too long";
  }
  return "is invalid";
}

}  // namespace secure_frontend
