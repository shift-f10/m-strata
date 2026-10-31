#pragma once

// The engine families (plugin spec 2026-10-30 section 5, phase 2): one FamilyDescriptor per family, holding what the
// server's switches on EngineProfile decided - its persisted name, its engine dialect and arguments, its tokenizer, the
// config rows it takes, how it launches and calibrates. The built-in families get theirs from this table, in
// EngineProfile's order; their code (renderers, parsers, argv composers, sweep plans) stays as it is, chosen by family.
// An engine plugin's comes from its manifest (plugin_family.hpp), with a profile past the built-ins'.

#include "secure_frontend/engine_family_kinds.hpp"
#include "secure_frontend/model_option_spec.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

struct PluginEngine;  // plugin_manifest.hpp: an engine plugin's manifest block

// Where a launch's tokenizer comes from: the tokenizer pack; the GGUF itself; the GGUF for a card's launch, the pack
// otherwise (qwen: the operator's single-model --expert keeps its pack).
enum class TokenizerSource { pack, gguf, gguf_for_cards };

// How the card's thinking option becomes reasoning.effort: its value as it is; on/off as high/none (gemma4, flint);
// its value as it is, but "auto" names no level (minimax: MiniMax's adaptive default).
enum class ThinkingWrite { verbatim, on_off, verbatim_auto_unset };

// What the card's mtp option off does: engine.mtp false, so the launch strips the draft-layer flags (qwen); the card's
// --mtp <file> taken out of the start's arguments (gemma4: its engine has no engine.mtp row).
enum class MtpOff { engine_row, drop_card_flag };

// Where a card's draft layer (MTP) is: none; in the model - inside its GGUF (no --pack), or its pack's folder (qwen); a
// file of its own (gemma4's drafter).
enum class DraftLayerKind { none, in_model, file };

// A family's recommended sampling (model_options.hpp, recommended_sampling: upstream's MODEL_CARD_SAMPLING and the cards).
struct RecommendedSampling final {
  double temperature{1.0};
  double top_p{0.95};
  std::optional<std::int64_t> top_k{};
  std::optional<double> min_p{};
};

struct FamilyDescriptor final {
  EngineProfile profile{EngineProfile::qwen};
  // the persisted name: engine.profile and --profile, a card's "profile", the routes' "profile", the catalog's "engine"
  std::string_view id;
  // the engine it talks to
  NativeChatProfile chat_profile{NativeChatProfile::qwen};      // the prompt renderer and reply parser
  EngineProtocolFlavor protocol_flavor{EngineProtocolFlavor::strata_0143};
  EngineFamily environment_family{EngineFamily::strata};        // the child environment's rows
  EngineArgumentsProfile arguments_profile{EngineArgumentsProfile::strata_0143};
  TokenizerMode tokenizer_mode{TokenizerMode::byte_bpe};
  TokenizerSource tokenizer_source{TokenizerSource::pack};
  bool per_request_reasoning_effort{true};  // the chat schema takes reasoning_effort per request
  // the config rows it takes
  bool glm_pack_rows{false};  // the glm_pack engine's: glm.* rows, glm.gpu (never engine.gpu), the glm idle window
  // qwen's launch rows (and reasoning.effort_position) refused, said in its own words ("the Gemma engine" / "Gemma's
  // template"); a family that does not refuse them composes no qwen tails, so they do nothing there
  bool refuses_qwen_launch_rows{false};
  std::string_view engine_words;
  std::string_view template_words;
  // how it launches
  bool composes_strata_tails{false};     // the VRAM/elastic, expert-profile-save and --batch tails, and their mtp removal
  bool reads_images{false};              // vision.enabled and a card's picture encoder
  bool effort_end_tail{false};           // reasoning.effort_position end adds --tail-role-token
  bool reads_conversation_cache{false};  // a conversation's cached prefix is read back
  bool download_template_card{false};    // a download's card may be made from a card of this family
  std::optional<std::string_view> shipped_expert_profile;  // Maya's <engine>/profiles/<file>, passed when it is there
  bool checks_mtp_side_file{false};      // a card's --mtp <file> must pass the trusted side-file check
  // how it calibrates
  double sweep_keep_margin{0.03};         // a candidate is kept only this much (a fraction) faster than the best
  bool calibrates_before_first_start{false};
  bool prefill_calibration{false};        // a prefill run at the first start, kept in the card's calibration
  // its card's options, in the page's order, the engine flags they own (a card's engine.args may not fix one), and how
  // they are written
  std::span<const ModelOptionSpec> options{};
  std::span<const std::string_view> option_controlled_flags{};
  RecommendedSampling sampling{};
  std::string_view kv_flag{"--kv"};
  std::optional<std::int64_t> yarn_trained_context{};  // past it, max_context adds YaRN (--rope-scaling yarn --rope-scale)
  ThinkingWrite thinking_write{ThinkingWrite::verbatim};
  MtpOff mtp_off{MtpOff::engine_row};
  std::optional<int> spec_min_p_places{};  // spec_min_p rounded to this many places (its engine's allowlist takes no more)
  // its cards
  DraftLayerKind draft_layer{DraftLayerKind::none};
  std::optional<std::int64_t> draft_spec{};  // a draft layer added to a card brings these defaults with it
  double draft_spec_min_p{0.5};
  bool maya_pack_model_file{false};  // its model file is found through the pack beside it (native_experts.txt, sources.json)
  // an engine plugin's (nullptr, empty: a built-in family): its manifest's block - the only flags its engine takes, each
  // option's flag, its calibration, its Monitor keys, its models - and its library's SHA-256 (the launcher's
  // --family-sha256)
  const PluginEngine* plugin{nullptr};
  std::string_view library_sha256{};
  MonitorTiers monitor_tiers{};  // the keys its Monitor tiers are read from (empty: the built-ins', told apart by their keys)
};

// The built-in families, in EngineProfile's order.
[[nodiscard]] std::span<const FamilyDescriptor> family_descriptors() noexcept;
// A family by its profile: a built-in's, or a plugin's that family_named handed out.
[[nodiscard]] const FamilyDescriptor& family_descriptor(EngineProfile profile) noexcept;
// The family a persisted name names (nullptr: none), by its id exactly: a built-in, then an engine plugin's.
[[nodiscard]] const FamilyDescriptor* family_named(std::string_view id) noexcept;
// Every family's name, as an error message lists them: "qwen, glm53, ... or minimax" (the plugins' after them).
[[nodiscard]] std::string family_choices();
// Every family: the built-ins, then the engine plugins'.
[[nodiscard]] std::vector<const FamilyDescriptor*> all_families();

// The options the server gives a card whatever its engine (temperature, top_p, top_k, min_p, answer_minutes,
// web_search), as the built-in tables hold them (nullptr: not one); an engine plugin's table ends with them.
[[nodiscard]] const ModelOptionSpec* server_card_option(std::string_view name) noexcept;

}  // namespace secure_frontend
