// The engine families (plugin spec 2026-10-30 section 5, phase 2): what the server's switches on EngineProfile decided,
// one record a family. family_descriptor_test pins every value.
#include "secure_frontend/family_descriptor.hpp"

#include <array>
#include <cstddef>

namespace secure_frontend {
namespace {

// Each family from the defaults (FamilyDescriptor's), setting what differs.
constexpr FamilyDescriptor qwen() {
  FamilyDescriptor family{};
  family.profile = EngineProfile::qwen;
  family.id = "qwen";
  family.chat_profile = NativeChatProfile::qwen;
  family.protocol_flavor = EngineProtocolFlavor::strata_0143;
  family.environment_family = EngineFamily::strata;
  family.arguments_profile = EngineArgumentsProfile::strata_0143;
  family.tokenizer_mode = TokenizerMode::byte_bpe;
  family.tokenizer_source = TokenizerSource::gguf_for_cards;  // a managed launch reads its pack
  family.composes_strata_tails = true;
  family.reads_images = true;
  family.effort_end_tail = true;
  family.reads_conversation_cache = true;
  family.download_template_card = true;
  return family;
}

constexpr FamilyDescriptor glm53() {
  FamilyDescriptor family{};
  family.profile = EngineProfile::glm53;
  family.id = "glm53";
  family.chat_profile = NativeChatProfile::glm53;
  family.protocol_flavor = EngineProtocolFlavor::maya_118;
  family.environment_family = EngineFamily::glm;
  family.arguments_profile = EngineArgumentsProfile::lighttransport_glm53;
  family.tokenizer_mode = TokenizerMode::glm_bpe;  // byte_bpe's files, Maya's glm4 split and ignore_merges (byte_bpe's qwen35 split is not GLM's)
  family.per_request_reasoning_effort = false;  // glm53 refuses the field: GLM-5.3's template has no effort levels
  family.glm_pack_rows = true;
  family.shipped_expert_profile = "expert-profile-glm.bin";
  return family;
}

constexpr FamilyDescriptor gemma4() {
  FamilyDescriptor family{};
  family.profile = EngineProfile::gemma4;
  family.id = "gemma4";
  family.chat_profile = NativeChatProfile::gemma4;
  family.protocol_flavor = EngineProtocolFlavor::strata_0143;  // strata-gemma4-engine speaks upstream's strata_0143 serve protocol (Gemma 4 spec 4.7)
  family.environment_family = EngineFamily::strata;  // the strata family's closed allowlist (Gemma 4 spec 6.1, 8)
  family.arguments_profile = EngineArgumentsProfile::strata_gemma4;
  family.tokenizer_mode = TokenizerMode::spm_bpe;
  family.tokenizer_source = TokenizerSource::gguf;
  family.per_request_reasoning_effort = true;  // none is thinking off, low/medium/high thinking on (Gemma 4 spec 6.6)
  family.refuses_qwen_launch_rows = true;
  family.engine_words = "the Gemma engine";
  family.template_words = "Gemma's template";
  family.checks_mtp_side_file = true;
  family.calibrates_before_first_start = true;
  return family;
}

constexpr FamilyDescriptor deepseek() {
  FamilyDescriptor family{};
  family.profile = EngineProfile::deepseek;
  family.id = "deepseek";
  family.chat_profile = NativeChatProfile::deepseek;
  family.protocol_flavor = EngineProtocolFlavor::maya_118;  // Maya's maya-ds4: GLM's maya_118 dialect with READY <ctx> stop
  family.environment_family = EngineFamily::strata;  // it takes its GPUs on the command line (--gpus)
  family.arguments_profile = EngineArgumentsProfile::maya_ds4;
  family.tokenizer_mode = TokenizerMode::deepseek_bpe;  // byte_bpe's files, Maya's deepseek3 splits (strata_tokenizer.py)
  family.per_request_reasoning_effort = true;  // as DeepSeek's encoder: none is chat mode, a level the effort line
  family.sweep_keep_margin = 0.05;
  return family;
}

constexpr FamilyDescriptor raon1() {
  FamilyDescriptor family{};
  family.profile = EngineProfile::raon1;
  family.id = "raon1";
  family.chat_profile = NativeChatProfile::raon1;
  family.protocol_flavor = EngineProtocolFlavor::maya_118;  // it rides the glm_pack engine (strata_glm, arch-detected): glm53's serve protocol
  family.environment_family = EngineFamily::glm;  // the glm env-variable mechanism, like glm53
  family.arguments_profile = EngineArgumentsProfile::lighttransport_glm53;
  family.per_request_reasoning_effort = true;  // its <think> block: none closes it (a direct answer), a level opens it
  family.tokenizer_mode = TokenizerMode::raon_bpe;  // byte_bpe's files, raon-1-mini's NFC + pre-tokenizer (byte_bpe's split gave ~46% more tokens)
  family.glm_pack_rows = true;
  return family;
}

constexpr FamilyDescriptor flint() {
  FamilyDescriptor family{};
  family.profile = EngineProfile::flint;
  family.id = "flint";
  family.chat_profile = NativeChatProfile::flint;
  family.protocol_flavor = EngineProtocolFlavor::strata_0143;  // multiengine_flint: Gemma 4's serve loop
  family.environment_family = EngineFamily::strata;  // argv --gpu, like gemma4
  family.arguments_profile = EngineArgumentsProfile::strata_flint;
  family.tokenizer_mode = TokenizerMode::flint_bpe;  // byte_bpe's files, llama.cpp's flint pre-tokenizer (three regexes over its collapsed text)
  family.tokenizer_source = TokenizerSource::gguf;
  family.per_request_reasoning_effort = true;  // none closes the think block, a level leaves it to the model
  family.refuses_qwen_launch_rows = true;
  family.engine_words = "the flint engine";
  family.template_words = "flint's template";
  family.calibrates_before_first_start = true;
  return family;
}

constexpr FamilyDescriptor minimax() {
  FamilyDescriptor family{};
  family.profile = EngineProfile::minimax;
  family.id = "minimax";
  family.chat_profile = NativeChatProfile::minimax;
  family.protocol_flavor = EngineProtocolFlavor::maya_118;  // Maya's maya-mm3: maya-ds4's wire
  family.environment_family = EngineFamily::strata;  // maya-mm3's --gpus, like maya-ds4
  family.arguments_profile = EngineArgumentsProfile::maya_mm3;
  family.tokenizer_mode = TokenizerMode::minimax_bpe;  // byte_bpe's files, NFC and Maya's minimax split (strata_tokenizer.py MINIMAX_PATTERN)
  family.per_request_reasoning_effort = true;  // none is thinking off, a level MiniMax's <effort>
  family.shipped_expert_profile = "expert-profile-mm3.bin";
  family.sweep_keep_margin = 0.05;
  family.prefill_calibration = true;
  return family;
}

constexpr std::array<FamilyDescriptor, 7> kFamilies{{qwen(), glm53(), gemma4(), deepseek(), raon1(), flint(), minimax()}};

// (the table is in EngineProfile's order: family_descriptor indexes it)
constexpr bool in_order() {
  for (std::size_t i = 0; i < kFamilies.size(); ++i) {
    if (static_cast<std::size_t>(kFamilies[i].profile) != i) return false;
  }
  return kFamilies.size() == static_cast<std::size_t>(EngineProfile::minimax) + 1U;
}
static_assert(in_order(), "one descriptor a profile, in EngineProfile's order");

}  // namespace

std::span<const FamilyDescriptor> family_descriptors() noexcept { return kFamilies; }

const FamilyDescriptor& family_descriptor(const EngineProfile profile) noexcept { return kFamilies[static_cast<std::size_t>(profile)]; }

std::string family_choices() {
  std::string words;
  for (std::size_t i = 0; i < kFamilies.size(); ++i) {
    if (i > 0) words += i + 1 == kFamilies.size() ? " or " : ", ";
    words += kFamilies[i].id;
  }
  return words;
}

const FamilyDescriptor* family_named(const std::string_view id) noexcept {
  for (const auto& family : kFamilies) {
    if (family.id == id) return &family;
  }
  return nullptr;
}

}  // namespace secure_frontend
