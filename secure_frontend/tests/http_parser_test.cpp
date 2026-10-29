#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/api_router.hpp"
#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/config.hpp"
#include "secure_frontend/http_policy.hpp"
#include "secure_frontend/http_request_head.hpp"
#include "secure_frontend/http_wire.hpp"
#include "secure_frontend/request_router.hpp"
#include "secure_frontend/secret_bytes.hpp"
#include "secure_frontend/token_verifier.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
using secure_frontend::ApiError;

// Spec §7.2, copied row by row: status, type, code, message.
struct ExpectedRow { ApiError error; int status; const char* type; const char* code; const char* message; };
constexpr ExpectedRow kExpected[] = {
    {ApiError::invalid_request, 400, "invalid_request_error", "invalid_request", "The request is malformed or not supported."},
    {ApiError::invalid_json, 400, "invalid_request_error", "invalid_json", "The request body is not valid JSON within the allowed limits."},
    {ApiError::unsupported_parameter, 400, "invalid_request_error", "unsupported_parameter", "This parameter is not supported by this server."},
    {ApiError::extension_disabled, 400, "invalid_request_error", "extension_disabled", "Tools, functions and extensions are disabled on this server."},
    {ApiError::context_length_exceeded, 400, "invalid_request_error", "context_length_exceeded", "The prompt exceeds the model context or the configured prompt limit."},
    {ApiError::invalid_api_key, 401, "authentication_error", "invalid_api_key", "Missing or invalid API key."},
    {ApiError::forbidden, 403, "permission_error", "forbidden", "The request origin or host is not allowed."},
    {ApiError::not_found, 404, "invalid_request_error", "not_found", "Not found."},
    {ApiError::model_not_found, 404, "invalid_request_error", "model_not_found", "The requested model is not served here."},
    {ApiError::method_not_allowed, 405, "invalid_request_error", "method_not_allowed", "Method not allowed."},
    {ApiError::request_timeout, 408, "invalid_request_error", "request_timeout", "The request was not received in time."},
    {ApiError::length_required, 411, "invalid_request_error", "length_required", "A Content-Length body is required; chunked bodies are not accepted."},
    {ApiError::request_too_large, 413, "invalid_request_error", "request_too_large", "The request body is too large."},
    {ApiError::request_line_too_long, 414, "invalid_request_error", "request_line_too_long", "The request line is too long."},
    {ApiError::unsupported_media_type, 415, "invalid_request_error", "unsupported_media_type", "Send application/json."},
    {ApiError::expectation_failed, 417, "invalid_request_error", "expectation_failed", "Unsupported Expect header."},
    {ApiError::header_too_large, 431, "invalid_request_error", "header_too_large", "The request header is too large."},
    {ApiError::internal_error, 500, "server_error", "internal_error", "Internal error."},
    {ApiError::server_busy, 503, "server_error", "server_busy", "The server is busy; retry later."},
    {ApiError::engine_unavailable, 503, "server_error", "engine_unavailable", "The local engine is not available."},
    {ApiError::engine_error, 503, "server_error", "engine_error", "The local engine could not complete the request."},
    {ApiError::server_shutting_down, 503, "server_error", "server_shutting_down", "The server is shutting down."},
    {ApiError::http_version_not_supported, 505, "invalid_request_error", "http_version_not_supported", "Only HTTP/1.1 is supported."},
    // Model launcher (plan 2026-10-23 Tasks 3-4).
    {ApiError::invalid_option, 400, "invalid_request_error", "invalid_option", "An option value is not valid for this model."},
    {ApiError::control_not_allowed, 403, "permission_error", "control_not_allowed", "Model control is not allowed for this client."},
    {ApiError::model_not_running, 409, "invalid_request_error", "model_not_running", "The requested model is configured but not running; start it first."},
    {ApiError::switch_in_progress, 409, "invalid_request_error", "switch_in_progress", "A model start or stop is already in progress."},
    {ApiError::model_switching, 503, "server_error", "model_switching", "The server is switching models; retry shortly."},
    {ApiError::no_model_running, 503, "server_error", "no_model_running", "No model is running; start one first."},
    // Model downloads (plan 2026-10-23-model-downloads Task 4).
    {ApiError::download_in_progress, 409, "invalid_request_error", "download_in_progress", "This model is already downloading, queued or installed as a card."},
    {ApiError::upstream_unavailable, 502, "server_error", "upstream_unavailable", "Hugging Face could not be reached or answered unexpectedly."},
    {ApiError::insufficient_storage, 507, "server_error", "insufficient_storage", "Insufficient disk space on the data drive for this download."},
    {ApiError::download_not_active, 409, "invalid_request_error", "download_not_active", "This model is not downloading or queued."},
    // GUI-first start-up (spec 2026-10-27 §3, §4): the login link, and the page's cards and folders.
    {ApiError::login_link_refused, 403, "permission_error", "login_link_refused", "This sign-in link is used, expired or wrong; restart the server for a new one."},
    {ApiError::folder_refused, 422, "invalid_request_error", "folder_refused", "This folder can't be used."},
    {ApiError::folder_not_private, 409, "invalid_request_error", "folder_not_private", "Other users of this PC can change files in this folder; make it private first."},
    {ApiError::card_refused, 422, "invalid_request_error", "card_refused", "This model can't be added as a card."},
    {ApiError::card_exists, 409, "invalid_request_error", "card_exists", "A card of that name is already loaded."},
    {ApiError::card_read_only, 403, "permission_error", "card_read_only", "This card is the operator's (models.cards); change its file instead."},
    {ApiError::card_in_use, 409, "invalid_request_error", "card_in_use", "The model is running, starting or stopping; stop it first."},
    {ApiError::delete_refused, 403, "permission_error", "delete_refused", "The code, the API key or the confirmation was not right; nothing was deleted. Ask for a new code."},
    {ApiError::calibration_unsupported, 422, "invalid_request_error", "calibration_unsupported", "This model's engine has no calibration here yet."},
    {ApiError::not_calibrating, 409, "invalid_request_error", "not_calibrating", "No calibration is running."},
    {ApiError::settings_refused, 422, "invalid_request_error", "settings_refused", "These settings can't be used."},
};

// GUI-first start-up (spec 2026-10-27 §4): a card or folder refusal carries the module's own fixed reason (never a
// path); every other row keeps its message.
void err_card_and_folder_rows_carry_their_reason() {
  const auto refused = secure_frontend::make_error_response(ApiError::card_refused, {.reason = "the model file must be a .gguf file"});
  expect(refused.status == 422 && refused.body.find("\"message\":\"the model file must be a .gguf file\"") != std::string::npos &&
             refused.body.find("\"code\":\"card_refused\"") != std::string::npos,
         "card_refused carries its reason: " + refused.body);
  const auto folder = secure_frontend::make_error_response(ApiError::folder_refused, {.reason = "there is no such folder"});
  expect(folder.body.find("\"message\":\"there is no such folder\"") != std::string::npos, "folder_refused carries its reason");
  const auto settings = secure_frontend::make_error_response(ApiError::settings_refused, {.reason = "alerts.email.from: must be an email address"});
  expect(settings.status == 422 && settings.body.find("\"message\":\"alerts.email.from: must be an email address\"") != std::string::npos,
         "settings_refused carries its reason");
  const auto other = secure_frontend::make_error_response(ApiError::invalid_request, {.reason = "anything"});
  expect(other.body.find("anything") == std::string::npos, "other rows never take a reason");
  const auto empty = secure_frontend::make_error_response(ApiError::card_refused);
  expect(empty.body.find("\"message\":\"This model can't be added as a card.\"") != std::string::npos, "no reason: the row's message");
}

// Model launcher (plan 2026-10-23): a switch answers like a restarting engine (Retry-After 5),
// and an option refusal names the option (an option name is a fixed literal of the option
// table; anything else stays null).
void err_launcher_rows_carry_their_headers_and_option() {
  const auto switching = secure_frontend::make_error_response(ApiError::model_switching);
  expect(switching.header_value("Retry-After") == std::optional<std::string_view>{"5"}, "model_switching carries Retry-After: 5");
  const auto none = secure_frontend::make_error_response(ApiError::no_model_running);
  expect(!none.header_value("Retry-After").has_value(), "no_model_running does not invite a retry");
  expect(secure_frontend::render_error_body(ApiError::invalid_option, "kv_resident").find("\"param\":\"kv_resident\"") != std::string::npos,
         "invalid_option names the option");
  expect(secure_frontend::render_error_body(ApiError::invalid_option, "not an option").find("\"param\":null") != std::string::npos,
         "a name that is no option's is never echoed");
  expect(secure_frontend::render_error_body(ApiError::invalid_request, "kv_resident").find("\"param\":null") != std::string::npos,
         "only invalid_option carries an option name");
}

void err_every_code_renders_fixed_body() {
  expect(secure_frontend::api_error_table().size() == std::size(kExpected), "the table has exactly the 23 rows of spec §7.2, the launcher's 6, the downloads' 4 and the GUI-first start-up's 11");
  for (const auto& row : kExpected) {
    const std::string body = std::string{"{\"error\":{\"message\":\""} + row.message + "\",\"type\":\"" + row.type +
                             "\",\"param\":null,\"code\":\"" + row.code + "\"}}\n";
    expect(secure_frontend::render_error_body(row.error) == body, std::string{"body of "} + row.code);
    const auto response = secure_frontend::make_error_response(row.error);
    expect(response.status == row.status, std::string{"status of "} + row.code);
    expect(response.body == body, std::string{"response body of "} + row.code);
    expect(response.header_value("Content-Type") == std::optional<std::string_view>{"application/json; charset=utf-8"},
           std::string{"content type of "} + row.code);
    expect(response.log_code == row.code, std::string{"log code of "} + row.code);
  }
  const auto unauthorized = secure_frontend::make_error_response(ApiError::invalid_api_key);
  expect(unauthorized.header_value("WWW-Authenticate") == std::optional<std::string_view>{"Bearer"}, "401 carries WWW-Authenticate: Bearer");
  const auto busy = secure_frontend::make_error_response(ApiError::server_busy);
  expect(busy.header_value("Retry-After") == std::optional<std::string_view>{"1"}, "server_busy carries Retry-After: 1");
  const auto engine_error = secure_frontend::make_error_response(ApiError::engine_error);
  expect(engine_error.header_value("x-should-retry") == std::optional<std::string_view>{"false"}, "engine_error carries x-should-retry: false");
  const auto starting = secure_frontend::make_error_response(ApiError::engine_unavailable, {.retry_after = 5});
  expect(starting.header_value("Retry-After") == std::optional<std::string_view>{"5"}, "engine_unavailable can carry Retry-After: 5");
  const auto exhausted = secure_frontend::make_error_response(ApiError::engine_unavailable);
  expect(!exhausted.header_value("Retry-After").has_value(), "engine_unavailable without retry_after carries none");
  // Spec §3.8 (Task 9): Retry-After 5 unless the supervisor's recovery is "exhausted".
  expect(secure_frontend::engine_retry_after("") == std::optional<int>{5} &&
             secure_frontend::engine_retry_after("restarting") == std::optional<int>{5} &&
             !secure_frontend::engine_retry_after("exhausted").has_value(),
         "engine_retry_after: 5 except when recovery is exhausted");
  const auto method = secure_frontend::make_error_response(ApiError::method_not_allowed, {.allow = "GET"});
  expect(method.header_value("Allow") == std::optional<std::string_view>{"GET"}, "405 carries Allow");
}

void err_param_is_key_only() {
  const std::string tools = secure_frontend::render_error_body(ApiError::extension_disabled, "tools");
  expect(tools.find("\"param\":\"tools\"") != std::string::npos, "a fixed extension key is named in param");
  const std::string stream = secure_frontend::render_error_body(ApiError::unsupported_parameter, "stream");
  expect(stream.find("\"param\":\"stream\"") != std::string::npos, "a fixed unsupported key is named in param");
  const std::string unknown = secure_frontend::render_error_body(ApiError::unsupported_parameter, "secret\"value");
  expect(unknown.find("\"param\":null") != std::string::npos && unknown.find("secret") == std::string::npos,
         "a key outside the fixed lists is never reflected");
  const std::string other_code = secure_frontend::render_error_body(ApiError::invalid_request, "tools");
  expect(other_code.find("\"param\":null") != std::string::npos, "param is null for every other code");
  for (const auto key : {"n", "presence_penalty", "frequency_penalty", "logit_bias", "stop", "logprobs", "top_k", "min_p",
                         "top_logprobs", "response_format", "content", "stream", "tools", "tool_choice", "functions",
                         "function_call", "parallel_tool_calls", "mcp_servers", "role"}) {
    expect(secure_frontend::is_fixed_param_key(key), std::string{"fixed key "} + key);
  }
  expect(!secure_frontend::is_fixed_param_key("model") && !secure_frontend::is_fixed_param_key(""), "other keys are not fixed");
}

void imf_fixdate_is_bounded_for_out_of_range_clocks() {
  using namespace std::chrono;
  // The extreme clocks system_clock can even represent (nanoseconds): years
  // ~2262 and ~1677. Every one still yields a well-formed 29-byte IMF-fixdate.
  const std::string far_future = secure_frontend::imf_fixdate(system_clock::time_point{system_clock::duration::max()});
  expect(far_future.size() == 29 && far_future.ends_with(" GMT") && far_future[19] == ':' && far_future[22] == ':',
         "the latest representable clock still yields a bounded IMF-fixdate");
  const std::string far_past = secure_frontend::imf_fixdate(system_clock::time_point{system_clock::duration::min()});
  expect(far_past.size() == 29 && far_past.ends_with(" GMT") && far_past[19] == ':' && far_past[22] == ':',
         "the earliest representable clock still yields a bounded IMF-fixdate");
  expect(secure_frontend::imf_fixdate(system_clock::time_point{seconds{0}}) == "Thu, 01 Jan 1970 00:00:00 GMT",
         "the epoch renders exactly: bounds change nothing for real clocks");
}

void wire_response_has_security_headers_and_date() {
  using namespace std::chrono;
  const auto now = system_clock::time_point{seconds{1791963785}};   // 2026-10-14 07:43:05 UTC
  expect(secure_frontend::imf_fixdate(now) == "Wed, 14 Oct 2026 07:43:05 GMT", "IMF-fixdate");
  expect(secure_frontend::imf_fixdate(system_clock::time_point{seconds{951782400}}) == "Tue, 29 Feb 2000 00:00:00 GMT",
         "IMF-fixdate on a leap day");
  secure_frontend::HttpResponse response{.status = 200, .headers = {{"Content-Type", "application/json; charset=utf-8"},
                                                                    {"X-Bad", "a\r\nInjected: 1"}, {"Server", "leak"}},
                                         .body = "{}\n"};
  const std::string wire = secure_frontend::serialize_response(response, now);
  expect(wire.rfind("HTTP/1.1 200 OK\r\n", 0) == 0, "status line");
  for (const auto line : {"Date: Wed, 14 Oct 2026 07:43:05 GMT\r\n", "Connection: close\r\n", "Content-Length: 3\r\n",
                          "Cache-Control: no-store\r\n", "X-Content-Type-Options: nosniff\r\n",
                          "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\n",
                          "Referrer-Policy: no-referrer\r\n", "Cross-Origin-Resource-Policy: same-origin\r\n",
                          "Content-Type: application/json; charset=utf-8\r\n"}) {
    expect(wire.find(line) != std::string::npos, std::string{"header line "} + line);
  }
  expect(wire.find("Injected") == std::string::npos, "a header value with CR/LF is dropped");
  expect(wire.find("Server:") == std::string::npos, "no Server header is ever written");
  expect(wire.size() >= 3U && wire.substr(wire.size() - 7U) == "\r\n\r\n{}\n", "body follows the blank line");
  const secure_frontend::HttpResponse no_content{.status = 204, .headers = {}, .body = ""};
  const std::string empty = secure_frontend::serialize_response(no_content, now);
  expect(empty.rfind("HTTP/1.1 204 No Content\r\n", 0) == 0 && empty.find("Content-Length") == std::string::npos,
         "204 has no Content-Length");
  for (const int status : {400, 401, 403, 404, 405, 408, 411, 413, 414, 415, 417, 431, 500, 503, 505}) {
    expect(!secure_frontend::reason_phrase(status).empty() && secure_frontend::reason_phrase(status) != "Unknown",
           "reason phrase for " + std::to_string(status));
  }
  expect(secure_frontend::kContinueResponse == "HTTP/1.1 100 Continue\r\n\r\n", "interim 100 response");
}
// The built-in page (session log step 12): a response's own policy replaces the fixed
// default-src 'none' line, once; the reserved-header guard still drops a CSP in `headers`.
void wire_response_carries_a_page_policy_once() {
  using namespace std::chrono;
  const auto now = system_clock::time_point{seconds{1791963785}};
  secure_frontend::HttpResponse page{.status = 200,
                                     .headers = {{"Content-Type", "text/html; charset=utf-8"},
                                                 {"Content-Security-Policy", "default-src *"}},
                                     .body = "<!doctype html>\n",
                                     .content_security_policy = "default-src 'none'; script-src 'self'"};
  const std::string wire = secure_frontend::serialize_response(page, now);
  const auto count = [&wire](std::string_view needle) {
    std::size_t n = 0;
    for (std::size_t at = wire.find(needle); at != std::string::npos; at = wire.find(needle, at + 1U)) ++n;
    return n;
  };
  expect(count("Content-Security-Policy: ") == 1U && wire.find("Content-Security-Policy: default-src 'none'; script-src 'self'\r\n") != std::string::npos,
         "exactly one CSP line, the page's own:\n" + wire);
  expect(wire.find("default-src *") == std::string::npos, "a CSP in the route's headers is still dropped (reserved)");
  for (const auto line : {"Cache-Control: no-store\r\n", "X-Content-Type-Options: nosniff\r\n", "Referrer-Policy: no-referrer\r\n",
                          "Cross-Origin-Resource-Policy: same-origin\r\n"}) {
    expect(wire.find(line) != std::string::npos, std::string{"the other fixed headers stay: "} + line);
  }
  const secure_frontend::HttpResponse plain{.status = 200, .headers = {}, .body = "{}"};
  expect(secure_frontend::serialize_response(plain, now).find("Content-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\n") !=
             std::string::npos,
         "without one, the fixed default stays");
}
constexpr secure_frontend::HeadLimits kLimits{.max_request_line_bytes = 4096, .max_header_bytes = 16384,
                                              .max_headers = 64, .max_request_bytes = 65536};

std::variant<secure_frontend::RequestHead, ApiError> head_of(const std::string& text) {
  const auto scan = secure_frontend::scan_request_head(text, kLimits);
  if (scan.state == secure_frontend::HeadScan::line_too_long) return ApiError::request_line_too_long;
  if (scan.state == secure_frontend::HeadScan::header_too_large) return ApiError::header_too_large;
  if (scan.state != secure_frontend::HeadScan::complete) return ApiError::internal_error;   // "incomplete" marker for tests
  return secure_frontend::parse_request_head(std::string_view{text}.substr(0, scan.head_bytes), kLimits);
}
bool is_error(const std::variant<secure_frontend::RequestHead, ApiError>& r, ApiError e) {
  return std::holds_alternative<ApiError>(r) && std::get<ApiError>(r) == e;
}
const std::string kHost = "Host: 127.0.0.1:8080\r\n";

void dos01_request_line_over_limit_is_414() {
  const std::string long_target(4100, 'a');
  expect(is_error(head_of("GET /" + long_target + " HTTP/1.1\r\n" + kHost + "\r\n"), ApiError::request_line_too_long),
         "a complete head with a request line over 4096 bytes is 414");
  const auto partial = secure_frontend::scan_request_head("GET /" + long_target, kLimits);
  expect(partial.state == secure_frontend::HeadScan::line_too_long, "414 is known before the line ends");
  const auto short_partial = secure_frontend::scan_request_head("GET /abc", kLimits);
  expect(short_partial.state == secure_frontend::HeadScan::need_more, "a short partial line needs more bytes");
}

void dos01_header_block_over_limit_is_431() {
  std::string head = "GET /health HTTP/1.1\r\n" + kHost;
  while (head.size() < 16500U) head += "X-Fill: " + std::string(100, 'f') + "\r\n";
  expect(secure_frontend::scan_request_head(head, kLimits).state == secure_frontend::HeadScan::header_too_large,
         "431 is known before the blank line arrives");
  expect(is_error(head_of(head + "\r\n"), ApiError::header_too_large), "a complete head over 16384 bytes is 431");
}

void dos01_too_many_headers_is_431() {
  std::string head = "GET /health HTTP/1.1\r\n" + kHost;
  for (int i = 0; i < 64; ++i) head += "X-" + std::to_string(i) + ": v\r\n";
  expect(is_error(head_of(head + "\r\n"), ApiError::header_too_large), "65 fields are 431");
  std::string ok = "GET /health HTTP/1.1\r\n" + kHost;
  for (int i = 0; i < 63; ++i) ok += "X-" + std::to_string(i) + ": v\r\n";
  expect(std::holds_alternative<secure_frontend::RequestHead>(head_of(ok + "\r\n")), "64 fields are accepted");
}

void dos01_transfer_encoding_variants_are_411() {
  for (const std::string te : {"Transfer-Encoding: chunked\r\n", "Transfer-Encoding: Chunked\r\n", "Transfer-Encoding:  chunked\r\n",
                               "Transfer-Encoding: gzip, chunked\r\n", "transfer-encoding: identity\r\n",
                               "Transfer-Encoding: chunked\r\nContent-Length: 2\r\n"}) {
    expect(is_error(head_of("POST /v1/chat/completions HTTP/1.1\r\n" + kHost + te + "\r\n"), ApiError::length_required),
           "411 for " + te);
  }
  expect(is_error(head_of("POST /v1/chat/completions HTTP/1.1\r\n" + kHost + "Transfer-Encoding : chunked\r\n\r\n"),
                  ApiError::invalid_request), "space before the colon is 400");
}

struct SeamRouter final : secure_frontend::RequestRouter {
  bool stream{false};
  std::string seen_body;
  secure_frontend::RouteDecision authorize(const secure_frontend::RequestHead& head) override {
    secure_frontend::RouteDecision decision;
    decision.route = secure_frontend::RouteId::chat;
    decision.body_limit = head.content_length.value_or(0U);
    return decision;
  }
  secure_frontend::RouteResult handle(const secure_frontend::RequestHead&, std::string_view body, const secure_frontend::RouteDecision&,
                                      const secure_frontend::RequestContext&) override {
    seen_body = std::string{body};
    if (stream) return secure_frontend::StreamHandler{[](secure_frontend::StreamSink&) {}};
    return secure_frontend::HttpResponse{.status = 200, .headers = {}, .body = "{}\n"};
  }
};

void dos01_content_length_variants() {
  const auto post = [](const std::string& cl) { return head_of("POST /v1/chat/completions HTTP/1.1\r\n" + kHost + cl + "\r\n"); };
  expect(is_error(post("Content-Length: 2\r\nContent-Length: 2\r\n"), ApiError::invalid_request), "duplicate");
  expect(is_error(post("Content-Length: +1\r\n"), ApiError::invalid_request), "sign");
  expect(is_error(post("Content-Length: 0x10\r\n"), ApiError::invalid_request), "hex");
  expect(is_error(post("Content-Length: 12345678901\r\n"), ApiError::invalid_request), "11 digits");
  expect(is_error(post("Content-Length: \r\n"), ApiError::invalid_request), "empty");
  expect(is_error(post("Content-Length: 70000\r\n"), ApiError::request_too_large), "over the whole-request limit is 413");
  expect(is_error(post(""), ApiError::length_required), "POST without Content-Length is 411");
  expect(is_error(head_of("GET /health HTTP/1.1\r\n" + kHost + "Content-Length: 1\r\n\r\n"), ApiError::invalid_request),
         "GET with a body is 400");
  const auto ok = post("Content-Length: 0000000002\r\n");
  expect(std::holds_alternative<secure_frontend::RequestHead>(ok) && std::get<secure_frontend::RequestHead>(ok).content_length == 2U,
         "ten digits with leading zeros are accepted");
  // Body-stage rules (pre-flight S12) through the in-memory seam: a body
  // shorter than Content-Length, and bytes after the body, are 400.
  SeamRouter router;
  const std::string head = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n\r\n";
  expect(secure_frontend::handle_http_request(head + "{}", router, kLimits).status == 400, "Content-Length mismatch: 400");
  expect(secure_frontend::handle_http_request(head + "{\"\"}x", router, kLimits).status == 400, "stray bytes after the body: 400");
  expect(secure_frontend::handle_http_request(head + "{\"\"}", router, kLimits).status == 200, "an exact body passes");
}

void head_request_line_and_header_syntax() {
  expect(is_error(head_of("GET /health HTTP/1.0\r\n" + kHost + "\r\n"), ApiError::http_version_not_supported), "1.0 is 505");
  expect(is_error(head_of("GET /health HTTP/2.0\r\n" + kHost + "\r\n"), ApiError::http_version_not_supported), "2.0 is 505");
  expect(is_error(head_of("GET /health HTTP/1.2\r\n" + kHost + "\r\n"), ApiError::invalid_request), "other versions are 400");
  expect(is_error(head_of("GET  /health HTTP/1.1\r\n" + kHost + "\r\n"), ApiError::invalid_request), "double space");
  for (const std::string target : {"/a?b", "/a#b", "/a%2e", "/a\\b", "*", "http://x/", "/a\x7f"}) {
    expect(is_error(head_of("GET " + target + " HTTP/1.1\r\n" + kHost + "\r\n"), ApiError::invalid_request), "target " + target);
  }
  expect(is_error(head_of("G(T /health HTTP/1.1\r\n" + kHost + "\r\n"), ApiError::invalid_request), "method token");
  expect(secure_frontend::ascii_to_lower("LocalHost:8080\xc3\x89") == "localhost:8080\xc3\x89" &&
             secure_frontend::trim_ows(" \tv \t") == "v" && secure_frontend::ascii_iequal("Expect", "eXPECT"),
         "shared ASCII helpers (pre-flight D6)");
  expect(is_error(head_of("GET /health HTTP/1.1\r\n" + kHost + " folded\r\n\r\n"), ApiError::invalid_request), "obs-fold");
  expect(is_error(head_of("GET /health HTTP/1.1\r\n" + kHost + "X: a\nY: b\r\n\r\n"), ApiError::invalid_request), "bare LF");
  expect(is_error(head_of("GET /health HTTP/1.1\r\n" + kHost + std::string("X: a\0b\r\n\r\n", 10)), ApiError::invalid_request), "NUL");
  expect(is_error(head_of("GET /health HTTP/1.1\r\n" + kHost + "X: caf\xc3\xa9\r\n\r\n"), ApiError::invalid_request), "non-ASCII value");
  expect(is_error(head_of("POST /v1/chat/completions HTTP/1.1\r\n" + kHost + "Content-Length: 2\r\nExpect: foo\r\n\r\n"),
                  ApiError::expectation_failed), "Expect: foo is 417");
  const auto cont = head_of("POST /v1/chat/completions HTTP/1.1\r\n" + kHost + "Content-Length: 2\r\nExpect: 100-Continue\r\n\r\n");
  expect(std::holds_alternative<secure_frontend::RequestHead>(cont) && std::get<secure_frontend::RequestHead>(cont).expect_continue,
         "Expect: 100-continue is recognised case-insensitively");
  const auto parsed = head_of("DELETE /v1/models HTTP/1.1\r\n" + kHost + "Origin: http://127.0.0.1:8080\r\n\r\n");
  expect(std::holds_alternative<secure_frontend::RequestHead>(parsed), "any token method parses (405 comes after auth)");
  if (const auto* head = std::get_if<secure_frontend::RequestHead>(&parsed)) {
    expect(head->method == "DELETE" && head->target == "/v1/models", "method and target");
    expect(head->values("HOST").size() == 1U && head->values("origin").front() == "http://127.0.0.1:8080", "values()");
  }
}

void wire_head_has_no_content_length() {
  const auto now = std::chrono::system_clock::time_point{std::chrono::seconds{1791963785}};
  const std::string head = secure_frontend::serialize_head(
      {200, {{"Content-Type", "text/event-stream; charset=utf-8"}, {"Transfer-Encoding", "chunked"}, {"Content-Length", "5"}}}, now);
  expect(head.rfind("HTTP/1.1 200 OK\r\n", 0) == 0, "status line");
  expect(head.find("Content-Length") == std::string::npos, "a head never carries Content-Length");
  expect(head.find("Transfer-Encoding: chunked\r\n") != std::string::npos, "a stream may set its own framing header");
  expect(head.find("X-Content-Type-Options: nosniff\r\n") != std::string::npos && head.find("Connection: close\r\n") != std::string::npos,
         "security headers and Connection: close");
  expect(head.size() >= 4U && head.substr(head.size() - 4U) == "\r\n\r\n", "ends with the blank line");
  const std::string complete = secure_frontend::serialize_response(
      {.status = 200, .headers = {{"Transfer-Encoding", "chunked"}}, .body = "ok"}, now);
  expect(complete.find("Transfer-Encoding") == std::string::npos && complete.find("Content-Length: 2\r\n") != std::string::npos,
         "complete responses always use Content-Length");
}

void route_result_seam_returns_complete_response() {
  SeamRouter router;
  const std::string request = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\n\r\n{}";
  const auto ok = secure_frontend::handle_http_request(request, router, kLimits);
  expect(ok.status == 200 && router.seen_body == "{}", "a complete response passes through the seam");
  expect(secure_frontend::handle_http_request(request + "x", router, kLimits).status == 400, "bytes after the body: 400");
  router.stream = true;
  const auto streamed = secure_frontend::handle_http_request(request, router, kLimits);
  expect(streamed.status == 500 && streamed.body == secure_frontend::render_error_body(ApiError::internal_error),
         "a StreamHandler needs a socket: the in-memory seam fails closed");
}

constexpr std::string_view kToken = "parser-token-0123456789abcdefghijklmnopq";

secure_frontend::ApiRouter make_router(std::vector<std::string> cors = {}, int* chat_calls = nullptr) {
  secure_frontend::SecretBytes secret;
  (void)secret.assign(kToken);
  secure_frontend::ApiRouterConfig config{
      .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address("127.0.0.1"), 8080, {}, cors),
      .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
      .model_id = "fake-model", .owned_by = "local", .chat_body_limit = 49152,
      .health = [] { return secure_frontend::HealthView{true, "ready", ""}; },
      .chat = [chat_calls](std::string_view, const secure_frontend::RequestContext&) -> secure_frontend::RouteResult {
        if (chat_calls != nullptr) ++*chat_calls;
        return secure_frontend::HttpResponse{.status = 200, .headers = {{"Content-Type", "application/json; charset=utf-8"}}, .body = "{}\n"};
      }};
  return secure_frontend::ApiRouter(std::move(config));
}

std::string chat_request(const std::string& extra_headers, const std::string& body = "{}") {
  return "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nAuthorization: Bearer " + std::string{kToken} +
         "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra_headers + "\r\n" + body;
}

void dos01_status_matrix_415_417_505() {
  auto router = make_router();
  const auto status_of = [&router](const std::string& request) {
    return secure_frontend::handle_http_request(request, router, kLimits).status;
  };
  expect(status_of(chat_request("Content-Type: text/plain\r\n")) == 415, "text/plain body is 415");
  expect(status_of(chat_request("")) == 415, "missing Content-Type is 415");
  expect(status_of(chat_request("Content-Type: application/json\r\nExpect: foo\r\n")) == 417, "Expect: foo is 417");
  expect(status_of("GET /health HTTP/1.0\r\nHost: 127.0.0.1:8080\r\n\r\n") == 505, "HTTP/1.0 is 505");
  expect(status_of("GET /health HTTP/2.0\r\nHost: 127.0.0.1:8080\r\n\r\n") == 505, "HTTP/2.0 is 505");
  expect(status_of(chat_request("Content-Type: application/json\r\n")) == 200, "a correct chat reaches the handler");
}

void content_type_variants_accepted() {
  auto router = make_router();
  for (const char* type : {"application/json", "application/json; charset=utf-8", "application/json;charset=UTF-8",
                           "Application/JSON", "application/json ;  charset=utf-8"}) {
    expect(secure_frontend::handle_http_request(chat_request(std::string{"Content-Type: "} + type + "\r\n"), router, kLimits).status == 200,
           std::string{"accepted Content-Type "} + type);
    expect(secure_frontend::is_json_content_type(type), std::string{"is_json_content_type "} + type);
  }
  for (const char* type : {"application/json; charset=latin1", "text/plain", "application/jsonx", "application/json; charset=utf-8; x=1",
                           "multipart/form-data"}) {
    expect(secure_frontend::handle_http_request(chat_request(std::string{"Content-Type: "} + type + "\r\n"), router, kLimits).status == 415,
           std::string{"rejected Content-Type "} + type);
    expect(!secure_frontend::is_json_content_type(type), std::string{"!is_json_content_type "} + type);
  }
  for (const char* type : {"", ";", "application/json;", "application/json; charset=", "application/json; charset=utf-88",
                           "application/json charset=utf-8", "xapplication/json", "application/json\t;\tcharset=utf-8x"}) {
    expect(!secure_frontend::is_json_content_type(type), std::string{"edge rejected: "} + type);
  }
  expect(secure_frontend::is_json_content_type(" \tapplication/json\t; charset=utf-8 "), "OWS around the parts is accepted");
  expect(secure_frontend::handle_http_request(chat_request("Content-Type: application/json\r\nContent-Type: application/json\r\n"),
                                              router, kLimits).status == 415, "two Content-Type fields are 415");
}

void auth_and_route_order_follows_spec() {
  int chat_calls = 0;
  auto router = make_router({"http://app.example:3000"}, &chat_calls);
  const auto run = [&router](const std::string& request) { return secure_frontend::handle_http_request(request, router, kLimits); };
  // Unauthenticated: 401 on every path and method, before 404/405/415/413.
  for (const std::string line : {"GET /nope", "DELETE /health", "POST /v1/chat/completions", "GET /v1/models/x", "PUT /v1/models"}) {
    const std::string body_headers = line.rfind("POST", 0) == 0 || line.rfind("PUT", 0) == 0 ? "Content-Length: 99999\r\n" : "";
    const auto r = run(line + " HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + body_headers + "\r\n");
    expect(r.status == (body_headers.empty() ? 401 : 413), line + " without a token");
  }
  const auto unauth = run("POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nContent-Length: 2\r\nContent-Type: text/plain\r\n\r\n{}");
  expect(unauth.status == 401 && unauth.header_value("WWW-Authenticate") == std::optional<std::string_view>{"Bearer"},
         "401 wins over 415 and carries WWW-Authenticate");
  const std::string auth = "Authorization: Bearer " + std::string{kToken} + "\r\n";
  expect(run("DELETE /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + auth + "\r\n").status == 405, "known path, other method: 405");
  expect(run("DELETE /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + auth + "\r\n").header_value("Allow") == std::optional<std::string_view>{"GET"},
         "405 names Allow");
  expect(run("GET /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + auth + "\r\n").header_value("Allow") ==
         std::optional<std::string_view>{"POST"}, "chat Allow: POST");
  expect(run("GET /nope HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + auth + "\r\n").status == 404, "unknown path after auth: 404");
  expect(run("GET /v1/models/ HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + auth + "\r\n").status == 404, "empty model id: 404");
  expect(run(chat_request("Content-Type: application/json\r\n", std::string(49153, ' '))).status == 413, "route body limit: 413");
  expect(run("GET /health HTTP/1.1\r\nHost: evil:8080\r\n" + auth + "\r\n").status == 403, "Host before auth: 403");
  expect(run("GET /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nOrigin: null\r\n" + auth + "\r\n").status == 403, "Origin before auth: 403");
  const auto cors = run("GET /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nOrigin: http://app.example:3000\r\n\r\n");
  expect(cors.status == 401 && cors.header_value("Access-Control-Allow-Origin") == std::optional<std::string_view>{"http://app.example:3000"},
         "an approved CORS origin gets ACAO even on 401");
  const auto preflight = run("OPTIONS /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nOrigin: http://app.example:3000\r\n"
                             "Access-Control-Request-Method: POST\r\n\r\n");
  expect(preflight.status == 204, "preflight needs no token");
  expect(run("GET /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + auth + auth + "\r\n").status == 400, "two Authorization fields: 400");
  expect(chat_calls == 0, "no chat handler call for any refused request");
  // Body-stage rules of the in-memory seam: Content-Length mismatch and stray bytes are 400.
  expect(run(chat_request("Content-Type: application/json\r\n") + "x").status == 400, "bytes after the body: 400");
  std::string short_body = chat_request("Content-Type: application/json\r\n");
  short_body.pop_back();
  expect(run(short_body).status == 400, "fewer bytes than Content-Length: 400");
}

// Task 7: chat request schema (spec §3.1, §7.2 param keys, B2, B6).
const secure_frontend::ChatSchemaLimits kSchema{};
std::string body_with(const std::string& members, const std::string& messages = R"([{"role":"user","content":"Hi"}])") {
  return R"({"model":"fake-model","messages":)" + messages + (members.empty() ? "" : "," + members) + "}";
}
secure_frontend::ChatRequestResult parse(const std::string& json) { return secure_frontend::parse_chat_request(json, kSchema); }
bool fails_with(const secure_frontend::ChatRequestResult& r, ApiError error, std::string_view param = {}) {
  const auto* e = std::get_if<secure_frontend::ChatRequestError>(&r);
  return e != nullptr && e->error == error && e->param == param;
}
const secure_frontend::ChatCompletionRequest* ok(const secure_frontend::ChatRequestResult& r) {
  return std::get_if<secure_frontend::ChatCompletionRequest>(&r);
}

void dos01_json_depth_5_is_400() {
  // The ceiling moved to 6 in SP5 M1 Task 12 (C-9 parts grammar, Ruling T12-k): the
  // parts form nests request -> messages -> message -> content -> part -> image_url.
  // The DOS property stands, one level deeper: depth 5 now reaches the SCHEMA, which
  // answers its own fixed 400; depth 7 is still invalid_json at the reader.
  // The ceiling is 16 since 2026-10-29 (an earlier answer's searches nest deeper): 16 levels reach the schema, 17 do
  // not reach it.
  const auto nested = [](const int levels) {
    std::string value = "1";
    for (int i = 0; i < levels; ++i) value = "{\"k\":" + value + "}";
    return R"("logit_bias":)" + value;
  };
  expect(fails_with(parse(body_with(R"("logit_bias":{"a":{"b":{"c":{"d":1}}}})")), ApiError::unsupported_parameter, "logit_bias"),
         "depth 5 reaches the schema: unsupported_parameter logit_bias");
  expect(fails_with(parse(body_with(nested(15))), ApiError::unsupported_parameter, "logit_bias"), "depth 16 (the ceiling) reaches the schema");
  expect(fails_with(parse(body_with(nested(16))), ApiError::invalid_json), "depth 17 is invalid_json");
  expect(fails_with(parse(body_with(R"("logit_bias":{"a":{"b":1}})")), ApiError::unsupported_parameter, "logit_bias"),
         "depth 4 parses and reaches the schema");
  expect(fails_with(parse("{\"model\":"), ApiError::invalid_json), "truncated JSON is invalid_json");
  expect(fails_with(parse(body_with(R"("model":"x")")), ApiError::invalid_json), "duplicate key is invalid_json");
}

void dos01_json_limits() {
  // the bounds themselves, at limits set to 128 messages and 64 KiB a message (the defaults are upstream's scale since
  // 2026-10-29: default_limits_take_a_long_conversation)
  secure_frontend::ChatSchemaLimits bounded;
  bounded.max_messages = 128U;
  bounded.max_message_bytes = 65536U;
  const auto parse_small = [&bounded](const std::string& json) { return secure_frontend::parse_chat_request(json, bounded); };
  std::string many = "[";
  for (int i = 0; i < 129; ++i) many += std::string{i ? "," : ""} + R"({"role":"user","content":"x"})";
  expect(fails_with(parse_small(body_with("", many + "]")), ApiError::invalid_request) ||
         fails_with(parse_small(body_with("", many + "]")), ApiError::invalid_json), "129 messages are refused");
  expect(fails_with(parse(body_with("", "[]")), ApiError::invalid_request), "no messages");
  expect(fails_with(parse_small(body_with("", R"([{"role":"user","content":")" + std::string(65537, 'a') + "\"}]")), ApiError::invalid_request) ||
         fails_with(parse_small(body_with("", R"([{"role":"user","content":")" + std::string(65537, 'a') + "\"}]")), ApiError::invalid_json),
         "a message over 64 KiB is refused");
  expect(fails_with(parse(body_with(R"("max_tokens":0)")), ApiError::invalid_request), "max_tokens 0");
  expect(fails_with(parse(body_with(R"("max_tokens":-1)")), ApiError::invalid_request), "max_tokens -1");
  expect(fails_with(parse(body_with(R"("max_tokens":1.5)")), ApiError::invalid_request), "max_tokens 1.5");
  expect(fails_with(parse(body_with(R"("max_tokens":1e3)")), ApiError::invalid_request), "max_tokens 1e3");
  expect(fails_with(parse(body_with(R"("max_tokens":"8")")), ApiError::invalid_request), "max_tokens as a string");
  expect(fails_with(parse(body_with(R"("max_tokens":01)")), ApiError::invalid_json), "leading zero is invalid JSON");
  expect(fails_with(parse(body_with(R"("max_tokens":5,"max_completion_tokens":5)")), ApiError::invalid_request), "both names");
  const auto big = parse(body_with(R"("max_tokens":99999999999)"));
  expect(ok(big) != nullptr && ok(big)->max_tokens == std::optional<std::size_t>{4294967295U}, "huge max_tokens is accepted and clamped later");
  const auto alias = parse(body_with(R"("max_completion_tokens":7)"));
  expect(ok(alias) != nullptr && ok(alias)->max_tokens == std::optional<std::size_t>{7U}, "max_completion_tokens is max_tokens");
  expect(fails_with(parse(body_with(R"("unknown":1)")), ApiError::invalid_request), "unknown key");
  expect(fails_with(parse(body_with(R"("user":")" + std::string(257, 'u') + "\"")), ApiError::invalid_request), "user over 256 bytes");
  expect(ok(parse(body_with(R"("user":"alice")"))) != nullptr, "user is accepted and ignored");
  // Session log step 15: remote-looking text is text (SP3 §6.2 "rejected or treated as text";
  // the server fetches nothing), so a link in an answer no longer breaks the next turn.
  const auto linked = parse(body_with("", R"([{"role":"user","content":"https://x"}])"));
  expect(ok(linked) != nullptr && ok(linked)->messages.front().content == "https://x", "remote-looking text is kept as text");
  // Ruling T12-l (SP5 M1 Task 12): name:tag is the served managed-model grammar, so
  // a colon alone no longer answers at the schema — "x::y" (empty segment) still
  // does, and "pull:x" passes the grammar only to be refused model_not_found by the
  // route (still a 400, still no engine contact — the egress proof covers it).
  expect(fails_with(parse(R"({"model":"x::y","messages":[{"role":"user","content":"Hi"}]})"), ApiError::invalid_request),
         "model grammar: an empty segment is invalid_request");
  expect(ok(parse(R"({"model":"pull:x","messages":[{"role":"user","content":"Hi"}]})")) != nullptr,
         "model grammar: name:tag parses and reaches the route");
}

void dos01_content_array_is_unsupported_parameter() {
  // A content array is container depth 4 and reaches the schema. The OpenAI
  // parts form nests deeper — since SP5 M1 Task 12 (C-9, Ruling T12-k) the reader
  // allows depth 6 so the VISION capability, not the JSON reader, answers it:
  // without one the schema refuses the array as unsupported_parameter "content".
  expect(fails_with(parse(body_with("", R"([{"role":"user","content":["Hi"]}])")),
                    ApiError::unsupported_parameter, "content"), "array content is unsupported_parameter content");
  expect(fails_with(parse(body_with("", R"([{"role":"user","content":[]}])")),
                    ApiError::unsupported_parameter, "content"), "empty array content is unsupported_parameter content");
  expect(fails_with(parse(body_with("", R"([{"role":"user","content":[{"type":"text","text":"Hi"}]}])")),
                    ApiError::unsupported_parameter, "content"),
         "content parts (depth 5) reach the schema and are unsupported_parameter content");
  expect(fails_with(parse(body_with("", R"([{"role":"tool","content":"run"}])")), ApiError::extension_disabled, "role"), "role tool");
  expect(fails_with(parse(body_with("", R"([{"role":"function","content":"run"}])")), ApiError::extension_disabled, "role"), "role function");
  for (const char* key : {"tools", "tool_choice", "functions", "function_call", "parallel_tool_calls", "mcp_servers"}) {
    expect(fails_with(parse(body_with(std::string{"\""} + key + "\":null,\"zzz\":1")), ApiError::extension_disabled, key),
           std::string{"extension key wins over an unknown key: "} + key);
  }
  expect(ok(parse(body_with(R"("stream":true)"))) != nullptr, "stream:true is a streamed response since M2");
  expect(ok(parse(body_with(R"("stream":false)"))) != nullptr, "stream:false is the complete response");
  expect(fails_with(parse(body_with(R"("stream_options":{"include_usage":true})")), ApiError::invalid_request), "stream_options without stream");
}

void stream_flag_type_and_options_rules() {
  // Named locals: ok() returns a pointer into the result, so it must not
  // be taken from a temporary (g++ -O3 flags the dangling use).
  const auto streaming = parse(body_with(R"("stream":true)"));
  expect(ok(streaming) != nullptr && ok(streaming)->stream, "stream:true is accepted (M2)");
  expect(ok(parse(body_with(R"("stream":false)"))) != nullptr && !ok(parse(body_with(R"("stream":false)")))->stream, "stream:false is a complete response");
  for (const char* bad : {R"("stream":"true")", R"("stream":1)", R"("stream":null)"}) {
    expect(fails_with(parse(body_with(bad)), ApiError::invalid_request), std::string{"stream must be a boolean: "} + bad);
  }
  const auto usage_parsed = parse(body_with(R"("stream":true,"stream_options":{"include_usage":true})"));
  const auto* usage = ok(usage_parsed);
  expect(usage != nullptr && usage->include_usage, "include_usage parsed");
  expect(ok(parse(body_with(R"("stream":true,"stream_options":{})"))) != nullptr, "empty stream_options is a no-op");
  for (const char* bad : {R"("stream_options":{"include_usage":true})", R"("stream":false,"stream_options":{"include_usage":true})",
                          R"("stream":true,"stream_options":{"include_usage":"yes"})", R"("stream":true,"stream_options":{"other":true})",
                          R"("stream":true,"stream_options":[])"}) {
    expect(fails_with(parse(body_with(bad)), ApiError::invalid_request), std::string{"invalid stream_options: "} + bad);
  }
}

void sampling_neutral_values_are_no_ops() {
  const auto r = parse(body_with(R"("presence_penalty":0,"frequency_penalty":0.0,"logit_bias":{},"stop":null,"n":1,"logprobs":false)"));
  expect(ok(r) != nullptr && ok(r)->sampling.empty(), "neutral values parse and change nothing");
  expect(ok(parse(body_with(R"("stop":[])"))) != nullptr, "stop [] is a no-op");
  expect(fails_with(parse(body_with(R"("presence_penalty":0.5)")), ApiError::unsupported_parameter, "presence_penalty"), "non-zero penalty");
  expect(fails_with(parse(body_with(R"("frequency_penalty":-1)")), ApiError::unsupported_parameter, "frequency_penalty"), "negative penalty");
  expect(fails_with(parse(body_with(R"("logit_bias":{"1":2})")), ApiError::unsupported_parameter, "logit_bias"), "non-empty logit_bias");
  expect(fails_with(parse(body_with(R"("stop":"x")")), ApiError::unsupported_parameter, "stop"), "stop string");
  expect(fails_with(parse(body_with(R"("stop":["x"])")), ApiError::unsupported_parameter, "stop"), "stop array");
  expect(fails_with(parse(body_with(R"("n":2)")), ApiError::unsupported_parameter, "n"), "n 2");
  expect(fails_with(parse(body_with(R"("logprobs":true)")), ApiError::unsupported_parameter, "logprobs"), "logprobs true");
  for (const char* key : {"top_k", "min_p", "top_logprobs", "response_format"}) {
    expect(fails_with(parse(body_with(std::string{"\""} + key + "\":1")), ApiError::unsupported_parameter, key), std::string{"SP5 key "} + key);
  }
  const auto passed = parse(body_with(R"("temperature":0.5,"top_p":0.9,"seed":42)"));
  expect(ok(passed) != nullptr && ok(passed)->sampling.temperature == std::optional<double>{0.5} &&
         ok(passed)->sampling.top_p == std::optional<double>{0.9} && ok(passed)->sampling.seed == std::optional<std::uint64_t>{42U},
         "bounded sampling values pass through (B6 option e)");
}

void sampling_bounds_rejected() {
  for (const char* member : {R"("temperature":-0.1)", R"("temperature":2.01)", R"("top_p":0)", R"("top_p":1.0001)",
                             R"("seed":-1)", R"("seed":1.5)", R"("temperature":"1")", R"("top_p":null)"}) {
    expect(fails_with(parse(body_with(member)), ApiError::invalid_request), std::string{"out of bounds: "} + member);
  }
  expect(ok(parse(body_with(R"("temperature":2,"top_p":1,"seed":9223372036854775807)"))) != nullptr, "upper bounds are inclusive");
}

void sampling_integer_forms_accepted() {
  const auto r = parse(body_with(R"("temperature":1,"top_p":1,"presence_penalty":0,"frequency_penalty":0)"));
  expect(ok(r) != nullptr && ok(r)->sampling.temperature == std::optional<double>{1.0} && ok(r)->sampling.top_p == std::optional<double>{1.0},
         "integer-valued numbers are numbers");
  const auto zero = parse(body_with(R"("temperature":0)"));
  expect(ok(zero) != nullptr && ok(zero)->sampling.temperature == std::optional<double>{0.0}, "temperature 0 is valid");
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"err_every_code_renders_fixed_body", err_every_code_renders_fixed_body},
      {"err_launcher_rows_carry_their_headers_and_option", err_launcher_rows_carry_their_headers_and_option},
      {"err_card_and_folder_rows_carry_their_reason", err_card_and_folder_rows_carry_their_reason},
      {"err_param_is_key_only", err_param_is_key_only},
      {"wire_response_has_security_headers_and_date", wire_response_has_security_headers_and_date},
      {"wire_response_carries_a_page_policy_once", wire_response_carries_a_page_policy_once},
      {"dos01_request_line_over_limit_is_414", dos01_request_line_over_limit_is_414},
      {"dos01_header_block_over_limit_is_431", dos01_header_block_over_limit_is_431},
      {"dos01_too_many_headers_is_431", dos01_too_many_headers_is_431},
      {"dos01_transfer_encoding_variants_are_411", dos01_transfer_encoding_variants_are_411},
      {"dos01_content_length_variants", dos01_content_length_variants},
      {"head_request_line_and_header_syntax", head_request_line_and_header_syntax},
      {"wire_head_has_no_content_length", wire_head_has_no_content_length},
      {"imf_fixdate_is_bounded_for_out_of_range_clocks", imf_fixdate_is_bounded_for_out_of_range_clocks},
      {"route_result_seam_returns_complete_response", route_result_seam_returns_complete_response},
      {"dos01_status_matrix_415_417_505", dos01_status_matrix_415_417_505},
      {"content_type_variants_accepted", content_type_variants_accepted},
      {"auth_and_route_order_follows_spec", auth_and_route_order_follows_spec},
      {"dos01_json_depth_5_is_400", dos01_json_depth_5_is_400},
      {"dos01_json_limits", dos01_json_limits},
      {"dos01_content_array_is_unsupported_parameter", dos01_content_array_is_unsupported_parameter},
      {"stream_flag_type_and_options_rules", stream_flag_type_and_options_rules},
      {"sampling_neutral_values_are_no_ops", sampling_neutral_values_are_no_ops},
      {"sampling_bounds_rejected", sampling_bounds_rejected},
      {"sampling_integer_forms_accepted", sampling_integer_forms_accepted},
  }, failures);
}
