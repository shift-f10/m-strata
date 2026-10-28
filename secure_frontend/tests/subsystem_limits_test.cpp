#include "secure_frontend/chat_execution.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/subsystem_limits.hpp"
#include "support/config_json.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <variant>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

std::shared_ptr<const secure_frontend::FrontendConfig> config_with(const std::map<std::string, std::string>& extra) {
  secure_frontend_test::ConfigFixture f;
  auto literals = f.literals;
  for (const auto& [k, v] : extra) literals[k] = v;
  auto draft = secure_frontend::parse_config_text(secure_frontend_test::render_config_json(literals), f.dir);
  auto result = secure_frontend::validate(std::get<secure_frontend::FrontendConfigDraft>(draft));
  return std::get<secure_frontend::ValidatedConfig>(result).shared();
}

void ready_timeout_defaults_to_900_seconds() {
  expect(secure_frontend::ChatExecutionLimits{}.ready_timeout == std::chrono::seconds(900),
         "ChatExecutionLimits{} waits 900 s for READY (upstream STRATA_ENGINE_READY_S)");
  expect(secure_frontend::chat_execution_limits(*config_with({})).ready_timeout == std::chrono::seconds(900),
         "the default config derives 900 s");
}

void subsystem_limits_follow_config() {
  const auto config = config_with({{"limits.http.max_request_bytes", "262144"},
                                   {"limits.http.max_header_bytes", "8192"},
                                   {"limits.http.max_headers", "32"},
                                   {"limits.http.request_deadline_ms", "1500"},
                                   // SP3 §5.1: header_deadline_ms (default 10000) must not exceed it.
                                   {"limits.http.header_deadline_ms", "1500"},
                                   {"listener.backlog", "64"},
                                   {"limits.chat.max_body_bytes", "131072"},
                                   {"limits.chat.default_max_tokens", "64"},
                                   {"limits.engine.ready_timeout_s", "7"}});
  const auto http = secure_frontend::connection_limits(*config);
  expect(http.head.max_request_bytes == 262144U && http.head.max_header_bytes == 8192U && http.head.max_headers == 32U &&
             http.head.max_request_line_bytes == 4096U && http.request_deadline == std::chrono::milliseconds(1500) &&
             http.header_deadline == std::chrono::milliseconds(1500),
         "connection and parser limits come from the config");
  expect(http.write_stall == std::chrono::milliseconds(10000) && http.response_write_deadline == std::chrono::milliseconds(60000),
         "write_stall and response_write_deadline default to 10 s and 60 s");
  // Pre-flight T8b(e)/V17: ConnectionLimits has no backlog; bind() takes it from the listener config.
  expect(config->listener.backlog == 64U, "the listener backlog comes from the config");
  const auto schema = secure_frontend::chat_schema_limits(*config);
  expect(schema.max_body_bytes == 131072U, "chat body limit comes from the config");
  const auto chat = secure_frontend::chat_execution_limits(*config);
  expect(chat.ready_timeout == std::chrono::seconds(7) && chat.default_max_tokens == 64U, "chat execution limits");
}

void supervisor_policy_follows_config() {
  const auto config = config_with({{"limits.engine.ready_timeout_s", "7"}});
  const secure_frontend::SupervisorConfig supervisor = secure_frontend::supervisor_config(*config);
  expect(supervisor.policy.ready_timeout == std::chrono::seconds(7), "limits.engine.ready_timeout_s drives RestartPolicy::ready_timeout");
  expect(supervisor.chat_limits.ready_timeout == std::chrono::seconds(7), "and the chat limits agree");
  const secure_frontend::RestartPolicy defaults{};
  expect(defaults.ready_timeout == std::chrono::seconds(secure_frontend::config_defaults::kEngineReadyTimeoutS),
         "the policy default is SP1's 900 s constant, not a second copy");
  expect(supervisor.policy.max_consecutive_failures == secure_frontend::config_defaults::kMaxConsecutiveFailures &&
             supervisor.policy.backoff_initial == secure_frontend::config_defaults::kBackoffInitial,
         "the other policy values come from config_defaults.hpp");
}

// SP5 M2 Task 7 (Ruling R-T5-b): the glm53 profile speaks Project Maya v1.1.21's wire
// dialect (Task 5), not the uncharacterized lighttransport one; the qwen profile is
// unchanged. The same selection stands at the bootstrap site (server_bootstrap.cpp).
void supervisor_flavor_is_the_maya_dialect_for_glm53() {
  const auto glm = config_with({{"engine.profile", "\"glm53\""}});
  const secure_frontend::SupervisorConfig supervisor = secure_frontend::supervisor_config(*glm);
  expect(supervisor.profile == secure_frontend::NativeChatProfile::glm53, "the chat profile stays glm53");
  expect(supervisor.flavor == secure_frontend::EngineProtocolFlavor::maya_118,
         "glm53 selects the maya_118 dialect at the selection site");
  const secure_frontend::SupervisorConfig plain = secure_frontend::supervisor_config(*config_with({}));
  expect(plain.flavor == secure_frontend::EngineProtocolFlavor::strata_0143, "the qwen profile keeps the strata dialect");
}

// Gemma 4 plan 1 Task 3 (spec §6.1): gemma4 is its own chat profile on the strata_0143
// wire and the strata environment family; the selection functions are exhaustive switches
// that the bootstrap shares (server_bootstrap.cpp), so a third profile never takes glm's.
void supervisor_profile_and_flavor_for_gemma4() {
  using secure_frontend::EngineProfile;
  const secure_frontend::SupervisorConfig gemma = secure_frontend::supervisor_config(*config_with({{"engine.profile", "\"gemma4\""}}));
  expect(gemma.profile == secure_frontend::NativeChatProfile::gemma4, "gemma4 selects NativeChatProfile::gemma4");
  expect(gemma.flavor == secure_frontend::EngineProtocolFlavor::strata_0143, "gemma4 speaks strata_0143 (spec §4.7)");
  expect(secure_frontend::native_chat_profile_for(EngineProfile::qwen) == secure_frontend::NativeChatProfile::qwen &&
             secure_frontend::native_chat_profile_for(EngineProfile::glm53) == secure_frontend::NativeChatProfile::glm53 &&
             secure_frontend::native_chat_profile_for(EngineProfile::gemma4) == secure_frontend::NativeChatProfile::gemma4,
         "each profile selects its own chat profile");
  expect(secure_frontend::protocol_flavor_for(EngineProfile::qwen) == secure_frontend::EngineProtocolFlavor::strata_0143 &&
             secure_frontend::protocol_flavor_for(EngineProfile::glm53) == secure_frontend::EngineProtocolFlavor::maya_118 &&
             secure_frontend::protocol_flavor_for(EngineProfile::gemma4) == secure_frontend::EngineProtocolFlavor::strata_0143,
         "the wire flavor per profile");
  expect(secure_frontend::engine_family_for(EngineProfile::qwen) == secure_frontend::EngineFamily::strata &&
             secure_frontend::engine_family_for(EngineProfile::glm53) == secure_frontend::EngineFamily::glm &&
             secure_frontend::engine_family_for(EngineProfile::gemma4) == secure_frontend::EngineFamily::strata,
         "the environment family per profile (gemma4: the strata allowlist)");
  expect(secure_frontend::per_request_reasoning_effort_for(EngineProfile::qwen) &&
             !secure_frontend::per_request_reasoning_effort_for(EngineProfile::glm53) &&
             secure_frontend::per_request_reasoning_effort_for(EngineProfile::gemma4),
         "reasoning_effort per request: qwen and gemma4, never glm53 (spec §6.6)");
}
// Gemma 4 plan 1 Task 4 (Review Focus 2): the bootstrap loads the pack in the profile's mode.
void tokenizer_mode_follows_the_profile() {
  using secure_frontend::EngineProfile;
  using secure_frontend::TokenizerMode;
  expect(secure_frontend::tokenizer_mode_for(EngineProfile::qwen) == TokenizerMode::byte_bpe, "qwen packs are byte_bpe");
  // glm53 (changed 2026-10-27; it was byte_bpe): byte_bpe's files, Maya's glm4 splits and ignore_merges
  expect(secure_frontend::tokenizer_mode_for(EngineProfile::glm53) == TokenizerMode::glm_bpe, "glm53 packs are glm_bpe");
  expect(secure_frontend::tokenizer_mode_for(EngineProfile::gemma4) == TokenizerMode::spm_bpe, "gemma4 packs are spm_bpe");
  // deepseek (2026-10-27, Maya's maya-ds4): byte_bpe's files, Maya's deepseek3 splits (strata_tokenizer.py)
  expect(secure_frontend::tokenizer_mode_for(EngineProfile::deepseek) == TokenizerMode::deepseek_bpe, "deepseek packs are deepseek_bpe");
  // raon1's files are byte_bpe's, but its text is NFC-normalized and split by its own pre-tokenizer (llama.cpp's
  // unicode_regex_split_custom_raon): byte_bpe's approximate split gave ~46% more tokens than the model's tokenizer
  expect(secure_frontend::tokenizer_mode_for(EngineProfile::raon1) == TokenizerMode::raon_bpe, "raon1 packs are raon_bpe");
}

// deepseek (2026-10-27; the merged-engine plan it was gated for gave way to one engine with a library per family): Maya's
// maya-ds4 behind multiengine --pack - its own chat profile (DeepSeek's encoder), Maya's maya_118 wire (READY ... stop,
// the 10-field DONE), the strata environment family (GPUs on the command line, --gpus) and per-request reasoning_effort
// (DeepSeek's effort line, none for chat mode). raon1 is a first-class glm_pack family: NativeChatProfile::raon1 (ChatML,
// honors reasoning_effort) but glm53's engine behavior -- the maya_118 serve flavor, the glm environment family and
// byte_bpe packs. The selection functions are exhaustive switches.
void supervisor_selection_for_unified_families() {
  using secure_frontend::EngineProfile;
  // deepseek -- Maya's maya-ds4.
  expect(secure_frontend::native_chat_profile_for(EngineProfile::deepseek) == secure_frontend::NativeChatProfile::deepseek,
         "deepseek selects its own chat profile");
  expect(secure_frontend::protocol_flavor_for(EngineProfile::deepseek) == secure_frontend::EngineProtocolFlavor::maya_118,
         "deepseek speaks Maya's maya_118 flavor");
  expect(secure_frontend::engine_family_for(EngineProfile::deepseek) == secure_frontend::EngineFamily::strata,
         "deepseek uses the strata environment family (GPUs on the command line)");
  expect(secure_frontend::per_request_reasoning_effort_for(EngineProfile::deepseek),
         "deepseek takes reasoning_effort per request (DeepSeek's effort line)");
  // minimax (owner 2026-10-28) -- Maya's maya-mm3: its own chat profile (Maya's MiniMax template), the same maya_118
  // wire as maya-ds4 (READY <ctx> stop, the 10-field DONE), GPUs on the command line, reasoning_effort per request
  // (MiniMax's effort, none for thinking off), minimax_bpe packs (byte_bpe's files, NFC and Maya's minimax split).
  expect(secure_frontend::native_chat_profile_for(EngineProfile::minimax) == secure_frontend::NativeChatProfile::minimax,
         "minimax selects its own chat profile");
  expect(secure_frontend::protocol_flavor_for(EngineProfile::minimax) == secure_frontend::EngineProtocolFlavor::maya_118,
         "minimax speaks Maya's maya_118 flavor");
  expect(secure_frontend::engine_family_for(EngineProfile::minimax) == secure_frontend::EngineFamily::strata,
         "minimax uses the strata environment family (GPUs on the command line)");
  expect(secure_frontend::per_request_reasoning_effort_for(EngineProfile::minimax), "minimax takes reasoning_effort per request");
  expect(secure_frontend::tokenizer_mode_for(EngineProfile::minimax) == secure_frontend::TokenizerMode::minimax_bpe,
         "minimax packs are minimax_bpe");
  const secure_frontend::SupervisorConfig minimax = secure_frontend::supervisor_config(*config_with({{"engine.profile", "\"minimax\""}}));
  expect(minimax.profile == secure_frontend::NativeChatProfile::minimax && minimax.flavor == secure_frontend::EngineProtocolFlavor::maya_118,
         "a minimax config's supervisor: its chat profile, maya_118");
  // raon1 -- a first-class glm_pack family: raon1 chat template, glm53 engine behavior.
  expect(secure_frontend::native_chat_profile_for(EngineProfile::raon1) == secure_frontend::NativeChatProfile::raon1,
         "raon1 selects its own ChatML chat profile");
  expect(secure_frontend::protocol_flavor_for(EngineProfile::raon1) == secure_frontend::EngineProtocolFlavor::maya_118,
         "raon1 speaks the glm_pack engine's maya_118 flavor");
  expect(secure_frontend::engine_family_for(EngineProfile::raon1) == secure_frontend::EngineFamily::glm,
         "raon1 uses the glm environment family (glm_pack engine)");
  expect(secure_frontend::per_request_reasoning_effort_for(EngineProfile::raon1),
         "raon1 honors per-request reasoning_effort (its ChatML think block)");
  expect(secure_frontend::tokenizer_mode_for(EngineProfile::raon1) == secure_frontend::TokenizerMode::raon_bpe,
         "raon1 packs are raon_bpe (byte_bpe's files, raon-1-mini's NFC + pre-tokenizer)");
  // qwen/glm53/gemma4 keep their current flavors (today's engines must not change).
  expect(secure_frontend::protocol_flavor_for(EngineProfile::qwen) == secure_frontend::EngineProtocolFlavor::strata_0143 &&
             secure_frontend::protocol_flavor_for(EngineProfile::glm53) == secure_frontend::EngineProtocolFlavor::maya_118 &&
             secure_frontend::protocol_flavor_for(EngineProfile::gemma4) == secure_frontend::EngineProtocolFlavor::strata_0143,
         "qwen/glm53/gemma4 flavors are unchanged");
}

// flint (multiengine_flint, whose serve loop is Gemma 4's): its own chat profile on the strata_0143 wire, the strata
// environment family (argv --gpu), reasoning_effort per request, flint_bpe packs (byte_bpe's files, llama.cpp's flint
// pre-tokenizer), and thinking on unless reasoning.effort is none (the template always lets the model think).
void supervisor_selection_for_flint() {
  using secure_frontend::EngineProfile;
  const secure_frontend::SupervisorConfig flint = secure_frontend::supervisor_config(*config_with({{"engine.profile", "\"flint\""}}));
  expect(flint.profile == secure_frontend::NativeChatProfile::flint, "flint selects NativeChatProfile::flint");
  expect(flint.flavor == secure_frontend::EngineProtocolFlavor::strata_0143, "flint speaks strata_0143");
  expect(secure_frontend::engine_family_for(EngineProfile::flint) == secure_frontend::EngineFamily::strata,
         "flint uses the strata environment family (argv --gpu)");
  expect(secure_frontend::per_request_reasoning_effort_for(EngineProfile::flint), "flint admits reasoning_effort per request");
  expect(secure_frontend::tokenizer_mode_for(EngineProfile::flint) == secure_frontend::TokenizerMode::flint_bpe,
         "flint packs are flint_bpe");
  expect(secure_frontend::flint_thinking_default(*config_with({{"engine.profile", "\"flint\""}})),
         "unset reasoning.effort: flint thinks");
  expect(!secure_frontend::flint_thinking_default(
             *config_with({{"engine.profile", "\"flint\""}, {"reasoning.effort", "\"none\""}})),
         "reasoning.effort none: flint answers directly");
  expect(secure_frontend::flint_thinking_default(*config_with({{"engine.profile", "\"flint\""}, {"reasoning.effort", "\"low\""}})),
         "reasoning.effort low: flint thinks");
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"ready_timeout_defaults_to_900_seconds", ready_timeout_defaults_to_900_seconds},
      {"subsystem_limits_follow_config", subsystem_limits_follow_config},
      {"supervisor_policy_follows_config", supervisor_policy_follows_config},
      {"supervisor_flavor_is_the_maya_dialect_for_glm53", supervisor_flavor_is_the_maya_dialect_for_glm53},
      {"supervisor_profile_and_flavor_for_gemma4", supervisor_profile_and_flavor_for_gemma4},
      {"tokenizer_mode_follows_the_profile", tokenizer_mode_follows_the_profile},
      {"supervisor_selection_for_unified_families", supervisor_selection_for_unified_families},
      {"supervisor_selection_for_flint", supervisor_selection_for_flint},
  }, failures);
}
