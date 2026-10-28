// Session log step 16 (listener.monitor): the Monitor's meter, its JSON, and this machine's
// readings — CPU and RAM from the OS, the GPU from nvidia-smi as a separate child.
#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/host_readings.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/serve_monitor.hpp"
#include "support/test_support.hpp"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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
  for (const std::string& bad : {std::string{},std::string{"GPU, 1"}, std::string{"GPU, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10"},
                                std::string{"GPU, abc, 1, 2, 3, 4, 5, 6, 7, 8"}, std::string{"GPU, -1, 1, 2, 3, 4, 5, 6, 7, 8"},
                                std::string{"GP\x01U, 1, 1, 2, 3, 4, 5, 6, 7, 8"}, std::string(97U, 'G') + ", 1, 1, 2, 3, 4, 5, 6, 7, 8",
                                std::string{", 1, 1, 2, 3, 4, 5, 6, 7, 8"}, std::string{"GPU, 1e9, 1, 2, 3, 4, 5, 6, 7, 8"}}) {
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
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"parses_nvidia_smi_lines", parses_nvidia_smi_lines},
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
  }, failures);
}
