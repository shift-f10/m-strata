// The engine families' descriptors (plugin spec 2026-10-30 section 5, phase 2): one per EngineProfile, in its order,
// each holding what the server's switches on the profile decided - pinned here value by value, so moving a switch onto
// the table changes nothing.
#include "secure_frontend/family_descriptor.hpp"
#include "support/test_support.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

using secure_frontend::DraftLayerKind;
using secure_frontend::EngineArgumentsProfile;
using secure_frontend::EngineFamily;
using secure_frontend::EngineProfile;
using secure_frontend::EngineProtocolFlavor;
using secure_frontend::family_descriptor;
using secure_frontend::family_descriptors;
using secure_frontend::family_named;
using secure_frontend::MtpOff;
using secure_frontend::NativeChatProfile;
using secure_frontend::ThinkingWrite;
using secure_frontend::TokenizerMode;
using secure_frontend::TokenizerSource;

constexpr std::array<EngineProfile, 7> kProfiles{EngineProfile::qwen,  EngineProfile::glm53, EngineProfile::gemma4, EngineProfile::deepseek,
                                                 EngineProfile::raon1, EngineProfile::flint, EngineProfile::minimax};
constexpr std::array<std::string_view, 7> kIds{"qwen", "glm53", "gemma4", "deepseek", "raon1", "flint", "minimax"};

std::string id_of(const EngineProfile profile) { return std::string{family_descriptor(profile).id}; }

// One per profile, in its order, found by its persisted name exactly.
void every_family_has_one_descriptor_in_order() {
  const auto all = family_descriptors();
  expect(all.size() == kProfiles.size(), "one descriptor a profile: " + std::to_string(all.size()));
  for (std::size_t i = 0; i < kProfiles.size() && i < all.size(); ++i) {
    expect(all[i].profile == kProfiles[i] && all[i].id == kIds[i], "in EngineProfile's order: " + std::string{kIds[i]});
    expect(&family_descriptor(kProfiles[i]) == &all[i], "family_descriptor: " + std::string{kIds[i]});
    expect(family_named(kIds[i]) == &all[i], "family_named: " + std::string{kIds[i]});
  }
  for (const std::string_view other : {"", "Qwen", "qwen ", " glm53", "glm", "gemma", "unified", "strata"}) {
    expect(family_named(other) == nullptr, "no family is named '" + std::string{other} + "'");
  }
  expect(secure_frontend::family_choices() == "qwen, glm53, gemma4, deepseek, raon1, flint or minimax",
         "the choices, as engine.profile's error lists them: " + secure_frontend::family_choices());
}

// The engine each talks to: its renderer, wire dialect, environment, arguments and tokenizer.
void each_family_talks_to_its_engine() {
  struct Row final {
    EngineProfile profile;
    NativeChatProfile chat;
    EngineProtocolFlavor flavor;
    EngineFamily environment;
    EngineArgumentsProfile arguments;
    TokenizerMode tokenizer;
    TokenizerSource source;
    bool per_request_effort;
  };
  constexpr std::array<Row, 7> kRows{{
      {EngineProfile::qwen, NativeChatProfile::qwen, EngineProtocolFlavor::strata_0143, EngineFamily::strata,
       EngineArgumentsProfile::strata_0143, TokenizerMode::byte_bpe, TokenizerSource::gguf_for_cards, true},
      {EngineProfile::glm53, NativeChatProfile::glm53, EngineProtocolFlavor::maya_118, EngineFamily::glm,
       EngineArgumentsProfile::lighttransport_glm53, TokenizerMode::glm_bpe, TokenizerSource::pack, false},
      {EngineProfile::gemma4, NativeChatProfile::gemma4, EngineProtocolFlavor::strata_0143, EngineFamily::strata,
       EngineArgumentsProfile::strata_gemma4, TokenizerMode::spm_bpe, TokenizerSource::gguf, true},
      {EngineProfile::deepseek, NativeChatProfile::deepseek, EngineProtocolFlavor::maya_118, EngineFamily::strata,
       EngineArgumentsProfile::maya_ds4, TokenizerMode::deepseek_bpe, TokenizerSource::pack, true},
      {EngineProfile::raon1, NativeChatProfile::raon1, EngineProtocolFlavor::maya_118, EngineFamily::glm,
       EngineArgumentsProfile::lighttransport_glm53, TokenizerMode::raon_bpe, TokenizerSource::pack, true},
      {EngineProfile::flint, NativeChatProfile::flint, EngineProtocolFlavor::strata_0143, EngineFamily::strata,
       EngineArgumentsProfile::strata_flint, TokenizerMode::flint_bpe, TokenizerSource::gguf, true},
      {EngineProfile::minimax, NativeChatProfile::minimax, EngineProtocolFlavor::maya_118, EngineFamily::strata,
       EngineArgumentsProfile::maya_mm3, TokenizerMode::minimax_bpe, TokenizerSource::pack, true},
  }};
  for (const auto& row : kRows) {
    const auto& family = family_descriptor(row.profile);
    const std::string id = id_of(row.profile);
    expect(family.chat_profile == row.chat, id + ": its renderer");
    expect(family.protocol_flavor == row.flavor, id + ": its wire dialect");
    expect(family.environment_family == row.environment, id + ": its environment family");
    expect(family.arguments_profile == row.arguments, id + ": its arguments");
    expect(family.tokenizer_mode == row.tokenizer, id + ": its tokenizer mode");
    expect(family.tokenizer_source == row.source, id + ": where its tokenizer comes from");
    expect(family.per_request_reasoning_effort == row.per_request_effort, id + ": reasoning_effort per request");
  }
}

// The config rows each takes: the glm_pack engine's (glm53, raon1); qwen's launch rows refused, in its own words, on
// gemma4 and flint.
void each_family_takes_its_config_rows() {
  for (const auto profile : kProfiles) {
    const auto& family = family_descriptor(profile);
    const bool glm_pack = profile == EngineProfile::glm53 || profile == EngineProfile::raon1;
    expect(family.glm_pack_rows == glm_pack, id_of(profile) + ": the glm_pack engine's rows");
    const bool refuses = profile == EngineProfile::gemma4 || profile == EngineProfile::flint;
    expect(family.refuses_qwen_launch_rows == refuses, id_of(profile) + ": qwen's launch rows refused");
    if (!refuses) expect(family.engine_words.empty() && family.template_words.empty(), id_of(profile) + ": no words of its own");
  }
  expect(family_descriptor(EngineProfile::gemma4).engine_words == "the Gemma engine" &&
             family_descriptor(EngineProfile::gemma4).template_words == "Gemma's template",
         "gemma4's words");
  expect(family_descriptor(EngineProfile::flint).engine_words == "the flint engine" &&
             family_descriptor(EngineProfile::flint).template_words == "flint's template",
         "flint's words");
}

// How each launches: qwen's tails, pictures, effort tail, conversation cache and download template; Maya's shipped
// expert profiles (glm53, minimax); gemma4's checked --mtp file.
void each_family_launches_as_before() {
  for (const auto profile : kProfiles) {
    const auto& family = family_descriptor(profile);
    const std::string id = id_of(profile);
    const bool qwen = profile == EngineProfile::qwen;
    expect(family.composes_strata_tails == qwen, id + ": strata's tails");
    expect(family.reads_images == qwen, id + ": pictures");
    expect(family.effort_end_tail == qwen, id + ": the effort-end tail");
    expect(family.reads_conversation_cache == qwen, id + ": the conversation cache");
    expect(family.download_template_card == qwen, id + ": a download's card template");
    expect(family.checks_mtp_side_file == (profile == EngineProfile::gemma4), id + ": the --mtp side-file check");
    // (no optional built by a ternary: GCC's optimiser takes its copy for maybe uninitialized)
    const char* const shipped = profile == EngineProfile::glm53     ? "expert-profile-glm.bin"
                                : profile == EngineProfile::minimax ? "expert-profile-mm3.bin"
                                                                    : nullptr;
    expect(shipped == nullptr ? !family.shipped_expert_profile.has_value()
                              : family.shipped_expert_profile.has_value() && *family.shipped_expert_profile == shipped,
           id + ": Maya's shipped expert profile");
  }
}

// How each calibrates: the keep margin (Maya's two 5%, the rest 3%), a calibration before the first start (gemma4,
// flint), the prefill calibration (minimax).
void each_family_calibrates_as_before() {
  for (const auto profile : kProfiles) {
    const auto& family = family_descriptor(profile);
    const std::string id = id_of(profile);
    const bool maya_moe = profile == EngineProfile::deepseek || profile == EngineProfile::minimax;
    expect(family.sweep_keep_margin == (maya_moe ? 0.05 : 0.03), id + ": its keep margin");
    expect(family.calibrates_before_first_start == (profile == EngineProfile::gemma4 || profile == EngineProfile::flint),
           id + ": calibrates before its first start");
    expect(family.prefill_calibration == (profile == EngineProfile::minimax), id + ": the prefill calibration");
  }
}
// Each family's card options: how many, the first and last in the page's order, and the flags they own.
void each_family_lists_its_options() {
  struct Row final {
    EngineProfile profile;
    std::size_t count;
    std::string_view first;
    std::string_view last;
    std::vector<std::string_view> flags;
  };
  const std::array<Row, 7> rows{{
      {EngineProfile::qwen, 20U, "max_context", "gpus",
       {"--max-context", "--rope-scaling", "--rope-scale", "--kv", "--kv-resident", "--kv-grow", "--spec", "--spec-min-p", "--expert-cache", "--prefill"}},
      {EngineProfile::glm53, 11U, "max_context", "gpus", {}},
      {EngineProfile::gemma4, 15U, "max_context", "gpus", {"--max-context", "--chunk", "--expert-vram", "--spec", "--spec-min-p"}},
      {EngineProfile::deepseek, 10U, "max_context", "gpus", {"--max-context"}},
      {EngineProfile::raon1, 11U, "max_context", "gpus", {}},
      {EngineProfile::flint, 12U, "max_context", "gpus", {"--max-context", "--chunk", "--expert-vram"}},
      {EngineProfile::minimax, 9U, "max_context", "gpus", {"--max-context", "--kv-cache"}},
  }};
  for (const auto& row : rows) {
    const auto& family = family_descriptor(row.profile);
    const std::string id = id_of(row.profile);
    expect(family.options.size() == row.count, id + ": its options, " + std::to_string(family.options.size()));
    if (!family.options.empty()) {
      expect(family.options.front().name == row.first && family.options.back().name == row.last,
             id + ": first and last " + std::string{family.options.front().name} + ", " + std::string{family.options.back().name});
    }
    expect(std::vector<std::string_view>(family.option_controlled_flags.begin(), family.option_controlled_flags.end()) == row.flags,
           id + ": the flags its options own");
  }
}

// How each family's options are written, and its recommended sampling.
void each_family_writes_its_options() {
  for (const auto profile : kProfiles) {
    const auto& family = family_descriptor(profile);
    const std::string id = id_of(profile);
    expect(family.kv_flag == (profile == EngineProfile::minimax ? "--kv-cache" : "--kv"), id + ": its kv flag");
    expect(family.yarn_trained_context == (profile == EngineProfile::qwen ? std::optional<std::int64_t>{262144} : std::nullopt),
           id + ": YaRN past its trained context");
    const auto thinking = profile == EngineProfile::gemma4 || profile == EngineProfile::flint ? ThinkingWrite::on_off
                          : profile == EngineProfile::minimax                                 ? ThinkingWrite::verbatim_auto_unset
                                                                                              : ThinkingWrite::verbatim;
    expect(family.thinking_write == thinking, id + ": how thinking is written");
    expect(family.mtp_off == (profile == EngineProfile::gemma4 ? MtpOff::drop_card_flag : MtpOff::engine_row), id + ": mtp off");
    expect(family.spec_min_p_places == (profile == EngineProfile::gemma4 ? std::optional<int>{3} : std::nullopt), id + ": spec_min_p's places");
    const auto& sampling = family.sampling;
    const bool top_k_20 = profile == EngineProfile::qwen || profile == EngineProfile::flint || profile == EngineProfile::raon1;
    expect(sampling.temperature == 1.0 && sampling.top_p == (profile == EngineProfile::deepseek ? 1.0 : 0.95), id + ": temperature, top-p");
    expect(sampling.top_k == (top_k_20 ? std::optional<std::int64_t>{20}
                              : profile == EngineProfile::gemma4 ? std::optional<std::int64_t>{64}
                                                                 : std::nullopt),
           id + ": top-k");
    expect(sampling.min_p == (profile == EngineProfile::gemma4 ? std::optional<double>{0.05} : std::nullopt), id + ": min-p");
  }
}

// Each family's cards: the draft layer and what adding one brings, and whether the model file is found through a pack.
void each_family_makes_its_cards() {
  for (const auto profile : kProfiles) {
    const auto& family = family_descriptor(profile);
    const std::string id = id_of(profile);
    const auto draft = profile == EngineProfile::qwen     ? DraftLayerKind::in_model
                       : profile == EngineProfile::gemma4 ? DraftLayerKind::file
                                                          : DraftLayerKind::none;
    expect(family.draft_layer == draft, id + ": its draft layer");
    expect(family.draft_spec == (profile == EngineProfile::gemma4 ? std::optional<std::int64_t>{4} : std::nullopt), id + ": a draft's spec");
    expect(family.draft_spec_min_p == (profile == EngineProfile::gemma4 ? 0.6 : 0.5), id + ": a draft's spec_min_p");
    const bool maya_pack = profile == EngineProfile::glm53 || profile == EngineProfile::raon1 || profile == EngineProfile::deepseek ||
                           profile == EngineProfile::minimax;
    expect(family.maya_pack_model_file == maya_pack, id + ": its model file through the pack");
  }
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"every_family_has_one_descriptor_in_order", every_family_has_one_descriptor_in_order},
      {"each_family_talks_to_its_engine", each_family_talks_to_its_engine},
      {"each_family_takes_its_config_rows", each_family_takes_its_config_rows},
      {"each_family_launches_as_before", each_family_launches_as_before},
      {"each_family_calibrates_as_before", each_family_calibrates_as_before},
      {"each_family_lists_its_options", each_family_lists_its_options},
      {"each_family_writes_its_options", each_family_writes_its_options},
      {"each_family_makes_its_cards", each_family_makes_its_cards},
  }, failures);
}
