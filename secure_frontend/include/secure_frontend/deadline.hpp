#pragma once

#include <algorithm>
#include <chrono>

namespace secure_frontend {

using MonotonicClock = std::chrono::steady_clock;

// The one deadline rule (SP2 K1) used by EngineProtocolClient::await_ready,
// EngineProtocolClient::read_generation and EngineStdioProcess::read_line:
// a deadline is `now + timeout` with a negative timeout treated as zero, and
// the remaining budget is never negative. A zero budget means "already
// expired": the call returns a timeout without reading or waiting, even when
// a complete line is already buffered. SP1 rejects 0 in configuration; this
// rule only decides what a 0 that slips through does.
[[nodiscard]] inline MonotonicClock::time_point deadline_after(const std::chrono::milliseconds timeout,
                                                                const MonotonicClock::time_point now = MonotonicClock::now()) noexcept {
  return now + std::max(timeout, std::chrono::milliseconds::zero());
}

[[nodiscard]] inline std::chrono::milliseconds remaining_until(const MonotonicClock::time_point deadline,
                                                                const MonotonicClock::time_point now = MonotonicClock::now()) noexcept {
  if (now >= deadline) return std::chrono::milliseconds::zero();
  return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
}

}  // namespace secure_frontend
