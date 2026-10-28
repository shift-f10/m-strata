#include "secure_frontend/sse_encoder.hpp"

#include "secure_frontend/json.hpp"

#include <algorithm>

namespace secure_frontend {
namespace {

constexpr std::string_view kChoiceTail = R"(,"logprobs":null,"finish_reason":null}]})" "\n\n";

std::string envelope(const SseIdentity& identity) {
  std::string out = R"(data: {"id":)";
  append_json_string(&out, identity.id);
  out += R"(,"object":"chat.completion.chunk","created":)";
  out += std::to_string(identity.created);
  out += R"(,"model":)";
  append_json_string(&out, identity.model);
  return out;
}

}  // namespace

std::string render_role_event(const SseIdentity& identity) {
  std::string out = envelope(identity);
  out += R"(,"choices":[{"index":0,"delta":{"role":"assistant","content":""})";
  out += kChoiceTail;
  return out;
}

std::size_t next_delta_size(const std::string_view text) noexcept {
  std::size_t size = std::min(text.size(), api_constants::kMaxDeltaTextBytes);
  if (size == text.size()) return size;
  while (size > 0U && (static_cast<unsigned char>(text[size]) & 0xC0U) == 0x80U) --size;
  return size;
}

std::string render_content_event(const SseIdentity& identity, const std::string_view text) {
  std::string out = envelope(identity);
  out += R"(,"choices":[{"index":0,"delta":{"content":)";
  append_json_string(&out, text);
  out += '}';
  out += kChoiceTail;
  return out;
}

std::string render_reasoning_event(const SseIdentity& identity, const std::string_view text) {
  std::string out = envelope(identity);
  out += R"(,"choices":[{"index":0,"delta":{"reasoning_content":)";
  append_json_string(&out, text);
  out += '}';
  out += kChoiceTail;
  return out;
}

std::string render_final_event(const SseIdentity& identity, const std::string_view finish_reason) {
  std::string out = envelope(identity);
  out += R"(,"choices":[{"index":0,"delta":{},"logprobs":null,"finish_reason":)";
  append_json_string(&out, finish_reason);
  out += "}]}\n\n";
  return out;
}

std::string render_search_event(const SseIdentity& identity, const std::string_view event_json) {
  std::string out = envelope(identity);
  out += R"(,"choices":[{"index":0,"delta":{},"logprobs":null,"finish_reason":null}],"strata_search":)";
  out += event_json;
  out += "}\n\n";
  return out;
}

std::string render_usage_event(const SseIdentity& identity, const std::size_t prompt_tokens, const std::size_t completion_tokens) {
  std::string out = envelope(identity);
  out += R"(,"choices":[],"usage":{"prompt_tokens":)" + std::to_string(prompt_tokens) + R"(,"completion_tokens":)" +
         std::to_string(completion_tokens) + R"(,"total_tokens":)" + std::to_string(prompt_tokens + completion_tokens) + "}}\n\n";
  return out;
}

std::string render_error_event(const ApiError code) {
  const auto& row = api_error_row(code);
  std::string out = R"(data: {"error":{"message":)";
  append_json_string(&out, row.message);
  out += R"(,"type":)";
  append_json_string(&out, row.type);
  out += R"(,"param":null,"code":)";
  append_json_string(&out, row.code);
  out += "}}\n\n";
  return out;
}

std::string frame_chunk(const std::string_view bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string size;
  for (std::size_t value = bytes.size(); value != 0U || size.empty(); value >>= 4U) size.insert(size.begin(), kHex[value & 0x0fU]);
  std::string out;
  out.reserve(size.size() + bytes.size() + 4U);
  out += size;
  out += "\r\n";
  out += bytes;
  out += "\r\n";
  return out;
}

}  // namespace secure_frontend
