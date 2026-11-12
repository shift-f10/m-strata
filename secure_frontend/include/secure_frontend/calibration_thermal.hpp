#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace secure_frontend {
struct CalibrationGpuSignals {
  std::optional<bool> thermal_throttled{};
  // All active compute processes on selected GPUs, not necessarily competitors.
  // Only interpret as competing activity when the calibration engine is stopped.
  std::optional<std::size_t> compute_processes{};
  std::vector<std::string> notes{};
};
// Snapshot only: an inactive reading does not rule out throttling between samples.
// Empty selection samples all GPUs. One total deadline covers the fixed queries;
// teardown may add the bounded EngineStdioProcess termination grace.
[[nodiscard]] CalibrationGpuSignals sample_calibration_gpu(
    std::span<const unsigned> selected_gpus = {}, std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});
// Dependency-injection seam for a trusted executable, used by synthetic tests.
// Arguments, environment, working directory and process restrictions stay fixed.
[[nodiscard]] CalibrationGpuSignals sample_calibration_gpu_with_tool(
    const std::filesystem::path& tool, std::span<const unsigned> selected_gpus,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});
}  // namespace secure_frontend
