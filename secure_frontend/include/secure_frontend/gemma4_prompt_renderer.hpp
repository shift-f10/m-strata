#pragma once

#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/prompt_piece.hpp"
#include "secure_frontend/tool_calls.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// Gemma 4 plan 1 Task 8 (spec §2.5, §6.3): the fixed, text-only Gemma 4 prompt, byte for byte
// the text path of the GGUF's chat template (tokenizer.chat_template, lines 141-266):
//
//   <bos>
//   <|turn>system\n[<|think|>]{system | trim}<turn|>\n   only with a leading system message or thinking on
//   <|turn>user\n{user | trim}<turn|>\n
//   <|turn>model\n{assistant | trim}<turn|>\n            earlier answers (strip_thinking: no channel)
//   <|turn>model\n                                       the generation prompt
//   <|channel>thought\n<channel|>                        only with thinking off
//
// There is no newline anywhere but those shown: <|think|> is followed directly by the system text
// (the GGUF's template, not llama.cpp's newer copy, which writes "<|think|>\n").
// An optional leading system message, then strict user/assistant alternation ending in user. A
// non-leading system message, an unknown role, the parts form (this profile is text only), an
// empty or all-whitespace text, invalid UTF-8, a C0 control byte other than \t, \n and \r, or any
// of the 24 Gemma 4 control literals (gemma4_control_literals) anywhere in a text fails closed, so
// client text can never open or close a turn or a channel. This is not a Jinja implementation.
[[nodiscard]] std::optional<std::string> render_gemma4_text_prompt(const std::vector<ChatMessage>& messages, bool thinking);

// Jinja's trim filter: Python's str.strip() with no argument, which removes, at both ends, every
// code point for which str.isspace() is true. A fixed table of those 29 code points (CPython 3.12:
// U+0009-U+000D, U+001C-U+001F, U+0020, U+0085, U+00A0, U+1680, U+2000-U+200A, U+2028, U+2029,
// U+202F, U+205F, U+3000); U+200B, U+FEFF and U+180E are not whitespace and stay. The input must
// be valid UTF-8; the result is a view into it.
[[nodiscard]] std::string_view gemma4_trim(std::string_view text);

// The 24 CONTROL and USER_DEFINED literals of the Gemma 4 vocabulary (spec §2.5): the renderer's
// framing markers and every tool, image, audio and video marker. None may appear in client text.
[[nodiscard]] const std::array<std::string_view, 24U>& gemma4_control_literals();

// Web search for Gemma 4 (owner 2026-10-28): render_gemma4_text_prompt's conversation with the tools and the search
// rounds, byte for byte as the GGUF's template renders llama.cpp's messages for it (tests/fixtures/
// gemma4_tool_prompts.json; llama.cpp 3cf03257 is the Gemma 4 oracle):
//
//   <bos><|turn>system\n[<|think|>]{system | trim}<|tool>{declaration}<tool|><turn|>\n   always, with the tools
//   ...the conversation as render_gemma4_text_prompt writes it...
//   per round: <|turn>model\n{<|tool_call>call:NAME{key:value,...}<tool_call|>}{<|tool_response>RESPONSE<tool_response|>}
//   the generation prompt: <|turn>model\n only with no round; <|channel>thought\n<channel|> only with thinking off
//
// A tool is its JSON object ({"name", "description", "parameters"}, or the OpenAI form with them under "function"),
// written by the template's format_function_declaration; a call's arguments by format_argument (keys in Jinja's
// dictsort order, case-insensitive for ASCII; a string between <|"|> delimiters, null as None, a number as Python writes
// it). A round is llama.cpp's convert_tool_responses_gemma4 of run_with_mcp's messages: its calls, then one response per
// result - `response:NAME{...}` of the result's members when it is a JSON object, else `response:NAME{value:...}` of the
// result as JSON when it parses and as a string otherwise; the round's own content and reasoning are not written (the
// template has no place for them). The client's messages obey every rule of the text prompt; the call names, the
// arguments and the results are plain (the web's and the model's words: their literals stay text). nullopt where
// render_gemma4_text_prompt fails, for no tools or a tool that is not an object with a string name, and for a round with
// no call, a call without a name or a result count unlike its calls.
[[nodiscard]] std::optional<std::vector<PromptPiece>> render_gemma4_tool_prompt(const std::vector<ChatMessage>& messages,
                                                                               std::span<const std::string> tools,
                                                                               std::span<const ToolRound> rounds, bool thinking);

}  // namespace secure_frontend
