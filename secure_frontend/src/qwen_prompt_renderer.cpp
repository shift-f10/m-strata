#include "secure_frontend/qwen_prompt_renderer.hpp"

#include "secure_frontend/prompt_builder.hpp"
#include "secure_frontend/python_text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace secure_frontend {
namespace {

// The most the limits.chat.* rows may allow (spec §9.5; session log step 13, the owner's 1M
// contexts): 65,536 messages, a message of up to a whole 256 MiB body, and the rendered prompt
// that much plus room for every message's framing. A request is bounded by its config first.
constexpr std::size_t kMaximumMessages = 65536U;
constexpr std::size_t kMaximumMessageBytes = 256U * 1024U * 1024U;
constexpr std::size_t kMaximumPromptBytes = kMaximumMessageBytes + 16U * 1024U * 1024U;
constexpr std::size_t kMaximumImagePadRun = 65536U;  // sanity bound on an encoder's answer
constexpr std::string_view kStartMarker = "<|im_start|>";
constexpr std::string_view kEndMarker = "<|im_end|>";
constexpr std::string_view kVisionStartMarker = "<|vision_start|>";
constexpr std::string_view kImagePadMarker = "<|image_pad|>";
constexpr std::string_view kVisionEndMarker = "<|vision_end|>";
constexpr std::string_view kThinkOpenMarker = "<think>";
constexpr std::string_view kThinkCloseMarker = "</think>";
// Session log step 16: the reasoning instructions of the Qwen3.8 template
// (tokenizer/chat_template.jinja, reasoning_instructions), byte for byte, and the medium
// sentence upstream says only in its end turn (serve/server.py EFFORT_TEXT, #458).
constexpr std::string_view kEffortHigh =
    "Reasoning effort is set to xhigh. Please think carefully through the task, validate key assumptions, consider "
    "plausible alternatives, and prioritize correctness, consistency, and clarity in the final answer.";
constexpr std::string_view kEffortLow =
    "Reasoning effort is set to low. Keep your thinking brief and focused, moving directly to the conclusion without "
    "unnecessary elaboration.";
constexpr std::string_view kEffortMediumTurn =
    "Reasoning effort is set to medium. Think as much as the task needs, without unnecessary elaboration.";

// The sentence at the top of the system turn. At the end, the conversation is the default
// one (xhigh) whatever the request asked; at the start it is the request's own level.
std::string_view top_effort_sentence(const QwenThinking thinking, const std::optional<QwenEffortPlacement>& effort) {
  if (!effort.has_value() || thinking == QwenThinking::absent) return {};
  if (effort->at_end) return kEffortHigh;
  if (thinking == QwenThinking::off) return {};
  switch (effort->effort) {
    case QwenEffort::high: return kEffortHigh;
    case QwenEffort::low: return kEffortLow;
    case QwenEffort::medium: return {};
  }
  return {};
}

// The short system turn right before the answer (effort_position "end"): low and medium only.
std::string_view end_effort_sentence(const QwenThinking thinking, const std::optional<QwenEffortPlacement>& effort) {
  if (!effort.has_value() || !effort->at_end || thinking != QwenThinking::on) return {};
  switch (effort->effort) {
    case QwenEffort::low: return kEffortLow;
    case QwenEffort::medium: return kEffortMediumTurn;
    case QwenEffort::high: return {};
  }
  return {};
}

bool valid_utf8(std::string_view value) {
  for (std::size_t index = 0; index < value.size();) {
    const auto byte = static_cast<unsigned char>(value[index]);
    if (byte <= 0x7fU) {
      ++index;
    } else if (byte >= 0xc2U && byte <= 0xdfU && index + 1U < value.size() &&
               (static_cast<unsigned char>(value[index + 1U]) & 0xc0U) == 0x80U) {
      index += 2U;
    } else if (byte >= 0xe0U && byte <= 0xefU && index + 2U < value.size() &&
               (static_cast<unsigned char>(value[index + 1U]) & 0xc0U) == 0x80U &&
               (static_cast<unsigned char>(value[index + 2U]) & 0xc0U) == 0x80U &&
               !(byte == 0xe0U && static_cast<unsigned char>(value[index + 1U]) < 0xa0U) &&
               !(byte == 0xedU && static_cast<unsigned char>(value[index + 1U]) > 0x9fU)) {
      index += 3U;
    } else if (byte >= 0xf0U && byte <= 0xf4U && index + 3U < value.size() &&
               (static_cast<unsigned char>(value[index + 1U]) & 0xc0U) == 0x80U &&
               (static_cast<unsigned char>(value[index + 2U]) & 0xc0U) == 0x80U &&
               (static_cast<unsigned char>(value[index + 3U]) & 0xc0U) == 0x80U &&
               !(byte == 0xf0U && static_cast<unsigned char>(value[index + 1U]) < 0x90U) &&
               !(byte == 0xf4U && static_cast<unsigned char>(value[index + 1U]) > 0x8fU)) {
      index += 4U;
    } else {
      return false;
    }
  }
  return true;
}

bool safe_content(std::string_view content) {
  if (content.empty() || content.size() > kMaximumMessageBytes || !valid_utf8(content) ||
      content.find(kStartMarker) != std::string_view::npos ||
      content.find(kEndMarker) != std::string_view::npos ||
      content.find(kVisionStartMarker) != std::string_view::npos ||
      content.find(kImagePadMarker) != std::string_view::npos ||
      content.find(kVisionEndMarker) != std::string_view::npos) {
    return false;
  }
  for (const unsigned char byte : content) {
    if (byte < 0x20U && byte != '\n' && byte != '\r' && byte != '\t') return false;
  }
  return true;
}

bool append_bounded(std::string* prompt, std::string_view value) {
  if (value.size() > kMaximumPromptBytes - prompt->size()) return false;
  prompt->append(value);
  return true;
}

// Renders one message body: the string form appends its content; the parts form
// (Ruling T12-f) appends each text part and, for each image part, the Qwen
// template's run — vision_start + N x image_pad + vision_end — consuming the
// encoder's N in part order. Text parts obey the same safety rules as content.
bool append_message_body(std::string* prompt, const ChatMessage& message,
                         const std::span<const std::size_t> image_token_counts, std::size_t& image_index) {
  if (message.parts.empty()) return append_bounded(prompt, message.content);
  for (const auto& part : message.parts) {
    if (part.image_data_url.empty()) {
      if (!safe_content(part.text) || !append_bounded(prompt, part.text)) return false;
      continue;
    }
    if (image_index >= image_token_counts.size() || image_token_counts[image_index] == 0U ||
        image_token_counts[image_index] > kMaximumImagePadRun) {
      return false;  // the counts must cover every image part exactly (the route feeds both from one parse)
    }
    if (!append_bounded(prompt, kVisionStartMarker)) return false;
    for (std::size_t pad = 0; pad < image_token_counts[image_index]; ++pad) {
      if (!append_bounded(prompt, kImagePadMarker)) return false;
    }
    if (!append_bounded(prompt, kVisionEndMarker)) return false;
    ++image_index;
  }
  return true;
}

bool image_free_parts(const ChatMessage& message) {
  for (const auto& part : message.parts) {
    if (!part.image_data_url.empty()) return false;
  }
  return true;
}

// With a reasoning tail only the renderer may place the think markers: a client's
// literal could otherwise open or close the model's think block.
bool holds_think_marker(const ChatMessage& message) {
  const auto holds = [](const std::string_view text) {
    return text.find(kThinkOpenMarker) != std::string_view::npos || text.find(kThinkCloseMarker) != std::string_view::npos;
  };
  if (holds(message.content)) return true;
  for (const auto& part : message.parts) {
    if (holds(part.text)) return true;
  }
  return false;
}

// render_qwen_text_prompt. With `searched_history` (the tool prompt's check of its conversation), an earlier answer that
// searched (ChatMessage::rounds) may have no text after its last round: its rounds are its turn.
std::optional<std::string> text_prompt(const std::vector<ChatMessage>& messages, const std::span<const std::size_t> image_token_counts,
                                       const QwenThinking thinking, const std::optional<QwenEffortPlacement>& effort,
                                       const bool searched_history) {
  if (messages.empty() || messages.size() > kMaximumMessages) return std::nullopt;
  if (thinking != QwenThinking::absent) {
    for (const auto& message : messages) {
      if (holds_think_marker(message)) return std::nullopt;
    }
  }

  std::size_t index = 0;
  if (messages.front().role == "system") {
    if (!safe_content(messages.front().content) || !image_free_parts(messages.front())) return std::nullopt;
    index = 1U;
  }
  if (index == messages.size()) return std::nullopt;

  bool expect_user = true;
  std::string prompt;
  prompt.reserve(256U);
  std::size_t image_index = 0;
  for (; index < messages.size(); ++index) {
    const ChatMessage& message = messages[index];
    const std::string_view expected_role = expect_user ? "user" : "assistant";
    if (message.role != expected_role) return std::nullopt;
    if (message.parts.empty()) {
      const bool rounds_only = searched_history && message.content.empty() && !message.rounds.empty();
      if (!rounds_only && !safe_content(message.content)) return std::nullopt;
    } else if (message.role != "user" && !image_free_parts(message)) {
      return std::nullopt;  // images ride user turns (the pin reads the request's user images)
    }
    if (!append_bounded(&prompt, kStartMarker) || !append_bounded(&prompt, message.role) ||
        !append_bounded(&prompt, "\n") || !append_message_body(&prompt, message, image_token_counts, image_index) ||
        !append_bounded(&prompt, kEndMarker) || !append_bounded(&prompt, "\n")) {
      return std::nullopt;
    }
    expect_user = !expect_user;
  }
  // A generation prompt is valid only after a user turn. At that point the
  // next expected historical role would be assistant. And every count given
  // must have been consumed by an image part (exact match, both directions).
  if (expect_user || image_index != image_token_counts.size()) return std::nullopt;

  // The template's system turn: its reasoning sentence first, a blank line, then the
  // system text; the sentence alone when there is no system message.
  const bool has_system = messages.front().role == "system";
  const std::string_view sentence = top_effort_sentence(thinking, effort);
  if (has_system || !sentence.empty()) {
    std::string system_prefix;
    system_prefix.reserve((has_system ? messages.front().content.size() : 0U) + sentence.size() + 32U);
    system_prefix.append(kStartMarker);
    system_prefix.append("system\n");
    system_prefix.append(sentence);
    if (!sentence.empty() && has_system) system_prefix.append("\n\n");
    if (has_system) system_prefix.append(messages.front().content);
    system_prefix.append(kEndMarker);
    system_prefix.push_back('\n');
    if (system_prefix.size() > kMaximumPromptBytes - prompt.size()) return std::nullopt;
    prompt.insert(0U, system_prefix);
  }
  if (const std::string_view turn = end_effort_sentence(thinking, effort); !turn.empty()) {
    if (!append_bounded(&prompt, kStartMarker) || !append_bounded(&prompt, "system\n") || !append_bounded(&prompt, turn) ||
        !append_bounded(&prompt, kEndMarker) || !append_bounded(&prompt, "\n")) {
      return std::nullopt;
    }
  }
  if (!append_bounded(&prompt, kStartMarker) || !append_bounded(&prompt, "assistant\n")) {
    return std::nullopt;
  }
  // The Qwen3 template's add_generation_prompt tail (tokenizer/chat_template.jinja).
  if (thinking == QwenThinking::on && !append_bounded(&prompt, "<think>\n")) return std::nullopt;
  if (thinking == QwenThinking::off && !append_bounded(&prompt, "<think>\n\n</think>\n\n")) return std::nullopt;
  return prompt;
}

}  // namespace

std::optional<std::string> render_qwen_text_prompt(const std::vector<ChatMessage>& messages,
                                                   const std::span<const std::size_t> image_token_counts,
                                                   const QwenThinking thinking, const std::optional<QwenEffortPlacement> effort) {
  return text_prompt(messages, image_token_counts, thinking, effort, false);
}

namespace {

constexpr std::string_view kToolsOpen = kQwenToolsOpen;
constexpr std::string_view kToolsClose = kQwenToolsClose;
constexpr std::string_view kToolInstructions = kQwenToolInstructions;
constexpr std::string_view kCallOpen = "<tool_call>";
constexpr std::string_view kCallClose = "</tool_call>";
constexpr std::string_view kResponseOpen = "<tool_response>";
constexpr std::string_view kResponseClose = "</tool_response>";
// The template's own text: its <tool_call> and </tool_call> are the special tokens.
constexpr std::array<std::string_view, 2U> kInstructionTags{kCallOpen, kCallClose};

// A text-only message's body: its content, or its text parts in order.
std::string message_body(const ChatMessage& message) {
  if (message.parts.empty()) return message.content;
  std::string body;
  for (const auto& part : message.parts) body += part.text;
  return body;
}

// A round with a call, a name for each, and one result per call.
bool valid_round(const ToolRound& round) {
  if (round.calls.empty() || round.results.size() != round.calls.size()) return false;
  return std::none_of(round.calls.begin(), round.calls.end(), [](const ToolCall& call) { return call.name.empty(); });
}

// An earlier answer's words, call names and results come from the client (chat_schema reads them so): UTF-8 without
// a control byte. They are plain, so a special literal in them is text.
bool plain_client_text(const std::string_view text) {
  if (text.size() > kMaximumMessageBytes || !valid_utf8(text)) return false;
  return std::none_of(text.begin(), text.end(), [](const char c) {
    const auto byte = static_cast<unsigned char>(c);
    return byte < 0x20U && byte != '\n' && byte != '\r' && byte != '\t';
  });
}

bool valid_history_round(const ToolRound& round) {
  if (!valid_round(round) || !plain_client_text(round.content)) return false;
  for (const auto& call : round.calls) {
    if (!plain_client_text(call.name)) return false;
  }
  return std::all_of(round.results.begin(), round.results.end(), [](const std::string& result) { return plain_client_text(result); });
}

// A round's calls after the assistant's words (`content`, stripped): "\n\n" before the first after words, "\n" before
// each later one.
void append_calls(PromptBuilder& out, const std::string_view content, const std::vector<ToolCall>& calls) {
  for (std::size_t k = 0U; k < calls.size(); ++k) {
    const ToolCall& call = calls[k];
    out.text(k > 0U ? "\n" : content.empty() ? "" : "\n\n");
    out.marker(kCallOpen);
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
    out.marker(kCallClose);
  }
}

// A round's results: one user turn of "\n<tool_response>\n{result}\n</tool_response>" blocks, each result stripped.
void append_results(PromptBuilder& out, const std::vector<std::string>& results) {
  out.marker(kStartMarker);
  out.text("user");
  for (const auto& result : results) {
    out.text("\n");
    out.marker(kResponseOpen);
    out.text("\n");
    out.plain(python_strip(result));
    out.text("\n");
    out.marker(kResponseClose);
  }
  out.marker(kEndMarker);
  out.text("\n");
}

// An earlier answer that searched (ChatMessage::rounds) as upstream's page sends it (serve/web/app.js
// assistantMessages) and the template renders it before the last user turn, so without a think block: per round an
// assistant turn of its words (stripped) and calls, then the results' user turn; then its text after the last round,
// stripped, as an assistant turn of its own when it is not empty.
void append_searched_answer(PromptBuilder& out, const ChatMessage& message) {
  for (const auto& round : message.rounds) {
    out.marker(kStartMarker);
    out.text("assistant\n");
    const std::string_view content = python_strip(round.content);
    out.plain(content);
    append_calls(out, content, round.calls);
    out.marker(kEndMarker);
    out.text("\n");
    append_results(out, round.results);
  }
  if (const std::string_view rest = python_strip(message.content); !rest.empty()) {
    out.marker(kStartMarker);
    out.text("assistant\n");
    out.text(rest);
    out.marker(kEndMarker);
    out.text("\n");
  }
}

}  // namespace

std::optional<std::vector<PromptPiece>> render_qwen_tool_prompt(const std::vector<ChatMessage>& messages,
                                                                const std::span<const std::string> tools,
                                                                const std::span<const ToolRound> rounds, const QwenThinking thinking,
                                                                const std::optional<QwenEffortPlacement> effort) {
  if (thinking == QwenThinking::absent || tools.empty()) return std::nullopt;
  for (const auto& message : messages) {
    if (!image_free_parts(message)) return std::nullopt;  // a request with images is served without search
    if (message.rounds.empty()) continue;
    // Owner 2026-10-29 ("keep earlier searches in the chat history as upstream's page does"): an earlier answer's
    // rounds, on an assistant message in the string form.
    if (message.role != "assistant" || !message.parts.empty()) return std::nullopt;
    if (!std::all_of(message.rounds.begin(), message.rounds.end(), valid_history_round)) return std::nullopt;
  }
  // The conversation obeys every rule of the plain prompt (roles, order, markers, sizes); an earlier answer that
  // searched may have no text after its rounds.
  if (!text_prompt(messages, {}, thinking, effort, true)) return std::nullopt;
  if (!std::all_of(rounds.begin(), rounds.end(), valid_round)) return std::nullopt;

  PromptBuilder out;
  const bool has_system = messages.front().role == "system";
  // The system turn: the effort sentence, the tools, the instructions, then the system text.
  out.marker(kStartMarker);
  out.text("system\n");
  if (const std::string_view sentence = top_effort_sentence(thinking, effort); !sentence.empty()) {
    out.text(sentence);
    out.text("\n\n");
  }
  out.text(kToolsOpen);
  for (const auto& tool : tools) {
    out.text("\n");
    out.text(tool);
  }
  out.text(kToolsClose);
  out.templated(kToolInstructions, kInstructionTags);
  if (has_system) {
    out.text("\n\n");
    out.text(message_body(messages.front()));
  }
  out.marker(kEndMarker);
  out.text("\n");
  for (std::size_t index = has_system ? 1U : 0U; index < messages.size(); ++index) {
    if (!messages[index].rounds.empty()) {
      append_searched_answer(out, messages[index]);
      continue;
    }
    out.marker(kStartMarker);
    out.text(messages[index].role);
    out.text("\n");
    out.text(message_body(messages[index]));
    out.marker(kEndMarker);
    out.text("\n");
  }
  // The rounds follow the last user turn, so each assistant turn keeps its think block (loop.index0 > last_query_index).
  for (const auto& round : rounds) {
    out.marker(kStartMarker);
    out.text("assistant\n");
    out.marker(kThinkOpenMarker);
    out.text("\n");
    out.plain(python_strip(round.reasoning));
    out.text("\n");
    out.marker(kThinkCloseMarker);
    out.text("\n\n");
    const std::string_view content = python_strip(round.content);
    out.plain(content);
    append_calls(out, content, round.calls);
    out.marker(kEndMarker);
    out.text("\n");
    append_results(out, round.results);
  }
  if (const std::string_view turn = end_effort_sentence(thinking, effort); !turn.empty()) {
    out.marker(kStartMarker);
    out.text("system\n");
    out.text(turn);
    out.marker(kEndMarker);
    out.text("\n");
  }
  out.marker(kStartMarker);
  out.text("assistant\n");
  out.marker(kThinkOpenMarker);
  if (thinking == QwenThinking::on) {
    out.text("\n");
  } else {
    out.text("\n\n");
    out.marker(kThinkCloseMarker);
    out.text("\n\n");
  }
  if (out.bytes() > kMaximumPromptBytes) return std::nullopt;
  return out.take();
}

}  // namespace secure_frontend
