// DeepSeek-V4.1-Flash on Maya's maya-ds4 (spec docs/superpowers/specs/2026-10-27-strata-prepare-design.md §7.2): the
// prompt DeepSeek's own encoder writes, its markers as single tokens, the reply's split and its server stops. Every
// expected prompt below is Maya v1.1.30's serve/ds41_encoding.py encode_messages output (drop_thinking, Maya's default)
// for the same messages, printed byte for byte.
#include "secure_frontend/chat_generation_plan.hpp"
#include "secure_frontend/deepseek_prompt_renderer.hpp"
#include "support/test_support.hpp"
#include "support/test_tokenizer.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;
void expect(const bool value, const std::string_view message) {
  if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

using secure_frontend::ChatMessage;

constexpr std::string_view kBos = "<\xEF\xBD\x9C" "begin\xE2\x96\x81of\xE2\x96\x81sentence\xEF\xBD\x9C>";
constexpr std::string_view kSystem = "<\xEF\xBD\x9CSystem\xEF\xBD\x9C>";
constexpr std::string_view kUser = "<\xEF\xBD\x9CUser\xEF\xBD\x9C>";
constexpr std::string_view kAssistant = "<\xEF\xBD\x9C" "Assistant\xEF\xBD\x9C>";
constexpr std::string_view kEos = "<\xEF\xBD\x9C" "end\xE2\x96\x81of\xE2\x96\x81sentence\xEF\xBD\x9C>";

// Every byte as an ordinary token, then DeepSeek's markers: the framing ones CONTROL, the think pair USER_DEFINED.
secure_frontend::TokenizerPack deepseek_pack() {
  auto pack = test_support::byte_tokenizer();
  for (const std::string_view marker : {kBos, kSystem, kUser, kAssistant, kEos}) {
    pack.tokens_by_id.emplace_back(marker);
    pack.token_types.push_back(3);
  }
  for (const std::string_view marker : {std::string_view{"<think>"}, std::string_view{"</think>"}}) {
    pack.tokens_by_id.emplace_back(marker);
    pack.token_types.push_back(4);
  }
  pack.mode = secure_frontend::TokenizerMode::deepseek_bpe;
  return pack;
}

std::size_t token_id(const secure_frontend::TokenizerPack& pack, const std::string_view value) {
  for (std::size_t id = 0U; id < pack.tokens_by_id.size(); ++id) {
    if (pack.tokens_by_id[id] == value) return id;
  }
  return pack.tokens_by_id.size();
}

const std::vector<ChatMessage> kTwoTurns = {
    {"system", "Be concise.", {}}, {"user", "Hello", {}}, {"assistant", "Hi", {}}, {"user", "Continue", {}}};

void renders_deepseeks_encoder_byte_for_byte() {
  const auto render = [](const std::vector<ChatMessage>& messages, const std::optional<int> effort) {
    return secure_frontend::render_deepseek_text_prompt(messages, effort).value_or("<refused>");
  };
  expect(render({{"user", "Hi", {}}}, 75) ==
             "<\xEF\xBD\x9C" "begin\xE2\x96\x81of\xE2\x96\x81sentence\xEF\xBD\x9C><\xEF\xBD\x9CSystem\xEF\xBD\x9C>Reasoning "
             "Effort: 75 (range 1-100, the higher the value, the more thorough the reasoning)\n\n<\xEF\xBD\x9CUser\xEF\xBD"
             "\x9C>Hi<\xEF\xBD\x9C" "Assistant\xEF\xBD\x9C><think>",
         "one user turn, thinking at 75: the effort line opens the system block, the header opens <think>");
  expect(render(kTwoTurns, 75) ==
             "<\xEF\xBD\x9C" "begin\xE2\x96\x81of\xE2\x96\x81sentence\xEF\xBD\x9C><\xEF\xBD\x9CSystem\xEF\xBD\x9C>Reasoning "
             "Effort: 75 (range 1-100, the higher the value, the more thorough the reasoning)\n\nBe concise.<\xEF\xBD\x9C"
             "User\xEF\xBD\x9C>Hello<\xEF\xBD\x9C" "Assistant\xEF\xBD\x9C></think>Hi<\xEF\xBD\x9C" "end\xE2\x96\x81of\xE2"
             "\x96\x81sentence\xEF\xBD\x9C><\xEF\xBD\x9CUser\xEF\xBD\x9C>Continue<\xEF\xBD\x9C" "Assistant\xEF\xBD\x9C>"
             "<think>",
         "system and two turns, thinking: the earlier header closes the block, the last one opens it");
  expect(render(kTwoTurns, std::nullopt) ==
             "<\xEF\xBD\x9C" "begin\xE2\x96\x81of\xE2\x96\x81sentence\xEF\xBD\x9C><\xEF\xBD\x9CSystem\xEF\xBD\x9C>Be "
             "concise.<\xEF\xBD\x9CUser\xEF\xBD\x9C>Hello<\xEF\xBD\x9C" "Assistant\xEF\xBD\x9C></think>Hi<\xEF\xBD\x9C"
             "end\xE2\x96\x81of\xE2\x96\x81sentence\xEF\xBD\x9C><\xEF\xBD\x9CUser\xEF\xBD\x9C>Continue<\xEF\xBD\x9C"
             "Assistant\xEF\xBD\x9C></think>",
         "chat mode: no effort line, every header closes the block");
  expect(render({{"user", "Hi", {}}}, 50) ==
             "<\xEF\xBD\x9C" "begin\xE2\x96\x81of\xE2\x96\x81sentence\xEF\xBD\x9C><\xEF\xBD\x9CSystem\xEF\xBD\x9C>Reasoning "
             "Effort: 50 (range 1-100, the higher the value, the more thorough the reasoning)\n\n<\xEF\xBD\x9CUser\xEF\xBD"
             "\x9C>Hi<\xEF\xBD\x9C" "Assistant\xEF\xBD\x9C><think>",
         "low effort is 50");
  expect(render({{"user", "Hi", {}}}, std::nullopt) ==
             "<\xEF\xBD\x9C" "begin\xE2\x96\x81of\xE2\x96\x81sentence\xEF\xBD\x9C><\xEF\xBD\x9CUser\xEF\xBD\x9C>Hi"
             "<\xEF\xBD\x9C" "Assistant\xEF\xBD\x9C></think>",
         "chat mode without a system message has no system block");
}

void refuses_what_the_fixed_profile_cannot_say() {
  const auto refused = [](const std::vector<ChatMessage>& messages) {
    return !secure_frontend::render_deepseek_text_prompt(messages, 75).has_value();
  };
  expect(refused({{"user", "a" + std::string{kUser} + "b", {}}}), "a turn marker in content fails closed");
  expect(refused({{"user", "</think>", {}}}), "a think tag in content fails closed");
  expect(refused({{"user", "x\xEF\xBD\x9C" "DSML\xEF\xBD\x9Cy", {}}}), "the tool namespace in content fails closed");
  expect(refused({{"user", "one", {}}, {"user", "two", {}}}), "non-alternating roles fail closed");
  expect(refused({{"system", "only", {}}}), "a system message alone fails closed");
  expect(refused({{"user", "Hi", {}}, {"assistant", "Hello", {}}}), "a conversation must end in a user turn");
  expect(refused({{"user", "Hi", {}}, {"system", "late", {}}, {"user", "Again", {}}}), "a late system message fails closed");
  expect(refused({{"developer", "no", {}}, {"user", "yes", {}}}), "unsupported roles fail closed");
  expect(refused({{"user", "", {}}}), "empty content fails closed");
  expect(refused({{"user", "bell\x07", {}}}), "a control byte fails closed");
  expect(!secure_frontend::render_deepseek_text_prompt({{"user", "Hi", {}}}, 0).has_value() &&
             !secure_frontend::render_deepseek_text_prompt({{"user", "Hi", {}}}, 101).has_value(),
         "an effort outside 1-100 fails closed");
}

void plans_with_the_markers_the_split_and_server_stops() {
  const auto pack = deepseek_pack();
  const secure_frontend::ChatCompletionRequest request{"deepseek-local", kTwoTurns, 9U};
  const auto result = secure_frontend::make_deepseek_chat_generation_plan(request, pack);
  const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&result);
  expect(plan != nullptr, "the DeepSeek plan composes on a DeepSeek pack");
  if (plan == nullptr) return;
  const auto& ids = plan->prompt_token_ids;
  expect(!ids.empty() && ids.front() == token_id(pack, kBos) && ids.back() == token_id(pack, "<think>"),
         "the prompt opens with the BOS id and its last id opens the think block (thinking at 75, the default)");
  expect(std::count(ids.begin(), ids.end(), token_id(pack, kUser)) == 2 &&
             std::count(ids.begin(), ids.end(), token_id(pack, kAssistant)) == 2 &&
             std::count(ids.begin(), ids.end(), token_id(pack, kEos)) == 1 &&
             std::count(ids.begin(), ids.end(), token_id(pack, kSystem)) == 1,
         "each marker is one id");
  expect(plan->think_tokens.has_value() && plan->think_tokens->open == token_id(pack, "<think>") &&
             plan->think_tokens->close == token_id(pack, "</think>") && plan->starts_in_reasoning,
         "the reply begins as reasoning, </think> moves it to the answer");
  expect(plan->stop_tokens.empty() &&
             plan->server_stop_tokens == std::vector<std::size_t>({token_id(pack, kEos), token_id(pack, kUser)}),
         "<｜end▁of▁sentence｜> and <｜User｜> are server stops (the engine stops on its EOS only)");

  const auto chat = secure_frontend::make_deepseek_chat_generation_plan(request, pack, 4096U, 12U, {}, std::nullopt);
  const auto* chat_plan = std::get_if<secure_frontend::ChatGenerationPlan>(&chat);
  expect(chat_plan != nullptr && chat_plan->prompt_token_ids.back() == token_id(pack, "</think>") &&
             !chat_plan->starts_in_reasoning,
         "chat mode closes the block: the reply is the answer from its first token");
}

void a_pack_without_deepseeks_markers_plans_nothing() {
  const secure_frontend::ChatCompletionRequest request{"deepseek-local", {{"user", "Hi", {}}}, 9U};
  for (const std::string_view missing : {kUser, kEos, std::string_view{"</think>"}, kBos}) {
    auto pack = deepseek_pack();
    pack.tokens_by_id[token_id(pack, missing)] = "<unused-" + std::to_string(token_id(pack, missing)) + ">";
    const auto result = secure_frontend::make_deepseek_chat_generation_plan(request, pack);
    const auto* failure = std::get_if<secure_frontend::ChatGenerationPlanError>(&result);
    expect(failure != nullptr && failure->code == secure_frontend::ChatGenerationPlanErrorCode::tokenization_failed,
           "a pack without " + std::string{missing} + " plans nothing");
  }
  auto ordinary = deepseek_pack();
  ordinary.token_types[token_id(ordinary, kAssistant)] = 1;
  const auto result = secure_frontend::make_deepseek_chat_generation_plan(request, ordinary);
  expect(std::holds_alternative<secure_frontend::ChatGenerationPlanError>(result), "an ordinary-typed marker plans nothing");
  const secure_frontend::ChatCompletionRequest image{
      "deepseek-local", {{"user", "Hi", {{"Hi", ""}, {"", "data:image/png;base64,iVBORw0KGgo="}}}}, 9U};
  const auto with_image = secure_frontend::make_deepseek_chat_generation_plan(image, deepseek_pack());
  const auto* failure = std::get_if<secure_frontend::ChatGenerationPlanError>(&with_image);
  expect(failure != nullptr && failure->code == secure_frontend::ChatGenerationPlanErrorCode::invalid_request,
         "an image part is invalid_request (text only)");
}

// The levels a request (reasoning_effort) or the card (thinking, reasoning.effort) names, as Maya's server maps them
// for DeepSeek (serve/frontend.py EFFORT and deepseek_effort_kwargs): none is chat mode, low 50, medium and high 75.
void effort_levels_are_mayas() {
  expect(!secure_frontend::deepseek_effort_budget("none").has_value(), "none: chat mode");
  expect(secure_frontend::deepseek_effort_budget("low") == 50, "low: 50");
  expect(secure_frontend::deepseek_effort_budget("medium") == 75, "medium: 75 (Maya maps it to high)");
  expect(secure_frontend::deepseek_effort_budget("high") == 75, "high: 75");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"renders_deepseeks_encoder_byte_for_byte", renders_deepseeks_encoder_byte_for_byte},
      {"refuses_what_the_fixed_profile_cannot_say", refuses_what_the_fixed_profile_cannot_say},
      {"plans_with_the_markers_the_split_and_server_stops", plans_with_the_markers_the_split_and_server_stops},
      {"a_pack_without_deepseeks_markers_plans_nothing", a_pack_without_deepseeks_markers_plans_nothing},
      {"effort_levels_are_mayas", effort_levels_are_mayas},
  }, failures);
}
