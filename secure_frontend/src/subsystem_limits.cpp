#include "secure_frontend/subsystem_limits.hpp"

#include "secure_frontend/image_payload.hpp"  // kVisionImageUrlMaxBytes (the pin's IMAGE_URL_MAX)

#include <chrono>

namespace secure_frontend {

ConnectionLimits connection_limits(const FrontendConfig& config) {
  const auto& http = config.limits.http;
  return {.head = {.max_request_line_bytes = http.max_request_line_bytes, .max_header_bytes = http.max_header_bytes,
                   .max_headers = http.max_headers, .max_request_bytes = http.max_request_bytes},
          .header_deadline = http.header_deadline, .request_deadline = http.request_deadline,
          .write_stall = http.write_stall, .response_write_deadline = http.response_write_deadline};
}

ChatSchemaLimits chat_schema_limits(const FrontendConfig& c) {
  ChatSchemaLimits limits;
  limits.max_body_bytes = c.limits.chat.max_body_bytes;
  limits.max_messages = c.limits.chat.max_messages;
  limits.max_message_bytes = c.limits.chat.max_message_bytes;
  limits.max_completion_tokens = c.limits.chat.max_completion_tokens;
  limits.max_generated_bytes = c.limits.chat.max_generated_bytes;
  // SP5 M1 Task 12 (C-9): the image caps come from the typed family; vision_capable
  // stays false HERE — only the composed launch (vision.enabled AND launch.mmproj)
  // can open the content-array gate (Ruling T12-b), and secure_server_main sets it.
  limits.max_images = static_cast<std::size_t>(c.vision.max_n);
  // Gemma 4 plan 1 Task 10: per-request reasoning_effort follows the configured profile (an
  // unset one is a managed model's: the bootstrap re-derives it from the composed launch).
  limits.per_request_reasoning_effort = per_request_reasoning_effort_for(c.engine.profile.value_or(EngineProfile::qwen));
  limits.max_image_b64_chars = (kVisionImageUrlMaxBytes + 2U) / 3U * 4U;
  return limits;
}

ChatExecutionLimits chat_execution_limits(const FrontendConfig& c) {
  ChatExecutionLimits limits;
  limits.maximum_prompt_tokens = c.limits.chat.max_prompt_tokens;
  limits.maximum_output_tokens = c.limits.chat.max_completion_tokens;
  limits.default_max_tokens = c.limits.chat.default_max_tokens;
  limits.max_generated_bytes = c.limits.chat.max_generated_bytes;
  limits.ready_timeout = c.limits.engine.ready_timeout;
  limits.generation_timeout = c.limits.engine.generation_timeout;
  limits.stop_drain_timeout = c.limits.engine.stop_drain_timeout;
  // SP5 M1 Task 8 (engine_silence_s / engine.repeat_stop_tokens): the watchdog and the
  // runaway rule ride the same limits into both generation pumps.
  limits.silence_timeout = c.engine.silence;
  limits.repeat_stop_tokens = c.engine.repeat_stop_tokens;
  return limits;
}

SupervisorConfig supervisor_config(const FrontendConfig& config) {
  SupervisorConfig supervisor;
  supervisor.chat_limits = chat_execution_limits(config);
  supervisor.policy.ready_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(config.limits.engine.ready_timeout);
  // An unset engine.profile (a managed model: its manifest decides) keeps qwen here; the
  // bootstrap re-selects from the composed launch through the same functions.
  const EngineProfile profile = config.engine.profile.value_or(EngineProfile::qwen);
  supervisor.profile = native_chat_profile_for(profile);
  // SP5 M2 Task 7 (R-T5-b): the glm53 profile speaks Project Maya v1.1.21's wire dialect
  // (Task 5); the selection is pinned by supervisor_flavor_is_the_maya_dialect_for_glm53.
  supervisor.flavor = protocol_flavor_for(profile);
  // SP5 M1 Task 6 (parity rows lazy_load / idle_unload_s, runconfig.py:35-38): the
  // supervisor's lifecycle knobs come straight from the typed launch family.
  supervisor.lazy_start = config.engine.lazy_load;
  supervisor.idle_unload = config.engine.idle_unload;
  return supervisor;
}

NativeChatProfile native_chat_profile_for(const EngineProfile profile) {
  switch (profile) {
    case EngineProfile::qwen: return NativeChatProfile::qwen;
    case EngineProfile::glm53: return NativeChatProfile::glm53;
    case EngineProfile::gemma4: return NativeChatProfile::gemma4;
    case EngineProfile::deepseek: return NativeChatProfile::deepseek;
    case EngineProfile::raon1: return NativeChatProfile::raon1;
    case EngineProfile::flint: return NativeChatProfile::flint;
    case EngineProfile::minimax: return NativeChatProfile::minimax;
  }
  return NativeChatProfile::qwen;  // unreachable: every value is a case above
}

// Gemma 4 spec §4.7: strata-gemma4-engine speaks upstream's strata_0143 serve protocol.
// deepseek is Maya's maya-ds4 (multiengine_maya_ds4, 2026-10-27): GLM's maya_118 dialect with READY <ctx> stop.
// raon1 is served by the glm_pack engine (strata_glm, arch-detected), so it speaks glm53's maya_118 serve
// protocol; qwen/glm53/gemma4 KEEP their current flavors.
EngineProtocolFlavor protocol_flavor_for(const EngineProfile profile) {
  switch (profile) {
    case EngineProfile::qwen: return EngineProtocolFlavor::strata_0143;
    case EngineProfile::glm53: return EngineProtocolFlavor::maya_118;
    case EngineProfile::gemma4: return EngineProtocolFlavor::strata_0143;
    case EngineProfile::deepseek: return EngineProtocolFlavor::maya_118;  // Maya's maya-ds4
    case EngineProfile::raon1: return EngineProtocolFlavor::maya_118;  // rides the glm_pack engine
    case EngineProfile::flint: return EngineProtocolFlavor::strata_0143;  // multiengine_flint: Gemma 4's serve loop
    case EngineProfile::minimax: return EngineProtocolFlavor::maya_118;  // Maya's maya-mm3: maya-ds4's wire
  }
  return EngineProtocolFlavor::strata_0143;  // unreachable
}

// Gemma 4 spec §6.1, §8: the Gemma engine gets the strata family's closed allowlist. deepseek (maya-ds4)
// takes its GPUs on the command line (--gpus), so it takes the strata environment family. raon1 rides the
// glm_pack engine, so it takes the glm env-variable mechanism like glm53.
EngineFamily engine_family_for(const EngineProfile profile) {
  switch (profile) {
    case EngineProfile::qwen: return EngineFamily::strata;
    case EngineProfile::glm53: return EngineFamily::glm;
    case EngineProfile::gemma4: return EngineFamily::strata;
    case EngineProfile::deepseek: return EngineFamily::strata;
    case EngineProfile::raon1: return EngineFamily::glm;  // rides the glm_pack engine
    case EngineProfile::flint: return EngineFamily::strata;  // argv --gpu, like gemma4
    case EngineProfile::minimax: return EngineFamily::strata;  // maya-mm3's --gpus, like maya-ds4
  }
  return EngineFamily::strata;  // unreachable
}

// Gemma 4 spec §6.6: none is thinking off, low/medium/high thinking on; glm53 refuses the field.
// raon1 (ChatML, KRAFTON chat_template.jinja) has a <think> block, so it admits reasoning_effort —
// none closes the block (direct answer), any level opens it. deepseek takes it as DeepSeek's encoder does: none is
// chat mode, a level the effort line (deepseek_effort_budget).
bool per_request_reasoning_effort_for(const EngineProfile profile) {
  switch (profile) {
    case EngineProfile::qwen: return true;
    case EngineProfile::glm53: return false;
    case EngineProfile::gemma4: return true;
    case EngineProfile::deepseek: return true;
    case EngineProfile::raon1: return true;
    case EngineProfile::flint: return true;  // none closes the think block, any level leaves it to the model
    case EngineProfile::minimax: return true;  // none is thinking off, a level MiniMax's <effort>
  }
  return false;  // unreachable
}

bool gemma4_thinking_default(const FrontendConfig& config) {
  return config.reasoning.effort.has_value() && *config.reasoning.effort != ReasoningEffort::none;
}

bool flint_thinking_default(const FrontendConfig& config) {
  return config.reasoning.effort != ReasoningEffort::none;  // unset: the template's own form, the model thinks
}

std::optional<std::string> minimax_effort_default(const FrontendConfig& config) {
  if (!config.reasoning.effort.has_value()) return std::nullopt;  // MiniMax's adaptive thinking, no effort named
  switch (*config.reasoning.effort) {
    case ReasoningEffort::none: return "none";
    case ReasoningEffort::low: return "low";
    case ReasoningEffort::medium: return "medium";
    case ReasoningEffort::high: return "high";
  }
  return std::nullopt;  // unreachable
}

TokenizerMode tokenizer_mode_for(const EngineProfile profile) {
  switch (profile) {
    case EngineProfile::qwen: return TokenizerMode::byte_bpe;
    // glm53: byte_bpe's files, Maya's glm4 split and ignore_merges (byte_bpe's qwen35 split is not GLM's)
    case EngineProfile::glm53: return TokenizerMode::glm_bpe;
    case EngineProfile::gemma4: return TokenizerMode::spm_bpe;
    // deepseek: byte_bpe's files, Maya's deepseek3 splits (strata_tokenizer.py)
    case EngineProfile::deepseek: return TokenizerMode::deepseek_bpe;
    // raon1: byte_bpe's files, raon-1-mini's NFC + pre-tokenizer (byte_bpe's approximate split gave ~46% more tokens)
    case EngineProfile::raon1: return TokenizerMode::raon_bpe;
    // flint: byte_bpe's files, llama.cpp's flint pre-tokenizer (three regexes over its collapsed text)
    case EngineProfile::flint: return TokenizerMode::flint_bpe;
    // minimax: byte_bpe's files, NFC and Maya's minimax split (strata_tokenizer.py MINIMAX_PATTERN)
    case EngineProfile::minimax: return TokenizerMode::minimax_bpe;
  }
  return TokenizerMode::byte_bpe;  // unreachable
}

}  // namespace secure_frontend
