#include "secure_frontend/deepseek_prompt_renderer.hpp"

#include "secure_frontend/json.hpp"
#include "secure_frontend/prompt_builder.hpp"
#include "secure_frontend/tokenizer_pack.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <variant>

namespace secure_frontend {
namespace {

// The bounds the GLM renderer keeps (spec §9.5; session log step 13): a request is bounded by its config first.
constexpr std::size_t kMaximumMessages = 65536U;
constexpr std::size_t kMaximumMessageBytes = 256U * 1024U * 1024U;
constexpr std::size_t kMaximumPromptBytes = kMaximumMessageBytes + 16U * 1024U * 1024U;
// ds41_encoding.py's names: bos_token, SYSTEM_SP_TOKEN, USER_SP_TOKEN, ASSISTANT_SP_TOKEN, thinking_start_token,
// thinking_end_token, eos_token (kDeepseekMarkers, in that order); dsml_token is the tool namespace.
const std::string_view kBos = kDeepseekMarkers[0];
const std::string_view kSystem = kDeepseekMarkers[1];
const std::string_view kUser = kDeepseekMarkers[2];
const std::string_view kAssistant = kDeepseekMarkers[3];
const std::string_view kThinkOpen = kDeepseekMarkers[4];
const std::string_view kThinkClose = kDeepseekMarkers[5];
const std::string_view kEos = kDeepseekMarkers[6];
constexpr std::string_view kDsml = "\xEF\xBD\x9C" "DSML\xEF\xBD\x9C";

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
  for (const auto marker : kDeepseekMarkers) {
    if (content.find(marker) != std::string_view::npos) return false;
  }
  if (content.find(kDsml) != std::string_view::npos) return false;
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

std::string effort_line(const int effort) {
  return "Reasoning Effort: " + std::to_string(effort) + " (range 1-100, the higher the value, the more thorough the reasoning)\n\n";
}

// ds41_encoding.py's TOOLS_TEMPLATE up to its {tool_schemas}, its names filled in (dsml_token, the " calls" block, the
// " invoke" and " parameter" tags, thinking_start_token and thinking_end_token), and what follows the schemas.
std::string tools_head() {
  const std::string d{kDsml};
  return "## Tools\n\nYou have access to a set of tools to help answer the user's question. You can invoke tools by writing a \"<" + d +
         " calls>\" block like the following:\n\n<" + d + " calls>\n<" + d + " invoke name=\"$TOOL_NAME\">\n<" + d +
         " parameter name=\"$PARAMETER_NAME\" string=\"true|false\">$PARAMETER_VALUE</" + d + " parameter>\n...\n</" + d +
         " invoke>\n<" + d + " invoke name=\"$TOOL_NAME2\">\n...\n</" + d + " invoke>\n</" + d +
         " calls>\n\nString parameters should be specified as is and set `string=\"true\"`. For all other types (numbers, booleans, "
         "arrays, objects), pass the value in JSON format and set `string=\"false\"`.\n\nIf thinking_mode is enabled (triggered by "
         "<think>), you MUST output your complete reasoning inside <think>...</think> BEFORE any tool calls or final response.\n\n"
         "Otherwise, output directly after </think> with tool calls or final response.\n\n### Available Tool Schemas\n\n";
}
constexpr std::string_view kToolsTail =
    "\n\nYou MUST strictly follow the above defined tool name and parameter schemas to invoke tool calls.\n";
constexpr JsonLimits kToolLimits{.max_bytes = 1U << 20U, .max_depth = 32U, .max_container_entries = 4096U,
                                 .max_string_bytes = 1U << 20U, .allow_fractions = true};

const JsonValue* member(const JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& entry : *members) {
    if (entry.key == key) return &entry.value;
  }
  return nullptr;
}

// A round the encoder can write: at least one call, each named, and one result per call.
bool valid_rounds(const std::span<const ToolRound> rounds) {
  for (const auto& round : rounds) {
    if (round.calls.empty() || round.results.size() != round.calls.size()) return false;
    for (const auto& call : round.calls) {
      if (call.name.empty()) return false;
    }
  }
  return true;
}

// The plain prompt, and its rules for the tool prompt. With `searches` (the tool prompt's check) an earlier answer's
// searches (ChatMessage::rounds) are rounds the encoder can write, only an answer has them, and an answer with them may
// have no text after them; without it (the plain prompt) they are ignored and the answer is its text.
std::optional<std::string> text_prompt(const std::vector<ChatMessage>& messages, const std::optional<int> effort,
                                       const bool searches) {
  if (messages.empty() || messages.size() > kMaximumMessages || (effort && (*effort < 1 || *effort > 100))) return std::nullopt;
  std::string output;
  output.reserve(256U);
  if (!append_bounded(&output, kBos)) return std::nullopt;

  // Index 0 (render_reasoning_effort, :619-630): <｜System｜> when there is an effort line or a system message, the
  // effort line (thinking mode only), then the system text.
  std::size_t index = 0U;
  const bool system = messages.front().role == "system";
  if (effort || system) {
    if (!append_bounded(&output, kSystem)) return std::nullopt;
  }
  if (effort) {
    const std::string line = "Reasoning Effort: " + std::to_string(*effort) +
                             " (range 1-100, the higher the value, the more thorough the reasoning)\n\n";
    if (!append_bounded(&output, line)) return std::nullopt;
  }
  if (system) {
    if (!safe_content(messages.front().content) || !append_bounded(&output, messages.front().content)) return std::nullopt;
    if (searches && !messages.front().rounds.empty()) return std::nullopt;
    index = 1U;
  }
  if (index == messages.size()) return std::nullopt;

  // User turns open an assistant header (the last one opens <think> in thinking mode, every other closes it); an earlier
  // assistant turn is its content and EOS, its reasoning dropped (drop_thinking, the turn before the last user's).
  bool expect_user = true;
  for (; index < messages.size(); ++index) {
    const ChatMessage& message = messages[index];
    const bool searched = searches && !message.rounds.empty();
    if (searched && (expect_user || !valid_rounds(message.rounds))) return std::nullopt;
    if (message.role != (expect_user ? "user" : "assistant") ||
        (!(searched && message.content.empty()) && !safe_content(message.content))) {
      return std::nullopt;
    }
    if (expect_user) {
      const bool last = index + 1U == messages.size();
      if (!append_bounded(&output, kUser) || !append_bounded(&output, message.content) ||
          !append_bounded(&output, kAssistant) || !append_bounded(&output, effort && last ? kThinkOpen : kThinkClose)) {
        return std::nullopt;
      }
    } else if (!append_bounded(&output, message.content) || !append_bounded(&output, kEos)) {
      return std::nullopt;
    }
    expect_user = !expect_user;
  }
  if (expect_user) return std::nullopt;  // the conversation ends in a user turn
  return output;
}

}  // namespace

std::optional<std::string> render_deepseek_text_prompt(const std::vector<ChatMessage>& messages, const std::optional<int> effort) {
  return text_prompt(messages, effort, false);
}

std::optional<std::vector<PromptPiece>> render_deepseek_tool_prompt(const std::vector<ChatMessage>& messages,
                                                                    const std::span<const std::string> tools,
                                                                    const std::span<const ToolRound> rounds,
                                                                    const std::optional<int> effort) {
  // The conversation obeys every rule of the plain prompt (roles, order, literals, sizes, the effort's range), and an
  // earlier answer's searches the rules of the rounds.
  if (tools.empty() || !text_prompt(messages, effort, true) || !valid_rounds(rounds)) return std::nullopt;
  // tools_from_openai_format: each tool's function object, written by to_json (json.dumps, ensure_ascii off)
  std::vector<std::string> schemas;
  for (const auto& tool : tools) {
    auto parsed = parse_json(tool, kToolLimits);
    const auto* value = std::get_if<JsonValue>(&parsed);
    if (value == nullptr || value->if_object() == nullptr) return std::nullopt;
    const JsonValue* wrapped = member(*value, "function");
    const JsonValue& function = wrapped != nullptr && wrapped->if_object() != nullptr ? *wrapped : *value;
    const JsonValue* name = member(function, "name");
    // (a "::" name is a namespace to DeepSeek's encoder; ours have none)
    if (name == nullptr || name->if_string() == nullptr || name->if_string()->empty() ||
        name->if_string()->find("::") != std::string::npos) {
      return std::nullopt;
    }
    schemas.push_back(python_json_text(function));
  }
  const bool thinking = effort.has_value();
  const std::array<std::string_view, 3> tags{kDsml, kThinkOpen, kThinkClose};
  PromptBuilder out;
  // index 0: the system message the tools ride on (render_message: <｜System｜>, the effort line, its text, the tools)
  out.marker(kBos);
  out.marker(kSystem);
  if (effort) out.text(effort_line(*effort));
  std::size_t index = 0U;
  if (messages.front().role == "system") {
    out.text(messages.front().content);
    index = 1U;
  }
  out.text("\n\n");
  out.templated(tools_head(), tags);
  for (std::size_t i = 0; i < schemas.size(); ++i) {
    if (i != 0U) out.text("\n");
    out.plain(schemas[i]);
  }
  out.text(kToolsTail);
  // With tools the encoder keeps every turn's thinking: each assistant header opens <think> (thinking) or closes it.
  const auto header = [&out, thinking] {
    out.marker(kAssistant);
    out.marker(thinking ? kThinkOpen : kThinkClose);
  };
  // A round: the assistant's reasoning and content, its calls in DSML (tool_calls_template), EOS; then its results as
  // one user turn of <tool_result> blocks (merge_tool_messages), which the next header (or a user's text) follows.
  const auto dsml_tag = [&out](const std::string_view before, const std::string_view after) {
    out.text(before);
    out.marker(kDsml);
    out.text(after);
  };
  const auto write_round = [&out, &dsml_tag, thinking](const ToolRound& round) {
    if (thinking) {
      out.plain(round.reasoning);
      out.marker(kThinkClose);
    }
    out.plain(round.content);
    dsml_tag("\n\n<", " calls>\n");
    for (std::size_t c = 0; c < round.calls.size(); ++c) {
      const ToolCall& call = round.calls[c];
      if (c != 0U) out.text("\n");
      dsml_tag("<", " invoke name=\"");
      out.plain(call.name);
      out.text("\">\n");
      for (std::size_t a = 0; a < call.arguments.size(); ++a) {
        const ToolArgument& argument = call.arguments[a];
        const std::string* text = argument.value.if_string();
        if (a != 0U) out.text("\n");
        dsml_tag("<", " parameter name=\"");
        out.plain(argument.name);
        out.text(text != nullptr ? "\" string=\"true\">" : "\" string=\"false\">");
        out.plain(text != nullptr ? *text : python_json_text(argument.value));
        dsml_tag("</", " parameter>");
      }
      dsml_tag("\n</", " invoke>");
    }
    dsml_tag("\n</", " calls>");
    out.marker(kEos);
    out.marker(kUser);
    for (std::size_t r = 0; r < round.results.size(); ++r) {
      if (r != 0U) out.text("\n\n");
      out.text("<tool_result>");
      out.plain(round.results[r]);
      out.text("</tool_result>");
    }
  };
  // An earlier answer with searches (owner 2026-10-29, "keep earlier searches in the chat history as upstream's page
  // does") is the messages upstream's page sends for it: per round an assistant message (the text before its calls,
  // its calls) and one tool message per result, then the text after its last round only when there is some. The
  // encoder writes each as a round of this turn is written (its reasoning empty); without text after them, the next
  // user's text joins the results' user turn ("\n\n", merge_tool_messages).
  bool results_open = false;
  for (; index < messages.size(); ++index) {
    const ChatMessage& message = messages[index];
    if (message.role == "user") {
      if (results_open) out.text("\n\n");
      else out.marker(kUser);
      results_open = false;
      out.text(message.content);
      header();
      continue;
    }
    for (std::size_t r = 0; r < message.rounds.size(); ++r) {
      if (r != 0U) header();
      write_round(message.rounds[r]);
    }
    if (!message.rounds.empty()) {
      if (message.content.empty()) {
        results_open = true;
        continue;
      }
      header();
    }
    // an earlier assistant turn: its (empty) reasoning closed, its content, EOS
    if (thinking) out.marker(kThinkClose);
    out.text(message.content);
    out.marker(kEos);
  }
  for (const auto& round : rounds) {
    write_round(round);
    header();
  }
  if (out.bytes() > kMaximumPromptBytes) return std::nullopt;
  return out.take();
}

}  // namespace secure_frontend
