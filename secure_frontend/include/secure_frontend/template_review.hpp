#pragma once

#include "secure_frontend/canonical_path.hpp"
#include <string>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {
struct ComposedLaunch;
inline constexpr std::size_t kMaxEmbeddedTemplateBytes = 256U * 1024U;
struct TemplateRisk final {
  std::size_t line;
  std::size_t end_line;
  std::string code;
  std::string message;
};
struct EmbeddedChatTemplate final {
  std::string source;
  std::string sha256;
  std::string origin;
  std::vector<TemplateRisk> risks;
  std::optional<std::string> bos_token{};
  std::optional<std::string> eos_token{};
  std::optional<std::size_t> bos_id{};
  std::optional<std::size_t> eos_id{};
};
// Read data only. Never interprets source; returns fixed errors, not model bytes.
[[nodiscard]] std::variant<EmbeddedChatTemplate, std::string> read_embedded_chat_template(
    const CanonicalRoot& models, const ComposedLaunch& launch);
[[nodiscard]] std::vector<TemplateRisk> review_template_risks(std::string_view source);
[[nodiscard]] std::string template_review_json(const EmbeddedChatTemplate& review, bool selected);
}  // namespace secure_frontend
