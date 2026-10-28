#include "secure_frontend/minimax_tool_calls.hpp"

#include "secure_frontend/minimax_prompt_renderer.hpp"
#include "secure_frontend/python_text.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace secure_frontend {
namespace {

constexpr std::string_view kNs = kMinimaxToolNamespace;
constexpr JsonLimits kScalarLimits{.max_bytes = 1U << 20U, .max_depth = 1U, .max_container_entries = 1U,
                                   .max_string_bytes = 1U << 20U, .allow_fractions = true};

// json.dumps(value, ensure_ascii=False) with the angle bracket escaped, as Maya's _close_value writes a value
std::string json_of(const JsonValue& value) {
  std::string out;
  for (const char c : python_json_text(value)) {
    if (c == '<') out += "\\u003c";
    else out += c;
  }
  return out;
}

// -?\d+(\.\d+)?([eE][-+]?\d+)?, whole (Maya's _scalar; ASCII digits)
bool is_number(const std::string_view t) {
  std::size_t i = 0U;
  const auto digits = [&t, &i] {
    const std::size_t from = i;
    while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])) != 0) ++i;
    return i > from;
  };
  if (i < t.size() && t[i] == '-') ++i;
  if (!digits()) return false;
  if (i < t.size() && t[i] == '.') {
    ++i;
    if (!digits()) return false;
  }
  if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
    ++i;
    if (i < t.size() && (t[i] == '-' || t[i] == '+')) ++i;
    if (!digits()) return false;
  }
  return i == t.size();
}

// Maya's _scalar: a number, true, false or null (stripped: Python's whitespace) as that JSON; anything else the text as
// written.
JsonValue scalar(const std::string_view text) {
  const std::string_view t = python_strip(text);
  if (t == "true" || t == "false" || t == "null" || is_number(t)) {
    auto parsed = parse_json(t, kScalarLimits);
    if (auto* value = std::get_if<JsonValue>(&parsed)) return std::move(*value);
  }
  return JsonValue::make_string(std::string{text});
}

// Maya's _parse: the elements a nested value spells (NS<k>..NS</k>) - an object, or an array when every one is `item`;
// text without elements is a scalar.
JsonValue nested(const std::string_view text) {
  std::vector<std::pair<std::string, JsonValue>> items;
  const std::string open = std::string{kNs} + "<";
  std::size_t pos = 0U;
  for (;;) {
    const std::size_t a = text.find(open, pos);
    if (a == std::string_view::npos) break;
    const std::size_t b = text.find('>', a + open.size());  // (past the namespace token's own '>')
    if (b == std::string_view::npos) break;
    const std::string name{text.substr(a + open.size(), b - a - open.size())};
    if (name.starts_with("/")) {
      pos = b + 1U;
      continue;
    }
    // its matching close, past nested elements of the same name
    const std::string close = std::string{kNs} + "</" + name + ">";
    const std::string opener = std::string{kNs} + "<" + name + ">";
    std::size_t depth = 1U;
    std::size_t k = b + 1U;
    std::size_t c = text.size();
    while (depth != 0U) {
      c = text.find(close, k);
      const std::size_t o = text.find(opener, k);
      if (c == std::string_view::npos) {
        c = text.size();
        break;
      }
      if (o != std::string_view::npos && o < c) {
        ++depth;
        k = o + opener.size();
      } else {
        --depth;
        k = c + close.size();
      }
    }
    items.emplace_back(name, nested(text.substr(b + 1U, c - (b + 1U))));
    pos = c + close.size();
  }
  if (items.empty()) return scalar(text);
  if (std::all_of(items.begin(), items.end(), [](const auto& item) { return item.first == "item"; })) {
    std::vector<JsonValue> array;
    for (auto& item : items) array.push_back(std::move(item.second));
    return JsonValue::make_array(std::move(array));
  }
  std::vector<JsonMember> members;  // a repeated name keeps its first place and its last value (a dict)
  for (auto& item : items) {
    auto known = std::find_if(members.begin(), members.end(), [&item](const JsonMember& m) { return m.key == item.first; });
    if (known != members.end()) known->value = std::move(item.second);
    else members.push_back({item.first, std::move(item.second)});
  }
  return JsonValue::make_object(std::move(members));
}

// the invoke's name="..." (re.search(r'name="([^"]*)"'))
std::string invoke_name(const std::string_view body) {
  const std::size_t at = body.find("name=\"");
  if (at == std::string_view::npos) return {};
  const std::size_t end = body.find('"', at + 6U);
  return end == std::string_view::npos ? std::string{} : std::string{body.substr(at + 6U, end - at - 6U)};
}

void append(std::vector<ToolParseEvent>& out, std::vector<ToolParseEvent> more) {
  for (auto& event : more) out.push_back(std::move(event));
}

// Maya reads a rewritten call with no schema: every value is the JSON the rewriter wrote.
std::vector<ToolDeclaration> without_types(std::vector<ToolDeclaration> tools) {
  for (auto& tool : tools) tool.string_parameters.clear();
  return tools;
}

}  // namespace

MinimaxRewriter::MinimaxRewriter(std::vector<ToolDeclaration> tools) : tools_(std::move(tools)) {}

std::string MinimaxRewriter::feed(const std::string_view text) {
  buf_ += text;
  std::string out;
  while (!buf_.empty()) {
    const std::size_t at = buf_.find(kNs);
    if (at == std::string::npos) {
      // a suffix that may still grow into the namespace token is held
      std::size_t keep = 0U;
      for (std::size_t n = 1U; n < kNs.size() && n <= buf_.size(); ++n) {
        if (std::string_view{buf_}.substr(buf_.size() - n) == kNs.substr(0, n)) keep = n;
      }
      out += plain(std::string_view{buf_}.substr(0, buf_.size() - keep));
      buf_.erase(0, buf_.size() - keep);
      break;
    }
    if (at != 0U) {
      out += plain(std::string_view{buf_}.substr(0, at));
      buf_.erase(0, at);
    }
    const std::string_view rest = std::string_view{buf_}.substr(kNs.size());
    if (rest.empty()) break;  // what follows the namespace token: not here yet
    if (rest.front() != '<') {
      out += plain(kNs);
      buf_.erase(0, kNs.size());
      continue;
    }
    const std::size_t gt = rest.find('>');
    if (gt == std::string_view::npos) break;  // the tag is not whole yet
    const std::string whole{rest.substr(0, gt + 1U)};
    buf_.erase(0, kNs.size() + gt + 1U);
    out += tag(whole);
  }
  return out;
}

std::string MinimaxRewriter::flush() {
  std::string rest;
  std::swap(rest, buf_);
  return plain(rest);
}

std::string MinimaxRewriter::finish() {
  std::string out = flush();
  if (key_) out += close_value();
  if (call_) {
    out += "</tool_call>";
    call_.reset();
  }
  return out;
}

std::string MinimaxRewriter::plain(const std::string_view text) {
  if (text.empty()) return {};
  if (key_) {
    if (vstring_ && !vnested_) return maya_json_escaped(text);
    vtext_ += text;
    return {};
  }
  if (in_calls_) return {};  // (between the calls' tags)
  return std::string{text};
}

std::string MinimaxRewriter::tag(const std::string_view whole) {
  const bool closing = whole.starts_with("</");
  const std::string_view body = closing ? whole.substr(2U, whole.size() - 3U) : whole.substr(1U, whole.size() - 2U);
  const std::string name{python_first_word(body)};  // body.split(None, 1)[0]
  if (key_) {  // inside a value: only its own closing tag ends it
    if (closing && name == *key_ && depth_ == 0) return close_value();
    if (vstring_) return maya_json_escaped(std::string{kNs} + std::string{whole});  // (a string: the tags are its text)
    depth_ += closing ? -1 : 1;
    vnested_ = true;
    vtext_ += std::string{kNs} + std::string{whole};
    return {};
  }
  if (name == "tool_call") {
    in_calls_ = !closing;
    return {};
  }
  if (name == "invoke") {
    if (closing) {
      call_.reset();
      return "</tool_call>";
    }
    call_ = invoke_name(body);
    return "<tool_call>" + *call_;
  }
  if (call_ && !closing) {
    key_ = name;
    depth_ = 0;
    vtext_.clear();
    vnested_ = false;
    const auto tool = std::find_if(tools_.begin(), tools_.end(), [this](const ToolDeclaration& t) { return t.name == *call_; });
    vstring_ = tool != tools_.end() &&
               std::find(tool->string_parameters.begin(), tool->string_parameters.end(), name) != tool->string_parameters.end();
    return "<arg_key>" + name + "</arg_key><arg_value>" + (vstring_ ? "\"" : "");
  }
  return plain(std::string{kNs} + std::string{whole});
}

std::string MinimaxRewriter::close_value() {
  std::string value;
  if (vstring_) value = "\"";
  else if (vnested_) value = json_of(nested(vtext_));
  else value = json_of(scalar(vtext_));
  key_.reset();
  vtext_.clear();
  return value + "</arg_value>";
}

MinimaxToolCallParser::MinimaxToolCallParser(std::vector<ToolDeclaration> tools)
    : rewriter_(tools), glm_(without_types(std::move(tools)), true) {}

std::vector<ToolParseEvent> MinimaxToolCallParser::feed_reasoning(const std::string_view text) {
  const std::string rewritten = rewriter_.feed(text);
  return rewritten.empty() ? std::vector<ToolParseEvent>{} : glm_.feed_reasoning(rewritten);
}

std::vector<ToolParseEvent> MinimaxToolCallParser::end_reasoning() {
  // what the rewriter held at the end of the reasoning is reasoning text
  std::vector<ToolParseEvent> out;
  if (const std::string held = rewriter_.flush(); !held.empty()) out = glm_.feed_reasoning(held);
  append(out, glm_.end_reasoning());
  answering_ = true;
  return out;
}

std::vector<ToolParseEvent> MinimaxToolCallParser::feed_content(const std::string_view text) {
  answering_ = true;
  const std::string rewritten = rewriter_.feed(text);
  return rewritten.empty() ? std::vector<ToolParseEvent>{} : glm_.feed_content(rewritten);
}

std::vector<ToolParseEvent> MinimaxToolCallParser::finish(const bool natural) {
  // the rewriter closes a value or a call still open; it belongs to the channel the reply ended in
  std::vector<ToolParseEvent> out;
  if (const std::string held = rewriter_.finish(); !held.empty()) out = answering_ ? glm_.feed_content(held) : glm_.feed_reasoning(held);
  append(out, glm_.finish(natural));
  return out;
}

}  // namespace secure_frontend
