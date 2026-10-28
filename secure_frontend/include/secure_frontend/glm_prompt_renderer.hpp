#pragma once

#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/prompt_piece.hpp"
#include "secure_frontend/tool_calls.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace secure_frontend {

// Renders the deliberately fixed, text-only GLM-5.3 prompt profile:
//
//   [gMASK]<sop><|system|>{system}<|user|>{user}<|assistant|>{assistant}<|user|>{user}<|assistant|><think>
//
// The system message is optional, but if supplied it must be first and occur
// once.  Remaining messages alternate user/assistant and must end in user.
// User text may not contain any GLM framing literal or control byte, so it
// cannot manufacture a new model turn.  This is a narrow local profile based
// on the documented LightTransport GLM fixture format, not a general GLM
// template interpreter and not compatibility with arbitrary tokenizer assets.
[[nodiscard]] std::optional<std::string> render_glm53_text_prompt(
    const std::vector<ChatMessage>& messages);

// Web search (docs/superpowers/specs/2026-10-27-web-search-design.md §2, phase 5): render_glm53_text_prompt's prompt with
// the template's tool sections, as Project Maya renders GLM-5.3-Flash's own chat_template.jinja for its run_with_mcp -
// but for one line that is the fixed profile's: no `<|system|>Reasoning Effort: ...` turn (render_glm53_text_prompt
// writes none, where the template writes "Max" for no level):
//
//   [gMASK]<sop><|system|>\n# Tools\n\nYou may call one or more functions to assist with the user query.\n\nYou are
//   provided with function signatures within <tools></tools> XML tags:\n<tools>\n{tool}\n</tools>\n\nFor each function
//   call, output the function name and arguments within the following XML format:\n<tool_call>{function-name}<arg_key>
//   {arg-key-1}</arg_key><arg_value>{arg-value-1}</arg_value><arg_key>{arg-key-2}</arg_key><arg_value>{arg-value-2}
//   </arg_value>...</tool_call>
//
// then the conversation as render_glm53_text_prompt renders it, then each round - <|assistant|><think>{reasoning}
// </think>{content}, its calls <tool_call>NAME<arg_key>K</arg_key><arg_value>V</arg_value>...</tool_call> (V a string as
// written, any other value as Python's json.dumps writes it), then <|observation|> and one <tool_response>{result}
// </tool_response> per call, the result as it is - then <|assistant|><think>. Reasoning and content are stripped as
// Python strips them. The pieces as render_qwen_tool_prompt's (the template's special tokens markers, its text and the
// client's text, the model's words and the results plain). nullopt where render_glm53_text_prompt fails, and for no
// tools or a round with no call or a result count unlike its calls.
[[nodiscard]] std::optional<std::vector<PromptPiece>> render_glm53_tool_prompt(const std::vector<ChatMessage>& messages,
                                                                              std::span<const std::string> tools,
                                                                              std::span<const ToolRound> rounds);

// Renders the fixed, text-only raon-1-mini ChatML prompt profile (KRAFTON's own
// chat_template.jinja):
//
//   <|im_start|>system\n{system}<|im_end|>\n<|im_start|>user\n{user}<|im_end|>\n
//   <|im_start|>assistant\n<think></think>\n\n{assistant}<|im_end|>\n ... <|im_start|>assistant\n<think>\n
//
// An earlier assistant turn carries the template's empty think block: the schema has no
// reasoning_content, which is how the template renders a turn without one.
//
// The same glm_pack engine serves either a GLM-template pack or a ChatML (raon1)
// pack; the pack's own control vocabulary (a unique <|im_start|> CONTROL token)
// selects this renderer over render_glm53_text_prompt. The system message is
// optional, must be first and occur once; the remaining messages alternate
// user/assistant and must end in user. Each message's content is trimmed like
// the jinja (content|trim) and may not contain a ChatML framing literal or a
// control byte, so it cannot manufacture a new model turn. This is a narrow
// local profile, not a general ChatML template interpreter. `thinking` selects the generation
// tail: an open think block (<think>\n, the reply begins as reasoning) when on, the empty block
// (<think></think>\n\n, a direct answer) when off.
[[nodiscard]] std::optional<std::string> render_raon1_text_prompt(
    const std::vector<ChatMessage>& messages, bool thinking);

// Web search for raon-1-mini (owner 2026-10-28): render_raon1_text_prompt's prompt with the tool sections, byte for byte
// as raon-1-mini's own chat_template.jinja renders a run_with_mcp conversation (upstream's ChatTemplate, transformers'
// Jinja settings; tests/fixtures/raon1_flint_tool_prompts.json):
//
//   <|im_start|>system\n# Tools\n\nYou have access to the following functions:\n\n<tools>\n{tool}\n</tools>
//   {Qwen's call instructions}[\n\n{system}]<|im_end|>\n
//
// then the conversation, then each round - <|im_start|>assistant\n<think>\n{reasoning}\n</think>\n\n (or <think></think>
// \n\n without reasoning){content}, its calls (\n\n before the first when there is content, \n between them)
// <tool_call>\n<function=NAME>\n<parameter=K>\nV\n</parameter>\n</function>\n</tool_call>, <|im_end|>\n, then one
// user turn of results <|im_start|>user\n<tool_response>\n{result}\n</tool_response>...<|im_end|>\n - then the
// generation tail of render_raon1_text_prompt. Every body is stripped as Jinja's trim strips (Python's whitespace);
// the pieces as render_qwen_tool_prompt's. nullopt where render_raon1_text_prompt fails, and for no tools or a round
// with no call or a result count unlike its calls.
[[nodiscard]] std::optional<std::vector<PromptPiece>> render_raon1_tool_prompt(const std::vector<ChatMessage>& messages,
                                                                              std::span<const std::string> tools,
                                                                              std::span<const ToolRound> rounds, bool thinking);

}  // namespace secure_frontend
