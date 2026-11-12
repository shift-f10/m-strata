#include "secure_frontend/calibration_experiment.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <iostream>

namespace {
using namespace secure_frontend;
int failures = 0;
void expect(bool condition, const char* message) { if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; } }
JsonValue json(std::string_view text) {
  return std::get<JsonValue>(parse_json(text, {65536U, 8U, 64U, 20000U, true}));
}
std::string value(const ExperimentTrial& trial, std::string_view key) {
  for (std::size_t i = 0; i + 1U < trial.args.size(); i += 2U) if (trial.args[i] == key) return trial.args[i + 1U];
  return {};
}
void benchmark_options_are_strict_and_bounded() {
  const auto defaults = parse_benchmark_options(json("{}"));
  const auto* base = std::get_if<BenchmarkOptions>(&defaults);
  expect(base && base->mode == BenchmarkMode::quick && base->workload == BenchmarkWorkload::chat &&
         base->budget_seconds == 1800U && !base->resume && !base->compare_speed && !base->memory_probe, "safe explicit-object defaults");
  const auto selected = parse_benchmark_options(json(R"({"mode":"thorough","workload":"custom","budget_seconds":30,"resume":true,"compare_speed":true,"label":"Trial 1","custom_prompt":"A deliberate prompt","memory_probe":true})"));
  const auto* options = std::get_if<BenchmarkOptions>(&selected);
  expect(options && options->mode == BenchmarkMode::thorough && options->workload == BenchmarkWorkload::custom &&
         options->budget_seconds == 30U && options->resume && options->compare_speed && options->memory_probe &&
         options->custom_prompt == "A deliberate prompt", "typed explicit choices preserved");
  for (const auto* bad : {"null", "[]", R"({"unknown":true})", R"({"mode":"auto"})", R"({"workload":"other"})",
       R"({"budget_seconds":29})", R"({"budget_seconds":86401})", R"({"budget_seconds":30.0})", R"({"resume":1})",
       R"({"label":"bad\nlabel"})", R"({"workload":"custom"})", R"({"custom_prompt":"unsolicited"})",
       R"({"workload":"custom","custom_prompt":"  "})"})
    expect(std::holds_alternative<std::string>(parse_benchmark_options(json(bad))), "malformed benchmark refused");
  expect(std::holds_alternative<std::string>(parse_benchmark_options(JsonValue::make_object({{"label", JsonValue::make_string(std::string(65U, 'a'))}}))), "label bounded");
  expect(std::holds_alternative<std::string>(parse_benchmark_options(JsonValue::make_object({{"workload", JsonValue::make_string("custom")}, {"custom_prompt", JsonValue::make_string(std::string(16385U, 'a'))}}))), "custom prompt bounded");
}

void experiment_plan_explains_fixed_and_inactive_settings() {
  BenchmarkOptions quick;
  auto thorough = quick; thorough.mode = BenchmarkMode::thorough;
  const SweepMachine machine{24U, 12U};
  const auto quick_plan = make_experiment_plan(EngineProfile::qwen, machine, quick, {}, true);
  const auto full = make_experiment_plan(EngineProfile::qwen, machine, thorough, {}, true);
  const auto count = [](const auto& plan) { std::size_t n = 0; for (const auto& step : plan.steps) n += step.values.size(); return n; };
  expect(count(quick_plan) < count(full) && count(quick_plan) > 0, "quick plan has fewer meaningful candidates");
  for (const auto& step : quick_plan.steps)
    expect(step.flag == "--pcie-frac" || step.flag == "--spec-min-p", "Quick only tunes controls that can change within one loaded engine");
  expect(make_experiment_plan(EngineProfile::gemma4, machine, quick, {}, true).steps.empty(),
         "Quick does not silently restart other engine families to tune startup settings");
  FrontendConfigDraft fixed;
  fixed.entries["engine.args"] = {std::vector<std::string>{"--pcie-frac=0.25", "--adapt-swaps", "80"}, ConfigSource::file};
  const auto filtered = make_experiment_plan(EngineProfile::qwen, machine, thorough, fixed, false);
  for (const auto& step : filtered.steps)
    expect(step.flag != "--pcie-frac" && step.flag != "--adapt-every" && step.flag != "--spec-min-p", "fixed, joint-fixed and absent-MTP trials skipped");
  expect(filtered.skipped.size() >= 3U, "skips carry explanations");
  fixed.entries["glm.pcie_share"] = {0.25, ConfigSource::file};
  const auto glm = make_experiment_plan(EngineProfile::glm53, machine, thorough, fixed, true);
  expect(std::none_of(glm.steps.begin(), glm.steps.end(), [](const auto& step) { return step.flag == "glm.pcie_share"; }), "fixed environment-backed row skipped");
}

void thorough_revisits_combinations_and_confirms() {
  const ExperimentPlan plan{{{"--cpu-threads", {"2", "4"}}, {"--pcie-frac", {"0.25", "0.5"}}}, {}, BenchmarkMode::thorough, 0.03};
  BenchmarkOptions options; options.mode = BenchmarkMode::thorough;
  std::size_t refined = 0;
  const auto result = run_experiment(plan, options, [&](const ExperimentTrial& trial) -> std::optional<ExperimentSample> {
    if (trial.stage.starts_with("refining")) ++refined;
    const auto threads = value(trial, "--cpu-threads"), share = value(trial, "--pcie-frac");
    const double score = threads == "2" && share == "0.25" ? 140 : threads == "2" && share == "0.5" ? 130 :
                         threads == "4" ? 95 : share == "0.25" ? 110 : share == "0.5" ? 105 : threads == "2" ? 101 : 100;
    return ExperimentSample{score};
  });
  expect(result.end == ExperimentOutcome::End::done && result.apply_allowed && !result.noisy && refined > 0U, "thorough refinement completes safely");
  expect(result.args == std::vector<std::string>{"--cpu-threads", "2", "--pcie-frac", "0.25"} && result.best_tok_s == 140 &&
         result.baseline_tok_s == 100, "second pass discovers the interaction missed by one sequential sweep");
  expect(std::count_if(result.records.begin(), result.records.end(), [](const auto& record) { return record.trial.stage == "confirming-winner"; }) == 3,
         "winner receives three separate confirmation trials");
}

void budgets_and_cancellation_never_expose_partial_winners() {
  const ExperimentPlan plan{{{"--cpu-threads", {"2", "4"}}}};
  BenchmarkOptions options; options.budget_seconds = 30U;
  double elapsed = 0;
  const auto budget = run_experiment(plan, options, [&](const ExperimentTrial& trial) -> std::optional<ExperimentSample> {
    elapsed += 10; return ExperimentSample{trial.args.empty() ? 100.0 : 200.0};
  }, {}, {}, [&] { return elapsed; });
  expect(budget.end == ExperimentOutcome::End::budget_exhausted && !budget.apply_allowed && budget.args.empty(), "budget exhausted after measure cannot apply a partial win");
  std::size_t measured = 0;
  const auto cancelled = run_experiment(plan, options, [&](const ExperimentTrial& trial) -> std::optional<ExperimentSample> {
    ++measured; return ExperimentSample{trial.args.empty() ? 100.0 : 200.0};
  }, {}, [&] { return measured >= 2U; });
  expect(cancelled.end == ExperimentOutcome::End::cancelled && !cancelled.apply_allowed && cancelled.args.empty(), "post-measure cancellation cannot apply");
}

void quality_contamination_noise_and_comparison_gate_application() {
  const ExperimentPlan plan{{{"--cpu-threads", {"2"}}}, {}, BenchmarkMode::thorough, 0.03};
  BenchmarkOptions options; options.mode = BenchmarkMode::thorough;
  for (const bool contamination : {false, true}) {
    const auto result = run_experiment(plan, options, [contamination](const ExperimentTrial& trial) -> std::optional<ExperimentSample> {
      return ExperimentSample{trial.args.empty() ? 100.0 : 500.0, trial.args.empty() || contamination,
                               !trial.args.empty() && contamination, "acceptance check"};
    });
    expect(result.end == (contamination ? ExperimentOutcome::End::contaminated : ExperimentOutcome::End::quality_failed) &&
           !result.apply_allowed && result.args.empty(), "faster but bad or contaminated trial cannot win");
    const auto invalid_speed = run_experiment(plan, options, [contamination](const ExperimentTrial& trial) -> std::optional<ExperimentSample> {
      return ExperimentSample{trial.args.empty() ? 100.0 : 0.0, trial.args.empty() || contamination,
                               !trial.args.empty() && contamination, "failed generation has no speed"};
    });
    expect(invalid_speed.end == (contamination ? ExperimentOutcome::End::contaminated : ExperimentOutcome::End::quality_failed) &&
           !invalid_speed.apply_allowed && invalid_speed.args.empty(), "quality or contamination failure cannot become a skipped missing-speed sample");
  }
  const auto noisy = run_experiment(plan, options, [](const ExperimentTrial& trial) -> std::optional<ExperimentSample> {
    const double factors[]{1.0, 1.3, 0.7};
    return ExperimentSample{(trial.args.empty() ? 100.0 : 200.0) * factors[trial.replicate % 3U]};
  });
  expect(noisy.end == ExperimentOutcome::End::done && noisy.noisy && noisy.relative_variation > 0.05 &&
         noisy.baseline_variance > 0 && !noisy.apply_allowed && noisy.args.empty(), "measured variation is reported and noisy winner withheld");
  options.compare_speed = true;
  const auto comparison = run_experiment(plan, options, [](const ExperimentTrial& trial) -> std::optional<ExperimentSample> {
    return ExperimentSample{trial.args.empty() ? 100.0 : 200.0, true, false, {}, true};
  });
  expect(comparison.end == ExperimentOutcome::End::done && !comparison.apply_allowed && comparison.best_tok_s > comparison.baseline_tok_s,
         "explicit comparison reports a result without permission to apply it");
  expect(!comparison.records.empty() && comparison.records.front().sample->reused, "bound resumed samples remain marked in results");
  const ExperimentPlan comparison_plan{{{"option:speed_fast_math", {"true"}}}, {}, BenchmarkMode::thorough, 0.03};
  const auto typed_comparison = run_experiment(comparison_plan, options, [](const ExperimentTrial& trial) -> std::optional<ExperimentSample> {
    return ExperimentSample{value(trial, "option:speed_fast_math") == "true" ? 200.0 : 100.0};
  });
  expect(typed_comparison.end == ExperimentOutcome::End::done && !typed_comparison.apply_allowed &&
         typed_comparison.args == std::vector<std::string>{"option:speed_fast_math", "true"},
         "typed controller comparison plans report options without applying them");
}
void progress_is_available_before_measurement_and_after_failure() {
  for (const auto mode : {BenchmarkMode::quick, BenchmarkMode::thorough}) {
    BenchmarkOptions options; options.mode = mode;
    ExperimentPlan plan; plan.mode = mode;
    std::optional<ExperimentProgress> latest;
    const auto outcome = run_experiment(plan, options,
        [&](const ExperimentTrial& trial) -> std::optional<ExperimentSample> {
          expect(latest && latest->completed + 1U == trial.ordinal,
                 "trial count published before loading or measurement begins");
          expect(latest && latest->trials_upper_bound == experiment_trial_upper_bound(plan) && latest->trials_upper_bound > 0U,
                 "quick and thorough publish their planned count before the first trial");
          return std::nullopt;
        }, [&](const ExperimentProgress& progress) { latest = progress; });
    expect(outcome.end == ExperimentOutcome::End::failed, "failed measurement remains failed");
    expect(latest && latest->completed == 1U, "failed first trial still reports its completed count");
  }
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"progress_is_available_before_measurement_and_after_failure", progress_is_available_before_measurement_and_after_failure},
      {"benchmark_options_are_strict_and_bounded", benchmark_options_are_strict_and_bounded},
      {"experiment_plan_explains_fixed_and_inactive_settings", experiment_plan_explains_fixed_and_inactive_settings},
      {"thorough_revisits_combinations_and_confirms", thorough_revisits_combinations_and_confirms},
      {"budgets_and_cancellation_never_expose_partial_winners", budgets_and_cancellation_never_expose_partial_winners},
      {"quality_contamination_noise_and_comparison_gate_application", quality_contamination_noise_and_comparison_gate_application}}, failures);
}
