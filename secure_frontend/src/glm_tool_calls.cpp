#include "secure_frontend/glm_tool_calls.hpp"

#include "secure_frontend/python_text.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

// Maya serve/frontend.py's tags
constexpr std::string_view kCallStart = "<tool_call>";
constexpr std::string_view kCallEnd = "</tool_call>";
constexpr std::string_view kFuncStart = "<function=";
constexpr std::string_view kArgKey = "<arg_key>";
constexpr std::string_view kArgKeyEnd = "</arg_key>";
constexpr std::string_view kArgValue = "<arg_value>";
constexpr std::string_view kArgValueEnd = "</arg_value>";
constexpr std::string_view kParamStart = "<parameter=";
constexpr std::string_view kParamEnd = "</parameter>";
constexpr std::size_t kNpos = std::string_view::npos;

bool starts_with(const std::string_view text, const std::string_view prefix) { return text.substr(0U, prefix.size()) == prefix; }

// _hold: the longest suffix of `text` that is a proper prefix of `tag`.
std::size_t hold_length(const std::string_view text, const std::string_view tag) {
  for (std::size_t k = std::min(tag.size() - 1U, text.size()); k > 0U; --k) {
    if (text.substr(text.size() - k) == tag.substr(0U, k)) return k;
  }
  return 0U;
}

// _arg_value: as written for a declared string, else the JSON it holds, else as written.
JsonValue argument_value(const std::string_view value, const bool is_string) {
  if (!is_string) {
    const JsonLimits limits{.max_bytes = value.size() + 1U, .max_depth = 64U, .max_container_entries = value.size() + 1U,
                            .max_string_bytes = value.size() + 1U, .allow_fractions = true};
    auto parsed = parse_json(value, limits);
    if (auto* decoded = std::get_if<JsonValue>(&parsed)) return std::move(*decoded);
  }
  return JsonValue::make_string(std::string{value});
}

bool declared_string(const ToolDeclaration* declaration, const std::string_view name) {
  return declaration != nullptr &&
         std::find(declaration->string_parameters.begin(), declaration->string_parameters.end(), name) != declaration->string_parameters.end();
}

// args[key] = value: a repeated key keeps its first place, its last value.
void set_argument(ToolCall& call, std::string name, JsonValue value) {
  const auto same = std::find_if(call.arguments.begin(), call.arguments.end(), [&name](const ToolArgument& a) { return a.name == name; });
  if (same != call.arguments.end()) same->value = std::move(value);
  else call.arguments.push_back({std::move(name), std::move(value)});
}

// call_name: the tool name of a call body, in either form.
std::string call_name(const std::string_view raw_body) {
  const std::string_view body = python_strip(raw_body);
  if (starts_with(body, kFuncStart)) {
    const std::string_view after = body.substr(kFuncStart.size());
    return std::string{after.substr(0U, after.find('>'))};
  }
  return std::string{python_strip(body.substr(0U, body.find('<')))};
}

// The streaming scan's name (Maya's _scan, state "name"): known once its end has arrived - Qwen's form at its ">", GLM's
// at the first "<" or the first line break after the leading whitespace - else nullopt.
std::optional<std::string> scanned_name(const std::string_view rest) {
  const std::string_view lead = python_lstrip(rest);
  if (lead.empty() || starts_with(kFuncStart, lead)) return std::nullopt;
  if (starts_with(lead, kFuncStart)) {
    const std::size_t a = rest.find(kFuncStart);
    const std::size_t b = rest.find('>', a + kFuncStart.size());
    if (b == kNpos) return std::nullopt;
    return std::string{rest.substr(a + kFuncStart.size(), b - a - kFuncStart.size())};
  }
  const std::size_t tag = rest.find('<');
  const std::size_t newline = rest.find('\n', rest.size() - lead.size());
  const std::size_t b = std::min(tag, newline);
  if (b == kNpos || python_strip(rest.substr(0U, b)).empty()) return std::nullopt;
  return std::string{python_strip(rest.substr(0U, b))};
}

}  // namespace

std::optional<ToolCall> parse_glm_tool_call(const std::string_view raw_body, const ToolDeclaration* const declaration) {
  const std::string_view body = python_strip(raw_body);
  if (!starts_with(body, kFuncStart)) {
    ToolCall call{call_name(body), {}};
    std::string_view rest = body.substr(std::min(body.find('<'), body.size()));
    if (call.name.empty() || !(rest.empty() || starts_with(python_lstrip(rest), kArgKey))) return std::nullopt;
    for (std::size_t at = rest.find(kArgKey); at != kNpos; at = rest.find(kArgKey)) {
      rest = rest.substr(at + kArgKey.size());
      const std::size_t key_end = rest.find(kArgKeyEnd);
      if (key_end == kNpos) return std::nullopt;
      std::string key{python_strip(rest.substr(0U, key_end))};
      rest = rest.substr(key_end + kArgKeyEnd.size());
      const std::size_t v0 = rest.find(kArgValue);
      if (v0 == kNpos || !python_strip(rest.substr(0U, v0)).empty()) return std::nullopt;
      rest = rest.substr(v0 + kArgValue.size());
      const std::size_t value_end = rest.find(kArgValueEnd);
      const std::string_view value = value_end != kNpos ? rest.substr(0U, value_end) : rest;
      rest = value_end != kNpos ? rest.substr(value_end + kArgValueEnd.size()) : std::string_view{};
      const bool is_string = declared_string(declaration, key);
      set_argument(call, std::move(key), argument_value(value, is_string));
    }
    return call;
  }
  // Qwen's form, as Maya reads it: each value to its first </parameter>
  const std::size_t gt = body.find('>');
  if (gt == kNpos) return std::nullopt;
  ToolCall call{std::string{body.substr(kFuncStart.size(), gt - kFuncStart.size())}, {}};
  std::string_view rest = body.substr(gt + 1U);
  for (std::size_t at = rest.find(kParamStart); at != kNpos; at = rest.find(kParamStart)) {
    rest = rest.substr(at + kParamStart.size());
    const std::size_t name_end = rest.find('>');
    if (name_end == kNpos) return std::nullopt;  // rest.index(">"): ValueError
    std::string name{rest.substr(0U, name_end)};
    rest = rest.substr(name_end + 1U);
    const std::size_t end = rest.find(kParamEnd);
    std::string_view value = end != kNpos ? rest.substr(0U, end) : rest;
    rest = end != kNpos ? rest.substr(end + kParamEnd.size()) : std::string_view{};
    if (starts_with(value, "\n")) value.remove_prefix(1U);
    if (!value.empty() && value.back() == '\n') value.remove_suffix(1U);
    const bool is_string = declared_string(declaration, name);
    set_argument(call, std::move(name), argument_value(value, is_string));
  }
  return call;
}

std::string maya_json_escaped(const std::string_view text) {
  std::string out;
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '<': out += "\\u003c"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20U) {
          constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out += kHex[(static_cast<unsigned char>(c) >> 4U) & 0xFU];
          out += kHex[static_cast<unsigned char>(c) & 0xFU];
        } else {
          out += c;
        }
    }
  }
  return out;
}

GlmToolCallParser::GlmToolCallParser(std::vector<ToolDeclaration> tools, const bool rewritten)
    : tools_(std::move(tools)), rewritten_(rewritten) {}

const ToolDeclaration* GlmToolCallParser::declared(const std::string_view name) const {
  for (const auto& tool : tools_) {
    if (tool.name == name) return &tool;
  }
  return nullptr;
}

void GlmToolCallParser::emit(std::vector<ToolParseEvent>& out, const ToolParseEvent::Kind kind, std::string text) {
  if (text.empty()) return;
  if (kind == ToolParseEvent::Kind::content) tracking_.track(text);
  out.push_back({kind, std::move(text), {}});
}

void GlmToolCallParser::start_content(const bool lead) {
  state_ = State::content;
  lead_ = lead;
  tracking_ = CodeTracking{};  // the answer's own text starts here
}

std::vector<ToolParseEvent> GlmToolCallParser::feed_reasoning(const std::string_view text) {
  std::vector<ToolParseEvent> out;
  if (state_ != State::reasoning) {
    // A <think> while answering (our generation run opens a new reasoning span): what the answer held is its text.
    emit(out, ToolParseEvent::Kind::content, state_ == State::call ? std::string{kCallStart} + buf_ : buf_);
    buf_.clear();
    state_ = State::reasoning;
    lead_ = false;
  }
  emit(out, ToolParseEvent::Kind::reasoning, std::string{text});  // Maya reads no call in the reasoning
  return out;
}

std::vector<ToolParseEvent> GlmToolCallParser::end_reasoning() {
  if (state_ == State::reasoning) start_content(true);
  return {};
}

std::vector<ToolParseEvent> GlmToolCallParser::feed_content(const std::string_view text) {
  std::vector<ToolParseEvent> out;
  if (state_ == State::reasoning) start_content(false);
  buf_.append(text);
  run(out);
  return out;
}

void GlmToolCallParser::run(std::vector<ToolParseEvent>& out) {
  for (;;) {
    if (state_ == State::call) {
      const std::size_t end = buf_.find(kCallEnd);
      if (end == kNpos) return;
      const std::string body = buf_.substr(0U, end);
      buf_.erase(0U, end + kCallEnd.size());
      const ToolDeclaration* tool = declared(call_name(body));
      std::optional<ToolCall> call = tool != nullptr ? parse_glm_tool_call(body, tool) : std::nullopt;
      if (call.has_value()) {
        out.push_back({ToolParseEvent::Kind::call, {}, std::move(*call)});
        state_ = State::content;
        lead_ = true;  // the newlines after a call are dropped
      } else {  // ours: another tool's call, or one that does not parse, is the text it was
        state_ = State::content;
        emit(out, ToolParseEvent::Kind::content, std::string{kCallStart} + body + std::string{kCallEnd});
      }
      continue;
    }
    if (lead_ && !rewritten_) {  // the newlines right after </think> or a call (GLM's and Qwen's rule only)
      const std::size_t first = buf_.find_first_not_of('\n');
      if (first == kNpos) {
        buf_.clear();
        return;
      }
      buf_.erase(0U, first);
      lead_ = false;
    }
    const std::size_t i = buf_.find(kCallStart);
    if (i == kNpos) {
      // a partial tag and the newlines before it are held: if a call follows, they are dropped
      std::size_t j = buf_.size() - hold_length(buf_, kCallStart);
      while (j > 0U && buf_[j - 1U] == '\n') --j;
      if (j > 0U) {
        emit(out, ToolParseEvent::Kind::content, buf_.substr(0U, j));
        buf_.erase(0U, j);
      }
      return;
    }
    // Strata #1058: a <tool_call> inside a code fence or inline code of the answer is a quoted example - text
    CodeTracking before = tracking_;
    before.track(std::string_view{buf_}.substr(0U, i));
    if (before.in_code()) {
      emit(out, ToolParseEvent::Kind::content, buf_.substr(0U, i + kCallStart.size()));
      buf_.erase(0U, i + kCallStart.size());
      continue;
    }
    if (rewritten_) {  // a rewriter's call: the template's "\n\n" before its calls block goes, the rest is text
      std::string before_call = buf_.substr(0U, i);
      if (before_call.ends_with("\n\n")) before_call.resize(before_call.size() - 2U);
      emit(out, ToolParseEvent::Kind::content, std::move(before_call));
    } else if (i > 0U && !python_strip(std::string_view{buf_}.substr(0U, i)).empty()) {
      std::string before_call = buf_.substr(0U, i);
      while (!before_call.empty() && before_call.back() == '\n') before_call.pop_back();
      emit(out, ToolParseEvent::Kind::content, std::move(before_call));
    }
    buf_.erase(0U, i + kCallStart.size());
    state_ = State::call;
  }
}

std::vector<ToolParseEvent> GlmToolCallParser::finish(const bool) {
  std::vector<ToolParseEvent> out;
  if (state_ == State::call) {
    // the reply ended inside a call: one whose name has arrived is a call with no arguments (Maya's scan delivers it)
    if (const auto name = scanned_name(buf_); name.has_value() && declared(*name) != nullptr) {
      out.push_back({ToolParseEvent::Kind::call, {}, ToolCall{*name, {}}});
      buf_.clear();
      return out;
    }
    emit(out, ToolParseEvent::Kind::content, std::string{kCallStart} + buf_);
    buf_.clear();
    return out;
  }
  emit(out, state_ == State::reasoning ? ToolParseEvent::Kind::reasoning : ToolParseEvent::Kind::content, std::move(buf_));
  buf_.clear();
  return out;
}

}  // namespace secure_frontend
