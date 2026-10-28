// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §5): calibration as upstream
// Strata and Project Maya do it - a decode-speed sweep of the machine-dependent engine flags that keeps a setting only
// when it is clearly faster (3%) and confirms the winner against the defaults in interleaved rounds - and its result
// file with the key it was measured under (stale when the machine, the engine or the model changed; "settings
// changed" when the options it was measured at did).
#include "secure_frontend/calibration.hpp"
#include "support/test_support.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

std::string joined(const std::vector<std::string>& args) {
  std::string out;
  for (const auto& arg : args) out += (out.empty() ? "" : " ") + arg;
  return out;
}

const std::vector<secure_frontend::SweepStep> kPlan{{"--cpu-lane", {"off", "on"}}, {"--pcie-frac", {"0", "0.25", "0.5"}},
                                                    {"--cpu-threads", {"16", "8"}}};

// A machine where only --pcie-frac 0.5 is clearly faster (10%); --cpu-lane on is 2% (noise), threads 8 fails to start.
std::optional<double> machine(const std::vector<std::string>& args) {
  double speed = 10.0;
  for (std::size_t i = 0; i + 1U < args.size(); i += 2U) {
    if (args[i] == "--cpu-lane" && args[i + 1U] == "on") speed *= 1.02;
    if (args[i] == "--pcie-frac" && args[i + 1U] == "0.5") speed *= 1.10;
    if (args[i] == "--cpu-threads" && args[i + 1U] == "8") return std::nullopt;
  }
  return speed;
}

void the_sweep_keeps_only_clear_wins() {
  std::vector<std::string> tried;
  std::size_t last_trial = 0;
  std::size_t trials = 0;
  const auto outcome = secure_frontend::run_sweep(
      kPlan,
      [&tried](const std::vector<std::string>& args) {
        tried.push_back(joined(args));
        return machine(args);
      },
      [&](const secure_frontend::SweepProgress& progress) {
        last_trial = progress.trial;
        trials = progress.trials;
      },
      [] { return false; });
  expect(outcome.end == secure_frontend::SweepOutcome::End::done, "the sweep ends");
  expect(joined(outcome.args) == "--pcie-frac 0.5", "only the clear win is kept: " + joined(outcome.args));
  expect(std::abs(outcome.baseline_tok_s - 10.0) < 1e-9 && std::abs(outcome.best_tok_s - 11.0) < 1e-9,
         "before and after: " + std::to_string(outcome.baseline_tok_s) + " -> " + std::to_string(outcome.best_tok_s));
  expect(!tried.empty() && tried.front().empty(), "the defaults are measured first");
  // the defaults, 2 + 3 + 2 candidates, then two interleaved rounds of the defaults and the winner
  expect(tried.size() == 12U && trials == 12U && last_trial == 12U, "every trial is counted: " + std::to_string(tried.size()));
  if (tried.size() != 12U) return;
  expect(tried[1] == "--cpu-lane off" && tried[2] == "--cpu-lane on" && tried[5] == "--pcie-frac 0.5" &&
             tried[6] == "--pcie-frac 0.5 --cpu-threads 16",
         "each step starts from the best so far: " + tried[6]);
  expect(tried[8].empty() && tried[9] == "--pcie-frac 0.5" && tried[10].empty() && tried[11] == "--pcie-frac 0.5",
         "the winner is confirmed against the defaults, interleaved");
}

void a_win_that_does_not_repeat_is_dropped() {
  // --cpu-lane on looks 20% faster once, then never again: the confirmation keeps the defaults
  int seen = 0;
  const auto outcome = secure_frontend::run_sweep(
      {{"--cpu-lane", {"on"}}},
      [&seen](const std::vector<std::string>& args) -> std::optional<double> {
        if (args.empty()) return 10.0;
        return ++seen == 1 ? 12.0 : 10.0;
      },
      [](const secure_frontend::SweepProgress&) {}, [] { return false; });
  expect(outcome.end == secure_frontend::SweepOutcome::End::done && outcome.args.empty() &&
             std::abs(outcome.baseline_tok_s - 10.0) < 1e-9 && std::abs(outcome.best_tok_s - outcome.baseline_tok_s) < 1e-9,
         "a win the confirmation does not repeat keeps the defaults: " + joined(outcome.args));
}

void a_failed_baseline_or_a_cancel_ends_it() {
  const auto failed = secure_frontend::run_sweep(
      kPlan, [](const std::vector<std::string>&) { return std::optional<double>{}; }, [](const secure_frontend::SweepProgress&) {},
      [] { return false; });
  expect(failed.end == secure_frontend::SweepOutcome::End::failed, "the engine does not start with its defaults: failed");
  int measured = 0;
  const auto cancelled = secure_frontend::run_sweep(
      kPlan,
      [&measured](const std::vector<std::string>& args) {
        ++measured;
        return machine(args);
      },
      [](const secure_frontend::SweepProgress&) {}, [&measured] { return measured >= 3; });
  expect(cancelled.end == secure_frontend::SweepOutcome::End::cancelled && measured == 3, "a cancel stops before the next trial");
  // a cancel that lands during a trial (the engine is stopped, the trial scores nothing) is a cancel, not a failure
  bool cancel = false;
  const auto during = secure_frontend::run_sweep(
      kPlan,
      [&cancel](const std::vector<std::string>&) {
        cancel = true;
        return std::optional<double>{};
      },
      [](const secure_frontend::SweepProgress&) {}, [&cancel] { return cancel; });
  expect(during.end == secure_frontend::SweepOutcome::End::cancelled, "a cancel during the defaults' trial is a cancel");
}

void the_plan_follows_the_engine() {
  const auto flint = secure_frontend::sweep_plan(secure_frontend::EngineProfile::flint, 24U);
  std::string flags;
  for (const auto& step : flint) flags += step.flag + "=" + joined(step.values) + ";";
  expect(flags == "--cpu-lane=off on;--pcie-frac=0 0.25 0.5 0.75;--cpu-threads=24 16 12;",
         "flint: the CPU lane, the PCIe share and the CPU threads (all, 2/3, 1/2): " + flags);
  const auto gemma = secure_frontend::sweep_plan(secure_frontend::EngineProfile::gemma4, 8U);
  expect(gemma.size() == 1U && gemma[0].flag == "--cpu-threads" && joined(gemma[0].values) == "8 5 4",
         "Gemma 4: the CPU threads (its engine's flags)");
  const auto one_core = secure_frontend::sweep_plan(secure_frontend::EngineProfile::flint, 1U);
  expect(!one_core.empty() && one_core.back().values == std::vector<std::string>{"1"}, "threads are distinct and at least 1");
  expect(secure_frontend::sweep_plan(secure_frontend::EngineProfile::qwen, 8U).empty() &&
             secure_frontend::sweep_plan(secure_frontend::EngineProfile::raon1, 8U).empty(),
         "other engines have no sweep here yet");
}

secure_frontend::CalibrationKey key() {
  return {.gpu = "NVIDIA GeForce RTX 5080, 16303 MiB", .cpu = "AMD Ryzen 9 7950X3D, 32 threads", .ram_bytes = 68719476736ULL,
          .engine = "68096:133000000", .model = "42301076928:134000000", .options = "expert_vram=auto;max_context=65536"};
}

void results_round_trip_and_keys_compare() {
  using secure_frontend::CalibrationState;
  const secure_frontend::CalibrationResult result{.card = "flint-Q8_0", .args = {"--pcie-frac", "0.5"}, .baseline_tok_s = 10.25,
                                                  .best_tok_s = 11.5, .date = "2026-10-27T18:30:00Z", .auto_recalibrate = true,
                                                  .key = key()};
  const std::string text = secure_frontend::render_calibration(result);
  const auto back = secure_frontend::parse_calibration(text);
  expect(back.has_value() && back->card == "flint-Q8_0" && back->args == result.args && back->baseline_tok_s == 10.25 &&
             back->best_tok_s == 11.5 && back->date == result.date && back->auto_recalibrate && back->key.options == key().options &&
             back->key.ram_bytes == key().ram_bytes && back->key.gpu == key().gpu,
         "a result reads back as written: " + text);
  expect(!secure_frontend::parse_calibration("{}").has_value() && !secure_frontend::parse_calibration("not json").has_value(),
         "a damaged file is no result");
  auto args = text;
  args.replace(args.find("--pcie-frac"), 11, "--model");
  expect(!secure_frontend::parse_calibration(args).has_value(), "a flag the sweep never sets is refused");

  expect(secure_frontend::compare_key(key(), key()) == CalibrationState::fresh, "the same key: fresh");
  for (const auto& change : std::vector<std::pair<std::string, secure_frontend::CalibrationKey>>{
           {"gpu", [] { auto k = key(); k.gpu = "NVIDIA RTX 4090, 24564 MiB"; return k; }()},
           {"cpu", [] { auto k = key(); k.cpu = "other"; return k; }()},
           {"ram", [] { auto k = key(); k.ram_bytes = 1; return k; }()},
           {"engine", [] { auto k = key(); k.engine = "1:2"; return k; }()},
           {"model", [] { auto k = key(); k.model = "1:2"; return k; }()}}) {
    expect(secure_frontend::compare_key(key(), change.second) == CalibrationState::stale, "a changed " + change.first + " is stale");
  }
  auto unknown_gpu = key();
  unknown_gpu.gpu.clear();
  expect(secure_frontend::compare_key(key(), unknown_gpu) == CalibrationState::fresh, "a GPU that can't be read now is not a change");
  auto options = key();
  options.options = "expert_vram=8;max_context=65536";
  expect(secure_frontend::compare_key(key(), options) == CalibrationState::settings_changed, "other options: settings changed");
}

void the_key_reads_files_and_options() {
  const auto dir = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(dir / "model.gguf", "GGUF1234");
  const std::string fact = secure_frontend::file_fact(dir / "model.gguf");
  expect(fact.rfind("8:", 0) == 0U && fact.size() > 2U, "a file's size and modification time: " + fact);
  expect(secure_frontend::file_fact(dir / "absent").empty(), "no file, no fact");
  const secure_frontend::ModelOptionValues values{{"max_context", std::int64_t{65536}}, {"expert_vram", std::string{"auto"}},
                                                  {"temperature", 0.6},                  {"top_p", 0.95},
                                                  {"thinking", std::string{"on"}},      {"mtp", true},
                                                  {"web_search", false}};
  expect(secure_frontend::options_fact(values) == "expert_vram=auto;max_context=65536;mtp=true",
         "the options that change the speed, by name; sampling, thinking and web search left out: " +
             secure_frontend::options_fact(values));
  expect(!secure_frontend::cpu_fact().empty(), "the CPU is named: " + secure_frontend::cpu_fact());
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"the_sweep_keeps_only_clear_wins", the_sweep_keeps_only_clear_wins},
      {"a_win_that_does_not_repeat_is_dropped", a_win_that_does_not_repeat_is_dropped},
      {"a_failed_baseline_or_a_cancel_ends_it", a_failed_baseline_or_a_cancel_ends_it},
      {"the_plan_follows_the_engine", the_plan_follows_the_engine},
      {"results_round_trip_and_keys_compare", results_round_trip_and_keys_compare},
      {"the_key_reads_files_and_options", the_key_reads_files_and_options},
  }, failures);
}
