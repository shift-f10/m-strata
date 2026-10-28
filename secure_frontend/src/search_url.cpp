#include "secure_frontend/search_url.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>

namespace secure_frontend {
namespace {

bool host_character(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

bool path_character(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
         c == '~' || c == '/';
}

std::string lower(std::string_view text) {
  std::string out(text);
  for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

}  // namespace

std::optional<SearchOrigin> parse_search_url(const std::string_view url) {
  SearchOrigin origin;
  std::string_view rest;
  if (lower(url.substr(0, 7)) == "http://") {
    rest = url.substr(7);
  } else if (lower(url.substr(0, 8)) == "https://") {
    origin.tls = true;
    rest = url.substr(8);
  } else {
    return std::nullopt;
  }
  const auto slash = rest.find('/');
  const std::string_view authority = rest.substr(0, slash);
  std::string_view path = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
  if (authority.empty() || authority.find('@') != std::string_view::npos) return std::nullopt;
  std::string_view port_text;
  if (authority.front() == '[') {
    const auto close = authority.find(']');
    if (close == std::string_view::npos || close < 3U) return std::nullopt;
    const auto inner = authority.substr(1, close - 1U);
    if (!std::all_of(inner.begin(), inner.end(), [](const char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0 || c == ':' || c == '.'; })) {
      return std::nullopt;
    }
    origin.host = std::string(inner);
    const auto after = authority.substr(close + 1U);
    if (!after.empty() && (after.front() != ':' || after.size() < 2U)) return std::nullopt;
    if (!after.empty()) port_text = after.substr(1);
  } else {
    const auto colon = authority.find(':');
    const auto host = authority.substr(0, colon);
    if (host.empty() || host.size() > 253U || !std::all_of(host.begin(), host.end(), host_character) || host.front() == '.' ||
        host.front() == '-') {
      return std::nullopt;
    }
    origin.host = lower(host);
    if (colon != std::string_view::npos) {
      port_text = authority.substr(colon + 1U);
      if (port_text.empty()) return std::nullopt;
    }
  }
  origin.port = origin.tls ? 443U : 80U;
  if (!port_text.empty()) {
    unsigned value = 0;
    const auto [end, error] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), value);
    if (error != std::errc{} || end != port_text.data() + port_text.size() || value == 0U || value > 65535U) return std::nullopt;
    origin.port = static_cast<std::uint16_t>(value);
  }
  if (!std::all_of(path.begin(), path.end(), path_character) || path.find("//") != std::string_view::npos) return std::nullopt;
  while (!path.empty() && path.back() == '/') path.remove_suffix(1);
  origin.base_path = std::string(path);
  return origin;
}

}  // namespace secure_frontend
