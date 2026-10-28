#include "secure_frontend/qwen_tool_calls.hpp"

#include "secure_frontend/python_text.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

// upstream serve/frontend.py's tags
constexpr std::string_view kCallStart = "<tool_call>";
constexpr std::string_view kCallEnd = "</tool_call>";
constexpr std::string_view kParamStart = "<parameter=";
constexpr std::string_view kParamEnd = "</parameter>";
constexpr std::string_view kFuncStart = "<function=";
constexpr std::string_view kFuncEnd = "</function>";
constexpr std::size_t kRcallMax = 32768U;  // RCALL_MAX: the most of a <tool_call> in the reasoning held, in characters
constexpr std::size_t kNpos = std::string_view::npos;

bool starts_with(const std::string_view text, const std::string_view prefix) { return text.substr(0U, prefix.size()) == prefix; }
// Python's whole.startswith(text): `text` is a prefix of `whole` (the empty text too).
bool prefix_of(const std::string_view text, const std::string_view whole) { return starts_with(whole, text); }

std::ptrdiff_t signed_at(const std::size_t at) { return static_cast<std::ptrdiff_t>(at); }
std::ptrdiff_t find(const std::string_view text, const std::string_view what, const std::size_t from = 0U) {
  const std::size_t at = text.find(what, from);
  return at == kNpos ? -1 : signed_at(at);
}

// param_end: where a parameter value ends - the first </parameter> followed (after whitespace) by the next
// <parameter= or </function>. -1: none yet; -2: a candidate whose follower has not arrived (`final` accepts it).
std::ptrdiff_t param_end(const std::string_view text, const bool final) {
  std::size_t at = text.find(kParamEnd);
  while (at != kNpos) {
    const std::string_view after = python_lstrip(text.substr(at + kParamEnd.size()));
    if (starts_with(after, kParamStart) || starts_with(after, kFuncEnd)) return signed_at(at);
    if (after.empty() || prefix_of(after, kParamStart) || prefix_of(after, kFuncEnd)) return final ? signed_at(at) : -2;
    at = text.find(kParamEnd, at + 1U);
  }
  return -1;
}

// call_end (#210): where a call body ends - the </tool_call> after the call's own </function>, found by walking its
// parameters, so a value may hold either tag. -1: not complete yet. A body not in the call format ends at the first
// </tool_call>.
std::ptrdiff_t call_end(const std::string_view text) {
  std::string_view s = python_lstrip(text);
  std::size_t pos = text.size() - s.size();
  if (!starts_with(s, kFuncStart)) return !prefix_of(s, kFuncStart) ? find(text, kCallEnd) : -1;
  std::size_t gt = text.find('>', pos);
  if (gt == kNpos) return -1;
  pos = gt + 1U;
  for (;;) {
    const std::string_view rest = text.substr(pos);
    s = python_lstrip(rest);
    pos += rest.size() - s.size();
    if (starts_with(s, kParamStart)) {
      gt = text.find('>', pos);
      if (gt == kNpos) return -1;
      const std::ptrdiff_t end = param_end(text.substr(gt + 1U), false);
      if (end < 0) return -1;
      pos = gt + 1U + static_cast<std::size_t>(end) + kParamEnd.size();
    } else if (starts_with(s, kFuncEnd)) {
      const std::string_view after = text.substr(pos + kFuncEnd.size());
      s = python_lstrip(after);
      if (starts_with(s, kCallEnd)) return signed_at(pos + kFuncEnd.size() + after.size() - s.size());
      return prefix_of(s, kCallEnd) ? -1 : find(text, kCallEnd, pos);
    } else if (s.empty() || prefix_of(s, kParamStart) || prefix_of(s, kFuncEnd)) {
      return -1;
    } else {
      return find(text, kCallEnd, pos);
    }
  }
}

// function_end: just past the </function> of a <function=NAME>...</function> at the start of `text`; -1: not complete
// (or not in the call format).
std::ptrdiff_t function_end(const std::string_view text) {
  std::string_view s = python_lstrip(text);
  std::size_t pos = text.size() - s.size();
  if (!starts_with(s, kFuncStart)) return -1;
  std::size_t gt = text.find('>', pos);
  if (gt == kNpos) return -1;
  pos = gt + 1U;
  for (;;) {
    const std::string_view rest = text.substr(pos);
    s = python_lstrip(rest);
    pos += rest.size() - s.size();
    if (starts_with(s, kParamStart)) {
      gt = text.find('>', pos);
      if (gt == kNpos) return -1;
      const std::ptrdiff_t end = param_end(text.substr(gt + 1U), false);
      if (end < 0) return -1;
      pos = gt + 1U + static_cast<std::size_t>(end) + kParamEnd.size();
    } else if (starts_with(s, kFuncEnd)) {
      return signed_at(pos + kFuncEnd.size());
    } else {
      return -1;
    }
  }
}

bool python_space_at(const std::string_view text, const std::size_t at) { return python_space(utf8_code_point(text, at).first); }

// The end of a run of [^<>\s] characters starting at `at`.
std::size_t name_run_end(const std::string_view text, std::size_t at) {
  while (at < text.size() && text[at] != '<' && text[at] != '>' && !python_space_at(text, at)) at += utf8_code_point(text, at).second;
  return at;
}

// re.match(r"<function=(\s*[^<>\s]+\s*)>", text): the group (the raw name), else nullopt.
std::optional<std::string_view> call_opener(const std::string_view text) {
  if (!starts_with(text, kFuncStart)) return std::nullopt;
  const std::size_t name = python_space_end(text, kFuncStart.size());
  const std::size_t run = name_run_end(text, name);
  if (run == name) return std::nullopt;
  const std::size_t close = python_space_end(text, run);
  if (close >= text.size() || text[close] != '>') return std::nullopt;
  return text.substr(kFuncStart.size(), close - kFuncStart.size());
}

// re.fullmatch(r"<function=\s*[^<>\s]*\s*", text): an opener whose name may still be arriving.
bool partial_opener(const std::string_view text) {
  if (!starts_with(text, kFuncStart)) return false;
  return python_space_end(text, name_run_end(text, python_space_end(text, kFuncStart.size()))) == text.size();
}

bool name_char(const char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}

struct LooseOpener final {
  std::string name;
  std::size_t length{0};
};

// _LOOSE_OPEN.match: <\s*(?:function\s*[=:]\s*|=\s*)?([A-Za-z0-9_.\-]+)\s*> - the drifted openers (#3441).
std::optional<LooseOpener> loose_opener(const std::string_view text) {
  if (!starts_with(text, "<")) return std::nullopt;
  const auto named = [&text](const std::size_t at) -> std::optional<LooseOpener> {
    std::size_t end = at;
    while (end < text.size() && name_char(text[end])) ++end;
    if (end == at) return std::nullopt;
    const std::size_t close = python_space_end(text, end);
    if (close >= text.size() || text[close] != '>') return std::nullopt;
    return LooseOpener{std::string{text.substr(at, end - at)}, close + 1U};
  };
  const std::size_t p = python_space_end(text, 1U);
  if (starts_with(text.substr(p), "function")) {
    const std::size_t q = python_space_end(text, p + 8U);
    if (q < text.size() && (text[q] == '=' || text[q] == ':')) {
      if (auto found = named(python_space_end(text, q + 1U))) return found;
    }
  }
  if (p < text.size() && text[p] == '=') {
    if (auto found = named(python_space_end(text, p + 1U))) return found;
  }
  return named(p);
}

// _LOOSE_PART.match: <\s*(?:function[ \t]*[=:]?\s*|=\s*)?[A-Za-z0-9_.\-]*\s*$ - one that may still become a drifted opener.
bool loose_partial(const std::string_view text) {
  if (!starts_with(text, "<")) return false;
  const auto tail = [&text](std::size_t at) {
    while (at < text.size() && name_char(text[at])) ++at;
    return python_space_end(text, at) == text.size();
  };
  const std::size_t p = python_space_end(text, 1U);
  if (starts_with(text.substr(p), "function")) {
    std::size_t q = p + 8U;
    while (q < text.size() && (text[q] == ' ' || text[q] == '\t')) ++q;
    if (q < text.size() && (text[q] == '=' || text[q] == ':')) ++q;
    if (tail(python_space_end(text, q))) return true;
  }
  if (p < text.size() && text[p] == '=' && tail(python_space_end(text, p + 1U))) return true;
  return tail(p);
}

// _hold: the longest suffix of `text` that is a proper prefix of `tag`.
std::size_t hold_length(const std::string_view text, const std::string_view tag) {
  for (std::size_t k = std::min(tag.size() - 1U, text.size()); k > 0U; --k) {
    if (text.substr(text.size() - k) == tag.substr(0U, k)) return k;
  }
  return 0U;
}

// json.loads, else nullopt (the text stays text).
std::optional<JsonValue> json_loads(const std::string_view text) {
  const JsonLimits limits{.max_bytes = text.size() + 1U, .max_depth = 64U, .max_container_entries = text.size() + 1U,
                          .max_string_bytes = text.size() + 1U, .allow_fractions = true};
  auto parsed = parse_json(text, limits);
  if (auto* value = std::get_if<JsonValue>(&parsed)) return std::move(*value);
  return std::nullopt;
}

}  // namespace

std::optional<ToolCall> parse_qwen_tool_call(const std::string_view raw_body, const ToolDeclaration* const declaration) {
  const std::string_view body = python_strip(raw_body);
  const std::size_t gt = body.find('>');
  if (!starts_with(body, kFuncStart) || gt == kNpos) return std::nullopt;
  ToolCall call{std::string{python_strip(body.substr(kFuncStart.size(), gt - kFuncStart.size()))}, {}};
  std::string_view rest = body.substr(gt + 1U);
  for (std::size_t at = rest.find(kParamStart); at != kNpos; at = rest.find(kParamStart)) {
    rest = rest.substr(at + kParamStart.size());
    const std::size_t name_end = rest.find('>');
    if (name_end == kNpos) return std::nullopt;  // rest.index(">"): ValueError
    std::string name{rest.substr(0U, name_end)};
    rest = rest.substr(name_end + 1U);
    const std::ptrdiff_t end = param_end(rest, true);
    std::string_view value = end >= 0 ? rest.substr(0U, static_cast<std::size_t>(end)) : rest;
    rest = end >= 0 ? rest.substr(static_cast<std::size_t>(end) + kParamEnd.size()) : std::string_view{};
    if (starts_with(value, "\n")) value.remove_prefix(1U);
    if (!value.empty() && value.back() == '\n') value.remove_suffix(1U);
    const bool is_string = declaration != nullptr && std::find(declaration->string_parameters.begin(),
                                                               declaration->string_parameters.end(), name) !=
                                                         declaration->string_parameters.end();
    std::optional<JsonValue> decoded = is_string ? std::nullopt : json_loads(value);
    JsonValue parsed = decoded ? std::move(*decoded) : JsonValue::make_string(std::string{value});
    // args[pname] = ...: a repeated name keeps its first place and takes the last value
    const auto same = std::find_if(call.arguments.begin(), call.arguments.end(), [&name](const ToolArgument& a) { return a.name == name; });
    if (same != call.arguments.end()) same->value = std::move(parsed);
    else call.arguments.push_back({std::move(name), std::move(parsed)});
  }
  return call;
}

QwenToolCallParser::QwenToolCallParser(std::vector<ToolDeclaration> tools) : tools_(std::move(tools)) {}

const ToolDeclaration* QwenToolCallParser::declared(const std::string_view name) const {
  for (const auto& tool : tools_) {
    if (tool.name == name) return &tool;
  }
  return nullptr;
}

void QwenToolCallParser::emit(std::vector<ToolParseEvent>& out, const ToolParseEvent::Kind kind, std::string text) {
  if (text.empty()) return;
  tracking_.track(text);
  out.push_back({kind, std::move(text), {}});
}

// _release: the calls waiting from the reasoning become calls, or all of it goes back out as reasoning text.
void QwenToolCallParser::release(std::vector<ToolParseEvent>& out, const bool deliver) {
  std::vector<Pending> waiting = std::move(pending_);
  pending_.clear();
  for (auto& p : waiting) {
    if (p.call.has_value() && deliver) out.push_back({ToolParseEvent::Kind::call, {}, std::move(*p.call)});
    else emit(out, ToolParseEvent::Kind::reasoning, std::move(p.raw));
  }
}

// What the reasoning still holds when it ends: a call being held never was one; waiting calls are calls when only
// whitespace followed them.
void QwenToolCallParser::close_reasoning(std::vector<ToolParseEvent>& out) {
  if (state_ == State::rcall) {
    release(out, false);
  } else if (state_ == State::reasoning && !pending_.empty()) {
    const std::size_t space = python_space_end(buf_);
    pending_.back().raw += buf_.substr(0U, space);
    buf_.erase(0U, space);
    release(out, buf_.empty());
  }
  if (state_ == State::reasoning || state_ == State::rcall) {
    emit(out, ToolParseEvent::Kind::reasoning, std::move(buf_));
    buf_.clear();
  }
}

void QwenToolCallParser::start_content(const bool lead) {
  state_ = State::content;
  lead_ = lead;
  tracking_ = Tracking{};  // the answer's own text starts here
}

std::vector<ToolParseEvent> QwenToolCallParser::feed_reasoning(const std::string_view text) {
  std::vector<ToolParseEvent> out;
  if (state_ == State::content || state_ == State::call) {
    // A <think> while answering (our generation run opens a new reasoning span): what the answer held is its text.
    emit(out, ToolParseEvent::Kind::content, state_ == State::call ? std::string{kCallStart} + buf_ : buf_);
    buf_.clear();
    state_ = State::reasoning;
    lead_ = false;
    tracking_ = Tracking{};
  }
  buf_.append(text);
  run_reasoning(out);
  return out;
}

std::vector<ToolParseEvent> QwenToolCallParser::end_reasoning() {
  std::vector<ToolParseEvent> out;
  if (state_ == State::reasoning || state_ == State::rcall) {
    close_reasoning(out);
    start_content(true);
  }
  return out;
}

std::vector<ToolParseEvent> QwenToolCallParser::feed_content(const std::string_view text) {
  std::vector<ToolParseEvent> out;
  if (state_ == State::reasoning || state_ == State::rcall) {
    close_reasoning(out);
    start_content(false);
  }
  buf_.append(text);
  run_content(out);
  return out;
}

void QwenToolCallParser::run_reasoning(std::vector<ToolParseEvent>& out) {
  for (;;) {
    if (state_ == State::rcall) {
      // #804: a <tool_call> inside the reasoning, held whole until it ends: a declared name is then a call, waiting to
      // be settled by what follows; anything else stays reasoning text.
      const std::string_view body = std::string_view{buf_}.substr(kCallStart.size());
      const std::ptrdiff_t end = call_end(body);
      if (end >= 0) {
        const auto at = static_cast<std::size_t>(end);
        const std::string_view head = python_strip(body.substr(0U, at));
        const std::string_view after_open = head.substr(std::min(kFuncStart.size(), head.size()));
        const std::string_view name = python_strip(after_open.substr(0U, after_open.find('>')));
        std::optional<ToolCall> call;
        if (const ToolDeclaration* tool = declared(name); tool != nullptr) {
          call = parse_qwen_tool_call(body.substr(0U, at), tool);
          if (call) call->name = std::string{name};
        }
        std::string raw = buf_.substr(0U, kCallStart.size() + at + kCallEnd.size());
        std::string rest{body.substr(at + kCallEnd.size())};
        if (call.has_value() || !pending_.empty()) pending_.push_back({std::move(raw), std::move(call)});
        else emit(out, ToolParseEvent::Kind::reasoning, std::move(raw));
        buf_ = std::move(rest);
        state_ = State::reasoning;
      } else if (python_length(body) > kRcallMax) {  // a tag in the prose that never closes: stop holding it back
        release(out, false);
        emit(out, ToolParseEvent::Kind::reasoning, std::move(buf_));
        buf_.clear();
        state_ = State::reasoning;
      } else {
        return;
      }
      continue;
    }
    if (!pending_.empty()) {
      // the calls wait for what follows: more calls or whitespace keep them, any other text means they were quoted
      const std::size_t space = python_space_end(buf_);
      pending_.back().raw += buf_.substr(0U, space);
      buf_.erase(0U, space);
      if (buf_.empty()) return;
      if (starts_with(buf_, kCallStart)) state_ = State::rcall;
      else if (prefix_of(buf_, kCallStart)) return;
      else release(out, false);
      continue;
    }
    const std::size_t tool = buf_.find(kCallStart);
    if (tool != kNpos) {
      emit(out, ToolParseEvent::Kind::reasoning, buf_.substr(0U, tool));
      if (tracking_.opener_ok()) {
        buf_.erase(0U, tool);
        state_ = State::rcall;
      } else {  // mid-sentence, in a fence or in inline code: a quote
        emit(out, ToolParseEvent::Kind::reasoning, std::string{kCallStart});
        buf_.erase(0U, tool + kCallStart.size());
      }
      continue;
    }
    const std::size_t keep = hold_length(buf_, kCallStart);
    emit(out, ToolParseEvent::Kind::reasoning, buf_.substr(0U, buf_.size() - keep));
    buf_.erase(0U, buf_.size() - keep);
    return;
  }
}

// Text up to `at` goes out, less the newlines right before it (dropped if a call follows); the rest is held.
void QwenToolCallParser::hold_before(std::vector<ToolParseEvent>& out, std::size_t at) {
  while (at > 0U && buf_[at - 1U] == '\n') --at;
  if (at == 0U) return;
  emit(out, ToolParseEvent::Kind::content, buf_.substr(0U, at));
  buf_.erase(0U, at);
}

// _wrapped_call_end: call_end, searching only the text that arrived since the last look while no </tool_call> has.
std::ptrdiff_t QwenToolCallParser::wrapped_call_end() {
  if (!call_end_seen_) {
    const std::size_t from = call_end_checked_ >= kCallEnd.size() ? call_end_checked_ - kCallEnd.size() + 1U : 0U;
    if (buf_.find(kCallEnd, from) == kNpos) {
      call_end_checked_ = buf_.size();
      return -1;
    }
    call_end_seen_ = true;
  }
  return call_end(buf_);
}

void QwenToolCallParser::finish_call(std::vector<ToolParseEvent>& out, const std::string& body) {
  const std::string_view head = python_strip(body);
  const std::string_view after_open = head.substr(std::min(kFuncStart.size(), head.size()));
  const std::string_view raw_name = after_open.substr(0U, after_open.find('>'));
  const std::string_view name = python_strip(raw_name);
  call_end_checked_ = 0U;
  call_end_seen_ = false;
  std::optional<ToolCall> call;
  if (raw_name == name || declared(name) != nullptr) call = parse_qwen_tool_call(body, declared(name));
  if (!call) {  // upstream's recover path: a body that does not parse is the text it was
    state_ = State::content;
    emit(out, ToolParseEvent::Kind::content, std::string{kCallStart} + body + std::string{kCallEnd});
    return;
  }
  call->name = std::string{name};
  out.push_back({ToolParseEvent::Kind::call, {}, std::move(*call)});
  state_ = State::content;
  lead_ = true;
}

void QwenToolCallParser::run_content(std::vector<ToolParseEvent>& out) {
  for (;;) {
    if (state_ == State::call) {
      std::ptrdiff_t end = wrapped_call_end();
      std::optional<std::size_t> drop;
      if (end >= 0) drop = kCallEnd.size();
      // only a following call opener confirms an omitted wrapper closer
      if (const std::ptrdiff_t f = function_end(buf_); f >= 0) {
        const std::string_view next = python_lstrip(std::string_view{buf_}.substr(static_cast<std::size_t>(f)));
        if (starts_with(next, kCallStart) && call_opener(python_lstrip(next.substr(kCallStart.size())))) {
          end = f;
          drop = 0U;
        }
      }
      if (end < 0 || !drop) return;
      const std::string body = buf_.substr(0U, static_cast<std::size_t>(end));
      buf_.erase(0U, static_cast<std::size_t>(end) + *drop);
      finish_call(out, body);
      continue;
    }
    if (lead_) {  // the newlines right after </think> or a call
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
      hold_before(out, buf_.size() - hold_length(buf_, kCallStart));
      return;
    }
    // #1058: an opener inside a code fence or inline code is text (a quoted example), never a call
    Tracking before = tracking_;
    before.track(std::string_view{buf_}.substr(0U, i));
    if (before.in_code()) {
      emit(out, ToolParseEvent::Kind::content, buf_.substr(0U, i + kCallStart.size()));
      buf_.erase(0U, i + kCallStart.size());
      continue;
    }
    const std::string_view after = python_lstrip(std::string_view{buf_}.substr(i + kCallStart.size()));
    if (!starts_with(after, kFuncStart)) {
      // #3441: `<=NAME>`, `<function:NAME>`, `<function =NAME>` or `<NAME>` for a declared tool is `<function=NAME>`
      if (const auto loose = loose_opener(after)) {
        if (declared(loose->name) != nullptr) {
          const std::size_t at = buf_.size() - after.size();
          buf_ = buf_.substr(0U, at) + std::string{kFuncStart} + loose->name + ">" + buf_.substr(at + loose->length);
          continue;
        }
      } else if (after.find('>') == kNpos && after.find('\n') == kNpos && starts_with(after, "<") && loose_partial(after)) {
        std::string_view bare = python_strip(after.substr(1U));  // after[1:].strip().lstrip("=").strip()
        while (!bare.empty() && bare.front() == '=') bare.remove_prefix(1U);
        bare = python_strip(bare);
        const std::string_view opened = python_lstrip(after.substr(1U));
        bool wait = starts_with(opened, "=") || starts_with(opened, "function");
        for (const auto& tool : tools_) wait = wait || starts_with(tool.name, bare);
        if (wait) {  // may still become one: held like a partial tag
          hold_before(out, i);
          return;
        }
      }
    }
    std::optional<std::string_view> raw_name = call_opener(after);
    if (raw_name && *raw_name != python_strip(*raw_name) && declared(python_strip(*raw_name)) == nullptr) {
      raw_name.reset();  // only declared names gain whitespace normalization
    }
    const bool partial = prefix_of(after, kFuncStart) || partial_opener(after);
    if (!raw_name && !partial) {  // prose that names the tag: text
      emit(out, ToolParseEvent::Kind::content, buf_.substr(0U, i + kCallStart.size()));
      buf_.erase(0U, i + kCallStart.size());
      continue;
    }
    if (!raw_name) {
      hold_before(out, i);
      return;
    }
    // #3075: a call counts only for a declared tool
    const std::size_t gt = after.find('>');
    const std::size_t nl = after.find('\n');
    if (gt != kNpos && (nl == kNpos || gt < nl) &&
        declared(python_strip(after.substr(kFuncStart.size(), gt - kFuncStart.size()))) == nullptr) {
      emit(out, ToolParseEvent::Kind::content, buf_.substr(0U, i + kCallStart.size()));
      buf_.erase(0U, i + kCallStart.size());
      continue;
    }
    if (i > 0U && !python_strip(std::string_view{buf_}.substr(0U, i)).empty()) {
      std::string before_call = buf_.substr(0U, i);
      while (!before_call.empty() && before_call.back() == '\n') before_call.pop_back();
      emit(out, ToolParseEvent::Kind::content, std::move(before_call));
    }
    buf_.erase(0U, i + kCallStart.size());
    state_ = State::call;
    call_end_checked_ = 0U;
    call_end_seen_ = false;
  }
}

std::vector<ToolParseEvent> QwenToolCallParser::finish(const bool natural) {
  std::vector<ToolParseEvent> out;
  if (!pending_.empty()) release(out, natural && state_ == State::reasoning && buf_.empty());
  if (!buf_.empty()) {
    // an unfinished call inside the reasoning is reasoning text; one in the answer is text with its <tool_call>
    const auto kind = state_ == State::reasoning || state_ == State::rcall ? ToolParseEvent::Kind::reasoning : ToolParseEvent::Kind::content;
    emit(out, kind, state_ == State::call ? std::string{kCallStart} + buf_ : buf_);
    buf_.clear();
  }
  return out;
}

}  // namespace secure_frontend
