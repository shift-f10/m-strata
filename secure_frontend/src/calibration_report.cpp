#include "secure_frontend/calibration_report.hpp"

#include "secure_frontend/engine_stdio_process.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <utility>

namespace secure_frontend {
namespace {

std::string_view trimmed(std::string_view text) {
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}

bool placeholder(const std::string_view field) { return field.size() >= 2U && field.front() == '[' && field.back() == ']'; }

bool printable(const std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](const char c) { return c >= 0x20 && c < 0x7F; });
}

// A field nvidia-smi answered: nullopt for a placeholder, else the number (a field that is neither refuses the line).
template <typename Number>
bool read_number(const std::string_view field, std::optional<Number>& out) {
  if (placeholder(field)) return true;
  Number value{};
  const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), value);
  if (error != std::errc{} || end != field.data() + field.size()) return false;
  out = value;
  return true;
}

// printable text of at most `limit` characters, or empty for a placeholder; false for anything else
bool read_text(const std::string_view field, const std::size_t limit, std::string& out) {
  if (placeholder(field)) return true;
  if (field.empty() || field.size() > limit || !printable(field)) return false;
  out = std::string{field};
  return true;
}

std::string decimal(const double value, const int digits) {
  char text[64];
  (void)std::snprintf(text, sizeof text, "%.*f", digits, value);
  return text;
}

std::string gib(const std::uint64_t bytes) { return decimal(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 1) + " GiB"; }

std::string mib(const std::uint64_t bytes) { return std::to_string(bytes >> 20U) + " MiB"; }

std::string watts(const double value) {
  return (std::fabs(value - std::round(value)) < 0.05 ? std::to_string(static_cast<long long>(std::llround(value))) : decimal(value, 1)) + " W";
}

std::string joined(const std::vector<std::string>& parts, const std::string_view separator) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) out += (i == 0U ? "" : std::string{separator}) + parts[i];
  return out;
}

// A table cell: '|' and line breaks would end it.
std::string cell(const std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == '|') out += "\\|";
    else if (c == '\n' || c == '\r') out += ' ';
    else out += c;
  }
  return out;
}

// A line inside a fenced block, cut to kReportLineChars; a run of backticks would close the fence.
std::string fenced_line(const std::string_view line) {
  std::string out{line.substr(0, kReportLineChars)};
  for (std::size_t at = out.find("```"); at != std::string::npos; at = out.find("```", at)) out.replace(at, 3U, "'''");
  if (line.size() > kReportLineChars) out += " ... (cut)";
  return out;
}

void fence(std::string& out, const std::vector<std::string>& lines, const std::size_t last) {
  out += "```text\n";
  for (std::size_t i = lines.size() > last ? lines.size() - last : 0U; i < lines.size(); ++i) out += fenced_line(lines[i]) + "\n";
  out += "```\n";
}

std::string setting(const CalibrationTrialRecord& trial) {
  if (trial.args.empty()) return trial.stage;
  const auto args = joined(trial.args, " ");
  return trial.stage == "confirming" ? "confirming " + args : args;
}

std::string result(const CalibrationTrialRecord& trial) {
  if (!trial.failure.empty()) return "failed: " + trial.failure;
  if (trial.tok_s) return decimal(*trial.tok_s, 2) + " tok/s";
  return "stopped";
}

void hardware_section(std::string& out, const ReportMachine& machine) {
  const auto& hardware = machine.hardware;
  out += "## Hardware\n\n";
  out += "- CPU: " + (hardware.cpu.empty() ? std::string{"unknown"} : hardware.cpu) +
         (machine.physical_cores > 0U ? " (" + std::to_string(machine.physical_cores) + " physical cores)" : std::string{}) + "\n";
  const auto total = machine.memory ? std::optional<std::uint64_t>{machine.memory->total_bytes} : hardware.ram_bytes;
  out += "- RAM: " + (total ? gib(*total) : std::string{"unknown"});
  if (machine.memory && machine.memory->used_bytes <= machine.memory->total_bytes) {
    out += ", " + gib(machine.memory->total_bytes - machine.memory->used_bytes) + " free when the report was made";
  }
  out += "\n";
  if (!machine.gpus.empty()) {
    for (const auto& gpu : machine.gpus) {
      out += "- GPU " + std::to_string(gpu.index) + ": " + gpu.name;
      if (gpu.mem_total_bytes) out += ", " + mib(*gpu.mem_total_bytes);
      if (gpu.mem_used_bytes) out += " (" + mib(*gpu.mem_used_bytes) + " in use)";
      if (!gpu.driver.empty()) out += ", driver " + gpu.driver;
      if (!gpu.compute_cap.empty()) out += ", compute capability " + gpu.compute_cap;
      if (gpu.pcie_gen_max || gpu.pcie_width_max) {
        out += ", PCIe" + (gpu.pcie_gen_max ? " gen " + std::to_string(*gpu.pcie_gen_max) : std::string{}) +
               (gpu.pcie_width_max ? " x" + std::to_string(*gpu.pcie_width_max) : std::string{});
      }
      if (gpu.power_limit_w) out += ", power limit " + watts(*gpu.power_limit_w);
      out += "\n";
    }
  } else if (!hardware.gpus.empty()) {
    for (const auto& gpu : hardware.gpus) {
      out += "- GPU " + std::to_string(gpu.index) + ": " + gpu.name + (gpu.mem_total_bytes ? ", " + mib(*gpu.mem_total_bytes) : std::string{}) + "\n";
    }
  } else {
    out += "- GPU: " + (hardware.gpu.empty() ? std::string{"none could be read (nvidia-smi)"} : hardware.gpu) + "\n";
  }
  out += "- Operating system: " + (hardware.os.empty() ? std::string{"unknown"} : hardware.os) + "\n\n";
}

void build_section(std::string& out, const std::optional<EngineBuild>& build) {
  out += "## Build\n\n";
  if (!build) {
    out += "The engine folder has no build record (make writes engine/build.json).\n";
    return;
  }
  out += "- Built " + build->built + " from commit " + build->commit + "\n";
  out += "- ggml " + build->ggml + ", GPU targets " + build->cuda_arch + "\n";
  out += "- Features: " + (build->features.empty() ? std::string{"none"} : joined(build->features, ", ")) + "\n";
  out += "- Engines:\n";
  for (const auto& engine : build->engines) {
    out += "  - " + engine.library + ": " + engine.models + " (" + engine.source + "), " + std::to_string(engine.bytes) + " bytes\n";
  }
}

#if defined(_WIN32)
char folded(const char c) {
  if (c == '/') return '\\';
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}
#else
char folded(const char c) { return c; }
#endif

bool name_character(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
}

}  // namespace

std::vector<std::string> nvidia_smi_report_arguments() {
  return {"--query-gpu=" + std::string{kNvidiaSmiReportQuery}, "--format=csv,noheader,nounits"};
}

std::vector<ReportGpu> parse_nvidia_smi_report(const std::string_view text) {
  std::vector<ReportGpu> gpus;
  std::set<unsigned> seen;
  std::size_t begin = 0;
  while (begin < text.size() && gpus.size() < 16U) {
    auto end = text.find('\n', begin);
    if (end == std::string_view::npos) end = text.size();
    const auto line = text.substr(begin, end - begin);
    begin = end + 1U;
    std::vector<std::string_view> fields;
    for (std::size_t from = 0;;) {
      const auto comma = line.find(',', from);
      fields.push_back(trimmed(line.substr(from, comma == std::string_view::npos ? std::string_view::npos : comma - from)));
      if (comma == std::string_view::npos) break;
      from = comma + 1U;
    }
    if (fields.size() != 9U) continue;
    ReportGpu gpu;
    std::optional<unsigned> index;
    std::optional<std::uint64_t> total_mib;
    std::optional<std::uint64_t> used_mib;
    if (placeholder(fields[0]) || !read_number(fields[0], index) || !index || *index > 63U || seen.contains(*index)) continue;
    if (placeholder(fields[1]) || !read_text(fields[1], 96U, gpu.name)) continue;
    if (!read_number(fields[2], total_mib) || !read_number(fields[3], used_mib) || !read_text(fields[4], 32U, gpu.driver) ||
        !read_text(fields[5], 8U, gpu.compute_cap) || !read_number(fields[6], gpu.pcie_gen_max) ||
        !read_number(fields[7], gpu.pcie_width_max) || !read_number(fields[8], gpu.power_limit_w)) {
      continue;
    }
    gpu.index = *index;
    if (total_mib) gpu.mem_total_bytes = *total_mib << 20U;
    if (used_mib) gpu.mem_used_bytes = *used_mib << 20U;
    seen.insert(*index);
    gpus.push_back(std::move(gpu));
  }
  return gpus;
}

// As list_gpus: one answer, read until the tool closes its output.
std::vector<ReportGpu> report_gpus(EngineCommand command) {
  std::error_code ec;
  if (command.executable.empty() || !std::filesystem::is_regular_file(command.executable, ec) || ec) return {};
  EngineStdioProcess child;
  if (child.start(command).has_value()) return {};
  std::string text;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < until && text.size() < 16384U) {
    const EngineReadResult read = child.read_line(std::chrono::milliseconds(250));
    if (read.kind == EngineReadKind::line) text += read.line + "\n";
    else if (read.kind != EngineReadKind::timeout) break;
  }
  (void)child.terminate(std::chrono::milliseconds(500));
  return parse_nvidia_smi_report(text);
}

bool report_needed(const std::vector<CalibrationTrialRecord>& trials) noexcept {
  return std::any_of(trials.begin(), trials.end(), [](const CalibrationTrialRecord& trial) { return !trial.failure.empty(); });
}

std::string render_calibration_report(const CalibrationReport& report) {
  std::string out = "# M-Strata calibration report\n\n";
  out += "Made " + report.date + " for a bug report. Look it over before you share it: it lists this PC's hardware, the "
         "model's file name and size, and the engine's own output for the trials that failed. Paths in the model, data, "
         "engine and home folders are shortened to <model folder>, <data>, <engine> and ~; it holds no API key, user name "
         "or host name.\n\n";
  const auto failed = static_cast<std::size_t>(std::count_if(report.trials.begin(), report.trials.end(),
                                                             [](const CalibrationTrialRecord& trial) { return !trial.failure.empty(); }));
  out += "## What happened\n\n";
  out += "- Card: " + report.card + " (" + report.family + ")\n";
  out += "- Model file: " + report.model_file +
         (report.model_bytes ? ", " + std::to_string(*report.model_bytes) + " bytes (" + gib(*report.model_bytes) + ")" : std::string{}) + "\n";
  out += "- Options: " + (report.options.empty() ? std::string{"the card's defaults"} : report.options) + "\n";
  out += "- Started " + report.started + "; this report " + report.date + "\n";
  out += "- Result: " + report.outcome + "\n";
  out += "- Trials: " + std::to_string(report.trials.size()) + ", " + std::to_string(failed) + " failed\n\n";

  out += "## Trials\n\n| # | Setting | Answer tokens | Result |\n| --- | --- | --- | --- |\n";
  for (const auto& trial : report.trials) {
    out += "| " + std::to_string(trial.number) + " | " + cell(setting(trial)) + " | " + std::to_string(trial.max_tokens) + " | " +
           cell(result(trial)) + " |\n";
  }
  out += "\n";
  std::size_t told = 0;
  for (const auto& trial : report.trials) {
    if (trial.failure.empty()) continue;
    if (told == kReportFailedTrials) {
      out += std::to_string(failed - told) + " more failed trials are in the table above.\n\n";
      break;
    }
    ++told;
    out += "### Trial " + std::to_string(trial.number) + " failed: " + setting(trial) + "\n\n";
    out += "Why: " + trial.failure + ".\n\n";
    if (trial.events.empty()) {
      out += "The server said nothing about its engine.\n\n";
    } else {
      out += "The server's lines for its engine:\n\n";
      fence(out, trial.events, kReportEventLines);
      out += "\n";
    }
    if (trial.engine_tail.empty()) {
      out += "The engine printed nothing.\n\n";
    } else {
      out += "The engine's last " + std::to_string(std::min(trial.engine_tail.size(), kReportEngineLines)) + " lines:\n\n";
      fence(out, trial.engine_tail, kReportEngineLines);
      out += "\n";
    }
  }
  hardware_section(out, report.machine);
  build_section(out, report.machine.build);
  return out;
}

std::string scrub_paths(const std::string_view line, const std::vector<PathAlias>& aliases) {
  std::vector<std::pair<std::string, const PathAlias*>> folders;
  for (const auto& alias : aliases) {
    if (alias.folder.empty()) continue;
    std::string key = alias.folder;
    while (key.size() > 1U && (key.back() == '\\' || key.back() == '/')) key.pop_back();
    std::transform(key.begin(), key.end(), key.begin(), folded);
    folders.emplace_back(std::move(key), &alias);
  }
  std::stable_sort(folders.begin(), folders.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
  std::string out;
  std::size_t at = 0;
  while (at < line.size()) {
    bool replaced = false;
    for (const auto& [key, alias] : folders) {
      if (line.size() - at < key.size()) continue;
      bool same = true;
      for (std::size_t i = 0; i < key.size() && same; ++i) same = folded(line[at + i]) == key[i];
      // the whole folder: the next character ends its name
      const std::size_t next = at + key.size();
      if (!same || (next < line.size() && name_character(line[next]))) continue;
      out += alias->name;
      at = next;
      replaced = true;
      break;
    }
    if (!replaced) out += line[at++];
  }
  return out;
}

std::string report_file_name(const std::string_view card, const std::string_view utc) {
  std::string name;
  for (const char c : card.substr(0, 80U)) name += name_character(c) ? c : '_';
  std::string digits;
  for (const char c : utc) {
    if (c >= '0' && c <= '9') digits += c;
  }
  digits.resize(14U, '0');
  return "calibration-" + name + "-" + digits.substr(0, 8U) + "-" + digits.substr(8U, 6U) + ".md";
}

}  // namespace secure_frontend
