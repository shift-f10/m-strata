#include "secure_frontend/tool_calls.hpp"

#include "secure_frontend/deepseek_tool_calls.hpp"
#include "secure_frontend/gemma4_tool_calls.hpp"
#include "secure_frontend/glm_tool_calls.hpp"
#include "secure_frontend/qwen_tool_calls.hpp"

#include <utility>

namespace secure_frontend {

std::unique_ptr<ToolCallParser> make_tool_call_parser(const ToolFormat format, std::vector<ToolDeclaration> tools) {
  switch (format) {
    case ToolFormat::glm:
    case ToolFormat::flint: return std::make_unique<GlmToolCallParser>(std::move(tools));  // flint's template writes GLM's calls
    case ToolFormat::gemma4: return std::make_unique<Gemma4ToolCallParser>(std::move(tools));
    case ToolFormat::deepseek: return std::make_unique<DeepseekToolCallParser>(std::move(tools));
    case ToolFormat::qwen:
    case ToolFormat::raon1: break;  // raon-1-mini's writes Qwen's
  }
  return std::make_unique<QwenToolCallParser>(std::move(tools));
}

}  // namespace secure_frontend
