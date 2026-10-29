#pragma once

#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/prompt_piece.hpp"
#include "secure_frontend/tool_calls.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace secure_frontend {

// Renders the fixed, text-only DeepSeek-V4.1 prompt as DeepSeek's own encoder writes it (Maya v1.1.30
// serve/ds41_encoding.py, vendored from DeepSeek under MIT: encode_messages with drop_thinking, Maya's default):
//
//   <｜begin▁of▁sentence｜><｜System｜>Reasoning Effort: {effort} (range 1-100, the higher the value, the more thorough
//   the reasoning)\n\n{system}<｜User｜>{user}<｜Assistant｜></think>{assistant}<｜end▁of▁sentence｜> ...
//   <｜User｜>{user}<｜Assistant｜><think>
//
// `effort` (1-100) is thinking mode: the effort line opens the system block and the last user turn's assistant header
// opens <think>, so the reply begins as reasoning. nullopt is chat mode: no effort line, <｜System｜> only before a system
// message, and every header closes the block (</think>). An earlier assistant turn is its content only (its reasoning
// is dropped, as Maya's server drops it). The system message is optional, must be first and occur once; the remaining
// messages alternate user/assistant and must end in user. Content may not be empty or hold a DeepSeek framing literal,
// ｜DSML｜ or a control byte, so it cannot manufacture a turn. Not a general DeepSeek encoder: no tools, tasks, images
// or mid-conversation system messages.
[[nodiscard]] std::optional<std::string> render_deepseek_text_prompt(const std::vector<ChatMessage>& messages,
                                                                     std::optional<int> effort);

// Web search for DeepSeek-V4.1 (owner 2026-10-28: "deepseek does not seem to have mcp search access"): the prompt
// DeepSeek's encoder writes for a chat with the server's tools and the search loop's rounds, as Maya v1.1.30's
// ChatTemplate hands them to it (the tools on the system message - an empty one first when the chat has none - and
// each round appended as Maya's run_with_mcp appends DeepSeek's: content and reasoning as the parser gave them, the
// calls, then one tool result each, merged into a user turn):
//
//   <BOS><｜System｜>{effort line}{system}\n\n## Tools ... {tool schemas} ...<｜User｜>{user}<｜Assistant｜><think>
//   {reasoning}</think>{content}\n\n<｜DSML｜ calls>\n<｜DSML｜ invoke name="N">\n<｜DSML｜ parameter name="K"
//   string="true|false">V</｜DSML｜ parameter>\n</｜DSML｜ invoke>\n</｜DSML｜ calls><EOS><｜User｜><tool_result>{result}
//   </tool_result><｜Assistant｜><think>
//
// With tools DeepSeek's encoder keeps every turn's thinking (drop_thinking off): each assistant header opens <think> in
// thinking mode, an earlier assistant turn is "</think>{content}<EOS>". The markers, ｜DSML｜, <think> and </think> the
// encoder writes are markers; the model's words, the results and the calls' names and values are plain (Maya's literal
// marking); the client's messages follow render_deepseek_text_prompt's rules. Each tool is its function object or the
// OpenAI form carrying it; it is written as DeepSeek's to_json writes it. nullopt: no tools, a tool that is not an
// object with a name, a round without calls or with a result missing, an unnamed call, or what the text prompt refuses.
// An earlier answer that searched (ChatMessage::rounds, owner 2026-10-29: "keep earlier searches in the chat history as
// upstream's page does") is written as the template writes the messages upstream's page sends for it: per round
// its text before the calls and the calls, the results; then its own text, which it may lack (the text prompt
// refuses that; a chat without the tool gets answers_as_text). Its rounds obey the rules of this turn's.
[[nodiscard]] std::optional<std::vector<PromptPiece>> render_deepseek_tool_prompt(const std::vector<ChatMessage>& messages,
                                                                                  std::span<const std::string> tools,
                                                                                  std::span<const ToolRound> rounds,
                                                                                  std::optional<int> effort);

}  // namespace secure_frontend
