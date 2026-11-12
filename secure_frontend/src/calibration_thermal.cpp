#include "secure_frontend/calibration_thermal.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/engine_stdio_process.hpp"
#include "secure_frontend/host_readings.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <set>
#include <thread>

namespace secure_frontend {
namespace {
constexpr std::size_t kOutputLimit = 16384U;
constexpr std::string_view kThermalQuery =
    "--query-gpu=index,clocks_event_reasons.sw_thermal_slowdown,clocks_event_reasons.hw_thermal_slowdown";
constexpr std::string_view kComputeQuery = "--query-compute-apps=pid";
constexpr std::string_view kFormat = "--format=csv,noheader,nounits";
using Clock = std::chrono::steady_clock;

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1U);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1U);
  return text;
}
std::vector<std::string_view> fields(const std::string_view line) {
  std::vector<std::string_view> result;
  std::size_t start = 0;
  while (true) {
    const auto comma = line.find(',', start);
    result.push_back(trim(line.substr(start, comma == std::string_view::npos ? comma : comma - start)));
    if (comma == std::string_view::npos) return result;
    if (result.size() > 3U) return {};
    start = comma + 1U;
  }
}
template <typename Number>
bool unsigned_number(const std::string_view text, Number& result) {
  if (text.empty() || text.front() < '0' || text.front() > '9') return false;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
struct Flag { bool valid; std::optional<bool> active; };
Flag thermal_flag(const std::string_view value) {
  if (value == "Active") return {true, true};
  if (value == "Not Active") return {true, false};
  if (value == "N/A") return {true, std::nullopt};
  if (value.size() >= 3U && value.size() <= 64U && value.front() == '[' && value.back() == ']' &&
      std::all_of(value.begin() + 1, value.end() - 1, [](const unsigned char c) {
        return c >= 0x20U && c <= 0x7eU && c != '[' && c != ']';
      })) return {true, std::nullopt};
  return {false, std::nullopt};
}
std::optional<std::vector<std::string>> query(const CanonicalFile& tool, const std::string_view query_name,
                                            const std::optional<unsigned> gpu, const Clock::time_point deadline) {
  if (Clock::now() >= deadline) return std::nullopt;
  EngineCommand command{.executable = tool.path(), .arguments = {std::string{query_name}, std::string{kFormat}},
                        .environment = nvidia_smi_environment(), .working_directory = tool.path().parent_path(),
                        .no_new_privileges = true, .private_files = true, .insert_serve = false, .max_read_line_bytes = 512U};
  if (gpu) command.arguments.push_back("--id=" + std::to_string(*gpu));
  EngineStdioProcess child;
  if (child.start(command)) return std::nullopt;
  std::vector<std::string> lines;
  std::size_t total = 0;
  bool closed = false;
  while (Clock::now() < deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    const auto line = child.read_line(std::max(std::chrono::milliseconds{1}, std::min(std::chrono::milliseconds{20}, remaining)));
    if (line.kind == EngineReadKind::line) {
      if (line.line.size() + 1U > kOutputLimit - total || lines.size() >= 4096U) break;
      total += line.line.size() + 1U;
      lines.push_back(line.line);
    } else if (line.kind == EngineReadKind::closed) { closed = true; break; }
    else if (line.kind == EngineReadKind::failure) break;
  }
  std::optional<ExitStatus> status;
  if (closed) {
    while (!(status = child.try_reap()) && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  const bool clean = closed && status && !status->signaled && status->value == 0U;
  const bool stopped = child.terminate(std::chrono::milliseconds{0});
  return clean && stopped ? std::optional{std::move(lines)} : std::nullopt;
}
std::optional<bool> thermal_reading(const std::vector<std::string>& lines, const std::span<const unsigned> selected) {
  std::set<unsigned> seen;
  bool any_active = false, any_unknown = false, found = false;
  for (const auto& line : lines) {
    if (trim(line).empty()) continue;
    const auto row = fields(line);
    unsigned index = 0;
    if (row.size() != 3U || !unsigned_number(row[0], index) || index > 255U || !seen.insert(index).second) return std::nullopt;
    const auto software = thermal_flag(row[1]), hardware = thermal_flag(row[2]);
    if (!software.valid || !hardware.valid) return std::nullopt;
    if (!selected.empty() && std::find(selected.begin(), selected.end(), index) == selected.end()) continue;
    found = true;
    any_active = any_active || software.active == true || hardware.active == true;
    any_unknown = any_unknown || !software.active || !hardware.active;
  }
  for (const auto index : selected) if (!seen.contains(index)) return std::nullopt;
  if (!found) return std::nullopt;
  if (any_active) return true;
  return any_unknown ? std::nullopt : std::optional{false};
}
bool compute_reading(const std::vector<std::string>& lines, std::set<std::uint32_t>& all) {
  std::set<std::uint32_t> seen;
  for (const auto& line : lines) {
    const auto text = trim(line);
    if (text.empty()) continue;
    std::uint32_t pid = 0;
    if (!unsigned_number(text, pid) || pid == 0U || !seen.insert(pid).second) return false;
  }
  all.insert(seen.begin(), seen.end());
  return true;
}
}  // namespace

CalibrationGpuSignals sample_calibration_gpu(const std::span<const unsigned> selected, const std::chrono::milliseconds timeout) {
  return sample_calibration_gpu_with_tool(nvidia_smi_path(), selected, timeout);
}
CalibrationGpuSignals sample_calibration_gpu_with_tool(const std::filesystem::path& path, const std::span<const unsigned> selected,
                                                       const std::chrono::milliseconds timeout) {
  CalibrationGpuSignals result;
  const std::set<unsigned> indices(selected.begin(), selected.end());
  if (selected.size() > 16U || indices.size() != selected.size() ||
      std::any_of(selected.begin(), selected.end(), [](const unsigned gpu) { return gpu > 255U; }) || timeout.count() <= 0) {
    result.notes.emplace_back("GPU telemetry unavailable: invalid selected devices or time budget.");
    return result;
  }
  auto trusted = open_executable(path, ExecutableWriters::also_trusted_installer);
  const auto* tool = std::get_if<CanonicalFile>(&trusted);
  if (!tool) { result.notes.emplace_back("GPU telemetry unavailable: the trusted NVIDIA tool is missing or inaccessible."); return result; }
  const auto deadline = Clock::now() + std::min(timeout, std::chrono::milliseconds{10000});
  if (const auto lines = query(*tool, kThermalQuery, std::nullopt, deadline)) result.thermal_throttled = thermal_reading(*lines, selected);
  if (!result.thermal_throttled) result.notes.emplace_back("Thermal slowdown telemetry unavailable; temperature alone is not evidence of throttling.");
#if defined(_WIN32)
  // WDDM's compute-apps list also includes desktop graphics contexts. Presence
  // in that list alone cannot establish competing computation.
  const auto models = query(*tool, "--query-gpu=index,driver_model.current", std::nullopt, deadline);
  std::set<unsigned> seen_models;
  bool compute_reliable = models.has_value();
  if (models) for (const auto& line : *models) {
    const auto row = fields(line);
    unsigned index = 0;
    if (row.size() != 2U || !unsigned_number(row[0], index) || index > 255U || !seen_models.insert(index).second) {
      compute_reliable = false; break;
    }
    if ((selected.empty() || indices.contains(index)) && row[1] != "TCC") compute_reliable = false;
  }
  if (seen_models.empty()) compute_reliable = false;
  for (const auto index : selected) if (!seen_models.contains(index)) compute_reliable = false;
  if (!compute_reliable) {
    result.notes.emplace_back("Competing GPU computation unavailable: Windows graphics contexts cannot be distinguished reliably from compute processes.");
    return result;
  }
#endif
  std::set<std::uint32_t> processes;
  bool compute_valid = true;
  if (selected.empty()) {
    const auto lines = query(*tool, kComputeQuery, std::nullopt, deadline);
    compute_valid = lines && compute_reading(*lines, processes);
  } else {
    for (const auto gpu : selected) {
      const auto lines = query(*tool, kComputeQuery, gpu, deadline);
      if (!lines || !compute_reading(*lines, processes)) { compute_valid = false; break; }
    }
  }
  if (compute_valid) result.compute_processes = processes.size();
  else result.notes.emplace_back("Active compute-process telemetry unavailable.");
  return result;
}
}  // namespace secure_frontend
