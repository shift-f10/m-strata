#pragma once

#include "secure_frontend/supervisor_wake.hpp"

#include <chrono>

namespace secure_frontend {

// Injectable time source and backoff timer, so PROC-04 policy tests are
// deterministic (spec §4.1 F3). Only the supervisor thread calls it.
class SupervisorClock {
 public:
  using time_point = std::chrono::steady_clock::time_point;
  virtual ~SupervisorClock() = default;
  [[nodiscard]] virtual time_point now() const = 0;
  // Waits out one backoff delay. Returns false when `wake` fired first (the
  // caller then re-checks for shutdown).
  [[nodiscard]] virtual bool sleep_backoff(std::chrono::milliseconds delay, SupervisorWake& wake) = 0;
};

class SteadySupervisorClock final : public SupervisorClock {
 public:
  [[nodiscard]] time_point now() const override { return std::chrono::steady_clock::now(); }
  [[nodiscard]] bool sleep_backoff(const std::chrono::milliseconds delay, SupervisorWake& wake) override {
    return wake.wait(kNoMonitorHandle, delay) == SupervisorWake::Event::timeout;
  }
};

}  // namespace secure_frontend
