#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {
inline constexpr std::size_t kTemplateSourceMax = 256U * 1024U;
inline constexpr std::size_t kTemplateContextMax = 4U * 1024U * 1024U;
inline constexpr std::size_t kTemplateOutputMax = 1024U * 1024U;
struct TemplateSpan final { std::string text; bool input; };
enum class TemplateRenderError { invalid_request, unavailable, rejected, resource_limit };
using TemplateRenderResult = std::variant<std::vector<TemplateSpan>, TemplateRenderError>;
[[nodiscard]] std::filesystem::path jinja_helper_path();

// The helper is the installed strata-template beside the server, never a model-supplied path.
// Native value-only Jinja, a fresh bounded process per render; errors contain no source or input.
[[nodiscard]] TemplateRenderResult render_jinja_template(const std::filesystem::path& helper,
                                                        std::string_view source,
                                                        std::string_view context_json);
} // namespace secure_frontend
