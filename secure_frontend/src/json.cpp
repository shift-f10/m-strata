#include "secure_frontend/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace secure_frontend {

JsonValue JsonValue::make_boolean(const bool value) { JsonValue v; v.kind_ = JsonKind::boolean; v.boolean_ = value; return v; }
JsonValue JsonValue::make_integer(const std::int64_t value) { JsonValue v; v.kind_ = JsonKind::integer; v.integer_ = value; return v; }
JsonValue JsonValue::make_decimal(const double value) { JsonValue v; v.kind_ = JsonKind::decimal; v.decimal_ = value; return v; }
JsonValue JsonValue::make_string(std::string value) { JsonValue v; v.kind_ = JsonKind::string; v.string_ = std::move(value); return v; }
JsonValue JsonValue::make_array(std::vector<JsonValue> items) { JsonValue v; v.kind_ = JsonKind::array; v.array_ = std::move(items); return v; }
JsonValue JsonValue::make_object(std::vector<JsonMember> members) { JsonValue v; v.kind_ = JsonKind::object; v.object_ = std::move(members); return v; }

namespace {

bool is_continuation(const unsigned char byte) { return (byte & 0xc0U) == 0x80U; }

bool valid_utf8(const std::string_view value) {
  for (std::size_t i = 0; i < value.size();) {
    const auto b = static_cast<unsigned char>(value[i]);
    const auto at = [&](const std::size_t k) { return static_cast<unsigned char>(value[i + k]); };
    const std::size_t left = value.size() - i;
    if (b <= 0x7fU) { i += 1U; continue; }
    if (b >= 0xc2U && b <= 0xdfU && left >= 2U && is_continuation(at(1))) { i += 2U; continue; }
    if (b == 0xe0U && left >= 3U && at(1) >= 0xa0U && at(1) <= 0xbfU && is_continuation(at(2))) { i += 3U; continue; }
    if (((b >= 0xe1U && b <= 0xecU) || b == 0xeeU || b == 0xefU) && left >= 3U && is_continuation(at(1)) &&
        is_continuation(at(2))) { i += 3U; continue; }
    if (b == 0xedU && left >= 3U && at(1) >= 0x80U && at(1) <= 0x9fU && is_continuation(at(2))) { i += 3U; continue; }
    if (b == 0xf0U && left >= 4U && at(1) >= 0x90U && at(1) <= 0xbfU && is_continuation(at(2)) && is_continuation(at(3))) { i += 4U; continue; }
    if (b >= 0xf1U && b <= 0xf3U && left >= 4U && is_continuation(at(1)) && is_continuation(at(2)) && is_continuation(at(3))) { i += 4U; continue; }
    if (b == 0xf4U && left >= 4U && at(1) >= 0x80U && at(1) <= 0x8fU && is_continuation(at(2)) && is_continuation(at(3))) { i += 4U; continue; }
    return false;
  }
  return true;
}

void append_utf8(std::string* out, const std::uint32_t point) {
  if (point <= 0x7fU) {
    out->push_back(static_cast<char>(point));
  } else if (point <= 0x7ffU) {
    out->push_back(static_cast<char>(0xc0U | (point >> 6U)));
    out->push_back(static_cast<char>(0x80U | (point & 0x3fU)));
  } else if (point <= 0xffffU) {
    out->push_back(static_cast<char>(0xe0U | (point >> 12U)));
    out->push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3fU)));
    out->push_back(static_cast<char>(0x80U | (point & 0x3fU)));
  } else {
    out->push_back(static_cast<char>(0xf0U | (point >> 18U)));
    out->push_back(static_cast<char>(0x80U | ((point >> 12U) & 0x3fU)));
    out->push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3fU)));
    out->push_back(static_cast<char>(0x80U | (point & 0x3fU)));
  }
}

class Parser final {
 public:
  Parser(const std::string_view text, const JsonLimits& limits) : text_(text), limits_(limits) {}

  JsonParseResult run() {
    if (text_.size() > limits_.max_bytes) return JsonError{JsonErrorCode::too_large, 0U};
    if (text_.size() >= 3U && static_cast<unsigned char>(text_[0]) == 0xefU &&
        static_cast<unsigned char>(text_[1]) == 0xbbU && static_cast<unsigned char>(text_[2]) == 0xbfU) {
      return JsonError{JsonErrorCode::byte_order_mark, 0U};
    }
    JsonValue value;
    if (!parse_value(0U, value)) return error_;
    skip();
    if (position_ != text_.size()) return JsonError{JsonErrorCode::trailing_data, position_};
    return value;
  }

 private:
  bool fail(const JsonErrorCode code, const std::size_t offset) {
    error_ = JsonError{code, offset};
    return false;
  }
  bool fail(const JsonErrorCode code) { return fail(code, position_); }
  bool at_end() const { return position_ >= text_.size(); }
  void skip() {
    while (!at_end() && (text_[position_] == ' ' || text_[position_] == '\t' || text_[position_] == '\n' ||
                         text_[position_] == '\r')) ++position_;
  }
  bool literal(const std::string_view word) {
    if (text_.substr(position_, word.size()) != word) return false;
    position_ += word.size();
    return true;
  }

  bool parse_value(const std::size_t depth, JsonValue& out) {
    skip();
    if (at_end()) return fail(JsonErrorCode::syntax);
    const char c = text_[position_];
    if (c == '{' || c == '[') {
      if (depth + 1U > limits_.max_depth) return fail(JsonErrorCode::depth_exceeded);
      return c == '{' ? parse_object(depth + 1U, out) : parse_array(depth + 1U, out);
    }
    if (c == '"') {
      std::string text;
      if (!parse_string(text)) return false;
      out = JsonValue::make_string(std::move(text));
      return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
    if (literal("true")) { out = JsonValue::make_boolean(true); return true; }
    if (literal("false")) { out = JsonValue::make_boolean(false); return true; }
    if (literal("null")) { out = JsonValue{}; return true; }
    return fail(JsonErrorCode::syntax);
  }

  bool parse_number(JsonValue& out) {
    const std::size_t start = position_;
    const bool negative = text_[position_] == '-';
    if (negative) ++position_;
    if (at_end() || text_[position_] < '0' || text_[position_] > '9') return fail(JsonErrorCode::syntax, start);
    const std::size_t digits_start = position_;
    if (text_[position_] == '0') {
      ++position_;
    } else {
      while (!at_end() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
    }
    if (!at_end() && (text_[position_] == '.' || text_[position_] == 'e' || text_[position_] == 'E')) {
      if (!limits_.allow_fractions) return fail(JsonErrorCode::non_integer_number, start);
      return parse_decimal_tail(start, out);
    }
    if (!at_end() && text_[position_] >= '0' && text_[position_] <= '9') return fail(JsonErrorCode::syntax, start);
    std::uint64_t magnitude = 0U;
    for (std::size_t i = digits_start; i < position_; ++i) {
      const auto digit = static_cast<std::uint64_t>(text_[i] - '0');
      if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
        return fail(JsonErrorCode::integer_out_of_range, start);
      }
      magnitude = magnitude * 10U + digit;
    }
    constexpr std::uint64_t kMaxPositive = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (magnitude > kMaxPositive + (negative ? 1U : 0U)) return fail(JsonErrorCode::integer_out_of_range, start);
    std::int64_t value = 0;
    if (negative) {
      value = magnitude == kMaxPositive + 1U ? std::numeric_limits<std::int64_t>::min()
                                             : -static_cast<std::int64_t>(magnitude);
    } else {
      value = static_cast<std::int64_t>(magnitude);
    }
    out = JsonValue::make_integer(value);
    return true;
  }

  // RFC 8259 frac/exp after the integer part; the whole lexeme is converted
  // with from_chars, so "1.", ".5" and "1e" fail and 1e400 (inf) fails.
  bool parse_decimal_tail(const std::size_t start, JsonValue& out) {
    if (!at_end() && text_[position_] == '.') {
      ++position_;
      const std::size_t digits = position_;
      while (!at_end() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
      if (position_ == digits) return fail(JsonErrorCode::syntax, start);
    }
    if (!at_end() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      ++position_;
      if (!at_end() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
      const std::size_t digits = position_;
      while (!at_end() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
      if (position_ == digits) return fail(JsonErrorCode::syntax, start);
    }
    double value = 0.0;
    const char* first = text_.data() + start;
    const char* last = text_.data() + position_;
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || !std::isfinite(value)) return fail(JsonErrorCode::integer_out_of_range, start);
    out = JsonValue::make_decimal(value);
    return true;
  }

  bool hex4(std::uint32_t& out) {
    if (text_.size() - position_ < 4U) return fail(JsonErrorCode::invalid_string);
    out = 0U;
    for (int n = 0; n < 4; ++n) {
      const char c = text_[position_++];
      std::uint32_t digit = 0U;
      if (c >= '0' && c <= '9') digit = static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') digit = static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') digit = static_cast<std::uint32_t>(c - 'A' + 10);
      else return fail(JsonErrorCode::invalid_string);
      out = (out << 4U) | digit;
    }
    return true;
  }

  bool parse_string(std::string& out) {
    const std::size_t start = position_;
    ++position_;  // opening quote
    while (!at_end()) {
      const auto byte = static_cast<unsigned char>(text_[position_++]);
      if (byte == '"') return valid_utf8(out) ? true : fail(JsonErrorCode::invalid_string, start);
      if (byte < 0x20U) return fail(JsonErrorCode::invalid_string);
      if (byte != '\\') {
        out.push_back(static_cast<char>(byte));
      } else {
        if (at_end()) return fail(JsonErrorCode::invalid_string);
        switch (text_[position_++]) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            std::uint32_t point = 0U;
            if (!hex4(point)) return false;
            if (point >= 0xd800U && point <= 0xdbffU) {
              std::uint32_t low = 0U;
              if (text_.substr(position_, 2U) != "\\u") return fail(JsonErrorCode::invalid_string);
              position_ += 2U;
              if (!hex4(low)) return false;
              if (low < 0xdc00U || low > 0xdfffU) return fail(JsonErrorCode::invalid_string);
              point = 0x10000U + ((point - 0xd800U) << 10U) + (low - 0xdc00U);
            } else if (point >= 0xdc00U && point <= 0xdfffU) {
              return fail(JsonErrorCode::invalid_string);
            }
            append_utf8(&out, point);
            break;
          }
          default: return fail(JsonErrorCode::invalid_string);
        }
      }
      if (out.size() > limits_.max_string_bytes) return fail(JsonErrorCode::string_too_long, start);
    }
    return fail(JsonErrorCode::invalid_string, start);
  }

  bool parse_array(const std::size_t depth, JsonValue& out) {
    ++position_;
    std::vector<JsonValue> items;
    skip();
    if (!at_end() && text_[position_] == ']') { ++position_; out = JsonValue::make_array({}); return true; }
    while (true) {
      if (items.size() == limits_.max_container_entries) return fail(JsonErrorCode::too_many_entries);
      JsonValue item;
      if (!parse_value(depth, item)) return false;
      items.push_back(std::move(item));
      skip();
      if (at_end()) return fail(JsonErrorCode::syntax);
      if (text_[position_] == ']') { ++position_; break; }
      if (text_[position_] != ',') return fail(JsonErrorCode::syntax);
      ++position_;
    }
    out = JsonValue::make_array(std::move(items));
    return true;
  }

  bool parse_object(const std::size_t depth, JsonValue& out) {
    ++position_;
    std::vector<JsonMember> members;
    std::vector<std::size_t> key_offsets;
    skip();
    if (!at_end() && text_[position_] == '}') { ++position_; out = JsonValue::make_object({}); return true; }
    while (true) {
      if (members.size() == limits_.max_container_entries) return fail(JsonErrorCode::too_many_entries);
      skip();
      if (at_end() || text_[position_] != '"') return fail(JsonErrorCode::syntax);
      key_offsets.push_back(position_);
      JsonMember member;
      if (!parse_string(member.key)) return false;
      skip();
      if (at_end() || text_[position_] != ':') return fail(JsonErrorCode::syntax);
      ++position_;
      if (!parse_value(depth, member.value)) return false;
      members.push_back(std::move(member));
      skip();
      if (at_end()) return fail(JsonErrorCode::syntax);
      if (text_[position_] == '}') { ++position_; break; }
      if (text_[position_] != ',') return fail(JsonErrorCode::syntax);
      ++position_;
    }
    std::vector<std::size_t> order(members.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](const std::size_t a, const std::size_t b) {
      return members[a].key < members[b].key || (members[a].key == members[b].key && a < b);
    });
    for (std::size_t i = 1U; i < order.size(); ++i) {
      if (members[order[i]].key == members[order[i - 1U]].key) {
        return fail(JsonErrorCode::duplicate_key, key_offsets[order[i]]);
      }
    }
    out = JsonValue::make_object(std::move(members));
    return true;
  }

  std::string_view text_;
  const JsonLimits& limits_;
  std::size_t position_{0U};
  JsonError error_{JsonErrorCode::syntax, 0U};
};

}  // namespace

JsonParseResult parse_json(const std::string_view text, const JsonLimits& limits) { return Parser(text, limits).run(); }

std::string_view describe_json_error(const JsonErrorCode code) noexcept {
  switch (code) {
    case JsonErrorCode::too_large: return "is larger than the size limit";
    case JsonErrorCode::byte_order_mark: return "starts with a UTF-8 byte order mark";
    case JsonErrorCode::syntax: return "is not valid JSON";
    case JsonErrorCode::depth_exceeded: return "is nested too deeply";
    case JsonErrorCode::too_many_entries: return "has too many entries in one object or array";
    case JsonErrorCode::string_too_long: return "has a string longer than the limit";
    case JsonErrorCode::invalid_string: return "has an invalid string (control byte, bad escape or invalid UTF-8)";
    case JsonErrorCode::duplicate_key: return "has a duplicate key";
    case JsonErrorCode::non_integer_number: return "has a number that is not an integer";
    case JsonErrorCode::integer_out_of_range: return "has an integer outside the 64-bit range";
    case JsonErrorCode::trailing_data: return "has data after the document";
  }
  return "is not valid JSON";
}

bool is_valid_utf8(const std::string_view value) noexcept { return valid_utf8(value); }

void append_json_string(std::string* const output, const std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  output->push_back('"');
  for (const unsigned char byte : value) {
    switch (byte) {
      case '"': output->append("\\\""); break;
      case '\\': output->append("\\\\"); break;
      case '\b': output->append("\\b"); break;
      case '\f': output->append("\\f"); break;
      case '\n': output->append("\\n"); break;
      case '\r': output->append("\\r"); break;
      case '\t': output->append("\\t"); break;
      default:
        if (byte < 0x20U) {
          output->append("\\u00");
          output->push_back(kHex[(byte >> 4U) & 0x0fU]);
          output->push_back(kHex[byte & 0x0fU]);
        } else {
          output->push_back(static_cast<char>(byte));
        }
    }
  }
  output->push_back('"');
}

namespace {

// Python's repr of a float, as json.dumps prints one (float_repr_style "short", format_float_short with 'r'): the
// shortest round-trip digits, positional while the exponent is from -4 to 15 (".0" on a whole number), else d.ddde±XX.
std::string python_float(const double value) {
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return value < 0.0 ? "-Infinity" : "Infinity";
  char buffer[64];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific);
  if (error != std::errc{}) return "0.0";
  const std::string_view written(buffer, static_cast<std::size_t>(end - buffer));  // [-]d[.ddd]e(+|-)XX
  const std::size_t e_at = written.find('e');
  const bool negative = written.front() == '-';
  std::string digits;
  for (const char c : written.substr(negative ? 1U : 0U, e_at - (negative ? 1U : 0U))) {
    if (c != '.') digits.push_back(c);
  }
  int exponent = 0;
  const std::string_view exponent_text = written.substr(e_at + 1U);
  (void)std::from_chars(exponent_text.data() + (exponent_text.front() == '+' ? 1 : 0), exponent_text.data() + exponent_text.size(),
                        exponent);
  std::string out = negative ? "-" : "";
  if (exponent < -4 || exponent >= 16) {
    out += digits.front();
    if (digits.size() > 1U) out += "." + digits.substr(1U);
    const int magnitude = exponent < 0 ? -exponent : exponent;
    out += exponent < 0 ? "e-" : "e+";
    if (magnitude < 10) out += '0';
    out += std::to_string(magnitude);
  } else if (exponent >= 0) {
    const auto whole = static_cast<std::size_t>(exponent) + 1U;
    if (digits.size() < whole) digits.append(whole - digits.size(), '0');
    const std::string fraction = digits.substr(whole);
    out += digits.substr(0U, whole) + "." + (fraction.empty() ? "0" : fraction);
  } else {
    out += "0." + std::string(static_cast<std::size_t>(-exponent - 1), '0') + digits;
  }
  return out;
}

void dump(const JsonValue& value, std::string& out) {
  switch (value.kind()) {
    case JsonKind::null: out += "null"; return;
    case JsonKind::boolean: out += *value.if_boolean() ? "true" : "false"; return;
    case JsonKind::integer: out += std::to_string(*value.if_integer()); return;
    case JsonKind::decimal: out += python_float(*value.if_decimal()); return;
    case JsonKind::string: append_json_string(&out, *value.if_string()); return;
    case JsonKind::array: {
      out += '[';
      bool first = true;
      for (const auto& item : *value.if_array()) {
        if (!first) out += ", ";
        first = false;
        dump(item, out);
      }
      out += ']';
      return;
    }
    case JsonKind::object: {
      out += '{';
      bool first = true;
      for (const auto& m : *value.if_object()) {
        if (!first) out += ", ";
        first = false;
        append_json_string(&out, m.key);
        out += ": ";
        dump(m.value, out);
      }
      out += '}';
      return;
    }
  }
}

}  // namespace

std::string python_json_text(const JsonValue& value) {
  std::string out;
  dump(value, out);
  return out;
}

}  // namespace secure_frontend
