#include "secure_frontend/utf8_stream_decoder.hpp"

namespace secure_frontend {
namespace {

std::size_t sequence_length(const unsigned char lead) noexcept {
  if (lead >= 0xC2U && lead <= 0xDFU) return 2U;
  if (lead >= 0xE0U && lead <= 0xEFU) return 3U;
  if (lead >= 0xF0U && lead <= 0xF4U) return 4U;
  return 0U;
}

// RFC 3629 table 3-7: the second byte's range depends on the lead.
bool valid_second(const unsigned char lead, const unsigned char byte) noexcept {
  switch (lead) {
    case 0xE0U: return byte >= 0xA0U && byte <= 0xBFU;
    case 0xEDU: return byte >= 0x80U && byte <= 0x9FU;
    case 0xF0U: return byte >= 0x90U && byte <= 0xBFU;
    case 0xF4U: return byte >= 0x80U && byte <= 0x8FU;
    default: return byte >= 0x80U && byte <= 0xBFU;
  }
}

}  // namespace

std::string Utf8StreamDecoder::push(const std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size() + held_size_);
  for (const char character : bytes) {
    const auto byte = static_cast<unsigned char>(character);
    if (held_size_ > 0U) {
      const bool continues = held_size_ == 1U ? valid_second(held_[0], byte) : (byte >= 0x80U && byte <= 0xBFU);
      if (continues) {
        held_[held_size_++] = byte;
        if (held_size_ == needed_) {
          out.append(reinterpret_cast<const char*>(held_.data()), held_size_);
          held_size_ = 0U;
          needed_ = 0U;
        }
        continue;
      }
      out += kReplacementCharacter;  // the broken sequence; `byte` is examined afresh below
      held_size_ = 0U;
      needed_ = 0U;
    }
    if (byte < 0x80U) {
      out.push_back(static_cast<char>(byte));
      continue;
    }
    const std::size_t length = sequence_length(byte);
    if (length == 0U) {
      out += kReplacementCharacter;
      continue;
    }
    held_[0] = byte;
    held_size_ = 1U;
    needed_ = length;
  }
  return out;
}

std::string Utf8StreamDecoder::finish(const bool drop_incomplete_tail) {
  const bool had_tail = held_size_ > 0U;
  held_size_ = 0U;
  needed_ = 0U;
  return had_tail && !drop_incomplete_tail ? std::string(kReplacementCharacter) : std::string();
}

}  // namespace secure_frontend
