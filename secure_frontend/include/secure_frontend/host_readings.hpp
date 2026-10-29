#pragma once

// The Monitor's readings of this machine (listener.monitor; owner request 2026-10-22, session
// log step 16): CPU and RAM from the operating system, the GPU from NVIDIA's own nvidia-smi run
// as a separate child, from the system directory only (Windows GetSystemDirectoryW, Linux
// /usr/bin), with a fixed argument list, no shell and a fixed environment (Windows: ProgramFiles
// from HKLM, which NVML needs; nothing inherited). No vendor library
// is loaded into this process (upstream reads NVML in-process, serve/telemetry.py), and nothing
// here touches a network.

#include "secure_frontend/conversation_cache_log.hpp"
#include "secure_frontend/engine_process.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace secure_frontend {

struct GpuReading final {
  std::string name;
  std::optional<double> util_percent{};
  std::optional<std::uint64_t> mem_used_bytes{};
  std::optional<std::uint64_t> mem_total_bytes{};
  std::optional<double> temp_c{};
  std::optional<double> power_w{};
  std::optional<double> power_limit_w{};
  std::optional<unsigned> pcie_gen{};
  std::optional<unsigned> pcie_gen_max{};
  std::optional<unsigned> pcie_width{};
};

// What nvidia-smi answers once a second, in this order (csv, no header, no units).
inline constexpr std::string_view kNvidiaSmiQuery =
    "name,utilization.gpu,memory.used,memory.total,temperature.gpu,power.draw,enforced.power.limit,"
    "pcie.link.gen.current,pcie.link.gen.max,pcie.link.width.current";

// One answer line: exactly ten comma-separated fields (memory in MiB). "[N/A]" or
// "[Not Supported]" leave that reading empty; any other non-number, a name that is not 1-96
// printable ASCII characters, or another field count refuses the whole line (nullopt).
[[nodiscard]] std::optional<GpuReading> parse_nvidia_smi_line(std::string_view line);

// This PC's GPUs (multi-GPU like upstream, owner 2026-10-28: a card's GPUs choice), numbered as nvidia-smi numbers them
// (the numbers --gpu and CUDA_VISIBLE_DEVICES take in PCI bus order).
struct GpuListing final {
  unsigned index{0};
  std::string name;
  std::optional<std::uint64_t> mem_total_bytes{};
};
inline constexpr std::string_view kNvidiaSmiListQuery = "index,name,memory.total";
// The fixed argv after the executable: one answer, every GPU.
[[nodiscard]] std::vector<std::string> nvidia_smi_list_arguments();
// The answer's lines, "<index>, <name>, <MiB>" (the index 0-63, the name as parse_nvidia_smi_line takes it, "[N/A]"
// memory left unread); any other line, and a repeated index, is skipped; at most 16 GPUs.
[[nodiscard]] std::vector<GpuListing> parse_nvidia_smi_gpu_list(std::string_view text);
// Runs `command` (nvidia_smi_path() with nvidia_smi_list_arguments() and nvidia_smi_environment()) once and reads its
// list, for at most 10 s; empty when the tool is missing, does not start or lists nothing.
[[nodiscard]] std::vector<GpuListing> list_gpus(EngineCommand command);

// System32\nvidia-smi.exe or /usr/bin/nvidia-smi; empty when the system directory is unknown.
[[nodiscard]] std::filesystem::path nvidia_smi_path();
// The fixed argv after the executable: GPU 0, the query above, one line every 1000 ms.
[[nodiscard]] std::vector<std::string> nvidia_smi_arguments();
// The child's whole environment. Windows: ProgramFiles alone (without it NVML does not start:
// "Failed to initialize NVML", session log step 16), read from HKLM's ProgramFilesDir, never
// from this process's environment. Elsewhere: empty.
[[nodiscard]] std::map<std::string, std::string, std::less<>> nvidia_smi_environment();

// CPU load over all cores between two calls; the first call has nothing to compare with.
class CpuMeter final {
 public:
  [[nodiscard]] std::optional<double> sample();

 private:
  std::optional<std::uint64_t> last_idle_{};
  std::optional<std::uint64_t> last_total_{};
};

struct MemoryReading final {
  std::uint64_t used_bytes;
  std::uint64_t total_bytes;
};
[[nodiscard]] std::optional<MemoryReading> read_memory();

// Disk reads and writes (owner 2026-10-28; upstream's serve/telemetry.py:586-600 _disk(), which reads psutil's
// disk_io_counters()): this machine's disks' cumulative bytes, read natively as psutil reads them.
struct DiskCounters final {
  std::uint64_t read_bytes{0};
  std::uint64_t write_bytes{0};
};
using DiskDevices = std::map<std::string, DiskCounters, std::less<>>;  // per disk, by its name
// Linux /proc/diskstats: sectors read and written (fields 6 and 10, x512) of the lines with 14 or at least 18 fields
// whose device `whole_disk` accepts (psutil: a name under /sys/block, so a partition is not counted twice); other
// lines are skipped. nullopt when no line counted.
[[nodiscard]] std::optional<DiskDevices> parse_diskstats(std::string_view text, const std::function<bool(std::string_view)>& whole_disk);
// Linux: /proc/diskstats over the devices in /sys/block. Windows: IOCTL_DISK_PERFORMANCE on \\.\PhysicalDrive0-32
// (psutil's own way; a drive that cannot be opened or does not answer is left out). nullopt when no disk answered.
[[nodiscard]] std::optional<DiskDevices> read_disk_counters();
struct DiskRates final {
  double read_mb{0.0};   // MiB/s (upstream: / 2**20)
  double write_mb{0.0};
};
// Rates between two readings, as _disk() (upstream sums psutil's totals first; this sums per disk, so a disk missing
// from one reading - unplugged, or it did not answer - never adds its lifetime count to one second): the disks in both
// readings whose counters did not go back. None for the first reading, a missing reading (the last one is kept), or
// no disk to compare.
class DiskMeter final {
 public:
  [[nodiscard]] std::optional<DiskRates> sample();  // read_disk_counters(), now
  [[nodiscard]] std::optional<DiskRates> next(std::optional<DiskDevices> devices, std::chrono::steady_clock::time_point now);

 private:
  std::optional<std::pair<std::chrono::steady_clock::time_point, DiskDevices>> last_{};
};

// The engine that is up, for the Monitor (EngineSupervisor::engine_sample, once a second): which generation (a restart
// is the next one), its startup readings (the INFO before READY), its process's resident memory (Windows working set,
// Linux VmRSS) and its conversation cache as its stderr told it (the Qwen engine; nullopt for the others).
struct EngineSample final {
  std::uint64_t generation{0};
  std::map<std::string, std::string> readings{};
  std::optional<std::uint64_t> resident_bytes{};
  std::optional<ConversationCacheState> conversation_cache{};
};

// One nvidia-smi child on its own thread, keeping the newest valid line. A tool that is
// missing, fails to start or exits leaves no reading and a note saying why; it is not
// restarted. stop() (and the destructor) ends the child and joins the thread.
class GpuSampler final {
 public:
  GpuSampler() = default;
  ~GpuSampler();
  GpuSampler(const GpuSampler&) = delete;
  GpuSampler& operator=(const GpuSampler&) = delete;

  // The server passes nvidia_smi_path() and nvidia_smi_arguments(); tests pass a fake.
  void start(EngineCommand command);
  void stop();
  [[nodiscard]] std::optional<GpuReading> latest() const;
  [[nodiscard]] std::string note() const;  // empty while readings arrive

 private:
  void run(EngineCommand command);
  void set_note(std::string note);

  mutable std::mutex mutex_;
  std::optional<GpuReading> latest_;
  std::string note_{"starting"};
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};

}  // namespace secure_frontend
