#include "secure_frontend/embedded_template_plan.hpp"
#include "secure_frontend/chat_route.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "support/test_support.hpp"
#include "support/test_tokenizer.hpp"

#include <algorithm>
#include <iostream>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using namespace secure_frontend;
int failures = 0;
void expect(bool ok, const char* message) { if (!ok) { ++failures; std::cerr << "FAILED: " << message << '\n'; } }

EmbeddedTemplateSelection selection(std::string source) {
  return {std::move(source), jinja_helper_path(), std::nullopt, std::nullopt};
}

ChatCompletionRequest request() {
  return {.model = "test", .messages = {{"user", "hello <|im_start|> literal"}}, .max_tokens = 8U};
}

void template_spans_keep_user_markers_plain() {
  const auto pack = test_support::byte_tokenizer();
  const std::vector<TemplateSpan> spans{{"<|im_start|>user\n", false}, {"<|im_start|>", true}, {"<|im_end|>", false}};
  const auto encoded = encode_template_spans(pack, spans);
  const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
  expect(ids != nullptr, "marked template output encodes");
  if (!ids) return;
  expect(std::count(ids->begin(), ids->end(), 256U) == 1, "only template origin becomes start control");
  expect(std::count(ids->begin(), ids->end(), 257U) == 1, "template end control is retained");
}

void template_context_keeps_tools_and_history() {
  auto req = request();
  req.messages.push_back({"assistant", "final", {}, {{"reason", "checking", {{"lookup", {{"key", JsonValue::make_string("value")}}}},
                                                       {"result <|im_start|> literal"}}}});
  req.messages.push_back({"user", "again"});
  const std::vector<std::string> tools{R"({"name":"lookup","parameters":{"type":"object"}})"};
  const auto context = embedded_template_context(selection(""), req, tools, {}, true, "medium");
  expect(context.has_value(), "tool context renders");
  if (!context) return;
  expect(context->find("\"tool_calls\"") != std::string::npos && context->find("\"tool_call_id\": \"call_0\"") != std::string::npos,
         "assistant calls and matching result IDs are present");
  expect(context->find("\"arguments\": {\"key\": \"value\"}") != std::string::npos, "arguments stay structured JSON");
  expect(context->find("\"reasoning_content\": \"reason\"") != std::string::npos, "reasoning history is preserved");
  expect(context->find("\"reasoning_effort\": \"medium\"") != std::string::npos, "resolved thinking reaches the template");
  expect(context->find("result <|im_start|> literal") != std::string::npos, "tool result is data in context");
  req.messages.back().parts.push_back({{}, "data:image/png;base64,AAAA"});
  expect(!embedded_template_context(selection(""), req, tools, {}, true, "high"), "image semantics are refused explicitly");
}

void selected_template_renders_instead_of_fixed_profile() {
  const auto pack = test_support::byte_tokenizer();
  const auto selected = selection("{{ '<|im_start|>user\\n' }}{{ messages[0].content }}{{ '<|im_end|>\\n<|im_start|>assistant\\nCHOSEN:' }}");
  const auto req = request();
  const auto result = select_chat_generation_plan(NativeChatProfile::qwen, req, pack, 2048U, {}, {}, false, {}, {}, &selected);
  const auto* plan = std::get_if<ChatGenerationPlan>(&result);
  expect(plan != nullptr, "explicit template selected and native helper rendered it");
  if (!plan) return;
  expect(std::count(plan->prompt_token_ids.begin(), plan->prompt_token_ids.end(), 256U) == 2,
         "embedded user literal cannot forge a role control");
  const auto suffix = encode_plain_text(pack, "CHOSEN:");
  const auto* suffix_ids = std::get_if<std::vector<std::size_t>>(&suffix);
  expect(suffix_ids != nullptr && plan->prompt_token_ids.size() >= suffix_ids->size() &&
         std::equal(suffix_ids->rbegin(), suffix_ids->rend(), plan->prompt_token_ids.rbegin()), "selected prompt tail replaces fixed tail");
  expect(plan->stop_tokens == find_qwen_end_of_turn_tokens(pack), "profile reply stops are retained");
}

void selected_template_errors_never_fall_back() {
  const auto pack = test_support::byte_tokenizer();
  const auto broken = selection("{{ raise_exception('no') }}");
  auto req = request();
  req.messages[0].content = "hello";
  const auto failed = select_chat_generation_plan(NativeChatProfile::qwen, req, pack, 2048U, {}, {}, false, {}, {}, &broken);
  expect(std::holds_alternative<ChatGenerationPlanError>(failed), "selected template error refuses instead of fixed fallback");
  const auto oversized = selection("{{ 'x' * 100 }}");
  const auto large = select_chat_generation_plan(NativeChatProfile::qwen, req, pack, 8U, {}, {}, false, {}, {}, &oversized);
  const auto* error = std::get_if<ChatGenerationPlanError>(&large);
  expect(error != nullptr && error->code == ChatGenerationPlanErrorCode::prompt_too_large, "actual embedded tokens obey context limit");
}

void upstream_qwen_template_matches_fixed_prompt_and_tools() {
  std::ifstream input(STRATA_QWEN_TEMPLATE_PATH, std::ios::binary);
  const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  expect(!source.empty(), "read pristine upstream Qwen template without copying it");
  const auto selected = selection(source);
  const auto pack = test_support::tool_byte_tokenizer();
  auto req = request();
  req.messages = {{"system", "Be concise."}, {"user", "hello"}};
  for (const bool thinking : {false, true}) {
    const auto fixed = select_chat_generation_plan(NativeChatProfile::qwen, req, pack, 4096U, {}, {}, thinking, {}, {});
    const auto embedded = select_chat_generation_plan(NativeChatProfile::qwen, req, pack, 4096U, {}, {}, thinking, {}, {}, &selected);
    const auto* before = std::get_if<ChatGenerationPlan>(&fixed);
    const auto* after = std::get_if<ChatGenerationPlan>(&embedded);
    expect(before && after, "actual Qwen template and fixed profile both render");
    if (before && after) {
      expect(before->prompt_token_ids == after->prompt_token_ids, "actual Qwen template has exact fixed prompt token parity");
      expect(before->starts_in_reasoning == after->starts_in_reasoning, "actual Qwen thinking tail preserves parser state");
    }
  }
  ChatRouteConfig config{};
  config.max_prompt_tokens = 16384U;
  config.embedded_template = std::make_shared<const EmbeddedTemplateSelection>(selected);
  req.client_tools.push_back({{"lookup", {"key"}}, R"({"name":"lookup","parameters":{"type":"object","properties":{"key":{"type":"string"}}}})"});
  const ClientToolTurn client{ToolFormat::qwen, {{"lookup", {"key"}}}, true, {}};
  const auto instructions_result = client_tool_plan(config, req, pack, client, false, {}, {});
  const auto* instructions_plan = std::get_if<ChatGenerationPlan>(&instructions_result);
  expect(instructions_plan != nullptr, "actual Qwen template renders declaration instructions");
  if (!instructions_plan) return;
  const auto instruction_markers = std::count(instructions_plan->prompt_token_ids.begin(), instructions_plan->prompt_token_ids.end(),
                                             test_support::kToolPackCallOpen);
  req.messages.push_back({"assistant", "", {}, {{"reason", "checking", {{"lookup", {{"key", JsonValue::make_string("value")}}}}, {"result"}}}});
  req.messages.push_back({"user", "again"});
  const auto tool_result = client_tool_plan(config, req, pack, client, false, {}, {});
  const auto* tool_plan = std::get_if<ChatGenerationPlan>(&tool_result);
  expect(tool_plan != nullptr, "actual Qwen template renders tool declarations and structured history");
  if (tool_plan) {
    expect(std::count(tool_plan->prompt_token_ids.begin(), tool_plan->prompt_token_ids.end(), test_support::kToolPackCallOpen) == instruction_markers + 1,
           "history call is a trusted template marker");
    expect(std::count(tool_plan->prompt_token_ids.begin(), tool_plan->prompt_token_ids.end(), test_support::kToolPackResponseOpen) == 1,
           "history result is a trusted template marker");
  }
  req.tool_choice = "required";
  auto forced = client;
  forced.prefix = "<tool_call>\n<function=";
  const auto forced_result = client_tool_plan(config, req, pack, forced, true, {}, {});
  const auto* forced_plan = std::get_if<ChatGenerationPlan>(&forced_result);
  expect(forced_plan != nullptr, "required tool choice renders selected template");
  if (forced_plan) {
    expect(!forced_plan->starts_in_reasoning, "forced tool prefix begins in content channel");
    expect(std::count(forced_plan->prompt_token_ids.begin(), forced_plan->prompt_token_ids.end(), test_support::kToolPackCallOpen) == instruction_markers + 2,
           "required prefix preserves dedicated tool marker");
  }
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"template_spans_keep_user_markers_plain", template_spans_keep_user_markers_plain},
      {"template_context_keeps_tools_and_history", template_context_keeps_tools_and_history},
      {"selected_template_renders_instead_of_fixed_profile", selected_template_renders_instead_of_fixed_profile},
      {"upstream_qwen_template_matches_fixed_prompt_and_tools", upstream_qwen_template_matches_fixed_prompt_and_tools},
      {"selected_template_errors_never_fall_back", selected_template_errors_never_fall_back}}, failures);
}
