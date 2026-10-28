#pragma once

// MiniMax-M3.1-Flash (owner 2026-10-28, under the MiniMax Community License): the text chat prompt as Maya v1.1.30's
// server renders it - serve/frontend.py ChatTemplate over Maya's tools/mm3/chat_template.jinja (MiniMax's own template
// plus thinking_mode "disabled"), its literals encoded as serve/server.py render_ids encodes them.

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

// The template's five literals: the turn framing (]~!b[ ]~b] [e~[) and the reasoning pair.
inline constexpr std::array<std::string_view, 5> kMinimaxLiterals{"]~!b[", "]~b]", "[e~[", "<mm:think>", "</mm:think>"};
// The template's tool namespace (ns_token): every tag of a call, and of the tool section's example, follows it.
inline constexpr std::string_view kMinimaxToolNamespace = "]<]minimax[>[";
// The call block's tags the template writes after the namespace (toolcall_begin_token, toolcall_end_token), which
// MiniMax-M3.1's vocabulary holds as special tokens of their own.
inline constexpr std::array<std::string_view, 2> kMinimaxCallTags{"<tool_call>", "</tool_call>"};

// One chat's thinking in Maya's words (frontend.py ChatTemplate.render for MiniMax): `off` is thinking_mode "disabled"
// (the system block says so and the generation prompt closes the think block with </mm:think>); otherwise MiniMax's
// adaptive thinking, with `effort` (low, medium, high, xhigh or max) written as <effort>, or none (MiniMax's default).
struct MinimaxThinking final {
  bool off{false};
  std::optional<std::string> effort{};
};

// The prompt in pieces:
//
//   ]~!b[]~b]system\n{MiniMax's default system text}\n\n<thinking_instructions>\n...{mode line}</thinking_instructions>
//   \n\n<model_id>8bb8149fe</model_id>[\n\n<effort>{effort}</effort>][e~[\n
//   ]~b]developer\n{the first message if a system one with text, else "You are a helpful assistant."}[e~[\n
//   ]~b]user\n{text}[e~[\n   ]~b]ai\n</mm:think>{answer}[e~[\n   ]~b]developer\n{a later system message}[e~[\n ...
//   ]~b]ai\n[</mm:think> when off]
//
// The template's own literals are markers when `special` lists them (the literals the pack holds as special tokens) and
// text otherwise, as Maya's tokenizer encodes them with parse_special; message text is plain (a literal in it is text,
// Maya's mark_literals). An earlier answer is its content alone (the page sends no reasoning). nullopt: no messages,
// a role other than system, user or assistant, a last message that is not the user's, an effort that is not one of
// MiniMax's five, or an earlier answer holding </mm:think> where it is no special token (the template would split it).
[[nodiscard]] std::optional<std::vector<PromptPiece>> render_minimax_text_prompt(const std::vector<ChatMessage>& messages,
                                                                                const MinimaxThinking& thinking,
                                                                                std::span<const std::string_view> special);

// The prompt with tools (web search, owner 2026-10-28), as Maya renders it for MiniMax: the text prompt above, with the
// template's tool section in the first developer turn (each tool's function object as tojson writes it, in <tool>,
// then how to call them in the tool namespace ]<]minimax[>[) and the search rounds before the generation prompt:
//
//   ...]~b]developer\n{system}\n\n# Tools\n...<tools>\n<tool>{function}</tool>\n</tools>\n\nTo call tools, ...[e~[\n
//   ...]~b]ai\n<mm:think>{reasoning}</mm:think>{content}]<]minimax[>[<tool_call>\n]<]minimax[>[<invoke name="N">
//   ]<]minimax[>[<K>V]<]minimax[>[</K>]<]minimax[>[</invoke>\n]<]minimax[>[</tool_call>[e~[\n
//   ]~b]tool\n<response>{result}</response>[e~[\n   ]~b]ai\n
//
// Each round as Maya's run_with_mcp appends it: its reasoning and content stripped, the reasoning only when there was
// some (else the </mm:think> prefix, the template taking the reasoning out of a content that holds </mm:think> as text).
// A value goes out as the template's to_xml writes it: an object's members and an array's items as nested elements
// (`item`), a null member or argument left out, true and false as JSON, a string as it is, a number as Python's str. The
// tools are JSON texts, each a function ({"name", "description", "parameters"}) or OpenAI's form carrying one. The
// model's words, the calls and the results are plain. nullopt: what the text prompt refuses, no tools, a tool that is
// not an object with a name, a round without calls or with a result missing, or an unnamed call.
[[nodiscard]] std::optional<std::vector<PromptPiece>> render_minimax_tool_prompt(const std::vector<ChatMessage>& messages,
                                                                                std::span<const std::string> tools,
                                                                                std::span<const ToolRound> rounds,
                                                                                const MinimaxThinking& thinking,
                                                                                std::span<const std::string_view> special);

}  // namespace secure_frontend
