#include "secure_frontend/calibration_experiment.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace secure_frontend {
namespace {
using End = ExperimentOutcome::End;
constexpr std::size_t kMaximumTrials = 4096U;
constexpr double kNoiseThreshold = 0.05;

bool fixed_setting(const FrontendConfigDraft& config, const std::string& name) {
  if (!name.starts_with("--")) return config.entries.contains(name);
  const auto entry = config.entries.find("engine.args");
  if (entry == config.entries.end()) return false;
  const auto* args = std::get_if<std::vector<std::string>>(&entry->second.value);
  if (!args) return true;
  return std::any_of(args->begin(), args->end(), [&](const auto& arg) { return arg == name || arg.starts_with(name + "="); });
}
using Choices = std::map<std::string, std::string>;
std::vector<std::string> arguments(const ExperimentPlan& plan, const Choices& chosen) {
  std::vector<std::string> result;
  for (const auto& step : plan.steps) {
    const auto found = chosen.find(step.flag);
    if (found == chosen.end()) continue;
    std::vector<std::string> names{step.flag};
    names.insert(names.end(), step.joint.begin(), step.joint.end());
    std::istringstream input(found->second);
    for (const auto& name : names) {
      std::string value;
      if (!(input >> value)) return {};
      result.push_back(name);
      result.push_back(std::move(value));
    }
  }
  return result;
}
double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const auto n = values.size();
  return n % 2U ? values[n / 2U] : values[n / 2U - 1U] / 2.0 + values[n / 2U] / 2.0;
}
struct Statistics { double middle, variance, cv; };
Statistics statistics(const std::vector<double>& values) {
  double mean = 0.0;
  for (const auto value : values) mean += value / static_cast<double>(values.size());
  double variance = 0.0;
  if (values.size() > 1U)
    for (const auto value : values) variance += (value - mean) * (value - mean) / static_cast<double>(values.size() - 1U);
  return {median(values), variance, mean > 0.0 ? std::sqrt(variance) / mean : 0.0};
}
bool valid_plan(const ExperimentPlan& plan) {
  if (plan.steps.size() > 64U || !std::isfinite(plan.keep_margin) || plan.keep_margin < 0.0 || plan.keep_margin > 1.0) return false;
  std::set<std::string> names;
  for (const auto& step : plan.steps) {
    if (step.flag.empty() || step.values.size() > 32U || step.joint.size() > 8U || step.max_tokens == 0U || step.max_tokens > 4096U ||
        !std::isfinite(step.stop_below) || step.stop_below < 0.0 || step.stop_below > 1.0 || !names.insert(step.flag).second) return false;
    for (const auto& joint : step.joint) if (joint.empty() || !names.insert(joint).second) return false;
    for (const auto& text : step.values) {
      if (text.empty() || text.size() > 256U) return false;
      std::istringstream input(text);
      std::size_t fields = 0U;
      for (std::string field; input >> field;) ++fields;
      if (fields != step.joint.size() + 1U) return false;
    }
  }
  return true;
}
}  // namespace

std::variant<BenchmarkOptions, std::string> parse_benchmark_options(const JsonValue& object) {
  const auto* fields = object.if_object();
  if (!fields) return std::string{"benchmark must be an object"};
  BenchmarkOptions options;
  bool custom_present = false;
  std::set<std::string> seen;
  for (const auto& field : *fields) {
    if (!seen.insert(field.key).second) return std::string{"benchmark contains a repeated field"};
    if (field.key == "mode") {
      const auto* mode = field.value.if_string();
      if (!mode || (*mode != "quick" && *mode != "thorough")) return std::string{"benchmark.mode must be quick or thorough"};
      options.mode = *mode == "quick" ? BenchmarkMode::quick : BenchmarkMode::thorough;
    } else if (field.key == "workload") {
      const auto* workload = field.value.if_string();
      if (!workload) return std::string{"benchmark.workload must name a supported workload"};
      if (*workload == "chat") options.workload = BenchmarkWorkload::chat;
      else if (*workload == "long_context") options.workload = BenchmarkWorkload::long_context;
      else if (*workload == "coding") options.workload = BenchmarkWorkload::coding;
      else if (*workload == "tools") options.workload = BenchmarkWorkload::tools;
      else if (*workload == "custom") options.workload = BenchmarkWorkload::custom;
      else return std::string{"benchmark.workload must name a supported workload"};
    } else if (field.key == "budget_seconds") {
      const auto* seconds = field.value.if_integer();
      if (!seconds || *seconds < 30 || *seconds > 86400) return std::string{"benchmark.budget_seconds must be an integer from 30 to 86400"};
      options.budget_seconds = static_cast<unsigned>(*seconds);
    } else if (field.key == "resume" || field.key == "compare_speed" || field.key == "memory_probe") {
      const auto* enabled = field.value.if_boolean();
      if (!enabled) return std::string{"benchmark switches must be boolean"};
      if (field.key == "resume") options.resume = *enabled;
      else if (field.key == "compare_speed") options.compare_speed = *enabled;
      else options.memory_probe = *enabled;
    } else if (field.key == "label") {
      const auto* label = field.value.if_string();
      if (!label || label->size() > 64U || !std::all_of(label->begin(), label->end(), [](unsigned char c) { return c >= 32U && c <= 126U; }))
        return std::string{"benchmark.label must contain at most 64 printable ASCII characters"};
      options.label = *label;
    } else if (field.key == "custom_prompt") {
      const auto* prompt = field.value.if_string();
      if (!prompt || prompt->empty() || prompt->size() > 16384U || !is_valid_utf8(*prompt) || prompt->find('\0') != std::string::npos ||
          prompt->find_first_not_of(" \t\r\n") == std::string::npos) return std::string{"benchmark.custom_prompt must contain 1 to 16384 bytes of text"};
      options.custom_prompt = *prompt;
      custom_present = true;
    } else return std::string{"benchmark contains an unsupported field"};
  }
  if (custom_present != (options.workload == BenchmarkWorkload::custom))
    return std::string{"benchmark.custom_prompt is required only for an explicitly selected custom workload"};
  return options;
}

ExperimentPlan make_experiment_plan(const EngineProfile profile, const SweepMachine& machine, const BenchmarkOptions& options,
                                   const FrontendConfigDraft& explicit_config, const bool mtp_available) {
  ExperimentPlan plan;
  plan.mode = options.mode;
  plan.keep_margin = sweep_keep_margin(profile);
  const SweepMachine bounded{std::clamp(machine.logical_cores, 1U, 256U), std::clamp(machine.physical_cores, 1U, 256U)};
  for (auto step : sweep_plan(profile, bounded)) {
    if (fixed_setting(explicit_config, step.flag) || std::any_of(step.joint.begin(), step.joint.end(),
        [&](const auto& name) { return fixed_setting(explicit_config, name); })) {
      plan.skipped.push_back({step.flag, "fixed by the card's explicit flags or configuration"});
      continue;
    }
    if (step.flag == "--spec-min-p" && !mtp_available) {
      plan.skipped.push_back({step.flag, "inactive because no MTP draft layer is available"});
      continue;
    }
    if (options.mode == BenchmarkMode::quick && step.max_tokens > 128U) {
      plan.skipped.push_back({step.flag, "long adaptive-tier experiments require thorough mode"});
      continue;
    }
    if (options.mode == BenchmarkMode::quick &&
        !(profile == EngineProfile::qwen && (step.flag == "--pcie-frac" || step.flag == "--spec-min-p"))) {
      plan.skipped.push_back({step.flag, "Requires an engine restart; use Thorough to tune this setting"});
      continue;
    }
    if (options.mode == BenchmarkMode::quick && step.values.size() > 3U)
      step.values = {step.values.front(), step.values[step.values.size() / 2U], step.values.back()};
    std::vector<std::string> unique;
    for (const auto& candidate : step.values)
      if (std::find(unique.begin(), unique.end(), candidate) == unique.end()) unique.push_back(candidate);
    step.values = std::move(unique);
    if (step.values.empty()) plan.skipped.push_back({step.flag, "no effective candidates on this machine"});
    else plan.steps.push_back(std::move(step));
  }
  return plan;
}

std::size_t experiment_trial_upper_bound(const ExperimentPlan& plan) {
  const unsigned repeats = plan.mode == BenchmarkMode::thorough ? 3U : 2U;
  const unsigned initial = plan.mode == BenchmarkMode::thorough ? 3U : 1U;
  std::size_t count = initial + (plan.steps.empty() ? repeats : 2U * repeats);
  for (const auto& step : plan.steps)
    count += (step.values.size() + 2U) * repeats * (plan.mode == BenchmarkMode::thorough ? 2U : 1U);
  return std::min(count, kMaximumTrials);
}

ExperimentOutcome run_experiment(const ExperimentPlan& plan, const BenchmarkOptions& options, const ExperimentMeasure& measure,
    const std::function<void(const ExperimentProgress&)>& progress, const std::function<bool()>& cancelled,
    const std::function<double()>& elapsed_seconds) {
  ExperimentOutcome out;
  out.skipped = plan.skipped;
  if (!measure || !valid_plan(plan) || plan.mode != options.mode || options.budget_seconds < 30U || options.budget_seconds > 86400U) {
    out.end = End::failed;
    return out;
  }
  const auto started = std::chrono::steady_clock::now();
  const auto elapsed = [&] {
    return elapsed_seconds ? elapsed_seconds() : std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  };
  const unsigned repeats = options.mode == BenchmarkMode::thorough ? 3U : 2U;
  const unsigned initial_repeats = options.mode == BenchmarkMode::thorough ? 3U : 1U;
  const auto upper_bound = experiment_trial_upper_bound(plan);
  const auto stopped = [&] {
    if (out.end != End::done) return true;
    if (cancelled && cancelled()) out.end = End::cancelled;
    else {
      const auto seconds = elapsed();
      if (!std::isfinite(seconds) || seconds < 0.0) out.end = End::failed;
      else if (seconds >= static_cast<double>(options.budget_seconds)) out.end = End::budget_exhausted;
    }
    return out.end != End::done;
  };
  const auto run = [&](const Choices& chosen, const unsigned tokens, const std::string& stage,
                       const unsigned replicate) -> std::optional<double> {
    if (stopped()) return {};
    if (out.records.size() >= kMaximumTrials) { out.end = End::failed; return {}; }
    ExperimentTrial trial{arguments(plan, chosen), tokens, stage, replicate, out.records.size() + 1U};
    if (progress) {
      try { progress({stage, out.records.size(), upper_bound,
                      out.best_tok_s > 0.0 ? std::optional<double>{out.best_tok_s} : std::nullopt,
                      elapsed(), 0.0}); }
      catch (...) { out.end = End::failed; return {}; }
    }
    std::optional<ExperimentSample> sample;
    try { sample = measure(trial); }
    catch (...) { out.end = End::failed; }
    if (sample && sample->contaminated) out.end = End::contaminated;
    else if (sample && !sample->quality_ok) out.end = End::quality_failed;
    if (sample && (!std::isfinite(sample->tok_s) || sample->tok_s <= 0.0 || sample->tok_s > 1.0e9)) {
      if (out.end != End::done) sample->tok_s = 0.0;
      else sample.reset();
    }
    out.records.push_back({trial, sample});
    if (progress) {
      const double seconds = elapsed();
      const auto remaining = upper_bound > out.records.size() ? upper_bound - out.records.size() : 0U;
      const double estimate = seconds / static_cast<double>(out.records.size()) * static_cast<double>(remaining);
      try {
        progress({stage, out.records.size(), upper_bound,
                  out.best_tok_s > 0.0 ? std::optional<double>{out.best_tok_s} : std::nullopt,
                  seconds, std::min(estimate, std::max(0.0, static_cast<double>(options.budget_seconds) - seconds))});
      } catch (...) { out.end = End::failed; }
    }
    if (stopped()) return {};
    return sample ? std::optional<double>{sample->tok_s} : std::nullopt;
  };
  const auto group = [&](const Choices& chosen, const unsigned tokens, const std::string& stage, const unsigned count) {
    std::vector<double> scores;
    for (unsigned replicate = 0; replicate < count; ++replicate) {
      const auto score = run(chosen, tokens, stage, replicate);
      if (!score) break;
      scores.push_back(*score);
    }
    return scores;
  };
  const auto baseline = group({}, 128U, "defaults", initial_repeats);
  if (out.end != End::done) return out;
  if (baseline.size() != initial_repeats) { out.end = End::failed; return out; }
  Choices chosen;
  double current = median(baseline);
  out.baseline_tok_s = current;
  out.best_tok_s = current;
  unsigned current_tokens = 128U;
  const auto sweep = [&](const bool refining) {
    for (const auto& step : plan.steps) {
      if (stopped()) return;
      if (current_tokens != step.max_tokens) {
        const auto matched = group(chosen, step.max_tokens, "matching-length-baseline", initial_repeats);
        if (out.end != End::done) return;
        if (matched.size() != initial_repeats) { out.end = End::failed; return; }
        current = median(matched);
        current_tokens = step.max_tokens;
      }
      std::vector<std::optional<std::string>> candidates;
      const auto old = chosen.find(step.flag);
      if (refining && old != chosen.end()) {
        candidates.emplace_back(std::nullopt);
        const auto position = std::find(step.values.begin(), step.values.end(), old->second);
        if (position != step.values.end()) {
          if (position != step.values.begin()) candidates.emplace_back(*(position - 1));
          if (position + 1 != step.values.end()) candidates.emplace_back(*(position + 1));
        }
      } else for (const auto& value : step.values) candidates.emplace_back(value);
      double step_best = current;
      for (const auto& value : candidates) {
        auto candidate = chosen;
        if (value) candidate[step.flag] = *value;
        else candidate.erase(step.flag);
        if (candidate == chosen) continue;
        const std::string stage = refining ? "refining " + step.flag : step.flag;
        const auto first = run(candidate, step.max_tokens, stage, 0U);
        if (out.end != End::done) return;
        if (!first) continue;
        double score = *first;
        if (score > current * (1.0 + plan.keep_margin)) {
          std::vector<double> scores{score};
          for (unsigned replicate = 1U; replicate < repeats; ++replicate) {
            const auto repeated = run(candidate, step.max_tokens, stage, replicate);
            if (!repeated) break;
            scores.push_back(*repeated);
          }
          if (out.end != End::done) return;
          if (scores.size() != repeats) continue;
          score = median(scores);
          if (score > current * (1.0 + plan.keep_margin)) {
            chosen = std::move(candidate);
            current = score;
            out.best_tok_s = score;
          }
        }
        if (!refining && step.stop_below > 0.0 && score < step.stop_below * step_best) {
          out.skipped.push_back({step.flag, "remaining candidates stopped after a measured throughput drop"});
          break;
        }
        step_best = std::max(step_best, score);
      }
    }
  };
  sweep(false);
  if (out.end == End::done && options.mode == BenchmarkMode::thorough) sweep(true);
  if (stopped()) return out;
  unsigned confirmation_tokens = 128U;
  for (const auto& step : plan.steps) if (chosen.contains(step.flag)) confirmation_tokens = std::max(confirmation_tokens, step.max_tokens);
  // Interleave the baseline and winner to expose drift rather than comparing
  // one cold baseline with a much later candidate. All repetitions are distinct.
  std::vector<double> defaults, winner;
  for (unsigned replicate = 0; replicate < repeats; ++replicate) {
    const auto base = run({}, confirmation_tokens, "confirming-baseline", replicate);
    if (out.end != End::done) return out;
    if (!base) { out.end = End::failed; return out; }
    defaults.push_back(*base);
    if (!chosen.empty()) {
      const auto selected = run(chosen, confirmation_tokens, "confirming-winner", replicate);
      if (out.end != End::done) return out;
      if (!selected) { out.end = End::failed; return out; }
      winner.push_back(*selected);
    }
  }
  if (stopped()) return out;
  if (chosen.empty()) winner = defaults;
  const auto base_stats = statistics(defaults), best_stats = statistics(winner);
  out.baseline_tok_s = base_stats.middle;
  out.best_tok_s = best_stats.middle;
  out.baseline_variance = base_stats.variance;
  out.best_variance = best_stats.variance;
  out.relative_variation = std::max(base_stats.cv, best_stats.cv);
  out.noisy = out.relative_variation > kNoiseThreshold;
  // A conservative noise margin, not a claim of statistical significance or
  // semantic correctness. Small gains and unstable measurements stay unapplied.
  const double uncertainty = 2.0 * std::sqrt(base_stats.cv * base_stats.cv / static_cast<double>(defaults.size()) +
                                           best_stats.cv * best_stats.cv / static_cast<double>(winner.size()));
  const bool gained = best_stats.middle > base_stats.middle * (1.0 + std::max(plan.keep_margin, uncertainty));
  if (!out.noisy && gained) out.args = arguments(plan, chosen);
  else if (!out.noisy) out.best_tok_s = out.baseline_tok_s;
  out.apply_allowed = !out.noisy && !options.compare_speed;
  return out;
}
}  // namespace secure_frontend
