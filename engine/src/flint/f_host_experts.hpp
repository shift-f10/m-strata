#pragma once

#include <cstddef>
#include <optional>
#include <utility>

namespace fl {
struct HostExpertAttempt {
  void* host{};
  void* device{};
  bool pin_capacity_exhausted{false};
};
struct HostExpertAllocation {
  void* host{};
  void* device{};
  bool pageable{false};
};

// A pin-capacity failure can use CPU-readable storage only when decode has its
// CPU lane. Other CUDA failures remain fatal. Once selected, avoid further large
// pin attempts and leave room for the lane's small mapped coordination buffers.
template <class Pin, class Pageable>
std::optional<HostExpertAllocation> allocate_host_experts(std::size_t bytes, bool allow_pageable,
    bool& pageable_mode, Pin&& pin, Pageable&& allocate) {
  if (!pageable_mode) {
    const auto attempt = std::forward<Pin>(pin)(bytes);
    if (attempt.host) return HostExpertAllocation{attempt.host, attempt.device, false};
    if (!attempt.pin_capacity_exhausted) return std::nullopt;
  }
  if (!allow_pageable) return std::nullopt;
  void* host = std::forward<Pageable>(allocate)(bytes);
  if (!host) return std::nullopt;
  pageable_mode = true;
  return HostExpertAllocation{host, nullptr, true};
}

// Unmapped expert addresses must never reach the direct mapped-memory prompt
// kernel. Short tails (or a disabled stream) use the existing CPU-lane decode path.
inline bool scalar_expert_prompt(bool pageable, bool streaming, int rows, int minimum_stream_rows) {
  return pageable && (!streaming || rows < minimum_stream_rows);
}
}  // namespace fl
