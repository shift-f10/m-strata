#pragma once

#include "secure_frontend/config_defaults.hpp"

#include <chrono>
#include <cstdint>

namespace secure_frontend {

// Spec §3.3 restart policy. Defaults come from config_defaults.hpp;
// supervisor_config() (subsystem_limits.hpp) sets ready_timeout from the config.
struct RestartPolicy final {
  std::uint32_t max_consecutive_failures{config_defaults::kMaxConsecutiveFailures};
  std::chrono::milliseconds backoff_initial{config_defaults::kBackoffInitial};
  std::uint32_t backoff_factor{config_defaults::kBackoffFactor};
  std::chrono::milliseconds backoff_max{config_defaults::kBackoffMax};
  std::chrono::milliseconds healthy_window{config_defaults::kHealthyWindow};
  std::chrono::milliseconds ready_timeout{std::chrono::seconds(config_defaults::kEngineReadyTimeoutS)};
  std::chrono::milliseconds reap_timeout{config_defaults::kReapTimeout};
};

struct RestartDecision final {
  bool circuit_open{false};
  std::chrono::milliseconds delay{0};
  std::uint32_t consecutive_failures{0};
};

// Spec §3.3: a failure is any unrequested end, every §3.5 kill and any spawn
// failure. The supervisor does not record KillReason::client_stop_drain_failed
// (SP3 D-2). Delays grow initial * factor^(n-1), capped at backoff_max; the
// max_consecutive_failures-th failure opens the circuit for good.
class RestartTracker final {
 public:
  explicit RestartTracker(RestartPolicy policy) noexcept : policy_(policy) {}
  // `healthy_for` is the ended generation's continuous ready/generating time
  // (zero for spawn failures and generations that never became ready);
  // reaching healthy_window first resets the count to 0.
  [[nodiscard]] RestartDecision record_failure(std::chrono::milliseconds healthy_for) noexcept;
  // An end that is not counted (SP3 D-2) still applies the healthy-window
  // reset: `healthy_for` >= healthy_window resets the count to 0; nothing is added.
  void note_healthy(std::chrono::milliseconds healthy_for) noexcept;
  [[nodiscard]] std::uint32_t consecutive_failures() const noexcept { return failures_; }

 private:
  RestartPolicy policy_;
  std::uint32_t failures_{0};
};

}  // namespace secure_frontend
