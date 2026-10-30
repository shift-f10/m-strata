#pragma once

// The engine families (plugin spec 2026-10-30 section 5, phase 2): one FamilyDescriptor per family, holding what the
// server's switches on EngineProfile decided - its persisted name, its engine dialect and arguments, its tokenizer, the
// config rows it takes, how it launches and calibrates. The built-in families get theirs from this table, in
// EngineProfile's order; their code (renderers, parsers, argv composers, sweep plans) stays as it is, chosen by family.
// A plugin's descriptor will come from its manifest (phase 2's next step).

#include "secure_frontend/engine_family_kinds.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace secure_frontend {

// Where a launch's tokenizer comes from: the tokenizer pack; the GGUF itself; the GGUF for a card's launch, the pack
// otherwise (qwen: the operator's single-model --expert keeps its pack).
enum class TokenizerSource { pack, gguf, gguf_for_cards };

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
};

// Every family, in EngineProfile's order.
[[nodiscard]] std::span<const FamilyDescriptor> family_descriptors() noexcept;
[[nodiscard]] const FamilyDescriptor& family_descriptor(EngineProfile profile) noexcept;
// The family a persisted name names (nullptr: none), by its id exactly.
[[nodiscard]] const FamilyDescriptor* family_named(std::string_view id) noexcept;
// Every family's name, as an error message lists them: "qwen, glm53, ... or minimax".
[[nodiscard]] std::string family_choices();

}  // namespace secure_frontend
