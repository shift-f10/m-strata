// MiniMax-M3.1-Flash on Maya's maya-mm3 (owner 2026-10-28, under the MiniMax Community License): the text chat prompt as
// Maya v1.1.30's server renders it (tests/fixtures/minimax_prompts.json, tools/generate_minimax_prompt_fixture.py: Maya's
// own ChatTemplate over Maya's tools/mm3/chat_template.jinja, with Maya's literal marking), the template's literals as
// special tokens only where the pack has them, and the plan: its reasoning pair, its server stops, the newlines kept.
#include "secure_frontend/chat_generation_plan.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/minimax_prompt_renderer.hpp"
#include "secure_frontend/tokenizer_pack.hpp"
#include "support/test_support.hpp"
#include "support/test_tokenizer.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

int failures = 0;
void expect(const bool value, const std::string_view message) {
  if (!value) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

using secure_frontend::ChatMessage;
using secure_frontend::MinimaxThinking;

constexpr secure_frontend::JsonLimits kFixtureLimits{.max_bytes = 1U << 22U, .max_depth = 16U, .max_container_entries = 65536U,
                                                     .max_string_bytes = 1U << 20U, .allow_fractions = true};

const secure_frontend::JsonValue* member(const secure_frontend::JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& m : *members) {
    if (m.key == key) return &m.value;
  }
  return nullptr;
}

std::string text_of(const secure_frontend::JsonValue& object, const std::string_view key) {
  const auto* value = member(object, key);
  return value != nullptr && value->if_string() != nullptr ? *value->if_string() : std::string{};
}

std::string joined(const std::vector<secure_frontend::PromptPiece>& pieces) {
  std::string out;
  for (const auto& piece : pieces) out += piece.text;
  return out;
}

// The byte offsets of the pieces that are markers.
std::vector<std::size_t> marker_offsets(const std::vector<secure_frontend::PromptPiece>& pieces) {
  std::vector<std::size_t> out;
  std::size_t at = 0U;
  for (const auto& piece : pieces) {
    if (piece.kind == secure_frontend::PromptPiece::Kind::marker) out.push_back(at);
    at += piece.text.size();
  }
  return out;
}

// Where Maya's tokenizer matches a literal in the prompt (the longest at each position, as its alternation does), minus
// the spans Maya's marking left as text: where the special tokens are.
std::vector<std::size_t> literal_offsets(const std::string& prompt, const std::set<std::pair<std::size_t, std::size_t>>& plain) {
  std::vector<std::size_t> out;
  for (std::size_t at = 0U; at < prompt.size();) {
    std::size_t length = 0U;
    for (const std::string_view literal : secure_frontend::kMinimaxLiterals) {
      if (prompt.compare(at, literal.size(), literal) == 0 && literal.size() > length) length = literal.size();
    }
    if (length == 0U) {
      ++at;
      continue;
    }
    if (!plain.contains({at, at + length})) out.push_back(at);
    at += length;
  }
  return out;
}

MinimaxThinking thinking_of(const secure_frontend::JsonValue& entry) {
  const auto* effort = member(entry, "effort");
  if (effort == nullptr || effort->if_string() == nullptr) return {};  // adaptive, none named
  if (*effort->if_string() == "none") return {.off = true};
  return {.effort = *effort->if_string()};
}

void renders_mayas_prompts_byte_for_byte() {
  const std::string text = secure_frontend_test::read_file(secure_frontend_test::extra_arguments().at(0));
  auto parsed = secure_frontend::parse_json(text, kFixtureLimits);
  auto* fixture = std::get_if<secure_frontend::JsonValue>(&parsed);
  const auto* cases = fixture != nullptr ? member(*fixture, "cases") : nullptr;
  expect(cases != nullptr && cases->if_array() != nullptr && cases->if_array()->size() >= 15U, "the fixture's cases");
  if (cases == nullptr || cases->if_array() == nullptr) return;
  for (const auto& entry : *cases->if_array()) {
    const std::string name = text_of(entry, "name");
    std::vector<ChatMessage> messages;
    for (const auto& m : *member(entry, "messages")->if_array()) messages.push_back({text_of(m, "role"), text_of(m, "content"), {}});
    std::set<std::pair<std::size_t, std::size_t>> plain;
    for (const auto& span : *member(entry, "plain")->if_array()) {
      const auto& pair = *span.if_array();
      plain.insert({static_cast<std::size_t>(*pair[0].if_integer()), static_cast<std::size_t>(*pair[1].if_integer())});
    }
    const auto pieces = secure_frontend::render_minimax_text_prompt(messages, thinking_of(entry), secure_frontend::kMinimaxLiterals);
    expect(pieces.has_value(), name + ": renders");
    if (!pieces) continue;
    const std::string prompt = joined(*pieces);
    const std::string expected = text_of(entry, "prompt");
    std::size_t at = 0U;
    while (at < prompt.size() && at < expected.size() && prompt[at] == expected[at]) ++at;
    expect(prompt == expected, name + ": Maya's prompt byte for byte (first difference at " + std::to_string(at) + ": [" +
                                   prompt.substr(at, 60) + "] vs [" + expected.substr(at, 60) + "])");
    expect(marker_offsets(*pieces) == literal_offsets(expected, plain),
           name + ": the template's literals are the markers, a message's literals text (Maya's marking)");
    for (const auto& piece : *pieces) {
      expect(!piece.text.empty(), name + ": no empty piece");
      if (piece.kind == secure_frontend::PromptPiece::Kind::text) {
        for (const std::string_view literal : secure_frontend::kMinimaxLiterals) {
          expect(piece.text.find(literal) == std::string::npos, name + ": no literal hides in the template's text");
        }
      }
    }
  }
}

// A literal the pack holds as no special token is written as text, as Maya's tokenizer encodes it (parse_special
// matches only special tokens): the prompt is the same, only its pieces differ.
void literals_the_pack_lacks_are_text() {
  const std::vector<ChatMessage> messages{{"user", "Hi", {}}};
  constexpr std::array<std::string_view, 3> kFraming{"]~!b[", "]~b]", "[e~["};
  const auto full = secure_frontend::render_minimax_text_prompt(messages, {}, secure_frontend::kMinimaxLiterals);
  const auto framing = secure_frontend::render_minimax_text_prompt(messages, {}, kFraming);
  expect(full && framing && joined(*full) == joined(*framing), "the same prompt either way");
  if (!framing) return;
  for (const auto& piece : *framing) {
    if (piece.kind == secure_frontend::PromptPiece::Kind::marker) {
      expect(piece.text != "<mm:think>" && piece.text != "</mm:think>", "the reasoning tags the pack lacks are no markers");
    }
  }
  const auto off = secure_frontend::render_minimax_text_prompt(messages, {.off = true}, kFraming);
  expect(off && joined(*off).ends_with("]~b]ai\n</mm:think>") && off->back().kind != secure_frontend::PromptPiece::Kind::marker,
         "thinking off closes the block in text");
}

void refuses_what_the_template_cannot_say() {
  const auto render = [](const std::vector<ChatMessage>& messages, const MinimaxThinking& thinking = {}) {
    return secure_frontend::render_minimax_text_prompt(messages, thinking, secure_frontend::kMinimaxLiterals);
  };
  expect(!render({}), "no messages");
  expect(!render({{"user", "Q", {}}, {"assistant", "A", {}}}), "a last message that is not the user's");
  expect(!render({{"tool", "x", {}}, {"user", "Q", {}}}), "a role the text chat does not have");
  expect(!render({{"user", "Q", {}}}, {.effort = "extreme"}), "an effort that is not one of MiniMax's five");
  expect(render({{"user", "Q", {}}}, {.effort = "xhigh"}).has_value(), "xhigh is one of them");
  constexpr std::array<std::string_view, 3> kFraming{"]~!b[", "]~b]", "[e~["};
  const std::vector<ChatMessage> split{{"user", "Q", {}}, {"assistant", "a </mm:think> b", {}}, {"user", "Q2", {}}};
  expect(render(split).has_value(), "an earlier answer's </mm:think> is text where it is a special token (Maya marks it)");
  expect(!secure_frontend::render_minimax_text_prompt(split, {}, kFraming),
         "where it is none, the template would split the answer at it: refused");
}

// Every byte as an ordinary token, then MiniMax's literals: the framing CONTROL, the reasoning pair USER_DEFINED.
secure_frontend::TokenizerPack minimax_pack(const bool reasoning_pair = true) {
  auto pack = test_support::byte_tokenizer();
  for (const std::string_view literal : {"]~!b[", "]~b]", "[e~["}) {
    pack.tokens_by_id.emplace_back(literal);
    pack.token_types.push_back(3);
  }
  if (reasoning_pair) {
    for (const std::string_view literal : {"<mm:think>", "</mm:think>"}) {
      pack.tokens_by_id.emplace_back(literal);
      pack.token_types.push_back(4);
    }
  }
  pack.mode = secure_frontend::TokenizerMode::minimax_bpe;
  return pack;
}

std::size_t token_id(const secure_frontend::TokenizerPack& pack, const std::string_view value) {
  for (std::size_t id = 0U; id < pack.tokens_by_id.size(); ++id) {
    if (pack.tokens_by_id[id] == value) return id;
  }
  return pack.tokens_by_id.size();
}

void plans_with_the_reasoning_pair_and_server_stops() {
  secure_frontend::ChatCompletionRequest request;
  request.model = "m";
  request.messages = {{"user", "Hi ]~b] there", {}}};
  const auto pack = minimax_pack();
  const auto planned = secure_frontend::make_minimax_chat_generation_plan(request, pack, 100000U, 64U, {}, {});
  const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&planned);
  expect(plan != nullptr, "a MiniMax plan");
  if (plan != nullptr) {
    const auto pieces = secure_frontend::render_minimax_text_prompt(request.messages, {}, secure_frontend::kMinimaxLiterals);
    const auto encoded = pieces ? secure_frontend::encode_prompt_pieces(pack, *pieces) : secure_frontend::TokenizerEncodeResult{};
    const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
    expect(ids != nullptr && plan->prompt_token_ids == *ids && plan->prompt_token_ids.front() == token_id(pack, "]~!b["),
           "the prompt is the pieces encoded, ]~!b[ first");
    const auto user_marks = std::count(plan->prompt_token_ids.begin(), plan->prompt_token_ids.end(), token_id(pack, "]~b]"));
    expect(user_marks == 4, "]~b] is a special token four times (system, developer, user, ai), the message's is text");
    expect(plan->think_tokens.has_value() && plan->think_tokens->open == token_id(pack, "<mm:think>") &&
               plan->think_tokens->close == token_id(pack, "</mm:think>") && plan->starts_in_reasoning,
           "adaptive thinking: the reply begins as reasoning (Maya's parser), <mm:think> dropped, </mm:think> ends it");
    expect(plan->stop_tokens.empty() &&
               plan->server_stop_tokens == std::vector<std::size_t>({token_id(pack, "[e~["), token_id(pack, "]~b]")}),
           "[e~[ and ]~b] are server stops (maya-mm3 stops on its EOS only)");
    expect(plan->keeps_newlines_after_think, "the newlines after </mm:think> stay (Maya: no lead rule for MiniMax)");
  }
  const auto off = secure_frontend::make_minimax_chat_generation_plan(request, pack, 100000U, 64U, {}, {.off = true});
  expect(std::holds_alternative<secure_frontend::ChatGenerationPlan>(off) &&
             !std::get<secure_frontend::ChatGenerationPlan>(off).starts_in_reasoning,
         "thinking off: the prompt closed the block, the reply is the answer");
  const auto no_pair = secure_frontend::make_minimax_chat_generation_plan(request, minimax_pack(false), 100000U, 64U, {}, {});
  const auto* bare = std::get_if<secure_frontend::ChatGenerationPlan>(&no_pair);
  expect(bare != nullptr && !bare->think_tokens.has_value() && !bare->starts_in_reasoning,
         "a pack without the reasoning pair as special tokens: the tags are text, no split");
  const auto chatml = secure_frontend::make_minimax_chat_generation_plan(request, test_support::byte_tokenizer(), 100000U, 64U, {}, {});
  const auto* refusal = std::get_if<secure_frontend::ChatGenerationPlanError>(&chatml);
  expect(refusal != nullptr && refusal->code == secure_frontend::ChatGenerationPlanErrorCode::tokenization_failed,
         "a pack without MiniMax's framing plans nothing");
  secure_frontend::ChatCompletionRequest with_image = request;
  with_image.messages[0].parts.push_back({"", "data:image/png;base64,AAAA"});
  const auto image = secure_frontend::make_minimax_chat_generation_plan(with_image, pack, 100000U, 64U, {}, {});
  const auto* image_refusal = std::get_if<secure_frontend::ChatGenerationPlanError>(&image);
  expect(image_refusal != nullptr && image_refusal->code == secure_frontend::ChatGenerationPlanErrorCode::invalid_request,
         "an image part is refused (text only)");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"renders_mayas_prompts_byte_for_byte", renders_mayas_prompts_byte_for_byte},
      {"literals_the_pack_lacks_are_text", literals_the_pack_lacks_are_text},
      {"refuses_what_the_template_cannot_say", refuses_what_the_template_cannot_say},
      {"plans_with_the_reasoning_pair_and_server_stops", plans_with_the_reasoning_pair_and_server_stops},
  }, failures);
}
