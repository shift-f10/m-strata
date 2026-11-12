#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <charconv>
#include <cmath>

namespace secure_frontend {

// Accuracy-first defaults, with a closed explicit opt-in channel for per-card
// speed controls. Arbitrary inherited STRATA_/MAYA_ aliases never opt in.
struct EngineSpeedSpec { std::string_view name; double minimum, maximum; bool integer, aliases; };
inline constexpr EngineSpeedSpec kEngineSpeedSettings[] = {
    {"ROUTE_TAIL_SKIP", 0, 9, true, true}, {"ROUTE_RESIDENT", 0, 1, false, true},
    {"ROUTE_PRIOR", 0, 1, false, true}, {"ROUTE_RESIDENT_MTP", 0, 1, true, true},
    {"GLM_TRUNK_KEEP", 1, 8, true, true}, {"GLM_DISK_SKIP_FROM", 0, 8, true, true},
    {"GLM_DISK_SKIP_W", 0, 1, false, true}, {"GLM_MISS_SKIP_W", 0, 1, false, true},
    {"GLM_MTP_KEEP", 0, 8, true, true}, {"GLM_DEFER_CPU", 0, 1, true, true},
    {"HC_REQ8", 0, 1, true, true}, {"SPEC_PROB", 0, 1, true, true},
    {"DS4_DISK_SKIP_W", 0, 1, false, true}, {"DS4_RAM_SKIP_W", 0, 1, false, true},
    {"DS4_DSPARK_SAMPLE", 0, 1, true, false}, {"DS4_YOCO", 0, 2, true, false},
    {"DS4_YOCO_KEEP", 0, 1048576, true, false},
};
struct EngineSpeedValue { const EngineSpeedSpec* spec; std::string value; };
inline bool parse_engine_speed_settings(std::string_view encoded, std::vector<EngineSpeedValue>& out) {
  out.clear();
  if (encoded.size() > 4096U) return false;
  while (!encoded.empty()) {
    const auto split = encoded.find(';');
    const auto entry = encoded.substr(0, split);
    const auto equal = entry.find('=');
    if (equal == std::string_view::npos) return false;
    const auto name = entry.substr(0, equal), value = entry.substr(equal + 1U);
    const EngineSpeedSpec* spec = nullptr;
    for (const auto& candidate : kEngineSpeedSettings) if (candidate.name == name) spec = &candidate;
    if (spec == nullptr || value.empty()) return false;
    for (const auto& previous : out) if (previous.spec == spec) return false;
    double number = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(number) ||
        number < spec->minimum || number > spec->maximum) return false;
    if (spec->integer) {
      // The backend's atoi readers cannot interpret decimal/exponent spellings.
      unsigned integer = 0;
      const auto parsed_integer = std::from_chars(value.data(), value.data() + value.size(), integer);
      if (parsed_integer.ec != std::errc{} || parsed_integer.ptr != value.data() + value.size()) return false;
    }
    out.push_back({spec, std::string(value)});
    if (split == std::string_view::npos) return true;
    encoded.remove_prefix(split + 1U);
    if (encoded.empty()) return false;
  }
  return true;
}
inline std::string encode_engine_speed_settings(const std::vector<std::string>& settings) {
  std::string out;
  for (const auto& setting : settings) {
    if (!out.empty()) out += ';';
    out += setting;
  }
  return out;
}
inline bool parse_engine_speed_settings(const std::vector<std::string>& settings, std::vector<EngineSpeedValue>& out) {
  out.clear();
  if (settings.size() > sizeof(kEngineSpeedSettings) / sizeof(kEngineSpeedSettings[0])) return false;
  for (const auto& entry : settings) if (entry.empty() || entry.find(';') != std::string::npos) return false;
  return parse_engine_speed_settings(encode_engine_speed_settings(settings), out);
}

// Apply defaults then validated choices before loading any family library.
template <typename Set>
bool apply_engine_accuracy_policy(Set&& set, const std::string_view speed_settings = {}) {
  std::vector<EngineSpeedValue> selected;
  if (!parse_engine_speed_settings(speed_settings, selected)) return false;
  struct Setting { const char* suffix; const char* value; };
  constexpr Setting settings[] = {
      {"ROUTE_TAIL_SKIP", "0"}, {"ROUTE_RESIDENT", "0"},
      {"ROUTE_PRIOR", "0"}, {"ROUTE_RESIDENT_MTP", "0"},
      {"GLM_TRUNK_KEEP", "8"}, {"GLM_TRUNK_KEEP_MAP", ""},
      {"GLM_DISK_SKIP_FROM", "8"}, {"GLM_DISK_SKIP_W", "0"},
      {"GLM_MISS_SKIP_W", "0"}, {"GLM_MTP_KEEP", "8"},
      {"GLM_MTP_MISS", "1"}, {"DS4_DISK_SKIP_W", "0"},
      {"DS4_RAM_SKIP_W", "0"},
      {"GLM_DEFER_CPU", "0"}, {"HC_REQ8", "0"},
      {"DS4_PROMPT_SKIP", "1"},
      // Qwen's sampled-draft confidence gate conditions q on the chosen
      // token, but its verifier uses unconditioned q. Keep exact-match MTP.
      {"SPEC_PROB", "0"},
  };
  for (const auto& setting : settings) {
    if (!set((std::string("STRATA_") + setting.suffix).c_str(), setting.value)) return false;
    // Maya aliases override STRATA_ at entry. Pin both spellings, including
    // clearing the per-layer map, so inherited aliases cannot undo the policy.
    if (!set((std::string("MAYA_") + setting.suffix).c_str(), setting.value)) return false;
  }
  // DeepSeek's experimental sampled drafter has the same confidence-gate
  // issue and uses an unprefixed environment name.
  if (!(set("DS4_DSPARK_SAMPLE", "0") &&
         // Exact dependency-based prompt pruning, never bounded replay.
         set("DS4_YOCO", "1") && set("DS4_YOCO_KEEP", "0"))) return false;
  for (const auto& entry : selected) {
    if (entry.spec->aliases) {
      if (!set((std::string("STRATA_") + std::string(entry.spec->name)).c_str(), entry.value.c_str()) ||
          !set((std::string("MAYA_") + std::string(entry.spec->name)).c_str(), entry.value.c_str())) return false;
    } else if (!set(std::string(entry.spec->name).c_str(), entry.value.c_str())) return false;
  }
  return true;
}

}  // namespace secure_frontend
