#pragma once

// The Monitor (listener.monitor; owner request 2026-10-22, session log step 16): what
// GET /metrics tells a key holder, in upstream's /metrics shape (serve/server.py metrics()) so
// the page's Monitor tab reads like upstream's. The engine's state and the queue, the last
// requests' token counts and the engine's own timings, and this machine's CPU, RAM and GPU
// readings with a minute of history. Never a prompt, an answer, a client address or a key.
// In memory only and bounded (kMonitorKeptRequests requests, kMonitorHistorySamples samples
// per series); gone when the server stops.

#include "secure_frontend/engine_protocol_client.hpp"
#include "secure_frontend/generation_run.hpp"
#include "secure_frontend/host_readings.hpp"

#include <utility>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

inline constexpr std::size_t kMonitorKeptRequests = 100;
inline constexpr std::size_t kMonitorHistorySamples = 60;  // one a second: a minute

struct HostSample final {
  std::optional<double> cpu_percent{};
  std::optional<MemoryReading> memory{};
  std::optional<GpuReading> gpu{};
  std::string gpu_note{};  // why there is no GPU reading (GpuSampler::note); empty when there is
  // Every disk's reads and writes over the last second, MiB/s (DiskMeter; upstream's disk_read_mb / disk_write_mb).
  std::optional<double> disk_read_mb{};
  std::optional<double> disk_write_mb{};
  // The engine that is up; nullopt when none is (stopped, starting, failed, unloaded).
  std::optional<EngineSample> engine{};
};

// What the route adds at the moment of each request.
struct MonitorView final {
  std::string_view model;   // the served name
  // the /health literal: starting | ready | generating | failed; the launcher's "none" when no engine runs (the Monitor
  // then says stopped and leaves out what the engine that ran reported)
  std::string_view engine;
  std::size_t queued;       // GenerationGate::waiting()
  // The supervisor unloaded the engine after idling (engine.idle_unload_s): /health says "starting" (the next request
  // loads it); the Monitor says unloaded. `{false}`: designated initializers that omit it keep compiling (-Werror).
  bool unloaded{false};
};

// The engine's RAM and VRAM share in one shape (owner 2026-10-28: the RAM card shows the experts and the memory), from
// each family's own readings: experts_ram / experts_vram count experts, experts_ram_bytes is their bytes in RAM;
// nullopt where the engine says nothing (Qwen 3.6 MoE says nothing at all; upstream's engine gives only the bytes).
struct EngineMemoryView final {
  std::optional<std::uint64_t> experts_ram{};
  std::optional<std::uint64_t> experts_ram_bytes{};
  std::optional<std::uint64_t> experts_vram{};
};
[[nodiscard]] EngineMemoryView engine_memory_view(const std::map<std::string, std::string>& readings);

// The finish a generation ended with, as the Monitor names it (upstream's badges):
// stop | length | cancel | disconnect | error.
[[nodiscard]] std::string_view monitor_finish(RunEnd end, RunCancel cancel) noexcept;

class ServeMonitor final : public GenerationMeter {
 public:
  using SteadyNow = std::function<std::chrono::steady_clock::time_point()>;
  using UnixNow = std::function<std::int64_t()>;
  // Empty clocks are the real ones; tests pass their own.
  explicit ServeMonitor(std::size_t kept_requests = kMonitorKeptRequests, SteadyNow steady = {}, UnixNow unix_seconds = {});

  // ChatRoute, around one generation (the gate admits one at a time). A start while another
  // is open ends that one as error first. `model`: the served name, which its request row keeps.
  void generation_started(std::size_t prompt_tokens, std::size_t max_tokens, std::size_t context_tokens, std::string_view model = {});
  void generation_ended(std::string_view finish);  // one of monitor_finish's literals
  void on_token(std::size_t generated) override;
  void on_done(const DoneFigures& figures) override;
  // Engine readings (INFO before READY; the maya_118 STAT cadence): keep the whitelisted
  // NUMERIC fields for the page's expert-tier lines. Anything else never gets stored,
  // so it can never reach the JSON. Last value seen per key wins.
  void on_engine_stats(const std::map<std::string, std::string>& readings) override;

  // Model launcher (plan 2026-10-23): another engine is starting; the readings of the one
  // before it (expert tier, READY context) no longer apply. The history and host readings stay.
  void engine_changed();

  // The sampler, once a second.
  void add_host_sample(const HostSample& sample);

  [[nodiscard]] std::string render_json(const MonitorView& view) const;
  // GUI-first start-up (calibration): the newest finished generation's decode tok/s (the engine's own DONE timing,
  // else the token arrival times); nullopt before one finished.
  [[nodiscard]] std::optional<double> last_decode_tok_s() const;
  // The GPU's name and memory from the newest host reading ("<name>, <MiB> MiB"); empty without one.
  [[nodiscard]] std::string gpu_fact() const;

 private:
  struct Live final {
    std::string model;  // the served name (a card name, at most 64 characters)
    std::size_t prompt_tokens{0};
    std::size_t max_tokens{0};
    std::size_t generated{0};
    std::chrono::steady_clock::time_point started{};
    std::optional<std::chrono::steady_clock::time_point> first_token{};
    std::optional<std::chrono::steady_clock::time_point> last_token{};
    std::optional<DoneFigures> figures{};
    // the live rate's samples (upstream's self.rate: (time, generated) per token, the newest kRateSamples)
    std::deque<std::pair<std::chrono::steady_clock::time_point, std::size_t>> rate{};
  };
  struct Finished final {
    std::int64_t time{0};
    std::string model;        // the model it ran on (owner 2026-10-27)
    std::string_view finish;  // a static literal (monitor_finish)
    std::size_t prompt_tokens{0};
    std::size_t output_tokens{0};
    std::optional<std::size_t> reused{};
    std::optional<double> prompt_ms{};
    std::optional<double> decode_ms{};
    std::optional<double> decode_tok_s{};
    double duration_s{0.0};
  };
  struct Series final {
    std::deque<std::optional<double>> values;
    void push(std::optional<double> value, std::size_t cap);
  };

  void end_locked(std::string_view finish);
  void store_readings_locked(const std::map<std::string, std::string>& readings);
  [[nodiscard]] std::optional<double> live_tok_s_locked() const;

  const std::size_t kept_;
  SteadyNow steady_;
  UnixNow unix_;
  const std::int64_t since_;
  mutable std::mutex mutex_;
  std::optional<Live> live_;
  std::deque<Finished> requests_;  // newest first
  std::size_t context_tokens_{0};
  // Whitelisted numeric engine readings (expert_slots & friends), validated in
  // on_engine_stats; rendered into the engine block as `experts`.
  std::map<std::string, std::string> experts_;
  // The engine's conversation cache budget and slots (its INFO), validated like experts_.
  std::map<std::string, std::string> cache_info_;
  // The newest engine sample: its generation (a new one replaces the readings), its process memory, its cache.
  std::optional<std::uint64_t> engine_generation_{};
  std::optional<std::uint64_t> engine_resident_{};
  std::optional<ConversationCacheState> conversation_cache_{};
  // Totals since start.
  std::size_t total_requests_{0};
  std::size_t total_prompt_tokens_{0};
  std::size_t total_output_tokens_{0};
  std::size_t total_reused_{0};
  double total_prompt_ms_{0.0};
  double total_decode_ms_{0.0};
  // The newest host reading and its history.
  HostSample host_{};
  std::string gpu_name_;  // the last GPU name seen
  Series tok_s_, gpu_util_, gpu_mem_used_, gpu_temp_, gpu_power_, cpu_, disk_read_;
};

}  // namespace secure_frontend
