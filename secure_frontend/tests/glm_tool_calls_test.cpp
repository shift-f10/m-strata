// Web search, phase 5 (docs/superpowers/specs/2026-10-27-web-search-design.md §4): GLM-5.3's tool calls in Maya's form -
// the template's tool sections (render_glm53_tool_prompt against Maya's own ChatTemplate, tests/fixtures/
// glm_tool_prompts.json), the tags, the plan, and the reply's calls (GlmToolCallParser against Maya's OutputParser,
// tests/fixtures/glm_tool_calls.json).
#include "secure_frontend/chat_generation_plan.hpp"
#include "secure_frontend/chat_route.hpp"
#include "secure_frontend/glm_prompt_renderer.hpp"
#include "secure_frontend/glm_tool_calls.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/tokenizer_pack.hpp"
#include "support/test_support.hpp"
#include "support/test_tokenizer.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

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

std::optional<secure_frontend::JsonValue> load_fixture() {
  const std::string text = secure_frontend_test::read_file(secure_frontend_test::extra_arguments().at(0));
  auto parsed = secure_frontend::parse_json(text, kFixtureLimits);
  auto* root = std::get_if<secure_frontend::JsonValue>(&parsed);
  if (root == nullptr) return std::nullopt;
  return std::move(*root);
}

secure_frontend::ToolCall call_of(const secure_frontend::JsonValue& object) {
  secure_frontend::ToolCall call{text_of(object, "name"), {}};
  if (const auto* arguments = member(object, "arguments"); arguments != nullptr && arguments->if_object() != nullptr) {
    for (const auto& m : *arguments->if_object()) call.arguments.push_back({m.key, m.value});
  }
  return call;
}

std::string arguments_text(const secure_frontend::ToolCall& call) {
  std::vector<secure_frontend::JsonMember> members;
  for (const auto& argument : call.arguments) members.push_back({argument.name, argument.value});
  return secure_frontend::python_json_text(secure_frontend::JsonValue::make_object(std::move(members)));
}

std::string joined(const std::vector<secure_frontend::PromptPiece>& pieces) {
  std::string out;
  for (const auto& piece : pieces) out += piece.text;
  return out;
}

std::size_t markers_named(const std::vector<secure_frontend::PromptPiece>& pieces, const std::string_view literal) {
  return static_cast<std::size_t>(std::count_if(pieces.begin(), pieces.end(), [literal](const secure_frontend::PromptPiece& p) {
    return p.kind == secure_frontend::PromptPiece::Kind::marker && p.text == literal;
  }));
}

void renders_mayas_glm_tool_prompts() {
  const auto fixture = load_fixture();
  expect(fixture.has_value(), "the prompt fixture parses");
  if (!fixture) return;
  const auto* tool = member(*fixture, "tool");
  const auto* cases = member(*fixture, "cases");
  expect(tool != nullptr && cases != nullptr && cases->if_array() != nullptr && cases->if_array()->size() >= 8U, "the fixture's cases");
  if (tool == nullptr || cases == nullptr || cases->if_array() == nullptr) return;
  const std::vector<std::string> tools{secure_frontend::python_json_text(*tool)};
  static const std::vector<std::string_view> kMarkers{"[gMASK]", "<sop>", "<|system|>", "<|user|>", "<|assistant|>", "<|observation|>",
                                                      "<think>", "</think>", "<tool_call>", "</tool_call>", "<arg_key>", "</arg_key>",
                                                      "<arg_value>", "</arg_value>", "<tool_response>", "</tool_response>"};
  for (const auto& entry : *cases->if_array()) {
    const std::string name = text_of(entry, "name");
    std::vector<secure_frontend::ChatMessage> conversation;
    for (const auto& m : *member(entry, "conversation")->if_array()) conversation.push_back({text_of(m, "role"), text_of(m, "content")});
    std::vector<secure_frontend::ToolRound> rounds;
    std::size_t calls = 0U;
    for (const auto& r : *member(entry, "rounds")->if_array()) {
      secure_frontend::ToolRound round{text_of(r, "reasoning"), text_of(r, "content"), {}, {}};
      for (const auto& c : *member(r, "calls")->if_array()) round.calls.push_back(call_of(c));
      for (const auto& t : *member(r, "results")->if_array()) round.results.push_back(t.if_string() != nullptr ? *t.if_string() : std::string{});
      calls += round.calls.size();
      rounds.push_back(std::move(round));
    }
    const std::string expected = text_of(entry, "prompt");
    const auto pieces = secure_frontend::render_glm53_tool_prompt(conversation, tools, rounds);
    expect(pieces.has_value(), name + ": renders");
    if (!pieces) continue;
    const std::string prompt = joined(*pieces);
    std::size_t at = 0U;
    while (at < prompt.size() && at < expected.size() && prompt[at] == expected[at]) ++at;
    expect(prompt == expected, name + ": Maya's prompt byte for byte (first difference at " + std::to_string(at) + ": [" +
                                   prompt.substr(at, 60) + "] vs [" + expected.substr(at, 60) + "])");
    for (const auto& piece : *pieces) {
      expect(!piece.text.empty(), name + ": no empty piece");
      if (piece.kind == secure_frontend::PromptPiece::Kind::marker) {
        expect(std::find(kMarkers.begin(), kMarkers.end(), piece.text) != kMarkers.end(), name + ": a marker is a special token");
      }
    }
    // The instructions write each call tag once; each call once more; a round's results follow one <|observation|>.
    expect(markers_named(*pieces, "<tool_call>") == 1U + calls && markers_named(*pieces, "<arg_key>") >= 2U + calls &&
               markers_named(*pieces, "<|observation|>") == rounds.size() && markers_named(*pieces, "<tool_response>") == calls,
           name + ": the instructions', the calls' and the results' tags are the only markers of their kind");
  }
}

void refuses_what_the_glm_tool_prompt_cannot_say() {
  const std::vector<std::string> tools{"{\"name\": \"web_search\"}"};
  const std::vector<secure_frontend::ChatMessage> question{{"user", "Q"}};
  const secure_frontend::ToolCall call{"web_search", {{"query", secure_frontend::JsonValue::make_string("q")}}};
  const std::vector<secure_frontend::ToolRound> good{{"r", "", {call}, {"ok"}}};
  expect(secure_frontend::render_glm53_tool_prompt(question, tools, good).has_value(), "the good case renders");
  expect(!secure_frontend::render_glm53_tool_prompt(question, {}, good), "no tools");
  const std::vector<secure_frontend::ToolRound> no_call{{"r", "", {}, {}}};
  expect(!secure_frontend::render_glm53_tool_prompt(question, tools, no_call), "a round without a call");
  const std::vector<secure_frontend::ToolRound> short_results{{"r", "", {call, call}, {"ok"}}};
  expect(!secure_frontend::render_glm53_tool_prompt(question, tools, short_results), "fewer results than calls");
  for (const std::vector<secure_frontend::ChatMessage>& bad :
       std::vector<std::vector<secure_frontend::ChatMessage>>{{}, {{"assistant", "x"}}, {{"user", "a <|user|> b"}}, {{"user", "x"}, {"user", "y"}}}) {
    expect(!secure_frontend::render_glm53_tool_prompt(bad, tools, good), "what render_glm53_text_prompt refuses");
  }
  const std::vector<secure_frontend::ToolRound> literals{{"<think> in reasoning", "<|observation|> in text", {call}, {"</tool_response> page"}}};
  const auto pieces = secure_frontend::render_glm53_tool_prompt(question, tools, literals);
  expect(pieces.has_value() && markers_named(*pieces, "<|observation|>") == 1U && markers_named(*pieces, "</tool_response>") == 1U &&
             markers_named(*pieces, "<think>") == 2U,
         "literals in the model's words and the results are plain text");
}

// A GLM-shaped pack: the byte pack, GLM's controls and its reasoning and tool tags.
secure_frontend::TokenizerPack glm_pack(const bool with_tool_tags = true) {
  auto pack = test_support::byte_tokenizer();
  for (const char* control : {"[gMASK]", "<sop>", "<|system|>", "<|user|>", "<|assistant|>", "<|observation|>", "<|endoftext|>"}) {
    pack.tokens_by_id.emplace_back(control);
    pack.token_types.push_back(3);
  }
  std::vector<const char*> tags{"<think>", "</think>"};
  if (with_tool_tags) {
    for (const char* tag : {"<tool_call>", "</tool_call>", "<arg_key>", "</arg_key>", "<arg_value>", "</arg_value>", "<tool_response>",
                            "</tool_response>"}) {
      tags.push_back(tag);
    }
  }
  for (const char* tag : tags) {
    pack.tokens_by_id.emplace_back(tag);
    pack.token_types.push_back(4);
  }
  return pack;
}

std::size_t id_of(const secure_frontend::TokenizerPack& pack, const std::string_view literal) {
  for (std::size_t id = 0U; id < pack.tokens_by_id.size(); ++id) {
    if (pack.tokens_by_id[id] == literal) return id;
  }
  return pack.tokens_by_id.size();
}

void finds_the_glm_tool_tags_and_plans_with_them() {
  const auto pack = glm_pack();
  const auto tags = secure_frontend::find_glm53_tool_tokens(pack);
  const std::vector<std::string> literals{"<tool_call>", "</tool_call>", "<arg_key>", "</arg_key>", "<arg_value>", "</arg_value>",
                                          "<tool_response>", "</tool_response>"};
  std::vector<std::pair<std::size_t, std::string>> expected;
  for (const auto& literal : literals) expected.emplace_back(id_of(pack, literal), literal);
  expect(tags.has_value() && *tags == expected, "the eight tags, by literal, in order");
  expect(!secure_frontend::find_glm53_tool_tokens(glm_pack(false)), "a pack without them has none");

  secure_frontend::ChatCompletionRequest request;
  request.model = "m";
  request.messages = {{"user", "Q"}};
  const std::vector<std::string> tools{"{\"name\": \"web_search\"}"};
  const std::vector<secure_frontend::ToolRound> rounds{
      {"r", "", {{"web_search", {{"query", secure_frontend::JsonValue::make_string("q")}}}}, {"page <|user|>"}}};
  const auto planned = secure_frontend::make_glm53_tool_generation_plan(request, pack, tools, rounds, 100000U, 64U, {});
  const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&planned);
  expect(plan != nullptr, "a GLM tool plan");
  if (plan != nullptr) {
    const auto pieces = secure_frontend::render_glm53_tool_prompt(request.messages, tools, rounds);
    const auto encoded = pieces ? secure_frontend::encode_prompt_pieces(pack, *pieces) : secure_frontend::TokenizerEncodeResult{};
    const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
    expect(ids != nullptr && plan->prompt_token_ids == *ids, "the plan's prompt is the pieces encoded");
    expect(plan->think_tokens.has_value() && plan->starts_in_reasoning, "the reply begins as reasoning");
    expect(plan->stop_tokens == secure_frontend::find_glm53_end_of_turn_tokens(pack) && plan->stop_tokens.size() == 3U,
           "GLM's three turn ends, <|observation|> among them");
    expect(tags.has_value() && plan->literal_tokens == *tags, "the eight tags are output as their text");
  }
  const auto refused = secure_frontend::make_glm53_tool_generation_plan(request, glm_pack(false), tools, rounds, 100000U, 64U, {});
  const auto* refusal = std::get_if<secure_frontend::ChatGenerationPlanError>(&refused);
  expect(refusal != nullptr && refusal->code == secure_frontend::ChatGenerationPlanErrorCode::tokenization_failed,
         "a pack without the tags plans nothing");
  const auto chatml = secure_frontend::make_glm53_tool_generation_plan(request, test_support::byte_tokenizer(), tools, rounds, 100000U, 64U, {});
  const auto* chatml_refusal = std::get_if<secure_frontend::ChatGenerationPlanError>(&chatml);
  expect(chatml_refusal != nullptr && chatml_refusal->code == secure_frontend::ChatGenerationPlanErrorCode::tokenization_failed,
         "a ChatML pack on the GLM engine is served without search");
}

std::vector<secure_frontend::ToolDeclaration> declarations(const secure_frontend::JsonValue& tool) {
  secure_frontend::ToolDeclaration declaration{text_of(tool, "name"), {}};
  const auto* parameters = member(tool, "parameters");
  const auto* properties = parameters != nullptr ? member(*parameters, "properties") : nullptr;
  if (properties != nullptr && properties->if_object() != nullptr) {
    for (const auto& p : *properties->if_object()) {
      if (text_of(p.value, "type") == "string") declaration.string_parameters.push_back(p.key);
    }
  }
  return {declaration};
}

std::string describe(const std::vector<secure_frontend::ToolParseEvent>& events) {
  std::vector<std::pair<std::string, std::string>> merged;
  for (const auto& e : events) {
    if (e.kind == secure_frontend::ToolParseEvent::Kind::call) {
      merged.emplace_back("call", e.call.name + " " + arguments_text(e.call));
      continue;
    }
    const std::string kind = e.kind == secure_frontend::ToolParseEvent::Kind::reasoning ? "reasoning" : "content";
    if (!merged.empty() && merged.back().first == kind) merged.back().second += e.text;
    else merged.emplace_back(kind, e.text);
  }
  std::string out;
  for (const auto& [kind, text] : merged) out += "<" + kind + ":" + text + ">";
  return out;
}

std::string describe_expected(const secure_frontend::JsonValue& events) {
  std::string out;
  for (const auto& e : *events.if_array()) {
    if (const auto* call = member(e, "call"); call != nullptr) out += "<call:" + text_of(*call, "name") + " " + arguments_text(call_of(*call)) + ">";
    else out += "<" + text_of(e, "kind") + ":" + text_of(e, "text") + ">";
  }
  return out;
}

void parses_mayas_glm_tool_calls() {
  const auto fixture = load_fixture();
  expect(fixture.has_value(), "the call fixture parses");
  if (!fixture) return;
  const auto* tool = member(*fixture, "tool");
  const auto* cases = member(*fixture, "cases");
  expect(tool != nullptr && cases != nullptr && cases->if_array() != nullptr && cases->if_array()->size() >= 15U, "the fixture's cases");
  if (tool == nullptr || cases == nullptr || cases->if_array() == nullptr) return;
  const auto tools = declarations(*tool);
  for (const auto& entry : *cases->if_array()) {
    const std::string name = text_of(entry, "name");
    const std::string reply = text_of(entry, "reply");
    const std::string expected = describe_expected(*member(entry, "events"));
    for (const std::size_t step : {std::size_t{1U << 20U}, std::size_t{1U}, std::size_t{4U}}) {
      secure_frontend::GlmToolCallParser parser(tools);
      std::vector<secure_frontend::ToolParseEvent> events;
      const auto append = [&events](std::vector<secure_frontend::ToolParseEvent> more) {
        for (auto& e : more) events.push_back(std::move(e));
      };
      const auto feed = [&](const std::string_view text, const bool reasoning) {
        for (std::size_t at = 0U; at < text.size();) {
          std::size_t end = at;
          for (std::size_t n = 0U; n < step && end < text.size(); ++n) {
            ++end;
            while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) ++end;
          }
          append(reasoning ? parser.feed_reasoning(text.substr(at, end - at)) : parser.feed_content(text.substr(at, end - at)));
          at = end;
        }
      };
      const std::size_t close = reply.find("</think>");
      feed(std::string_view{reply}.substr(0U, close), true);
      if (close != std::string::npos) {
        append(parser.end_reasoning());
        feed(std::string_view{reply}.substr(close + 8U), false);
      }
      append(parser.finish(true));
      const std::string actual = describe(events);
      expect(actual == expected, name + " (fed " + std::to_string(step) + " at a time):\n  got  " + actual + "\n  Maya " + expected);
    }
  }
}

void parse_glm_tool_call_follows_maya_and_our_rules() {
  const secure_frontend::ToolDeclaration tool{"web_search", {"query"}};
  const auto parsed = secure_frontend::parse_glm_tool_call(
      " web_search \n<arg_key> query </arg_key>\n<arg_value> two words </arg_value><arg_key>max_results</arg_key><arg_value>4</arg_value>", &tool);
  expect(parsed.has_value() && parsed->name == "web_search" && arguments_text(*parsed) == R"({"query": " two words ", "max_results": 4})",
         "the name and keys stripped, a string value as written, the others decoded");
  const auto bare = secure_frontend::parse_glm_tool_call("web_search", &tool);
  expect(bare.has_value() && bare->arguments.empty(), "a call without arguments");
  const auto qwen = secure_frontend::parse_glm_tool_call("<function=web_search>\n<parameter=query>\nq\n</parameter>\n</function>", &tool);
  expect(qwen.has_value() && qwen->name == "web_search" && arguments_text(*qwen) == R"({"query": "q"})", "Qwen's form, as Maya reads it");
  for (const std::string_view bad : {"", "<arg_key>a</arg_key><arg_value>1</arg_value>", "f<arg_key>a<arg_value>1</arg_value>",
                                     "f<arg_key>a</arg_key>x<arg_value>1</arg_value>", "f<arg_key>a</arg_key>", "f text<arg_key>a</arg_key>",
                                     "<function=web_search"}) {
    expect(!secure_frontend::parse_glm_tool_call(bad, &tool), "malformed: [" + std::string{bad} + "]");
  }
  // ours: a malformed body and a tool the server did not declare are text
  secure_frontend::GlmToolCallParser parser({tool});
  std::vector<secure_frontend::ToolParseEvent> events;
  for (auto& e : parser.feed_content("A <tool_call>python<arg_key>code</arg_key><arg_value>1</arg_value></tool_call> B "
                                     "<tool_call>web_search<arg_key>q</arg_key>x</tool_call> C")) {
    events.push_back(std::move(e));
  }
  for (auto& e : parser.finish(true)) events.push_back(std::move(e));
  expect(describe(events) == "<content:A <tool_call>python<arg_key>code</arg_key><arg_value>1</arg_value></tool_call> B <tool_call>"
                             "web_search<arg_key>q</arg_key>x</tool_call> C>",
         "an undeclared tool's call and a malformed one are the text they were: " + describe(events));
}

// The chat route's first search plan takes the turn's family form: GLM's for a GLM-5.3 chat, Qwen's otherwise.
void the_route_plans_a_search_in_the_turns_form() {
  secure_frontend::ChatRouteConfig config{};
  config.max_prompt_tokens = 100000U;
  secure_frontend::ChatCompletionRequest request;
  request.model = "m";
  request.messages = {{"user", "Q"}};
  secure_frontend::SearchTurn turn{request, {{"web_search", {"query"}}, "{\"name\": \"web_search\"}"}, true, {}, secure_frontend::ToolFormat::glm};
  const auto pack = glm_pack();
  const auto planned = secure_frontend::search_first_plan(config, turn, pack);
  const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&planned);
  const auto tags = secure_frontend::find_glm53_tool_tokens(pack);
  expect(plan != nullptr && tags.has_value() && plan->literal_tokens == *tags && !plan->prompt_token_ids.empty() &&
             plan->prompt_token_ids.front() == id_of(pack, "[gMASK]"),
         "a GLM turn plans in GLM's form: its eight tags, [gMASK] first");
  turn.format = secure_frontend::ToolFormat::qwen;  // the test pack carries ChatML and Qwen's four tags as well
  const auto as_qwen = secure_frontend::search_first_plan(config, turn, pack);
  const auto* qwen = std::get_if<secure_frontend::ChatGenerationPlan>(&as_qwen);
  expect(qwen != nullptr && qwen->literal_tokens.size() == 4U && !qwen->prompt_token_ids.empty() &&
             qwen->prompt_token_ids.front() == id_of(pack, "<|im_start|>"),
         "a Qwen turn plans in Qwen's form: its four tags, <|im_start|> first");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"renders_mayas_glm_tool_prompts", renders_mayas_glm_tool_prompts},
      {"refuses_what_the_glm_tool_prompt_cannot_say", refuses_what_the_glm_tool_prompt_cannot_say},
      {"finds_the_glm_tool_tags_and_plans_with_them", finds_the_glm_tool_tags_and_plans_with_them},
      {"parses_mayas_glm_tool_calls", parses_mayas_glm_tool_calls},
      {"parse_glm_tool_call_follows_maya_and_our_rules", parse_glm_tool_call_follows_maya_and_our_rules},
      {"the_route_plans_a_search_in_the_turns_form", the_route_plans_a_search_in_the_turns_form},
  }, failures);
}
