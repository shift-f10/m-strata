#pragma once

// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §5): calibration, the way upstream
// Strata (tools/calibrate.py) and Project Maya (calibrate_glm.py) do it - a decode-speed sweep of the engine flags that
// depend on the machine, one engine start per candidate, a setting kept only when it is clearly faster (3%), the
// winner confirmed against the defaults in interleaved rounds - and the result kept per card with the key it was
// measured under. Pure logic here (the sweep over a measure function, the plan per engine, the result file and its
// key); the launcher runs the measurements (model_launcher.hpp).

#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/model_options.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// One flag the sweep tries: each value a candidate; the flag's absence (the engine's own default) is the baseline.
struct SweepStep final {
  std::string flag;
  std::vector<std::string> values;
};
// flint: --cpu-lane {off, on}, --pcie-frac {0, .25, .5, .75}, --cpu-threads {all, 2/3, 1/2 of the logical cores};
// gemma4: --cpu-threads (the flags its engine and allowlist take). Every other engine: none here yet (empty).
[[nodiscard]] std::vector<SweepStep> sweep_plan(EngineProfile profile, unsigned logical_cores);

struct SweepProgress final {
  std::string stage;                 // "defaults", the flag being tried, or "confirming"
  std::size_t trial{0};              // 1-based, the trial now measured
  std::size_t trials{0};             // all of them
  std::optional<double> best_tok_s;  // the best decode speed so far
};
struct SweepOutcome final {
  enum class End { done, cancelled, failed } end{End::done};
  std::vector<std::string> args;  // the kept flags and values, in the plan's order (empty: the defaults won)
  double baseline_tok_s{0.0};
  double best_tok_s{0.0};         // the kept setting's (the baseline's when nothing was kept)
};
// One engine start with these extra flags and its median decode tok/s over the workload; nullopt when it failed.
using SweepMeasure = std::function<std::optional<double>(const std::vector<std::string>& args)>;
// The defaults first (failed: the sweep fails), then each step from the best so far: a candidate replaces the best
// only when more than `keep_margin` faster; a candidate that fails is dropped. A winner other than the defaults is
// then measured against them in `confirm_rounds` interleaved rounds (defaults, winner, ...) and kept only when its
// median stays more than `keep_margin` ahead. `cancelled` is asked before every trial.
[[nodiscard]] SweepOutcome run_sweep(const std::vector<SweepStep>& plan, const SweepMeasure& measure,
                                     const std::function<void(const SweepProgress&)>& progress,
                                     const std::function<bool()>& cancelled, double keep_margin = 0.03,
                                     unsigned confirm_rounds = 2U);

// What a calibration was measured under (§5.2).
struct CalibrationKey final {
  std::string gpu;          // GPU name and memory; empty when it could not be read (then never a change)
  std::string cpu;          // the CPU's name and logical threads
  std::uint64_t ram_bytes{0};
  std::string engine;       // file_fact of the engine
  std::string model;        // file_fact of the model file
  std::string options;      // options_fact of the start's option values
};
enum class CalibrationState { missing, fresh, stale, settings_changed };
// stale: the CPU, RAM, engine or model changed, or the GPU did (both known); settings_changed: only the options did.
[[nodiscard]] CalibrationState compare_key(const CalibrationKey& stored, const CalibrationKey& now);

struct CalibrationResult final {
  std::string card;
  std::vector<std::string> args;  // flag, value pairs of the sweep's own flags only
  double baseline_tok_s{0.0};
  double best_tok_s{0.0};
  std::string date;               // UTC, YYYY-MM-DDTHH:MM:SSZ
  bool auto_recalibrate{false};   // "Recalibrate when settings change": a start at other options calibrates first
  CalibrationKey key;
};
[[nodiscard]] std::string render_calibration(const CalibrationResult& result);
// Bounded strict JSON; nullopt for anything else, or args that are not the sweep's flags with valid values.
[[nodiscard]] std::optional<CalibrationResult> parse_calibration(std::string_view text);

// "<size>:<modification time>" of a file; empty when it cannot be read.
[[nodiscard]] std::string file_fact(const std::filesystem::path& file);
// The CPU's name and its logical threads ("<name>, <n> threads").
[[nodiscard]] std::string cpu_fact();
// The option values that change the decode speed, "name=value" by name joined with ';' (thinking, the sampling
// defaults and web search left out).
[[nodiscard]] std::string options_fact(const ModelOptionValues& values);
// The current UTC time as YYYY-MM-DDTHH:MM:SSZ.
[[nodiscard]] std::string utc_now();

}  // namespace secure_frontend
