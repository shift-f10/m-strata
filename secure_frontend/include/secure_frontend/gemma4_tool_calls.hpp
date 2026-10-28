#pragma once

// The calls a Gemma 4 reply writes in its template's form (web search for Gemma 4, owner 2026-10-28):
// `<|tool_call>call:NAME{key:value,...}<tool_call|>`, read as llama.cpp 3cf03257 reads them - common/parsers/gemma4.cpp's
// PEG rules (common_chat_params_init_gemma4) and gemma4_to_json in common/chat-peg-parser.cpp.

#include "secure_frontend/tool_calls.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// One call's body, between <|tool_call> and <tool_call|>: "call:", a declared tool's name followed by "{", then one
// gemma4-dict and nothing more. The dict is "{" ws ("}" | kv ("," ws kv)* ws "}"), a kv a key (one or more bytes but
// ":" and "}") then ":" ws and a value; a value is a string (<|"|>, anything up to the next <|"|>, <|"|>), a dict, an
// array ("[" ws ("]" | value ("," ws value)* ws "]")), a JSON number, true, false or null; ws is C isspace. The dict's
// members are the call's arguments in the order written (a repeated key keeps its first place and its last value, as in
// a nested dict); a string is its text, a number the JSON number. nullopt: a body that grammar does not accept, an
// undeclared name, or nesting past 64 levels.
[[nodiscard]] std::optional<ToolCall> parse_gemma4_tool_call(std::string_view body, std::span<const ToolDeclaration> tools);

// llama.cpp's reading of a Gemma 4 reply for the calls (a ToolCallParser): the reasoning (the thought channel) is
// reasoning - no call is read there; in the answer, text before <|tool_call> is content as soon as it cannot be the start
// of that tag, and a call runs to the first <tool_call|>. A call that parses (parse_gemma4_tool_call) is a call event;
// one that does not, and one cut short at the end, is the text it was, its tags included (llama.cpp's grammar refuses
// both). The run has already dropped the newlines after <channel|>; nothing more is dropped here.
class Gemma4ToolCallParser final : public ToolCallParser {
 public:
  explicit Gemma4ToolCallParser(std::vector<ToolDeclaration> tools);

  [[nodiscard]] std::vector<ToolParseEvent> feed_reasoning(std::string_view text) override;
  [[nodiscard]] std::vector<ToolParseEvent> end_reasoning() override;
  [[nodiscard]] std::vector<ToolParseEvent> feed_content(std::string_view text) override;
  [[nodiscard]] std::vector<ToolParseEvent> finish(bool natural) override;

 private:
  void run(std::vector<ToolParseEvent>& out, bool final);

  std::vector<ToolDeclaration> tools_;
  std::string buf_;
  bool in_call_{false};
};

}  // namespace secure_frontend
