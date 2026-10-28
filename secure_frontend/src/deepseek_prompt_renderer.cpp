#include "secure_frontend/deepseek_prompt_renderer.hpp"

#include "secure_frontend/tokenizer_pack.hpp"

#include <cstddef>
#include <string_view>

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

}  // namespace

std::optional<std::string> render_deepseek_text_prompt(const std::vector<ChatMessage>& messages, const std::optional<int> effort) {
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
    index = 1U;
  }
  if (index == messages.size()) return std::nullopt;

  // User turns open an assistant header (the last one opens <think> in thinking mode, every other closes it); an earlier
  // assistant turn is its content and EOS, its reasoning dropped (drop_thinking, the turn before the last user's).
  bool expect_user = true;
  for (; index < messages.size(); ++index) {
    const ChatMessage& message = messages[index];
    if (message.role != (expect_user ? "user" : "assistant") || !safe_content(message.content)) return std::nullopt;
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

}  // namespace secure_frontend
