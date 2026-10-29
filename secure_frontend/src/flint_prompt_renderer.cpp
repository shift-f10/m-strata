#include "secure_frontend/flint_prompt_renderer.hpp"

#include "secure_frontend/json.hpp"
#include "secure_frontend/prompt_builder.hpp"
#include "secure_frontend/python_text.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace secure_frontend {
namespace {

// The bounds of the Qwen, GLM and Gemma renderers (spec §9.5; session log step 13): 65,536 messages, a message of up
// to a whole 256 MiB body, and the rendered prompt that much plus room for every message's framing.
constexpr std::size_t kMaximumMessages = 65536U;
constexpr std::size_t kMaximumMessageBytes = 256U * 1024U * 1024U;
constexpr std::size_t kMaximumPromptBytes = kMaximumMessageBytes + 16U * 1024U * 1024U;

// The five literals the renderer writes and encode_flint_prompt takes as markers.
constexpr std::array<std::string_view, 5U> kMarkers{"<|system|>", "<|user|>", "<|assistant|>", "<think>", "</think>"};

// flint_text's replacements, in the template's order.
constexpr std::array<std::pair<std::string_view, std::string_view>, 10U> kFlintText{{
    {std::string_view{"\0", 1U}, ""},
    {"<think>", ""},
    {"</think>", ""},
    {"<|begin_of_text|>", "&lt;|begin_of_text|&gt;"},
    {"<|end_of_text|>", "&lt;|end_of_text|&gt;"},
    {"<|system|>", "&lt;|system|&gt;"},
    {"<|developer|>", "&lt;|developer|&gt;"},
    {"<|user|>", "&lt;|user|&gt;"},
    {"<|assistant|>", "&lt;|assistant|&gt;"},
    {"<|observation|>", "&lt;|observation|&gt;"},
}};

// Jinja's replace: every non-overlapping occurrence, left to right.
std::string replace_all(const std::string_view text, const std::string_view from, const std::string_view to) {
  std::string out;
  out.reserve(text.size());
  std::size_t at = 0U;
  for (std::size_t found = text.find(from); found != std::string_view::npos; found = text.find(from, at)) {
    out.append(text.substr(at, found - at));
    out.append(to);
    at = found + from.size();
  }
  out.append(text.substr(at));
  return out;
}

std::string flint_text(const std::string_view text) {
  std::string out{text};
  for (const auto& [from, to] : kFlintText) out = replace_all(out, from, to);
  return out;
}

bool holds_marker(const std::string_view text) {
  for (const auto marker : kMarkers) {
    if (text.find(marker) != std::string_view::npos) return true;
  }
  return false;
}

// Bounded, valid UTF-8, and no C0 control byte but \t, \n and \r (the \x00 flint_text removes included).
bool acceptable(const std::string_view content) {
  if (content.empty() || content.size() > kMaximumMessageBytes || !is_valid_utf8(content)) return false;
  for (const unsigned char byte : content) {
    if (byte < 0x20U && byte != '\n' && byte != '\r' && byte != '\t') return false;
  }
  return true;
}

bool append_bounded(std::string& output, const std::string_view text) {
  if (text.size() > kMaximumPromptBytes - output.size()) return false;
  output.append(text);
  return true;
}

// Rounds the tool prompt can say: each with a call at least, every call named, one result per call.
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
// does"), as the tool prompt takes them: on an answer only, sayable as this turn's rounds are. Such an answer needs no
// text of its own (all of it came before its searches); the text it has obeys the plain prompt's rules.
bool history_ok(const ChatMessage& message) {
  return message.rounds.empty() || (message.role == "assistant" && sayable(message.rounds));
}

bool searches_only(const bool history_rounds, const ChatMessage& message) {
  return history_rounds && !message.rounds.empty() && message.content.empty();
}

// render_flint_text_prompt; `history_rounds` is the tool prompt's check of the conversation, in which an earlier
// answer may carry its searches (history_ok).
std::optional<std::string> flint_text_prompt(const std::vector<ChatMessage>& messages, const bool thinking, const bool history_rounds) {
  if (messages.empty() || messages.size() > kMaximumMessages || messages.back().role != "user") return std::nullopt;
  std::string output;
  output.reserve(256U);
  for (const ChatMessage& message : messages) {
    if ((history_rounds && !history_ok(message)) || !message.parts.empty()) return std::nullopt;
    if (searches_only(history_rounds, message)) continue;
    if (!acceptable(message.content)) return std::nullopt;
    if (message.role == "system" || message.role == "user") {
      const std::string text = flint_text(message.content);
      if (holds_marker(text) || !append_bounded(output, message.role == "system" ? "<|system|>" : "<|user|>") ||
          !append_bounded(output, text)) {
        return std::nullopt;
      }
    } else if (message.role == "assistant") {
      if (holds_marker(message.content) || !append_bounded(output, "<|assistant|>") ||
          !append_bounded(output, message.content)) {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
  }
  if (!append_bounded(output, "<|assistant|>") || (!thinking && !append_bounded(output, "<think></think>"))) return std::nullopt;
  return output;
}

}  // namespace

std::optional<std::string> render_flint_text_prompt(const std::vector<ChatMessage>& messages, const bool thinking) {
  return flint_text_prompt(messages, thinking, false);
}

std::optional<std::vector<PromptPiece>> render_flint_tool_prompt(const std::vector<ChatMessage>& messages,
                                                                 const std::span<const std::string> tools,
                                                                 const std::span<const ToolRound> rounds, const bool thinking) {
  if (tools.empty() || !sayable(rounds)) return std::nullopt;
  // The conversation obeys every rule of the plain prompt (roles, markers, escapes, sizes); an earlier answer's
  // searches those of this turn's rounds (history_ok).
  if (!flint_text_prompt(messages, thinking, true)) return std::nullopt;
  static constexpr std::array<std::string_view, 6U> kCallTags{"<tool_call>", "</tool_call>", "<arg_key>", "</arg_key>", "<arg_value>",
                                                              "</arg_value>"};
  // flint's vocabulary holds <tools> and </tools> as special tokens (USER_DEFINED, matched wherever the template writes
  // them, as llama.cpp tokenizes); written as text they were refused and the chat ran without its tool (owner 2026-10-28)
  static constexpr std::array<std::string_view, 2U> kToolsTags{"<tools>", "</tools>"};
  PromptBuilder out;
  // the template's tool_preamble, written before the first message that is no system message
  const auto preamble = [&out, tools] {
    out.marker("<|system|>");
    out.templated("\n# Tools\n\nYou may call one or more functions to assist with the user query.\n\nYou are provided with function "
                  "signatures within <tools></tools> XML tags:\n<tools>",
                  kToolsTags);
    for (const auto& tool : tools) out.text(tool);
    out.marker("</tools>");
    out.text("\n\nFor each function call, output the function name and arguments within the following XML format:\n");
    out.templated("<tool_call>{function-name}<arg_key>{arg-key-1}</arg_key><arg_value>{arg-value-1}</arg_value>...</tool_call>\n", kCallTags);
  };
  // A round: <|assistant|>, its reasoning, text and calls, then each result after its own <|observation|>. This turn's
  // rounds as run_with_mcp appends them, reasoning and text stripped; an earlier answer's as upstream's page sends them
  // (serve/web/app.js assistantMessages: per round the text before the calls and the calls, one tool message per
  // result), without reasoning, and the template writes their text as it is.
  const auto write_round = [&out](const ToolRound& round, const bool earlier) {
    out.marker("<|assistant|>");
    if (const std::string_view reasoning = python_strip(round.reasoning); !reasoning.empty()) {
      out.marker("<think>");
      out.plain(reasoning);
      out.marker("</think>");
    }
    out.plain(earlier ? std::string_view{round.content} : python_strip(round.content));
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
    for (const auto& result : round.results) {
      out.marker("<|observation|>");
      out.marker("<tool_response>");
      out.plain(flint_text(result));
      out.marker("</tool_response>");
    }
  };
  bool pending = true;
  for (const ChatMessage& message : messages) {
    if (pending && message.role != "system") {
      preamble();
      pending = false;
    }
    for (const auto& round : message.rounds) write_round(round, true);
    if (searches_only(true, message)) continue;  // the page sends no turn for an answer's empty rest
    if (message.role == "system" || message.role == "user") {
      out.marker(message.role == "system" ? "<|system|>" : "<|user|>");
      out.text(flint_text(message.content));
    } else {
      out.marker("<|assistant|>");
      out.text(message.content);
    }
  }
  if (pending) preamble();
  for (const auto& round : rounds) write_round(round, false);
  out.marker("<|assistant|>");
  if (!thinking) {
    out.marker("<think>");
    out.marker("</think>");
  }
  if (out.bytes() > kMaximumPromptBytes) return std::nullopt;
  return out.take();
}

}  // namespace secure_frontend
