#pragma once

#include "secure_frontend/calibration.hpp"
#include "secure_frontend/json.hpp"

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace secure_frontend {

enum class BenchmarkMode { quick, thorough };
enum class BenchmarkWorkload { chat, long_context, coding, tools, custom };
struct BenchmarkOptions final {
  BenchmarkMode mode{BenchmarkMode::quick};
  BenchmarkWorkload workload{BenchmarkWorkload::chat};
  unsigned budget_seconds{1800U};
  bool resume{false};
  bool compare_speed{false};
  std::string label{};
  std::string custom_prompt{};
  bool memory_probe{false};
};
// The optional benchmark object's contents only. Callers retain their legacy
// path when the field is absent. Errors are fixed messages, never input text.
[[nodiscard]] std::variant<BenchmarkOptions, std::string> parse_benchmark_options(const JsonValue& object);

struct ExperimentSkip final { std::string setting; std::string reason; };
struct ExperimentPlan final {
  std::vector<SweepStep> steps{};
  std::vector<ExperimentSkip> skipped{};
  BenchmarkMode mode{BenchmarkMode::quick};
  double keep_margin{0.03};
};
// Only explicitly configured rows/flags belong in explicit_config. A joint
// step is fixed if any member is fixed. This never modifies run_sweep's plan.
[[nodiscard]] ExperimentPlan make_experiment_plan(EngineProfile profile, const SweepMachine& machine,
    const BenchmarkOptions& options, const FrontendConfigDraft& explicit_config, bool mtp_available);

struct ExperimentTrial final {
  std::vector<std::string> args;
  unsigned max_tokens{128U};
  std::string stage;
  unsigned replicate{0U};
  std::size_t ordinal{0U};
};
[[nodiscard]] std::size_t experiment_trial_upper_bound(const ExperimentPlan& plan);
struct ExperimentSample final {
  double tok_s{0.0};
  bool quality_ok{true};
  bool contaminated{false};
  std::string reason{};
  bool reused{false};
};
struct ExperimentRecord final { ExperimentTrial trial; std::optional<ExperimentSample> sample; };
struct ExperimentProgress final {
  std::string stage;
  std::size_t completed{0U};
  std::size_t trials_upper_bound{0U};
  std::optional<double> best_tok_s;
  double elapsed_seconds{0.0};
  double estimated_remaining_seconds{0.0};
};
struct ExperimentOutcome final {
  enum class End { done, cancelled, budget_exhausted, failed, quality_failed, contaminated } end{End::done};
  std::vector<std::string> args;
  double baseline_tok_s{0.0};
  double best_tok_s{0.0};
  double baseline_variance{0.0};
  double best_variance{0.0};
  double relative_variation{0.0};  // variation of selected settings; rejected candidates are separate
  bool noisy{false};
  bool retained_baseline{false};
  double rejected_candidate_variation{0.0};
  bool apply_allowed{false};
  std::vector<ExperimentRecord> records;
  std::vector<ExperimentSkip> skipped;
};
using ExperimentMeasure = std::function<std::optional<ExperimentSample>(const ExperimentTrial&)>;
// A resume-aware measure may return a bound cached sample with reused=true;
// identity validation and persistence belong to the caller. Every replicate
// remains distinct. No partial winner is exposed on cancellation/failure.
[[nodiscard]] ExperimentOutcome run_experiment(const ExperimentPlan& plan, const BenchmarkOptions& options,
    const ExperimentMeasure& measure, const std::function<void(const ExperimentProgress&)>& progress = {},
    const std::function<bool()>& cancelled = {}, const std::function<double()>& elapsed_seconds = {});

}  // namespace secure_frontend
