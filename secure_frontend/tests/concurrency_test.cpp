#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/connection_pool.hpp"
#include "secure_frontend/generation_gate.hpp"
#include "secure_frontend/http_socket_server.hpp"
#include "secure_frontend/request_log.hpp"
#include "secure_frontend/socket_io.hpp"
#include "support/fake_supervised_process.hpp"
#include "support/raw_http.hpp"
#include "support/test_support.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <algorithm>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using secure_frontend::AdmissionState;
int failures = 0;
void expect(bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
secure_frontend::PeerKey peer(unsigned char last) { secure_frontend::PeerKey k{}; k[0] = 4; k[1] = 192; k[2] = 168; k[3] = 1; k[4] = last; return k; }
const secure_frontend::PeerKey kLoop = [] { secure_frontend::PeerKey k{}; k[0] = 4; k[1] = 127; k[4] = 1; return k; }();

void dos01_oldest_header_phase_connection_is_evicted() {
  AdmissionState state(3, 3);
  const auto t0 = AdmissionState::Clock::time_point{} + 1s;
  const auto a = state.admit(secure_frontend::AddressScope::loopback, kLoop, t0);
  const auto b = state.admit(secure_frontend::AddressScope::loopback, kLoop, t0 + 10ms);
  const auto c = state.admit(secure_frontend::AddressScope::loopback, kLoop, t0 + 20ms);
  expect(a.kind == AdmissionState::Kind::assign && b.kind == AdmissionState::Kind::assign && c.kind == AdmissionState::Kind::assign,
         "three idle workers take three connections");
  expect(state.header_complete(a.worker, a.connection), "a authenticates and leaves P1");
  const auto d = state.admit(secure_frontend::AddressScope::loopback, kLoop, t0 + 30ms);
  expect(d.kind == AdmissionState::Kind::evict && d.worker == b.worker, "the longest-waiting P1 connection (b) is evicted, never a");
  expect(!state.header_complete(b.worker, b.connection), "the evicted connection learns it was evicted");
  expect(state.phase(b.worker) == secure_frontend::WorkerPhase::header, "the worker now serves the new connection in P1");
  expect(state.header_complete(c.worker, c.connection) && state.header_complete(d.worker, d.connection), "c and d authenticate");
  const auto e = state.admit(secure_frontend::AddressScope::loopback, kLoop, t0 + 40ms);
  expect(e.kind == AdmissionState::Kind::refuse, "no idle and no P1 worker: refuse");
  state.finished(b.worker, b.connection);   // stale id: ignored
  expect(state.phase(b.worker) == secure_frontend::WorkerPhase::busy, "a stale finish does not free the worker");
  state.finished(d.worker, d.connection);
  expect(state.phase(d.worker) == secure_frontend::WorkerPhase::idle, "the real finish frees it");
}

void dos01_remote_address_cap() {
  AdmissionState state(8, 2);
  const auto t0 = AdmissionState::Clock::time_point{} + 1s;
  const auto first = state.admit(secure_frontend::AddressScope::remote, peer(5), t0);
  const auto second = state.admit(secure_frontend::AddressScope::remote, peer(5), t0);
  const auto third = state.admit(secure_frontend::AddressScope::remote, peer(5), t0);
  expect(first.kind == AdmissionState::Kind::assign && second.kind == AdmissionState::Kind::assign, "two connections from one remote address");
  expect(third.kind == AdmissionState::Kind::refuse, "the third is refused although workers are idle");
  expect(state.admit(secure_frontend::AddressScope::remote, peer(6), t0).kind == AdmissionState::Kind::assign, "another address is admitted");
  for (int i = 0; i < 4; ++i) {
    expect(state.admit(secure_frontend::AddressScope::loopback, kLoop, t0).kind == AdmissionState::Kind::assign,
           "loopback is exempt from the per-address cap");
  }
  state.finished(first.worker, first.connection);
  expect(state.remote_count(peer(5)) == 1U, "finishing releases the address count");
  expect(state.admit(secure_frontend::AddressScope::remote, peer(5), t0).kind == AdmissionState::Kind::assign, "admitted again under the cap");

  // Evicting a remote peer's P1 connection releases that address's count.
  AdmissionState one(1, 2);
  const auto held = one.admit(secure_frontend::AddressScope::remote, peer(7), t0);
  expect(held.kind == AdmissionState::Kind::assign && one.remote_count(peer(7)) == 1U, "the remote peer holds the only worker in P1");
  const auto evictor = one.admit(secure_frontend::AddressScope::loopback, kLoop, t0 + 10ms);
  expect(evictor.kind == AdmissionState::Kind::evict && evictor.worker == held.worker, "a new connection evicts it");
  expect(one.remote_count(peer(7)) == 0U, "eviction releases the evicted remote peer's count");
  one.finished(held.worker, held.connection);   // stale: must not underflow or free the worker
  expect(one.remote_count(peer(7)) == 0U && one.phase(evictor.worker) == secure_frontend::WorkerPhase::header,
         "a stale finish after eviction changes nothing");
}

std::optional<secure_frontend::HttpSocketServer> loopback_listener() {
  secure_frontend::Config listener{};
  listener.host = "127.0.0.1"; listener.port = 0; listener.has_api_key = true;
  std::string error;
  auto server = secure_frontend::HttpSocketServer::bind(listener, 128, &error);
  expect(server.has_value(), "listener: " + error);
  return server;
}

// Opens a client that sends a partial request and accepts it on `server`.
bool connect_and_accept(secure_frontend::HttpSocketServer& server, std::optional<raw_http::Connection>* client,
                        secure_frontend::HttpSocketServer::Accepted* accepted) {
  *client = raw_http::Connection::open("127.0.0.1", server.port());
  expect(client->has_value(), "client connects");
  if (!client->has_value()) return false;
  expect((*client)->send("GET /health HTTP/1.1\r\n"), "client sends a partial request it will never finish");
  auto status = secure_frontend::HttpSocketServer::AcceptStatus::timeout;
  for (int tries = 0; tries < 50 && status != secure_frontend::HttpSocketServer::AcceptStatus::accepted; ++tries) {
    status = server.accept_next(100ms, accepted);
  }
  expect(status == secure_frontend::HttpSocketServer::AcceptStatus::accepted, "accepted");
  return status == secure_frontend::HttpSocketServer::AcceptStatus::accepted;
}

std::vector<secure_frontend::NativeSocket> lingering_natives(const secure_frontend::LingerList& linger) {
  std::vector<secure_frontend::PollEntry> entries;
  linger.append_poll_entries(entries);
  std::vector<secure_frontend::NativeSocket> natives;
  for (const auto& entry : entries) {
    expect(entry.want_read && !entry.want_write, "every lingering socket is polled for reading only");
    natives.push_back(entry.socket);
  }
  return natives;
}

// Pre-flight T11: the socket the overflowing add() closed was closed WITHOUT
// draining (an RST may destroy its 503), so the 503 is read from clients[1],
// which still lingers (half-closed: the 503 then FIN); the evicted one is only
// asserted closed. Which socket was evicted is pinned by native handle: no
// accept happens after the last add(), so no handle can have been reused.
void dos01_linger_list_overflow_closes_oldest() {
  constexpr std::size_t kMax = secure_frontend::api_constants::kLingerListMax;
  auto server = loopback_listener();
  if (!server) return;
  secure_frontend::LingerList linger;
  std::vector<raw_http::Connection> clients;
  std::vector<secure_frontend::NativeSocket> natives;
  const auto now = secure_frontend::LingerList::Clock::now();
  for (std::size_t i = 0; i < kMax + 1U; ++i) {
    std::optional<raw_http::Connection> client;
    secure_frontend::HttpSocketServer::Accepted accepted;
    if (!connect_and_accept(*server, &client, &accepted)) return;
    natives.push_back(accepted.socket.get());
    secure_frontend::refuse_connection(std::move(accepted.socket), linger, now);
    clients.push_back(std::move(*client));
  }
  expect(linger.size() == kMax, "at most kLingerListMax refused sockets are kept");
  const auto held = lingering_natives(linger);
  expect(std::find(held.begin(), held.end(), natives.front()) == held.end(), "the oldest refused socket left the list");
  expect(held == std::vector<secure_frontend::NativeSocket>(natives.begin() + 1, natives.end()),
         "the list holds exactly the newer refused sockets, oldest first");
  expect(clients.front().wait_closed(1s), "the oldest refused socket was closed when the list overflowed");
  const std::string lingering = clients[1].read_until_close(1s);
  expect(lingering.rfind("HTTP/1.1 503 ", 0) == 0 && lingering.find("server_busy") != std::string::npos,
         "a lingering refused client reads the fixed 503");
  expect(lingering.size() <= secure_frontend::api_constants::kRefusalMaxBytes, "refusal is at most 512 bytes");
  linger.service(now + 150ms);
  expect(linger.size() == 0U, "every lingering socket is closed 100 ms after its refusal");
  expect(clients.back().wait_closed(1s), "the newest refused client sees the close");
}

void dos01_linger_closes_on_peer_eof() {
  auto server = loopback_listener();
  if (!server) return;
  secure_frontend::LingerList linger;
  std::optional<raw_http::Connection> client;
  secure_frontend::HttpSocketServer::Accepted accepted;
  if (!connect_and_accept(*server, &client, &accepted)) return;
  const auto now = secure_frontend::LingerList::Clock::now();
  secure_frontend::refuse_connection(std::move(accepted.socket), linger, now);
  linger.service(now);
  expect(linger.size() == 1U, "a refused socket whose peer is still open lingers within the budget");
  const std::string response = client->read_until_close(1s);
  expect(response.rfind("HTTP/1.1 503 ", 0) == 0, "the client reads the 503 before closing its side");
  expect(client->shutdown_send(), "the client half-closes");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (linger.size() != 0U && std::chrono::steady_clock::now() < deadline) {
    std::vector<secure_frontend::PollEntry> entries;
    linger.append_poll_entries(entries);
    (void)secure_frontend::poll_sockets(entries, 20ms);
    linger.service(now);   // the clock stays inside the 100 ms budget: only EOF can close it
  }
  expect(linger.size() == 0U, "peer EOF closes the lingering socket before its budget runs out");
}

void refusal_response_is_fixed_and_small() {
  const std::string r = secure_frontend::refusal_response(std::chrono::system_clock::now());
  expect(r.rfind("HTTP/1.1 503 Service Unavailable\r\n", 0) == 0 && r.find("Retry-After: 1\r\n") != std::string::npos &&
         r.find("\"code\":\"server_busy\"") != std::string::npos && r.size() <= secure_frontend::api_constants::kRefusalMaxBytes,
         "fixed 503 server_busy with Retry-After: 1 in at most 512 bytes (" + std::to_string(r.size()) + ")");
}

// --- Task 13: SupervisorPort + GenerationGate (spec §2.5, D-1) -----------------

struct GateFixture final {
  test_support::FakeFactory fakes{[](test_support::FakeChild& child, std::uint64_t) { child.push_line("READY 4096 stop"); }};
  std::shared_ptr<test_support::ManualClock> clock = std::make_shared<test_support::ManualClock>();
  secure_frontend::EngineSupervisor supervisor{{}, fakes.factory(), clock};
  secure_frontend::SupervisorPort port{supervisor};
  GateFixture() {
    expect(supervisor.start(), "supervisor starts");
    expect(test_support::wait_generation_ready(supervisor, 1U, 5s), "fake generation ready");
  }
  ~GateFixture() { supervisor.shutdown(5s); }
};
std::chrono::steady_clock::time_point deadline_in(const std::chrono::milliseconds d) { return std::chrono::steady_clock::now() + d; }
const std::function<bool()> kStaying = [] { return false; };

void gate_first_request_gets_the_slot() {
  GateFixture f;
  secure_frontend::GenerationGate gate({.max_queued = 4, .queue_timeout = 1000ms});
  auto result = gate.admit(f.port, deadline_in(1s), kStaying);
  expect(std::holds_alternative<secure_frontend::Lease>(result), "the first chat gets the lease at once");
  expect(f.port.snapshot().state == secure_frontend::SupervisorState::generating, "SP2 is generating");
}

void gate_queue_full_is_busy() {
  GateFixture f;
  secure_frontend::GenerationGate gate({.max_queued = 1, .queue_timeout = 5000ms});
  auto holder = gate.admit(f.port, deadline_in(1s), kStaying);
  std::thread waiter([&] { (void)gate.admit(f.port, deadline_in(2s), kStaying); });
  for (int i = 0; i < 100 && gate.waiting() != 1U; ++i) std::this_thread::sleep_for(5ms);
  const auto started = std::chrono::steady_clock::now();
  auto refused = gate.admit(f.port, deadline_in(5s), kStaying);
  expect(std::holds_alternative<secure_frontend::GateRefusal>(refused) &&
         std::get<secure_frontend::GateRefusal>(refused) == secure_frontend::GateRefusal::busy, "a full queue answers busy");
  expect(std::chrono::steady_clock::now() - started < 100ms, "at once");
  { auto lease = std::get<secure_frontend::Lease>(std::move(holder)); }
  gate.release();
  waiter.join();
}

void gate_zero_queue_admits_one_at_a_time() {
  // Task 16 (DOS-02 serial mode): max_queued bounds WAITERS only; an arrival
  // that finds the slot idle is served even at max_queued = 0.
  GateFixture f;
  secure_frontend::GenerationGate gate({.max_queued = 0, .queue_timeout = 5000ms});
  auto first = gate.admit(f.port, deadline_in(5s), kStaying);
  expect(std::holds_alternative<secure_frontend::Lease>(first), "max_queued = 0 still admits when the slot is idle (serial mode)");
  const auto started = std::chrono::steady_clock::now();
  auto second = gate.admit(f.port, deadline_in(5s), kStaying);
  expect(std::holds_alternative<secure_frontend::GateRefusal>(second) &&
         std::get<secure_frontend::GateRefusal>(second) == secure_frontend::GateRefusal::busy, "with the slot taken, zero queue means busy at once");
  expect(std::chrono::steady_clock::now() - started < 100ms, "at once");
  { auto lease = std::get<secure_frontend::Lease>(std::move(first)); }
  gate.release();
  auto third = gate.admit(f.port, deadline_in(5s), kStaying);
  expect(std::holds_alternative<secure_frontend::Lease>(third), "after release the next arrival gets the slot");
  { auto lease = std::get<secure_frontend::Lease>(std::move(third)); }
  gate.release();
}

void gate_fifo_order() {
  GateFixture f;
  secure_frontend::GenerationGate gate({.max_queued = 3, .queue_timeout = 10000ms});
  auto holder = std::optional<secure_frontend::Lease>{std::get<secure_frontend::Lease>(gate.admit(f.port, deadline_in(1s), kStaying))};
  std::mutex order_mutex;
  std::vector<int> order;
  std::vector<std::thread> waiters;
  for (int i = 0; i < 3; ++i) {
    waiters.emplace_back([&, i] {
      auto r = gate.admit(f.port, deadline_in(10s), kStaying);
      if (!std::holds_alternative<secure_frontend::Lease>(r)) return;
      { std::lock_guard g(order_mutex); order.push_back(i); }
      { auto lease = std::get<secure_frontend::Lease>(std::move(r)); }
      gate.release();
    });
    for (int t = 0; t < 100 && gate.waiting() != static_cast<std::size_t>(i + 1); ++t) std::this_thread::sleep_for(5ms);
  }
  holder.reset();
  gate.release();
  for (auto& t : waiters) t.join();
  expect(order == std::vector<int>{0, 1, 2}, "strict FIFO by admission time");
}

void gate_timeout_returns_timeout() {
  GateFixture f;
  secure_frontend::GenerationGate gate({.max_queued = 1, .queue_timeout = 300ms});
  auto holder = gate.admit(f.port, deadline_in(1s), kStaying);
  const auto started = std::chrono::steady_clock::now();
  auto r = gate.admit(f.port, deadline_in(300ms), kStaying);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  expect(std::holds_alternative<secure_frontend::GateRefusal>(r) && std::get<secure_frontend::GateRefusal>(r) == secure_frontend::GateRefusal::timeout,
         "the queue deadline answers timeout");
  expect(elapsed >= 280ms && elapsed < 1s, "at the deadline");
  expect(gate.waiting() == 0U, "the ticket left the queue");
}

void gate_cancelled_waiter_leaves_queue() {
  GateFixture f;
  secure_frontend::GenerationGate gate({.max_queued = 2, .queue_timeout = 5000ms});
  auto holder = gate.admit(f.port, deadline_in(1s), kStaying);
  std::atomic<bool> gone{false};
  std::optional<secure_frontend::GateRefusal> refusal;
  std::thread waiter([&] {
    auto r = gate.admit(f.port, deadline_in(5s), [&gone] { return gone.load(); });
    if (std::holds_alternative<secure_frontend::GateRefusal>(r)) refusal = std::get<secure_frontend::GateRefusal>(r);
  });
  for (int i = 0; i < 100 && gate.waiting() != 1U; ++i) std::this_thread::sleep_for(5ms);
  gone = true;
  waiter.join();
  expect(refusal == std::optional<secure_frontend::GateRefusal>{secure_frontend::GateRefusal::cancelled}, "a gone client leaves silently");
  expect(gate.waiting() == 0U, "and frees its queue place");
}

void gate_fast_fails_when_engine_not_ready() {
  test_support::FakeFactory silent([](test_support::FakeChild&, std::uint64_t) {});   // never READY: stays starting
  secure_frontend::EngineSupervisor supervisor({}, silent.factory(), std::make_shared<test_support::ManualClock>());
  expect(supervisor.start(), "supervisor starts (engine never reaches READY)");
  secure_frontend::SupervisorPort port(supervisor);
  secure_frontend::GenerationGate gate({.max_queued = 4, .queue_timeout = 5000ms});
  const auto started = std::chrono::steady_clock::now();
  auto r = gate.admit(port, deadline_in(5s), kStaying);
  expect(std::holds_alternative<secure_frontend::GateRefusal>(r) &&
         std::get<secure_frontend::GateRefusal>(r) == secure_frontend::GateRefusal::engine_unavailable, "starting: engine_unavailable");
  expect(std::chrono::steady_clock::now() - started < 100ms, "never waits for READY (SP2 V11)");
  supervisor.shutdown(5s);
}

void gate_shutdown_refuses_waiters() {
  GateFixture f;
  secure_frontend::GenerationGate gate({.max_queued = 2, .queue_timeout = 5000ms});
  auto holder = gate.admit(f.port, deadline_in(1s), kStaying);
  std::optional<secure_frontend::GateRefusal> refusal;
  std::thread waiter([&] {
    auto r = gate.admit(f.port, deadline_in(5s), kStaying);
    if (std::holds_alternative<secure_frontend::GateRefusal>(r)) refusal = std::get<secure_frontend::GateRefusal>(r);
  });
  for (int i = 0; i < 100 && gate.waiting() != 1U; ++i) std::this_thread::sleep_for(5ms);
  gate.shutdown();
  waiter.join();
  expect(refusal == std::optional<secure_frontend::GateRefusal>{secure_frontend::GateRefusal::shutting_down}, "queued tickets get shutting_down");
  auto late = gate.admit(f.port, deadline_in(1s), kStaying);
  expect(std::holds_alternative<secure_frontend::GateRefusal>(late), "nothing is admitted after shutdown");
}
// SP3 Task 21 (spec §8): the request log is fixed-field, rate-limited, and off by default.
void request_log_format_and_token_bucket() {
  using secure_frontend::RequestLogger;
  const secure_frontend::RequestLogRecord rec{secure_frontend::RouteId::chat, 200, "", std::chrono::milliseconds(12), 345U,
                                              secure_frontend::AddressScope::loopback};
  expect(secure_frontend::format_request_line(7, rec) == "req 7 chat 200 - 12 345 loopback", "line format");
  const secure_frontend::RequestLogRecord gone{secure_frontend::RouteId::health, 0, "client_gone", std::chrono::milliseconds(1), 0U,
                                               secure_frontend::AddressScope::remote};
  expect(secure_frontend::format_request_line(8, gone) == "req 8 health 0 client_gone 1 0 remote", "log-only code");
  // every route the router knows is logged by its own name, never as "other" (a route added later included)
  using R = secure_frontend::RouteId;
  const std::pair<R, std::string_view> named[] = {
      {R::strata_calibrate, "strata_calibrate"},       {R::strata_calibrate_cancel, "strata_calibrate_cancel"},
      {R::strata_calibrate_auto, "strata_calibrate_auto"}, {R::strata_login_link, "strata_login_link"},
      {R::strata_alerts, "strata_alerts"},             {R::strata_alerts_settings, "strata_alerts_settings"},
      {R::strata_alerts_password, "strata_alerts_password"}, {R::strata_alerts_test, "strata_alerts_test"},
      {R::strata_search, "strata_search"},             {R::strata_search_settings, "strata_search_settings"},
      {R::strata_search_test, "strata_search_test"},   {R::strata_config, "strata_config"},
      {R::strata_config_settings, "strata_config_settings"}, {R::strata_key_rotate, "strata_key_rotate"},
      {R::strata_folders, "strata_folders"},           {R::strata_folder_private, "strata_folder_private"},
      {R::strata_folder_new, "strata_folder_new"},     {R::strata_card_add, "strata_card_add"},
      {R::strata_card_folder, "strata_card_folder"},   {R::strata_card_delete_begin, "strata_card_delete_begin"},
      {R::strata_card_delete, "strata_card_delete"},   {R::strata_folder_sharing, "strata_folder_sharing"},
      {R::strata_about, "strata_about"},               {R::strata_download_link, "strata_download_link"}};
  for (const auto& [route, name] : named) {
    const secure_frontend::RequestLogRecord line{route, 200, "", std::chrono::milliseconds(1), 0U, secure_frontend::AddressScope::loopback};
    expect(secure_frontend::format_request_line(1, line) == "req 1 " + std::string{name} + " 200 - 1 0 loopback",
           "the " + std::string{name} + " route is logged by its name: " + secure_frontend::format_request_line(1, line));
  }
  auto now = RequestLogger::Clock::time_point{} + std::chrono::seconds(10);
  std::vector<std::string> lines;
  RequestLogger logger(true, [&lines](std::string_view line) { lines.emplace_back(line); }, [&now] { return now; });
  for (int i = 0; i < 300; ++i) logger.record(rec);
  expect(lines.size() == 100U, "a burst of 100 lines");
  now += std::chrono::seconds(1);
  for (int i = 0; i < 300; ++i) logger.record(rec);
  const auto suppressed = std::count_if(lines.begin(), lines.end(), [](const std::string& l) { return l.rfind("log: ", 0) == 0; });
  expect(suppressed == 1 && lines[100] == "log: 200 request lines suppressed", "one suppression line with the drop count");
  expect(lines.size() == 100U + 1U + 20U, "20 more lines after one second of refill");
  RequestLogger off(false, [&lines](std::string_view line) { lines.emplace_back(line); }, [&now] { return now; });
  const auto before = lines.size();
  off.record(rec);
  expect(lines.size() == before, "logging.requests=false writes nothing per request");
}

void request_log_names_streamed_chats() {
  secure_frontend::RequestLogRecord stream{secure_frontend::RouteId::chat, 200, "client_gone", std::chrono::milliseconds(7), 99U,
                                           secure_frontend::AddressScope::loopback, true};
  expect(secure_frontend::format_request_line(3, stream) == "req 3 chat_stream 200 client_gone 7 99 loopback", "streamed chat line");
  stream.streamed = false;
  expect(secure_frontend::format_request_line(4, stream).find(" chat 200 ") != std::string::npos, "complete chat line unchanged");
}
}  // namespace

int main(int argc, char** argv) {
  secure_frontend::WinsockGuard winsock;
  return secure_frontend_test::run_cases(argc, argv, {
      {"dos01_oldest_header_phase_connection_is_evicted", dos01_oldest_header_phase_connection_is_evicted},
      {"dos01_remote_address_cap", dos01_remote_address_cap},
      {"dos01_linger_list_overflow_closes_oldest", dos01_linger_list_overflow_closes_oldest},
      {"dos01_linger_closes_on_peer_eof", dos01_linger_closes_on_peer_eof},
      {"refusal_response_is_fixed_and_small", refusal_response_is_fixed_and_small},
      {"gate_first_request_gets_the_slot", gate_first_request_gets_the_slot},
      {"gate_queue_full_is_busy", gate_queue_full_is_busy},
      {"gate_zero_queue_admits_one_at_a_time", gate_zero_queue_admits_one_at_a_time},
      {"request_log_format_and_token_bucket", request_log_format_and_token_bucket},
      {"request_log_names_streamed_chats", request_log_names_streamed_chats},
      {"gate_fifo_order", gate_fifo_order},
      {"gate_timeout_returns_timeout", gate_timeout_returns_timeout},
      {"gate_cancelled_waiter_leaves_queue", gate_cancelled_waiter_leaves_queue},
      {"gate_fast_fails_when_engine_not_ready", gate_fast_fails_when_engine_not_ready},
      {"gate_shutdown_refuses_waiters", gate_shutdown_refuses_waiters},
  }, failures);
}
