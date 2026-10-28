#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace secure_frontend_test {

#if defined(_WIN32)
using TestSocket = SOCKET;
inline constexpr TestSocket kNoSocket = INVALID_SOCKET;
inline void close_test_socket(const TestSocket s) { closesocket(s); }
inline bool winsock_ready() {
  static const bool started = [] { WSADATA data{}; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
  return started;
}
#else
using TestSocket = int;
inline constexpr TestSocket kNoSocket = -1;
inline void close_test_socket(const TestSocket s) { close(s); }
inline bool winsock_ready() { return true; }
#endif

inline TestSocket connect_loopback(const std::uint16_t port, const std::chrono::milliseconds timeout) {
  if (!winsock_ready()) return kNoSocket;
  const TestSocket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kNoSocket) return kNoSocket;
#if defined(_WIN32)
  const DWORD ms = static_cast<DWORD>(timeout.count());
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
  const timeval tv{static_cast<time_t>(timeout.count() / 1000), static_cast<suseconds_t>((timeout.count() % 1000) * 1000)};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    close_test_socket(s);
    return kNoSocket;
  }
  return s;
}

inline bool can_connect(const std::uint16_t port) {
  const TestSocket s = connect_loopback(port, std::chrono::seconds(1));
  if (s == kNoSocket) return false;
  close_test_socket(s);
  return true;
}

// Sends one request to 127.0.0.1:<port> and reads to EOF; empty if the connect fails.
inline std::string http_exchange(const std::uint16_t port, const std::string& request,
                                 const std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
  const TestSocket s = connect_loopback(port, timeout);
  if (s == kNoSocket) return {};
  std::size_t sent = 0;
  while (sent < request.size()) {
#if defined(_WIN32)
    const int n = send(s, request.data() + sent, static_cast<int>(request.size() - sent), 0);
#else
    const ssize_t n = send(s, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
#endif
    if (n <= 0) break;
    sent += static_cast<std::size_t>(n);
  }
  std::string response;
  char chunk[4096];
  for (;;) {
#if defined(_WIN32)
    const int n = recv(s, chunk, static_cast<int>(sizeof(chunk)), 0);
#else
    const ssize_t n = recv(s, chunk, sizeof(chunk), 0);
#endif
    if (n <= 0) break;
    response.append(chunk, static_cast<std::size_t>(n));
  }
  close_test_socket(s);
  return response;
}

// Binds 127.0.0.1:0 to learn a free port, then releases it.
inline std::uint16_t pick_free_port() {
  if (!winsock_ready()) return 0;
  const TestSocket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  std::uint16_t port = 0;
  if (bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
#if defined(_WIN32)
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(s, reinterpret_cast<sockaddr*>(&address), &length) == 0) port = ntohs(address.sin_port);
  }
  close_test_socket(s);
  return port;
}

}  // namespace secure_frontend_test
