#pragma once

// The server's tools, as the chat route uses them: what to offer the model and how to run one call. The tool hub
// (tool_hub.hpp: the tool plugins that are on, web search among them; docs/superpowers/specs/2026-10-30-engine-family-
// plugins-design.md, section 10) is the one implementation; with none a route serves every chat as before.

#include "secure_frontend/tool_calls.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
  // The tools for a chat's prompt (their programs started on first use); none: the chat is served without.
  [[nodiscard]] virtual std::vector<ToolOffer> offers() = 0;
  // One call, to the tool it names; `cancelled` is asked while it waits.
  [[nodiscard]] virtual ToolResult run(const ToolCall& call, const std::function<bool()>& cancelled) = 0;
  // A chat they were offered to is served without them: the model's prompt cannot carry the tools (its vocabulary
  // lacks a tag that prompt needs); `why` is the plan's fixed reason (owner 2026-10-28: flint's fallback was silent).
  virtual void served_without(const std::string_view why) { (void)why; }
  // What a chat's system message says while the tools are offered (owner 2026-10-28: today's date, and that the model's
  // knowledge may be out of date so it should search for anything recent); empty: nothing.
  [[nodiscard]] virtual std::string system_note() const { return {}; }
};

}  // namespace secure_frontend
