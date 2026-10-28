#pragma once

#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/config_defaults.hpp"
#include "secure_frontend/minimax_prompt_renderer.hpp"
#include "secure_frontend/qwen_prompt_renderer.hpp"
#include "secure_frontend/sampling_params.hpp"
#include "secure_frontend/tokenizer_pack.hpp"
#include "secure_frontend/tool_calls.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace secure_frontend {

struct SamplingDefaults;  // frontend_config.hpp: the defaults.sampling.* rows

// The only prompt format currently supported by the secure native serving
// path.  It is deliberately explicit: selecting a tokenizer pack alone must
// never cause an unreviewed model-provided template to execute.
struct ChatGenerationPlan final {
  std::string model;
  std::vector<std::size_t> prompt_token_ids;
  std::size_t max_tokens;
  SamplingParams sampling;  // SP5 M1 Task 4: config defaults, request fields win
  // The reasoning split for the reply: the pack's <think>/</think> ids (nullopt: the pack
  // has none, the plain prompt) and whether the prompt left a think block open, so the
  // reply begins as reasoning (upstream OutputParser(thinking=...), serve/frontend.py:884).
  std::optional<ThinkTokens> think_tokens{};
  bool starts_in_reasoning{false};
  // Issue 26: the end-of-turn ids that end the reply as stop (find_qwen_end_of_turn_tokens;
  // glm53: find_glm53_end_of_turn_tokens).
  std::vector<std::size_t> stop_tokens{};
  // Gemma 4 plan 1 Task 9: the reasoning channel's name line (GenerationRunLimits::channel_name),
  // "thought" for gemma4; nullopt for qwen and glm53.
  std::optional<std::string> channel_name{};
  // The turn markers the frontend stops the engine on (GenerationRunLimits::server_stop_tokens): deepseek's
  // find_deepseek_end_of_turn_tokens; empty for the profiles whose engines stop on their own.
  std::vector<std::size_t> server_stop_tokens{};
  // Web search: the special tokens the reply outputs as their text (GenerationRunLimits::literal_tokens) - the Qwen tool
  // tags of a tool plan (make_qwen_tool_generation_plan); empty for every other plan.
  std::vector<std::pair<std::size_t, std::string>> literal_tokens{};
  // The newlines right after the reasoning's close stay in the answer (GenerationRunLimits::keeps_newlines_after_think):
  // MiniMax's, as Maya's parser keeps them there (its lead rule is GLM's and Qwen's only); false for every other plan.
  bool keeps_newlines_after_think{false};
};

enum class ChatGenerationPlanErrorCode {
  invalid_request,
  prompt_render_failed,
  tokenization_failed,
  prompt_too_large,
};

struct ChatGenerationPlanError final {
  ChatGenerationPlanErrorCode code;
  std::string message;
};

using ChatGenerationPlanResult = std::variant<ChatGenerationPlan, ChatGenerationPlanError>;

// Converts the already schema-validated request into a fixed Qwen ChatML prompt and
// then local token IDs.  This function never loads files, executes templates, starts
// the engine, or contacts the network.  It rejects zero output tokens and prompt sizes
// beyond the supplied fixed limit.
// SP5 M1 Task 12 (Ruling T12-f): image parts (schema-gated by the composed vision
// capability) render the template's per-image marker runs; image_token_counts carries
// the encoder's per-image token counts IN PART ORDER and must match the request's
// image parts exactly — a mismatch is an internal wiring bug, closed here.
// thinking (reasoning.effort; upstream's default is on, serve/frontend.py:134-148): when
// the pack carries <think>/</think>, true opens the template's think block and false
// writes it closed; a pack without them renders the plain prompt either way.
// effort (session log step 16): the template's reasoning sentence and where it sits
// (render_qwen_text_prompt); the route always passes one, nullopt keeps the fixed format.
[[nodiscard]] ChatGenerationPlanResult make_qwen_chat_generation_plan(
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
    std::size_t maximum_prompt_tokens = static_cast<std::size_t>(config_defaults::kChatMaxPromptTokens),
    std::size_t default_max_tokens = static_cast<std::size_t>(config_defaults::kChatDefaultMaxTokens),
    const SamplingDefaults& defaults = {}, std::span<const std::size_t> image_token_counts = {},
    bool thinking = true, std::optional<QwenEffortPlacement> effort = std::nullopt);

// Web search (spec 2026-10-27-web-search-design §2): make_qwen_chat_generation_plan's plan with the server's tools in
// the prompt and the search loop's rounds after the conversation (render_qwen_tool_prompt, encoded by
// encode_prompt_pieces), and the four Qwen tool tags output as their text (literal_tokens) for QwenToolCallParser. Text
// only: an image part is invalid_request (a request with images is served without search). A pack without the reasoning
// markers or the tool tags is tokenization_failed; a prompt past maximum_prompt_tokens is prompt_too_large.
[[nodiscard]] ChatGenerationPlanResult make_qwen_tool_generation_plan(
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer, std::span<const std::string> tools,
    std::span<const ToolRound> rounds, std::size_t maximum_prompt_tokens, std::size_t default_max_tokens,
    const SamplingDefaults& defaults, bool thinking, std::optional<QwenEffortPlacement> effort = std::nullopt);

// Web search, GLM-5.3 (phase 5): make_glm53_chat_generation_plan's plan with the server's tools and the search loop's
// rounds (render_glm53_tool_prompt, encoded by encode_prompt_pieces), and GLM's eight tool tags output as their text
// (literal_tokens) for GlmToolCallParser. The reply begins as reasoning (the prompt ends in <think>) and ends on
// find_glm53_end_of_turn_tokens - <|observation|> among them, the turn end after a call. A ChatML (raon1) pack, or a
// pack without the reasoning markers, the tool tags or all three turn ends, is tokenization_failed; an image part is
// invalid_request.
[[nodiscard]] ChatGenerationPlanResult make_glm53_tool_generation_plan(
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer, std::span<const std::string> tools,
    std::span<const ToolRound> rounds, std::size_t maximum_prompt_tokens, std::size_t default_max_tokens,
    const SamplingDefaults& defaults);

// Web search for raon-1-mini (owner 2026-10-28): the raon1 ChatML plan with the server's tools and the rounds
// (render_raon1_tool_prompt, encoded by encode_prompt_pieces) and Qwen's four tool tags output as their text for the
// Qwen call parser; `thinking` opens the reply's think block. A pack that is not raon-1-mini's ChatML one, or lacks the
// reasoning markers or the tags, is tokenization_failed; an image part is invalid_request.
[[nodiscard]] ChatGenerationPlanResult make_raon1_tool_generation_plan(
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer, std::span<const std::string> tools,
    std::span<const ToolRound> rounds, std::size_t maximum_prompt_tokens, std::size_t default_max_tokens,
    const SamplingDefaults& defaults, bool thinking);

// Web search for flint (owner 2026-10-28): the flint plan with the server's tools and the rounds (render_flint_tool_prompt,
// encoded by encode_prompt_pieces) and GLM's eight tool tags output as their text for the GLM call parser; the model opens
// its own think block (thinking off closes it in the prompt) and the reply ends on flint's four end-of-generation ids.
// A pack without flint's markers, its four ends or the eight tags is tokenization_failed; an image part is invalid_request.
[[nodiscard]] ChatGenerationPlanResult make_flint_tool_generation_plan(
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer, std::span<const std::string> tools,
    std::span<const ToolRound> rounds, std::size_t maximum_prompt_tokens, std::size_t default_max_tokens,
    const SamplingDefaults& defaults, bool thinking);

// Converts a schema-validated text request using only the fixed GLM-5.3
// profile. It neither interprets a model template nor contacts the network.
// Images never reach this profile (the manifest rules refuse launch.mmproj for
// glm53, Task 11): an image part closes the plan as invalid_request. The prompt
// ends in <think>, so the reply starts as reasoning when the pack has both think
// markers; it stops on find_glm53_end_of_turn_tokens.
[[nodiscard]] ChatGenerationPlanResult make_glm53_chat_generation_plan(
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
    std::size_t maximum_prompt_tokens = static_cast<std::size_t>(config_defaults::kChatMaxPromptTokens),
    std::size_t default_max_tokens = static_cast<std::size_t>(config_defaults::kChatDefaultMaxTokens),
    const SamplingDefaults& defaults = {}, std::span<const std::size_t> image_token_counts = {});

// Gemma 4 plan 1 (spec §2.5, §6.3-§6.5): a schema-validated text request through the fixed
// Gemma 4 template (render_gemma4_text_prompt, encode_gemma4_prompt). thinking writes <|think|>
// in the system turn; off closes the thought channel in the generation prompt. The pack must
// carry the Gemma 4 markers (find_gemma4_markers), else tokenization_failed; an image part is
// invalid_request (text only). The reasoning split is the thought channel, <|channel> and
// <channel|>, and the reply never starts inside it (starts_in_reasoning false in both modes).
[[nodiscard]] ChatGenerationPlanResult make_gemma4_chat_generation_plan(
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
    std::size_t maximum_prompt_tokens = static_cast<std::size_t>(config_defaults::kChatMaxPromptTokens),
    std::size_t default_max_tokens = static_cast<std::size_t>(config_defaults::kChatDefaultMaxTokens),
    const SamplingDefaults& defaults = {}, bool thinking = false);

// flint: a schema-validated text request through the GGUF's own template (render_flint_text_prompt,
// encode_flint_prompt). thinking leaves the generation prompt as the template writes it (<|assistant|>: the model opens
// <think> itself); off closes the block after it (<think></think>). The pack must carry flint's markers
// (find_flint_markers) and its four end-of-generation ids (find_flint_end_of_turn_tokens), else tokenization_failed;
// an image part is invalid_request (text only). The reasoning split is <think>/</think>, and the reply never starts
// inside it (starts_in_reasoning false in both modes).
[[nodiscard]] ChatGenerationPlanResult make_flint_chat_generation_plan(
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
    std::size_t maximum_prompt_tokens = static_cast<std::size_t>(config_defaults::kChatMaxPromptTokens),
    std::size_t default_max_tokens = static_cast<std::size_t>(config_defaults::kChatDefaultMaxTokens),
    const SamplingDefaults& defaults = {}, bool thinking = true);

// DeepSeek-V4.1-Flash on Maya's maya-ds4 (spec 2026-10-27-strata-prepare-design §7.2): the prompt DeepSeek's own
// encoder writes (render_deepseek_text_prompt; `effort` 1-100 is thinking mode, nullopt chat mode), its framing markers
// as single tokens (encode_deepseek_prompt). The reply's split is <think>/</think> and it begins as reasoning in
// thinking mode; it ends on <｜end▁of▁sentence｜> or <｜User｜>, both server stops (the engine stops on its EOS only, so
// the frontend sends STOP, as Maya's server does). A pack without the markers, the think pair or both turn ends is
// tokenization_failed; an image part is invalid_request (text only).
// A reasoning level (a request's reasoning_effort, or the card's reasoning.effort) as DeepSeek's effort budget, the way
// Maya's server maps it (serve/frontend.py EFFORT, deepseek_effort_kwargs): "none" is chat mode (nullopt), "low" 50,
// "medium" and "high" 75 (Maya's default, "high").
[[nodiscard]] std::optional<int> deepseek_effort_budget(std::string_view level);

[[nodiscard]] ChatGenerationPlanResult make_deepseek_chat_generation_plan(
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
    std::size_t maximum_prompt_tokens = static_cast<std::size_t>(config_defaults::kChatMaxPromptTokens),
    std::size_t default_max_tokens = static_cast<std::size_t>(config_defaults::kChatDefaultMaxTokens),
    const SamplingDefaults& defaults = {}, std::optional<int> effort = 75);

// MiniMax-M3.1-Flash (Maya's maya-mm3; owner 2026-10-28, under the MiniMax Community License): the prompt Maya's server
// renders (render_minimax_text_prompt), the template's literals that the pack holds as special tokens encoded as those
// tokens and any other as text (Maya's parse_special). The reply begins as reasoning unless thinking is off (Maya's
// parser: <mm:think> dropped, </mm:think> ends it, the newlines after it kept) when the pair are special tokens; without
// them the tags are text and the reply is not split. It ends on [e~[ or ]~b], both server stops (maya-mm3 stops on its
// EOS only). A pack without ]~!b[, ]~b] and [e~[ as special tokens is tokenization_failed; an image part is
// invalid_request (text only).
[[nodiscard]] ChatGenerationPlanResult make_minimax_chat_generation_plan(const ChatCompletionRequest& request,
                                                                         const TokenizerPack& tokenizer,
                                                                         std::size_t maximum_prompt_tokens,
                                                                         std::size_t default_max_tokens,
                                                                         const SamplingDefaults& defaults,
                                                                         const MinimaxThinking& thinking);
[[nodiscard]] ChatGenerationPlanResult make_raon1_chat_generation_plan(
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
    std::size_t maximum_prompt_tokens = static_cast<std::size_t>(config_defaults::kChatMaxPromptTokens),
    std::size_t default_max_tokens = static_cast<std::size_t>(config_defaults::kChatDefaultMaxTokens),
    const SamplingDefaults& defaults = {}, bool thinking = true);

}  // namespace secure_frontend
