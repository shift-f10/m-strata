#include "secure_frontend/http_socket_server.hpp"

#include "secure_frontend/api_constants.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <thread>
#include <utility>
#include <variant>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace secure_frontend {
namespace {

#if defined(_WIN32)
using AddressLength = int;
#else
using AddressLength = socklen_t;
#endif

// Pre-flight V12 (SP2 K4): the listener is created non-inheritable, so no
// engine child (or any other child) can ever hold it.
NativeSocket open_listener_socket(const int family) noexcept {
#if defined(_WIN32)
  return WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#elif defined(SOCK_CLOEXEC)
  return ::socket(family, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
#else
  const NativeSocket s = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
  if (s != kInvalidNativeSocket && !set_no_inherit(s)) {
    close_native_socket(s);
    return kInvalidNativeSocket;
  }
  return s;
#endif
}

// accept() whose result is close-on-exec / non-inheritable and non-blocking
// (socket_io's precondition), or kInvalidNativeSocket with the socket error set.
NativeSocket accept_client(const NativeSocket listener, sockaddr_storage* peer, AddressLength* peer_size) noexcept {
#if defined(__linux__)
  return ::accept4(listener, reinterpret_cast<sockaddr*>(peer), peer_size, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
  const NativeSocket client = ::accept(listener, reinterpret_cast<sockaddr*>(peer), peer_size);
  if (client == kInvalidNativeSocket) return client;
  if (!set_no_inherit(client) || !set_non_blocking(client)) {
    const int code = detail::last_socket_error();
    close_native_socket(client);
    detail::set_last_socket_error(code);
    return kInvalidNativeSocket;
  }
  return client;
#endif
}

void classify_peer(const sockaddr_storage& address, HttpSocketServer::Accepted* out) {
  out->peer_key.fill(0);
  if (address.ss_family == AF_INET) {
    const auto* v4 = reinterpret_cast<const sockaddr_in*>(&address);
    unsigned char bytes[4];
    std::memcpy(bytes, &v4->sin_addr, 4U);
    out->peer_key[0] = 4;
    std::memcpy(out->peer_key.data() + 1, bytes, 4U);
    out->peer_scope = bytes[0] == 127U ? AddressScope::loopback : AddressScope::remote;
    return;
  }
  if (address.ss_family != AF_INET6) {
    out->peer_scope = AddressScope::remote;
    return;
  }
  const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&address);
  unsigned char bytes[16];
  std::memcpy(bytes, &v6->sin6_addr, 16U);
  static constexpr unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
  static constexpr unsigned char kLoopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  if (std::memcmp(bytes, kMapped, 12U) == 0) {   // ::ffff:a.b.c.d is the IPv4 peer a.b.c.d
    out->peer_key[0] = 4;
    std::memcpy(out->peer_key.data() + 1, bytes + 12, 4U);
    out->peer_scope = bytes[12] == 127U ? AddressScope::loopback : AddressScope::remote;
    return;
  }
  out->peer_key[0] = 6;
  std::memcpy(out->peer_key.data() + 1, bytes, 16U);
  out->peer_scope = std::memcmp(bytes, kLoopback, 16U) == 0 ? AddressScope::loopback : AddressScope::remote;
}

bool would_block(const int code) noexcept {
#if defined(_WIN32)
  return code == WSAEWOULDBLOCK;
#else
  return code == EAGAIN || code == EWOULDBLOCK;
#endif
}

}  // namespace

struct HttpSocketServer::State final {
  std::atomic<NativeSocket> socket{kInvalidNativeSocket};
  std::uint16_t port{0};
  std::atomic<bool> listener_failed{false};
  // Set by shutdown_listener(); serve_one() hands it to serve_connection as
  // WorkerSignals::shutting_down, so an in-flight P1/P3 ends within one 20 ms
  // slice (pre-flight P34, SP2 C4).
  std::atomic<bool> stopped{false};
  ~State() { close_native_socket(socket.exchange(kInvalidNativeSocket)); }
};

HttpSocketServer::HttpSocketServer(std::unique_ptr<State> state) : state_(std::move(state)) {}
HttpSocketServer::HttpSocketServer(HttpSocketServer&&) noexcept = default;
HttpSocketServer& HttpSocketServer::operator=(HttpSocketServer&&) noexcept = default;
HttpSocketServer::~HttpSocketServer() = default;

std::optional<HttpSocketServer> HttpSocketServer::bind(const Config& listener, const int backlog, std::string* error) {
  if (error != nullptr) error->clear();
  // Config is a public value type, so do not assume a caller obtained it via
  // validate_config. Re-apply the bind-related policy before opening a socket.
  // The policy check does not depend on the port value, so port 0 is checked as 1.
  const ConfigInput verification{.host = listener.host,
                                 .port = listener.port == 0U ? 1U : listener.port,
                                 .api_key = listener.has_api_key ? std::optional<std::string>{"present"} : std::nullopt,
                                 .allow_remote = listener.allow_remote,
                                 .allow_wildcard_bind = listener.allow_wildcard_bind,
                                 .allow_loopback_without_key = listener.loopback_without_key};
  const ConfigValidation verified = validate_config(verification);
  const auto* checked = std::get_if<Config>(&verified);
  if (checked == nullptr) {
    if (error != nullptr) *error = "invalid listener configuration";
    return std::nullopt;
  }
  sockaddr_storage address{};
  AddressLength size = 0;
  int family = AF_UNSPEC;
  auto* v4 = reinterpret_cast<sockaddr_in*>(&address);
  auto* v6 = reinterpret_cast<sockaddr_in6*>(&address);
  if (inet_pton(AF_INET, checked->host.c_str(), &v4->sin_addr) == 1) {
    family = AF_INET;
    v4->sin_family = AF_INET;
    v4->sin_port = htons(listener.port);
    size = static_cast<AddressLength>(sizeof(sockaddr_in));
  } else if (inet_pton(AF_INET6, checked->host.c_str(), &v6->sin6_addr) == 1) {
    family = AF_INET6;
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons(listener.port);
    size = static_cast<AddressLength>(sizeof(sockaddr_in6));
  } else {
    if (error != nullptr) *error = "listener host must be a numeric IP literal";
    return std::nullopt;
  }
  Socket socket{open_listener_socket(family)};
  if (!socket.valid()) {
    if (error != nullptr) *error = "listener socket setup failed";
    return std::nullopt;
  }
  const int one = 1;
  // Quick restarts must not fail on TIME_WAIT (POSIX); on Windows the
  // equivalent protection is exclusive use so no other socket can hijack the port.
#if defined(_WIN32)
  (void)setsockopt(socket.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof(one));
#else
  (void)setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
  // An IPv6 listener serves IPv6 only; IPv4 needs its own explicit bind.
  if (family == AF_INET6 &&
      setsockopt(socket.get(), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&one), sizeof(one)) != 0) {
    if (error != nullptr) *error = "listener socket setup failed";
    return std::nullopt;
  }
  // The listener is non-blocking and polled in slices (accept_next).
  if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), size) != 0 ||
      ::listen(socket.get(), std::clamp(backlog, 1, 4096)) != 0 || !set_non_blocking(socket.get())) {
    if (error != nullptr) *error = "listener socket setup failed";
    return std::nullopt;
  }
  sockaddr_storage bound{};
  AddressLength bound_size = sizeof(bound);
  if (getsockname(socket.get(), reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
    if (error != nullptr) *error = "listener socket setup failed";
    return std::nullopt;
  }
  auto state = std::make_unique<State>();
  state->port = ntohs(family == AF_INET ? reinterpret_cast<const sockaddr_in*>(&bound)->sin_port
                                        : reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port);
  state->socket = socket.release();
  return HttpSocketServer{std::move(state)};
}

HttpSocketServer::AcceptStatus HttpSocketServer::accept_next(const std::chrono::milliseconds slice, Accepted* out) {
  if (!state_ || state_->stopped.load()) return AcceptStatus::stopped;
  const NativeSocket listener = state_->socket.load();
  PollEntry entry{listener, true, false};
  const int ready = poll_sockets(std::span<PollEntry>{&entry, 1U}, slice);
  if (state_->stopped.load()) return AcceptStatus::stopped;
  if (ready == 0) return AcceptStatus::timeout;
  sockaddr_storage peer{};
  AddressLength peer_size = sizeof(peer);
  const NativeSocket client = accept_client(listener, &peer, &peer_size);
  if (client == kInvalidNativeSocket) {
    const int code = detail::last_socket_error();
    // shutdown_listener() raced the poll: the handle is closed (or reused), not broken.
    if (state_->stopped.load()) return AcceptStatus::stopped;
    if (would_block(code)) return AcceptStatus::timeout;   // a client that went away before accept
    // Inverted on purpose: only errors that mean the listening socket itself
    // is broken stop serving; anything a peer or the network can cause backs off.
    const auto backoff = detail::accept_backoff_for(code);
    if (!backoff.has_value()) {
      state_->listener_failed = true;
      return AcceptStatus::listener_failed;
    }
    std::this_thread::sleep_for(*backoff);
    return AcceptStatus::transient;
  }
  out->socket = Socket{client};
  set_no_delay(client);
#if defined(SO_NOSIGPIPE)
  {
    const int one = 1;
    (void)setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  }
#endif
  classify_peer(peer, out);
  out->accepted_at = std::chrono::steady_clock::now();
  return AcceptStatus::accepted;
}

bool HttpSocketServer::serve_one(RequestRouter& router, const ConnectionLimits& limits, std::string* error) {
  if (error != nullptr) error->clear();
  Accepted accepted;
  for (;;) {
    const AcceptStatus status = accept_next(api_constants::kAcceptPollSlice, &accepted);
    if (status == AcceptStatus::accepted) break;
    if (status == AcceptStatus::listener_failed) { if (error != nullptr) *error = "accept failed"; return false; }
    if (status == AcceptStatus::stopped) { if (error != nullptr) *error = "listener stopped"; return false; }
    if (status == AcceptStatus::transient) { if (error != nullptr) *error = "accept interrupted"; return false; }
  }
  // Pre-flight P34: the stop flag is the worker's shutting_down signal.
  WorkerSignals signals;
  signals.shutting_down = &state_->stopped;
  const ConnectionResult result = serve_connection(accepted.socket.get(), router, limits, signals, accepted.accepted_at);
  if (result.outcome == ConnectionOutcome::responded) return true;
  if (error != nullptr) *error = result.outcome == ConnectionOutcome::shutdown ? "listener stopped" : "client request not completed";
  return false;
}

std::uint16_t HttpSocketServer::port() const { return state_ ? state_->port : 0U; }
bool HttpSocketServer::listener_failed() const noexcept { return state_ && state_->listener_failed.load(); }

void HttpSocketServer::shutdown_listener() noexcept {
  if (!state_) return;
  state_->stopped = true;   // first: a racing accept_next sees it before it could use a closed handle
  const NativeSocket socket = state_->socket.exchange(kInvalidNativeSocket);
#if !defined(_WIN32)
  // Task 8b carry-in: close() alone does not reliably wake an in-progress poll
  // on the listener; shutdown(SHUT_RDWR) first so the accept slice ends at once.
  if (socket != kInvalidNativeSocket) ::shutdown(socket, SHUT_RDWR);
#endif
  close_native_socket(socket);
}

NativeSocket HttpSocketServer::native_handle() const noexcept {
  return state_ ? state_->socket.load() : kInvalidNativeSocket;
}

}  // namespace secure_frontend
