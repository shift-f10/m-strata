#pragma once

// Building a prompt in pieces (prompt_piece.hpp) for the tool renderers (render_qwen_tool_prompt,
// render_glm53_tool_prompt): adjacent text, or plain, pieces are joined; empty ones are dropped.

#include "secure_frontend/json.hpp"
#include "secure_frontend/prompt_piece.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace secure_frontend {

class PromptBuilder final {
 public:
  void marker(const std::string_view literal) { add(PromptPiece::Kind::marker, literal); }
  void text(const std::string_view value) { add(PromptPiece::Kind::text, value); }
  void plain(const std::string_view value) { add(PromptPiece::Kind::plain, value); }
  // The template's own text, in which `tags` are its special tokens.
  void templated(std::string_view value, const std::span<const std::string_view> tags) {
    for (;;) {
      std::size_t at = std::string_view::npos;
      std::string_view tag;
      for (const std::string_view candidate : tags) {
        const std::size_t found = value.find(candidate);
        if (found < at || (found == at && candidate.size() > tag.size())) {
          at = found;
          tag = candidate;
        }
      }
      if (at == std::string_view::npos) break;
      text(value.substr(0U, at));
      marker(tag);
      value.remove_prefix(at + tag.size());
    }
    text(value);
  }
  [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::vector<PromptPiece> take() { return std::move(pieces_); }

 private:
  void add(const PromptPiece::Kind kind, const std::string_view value) {
    if (value.empty()) return;
    bytes_ += value.size();
    if (kind != PromptPiece::Kind::marker && !pieces_.empty() && pieces_.back().kind == kind) pieces_.back().text.append(value);
    else pieces_.push_back({kind, std::string{value}});
  }
  std::vector<PromptPiece> pieces_;
  std::size_t bytes_{0};
};

// A call's argument as the templates write it back: a string as it is, any other value as Python's json.dumps writes
// it (Qwen3.8's `args_value | tojson`, GLM-5.3's `v | tojson(ensure_ascii=False)`).
[[nodiscard]] inline std::string template_argument_text(const JsonValue& value) {
  if (const auto* text = value.if_string()) return *text;
  return python_json_text(value);
}

}  // namespace secure_frontend
