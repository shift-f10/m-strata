#include "secure_frontend/http_request_handler.hpp"

#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/http_policy.hpp"
#include "secure_frontend/request_router.hpp"
#include "secure_frontend/secret_bytes.hpp"

#include <utility>

namespace secure_frontend {

std::optional<std::string_view> HttpResponse::header_value(const std::string_view name) const {
  for (const auto& [header_name, value] : headers) {
    if (ascii_iequal(header_name, name)) return value;
  }
  return std::nullopt;
}

// Kept (pre-flight P24/V8): the socket server tests and the handler tests still use it.
bool constant_time_token_equals(std::string_view candidate, std::string_view expected) {
  return detail::fixed_window_equals(candidate, expected, nullptr);
}

HttpResponse handle_http_request(const std::string_view request, RequestRouter& router, const HeadLimits& limits) {
  const HeadScanResult scan = scan_request_head(request, limits);
  if (scan.state == HeadScan::line_too_long) return make_error_response(ApiError::request_line_too_long);
  if (scan.state == HeadScan::header_too_large) return make_error_response(ApiError::header_too_large);
  if (scan.state != HeadScan::complete) return make_error_response(ApiError::invalid_request);
  const HeadParseResult parsed = parse_request_head(request.substr(0, scan.head_bytes), limits);
  if (const auto* error = std::get_if<ApiError>(&parsed)) return make_error_response(*error);
  const RequestHead& head = std::get<RequestHead>(parsed);
  RouteDecision decision = router.authorize(head);
  if (decision.early.has_value()) return std::move(*decision.early);
  const std::string_view body = request.substr(scan.head_bytes);
  if (body.size() != head.content_length.value_or(0U)) return make_error_response(ApiError::invalid_request);
  RouteResult result = router.handle(head, body, decision, RequestContext{});
  if (auto* complete = std::get_if<HttpResponse>(&result)) return std::move(*complete);
  return make_error_response(ApiError::internal_error);
}

}  // namespace secure_frontend
