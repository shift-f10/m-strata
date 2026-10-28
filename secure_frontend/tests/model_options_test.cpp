// Model launcher plan Task 2 (spec docs/superpowers/specs/2026-10-23-model-launcher-design.md §4): the
// options a model card lets the page set, per engine profile — the tables, the card's "options"
// object, the page's values against the card's bounds, their flags and config keys, the GET
// /v1/strata/models description and the remembered-options file (D7).
#include "secure_frontend/engine_arguments.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/model_options.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using secure_frontend::ConfigErrors;
using secure_frontend::ConfigIssueCode;
using secure_frontend::EngineProfile;
using secure_frontend::JsonValue;
using secure_frontend::ModelOptionKind;
using secure_frontend::ModelOptionLimits;
using secure_frontend::ModelOptionValues;

int failures = 0;
void expect(const bool value, const std::string_view message) {
  if (!value) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

using Strings = std::vector<std::string>;

JsonValue json(const std::string_view text) {
  const auto parsed = secure_frontend::parse_json(text, secure_frontend::JsonLimits{1U << 20U, 16U, 4096U, 4096U, true});
  if (const auto* value = std::get_if<JsonValue>(&parsed)) return *value;
  ++failures;
  std::cerr << "FAILED: test JSON does not parse: " << text << '\n';
  return {};
}

Strings names_of(const EngineProfile profile) {
  Strings names;
  for (const auto& spec : secure_frontend::model_options_for(profile)) names.emplace_back(spec.name);
  return names;
}

std::vector<ModelOptionLimits> limits_of(const EngineProfile profile, const std::string_view options) {
  const auto parsed = secure_frontend::parse_card_options(profile, json(options));
  if (const auto* limits = std::get_if<std::vector<ModelOptionLimits>>(&parsed)) return *limits;
  ++failures;
  std::cerr << "FAILED: the card options are refused: " << options << '\n';
  for (const auto& issue : std::get<ConfigErrors>(parsed)) std::cerr << "  " << secure_frontend::format_issue(issue) << '\n';
  return {};
}

const ModelOptionLimits* limit_named(const std::vector<ModelOptionLimits>& limits, const std::string_view name) {
  for (const auto& limit : limits) {
    if (limit.spec != nullptr && limit.spec->name == name) return &limit;
  }
  return nullptr;
}

ConfigErrors card_issues(const EngineProfile profile, const JsonValue& options) {
  const auto parsed = secure_frontend::parse_card_options(profile, options);
  if (const auto* errors = std::get_if<ConfigErrors>(&parsed)) return *errors;
  return {};
}

bool has_issue(const ConfigErrors& errors, const std::string_view key_path, const ConfigIssueCode code) {
  return std::any_of(errors.begin(), errors.end(), [&](const secure_frontend::ConfigIssue& issue) {
    return issue.key_path == key_path && issue.code == code && !issue.reason.empty();
  });
}

bool any_reason_contains(const ConfigErrors& errors, const std::string_view text) {
  return std::any_of(errors.begin(), errors.end(), [&](const secure_frontend::ConfigIssue& issue) {
    return issue.reason.find(text) != std::string::npos;
  });
}

std::string listed(const ConfigErrors& errors) {
  std::string text;
  for (const auto& issue : errors) text += " [" + secure_frontend::format_issue(issue) + "]";
  return text.empty() ? std::string{" (no issue)"} : text;
}

std::optional<ModelOptionValues> resolved(const std::vector<ModelOptionLimits>& limits, const JsonValue* requested,
                                          const ModelOptionValues& remembered = {}) {
  auto result = secure_frontend::resolve_option_values(limits, requested, remembered);
  if (auto* values = std::get_if<ModelOptionValues>(&result)) return std::move(*values);
  return std::nullopt;
}

ConfigErrors resolve_issues(const std::vector<ModelOptionLimits>& limits, const JsonValue& requested,
                            const ModelOptionValues& remembered = {}) {
  const auto result = secure_frontend::resolve_option_values(limits, &requested, remembered);
  if (const auto* errors = std::get_if<ConfigErrors>(&result)) return *errors;
  return {};
}

template <typename T>
bool value_is(const ModelOptionValues& values, const std::string_view name, const T& expected) {
  const auto it = values.find(name);
  if (it == values.end()) return false;
  const auto* value = std::get_if<T>(&it->second);
  return value != nullptr && *value == expected;
}

const secure_frontend::ConfigDraftEntry* entry_of(const secure_frontend::FrontendConfigDraft& draft, const std::string_view key) {
  const auto it = draft.entries.find(key);
  return it == draft.entries.end() ? nullptr : &it->second;
}

Strings args_of(const secure_frontend::FrontendConfigDraft& draft) {
  const auto* entry = entry_of(draft, "engine.args");
  if (entry == nullptr) return {};
  const auto* args = std::get_if<std::vector<std::string>>(&entry->value);
  return args == nullptr ? Strings{} : *args;
}

std::string joined(const Strings& items) {
  std::string text;
  for (const auto& item : items) text += (text.empty() ? "" : " ") + item;
  return "[" + text + "]";
}

template <typename T>
bool draft_is(const secure_frontend::FrontendConfigDraft& draft, const std::string_view key, const T& expected) {
  const auto* entry = entry_of(draft, key);
  if (entry == nullptr || entry->source != secure_frontend::ConfigSource::file) return false;
  const auto* value = std::get_if<T>(&entry->value);
  return value != nullptr && *value == expected;
}

const JsonValue* member(const JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& entry : *members) {
    if (entry.key == key) return &entry.value;
  }
  return nullptr;
}

Strings member_keys(const JsonValue& object) {
  Strings keys;
  if (const auto* members = object.if_object()) {
    for (const auto& entry : *members) keys.push_back(entry.key);
  }
  return keys;
}

bool string_member(const JsonValue& object, const std::string_view key, const std::string_view expected) {
  const auto* value = member(object, key);
  return value != nullptr && value->if_string() != nullptr && *value->if_string() == expected;
}

bool integer_member(const JsonValue& object, const std::string_view key, const std::int64_t expected) {
  const auto* value = member(object, key);
  return value != nullptr && value->if_integer() != nullptr && *value->if_integer() == expected;
}

// A decimal written in shortest form may read back as an integer literal ("1").
bool number_member(const JsonValue& object, const std::string_view key, const double expected) {
  const auto* value = member(object, key);
  if (value == nullptr) return false;
  if (const auto* d = value->if_decimal()) return *d == expected;
  if (const auto* i = value->if_integer()) return static_cast<double>(*i) == expected;
  return false;
}

// The engine's own argument rules accept `value` for `flag` (the second validation, spec §7).
bool engine_accepts(const secure_frontend::EngineArgumentsProfile profile, const std::string_view flag, const std::int64_t value) {
  return !secure_frontend::validate_engine_user_arguments(profile, {std::string{flag}, std::to_string(value)}).has_value();
}

void option_tables_per_profile() {
  expect(names_of(EngineProfile::qwen) == Strings{"max_context", "kv", "kv_resident", "kv_grow", "mtp", "spec", "spec_min_p",
                                                  "expert_cache", "prefill", "thinking", "temperature", "top_p", "top_k", "min_p",
                                                  "web_search"},
         "the qwen options in display order: " + joined(names_of(EngineProfile::qwen)));
  expect(names_of(EngineProfile::gemma4) ==
             Strings{"max_context", "chunk", "expert_vram", "thinking", "temperature", "top_p", "top_k", "min_p"},
         "the gemma4 options in display order: " + joined(names_of(EngineProfile::gemma4)));
  // D10 met (owner 2026-10-27): Maya's own GLM choices - its context menu, the engine's --kv, the sampling defaults
  expect(names_of(EngineProfile::glm53) == Strings{"max_context", "kv", "temperature", "top_p", "top_k", "min_p", "web_search"},
         "the glm53 options in display order: " + joined(names_of(EngineProfile::glm53)));

  for (const auto profile : {EngineProfile::qwen, EngineProfile::gemma4}) {
    for (const auto& spec : secure_frontend::model_options_for(profile)) {
      const bool name_ok = !spec.name.empty() && std::all_of(spec.name.begin(), spec.name.end(),
                                                             [](const char c) { return (c >= 'a' && c <= 'z') || c == '_'; });
      expect(name_ok && !spec.label.empty() && !spec.help.empty(), "every option has a [a-z_]+ name, a label and a help line");
      expect(secure_frontend::find_model_option(profile, spec.name) == &spec, "find_model_option finds the table row");
      expect(secure_frontend::is_model_option_name(spec.name), "every option name is a fixed option name");
      if (spec.kind == ModelOptionKind::choice) expect(!spec.choices.empty(), "a choice option lists its choices");
      if (spec.kind == ModelOptionKind::integer || spec.kind == ModelOptionKind::integer_or_auto) {
        expect(spec.minimum <= spec.maximum, "integer bounds are ordered");
      }
      if (spec.kind == ModelOptionKind::decimal) expect(spec.minimum_decimal <= spec.maximum_decimal, "decimal bounds are ordered");
    }
  }

  const auto* const kv = secure_frontend::find_model_option(EngineProfile::qwen, "kv");
  expect(kv != nullptr && kv->kind == ModelOptionKind::choice && kv->choices.size() == 4U && kv->choices[0] == "fp16" &&
             kv->choices[1] == "int8" && kv->choices[2] == "q4_0" && kv->choices[3] == "k8v4",
         "qwen kv: fp16, int8, q4_0, k8v4 (upstream generate.cpp --kv)");
  const auto integer_bounds = [](const EngineProfile profile, const std::string_view name, const ModelOptionKind kind,
                                 const std::int64_t minimum, const std::int64_t maximum) {
    const auto* spec = secure_frontend::find_model_option(profile, name);
    expect(spec != nullptr && spec->kind == kind && spec->minimum == minimum && spec->maximum == maximum,
           std::string{name} + ": kind and hard bounds " + std::to_string(minimum) + "-" + std::to_string(maximum));
  };
  integer_bounds(EngineProfile::qwen, "max_context", ModelOptionKind::integer, 1, 1048576);
  integer_bounds(EngineProfile::qwen, "kv_resident", ModelOptionKind::integer, 0, 1048576);
  integer_bounds(EngineProfile::qwen, "spec", ModelOptionKind::integer, 2, 8);
  integer_bounds(EngineProfile::qwen, "expert_cache", ModelOptionKind::integer_or_auto, 0, 24576);
  integer_bounds(EngineProfile::qwen, "prefill", ModelOptionKind::integer_or_auto, 1, 32768);
  integer_bounds(EngineProfile::gemma4, "max_context", ModelOptionKind::integer, 1, 262144);
  integer_bounds(EngineProfile::gemma4, "chunk", ModelOptionKind::integer, 1, 8192);
  integer_bounds(EngineProfile::gemma4, "expert_vram", ModelOptionKind::integer_or_auto, 0, 1024);
  for (const auto* name : {"kv_grow", "mtp"}) {
    const auto* spec = secure_frontend::find_model_option(EngineProfile::qwen, name);
    expect(spec != nullptr && spec->kind == ModelOptionKind::boolean, std::string{name} + " is a boolean");
  }
  const auto* spec_min_p = secure_frontend::find_model_option(EngineProfile::qwen, "spec_min_p");
  expect(spec_min_p != nullptr && spec_min_p->kind == ModelOptionKind::decimal && spec_min_p->minimum_decimal == 0.0 &&
             spec_min_p->maximum_decimal == 1.0,
         "spec_min_p is a decimal 0-1");
  const auto* qwen_thinking = secure_frontend::find_model_option(EngineProfile::qwen, "thinking");
  expect(qwen_thinking != nullptr && qwen_thinking->kind == ModelOptionKind::choice && qwen_thinking->choices.size() == 4U &&
             qwen_thinking->choices[0] == "none" && qwen_thinking->choices[1] == "low" && qwen_thinking->choices[2] == "medium" &&
             qwen_thinking->choices[3] == "high",
         "qwen thinking: none, low, medium, high (reasoning.effort's levels)");
  const auto* gemma_thinking = secure_frontend::find_model_option(EngineProfile::gemma4, "thinking");
  expect(gemma_thinking != nullptr && gemma_thinking->kind == ModelOptionKind::choice && gemma_thinking->choices.size() == 2U &&
             gemma_thinking->choices[0] == "off" && gemma_thinking->choices[1] == "on",
         "gemma4 thinking: off, on");

  // The sampling options carry the defaults.sampling.* rows' own bounds; an exclusive row
  // minimum becomes the next double above it (so "> 0" stays exactly "> 0").
  for (const auto profile : {EngineProfile::qwen, EngineProfile::gemma4}) {
    for (const auto* name : {"temperature", "top_p", "min_p"}) {
      const auto* spec = secure_frontend::find_model_option(profile, name);
      const auto* key = secure_frontend::find_config_key(std::string{"defaults.sampling."} + name);
      const double minimum = key == nullptr ? 0.0
                             : key->minimum_exclusive ? std::nextafter(key->minimum_decimal, std::numeric_limits<double>::infinity())
                                                      : key->minimum_decimal;
      expect(spec != nullptr && key != nullptr && spec->kind == ModelOptionKind::decimal && spec->minimum_decimal == minimum &&
                 spec->maximum_decimal == key->maximum_decimal,
             std::string{name} + " takes the defaults.sampling row's bounds");
    }
    const auto* top_k = secure_frontend::find_model_option(profile, "top_k");
    const auto* key = secure_frontend::find_config_key("defaults.sampling.top_k");
    expect(top_k != nullptr && key != nullptr && top_k->kind == ModelOptionKind::integer && top_k->minimum == key->minimum &&
               top_k->maximum == key->maximum,
           "top_k takes the defaults.sampling.top_k row's bounds (1-64)");
  }
  // The Gemma bounds are the Gemma engine's own argument rules (kGemma4Flags).
  using secure_frontend::EngineArgumentsProfile;
  for (const auto& [name, flag] : {std::pair{"max_context", "--max-context"}, std::pair{"chunk", "--chunk"},
                                   std::pair{"expert_vram", "--expert-vram"}}) {
    const auto* spec = secure_frontend::find_model_option(EngineProfile::gemma4, name);
    expect(spec != nullptr && engine_accepts(EngineArgumentsProfile::strata_gemma4, flag, spec->minimum) &&
               engine_accepts(EngineArgumentsProfile::strata_gemma4, flag, spec->maximum) &&
               !engine_accepts(EngineArgumentsProfile::strata_gemma4, flag, spec->maximum + 1) &&
               (spec->minimum == 0 || !engine_accepts(EngineArgumentsProfile::strata_gemma4, flag, spec->minimum - 1)),
           std::string{name} + ": the bounds are the Gemma engine's for " + flag);
  }

  expect(secure_frontend::find_model_option(EngineProfile::gemma4, "kv") == nullptr &&
             secure_frontend::find_model_option(EngineProfile::qwen, "chunk") == nullptr &&
             secure_frontend::find_model_option(EngineProfile::glm53, "thinking") == nullptr &&
             secure_frontend::find_model_option(EngineProfile::glm53, "kv_resident") == nullptr,
         "an option of another profile is not found (GLM: no thinking switch, no KV-streaming size)");
  expect(secure_frontend::is_model_option_name("chunk") && secure_frontend::is_model_option_name("kv_resident") &&
             !secure_frontend::is_model_option_name("bogus") && !secure_frontend::is_model_option_name("") &&
             !secure_frontend::is_model_option_name("KV"),
         "is_model_option_name is the union of the tables' names, nothing else");
}

void card_options_parse_and_bound() {
  const auto qwen = limits_of(EngineProfile::qwen, R"({
      "top_p": {"default": 0.95, "min": 0.5},
      "max_context": {"default": 131072, "min": 4096, "max": 131072},
      "kv": {"default": "int8", "choices": ["k8v4", "int8", "fp16"]},
      "kv_resident": {"default": 32768, "min": 0, "max": 131072},
      "mtp": {"default": true},
      "spec": {"default": 4},
      "expert_cache": {"default": "auto", "max": 4096},
      "prefill": {"default": 4096},
      "temperature": {"default": 1}})");
  Strings order;
  for (const auto& limit : qwen) order.emplace_back(limit.spec == nullptr ? std::string_view{"?"} : limit.spec->name);
  expect(order == Strings{"max_context", "kv", "kv_resident", "mtp", "spec", "expert_cache", "prefill", "temperature", "top_p"},
         "the limits keep the table's display order, only the listed options: " + joined(order));
  const auto* max_context = limit_named(qwen, "max_context");
  expect(max_context != nullptr && max_context->minimum == 4096 && max_context->maximum == 131072 &&
             std::get_if<std::int64_t>(&max_context->default_value) != nullptr &&
             std::get<std::int64_t>(max_context->default_value) == 131072,
         "max_context: the card's min, max and default");
  const auto* kv = limit_named(qwen, "kv");
  expect(kv != nullptr && kv->choices == Strings{"fp16", "int8", "k8v4"} &&
             std::get_if<std::string>(&kv->default_value) != nullptr && std::get<std::string>(kv->default_value) == "int8",
         "kv: the card's subset of choices in the table's order, its default");
  const auto* mtp = limit_named(qwen, "mtp");
  expect(mtp != nullptr && std::get_if<bool>(&mtp->default_value) != nullptr && std::get<bool>(mtp->default_value),
         "mtp: a boolean default");
  const auto* spec = limit_named(qwen, "spec");
  expect(spec != nullptr && spec->minimum == 2 && spec->maximum == 8, "spec: the engine's bounds when the card names none");
  const auto* expert_cache = limit_named(qwen, "expert_cache");
  expect(expert_cache != nullptr && expert_cache->minimum == 0 && expert_cache->maximum == 4096 &&
             std::get_if<std::string>(&expert_cache->default_value) != nullptr &&
             std::get<std::string>(expert_cache->default_value) == "auto",
         "expert_cache: \"auto\" as the default, the card's max");
  const auto* prefill = limit_named(qwen, "prefill");
  expect(prefill != nullptr && std::get_if<std::int64_t>(&prefill->default_value) != nullptr &&
             std::get<std::int64_t>(prefill->default_value) == 4096,
         "prefill: an integer default for integer_or_auto");
  const auto* temperature = limit_named(qwen, "temperature");
  expect(temperature != nullptr && std::get_if<double>(&temperature->default_value) != nullptr &&
             std::get<double>(temperature->default_value) == 1.0 && temperature->minimum_decimal == 0.0 &&
             temperature->maximum_decimal == 2.0,
         "temperature: an integer literal is a decimal default; the engine's bounds");
  const auto* top_p = limit_named(qwen, "top_p");
  expect(top_p != nullptr && top_p->minimum_decimal == 0.5 && top_p->maximum_decimal == 1.0 &&
             std::get_if<double>(&top_p->default_value) != nullptr && std::get<double>(top_p->default_value) == 0.95,
         "top_p: the card's min inside the engine's (0, 1]");

  const auto gemma = limits_of(EngineProfile::gemma4, R"({"expert_vram": {"default": "auto", "max": 64},
      "thinking": {"default": "on"}, "chunk": {"default": 2048, "min": 256, "max": 4096}})");
  const auto* vram = limit_named(gemma, "expert_vram");
  expect(gemma.size() == 3U && vram != nullptr && vram->minimum == 0 && vram->maximum == 64 &&
             std::get_if<std::string>(&vram->default_value) != nullptr,
         "gemma4 expert_vram: auto default, the card's max");
  const auto* thinking = limit_named(gemma, "thinking");
  expect(thinking != nullptr && thinking->choices == Strings{"off", "on"}, "gemma4 thinking: every choice when the card names none");
  const auto* chunk = limit_named(gemma, "chunk");
  expect(chunk != nullptr && chunk->minimum == 256 && chunk->maximum == 4096, "gemma4 chunk: the card's bounds");

  expect(limits_of(EngineProfile::qwen, "{}").empty() && limits_of(EngineProfile::glm53, "{}").empty(),
         "an empty options object lists nothing");
}

void card_option_refusals_name_the_option() {
  struct Refusal final {
    EngineProfile profile;
    std::string_view options;
    std::string_view key_path;
    ConfigIssueCode code;
    std::string_view secret;  // a value from the card that no reason may carry
  };
  const std::vector<Refusal> refusals{
      {EngineProfile::qwen, R"({"chunk": {"default": 1}})", "options.chunk", ConfigIssueCode::unknown_key, ""},
      {EngineProfile::gemma4, R"({"kv": {"default": "fp16"}})", "options.kv", ConfigIssueCode::unknown_key, "fp16"},
      {EngineProfile::glm53, R"({"thinking": {"default": "high"}})", "options.thinking", ConfigIssueCode::unknown_key, ""},
      {EngineProfile::qwen, R"({"max_context": {}})", "options.max_context", ConfigIssueCode::missing_required, ""},
      {EngineProfile::qwen, R"({"max_context": {"default": 2000000}})", "options.max_context", ConfigIssueCode::out_of_range,
       "2000000"},
      {EngineProfile::qwen, R"({"max_context": {"default": 4096, "max": 2000000}})", "options.max_context",
       ConfigIssueCode::out_of_range, "2000000"},
      {EngineProfile::qwen, R"({"max_context": {"default": 0}})", "options.max_context", ConfigIssueCode::out_of_range, ""},
      {EngineProfile::qwen, R"({"max_context": {"default": 8192, "min": 16384, "max": 8192}})", "options.max_context",
       ConfigIssueCode::invalid_value, "16384"},
      {EngineProfile::qwen, R"({"max_context": {"default": 4096, "max": 8192.5}})", "options.max_context",
       ConfigIssueCode::wrong_type, "8192.5"},
      {EngineProfile::qwen, R"({"max_context": {"default": 9000, "max": 8192}})", "options.max_context",
       ConfigIssueCode::out_of_range, "9000"},
      {EngineProfile::qwen, R"({"kv": {"default": "int8", "choices": []}})", "options.kv", ConfigIssueCode::invalid_value, ""},
      {EngineProfile::qwen, R"({"kv": {"default": "int8", "choices": ["int8", "int8"]}})", "options.kv",
       ConfigIssueCode::invalid_value, ""},
      {EngineProfile::qwen, R"({"kv": {"default": "int8", "choices": ["int8", "fp32"]}})", "options.kv",
       ConfigIssueCode::invalid_value, "fp32"},
      {EngineProfile::qwen, R"({"kv": {"default": "int8", "choices": "int8"}})", "options.kv", ConfigIssueCode::wrong_type, ""},
      {EngineProfile::qwen, R"({"kv": {"default": "q4_0", "choices": ["int8"]}})", "options.kv", ConfigIssueCode::out_of_range,
       "q4_0"},
      {EngineProfile::qwen, R"({"kv": {"default": 8}})", "options.kv", ConfigIssueCode::wrong_type, ""},
      {EngineProfile::qwen, R"({"mtp": {"default": "yes"}})", "options.mtp", ConfigIssueCode::wrong_type, "yes"},
      {EngineProfile::qwen, R"({"mtp": true})", "options.mtp", ConfigIssueCode::wrong_type, ""},
      {EngineProfile::qwen, R"({"mtp": {"default": true, "min": 0}})", "options.mtp.min", ConfigIssueCode::unknown_key, ""},
      {EngineProfile::qwen, R"({"spec": {"default": 4, "choices": ["4"]}})", "options.spec.choices", ConfigIssueCode::unknown_key,
       ""},
      {EngineProfile::qwen, R"({"kv": {"default": "int8", "colour": 1}})", "options.kv.colour", ConfigIssueCode::unknown_key, ""},
      {EngineProfile::qwen, R"({"spec": {"default": 4.5}})", "options.spec", ConfigIssueCode::wrong_type, "4.5"},
      {EngineProfile::qwen, R"({"spec": {"default": 9}})", "options.spec", ConfigIssueCode::out_of_range, ""},
      {EngineProfile::qwen, R"({"expert_cache": {"default": "manual"}})", "options.expert_cache", ConfigIssueCode::wrong_type,
       "manual"},
      {EngineProfile::qwen, R"({"expert_cache": {"default": -1}})", "options.expert_cache", ConfigIssueCode::out_of_range, ""},
      {EngineProfile::qwen, R"({"top_p": {"default": 0}})", "options.top_p", ConfigIssueCode::out_of_range, ""},
      {EngineProfile::qwen, R"({"top_p": {"default": 0.9, "min": 0}})", "options.top_p", ConfigIssueCode::out_of_range, ""},
      {EngineProfile::qwen, R"({"temperature": {"default": 0.7, "max": 2.5}})", "options.temperature",
       ConfigIssueCode::out_of_range, "2.5"},
      {EngineProfile::qwen, R"({"temperature": {"default": "warm"}})", "options.temperature", ConfigIssueCode::wrong_type, "warm"},
      {EngineProfile::qwen, R"({"kv_resident": {"default": 1000}})", "options.kv_resident", ConfigIssueCode::invalid_value, ""},
      {EngineProfile::qwen, R"({"max_context": {"default": 16384}, "kv_resident": {"default": 32768}})", "options.kv_resident",
       ConfigIssueCode::conflicting_values, ""},
      {EngineProfile::gemma4, R"({"expert_vram": {"default": 2048}})", "options.expert_vram", ConfigIssueCode::out_of_range, ""},
      {EngineProfile::gemma4, R"({"thinking": {"default": "high"}})", "options.thinking", ConfigIssueCode::out_of_range, ""},
  };
  for (const auto& refusal : refusals) {
    const auto errors = card_issues(refusal.profile, json(refusal.options));
    expect(has_issue(errors, refusal.key_path, refusal.code),
           std::string{refusal.options} + " is refused at " + std::string{refusal.key_path} + ":" + listed(errors));
    if (!refusal.secret.empty()) {
      expect(!any_reason_contains(errors, refusal.secret), std::string{refusal.options} + ": no reason echoes a value");
    }
  }
  const auto not_object = card_issues(EngineProfile::qwen, JsonValue::make_array({}));
  expect(has_issue(not_object, "options", ConfigIssueCode::wrong_type), "options that are not an object:" + listed(not_object));
  const auto two = card_issues(EngineProfile::qwen, json(R"({"kv": {"default": "fp32"}, "spec": {"default": 99}})"));
  expect(has_issue(two, "options.kv", ConfigIssueCode::out_of_range) && has_issue(two, "options.spec", ConfigIssueCode::out_of_range),
         "every bad option is reported, not just the first:" + listed(two));
}

std::vector<ModelOptionLimits> page_card() {
  return limits_of(EngineProfile::qwen, R"({
      "max_context": {"default": 131072, "min": 4096, "max": 131072},
      "kv": {"default": "int8", "choices": ["fp16", "int8", "k8v4"]},
      "mtp": {"default": true},
      "spec": {"default": 4},
      "expert_cache": {"default": "auto", "max": 4096},
      "temperature": {"default": 0.7}})");
}

void page_values_resolve_within_bounds() {
  const auto limits = page_card();
  const auto defaults = resolved(limits, nullptr);
  expect(defaults.has_value() && defaults->size() == 6U && value_is<std::int64_t>(*defaults, "max_context", 131072) &&
             value_is<std::string>(*defaults, "kv", "int8") && value_is<bool>(*defaults, "mtp", true) &&
             value_is<std::int64_t>(*defaults, "spec", 4) && value_is<std::string>(*defaults, "expert_cache", "auto") &&
             value_is<double>(*defaults, "temperature", 0.7),
         "nothing requested, nothing remembered: the card's defaults for every listed option");

  const JsonValue request = json(R"({"max_context": 65536, "temperature": 1, "expert_cache": 2048, "mtp": false})");
  const auto chosen = resolved(limits, &request);
  expect(chosen.has_value() && value_is<std::int64_t>(*chosen, "max_context", 65536) && value_is<double>(*chosen, "temperature", 1.0) &&
             value_is<std::int64_t>(*chosen, "expert_cache", 2048) && value_is<bool>(*chosen, "mtp", false) &&
             value_is<std::string>(*chosen, "kv", "int8"),
         "requested values win (an integer literal for a decimal); the rest are defaults");

  const ModelOptionValues remembered{{"kv", std::string{"fp16"}}, {"spec", std::int64_t{6}}, {"expert_cache", std::string{"auto"}}};
  const auto recalled = resolved(limits, nullptr, remembered);
  expect(recalled.has_value() && value_is<std::string>(*recalled, "kv", "fp16") && value_is<std::int64_t>(*recalled, "spec", 6),
         "remembered values inside the card's bounds are used (D7)");
  const JsonValue override_kv = json(R"({"kv": "k8v4"})");
  const auto overridden = resolved(limits, &override_kv, remembered);
  expect(overridden.has_value() && value_is<std::string>(*overridden, "kv", "k8v4") && value_is<std::int64_t>(*overridden, "spec", 6),
         "a requested value wins over the remembered one");

  const ModelOptionValues stale{{"max_context", std::int64_t{2048}},   // below the card's min
                                {"kv", std::string{"q4_0"}},           // not one of the card's choices
                                {"spec", std::string{"six"}},          // wrong type
                                {"mtp", std::int64_t{1}},              // wrong type
                                {"expert_cache", std::int64_t{8192}},  // above the card's max
                                {"temperature", std::int64_t{1}},      // an integer for a decimal: 1.0
                                {"top_k", std::int64_t{20}}};          // not listed by this card
  const auto replaced = resolved(limits, nullptr, stale);
  expect(replaced.has_value() && replaced->size() == 6U && value_is<std::int64_t>(*replaced, "max_context", 131072) &&
             value_is<std::string>(*replaced, "kv", "int8") && value_is<std::int64_t>(*replaced, "spec", 4) &&
             value_is<bool>(*replaced, "mtp", true) && value_is<std::string>(*replaced, "expert_cache", "auto") &&
             value_is<double>(*replaced, "temperature", 1.0) && replaced->find("top_k") == replaced->end(),
         "remembered values outside the card's bounds are silently replaced by the defaults");

  struct Refusal final {
    std::string_view request;
    std::string_view key_path;
    ConfigIssueCode code;
    std::string_view secret;
  };
  const std::vector<Refusal> refusals{
      {R"({"max_context": 2000000})", "options.max_context", ConfigIssueCode::out_of_range, "2000000"},
      {R"({"max_context": 2048})", "options.max_context", ConfigIssueCode::out_of_range, "2048"},
      {R"({"max_context": 4096.5})", "options.max_context", ConfigIssueCode::wrong_type, "4096.5"},
      {R"({"max_context": "4096"})", "options.max_context", ConfigIssueCode::wrong_type, "4096"},
      {R"({"kv": "q4_0"})", "options.kv", ConfigIssueCode::out_of_range, "q4_0"},
      {R"({"kv": "<b>"})", "options.kv", ConfigIssueCode::out_of_range, "<b>"},
      {R"({"mtp": 1})", "options.mtp", ConfigIssueCode::wrong_type, ""},
      {R"({"mtp": null})", "options.mtp", ConfigIssueCode::wrong_type, ""},
      {R"({"expert_cache": "AUTO"})", "options.expert_cache", ConfigIssueCode::wrong_type, "AUTO"},
      {R"({"expert_cache": 5000})", "options.expert_cache", ConfigIssueCode::out_of_range, "5000"},
      {R"({"temperature": 2.5})", "options.temperature", ConfigIssueCode::out_of_range, "2.5"},
      {R"({"temperature": [1]})", "options.temperature", ConfigIssueCode::wrong_type, ""},
      {R"({"spec": 1})", "options.spec", ConfigIssueCode::out_of_range, ""},
      {R"({"chunk": 8})", "options.chunk", ConfigIssueCode::unknown_key, ""},      // another profile's option
      {R"({"kv_grow": true})", "options.kv_grow", ConfigIssueCode::unknown_key, ""},  // not listed by this card
      {R"({"<script>": 1})", "options", ConfigIssueCode::unknown_key, "<script>"},    // never echoed
  };
  for (const auto& refusal : refusals) {
    const auto errors = resolve_issues(limits, json(refusal.request));
    expect(has_issue(errors, refusal.key_path, refusal.code),
           std::string{refusal.request} + " is refused at " + std::string{refusal.key_path} + ":" + listed(errors));
    if (!refusal.secret.empty()) {
      expect(!any_reason_contains(errors, refusal.secret), std::string{refusal.request} + ": no reason echoes a value");
      expect(std::none_of(errors.begin(), errors.end(),
                          [&](const auto& issue) { return issue.key_path.find(refusal.secret) != std::string::npos; }),
             std::string{refusal.request} + ": no key path echoes a request value");
    }
  }
  const auto not_object = resolve_issues(limits, JsonValue::make_array({}));
  expect(has_issue(not_object, "options", ConfigIssueCode::wrong_type), "a start's options that are not an object:" + listed(not_object));
}

void kv_resident_cross_rules() {
  const auto limits = limits_of(EngineProfile::qwen, R"({"max_context": {"default": 131072, "max": 131072},
      "kv_resident": {"default": 0, "max": 131072}})");
  const auto accepts = [&limits](const std::string_view request, const std::int64_t expected) {
    const JsonValue body = json(request);
    const auto values = resolved(limits, &body);
    expect(values.has_value() && value_is<std::int64_t>(*values, "kv_resident", expected),
           std::string{request} + " is accepted" + (values.has_value() ? "" : listed(resolve_issues(limits, body))));
  };
  accepts(R"({"kv_resident": 0})", 0);
  accepts(R"({"kv_resident": 20480})", 20480);
  accepts(R"({"kv_resident": 32768, "max_context": 32768})", 32768);
  accepts(R"({"kv_resident": 131072})", 131072);
  const auto refused = [&limits](const std::string_view request, const ConfigIssueCode code) {
    const auto errors = resolve_issues(limits, json(request));
    expect(has_issue(errors, "options.kv_resident", code), std::string{request} + " is refused at options.kv_resident:" + listed(errors));
  };
  refused(R"({"kv_resident": 1})", ConfigIssueCode::invalid_value);
  refused(R"({"kv_resident": 20479})", ConfigIssueCode::invalid_value);
  refused(R"({"kv_resident": 65536, "max_context": 32768})", ConfigIssueCode::conflicting_values);

  // A remembered value the new context no longer allows falls back to the default (0 = off).
  const JsonValue smaller = json(R"({"max_context": 32768})");
  const auto fallback = resolved(limits, &smaller, ModelOptionValues{{"kv_resident", std::int64_t{65536}}});
  expect(fallback.has_value() && value_is<std::int64_t>(*fallback, "kv_resident", 0),
         "a remembered kv_resident above the requested context gives way to the default");
  const auto streamed = resolved(limits, nullptr, ModelOptionValues{{"kv_resident", std::int64_t{10000}}});
  expect(streamed.has_value() && value_is<std::int64_t>(*streamed, "kv_resident", 0),
         "a remembered kv_resident below the engine's 20,480 gives way to the default");

  // A default that the requested context does not allow is refused (the page must send kv_resident).
  const auto streaming_card = limits_of(EngineProfile::qwen, R"({"max_context": {"default": 131072, "max": 131072},
      "kv_resident": {"default": 32768, "max": 131072}})");
  const auto errors = resolve_issues(streaming_card, json(R"({"max_context": 16384})"));
  expect(has_issue(errors, "options.kv_resident", ConfigIssueCode::conflicting_values),
         "the default kv_resident above the requested context is refused:" + listed(errors));

  // Without max_context on the card only the engine's minimum applies.
  const auto no_context = limits_of(EngineProfile::qwen, R"({"kv_resident": {"default": 0}})");
  // (262,144: a listed value, since the page's dropdown, owner request 2026-10-23.)
  const JsonValue large = json(R"({"kv_resident": 262144})");
  const auto unbounded = resolved(no_context, &large);
  expect(unbounded.has_value() && value_is<std::int64_t>(*unbounded, "kv_resident", 262144),
         "without max_context on the card, kv_resident is bounded by its own limits only");
  const auto low = resolve_issues(no_context, json(R"({"kv_resident": 4096})"));
  expect(has_issue(low, "options.kv_resident", ConfigIssueCode::invalid_value), "the engine minimum still applies:" + listed(low));
}

secure_frontend::FrontendConfigDraft draft_with_args(const Strings& args) {
  secure_frontend::FrontendConfigDraft draft;
  draft.entries.emplace("engine.args", secure_frontend::ConfigDraftEntry{args, secure_frontend::ConfigSource::file});
  return draft;
}

ModelOptionValues full_qwen_values() {
  return ModelOptionValues{{"max_context", std::int64_t{131072}}, {"kv", std::string{"k8v4"}},
                           {"kv_resident", std::int64_t{32768}},  {"kv_grow", true},
                           {"mtp", true},                         {"spec", std::int64_t{4}},
                           {"spec_min_p", 0.5},                   {"expert_cache", std::string{"auto"}},
                           {"prefill", std::int64_t{4096}},       {"thinking", std::string{"medium"}},
                           {"temperature", 0.7},                  {"top_p", 0.95},
                           {"top_k", std::int64_t{20}},           {"min_p", 0.05}};
}

void options_apply_as_flags_and_keys() {
  auto draft = draft_with_args({"--pack", "C:\\packs\\iq3_s", "--mtp", "C:\\mtp\\rt"});
  secure_frontend::apply_option_values(EngineProfile::qwen, full_qwen_values(), draft);
  const Strings expected{"--pack",        "C:\\packs\\iq3_s", "--mtp",          "C:\\mtp\\rt", "--max-context", "131072",
                         "--kv",          "k8v4",             "--kv-resident",  "32768",       "--kv-grow",     "--spec",
                         "4",             "--spec-min-p",     "0.5",            "--expert-cache", "auto",       "--prefill",
                         "4096"};
  expect(args_of(draft) == expected, "qwen: the flags are appended to the card's args in the table's order: " + joined(args_of(draft)));
  expect(entry_of(draft, "engine.args") != nullptr && entry_of(draft, "engine.args")->source == secure_frontend::ConfigSource::file,
         "engine.args stays a file entry");
  expect(entry_of(draft, "engine.mtp") == nullptr, "mtp on writes nothing (the card's --mtp stays)");
  expect(draft_is<std::string>(draft, "reasoning.effort", "medium"), "thinking sets reasoning.effort");
  expect(draft_is<double>(draft, "defaults.sampling.temperature", 0.7) && draft_is<double>(draft, "defaults.sampling.top_p", 0.95) &&
             draft_is<std::int64_t>(draft, "defaults.sampling.top_k", 20) && draft_is<double>(draft, "defaults.sampling.min_p", 0.05),
         "the sampling options set defaults.sampling.* (decimals as doubles, top_k as an integer)");
  expect(!secure_frontend::validate_engine_user_arguments(secure_frontend::EngineArgumentsProfile::strata_0143, args_of(draft))
              .has_value(),
         "the composed qwen args pass the strata engine's argument rules");

  auto off = draft_with_args({});
  secure_frontend::apply_option_values(
      EngineProfile::qwen,
      ModelOptionValues{{"kv_resident", std::int64_t{0}}, {"kv_grow", false}, {"expert_cache", std::int64_t{2048}},
                        {"prefill", std::string{"auto"}}, {"spec_min_p", 0.1}, {"thinking", std::string{"none"}}},
      off);
  expect(args_of(off) == Strings{"--spec-min-p", "0.1", "--expert-cache", "2048", "--prefill", "auto"},
         "kv_resident 0 and kv_grow false write no flag; numbers and auto as given; decimals shortest: " + joined(args_of(off)));
  expect(draft_is<std::string>(off, "reasoning.effort", "none"), "thinking none is reasoning.effort none");

  secure_frontend::FrontendConfigDraft fresh;
  secure_frontend::apply_option_values(EngineProfile::qwen, ModelOptionValues{{"max_context", std::int64_t{8192}}}, fresh);
  expect(args_of(fresh) == Strings{"--max-context", "8192"} && entry_of(fresh, "engine.args") != nullptr &&
             entry_of(fresh, "engine.args")->source == secure_frontend::ConfigSource::file,
         "engine.args is created (a file entry) when the card has none");
  secure_frontend::FrontendConfigDraft untouched;
  secure_frontend::apply_option_values(EngineProfile::qwen, ModelOptionValues{}, untouched);
  expect(untouched.entries.empty(), "no values write nothing");

  auto gemma = draft_with_args({"--expert-slots", "8"});
  secure_frontend::apply_option_values(
      EngineProfile::gemma4,
      ModelOptionValues{{"max_context", std::int64_t{262144}}, {"chunk", std::int64_t{2048}}, {"expert_vram", std::string{"auto"}},
                        {"thinking", std::string{"on"}}, {"top_k", std::int64_t{64}}, {"temperature", 1.0}},
      gemma);
  expect(args_of(gemma) == Strings{"--expert-slots", "8", "--max-context", "262144", "--chunk", "2048"},
         "gemma4: --max-context and --chunk; expert_vram auto writes no flag: " + joined(args_of(gemma)));
  expect(draft_is<std::string>(gemma, "reasoning.effort", "high"), "gemma4 thinking on is reasoning.effort high");
  expect(draft_is<std::int64_t>(gemma, "defaults.sampling.top_k", 64) && draft_is<double>(gemma, "defaults.sampling.temperature", 1.0),
         "gemma4 sampling keys");
  expect(!secure_frontend::validate_engine_user_arguments(secure_frontend::EngineArgumentsProfile::strata_gemma4, args_of(gemma))
              .has_value(),
         "the composed gemma4 args pass the Gemma engine's allowlist");

  auto gemma_vram = draft_with_args({});
  secure_frontend::apply_option_values(EngineProfile::gemma4,
                                       ModelOptionValues{{"expert_vram", std::int64_t{12}}, {"thinking", std::string{"off"}}}, gemma_vram);
  expect(args_of(gemma_vram) == Strings{"--expert-vram", "12"}, "gemma4 expert_vram N: --expert-vram N GiB");
  expect(draft_is<std::string>(gemma_vram, "reasoning.effort", "none"), "gemma4 thinking off is reasoning.effort none");
  expect(!secure_frontend::validate_engine_user_arguments(secure_frontend::EngineArgumentsProfile::strata_gemma4, args_of(gemma_vram))
              .has_value(),
         "--expert-vram N passes the Gemma engine's allowlist");
}

// flint: the Gemma 4 option set (its engine is built the same way) bounded by multiengine_flint's own argument rules
// (strata_flint); thinking off/on is reasoning.effort none/high, and the composed args pass the flint allowlist.
void flint_options_follow_the_flint_engine() {
  using secure_frontend::EngineArgumentsProfile;
  expect(names_of(EngineProfile::flint) ==
             Strings{"max_context", "chunk", "expert_vram", "thinking", "temperature", "top_p", "top_k", "min_p", "web_search"},
         "the flint options in display order: " + joined(names_of(EngineProfile::flint)));
  for (const auto& [name, flag] : {std::pair{"max_context", "--max-context"}, std::pair{"chunk", "--chunk"},
                                   std::pair{"expert_vram", "--expert-vram"}}) {
    const auto* spec = secure_frontend::find_model_option(EngineProfile::flint, name);
    expect(spec != nullptr && engine_accepts(EngineArgumentsProfile::strata_flint, flag, spec->minimum) &&
               engine_accepts(EngineArgumentsProfile::strata_flint, flag, spec->maximum) &&
               !engine_accepts(EngineArgumentsProfile::strata_flint, flag, spec->maximum + 1) &&
               (spec->minimum == 0 || !engine_accepts(EngineArgumentsProfile::strata_flint, flag, spec->minimum - 1)),
           std::string{name} + ": the bounds are the flint engine's for " + flag);
  }
  const auto* thinking = secure_frontend::find_model_option(EngineProfile::flint, "thinking");
  expect(thinking != nullptr && thinking->choices.size() == 2U && thinking->choices[0] == "off" && thinking->choices[1] == "on",
         "flint thinking: off, on");
  expect(secure_frontend::option_controlled_flags(EngineProfile::flint).size() == 3U, "flint controls --max-context, --chunk, --expert-vram");
  auto draft = draft_with_args({"--cpu-lane", "on"});
  secure_frontend::apply_option_values(
      EngineProfile::flint,
      ModelOptionValues{{"max_context", std::int64_t{65536}}, {"chunk", std::int64_t{1024}}, {"expert_vram", std::int64_t{10}},
                        {"thinking", std::string{"off"}}, {"temperature", 0.6}},
      draft);
  expect(args_of(draft) == Strings{"--cpu-lane", "on", "--max-context", "65536", "--chunk", "1024", "--expert-vram", "10"},
         "flint: --max-context, --chunk, --expert-vram after the card's own: " + joined(args_of(draft)));
  expect(draft_is<std::string>(draft, "reasoning.effort", "none"), "flint thinking off is reasoning.effort none");
  expect(!secure_frontend::validate_engine_user_arguments(EngineArgumentsProfile::strata_flint, args_of(draft)).has_value(),
         "the composed flint args pass the flint engine's allowlist");
  auto on = draft_with_args({});
  secure_frontend::apply_option_values(EngineProfile::flint, ModelOptionValues{{"thinking", std::string{"on"}}}, on);
  expect(draft_is<std::string>(on, "reasoning.effort", "high"), "flint thinking on is reasoning.effort high");
}

// Owner 2026-10-28 ("a toggle in the card that can enable search"): web_search, a checkbox of each profile with the
// server's tool (qwen, glm53, raon1, flint). It writes no flag and no row: the launcher reads it at a start to hand the
// model the tool or not. A card may list it with its own default.
void web_search_is_a_checkbox_of_the_tool_profiles() {
  for (const auto profile : {EngineProfile::qwen, EngineProfile::glm53, EngineProfile::raon1, EngineProfile::flint}) {
    const auto* spec = secure_frontend::find_model_option(profile, "web_search");
    expect(spec != nullptr && spec->kind == ModelOptionKind::boolean && spec->label == "Web search" && !spec->help.empty(),
           "a tool profile has the Web search checkbox");
    expect(secure_frontend::option_flags(profile, "web_search").empty(), "it sets no engine flag");
    secure_frontend::FrontendConfigDraft draft;
    secure_frontend::apply_option_values(profile, ModelOptionValues{{"web_search", false}}, draft);
    secure_frontend::apply_option_values(profile, ModelOptionValues{{"web_search", true}}, draft);
    expect(draft.entries.empty(), "it writes no config row");
  }
  expect(secure_frontend::find_model_option(EngineProfile::gemma4, "web_search") == nullptr &&
             secure_frontend::find_model_option(EngineProfile::deepseek, "web_search") == nullptr,
         "gemma4 and deepseek have no tool to offer");
  expect(secure_frontend::is_model_option_name("web_search"), "a fixed option name (kept in the remembered file)");
  const auto listed = limits_of(EngineProfile::raon1, R"({"web_search": {"default": false}})");
  expect(listed.size() == 1U && std::get_if<bool>(&listed[0].default_value) != nullptr && !std::get<bool>(listed[0].default_value),
         "a card may list it off by default");
}

void mtp_off_drops_the_draft_options() {
  auto draft = draft_with_args({"--pack", "C:\\packs\\iq3_s", "--mtp", "C:\\mtp\\rt"});
  auto values = full_qwen_values();
  values["mtp"] = false;
  secure_frontend::apply_option_values(EngineProfile::qwen, values, draft);
  const auto args = args_of(draft);
  expect(draft_is<bool>(draft, "engine.mtp", false), "mtp off sets engine.mtp false (a file entry)");
  expect(std::find(args.begin(), args.end(), "--spec-min-p") == args.end() && std::find(args.begin(), args.end(), "0.5") == args.end(),
         "mtp off: the draft layer's --spec-min-p is not written: " + joined(args));
  // upstream strata_control/draft_layer.py:16-17 — without the draft layer the engine still
  // drafts by prompt lookup with --spec, and a native (IQ) pack needs --spec T >= 2 to start
  // (generate.cpp:3706-3709), so --spec stays; the composition strips only the card's --mtp.
  const auto spec = std::find(args.begin(), args.end(), "--spec");
  expect(spec != args.end() && spec + 1 != args.end() && *(spec + 1) == "4", "mtp off keeps --spec (prompt lookup): " + joined(args));
  expect(secure_frontend::draft_layer_removed(args) == Strings{"--mtp"},
         "the existing mtp=false composition then strips only the card's --mtp");

  auto on = draft_with_args({});
  secure_frontend::apply_option_values(EngineProfile::qwen,
                                       ModelOptionValues{{"mtp", true}, {"spec", std::int64_t{6}}, {"spec_min_p", 0.25}}, on);
  expect(args_of(on) == Strings{"--spec", "6", "--spec-min-p", "0.25"} && entry_of(on, "engine.mtp") == nullptr,
         "mtp on: --spec and --spec-min-p, no engine.mtp entry: " + joined(args_of(on)));
  auto unlisted = draft_with_args({});
  secure_frontend::apply_option_values(EngineProfile::qwen, ModelOptionValues{{"spec", std::int64_t{3}}, {"spec_min_p", 0.75}},
                                       unlisted);
  expect(args_of(unlisted) == Strings{"--spec", "3", "--spec-min-p", "0.75"} && entry_of(unlisted, "engine.mtp") == nullptr,
         "mtp not listed: both draft options apply: " + joined(args_of(unlisted)));

  // A start that turns MTP off still validates (and remembers) the draft values.
  const auto limits = limits_of(EngineProfile::qwen, R"({"mtp": {"default": true}, "spec": {"default": 4},
      "spec_min_p": {"default": 0.5}})");
  const JsonValue body = json(R"({"mtp": false, "spec_min_p": 0.25})");
  const auto off = resolved(limits, &body);
  expect(off.has_value() && value_is<bool>(*off, "mtp", false) && value_is<double>(*off, "spec_min_p", 0.25) &&
             value_is<std::int64_t>(*off, "spec", 4),
         "mtp off: the draft values still resolve (they are not applied)");
  const auto bad = resolve_issues(limits, json(R"({"mtp": false, "spec_min_p": 2})"));
  expect(has_issue(bad, "options.spec_min_p", ConfigIssueCode::out_of_range), "a bad draft value is refused with mtp off too");
}

void controlled_flags_listed() {
  const auto as_strings = [](const EngineProfile profile) {
    Strings flags;
    for (const auto flag : secure_frontend::option_controlled_flags(profile)) flags.emplace_back(flag);
    return flags;
  };
  const auto qwen = as_strings(EngineProfile::qwen);
  expect(qwen == Strings{"--max-context", "--rope-scaling", "--rope-scale", "--kv", "--kv-resident", "--kv-grow", "--spec",
                         "--spec-min-p", "--expert-cache", "--prefill"},
         "the qwen option-controlled flags: " + joined(qwen));
  expect(std::find(qwen.begin(), qwen.end(), "--mtp") == qwen.end(), "--mtp is the card's, not an option's");
  const auto gemma = as_strings(EngineProfile::gemma4);
  expect(gemma == Strings{"--max-context", "--chunk", "--expert-vram"}, "the gemma4 option-controlled flags: " + joined(gemma));
  expect(secure_frontend::option_controlled_flags(EngineProfile::glm53).empty(), "glm53 has none");

  // Every flag an option writes is in its profile's list.
  const auto covered = [&as_strings](const EngineProfile profile, const ModelOptionValues& values) {
    secure_frontend::FrontendConfigDraft draft;
    secure_frontend::apply_option_values(profile, values, draft);
    const auto flags = as_strings(profile);
    const auto args = args_of(draft);
    expect(!args.empty(), "the options write flags");
    for (const auto& arg : args) {
      if (arg.rfind("--", 0) == 0) expect(std::find(flags.begin(), flags.end(), arg) != flags.end(), arg + " is option-controlled");
    }
  };
  covered(EngineProfile::qwen, full_qwen_values());
  covered(EngineProfile::gemma4, ModelOptionValues{{"max_context", std::int64_t{4096}}, {"chunk", std::int64_t{512}},
                                                   {"expert_vram", std::int64_t{8}}});
}

void description_json_shape() {
  const auto limits = limits_of(EngineProfile::qwen, R"({
      "max_context": {"default": 32768, "min": 4096, "max": 262144},
      "kv": {"default": "int8", "choices": ["fp16", "int8"]},
      "mtp": {"default": true},
      "expert_cache": {"default": "auto", "max": 4096},
      "top_p": {"default": 0.95, "min": 0.5}})");
  const ModelOptionValues values{{"max_context", std::int64_t{65536}}, {"kv", std::string{"q4_0"}}, {"top_p", 0.9},
                                 {"expert_cache", std::int64_t{99999}}};
  const std::string text = secure_frontend::describe_options_json(limits, values);
  const auto parsed = secure_frontend::parse_json(text, secure_frontend::JsonLimits{1U << 20U, 8U, 1024U, 4096U, true});
  const auto* document = std::get_if<JsonValue>(&parsed);
  const auto* items = document == nullptr ? nullptr : document->if_array();
  expect(items != nullptr && items->size() == 5U, "an array with one object per limit: " + text);
  if (items == nullptr || items->size() != 5U) return;

  const auto& context = (*items)[0];
  expect(member_keys(context) == Strings{"name", "kind", "label", "help", "default", "min", "max", "choices", "choice_labels", "value"},
         "integer fields (a listed integer adds choices and their labels): " + joined(member_keys(context)));
  const auto* context_spec = secure_frontend::find_model_option(EngineProfile::qwen, "max_context");
  expect(string_member(context, "name", "max_context") && string_member(context, "kind", "integer") && context_spec != nullptr &&
             string_member(context, "label", context_spec->label) && string_member(context, "help", context_spec->help) &&
             integer_member(context, "default", 32768) && integer_member(context, "min", 4096) &&
             integer_member(context, "max", 262144) && integer_member(context, "value", 65536),
         "max_context: the card's default and bounds, the given value");

  const auto& kv = (*items)[1];
  expect(member_keys(kv) == Strings{"name", "kind", "label", "help", "default", "choices", "value"},
         "choice fields: name kind label help default choices value: " + joined(member_keys(kv)));
  const auto* choices = member(kv, "choices");
  expect(string_member(kv, "kind", "choice") && string_member(kv, "default", "int8") && choices != nullptr &&
             choices->if_array() != nullptr && choices->if_array()->size() == 2U &&
             string_member(kv, "value", "int8") && (*choices->if_array())[0].if_string() != nullptr &&
             *(*choices->if_array())[0].if_string() == "fp16",
         "kv: the card's choices; a value outside them shows the default");

  const auto& mtp = (*items)[2];
  expect(member_keys(mtp) == Strings{"name", "kind", "label", "help", "default", "value"},
         "boolean fields: name kind label help default value: " + joined(member_keys(mtp)));
  const auto* mtp_default = member(mtp, "default");
  const auto* mtp_value = member(mtp, "value");
  expect(string_member(mtp, "kind", "boolean") && mtp_default != nullptr && mtp_default->if_boolean() != nullptr &&
             *mtp_default->if_boolean() && mtp_value != nullptr && mtp_value->if_boolean() != nullptr && *mtp_value->if_boolean(),
         "mtp: booleans; no value given shows the default");

  const auto& cache = (*items)[3];
  expect(member_keys(cache) == Strings{"name", "kind", "label", "help", "default", "min", "max", "value"},
         "integer_or_auto fields: name kind label help default min max value: " + joined(member_keys(cache)));
  expect(string_member(cache, "kind", "integer_or_auto") && string_member(cache, "default", "auto") && integer_member(cache, "min", 0) &&
             integer_member(cache, "max", 4096) && string_member(cache, "value", "auto"),
         "expert_cache: auto default; a value above the card's max shows the default");

  const auto& top_p = (*items)[4];
  expect(member_keys(top_p) == Strings{"name", "kind", "label", "help", "default", "min", "max", "value"},
         "decimal fields: name kind label help default min max value: " + joined(member_keys(top_p)));
  expect(string_member(top_p, "kind", "decimal") && number_member(top_p, "default", 0.95) && number_member(top_p, "min", 0.5) &&
             number_member(top_p, "max", 1.0) && number_member(top_p, "value", 0.9),
         "top_p: decimals in shortest round-trip form");
  expect(text.find("0.95") != std::string::npos && text.find("0.950") == std::string::npos, "decimals are written shortest: " + text);

  // The engine-bound minimum of top_p (just above 0) survives the round trip.
  const auto wide = limits_of(EngineProfile::gemma4, R"({"top_p": {"default": 1}})");
  const auto wide_text = secure_frontend::describe_options_json(wide, {});
  const auto wide_parsed = secure_frontend::parse_json(wide_text, secure_frontend::JsonLimits{1U << 16U, 8U, 64U, 1024U, true});
  const auto* wide_doc = std::get_if<JsonValue>(&wide_parsed);
  const auto* wide_spec = secure_frontend::find_model_option(EngineProfile::gemma4, "top_p");
  expect(wide_doc != nullptr && wide_doc->if_array() != nullptr && wide_doc->if_array()->size() == 1U && wide_spec != nullptr &&
             number_member((*wide_doc->if_array())[0], "min", wide_spec->minimum_decimal) &&
             wide_spec->minimum_decimal > 0.0,
         "top_p's exclusive minimum is written exactly: " + wide_text);

  expect(secure_frontend::describe_options_json({}, {}) == "[]", "no limits: an empty array");
}

void remembered_options_round_trip_and_tolerate_junk() {
  const secure_frontend::RememberedOptions remembered{
      {"Qwen3.8-Flash-Next",
       ModelOptionValues{{"max_context", std::int64_t{131072}}, {"kv", std::string{"k8v4"}}, {"mtp", false},
                         {"temperature", 0.7}, {"top_p", 1.0}, {"expert_cache", std::string{"auto"}}}},
      {"Gemma \"4\" \\ test", ModelOptionValues{{"chunk", std::int64_t{2048}}}},
      {"Empty", ModelOptionValues{}}};
  const std::string text = secure_frontend::render_remembered_options(remembered);
  expect(!text.empty() && text.back() == '\n', "the rendering ends with a newline");
  const auto strict = secure_frontend::parse_json(text, secure_frontend::JsonLimits{1U << 20U, 4U, 1024U, 4096U, true});
  expect(std::holds_alternative<JsonValue>(strict) && std::get<JsonValue>(strict).if_object() != nullptr,
         "the rendering is strict JSON (an object): " + text);
  const auto back = secure_frontend::parse_remembered_options(text);
  expect(back == remembered, "the remembered options round-trip exactly, a whole decimal stays a decimal: " + text);
  expect(secure_frontend::render_remembered_options({}) == "{}\n", "nothing remembered is {}");

  const auto parse = [](const std::string_view junk) {
    try {
      return secure_frontend::parse_remembered_options(junk);
    } catch (...) {
      ++failures;
      std::cerr << "FAILED: parse_remembered_options threw\n";
      return secure_frontend::RememberedOptions{};
    }
  };
  for (const std::string_view junk : {std::string_view{""}, std::string_view{"not json"}, std::string_view{"[1, 2]"},
                                      std::string_view{"\xEF\xBB\xBF{}"}, std::string_view{"{\"A\": {\"kv\": \"int8\""},
                                      std::string_view{"null"}, std::string_view{"{\"A\": 1, \"A\": 2}"}}) {
    expect(parse(junk).empty(), "a file that is not the shape gives nothing: " + std::string{junk});
  }
  const auto partial = parse(R"({"A": {"kv": "int8", "bogus": 1, "spec": [1], "mtp": null, "max_context": 4096, "top_p": 0.5,
      "kv_grow": true, "nested": {"x": 1}}, "B": 5, "C": [], "D": {"chunk": 2048}})");
  const auto a = partial.find("A");
  expect(partial.size() == 2U && a != partial.end() && a->second.size() == 4U && value_is<std::string>(a->second, "kv", "int8") &&
             value_is<std::int64_t>(a->second, "max_context", 4096) && value_is<double>(a->second, "top_p", 0.5) &&
             value_is<bool>(a->second, "kv_grow", true) && partial.find("D") != partial.end(),
         "members that are not the shape are dropped, the rest kept");
  std::string deep(200, '[');
  expect(parse(deep).empty(), "a deeply nested file gives nothing");
  std::string huge = "{\"A\": {\"kv\": \"";
  huge.append(8U << 20U, 'x');
  huge += "\"}}";
  expect(parse(huge).empty(), "an oversized file gives nothing");
}

}  // namespace

// Owner request 2026-10-23: "make the context length a drop down menu that tops out to 1M, and make
// the kv streaming into a dropdown selection also". Both options offer fixed values (the card's
// min/max trim the list); a value off the list is refused; the description carries the list with
// labels, and KV streaming says it may not exceed the context.
void context_and_kv_streaming_offer_fixed_values() {
  const auto values_of = [](const EngineProfile profile, const std::string_view name) {
    std::vector<std::int64_t> out;
    if (const auto* spec = secure_frontend::find_model_option(profile, name)) out.assign(spec->values.begin(), spec->values.end());
    return out;
  };
  using Ints = std::vector<std::int64_t>;
  expect(values_of(EngineProfile::qwen, "max_context") ==
             Ints{4096, 8192, 16384, 32768, 65536, 131072, 262144, 393216, 524288, 786432, 1048576},
         "Qwen's contexts: 4K to 1M");
  expect(values_of(EngineProfile::gemma4, "max_context") == Ints{4096, 8192, 16384, 32768, 65536, 131072, 262144},
         "Gemma's contexts stop at its trained 256K");
  expect(values_of(EngineProfile::qwen, "kv_resident") == Ints{0, 20480, 32768, 65536, 131072, 262144, 524288},
         "KV streaming: off, then the resident cells from the engine's 20,480 floor");
  const auto* resident = secure_frontend::find_model_option(EngineProfile::qwen, "kv_resident");
  expect(resident != nullptr && resident->at_most == "max_context" && resident->zero_label == "Off",
         "KV streaming names its ceiling and its off label");
  const auto* context = secure_frontend::find_model_option(EngineProfile::qwen, "max_context");
  if (context == nullptr || resident == nullptr) return;
  for (const auto& [value, label] : std::vector<std::pair<std::int64_t, std::string>>{
           {4096, "4K"}, {131072, "128K"}, {262144, "256K"}, {393216, "384K"}, {786432, "768K"}, {1048576, "1M"}}) {
    expect(secure_frontend::option_value_label(*context, value) == label, "label of " + std::to_string(value) + ": " + label);
  }
  expect(secure_frontend::option_value_label(*resident, 0) == "Off" && secure_frontend::option_value_label(*resident, 20480) == "20K",
         "KV streaming's labels");

  // A card's default and a page value must be on the list (inside the card's bounds).
  ConfigErrors off_list = card_issues(EngineProfile::qwen, json(R"({"max_context": {"default": 100000}})"));
  expect(has_issue(off_list, "options.max_context", ConfigIssueCode::invalid_value), "a default off the list: " + listed(off_list));
  const auto limits = limits_of(EngineProfile::qwen, R"({"max_context": {"default": 131072, "max": 1048576},
      "kv_resident": {"default": 32768, "max": 262144}})");
  const JsonValue big = json(R"({"max_context": 1048576, "kv_resident": 262144})");
  const auto accepted = resolved(limits, &big);
  expect(accepted.has_value() && value_is<std::int64_t>(*accepted, "max_context", 1048576) &&
             value_is<std::int64_t>(*accepted, "kv_resident", 262144),
         "1M with 256K resident is accepted");
  expect(has_issue(resolve_issues(limits, json(R"({"max_context": 100000})")), "options.max_context", ConfigIssueCode::invalid_value),
         "a context off the list is refused");
  expect(has_issue(resolve_issues(limits, json(R"({"kv_resident": 40000})")), "options.kv_resident", ConfigIssueCode::invalid_value),
         "a resident size off the list is refused");
  const auto stale = resolved(limits, nullptr, ModelOptionValues{{"max_context", std::int64_t{100000}}});
  expect(stale.has_value() && value_is<std::int64_t>(*stale, "max_context", 131072), "a remembered value off the list gives way to the default");

  // The description: the list inside the card's bounds, its labels, and KV streaming's ceiling.
  const std::string text = secure_frontend::describe_options_json(limits_of(EngineProfile::qwen, R"({
      "max_context": {"default": 131072, "min": 8192, "max": 524288},
      "kv_resident": {"default": 32768, "max": 131072}})"), {});
  expect(text.find(R"("choices":[8192,16384,32768,65536,131072,262144,393216,524288],"choice_labels":["8K","16K","32K","64K","128K","256K","384K","512K"])") !=
             std::string::npos,
         "the context list within the card's bounds, with labels: " + text);
  expect(text.find(R"("choices":[0,20480,32768,65536,131072],"choice_labels":["Off","20K","32K","64K","128K"],"at_most":"max_context")") !=
             std::string::npos,
         "KV streaming's list, labels and ceiling: " + text);
  const std::string alone = secure_frontend::describe_options_json(
      limits_of(EngineProfile::qwen, R"({"kv_resident": {"default": 0, "max": 65536}})"), {});
  expect(alone.find("at_most") == std::string::npos, "no ceiling named when the card does not offer the context");
}

// Past Qwen's trained 262,144 positions the start adds YaRN with factor context / 262,144, upstream's
// own setup rule (docs/INSTALL.md "How much context?", DETAILS.md "Context extension past 262K").
// Inside it, and for Gemma (whose engine stops at its trained length), nothing is added.
// A GLM card's options write the typed glm rows the GLM launch is composed from (glm.max_context, glm.kv), never
// engine.args (the glm53 profile refuses every engine argument); Maya's context menu (maya.py CONTEXTS: 8K, 32K, 64K,
// 128K) is the dropdown, the engine's --kv vocabulary the choices.
void glm_options_write_the_glm_rows() {
  const auto* context = secure_frontend::find_model_option(EngineProfile::glm53, "max_context");
  expect(context != nullptr && context->kind == ModelOptionKind::integer && context->minimum == 1 && context->maximum == 1048576 &&
             std::vector<std::int64_t>(context->values.begin(), context->values.end()) == std::vector<std::int64_t>{8192, 32768, 65536, 131072},
         "glm max_context: glm.max_context's bounds, Maya's menu");
  const auto* kv = secure_frontend::find_model_option(EngineProfile::glm53, "kv");
  expect(kv != nullptr && Strings(kv->choices.begin(), kv->choices.end()) == Strings{"fp16", "int8", "q8_0", "q4_0"},
         "glm kv: the GLM engine's --kv vocabulary");
  secure_frontend::FrontendConfigDraft draft;
  secure_frontend::apply_option_values(EngineProfile::glm53,
                                       ModelOptionValues{{"max_context", std::int64_t{65536}}, {"kv", std::string{"int8"}},
                                                         {"temperature", 1.0}, {"top_p", 0.95}},
                                       draft);
  const auto row = [&draft](const std::string& key) { return draft.entries.find(key); };
  expect(row("glm.max_context") != draft.entries.end() && std::get<std::int64_t>(row("glm.max_context")->second.value) == 65536,
         "max_context -> glm.max_context");
  expect(row("glm.kv") != draft.entries.end() && std::get<std::string>(row("glm.kv")->second.value) == "int8", "kv -> glm.kv");
  expect(row("defaults.sampling.temperature") != draft.entries.end() && row("defaults.sampling.top_p") != draft.entries.end(),
         "sampling -> defaults.sampling.*");
  expect(row("engine.args") == draft.entries.end(), "no engine.args for the GLM engine");
  expect(secure_frontend::option_flags(EngineProfile::glm53, "max_context").empty() &&
             secure_frontend::option_controlled_flags(EngineProfile::glm53).empty(),
         "no engine flag is a GLM option's");
}

// DeepSeek-V4.1 (2026-10-27, spec 2026-10-27-strata-prepare-design §7.2): Maya's own DeepSeek choices - its context menu
// (maya.py v1.1.30 DS4_CONTEXTS: 32K to 1M), the thinking level (DS4_EFFORT; DeepSeek's effort line: low 50, medium and
// high 75, none chat mode), the sampling rows. The context is maya-ds4's own --max-context in engine.args (the maya_ds4
// allowlist), the thinking level reasoning.effort.
void deepseek_options_are_mayas() {
  expect(names_of(EngineProfile::deepseek) == Strings{"max_context", "thinking", "temperature", "top_p", "top_k", "min_p"},
         "the deepseek options in display order: " + joined(names_of(EngineProfile::deepseek)));
  const auto* context = secure_frontend::find_model_option(EngineProfile::deepseek, "max_context");
  expect(context != nullptr && context->kind == ModelOptionKind::integer && context->minimum == 1 && context->maximum == 1048576 &&
             std::vector<std::int64_t>(context->values.begin(), context->values.end()) ==
                 std::vector<std::int64_t>{32768, 65536, 131072, 262144, 524288, 1048576},
         "deepseek max_context: Maya's DS4_CONTEXTS");
  const auto* thinking = secure_frontend::find_model_option(EngineProfile::deepseek, "thinking");
  expect(thinking != nullptr && Strings(thinking->choices.begin(), thinking->choices.end()) == Strings{"none", "low", "medium", "high"},
         "deepseek thinking: none, low, medium, high");
  secure_frontend::FrontendConfigDraft draft;
  secure_frontend::apply_option_values(EngineProfile::deepseek,
                                       ModelOptionValues{{"max_context", std::int64_t{65536}}, {"thinking", std::string{"low"}},
                                                         {"temperature", 1.0}, {"top_p", 1.0}},
                                       draft);
  expect(args_of(draft) == Strings{"--max-context", "65536"}, "max_context -> --max-context: " + joined(args_of(draft)));
  const auto effort = draft.entries.find("reasoning.effort");
  expect(effort != draft.entries.end() && std::get<std::string>(effort->second.value) == "low", "thinking -> reasoning.effort");
  expect(draft.entries.find("defaults.sampling.temperature") != draft.entries.end() &&
             draft.entries.find("defaults.sampling.top_p") != draft.entries.end(),
         "sampling -> defaults.sampling.*");
  expect(secure_frontend::option_flags(EngineProfile::deepseek, "max_context") == std::vector<std::string_view>{"--max-context"},
         "--max-context is the context option's flag");
}

// MiniMax-M3.1-Flash (owner 2026-10-28): Maya's own MiniMax choices - its context menu (maya.py v1.1.30 MM3_CONTEXTS,
// 32K to 1M, MM3_DEFAULT_CONTEXT 64K), the KV cache formats it sizes (MM3_KV_BYTES: bf16, int8, fp8, fp4), the thinking
// level (MM3_EFFORT "auto": MiniMax's adaptive thinking with no effort named; none, low, medium, high) and MiniMax's
// sampling (temperature, top_p; it names no top_k or min_p). The context and the cache are maya-mm3's --max-context and
// --kv-cache in engine.args; thinking auto writes no row, any other level reasoning.effort.
void minimax_options_are_mayas() {
  expect(names_of(EngineProfile::minimax) == Strings{"max_context", "kv", "thinking", "temperature", "top_p"},
         "the minimax options in display order: " + joined(names_of(EngineProfile::minimax)));
  const auto* context = secure_frontend::find_model_option(EngineProfile::minimax, "max_context");
  expect(context != nullptr && context->kind == ModelOptionKind::integer && context->minimum == 1 && context->maximum == 1048576 &&
             std::vector<std::int64_t>(context->values.begin(), context->values.end()) ==
                 std::vector<std::int64_t>{32768, 65536, 131072, 262144, 524288, 1048576},
         "minimax max_context: Maya's MM3_CONTEXTS");
  const auto* kv = secure_frontend::find_model_option(EngineProfile::minimax, "kv");
  expect(kv != nullptr && Strings(kv->choices.begin(), kv->choices.end()) == Strings{"bf16", "int8", "fp8", "fp4"},
         "minimax kv: Maya's four cache formats");
  const auto* thinking = secure_frontend::find_model_option(EngineProfile::minimax, "thinking");
  expect(thinking != nullptr && Strings(thinking->choices.begin(), thinking->choices.end()) == Strings{"auto", "none", "low", "medium", "high"},
         "minimax thinking: auto (MiniMax's adaptive default), none, low, medium, high");
  secure_frontend::FrontendConfigDraft draft;
  secure_frontend::apply_option_values(EngineProfile::minimax,
                                       ModelOptionValues{{"max_context", std::int64_t{65536}}, {"kv", std::string{"int8"}},
                                                         {"thinking", std::string{"auto"}}, {"temperature", 1.0}, {"top_p", 0.95}},
                                       draft);
  expect(args_of(draft) == Strings{"--max-context", "65536", "--kv-cache", "int8"}, "the context and the cache -> maya-mm3's flags: " +
                                                                                       joined(args_of(draft)));
  expect(draft.entries.find("reasoning.effort") == draft.entries.end(), "thinking auto writes no row (no effort named)");
  secure_frontend::FrontendConfigDraft low;
  secure_frontend::apply_option_values(EngineProfile::minimax, ModelOptionValues{{"thinking", std::string{"low"}}}, low);
  expect(draft_is<std::string>(low, "reasoning.effort", "low"), "a level -> reasoning.effort");
  expect(secure_frontend::option_flags(EngineProfile::minimax, "kv") == std::vector<std::string_view>{"--kv-cache"} &&
             secure_frontend::option_flags(EngineProfile::minimax, "max_context") == std::vector<std::string_view>{"--max-context"},
         "--kv-cache and --max-context are the options' flags");
  expect(secure_frontend::find_model_option(EngineProfile::minimax, "web_search") == nullptr,
         "no Web search checkbox: MiniMax has no tool form wired");
}

void past_trained_context_adds_yarn() {
  const auto args_for = [](const EngineProfile profile, const std::int64_t context) {
    secure_frontend::FrontendConfigDraft draft;
    secure_frontend::apply_option_values(profile, ModelOptionValues{{"max_context", context}}, draft);
    return args_of(draft);
  };
  expect(args_for(EngineProfile::qwen, 1048576) == Strings{"--max-context", "1048576", "--rope-scaling", "yarn", "--rope-scale", "4"},
         "1M: yarn, factor 4: " + joined(args_for(EngineProfile::qwen, 1048576)));
  expect(args_for(EngineProfile::qwen, 393216) == Strings{"--max-context", "393216", "--rope-scaling", "yarn", "--rope-scale", "1.5"},
         "384K: yarn, factor 1.5: " + joined(args_for(EngineProfile::qwen, 393216)));
  expect(args_for(EngineProfile::qwen, 262144) == Strings{"--max-context", "262144"}, "the trained 256K: no scaling");
  expect(args_for(EngineProfile::gemma4, 262144) == Strings{"--max-context", "262144"}, "Gemma: no scaling");
  Strings controlled;
  for (const auto flag : secure_frontend::option_controlled_flags(EngineProfile::qwen)) controlled.emplace_back(flag);
  expect(std::find(controlled.begin(), controlled.end(), "--rope-scaling") != controlled.end() &&
             std::find(controlled.begin(), controlled.end(), "--rope-scale") != controlled.end(),
         "the rope flags are the context option's");
  Strings context_flags;
  for (const auto flag : secure_frontend::option_flags(EngineProfile::qwen, "max_context")) context_flags.emplace_back(flag);
  expect(context_flags == Strings{"--max-context", "--rope-scaling", "--rope-scale"}, "the context option's flags: " + joined(context_flags));
  Strings gemma_flags;
  for (const auto flag : secure_frontend::option_flags(EngineProfile::gemma4, "max_context")) gemma_flags.emplace_back(flag);
  expect(gemma_flags == Strings{"--max-context"}, "Gemma's context option sets one flag");
  expect(secure_frontend::option_flags(EngineProfile::qwen, "thinking").empty(), "a config-row option sets no flag");
}

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"option_tables_per_profile", option_tables_per_profile},
      {"card_options_parse_and_bound", card_options_parse_and_bound},
      {"card_option_refusals_name_the_option", card_option_refusals_name_the_option},
      {"page_values_resolve_within_bounds", page_values_resolve_within_bounds},
      {"kv_resident_cross_rules", kv_resident_cross_rules},
      {"options_apply_as_flags_and_keys", options_apply_as_flags_and_keys},
      {"mtp_off_drops_the_draft_options", mtp_off_drops_the_draft_options},
      {"flint_options_follow_the_flint_engine", flint_options_follow_the_flint_engine},
      {"web_search_is_a_checkbox_of_the_tool_profiles", web_search_is_a_checkbox_of_the_tool_profiles},
      {"controlled_flags_listed", controlled_flags_listed},
      {"description_json_shape", description_json_shape},
      {"remembered_options_round_trip_and_tolerate_junk", remembered_options_round_trip_and_tolerate_junk},
      {"context_and_kv_streaming_offer_fixed_values", context_and_kv_streaming_offer_fixed_values},
      {"past_trained_context_adds_yarn", past_trained_context_adds_yarn},
      {"glm_options_write_the_glm_rows", glm_options_write_the_glm_rows},
      {"deepseek_options_are_mayas", deepseek_options_are_mayas},
      {"minimax_options_are_mayas", minimax_options_are_mayas},
  }, failures);
}
