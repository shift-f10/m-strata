#include "secure_frontend/mcp_client.hpp"

#include "secure_frontend/deadline.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <utility>

namespace secure_frontend {
namespace {

constexpr JsonLimits kMessageLimits{.max_bytes = 4U << 20U, .max_depth = 64U, .max_container_entries = 100000U,
                                    .max_string_bytes = 4U << 20U, .allow_fractions = true};
constexpr std::size_t kMaxListPages = 100U;  // upstream's bound on tools/list pages
constexpr std::chrono::milliseconds kPollSlice{50};

std::string json_quoted(const std::string_view text) {
  std::string out;
  append_json_string(&out, text);
  return out;
}

const JsonValue* member(const JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& m : *members) {
    if (m.key == key) return &m.value;
  }
  return nullptr;
}

std::string text_member(const JsonValue& object, const std::string_view key) {
  const auto* value = member(object, key);
  return value != nullptr && value->if_string() != nullptr ? *value->if_string() : std::string{};
}

// The number of characters (code points) in valid UTF-8, and the byte offset where the first `count` end.
std::size_t code_points(const std::string_view text) {
  return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](const char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
}
std::size_t prefix_bytes(const std::string_view text, const std::size_t count) {
  std::size_t seen = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0U) != 0x80U) {
      if (seen == count) return i;
      ++seen;
    }
  }
  return text.size();
}

// Python's {n:,}: thousands with commas
std::string with_commas(const std::size_t n) {
  std::string digits = std::to_string(n);
  for (std::size_t at = digits.size(); at > 3U; at -= 3U) digits.insert(at - 3U, 1, ',');
  return digits;
}

// Python's {seconds:g}
std::string seconds_g(const std::chrono::milliseconds timeout) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(timeout.count()) / 1000.0);
  return buffer;
}

struct Awaited final {
  std::optional<JsonValue> result;
  std::optional<std::string> error;  // "message (code N)", or why nothing came
  std::optional<std::int64_t> code;
  bool cancelled{false};
};

}  // namespace

std::string mcp_result_text(const JsonValue& result) {
  std::vector<std::string> parts;
  if (const auto* content = member(result, "content"); content != nullptr && content->if_array() != nullptr) {
    for (const auto& block : *content->if_array()) {
      if (block.if_object() == nullptr) continue;
      const std::string kind = text_member(block, "type");
      if (kind == "text") {
        parts.push_back(text_member(block, "text"));
      } else if (kind == "resource") {
        const auto* resource = member(block, "resource");
        const JsonValue empty = JsonValue::make_object({});
        const JsonValue& res = resource != nullptr && resource->if_object() != nullptr ? *resource : empty;
        if (const auto* text = member(res, "text"); text != nullptr) {
          parts.push_back(text->if_string() != nullptr ? *text->if_string() : python_json_text(*text));
        } else {
          const std::string mime = text_member(res, "mimeType");
          parts.push_back("[resource " + text_member(res, "uri") + " (" + (mime.empty() ? std::string{"binary"} : mime) + "), not shown]");
        }
      } else if (kind == "resource_link") {
        std::string note = "[resource link: " + text_member(block, "name") + " " + text_member(block, "uri") + "]";
        for (std::size_t at; (at = note.find("  ")) != std::string::npos;) note.erase(at, 1);
        parts.push_back(note);
      } else if (kind == "image" || kind == "audio") {
        const std::string mime = text_member(block, "mimeType");
        const std::size_t size = text_member(block, "data").size() * 3U / 4U;
        parts.push_back("[" + kind + " (" + (mime.empty() ? std::string{"?"} : mime) + ", " + with_commas(size) + " bytes): not shown to the model]");
      } else {
        parts.push_back(python_json_text(block));
      }
    }
  }
  if (parts.empty()) {
    if (const auto* structured = member(result, "structuredContent"); structured != nullptr && structured->kind() != JsonKind::null) {
      parts.push_back(python_json_text(*structured));
    }
  }
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) out += (i != 0U ? "\n" : "") + parts[i];
  return out;
}

McpClient::McpClient(EngineLineTransport& transport, const McpLimits limits) : transport_(transport), limits_(limits) {}

namespace {

// Writes one message line; false when the server cannot take it.
bool send(EngineLineTransport& transport, const std::string& message) {
  std::string error;
  return transport.write_line(message, deadline_after(std::chrono::seconds(10)), error);
}

// Reads until the answer to `id`, answering the server's own requests (ping; anything else -32601) and dropping its
// notifications, other answers and lines that are not JSON. A timeout or a cancel sends notifications/cancelled.
Awaited await_answer(EngineLineTransport& transport, const std::int64_t id, const std::chrono::milliseconds timeout,
                     const std::function<bool()>& cancelled, const std::string_view what) {
  const auto deadline = deadline_after(timeout);
  const auto cancel_it = [&](const std::string& reason) {
    (void)send(transport, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":" + std::to_string(id) +
                              ",\"reason\":" + json_quoted(reason) + "}}");
  };
  for (;;) {
    if (cancelled && cancelled()) {
      cancel_it("stopped");
      return Awaited{.cancelled = true};
    }
    const auto left = remaining_until(deadline);
    if (left.count() <= 0) {
      const std::string reason = std::string{what} + " timed out after " + seconds_g(timeout) + " s";
      cancel_it(reason);
      return Awaited{.error = reason};
    }
    const auto read = transport.read_line(std::min(left, kPollSlice));
    if (read.kind == EngineReadKind::timeout) continue;
    if (read.kind != EngineReadKind::line) return Awaited{.error = std::string{"the server stopped"}};
    const auto parsed = parse_json(read.line, kMessageLimits);
    const auto* root = std::get_if<JsonValue>(&parsed);
    if (root == nullptr) continue;  // (a server that prints to stdout; the spec forbids it)
    std::vector<const JsonValue*> messages;
    if (const auto* batch = root->if_array(); batch != nullptr) {
      for (const auto& item : *batch) messages.push_back(&item);
    } else {
      messages.push_back(root);
    }
    for (const JsonValue* message : messages) {
      if (message->if_object() == nullptr) continue;
      const auto* message_id = member(*message, "id");
      if (const auto* method = member(*message, "method"); method != nullptr) {
        if (message_id == nullptr) continue;  // a notification
        const std::string reply_id = python_json_text(*message_id);
        if (method->if_string() != nullptr && *method->if_string() == "ping") {
          (void)send(transport, "{\"jsonrpc\":\"2.0\",\"id\":" + reply_id + ",\"result\":{}}");
        } else {
          (void)send(transport, "{\"jsonrpc\":\"2.0\",\"id\":" + reply_id + ",\"error\":{\"code\":-32601,\"message\":\"method not supported\"}}");
        }
        continue;
      }
      if (message_id == nullptr || message_id->if_integer() == nullptr || *message_id->if_integer() != id) continue;  // another's
      if (const auto* failure = member(*message, "error"); failure != nullptr) {
        std::string text = failure->if_object() != nullptr ? text_member(*failure, "message") : python_json_text(*failure);
        const auto* code = failure->if_object() != nullptr ? member(*failure, "code") : nullptr;
        Awaited out;
        if (code != nullptr && code->if_integer() != nullptr) out.code = *code->if_integer();
        out.error = (text.empty() ? std::string{"error"} : text) + " (code " + (code != nullptr ? python_json_text(*code) : std::string{"None"}) + ")";
        return out;
      }
      const auto* result = member(*message, "result");
      return Awaited{.result = result != nullptr && result->if_object() != nullptr ? *result : JsonValue::make_object({})};
    }
  }
}

}  // namespace

std::optional<std::string> McpClient::start() {
  tools_.clear();
  const auto initialize_id = next_id_++;
  if (!send(transport_, "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(initialize_id) +
                            ",\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
                            "\"clientInfo\":{\"name\":\"strata\",\"title\":\"Strata\",\"version\":\"1\"}}}")) {
    return std::string{"the MCP server is not running"};
  }
  const auto started = await_answer(transport_, initialize_id, limits_.start_timeout, {}, "initialize");
  if (!started.result) return std::string{"the MCP server did not initialize"};
  if (!send(transport_, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}")) return std::string{"the MCP server is not running"};
  std::string cursor;
  for (std::size_t page = 0; page < kMaxListPages; ++page) {
    const auto list_id = next_id_++;
    const std::string params = cursor.empty() ? std::string{"{}"} : "{\"cursor\":" + json_quoted(cursor) + "}";
    if (!send(transport_, "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(list_id) + ",\"method\":\"tools/list\",\"params\":" + params + "}")) {
      return std::string{"the MCP server is not running"};
    }
    const auto listed = await_answer(transport_, list_id, limits_.start_timeout, {}, "tools/list");
    if (!listed.result) {
      if (listed.code == std::optional<std::int64_t>{-32601} && tools_.empty()) return std::nullopt;  // a server without tools
      return std::string{"the MCP server did not list its tools"};
    }
    if (const auto* tools = member(*listed.result, "tools"); tools != nullptr && tools->if_array() != nullptr) {
      for (const auto& tool : *tools->if_array()) {
        const std::string name = text_member(tool, "name");
        if (name.empty()) continue;
        std::string description = text_member(tool, "description");
        if (description.empty()) description = text_member(tool, "title");
        const auto* schema = member(tool, "inputSchema");
        tools_.push_back(McpTool{name, description,
                                 schema != nullptr && schema->if_object() != nullptr ? python_json_text(*schema)
                                                                                     : std::string{R"({"type": "object", "properties": {}})"}});
      }
    }
    cursor = text_member(*listed.result, "nextCursor");
    if (cursor.empty()) return std::nullopt;
  }
  return std::nullopt;
}

McpCallResult McpClient::call(const std::string_view name, const std::string_view arguments_json, const std::function<bool()>& cancelled) {
  McpCallResult out;
  const auto id = next_id_++;
  std::string text;
  if (!send(transport_, "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"method\":\"tools/call\",\"params\":{\"name\":" +
                            json_quoted(name) + ",\"arguments\":" + std::string{arguments_json} + "}}")) {
    text = "error: the server is not running";
  } else {
    const auto answered = await_answer(transport_, id, limits_.call_timeout, cancelled, "tools/call");
    if (answered.cancelled) {
      out.cancelled = true;
      text = "error: stopped";
    } else if (!answered.result) {
      text = "error: " + answered.error.value_or("no answer");
    } else {
      text = mcp_result_text(*answered.result);
      const auto* failed = member(*answered.result, "isError");
      out.ok = failed == nullptr || failed->if_boolean() == nullptr || !*failed->if_boolean();
      if (!out.ok) text = "error: " + (text.empty() ? std::string{"the tool reported an error"} : text);
    }
  }
  out.chars = code_points(text);
  const std::size_t cap = limits_.max_result_chars;
  if (cap > 0U && out.chars > cap) {
    text = text.substr(0, prefix_bytes(text, cap)) + "\n\n[... truncated: the tool returned " + with_commas(out.chars) +
           " characters; only the first " + with_commas(cap) + " are shown]";
    out.truncated = true;
  }
  out.text = std::move(text);
  return out;
}

}  // namespace secure_frontend
