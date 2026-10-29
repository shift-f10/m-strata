// Gemma 4 plan 1 Task 8 (spec §2.5, §6.3): the fixed Gemma 4 text template, its trim, the marker
// encoder and the generation plan. The expected bytes are written from the GGUF's template text
// (tokenizer.chat_template, lines 141-266), not from a Jinja run (jinja2 is not in .venv-dev).
#include "secure_frontend/chat_generation_plan.hpp"
#include "secure_frontend/gemma4_prompt_renderer.hpp"
#include "secure_frontend/sampling_params.hpp"
#include "secure_frontend/tokenizer_pack.hpp"
#include "support/test_support.hpp"
#include "support/test_tokenizer.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
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

using Messages = std::vector<secure_frontend::ChatMessage>;

std::string shown(const std::optional<std::string>& prompt) { return prompt.has_value() ? *prompt : std::string{"<refused>"}; }

std::string hex(const std::uint32_t point) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string out;
  for (int shift = 12; shift >= 0; shift -= 4) out.push_back(kDigits[(point >> static_cast<unsigned>(shift)) & 0xFU]);
  return "U+" + out;
}

std::string utf8(const std::uint32_t point) {
  std::string out;
  if (point < 0x80U) {
    out.push_back(static_cast<char>(point));
  } else if (point < 0x800U) {
    out.push_back(static_cast<char>(0xC0U | (point >> 6U)));
    out.push_back(static_cast<char>(0x80U | (point & 0x3FU)));
  } else if (point < 0x10000U) {
    out.push_back(static_cast<char>(0xE0U | (point >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (point & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (point >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((point >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (point & 0x3FU)));
  }
  return out;
}

// The 24 CONTROL/USER_DEFINED literals of the Gemma 4 vocabulary (the owner's GGUF, read by the
// Task 6 pack tool: ids 0-4, 46-52, 98, 100, 101, 105, 106, 255999, 256000, 258880-258884).
const std::vector<std::string>& gemma4_literals() {
  static const std::vector<std::string> literals{
      "<pad>",        "<eos>",        "<bos>",          "<unk>",           "<mask>",     "<|tool>",
      "<tool|>",      "<|tool_call>", "<tool_call|>",   "<|tool_response>", "<tool_response|>", "<|\"|>",
      "<|think|>",    "<|channel>",   "<channel|>",     "<|turn>",         "<turn|>",    "<|image>",
      "<|audio>",     "<|image|>",    "<|audio|>",      "<image|>",        "<audio|>",   "<|video|>"};
  return literals;
}

void renders_the_template_bytes_thinking_off() {
  expect(shown(secure_frontend::render_gemma4_text_prompt({{"user", "Hi"}}, false)) ==
             "<bos><|turn>user\nHi<turn|>\n<|turn>model\n<|channel>thought\n<channel|>",
         "no system message and thinking off: no system turn, the closed thought channel last");
  const Messages history{{"system", "Be brief."}, {"user", "Hi"}, {"assistant", "Hello."}, {"user", "Bye"}};
  expect(shown(secure_frontend::render_gemma4_text_prompt(history, false)) ==
             "<bos><|turn>system\nBe brief.<turn|>\n<|turn>user\nHi<turn|>\n<|turn>model\nHello.<turn|>\n"
             "<|turn>user\nBye<turn|>\n<|turn>model\n<|channel>thought\n<channel|>",
         "a system turn, user turns and earlier answers as model turns: " +
             shown(secure_frontend::render_gemma4_text_prompt(history, false)));
  const Messages padded{{"system", "  Be brief.\n"}, {"user", "\nline one\n\n  line two  "}};
  expect(shown(secure_frontend::render_gemma4_text_prompt(padded, false)) ==
             "<bos><|turn>system\nBe brief.<turn|>\n<|turn>user\nline one\n\n  line two<turn|>\n<|turn>model\n"
             "<|channel>thought\n<channel|>",
         "every text is trimmed at both ends, its inner whitespace kept");
  const Messages answered{{"user", "Hi"}, {"assistant", "  Hello.\n\n"}, {"user", " Bye"}};
  expect(shown(secure_frontend::render_gemma4_text_prompt(answered, false)) ==
             "<bos><|turn>user\nHi<turn|>\n<|turn>model\nHello.<turn|>\n<|turn>user\nBye<turn|>\n<|turn>model\n"
             "<|channel>thought\n<channel|>",
         "strip_thinking trims an earlier answer too");
}

void renders_the_template_bytes_thinking_on() {
  // the newer template (owner 2026-10-28): <|think|> and a newline
  expect(shown(secure_frontend::render_gemma4_text_prompt({{"user", "Hi"}}, true)) ==
             "<bos><|turn>system\n<|think|>\n<turn|>\n<|turn>user\nHi<turn|>\n<|turn>model\n",
         "thinking on without a system message: a system turn holding only <|think|> and its newline, an open generation prompt");
  const Messages history{{"system", " Be brief. "}, {"user", "Hi"}, {"assistant", "Hello."}, {"user", "Bye"}};
  expect(shown(secure_frontend::render_gemma4_text_prompt(history, true)) ==
             "<bos><|turn>system\n<|think|>\nBe brief.<turn|>\n<|turn>user\nHi<turn|>\n<|turn>model\nHello.<turn|>\n"
             "<|turn>user\nBye<turn|>\n<|turn>model\n",
         "<|think|> first in the system turn, then a newline and the trimmed system text: " +
             shown(secure_frontend::render_gemma4_text_prompt(history, true)));
}

void trims_like_jinja() {
  // CPython 3.12.10's str.isspace() set (computed on this box, 2026-10-22: every code point
  // 0..0x10FFFF with chr(c).isspace()); Jinja's trim is str.strip().
  const std::vector<std::uint32_t> whitespace{0x09,   0x0A,   0x0B,   0x0C,   0x0D,   0x1C,   0x1D,   0x1E,
                                              0x1F,   0x20,   0x85,   0xA0,   0x1680, 0x2000, 0x2001, 0x2002,
                                              0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008, 0x2009, 0x200A,
                                              0x2028, 0x2029, 0x202F, 0x205F, 0x3000};
  std::string all;
  for (const auto point : whitespace) {
    const std::string space = utf8(point);
    all += space;
    const std::string text = space + space + "a" + space + "b" + space;
    expect(secure_frontend::gemma4_trim(text) == "a" + space + "b",
           hex(point) + " is trimmed at both ends and kept inside");
  }
  expect(secure_frontend::gemma4_trim(all + "x" + all) == "x", "every whitespace code point in one run, both ends");
  expect(secure_frontend::gemma4_trim(all).empty() && secure_frontend::gemma4_trim("").empty(), "all-whitespace trims to nothing");
  // Not whitespace for Python: zero-width space, BOM, Mongolian vowel separator, word joiner,
  // soft hyphen, the C1 neighbours of NEL, ESC, and the C0 neighbours of the trimmed run.
  for (const std::uint32_t point : {0x200BU, 0xFEFFU, 0x180EU, 0x2060U, 0xADU, 0x84U, 0x86U, 0x1BU, 0x08U, 0x0EU, 0x21U}) {
    const std::string kept = utf8(point) + "a" + utf8(point);
    expect(secure_frontend::gemma4_trim(kept) == kept, hex(point) + " is not whitespace and stays");
  }
  expect(secure_frontend::gemma4_trim("\xE3\x80\x80\xE3\x80") == "\xE3\x80",  // never reached: the renderer validates UTF-8 first
         "an incomplete trailing sequence is not taken for whitespace");

  const Messages wide{{"user", "\xC2\xA0\xE3\x80\x80 Hi\xE3\x80\x80there \xE2\x80\xA8\xE2\x80\xA9"}};
  expect(shown(secure_frontend::render_gemma4_text_prompt(wide, false)) ==
             "<bos><|turn>user\nHi\xE3\x80\x80there<turn|>\n<|turn>model\n<|channel>thought\n<channel|>",
         "the renderer trims U+00A0, U+3000, U+2028, U+2029 and keeps the inner U+3000");
  const Messages nel{{"system", "\xC2\x85sys\xC2\x85"}, {"user", "\xE1\x9A\x80u\xE2\x80\xAF"}};
  expect(shown(secure_frontend::render_gemma4_text_prompt(nel, true)) ==
             "<bos><|turn>system\n<|think|>\nsys<turn|>\n<|turn>user\nu<turn|>\n<|turn>model\n",
         "NEL, U+1680 and U+202F are trimmed in the system and user turns");
  const Messages zero_width{{"user", "\xE2\x80\x8BHi\xEF\xBB\xBF"}};
  expect(shown(secure_frontend::render_gemma4_text_prompt(zero_width, false)) ==
             "<bos><|turn>user\n\xE2\x80\x8BHi\xEF\xBB\xBF<turn|>\n<|turn>model\n<|channel>thought\n<channel|>",
         "a zero-width space and a BOM are text, as in Python");
}

void refuses_markers_order_and_empty_text() {
  const auto& literals = secure_frontend::gemma4_control_literals();
  const std::set<std::string> listed(literals.begin(), literals.end());
  const std::set<std::string> expected(gemma4_literals().begin(), gemma4_literals().end());
  expect(listed == expected && listed.size() == 24U, "the renderer's table is the vocabulary's 24 special literals");

  for (const bool thinking : {false, true}) {
    for (const auto& literal : gemma4_literals()) {
      for (const std::string& text : {"a" + literal + "b", literal, "Hi " + literal, literal + "\n"}) {
        expect(!secure_frontend::render_gemma4_text_prompt({{"user", text}}, thinking).has_value(), "user text holds " + literal);
        expect(!secure_frontend::render_gemma4_text_prompt({{"system", text}, {"user", "Hi"}}, thinking).has_value(),
               "system text holds " + literal);
        expect(!secure_frontend::render_gemma4_text_prompt({{"user", "Hi"}, {"assistant", text}, {"user", "Go"}}, thinking)
                    .has_value(),
               "an earlier answer holds " + literal);
      }
    }
    const std::vector<Messages> disordered{
        {},
        {{"system", "Be brief."}},
        {{"assistant", "Hello."}},
        {{"assistant", "Hello."}, {"user", "Hi"}},
        {{"user", "Hi"}, {"user", "Again"}},
        {{"user", "Hi"}, {"assistant", "Hello."}},
        {{"user", "Hi"}, {"system", "Late."}, {"user", "Go"}},
        {{"system", "One."}, {"system", "Two."}, {"user", "Go"}},
        {{"user", "Hi"}, {"assistant", "A"}, {"assistant", "B"}, {"user", "Go"}},
        {{"tool", "x"}},
        {{"developer", "x"}, {"user", "Hi"}},
        {{"model", "x"}},
        {{"user", "Hi"}, {"model", "Hello."}, {"user", "Go"}},
    };
    for (const auto& messages : disordered) {
      expect(!secure_frontend::render_gemma4_text_prompt(messages, thinking).has_value(),
             "only a leading system message, then user/assistant alternation ending in user");
    }
    for (const std::string& text : {std::string{}, std::string{" "}, std::string{" \n\t\r "}, std::string{"\xE3\x80\x80\xC2\xA0"}}) {
      expect(!secure_frontend::render_gemma4_text_prompt({{"user", text}}, thinking).has_value(), "an empty or all-whitespace user text");
      expect(!secure_frontend::render_gemma4_text_prompt({{"system", text}, {"user", "Hi"}}, thinking).has_value(),
             "an empty or all-whitespace system text");
      expect(!secure_frontend::render_gemma4_text_prompt({{"user", "Hi"}, {"assistant", text}, {"user", "Go"}}, thinking)
                  .has_value(),
             "an empty or all-whitespace earlier answer");
    }
    for (const std::string& text : {std::string{"a\x01z"}, std::string{"\x0b"} + "Hi", std::string{"Hi\x1f"}, std::string{"a\0b", 3U},
                                    std::string{"\xFF"}, std::string{"\xC3"}, std::string{"\xED\xA0\x80"}}) {
      expect(!secure_frontend::render_gemma4_text_prompt({{"user", text}}, thinking).has_value(),
             "a C0 control byte or invalid UTF-8 fails closed");
    }
    secure_frontend::ChatMessage parts{"user", "Hi", {}};
    parts.parts.push_back({"Hi", ""});
    expect(!secure_frontend::render_gemma4_text_prompt({parts}, thinking).has_value(), "the parts form is refused (text only)");
    // Near-markers are text: only the exact literals are refused.
    for (const std::string text : {"<|turn", "turn|>", "<channel", "<|think|", "thought\n", "<bos", "<|channel |>", "< bos>"}) {
      expect(secure_frontend::render_gemma4_text_prompt({{"user", "x " + text + " y"}}, thinking).has_value(),
             "a near-marker is ordinary text: " + text);
    }
  }
}

std::vector<std::size_t> ids_of(const secure_frontend::TokenizerPack& pack, const std::string_view text) {
  const auto encoded = secure_frontend::encode_text(pack, text);
  const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded);
  expect(ids != nullptr && !ids->empty(), "test text must encode: " + std::string{text});
  return ids != nullptr ? *ids : std::vector<std::size_t>{};
}

void plan_encodes_markers_as_single_ids_and_text_as_spm() {
  using test_support::token_id;
  const auto pack = test_support::spm_tokenizer();
  const auto markers = secure_frontend::find_gemma4_markers(pack);
  expect(markers.has_value(), "the synthetic Gemma pack carries the seven markers");
  if (!markers) return;
  expect(markers->bos == token_id(pack, "<bos>") && markers->eos == token_id(pack, "<eos>") &&
             markers->turn_open == token_id(pack, "<|turn>") && markers->turn_close == token_id(pack, "<turn|>") &&
             markers->think == token_id(pack, "<|think|>") && markers->channel_open == token_id(pack, "<|channel>") &&
             markers->channel_close == token_id(pack, "<channel|>"),
         "each marker is found by its literal");

  // Text is spm_bpe: "Hi world" is two merged tokens, "Hi" and "▁world".
  const std::string space{test_support::kSpmSpace};
  expect(ids_of(pack, "Hi world") == std::vector<std::size_t>{token_id(pack, "Hi"), token_id(pack, space + "world")},
         "the synthetic pack merges Hi and ▁world");

  const auto append = [](std::vector<std::size_t>& to, const std::vector<std::size_t>& from) { to.insert(to.end(), from.begin(), from.end()); };
  std::vector<std::size_t> off{markers->bos, markers->turn_open};
  append(off, ids_of(pack, "user\n"));
  append(off, ids_of(pack, "Hi world"));
  off.push_back(markers->turn_close);
  append(off, ids_of(pack, "\n"));
  off.push_back(markers->turn_open);
  append(off, ids_of(pack, "model\n"));
  off.push_back(markers->channel_open);
  append(off, ids_of(pack, "thought\n"));
  off.push_back(markers->channel_close);

  const secure_frontend::ChatCompletionRequest request{"gemma-local", {{"user", " Hi world "}}, 4U};
  const auto off_result = secure_frontend::make_gemma4_chat_generation_plan(request, pack, 4096U, 16U, {}, false);
  const auto* off_plan = std::get_if<secure_frontend::ChatGenerationPlan>(&off_result);
  expect(off_plan != nullptr, "thinking off plans on the Gemma pack");
  if (off_plan != nullptr) {
    expect(off_plan->prompt_token_ids == off, "markers are single ids, the rest spm text, thinking off");
    expect(off_plan->model == "gemma-local" && off_plan->max_tokens == 4U, "the plan keeps the model and max_tokens");
    expect(off_plan->think_tokens.has_value() && off_plan->think_tokens->open == markers->channel_open &&
               off_plan->think_tokens->close == markers->channel_close && !off_plan->starts_in_reasoning,
           "the thought channel is the reasoning split; the reply starts as the answer (the prompt closed the channel)");
  }
  const auto rendered_off = secure_frontend::render_gemma4_text_prompt(request.messages, false);
  const auto direct = rendered_off ? secure_frontend::encode_gemma4_prompt(pack, *rendered_off) : secure_frontend::TokenizerEncodeResult{};
  expect(std::holds_alternative<std::vector<std::size_t>>(direct) && std::get<std::vector<std::size_t>>(direct) == off,
         "encode_gemma4_prompt of the rendered bytes is the plan's prompt");

  std::vector<std::size_t> on{markers->bos, markers->turn_open};
  append(on, ids_of(pack, "system\n"));
  on.push_back(markers->think);
  append(on, ids_of(pack, "\nBe brief."));   // the newer template's newline after <|think|>, one text span with the system text
  on.push_back(markers->turn_close);
  append(on, ids_of(pack, "\n"));
  on.push_back(markers->turn_open);
  append(on, ids_of(pack, "user\n"));
  append(on, ids_of(pack, "Hi world"));
  on.push_back(markers->turn_close);
  append(on, ids_of(pack, "\n"));
  on.push_back(markers->turn_open);
  append(on, ids_of(pack, "model\n"));
  const secure_frontend::ChatCompletionRequest with_system{"gemma-local", {{"system", "Be brief."}, {"user", "Hi world"}}, 4U};
  const auto on_result = secure_frontend::make_gemma4_chat_generation_plan(with_system, pack, 4096U, 16U, {}, true);
  const auto* on_plan = std::get_if<secure_frontend::ChatGenerationPlan>(&on_result);
  expect(on_plan != nullptr && on_plan->prompt_token_ids == on, "thinking on: <|think|> one id at the head of the system turn, then its newline");
  expect(on_plan != nullptr && on_plan->think_tokens.has_value() && !on_plan->starts_in_reasoning,
         "thinking on: the model opens the channel itself, so the reply starts outside it");

  // The encoder takes the markers only where the renderer puts them.
  const std::vector<std::string> misplaced{
      "<|turn>user\nHi<turn|>\n<|turn>model\n",                                               // no <bos>
      "<bos><bos><|turn>user\nHi<turn|>\n<|turn>model\n",                                     // a second <bos>
      "<bos><|turn>user\nHi<bos><turn|>\n<|turn>model\n",                                     // <bos> inside
      "<bos><|turn>user\n<|think|>Hi<turn|>\n<|turn>model\n",                                 // <|think|> in a user turn
      "<bos><|turn>system\nx<|think|><turn|>\n<|turn>user\nHi<turn|>\n<|turn>model\n",        // not first in the system turn
      "<bos><|turn>user\n<|channel>thought\n<channel|>Hi<turn|>\n<|turn>model\n",            // a channel mid-prompt
      "<bos><|turn>user\nHi<turn|>\n<|turn>model\n<|channel>analysis\n<channel|>",           // another channel name
      "<bos><|turn>user\nHi<turn|>\n<|turn>model\n<|channel>thought\n<channel|>x",           // text after the tail
      "<bos><|turn>user\nHi<turn|>\n<|channel>thought\n<channel|>",                          // the tail not after model
      "<bos><|turn>user\nHi<eos><turn|>\n<|turn>model\n",                                     // another control literal
      "<bos><|turn>user\nHi<|tool_call><turn|>\n<|turn>model\n",                              // a user-defined literal
  };
  for (const auto& prompt : misplaced) {
    const auto refused = secure_frontend::encode_gemma4_prompt(pack, prompt);
    const auto* failure = std::get_if<secure_frontend::TokenizerPackError>(&refused);
    expect(failure != nullptr && failure->code == secure_frontend::TokenizerPackErrorCode::unsupported_special_token,
           "a marker outside its place fails closed");
  }

  // The markers must be exactly one each, of the GGUF's type, in an spm_bpe pack.
  const auto mutated = [&pack](const std::string_view literal, const int type, const bool duplicate) {
    auto copy = pack;
    const auto id = token_id(copy, literal);
    if (duplicate) {
      copy.tokens_by_id.emplace_back(literal);
      copy.token_types.push_back(copy.token_types[id]);
    } else {
      copy.token_types[id] = type;
    }
    return copy;
  };
  for (const auto& literal : {"<bos>", "<eos>", "<|turn>", "<turn|>", "<|think|>", "<|channel>", "<channel|>"}) {
    const auto twice = mutated(literal, 0, true);
    const int other_type = std::string_view{literal} == "<|channel>" || std::string_view{literal} == "<channel|>" ? 3 : 4;
    const auto retyped = mutated(literal, other_type, false);
    const auto normal = mutated(literal, 1, false);
    for (const auto* broken : {&twice, &retyped, &normal}) {
      // <eos> typed NORMAL is a later GGUF's, not a broken one (below)
      if (broken == &normal && std::string_view{literal} == "<eos>") continue;
      expect(!secure_frontend::find_gemma4_markers(*broken).has_value(), std::string{"no markers: "} + literal);
      const auto result = secure_frontend::make_gemma4_chat_generation_plan(request, *broken, 4096U, 16U, {}, false);
      const auto* failure = std::get_if<secure_frontend::ChatGenerationPlanError>(&result);
      expect(failure != nullptr && failure->code == secure_frontend::ChatGenerationPlanErrorCode::tokenization_failed,
             std::string{"the plan fails closed without unambiguous markers: "} + literal);
    }
  }
  // Owner 2026-10-28: unsloth's later Gemma 4 GGUFs (gemma-4-26B-A4B-it-Q8_0 at c099eb48, his download) type <eos>
  // NORMAL and name <turn|> the end of a turn (eos_token_id 106), so every chat on that card was refused (his
  // calibration's "a calibration reply failed (invalid_request)"). <eos>, once and NORMAL, is still the marker; the
  // other six keep their types.
  {
    auto later = pack;
    later.token_types[token_id(later, "<eos>")] = 1;
    const auto found = secure_frontend::find_gemma4_markers(later);
    const auto result = secure_frontend::make_gemma4_chat_generation_plan(request, later, 4096U, 16U, {}, false);
    const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&result);
    expect(found.has_value() && found->eos == token_id(later, "<eos>") && plan != nullptr &&
               plan->stop_tokens == std::vector<std::size_t>{token_id(later, "<turn|>"), token_id(later, "<eos>")},
           "a later GGUF's NORMAL <eos> is still the marker, and a stop id");
  }
  auto byte_mode = pack;
  byte_mode.mode = secure_frontend::TokenizerMode::byte_bpe;
  expect(!secure_frontend::find_gemma4_markers(byte_mode).has_value() &&
             std::holds_alternative<secure_frontend::TokenizerPackError>(secure_frontend::encode_gemma4_prompt(byte_mode, *rendered_off)),
         "a byte_bpe pack is never a Gemma pack, whatever its literals");
  const auto qwen = test_support::byte_tokenizer();
  expect(std::holds_alternative<secure_frontend::ChatGenerationPlanError>(
             secure_frontend::make_gemma4_chat_generation_plan(request, qwen, 4096U, 16U, {}, false)),
         "a Qwen pack cannot serve the gemma4 plan");

  // The plan's other rules, as the qwen and GLM plans.
  const auto code_of = [](const secure_frontend::ChatGenerationPlanResult& result) {
    const auto* failure = std::get_if<secure_frontend::ChatGenerationPlanError>(&result);
    return failure != nullptr ? failure->code : secure_frontend::ChatGenerationPlanErrorCode{-1};
  };
  expect(code_of(secure_frontend::make_gemma4_chat_generation_plan({"gemma-local", {{"user", "Hi"}}, 0U}, pack, 4096U, 16U, {}, false)) ==
             secure_frontend::ChatGenerationPlanErrorCode::invalid_request,
         "max_tokens 0 is refused");
  expect(code_of(secure_frontend::make_gemma4_chat_generation_plan({"", {{"user", "Hi"}}, 4U}, pack, 4096U, 16U, {}, false)) ==
             secure_frontend::ChatGenerationPlanErrorCode::invalid_request,
         "an empty model is refused");
  expect(code_of(secure_frontend::make_gemma4_chat_generation_plan(request, pack, off.size() - 1U, 16U, {}, false)) ==
             secure_frontend::ChatGenerationPlanErrorCode::prompt_too_large,
         "a prompt above the limit is too large");
  expect(std::holds_alternative<secure_frontend::ChatGenerationPlan>(
             secure_frontend::make_gemma4_chat_generation_plan(request, pack, off.size(), 16U, {}, false)),
         "a prompt at the limit plans");
  expect(code_of(secure_frontend::make_gemma4_chat_generation_plan({"gemma-local", {{"user", "a<|turn>b"}}, 4U}, pack, 4096U, 16U, {},
                                                                    false)) == secure_frontend::ChatGenerationPlanErrorCode::prompt_render_failed,
         "a marker in client text fails at rendering");
  secure_frontend::ChatMessage image{"user", "", {}};
  image.parts.push_back({"", "data:image/png;base64,AAAA"});
  expect(code_of(secure_frontend::make_gemma4_chat_generation_plan({"gemma-local", {image}, 4U}, pack, 4096U, 16U, {}, false)) ==
             secure_frontend::ChatGenerationPlanErrorCode::invalid_request,
         "the gemma4 profile cannot serve image parts");
  secure_frontend::SamplingDefaults defaults;
  defaults.temperature = 1.0;
  defaults.top_k = 64;
  secure_frontend::ChatCompletionRequest sampled = request;
  sampled.sampling.temperature = 0.5;
  sampled.max_tokens.reset();
  const auto sampled_result = secure_frontend::make_gemma4_chat_generation_plan(sampled, pack, 4096U, 16U, defaults, false);
  const auto* sampled_plan = std::get_if<secure_frontend::ChatGenerationPlan>(&sampled_result);
  expect(sampled_plan != nullptr && sampled_plan->sampling.temperature == 0.5 && sampled_plan->sampling.top_k == 64 &&
             !sampled_plan->sampling.top_p.has_value() && sampled_plan->max_tokens == 16U,
         "the request's sampling wins over the defaults, which fill the rest; no max_tokens takes the default");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"renders_the_template_bytes_thinking_off", renders_the_template_bytes_thinking_off},
      {"renders_the_template_bytes_thinking_on", renders_the_template_bytes_thinking_on},
      {"trims_like_jinja", trims_like_jinja},
      {"refuses_markers_order_and_empty_text", refuses_markers_order_and_empty_text},
      {"plan_encodes_markers_as_single_ids_and_text_as_spm", plan_encodes_markers_as_single_ids_and_text_as_spm},
  }, failures);
}
