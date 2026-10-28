#pragma once
#include "support/connection_scan.hpp"
#include "support/raw_http.hpp"
#include "support/server_harness.hpp"
#include "support/test_paths.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace test_support {

inline bool arm_egress_tripwire(ServerOptions& options, const std::filesystem::path& record) {
#if defined(STRATA_EGRESS_TRIPWIRE_PATH)
  options.extra_env["LD_PRELOAD"] = STRATA_EGRESS_TRIPWIRE_PATH;
  options.extra_env["STRATA_EGRESS_RECORD"] = utf8_path(record);
  return true;
#else
  (void)options;
  (void)record;
  return false;
#endif
}

inline std::string file_text(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Spec §6.2 connection-table scan, every 50 ms, server and (POSIX) its descendants.
class SocketScan final {
 public:
  SocketScan(const long pid, const std::uint16_t port) : pid_(pid), port_(port), thread_([this] { loop(); }) {}
  ~SocketScan() { (void)stop(); }
  std::vector<std::string> stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_);
    return violations_;
  }

 private:
  void violation(const std::string& text) {
    std::lock_guard lock(mutex_);
    violations_.push_back(text);
  }
  void check(const long pid, const bool is_server) {
#if defined(_WIN32)
    // The iphlpapi tables attribute every row to its owning pid, so the plan's
    // strict rule runs unchanged: on the server only the listener port; nothing
    // anywhere else.
    for (const auto& row : sockets_of(pid)) {
      const bool tcp = row.kind == "tcp" || row.kind == "tcp6";
      const bool ok = tcp && is_server && row.local_port == port_;
      if (ok) continue;
      violation((is_server ? "server " : "engine ") + row.kind + " local port " + std::to_string(row.local_port));
    }
#else
    if (is_server) {
      // Ruling T19-scan: the server sets PR_SET_DUMPABLE(0) (SP0), so its rows
      // can only be uid-attributed (ruling T12-scan) — the test's own client
      // connections and the container's ambient same-uid sockets are
      // inseparable from the server's. What the scan CAN pin on the server is the
      // listener set (NET-01 semantics, differential like ruling T12-net01):
      // every listener absent from the start baseline and not on the server's
      // own port is a violation. The server's own outbound calls are pinned by
      // the interposer case, which has no attribution problem.
      for (const auto& row : sockets_of(pid)) {
        if (!row.listening) continue;
        const std::string key = row.kind + " local port " + std::to_string(row.local_port);
        if (!baseline_taken_) { baseline_.insert(key); continue; }
        if (baseline_.count(key) != 0U || row.local_port == port_) continue;
        violation("server " + key);
      }
      baseline_taken_ = true;
    } else {
      // Children (the fake engine) are dumpable: exact inode attribution. No
      // inodes means a socket-less engine (fine) or a child that just died
      // (nothing to attribute) — either way nothing is provable, so nothing is
      // claimed; a live engine that opened ANY socket always lands its inodes.
      if (socket_inodes(pid).empty()) return;
      for (const auto& row : sockets_of(pid)) violation("engine " + row.kind + " local port " + std::to_string(row.local_port));
    }
#endif
  }
  void loop() {
    while (running_) {
      check(pid_, true);
#if !defined(_WIN32)
      for (const long child : descendants_of(pid_)) check(child, false);
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  long pid_;
  std::uint16_t port_;
  std::atomic<bool> running_{true};
  std::mutex mutex_;
  std::vector<std::string> violations_;
  std::thread thread_;
#if !defined(_WIN32)
  // Ruling T19-scan: the differential listener baseline only applies to the
  // Linux uid-attributed scan; the Windows tables attribute per-pid exactly.
  std::set<std::string> baseline_;
  bool baseline_taken_{false};
#endif
};

// The §6.2 lifecycle (M1 part; M2 adds a stream): health, models, chat, cancel,
// command-like values, refusal flood, shutdown. The engine restart happened at
// startup (--fail-first=1).
inline void run_lifecycle(ServerProcess& server, const std::string& token) {
  using namespace std::chrono_literals;
  const std::string a = "127.0.0.1:" + std::to_string(server.port());
  const auto get = [&](const std::string& target) {
    return raw_http::exchange("127.0.0.1", server.port(), raw_http::request("GET", target, a, {raw_http::bearer(token)}));
  };
  const auto chat = [&](const std::string& body) {
    return raw_http::exchange("127.0.0.1", server.port(),
                              raw_http::request("POST", "/v1/chat/completions", a, {raw_http::bearer(token), "Content-Type: application/json"}, body),
                              30s);
  };
  (void)get("/health");
  (void)get("/v1/models");
  (void)chat(R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}],"max_tokens":2})");
  {
    auto c = raw_http::Connection::open("127.0.0.1", server.port());
    if (c) (void)c->send(raw_http::request("POST", "/v1/chat/completions", a, {raw_http::bearer(token), "Content-Type: application/json"},
                                           R"({"model":"fake-model","messages":[{"role":"user","content":"Hi"}]})"));
    std::this_thread::sleep_for(300ms);
  }   // cancel
  for (const std::string body : {R"({"model":"pull:x","messages":[{"role":"user","content":"Hi"}]})",
                                 R"({"model":"fake-model","messages":[{"role":"user","content":"see https://example.com/m.gguf"}]})",
                                 R"({"model":"fake-model","messages":[{"role":"user","content":"mcp://tools.example"}]})",
                                 R"({"model":"fake-model","messages":[{"role":"user","content":"file:///etc/passwd"}]})",
                                 R"({"model":"fake-model","messages":[{"role":"user","content":"data:text/plain,x"}]})"}) {
    (void)chat(body);
  }
  (void)get("/v1/models/pull:x");
  std::vector<raw_http::Connection> flood;
  for (int i = 0; i < 40; ++i) {
    auto c = raw_http::Connection::open("127.0.0.1", server.port());
    if (c) { (void)c->send("GET /health HTTP/1.1\r\n"); flood.push_back(std::move(*c)); }
  }
  std::this_thread::sleep_for(200ms);
  flood.clear();
  (void)get("/health");
}

}  // namespace test_support
