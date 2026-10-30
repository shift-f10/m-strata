// A calibration's report for a bug report (owner 2026-10-30: "When calibrating a model and an error occurs, it should
// create a detailed message of the errors encountered along with a detailed list of the hardware, that way a user can
// submit this as a report for bug fixes"): what it says, how it shortens paths, the GPUs nvidia-smi tells it, its file
// name and its bounds.
#include "secure_frontend/calibration_report.hpp"
#include "support/test_support.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

bool has(const std::string& text, const std::string_view part) { return text.find(part) != std::string::npos; }

secure_frontend::CalibrationReport sample() {
  secure_frontend::CalibrationReport report;
  report.date = "2026-10-30T04:16:46Z";
  report.card = "gemma-4-26B-A4B-it-UD-Q8_K_XL";
  report.family = "gemma4";
  report.model_file = "gemma-4-26B-A4B-it-UD-Q8_K_XL.gguf";
  report.model_bytes = 27866180544ULL;
  report.options = "expert_vram=auto;max_context=65536";
  report.started = "2026-10-30T04:10:00Z";
  report.outcome = "the calibration failed: the engine did not start for the calibration";
  report.trials.push_back({.number = 1U, .stage = "defaults", .args = {}, .max_tokens = 128U, .tok_s = 72.87, .failure = {},
                           .events = {}, .engine_tail = {}});
  report.trials.push_back({.number = 2U,
                           .stage = "--cpu-threads",
                           .args = {"--cpu-threads", "16"},
                           .max_tokens = 128U,
                           .tok_s = std::nullopt,
                           .failure = "the engine ended before it was ready",
                           .events = {"engine generation 1 ended: crashed", "engine generation 1: exit code 0xC0000005"},
                           .engine_tail = {"ggml_cuda_init: found 1 CUDA devices", "CUDA error: out of memory"}});
  report.machine.hardware.cpu = "AMD Ryzen 9 3900X 12-Core Processor, 24 threads";
  report.machine.hardware.ram_bytes = 103079215104ULL;  // 96 GiB
  report.machine.hardware.os = "Windows 11 Pro 25H2 (build 26200.9711)";
  report.machine.physical_cores = 12U;
  report.machine.memory = secure_frontend::MemoryReading{.used_bytes = 38654705664ULL, .total_bytes = 103079215104ULL};
  report.machine.gpus.push_back({.index = 0U, .name = "NVIDIA GeForce RTX 4080 SUPER", .mem_total_bytes = 16376ULL << 20U,
                                 .mem_used_bytes = 911ULL << 20U, .driver = "610.88", .compute_cap = "8.9", .pcie_gen_max = 4U,
                                 .pcie_width_max = 16U, .power_limit_w = 320.0});
  report.machine.build = secure_frontend::EngineBuild{.built = "2026-10-30T01:12:21Z", .commit = "3cb32db72782", .ggml = "3cf03257f219",
                                                      .cuda_arch = "89-real", .features = {"email", "search"},
                                                      .engines = {{.library = "multiengine_gemma4.dll", .models = "Gemma 4",
                                                                   .source = "the Strata core, engine 0.1.49.2", .bytes = 27421184U}}};
  return report;
}

// What happened (the card, its model file and size, the options, the outcome), every trial's row, each failed trial in
// full (the server's lines for its engine with its exit code, and the engine's own last lines), then the hardware (the
// CPU and its cores, the RAM and how much was free, each GPU with its driver, compute capability, PCIe link and power
// limit, the OS) and the engine folder's build - and a word to look it over before sharing it.
void the_report_says_what_failed_and_on_what_hardware() {
  const auto text = secure_frontend::render_calibration_report(sample());
  expect(text.rfind("# M-Strata calibration report\n", 0) == 0U, "a Markdown title first: " + text.substr(0, 80));
  expect(has(text, "Look it over before you share it"), "it says to look it over before sharing it");
  for (const char* part : {"gemma-4-26B-A4B-it-UD-Q8_K_XL", "gemma4", "gemma-4-26B-A4B-it-UD-Q8_K_XL.gguf, 27866180544 bytes",
                           "expert_vram=auto;max_context=65536", "2026-10-30T04:10:00Z", "2026-10-30T04:16:46Z",
                           "the calibration failed: the engine did not start for the calibration"}) {
    expect(has(text, part), std::string{"what happened: "} + part);
  }
  expect(has(text, "| 1 | defaults | 128 | 72.87 tok/s |") && has(text, "| 2 | --cpu-threads 16 | 128 | failed: the engine ended before it was ready |"),
         "a row per trial: " + text);
  for (const char* part : {"engine generation 1 ended: crashed", "engine generation 1: exit code 0xC0000005",
                           "ggml_cuda_init: found 1 CUDA devices", "CUDA error: out of memory"}) {
    expect(has(text, part), std::string{"the failed trial in full: "} + part);
  }
  for (const char* part : {"AMD Ryzen 9 3900X 12-Core Processor, 24 threads (12 physical cores)", "96.0 GiB", "60.0 GiB free",
                           "GPU 0: NVIDIA GeForce RTX 4080 SUPER", "16376 MiB", "911 MiB in use", "driver 610.88",
                           "compute capability 8.9", "PCIe gen 4 x16", "power limit 320 W",
                           "Windows 11 Pro 25H2 (build 26200.9711)", "3cb32db72782", "3cf03257f219", "89-real", "email, search",
                           "multiengine_gemma4.dll", "Gemma 4", "the Strata core, engine 0.1.49.2"}) {
    expect(has(text, part), std::string{"the hardware and the build: "} + part);
  }
  expect(text.find("## Hardware") > text.find("## Trials"), "the trials before the hardware");
  // no GPU read by nvidia-smi: the GPU list stands in, and no build record says so
  auto bare = sample();
  bare.machine.gpus.clear();
  bare.machine.hardware.gpus = {{.index = 0U, .name = "NVIDIA GeForce RTX 5090", .mem_total_bytes = 32607ULL << 20U}};
  bare.machine.build.reset();
  const auto other = secure_frontend::render_calibration_report(bare);
  expect(has(other, "GPU 0: NVIDIA GeForce RTX 5090, 32607 MiB") && has(other, "no build record"),
         "the GPU list stands in, and a missing build record is said: " + other);
}

// A failed trial is one whose failure is set; a report is made only then.
void a_report_is_made_only_when_a_trial_failed() {
  auto report = sample();
  expect(secure_frontend::report_needed(report.trials), "a failed trial");
  report.trials.pop_back();
  expect(!secure_frontend::report_needed(report.trials), "none failed");
  expect(!secure_frontend::report_needed({}), "no trial");
}

// The model, data, engine and home folders are shortened to their placeholders, the longest first; on Windows with
// either separator and in any case.
void paths_are_shortened() {
#if defined(_WIN32)
  const std::vector<secure_frontend::PathAlias> aliases{{"C:\\Users\\someone", "~"},
                                                        {"C:\\Users\\someone\\models\\gemma", "<model folder>"},
                                                        {"D:\\M-Strata\\data", "<data>"}};
  expect(secure_frontend::scrub_paths("loading C:\\Users\\someone\\models\\gemma\\g.gguf", aliases) == "loading <model folder>\\g.gguf",
         "the longest folder first: " + secure_frontend::scrub_paths("loading C:\\Users\\someone\\models\\gemma\\g.gguf", aliases));
  expect(secure_frontend::scrub_paths("open c:/users/SOMEONE/AppData/x.bin failed", aliases) == "open ~/AppData/x.bin failed",
         "either separator, any case: " + secure_frontend::scrub_paths("open c:/users/SOMEONE/AppData/x.bin failed", aliases));
  expect(secure_frontend::scrub_paths("cwd D:\\M-Strata\\data\\state\\engine and C:\\Users\\someone", aliases) ==
             "cwd <data>\\state\\engine and ~",
         "each place in the line");
  expect(secure_frontend::scrub_paths("G:\\models\\x.gguf", aliases) == "G:\\models\\x.gguf", "another folder stays");
#else
  const std::vector<secure_frontend::PathAlias> aliases{{"/home/someone", "~"}, {"/home/someone/models/gemma", "<model folder>"}};
  expect(secure_frontend::scrub_paths("loading /home/someone/models/gemma/g.gguf", aliases) == "loading <model folder>/g.gguf",
         "the longest folder first");
  expect(secure_frontend::scrub_paths("open /home/someone/.cache/x and /HOME/someone", aliases) == "open ~/.cache/x and /HOME/someone",
         "byte for byte");
#endif
  expect(secure_frontend::scrub_paths("no path here", {}) == "no path here", "no alias");
  expect(secure_frontend::scrub_paths("x", {{"", "<empty>"}}) == "x", "an empty folder is no alias");
}

// nvidia-smi's report answer: each GPU with its driver, compute capability, PCIe link and power limit; a placeholder
// leaves its reading empty; a line that is not one is skipped.
void nvidia_smi_report_lines_are_read() {
  const auto gpus = secure_frontend::parse_nvidia_smi_report(
      "0, NVIDIA GeForce RTX 4080 SUPER, 16376, 911, 610.88, 8.9, 4, 16, 320.00\n"
      "1, NVIDIA GeForce RTX 5090, 32607, [N/A], 610.88, 12.0, [Not Supported], 16, [N/A]\n"
      "not a gpu line\n"
      "1, a repeat, 1, 1, 1, 1, 1, 1, 1\n");
  expect(gpus.size() == 2U, "two GPUs: " + std::to_string(gpus.size()));
  if (gpus.size() != 2U) return;
  expect(gpus[0].index == 0U && gpus[0].name == "NVIDIA GeForce RTX 4080 SUPER" && gpus[0].mem_total_bytes == (16376ULL << 20U) &&
             gpus[0].mem_used_bytes == (911ULL << 20U) && gpus[0].driver == "610.88" && gpus[0].compute_cap == "8.9" &&
             gpus[0].pcie_gen_max == 4U && gpus[0].pcie_width_max == 16U && gpus[0].power_limit_w == 320.0,
         "every reading of the first");
  expect(gpus[1].index == 1U && gpus[1].name == "NVIDIA GeForce RTX 5090" && !gpus[1].mem_used_bytes && !gpus[1].pcie_gen_max &&
             gpus[1].pcie_width_max == 16U && !gpus[1].power_limit_w && gpus[1].compute_cap == "12.0",
         "the placeholders left empty");
  const auto args = secure_frontend::nvidia_smi_report_arguments();
  expect(args.size() == 2U && args[0] == "--query-gpu=" + std::string{secure_frontend::kNvidiaSmiReportQuery} &&
             args[1] == "--format=csv,noheader,nounits",
         "one answer, every GPU");
}

// calibration-<card>-<date>-<time>.md, the card's name safe for any file system.
void the_report_file_name_is_safe() {
  expect(secure_frontend::report_file_name("gemma-4-26B", "2026-10-30T04:16:46Z") == "calibration-gemma-4-26B-20261030-041646.md",
         secure_frontend::report_file_name("gemma-4-26B", "2026-10-30T04:16:46Z"));
  expect(secure_frontend::report_file_name("a:b/c\\d e..f", "2026-10-30T04:16:46Z") == "calibration-a_b_c_d_e..f-20261030-041646.md",
         secure_frontend::report_file_name("a:b/c\\d e..f", "2026-10-30T04:16:46Z"));
  expect(secure_frontend::report_file_name(std::string(200U, 'x'), "2026-10-30T04:16:46Z").size() ==
             std::string{"calibration--20261030-041646.md"}.size() + 80U,
         "at most 80 characters of the name");
}

// Bounded: each failed trial's engine lines (the last 40) and the server's (20), each line cut, and at most 10 failed
// trials told in full.
void the_report_is_bounded() {
  auto report = sample();
  report.trials.clear();
  for (std::size_t i = 1; i <= 30U; ++i) {
    secure_frontend::CalibrationTrialRecord trial{.number = i, .stage = "--pcie-frac", .args = {"--pcie-frac", "0.5"},
                                                  .max_tokens = 128U, .tok_s = std::nullopt, .failure = "the engine ended before it was ready",
                                                  .events = {}, .engine_tail = {}};
    for (std::size_t line = 0; line < 100U; ++line) trial.engine_tail.push_back("line " + std::to_string(line) + " " + std::string(2000U, 'z'));
    for (std::size_t line = 0; line < 50U; ++line) trial.events.push_back("event " + std::to_string(line));
    report.trials.push_back(std::move(trial));
  }
  const auto text = secure_frontend::render_calibration_report(report);
  expect(!has(text, "line 59 ") && has(text, "line 60 ") && has(text, "line 99 "), "the engine's last 40 lines");
  expect(!has(text, std::string(secure_frontend::kReportLineChars + 1U, 'z')), "each line cut");
  expect(!has(text, "event 29\n") && has(text, "event 30\n") && has(text, "event 49\n"), "the server's last 20 lines");
  expect(has(text, "| 30 | --pcie-frac 0.5 |"), "every trial has its row");
  expect(has(text, "### Trial 10 failed") && !has(text, "### Trial 11 failed") && has(text, "20 more failed trials are in the table"),
         "ten told in full, the rest counted");
  expect(text.size() < 256U * 1024U, "at most 256 KiB: " + std::to_string(text.size()));
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"the_report_says_what_failed_and_on_what_hardware", the_report_says_what_failed_and_on_what_hardware},
      {"a_report_is_made_only_when_a_trial_failed", a_report_is_made_only_when_a_trial_failed},
      {"paths_are_shortened", paths_are_shortened},
      {"nvidia_smi_report_lines_are_read", nvidia_smi_report_lines_are_read},
      {"the_report_file_name_is_safe", the_report_file_name_is_safe},
      {"the_report_is_bounded", the_report_is_bounded},
  }, failures);
}
