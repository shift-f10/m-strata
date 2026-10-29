#pragma once

#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/config_defaults.hpp"
#include "secure_frontend/sampling_params.hpp"
#include "secure_frontend/tool_calls.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

// Deliberately small, offline-only subset of the OpenAI chat request schema.
// This is a schema boundary, not an engine API: text messages are the rule;
// the ONE carve-out is SP5 M1 Task 12 (C-9): a content ARRAY of text/image_url
// parts, accepted only when the composition proved the active model can read
// images (vision_capable). It still has no representation for tools, URLs of
// any other kind, or MCP.
struct ChatMessagePart final {
  std::string text;            // set for a {"type":"text"} part
  std::string image_data_url;  // set for a {"type":"image_url"} part — the data URL, verbatim
};

struct ChatMessage final {
  std::string role;
  std::string content;           // the text of a string content, or of all text parts (parts form)
  std::vector<ChatMessagePart> parts{};  // empty: the string-content form; else the parts array
  // An earlier answer's searches (owner 2026-10-29, "keep earlier searches in the chat history as upstream's page
  // does"): the request's "strata_rounds", assistant messages only - per round the text before its calls, the calls
  // and one result each (reasoning empty); `content` is then the answer's text after its last round, and may be empty.
  std::vector<ToolRound> rounds{};
};

struct ChatCompletionRequest final {
  std::string model;
  std::vector<ChatMessage> messages;
  std::optional<std::size_t> max_tokens;   // >= 1, at most UINT32_MAX; clamped at resolve (B2)
  bool stream{false};
  bool include_usage{false};
  SamplingParams sampling{};
  // Session log step 14 (upstream's per-request reasoning_effort): none, low, medium or high,
  // the reasoning.effort words; unset = the server's reasoning.effort. Qwen profile only.
  std::optional<std::string> reasoning_effort{};
  // Owner 2026-10-28 (the chat's web search checkbox): false = no search in this request; true or unset = the
  // server's and the card's choice. Any profile takes it; one without the tool has nothing to turn off.
  std::optional<bool> strata_search{};
};

// Chat request/response bounds; derived from FrontendConfig by chat_schema_limits().
struct ChatSchemaLimits final {
  std::size_t max_body_bytes{static_cast<std::size_t>(config_defaults::kChatMaxBodyBytes)};
  std::size_t max_messages{static_cast<std::size_t>(config_defaults::kChatMaxMessages)};
  std::size_t max_message_bytes{static_cast<std::size_t>(config_defaults::kChatMaxMessageBytes)};
  std::size_t max_completion_tokens{static_cast<std::size_t>(config_defaults::kChatMaxCompletionTokens)};
  std::size_t max_generated_bytes{static_cast<std::size_t>(config_defaults::kChatMaxGeneratedBytes)};
  // SP5 M1 Task 12 (C-9; Ruling T12-b): the content-array gate. vision_capable is set
  // ONLY from the composed launch — vision.enabled AND the manifest's launch.mmproj —
  // never from the request. max_images is vision.max_n (pin :103 family, re-scoped);
  // max_image_b64_chars bounds each URL's base64 LENGTH before anything decodes
  // (T12-c: the pin's IMAGE_URL_MAX = 32 MiB of decoded bytes, serve/server.py:2474).
  bool vision_capable{false};
  // Session log step 14: the qwen renderer takes reasoning_effort per request; set at boot from
  // the composed launch's profile (false: the key is refused by name).
  bool per_request_reasoning_effort{false};
  std::size_t max_images{4};
  std::size_t max_image_b64_chars{(std::size_t{32} << 20) / 3U * 4U + 4U};
};

struct ChatRequestError final {
  ApiError error;
  std::string_view param;   // a fixed key from api_errors (is_fixed_param_key) or empty
};
using ChatRequestResult = std::variant<ChatCompletionRequest, ChatRequestError>;

// Spec §3.1: the full request schema. Errors name only a fixed parameter key
// (string literal storage), never request bytes. Extension keys and tool or
// function roles are refused first, in document order (EXT-01).
[[nodiscard]] ChatRequestResult parse_chat_request(std::string_view json, const ChatSchemaLimits& limits = {});

// Wrapper over parse_chat_request: nullopt on any error.
// Returns nullopt for malformed JSON, unsupported fields/features, or values
// that exceed the fixed request limits.  Errors intentionally carry no client
// supplied input so callers cannot accidentally reflect sensitive prompts.
[[nodiscard]] std::optional<ChatCompletionRequest> parse_chat_completion_request(
    std::string_view json, const ChatSchemaLimits& limits = {});

// Produces the non-streaming response form only.  The generated text is JSON
// escaped and bounded; nullopt means either caller-owned request fields or the
// generated text are unsafe to serialize.
[[nodiscard]] std::optional<std::string> render_chat_completion_response(
    const ChatCompletionRequest& request, std::string_view generated_text, const ChatSchemaLimits& limits = {});

// SP3 Task 15 (spec §3.5): the OpenAI chat.completion body for a finished
// generation — the form M2's streaming path finalizes with too.
struct CompletionView final {
  std::string_view id;
  std::int64_t created;
  std::string_view model;
  std::string_view text;
  std::string_view finish_reason;
  std::size_t prompt_tokens;
  std::size_t completion_tokens;
  // message.reasoning_content, written only when non-empty (upstream serve/server.py:5511-5513).
  std::string_view reasoning{};
  // Web search: message.strata_search, the search loop's events (each a JSON object), written only when there are any
  // (upstream's openai_collect keeps its strata_mcp items so).
  std::span<const std::string> search_events{};
};

[[nodiscard]] std::optional<std::string> render_chat_completion(const CompletionView& view, const ChatSchemaLimits& limits = {});

}  // namespace secure_frontend
