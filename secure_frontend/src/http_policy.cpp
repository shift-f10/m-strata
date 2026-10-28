#include "secure_frontend/http_policy.hpp"

#include "secure_frontend/api_errors.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

namespace secure_frontend {
namespace {

bool has_control_or_non_ascii(std::string_view value) {
  return std::any_of(value.begin(), value.end(), [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return byte <= 0x1fU || byte == 0x7fU || byte > 0x7fU;
  });
}

bool is_decimal_port(std::string_view value) {
  if (value.empty() || value.size() > 5U) return false;
  std::uint32_t port = 0;
  for (const char character : value) {
    if (character < '0' || character > '9') return false;
    port = (port * 10U) + static_cast<std::uint32_t>(character - '0');
  }
  return port > 0U && port <= 65535U;
}

bool is_valid_ipv4(std::string_view value) {
  std::uint32_t parts = 0;
  std::size_t start = 0;
  while (start < value.size()) {
    const std::size_t end = value.find('.', start);
    const std::string_view part = value.substr(start, end - start);
    if (part.empty() || part.size() > 3U || (part.size() > 1U && part.front() == '0')) return false;
    std::uint32_t number = 0;
    for (const char character : part) {
      if (character < '0' || character > '9') return false;
      number = (number * 10U) + static_cast<std::uint32_t>(character - '0');
    }
    if (number > 255U || ++parts > 4U) return false;
    if (end == std::string_view::npos) break;
    start = end + 1U;
  }
  return parts == 4U;
}

bool is_valid_hostname(std::string_view value) {
  if (value.empty() || value.size() > 253U || value.back() == '.') return false;
  const bool only_digits_and_dots = std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= '0' && character <= '9') || character == '.';
  });
  if (only_digits_and_dots) return is_valid_ipv4(value);
  std::size_t start = 0;
  while (start < value.size()) {
    const std::size_t end = value.find('.', start);
    const std::string_view label = value.substr(start, end - start);
    if (label.empty() || label.size() > 63U || label.front() == '-' || label.back() == '-') return false;
    for (const char character : label) {
      const bool alphanumeric = (character >= 'A' && character <= 'Z') ||
                                (character >= 'a' && character <= 'z') ||
                                (character >= '0' && character <= '9');
      if (!alphanumeric && character != '-') return false;
    }
    if (end == std::string_view::npos) break;
    start = end + 1U;
  }
  return true;
}

bool is_hexadecimal(std::string_view value) {
  return !value.empty() && value.size() <= 4U && std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
  });
}

bool is_valid_ipv6(std::string_view value) {
  if (value.empty() || value.find(":::") != std::string_view::npos) return false;
  const std::size_t compression = value.find("::");
  if (compression != std::string_view::npos && value.find("::", compression + 2U) != std::string_view::npos) return false;
  std::uint32_t groups = 0;
  std::size_t start = 0;
  while (start < value.size()) {
    const std::size_t end = value.find(':', start);
    const std::string_view group = value.substr(start, end - start);
    if (!group.empty() && (!is_hexadecimal(group) || ++groups > 8U)) return false;
    if (end == std::string_view::npos) break;
    start = end + 1U;
  }
  return compression == std::string_view::npos ? groups == 8U : groups < 8U;
}

}  // namespace

bool is_valid_host_header(std::string_view value) {
  if (value.empty() || has_control_or_non_ascii(value)) return false;
  if (value.front() == '[') {
    const std::size_t close = value.find(']');
    if (close == std::string_view::npos || !is_valid_ipv6(value.substr(1U, close - 1U))) return false;
    const std::string_view suffix = value.substr(close + 1U);
    return suffix.empty() || (suffix.front() == ':' && is_decimal_port(suffix.substr(1U)));
  }
  if (value.find(':') != std::string_view::npos) {
    const std::size_t separator = value.rfind(':');
    return value.find(':') == separator && is_valid_hostname(value.substr(0U, separator)) &&
           is_decimal_port(value.substr(separator + 1U));
  }
  return is_valid_hostname(value);
}

namespace {

bool contains(const std::vector<std::string>& values, std::string_view candidate) {
  return std::find(values.begin(), values.end(), candidate) != values.end();
}

std::string bound_authority(const IpAddress& bind, std::uint16_t port) {
  const std::string& text = bind.canonical_text();
  return (bind.family() == IpFamily::v6 ? "[" + text + "]" : text) + ":" + std::to_string(port);
}

constexpr std::string_view kAllowedRequestHeaders[] = {
    "authorization", "content-type", "x-stainless-arch", "x-stainless-lang", "x-stainless-os",
    "x-stainless-package-version", "x-stainless-retry-count", "x-stainless-runtime",
    "x-stainless-runtime-version", "x-stainless-timeout"};

// Every comma-separated item of every Access-Control-Request-Headers field,
// OWS-trimmed and compared ASCII case-insensitively, is in the fixed list.
// Empty items (",,", a trailing comma) are skipped.
bool request_headers_allowed(const std::vector<std::string_view>& lists) {
  for (std::string_view list : lists) {
    for (;;) {
      const std::size_t comma = list.find(',');
      const std::string_view item = trim_ows(list.substr(0, comma));
      if (!item.empty() &&
          std::none_of(std::begin(kAllowedRequestHeaders), std::end(kAllowedRequestHeaders),
                       [item](std::string_view allowed) { return ascii_iequal(item, allowed); })) {
        return false;
      }
      if (comma == std::string_view::npos) break;
      list.remove_prefix(comma + 1U);
    }
  }
  return true;
}

}  // namespace

bool is_valid_host_alias(const std::string_view value) noexcept {
  return value.find('@') == std::string_view::npos && is_valid_host_header(value);
}

bool is_valid_cors_origin(const std::string_view value) noexcept {
  std::string_view rest;
  if (value.starts_with("http://")) {
    rest = value.substr(7U);
  } else if (value.starts_with("https://")) {
    rest = value.substr(8U);
  } else {
    return false;
  }
  if (rest.empty() || rest.find_first_of("/?#@") != std::string_view::npos) return false;
  if (std::any_of(rest.begin(), rest.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) return false;
  return is_valid_host_header(rest);
}

HttpPolicy make_http_policy(const IpAddress& bind, const std::uint16_t port, const std::vector<std::string>& host_aliases,
                            const std::vector<std::string>& cors_origins) {
  HttpPolicy policy;
  if (bind.scope() != AddressScope::wildcard) policy.allowed_hosts.push_back(bound_authority(bind, port));
  if (bind.scope() == AddressScope::loopback) policy.allowed_hosts.push_back("localhost:" + std::to_string(port));
  for (const auto& alias : host_aliases) {
    std::string lowered = ascii_to_lower(alias);
    if (!contains(policy.allowed_hosts, lowered)) policy.allowed_hosts.push_back(std::move(lowered));
  }
  for (const auto& host : policy.allowed_hosts) policy.same_origins.push_back("http://" + host);
  policy.cors_origins = cors_origins;
  return policy;
}

PolicyVerdict check_host(const HttpPolicy& policy, const std::vector<std::string_view>& hosts) {
  if (hosts.size() != 1U || !is_valid_host_header(hosts.front())) return PolicyVerdict::malformed;
  return contains(policy.allowed_hosts, ascii_to_lower(hosts.front())) ? PolicyVerdict::allowed : PolicyVerdict::forbidden;
}

OriginDecision check_origin(const HttpPolicy& policy, const std::vector<std::string_view>& origins) {
  if (origins.empty()) return {PolicyVerdict::allowed, std::nullopt};
  if (origins.size() != 1U) return {PolicyVerdict::forbidden, std::nullopt};
  if (contains(policy.same_origins, origins.front())) return {PolicyVerdict::allowed, std::nullopt};
  if (contains(policy.cors_origins, origins.front())) return {PolicyVerdict::allowed, std::string{origins.front()}};
  return {PolicyVerdict::forbidden, std::nullopt};
}

std::optional<HttpResponse> evaluate_preflight(const HttpPolicy& policy, const RequestHead& head) {
  const auto origins = head.values("origin");
  const auto methods = head.values("access-control-request-method");
  if (head.method != "OPTIONS" || origins.empty() || methods.empty()) return std::nullopt;
  const bool allowed = origins.size() == 1U && contains(policy.cors_origins, origins.front()) && methods.size() == 1U &&
                       (methods.front() == "GET" || methods.front() == "POST") &&
                       request_headers_allowed(head.values("access-control-request-headers"));
  if (!allowed) return make_error_response(ApiError::forbidden);
  return HttpResponse{.status = 204,
                      .headers = {{"Access-Control-Allow-Origin", std::string{origins.front()}},
                                  {"Access-Control-Allow-Methods", "GET, POST"},
                                  {"Access-Control-Allow-Headers", std::string{kPreflightAllowHeaders}},
                                  {"Access-Control-Max-Age", "600"},
                                  {"Vary", "Origin"}},
                      .body = ""};
}

}  // namespace secure_frontend
