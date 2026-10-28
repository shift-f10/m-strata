// The server's MCP client (docs/superpowers/specs/2026-10-27-web-search-design.md §2, phase 2): upstream's serve/mcp.py
// over one stdio server - the handshake, the paged tool list, a call and the server's own messages around it, a timeout
// and a cancel, upstream's result text and its cut.
#include "secure_frontend/mcp_client.hpp"
#include "support/test_support.hpp"

#include <deque>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;
void expect(const bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::cerr << "FAILED: " << what << '\n';
  }
}

// A server on a scripted channel: each line the client writes is recorded and answered by `respond`.
class FakeServer final : public secure_frontend::EngineLineTransport {
 public:
  std::vector<std::string> written;
  std::function<std::vector<std::string>(const std::string&)> respond;
  bool write_line(const std::string_view line, secure_frontend::MonotonicClock::time_point, std::string&) override {
    written.emplace_back(line);
    if (respond) {
      for (auto& reply : respond(written.back())) pending_.push_back(std::move(reply));
    }
    return true;
  }
  secure_frontend::EngineReadResult read_line(std::chrono::milliseconds) override {
    if (pending_.empty()) return {secure_frontend::EngineReadKind::timeout, {}, {}};
    auto line = std::move(pending_.front());
    pending_.pop_front();
    return {secure_frontend::EngineReadKind::line, std::move(line), {}};
  }

 private:
  std::deque<std::string> pending_;
};

bool has(const std::string& text, const std::string_view part) { return text.find(part) != std::string::npos; }

const std::string kInitialized = R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-06-18","capabilities":{"tools":{}},"serverInfo":{"name":"strata-search","version":"1"}}})";

void the_handshake_and_the_tool_list_are_upstreams() {
  FakeServer server;
  server.respond = [](const std::string& line) -> std::vector<std::string> {
    if (has(line, "\"initialize\"")) return {kInitialized};
    if (has(line, "\"cursor\":\"p2\"")) {
      return {R"({"jsonrpc":"2.0","id":3,"result":{"tools":[{"name":"web_search","title":"Search","inputSchema":{"type":"object","properties":{"query":{"type":"string"}},"required":["query"]}}]}})"};
    }
    if (has(line, "\"tools/list\"")) {
      return {R"({"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"a","description":"A tool"},{"description":"no name: dropped"}],"nextCursor":"p2"}})"};
    }
    return {};
  };
  secure_frontend::McpClient client(server, {});
  const auto failure = client.start();
  expect(!failure.has_value(), "the handshake succeeds: " + failure.value_or(""));
  expect(server.written.size() == 4U, "four messages: initialize, initialized, two tools/list pages");
  if (server.written.size() != 4U) return;
  expect(server.written[0] ==
             R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"strata","title":"Strata","version":"1"}}})",
         "initialize as upstream's client sends it: " + server.written[0]);
  expect(server.written[1] == R"({"jsonrpc":"2.0","method":"notifications/initialized"})", "then notifications/initialized");
  expect(server.written[2] == R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})" &&
             server.written[3] == R"({"jsonrpc":"2.0","id":3,"method":"tools/list","params":{"cursor":"p2"}})",
         "tools/list, paged by nextCursor");
  const auto& tools = client.tools();
  expect(tools.size() == 2U && tools[0].name == "a" && tools[0].description == "A tool" &&
             tools[0].input_schema == R"({"type": "object", "properties": {}})",
         "a tool without a schema gets an empty object's (json.dumps' spacing)");
  expect(tools.size() == 2U && tools[1].name == "web_search" && tools[1].description == "Search" &&
             tools[1].input_schema == R"({"type": "object", "properties": {"query": {"type": "string"}}, "required": ["query"]})",
         "the title stands in for a missing description; the schema as json.dumps writes it: " +
             (tools.size() == 2U ? tools[1].input_schema : std::string{}));

  FakeServer refusing;
  refusing.respond = [](const std::string& line) -> std::vector<std::string> {
    if (has(line, "\"initialize\"")) return {kInitialized};
    if (has(line, "\"tools/list\"")) return {R"({"jsonrpc":"2.0","id":2,"error":{"code":-32601,"message":"no tools"}})"};
    return {};
  };
  secure_frontend::McpClient no_tools(refusing, {});
  expect(!no_tools.start().has_value() && no_tools.tools().empty(), "-32601 to tools/list: a server without tools, not a failure");

  FakeServer broken;
  broken.respond = [](const std::string& line) -> std::vector<std::string> {
    if (has(line, "\"initialize\"")) return {R"({"jsonrpc":"2.0","id":1,"error":{"code":-32603,"message":"boom"}})"};
    return {};
  };
  secure_frontend::McpClient failed(broken, {});
  expect(failed.start().has_value(), "an initialize error fails the start");
  FakeServer silent;
  secure_frontend::McpClient late(silent, {.start_timeout = std::chrono::milliseconds(30)});
  expect(late.start().has_value(), "no answer within start_timeout fails the start");
}

void a_call_answers_the_servers_own_messages_and_returns_the_text() {
  FakeServer server;
  server.respond = [](const std::string& line) -> std::vector<std::string> {
    if (has(line, "\"initialize\"")) return {kInitialized};
    if (has(line, "\"tools/list\"")) return {R"({"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"web_search"}]}})"};
    if (has(line, R"("query":"boom")")) return {R"({"jsonrpc":"2.0","id":4,"result":{"content":[{"type":"text","text":"down"}],"isError":true}})"};
    if (has(line, R"("query":"bad")")) return {R"({"jsonrpc":"2.0","id":5,"error":{"code":-32602,"message":"bad query"}})"};
    if (has(line, "\"tools/call\"")) {
      return {R"({"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info","data":"x"}})",
              R"({"jsonrpc":"2.0","id":"s1","method":"ping"})", R"({"jsonrpc":"2.0","id":"s2","method":"roots/list"})",
              R"({"jsonrpc":"2.0","id":99,"result":{"content":[{"type":"text","text":"stale"}]}})", "not json from a chatty server",
              R"({"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":"RESULTS é"}]}})"};
    }
    return {};
  };
  secure_frontend::McpClient client(server, {});
  expect(!client.start().has_value(), "started");
  const auto found = client.call("web_search", R"({"query":"q"})", {});
  expect(found.ok && found.text == "RESULTS \xC3\xA9" && found.chars == 9U && !found.truncated && !found.cancelled,
         "the call's text (characters counted as code points): " + found.text);
  expect(server.written.size() >= 6U && server.written[3] ==
                                            R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"web_search","arguments":{"query":"q"}}})",
         "tools/call with the name and the arguments");
  expect(server.written.size() >= 6U && server.written[4] == R"({"jsonrpc":"2.0","id":"s1","result":{}})" &&
             server.written[5] == R"({"jsonrpc":"2.0","id":"s2","error":{"code":-32601,"message":"method not supported"}})",
         "the server's ping is answered, its other request refused; its notification and a stale reply are dropped");
  const auto failed = client.call("web_search", R"({"query":"boom"})", {});
  expect(!failed.ok && failed.text == "error: down", "isError: upstream's \"error: \" and the text");
  const auto refused = client.call("web_search", R"({"query":"bad"})", {});
  expect(!refused.ok && refused.text == "error: bad query (code -32602)", "a JSON-RPC error: its message and code");
}

void a_timeout_or_a_cancel_sends_notifications_cancelled() {
  FakeServer server;
  server.respond = [](const std::string& line) -> std::vector<std::string> {
    if (has(line, "\"initialize\"")) return {kInitialized};
    if (has(line, "\"tools/list\"")) return {R"({"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"web_search"}]}})"};
    return {};
  };
  secure_frontend::McpClient client(server, {.call_timeout = std::chrono::milliseconds(40)});
  expect(!client.start().has_value(), "started");
  const auto late = client.call("web_search", R"({"query":"q"})", {});
  expect(!late.ok && !late.cancelled && late.text == "error: tools/call timed out after 0.04 s", "a timeout: upstream's text: " + late.text);
  expect(!server.written.empty() &&
             server.written.back() == R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":3,"reason":"tools/call timed out after 0.04 s"}})",
         "notifications/cancelled names the request and why: " + (server.written.empty() ? std::string{} : server.written.back()));
  const auto stopped = client.call("web_search", R"({"query":"q"})", [] { return true; });
  expect(!stopped.ok && stopped.cancelled, "a cancel: cancelled");
  expect(server.written.back() == R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":4,"reason":"stopped"}})",
         "the cancel tells the server too");
}

void long_text_is_cut_with_upstreams_note() {
  const auto cut_of = [](const std::string& text, const std::size_t cap) {
    FakeServer server;
    std::string quoted;
    secure_frontend::append_json_string(&quoted, text);
    server.respond = [quoted](const std::string& line) -> std::vector<std::string> {
      if (has(line, "\"initialize\"")) return {kInitialized};
      if (has(line, "\"tools/list\"")) return {R"({"jsonrpc":"2.0","id":2,"result":{"tools":[{"name":"t"}]}})"};
      if (has(line, "\"tools/call\"")) return {R"({"jsonrpc":"2.0","id":3,"result":{"content":[{"type":"text","text":)" + quoted + "}]}}"};
      return {};
    };
    secure_frontend::McpClient client(server, {.max_result_chars = cap});
    (void)client.start();
    return client.call("t", "{}", {});
  };
  const auto brief = cut_of("\xC3\xA9" + std::string(24, 'a'), 10U);
  expect(brief.ok && brief.truncated && brief.chars == 25U &&
             brief.text == "\xC3\xA9" + std::string(9, 'a') + "\n\n[... truncated: the tool returned 25 characters; only the first 10 are shown]",
         "cut on characters, upstream's note: " + brief.text);
  const auto big = cut_of(std::string(25000, 'x'), 20000U);
  expect(big.truncated && has(big.text, "the tool returned 25,000 characters; only the first 20,000 are shown"), "thousands as Python's {:,}");
  const auto fits = cut_of("short", 10U);
  expect(fits.text == "short" && !fits.truncated, "a text that fits is not cut");
}

void content_blocks_become_upstreams_text() {
  const auto text_of = [](const std::string& json) {
    const auto parsed = secure_frontend::parse_json(json, secure_frontend::JsonLimits{1U << 20U, 16U, 1024U, 1U << 20U, true});
    return std::holds_alternative<secure_frontend::JsonValue>(parsed) ? secure_frontend::mcp_result_text(std::get<secure_frontend::JsonValue>(parsed))
                                                                     : std::string{"<bad json>"};
  };
  expect(text_of(R"({"content":[{"type":"text","text":"a"},{"type":"image","mimeType":"image/png","data":"AAAAAAAA"},)"
                 R"({"type":"resource","resource":{"uri":"u","text":"R"}},{"type":"resource","resource":{"uri":"u2"}},)"
                 R"({"type":"resource_link","name":"n","uri":"u3"},{"type":"resource_link","uri":"u4"},{"type":"video","x":1}]})") ==
             "a\n[image (image/png, 6 bytes): not shown to the model]\nR\n[resource u2 (binary), not shown]\n[resource link: n u3]\n"
             "[resource link: u4]\n{\"type\": \"video\", \"x\": 1}",
         "text, image, resource (text or a note), resource link, an unknown block as json.dumps writes it: " +
             text_of(R"({"content":[{"type":"resource_link","uri":"u4"}]})"));
  expect(text_of(R"({"content":[],"structuredContent":{"x":[1,2.5,"y"],"z":null,"w":true}})") ==
             R"({"x": [1, 2.5, "y"], "z": null, "w": true})",
         "no parts: structuredContent as json.dumps writes it");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"the_handshake_and_the_tool_list_are_upstreams", the_handshake_and_the_tool_list_are_upstreams},
      {"a_call_answers_the_servers_own_messages_and_returns_the_text", a_call_answers_the_servers_own_messages_and_returns_the_text},
      {"a_timeout_or_a_cancel_sends_notifications_cancelled", a_timeout_or_a_cancel_sends_notifications_cancelled},
      {"long_text_is_cut_with_upstreams_note", long_text_is_cut_with_upstreams_note},
      {"content_blocks_become_upstreams_text", content_blocks_become_upstreams_text},
  }, failures);
}
