#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <cerrno>
#endif

// Polled, non-blocking socket I/O shared by the HTTP workers and the accept
// thread (SP3 spec §2.2, §2.4, §2.7, §10). Every function here is noexcept and
// never blocks longer than the bound it is given.
namespace secure_frontend {

#if defined(_WIN32)
using NativeSocket = SOCKET;
inline constexpr NativeSocket kInvalidNativeSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
inline constexpr NativeSocket kInvalidNativeSocket = -1;
#endif

void close_native_socket(NativeSocket socket) noexcept;

class Socket final {
 public:
  Socket() = default;
  explicit Socket(NativeSocket socket) noexcept : socket_(socket) {}
  Socket(Socket&& other) noexcept : socket_(other.release()) {}
  Socket& operator=(Socket&& other) noexcept {
    if (this != &other) {
      reset();
      socket_ = other.release();
    }
    return *this;
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  ~Socket() { reset(); }
  [[nodiscard]] NativeSocket get() const noexcept { return socket_; }
  [[nodiscard]] bool valid() const noexcept { return socket_ != kInvalidNativeSocket; }
  NativeSocket release() noexcept {
    const NativeSocket s = socket_;
    socket_ = kInvalidNativeSocket;
    return s;
  }
  void reset() noexcept { close_native_socket(release()); }

 private:
  NativeSocket socket_{kInvalidNativeSocket};
};

[[nodiscard]] bool set_non_blocking(NativeSocket socket) noexcept;
void set_no_delay(NativeSocket socket) noexcept;
// POSIX: FD_CLOEXEC; Windows: clears HANDLE_FLAG_INHERIT. For sockets that were
// not created close-on-exec / no-inherit already (pre-flight V12, Task 8b).
[[nodiscard]] bool set_no_inherit(NativeSocket socket) noexcept;

struct PollEntry final {
  NativeSocket socket;
  bool want_read;
  bool want_write;
  bool readable{false};
  bool writable{false};
  bool failed{false};
};
// poll()/WSAPoll(); returns the number of ready entries, 0 on timeout (or a
// POSIX EINTR), -1 on error or more than api_constants::kMaxPollEntries entries.
// The readiness flags are reset on every call.
int poll_sockets(std::span<PollEntry> entries, std::chrono::milliseconds timeout) noexcept;

enum class IoStatus { done, would_block, closed, failed };
struct IoResult final {
  IoStatus status;
  std::size_t bytes{0};
};
[[nodiscard]] IoResult recv_some(NativeSocket socket, char* buffer, std::size_t size) noexcept;
// At most api_constants::kMaxSendChunk bytes; EINTR is retried (SP0 gap fixed).
[[nodiscard]] IoResult send_some(NativeSocket socket, std::string_view bytes) noexcept;

enum class WriteOutcome { complete, stalled, deadline, failed };
// Spec §2.7 / E4: `stall` is measured on progress, never on call return;
// `deadline` bounds the whole write. Precondition: `socket` is non-blocking
// (set_non_blocking). Every send is sliced to kMaxSendChunk and waits for
// writability in kStreamPollSlice steps, on POSIX and Windows alike.
// Task 12 carry-in (spec §2.8): when `stop` is set, the write is cut within
// api_constants::kShutdownDrainBudget of the first observation, even if the
// whole-write deadline is later — an in-flight write can never outlive
// shutdown by more than the drain budget. `stop` may be null (never set).
[[nodiscard]] WriteOutcome send_all_bounded(NativeSocket socket, std::string_view bytes, std::chrono::milliseconds stall,
                                            std::chrono::steady_clock::time_point deadline,
                                            const std::atomic<bool>* stop = nullptr) noexcept;

enum class PeerState { alive, gone };
// recv(MSG_PEEK) on the non-blocking socket: EOF, reset or ANY byte -> gone (spec §2.6).
[[nodiscard]] PeerState probe_peer(NativeSocket socket) noexcept;
// One non-blocking MSG_PEEK: true when at least one unread byte is waiting (EOF and errors are false).
[[nodiscard]] bool bytes_pending(NativeSocket socket) noexcept;
// Half-close, then drain for kDrainBudget / kDrainBytes (SP0 S4), polled.
void half_close_and_drain(NativeSocket socket) noexcept;

// WSAStartup/WSACleanup once per process (spec §10); a no-op on POSIX.
class WinsockGuard final {
 public:
  WinsockGuard() noexcept;
  ~WinsockGuard();
  WinsockGuard(const WinsockGuard&) = delete;
  WinsockGuard& operator=(const WinsockGuard&) = delete;
  [[nodiscard]] bool ok() const noexcept { return ok_; }

 private:
  bool ok_{true};
};

namespace detail {
using SendCall = long (*)(NativeSocket, const char*, std::size_t) noexcept;
[[nodiscard]] IoResult send_some_with(SendCall call, NativeSocket socket, std::string_view bytes) noexcept;
[[nodiscard]] int last_socket_error() noexcept;
void set_last_socket_error(int code) noexcept;
#if defined(_WIN32)
inline constexpr int kInterruptedCode = WSAEINTR;
#else
inline constexpr int kInterruptedCode = EINTR;
#endif
// nullopt: the listening socket itself is broken; otherwise the backoff before the next accept.
[[nodiscard]] std::optional<std::chrono::milliseconds> accept_backoff_for(int error_code) noexcept;
}  // namespace detail

}  // namespace secure_frontend
