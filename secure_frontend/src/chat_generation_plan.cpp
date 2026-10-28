#include "secure_frontend/chat_generation_plan.hpp"

#include "secure_frontend/qwen_prompt_renderer.hpp"
#include "secure_frontend/glm_prompt_renderer.hpp"
#include "secure_frontend/gemma4_prompt_renderer.hpp"
#include "secure_frontend/flint_prompt_renderer.hpp"
#include "secure_frontend/deepseek_prompt_renderer.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace secure_frontend {
namespace {

ChatGenerationPlanError error(const ChatGenerationPlanErrorCode code,
                              const char* const message) {
  return ChatGenerationPlanError{code, message};
}

// SP5 M1 Task 4 (parity row `sampling`): the plan's sampling is the request's
// own fields first, with unset entries filled from the defaults.sampling.*
// config rows.  A request field always wins; an unset config row stays unset
// so the engine keeps its own default.
SamplingParams merge_sampling(const SamplingParams& request, const SamplingDefaults& defaults) {
  SamplingParams out = request;
  const auto fill = [](std::optional<double>& field, const std::optional<double>& fallback) {
    if (!field.has_value()) field = fallback;
  };
  fill(out.temperature, defaults.temperature);
  fill(out.top_p, defaults.top_p);
  fill(out.min_p, defaults.min_p);
  fill(out.presence_penalty, defaults.presence_penalty);
  fill(out.repetition_penalty, defaults.repetition_penalty);
  fill(out.frequency_penalty, defaults.frequency_penalty);
  const auto fill_int = [](std::optional<std::int64_t>& field, const std::optional<std::int64_t>& fallback) {
    if (!field.has_value()) field = fallback;
  };
  fill_int(out.top_k, defaults.top_k);
  fill_int(out.penalty_last_n, defaults.penalty_last_n);
  // Config seed rows are validated positive and 32-bit capped (C-1), so the
  // widening cast to the wire's unsigned form is exact.
  if (!out.seed.has_value() && defaults.seed.has_value()) out.seed = static_cast<std::uint64_t>(*defaults.seed);
  return out;
}

// SP5 M1 Task 12: the request's image parts, counted in render order (messages
// outer, parts inner) — the same order the renderer consumes the counts in.
std::size_t count_image_parts(const ChatCompletionRequest& request) {
  std::size_t images = 0U;
  for (const auto& message : request.messages) {
    for (const auto& part : message.parts) {
      if (!part.image_data_url.empty()) ++images;
    }
  }
  return images;
}

// The single glm_pack engine serves either a GLM-template pack or a ChatML
// (raon-1-mini) pack. A ChatML pack carries <|im_start|> as exactly one CONTROL
// token AND carries no GLM turn marker (<|user|>): raon-1-mini's vocabulary has
// the former and none of the latter, while a GLM pack always has <|user|>. So
// this picks the raon1 renderer over render_glm53_text_prompt from the loaded
// pack alone — no config or engine change, and moving a model needs no recompile.
bool pack_is_raon1_chatml(const TokenizerPack& pack) {
  if (pack.tokens_by_id.size() != pack.token_types.size()) return false;
  bool im_start = false;
  for (std::size_t id = 0U; id < pack.tokens_by_id.size(); ++id) {
    if (pack.tokens_by_id[id] == "<|user|>") return false;  // a GLM-template pack, never raon1
    if (pack.tokens_by_id[id] != "<|im_start|>") continue;
    if (pack.token_types[id] != 3 || im_start) return false;  // must be exactly one CONTROL token
    im_start = true;
  }
  return im_start;
}

// The raon-1-mini ChatML plan: faithful ChatML framing (render_raon1_text_prompt),
// its four markers encoded as single CONTROL ids (encode_raon1_chatml_prompt), with
// the pack's <think>/</think> ids for the reply's reasoning split and <|im_end|>/
// <|endoftext|> as the end-of-turn stops. `thinking` opens the generation tail's think
// block, so the reply begins as reasoning (starts_in_reasoning = thinking).
ChatGenerationPlanResult make_raon1_chatml_plan(const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
                                                const std::size_t maximum_prompt_tokens,
                                                const std::size_t default_max_tokens, const SamplingDefaults& defaults,
                                                const bool thinking) {
  if (request.model.empty() || request.messages.empty() || maximum_prompt_tokens == 0U ||
      (request.max_tokens.has_value() && *request.max_tokens == 0U)) {
    return error(ChatGenerationPlanErrorCode::invalid_request,
                 "chat generation request is outside native serving limits");
  }
  if (count_image_parts(request) != 0U) {
    return error(ChatGenerationPlanErrorCode::invalid_request, "the raon1 profile cannot serve image parts");
  }
  const auto prompt = render_raon1_text_prompt(request.messages, thinking);
  if (!prompt) {
    return error(ChatGenerationPlanErrorCode::prompt_render_failed,
                 "chat messages cannot be represented by the fixed raon1 ChatML profile");
  }
  const auto encoded = encode_raon1_chatml_prompt(tokenizer, *prompt);
  const auto* token_ids = std::get_if<std::vector<std::size_t>>(&encoded);
  if (token_ids == nullptr) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed,
                 "rendered raon1 prompt cannot be encoded by the local tokenizer pack");
  }
  if (token_ids->empty() || token_ids->size() > maximum_prompt_tokens) {
    return error(ChatGenerationPlanErrorCode::prompt_too_large, "encoded chat prompt exceeds native serving limits");
  }
  return ChatGenerationPlan{request.model, *token_ids, request.max_tokens.value_or(default_max_tokens),
                            merge_sampling(request.sampling, defaults), find_qwen_think_tokens(tokenizer), thinking,
                            find_qwen_end_of_turn_tokens(tokenizer), std::nullopt, {}, {}};
}

}  // namespace

ChatGenerationPlanResult make_qwen_chat_generation_plan(
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
    const std::size_t maximum_prompt_tokens, const std::size_t default_max_tokens,
    const SamplingDefaults& defaults, const std::span<const std::size_t> image_token_counts,
    const bool thinking, const std::optional<QwenEffortPlacement> effort) {
  if (request.model.empty() || request.messages.empty() || maximum_prompt_tokens == 0U ||
      (request.max_tokens.has_value() && *request.max_tokens == 0U)) {
    return error(ChatGenerationPlanErrorCode::invalid_request,
                 "chat generation request is outside native serving limits");
  }
  // Ruling T12-f: every image part needs exactly one encoder answer, and no answer may
  // lack an image part — a mismatch is a route wiring bug, closed here fail-closed.
  if (count_image_parts(request) != image_token_counts.size()) {
    return error(ChatGenerationPlanErrorCode::invalid_request,
                 "image answers do not match the request's image parts");
  }

  // Issue 19: a Qwen3 pack's think markers select the template's reasoning tail; a pack
  // without them keeps the plain prompt (the tags then fail closed in output as before).
  const auto think_tokens = find_qwen_think_tokens(tokenizer);
  const QwenThinking tail = !think_tokens.has_value() ? QwenThinking::absent : thinking ? QwenThinking::on : QwenThinking::off;
  const auto prompt = render_qwen_text_prompt(request.messages, image_token_counts, tail, effort);
  if (!prompt) {
    return error(ChatGenerationPlanErrorCode::prompt_render_failed,
                 "chat messages cannot be represented by the fixed prompt format");
  }

  // The renderer alone owns these two fixed control markers.  User content
  // was rejected by the renderer before this narrow encoder is reached.
  const auto encoded = encode_qwen_chatml_prompt(tokenizer, *prompt);
  const auto* token_ids = std::get_if<std::vector<std::size_t>>(&encoded);
  if (token_ids == nullptr) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed,
                 "rendered chat prompt cannot be encoded by the local tokenizer pack");
  }
  if (token_ids->empty() || token_ids->size() > maximum_prompt_tokens) {
    return error(ChatGenerationPlanErrorCode::prompt_too_large,
                 "encoded chat prompt exceeds native serving limits");
  }

  return ChatGenerationPlan{request.model, *token_ids, request.max_tokens.value_or(default_max_tokens),
                            merge_sampling(request.sampling, defaults), think_tokens, tail == QwenThinking::on,
                            find_qwen_end_of_turn_tokens(tokenizer), std::nullopt, {}, {}};
}

ChatGenerationPlanResult make_qwen_tool_generation_plan(const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
                                                        const std::span<const std::string> tools, const std::span<const ToolRound> rounds,
                                                        const std::size_t maximum_prompt_tokens, const std::size_t default_max_tokens,
                                                        const SamplingDefaults& defaults, const bool thinking,
                                                        const std::optional<QwenEffortPlacement> effort) {
  if (request.model.empty() || request.messages.empty() || maximum_prompt_tokens == 0U ||
      (request.max_tokens.has_value() && *request.max_tokens == 0U)) {
    return error(ChatGenerationPlanErrorCode::invalid_request, "chat generation request is outside native serving limits");
  }
  if (count_image_parts(request) != 0U) {
    return error(ChatGenerationPlanErrorCode::invalid_request, "a request with images is served without search");
  }
  // The pack must carry the reasoning markers and the four tool tags before anything is rendered for it.
  const auto think_tokens = find_qwen_think_tokens(tokenizer);
  const auto tags = find_qwen_tool_tokens(tokenizer);
  if (!think_tokens.has_value() || !tags.has_value()) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed, "the local tokenizer pack does not carry Qwen's tool tags");
  }
  const QwenThinking tail = thinking ? QwenThinking::on : QwenThinking::off;
  const auto pieces = render_qwen_tool_prompt(request.messages, tools, rounds, tail, effort);
  if (!pieces) {
    return error(ChatGenerationPlanErrorCode::prompt_render_failed, "chat messages cannot be represented by the fixed prompt format");
  }
  const auto encoded = encode_prompt_pieces(tokenizer, *pieces);
  const auto* token_ids = std::get_if<std::vector<std::size_t>>(&encoded);
  if (token_ids == nullptr) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed, "rendered chat prompt cannot be encoded by the local tokenizer pack");
  }
  if (token_ids->empty() || token_ids->size() > maximum_prompt_tokens) {
    return error(ChatGenerationPlanErrorCode::prompt_too_large, "encoded chat prompt exceeds native serving limits");
  }
  std::vector<std::pair<std::size_t, std::string>> literals{{tags->call_open, "<tool_call>"},
                                                            {tags->call_close, "</tool_call>"},
                                                            {tags->response_open, "<tool_response>"},
                                                            {tags->response_close, "</tool_response>"}};
  return ChatGenerationPlan{request.model, *token_ids, request.max_tokens.value_or(default_max_tokens),
                            merge_sampling(request.sampling, defaults), think_tokens, thinking,
                            find_qwen_end_of_turn_tokens(tokenizer), std::nullopt, {}, std::move(literals)};
}

ChatGenerationPlanResult make_glm53_tool_generation_plan(const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
                                                         const std::span<const std::string> tools, const std::span<const ToolRound> rounds,
                                                         const std::size_t maximum_prompt_tokens, const std::size_t default_max_tokens,
                                                         const SamplingDefaults& defaults) {
  if (request.model.empty() || request.messages.empty() || maximum_prompt_tokens == 0U ||
      (request.max_tokens.has_value() && *request.max_tokens == 0U)) {
    return error(ChatGenerationPlanErrorCode::invalid_request, "chat generation request is outside native serving limits");
  }
  if (count_image_parts(request) != 0U) return error(ChatGenerationPlanErrorCode::invalid_request, "the GLM profile cannot serve image parts");
  // A GLM pack with its reasoning markers, its tool tags and its three turn ends (a ChatML pack has none of the tags).
  const auto think_tokens = find_qwen_think_tokens(tokenizer);
  auto tags = find_glm53_tool_tokens(tokenizer);
  auto stops = find_glm53_end_of_turn_tokens(tokenizer);
  if (pack_is_raon1_chatml(tokenizer) || !think_tokens.has_value() || !tags.has_value() || stops.size() != 3U) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed, "the local tokenizer pack does not carry GLM's tool tags");
  }
  const auto pieces = render_glm53_tool_prompt(request.messages, tools, rounds);
  if (!pieces) return error(ChatGenerationPlanErrorCode::prompt_render_failed, "chat messages cannot be represented by the fixed GLM profile");
  const auto encoded = encode_prompt_pieces(tokenizer, *pieces);
  const auto* token_ids = std::get_if<std::vector<std::size_t>>(&encoded);
  if (token_ids == nullptr) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed, "rendered GLM prompt cannot be encoded by the local tokenizer pack");
  }
  if (token_ids->empty() || token_ids->size() > maximum_prompt_tokens) {
    return error(ChatGenerationPlanErrorCode::prompt_too_large, "encoded chat prompt exceeds native serving limits");
  }
  return ChatGenerationPlan{request.model, *token_ids, request.max_tokens.value_or(default_max_tokens),
                            merge_sampling(request.sampling, defaults), think_tokens, true, std::move(stops), std::nullopt, {},
                            std::move(*tags)};
}

ChatGenerationPlanResult make_raon1_tool_generation_plan(const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
                                                         const std::span<const std::string> tools, const std::span<const ToolRound> rounds,
                                                         const std::size_t maximum_prompt_tokens, const std::size_t default_max_tokens,
                                                         const SamplingDefaults& defaults, const bool thinking) {
  if (request.model.empty() || request.messages.empty() || maximum_prompt_tokens == 0U ||
      (request.max_tokens.has_value() && *request.max_tokens == 0U)) {
    return error(ChatGenerationPlanErrorCode::invalid_request, "chat generation request is outside native serving limits");
  }
  if (count_image_parts(request) != 0U) return error(ChatGenerationPlanErrorCode::invalid_request, "the raon1 profile cannot serve image parts");
  // raon-1-mini's ChatML pack with its reasoning markers and Qwen's four tool tags, before anything is rendered for it
  const auto think_tokens = find_qwen_think_tokens(tokenizer);
  const auto tags = find_qwen_tool_tokens(tokenizer);
  if (!pack_is_raon1_chatml(tokenizer) || !think_tokens.has_value() || !tags.has_value()) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed, "the local tokenizer pack does not carry raon-1-mini's tool tags");
  }
  const auto pieces = render_raon1_tool_prompt(request.messages, tools, rounds, thinking);
  if (!pieces) {
    return error(ChatGenerationPlanErrorCode::prompt_render_failed, "chat messages cannot be represented by the fixed raon1 ChatML profile");
  }
  const auto encoded = encode_prompt_pieces(tokenizer, *pieces);
  const auto* token_ids = std::get_if<std::vector<std::size_t>>(&encoded);
  if (token_ids == nullptr) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed, "rendered raon1 prompt cannot be encoded by the local tokenizer pack");
  }
  if (token_ids->empty() || token_ids->size() > maximum_prompt_tokens) {
    return error(ChatGenerationPlanErrorCode::prompt_too_large, "encoded chat prompt exceeds native serving limits");
  }
  std::vector<std::pair<std::size_t, std::string>> literals{{tags->call_open, "<tool_call>"},
                                                            {tags->call_close, "</tool_call>"},
                                                            {tags->response_open, "<tool_response>"},
                                                            {tags->response_close, "</tool_response>"}};
  return ChatGenerationPlan{request.model, *token_ids, request.max_tokens.value_or(default_max_tokens),
                            merge_sampling(request.sampling, defaults), think_tokens, thinking,
                            find_qwen_end_of_turn_tokens(tokenizer), std::nullopt, {}, std::move(literals)};
}

ChatGenerationPlanResult make_flint_tool_generation_plan(const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
                                                         const std::span<const std::string> tools, const std::span<const ToolRound> rounds,
                                                         const std::size_t maximum_prompt_tokens, const std::size_t default_max_tokens,
                                                         const SamplingDefaults& defaults, const bool thinking) {
  if (request.model.empty() || request.messages.empty() || maximum_prompt_tokens == 0U ||
      (request.max_tokens.has_value() && *request.max_tokens == 0U)) {
    return error(ChatGenerationPlanErrorCode::invalid_request, "chat generation request is outside native serving limits");
  }
  if (count_image_parts(request) != 0U) return error(ChatGenerationPlanErrorCode::invalid_request, "the flint profile cannot serve image parts");
  // flint's pack: its markers, its four ends of generation and GLM's eight tool tags
  const auto markers = find_flint_markers(tokenizer);
  auto stops = find_flint_end_of_turn_tokens(tokenizer);
  auto tags = find_glm53_tool_tokens(tokenizer);
  if (!markers.has_value() || stops.size() != 4U || !tags.has_value()) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed, "the local tokenizer pack does not carry flint's tool tags");
  }
  const auto pieces = render_flint_tool_prompt(request.messages, tools, rounds, thinking);
  if (!pieces) return error(ChatGenerationPlanErrorCode::prompt_render_failed, "chat messages cannot be represented by the fixed flint template");
  const auto encoded = encode_prompt_pieces(tokenizer, *pieces);
  const auto* token_ids = std::get_if<std::vector<std::size_t>>(&encoded);
  if (token_ids == nullptr) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed, "rendered flint prompt cannot be encoded by the local tokenizer pack");
  }
  if (token_ids->empty() || token_ids->size() > maximum_prompt_tokens) {
    return error(ChatGenerationPlanErrorCode::prompt_too_large, "encoded chat prompt exceeds native serving limits");
  }
  // as the text plan: the model opens <think> itself (thinking off closed it in the prompt)
  return ChatGenerationPlan{request.model, *token_ids, request.max_tokens.value_or(default_max_tokens),
                            merge_sampling(request.sampling, defaults), ThinkTokens{markers->think_open, markers->think_close},
                            false, std::move(stops), std::nullopt, {}, std::move(*tags)};
}

ChatGenerationPlanResult make_glm53_chat_generation_plan(
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
    const std::size_t maximum_prompt_tokens, const std::size_t default_max_tokens,
    const SamplingDefaults& defaults, const std::span<const std::size_t> image_token_counts) {
  // The same glm_pack engine serves GLM-template and ChatML (raon-1-mini) packs; a ChatML
  // pack takes the faithful raon1 ChatML plan, leaving the GLM path below byte-for-byte for
  // GLM packs. The template follows the loaded pack, so moving a model needs no recompile.
  // Under the glm53 profile the schema refuses reasoning_effort (per_request_reasoning_effort
  // is false), so this fallback renders raon1's reasoning-disabled tail; the raon1 profile, which
  // does honor reasoning_effort, routes through make_raon1_chat_generation_plan instead.
  if (pack_is_raon1_chatml(tokenizer)) {
    return make_raon1_chatml_plan(request, tokenizer, maximum_prompt_tokens, default_max_tokens, defaults, false);
  }
  if (request.model.empty() || request.messages.empty() || maximum_prompt_tokens == 0U ||
      (request.max_tokens.has_value() && *request.max_tokens == 0U)) {
    return error(ChatGenerationPlanErrorCode::invalid_request,
                 "chat generation request is outside native serving limits");
  }
  // The glm53 profile never sees images (Task 11's manifest rules refuse launch.mmproj,
  // and the schema gate follows the composed capability): any image part or answer
  // closes the plan.
  if (count_image_parts(request) != 0U || !image_token_counts.empty()) {
    return error(ChatGenerationPlanErrorCode::invalid_request,
                 "the GLM profile cannot serve image parts");
  }
  const auto prompt = render_glm53_text_prompt(request.messages);
  if (!prompt) return error(ChatGenerationPlanErrorCode::prompt_render_failed,
                            "chat messages cannot be represented by the fixed GLM profile");
  const auto encoded = encode_glm53_prompt(tokenizer, *prompt);
  const auto* token_ids = std::get_if<std::vector<std::size_t>>(&encoded);
  if (token_ids == nullptr) return error(ChatGenerationPlanErrorCode::tokenization_failed,
                                          "rendered GLM prompt cannot be encoded by the local tokenizer pack");
  if (token_ids->empty() || token_ids->size() > maximum_prompt_tokens) {
    return error(ChatGenerationPlanErrorCode::prompt_too_large,
                 "encoded chat prompt exceeds native serving limits");
  }
  // GLM-5.3's template opens <think> at the end of every generation prompt (Maya's server renders it so whatever
  // it is told), so the reply is reasoning until </think>, as Maya's OutputParser(thinking=True) reads it, and the
  // engine ends the turn on <|endoftext|>, <|user|> or <|observation|> (printed as a T record before DONE). A pack
  // without both think markers keeps the plain reply.
  const auto think_tokens = find_qwen_think_tokens(tokenizer);
  return ChatGenerationPlan{request.model, *token_ids, request.max_tokens.value_or(default_max_tokens),
                            merge_sampling(request.sampling, defaults), think_tokens, think_tokens.has_value(),
                            find_glm53_end_of_turn_tokens(tokenizer), std::nullopt, {}, {}};
}

ChatGenerationPlanResult make_gemma4_tool_generation_plan(const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
                                                          const std::span<const std::string> tools, const std::span<const ToolRound> rounds,
                                                          const std::size_t maximum_prompt_tokens, const std::size_t default_max_tokens,
                                                          const SamplingDefaults& defaults, const bool thinking) {
  if (request.model.empty() || request.messages.empty() || maximum_prompt_tokens == 0U ||
      (request.max_tokens.has_value() && *request.max_tokens == 0U)) {
    return error(ChatGenerationPlanErrorCode::invalid_request, "chat generation request is outside native serving limits");
  }
  if (count_image_parts(request) != 0U) return error(ChatGenerationPlanErrorCode::invalid_request, "the Gemma 4 profile cannot serve image parts");
  // Gemma's pack: its markers, its two turn ends and the seven tool tags the prompt and the reply use
  const auto markers = find_gemma4_markers(tokenizer);
  auto stops = find_gemma4_end_of_turn_tokens(tokenizer);
  std::vector<std::optional<std::size_t>> tags;
  for (const std::string_view tag : {"<|tool>", "<tool|>", "<|tool_call>", "<tool_call|>", "<|tool_response>", "<tool_response|>", "<|\"|>"}) {
    tags.push_back(find_special_token(tokenizer, tag));
  }
  if (!markers.has_value() || stops.size() != 2U ||
      std::any_of(tags.begin(), tags.end(), [](const std::optional<std::size_t>& id) { return !id.has_value(); })) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed, "the local tokenizer pack does not carry Gemma 4's tool tags");
  }
  const auto pieces = render_gemma4_tool_prompt(request.messages, tools, rounds, thinking);
  if (!pieces) {
    return error(ChatGenerationPlanErrorCode::prompt_render_failed, "chat messages cannot be represented by the fixed Gemma 4 template");
  }
  const auto encoded = encode_prompt_pieces(tokenizer, *pieces);
  const auto* token_ids = std::get_if<std::vector<std::size_t>>(&encoded);
  if (token_ids == nullptr) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed, "rendered Gemma 4 prompt cannot be encoded by the local tokenizer pack");
  }
  if (token_ids->empty() || token_ids->size() > maximum_prompt_tokens) {
    return error(ChatGenerationPlanErrorCode::prompt_too_large, "encoded chat prompt exceeds native serving limits");
  }
  // as the text plan, plus the call's tags as their text and the server's stop on <|tool_response>
  std::vector<std::pair<std::size_t, std::string>> literals{{*tags[2], "<|tool_call>"}, {*tags[3], "<tool_call|>"}, {*tags[6], "<|\"|>"}};
  return ChatGenerationPlan{request.model, *token_ids, request.max_tokens.value_or(default_max_tokens),
                            merge_sampling(request.sampling, defaults), ThinkTokens{markers->channel_open, markers->channel_close},
                            false, std::move(stops), std::string{"thought"}, std::vector<std::size_t>{*tags[4]}, std::move(literals)};
}

ChatGenerationPlanResult make_gemma4_chat_generation_plan(const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
                                                          const std::size_t maximum_prompt_tokens,
                                                          const std::size_t default_max_tokens, const SamplingDefaults& defaults,
                                                          const bool thinking) {
  if (request.model.empty() || request.messages.empty() || maximum_prompt_tokens == 0U ||
      (request.max_tokens.has_value() && *request.max_tokens == 0U)) {
    return error(ChatGenerationPlanErrorCode::invalid_request,
                 "chat generation request is outside native serving limits");
  }
  // Gemma 4 plan 1 is text only (vision_capable false, Task 10): an image part closes the plan.
  if (count_image_parts(request) != 0U) {
    return error(ChatGenerationPlanErrorCode::invalid_request, "the Gemma 4 profile cannot serve image parts");
  }
  // The pack must be Gemma's (spm_bpe, every marker exactly once with its GGUF type) before
  // anything is rendered for it.
  const auto markers = find_gemma4_markers(tokenizer);
  if (!markers.has_value()) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed,
                 "the local tokenizer pack does not carry the Gemma 4 markers");
  }
  const auto prompt = render_gemma4_text_prompt(request.messages, thinking);
  if (!prompt) {
    return error(ChatGenerationPlanErrorCode::prompt_render_failed,
                 "chat messages cannot be represented by the fixed Gemma 4 template");
  }
  const auto encoded = encode_gemma4_prompt(tokenizer, *prompt);
  const auto* token_ids = std::get_if<std::vector<std::size_t>>(&encoded);
  if (token_ids == nullptr) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed,
                 "rendered Gemma 4 prompt cannot be encoded by the local tokenizer pack");
  }
  if (token_ids->empty() || token_ids->size() > maximum_prompt_tokens) {
    return error(ChatGenerationPlanErrorCode::prompt_too_large,
                 "encoded chat prompt exceeds native serving limits");
  }
  // The reasoning split is the thought channel in both modes. The reply never starts inside it:
  // thinking off closed it in the prompt, and thinking on leaves the model to open it. Task 9: the
  // channel's name line must be "thought", and <turn|> and <eos> end the reply (both are CONTROL
  // tokens the markers check already found exactly once).
  auto stop_tokens = find_gemma4_end_of_turn_tokens(tokenizer);
  if (stop_tokens.size() != 2U) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed,
                 "the local tokenizer pack does not carry the Gemma 4 markers");
  }
  return ChatGenerationPlan{request.model, *token_ids, request.max_tokens.value_or(default_max_tokens),
                            merge_sampling(request.sampling, defaults), ThinkTokens{markers->channel_open, markers->channel_close},
                            false, std::move(stop_tokens), std::string{"thought"}, {}, {}};
}

ChatGenerationPlanResult make_flint_chat_generation_plan(const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
                                                         const std::size_t maximum_prompt_tokens,
                                                         const std::size_t default_max_tokens, const SamplingDefaults& defaults,
                                                         const bool thinking) {
  if (request.model.empty() || request.messages.empty() || maximum_prompt_tokens == 0U ||
      (request.max_tokens.has_value() && *request.max_tokens == 0U)) {
    return error(ChatGenerationPlanErrorCode::invalid_request,
                 "chat generation request is outside native serving limits");
  }
  if (count_image_parts(request) != 0U) {
    return error(ChatGenerationPlanErrorCode::invalid_request, "the flint profile cannot serve image parts");
  }
  // The pack must be flint's (flint_bpe, every marker exactly once with its GGUF type, the four end-of-generation ids)
  // before anything is rendered for it: the engine stops on those four, and each must end the reply here too.
  const auto markers = find_flint_markers(tokenizer);
  auto stop_tokens = find_flint_end_of_turn_tokens(tokenizer);
  if (!markers.has_value() || stop_tokens.size() != 4U) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed, "the local tokenizer pack does not carry the flint markers");
  }
  const auto prompt = render_flint_text_prompt(request.messages, thinking);
  if (!prompt) {
    return error(ChatGenerationPlanErrorCode::prompt_render_failed,
                 "chat messages cannot be represented by the fixed flint template");
  }
  const auto encoded = encode_flint_prompt(tokenizer, *prompt);
  const auto* token_ids = std::get_if<std::vector<std::size_t>>(&encoded);
  if (token_ids == nullptr) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed,
                 "rendered flint prompt cannot be encoded by the local tokenizer pack");
  }
  if (token_ids->empty() || token_ids->size() > maximum_prompt_tokens) {
    return error(ChatGenerationPlanErrorCode::prompt_too_large, "encoded chat prompt exceeds native serving limits");
  }
  // Thinking on leaves the model to open <think>; off closed it in the prompt. Either way the reply starts outside it.
  return ChatGenerationPlan{request.model, *token_ids, request.max_tokens.value_or(default_max_tokens),
                            merge_sampling(request.sampling, defaults), ThinkTokens{markers->think_open, markers->think_close},
                            false, std::move(stop_tokens), std::nullopt, {}, {}};
}

std::optional<int> deepseek_effort_budget(const std::string_view level) {
  if (level == "none") return std::nullopt;  // chat mode (Maya: none / off / disabled set enable_thinking false)
  return level == "low" ? 50 : 75;           // ds41_encoding.py REASONING_EFFORT_MAPPINGS; medium is Maya's "high"
}

ChatGenerationPlanResult make_deepseek_chat_generation_plan(const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
                                                            const std::size_t maximum_prompt_tokens,
                                                            const std::size_t default_max_tokens, const SamplingDefaults& defaults,
                                                            const std::optional<int> effort) {
  if (request.model.empty() || request.messages.empty() || maximum_prompt_tokens == 0U ||
      (request.max_tokens.has_value() && *request.max_tokens == 0U) || (effort && (*effort < 1 || *effort > 100))) {
    return error(ChatGenerationPlanErrorCode::invalid_request, "chat generation request is outside native serving limits");
  }
  if (count_image_parts(request) != 0U) {
    return error(ChatGenerationPlanErrorCode::invalid_request, "the DeepSeek profile cannot serve image parts");
  }
  // The pack must carry DeepSeek's reasoning pair and both turn ends before anything is rendered for it.
  const auto think_tokens = find_qwen_think_tokens(tokenizer);
  auto stops = find_deepseek_end_of_turn_tokens(tokenizer);
  if (!think_tokens.has_value() || stops.size() != 2U) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed,
                 "the local tokenizer pack does not carry DeepSeek's markers");
  }
  const auto prompt = render_deepseek_text_prompt(request.messages, effort);
  if (!prompt) {
    return error(ChatGenerationPlanErrorCode::prompt_render_failed,
                 "chat messages cannot be represented by the fixed DeepSeek profile");
  }
  const auto encoded = encode_deepseek_prompt(tokenizer, *prompt);
  const auto* token_ids = std::get_if<std::vector<std::size_t>>(&encoded);
  if (token_ids == nullptr) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed,
                 "rendered DeepSeek prompt cannot be encoded by the local tokenizer pack");
  }
  if (token_ids->empty() || token_ids->size() > maximum_prompt_tokens) {
    return error(ChatGenerationPlanErrorCode::prompt_too_large, "encoded chat prompt exceeds native serving limits");
  }
  // Thinking mode leaves <think> open after the last header (the reply begins as reasoning); chat mode closed it.
  return ChatGenerationPlan{request.model, *token_ids, request.max_tokens.value_or(default_max_tokens),
                            merge_sampling(request.sampling, defaults), think_tokens, effort.has_value(),
                            std::vector<std::size_t>{}, std::nullopt, std::move(stops), {}};
}

ChatGenerationPlanResult make_deepseek_tool_generation_plan(const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
                                                            const std::span<const std::string> tools,
                                                            const std::span<const ToolRound> rounds,
                                                            const std::size_t maximum_prompt_tokens,
                                                            const std::size_t default_max_tokens, const SamplingDefaults& defaults,
                                                            const std::optional<int> effort) {
  if (request.model.empty() || request.messages.empty() || maximum_prompt_tokens == 0U ||
      (request.max_tokens.has_value() && *request.max_tokens == 0U) || (effort && (*effort < 1 || *effort > 100))) {
    return error(ChatGenerationPlanErrorCode::invalid_request, "chat generation request is outside native serving limits");
  }
  if (count_image_parts(request) != 0U) {
    return error(ChatGenerationPlanErrorCode::invalid_request, "the DeepSeek profile cannot serve image parts");
  }
  // the text plan's markers, plus ｜DSML｜: the tag every call is written in
  const auto think_tokens = find_qwen_think_tokens(tokenizer);
  auto stops = find_deepseek_end_of_turn_tokens(tokenizer);
  const std::string dsml{"\xEF\xBD\x9C" "DSML\xEF\xBD\x9C"};
  const auto dsml_id = find_special_token(tokenizer, dsml);
  if (!think_tokens.has_value() || stops.size() != 2U || !dsml_id.has_value()) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed, "the local tokenizer pack does not carry DeepSeek's tool tag");
  }
  const auto pieces = render_deepseek_tool_prompt(request.messages, tools, rounds, effort);
  if (!pieces) {
    return error(ChatGenerationPlanErrorCode::prompt_render_failed, "chat messages cannot be represented by the fixed DeepSeek profile");
  }
  const auto encoded = encode_prompt_pieces(tokenizer, *pieces);
  const auto* token_ids = std::get_if<std::vector<std::size_t>>(&encoded);
  if (token_ids == nullptr) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed, "rendered DeepSeek prompt cannot be encoded by the local tokenizer pack");
  }
  if (token_ids->empty() || token_ids->size() > maximum_prompt_tokens) {
    return error(ChatGenerationPlanErrorCode::prompt_too_large, "encoded chat prompt exceeds native serving limits");
  }
  return ChatGenerationPlan{request.model, *token_ids, request.max_tokens.value_or(default_max_tokens),
                            merge_sampling(request.sampling, defaults), think_tokens, effort.has_value(),
                            std::vector<std::size_t>{}, std::nullopt, std::move(stops),
                            std::vector<std::pair<std::size_t, std::string>>{{*dsml_id, dsml}}};
}

namespace {

// MiniMax's plan for a plain chat (no tools) or a search round (its tools and the rounds so far).
ChatGenerationPlanResult minimax_plan(const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
                                      const std::optional<std::span<const std::string>> tools,
                                      const std::span<const ToolRound> rounds, const std::size_t maximum_prompt_tokens,
                                      const std::size_t default_max_tokens, const SamplingDefaults& defaults,
                                      const MinimaxThinking& thinking) {
  if (request.model.empty() || request.messages.empty() || maximum_prompt_tokens == 0U ||
      (request.max_tokens.has_value() && *request.max_tokens == 0U)) {
    return error(ChatGenerationPlanErrorCode::invalid_request, "chat generation request is outside native serving limits");
  }
  if (count_image_parts(request) != 0U) {
    return error(ChatGenerationPlanErrorCode::invalid_request, "the MiniMax profile cannot serve image parts");
  }
  // The literals the pack holds as special tokens; the turn framing must be among them. With tools the namespace too:
  // where it is a special token it is output as its text, for the call reader.
  std::vector<std::string_view> special;
  for (const std::string_view literal : kMinimaxLiterals) {
    if (find_special_token(tokenizer, literal).has_value()) special.push_back(literal);
  }
  const auto ns = tools ? find_special_token(tokenizer, kMinimaxToolNamespace) : std::nullopt;
  if (ns) special.push_back(kMinimaxToolNamespace);
  const auto turn_end = find_special_token(tokenizer, "[e~[");
  const auto turn_start = find_special_token(tokenizer, "]~b]");
  if (!find_special_token(tokenizer, "]~!b[") || !turn_end || !turn_start) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed, "the local tokenizer pack does not carry MiniMax's turn framing");
  }
  const auto pieces = tools ? render_minimax_tool_prompt(request.messages, *tools, rounds, thinking, special)
                            : render_minimax_text_prompt(request.messages, thinking, special);
  if (!pieces) {
    return error(ChatGenerationPlanErrorCode::prompt_render_failed, "chat messages cannot be represented by MiniMax's template");
  }
  const auto encoded = encode_prompt_pieces(tokenizer, *pieces);
  const auto* token_ids = std::get_if<std::vector<std::size_t>>(&encoded);
  if (token_ids == nullptr) {
    return error(ChatGenerationPlanErrorCode::tokenization_failed, "rendered MiniMax prompt cannot be encoded by the local tokenizer pack");
  }
  if (token_ids->empty() || token_ids->size() > maximum_prompt_tokens) {
    return error(ChatGenerationPlanErrorCode::prompt_too_large, "encoded chat prompt exceeds native serving limits");
  }
  // Maya's parser starts in the reasoning unless thinking is off (OutputParser(thinking=...)); with the pair as text there
  // is no split to make.
  std::optional<ThinkTokens> think_tokens;
  const auto open = find_special_token(tokenizer, "<mm:think>");
  const auto close = find_special_token(tokenizer, "</mm:think>");
  if (open && close) think_tokens = ThinkTokens{*open, *close};
  ChatGenerationPlan plan{request.model, *token_ids, request.max_tokens.value_or(default_max_tokens),
                          merge_sampling(request.sampling, defaults), think_tokens, think_tokens.has_value() && !thinking.off,
                          std::vector<std::size_t>{}, std::nullopt, std::vector<std::size_t>{*turn_end, *turn_start}, {}};
  if (ns) plan.literal_tokens.emplace_back(*ns, std::string{kMinimaxToolNamespace});
  plan.keeps_newlines_after_think = true;
  return plan;
}

}  // namespace

ChatGenerationPlanResult make_minimax_chat_generation_plan(const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
                                                           const std::size_t maximum_prompt_tokens,
                                                           const std::size_t default_max_tokens, const SamplingDefaults& defaults,
                                                           const MinimaxThinking& thinking) {
  return minimax_plan(request, tokenizer, std::nullopt, {}, maximum_prompt_tokens, default_max_tokens, defaults, thinking);
}

ChatGenerationPlanResult make_minimax_tool_generation_plan(const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
                                                           const std::span<const std::string> tools,
                                                           const std::span<const ToolRound> rounds,
                                                           const std::size_t maximum_prompt_tokens,
                                                           const std::size_t default_max_tokens, const SamplingDefaults& defaults,
                                                           const MinimaxThinking& thinking) {
  return minimax_plan(request, tokenizer, tools, rounds, maximum_prompt_tokens, default_max_tokens, defaults, thinking);
}

ChatGenerationPlanResult make_raon1_chat_generation_plan(const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
                                                         const std::size_t maximum_prompt_tokens,
                                                         const std::size_t default_max_tokens, const SamplingDefaults& defaults,
                                                         const bool thinking) {
  // raon-1-mini rides the glm_pack engine with its own ChatML template (KRAFTON's
  // chat_template.jinja), not the deprecated unified-engine placeholder. `thinking` opens the
  // generation tail's think block (the route derives it from reasoning_effort; KRAFTON's own
  // default is thinking on, so a caller that does not pass it gets an open block).
  return make_raon1_chatml_plan(request, tokenizer, maximum_prompt_tokens, default_max_tokens, defaults, thinking);
}

}  // namespace secure_frontend
