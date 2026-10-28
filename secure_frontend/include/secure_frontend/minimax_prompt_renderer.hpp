#pragma once

// MiniMax-M3.1-Flash (owner 2026-10-28, under the MiniMax Community License): the text chat prompt as Maya v1.1.30's
// server renders it - serve/frontend.py ChatTemplate over Maya's tools/mm3/chat_template.jinja (MiniMax's own template
// plus thinking_mode "disabled"), its literals encoded as serve/server.py render_ids encodes them.

#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/prompt_piece.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// The template's five literals: the turn framing (]~!b[ ]~b] [e~[) and the reasoning pair.
inline constexpr std::array<std::string_view, 5> kMinimaxLiterals{"]~!b[", "]~b]", "[e~[", "<mm:think>", "</mm:think>"};

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

}  // namespace secure_frontend
