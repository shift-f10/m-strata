#include "secure_frontend/socket_io.hpp"

#include "secure_frontend/api_constants.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>

#if !defined(_WIN32)
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace secure_frontend {
namespace {
using Clock = std::chrono::steady_clock;

// POSIX: MSG_DONTWAIT makes every send non-blocking even if the caller forgot
// set_non_blocking; MSG_NOSIGNAL keeps a reset peer from raising SIGPIPE (SP0 S1).
#if !defined(_WIN32)
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
constexpr int kSendFlags = MSG_DONTWAIT;
#endif
#endif

long native_send(const NativeSocket socket, const char* data, const std::size_t size) noexcept {
#if defined(_WIN32)
  return ::send(socket, data, static_cast<int>(std::min<std::size_t>(size, INT_MAX)), 0);
#else
  return static_cast<long>(::send(socket, data, size, kSendFlags));
#endif
}

bool would_block(const int code) noexcept {
#if defined(_WIN32)
  return code == WSAEWOULDBLOCK;
#else
  return code == EAGAIN || code == EWOULDBLOCK;
#endif
}

// One-entry poll: > 0 when the socket is ready (or reports an error, which the
// following send/recv then surfaces), 0 on timeout OR a POSIX EINTR, -1 on a
// poll error. A 0 is never proof that the timeout elapsed: callers re-check
// their own clock.
int wait_one(const NativeSocket socket, const bool read, const std::chrono::milliseconds timeout) noexcept {
  PollEntry entry{socket, read, !read};
  return poll_sockets(std::span<PollEntry>{&entry, 1U}, timeout);
}

std::chrono::milliseconds ceil_ms(const Clock::duration d) noexcept {
  return std::max(std::chrono::ceil<std::chrono::milliseconds>(d), std::chrono::milliseconds(1));
}

// recv(MSG_PEEK) of one byte without blocking; EINTR retried.
long peek_one(const NativeSocket socket) noexcept {
  char byte = 0;
  for (;;) {
#if defined(_WIN32)
    const long n = ::recv(socket, &byte, 1, MSG_PEEK);
#else
    const long n = static_cast<long>(::recv(socket, &byte, 1, MSG_PEEK | MSG_DONTWAIT));
#endif
    if (n < 0 && detail::last_socket_error() == detail::kInterruptedCode) continue;
    return n;
  }
}

}  // namespace

void close_native_socket(const NativeSocket socket) noexcept {
  if (socket == kInvalidNativeSocket) return;
#if defined(_WIN32)
  (void)closesocket(socket);
#else
  (void)::close(socket);
#endif
}

bool set_non_blocking(const NativeSocket socket) noexcept {
#if defined(_WIN32)
  u_long mode = 1;
  return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
  const int flags = fcntl(socket, F_GETFL, 0);
  return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void set_no_delay(const NativeSocket socket) noexcept {
  const int one = 1;
  (void)setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

bool set_no_inherit(const NativeSocket socket) noexcept {
#if defined(_WIN32)
  return SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0) != 0;
#else
  const int flags = fcntl(socket, F_GETFD, 0);
  return flags >= 0 && fcntl(socket, F_SETFD, flags | FD_CLOEXEC) == 0;
#endif
}

int poll_sockets(const std::span<PollEntry> entries, const std::chrono::milliseconds timeout) noexcept {
  constexpr std::size_t kMax = api_constants::kMaxPollEntries;
  if (entries.size() > kMax) return -1;
#if defined(_WIN32)
  std::array<WSAPOLLFD, kMax> fds{};
#else
  std::array<pollfd, kMax> fds{};
#endif
  for (std::size_t i = 0; i < entries.size(); ++i) {
    entries[i].readable = entries[i].writable = entries[i].failed = false;
    fds[i].fd = entries[i].socket;
    fds[i].events = static_cast<short>((entries[i].want_read ? POLLIN : 0) | (entries[i].want_write ? POLLOUT : 0));
    fds[i].revents = 0;
  }
  const int ms = static_cast<int>(std::clamp<long long>(timeout.count(), 0, INT_MAX));
#if defined(_WIN32)
  const int ready = WSAPoll(fds.data(), static_cast<ULONG>(entries.size()), ms);
#else
  int ready = ::poll(fds.data(), static_cast<nfds_t>(entries.size()), ms);
  if (ready < 0 && errno == EINTR) ready = 0;
#endif
  if (ready <= 0) return ready < 0 ? -1 : 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    entries[i].readable = (fds[i].revents & (POLLIN | POLLHUP)) != 0;
    entries[i].writable = (fds[i].revents & POLLOUT) != 0;
    entries[i].failed = (fds[i].revents & (POLLERR | POLLNVAL)) != 0;
  }
  return ready;
}

IoResult recv_some(const NativeSocket socket, char* buffer, const std::size_t size) noexcept {
  if (size == 0U) return {IoStatus::done, 0U};
  for (;;) {
#if defined(_WIN32)
    const long n = ::recv(socket, buffer, static_cast<int>(std::min<std::size_t>(size, INT_MAX)), 0);
#else
    const long n = static_cast<long>(::recv(socket, buffer, size, MSG_DONTWAIT));
#endif
    if (n > 0) return {IoStatus::done, static_cast<std::size_t>(n)};
    if (n == 0) return {IoStatus::closed};
    const int code = detail::last_socket_error();
    if (code == detail::kInterruptedCode) continue;
    return {would_block(code) ? IoStatus::would_block : IoStatus::failed};
  }
}

IoResult send_some(const NativeSocket socket, const std::string_view bytes) noexcept {
  return detail::send_some_with(&native_send, socket, bytes);
}

WriteOutcome send_all_bounded(const NativeSocket socket, std::string_view bytes, const std::chrono::milliseconds stall,
                              const Clock::time_point deadline, const std::atomic<bool>* stop) noexcept {
  // A non-positive `stall` reports `stalled` before the first send (callers pass
  // write_stall_ms, which the schema keeps >= 1). The stall clock restarts on
  // every send that moves at least one byte (spec §2.7, E4).
  auto last_progress = Clock::now();
  // Spec §2.8 (Task 12 carry-in): once `stop` is seen, the whole-write bound
  // tightens to kShutdownDrainBudget from that observation; a write already in
  // flight can never hold a worker past the shutdown drain budget.
  std::optional<Clock::time_point> drain_bound;
  while (!bytes.empty()) {
    const auto now = Clock::now();
    if (stop != nullptr && stop->load() && !drain_bound.has_value()) drain_bound = now + api_constants::kShutdownDrainBudget;
    const auto bound = drain_bound.has_value() && *drain_bound < deadline ? *drain_bound : deadline;
    if (now >= bound) return WriteOutcome::deadline;
    if (now - last_progress >= stall) return WriteOutcome::stalled;
    const IoResult result = send_some(socket, bytes);
    if (result.status == IoStatus::done) {
      bytes.remove_prefix(result.bytes);
      last_progress = Clock::now();
      continue;
    }
    if (result.status != IoStatus::would_block) return WriteOutcome::failed;
    // Wait for writability, never past the stall bound or the deadline.
    const auto wait = std::min({ceil_ms(bound - now), ceil_ms(last_progress + stall - now), api_constants::kStreamPollSlice});
    (void)wait_one(socket, false, wait);  // 0 (timeout or EINTR) loops back to the clock checks
  }
  return WriteOutcome::complete;
}

PeerState probe_peer(const NativeSocket socket) noexcept {
  const long n = peek_one(socket);
  if (n < 0 && would_block(detail::last_socket_error())) return PeerState::alive;
  return PeerState::gone;  // EOF (0), reset (<0) or a byte after the body (>0)
}

bool bytes_pending(const NativeSocket socket) noexcept { return peek_one(socket) > 0; }

void half_close_and_drain(const NativeSocket socket) noexcept {
#if defined(_WIN32)
  if (::shutdown(socket, SD_SEND) != 0) return;
#else
  if (::shutdown(socket, SHUT_WR) != 0) return;
#endif
  const auto deadline = Clock::now() + api_constants::kDrainBudget;
  std::array<char, api_constants::kRecvChunk> sink{};
  std::size_t drained = 0;
  while (drained < api_constants::kDrainBytes) {
    const auto now = Clock::now();
    if (now >= deadline) return;
    const int ready = wait_one(socket, true, ceil_ms(deadline - now));
    if (ready < 0) return;
    if (ready == 0) continue;  // timeout or EINTR: the clock check above decides
    const IoResult r = recv_some(socket, sink.data(), sink.size());
    if (r.status == IoStatus::would_block) continue;  // spurious readiness
    if (r.status != IoStatus::done) return;           // EOF or error
    drained += r.bytes;
  }
}

WinsockGuard::WinsockGuard() noexcept {
#if defined(_WIN32)
  WSADATA data{};
  ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
}

WinsockGuard::~WinsockGuard() {
#if defined(_WIN32)
  if (ok_) WSACleanup();
#endif
}

namespace detail {

IoResult send_some_with(const SendCall call, const NativeSocket socket, const std::string_view bytes) noexcept {
  if (bytes.empty()) return {IoStatus::done, 0U};
  const std::size_t size = std::min(bytes.size(), api_constants::kMaxSendChunk);
  for (;;) {
    const long n = call(socket, bytes.data(), size);
    if (n > 0) return {IoStatus::done, static_cast<std::size_t>(n)};
    if (n == 0) return {IoStatus::failed};
    const int code = last_socket_error();
    if (code == kInterruptedCode) continue;
    return {would_block(code) ? IoStatus::would_block : IoStatus::failed};
  }
}

int last_socket_error() noexcept {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}

void set_last_socket_error(const int code) noexcept {
#if defined(_WIN32)
  WSASetLastError(code);
#else
  errno = code;
#endif
}

std::optional<std::chrono::milliseconds> accept_backoff_for(const int code) noexcept {
#if defined(_WIN32)
  const bool fatal = code == WSAENOTSOCK || code == WSAEINVAL || code == WSANOTINITIALISED;
#else
  const bool fatal = code == EBADF || code == ENOTSOCK || code == EINVAL;
#endif
  if (fatal) return std::nullopt;
  return api_constants::kAcceptBackoff;
}

}  // namespace detail
}  // namespace secure_frontend
