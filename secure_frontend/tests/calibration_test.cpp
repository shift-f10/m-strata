// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §5): calibration as upstream
// Strata and Project Maya do it - a decode-speed sweep of the machine-dependent engine flags that keeps a setting only
// when it is clearly faster (3%) and confirms the winner against the defaults in interleaved rounds - and its result
// file with the key it was measured under (stale when the machine, the engine or the model changed; "settings
// changed" when the options it was measured at did).
#include "secure_frontend/calibration.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <fstream>
#include <set>
#endif

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
      [&tried](const std::vector<std::string>& args, unsigned) {
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
      [&seen](const std::vector<std::string>& args, unsigned) -> std::optional<double> {
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
      kPlan, [](const std::vector<std::string>&, unsigned) { return std::optional<double>{}; }, [](const secure_frontend::SweepProgress&) {},
      [] { return false; });
  expect(failed.end == secure_frontend::SweepOutcome::End::failed, "the engine does not start with its defaults: failed");
  int measured = 0;
  const auto cancelled = secure_frontend::run_sweep(
      kPlan,
      [&measured](const std::vector<std::string>& args, unsigned) {
        ++measured;
        return machine(args);
      },
      [](const secure_frontend::SweepProgress&) {}, [&measured] { return measured >= 3; });
  expect(cancelled.end == secure_frontend::SweepOutcome::End::cancelled && measured == 3, "a cancel stops before the next trial");
  // a cancel that lands during a trial (the engine is stopped, the trial scores nothing) is a cancel, not a failure
  bool cancel = false;
  const auto during = secure_frontend::run_sweep(
      kPlan,
      [&cancel](const std::vector<std::string>&, unsigned) {
        cancel = true;
        return std::optional<double>{};
      },
      [](const secure_frontend::SweepProgress&) {}, [&cancel] { return cancel; });
  expect(during.end == secure_frontend::SweepOutcome::End::cancelled, "a cancel during the defaults' trial is a cancel");
}

// A plan as text: each step's setting (its joint flags after '+'), its candidates, its answers' length and its floor.
std::string plan_text(const std::vector<secure_frontend::SweepStep>& plan) {
  std::string out;
  for (const auto& step : plan) {
    out += step.flag;
    for (const auto& flag : step.joint) out += "+" + flag;
    out += "=";
    for (std::size_t i = 0; i < step.values.size(); ++i) out += (i == 0U ? "" : ",") + step.values[i];
    if (step.max_tokens != 128U) out += " tokens " + std::to_string(step.max_tokens);
    if (step.stop_below > 0.0) out += " stop " + std::to_string(static_cast<int>(std::lround(step.stop_below * 100.0))) + "%";
    out += ";";
  }
  return out;
}

void the_plan_follows_the_engine() {
  using secure_frontend::EngineProfile;
  using secure_frontend::sweep_plan;
  const secure_frontend::SweepMachine smt{24U, 12U};  // 12 cores, 2 threads each
  expect(plan_text(sweep_plan(EngineProfile::flint, smt)) == "--cpu-lane=off,on;--pcie-frac=0,0.25,0.5,0.75;--cpu-threads=24,16,12;",
         "flint: the CPU lane, the PCIe share and the CPU threads (all, 2/3, 1/2): " + plan_text(sweep_plan(EngineProfile::flint, smt)));
  expect(plan_text(sweep_plan(EngineProfile::gemma4, {8U, 4U})) == "--cpu-threads=8,5,4;", "Gemma 4: the CPU threads (its engine's flags)");
  const auto one_core = sweep_plan(EngineProfile::flint, {1U, 1U});
  expect(!one_core.empty() && one_core.back().values == std::vector<std::string>{"1"}, "threads are distinct and at least 1");
  // upstream's tools/calibrate.py: the PCIe share, the draft floor, fewer pool workers than the engine's 11 (12 cores
  // but the host's; Python's round, half to even: 7, 6, 3), the expert tier on 512-token answers
  expect(plan_text(sweep_plan(EngineProfile::qwen, smt)) ==
             "--pcie-frac=0,0.2,0.35,0.55,0.75,0.9,1;--spec-min-p=0.3,0.5,0.7;--pool-workers=7,6,3;"
             "--adapt-every+--adapt-swaps+--adapt-decay=1 80 0.97,1 160 0.97 tokens 512;",
         "Qwen: upstream's steps: " + plan_text(sweep_plan(EngineProfile::qwen, smt)));
  // Maya's tools/calibrate_glm.py through the environment rows: the share, then fewer lane threads than the engine's 12
  const std::string glm = "glm.pcie_share=0,0.1,0.25,0.5,0.75 stop 70%;glm.cpu_lane=9,8,6,3 stop 70%;";
  expect(plan_text(sweep_plan(EngineProfile::glm53, smt)) == glm && plan_text(sweep_plan(EngineProfile::raon1, smt)) == glm,
         "GLM-5.3 and raon1 (the GLM engine): Maya's steps: " + plan_text(sweep_plan(EngineProfile::glm53, smt)));
  // without SMT siblings the GLM engine's own lane is two thirds of the CPUs (16: 10)
  expect(plan_text(sweep_plan(EngineProfile::glm53, {16U, 16U})) == "glm.pcie_share=0,0.1,0.25,0.5,0.75 stop 70%;glm.cpu_lane=8,7,5,2 stop 70%;",
         "a CPU without SMT: " + plan_text(sweep_plan(EngineProfile::glm53, {16U, 16U})));
  // Maya's tools/calibrate_ds4_tp.py (and tools/mm3/calibrate.py): fewer CPU threads than the physical cores; 2 and 4
  // fewer first where the lane takes every CPU (no SMT)
  for (const auto profile : {EngineProfile::deepseek, EngineProfile::minimax}) {
    expect(plan_text(sweep_plan(profile, smt)) == "--cpu-threads=9,6 stop 100%;" &&
               plan_text(sweep_plan(profile, {16U, 16U})) == "--cpu-threads=14,12,8 stop 100%;",
           "DeepSeek and MiniMax: Maya's CPU threads: " + plan_text(sweep_plan(profile, smt)) + " / " +
               plan_text(sweep_plan(profile, {16U, 16U})));
  }
  expect(sweep_plan(EngineProfile::deepseek, {1U, 1U}).empty() && sweep_plan(EngineProfile::glm53, {1U, 1U}).size() == 1U,
         "nothing fewer than one thread to try");
  // the margins and the first start
  expect(secure_frontend::sweep_keep_margin(EngineProfile::deepseek) == 0.05 && secure_frontend::sweep_keep_margin(EngineProfile::minimax) == 0.05 &&
             secure_frontend::sweep_keep_margin(EngineProfile::qwen) == 0.03 && secure_frontend::sweep_keep_margin(EngineProfile::glm53) == 0.03,
         "Maya's 5% for separate DeepSeek and MiniMax starts, 3% else");
  for (const auto profile : {EngineProfile::qwen, EngineProfile::glm53, EngineProfile::raon1, EngineProfile::deepseek,
                             EngineProfile::minimax, EngineProfile::flint, EngineProfile::gemma4}) {
    expect(!sweep_plan(profile, smt).empty(), "every engine has its sweep");
    const bool first = profile == EngineProfile::flint || profile == EngineProfile::gemma4;
    expect(secure_frontend::calibrates_before_first_start(profile) == first,
           "only flint and Gemma 4 calibrate before a first start (the others' sweeps take many starts of a large model)");
  }
  const unsigned cores = secure_frontend::physical_cores();
  expect(cores >= 1U && cores <= std::max(1U, std::thread::hardware_concurrency()),
         "this machine's physical cores: " + std::to_string(cores));
}

// Every physical core is counted (2026-10-30: a calibration report said "11 physical cores" for a 12-core Ryzen 9 3900X -
// the last core record of GetLogicalProcessorInformationEx was left out), checked against another count: Windows' older
// GetLogicalProcessorInformation, /proc/cpuinfo's (physical id, core id) pairs on Linux.
void physical_cores_count_every_core() {
  unsigned expected = 0;
#if defined(_WIN32)
  DWORD length = 0;
  (void)GetLogicalProcessorInformation(nullptr, &length);
  std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> records(length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1U);
  length = static_cast<DWORD>(records.size() * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
  if (GetLogicalProcessorInformation(records.data(), &length) != FALSE) {
    for (std::size_t i = 0; i < length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); ++i) {
      if (records[i].Relationship == RelationProcessorCore) ++expected;
    }
  }
#else
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::set<std::pair<std::string, std::string>> cores;
  std::string package;
  for (std::string line; std::getline(cpuinfo, line);) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const auto value = colon + 2U <= line.size() ? line.substr(colon + 2U) : std::string{};
    if (line.rfind("physical id", 0) == 0U) package = value;
    if (line.rfind("core id", 0) == 0U) cores.emplace(package, value);
  }
  expected = static_cast<unsigned>(cores.size());
#endif
  if (expected == 0U) {
    std::cout << "SKIP: this machine's cores could not be counted another way\n";
    return;
  }
  expect(secure_frontend::physical_cores() == expected,
         "every core: " + std::to_string(secure_frontend::physical_cores()) + " of " + std::to_string(expected));
}

// upstream's expert tier: three flags set together, measured on longer answers (after the best so far is measured on
// them too); the winner is confirmed on answers as long.
void joint_steps_and_longer_answers() {
  std::vector<std::pair<std::string, unsigned>> tried;
  const std::string tier = "--adapt-every 1 --adapt-swaps 160 --adapt-decay 0.97";
  const auto outcome = secure_frontend::run_sweep(
      {{"--pcie-frac", {"0.5"}}, {"--adapt-every", {"1 80 0.97", "1 160 0.97"}, {"--adapt-swaps", "--adapt-decay"}, 512U}},
      [&tried](const std::vector<std::string>& args, const unsigned max_tokens) -> std::optional<double> {
        tried.emplace_back(joined(args), max_tokens);
        return joined(args).find("--adapt-swaps 160") != std::string::npos ? 12.0 : 10.0;
      },
      [](const secure_frontend::SweepProgress&) {}, [] { return false; });
  expect(outcome.end == secure_frontend::SweepOutcome::End::done && joined(outcome.args) == tier,
         "the joint candidate is kept, its flags together: " + joined(outcome.args));
  expect(std::abs(outcome.baseline_tok_s - 10.0) < 1e-9 && std::abs(outcome.best_tok_s - 12.0) < 1e-9, "scored on the long answers");
  std::string order;
  for (const auto& [args, tokens] : tried) order += "[" + args + " @" + std::to_string(tokens) + "]";
  expect(order == "[ @128][--pcie-frac 0.5 @128][ @512][--adapt-every 1 --adapt-swaps 80 --adapt-decay 0.97 @512][" + tier + " @512]"
                  "[ @512][" + tier + " @512][ @512][" + tier + " @512]",
         "the best so far is measured again on the step's answers, and the confirmation on them: " + order);
}

// Maya's GLM steps end at a candidate below 70% of the step's best (fewer only get slower from there).
void a_step_ends_below_its_floor() {
  std::vector<std::string> tried;
  std::size_t trials = 0;
  const auto outcome = secure_frontend::run_sweep(
      {{"glm.pcie_share", {"0", "0.1", "0.25"}, {}, 128U, 0.7}, {"glm.cpu_lane", {"9"}, {}, 128U, 0.7}},
      [&tried](const std::vector<std::string>& args, unsigned) -> std::optional<double> {
        tried.push_back(joined(args));
        return args.size() >= 2U && args[1] == "0" ? 6.0 : 10.0;
      },
      [&trials](const secure_frontend::SweepProgress& progress) { trials = progress.trials; }, [] { return false; });
  expect(outcome.end == secure_frontend::SweepOutcome::End::done && outcome.args.empty(), "nothing kept");
  expect(tried == std::vector<std::string>{"", "glm.pcie_share 0", "glm.cpu_lane 9"},
         "a share at 60% of the best ends its step; the next step still runs: " + std::to_string(tried.size()));
  // the count the page shows (the defaults, 3 + 1 candidates and 4 possible confirmations) loses the 2 skipped
  expect(trials == 7U, "the trials the page counts lose the skipped ones: " + std::to_string(trials));
}

// A calibration's settings go into a card's draft: flags after the card's arguments, rows as typed values; the card's
// own flag or row stays.
void a_calibration_applies_flags_and_rows() {
  using secure_frontend::ConfigDraftEntry;
  using secure_frontend::ConfigSource;
  secure_frontend::FrontendConfigDraft draft;
  draft.entries.emplace("engine.args", ConfigDraftEntry{std::vector<std::string>{"--cpu-threads", "4"}, ConfigSource::file});
  draft.entries.emplace("glm.cpu_lane", ConfigDraftEntry{std::int64_t{5}, ConfigSource::file});
  expect(secure_frontend::apply_calibration(draft, {"--cpu-threads", "8", "--pcie-frac", "0.35", "glm.pcie_share", "0.25", "glm.cpu_lane", "6"}),
         "applied");
  const auto* args = std::get_if<std::vector<std::string>>(&draft.entries.at("engine.args").value);
  expect(args != nullptr && *args == std::vector<std::string>{"--cpu-threads", "4", "--pcie-frac", "0.35"},
         "the flags after the card's own, which stay: " + (args ? joined(*args) : std::string{}));
  const auto share = draft.entries.find("glm.pcie_share");
  expect(share != draft.entries.end() && std::holds_alternative<double>(share->second.value) && std::get<double>(share->second.value) == 0.25,
         "a decimal row as a decimal");
  expect(std::get<std::int64_t>(draft.entries.at("glm.cpu_lane").value) == 5, "the card's own row stays");
  secure_frontend::FrontendConfigDraft fresh;
  expect(secure_frontend::apply_calibration(fresh, {"--pool-workers", "7", "glm.cpu_lane", "6"}) &&
             std::get<std::vector<std::string>>(fresh.entries.at("engine.args").value) == std::vector<std::string>{"--pool-workers", "7"} &&
             std::get<std::int64_t>(fresh.entries.at("glm.cpu_lane").value) == 6,
         "engine.args made when the card has none; an integer row as an integer");
  secure_frontend::FrontendConfigDraft odd;
  odd.entries.emplace("engine.args", ConfigDraftEntry{std::string{"--x"}, ConfigSource::file});
  expect(!secure_frontend::apply_calibration(odd, {"--cpu-threads", "8"}), "engine.args that is not a list");
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

// Every sweep's settings read back from the file, each value within what its engine takes; anything else is refused.
void the_file_takes_every_sweeps_settings() {
  const auto file = [](const std::vector<std::string>& args) {
    return secure_frontend::render_calibration(secure_frontend::CalibrationResult{
        .card = "c", .args = args, .baseline_tok_s = 1.0, .best_tok_s = 1.0, .date = "2026-10-28T00:00:00Z", .auto_recalibrate = false, .key = key()});
  };
  for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
           {"--cpu-lane", "on"}, {"--cpu-threads", "9"}, {"--pcie-frac", "0.35"}, {"--pcie-frac", "1"}, {"--spec-min-p", "0.3"},
           {"--pool-workers", "7"}, {"--adapt-every", "1", "--adapt-swaps", "160", "--adapt-decay", "0.97"},
           {"glm.pcie_share", "0.1"}, {"glm.pcie_share", "0"}, {"glm.cpu_lane", "9"}}) {
    const auto back = secure_frontend::parse_calibration(file(args));
    expect(back.has_value() && back->args == args, "kept: " + joined(args));
  }
  for (const std::vector<std::string>& args : std::vector<std::vector<std::string>>{
           {"glm.cpu_lane", "65"}, {"glm.cpu_lane", "0"}, {"--adapt-decay", "1.5"}, {"--pool-workers", "0"}, {"glm.kv", "int8"},
           {"--pcie-frac", "0.1234"}, {"--pcie-frac", "-0.1"}, {"--spec-min-p", "abc"}, {"--adapt-swaps", "100000"}, {"--tp", "2"}}) {
    expect(!secure_frontend::parse_calibration(file(args)).has_value(), "refused: " + joined(args));
  }
}

// MiniMax's prefill calibration: the record round-trips, and the engine's own JSON (mm3_main.cpp calibrate_prefill)
// gives the kept chunk, GEMMs and speeds.
void prefill_records_round_trip() {
  const secure_frontend::PrefillRecord record{.card = "MiniMax-M3.1", .date = "2026-10-28T00:00:00Z", .finished = true, .chunk = 2048,
                                              .gemm = "blas", .tok_s = 950.5, .default_tok_s = 900.25, .key = key()};
  const auto back = secure_frontend::parse_prefill(secure_frontend::render_prefill(record));
  expect(back.has_value() && back->card == record.card && back->finished && back->chunk == 2048 && back->gemm == "blas" &&
             back->tok_s == 950.5 && back->default_tok_s == 900.25 && back->key.model == key().model,
         "a prefill record reads back as written: " + secure_frontend::render_prefill(record));
  expect(!secure_frontend::parse_prefill("{}").has_value() && !secure_frontend::parse_prefill("[1]").has_value(), "a damaged record is none");
  secure_frontend::PrefillRecord measured;
  const std::string engine_json =
      R"({"key": "NVIDIA GeForce RTX 4080 SUPER cc 8.9 cuda 13000 driver 581.57", "file": "C:\\packs\\m\\prefill-tune.txt", )"
      R"("chunk": 4096, "gemm": "blas", "tok_s": 1201.3, "default_tok_s": 1100.0, "tokens": 4096, "all_resident": false, )"
      R"("lt_available": false, "seconds": 61, "trials": [{"chunk": 4096, "gemm": "blas", "tok_s": 1201.3}]})";
  expect(secure_frontend::read_prefill_measurement(engine_json, measured) && measured.chunk == 4096 && measured.gemm == "blas" &&
             measured.tok_s == 1201.3 && measured.default_tok_s == 1100.0,
         "the engine's measurement is read");
  for (const std::string& bad : {std::string{"{}"}, std::string{R"({"chunk": 0, "gemm": "blas", "tok_s": 1, "default_tok_s": 1})"},
                                 std::string{R"({"chunk": 1024, "gemm": "cublaslt", "tok_s": 1, "default_tok_s": 1})"}, std::string{"not json"}}) {
    expect(!secure_frontend::read_prefill_measurement(bad, measured), "not the engine's measurement: " + bad);
  }
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
      {"joint_steps_and_longer_answers", joint_steps_and_longer_answers},
      {"a_step_ends_below_its_floor", a_step_ends_below_its_floor},
      {"a_calibration_applies_flags_and_rows", a_calibration_applies_flags_and_rows},
      {"the_file_takes_every_sweeps_settings", the_file_takes_every_sweeps_settings},
      {"prefill_records_round_trip", prefill_records_round_trip},
      {"results_round_trip_and_keys_compare", results_round_trip_and_keys_compare},
      {"the_key_reads_files_and_options", the_key_reads_files_and_options},
      {"physical_cores_count_every_core", physical_cores_count_every_core},
  }, failures);
}
