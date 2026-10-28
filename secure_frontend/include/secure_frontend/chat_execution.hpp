#pragma once

#include "secure_frontend/chat_generation_plan.hpp"
#include "secure_frontend/config_defaults.hpp"
#include "secure_frontend/engine_protocol_client.hpp"
#include "secure_frontend/generation_run.hpp"
#include "secure_frontend/sampling_params.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace secure_frontend {

// An explicit startup-selected prompt/tokenizer contract.  This is not
// inferred from a model name or tokenizer asset: that would allow a model
// artifact to select behavior in the HTTP-serving process.
enum class NativeChatProfile {
  qwen,
  glm53,
  gemma4,     // Gemma 4 plan 1 Task 3 (spec §6.1)
  deepseek,   // unified-engine design §3.2/§3.3: merged-engine families, placeholder templates
  raon1,
  flint,      // multiengine_flint: the GGUF's template (render_flint_text_prompt)
  minimax,    // Maya's maya-mm3 (owner 2026-10-28, MiniMax Community License): Maya's template (render_minimax_text_prompt)
};

// Bounds for the native, non-streaming chat path, derived from FrontendConfig by
// chat_execution_limits(). Separate from engine-protocol bounds: both layers
// must agree before an engine command is issued.
struct ChatExecutionLimits final {
  std::size_t maximum_prompt_tokens{static_cast<std::size_t>(config_defaults::kChatMaxPromptTokens)};
  std::size_t maximum_output_tokens{static_cast<std::size_t>(config_defaults::kChatMaxCompletionTokens)};
  std::size_t default_max_tokens{static_cast<std::size_t>(config_defaults::kChatDefaultMaxTokens)};
  std::chrono::milliseconds ready_timeout{std::chrono::seconds(config_defaults::kEngineReadyTimeoutS)};
  std::chrono::milliseconds generation_timeout{std::chrono::seconds(config_defaults::kEngineGenerationTimeoutS)};
  // Total budget for draining to DONE after STOP once generation_timeout expires.
  std::chrono::milliseconds stop_drain_timeout{std::chrono::seconds(config_defaults::kEngineStopDrainTimeoutS)};
  // SP3 §3.4: the generated text is capped on a code-point boundary at this many bytes.
  std::size_t max_generated_bytes{static_cast<std::size_t>(config_defaults::kChatMaxGeneratedBytes)};
  // SP5 M1 Task 8 (parity row engine_silence_s, runconfig.py:39 + serve/server.py:274-276):
  // an engine that says nothing valid for this long reaches the stall path at the silence
  // bound (0: only the total generation deadline, like the pin's 0 = wait).
  std::chrono::milliseconds silence_timeout{0};
  // SP5 M1 Task 8 (parity row repeat_stop_tokens, serve/server.py:197/:4908): one token this
  // many times in a row ends a reply as length (0: off, the pin's opt-out, :8115).
  std::size_t repeat_stop_tokens{0};
};

enum class ChatExecutionErrorCode {
  invalid_request,
  planning_failed,
  context_limit_exceeded,
  engine_protocol_failed,
  output_decode_failed,
  generation_timed_out,
};

struct ChatExecutionError final {
  ChatExecutionErrorCode code;
  std::string message;
};

struct ChatExecutionResult final {
  std::string text;
  std::size_t generated_tokens;
};

using ChatExecutionOutcome = std::variant<ChatExecutionResult, ChatExecutionError>;

// Engine protocol state for health reporting. Carries no engine or prompt data.
// starting: READY not yet received; ready: protocol idle; generating: a
// request is in flight; failed: the protocol is closed or failed.
enum class EngineHealth { starting, ready, generating, failed };

// Spec §5.4, owner decisions B1/B2. Returns the max_new for GEN, or nullopt
// when the prompt alone leaves no room (400 context_length_exceeded).
// context_tokens 0 = not known yet (no context clamp); default_max_tokens 0 =
// auto (min(max_completion_tokens, context - prompt - 8)).
// fit_max_tokens (SP5 M1 Task 5, parity row fit_max_tokens, serve/server.py:4526-4544): true (the
// pin default) shortens an explicit request over the context room to the room; false answers the
// §7.2 400 instead. The engine-internal max_completion cap still clamps (B2 is not the pin's room
// rule); an auto request always fits the room either way.
[[nodiscard]] std::optional<std::size_t> resolve_max_tokens(std::optional<std::size_t> requested, std::size_t prompt_tokens,
                                                            std::size_t context_tokens, std::size_t max_completion_tokens,
                                                            std::size_t default_max_tokens, bool fit_max_tokens = true,
                                                            std::size_t minimum_tokens = 0,
                                                            std::size_t maximum_tokens = 0) noexcept;

// SP3 Task 15 (spec §2.6 P6): the concurrent generation path. run_generation
// owns one engine generation end to end — GEN, 20 ms read slices, and the
// cancel/timeout/shutdown exits (STOP + bounded drain) — and reports the §3.5
// ingredients. The pool's shutdown flag and SP2's signal flag are both watched
// (ruling R-H: ChatRoute may supply either).
enum class FinishReason { stop, length };

struct GenerationSettings final {
  std::size_t max_tokens;
  SamplingParams sampling{};
  // SP5 M1 Task 12 (T12-i): the combined vision embeddings file for a GENI line;
  // empty renders a plain GEN (text requests unchanged).
  std::string embeddings{};
  // The plan's reasoning split (ChatGenerationPlan::think_tokens / starts_in_reasoning),
  // handed to drive_generation through drive_limits.
  std::optional<ThinkTokens> think_tokens{};
  bool starts_in_reasoning{false};
  std::vector<std::size_t> stop_tokens{};  // ChatGenerationPlan::stop_tokens (issue 26)
  std::optional<std::string> channel_name{};  // ChatGenerationPlan::channel_name (Gemma 4 plan 1 Task 9)
  std::vector<std::size_t> server_stop_tokens{};  // ChatGenerationPlan::server_stop_tokens (DeepSeek's turn ends)
  bool keeps_newlines_after_think{false};          // ChatGenerationPlan::keeps_newlines_after_think (MiniMax's)
  GenerationMeter* meter{nullptr};         // the Monitor (listener.monitor); null: none
};

struct ExecutionControl final {
  std::function<bool()> cancelled;
  const std::atomic<bool>* shutting_down{nullptr};
  std::function<bool()> shutdown_requested{};
};

struct GenerationResult final {
  std::string text;
  std::size_t prompt_tokens;
  std::size_t completion_tokens;
  FinishReason finish;
  bool timed_out{false};  // B4: a timeout is a result with FinishReason::length, flagged for the log
  std::string reasoning{};  // the think block's text (message.reasoning_content); empty: none
};

enum class GenerationFailure { cancelled, engine_error, engine_unavailable, decode_failed, shutting_down };

using GenerationOutcome = std::variant<GenerationResult, GenerationFailure>;

// Stateful adapter for one private Strata --serve connection. It accepts a
// schema-validated request, uses the startup-selected fixed prompt renderer
// and offline tokenizer, and decodes only ordinary generated token IDs. It does not load
// files, spawn processes, expose streaming data, or contact a network.
//
// The first execution waits for READY. Later executions reuse a clean idle
// protocol client. Any engine protocol failure is returned without reflecting
// engine or prompt data to a network caller.
class ChatExecutionService final {
 public:
  explicit ChatExecutionService(EngineProtocolClient& protocol,
                                ChatExecutionLimits limits = {},
                                NativeChatProfile profile = NativeChatProfile::qwen);

  [[nodiscard]] ChatExecutionOutcome execute(const ChatCompletionRequest& request,
                                              const TokenizerPack& tokenizer);

  [[nodiscard]] EngineHealth health() const noexcept;
  // Eager READY (SP2): the supervisor consumed READY on `protocol` before the
  // first request; execute() then never waits for it. Call once, before execute().
  void adopt_ready(const EngineReady& ready) noexcept;

  // READY context from adopt_ready/await_ready; 0 before READY.
  [[nodiscard]] std::size_t context_tokens() const noexcept;

  [[nodiscard]] GenerationOutcome run_generation(const std::vector<std::uint32_t>& prompt_ids, const GenerationSettings& settings,
                                                 const ExecutionControl& control, const TokenizerPack& tokenizer);

  // SP3 M2 Task 5: the two halves of run_generation, so the stream path (Task 10)
  // can own the driving loop. begin adopts/awaits READY and writes GEN with
  // M1's sampling keys (B6); drive_limits maps settings.max_tokens plus this
  // service's limits onto the shared loop; protocol() is the leased client.
  [[nodiscard]] std::optional<GenerationFailure> begin(const std::vector<std::uint32_t>& prompt_ids, const GenerationSettings& settings);
  [[nodiscard]] GenerationRunLimits drive_limits(std::size_t max_tokens) const noexcept;
  [[nodiscard]] EngineProtocolClient& protocol() noexcept;

 private:
  EngineProtocolClient& protocol_;
  ChatExecutionLimits limits_;
  NativeChatProfile profile_;
  bool received_ready_{false};
  std::size_t engine_context_tokens_{0U};
};

}  // namespace secure_frontend
