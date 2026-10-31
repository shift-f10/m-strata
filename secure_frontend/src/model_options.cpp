#include "secure_frontend/model_options.hpp"

#include "secure_frontend/family_descriptor.hpp"
#include "secure_frontend/plugin_manifest.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace secure_frontend {
namespace {

using Kind = ModelOptionKind;

// KV streaming's floor: upstream/src/core/layer.cpp:771 qsa_kv_resident_min() (a smaller
// --kv-resident is raised to it, :752; usage generate.cpp:820 "min 20480").
constexpr std::int64_t kKvResidentMinimum = 20480;


// --- Values ---------------------------------------------------------------------------------

void add(ConfigErrors& errors, std::string key_path, const ConfigIssueCode code, std::string reason) {
  errors.push_back(ConfigIssue{std::move(key_path), code, std::move(reason)});
}

std::string option_path(const std::string_view name) { return "options." + std::string{name}; }

// Shortest round-trip decimal text.
std::string decimal_text(const double value) {
  char buffer[32];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return error == std::errc{} ? std::string{buffer, end} : std::string{"0"};
}

std::string_view kind_name(const Kind kind) {
  switch (kind) {
    case Kind::boolean: return "boolean";
    case Kind::integer: return "integer";
    case Kind::decimal: return "decimal";
    case Kind::choice: return "choice";
    case Kind::integer_or_auto: return "integer_or_auto";
    case Kind::devices: return "devices";
  }
  return "boolean";  // unreachable: every kind is a case above
}

std::string_view type_reason(const Kind kind) {
  switch (kind) {
    case Kind::boolean: return "must be true or false";
    case Kind::integer: return "must be an integer";
    case Kind::decimal: return "must be a number";
    case Kind::choice: return "must be a string";
    case Kind::integer_or_auto: return "must be an integer or \"auto\"";
    case Kind::devices: return "must be \"auto\" or GPU numbers 0-63 such as \"0,1\", none repeated";
  }
  return "has the wrong type";  // unreachable
}

std::string_view bounds_reason(const Kind kind) {
  return kind == Kind::choice ? "is not one of the card's choices" : "is outside the card's bounds";
}

// A devices value: "auto" (0 GPUs named), or 1-16 GPU numbers 0-63 separated by commas, none repeated; nullopt otherwise.
std::optional<std::size_t> device_count(const std::string_view text) {
  if (text == "auto") return 0U;
  std::vector<unsigned> seen;
  std::size_t begin = 0;
  for (;;) {
    const std::size_t comma = text.find(',', begin);
    const std::string_view part = text.substr(begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin);
    if (part.empty() || part.size() > 2U || !std::all_of(part.begin(), part.end(), [](const char c) { return c >= '0' && c <= '9'; })) {
      return std::nullopt;
    }
    const unsigned device = static_cast<unsigned>(part.size() == 1U ? part[0] - '0' : (part[0] - '0') * 10 + (part[1] - '0'));
    if (device > 63U || std::find(seen.begin(), seen.end(), device) != seen.end() || seen.size() == 16U) return std::nullopt;
    seen.push_back(device);
    if (comma == std::string_view::npos) return seen.size();
    begin = comma + 1U;
  }
}

// A JSON value read as the option's kind (no bounds yet): a decimal takes an integer literal,
// an integer refuses a fraction, integer_or_auto is an integer or exactly "auto".
std::optional<ModelOptionValue> typed(const Kind kind, const JsonValue& json) {
  switch (kind) {
    case Kind::boolean:
      if (const auto* value = json.if_boolean()) return ModelOptionValue{std::in_place_type<bool>, *value};
      break;
    case Kind::integer:
      if (const auto* value = json.if_integer()) return ModelOptionValue{std::in_place_type<std::int64_t>, *value};
      break;
    case Kind::decimal:
      if (const auto* value = json.if_decimal()) return ModelOptionValue{std::in_place_type<double>, *value};
      if (const auto* value = json.if_integer()) return ModelOptionValue{std::in_place_type<double>, static_cast<double>(*value)};
      break;
    case Kind::choice:
      if (const auto* value = json.if_string()) return ModelOptionValue{std::in_place_type<std::string>, *value};
      break;
    case Kind::integer_or_auto:
      if (const auto* value = json.if_integer()) return ModelOptionValue{std::in_place_type<std::int64_t>, *value};
      if (const auto* value = json.if_string(); value != nullptr && *value == "auto") {
        return ModelOptionValue{std::in_place_type<std::string>, "auto"};
      }
      break;
    case Kind::devices:
      if (const auto* value = json.if_string(); value != nullptr && device_count(*value)) {
        return ModelOptionValue{std::in_place_type<std::string>, *value};
      }
      break;
  }
  return std::nullopt;
}

// The same reading for a value already held (a remembered one, or a value to describe).
std::optional<ModelOptionValue> typed(const Kind kind, const ModelOptionValue& held) {
  switch (kind) {
    case Kind::boolean:
      if (std::holds_alternative<bool>(held)) return held;
      break;
    case Kind::integer:
      if (std::holds_alternative<std::int64_t>(held)) return held;
      break;
    case Kind::decimal:
      if (std::holds_alternative<double>(held)) return held;
      if (const auto* value = std::get_if<std::int64_t>(&held)) {
        return ModelOptionValue{std::in_place_type<double>, static_cast<double>(*value)};
      }
      break;
    case Kind::choice:
      if (std::holds_alternative<std::string>(held)) return held;
      break;
    case Kind::integer_or_auto:
      if (std::holds_alternative<std::int64_t>(held)) return held;
      if (const auto* value = std::get_if<std::string>(&held); value != nullptr && *value == "auto") return held;
      break;
    case Kind::devices:
      if (const auto* value = std::get_if<std::string>(&held); value != nullptr && device_count(*value)) return held;
      break;
  }
  return std::nullopt;
}

// A value of the limit's kind (from typed()) inside the card's bounds.
bool within(const ModelOptionLimits& limit, const ModelOptionValue& value) {
  if (const auto* integer = std::get_if<std::int64_t>(&value)) return *integer >= limit.minimum && *integer <= limit.maximum;
  if (const auto* decimal = std::get_if<double>(&value)) {
    return std::isfinite(*decimal) && *decimal >= limit.minimum_decimal && *decimal <= limit.maximum_decimal;
  }
  if (const auto* text = std::get_if<std::string>(&value)) {
    if (limit.spec->kind == Kind::integer_or_auto) return *text == "auto";
    if (limit.spec->kind == Kind::devices) {   // auto, or as many GPUs as the engine runs on
      const auto count = device_count(*text);
      return count.has_value() && static_cast<std::int64_t>(*count) <= limit.maximum;
    }
    return std::find(limit.choices.begin(), limit.choices.end(), *text) != limit.choices.end();
  }
  return true;  // a boolean has no bounds
}

// An integer on the option's list, when it has one (the page's dropdowns).
bool offered(const ModelOptionLimits& limit, const ModelOptionValue& value) {
  const auto* integer = std::get_if<std::int64_t>(&value);
  const auto values = limit.spec->values;
  return integer == nullptr || values.empty() || std::find(values.begin(), values.end(), *integer) != values.end();
}

// `held` as the limit's kind when it is inside the card's bounds and on the option's list (D7:
// else the default applies).
std::optional<ModelOptionValue> usable(const ModelOptionLimits& limit, const ModelOptionValues& values) {
  const auto it = values.find(limit.spec->name);
  if (it == values.end()) return std::nullopt;
  auto value = typed(limit.spec->kind, it->second);
  if (!value.has_value() || !within(limit, *value) || !offered(limit, *value)) return std::nullopt;
  return value;
}

const ModelOptionLimits* limit_named(const std::vector<ModelOptionLimits>& limits, const std::string_view name) {
  const auto it = std::find_if(limits.begin(), limits.end(),
                               [name](const ModelOptionLimits& limit) { return limit.spec != nullptr && limit.spec->name == name; });
  return it == limits.end() ? nullptr : &*it;
}

const JsonValue* member_named(const std::vector<JsonMember>& members, const std::string_view key) {
  const auto it = std::find_if(members.begin(), members.end(), [key](const JsonMember& member) { return member.key == key; });
  return it == members.end() ? nullptr : &it->value;
}

// --- A card's options -----------------------------------------------------------------------

bool bounded_kind(const Kind kind) { return kind == Kind::integer || kind == Kind::integer_or_auto || kind == Kind::decimal; }

bool is_option_member(const std::string_view key) { return key == "default" || key == "min" || key == "max" || key == "choices"; }

// A card's "min" or "max": the option's number type, inside the engine's hard bounds.
bool card_bound(const ModelOptionSpec& spec, const JsonValue& json, const std::string_view which, const std::string& path,
                ConfigErrors& errors, std::int64_t& integer, double& decimal) {
  if (spec.kind == Kind::decimal) {
    double value = 0.0;
    if (const auto* d = json.if_decimal()) value = *d;
    else if (const auto* i = json.if_integer()) value = static_cast<double>(*i);
    else return add(errors, path, ConfigIssueCode::wrong_type, std::string{which} + " must be a number"), false;
    if (!(value >= spec.minimum_decimal && value <= spec.maximum_decimal)) {
      return add(errors, path, ConfigIssueCode::out_of_range, std::string{which} + " is outside the engine's bounds"), false;
    }
    decimal = value;
    return true;
  }
  const auto* value = json.if_integer();
  if (value == nullptr) return add(errors, path, ConfigIssueCode::wrong_type, std::string{which} + " must be an integer"), false;
  if (*value < spec.minimum || *value > spec.maximum) {
    return add(errors, path, ConfigIssueCode::out_of_range, std::string{which} + " is outside the engine's bounds"), false;
  }
  integer = *value;
  return true;
}

// A card's "choices": a non-empty subset of the spec's, no repeats; kept in the spec's order.
bool card_choices(const ModelOptionSpec& spec, const JsonValue& json, const std::string& path, ConfigErrors& errors,
                  std::vector<std::string>& choices) {
  const auto* items = json.if_array();
  if (items == nullptr ||
      !std::all_of(items->begin(), items->end(), [](const JsonValue& item) { return item.if_string() != nullptr; })) {
    return add(errors, path, ConfigIssueCode::wrong_type, "choices must be an array of strings"), false;
  }
  if (items->empty()) return add(errors, path, ConfigIssueCode::invalid_value, "choices must not be empty"), false;
  std::vector<std::string_view> picked;
  for (const auto& item : *items) {
    const std::string_view text = *item.if_string();
    if (std::find(spec.choices.begin(), spec.choices.end(), text) == spec.choices.end()) {
      return add(errors, path, ConfigIssueCode::invalid_value, "choices must hold only this option's own choices"), false;
    }
    if (std::find(picked.begin(), picked.end(), text) != picked.end()) {
      return add(errors, path, ConfigIssueCode::invalid_value, "choices must not repeat a choice"), false;
    }
    picked.push_back(text);
  }
  choices.clear();
  for (const auto choice : spec.choices) {
    if (std::find(picked.begin(), picked.end(), choice) != picked.end()) choices.emplace_back(choice);
  }
  return true;
}

std::optional<ModelOptionLimits> card_option(const ModelOptionSpec& spec, const JsonValue& json, const std::string& path,
                                             ConfigErrors& errors) {
  const auto* members = json.if_object();
  if (members == nullptr) return add(errors, path, ConfigIssueCode::wrong_type, "must be an object"), std::nullopt;
  ModelOptionLimits limit;
  limit.spec = &spec;
  limit.minimum = spec.minimum;
  limit.maximum = spec.maximum;
  limit.minimum_decimal = spec.minimum_decimal;
  limit.maximum_decimal = spec.maximum_decimal;
  for (const auto choice : spec.choices) limit.choices.emplace_back(choice);

  const std::size_t issues_before = errors.size();
  const JsonValue* default_json = nullptr;
  const JsonValue* min_json = nullptr;
  const JsonValue* max_json = nullptr;
  const JsonValue* choices_json = nullptr;
  for (const auto& member : *members) {
    if (member.key == "default") default_json = &member.value;
    else if (bounded_kind(spec.kind) && member.key == "min") min_json = &member.value;
    else if (bounded_kind(spec.kind) && member.key == "max") max_json = &member.value;
    else if (spec.kind == Kind::choice && member.key == "choices") choices_json = &member.value;
    else {
      add(errors, path + "." + member.key, ConfigIssueCode::unknown_key,
          is_option_member(member.key) ? "does not apply to this option" : "is not a member of an option");
    }
  }
  bool bounds_ok = true;
  if (min_json != nullptr) bounds_ok = card_bound(spec, *min_json, "min", path, errors, limit.minimum, limit.minimum_decimal) && bounds_ok;
  if (max_json != nullptr) bounds_ok = card_bound(spec, *max_json, "max", path, errors, limit.maximum, limit.maximum_decimal) && bounds_ok;
  if (bounds_ok && (min_json != nullptr || max_json != nullptr)) {
    const bool ordered = spec.kind == Kind::decimal ? limit.minimum_decimal <= limit.maximum_decimal : limit.minimum <= limit.maximum;
    if (!ordered) {
      add(errors, path, ConfigIssueCode::invalid_value, "min must not exceed max");
      bounds_ok = false;
    }
  }
  if (choices_json != nullptr) bounds_ok = card_choices(spec, *choices_json, path, errors, limit.choices) && bounds_ok;
  if (default_json == nullptr) {
    add(errors, path, ConfigIssueCode::missing_required, "default is required");
    return std::nullopt;
  }
  if (!bounds_ok) return std::nullopt;  // the default cannot be checked against broken bounds
  auto value = typed(spec.kind, *default_json);
  if (!value.has_value()) {
    add(errors, path, ConfigIssueCode::wrong_type, "default " + std::string{type_reason(spec.kind)});
    return std::nullopt;
  }
  if (!within(limit, *value)) {
    add(errors, path, ConfigIssueCode::out_of_range, "default " + std::string{bounds_reason(spec.kind)});
    return std::nullopt;
  }
  if (!offered(limit, *value)) {
    add(errors, path, ConfigIssueCode::invalid_value, "default must be one of the option's offered values");
    return std::nullopt;
  }
  if (errors.size() != issues_before) return std::nullopt;
  limit.default_value = std::move(*value);
  return limit;
}

// --- Cross-option rules -----------------------------------------------------------------------

// kv_resident is 0 (off) or at least the engine's floor, and at most max_context when the card
// lists max_context (`context` set).
std::optional<ConfigIssueCode> kv_resident_fault(const std::int64_t cells, const std::optional<std::int64_t> context) {
  if (cells != 0 && cells < kKvResidentMinimum) return ConfigIssueCode::invalid_value;
  if (context.has_value() && cells > *context) return ConfigIssueCode::conflicting_values;
  return std::nullopt;
}

std::string_view kv_resident_reason(const ConfigIssueCode code) {
  return code == ConfigIssueCode::invalid_value ? "must be 0 (off) or at least 20,480" : "must not exceed max_context";
}

// --- Writing into a draft ---------------------------------------------------------------------

void set_entry(FrontendConfigDraft& draft, const std::string_view key, ConfigDraftValue value) {
  draft.entries.insert_or_assign(std::string{key}, ConfigDraftEntry{std::move(value), ConfigSource::file});
}

// The card's `flag` and its value (the next argument, unless that is another flag) taken out of the draft's engine.args.
void drop_card_flag(FrontendConfigDraft& draft, const std::string_view flag) {
  const auto args = draft.entries.find("engine.args");
  auto* list = args == draft.entries.end() ? nullptr : std::get_if<std::vector<std::string>>(&args->second.value);
  if (list == nullptr) return;
  std::vector<std::string> kept;
  for (std::size_t i = 0; i < list->size(); ++i) {
    if ((*list)[i] != flag) {
      kept.push_back((*list)[i]);
      continue;
    }
    if (i + 1U < list->size() && (*list)[i + 1U].rfind("--", 0) != 0) ++i;   // its value goes too
  }
  *list = std::move(kept);
}

// An engine plugin's declared option (nullptr: the server's, or not one).
const PluginEngineOption* plugin_option(const PluginEngine& engine, const std::string_view name) {
  const auto found = std::find_if(engine.options.begin(), engine.options.end(),
                                  [name](const PluginEngineOption& option) { return option.name == name; });
  return found == engine.options.end() ? nullptr : &*found;
}

// A plugin's option writes its flag as its manifest's argument takes it: a boolean on a flag without a value is there or
// not, on on_off "on" or "off"; a number as it is (a fraction at the three places the arguments' check takes), integer
// kinds' auto no flag (the engine decides); a choice its text. Its GPUs write engine.gpu, as every engine's do.
void write_plugin_option(const PluginEngine& engine, const PluginEngineOption& option, const ModelOptionValue& value,
                         std::vector<std::string>& flags, FrontendConfigDraft& draft) {
  const auto* text = std::get_if<std::string>(&value);
  if (option.kind == Kind::devices) {
    if (text != nullptr && *text != "auto") set_entry(draft, "engine.gpu", ConfigDraftValue{std::in_place_type<std::string>, *text});
    return;
  }
  const auto argument = std::find_if(engine.arguments.begin(), engine.arguments.end(),
                                     [&option](const PluginArgument& each) { return each.flag == option.flag; });
  if (argument == engine.arguments.end()) return;  // (the manifest's reader refuses such an option)
  if (const auto* on = std::get_if<bool>(&value)) {
    if (argument->value == PluginArgumentValue::on_off) {
      flags.push_back(option.flag);
      flags.emplace_back(*on ? "on" : "off");
    } else if (*on) {
      flags.push_back(option.flag);
    }
  } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
    flags.push_back(option.flag);
    flags.push_back(std::to_string(*integer));
  } else if (const auto* decimal = std::get_if<double>(&value)) {
    flags.push_back(option.flag);
    flags.push_back(decimal_text(std::round(*decimal * 1000.0) / 1000.0));
  } else if (text != nullptr && !(option.kind == Kind::integer_or_auto && *text == "auto")) {
    flags.push_back(option.flag);
    flags.push_back(*text);
  }
}

void write_option(const EngineProfile profile, const std::string_view name, const ModelOptionValue& value, const bool draft_layer_on,
                  std::vector<std::string>& flags, FrontendConfigDraft& draft) {
  const auto& family = family_descriptor(profile);
  if (family.plugin != nullptr) {
    if (const auto* declared = plugin_option(*family.plugin, name)) {
      write_plugin_option(*family.plugin, *declared, value, flags, draft);
      return;
    }
  }
  const auto* integer = std::get_if<std::int64_t>(&value);
  const auto* decimal = std::get_if<double>(&value);
  const auto* text = std::get_if<std::string>(&value);
  const auto* on = std::get_if<bool>(&value);
  const auto flag_with = [&flags](const std::string_view flag, std::string argument) {
    flags.emplace_back(flag);
    flags.push_back(std::move(argument));
  };
  const auto integer_or_auto = [integer, text]() { return integer != nullptr ? std::to_string(*integer) : *text; };
  if (name == "answer_minutes") {
    // every engine: the card's generation timeout (chat_execution's deadline), never an engine flag
    if (integer != nullptr) {
      set_entry(draft, "limits.engine.generation_timeout_s", ConfigDraftValue{std::in_place_type<std::int64_t>, *integer * 60});
    }
    return;
  }
  if (family.glm_pack_rows && (name == "max_context" || name == "kv" || name == "ram_headroom")) {
    // the GLM engine's launch (glm53, raon1) reads its typed rows (launch_composition: Glm53LaunchArgs; the headroom its
    // environment's STRATA_GLM_RAM_HEADROOM_GB, glm_environment_knobs), never engine.args
    if (name == "max_context" && integer != nullptr) set_entry(draft, "glm.max_context", ConfigDraftValue{std::in_place_type<std::int64_t>, *integer});
    if (name == "kv" && text != nullptr) set_entry(draft, "glm.kv", ConfigDraftValue{std::in_place_type<std::string>, *text});
    if (name == "ram_headroom" && integer != nullptr) {
      set_entry(draft, "glm.ram_headroom_gb", ConfigDraftValue{std::in_place_type<std::int64_t>, *integer});
    }
    return;
  }
  if (name == "max_context" && integer != nullptr) {
    flag_with("--max-context", std::to_string(*integer));
    if (family.yarn_trained_context && *integer > *family.yarn_trained_context) {
      flag_with("--rope-scaling", "yarn");
      flag_with("--rope-scale", decimal_text(static_cast<double>(*integer) / static_cast<double>(*family.yarn_trained_context)));
    }
  }
  else if (name == "kv" && text != nullptr) flag_with(family.kv_flag, *text);
  else if (name == "kv_resident" && integer != nullptr) {
    if (*integer > 0) flag_with("--kv-resident", std::to_string(*integer));
  } else if (name == "kv_grow" && on != nullptr) {
    if (*on) flags.emplace_back("--kv-grow");
  } else if (name == "mtp" && on != nullptr && family.mtp_off == MtpOff::drop_card_flag) {
    // Gemma 4's MTP draft file: off takes the card's --mtp <file> out of this start's arguments (the Gemma engine has no
    // engine.mtp row: the config refuses it on gemma4 by name); on writes nothing.
    if (!*on) drop_card_flag(draft, "--mtp");
  } else if (name == "mtp" && on != nullptr) {
    // Off: engine.mtp false, so the launch composition strips the card's --mtp <dir> (and any
    // other draft-layer flag) at every start, draft_layer.py's rule; on writes nothing.
    if (!*on) set_entry(draft, "engine.mtp", ConfigDraftValue{std::in_place_type<bool>, false});
  } else if (name == "spec" && integer != nullptr) {
    // Kept without the draft layer: upstream strata_control/draft_layer.py:16-17 — the engine
    // still drafts by prompt lookup with --spec, and a native (IQ) pack needs --spec T >= 2 to
    // start at all (generate.cpp:3706-3709).
    flag_with("--spec", std::to_string(*integer));
  } else if (name == "spec_min_p" && decimal != nullptr) {
    // (the Gemma engine's allowlist takes at most three places: a page's 0.12345 is its 0.123)
    double floor_p = *decimal;
    if (family.spec_min_p_places) {
      double scale = 1.0;
      for (int place = 0; place < *family.spec_min_p_places; ++place) scale *= 10.0;
      floor_p = std::round(*decimal * scale) / scale;
    }
    if (draft_layer_on) flag_with("--spec-min-p", decimal_text(floor_p));
  } else if ((name == "expert_cache" || name == "prefill") && (integer != nullptr || text != nullptr)) {
    flag_with(name == "expert_cache" ? "--expert-cache" : "--prefill", integer_or_auto());
  } else if (name == "low_ram" && text != nullptr) {
    // upstream's low-RAM mode: the variant a start resolved (auto and on are resolved by the launcher, low_ram.hpp)
    if (*text == "mmap") flags.emplace_back("--mmap-experts");
    if (*text == "resident") flags.emplace_back("--resident-experts");
  } else if (name == "chunk" && integer != nullptr) flag_with("--chunk", std::to_string(*integer));
  else if (name == "expert_vram" && integer != nullptr) flag_with("--expert-vram", std::to_string(*integer));  // auto: no flag
  else if (name == "thinking" && text != nullptr) {
    if (family.thinking_write == ThinkingWrite::verbatim_auto_unset && *text == "auto") return;  // MiniMax's adaptive default
    if (family.thinking_write == ThinkingWrite::on_off) {
      if (*text == "on" || *text == "off") {
        set_entry(draft, "reasoning.effort", ConfigDraftValue{std::in_place_type<std::string>, *text == "on" ? "high" : "none"});
      }
    } else {
      set_entry(draft, "reasoning.effort", ConfigDraftValue{std::in_place_type<std::string>, *text});
    }
  } else if ((name == "temperature" || name == "top_p" || name == "min_p") && decimal != nullptr) {
    set_entry(draft, "defaults.sampling." + std::string{name}, ConfigDraftValue{std::in_place_type<double>, *decimal});
  } else if (name == "top_k" && integer != nullptr) {
    set_entry(draft, "defaults.sampling.top_k", ConfigDraftValue{std::in_place_type<std::int64_t>, *integer});
  } else if (name == "gpus" && text != nullptr) {
    // the GPUs it runs on (auto: the card's own rows stand): the GLM engine (glm53, raon1) reads glm.gpu, every other
    // launch engine.gpu (multi-GPU like upstream, 2026-10-28)
    if (*text != "auto") {
      set_entry(draft, family.glm_pack_rows ? "glm.gpu" : "engine.gpu", ConfigDraftValue{std::in_place_type<std::string>, *text});
    }
  } else if (name == "vision" && text != nullptr) {  // the card's picture encoder (its vision.mmproj): where it runs, or off
    set_entry(draft, "vision.enabled", ConfigDraftValue{std::in_place_type<bool>, *text != "off"});
    set_entry(draft, "vision.gpu", ConfigDraftValue{std::in_place_type<bool>, *text == "gpu"});
  }
}

// --- JSON text --------------------------------------------------------------------------------

void append_value(std::string& out, const ModelOptionValue& value, const bool keep_decimal_marker) {
  if (const auto* on = std::get_if<bool>(&value)) {
    out += *on ? "true" : "false";
  } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
    out += std::to_string(*integer);
  } else if (const auto* decimal = std::get_if<double>(&value)) {
    std::string text = decimal_text(*decimal);
    // The remembered file reads a whole number back as an integer; ".0" keeps it a decimal.
    if (keep_decimal_marker && text.find_first_of(".eE") == std::string::npos) text += ".0";
    out += text;
  } else {
    append_json_string(&out, std::get<std::string>(value));
  }
}

// A value the strict writer can put in the remembered file.
bool writable(const ModelOptionValue& value) {
  if (const auto* decimal = std::get_if<double>(&value)) return std::isfinite(*decimal);
  if (const auto* text = std::get_if<std::string>(&value)) return is_valid_utf8(*text);
  return true;
}

// The remembered file is small (a few cards, a few options each): 1 MiB is far past any real one.
constexpr JsonLimits kRememberedLimits{1U << 20U, 8U, 4096U, 4096U, true};

std::optional<ModelOptionValue> remembered_value(const JsonValue& json) {
  if (const auto* on = json.if_boolean()) return ModelOptionValue{std::in_place_type<bool>, *on};
  if (const auto* integer = json.if_integer()) return ModelOptionValue{std::in_place_type<std::int64_t>, *integer};
  if (const auto* decimal = json.if_decimal()) return ModelOptionValue{std::in_place_type<double>, *decimal};
  if (const auto* text = json.if_string()) return ModelOptionValue{std::in_place_type<std::string>, *text};
  return std::nullopt;
}

}  // namespace

std::string option_value_label(const ModelOptionSpec& spec, const std::int64_t value) {
  if (value == 0 && !spec.zero_label.empty()) return std::string{spec.zero_label};
  if (value > 0 && value % 1048576 == 0) return std::to_string(value / 1048576) + "M";
  if (value > 0 && value % 1024 == 0) return std::to_string(value / 1024) + "K";
  return std::to_string(value);
}

std::vector<std::string_view> option_flags(const EngineProfile profile, const std::string_view name) {
  const auto& family = family_descriptor(profile);
  // the glm_pack engine's (glm53, raon1): typed rows only
  if (find_model_option(profile, name) == nullptr || family.glm_pack_rows) return {};
  if (family.plugin != nullptr) {
    // an engine plugin's declared option: its flag (its GPUs and the server's options: none)
    const auto* declared = plugin_option(*family.plugin, name);
    if (declared == nullptr || declared->flag.empty()) return {};
    return {declared->flag};
  }
  if (name == "max_context") {
    if (family.yarn_trained_context) return {"--max-context", "--rope-scaling", "--rope-scale"};
    return {"--max-context"};
  }
  if (name == "kv") return {family.kv_flag};  // (maya-mm3's own --kv-cache)
  if (name == "low_ram") return {"--mmap-experts", "--resident-experts"};
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 8> kFlags{{
      {"kv_resident", "--kv-resident"}, {"kv_grow", "--kv-grow"}, {"spec", "--spec"},
      {"spec_min_p", "--spec-min-p"}, {"expert_cache", "--expert-cache"}, {"prefill", "--prefill"}, {"chunk", "--chunk"},
      {"expert_vram", "--expert-vram"}}};
  for (const auto& [option, flag] : kFlags) {
    if (option == name) return {flag};
  }
  return {};
}

std::span<const ModelOptionSpec> model_options_for(const EngineProfile profile) noexcept { return family_descriptor(profile).options; }

ModelOptionValues recommended_sampling(const EngineProfile profile) {
  const auto& sampling = family_descriptor(profile).sampling;
  ModelOptionValues out{{"temperature", sampling.temperature}, {"top_p", sampling.top_p}};
  if (sampling.top_k) out.emplace("top_k", *sampling.top_k);
  if (sampling.min_p) out.emplace("min_p", *sampling.min_p);
  return out;
}

const ModelOptionSpec* find_model_option(const EngineProfile profile, const std::string_view name) noexcept {
  const auto options = model_options_for(profile);
  const auto it = std::find_if(options.begin(), options.end(), [name](const ModelOptionSpec& spec) { return spec.name == name; });
  return it == options.end() ? nullptr : &*it;
}

bool is_model_option_name(const std::string_view name) noexcept {
  // every family's: an option may be one engine's alone (the GLM engine's RAM headroom, an engine plugin's own)
  const auto families = all_families();
  return std::any_of(families.begin(), families.end(),
                     [name](const FamilyDescriptor* family) { return find_model_option(family->profile, name) != nullptr; });
}

std::variant<std::vector<ModelOptionLimits>, ConfigErrors> parse_card_options(const EngineProfile profile,
                                                                             const JsonValue& options) {
  ConfigErrors errors;
  const auto* members = options.if_object();
  if (members == nullptr) {
    add(errors, "options", ConfigIssueCode::wrong_type, "must be an object");
    return errors;
  }
  std::vector<ModelOptionLimits> limits;
  for (const auto& member : *members) {
    const std::string path = option_path(member.key);
    const auto* spec = find_model_option(profile, member.key);
    if (spec == nullptr) {
      add(errors, path, ConfigIssueCode::unknown_key, "is not an option of this engine");
      continue;
    }
    if (auto limit = card_option(*spec, member.value, path, errors)) limits.push_back(std::move(*limit));
  }
  // The card's defaults must make a valid start on their own.
  if (const auto* resident = limit_named(limits, "kv_resident")) {
    const auto* context = limit_named(limits, "max_context");
    const auto* cells = std::get_if<std::int64_t>(&resident->default_value);
    const auto* tokens = context == nullptr ? nullptr : std::get_if<std::int64_t>(&context->default_value);
    if (cells != nullptr) {
      if (const auto fault = kv_resident_fault(*cells, tokens == nullptr ? std::nullopt : std::optional<std::int64_t>{*tokens})) {
        add(errors, "options.kv_resident", *fault,
            *fault == ConfigIssueCode::invalid_value ? "default must be 0 (off) or at least 20,480"
                                                     : "default must not exceed the max_context default");
      }
    }
  }
  if (!errors.empty()) return errors;
  const auto table = model_options_for(profile);
  std::sort(limits.begin(), limits.end(), [&table](const ModelOptionLimits& a, const ModelOptionLimits& b) {
    return a.spec - table.data() < b.spec - table.data();
  });
  return limits;
}

std::variant<ModelOptionValues, ConfigErrors> resolve_option_values(const std::vector<ModelOptionLimits>& limits,
                                                                    const JsonValue* requested,
                                                                    const ModelOptionValues& remembered) {
  ConfigErrors errors;
  const std::vector<JsonMember>* members = nullptr;
  if (requested != nullptr) {
    members = requested->if_object();
    if (members == nullptr) {
      add(errors, "options", ConfigIssueCode::wrong_type, "must be an object");
      return errors;
    }
    for (const auto& member : *members) {
      if (limit_named(limits, member.key) != nullptr) continue;
      // A name from the request is echoed only when it is one of the fixed option names.
      if (is_model_option_name(member.key)) {
        add(errors, option_path(member.key), ConfigIssueCode::unknown_key, "is not an option of this card");
      } else {
        add(errors, "options", ConfigIssueCode::unknown_key, "names an option this card does not have");
      }
    }
  }
  ModelOptionValues values;
  bool resident_requested = false;
  for (const auto& limit : limits) {
    if (limit.spec == nullptr) continue;
    const std::string_view name = limit.spec->name;
    const JsonValue* asked = members == nullptr ? nullptr : member_named(*members, name);
    if (asked != nullptr) {
      auto value = typed(limit.spec->kind, *asked);
      if (!value.has_value()) {
        add(errors, option_path(name), ConfigIssueCode::wrong_type, std::string{type_reason(limit.spec->kind)});
        continue;
      }
      if (!within(limit, *value)) {
        add(errors, option_path(name), ConfigIssueCode::out_of_range, std::string{bounds_reason(limit.spec->kind)});
        continue;
      }
      if (!offered(limit, *value)) {
        add(errors, option_path(name), ConfigIssueCode::invalid_value, "must be one of the option's offered values");
        continue;
      }
      if (name == "kv_resident") resident_requested = true;
      values.insert_or_assign(std::string{name}, std::move(*value));
      continue;
    }
    values.insert_or_assign(std::string{name}, usable(limit, remembered).value_or(limit.default_value));
  }
  // kv_resident against the resolved max_context (when both resolved). A remembered value the
  // rule no longer allows gives way to the default, like any remembered value out of bounds.
  const auto resident = values.find("kv_resident");
  const auto* resident_limit = limit_named(limits, "kv_resident");
  const bool context_listed = limit_named(limits, "max_context") != nullptr;
  const auto context = values.find("max_context");
  if (resident != values.end() && resident_limit != nullptr && (!context_listed || context != values.end())) {
    std::optional<std::int64_t> tokens;
    if (context_listed) {
      if (const auto* value = std::get_if<std::int64_t>(&context->second)) tokens = *value;
    }
    const auto* cells = std::get_if<std::int64_t>(&resident->second);
    auto fault = cells == nullptr ? std::nullopt : kv_resident_fault(*cells, tokens);
    if (fault.has_value() && !resident_requested) {
      const auto* fallback = std::get_if<std::int64_t>(&resident_limit->default_value);
      fault = fallback == nullptr ? std::nullopt : kv_resident_fault(*fallback, tokens);
      if (!fault.has_value()) resident->second = resident_limit->default_value;
    }
    if (fault.has_value()) add(errors, "options.kv_resident", *fault, std::string{kv_resident_reason(*fault)});
  }
  if (!errors.empty()) return errors;
  return values;
}

void apply_option_values(const EngineProfile profile, const ModelOptionValues& values, FrontendConfigDraft& draft) {
  const auto mtp = values.find("mtp");
  const bool* mtp_on = mtp == values.end() ? nullptr : std::get_if<bool>(&mtp->second);
  const bool draft_layer_on = mtp_on == nullptr || *mtp_on;
  std::vector<std::string> flags;
  for (const auto& spec : model_options_for(profile)) {
    const auto it = values.find(spec.name);
    if (it != values.end()) write_option(profile, spec.name, it->second, draft_layer_on, flags, draft);
  }
  if (flags.empty()) return;
  const auto args = draft.entries.find("engine.args");
  if (args == draft.entries.end()) {
    set_entry(draft, "engine.args", ConfigDraftValue{std::in_place_type<std::vector<std::string>>, std::move(flags)});
    return;
  }
  // An engine.args entry of another type is left as written: validate() refuses it by name.
  if (auto* list = std::get_if<std::vector<std::string>>(&args->second.value)) {
    list->insert(list->end(), std::make_move_iterator(flags.begin()), std::make_move_iterator(flags.end()));
    args->second.source = ConfigSource::file;
  }
}

std::span<const std::string_view> option_controlled_flags(const EngineProfile profile) noexcept {
  return family_descriptor(profile).option_controlled_flags;
}

std::string describe_options_json(const std::vector<ModelOptionLimits>& limits, const ModelOptionValues& values) {
  std::string out = "[";
  for (const auto& limit : limits) {
    if (limit.spec == nullptr) continue;
    const ModelOptionSpec& spec = *limit.spec;
    if (out.size() > 1U) out += ',';
    out += "{\"name\":";
    append_json_string(&out, spec.name);
    out += ",\"kind\":";
    append_json_string(&out, kind_name(spec.kind));
    out += ",\"label\":";
    append_json_string(&out, spec.label);
    out += ",\"help\":";
    append_json_string(&out, spec.help);
    out += ",\"default\":";
    append_value(out, limit.default_value, false);
    switch (spec.kind) {
      case Kind::integer:
      case Kind::integer_or_auto:
        out += ",\"min\":" + std::to_string(limit.minimum) + ",\"max\":" + std::to_string(limit.maximum);
        if (!spec.values.empty()) {
          // The page's dropdown: the offered values inside the card's bounds, with their labels.
          std::vector<std::int64_t> shown;
          for (const auto value : spec.values) {
            if (value >= limit.minimum && value <= limit.maximum) shown.push_back(value);
          }
          out += ",\"choices\":[";
          for (std::size_t i = 0; i < shown.size(); ++i) out += (i != 0U ? "," : "") + std::to_string(shown[i]);
          out += "],\"choice_labels\":[";
          for (std::size_t i = 0; i < shown.size(); ++i) {
            if (i != 0U) out += ',';
            append_json_string(&out, option_value_label(spec, shown[i]));
          }
          out += ']';
          if (!spec.at_most.empty() && limit_named(limits, spec.at_most) != nullptr) {
            out += ",\"at_most\":";
            append_json_string(&out, spec.at_most);
          }
        }
        break;
      case Kind::decimal:
        out += ",\"min\":" + decimal_text(limit.minimum_decimal) + ",\"max\":" + decimal_text(limit.maximum_decimal);
        break;
      case Kind::choice:
        out += ",\"choices\":[";
        for (std::size_t i = 0; i < limit.choices.size(); ++i) {
          if (i != 0U) out += ',';
          append_json_string(&out, limit.choices[i]);
        }
        out += ']';
        break;
      case Kind::devices:
        out += ",\"max\":" + std::to_string(limit.maximum);   // the most GPUs the engine runs on
        break;
      case Kind::boolean: break;
    }
    out += ",\"value\":";
    append_value(out, usable(limit, values).value_or(limit.default_value), false);
    out += '}';
  }
  out += ']';
  return out;
}

std::string render_remembered_options(const RememberedOptions& remembered) {
  std::string out;
  for (const auto& [card, values] : remembered) {
    if (!is_valid_utf8(card)) continue;
    out += out.empty() ? "{\n  " : ",\n  ";
    append_json_string(&out, card);
    out += ": {";
    bool first = true;
    for (const auto& [name, value] : values) {
      if (!is_model_option_name(name) || !writable(value)) continue;
      if (!first) out += ", ";
      first = false;
      append_json_string(&out, name);
      out += ": ";
      append_value(out, value, true);
    }
    out += '}';
  }
  out += out.empty() ? "{}\n" : "\n}\n";
  return out;
}

RememberedOptions parse_remembered_options(const std::string_view text) {
  try {
    const auto parsed = parse_json(text, kRememberedLimits);
    const auto* document = std::get_if<JsonValue>(&parsed);
    const auto* cards = document == nullptr ? nullptr : document->if_object();
    if (cards == nullptr) return {};
    RememberedOptions remembered;
    for (const auto& card : *cards) {
      const auto* options = card.value.if_object();
      if (options == nullptr) continue;
      ModelOptionValues values;
      for (const auto& option : *options) {
        if (!is_model_option_name(option.key)) continue;
        if (auto value = remembered_value(option.value)) values.insert_or_assign(option.key, std::move(*value));
      }
      remembered.insert_or_assign(card.key, std::move(values));
    }
    return remembered;
  } catch (...) {
    return {};  // lenient by design (D7): the card defaults apply
  }
}

}  // namespace secure_frontend
