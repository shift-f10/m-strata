#include "secure_frontend/gguf_metadata.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace secure_frontend {
namespace {

constexpr std::uint32_t kMagic = 0x46554747U;   // "GGUF", little-endian
constexpr std::size_t kMaxKeyBytes = 65536;
constexpr std::size_t kChunk = std::size_t{1} << 20;

enum : std::uint32_t {
  kU8 = 0, kI8 = 1, kU16 = 2, kI16 = 3, kU32 = 4, kI32 = 5, kF32 = 6, kBool = 7, kString = 8, kArray = 9, kU64 = 10,
  kI64 = 11, kF64 = 12,
};

GgufError fail(const GgufErrorCode code, const char* message) { return GgufError{code, message}; }

// Bytes from a source, through a chunk buffer, never more than the header's bound.
class Reader final {
 public:
  using Fill = std::function<std::variant<std::size_t, GgufError>(char* into, std::size_t capacity)>;
  Reader(Fill fill, const std::uint64_t max_bytes) : fill_(std::move(fill)), max_bytes_(max_bytes) {}

  // Exactly `count` bytes into `out` (nullptr: skipped).
  std::optional<GgufError> take(void* out, const std::size_t count) {
    std::size_t done = 0;
    while (done < count) {
      if (at_ == size_) {
        if (const auto refill = more()) return refill;
      }
      const std::size_t step = std::min(count - done, size_ - at_);
      if (out != nullptr) std::memcpy(static_cast<char*>(out) + done, buffer_.data() + at_, step);
      at_ += step;
      done += step;
    }
    consumed_ += count;
    if (consumed_ > max_bytes_) return fail(GgufErrorCode::too_large, "the GGUF header is larger than the limit");
    return std::nullopt;
  }
  template <typename T>
  std::optional<GgufError> scalar(T& value) { return take(&value, sizeof(T)); }
  [[nodiscard]] std::uint64_t consumed() const noexcept { return consumed_; }

 private:
  std::optional<GgufError> more() {
    if (buffer_.empty()) buffer_.resize(kChunk);
    auto got = fill_(buffer_.data(), buffer_.size());
    if (const auto* failure = std::get_if<GgufError>(&got)) return *failure;
    size_ = std::get<std::size_t>(got);
    at_ = 0;
    if (size_ == 0) return fail(GgufErrorCode::truncated, "the GGUF header ends early");
    return std::nullopt;
  }
  Fill fill_;
  std::uint64_t max_bytes_;
  std::vector<char> buffer_;
  std::size_t size_{0};
  std::size_t at_{0};
  std::uint64_t consumed_{0};
};

std::variant<std::string, GgufError> read_string(Reader& in, const std::size_t limit) {
  std::uint64_t length = 0;
  if (auto failure = in.scalar(length)) return *failure;
  if (length > limit) return fail(GgufErrorCode::too_large, "a GGUF string is longer than the limit");
  std::string text(static_cast<std::size_t>(length), '\0');
  if (auto failure = in.take(text.data(), text.size())) return *failure;
  return text;
}

// One scalar of `type` as int64, double or bool; nullopt for a type that is not a scalar.
std::variant<GgufValue, GgufError> read_scalar(Reader& in, const std::uint32_t type) {
  const auto integer = [&in]<typename T>(T) -> std::variant<GgufValue, GgufError> {
    T value{};
    if (auto failure = in.scalar(value)) return *failure;
    if constexpr (std::is_same_v<T, std::uint64_t>) {
      if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return fail(GgufErrorCode::malformed, "a GGUF integer is out of range");
      }
    }
    return GgufValue{static_cast<std::int64_t>(value)};
  };
  switch (type) {
    case kU8: return integer(std::uint8_t{});
    case kI8: return integer(std::int8_t{});
    case kU16: return integer(std::uint16_t{});
    case kI16: return integer(std::int16_t{});
    case kU32: return integer(std::uint32_t{});
    case kI32: return integer(std::int32_t{});
    case kU64: return integer(std::uint64_t{});
    case kI64: return integer(std::int64_t{});
    case kF32: {
      float value = 0.0F;
      if (auto failure = in.scalar(value)) return *failure;
      return GgufValue{static_cast<double>(value)};
    }
    case kF64: {
      double value = 0.0;
      if (auto failure = in.scalar(value)) return *failure;
      return GgufValue{value};
    }
    case kBool: {
      std::uint8_t value = 0;
      if (auto failure = in.scalar(value)) return *failure;
      if (value > 1U) return fail(GgufErrorCode::malformed, "a GGUF boolean is neither 0 nor 1");
      return GgufValue{value == 1U};
    }
    default: return fail(GgufErrorCode::malformed, "a GGUF value has an unknown type");
  }
}

GgufResult parse(Reader& in, const GgufLimits& limits) {
  std::uint32_t magic = 0;
  if (auto failure = in.scalar(magic)) return failure->code == GgufErrorCode::truncated ? fail(GgufErrorCode::not_gguf, "not a GGUF file") : *failure;
  if (magic != kMagic) return fail(GgufErrorCode::not_gguf, "not a GGUF file");
  GgufMetadata md;
  if (auto failure = in.scalar(md.version)) return *failure;
  if (md.version != 2U && md.version != 3U) return fail(GgufErrorCode::unsupported_version, "only GGUF versions 2 and 3 are read");
  std::uint64_t keys = 0;
  if (auto failure = in.scalar(md.tensor_count)) return *failure;
  if (auto failure = in.scalar(keys)) return *failure;
  if (keys > limits.max_keys) return fail(GgufErrorCode::too_large, "the GGUF header has more keys than the limit");
  for (std::uint64_t k = 0; k < keys; ++k) {
    auto key = read_string(in, kMaxKeyBytes);
    if (auto* failure = std::get_if<GgufError>(&key)) return *failure;
    std::uint32_t type = 0;
    if (auto failure = in.scalar(type)) return *failure;
    GgufValue value;
    if (type == kString) {
      auto text = read_string(in, limits.max_string);
      if (auto* failure = std::get_if<GgufError>(&text)) return *failure;
      value = std::get<std::string>(std::move(text));
    } else if (type == kArray) {
      std::uint32_t item = 0;
      std::uint64_t count = 0;
      if (auto failure = in.scalar(item)) return *failure;
      if (auto failure = in.scalar(count)) return *failure;
      if (item == kArray) return fail(GgufErrorCode::malformed, "a GGUF array of arrays is not read");
      if (count > limits.max_array) return fail(GgufErrorCode::too_large, "a GGUF array is longer than the limit");
      GgufArray array;
      if (item == kString) {
        array.strings.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) {
          auto text = read_string(in, limits.max_string);
          if (auto* failure = std::get_if<GgufError>(&text)) return *failure;
          array.strings.push_back(std::get<std::string>(std::move(text)));
        }
      } else {
        for (std::uint64_t i = 0; i < count; ++i) {
          auto scalar = read_scalar(in, item);
          if (auto* failure = std::get_if<GgufError>(&scalar)) return *failure;
          const auto& got = std::get<GgufValue>(scalar);
          if (const auto* integer = std::get_if<std::int64_t>(&got)) array.integers.push_back(*integer);
          else if (const auto* real = std::get_if<double>(&got)) array.reals.push_back(*real);
          else array.integers.push_back(std::get<bool>(got) ? 1 : 0);
        }
      }
      value = std::move(array);
    } else {
      auto scalar = read_scalar(in, type);
      if (auto* failure = std::get_if<GgufError>(&scalar)) return *failure;
      value = std::get<GgufValue>(std::move(scalar));
    }
    if (!md.values.emplace(std::get<std::string>(std::move(key)), std::move(value)).second) {
      return fail(GgufErrorCode::malformed, "a GGUF key appears twice");
    }
  }
  md.header_bytes = in.consumed();
  return md;
}

}  // namespace

const std::string* GgufMetadata::string(const std::string_view key) const {
  const auto it = values.find(key);
  return it == values.end() ? nullptr : std::get_if<std::string>(&it->second);
}

std::optional<std::int64_t> GgufMetadata::integer(const std::string_view key) const {
  const auto it = values.find(key);
  if (it == values.end()) return std::nullopt;
  if (const auto* value = std::get_if<std::int64_t>(&it->second)) return *value;
  return std::nullopt;
}

std::optional<double> GgufMetadata::real(const std::string_view key) const {
  const auto it = values.find(key);
  if (it == values.end()) return std::nullopt;
  if (const auto* value = std::get_if<double>(&it->second)) return *value;
  if (const auto* value = std::get_if<std::int64_t>(&it->second)) return static_cast<double>(*value);
  return std::nullopt;
}

std::optional<bool> GgufMetadata::boolean(const std::string_view key) const {
  const auto it = values.find(key);
  if (it == values.end()) return std::nullopt;
  if (const auto* value = std::get_if<bool>(&it->second)) return *value;
  return std::nullopt;
}

const std::vector<std::string>* GgufMetadata::strings(const std::string_view key) const {
  const auto it = values.find(key);
  const auto* array = it == values.end() ? nullptr : std::get_if<GgufArray>(&it->second);
  return array == nullptr || (array->strings.empty() && !array->integers.empty()) ? nullptr : &array->strings;
}

const std::vector<std::int64_t>* GgufMetadata::integers(const std::string_view key) const {
  const auto it = values.find(key);
  const auto* array = it == values.end() ? nullptr : std::get_if<GgufArray>(&it->second);
  return array == nullptr || (array->integers.empty() && !array->strings.empty()) ? nullptr : &array->integers;
}

GgufResult parse_gguf_metadata(const std::string_view bytes, const GgufLimits& limits) {
  std::size_t at = 0;
  Reader in([&bytes, &at](char* into, const std::size_t capacity) -> std::variant<std::size_t, GgufError> {
    const std::size_t step = std::min(capacity, bytes.size() - at);
    std::memcpy(into, bytes.data() + at, step);
    at += step;
    return step;
  }, limits.max_header_bytes);
  return parse(in, limits);
}

GgufResult read_gguf_metadata(const OpenedFile& file, const GgufLimits& limits) {
  Reader in([&file](char* into, const std::size_t capacity) -> std::variant<std::size_t, GgufError> {
    auto got = file.read_some(into, capacity);
    if (std::holds_alternative<PathError>(got)) return fail(GgufErrorCode::read_failed, "the GGUF file could not be read");
    return std::get<std::size_t>(got);
  }, limits.max_header_bytes);
  return parse(in, limits);
}

}  // namespace secure_frontend
