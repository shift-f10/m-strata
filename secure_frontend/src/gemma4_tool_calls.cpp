#include "secure_frontend/gemma4_tool_calls.hpp"

#include "secure_frontend/json.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

constexpr std::string_view kOpen = "<|tool_call>";
constexpr std::string_view kClose = "<tool_call|>";
constexpr std::string_view kQuote = "<|\"|>";
constexpr std::size_t kMaximumDepth = 64U;

// A JSON number's text as JSON (llama.cpp's json_number rule matched it first).
constexpr JsonLimits kNumberLimits{.max_bytes = 4096U, .max_depth = 1U, .max_container_entries = 1U, .max_string_bytes = 1U,
                                   .allow_fractions = true};

// A member list with a repeated key keeping its first place and its last value.
void put(std::vector<JsonMember>& members, std::string key, JsonValue value) {
  for (auto& m : members) {
    if (m.key == key) {
      m.value = std::move(value);
      return;
    }
  }
  members.push_back({std::move(key), std::move(value)});
}

// llama.cpp's Gemma 4 call grammar over one body, by hand: the same rules in the same order (a PEG's ordered choice).
class Reader final {
 public:
  explicit Reader(const std::string_view text) : text_(text) {}

  [[nodiscard]] bool done() const { return at_ == text_.size(); }
  bool literal(const std::string_view word) {
    if (text_.substr(at_, word.size()) != word) return false;
    at_ += word.size();
    return true;
  }
  void space() {  // common_peg_space_parser: std::isspace in the C locale
    while (at_ < text_.size() && is_space(static_cast<unsigned char>(text_[at_]))) ++at_;
  }
  [[nodiscard]] char peek() const { return at_ < text_.size() ? text_[at_] : '\0'; }

  // gemma4-dict: "{" ws ("}" | members ws "}")
  std::optional<std::vector<JsonMember>> dict(const std::size_t depth) {
    if (depth > kMaximumDepth || !literal("{")) return std::nullopt;
    space();
    std::vector<JsonMember> members;
    if (literal("}")) return members;
    for (;;) {
      auto key = key_name();
      if (!key || !literal(":")) return std::nullopt;
      space();
      auto item = value(depth + 1U);
      if (!item) return std::nullopt;
      put(members, std::move(*key), std::move(*item));
      const std::size_t mark = at_;
      if (literal(",")) {
        space();
        continue;
      }
      at_ = mark;
      space();
      if (!literal("}")) return std::nullopt;
      return members;
    }
  }

 private:
  static bool is_space(const unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

  // gemma4-dict-key-name: one or more bytes but ':' and '}'
  std::optional<std::string> key_name() {
    const std::size_t start = at_;
    while (at_ < text_.size() && text_[at_] != ':' && text_[at_] != '}') ++at_;
    if (at_ == start) return std::nullopt;
    return std::string{text_.substr(start, at_ - start)};
  }

  // gemma4-value: string | dict | array | number | bool | null
  std::optional<JsonValue> value(const std::size_t depth) {
    if (depth > kMaximumDepth) return std::nullopt;
    if (literal(kQuote)) {
      const std::size_t end = text_.find(kQuote, at_);
      if (end == std::string_view::npos) return std::nullopt;
      std::string content{text_.substr(at_, end - at_)};
      at_ = end + kQuote.size();
      return JsonValue::make_string(std::move(content));
    }
    if (peek() == '{') {
      auto members = dict(depth);
      if (!members) return std::nullopt;
      return JsonValue::make_object(std::move(*members));
    }
    if (peek() == '[') return array(depth);
    if (auto number = this->number()) return number;
    if (literal("true")) return JsonValue::make_boolean(true);
    if (literal("false")) return JsonValue::make_boolean(false);
    if (literal("null")) return JsonValue{};
    return std::nullopt;
  }

  // gemma4-array: "[" ws ("]" | value ("," ws value)* ws "]")
  std::optional<JsonValue> array(const std::size_t depth) {
    if (!literal("[")) return std::nullopt;
    space();
    std::vector<JsonValue> items;
    if (literal("]")) return JsonValue::make_array(std::move(items));
    for (;;) {
      auto item = value(depth + 1U);
      if (!item) return std::nullopt;
      items.push_back(std::move(*item));
      const std::size_t mark = at_;
      if (literal(",")) {
        space();
        continue;
      }
      at_ = mark;
      space();
      if (!literal("]")) return std::nullopt;
      return JsonValue::make_array(std::move(items));
    }
  }

  // json-number: -? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?, not followed by one of [0-9.eE+-]
  std::optional<JsonValue> number() {
    const std::size_t start = at_;
    const auto digits = [this] {
      const std::size_t from = at_;
      while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
      return at_ > from;
    };
    (void)literal("-");
    if (!literal("0")) {
      if (peek() < '1' || peek() > '9') {
        at_ = start;
        return std::nullopt;
      }
      (void)digits();
    }
    if (const std::size_t mark = at_; literal(".") && !digits()) at_ = mark;
    if (const std::size_t mark = at_; literal("e") || literal("E")) {
      if (!literal("+")) (void)literal("-");
      if (!digits()) at_ = mark;
    }
    const char next = peek();
    if ((next >= '0' && next <= '9') || next == '.' || next == 'e' || next == 'E' || next == '+' || next == '-') {
      at_ = start;
      return std::nullopt;
    }
    auto parsed = parse_json(text_.substr(start, at_ - start), kNumberLimits);
    auto* number = std::get_if<JsonValue>(&parsed);
    if (number == nullptr) {
      at_ = start;
      return std::nullopt;
    }
    return std::move(*number);
  }

  std::string_view text_;
  std::size_t at_{0U};
};

}  // namespace

std::optional<ToolCall> parse_gemma4_tool_call(const std::string_view body, const std::span<const ToolDeclaration> tools) {
  constexpr std::string_view kCall = "call:";
  if (body.substr(0U, kCall.size()) != kCall) return std::nullopt;
  const std::string_view rest = body.substr(kCall.size());
  // the declared tool whose name the body spells, right before its "{" (llama.cpp: tool_name(literal) + peek("{"))
  const auto tool = std::find_if(tools.begin(), tools.end(), [rest](const ToolDeclaration& declared) {
    return !declared.name.empty() && rest.size() > declared.name.size() && rest.substr(0U, declared.name.size()) == declared.name &&
           rest[declared.name.size()] == '{';
  });
  if (tool == tools.end()) return std::nullopt;
  Reader reader{rest.substr(tool->name.size())};
  auto members = reader.dict(1U);
  if (!members || !reader.done()) return std::nullopt;
  ToolCall call{tool->name, {}};
  for (auto& member : *members) call.arguments.push_back({std::move(member.key), std::move(member.value)});
  return call;
}

Gemma4ToolCallParser::Gemma4ToolCallParser(std::vector<ToolDeclaration> tools) : tools_(std::move(tools)) {}

std::vector<ToolParseEvent> Gemma4ToolCallParser::feed_reasoning(const std::string_view text) {
  std::vector<ToolParseEvent> out;
  run(out, true);  // the answer's pending text (a call cut by a new thought) is what it was
  if (!text.empty()) out.push_back({ToolParseEvent::Kind::reasoning, std::string{text}, {}});
  return out;
}

std::vector<ToolParseEvent> Gemma4ToolCallParser::end_reasoning() { return {}; }

std::vector<ToolParseEvent> Gemma4ToolCallParser::feed_content(const std::string_view text) {
  buf_.append(text);
  std::vector<ToolParseEvent> out;
  run(out, false);
  return out;
}

std::vector<ToolParseEvent> Gemma4ToolCallParser::finish(const bool natural) {
  (void)natural;  // a call cut short is text either way
  std::vector<ToolParseEvent> out;
  run(out, true);
  return out;
}

void Gemma4ToolCallParser::run(std::vector<ToolParseEvent>& out, const bool final) {
  const auto content = [&out](std::string text) {
    if (!text.empty()) out.push_back({ToolParseEvent::Kind::content, std::move(text), {}});
  };
  for (;;) {
    if (!in_call_) {
      const std::size_t open = buf_.find(kOpen);
      if (open != std::string::npos) {
        content(buf_.substr(0U, open));
        buf_.erase(0U, open + kOpen.size());
        in_call_ = true;
        continue;
      }
      // hold the longest end that could still become <|tool_call>
      std::size_t hold = 0U;
      if (!final) {
        for (std::size_t n = std::min(buf_.size(), kOpen.size() - 1U); n > 0U; --n) {
          if (std::string_view{buf_}.substr(buf_.size() - n) == kOpen.substr(0U, n)) {
            hold = n;
            break;
          }
        }
      }
      content(buf_.substr(0U, buf_.size() - hold));
      buf_.erase(0U, buf_.size() - hold);
      return;
    }
    const std::size_t close = buf_.find(kClose);
    if (close == std::string::npos) {
      if (!final) return;
      content(std::string{kOpen} + buf_);
      buf_.clear();
      in_call_ = false;
      return;
    }
    const std::string_view body = std::string_view{buf_}.substr(0U, close);
    if (auto call = parse_gemma4_tool_call(body, tools_)) out.push_back({ToolParseEvent::Kind::call, {}, std::move(*call)});
    else content(std::string{kOpen} + std::string{body} + std::string{kClose});
    buf_.erase(0U, close + kClose.size());
    in_call_ = false;
  }
}

}  // namespace secure_frontend
