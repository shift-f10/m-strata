#include "secure_frontend/supervisor_wake.hpp"

#include <algorithm>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace secure_frontend {

SupervisorWake::SupervisorWake() {
#if defined(_WIN32)
  event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
#else
  int fds[2]{-1, -1};
  if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
    read_fd_ = fds[0];
    write_fd_ = fds[1];
  }
#endif
}

SupervisorWake::~SupervisorWake() {
#if defined(_WIN32)
  if (event_ != nullptr) CloseHandle(static_cast<HANDLE>(event_));
#else
  if (read_fd_ >= 0) (void)close(read_fd_);
  if (write_fd_ >= 0) (void)close(write_fd_);
#endif
}

bool SupervisorWake::valid() const noexcept {
#if defined(_WIN32)
  return event_ != nullptr;
#else
  return read_fd_ >= 0 && write_fd_ >= 0;
#endif
}

void SupervisorWake::notify() noexcept {
#if defined(_WIN32)
  if (event_ != nullptr) (void)SetEvent(static_cast<HANDLE>(event_));
#else
  // EAGAIN on a full pipe is fine: the pipe is already readable.
  if (write_fd_ >= 0) {
    const char byte = 'w';
    (void)!write(write_fd_, &byte, 1);
  }
#endif
}

void SupervisorWake::drain() noexcept {
#if defined(_WIN32)
  if (event_ != nullptr) (void)ResetEvent(static_cast<HANDLE>(event_));
#else
  char buffer[64];
  while (read_fd_ >= 0 && read(read_fd_, buffer, sizeof(buffer)) > 0) {
  }
#endif
}

SupervisorWake::Event SupervisorWake::wait(const std::intptr_t monitored, const std::chrono::milliseconds timeout) noexcept {
#if defined(_WIN32)
  HANDLE handles[2]{static_cast<HANDLE>(event_), reinterpret_cast<HANDLE>(monitored)};
  const DWORD count = monitored == kNoMonitorHandle ? 1U : 2U;
  const DWORD milliseconds = timeout.count() < 0 ? INFINITE : static_cast<DWORD>(std::min<std::int64_t>(timeout.count(), 0xFFFFFFFE));
  const DWORD result = WaitForMultipleObjects(count, handles, FALSE, milliseconds);
  if (result == WAIT_OBJECT_0) {
    drain();
    return Event::woken;
  }
  if (result == WAIT_OBJECT_0 + 1U) return Event::monitored;
  return Event::timeout;
#else
  pollfd fds[2]{{read_fd_, POLLIN, 0}, {static_cast<int>(monitored), POLLIN, 0}};
  const nfds_t count = monitored == kNoMonitorHandle ? 1U : 2U;
  const int milliseconds = timeout.count() < 0 ? -1 : static_cast<int>(std::min<std::int64_t>(timeout.count(), 0x7fffffff));
  const int ready = poll(fds, count, milliseconds);
  if (ready < 0) return errno == EINTR ? Event::woken : Event::timeout;
  if (ready == 0) return Event::timeout;
  if (fds[0].revents != 0) {
    drain();
    return Event::woken;
  }
  return Event::monitored;
#endif
}

}  // namespace secure_frontend
