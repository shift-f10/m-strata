#include "secure_frontend/minimax_prompt_renderer.hpp"

#include "secure_frontend/json.hpp"
#include "secure_frontend/prompt_builder.hpp"
#include "secure_frontend/python_text.hpp"

#include <algorithm>
#include <variant>

namespace secure_frontend {
namespace {

// The template's own text: Maya v1.1.30's tools/mm3/chat_template.jinja, which is MiniMax's (MiniMaxAI/MiniMax-M3.1-Flash
// at 54e4e69, "Copyright (c) 2026 MiniMax", the MiniMax Community License: engine/third_party/maya/LICENSE.minimax-m31)
// plus Maya's "disabled" mode line in MiniMax-M3's words. Kept verbatim: the model was trained on it.
constexpr std::string_view kSystemText =
    "Your model version is MiniMax-M3.1, developed by MiniMax. Knowledge cutoff: January 2026. Founded in early 2022, "
    "MiniMax is a global AI foundation model company committed to advancing the frontiers of AI towards AGI.";
constexpr std::string_view kInstructions =
    "\n\n<thinking_instructions>\nYou have a thinking capability that allows you to reason step by step before responding. "
    "When thinking is enabled, wrap your reasoning in <mm:think></mm:think> tags before your response. When thinking is "
    "disabled, begin your response directly after the </mm:think> prefix. When thinking is adaptive, decide on your own "
    "whether to think for the current turn.\n";
constexpr std::string_view kAdaptive =
    "Current thinking mode: adaptive. You are encouraged to think for complex decision-making, multi-step reasoning, or "
    "when analyzing function/tool results.\n";
constexpr std::string_view kDisabled = "Current thinking mode: disabled. Do not output any thinking process.\n";
constexpr std::string_view kModelId = "</thinking_instructions>\n\n<model_id>8bb8149fe</model_id>";
constexpr std::string_view kDeveloperDefault = "You are a helpful assistant.";  // the template's model_identity
// MiniMax's levels (its model card: "low, medium, high, xhigh or max"; Maya's EFFORT_NAMES)
constexpr std::array<std::string_view, 5> kEfforts{"low", "medium", "high", "xhigh", "max"};
// The tool section the template adds to the developer turn (its tools in <tools>, then how to call them, in the tool
// namespace, ns_token).
constexpr std::string_view kNs = kMinimaxToolNamespace;
constexpr std::string_view kToolsHead =
    "\n\n# Tools\nYou may call one or more tools to assist with the user query.\nHere are the tools available in JSONSchema "
    "format:\n\n<tools>\n";
std::string tools_tail() {
  const std::string ns{kNs};
  return "</tools>\n\nTo call tools, wrap all invocations in a single " + ns + "<tool_call>" + ns +
         "</tool_call> block. Parameter values containing nested objects or arrays are recursively expanded into XML "
         "elements. Example:\n\n" +
         ns + "<tool_call>\n" + ns + "<invoke name=\"tool-name-1\">" + ns + "<param-1>value-1" + ns + "</param-1>" + ns +
         "<param-2>" + ns + "<item>" + ns + "<key-a>val-a" + ns + "</key-a>" + ns + "<key-b>val-b" + ns + "</key-b>" + ns +
         "</item>" + ns + "</param-2>" + ns + "</invoke>\n" + ns + "<invoke name=\"tool-name-2\">" + ns + "<param-1>value-1" +
         ns + "</param-1>" + ns + "</invoke>\n" + ns + "</tool_call>";
}
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

// str.strip('\n')
std::string_view newlines_stripped(std::string_view text) {
  while (!text.empty() && text.front() == '\n') text.remove_prefix(1U);
  while (!text.empty() && text.back() == '\n') text.remove_suffix(1U);
  return text;
}

// Both prompts: the text prompt has no schemas and no rounds.
std::optional<std::vector<PromptPiece>> render(const std::vector<ChatMessage>& messages, const MinimaxThinking& thinking,
                                               const std::span<const std::string_view> special,
                                               const std::span<const std::string> schemas, const std::span<const ToolRound> rounds) {
  if (messages.empty() || messages.back().role != "user") return std::nullopt;
  if (thinking.effort && std::find(kEfforts.begin(), kEfforts.end(), *thinking.effort) == kEfforts.end()) return std::nullopt;
  const auto is_special = [special](const std::string_view literal) {
    return std::find(special.begin(), special.end(), literal) != special.end();
  };
  for (const ChatMessage& message : messages) {
    if (message.role != "system" && message.role != "user" && message.role != "assistant") return std::nullopt;
    // where </mm:think> is no special token Maya does not mark it, and the template takes the answer apart at it
    if (message.role == "assistant" && !is_special("</mm:think>") && message.content.find("</mm:think>") != std::string::npos) {
      return std::nullopt;
    }
  }
  PromptBuilder out;
  // a literal the template writes: the special token where the pack has one, else its text (Maya's parse_special)
  const auto literal = [&](const std::string_view text) {
    if (is_special(text)) out.marker(text);
    else out.text(text);
  };
  const auto own = [&](const std::string_view text) { out.templated(text, special); };
  // a developer turn: the system message's text, or the template's identity line when it has none; the first one
  // carries the tools
  const auto developer = [&](const ChatMessage* message, const bool with_tools) {
    literal("]~b]");
    own("developer\n");
    if (message != nullptr && !message->content.empty()) out.plain(message->content);
    else own(kDeveloperDefault);
    if (with_tools) {
      own(kToolsHead);
      for (const std::string& schema : schemas) {
        own("<tool>");
        out.plain(schema);
        own("</tool>\n");
      }
      own(tools_tail());
    }
    literal("[e~[");
    own("\n");
  };
  // to_xml: an object's members (null ones left out) and an array's items as namespaced elements, a scalar as the
  // template writes it (true and false as JSON, a string as it is, a number as Python's str)
  const auto element = [&](const auto& self, const std::string_view name, const JsonValue& value) -> void {
    literal(kNs);
    own("<");
    out.plain(name);
    own(">");
    if (const auto* members = value.if_object()) {
      for (const auto& entry : *members) {
        if (entry.value.kind() != JsonKind::null) self(self, entry.key, entry.value);
      }
    } else if (const auto* items = value.if_array()) {
      for (const auto& item : *items) self(self, "item", item);
    } else if (value.kind() != JsonKind::null) {
      out.plain(template_argument_text(value));
    }
    literal(kNs);
    own("</");
    out.plain(name);
    own(">");
  };

  // The system turn: MiniMax's own text, the thinking instructions and mode, the model id, the effort.
  literal("]~!b[");
  literal("]~b]");
  own("system\n");
  own(kSystemText);
  own(kInstructions);
  own(thinking.off ? kDisabled : kAdaptive);
  own(kModelId);
  if (!thinking.off && thinking.effort) own("\n\n<effort>" + *thinking.effort + "</effort>");
  literal("[e~[");
  own("\n");
  // The developer turn: a leading system message, else the identity line; then the tools.
  const bool leading_system = messages.front().role == "system";
  developer(leading_system ? &messages.front() : nullptr, !schemas.empty());
  for (std::size_t i = leading_system ? 1U : 0U; i < messages.size(); ++i) {
    const ChatMessage& message = messages[i];
    if (message.role == "system") {  // a later system message keeps its place, as a developer turn
      developer(&message, false);
      continue;
    }
    literal("]~b]");
    if (message.role == "assistant") {
      own("ai\n");
      literal("</mm:think>");  // no reasoning sent back: the template's prefix of a turn without thinking
    } else {
      own("user\n");
    }
    out.plain(message.content);
    literal("[e~[");
    own("\n");
  }
  // The search rounds, as Maya's run_with_mcp appends them (each stripped; the reasoning only when there was some): the
  // assistant's turn with its calls, then one tool turn with a <response> per result.
  for (const ToolRound& round : rounds) {
    std::string_view reasoning = python_strip(round.reasoning);
    std::string_view content = python_strip(round.content);
    if (round.reasoning.empty() && !is_special("</mm:think>")) {
      // no reasoning_content: the template takes the reasoning out of a content that holds </mm:think> as text
      if (const std::size_t end = content.find("</mm:think>"); end != std::string_view::npos) {
        reasoning = newlines_stripped(content.substr(0U, end));
        if (const std::size_t open = reasoning.rfind("<mm:think>"); open != std::string_view::npos) {
          reasoning = newlines_stripped(reasoning.substr(open + 10U));
        }
        content = newlines_stripped(content.substr(content.rfind("</mm:think>") + 11U));
      }
    }
    literal("]~b]");
    own("ai\n");
    if (!reasoning.empty()) {
      literal("<mm:think>");
      out.plain(reasoning);
    }
    literal("</mm:think>");
    out.plain(content);
    literal(kNs);
    own("<tool_call>\n");
    for (const ToolCall& call : round.calls) {
      literal(kNs);
      own("<invoke name=\"");
      out.plain(call.name);
      own("\">");
      for (const ToolArgument& argument : call.arguments) {
        if (argument.value.kind() != JsonKind::null) element(element, argument.name, argument.value);
      }
      literal(kNs);
      own("</invoke>\n");
    }
    literal(kNs);
    own("</tool_call>");
    literal("[e~[");
    own("\n");
    literal("]~b]");
    own("tool");
    for (const std::string& result : round.results) {
      own("\n<response>");
      out.plain(result);
      own("</response>");
    }
    literal("[e~[");
    own("\n");
  }
  // The generation prompt: adaptive thinking leaves it to the model; off closes the block.
  literal("]~b]");
  own("ai\n");
  if (thinking.off) literal("</mm:think>");
  return out.take();
}

}  // namespace

std::optional<std::vector<PromptPiece>> render_minimax_text_prompt(const std::vector<ChatMessage>& messages,
                                                                   const MinimaxThinking& thinking,
                                                                   const std::span<const std::string_view> special) {
  return render(messages, thinking, special, {}, {});
}

std::optional<std::vector<PromptPiece>> render_minimax_tool_prompt(const std::vector<ChatMessage>& messages,
                                                                   const std::span<const std::string> tools,
                                                                   const std::span<const ToolRound> rounds,
                                                                   const MinimaxThinking& thinking,
                                                                   const std::span<const std::string_view> special) {
  if (tools.empty()) return std::nullopt;
  for (const auto& round : rounds) {
    if (round.calls.empty() || round.results.size() != round.calls.size()) return std::nullopt;
    for (const auto& call : round.calls) {
      if (call.name.empty()) return std::nullopt;
    }
  }
  // each tool's function object, as the template's tojson(ensure_ascii=False) writes it
  std::vector<std::string> schemas;
  for (const auto& tool : tools) {
    auto parsed = parse_json(tool, kToolLimits);
    const auto* value = std::get_if<JsonValue>(&parsed);
    if (value == nullptr || value->if_object() == nullptr) return std::nullopt;
    const JsonValue* wrapped = member(*value, "function");
    const JsonValue& function = wrapped != nullptr && wrapped->if_object() != nullptr ? *wrapped : *value;
    const JsonValue* name = member(function, "name");
    if (name == nullptr || name->if_string() == nullptr || name->if_string()->empty()) return std::nullopt;
    schemas.push_back(python_json_text(function));
  }
  return render(messages, thinking, special, schemas, rounds);
}

}  // namespace secure_frontend
