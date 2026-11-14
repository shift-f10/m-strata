#include "secure_frontend/embedded_template_plan.hpp"

#include "secure_frontend/json.hpp"
#include "secure_frontend/prompt_builder.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace secure_frontend {
namespace {

constexpr std::size_t kContextBytes = 4U << 20;
JsonValue text(std::string_view value) { return JsonValue::make_string(std::string(value)); }
JsonValue object(std::vector<JsonMember> values) { return JsonValue::make_object(std::move(values)); }

JsonValue message(std::string_view role, std::string_view content) {
  return object({{"role", text(role)}, {"content", text(content)}});
}

bool add_round(std::vector<JsonValue>& messages, const ToolRound& round, std::size_t& next_id) {
  if (round.calls.empty() || round.calls.size() != round.results.size()) return false;
  std::vector<JsonValue> calls;
  std::vector<std::string> ids;
  for (const auto& call : round.calls) {
    ids.push_back("call_" + std::to_string(next_id++));
    std::vector<JsonMember> arguments;
    for (const auto& argument : call.arguments) arguments.push_back({argument.name, argument.value});
    calls.push_back(object({{"id", text(ids.back())}, {"type", text("function")},
        {"function", object({{"name", text(call.name)}, {"arguments", object(std::move(arguments))}})}}));
  }
  messages.push_back(object({{"role", text("assistant")}, {"content", text(round.content)},
      {"reasoning_content", text(round.reasoning)}, {"tool_calls", JsonValue::make_array(std::move(calls))}}));
  for (std::size_t i = 0; i < round.calls.size(); ++i) {
    messages.push_back(object({{"role", text("tool")}, {"name", text(round.calls[i].name)},
        {"tool_call_id", text(ids[i])}, {"content", text(round.results[i])}}));
  }
  return true;
}

ChatGenerationPlanError failure(ChatGenerationPlanErrorCode code, const char* why) { return {code, why}; }

}  // namespace

std::optional<std::string> embedded_template_context(
    const EmbeddedTemplateSelection& selected, const ChatCompletionRequest& request,
    const std::span<const std::string> tools, const std::span<const ToolRound> trailing,
    const bool thinking, const std::string_view effort) {
  std::vector<JsonValue> messages;
  std::size_t next_id = 0;
  for (const auto& item : request.messages) {
    for (const auto& part : item.parts) if (!part.image_data_url.empty()) return std::nullopt;
    if (item.role != "system" && item.role != "user" && item.role != "assistant") return std::nullopt;
    for (const auto& round : item.rounds) if (!add_round(messages, round, next_id)) return std::nullopt;
    if (!item.content.empty() || item.rounds.empty()) messages.push_back(message(item.role, item.content));
  }
  for (const auto& round : trailing) if (!add_round(messages, round, next_id)) return std::nullopt;
  std::vector<JsonValue> declarations;
  for (const auto& tool : tools) {
    auto parsed = parse_json(tool, {.max_bytes = kContextBytes, .max_depth = 32U, .max_container_entries = 4096U,
                                   .max_string_bytes = kContextBytes, .allow_fractions = true});
    const auto* value = std::get_if<JsonValue>(&parsed);
    if (value == nullptr || value->if_object() == nullptr) return std::nullopt;
    declarations.push_back(object({{"type", text("function")}, {"function", *value}}));
  }
  JsonValue choice = text(request.tool_choice);
  if (request.named_tool) choice = object({{"type", text("function")}, {"function", object({{"name", text(*request.named_tool)}})}});
  std::vector<JsonMember> context{{"messages", JsonValue::make_array(std::move(messages))},
      {"tools", JsonValue::make_array(std::move(declarations))}, {"add_generation_prompt", JsonValue::make_boolean(true)},
      {"enable_thinking", JsonValue::make_boolean(thinking)}, {"thinking", JsonValue::make_boolean(thinking)},
      {"reasoning_effort", text(thinking ? effort : "none")}, {"tool_choice", std::move(choice)},
      {"parallel_tool_calls", JsonValue::make_boolean(request.parallel_tool_calls)}};
  if (selected.bos_token) context.push_back({"bos_token", text(*selected.bos_token)});
  if (selected.eos_token) context.push_back({"eos_token", text(*selected.eos_token)});
  auto encoded = python_json_text(object(std::move(context)));
  if (encoded.size() > kContextBytes) return std::nullopt;
  return encoded;
}

TokenizerEncodeResult encode_template_spans(const TokenizerPack& tokenizer, const std::span<const TemplateSpan> spans) {
  struct Node { std::map<unsigned char, std::size_t> next; std::optional<std::size_t> token; };
  std::vector<Node> trie(1U);
  if (tokenizer.tokens_by_id.size() != tokenizer.token_types.size())
    return TokenizerPackError{TokenizerPackErrorCode::invalid_token_types, "template vocabulary types"};
  for (std::size_t i = 0; i < tokenizer.tokens_by_id.size(); ++i) {
    if (tokenizer.token_types[i] != 3 && tokenizer.token_types[i] != 4) continue;
    if (tokenizer.tokens_by_id[i].empty()) continue;
    std::size_t node = 0U;
    for (const unsigned char byte : tokenizer.tokens_by_id[i]) {
      const auto found = trie[node].next.find(byte);
      if (found != trie[node].next.end()) { node = found->second; continue; }
      if (trie.size() >= 65536U) return TokenizerPackError{TokenizerPackErrorCode::input_text_too_large, "template marker budget"};
      const auto child = trie.size();
      trie[node].next.emplace(byte, child);
      trie.emplace_back();
      node = child;
    }
    if (trie[node].token) return TokenizerPackError{TokenizerPackErrorCode::unsupported_special_token, "ambiguous template marker"};
    trie[node].token = i;
  }
  PromptBuilder builder;
  std::size_t steps = 0U;
  const auto templated = [&](const std::string_view value) {
    std::size_t plain = 0U, offset = 0U;
    while (offset < value.size()) {
      std::size_t node = 0U, end = offset;
      std::optional<std::size_t> matched;
      for (std::size_t at = offset; at < value.size(); ++at) {
        if (++steps > (8U << 20)) return false;
        const auto found = trie[node].next.find(static_cast<unsigned char>(value[at]));
        if (found == trie[node].next.end()) break;
        node = found->second;
        if (trie[node].token) { matched = trie[node].token; end = at + 1U; }
      }
      if (!matched) { ++offset; continue; }
      builder.plain(value.substr(plain, offset - plain));
      builder.marker(tokenizer.tokens_by_id[*matched]);
      offset = end;
      plain = end;
    }
    builder.plain(value.substr(plain));
    return true;
  };
  std::string trusted;
  std::size_t bytes = 0U;
  for (const auto& span : spans) {
    if (span.text.size() > (1U << 20) - bytes)
      return TokenizerPackError{TokenizerPackErrorCode::input_text_too_large, "template output budget"};
    bytes += span.text.size();
    if (!span.input) { trusted += span.text; continue; }
    if (!templated(trusted)) return TokenizerPackError{TokenizerPackErrorCode::input_text_too_large, "template marker work budget"};
    trusted.clear();
    builder.plain(span.text);
  }
  if (!templated(trusted)) return TokenizerPackError{TokenizerPackErrorCode::input_text_too_large, "template marker work budget"};
  return encode_prompt_pieces(tokenizer, builder.take());
}

ChatGenerationPlanResult apply_embedded_template(
    ChatGenerationPlanResult baseline, const EmbeddedTemplateSelection& selected,
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
    const std::span<const std::string> tools, const std::span<const ToolRound> trailing,
    const std::size_t maximum_prompt_tokens, const bool thinking, const std::string_view effort) {
  auto* plan = std::get_if<ChatGenerationPlan>(&baseline);
  if (plan == nullptr) return baseline;
  const auto context = embedded_template_context(selected, request, tools, trailing, thinking, effort);
  if (!context) return failure(ChatGenerationPlanErrorCode::invalid_request, "embedded template context is unsupported or too large");
  const auto rendered = render_jinja_template(selected.helper, selected.source, *context);
  const auto* spans = std::get_if<std::vector<TemplateSpan>>(&rendered);
  if (spans == nullptr) return failure(ChatGenerationPlanErrorCode::prompt_render_failed, "the selected embedded template could not render");
  auto encoded = encode_template_spans(tokenizer, *spans);
  const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
  if (ids == nullptr) return failure(ChatGenerationPlanErrorCode::tokenization_failed, "the selected embedded template could not be tokenized");
  if (ids->empty() || ids->size() > maximum_prompt_tokens)
    return failure(ChatGenerationPlanErrorCode::prompt_too_large, "the selected embedded template exceeds the prompt limit");
  plan->prompt_token_ids = *ids;
  // The selected template, not the fixed renderer, owns the generation tail.
  // An explicit open/close token is the same boundary the output parser uses.
  plan->starts_in_reasoning = false;
  if (plan->think_tokens) {
    for (const auto id : *ids) {
      if (id == plan->think_tokens->open) plan->starts_in_reasoning = true;
      if (id == plan->think_tokens->close || std::find(plan->stop_tokens.begin(), plan->stop_tokens.end(), id) != plan->stop_tokens.end() ||
          std::find(plan->server_stop_tokens.begin(), plan->server_stop_tokens.end(), id) != plan->server_stop_tokens.end())
        plan->starts_in_reasoning = false;
    }
  }
  return baseline;
}

}  // namespace secure_frontend
