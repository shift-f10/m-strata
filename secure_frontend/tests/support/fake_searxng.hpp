#pragma once

// Web search tests (spec 2026-10-27-web-search-design.md): a fake SearXNG instance on loopback, shared by the search
// program's tests and the server's.

#include <chrono>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace test_support {

#if defined(_WIN32)
using NativeSocket = SOCKET;
using socklen_t = int;
#else
using NativeSocket = int;
#endif

// SearXNG's JSON reply for two results (the shape its /search?format=json answers; a score is a fraction).
inline const std::string kSearxngReply =
    R"({"query": "rust 1.90", "number_of_results": 0, "results": [)"
    R"({"url": "https://blog.rust-lang.org/2026/09/18/Rust-1.90.0/", "title": "Announcing Rust 1.90.0", )"
    R"("content": "The Rust team is happy to announce\n a new version,   1.90.0.", "engine": "duckduckgo", "score": 2.5, )"
    R"("publishedDate": "2026-09-18T00:00:00"},)"
    R"({"url": "https://doc.rust-lang.org/stable/releases.html", "title": "Rust Release Notes", "content": "", "score": 1.0},)"
    R"({"title": "no url, skipped", "content": "x"}], "answers": ["Rust 1.90.0 was released on 18 September 2026"], )"
    R"("corrections": [], "infoboxes": [], "suggestions": [], "unresponsive_engines": []})";

// A fake SearXNG on loopback: answers each connection SearXNG's JSON, chunked, and keeps the request line it read.
class FakeSearxng final {
 public:
  // `delay`: how long each answer waits once its request has arrived (a slow instance).
  explicit FakeSearxng(std::string reply = kSearxngReply, const std::chrono::milliseconds delay = std::chrono::milliseconds(0))
      : reply_(std::move(reply)), delay_(delay) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    (void)::listen(listener_, 4);
    socklen_t length = sizeof(address);
    (void)::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
    port_ = ntohs(address.sin_port);
    thread_ = std::thread([this] {
      for (;;) {
        const auto client = ::accept(listener_, nullptr, nullptr);
#if defined(_WIN32)
        if (client == INVALID_SOCKET) return;
#else
        if (client < 0) return;
#endif
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
          const auto got = ::recv(client, buffer, sizeof(buffer), 0);
          if (got <= 0) break;
          request.append(buffer, static_cast<std::size_t>(got));
        }
        {
          const std::scoped_lock lock(mutex_);
          requests_.push_back(request.substr(0, request.find("\r\n")));
        }
        std::this_thread::sleep_for(delay_);
        std::ostringstream reply;
        reply << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n" << std::hex
              << reply_.size() << "\r\n" << reply_ << "\r\n0\r\n\r\n";
        const std::string bytes = reply.str();
        (void)::send(client, bytes.data(), static_cast<int>(bytes.size()), 0);
#if defined(_WIN32)
        closesocket(client);
#else
        ::close(client);
#endif
      }
    });
  }
  ~FakeSearxng() {
#if defined(_WIN32)
    closesocket(listener_);
#else
    ::shutdown(listener_, SHUT_RDWR);
    ::close(listener_);
#endif
    thread_.join();
  }
  FakeSearxng(const FakeSearxng&) = delete;
  FakeSearxng& operator=(const FakeSearxng&) = delete;
  [[nodiscard]] std::uint16_t port() const { return port_; }
  [[nodiscard]] std::vector<std::string> requests() const {
    const std::scoped_lock lock(mutex_);
    return requests_;
  }

 private:
  std::string reply_;
  std::chrono::milliseconds delay_;
  NativeSocket listener_{};
  std::uint16_t port_{0};
  std::thread thread_;
  mutable std::mutex mutex_;
  std::vector<std::string> requests_;
};

}  // namespace test_support
