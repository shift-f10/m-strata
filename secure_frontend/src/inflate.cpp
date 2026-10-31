// RFC 1951 DEFLATE decoding, written from the RFC (plugin spec 2026-10-30 section 12). Canonical Huffman codes are built
// from their code lengths (section 3.2.2) and decoded a bit at a time; blocks are stored (3.2.4), fixed (3.2.6) or
// dynamic (3.2.7); a back-reference copies from the last 32 KiB of output, which is kept as the window while older
// output goes to the sink in chunks.
#include "secure_frontend/inflate.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace secure_frontend {
namespace {

constexpr int kMaxBits = 15;              // the longest code (3.2.2)
constexpr std::size_t kWindow = 32768;    // the farthest a back-reference reaches (3.2.5)
constexpr std::size_t kChunk = 65536;     // what the sink gets at most at a time
constexpr int kMaxLiteralCodes = 286;     // 257-285 lengths and end-of-block, plus literals (3.2.5)
constexpr int kMaxDistanceCodes = 30;

// 3.2.5: the length and distance codes' bases and extra bits
constexpr std::array<std::uint16_t, 29> kLengthBase{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLengthExtra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistanceBase{1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                                      193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> kDistanceExtra{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                                      6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// 3.2.7: the order the code length code's lengths come in
constexpr std::array<std::uint8_t, 19> kLengthOrder{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// The input, a bit at a time, least significant bit of each byte first (3.1.1).
class BitReader final {
 public:
  explicit BitReader(const std::span<const std::uint8_t> input) : input_(input) {}

  // `count` bits (at most 16) as a number, the first bit lowest; false: the input ends first.
  [[nodiscard]] bool bits(const int count, std::uint32_t& value) {
    while (held_ < count) {
      if (next_ >= input_.size()) return false;
      buffer_ |= std::uint32_t{input_[next_++]} << held_;
      held_ += 8;
    }
    value = buffer_ & ((std::uint32_t{1} << count) - 1U);
    buffer_ >>= count;
    held_ -= count;
    return true;
  }
  // To the next byte (a stored block's start): the bits left are the rest of the byte already read.
  void to_byte() {
    buffer_ = 0;
    held_ = 0;
  }
  // `count` whole bytes from here (after to_byte); false: the input ends first.
  [[nodiscard]] bool bytes(const std::size_t count, std::span<const std::uint8_t>& out) {
    if (input_.size() - next_ < count) return false;
    out = input_.subspan(next_, count);
    next_ += count;
    return true;
  }
  // The bytes taken so far (a byte partly read counts: the stream's last byte is its own).
  [[nodiscard]] std::size_t consumed() const { return next_; }

 private:
  std::span<const std::uint8_t> input_;
  std::size_t next_{0};
  std::uint32_t buffer_{0};
  int held_{0};
};

// A canonical Huffman code (3.2.2): how many codes each length has, and the symbols in code order.
struct Huffman final {
  std::array<std::uint16_t, kMaxBits + 1> count{};
  std::array<std::uint16_t, 288> symbol{};
};

// The code the lengths make: 0 when complete (or empty), above 0 when incomplete (codes left), below 0 when
// over-subscribed.
int build(Huffman& code, const std::span<const std::uint8_t> lengths) {
  code.count.fill(0);
  for (const auto length : lengths) ++code.count[length];
  if (code.count[0] == lengths.size()) return 0;
  int left = 1;
  for (int length = 1; length <= kMaxBits; ++length) {
    left <<= 1;
    left -= code.count[static_cast<std::size_t>(length)];
    if (left < 0) return left;
  }
  std::array<std::uint16_t, kMaxBits + 2> offset{};
  for (int length = 1; length <= kMaxBits; ++length) {
    offset[static_cast<std::size_t>(length) + 1U] =
        static_cast<std::uint16_t>(offset[static_cast<std::size_t>(length)] + code.count[static_cast<std::size_t>(length)]);
  }
  for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
    if (lengths[symbol] != 0U) code.symbol[offset[lengths[symbol]]++] = static_cast<std::uint16_t>(symbol);
  }
  return left;
}

enum class Decode { ok, truncated, bad };

// One symbol: the code's bits come first-bit-highest (3.1.1); the lengths are tried shortest first.
Decode decode(BitReader& in, const Huffman& code, int& symbol) {
  int value = 0;
  int first = 0;
  int index = 0;
  for (int length = 1; length <= kMaxBits; ++length) {
    std::uint32_t bit = 0;
    if (!in.bits(1, bit)) return Decode::truncated;
    value |= static_cast<int>(bit);
    const int count = code.count[static_cast<std::size_t>(length)];
    if (value - count < first) {
      symbol = code.symbol[static_cast<std::size_t>(index + (value - first))];
      return Decode::ok;
    }
    index += count;
    first += count;
    first <<= 1;
    value <<= 1;
  }
  return Decode::bad;  // no code of any length
}

// The output: the window back-references read, and the sink the older bytes go to.
class Output final {
 public:
  Output(const std::uint64_t limit, const InflateSink& sink) : limit_(limit), sink_(sink) { data_.reserve(kWindow + 2U * kChunk); }

  [[nodiscard]] std::uint64_t written() const { return written_; }

  [[nodiscard]] std::optional<InflateError> byte(const std::uint8_t value) {
    if (written_ >= limit_) return InflateError::too_large;
    data_.push_back(value);
    ++written_;
    return drain(false);
  }
  // `length` bytes from `distance` back; the copy may overlap what it writes (3.2.3).
  [[nodiscard]] std::optional<InflateError> copy(const std::size_t distance, const std::size_t length) {
    if (distance == 0U || distance > written_) return InflateError::distance_too_far;
    if (limit_ - written_ < length) return InflateError::too_large;
    for (std::size_t i = 0; i < length; ++i) data_.push_back(data_[data_.size() - distance]);
    written_ += length;
    return drain(false);
  }
  // Everything not yet given to the sink, or (not at the end) whole chunks of it, keeping the window.
  [[nodiscard]] std::optional<InflateError> drain(const bool end) {
    while (data_.size() - given_ >= kChunk || (end && given_ < data_.size())) {
      const std::size_t count = std::min(kChunk, data_.size() - given_);
      if (!sink_(std::span<const std::uint8_t>{data_.data() + given_, count})) return InflateError::sink_refused;
      given_ += count;
    }
    if (given_ > kWindow) {
      const std::size_t drop = given_ - kWindow;
      data_.erase(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(drop));
      given_ -= drop;
    }
    return std::nullopt;
  }

 private:
  std::uint64_t limit_;
  const InflateSink& sink_;
  std::vector<std::uint8_t> data_;
  std::size_t given_{0};  // data_[0, given_) went to the sink already
  std::uint64_t written_{0};
};

// A compressed block's symbols until its end-of-block (3.2.5).
std::optional<InflateError> codes(BitReader& in, Output& out, const Huffman& literals, const Huffman& distances) {
  for (;;) {
    int symbol = 0;
    if (const auto got = decode(in, literals, symbol); got != Decode::ok) {
      return got == Decode::truncated ? InflateError::truncated : InflateError::bad_symbol;
    }
    if (symbol < 256) {
      if (auto failed = out.byte(static_cast<std::uint8_t>(symbol))) return failed;
      continue;
    }
    if (symbol == 256) return std::nullopt;
    const auto index = static_cast<std::size_t>(symbol - 257);
    if (index >= kLengthBase.size()) return InflateError::bad_symbol;
    std::uint32_t extra = 0;
    if (!in.bits(kLengthExtra[index], extra)) return InflateError::truncated;
    const std::size_t length = kLengthBase[index] + extra;
    if (const auto got = decode(in, distances, symbol); got != Decode::ok) {
      return got == Decode::truncated ? InflateError::truncated : InflateError::bad_symbol;
    }
    const auto at = static_cast<std::size_t>(symbol);
    if (at >= kDistanceBase.size()) return InflateError::bad_symbol;
    if (!in.bits(kDistanceExtra[at], extra)) return InflateError::truncated;
    if (auto failed = out.copy(kDistanceBase[at] + extra, length)) return failed;
  }
}

std::optional<InflateError> stored(BitReader& in, Output& out) {
  in.to_byte();
  std::span<const std::uint8_t> header;
  if (!in.bytes(4U, header)) return InflateError::truncated;
  const unsigned length = header[0] | (unsigned{header[1]} << 8U);
  const unsigned check = header[2] | (unsigned{header[3]} << 8U);
  if (length != (~check & 0xFFFFU)) return InflateError::bad_stored_length;
  std::span<const std::uint8_t> body;
  if (!in.bytes(length, body)) return InflateError::truncated;
  for (const auto value : body) {
    if (auto failed = out.byte(value)) return failed;
  }
  return std::nullopt;
}

std::optional<InflateError> fixed(BitReader& in, Output& out) {
  static const auto tables = [] {
    std::array<std::uint8_t, 288> lengths{};
    std::fill(lengths.begin(), lengths.begin() + 144, std::uint8_t{8});
    std::fill(lengths.begin() + 144, lengths.begin() + 256, std::uint8_t{9});
    std::fill(lengths.begin() + 256, lengths.begin() + 280, std::uint8_t{7});
    std::fill(lengths.begin() + 280, lengths.end(), std::uint8_t{8});
    std::pair<Huffman, Huffman> made;
    (void)build(made.first, lengths);
    std::array<std::uint8_t, 32> distances{};
    distances.fill(5);  // (30 and 31 have codes, and are refused as symbols)
    (void)build(made.second, distances);
    return made;
  }();
  return codes(in, out, tables.first, tables.second);
}

std::optional<InflateError> dynamic(BitReader& in, Output& out) {
  std::uint32_t literal_count = 0;
  std::uint32_t distance_count = 0;
  std::uint32_t length_count = 0;
  if (!in.bits(5, literal_count) || !in.bits(5, distance_count) || !in.bits(4, length_count)) return InflateError::truncated;
  literal_count += 257U;
  distance_count += 1U;
  length_count += 4U;
  if (literal_count > static_cast<std::uint32_t>(kMaxLiteralCodes) || distance_count > static_cast<std::uint32_t>(kMaxDistanceCodes)) {
    return InflateError::bad_code_lengths;
  }
  // the code length code: its lengths, three bits each, in kLengthOrder; it must be complete
  std::array<std::uint8_t, 19> length_lengths{};
  for (std::uint32_t i = 0; i < length_count; ++i) {
    std::uint32_t value = 0;
    if (!in.bits(3, value)) return InflateError::truncated;
    length_lengths[kLengthOrder[i]] = static_cast<std::uint8_t>(value);
  }
  Huffman lengths_code;
  if (build(lengths_code, length_lengths) != 0) return InflateError::bad_code_lengths;
  // the literal/length and distance codes' lengths, run-length coded (16: the last length again, 17 and 18: zeros)
  std::array<std::uint8_t, kMaxLiteralCodes + kMaxDistanceCodes> lengths{};
  const std::size_t total = literal_count + distance_count;
  std::size_t index = 0;
  while (index < total) {
    int symbol = 0;
    if (const auto got = decode(in, lengths_code, symbol); got != Decode::ok) {
      return got == Decode::truncated ? InflateError::truncated : InflateError::bad_code_lengths;
    }
    if (symbol < 16) {
      lengths[index++] = static_cast<std::uint8_t>(symbol);
      continue;
    }
    std::uint8_t value = 0;
    std::uint32_t repeat = 0;
    if (symbol == 16) {
      if (index == 0U) return InflateError::bad_code_lengths;  // nothing to repeat
      value = lengths[index - 1U];
      if (!in.bits(2, repeat)) return InflateError::truncated;
      repeat += 3U;
    } else if (symbol == 17) {
      if (!in.bits(3, repeat)) return InflateError::truncated;
      repeat += 3U;
    } else {
      if (!in.bits(7, repeat)) return InflateError::truncated;
      repeat += 11U;
    }
    if (index + repeat > total) return InflateError::bad_code_lengths;
    std::fill_n(lengths.begin() + static_cast<std::ptrdiff_t>(index), repeat, value);
    index += repeat;
  }
  if (lengths[256] == 0U) return InflateError::bad_code_lengths;  // no end-of-block
  // each code complete; an incomplete one only when it has a single code (3.2.7's one-distance-code case, as zlib)
  Huffman literals;
  const std::span<const std::uint8_t> literal_lengths{lengths.data(), literal_count};
  if (const int left = build(literals, literal_lengths); left < 0 || (left > 0 && literal_count - literals.count[0] != 1U)) {
    return InflateError::bad_code_lengths;
  }
  Huffman distances;
  const std::span<const std::uint8_t> distance_lengths{lengths.data() + literal_count, distance_count};
  if (const int left = build(distances, distance_lengths); left < 0 || (left > 0 && distance_count - distances.count[0] != 1U)) {
    return InflateError::bad_code_lengths;
  }
  return codes(in, out, literals, distances);
}

}  // namespace

std::string_view inflate_error_text(const InflateError error) noexcept {
  switch (error) {
    case InflateError::truncated: return "the compressed data ends early";
    case InflateError::bad_block_type: return "the compressed data has a block of an unknown type";
    case InflateError::bad_stored_length: return "the compressed data has a stored block whose length does not check";
    case InflateError::bad_code_lengths: return "the compressed data has a Huffman code that is not one";
    case InflateError::bad_symbol: return "the compressed data has a code it may not use";
    case InflateError::distance_too_far: return "the compressed data refers back before its start";
    case InflateError::too_large: return "the data is larger than allowed";
    case InflateError::sink_refused: return "the data was not taken";
  }
  return "the compressed data is not valid";
}

std::variant<InflateResult, InflateError> inflate_raw(const std::span<const std::uint8_t> input, const std::uint64_t max_output,
                                                      const InflateSink& sink) {
  BitReader in(input);
  Output out(max_output, sink);
  for (;;) {
    std::uint32_t last = 0;
    std::uint32_t type = 0;
    if (!in.bits(1, last) || !in.bits(2, type)) return InflateError::truncated;
    std::optional<InflateError> failed;
    switch (type) {
      case 0: failed = stored(in, out); break;
      case 1: failed = fixed(in, out); break;
      case 2: failed = dynamic(in, out); break;
      default: return InflateError::bad_block_type;
    }
    if (failed) return *failed;
    if (last != 0U) break;
  }
  if (auto failed = out.drain(true)) return *failed;
  return InflateResult{.written = out.written(), .consumed = in.consumed()};
}

}  // namespace secure_frontend
