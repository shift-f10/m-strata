#pragma once

// A calibration's report for a bug report (owner 2026-10-30: "When calibrating a model and an error occurs, it should
// create a detailed message of the errors encountered along with a detailed list of the hardware, that way a user can
// submit this as a report for bug fixes"). The launcher keeps a record of each trial of a calibration - its setting, its
// speed or why it failed, the server's lines for its engine (how each generation ended, with its exit code) and the
// engine's last lines of output - and, when a trial failed, renders them with this PC's hardware and the engine
// folder's build record as Markdown (a GitHub issue takes it as it is), written to <roots.logs>/calibration-reports.
// Never a key, a login link, a user or a host name; a path under the model, data, engine or home folder is shortened to
// a placeholder (scrub_paths). Pure logic here; the launcher collects the trials, the server the hardware.

#include "secure_frontend/about_info.hpp"
#include "secure_frontend/engine_process.hpp"
#include "secure_frontend/host_readings.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// How much of a failed trial a report keeps: the engine's last lines, each cut to a length, and the server's lines.
inline constexpr std::size_t kReportEngineLines = 40U;
inline constexpr std::size_t kReportLineChars = 400U;
inline constexpr std::size_t kReportEventLines = 20U;
// At most this many trials are told in full (every trial still has its row).
inline constexpr std::size_t kReportFailedTrials = 10U;

// One engine start of a calibration.
struct CalibrationTrialRecord final {
  std::size_t number{0};                 // 1-based, as the progress counts them
  std::string stage;                     // "defaults", the setting tried, "confirming", or "prompt reading"
  std::vector<std::string> args;         // the start's extra settings (flag or row, value pairs)
  unsigned max_tokens{0};                // the answers' length it measured
  std::optional<double> tok_s;           // its median decode speed; nullopt: it did not measure one
  std::string failure;                   // why it failed (empty: it did not)
  std::vector<std::string> events;       // the server's lines for its engine, oldest first
  std::vector<std::string> engine_tail;  // the engine's last lines of output, oldest first
};

// One GPU as nvidia-smi tells it for a report.
struct ReportGpu final {
  unsigned index{0};
  std::string name;
  std::optional<std::uint64_t> mem_total_bytes{};
  std::optional<std::uint64_t> mem_used_bytes{};
  std::string driver;        // the driver's version
  std::string compute_cap;   // "8.9"
  std::optional<unsigned> pcie_gen_max{};
  std::optional<unsigned> pcie_width_max{};
  std::optional<double> power_limit_w{};
};
// What nvidia-smi answers for a report, in this order (csv, no header, no units).
inline constexpr std::string_view kNvidiaSmiReportQuery =
    "index,name,memory.total,memory.used,driver_version,compute_cap,pcie.link.gen.max,pcie.link.width.max,power.limit";
// The fixed argv after the executable: one answer, every GPU.
[[nodiscard]] std::vector<std::string> nvidia_smi_report_arguments();
// The answer's lines: nine comma-separated fields, the index 0-63, the name, driver and compute capability printable
// ASCII (a placeholder such as "[N/A]" leaves a reading empty); any other line, and a repeated index, is skipped; at
// most 16 GPUs.
[[nodiscard]] std::vector<ReportGpu> parse_nvidia_smi_report(std::string_view text);
// Runs `command` (nvidia_smi_path() with nvidia_smi_report_arguments() and nvidia_smi_environment()) once and reads
// its answer, for at most 10 s; empty when the tool is missing, does not start or tells nothing.
[[nodiscard]] std::vector<ReportGpu> report_gpus(EngineCommand command);

// The PC a report was made on.
struct ReportMachine final {
  HardwareFacts hardware;                // the CPU and its threads, the RAM, the OS (gpus: the GPU list)
  unsigned physical_cores{0};
  std::optional<MemoryReading> memory{}; // the RAM in use when the report was made
  std::vector<ReportGpu> gpus{};         // nvidia-smi's; empty: it could not be read (hardware.gpus then stands in)
  std::optional<EngineBuild> build{};    // the engine folder's build record
};

struct CalibrationReport final {
  std::string date;                      // UTC, when it was made
  std::string card;
  std::string family;                    // the card's profile
  std::string model_file;                // the model's file name (never its folder)
  std::optional<std::uint64_t> model_bytes{};
  std::string options;                   // the options it was calibrated at (options_fact)
  std::string started;                   // UTC
  std::string outcome;                   // how the calibration ended, in words
  std::vector<CalibrationTrialRecord> trials;
  ReportMachine machine;
};
// The report as Markdown: what happened, each trial, each failed one in full, the hardware and the build.
[[nodiscard]] std::string render_calibration_report(const CalibrationReport& report);
// Whether a report is worth making: a trial failed.
[[nodiscard]] bool report_needed(const std::vector<CalibrationTrialRecord>& trials) noexcept;

// A folder a path in an engine line is shortened to (its placeholder, such as "<model folder>" or "~").
struct PathAlias final {
  std::string folder;
  std::string name;
};
// `line` with each alias's folder replaced by its name, the longest folder first. Windows: either separator, and any
// case; elsewhere byte for byte.
[[nodiscard]] std::string scrub_paths(std::string_view line, const std::vector<PathAlias>& aliases);
// The report's file name: calibration-<card>-<YYYYMMDD>-<HHMMSS>.md, the card's name with every character other than
// a letter, a digit, '.', '-' or '_' made '_' (at most 80 of them).
[[nodiscard]] std::string report_file_name(std::string_view card, std::string_view utc);

}  // namespace secure_frontend
