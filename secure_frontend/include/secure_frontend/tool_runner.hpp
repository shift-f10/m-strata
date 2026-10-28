#pragma once

// The server's own tool (web search, docs/superpowers/specs/2026-10-27-web-search-design.md §2), as the chat route uses
// it: what to offer the model and how to run one call. The search service (search_service.hpp, built only with
// SECURE_FRONTEND_WEB_SEARCH) is the one implementation; without it a route has none and serves every chat as before.

#include "secure_frontend/tool_calls.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

struct ToolOffer final {
  ToolDeclaration declaration;  // its name and string parameters, for the reply's parser
  std::string template_json;    // the tool as the template's tojson writes it: {"name", "description", "parameters"}
};

// One call's outcome, as upstream's McpHub.call reports it.
struct ToolResult final {
  bool ok{false};
  std::string text;               // what the model reads ("error: ..." when the call failed)
  std::size_t chars{0};           // the whole text's characters, before any cut
  bool truncated{false};
  bool cancelled{false};          // the chat stopped while it waited: the answer ends here
  std::chrono::milliseconds took{0};
};

class ToolRunner {
 public:
  virtual ~ToolRunner() = default;
  // The tool for a chat's prompt (its program started on first use); nullopt: none now - the chat is served without.
  [[nodiscard]] virtual std::optional<ToolOffer> offer() = 0;
  // One call; `cancelled` is asked while it waits.
  [[nodiscard]] virtual ToolResult run(const ToolCall& call, const std::function<bool()>& cancelled) = 0;
  // A chat it was offered to is served without it: the model's prompt cannot carry the tool (its vocabulary lacks a tag
  // that prompt needs); `why` is the plan's fixed reason (owner 2026-10-28: flint's fallback was silent).
  virtual void served_without(const std::string_view why) { (void)why; }
};

}  // namespace secure_frontend
