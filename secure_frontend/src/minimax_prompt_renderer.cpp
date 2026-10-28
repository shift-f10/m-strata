#include "secure_frontend/minimax_prompt_renderer.hpp"

#include "secure_frontend/prompt_builder.hpp"

#include <algorithm>

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

}  // namespace

std::optional<std::vector<PromptPiece>> render_minimax_text_prompt(const std::vector<ChatMessage>& messages,
                                                                   const MinimaxThinking& thinking,
                                                                   const std::span<const std::string_view> special) {
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
  // a developer turn: the system message's text, or the template's identity line when it has none
  const auto developer = [&](const ChatMessage* message) {
    literal("]~b]");
    own("developer\n");
    if (message != nullptr && !message->content.empty()) out.plain(message->content);
    else own(kDeveloperDefault);
    literal("[e~[");
    own("\n");
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
  // The developer turn: a leading system message, else the identity line.
  const bool leading_system = messages.front().role == "system";
  developer(leading_system ? &messages.front() : nullptr);
  for (std::size_t i = leading_system ? 1U : 0U; i < messages.size(); ++i) {
    const ChatMessage& message = messages[i];
    if (message.role == "system") {  // a later system message keeps its place, as a developer turn
      developer(&message);
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
  // The generation prompt: adaptive thinking leaves it to the model; off closes the block.
  literal("]~b]");
  own("ai\n");
  if (thinking.off) literal("</mm:think>");
  return out.take();
}

}  // namespace secure_frontend
