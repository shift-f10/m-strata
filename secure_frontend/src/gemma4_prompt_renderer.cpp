#include "secure_frontend/gemma4_prompt_renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace secure_frontend {
namespace {

// The bounds of the Qwen and GLM renderers (spec §9.5; session log step 13): 65,536 messages, a
// message of up to a whole 256 MiB body, and the rendered prompt that much plus room for every
// message's framing. A request is bounded by its config first.
constexpr std::size_t kMaximumMessages = 65536U;
constexpr std::size_t kMaximumMessageBytes = 256U * 1024U * 1024U;
constexpr std::size_t kMaximumPromptBytes = kMaximumMessageBytes + 16U * 1024U * 1024U;

constexpr std::string_view kBos = "<bos>";
constexpr std::string_view kTurnOpen = "<|turn>";
constexpr std::string_view kTurnClose = "<turn|>\n";
constexpr std::string_view kThink = "<|think|>";
constexpr std::string_view kClosedChannel = "<|channel>thought\n<channel|>";

constexpr std::array<std::string_view, 24U> kControlLiterals{
    "<pad>",     "<eos>",        "<bos>",        "<unk>",           "<mask>",           "<|tool>",
    "<tool|>",   "<|tool_call>", "<tool_call|>", "<|tool_response>", "<tool_response|>", "<|\"|>",
    "<|think|>", "<|channel>",   "<channel|>",   "<|turn>",         "<turn|>",          "<|image>",
    "<|audio>",  "<|image|>",    "<|audio|>",    "<image|>",        "<audio|>",         "<|video|>"};

// CPython 3.12's str.isspace() set, which str.strip() (Jinja's trim) removes.
constexpr std::array<std::uint32_t, 29U> kWhitespace{0x09U,   0x0AU,   0x0BU,   0x0CU,   0x0DU,   0x1CU,   0x1DU,   0x1EU,
                                                    0x1FU,   0x20U,   0x85U,   0xA0U,   0x1680U, 0x2000U, 0x2001U, 0x2002U,
                                                    0x2003U, 0x2004U, 0x2005U, 0x2006U, 0x2007U, 0x2008U, 0x2009U, 0x200AU,
                                                    0x2028U, 0x2029U, 0x202FU, 0x205FU, 0x3000U};

bool is_whitespace(const std::uint32_t point) {
  for (const auto space : kWhitespace) {
    if (space == point) return true;
  }
  return false;
}

bool continuation(const unsigned char byte) { return (byte & 0xC0U) == 0x80U; }

// The code point starting at `at` and its width; width 0 when the bytes there are not a complete
// sequence (the caller then stops: an incomplete sequence is never whitespace).
std::pair<std::uint32_t, std::size_t> code_point_at(const std::string_view text, const std::size_t at) {
  const auto lead = static_cast<unsigned char>(text[at]);
  std::size_t width = 0U;
  std::uint32_t point = 0U;
  if (lead < 0x80U) return {lead, 1U};
  if ((lead & 0xE0U) == 0xC0U) {
    width = 2U;
    point = lead & 0x1FU;
  } else if ((lead & 0xF0U) == 0xE0U) {
    width = 3U;
    point = lead & 0x0FU;
  } else if ((lead & 0xF8U) == 0xF0U) {
    width = 4U;
    point = lead & 0x07U;
  } else {
    return {0U, 0U};
  }
  if (width > text.size() - at) return {0U, 0U};
  for (std::size_t index = 1U; index < width; ++index) {
    const auto byte = static_cast<unsigned char>(text[at + index]);
    if (!continuation(byte)) return {0U, 0U};
    point = (point << 6U) | (byte & 0x3FU);
  }
  return {point, width};
}

bool valid_utf8(const std::string_view value) {
  for (std::size_t index = 0U; index < value.size();) {
    const auto byte = static_cast<unsigned char>(value[index]);
    if (byte <= 0x7FU) {
      ++index;
    } else if (byte >= 0xC2U && byte <= 0xDFU && index + 1U < value.size() &&
               continuation(static_cast<unsigned char>(value[index + 1U]))) {
      index += 2U;
    } else if (byte >= 0xE0U && byte <= 0xEFU && index + 2U < value.size() &&
               continuation(static_cast<unsigned char>(value[index + 1U])) &&
               continuation(static_cast<unsigned char>(value[index + 2U])) &&
               !(byte == 0xE0U && static_cast<unsigned char>(value[index + 1U]) < 0xA0U) &&
               !(byte == 0xEDU && static_cast<unsigned char>(value[index + 1U]) > 0x9FU)) {
      index += 3U;
    } else if (byte >= 0xF0U && byte <= 0xF4U && index + 3U < value.size() &&
               continuation(static_cast<unsigned char>(value[index + 1U])) &&
               continuation(static_cast<unsigned char>(value[index + 2U])) &&
               continuation(static_cast<unsigned char>(value[index + 3U])) &&
               !(byte == 0xF0U && static_cast<unsigned char>(value[index + 1U]) < 0x90U) &&
               !(byte == 0xF4U && static_cast<unsigned char>(value[index + 1U]) > 0x8FU)) {
      index += 4U;
    } else {
      return false;
    }
  }
  return true;
}

// The text a turn carries: bounded, valid UTF-8, no C0 control but \t \n \r, no Gemma literal,
// and something left after trim. Returns the trimmed view, empty when refused.
std::string_view safe_text(const ChatMessage& message) {
  const std::string_view content = message.content;
  if (!message.parts.empty() || content.empty() || content.size() > kMaximumMessageBytes || !valid_utf8(content)) return {};
  for (const unsigned char byte : content) {
    if (byte < 0x20U && byte != '\n' && byte != '\r' && byte != '\t') return {};
  }
  for (const auto literal : kControlLiterals) {
    if (content.find(literal) != std::string_view::npos) return {};
  }
  return gemma4_trim(content);
}

bool append_bounded(std::string* const prompt, const std::string_view text) {
  if (text.size() > kMaximumPromptBytes - prompt->size()) return false;
  prompt->append(text);
  return true;
}

bool append_turn(std::string* const prompt, const std::string_view role, const std::string_view prefix, const std::string_view text) {
  return append_bounded(prompt, kTurnOpen) && append_bounded(prompt, role) && append_bounded(prompt, "\n") &&
         append_bounded(prompt, prefix) && append_bounded(prompt, text) && append_bounded(prompt, kTurnClose);
}

}  // namespace

std::string_view gemma4_trim(std::string_view text) {
  while (!text.empty()) {
    const auto [point, width] = code_point_at(text, 0U);
    if (width == 0U || !is_whitespace(point)) break;
    text.remove_prefix(width);
  }
  while (!text.empty()) {
    // The last code point: step back over continuation bytes (at most three) to its lead byte.
    std::size_t start = text.size() - 1U;
    while (start > 0U && text.size() - start < 4U && continuation(static_cast<unsigned char>(text[start]))) --start;
    const auto [point, width] = code_point_at(text, start);
    if (width != text.size() - start || !is_whitespace(point)) break;
    text.remove_suffix(width);
  }
  return text;
}

const std::array<std::string_view, 24U>& gemma4_control_literals() { return kControlLiterals; }

std::optional<std::string> render_gemma4_text_prompt(const std::vector<ChatMessage>& messages, const bool thinking) {
  if (messages.empty() || messages.size() > kMaximumMessages) return std::nullopt;
  std::string prompt;
  prompt.reserve(256U);
  if (!append_bounded(&prompt, kBos)) return std::nullopt;

  std::size_t index = 0U;
  const bool has_system = messages.front().role == "system";
  if (has_system || thinking) {
    std::string_view system_text;
    if (has_system) {
      system_text = safe_text(messages.front());
      if (system_text.empty()) return std::nullopt;
      index = 1U;
    }
    if (!append_turn(&prompt, "system", thinking ? kThink : std::string_view{}, system_text)) return std::nullopt;
  }
  if (index == messages.size()) return std::nullopt;

  bool expect_user = true;
  for (; index < messages.size(); ++index) {
    const ChatMessage& message = messages[index];
    if (message.role != (expect_user ? "user" : "assistant")) return std::nullopt;
    const std::string_view text = safe_text(message);
    if (text.empty() || !append_turn(&prompt, expect_user ? "user" : "model", {}, text)) return std::nullopt;
    expect_user = !expect_user;
  }
  // A generation prompt follows a user turn only.
  if (expect_user || !append_bounded(&prompt, kTurnOpen) || !append_bounded(&prompt, "model\n")) return std::nullopt;
  if (!thinking && !append_bounded(&prompt, kClosedChannel)) return std::nullopt;
  return prompt;
}

}  // namespace secure_frontend
