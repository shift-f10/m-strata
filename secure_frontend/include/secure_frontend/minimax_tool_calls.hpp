#pragma once

// The calls a MiniMax-M3.1 reply writes (web search; owner 2026-10-28: "I believe minimax may have the same issue"):
// `]<]minimax[>[<tool_call>` around `]<]minimax[>[<invoke name="N">` blocks of `]<]minimax[>[<K>V]<]minimax[>[</K>`
// (a value that holds such elements is an object, or an array when every one is `item`), read as Project Maya v1.1.30's
// serve/frontend.py reads them: its MinimaxRewriter turns them, as they stream, into GLM's form with JSON values, and its
// OutputParser (GlmToolCallParser here) reads that with no schema.

#include "secure_frontend/glm_tool_calls.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/tool_calls.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// Maya's MinimaxRewriter, for the namespace's tags (the think pair is our engine run's split, not text here):
//  - a tag split across feeds is held until it is whole;
//  - `tool_call` tags go, and so does any text between the calls' tags;
//  - `invoke` opens a call with its name and closes it;
//  - any other opening tag inside a call opens a value. A parameter the tool types as a string (`string_parameters`)
//    goes out JSON-quoted piece by piece as it streams, its tags being its text. Any other value goes out whole at its
//    own closing tag: a nested one as the object or array it spells, a plain one as JSON when it is a number, true,
//    false or null, else as a string;
//  - the namespace token not followed by a tag is text.
// finish() also closes an open value and an open call.
class MinimaxRewriter final {
 public:
  explicit MinimaxRewriter(std::vector<ToolDeclaration> tools);
  [[nodiscard]] std::string feed(std::string_view text);
  // What is held, as text; a value or a call still open is closed.
  [[nodiscard]] std::string finish();
  // The held text alone, as text (at the end of the reasoning: the state stays).
  [[nodiscard]] std::string flush();

 private:
  [[nodiscard]] std::string plain(std::string_view text);
  [[nodiscard]] std::string tag(std::string_view whole);
  [[nodiscard]] std::string close_value();

  std::vector<ToolDeclaration> tools_;
  std::string buf_;
  bool in_calls_{false};
  std::optional<std::string> call_;  // the invoke's tool name
  std::optional<std::string> key_;   // the parameter whose value is being read
  int depth_{0};
  std::string vtext_;
  bool vstring_{false};
  bool vnested_{false};
};

// A ToolCallParser for MiniMax's calls: both channels through one MinimaxRewriter (as Maya rewrites the whole reply,
// reasoning included), then Maya's GLM reading of rewritten calls (GlmToolCallParser, rewritten, no schema). A call for
// a tool the server did not declare is text (GlmToolCallParser's rule).
class MinimaxToolCallParser final : public ToolCallParser {
 public:
  explicit MinimaxToolCallParser(std::vector<ToolDeclaration> tools);

  [[nodiscard]] std::vector<ToolParseEvent> feed_reasoning(std::string_view text) override;
  [[nodiscard]] std::vector<ToolParseEvent> end_reasoning() override;
  [[nodiscard]] std::vector<ToolParseEvent> feed_content(std::string_view text) override;
  [[nodiscard]] std::vector<ToolParseEvent> finish(bool natural) override;

 private:
  MinimaxRewriter rewriter_;
  GlmToolCallParser glm_;
  bool answering_{false};  // the answer has started (end_reasoning, or content without reasoning)
};

}  // namespace secure_frontend
