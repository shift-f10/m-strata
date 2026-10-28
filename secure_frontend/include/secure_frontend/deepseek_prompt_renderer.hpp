#pragma once

#include "secure_frontend/chat_schema.hpp"

#include <optional>
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

}  // namespace secure_frontend
