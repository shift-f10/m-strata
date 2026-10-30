// A stand-in tool plugin for the tool hub's tests (plugin spec 2026-10-30, section 10): an MCP server on stdio, JSON-RPC
// one message a line, as strata-search speaks it. It lists one tool per --tool NAME (each with a string parameter
// "query"), and answers a tools/call with what it was given: the tool's name, its arguments, its own command line and
// the FAKE_TOOL_TOKEN variable it got (or "none"), so a test sees what reaches a plugin's process. --listing-only makes
// it list its tools and then answer nothing (a stuck plugin); never installed.
#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/json.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

constexpr secure_frontend::JsonLimits kLimits{.max_bytes = 1U << 20U, .max_depth = 16U, .max_container_entries = 1024U,
                                              .max_string_bytes = 64U * 1024U, .allow_fractions = true};

const secure_frontend::JsonValue* member(const secure_frontend::JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& m : *members) {
    if (m.key == key) return &m.value;
  }
  return nullptr;
}

std::string quoted(const std::string_view text) {
  std::string out;
  secure_frontend::append_json_string(&out, text);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  (void)_setmode(_fileno(stdin), _O_BINARY);
  (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
  std::vector<std::string> tools;
  std::string command_line;
  bool listing_only = false;
  for (int i = 1; i < argc; ++i) {
    command_line += (i > 1 ? " " : "") + std::string{argv[i]};
    if (std::string_view{argv[i]} == "--tool" && i + 1 < argc) tools.emplace_back(argv[i + 1]);
    if (std::string_view{argv[i]} == "--listing-only") listing_only = true;
  }
  const char* token = secure_frontend::crt_getenv("FAKE_TOOL_TOKEN");
  std::string line;
  while (std::getline(std::cin, line)) {
    const auto parsed = secure_frontend::parse_json(line, kLimits);
    const auto* message = std::get_if<secure_frontend::JsonValue>(&parsed);
    if (message == nullptr) continue;
    const auto* id = member(*message, "id");
    const auto* method = member(*message, "method");
    if (id == nullptr || method == nullptr || method->if_string() == nullptr) continue;  // a notification
    const std::string request_id = secure_frontend::python_json_text(*id);
    std::string result;
    if (*method->if_string() == "initialize") {
      result = R"({"protocolVersion":"2025-06-18","capabilities":{"tools":{}},"serverInfo":{"name":"fake-tool","version":"1"}})";
    } else if (*method->if_string() == "tools/list") {
      result = "{\"tools\":[";
      for (std::size_t i = 0; i < tools.size(); ++i) {
        result += (i == 0U ? "" : ",") + std::string{"{\"name\":"} + quoted(tools[i]) + ",\"description\":" +
                  quoted("Fake tool " + tools[i] + ".") +
                  R"(,"inputSchema":{"type":"object","properties":{"query":{"type":"string"}},"required":["query"]}})";
      }
      result += "]}";
    } else if (*method->if_string() == "tools/call") {
      if (listing_only) continue;  // (stuck: the call waits for its time limit)
      const auto* params = member(*message, "params");
      const auto* name = params != nullptr ? member(*params, "name") : nullptr;
      const auto* arguments = params != nullptr ? member(*params, "arguments") : nullptr;
      const std::string text = (name != nullptr && name->if_string() != nullptr ? *name->if_string() : std::string{"?"}) + " called with " +
                               (arguments != nullptr ? secure_frontend::python_json_text(*arguments) : std::string{"{}"}) +
                               "; args: " + command_line + "; token: " + (token != nullptr ? token : "none");
      result = "{\"content\":[{\"type\":\"text\",\"text\":" + quoted(text) + "}],\"isError\":false}";
    } else {
      std::cout << "{\"jsonrpc\":\"2.0\",\"id\":" << request_id << ",\"error\":{\"code\":-32601,\"message\":\"no such method\"}}\n" << std::flush;
      continue;
    }
    std::cout << "{\"jsonrpc\":\"2.0\",\"id\":" << request_id << ",\"result\":" << result << "}\n" << std::flush;
  }
  return 0;
}
