#pragma once

// Python's whitespace over valid UTF-8, for the code that follows upstream's Python text handling (str.strip, Jinja's
// trim, the regex class \s): the code points str.isspace() is true for.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace secure_frontend {

// The code point at `at` of valid UTF-8 and its length in bytes.
[[nodiscard]] inline std::pair<std::uint32_t, std::size_t> utf8_code_point(const std::string_view text, const std::size_t at) {
  const auto byte = [&text](const std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(text[i])); };
  const std::uint32_t lead = byte(at);
  const std::size_t width = lead < 0x80U ? 1U : lead < 0xE0U ? 2U : lead < 0xF0U ? 3U : 4U;
  if (at + width > text.size()) return {lead, 1U};
  std::uint32_t value = width == 1U ? lead : width == 2U ? lead & 0x1FU : width == 3U ? lead & 0x0FU : lead & 0x07U;
  for (std::size_t i = 1U; i < width; ++i) value = (value << 6U) | (byte(at + i) & 0x3FU);
  return {value, width};
}

[[nodiscard]] inline bool python_space(const std::uint32_t c) noexcept {
  return (c >= 0x09U && c <= 0x0DU) || (c >= 0x1CU && c <= 0x20U) || c == 0x85U || c == 0xA0U || c == 0x1680U ||
         (c >= 0x2000U && c <= 0x200AU) || c == 0x2028U || c == 0x2029U || c == 0x202FU || c == 0x205FU || c == 0x3000U;
}

// The first offset at or after `at` that starts no whitespace.
[[nodiscard]] inline std::size_t python_space_end(const std::string_view text, std::size_t at = 0U) {
  while (at < text.size()) {
    const auto [c, width] = utf8_code_point(text, at);
    if (!python_space(c)) break;
    at += width;
  }
  return at;
}

[[nodiscard]] inline std::string_view python_lstrip(const std::string_view text) { return text.substr(python_space_end(text)); }

[[nodiscard]] inline std::string_view python_rstrip(const std::string_view text) {
  std::size_t end = text.size();
  while (end > 0U) {
    std::size_t start = end - 1U;
    while (start > 0U && (static_cast<unsigned char>(text[start]) & 0xC0U) == 0x80U) --start;
    if (!python_space(utf8_code_point(text, start).first)) break;
    end = start;
  }
  return text.substr(0U, end);
}

[[nodiscard]] inline std::string_view python_strip(const std::string_view text) { return python_rstrip(python_lstrip(text)); }

// str.split(None, 1)[0] of a text that is not all whitespace: its first run of non-whitespace (empty when there is none).
[[nodiscard]] inline std::string_view python_first_word(const std::string_view text) {
  const std::size_t start = python_space_end(text);
  std::size_t end = start;
  while (end < text.size()) {
    const auto [c, width] = utf8_code_point(text, end);
    if (python_space(c)) break;
    end += width;
  }
  return text.substr(start, end - start);
}

// The number of code points (Python's len) of valid UTF-8.
[[nodiscard]] inline std::size_t python_length(const std::string_view text) {
  std::size_t count = 0U;
  for (const char c : text) count += (static_cast<unsigned char>(c) & 0xC0U) != 0x80U ? 1U : 0U;
  return count;
}

}  // namespace secure_frontend
