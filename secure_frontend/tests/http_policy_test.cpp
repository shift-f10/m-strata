#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/config.hpp"
#include "secure_frontend/http_policy.hpp"
#include "secure_frontend/http_request_head.hpp"
#include "support/test_support.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
using secure_frontend::PolicyVerdict;

secure_frontend::HttpPolicy policy_for(const char* host, std::vector<std::string> aliases = {},
                                       std::vector<std::string> cors = {}) {
  return secure_frontend::make_http_policy(*secure_frontend::parse_listener_address(host), 8080, aliases, cors);
}
PolicyVerdict host(const secure_frontend::HttpPolicy& p, std::vector<std::string_view> values) {
  return secure_frontend::check_host(p, values);
}

void web01_host_matrix() {
  const auto p = policy_for("127.0.0.1", {"strata.lan:8080"});
  expect(host(p, {"127.0.0.1:8080"}) == PolicyVerdict::allowed, "bound authority");
  expect(host(p, {"localhost:8080"}) == PolicyVerdict::allowed, "localhost alias on loopback (E10)");
  expect(host(p, {"strata.lan:8080"}) == PolicyVerdict::allowed, "configured alias");
  expect(host(p, {}) == PolicyVerdict::malformed, "missing Host is 400");
  expect(host(p, {"127.0.0.1:8080", "127.0.0.1:8080"}) == PolicyVerdict::malformed, "duplicate Host is 400");
  expect(host(p, {"evil.example:8080"}) == PolicyVerdict::forbidden, "unknown name is 403");
  expect(host(p, {"127.0.0.2:8080"}) == PolicyVerdict::forbidden, "other IP literal is 403");
  expect(host(p, {"127.0.0.1:8081"}) == PolicyVerdict::forbidden, "other port is 403");
  expect(host(p, {"127.0.0.1"}) == PolicyVerdict::forbidden, "missing port is 403");
  expect(host(p, {"localhost.:8080"}) == PolicyVerdict::malformed, "trailing dot is 400");
  expect(host(p, {"user@127.0.0.1:8080"}) == PolicyVerdict::malformed, "userinfo is 400");
  expect(host(p, {"127.0.0.1:8080\x01"}) == PolicyVerdict::malformed, "control byte is 400");
  expect(host(p, {"::1:8080"}) == PolicyVerdict::malformed, "unbracketed IPv6 is 400");
  const auto v6 = policy_for("::1");
  expect(host(v6, {"[::1]:8080"}) == PolicyVerdict::allowed, "bracketed bound IPv6 authority");
  expect(host(v6, {"[::1]"}) == PolicyVerdict::forbidden && host(v6, {"::1"}) == PolicyVerdict::malformed, "v6 without port");
  const auto wildcard = policy_for("0.0.0.0", {"box.lan:8080"});
  expect(host(wildcard, {"0.0.0.0:8080"}) == PolicyVerdict::forbidden, "a wildcard bind has no bound authority");
  expect(host(wildcard, {"localhost:8080"}) == PolicyVerdict::allowed, "a wildcard bind takes its own loopback names (2026-10-31)");
  expect(host(wildcard, {"box.lan:8080"}) == PolicyVerdict::allowed, "a wildcard bind trusts its aliases");
  expect(host(wildcard, {"other.lan:8080"}) == PolicyVerdict::forbidden, "and no other name");
}

void web02_host_name_case_is_folded() {
  const auto p = policy_for("127.0.0.1", {"Strata.LAN:8080"});
  expect(host(p, {"LOCALHOST:8080"}) == PolicyVerdict::allowed && host(p, {"Localhost:8080"}) == PolicyVerdict::allowed,
         "the ASCII case of a name is folded");
  expect(host(p, {"strata.lan:8080"}) == PolicyVerdict::allowed, "aliases are stored lower-cased");
  const auto origin = secure_frontend::check_origin(p, {"http://LOCALHOST:8080"});
  expect(origin.verdict == PolicyVerdict::forbidden, "origins are compared exactly (no folding)");
}

void web03_origin_matrix() {
  const auto p = policy_for("127.0.0.1", {}, {"http://app.example:3000"});
  const auto origin = [&p](std::vector<std::string_view> values) { return secure_frontend::check_origin(p, values); };
  expect(origin({}).verdict == PolicyVerdict::allowed && !origin({}).allow_origin, "no Origin is allowed, no CORS");
  expect(origin({"http://127.0.0.1:8080"}).verdict == PolicyVerdict::allowed && !origin({"http://127.0.0.1:8080"}).allow_origin,
         "same origin, no CORS headers");
  expect(origin({"http://localhost:8080"}).verdict == PolicyVerdict::allowed, "same origin through the localhost alias");
  const auto trusted = origin({"http://app.example:3000"});
  expect(trusted.verdict == PolicyVerdict::allowed && trusted.allow_origin == std::optional<std::string>{"http://app.example:3000"},
         "configured origin gets its own exact value");
  for (const std::string_view bad : {"null", "http://app.example.evil:3000", "http://APP.example:3000", "http://127.0.0.1:80",
                                     "https://127.0.0.1:8080", "http://127.0.0.1:8080/", "http://app.example:3000/",
                                     "http://127.0.0.1", "file://", "http://app.example:3000 "}) {
    expect(origin({bad}).verdict == PolicyVerdict::forbidden && !origin({bad}).allow_origin, "403 for origin " + std::string{bad});
  }
  expect(origin({"http://127.0.0.1:8080", "http://127.0.0.1:8080"}).verdict == PolicyVerdict::forbidden, "duplicate Origin is 403");
}

secure_frontend::RequestHead preflight(std::string origin, std::string method, std::vector<std::string> request_headers) {
  secure_frontend::RequestHead head;
  head.method = "OPTIONS";
  head.target = "/v1/chat/completions";
  head.fields.push_back({"Host", "127.0.0.1:8080"});
  head.fields.push_back({"Origin", std::move(origin)});
  head.fields.push_back({"Access-Control-Request-Method", std::move(method)});
  for (auto& value : request_headers) head.fields.push_back({"Access-Control-Request-Headers", std::move(value)});
  return head;
}

void web06_preflight_rules_unit() {
  const auto p = policy_for("127.0.0.1", {}, {"http://app.example:3000"});
  const auto ok = secure_frontend::evaluate_preflight(p, preflight("http://app.example:3000", "POST",
                                                                   {"Authorization, Content-Type", "X-Stainless-OS,x-stainless-lang"}));
  expect(ok.has_value() && ok->status == 204, "allowed origin, method and headers give 204");
  if (ok) {
    expect(ok->header_value("Access-Control-Allow-Origin") == std::optional<std::string_view>{"http://app.example:3000"}, "ACAO exact");
    expect(ok->header_value("Access-Control-Allow-Methods") == std::optional<std::string_view>{"GET, POST"}, "methods");
    expect(ok->header_value("Access-Control-Allow-Headers") == std::optional<std::string_view>{secure_frontend::kPreflightAllowHeaders}, "headers");
    expect(ok->header_value("Access-Control-Max-Age") == std::optional<std::string_view>{"600"}, "max age");
    expect(ok->header_value("Vary") == std::optional<std::string_view>{"Origin"}, "vary");
    expect(!ok->header_value("Access-Control-Allow-Credentials").has_value(), "never credentials");
  }
  const auto forbidden = [&p](secure_frontend::RequestHead head) {
    const auto r = secure_frontend::evaluate_preflight(p, head);
    return r.has_value() && r->status == 403 && !r->header_value("Access-Control-Allow-Origin").has_value();
  };
  expect(forbidden(preflight("http://127.0.0.1:8080", "POST", {})), "same origin is not a CORS origin");
  expect(forbidden(preflight("http://other.example:3000", "POST", {})), "unconfigured origin");
  expect(forbidden(preflight("http://app.example:3000", "DELETE", {})), "other method");
  expect(forbidden(preflight("http://app.example:3000", "post", {})), "method is case-sensitive");
  expect(forbidden(preflight("http://app.example:3000", "POST", {"x-api-key"})), "header outside the list");
  secure_frontend::RequestHead plain = preflight("http://app.example:3000", "POST", {});
  plain.fields.pop_back();   // no Access-Control-Request-Method: not a preflight
  expect(!secure_frontend::evaluate_preflight(p, plain).has_value(), "OPTIONS without ACRM is not a preflight");
}

// The owner, 2026-10-31, on a VM ("I am getting request origin not allowed"): a server bound to every address, reached by
// its own TUI or window at 127.0.0.1:<port> (window_shell's listening_origin), refused them - it trusted only the aliases -
// and an alias written without its port matched no browser's Host. A wildcard bind also takes its own loopback names, and
// an alias without a port also takes the listener's port; any other name is still refused (DNS rebinding still fails).
void a_wildcard_bind_takes_its_loopback_names_and_bare_aliases() {
  const auto wildcard = policy_for("0.0.0.0", {"vm.example.test", "192.0.2.10"});
  for (const std::string_view ok : {"127.0.0.1:8080", "localhost:8080", "vm.example.test:8080", "vm.example.test", "192.0.2.10:8080",
                                    "192.0.2.10"}) {
    expect(host(wildcard, {ok}) == PolicyVerdict::allowed, "allowed: " + std::string{ok});
  }
  for (const std::string_view bad : {"evil.example:8080", "127.0.0.1:8081", "vm.example.test:9090", "0.0.0.0:8080", "[::1]:8080"}) {
    expect(host(wildcard, {bad}) == PolicyVerdict::forbidden, "refused: " + std::string{bad});
  }
  const auto origin = [&wildcard](const std::string_view value) { return secure_frontend::check_origin(wildcard, {value}).verdict; };
  expect(origin("http://127.0.0.1:8080") == PolicyVerdict::allowed && origin("http://vm.example.test:8080") == PolicyVerdict::allowed &&
             origin("http://192.0.2.10:8080") == PolicyVerdict::allowed && origin("http://evil.example:8080") == PolicyVerdict::forbidden,
         "the same origins, and no other");
  const auto v6 = policy_for("::", {"[2001:db8::10]"});
  expect(host(v6, {"[::1]:8080"}) == PolicyVerdict::allowed && host(v6, {"localhost:8080"}) == PolicyVerdict::allowed &&
             host(v6, {"[2001:db8::10]:8080"}) == PolicyVerdict::allowed && host(v6, {"127.0.0.1:8080"}) == PolicyVerdict::forbidden,
         "an IPv6 wildcard: [::1], localhost, and the bare alias on the port");
  const auto bound = policy_for("192.0.2.10", {"vm.example.test"});
  expect(host(bound, {"vm.example.test:8080"}) == PolicyVerdict::allowed && host(bound, {"localhost:8080"}) == PolicyVerdict::forbidden,
         "a bare alias takes the port on any bind; loopback names only on a loopback or wildcard bind");
}

void cors_and_alias_grammar() {
  for (const char* good : {"http://app.example:3000", "https://app.example", "http://127.0.0.1:5173", "http://[::1]:5173"}) {
    expect(secure_frontend::is_valid_cors_origin(good), std::string{"valid origin "} + good);
  }
  for (const char* bad : {"*", "null", "http://app.example/", "http://App.example", "ftp://x", "http://x:0", "http://", "app.example"}) {
    expect(!secure_frontend::is_valid_cors_origin(bad), std::string{"invalid origin "} + bad);
  }
  for (const char* good : {"strata.lan:8080", "strata.lan", "172.16.1.5:8080", "[fe80::1]:8080"}) {
    expect(secure_frontend::is_valid_host_alias(good), std::string{"valid alias "} + good);
  }
  for (const char* bad : {"", "http://x", "x:", "x:99999", "a b", "x.:80", "user@x"}) {
    expect(!secure_frontend::is_valid_host_alias(bad), std::string{"invalid alias "} + bad);
  }
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"web01_host_matrix", web01_host_matrix},
      {"web02_host_name_case_is_folded", web02_host_name_case_is_folded},
      {"web03_origin_matrix", web03_origin_matrix},
      {"web06_preflight_rules_unit", web06_preflight_rules_unit},
      {"cors_and_alias_grammar", cors_and_alias_grammar},
      {"a_wildcard_bind_takes_its_loopback_names_and_bare_aliases", a_wildcard_bind_takes_its_loopback_names_and_bare_aliases},
  }, failures);
}
