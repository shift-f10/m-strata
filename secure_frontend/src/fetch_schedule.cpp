#include "secure_frontend/fetch_schedule.hpp"

#include <thread>

namespace secure_frontend {

bool is_transient(const AcquireReason reason) noexcept {
  const auto same = reason_info(reason).same_source;
  return same == SameSource::retried || same == SameSource::restart_file;
}

SourceSchedule::Step SourceSchedule::record_failure(const AcquireReason reason, const bool transient) noexcept {
  last_ = reason;
  if (transient && attempt_ < kAttemptsPerSource) {
    const auto wait = kRetryBackoff[attempt_ - 1U];
    ++attempt_;
    return Step{Step::Kind::retry_same_source, wait, source_};
  }
  if (source_ + 1U >= count_) return Step{Step::Kind::exhausted, std::chrono::seconds{0}, source_};
  ++source_;
  attempt_ = 1;
  return Step{Step::Kind::next_source, transient ? kRetryBackoff[4] : std::chrono::seconds{0}, source_};
}

std::optional<std::uint64_t> RunBudget::remaining_seconds(const std::chrono::steady_clock::time_point now) const noexcept {
  if (now >= deadline_) return std::nullopt;
  const auto left = std::chrono::ceil<std::chrono::seconds>(deadline_ - now).count();
  return static_cast<std::uint64_t>(left < 1 ? 1 : left);
}

std::chrono::steady_clock::time_point SystemClock::now() { return std::chrono::steady_clock::now(); }
void SystemClock::sleep_for(const std::chrono::seconds duration) { std::this_thread::sleep_for(duration); }

}  // namespace secure_frontend
