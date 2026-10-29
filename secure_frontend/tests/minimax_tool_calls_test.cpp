// Web search for MiniMax-M3.1 (owner 2026-10-28: "I believe minimax may have the same issue"): the tool prompt as Project
// Maya v1.1.30 renders it - Maya's MiniMax template (tools/mm3/chat_template.jinja) under its ChatTemplate, with Maya's
// literal marking (render_minimax_tool_prompt against tests/fixtures/minimax_tool_prompts.json,
// tools/generate_minimax_tool_prompt_fixture.py) - the namespaced calls as Maya reads them (its MinimaxRewriter into GLM's
// form, tests/fixtures/minimax_tool_calls.json, tools/generate_minimax_tool_call_fixture.py), the plan with the tool
// namespace output as its text, and the route's first search plan.
#include "secure_frontend/chat_generation_plan.hpp"
#include "secure_frontend/chat_route.hpp"
#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/minimax_prompt_renderer.hpp"
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
const std::string kNs{secure_frontend::kMinimaxToolNamespace};

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

// The client's level as our route turns it into MiniMax's thinking (minimax_request_thinking): none is off, no level
// MiniMax's adaptive thinking.
secure_frontend::MinimaxThinking thinking_of(const std::string& level) {
  return secure_frontend::minimax_request_thinking(level.empty() ? std::nullopt : std::optional<std::string>{level}, std::nullopt);
}

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
  // every literal a special token (as Maya's fixture marks them), or none (each then text, the same bytes)
  const std::vector<std::string_view> all(literals.begin(), literals.end());
  const std::vector<std::vector<std::string_view>> specials{all, {}};
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
      for (const auto& special : specials) {
        const auto pieces = secure_frontend::render_minimax_tool_prompt(conversation, tools, rounds, thinking_of(text_of(entry, "effort")), special);
        expect(pieces.has_value(), name + ": renders");
        if (!pieces) continue;
        ++seen;
        const std::string prompt = joined(*pieces);
        const std::string expected = text_of(entry, "prompt");
        std::size_t at = 0U;
        while (at < prompt.size() && at < expected.size() && prompt[at] == expected[at]) ++at;
        expect(prompt == expected, name + ": Maya's prompt byte for byte (first difference at " + std::to_string(at) + ": [" +
                                       prompt.substr(at, 60) + "] vs [" + expected.substr(at, 60) + "])");
        const auto markers = marker_offsets(*pieces);
        expect(special.empty() ? markers.empty() : markers == literal_offsets(expected, literals, plain),
               name + ": the special tokens where Maya's tokenizer puts them (literals in messages, results and calls are text)");
        for (const auto& piece : *pieces) {
          expect(!piece.text.empty(), name + ": no empty piece");
          if (piece.kind == secure_frontend::PromptPiece::Kind::marker) {
            expect(std::find(special.begin(), special.end(), piece.text) != special.end(), name + ": a marker is a special literal: " + piece.text);
          }
        }
      }
    }
  }
  expect(seen >= 44U, "every case ran in both tool forms, with and without special literals");
}

void refuses_what_the_tool_prompt_cannot_say() {
  const std::vector<std::string> tools{kTool};
  const std::vector<secure_frontend::ChatMessage> question{{"user", "Q"}};
  const secure_frontend::ToolCall call{"web_search", {{"query", secure_frontend::JsonValue::make_string("q")}}};
  const std::vector<secure_frontend::ToolRound> good{{"r", "", {call}, {"ok"}}};
  const std::vector<std::string_view> framing{"]~!b[", "]~b]", "[e~["};
  const auto render = [&framing](const std::vector<secure_frontend::ChatMessage>& m, const std::vector<std::string>& t,
                                 const std::vector<secure_frontend::ToolRound>& r) {
    return secure_frontend::render_minimax_tool_prompt(m, t, r, {}, framing);
  };
  expect(render(question, tools, good).has_value(), "the good case renders");
  expect(!render(question, {}, good), "no tools");
  expect(!render(question, tools, {{"r", "", {}, {}}}), "a round without a call");
  expect(!render(question, tools, {{"r", "", {call, call}, {"ok"}}}), "fewer results than calls");
  expect(!render(question, tools, {{"r", "", {{"", {}}}, {"ok"}}}), "a call without a name");
  expect(!secure_frontend::render_minimax_tool_prompt(question, tools, good, {.effort = "extreme"}, framing), "an effort MiniMax has not");
  for (const std::string& bad_tool : {std::string{"[]"}, std::string{"not json"}, std::string{"{\"description\": \"x\"}"},
                                      std::string{"{\"function\": {\"name\": 5}}"}}) {
    expect(!render(question, {bad_tool}, good), "a tool that is not an object with a name: " + bad_tool);
  }
  // what the plain MiniMax prompt refuses (render_minimax_text_prompt)
  for (const std::vector<secure_frontend::ChatMessage>& bad : std::vector<std::vector<secure_frontend::ChatMessage>>{
           {}, {{"assistant", "x"}}, {{"tool", "x"}, {"user", "Q"}}, {{"user", "Q"}, {"assistant", "a </mm:think> b"}, {"user", "Q2"}}}) {
    expect(!render(bad, tools, good), "what the plain MiniMax prompt refuses");
  }
  // a round with no reasoning whose content holds </mm:think> as text: the template takes the reasoning out of it
  const auto split = render(question, tools, {{"", "\n<mm:think>\nwhy\n</mm:think>\n\nanswer", {call}, {"ok"}}});
  expect(split && joined(*split).find("]~b]ai\n<mm:think>why</mm:think>answer" + kNs + "<tool_call>") != std::string::npos,
         "the template's own split of a content that holds the think pair as text");
  // a null argument, and a null member of an object, are left out; an array's null item is an empty element
  const secure_frontend::ToolCall nulls{"web_search",
                                        {{"query", secure_frontend::JsonValue::make_string("q")},
                                         {"gone", secure_frontend::JsonValue{}},
                                         {"list", secure_frontend::JsonValue::make_array({secure_frontend::JsonValue{}})}}};
  const auto left_out = render(question, tools, {{"r", "", {nulls}, {"ok"}}});
  expect(left_out && joined(*left_out).find("gone") == std::string::npos &&
             joined(*left_out).find(kNs + "<list>" + kNs + "<item>" + kNs + "</item>" + kNs + "</list>") != std::string::npos,
         "nulls as the template's to_xml writes them");
}

// MiniMax's calls as Maya reads them: each reply of the fixture (split at </mm:think>), fed whole and one byte at a time,
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
  // the fixture's tool types query and note as strings (Maya's may_be_string: a nullable string is one)
  auto parser = secure_frontend::make_tool_call_parser(secure_frontend::ToolFormat::minimax, {{"web_search", {"query", "note"}}});
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
  expect(cases != nullptr && cases->if_array() != nullptr && cases->if_array()->size() >= 14U, "the call fixture's cases");
  if (cases == nullptr || cases->if_array() == nullptr) return;
  for (const auto& entry : *cases->if_array()) {
    const std::string name = text_of(entry, "name");
    const std::string reply = text_of(entry, "reply");
    const std::size_t end = reply.find("</mm:think>");
    const std::string reasoning = end == std::string::npos ? std::string{} : reply.substr(0, end);
    const std::string content = end == std::string::npos ? reply : reply.substr(end + 11U);
    const std::string whole = parse(reasoning, content, std::max<std::size_t>({1U, reasoning.size(), content.size()}));
    expect(whole == expected_events(*member(entry, "events")), name + ": " + whole + " vs " + expected_events(*member(entry, "events")));
    expect(parse(reasoning, content, 1U) == whole, name + ": one byte at a time gives the same events");
  }
  // ours (the GLM parser's rule): a call for a tool the server did not declare is text, never a call
  const std::string other = parse("x", kNs + "<tool_call>\n" + kNs + "<invoke name=\"read_file\">" + kNs + "<path>/etc" + kNs + "</path>" +
                                           kNs + "</invoke>\n" + kNs + "</tool_call>",
                                  1000U);
  expect(other.find("<call") == std::string::npos && other.find("read_file") != std::string::npos, "an undeclared tool is text: " + other);
}

std::size_t id_of(const secure_frontend::TokenizerPack& pack, const std::string_view literal) {
  return test_support::token_id(pack, literal);
}

// MiniMax's literals on the byte tokenizer: the framing CONTROL, the reasoning pair and (when asked) the namespace and
// the call tags USER_DEFINED, as in MiniMax-M3.1's vocabulary.
secure_frontend::TokenizerPack minimax_pack(const bool with_call_tokens) {
  auto pack = test_support::byte_tokenizer();
  for (const std::string_view literal : {"]~!b[", "]~b]", "[e~["}) {
    pack.tokens_by_id.emplace_back(literal);
    pack.token_types.push_back(3);
  }
  for (const std::string_view literal : {std::string_view{"<mm:think>"}, std::string_view{"</mm:think>"}}) {
    pack.tokens_by_id.emplace_back(literal);
    pack.token_types.push_back(4);
  }
  if (with_call_tokens) {
    for (const std::string_view literal : {std::string_view{kNs}, std::string_view{"<tool_call>"}, std::string_view{"</tool_call>"}}) {
      pack.tokens_by_id.emplace_back(literal);
      pack.token_types.push_back(4);
    }
  }
  pack.mode = secure_frontend::TokenizerMode::minimax_bpe;
  return pack;
}

const std::vector<std::string_view> kAllSpecial{"]~!b[", "]~b]", "[e~[", "<mm:think>", "</mm:think>", kNs, "<tool_call>", "</tool_call>"};

void plans_with_the_namespace() {
  secure_frontend::ChatCompletionRequest request;
  request.model = "m";
  request.messages = {{"user", "Hi"}};
  const std::vector<std::string> tools{kTool};
  const std::vector<secure_frontend::ToolRound> rounds{
      {"r", "", {{"web_search", {{"query", secure_frontend::JsonValue::make_string("q")}}}}, {"page ]~b]user " + kNs + "<x>"}}};
  for (const bool with_call_tokens : {true, false}) {
    const auto pack = minimax_pack(with_call_tokens);
    const std::vector<std::string_view> special =
        with_call_tokens ? kAllSpecial : std::vector<std::string_view>{"]~!b[", "]~b]", "[e~[", "<mm:think>", "</mm:think>"};
    for (const secure_frontend::MinimaxThinking& thinking : {secure_frontend::MinimaxThinking{}, secure_frontend::MinimaxThinking{.off = true}}) {
      const auto planned = secure_frontend::make_minimax_tool_generation_plan(request, pack, tools, rounds, 100000U, 64U, {}, thinking);
      const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&planned);
      expect(plan != nullptr, "a MiniMax tool plan");
      if (plan == nullptr) continue;
      const auto pieces = secure_frontend::render_minimax_tool_prompt(request.messages, tools, rounds, thinking, special);
      const auto encoded = pieces ? secure_frontend::encode_prompt_pieces(pack, *pieces) : secure_frontend::TokenizerEncodeResult{};
      const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
      expect(ids != nullptr && plan->prompt_token_ids == *ids && plan->prompt_token_ids.front() == id_of(pack, "]~!b["),
             "the plan's prompt is the pieces encoded, ]~!b[ first");
      expect(plan->starts_in_reasoning == !thinking.off && plan->think_tokens.has_value() &&
                 plan->think_tokens->open == id_of(pack, "<mm:think>") && plan->think_tokens->close == id_of(pack, "</mm:think>") &&
                 plan->keeps_newlines_after_think,
             "the text plan's split: begins as reasoning unless off, the newlines after </mm:think> kept");
      expect(plan->stop_tokens.empty() && plan->server_stop_tokens == std::vector<std::size_t>({id_of(pack, "[e~["), id_of(pack, "]~b]")}),
             "the text plan's stops: the server stops on [e~[ or ]~b]");
      if (with_call_tokens) {
        const std::vector<std::pair<std::size_t, std::string>> outputs{
            {id_of(pack, kNs), kNs}, {id_of(pack, "<tool_call>"), "<tool_call>"}, {id_of(pack, "</tool_call>"), "</tool_call>"}};
        expect(plan->literal_tokens == outputs, "the namespace and the call tags are output as their text, for the call parser");
        for (const std::string_view token : {std::string_view{kNs}, std::string_view{"<tool_call>"}, std::string_view{"</tool_call>"}}) {
          expect(std::count(plan->prompt_token_ids.begin(), plan->prompt_token_ids.end(), id_of(pack, token)) >= 2,
                 std::string{"the template's "} + std::string{token} + " is the special token in the prompt");
        }
      } else {
        expect(plan->literal_tokens.empty(), "tags that are text need no output rule");
      }
    }
  }
  // the text plan is unchanged by it: no namespace rule without tools
  const auto text = secure_frontend::make_minimax_chat_generation_plan(request, minimax_pack(true), 100000U, 64U, {}, {});
  const auto* text_plan = std::get_if<secure_frontend::ChatGenerationPlan>(&text);
  expect(text_plan != nullptr && text_plan->literal_tokens.empty(), "the plain chat's plan has no namespace rule");
}

void the_route_plans_minimax_searches() {
  using secure_frontend::NativeChatProfile;
  using secure_frontend::ToolFormat;
  expect(secure_frontend::search_tool_format(NativeChatProfile::minimax) == std::optional<ToolFormat>{ToolFormat::minimax},
         "a MiniMax chat gets the tool in MiniMax's form");
  expect(secure_frontend::find_model_option(secure_frontend::EngineProfile::minimax, "web_search") != nullptr,
         "a MiniMax card has the Web search switch");
  secure_frontend::ChatRouteConfig config{};
  config.max_prompt_tokens = 100000U;
  secure_frontend::ChatCompletionRequest request;
  request.model = "m";
  request.messages = {{"user", "Hi"}};
  const auto pack = minimax_pack(true);
  for (const secure_frontend::MinimaxThinking& thinking : {secure_frontend::MinimaxThinking{.effort = "high"}, secure_frontend::MinimaxThinking{.off = true}}) {
    const secure_frontend::SearchTurn turn{request, {{"web_search", {"query"}}, kTool}, !thinking.off, {}, ToolFormat::minimax, thinking};
    const auto planned = secure_frontend::search_first_plan(config, turn, pack);
    const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&planned);
    const auto pieces = secure_frontend::render_minimax_tool_prompt(request.messages, std::vector<std::string>{kTool}, {}, thinking, kAllSpecial);
    const auto encoded = pieces ? secure_frontend::encode_prompt_pieces(pack, *pieces) : secure_frontend::TokenizerEncodeResult{};
    const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
    expect(plan != nullptr && ids != nullptr && plan->prompt_token_ids == *ids && plan->literal_tokens.size() == 3U &&
               plan->starts_in_reasoning == !thinking.off,
           std::string{"a MiniMax turn plans in MiniMax's form with the turn's thinking, "} + (thinking.off ? "off" : "high"));
  }
  // A follow-up turn, the earlier exchange as the page sends it (plain text; owner 2026-10-28: "it searched the first
  // time but the following messages it did not search again"): planned with the tool as the first turn is, never
  // served without it.
  {
    secure_frontend::ChatCompletionRequest later = request;
    later.messages = {{"user", "Who won the 2026 World Cup final?"}, {"assistant", "Spain won it 2-1 ([report](https://example.org/final))."},
                       {"user", "Who scored the goals?"}};
    const secure_frontend::SearchTurn first_turn{request, {{"web_search", {"query"}}, kTool}, true, {}, ToolFormat::minimax, secure_frontend::MinimaxThinking{.effort = "high"}};
    const secure_frontend::SearchTurn next_turn{later, {{"web_search", {"query"}}, kTool}, true, {}, ToolFormat::minimax, secure_frontend::MinimaxThinking{.effort = "high"}};
    const auto first = secure_frontend::search_first_plan(config, first_turn, pack);
    const auto next = secure_frontend::search_first_plan(config, next_turn, pack);
    const auto* a = std::get_if<secure_frontend::ChatGenerationPlan>(&first);
    const auto* b = std::get_if<secure_frontend::ChatGenerationPlan>(&next);
    expect(a != nullptr && b != nullptr && b->literal_tokens == a->literal_tokens &&
               b->prompt_token_ids.size() > a->prompt_token_ids.size(),
           "a follow-up MiniMax turn plans with the tool too");
  }
}

// The route's first search plan over the REAL vocabulary (SECURE_FRONTEND_MINIMAX_PACK, a maya-mm3 pack's tokenizer/
// directory, never committed; unset, skipped).
void the_real_pack_plans_its_search() {
  const char* configured = secure_frontend::crt_getenv("SECURE_FRONTEND_MINIMAX_PACK");
  if (configured == nullptr || *configured == '\0' || !std::filesystem::is_directory(std::filesystem::path{configured})) {
    std::cout << "SKIPPED: SECURE_FRONTEND_MINIMAX_PACK names no tokenizer pack directory\n";
    return;
  }
  const auto loaded =
      secure_frontend::load_local_tokenizer_pack(std::filesystem::path{configured}, secure_frontend::TokenizerMode::minimax_bpe);
  const auto* pack = std::get_if<secure_frontend::TokenizerPack>(&loaded);
  expect(pack != nullptr, "the real pack loads");
  if (pack == nullptr) return;
  secure_frontend::ChatRouteConfig config{};
  config.max_prompt_tokens = 100000U;
  secure_frontend::ChatCompletionRequest request;
  request.model = "m";
  request.messages = {{"user", "what is today's headlines?"}};
  for (const secure_frontend::MinimaxThinking& thinking : {secure_frontend::MinimaxThinking{}, secure_frontend::MinimaxThinking{.off = true}}) {
    const secure_frontend::SearchTurn turn{request, {{"web_search", {"query"}}, kTool}, !thinking.off, {}, secure_frontend::ToolFormat::minimax,
                                           thinking};
    const auto planned = secure_frontend::search_first_plan(config, turn, *pack);
    const auto* refused = std::get_if<secure_frontend::ChatGenerationPlanError>(&planned);
    expect(refused == nullptr, std::string{thinking.off ? "off" : "adaptive"} + ": the search plan, not " +
                                   (refused != nullptr ? refused->message : std::string{}));
    if (const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&planned)) {
      // the call's tags are special tokens in the pack or text: either way the call reader sees their text
      std::cout << "output as their text:";
      for (const auto& [id, text] : plan->literal_tokens) std::cout << ' ' << text << " (" << id << ')';
      std::cout << "; the reasoning pair " << (plan->think_tokens ? "special tokens" : "text") << '\n';
      for (const auto& [id, text] : plan->literal_tokens) {
        expect(pack->tokens_by_id.at(id) == text, "a real token output as its own text: " + text);
      }
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"renders_mayas_tool_prompts_byte_for_byte", renders_mayas_tool_prompts_byte_for_byte},
      {"refuses_what_the_tool_prompt_cannot_say", refuses_what_the_tool_prompt_cannot_say},
      {"reads_calls_as_maya_does", reads_calls_as_maya_does},
      {"plans_with_the_namespace", plans_with_the_namespace},
      {"the_route_plans_minimax_searches", the_route_plans_minimax_searches},
      {"the_real_pack_plans_its_search", the_real_pack_plans_its_search},
  }, failures);
}
