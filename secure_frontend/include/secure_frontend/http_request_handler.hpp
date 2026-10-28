#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace secure_frontend {

// The validator owns any secret material. It is called only after request
// syntax and Host/Origin policy have been accepted, and should compare the
// supplied token in constant time (constant_time_token_equals is provided for
// that purpose). It must return false on any token-source error.
using BearerTokenValidator = std::function<bool(std::string_view)>;

struct HttpResponse final {
  int status;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::string_view log_code{};   // §7.2 code for the request log (spec §8); never sent
  // Empty: the fixed `default-src 'none'` policy. Set only by the built-in page's own
  // responses (kWebUiContentSecurityPolicy); it replaces that one header, never adds one.
  std::string_view content_security_policy{};

  [[nodiscard]] std::optional<std::string_view> header_value(std::string_view name) const;
};

// Fixed 256-byte window comparison (detail::fixed_window_equals): no early
// exit on content and no dependence on either length up to the public maximum.
[[nodiscard]] bool constant_time_token_equals(std::string_view candidate,
                                              std::string_view expected);

}  // namespace secure_frontend
