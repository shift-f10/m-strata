#pragma once

// The Monitor's readings of this machine (listener.monitor; owner request 2026-10-22, session
// log step 16): CPU and RAM from the operating system, the GPU from NVIDIA's own nvidia-smi run
// as a separate child, from the system directory only (Windows GetSystemDirectoryW, Linux
// /usr/bin), with a fixed argument list, no shell and a fixed environment (Windows: ProgramFiles
// from HKLM, which NVML needs; nothing inherited). No vendor library
// is loaded into this process (upstream reads NVML in-process, serve/telemetry.py), and nothing
// here touches a network.

#include "secure_frontend/engine_process.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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
