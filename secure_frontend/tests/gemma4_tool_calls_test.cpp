// Web search for Gemma 4 (owner 2026-10-28: "gemma4 did not use search"): the tool prompt as the GGUF's own template
// renders it over llama.cpp's message path (render_gemma4_tool_prompt against tests/fixtures/gemma4_tool_prompts.json,
// tools/generate_gemma4_tool_prompt_fixture.py), the calls as llama.cpp 3cf03257 reads them (common/parsers/gemma4.cpp's
// grammar; the cases of tests/test-chat.cpp's "Google Gemma 4 (tool calling with Gemma4 dict format)" block), the plan
// with its tags and its stop on <|tool_response>, and the route's first search plan.
#include "secure_frontend/chat_generation_plan.hpp"
#include "secure_frontend/chat_route.hpp"
#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/gemma4_prompt_renderer.hpp"
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

const std::string kTool =
    "{\"name\": \"web_search\", \"description\": \"Search the web.\", \"parameters\": {\"type\": \"object\", \"properties\": "
    "{\"query\": {\"type\": \"string\", \"description\": \"What to search for\"}}, \"required\": [\"query\"]}}";

void renders_the_templates_tool_prompts() {
  const std::string text = secure_frontend_test::read_file(secure_frontend_test::extra_arguments().at(0));
  auto parsed = secure_frontend::parse_json(text, kFixtureLimits);
  auto* fixture = std::get_if<secure_frontend::JsonValue>(&parsed);
  expect(fixture != nullptr, "the prompt fixture parses");
  if (fixture == nullptr) return;
  const auto* tool = member(*fixture, "tool");
  const auto* cases = member(*fixture, "cases");
  expect(tool != nullptr && cases != nullptr && cases->if_array() != nullptr && cases->if_array()->size() >= 12U, "the fixture's cases");
  if (tool == nullptr || cases == nullptr || cases->if_array() == nullptr) return;
  // the search service's form ({"name", "description", "parameters"}) and llama.cpp's OpenAI form render alike
  const std::vector<std::vector<std::string>> forms{{secure_frontend::python_json_text(*tool)},
                                                    {"{\"type\": \"function\", \"function\": " + secure_frontend::python_json_text(*tool) + "}"}};
  static const std::vector<std::string_view> kMarkers{"<bos>", "<|turn>", "<turn|>", "<|think|>", "<|tool>", "<tool|>", "<|tool_call>",
                                                      "<tool_call|>", "<|tool_response>", "<tool_response|>", "<|\"|>", "<|channel>", "<channel|>"};
  std::size_t seen = 0U;
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
    for (const auto& tools : forms) {
      const auto pieces = secure_frontend::render_gemma4_tool_prompt(conversation, tools, rounds, flag_of(entry, "thinking"));
      expect(pieces.has_value(), name + ": renders");
      if (!pieces) continue;
      ++seen;
      const std::string prompt = joined(*pieces);
      const std::string expected = text_of(entry, "prompt");
      std::size_t at = 0U;
      while (at < prompt.size() && at < expected.size() && prompt[at] == expected[at]) ++at;
      expect(prompt == expected, name + ": the template's prompt byte for byte (first difference at " + std::to_string(at) + ": [" +
                                     prompt.substr(at, 60) + "] vs [" + expected.substr(at, 60) + "])");
      for (const auto& piece : *pieces) {
        expect(!piece.text.empty(), name + ": no empty piece");
        if (piece.kind == secure_frontend::PromptPiece::Kind::marker) {
          expect(std::find(kMarkers.begin(), kMarkers.end(), piece.text) != kMarkers.end(), name + ": a marker is a special token: " + piece.text);
        }
      }
      // one declaration, each call and each result its own tags; <bos> first
      expect(markers_named(*pieces, "<|tool>") == 1U && markers_named(*pieces, "<|tool_call>") == calls &&
                 markers_named(*pieces, "<|tool_response>") == calls && markers_named(*pieces, "<bos>") == 1U &&
                 pieces->front().text == "<bos>",
             name + ": only the declaration's, the calls' and the results' tags are markers of their kind");
      // the web's and the model's words are plain (their literals are text to the encoder), never a marker
      for (const auto& round : rounds) {
        for (const auto& result : round.results) {
          if (result.find("<|") == std::string::npos) continue;
          expect(std::any_of(pieces->begin(), pieces->end(), [&result](const secure_frontend::PromptPiece& p) {
                   return p.kind == secure_frontend::PromptPiece::Kind::plain && p.text.find(result) != std::string::npos;
                 }),
                 name + ": a result with literals is one plain span");
        }
      }
    }
  }
  expect(seen >= 24U, "every case ran in both tool forms");
}

void refuses_what_the_tool_prompt_cannot_say() {
  const std::vector<std::string> tools{kTool};
  const std::vector<secure_frontend::ChatMessage> question{{"user", "Q"}};
  const secure_frontend::ToolCall call{"web_search", {{"query", secure_frontend::JsonValue::make_string("q")}}};
  const std::vector<secure_frontend::ToolRound> good{{"r", "", {call}, {"ok"}}};
  const auto render = [](const std::vector<secure_frontend::ChatMessage>& m, const std::vector<std::string>& t,
                         const std::vector<secure_frontend::ToolRound>& r) {
    return secure_frontend::render_gemma4_tool_prompt(m, t, r, true);
  };
  expect(render(question, tools, good).has_value(), "the good case renders");
  expect(!render(question, {}, good), "no tools");
  expect(!render(question, tools, {{"r", "", {}, {}}}), "a round without a call");
  expect(!render(question, tools, {{"r", "", {call, call}, {"ok"}}}), "fewer results than calls");
  expect(!render(question, tools, {{"r", "", {{"", {}}}, {"ok"}}}), "a call without a name");
  for (const std::string& bad_tool : {std::string{"[]"}, std::string{"not json"}, std::string{"{\"description\": \"x\"}"},
                                      std::string{"{\"function\": {\"name\": 5}}"}}) {
    expect(!render(question, {bad_tool}, good), "a tool that is not an object with a name: " + bad_tool);
  }
  for (const std::vector<secure_frontend::ChatMessage>& bad : std::vector<std::vector<secure_frontend::ChatMessage>>{
           {}, {{"assistant", "x"}}, {{"user", "a <|tool_call> b"}}, {{"user", "a <|\"|> b"}}, {{"user", "  "}}}) {
    expect(!render(bad, tools, good), "what the plain Gemma 4 prompt refuses");
  }
}

// Gemma 4's calls as llama.cpp reads them: each reply of test-chat.cpp's Gemma 4 block (and a few of ours), fed whole and
// one byte at a time, gives the same events.
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
  auto parser = secure_frontend::make_tool_call_parser(
      secure_frontend::ToolFormat::gemma4,
      {{"get_time", {"city"}}, {"get_weather", {"city"}}, {"empty_args", {}}, {"special_function", {}}, {"amount", {}}, {"toggle", {}},
       {"set_nullable", {}}, {"todo_list", {}}, {"set_config", {}}, {"web_search", {"query"}}});
  std::vector<secure_frontend::ToolParseEvent> events;
  const auto add = [&events](std::vector<secure_frontend::ToolParseEvent> more) {
    for (auto& e : more) events.push_back(std::move(e));
  };
  for (std::size_t i = 0; i < reasoning.size(); i += step) add(parser->feed_reasoning(reasoning.substr(i, step)));
  if (!reasoning.empty()) add(parser->end_reasoning());
  for (std::size_t i = 0; i < content.size(); i += step) add(parser->feed_content(content.substr(i, step)));
  add(parser->finish(true));
  return merged(std::move(events));
}

void reads_calls_as_llama_cpp_does() {
  struct Case {
    std::string_view name;
    std::string_view reasoning;
    std::string_view content;
    std::string_view events;
  };
  static const std::vector<Case> kCases{
      {"text only", "", "Hello, world!", "[c:Hello, world!]"},
      {"reasoning and content", "I'm\nthinking", "Hello, world!\nWhat's up?", "[r:I'm\nthinking][c:Hello, world!\nWhat's up?]"},
      {"a string argument", "", "<|tool_call>call:get_time{city:<|\"|>London<|\"|>}<tool_call|>", "<call get_time {city: \"London\"}>"},
      {"a string with a space", "", "<|tool_call>call:get_time{city:<|\"|>San Francisco<|\"|>}<tool_call|>",
       "<call get_time {city: \"San Francisco\"}>"},
      {"empty arguments", "", "<|tool_call>call:empty_args{}<tool_call|>", "<call empty_args {}>"},
      {"content then a call", "", "Hello, world!\nWhat's up?<|tool_call>call:get_time{city:<|\"|>Paris<|\"|>}<tool_call|>",
       "[c:Hello, world!\nWhat's up?]<call get_time {city: \"Paris\"}>"},
      {"a call after reasoning", "I'm\nthinking", "<|tool_call>call:get_time{city:<|\"|>Paris<|\"|>}<tool_call|>",
       "[r:I'm\nthinking]<call get_time {city: \"Paris\"}>"},
      {"parallel calls", "",
       "<|tool_call>call:get_time{city:<|\"|>London<|\"|>}<tool_call|><|tool_call>call:get_weather{city:<|\"|>Paris<|\"|>}<tool_call|>",
       "<call get_time {city: \"London\"}><call get_weather {city: \"Paris\"}>"},
      {"an integer", "", "<|tool_call>call:special_function{arg1:42}<tool_call|>", "<call special_function {arg1: 42}>"},
      {"a negative number", "", "<|tool_call>call:special_function{arg1:-7}<tool_call|>", "<call special_function {arg1: -7}>"},
      {"a decimal", "", "<|tool_call>call:amount{orig:3.14}<tool_call|>", "<call amount {orig: 3.14}>"},
      {"true", "", "<|tool_call>call:toggle{enabled:true}<tool_call|>", "<call toggle {enabled: true}>"},
      {"false", "", "<|tool_call>call:toggle{enabled:false}<tool_call|>", "<call toggle {enabled: false}>"},
      {"null", "", "<|tool_call>call:set_nullable{value:null}<tool_call|>", "<call set_nullable {value: null}>"},
      {"an array", "", "<|tool_call>call:todo_list{todos:[<|\"|>buy milk<|\"|>,<|\"|>walk dog<|\"|>]}<tool_call|>",
       "<call todo_list {todos: [\"buy milk\", \"walk dog\"]}>"},
      {"a dict", "", "<|tool_call>call:set_config{config:{theme:<|\"|>dark<|\"|>,count:3}}<tool_call|>",
       "<call set_config {config: {\"theme\": \"dark\", \"count\": 3}}>"},
      {"an empty array", "", "<|tool_call>call:todo_list{todos:[]}<tool_call|>", "<call todo_list {todos: []}>"},
      {"an empty dict", "", "<|tool_call>call:set_config{config:{}}<tool_call|>", "<call set_config {config: {}}>"},
      {"an exponent", "", "<|tool_call>call:amount{orig:1.5e10}<tool_call|>", "<call amount {orig: 15000000000.0}>"},
      // ours: the grammar's spaces, two arguments, a string holding anything but its delimiter, the web_search tool
      {"spaces where the grammar allows them", "",
       "<|tool_call>call:web_search{ query: <|\"|>a, b: {c}<|\"|>, max_results: 3 }<tool_call|>",
       "<call web_search {query: \"a, b: {c}\", max_results: 3}>"},
      {"a repeated name keeps its first place and its last value", "",
       "<|tool_call>call:web_search{query:<|\"|>a<|\"|>,max_results:2,query:<|\"|>b<|\"|>}<tool_call|>",
       "<call web_search {query: \"b\", max_results: 2}>"},
      {"a call in the reasoning stays reasoning", "I would write <|tool_call>call:web_search{query:<|\"|>q<|\"|>}<tool_call|>", "Answer.",
       "[r:I would write <|tool_call>call:web_search{query:<|\"|>q<|\"|>}<tool_call|>][c:Answer.]"},
      {"an undeclared tool is text", "", "<|tool_call>call:delete_files{all:true}<tool_call|>",
       "[c:<|tool_call>call:delete_files{all:true}<tool_call|>]"},
      {"a malformed call is text", "", "<|tool_call>call:web_search{query:<|\"|>q}<tool_call|>",
       "[c:<|tool_call>call:web_search{query:<|\"|>q}<tool_call|>]"},
      {"text after the dict is no call", "", "<|tool_call>call:web_search{query:<|\"|>q<|\"|>} extra<tool_call|>",
       "[c:<|tool_call>call:web_search{query:<|\"|>q<|\"|>} extra<tool_call|>]"},
      {"an unfinished call is text", "", "Now <|tool_call>call:web_search{query:<|\"|>q",
       "[c:Now <|tool_call>call:web_search{query:<|\"|>q]"},
      {"a partial tag at the end is text", "", "The end <|tool_", "[c:The end <|tool_]"},
      {"unicode around a call", "", "答え：<|tool_call>call:web_search{query:<|\"|>日本 été<|\"|>}<tool_call|>",
       "[c:答え：]<call web_search {query: \"日本 été\"}>"},
  };
  for (const auto& c : kCases) {
    const std::string whole = parse(c.reasoning, c.content, std::max<std::size_t>({1U, c.reasoning.size(), c.content.size()}));
    expect(whole == c.events, std::string{c.name} + ": " + whole);
    expect(parse(c.reasoning, c.content, 1U) == whole, std::string{c.name} + ": one byte at a time gives the same events");
  }
}

std::size_t id_of(const secure_frontend::TokenizerPack& pack, const std::string_view literal) {
  return test_support::token_id(pack, literal);
}

void plans_with_its_tags() {
  secure_frontend::ChatCompletionRequest request;
  request.model = "m";
  request.messages = {{"user", "Hi"}};
  const std::vector<std::string> tools{kTool};
  const std::vector<secure_frontend::ToolRound> rounds{
      {"r", "", {{"web_search", {{"query", secure_frontend::JsonValue::make_string("q")}}}}, {"page <tool_response|>"}}};
  const auto pack = test_support::spm_tokenizer();
  for (const bool thinking : {true, false}) {
    const auto planned = secure_frontend::make_gemma4_tool_generation_plan(request, pack, tools, rounds, 100000U, 64U, {}, thinking);
    const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&planned);
    expect(plan != nullptr, "a Gemma 4 tool plan");
    if (plan == nullptr) continue;
    const auto pieces = secure_frontend::render_gemma4_tool_prompt(request.messages, tools, rounds, thinking);
    const auto encoded = pieces ? secure_frontend::encode_prompt_pieces(pack, *pieces) : secure_frontend::TokenizerEncodeResult{};
    const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
    expect(ids != nullptr && plan->prompt_token_ids == *ids && plan->prompt_token_ids.front() == id_of(pack, "<bos>"),
           "the plan's prompt is the pieces encoded, <bos> first");
    expect(plan->think_tokens.has_value() && plan->think_tokens->open == id_of(pack, "<|channel>") &&
               plan->think_tokens->close == id_of(pack, "<channel|>") && !plan->starts_in_reasoning &&
               plan->channel_name == std::optional<std::string>{"thought"},
           "the thought channel splits the reasoning, as the text plan's does");
    expect(plan->stop_tokens == secure_frontend::find_gemma4_end_of_turn_tokens(pack), "the engine ends the turn on <turn|> or <eos>");
    // llama.cpp ends a Gemma 4 generation on <|tool_response> (llama-vocab.cpp's gemma4 end-of-generation set); our
    // engine does not, so the server stops it there
    expect(plan->server_stop_tokens == std::vector<std::size_t>{id_of(pack, "<|tool_response>")}, "the server stops it on <|tool_response>");
    std::vector<std::string> literals;
    for (const auto& [id, literal] : plan->literal_tokens) {
      expect(id == id_of(pack, literal), "a literal's id is its token");
      literals.push_back(literal);
    }
    expect(literals == std::vector<std::string>{"<|tool_call>", "<tool_call|>", "<|\"|>"}, "the call's three tags are output as their text");
  }
  // a pack without Gemma's tool tags plans nothing
  auto bare = test_support::spm_tokenizer();
  for (auto& token : bare.tokens_by_id) {
    if (token == "<|\"|>") token = "<|quote_gone|>";
  }
  const auto refused = secure_frontend::make_gemma4_tool_generation_plan(request, bare, tools, rounds, 100000U, 64U, {}, true);
  expect(std::holds_alternative<secure_frontend::ChatGenerationPlanError>(refused), "a pack without the tags plans nothing");
  // the piece encoder and the Gemma 4 prompt encoder agree on a plain chat (the spans between markers)
  std::vector<secure_frontend::PromptPiece> plain{{secure_frontend::PromptPiece::Kind::marker, "<bos>"},
                                                  {secure_frontend::PromptPiece::Kind::marker, "<|turn>"},
                                                  {secure_frontend::PromptPiece::Kind::text, "user\nHi world\n\nthough"},
                                                  {secure_frontend::PromptPiece::Kind::marker, "<turn|>"},
                                                  {secure_frontend::PromptPiece::Kind::text, "\n"},
                                                  {secure_frontend::PromptPiece::Kind::marker, "<|turn>"},
                                                  {secure_frontend::PromptPiece::Kind::text, "model\n"}};
  const auto by_pieces = secure_frontend::encode_prompt_pieces(pack, plain);
  const auto by_gemma = secure_frontend::encode_gemma4_prompt(pack, "<bos><|turn>user\nHi world\n\nthough<turn|>\n<|turn>model\n");
  expect(std::holds_alternative<std::vector<std::size_t>>(by_pieces) && std::holds_alternative<std::vector<std::size_t>>(by_gemma) &&
             std::get<std::vector<std::size_t>>(by_pieces) == std::get<std::vector<std::size_t>>(by_gemma),
         "the piece encoder encodes a Gemma 4 prompt as encode_gemma4_prompt does");
}

void the_route_plans_gemma4_searches() {
  using secure_frontend::NativeChatProfile;
  using secure_frontend::ToolFormat;
  // which profiles' chats the route offers the tool, in which form
  expect(secure_frontend::search_tool_format(NativeChatProfile::gemma4) == std::optional<ToolFormat>{ToolFormat::gemma4} &&
             secure_frontend::search_tool_format(NativeChatProfile::qwen) == std::optional<ToolFormat>{ToolFormat::qwen} &&
             secure_frontend::search_tool_format(NativeChatProfile::glm53) == std::optional<ToolFormat>{ToolFormat::glm} &&
             secure_frontend::search_tool_format(NativeChatProfile::raon1) == std::optional<ToolFormat>{ToolFormat::raon1} &&
             secure_frontend::search_tool_format(NativeChatProfile::flint) == std::optional<ToolFormat>{ToolFormat::flint} &&
             // (2026-10-28, the owner: "deepseek does not seem to have mcp search access": DeepSeek's DSML form)
             secure_frontend::search_tool_format(NativeChatProfile::deepseek) == std::optional<ToolFormat>{ToolFormat::deepseek} &&
             !secure_frontend::search_tool_format(NativeChatProfile::minimax),
         "a Gemma 4 chat gets the tool in its own form, DeepSeek in DSML; MiniMax none yet");
  secure_frontend::ChatRouteConfig config{};
  config.max_prompt_tokens = 100000U;
  secure_frontend::ChatCompletionRequest request;
  request.model = "m";
  request.messages = {{"user", "Hi"}};
  const secure_frontend::SearchTurn turn{request, {{"web_search", {"query"}}, kTool}, true, {}, secure_frontend::ToolFormat::gemma4};
  const auto pack = test_support::spm_tokenizer();
  const auto planned = secure_frontend::search_first_plan(config, turn, pack);
  const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&planned);
  expect(plan != nullptr && plan->literal_tokens.size() == 3U && plan->server_stop_tokens.size() == 1U &&
             plan->prompt_token_ids.front() == id_of(pack, "<bos>"),
         "a Gemma 4 turn plans in Gemma 4's form");
}

// The route's first search plan over the REAL vocabulary (SECURE_FRONTEND_GEMMA4_PACK, the pack's tokenizer/ directory,
// never committed; unset, skipped), and the two encoders agreeing on it.
void the_real_pack_plans_its_search() {
  const char* configured = secure_frontend::crt_getenv("SECURE_FRONTEND_GEMMA4_PACK");
  if (configured == nullptr || *configured == '\0' || !std::filesystem::is_directory(std::filesystem::path{configured})) {
    std::cout << "SKIPPED: SECURE_FRONTEND_GEMMA4_PACK names no tokenizer pack directory\n";
    return;
  }
  const auto loaded = secure_frontend::load_local_tokenizer_pack(std::filesystem::path{configured}, secure_frontend::TokenizerMode::spm_bpe);
  const auto* pack = std::get_if<secure_frontend::TokenizerPack>(&loaded);
  expect(pack != nullptr, "the real pack loads");
  if (pack == nullptr) return;
  secure_frontend::ChatRouteConfig config{};
  config.max_prompt_tokens = 100000U;
  secure_frontend::ChatCompletionRequest request;
  request.model = "m";
  request.messages = {{"user", "what is today's headlines?"}};
  for (const bool thinking : {true, false}) {
    const secure_frontend::SearchTurn turn{request, {{"web_search", {"query"}}, kTool}, thinking, {}, secure_frontend::ToolFormat::gemma4};
    const auto planned = secure_frontend::search_first_plan(config, turn, *pack);
    const auto* refused = std::get_if<secure_frontend::ChatGenerationPlanError>(&planned);
    expect(refused == nullptr, std::string{thinking ? "thinking" : "not thinking"} + ": the search plan, not " +
                                   (refused != nullptr ? refused->message : std::string{}));
  }
  const auto text = secure_frontend::render_gemma4_text_prompt(request.messages, true);
  const std::vector<std::string> tools{kTool};
  const auto pieces = secure_frontend::render_gemma4_tool_prompt(request.messages, tools, {}, true);
  expect(text.has_value() && pieces.has_value(), "both prompts render");
  if (!text || !pieces) return;
  const auto whole = secure_frontend::encode_gemma4_prompt(*pack, *text);
  expect(std::holds_alternative<std::vector<std::size_t>>(whole), "the text prompt encodes");
  // the tool prompt's user turn and generation prompt encode as the text prompt's do
  const auto tool_ids = secure_frontend::encode_prompt_pieces(*pack, *pieces);
  const auto* ids = std::get_if<std::vector<std::size_t>>(&tool_ids);
  const auto* plain = std::get_if<std::vector<std::size_t>>(&whole);
  expect(ids != nullptr && plain != nullptr && ids->size() > plain->size() &&
             std::equal(plain->end() - 12, plain->end(), ids->end() - 12),
         "the conversation's ids end the same in both prompts");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"renders_the_templates_tool_prompts", renders_the_templates_tool_prompts},
      {"refuses_what_the_tool_prompt_cannot_say", refuses_what_the_tool_prompt_cannot_say},
      {"reads_calls_as_llama_cpp_does", reads_calls_as_llama_cpp_does},
      {"plans_with_its_tags", plans_with_its_tags},
      {"the_route_plans_gemma4_searches", the_route_plans_gemma4_searches},
      {"the_real_pack_plans_its_search", the_real_pack_plans_its_search},
  }, failures);
}
