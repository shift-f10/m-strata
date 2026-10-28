#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/chat_route.hpp"
#include "secure_frontend/chat_schema.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/subsystem_limits.hpp"
#include "support/config_json.hpp"
#include "support/test_support.hpp"
#include "support/test_tokenizer.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

void parses_minimal_text_request() {
  const auto request = secure_frontend::parse_chat_completion_request(
      R"({"model":"local-model.Q4_K_M","messages":[{"role":"system","content":"Be concise."},{"role":"user","content":"Hello"}],"max_tokens":42,"stream":false})");
  expect(request.has_value(), "valid text-only request should parse");
  if (!request) return;
  expect(request->model == "local-model.Q4_K_M", "model should be retained");
  expect(request->messages.size() == 2U, "messages should be retained");
  expect(request->messages[1].content == "Hello", "content should be retained");
  expect(request->max_tokens == 42U, "max_tokens should be retained");
}

// SP5 M1 Task 12 (C-9 carve-out; Ruling T12-b): the content ARRAY opens only for a model
// the composition proved can read images (vision_capable = vision.enabled AND the manifest's
// launch.mmproj). Without the flag today's 400 unsupported_parameter "content" stands
// verbatim; with it, text/image_url parts parse into ChatMessage.parts, the image keeps its
// data URL, and the caps fire BEFORE any decode: max_n on the count (vision.max_n) and the
// base64 LENGTH bound on each URL (32 MiB decoded = the pin's IMAGE_URL_MAX, :2474).
void image_url_parts_accepted_only_for_vision_models() {
  const std::string body =
      R"({"model":"m","messages":[{"role":"user","content":[)"
      R"({"type":"text","text":"look"},)"
      R"({"type":"image_url","image_url":{"url":"data:image/png;base64,YWJj"}}]}]})";
  const auto plain = secure_frontend::parse_chat_request(body);  // default limits: no capability
  const auto* plain_error = std::get_if<secure_frontend::ChatRequestError>(&plain);
  expect(plain_error != nullptr && plain_error->error == secure_frontend::ApiError::unsupported_parameter &&
             plain_error->param == "content",
         "without mmproj the today-400 on content arrays stands (C-9: the carve-out is capability-gated)");
  expect(!secure_frontend::parse_chat_completion_request(body), "the optional-form gate keeps rejecting arrays");

  secure_frontend::ChatSchemaLimits vision;
  vision.vision_capable = true;
  const auto parsed = secure_frontend::parse_chat_request(body, vision);
  const auto* request = std::get_if<secure_frontend::ChatCompletionRequest>(&parsed);
  if (request == nullptr) {
    const auto* error = std::get_if<secure_frontend::ChatRequestError>(&parsed);
    expect(false, "a vision model accepts the parts array (got " +
                      std::string{error == nullptr ? "nothing" : secure_frontend::api_error_row(error->error).code} + ")");
    return;
  }
  expect(request->messages.size() == 1U && request->messages[0].parts.size() == 2U, "both parts are retained");
  expect(request->messages[0].content == "look", "the text parts still drive the content string");
  expect(request->messages[0].parts[1].image_data_url == "data:image/png;base64,YWJj", "the data URL survives verbatim");
}

void image_part_caps_and_shape_are_checked_before_decode() {
  secure_frontend::ChatSchemaLimits vision;
  vision.vision_capable = true;
  const auto image = [](const std::string& url) { return "{\"type\":\"image_url\",\"image_url\":{\"url\":\"" + url + "\"}}"; };
  const auto body = [&](const std::string& parts) {
    return std::string(R"({"model":"m","messages":[{"role":"user","content":[)") + parts + R"(]}]})";
  };
  const auto named_image = [&](const char* expect_why, const std::string& url) {
    const auto result = secure_frontend::parse_chat_request(body(image(url)), vision);
    const auto* error = std::get_if<secure_frontend::ChatRequestError>(&result);
    expect(error != nullptr && error->error == secure_frontend::ApiError::unsupported_parameter &&
               error->param == "image_url",
           expect_why);
  };
  vision.max_images = 1U;
  {
    // Two data-URL images where one is allowed: refused by name, before any decode.
    const auto second = secure_frontend::parse_chat_request(
        body(image("data:image/png;base64,YWJj") + "," + image("data:image/png;base64,YWJk")), vision);
    const auto* second_error = std::get_if<secure_frontend::ChatRequestError>(&second);
    expect(second_error != nullptr && second_error->error == secure_frontend::ApiError::unsupported_parameter &&
               second_error->param == "image_url",
           "max_n: a second image in one request is refused by name");
  }
  const auto two = secure_frontend::parse_chat_request(
      body(image("data:image/png;base64,YWJj") + "," + image("data:image/png;base64,YWJk")), vision);
  const auto* two_error = std::get_if<secure_frontend::ChatRequestError>(&two);
  expect(two_error != nullptr && two_error->error == secure_frontend::ApiError::unsupported_parameter &&
             two_error->param == "image_url",
         "over max_n names image_url");
  vision.max_images = 8U;
  secure_frontend::ChatSchemaLimits tiny_b64 = vision;
  tiny_b64.max_image_b64_chars = 8U;
  const auto long_url = "data:image/png;base64," + std::string(64U, 'A');
  const auto length_result = secure_frontend::parse_chat_request(body(image(long_url)), tiny_b64);
  const auto* length_error = std::get_if<secure_frontend::ChatRequestError>(&length_result);
  expect(length_error != nullptr && length_error->error == secure_frontend::ApiError::unsupported_parameter &&
             length_error->param == "image_url",
         "the base64 LENGTH cap fires first (never decoded, nothing staged)");
  // Shape strictness: unknown part keys, a missing/odd image_url member, and an empty array
  // are plain invalid_request — the plan's parts vocabulary is closed.
  for (const auto& bad : {body(R"({"type":"text","text":"a","extra":1})"),
                                body(R"({"type":"image_url","image_url":{}})"),
                                body(R"({"type":"image_url","image_url":"data:image/png;base64,YWJj"})"),
                                body(R"({"type":"file"})"),
                                std::string{"[]"},
                                body(R"({"type":"text"})")}) {
    expect(!std::holds_alternative<secure_frontend::ChatCompletionRequest>(secure_frontend::parse_chat_request(bad, vision)),
           "closed part vocabulary: " + bad);
  }
  // An http(s) URL is out for M1 (Ruling T12-a): the same fixed 400 naming image_url.
  named_image("http(s) image URLs are not fetched in M1", "https://example.test/a.png");
}

void rejects_unsupported_and_unsafe_features() {
  for (const std::string json : {
           R"({"model":"local","messages":[{"role":"user","content":"hi"}],"stream":"true"})",
           R"({"model":"local","messages":[{"role":"user","content":"hi"}],"stream":true,"stream_options":{"include_usage":"yes"}}"),
           R"({"model":"local","messages":[{"role":"user","content":"hi"}],"tools":[]})",
           R"({"model":"local","messages":[{"role":"user","content":[{"type":"image_url"}]]})",
           R"({"model":"mcp://remote","messages":[{"role":"user","content":"hi"}]})",
           R"({"model":"local","messages":[{"role":"tool","content":"hi"}]})",
           R"({"model":"local","messages":[{"role":"user","content":"hi","mcp":"x"}]})",
           R"({"model":"local","messages":[{"role":"user"}]})",
           R"({"model":"local","messages":[],"max_tokens":-1})",
       }) {
    expect(!secure_frontend::parse_chat_completion_request(json), "unsafe or unsupported request must fail closed");
  }
}

void rejects_duplicate_and_malformed_json() {
  expect(!secure_frontend::parse_chat_completion_request(
             R"({"model":"a","model":"b","messages":[{"role":"user","content":"x"}]})"),
         "duplicate keys must fail closed");
  expect(!secure_frontend::parse_chat_completion_request(
             R"({"model":"a","messages":[{"role":"user","content":"\ud800"}]})"),
         "unpaired UTF-16 surrogates must be rejected");
  expect(!secure_frontend::parse_chat_completion_request("{not json}"), "malformed JSON must fail closed");
}

void renders_escaped_minimal_response() {
  const auto request = secure_frontend::parse_chat_completion_request(
      R"({"model":"local","messages":[{"role":"user","content":"hi"}]})");
  expect(request.has_value(), "render fixture should parse");
  if (!request) return;
  const auto response = secure_frontend::render_chat_completion_response(*request, "quote \" slash \\ newline\n");
  expect(response.has_value(), "safe generated text should render");
  if (response) {
    expect(response->find("\"content\":\"quote \\\" slash \\\\ newline\\n\"") != std::string::npos,
           "generated text must be JSON escaped rather than interpolated");
    expect(response->find("\"stream\"") == std::string::npos, "response must be non-streaming only");
  }
}
// SP3 Task 15 (spec §3.5): the non-streaming OpenAI completion shape, exact bytes.
void renders_openai_completion_shape() {
  const auto body = secure_frontend::render_chat_completion({.id = "chatcmpl-0123456789abcdef0123456789abcdef", .created = 1791963785,
                                                             .model = "fake-model", .text = "Hi \"there\"\n", .finish_reason = "length",
                                                             .prompt_tokens = 7U, .completion_tokens = 3U});
  expect(body == std::optional<std::string>{
                      "{\"id\":\"chatcmpl-0123456789abcdef0123456789abcdef\",\"object\":\"chat.completion\",\"created\":1791963785,"
                      "\"model\":\"fake-model\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"Hi \\\"there\\\"\\n\"},"
                      "\"logprobs\":null,\"finish_reason\":\"length\"}],\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3,\"total_tokens\":10}}\n"},
         "spec §3.5 shape: " + body.value_or("<nullopt>"));
  expect(!secure_frontend::render_chat_completion({.id = "x", .created = 0, .model = "m", .text = "\xff", .finish_reason = "stop",
                                                   .prompt_tokens = 0U, .completion_tokens = 0U}).has_value(), "invalid UTF-8 is refused");
}

// Issue 19: a Qwen3 think block comes back as message.reasoning_content beside the
// answer, written only when non-empty (upstream serve/server.py:5511-5513).
void renders_reasoning_content_beside_the_answer() {
  const auto body = secure_frontend::render_chat_completion({.id = "x", .created = 0, .model = "m", .text = "Paris.", .finish_reason = "stop",
                                                             .prompt_tokens = 1U, .completion_tokens = 2U,
                                                             .reasoning = "The capital\nis Paris."});
  expect(body.has_value() && body->find(R"("message":{"role":"assistant","content":"Paris.","reasoning_content":"The capital\nis Paris."})") !=
                                 std::string::npos,
         "reasoning_content follows content in the message: " + body.value_or("<nullopt>"));
  const auto plain = secure_frontend::render_chat_completion({.id = "x", .created = 0, .model = "m", .text = "Paris.", .finish_reason = "stop",
                                                              .prompt_tokens = 1U, .completion_tokens = 2U});
  expect(plain.has_value() && plain->find("reasoning_content") == std::string::npos, "no reasoning, no field");
  expect(!secure_frontend::render_chat_completion({.id = "x", .created = 0, .model = "m", .text = "ok", .finish_reason = "stop",
                                                   .prompt_tokens = 0U, .completion_tokens = 0U, .reasoning = "\xff"}).has_value(),
         "invalid UTF-8 reasoning is refused");
}

// Web search (spec 2026-10-27-web-search-design §2): the search loop's events go back as message.strata_search, as
// upstream's openai_collect keeps its strata_mcp items; written only when there are any.
void renders_the_search_events_in_the_message() {
  const std::vector<std::string> events{R"({"event":"call","round":1})", R"({"event":"limit","max_rounds":8})"};
  const auto body = secure_frontend::render_chat_completion({.id = "x", .created = 0, .model = "m", .text = "Paris.", .finish_reason = "stop",
                                                             .prompt_tokens = 1U, .completion_tokens = 2U, .reasoning = "r",
                                                             .search_events = events});
  expect(body.has_value() && body->find(R"("content":"Paris.","reasoning_content":"r","strata_search":[{"event":"call","round":1},)"
                                        R"({"event":"limit","max_rounds":8}]})") != std::string::npos,
         "strata_search after reasoning_content: " + body.value_or("<nullopt>"));
  const auto plain = secure_frontend::render_chat_completion({.id = "x", .created = 0, .model = "m", .text = "ok", .finish_reason = "stop",
                                                              .prompt_tokens = 1U, .completion_tokens = 2U});
  expect(plain.has_value() && plain->find("strata_search") == std::string::npos, "no search, no field");
}

}  // namespace

// Session log step 14 (the page's Thinking control; upstream's per-request reasoning_effort,
// serve/web/app.js:802): the qwen profile takes reasoning_effort per request in the
// reasoning.effort words — none, low, medium, high — and a profile without it (GLM) refuses
// it by name, never ignores it.
void reasoning_effort_per_request() {
  using secure_frontend::ChatCompletionRequest;
  using secure_frontend::ChatRequestError;
  secure_frontend::ChatSchemaLimits qwen;
  qwen.per_request_reasoning_effort = true;
  const auto body = [](const std::string& value) {
    return R"({"model":"local","messages":[{"role":"user","content":"hi"}],"reasoning_effort":)" + value + "}";
  };
  for (const char* word : {"none", "low", "medium", "high"}) {
    const auto result = secure_frontend::parse_chat_request(body("\"" + std::string{word} + "\""), qwen);
    const auto* request = std::get_if<ChatCompletionRequest>(&result);
    expect(request != nullptr && request->reasoning_effort == std::optional<std::string>{word},
           std::string{"accepted and kept: "} + word);
  }
  const auto absent = secure_frontend::parse_chat_request(R"({"model":"local","messages":[{"role":"user","content":"hi"}]})", qwen);
  const auto* plain = std::get_if<ChatCompletionRequest>(&absent);
  expect(plain != nullptr && !plain->reasoning_effort.has_value(), "absent: the server's reasoning.effort applies");
  for (const std::string bad : {"\"max\"", "\"xhigh\"", "\"\"", "\"NONE\"", "1", "true", "null"}) {
    const auto result = secure_frontend::parse_chat_request(body(bad), qwen);
    const auto* error = std::get_if<ChatRequestError>(&result);
    expect(error != nullptr && error->error == secure_frontend::ApiError::invalid_request && error->param == "reasoning_effort",
           "refused, naming the key: " + bad);
  }
  const secure_frontend::ChatSchemaLimits glm;  // per_request_reasoning_effort stays false
  const auto refused = secure_frontend::parse_chat_request(body("\"none\""), glm);
  const auto* error = std::get_if<ChatRequestError>(&refused);
  expect(error != nullptr && error->error == secure_frontend::ApiError::unsupported_parameter && error->param == "reasoning_effort",
         "a profile without it refuses it by name");
}

// Owner 2026-10-28 (a web search checkbox in the chat): "strata_search": false asks for no search in this request; true
// or absent leaves it to the server and the card. Any other value is refused by name.
void strata_search_per_request() {
  using secure_frontend::ChatCompletionRequest;
  using secure_frontend::ChatRequestError;
  const auto body = [](const std::string& value) {
    return R"({"model":"local","messages":[{"role":"user","content":"hi"}],"strata_search":)" + value + "}";
  };
  for (const auto& [value, want] : std::vector<std::pair<std::string, bool>>{{"false", false}, {"true", true}}) {
    const auto result = secure_frontend::parse_chat_request(body(value), {});
    const auto* request = std::get_if<ChatCompletionRequest>(&result);
    expect(request != nullptr && request->strata_search == std::optional<bool>{want}, "kept: " + value);
  }
  const auto absent = secure_frontend::parse_chat_request(R"({"model":"local","messages":[{"role":"user","content":"hi"}]})", {});
  const auto* plain = std::get_if<ChatCompletionRequest>(&absent);
  expect(plain != nullptr && !plain->strata_search.has_value(), "absent: the server decides");
  for (const std::string bad : {"0", "\"no\"", "null", "{}"}) {
    const auto result = secure_frontend::parse_chat_request(body(bad), {});
    const auto* error = std::get_if<ChatRequestError>(&result);
    expect(error != nullptr && error->error == secure_frontend::ApiError::invalid_request && error->param == "strata_search",
           "refused, naming the key: " + bad);
  }
}

// Session log step 15 (owner bug: "invalid_request after first answer"): SP3 §6.2 lets
// remote-looking content be "rejected or treated as text"; SP0 rejected it anywhere in a
// message, so a model answer with a link, or prose such as "Training data:", made the next
// turn a 400. Text content is now text, verbatim (the server never fetches it); the model
// field and image URLs keep their guards.
void remote_looking_text_is_text() {
  using secure_frontend::ChatCompletionRequest;
  const secure_frontend::ChatSchemaLimits limits;
  for (const std::string text : {"https://example.test/a", "Training data: examples the model learns from.",
                                 "see file:///etc/passwd and mcp://x", "data:text/plain,x", "HTTP://EXAMPLE.COM"}) {
    const std::string json = R"({"model":"local","messages":[{"role":"system","content":")" + text +
                             R"("},{"role":"user","content":"hi"},{"role":"assistant","content":")" + text +
                             R"("},{"role":"user","content":")" + text + R"("}]})";
    const auto result = secure_frontend::parse_chat_request(json, limits);
    const auto* request = std::get_if<ChatCompletionRequest>(&result);
    expect(request != nullptr && request->messages.size() == 4U && request->messages[0].content == text &&
               request->messages[2].content == text && request->messages[3].content == text,
           "kept verbatim as text in every role: " + text);
  }
  expect(!secure_frontend::parse_chat_completion_request(R"({"model":"mcp://remote","messages":[{"role":"user","content":"hi"}]})"),
         "the model field keeps its remote-scheme guard");
}

// ---- Gemma 4 plan 1 Task 10 (spec §6.6, D10): route plan selection and the thinking default ----

std::shared_ptr<const secure_frontend::FrontendConfig> config_with(const std::map<std::string, std::string>& extra) {
  secure_frontend_test::ConfigFixture f;
  auto literals = f.literals;
  for (const auto& [k, v] : extra) literals[k] = v;
  auto draft = secure_frontend::parse_config_text(secure_frontend_test::render_config_json(literals), f.dir);
  auto result = secure_frontend::validate(std::get<secure_frontend::FrontendConfigDraft>(draft));
  return std::get<secure_frontend::ValidatedConfig>(result).shared();
}

const std::string kEffortBody = R"({"model":"local","messages":[{"role":"user","content":"hi"}],"reasoning_effort":"low"})";

// The schema limits a gemma4 config derives admit reasoning_effort per request (none = off,
// low/medium/high = on); glm53's still refuse it by name (unchanged).
void reasoning_effort_per_request_on_gemma4() {
  const auto gemma = secure_frontend::chat_schema_limits(*config_with({{"engine.profile", "\"gemma4\""}}));
  expect(gemma.per_request_reasoning_effort, "a gemma4 config's schema limits admit reasoning_effort");
  for (const char* word : {"none", "low", "medium", "high"}) {
    const auto result = secure_frontend::parse_chat_request(
        R"({"model":"local","messages":[{"role":"user","content":"hi"}],"reasoning_effort":")" + std::string{word} + "\"}", gemma);
    expect(std::holds_alternative<secure_frontend::ChatCompletionRequest>(result), std::string{"gemma4 admits "} + word);
  }
  const auto glm = secure_frontend::chat_schema_limits(*config_with({{"engine.profile", "\"glm53\""}}));
  const auto refused = secure_frontend::parse_chat_request(kEffortBody, glm);
  const auto* error = std::get_if<secure_frontend::ChatRequestError>(&refused);
  expect(error != nullptr && error->error == secure_frontend::ApiError::unsupported_parameter && error->param == "reasoning_effort",
         "glm53 still refuses reasoning_effort by name");
  expect(secure_frontend::chat_schema_limits(*config_with({{"engine.profile", "\"qwen\""}})).per_request_reasoning_effort,
         "qwen keeps it");
}

std::vector<std::size_t> encoded(const secure_frontend::TokenizerPack& pack, const std::string_view text) {
  const auto result = secure_frontend::encode_text(pack, text);
  const auto* ids = std::get_if<std::vector<std::size_t>>(&result);
  return ids != nullptr ? *ids : std::vector<std::size_t>{};
}

bool ends_with(const std::vector<std::size_t>& ids, const std::vector<std::size_t>& tail) {
  return ids.size() >= tail.size() && std::equal(tail.begin(), tail.end(), ids.end() - static_cast<std::ptrdiff_t>(tail.size()));
}

secure_frontend::ChatGenerationPlanResult route_plan(const secure_frontend::NativeChatProfile profile,
                                                      const secure_frontend::ChatCompletionRequest& request,
                                                      const secure_frontend::TokenizerPack& pack, const bool thinking) {
  return secure_frontend::select_chat_generation_plan(profile, request, pack, 4096U, secure_frontend::SamplingDefaults{}, {},
                                                      thinking, secure_frontend::QwenEffortPlacement{secure_frontend::QwenEffort::high, false});
}

// D10: with neither the request's reasoning_effort nor reasoning.effort, Gemma does not think,
// and the GEN prompt ends in the closed thought channel; none is off, low/medium/high are on.
void gemma4_thinking_defaults_to_off() {
  expect(!secure_frontend::gemma4_thinking_default(*config_with({{"engine.profile", "\"gemma4\""}})),
         "unset reasoning.effort: thinking off (the template's default)");
  expect(!secure_frontend::gemma4_thinking_default(*config_with({{"engine.profile", "\"gemma4\""}, {"reasoning.effort", "\"none\""}})),
         "reasoning.effort none: off");
  for (const char* level : {"low", "medium", "high"}) {
    expect(secure_frontend::gemma4_thinking_default(
               *config_with({{"engine.profile", "\"gemma4\""}, {"reasoning.effort", "\"" + std::string{level} + "\""}})),
           std::string{"reasoning.effort "} + level + ": on");
  }
  expect(!secure_frontend::gemma4_request_thinks(std::nullopt, false), "neither set: off");
  expect(secure_frontend::gemma4_request_thinks(std::nullopt, true), "the server's level applies when the request names none");
  expect(!secure_frontend::gemma4_request_thinks(std::string{"none"}, true), "the request's none wins: off");
  for (const char* level : {"low", "medium", "high"}) {
    expect(secure_frontend::gemma4_request_thinks(std::string{level}, false), std::string{"the request's "} + level + " wins: on");
  }
  const auto pack = test_support::spm_tokenizer();
  const secure_frontend::ChatCompletionRequest request{"local", {{"user", "Hi"}}, 2U};
  const auto off = route_plan(secure_frontend::NativeChatProfile::gemma4, request, pack, false);
  const auto* off_plan = std::get_if<secure_frontend::ChatGenerationPlan>(&off);
  std::vector<std::size_t> closed{test_support::token_id(pack, "<|channel>")};
  for (const auto id : encoded(pack, "thought\n")) closed.push_back(id);
  closed.push_back(test_support::token_id(pack, "<channel|>"));
  expect(off_plan != nullptr && ends_with(off_plan->prompt_token_ids, closed),
         "thinking off: the GEN prompt ends <|channel>thought\\n<channel|>");
  const auto on = route_plan(secure_frontend::NativeChatProfile::gemma4, request, pack, true);
  const auto* on_plan = std::get_if<secure_frontend::ChatGenerationPlan>(&on);
  const auto think = test_support::token_id(pack, "<|think|>");
  expect(on_plan != nullptr && !ends_with(on_plan->prompt_token_ids, closed) &&
             std::find(on_plan->prompt_token_ids.begin(), on_plan->prompt_token_ids.end(), think) != on_plan->prompt_token_ids.end(),
         "thinking on: <|think|> in the system turn, the channel left to the model");
}

// The route's plan is chosen by an exhaustive switch: a gemma4 route never plans with the
// GLM or qwen renderer, and never takes images.
void gemma4_route_never_takes_another_profiles_plan() {
  using secure_frontend::NativeChatProfile;
  auto glm = test_support::byte_tokenizer();
  for (const char* marker : {"[gMASK]", "<sop>", "<|system|>", "<|user|>", "<|assistant|>", "<think>"}) {
    glm.tokens_by_id.emplace_back(marker);
    glm.token_types.push_back(3);
  }
  const secure_frontend::ChatCompletionRequest request{"local", {{"user", "Hi"}}, 2U};
  expect(std::holds_alternative<secure_frontend::ChatGenerationPlan>(route_plan(NativeChatProfile::glm53, request, glm, false)),
         "control: the GLM pack plans under glm53");
  expect(std::holds_alternative<secure_frontend::ChatGenerationPlan>(
             route_plan(NativeChatProfile::qwen, request, test_support::byte_tokenizer(), true)),
         "control: the ChatML pack plans under qwen");
  expect(std::holds_alternative<secure_frontend::ChatGenerationPlanError>(route_plan(NativeChatProfile::gemma4, request, glm, false)),
         "gemma4 with a GLM pack: no plan (never the GLM renderer)");
  expect(std::holds_alternative<secure_frontend::ChatGenerationPlanError>(
             route_plan(NativeChatProfile::gemma4, request, test_support::byte_tokenizer(), true)),
         "gemma4 with a ChatML pack: no plan (never the qwen renderer)");
  const auto pack = test_support::spm_tokenizer();
  const auto gemma = route_plan(NativeChatProfile::gemma4, request, pack, false);
  const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&gemma);
  expect(plan != nullptr && !plan->prompt_token_ids.empty() && plan->prompt_token_ids.front() == test_support::token_id(pack, "<bos>"),
         "gemma4 with its own pack: the Gemma plan (starts with <bos>)");
  const std::vector<std::size_t> one_image{4U};
  const auto images = secure_frontend::select_chat_generation_plan(NativeChatProfile::gemma4, request, pack, 4096U,
                                                                   secure_frontend::SamplingDefaults{}, one_image, false,
                                                                   secure_frontend::QwenEffortPlacement{secure_frontend::QwenEffort::high, false});
  expect(std::holds_alternative<secure_frontend::ChatGenerationPlanError>(images), "gemma4 never takes image answers (D2)");
}

// DeepSeek-V4.1 (spec 2026-10-27-strata-prepare-design §7.2): the route's thinking decision and level become DeepSeek's
// effort - low 50, medium and high 75, thinking off chat mode - and its plan is the deepseek one on a DeepSeek pack.
void deepseek_route_plan_follows_thinking_and_its_level() {
  using secure_frontend::NativeChatProfile;
  using secure_frontend::QwenEffort;
  auto pack = test_support::byte_tokenizer();
  for (std::size_t i = 0U; i < secure_frontend::kDeepseekMarkers.size(); ++i) {
    pack.tokens_by_id.emplace_back(secure_frontend::kDeepseekMarkers[i]);
    pack.token_types.push_back(i == 4U || i == 5U ? 4 : 3);  // the think pair USER_DEFINED
  }
  pack.mode = secure_frontend::TokenizerMode::deepseek_bpe;
  const secure_frontend::ChatCompletionRequest request{"local", {{"user", "Hi"}}, 2U};
  const auto ids = [](const secure_frontend::ChatGenerationPlanResult& result) {
    const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&result);
    return plan != nullptr ? plan->prompt_token_ids : std::vector<std::size_t>{};
  };
  const auto routed = [&](const bool thinking, const QwenEffort level) {
    return ids(secure_frontend::select_chat_generation_plan(NativeChatProfile::deepseek, request, pack, 4096U,
                                                            secure_frontend::SamplingDefaults{}, {}, thinking,
                                                            secure_frontend::QwenEffortPlacement{level, false}));
  };
  const auto direct = [&](const std::optional<int> effort) {
    return ids(secure_frontend::make_deepseek_chat_generation_plan(request, pack, 4096U, 1U, {}, effort));
  };
  expect(!direct(75).empty() && routed(true, QwenEffort::high) == direct(75), "thinking at high: effort 75");
  expect(routed(true, QwenEffort::medium) == direct(75), "thinking at medium: effort 75 (Maya's high)");
  expect(!direct(50).empty() && routed(true, QwenEffort::low) == direct(50), "thinking at low: effort 50");
  expect(!direct(std::nullopt).empty() && routed(false, QwenEffort::high) == direct(std::nullopt), "thinking off: chat mode");
}

// MiniMax-M3.1 (owner 2026-10-28): a request's reasoning_effort in Maya's words (none: thinking off; low, medium,
// high: MiniMax's <effort>, Maya keeps the client's own word), else the card's default (reasoning.effort; unset:
// MiniMax's adaptive thinking with no effort named); the route's plan is the minimax one with that thinking.
void minimax_route_plan_follows_thinking_and_its_level() {
  using secure_frontend::MinimaxThinking;
  const auto thinks = [](const std::optional<std::string>& asked, const std::optional<std::string>& fallback) {
    return secure_frontend::minimax_request_thinking(asked, fallback);
  };
  const auto same = [](const MinimaxThinking& a, const MinimaxThinking& b) { return a.off == b.off && a.effort == b.effort; };
  expect(same(thinks(std::nullopt, std::nullopt), {}), "nothing named: adaptive, no effort");
  expect(same(thinks("none", "high"), {.off = true}), "none: thinking off");
  expect(same(thinks("medium", std::nullopt), {.effort = "medium"}), "medium stays medium (Maya's effort_name)");
  expect(same(thinks(std::nullopt, "low"), {.effort = "low"}) && same(thinks(std::nullopt, "none"), {.off = true}),
         "the card's default when the request names none");
  auto pack = test_support::byte_tokenizer();
  for (const std::string_view literal : {"]~!b[", "]~b]", "[e~[", "<mm:think>", "</mm:think>"}) {
    pack.tokens_by_id.emplace_back(literal);
    pack.token_types.push_back(literal.front() == '<' ? 4 : 3);
  }
  pack.mode = secure_frontend::TokenizerMode::minimax_bpe;
  const secure_frontend::ChatCompletionRequest request{"local", {{"user", "Hi"}}, 2U};
  const auto ids = [](const secure_frontend::ChatGenerationPlanResult& result) {
    const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&result);
    return plan != nullptr ? plan->prompt_token_ids : std::vector<std::size_t>{};
  };
  for (const MinimaxThinking& thinking : {MinimaxThinking{}, MinimaxThinking{.off = true}, MinimaxThinking{.effort = "high"}}) {
    const auto routed = ids(secure_frontend::select_chat_generation_plan(secure_frontend::NativeChatProfile::minimax, request, pack, 4096U,
                                                                         secure_frontend::SamplingDefaults{}, {}, !thinking.off, {},
                                                                         thinking));
    const auto direct = ids(secure_frontend::make_minimax_chat_generation_plan(request, pack, 4096U, 1U, {}, thinking));
    expect(!direct.empty() && routed == direct, "the route takes the minimax plan with the request's thinking");
  }
}

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"minimax_route_plan_follows_thinking_and_its_level", minimax_route_plan_follows_thinking_and_its_level},
      {"reasoning_effort_per_request_on_gemma4", reasoning_effort_per_request_on_gemma4},
      {"gemma4_thinking_defaults_to_off", gemma4_thinking_defaults_to_off},
      {"deepseek_route_plan_follows_thinking_and_its_level", deepseek_route_plan_follows_thinking_and_its_level},
      {"gemma4_route_never_takes_another_profiles_plan", gemma4_route_never_takes_another_profiles_plan},
      {"remote_looking_text_is_text", remote_looking_text_is_text},
      {"reasoning_effort_per_request", reasoning_effort_per_request},
      {"strata_search_per_request", strata_search_per_request},
      {"parses_minimal_text_request", parses_minimal_text_request},
      {"image_url_parts_accepted_only_for_vision_models", image_url_parts_accepted_only_for_vision_models},
      {"image_part_caps_and_shape_are_checked_before_decode", image_part_caps_and_shape_are_checked_before_decode},
      {"rejects_unsupported_and_unsafe_features", rejects_unsupported_and_unsafe_features},
      {"rejects_duplicate_and_malformed_json", rejects_duplicate_and_malformed_json},
      {"renders_escaped_minimal_response", renders_escaped_minimal_response},
      {"renders_openai_completion_shape", renders_openai_completion_shape},
      {"renders_reasoning_content_beside_the_answer", renders_reasoning_content_beside_the_answer},
      {"renders_the_search_events_in_the_message", renders_the_search_events_in_the_message},
  }, failures);
}
