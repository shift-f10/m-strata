#include "secure_frontend/host_readings.hpp"

#include "secure_frontend/engine_stdio_process.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace secure_frontend {
namespace {

constexpr std::size_t kNvidiaSmiFields = 10U;
constexpr std::size_t kMaximumGpuNameBytes = 96U;
// nvidia-smi answers once a second; this long without a valid line and the reading is stale.
constexpr std::chrono::seconds kGpuStaleAfter{5};

std::string_view trim(std::string_view text) {
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1U);
  while (!text.empty() && text.back() == ' ') text.remove_suffix(1U);
  return text;
}

// One numeric field: a plain non-negative decimal (digits and at most one dot), or nvidia-smi's
// "[N/A]" / "[Not Supported]" (an empty reading). Anything else refuses the whole line.
struct Field final {
  bool valid{false};
  std::optional<double> value{};
};
Field numeric_field(const std::string_view raw) {
  const std::string_view text = trim(raw);
  if (text == "[N/A]" || text == "[Not Supported]") return {true, std::nullopt};
  if (text.empty() || text.front() < '0' || text.front() > '9') return {};
  bool dot = false;
  for (const char character : text) {
    if (character == '.') {
      if (dot) return {};
      dot = true;
    } else if (character < '0' || character > '9') {
      return {};
    }
  }
  double value = 0.0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::fixed);
  if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return {};
  return {true, value};
}

std::optional<std::uint64_t> mebibytes(const std::optional<double> value) {
  if (!value.has_value()) return std::nullopt;
  return static_cast<std::uint64_t>(std::llround(*value * 1048576.0));
}

}  // namespace

std::optional<GpuReading> parse_nvidia_smi_line(const std::string_view line) {
  std::vector<std::string_view> fields;
  std::size_t begin = 0;
  for (;;) {
    const auto comma = line.find(',', begin);
    fields.push_back(line.substr(begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin));
    if (comma == std::string_view::npos) break;
    begin = comma + 1U;
  }
  if (fields.size() != kNvidiaSmiFields) return std::nullopt;
  GpuReading reading;
  const std::string_view name = trim(fields[0]);
  if (name.empty() || name.size() > kMaximumGpuNameBytes) return std::nullopt;
  for (const unsigned char character : name) {
    if (character < 0x20U || character > 0x7eU) return std::nullopt;
  }
  reading.name = std::string{name};
  std::optional<double> values[kNvidiaSmiFields - 1U];
  for (std::size_t index = 1; index < kNvidiaSmiFields; ++index) {
    const Field field = numeric_field(fields[index]);
    if (!field.valid) return std::nullopt;
    values[index - 1U] = field.value;
  }
  const auto whole = [](const std::optional<double> value, std::optional<unsigned>& out) {
    if (!value.has_value()) return true;
    if (*value != std::floor(*value) || *value > 1024.0) return false;
    out = static_cast<unsigned>(*value);
    return true;
  };
  reading.util_percent = values[0];
  reading.mem_used_bytes = mebibytes(values[1]);
  reading.mem_total_bytes = mebibytes(values[2]);
  reading.temp_c = values[3];
  reading.power_w = values[4];
  reading.power_limit_w = values[5];
  if (!whole(values[6], reading.pcie_gen) || !whole(values[7], reading.pcie_gen_max) || !whole(values[8], reading.pcie_width)) {
    return std::nullopt;
  }
  return reading;
}

std::vector<std::string> nvidia_smi_list_arguments() {
  return {"--query-gpu=" + std::string{kNvidiaSmiListQuery}, "--format=csv,noheader,nounits"};
}

std::vector<GpuListing> parse_nvidia_smi_gpu_list(const std::string_view text) {
  constexpr std::size_t kMaximumListedGpus = 16U;
  std::vector<GpuListing> listed;
  std::size_t begin = 0;
  while (begin < text.size() && listed.size() < kMaximumListedGpus) {
    const std::size_t newline = text.find('\n', begin);
    std::string_view line = text.substr(begin, newline == std::string_view::npos ? std::string_view::npos : newline - begin);
    begin = newline == std::string_view::npos ? text.size() : newline + 1U;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
    const std::size_t first = line.find(',');
    const std::size_t second = first == std::string_view::npos ? first : line.find(',', first + 1U);
    if (second == std::string_view::npos || line.find(',', second + 1U) != std::string_view::npos) continue;
    const std::string_view index_text = trim(line.substr(0, first));
    const std::string_view name = trim(line.substr(first + 1U, second - first - 1U));
    if (index_text.empty() || index_text.size() > 2U ||
        !std::all_of(index_text.begin(), index_text.end(), [](const char c) { return c >= '0' && c <= '9'; })) {
      continue;
    }
    const unsigned index = static_cast<unsigned>(std::stoul(std::string{index_text}));
    if (index > 63U || name.empty() || name.size() > kMaximumGpuNameBytes ||
        !std::all_of(name.begin(), name.end(), [](const unsigned char c) { return c >= 0x20U && c <= 0x7eU; })) {
      continue;
    }
    const Field memory = numeric_field(line.substr(second + 1U));
    if (!memory.valid) continue;
    if (std::any_of(listed.begin(), listed.end(), [index](const GpuListing& gpu) { return gpu.index == index; })) continue;
    listed.push_back(GpuListing{index, std::string{name}, mebibytes(memory.value)});
  }
  return listed;
}

std::vector<GpuListing> list_gpus(EngineCommand command) {
  std::error_code ec;
  if (command.executable.empty() || !std::filesystem::is_regular_file(command.executable, ec) || ec) return {};
  EngineStdioProcess child;
  if (child.start(command).has_value()) return {};
  std::string text;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < until && text.size() < 16384U) {
    const EngineReadResult read = child.read_line(std::chrono::milliseconds(250));
    if (read.kind == EngineReadKind::line) text += read.line + "\n";
    else if (read.kind != EngineReadKind::timeout) break;   // the answer is complete: the tool closed its output
  }
  (void)child.terminate(std::chrono::milliseconds(500));
  return parse_nvidia_smi_gpu_list(text);
}

std::filesystem::path nvidia_smi_path() {
#if defined(_WIN32)
  // As default_downloader_path: a failure or a too-long directory never yields a bare name.
  std::wstring system(MAX_PATH, L'\0');
  for (int attempt = 0; attempt < 2; ++attempt) {
    const UINT length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
    if (length == 0) return {};
    if (length < system.size()) {
      system.resize(length);
      const std::filesystem::path directory(system);
      if (!directory.is_absolute()) return {};
      return directory / L"nvidia-smi.exe";
    }
    system.assign(length, L'\0');
  }
  return {};
#else
  return std::filesystem::path{"/usr/bin/nvidia-smi"};
#endif
}

std::vector<std::string> nvidia_smi_arguments() {
  return {"--id=0", "--query-gpu=" + std::string{kNvidiaSmiQuery}, "--format=csv,noheader,nounits", "--loop-ms=1000"};
}

std::map<std::string, std::string, std::less<>> nvidia_smi_environment() {
#if defined(_WIN32)
  // HKLM is administrators' to write; this process's own environment is whoever started it.
  wchar_t buffer[MAX_PATH + 1]{};
  DWORD size = sizeof(buffer);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion", L"ProgramFilesDir", RRF_RT_REG_SZ,
                   nullptr, buffer, &size) != ERROR_SUCCESS) {
    return {};
  }
  const std::filesystem::path directory{std::wstring{buffer}};
  std::error_code ec;
  if (!directory.is_absolute() || !std::filesystem::is_directory(directory, ec) || ec) return {};
  const std::wstring wide = directory.native();
  const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (length <= 0) return {};
  std::string text(static_cast<std::size_t>(length), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), text.data(), length, nullptr, nullptr) != length) {
    return {};
  }
  return {{"ProgramFiles", text}};
#else
  return {};
#endif
}

std::optional<double> CpuMeter::sample() {
  std::uint64_t idle = 0;
  std::uint64_t total = 0;
#if defined(_WIN32)
  FILETIME idle_time{};
  FILETIME kernel_time{};
  FILETIME user_time{};
  if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) return std::nullopt;
  const auto ticks = [](const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
  };
  idle = ticks(idle_time);
  total = ticks(kernel_time) + ticks(user_time);  // kernel time includes the idle time
#else
  std::ifstream stat("/proc/stat");
  std::string label;
  std::uint64_t fields[8]{};
  if (!(stat >> label) || label != "cpu") return std::nullopt;
  for (auto& field : fields) {
    if (!(stat >> field)) return std::nullopt;
  }
  idle = fields[3] + fields[4];  // idle + iowait
  for (const auto field : fields) total += field;
#endif
  std::optional<double> load;
  if (last_idle_.has_value() && last_total_.has_value() && total > *last_total_ && idle >= *last_idle_) {
    const double spent = static_cast<double>(total - *last_total_);
    const double resting = static_cast<double>(idle - *last_idle_);
    load = std::clamp(100.0 * (spent - resting) / spent, 0.0, 100.0);
  }
  last_idle_ = idle;
  last_total_ = total;
  return load;
}

std::optional<MemoryReading> read_memory() {
#if defined(_WIN32)
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status) || status.ullTotalPhys == 0U || status.ullAvailPhys > status.ullTotalPhys) return std::nullopt;
  return MemoryReading{.used_bytes = status.ullTotalPhys - status.ullAvailPhys, .total_bytes = status.ullTotalPhys};
#else
  std::ifstream meminfo("/proc/meminfo");
  std::optional<std::uint64_t> total_kib;
  std::optional<std::uint64_t> available_kib;
  for (std::string line; std::getline(meminfo, line) && !(total_kib && available_kib);) {
    std::istringstream fields(line);
    std::string key;
    std::uint64_t value = 0;
    if (!(fields >> key >> value)) continue;
    if (key == "MemTotal:") total_kib = value;
    if (key == "MemAvailable:") available_kib = value;
  }
  if (!total_kib || !available_kib || *total_kib == 0U || *available_kib > *total_kib) return std::nullopt;
  return MemoryReading{.used_bytes = (*total_kib - *available_kib) * 1024U, .total_bytes = *total_kib * 1024U};
#endif
}

GpuSampler::~GpuSampler() { stop(); }

void GpuSampler::start(EngineCommand command) {
  if (thread_.joinable()) return;
  stopping_ = false;
  set_note("starting");
  thread_ = std::thread([this, command = std::move(command)]() mutable { run(std::move(command)); });
}

void GpuSampler::stop() {
  stopping_ = true;
  if (thread_.joinable()) thread_.join();
}

std::optional<GpuReading> GpuSampler::latest() const {
  std::lock_guard lock(mutex_);
  return latest_;
}

std::string GpuSampler::note() const {
  std::lock_guard lock(mutex_);
  return note_;
}

void GpuSampler::run(EngineCommand command) {
  std::error_code ec;
  if (command.executable.empty() || !std::filesystem::is_regular_file(command.executable, ec) || ec) {
    set_note("nvidia-smi is not installed");
    return;
  }
  EngineStdioProcess child;
  if (child.start(command).has_value()) {
    set_note("nvidia-smi did not start");
    return;
  }
  auto last_valid = std::chrono::steady_clock::now();
  while (!stopping_.load()) {
    const EngineReadResult read = child.read_line(std::chrono::milliseconds(250));
    if (read.kind == EngineReadKind::line) {
      if (auto reading = parse_nvidia_smi_line(read.line)) {
        last_valid = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex_);
        latest_ = std::move(reading);
        note_.clear();
      }
    } else if (read.kind != EngineReadKind::timeout) {  // closed or failed: the tool is gone
      std::lock_guard lock(mutex_);
      latest_.reset();
      note_ = "nvidia-smi stopped";
      break;
    }
    if (std::chrono::steady_clock::now() - last_valid > kGpuStaleAfter) {
      std::lock_guard lock(mutex_);
      latest_.reset();
      note_ = "nvidia-smi is not answering";
    }
  }
  (void)child.terminate(std::chrono::milliseconds(500));
  if (stopping_.load()) set_note("stopped");
}

void GpuSampler::set_note(std::string note) {
  std::lock_guard lock(mutex_);
  note_ = std::move(note);
}

}  // namespace secure_frontend
