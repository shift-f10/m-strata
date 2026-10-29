// Session log step 16 (listener.monitor): the Monitor's meter, its JSON, and this machine's
// readings — CPU and RAM from the OS, the GPU from nvidia-smi as a separate child.
#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/host_readings.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/serve_monitor.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

// --- JSON reading helpers ---------------------------------------------------------------
std::optional<secure_frontend::JsonValue> parsed(const std::string& text) {
  const secure_frontend::JsonLimits limits{.max_bytes = 1U << 20U, .max_depth = 8U, .max_container_entries = 4096U,
                                           .max_string_bytes = 1U << 16U, .allow_fractions = true};
  auto result = secure_frontend::parse_json(text, limits);
  if (auto* value = std::get_if<secure_frontend::JsonValue>(&result)) return std::move(*value);
  return std::nullopt;
}
const secure_frontend::JsonValue* at(const secure_frontend::JsonValue* value, const std::string_view key) {
  if (value == nullptr || value->if_object() == nullptr) return nullptr;
  for (const auto& member : *value->if_object()) {
    if (member.key == key) return &member.value;
  }
  return nullptr;
}
const secure_frontend::JsonValue* item(const secure_frontend::JsonValue* value, const std::size_t index) {
  if (value == nullptr || value->if_array() == nullptr || index >= value->if_array()->size()) return nullptr;
  return &(*value->if_array())[index];
}
std::optional<double> number(const secure_frontend::JsonValue* value) {
  if (value == nullptr) return std::nullopt;
  if (const auto* i = value->if_integer()) return static_cast<double>(*i);
  if (const auto* d = value->if_decimal()) return *d;
  return std::nullopt;
}
bool is_null(const secure_frontend::JsonValue* value) { return value != nullptr && value->kind() == secure_frontend::JsonKind::null; }
std::string text(const secure_frontend::JsonValue* value) {
  return value != nullptr && value->if_string() != nullptr ? *value->if_string() : std::string{"<not a string>"};
}
bool close_to(const std::optional<double> value, const double expected) { return value.has_value() && std::abs(*value - expected) < 0.051; }
std::size_t size_of(const secure_frontend::JsonValue* value) {
  return value != nullptr && value->if_array() != nullptr ? value->if_array()->size() : 0U;
}

// A monitor on clocks the test moves.
struct Clocked final {
  std::chrono::steady_clock::time_point now{};
  std::int64_t unix_now{1'700'000'000};
  secure_frontend::ServeMonitor monitor;
  explicit Clocked(const std::size_t kept = secure_frontend::kMonitorKeptRequests)
      : monitor(kept, [this] { return now; }, [this] { return unix_now; }) {}
  secure_frontend::JsonValue json(const std::string_view engine = "ready", const std::size_t queued = 0) {
    const std::string rendered = monitor.render_json({.model = "qwen-test", .engine = engine, .queued = queued});
    auto value = parsed(rendered);
    expect(value.has_value(), "the Monitor's JSON parses: " + rendered.substr(0, 300));
    return value.value_or(secure_frontend::JsonValue{});
  }
};

// Owner 2026-10-29: "in the monitor section, the speed portion does not seem to update in realtime". It was the mean since
// the first token, which barely moves in a long answer; upstream's live rate (serve/server.py _rate_of, RATE_WINDOW_S
// 2.0, RATE_MIN_SPAN_S 0.25, at most 32 samples) is the tokens over the last two seconds' samples, and the mean so far
// (generated over the time since the first token, at least 0.25 s) before there is such a span or after a stall.
void the_live_speed_is_the_last_two_seconds() {
  Clocked c;
  c.monitor.generation_started(12, 4096, 8192);
  c.now += 100ms;
  c.monitor.on_token(1);
  for (std::size_t n = 2; n <= 11; ++n) {  // one token a second for ten seconds
    c.now += 1s;
    c.monitor.on_token(n);
  }
  for (std::size_t n = 12; n <= 31; ++n) {  // then twenty in one second
    c.now += 50ms;
    c.monitor.on_token(n);
  }
  auto json = c.json("generating");
  expect(close_to(number(at(at(&json, "live"), "tok_s")), 10.5),
         "the last two seconds: 21 tokens in 2 s, not the mean since the first token (30 in 11 s)");
  c.now += 5s;  // no token for five seconds
  json = c.json("generating");
  expect(close_to(number(at(at(&json, "live"), "tok_s")), 31.0 / 16.1), "after a stall: the mean since the first token");
}

// --- nvidia-smi ---------------------------------------------------------------------------
void parses_nvidia_smi_lines() {
  const auto real = secure_frontend::parse_nvidia_smi_line("NVIDIA GeForce RTX 4080 SUPER, 0, 1036, 16376, 34, 8.66, 320.00, 1, 4, 16");
  expect(real.has_value(), "the owner box's own line parses");
  if (real) {
    expect(real->name == "NVIDIA GeForce RTX 4080 SUPER" && real->util_percent == std::optional<double>{0.0} &&
               real->mem_used_bytes == std::optional<std::uint64_t>{1036ULL * 1048576ULL} &&
               real->mem_total_bytes == std::optional<std::uint64_t>{16376ULL * 1048576ULL} &&
               real->temp_c == std::optional<double>{34.0} && close_to(real->power_w, 8.66) &&
               real->power_limit_w == std::optional<double>{320.0} && real->pcie_gen == std::optional<unsigned>{1U} &&
               real->pcie_gen_max == std::optional<unsigned>{4U} && real->pcie_width == std::optional<unsigned>{16U},
           "every field, memory in bytes");
  }
  const auto partial = secure_frontend::parse_nvidia_smi_line("Tesla T4, [N/A], 0, 15360, 40, [Not Supported], 70.00, 3, 3, 16");
  expect(partial.has_value() && !partial->util_percent && !partial->power_w && partial->power_limit_w == std::optional<double>{70.0},
         "[N/A] and [Not Supported] leave just that reading empty");
  // Owner 2026-10-29: an RTX 5090 showed no GPU reading at all. Any bracketed placeholder nvidia-smi prints for a field
  // it cannot read leaves just that reading empty, as [N/A] does; the line's other readings still show.
  const auto placeholders = secure_frontend::parse_nvidia_smi_line(
      "NVIDIA GeForce RTX 5090, 3, 2048, 32607, 41, [Unknown Error], [Insufficient Permissions], [GPU requires reset], 5, "
      "[Requested functionality has been deprecated]");
  expect(placeholders.has_value() && placeholders->util_percent == std::optional<double>{3.0} &&
             placeholders->mem_used_bytes == std::optional<std::uint64_t>{2048ULL * 1048576ULL} &&
             placeholders->temp_c == std::optional<double>{41.0} && !placeholders->power_w && !placeholders->power_limit_w &&
             !placeholders->pcie_gen && placeholders->pcie_gen_max == std::optional<unsigned>{5U} && !placeholders->pcie_width,
         "any bracketed placeholder leaves just that reading empty");
  for (const std::string& bad : {std::string{},std::string{"GPU, 1"}, std::string{"GPU, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10"},
                                std::string{"GPU, abc, 1, 2, 3, 4, 5, 6, 7, 8"}, std::string{"GPU, -1, 1, 2, 3, 4, 5, 6, 7, 8"},
                                std::string{"GP\x01U, 1, 1, 2, 3, 4, 5, 6, 7, 8"}, std::string(97U, 'G') + ", 1, 1, 2, 3, 4, 5, 6, 7, 8",
                                std::string{", 1, 1, 2, 3, 4, 5, 6, 7, 8"}, std::string{"GPU, 1e9, 1, 2, 3, 4, 5, 6, 7, 8"},
                                std::string{"GPU, [Unknown Error, 1, 2, 3, 4, 5, 6, 7, 8"}, std::string{"GPU, [], 1, 2, 3, 4, 5, 6, 7, 8"},
                                std::string{"GPU, [N/A]1, 1, 2, 3, 4, 5, 6, 7, 8"}, std::string{"GPU, [[N/A]], 1, 2, 3, 4, 5, 6, 7, 8"},
                                std::string{"GPU, [Unknown\x01" "Error], 1, 2, 3, 4, 5, 6, 7, 8"},
                                "GPU, [" + std::string(63U, 'x') + "], 1, 2, 3, 4, 5, 6, 7, 8"}) {
    expect(!secure_frontend::parse_nvidia_smi_line(bad).has_value(), "the whole line is refused: [" + bad + "]");
  }
}

void nvidia_smi_command_is_fixed() {
  const auto path = secure_frontend::nvidia_smi_path();
#if defined(_WIN32)
  std::string directory = path.parent_path().filename().string();
  for (char& character : directory) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  expect(path.is_absolute() && path.filename() == "nvidia-smi.exe" && directory == "system32",
         "System32\\nvidia-smi.exe only: " + path.string());
#else
  expect(path == std::filesystem::path{"/usr/bin/nvidia-smi"}, "/usr/bin/nvidia-smi only: " + path.string());
#endif
  expect(secure_frontend::nvidia_smi_arguments() ==
             std::vector<std::string>({"--id=0", "--query-gpu=" + std::string{secure_frontend::kNvidiaSmiQuery},
                                       "--format=csv,noheader,nounits", "--loop-ms=1000"}),
         "GPU 0, the fixed query, csv without header or units, one line a second");
}

// The real tool needs ProgramFiles (NVML does not start without it); nothing else is passed,
// and the value is the machine's (HKLM), not the caller's environment.
void nvidia_smi_environment_is_program_files_only() {
  const auto environment = secure_frontend::nvidia_smi_environment();
#if defined(_WIN32)
  const auto found = environment.find("ProgramFiles");
  expect(environment.size() == 1U && found != environment.end(), "exactly ProgramFiles");
  if (found == environment.end()) return;
  std::error_code ec;
  const std::filesystem::path directory{found->second};
  expect(directory.is_absolute() && std::filesystem::is_directory(directory, ec), "an existing absolute directory: " + found->second);
  const char* own = secure_frontend::crt_getenv("ProgramFiles");
  expect(own == nullptr || std::string{own} == found->second, "the machine's Program Files (here equal to this process's)");
#else
  expect(environment.empty(), "an empty environment off Windows");
#endif
}

// --- the meter and its JSON -------------------------------------------------------------
void monitor_records_generations() {
  Clocked c(3);
  const auto idle = c.json();
  expect(text(at(at(&idle, "engine"), "model")) == "qwen-test" && is_null(at(at(&idle, "engine"), "max_context")),
         "the served name; the context is not known before a generation");
  expect(text(at(at(&idle, "live"), "state")) == "idle" && size_of(at(&idle, "requests")) == 0U,
         "idle, no requests");
  expect(number(at(at(&idle, "totals"), "since")) == std::optional<double>{1'700'000'000.0}, "totals since the start");

  c.monitor.generation_started(10, 100, 4096);
  c.now += 200ms;
  c.monitor.on_token(1);
  c.now += 100ms;
  c.monitor.on_token(2);
  c.monitor.on_token(3);
  c.now += 100ms;
  c.monitor.on_token(4);
  c.monitor.on_token(5);
  c.monitor.on_done({.prompt_tokens = 10U, .prompt_ms = 200.0, .decode_ms = 400.0, .reused = 4U});
  c.unix_now += 1;
  c.monitor.generation_ended("stop");
  const auto one = c.json();
  const auto* first = item(at(&one, "requests"), 0);
  expect(text(at(first, "finish")) == "stop" && number(at(first, "prompt_tokens")) == std::optional<double>{10.0} &&
             number(at(first, "reused")) == std::optional<double>{4.0} && number(at(first, "output_tokens")) == std::optional<double>{5.0},
         "a record: finish, prompt, reused and output counts");
  expect(close_to(number(at(first, "prompt_ms")), 200.0) && close_to(number(at(first, "decode_tok_s")), 12.5) &&
             close_to(number(at(first, "duration_s")), 0.4) && number(at(first, "time")) == std::optional<double>{1'700'000'001.0},
         "the engine's timings: 5 tokens in 400 ms is 12.5 tok/s; the wall time; when it ended");
  expect(number(at(at(&one, "engine"), "max_context")) == std::optional<double>{4096.0}, "the context from the generation's READY");
  const auto* totals = at(&one, "totals");
  expect(number(at(totals, "requests")) == std::optional<double>{1.0} && number(at(totals, "prompt_tokens")) == std::optional<double>{10.0} &&
             number(at(totals, "output_tokens")) == std::optional<double>{5.0} && number(at(totals, "reused")) == std::optional<double>{4.0} &&
             close_to(number(at(totals, "prompt_ms")), 200.0) && close_to(number(at(totals, "decode_ms")), 400.0),
         "the totals");

  // A short DONE: the frontend's own timings stand in (first token, and first to last token).
  c.monitor.generation_started(7, 50, 4096);
  c.now += 50ms;
  c.monitor.on_token(1);
  c.now += 100ms;
  c.monitor.on_token(2);
  c.monitor.on_token(3);
  c.monitor.on_done({});
  c.monitor.generation_ended("length");
  const auto two = c.json();
  const auto* newest = item(at(&two, "requests"), 0);
  expect(text(at(newest, "finish")) == "length" && number(at(newest, "prompt_tokens")) == std::optional<double>{7.0} &&
             is_null(at(newest, "reused")) && close_to(number(at(newest, "prompt_ms")), 50.0) &&
             close_to(number(at(newest, "decode_tok_s")), 20.0),
         "newest first; without engine figures: 50 ms to the first token, 2 more in 100 ms");
  expect(text(at(item(at(&two, "requests"), 1), "finish")) == "stop", "the older one second");

  for (int i = 0; i < 3; ++i) {
    c.monitor.generation_started(1, 8, 4096);
    c.monitor.generation_ended("error");
  }
  const auto capped = c.json();
  expect(size_of(at(&capped, "requests")) == 3U && number(at(&capped, "requests_kept")) == std::optional<double>{3.0} &&
             number(at(at(&capped, "totals"), "requests")) == std::optional<double>{5.0},
         "only the newest kept_requests stay; the totals count all");
  expect(is_null(at(item(at(&capped, "requests"), 0), "decode_tok_s")), "no token: no rate");
}

void monitor_reports_live_states() {
  Clocked c;
  const auto starting = c.json("starting");
  expect(text(at(at(&starting, "live"), "state")) == "starting", "the engine is starting");
  const auto failed = c.json("failed");
  expect(text(at(at(&failed, "live"), "state")) == "error", "the engine failed");
  c.monitor.generation_started(12, 64, 8192);
  const auto reading = c.json("generating", 2);
  const auto* live = at(&reading, "live");
  expect(text(at(live, "state")) == "reading" && number(at(live, "queued")) == std::optional<double>{2.0} &&
             number(at(live, "prompt_tokens")) == std::optional<double>{12.0} && number(at(live, "max_tokens")) == std::optional<double>{64.0} &&
             number(at(live, "generated")) == std::optional<double>{0.0},
         "reading the prompt until the first token; the queue as the gate says");
  c.now += 300ms;
  c.monitor.on_token(1);
  c.now += 500ms;
  for (std::size_t n = 2; n <= 6; ++n) c.monitor.on_token(n);
  const auto generating = c.json("generating");
  live = at(&generating, "live");
  expect(text(at(live, "state")) == "generating" && number(at(live, "generated")) == std::optional<double>{6.0} &&
             close_to(number(at(live, "tok_s")), 10.0) && close_to(number(at(live, "prefill_tok_s")), 40.0) &&
             close_to(number(at(live, "elapsed_s")), 0.8),
         "generating: 5 tokens after the first in 500 ms; 12 prompt tokens in 300 ms");
  c.monitor.generation_started(3, 8, 8192);  // the old one never ended
  c.monitor.generation_ended("disconnect");
  const auto after = c.json();
  expect(text(at(at(&after, "live"), "state")) == "idle" && text(at(item(at(&after, "requests"), 0), "finish")) == "disconnect" &&
             text(at(item(at(&after, "requests"), 1), "finish")) == "error",
         "a start over an open generation ends that one as error first");
  expect(secure_frontend::monitor_finish(secure_frontend::RunEnd::stop, secure_frontend::RunCancel::none) == "stop" &&
             secure_frontend::monitor_finish(secure_frontend::RunEnd::length, secure_frontend::RunCancel::none) == "length" &&
             secure_frontend::monitor_finish(secure_frontend::RunEnd::cancelled, secure_frontend::RunCancel::client_gone) == "disconnect" &&
             secure_frontend::monitor_finish(secure_frontend::RunEnd::cancelled, secure_frontend::RunCancel::write_stall) == "cancel" &&
             secure_frontend::monitor_finish(secure_frontend::RunEnd::cancelled, secure_frontend::RunCancel::shutting_down) == "cancel" &&
             secure_frontend::monitor_finish(secure_frontend::RunEnd::engine_error, secure_frontend::RunCancel::none) == "error" &&
             secure_frontend::monitor_finish(secure_frontend::RunEnd::engine_unavailable, secure_frontend::RunCancel::none) == "error" &&
             secure_frontend::monitor_finish(secure_frontend::RunEnd::undecodable, secure_frontend::RunCancel::none) == "error",
         "each way a generation ends has its badge");
}

// Owner 2026-10-27 ("when I stop the running engine, the stats/monitor page does not reset stats like it still have
// something loaded"; "the model state should say unloaded or stopped"; "on recent request, we should identify what
// model that line was ran on"): with no engine (the launcher's "none") the state is stopped and the stopped engine's
// context and expert readings are gone, while the history stays - each request naming the model it ran on.
void a_stopped_engine_reads_stopped_and_requests_name_their_model() {
  Clocked c;
  const auto none = c.json("none");
  expect(text(at(at(&none, "live"), "state")) == "stopped", "no engine: stopped");
  c.monitor.generation_started(10, 100, 131072, "raon-1-mini-preview-Q8_0");
  c.monitor.on_engine_stats({{"expert_slots", "120"}, {"expert_slots_primary", "90"}});
  c.monitor.on_token(1);
  c.monitor.generation_ended("stop");
  c.monitor.generation_started(4, 50, 65536, "flint-Q8_0");
  c.monitor.on_token(1);
  c.monitor.generation_ended("length");
  const auto running = c.json("ready");
  expect(number(at(at(&running, "engine"), "max_context")) == std::optional<double>{65536.0} && at(at(&running, "engine"), "experts") != nullptr &&
             text(at(at(&running, "live"), "state")) == "idle",
         "while it runs: its context and expert readings, idle between requests");
  expect(text(at(item(at(&running, "requests"), 0), "model")) == "flint-Q8_0" &&
             text(at(item(at(&running, "requests"), 1), "model")) == "raon-1-mini-preview-Q8_0",
         "each request names the model it ran on");
  const auto stopped = c.json("none");
  expect(text(at(at(&stopped, "live"), "state")) == "stopped" && is_null(at(at(&stopped, "engine"), "max_context")) &&
             at(at(&stopped, "engine"), "experts") == nullptr,
         "stopped: no context, no expert readings of the engine that ran");
  expect(size_of(at(&stopped, "requests")) == 2U && number(at(at(&stopped, "totals"), "requests")) == std::optional<double>{2.0},
         "the history and the totals stay");
  c.monitor.engine_changed();
  const auto again = c.json("ready");
  expect(is_null(at(at(&again, "engine"), "max_context")) && at(at(&again, "engine"), "experts") == nullptr,
         "the next engine starts without the old readings");
}

void monitor_keeps_a_bounded_history() {
  Clocked c;
  for (int i = 1; i <= 70; ++i) {
    secure_frontend::HostSample sample;
    sample.cpu_percent = static_cast<double>(i);
    sample.memory = secure_frontend::MemoryReading{.used_bytes = 8ULL << 30U, .total_bytes = 64ULL << 30U};
    sample.gpu = secure_frontend::parse_nvidia_smi_line("Fake GPU, 42, 2048, 16384, 55, 120.50, 320.00, 4, 4, 16");
    c.monitor.add_host_sample(sample);
  }
  const auto full = c.json();
  const auto* hardware = at(&full, "hardware");
  const auto* history = at(&full, "history");
  expect(number(at(hardware, "cpu")) == std::optional<double>{70.0} && number(at(hardware, "ram_total")) == std::optional<double>{68719476736.0} &&
             number(at(hardware, "gpu_util")) == std::optional<double>{42.0} &&
             number(at(hardware, "gpu_mem_used")) == std::optional<double>{2048.0 * 1048576.0} &&
             number(at(hardware, "gpu_mem_total")) == std::optional<double>{16384.0 * 1048576.0} && number(at(hardware, "gpu_temp")) == std::optional<double>{55.0} &&
             close_to(number(at(hardware, "gpu_power")), 120.5) && number(at(hardware, "gpu_power_limit")) == std::optional<double>{320.0} &&
             number(at(hardware, "gpu_pcie_gen")) == std::optional<double>{4.0} && number(at(hardware, "gpu_pcie_width")) == std::optional<double>{16.0},
         "the newest reading");
  expect(text(at(at(&full, "hardware_static"), "gpu_name")) == "Fake GPU" && number(at(at(&full, "hardware_static"), "threads")).value_or(0.0) >= 1.0,
         "the GPU's name and this machine's threads");
  for (const char* series : {"cpu", "gpu_util", "gpu_mem_used", "gpu_temp", "gpu_power", "tok_s"}) {
    expect(size_of(at(history, series)) == secure_frontend::kMonitorHistorySamples, std::string{"a minute of "} + series);
  }
  expect(number(item(at(history, "cpu"), secure_frontend::kMonitorHistorySamples - 1U)) == std::optional<double>{70.0} &&
             number(item(at(history, "cpu"), 0)) == std::optional<double>{11.0},
         "oldest first, the newest last, the first ten gone");
  secure_frontend::HostSample without_gpu;
  without_gpu.gpu_note = "nvidia-smi is not installed";
  c.monitor.add_host_sample(without_gpu);
  const auto none = c.json();
  expect(is_null(at(at(&none, "hardware"), "gpu_util")) && is_null(at(at(&none, "hardware"), "cpu")) &&
             text(at(at(&none, "hardware_static"), "gpu_note")) == "nvidia-smi is not installed",
         "a sample without readings says so: null, and the note");
  expect(is_null(item(at(at(&none, "history"), "gpu_util"), secure_frontend::kMonitorHistorySamples - 1U)), "a gap is null");
}

// --- this machine -------------------------------------------------------------------------
void host_readings_are_plausible() {
  const auto memory = secure_frontend::read_memory();
  expect(memory.has_value() && memory->total_bytes > (256ULL << 20U) && memory->used_bytes > 0U && memory->used_bytes <= memory->total_bytes,
         "RAM: used within total");
  secure_frontend::CpuMeter cpu;
  expect(!cpu.sample().has_value(), "the first CPU sample has nothing to compare with");
  std::this_thread::sleep_for(200ms);
  const auto load = cpu.sample();
  expect(load.has_value() && *load >= 0.0 && *load <= 100.0, "the second is a percentage");
}

void gpu_sampler_reads_a_fake_tool() {
  const auto wait_for = [](const auto& condition) {
    for (int i = 0; i < 100 && !condition(); ++i) std::this_thread::sleep_for(50ms);
    return condition();
  };
  {
    secure_frontend::GpuSampler sampler;
    sampler.start({.executable = STRATA_FAKE_ENGINE_PATH, .arguments = {"--fake-nvidia-smi"}, .environment = {}, .insert_serve = false});
    expect(wait_for([&] { return sampler.latest().has_value(); }), "the fake tool's line arrives: " + sampler.note());
    const auto reading = sampler.latest();
    expect(reading.has_value() && reading->name == "Fake GPU" && reading->util_percent == std::optional<double>{42.0} && sampler.note().empty(),
           "parsed, and no note while readings arrive");
    sampler.stop();
    expect(!sampler.note().empty(), "stopped says so");
  }
  {
    secure_frontend::GpuSampler once;
    once.start({.executable = STRATA_FAKE_ENGINE_PATH, .arguments = {"--fake-nvidia-smi=2"}, .environment = {}, .insert_serve = false});
    expect(wait_for([&] { return !once.note().empty() && once.note() != "starting" && !once.latest().has_value(); }),
           "a tool that exits leaves no stale reading and a note: " + once.note());
  }
  {
    secure_frontend::GpuSampler missing;
    missing.start({.executable = std::filesystem::path{STRATA_FAKE_ENGINE_PATH}.parent_path() / "no-such-nvidia-smi.exe",
                   .arguments = {}, .environment = {}, .insert_serve = false});
    expect(wait_for([&] { return missing.note() != "starting"; }) && !missing.latest().has_value() && !missing.note().empty(),
           "a missing tool: no reading, a note: " + missing.note());
  }
  // Owner 2026-10-29 (an RTX 5090 showed no GPU reading and no reason): what nvidia-smi answered instead of a reading was
  // dropped unread, and the note said only "nvidia-smi stopped" or "is not answering". The note quotes it: printable
  // ASCII only (anything else '?'), bounded.
  const auto quotes_the_answer = [](const std::string& note) {
    expect(note.find("Failed to initialize NVML: Driver/library version mismatch") != std::string::npos &&
               note.find("NVML library version: 999.??") != std::string::npos,
           "the note quotes what nvidia-smi answered, a non-ASCII byte as '?': " + note);
    expect(std::all_of(note.begin(), note.end(), [](const unsigned char c) { return c >= 0x20U && c <= 0x7eU; }) &&
               note.size() <= 160U && note.find("installed and running") == std::string::npos,
           "printable ASCII, cut at its bound: " + note);
  };
  {
    secure_frontend::GpuSampler exited;
    exited.start({.executable = STRATA_FAKE_ENGINE_PATH, .arguments = {"--fake-nvidia-smi-unreadable=1"}, .environment = {},
                  .insert_serve = false});
    expect(wait_for([&] { return exited.note() != "starting"; }) && !exited.latest().has_value(),
           "a tool that answers no reading and exits: no reading: " + exited.note());
    quotes_the_answer(exited.note());
  }
  {
    secure_frontend::GpuSampler unreadable;
    unreadable.start({.executable = STRATA_FAKE_ENGINE_PATH, .arguments = {"--fake-nvidia-smi-unreadable"}, .environment = {},
                      .insert_serve = false});
    for (int i = 0; i < 200 && unreadable.note().find("NVML") == std::string::npos; ++i) std::this_thread::sleep_for(50ms);
    expect(!unreadable.latest().has_value(), "lines that are not readings leave no reading");
    quotes_the_answer(unreadable.note());
    unreadable.stop();
  }
}

// The server's own command against the real tool, where the box has one (the owner's does; a
// CI runner without NVIDIA passes on the absence). Session log step 16: with an empty
// environment NVML failed to start and the sampler only said "nvidia-smi stopped".
void gpu_sampler_reads_the_real_tool_when_present() {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(secure_frontend::nvidia_smi_path(), ec)) {
    std::cerr << "note: no nvidia-smi on this machine; nothing to read\n";
    return;
  }
  secure_frontend::GpuSampler sampler;
  sampler.start({.executable = secure_frontend::nvidia_smi_path(), .arguments = secure_frontend::nvidia_smi_arguments(),
                 .environment = secure_frontend::nvidia_smi_environment(), .no_new_privileges = true, .insert_serve = false});
  for (int i = 0; i < 100 && !sampler.latest().has_value(); ++i) std::this_thread::sleep_for(50ms);
  const auto reading = sampler.latest();
  expect(reading.has_value() && !reading->name.empty() && reading->mem_total_bytes.value_or(0U) > 0U,
         "the real nvidia-smi answers with the server's command: " + sampler.note());
}

// Owner Monitor fix: the engine block carries the expert-tier readings so the VRAM
// and RAM cards can show experts loaded. on_engine_stats feeds it; keys outside the
// whitelist and non-numeric values never reach the JSON.
void engine_readings_reach_the_engine_block() {
  secure_frontend::ServeMonitor monitor;
  monitor.on_engine_stats({{"expert_slots", "384"},
                           {"expert_slots_primary", "212"},
                           {"expert_cache_mib", "10240"},
                           {"vram_used", "2102"},
                           {"ram_used", "1790"},
                           {"vram_gb", "14.6"},
                           {"tok_s", "30.10"},
                           {"model_path", "/private/path"}});
  const std::string json = monitor.render_json({.model = "qwen-test", .engine = "ready", .queued = 0});
  expect(json.find("\"experts\"") != std::string::npos, "the engine block carries an experts object");
  expect(json.find("\"expert_slots\":384") != std::string::npos && json.find("\"vram_used\":2102") != std::string::npos &&
         json.find("\"ram_used\":1790") != std::string::npos && json.find("\"vram_gb\":14.6") != std::string::npos,
         "whitelisted numbers pass through as numbers");
  // tok_s legitimately exists elsewhere in the payload; the rule is that the experts
  // object carries only whitelisted numbers (and model_path never appears at all).
  const auto experts_pos = json.find("\"experts\":");
  const std::string experts_block = experts_pos == std::string::npos ? std::string{} : json.substr(experts_pos, json.find('}', experts_pos) - experts_pos);
  expect(json.find("model_path") == std::string::npos && experts_block.find("tok_s") == std::string::npos,
         "non-whitelisted keys never reach the experts block");
}

// Model launcher (plan 2026-10-23): a switch to another engine drops the readings of the one
// before it (its expert tier and its READY context), so the Monitor never shows Qwen's expert
// cache under a Gemma engine. The request history and the host readings stay.
void engine_change_drops_the_old_engines_readings() {
  secure_frontend::ServeMonitor monitor;
  monitor.on_engine_stats({{"expert_slots", "4232"}, {"expert_cache_mib", "8269"}});
  monitor.generation_started(10, 20, 131072);
  monitor.generation_ended("stop");
  monitor.engine_changed();
  const std::string json = monitor.render_json({.model = "Gemma", .engine = "starting", .queued = 0});
  expect(json.find("8269") == std::string::npos && json.find("\"expert_slots\":4232") == std::string::npos,
         "the old engine's expert readings are gone: " + json);
  expect(json.find("131072") == std::string::npos, "the old engine's context is gone: " + json);
  expect(json.find("\"prompt_tokens\":10") != std::string::npos, "the request history stays: " + json);
  monitor.on_engine_stats({{"expert_slots", "3840"}, {"expert_slots_primary", "870"}});
  const std::string after = monitor.render_json({.model = "Gemma", .engine = "ready", .queued = 0});
  expect(after.find("\"expert_slots\":3840") != std::string::npos && after.find("\"expert_slots_primary\":870") != std::string::npos,
         "the new engine's readings show: " + after);
}

// --- owner 2026-10-28: experts + memory on the RAM card, stale stats, disk reads, the conversation cache ----------
constexpr double kGiB = 1073741824.0;
constexpr double kMiB = 1048576.0;

// "The System Ram reports MB usage only and not experts + mb usage": every family says how much of the model it holds
// in RAM, each in its own words; engine.memory carries it in one shape. The rows are each engine's own startup INFO
// (before READY) and its STAT cadence (while it decodes), key for key.
void engine_memory_reads_every_familys_ram_tier() {
  struct Row final {
    std::string family;
    std::map<std::string, std::string> readings;
    std::optional<double> ram;        // experts held in RAM
    std::optional<double> ram_bytes;  // their bytes
    std::optional<double> vram;       // experts held in VRAM
  };
  const std::vector<Row> rows{
      // Qwen on upstream's engine (generate.cpp:11114): expert_slots is every VRAM tier, the RAM arena is arena_mib.
      {"qwen", {{"expert_slots", "4232"}, {"expert_cache_mib", "8269"}, {"expert_slots_primary", "4232"},
                {"expert_cache_primary_mib", "8269"}, {"vram_free_mib", "900"}, {"arena_mib", "20480"}},
       std::nullopt, 20480.0 * kMiB, 4232.0},
      // GLM and raon1 (Maya generate.cpp:1049, project-maya glm_driver.cpp:829): the RAM tier's slots and GiB.
      {"glm", {{"expert_slots", "870"}, {"expert_cache_mib", "12800"}, {"experts", "19456"}, {"vram_slots", "870"},
               {"vram_gb", "12.5"}, {"ram_slots", "6000"}, {"ram_gb", "24.0"}},
       6000.0, 24.0 * kGiB, 870.0},
      // ... and once it decodes (generate.cpp:2121): the slots in use.
      {"glm decoding", {{"expert_slots", "870"}, {"vram_slots", "870"}, {"ram_slots", "6000"}, {"ram_gb", "24.0"},
                        {"vram_used", "850"}, {"ram_used", "5900"}, {"vram_gb", "12.5"}},
       5900.0, 24.0 * kGiB, 850.0},
      // DeepSeek (Maya ds4_model.cu:13433): expert_slots are the VRAM ones, ram_experts the RAM-held ones.
      {"deepseek", {{"expert_slots", "1200"}, {"cpu_experts", "9000"}, {"ram_experts", "7000"}}, 7000.0, std::nullopt, 1200.0},
      // ... decoding (ds4_main.cpp:1078): its ram_gb is decimal GB (tr.ram * cpu_blob / 1e9).
      {"deepseek decoding", {{"expert_slots", "1200"}, {"ram_experts", "7000"}, {"vram_used", "1180"}, {"ram_used", "6900"},
                             {"vram_gb", "10.50"}, {"ram_gb", "30.20"}},
       6900.0, 30.2e9, 1180.0},
      // MiniMax (Maya mm3_main.cpp:276): the resident and the pooled experts.
      {"minimax", {{"experts_vram", "500"}, {"experts", "15000"}, {"expert_slots", "500"}, {"ram_slots", "14500"}},
       14500.0, std::nullopt, 500.0},
      // ... decoding (mm3_main.cpp:437): ram_gb is the pinned pool in GiB.
      {"minimax decoding", {{"experts_vram", "500"}, {"ram_slots", "14500"}, {"vram_used", "510"}, {"ram_used", "14490"},
                            {"vram_gb", "9.00"}, {"ram_gb", "40.00"}},
       14490.0, 40.0 * kGiB, 510.0},
      // Gemma 4 and flint (g4_cli.cpp:234, f_cli.cpp:209): every expert pinned in RAM (pinned_gib), some in VRAM slots.
      {"gemma4", {{"expert_slots", "3840"}, {"expert_slots_primary", "870"}, {"pinned_gib", "13.25"}}, 3840.0, 13.25 * kGiB, 870.0},
      {"flint", {{"expert_slots", "12032"}, {"expert_slots_primary", "2000"}, {"pinned_gib", "40.00"}}, 12032.0, 40.0 * kGiB, 2000.0},
      // Qwen 3.6 MoE (q36_engine.cu:3679) prints no expert readings at all.
      {"qwen35moe", {}, std::nullopt, std::nullopt, std::nullopt},
  };
  for (const auto& row : rows) {
    Clocked c;
    c.monitor.on_engine_stats(row.readings);
    const auto json = c.json("ready");
    const auto* memory = at(at(&json, "engine"), "memory");
    expect(memory != nullptr, row.family + ": engine.memory while the engine is up");
    const auto check = [&](const char* key, const std::optional<double> want) {
      const auto* value = at(memory, key);
      const bool ok = want.has_value() ? close_to(number(value), *want) : is_null(value);
      expect(ok, row.family + ": " + key + " is " + (want ? std::to_string(*want) : std::string{"null"}));
    };
    check("experts_ram", row.ram);
    check("experts_ram_bytes", row.ram_bytes);
    check("experts_vram", row.vram);
  }
}

// "When the models are stop and nothing is loaded, the stats seem to stay": the engine's own readings (its context,
// its expert tier, its memory) show only while an engine is up - not while one starts, has failed or is unloaded.
void engine_readings_show_only_while_an_engine_is_up() {
  Clocked c;
  c.monitor.on_engine_stats({{"expert_slots", "3840"}, {"expert_slots_primary", "870"}, {"pinned_gib", "13.25"}});
  c.monitor.generation_started(10, 20, 8192, "Gemma");
  c.monitor.on_token(1);
  c.monitor.generation_ended("stop");
  const auto up = c.json("ready");
  expect(at(at(&up, "engine"), "experts") != nullptr && at(at(&up, "engine"), "memory") != nullptr &&
             number(at(at(&up, "engine"), "max_context")) == std::optional<double>{8192.0},
         "up: its readings");
  for (const char* engine : {"starting", "failed", "none"}) {
    const auto down = c.json(engine);
    const auto* block = at(&down, "engine");
    expect(at(block, "experts") == nullptr && at(block, "memory") == nullptr && is_null(at(block, "max_context")),
           std::string{engine} + ": none of the engine's readings");
    expect(size_of(at(&down, "requests")) == 1U, std::string{engine} + ": the history stays");
  }
}

// An engine the supervisor unloaded after idling (engine.idle_unload_s): /health still says "starting" (the next
// request loads it), the Monitor says unloaded, as upstream's does (serve/server.py:4611).
void an_unloaded_engine_reads_unloaded() {
  Clocked c;
  c.monitor.on_engine_stats({{"expert_slots", "384"}});
  const auto loaded = c.json("ready");
  expect(at(at(&loaded, "engine"), "loaded") != nullptr && at(at(&loaded, "engine"), "loaded")->if_boolean() != nullptr &&
             *at(at(&loaded, "engine"), "loaded")->if_boolean(),
         "engine.loaded while it is up");
  const std::string rendered = c.monitor.render_json({.model = "qwen-test", .engine = "starting", .queued = 0, .unloaded = true});
  const auto unloaded = parsed(rendered).value_or(secure_frontend::JsonValue{});
  expect(text(at(at(&unloaded, "live"), "state")) == "unloaded", "live.state unloaded: " + rendered.substr(0, 300));
  const auto* flag = at(at(&unloaded, "engine"), "unloaded");
  const auto* up = at(at(&unloaded, "engine"), "loaded");
  expect(flag != nullptr && flag->if_boolean() != nullptr && *flag->if_boolean() && up != nullptr && up->if_boolean() != nullptr && !*up->if_boolean(),
         "engine.unloaded true, engine.loaded false");
  expect(at(at(&unloaded, "engine"), "experts") == nullptr, "no expert readings of the engine that unloaded");
}

// The engine that is up, once a second (EngineSupervisor::engine_sample): its process's memory, and its startup
// readings per generation - a restarted engine's readings replace the last one's, its STAT values included.
void the_engine_sample_reaches_the_monitor() {
  Clocked c;
  secure_frontend::HostSample sample;
  sample.engine = secure_frontend::EngineSample{.generation = 1U, .readings = {{"expert_slots", "4232"}, {"arena_mib", "20480"}},
                                                .resident_bytes = 30ULL << 30U, .conversation_cache = std::nullopt};
  c.monitor.add_host_sample(sample);
  const auto first = c.json("ready");
  expect(close_to(number(at(at(at(&first, "engine"), "memory"), "process_bytes")), 30.0 * kGiB) &&
             close_to(number(at(at(at(&first, "engine"), "memory"), "experts_ram_bytes")), 20480.0 * kMiB),
         "the engine process's memory and its experts' share, before any chat");
  c.monitor.on_engine_stats({{"ram_used", "77"}});
  sample.engine->generation = 2U;
  sample.engine->readings = {{"expert_slots", "4000"}, {"arena_mib", "10240"}};
  c.monitor.add_host_sample(sample);
  const std::string second = c.monitor.render_json({.model = "q", .engine = "ready", .queued = 0});
  expect(second.find("\"experts\":{\"arena_mib\":10240,\"expert_slots\":4000}") != std::string::npos,
         "a new generation: its own readings only (the old one's STAT ram_used is gone): " + second.substr(0, 400));
  c.monitor.add_host_sample(secure_frontend::HostSample{});
  const auto gone = c.json("ready");
  expect(is_null(at(at(at(&gone, "engine"), "memory"), "process_bytes")), "no engine sample: no process memory");
}

// Upstream's disk reading (serve/telemetry.py:586-600, through psutil.disk_io_counters): every whole disk's
// /proc/diskstats sectors (x512), partitions not counted twice.
void diskstats_add_up_the_whole_disks() {
  const std::string text =
      "   8       0 sda 1000 10 2048 100 500 5 4096 50 0 150 150 0 0 0 0\n"
      "   8       1 sda1 900 10 2000 90 400 5 4000 40 0 130 130 0 0 0 0\n"
      " 259       0 nvme0n1 2000 0 8192 200 1000 0 16384 100 0 300 300 0 0 0 0 0 0\n"
      " 259       1 nvme0n1p1 1900 0 8000 190 900 0 16000 90 0 280 280 0 0 0 0 0 0\n"
      "   7       0 loop0 10 0 20 1 0 0 0 0\n"
      "   8      16 sdb 1 2 three 4 5 6 7 8 9 10 11 12 13 14 15\n"
      "garbage\n";
  const auto whole = [](const std::string_view name) { return name == "sda" || name == "nvme0n1" || name == "loop0" || name == "sdb"; };
  const auto devices = secure_frontend::parse_diskstats(text, whole);
  expect(devices.has_value() && devices->size() == 2U && devices->count("sda") == 1U && devices->count("nvme0n1") == 1U &&
             devices->at("sda").read_bytes == 2048ULL * 512ULL && devices->at("sda").write_bytes == 4096ULL * 512ULL &&
             devices->at("nvme0n1").read_bytes == 8192ULL * 512ULL && devices->at("nvme0n1").write_bytes == 16384ULL * 512ULL,
         "sda and nvme0n1 only: a partition is in its disk already; an 11-field or non-numeric line is skipped");
  expect(!secure_frontend::parse_diskstats(text, [](std::string_view) { return false; }).has_value() &&
             !secure_frontend::parse_diskstats("", whole).has_value(),
         "no whole disk: no reading (upstream's None)");
  using Devices = secure_frontend::DiskDevices;
  const auto disk = [](const std::uint64_t read, const std::uint64_t written) {
    return secure_frontend::DiskCounters{.read_bytes = read, .write_bytes = written};
  };
  secure_frontend::DiskMeter meter;
  const auto t0 = std::chrono::steady_clock::time_point{} + 100s;
  expect(!meter.next(Devices{{"sda", disk(1ULL << 30U, 1ULL << 29U)}}, t0).has_value(), "the first sample has nothing to compare with");
  const auto rates = meter.next(Devices{{"sda", disk((1ULL << 30U) + (4ULL << 20U), (1ULL << 29U) + (2ULL << 20U))}}, t0 + 2s);
  expect(rates.has_value() && close_to(rates->read_mb, 2.0) && close_to(rates->write_mb, 1.0), "MiB/s over the interval: 4 MiB in 2 s");
  expect(!meter.next(std::nullopt, t0 + 3s).has_value(), "no counters: no reading");
  expect(!meter.next(Devices{{"sda", disk(5U, 5U)}}, t0 + 4s).has_value(), "counters that went back: no reading");
  const auto after = meter.next(Devices{{"sda", disk(5U + (1ULL << 20U), 5U)}}, t0 + 5s);
  expect(after.has_value() && close_to(after->read_mb, 1.0) && close_to(after->write_mb, 0.0), "and on from the new counters");
  // A disk missing from one reading (unplugged, or it did not answer) never adds its lifetime count to one second.
  const auto two = meter.next(Devices{{"sda", disk(6U + (2ULL << 20U), 5U)}, {"sdb", disk(500ULL << 30U, 0U)}}, t0 + 6s);
  const auto gone = meter.next(Devices{{"sda", disk(6U + (3ULL << 20U), 5U)}}, t0 + 7s);
  const auto back = meter.next(Devices{{"sda", disk(6U + (4ULL << 20U), 5U)}, {"sdb", disk((500ULL << 30U) + (1ULL << 20U), 0U)}}, t0 + 8s);
  expect(two.has_value() && close_to(two->read_mb, 1.0) && gone.has_value() && close_to(gone->read_mb, 1.0) && back.has_value() &&
             close_to(back->read_mb, 1.0),
         "sdb's 500 GB since boot never shows as one second's reads");
  const auto real = secure_frontend::read_disk_counters();
  std::cerr << "note: this machine's disk counters " << (real ? "read" : "are not readable") << '\n';
}

// /metrics: hardware.disk_read_mb and disk_write_mb (MiB/s, null when unknown) and history.disk_read_mb, as upstream's.
void disk_readings_reach_the_json() {
  Clocked c;
  secure_frontend::HostSample sample;
  sample.disk_read_mb = 12.5;
  sample.disk_write_mb = 3.25;
  c.monitor.add_host_sample(sample);
  const auto json = c.json();
  expect(close_to(number(at(at(&json, "hardware"), "disk_read_mb")), 12.5) && close_to(number(at(at(&json, "hardware"), "disk_write_mb")), 3.25),
         "the newest rates");
  expect(size_of(at(at(&json, "history"), "disk_read_mb")) == 1U && close_to(number(item(at(at(&json, "history"), "disk_read_mb"), 0)), 12.5),
         "the read rate's history");
  c.monitor.add_host_sample(secure_frontend::HostSample{});
  const auto unknown = c.json();
  expect(is_null(at(at(&unknown, "hardware"), "disk_read_mb")) && is_null(at(at(&unknown, "hardware"), "disk_write_mb")) &&
             is_null(item(at(at(&unknown, "history"), "disk_read_mb"), 1)),
         "unknown: null");
}

// The engine's conversation cache lines (generate.cpp:9519, :9650, :12953; upstream ConvCacheLog's patterns), whole
// lines only, numbers only.
void conversation_cache_lines_parse_exactly() {
  using secure_frontend::ConversationCacheEvent;
  const auto parked = secure_frontend::parse_conversation_cache_line(
      "strata serve: conversation cache: parked 1234 tokens in 12.5 ms; parked=2 bytes=1048576 evictions=1 snapshot_bytes=524288 "
      "reused_kv_bytes=0");
  expect(parked.has_value() && parked->event == ConversationCacheEvent::parked && parked->tokens == 1234U && parked->parked == 2U &&
             parked->bytes == std::optional<std::uint64_t>{1048576U} && parked->evictions == std::optional<std::uint64_t>{1U},
         "a park");
  const auto skipped = secure_frontend::parse_conversation_cache_line(
      "strata serve: conversation cache: skipped 99 tokens in 0.4 ms; parked=2 bytes=1048576 evictions=1 snapshot_bytes=0 reused_kv_bytes=0");
  expect(skipped.has_value() && skipped->event == ConversationCacheEvent::skipped && skipped->tokens == 99U, "a skip");
  for (const char* from : {"live", "checkpoint"}) {
    const auto restored = secure_frontend::parse_conversation_cache_line(
        std::string{"strata serve: conversation cache: restored 1200 tokens ("} + from + ") in 3.0 ms; parked=1 bytes=524288");
    expect(restored.has_value() && restored->event == ConversationCacheEvent::restored && restored->tokens == 1200U &&
               restored->parked == 1U && restored->bytes == std::optional<std::uint64_t>{524288U} && !restored->evictions,
           std::string{"a restore from "} + from);
  }
  const auto copies = secure_frontend::parse_conversation_cache_line(
      "strata serve: conversation cache: dropped 2 superseded copies of this conversation; parked=3");
  const auto copy = secure_frontend::parse_conversation_cache_line(
      "strata serve: conversation cache: dropped 1 superseded copy of this conversation; parked=0");
  expect(copies.has_value() && copies->event == ConversationCacheEvent::dropped && copies->parked == 3U && copy.has_value() && copy->parked == 0U,
         "a drop");
  // upstream's pattern takes a park without the newer tail fields too
  expect(secure_frontend::parse_conversation_cache_line("strata serve: conversation cache: parked 5 tokens in 1.0 ms; parked=1 bytes=2").has_value() &&
             secure_frontend::parse_conversation_cache_line(
                 "strata serve: conversation cache: parked 5 tokens in 1.0 ms; parked=1 bytes=2 evictions=0").has_value(),
         "the shorter tails upstream's pattern allows");
  for (const std::string& refused : std::vector<std::string>{
           "",
           "strata serve: conversation cache: skip parking (12 tokens, below the 512 minimum; parked=1)",
           "strata serve: conversation cache: parked 12 tokens in 1.0 ms; parked=1 bytes=2 evictions=3 snapshot_bytes=4 reused_kv_bytes=5 user text",
           "strata serve: conversation cache: parked 12 tokens in 1.0 ms; parked=1 bytes=2 user text",
           "xstrata serve: conversation cache: parked 12 tokens in 1.0 ms; parked=1 bytes=2",
           " strata serve: conversation cache: parked 12 tokens in 1.0 ms; parked=1 bytes=2",
           "strata serve: conversation cache: parked -1 tokens in 1.0 ms; parked=1 bytes=2",
           "strata serve: conversation cache: parked 12 tokens in ms; parked=1 bytes=2",
           "strata serve: conversation cache: parked 12 tokens in 1.0 ms; parked=1 bytes=99999999999999999999999",
           "strata serve: conversation cache: parked 12 tokens in 1.0 ms; parked=1 bytes=2\r",
           "strata serve: conversation cache: restored 12 tokens (a secret prompt) in 1.0 ms; parked=1 bytes=2",
           "strata serve: conversation cache: restored 12 tokens (live) in 1.0 ms; parked=1 bytes=2 evictions=1",
           "strata serve: conversation cache: dropped 2 superseded copies of this conversation; parked=3 bytes=4",
           "strata serve: conversation cache: dropped 2 superseded copies of that conversation; parked=3",
           "strata serve: conversation cache: borrowed 100 tokens from a parked conversation",
           "strata serve: session 0: prompt 1200 tokens = 1000 reused + 200 read in 20 ms",
       }) {
    expect(!secure_frontend::parse_conversation_cache_line(refused).has_value(), "refused: [" + refused + "]");
  }
  secure_frontend::ConversationCacheState state;
  secure_frontend::apply_conversation_cache_line(state, *parked, 100);
  secure_frontend::apply_conversation_cache_line(state, *skipped, 101);
  expect(state.parks == 1U && state.restores == 0U && state.last_event == std::optional<ConversationCacheEvent>{ConversationCacheEvent::parked} &&
             state.last_tokens == std::optional<std::uint64_t>{1234U} && state.last_at == std::optional<std::int64_t>{100} &&
             state.parked == 2U && state.bytes == 1048576U && state.evictions == 1U,
         "a skip counts nothing and is not the last event");
  secure_frontend::apply_conversation_cache_line(state, *secure_frontend::parse_conversation_cache_line(
                                                            "strata serve: conversation cache: restored 1200 tokens (live) in 3.0 ms; parked=1 bytes=524288"),
                                                 102);
  secure_frontend::apply_conversation_cache_line(state, *copies, 103);
  expect(state.restores == 1U && state.last_event == std::optional<ConversationCacheEvent>{ConversationCacheEvent::restored} &&
             state.last_tokens == std::optional<std::uint64_t>{1200U} && state.last_at == std::optional<std::int64_t>{102} &&
             state.parked == 3U && state.bytes == 524288U && state.evictions == 1U,
         "a restore counts; a drop sets only the parked count");
}

// /metrics' conversation_cache: upstream's object (serve/server.py:827-837) field for field - the engine's INFO
// budget and slots, what its stderr told, and how much of the prompts the requests reused.
void conversation_cache_reaches_the_json() {
  Clocked c;
  const auto none = c.json("none");
  const auto* off = at(&none, "conversation_cache");
  expect(off != nullptr && off->if_boolean() == nullptr && at(off, "enabled") != nullptr && at(off, "enabled")->if_boolean() != nullptr &&
             !*at(off, "enabled")->if_boolean() && is_null(at(off, "budget_mib")) && is_null(at(off, "slots")) &&
             number(at(off, "parked")) == std::optional<double>{0.0} && number(at(off, "requests")) == std::optional<double>{0.0} &&
             is_null(at(off, "last_event")) && is_null(at(off, "last_reused")),
         "no engine: not enabled, nothing parked");
  c.monitor.on_engine_stats({{"conversation_cache_mib", "8192"}, {"conversation_cache_slots", "4"}});
  secure_frontend::ConversationCacheState state;
  state.parked = 2U;
  state.bytes = 1ULL << 30U;
  state.parks = 3U;
  state.restores = 1U;
  state.evictions = 1U;
  state.last_event = secure_frontend::ConversationCacheEvent::restored;
  state.last_tokens = 1200U;
  state.last_at = 1'700'000'005;
  secure_frontend::HostSample sample;
  // the engine's READY readings ride with every sample (a new generation's replace the last one's)
  sample.engine = secure_frontend::EngineSample{.generation = 1U,
                                                .readings = {{"conversation_cache_mib", "8192"}, {"conversation_cache_slots", "4"}},
                                                .resident_bytes = std::nullopt, .conversation_cache = state};
  c.monitor.add_host_sample(sample);
  c.monitor.generation_started(1200, 50, 8192);
  c.monitor.on_token(1);
  c.monitor.on_done({.prompt_tokens = 1200U, .prompt_ms = 10.0, .decode_ms = 10.0, .reused = 1000U});
  c.monitor.generation_ended("stop");
  c.monitor.generation_started(300, 50, 8192);
  c.monitor.on_token(1);
  c.monitor.on_done({.prompt_tokens = 300U, .prompt_ms = 10.0, .decode_ms = 10.0, .reused = 0U});
  c.monitor.generation_ended("stop");
  const auto on = c.json("ready");
  const auto* cache = at(&on, "conversation_cache");
  const auto boolean = [](const secure_frontend::JsonValue* value) { return value != nullptr && value->if_boolean() != nullptr && *value->if_boolean(); };
  expect(boolean(at(cache, "enabled")) && number(at(cache, "budget_mib")) == std::optional<double>{8192.0} &&
             number(at(cache, "slots")) == std::optional<double>{4.0} && boolean(at(cache, "supported")),
         "the engine's budget and slots");
  expect(number(at(cache, "parked")) == std::optional<double>{2.0} && close_to(number(at(cache, "bytes")), kGiB) &&
             number(at(cache, "parks")) == std::optional<double>{3.0} && number(at(cache, "restores")) == std::optional<double>{1.0} &&
             number(at(cache, "evictions")) == std::optional<double>{1.0} && text(at(cache, "last_event")) == "restored" &&
             number(at(cache, "last_tokens")) == std::optional<double>{1200.0} && number(at(cache, "last_at")) == std::optional<double>{1'700'000'005.0},
         "what its stderr told");
  expect(number(at(cache, "requests")) == std::optional<double>{2.0} && number(at(cache, "requests_reused")) == std::optional<double>{1.0} &&
             number(at(cache, "reused_tokens")) == std::optional<double>{1000.0} && number(at(cache, "prompt_tokens")) == std::optional<double>{1500.0} &&
             number(at(cache, "last_reused")) == std::optional<double>{0.0} && number(at(cache, "last_prompt")) == std::optional<double>{300.0},
         "the requests' reuse, the newest last");
  const auto stopped = c.json("none");
  const auto* after = at(&stopped, "conversation_cache");
  expect(at(after, "enabled") != nullptr && at(after, "enabled")->if_boolean() != nullptr && !*at(after, "enabled")->if_boolean() &&
             number(at(after, "parked")) == std::optional<double>{0.0} && is_null(at(after, "last_event")) &&
             number(at(after, "requests")) == std::optional<double>{2.0},
         "stopped: nothing parked any more; the requests' history stays");
  Clocked other;
  other.monitor.on_engine_stats({{"expert_slots", "3840"}});
  const auto gemma = other.json("ready");
  const auto* supported = at(at(&gemma, "conversation_cache"), "supported");
  expect(supported != nullptr && supported->if_boolean() != nullptr && !*supported->if_boolean(), "an engine without the cache says so");
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"parses_nvidia_smi_lines", parses_nvidia_smi_lines},
      {"the_live_speed_is_the_last_two_seconds", the_live_speed_is_the_last_two_seconds},
      {"engine_readings_reach_the_engine_block", engine_readings_reach_the_engine_block},
      {"engine_change_drops_the_old_engines_readings", engine_change_drops_the_old_engines_readings},
      {"nvidia_smi_command_is_fixed", nvidia_smi_command_is_fixed},
      {"nvidia_smi_environment_is_program_files_only", nvidia_smi_environment_is_program_files_only},
      {"monitor_records_generations", monitor_records_generations},
      {"monitor_reports_live_states", monitor_reports_live_states},
      {"a_stopped_engine_reads_stopped_and_requests_name_their_model", a_stopped_engine_reads_stopped_and_requests_name_their_model},
      {"monitor_keeps_a_bounded_history", monitor_keeps_a_bounded_history},
      {"host_readings_are_plausible", host_readings_are_plausible},
      {"gpu_sampler_reads_a_fake_tool", gpu_sampler_reads_a_fake_tool},
      {"gpu_sampler_reads_the_real_tool_when_present", gpu_sampler_reads_the_real_tool_when_present},
      {"engine_memory_reads_every_familys_ram_tier", engine_memory_reads_every_familys_ram_tier},
      {"engine_readings_show_only_while_an_engine_is_up", engine_readings_show_only_while_an_engine_is_up},
      {"an_unloaded_engine_reads_unloaded", an_unloaded_engine_reads_unloaded},
      {"the_engine_sample_reaches_the_monitor", the_engine_sample_reaches_the_monitor},
      {"diskstats_add_up_the_whole_disks", diskstats_add_up_the_whole_disks},
      {"disk_readings_reach_the_json", disk_readings_reach_the_json},
      {"conversation_cache_lines_parse_exactly", conversation_cache_lines_parse_exactly},
      {"conversation_cache_reaches_the_json", conversation_cache_reaches_the_json},
  }, failures);
}
