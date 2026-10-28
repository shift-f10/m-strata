#include "secure_frontend/model_options.hpp"

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

// --- The option tables (spec §4) -----------------------------------------------------------

constexpr ModelOptionSpec make_spec(const std::string_view name, const Kind kind, const std::string_view label,
                                    const std::string_view help) {
  ModelOptionSpec spec{};
  spec.name = name;
  spec.kind = kind;
  spec.label = label;
  spec.help = help;
  return spec;
}

constexpr ModelOptionSpec boolean_option(const std::string_view name, const std::string_view label, const std::string_view help) {
  return make_spec(name, Kind::boolean, label, help);
}

constexpr ModelOptionSpec integer_option(const std::string_view name, const std::string_view label, const std::string_view help,
                                         const std::int64_t minimum, const std::int64_t maximum, const Kind kind = Kind::integer) {
  ModelOptionSpec spec = make_spec(name, kind, label, help);
  spec.minimum = minimum;
  spec.maximum = maximum;
  return spec;
}

constexpr ModelOptionSpec decimal_option(const std::string_view name, const std::string_view label, const std::string_view help,
                                         const double minimum, const double maximum) {
  ModelOptionSpec spec = make_spec(name, Kind::decimal, label, help);
  spec.minimum_decimal = minimum;
  spec.maximum_decimal = maximum;
  return spec;
}

constexpr ModelOptionSpec choice_option(const std::string_view name, const std::string_view label, const std::string_view help,
                                        const std::span<const std::string_view> choices) {
  ModelOptionSpec spec = make_spec(name, Kind::choice, label, help);
  spec.choices = choices;
  return spec;
}

// Multi-GPU like upstream (owner 2026-10-28): the GPUs a card runs on, as nvidia-smi numbers them - upstream's "gpu"
// (serve/server.py gpu_list: several GPUs split the model's layers). The engines that split take up to 16; our Gemma 4
// and flint engines run on one (their --gpu uses the first).
constexpr ModelOptionSpec devices_option(const std::string_view help, const std::int64_t most) {
  ModelOptionSpec spec = make_spec("gpus", Kind::devices, "GPUs", help);
  spec.minimum = 1;
  spec.maximum = most;
  return spec;
}
constexpr ModelOptionSpec kGpus =
    devices_option("The graphics cards this model runs on (auto: the first); two or more split its layers across them", 16);
constexpr ModelOptionSpec kOneGpu = devices_option("The graphics card this model runs on (auto: the first); this engine uses one", 1);

// upstream/src/program/generate.cpp:3148-3151 (--kv; "q4" is only an alias of q4_0).
constexpr std::array<std::string_view, 4> kQwenKv{"fp16", "int8", "q4_0", "k8v4"};
// reasoning.effort's levels (frontend_config.cpp, the reasoning.effort row and its reader).
constexpr std::array<std::string_view, 4> kQwenThinking{"none", "low", "medium", "high"};
// Vision for cards (spec 2026-10-28-vision-for-cards-design.md): upstream setup's --vision gpu|cpu|no, as card rows.
constexpr std::array<std::string_view, 3> kVisionModes{"off", "gpu", "cpu"};
// The Gemma spec's D10: thinking off or on (reasoning.effort none or high).
constexpr std::array<std::string_view, 2> kGemmaThinking{"off", "on"};

// The page's dropdowns (owner request 2026-10-23: "make the context length a drop down menu that
// tops out to 1M, and make the kv streaming into a dropdown selection also"). Qwen's contexts run
// to the 1M upstream measured end to end (docs/DETAILS.md "Context extension past 262K"), with
// upstream's setup steps 384K and 768K (docs/INSTALL.md); Gemma's stop at its trained 256K (the
// strata_gemma4 allowlist). KV streaming: off, then resident cells from the engine's floor.
constexpr std::array<std::int64_t, 11> kQwenContexts{4096, 8192, 16384, 32768, 65536, 131072,
                                                     262144, 393216, 524288, 786432, 1048576};
constexpr std::array<std::int64_t, 7> kGemmaContexts{4096, 8192, 16384, 32768, 65536, 131072, 262144};
constexpr std::array<std::int64_t, 7> kKvResidentCells{0, 20480, 32768, 65536, 131072, 262144, 524288};
// Qwen's trained positions: past them a start adds YaRN with factor context / 262,144, upstream's
// own setup rule (docs/INSTALL.md "How much context?"; generate.cpp's --yarn-orig-ctx default).
constexpr std::int64_t kQwenTrainedContext = 262144;

constexpr ModelOptionSpec listed(ModelOptionSpec spec, const std::span<const std::int64_t> values,
                                 const std::string_view zero_label = {}, const std::string_view at_most = {}) {
  spec.values = values;
  spec.zero_label = zero_label;
  spec.at_most = at_most;
  return spec;
}

// KV streaming's floor: upstream/src/core/layer.cpp:771 qsa_kv_resident_min() (a smaller
// --kv-resident is raised to it, :752; usage generate.cpp:820 "min 20480").
constexpr std::int64_t kKvResidentMinimum = 20480;

// The sampling options carry the defaults.sampling.* rows' bounds (frontend_config.cpp, SP5 M1
// Task 4). top_p's row is 0 < top_p <= 1 (minimum_exclusive): the smallest double above zero
// makes the same set an inclusive bound, so the card's and the page's checks stay one compare.
constexpr ModelOptionSpec kTemperature =
    decimal_option("temperature", "Temperature", "Default sampling temperature for requests that set none (0-2)", 0.0, 2.0);
constexpr ModelOptionSpec kTopP = decimal_option("top_p", "Top-p", "Default nucleus mass for requests that set none (above 0, at most 1)",
                                                 std::numeric_limits<double>::denorm_min(), 1.0);
constexpr ModelOptionSpec kTopK =
    integer_option("top_k", "Top-k", "Default number of candidate tokens for requests that set none (1-64)", 1, 64);
constexpr ModelOptionSpec kMinP =
    decimal_option("min_p", "Min-p", "Default minimum token probability, relative to the likeliest, for requests that set none (0 = off)",
                   0.0, 1.0);
// Owner 2026-10-28 ("a toggle in the card that can enable search"): the profiles with the server's tool (the chat
// route's tool formats). It writes no flag and no row; the launcher reads it at a start (ModelLauncher::run_start).
constexpr ModelOptionSpec kWebSearch =
    boolean_option("web_search", "Web search", "Let this model search the web through the server's SearXNG while it answers");

// The upstream Strata engine (strata_0143). Flags, units and limits as its argument parser reads
// them (upstream/src/program/generate.cpp): --max-context :2428 (positive, below 2^31 :3254-3258;
// the frontend's cap 1,048,576 is glm.max_context's); --kv-resident :2464 (>= 0 :3162); --kv-grow
// :2465; --spec :2591 (the verify window: below 2 the draft layer is ignored :5403-5404, above
// kVerifyMaxT = 8 the drafter refuses, core/mtp.cpp:266); --spec-min-p :2613 (a probability the
// draft must reach, core/mtp.cpp:1689); --expert-cache :2541-2544 (auto or a count of expert blobs
// in VRAM, >= -1 :3304; at most the 48 x 512 experts there are, core/geometry.hpp:25/:53);
// --prefill :2558-2565 (auto or a chunk of N prompt tokens; auto's largest is 32,768, :2564).
constexpr std::array<ModelOptionSpec, 17> kQwenOptions{{
    listed(integer_option("max_context", "Context length",
                          "Tokens of context the engine allocates; past the trained 256K the engine stretches the model with "
                          "YaRN rope scaling (experimental, upstream's own rule)",
                          1, 1048576),
           kQwenContexts),
    choice_option("kv", "KV cache precision",
                  "How the engine stores the attention cache: fp16, int8, 4-bit q4_0, or k8v4 (int8 keys, 4-bit values)", kQwenKv),
    listed(integer_option("kv_resident", "KV streaming",
                          "Attention-cache tokens kept in VRAM, the rest streamed from RAM (Off: all of it in VRAM); at most "
                          "the context",
                          0, 1048576),
           kKvResidentCells, "Off", "max_context"),
    boolean_option("kv_grow", "Grow the KV cache", "Take VRAM for the attention cache as the context fills instead of all at start"),
    boolean_option("mtp", "MTP draft layer", "Guess tokens ahead with the model's draft layer"),
    integer_option("spec", "Draft window", "Tokens the engine proposes per verify step (2-8)", 2, 8),
    decimal_option("spec_min_p", "Draft confidence", "How sure the draft layer must be to extend a window by another guess (0-1)",
                   0.0, 1.0),
    integer_option("expert_cache", "Expert cache", "Expert blobs kept in VRAM, or auto to fill what fits", 0, 24576,
                   Kind::integer_or_auto),
    integer_option("prefill", "Prompt chunk", "Prompt tokens processed per batch, or auto for the largest that fits", 1, 32768,
                   Kind::integer_or_auto),
    choice_option("thinking", "Thinking", "The thinking level for requests that name none", kQwenThinking),
    kTemperature,
    kTopP,
    kTopK,
    kMinP,
    choice_option("vision", "Pictures",
                  "Read pictures with the model's picture encoder: on the GPU (faster, takes about 1 GB of VRAM), on the CPU, or off",
                  kVisionModes),
    kWebSearch,
    kGpus,
}};

// Our strata-gemma4-engine (strata_gemma4): the bounds are kGemma4Flags (engine_arguments.cpp). Web search since
// 2026-10-28 (Gemma 4's own tool form).
constexpr std::array<ModelOptionSpec, 10> kGemmaOptions{{
    listed(integer_option("max_context", "Context length", "Tokens of context the engine allocates (up to the model's trained 256K)",
                          1, 262144),
           kGemmaContexts),
    integer_option("chunk", "Prompt chunk", "Prompt tokens processed per batch", 1, 8192),
    integer_option("expert_vram", "Expert VRAM", "GiB of VRAM for the experts, or auto to let the engine decide", 0, 1024,
                   Kind::integer_or_auto),
    choice_option("thinking", "Thinking", "Whether requests that name no thinking level think", kGemmaThinking),
    kTemperature,
    kTopP,
    kTopK,
    kMinP,
    kWebSearch,
    kOneGpu,
}};

// multiengine_flint (strata_flint): the Gemma engine's options, bounded by kFlintFlags (engine_arguments.cpp); its
// trained context is 256K too.
constexpr std::array<ModelOptionSpec, 10> kFlintOptions{{
    listed(integer_option("max_context", "Context length", "Tokens of context the engine allocates (up to the model's trained 256K)",
                          1, 262144),
           kGemmaContexts),
    integer_option("chunk", "Prompt chunk", "Prompt tokens processed per batch", 1, 8192),
    integer_option("expert_vram", "Expert VRAM", "GiB of VRAM for the experts, or auto to let the engine decide", 0, 1024,
                   Kind::integer_or_auto),
    choice_option("thinking", "Thinking", "Whether requests that name no thinking level think", kGemmaThinking),
    kTemperature,
    kTopP,
    kTopK,
    kMinP,
    kWebSearch,
    kOneGpu,
}};

// Maya's GLM-5.3 engine (multiengine_maya, the glm53 profile, launcher spec D10 met 2026-10-27): its launch is
// composed from typed rows, never engine.args, so each option writes the row the composition reads (write_option).
// Contexts: Maya's menu (maya.py v1.1.30 CONTEXTS, DEFAULT_CONTEXT 32768) inside glm.max_context's bounds; --kv: the
// GLM engine's vocabulary (glm.kv, glm_driver.cpp); no thinking option (the GLM prompt opens its think block always).
constexpr std::array<std::int64_t, 4> kGlmContexts{8192, 32768, 65536, 131072};
constexpr std::array<std::string_view, 4> kGlmKv{"fp16", "int8", "q8_0", "q4_0"};
constexpr std::array<ModelOptionSpec, 8> kGlmOptions{{
    listed(integer_option("max_context", "Context length",
                          "Tokens of context the engine allocates (Maya's choices; past 32K the attention cache streams from RAM)",
                          1, 1048576),
           kGlmContexts),
    choice_option("kv", "KV cache precision", "How the engine stores the attention cache: fp16, int8, or ggml's q8_0 / q4_0 blocks",
                  kGlmKv),
    kTemperature,
    kTopP,
    kTopK,
    kMinP,
    kWebSearch,
    kGpus,
}};

// raon-1-mini on our port of Maya's GLM engine (multiengine_raon1, the raon1 profile; owner 2026-10-27: its card from
// its files): the same typed rows as glm53 (glm.max_context, glm.kv) up to the 256K it was served at; the cache in the
// GLM engine's vocabulary (fp16 keeps raon1's exact F32 cache); KRAFTON's sampling as the card's defaults.
constexpr std::array<std::int64_t, 5> kRaon1Contexts{8192, 32768, 65536, 131072, 262144};
constexpr std::array<ModelOptionSpec, 8> kRaon1Options{{
    listed(integer_option("max_context", "Context length", "Tokens of context the engine allocates (its full attention keeps them all)",
                          1, 1048576),
           kRaon1Contexts),
    choice_option("kv", "KV cache precision", "How the engine stores the attention cache: fp16 (raon1's exact cache), int8, or ggml's q8_0 / q4_0 blocks",
                  kGlmKv),
    kTemperature,
    kTopP,
    kTopK,
    kMinP,
    kWebSearch,
    kGpus,
}};

// DeepSeek-V4.1 on Maya's maya-ds4 (2026-10-27, spec 2026-10-27-strata-prepare-design §7.2): Maya's own choices - its
// context menu (maya.py v1.1.30 DS4_CONTEXTS; maya-ds4's own --max-context, in engine.args by the maya_ds4 allowlist),
// the thinking level (DS4_EFFORT "high"; DeepSeek's effort line: low 50, medium and high 75, none chat mode), sampling.
constexpr std::array<std::int64_t, 6> kDeepseekContexts{32768, 65536, 131072, 262144, 524288, 1048576};
constexpr std::array<ModelOptionSpec, 8> kDeepseekOptions{{
    listed(integer_option("max_context", "Context length", "Tokens of context the engine allocates (Maya's choices)", 1, 1048576),
           kDeepseekContexts),
    choice_option("thinking", "Thinking", "The thinking level for requests that name none (DeepSeek's reasoning effort)",
                  kQwenThinking),
    kTemperature,
    kTopP,
    kTopK,
    kMinP,
    kWebSearch,  // DSML calls, as Maya renders and reads them (owner 2026-10-28)
    kGpus,
}};

// MiniMax-M3.1-Flash on Maya's maya-mm3 (owner 2026-10-28): Maya's own choices - its context menu (maya.py v1.1.30
// MM3_CONTEXTS, MM3_DEFAULT_CONTEXT 64K), the KV cache formats Maya's setup sizes (MM3_KV_BYTES; MM3_KV_FORMAT int8),
// the thinking level (MM3_EFFORT "auto": MiniMax's adaptive thinking, no effort named), MiniMax's sampling (its model
// card's temperature and top_p, no top-k or min-p). The context and the cache are maya-mm3's own flags in engine.args.
constexpr std::array<std::int64_t, 6> kMinimaxContexts{32768, 65536, 131072, 262144, 524288, 1048576};
constexpr std::array<std::string_view, 4> kMinimaxKv{"bf16", "int8", "fp8", "fp4"};
constexpr std::array<std::string_view, 5> kMinimaxThinking{"auto", "none", "low", "medium", "high"};
constexpr std::array<ModelOptionSpec, 6> kMinimaxOptions{{
    listed(integer_option("max_context", "Context length",
                          "Tokens of context the engine allocates (Maya's choices; past 256K not yet run end to end)", 1, 1048576),
           kMinimaxContexts),
    choice_option("kv", "KV cache precision", "How the engine stores the attention cache: bf16, int8, fp8 or fp4", kMinimaxKv),
    choice_option("thinking", "Thinking",
                  "The thinking level for requests that name none (auto: MiniMax decides when to think)", kMinimaxThinking),
    kTemperature,
    kTopP,
    kGpus,
}};

constexpr std::array<std::string_view, 1> kDeepseekFlags{"--max-context"};
constexpr std::array<std::string_view, 2> kMinimaxFlags{"--max-context", "--kv-cache"};
constexpr std::array<std::string_view, 10> kQwenFlags{"--max-context", "--rope-scaling", "--rope-scale",   "--kv",
                                                      "--kv-resident", "--kv-grow",      "--spec",         "--spec-min-p",
                                                      "--expert-cache", "--prefill"};
constexpr std::array<std::string_view, 3> kGemmaFlags{"--max-context", "--chunk", "--expert-vram"};

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

void write_option(const EngineProfile profile, const std::string_view name, const ModelOptionValue& value, const bool draft_layer_on,
                  std::vector<std::string>& flags, FrontendConfigDraft& draft) {
  const auto* integer = std::get_if<std::int64_t>(&value);
  const auto* decimal = std::get_if<double>(&value);
  const auto* text = std::get_if<std::string>(&value);
  const auto* on = std::get_if<bool>(&value);
  const auto flag_with = [&flags](const std::string_view flag, std::string argument) {
    flags.emplace_back(flag);
    flags.push_back(std::move(argument));
  };
  const auto integer_or_auto = [integer, text]() { return integer != nullptr ? std::to_string(*integer) : *text; };
  if ((profile == EngineProfile::glm53 || profile == EngineProfile::raon1) && (name == "max_context" || name == "kv")) {
    // the GLM engine's launch (glm53, raon1) reads its typed rows (launch_composition: Glm53LaunchArgs), never engine.args
    if (name == "max_context" && integer != nullptr) set_entry(draft, "glm.max_context", ConfigDraftValue{std::in_place_type<std::int64_t>, *integer});
    if (name == "kv" && text != nullptr) set_entry(draft, "glm.kv", ConfigDraftValue{std::in_place_type<std::string>, *text});
    return;
  }
  if (name == "max_context" && integer != nullptr) {
    flag_with("--max-context", std::to_string(*integer));
    if (profile == EngineProfile::qwen && *integer > kQwenTrainedContext) {
      flag_with("--rope-scaling", "yarn");
      flag_with("--rope-scale", decimal_text(static_cast<double>(*integer) / static_cast<double>(kQwenTrainedContext)));
    }
  }
  else if (name == "kv" && text != nullptr) flag_with(profile == EngineProfile::minimax ? "--kv-cache" : "--kv", *text);
  else if (name == "kv_resident" && integer != nullptr) {
    if (*integer > 0) flag_with("--kv-resident", std::to_string(*integer));
  } else if (name == "kv_grow" && on != nullptr) {
    if (*on) flags.emplace_back("--kv-grow");
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
    if (draft_layer_on) flag_with("--spec-min-p", decimal_text(*decimal));
  } else if ((name == "expert_cache" || name == "prefill") && (integer != nullptr || text != nullptr)) {
    flag_with(name == "expert_cache" ? "--expert-cache" : "--prefill", integer_or_auto());
  } else if (name == "chunk" && integer != nullptr) flag_with("--chunk", std::to_string(*integer));
  else if (name == "expert_vram" && integer != nullptr) flag_with("--expert-vram", std::to_string(*integer));  // auto: no flag
  else if (name == "thinking" && text != nullptr) {
    if (profile == EngineProfile::minimax && *text == "auto") return;  // MiniMax's adaptive default: no level named
    if (profile == EngineProfile::gemma4 || profile == EngineProfile::flint) {
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
      const bool glm = profile == EngineProfile::glm53 || profile == EngineProfile::raon1;
      set_entry(draft, glm ? "glm.gpu" : "engine.gpu", ConfigDraftValue{std::in_place_type<std::string>, *text});
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
  // glm53, raon1: typed rows only
  if (find_model_option(profile, name) == nullptr || profile == EngineProfile::glm53 || profile == EngineProfile::raon1) return {};
  if (name == "max_context") {
    if (profile == EngineProfile::qwen) return {"--max-context", "--rope-scaling", "--rope-scale"};
    return {"--max-context"};
  }
  if (profile == EngineProfile::minimax && name == "kv") return {"--kv-cache"};  // maya-mm3's own
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 9> kFlags{{
      {"kv", "--kv"}, {"kv_resident", "--kv-resident"}, {"kv_grow", "--kv-grow"}, {"spec", "--spec"},
      {"spec_min_p", "--spec-min-p"}, {"expert_cache", "--expert-cache"}, {"prefill", "--prefill"}, {"chunk", "--chunk"},
      {"expert_vram", "--expert-vram"}}};
  for (const auto& [option, flag] : kFlags) {
    if (option == name) return {flag};
  }
  return {};
}

std::span<const ModelOptionSpec> model_options_for(const EngineProfile profile) noexcept {
  switch (profile) {
    case EngineProfile::qwen: return kQwenOptions;
    case EngineProfile::gemma4: return kGemmaOptions;
    case EngineProfile::glm53: return kGlmOptions;
    case EngineProfile::deepseek: return kDeepseekOptions;
    case EngineProfile::raon1: return kRaon1Options;
    case EngineProfile::flint: return kFlintOptions;
    case EngineProfile::minimax: return kMinimaxOptions;
  }
  return {};
}

const ModelOptionSpec* find_model_option(const EngineProfile profile, const std::string_view name) noexcept {
  const auto options = model_options_for(profile);
  const auto it = std::find_if(options.begin(), options.end(), [name](const ModelOptionSpec& spec) { return spec.name == name; });
  return it == options.end() ? nullptr : &*it;
}

bool is_model_option_name(const std::string_view name) noexcept {
  return find_model_option(EngineProfile::qwen, name) != nullptr || find_model_option(EngineProfile::gemma4, name) != nullptr;
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
  switch (profile) {
    case EngineProfile::qwen: return kQwenFlags;
    case EngineProfile::gemma4: return kGemmaFlags;
    case EngineProfile::glm53: return {};
    case EngineProfile::deepseek: return kDeepseekFlags;  // maya-ds4's --max-context
    case EngineProfile::raon1: return {};
    case EngineProfile::flint: return kGemmaFlags;  // --max-context, --chunk, --expert-vram
    case EngineProfile::minimax: return kMinimaxFlags;  // maya-mm3's --max-context, --kv-cache
  }
  return {};
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
