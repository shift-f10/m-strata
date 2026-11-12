#pragma once

#include "secure_frontend/chat_generation_plan.hpp"
#include "secure_frontend/jinja_renderer.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace secure_frontend {

// Immutable, explicitly reviewed template snapshot for one running model.
// No file is reread while rendering a request.
struct EmbeddedTemplateSelection final {
  std::string source;
  std::filesystem::path helper;
  std::optional<std::string> bos_token;
  std::optional<std::string> eos_token;
};

[[nodiscard]] std::optional<std::string> embedded_template_context(
    const EmbeddedTemplateSelection& selected, const ChatCompletionRequest& request,
    std::span<const std::string> tools, std::span<const ToolRound> trailing,
    bool thinking, std::string_view effort);

// Keeps the profile's reply parsing/stops and sampling, but uses only the
// selected template's prompt. A renderer failure never falls back silently.
[[nodiscard]] ChatGenerationPlanResult apply_embedded_template(
    ChatGenerationPlanResult baseline, const EmbeddedTemplateSelection& selected,
    const ChatCompletionRequest& request, const TokenizerPack& tokenizer,
    std::span<const std::string> tools, std::span<const ToolRound> trailing,
    std::size_t maximum_prompt_tokens, bool thinking, std::string_view effort);

[[nodiscard]] TokenizerEncodeResult encode_template_spans(const TokenizerPack& tokenizer,
                                                         std::span<const TemplateSpan> spans);

}  // namespace secure_frontend
