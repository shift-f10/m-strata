#pragma once

#include "secure_frontend/request_router.hpp"
#include "secure_frontend/stream_socket.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

struct StreamSinkTimeouts final {
  std::chrono::milliseconds write_stall{std::chrono::seconds(10)};
  std::function<bool()> shutting_down{};  // R-H: finish() stops waiting kShutdownDrainBudget after shutdown is seen
};

// The chunked SSE sink (spec §2.7, §3.2): one output buffer holding at most
// one head or one event plus framing; every send is a non-blocking step. The
// stall clock starts when pending output appears and restarts on every byte
// of progress; no progress for write_stall fails the sink.
class ChunkedStreamSink final : public StreamSink {
 public:
  using Clock = std::chrono::steady_clock;
  ChunkedStreamSink(StreamSocket& socket, StreamSinkTimeouts timeouts, std::optional<std::string> allow_origin = std::nullopt,
                    std::function<Clock::time_point()> now = &Clock::now);
  bool send_headers(const HttpHead& head) override;  // adds the request's CORS headers (M1 rule), buffers the head, tries to send
  bool send_event(std::string_view event) override;  // frames one chunk, tries to send; false once failed
  bool writable(std::chrono::milliseconds wait) override;  // flushes up to `wait`; true when nothing is pending and not failed
  bool client_gone() override;                             // failed (send error or stall) or the peer probe says gone
  void finish() override;                                  // terminal chunk, flushed under the stall rule
  [[nodiscard]] bool failed() const noexcept;
  [[nodiscard]] bool stalled() const noexcept;
  [[nodiscard]] std::size_t pending_bytes() const noexcept;
  void set_log_code(std::string_view code) override;
  // The accessors M1's deliver_route_result reads from its sink:
  [[nodiscard]] int status() const noexcept;  // 0 until the head was queued
  [[nodiscard]] std::size_t bytes() const noexcept;   // bytes actually sent
  [[nodiscard]] bool finished() const noexcept;       // the terminal chunk was sent completely
  [[nodiscard]] std::string_view log_code() const noexcept;

 private:
  bool flush(std::chrono::milliseconds wait);
  void append(std::string bytes);
  StreamSocket& socket_;
  StreamSinkTimeouts timeouts_;
  std::function<Clock::time_point()> now_;
  std::string outbound_;
  std::size_t offset_{0};
  Clock::time_point last_progress_{};
  bool failed_{false};
  bool stalled_{false};
  bool headers_sent_{false};
  std::size_t bytes_sent_{0};
  std::optional<std::string> allow_origin_;
  int status_{0};
  bool finished_{false};
  std::string_view log_code_{};
};

// 200; text/event-stream; chunked; proxy buffering off. Cache-Control,
// Connection: close and the §4.6 headers come from serialize_head.
[[nodiscard]] HttpHead make_sse_head();

}  // namespace secure_frontend
