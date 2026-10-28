#include "secure_frontend/search_mcp_server.hpp"

#include "secure_frontend/json.hpp"

#include <string>
#include <utility>

namespace secure_frontend {
namespace {

constexpr JsonLimits kMessageLimits{.max_bytes = 1U << 20U, .max_depth = 16U, .max_container_entries = 1024U,
                                    .max_string_bytes = 64U * 1024U, .allow_fractions = true};

const JsonValue* member(const JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& m : *members) {
    if (m.key == key) return &m.value;
  }
  return nullptr;
}

std::string json_quoted(const std::string_view text) {
  std::string out;
  append_json_string(&out, text);
  return out;
}

std::string reply(const std::string& id, const std::string& result) {
  return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result + "}";
}

std::string error_reply(const std::string& id, const int code, const std::string_view message) {
  return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":" + json_quoted(message) + "}}";
}

std::string tool_list() {
  return "{\"tools\":[{\"name\":" + json_quoted(kWebSearchTool) + ",\"description\":" + json_quoted(kWebSearchDescription) +
         ",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":"
         "\"What to search for\"},\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":10,\"description\":"
         "\"How many results to get (1-10)\"}},\"required\":[\"query\"]}}]}";
}

std::string tool_result(const std::string_view text, const bool error) {
  return "{\"content\":[{\"type\":\"text\",\"text\":" + json_quoted(text) + "}],\"isError\":" + (error ? "true" : "false") + "}";
}

}  // namespace

SearchMcpServer::SearchMcpServer(Search search, const std::size_t default_results)
    : search_(std::move(search)), default_results_(default_results) {}

std::vector<std::string> SearchMcpServer::handle(std::string_view line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.remove_suffix(1);
  while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
  if (line.empty()) return {};
  const auto parsed = parse_json(line, kMessageLimits);
  const auto* message = std::get_if<JsonValue>(&parsed);
  if (message == nullptr) return {error_reply("null", -32700, "parse error")};
  if (message->if_object() == nullptr) return {error_reply("null", -32600, "a message must be one JSON-RPC object")};
  const auto* id = member(*message, "id");
  std::string id_text = "null";
  if (id != nullptr && id->if_integer() != nullptr) id_text = std::to_string(*id->if_integer());
  else if (id != nullptr && id->if_string() != nullptr) id_text = json_quoted(*id->if_string());
  else if (id != nullptr) return {error_reply("null", -32600, "the id must be a number or a string")};
  const auto* version = member(*message, "jsonrpc");
  const auto* method = member(*message, "method");
  if (version == nullptr || version->if_string() == nullptr || *version->if_string() != "2.0" || method == nullptr ||
      method->if_string() == nullptr) {
    return id == nullptr ? std::vector<std::string>{} : std::vector<std::string>{error_reply(id_text, -32600, "not a JSON-RPC 2.0 request")};
  }
  if (id == nullptr) return {};  // a notification: notifications/initialized, notifications/cancelled, any other
  const std::string& name = *method->if_string();
  if (name == "initialize") {
    return {reply(id_text, "{\"protocolVersion\":" + json_quoted(kMcpProtocolVersion) +
                               ",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"strata-search\",\"version\":\"1\"}}")};
  }
  if (name == "ping") return {reply(id_text, "{}")};
  if (name == "tools/list") return {reply(id_text, tool_list())};
  if (name != "tools/call") return {error_reply(id_text, -32601, "method not supported")};
  const auto* params = member(*message, "params");
  const auto* tool = params != nullptr ? member(*params, "name") : nullptr;
  if (tool == nullptr || tool->if_string() == nullptr || *tool->if_string() != kWebSearchTool) {
    return {error_reply(id_text, -32602, "there is no such tool")};
  }
  const auto* arguments = member(*params, "arguments");
  const auto* query = arguments != nullptr ? member(*arguments, "query") : nullptr;
  if (query == nullptr || query->if_string() == nullptr || query->if_string()->empty()) {
    return {error_reply(id_text, -32602, "web_search needs a query (a non-empty string)")};
  }
  std::size_t results = default_results_;
  if (const auto* count = member(*arguments, "max_results"); count != nullptr) {
    const auto* n = count->if_integer();
    if (n == nullptr || *n < 1 || *n > 10) return {error_reply(id_text, -32602, "max_results must be an integer 1-10")};
    results = static_cast<std::size_t>(*n);
  }
  const auto found = search_ ? search_(*query->if_string(), results) : SearchText{std::in_place_index<1>, "no search"};
  if (const auto* text = std::get_if<0>(&found)) return {reply(id_text, tool_result(*text, false))};
  return {reply(id_text, tool_result("The search failed: " + std::get<1>(found), true))};
}

}  // namespace secure_frontend
