#pragma once

#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/prompt_piece.hpp"
#include "secure_frontend/tool_calls.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace secure_frontend {

// flint: the fixed, text-only prompt, byte for byte the text path of the GGUF's chat template (tokenizer.chat_template;
// llama.cpp master e86f5fd1's Jinja engine renders the same bytes, tests/fixtures/flint_tokenizer_texts.json "chats"):
//
//   <|system|>{flint_text(system)}       a system message, wherever the client put it
//   <|user|>{flint_text(user)}
//   <|assistant|>{assistant}             an earlier answer, as written (the schema carries no reasoning_content)
//   <|assistant|>                        the generation prompt
//   <think></think>                      only with thinking off (the template has no switch: the model opens <think>)
//
// No newline, no trim and no BOS anywhere (add_bos false). flint_text is the template's macro: \x00, <think> and
// </think> removed, then <|begin_of_text|>, <|end_of_text|>, <|system|>, <|developer|>, <|user|>, <|assistant|> and
// <|observation|> written as &lt;|...|&gt;. Messages are system, user or assistant in any order, the last one the
// user's. The parts form (this profile is text only), an empty text, invalid UTF-8, a C0 control byte other than \t,
// \n and \r, a system or user text whose escaped form still holds one of the renderer's five markers (<|system|>,
// <|user|>, <|assistant|>, <think>, </think>: the escapes can rebuild one, "<thi<think>nk>"), and an assistant text
// holding any of them (the template writes it as is) fail closed, so client text never becomes a marker. Every other
// special literal stays text for the encoder, which refuses it. This is not a Jinja implementation.
[[nodiscard]] std::optional<std::string> render_flint_text_prompt(const std::vector<ChatMessage>& messages, bool thinking);

// Web search for flint (owner 2026-10-28): render_flint_text_prompt's prompt with the template's tool preamble, byte for
// byte as the GGUF's template renders a run_with_mcp conversation (upstream's ChatTemplate, transformers' Jinja settings:
// trim_blocks, so the tools sit inside <tools></tools> with no newline; tests/fixtures/raon1_flint_tool_prompts.json).
// The preamble - <|system|>\n# Tools\n\nYou may call one or more functions to assist with the user query.\n\nYou are
// provided with function signatures within <tools></tools> XML tags:\n<tools>{tool}</tools>\n\nFor each function call,
// output the function name and arguments within the following XML format:\n<tool_call>{function-name}<arg_key>
// {arg-key-1}</arg_key><arg_value>{arg-value-1}</arg_value>...</tool_call>\n - goes before the first message that is no
// system message; each round is <|assistant|>[<think>{reasoning}</think>]{content}, its calls <tool_call>NAME<arg_key>K
// </arg_key><arg_value>V</arg_value>...</tool_call> (V a string as written, else Python's json.dumps), then per result
// <|observation|><tool_response>{flint_text(result)}</tool_response>; then the generation tail. Reasoning and content
// are stripped as Python strips them (run_with_mcp's append), the results kept as they are. nullopt where
// render_flint_text_prompt fails, and for no tools or a round with no call or a result count unlike its calls.
// An earlier answer that searched (ChatMessage::rounds, owner 2026-10-29: "keep earlier searches in the chat history as
// upstream's page does") is written as the template writes the messages upstream's page sends for it: per round
// its text before the calls and the calls, the results; then its own text, which it may lack (the text prompt
// refuses that; a chat without the tool gets answers_as_text). Its rounds obey the rules of this turn's.
[[nodiscard]] std::optional<std::vector<PromptPiece>> render_flint_tool_prompt(const std::vector<ChatMessage>& messages,
                                                                              std::span<const std::string> tools,
                                                                              std::span<const ToolRound> rounds, bool thinking);

}  // namespace secure_frontend
