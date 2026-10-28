#include "secure_frontend/deepseek_tool_calls.hpp"

#include "secure_frontend/python_text.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace secure_frontend {
namespace {

constexpr std::string_view kDsml = "\xEF\xBD\x9C" "DSML\xEF\xBD\x9C";

std::string open_tag() { return "<" + std::string{kDsml}; }
std::string close_tag() { return "</" + std::string{kDsml}; }
// the only structural token inside a value (Maya's CLOSER)
std::string closer() { return close_tag() + " parameter>"; }

std::string angles_escaped(const std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (c == '<') out += "\\u003c";
    else out += c;
  }
  return out;
}

// not s.strip(): Python's whitespace, Unicode's beyond ASCII too
bool blank(const std::string_view text) { return python_strip(text).empty(); }

// _DSML_ATTR = (\w+)="([^"]*)", the last of a repeated name winning (a dict).
std::string attribute(const std::string_view body, const std::string_view name) {
  std::string found;
  for (std::size_t at = 0; (at = body.find("=\"", at)) != std::string_view::npos;) {
    std::size_t start = at;
    while (start > 0U && (std::isalnum(static_cast<unsigned char>(body[start - 1U])) != 0 || body[start - 1U] == '_')) --start;
    const std::size_t end = body.find('"', at + 2U);
    if (end == std::string_view::npos) break;
    if (start < at && body.substr(start, at - start) == name) found = std::string{body.substr(at + 2U, end - at - 2U)};
    at = end + 1U;
  }
  return found;
}

}  // namespace

std::string DsmlRewriter::feed(const std::string_view text) {
  buf_ += text;
  std::string out;
  const std::string opener = open_tag();
  const std::string closing_opener = close_tag();
  const std::string end_of_value = closer();
  while (!buf_.empty()) {
    if (in_param_) {
      // inside a value only the whole closing tag ends it; a suffix that may still grow into it is held
      const std::size_t at = buf_.find(end_of_value);
      if (at == std::string::npos) {
        std::size_t hold = 0U;
        for (std::size_t n = 1U; n <= std::min(end_of_value.size(), buf_.size()); ++n) {
          if (std::string_view{buf_}.substr(buf_.size() - n) == std::string_view{end_of_value}.substr(0, n)) hold = n;
        }
        if (buf_.size() > hold) {
          out += plain(std::string_view{buf_}.substr(0, buf_.size() - hold));
          buf_.erase(0, buf_.size() - hold);
        }
        break;
      }
      if (at != 0U) out += plain(std::string_view{buf_}.substr(0, at));
      buf_.erase(0, at + end_of_value.size());
      out += close_value();
      continue;
    }
    const std::size_t lt = buf_.find('<');
    if (lt == std::string::npos) {
      out += plain(buf_);
      buf_.clear();
      break;
    }
    if (lt > 0U) {
      out += plain(std::string_view{buf_}.substr(0, lt));
      buf_.erase(0, lt);
    }
    const bool closing = buf_.starts_with(closing_opener);
    if (!closing && !buf_.starts_with(opener)) {
      if (opener.starts_with(buf_) || closing_opener.starts_with(buf_)) break;  // could still become a tag: held
      out += plain("<");
      buf_.erase(0, 1);
      continue;
    }
    const std::size_t gt = buf_.find('>');
    if (gt == std::string::npos) break;  // the tag is not whole yet
    const std::string whole = buf_.substr(0, gt + 1U);
    buf_.erase(0, gt + 1U);
    const std::size_t prefix = closing ? closing_opener.size() : opener.size();
    out += tag(whole, closing, std::string_view{whole}.substr(prefix, whole.size() - prefix - 1U));
  }
  return out;
}

std::string DsmlRewriter::finish() {
  std::string rest;
  std::swap(rest, buf_);
  return plain(rest);
}

std::string DsmlRewriter::close_value() {
  in_param_ = false;
  if (param_string_) {
    param_text_.clear();
    return "\"</arg_value>";
  }
  std::string value = angles_escaped(param_text_);
  param_text_.clear();
  return value + "</arg_value>";
}

std::string DsmlRewriter::plain(const std::string_view text) {
  if (in_param_) {
    if (param_string_) return maya_json_escaped(text);  // a string value goes out as it is written
    param_text_ += text;
    return {};
  }
  if (in_calls_ && blank(text)) return {};
  return std::string{text};
}

std::string DsmlRewriter::tag(const std::string_view whole, const bool closing, const std::string_view body) {
  const std::string_view kind = python_first_word(body);  // body.split(None, 1)[0]
  if (in_param_ && !(closing && kind == "parameter")) return plain(whole);
  if (kind == "calls") {
    in_calls_ = !closing;
    return {};
  }
  if (kind == "invoke") return closing ? std::string{"</tool_call>"} : "<tool_call>" + attribute(body, "name");
  if (kind == "parameter") {
    in_param_ = !closing;
    if (closing) {
      std::string value = param_string_ ? std::string{"\""} : angles_escaped(param_text_);
      param_text_.clear();
      return value + "</arg_value>";
    }
    param_string_ = attribute(body, "string") == "true";
    param_text_.clear();
    return "<arg_key>" + attribute(body, "name") + "</arg_key><arg_value>" + (param_string_ ? "\"" : "");
  }
  return std::string{whole};
}

namespace {

// Maya reads a DSML call with no schema: every value is the JSON the rewriter wrote.
std::vector<ToolDeclaration> without_types(std::vector<ToolDeclaration> tools) {
  for (auto& tool : tools) tool.string_parameters.clear();
  return tools;
}

void append(std::vector<ToolParseEvent>& out, std::vector<ToolParseEvent> more) {
  for (auto& event : more) out.push_back(std::move(event));
}

}  // namespace

DeepseekToolCallParser::DeepseekToolCallParser(std::vector<ToolDeclaration> tools) : glm_(without_types(std::move(tools)), true) {}

std::vector<ToolParseEvent> DeepseekToolCallParser::feed_reasoning(const std::string_view text) {
  const std::string rewritten = rewriter_.feed(text);
  return rewritten.empty() ? std::vector<ToolParseEvent>{} : glm_.feed_reasoning(rewritten);
}

std::vector<ToolParseEvent> DeepseekToolCallParser::end_reasoning() {
  // what the rewriter held at </think> is reasoning text (Maya's rewriter sees </think> as text that ends no tag)
  std::vector<ToolParseEvent> out;
  if (const std::string held = rewriter_.finish(); !held.empty()) out = glm_.feed_reasoning(held);
  append(out, glm_.end_reasoning());
  answering_ = true;
  return out;
}

std::vector<ToolParseEvent> DeepseekToolCallParser::feed_content(const std::string_view text) {
  answering_ = true;
  const std::string rewritten = rewriter_.feed(text);
  return rewritten.empty() ? std::vector<ToolParseEvent>{} : glm_.feed_content(rewritten);
}

std::vector<ToolParseEvent> DeepseekToolCallParser::finish(const bool natural) {
  std::vector<ToolParseEvent> out;
  // what the rewriter still holds belongs to the channel the reply ended in
  if (const std::string held = rewriter_.finish(); !held.empty()) out = answering_ ? glm_.feed_content(held) : glm_.feed_reasoning(held);
  append(out, glm_.finish(natural));
  return out;
}

}  // namespace secure_frontend
