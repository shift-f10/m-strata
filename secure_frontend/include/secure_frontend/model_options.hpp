#pragma once

// Model launcher (spec docs/superpowers/specs/2026-10-23-model-launcher-design.md §4, plan
// Task 2): the options the page may set on a model card before it starts, per engine profile.
// Each option maps to one engine flag or one config key the frontend composes; the page never
// supplies a flag, a path or free text, only a typed value inside the card's bounds, which sit
// inside the engine's own (the hard bounds below).

#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/json.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

// integer_or_auto: the string "auto" or an integer. devices: "auto" or GPU numbers 0-63 separated by commas, none
// repeated ("0,1"), at most `maximum` of them (multi-GPU, 2026-10-28).
enum class ModelOptionKind { boolean, integer, decimal, choice, integer_or_auto, devices };

// One option of an engine profile with the engine's hard bounds.
struct ModelOptionSpec final {
  std::string_view name;   // the card key and the API name: [a-z_]+
  ModelOptionKind kind;
  std::string_view label;  // the page's label
  std::string_view help;   // one fixed sentence for the page
  std::int64_t minimum{0};   // integer and integer_or_auto
  std::int64_t maximum{0};
  double minimum_decimal{0.0};  // decimal (inclusive)
  double maximum_decimal{0.0};
  std::span<const std::string_view> choices{};  // choice
  // Integer kinds (owner request 2026-10-23, the page's dropdowns): the only values offered,
  // ascending; the card's min/max trim the list. Empty: any integer within the bounds.
  std::span<const std::int64_t> values{};
  std::string_view zero_label{};  // the label of 0 among `values` ("Off"); empty: "0"
  std::string_view at_most{};     // another option this one may not exceed (KV streaming: the context)
};

// The profile's options in display order (qwen and gemma4; glm53 has none in v1, D10).
[[nodiscard]] std::span<const ModelOptionSpec> model_options_for(EngineProfile profile) noexcept;
[[nodiscard]] const ModelOptionSpec* find_model_option(EngineProfile profile, std::string_view name) noexcept;
// True for an option name of any profile (the fixed names an error may carry as its param).
[[nodiscard]] bool is_model_option_name(std::string_view name) noexcept;
// The page's label of an offered value: zero_label for 0, else 1,024-token units ("4K", "256K",
// "1M"); a value that is not a whole number of them is written out.
[[nodiscard]] std::string option_value_label(const ModelOptionSpec& spec, std::int64_t value);
// The engine flags one option writes (empty for an option that sets a config row): a card that
// lists the option may not fix any of them in its engine.args.
[[nodiscard]] std::vector<std::string_view> option_flags(EngineProfile profile, std::string_view name);

using ModelOptionValue = std::variant<bool, std::int64_t, double, std::string>;
using ModelOptionValues = std::map<std::string, ModelOptionValue, std::less<>>;

// Each model family's recommended sampling (owner 2026-10-29: "each model reset it to the recommended"): upstream's
// MODEL_CARD_SAMPLING for thinking (serve/server.py:8054: Qwen3.8 and Qwen3.6 1.0 / 0.95 / top-k 20, DeepSeek-V4.1
// 1.0 / 1.0) and the shipped and page-made cards for the rest (Gemma 4 1.0 / 0.95 / 64 / min-p 0.05; flint and raon1
// 1.0 / 0.95 / 20; GLM-5.3 and MiniMax 1.0 / 0.95). A card whose file does not list one of these options offers it
// with this default (the model launcher), so a start with the card's defaults samples as the model's card says.
[[nodiscard]] ModelOptionValues recommended_sampling(EngineProfile profile);

// A card's view of one option: its default and the card's own bounds inside the spec's.
struct ModelOptionLimits final {
  const ModelOptionSpec* spec{nullptr};
  ModelOptionValue default_value;
  std::int64_t minimum{0};       // integer kinds (the spec's unless the card narrows them)
  std::int64_t maximum{0};
  double minimum_decimal{0.0};   // decimal
  double maximum_decimal{0.0};
  std::vector<std::string> choices{};  // choice: the card's subset, in the spec's order
};

// A card's "options" object. Each member names an option of `profile` and holds an object
// {"default": v, "min": n, "max": n, "choices": [...]} ("default" required; min/max only for
// integer, integer_or_auto and decimal; choices only for choice, a non-empty subset of the
// spec's). Issues use the key path "options.<name>" (or "options" for the object itself) and
// fixed reasons; they never echo a value. The result keeps the spec's display order.
[[nodiscard]] std::variant<std::vector<ModelOptionLimits>, ConfigErrors> parse_card_options(EngineProfile profile,
                                                                                             const JsonValue& options);

// The values a start uses, for every option the card lists: the requested value (the start
// body's "options" object; nullptr = none) when present, else the remembered one when it is
// still inside the card's bounds, else the card's default. A requested name the card does not
// list, a value of the wrong type or outside the bounds is refused (key "options.<name>"; a
// name that is no option's is never echoed: key "options").
// Cross-option rules: kv_resident is 0 or at least 20,480 and at most max_context (when the
// card lists max_context); spec_min_p is resolved but not applied while mtp is false (spec
// stays: without the draft layer the engine still drafts by prompt lookup with --spec, and a
// native pack needs it to start — upstream strata_control/draft_layer.py:16-17).
[[nodiscard]] std::variant<ModelOptionValues, ConfigErrors> resolve_option_values(
    const std::vector<ModelOptionLimits>& limits, const JsonValue* requested, const ModelOptionValues& remembered);

// Writes the values into a card's config draft: flags are appended to the "engine.args" entry
// (created when absent) in the profile's option order; mtp false sets "engine.mtp" false;
// thinking sets "reasoning.effort"; temperature, top_p, top_k and min_p set
// "defaults.sampling.*". Every entry written has ConfigSource::file.
void apply_option_values(EngineProfile profile, const ModelOptionValues& values, FrontendConfigDraft& draft);

// The engine flags an option controls. A card's fixed engine.args may not hold one (its
// option is the only source), refused by name in model_cards.
[[nodiscard]] std::span<const std::string_view> option_controlled_flags(EngineProfile profile) noexcept;

// The "options" array of GET /v1/strata/models: one object per limit, in order, with name,
// kind ("boolean" | "integer" | "decimal" | "choice" | "integer_or_auto"), label, help,
// default, min and max (integer kinds and decimal), choices (choice), and value (from
// `values`, else the default).
[[nodiscard]] std::string describe_options_json(const std::vector<ModelOptionLimits>& limits, const ModelOptionValues& values);

// D7: the remembered options file under roots.state, {"<card name>": {"<option>": value}}.
// Rendering is strict JSON with a final newline. Parsing is lenient by design: a file that is
// not this shape, or a member that is not, is dropped (the card's defaults apply), never an error.
using RememberedOptions = std::map<std::string, ModelOptionValues, std::less<>>;
[[nodiscard]] std::string render_remembered_options(const RememberedOptions& remembered);
[[nodiscard]] RememberedOptions parse_remembered_options(std::string_view text);

}  // namespace secure_frontend
