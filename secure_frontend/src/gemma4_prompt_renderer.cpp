#include "secure_frontend/gemma4_prompt_renderer.hpp"

#include "secure_frontend/json.hpp"
#include "secure_frontend/prompt_builder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

// The bounds of the Qwen and GLM renderers (spec §9.5; session log step 13): 65,536 messages, a
// message of up to a whole 256 MiB body, and the rendered prompt that much plus room for every
// message's framing. A request is bounded by its config first.
constexpr std::size_t kMaximumMessages = 65536U;
constexpr std::size_t kMaximumMessageBytes = 256U * 1024U * 1024U;
constexpr std::size_t kMaximumPromptBytes = kMaximumMessageBytes + 16U * 1024U * 1024U;

constexpr std::string_view kBos = "<bos>";
constexpr std::string_view kTurnOpen = "<|turn>";
constexpr std::string_view kTurnClose = "<turn|>\n";
constexpr std::string_view kThink = "<|think|>";
constexpr std::string_view kClosedChannel = "<|channel>thought\n<channel|>";

constexpr std::array<std::string_view, 24U> kControlLiterals{
    "<pad>",     "<eos>",        "<bos>",        "<unk>",           "<mask>",           "<|tool>",
    "<tool|>",   "<|tool_call>", "<tool_call|>", "<|tool_response>", "<tool_response|>", "<|\"|>",
    "<|think|>", "<|channel>",   "<channel|>",   "<|turn>",         "<turn|>",          "<|image>",
    "<|audio>",  "<|image|>",    "<|audio|>",    "<image|>",        "<audio|>",         "<|video|>"};

// CPython 3.12's str.isspace() set, which str.strip() (Jinja's trim) removes.
constexpr std::array<std::uint32_t, 29U> kWhitespace{0x09U,   0x0AU,   0x0BU,   0x0CU,   0x0DU,   0x1CU,   0x1DU,   0x1EU,
                                                    0x1FU,   0x20U,   0x85U,   0xA0U,   0x1680U, 0x2000U, 0x2001U, 0x2002U,
                                                    0x2003U, 0x2004U, 0x2005U, 0x2006U, 0x2007U, 0x2008U, 0x2009U, 0x200AU,
                                                    0x2028U, 0x2029U, 0x202FU, 0x205FU, 0x3000U};

bool is_whitespace(const std::uint32_t point) {
  for (const auto space : kWhitespace) {
    if (space == point) return true;
  }
  return false;
}

bool continuation(const unsigned char byte) { return (byte & 0xC0U) == 0x80U; }

// The code point starting at `at` and its width; width 0 when the bytes there are not a complete
// sequence (the caller then stops: an incomplete sequence is never whitespace).
std::pair<std::uint32_t, std::size_t> code_point_at(const std::string_view text, const std::size_t at) {
  const auto lead = static_cast<unsigned char>(text[at]);
  std::size_t width = 0U;
  std::uint32_t point = 0U;
  if (lead < 0x80U) return {lead, 1U};
  if ((lead & 0xE0U) == 0xC0U) {
    width = 2U;
    point = lead & 0x1FU;
  } else if ((lead & 0xF0U) == 0xE0U) {
    width = 3U;
    point = lead & 0x0FU;
  } else if ((lead & 0xF8U) == 0xF0U) {
    width = 4U;
    point = lead & 0x07U;
  } else {
    return {0U, 0U};
  }
  if (width > text.size() - at) return {0U, 0U};
  for (std::size_t index = 1U; index < width; ++index) {
    const auto byte = static_cast<unsigned char>(text[at + index]);
    if (!continuation(byte)) return {0U, 0U};
    point = (point << 6U) | (byte & 0x3FU);
  }
  return {point, width};
}

bool valid_utf8(const std::string_view value) {
  for (std::size_t index = 0U; index < value.size();) {
    const auto byte = static_cast<unsigned char>(value[index]);
    if (byte <= 0x7FU) {
      ++index;
    } else if (byte >= 0xC2U && byte <= 0xDFU && index + 1U < value.size() &&
               continuation(static_cast<unsigned char>(value[index + 1U]))) {
      index += 2U;
    } else if (byte >= 0xE0U && byte <= 0xEFU && index + 2U < value.size() &&
               continuation(static_cast<unsigned char>(value[index + 1U])) &&
               continuation(static_cast<unsigned char>(value[index + 2U])) &&
               !(byte == 0xE0U && static_cast<unsigned char>(value[index + 1U]) < 0xA0U) &&
               !(byte == 0xEDU && static_cast<unsigned char>(value[index + 1U]) > 0x9FU)) {
      index += 3U;
    } else if (byte >= 0xF0U && byte <= 0xF4U && index + 3U < value.size() &&
               continuation(static_cast<unsigned char>(value[index + 1U])) &&
               continuation(static_cast<unsigned char>(value[index + 2U])) &&
               continuation(static_cast<unsigned char>(value[index + 3U])) &&
               !(byte == 0xF0U && static_cast<unsigned char>(value[index + 1U]) < 0x90U) &&
               !(byte == 0xF4U && static_cast<unsigned char>(value[index + 1U]) > 0x8FU)) {
      index += 4U;
    } else {
      return false;
    }
  }
  return true;
}

// The text a turn carries: bounded, valid UTF-8, no C0 control but \t \n \r, no Gemma literal,
// and something left after trim. Returns the trimmed view, empty when refused.
std::string_view safe_text(const ChatMessage& message) {
  const std::string_view content = message.content;
  if (!message.parts.empty() || content.empty() || content.size() > kMaximumMessageBytes || !valid_utf8(content)) return {};
  for (const unsigned char byte : content) {
    if (byte < 0x20U && byte != '\n' && byte != '\r' && byte != '\t') return {};
  }
  for (const auto literal : kControlLiterals) {
    if (content.find(literal) != std::string_view::npos) return {};
  }
  return gemma4_trim(content);
}

bool append_bounded(std::string* const prompt, const std::string_view text) {
  if (text.size() > kMaximumPromptBytes - prompt->size()) return false;
  prompt->append(text);
  return true;
}

bool append_turn(std::string* const prompt, const std::string_view role, const std::string_view prefix, const std::string_view text) {
  return append_bounded(prompt, kTurnOpen) && append_bounded(prompt, role) && append_bounded(prompt, "\n") &&
         append_bounded(prompt, prefix) && append_bounded(prompt, text) && append_bounded(prompt, kTurnClose);
}

// ---- web search (owner 2026-10-28): the template's tool macros over JSON values ----

constexpr std::string_view kQuote = "<|\"|>";
constexpr std::size_t kMaximumDepth = 64U;
constexpr JsonLimits kToolLimits{.max_bytes = 1U << 20U, .max_depth = 32U, .max_container_entries = 4096U,
                                 .max_string_bytes = 1U << 20U, .allow_fractions = true};
// llama.cpp's collect_result reads a result as JSON when it parses (nlohmann's json::parse; no NaN or Infinity).
constexpr JsonLimits kResultLimits{.max_bytes = kMaximumMessageBytes, .max_depth = kMaximumDepth, .max_container_entries = 1U << 20U,
                                   .max_string_bytes = kMaximumMessageBytes, .allow_fractions = true};

const JsonValue* get(const JsonValue* const object, const std::string_view key) {
  const auto* members = object != nullptr ? object->if_object() : nullptr;
  if (members == nullptr) return nullptr;
  for (const auto& m : *members) {
    if (m.key == key) return &m.value;
  }
  return nullptr;
}

// Jinja's truth: a missing value, null, false, zero and an empty string, list or mapping are false.
bool truthy(const JsonValue* const value) {
  if (value == nullptr) return false;
  switch (value->kind()) {
    case JsonKind::null: return false;
    case JsonKind::boolean: return *value->if_boolean();
    case JsonKind::integer: return *value->if_integer() != 0;
    case JsonKind::decimal: return *value->if_decimal() != 0.0;
    case JsonKind::string: return !value->if_string()->empty();
    case JsonKind::array: return !value->if_array()->empty();
    case JsonKind::object: return !value->if_object()->empty();
  }
  return false;
}

// What {{ value }} prints for a value the macros print: a string as it is, a missing one as "" (Jinja's Undefined);
// nullopt for any other (not this renderer's to print).
std::optional<std::string_view> printed(const JsonValue* const value) {
  if (value == nullptr) return std::string_view{};
  if (const auto* text = value->if_string()) return std::string_view{*text};
  return std::nullopt;
}

// Jinja's upper over a type name: ASCII only (nullopt for other bytes or a value that is no string); a missing one is "".
std::optional<std::string> upper(const JsonValue* const value) {
  const auto text = printed(value);
  if (!text) return std::nullopt;
  std::string out{*text};
  for (char& c : out) {
    if (static_cast<unsigned char>(c) >= 0x80U) return std::nullopt;
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

std::string ascii_lower(const std::string_view text) {
  std::string out{text};
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

// Jinja's dictsort: by key, case-insensitive (ASCII here), stable for keys equal but for case.
template <typename T, typename Key>
std::vector<const T*> dictsorted(const std::vector<T>& items, Key key) {
  std::vector<const T*> out;
  out.reserve(items.size());
  for (const auto& item : items) out.push_back(&item);
  std::stable_sort(out.begin(), out.end(), [&key](const T* a, const T* b) { return ascii_lower(key(*a)) < ascii_lower(key(*b)); });
  return out;
}

const std::string& member_key(const JsonMember& m) { return m.key; }

// The template's text and values: the server's own (text) or the model's and the web's (plain: literals stay text).
class Writer final {
 public:
  Writer(PromptBuilder& out, const bool untrusted) : out_(out), untrusted_(untrusted) {}

  void text(const std::string_view value) { out_.text(value); }
  void word(const std::string_view value) { untrusted_ ? out_.plain(value) : out_.text(value); }
  void quoted(const std::string_view value) {
    out_.marker(kQuote);
    word(value);
    out_.marker(kQuote);
  }

  // format_argument(argument, escape_keys)
  bool argument(const JsonValue& value, const bool escape_keys, const std::size_t depth = 0U) {
    if (depth > kMaximumDepth) return false;
    switch (value.kind()) {
      case JsonKind::string: quoted(*value.if_string()); return true;
      case JsonKind::boolean: text(*value.if_boolean() ? "true" : "false"); return true;
      case JsonKind::null: text("None"); return true;  // {{ argument }} of None
      case JsonKind::integer:
      case JsonKind::decimal: word(python_json_text(value)); return true;
      case JsonKind::array: {
        text("[");
        bool first = true;
        for (const auto& item : *value.if_array()) {
          if (!first) text(",");
          first = false;
          if (!argument(item, escape_keys, depth + 1U)) return false;
        }
        text("]");
        return true;
      }
      case JsonKind::object: {
        text("{");
        bool first = true;
        for (const JsonMember* m : dictsorted(*value.if_object(), member_key)) {
          if (!first) text(",");
          first = false;
          if (escape_keys) quoted(m->key);
          else word(m->key);
          text(":");
          if (!argument(m->value, escape_keys, depth + 1U)) return false;
        }
        text("}");
        return true;
      }
    }
    return false;
  }

  // `<|"|>item<|"|>` for each item of a required list, comma-separated.
  bool required(const JsonValue* const list) {
    const auto* items = list != nullptr ? list->if_array() : nullptr;
    if (items == nullptr) return false;
    bool first = true;
    for (const auto& item : *items) {
      const auto name = item.if_string();
      if (name == nullptr) return false;
      if (!first) text(",");
      first = false;
      quoted(*name);
    }
    return true;
  }

 private:
  PromptBuilder& out_;
  bool untrusted_;
};

// format_parameters(properties, required) of this GGUF's template (the parameter list is unused there).
bool parameters(Writer& w, const JsonValue& properties, const std::size_t depth) {
  static constexpr std::array<std::string_view, 5U> kStandard{"description", "type", "properties", "required", "nullable"};
  const auto* members = properties.if_object();
  if (members == nullptr || depth > kMaximumDepth) return false;
  bool found_first = false;
  for (const JsonMember* m : dictsorted(*members, member_key)) {
    if (std::find(kStandard.begin(), kStandard.end(), m->key) != kStandard.end()) continue;
    if (found_first) w.text(",");
    found_first = true;
    w.text(m->key);
    w.text(":{");
    const JsonValue* value = &m->value;
    bool add_comma = false;
    const auto comma = [&w, &add_comma] {
      if (add_comma) w.text(",");
      else add_comma = true;
    };
    if (truthy(get(value, "description"))) {
      const auto description = printed(get(value, "description"));
      if (!description) return false;
      w.text("description:");
      w.quoted(*description);
      add_comma = true;
    }
    if (truthy(get(value, "nullable"))) {
      comma();
      w.text("nullable:true");
    }
    const auto type = upper(get(value, "type"));
    if (!type) return false;
    if (*type == "STRING") {
      if (const JsonValue* choices = get(value, "enum"); truthy(choices)) {
        comma();
        w.text("enum:");
        if (!w.argument(*choices, true)) return false;
      }
    } else if (*type == "OBJECT") {
      w.text(",properties:{");
      const JsonValue* nested = get(value, "properties");
      if (nested != nullptr && nested->if_object() != nullptr) {
        if (!parameters(w, *nested, depth + 1U)) return false;
      } else if (value->if_object() != nullptr && !parameters(w, *value, depth + 1U)) {
        return false;
      }
      w.text("}");
      if (const JsonValue* list = get(value, "required"); truthy(list)) {
        w.text(",required:[");
        if (!w.required(list)) return false;
        w.text("]");
      }
    } else if (*type == "ARRAY") {
      const JsonValue* items = get(value, "items");
      if (items != nullptr && items->if_object() != nullptr && truthy(items)) {
        w.text(",items:{");
        bool found = false;
        for (const JsonMember* item : dictsorted(*items->if_object(), member_key)) {
          if (item->value.kind() == JsonKind::null) continue;
          if (found) w.text(",");
          found = true;
          if (item->key == "properties") {
            w.text("properties:{");
            if (item->value.if_object() != nullptr && !parameters(w, item->value, depth + 1U)) return false;
            w.text("}");
          } else if (item->key == "required") {
            w.text("required:[");
            if (!w.required(&item->value)) return false;
            w.text("]");
          } else if (item->key == "type") {
            w.text("type:");
            if (item->value.if_string() != nullptr) {
              const auto name = upper(&item->value);
              if (!name || !w.argument(JsonValue::make_string(*name), true)) return false;
            } else {
              const auto* names = item->value.if_array();
              if (names == nullptr) return false;
              std::vector<JsonValue> upper_names;
              for (const auto& n : *names) {
                const auto name = upper(&n);
                if (!name || n.if_string() == nullptr) return false;
                upper_names.push_back(JsonValue::make_string(*name));
              }
              if (!w.argument(JsonValue::make_array(std::move(upper_names)), true)) return false;
            }
          } else {
            w.text(item->key);
            w.text(":");
            if (!w.argument(item->value, true)) return false;
          }
        }
        w.text("}");
      }
    }
    comma();
    w.text("type:");
    w.quoted(*type);
    w.text("}");
  }
  return true;
}

// format_function_declaration(tool) | trim (a no-op: it opens with "declaration:" and closes with "}"). The tool is its
// function object, or the OpenAI form carrying it under "function".
bool declaration(Writer& w, const JsonValue& tool) {
  const JsonValue* wrapped = get(&tool, "function");
  const JsonValue* function = wrapped != nullptr && wrapped->if_object() != nullptr ? wrapped : &tool;
  const JsonValue* name = get(function, "name");
  if (function->if_object() == nullptr || name == nullptr || name->if_string() == nullptr || name->if_string()->empty()) return false;
  const auto description = printed(get(function, "description"));
  if (!description) return false;
  w.text("declaration:");
  w.text(*name->if_string());
  w.text("{description:");
  w.quoted(*description);
  if (const JsonValue* params = get(function, "parameters"); truthy(params)) {
    w.text(",parameters:{");
    if (const JsonValue* properties = get(params, "properties"); truthy(properties)) {
      w.text("properties:{");
      if (!parameters(w, *properties, 0U)) return false;
      w.text("},");
    }
    if (const JsonValue* list = get(params, "required"); truthy(list)) {
      w.text("required:[");
      if (!w.required(list)) return false;
      w.text("],");
    }
    if (const JsonValue* type = get(params, "type"); truthy(type)) {
      const auto name_upper = upper(type);
      if (!name_upper) return false;
      w.text("type:");
      w.quoted(*name_upper);
      w.text("}");
    }
  }
  if (const auto* members = function->if_object();
      std::any_of(members->begin(), members->end(), [](const JsonMember& m) { return m.key == "response"; })) {
    const JsonValue* response = get(function, "response");
    w.text(",response:{");
    if (truthy(get(response, "description"))) {
      const auto text = printed(get(response, "description"));
      if (!text) return false;
      w.text("description:");
      w.quoted(*text);
      w.text(",");
    }
    if (const auto type = upper(get(response, "type")); type && *type == "OBJECT") {
      w.text("type:");
      w.quoted(*type);
      w.text("}");
    }
  }
  w.text("}");
  return true;
}

// One result as the template writes llama.cpp's tool_responses entry for it.
bool tool_response(PromptBuilder& out, const std::string_view name, const std::string& result) {
  Writer w{out, true};
  out.marker("<|tool_response>");
  w.text("response:");
  w.word(name);
  auto parsed = parse_json(result, kResultLimits);
  const auto* value = std::get_if<JsonValue>(&parsed);
  if (value != nullptr && value->if_object() != nullptr) {
    w.text("{");
    bool first = true;
    for (const JsonMember* m : dictsorted(*value->if_object(), member_key)) {
      if (!first) w.text(",");
      first = false;
      w.word(m->key);
      w.text(":");
      if (!w.argument(m->value, false)) return false;
    }
    w.text("}");
  } else {
    w.text("{value:");
    if (value == nullptr) w.quoted(result);  // not JSON: the text, as a string
    else if (!w.argument(*value, false)) return false;
    w.text("}");
  }
  out.marker("<tool_response|>");
  return true;
}

const std::string& argument_name(const ToolArgument& a) { return a.name; }

bool tool_call(PromptBuilder& out, const ToolCall& call) {
  Writer w{out, true};
  out.marker("<|tool_call>");
  w.text("call:");
  w.word(call.name);
  w.text("{");
  bool first = true;
  for (const ToolArgument* a : dictsorted(call.arguments, argument_name)) {
    if (!first) w.text(",");
    first = false;
    w.word(a->name);
    w.text(":");
    if (!w.argument(a->value, false)) return false;
  }
  w.text("}");
  out.marker("<tool_call|>");
  return true;
}

}  // namespace

std::string_view gemma4_trim(std::string_view text) {
  while (!text.empty()) {
    const auto [point, width] = code_point_at(text, 0U);
    if (width == 0U || !is_whitespace(point)) break;
    text.remove_prefix(width);
  }
  while (!text.empty()) {
    // The last code point: step back over continuation bytes (at most three) to its lead byte.
    std::size_t start = text.size() - 1U;
    while (start > 0U && text.size() - start < 4U && continuation(static_cast<unsigned char>(text[start]))) --start;
    const auto [point, width] = code_point_at(text, start);
    if (width != text.size() - start || !is_whitespace(point)) break;
    text.remove_suffix(width);
  }
  return text;
}

const std::array<std::string_view, 24U>& gemma4_control_literals() { return kControlLiterals; }

std::optional<std::string> render_gemma4_text_prompt(const std::vector<ChatMessage>& messages, const bool thinking) {
  if (messages.empty() || messages.size() > kMaximumMessages) return std::nullopt;
  std::string prompt;
  prompt.reserve(256U);
  if (!append_bounded(&prompt, kBos)) return std::nullopt;

  std::size_t index = 0U;
  const bool has_system = messages.front().role == "system";
  if (has_system || thinking) {
    std::string_view system_text;
    if (has_system) {
      system_text = safe_text(messages.front());
      if (system_text.empty()) return std::nullopt;
      index = 1U;
    }
    if (!append_turn(&prompt, "system", thinking ? kThink : std::string_view{}, system_text)) return std::nullopt;
  }
  if (index == messages.size()) return std::nullopt;

  bool expect_user = true;
  for (; index < messages.size(); ++index) {
    const ChatMessage& message = messages[index];
    if (message.role != (expect_user ? "user" : "assistant")) return std::nullopt;
    const std::string_view text = safe_text(message);
    if (text.empty() || !append_turn(&prompt, expect_user ? "user" : "model", {}, text)) return std::nullopt;
    expect_user = !expect_user;
  }
  // A generation prompt follows a user turn only.
  if (expect_user || !append_bounded(&prompt, kTurnOpen) || !append_bounded(&prompt, "model\n")) return std::nullopt;
  if (!thinking && !append_bounded(&prompt, kClosedChannel)) return std::nullopt;
  return prompt;
}

std::optional<std::vector<PromptPiece>> render_gemma4_tool_prompt(const std::vector<ChatMessage>& messages,
                                                                  const std::span<const std::string> tools,
                                                                  const std::span<const ToolRound> rounds, const bool thinking) {
  // The conversation obeys every rule of the plain prompt (roles, order, literals, sizes).
  if (tools.empty() || !render_gemma4_text_prompt(messages, thinking)) return std::nullopt;
  for (const auto& round : rounds) {
    if (round.calls.empty() || round.results.size() != round.calls.size()) return std::nullopt;
    for (const auto& call : round.calls) {
      if (call.name.empty()) return std::nullopt;
    }
  }
  std::vector<JsonValue> declared;
  for (const auto& tool : tools) {
    auto parsed = parse_json(tool, kToolLimits);
    auto* value = std::get_if<JsonValue>(&parsed);
    if (value == nullptr || value->if_object() == nullptr) return std::nullopt;
    declared.push_back(std::move(*value));
  }
  PromptBuilder out;
  // the system turn, always with tools: <|think|> first, the system text, then each declaration
  out.marker(kBos);
  out.marker(kTurnOpen);
  out.text("system\n");
  if (thinking) out.marker(kThink);
  std::size_t index = 0U;
  if (messages.front().role == "system") {
    out.text(safe_text(messages.front()));
    index = 1U;
  }
  Writer ours{out, false};
  for (const auto& tool : declared) {
    out.marker("<|tool>");
    if (!declaration(ours, tool)) return std::nullopt;
    out.marker("<tool|>");
  }
  out.marker("<turn|>");
  out.text("\n");
  for (; index < messages.size(); ++index) {
    out.marker(kTurnOpen);
    out.text(messages[index].role == "assistant" ? "model\n" : "user\n");
    out.text(safe_text(messages[index]));
    out.marker("<turn|>");
    out.text("\n");
  }
  // llama.cpp's convert_tool_responses_gemma4: a round is one model turn of its calls and their responses, left open
  for (const auto& round : rounds) {
    out.marker(kTurnOpen);
    out.text("model\n");
    for (const auto& call : round.calls) {
      if (!tool_call(out, call)) return std::nullopt;
    }
    for (std::size_t i = 0U; i < round.results.size(); ++i) {
      if (!tool_response(out, round.calls[i].name, round.results[i])) return std::nullopt;
    }
  }
  // the generation prompt: a new model turn only after the conversation (after a response the turn goes on)
  if (rounds.empty()) {
    out.marker(kTurnOpen);
    out.text("model\n");
  }
  if (!thinking) {
    out.marker("<|channel>");
    out.text("thought\n");
    out.marker("<channel|>");
  }
  if (out.bytes() > kMaximumPromptBytes) return std::nullopt;
  return out.take();
}

}  // namespace secure_frontend
