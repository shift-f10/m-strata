#pragma once

// The server's own tool calls (web search, docs/superpowers/specs/2026-10-27-web-search-design.md §2): a call the model
// wrote, as its family's parser read it, and one round of the search loop as the next prompt renders it.

#include "secure_frontend/json.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// One argument: its name as written and its value as the parser decoded it (the text of a string parameter, else the
// JSON the text holds when it parses, else the text).
struct ToolArgument final {
  std::string name;
  JsonValue value;
};

struct ToolCall final {
  std::string name;
  std::vector<ToolArgument> arguments;  // in the order written; a repeated name keeps its first place and its last value
};

// A tool the server offers, as the parsers need it: its name and the parameters its schema types "string".
struct ToolDeclaration final {
  std::string name;
  std::vector<std::string> string_parameters;
};

// One round of the search loop, as upstream's run_with_mcp appends it to the conversation: the assistant's reasoning and
// content (each joined; the renderers strip them, as upstream does), its calls, and one result text per call.
struct ToolRound final {
  std::string reasoning;
  std::string content;
  std::vector<ToolCall> calls;
  std::vector<std::string> results;
};

// Which family's form the calls take: upstream's Qwen template (qwen_tool_calls.hpp) or Maya's GLM-5.3 one
// (glm_tool_calls.hpp); raon-1-mini's template writes Qwen's calls and flint's GLM's, each in its own prompt
// (owner 2026-10-28); Gemma 4's are its own <|tool_call>call:NAME{...}<tool_call|> (gemma4_tool_calls.hpp); DeepSeek-V4.1's
// are DSML, read as Maya reads them (deepseek_tool_calls.hpp).
enum class ToolFormat { qwen, glm, raon1, flint, gemma4, deepseek };

struct ToolParseEvent final {
  enum class Kind { reasoning, content, call };
  Kind kind;
  std::string text;  // reasoning, content: never empty
  ToolCall call{};   // call
};

// A reply's calls read out of its two channels as our generation run splits them (the reasoning by the pack's think
// ids, its end the </think> token): text that is no call goes out as reasoning or content as soon as it cannot be part
// of one, a call for a declared tool as a call event once it is whole.
class ToolCallParser {
 public:
  virtual ~ToolCallParser() = default;
  [[nodiscard]] virtual std::vector<ToolParseEvent> feed_reasoning(std::string_view text) = 0;
  // The </think> token (GenerationObserver::on_reasoning_end): the answer starts, its leading newlines dropped.
  [[nodiscard]] virtual std::vector<ToolParseEvent> end_reasoning() = 0;
  // The answer's text. Without end_reasoning first it starts the answer as is (a reply that never reasoned).
  [[nodiscard]] virtual std::vector<ToolParseEvent> feed_content(std::string_view text) = 0;
  // The end of the reply: `natural` when it ended as stop.
  [[nodiscard]] virtual std::vector<ToolParseEvent> finish(bool natural) = 0;
};

// The parser of a family's form, for the declared tools.
[[nodiscard]] std::unique_ptr<ToolCallParser> make_tool_call_parser(ToolFormat format, std::vector<ToolDeclaration> tools);

}  // namespace secure_frontend
