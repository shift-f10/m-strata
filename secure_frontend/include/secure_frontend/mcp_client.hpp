#pragma once

// The server's MCP client (docs/superpowers/specs/2026-10-27-web-search-design.md §2): upstream's serve/mcp.py (and Maya
// v1.1.30's, the same client) for one stdio server - newline-delimited JSON-RPC 2.0, protocol 2025-06-18: initialize,
// notifications/initialized, tools/list (paged), tools/call, notifications/cancelled on a timeout or a cancel; a
// server's ping answered, any other server request refused (-32601), notifications ignored, a reply to another id
// dropped. A call's content becomes text as upstream's result_text makes it, cut at max_result_chars characters (code
// points) with upstream's note. The transport is the child's stdio (EngineStdioProcess, the bundled strata-search).

#include "secure_frontend/engine_protocol_client.hpp"
#include "secure_frontend/json.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

struct McpTool final {
  std::string name;
  std::string description;   // (the tool's title when it has no description)
  std::string input_schema;  // the inputSchema object as compact JSON ({"type":"object","properties":{}} when none)
};

struct McpCallResult final {
  bool ok{false};
  std::string text;       // upstream's: the content's text, "error: ..." when it failed
  std::size_t chars{0};   // the whole text's length in characters, before any cut
  bool truncated{false};
  bool cancelled{false};  // the chat stopped while it waited (upstream's McpCancelled: the loop ends)
};

struct McpLimits final {
  std::chrono::milliseconds start_timeout{std::chrono::seconds(120)};  // initialize and tools/list
  std::chrono::milliseconds call_timeout{std::chrono::seconds(60)};    // each tools/call
  std::size_t max_result_chars{20000};
};

class McpClient final {
 public:
  McpClient(EngineLineTransport& transport, McpLimits limits);

  // The handshake and the tool list: nullopt, or why it failed (fixed text, never the server's).
  [[nodiscard]] std::optional<std::string> start();
  [[nodiscard]] const std::vector<McpTool>& tools() const noexcept { return tools_; }
  // One tools/call with `arguments_json` (an object); `cancelled` is asked while waiting.
  [[nodiscard]] McpCallResult call(std::string_view name, std::string_view arguments_json, const std::function<bool()>& cancelled);

 private:
  EngineLineTransport& transport_;
  McpLimits limits_;
  std::vector<McpTool> tools_;
  std::int64_t next_id_{1};
};

// upstream's result_text over a tools/call result object: text blocks as they are; a resource its text, else a note; a
// resource link, an image or audio a note; joined with newlines; structuredContent as JSON when there are no parts.
[[nodiscard]] std::string mcp_result_text(const JsonValue& result);

}  // namespace secure_frontend
