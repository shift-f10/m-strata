#include "secure_frontend/calibration.hpp"

#include "secure_frontend/json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

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

// The chosen candidates as engine settings, in the plan's order; a joint step's flags each with its own value.
std::vector<std::string> arguments_of(const std::vector<SweepStep>& plan, const std::map<std::string, std::string>& chosen) {
  std::vector<std::string> args;
  for (const auto& step : plan) {
    const auto found = chosen.find(step.flag);
    if (found == chosen.end()) continue;
    if (step.joint.empty()) {
      args.push_back(step.flag);
      args.push_back(found->second);
      continue;
    }
    std::vector<std::string> flags{step.flag};
    flags.insert(flags.end(), step.joint.begin(), step.joint.end());
    std::size_t at = 0;
    for (const auto& flag : flags) {
      const std::size_t space = found->second.find(' ', at);
      args.push_back(flag);
      args.push_back(found->second.substr(at, space == std::string::npos ? std::string::npos : space - at));
      if (space == std::string::npos) break;
      at = space + 1U;
    }
  }
  return args;
}

// Python's round (half to even), as upstream's and Maya's candidates are computed.
int python_round(const double value) { return static_cast<int>(std::nearbyint(value)); }

// Counts below the engine's own: `first`, then each share of it, at least 2, without repeats, in that order.
std::vector<int> fewer(const int own, const std::vector<double>& shares, const std::vector<int>& first = {}) {
  std::vector<int> out;
  const auto add = [&out, own](const int count) {
    if (count >= 2 && count < own && std::find(out.begin(), out.end(), count) == out.end()) out.push_back(count);
  };
  for (const int count : first) add(count);
  for (const double share : shares) add(python_round(own * share));
  return out;
}

std::vector<std::string> texts_of(std::vector<int> counts, const bool descending) {
  if (descending) std::sort(counts.begin(), counts.end(), std::greater<>{});
  std::vector<std::string> out;
  for (const int count : counts) out.push_back(std::to_string(count));
  return out;
}

// A whole number lo..hi in decimal, without a sign or leading zeros.
bool count_in(const std::string_view text, const int lo, const int hi) {
  if (text.empty() || text.size() > 5U || text.front() == '0' ||
      !std::all_of(text.begin(), text.end(), [](const char c) { return c >= '0' && c <= '9'; })) {
    return false;
  }
  int n = 0;
  std::from_chars(text.data(), text.data() + text.size(), n);
  return n >= lo && n <= hi;
}

// A share 0-1 with at most three decimals ("0", "1", "0.35", "1.0").
bool fraction(const std::string_view text) {
  if (text == "0" || text == "1") return true;
  if (text.size() < 3U || text.size() > 5U || text[1] != '.' || (text[0] != '0' && text[0] != '1')) return false;
  const std::string_view decimals = text.substr(2U);
  if (!std::all_of(decimals.begin(), decimals.end(), [](const char c) { return c >= '0' && c <= '9'; })) return false;
  return text[0] == '0' || std::all_of(decimals.begin(), decimals.end(), [](const char c) { return c == '0'; });
}

// The sweeps' own settings with the values their engines take (engine_arguments.cpp's bounds; the GLM engine's lane at
// most 64 threads, glm_fast_path.cu).
bool valid_pair(const std::string_view flag, const std::string_view value) {
  if (flag == "--cpu-lane") return value == "on" || value == "off";
  if (flag == "--pcie-frac" || flag == "--spec-min-p" || flag == "--adapt-decay" || flag == "glm.pcie_share") return fraction(value);
  if (flag == "--cpu-threads" || flag == "--pool-workers") return count_in(value, 1, 256);
  if (flag == "--adapt-every" || flag == "--adapt-swaps") return count_in(value, 1, 4096);
  if (flag == "glm.cpu_lane") return count_in(value, 1, 64);
  return false;
}

// The card config rows a sweep sets (the GLM engine's environment rows), and their kind.
enum class RowKind { none, decimal, integer };
RowKind row_kind(const std::string_view row) {
  if (row == "glm.pcie_share") return RowKind::decimal;
  if (row == "glm.cpu_lane") return RowKind::integer;
  return RowKind::none;
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

std::vector<SweepStep> sweep_plan(const EngineProfile profile, const SweepMachine& machine) {
  const unsigned cores = std::max(1U, machine.logical_cores);
  const int logical = static_cast<int>(cores);
  const int physical = static_cast<int>(std::clamp(machine.physical_cores, 1U, cores));
  std::vector<std::string> threads;
  for (const unsigned n : {cores, cores * 2U / 3U, cores / 2U}) {
    const std::string text = std::to_string(std::max(1U, n));
    if (std::find(threads.begin(), threads.end(), text) == threads.end()) threads.push_back(text);
  }
  switch (profile) {
    case EngineProfile::flint:
      return {{"--cpu-lane", {"off", "on"}}, {"--pcie-frac", {"0", "0.25", "0.5", "0.75"}}, {"--cpu-threads", threads}};
    case EngineProfile::gemma4: return {{"--cpu-threads", threads}};
    case EngineProfile::qwen: {
      // upstream's tools/calibrate.py: PCIE_FRACS, SPEC_MIN_PS, worker_candidates of the engine's own pool (every
      // physical core but the host's, generate.cpp --pool-workers 0), ADAPT_CANDIDATES on TIER_MAX_NEW-token answers
      std::vector<SweepStep> plan{{"--pcie-frac", {"0", "0.2", "0.35", "0.55", "0.75", "0.9", "1"}},
                                  {"--spec-min-p", {"0.3", "0.5", "0.7"}}};
      if (auto workers = fewer(physical - 1, {2.0 / 3.0, 0.5, 0.25}); !workers.empty()) {
        plan.push_back({"--pool-workers", texts_of(std::move(workers), false)});
      }
      plan.push_back({"--adapt-every", {"1 80 0.97", "1 160 0.97"}, {"--adapt-swaps", "--adapt-decay"}, 512U});
      return plan;
    }
    case EngineProfile::glm53:
    case EngineProfile::raon1: {
      // Maya's tools/calibrate_glm.py: PCIE_SHARES, then thread_candidates of the engine's own lane (glm_fast_path.cu:
      // the physical cores, two thirds of the CPUs where every core is one), each step ending below STOP_BELOW of its best
      const int lane = physical < logical ? physical : logical * 2 / 3;
      std::vector<SweepStep> plan{{"glm.pcie_share", {"0", "0.1", "0.25", "0.5", "0.75"}, {}, 128U, 0.7}};
      if (auto lanes = fewer(lane, {0.75, 2.0 / 3.0, 0.5, 0.25}); !lanes.empty()) {
        plan.push_back({"glm.cpu_lane", texts_of(std::move(lanes), true), {}, 128U, 0.7});
      }
      return plan;
    }
    case EngineProfile::deepseek:
    case EngineProfile::minimax: {
      // Maya's tools/calibrate_ds4_tp.py thread_candidates (tools/mm3/calibrate.py takes it as it is): the engine's own
      // count is the physical cores (ds4_model.hpp, mm3 CpuLane::auto_workers); a lane on every CPU tries 2 and 4 fewer
      // first; the fewest last, the sweep stopping at the first that gains nothing
      const bool every_cpu = physical >= logical - 1;
      auto counts = fewer(physical, {0.75, 0.5}, every_cpu ? std::vector<int>{physical - 2, physical - 4} : std::vector<int>{});
      if (counts.empty()) return {};
      return {{"--cpu-threads", texts_of(std::move(counts), true), {}, 128U, 1.0}};
    }
  }
  return {};
}

double sweep_keep_margin(const EngineProfile profile) noexcept {
  return profile == EngineProfile::deepseek || profile == EngineProfile::minimax ? 0.05 : 0.03;
}

bool calibrates_before_first_start(const EngineProfile profile) noexcept {
  return profile == EngineProfile::flint || profile == EngineProfile::gemma4;
}

unsigned physical_cores() {
  const unsigned logical = std::max(1U, std::thread::hardware_concurrency());
  unsigned count = 0;
#if defined(_WIN32)
  DWORD length = 0;
  (void)GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
  if (length != 0U) {
    std::vector<unsigned char> buffer(length);
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
                                         &length) != FALSE) {
      // each record is its own Size long - a core's is shorter than the struct (its union's largest member), so the
      // walk needs only the header in the buffer (the whole struct missed the last core: 11 of a 3900X's 12)
      for (DWORD offset = 0; offset + offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor) <= length;) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (info->Size == 0U || offset + info->Size > length) break;
        if (info->Relationship == RelationProcessorCore) ++count;
        offset += info->Size;
      }
    }
  }
#else
  // each (package, core) pair once
  std::vector<std::pair<std::string, std::string>> seen;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/sys/devices/system/cpu", ec)) {
    const std::string name = entry.path().filename().string();
    if (name.size() < 4U || name.rfind("cpu", 0) != 0U ||
        !std::all_of(name.begin() + 3, name.end(), [](const char c) { return c >= '0' && c <= '9'; })) {
      continue;
    }
    std::ifstream core(entry.path() / "topology" / "core_id");
    std::ifstream package(entry.path() / "topology" / "physical_package_id");
    std::string core_id;
    std::string package_id;
    if (!(core >> core_id) || !(package >> package_id)) continue;
    if (std::find(seen.begin(), seen.end(), std::pair{package_id, core_id}) == seen.end()) seen.emplace_back(package_id, core_id);
  }
  count = static_cast<unsigned>(seen.size());
#endif
  return count == 0U ? logical : std::min(count, logical);
}

bool apply_calibration(FrontendConfigDraft& draft, const std::vector<std::string>& settings) {
  for (std::size_t i = 0; i + 1U < settings.size(); i += 2U) {
    const std::string& key = settings[i];
    const std::string& value = settings[i + 1U];
    if (key.rfind("--", 0) == 0U) {
      auto found = draft.entries.find("engine.args");
      if (found == draft.entries.end()) {
        found = draft.entries.emplace("engine.args", ConfigDraftEntry{std::vector<std::string>{}, ConfigSource::file}).first;
      }
      auto* list = std::get_if<std::vector<std::string>>(&found->second.value);
      if (list == nullptr) return false;
      if (std::find(list->begin(), list->end(), key) != list->end()) continue;  // the card's own flag stays
      list->push_back(key);
      list->push_back(value);
      continue;
    }
    if (draft.entries.contains(key)) continue;  // the card's own row stays
    const RowKind kind = row_kind(key);
    if (kind == RowKind::none) continue;
    const auto parsed = parse_json(value, kResultLimits);
    const auto* number_value = std::get_if<JsonValue>(&parsed);
    const auto as_number = number_value == nullptr ? std::nullopt : number(number_value);
    if (!as_number) continue;
    if (kind == RowKind::decimal) {
      draft.entries.emplace(key, ConfigDraftEntry{*as_number, ConfigSource::file});
    } else {
      draft.entries.emplace(key, ConfigDraftEntry{static_cast<std::int64_t>(*as_number), ConfigSource::file});
    }
  }
  return true;
}

SweepOutcome run_sweep(const std::vector<SweepStep>& plan, const SweepMeasure& measure,
                       const std::function<void(const SweepProgress&)>& progress, const std::function<bool()>& cancelled,
                       const double keep_margin, const unsigned confirm_rounds) {
  constexpr unsigned kDefaultTokens = 128U;
  // every candidate, plus a measurement of the best so far wherever a step's answers are of another length
  std::size_t trials = 1U + 2U * confirm_rounds;
  unsigned tokens_seen = kDefaultTokens;
  for (const auto& step : plan) {
    trials += step.values.size() + (step.max_tokens != tokens_seen ? 1U : 0U);
    tokens_seen = step.max_tokens;
  }
  std::size_t trial = 0;
  std::optional<double> best_seen;
  // One trial: asked to stop first; nullopt for a cancel (the outcome says so) or a failed start.
  bool stop = false;
  const auto run = [&](const std::string& stage, const std::vector<std::string>& args, const unsigned max_tokens) -> std::optional<double> {
    if (stop || (cancelled && cancelled())) {
      stop = true;
      return std::nullopt;
    }
    ++trial;
    if (progress) progress(SweepProgress{stage, trial, trials, best_seen});
    auto score = measure(args, max_tokens);
    // a cancel that arrived during the trial (its engine was stopped early) ends the sweep as cancelled
    if (cancelled && cancelled()) {
      stop = true;
      return std::nullopt;
    }
    if (score && (!best_seen || *score > *best_seen)) best_seen = score;
    return score;
  };

  SweepOutcome outcome;
  const auto baseline = run("defaults", {}, kDefaultTokens);
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
  unsigned current_tokens = kDefaultTokens;
  for (const auto& step : plan) {
    if (step.max_tokens != current_tokens) {
      // the best so far on this step's answers, the reference its candidates must beat
      const auto again = run(step.flag, arguments_of(plan, chosen), step.max_tokens);
      if (stop) {
        outcome.end = SweepOutcome::End::cancelled;
        return outcome;
      }
      current_tokens = step.max_tokens;
      if (!again) {
        trials -= step.values.size();
        continue;
      }
      current = *again;
    }
    double step_best = current;
    for (std::size_t i = 0; i < step.values.size(); ++i) {
      auto candidate = chosen;
      candidate[step.flag] = step.values[i];
      const auto score = run(step.flag, arguments_of(plan, candidate), step.max_tokens);
      if (stop) {
        outcome.end = SweepOutcome::End::cancelled;
        return outcome;
      }
      if (!score) continue;
      if (*score > current * (1.0 + keep_margin)) {
        chosen = std::move(candidate);
        current = *score;
      }
      step_best = std::max(step_best, *score);
      if (step.stop_below > 0.0 && *score < step.stop_below * step_best) {
        trials -= step.values.size() - 1U - i;  // the step's later candidates are not measured
        break;
      }
    }
  }
  outcome.best_tok_s = outcome.baseline_tok_s;
  if (chosen.empty()) return outcome;
  // The confirmation: the defaults and the winner, interleaved, on answers as long as the winner's steps' longest; the
  // winner keeps its place only when it repeats.
  const auto winner = arguments_of(plan, chosen);
  unsigned confirm_tokens = kDefaultTokens;
  for (const auto& step : plan) {
    if (chosen.contains(step.flag)) confirm_tokens = std::max(confirm_tokens, step.max_tokens);
  }
  std::vector<double> defaults_scores;
  if (confirm_tokens == kDefaultTokens) defaults_scores.push_back(*baseline);
  std::vector<double> winner_scores;
  for (unsigned round = 0; round < confirm_rounds; ++round) {
    const auto a = run("confirming", {}, confirm_tokens);
    const auto b = stop ? std::nullopt : run("confirming", winner, confirm_tokens);
    if (stop) {
      outcome.end = SweepOutcome::End::cancelled;
      return outcome;
    }
    if (a) defaults_scores.push_back(*a);
    if (b) winner_scores.push_back(*b);
  }
  if (winner_scores.empty() || defaults_scores.empty()) return outcome;
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

namespace {

std::string key_json(const CalibrationKey& key) {
  return "{\"gpu\": " + json_text(key.gpu) + ", \"cpu\": " + json_text(key.cpu) + ", \"ram_bytes\": " + std::to_string(key.ram_bytes) +
         ", \"engine\": " + json_text(key.engine) + ", \"model\": " + json_text(key.model) + ", \"options\": " + json_text(key.options) + "}";
}

std::optional<CalibrationKey> key_of(const JsonValue* key) {
  if (key == nullptr) return std::nullopt;
  const auto text_of = [](const JsonValue* value) -> const std::string* { return value == nullptr ? nullptr : value->if_string(); };
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
  return CalibrationKey{*gpu, *cpu, static_cast<std::uint64_t>(*ram->if_integer()), *engine, *model, *options};
}

}  // namespace

std::string render_prefill(const PrefillRecord& r) {
  return "{\n  \"schema_version\": 1,\n  \"card\": " + json_text(r.card) + ",\n  \"date\": " + json_text(r.date) +
         ",\n  \"finished\": " + (r.finished ? "true" : "false") + ",\n  \"chunk\": " + std::to_string(r.chunk) +
         ",\n  \"gemm\": " + json_text(r.gemm) + ",\n  \"tok_s\": " + decimal(r.tok_s) + ",\n  \"default_tok_s\": " +
         decimal(r.default_tok_s) + ",\n  \"key\": " + key_json(r.key) + "\n}\n";
}

std::optional<PrefillRecord> parse_prefill(const std::string_view text) {
  const auto parsed = parse_json(text, kResultLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  if (root == nullptr || root->if_object() == nullptr) return std::nullopt;
  const auto* version = member(*root, "schema_version");
  const auto* card = member(*root, "card");
  const auto* date = member(*root, "date");
  const auto* finished = member(*root, "finished");
  const auto* chunk = member(*root, "chunk");
  const auto* gemm = member(*root, "gemm");
  const auto tok_s = number(member(*root, "tok_s"));
  const auto default_tok_s = number(member(*root, "default_tok_s"));
  const auto key = key_of(member(*root, "key"));
  if (version == nullptr || version->if_integer() == nullptr || *version->if_integer() != 1 || card == nullptr ||
      card->if_string() == nullptr || date == nullptr || date->if_string() == nullptr || finished == nullptr ||
      finished->if_boolean() == nullptr || chunk == nullptr || chunk->if_integer() == nullptr || *chunk->if_integer() < 0 ||
      *chunk->if_integer() > 65536 || gemm == nullptr || gemm->if_string() == nullptr || gemm->if_string()->size() > 16U ||
      !tok_s || !default_tok_s || !key) {
    return std::nullopt;
  }
  return PrefillRecord{*card->if_string(), *date->if_string(), *finished->if_boolean(), *chunk->if_integer(), *gemm->if_string(),
                       *tok_s, *default_tok_s, *key};
}

bool read_prefill_measurement(const std::string_view text, PrefillRecord& record) {
  // the engine's JSON also lists its trials and the tune file's path; only the kept settings and speeds are read
  constexpr JsonLimits kMeasurementLimits{.max_bytes = 64U * 1024U, .max_depth = 4U, .max_container_entries = 256U,
                                          .max_string_bytes = 4096U, .allow_fractions = true};
  const auto parsed = parse_json(text, kMeasurementLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  if (root == nullptr || root->if_object() == nullptr) return false;
  const auto* chunk = member(*root, "chunk");
  const auto* gemm = member(*root, "gemm");
  const auto tok_s = number(member(*root, "tok_s"));
  const auto default_tok_s = number(member(*root, "default_tok_s"));
  if (chunk == nullptr || chunk->if_integer() == nullptr || *chunk->if_integer() < 1 || *chunk->if_integer() > 65536 ||
      gemm == nullptr || gemm->if_string() == nullptr || (*gemm->if_string() != "blas" && *gemm->if_string() != "hipblaslt") ||
      !tok_s || !default_tok_s || !std::isfinite(*tok_s) || !std::isfinite(*default_tok_s)) {
    return false;
  }
  record.chunk = *chunk->if_integer();
  record.gemm = *gemm->if_string();
  record.tok_s = *tok_s;
  record.default_tok_s = *default_tok_s;
  return true;
}

std::string file_fact(const std::filesystem::path& file) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec) return {};
  const auto written = std::filesystem::last_write_time(file, ec);
  if (ec) return {};
  // (the count as a long long: libc++'s file clock may count in __int128, which to_string does not take; the same number
  // as before on MSVC and libstdc++, so the records already kept stay fresh)
  return std::to_string(size) + ":" + std::to_string(static_cast<long long>(written.time_since_epoch().count()));
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
