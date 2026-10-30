#include "secure_frontend/config_defaults.hpp"
#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/request_router.hpp"
#include "secure_frontend/socket_io.hpp"
#include "secure_frontend/web_ui.hpp"
#include "support/chat_json.hpp"
#include "support/stream_client.hpp"
#include "support/connection_scan.hpp"
#include "support/raw_http.hpp"
#include "support/server_harness.hpp"
#include "support/test_paths.hpp"
#include "support/test_support.hpp"
#include "support/test_tokenizer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

// SP3 DOS-01 / AUTH-01 / §4.6 over TCP against the real strata-unified-server
// (spec §9.3). Every case starts its own server on an ephemeral port.
namespace {
using namespace std::chrono_literals;
using secure_frontend::ApiError;
using secure_frontend::ChatExchangeEnd;
int failures = 0;
void expect(bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

std::unique_ptr<test_support::ServerProcess> start_server(std::map<std::string, std::string> literals = {},
                                                         std::vector<std::string> engine_args = {}) {
  test_support::ServerOptions options;
  options.config_literals = std::move(literals);
  options.engine_args = std::move(engine_args);
  auto server = test_support::ServerProcess::start(options);
  const bool ready = server != nullptr && server->wait_until_listening(10s) && server->wait_for_health(test_support::kHealthReady, 15s);
  expect(ready, "server ready" + (server ? std::string{": "} + server->stderr_text() : std::string{}));
  return ready ? std::move(server) : nullptr;
}
std::string authority(const test_support::ServerProcess& s) { return "127.0.0.1:" + std::to_string(s.port()); }
std::string token() { return std::string{test_support::kHarnessToken}; }
std::string chat_body(const std::string& extra = R"("max_tokens":2)", const std::string& content = "Hi") {
  return R"({"model":"fake-model","messages":[{"role":"user","content":")" + content + R"("}])" +
         (extra.empty() ? "" : "," + extra) + "}";
}
std::string chat_request(const test_support::ServerProcess& s, const std::string& body, std::vector<std::string> extra = {}) {
  extra.insert(extra.begin(), {raw_http::bearer(token()), "Content-Type: application/json"});
  return raw_http::request("POST", "/v1/chat/completions", authority(s), extra, body);
}
std::optional<raw_http::Response> raw(const test_support::ServerProcess& s, const std::string& request,
                                      std::chrono::milliseconds timeout = 10s) {
  return raw_http::exchange("127.0.0.1", s.port(), request, timeout);
}
// record_of / count_of: shared helpers (pre-flight P35) for the later cases of Tasks 10, 12, 16, 17 and 19-22.
[[maybe_unused]] std::string record_of(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
[[maybe_unused]] std::size_t count_of(const std::string& text, std::string_view needle) {
  std::size_t n = 0;
  for (std::size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + 1U)) ++n;
  return n;
}
// Pre-flight D2: the "retry /health on <ip> up to 100 x 100 ms" loop shared by
// web01_ipv6_bound_host_rules (Task 10) and net01_ipv6_loopback_listener_only (Task 12).
std::optional<raw_http::Response> wait_health_on(std::string_view ip, std::uint16_t port, const std::string& host) {
  std::optional<raw_http::Response> r;
  for (int i = 0; i < 100; ++i) {
    r = raw_http::exchange(ip, port, raw_http::request("GET", "/health", host, {raw_http::bearer(token())}));
    if (r && r->status == 200) break;
    std::this_thread::sleep_for(100ms);
  }
  return r;
}
bool has_no_cors_headers(const raw_http::Response& r) {
  return !r.header("Access-Control-Allow-Origin") && !r.header("Access-Control-Allow-Credentials");
}
// One authenticated request for a kRouteTable entry (prefix routes get the served id).
std::string route_request(const secure_frontend::RouteEntry& e, const std::string& host, std::vector<std::string> extra = {}) {
  extra.insert(extra.begin(), raw_http::bearer(token()));
  const std::string target = e.prefix ? std::string{e.path} + "fake-model" : std::string{e.path};
  if (e.method == "POST") {
    extra.push_back("Content-Type: application/json");
    return raw_http::request("POST", target, host, extra, chat_body());
  }
  return raw_http::request(e.method, target, host, extra);
}

void dos01_body_over_limit_is_413_before_reading() {
  auto server = start_server();
  if (!server) return;
  auto c = raw_http::Connection::open("127.0.0.1", server->port());
  const std::string head = "POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) + "\r\n" + raw_http::bearer(token()) +
                           "\r\nContent-Type: application/json\r\nContent-Length: " +
                           std::to_string(secure_frontend::config_defaults::kChatMaxBodyBytes + 1) + "\r\n\r\n";
  const auto started = std::chrono::steady_clock::now();
  expect(c && c->send(head), "header only");
  const auto r = raw_http::parse_response(c ? c->read_until_close(5s) : "");
  expect(r && r->status == 413 && r->body == secure_frontend::render_error_body(ApiError::request_too_large),
         "one byte over max_body_bytes (the default, upstream's scale since 2026-10-29): 413 after auth, body never read");
  expect(std::chrono::steady_clock::now() - started < 1s, "answered at once");
}

void dos01_content_length_over_request_limit_is_413_before_auth() {
  auto server = start_server();
  if (!server) return;
  auto c = raw_http::Connection::open("127.0.0.1", server->port());
  const auto started = std::chrono::steady_clock::now();
  expect(c && c->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) + "\r\nContent-Length: " +
                      std::to_string(secure_frontend::config_defaults::kMaxRequestBytes + 1) + "\r\n\r\n"),
         "no token, no body");
  const auto r = raw_http::parse_response(c ? c->read_until_close(5s) : "");
  expect(r && r->status == 413, "413 (a global limit), not 401");
  expect(std::chrono::steady_clock::now() - started < 100ms + 400ms, "within 100 ms plus scheduling slack");
}

void dos01_unauthenticated_gets_401_before_body_is_read() {
  auto server = start_server();
  if (!server) return;
  auto c = raw_http::Connection::open("127.0.0.1", server->port());
  const auto started = std::chrono::steady_clock::now();
  expect(c && c->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) +
                      "\r\nAuthorization: Bearer wrong-token-0123456789abcdefghijklmnop\r\nContent-Type: application/json\r\n"
                      "Content-Length: 40000\r\n\r\n"), "wrong token, body withheld");
  const auto r = raw_http::parse_response(c ? c->read_until_close(5s) : "");
  expect(r && r->status == 401 && r->header("WWW-Authenticate") == std::optional<std::string>{"Bearer"}, "401 before any body byte");
  expect(std::chrono::steady_clock::now() - started < 500ms, "within 100 ms plus slack");
}

void dos01_expect_continue_after_auth_only() {
  auto server = start_server();
  if (!server) return;
  const std::string body = chat_body();
  auto bad = raw_http::Connection::open("127.0.0.1", server->port());
  expect(bad && bad->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) +
                          "\r\nAuthorization: Bearer nope-0123456789abcdefghijklmnopqrstuv\r\nContent-Type: application/json\r\n"
                          "Expect: 100-continue\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n"), "bad token");
  const std::string refused = bad ? bad->read_until_close(5s) : "";
  expect(refused.find("100 Continue") == std::string::npos && refused.rfind("HTTP/1.1 401", 0) == 0, "no 100 before auth");
  auto good = raw_http::Connection::open("127.0.0.1", server->port());
  expect(good && good->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) + "\r\n" + raw_http::bearer(token()) +
                            "\r\nContent-Type: application/json\r\nExpect: 100-continue\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n"), "valid token");
  expect(good && good->read_for(500ms) == "HTTP/1.1 100 Continue\r\n\r\n", "100 after auth");
  expect(good && good->send(body), "body");
  const auto r = raw_http::parse_response(good ? good->read_until_close(10s) : "");
  expect(r && r->status == 200, "the chat completes");
}

void dos01_slow_header_is_408_at_header_deadline() {
  auto server = start_server({{"limits.http.header_deadline_ms", "1000"}});
  if (!server) return;
  auto c = raw_http::Connection::open("127.0.0.1", server->port());
  const auto started = std::chrono::steady_clock::now();
  expect(c && c->send("GET /health HTTP/1.1\r\nHost: "), "partial header");
  const auto r = raw_http::parse_response(c ? c->read_until_close(5s) : "");
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(r && r->status == 408 && r->body == secure_frontend::render_error_body(ApiError::request_timeout), "408 request_timeout");
  expect(elapsed >= 900ms && elapsed < 2s, "at the 1 s header deadline");
}

void dos01_slow_body_is_408_at_request_deadline() {
  auto server = start_server({{"limits.http.header_deadline_ms", "1000"}, {"limits.http.request_deadline_ms", "2000"}});
  if (!server) return;
  auto c = raw_http::Connection::open("127.0.0.1", server->port());
  const auto started = std::chrono::steady_clock::now();
  const std::string body = chat_body();
  expect(c && c->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) + "\r\n" + raw_http::bearer(token()) +
                      "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
                      body.substr(0, 5)), "valid token, partial body");
  const auto r = raw_http::parse_response(c ? c->read_until_close(5s) : "");
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(r && r->status == 408, "408 for an authenticated slow body");
  expect(elapsed >= 1900ms && elapsed < 3s, "at the 2 s request deadline");
}

void dos01_incomplete_body_then_close_frees_worker() {
  auto server = start_server();
  if (!server) return;
  {
    auto c = raw_http::Connection::open("127.0.0.1", server->port());
    expect(c && c->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) + "\r\n" + raw_http::bearer(token()) +
                        "\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{\"model\""), "partial body");
  }   // closed here
  const auto started = std::chrono::steady_clock::now();
  const auto r = raw(*server, chat_request(*server, chat_body()));
  expect(r && r->status == 200 && std::chrono::steady_clock::now() - started < 2s, "the next client is served at once");
}

void err_unauthenticated_is_401_on_every_path_and_method() {
  auto server = start_server();
  if (!server) return;
  for (const char* method : {"GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS", "HEAD"}) {
    for (const char* target : {"/health", "/v1/models", "/v1/models/fake-model", "/v1/chat/completions", "/v1/completions", "/x"}) {
      const std::string request = std::string{method} + " " + target + " HTTP/1.1\r\nHost: " + authority(*server) +
                                  (std::string{method} == "POST" || std::string{method} == "PUT" ? "\r\nContent-Length: 0" : "") + "\r\n\r\n";
      const auto r = raw(*server, request);
      expect(r && r->status == 401 && r->body == secure_frontend::render_error_body(ApiError::invalid_api_key),
             std::string{method} + " " + target + " is 401 without a token");
    }
  }
}

void security_headers_on_every_response() {
  auto server = start_server();
  if (!server) return;
  const std::vector<std::string> requests{
      raw_http::request("GET", "/health", authority(*server), {raw_http::bearer(token())}),
      raw_http::request("GET", "/v1/models", authority(*server), {raw_http::bearer(token())}),
      raw_http::request("GET", "/nope", authority(*server), {raw_http::bearer(token())}),
      raw_http::request("GET", "/health", authority(*server), {}),
      raw_http::request("GET", "/health", "evil:1", {raw_http::bearer(token())}),
      chat_request(*server, chat_body()),
      chat_request(*server, "{"),
      "GET /health HTTP/1.0\r\nHost: " + authority(*server) + "\r\n\r\n"};
  for (const auto& request : requests) {
    const auto r = raw(*server, request);
    expect(r.has_value(), "response for " + request.substr(0, request.find('\r')));
    if (!r) continue;
    for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
             {"Connection", "close"}, {"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"},
             {"Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'"}, {"Referrer-Policy", "no-referrer"},
             {"Cross-Origin-Resource-Policy", "same-origin"}}) {
      expect(r->header(name) == std::optional<std::string>{value}, name + " on " + std::to_string(r->status));
    }
    expect(r->header("Date").has_value() && !r->header("Server") && !r->header("X-Powered-By"), "Date, no Server, no X-Powered-By");
  }
}

// SP3 WEB-01..06 / AUTH-01 / CFG-01 over TCP (spec §4.3, §4.4, §9.3).
void web01_bad_host_rejected_on_every_route() {
  auto server = start_server();
  if (!server) return;
  const std::string port = std::to_string(server->port());
  const std::vector<std::pair<std::string, int>> hosts{
      {"evil.example:" + port, 403}, {"127.0.0.2:" + port, 403}, {"127.0.0.1:1", 403}, {"127.0.0.1", 403},
      {"localhost.:" + port, 400}, {"user@127.0.0.1:" + port, 400}, {"::1:" + port, 400}};
  for (const auto& entry : secure_frontend::kRouteTable) {
    for (const auto& [host, status] : hosts) {
      const auto r = raw(*server, route_request(entry, host));
      expect(r && r->status == status && r->body.find("fake-model") == std::string::npos && r->body.find("\"status\"") == std::string::npos,
             std::string{entry.path} + " with Host " + host + " is " + std::to_string(status) + " and carries no route data");
    }
    const auto twice = raw(*server, route_request(entry, authority(*server), {"Host: " + authority(*server)}));
    expect(twice && twice->status == 400, std::string{entry.path} + " with two Host fields is 400");
  }
}

void web01_ipv6_bound_host_rules() {
  if (!test_support::ipv6_loopback_available()) test_support::skip("no IPv6 loopback on this host");
  test_support::ServerOptions options;
  options.config_literals = {{"listener.host", "\"::1\""}};
  auto server = test_support::ServerProcess::start(options);
  expect(server != nullptr, "server starts on ::1");
  if (!server) return;
  const std::string port = std::to_string(server->port());
  const auto ok = wait_health_on("::1", server->port(), "[::1]:" + port);
  expect(ok && ok->status == 200, "[::1]:port is the bound authority" + (ok ? std::string{} : ": " + server->stderr_text()));
  const auto unbracketed = raw_http::exchange("::1", server->port(), raw_http::request("GET", "/health", "::1", {raw_http::bearer(token())}));
  expect(unbracketed && unbracketed->status == 400, "::1 is malformed");
  const auto no_port = raw_http::exchange("::1", server->port(), raw_http::request("GET", "/health", "[::1]", {raw_http::bearer(token())}));
  expect(no_port && no_port->status == 403, "[::1] without the port is not allowed");
  const auto local = raw_http::exchange("::1", server->port(), raw_http::request("GET", "/health", "localhost:" + port, {raw_http::bearer(token())}));
  expect(local && local->status == 200, "localhost:port is allowed on a loopback bind (E10)");
}

void web02_bound_authority_and_localhost_alias_succeed() {
  // Session log step 16: /metrics is an opt-in route (listener.monitor); on here, so every
  // route in the table is exercised under every authority.
  auto server = start_server({{"listener.monitor", "true"}});
  if (!server) return;
  const std::string port = std::to_string(server->port());
  for (const std::string& host : std::vector<std::string>{authority(*server), "localhost:" + port, "LOCALHOST:" + port}) {
    for (const auto& entry : secure_frontend::kRouteTable) {
      const auto r = raw(*server, route_request(entry, host));
      expect(r && r->status == 200, std::string{entry.path} + " succeeds with Host " + host);
    }
  }
}

void web02_host_alias_from_config_succeeds() {
  auto server = start_server({{"listener.host_aliases", "[\"strata.lan:8080\",\"box.lan\"]"}});
  if (!server) return;
  for (const std::string host : {"strata.lan:8080", "box.lan"}) {
    const auto r = raw(*server, raw_http::request("GET", "/health", host, {raw_http::bearer(token())}));
    expect(r && r->status == 200, "configured alias " + host);
  }
  const auto other = raw(*server, raw_http::request("GET", "/health", "strata.lan:8081", {raw_http::bearer(token())}));
  expect(other && other->status == 403, "the alias is exact (other port refused)");
}

void web03_untrusted_origin_no_cors_header() {
  auto server = start_server({{"listener.cors_origins", "[\"http://app.example:3000\"]"}});
  if (!server) return;
  const std::string port = std::to_string(server->port());
  for (const std::string& origin : std::vector<std::string>{"null", "http://app.example.evil:3000", "http://APP.example:3000",
                                                            "http://127.0.0.1:80", "https://127.0.0.1:" + port, "http://app.example:3000/"}) {
    const auto r = raw(*server, raw_http::request("GET", "/health", authority(*server), {raw_http::bearer(token()), "Origin: " + origin}));
    expect(r && r->status == 403 && has_no_cors_headers(*r), "origin " + origin + " is 403 without CORS headers");
  }
  const auto dup = raw(*server, raw_http::request("GET", "/health", authority(*server),
                                                  {raw_http::bearer(token()), "Origin: http://app.example:3000", "Origin: http://app.example:3000"}));
  expect(dup && dup->status == 403 && has_no_cors_headers(*dup), "duplicate Origin is 403");
}

void web04_default_emits_no_cors_header_anywhere() {
  const auto dir = test_support::make_temp_dir("web04");
  auto server = start_server({}, {"--record-file=" + test_support::utf8_path(dir / "record.txt")});
  if (!server) return;
  const std::string a = authority(*server);
  const std::string origin = "Origin: http://" + a;
  const std::vector<std::string> requests{
      raw_http::request("GET", "/health", a, {raw_http::bearer(token()), origin}),
      raw_http::request("GET", "/v1/models", a, {raw_http::bearer(token()), origin}),
      raw_http::request("GET", "/v1/models/fake-model", a, {raw_http::bearer(token()), origin}),
      raw_http::request("GET", "/v1/models/other", a, {raw_http::bearer(token()), origin}),
      chat_request(*server, chat_body(), {origin}),
      chat_request(*server, "{", {origin}),
      chat_request(*server, chat_body(R"("tools":[])"), {origin}),
      raw_http::request("GET", "/nope", a, {raw_http::bearer(token()), origin}),
      raw_http::request("DELETE", "/health", a, {raw_http::bearer(token()), origin}),
      raw_http::request("GET", "/health", a, {origin}),
      raw_http::request("GET", "/health", "evil:1", {origin}),
      raw_http::request("GET", "/health", a, {raw_http::bearer(token()), "Origin: http://evil.example"}),
      raw_http::request("OPTIONS", "/v1/chat/completions", a, {"Origin: http://evil.example", "Access-Control-Request-Method: POST"}),
      raw_http::request("POST", "/v1/chat/completions", a, {raw_http::bearer(token()), "Content-Type: text/plain"}, "{}"),
      raw_http::request("POST", "/v1/chat/completions", a, {raw_http::bearer(token()), "Transfer-Encoding: chunked"}),
      raw_http::request("POST", "/v1/chat/completions", a, {raw_http::bearer(token()), "Content-Type: application/json", "Expect: x"}, "{}"),
      "GET /health HTTP/1.0\r\nHost: " + a + "\r\n\r\n",
      "GET /" + std::string(5000, 'a') + " HTTP/1.1\r\nHost: " + a + "\r\n\r\n",
      raw_http::request("GET", "/health", a, {"X-Fill: " + std::string(17000, 'f')}),
      // over the request limit (256 MiB since 2026-10-29): only its header is sent, the 413 comes before auth
      "POST /v1/chat/completions HTTP/1.1\r\nHost: " + a + "\r\nContent-Length: " +
          std::to_string(secure_frontend::config_defaults::kMaxRequestBytes + 1) + "\r\n\r\n"};
  std::set<int> statuses;
  for (const auto& request : requests) {
    const auto r = raw(*server, request);
    if (!r) { expect(false, "no response for " + request.substr(0, 40)); continue; }
    statuses.insert(r->status);
    expect(has_no_cors_headers(*r), "no CORS header on a " + std::to_string(r->status));
  }
  for (const int s : {200, 400, 401, 403, 404, 405, 411, 413, 414, 415, 417, 431, 505}) {
    expect(statuses.count(s) == 1U, "status " + std::to_string(s) + " was exercised");
  }
  const auto sse = test_support::chat(*server, test_support::chat_body(R"("stream":true)"),
                                      {.extra_headers = {{"Origin", "http://127.0.0.1:" + std::to_string(server->port())}}});
  expect(sse.streamed && !sse.head.header("Access-Control-Allow-Origin") && !sse.head.header("Vary"), "no CORS header on an SSE response");
}

void web05_only_exact_configured_origin_allowed() {
  auto server = start_server({{"listener.cors_origins", "[\"http://app.example:3000\"]"}});
  if (!server) return;
  const auto ok = raw(*server, raw_http::request("GET", "/v1/models", authority(*server), {raw_http::bearer(token()), "Origin: http://app.example:3000"}));
  expect(ok && ok->status == 200 && ok->header("Access-Control-Allow-Origin") == std::optional<std::string>{"http://app.example:3000"} &&
         ok->header("Vary") == std::optional<std::string>{"Origin"} && !ok->header("Access-Control-Allow-Credentials"),
         "the configured origin gets its exact value, Vary, never credentials");
  const auto other = raw(*server, raw_http::request("GET", "/v1/models", authority(*server), {raw_http::bearer(token()), "Origin: http://other.example:3000"}));
  expect(other && other->status == 403 && has_no_cors_headers(*other), "another origin is never reflected");
  const auto same = raw(*server, raw_http::request("GET", "/v1/models", authority(*server), {raw_http::bearer(token()), "Origin: http://" + authority(*server)}));
  expect(same && same->status == 200 && has_no_cors_headers(*same), "same origin needs no CORS headers");
}

void web06_preflight_matrix() {
  const auto dir = test_support::make_temp_dir("web06p");
  auto server = start_server({{"listener.cors_origins", "[\"http://app.example:3000\"]"}},
                             {"--record-file=" + test_support::utf8_path(dir / "record.txt")});
  if (!server) return;
  const std::string a = authority(*server);
  const auto preflight = [&](const std::string& origin, const std::string& method, const std::string& headers) {
    std::vector<std::string> lines{"Origin: " + origin, "Access-Control-Request-Method: " + method};
    if (!headers.empty()) lines.push_back("Access-Control-Request-Headers: " + headers);
    return raw(*server, raw_http::request("OPTIONS", "/v1/chat/completions", a, lines));
  };
  for (const std::string method : {"GET", "POST"}) {
    for (const std::string headers : {"", "authorization", "authorization, content-type",
                                      "Authorization,Content-Type,X-Stainless-OS,x-stainless-lang,x-stainless-arch,x-stainless-runtime,"
                                      "x-stainless-runtime-version,x-stainless-package-version,x-stainless-retry-count,x-stainless-timeout"}) {
      const auto r = preflight("http://app.example:3000", method, headers);
      expect(r && r->status == 204 && r->header("Access-Control-Allow-Origin") == std::optional<std::string>{"http://app.example:3000"} &&
             r->header("Access-Control-Allow-Methods") == std::optional<std::string>{"GET, POST"} &&
             r->header("Access-Control-Max-Age") == std::optional<std::string>{"600"},
             "allowed preflight " + method + " [" + headers + "] needs no token");
    }
  }
  for (const auto& [origin, method, headers] : std::vector<std::tuple<std::string, std::string, std::string>>{
           {"http://evil.example", "POST", ""}, {"http://app.example:3000", "PUT", ""}, {"http://app.example:3000", "POST", "x-api-key"},
           {"http://" + a, "POST", ""}, {"null", "POST", ""}}) {
    const auto r = preflight(origin, method, headers);
    expect(r && r->status == 403 && has_no_cors_headers(*r), "refused preflight " + origin + " " + method + " " + headers);
  }
  expect(record_of(dir / "record.txt").find("GEN") == std::string::npos, "no preflight reached a route");
}

void web06_cross_origin_post_without_trust_rejected() {
  const auto dir = test_support::make_temp_dir("web06");
  auto server = start_server({}, {"--record-file=" + test_support::utf8_path(dir / "record.txt")});
  if (!server) return;
  const auto r = raw(*server, chat_request(*server, chat_body(), {"Origin: http://evil.example"}));
  expect(r && r->status == 403 && has_no_cors_headers(*r), "a valid token from a foreign origin is 403");
  expect(record_of(dir / "record.txt").find("GEN") == std::string::npos, "the engine received no GEN");
}

void auth01_every_route_rejects_missing_malformed_wrong_token() {
  auto server = start_server();
  if (!server) return;
  const std::pair<std::string, int> variants[] = {
      {"", 401}, {"Authorization: bearer " + token(), 401}, {"Authorization: Bearer ", 401}, {"Authorization: Basic " + token(), 401},
      {"Authorization: Bearer wrong-token-0123456789abcdefghijklmn", 401},
      {"Authorization: Bearer " + token() + "\r\nAuthorization: Bearer " + token(), 400}};
  for (const auto& entry : secure_frontend::kRouteTable) {
    for (const auto& [line, status] : variants) {
      std::vector<std::string> lines;
      if (!line.empty()) lines.push_back(line);
      const std::string target = entry.prefix ? std::string{entry.path} + "fake-model" : std::string{entry.path};
      if (entry.method == "POST") lines.push_back("Content-Type: application/json");
      const auto r = raw(*server, raw_http::request(entry.method, target, authority(*server), lines, entry.method == "POST" ? chat_body() : ""));
      expect(r && r->status == status && r->body.find("fake-model") == std::string::npos,
             std::string{entry.method} + " " + target + " -> " + std::to_string(status) + " for [" + line.substr(0, 30) + "]");
    }
  }
  for (const std::string_view bad_token : {std::string_view(""), std::string_view("wrong-token-wrong-token-wrong-token-xx")}) {
    const auto result = secure_frontend::post_chat_completion(test_support::loopback_v4(), server->port(), bad_token,
                                                              test_support::chat_body(R"("stream":true)"));
    expect(result.head.status == 401 && !result.streamed, "stream:true without a valid token is 401, never a stream");
  }
}

// CFG-01 / NET-03, process half (plan note 12): the validation half is typed_config_tests (Task 4).
void cfg01_wildcard_bind_requires_host_aliases() {
  const auto dir = test_support::make_temp_dir("wild");
  test_support::ServerOptions options;
  options.config_literals = {{"listener.host", "\"0.0.0.0\""}, {"listener.allow_remote", "true"}, {"listener.allow_wildcard_bind", "true"}};
  options.engine_args = {"--record-file=" + test_support::utf8_path(dir / "record.txt")};
  auto server = test_support::ServerProcess::start(options);
  expect(server != nullptr, "the process starts");
  if (!server) return;
  const auto code = server->wait_exit(10s);
  expect(code.has_value() && *code != 0, "a wildcard bind without listener.host_aliases fails validation");
  expect(server->stderr_text().find("listener.host_aliases") != std::string::npos, "the error names the key: " + server->stderr_text());
  expect(!std::filesystem::exists(dir / "record.txt"), "no engine was spawned (the fake creates its record file at start)");
}

// DOS-01 / §2.3: eight slow-header clients hold all eight workers in P1; a ninth
// connection evicts the oldest of them and is answered anyway (spec §2.3).
void dos01_eviction_over_tcp_admits_new_client() {
  auto server = start_server({{"limits.http.header_deadline_ms", "30000"}});
  if (!server) return;
  std::vector<raw_http::Connection> holders;
  for (int i = 0; i < 8; ++i) {
    auto c = raw_http::Connection::open("127.0.0.1", server->port());
    expect(c && c->send("GET /health HTTP/1.1\r\nHost: "), "slow-header client " + std::to_string(i));
    if (c) holders.push_back(std::move(*c));
    std::this_thread::sleep_for(20ms);
  }
  std::this_thread::sleep_for(200ms);   // all eight workers now sit in P1
  const auto r = raw(*server, raw_http::request("GET", "/health", authority(*server), {raw_http::bearer(token())}), 5s);
  expect(r && r->status == 200, "a ninth client completes while eight workers are held in P1");
  bool someone_closed = false;
  for (auto& h : holders) someone_closed = someone_closed || h.wait_closed(100ms);
  expect(someone_closed, "one P1 client was evicted (closed without a response)");
}

// DOS-01 / §2.3: 16 one-byte-per-second header clients can never hold every
// worker: eviction cycles through them and a real chat completes promptly.
void dos01_slowloris_cannot_starve_a_real_client() {
  auto server = start_server({{"limits.http.header_deadline_ms", "30000"}});
  if (!server) return;
  std::atomic<bool> stop{false};
  std::vector<std::thread> lorises;
  for (int i = 0; i < 16; ++i) {
    lorises.emplace_back([&stop, port = server->port()] {
      while (!stop) {
        auto c = raw_http::Connection::open("127.0.0.1", port);
        if (!c) continue;
        const std::string head = "GET /health HTTP/1.1\r\nHost: x\r\nX-Slow: ";
        for (std::size_t i = 0; i < head.size() && !stop; ++i) {
          if (!c->send(std::string_view{head}.substr(i, 1U))) break;
          std::this_thread::sleep_for(1s);
        }
      }
    });
  }
  std::this_thread::sleep_for(500ms);
  const auto started = std::chrono::steady_clock::now();
  const auto r = raw(*server, chat_request(*server, chat_body()), 20s);
  expect(r && r->status == 200, "a real chat completes under 16 one-byte-per-second header clients");
  expect(std::chrono::steady_clock::now() - started < 10s, "and promptly");
  stop = true;
  for (auto& t : lorises) t.join();
}

// DOS-01 / §2.2 + §2.3: an unauthenticated slow-body client never reaches P3 —
// it is answered at once with 401 (auth before the body, Task 9) or, while all
// eight workers drain a previous 401, refused with 503 server_busy at accept.
// Neither can starve a real client: every dribbler answer is instant and the
// real chat completes (Retry-After: 1 is honored by the retry loop).
void dos01_slow_body_clients_cannot_starve_a_real_client() {
  auto server = start_server();
  if (!server) return;
  std::vector<raw_http::Connection> dribblers;
  int fortyones = 0;
  for (int i = 0; i < 16; ++i) {
    auto c = raw_http::Connection::open("127.0.0.1", server->port());
    const auto started = std::chrono::steady_clock::now();
    expect(c && c->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) +
                        "\r\nAuthorization: Bearer nope-0123456789abcdefghijklmnopqrstuvw\r\nContent-Type: application/json\r\n"
                        "Content-Length: 40000\r\n\r\n{"), "header with a bad token, then a dribbled body");
    const std::string reply = c ? c->read_until_close(2s) : "";
    const bool refused = reply.rfind("HTTP/1.1 503", 0) == 0;
    expect((reply.rfind("HTTP/1.1 401", 0) == 0 || refused) && std::chrono::steady_clock::now() - started < 1s,
           "answered at once: 401, or 503 server_busy while every worker drains an earlier 401 (spec §2.3)");
    if (reply.rfind("HTTP/1.1 401", 0) == 0) ++fortyones;
    if (c) dribblers.push_back(std::move(*c));
  }
  expect(fortyones >= 1, "at least one dribbling bad-token client got the 401 auth-before-body answer");
  std::optional<raw_http::Response> r;
  for (int attempt = 0; attempt < 12 && !(r && r->status == 200); ++attempt) {
    r = raw(*server, chat_request(*server, chat_body()), 10s);
    if (!(r && r->status == 200)) std::this_thread::sleep_for(300ms);
  }
  expect(r && r->status == 200, "a real chat completes once the P8 drains free the workers");
}

// Ruling T12-net01: differential listener comparison helper (see below).
std::vector<std::string> listening_signature(const std::vector<test_support::SocketRow>& rows) {
  std::vector<std::string> sig;
  for (const auto& row : rows)
    if (row.listening) sig.push_back(row.kind + ":" + std::to_string(row.local_port) + ":" + row.local_address);
  std::sort(sig.begin(), sig.end());
  return sig;
}

// NET-01 / §2.1: a fixed worker pool adds no listener. D3: the traffic is
// asserted, not asserted-away. Ruling T12-net01b: the plan's fixed "20 x 200"
// expectation contradicts spec §2.3 — with ten concurrent chats holding all
// eight workers past P1 (one lease; the queue arrives in Task 16), later
// clients, GETs included, are legitimately refused 503 server_busy at accept.
// The asserted invariants: every client gets a COMPLETE response (no nulls, no
// hangs), every status is 200 or 503, at least one client got 200 (the pool
// served), a sequential health check succeeds after the burst (the workers
// came back), and the listening-socket set is unchanged (ruling T12-net01:
// differential, because the non-dumpable server forces uid attribution and the
// dev container has other same-uid listeners).
void net01_workers_add_no_listener() {
  auto server = start_server();
  if (!server) return;
  const auto before = listening_signature(test_support::sockets_of(server->pid()));
  std::atomic<int> twos{0};
  std::atomic<int> refused{0};
  std::atomic<int> evicted{0};
  std::atomic<int> wrong{0};
  std::vector<std::thread> clients;
  for (int i = 0; i < 20; ++i) {
    clients.emplace_back([&server, &twos, &refused, &evicted, &wrong, i] {
      const auto r = i % 2 == 0
          ? raw(*server, raw_http::request("GET", i % 4 == 0 ? "/health" : "/v1/models", authority(*server), {raw_http::bearer(token())}))
          : raw(*server, chat_request(*server, chat_body()));
      if (!r) ++evicted;   // closed without a response: the §2.3 eviction outcome
      else if (r->status == 200) ++twos;
      else if (r->status == 503) ++refused;
      else ++wrong;
    });
  }
  for (auto& t : clients) t.join();
  expect(wrong == 0, "no client of the burst got a status outside {200,503}");
  expect(twos >= 1, "at least one client of the burst got 200 (the pool served)");
  // A burst of 20 near-simultaneous clients is exactly the §2.3 admission
  // situation: late arrivals evict the oldest not-yet-authenticated connection
  // (closed without a response -> a nullopt exchange) or arrive with no idle
  // worker and no P1 victim (503 server_busy). Both are complete outcomes;
  // nothing may hang, and the server must serve again right after.
  std::cerr << "net01 burst: 200=" << twos.load() << " refused503=" << refused.load()
            << " evicted=" << evicted.load() << " wrong=" << wrong.load() << '\n';
  std::optional<raw_http::Response> health;
  for (int attempt = 0; attempt < 10 && !(health && health->status == 200); ++attempt) {
    health = raw(*server, raw_http::request("GET", "/health", authority(*server), {raw_http::bearer(token())}), 2s);
    if (!(health && health->status == 200)) std::this_thread::sleep_for(300ms);
  }
  expect(health && health->status == 200, "after the burst a sequential /health answers 200: the workers came back");
  const auto after_rows = test_support::sockets_of(server->pid());
  const auto after = listening_signature(after_rows);
  expect(after == before, "20 mixed requests added no listening socket to the server");
  bool server_listener = false;
  for (const auto& row : after_rows) {
    if (!row.listening || row.local_port != server->port()) continue;
    server_listener = true;
    expect(test_support::is_loopback_row(row), "the server's listener is the loopback one");
  }
  expect(server_listener, "the server still listens on its port after the traffic");
}

// NET-01: on a ::1 bind the server's listener is the v6 loopback one, serving
// adds no listener (differential, ruling T12-net01), and nothing answers on
// IPv4. D2: reuses wait_health_on.
void net01_ipv6_loopback_listener_only() {
  if (!test_support::ipv6_loopback_available()) test_support::skip("no IPv6 loopback on this host");
  test_support::ServerOptions options;
  options.config_literals = {{"listener.host", "\"::1\""}};
  auto server = test_support::ServerProcess::start(options);
  expect(server != nullptr, "server starts on ::1");
  if (!server) return;
  const auto r = wait_health_on("::1", server->port(), "[::1]:" + std::to_string(server->port()));
  expect(r && r->status == 200, "served on ::1");
  const auto rows = test_support::sockets_of(server->pid());
  const auto before = listening_signature(rows);
  std::size_t own_listeners = 0;
  for (const auto& row : rows) {
    if (!row.listening || row.local_port != server->port()) continue;
    ++own_listeners;
    expect(row.kind == "tcp6" && test_support::is_loopback_row(row), "the server's listener is ::1 only");
  }
  expect(own_listeners == 1U, "the server has exactly one listener, on its port");
  const auto again = test_support::sockets_of(server->pid());
  expect(listening_signature(again) == before, "the /health traffic added no listener");
  expect(!raw_http::Connection::open("127.0.0.1", server->port()).has_value(), "nothing listens on IPv4");
}

// SP3 Task 16 (ChatRoute; spec §5.4, B1/B2/B6, §3.5, DOS-02): the server-side
// budget/sampling/response cases read the engine's --record-file, so they can
// assert exactly what GEN reached the engine.
std::vector<std::string> gen_lines(const std::string& record) {
  std::vector<std::string> lines;
  std::istringstream in(record);
  for (std::string line; std::getline(in, line);) if (line.rfind("GEN ", 0) == 0) lines.push_back(line);
  return lines;
}
std::size_t gen_max(const std::string& gen) { return std::stoul(gen.substr(4, gen.find(' ', 4) - 4)); }
std::size_t gen_prompt(const std::string& gen) {
  const std::string ids = gen.substr(gen.rfind(' ') + 1U);
  return static_cast<std::size_t>(std::count(ids.begin(), ids.end(), ',')) + 1U;
}
struct Recorded final {
  std::filesystem::path record;
  std::unique_ptr<test_support::ServerProcess> server;
};
Recorded start_recorded(const std::string& tag, std::map<std::string, std::string> literals = {}, std::vector<std::string> engine = {}) {
  Recorded r;
  r.record = test_support::make_temp_dir(tag) / "record.txt";
  engine.push_back("--record-file=" + test_support::utf8_path(r.record));
  r.server = start_server(std::move(literals), std::move(engine));
  return r;
}

void max_tokens_above_limit_is_clamped() {
  auto r = start_recorded("mt1", {{"limits.chat.max_completion_tokens", "16"}, {"limits.chat.default_max_tokens", "0"}});
  if (!r.server) return;
  const auto response = raw(*r.server, chat_request(*r.server, chat_body(R"("max_tokens":16384)")));
  expect(response && response->status == 200, "an over-limit max_tokens is clamped, not rejected (B2)");
  const auto gens = gen_lines(record_of(r.record));
  expect(gens.size() == 1U && gen_max(gens.front()) == 16U, "GEN max_new is the configured cap");
}

void max_tokens_clamped_to_context_room() {
  auto r = start_recorded("mt2", {}, {"--ready-context=200"});
  if (!r.server) return;
  const auto response = raw(*r.server, chat_request(*r.server, chat_body(R"("max_tokens":1000)")));
  expect(response && response->status == 200, "clamped to the context room");
  const auto gens = gen_lines(record_of(r.record));
  expect(gens.size() == 1U && gen_max(gens.front()) == 200U - gen_prompt(gens.front()) - 8U, "max_new = ctx - n - 8");
}

void prompt_alone_over_context_is_400() {
  auto r = start_recorded("mt3", {}, {"--ready-context=64"});
  if (!r.server) return;
  const auto response = raw(*r.server, chat_request(*r.server, chat_body("", std::string(100, 'x'))));
  expect(response && response->status == 400 &&
         response->body == secure_frontend::render_error_body(ApiError::context_length_exceeded), "the prompt alone fills the context");
  expect(gen_lines(record_of(r.record)).empty(), "no GEN was written");
}

// SP5 M1 Task 5 (parity row fit_max_tokens, serve/server.py:4526-4544): a configured false turns an
// over-room request into the §7.2 400 instead of shortening it; the default true keeps SP3's B2 clamp
// (covered by max_tokens_clamped_to_context_room).
void fit_max_tokens_false_returns_400_like_the_pin() {
  auto r = start_recorded("fitoff", {{"engine.fit_max_tokens", "false"}}, {"--ready-context=200"});
  if (!r.server) return;
  const auto response = raw(*r.server, chat_request(*r.server, chat_body(R"("max_tokens":1000)")));
  expect(response && response->status == 400 && response->body.find("context_length_exceeded") != std::string::npos,
         "400 like the pin's over-room refusal (:4536-4543)");
  expect(gen_lines(record_of(r.record)).empty(), "no GEN is written when fit_max_tokens is false");
}

// SP5 M1 Task 6 (parity row idle_unload_s, runconfig.py:35): the serving path's teardown +
// respawn (pin :4100-4109 unload thread, :6252 "loads on use"). With a 1 s window the
// supervisor tears the engine down after the first request; /health keeps the closed literal
// set and reports the cold engine as ok/"starting" (Ruling T6-e); the next POST wakes the
// supervisor and a fresh generation answers. The fake engine rewrites its record at every
// start, so the record proves WHICH generation answered last and the supervisor's stderr
// proves the unload: generation 1 ends "unloaded", and the final record holds exactly the
// respawned generation's GEN.
void idle_unload_respawns_before_the_next_request() {
  auto r = start_recorded("idle", {{"engine.idle_unload_s", "1"}}, {"--ready-context=200"});
  if (!r.server) return;
  const auto first = raw(*r.server, chat_request(*r.server, chat_body(R"("max_tokens":4)")));
  expect(first && first->status == 200, "the first request answers");
  bool cold = false;
  std::string last_body;
  for (int i = 0; i < 100 && !cold; ++i) {
    const auto health = raw(*r.server, raw_http::request("GET", "/health", authority(*r.server), {raw_http::bearer(token())}));
    last_body = health ? health->body : std::string{"<no response>"};
    cold = health && health->body == "{\"status\":\"ok\",\"engine\":\"starting\"}\n";
    if (!cold) std::this_thread::sleep_for(100ms);
  }
  expect(cold, "the idle window tears the engine down: /health reports ok + starting (unloaded), saw " + last_body);
  const auto second = raw(*r.server, chat_request(*r.server, chat_body(R"("max_tokens":4)")), 30s);
  expect(second && second->status == 200, "the next request answers: the wake respawned the engine");
  const std::string log = r.server->stderr_text();
  expect(log.find("engine generation 1 ended: unloaded") != std::string::npos,
         "the supervisor's log names generation 1's intentional unload");
  const auto gens = gen_lines(record_of(r.record));
  expect(gens.size() == 1U, "the answer came from the respawned generation: one GEN in the final record (got " +
                                std::to_string(gens.size()) + ")");
}

void absent_max_tokens_uses_auto() {
  auto r = start_recorded("mt4");
  if (!r.server) return;
  const auto response = raw(*r.server, chat_request(*r.server, chat_body("")));
  expect(response && response->status == 200, "a request without max_tokens");
  const auto gens = gen_lines(record_of(r.record));
  expect(gens.size() == 1U && gen_max(gens.front()) == std::min<std::size_t>(4096U, 4096U - gen_prompt(gens.front()) - 8U),
         "auto = min(max_completion_tokens, ctx - n - 8) (B1)");
}

// SP5 M1 Task 8 (parity rows engine.max_tokens / engine.min_tokens re-scoped as request
// output bounds per the Task-2 rows and Ruling T8-e, and the answer_room typed row): the
// configured ceiling/floor ride the GEN budget at admission — cap wins downward, floor
// lifts upward, and answer_room=false changes nothing on a path with no thinking parser
// (Ruling T8-d).
void queue_and_answer_room_limits_are_enforced() {
  auto ceiling = start_recorded("shape_c", {{"engine.max_tokens", "8"}});
  if (!ceiling.server) return;
  const auto over = raw(*ceiling.server, chat_request(*ceiling.server, chat_body(R"("max_tokens":64)")));
  expect(over && over->status == 200, "an over-ceiling request is capped, not refused");
  const auto ceiling_gens = gen_lines(record_of(ceiling.record));
  expect(ceiling_gens.size() == 1U && gen_max(ceiling_gens.front()) == 8U,
         "engine.max_tokens caps the GEN budget: " + (ceiling_gens.empty() ? "<none>" : ceiling_gens.front()));
  auto floor = start_recorded("shape_f", {{"engine.min_tokens", "16"}});
  if (!floor.server) return;
  const auto tiny = raw(*floor.server, chat_request(*floor.server, chat_body(R"("max_tokens":2)")));
  expect(tiny && tiny->status == 200, "a below-floor request is lifted, not refused");
  const auto floor_gens = gen_lines(record_of(floor.record));
  expect(floor_gens.size() == 1U && gen_max(floor_gens.front()) == 16U,
         "engine.min_tokens lifts the GEN budget: " + (floor_gens.empty() ? "<none>" : floor_gens.front()));
  auto room = start_recorded("shape_a", {{"engine.answer_room", "false"}});
  if (!room.server) return;
  const auto still = raw(*room.server, chat_request(*room.server, chat_body(R"("max_tokens":64)")));
  expect(still && still->status == 200, "answer_room false changes nothing on this path (T8-d)");
}

// SP5 M1 Task 8 (parity row aliases, runconfig.py:34 + set_aliases :3764-3777): a
// configured alias is a served name — listed by /v1/models, answered by chat; matching is
// exact (§4: never empty or partial) and an alias equal to the model is dropped (:3775).
// Session log step 13 (owner: "name the models better instead of the filename"): model.name is the
// served name, like the pin's model_name (serve/server.py:7855, default_model_name): /v1/models
// lists it and a chat names it; the inventory file name is no longer an answerable id.
void model_name_replaces_the_file_name() {
  auto r = start_recorded("named", {{"model.name", "\"qwen3.8-flash-next\""}, {"model.aliases", R"(["qwen-flash"])"}});
  if (!r.server) return;
  const auto list = raw(*r.server, raw_http::request("GET", "/v1/models", authority(*r.server), {raw_http::bearer(token())}));
  expect(list && list->status == 200 && list->body.find("\"id\":\"qwen3.8-flash-next\"") != std::string::npos &&
             list->body.find("\"id\":\"qwen-flash\"") != std::string::npos && list->body.find("fake-model") == std::string::npos,
         "the list names the served name and its alias, not the file: " + (list ? list->body : std::string{"<none>"}));
  const auto named = raw(*r.server, chat_request(*r.server, R"({"model":"qwen3.8-flash-next","messages":[{"role":"user","content":"Hi"}]})"));
  expect(named && named->status == 200 && named->body.find("\"model\":\"qwen3.8-flash-next\"") != std::string::npos,
         "a chat names the served name and is answered with it");
  const auto alias = raw(*r.server, chat_request(*r.server, R"({"model":"qwen-flash","messages":[{"role":"user","content":"Hi"}]})"));
  expect(alias && alias->status == 200, "an alias still answers");
  const auto file = raw(*r.server, chat_request(*r.server, R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}]})"));
  expect(file && file->status == 404, "the file name is not a served name any more");
  const auto one = raw(*r.server, raw_http::request("GET", "/v1/models/qwen3.8-flash-next", authority(*r.server), {raw_http::bearer(token())}));
  expect(one && one->status == 200, "GET /v1/models/<served name> finds it");
}

// Session log step 14: the qwen profile serves a per-request reasoning_effort (the page's
// Thinking control); an unknown word is a 400 naming nothing (invalid_request).
void reasoning_effort_per_request_is_served() {
  auto server = start_server();
  if (!server) return;
  for (const char* word : {"none", "high"}) {
    const auto r = raw(*server, chat_request(*server, chat_body(std::string{R"("max_tokens":2,"reasoning_effort":")"} + word + "\"")));
    expect(r && r->status == 200, std::string{"reasoning_effort "} + word + " is served");
  }
  const auto bad = raw(*server, chat_request(*server, chat_body(R"("max_tokens":2,"reasoning_effort":"max")")));
  expect(bad && bad->status == 400, "an unknown effort word is refused");
}

void aliases_answer_like_the_primary_model() {
  auto r = start_recorded("aliases", {{"model.aliases", R"(["Alt.Model","fake-model","secondary"])"}});
  if (!r.server) return;
  const auto list = raw(*r.server, raw_http::request("GET", "/v1/models", authority(*r.server), {raw_http::bearer(token())}));
  expect(list && list->status == 200, "the model list answers");
  if (list) {
    expect(list->body.find("\"fake-model\"") != std::string::npos && list->body.find("\"Alt.Model\"") != std::string::npos &&
               list->body.find("\"secondary\"") != std::string::npos,
           "the list names the model and both aliases: " + list->body);
    std::size_t ids = 0;
    for (std::size_t at = list->body.find("\"id\":\""); at != std::string::npos; at = list->body.find("\"id\":\"", at + 1)) ++ids;
    expect(ids == 3U, "the alias equal to the model is dropped (:3775): " + list->body);
  }
  const auto by_alias = raw(*r.server, raw_http::request("GET", "/v1/models/Alt.Model", authority(*r.server), {raw_http::bearer(token())}));
  expect(by_alias && by_alias->status == 200, "GET /v1/models/<alias> finds the served model");
  const auto alt = raw(*r.server, chat_request(*r.server, R"({"model":"Alt.Model","messages":[{"role":"user","content":"Hi"}]})"));
  expect(alt && alt->status == 200, "an alias answers like the primary model");
  const auto miss = raw(*r.server, chat_request(*r.server, R"({"model":"Alt.Mod","messages":[{"role":"user","content":"Hi"}]})"));
  expect(miss && miss->status == 404, "a prefix of an alias is not a match (§4: never partial)");
  const auto empty = raw(*r.server, chat_request(*r.server, R"({"model":"","messages":[{"role":"user","content":"Hi"}]})"));
  expect(empty && empty->status != 200, "an empty model name never matches (§4)");
}

void sampling_passthrough_reaches_gen() {  auto r = start_recorded("smp");
  if (!r.server) return;
  const auto response = raw(*r.server, chat_request(*r.server, chat_body(R"("max_tokens":2,"temperature":0.5,"top_p":0.9,"seed":42,"presence_penalty":0)")));
  expect(response && response->status == 200, "sampling accepted");
  const auto gens = gen_lines(record_of(r.record));
  expect(gens.size() == 1U && gens.front().find(" temperature=0.5 top_p=0.9 seed=42 ") != std::string::npos, "GEN carries the keys: " +
         (gens.empty() ? std::string{} : gens.front()));
}

void dos01_context_rule_is_400_without_gen() {
  auto r = start_recorded("ctx", {}, {"--ready-context=64"});
  if (!r.server) return;
  const auto response = raw(*r.server, chat_request(*r.server, chat_body(R"("max_tokens":5)", std::string(100, 'y'))));
  expect(response && response->status == 400 && response->body.find("context_length_exceeded") != std::string::npos, "400");
  expect(gen_lines(record_of(r.record)).empty() && r.server->wait_for_health(test_support::kHealthReady, 2s),
         "no GEN; the engine never saw a request it would refuse and stays ready");
}

// --- SP5 M1 Task 12 e2e: the managed model that reads images (C-9 carve-out) ----------
// Tiny helpers: a base64 encoder and a PNG header with an 8x6 IHDR (the payload gate
// reads IHDR; nothing decodes pixels on this path).
std::string b64e(const std::string& raw_bytes) {
  static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (std::size_t i = 0; i < raw_bytes.size(); i += 3U) {
    const unsigned b0 = static_cast<unsigned char>(raw_bytes[i]);
    const unsigned b1 = i + 1U < raw_bytes.size() ? static_cast<unsigned char>(raw_bytes[i + 1U]) : 0U;
    const unsigned b2 = i + 2U < raw_bytes.size() ? static_cast<unsigned char>(raw_bytes[i + 2U]) : 0U;
    out += table[b0 >> 2U];
    out += table[((b0 & 3U) << 4U) | (b1 >> 4U)];
    out += i + 1U < raw_bytes.size() ? table[((b1 & 15U) << 2U) | (b2 >> 6U)] : '=';
    out += i + 2U < raw_bytes.size() ? table[b2 & 63U] : '=';
  }
  return out;
}
std::string png8x6() {
  const auto be32 = [](const unsigned v) {
    return std::string{static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8),
                       static_cast<char>(v)};
  };
  return std::string("\x89PNG\r\n\x1a\n", 8) + be32(13) + "IHDR" + be32(8) + be32(6) + std::string(9U, '\0');
}
std::string image_part(const std::string& url) {
  return R"({"type":"image_url","image_url":{"url":")" + url + R"("}})";
}
std::string vision_body(const std::string& parts, const std::string& model_id = "m:t") {
  return R"({"model":")" + model_id + R"(","messages":[{"role":"user","content":[)" + parts +
         R"(]}],"max_tokens":2})";
}

// Boots the managed vision server: fx m:t (mmproj + byte tokenizer pack) installed in
// the harness, maya-vision (the fake child) beside the engine, and the fake engine
// recording its stdin to `record`. Returns nullopt-style on boot failure (expect fires).
std::unique_ptr<test_support::ServerProcess> start_vision_server(const std::filesystem::path& state,
                                                                 const std::filesystem::path& record,
                                                                 std::map<std::string, std::string> literals = {}) {
  test_support::ServerOptions options;
  options.managed_vision = true;
  options.state_dir = state;
  options.engine_args = {"--record-file=" + test_support::utf8_path(record)};
  literals["vision.enabled"] = "true";
  options.config_literals = std::move(literals);
  auto server = test_support::ServerProcess::start(options);
  const bool ready = server != nullptr && server->wait_until_listening(10s) &&
                     server->wait_for_health(test_support::kHealthReady, 15s);
  expect(ready, "managed+vision server ready" + (server ? ": " + server->stderr_text() : std::string{}));
  return ready ? std::move(server) : nullptr;
}

// The C-9 200 path, end to end (plan RED enc_round_trip_reaches_the_prompt): an
// image_url part on the managed vision model reaches the fake encoder over ENC, the
// fake engine receives the pin's GENI line (:1778: head, ids, then the combined .sve
// path at the END), and the request's scratch files are gone afterwards (no LRU in
// M1, Ruling T12-d). A text request on the same server still writes a plain GEN.
void enc_round_trip_reaches_the_prompt() {
  const auto dir = test_support::make_temp_dir("api-vision");
  const auto state = dir / "state";
  secure_frontend_test::make_owner_private_directory(state);
  const auto record = dir / "engine-record.txt";
  auto server = start_vision_server(state, record);
  if (server == nullptr) return;
  const auto body = vision_body(R"({"type":"text","text":"look"},)" + image_part("data:image/png;base64," + b64e(png8x6())));
  const auto response = raw(*server, chat_request(*server, body));
  const std::string what = [&response] {
    if (!response.has_value()) return std::string{"<no response>"};
    return "status=" + std::to_string(response->status) + " body=" + response->body.substr(0, 400);
  }();
  expect(response.has_value() && response->status == 200,
         "the image request answers like text on the vision model (C-9 200 path): " + what + " server: " + server->stderr_text());
  const auto text = record_of(record);
  expect(count_of(text, "GENI ") == 1U, "exactly one GENI line reached the engine");
  const auto at = text.find("GENI ");
  std::string line;
  if (at != std::string::npos) {
    const auto eol = text.find('\n', at);
    line = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
  }
  expect(!line.empty() && line.find(".sve") != std::string::npos,
         "the GENI tail names the combined embeddings file (pin :1778, :4547-4556): " + line);
  std::error_code ec;
  bool leftover = false;
  for (const auto& entry : std::filesystem::directory_iterator(state / "vision", ec)) {
    (void)entry;
    leftover = true;
  }
  expect(!leftover, "the staged image and every .sve are deleted when the request ends (T12-d)");
  const auto text_response = raw(*server, chat_request(*server, R"({"model":"m:t","messages":[{"role":"user","content":"Hi"}],"max_tokens":2})"));
  expect(text_response.has_value() && text_response->status == 200, "text still answers on the vision server");
  const auto after = record_of(record);
  expect(count_of(after, "GENI ") == 1U, "the text request wrote a plain GEN, not a GENI (T12-i)");
}

// Refusals fire before any write (plan RED oversized_or_bad_base64_rejected_before_decode,
// server half): bad base64, an unfetched https URL, and one image past vision.max_n all
// take the fixed 400 naming image_url; the scratch dir stays empty; text serving continues.
void image_rejections_name_image_url_before_any_write() {
  const auto dir = test_support::make_temp_dir("api-vision-refuse");
  const auto state = dir / "state";
  secure_frontend_test::make_owner_private_directory(state);
  const auto record = dir / "engine-record.txt";
  auto server = start_vision_server(state, record, {{"vision.max_n", "1"}});
  if (server == nullptr) return;
  for (const auto& url : {std::string{"data:image/png;base64,@@"}, std::string{"https://example.test/a.png"}}) {
    const auto response = raw(*server, chat_request(*server, vision_body(image_part(url))));
    expect(response.has_value() && response->status == 400 && response->body.find("\"param\":\"image_url\"") != std::string::npos,
           "the 400 names image_url before any decode or write: " + url);
  }
  const auto two = vision_body(image_part("data:image/png;base64," + b64e(png8x6())) + "," +
                               image_part("data:image/png;base64," + b64e(png8x6())));
  const auto over = raw(*server, chat_request(*server, two));
  expect(over.has_value() && over->status == 400 && over->body.find("\"param\":\"image_url\"") != std::string::npos,
         "a second image past vision.max_n names image_url (:103 family, re-scoped)");
  std::error_code ec;
  bool leftover = false;
  for (const auto& entry : std::filesystem::directory_iterator(state / "vision", ec)) {
    (void)entry;
    leftover = true;
  }
  expect(!leftover, "no refusal left a scratch file behind");
  const auto ok = raw(*server, chat_request(*server, R"({"model":"m:t","messages":[{"role":"user","content":"Hi"}],"max_tokens":2})"));
  expect(ok.has_value() && ok->status == 200, "refusals do not poison the serving loop");
}

void chat_response_has_openai_shape() {
  auto r = start_recorded("shape");
  if (!r.server) return;
  const auto first = raw(*r.server, chat_request(*r.server, chat_body()));
  const auto second = raw(*r.server, chat_request(*r.server, chat_body()));
  expect(first && first->status == 200 && second && second->status == 200, "two chats");
  if (!first || !second) return;
  const auto id_of = [](const std::string& body) { const auto p = body.find("\"id\":\"chatcmpl-"); return p == std::string::npos ? std::string{} : body.substr(p + 15, 32); };
  expect(id_of(first->body).size() == 32U && id_of(first->body).find_first_not_of("0123456789abcdef") == std::string::npos, "32 lowercase hex");
  expect(id_of(first->body) != id_of(second->body), "a fresh id per response");
  expect(first->body.find("\"created\":0") == std::string::npos && first->body.find("\"finish_reason\":") != std::string::npos &&
         first->body.find("\"completion_tokens\":1") != std::string::npos, "created, finish_reason and usage are real");
}

void dos02_excess_connections_get_503_retry_after() {
  auto r = start_recorded("exc", {{"limits.concurrency.max_concurrent_requests", "2"}}, {"--tokens=60", "--token-interval-ms=50"});
  if (!r.server) return;
  std::thread chat([&] { (void)raw(*r.server, chat_request(*r.server, chat_body("")), 30s); });
  std::this_thread::sleep_for(300ms);   // worker 1 in P6
  auto body_holder = raw_http::Connection::open("127.0.0.1", r.server->port());
  expect(body_holder && body_holder->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*r.server) + "\r\n" +
                                          raw_http::bearer(token()) + "\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{"),
         "worker 2 held in P3 by an authenticated slow body");
  std::this_thread::sleep_for(200ms);
  const auto refused = raw(*r.server, raw_http::request("GET", "/health", authority(*r.server), {raw_http::bearer(token())}), 5s);
  expect(refused && refused->status == 503 && refused->header("Retry-After") == std::optional<std::string>{"1"} &&
         refused->body == secure_frontend::render_error_body(ApiError::server_busy), "no worker free: 503 server_busy, Retry-After: 1, JSON, got " +
         (refused ? std::to_string(refused->status) + " " + refused->body : std::string{"<no response>"}));
  body_holder.reset();
  chat.join();
}

void dos02_queue_full_is_503() {
  auto r = start_recorded("qf", {}, {"--tokens=20", "--token-interval-ms=50"});
  if (!r.server) return;
  std::vector<std::thread> holders;
  for (int i = 0; i < 5; ++i) {
    holders.emplace_back([&] { (void)raw(*r.server, chat_request(*r.server, chat_body("")), 30s); });
    std::this_thread::sleep_for(50ms);
  }
  std::this_thread::sleep_for(200ms);   // 1 running + 4 queued
  const auto started = std::chrono::steady_clock::now();
  const auto refused = raw(*r.server, chat_request(*r.server, chat_body("")));
  expect(refused && refused->status == 503 && refused->body == secure_frontend::render_error_body(ApiError::server_busy) &&
         refused->header("Retry-After") == std::optional<std::string>{"1"}, "the sixth chat is refused at once");
  expect(std::chrono::steady_clock::now() - started < 500ms, "without waiting");
  for (auto& t : holders) t.join();
}

void dos02_queue_timeout_is_503() {
  auto r = start_recorded("qt", {{"limits.concurrency.queue_timeout_ms", "1000"}}, {"--tokens=30", "--token-interval-ms=100"});
  if (!r.server) return;
  std::thread running([&] { (void)raw(*r.server, chat_request(*r.server, chat_body("")), 30s); });
  std::this_thread::sleep_for(200ms);
  const auto started = std::chrono::steady_clock::now();
  const auto waited = raw(*r.server, chat_request(*r.server, chat_body("")), 10s);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(waited && waited->status == 503 && waited->body.find("server_busy") != std::string::npos, "queue timeout is 503 server_busy");
  expect(elapsed >= 900ms && elapsed < 2500ms, "after about 1 s");
  running.join();
}

void dos02_queue_is_fifo() {
  auto r = start_recorded("fifo", {}, {"--tokens=8", "--token-interval-ms=50"});
  if (!r.server) return;
  std::mutex m;
  std::vector<int> done;
  std::vector<std::thread> clients;
  for (int i = 0; i < 5; ++i) {
    clients.emplace_back([&, i] {
      const auto response = raw(*r.server, chat_request(*r.server, chat_body("", "client" + std::to_string(i))), 30s);
      std::lock_guard g(m);
      if (response && response->status == 200) done.push_back(i);
    });
    std::this_thread::sleep_for(100ms);
  }
  for (auto& t : clients) t.join();
  expect(done == std::vector<int>{0, 1, 2, 3, 4}, "completion order equals admission order");
}

void dos02_health_answers_while_generation_places_full() {
  auto r = start_recorded("hp", {}, {"--tokens=40", "--token-interval-ms=50"});
  if (!r.server) return;
  std::vector<std::thread> holders;
  for (int i = 0; i < 5; ++i) holders.emplace_back([&] { (void)raw(*r.server, chat_request(*r.server, chat_body("")), 60s); });
  std::this_thread::sleep_for(500ms);
  const auto started = std::chrono::steady_clock::now();
  const auto health = raw(*r.server, raw_http::request("GET", "/health", authority(*r.server), {raw_http::bearer(token())}));
  const auto models = raw(*r.server, raw_http::request("GET", "/v1/models", authority(*r.server), {raw_http::bearer(token())}));
  expect(health && health->status == 200 && health->body == "{\"status\":\"ok\",\"engine\":\"generating\"}\n", "/health answers at once");
  expect(models && models->status == 200, "/v1/models answers at once");
  expect(std::chrono::steady_clock::now() - started < 1s, "within one second while every generation place is taken");
  for (auto& t : holders) t.join();
}

void dos02_serial_mode_one_worker() {
  auto r = start_recorded("serial", {{"limits.concurrency.max_concurrent_requests", "1"}}, {"--tokens=40", "--token-interval-ms=50"});
  if (!r.server) return;
  std::thread first([&] { (void)raw(*r.server, chat_request(*r.server, chat_body("")), 30s); });
  std::this_thread::sleep_for(300ms);
  const auto second = raw(*r.server, chat_request(*r.server, chat_body("")));
  expect(second && second->status == 503 && second->body.find("server_busy") != std::string::npos, "the second chat is busy in serial mode");
  first.join();
}

// SP3 Task 17 (acceptance: DOS-02 cancel/release, §2.6, §2.8; the behavior was
// built in Tasks 6a/8a/8b/12/13/15/16 — these lock it in).
void dos02_queued_client_disconnect_leaves_queue() {
  auto r = start_recorded("qdc", {}, {"--tokens=40", "--token-interval-ms=50"});
  if (!r.server) return;
  std::thread a([&] { (void)raw(*r.server, chat_request(*r.server, chat_body("", "AAA")), 30s); });
  std::this_thread::sleep_for(300ms);
  {
    auto b = raw_http::Connection::open("127.0.0.1", r.server->port());
    expect(b && b->send(chat_request(*r.server, chat_body("", "BBB"))), "B queued");
    std::this_thread::sleep_for(300ms);
  }   // B goes away while queued
  const auto c = raw(*r.server, chat_request(*r.server, chat_body("", "CCC")), 30s);
  a.join();
  expect(c && c->status == 200, "C is served");
  expect(gen_lines(record_of(r.record)).size() == 2U, "only A and C reached the engine; B left the queue silently");
}

void dos02_non_stream_disconnect_cancels() {
  auto r = start_recorded("ndc", {}, {"--tokens=10000", "--token-interval-ms=5"});
  if (!r.server) return;
  {
    auto c = raw_http::Connection::open("127.0.0.1", r.server->port());
    expect(c && c->send(chat_request(*r.server, chat_body(""))), "a long chat");
    std::this_thread::sleep_for(300ms);
  }   // client gone during P6
  bool stopped = false;
  for (int i = 0; i < 50 && !stopped; ++i) {
    stopped = record_of(r.record).find("\nSTOP\n") != std::string::npos;
    if (!stopped) std::this_thread::sleep_for(20ms);
  }
  expect(stopped, "STOP reached the engine within 1 s of the disconnect");
  const auto started = std::chrono::steady_clock::now();
  const auto next = raw(*r.server, chat_request(*r.server, chat_body(R"("max_tokens":2)")), 5s);
  expect(next && next->status == 200 && std::chrono::steady_clock::now() - started < 1s, "the next request is served within 1 s");
  expect(r.server->stderr_text().find("killed_by_supervisor") == std::string::npos, "a drained cancel never kills the engine");
}

void dos02_resources_return_to_idle() {
  auto r = start_recorded("idle", {}, {"--tokens=10000", "--token-interval-ms=5"});
  if (!r.server) return;
  (void)raw(*r.server, raw_http::request("GET", "/health", authority(*r.server), {raw_http::bearer(token())}));
  std::this_thread::sleep_for(300ms);
  const auto threads0 = test_support::thread_count(r.server->pid());
  // The kinds a request opens (connection_scan.hpp): every fd on Linux; File and
  // Process handles on Windows, whose total also holds the OS thread pool's own.
  const auto handles0 = test_support::request_handle_count(r.server->pid());
  expect(handles0.has_value(), "the server's handles can be counted");
  for (int i = 0; i < 100; ++i) {
    auto c = raw_http::Connection::open("127.0.0.1", r.server->port());
    if (c) (void)c->send(chat_request(*r.server, chat_body("")));
    std::this_thread::sleep_for(30ms);
  }
  for (int index = 0; index < 20; ++index) {
    secure_frontend::ChatRequestOptions cancel_early;
    cancel_early.on_item = [](const secure_frontend::SseItem&) { return false; };
    (void)test_support::chat(*r.server, test_support::chat_body(R"("stream":true)"), cancel_early);
  }
  bool idle = false;
  for (int i = 0; i < 100 && !idle; ++i) {
    std::this_thread::sleep_for(50ms);
    idle = test_support::thread_count(r.server->pid()) == threads0 && handles0.has_value() &&
           test_support::request_handle_count(r.server->pid()) == handles0;
  }
  expect(idle, "after 100 cancelled chats threads (" + std::to_string(test_support::thread_count(r.server->pid())) + "/" +
               std::to_string(threads0) + ") and request handles (" +
               std::to_string(test_support::request_handle_count(r.server->pid()).value_or(0)) + "/" +
               std::to_string(handles0.value_or(0)) + "; all handles " + std::to_string(test_support::handle_count(r.server->pid())) +
               ") return to their idle values");
  const auto health = raw(*r.server, raw_http::request("GET", "/health", authority(*r.server), {raw_http::bearer(token())}));
  expect(health && health->status == 200, "and the service is healthy");
}

void shutdown_errors_never_report_fatal() {
  auto r = start_recorded("sd", {}, {"--tokens=10000", "--token-interval-ms=5"});
  if (!r.server) return;
  std::optional<raw_http::Response> answer;
  std::string raw_answer;
  std::thread chat([&] {
    auto c = raw_http::Connection::open("127.0.0.1", r.server->port());
    if (c && c->send(chat_request(*r.server, chat_body("")))) raw_answer = c->read_until_close(10s);
  });
  std::this_thread::sleep_for(500ms);
  const auto started = std::chrono::steady_clock::now();
#if defined(_WIN32)
  r.server->stop(test_support::ServerProcess::Stop::ctrl_break);
#else
  r.server->stop(test_support::ServerProcess::Stop::term);
#endif
  const auto code = r.server->wait_exit(10s);
  chat.join();
  expect(code == std::optional<int>{0} && std::chrono::steady_clock::now() - started < 5s, "exit 0 within SP2's 5 s bound");
  answer = raw_http::parse_response(raw_answer);
  expect(raw_answer.empty() || (answer && answer->status == 503 && answer->body == secure_frontend::render_error_body(ApiError::server_shutting_down)),
         "the in-flight chat gets 503 server_shutting_down or a close");
  const std::string log = r.server->stderr_text();
  expect(log.find("drain_failed") == std::string::npos && log.find("protocol_failed") == std::string::npos &&
         log.find("write_timeout") == std::string::npos, "no worker-side failure was reported as fatal during shutdown");
}

void dos01_service_healthy_after_each_attack() {
  auto server = start_server({{"limits.http.header_deadline_ms", "500"}, {"limits.http.request_deadline_ms", "1000"}});
  if (!server) return;
  const std::string a = authority(*server);
  const std::vector<std::pair<std::string, std::function<void()>>> attacks{
      {"oversize header", [&] { (void)raw(*server, "GET /health HTTP/1.1\r\nHost: " + a + "\r\nX: " + std::string(20000, 'x') + "\r\n\r\n"); }},
      {"deep JSON", [&] { (void)raw(*server, chat_request(*server, R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"logit_bias":{"a":{"b":{"c":{"d":1}}}}})")); }},
      {"chunked body", [&] { (void)raw(*server, raw_http::request("POST", "/v1/chat/completions", a, {raw_http::bearer(token()), "Transfer-Encoding: chunked"})); }},
      {"slow then closed body", [&] {
         auto c = raw_http::Connection::open("127.0.0.1", server->port());
         if (c) (void)c->send(chat_request(*server, chat_body()).substr(0, 120));
       }},
      {"slow header", [&] {
         auto c = raw_http::Connection::open("127.0.0.1", server->port());
         if (c) { (void)c->send("GET /he"); (void)c->read_until_close(2s); }
       }},
      {"body over limit", [&] { (void)raw(*server, chat_request(*server, std::string(60000, ' '))); }},
      {"garbage", [&] { (void)raw(*server, std::string("\x16\x03\x01\x02\x00", 5) + std::string(100, '\x01')); }}};
  for (const auto& [name, attack] : attacks) {
    attack();
    const auto health = raw(*server, raw_http::request("GET", "/health", a, {raw_http::bearer(token())}));
    const auto chat = raw(*server, chat_request(*server, chat_body()));
    expect(health && health->status == 200 && chat && chat->status == 200, "healthy and serving after: " + name);
  }
}

// SP3 Task 20 (EXT-01/EXT-02 amendments, spec §6.4): the upstream product's
// control surface never exists here, and HTTP headers cannot switch extensions on.
void ext01_upstream_control_routes_are_404_after_auth() {
  auto server = start_server();
  if (!server) return;
  for (const std::string target : {"/v1/load", "/settings", "/config", "/slots", "/metrics", "/props", "/mcp", "/v1/completions",
                                   "/v1/messages", "/v1/responses", "/v1/embeddings", "/api/chat", "/api/generate", "/api/tags",
                                   "/api/pull", "/v1/tools", "/v1/mcp"}) {
    for (const std::string method : {"GET", "POST"}) {
      std::vector<std::string> lines{raw_http::bearer(token())};
      if (method == "POST") lines.push_back("Content-Type: application/json");
      const auto r = raw(*server, raw_http::request(method, target, authority(*server), lines, method == "POST" ? "{}" : ""));
      expect(r && r->status == 404 && r->body == secure_frontend::render_error_body(secure_frontend::ApiError::not_found),
             method + " " + target + " is 404 after authentication");
    }
  }
}

void ext02_http_headers_cannot_enable_extensions() {
  const auto dir = test_support::make_temp_dir("ext02h");
  auto server = start_server({}, {"--record-file=" + test_support::utf8_path(dir / "record.txt")});
  if (!server) return;
  const auto before = server->active_processes();
  const std::vector<std::string> switches{"X-Strata-Enable-Tools: 1", "X-Strata-Mcp: on", "X-Strata-Extensions: all",
                                          "X-Stainless-Helper-Method: tools"};
  const auto r = raw(*server, chat_request(*server, chat_body(R"("max_tokens":2,"tools":[{"type":"function","function":{"name":"x"}}])"), switches));
  expect(r && r->status == 400 && r->body == secure_frontend::render_error_body(secure_frontend::ApiError::extension_disabled, "tools"),
         "header switches change nothing: tools stay disabled");
  const auto plain = raw(*server, chat_request(*server, chat_body(), switches));
  expect(plain && plain->status == 200 && plain->body.find("tool") == std::string::npos, "an ordinary chat is unaffected");
  expect(count_of(record_of(dir / "record.txt"), "GEN ") == 1U, "only the ordinary chat reached the engine");
  expect(server->active_processes() == before, "no process started");
}

// Owner 2026-10-29 (logging.engine_output, to see why an engine fails on a new GPU): the engine's own stderr lines reach
// the server log as "[engine] <line>" - what the engine prints of a prompt too, which is why the row is off unless
// asked for; without it no engine line is in the log.
void engine_output_reaches_the_log_only_when_asked() {
  const std::string last = "[engine] strata serve: conversation cache: dropped 1 superseded copy of this conversation; parked=1";
  {
    auto server = start_server({{"logging.engine_output", "true"}}, {"--stderr-cache-lines"});
    if (!server) return;
    for (int i = 0; i < 200 && server->stderr_text().find(last) == std::string::npos; ++i) std::this_thread::sleep_for(50ms);
    const std::string log = server->stderr_text();
    const auto at = log.find(last);
    const bool line_end = at != std::string::npos && at + last.size() < log.size() &&
                          (log[at + last.size()] == '\n' || log[at + last.size()] == '\r');  // Windows' text-mode stderr: \r\n
    expect(log.find("\n[engine] strata serve: session 0: prompt: ") != std::string::npos && line_end,
           "the engine's lines, one per log line: " + log);
    expect(log.find("strata: warning: logging.engine_output is true: the engine's own output goes to this log") != std::string::npos,
           "the start says so, with what it can hold");
  }
  auto server = start_server({}, {"--stderr-cache-lines"});
  if (!server) return;
  std::this_thread::sleep_for(1s);
  const std::string log = server->stderr_text();
  expect(log.find("[engine]") == std::string::npos && log.find("logging.engine_output") == std::string::npos,
         "off: no engine line and no notice in the log");
}

// The owner (2026-10-29): model folders on network shares, "mark that as experimental" - a server with
// roots.allow_network_shares says so at every start, with what a share allows; without it, nothing is said.
void network_shares_warn_at_every_start() {
  {
    auto server = start_server({{"roots.allow_network_shares", "true"}});
    if (!server) return;
    const std::string log = server->stderr_text();
    expect(log.find("strata: warning: roots.allow_network_shares is true (experimental)") != std::string::npos &&
               log.find("anyone who can write to a share can change what a model runs") != std::string::npos,
           "the start says so, with what it allows: " + log);
  }
  auto server = start_server({});
  if (!server) return;
  expect(server->stderr_text().find("allow_network_shares") == std::string::npos, "off: nothing is said");
}

// SP3 Task 21 (spec §8): fixed-field request lines under a token bucket, the
// startup summary, and nothing secret anywhere (PRIV-02 frontend part).
void log_line_format_and_rate_limit() {
  auto server = start_server({{"logging.requests", "true"}});
  if (!server) return;
  // NOTE: the readiness gate in start_server POLLS /health; while the engine
  // boots under a slow toolchain (TSan) those polls are logged as health 503
  // lines (404-byte body, code "-"), and the pool logs evicted sockets as
  // `other 0 evicted`. All are real requests with the SAME fixed field shape;
  // the rate rule counts every logged line. The wave size (6x80) guarantees
  // the bucket overflows even when the arrival phase is stretched by TSan.
  const auto started = std::chrono::steady_clock::now();
  std::vector<std::thread> clients;
  for (int t = 0; t < 6; ++t) {
    clients.emplace_back([&] { for (int i = 0; i < 80; ++i) (void)raw(*server, raw_http::request("GET", "/health", authority(*server), {raw_http::bearer(token())})); });
  }
  for (auto& c : clients) c.join();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::this_thread::sleep_for(1100ms);
  (void)raw(*server, raw_http::request("GET", "/health", authority(*server), {raw_http::bearer(token())}));   // lets a suppression line out
  const std::string log = server->stderr_text();
  std::istringstream in(log);
  std::size_t requests = 0, suppressions = 0;
  for (std::string line; std::getline(in, line);) {
    if (line.rfind("req ", 0) == 0) {
      ++requests;
      std::istringstream f(line);
      std::string req, seq, route, status, code, ms, bytes, peer, extra;
      f >> req >> seq >> route >> status >> code >> ms >> bytes >> peer;
      const bool health_line = route == "health" && (status == "200" || status == "503") && code == "-";
      const bool evicted_line = route == "other" && status == "0" && code == "evicted";
      expect((health_line || evicted_line) && peer == "loopback" && !(f >> extra), "fixed fields only: " + line);
    }
    if (line.rfind("log: ", 0) == 0 && line.find(" request lines suppressed") != std::string::npos) ++suppressions;
  }
  const auto bound = 100.0 + 20.0 * (seconds + 1.2) + 1.0;
  expect(static_cast<double>(requests) <= bound, "rate-limited: " + std::to_string(requests) + " <= " + std::to_string(bound));
  expect(requests >= 100U && suppressions >= 1U && static_cast<double>(suppressions) <= seconds + 2.0, "burst then counted drops");
  expect(log.find("strata: listening 127.0.0.1:") != std::string::npos && log.find("workers 8, queued generations 4") != std::string::npos,
         "startup summary");
}

// Issue 20 (docs/validation/2026-10-21-windows-gpu-session.md): a reply that reaches a token the
// frontend refuses to output still answers the generic 500 (stream: an internal_error chunk), and
// the request log now says why with a fixed, text-free log-only code, `undecodable_token`, for the
// complete and the streaming form alike. Token 256 is the byte pack's <|im_start|> (CONTROL, not
// an end-of-turn token).
void undecodable_output_logs_its_own_code() {
  auto server = start_server({{"logging.requests", "true"}}, {"--token-ids=256"});
  if (!server) return;
  const auto complete = raw(*server, chat_request(*server, chat_body()));
  expect(complete && complete->status == 500, "the client still sees the generic 500");
  const auto sse = test_support::chat(*server, test_support::chat_body(R"("stream":true)"));
  expect(sse.streamed, "the stream starts before the token is refused");
  std::this_thread::sleep_for(200ms);
  const std::string log = server->stderr_text();
  std::istringstream in(log);
  bool complete_logged = false;
  bool stream_logged = false;
  for (std::string line; std::getline(in, line);) {
    std::istringstream f(line);
    std::string req, seq, route, status, code;
    f >> req >> seq >> route >> status >> code;
    if (req != "req") continue;
    if (route == "chat" && status == "500" && code == "undecodable_token") complete_logged = true;
    if (route == "chat_stream" && code == "undecodable_token") stream_logged = true;
  }
  expect(complete_logged, "the complete reply logs undecodable_token: " + log);
  expect(stream_logged, "the streamed reply logs undecodable_token: " + log);
}

// Owner decisions 2026-10-21 (session log step 12), over a real socket: the built-in page
// (listener.web_ui) answers without the key while the API keeps it; the keyless loopback
// listener (listener.loopback_without_key) answers without one and still checks Host/Origin.
void web_ui_page_is_served_and_the_api_keeps_its_key() {
  auto server = start_server({{"listener.web_ui", "true"}, {"logging.requests", "true"}});
  expect(server != nullptr, "the server starts with listener.web_ui");
  if (!server) return;
  const auto page = raw(*server, raw_http::request("GET", "/", authority(*server), {}));
  // owner 2026-10-27: "change the Strata on the header page and title to M-Strata"
  expect(page && page->status == 200 && page->body.find("<title>M-Strata</title>") != std::string::npos &&
             page->body.find("<span class=\"st-header__brand\">M-Strata</span>") != std::string::npos,
         "GET / without a key is the page, named M-Strata");
  if (page) {
    expect(page->header("Content-Security-Policy") == std::optional<std::string>{std::string{secure_frontend::kWebUiContentSecurityPolicy}},
           "the page policy is on the wire, once");
    expect(page->header("Content-Type") == std::optional<std::string>{"text/html; charset=utf-8"} &&
               page->header("X-Content-Type-Options") == std::optional<std::string>{"nosniff"},
           "HTML, and nosniff stays");
  }
  const auto script = raw(*server, raw_http::request("GET", "/ui/app.js", authority(*server), {}));
  expect(script && script->status == 200 && script->header("Content-Type") == std::optional<std::string>{"text/javascript; charset=utf-8"},
         "the bundled script");
  const auto models = raw(*server, raw_http::request("GET", "/v1/models", authority(*server), {}));
  expect(models && models->status == 401, "the API still needs the key with the page on");
  const auto keyed = raw(*server, raw_http::request("GET", "/v1/models", authority(*server), {raw_http::bearer(token())}));
  expect(keyed && keyed->status == 200, "and answers with it");
  std::this_thread::sleep_for(200ms);
  const std::string log = server->stderr_text();
  expect(log.find(" web 200 - ") != std::string::npos, "the request log names the page route 'web': " + log);
}

// Session log step 16 (listener.monitor), over a real socket: GET /metrics needs the key,
// reports each finished generation's counts and finish on both chat paths, never its text,
// and is valid JSON; with the switch off it is not served.
void monitor_reports_generations_without_their_text() {
  {
    auto off = start_server();
    if (!off) return;
    const auto missing = raw(*off, raw_http::request("GET", "/metrics", authority(*off), {raw_http::bearer(token())}));
    expect(missing && missing->status == 404, "listener.monitor off: /metrics is not served");
  }
  auto server = start_server({{"listener.monitor", "true"}, {"logging.requests", "true"}}, {"--tokens=3"});
  expect(server != nullptr, "the server starts with listener.monitor");
  if (!server) return;
  const auto metrics = [&server] {
    return raw(*server, raw_http::request("GET", "/metrics", authority(*server), {raw_http::bearer(token())}));
  };
  const auto unkeyed = raw(*server, raw_http::request("GET", "/metrics", authority(*server), {}));
  expect(unkeyed && unkeyed->status == 401, "the Monitor needs the key");
  const auto idle = metrics();
  expect(idle && idle->status == 200 && idle->body.find("\"state\":\"idle\"") != std::string::npos &&
             idle->body.find("\"requests\":[]") != std::string::npos,
         "idle, no requests yet: " + (idle ? idle->body.substr(0, 400) : std::string{"<none>"}));
  const auto plain = raw(*server, chat_request(*server, chat_body(R"("max_tokens":8)", "monitor-secret-words")));
  expect(plain && plain->status == 200, "a chat");
  const auto streamed = raw(*server, chat_request(*server, chat_body(R"("max_tokens":8,"stream":true)", "monitor-secret-words")));
  expect(streamed && streamed->status == 200, "a streamed chat");
  const auto after = metrics();
  const std::string body = after ? after->body : std::string{};
  expect(after && after->status == 200 && body.find("\"requests_kept\":2") != std::string::npos,
         "both generations are kept: " + body.substr(0, 600));
  std::size_t stops = 0;
  for (std::size_t at = body.find("\"finish\":\"stop\""); at != std::string::npos; at = body.find("\"finish\":\"stop\"", at + 1U)) ++stops;
  expect(stops == 2U && body.find("\"output_tokens\":3") != std::string::npos,
         "each one's finish and token count, on both paths: " + body.substr(0, 600));
  expect(body.find("\"model\":\"fake-model\"") != std::string::npos, "the served model is named");
  expect(body.find("monitor-secret-words") == std::string::npos && body.find("\"content\"") == std::string::npos,
         "no prompt and no answer in the Monitor");
  const secure_frontend::JsonLimits limits{.max_bytes = 1U << 20U, .max_depth = 8U, .max_container_entries = 4096U,
                                           .max_string_bytes = 1U << 16U, .allow_fractions = true};
  expect(std::holds_alternative<secure_frontend::JsonValue>(secure_frontend::parse_json(body, limits)), "valid JSON");
  std::this_thread::sleep_for(200ms);
  expect(server->stderr_text().find(" metrics 200 - ") != std::string::npos, "the request log names the route 'metrics'");
}

void keyless_loopback_server_answers_without_a_key() {
  test_support::ServerOptions options;
  options.no_token = true;
  options.config_literals = {{"listener.loopback_without_key", "true"}, {"listener.web_ui", "true"}};
  auto server = test_support::ServerProcess::start(options);
  const bool ready = server != nullptr && server->wait_until_listening(10s) && server->wait_for_health(test_support::kHealthReady, 15s);
  expect(ready, "a keyless loopback server starts with no key source");
  if (!ready) return;
  const std::string a = authority(*server);
  const auto models = raw(*server, raw_http::request("GET", "/v1/models", a, {}));
  expect(models && models->status == 200 && models->body.find("fake-model") != std::string::npos, "/v1/models without a key");
  const auto chat = raw(*server, raw_http::request("POST", "/v1/chat/completions", a, {"Content-Type: application/json"}, chat_body()));
  expect(chat && chat->status == 200, "a chat without a key");
  const auto rebound = raw(*server, raw_http::request("GET", "/v1/models", "evil.example:" + std::to_string(server->port()), {}));
  expect(rebound && rebound->status == 403, "a foreign Host is still refused (DNS rebinding)");
  const auto cross = raw(*server, raw_http::request("GET", "/v1/models", a, {"Origin: https://evil.example"}));
  expect(cross && cross->status == 403, "a foreign Origin is still refused (another site's page)");
  expect(server->stderr_text().find("listener.loopback_without_key") != std::string::npos,
         "the boot says, on stderr, that it runs without a key: " + server->stderr_text());
  // A key source next to the keyless switch is a configuration error, not a silently unused key.
  test_support::ServerOptions keyed = options;
  keyed.no_token = false;
  auto refused = test_support::ServerProcess::start(keyed);
  expect(refused != nullptr, "the refused server was launched");
  if (!refused) return;
  const auto code = refused->wait_exit(10s);
  expect(code.has_value() && *code != 0 && refused->stderr_text().find("token.source") != std::string::npos,
         "token.source with listener.loopback_without_key exits non-zero naming the key: " + refused->stderr_text());
}

void err_no_token_prompt_or_path_in_any_body_or_log() {
  const std::string marker = "ZZPROMPTMARKERZZ";
  auto server = start_server({{"logging.requests", "true"}});
  if (!server) return;
  const std::string a = authority(*server);
  const std::string root = test_support::utf8_path(server->root());
  std::vector<std::string> bodies;
  const auto send = [&](const std::string& request) {
    if (auto c = raw_http::Connection::open("127.0.0.1", server->port()); c && c->send(request)) bodies.push_back(c->read_until_close(10s));
  };
  send(chat_request(*server, chat_body(R"("max_tokens":2)", marker)));
  send(chat_request(*server, chat_body(R"("max_tokens":2,"tools":[")" + marker + "\"]")));
  send(chat_request(*server, "{\"" + marker + "\":1}"));
  send(chat_request(*server, R"({"model":")" + marker + R"(","messages":[{"role":"user","content":"Hi"}]})"));
  send(raw_http::request("GET", "/" + marker, a, {raw_http::bearer(token())}));
  send(raw_http::request("GET", "/health", a, {"Authorization: Bearer " + marker + "-0123456789abcdefghijk"}));
  send(raw_http::request("GET", "/health", a, {raw_http::bearer(token()), "X-Marker: " + marker}));
  send(raw_http::request("GET", "/health", marker + ":1", {raw_http::bearer(token())}));
  send(raw_http::request("GET", "/health", a, {raw_http::bearer(token()), "Origin: http://" + marker}));
  send(chat_request(*server, chat_body(R"("max_tokens":2,"temperature":9.5)", marker)));
  std::this_thread::sleep_for(200ms);
  const std::string log = server->stderr_text();
  for (std::size_t i = 0; i < bodies.size(); ++i) {
    // The 200 chat legitimately echoes generated text, never the prompt; the fake emits token 7, not the marker.
    expect(bodies[i].find(marker) == std::string::npos, "response " + std::to_string(i) + " never reflects the marker");
    expect(bodies[i].find(token()) == std::string::npos, "response " + std::to_string(i) + " never contains the token");
    expect(bodies[i].find(root) == std::string::npos, "response " + std::to_string(i) + " never contains a configured path");
  }
  expect(log.find(marker) == std::string::npos && log.find(token()) == std::string::npos && log.find(root) == std::string::npos,
         "stderr (with logging.requests=true) has no marker, token or path");
}

// SP3 Task 22 (spec §10 Windows parity): these three exercise the Winsock paths;
// they are registered on Windows only, but compiled and runnable everywhere —
// on Linux they exercise the same pool code and must pass by hand.
void win_refusal_is_read_not_reset() {
  auto server = start_server({{"limits.concurrency.max_concurrent_requests", "1"}});
  if (!server) return;
  auto holder = raw_http::Connection::open("127.0.0.1", server->port());
  expect(holder && holder->send("POST /v1/chat/completions HTTP/1.1\r\nHost: " + authority(*server) + "\r\n" + raw_http::bearer(token()) +
                                "\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{"), "the only worker is held in P3");
  std::this_thread::sleep_for(200ms);
  int complete = 0;
  std::vector<std::thread> flood;
  std::mutex m;
  for (int i = 0; i < 30; ++i) {
    flood.emplace_back([&] {
      auto c = raw_http::Connection::open("127.0.0.1", server->port());
      if (!c || !c->send(raw_http::request("GET", "/health", authority(*server), {raw_http::bearer(token())}))) return;
      const auto r = raw_http::parse_response(c->read_until_close(5s));
      std::lock_guard g(m);
      if (r && r->status == 503 && r->body == secure_frontend::render_error_body(secure_frontend::ApiError::server_busy)) ++complete;
    });
  }
  for (auto& t : flood) t.join();
  expect(complete == 30, "every refused client read the whole 503 (lingering close, no RST): " + std::to_string(complete));
}

void win_eviction_wakes_worker_within_one_slice() {
  auto server = start_server({{"limits.concurrency.max_concurrent_requests", "1"}, {"limits.http.header_deadline_ms", "30000"}});
  if (!server) return;
  auto slow = raw_http::Connection::open("127.0.0.1", server->port());
  expect(slow && slow->send("GET /health HTTP/1.1\r\nHost: "), "P1 holder");
  std::this_thread::sleep_for(200ms);
  const auto started = std::chrono::steady_clock::now();
  const auto r = raw(*server, raw_http::request("GET", "/health", authority(*server), {raw_http::bearer(token())}), 5s);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(r && r->status == 200, "the new client is served");
  expect(elapsed < 250ms, "the evicted worker woke within one 20 ms slice plus slack (" +
                          std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()) + " ms)");
}

void win_disconnect_detected_by_peek() {
  auto r = start_recorded("wpeek", {}, {"--tokens=10000", "--token-interval-ms=5"});
  if (!r.server) return;
  {
    auto c = raw_http::Connection::open("127.0.0.1", r.server->port());
    expect(c && c->send(chat_request(*r.server, chat_body(""))), "long chat");
    std::this_thread::sleep_for(300ms);
  }
  bool stopped = false;
  for (int i = 0; i < 50 && !stopped; ++i) {
    stopped = record_of(r.record).find("\nSTOP\n") != std::string::npos;
    if (!stopped) std::this_thread::sleep_for(20ms);
  }
  expect(stopped, "recv(MSG_PEEK) saw the close on Winsock and STOP was sent within 1 s");
}

// SP3 M2 (Task 5): §3.3 mid-character cut and decision 4 (timeout is 200 length).
void utf8_non_stream_max_tokens_cut_mid_character_returns_length() {
  auto server = test_support::start_stream_server({"--token-ids=" + test_support::token_ids_for("A\xE2\x82\xAC")});
  expect(server != nullptr, "the server becomes ready");
  if (!server) return;
  const auto result = server->post_chat(test_support::chat_body(R"("max_tokens":3)"), 10s);
  expect(result && result->status == 200, "a max_tokens cut mid-character is a 200, not a decode failure");
  if (result) {
    expect(test_support::complete_text(result->body) == "A", "the held lead bytes are dropped: " + result->body);
    expect(test_support::complete_finish(result->body) == "length", "finish_reason length");
  }
}

void generation_timeout_returns_text_with_length() {
  auto server = test_support::start_stream_server({"--tokens=100", "--token-interval-ms=300"}, {{"limits.engine.generation_timeout_s", "1"}});
  expect(server != nullptr, "the server becomes ready");
  if (!server) return;
  const auto result = server->post_chat(test_support::chat_body(R"("max_tokens":100)"), 10s);
  expect(result && result->status == 200, "a timed-out generation is a 200 (decision 4)");
  if (result) {
    const auto text = test_support::complete_text(result->body);
    expect(!text.empty() && text.size() <= 5U && text.find_first_not_of('H') == std::string::npos, "the text so far: " + text);
    expect(test_support::complete_finish(result->body) == "length", "finish_reason length");
  }
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("max_tokens":100,"stream":true)"), {.timeout = 10s});
  expect(streamed.end == ChatExchangeEnd::complete && test_support::stream_finish(streamed) == "length" &&
             !test_support::stream_text(streamed).empty(), "stream: text so far, final chunk length");
}

std::string long_token_ids() { return "--token-ids=" + std::to_string(test_support::kLongTokenId); }

void stream_matches_non_stream_text() {
  const std::string text = "Hi \xE2\x82\xAC \xF0\x9F\x98\x80!";
  auto server = test_support::start_stream_server({"--token-ids=" + test_support::token_ids_for(text)});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const auto complete = test_support::chat(*server, test_support::chat_body(R"("max_tokens":64)"));
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("max_tokens":64,"stream":true)"));
  expect(complete.end == ChatExchangeEnd::complete && complete.head.status == 200 && !complete.streamed, "complete response");
  expect(streamed.end == ChatExchangeEnd::complete && streamed.streamed && streamed.done_seen && streamed.terminal_chunk_seen,
         "stream with [DONE] and the terminal chunk");
  expect(test_support::complete_text(complete.body) == text, "non-stream text");
  expect(test_support::stream_text(streamed) == text, "stream text equals non-stream text: " + test_support::stream_text(streamed));
  expect(test_support::stream_finish(streamed) == "stop" && test_support::complete_finish(complete.body) == "stop", "both stop");
  const auto head = streamed.head;
  expect(head.header("Cache-Control") == "no-store" && head.header("Connection") == "close" && !head.header("Content-Length"),
         "stream headers (§3.2, §4.6)");
}

void dos02_queue_full_with_streams_is_503_before_headers() {
  auto server = test_support::start_stream_server({"--tokens=100000", "--token-interval-ms=20"});
  expect(server != nullptr, "server ready");
  if (!server) return;
  std::vector<secure_frontend::LoopbackStreamConnection> held;
  for (int index = 0; index < 5; ++index) {  // 1 running + max_queued_generations (4, derived at 8 workers)
    auto connection = test_support::open_raw(*server, test_support::chat_body(R"("stream":true)"));
    expect(connection.has_value(), "held stream " + std::to_string(index));
    if (connection) held.push_back(std::move(*connection));
  }
  std::this_thread::sleep_for(500ms);
  const auto refused = test_support::chat(*server, test_support::chat_body(R"("stream":true)"));
  expect(refused.end == ChatExchangeEnd::complete && refused.head.status == 503 && !refused.streamed, "a plain 503, never a stream");
  expect(test_support::error_code_of(refused.body) == "server_busy" && refused.head.header("Retry-After") == "1", "server_busy + Retry-After: 1");
  const auto type = refused.head.header("Content-Type");
  expect(type && type->rfind("application/json", 0) == 0, "no text/event-stream header before admission");
  held.clear();  // every held client goes away: the running stream is cancelled, the queue empties
  bool served = false;
  for (int attempt = 0; attempt < 50 && !served; ++attempt) {
    served = test_support::chat(*server, test_support::chat_body(R"("max_tokens":1)")).head.status == 200;
    if (!served) std::this_thread::sleep_for(100ms);
  }
  expect(served, "the server serves again once the held clients left");
}

void ext01_extension_fields_rejected_on_stream_path() {
  const auto dir = test_support::make_temp_dir("ext01s");
  const auto record = dir / "record.txt";
  auto server = test_support::start_stream_server({"--record-file=" + test_support::utf8_path(record)});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const std::vector<std::string> bodies{
      test_support::chat_body(R"("stream":true,"tools":[])"), test_support::chat_body(R"("stream":true,"tool_choice":"auto")"),
      test_support::chat_body(R"("stream":true,"functions":[])"), test_support::chat_body(R"("stream":true,"function_call":"auto")"),
      test_support::chat_body(R"("stream":true,"parallel_tool_calls":false)"), test_support::chat_body(R"("stream":true,"mcp_servers":[])"),
      R"({"model":"fake-model","stream":true,"messages":[{"role":"tool","content":"x"}]})",
      R"({"model":"fake-model","stream":true,"messages":[{"role":"function","content":"x"}]})"};
  for (const auto& body : bodies) {
    const auto result = test_support::chat(*server, body);
    expect(result.head.status == 400 && !result.streamed && test_support::error_code_of(result.body) == "extension_disabled",
           "400 extension_disabled before any stream header: " + body);
  }
  const std::string recorded = "\n" + secure_frontend_test::read_file(record);
  expect(recorded.find("\nGEN ") == std::string::npos, "no GEN reached the engine");
}

void dos02_slow_reader_never_backpressures_engine() {
  // engine.repeat_stop_tokens 0 (SP5 M1 Task 8): the filler token repeats 1024 times, so the
  // pin's default runaway rule (256) would legitimately cut this reply; this test is about
  // reader backpressure, not the repeat rule.
  auto server = test_support::start_stream_server({long_token_ids(), "--tokens=1024", "--ready-context=65536"},
                                           {{"limits.chat.max_completion_tokens", "8192"}, {"engine.repeat_stop_tokens", "0"}});
  expect(server != nullptr, "server ready");
  if (!server) return;
  auto slow = test_support::open_raw(*server, test_support::chat_body(R"("stream":true,"max_tokens":1024)"), 4096);
  expect(slow.has_value(), "slow client connected; it reads nothing for now");
  if (!slow) return;
  const auto start = std::chrono::steady_clock::now();
  const auto second = test_support::chat(*server, test_support::chat_body(R"("max_tokens":1)"), {.timeout = 5s});
  const auto waited = std::chrono::steady_clock::now() - start;
  expect(second.end == ChatExchangeEnd::complete && second.head.status == 200, "a second chat is served while the first client reads nothing");
  expect(waited < 2s, "1 MiB reached DONE at engine speed (< 2 s): " +
                          std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(waited).count()) + " ms");
  const auto first = secure_frontend::read_chat_exchange(*slow, {.timeout = 20s});
  expect(first.end == ChatExchangeEnd::complete && test_support::stream_text(first).size() == 1048576U, "the slow reader then gets all 1 MiB");
  expect(test_support::content_event_count(first) <= 80U, "coalesced into 16 KiB deltas, not 1024 small events");
}

void finish_stop_length_and_generated_bytes_cap() {
  {
    auto server = test_support::start_stream_server({"--tokens=3"});
    expect(server != nullptr, "server A ready");
    if (!server) return;
    for (const bool stream : {false, true}) {
      const std::string mode = stream ? R"(,"stream":true)" : "";
      const auto stop = test_support::chat(*server, test_support::chat_body(R"("max_tokens":3)" + mode));
      const auto length = test_support::chat(*server, test_support::chat_body(R"("max_tokens":2)" + mode));
      const auto finish = [stream](const secure_frontend::ChatExchangeResult& r) {
        return stream ? test_support::stream_finish(r) : test_support::complete_finish(r.body);
      };
      const auto text = [stream](const secure_frontend::ChatExchangeResult& r) {
        return stream ? test_support::stream_text(r) : test_support::complete_text(r.body);
      };
      expect(finish(stop) == "stop" && text(stop) == "HHH", "EOS: stop" + mode);
      expect(finish(length) == "length" && text(length) == "HH", "max_tokens: length" + mode);
    }
  }
  auto server = test_support::start_stream_server({long_token_ids(), "--tokens=4", "--ready-context=65536"},
                                           {{"limits.chat.max_generated_bytes", "2048"}});
  expect(server != nullptr, "server B ready");
  if (!server) return;
  const auto complete = test_support::chat(*server, test_support::chat_body(R"("max_tokens":4)"));
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("max_tokens":4,"stream":true)"));
  expect(test_support::complete_text(complete.body) == std::string(2048U, 'a') && test_support::complete_finish(complete.body) == "length",
         "generated-bytes cap: 2048 bytes, length");
  expect(test_support::stream_text(streamed) == std::string(2048U, 'a') && test_support::stream_finish(streamed) == "length" &&
             streamed.end == ChatExchangeEnd::complete, "the same on the stream");
}

void utf8_timeout_cut_mid_character_returns_length() {
  // 'A' at once, then E2, 82 at 400 ms steps; the 1 s timeout falls before the final AC (1200 ms).
  auto server = test_support::start_stream_server({"--token-ids=" + test_support::token_ids_for("A\xE2\x82\xAC"), "--token-interval-ms=400"},
                                           {{"limits.engine.generation_timeout_s", "1"}});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("max_tokens":4,"stream":true)"), {.timeout = 10s});
  expect(streamed.end == ChatExchangeEnd::complete && test_support::stream_text(streamed) == "A", "the cut lead bytes are dropped");
  expect(test_support::stream_finish(streamed) == "length", "timeout is length (decision 4)");
}

void dos02_stream_disconnect_sends_stop_and_frees_engine() {
  const auto dir = test_support::make_temp_dir("dos02d");
  const auto record = dir / "record.txt";
  auto server = test_support::start_stream_server({"--tokens=10000", "--token-interval-ms=5", "--ready-context=65536",
                                            "--record-file=" + test_support::utf8_path(record)});
  expect(server != nullptr, "server ready");
  if (!server) return;
  int contents = 0;
  secure_frontend::ChatRequestOptions options;
  options.on_item = [&contents](const secure_frontend::SseItem& item) {
    contents += item.text.find(R"("delta":{"content")") != std::string::npos;
    return contents < 3;
  };
  const auto cancelled = test_support::chat(*server, test_support::chat_body(R"("stream":true,"max_tokens":4096)"), options);
  expect(cancelled.end == ChatExchangeEnd::cancelled, "the client closed after 3 content chunks");
  expect(test_support::file_has_line(record, "STOP", 2s), "the frontend sent STOP to the engine");
  const auto start = std::chrono::steady_clock::now();
  const auto next = test_support::chat(*server, test_support::chat_body(R"("max_tokens":1)"), {.timeout = 3s});
  expect(next.head.status == 200 && std::chrono::steady_clock::now() - start < 1s, "the next request is served within 1 s");
}

// A client that never reads (4 KiB receive buffer) while the engine streams 16 MiB.
struct StalledStream final {
  std::filesystem::path record;
  std::unique_ptr<test_support::ServerProcess> server;
  std::optional<secure_frontend::LoopbackStreamConnection> slow;
  StalledStream() {
    const auto dir = test_support::make_temp_dir("stall");
    record = dir / "record.txt";
    // Volume, not time (Windows Sleep(1) is ~15.6 ms; SP0 S2 saw Winsock accept one 32 MiB *blocking* send whole): 15 MiB at once, then
    // 1024 more tokens at 50 ms keep the generation running for ~50 s while the unread socket fills.
    server = test_support::start_stream_server({long_token_ids(), "--tokens=16384", "--burst-tokens=15360", "--token-interval-ms=50", "--ready-context=65536",
                                         "--record-file=" + test_support::utf8_path(record)},
                                        {{"limits.http.write_stall_ms", "500"}, {"limits.chat.max_completion_tokens", "16384"},
                                         {"limits.chat.max_generated_bytes", "16777216"},
                                         // SP5 M1 Task 8: the filler repeats far past the pin's 256-token runaway bound;
                                         // this fixture is about the write-stall cut, so the rule is off here.
                                         {"engine.repeat_stop_tokens", "0"}});
    if (server) slow = test_support::open_raw(*server, test_support::chat_body(R"("stream":true,"max_tokens":16384)"), 4096);
  }
};

void dos02_slow_stream_reader_hits_write_stall() {
  StalledStream stalled;
  expect(stalled.server && stalled.slow, "server ready, slow client connected");
  if (!stalled.server || !stalled.slow) return;
  expect(test_support::file_has_line(stalled.record, "STOP", 45s), "no progress for write_stall_ms (500) cancels with STOP");
  const auto next = test_support::chat(*stalled.server, test_support::chat_body(R"("max_tokens":1)"), {.timeout = 5s});
  expect(next.head.status == 200, "the worker and the engine are free again");
}

void stream_cut_has_no_terminal_chunk() {
  StalledStream stalled;
  expect(stalled.server && stalled.slow, "server ready, slow client connected");
  if (!stalled.server || !stalled.slow) return;
  expect(test_support::file_has_line(stalled.record, "STOP", 45s), "the stall cut the stream");
  const auto read = secure_frontend::read_chat_exchange(*stalled.slow, {.timeout = 30s});
  expect(read.streamed && read.end == ChatExchangeEnd::cut && !read.terminal_chunk_seen && !read.done_seen,
         "a cut stream ends without [DONE] and without the terminal chunk, so SDKs raise instead of accepting it");
}

void stream_cancel_during_prefill_frees_engine() {
  const auto dir = test_support::make_temp_dir("prefill");
  const auto record = dir / "record.txt";
  auto server = test_support::start_stream_server({"--prefill-delay-ms=5000", "--record-file=" + test_support::utf8_path(record)});
  expect(server != nullptr, "server ready");
  if (!server) return;
  secure_frontend::ChatRequestOptions options;
  options.on_item = [](const secure_frontend::SseItem&) { return false; };  // close right after the role chunk
  const auto start = std::chrono::steady_clock::now();
  (void)test_support::chat(*server, test_support::chat_body(R"("stream":true)"), options);
  expect(test_support::file_has_line(record, "STOP", 1s), "STOP during prefill");
  expect(std::chrono::steady_clock::now() - start < 2s, "seen within a poll slice, not after the 5 s prefill");
  const auto next = test_support::chat(*server, test_support::chat_body(R"("max_tokens":1)"), {.timeout = 10s});
  expect(next.head.status == 200, "the engine serves the next request");
}

void log_stream_outcomes_use_fixed_codes() {
  auto server = test_support::start_stream_server({"--tokens=200", "--token-interval-ms=5"}, {{"logging.requests", "true"}});
  expect(server != nullptr, "server ready");
  if (!server) return;
  (void)test_support::chat(*server, test_support::chat_body(R"("stream":true,"max_tokens":2)"));
  secure_frontend::ChatRequestOptions options;
  options.on_item = [](const secure_frontend::SseItem& item) { return item.text.find(R"("delta":{"content")") == std::string::npos; };
  (void)test_support::chat(*server, test_support::chat_body(R"("stream":true,"max_tokens":200)"), options);
  std::this_thread::sleep_for(500ms);
  const auto log = server->stderr_text();
  expect(log.find(" chat_stream 200 - ") != std::string::npos, "a normal stream logs '-': " + log);
  expect(log.find(" chat_stream 200 client_gone ") != std::string::npos, "a cancelled stream logs client_gone");
  expect(log.find("fake-model") == std::string::npos && log.find("Hi") == std::string::npos, "no model field or prompt text in the log");
}

void stream_keepalive_comment_during_prefill() {
  auto server = test_support::start_stream_server({"--prefill-delay-ms=2500"}, {{"limits.http.stream_keepalive_ms", "1000"}});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("stream":true)"), {.timeout = 15s});
  expect(streamed.end == ChatExchangeEnd::complete, "the stream completes");
  expect(test_support::comments_before_content(streamed) >= 2U,
         "at least 2 ': keep-alive' comments during a 2.5 s prefill: " + std::to_string(test_support::comments_before_content(streamed)));
}

void err_mid_stream_is_error_event_then_done() {
  auto server = test_support::start_stream_server({"--tokens=5", "--err-mid-gen=2"});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("stream":true)"));
  expect(test_support::stream_text(streamed) == "HH" && test_support::stream_error_code(streamed) == "engine_error", "text, then engine_error");
  expect(streamed.done_seen && streamed.terminal_chunk_seen && streamed.end == ChatExchangeEnd::complete, "[DONE] and the terminal chunk follow");
  expect(test_support::stream_finish(streamed) == "<none>", "no finish_reason after an error");
}

void crash_mid_stream_is_engine_unavailable_event() {
  auto server = test_support::start_stream_server({"--tokens=10", "--crash-mid-gen=3", "--token-interval-ms=10"});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("stream":true)"));
  expect(test_support::stream_error_code(streamed) == "engine_unavailable" && streamed.terminal_chunk_seen, "engine_unavailable event, ended normally");
  bool restarting = false;
  for (int poll = 0; poll < 100 && !restarting; ++poll) {
    const auto health = server->get("/health");
    restarting = health && health->body.find(R"("recovery":"restarting")") != std::string::npos;
    if (!restarting) std::this_thread::sleep_for(20ms);
  }
  expect(restarting, "/health follows SP2: recovery=restarting");
}

void nonfinite_is_engine_error_and_engine_stays_ready() {
  auto server = test_support::start_stream_server({"--finish=nonfinite"});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const auto engines_before = server->descendants();
  const auto complete = test_support::chat(*server, test_support::chat_body(""));
  expect(complete.head.status == 503 && test_support::error_code_of(complete.body) == "engine_error", "non-stream: 503 engine_error");
  expect(complete.head.header("x-should-retry") == "false", "SDK retries suppressed");
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("stream":true)"));
  expect(test_support::stream_error_code(streamed) == "engine_error" && streamed.done_seen && streamed.terminal_chunk_seen, "stream: error event");
  expect(server->wait_for_health(test_support::kHealthReady, 2s), "the engine stays ready (SP0 E3)");
  expect(server->descendants() == engines_before, "no restart");
}

void engine_error_carries_should_retry_false() {
  auto server = test_support::start_stream_server({"--tokens=3", "--err-mid-gen=0"});
  expect(server != nullptr, "server ready");
  if (!server) return;
  const auto result = test_support::chat(*server, test_support::chat_body(""));
  expect(result.head.status == 503 && test_support::error_code_of(result.body) == "engine_error", "ERR is 503 engine_error");
  expect(result.head.header("x-should-retry") == "false", "x-should-retry: false (risk R5)");
}

void dos02_shutdown_during_stream_ends_with_error_event() {
  auto server = test_support::start_stream_server({"--tokens=100000", "--token-interval-ms=10"});
  expect(server != nullptr, "server ready");
  if (!server) return;
  int contents = 0;
  bool stopped = false;
  secure_frontend::ChatRequestOptions options;
  options.timeout = 10s;
  options.on_item = [&](const secure_frontend::SseItem& item) {
    contents += item.text.find(R"("delta":{"content")") != std::string::npos;
    if (contents == 2 && !stopped) {
      stopped = true;
      server->stop(test_support::ServerProcess::Stop::term);
    }
    return true;
  };
  const auto streamed = test_support::chat(*server, test_support::chat_body(R"("stream":true)"), options);
  expect(test_support::stream_error_code(streamed) == "server_shutting_down", "in-stream server_shutting_down");
  expect(streamed.done_seen && streamed.terminal_chunk_seen && streamed.end == ChatExchangeEnd::complete, "[DONE] and the terminal chunk");
  expect(server->wait_exit(5s) == 0, "exit 0 within SP2's 5 s bound");
}

// ---- Gemma 4 plan 1 Task 11: the gemma4 profile end to end on the fake engine ----------------
// The server runs --expert --profile gemma4 over write_spm_tokenizer_pack (the synthetic
// spm_tokenizer() pack); the fake engine's flags travel in ServerOptions::fake_engine_options,
// since strata_gemma4's allowlist refuses them as engine.args. The fake models the Gemma engine:
// it emits the reply ids, the last one <turn|> (the engine ends its turn by itself), then
// `DONE ... stop`. Every expected prompt is written by hand from the template bytes (spec §2.5)
// and the pack's literals, never through the renderer or encoder under test.
const secure_frontend::TokenizerPack& gemma4_pack() {
  static const auto pack = test_support::spm_tokenizer();
  return pack;
}

const std::string kGemma4Space{test_support::kSpmSpace};  // U+2581, a space in spm_bpe text

// The synthetic pack's ids of `literals`, in order; a literal the pack lacks fails the case.
std::vector<std::size_t> gemma4_ids(const std::vector<std::string>& literals) {
  std::vector<std::size_t> ids;
  for (const auto& literal : literals) {
    const auto id = test_support::token_id(gemma4_pack(), literal);
    expect(id < gemma4_pack().tokens_by_id.size(), "test setup: the synthetic pack holds " + literal);
    ids.push_back(id);
  }
  return ids;
}

std::vector<std::string> gemma4_join(const std::vector<std::vector<std::string>>& parts) {
  std::vector<std::string> joined;
  for (const auto& part : parts) joined.insert(joined.end(), part.begin(), part.end());
  return joined;
}

// Text pieces as the pack's spm_bpe encoding: spaces are U+2581; "Hi", "<U+2581>world" and
// "thought" are merges; every other ASCII character is its own token; "\n" is one token.
const std::vector<std::string> kGemma4User{"u", "s", "e", "r", "\n"};
const std::vector<std::string> kGemma4Model{"m", "o", "d", "e", "l", "\n"};
const std::vector<std::string> kGemma4System{"s", "y", "s", "t", "e", "m", "\n"};
const std::vector<std::string> kGemma4HelloWorld{"H", "e", "l", "l", "o", kGemma4Space + "world"};
const std::vector<std::string> kGemma4HiWorld{"Hi", kGemma4Space + "world"};

// The user text every gemma4 case sends; the template trims it (Jinja's trim) to "Hello world".
constexpr std::string_view kGemma4UserContent = "  Hello world  ";

// Spec §2.5, thinking off: <bos><|turn>user\nHello world<turn|>\n<|turn>model\n<|channel>thought\n<channel|>
std::vector<std::size_t> gemma4_prompt_thinking_off() {
  return gemma4_ids(gemma4_join({{"<bos>", "<|turn>"}, kGemma4User, kGemma4HelloWorld, {"<turn|>", "\n", "<|turn>"}, kGemma4Model,
                                 {"<|channel>", "thought", "\n", "<channel|>"}}));
}

// Thinking on: the system turn holds <|think|> and its newline (the newer template, 2026-10-28) and no text (no system
// message), and the generation prompt is the open model turn:
// <bos><|turn>system\n<|think|>\n<turn|>\n<|turn>user\nHello world<turn|>\n<|turn>model\n
std::vector<std::size_t> gemma4_prompt_thinking_on() {
  return gemma4_ids(gemma4_join({{"<bos>", "<|turn>"}, kGemma4System, {"<|think|>", "\n", "<turn|>", "\n", "<|turn>"}, kGemma4User,
                                 kGemma4HelloWorld, {"<turn|>", "\n", "<|turn>"}, kGemma4Model}));
}

// A system message and an earlier exchange, thinking off:
// <bos><|turn>system\nBe brief.<turn|>\n<|turn>user\nHi<turn|>\n<|turn>model\nHi world<turn|>\n
// <|turn>user\nHello world<turn|>\n<|turn>model\n<|channel>thought\n<channel|>
std::vector<std::size_t> gemma4_prompt_with_history() {
  return gemma4_ids(gemma4_join({{"<bos>", "<|turn>"}, kGemma4System, {"B", "e", kGemma4Space, "b", "r", "i", "e", "f", "."},
                                 {"<turn|>", "\n", "<|turn>"}, kGemma4User, {"Hi"}, {"<turn|>", "\n", "<|turn>"}, kGemma4Model,
                                 kGemma4HiWorld, {"<turn|>", "\n", "<|turn>"}, kGemma4User, kGemma4HelloWorld,
                                 {"<turn|>", "\n", "<|turn>"}, kGemma4Model, {"<|channel>", "thought", "\n", "<channel|>"}}));
}

std::string gemma4_csv(const std::vector<std::size_t>& ids) {
  std::string csv;
  for (const std::size_t id : ids) csv += (csv.empty() ? "" : ",") + std::to_string(id);
  return csv;
}

// The prompt ids of one recorded `GEN <max_new> [k=v ...] <ids>` line (its last field).
std::vector<std::size_t> gen_ids(const std::string& gen) {
  std::vector<std::size_t> ids;
  std::stringstream field(gen.substr(gen.rfind(' ') + 1U));
  for (std::string id; std::getline(field, id, ',');) {
    if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) return {};
    ids.push_back(static_cast<std::size_t>(std::stoull(id)));
  }
  return ids;
}

std::string gemma4_body(const std::string& extra,
                        const std::string& messages = R"([{"role":"user","content":")" + std::string{kGemma4UserContent} + R"("}])") {
  return R"({"model":"fake-model","messages":)" + messages + R"(,"max_tokens":64)" + (extra.empty() ? "" : "," + extra) + "}";
}

// choices[0].message.<key> of a complete response, absent when the message has no such string.
std::optional<std::string> message_string(const std::string& body, const std::string_view key) {
  const auto root = test_support::parse_test_json(body);
  const auto* choice = root ? test_support::first_choice(*root) : nullptr;
  return test_support::string_at(choice != nullptr ? test_support::json_member(*choice, "message") : nullptr, key);
}

// The joined choices[0].delta.reasoning_content of a stream; *first_reasoning / *first_content
// get the index of the first data item carrying each (the items' size when none does).
std::string stream_reasoning(const secure_frontend::ChatExchangeResult& result, std::size_t* first_reasoning = nullptr,
                             std::size_t* first_content = nullptr) {
  std::string text;
  if (first_reasoning != nullptr) *first_reasoning = result.items.size();
  if (first_content != nullptr) *first_content = result.items.size();
  for (std::size_t index = 0; index < result.items.size(); ++index) {
    const auto& item = result.items[index];
    if (item.kind != secure_frontend::SseItemKind::data || item.text == "[DONE]") continue;
    const auto root = test_support::parse_test_json(item.text);
    const auto* choice = root ? test_support::first_choice(*root) : nullptr;
    const auto* delta = choice != nullptr ? test_support::json_member(*choice, "delta") : nullptr;
    if (const auto reasoning = test_support::string_at(delta, "reasoning_content")) {
      text += *reasoning;
      if (first_reasoning != nullptr && *first_reasoning == result.items.size()) *first_reasoning = index;
    }
    // The role chunk carries "content":"" (sse_encoder.cpp); the first answer delta is the first
    // non-empty content.
    if (const auto content = test_support::string_at(delta, "content");
        content.has_value() && !content->empty() && first_content != nullptr && *first_content == result.items.size()) {
      *first_content = index;
    }
  }
  return text;
}

// Every data payload of a stream, joined (for "no tag or channel name ever leaks" checks).
std::string stream_payloads(const secure_frontend::ChatExchangeResult& result) {
  std::string joined;
  for (const auto& item : result.items) {
    if (item.kind == secure_frontend::SseItemKind::data) joined += item.text + "\n";
  }
  return joined;
}

struct Gemma4Server final {
  std::filesystem::path record;
  std::unique_ptr<test_support::ServerProcess> server;
};

// A ready gemma4 server whose fake engine answers every GEN with `reply` (pack literals).
Gemma4Server start_gemma4(const std::string& tag, const std::vector<std::string>& reply,
                          std::map<std::string, std::string> literals = {}) {
  Gemma4Server started;
  started.record = test_support::make_temp_dir(tag) / "record.txt";
  test_support::ServerOptions options;
  options.profile = "gemma4";
  options.config_literals = std::move(literals);
  options.fake_engine_options = {"--token-ids=" + gemma4_csv(gemma4_ids(reply)),
                                 "--record-file=" + test_support::utf8_path(started.record)};
  started.server = test_support::ServerProcess::start(options);
  const bool ready = started.server != nullptr && started.server->wait_until_listening(10s) &&
                     started.server->wait_for_health(test_support::kHealthReady, 15s);
  expect(ready, "the --expert --profile gemma4 server is ready" + (started.server ? ": " + started.server->stderr_text() : std::string{}));
  if (!ready) started.server.reset();
  return started;
}

std::string head_of(const secure_frontend::ChatExchangeResult& result) {
  return "status=" + std::to_string(result.head.status) + " body=" + result.body.substr(0, 400);
}

// Plan Task 11 RED gemma4_profile_chats_end_to_end: thinking off (no reasoning_effort: the
// template's default, D10), complete and streamed. The answer is the reply text, the finish is
// stop, no reasoning_content; each GEN's ids are the rendered prompt: the trimmed user turn
// and the closed empty thought channel. reasoning_effort "none" renders the same prompt, and a
// system message with an earlier exchange renders the system turn and the model's turn.
void gemma4_profile_chats_end_to_end() {
  auto started = start_gemma4("g4e2e", gemma4_join({kGemma4HiWorld, {"<turn|>"}}));
  if (!started.server) return;
  auto& server = *started.server;
  const auto complete = test_support::chat(server, gemma4_body(""));
  expect(complete.end == ChatExchangeEnd::complete && complete.head.status == 200 && !complete.streamed,
         "gemma4 complete chat answers 200: " + head_of(complete));
  expect(test_support::complete_text(complete.body) == "Hi world", "the answer is the reply's text: " + complete.body.substr(0, 400));
  expect(test_support::complete_finish(complete.body) == "stop", "<turn|> ends the reply as stop: " + complete.body.substr(0, 400));
  expect(!message_string(complete.body, "reasoning_content").has_value(), "thinking off: no reasoning_content");
  expect(complete.body.find("turn|") == std::string::npos, "the end-of-turn token is never output");

  const auto streamed = test_support::chat(server, gemma4_body(R"("stream":true)"));
  expect(streamed.end == ChatExchangeEnd::complete && streamed.head.status == 200 && streamed.streamed && streamed.done_seen &&
             streamed.terminal_chunk_seen,
         "gemma4 streamed chat: [DONE] and the terminal chunk: " + head_of(streamed));
  expect(test_support::stream_text(streamed) == "Hi world", "stream text: " + test_support::stream_text(streamed));
  expect(test_support::stream_finish(streamed) == "stop", "stream finish: " + test_support::stream_finish(streamed));
  expect(stream_reasoning(streamed).empty(), "thinking off: no reasoning delta");

  const auto none = test_support::chat(server, gemma4_body(R"("reasoning_effort":"none")"));
  expect(none.head.status == 200 && test_support::complete_text(none.body) == "Hi world",
         "reasoning_effort none is served on gemma4: " + head_of(none));
  const auto history = test_support::chat(
      server, gemma4_body("", R"([{"role":"system","content":"Be brief."},{"role":"user","content":"Hi"},)"
                              R"({"role":"assistant","content":"Hi world"},{"role":"user","content":")" +
                                  std::string{kGemma4UserContent} + R"("}])"));
  expect(history.head.status == 200 && test_support::complete_text(history.body) == "Hi world",
         "a system message and an earlier exchange are served: " + head_of(history));

  const auto gens = gen_lines(record_of(started.record));
  expect(gens.size() == 4U, "one GEN per chat reached the engine: " + std::to_string(gens.size()));
  const auto off = gemma4_prompt_thinking_off();
  const std::vector<std::string> what{"complete", "streamed", "reasoning_effort none"};
  for (std::size_t index = 0; index < std::min<std::size_t>(gens.size(), 3U); ++index) {
    expect(gen_ids(gens[index]) == off, "the " + what[index] + " GEN's ids are the rendered thinking-off prompt (" +
                                            gemma4_csv(off) + "): " + gens[index]);
  }
  if (gens.size() == 4U) {
    expect(gen_ids(gens[3]) == gemma4_prompt_with_history(),
           "the history GEN's ids are the rendered prompt (" + gemma4_csv(gemma4_prompt_with_history()) + "): " + gens[3]);
  }
}

// Plan Task 11 RED gemma4_thinking_round_trip: reasoning_effort "high" turns thinking on (the
// system turn opens with <|think|>; the generation prompt is the open model turn). The fake
// answers <|channel> "thought\n" <reasoning> <channel|> <answer> <turn|>, as the engine did on the
// box: the channel-name line is dropped, the thought is reasoning_content, the rest is content,
// and no tag or channel name is ever output. "low" is the same prompt (no levels, spec §6.6).
void gemma4_thinking_round_trip() {
  auto started = start_gemma4("g4think", gemma4_join({{"<|channel>", "thought", "\n"}, kGemma4HelloWorld, {"<channel|>"},
                                                      kGemma4HiWorld, {"<turn|>"}}));
  if (!started.server) return;
  auto& server = *started.server;
  const auto complete = test_support::chat(server, gemma4_body(R"("reasoning_effort":"high")"));
  expect(complete.end == ChatExchangeEnd::complete && complete.head.status == 200 && !complete.streamed,
         "gemma4 thinking chat answers 200: " + head_of(complete));
  expect(test_support::complete_text(complete.body) == "Hi world", "content is the answer after <channel|>: " + complete.body.substr(0, 400));
  expect(message_string(complete.body, "reasoning_content") == std::optional<std::string>{"Hello world"},
         "reasoning_content is the thought, its channel-name line dropped: " + complete.body.substr(0, 400));
  expect(test_support::complete_finish(complete.body) == "stop", "<turn|> ends the reply as stop");
  expect(complete.body.find("thought") == std::string::npos && complete.body.find("channel") == std::string::npos,
         "neither a channel tag nor the channel name is output: " + complete.body.substr(0, 400));

  const auto streamed = test_support::chat(server, gemma4_body(R"("stream":true,"reasoning_effort":"high")"));
  expect(streamed.end == ChatExchangeEnd::complete && streamed.head.status == 200 && streamed.streamed && streamed.done_seen &&
             streamed.terminal_chunk_seen,
         "gemma4 thinking stream: [DONE] and the terminal chunk: " + head_of(streamed));
  std::size_t first_reasoning = 0;
  std::size_t first_content = 0;
  expect(stream_reasoning(streamed, &first_reasoning, &first_content) == "Hello world",
         "the stream's reasoning deltas are the thought: " + stream_reasoning(streamed));
  expect(test_support::stream_text(streamed) == "Hi world", "the stream's content deltas are the answer: " + test_support::stream_text(streamed));
  expect(first_reasoning < first_content && first_content < streamed.items.size(), "the thought streams before the answer");
  expect(test_support::stream_finish(streamed) == "stop", "stream finish: " + test_support::stream_finish(streamed));
  const auto payloads = stream_payloads(streamed);
  expect(payloads.find("thought") == std::string::npos && payloads.find("channel") == std::string::npos,
         "no channel tag or name in any stream event");

  const auto low = test_support::chat(server, gemma4_body(R"("reasoning_effort":"low")"));
  expect(low.head.status == 200 && message_string(low.body, "reasoning_content") == std::optional<std::string>{"Hello world"} &&
             test_support::complete_text(low.body) == "Hi world",
         "reasoning_effort low is thinking on too: " + head_of(low));

  const auto gens = gen_lines(record_of(started.record));
  expect(gens.size() == 3U, "one GEN per chat reached the engine: " + std::to_string(gens.size()));
  const auto on = gemma4_prompt_thinking_on();
  const std::vector<std::string> what{"high complete", "high streamed", "low"};
  for (std::size_t index = 0; index < std::min<std::size_t>(gens.size(), 3U); ++index) {
    expect(gen_ids(gens[index]) == on, "the " + what[index] + " GEN's ids are the rendered thinking-on prompt (" +
                                           gemma4_csv(on) + "): " + gens[index]);
  }
}

// Plan Task 11 RED gemma4_monitor_and_page_unchanged: on the gemma4 profile the built-in page
// (listener.web_ui) is served as on qwen, and GET /metrics (listener.monitor) reports each
// generation's finish and count on both chat paths, never a prompt or an answer.
void gemma4_monitor_and_page_unchanged() {
  auto started = start_gemma4("g4mon", gemma4_join({kGemma4HiWorld, {"<turn|>"}}),
                              {{"listener.monitor", "true"}, {"listener.web_ui", "true"}, {"logging.requests", "true"}});
  if (!started.server) return;
  auto& server = *started.server;
  const auto page = raw(server, raw_http::request("GET", "/", authority(server), {}));
  expect(page && page->status == 200 && page->body.find("<title>M-Strata</title>") != std::string::npos,
         "GET / without a key is the page on gemma4");
  if (page) {
    expect(page->header("Content-Security-Policy") == std::optional<std::string>{std::string{secure_frontend::kWebUiContentSecurityPolicy}} &&
               page->header("Content-Type") == std::optional<std::string>{"text/html; charset=utf-8"},
           "the page's own policy and type");
  }
  const auto script = raw(server, raw_http::request("GET", "/ui/app.js", authority(server), {}));
  expect(script && script->status == 200 && script->header("Content-Type") == std::optional<std::string>{"text/javascript; charset=utf-8"},
         "the bundled script");
  const auto models = raw(server, raw_http::request("GET", "/v1/models", authority(server), {raw_http::bearer(token())}));
  expect(models && models->status == 200 && models->body.find("\"id\":\"fake-model\"") != std::string::npos, "/v1/models lists the model");

  const auto metrics = [&server] {
    return raw(server, raw_http::request("GET", "/metrics", authority(server), {raw_http::bearer(token())}));
  };
  const auto unkeyed = raw(server, raw_http::request("GET", "/metrics", authority(server), {}));
  expect(unkeyed && unkeyed->status == 401, "the Monitor needs the key");
  const auto idle = metrics();
  expect(idle && idle->status == 200 && idle->body.find("\"state\":\"idle\"") != std::string::npos &&
             idle->body.find("\"requests\":[]") != std::string::npos,
         "idle, no requests yet: " + (idle ? idle->body.substr(0, 400) : std::string{"<none>"}));
  // The page sends a reasoning_effort with every chat (web/app.js); the stream goes without one.
  const auto plain = test_support::chat(server, gemma4_body(R"("reasoning_effort":"none")"));
  expect(plain.head.status == 200 && test_support::complete_text(plain.body) == "Hi world", "a chat: " + head_of(plain));
  const auto streamed = test_support::chat(server, gemma4_body(R"("stream":true)"));
  expect(streamed.head.status == 200 && test_support::stream_text(streamed) == "Hi world", "a streamed chat: " + head_of(streamed));

  const auto after = metrics();
  const std::string body = after ? after->body : std::string{};
  expect(after && after->status == 200 && body.find("\"requests_kept\":2") != std::string::npos,
         "both generations are kept: " + body.substr(0, 600));
  std::size_t stops = 0;
  for (std::size_t at = body.find("\"finish\":\"stop\""); at != std::string::npos; at = body.find("\"finish\":\"stop\"", at + 1U)) ++stops;
  expect(stops == 2U, "each one finished stop (at <turn|>): " + body.substr(0, 600));
  expect(count_of(body, "\"output_tokens\":3") == 2U,
         "each one counted the engine's three T records (<turn|> included, as the engine counts it): " + body.substr(0, 600));
  expect(body.find("\"model\":\"fake-model\"") != std::string::npos, "the served model is named");
  expect(body.find("Hello") == std::string::npos && body.find("Hi world") == std::string::npos &&
             body.find("\"content\"") == std::string::npos,
         "no prompt and no answer in the Monitor");
  const secure_frontend::JsonLimits limits{.max_bytes = 1U << 20U, .max_depth = 8U, .max_container_entries = 4096U,
                                           .max_string_bytes = 1U << 16U, .allow_fractions = true};
  expect(std::holds_alternative<secure_frontend::JsonValue>(secure_frontend::parse_json(body, limits)), "valid JSON");
  std::this_thread::sleep_for(200ms);
  const std::string log = server.stderr_text();
  expect(log.find(" web 200 - ") != std::string::npos && log.find(" metrics 200 - ") != std::string::npos,
         "the request log names the page and the Monitor routes: " + log);
}

}  // namespace

int main(int argc, char** argv) {
  secure_frontend::WinsockGuard winsock;
  return secure_frontend_test::run_cases(argc, argv, {
      {"dos01_body_over_limit_is_413_before_reading", dos01_body_over_limit_is_413_before_reading},
      {"dos01_content_length_over_request_limit_is_413_before_auth", dos01_content_length_over_request_limit_is_413_before_auth},
      {"dos01_unauthenticated_gets_401_before_body_is_read", dos01_unauthenticated_gets_401_before_body_is_read},
      {"dos01_expect_continue_after_auth_only", dos01_expect_continue_after_auth_only},
      {"dos01_slow_header_is_408_at_header_deadline", dos01_slow_header_is_408_at_header_deadline},
      {"dos01_slow_body_is_408_at_request_deadline", dos01_slow_body_is_408_at_request_deadline},
      {"dos01_incomplete_body_then_close_frees_worker", dos01_incomplete_body_then_close_frees_worker},
      {"err_unauthenticated_is_401_on_every_path_and_method", err_unauthenticated_is_401_on_every_path_and_method},
      {"security_headers_on_every_response", security_headers_on_every_response},
      {"web01_bad_host_rejected_on_every_route", web01_bad_host_rejected_on_every_route},
      {"web01_ipv6_bound_host_rules", web01_ipv6_bound_host_rules},
      {"web02_bound_authority_and_localhost_alias_succeed", web02_bound_authority_and_localhost_alias_succeed},
      {"web02_host_alias_from_config_succeeds", web02_host_alias_from_config_succeeds},
      {"web03_untrusted_origin_no_cors_header", web03_untrusted_origin_no_cors_header},
      {"web04_default_emits_no_cors_header_anywhere", web04_default_emits_no_cors_header_anywhere},
      {"web05_only_exact_configured_origin_allowed", web05_only_exact_configured_origin_allowed},
      {"web06_preflight_matrix", web06_preflight_matrix},
      {"web06_cross_origin_post_without_trust_rejected", web06_cross_origin_post_without_trust_rejected},
      {"auth01_every_route_rejects_missing_malformed_wrong_token", auth01_every_route_rejects_missing_malformed_wrong_token},
      {"cfg01_wildcard_bind_requires_host_aliases", cfg01_wildcard_bind_requires_host_aliases},
      {"dos01_eviction_over_tcp_admits_new_client", dos01_eviction_over_tcp_admits_new_client},
      {"max_tokens_above_limit_is_clamped", max_tokens_above_limit_is_clamped},
      {"max_tokens_clamped_to_context_room", max_tokens_clamped_to_context_room},
      {"prompt_alone_over_context_is_400", prompt_alone_over_context_is_400},
      {"fit_max_tokens_false_returns_400_like_the_pin", fit_max_tokens_false_returns_400_like_the_pin},
      {"idle_unload_respawns_before_the_next_request", idle_unload_respawns_before_the_next_request},
      {"queue_and_answer_room_limits_are_enforced", queue_and_answer_room_limits_are_enforced},
      {"aliases_answer_like_the_primary_model", aliases_answer_like_the_primary_model},
      {"model_name_replaces_the_file_name", model_name_replaces_the_file_name},
      {"reasoning_effort_per_request_is_served", reasoning_effort_per_request_is_served},
      {"absent_max_tokens_uses_auto", absent_max_tokens_uses_auto},
      {"sampling_passthrough_reaches_gen", sampling_passthrough_reaches_gen},
      {"dos01_context_rule_is_400_without_gen", dos01_context_rule_is_400_without_gen},
      {"chat_response_has_openai_shape", chat_response_has_openai_shape},
      {"enc_round_trip_reaches_the_prompt", enc_round_trip_reaches_the_prompt},
      {"image_rejections_name_image_url_before_any_write", image_rejections_name_image_url_before_any_write},
      {"dos02_excess_connections_get_503_retry_after", dos02_excess_connections_get_503_retry_after},
      {"dos02_queue_full_is_503", dos02_queue_full_is_503},
      {"dos02_queue_timeout_is_503", dos02_queue_timeout_is_503},
      {"dos02_queue_is_fifo", dos02_queue_is_fifo},
      {"dos02_health_answers_while_generation_places_full", dos02_health_answers_while_generation_places_full},
      {"dos02_serial_mode_one_worker", dos02_serial_mode_one_worker},
      {"dos02_queued_client_disconnect_leaves_queue", dos02_queued_client_disconnect_leaves_queue},
      {"dos02_non_stream_disconnect_cancels", dos02_non_stream_disconnect_cancels},
      {"dos02_resources_return_to_idle", dos02_resources_return_to_idle},
      {"shutdown_errors_never_report_fatal", shutdown_errors_never_report_fatal},
      {"dos01_service_healthy_after_each_attack", dos01_service_healthy_after_each_attack},
      {"ext01_upstream_control_routes_are_404_after_auth", ext01_upstream_control_routes_are_404_after_auth},
      {"ext02_http_headers_cannot_enable_extensions", ext02_http_headers_cannot_enable_extensions},
      {"log_line_format_and_rate_limit", log_line_format_and_rate_limit},
      {"engine_output_reaches_the_log_only_when_asked", engine_output_reaches_the_log_only_when_asked},
      {"network_shares_warn_at_every_start", network_shares_warn_at_every_start},
      {"undecodable_output_logs_its_own_code", undecodable_output_logs_its_own_code},
      {"web_ui_page_is_served_and_the_api_keeps_its_key", web_ui_page_is_served_and_the_api_keeps_its_key},
      {"monitor_reports_generations_without_their_text", monitor_reports_generations_without_their_text},
      {"keyless_loopback_server_answers_without_a_key", keyless_loopback_server_answers_without_a_key},
      {"err_no_token_prompt_or_path_in_any_body_or_log", err_no_token_prompt_or_path_in_any_body_or_log},
      {"utf8_non_stream_max_tokens_cut_mid_character_returns_length", utf8_non_stream_max_tokens_cut_mid_character_returns_length},
      {"generation_timeout_returns_text_with_length", generation_timeout_returns_text_with_length},
      {"stream_matches_non_stream_text", stream_matches_non_stream_text},
      {"dos02_queue_full_with_streams_is_503_before_headers", dos02_queue_full_with_streams_is_503_before_headers},
      {"ext01_extension_fields_rejected_on_stream_path", ext01_extension_fields_rejected_on_stream_path},
      {"dos02_slow_reader_never_backpressures_engine", dos02_slow_reader_never_backpressures_engine},
      {"finish_stop_length_and_generated_bytes_cap", finish_stop_length_and_generated_bytes_cap},
      {"utf8_timeout_cut_mid_character_returns_length", utf8_timeout_cut_mid_character_returns_length},
      {"dos02_stream_disconnect_sends_stop_and_frees_engine", dos02_stream_disconnect_sends_stop_and_frees_engine},
      {"dos02_slow_stream_reader_hits_write_stall", dos02_slow_stream_reader_hits_write_stall},
      {"stream_cut_has_no_terminal_chunk", stream_cut_has_no_terminal_chunk},
      {"stream_cancel_during_prefill_frees_engine", stream_cancel_during_prefill_frees_engine},
      {"log_stream_outcomes_use_fixed_codes", log_stream_outcomes_use_fixed_codes},
      {"stream_keepalive_comment_during_prefill", stream_keepalive_comment_during_prefill},
      {"err_mid_stream_is_error_event_then_done", err_mid_stream_is_error_event_then_done},
      {"crash_mid_stream_is_engine_unavailable_event", crash_mid_stream_is_engine_unavailable_event},
      {"nonfinite_is_engine_error_and_engine_stays_ready", nonfinite_is_engine_error_and_engine_stays_ready},
      {"engine_error_carries_should_retry_false", engine_error_carries_should_retry_false},
      {"dos02_shutdown_during_stream_ends_with_error_event", dos02_shutdown_during_stream_ends_with_error_event},
      {"win_refusal_is_read_not_reset", win_refusal_is_read_not_reset},
      {"win_eviction_wakes_worker_within_one_slice", win_eviction_wakes_worker_within_one_slice},
      {"win_disconnect_detected_by_peek", win_disconnect_detected_by_peek},
      {"dos01_slowloris_cannot_starve_a_real_client", dos01_slowloris_cannot_starve_a_real_client},
      {"dos01_slow_body_clients_cannot_starve_a_real_client", dos01_slow_body_clients_cannot_starve_a_real_client},
      {"net01_workers_add_no_listener", net01_workers_add_no_listener},
      {"net01_ipv6_loopback_listener_only", net01_ipv6_loopback_listener_only},
      {"gemma4_profile_chats_end_to_end", gemma4_profile_chats_end_to_end},
      {"gemma4_thinking_round_trip", gemma4_thinking_round_trip},
      {"gemma4_monitor_and_page_unchanged", gemma4_monitor_and_page_unchanged},
  }, failures);
}
