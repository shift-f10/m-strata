#pragma once

#include "secure_frontend/api_client.hpp"
#include "secure_frontend/config.hpp"
#include "support/chat_json.hpp"

#include <string>
#include <string_view>

namespace test_support {

inline secure_frontend::LoopbackAddress loopback_v4() {
  return *secure_frontend::LoopbackAddress::from(*secure_frontend::parse_listener_address("127.0.0.1"));
}

inline secure_frontend::ChatExchangeResult chat(const ServerProcess& server, const std::string_view body,
                                                const secure_frontend::ChatRequestOptions& options = {}) {
  return secure_frontend::post_chat_completion(loopback_v4(), server.port(), kHarnessToken, body, options);
}

// A connection that has sent one chat request and has read nothing.
inline std::optional<secure_frontend::LoopbackStreamConnection> open_raw(const ServerProcess& server, const std::string_view body,
                                                                        const int receive_buffer_bytes = 0) {
  auto connection = secure_frontend::LoopbackStreamConnection::open(loopback_v4(), server.port(), std::chrono::seconds(5), receive_buffer_bytes);
  if (connection && !connection->send_all(secure_frontend::render_chat_request(loopback_v4(), server.port(), kHarnessToken, body),
                                          std::chrono::seconds(5))) {
    connection.reset();
  }
  return connection;
}

inline std::string stream_text(const secure_frontend::ChatExchangeResult& result) {
  std::string text;
  for (const auto& item : result.items) {
    if (item.kind != secure_frontend::SseItemKind::data || item.text == "[DONE]") continue;
    if (const auto content = delta_content(item.text)) text += *content;
  }
  return text;
}

// finish_reason of the final chunk, or "<none>".
inline std::string stream_finish(const secure_frontend::ChatExchangeResult& result) {
  for (const auto& item : result.items) {
    if (item.kind != secure_frontend::SseItemKind::data || item.text == "[DONE]") continue;
    if (const auto finish = chunk_finish(item.text)) return *finish;
  }
  return "<none>";
}

// error.code of the first in-stream error event, or "<none>".
inline std::string stream_error_code(const secure_frontend::ChatExchangeResult& result) {
  for (const auto& item : result.items) {
    if (item.kind == secure_frontend::SseItemKind::data && item.text.rfind(R"({"error")", 0) == 0) return error_code_of(item.text);
  }
  return "<none>";
}

inline std::size_t content_event_count(const secure_frontend::ChatExchangeResult& result) {
  std::size_t count = 0;
  for (const auto& item : result.items) count += item.kind == secure_frontend::SseItemKind::data && item.text.find(R"("delta":{"content")") != std::string::npos;
  return count;
}

inline std::size_t comments_before_content(const secure_frontend::ChatExchangeResult& result) {
  std::size_t count = 0;
  for (const auto& item : result.items) {
    if (item.kind == secure_frontend::SseItemKind::comment) ++count;
    else if (item.text.find(R"("delta":{"content")") != std::string::npos) break;
  }
  return count;
}

}  // namespace test_support
