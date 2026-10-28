#pragma once
// strata-prepare: JSON exactly as Python's json.dumps writes it (ensure_ascii, ", " / ": " separators, `indent` spaces
// per level or none, insertion order, and float repr - the shortest round-trip digits, exponent form below 1e-4 and from
// 1e16), so a pack's JSON files match upstream's tools byte for byte.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace strata_prepare {

class Json {
 public:
  enum class Kind { null, boolean, integer, real, text, list, object };
  Json() = default;
  static Json null_value() { return Json{}; }
  static Json boolean(bool value);
  static Json integer(std::int64_t value);
  static Json real(double value);
  static Json text(std::string value);
  static Json list();
  static Json object();
  Json& push(Json value);                          // list
  Json& set(std::string key, Json value);          // object, in insertion order (a key set twice keeps its place)
  Json& merge(const Json& other);                  // object: other's members, in their order ({**a, **b})
  // an object member's value (empty / false / 0.0 when absent or of another kind)
  [[nodiscard]] std::string member_text(const std::string& key) const;
  [[nodiscard]] bool member_bool(const std::string& key) const;
  [[nodiscard]] double member_real(const std::string& key) const;
  // json.dumps(value, indent=indent, ensure_ascii=ensure_ascii); indent < 0 is json.dumps(value) on one line
  [[nodiscard]] std::string dumps(int indent, bool ensure_ascii = true) const;

 private:
  void write(std::string& out, int indent, int level, bool ensure_ascii) const;
  Kind kind_ = Kind::null;
  bool boolean_ = false;
  std::int64_t integer_ = 0;
  double real_ = 0.0;
  std::string text_;
  std::vector<Json> items_;
  std::vector<std::pair<std::string, Json>> members_;
};

// Python's repr() of a finite float ("0.0", "1e-05", "0.000244140625", "1e+16").
[[nodiscard]] std::string python_float_repr(double value);
// A JSON string literal as json.dumps writes it: ensure_ascii escapes all but printable ASCII (\uXXXX, surrogate pairs);
// without it only '"', '\\' and the C0 controls are escaped and the rest stays UTF-8. `utf8` is valid UTF-8 (a GGUF
// string as gguf_reader.py decodes it: GgufHeader's strings).
[[nodiscard]] std::string python_json_string(const std::string& utf8, bool ensure_ascii = true);
// Python's repr() of a str ('text', or "text" when it holds a ' and no ").
[[nodiscard]] std::string python_repr(const std::string& utf8);

}  // namespace strata_prepare
