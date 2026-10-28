#pragma once

// The calls a Qwen reply writes in upstream's template form (web search, docs/superpowers/specs/
// 2026-10-27-web-search-design.md §2): `<tool_call>\n<function=NAME>\n<parameter=K>\nV\n</parameter>\n</function>\n
// </tool_call>`, read as upstream's serve/frontend.py reads them for run_with_mcp - OutputParser with the server's tools
// declared and declared_only (recover and stream_tools off, the defaults), and parse_tool_call for one call.

#include "secure_frontend/code_tracking.hpp"
#include "secure_frontend/tool_calls.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// upstream's parse_tool_call over one call body (the text between <tool_call> and </tool_call>): the body stripped
// must start "<function=" and hold a ">"; the name runs to the first ">", stripped; each "<parameter=P>" value ends at
// the first "</parameter>" that is followed (after whitespace) by "<parameter=" or "</function>" (else the rest), and
// loses one leading and one trailing newline; a parameter `declaration` types "string" keeps its text, any other is the
// JSON its text holds (parse_json with fractions), else the text. nullopt: a malformed body (upstream's ValueError).
// Python's json.loads also reads NaN, Infinity, integers past 64 bits and repeated keys; here those stay text.
[[nodiscard]] std::optional<ToolCall> parse_qwen_tool_call(std::string_view body, const ToolDeclaration* declaration);

// upstream's OutputParser for the calls (a ToolCallParser): text that is no call goes out as reasoning or content as
// soon as it cannot be part of one (a partial tag, and in the answer the newlines before it, are held), and a call for
// a declared tool goes out as a call event once its </tool_call> (or the next call's opener) has arrived:
//
//  - in the answer: <tool_call> then (after whitespace) <function=NAME> for a declared NAME, outside a code fence and
//    inline code (#1058); the drifted openers <=NAME>, <function:NAME>, <function =NAME> and <NAME> name it too (#3441);
//    the text before it loses its trailing newlines (all of it when blank), the newlines after it are dropped; anything
//    else is text, and a call cut short is text at the end (<tool_call> and what followed);
//  - in the reasoning (#804/#1058): a <tool_call> at the start of a line outside code is held whole (32768 characters at
//    most); a declared call waits, and later ones with it, while only whitespace follows; the end of the reasoning, or a
//    natural end of the reply, makes them calls; any other text makes them the reasoning text they were;
//  - a call body that does not parse is the text it was (upstream's recover path; without it upstream ends the request).
//
// The tool names hold no ".", so upstream's namespace__member aliases (PR #3556) never apply.
class QwenToolCallParser final : public ToolCallParser {
 public:
  explicit QwenToolCallParser(std::vector<ToolDeclaration> tools);

  [[nodiscard]] std::vector<ToolParseEvent> feed_reasoning(std::string_view text) override;
  [[nodiscard]] std::vector<ToolParseEvent> end_reasoning() override;
  [[nodiscard]] std::vector<ToolParseEvent> feed_content(std::string_view text) override;
  // Calls waiting in the reasoning become calls only at a natural end.
  [[nodiscard]] std::vector<ToolParseEvent> finish(bool natural) override;

 private:
  enum class State { reasoning, rcall, content, call };
  struct Pending final {
    std::string raw;
    std::optional<ToolCall> call;
  };
  using Tracking = CodeTracking;  // upstream's _track: an act told from a quote

  [[nodiscard]] const ToolDeclaration* declared(std::string_view name) const;
  void emit(std::vector<ToolParseEvent>& out, ToolParseEvent::Kind kind, std::string text);
  void release(std::vector<ToolParseEvent>& out, bool deliver);
  void close_reasoning(std::vector<ToolParseEvent>& out);
  void start_content(bool lead);
  void run_reasoning(std::vector<ToolParseEvent>& out);
  void run_content(std::vector<ToolParseEvent>& out);
  void hold_before(std::vector<ToolParseEvent>& out, std::size_t at);
  [[nodiscard]] std::ptrdiff_t wrapped_call_end();
  void finish_call(std::vector<ToolParseEvent>& out, const std::string& body);

  std::vector<ToolDeclaration> tools_;
  State state_{State::reasoning};
  std::string buf_;
  bool lead_{false};
  std::vector<Pending> pending_;
  Tracking tracking_;
  std::size_t call_end_checked_{0};  // upstream's _call_end_checked: how much of a call body holds no </tool_call>
  bool call_end_seen_{false};        // a </tool_call> candidate arrived: the full walk from then on
};

}  // namespace secure_frontend
