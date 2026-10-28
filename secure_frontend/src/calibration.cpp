#include "secure_frontend/calibration.hpp"

#include "secure_frontend/json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#endif

namespace secure_frontend {
namespace {

constexpr JsonLimits kResultLimits{.max_bytes = 16U * 1024U, .max_depth = 4U, .max_container_entries = 64U, .max_string_bytes = 512U,
                                   .allow_fractions = true};
// The options that never change the decode speed (sampling, thinking and the card's web search).
constexpr std::array<std::string_view, 6> kSpeedNeutral{"thinking", "temperature", "top_p", "top_k", "min_p", "web_search"};

std::string decimal(const double value) {
  char buffer[32];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
  std::string text = error == std::errc{} ? std::string{buffer, end} : std::string{"0"};
  if (text.find_first_of(".eE") == std::string::npos) text += ".0";
  return text;
}

std::string json_text(const std::string_view text) {
  std::string out;
  append_json_string(&out, text);
  return out;
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const std::size_t n = values.size();
  return n % 2U == 1U ? values[n / 2U] : (values[n / 2U - 1U] + values[n / 2U]) / 2.0;
}

std::vector<std::string> arguments_of(const std::vector<SweepStep>& plan, const std::map<std::string, std::string>& chosen) {
  std::vector<std::string> args;
  for (const auto& step : plan) {
    if (const auto found = chosen.find(step.flag); found != chosen.end()) {
      args.push_back(step.flag);
      args.push_back(found->second);
    }
  }
  return args;
}

bool all_digits(const std::string_view text) {
  return !text.empty() && text.size() <= 3U && std::all_of(text.begin(), text.end(), [](const char c) { return c >= '0' && c <= '9'; });
}

// The sweep's own flags with the values the engine takes (engine_arguments.cpp's bounds).
bool valid_pair(const std::string_view flag, const std::string_view value) {
  if (flag == "--cpu-lane") return value == "on" || value == "off";
  if (flag == "--pcie-frac") return value == "0" || value == "0.25" || value == "0.5" || value == "0.75" || value == "1";
  if (flag == "--cpu-threads") {
    if (!all_digits(value) || value.front() == '0') return false;
    int n = 0;
    std::from_chars(value.data(), value.data() + value.size(), n);
    return n >= 1 && n <= 256;
  }
  return false;
}

const JsonValue* member(const JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& entry : *members) {
    if (entry.key == key) return &entry.value;
  }
  return nullptr;
}

std::optional<double> number(const JsonValue* value) {
  if (value == nullptr) return std::nullopt;
  if (const auto* d = value->if_decimal()) return *d;
  if (const auto* i = value->if_integer()) return static_cast<double>(*i);
  return std::nullopt;
}

}  // namespace

std::vector<SweepStep> sweep_plan(const EngineProfile profile, const unsigned logical_cores) {
  const unsigned cores = std::max(1U, logical_cores);
  std::vector<std::string> threads;
  for (const unsigned n : {cores, cores * 2U / 3U, cores / 2U}) {
    const std::string text = std::to_string(std::max(1U, n));
    if (std::find(threads.begin(), threads.end(), text) == threads.end()) threads.push_back(text);
  }
  switch (profile) {
    case EngineProfile::flint:
      return {{"--cpu-lane", {"off", "on"}}, {"--pcie-frac", {"0", "0.25", "0.5", "0.75"}}, {"--cpu-threads", threads}};
    case EngineProfile::gemma4: return {{"--cpu-threads", threads}};
    case EngineProfile::qwen:
    case EngineProfile::glm53:
    case EngineProfile::deepseek:
    case EngineProfile::minimax:  // (maya-mm3 tunes its own prompt reading, --prefill-tune; no flag sweep)
    case EngineProfile::raon1: return {};
  }
  return {};
}

SweepOutcome run_sweep(const std::vector<SweepStep>& plan, const SweepMeasure& measure,
                       const std::function<void(const SweepProgress&)>& progress, const std::function<bool()>& cancelled,
                       const double keep_margin, const unsigned confirm_rounds) {
  std::size_t trials = 1U + 2U * confirm_rounds;
  for (const auto& step : plan) trials += step.values.size();
  std::size_t trial = 0;
  std::optional<double> best_seen;
  // One trial: asked to stop first; nullopt for a cancel (the outcome says so) or a failed start.
  bool stop = false;
  const auto run = [&](const std::string& stage, const std::vector<std::string>& args) -> std::optional<double> {
    if (stop || (cancelled && cancelled())) {
      stop = true;
      return std::nullopt;
    }
    ++trial;
    if (progress) progress(SweepProgress{stage, trial, trials, best_seen});
    auto score = measure(args);
    // a cancel that arrived during the trial (its engine was stopped early) ends the sweep as cancelled
    if (cancelled && cancelled()) {
      stop = true;
      return std::nullopt;
    }
    if (score && (!best_seen || *score > *best_seen)) best_seen = score;
    return score;
  };

  SweepOutcome outcome;
  const auto baseline = run("defaults", {});
  if (stop) {
    outcome.end = SweepOutcome::End::cancelled;
    return outcome;
  }
  if (!baseline) {
    outcome.end = SweepOutcome::End::failed;
    return outcome;
  }
  outcome.baseline_tok_s = *baseline;
  std::map<std::string, std::string> chosen;
  double current = *baseline;
  for (const auto& step : plan) {
    for (const auto& value : step.values) {
      auto candidate = chosen;
      candidate[step.flag] = value;
      const auto score = run(step.flag, arguments_of(plan, candidate));
      if (stop) {
        outcome.end = SweepOutcome::End::cancelled;
        return outcome;
      }
      if (score && *score > current * (1.0 + keep_margin)) {
        chosen = std::move(candidate);
        current = *score;
      }
    }
  }
  outcome.best_tok_s = outcome.baseline_tok_s;
  if (chosen.empty()) return outcome;
  // The confirmation: the defaults and the winner, interleaved; the winner keeps its place only when it repeats.
  const auto winner = arguments_of(plan, chosen);
  std::vector<double> defaults_scores{*baseline};
  std::vector<double> winner_scores;
  for (unsigned round = 0; round < confirm_rounds; ++round) {
    const auto a = run("confirming", {});
    const auto b = stop ? std::nullopt : run("confirming", winner);
    if (stop) {
      outcome.end = SweepOutcome::End::cancelled;
      return outcome;
    }
    if (a) defaults_scores.push_back(*a);
    if (b) winner_scores.push_back(*b);
  }
  if (winner_scores.empty()) return outcome;
  const double defaults_median = median(defaults_scores);
  const double winner_median = median(winner_scores);
  if (winner_median > defaults_median * (1.0 + keep_margin)) {
    outcome.args = winner;
    outcome.baseline_tok_s = defaults_median;
    outcome.best_tok_s = winner_median;
  } else {
    outcome.baseline_tok_s = defaults_median;
    outcome.best_tok_s = defaults_median;
  }
  return outcome;
}

CalibrationState compare_key(const CalibrationKey& stored, const CalibrationKey& now) {
  const bool gpu_changed = !stored.gpu.empty() && !now.gpu.empty() && stored.gpu != now.gpu;
  if (gpu_changed || stored.cpu != now.cpu || stored.ram_bytes != now.ram_bytes || stored.engine != now.engine ||
      stored.model != now.model) {
    return CalibrationState::stale;
  }
  return stored.options == now.options ? CalibrationState::fresh : CalibrationState::settings_changed;
}

std::string render_calibration(const CalibrationResult& r) {
  std::string args;
  for (std::size_t i = 0; i < r.args.size(); ++i) args += (i == 0U ? "" : ", ") + json_text(r.args[i]);
  return "{\n  \"schema_version\": 1,\n  \"card\": " + json_text(r.card) + ",\n  \"args\": [" + args + "],\n" +
         "  \"baseline_tok_s\": " + decimal(r.baseline_tok_s) + ",\n  \"best_tok_s\": " + decimal(r.best_tok_s) + ",\n" +
         "  \"date\": " + json_text(r.date) + ",\n  \"auto_recalibrate\": " + (r.auto_recalibrate ? "true" : "false") + ",\n" +
         "  \"key\": {\"gpu\": " + json_text(r.key.gpu) + ", \"cpu\": " + json_text(r.key.cpu) +
         ", \"ram_bytes\": " + std::to_string(r.key.ram_bytes) + ", \"engine\": " + json_text(r.key.engine) +
         ", \"model\": " + json_text(r.key.model) + ", \"options\": " + json_text(r.key.options) + "}\n}\n";
}

std::optional<CalibrationResult> parse_calibration(const std::string_view text) {
  const auto parsed = parse_json(text, kResultLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  if (root == nullptr || root->if_object() == nullptr) return std::nullopt;
  const auto* version = member(*root, "schema_version");
  if (version == nullptr || version->if_integer() == nullptr || *version->if_integer() != 1) return std::nullopt;
  CalibrationResult result;
  const auto text_of = [](const JsonValue* value) -> const std::string* { return value == nullptr ? nullptr : value->if_string(); };
  const auto* card = text_of(member(*root, "card"));
  const auto* date = text_of(member(*root, "date"));
  const auto* args = member(*root, "args");
  const auto baseline = number(member(*root, "baseline_tok_s"));
  const auto best = number(member(*root, "best_tok_s"));
  const auto* automatic = member(*root, "auto_recalibrate");
  const auto* key = member(*root, "key");
  if (card == nullptr || date == nullptr || args == nullptr || args->if_array() == nullptr || !baseline || !best || key == nullptr ||
      automatic == nullptr || automatic->if_boolean() == nullptr) {
    return std::nullopt;
  }
  result.card = *card;
  result.date = *date;
  result.baseline_tok_s = *baseline;
  result.best_tok_s = *best;
  result.auto_recalibrate = *automatic->if_boolean();
  for (const auto& arg : *args->if_array()) {
    if (arg.if_string() == nullptr) return std::nullopt;
    result.args.push_back(*arg.if_string());
  }
  if (result.args.size() % 2U != 0U) return std::nullopt;
  for (std::size_t i = 0; i < result.args.size(); i += 2U) {
    if (!valid_pair(result.args[i], result.args[i + 1U])) return std::nullopt;
  }
  const auto* gpu = text_of(member(*key, "gpu"));
  const auto* cpu = text_of(member(*key, "cpu"));
  const auto* engine = text_of(member(*key, "engine"));
  const auto* model = text_of(member(*key, "model"));
  const auto* options = text_of(member(*key, "options"));
  const auto* ram = member(*key, "ram_bytes");
  if (gpu == nullptr || cpu == nullptr || engine == nullptr || model == nullptr || options == nullptr || ram == nullptr ||
      ram->if_integer() == nullptr || *ram->if_integer() < 0) {
    return std::nullopt;
  }
  result.key = CalibrationKey{*gpu, *cpu, static_cast<std::uint64_t>(*ram->if_integer()), *engine, *model, *options};
  return result;
}

std::string file_fact(const std::filesystem::path& file) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec) return {};
  const auto written = std::filesystem::last_write_time(file, ec);
  if (ec) return {};
  return std::to_string(size) + ":" + std::to_string(written.time_since_epoch().count());
}

std::string cpu_fact() {
  std::string name;
#if defined(_WIN32)
  wchar_t buffer[256]{};
  DWORD bytes = sizeof(buffer);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", RRF_RT_REG_SZ,
                   nullptr, buffer, &bytes) == ERROR_SUCCESS) {
    for (const wchar_t* c = buffer; *c != L'\0'; ++c) name.push_back(*c >= 0x20 && *c < 0x7f ? static_cast<char>(*c) : '?');
  }
#else
  std::ifstream info("/proc/cpuinfo");
  std::string line;
  while (std::getline(info, line)) {
    if (line.rfind("model name", 0) != 0U) continue;
    const auto colon = line.find(':');
    if (colon == std::string::npos) break;
    for (const char c : line.substr(colon + 1U)) name.push_back(c >= 0x20 && c < 0x7f ? c : '?');
    break;
  }
#endif
  while (!name.empty() && name.front() == ' ') name.erase(name.begin());
  while (!name.empty() && name.back() == ' ') name.pop_back();
  if (name.size() > 128U) name.resize(128U);
  if (name.empty()) name = "unknown CPU";
  return name + ", " + std::to_string(std::thread::hardware_concurrency()) + " threads";
}

std::string options_fact(const ModelOptionValues& values) {
  std::string out;
  for (const auto& [name, value] : values) {
    if (std::find(kSpeedNeutral.begin(), kSpeedNeutral.end(), name) != kSpeedNeutral.end()) continue;
    std::string text;
    if (const auto* b = std::get_if<bool>(&value)) text = *b ? "true" : "false";
    else if (const auto* i = std::get_if<std::int64_t>(&value)) text = std::to_string(*i);
    else if (const auto* d = std::get_if<double>(&value)) text = decimal(*d);
    else text = std::get<std::string>(value);
    out += (out.empty() ? "" : ";") + name + "=" + text;
  }
  return out;
}

std::string utc_now() {
  const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &now);
#else
  gmtime_r(&now, &utc);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return buffer;
}

}  // namespace secure_frontend
