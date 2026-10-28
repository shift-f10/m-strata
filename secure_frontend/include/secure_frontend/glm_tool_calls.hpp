#pragma once

// The calls a GLM-5.3 reply writes in its template's form (web search, docs/superpowers/specs/
// 2026-10-27-web-search-design.md §2, phase 5): `<tool_call>NAME<arg_key>K</arg_key><arg_value>V</arg_value>...
// </tool_call>`, read as Project Maya v1.1.30's serve/frontend.py reads them for its run_with_mcp - OutputParser with the
// server's tools and stream_tools (its Service.run's), and parse_tool_call for one call.

#include "secure_frontend/code_tracking.hpp"
#include "secure_frontend/tool_calls.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// Maya's parse_tool_call over one call body. GLM's form: the body stripped, the name up to its first "<" (stripped),
// then each <arg_key>K</arg_key> (K stripped) followed, after whitespace only, by <arg_value>V</arg_value> (V as written,
// to its </arg_value> or the end). Or Qwen's <function=NAME> form, as Maya reads it: each value to its first
// </parameter>, one newline off each end. A parameter `declaration` types "string" keeps its text; any other is the
// JSON the text holds (parse_json with fractions), else the text; a repeated key keeps its first place, its last value.
// nullopt: a malformed body (Maya's ValueError).
[[nodiscard]] std::optional<ToolCall> parse_glm_tool_call(std::string_view body, const ToolDeclaration* declaration);

// Maya's OutputParser for the calls (a ToolCallParser):
//  - the reasoning is reasoning: Maya reads no call there;
//  - in the answer, a <tool_call> outside a code fence and inline code (#1058) starts a call - the text before it loses
//    its trailing newlines (all of it when blank) - and the call ends at the first </tool_call>, the newlines after it
//    dropped; a partial tag, and the newlines before it, are held;
//  - at the end, a call cut short whose name has arrived (the name runs to the first "<" or line break) is a call with
//    no arguments, as Maya's streaming scan delivers it; one without its name is text, with its <tool_call>.
// Two rules are ours: a call names a tool the server declared, else it is the text it was (in Maya a call for another
// tool is the client's, and our clients declare none); and a body that does not parse is text too (Maya's request
// fails).
class GlmToolCallParser final : public ToolCallParser {
 public:
  explicit GlmToolCallParser(std::vector<ToolDeclaration> tools);

  [[nodiscard]] std::vector<ToolParseEvent> feed_reasoning(std::string_view text) override;
  [[nodiscard]] std::vector<ToolParseEvent> end_reasoning() override;
  [[nodiscard]] std::vector<ToolParseEvent> feed_content(std::string_view text) override;
  [[nodiscard]] std::vector<ToolParseEvent> finish(bool natural) override;

 private:
  enum class State { reasoning, content, call };
  [[nodiscard]] const ToolDeclaration* declared(std::string_view name) const;
  void emit(std::vector<ToolParseEvent>& out, ToolParseEvent::Kind kind, std::string text);
  void start_content(bool lead);
  void run(std::vector<ToolParseEvent>& out);

  std::vector<ToolDeclaration> tools_;
  State state_{State::reasoning};
  std::string buf_;
  bool lead_{false};
  CodeTracking tracking_;
};

}  // namespace secure_frontend
