#include "secure_frontend/http_socket_server.hpp"
#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/api_router.hpp"
#include "secure_frontend/connection_handler.hpp"
#include "secure_frontend/http_policy.hpp"
#include "secure_frontend/process_defaults.hpp"
#include "secure_frontend/secret_bytes.hpp"
#include "secure_frontend/socket_io.hpp"
#include "secure_frontend/stream_socket.hpp"
#include "secure_frontend/token_verifier.hpp"
#include "support/raw_http.hpp"
#include "support/test_paths.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include "secure_frontend/win32_prelude.hpp"
#include <mmsystem.h>  // timeBeginPeriod (winmm), see main
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
int failures = 0;
void expect(bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

#ifdef _WIN32
int send_bytes(SOCKET s, const std::string& b) { return send(s, b.data(), static_cast<int>(b.size()), 0); }
int recv_bytes(SOCKET s, char* out, int n) { return recv(s, out, n, 0); }
#else
int send_bytes(int s, const std::string& b) { return static_cast<int>(send(s, b.data(), b.size(), MSG_NOSIGNAL)); }
int recv_bytes(int s, char* out, int n) { return static_cast<int>(recv(s, out, static_cast<std::size_t>(n), 0)); }
#endif

constexpr std::string_view kSocketToken = "socket-token-0123456789abcdefghijklmnopqr";
constexpr std::size_t kDefaultChatBodyLimit = 49152U;

secure_frontend::ConnectionLimits fast_limits() {
  return {.head = {4096, 16384, 64, 65536}, .header_deadline = std::chrono::milliseconds(1000),
          .request_deadline = std::chrono::milliseconds(2000), .write_stall = std::chrono::milliseconds(500),
          .response_write_deadline = std::chrono::milliseconds(5000)};
}

std::string authority_for(const std::string& h, const std::uint16_t port) {
  return (h.find(':') != std::string::npos ? "[" + h + "]" : h) + ":" + std::to_string(port);
}

// One listener on an ephemeral port plus an ApiRouter over it. `host` is the
// bind address; `client_host` is what clients connect to and name in Host (it
// differs only for a wildcard bind, which clients cannot connect to by name).
struct Fixture final {
  std::optional<secure_frontend::HttpSocketServer> server;
  std::unique_ptr<secure_frontend::ApiRouter> router;
  std::string error;
  std::string host;
  std::string client_host;
  std::uint16_t port{0};
  secure_frontend::ConnectionLimits limits = fast_limits();
  std::string big_body;      // returned by the chat handler when set (write-stall tests)
  bool stream{false};        // the chat handler returns a StreamHandler (dispatch-site test)
  bool no_response{false};   // the chat handler returns kNoResponseStatus (client gone)

  // chat_body_limit is raised only by the test-only "sink" use of the chat
  // route (parser_honors_configured_max_request_bytes); the chat handler then
  // answers {"bytes":N} for an N-byte body.
  explicit Fixture(std::string bind_host = "127.0.0.1", std::string connect_host = {},
                   const std::size_t chat_body_limit = kDefaultChatBodyLimit)
      : host(std::move(bind_host)), client_host(connect_host.empty() ? host : std::move(connect_host)) {
    const bool wildcard = host == "::" || host == "0.0.0.0";
    secure_frontend::Config listener{};
    listener.host = host;
    listener.port = 0;
    listener.has_api_key = true;
    listener.is_loopback = !wildcard;
    listener.allow_remote = wildcard;
    listener.allow_wildcard_bind = wildcard;
    server = secure_frontend::HttpSocketServer::bind(listener, 16, &error);
    if (!server) return;
    port = server->port();
    // A wildcard bind trusts only aliases (spec §4.3): name the client authority.
    const std::vector<std::string> aliases =
        wildcard ? std::vector<std::string>{authority_for(client_host, port)} : std::vector<std::string>{};
    secure_frontend::SecretBytes secret;
    (void)secret.assign(kSocketToken);
    router = std::make_unique<secure_frontend::ApiRouter>(secure_frontend::ApiRouterConfig{
        .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address(host), port, aliases, {}),
        .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
        .model_id = "fake-model", .owned_by = "local", .chat_body_limit = chat_body_limit,
        .health = [] { return secure_frontend::HealthView{true, "ready", ""}; },
        .chat = [this](std::string_view body, const secure_frontend::RequestContext&) -> secure_frontend::RouteResult {
          if (no_response) return secure_frontend::HttpResponse{.status = secure_frontend::kNoResponseStatus, .headers = {}, .body = {}};
          if (stream) {
            return secure_frontend::StreamHandler{[](secure_frontend::StreamSink& sink) {
              (void)sink.send_headers({200, {{"Content-Type", "text/event-stream; charset=utf-8"}, {"Transfer-Encoding", "chunked"}}});
              (void)sink.send_event("data: a\n\n");
              (void)sink.send_event("data: [DONE]\n\n");
              sink.finish();
            }};
          }
          return secure_frontend::HttpResponse{.status = 200, .headers = {{"Content-Type", "application/json; charset=utf-8"}},
                                               .body = big_body.empty() ? "{\"bytes\":" + std::to_string(body.size()) + "}\n" : big_body};
        }});
  }
  [[nodiscard]] std::string authority() const { return authority_for(client_host, port); }
  [[nodiscard]] std::string health_request() const {
    return raw_http::request("GET", "/health", authority(), {raw_http::bearer(kSocketToken)});
  }
  // Serves exactly one connection on a background thread.
  std::thread serve_once(bool* result = nullptr) {
    return std::thread([this, result] {
      std::string e;
      const bool ok = server->serve_one(*router, limits, &e);
      if (result != nullptr) *result = ok;
    });
  }
  // Serves one GET /health and returns the parsed response (nullopt on any failure).
  std::optional<raw_http::Response> round_trip() {
    bool served = false;
    auto worker = serve_once(&served);
    auto response = raw_http::exchange(client_host, port, health_request());
    worker.join();
    if (!served) return std::nullopt;
    return response;
  }
};

std::string chat_request_for(const Fixture& f, const std::string& body) {
  return raw_http::request("POST", "/v1/chat/completions", f.authority(),
                           {raw_http::bearer(kSocketToken), "Content-Type: application/json"}, body);
}

bool is_ok(const std::optional<raw_http::Response>& response) { return response && response->status == 200; }

void serves_a_loopback_request_without_outbound_connections() {
  Fixture f;
  expect(f.server.has_value(), "loopback listener should bind: " + f.error);
  if (!f.server) return;
  const auto response = f.round_trip();
  expect(is_ok(response) && response->body == "{\"status\":\"ok\",\"engine\":\"ready\"}\n",
         "authorized loopback request should receive HTTP 200");
}

// S1: a client that sends a request and closes before reading must not kill
// the process (SIGPIPE) and must not stop the listener from serving the next client.
void socket_server_survives_client_early_close() {
  Fixture f;
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  // A 1-byte receive window and a 32 MiB response: the response cannot be
  // absorbed by socket buffers before we close, so the server's send is
  // guaranteed to hit a closed peer (EPIPE/reset) instead of depending on timing.
  f.big_body = std::string(32U * 1024U * 1024U, 'x');
  bool served = true;
  auto worker = f.serve_once(&served);
  {
    auto client = raw_http::Connection::open(f.client_host, f.port, 1);
    expect(client && client->send(chat_request_for(f, "{}")), "request sent");
  }  // closed without reading
  worker.join();
  expect(!served, "a client that closed before the response is not a served request");
  // Still alive here means no SIGPIPE killed us. The next client must be served.
  f.big_body.clear();
  expect(is_ok(f.round_trip()), "listener must serve the next client after an early close");
}

// S3: one byte every 400 ms must not hold the listener past the header deadline.
void socket_server_rejects_slow_request_past_deadline() {
  Fixture f;   // fast_limits(): header_deadline 1 s
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  bool served = true;
  std::atomic<bool> returned{false};
  const auto start = std::chrono::steady_clock::now();
  auto returned_at = start;
  // Time the listener itself: when serve_one returns, not when the dripping
  // client next notices the closed connection (that depends on the close path).
  std::thread worker([&] {
    std::string e;
    served = f.server->serve_one(*f.router, f.limits, &e);
    returned_at = std::chrono::steady_clock::now();
    returned = true;
  });
  auto client = raw_http::Connection::open(f.client_host, f.port);
  if (!client) { expect(false, "client should connect"); worker.join(); return; }
  const std::string request = f.health_request();
  for (std::size_t i = 0; i < request.size() - 1 && !returned; ++i) {  // never send the final byte
    if (!client->send(std::string_view{request}.substr(i, 1))) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(4)) break;
  }
  const auto response = raw_http::parse_response(client->read_until_close(std::chrono::seconds(5)));
  worker.join();
  const auto elapsed = returned_at - start;
  expect(!served, "a dripping client must be rejected, not served");
  expect(elapsed >= std::chrono::milliseconds(900) && elapsed < std::chrono::milliseconds(2500),
         "listener must give up at the 1 s header deadline, not per-recv");
  expect(response && response->status == 408 &&
             response->body == secure_frontend::render_error_body(secure_frontend::ApiError::request_timeout),
         "408 request_timeout at the header deadline");
}

// D1: one body for both stalled-reader cases (the W case is registered separately).
void stalled_reader_case(Fixture& f) {
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  f.big_body = std::string(32U * 1024U * 1024U, 'x');   // far more than any socket buffer
  bool served = true;
  auto worker = f.serve_once(&served);
  auto client = raw_http::Connection::open(f.client_host, f.port, 1);
  expect(client && client->send(chat_request_for(f, "{}")), "request sent; the client then never reads");
  const auto started = std::chrono::steady_clock::now();
  worker.join();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(!served, "a 32 MiB response to a reader that never reads fails");
  expect(elapsed < std::chrono::seconds(3), "it ends at the 500 ms write stall on progress, not the 5 s response deadline");
}

// S2: a peer that never reads must not hold the worker (stall rule, spec §2.7).
void socket_server_send_times_out_on_stalled_reader() {
  Fixture f;
  stalled_reader_case(f);
}

void win_send_chunks_respect_write_stall() {
  Fixture f;
  stalled_reader_case(f);
}

// S4 (spec §9.4): a Content-Length beyond max_request_bytes is answered 413
// from the header alone (parse time, before authentication); the listener stays usable.
void socket_server_rejects_oversize_request_over_tcp() {
  Fixture f;   // fast_limits(): max_request_bytes 65536
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  bool served = false;
  auto worker = f.serve_once(&served);
  auto client = raw_http::Connection::open(f.client_host, f.port);
  const std::string body(70000U, 'b');
  const std::string request = "POST /v1/chat/completions HTTP/1.1\r\nHost: " + f.authority() +
                              "\r\nContent-Type: application/json\r\nContent-Length: 70000\r\n\r\n" + body;
  expect(client && client->send(request), "oversize request sent with its body");
  const auto response = raw_http::parse_response(client ? client->read_until_close(std::chrono::seconds(5)) : "");
  worker.join();
  expect(served, "a 413 is an answered request");
  expect(response && response->status == 413 &&
             response->body == secure_frontend::render_error_body(secure_frontend::ApiError::request_too_large),
         "oversize request gets 413 request_too_large");
  expect(is_ok(f.round_trip()), "listener must serve the next client after an oversize request");
}

// An over-long header (no blank line within max_header_bytes) is 431 (spec §4.2).
void socket_server_rejects_oversize_header_with_431() {
  Fixture f;
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  bool served = true;
  auto worker = f.serve_once(&served);
  auto client = raw_http::Connection::open(f.client_host, f.port);
  std::string head = "GET /health HTTP/1.1\r\nHost: " + f.authority() + "\r\n";
  while (head.size() < 17000U) head += "X-Fill: " + std::string(200, 'f') + "\r\n";
  expect(client && client->send(head), "send an over-long header without its blank line");
  const auto response = raw_http::parse_response(client ? client->read_until_close(std::chrono::seconds(5)) : "");
  worker.join();
  expect(response && response->status == 431 &&
             response->body == secure_frontend::render_error_body(secure_frontend::ApiError::header_too_large),
         "431 header_too_large before the blank line");
}

// S5: no client for a while must NOT make serve_one return; the listener is
// polled in 100 ms slices instead of a blocking accept.
void socket_server_accept_waits_without_timeout() {
  Fixture f;
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  std::atomic<bool> returned{false};
  bool served = false;
  std::thread worker([&] {
    std::string e;
    served = f.server->serve_one(*f.router, f.limits, &e);
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // many 100 ms accept slices
  expect(!returned.load(), "serve_one keeps waiting for a client across poll slices");
  expect(!f.server->listener_failed(), "idle is not a listener failure");
  const auto response = raw_http::exchange(f.client_host, f.port, f.health_request());
  worker.join();
  expect(served && is_ok(response), "the late client is served");
}

// SP2 C4: the orderly shutdown wakes a serve_one() waiting for a client from
// another thread; that call and every later one return "listener stopped".
void socket_server_shutdown_listener_wakes_accept() {
  Fixture f;
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  std::atomic<bool> returned{false};
  bool served = true;
  std::string worker_error;
  std::thread worker([&] { served = f.server->serve_one(*f.router, f.limits, &worker_error); returned = true; });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  f.server->shutdown_listener();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!returned.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  expect(returned.load(), "shutdown_listener() wakes the waiting serve_one()");
  worker.join();
  expect(!served && worker_error == "listener stopped", "the woken call reports the stop: " + worker_error);
  expect(!f.server->listener_failed(), "an orderly stop is not a listener failure");
  expect(f.server->native_handle() == secure_frontend::kInvalidNativeSocket, "the listener handle is closed");
  std::string error;
  expect(!f.server->serve_one(*f.router, f.limits, &error) && error == "listener stopped", "every later call fails at once: " + error);
  f.server->shutdown_listener();  // idempotent
}

// SP2 C4 (final review I1, pre-flight P34): an accepted client that sends
// nothing must not hold serve_one() until its header deadline once the
// orderly shutdown has begun; the stop flag is the worker's shutting_down signal.
void socket_server_shutdown_listener_wakes_idle_client() {
  Fixture f;
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  f.limits.header_deadline = std::chrono::milliseconds(30000);   // far beyond the bound asserted below
  f.limits.request_deadline = std::chrono::milliseconds(30000);
  std::atomic<bool> returned{false};
  bool served = true;
  std::string worker_error;
  std::thread worker([&] { served = f.server->serve_one(*f.router, f.limits, &worker_error); returned = true; });
  auto idle = raw_http::Connection::open(f.client_host, f.port);  // connected, sends nothing
  expect(idle.has_value(), "the idle client connects");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));  // serve_one() is now waiting on it
  expect(!returned.load(), "serve_one() is still waiting on the idle client");
  const auto started = std::chrono::steady_clock::now();
  f.server->shutdown_listener();
  const auto deadline = started + std::chrono::seconds(3);
  while (!returned.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
  expect(returned.load() && waited <= std::chrono::milliseconds(500),
         "shutdown_listener() ends a serve_one() waiting on an idle client within 500 ms, took " + std::to_string(waited.count()) + " ms");
  if (idle) idle->close();  // also ends serve_one() if the fix is missing (recv sees EOF)
  worker.join();
  expect(!served && worker_error == "listener stopped", "the woken call reports the stop: " + worker_error);
}

// S6 (pre-flight T8b(c)): an IPv6 wildcard listener must be v6-only — only a
// wildcard '::' socket could take IPv4-mapped traffic, so it proves V6ONLY —
// and serves a bracketed Host.
void socket_server_ipv6_bind_is_v6only() {
  if (!test_support::ipv6_loopback_available()) test_support::skip("no IPv6 loopback on this host");
  Fixture f{"::", "::1"};
  expect(f.server.has_value(), "binds :: : " + f.error);
  if (!f.server) return;
  expect(!raw_http::Connection::open("127.0.0.1", f.port).has_value(), "an IPv6 listener serves IPv6 only");
  auto worker = f.serve_once();
  const auto response = raw_http::exchange("::1", f.port, f.health_request());
  worker.join();
  expect(is_ok(response), "[::1]:port is served");
}

// S7: Content-Length matches the body and the whole body arrives.
void socket_server_response_is_complete() {
  Fixture f;
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  auto worker = f.serve_once();
  auto client = raw_http::Connection::open(f.client_host, f.port);
  expect(client && client->send(f.health_request()), "health request sent");
  const std::string raw = client ? client->read_until_close(std::chrono::seconds(5)) : "";
  worker.join();
  const auto header_end = raw.find("\r\n\r\n");
  expect(header_end != std::string::npos, "response must have a header block");
  if (header_end == std::string::npos) return;
  const auto response = raw_http::parse_response(raw);
  const auto declared = response ? response->header("Content-Length") : std::nullopt;
  expect(declared.has_value(), "Content-Length must be present");
  if (!declared) return;
  expect(raw.size() - header_end - 4U == std::stoul(*declared), "body length must equal Content-Length");
}

// A peer that connects and resets before the server accepts must never be
// treated as a listener failure; the next client must still be served.
void socket_server_survives_reset_before_accept() {
  Fixture f;
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  // The kernel completes the handshake into the backlog, so no worker is
  // needed: connect, force an RST on close, and only then call serve_one.
  auto client = raw_http::Connection::open(f.client_host, f.port);
  if (!client) { expect(false, "client should connect"); return; }
  linger hard_reset{};
  hard_reset.l_onoff = 1;
  hard_reset.l_linger = 0;
  expect(setsockopt(client->native(), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&hard_reset), sizeof(hard_reset)) == 0,
         "SO_LINGER {1,0} should be accepted");
  client->close();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let the RST land before accept
  std::string e;
  const bool served = f.server->serve_one(*f.router, f.limits, &e);
  expect(!served, "a reset client is a client error, not a served request");
  expect(!f.server->listener_failed(), "a peer reset must not mark the listener failed: " + e);
  expect(is_ok(f.round_trip()), "listener must serve the next client after a peer reset");
}

// Spec §2.6, §9.4: a GET whose client half-closed still gets its answer.
void socket_server_responds_after_client_half_close() {
  Fixture f;
  bool served = false;
  auto worker = f.serve_once(&served);
  auto client = raw_http::Connection::open(f.client_host, f.port);
  expect(client && client->send(f.health_request()), "GET sent");
  expect(client && client->shutdown_send(), "client half-closes after its GET");
  const auto response = raw_http::parse_response(client ? client->read_until_close(std::chrono::seconds(5)) : "");
  worker.join();
  expect(served && response && response->status == 200 && response->body == "{\"status\":\"ok\",\"engine\":\"ready\"}\n",
         "a GET whose client half-closed still gets its answer (EOF is not a stray byte)");
}

// C4 / defect (d): the parser honors the configured max_request_bytes. The
// chat route acts as the old /sink with its body limit raised (pre-flight
// T8b(b)), so only ConnectionLimits.head.max_request_bytes differs.
std::optional<raw_http::Response> post_sink(Fixture& f, const std::size_t body_bytes) {
  auto worker = f.serve_once();
  const std::string body(body_bytes, 'b');
  const auto response = raw_http::exchange(f.client_host, f.port, chat_request_for(f, body));
  worker.join();
  return response;
}

void parser_honors_configured_max_request_bytes() {
  constexpr std::size_t kRaised = 256U * 1024U;
  Fixture big("127.0.0.1", {}, kRaised);
  if (!big.server) { expect(false, "bind failed: " + big.error); return; }
  big.limits.head.max_request_bytes = kRaised;
  const auto accepted = post_sink(big, 128U * 1024U);
  expect(accepted && accepted->status == 200 && accepted->body == "{\"bytes\":131072}\n",
         "a 128 KiB body must pass a 256 KiB limit");
  Fixture standard("127.0.0.1", {}, kRaised);   // fast_limits(): 64 KiB
  if (!standard.server) { expect(false, "bind failed: " + standard.error); return; }
  const auto rejected = post_sink(standard, 128U * 1024U);
  // A Content-Length beyond the bound is answered from the header alone,
  // without reading the body: 413 request_too_large (spec §4.2, §9.4).
  expect(rejected && rejected->status == 413, "the 64 KiB limit still rejects it");
}

// Review Focus 2: a header delivered one byte per segment parses like one segment.
void head_terminator_split_across_reads() {
  Fixture f;
  bool served = false;
  auto worker = f.serve_once(&served);
  auto client = raw_http::Connection::open(f.client_host, f.port);
  const std::string request = "GET /health HTTP/1.1\r\nHost: " + f.authority() + "\r\n" + raw_http::bearer(kSocketToken) + "\r\n\r\n";
  bool sent = client.has_value();
  for (std::size_t i = 0; sent && i < request.size(); ++i) {   // one byte per segment, \r\n | \r\n split too
    sent = client->send(std::string_view{request}.substr(i, 1U));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const auto response = raw_http::parse_response(client ? client->read_until_close(std::chrono::seconds(5)) : "");
  worker.join();
  expect(sent && served && response && response->status == 200, "a header delivered byte by byte parses exactly like one segment");
}

// Review Focus 3: no interim 100 once a body byte has arrived.
void expect_continue_skipped_when_body_already_sent() {
  Fixture f;
  auto worker = f.serve_once();
  const std::string body = R"({"x":1})";
  const std::string request = "POST /v1/chat/completions HTTP/1.1\r\nHost: " + f.authority() + "\r\n" + raw_http::bearer(kSocketToken) +
                              "\r\nContent-Type: application/json\r\nExpect: 100-continue\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\n\r\n" + body;
  auto client = raw_http::Connection::open(f.client_host, f.port);
  expect(client && client->send(request), "header and body in one write");
  const std::string raw = client ? client->read_until_close(std::chrono::seconds(5)) : "";
  worker.join();
  expect(raw.find("100 Continue") == std::string::npos, "no interim 100 after the body has arrived");
  const auto response = raw_http::parse_response(raw);
  expect(response && response->status == 200 && !response->saw_continue, "exactly one final response");
}

void expect_continue_sent_after_authorization() {
  Fixture f;
  auto worker = f.serve_once();
  const std::string body = R"({"x":1})";
  auto client = raw_http::Connection::open(f.client_host, f.port);
  const std::string head = "POST /v1/chat/completions HTTP/1.1\r\nHost: " + f.authority() + "\r\n" + raw_http::bearer(kSocketToken) +
                           "\r\nContent-Type: application/json\r\nExpect: 100-continue\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\n\r\n";
  expect(client && client->send(head), "header only");
  const std::string interim = client ? client->read_for(std::chrono::milliseconds(300)) : "";
  expect(interim == "HTTP/1.1 100 Continue\r\n\r\n", "100 Continue once the header passed every check");
  expect(client && client->send(body), "then the body");
  const auto response = raw_http::parse_response(client ? client->read_until_close(std::chrono::seconds(5)) : "");
  worker.join();
  expect(response && response->status == 200 && response->body == "{\"bytes\":7}\n", "final response after the body");
}

// Spec §3.11 dispatch site: a StreamHandler is written as a chunked stream.
void stream_handler_dispatch_writes_head_chunks_and_terminal_chunk() {
  Fixture f;
  f.stream = true;
  bool served = false;
  auto worker = f.serve_once(&served);
  auto client = raw_http::Connection::open(f.client_host, f.port);
  expect(client && client->send(chat_request_for(f, "{}")), "request sent");
  const std::string raw = client ? client->read_until_close(std::chrono::seconds(5)) : "";
  worker.join();
  expect(served && raw.rfind("HTTP/1.1 200 OK\r\n", 0) == 0, "the stream head is written by deliver_route_result");
  const auto head_end = raw.find("\r\n\r\n");
  if (head_end == std::string::npos) { expect(false, "stream head complete"); return; }
  const std::string head = raw.substr(0, head_end);
  expect(head.find("Content-Length") == std::string::npos && head.find("Transfer-Encoding: chunked") != std::string::npos &&
             head.find("X-Content-Type-Options: nosniff") != std::string::npos,
         "chunked head without Content-Length, security headers kept");
  expect(raw.substr(head_end + 4U) == "9\r\ndata: a\n\n\r\ne\r\ndata: [DONE]\n\n\r\n0\r\n\r\n",
         "one chunk per event, then the terminal chunk");
}

// Task 6a carry: a RouteResult with kNoResponseStatus is never serialized; the
// connection is closed with no bytes written.
void no_response_status_closes_without_writing() {
  Fixture f;
  f.no_response = true;
  bool served = true;
  auto worker = f.serve_once(&served);
  auto client = raw_http::Connection::open(f.client_host, f.port);
  expect(client && client->send(chat_request_for(f, "{}")), "request sent");
  bool closed = false;
  const std::string raw = client ? client->read_until_close(std::chrono::seconds(5), &closed) : "x";
  worker.join();
  expect(closed && raw.empty(), "kNoResponseStatus writes nothing and closes");
  expect(!served, "a request answered with nothing is not served");
}

#if defined(_WIN32)
bool inheritable(const secure_frontend::NativeSocket s) {
  DWORD flags = 0;
  return GetHandleInformation(reinterpret_cast<HANDLE>(s), &flags) == 0 || (flags & HANDLE_FLAG_INHERIT) != 0;
}
#else
bool inheritable(const secure_frontend::NativeSocket s) {
  const int flags = fcntl(s, F_GETFD);
  return flags < 0 || (flags & FD_CLOEXEC) == 0;
}
#endif

// Pre-flight V12 (SP2 K4): the listener and every accepted socket are
// close-on-exec / non-inheritable, so an engine child can never hold them.
void listener_and_accepted_sockets_are_not_inherited() {
  Fixture f;
  if (!f.server) { expect(false, "bind failed: " + f.error); return; }
  expect(!inheritable(f.server->native_handle()), "the listener is not inheritable");
  auto client = raw_http::Connection::open(f.client_host, f.port);
  expect(client.has_value(), "client connects");
  secure_frontend::HttpSocketServer::Accepted accepted;
  auto status = secure_frontend::HttpSocketServer::AcceptStatus::timeout;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (status != secure_frontend::HttpSocketServer::AcceptStatus::accepted && std::chrono::steady_clock::now() < deadline) {
    status = f.server->accept_next(std::chrono::milliseconds(100), &accepted);
  }
  expect(status == secure_frontend::HttpSocketServer::AcceptStatus::accepted && accepted.socket.valid(), "a client is accepted");
  if (!accepted.socket.valid()) return;
  expect(!inheritable(accepted.socket.get()), "the accepted socket is not inheritable");
  expect(accepted.peer_scope == secure_frontend::AddressScope::loopback && accepted.peer_key[0] == 4U &&
             accepted.peer_key[1] == 127U && accepted.peer_key[4] == 1U,
         "the peer is classified as IPv4 loopback 127.0.0.1");
  expect(accepted.accepted_at <= std::chrono::steady_clock::now(), "accepted_at is stamped");
}

// SP3 Task 8a: socket_io.

void send_retries_on_eintr() {
  static int calls = 0;
  calls = 0;
  const auto flaky = [](secure_frontend::NativeSocket, const char*, std::size_t size) noexcept -> long {
    if (++calls == 1) { secure_frontend::detail::set_last_socket_error(secure_frontend::detail::kInterruptedCode); return -1; }
    return static_cast<long>(size);
  };
  const auto result = secure_frontend::detail::send_some_with(flaky, secure_frontend::kInvalidNativeSocket, "hello");
  expect(result.status == secure_frontend::IoStatus::done && result.bytes == 5U && calls == 2, "an interrupted send is retried, not failed");
}

void accept_backs_off_on_transient_failure() {
#if defined(_WIN32)
  const int transient[] = {WSAECONNRESET, WSAEMFILE, WSAENOBUFS, WSAEINTR};
  const int fatal[] = {WSAENOTSOCK, WSAEINVAL, WSANOTINITIALISED};
#else
  const int transient[] = {ECONNABORTED, EMFILE, ENFILE, ENOBUFS, ENOMEM, EINTR, EPROTO, EPERM};
  const int fatal[] = {EBADF, ENOTSOCK, EINVAL};
#endif
  for (const int code : transient) {
    expect(secure_frontend::detail::accept_backoff_for(code) == std::optional<std::chrono::milliseconds>{std::chrono::milliseconds(50)},
           "50 ms backoff for transient accept error " + std::to_string(code));
  }
  for (const int code : fatal) {
    expect(!secure_frontend::detail::accept_backoff_for(code).has_value(), "fatal accept error " + std::to_string(code));
  }
}

// SP2 carry-in: every send is sliced to kMaxSendChunk on both platforms.
void send_some_slices_to_max_send_chunk() {
  static std::size_t largest = 0;
  largest = 0;
  const auto record = [](secure_frontend::NativeSocket, const char*, std::size_t size) noexcept -> long {
    largest = std::max(largest, size);
    return static_cast<long>(size);
  };
  const std::string payload(3U * secure_frontend::api_constants::kMaxSendChunk + 7U, 'p');
  const auto result = secure_frontend::detail::send_some_with(record, secure_frontend::kInvalidNativeSocket, payload);
  expect(result.status == secure_frontend::IoStatus::done && result.bytes == secure_frontend::api_constants::kMaxSendChunk &&
             largest == secure_frontend::api_constants::kMaxSendChunk,
         "one send call carries at most kMaxSendChunk bytes");
  const auto empty = secure_frontend::detail::send_some_with(record, secure_frontend::kInvalidNativeSocket, "");
  expect(empty.status == secure_frontend::IoStatus::done && empty.bytes == 0U, "an empty send is a no-op");
}

// A connected loopback pair on an ephemeral port: `server` is the accepted end.
struct LoopbackPair final {
  secure_frontend::Socket server;
  secure_frontend::Socket client;
};

std::optional<LoopbackPair> make_loopback_pair(const int client_receive_buffer = 0, const int server_send_buffer = 0) {
  secure_frontend::Socket listener{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
  if (!listener.valid()) return std::nullopt;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = 0;
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) return std::nullopt;
  if (listen(listener.get(), 1) != 0) return std::nullopt;
  socklen_t length = sizeof(address);
  if (getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) return std::nullopt;
  LoopbackPair pair;
  pair.client = secure_frontend::Socket{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
  if (!pair.client.valid()) return std::nullopt;
  if (client_receive_buffer > 0) {
    (void)setsockopt(pair.client.get(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&client_receive_buffer),
                     sizeof(client_receive_buffer));
  }
  if (connect(pair.client.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) return std::nullopt;
  pair.server = secure_frontend::Socket{accept(listener.get(), nullptr, nullptr)};
  if (!pair.server.valid() || !secure_frontend::set_non_blocking(pair.server.get())) return std::nullopt;
  if (server_send_buffer > 0) {
    (void)setsockopt(pair.server.get(), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&server_send_buffer),
                     sizeof(server_send_buffer));
  }
  return pair;
}

// SP2 carry-in (Winsock accepted one 32 MiB blocking send whole): a reader that
// never reads cannot hold send_all_bounded past the stall bound, on either OS.
void send_all_bounded_stops_at_stall_on_a_stalled_reader() {
  auto pair = make_loopback_pair(1);
  if (!pair) { expect(false, "loopback pair"); return; }
  const std::string payload(32U * 1024U * 1024U, 'x');
  const auto stall = std::chrono::milliseconds(300);
  const auto start = std::chrono::steady_clock::now();
  const auto outcome = secure_frontend::send_all_bounded(pair->server.get(), payload, stall, start + std::chrono::seconds(30));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  expect(outcome == secure_frontend::WriteOutcome::stalled, "a stalled reader yields WriteOutcome::stalled");
  expect(elapsed >= stall && elapsed < std::chrono::seconds(3), "the stall bound is honoured, not the deadline");
}

// A reader that keeps making progress is never `stalled`; the whole-write
// deadline still bounds it, and a reader that drains everything completes.
void send_all_bounded_meets_deadline_and_completes() {
  {
    auto pair = make_loopback_pair(1);
    if (!pair) { expect(false, "loopback pair"); return; }
    std::atomic<bool> stop{false};
    std::thread slow_reader([&] {
      char chunk[512];
      while (!stop.load()) {
        (void)recv_bytes(pair->client.get(), chunk, sizeof(chunk));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
    const std::string payload(32U * 1024U * 1024U, 'd');
    const auto start = std::chrono::steady_clock::now();
    const auto outcome = secure_frontend::send_all_bounded(pair->server.get(), payload, std::chrono::seconds(5),
                                                           start + std::chrono::milliseconds(400));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    stop.store(true);
    pair->server.reset();  // unblocks the reader's recv with EOF
    slow_reader.join();
    expect(outcome == secure_frontend::WriteOutcome::deadline, "a slow but progressing reader hits the deadline");
    expect(elapsed >= std::chrono::milliseconds(400) && elapsed < std::chrono::seconds(3), "deadline honoured");
  }
  {
    auto pair = make_loopback_pair();
    if (!pair) { expect(false, "loopback pair"); return; }
    const std::string payload(1024U * 1024U + 3U, 'c');
    std::size_t received = 0;
    std::thread reader([&] {
      char chunk[16384];
      for (;;) { const int n = recv_bytes(pair->client.get(), chunk, sizeof(chunk)); if (n <= 0) break; received += static_cast<std::size_t>(n); }
    });
    const auto outcome = secure_frontend::send_all_bounded(pair->server.get(), payload, std::chrono::seconds(5),
                                                           std::chrono::steady_clock::now() + std::chrono::seconds(20));
    secure_frontend::half_close_and_drain(pair->server.get());
    reader.join();
    expect(outcome == secure_frontend::WriteOutcome::complete && received == payload.size(), "a draining reader gets every byte");
  }
}

// Spec §2.7 / E4: the stall clock restarts on every bit of progress. A reader
// that drains slowly but steadily for well past the stall bound completes; a
// stall measured from the call (or per call return) would report `stalled`.
void send_all_bounded_stall_restarts_on_progress() {
#if defined(_WIN32)
  // Winsock takes a whole kMaxSendChunk (64 KiB) slice at once, so progress comes per
  // slice, and a 1-byte receive window throttles loopback TCP to ~120 KB/s: ~540 ms a
  // slice, past the 200 ms bound (session log issue 29, measured). A 4 KiB window still
  // keeps the 1 MiB payload waiting on the reader (~1.3 s in all, slices ~90 ms apart).
  auto pair = make_loopback_pair(4096, 4096);
#else
  auto pair = make_loopback_pair(1, 4096);
#endif
  if (!pair) { expect(false, "loopback pair"); return; }
  const std::string payload(1024U * 1024U, 's');
  std::size_t received = 0;
  std::thread reader([&] {
    char chunk[2048];
    for (;;) {
      const int n = recv_bytes(pair->client.get(), chunk, sizeof(chunk));
      if (n <= 0) break;
      received += static_cast<std::size_t>(n);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });
  const auto stall = std::chrono::milliseconds(200);
  const auto start = std::chrono::steady_clock::now();
  const auto outcome = secure_frontend::send_all_bounded(pair->server.get(), payload, stall, start + std::chrono::seconds(10));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  secure_frontend::half_close_and_drain(pair->server.get());
  reader.join();
  expect(outcome == secure_frontend::WriteOutcome::complete, "steady progress never counts as a stall");
  expect(elapsed > stall, "the write outlasted the stall bound, so the bound restarted on progress");
  expect(received == payload.size(), "the reader got every byte");
}

// Task 12 carry-in (spec §2.8): once the stop flag is set, an in-flight write
// ends within api_constants::kShutdownDrainBudget of that observation, not at
// its whole-write deadline — the pool's join stays inside SP2's shutdown bound
// even with a stalled reader.
void send_all_bounded_stop_flag_cuts_an_inflight_write_within_the_drain_budget() {
  auto pair = make_loopback_pair(1);
  if (!pair) { expect(false, "loopback pair"); return; }
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    char chunk[512];
    while (!stop.load()) {
      if (recv_bytes(pair->client.get(), chunk, sizeof(chunk)) <= 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  std::thread stopper([&] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); stop.store(true); });
  // Enough payload that the flag always lands mid-write: the kernel send
  // buffer autotunes to megabytes, so 32 MiB is never drained in 300 ms.
  const std::string payload(32U * 1024U * 1024U, 'x');
  const auto start = std::chrono::steady_clock::now();
  const auto outcome = secure_frontend::send_all_bounded(pair->server.get(), payload, std::chrono::seconds(10),
                                                         start + std::chrono::seconds(30), &stop);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  stop.store(true);      // releases the reader even if the flag had not been set yet
  pair->server.reset();  // unblocks the reader with EOF
  stopper.join();
  reader.join();
  expect(outcome == secure_frontend::WriteOutcome::deadline, "the stop flag cuts the write at the drain budget");
  expect(elapsed >= std::chrono::milliseconds(300), "the write outlived the flag's setting, so it was in flight");
  expect(elapsed < std::chrono::milliseconds(2900),
         "stop at 300 ms + kShutdownDrainBudget (2 s) + slice slack, never the 30 s deadline");
}

// poll_sockets, recv_some, probe_peer and bytes_pending on a live pair.
void socket_io_poll_probe_and_recv() {
  auto pair = make_loopback_pair();
  if (!pair) { expect(false, "loopback pair"); return; }
  const secure_frontend::NativeSocket server = pair->server.get();
  secure_frontend::PollEntry entry{server, true, false};
  expect(secure_frontend::poll_sockets(std::span<secure_frontend::PollEntry>{&entry, 1U}, std::chrono::milliseconds(20)) == 0 &&
             !entry.readable,
         "an idle socket times out unreadable");
  expect(secure_frontend::probe_peer(server) == secure_frontend::PeerState::alive && !secure_frontend::bytes_pending(server),
         "an idle peer is alive with nothing pending");
  char buffer[16];
  expect(secure_frontend::recv_some(server, buffer, sizeof(buffer)).status == secure_frontend::IoStatus::would_block,
         "an empty non-blocking recv would block");
  (void)send_bytes(pair->client.get(), "ab");
  entry = secure_frontend::PollEntry{server, true, false};
  expect(secure_frontend::poll_sockets(std::span<secure_frontend::PollEntry>{&entry, 1U}, std::chrono::seconds(2)) == 1 &&
             entry.readable,
         "a pending byte makes the socket readable");
  expect(secure_frontend::bytes_pending(server) && secure_frontend::probe_peer(server) == secure_frontend::PeerState::gone,
         "any byte after the body means gone (spec 2.6)");
  const auto got = secure_frontend::recv_some(server, buffer, sizeof(buffer));
  expect(got.status == secure_frontend::IoStatus::done && got.bytes == 2U, "recv_some reads the bytes");
  pair->client.reset();
  entry = secure_frontend::PollEntry{server, true, false};
  (void)secure_frontend::poll_sockets(std::span<secure_frontend::PollEntry>{&entry, 1U}, std::chrono::seconds(2));
  expect(secure_frontend::probe_peer(server) == secure_frontend::PeerState::gone && !secure_frontend::bytes_pending(server),
         "EOF means gone with nothing pending");
  expect(secure_frontend::recv_some(server, buffer, sizeof(buffer)).status == secure_frontend::IoStatus::closed, "EOF is closed");
  std::vector<secure_frontend::PollEntry> too_many(secure_frontend::api_constants::kMaxPollEntries + 1U,
                                                   secure_frontend::PollEntry{server, true, false});
  expect(secure_frontend::poll_sockets(too_many, std::chrono::milliseconds(0)) == -1, "more than kMaxPollEntries is refused");
}

// Helper for Task 8b (pre-flight V12): a socket can be marked non-inheritable.
void set_no_inherit_marks_the_socket() {
  secure_frontend::Socket s{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
  if (!s.valid()) { expect(false, "socket"); return; }
  expect(secure_frontend::set_no_inherit(s.get()), "set_no_inherit succeeds");
#if defined(_WIN32)
  DWORD flags = 0;
  expect(GetHandleInformation(reinterpret_cast<HANDLE>(s.get()), &flags) != 0 && (flags & HANDLE_FLAG_INHERIT) == 0,
         "the handle is not inheritable");
#else
  expect((fcntl(s.get(), F_GETFD) & FD_CLOEXEC) != 0, "the descriptor is close-on-exec");
#endif
}

// SP3 M2: AcceptedSocketStream over a real loopback connection (Winsock semantics are authoritative in MSYS2).
void stream_socket_peek_and_nonblocking_send() {
  using Native = secure_frontend::NativeSocket;
  const auto close_native = [](Native s) { secure_frontend::close_native_socket(s); };
  // listener on 127.0.0.1:0, one client, one accepted socket.
  Native listener = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  expect(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 4) == 0, "listen");
  socklen_t length = sizeof(address);
  (void)getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
  const auto connected_pair = [&] {
    Native client = socket(AF_INET, SOCK_STREAM, 0);
    int small_buffer = 4096;
    (void)setsockopt(client, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&small_buffer), sizeof(small_buffer));
    expect(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect");
    Native server = accept(listener, nullptr, nullptr);
    return std::pair<Native, Native>{client, server};
  };
  {
    auto [client, server] = connected_pair();
    secure_frontend::AcceptedSocketStream stream(server);
    expect(!stream.peer_gone(), "a quiet client is alive");
    expect(send(client, "x", 1, 0) == 1, "client sends a stray byte");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect(stream.peer_gone(), "a byte after the request is treated as gone (spec §2.6)");
    close_native(client);
    close_native(server);
  }
  {
    auto [client, server] = connected_pair();
    secure_frontend::AcceptedSocketStream stream(server);
    close_native(client);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect(stream.peer_gone(), "peer EOF or reset is gone");
    close_native(server);
  }
  {
    auto [client, server] = connected_pair();
    secure_frontend::AcceptedSocketStream stream(server);
    const std::string block(64U * 1024U, 'z');
    bool blocked = false;
    for (int round = 0; round < 4096 && !blocked; ++round) {
      const auto step = stream.send_some(block);
      expect(step.status != secure_frontend::SendStatus::failed, "send never fails on a live, non-reading peer");
      expect(step.sent <= block.size(), "at most api_constants::kMaxSendChunk per call");
      blocked = step.status == secure_frontend::SendStatus::would_block;
    }
    expect(blocked, "a non-reading peer makes a non-blocking send report would_block, never block");
    expect(!stream.wait_writable(std::chrono::milliseconds(50)), "not writable while the peer reads nothing");
    close_native(client);
    close_native(server);
  }
  close_native(listener);
}
}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  // Session log issue 29: Windows sleeps in 15.6 ms timer ticks by default, so the
  // byte-paced cases' 2 ms steps took ~16 ms each (head_terminator_split_across_reads
  // overran its 1 s header deadline; the paced reader in
  // send_all_bounded_stall_restarts_on_progress overran the 10 s write deadline).
  // 1 ms resolution for this process only (per-process since Windows 10 2004).
  (void)timeBeginPeriod(1);
#endif
  secure_frontend::install_process_defaults();
  const secure_frontend::WinsockGuard winsock;
  if (!winsock.ok()) { std::cerr << "WSAStartup failed\n"; return 1; }
  return secure_frontend_test::run_cases(argc, argv, {
      {"serves_a_loopback_request_without_outbound_connections", serves_a_loopback_request_without_outbound_connections},
      {"socket_server_survives_client_early_close", socket_server_survives_client_early_close},
      {"socket_server_rejects_slow_request_past_deadline", socket_server_rejects_slow_request_past_deadline},
      {"socket_server_send_times_out_on_stalled_reader", socket_server_send_times_out_on_stalled_reader},
      {"socket_server_rejects_oversize_request_over_tcp", socket_server_rejects_oversize_request_over_tcp},
      {"socket_server_accept_waits_without_timeout", socket_server_accept_waits_without_timeout},
      {"socket_server_ipv6_bind_is_v6only", socket_server_ipv6_bind_is_v6only},
      {"socket_server_response_is_complete", socket_server_response_is_complete},
      {"socket_server_survives_reset_before_accept", socket_server_survives_reset_before_accept},
      {"socket_server_responds_after_client_half_close", socket_server_responds_after_client_half_close},
      {"parser_honors_configured_max_request_bytes", parser_honors_configured_max_request_bytes},
      {"socket_server_shutdown_listener_wakes_accept", socket_server_shutdown_listener_wakes_accept},
      {"socket_server_shutdown_listener_wakes_idle_client", socket_server_shutdown_listener_wakes_idle_client},
      {"socket_server_rejects_oversize_header_with_431", socket_server_rejects_oversize_header_with_431},
      {"head_terminator_split_across_reads", head_terminator_split_across_reads},
      {"expect_continue_skipped_when_body_already_sent", expect_continue_skipped_when_body_already_sent},
      {"expect_continue_sent_after_authorization", expect_continue_sent_after_authorization},
      {"stream_handler_dispatch_writes_head_chunks_and_terminal_chunk", stream_handler_dispatch_writes_head_chunks_and_terminal_chunk},
      {"no_response_status_closes_without_writing", no_response_status_closes_without_writing},
      {"listener_and_accepted_sockets_are_not_inherited", listener_and_accepted_sockets_are_not_inherited},
      {"win_send_chunks_respect_write_stall", win_send_chunks_respect_write_stall},
      {"send_retries_on_eintr", send_retries_on_eintr},
      {"accept_backs_off_on_transient_failure", accept_backs_off_on_transient_failure},
      {"send_some_slices_to_max_send_chunk", send_some_slices_to_max_send_chunk},
      {"send_all_bounded_stops_at_stall_on_a_stalled_reader", send_all_bounded_stops_at_stall_on_a_stalled_reader},
      {"send_all_bounded_meets_deadline_and_completes", send_all_bounded_meets_deadline_and_completes},
      {"send_all_bounded_stall_restarts_on_progress", send_all_bounded_stall_restarts_on_progress},
      {"send_all_bounded_stop_flag_cuts_an_inflight_write_within_the_drain_budget",
       send_all_bounded_stop_flag_cuts_an_inflight_write_within_the_drain_budget},
      {"socket_io_poll_probe_and_recv", socket_io_poll_probe_and_recv},
      {"set_no_inherit_marks_the_socket", set_no_inherit_marks_the_socket},
      {"stream_socket_peek_and_nonblocking_send", stream_socket_peek_and_nonblocking_send},
  }, failures);
}
