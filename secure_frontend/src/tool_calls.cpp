#include "secure_frontend/tool_calls.hpp"

#include "secure_frontend/glm_tool_calls.hpp"
#include "secure_frontend/qwen_tool_calls.hpp"

#include <utility>

namespace secure_frontend {

std::unique_ptr<ToolCallParser> make_tool_call_parser(const ToolFormat format, std::vector<ToolDeclaration> tools) {
  // flint's template writes GLM's calls, raon-1-mini's Qwen's
  if (format == ToolFormat::glm || format == ToolFormat::flint) return std::make_unique<GlmToolCallParser>(std::move(tools));
  return std::make_unique<QwenToolCallParser>(std::move(tools));
}

}  // namespace secure_frontend
