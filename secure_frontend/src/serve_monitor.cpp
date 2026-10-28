#include "secure_frontend/serve_monitor.hpp"

#include "secure_frontend/json.hpp"

#include <array>
#include <charconv>
#include <cmath>
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
constexpr std::array<std::string_view, 9> kExpertReadingKeys{
    "expert_slots", "expert_slots_primary", "expert_cache_mib", "expert_cache_primary_mib", "vram_free_mib",
    "vram_used", "ram_used", "vram_gb", "ram_gb"};

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

}  // namespace

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
}

void ServeMonitor::on_done(const DoneFigures& figures) {
  std::lock_guard lock(mutex_);
  if (live_.has_value()) live_->figures = figures;
}

void ServeMonitor::on_engine_stats(const std::map<std::string, std::string>& readings) {
  std::lock_guard lock(mutex_);
  for (const auto& key : kExpertReadingKeys) {
    const auto found = readings.find(std::string(key));
    if (found != readings.end() && is_plain_number(found->second)) experts_[std::string(key)] = found->second;
  }
}

void ServeMonitor::engine_changed() {
  std::lock_guard lock(mutex_);
  experts_.clear();
  context_tokens_ = 0;
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

std::optional<double> ServeMonitor::live_tok_s_locked() const {
  if (!live_.has_value() || !live_->first_token.has_value() || !live_->last_token.has_value() || live_->generated < 2U) return std::nullopt;
  const double between = milliseconds_between(*live_->first_token, *live_->last_token);
  if (between <= 0.0) return std::nullopt;
  return static_cast<double>(live_->generated - 1U) / (between / 1000.0);
}

void ServeMonitor::add_host_sample(const HostSample& sample) {
  std::lock_guard lock(mutex_);
  host_ = sample;
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
  append_key(&out, "engine", true);
  out.push_back('{');
  append_key(&out, "model", true);
  append_json_string(&out, view.model);
  append_key(&out, "max_context");
  append_optional_count(&out, stopped || context_tokens_ == 0U ? std::optional<std::size_t>{} : std::optional<std::size_t>{context_tokens_});
  append_key(&out, "health");
  append_json_string(&out, view.engine);
  // The engine's expert-tier readings (validated on store): the page renders
  // "N experts loaded" on the VRAM and RAM cards from these.
  if (!stopped && !experts_.empty()) {
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
  out.push_back('}');

  std::string_view state = "idle";
  if (stopped) state = "stopped";
  else if (view.engine == "starting") state = "starting";
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
  const std::array<std::pair<std::string_view, const Series*>, 6> series{{{"tok_s", &tok_s_},
                                                                          {"cpu", &cpu_},
                                                                          {"gpu_util", &gpu_util_},
                                                                          {"gpu_mem_used", &gpu_mem_used_},
                                                                          {"gpu_temp", &gpu_temp_},
                                                                          {"gpu_power", &gpu_power_}}};
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
