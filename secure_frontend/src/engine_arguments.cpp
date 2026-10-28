#include "secure_frontend/engine_arguments.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace secure_frontend {
namespace {
// --pack (the expert directory) and the other weight sources (--ple-gguf, --embd-gguf, --native-head-gguf,
// --native-dense-gguf, --mtp) are operator-supplied and pass through; only shard 1 (--native) is frontend-owned.
// --gpu is frontend-owned too: GPUs are chosen only by the typed engine.gpu list (spec §3.9).
constexpr std::array<std::string_view, 3> kOwnedFlags{"--native", "--serve", "--gpu"};
constexpr std::array<std::string_view, 3> kUnsupportedModes{"--batch", "--slots", "--vision"};

std::string_view flag_name(const std::string_view argument) {
  const auto equals = argument.find('=');
  return equals == std::string_view::npos ? argument : argument.substr(0, equals);
}

// The serve flags of an engine built on our own family code (Gemma 4 plan 1 Task 1, spec §4.8; flint), a closed
// allowlist: each flag at most once, in the `--flag VALUE` spelling only, its value bounded.
enum class FamilyValue { none, integer, kv_precision, gibibytes, fraction, on_off, mm3_kv_cache };
struct FamilyFlag final {
  std::string_view name;
  FamilyValue kind;
  std::int64_t minimum;
  std::int64_t maximum;
};
struct FamilyArguments final {
  std::string_view profile;  // "gemma4": the refusals read "engine.profile gemma4: ..."
  std::string_view engine;   // "the Gemma engine's": "... is not one of the Gemma engine's flags"
  std::span<const FamilyFlag> flags;
  // Flags an operator may carry over from another engine, or the engine's own test-only ones (refused with --serve):
  // refused by name. Any other argument is refused without being echoed (EngineArgumentsError::message is fixed).
  std::span<const std::string_view> refused_by_name;
};

// Gemma 4: --max-context up to the trained context (262,144, spec §2.1); --expert-slots up to the 128 experts of a
// layer; --kv the two cache precisions (D3); --expert-vram GiB with at most two decimal places.
constexpr std::array<FamilyFlag, 8> kGemma4Flags{{{"--max-context", FamilyValue::integer, 1, 262144},
                                                  {"--chunk", FamilyValue::integer, 1, 8192},
                                                  {"--kv", FamilyValue::kv_precision, 0, 0},
                                                  {"--no-graph", FamilyValue::none, 0, 0},
                                                  {"--expert-vram", FamilyValue::gibibytes, 0, 1024},
                                                  {"--expert-slots", FamilyValue::integer, 0, 128},
                                                  {"--hybrid-rows", FamilyValue::integer, 1, 4096},
                                                  {"--cpu-threads", FamilyValue::integer, 1, 256}}};
constexpr std::array<std::string_view, 31> kGemma4RefusedByName{
    "--model",          "--tokens",          "--force",           "--dump-full-logits", "--dump-layers",
    "--bench-pp",       "--bench-tg",        "--mtp",             "--spec",             "--spec-min-p",
    "--batch-mtp",      "--draft-vocab",     "--pack",            "--ple-gguf",         "--embd-gguf",
    "--native-head-gguf", "--native-dense-gguf", "--tail-role-token", "--emb",          "--ctx",
    "--prefill",        "--pool-workers",    "--no-token-graph",  "--no-warmup",        "--greedy",
    "--max-new",        "--n",               "--vram-reserve-mib", "--vram-elastic",    "--layer-split",
    "--expert-profile-save"};
constexpr FamilyArguments kGemma4Arguments{"gemma4", "the Gemma engine's", kGemma4Flags, kGemma4RefusedByName};

// flint (multiengine_flint, engine/src/flint/f_cli.cpp): its serve flags with the engine's own bounds; --expert-slots
// up to the 256 experts of a layer, --pcie-frac a share 0-1 (at most three decimals), --cpu-lane on or off. The
// expert-profile paths stay out (the frontend gives the engine no file path but --native).
constexpr std::array<FamilyFlag, 9> kFlintFlags{{{"--max-context", FamilyValue::integer, 1, 262144},
                                                 {"--chunk", FamilyValue::integer, 1, 8192},
                                                 {"--no-graph", FamilyValue::none, 0, 0},
                                                 {"--expert-vram", FamilyValue::gibibytes, 0, 1024},
                                                 {"--expert-slots", FamilyValue::integer, 0, 256},
                                                 {"--cpu-threads", FamilyValue::integer, 1, 256},
                                                 {"--pcie-frac", FamilyValue::fraction, 0, 1},
                                                 {"--cpu-lane", FamilyValue::on_off, 0, 0},
                                                 {"--no-prompt-borrow", FamilyValue::none, 0, 0}}};
constexpr std::array<std::string_view, 37> kFlintRefusedByName{
    "--model",          "--tokens",          "--force",           "--dump-full-logits", "--dump-layers",
    "--layers",         "--check",           "--bench-pp",        "--bench-tg",         "--n",
    "--kv",             "--hybrid-rows",     "--expert-profile",  "--expert-profile-save", "--mtp",
    "--spec",           "--spec-min-p",      "--batch-mtp",       "--draft-vocab",      "--pack",
    "--ple-gguf",       "--embd-gguf",       "--native-head-gguf", "--native-dense-gguf", "--tail-role-token",
    "--emb",            "--ctx",             "--prefill",         "--pool-workers",     "--no-token-graph",
    "--no-warmup",      "--greedy",          "--max-new",         "--vram-reserve-mib", "--vram-elastic",
    "--layer-split",    "--expert-cache"};
constexpr FamilyArguments kFlintArguments{"flint", "the flint engine's", kFlintFlags, kFlintRefusedByName};

// DeepSeek-V4.1 on Maya's maya-ds4 (Maya v1.1.30 src/ds4/ds4_main.cpp:1259-1298): the serve flags write_config_ds4
// writes or keeps (maya.py :4136-4201) - --tp up to 8 GPUs, --threads on/off, the context to Maya's 1M. The weight
// source (--pack, composed), the GPUs (--gpus, composed), the one-shot modes, the vision tower, the drafter and the
// expert-profile path (the pack's own expert-profile.bin) are refused by name.
constexpr std::array<FamilyFlag, 6> kMayaDs4Flags{{{"--max-context", FamilyValue::integer, 1, 1048576},
                                                   {"--tp", FamilyValue::integer, 1, 8},
                                                   {"--threads", FamilyValue::on_off, 0, 0},
                                                   {"--cpu-threads", FamilyValue::integer, 1, 256},
                                                   {"--prefill-chunk", FamilyValue::integer, 1, 65536},
                                                   {"--reserve-mib", FamilyValue::integer, 0, 1048576}}};
constexpr std::array<std::string_view, 17> kMayaDs4RefusedByName{
    "--pack",         "--gpus",          "--ids",        "--images",       "--n",          "--dump-logits",
    "--ppl",          "--encode-image",  "--size",       "--dump-image",   "--expert-profile", "--no-expert-cache",
    "--verbose",      "--dspark",        "--dspark-conf", "--vram-experts", "--mmap-experts"};
constexpr FamilyArguments kMayaDs4Arguments{"deepseek", "maya-ds4's", kMayaDs4Flags, kMayaDs4RefusedByName};

// MiniMax-M3.1-Flash on Maya's maya-mm3 (owner 2026-10-28; Maya v1.1.30 src/mm3/mm3_main.cpp:901-994): the serve flags
// write_config_mm3 writes or keeps for a GGUF pack (maya.py :4536-4634) - the context to 1M, the KV cache in the four
// formats Maya's setup offers (MM3_KV_BYTES), KV streaming and its resident window, the prompt chunk, CPU threads, the
// VRAM kept free. The weight source (--pack), the GPUs (--gpus) and the expert profile (--profile) are composed; the
// one-shot modes, the vision tower, the drafter and the EXL3-only tensor parallel are refused by name.
constexpr std::array<FamilyFlag, 7> kMayaMm3Flags{{{"--max-context", FamilyValue::integer, 1, 1048576},
                                                   {"--kv-cache", FamilyValue::mm3_kv_cache, 0, 0},
                                                   {"--kv-streaming", FamilyValue::on_off, 0, 0},
                                                   {"--kv-resident", FamilyValue::integer, 0, 1048576},
                                                   {"--chunk", FamilyValue::integer, 1, 65536},
                                                   {"--cpu-threads", FamilyValue::integer, 1, 256},
                                                   {"--reserve-gb", FamilyValue::gibibytes, 0, 1024}}};
constexpr std::array<std::string_view, 30> kMayaMm3RefusedByName{
    "--pack",          "--gpus",        "--profile",      "--profile-out",  "--expert-profile", "--ids",
    "--gen",           "--last",        "--dump-logits",  "--ppl",          "--batch",          "--calibrate-prefill",
    "--encode-image",  "--size",        "--dump-image",   "--images",       "--vision-gpu",     "--dspark",
    "--dspark-conf",   "--dspark-cost", "--score",        "--write-ref",    "--eos",            "--verbose",
    "--tp",            "--tp-chunk",    "--tp-ffn-rows",  "--cache-gb",     "--layer-split",    "--prefill-tune"};
constexpr FamilyArguments kMayaMm3Arguments{"minimax", "maya-mm3's", kMayaMm3Flags, kMayaMm3RefusedByName};

EngineArgumentsError family_refusal(const FamilyArguments& family, const EngineArgumentsErrorCode code,
                                    const std::string_view flag, const std::string_view what) {
  return EngineArgumentsError{code, "engine.profile " + std::string{family.profile} + ": " + std::string{flag} + " " +
                                        std::string{what}};
}

bool all_digits(const std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](const char c) { return c >= '0' && c <= '9'; });
}

// Digits only: no sign, no leading zero (but "0"), at most 19 of them.
std::optional<EngineArgumentsError> family_integer(const FamilyArguments& family, const FamilyFlag& flag,
                                                   const std::string_view value) {
  const std::string range = "must be " + std::to_string(flag.minimum) + "-" + std::to_string(flag.maximum);
  if (value.empty() || !all_digits(value) || (value.size() > 1 && value.front() == '0')) {
    return family_refusal(family, EngineArgumentsErrorCode::invalid_value, flag.name, "takes a decimal integer");
  }
  std::int64_t parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() || parsed < flag.minimum || parsed > flag.maximum) {
    return family_refusal(family, EngineArgumentsErrorCode::out_of_range, flag.name, range);
  }
  return std::nullopt;
}

// A decimal `0|[1-9][0-9]{0,3}`, then optionally `.` and 1 to `places` digits, within the flag's whole-number bounds.
std::optional<EngineArgumentsError> family_decimal(const FamilyArguments& family, const FamilyFlag& flag,
                                                   const std::string_view value, const std::size_t places,
                                                   const std::string_view shape, const std::string_view unit) {
  const auto dot = value.find('.');
  const std::string_view whole = value.substr(0, dot);
  const std::string_view fraction = dot == std::string_view::npos ? std::string_view{} : value.substr(dot + 1);
  const bool shaped = !whole.empty() && whole.size() <= 4 && all_digits(whole) && (whole.size() == 1 || whole.front() != '0') &&
                      (dot == std::string_view::npos || (!fraction.empty() && fraction.size() <= places && all_digits(fraction)));
  if (!shaped) return family_refusal(family, EngineArgumentsErrorCode::invalid_value, flag.name, shape);
  std::int64_t scale = 1;
  for (std::size_t i = 0; i < places; ++i) scale *= 10;
  std::int64_t scaled = 0;
  for (const char c : whole) scaled = scaled * 10 + (c - '0');
  scaled *= scale;
  std::int64_t digit = scale;
  for (const char c : fraction) {
    digit /= 10;
    scaled += (c - '0') * digit;
  }
  if (scaled < flag.minimum * scale || scaled > flag.maximum * scale) {
    return family_refusal(family, EngineArgumentsErrorCode::out_of_range, flag.name,
                          "must be " + std::to_string(flag.minimum) + "-" + std::to_string(flag.maximum) + std::string{unit});
  }
  return std::nullopt;
}

std::optional<EngineArgumentsError> validate_family_arguments(const FamilyArguments& family,
                                                              const std::vector<std::string>& user_arguments) {
  std::vector<std::string_view> seen;
  for (std::size_t i = 0; i < user_arguments.size(); ++i) {
    const std::string_view argument = user_arguments[i];
    const auto name = flag_name(argument);
    const auto flag = std::find_if(family.flags.begin(), family.flags.end(),
                                   [name](const FamilyFlag& entry) { return entry.name == name; });
    if (flag == family.flags.end()) {
      const auto known = std::find(family.refused_by_name.begin(), family.refused_by_name.end(), name);
      if (known != family.refused_by_name.end()) {
        return family_refusal(family, EngineArgumentsErrorCode::not_accepted, *known,
                              "is not one of " + std::string{family.engine} + " flags");
      }
      return EngineArgumentsError{EngineArgumentsErrorCode::not_accepted,
                                  "engine.profile " + std::string{family.profile} + ": the engine argument is not one of " +
                                      std::string{family.engine} + " flags"};
    }
    if (name.size() != argument.size()) {
      return family_refusal(family, EngineArgumentsErrorCode::not_accepted, flag->name, "takes its value as the next argument");
    }
    if (std::find(seen.begin(), seen.end(), flag->name) != seen.end()) {
      return family_refusal(family, EngineArgumentsErrorCode::invalid_value, flag->name, "is given more than once");
    }
    seen.push_back(flag->name);
    if (flag->kind == FamilyValue::none) continue;
    if (i + 1 >= user_arguments.size() || user_arguments[i + 1].rfind("--", 0) == 0) {
      return family_refusal(family, EngineArgumentsErrorCode::invalid_value, flag->name, "needs a value");
    }
    const std::string_view value = user_arguments[++i];
    std::optional<EngineArgumentsError> refused;
    switch (flag->kind) {
      case FamilyValue::none: break;
      case FamilyValue::integer: refused = family_integer(family, *flag, value); break;
      case FamilyValue::gibibytes:
        refused = family_decimal(family, *flag, value, 2U, "takes GiB with at most two decimal places", " GiB");
        break;
      case FamilyValue::fraction:
        refused = family_decimal(family, *flag, value, 3U, "takes a share with at most three decimal places", "");
        break;
      case FamilyValue::kv_precision:
        if (value != "f16" && value != "f32") {
          refused = family_refusal(family, EngineArgumentsErrorCode::invalid_value, flag->name, "must be f16 or f32");
        }
        break;
      case FamilyValue::on_off:
        if (value != "on" && value != "off") {
          refused = family_refusal(family, EngineArgumentsErrorCode::invalid_value, flag->name, "must be on or off");
        }
        break;
      case FamilyValue::mm3_kv_cache:
        if (value != "bf16" && value != "int8" && value != "fp8" && value != "fp4") {
          refused = family_refusal(family, EngineArgumentsErrorCode::invalid_value, flag->name, "must be bf16, int8, fp8 or fp4");
        }
        break;
    }
    if (refused) return refused;
  }
  return std::nullopt;
}

// unified-engine design §3.3/§3.5: the merged engine's own serve flags, a closed allowlist, each
// at most once, in the `--flag VALUE` spelling. --native and --gpu are frontend-composed (owned),
// so the shared owned-flag loop in validate_engine_user_arguments refuses them first; the operator
// weight sources are refused by name here (they come from the manifest). Values: --max-context and
// --chunk are decimal integers in range; --expert-profile and --expert-profile-save take a path
// (any non-empty, non-flag token — NUL is already rejected by the shared loop).
enum class UnifiedValue { integer, path };
struct UnifiedFlag final {
  std::string_view name;
  UnifiedValue kind;
  std::int64_t minimum;
  std::int64_t maximum;
};
constexpr std::array<UnifiedFlag, 4> kUnifiedFlags{{{"--max-context", UnifiedValue::integer, 1, 1048576},
                                                   {"--chunk", UnifiedValue::integer, 1, 32768},
                                                   {"--expert-profile", UnifiedValue::path, 0, 0},
                                                   {"--expert-profile-save", UnifiedValue::path, 0, 0}}};
// The merged engine's weight sources come from the manifest, never the operator: refused by name.
constexpr std::array<std::string_view, 6> kUnifiedRefusedByName{
    "--pack", "--ple-gguf", "--embd-gguf", "--native-head-gguf", "--native-dense-gguf", "--mtp"};

EngineArgumentsError unified_refusal(const EngineArgumentsErrorCode code, const std::string_view flag,
                                     const std::string_view what) {
  return EngineArgumentsError{code, "engine.profile unified: " + std::string{flag} + " " + std::string{what}};
}

std::optional<EngineArgumentsError> validate_unified_arguments(const std::vector<std::string>& user_arguments) {
  std::vector<std::string_view> seen;
  for (std::size_t i = 0; i < user_arguments.size(); ++i) {
    const std::string_view argument = user_arguments[i];
    const auto name = flag_name(argument);
    const auto flag = std::find_if(kUnifiedFlags.begin(), kUnifiedFlags.end(),
                                   [name](const UnifiedFlag& entry) { return entry.name == name; });
    if (flag == kUnifiedFlags.end()) {
      const auto known = std::find(kUnifiedRefusedByName.begin(), kUnifiedRefusedByName.end(), name);
      if (known != kUnifiedRefusedByName.end()) {
        return unified_refusal(EngineArgumentsErrorCode::not_accepted, *known,
                               "is a weight source from the manifest, not an operator flag");
      }
      return EngineArgumentsError{EngineArgumentsErrorCode::not_accepted,
                                  "engine.profile unified: the engine argument is not one of the merged engine's flags"};
    }
    if (name.size() != argument.size()) {
      return unified_refusal(EngineArgumentsErrorCode::not_accepted, flag->name, "takes its value as the next argument");
    }
    if (std::find(seen.begin(), seen.end(), flag->name) != seen.end()) {
      return unified_refusal(EngineArgumentsErrorCode::invalid_value, flag->name, "is given more than once");
    }
    seen.push_back(flag->name);
    if (i + 1 >= user_arguments.size() || user_arguments[i + 1].rfind("--", 0) == 0) {
      return unified_refusal(EngineArgumentsErrorCode::invalid_value, flag->name, "needs a value");
    }
    const std::string_view value = user_arguments[++i];
    if (flag->kind == UnifiedValue::integer) {
      const std::string range = "must be " + std::to_string(flag->minimum) + "-" + std::to_string(flag->maximum);
      if (value.empty() || !all_digits(value) || (value.size() > 1 && value.front() == '0')) {
        return unified_refusal(EngineArgumentsErrorCode::invalid_value, flag->name, "takes a decimal integer");
      }
      std::int64_t parsed = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (error != std::errc{} || end != value.data() + value.size() || parsed < flag->minimum || parsed > flag->maximum) {
        return unified_refusal(EngineArgumentsErrorCode::out_of_range, flag->name, range);
      }
    }
    // path: any non-empty non-flag token (NUL already rejected); no further grammar in schema 1.
  }
  return std::nullopt;
}
}  // namespace

std::optional<EngineArgumentsError> validate_engine_user_arguments(const EngineArgumentsProfile profile,
                                                                   const std::vector<std::string>& user_arguments) {
  for (const auto& argument : user_arguments) {
    if (argument.find('\0') != std::string::npos) {
      return EngineArgumentsError{EngineArgumentsErrorCode::invalid_value, "engine argument contains NUL"};
    }
    const auto name = flag_name(argument);
    if (profile == EngineArgumentsProfile::lighttransport_glm53) {
      // SP5 M2 Task 7 (D-8, C-5): the pass-through is retired — the GLM engine's whole argv
      // is composed (build_engine_arguments), so no operator flag may reach it at all.
      static_cast<void>(name);
      return EngineArgumentsError{EngineArgumentsErrorCode::not_accepted,
                                  "engine.profile glm53 takes no engine arguments: the launch is composed from the "
                                  "model directory and the typed options"};
    }
    for (const auto owned : kOwnedFlags) {
      if (name == owned) {
        return EngineArgumentsError{EngineArgumentsErrorCode::owned_flag, "engine argument is owned by the frontend"};
      }
    }
    for (const auto mode : kUnsupportedModes) {
      if (name == mode) {
        return EngineArgumentsError{EngineArgumentsErrorCode::unsupported_mode,
                                    "engine mode is not supported by the frontend"};
      }
    }
  }
  if (profile == EngineArgumentsProfile::strata_gemma4) return validate_family_arguments(kGemma4Arguments, user_arguments);
  if (profile == EngineArgumentsProfile::strata_flint) return validate_family_arguments(kFlintArguments, user_arguments);
  if (profile == EngineArgumentsProfile::maya_ds4) return validate_family_arguments(kMayaDs4Arguments, user_arguments);
  if (profile == EngineArgumentsProfile::maya_mm3) return validate_family_arguments(kMayaMm3Arguments, user_arguments);
  if (profile == EngineArgumentsProfile::unified) return validate_unified_arguments(user_arguments);
  return std::nullopt;
}

namespace {

// SP5 M1 Task 6 (parity row mtp): the draft layer's own options — strata_control/
// draft_layer.py MTP_FLAGS verbatim: flag -> takes a value. `--spec` (prompt lookup)
// is NOT here: "why --spec stays and only the draft layer's own options go".
constexpr std::array<std::pair<std::string_view, bool>, 3> kMtpFlags{
    std::pair{"--mtp", true}, std::pair{"--spec-min-p", true}, std::pair{"--batch-mtp", false}};

// draft_layer.strip() verbatim: drop each flag, and consume the following token as its
// value only when it is not another flag.
void strip_mtp_flags(std::vector<std::string>& arguments, std::vector<std::string>* removed) {
  std::vector<std::string> kept;
  kept.reserve(arguments.size());
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const auto flag = std::find_if(kMtpFlags.begin(), kMtpFlags.end(),
                                   [&arguments, i](const auto& entry) { return arguments[i] == entry.first; });
    if (flag == kMtpFlags.end()) {
      kept.push_back(arguments[i]);
      continue;
    }
    if (removed != nullptr) removed->emplace_back(flag->first);
    if (flag->second && i + 1 < arguments.size() && arguments[i + 1].rfind("--", 0) != 0) ++i;  // the value goes too
  }
  arguments = std::move(kept);
}

}  // namespace

EngineArgumentsResult build_engine_arguments(const EngineArgumentsProfile profile, const ModelManifest& selected,
                                             const CanonicalRoot& models_root,
                                             const std::vector<std::string>& user_arguments,
                                             const std::vector<unsigned>& gpus, bool mtp_on,
                                             const Glm53LaunchArgs& glm) {
  static_cast<void>(glm);  // used by the lighttransport_glm53 composition below (Task 7)
  if (auto rejected = validate_engine_user_arguments(profile, user_arguments)) return *rejected;
  std::vector<std::string> arguments = user_arguments;
  if (profile == EngineArgumentsProfile::lighttransport_glm53) {
    // SP5 M2 Task 7 (D-8): Maya's launcher base (write_config, maya.py:2228-2230 at
    // v1.1.8; SP5-P2 Task 6 re-anchored it at 451f9cb6 — the base renders
    // "--expert-cache auto" after --max-context, maya.py:2228) composed from the
    // inventory model's directory (its pack — Maya keeps the tokenizer at
    // <pack>/tokenizer, :2231) plus the typed options. --prefill auto and --kv-streaming on
    // are every Maya config's fixed base; --parallel renders only for 2..8 (set_arg :2291,
    // choices :3885). --expert-profile: Maya's shipped one when the install has it (owner
    // 2026-10-27, launch_composition sets it). GPUs stay out of argv (Task 4 puts the typed glm.gpu list
    // in the child environment); the bounds are Maya's (max_context C-1 added, parallel :3885).
    if (!gpus.empty()) {
      return EngineArgumentsError{EngineArgumentsErrorCode::unsupported_mode,
                                  "GPU selection is not supported by this engine profile"};
    }
    const auto parsed = parse_relative_path(selected.relative_artifact_path);
    const auto* relative = std::get_if<RelativePath>(&parsed);
    if (relative == nullptr) {
      return EngineArgumentsError{EngineArgumentsErrorCode::invalid_value, "selected model has an invalid artifact path"};
    }
    if (glm.max_context < 1 || glm.max_context > 1048576) {
      return EngineArgumentsError{EngineArgumentsErrorCode::out_of_range, "glm.max_context: 1-1048576"};
    }
    if (glm.parallel < 1 || glm.parallel > 8) {
      return EngineArgumentsError{EngineArgumentsErrorCode::out_of_range, "replicas.parallel: 1-8 with the GLM engine"};
    }
    // glm.kv: the engine's --kv vocabulary (glm_driver.cpp), fp16 its own default
    if (glm.kv != "fp16" && glm.kv != "int8" && glm.kv != "q8_0" && glm.kv != "q4_0") {
      return EngineArgumentsError{EngineArgumentsErrorCode::invalid_value, "glm.kv: fp16, int8, q8_0 or q4_0"};
    }
    const auto pack = models_root.join(*relative).parent_path();
    std::vector<std::string> composed{"--glm-pack", path_to_utf8(pack), "--max-context", std::to_string(glm.max_context),
                                      "--expert-cache", "auto", "--prefill", "auto", "--kv-streaming", "on"};
    // Maya's shipped expert profile, as its setup adds it to every GLM config (maya.py v1.1.30 write_config), before
    // --parallel: its ranking of every (layer, expert) pair decides each layer's VRAM slots and the experts there first
    if (glm.expert_profile) {
      composed.emplace_back("--expert-profile");
      composed.push_back(*glm.expert_profile);
    }
    if (glm.kv != "fp16") {
      composed.emplace_back("--kv");
      composed.push_back(glm.kv);
    }
    if (glm.parallel > 1) {
      composed.emplace_back("--parallel");
      composed.push_back(std::to_string(glm.parallel));
    }
    return composed;
  }
  if (profile == EngineArgumentsProfile::maya_ds4 || profile == EngineArgumentsProfile::maya_mm3) {
    // Maya's write_config_ds4 (maya.py v1.1.30 :4136-4160) for a GGUF pack, which has no vision tower: --pack is the
    // inventory model's folder (the pack, ds4.txt beside the marker), then the card's allowlisted flags in order,
    // maya-ds4's own --gpus for named GPUs, and --vision off. write_config_mm3 (:4536-4634) for a text-only GGUF pack
    // adds --kv-streaming on unless the card sets it, and the shipped expert profile (--profile) when the install has it.
    const auto parsed = parse_relative_path(selected.relative_artifact_path);
    const auto* relative = std::get_if<RelativePath>(&parsed);
    if (relative == nullptr) {
      return EngineArgumentsError{EngineArgumentsErrorCode::invalid_value, "selected model has an invalid artifact path"};
    }
    std::vector<std::string> composed{"--pack", path_to_utf8(models_root.join(*relative).parent_path())};
    composed.insert(composed.end(), arguments.begin(), arguments.end());
    if (profile == EngineArgumentsProfile::maya_ds4) {
      // write_config_ds4: --tp min(2, GPUs) (two equal GPUs per tensor-parallel group) and, on one GPU, --threads on
      // (the chunked prompt path); a card that names either keeps its own
      const std::size_t cards = std::max<std::size_t>(1U, gpus.size());
      const auto names = [&arguments](const std::string_view flag) { return std::find(arguments.begin(), arguments.end(), flag) != arguments.end(); };
      if (!names("--tp")) composed.insert(composed.end(), {"--tp", std::to_string(std::min<std::size_t>(2U, cards))});
      if (cards == 1U && !names("--threads")) composed.insert(composed.end(), {"--threads", "on"});
    }
    if (profile == EngineArgumentsProfile::maya_mm3) {
      if (std::find(arguments.begin(), arguments.end(), "--kv-streaming") == arguments.end()) {
        composed.emplace_back("--kv-streaming");
        composed.emplace_back("on");
      }
      if (glm.expert_profile) {
        composed.emplace_back("--profile");
        composed.push_back(*glm.expert_profile);
      }
    }
    if (!gpus.empty()) {
      std::string list;
      for (const unsigned device : gpus) {
        if (!list.empty()) list += ',';
        list += std::to_string(device);
      }
      composed.emplace_back("--gpus");
      composed.push_back(std::move(list));
    }
    composed.emplace_back("--vision");
    composed.emplace_back("off");
    return composed;
  }
  const auto parsed = parse_relative_path(selected.relative_artifact_path);
  // SP5 M1 Task 6 (parity row mtp, runconfig.py:36 + draft_layer.is_off): an explicit
  // false strips the draft layer's own options at every start, before anything else is
  // composed. The GLM profile returns above: its runner does not speak these flags. The
  // Gemma engine has no draft layer (spec D5) and its allowlist refuses those flags, so
  // nothing is stripped for strata_gemma4 (Gemma 4 plan 1 Task 3).
  if (!mtp_on && profile == EngineArgumentsProfile::strata_0143) strip_mtp_flags(arguments, nullptr);
  const auto* relative = std::get_if<RelativePath>(&parsed);
  if (relative == nullptr) {
    return EngineArgumentsError{EngineArgumentsErrorCode::invalid_value, "selected model has an invalid artifact path"};
  }
  // The engine's own --gpu sets CUDA_DEVICE_ORDER/CUDA_VISIBLE_DEVICES (or
  // HIP) inside the engine (upstream generate.cpp:2357-2375 at v0.2.2), so
  // the strata family's environment carries no GPU variable.
  if (!gpus.empty()) {
    std::string list;
    for (const unsigned device : gpus) {
      if (!list.empty()) list += ',';
      list += std::to_string(device);
    }
    arguments.emplace_back("--gpu");
    arguments.push_back(std::move(list));
  }
  // The engine receives the canonical root the inventory used, never the raw
  // configured string (defect e).
  arguments.emplace_back("--native");
  // UTF-8, never path::string(): on Windows that narrows through the ANSI code
  // page, and the engine launcher decodes every argument as UTF-8.
  arguments.push_back(path_to_utf8(models_root.join(*relative)));
  return arguments;
}

// SP5 M1 Task 6: the flags an mtp=false composition drops, for the pin's fixed startup
// line (serve/server.py:2969). The frontend only ever passes user arguments through, so
// the removals can only come from them — the same view build_engine_arguments applies.
std::vector<std::string> strip_draft_layer_arguments(std::vector<std::string>& arguments) {
  std::vector<std::string> removed;
  strip_mtp_flags(arguments, &removed);
  return removed;
}

std::vector<std::string> draft_layer_removed(const std::vector<std::string>& arguments) {
  std::vector<std::string> copy = arguments;
  return strip_draft_layer_arguments(copy);
}

namespace {

bool owns_flag(const std::vector<std::string>& user_arguments, const std::string_view flag) {
  return std::find(user_arguments.begin(), user_arguments.end(), flag) != user_arguments.end();
}

}  // namespace

// SP5 M1 Task 7. Each rendering mirrors the pin branch exactly: the reserve flag unless
// the operator put one in the args; the elastic branch nests the segment size (and is
// skipped whole when the operator owns --vram-elastic, :2989-2992); the layer-split
// pair is gated on several GPUs without --peer-device (layer_split_of, :2940-2943) and
// skips flags the operator already spelled (:2982, :2986).
void append_vram_gpu_elastic_arguments(std::vector<std::string>& arguments, const VramGpuElasticArgs& vram,
                                       const std::vector<std::string>& user_arguments, const std::vector<unsigned>& gpus) {
  if (vram.vram_reserve_mib.has_value() && !owns_flag(user_arguments, "--vram-reserve-mib")) {
    arguments.emplace_back("--vram-reserve-mib");
    arguments.push_back(std::to_string(*vram.vram_reserve_mib));
  }
  if (vram.vram_elastic && !owns_flag(user_arguments, "--vram-elastic")) {
    arguments.emplace_back("--vram-elastic");
    if (vram.vram_segment_mib.has_value() && *vram.vram_segment_mib > 0 && !owns_flag(user_arguments, "--vram-segment-mib")) {
      arguments.emplace_back("--vram-segment-mib");
      arguments.push_back(std::to_string(*vram.vram_segment_mib));
    }
  }
  const bool split_gate = gpus.size() > 1 && !owns_flag(user_arguments, "--peer-device");
  if (split_gate && !owns_flag(user_arguments, "--layer-split")) {
    arguments.emplace_back("--layer-split");
    arguments.push_back(vram.layer_split.empty() ? "auto" : vram.layer_split);
  }
  if (vram.split_skip_if_fits && split_gate && !owns_flag(user_arguments, "--split-skip-if-fits")) {
    arguments.emplace_back("--split-skip-if-fits");
  }
}

// SP5 M1 Task 9 (serve/server.py:3071-3076): the pin's guard is literal — a blank path or
// an operator-owned --expert-profile-save returns the args unchanged, before EITHER flag;
// so the every rides the save, never stands alone.
void append_expert_profile_save_arguments(std::vector<std::string>& arguments, const ExpertProfileSaveArgs& save,
                                          const std::vector<std::string>& user_arguments) {
  if (save.path.empty() || save.path.find_first_not_of(" \t") == std::string::npos ||
      owns_flag(user_arguments, "--expert-profile-save")) {
    return;
  }
  arguments.emplace_back("--expert-profile-save");
  arguments.push_back(save.path);
  arguments.emplace_back("--expert-profile-save-every");
  arguments.push_back(std::to_string(save.every));
}

// SP5 M1 Task 10 (serve/server.py:3000-3020): the guards are the pin's, in order — operator
// --batch/--slots first (:3006), then the <=1 no-op (:3013), then the value rides as
// --batch N even past the 8-row window (:3014-3020).
void append_parallel_batch_arguments(std::vector<std::string>& arguments, const ParallelBatchArgs& parallel,
                                     const std::vector<std::string>& user_arguments) {
  if (owns_flag(user_arguments, "--batch") || owns_flag(user_arguments, "--slots")) return;
  if (parallel.parallel <= 1) return;
  arguments.emplace_back("--batch");
  arguments.push_back(std::to_string(parallel.parallel));
}

std::vector<unsigned> effective_gpu_list(const std::vector<unsigned>& gpus, std::optional<std::int64_t> hip_ordinal) {
  // hip_visible (:3096): the ordinal wins only while at most one card is configured.
  if (hip_ordinal.has_value() && gpus.size() <= 1U) return {static_cast<unsigned>(*hip_ordinal)};
  return gpus;
}

EffortEndArguments compose_effort_end_arguments(const std::filesystem::path& engine_binary, const TokenizerPack& tokenizer) {
  // The pin greps the engine binary for the flag literal (:2893) and encodes "system" expecting a
  // single token (:2894); either failure means the pin's one fixed fallback line and no argument.
  constexpr std::string_view kFlag = "--tail-role-token";
  constexpr std::string_view kFallback =
      "effort_position \"end\" needs engine 0.1.39 or newer (--tail-role-token): the reasoning effort stays at the top of the prompt";
  constexpr std::string_view kSuccess =
      "effort_position end: a request's non-default reasoning effort goes right before the answer, so changing it keeps the cached conversation";
  EffortEndArguments result;
  bool known = false;
  std::ifstream in(engine_binary, std::ios::binary);
  if (in) {
    std::string window;
    std::vector<char> block(64U * 1024U);
    while (!known && in) {
      in.read(block.data(), static_cast<std::streamsize>(block.size()));
      const std::streamsize got = in.gcount();
      if (got <= 0) break;
      window.append(block.data(), static_cast<std::size_t>(got));
      if (window.find(kFlag) != std::string::npos) {
        known = true;
        break;
      }
      // Keep the overlap so a flag literal split across block boundaries is still found.
      if (window.size() >= kFlag.size()) window.erase(0, window.size() - (kFlag.size() - 1U));
    }
  }
  const auto encoded = encode_text(tokenizer, "system");
  const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
  if (!known || ids == nullptr || ids->size() != 1U) {
    result.notice = kFallback;
    return result;
  }
  result.arguments = {std::string(kFlag), std::to_string((*ids)[0])};
  result.notice = kSuccess;
  return result;
}

std::optional<std::vector<unsigned>> parse_gpu_list(const std::string_view value) {
  if (value.empty() || value.size() > 64U) return std::nullopt;
  std::vector<unsigned> devices;
  std::size_t begin = 0;
  for (;;) {
    const auto comma = value.find(',', begin);
    const auto item = value.substr(begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin);
    if (item.empty() || item.size() > 3U || (item.size() > 1U && item.front() == '0')) return std::nullopt;
    unsigned device = 0;
    const auto [end, error] = std::from_chars(item.data(), item.data() + item.size(), device);
    if (error != std::errc{} || end != item.data() + item.size() || device > 255U) return std::nullopt;
    if (std::find(devices.begin(), devices.end(), device) != devices.end() || devices.size() == 16U) return std::nullopt;
    devices.push_back(device);
    if (comma == std::string_view::npos) return devices;
    begin = comma + 1U;
  }
}

}  // namespace secure_frontend
