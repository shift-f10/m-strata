#include "secure_frontend/calibration_thermal.hpp"
#include "support/test_support.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

namespace {
using namespace secure_frontend;
int failures = 0;
std::filesystem::path self;
void expect(bool ok, const char* reason) { if (!ok) { ++failures; std::cerr << "FAILED: " << reason << '\n'; } }
std::string read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write(const std::filesystem::path& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }
struct Fixture {
  std::filesystem::path root{secure_frontend_test::make_scratch_directory()}, tool{root / self.filename()};
  Fixture() {
    std::filesystem::copy_file(self, tool);
    std::filesystem::permissions(tool, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    write(root / "thermal.csv", "0, Not Active, Not Active\n1, Active, Not Active\n");
    write(root / "compute.csv", "12\n34\n");
    write(root / "driver.csv", "0, TCC\n1, TCC\n");
  }
  CalibrationGpuSignals sample(std::vector<unsigned> gpus = {0}, int milliseconds = 2000) const {
    return sample_calibration_gpu_with_tool(tool, gpus, std::chrono::milliseconds{milliseconds});
  }
};
void thermal_flags_and_selected_devices() {
  Fixture f;
  auto sample = f.sample();
  expect(sample.thermal_throttled == false, "unselected hot GPU does not contaminate selected GPU");
  expect(sample.compute_processes == 2U, "compute process count from selected GPU");
  expect(f.sample({1}).thermal_throttled == true, "software thermal Active is detected");
  expect(f.sample({0, 1}).compute_processes == 2U, "same PID on multiple GPUs counted once");
  expect(f.sample({}).thermal_throttled == true, "empty device list aggregates every GPU");
  write(f.root / "thermal.csv", "0, Not Active, Active\n");
  expect(f.sample().thermal_throttled == true, "hardware thermal Active is detected");
  write(f.root / "compute.csv", "");
  expect(f.sample().compute_processes == 0U, "successful empty compute query means no active processes");
#if defined(_WIN32)
  write(f.root / "driver.csv", "0, WDDM\n1, TCC\n");
  write(f.root / "compute.csv", "12\n34\n");
  expect(!f.sample().compute_processes, "WDDM desktop process presence does not prove competing compute");
  expect(f.sample({1}).compute_processes == 2U, "selected TCC GPU retains reliable compute checks");
#endif
}
void unavailable_and_malformed_fields() {
  Fixture f;
  for (const auto& text : {"0, [N/A], Not Active\n", "0, 0, 1\n", "0, Active\n", "0, Active, Not Active, extra\n",
                         "0, Not Active, Not Active\n0, Active, Not Active\n", "256, Active, Not Active\n", "1, Not Active, Not Active\n"}) {
    write(f.root / "thermal.csv", text);
    const auto sample = f.sample();
    expect(!sample.thermal_throttled, "unknown, malformed, duplicate or missing selected thermal data unavailable");
    expect(!sample.notes.empty(), "unavailable signal has an honest note");
    expect(sample.compute_processes == 2U, "unsupported thermal signal does not break independent process query");
  }
  for (const auto& text : {"[N/A]\n", "0\n", "12x\n", "12, process.exe\n", "4294967296\n", "12\n12\n"}) {
    write(f.root / "compute.csv", text);
    expect(!f.sample().compute_processes, "malformed or unavailable process IDs not silently counted");
  }
  expect(!f.sample({256}).compute_processes, "out-of-range GPU selection refused");
  expect(!f.sample({0, 0}).thermal_throttled, "duplicate GPU selection refused");
}
void tool_failure_timeout_and_output_bounds() {
  Fixture f;
  write(f.root / "mode", "fail");
  expect(!f.sample().thermal_throttled && !f.sample().compute_processes, "nonzero exit never accepts apparently valid stdout");
  write(f.root / "mode", "hang");
  const auto start = std::chrono::steady_clock::now();
  const auto timed = f.sample({0}, 40);
  expect(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), "hung tool bounded and terminated");
  expect(!timed.thermal_throttled && !timed.compute_processes, "partial timeout data unavailable");
  write(f.root / "mode", "");
  write(f.root / "thermal.csv", std::string(70000U, 'x'));
  expect(!f.sample().thermal_throttled, "long output line refused");
  std::string many;
  for (int i = 0; i < 5000; ++i) many += std::to_string(i + 1) + "\n";
  write(f.root / "compute.csv", many);
  expect(!f.sample().compute_processes, "large multi-line output refused");
  expect(!sample_calibration_gpu_with_tool(f.root / "missing", {}, std::chrono::milliseconds{20}).thermal_throttled,
         "missing trusted tool is unavailable");
}
int fake_tool(int argc, char** argv) {
  if (argc < 3 || argc > 4 || std::string_view{argv[2]} != "--format=csv,noheader,nounits") return 9;
  const std::string_view query{argv[1]};
  const bool thermal = query == "--query-gpu=index,clocks_event_reasons.sw_thermal_slowdown,clocks_event_reasons.hw_thermal_slowdown";
  const bool driver = query == "--query-gpu=index,driver_model.current";
  if (!thermal && !driver && query != "--query-compute-apps=pid") return 9;
  if (argc == 4 && (thermal || (std::string_view{argv[3]} != "--id=0" && std::string_view{argv[3]} != "--id=1"))) return 9;
  const auto mode = read("mode");
  std::cout << read(thermal ? "thermal.csv" : driver ? "driver.csv" : "compute.csv") << std::flush;
  if (mode == "hang") std::this_thread::sleep_for(std::chrono::seconds(10));
  return mode == "fail" ? 7 : 0;
}
}  // namespace
int main(int argc, char** argv) {
  if (argc >= 2 && std::string_view{argv[1]}.starts_with("--query-")) return fake_tool(argc, argv);
  self = std::filesystem::absolute(argv[0]);
  return secure_frontend_test::run_cases(argc, argv, {
    {"thermal_flags_and_selected_devices", thermal_flags_and_selected_devices},
    {"unavailable_and_malformed_fields", unavailable_and_malformed_fields},
    {"tool_failure_timeout_and_output_bounds", tool_failure_timeout_and_output_bounds}}, failures);
}
