#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/api_router.hpp"
#include "secure_frontend/config.hpp"
#include "secure_frontend/http_policy.hpp"
#include "secure_frontend/http_request_handler.hpp"
#include "secure_frontend/request_router.hpp"
#include "secure_frontend/secret_bytes.hpp"
#include "secure_frontend/token_verifier.hpp"
#include "support/test_support.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

constexpr std::string_view kHandlerToken = "handler-token-0123456789abcdefghijklmnop";
constexpr std::string_view kCorsOrigin = "https://console.example.test";
constexpr secure_frontend::HeadLimits kHandlerLimits{4096, 16384, 64, 65536};

secure_frontend::BearerTokenValidator verifier_validator(std::string_view token) {
  secure_frontend::SecretBytes secret;
  (void)secret.assign(token);
  return secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret)));
}

// Delegates authorize/handle to a real ApiRouter whose chat handler records
// the body it receives and answers 200. `handled` counts handle() calls.
class RecordingRouter final : public secure_frontend::RequestRouter {
 public:
  explicit RecordingRouter(secure_frontend::BearerTokenValidator validator = verifier_validator(kHandlerToken))
      : router_(secure_frontend::ApiRouterConfig{
            // The policy has no CORS switch: a configured cors_origins entry is the switch (pre-flight P10).
            .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address("127.0.0.1"), 8080, {},
                                                        {std::string{kCorsOrigin}}),
            .validator = std::move(validator),
            .model_id = "fake-model",
            .owned_by = "local",
            .chat_body_limit = 49152,
            .health = [] { return secure_frontend::HealthView{true, "ready", ""}; },
            .chat = [this](std::string_view body, const secure_frontend::RequestContext&) -> secure_frontend::RouteResult {
              body_ = std::string{body};
              return secure_frontend::HttpResponse{
                  .status = 200, .headers = {{"Content-Type", "application/json; charset=utf-8"}}, .body = "{}\n"};
            }}) {}

  // The chat lambda captures `this`: never copied or moved.
  RecordingRouter(const RecordingRouter&) = delete;
  RecordingRouter& operator=(const RecordingRouter&) = delete;
  RecordingRouter(RecordingRouter&&) = delete;
  RecordingRouter& operator=(RecordingRouter&&) = delete;
  ~RecordingRouter() override = default;

  secure_frontend::RouteDecision authorize(const secure_frontend::RequestHead& head) override { return router_.authorize(head); }
  secure_frontend::RouteResult handle(const secure_frontend::RequestHead& head, std::string_view body,
                                      const secure_frontend::RouteDecision& decision,
                                      const secure_frontend::RequestContext& context) override {
    ++handled_;
    return router_.handle(head, body, decision, context);
  }
  [[nodiscard]] int handled() const { return handled_; }
  [[nodiscard]] const std::optional<std::string>& body() const { return body_; }

 private:
  secure_frontend::ApiRouter router_;
  int handled_{0};
  std::optional<std::string> body_;
};

std::string bearer() { return "Authorization: Bearer " + std::string{kHandlerToken} + "\r\n"; }

void accepts_authorized_request() {
  RecordingRouter router;
  const auto response = secure_frontend::handle_http_request(
      "GET /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + bearer() + "\r\n", router, kHandlerLimits);
  expect(response.status == 200, "valid request should reach the router");
  expect(response.body == "{\"status\":\"ok\",\"engine\":\"ready\"}\n", "success response should come from the routed API");
  expect(router.handled() == 1, "handle is called exactly once");
}

void passes_the_bounded_raw_body_to_an_authorized_dispatcher() {
  RecordingRouter router;
  const std::string body = "{\"message\":\"hi\"}";
  const auto response = secure_frontend::handle_http_request(
      "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + bearer() +
          "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body,
      router, kHandlerLimits);
  expect(response.status == 200, "authorized POST should reach the chat handler");
  expect(router.body() == std::optional<std::string>{body}, "the chat handler receives the exact bounded body");
}

void rejects_malformed_and_ambiguous_messages() {
  RecordingRouter router;
  const auto missing_host = secure_frontend::handle_http_request("GET /x HTTP/1.1\r\n\r\n", router, kHandlerLimits);
  expect(missing_host.status == 400 &&
             missing_host.body == secure_frontend::render_error_body(secure_frontend::ApiError::invalid_request),
         "a request without Host is 400 invalid_request");
  for (const std::string& request : std::vector<std::string>{
           "GET / HTTP/1.1\nHost: 127.0.0.1:8080\n\n",
           "GET / HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nHost: evil.test\r\n" + bearer() + "\r\n",
           "GET /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + bearer() + "Content-Length: 2\r\n\r\nx",
       }) {
    const auto response = secure_frontend::handle_http_request(request, router, kHandlerLimits);
    expect(response.status == 400, "ambiguous or malformed HTTP must be rejected with 400");
  }
  // Spec §4.2 / §9.4: any Transfer-Encoding is 411 length_required (was the SP0 generic 400).
  const auto chunked = secure_frontend::handle_http_request(
      "GET / HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + bearer() + "Transfer-Encoding: chunked\r\n\r\n", router, kHandlerLimits);
  expect(chunked.status == 411, "a chunked request is rejected with 411");
  expect(router.handled() == 0, "no malformed request reaches handle");
}

void requires_one_well_formed_bearer_token() {
  RecordingRouter router;
  const std::string token{kHandlerToken};
  const std::pair<std::string, int> variants[] = {
      {"", 401}, {"Authorization: bearer " + token + "\r\n", 401}, {"Authorization: Bearer \r\n", 401},
      {"Authorization: Bearer\t" + token + "\r\n", 401}, {"Authorization: Basic " + token + "\r\n", 401},
      {"Authorization: Bearer " + token + "\r\nAuthorization: Bearer " + token + "\r\n", 400},
      {"Authorization: Bearer wrong-token-0123456789abcdefghijklmn\r\n", 401}};
  for (const auto& [headers, status] : variants) {
    const auto response = secure_frontend::handle_http_request(
        "GET /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + headers + "\r\n", router, kHandlerLimits);
    expect(response.status == status, "/health must answer " + std::to_string(status) + " for: " + headers);
    const auto code = status == 401 ? secure_frontend::ApiError::invalid_api_key : secure_frontend::ApiError::invalid_request;
    expect(response.body == secure_frontend::render_error_body(code), "§7.2 body for " + std::to_string(status));
  }
  expect(router.handled() == 0, "missing, invalid, or duplicate authorization never reaches handle");
}

void fails_closed_before_calling_validator_for_untrusted_origin() {
  int calls = 0;
  RecordingRouter router([&calls](std::string_view) { ++calls; return true; });
  const auto origin = secure_frontend::handle_http_request(
      "GET /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nOrigin: http://evil.example\r\n" + bearer() + "\r\n", router, kHandlerLimits);
  expect(origin.status == 403, "untrusted origin should be forbidden");
  const auto host = secure_frontend::handle_http_request(
      "GET /health HTTP/1.1\r\nHost: evil.test\r\n" + bearer() + "\r\n", router, kHandlerLimits);
  expect(host.status == 403, "untrusted host should be forbidden");
  expect(calls == 0, "an untrusted request must not expose a token to the validator");
  expect(router.handled() == 0, "nor reach handle");
}

void emits_cors_only_for_policy_approved_origin() {
  RecordingRouter router;
  const auto response = secure_frontend::handle_http_request(
      "GET /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nOrigin: " + std::string{kCorsOrigin} + "\r\n" + bearer() + "\r\n", router,
      kHandlerLimits);
  expect(response.status == 200, "configured CORS origin should be accepted");
  expect(response.header_value("Access-Control-Allow-Origin") == std::optional<std::string_view>{kCorsOrigin},
         "approved CORS origin must be emitted exactly");
  expect(response.header_value("Vary") == std::optional<std::string_view>{"Origin"}, "an echoed origin varies on Origin");

  const auto same_origin = secure_frontend::handle_http_request(
      "GET /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nOrigin: http://127.0.0.1:8080\r\n" + bearer() + "\r\n", router, kHandlerLimits);
  expect(same_origin.status == 200 && !same_origin.header_value("Access-Control-Allow-Origin").has_value(),
         "same-origin requests must not receive CORS headers");
  const auto none = secure_frontend::handle_http_request("GET /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + bearer() + "\r\n",
                                                         router, kHandlerLimits);
  expect(!none.header_value("Access-Control-Allow-Origin").has_value(), "no Origin, no CORS headers");
  for (const auto& r : {response, same_origin, none}) {
    expect(r.header_value("Access-Control-Allow-Origin") != std::optional<std::string_view>{"*"}, "never a wildcard origin");
  }
}

void handles_policy_approved_preflight_without_exposing_an_endpoint() {
  RecordingRouter router;
  const auto response = secure_frontend::handle_http_request(
      "OPTIONS /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nOrigin: " + std::string{kCorsOrigin} +
          "\r\nAccess-Control-Request-Method: POST\r\n\r\n",
      router, kHandlerLimits);
  expect(response.status == 204, "CORS preflight is not an authenticated API endpoint");
  expect(response.header_value("Access-Control-Allow-Origin") == std::optional<std::string_view>{kCorsOrigin},
         "preflight must only emit policy-approved CORS");

  const auto unsupported_method = secure_frontend::handle_http_request(
      "OPTIONS /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nOrigin: " + std::string{kCorsOrigin} +
          "\r\nAccess-Control-Request-Method: DELETE\r\n\r\n",
      router, kHandlerLimits);
  expect(unsupported_method.status == 403, "preflight must reject methods outside the CORS allowlist");
  expect(router.handled() == 0, "a preflight never reaches handle");
}

void constant_time_helper_matches_only_equal_tokens() {
  expect(secure_frontend::constant_time_token_equals("same", "same"), "equal tokens should match");
  expect(!secure_frontend::constant_time_token_equals("same", "different"), "unequal tokens should not match");
  expect(!secure_frontend::constant_time_token_equals("same", "sam"), "prefixes should not match");
  expect(!secure_frontend::constant_time_token_equals("", ""), "an empty expected token never matches");
}

void auth03_router_uses_verifier() {
  const std::string token = "router-token-0123456789abcdefghijklmnopqrs";
  RecordingRouter router(verifier_validator(token));
  const auto request = [](const std::string& presented) {
    return "GET /health HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nAuthorization: Bearer " + presented + "\r\n\r\n";
  };
  const auto good = secure_frontend::handle_http_request(request(token), router, kHandlerLimits);
  expect(good.status == 200 && router.handled() == 1, "the verifier's token is accepted through the router");
  const auto prefix = secure_frontend::handle_http_request(request(token.substr(0, token.size() - 1U)), router, kHandlerLimits);
  expect(prefix.status == 401 && router.handled() == 1, "a prefix is rejected and handle never runs");
  const auto longer = secure_frontend::handle_http_request(request(token + "s"), router, kHandlerLimits);
  expect(longer.status == 401 && router.handled() == 1, "a one-byte-longer value is rejected");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"accepts_authorized_request", accepts_authorized_request},
      {"passes_the_bounded_raw_body_to_an_authorized_dispatcher", passes_the_bounded_raw_body_to_an_authorized_dispatcher},
      {"rejects_malformed_and_ambiguous_messages", rejects_malformed_and_ambiguous_messages},
      {"requires_one_well_formed_bearer_token", requires_one_well_formed_bearer_token},
      {"fails_closed_before_calling_validator_for_untrusted_origin", fails_closed_before_calling_validator_for_untrusted_origin},
      {"emits_cors_only_for_policy_approved_origin", emits_cors_only_for_policy_approved_origin},
      {"handles_policy_approved_preflight_without_exposing_an_endpoint", handles_policy_approved_preflight_without_exposing_an_endpoint},
      {"constant_time_helper_matches_only_equal_tokens", constant_time_helper_matches_only_equal_tokens},
      {"auth03_router_uses_verifier", auth03_router_uses_verifier},
  }, failures);
}
