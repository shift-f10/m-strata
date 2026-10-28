#pragma once

#include "secure_frontend/api_errors.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

struct HeadLimits final {
  std::size_t max_request_line_bytes;
  std::size_t max_header_bytes;
  std::size_t max_headers;
  std::size_t max_request_bytes;
};

enum class HeadScan { need_more, complete, line_too_long, header_too_large };
struct HeadScanResult final {
  HeadScan state;
  std::size_t head_bytes{0};   // complete only: bytes up to and including the final CRLFCRLF
};
// Looks only at the bytes received so far; never allocates.
[[nodiscard]] HeadScanResult scan_request_head(std::string_view received, const HeadLimits& limits) noexcept;

struct HeaderField final {
  std::string name;    // as sent
  std::string value;   // optional whitespace trimmed
};

struct RequestHead final {
  std::string method;
  std::string target;
  std::vector<HeaderField> fields;
  std::size_t head_bytes{0};
  std::optional<std::size_t> content_length;
  bool expect_continue{false};
  // Every value of the named field (ASCII case-insensitive), in order.
  [[nodiscard]] std::vector<std::string_view> values(std::string_view name) const;
};

using HeadParseResult = std::variant<RequestHead, ApiError>;
// `head` is exactly the scanned head (request line, fields, CRLFCRLF). The
// rules and their order are spec §4.2 / plan Task 2.
[[nodiscard]] HeadParseResult parse_request_head(std::string_view head, const HeadLimits& limits);

// Shared ASCII helpers (pre-flight D6): the one copy of case folding and OWS
// trimming for every HTTP-layer user (policy, router).
[[nodiscard]] bool ascii_iequal(std::string_view a, std::string_view b) noexcept;
[[nodiscard]] std::string ascii_to_lower(std::string_view value);       // A-Z only; other bytes unchanged
[[nodiscard]] std::string_view trim_ows(std::string_view value) noexcept; // strips leading/trailing SP and HTAB
[[nodiscard]] bool is_http_token(std::string_view value) noexcept;

}  // namespace secure_frontend
