#include "secure_frontend/chat_execution.hpp"

#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/deadline.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace secure_frontend {
namespace {

ChatExecutionOutcome error(const ChatExecutionErrorCode code, const char* const message) {
  return ChatExecutionError{code, message};
}

// SP3 M2: non-streaming observer; cancels on a gone client or on shutdown (R-H).
struct ControlObserver final : GenerationObserver {
  explicit ControlObserver(const ExecutionControl& control) : control_(control) {}
  void on_text(const std::string_view piece) override { text += piece; }
  void on_reasoning(const std::string_view piece) override { reasoning += piece; }
  RunCancel on_tick(bool) override {
    if ((control_.shutting_down != nullptr && control_.shutting_down->load()) || (control_.shutdown_requested && control_.shutdown_requested())) {
      return RunCancel::shutting_down;
    }
    if (control_.cancelled && control_.cancelled()) return RunCancel::client_gone;
    return RunCancel::none;
  }
  std::string text;
  std::string reasoning;
  const ExecutionControl& control_;
};

}  // namespace

std::optional<std::size_t> resolve_max_tokens(const std::optional<std::size_t> requested, const std::size_t prompt_tokens,
                                              const std::size_t context_tokens, const std::size_t max_completion_tokens,
                                              const std::size_t default_max_tokens, const bool fit_max_tokens,
                                              const std::size_t minimum_tokens, const std::size_t maximum_tokens) noexcept {
  // The engine's rule (generate.cpp): n + max_new + kContextMarginTokens <= ctx.
  constexpr std::size_t kMargin = api_constants::kContextMarginTokens;
  std::size_t room = max_completion_tokens;
  const bool context_clamp = context_tokens != 0U;
  if (context_clamp) {
    if (prompt_tokens >= context_tokens || context_tokens - prompt_tokens <= kMargin) return std::nullopt;
    room = context_tokens - prompt_tokens - kMargin;
  }
  std::size_t base = requested.has_value() ? *requested
                                           : (default_max_tokens == 0U ? max_completion_tokens : default_max_tokens);
  // SP5 M1 Task 5 (parity row fit_max_tokens, serve/server.py:4536-4543): with the pin's
  // fit_max_tokens false, an explicit max_tokens over the context room is refused, never shortened.
  if (!fit_max_tokens && requested.has_value() && context_clamp && base > room) return std::nullopt;
  // SP5 M1 Task 8 (parity rows engine.min_tokens / engine.max_tokens, added frontend bounds
  // per the Task-2 rows and Ruling T8-e): the floor lifts the base after the §7.2 room rule
  // (our bound never turns a request into a refusal), the ceiling caps last (0 = unset).
  if (minimum_tokens != 0U) base = std::max(base, minimum_tokens);
  std::size_t resolved = std::min({base, max_completion_tokens, room});
  if (maximum_tokens != 0U) resolved = std::min(resolved, maximum_tokens);
  if (resolved == 0U) return std::nullopt;
  return resolved;
}

ChatExecutionService::ChatExecutionService(EngineProtocolClient& protocol,
                                           const ChatExecutionLimits limits,
                                           const NativeChatProfile profile)
    : protocol_(protocol), limits_(limits), profile_(profile) {}

ChatExecutionOutcome ChatExecutionService::execute(const ChatCompletionRequest& request,
                                                   const TokenizerPack& tokenizer) {
  if (limits_.maximum_prompt_tokens == 0U || limits_.maximum_output_tokens == 0U ||
      limits_.maximum_output_tokens > std::numeric_limits<std::uint32_t>::max() ||
      limits_.ready_timeout.count() < 0 || limits_.generation_timeout.count() < 0 ||
      limits_.stop_drain_timeout.count() < 0 ||
      protocol_.closed()) {
    return error(ChatExecutionErrorCode::invalid_request,
                 "native chat execution limits or engine state are invalid");
  }

  // Gemma 4 plan 1 Task 3: an exhaustive switch, so a profile never takes another's plan.
  const auto plan_result = [&]() -> ChatGenerationPlanResult {
    switch (profile_) {
      case NativeChatProfile::qwen:
        return make_qwen_chat_generation_plan(request, tokenizer, limits_.maximum_prompt_tokens, limits_.default_max_tokens);
      case NativeChatProfile::glm53:
        return make_glm53_chat_generation_plan(request, tokenizer, limits_.maximum_prompt_tokens, limits_.default_max_tokens);
      case NativeChatProfile::gemma4:  // the template's default: thinking off (D10)
        return make_gemma4_chat_generation_plan(request, tokenizer, limits_.maximum_prompt_tokens, limits_.default_max_tokens);
      case NativeChatProfile::deepseek:  // unified-engine placeholder template (merged engine, gated)
        return make_deepseek_chat_generation_plan(request, tokenizer, limits_.maximum_prompt_tokens, limits_.default_max_tokens);
      case NativeChatProfile::raon1:
        return make_raon1_chat_generation_plan(request, tokenizer, limits_.maximum_prompt_tokens, limits_.default_max_tokens);
      case NativeChatProfile::flint:  // the template's own form: thinking on
        return make_flint_chat_generation_plan(request, tokenizer, limits_.maximum_prompt_tokens, limits_.default_max_tokens);
      case NativeChatProfile::minimax:  // MiniMax's adaptive thinking, no effort named
        return make_minimax_chat_generation_plan(request, tokenizer, limits_.maximum_prompt_tokens, limits_.default_max_tokens, {}, {});
    }
    return ChatGenerationPlanError{ChatGenerationPlanErrorCode::invalid_request, "unknown chat profile"};
  }();
  const auto* plan = std::get_if<ChatGenerationPlan>(&plan_result);
  if (plan == nullptr) {
    return error(ChatExecutionErrorCode::planning_failed,
                 "chat request cannot be represented by the native serving profile");
  }
  if (!received_ready_) {
    EngineReady ready{};
    if (protocol_.ready()) {
      return error(ChatExecutionErrorCode::engine_protocol_failed,
                   "engine protocol readiness state is inconsistent");
    }
    if (protocol_.await_ready(limits_.ready_timeout, ready).has_value()) {
      return error(ChatExecutionErrorCode::engine_protocol_failed,
                   "local engine did not become ready");
    }
    engine_context_tokens_ = ready.max_context_tokens;
    received_ready_ = true;
  }

  // B1/B2 (spec §5.4): an omitted max_tokens (plan value 0) is auto; a value
  // above the local output bound or the engine's context rule
  // (n + max_new + 8 <= --max-context) is clamped; only a prompt that leaves no
  // room is rejected. The result is at most maximum_output_tokens (<= uint32).
  const auto budget = resolve_max_tokens(plan->max_tokens == 0U ? std::nullopt : std::optional<std::size_t>{plan->max_tokens},
                                         plan->prompt_token_ids.size(), engine_context_tokens_,
                                         limits_.maximum_output_tokens, 0U);
  if (!protocol_.ready() || engine_context_tokens_ == 0U || !budget.has_value()) {
    return error(ChatExecutionErrorCode::context_limit_exceeded,
                 "request exceeds the local engine context limit");
  }

  std::vector<std::uint32_t> prompt_ids;
  prompt_ids.reserve(plan->prompt_token_ids.size());
  for (const std::size_t token_id : plan->prompt_token_ids) {
    if (token_id > std::numeric_limits<std::uint32_t>::max()) {
      return error(ChatExecutionErrorCode::planning_failed,
                   "local tokenizer produced an unsupported token identifier");
    }
    prompt_ids.push_back(static_cast<std::uint32_t>(token_id));
  }

  // One request generation deadline (K1, R1): it bounds the GEN write overall
  // and then the read to DONE, which gets only what the write left.
  const auto generation_deadline = deadline_after(limits_.generation_timeout);
  if (protocol_.begin_generation(static_cast<std::uint32_t>(*budget), prompt_ids, generation_deadline)
          .has_value()) {
    return error(ChatExecutionErrorCode::engine_protocol_failed,
                 "local engine rejected the generation request");
  }

  EngineGeneration generation{};
  if (const auto generation_error = protocol_.read_generation(remaining_until(generation_deadline), generation);
      generation_error.has_value()) {
    if (generation_error->code != EngineProtocolErrorCode::timeout) {
      return error(ChatExecutionErrorCode::engine_protocol_failed,
                   "local engine generation failed");
    }
    // The generation is still active. Stop it and drain to DONE so the
    // engine is idle for the next request instead of wedged forever.
    if (protocol_.stop(EngineStopCause::generation_timeout).has_value()) {
      protocol_.abandon();
      return error(ChatExecutionErrorCode::engine_protocol_failed, "engine could not be stopped");
    }
    EngineGeneration drained{};
    if (const auto drain_error = protocol_.read_generation(limits_.stop_drain_timeout, drained);
        drain_error.has_value()) {
      // Terminal failures already closed the protocol. A second timeout
      // means the engine ignores STOP; it must not receive further commands.
      if (drain_error->code == EngineProtocolErrorCode::timeout) protocol_.abandon();
      return error(ChatExecutionErrorCode::engine_protocol_failed, "engine did not drain after STOP");
    }
    return error(ChatExecutionErrorCode::generation_timed_out, "generation exceeded the time limit");
  }
  if (generation.token_ids.size() > *budget) {
    return error(ChatExecutionErrorCode::engine_protocol_failed,
                 "local engine exceeded the requested output limit");
  }

  std::vector<std::size_t> generated_ids;
  generated_ids.reserve(generation.token_ids.size());
  for (const std::uint32_t token_id : generation.token_ids) {
    generated_ids.push_back(static_cast<std::size_t>(token_id));
  }
  const auto decoded = decode_token_ids(tokenizer, generated_ids);
  const auto* text = std::get_if<std::string>(&decoded);
  if (text == nullptr) {
    return error(ChatExecutionErrorCode::output_decode_failed,
                 "local engine output cannot be safely decoded");
  }
  return ChatExecutionResult{*text, generated_ids.size()};
}

EngineHealth ChatExecutionService::health() const noexcept {
  if (protocol_.closed()) return EngineHealth::failed;
  if (!received_ready_) return EngineHealth::starting;
  if (protocol_.generating()) return EngineHealth::generating;
  return EngineHealth::ready;
}

void ChatExecutionService::adopt_ready(const EngineReady& ready) noexcept {
  engine_context_tokens_ = ready.max_context_tokens;
  received_ready_ = true;
}

std::size_t ChatExecutionService::context_tokens() const noexcept { return received_ready_ ? engine_context_tokens_ : 0U; }

// SP3 M2 Task 5: begin + the shared drive_generation loop (spec §3.3/§3.4).
// Ruling t5-begin-deadline: begin_generation keeps M1's GEN write deadline
// (ruling T15-signature), re-anchored at limits_.generation_timeout — the
// brief's three-argument call is M1-incompatible.
std::optional<GenerationFailure> ChatExecutionService::begin(const std::vector<std::uint32_t>& prompt_ids, const GenerationSettings& settings) {
  if (!received_ready_) {
    EngineReady ready{};
    if (protocol_.await_ready(limits_.ready_timeout, ready).has_value()) return GenerationFailure::engine_unavailable;
    adopt_ready(ready);
  }
  if (!protocol_.idle() || settings.max_tokens == 0U || settings.max_tokens > std::numeric_limits<std::uint32_t>::max()) {
    return GenerationFailure::engine_unavailable;
  }
  if (protocol_.begin_generation(static_cast<std::uint32_t>(settings.max_tokens), prompt_ids,
                                 deadline_after(limits_.generation_timeout), settings.sampling, settings.embeddings).has_value()) {
    return protocol_.closed() ? GenerationFailure::engine_unavailable : GenerationFailure::engine_error;
  }
  return std::nullopt;
}

GenerationRunLimits ChatExecutionService::drive_limits(const std::size_t max_tokens) const noexcept {
  GenerationRunLimits limits;
  limits.generation_timeout = limits_.generation_timeout;
  limits.stop_drain_timeout = limits_.stop_drain_timeout;
  limits.max_generated_bytes = limits_.max_generated_bytes;
  limits.max_tokens = max_tokens;
  // SP5 M1 Task 8: the silence watchdog and the repeat-stop rule ride both pumps.
  limits.silence = limits_.silence_timeout;
  limits.repeat_stop_tokens = limits_.repeat_stop_tokens;
  return limits;
}

EngineProtocolClient& ChatExecutionService::protocol() noexcept { return protocol_; }

GenerationOutcome ChatExecutionService::run_generation(const std::vector<std::uint32_t>& prompt_ids, const GenerationSettings& settings,
                                                       const ExecutionControl& control, const TokenizerPack& tokenizer) {
  if (const auto failure = begin(prompt_ids, settings)) return *failure;
  ControlObserver observer(control);
  const TokenByteDecoder decoder(tokenizer);
  GenerationRunLimits limits = drive_limits(settings.max_tokens);
  limits.think_tokens = settings.think_tokens;
  limits.starts_in_reasoning = settings.starts_in_reasoning;
  limits.stop_tokens = settings.stop_tokens;
  limits.server_stop_tokens = settings.server_stop_tokens;
  limits.keeps_newlines_after_think = settings.keeps_newlines_after_think;
  limits.channel_name = settings.channel_name;  // Gemma 4 plan 1 Task 9
  limits.meter = settings.meter;
  const GenerationRunResult run = drive_generation(protocol_, decoder, limits, observer);
  const bool shutting = (control.shutting_down != nullptr && control.shutting_down->load()) ||
                        (control.shutdown_requested && control.shutdown_requested());
  switch (run.end) {
    case RunEnd::stop:
    case RunEnd::length:
      return GenerationResult{std::move(observer.text), prompt_ids.size(), run.completion_tokens,
                              run.end == RunEnd::stop ? FinishReason::stop : FinishReason::length, run.timed_out,
                              std::move(observer.reasoning)};
    case RunEnd::cancelled: return run.cancel == RunCancel::shutting_down ? GenerationFailure::shutting_down : GenerationFailure::cancelled;
    case RunEnd::engine_error: return GenerationFailure::engine_error;
    case RunEnd::undecodable: return GenerationFailure::decode_failed;
    case RunEnd::engine_unavailable: break;
  }
  return shutting ? GenerationFailure::shutting_down : GenerationFailure::engine_unavailable;
}

}  // namespace secure_frontend
