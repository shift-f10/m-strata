#include "secure_frontend/serve_monitor.hpp"

#include "secure_frontend/json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <thread>
#include <utility>

namespace secure_frontend {
namespace {

using Steady = std::chrono::steady_clock;

double milliseconds_between(const Steady::time_point from, const Steady::time_point to) {
  return std::chrono::duration<double, std::milli>(to - from).count();
}

void append_count(std::string* out, const std::uint64_t value) { out->append(std::to_string(value)); }

// The Monitor's expert-tier whitelist (owner: the VRAM and RAM cards show experts
// loaded): the engine's own key names, INFO split for the main engine and STAT for
// the GLM engine. Only plain numbers are stored; the page adds the words.
// Owner 2026-10-28 ("the System Ram reports MB usage only"): each family's own words for its RAM tier too - the GLM
// and raon1 INFO's ram_slots/vram_slots, DeepSeek's ram_experts, MiniMax's experts_vram, Gemma 4's and flint's
// pinned_gib, upstream's engine's arena_mib (engine_memory_view reads them).
constexpr std::array<std::string_view, 15> kExpertReadingKeys{
    "expert_slots", "expert_slots_primary", "expert_cache_mib", "expert_cache_primary_mib", "vram_free_mib",
    "vram_used", "ram_used", "vram_gb", "ram_gb", "ram_slots", "vram_slots", "ram_experts", "experts_vram",
    "pinned_gib", "arena_mib"};
// The conversation cache's INFO (upstream's engine, generate.cpp:11117): its budget and slots.
constexpr std::array<std::string_view, 2> kCacheReadingKeys{"conversation_cache_mib", "conversation_cache_slots"};

bool is_plain_number(const std::string_view value) {
  if (value.empty() || value.size() > 20U) return false;
  std::size_t index = value.front() == '-' ? 1U : 0U;
  bool digits = false;
  bool dot = false;
  for (; index < value.size(); ++index) {
    if (value[index] == '.') {
      if (dot) return false;
      dot = true;
      continue;
    }
    if (value[index] < '0' || value[index] > '9') return false;
    digits = true;
  }
  return digits;
}

// A stored reading as a number: plain, finite and not negative; nullopt otherwise.
std::optional<double> reading_number(const std::map<std::string, std::string>& readings, const std::string_view key) {
  const auto found = readings.find(std::string{key});
  if (found == readings.end() || !is_plain_number(found->second) || found->second.front() == '-') return std::nullopt;
  double value = 0.0;
  const char* const begin = found->second.data();
  const char* const end = begin + found->second.size();
  const auto [stop, error] = std::from_chars(begin, end, value);
  if (error != std::errc{} || stop != end || !std::isfinite(value)) return std::nullopt;
  return value;
}

// Whole units (a count, bytes) from a reading scaled by `unit`; nullopt past 2^62 (nothing real is that big).
std::optional<std::uint64_t> reading_units(const std::map<std::string, std::string>& readings, const std::string_view key,
                                           const double unit = 1.0) {
  const auto value = reading_number(readings, key);
  if (!value.has_value()) return std::nullopt;
  const double scaled = *value * unit;
  if (!std::isfinite(scaled) || scaled >= 4.6e18) return std::nullopt;
  return static_cast<std::uint64_t>(std::llround(scaled));
}

template <typename T>
std::optional<T> first_of(std::initializer_list<std::optional<T>> values) {
  for (const auto& value : values) {
    if (value.has_value()) return value;
  }
  return std::nullopt;
}

// Finite values only, two decimals; anything else is null (the page shows a dash).
void append_decimal(std::string* out, const std::optional<double> value) {
  if (!value.has_value() || !std::isfinite(*value)) {
    out->append("null");
    return;
  }
  std::array<char, 64> buffer{};
  const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), *value, std::chars_format::fixed, 2);
  if (error != std::errc{}) {
    out->append("null");
    return;
  }
  out->append(buffer.data(), end);
}

template <typename T>
void append_optional_count(std::string* out, const std::optional<T> value) {
  if (value.has_value()) append_count(out, static_cast<std::uint64_t>(*value));
  else out->append("null");
}

void append_key(std::string* out, const std::string_view key, const bool first = false) {
  if (!first) out->push_back(',');
  append_json_string(out, key);
  out->push_back(':');
}

// upstream's RATE_WINDOW_S, RATE_MIN_SPAN_S and its rate deque's maxlen
constexpr double kRateWindowMs = 2000.0;
constexpr double kRateMinSpanMs = 250.0;
constexpr std::size_t kRateSamples = 32U;

}  // namespace

// Each family names its tiers in its own words (the engine sources, read for this):
// - Gemma 4 and flint (g4_cli.cpp:234, f_cli.cpp:209): every routed expert is pinned in RAM (pinned_gib, GiB) and
//   expert_slots counts all of them; expert_slots_primary are the ones in VRAM slots.
// - upstream's engine, the Qwen cards (generate.cpp:11114): expert_slots is every VRAM tier; arena_mib is the experts'
//   RAM arena in MiB. It gives no count of them.
// - Maya's GLM and our raon1 (generate.cpp:1049, :2121; glm_driver.cpp:829): INFO vram_slots/ram_slots and ram_gb
//   (GiB), then STAT vram_used/ram_used while decoding. MiniMax (mm3_main.cpp:276, :437): INFO experts_vram/ram_slots,
//   STAT the same _used keys and ram_gb (GiB). DeepSeek (ds4_model.cu:13433, ds4_main.cpp:1078): INFO expert_slots
//   (VRAM) and ram_experts, STAT the _used keys and ram_gb in decimal GB (tr.ram * cpu_blob / 1e9).
// - Qwen 3.6 MoE (q36_engine.cu:3679) prints none.
EngineMemoryView engine_memory_view(const std::map<std::string, std::string>& readings) {
  constexpr double kGiB = 1073741824.0;
  constexpr double kMiB = 1048576.0;
  EngineMemoryView view;
  if (readings.count("pinned_gib") != 0U) {
    view.experts_ram = reading_units(readings, "expert_slots");
    view.experts_vram = reading_units(readings, "expert_slots_primary");
    view.experts_ram_bytes = reading_units(readings, "pinned_gib", kGiB);
  } else if (readings.count("arena_mib") != 0U) {
    view.experts_vram = reading_units(readings, "expert_slots");
    view.experts_ram_bytes = reading_units(readings, "arena_mib", kMiB);
  } else {
    view.experts_vram = first_of({reading_units(readings, "vram_used"), reading_units(readings, "vram_slots"),
                                  reading_units(readings, "experts_vram"), reading_units(readings, "expert_slots")});
    view.experts_ram = first_of({reading_units(readings, "ram_used"), reading_units(readings, "ram_slots"),
                                 reading_units(readings, "ram_experts")});
    view.experts_ram_bytes = reading_units(readings, "ram_gb", readings.count("ram_experts") != 0U ? 1e9 : kGiB);
  }
  return view;
}

std::string_view monitor_finish(const RunEnd end, const RunCancel cancel) noexcept {
  switch (end) {
    case RunEnd::stop: return "stop";
    case RunEnd::length: return "length";
    case RunEnd::cancelled: return cancel == RunCancel::client_gone ? "disconnect" : "cancel";
    case RunEnd::engine_error:
    case RunEnd::engine_unavailable:
    case RunEnd::undecodable: return "error";
  }
  return "error";
}

void ServeMonitor::Series::push(const std::optional<double> value, const std::size_t cap) {
  values.push_back(value);
  while (values.size() > cap) values.pop_front();
}

ServeMonitor::ServeMonitor(const std::size_t kept_requests, SteadyNow steady, UnixNow unix_seconds)
    : kept_(kept_requests == 0U ? 1U : kept_requests),
      steady_(steady ? std::move(steady) : SteadyNow{[] { return Steady::now(); }}),
      unix_(unix_seconds ? std::move(unix_seconds) : UnixNow{[] {
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
      }}),
      since_(unix_()) {}

void ServeMonitor::generation_started(const std::size_t prompt_tokens, const std::size_t max_tokens, const std::size_t context_tokens,
                                      const std::string_view model) {
  std::lock_guard lock(mutex_);
  if (live_.has_value()) end_locked("error");
  live_ = Live{.model = std::string{model}, .prompt_tokens = prompt_tokens, .max_tokens = max_tokens, .generated = 0U, .started = steady_()};
  if (context_tokens != 0U) context_tokens_ = context_tokens;
}

void ServeMonitor::generation_ended(const std::string_view finish) {
  std::lock_guard lock(mutex_);
  if (live_.has_value()) end_locked(finish);
}

void ServeMonitor::on_token(const std::size_t generated) {
  std::lock_guard lock(mutex_);
  if (!live_.has_value()) return;
  const auto now = steady_();
  if (!live_->first_token.has_value()) live_->first_token = now;
  live_->last_token = now;
  live_->generated = generated;
  live_->rate.emplace_back(now, generated);
  if (live_->rate.size() > kRateSamples) live_->rate.pop_front();
}

void ServeMonitor::on_done(const DoneFigures& figures) {
  std::lock_guard lock(mutex_);
  if (live_.has_value()) live_->figures = figures;
}

void ServeMonitor::on_engine_stats(const std::map<std::string, std::string>& readings) {
  std::lock_guard lock(mutex_);
  store_readings_locked(readings);
}

void ServeMonitor::store_readings_locked(const std::map<std::string, std::string>& readings) {
  for (const auto& key : kExpertReadingKeys) {
    const auto found = readings.find(std::string(key));
    if (found != readings.end() && is_plain_number(found->second)) experts_[std::string(key)] = found->second;
  }
  for (const auto& key : kCacheReadingKeys) {
    const auto found = readings.find(std::string(key));
    if (found != readings.end() && is_plain_number(found->second)) cache_info_[std::string(key)] = found->second;
  }
}

void ServeMonitor::engine_changed() {
  std::lock_guard lock(mutex_);
  experts_.clear();
  cache_info_.clear();
  context_tokens_ = 0;
  engine_generation_.reset();
  engine_resident_.reset();
  conversation_cache_.reset();
}

void ServeMonitor::end_locked(const std::string_view finish) {
  const Live live = *live_;
  live_.reset();
  const auto now = steady_();
  Finished record;
  record.time = unix_();
  record.model = live.model;
  record.finish = finish;
  const DoneFigures figures = live.figures.value_or(DoneFigures{});
  record.prompt_tokens = figures.prompt_tokens.value_or(live.prompt_tokens);
  record.output_tokens = live.generated;
  record.reused = figures.reused;
  // The engine's own timings when its DONE gave them; else this server's view of the
  // same intervals (GEN to the first token; the first token to the last).
  if (figures.prompt_ms.has_value() && *figures.prompt_ms > 0.0) {
    record.prompt_ms = figures.prompt_ms;
  } else if (live.first_token.has_value()) {
    record.prompt_ms = milliseconds_between(live.started, *live.first_token);
  }
  if (figures.decode_ms.has_value() && *figures.decode_ms > 0.0) {
    record.decode_ms = figures.decode_ms;
    if (live.generated > 0U) record.decode_tok_s = static_cast<double>(live.generated) / (*figures.decode_ms / 1000.0);
  } else if (live.first_token.has_value() && live.last_token.has_value() && live.generated >= 2U) {
    const double between = milliseconds_between(*live.first_token, *live.last_token);
    if (between > 0.0) {
      record.decode_ms = between;
      record.decode_tok_s = static_cast<double>(live.generated - 1U) / (between / 1000.0);
    }
  }
  record.duration_s = milliseconds_between(live.started, now) / 1000.0;
  ++total_requests_;
  total_prompt_tokens_ += record.prompt_tokens;
  total_output_tokens_ += record.output_tokens;
  total_reused_ += record.reused.value_or(0U);
  total_prompt_ms_ += record.prompt_ms.value_or(0.0);
  total_decode_ms_ += record.decode_ms.value_or(0.0);
  requests_.push_front(record);
  while (requests_.size() > kept_) requests_.pop_back();
}

// Owner 2026-10-29 (the Monitor's speed "does not seem to update in realtime": it was the mean since the first token):
// upstream's live rate (serve/server.py _rate_of) - the tokens over the samples of the last kRateWindowMs, once they
// span kRateMinSpanMs; before that, or after a stall, the mean since the first token (over at least kRateMinSpanMs).
std::optional<double> ServeMonitor::live_tok_s_locked() const {
  if (!live_.has_value() || !live_->first_token.has_value() || live_->generated < 2U) return std::nullopt;
  const auto now = steady_();
  const auto& rate = live_->rate;
  if (!rate.empty()) {
    const auto& newest = rate.back();
    const auto oldest = std::find_if(rate.begin(), rate.end(), [&now](const auto& sample) {
      return milliseconds_between(sample.first, now) <= kRateWindowMs;
    });
    if (oldest != rate.end()) {
      const double span = milliseconds_between(oldest->first, newest.first);
      if (span >= kRateMinSpanMs) {
        return std::max(0.0, (static_cast<double>(newest.second) - static_cast<double>(oldest->second)) / (span / 1000.0));
      }
    }
  }
  return static_cast<double>(live_->generated) / (std::max(kRateMinSpanMs, milliseconds_between(*live_->first_token, now)) / 1000.0);
}

void ServeMonitor::add_host_sample(const HostSample& sample) {
  std::lock_guard lock(mutex_);
  host_ = sample;
  host_.engine.reset();  // kept below in its own members
  if (sample.gpu.has_value()) gpu_name_ = sample.gpu->name;
  const auto gpu = [&sample](const auto field) -> std::optional<double> {
    if (!sample.gpu.has_value()) return std::nullopt;
    const auto value = (*sample.gpu).*field;
    if (!value.has_value()) return std::nullopt;
    return static_cast<double>(*value);
  };
  const std::size_t cap = kMonitorHistorySamples;
  tok_s_.push(live_tok_s_locked().value_or(0.0), cap);
  cpu_.push(sample.cpu_percent, cap);
  gpu_util_.push(gpu(&GpuReading::util_percent), cap);
  gpu_mem_used_.push(gpu(&GpuReading::mem_used_bytes), cap);
  gpu_temp_.push(gpu(&GpuReading::temp_c), cap);
  gpu_power_.push(gpu(&GpuReading::power_w), cap);
  disk_read_.push(sample.disk_read_mb, cap);
  if (sample.engine.has_value()) {
    // Another READY (a restart, a reload after an idle unload, another model): its own startup readings replace the
    // last engine's, the STAT values that engine added included.
    if (engine_generation_ != sample.engine->generation) {
      experts_.clear();
      cache_info_.clear();
      store_readings_locked(sample.engine->readings);
      engine_generation_ = sample.engine->generation;
    }
    engine_resident_ = sample.engine->resident_bytes;
    conversation_cache_ = sample.engine->conversation_cache;
  } else {
    engine_resident_.reset();
    conversation_cache_.reset();
  }
}

std::optional<double> ServeMonitor::last_decode_tok_s() const {
  std::lock_guard lock(mutex_);
  if (requests_.empty()) return std::nullopt;
  return requests_.front().decode_tok_s;
}

std::string ServeMonitor::gpu_fact() const {
  std::lock_guard lock(mutex_);
  if (!host_.gpu.has_value() || host_.gpu->name.empty()) return {};
  std::string fact = host_.gpu->name;
  if (host_.gpu->mem_total_bytes.has_value()) fact += ", " + std::to_string(*host_.gpu->mem_total_bytes / (1024U * 1024U)) + " MiB";
  return fact;
}

std::string ServeMonitor::render_json(const MonitorView& view) const {
  std::lock_guard lock(mutex_);
  const auto now = steady_();
  std::string out;
  out.reserve(4096U + requests_.size() * 200U);
  out.push_back('{');

  // No engine runs (the launcher's "none": stopped, or none started yet): what the engine that ran reported no longer
  // describes anything loaded (owner 2026-10-27).
  const bool stopped = view.engine == "none";
  // Owner 2026-10-28 ("when the models are stop and nothing is loaded, the stats seem to stay"): what the engine
  // reported describes it only while it is up - not while one starts, has failed (a crash, a restart's backoff, the
  // circuit open) or was unloaded after idling.
  const bool loaded = !stopped && (view.engine == "ready" || view.engine == "generating");
  const bool unloaded = !stopped && view.unloaded;
  append_key(&out, "engine", true);
  out.push_back('{');
  append_key(&out, "model", true);
  append_json_string(&out, view.model);
  append_key(&out, "max_context");
  append_optional_count(&out, !loaded || context_tokens_ == 0U ? std::optional<std::size_t>{} : std::optional<std::size_t>{context_tokens_});
  append_key(&out, "health");
  append_json_string(&out, view.engine);
  // The engine's expert-tier readings (validated on store): the page renders
  // "N experts loaded" on the VRAM and RAM cards from these.
  if (loaded && !experts_.empty()) {
    append_key(&out, "experts");
    out.push_back('{');
    bool first_expert = true;
    for (const auto& entry : experts_) {
      append_key(&out, entry.first, first_expert);
      first_expert = false;
      out += entry.second;
    }
    out.push_back('}');
  }
  append_key(&out, "loaded");
  out.append(loaded ? "true" : "false");
  append_key(&out, "unloaded");
  out.append(unloaded ? "true" : "false");
  if (loaded) {
    // The RAM and VRAM cards' figures in one shape for every family (engine_memory_view), and the engine
    // process's own resident memory.
    const EngineMemoryView memory = engine_memory_view(experts_);
    append_key(&out, "memory");
    out.push_back('{');
    append_key(&out, "experts_ram", true);
    append_optional_count(&out, memory.experts_ram);
    append_key(&out, "experts_ram_bytes");
    append_optional_count(&out, memory.experts_ram_bytes);
    append_key(&out, "experts_vram");
    append_optional_count(&out, memory.experts_vram);
    append_key(&out, "process_bytes");
    append_optional_count(&out, engine_resident_);
    out.push_back('}');
  }
  out.push_back('}');

  std::string_view state = "idle";
  if (stopped) state = "stopped";
  else if (unloaded) state = "unloaded";
  else if (view.engine == "starting") state = "starting";
  else if (view.engine == "calibrating") state = "calibrating";  // a calibration's trials (owner 2026-10-29)
  else if (view.engine == "failed") state = "error";
  else if (live_.has_value()) state = live_->first_token.has_value() ? "generating" : "reading";
  append_key(&out, "live");
  out.push_back('{');
  append_key(&out, "state", true);
  append_json_string(&out, state);
  append_key(&out, "queued");
  append_count(&out, view.queued);
  const bool busy = live_.has_value();
  append_key(&out, "prompt_tokens");
  append_optional_count(&out, busy ? std::optional<std::size_t>{live_->prompt_tokens} : std::nullopt);
  append_key(&out, "generated");
  append_optional_count(&out, busy ? std::optional<std::size_t>{live_->generated} : std::nullopt);
  append_key(&out, "max_tokens");
  append_optional_count(&out, busy ? std::optional<std::size_t>{live_->max_tokens} : std::nullopt);
  append_key(&out, "elapsed_s");
  append_decimal(&out, busy ? std::optional<double>{milliseconds_between(live_->started, now) / 1000.0} : std::nullopt);
  append_key(&out, "tok_s");
  append_decimal(&out, live_tok_s_locked());
  std::optional<double> prefill;
  if (busy && live_->first_token.has_value()) {
    const double ms = milliseconds_between(live_->started, *live_->first_token);
    if (ms > 0.0) prefill = static_cast<double>(live_->prompt_tokens) / (ms / 1000.0);
  }
  append_key(&out, "prefill_tok_s");
  append_decimal(&out, prefill);
  out.push_back('}');

  append_key(&out, "requests");
  out.push_back('[');
  bool first = true;
  for (const auto& record : requests_) {
    if (!first) out.push_back(',');
    first = false;
    out.push_back('{');
    append_key(&out, "time", true);
    out.append(std::to_string(record.time));
    append_key(&out, "model");
    append_json_string(&out, record.model);
    append_key(&out, "finish");
    append_json_string(&out, record.finish);
    append_key(&out, "prompt_tokens");
    append_count(&out, record.prompt_tokens);
    append_key(&out, "reused");
    append_optional_count(&out, record.reused);
    append_key(&out, "output_tokens");
    append_count(&out, record.output_tokens);
    append_key(&out, "prompt_ms");
    append_decimal(&out, record.prompt_ms);
    append_key(&out, "decode_ms");
    append_decimal(&out, record.decode_ms);
    append_key(&out, "decode_tok_s");
    append_decimal(&out, record.decode_tok_s);
    append_key(&out, "duration_s");
    append_decimal(&out, record.duration_s);
    out.push_back('}');
  }
  out.push_back(']');
  append_key(&out, "requests_kept");
  append_count(&out, requests_.size());

  append_key(&out, "totals");
  out.push_back('{');
  append_key(&out, "since", true);
  out.append(std::to_string(since_));
  append_key(&out, "requests");
  append_count(&out, total_requests_);
  append_key(&out, "prompt_tokens");
  append_count(&out, total_prompt_tokens_);
  append_key(&out, "output_tokens");
  append_count(&out, total_output_tokens_);
  append_key(&out, "reused");
  append_count(&out, total_reused_);
  append_key(&out, "prompt_ms");
  append_decimal(&out, total_prompt_ms_);
  append_key(&out, "decode_ms");
  append_decimal(&out, total_decode_ms_);
  out.push_back('}');

  // Upstream's conversation_cache_view (serve/server.py:827-837), field for field, plus `supported`: whether the
  // engine that is up has the cache at all (its INFO names it; null with none up). The parked conversations live in
  // the engine, so with none up nothing is parked.
  {
    const std::optional<double> budget = loaded ? reading_number(cache_info_, "conversation_cache_mib") : std::nullopt;
    const std::optional<double> slots = loaded ? reading_number(cache_info_, "conversation_cache_slots") : std::nullopt;
    const ConversationCacheState parked = loaded && conversation_cache_.has_value() ? *conversation_cache_ : ConversationCacheState{};
    const auto whole = [](const std::optional<double> value) {
      return value.has_value() ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(std::llround(*value))} : std::nullopt;
    };
    append_key(&out, "conversation_cache");
    out.push_back('{');
    append_key(&out, "enabled", true);
    out.append(budget.value_or(0.0) > 0.0 ? "true" : "false");
    append_key(&out, "budget_mib");
    append_optional_count(&out, whole(budget));
    append_key(&out, "slots");
    append_optional_count(&out, whole(slots));
    append_key(&out, "supported");
    out.append(!loaded ? "null" : cache_info_.count("conversation_cache_mib") != 0U ? "true" : "false");
    append_key(&out, "parked");
    append_count(&out, parked.parked);
    append_key(&out, "bytes");
    append_count(&out, parked.bytes);
    append_key(&out, "evictions");
    append_count(&out, parked.evictions);
    append_key(&out, "parks");
    append_count(&out, parked.parks);
    append_key(&out, "restores");
    append_count(&out, parked.restores);
    append_key(&out, "last_event");
    if (!parked.last_event.has_value()) out.append("null");
    else append_json_string(&out, *parked.last_event == ConversationCacheEvent::restored ? "restored" : "parked");
    append_key(&out, "last_tokens");
    append_optional_count(&out, parked.last_tokens);
    append_key(&out, "last_at");
    if (parked.last_at.has_value()) out.append(std::to_string(*parked.last_at));
    else out.append("null");
    std::size_t reused_requests = 0;
    for (const auto& record : requests_) {
      if (record.reused.value_or(0U) > 0U) ++reused_requests;
    }
    append_key(&out, "requests");
    append_count(&out, requests_.size());
    append_key(&out, "requests_reused");
    append_count(&out, reused_requests);
    append_key(&out, "reused_tokens");
    append_count(&out, total_reused_);
    append_key(&out, "prompt_tokens");
    append_count(&out, total_prompt_tokens_);
    append_key(&out, "last_reused");
    append_optional_count(&out, requests_.empty() ? std::nullopt : requests_.front().reused);
    append_key(&out, "last_prompt");
    append_optional_count(&out, requests_.empty() ? std::optional<std::size_t>{} : std::optional<std::size_t>{requests_.front().prompt_tokens});
    out.push_back('}');
  }

  const auto& gpu = host_.gpu;
  append_key(&out, "hardware");
  out.push_back('{');
  append_key(&out, "cpu", true);
  append_decimal(&out, host_.cpu_percent);
  append_key(&out, "ram_used");
  append_optional_count(&out, host_.memory.has_value() ? std::optional<std::uint64_t>{host_.memory->used_bytes} : std::nullopt);
  append_key(&out, "ram_total");
  append_optional_count(&out, host_.memory.has_value() ? std::optional<std::uint64_t>{host_.memory->total_bytes} : std::nullopt);
  append_key(&out, "gpu_util");
  append_decimal(&out, gpu.has_value() ? gpu->util_percent : std::nullopt);
  append_key(&out, "gpu_mem_used");
  append_optional_count(&out, gpu.has_value() ? gpu->mem_used_bytes : std::nullopt);
  append_key(&out, "gpu_mem_total");
  append_optional_count(&out, gpu.has_value() ? gpu->mem_total_bytes : std::nullopt);
  append_key(&out, "gpu_temp");
  append_decimal(&out, gpu.has_value() ? gpu->temp_c : std::nullopt);
  append_key(&out, "gpu_power");
  append_decimal(&out, gpu.has_value() ? gpu->power_w : std::nullopt);
  append_key(&out, "gpu_power_limit");
  append_decimal(&out, gpu.has_value() ? gpu->power_limit_w : std::nullopt);
  append_key(&out, "gpu_pcie_gen");
  append_optional_count(&out, gpu.has_value() ? gpu->pcie_gen : std::nullopt);
  append_key(&out, "gpu_pcie_gen_max");
  append_optional_count(&out, gpu.has_value() ? gpu->pcie_gen_max : std::nullopt);
  append_key(&out, "gpu_pcie_width");
  append_optional_count(&out, gpu.has_value() ? gpu->pcie_width : std::nullopt);
  // upstream's telemetry.py:631: every disk's reads and writes over the last second, MiB/s
  append_key(&out, "disk_read_mb");
  append_decimal(&out, host_.disk_read_mb);
  append_key(&out, "disk_write_mb");
  append_decimal(&out, host_.disk_write_mb);
  out.push_back('}');

  append_key(&out, "hardware_static");
  out.push_back('{');
  append_key(&out, "gpu_name", true);
  if (gpu_name_.empty()) out.append("null");
  else append_json_string(&out, gpu_name_);
  append_key(&out, "gpu_note");
  if (host_.gpu_note.empty()) out.append("null");
  else append_json_string(&out, host_.gpu_note);
  append_key(&out, "threads");
  append_count(&out, std::thread::hardware_concurrency());
  out.push_back('}');

  append_key(&out, "history");
  out.push_back('{');
  const std::array<std::pair<std::string_view, const Series*>, 7> series{{{"tok_s", &tok_s_},
                                                                          {"cpu", &cpu_},
                                                                          {"gpu_util", &gpu_util_},
                                                                          {"gpu_mem_used", &gpu_mem_used_},
                                                                          {"gpu_temp", &gpu_temp_},
                                                                          {"gpu_power", &gpu_power_},
                                                                          {"disk_read_mb", &disk_read_}}};
  bool first_series = true;
  for (const auto& [name, values] : series) {
    append_key(&out, name, first_series);
    first_series = false;
    out.push_back('[');
    bool first_value = true;
    for (const auto& value : values->values) {
      if (!first_value) out.push_back(',');
      first_value = false;
      append_decimal(&out, value);
    }
    out.push_back(']');
  }
  out.push_back('}');

  append_key(&out, "time");
  out.append(std::to_string(unix_()));
  out.append("}\n");
  return out;
}

}  // namespace secure_frontend
