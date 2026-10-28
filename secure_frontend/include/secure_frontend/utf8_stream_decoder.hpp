#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace secure_frontend {

inline constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";

// Incremental UTF-8 decoder for generated text (spec §3.3, decision 3).
// push() releases the longest complete prefix and holds an incomplete
// trailing sequence (at most 3 bytes) for the next token. A byte that can
// never start or continue a valid sequence becomes U+FFFD (maximal-subpart
// rule: one U+FFFD per broken sequence, then the offending byte is
// re-examined). finish() ends the text: the held tail is dropped for
// length/cancel ends and becomes one U+FFFD for a stop end.
class Utf8StreamDecoder final {
 public:
  [[nodiscard]] std::string push(std::string_view bytes);
  [[nodiscard]] std::string finish(bool drop_incomplete_tail);
  [[nodiscard]] std::size_t held_bytes() const noexcept { return held_size_; }

 private:
  std::array<unsigned char, 4> held_{};
  std::size_t held_size_{0};
  std::size_t needed_{0};
};

}  // namespace secure_frontend
