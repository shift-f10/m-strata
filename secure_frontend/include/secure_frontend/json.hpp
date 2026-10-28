#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

enum class JsonKind { null, boolean, integer, decimal, string, array, object };

struct JsonMember;

// An immutable JSON value produced by parse_json. Numbers are integers unless
// JsonLimits::allow_fractions: otherwise a fraction or exponent is a parse
// error, never a silent conversion.
class JsonValue final {
 public:
  JsonValue() = default;
  static JsonValue make_boolean(bool value);
  static JsonValue make_integer(std::int64_t value);
  static JsonValue make_decimal(double value);
  static JsonValue make_string(std::string value);
  static JsonValue make_array(std::vector<JsonValue> items);
  static JsonValue make_object(std::vector<JsonMember> members);

  [[nodiscard]] JsonKind kind() const noexcept { return kind_; }
  [[nodiscard]] const bool* if_boolean() const noexcept { return kind_ == JsonKind::boolean ? &boolean_ : nullptr; }
  [[nodiscard]] const std::int64_t* if_integer() const noexcept { return kind_ == JsonKind::integer ? &integer_ : nullptr; }
  [[nodiscard]] const double* if_decimal() const noexcept { return kind_ == JsonKind::decimal ? &decimal_ : nullptr; }
  [[nodiscard]] const std::string* if_string() const noexcept { return kind_ == JsonKind::string ? &string_ : nullptr; }
  [[nodiscard]] const std::vector<JsonValue>* if_array() const noexcept { return kind_ == JsonKind::array ? &array_ : nullptr; }
  [[nodiscard]] const std::vector<JsonMember>* if_object() const noexcept { return kind_ == JsonKind::object ? &object_ : nullptr; }

 private:
  JsonKind kind_{JsonKind::null};
  bool boolean_{false};
  std::int64_t integer_{0};
  double decimal_{0.0};
  std::string string_;
  std::vector<JsonValue> array_;
  std::vector<JsonMember> object_;  // document order; keys are unique
};

struct JsonMember final {
  std::string key;
  JsonValue value;
};

// Every bound is enforced while parsing; nothing is allocated past it.
// Depth counts containers: a top-level object or array is depth 1.
struct JsonLimits final {
  std::size_t max_bytes;
  std::size_t max_depth;
  std::size_t max_container_entries;
  std::size_t max_string_bytes;
  bool allow_fractions{false};   // SP3: chat sampling numbers; config files keep integers only
};

enum class JsonErrorCode {
  too_large, byte_order_mark, syntax, depth_exceeded, too_many_entries, string_too_long,
  invalid_string, duplicate_key, non_integer_number, integer_out_of_range, trailing_data,
};

// `offset` is a byte offset into the input; errors never carry input text.
struct JsonError final {
  JsonErrorCode code;
  std::size_t offset;
};

using JsonParseResult = std::variant<JsonValue, JsonError>;

// Strict RFC 8259 subset: UTF-8 without BOM, no comments, no trailing commas,
// duplicate object keys rejected (compared after unescaping), integers only
// (int64) unless limits.allow_fractions (then a frac/exp lexeme is a finite double), strings must be valid UTF-8 with no raw control bytes and no lone
// surrogates, exactly one value plus whitespace.
[[nodiscard]] JsonParseResult parse_json(std::string_view text, const JsonLimits& limits);
[[nodiscard]] std::string_view describe_json_error(JsonErrorCode code) noexcept;

// Appends `value` as a quoted JSON string ("\"", "\\", \b \f \n \r \t, other
// bytes < 0x20 as \u00XX; everything else, including UTF-8, verbatim).
void append_json_string(std::string* output, std::string_view value);

// The JSON text of a value as Python's json.dumps(value, ensure_ascii=False) writes it (", " and ": ", keys in order, a
// float as its shortest repr with ".0" on a whole one): what upstream's prompts and MCP client print (a tool in the Qwen
// template's <tools>, a non-string argument, structuredContent).
[[nodiscard]] std::string python_json_text(const JsonValue& value);

// RFC 3629 UTF-8 (no overlongs, no surrogates, nothing above U+10FFFF); the
// one validator the escaping writer's callers use before copying text (spec §3.9).
[[nodiscard]] bool is_valid_utf8(std::string_view value) noexcept;

}  // namespace secure_frontend
