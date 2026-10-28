#pragma once

#include "secure_frontend/chat_execution.hpp"
#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/engine_supervisor.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/connection_handler.hpp"
#include "secure_frontend/engine_environment.hpp"
#include "secure_frontend/tokenizer_pack.hpp"

namespace secure_frontend {

// Derived once from the validated configuration, before bind/spawn, and
// passed by value into each subsystem's private const state.
// Per-connection bounds (spec §2.2); the listen backlog is listener.backlog, passed to HttpSocketServer::bind.
[[nodiscard]] ConnectionLimits connection_limits(const FrontendConfig& config);
[[nodiscard]] ChatSchemaLimits chat_schema_limits(const FrontendConfig& config);
[[nodiscard]] ChatExecutionLimits chat_execution_limits(const FrontendConfig& config);
// The supervisor's configuration from the validated config: chat limits as
// chat_execution_limits(), READY deadline from limits.engine.ready_timeout_s,
// every other policy value from config_defaults.hpp.
[[nodiscard]] SupervisorConfig supervisor_config(const FrontendConfig& config);

// Gemma 4 plan 1 Task 3 (spec §6.1): what each engine profile selects, as exhaustive
// switches shared by supervisor_config() and the bootstrap (server_bootstrap.cpp), so a new
// profile cannot silently take another profile's branch.
[[nodiscard]] NativeChatProfile native_chat_profile_for(EngineProfile profile);
[[nodiscard]] EngineProtocolFlavor protocol_flavor_for(EngineProfile profile);
[[nodiscard]] EngineFamily engine_family_for(EngineProfile profile);
// Whether the schema admits reasoning_effort per request (qwen and gemma4; glm53 refuses it).
[[nodiscard]] bool per_request_reasoning_effort_for(EngineProfile profile);
// Gemma 4 plan 1 Task 4 (spec §6.2): the tokenizer mode the pack is loaded in (gemma4:
// spm_bpe, every other profile byte_bpe); never inferred from the pack's files.
[[nodiscard]] TokenizerMode tokenizer_mode_for(EngineProfile profile);
// Gemma 4 plan 1 Task 10 (spec §6.6, D10): whether a gemma4 request that names no
// reasoning_effort thinks: reasoning.effort low/medium/high yes; none or unset no (the
// template's default is thinking off).
[[nodiscard]] bool gemma4_thinking_default(const FrontendConfig& config);
// flint: whether a request that names no reasoning_effort thinks: yes unless reasoning.effort is none (the template
// always lets the model open <think>; unset keeps that form).
[[nodiscard]] bool flint_thinking_default(const FrontendConfig& config);
// minimax: the level a request that names no reasoning_effort gets - reasoning.effort's word (none: thinking off), or
// nullopt when it is unset (MiniMax's adaptive thinking with no effort named, Maya's default "auto").
[[nodiscard]] std::optional<std::string> minimax_effort_default(const FrontendConfig& config);

}  // namespace secure_frontend
