#include "secure_frontend/http_request_head.hpp"

#include <algorithm>

namespace secure_frontend {
namespace {

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }

bool target_ok(std::string_view target) noexcept {
  if (target.empty() || target.front() != '/') return false;
  return std::all_of(target.begin(), target.end(), [](char c) {
    const auto byte = static_cast<unsigned char>(c);
    return byte >= 0x21U && byte <= 0x7eU && c != '?' && c != '#' && c != '%' && c != '\\';
  });
}

bool value_ok(std::string_view value) noexcept {
  return std::all_of(value.begin(), value.end(), [](char c) {
    const auto byte = static_cast<unsigned char>(c);
    return (byte >= 0x20U && byte <= 0x7eU) || c == '\t';
  });
}

}  // namespace

bool ascii_iequal(const std::string_view a, const std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

std::string ascii_to_lower(const std::string_view value) {
  std::string lowered{value};
  for (char& c : lowered) c = lower(c);
  return lowered;
}

std::string_view trim_ows(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1U);
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1U);
  return value;
}

bool is_http_token(const std::string_view value) noexcept {
  if (value.empty()) return false;
  return std::all_of(value.begin(), value.end(), [](char c) {
    const bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    return alnum || std::string_view{"!#$%&'*+-.^_`|~"}.find(c) != std::string_view::npos;
  });
}

HeadScanResult scan_request_head(const std::string_view received, const HeadLimits& limits) noexcept {
  const std::size_t line_end = received.find("\r\n");
  if (line_end == std::string_view::npos) {
    return {received.size() > limits.max_request_line_bytes ? HeadScan::line_too_long : HeadScan::need_more, 0U};
  }
  if (line_end > limits.max_request_line_bytes) return {HeadScan::line_too_long, 0U};
  const std::size_t end = received.find("\r\n\r\n");
  if (end == std::string_view::npos) {
    return {received.size() > limits.max_header_bytes ? HeadScan::header_too_large : HeadScan::need_more, 0U};
  }
  const std::size_t head_bytes = end + 4U;
  if (head_bytes > limits.max_header_bytes) return {HeadScan::header_too_large, 0U};
  return {HeadScan::complete, head_bytes};
}

std::vector<std::string_view> RequestHead::values(const std::string_view name) const {
  std::vector<std::string_view> found;
  for (const auto& field : fields) {
    if (ascii_iequal(field.name, name)) found.emplace_back(field.value);
  }
  return found;
}

HeadParseResult parse_request_head(const std::string_view head, const HeadLimits& limits) {
  if (head.size() < 4U || head.substr(head.size() - 4U) != "\r\n\r\n") return ApiError::invalid_request;
  const std::string_view block = head.substr(0, head.size() - 2U);   // keeps the last field's CRLF
  // Bare CR or bare LF anywhere is a syntax error.
  for (std::size_t i = 0; i < block.size(); ++i) {
    if (block[i] == '\n' && (i == 0U || block[i - 1U] != '\r')) return ApiError::invalid_request;
    if (block[i] == '\r' && (i + 1U >= block.size() || block[i + 1U] != '\n')) return ApiError::invalid_request;
    if (block[i] == '\0') return ApiError::invalid_request;
  }
  const std::size_t line_end = block.find("\r\n");
  const std::string_view line = block.substr(0, line_end);
  const std::size_t sp1 = line.find(' ');
  const std::size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1U);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos || line.find(' ', sp2 + 1U) != std::string_view::npos) {
    return ApiError::invalid_request;
  }
  const std::string_view method = line.substr(0, sp1);
  const std::string_view target = line.substr(sp1 + 1U, sp2 - sp1 - 1U);
  const std::string_view version = line.substr(sp2 + 1U);
  if (version == "HTTP/1.0" || version == "HTTP/2.0") return ApiError::http_version_not_supported;
  if (version != "HTTP/1.1" || !is_http_token(method) || !target_ok(target)) return ApiError::invalid_request;

  RequestHead parsed;
  parsed.method = std::string{method};
  parsed.target = std::string{target};
  parsed.head_bytes = head.size();
  std::size_t start = line_end + 2U;
  while (start < block.size()) {
    const std::size_t end = block.find("\r\n", start);
    const std::string_view field = block.substr(start, end - start);
    if (field.empty() || field.front() == ' ' || field.front() == '\t') return ApiError::invalid_request;   // obs-fold
    const std::size_t colon = field.find(':');
    if (colon == std::string_view::npos || !is_http_token(field.substr(0, colon))) return ApiError::invalid_request;
    const std::string_view raw = field.substr(colon + 1U);
    if (!value_ok(raw)) return ApiError::invalid_request;
    parsed.fields.push_back({std::string{field.substr(0, colon)}, std::string{trim_ows(raw)}});
    start = end + 2U;
  }
  if (parsed.fields.size() > limits.max_headers) return ApiError::header_too_large;
  if (!parsed.values("transfer-encoding").empty()) return ApiError::length_required;
  const auto lengths = parsed.values("content-length");
  if (lengths.size() > 1U) return ApiError::invalid_request;
  if (lengths.size() == 1U) {
    const std::string_view digits = lengths.front();
    if (digits.empty() || digits.size() > 10U ||
        !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      return ApiError::invalid_request;
    }
    std::size_t value = 0;
    for (const char c : digits) value = value * 10U + static_cast<std::size_t>(c - '0');
    if (value > limits.max_request_bytes - std::min(limits.max_request_bytes, head.size())) return ApiError::request_too_large;
    parsed.content_length = value;
  }
  if (parsed.method == "GET" && parsed.content_length.value_or(0U) > 0U) return ApiError::invalid_request;
  if (parsed.method == "POST" && !parsed.content_length.has_value()) return ApiError::length_required;
  for (const auto expectation : parsed.values("expect")) {
    if (!ascii_iequal(expectation, "100-continue")) return ApiError::expectation_failed;
    parsed.expect_continue = true;
  }
  return parsed;
}

}  // namespace secure_frontend
