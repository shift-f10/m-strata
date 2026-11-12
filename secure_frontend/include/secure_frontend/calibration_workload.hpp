#pragma once

#include "secure_frontend/calibration_experiment.hpp"
#include "secure_frontend/request_router.hpp"
#include "secure_frontend/serve_monitor.hpp"
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {
inline constexpr std::size_t kCalibrationPromptMaxBytes = 1U << 20U;
inline constexpr std::size_t kCalibrationResponseMaxBytes = 1U << 20U;
enum class CalibrationAcceptance { exact_text, json_value, tool_calls, structural_only, performance_text };
struct CalibrationWorkloadRequest final {
  std::string model;
  BenchmarkWorkload workload{BenchmarkWorkload::chat};
  std::size_t context_tokens{8192U};
  std::size_t max_prompt_tokens{8192U};
  std::size_t max_completion_tokens{256U};
  std::string custom_prompt;
  bool disable_thinking{false}; // Only when the selected profile accepts per-request reasoning.
};
struct CalibrationCase final {
  std::string id;
  std::string request_body;
  bool stream{false};
  bool measured{true}; // Known-answer probes gate quality without timing tiny outputs.
  CalibrationAcceptance acceptance{CalibrationAcceptance::exact_text};
  std::string expected;  // synthetic known answer only; never a generated answer
  std::size_t prompt_tokens{0};
  std::size_t target_prompt_tokens{0};
  double context_coverage{0.0};
  bool context_probe{false}; // Identifies context-fill cases even when coverage is partial.
  bool full_context_probe{false};
};
// Count the full rendered request using the selected profile, tokenizer and
// optional embedded template. nullopt means it cannot be rendered/admitted.
using CalibrationTokenCounter = std::function<std::optional<std::size_t>(std::string_view)>;
using CalibrationCases = std::variant<std::vector<CalibrationCase>, std::string>;
[[nodiscard]] CalibrationCases build_calibration_workload(const CalibrationWorkloadRequest& request,
                                                         const CalibrationTokenCounter& count);
[[nodiscard]] std::variant<CalibrationCase, std::string> build_calibration_memory_probe(
    const CalibrationWorkloadRequest& request, const CalibrationTokenCounter& count);
struct CalibrationQuality final {
  bool accepted{false};
  bool semantic_check{false}; // custom output can only receive structural checks
  std::string reason;        // fixed diagnostics, never an answer or custom prompt
};
[[nodiscard]] CalibrationQuality validate_calibration_response(const CalibrationCase& test, int status,
                                                              std::string_view body);
// Execute a RouteResult's StreamHandler into this sink. No network; bounded
// capture fails closed. A cancellation callback lets the controller stop it.
class CalibrationCapture final : public StreamSink {
 public:
  explicit CalibrationCapture(std::function<bool()> cancelled = {});
  bool send_headers(const HttpHead& head) override;
  bool send_event(std::string_view bytes) override;
  bool writable(std::chrono::milliseconds) override;
  bool client_gone() override;
  void finish() override;
  [[nodiscard]] int status() const noexcept { return status_; }
  [[nodiscard]] const std::string& body() const noexcept { return body_; }
  [[nodiscard]] bool complete() const noexcept { return finished_ && !failed_; }
 private:
  std::function<bool()> cancelled_;
  int status_{0};
  std::string body_;
  bool finished_{false};
  bool failed_{false};
};
struct CalibrationHostDiagnostics final {
  bool contaminated{false};
  std::vector<std::string> flags;
  std::optional<double> ram_headroom_percent;
  std::optional<double> gpu_headroom_percent;
};
// Call after every previous engine has stopped/reaped. The second distinct
// post-call host sample excludes the prior trial's CPU accounting interval.
// GPU readings are omitted: the monitor has no GPU acquisition timestamp.
// Independent fresh GPU-process/thermal queries remain the controller's job.
// At most three seconds; missing readings/cancellation return nullopt.
[[nodiscard]] std::optional<MonitorHostSnapshot> await_calibration_baseline(
    const std::function<std::optional<MonitorHostSnapshot>()>& read,
    const std::function<bool()>& cancelled = {},
    ServeMonitor::SteadyNow now = {},
    std::function<void(std::chrono::milliseconds)> pause = {});
[[nodiscard]] CalibrationHostDiagnostics calibration_host_diagnostics(
    const std::optional<MonitorHostSnapshot>& before, const std::optional<MonitorHostSnapshot>& after,
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now(),
    std::optional<std::chrono::steady_clock::time_point> trial_started = std::nullopt,
    bool calibration_model_loaded = false);
} // namespace secure_frontend
