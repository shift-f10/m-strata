#pragma once

// RFC 1951 DEFLATE decoding (plugin spec 2026-10-30 section 12: a plugin's .zip is inflated by code written here, with its
// own tests - no vendored decompressor). A raw DEFLATE stream - no zlib or gzip wrapper - is decoded into `sink` in
// chunks, keeping the last 32 KiB as the window back-references read, so a large file never sits in memory whole. Every
// bound is checked, and a malformed stream is refused by name; nothing is guessed.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <variant>

namespace secure_frontend {

enum class InflateError {
  truncated,          // the stream ends before its last block does
  bad_block_type,     // BTYPE 11, reserved
  bad_stored_length,  // a stored block's LEN and NLEN disagree
  bad_code_lengths,   // a dynamic block's code lengths do not make a code (over-subscribed, incomplete, a repeat first)
  bad_symbol,         // a code that is not one, or a literal/length or distance symbol the format leaves unused
  distance_too_far,   // a back-reference before the start of the output
  too_large,          // more output than the caller allows
  sink_refused,       // the sink stopped it
};
[[nodiscard]] std::string_view inflate_error_text(InflateError error) noexcept;

struct InflateResult final {
  std::uint64_t written{0};  // the bytes given to the sink
  std::size_t consumed{0};   // the input bytes the stream took (its last byte included)
};

// The decoded bytes, in order, in chunks of at most 64 KiB; false stops the stream (sink_refused).
using InflateSink = std::function<bool(std::span<const std::uint8_t>)>;

[[nodiscard]] std::variant<InflateResult, InflateError> inflate_raw(std::span<const std::uint8_t> input, std::uint64_t max_output,
                                                                    const InflateSink& sink);

}  // namespace secure_frontend
