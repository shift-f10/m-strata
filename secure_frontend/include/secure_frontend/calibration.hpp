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

// One setting the sweep tries: each value a candidate; the setting's absence (the engine's own default) is the
// baseline. A setting is an engine flag ("--pcie-frac") or a card config row the engine reads through its environment
// ("glm.pcie_share": no leading "--"). A joint step sets `flag` and the `joint` flags together, each candidate their
// values space-separated in that order (upstream's expert tier: --adapt-every, --adapt-swaps, --adapt-decay).
struct SweepStep final {
  std::string flag;
  std::vector<std::string> values;
  std::vector<std::string> joint{};
  unsigned max_tokens{128U};  // the answers' length the step's candidates are measured with
  double stop_below{0.0};     // the step ends after a candidate slower than this share of its best so far (Maya's)
};
// The machine a plan is for.
struct SweepMachine final {
  unsigned logical_cores{1U};
  unsigned physical_cores{1U};
};
// Each engine's own sweep (one engine start per candidate):
//  - flint: --cpu-lane {off, on}, --pcie-frac {0, .25, .5, .75}, --cpu-threads {all, 2/3, 1/2 of the logical cores};
//  - gemma4: --cpu-threads (the flags its engine and allowlist take);
//  - qwen, upstream's tools/calibrate.py: --pcie-frac {0, .2, .35, .55, .75, .9, 1}, --spec-min-p {.3, .5, .7}, fewer CPU
//    pool workers than the engine's own (every physical core but the host's: 2/3, 1/2, 1/4, at least 2), then the
//    adaptive expert tier (every 1 window, 80 or 160 swaps, decay .97) on answers of 512 tokens;
//  - glm53 and raon1, Maya's tools/calibrate_glm.py through the GLM engine's environment rows: glm.pcie_share
//    {0, .1, .25, .5, .75}, then glm.cpu_lane, fewer threads than the engine's own (3/4, 2/3, 1/2, 1/4), each step ending
//    below 70% of its best;
//  - deepseek and minimax, Maya's tools/calibrate_ds4_tp.py and tools/mm3/calibrate.py: --cpu-threads, fewer than the
//    engine's own (the physical cores; 2 and 4 fewer first on a CPU without SMT siblings, then 3/4 and 1/2), the step
//    ending at the first that gains nothing.
[[nodiscard]] std::vector<SweepStep> sweep_plan(EngineProfile profile, const SweepMachine& machine);
// The margin a candidate must win by: 5% for deepseek and minimax (Maya's MIN_GAIN for separate engine starts), 3% else.
[[nodiscard]] double sweep_keep_margin(EngineProfile profile) noexcept;
// Whether a card the page made calibrates before its first start: flint and gemma4 (a few short starts). The others'
// sweeps take many starts of a large model, so they run when asked (Calibrate), as upstream's and Maya's do.
[[nodiscard]] bool calibrates_before_first_start(EngineProfile profile) noexcept;
// This machine's physical CPU cores (each counted once, SMT siblings and all); the logical CPUs when unknown.
[[nodiscard]] unsigned physical_cores();
// A calibration's settings (flag or row, value pairs) put into a card's draft: flags appended to engine.args, rows set
// with their typed values; a flag or row the card sets itself stays the card's. False: engine.args is not a list.
[[nodiscard]] bool apply_calibration(FrontendConfigDraft& draft, const std::vector<std::string>& settings);

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
// One engine start with these extra settings and its median decode tok/s over the workload, its answers `max_tokens`
// long; nullopt when it failed.
using SweepMeasure = std::function<std::optional<double>(const std::vector<std::string>& args, unsigned max_tokens)>;
// The defaults first (failed: the sweep fails), then each step from the best so far: a candidate replaces the best
// only when more than `keep_margin` faster; a candidate that fails is dropped; a step with `stop_below` ends after a
// candidate below that share of the step's best. A winner other than the defaults is then measured against them in
// `confirm_rounds` interleaved rounds (defaults, winner, ...; answers as long as its steps' longest) and kept only when
// its median stays more than `keep_margin` ahead. `cancelled` is asked before every trial.
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

// MiniMax's prefill calibration (Maya's tools/calibrate_mm3_prefill.py over the engine's own --calibrate-prefill: the
// chunk a prompt is read in and the dense GEMMs' backend, kept by the engine in the pack's prefill-tune.txt), as this
// server ran it for a card: when, whether the engine finished, what it kept, and the key it was measured under.
struct PrefillRecord final {
  std::string card;
  std::string date;
  bool finished{false};  // false: the engine stopped first (its defaults stay; Calibrate measures again)
  std::int64_t chunk{0};
  std::string gemm;      // "blas" or "hipblaslt"
  double tok_s{0.0};
  double default_tok_s{0.0};
  CalibrationKey key;
};
[[nodiscard]] std::string render_prefill(const PrefillRecord& record);
// Bounded strict JSON; nullopt for anything else.
[[nodiscard]] std::optional<PrefillRecord> parse_prefill(std::string_view text);
// The engine's --calibrate-prefill output ({"chunk", "gemm", "tok_s", "default_tok_s", ...}) into `record`; false when
// it is not that JSON.
[[nodiscard]] bool read_prefill_measurement(std::string_view text, PrefillRecord& record);

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
