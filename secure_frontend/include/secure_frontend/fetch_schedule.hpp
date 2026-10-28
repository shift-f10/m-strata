#pragma once

#include "secure_frontend/acquire_reason.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace secure_frontend {

inline constexpr std::array<std::chrono::seconds, 5> kRetryBackoff{
    std::chrono::seconds{2}, std::chrono::seconds{4}, std::chrono::seconds{8}, std::chrono::seconds{16},
    std::chrono::seconds{32}};
inline constexpr std::uint32_t kAttemptsPerSource = 5;

// Retried on the same source (§8.2 "retried" or "file restarts from 0").
[[nodiscard]] bool is_transient(AcquireReason reason) noexcept;

// Spec §3.6: up to 5 attempts per source (2/4/8/16 s between them), 32 s
// before moving on after the fifth transient failure; a non-transient reason
// moves on at once. The run's class is the last source's reason.
class SourceSchedule final {
 public:
  explicit SourceSchedule(std::size_t source_count) noexcept : count_(source_count) {}
  struct Step final {
    enum class Kind { retry_same_source, next_source, exhausted } kind;
    std::chrono::seconds wait;
    std::size_t source;
  };
  [[nodiscard]] Step record_failure(AcquireReason reason, bool transient) noexcept;
  [[nodiscard]] std::size_t source() const noexcept { return source_; }
  [[nodiscard]] std::uint32_t attempt() const noexcept { return attempt_; }
  [[nodiscard]] std::optional<AcquireReason> last_reason() const noexcept { return last_; }

 private:
  std::size_t count_;
  std::size_t source_{0};
  std::uint32_t attempt_{1};
  std::optional<AcquireReason> last_;
};

// pull.max_duration_s for the whole run, passed to curl as the remaining --max-time.
class RunBudget final {
 public:
  RunBudget(std::chrono::steady_clock::time_point start, std::chrono::seconds total) noexcept
      : deadline_(start + total) {}
  [[nodiscard]] std::optional<std::uint64_t> remaining_seconds(std::chrono::steady_clock::time_point now) const noexcept;

 private:
  std::chrono::steady_clock::time_point deadline_;
};

// Clock seam (§10.1): tests advance time and record sleeps.
class AcquireClock {
 public:
  virtual ~AcquireClock() = default;
  [[nodiscard]] virtual std::chrono::steady_clock::time_point now() = 0;
  virtual void sleep_for(std::chrono::seconds duration) = 0;
};

class SystemClock final : public AcquireClock {
 public:
  [[nodiscard]] std::chrono::steady_clock::time_point now() override;
  void sleep_for(std::chrono::seconds duration) override;
};

}  // namespace secure_frontend
