#pragma once

#include <cstdint>
#include <optional>

namespace secure_frontend {

// SP5 M1 Task 4 (parity row `sampling`): the sampling keys the engine's GEN
// line carries; render_sampling_keys applies the pinned v0.2.2 wire rules of
// serve/server.py:1367-1401 (conditional omission, top_k clamp, the
// penalty_repeat/freq/present aliases, the penalty_last_n default of 64).
// Absent = the engine default.
struct SamplingParams final {
  std::optional<double> temperature;            // 0..2
  std::optional<double> top_p;                  // (0, 1]
  std::optional<std::uint64_t> seed;            // 0..2^63-1
  std::optional<std::int64_t> top_k;            // 1..64
  std::optional<double> min_p;                  // [0, 1]
  std::optional<double> presence_penalty;       // [0, 2] (cap added, C-1)
  std::optional<double> repetition_penalty;     // (0, 2] (cap added, C-1)
  std::optional<double> frequency_penalty;      // [0, 2] (cap added, C-1)
  std::optional<std::int64_t> penalty_last_n;   // 0..8192 (cap added, C-1)
  [[nodiscard]] bool empty() const noexcept {
    return !temperature && !top_p && !seed && !top_k && !min_p && !presence_penalty && !repetition_penalty &&
           !frequency_penalty && !penalty_last_n;
  }
};

// SP5 M1 Task 4 (parity row `sampling`): the defaults.sampling.* config rows.
// Unset rows stay unset (never sent); kinds and bounds per the pin
// (clean_shared_defaults serve/server.py:7331-7367; wire conditions
// :1360-1394); the 2.0 / 8192 / 32-bit caps are added security bounds (C-1).
struct SamplingDefaults final {
  std::optional<double> temperature;
  std::optional<double> top_p;
  std::optional<double> min_p;
  std::optional<double> presence_penalty;
  std::optional<double> repetition_penalty;
  std::optional<double> frequency_penalty;
  std::optional<std::int64_t> top_k;
  std::optional<std::int64_t> penalty_last_n;
  std::optional<std::int64_t> seed;
};

}  // namespace secure_frontend
