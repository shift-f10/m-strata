// Web search for DeepSeek-V4.1 (owner 2026-10-28: "deepseek does not seem to have mcp search access"): the tool prompt as
// Project Maya v1.1.30 renders it - DeepSeek's own encoder (serve/ds41_encoding.py) under Maya's ChatTemplate, with
// Maya's literal marking (render_deepseek_tool_prompt against tests/fixtures/deepseek_tool_prompts.json,
// tools/generate_deepseek_tool_prompt_fixture.py) - the DSML calls as Maya reads them (its DsmlRewriter into GLM's form,
// tests/fixtures/deepseek_tool_calls.json, tools/generate_deepseek_tool_call_fixture.py), the plan with ｜DSML｜ output as
// its text, and the route's first search plan.
#include "secure_frontend/chat_generation_plan.hpp"
#include "secure_frontend/chat_route.hpp"
#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/deepseek_prompt_renderer.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/model_options.hpp"
#include "secure_frontend/tokenizer_pack.hpp"
#include "secure_frontend/tool_calls.hpp"
#include "support/test_support.hpp"
#include "support/test_tokenizer.hpp"

#include <algorithm>
#include <filesystem>
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

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

constexpr secure_frontend::JsonLimits kFixtureLimits{.max_bytes = 1U << 22U, .max_depth = 16U, .max_container_entries = 65536U,
                                                     .max_string_bytes = 1U << 20U, .allow_fractions = true};
constexpr std::string_view kDsml = "\xEF\xBD\x9C" "DSML\xEF\xBD\x9C";

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

secure_frontend::ToolCall call_of(const secure_frontend::JsonValue& object) {
  secure_frontend::ToolCall call{text_of(object, "name"), {}};
  if (const auto* arguments = member(object, "arguments"); arguments != nullptr && arguments->if_object() != nullptr) {
    for (const auto& m : *arguments->if_object()) call.arguments.push_back({m.key, m.value});
  }
  return call;
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

// Where Maya's tokenizer matches a literal in the prompt (the longest at each position), minus the spans Maya's marking
// left as text: where the special tokens are.
std::vector<std::size_t> literal_offsets(const std::string& prompt, const std::vector<std::string>& literals,
                                         const std::set<std::pair<std::size_t, std::size_t>>& plain) {
  std::vector<std::size_t> out;
  for (std::size_t at = 0U; at < prompt.size();) {
    std::size_t length = 0U;
    for (const std::string& literal : literals) {
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

// The client's level as our profile turns it into DeepSeek's effort (deepseek_effort_budget): none is chat mode.
std::optional<int> effort_of(const std::string& level) { return secure_frontend::deepseek_effort_budget(level); }

const std::string kTool =
    "{\"name\": \"web_search\", \"description\": \"Search the web.\", \"parameters\": {\"type\": \"object\", \"properties\": "
    "{\"query\": {\"type\": \"string\", \"description\": \"What to search for\"}}, \"required\": [\"query\"]}}";

void renders_mayas_tool_prompts_byte_for_byte() {
  const std::string text = secure_frontend_test::read_file(secure_frontend_test::extra_arguments().at(0));
  auto parsed = secure_frontend::parse_json(text, kFixtureLimits);
  auto* fixture = std::get_if<secure_frontend::JsonValue>(&parsed);
  expect(fixture != nullptr, "the prompt fixture parses");
  if (fixture == nullptr) return;
  const auto* tool = member(*fixture, "tool");
  const auto* cases = member(*fixture, "cases");
  const auto* literal_list = member(*fixture, "literals");
  expect(tool != nullptr && literal_list != nullptr && cases != nullptr && cases->if_array() != nullptr && cases->if_array()->size() >= 11U,
         "the fixture's cases");
  if (tool == nullptr || literal_list == nullptr || cases == nullptr || cases->if_array() == nullptr) return;
  std::vector<std::string> literals;
  for (const auto& l : *literal_list->if_array()) literals.push_back(*l.if_string());
  // the search service's form ({"name", "description", "parameters"}) and the OpenAI form render alike
  const std::vector<std::vector<std::string>> forms{{secure_frontend::python_json_text(*tool)},
                                                    {"{\"type\": \"function\", \"function\": " + secure_frontend::python_json_text(*tool) + "}"}};
  std::size_t seen = 0U;
  for (const auto& entry : *cases->if_array()) {
    const std::string name = text_of(entry, "name");
    std::vector<secure_frontend::ChatMessage> conversation;
    for (const auto& m : *member(entry, "conversation")->if_array()) conversation.push_back({text_of(m, "role"), text_of(m, "content")});
    std::vector<secure_frontend::ToolRound> rounds;
    for (const auto& r : *member(entry, "rounds")->if_array()) {
      secure_frontend::ToolRound round{text_of(r, "reasoning"), text_of(r, "content"), {}, {}};
      for (const auto& c : *member(r, "calls")->if_array()) round.calls.push_back(call_of(c));
      for (const auto& t : *member(r, "results")->if_array()) round.results.push_back(t.if_string() != nullptr ? *t.if_string() : std::string{});
      rounds.push_back(std::move(round));
    }
    std::set<std::pair<std::size_t, std::size_t>> plain;
    for (const auto& span : *member(entry, "plain")->if_array()) {
      const auto& pair = *span.if_array();
      plain.insert({static_cast<std::size_t>(*pair[0].if_integer()), static_cast<std::size_t>(*pair[1].if_integer())});
    }
    for (const auto& tools : forms) {
      const auto pieces = secure_frontend::render_deepseek_tool_prompt(conversation, tools, rounds, effort_of(text_of(entry, "effort")));
      expect(pieces.has_value(), name + ": renders");
      if (!pieces) continue;
      ++seen;
      const std::string prompt = joined(*pieces);
      const std::string expected = text_of(entry, "prompt");
      std::size_t at = 0U;
      while (at < prompt.size() && at < expected.size() && prompt[at] == expected[at]) ++at;
      expect(prompt == expected, name + ": Maya's prompt byte for byte (first difference at " + std::to_string(at) + ": [" +
                                     prompt.substr(at, 60) + "] vs [" + expected.substr(at, 60) + "])");
      expect(marker_offsets(*pieces) == literal_offsets(expected, literals, plain),
             name + ": the special tokens where Maya's tokenizer puts them (literals in messages, results and calls are text)");
      for (const auto& piece : *pieces) {
        expect(!piece.text.empty(), name + ": no empty piece");
        if (piece.kind == secure_frontend::PromptPiece::Kind::marker) {
          expect(std::find(literals.begin(), literals.end(), piece.text) != literals.end(), name + ": a marker is a literal: " + piece.text);
        }
      }
    }
  }
  expect(seen >= 22U, "every case ran in both tool forms");
}

void refuses_what_the_tool_prompt_cannot_say() {
  const std::vector<std::string> tools{kTool};
  const std::vector<secure_frontend::ChatMessage> question{{"user", "Q"}};
  const secure_frontend::ToolCall call{"web_search", {{"query", secure_frontend::JsonValue::make_string("q")}}};
  const std::vector<secure_frontend::ToolRound> good{{"r", "", {call}, {"ok"}}};
  const auto render = [](const std::vector<secure_frontend::ChatMessage>& m, const std::vector<std::string>& t,
                         const std::vector<secure_frontend::ToolRound>& r) {
    return secure_frontend::render_deepseek_tool_prompt(m, t, r, 75);
  };
  expect(render(question, tools, good).has_value(), "the good case renders");
  expect(!render(question, {}, good), "no tools");
  expect(!render(question, tools, {{"r", "", {}, {}}}), "a round without a call");
  expect(!render(question, tools, {{"r", "", {call, call}, {"ok"}}}), "fewer results than calls");
  expect(!render(question, tools, {{"r", "", {{"", {}}}, {"ok"}}}), "a call without a name");
  expect(!secure_frontend::render_deepseek_tool_prompt(question, tools, good, 0), "an effort outside 1-100");
  for (const std::string& bad_tool : {std::string{"[]"}, std::string{"not json"}, std::string{"{\"description\": \"x\"}"},
                                      std::string{"{\"function\": {\"name\": 5}}"}}) {
    expect(!render(question, {bad_tool}, good), "a tool that is not an object with a name: " + bad_tool);
  }
  // what the plain DeepSeek prompt refuses (render_deepseek_text_prompt): a client's message cannot write a turn
  for (const std::vector<secure_frontend::ChatMessage>& bad : std::vector<std::vector<secure_frontend::ChatMessage>>{
           {}, {{"assistant", "x"}}, {{"user", "a <\xEF\xBD\x9C" "User\xEF\xBD\x9C> b"}}, {{"user", std::string{"a "} + std::string{kDsml}}},
           {{"user", ""}}, {{"user", "a"}, {"system", "late"}, {"user", "b"}}, {{"user", "a"}, {"user", "b"}}}) {
    expect(!render(bad, tools, good), "what the plain DeepSeek prompt refuses");
  }
}

// DeepSeek's calls as Maya reads them: each reply of the fixture (split at </think>), fed whole and one byte at a time,
// gives Maya's events.
std::string events_of(const std::vector<secure_frontend::ToolParseEvent>& events) {
  std::string out;
  for (const auto& e : events) {
    if (e.kind == secure_frontend::ToolParseEvent::Kind::call) {
      out += "<call " + e.call.name + " {";
      for (std::size_t i = 0; i < e.call.arguments.size(); ++i) {
        out += (i == 0U ? "" : ", ") + e.call.arguments[i].name + ": " + secure_frontend::python_json_text(e.call.arguments[i].value);
      }
      out += "}>";
    } else {
      out += (e.kind == secure_frontend::ToolParseEvent::Kind::reasoning ? "[r:" : "[c:") + e.text + "]";
    }
  }
  return out;
}

std::string merged(std::vector<secure_frontend::ToolParseEvent> events) {
  std::vector<secure_frontend::ToolParseEvent> out;
  for (auto& e : events) {
    if (e.kind != secure_frontend::ToolParseEvent::Kind::call && !out.empty() && out.back().kind == e.kind) out.back().text += e.text;
    else out.push_back(std::move(e));
  }
  return events_of(out);
}

std::string parse(const std::string_view reasoning, const std::string_view content, const std::size_t step) {
  // the web_search declaration types query "string"; DeepSeek's calls carry their own string flag (Maya reads them with no schema)
  auto parser = secure_frontend::make_tool_call_parser(secure_frontend::ToolFormat::deepseek, {{"web_search", {"query"}}});
  std::vector<secure_frontend::ToolParseEvent> events;
  const auto add = [&events](std::vector<secure_frontend::ToolParseEvent> more) {
    for (auto& e : more) events.push_back(std::move(e));
  };
  for (std::size_t i = 0; i < reasoning.size(); i += step) add(parser->feed_reasoning(reasoning.substr(i, step)));
  add(parser->end_reasoning());
  for (std::size_t i = 0; i < content.size(); i += step) add(parser->feed_content(content.substr(i, step)));
  add(parser->finish(true));
  return merged(std::move(events));
}

std::string expected_events(const secure_frontend::JsonValue& events) {
  std::string out;
  for (const auto& e : *events.if_array()) {
    if (const auto* c = member(e, "call")) {
      out += "<call " + text_of(*c, "name") + " {";
      const auto* arguments = member(*c, "arguments");
      for (std::size_t i = 0; arguments != nullptr && i < arguments->if_object()->size(); ++i) {
        const auto& a = (*arguments->if_object())[i];
        out += (i == 0U ? "" : ", ") + a.key + ": " + secure_frontend::python_json_text(a.value);
      }
      out += "}>";
    } else {
      out += (text_of(e, "kind") == "reasoning" ? "[r:" : "[c:") + text_of(e, "text") + "]";
    }
  }
  return out;
}

void reads_calls_as_maya_does() {
  const std::string text = secure_frontend_test::read_file(secure_frontend_test::extra_arguments().at(0));
  auto parsed = secure_frontend::parse_json(text, kFixtureLimits);
  auto* fixture = std::get_if<secure_frontend::JsonValue>(&parsed);
  const auto* cases = fixture != nullptr ? member(*fixture, "cases") : nullptr;
  expect(cases != nullptr && cases->if_array() != nullptr && cases->if_array()->size() >= 13U, "the call fixture's cases");
  if (cases == nullptr || cases->if_array() == nullptr) return;
  for (const auto& entry : *cases->if_array()) {
    const std::string name = text_of(entry, "name");
    const std::string reply = text_of(entry, "reply");
    const std::size_t end = reply.find("</think>");
    const std::string reasoning = end == std::string::npos ? std::string{} : reply.substr(0, end);
    const std::string content = end == std::string::npos ? reply : reply.substr(end + 8U);
    const std::string whole = parse(reasoning, content, std::max<std::size_t>({1U, reasoning.size(), content.size()}));
    expect(whole == expected_events(*member(entry, "events")), name + ": " + whole + " vs " + expected_events(*member(entry, "events")));
    expect(parse(reasoning, content, 1U) == whole, name + ": one byte at a time gives the same events");
  }
  // ours (the GLM parser's rule): a call for a tool the server did not declare is text, never a call
  const std::string other = parse("x", std::string{"\n\n<"} + std::string{kDsml} + " calls>\n<" + std::string{kDsml} +
                                           " invoke name=\"read_file\">\n<" + std::string{kDsml} +
                                           " parameter name=\"path\" string=\"true\">/etc</" + std::string{kDsml} + " parameter>\n</" +
                                           std::string{kDsml} + " invoke>\n</" + std::string{kDsml} + " calls>",
                                  1000U);
  expect(other.find("<call") == std::string::npos && other.find("read_file") != std::string::npos, "an undeclared tool is text: " + other);
}

std::size_t id_of(const secure_frontend::TokenizerPack& pack, const std::string_view literal) {
  return test_support::token_id(pack, literal);
}

// DeepSeek's markers and ｜DSML｜ (a user-defined token, as in the maya-ds4 pack) on the byte tokenizer.
secure_frontend::TokenizerPack deepseek_pack(const bool with_dsml = true) {
  auto pack = test_support::byte_tokenizer();
  for (const std::string_view marker : {secure_frontend::kDeepseekMarkers[0], secure_frontend::kDeepseekMarkers[1],
                                        secure_frontend::kDeepseekMarkers[2], secure_frontend::kDeepseekMarkers[3],
                                        secure_frontend::kDeepseekMarkers[6]}) {
    pack.tokens_by_id.emplace_back(marker);
    pack.token_types.push_back(3);
  }
  for (const std::string_view marker : {std::string_view{"<think>"}, std::string_view{"</think>"}}) {
    pack.tokens_by_id.emplace_back(marker);
    pack.token_types.push_back(4);
  }
  if (with_dsml) {
    pack.tokens_by_id.emplace_back(kDsml);
    pack.token_types.push_back(4);
  }
  pack.mode = secure_frontend::TokenizerMode::deepseek_bpe;
  return pack;
}

void plans_with_dsml_as_text() {
  secure_frontend::ChatCompletionRequest request;
  request.model = "m";
  request.messages = {{"user", "Hi"}};
  const std::vector<std::string> tools{kTool};
  const std::vector<secure_frontend::ToolRound> rounds{
      {"r", "", {{"web_search", {{"query", secure_frontend::JsonValue::make_string("q")}}}}, {"page <\xEF\xBD\x9C" "User\xEF\xBD\x9C>"}}};
  const auto pack = deepseek_pack();
  for (const std::optional<int> effort : {std::optional<int>{75}, std::optional<int>{}}) {
    const auto planned = secure_frontend::make_deepseek_tool_generation_plan(request, pack, tools, rounds, 100000U, 64U, {}, effort);
    const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&planned);
    expect(plan != nullptr, "a DeepSeek tool plan");
    if (plan == nullptr) continue;
    const auto pieces = secure_frontend::render_deepseek_tool_prompt(request.messages, tools, rounds, effort);
    const auto encoded = pieces ? secure_frontend::encode_prompt_pieces(pack, *pieces) : secure_frontend::TokenizerEncodeResult{};
    const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
    expect(ids != nullptr && plan->prompt_token_ids == *ids && plan->prompt_token_ids.front() == id_of(pack, secure_frontend::kDeepseekMarkers[0]),
           "the plan's prompt is the pieces encoded, BOS first");
    expect(plan->prompt_token_ids.back() == id_of(pack, effort ? "<think>" : "</think>") && plan->starts_in_reasoning == effort.has_value(),
           "thinking opens <think> after the last header and begins as reasoning; chat mode closes it");
    expect(plan->think_tokens.has_value() && plan->think_tokens->open == id_of(pack, "<think>") &&
               plan->think_tokens->close == id_of(pack, "</think>"),
           "<think>/</think> split the reasoning, as the text plan's do");
    expect(plan->stop_tokens.empty() && plan->server_stop_tokens == std::vector<std::size_t>({id_of(pack, secure_frontend::kDeepseekMarkers[6]),
                                                                                             id_of(pack, secure_frontend::kDeepseekMarkers[2])}),
           "the text plan's stops: the server stops on <｜end▁of▁sentence｜> or <｜User｜>");
    expect(plan->literal_tokens.size() == 1U && plan->literal_tokens[0].first == id_of(pack, kDsml) && plan->literal_tokens[0].second == kDsml,
           "｜DSML｜ is output as its text, for the call parser");
  }
  // a pack without ｜DSML｜ plans nothing (the chat is served without the tool)
  const auto refused = secure_frontend::make_deepseek_tool_generation_plan(request, deepseek_pack(false), tools, rounds, 100000U, 64U, {}, 75);
  const auto* error = std::get_if<secure_frontend::ChatGenerationPlanError>(&refused);
  expect(error != nullptr && error->code == secure_frontend::ChatGenerationPlanErrorCode::tokenization_failed,
         "a pack without ｜DSML｜ plans nothing (tokenization_failed: served without the tool)");
}

void the_route_plans_deepseek_searches() {
  using secure_frontend::NativeChatProfile;
  using secure_frontend::ToolFormat;
  expect(secure_frontend::search_tool_format(NativeChatProfile::deepseek) == std::optional<ToolFormat>{ToolFormat::deepseek},
         "a DeepSeek chat gets the tool in DeepSeek's form");
  expect(secure_frontend::find_model_option(secure_frontend::EngineProfile::deepseek, "web_search") != nullptr,
         "a DeepSeek card has the Web search switch");
  secure_frontend::ChatRouteConfig config{};
  config.max_prompt_tokens = 100000U;
  secure_frontend::ChatCompletionRequest request;
  request.model = "m";
  request.messages = {{"user", "Hi"}};
  const auto pack = deepseek_pack();
  for (const bool thinking : {true, false}) {
    const secure_frontend::SearchTurn turn{request, {{"web_search", {"query"}}, kTool}, thinking, {}, ToolFormat::deepseek};
    const auto planned = secure_frontend::search_first_plan(config, turn, pack);
    const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&planned);
    expect(plan != nullptr && plan->literal_tokens.size() == 1U && plan->starts_in_reasoning == thinking &&
               plan->prompt_token_ids.front() == id_of(pack, secure_frontend::kDeepseekMarkers[0]),
           std::string{"a DeepSeek turn plans in DeepSeek's form, "} + (thinking ? "thinking" : "chat mode"));
  }
}

// The route's first search plan over the REAL vocabulary (SECURE_FRONTEND_DEEPSEEK_PACK, a maya-ds4 pack's tokenizer/
// directory, never committed; unset, skipped).
void the_real_pack_plans_its_search() {
  const char* configured = secure_frontend::crt_getenv("SECURE_FRONTEND_DEEPSEEK_PACK");
  if (configured == nullptr || *configured == '\0' || !std::filesystem::is_directory(std::filesystem::path{configured})) {
    std::cout << "SKIPPED: SECURE_FRONTEND_DEEPSEEK_PACK names no tokenizer pack directory\n";
    return;
  }
  const auto loaded =
      secure_frontend::load_local_tokenizer_pack(std::filesystem::path{configured}, secure_frontend::TokenizerMode::deepseek_bpe);
  const auto* pack = std::get_if<secure_frontend::TokenizerPack>(&loaded);
  expect(pack != nullptr, "the real pack loads");
  if (pack == nullptr) return;
  secure_frontend::ChatRouteConfig config{};
  config.max_prompt_tokens = 100000U;
  secure_frontend::ChatCompletionRequest request;
  request.model = "m";
  request.messages = {{"user", "what is today's headlines?"}};
  for (const bool thinking : {true, false}) {
    const secure_frontend::SearchTurn turn{request, {{"web_search", {"query"}}, kTool}, thinking, {}, secure_frontend::ToolFormat::deepseek};
    const auto planned = secure_frontend::search_first_plan(config, turn, *pack);
    const auto* refused = std::get_if<secure_frontend::ChatGenerationPlanError>(&planned);
    expect(refused == nullptr, std::string{thinking ? "thinking" : "chat mode"} + ": the search plan, not " +
                                   (refused != nullptr ? refused->message : std::string{}));
    if (const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&planned)) {
      expect(plan->literal_tokens.size() == 1U && pack->tokens_by_id.at(plan->literal_tokens[0].first) == kDsml,
             "the real ｜DSML｜ token is output as its text");
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"renders_mayas_tool_prompts_byte_for_byte", renders_mayas_tool_prompts_byte_for_byte},
      {"refuses_what_the_tool_prompt_cannot_say", refuses_what_the_tool_prompt_cannot_say},
      {"reads_calls_as_maya_does", reads_calls_as_maya_does},
      {"plans_with_dsml_as_text", plans_with_dsml_as_text},
      {"the_route_plans_deepseek_searches", the_route_plans_deepseek_searches},
      {"the_real_pack_plans_its_search", the_real_pack_plans_its_search},
  }, failures);
}
