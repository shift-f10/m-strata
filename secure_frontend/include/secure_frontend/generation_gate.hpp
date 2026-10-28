#pragma once

#include "secure_frontend/engine_supervisor.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <variant>

namespace secure_frontend {

// Serializes SP3's explicit calls into SP2 (spec §2.5, D-1). ~Lease and
// EngineSupervisor::shutdown() do not pass through here; they rely on SP2's
// own mutex.
class SupervisorPort final {
 public:
  explicit SupervisorPort(EngineSupervisor& supervisor) noexcept : supervisor_(supervisor) {}
  [[nodiscard]] HealthSnapshot snapshot();
  [[nodiscard]] std::optional<Lease> acquire();
  void report_fatal(const Lease& lease, KillReason reason);
  // SP5 M1 Task 6 (parity rows lazy_load / idle_unload_s): request bracketing for the
  // idle window, and the wake a cold engine needs before it can hand out a Lease.
  void note_request_started();
  void note_request_finished();
  void wake_for_request();

 private:
  EngineSupervisor& supervisor_;
  std::mutex mutex_;
};

enum class GateRefusal { busy, timeout, cancelled, engine_unavailable, shutting_down };

struct GateLimits final {
  std::uint32_t max_queued;
  std::chrono::milliseconds queue_timeout;
};

// FIFO of tickets owning the single running slot (spec §2.5).
class GenerationGate final {
 public:
  using Clock = std::chrono::steady_clock;
  explicit GenerationGate(GateLimits limits, std::function<Clock::time_point()> now = &Clock::now);
  [[nodiscard]] std::variant<Lease, GateRefusal> admit(SupervisorPort& port, Clock::time_point deadline,
                                                       const std::function<bool()>& client_gone);
  // Called by the holder after its Lease was destroyed; wakes the next head.
  void release() noexcept;
  void shutdown() noexcept;
  [[nodiscard]] std::size_t waiting() const;

 private:
  void leave(std::uint64_t ticket);
  GateLimits limits_;
  std::function<Clock::time_point()> now_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<std::uint64_t> queue_;
  bool running_{false};
  bool shutdown_{false};
  std::uint64_t next_ticket_{0};
};

}  // namespace secure_frontend
