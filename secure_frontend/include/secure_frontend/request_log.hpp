#pragma once
// SP3 Task 21 (spec §8): the request log is telemetry-free — fixed fields, no
// token, prompt, path or client payload — rate-limited by a token bucket, and
// OFF unless logging.requests is set.
#include "secure_frontend/connection_pool.hpp"
#include "secure_frontend/request_router.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace secure_frontend {

struct RequestLogRecord final {
  RouteId route;
  int status;
  std::string_view code;      // the §7.2 code, or a log-only outcome; empty prints "-"
  std::chrono::milliseconds elapsed;
  std::size_t bytes_out;
  AddressScope peer;
  bool streamed{false};       // an SSE stream: the route prints as chat_stream (M2)
};

[[nodiscard]] std::string format_request_line(std::uint64_t sequence, const RequestLogRecord& record);

// Maps a pool connection to the §8 line (log-only outcomes: generation_timeout,
// client_gone, write_stall, evicted).
[[nodiscard]] RequestLogRecord log_record_for(const FinishedConnection& finished);

class RequestLogger final {
 public:
  using Clock = std::chrono::steady_clock;
  using Sink = std::function<void(std::string_view line)>;
  RequestLogger(bool enabled, Sink sink, std::function<Clock::time_point()> now = &Clock::now);
  void record(const RequestLogRecord& record);   // thread-safe; never blocks beyond its mutex

 private:
  bool enabled_;
  Sink sink_;
  std::function<Clock::time_point()> now_;
  std::mutex mutex_;
  double tokens_;
  Clock::time_point last_refill_;
  Clock::time_point last_report_;
  std::uint64_t sequence_{0};
  std::uint64_t dropped_{0};
};

[[nodiscard]] RequestLogger::Sink stderr_sink();   // fixed-size buffer, one fwrite per line, failures dropped

}  // namespace secure_frontend
