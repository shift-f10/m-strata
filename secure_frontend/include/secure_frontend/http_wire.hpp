#pragma once

#include "secure_frontend/http_request_handler.hpp"
#include "secure_frontend/request_router.hpp"

#include <chrono>
#include <string>
#include <string_view>

namespace secure_frontend {

inline constexpr std::string_view kContinueResponse = "HTTP/1.1 100 Continue\r\n\r\n";

[[nodiscard]] std::string_view reason_phrase(int status) noexcept;
// RFC 9110 §5.6.7 IMF-fixdate, e.g. "Wed, 14 Oct 2026 07:43:05 GMT".
[[nodiscard]] std::string imf_fixdate(std::chrono::system_clock::time_point now);
// Status line, Date, Connection: close, the security headers and head.headers;
// never Content-Length (a stream frames itself); ends with the blank line.
[[nodiscard]] std::string serialize_head(const HttpHead& head, std::chrono::system_clock::time_point now);
// Status line, Date, Connection: close, Content-Length (not for 204), the six
// fixed security headers of spec §4.6, then the response's own headers except
// any that carries CR/LF or names a header written here, Transfer-Encoding
// (a complete response is always Content-Length framed) or Server/X-Powered-By.
// Built on the same helper as serialize_head.
[[nodiscard]] std::string serialize_response(const HttpResponse& response, std::chrono::system_clock::time_point now);

}  // namespace secure_frontend
