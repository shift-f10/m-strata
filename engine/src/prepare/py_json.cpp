#include "py_json.hpp"

#include <charconv>
#include <cstdio>
#include <system_error>

namespace strata_prepare {

Json Json::boolean(const bool value) {
  Json j;
  j.kind_ = Kind::boolean;
  j.boolean_ = value;
  return j;
}
Json Json::integer(const std::int64_t value) {
  Json j;
  j.kind_ = Kind::integer;
  j.integer_ = value;
  return j;
}
Json Json::real(const double value) {
  Json j;
  j.kind_ = Kind::real;
  j.real_ = value;
  return j;
}
Json Json::text(std::string value) {
  Json j;
  j.kind_ = Kind::text;
  j.text_ = std::move(value);
  return j;
}
Json Json::list() {
  Json j;
  j.kind_ = Kind::list;
  return j;
}
Json Json::object() {
  Json j;
  j.kind_ = Kind::object;
  return j;
}
Json& Json::push(Json value) {
  items_.push_back(std::move(value));
  return *this;
}
Json& Json::set(std::string key, Json value) {
  for (auto& member : members_) {
    if (member.first == key) {
      member.second = std::move(value);
      return *this;
    }
  }
  members_.emplace_back(std::move(key), std::move(value));
  return *this;
}

Json& Json::merge(const Json& other) {
  for (const auto& [key, value] : other.members_) set(key, value);
  return *this;
}

std::string Json::member_text(const std::string& key) const {
  for (const auto& [name, value] : members_) {
    if (name == key && value.kind_ == Kind::text) return value.text_;
  }
  return {};
}
bool Json::member_bool(const std::string& key) const {
  for (const auto& [name, value] : members_) {
    if (name == key && value.kind_ == Kind::boolean) return value.boolean_;
  }
  return false;
}
double Json::member_real(const std::string& key) const {
  for (const auto& [name, value] : members_) {
    if (name == key && value.kind_ == Kind::real) return value.real_;
  }
  return 0.0;
}

std::string python_float_repr(const double value) {
  // the shortest round-trip digits and their decimal exponent, from scientific form "d.ddde[+-]XX"
  char buffer[64];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
  if (error != std::errc{}) return "0.0";
  const std::string sci(buffer, end);
  const auto e_at = sci.find('e');
  std::string mantissa = sci.substr(0, e_at);
  const int exponent = std::stoi(sci.substr(e_at + 1));
  std::string sign;
  if (!mantissa.empty() && mantissa.front() == '-') {
    sign = "-";
    mantissa.erase(0, 1);
  }
  std::string digits;
  for (const char c : mantissa) {
    if (c != '.') digits.push_back(c);
  }
  // Python's float_repr_style 'short': fixed for -4 <= exponent < 16, else d[.ddd]e[+-]XX
  if (exponent < -4 || exponent >= 16) {
    std::string out = sign + digits.substr(0, 1);
    if (digits.size() > 1U) out += "." + digits.substr(1);
    char exp[8];
    std::snprintf(exp, sizeof(exp), "e%c%02d", exponent < 0 ? '-' : '+', exponent < 0 ? -exponent : exponent);
    return out + exp;
  }
  std::string out;
  if (exponent < 0) {
    out = "0." + std::string(static_cast<std::size_t>(-exponent - 1), '0') + digits;
  } else {
    const auto whole = static_cast<std::size_t>(exponent) + 1U;
    if (digits.size() <= whole) {
      out = digits + std::string(whole - digits.size(), '0') + ".0";
    } else {
      out = digits.substr(0, whole) + "." + digits.substr(whole);
    }
  }
  return sign + out;
}

std::string python_json_string(const std::string& utf8, const bool ensure_ascii) {
  std::string out = "\"";
  const auto escape = [&out](const unsigned value) {
    char hex[8];
    std::snprintf(hex, sizeof(hex), "\\u%04x", value);
    out += hex;
  };
  for (std::size_t i = 0; i < utf8.size();) {
    const auto c = static_cast<unsigned char>(utf8[i]);
    if (c < 0x80U || !ensure_ascii) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
          if (c < 0x20U) escape(c);
          else out.push_back(static_cast<char>(c));
      }
      ++i;
      continue;
    }
    // a UTF-8 sequence -> \uXXXX (a surrogate pair above the BMP); an invalid byte as U+FFFD (Python's "replace")
    unsigned point = 0xFFFDU;
    std::size_t width = 1;
    if ((c & 0xE0U) == 0xC0U && i + 1 < utf8.size()) {
      point = ((c & 0x1FU) << 6U) | (static_cast<unsigned char>(utf8[i + 1]) & 0x3FU);
      width = 2;
    } else if ((c & 0xF0U) == 0xE0U && i + 2 < utf8.size()) {
      point = ((c & 0x0FU) << 12U) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3FU) << 6U) |
              (static_cast<unsigned char>(utf8[i + 2]) & 0x3FU);
      width = 3;
    } else if ((c & 0xF8U) == 0xF0U && i + 3 < utf8.size()) {
      point = ((c & 0x07U) << 18U) | ((static_cast<unsigned char>(utf8[i + 1]) & 0x3FU) << 12U) |
              ((static_cast<unsigned char>(utf8[i + 2]) & 0x3FU) << 6U) | (static_cast<unsigned char>(utf8[i + 3]) & 0x3FU);
      width = 4;
    }
    if (point >= 0x10000U) {
      const unsigned v = point - 0x10000U;
      escape(0xD800U + (v >> 10U));
      escape(0xDC00U + (v & 0x3FFU));
    } else {
      escape(point);
    }
    i += width;
  }
  return out + "\"";
}

std::string python_repr(const std::string& utf8) {
  const char quote = utf8.find('\'') != std::string::npos && utf8.find('"') == std::string::npos ? '"' : '\'';
  std::string out(1, quote);
  for (const char ch : utf8) {
    const auto c = static_cast<unsigned char>(ch);
    if (ch == quote || ch == '\\') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (ch == '\n') {
      out += "\\n";
    } else if (ch == '\r') {
      out += "\\r";
    } else if (ch == '\t') {
      out += "\\t";
    } else if (c < 0x20U || c == 0x7fU) {
      char hex[8];
      std::snprintf(hex, sizeof(hex), "\\x%02x", c);
      out += hex;
    } else {
      out.push_back(ch);
    }
  }
  return out + quote;
}

void Json::write(std::string& out, const int indent, const int level, const bool ensure_ascii) const {
  const auto newline = [&](const int depth) {
    if (indent < 0) return;
    out += "\n";
    out += std::string(static_cast<std::size_t>(indent * depth), ' ');
  };
  switch (kind_) {
    case Kind::null: out += "null"; return;
    case Kind::boolean: out += boolean_ ? "true" : "false"; return;
    case Kind::integer: out += std::to_string(integer_); return;
    case Kind::real: out += python_float_repr(real_); return;
    case Kind::text: out += python_json_string(text_, ensure_ascii); return;
    case Kind::list:
      if (items_.empty()) {
        out += "[]";
        return;
      }
      out += "[";
      for (std::size_t i = 0; i < items_.size(); ++i) {
        if (i != 0U) out += indent < 0 ? ", " : ",";
        newline(level + 1);
        items_[i].write(out, indent, level + 1, ensure_ascii);
      }
      newline(level);
      out += "]";
      return;
    case Kind::object:
      if (members_.empty()) {
        out += "{}";
        return;
      }
      out += "{";
      for (std::size_t i = 0; i < members_.size(); ++i) {
        if (i != 0U) out += indent < 0 ? ", " : ",";
        newline(level + 1);
        out += python_json_string(members_[i].first, ensure_ascii) + ": ";
        members_[i].second.write(out, indent, level + 1, ensure_ascii);
      }
      newline(level);
      out += "}";
      return;
  }
}

std::string Json::dumps(const int indent, const bool ensure_ascii) const {
  std::string out;
  write(out, indent, 0, ensure_ascii);
  return out;
}

}  // namespace strata_prepare
