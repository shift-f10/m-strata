#pragma once

#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/prompt_piece.hpp"
#include "secure_frontend/tool_calls.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// Renders the intentionally constrained, text-only Qwen/ChatML generation
// prompt used by the native frontend:
//
//   <|im_start|>system\n{system}<|im_end|>\n
//   <|im_start|>user\n{user}<|im_end|>\n
//   <|im_start|>assistant\n{assistant}<|im_end|>\n
//   <|im_start|>assistant\n
// A conversation may omit system, otherwise it must begin with exactly one
// system message and then alternate user/assistant, ending in user. Only the
// system, user, and assistant roles are accepted. Content containing ChatML
// control markers or control bytes fails closed so user text cannot create a
// synthetic prompt boundary.
//
// This is deliberately not a Jinja implementation and does not claim parity
// with a model's tokenizer_config chat_template. Callers must choose this
// renderer only for models configured for this fixed prompt format.
//
// SP5 M1 Task 12 (Ruling T12-f): a message in the PARTS form (ChatMessage.parts)
// renders its text parts inline and its image parts as the Qwen template's
// per-image run — <|vision_start|> + N x <|image_pad|> + <|vision_end|> — where N
// is the image-token count the encoder answered (the pin expands the pad run one
// id per image token, serve/server.py:4488-4505; we compute it up front). The
// counts are consumed in part order across all messages; a request whose image
// parts do not match the counts fails closed.
//
// The generation tail follows the Qwen3 template's add_generation_prompt block
// (tokenizer/chat_template.jinja; upstream renders it at serve/server.py:4370-4386):
// absent writes nothing after "assistant\n" (a pack without reasoning markers), on
// writes "<think>\n" (thinking, the template default), off writes the closed block
// "<think>\n\n</think>\n\n" (enable_thinking=false). With on or off, content holding
// either literal fails closed, so only the renderer places them.
enum class QwenThinking { absent, on, off };

// Session log step 16: the template's reasoning instructions (chat_template.jinja,
// reasoning_effort; its default is xhigh, which "high" names). With thinking on, high and low
// put their fixed sentence at the top of the system turn (before any system text, a blank line
// between), medium none; with thinking off there is none. at_end (reasoning.effort_position
// "end", upstream #458, serve/server.py render_prompt): a low, medium or off request renders the
// conversation as high would, and low or medium add their sentence as a short system turn right
// before the answer. No placement (nullopt): the fixed format above, without a sentence.
enum class QwenEffort { low, medium, high };
struct QwenEffortPlacement final {
  QwenEffort effort;
  bool at_end{false};
};

[[nodiscard]] std::optional<std::string> render_qwen_text_prompt(
    const std::vector<ChatMessage>& messages, std::span<const std::size_t> image_token_counts = {},
    QwenThinking thinking = QwenThinking::absent, std::optional<QwenEffortPlacement> effort = std::nullopt);

// Web search (docs/superpowers/specs/2026-10-27-web-search-design.md §2): render_qwen_text_prompt's prompt with the
// template's tool sections, as upstream renders a Qwen3.6 pack's own chat_template.jinja for run_with_mcp (the effort
// sentence where render_qwen_text_prompt puts it, as serve/chat_template.jinja does):
//
//   <|im_start|>system\n[sentence\n\n]# Tools\n\nYou have access to the following functions:\n\n<tools>\n{tool}...
//   \n</tools>\n\nIf you choose to call a function ONLY reply in the following format ...</IMPORTANT>[\n\n{system}]
//   <|im_end|>\n
//
// then the conversation as render_qwen_text_prompt renders it, then each round: the assistant's turn with its think
// block (it follows the last user turn) - <think>\n{reasoning}\n</think>\n\n{content} - and its calls
// (<tool_call>\n<function=NAME>\n<parameter=K>\nV\n</parameter>\n...</function>\n</tool_call>, "\n\n" before the first
// after content, "\n" before each later one; V a string as written, any other value as Python's json.dumps writes it,
// as Qwen3.8's template does - the Qwen3.6 GGUF's writes True and None for a boolean and a null, which web_search never
// takes), then one user turn of "\n<tool_response>\n{result}\n</tool_response>" blocks;
// then the effort turn and the generation tail. Reasoning, content and results are stripped as Python strips them.
// `tools`: each tool's JSON as the template's tojson writes it (python_json_text). The pieces: the template's special
// tokens are markers, its text and the client's messages text, the model's words and the results plain. nullopt where
// render_qwen_text_prompt fails, and for an image part, a pack without reasoning markers (absent), no tools, or a round
// with no call or with a result count unlike its calls.
[[nodiscard]] std::optional<std::vector<PromptPiece>> render_qwen_tool_prompt(
    const std::vector<ChatMessage>& messages, std::span<const std::string> tools, std::span<const ToolRound> rounds,
    QwenThinking thinking, std::optional<QwenEffortPlacement> effort = std::nullopt);

// The template's tool text around the tools and after </tools> (chat_template.jinja: the same in Qwen3.6's, Qwen3.8's
// and raon-1-mini's); its <tool_call> and </tool_call> are the special tokens.
inline constexpr std::string_view kQwenToolsOpen = "# Tools\n\nYou have access to the following functions:\n\n<tools>";
inline constexpr std::string_view kQwenToolsClose = "\n</tools>";
inline constexpr std::string_view kQwenToolInstructions =
    "\n\nIf you choose to call a function ONLY reply in the following format with NO suffix:\n\n<tool_call>\n"
    "<function=example_function_name>\n<parameter=example_parameter_1>\nvalue_1\n</parameter>\n"
    "<parameter=example_parameter_2>\nThis is the value for the second parameter\nthat can span\nmultiple lines\n"
    "</parameter>\n</function>\n</tool_call>\n\n<IMPORTANT>\nReminder:\n- Function calls MUST follow the specified format: "
    "an inner <function=...></function> block must be nested within <tool_call></tool_call> XML tags\n- Required "
    "parameters MUST be specified\n- You may provide optional reasoning for your function call in natural language "
    "BEFORE the function call, but NOT after\n- If there is no function call available, answer the question like normal "
    "with your current knowledge and do not tell the user about function calls\n</IMPORTANT>";

}  // namespace secure_frontend
