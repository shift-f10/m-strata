#pragma once

#include "secure_frontend/config.hpp"
#include "secure_frontend/connection_handler.hpp"
#include "secure_frontend/socket_io.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace secure_frontend {

// The listening socket. It only accepts TCP clients; it never creates
// outbound sockets or resolves names (spec §6.1): the bind host must be an IP
// literal. Networking is initialized by the process's WinsockGuard, not here.
// The listener and every accepted socket are close-on-exec / non-inheritable
// (SP2 K4), and accepted sockets are non-blocking (socket_io's precondition).
class HttpSocketServer final {
 public:
  // port 0 binds an ephemeral port (tests only; the schema range is 1-65535).
  [[nodiscard]] static std::optional<HttpSocketServer> bind(const Config& listener, int backlog, std::string* error);
  HttpSocketServer(HttpSocketServer&&) noexcept;
  HttpSocketServer& operator=(HttpSocketServer&&) noexcept;
  ~HttpSocketServer();

  enum class AcceptStatus { accepted, timeout, transient, listener_failed, stopped };
  struct Accepted final {
    Socket socket;
    AddressScope peer_scope{AddressScope::remote};
    std::array<unsigned char, 17> peer_key{};   // family byte + 16 address bytes (IPv4-mapped folded to v4)
    std::chrono::steady_clock::time_point accepted_at{};
  };
  // Waits at most `slice` for a client; a transient failure sleeps the 50 ms backoff first.
  [[nodiscard]] AcceptStatus accept_next(std::chrono::milliseconds slice, Accepted* out);
  // Accepts one client (polling in kAcceptPollSlice) and serves it with
  // serve_connection. True for every answered request (4xx/5xx included);
  // false for a client that never completed a request, a write that failed,
  // or a stopped/failed listener (error "listener stopped" after
  // shutdown_listener(), which also ends an in-flight P1/P3 within one slice).
  [[nodiscard]] bool serve_one(RequestRouter& router, const ConnectionLimits& limits, std::string* error);
  [[nodiscard]] std::uint16_t port() const;
  // True once accept() failed because the listening socket itself is broken.
  [[nodiscard]] bool listener_failed() const noexcept;
  // Closes the listener; accept_next/serve_one return `stopped` afterwards and
  // native_handle() is kInvalidNativeSocket. Thread-safe and idempotent.
  void shutdown_listener() noexcept;
  [[nodiscard]] NativeSocket native_handle() const noexcept;

 private:
  struct State;
  explicit HttpSocketServer(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};

}  // namespace secure_frontend
