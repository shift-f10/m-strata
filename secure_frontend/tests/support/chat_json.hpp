#pragma once

// SP3 M2 test helpers: JSON reading of chat responses and SSE payloads, the
// chat body, a server started with fake-engine flags and config keys (M1's `config_literals`).
#include "secure_frontend/json.hpp"
#include "support/server_harness.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace test_support {

inline std::optional<secure_frontend::JsonValue> parse_test_json(const std::string_view text) {
  auto parsed = secure_frontend::parse_json(text, {.max_bytes = 16U << 20U, .max_depth = 8U, .max_container_entries = 4096U, .max_string_bytes = 16U << 20U});
  if (auto* value = std::get_if<secure_frontend::JsonValue>(&parsed)) return std::move(*value);
  return std::nullopt;
}

inline const secure_frontend::JsonValue* json_member(const secure_frontend::JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& member : *members) {
    if (member.key == key) return &member.value;
  }
  return nullptr;
}

inline const secure_frontend::JsonValue* first_choice(const secure_frontend::JsonValue& root) {
  const auto* choices = json_member(root, "choices");
  if (choices == nullptr || choices->if_array() == nullptr || choices->if_array()->empty()) return nullptr;
  return &choices->if_array()->front();
}

inline std::optional<std::string> string_at(const secure_frontend::JsonValue* object, const std::string_view key) {
  const auto* value = object != nullptr ? json_member(*object, key) : nullptr;
  if (value == nullptr || value->if_string() == nullptr) return std::nullopt;
  return *value->if_string();
}

// choices[0].message.content / choices[0].finish_reason of a complete response.
inline std::string complete_text(const std::string_view body) {
  const auto root = parse_test_json(body);
  const auto* choice = root ? first_choice(*root) : nullptr;
  const auto* message = choice != nullptr ? json_member(*choice, "message") : nullptr;
  return string_at(message, "content").value_or("<no content>");
}
inline std::string complete_finish(const std::string_view body) {
  const auto root = parse_test_json(body);
  return string_at(root ? first_choice(*root) : nullptr, "finish_reason").value_or("<no finish>");
}
// error.code of an error body or of an SSE error payload.
inline std::string error_code_of(const std::string_view body) {
  const auto root = parse_test_json(body);
  return string_at(root ? json_member(*root, "error") : nullptr, "code").value_or("<no error>");
}
// For one SSE data payload: choices[0].delta.content, choices[0].finish_reason (null → nullopt).
inline std::optional<std::string> delta_content(const std::string_view data) {
  const auto root = parse_test_json(data);
  const auto* choice = root ? first_choice(*root) : nullptr;
  return string_at(choice != nullptr ? json_member(*choice, "delta") : nullptr, "content");
}
inline std::optional<std::string> chunk_finish(const std::string_view data) {
  const auto root = parse_test_json(data);
  return string_at(root ? first_choice(*root) : nullptr, "finish_reason");
}

// {"model":"fake-model","messages":[{"role":"user","content":"<content>"}],<extra>}
inline std::string chat_body(const std::string_view extra = "", const std::string_view content = "Hi") {
  std::string body = R"({"model":"fake-model","messages":[{"role":"user","content":")";
  body += content;
  body += "\"}]";
  if (!extra.empty()) {
    body += ',';
    body += extra;
  }
  body += '}';
  return body;
}

// The server with fake-engine flags and config keys (key -> JSON literal), ready to serve.
inline std::unique_ptr<ServerProcess> start_stream_server(std::vector<std::string> engine_args, std::map<std::string, std::string> config = {}) {
  ServerOptions options;
  options.engine_args = std::move(engine_args);
  options.config_literals = std::move(config);
  auto server = ServerProcess::start(options);
  if (server && !server->wait_for_health(kHealthReady, std::chrono::seconds(20))) server.reset();
  return server;
}

// True once `path` contains `line` as a whole line, polled every 20 ms.
inline bool file_has_line(const std::filesystem::path& path, const std::string_view line, const std::chrono::milliseconds wait) {
  const auto until = std::chrono::steady_clock::now() + wait;
  do {
    std::error_code ignored;
    if (std::filesystem::exists(path, ignored)) {
      const std::string text = "\n" + secure_frontend_test::read_file(path);
      if (text.find("\n" + std::string(line) + "\n") != std::string::npos) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  } while (std::chrono::steady_clock::now() < until);
  return false;
}

}  // namespace test_support
