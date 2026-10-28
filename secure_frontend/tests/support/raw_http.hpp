#pragma once
// Test-only loopback HTTP client over raw sockets. Callers on Windows hold a
// secure_frontend::WinsockGuard for the whole test process.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace raw_http {

#if defined(_WIN32)
using Native = SOCKET;
inline constexpr Native kBad = INVALID_SOCKET;
inline void close_native(Native s) { closesocket(s); }
inline int poll_one(Native s, short events, int ms) { WSAPOLLFD p{s, events, 0}; return WSAPoll(&p, 1, ms); }
#else
using Native = int;
inline constexpr Native kBad = -1;
inline void close_native(Native s) { ::close(s); }
inline int poll_one(Native s, short events, int ms) { pollfd p{s, events, 0}; return ::poll(&p, 1, ms); }
#endif

class Connection final {
 public:
  // receive_buffer > 0 shrinks SO_RCVBUF before connect (the window only
  // shrinks when set before the handshake).
  static std::optional<Connection> open(std::string_view ip, std::uint16_t port, int receive_buffer = 0) {
    sockaddr_storage address{};
    socklen_t size = 0;
    int family = AF_INET;
    const std::string text{ip};
    auto* v4 = reinterpret_cast<sockaddr_in*>(&address);
    auto* v6 = reinterpret_cast<sockaddr_in6*>(&address);
    if (inet_pton(AF_INET, text.c_str(), &v4->sin_addr) == 1) {
      v4->sin_family = AF_INET; v4->sin_port = htons(port); size = sizeof(sockaddr_in);
    } else if (inet_pton(AF_INET6, text.c_str(), &v6->sin6_addr) == 1) {
      family = AF_INET6; v6->sin6_family = AF_INET6; v6->sin6_port = htons(port); size = sizeof(sockaddr_in6);
    } else {
      return std::nullopt;
    }
    const Native s = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBad) return std::nullopt;
    if (receive_buffer > 0 &&
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receive_buffer), sizeof(receive_buffer)) != 0) {
      close_native(s);
      return std::nullopt;
    }
    if (::connect(s, reinterpret_cast<const sockaddr*>(&address), size) != 0) { close_native(s); return std::nullopt; }
    const int one = 1;
    (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    return Connection{s};
  }
  Connection(Connection&& other) noexcept : s_(std::exchange(other.s_, kBad)) {}
  Connection& operator=(Connection&& other) noexcept { if (this != &other) { close(); s_ = std::exchange(other.s_, kBad); } return *this; }
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  ~Connection() { close(); }

  bool send(std::string_view bytes) {
    while (!bytes.empty()) {
#if defined(_WIN32)
      const int n = ::send(s_, bytes.data(), static_cast<int>(std::min<std::size_t>(bytes.size(), 1U << 20)), 0);
#else
      const ssize_t n = ::send(s_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
#endif
      if (n <= 0) return false;
      bytes.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
  }
  bool shutdown_send() {
#if defined(_WIN32)
    return ::shutdown(s_, SD_SEND) == 0;
#else
    return ::shutdown(s_, SHUT_WR) == 0;
#endif
  }
  // Everything that arrives until EOF/reset or `timeout`; *closed says which.
  std::string read_until_close(std::chrono::milliseconds timeout, bool* closed = nullptr) {
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (closed != nullptr) *closed = false;
    char buffer[65536];
    while (std::chrono::steady_clock::now() < deadline) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (poll_one(s_, POLLIN, static_cast<int>(std::max<long long>(1, left.count()))) <= 0) continue;
      const auto n = ::recv(s_, buffer, static_cast<int>(sizeof(buffer)), 0);
      if (n <= 0) { if (closed != nullptr) *closed = true; break; }
      out.append(buffer, static_cast<std::size_t>(n));
    }
    return out;
  }
  // Whatever arrives within `window`, without waiting for EOF.
  std::string read_for(std::chrono::milliseconds window) {
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + window;
    char buffer[65536];
    while (std::chrono::steady_clock::now() < deadline) {
      if (poll_one(s_, POLLIN, 10) <= 0) continue;
      const auto n = ::recv(s_, buffer, static_cast<int>(sizeof(buffer)), 0);
      if (n <= 0) break;
      out.append(buffer, static_cast<std::size_t>(n));
    }
    return out;
  }
  bool wait_closed(std::chrono::milliseconds timeout) {
    bool closed = false;
    (void)read_until_close(timeout, &closed);
    return closed;
  }
  void close() { if (s_ != kBad) { close_native(s_); s_ = kBad; } }
  [[nodiscard]] Native native() const { return s_; }

 private:
  explicit Connection(Native s) : s_(s) {}
  Native s_{kBad};
};

struct Response final {
  int status{0};
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  bool saw_continue{false};
  [[nodiscard]] std::optional<std::string> header(std::string_view name) const {
    for (const auto& [n, v] : headers) {
      if (n.size() == name.size() && std::equal(n.begin(), n.end(), name.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
          })) {
        return v;
      }
    }
    return std::nullopt;
  }
};

// Parses one complete response; an interim "100 Continue" block is skipped and recorded.
inline std::optional<Response> parse_response(std::string_view raw) {
  Response response;
  if (raw.rfind("HTTP/1.1 100 ", 0) == 0) {
    const auto end = raw.find("\r\n\r\n");
    if (end == std::string_view::npos) return std::nullopt;
    raw.remove_prefix(end + 4U);
    response.saw_continue = true;
  }
  const auto head_end = raw.find("\r\n\r\n");
  if (raw.size() < 12U || raw.rfind("HTTP/1.1 ", 0) != 0 || head_end == std::string_view::npos) return std::nullopt;
  response.status = std::atoi(std::string{raw.substr(9U, 3U)}.c_str());
  std::size_t start = raw.find("\r\n") + 2U;
  while (start < head_end) {
    const auto end = raw.find("\r\n", start);
    const auto line = raw.substr(start, end - start);
    const auto colon = line.find(':');
    if (colon != std::string_view::npos) {
      std::string_view value = line.substr(colon + 1U);
      while (!value.empty() && value.front() == ' ') value.remove_prefix(1U);
      response.headers.emplace_back(std::string{line.substr(0, colon)}, std::string{value});
    }
    start = end + 2U;
  }
  response.body = std::string{raw.substr(head_end + 4U)};
  if (const auto length = response.header("Content-Length")) {
    const auto expected = static_cast<std::size_t>(std::stoull(*length));
    if (response.body.size() < expected) return std::nullopt;
    response.body.resize(expected);
  }
  return response;
}

inline std::string request(std::string_view method, std::string_view target, std::string_view host,
                           const std::vector<std::string>& header_lines, std::string_view body = {}) {
  std::string out = std::string{method} + " " + std::string{target} + " HTTP/1.1\r\nHost: " + std::string{host} + "\r\n";
  for (const auto& line : header_lines) out += line + "\r\n";
  if (!body.empty() || method == "POST") out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  out += "\r\n";
  out += body;
  return out;
}

inline std::optional<Response> exchange(std::string_view ip, std::uint16_t port, std::string_view raw_request,
                                        std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
  auto connection = Connection::open(ip, port);
  if (!connection || !connection->send(raw_request)) return std::nullopt;
  return parse_response(connection->read_until_close(timeout));
}

inline std::string bearer(std::string_view token) { return "Authorization: Bearer " + std::string{token}; }

}  // namespace raw_http
