#pragma once

// The calls a DeepSeek-V4.1 reply writes (web search; owner 2026-10-28: "deepseek does not seem to have mcp search
// access"): DSML - `<｜DSML｜ calls>` around `<｜DSML｜ invoke name="N">` blocks of `<｜DSML｜ parameter name="K"
// string="true|false">V</｜DSML｜ parameter>` - read as Project Maya v1.1.30's serve/frontend.py reads them: its
// DsmlRewriter turns them, as they stream, into GLM's form, and its OutputParser (GlmToolCallParser here) reads that
// with no schema, so every value is the JSON the rewriter wrote (a string="true" value JSON-quoted).

#include "secure_frontend/glm_tool_calls.hpp"
#include "secure_frontend/tool_calls.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// Maya's DsmlRewriter: DSML in, GLM's `<tool_call>N<arg_key>K</arg_key><arg_value>V</arg_value>...</tool_call>` out.
//  - A tag split across feeds is held until it is whole (a "<" that may still become `<｜DSML｜` or `</｜DSML｜`).
//  - `calls` tags go; inside a calls block, text that is only whitespace goes.
//  - `invoke` opens a call with its name and closes it.
//  - `parameter` opens a value. Inside it the only structural token is the whole closing tag `</｜DSML｜ parameter>`
//    (a "<" or another DSML tag is data). A string="true" value goes out JSON-quoted piece by piece as it streams;
//    any other value goes out whole at its close, with "<" as \u003c.
//  - Other DSML tags pass as they are, and text outside a value passes as it is.
class DsmlRewriter final {
 public:
  [[nodiscard]] std::string feed(std::string_view text);
  // The held text as plain text (Maya's finish; the state stays, so a later feed goes on).
  [[nodiscard]] std::string finish();

 private:
  [[nodiscard]] std::string close_value();
  [[nodiscard]] std::string plain(std::string_view text);
  [[nodiscard]] std::string tag(std::string_view whole, bool closing, std::string_view body);

  std::string buf_;
  bool in_calls_{false};
  bool in_param_{false};
  bool param_string_{false};
  std::string param_text_;
};

// A ToolCallParser for DeepSeek's calls: both channels through one DsmlRewriter (as Maya rewrites the whole reply,
// reasoning included), then Maya's GLM reading. The tools' string parameters are not used: a DSML call says itself which
// values are strings. A call for a tool the server did not declare is text (GlmToolCallParser's rule).
class DeepseekToolCallParser final : public ToolCallParser {
 public:
  explicit DeepseekToolCallParser(std::vector<ToolDeclaration> tools);

  [[nodiscard]] std::vector<ToolParseEvent> feed_reasoning(std::string_view text) override;
  [[nodiscard]] std::vector<ToolParseEvent> end_reasoning() override;
  [[nodiscard]] std::vector<ToolParseEvent> feed_content(std::string_view text) override;
  [[nodiscard]] std::vector<ToolParseEvent> finish(bool natural) override;

 private:
  DsmlRewriter rewriter_;
  GlmToolCallParser glm_;
  bool answering_{false};  // the answer has started (end_reasoning, or content without reasoning)
};

}  // namespace secure_frontend
