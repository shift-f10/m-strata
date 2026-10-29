#include "secure_frontend/glm_prompt_renderer.hpp"

#include "secure_frontend/prompt_builder.hpp"
#include "secure_frontend/python_text.hpp"
#include "secure_frontend/qwen_prompt_renderer.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace secure_frontend {
namespace {

// The most the limits.chat.* rows may allow (spec §9.5; session log step 13, the owner's 1M
// contexts): 65,536 messages, a message of up to a whole 256 MiB body, and the rendered prompt
// that much plus room for every message's framing. A request is bounded by its config first.
constexpr std::size_t kMaximumMessages = 65536U;
constexpr std::size_t kMaximumMessageBytes = 256U * 1024U * 1024U;
constexpr std::size_t kMaximumPromptBytes = kMaximumMessageBytes + 16U * 1024U * 1024U;
constexpr std::array<std::string_view, 6U> kControlLiterals = {
    "[gMASK]", "<sop>", "<|system|>", "<|user|>", "<|assistant|>", "<think>"};

bool valid_utf8(const std::string_view value) {
  for (std::size_t index = 0U; index < value.size();) {
    const auto byte = static_cast<unsigned char>(value[index]);
    if (byte <= 0x7fU) ++index;
    else if (byte >= 0xc2U && byte <= 0xdfU && index + 1U < value.size() &&
             (static_cast<unsigned char>(value[index + 1U]) & 0xc0U) == 0x80U) index += 2U;
    else if (byte >= 0xe0U && byte <= 0xefU && index + 2U < value.size() &&
             (static_cast<unsigned char>(value[index + 1U]) & 0xc0U) == 0x80U &&
             (static_cast<unsigned char>(value[index + 2U]) & 0xc0U) == 0x80U &&
             !(byte == 0xe0U && static_cast<unsigned char>(value[index + 1U]) < 0xa0U) &&
             !(byte == 0xedU && static_cast<unsigned char>(value[index + 1U]) > 0x9fU)) index += 3U;
    else if (byte >= 0xf0U && byte <= 0xf4U && index + 3U < value.size() &&
             (static_cast<unsigned char>(value[index + 1U]) & 0xc0U) == 0x80U &&
             (static_cast<unsigned char>(value[index + 2U]) & 0xc0U) == 0x80U &&
             (static_cast<unsigned char>(value[index + 3U]) & 0xc0U) == 0x80U &&
             !(byte == 0xf0U && static_cast<unsigned char>(value[index + 1U]) < 0x90U) &&
             !(byte == 0xf4U && static_cast<unsigned char>(value[index + 1U]) > 0x8fU)) index += 4U;
    else return false;
  }
  return true;
}

bool safe_content(const std::string_view content) {
  if (content.empty() || content.size() > kMaximumMessageBytes || !valid_utf8(content)) return false;
  for (const auto marker : kControlLiterals) {
    if (content.find(marker) != std::string_view::npos) return false;
  }
  for (const unsigned char byte : content) {
    if (byte < 0x20U && byte != '\n' && byte != '\r' && byte != '\t') return false;
  }
  return true;
}

bool append_bounded(std::string* const output, const std::string_view text) {
  if (text.size() > kMaximumPromptBytes - output->size()) return false;
  output->append(text);
  return true;
}

// raon-1-mini ChatML framing literals: user content carrying any of them is
// rejected, so the encoder's scan-anywhere for these markers can never be
// steered by message text.
constexpr std::array<std::string_view, 5U> kRaonControlLiterals = {
    "<|im_start|>", "<|im_end|>", "<think>", "</think>", "<|endoftext|>"};

bool raon_safe_content(const std::string_view content) {
  if (content.empty() || content.size() > kMaximumMessageBytes || !valid_utf8(content)) return false;
  for (const auto marker : kRaonControlLiterals) {
    if (content.find(marker) != std::string_view::npos) return false;
  }
  for (const unsigned char byte : content) {
    if (byte < 0x20U && byte != '\n' && byte != '\r' && byte != '\t') return false;
  }
  return true;
}

// The jinja renders each message as content|trim, which strips Python's whitespace (Unicode spaces too) at both ends.
std::string_view raon_trim(const std::string_view value) { return python_strip(value); }

// Rounds the tool prompts can say: each with a call at least, every call named, one result per call.
bool sayable(const std::span<const ToolRound> rounds) {
  for (const auto& round : rounds) {
    if (round.calls.empty() || round.results.size() != round.calls.size()) return false;
    for (const auto& call : round.calls) {
      if (call.name.empty()) return false;
    }
  }
  return true;
}

// An earlier answer's searches (owner 2026-10-29, "keep earlier searches in the chat history as upstream's page
// does"), as the tool prompts take them: on an answer only, sayable as this turn's rounds are. Such an answer needs no
// text of its own (all of it came before its searches); the text it has obeys the plain prompt's rules.
bool history_ok(const ChatMessage& message) {
  return message.rounds.empty() || (message.role == "assistant" && sayable(message.rounds));
}

bool searches_only(const bool history_rounds, const ChatMessage& message) {
  return history_rounds && !message.rounds.empty() && message.content.empty();
}

// render_glm53_text_prompt; `history_rounds` is the tool prompt's check of the conversation, in which an earlier
// answer may carry its searches (history_ok).
std::optional<std::string> glm53_text_prompt(const std::vector<ChatMessage>& messages, const bool history_rounds) {
  if (messages.empty() || messages.size() > kMaximumMessages) return std::nullopt;
  std::string output;
  output.reserve(256U);
  if (!append_bounded(&output, "[gMASK]<sop>")) return std::nullopt;

  std::size_t index = 0U;
  if (messages.front().role == "system") {
    if ((history_rounds && !history_ok(messages.front())) || !safe_content(messages.front().content) ||
        !append_bounded(&output, "<|system|>") || !append_bounded(&output, messages.front().content)) return std::nullopt;
    index = 1U;
  }
  if (index == messages.size()) return std::nullopt;

  bool expect_user = true;
  for (; index < messages.size(); ++index) {
    const ChatMessage& message = messages[index];
    if (message.role != (expect_user ? "user" : "assistant") || (history_rounds && !history_ok(message)) ||
        (!searches_only(history_rounds, message) && !safe_content(message.content)) ||
        !append_bounded(&output, expect_user ? "<|user|>" : "<|assistant|>") ||
        !append_bounded(&output, message.content)) return std::nullopt;
    expect_user = !expect_user;
  }
  if (expect_user || !append_bounded(&output, "<|assistant|><think>")) return std::nullopt;
  return output;
}

// render_raon1_text_prompt; `history_rounds` as glm53_text_prompt's.
std::optional<std::string> raon1_text_prompt(const std::vector<ChatMessage>& messages, const bool thinking, const bool history_rounds) {
  if (messages.empty() || messages.size() > kMaximumMessages) return std::nullopt;
  std::string output;
  output.reserve(256U);

  std::size_t index = 0U;
  if (messages.front().role == "system") {
    if ((history_rounds && !history_ok(messages.front())) || !raon_safe_content(messages.front().content) ||
        !append_bounded(&output, "<|im_start|>system\n") || !append_bounded(&output, raon_trim(messages.front().content)) ||
        !append_bounded(&output, "<|im_end|>\n")) {
      return std::nullopt;
    }
    index = 1U;
  }
  if (index == messages.size()) return std::nullopt;

  bool expect_user = true;
  for (; index < messages.size(); ++index) {
    const ChatMessage& message = messages[index];
    const std::string_view role = expect_user ? "user" : "assistant";
    // An earlier assistant turn is the template's render_assistant_body: the schema carries no
    // reasoning_content, so its think block is the empty one before the content.
    if (message.role != role || (history_rounds && !history_ok(message)) ||
        (!searches_only(history_rounds, message) && !raon_safe_content(message.content)) ||
        !append_bounded(&output, "<|im_start|>") || !append_bounded(&output, role) || !append_bounded(&output, "\n") ||
        (!expect_user && !append_bounded(&output, "<think></think>\n\n")) ||
        !append_bounded(&output, raon_trim(message.content)) || !append_bounded(&output, "<|im_end|>\n")) {
      return std::nullopt;
    }
    expect_user = !expect_user;
  }
  // A generation prompt is valid only after a user turn. raon-1-mini's add_generation_prompt tail:
  // an open think block when thinking (the reply begins as reasoning), the empty block otherwise
  // (enable_thinking=false, a direct answer).
  if (expect_user || !append_bounded(&output, "<|im_start|>assistant\n") ||
      !append_bounded(&output, thinking ? "<think>\n" : "<think></think>\n\n")) {
    return std::nullopt;
  }
  return output;
}

}  // namespace

std::optional<std::string> render_glm53_text_prompt(const std::vector<ChatMessage>& messages) {
  return glm53_text_prompt(messages, false);
}

std::optional<std::vector<PromptPiece>> render_raon1_tool_prompt(const std::vector<ChatMessage>& messages,
                                                                 const std::span<const std::string> tools,
                                                                 const std::span<const ToolRound> rounds, const bool thinking) {
  if (tools.empty() || !sayable(rounds)) return std::nullopt;
  // The conversation obeys every rule of the plain prompt (roles, order, markers, sizes); an earlier answer's
  // searches those of this turn's rounds (history_ok).
  if (!raon1_text_prompt(messages, thinking, true)) return std::nullopt;
  static constexpr std::array<std::string_view, 2U> kInstructionTags{"<tool_call>", "</tool_call>"};
  // render_assistant_body's think block: the reasoning, else the empty block
  const auto think_block = [](PromptBuilder& out, const std::string_view reasoning) {
    out.marker("<think>");
    if (!reasoning.empty()) {
      out.text("\n");
      out.plain(reasoning);
      out.text("\n");
    }
    out.marker("</think>");
    out.text("\n\n");
  };
  PromptBuilder out;
  out.marker("<|im_start|>");
  out.text("system\n");
  out.text(kQwenToolsOpen);
  for (const auto& tool : tools) {
    out.text("\n");
    out.text(tool);
  }
  out.text(kQwenToolsClose);
  out.templated(kQwenToolInstructions, kInstructionTags);
  std::size_t index = 0U;
  if (messages.front().role == "system") {
    if (const std::string_view system = raon_trim(messages.front().content); !system.empty()) {
      out.text("\n\n");
      out.text(system);
    }
    index = 1U;
  }
  out.marker("<|im_end|>");
  out.text("\n");
  // A round: the assistant turn of its text and calls, then the user turn of its results. This turn's rounds as
  // run_with_mcp appends them; an earlier answer's as upstream's page sends them (serve/web/app.js assistantMessages:
  // per round the text before the calls and the calls, one tool message per result), whose reasoning is empty - the
  // template's empty think block - and whose text and results the template trims as it trims these.
  const auto write_round = [&out, &think_block](const ToolRound& round) {
    out.marker("<|im_start|>");
    out.text("assistant\n");
    think_block(out, python_strip(round.reasoning));
    const std::string_view content = python_strip(round.content);
    out.plain(content);
    for (std::size_t k = 0U; k < round.calls.size(); ++k) {
      const ToolCall& call = round.calls[k];
      out.text(k > 0U ? "\n" : content.empty() ? "" : "\n\n");
      out.marker("<tool_call>");
      out.text("\n<function=");
      out.plain(call.name);
      out.text(">\n");
      for (const auto& argument : call.arguments) {
        out.text("<parameter=");
        out.plain(argument.name);
        out.text(">\n");
        out.plain(template_argument_text(argument.value));
        out.text("\n</parameter>\n");
      }
      out.text("</function>\n");
      out.marker("</tool_call>");
    }
    out.marker("<|im_end|>");
    out.text("\n");
    // the tool messages: one user turn of results, each stripped (content|trim)
    out.marker("<|im_start|>");
    out.text("user");
    for (const auto& result : round.results) {
      out.text("\n");
      out.marker("<tool_response>");
      out.text("\n");
      out.plain(python_strip(result));
      out.text("\n");
      out.marker("</tool_response>");
    }
    out.marker("<|im_end|>");
    out.text("\n");
  };
  for (bool user = true; index < messages.size(); ++index, user = !user) {
    const ChatMessage& message = messages[index];
    for (const auto& round : message.rounds) write_round(round);
    if (searches_only(true, message)) continue;  // the page sends no turn for an answer's empty rest
    out.marker("<|im_start|>");
    out.text(user ? "user\n" : "assistant\n");
    if (!user) think_block(out, {});  // an earlier answer: the schema carries no reasoning_content
    out.text(raon_trim(message.content));
    out.marker("<|im_end|>");
    out.text("\n");
  }
  for (const auto& round : rounds) write_round(round);
  out.marker("<|im_start|>");
  out.text("assistant\n");
  out.marker("<think>");
  if (thinking) {
    out.text("\n");
  } else {
    out.marker("</think>");
    out.text("\n\n");
  }
  if (out.bytes() > kMaximumPromptBytes) return std::nullopt;
  return out.take();
}

std::optional<std::string> render_raon1_text_prompt(const std::vector<ChatMessage>& messages, const bool thinking) {
  return raon1_text_prompt(messages, thinking, false);
}

namespace {

// GLM-5.3-Flash's chat_template.jinja: its tools system turn after <|system|>, and its call and result tags.
constexpr std::string_view kGlmToolsHead =
    "\n# Tools\n\nYou may call one or more functions to assist with the user query.\n\nYou are provided with function "
    "signatures within <tools></tools> XML tags:\n<tools>";
constexpr std::string_view kGlmToolsTail =
    "\n</tools>\n\nFor each function call, output the function name and arguments within the following XML format:\n"
    "<tool_call>{function-name}<arg_key>{arg-key-1}</arg_key><arg_value>{arg-value-1}</arg_value><arg_key>{arg-key-2}"
    "</arg_key><arg_value>{arg-value-2}</arg_value>...</tool_call>";
constexpr std::array<std::string_view, 8U> kGlmToolTags{"<tool_call>", "</tool_call>", "<arg_key>",       "</arg_key>",
                                                        "<arg_value>", "</arg_value>", "<tool_response>", "</tool_response>"};

}  // namespace

std::optional<std::vector<PromptPiece>> render_glm53_tool_prompt(const std::vector<ChatMessage>& messages,
                                                                 const std::span<const std::string> tools,
                                                                 const std::span<const ToolRound> rounds) {
  if (tools.empty() || !sayable(rounds)) return std::nullopt;
  // The conversation obeys every rule of the fixed profile (roles, order, markers, sizes); an earlier answer's
  // searches those of this turn's rounds (history_ok).
  if (!glm53_text_prompt(messages, true)) return std::nullopt;
  PromptBuilder out;
  out.marker("[gMASK]");
  out.marker("<sop>");
  out.marker("<|system|>");
  out.text(kGlmToolsHead);
  for (const auto& tool : tools) {
    out.text("\n");
    out.text(tool);
  }
  out.templated(kGlmToolsTail, kGlmToolTags);
  std::size_t index = 0U;
  if (messages.front().role == "system") {
    out.marker("<|system|>");
    out.text(messages.front().content);
    index = 1U;
  }
  // A round: its assistant turn - reasoning, text and calls - then one <|observation|> of its results. This turn's
  // rounds as run_with_mcp appends them; an earlier answer's as upstream's page sends them (serve/web/app.js
  // assistantMessages: per round the text before the calls and the calls, one tool message per result), whose
  // reasoning is empty: Maya's template writes <think></think> for an assistant turn without reasoning_content, strips
  // its text, and writes the results as they are, as for these.
  const auto write_round = [&out](const ToolRound& round) {
    out.marker("<|assistant|>");
    out.marker("<think>");
    out.plain(python_strip(round.reasoning));
    out.marker("</think>");
    out.plain(python_strip(round.content));
    for (const auto& call : round.calls) {
      out.marker("<tool_call>");
      out.plain(call.name);
      for (const auto& argument : call.arguments) {
        out.marker("<arg_key>");
        out.plain(argument.name);
        out.marker("</arg_key>");
        out.marker("<arg_value>");
        out.plain(template_argument_text(argument.value));
        out.marker("</arg_value>");
      }
      out.marker("</tool_call>");
    }
    out.marker("<|observation|>");
    for (const auto& result : round.results) {
      out.marker("<tool_response>");
      out.plain(result);
      out.marker("</tool_response>");
    }
  };
  for (bool user = true; index < messages.size(); ++index, user = !user) {
    const ChatMessage& message = messages[index];
    for (const auto& round : message.rounds) write_round(round);
    // The rest of an earlier answer's text is the fixed profile's earlier answer, <|assistant|>{text} - where Maya's
    // template writes <|assistant|><think></think>{text} - and the page sends no turn for an empty rest.
    if (searches_only(true, message)) continue;
    out.marker(user ? "<|user|>" : "<|assistant|>");
    out.text(message.content);
  }
  for (const auto& round : rounds) write_round(round);
  out.marker("<|assistant|>");
  out.marker("<think>");
  if (out.bytes() > kMaximumPromptBytes) return std::nullopt;
  return out.take();
}

}  // namespace secure_frontend
