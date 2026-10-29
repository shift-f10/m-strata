#include "secure_frontend/chat_schema.hpp"

#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

constexpr std::size_t kMaximumModelBytes = 256U;  // model-id format bound, not a tuning limit

bool is_continuation(unsigned char byte) { return (byte & 0xc0U) == 0x80U; }

bool valid_utf8(std::string_view value) {
  for (std::size_t index = 0; index < value.size();) {
    const auto byte = static_cast<unsigned char>(value[index]);
    if (byte <= 0x7fU) {
      ++index;
    } else if (byte >= 0xc2U && byte <= 0xdfU && index + 1U < value.size() &&
               is_continuation(static_cast<unsigned char>(value[index + 1U]))) {
      index += 2U;
    } else if (byte == 0xe0U && index + 2U < value.size() &&
               static_cast<unsigned char>(value[index + 1U]) >= 0xa0U &&
               static_cast<unsigned char>(value[index + 1U]) <= 0xbfU &&
               is_continuation(static_cast<unsigned char>(value[index + 2U]))) {
      index += 3U;
    } else if (((byte >= 0xe1U && byte <= 0xecU) || (byte >= 0xeeU && byte <= 0xefU)) &&
               index + 2U < value.size() && is_continuation(static_cast<unsigned char>(value[index + 1U])) &&
               is_continuation(static_cast<unsigned char>(value[index + 2U]))) {
      index += 3U;
    } else if (byte == 0xedU && index + 2U < value.size() &&
               static_cast<unsigned char>(value[index + 1U]) >= 0x80U &&
               static_cast<unsigned char>(value[index + 1U]) <= 0x9fU &&
               is_continuation(static_cast<unsigned char>(value[index + 2U]))) {
      index += 3U;
    } else if (byte == 0xf0U && index + 3U < value.size() &&
               static_cast<unsigned char>(value[index + 1U]) >= 0x90U &&
               static_cast<unsigned char>(value[index + 1U]) <= 0xbfU &&
               is_continuation(static_cast<unsigned char>(value[index + 2U])) &&
               is_continuation(static_cast<unsigned char>(value[index + 3U]))) {
      index += 4U;
    } else if (byte >= 0xf1U && byte <= 0xf3U && index + 3U < value.size() &&
               is_continuation(static_cast<unsigned char>(value[index + 1U])) &&
               is_continuation(static_cast<unsigned char>(value[index + 2U])) &&
               is_continuation(static_cast<unsigned char>(value[index + 3U]))) {
      index += 4U;
    } else if (byte == 0xf4U && index + 3U < value.size() &&
               static_cast<unsigned char>(value[index + 1U]) >= 0x80U &&
               static_cast<unsigned char>(value[index + 1U]) <= 0x8fU &&
               is_continuation(static_cast<unsigned char>(value[index + 2U])) &&
               is_continuation(static_cast<unsigned char>(value[index + 3U]))) {
      index += 4U;
    } else {
      return false;
    }
  }
  return true;
}

bool contains_remote_scheme(std::string_view value) {
  constexpr std::array<std::string_view, 5> schemes{"http://", "https://", "file://", "data:", "mcp://"};
  std::string lowered;
  lowered.reserve(value.size());
  for (const unsigned char byte : value) lowered.push_back(static_cast<char>(std::tolower(byte)));
  for (const std::string_view scheme : schemes) {
    if (lowered.find(scheme) != std::string::npos) return true;
  }
  return false;
}

bool valid_model(std::string_view value) {
  if (value.empty() || value.size() > kMaximumModelBytes || contains_remote_scheme(value)) return false;
  // Ruling T12-l (SP5 M1 Task 12): a managed model's served id is name[:tag] — the
  // exact ModelId config grammar (make_model_id/validate_model_id) — so the chat
  // field must be able to address it, or the served model would be unaddressable.
  // At most one colon; both segments non-empty, [A-Za-z0-9._-], starting with a
  // letter or digit; the total bound and the remote-scheme guard are unchanged.
  const auto colon = value.find(':');
  if (colon != std::string_view::npos && value.find(':', colon + 1U) != std::string_view::npos) return false;
  const auto valid_segment = [](const std::string_view segment) {
    if (segment.empty() || !std::isalnum(static_cast<unsigned char>(segment.front()))) return false;
    for (const unsigned char byte : segment) {
      if (!(std::isalnum(byte) || byte == '.' || byte == '_' || byte == '-')) return false;
    }
    return true;
  };
  return valid_segment(value.substr(0, colon)) &&
         (colon == std::string_view::npos || valid_segment(value.substr(colon + 1U)));
}

// Message text is text: a link or "data:" in it is never fetched or acted on (SP3 §6.2 lets
// such content be "rejected or treated as text"; rejecting it made any answer with a link
// break the next turn, session log step 15). The model field keeps the scheme guard.
bool valid_content(std::string_view value, std::size_t max_message_bytes);

// Owner 2026-10-29 ("keep earlier searches in the chat history as upstream's page does"): an assistant message's
// strata_rounds - at most kMaxHistoryRounds rounds, each its text before the calls (may be empty), 1 to kMaxRoundCalls
// calls (a name of printable text, arguments an object) and one result text each (may be empty). Closed vocabulary.
constexpr std::size_t kMaxHistoryRounds = 64U;
constexpr std::size_t kMaxRoundCalls = 16U;
constexpr std::size_t kMaxCallNameBytes = 64U;

bool text_or_empty(const JsonValue& value, const std::size_t max_bytes, std::string& out) {
  const auto* text = value.if_string();
  if (text == nullptr || (!text->empty() && !valid_content(*text, max_bytes))) return false;
  out = *text;
  return true;
}

bool parse_call(const JsonValue& value, ToolCall& call) {
  const auto* fields = value.if_object();
  if (fields == nullptr) return false;
  bool name_seen = false;
  bool arguments_seen = false;
  for (const auto& field : *fields) {
    if (field.key == "name") {
      const auto* name = field.value.if_string();
      if (name == nullptr || !valid_content(*name, kMaxCallNameBytes) || name->find_first_of("\n\r\t") != std::string::npos) return false;
      call.name = *name;
      name_seen = true;
    } else if (field.key == "arguments") {
      const auto* arguments = field.value.if_object();
      if (arguments == nullptr) return false;
      for (const auto& argument : *arguments) call.arguments.push_back(ToolArgument{argument.key, argument.value});
      arguments_seen = true;
    } else {
      return false;
    }
  }
  return name_seen && arguments_seen;
}

bool parse_rounds(const JsonValue& value, const std::size_t max_bytes, std::vector<ToolRound>& out) {
  const auto* items = value.if_array();
  if (items == nullptr || items->empty() || items->size() > kMaxHistoryRounds) return false;
  for (const auto& item : *items) {
    const auto* fields = item.if_object();
    if (fields == nullptr) return false;
    ToolRound round;
    bool calls_seen = false;
    bool results_seen = false;
    for (const auto& field : *fields) {
      if (field.key == "content") {
        if (!text_or_empty(field.value, max_bytes, round.content)) return false;
      } else if (field.key == "calls") {
        const auto* calls = field.value.if_array();
        if (calls == nullptr || calls->empty() || calls->size() > kMaxRoundCalls) return false;
        for (const auto& c : *calls) {
          ToolCall call;
          if (!parse_call(c, call)) return false;
          round.calls.push_back(std::move(call));
        }
        calls_seen = true;
      } else if (field.key == "results") {
        const auto* results = field.value.if_array();
        if (results == nullptr) return false;
        for (const auto& r : *results) {
          std::string text;
          if (!text_or_empty(r, max_bytes, text)) return false;
          round.results.push_back(std::move(text));
        }
        results_seen = true;
      } else {
        return false;
      }
    }
    if (!calls_seen || !results_seen || round.results.size() != round.calls.size()) return false;
    out.push_back(std::move(round));
  }
  return true;
}

bool valid_content(std::string_view value, const std::size_t max_message_bytes) {
  if (value.empty() || value.size() > max_message_bytes || !valid_utf8(value)) return false;
  for (const unsigned char byte : value) {
    if (byte < 0x20U && byte != '\n' && byte != '\r' && byte != '\t') return false;
  }
  return true;
}

// SP5 M1 Task 12 (Ruling T12-a/T12-c): the schema half of the image_url gate. Only a
// data URL with a media type and an explicit ;base64, reaches the decoder, and its
// base64 LENGTH is bounded before ANY decode (the pin's 32 MiB cap on fetched image
// URLs, serve/server.py:2474 — an http(s) image URL is not fetched in M1 at all).
bool image_data_url_shape(const std::string_view url, const std::size_t max_b64_chars) {
  if (url.rfind("data:", 0) != 0) return false;
  const auto semicolon = url.find(";base64,");
  if (semicolon == std::string_view::npos || semicolon <= std::string_view{"data:"}.size()) return false;
  return url.size() - semicolon - std::string_view{";base64,"}.size() <= max_b64_chars;
}

}  // namespace

namespace {

constexpr std::string_view kExtensionKeys[] = {"tools", "tool_choice", "functions", "function_call", "parallel_tool_calls", "mcp_servers"};
constexpr std::string_view kSp5Keys[] = {"top_k", "min_p", "top_logprobs", "response_format"};
template <std::size_t N>
std::string_view find_key(const std::string_view (&keys)[N], const std::string_view key) {
  for (const auto k : keys) {
    if (k == key) return k;
  }
  return {};
}

// Integer forms (`1`, `0`) are numbers exactly like `1.0`/`0.0` (Review Focus 5).
std::optional<double> number_of(const JsonValue& value) {
  if (const auto* i = value.if_integer()) return static_cast<double>(*i);
  if (const auto* d = value.if_decimal()) return *d;
  return std::nullopt;
}

// Pre-flight T7(c): named `refuse`, not `fail`. `param` must be a literal.
ChatRequestError refuse(const ApiError error, const std::string_view param = {}) { return {error, param}; }

}  // namespace

ChatRequestResult parse_chat_request(const std::string_view json, const ChatSchemaLimits& limits) {
  // Depth (kChatMaxJsonDepth, 6 since C-9): request -> messages -> message ->
  // content array -> part -> image_url object. Without the parts form the old
  // ceiling of 4 stands; entry and size limits are unchanged either way.
  // level, so `content` arrays and small extension objects reach the schema
  // and get their own error; deeper input is invalid_json at the reader.
  const JsonLimits json_limits{.max_bytes = limits.max_body_bytes, .max_depth = api_constants::kChatMaxJsonDepth,
                               .max_container_entries = std::max<std::size_t>(limits.max_messages, 16U),
                               .max_string_bytes = limits.max_body_bytes, .allow_fractions = true};
  const auto parsed = parse_json(json, json_limits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  if (root == nullptr) return refuse(ApiError::invalid_json);
  const auto* members = root->if_object();
  if (members == nullptr) return refuse(ApiError::invalid_request);

  // Extensions first, in document order (EXT-01), including message roles.
  for (const auto& member : *members) {
    if (const auto key = find_key(kExtensionKeys, member.key); !key.empty()) return refuse(ApiError::extension_disabled, key);
    if (member.key == "messages") {
      if (const auto* items = member.value.if_array()) {
        for (const auto& item : *items) {
          const auto* fields = item.if_object();
          if (fields == nullptr) continue;
          for (const auto& field : *fields) {
            const auto* role = field.value.if_string();
            if (field.key == "role" && role != nullptr && (*role == "tool" || *role == "function")) {
              return refuse(ApiError::extension_disabled, "role");
            }
          }
        }
      }
    }
  }

  ChatCompletionRequest request;
  bool model_seen = false;
  bool messages_seen = false;
  bool max_seen = false;
  bool stream_options_seen = false;
  for (const auto& member : *members) {
    const std::string& key = member.key;
    const JsonValue& value = member.value;
    if (key == "model") {
      const auto* text = value.if_string();
      if (text == nullptr || text->size() > api_constants::kChatModelMaxBytes || !valid_model(*text)) {
        return refuse(ApiError::invalid_request);
      }
      request.model = *text;
      model_seen = true;
    } else if (key == "messages") {
      const auto* items = value.if_array();
      if (items == nullptr || items->empty() || items->size() > limits.max_messages) return refuse(ApiError::invalid_request);
      std::size_t image_count = 0;  // vision.max_n counts the WHOLE request (the pin reads one request's images at a time)
      for (const auto& item : *items) {
        const auto* fields = item.if_object();
        if (fields == nullptr) return refuse(ApiError::invalid_request);
        ChatMessage message;
        bool role_seen = false;
        bool content_seen = false;
        for (const auto& field : *fields) {
          if (field.key == "content" && field.value.kind() == JsonKind::array) {
            // C-9 carve-out (Ruling T12-b): the parts array is a MODEL capability decided by
            // the composed launch (vision_capable), never a request option. Without it the
            // 400 unsupported_parameter "content" stands verbatim.
            if (!limits.vision_capable) return refuse(ApiError::unsupported_parameter, "content");
            const auto* parts = field.value.if_array();
            if (parts->empty() || parts->size() > limits.max_messages) return refuse(ApiError::invalid_request);
            for (const auto& part : *parts) {
              const auto* part_fields = part.if_object();
              if (part_fields == nullptr) return refuse(ApiError::invalid_request);
              ChatMessagePart parsed_part;
              std::string_view type;
              bool type_seen = false;
              for (const auto& part_member : *part_fields) {
                if (part_member.key == "type") {
                  const auto* name = part_member.value.if_string();
                  if (name == nullptr) return refuse(ApiError::invalid_request);
                  type = *name;
                  type_seen = true;
                } else if (type == "text" && part_member.key == "text") {
                  const auto* value_of_text = part_member.value.if_string();
                  if (value_of_text == nullptr) return refuse(ApiError::invalid_request);
                  parsed_part.text = *value_of_text;
                } else if (type == "image_url" && part_member.key == "image_url") {
                  const auto* url_object = part_member.value.if_object();
                  if (url_object == nullptr) return refuse(ApiError::invalid_request);
                  const std::string* url = nullptr;
                  for (const auto& url_member : *url_object) {
                    if (url_member.key != "url" || url != nullptr) return refuse(ApiError::invalid_request);
                    url = url_member.value.if_string();
                    if (url == nullptr) return refuse(ApiError::invalid_request);
                  }
                  if (url == nullptr || url->empty()) return refuse(ApiError::invalid_request);
                  parsed_part.image_data_url = *url;
                } else {
                  return refuse(ApiError::invalid_request);  // closed part vocabulary, "type" first
                }
              }
              if (!type_seen) return refuse(ApiError::invalid_request);
              if (type == "text") {
                if (!valid_content(parsed_part.text, limits.max_message_bytes)) return refuse(ApiError::invalid_request);
                message.content += parsed_part.text;
              } else if (type == "image_url") {
                if (++image_count > limits.max_images) return refuse(ApiError::unsupported_parameter, "image_url");
                if (!image_data_url_shape(parsed_part.image_data_url, limits.max_image_b64_chars))
                  return refuse(ApiError::unsupported_parameter, "image_url");  // length cap BEFORE any decode
              } else {
                return refuse(ApiError::invalid_request);
              }
              message.parts.push_back(std::move(parsed_part));
            }
            content_seen = true;
            continue;
          }
          if (field.key == "strata_rounds") {
            if (!message.rounds.empty() || !parse_rounds(field.value, limits.max_message_bytes, message.rounds)) {
              return refuse(ApiError::invalid_request);
            }
            continue;
          }
          const auto* text = field.value.if_string();
          if (text == nullptr) return refuse(ApiError::invalid_request);
          if (field.key == "role" && (*text == "system" || *text == "user" || *text == "assistant")) {
            message.role = *text;
            role_seen = true;
          } else if (field.key == "content" && (text->empty() || valid_content(*text, limits.max_message_bytes))) {
            message.content = *text;
            content_seen = true;
          } else {
            return refuse(ApiError::invalid_request);
          }
        }
        if (!role_seen || !content_seen) return refuse(ApiError::invalid_request);
        // text, except for an earlier answer whose text all came before its searches; searches on answers only
        if (message.content.empty() && message.parts.empty() && message.rounds.empty()) return refuse(ApiError::invalid_request);
        if (!message.rounds.empty() && (message.role != "assistant" || !message.parts.empty())) return refuse(ApiError::invalid_request);
        request.messages.push_back(std::move(message));
      }
      messages_seen = true;
    } else if (key == "max_tokens" || key == "max_completion_tokens") {
      // B2: any integer >= 1 is accepted; values above UINT32_MAX are clamped
      // here and the real budget is resolved against the config and context later.
      const auto* integer = value.if_integer();
      if (max_seen || integer == nullptr || *integer < 1) return refuse(ApiError::invalid_request);
      request.max_tokens = static_cast<std::size_t>(std::min<std::int64_t>(*integer, 4294967295LL));
      max_seen = true;
    } else if (key == "stream") {
      const auto* flag = value.if_boolean();
      if (flag == nullptr) return refuse(ApiError::invalid_request);
      request.stream = *flag;
    } else if (key == "stream_options") {
      const auto* options = value.if_object();
      if (options == nullptr) return refuse(ApiError::invalid_request);
      for (const auto& option : *options) {
        const bool* include_usage = option.value.if_boolean();
        if (option.key != "include_usage" || include_usage == nullptr) return refuse(ApiError::invalid_request);
        request.include_usage = *include_usage;
      }
      stream_options_seen = true;
    } else if (key == "n") {
      const auto* integer = value.if_integer();
      if (integer == nullptr) return refuse(ApiError::invalid_request);
      if (*integer != 1) return refuse(ApiError::unsupported_parameter, "n");
    } else if (key == "user") {
      const auto* text = value.if_string();
      if (text == nullptr || text->size() > api_constants::kChatUserMaxBytes) return refuse(ApiError::invalid_request);
    } else if (key == "temperature") {
      const auto number = number_of(value);
      if (!number || *number < 0.0 || *number > api_constants::kTemperatureMax) return refuse(ApiError::invalid_request);
      request.sampling.temperature = *number;
    } else if (key == "top_p") {
      const auto number = number_of(value);
      if (!number || *number <= 0.0 || *number > 1.0) return refuse(ApiError::invalid_request);
      request.sampling.top_p = *number;
    } else if (key == "seed") {
      // Above INT64_MAX (= kSeedMax) the reader already refused (invalid_json);
      // a decimal seed is not an integer (invalid_request).
      const auto* integer = value.if_integer();
      if (integer == nullptr || *integer < 0 || static_cast<std::uint64_t>(*integer) > api_constants::kSeedMax) {
        return refuse(ApiError::invalid_request);
      }
      request.sampling.seed = static_cast<std::uint64_t>(*integer);
    } else if (key == "reasoning_effort") {
      // Session log step 14: the reasoning.effort words, per request, on the qwen profile only.
      if (!limits.per_request_reasoning_effort) return refuse(ApiError::unsupported_parameter, "reasoning_effort");
      const auto* word = value.if_string();
      if (word == nullptr || (*word != "none" && *word != "low" && *word != "medium" && *word != "high")) {
        return refuse(ApiError::invalid_request, "reasoning_effort");
      }
      request.reasoning_effort = std::string{*word};
    } else if (key == "strata_search") {
      // Owner 2026-10-28: the chat's web search checkbox; false turns the server's search off for this request.
      const auto* on = value.if_boolean();
      if (on == nullptr) return refuse(ApiError::invalid_request, "strata_search");
      request.strata_search = *on;
    } else if (key == "presence_penalty" || key == "frequency_penalty") {
      const auto number = number_of(value);
      if (!number) return refuse(ApiError::invalid_request);
      if (*number != 0.0) {
        return refuse(ApiError::unsupported_parameter, key == "presence_penalty" ? "presence_penalty" : "frequency_penalty");
      }
    } else if (key == "logit_bias") {
      const auto* object = value.if_object();
      if (object == nullptr || !object->empty()) return refuse(ApiError::unsupported_parameter, "logit_bias");
    } else if (key == "stop") {
      const auto* array = value.if_array();
      if (!(value.kind() == JsonKind::null || (array != nullptr && array->empty()))) {
        return refuse(ApiError::unsupported_parameter, "stop");
      }
    } else if (key == "logprobs") {
      const auto* flag = value.if_boolean();
      if (flag == nullptr) return refuse(ApiError::invalid_request);
      if (*flag) return refuse(ApiError::unsupported_parameter, "logprobs");
    } else if (const auto sp5 = find_key(kSp5Keys, key); !sp5.empty()) {
      return refuse(ApiError::unsupported_parameter, sp5);
    } else {
      return refuse(ApiError::invalid_request);
    }
  }
  // stream_options is only valid with stream:true (accepted since M2).
  if (stream_options_seen && !request.stream) return refuse(ApiError::invalid_request);
  if (!model_seen || !messages_seen) return refuse(ApiError::invalid_request);
  return request;
}

std::optional<ChatCompletionRequest> parse_chat_completion_request(const std::string_view json,
                                                                   const ChatSchemaLimits& limits) {
  auto result = parse_chat_request(json, limits);
  if (auto* request = std::get_if<ChatCompletionRequest>(&result)) return std::move(*request);
  return std::nullopt;
}

std::optional<std::string> render_chat_completion_response(const ChatCompletionRequest& request,
                                                           std::string_view generated_text,
                                                           const ChatSchemaLimits& limits) {
  if (!valid_model(request.model) || generated_text.size() > limits.max_generated_bytes ||
      !valid_utf8(generated_text)) {
    return std::nullopt;
  }
  std::string response{"{\"id\":\"chatcmpl-local\",\"object\":\"chat.completion\",\"created\":0,\"model\":"};
  append_json_string(&response, request.model);
  response.append(",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":");
  append_json_string(&response, generated_text);
  response.append("},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":0,\"total_tokens\":0}}\n");
  return response;
}

std::optional<std::string> render_chat_completion(const CompletionView& view, const ChatSchemaLimits& limits) {
  if (view.text.size() > limits.max_generated_bytes || view.reasoning.size() > limits.max_generated_bytes || !valid_utf8(view.text) ||
      !valid_utf8(view.reasoning) || !valid_utf8(view.model)) {
    return std::nullopt;
  }
  std::string out{"{\"id\":"};
  append_json_string(&out, view.id);
  out += ",\"object\":\"chat.completion\",\"created\":" + std::to_string(view.created) + ",\"model\":";
  append_json_string(&out, view.model);
  out += ",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":";
  append_json_string(&out, view.text);
  if (!view.reasoning.empty()) {
    out += ",\"reasoning_content\":";
    append_json_string(&out, view.reasoning);
  }
  if (!view.search_events.empty()) {
    out += ",\"strata_search\":[";
    for (std::size_t i = 0; i < view.search_events.size(); ++i) {
      if (i > 0U) out += ',';
      out += view.search_events[i];  // each a JSON object the search loop wrote
    }
    out += ']';
  }
  out += "},\"logprobs\":null,\"finish_reason\":";
  append_json_string(&out, view.finish_reason);
  out += "}],\"usage\":{\"prompt_tokens\":" + std::to_string(view.prompt_tokens) + ",\"completion_tokens\":" +
         std::to_string(view.completion_tokens) + ",\"total_tokens\":" + std::to_string(view.prompt_tokens + view.completion_tokens) + "}}\n";
  return out;
}

}  // namespace secure_frontend
