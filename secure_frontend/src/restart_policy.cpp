#include "secure_frontend/restart_policy.hpp"

#include <algorithm>
#include <limits>

namespace secure_frontend {

void RestartTracker::note_healthy(const std::chrono::milliseconds healthy_for) noexcept {
  if (healthy_for >= policy_.healthy_window) failures_ = 0U;
}

RestartDecision RestartTracker::record_failure(const std::chrono::milliseconds healthy_for) noexcept {
  note_healthy(healthy_for);
  if (failures_ < std::numeric_limits<std::uint32_t>::max()) ++failures_;
  const std::uint32_t limit = std::max<std::uint32_t>(policy_.max_consecutive_failures, 1U);
  if (failures_ >= limit) return {true, std::chrono::milliseconds::zero(), failures_};
  const auto factor = static_cast<std::chrono::milliseconds::rep>(std::max<std::uint32_t>(policy_.backoff_factor, 1U));
  std::chrono::milliseconds delay = std::min(policy_.backoff_initial, policy_.backoff_max);
  for (std::uint32_t step = 1U; step < failures_; ++step) {
    if (delay.count() > policy_.backoff_max.count() / factor) {
      delay = policy_.backoff_max;
      break;
    }
    delay *= factor;
  }
  return {false, std::min(delay, policy_.backoff_max), failures_};
}

}  // namespace secure_frontend
