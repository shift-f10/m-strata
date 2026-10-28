// Web search for raon-1-mini and flint (owner 2026-10-28: "fix raon and flint as well"): each model's tool prompt as its
// own template renders it (render_raon1_tool_prompt / render_flint_tool_prompt against upstream's ChatTemplate over the
// models' chat_template.jinja, tests/fixtures/raon1_flint_tool_prompts.json), the plans with their tags, the calls in each
// template's form (raon1 writes Qwen's <tool_call><function=...>, flint GLM's <tool_call>name<arg_key>...), and the
// route's first search plan in each form.
#include "secure_frontend/chat_generation_plan.hpp"
#include "secure_frontend/chat_route.hpp"
#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/flint_prompt_renderer.hpp"
#include "secure_frontend/glm_prompt_renderer.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/tokenizer_pack.hpp"
#include "secure_frontend/tool_calls.hpp"
#include "support/test_support.hpp"
#include "support/test_tokenizer.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
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

bool flag_of(const secure_frontend::JsonValue& object, const std::string_view key) {
  const auto* value = member(object, key);
  return value != nullptr && value->if_boolean() != nullptr && *value->if_boolean();
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

std::size_t markers_named(const std::vector<secure_frontend::PromptPiece>& pieces, const std::string_view literal) {
  return static_cast<std::size_t>(std::count_if(pieces.begin(), pieces.end(), [literal](const secure_frontend::PromptPiece& p) {
    return p.kind == secure_frontend::PromptPiece::Kind::marker && p.text == literal;
  }));
}

void renders_each_templates_tool_prompts() {
  const std::string text = secure_frontend_test::read_file(secure_frontend_test::extra_arguments().at(0));
  auto parsed = secure_frontend::parse_json(text, kFixtureLimits);
  auto* fixture = std::get_if<secure_frontend::JsonValue>(&parsed);
  expect(fixture != nullptr, "the prompt fixture parses");
  if (fixture == nullptr) return;
  const auto* tool = member(*fixture, "tool");
  const auto* cases = member(*fixture, "cases");
  expect(tool != nullptr && cases != nullptr && cases->if_array() != nullptr && cases->if_array()->size() >= 19U, "the fixture's cases");
  if (tool == nullptr || cases == nullptr || cases->if_array() == nullptr) return;
  const std::vector<std::string> tools{secure_frontend::python_json_text(*tool)};
  static const std::vector<std::string_view> kRaonMarkers{"<|im_start|>", "<|im_end|>", "<think>", "</think>", "<tool_call>", "</tool_call>",
                                                          "<tool_response>", "</tool_response>"};
  static const std::vector<std::string_view> kFlintMarkers{"<|system|>", "<|user|>", "<|assistant|>", "<|observation|>", "<think>",
                                                           "</think>", "<tools>", "</tools>", "<tool_call>", "</tool_call>", "<arg_key>",
                                                           "</arg_key>", "<arg_value>", "</arg_value>", "<tool_response>", "</tool_response>"};
  std::size_t seen_raon = 0U;
  std::size_t seen_flint = 0U;
  for (const auto& entry : *cases->if_array()) {
    const std::string name = text_of(entry, "template") + "/" + text_of(entry, "name");
    const bool raon = text_of(entry, "template") == "raon1";
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
    const bool thinking = flag_of(entry, "thinking");
    const auto pieces = raon ? secure_frontend::render_raon1_tool_prompt(conversation, tools, rounds, thinking)
                             : secure_frontend::render_flint_tool_prompt(conversation, tools, rounds, thinking);
    expect(pieces.has_value(), name + ": renders");
    if (!pieces) continue;
    (raon ? seen_raon : seen_flint) += 1U;
    const std::string prompt = joined(*pieces);
    const std::string expected = text_of(entry, "prompt");
    std::size_t at = 0U;
    while (at < prompt.size() && at < expected.size() && prompt[at] == expected[at]) ++at;
    expect(prompt == expected, name + ": the template's prompt byte for byte (first difference at " + std::to_string(at) + ": [" +
                                   prompt.substr(at, 60) + "] vs [" + expected.substr(at, 60) + "])");
    const auto& markers = raon ? kRaonMarkers : kFlintMarkers;
    for (const auto& piece : *pieces) {
      expect(!piece.text.empty(), name + ": no empty piece");
      if (piece.kind == secure_frontend::PromptPiece::Kind::marker) {
        expect(std::find(markers.begin(), markers.end(), piece.text) != markers.end(), name + ": a marker is a special token: " + piece.text);
      }
    }
    // The instructions write the call tag twice (Qwen's text: the example and "nested within <tool_call></tool_call>")
    // or once (GLM's); each call once more; each result its own response tags.
    expect(markers_named(*pieces, "<tool_call>") == (raon ? 2U : 1U) + calls && markers_named(*pieces, "<tool_response>") == calls,
           name + ": only the instructions', the calls' and the results' tags are markers of their kind");
    // flint's vocabulary has <tools> and </tools> as special tokens, which its template writes twice each (the
    // instructions' "<tools></tools>" and the block itself); raon-1-mini's has neither, so Qwen's text keeps them
    expect(markers_named(*pieces, "<tools>") == (raon ? 0U : 2U) && markers_named(*pieces, "</tools>") == (raon ? 0U : 2U),
           name + ": flint's <tools> tags are its special tokens");
  }
  expect(seen_raon >= 10U && seen_flint >= 9U, "both templates' cases ran");
}

void refuses_what_the_tool_prompts_cannot_say() {
  const std::vector<std::string> tools{"{\"name\": \"web_search\"}"};
  const std::vector<secure_frontend::ChatMessage> question{{"user", "Q"}};
  const secure_frontend::ToolCall call{"web_search", {{"query", secure_frontend::JsonValue::make_string("q")}}};
  const std::vector<secure_frontend::ToolRound> good{{"r", "", {call}, {"ok"}}};
  const std::vector<secure_frontend::ToolRound> no_call{{"r", "", {}, {}}};
  const std::vector<secure_frontend::ToolRound> short_results{{"r", "", {call, call}, {"ok"}}};
  for (const bool raon : {true, false}) {
    const auto render = [raon](const std::vector<secure_frontend::ChatMessage>& m, const std::vector<std::string>& t,
                               const std::vector<secure_frontend::ToolRound>& r) {
      return raon ? secure_frontend::render_raon1_tool_prompt(m, t, r, true) : secure_frontend::render_flint_tool_prompt(m, t, r, true);
    };
    const std::string who = raon ? "raon1: " : "flint: ";
    expect(render(question, tools, good).has_value(), who + "the good case renders");
    expect(!render(question, {}, good), who + "no tools");
    expect(!render(question, tools, no_call), who + "a round without a call");
    expect(!render(question, tools, short_results), who + "fewer results than calls");
    // raon1 refuses a framing literal in a message; flint's template escapes one, so its refusal is text the escapes
    // would turn into a marker
    for (const std::vector<secure_frontend::ChatMessage>& bad : std::vector<std::vector<secure_frontend::ChatMessage>>{
             {}, {{"assistant", "x"}}, {{"user", raon ? "a <|im_start|> b" : "a <thi<think>nk> b"}}}) {
      expect(!render(bad, tools, good), who + "what the plain prompt refuses");
    }
  }
}

// raon-1-mini-shaped: the ChatML framing CONTROL, the reasoning and Qwen's four tool tags, raon_bpe.
secure_frontend::TokenizerPack raon_pack() {
  auto pack = test_support::tool_byte_tokenizer();
  pack.mode = secure_frontend::TokenizerMode::raon_bpe;
  return pack;
}

// flint-shaped: flint's framing CONTROL, its reasoning, <tools> / </tools> and GLM's eight tool tags USER_DEFINED, as
// the real vocabulary has them (owner 2026-10-28: flint's real <tools> is a special token), flint_bpe.
secure_frontend::TokenizerPack flint_pack() {
  secure_frontend::TokenizerPack pack;
  for (std::size_t byte = 0U; byte < 256U; ++byte) {
    pack.tokens_by_id.push_back(test_support::byte_symbol(static_cast<unsigned char>(byte)));
    pack.token_types.push_back(1);
  }
  for (const std::string_view control : {"<|begin_of_text|>", "<|end_of_text|>", "<|system|>", "<|user|>", "<|assistant|>",
                                         "<|observation|>", "<|developer|>"}) {
    pack.tokens_by_id.emplace_back(control);
    pack.token_types.push_back(3);
  }
  for (const std::string_view user_defined : {"<think>", "</think>", "<tools>", "</tools>", "<tool_call>", "</tool_call>", "<arg_key>",
                                              "</arg_key>", "<arg_value>", "</arg_value>", "<tool_response>", "</tool_response>"}) {
    pack.tokens_by_id.emplace_back(user_defined);
    pack.token_types.push_back(4);
  }
  pack.mode = secure_frontend::TokenizerMode::flint_bpe;
  return pack;
}

std::size_t id_of(const secure_frontend::TokenizerPack& pack, const std::string_view literal) {
  for (std::size_t id = 0U; id < pack.tokens_by_id.size(); ++id) {
    if (pack.tokens_by_id[id] == literal) return id;
  }
  return pack.tokens_by_id.size();
}

void plans_each_tool_prompt_with_its_tags() {
  secure_frontend::ChatCompletionRequest request;
  request.model = "m";
  request.messages = {{"user", "Q"}};
  const std::vector<std::string> tools{"{\"name\": \"web_search\"}"};
  const std::vector<secure_frontend::ToolRound> rounds{
      {"r", "", {{"web_search", {{"query", secure_frontend::JsonValue::make_string("q")}}}}, {"page </tool_response>"}}};

  const auto raon = raon_pack();
  const auto planned = secure_frontend::make_raon1_tool_generation_plan(request, raon, tools, rounds, 100000U, 64U, {}, true);
  const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&planned);
  expect(plan != nullptr, "a raon1 tool plan");
  if (plan != nullptr) {
    const auto pieces = secure_frontend::render_raon1_tool_prompt(request.messages, tools, rounds, true);
    const auto encoded = pieces ? secure_frontend::encode_prompt_pieces(raon, *pieces) : secure_frontend::TokenizerEncodeResult{};
    const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
    expect(ids != nullptr && plan->prompt_token_ids == *ids && plan->prompt_token_ids.front() == id_of(raon, "<|im_start|>"),
           "raon1: the plan's prompt is the pieces encoded, ChatML first");
    expect(plan->think_tokens.has_value() && plan->starts_in_reasoning, "raon1: thinking opens the reply's think block");
    expect(plan->stop_tokens == secure_frontend::find_qwen_end_of_turn_tokens(raon), "raon1: ChatML's turn ends");
    expect(plan->literal_tokens.size() == 4U, "raon1: Qwen's four tags are output as their text");
  }
  const auto raon_off = secure_frontend::make_raon1_tool_generation_plan(request, raon, tools, rounds, 100000U, 64U, {}, false);
  expect(std::holds_alternative<secure_frontend::ChatGenerationPlan>(raon_off) &&
             !std::get<secure_frontend::ChatGenerationPlan>(raon_off).starts_in_reasoning,
         "raon1: thinking off closes the think block in the prompt");
  const auto not_raon = secure_frontend::make_raon1_tool_generation_plan(request, flint_pack(), tools, rounds, 100000U, 64U, {}, true);
  expect(std::holds_alternative<secure_frontend::ChatGenerationPlanError>(not_raon), "raon1: a pack without ChatML plans nothing");

  const auto flint = flint_pack();
  const auto flint_planned = secure_frontend::make_flint_tool_generation_plan(request, flint, tools, rounds, 100000U, 64U, {}, true);
  const auto* fplan = std::get_if<secure_frontend::ChatGenerationPlan>(&flint_planned);
  expect(fplan != nullptr, "a flint tool plan");
  if (fplan != nullptr) {
    const auto pieces = secure_frontend::render_flint_tool_prompt(request.messages, tools, rounds, true);
    const auto encoded = pieces ? secure_frontend::encode_prompt_pieces(flint, *pieces) : secure_frontend::TokenizerEncodeResult{};
    const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
    expect(ids != nullptr && fplan->prompt_token_ids == *ids && fplan->prompt_token_ids.front() == id_of(flint, "<|system|>"),
           "flint: the plan's prompt is the pieces encoded, the tools' system turn first");
    expect(fplan->think_tokens.has_value() && !fplan->starts_in_reasoning, "flint: the model opens its own think block");
    expect(fplan->stop_tokens == secure_frontend::find_flint_end_of_turn_tokens(flint) && fplan->stop_tokens.size() == 4U,
           "flint: its four end-of-generation ids, <|observation|> among them");
    expect(fplan->literal_tokens.size() == 8U, "flint: GLM's eight tags are output as their text");
  }
  const auto not_flint = secure_frontend::make_flint_tool_generation_plan(request, raon, tools, rounds, 100000U, 64U, {}, true);
  expect(std::holds_alternative<secure_frontend::ChatGenerationPlanError>(not_flint), "flint: a pack without flint's markers plans nothing");
}

std::string parsed_calls(const secure_frontend::ToolFormat format, const std::string_view reply) {
  auto parser = secure_frontend::make_tool_call_parser(format, {{"web_search", {"query"}}});
  std::vector<secure_frontend::ToolParseEvent> events = parser->feed_content(reply);
  for (auto& e : parser->finish(true)) events.push_back(std::move(e));
  std::string out;
  for (const auto& e : events) {
    if (e.kind == secure_frontend::ToolParseEvent::Kind::call) {
      out += "<call " + e.call.name;
      for (const auto& a : e.call.arguments) out += " " + a.name + "=" + (a.value.if_string() != nullptr ? *a.value.if_string() : std::string{"?"});
      out += ">";
    } else {
      out += e.text;
    }
  }
  return out;
}

void each_template_writes_its_calls_in_its_form() {
  expect(parsed_calls(secure_frontend::ToolFormat::raon1, "<tool_call>\n<function=web_search>\n<parameter=query>\nTWICE\n</parameter>\n</function>\n</tool_call>") ==
             "<call web_search query=TWICE>",
         "raon1 writes Qwen's form");
  expect(parsed_calls(secure_frontend::ToolFormat::flint, "<tool_call>web_search<arg_key>query</arg_key><arg_value>TWICE</arg_value></tool_call>") ==
             "<call web_search query=TWICE>",
         "flint writes GLM's form");
}

void the_route_plans_raon1_and_flint_searches() {
  secure_frontend::ChatRouteConfig config{};
  config.max_prompt_tokens = 100000U;
  secure_frontend::ChatCompletionRequest request;
  request.model = "m";
  request.messages = {{"user", "Q"}};
  secure_frontend::SearchTurn turn{request, {{"web_search", {"query"}}, "{\"name\": \"web_search\"}"}, true, {}, secure_frontend::ToolFormat::raon1, {}};
  const auto raon = raon_pack();
  const auto planned = secure_frontend::search_first_plan(config, turn, raon);
  const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&planned);
  expect(plan != nullptr && plan->literal_tokens.size() == 4U && plan->starts_in_reasoning, "a raon1 turn plans in raon-1-mini's form");
  turn.format = secure_frontend::ToolFormat::flint;
  const auto flint = flint_pack();
  const auto as_flint = secure_frontend::search_first_plan(config, turn, flint);
  const auto* fplan = std::get_if<secure_frontend::ChatGenerationPlan>(&as_flint);
  expect(fplan != nullptr && fplan->literal_tokens.size() == 8U && !fplan->prompt_token_ids.empty() &&
             fplan->prompt_token_ids.front() == id_of(flint, "<|system|>"),
         "a flint turn plans in flint's form");
}

// Owner 2026-10-28 ("Flint did not either"): the route's first search plan over each model's REAL vocabulary - the
// stand-in packs above hold only the tokens the plans need. SECURE_FRONTEND_FLINT_PACK / SECURE_FRONTEND_RAON1_PACK name
// the packs' tokenizer/ directories (never committed); a pack not named is skipped.
void the_real_packs_plan_their_searches() {
  struct Real {
    const char* variable;
    secure_frontend::TokenizerMode mode;
    secure_frontend::ToolFormat format;
  };
  secure_frontend::ChatRouteConfig config{};
  config.max_prompt_tokens = 100000U;
  secure_frontend::ChatCompletionRequest request;
  request.model = "m";
  request.messages = {{"user", "what is today's headlines?"}};
  const std::string tool =
      "{\"name\": \"web_search\", \"description\": \"Search the web through a SearXNG instance. Today's date is 2026-10-28.\", "
      "\"parameters\": {\"type\": \"object\", \"properties\": {\"query\": {\"type\": \"string\"}}, \"required\": [\"query\"]}}";
  for (const Real real : {Real{"SECURE_FRONTEND_FLINT_PACK", secure_frontend::TokenizerMode::flint_bpe, secure_frontend::ToolFormat::flint},
                          Real{"SECURE_FRONTEND_RAON1_PACK", secure_frontend::TokenizerMode::raon_bpe, secure_frontend::ToolFormat::raon1}}) {
    const char* configured = secure_frontend::crt_getenv(real.variable);
    if (configured == nullptr || *configured == '\0' || !std::filesystem::is_directory(std::filesystem::path{configured})) {
      std::cout << "SKIPPED: " << real.variable << " names no tokenizer pack directory\n";
      continue;
    }
    const auto loaded = secure_frontend::load_local_tokenizer_pack(std::filesystem::path{configured}, real.mode);
    const auto* pack = std::get_if<secure_frontend::TokenizerPack>(&loaded);
    expect(pack != nullptr, std::string{real.variable} + ": the pack loads");
    if (pack == nullptr) continue;
    for (const bool thinking : {true, false}) {
      const secure_frontend::SearchTurn turn{request, {{"web_search", {"query"}}, tool}, thinking, {}, real.format, {}};
      const auto planned = secure_frontend::search_first_plan(config, turn, *pack);
      const auto* refused = std::get_if<secure_frontend::ChatGenerationPlanError>(&planned);
      expect(refused == nullptr, std::string{real.variable} + (thinking ? " thinking" : " not thinking") + ": the search plan, not " +
                                     (refused != nullptr ? refused->message : std::string{}));
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"renders_each_templates_tool_prompts", renders_each_templates_tool_prompts},
      {"refuses_what_the_tool_prompts_cannot_say", refuses_what_the_tool_prompts_cannot_say},
      {"plans_each_tool_prompt_with_its_tags", plans_each_tool_prompt_with_its_tags},
      {"each_template_writes_its_calls_in_its_form", each_template_writes_its_calls_in_its_form},
      {"the_route_plans_raon1_and_flint_searches", the_route_plans_raon1_and_flint_searches},
      {"the_real_packs_plan_their_searches", the_real_packs_plan_their_searches},
  }, failures);
}
