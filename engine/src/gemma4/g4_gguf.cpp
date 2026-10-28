#include "g4_gguf.hpp"

#include "g4_file.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace g4 {
namespace {

class Cursor final {
 public:
  Cursor(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
  [[nodiscard]] bool take(void* out, std::size_t n) {
    if (n > size_ - at_) return false;
    std::memcpy(out, data_ + at_, n);
    at_ += n;
    return true;
  }
  [[nodiscard]] bool skip(std::uint64_t n) {
    if (n > size_ - at_) return false;
    at_ += static_cast<std::size_t>(n);
    return true;
  }
  template <typename T>
  [[nodiscard]] bool get(T& out) {
    return take(&out, sizeof(T));
  }
  [[nodiscard]] std::size_t position() const noexcept { return at_; }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t at_{0};
};

bool read_string(Cursor& c, std::uint64_t cap, std::string& out) {
  std::uint64_t n = 0;
  if (!c.get(n) || n > cap) return false;
  out.resize(static_cast<std::size_t>(n));
  return n == 0 || c.take(out.data(), static_cast<std::size_t>(n));
}

bool is_scalar_type(std::uint32_t t) { return t <= 7 || t == 10 || t == 11 || t == 12 || t == 8; }

bool read_scalar(Cursor& c, GgufType t, const GgufLimits& limits, std::int64_t& integer, double& real, std::string* text) {
  switch (t) {
    case GgufType::u8: { std::uint8_t v; if (!c.get(v)) return false; integer = v; return true; }
    case GgufType::i8: { std::int8_t v; if (!c.get(v)) return false; integer = v; return true; }
    case GgufType::u16: { std::uint16_t v; if (!c.get(v)) return false; integer = v; return true; }
    case GgufType::i16: { std::int16_t v; if (!c.get(v)) return false; integer = v; return true; }
    case GgufType::u32: { std::uint32_t v; if (!c.get(v)) return false; integer = v; return true; }
    case GgufType::i32: { std::int32_t v; if (!c.get(v)) return false; integer = v; return true; }
    case GgufType::u64: {
      std::uint64_t v;
      if (!c.get(v) || v > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
      integer = static_cast<std::int64_t>(v);
      return true;
    }
    case GgufType::i64: { std::int64_t v; if (!c.get(v)) return false; integer = v; return true; }
    case GgufType::f32: { float v; if (!c.get(v)) return false; real = v; return true; }
    case GgufType::f64: { double v; if (!c.get(v)) return false; real = v; return true; }
    case GgufType::boolean: {
      std::uint8_t v;
      if (!c.get(v) || v > 1) return false;
      integer = v;
      return true;
    }
    case GgufType::string: return text != nullptr && read_string(c, limits.max_string_bytes, *text);
    case GgufType::array: return false;
  }
  return false;
}

bool is_real(GgufType t) { return t == GgufType::f32 || t == GgufType::f64; }

GgufError fail(const char* what) { return GgufError{what}; }

}  // namespace

std::uint64_t GgufTensor::elements() const {
  std::uint64_t n = 1;
  for (const auto d : dims) n *= d;
  return n;
}

const GgufValue* GgufFile::find(std::string_view key) const {
  const auto it = metadata.find(std::string{key});
  return it == metadata.end() ? nullptr : &it->second;
}

const GgufTensor* GgufFile::tensor(std::string_view name) const {
  for (const auto& t : tensors) {
    if (t.name == name) return &t;
  }
  return nullptr;
}

std::optional<std::uint64_t> tensor_bytes(TensorType type, std::uint64_t row, std::uint64_t elements) {
  switch (type) {
    case TensorType::f32: return elements * 4;
    case TensorType::f16:
    case TensorType::bf16: return elements * 2;
    case TensorType::q8_0:
      if (row == 0 || row % 32 != 0 || elements % 32 != 0) return std::nullopt;
      return elements / 32 * 34;
  }
  return std::nullopt;
}

std::variant<GgufFile, GgufError> parse_gguf(const std::uint8_t* data, std::size_t size, std::uint64_t file_size,
                                             const GgufLimits& limits) {
  if (data == nullptr || size > file_size) return fail("gguf: bad input");
  Cursor c(data, size);
  char magic[4];
  if (!c.take(magic, 4) || std::memcmp(magic, "GGUF", 4) != 0) return fail("gguf: not a GGUF file");
  GgufFile f;
  f.file_size = file_size;
  std::uint64_t n_tensors = 0, n_kv = 0;
  if (!c.get(f.version) || f.version != 3) return fail("gguf: only version 3 is supported");
  if (!c.get(n_tensors) || !c.get(n_kv)) return fail("gguf: truncated header");
  if (n_tensors > limits.max_tensors) return fail("gguf: too many tensors");
  if (n_kv > limits.max_metadata) return fail("gguf: too many metadata keys");
  std::uint64_t array_budget = limits.max_header_bytes;  // elements over all arrays, bounded too
  for (std::uint64_t i = 0; i < n_kv; ++i) {
    std::string key;
    if (!read_string(c, limits.max_key_bytes, key) || key.empty()) return fail("gguf: bad metadata key");
    std::uint32_t type = 0;
    if (!c.get(type)) return fail("gguf: truncated metadata");
    GgufValue v;
    if (type == static_cast<std::uint32_t>(GgufType::array)) {
      std::uint32_t element = 0;
      std::uint64_t count = 0;
      if (!c.get(element) || !c.get(count)) return fail("gguf: truncated array");
      if (!is_scalar_type(element)) return fail("gguf: bad array element type");
      if (count > limits.max_array_elements || count > array_budget) return fail("gguf: array too long");
      array_budget -= count;
      v.type = GgufType::array;
      v.element = static_cast<GgufType>(element);
      v.count = count;
      if (v.element == GgufType::string) {
        v.texts.reserve(static_cast<std::size_t>(count));
      } else if (is_real(v.element)) {
        v.reals.reserve(static_cast<std::size_t>(count));
      } else {
        v.integers.reserve(static_cast<std::size_t>(count));
      }
      for (std::uint64_t k = 0; k < count; ++k) {
        std::int64_t integer = 0;
        double real = 0.0;
        std::string text;
        if (!read_scalar(c, v.element, limits, integer, real, &text)) return fail("gguf: bad array element");
        if (v.element == GgufType::string) v.texts.push_back(std::move(text));
        else if (is_real(v.element)) v.reals.push_back(real);
        else v.integers.push_back(integer);
      }
    } else {
      if (!is_scalar_type(type)) return fail("gguf: bad metadata value type");
      v.type = static_cast<GgufType>(type);
      if (!read_scalar(c, v.type, limits, v.integer, v.real, &v.text)) return fail("gguf: bad metadata value");
    }
    if (!f.metadata.emplace(std::move(key), std::move(v)).second) return fail("gguf: duplicate metadata key");
    if (c.position() > limits.max_header_bytes) return fail("gguf: header too large");
  }
  if (const auto* a = f.find("general.alignment")) {
    if (a->type != GgufType::u32 || a->integer == 0 || (a->integer & (a->integer - 1)) != 0 || a->integer > 4096) {
      return fail("gguf: bad general.alignment");
    }
    f.alignment = static_cast<std::uint64_t>(a->integer);
  }
  f.tensors.reserve(static_cast<std::size_t>(n_tensors));
  for (std::uint64_t i = 0; i < n_tensors; ++i) {
    GgufTensor t;
    if (!read_string(c, limits.max_key_bytes, t.name) || t.name.empty()) return fail("gguf: bad tensor name");
    std::uint32_t n_dims = 0;
    if (!c.get(n_dims) || n_dims == 0 || n_dims > limits.max_dims) return fail("gguf: bad tensor dimensions");
    std::uint64_t elements = 1;
    for (std::uint32_t d = 0; d < n_dims; ++d) {
      std::uint64_t dim = 0;
      if (!c.get(dim) || dim == 0 || dim > (1ull << 40)) return fail("gguf: bad tensor dimension");
      if (elements > std::numeric_limits<std::uint64_t>::max() / dim || elements * dim > (1ull << 40)) {
        return fail("gguf: tensor too large");
      }
      elements *= dim;
      t.dims.push_back(dim);
    }
    std::uint32_t type = 0;
    if (!c.get(type) || !c.get(t.offset)) return fail("gguf: truncated tensor directory");
    if (type != 0 && type != 1 && type != 8 && type != 30) return fail("gguf: unsupported tensor type");
    t.type = static_cast<TensorType>(type);
    const auto bytes = tensor_bytes(t.type, t.dims[0], elements);
    if (!bytes) return fail("gguf: bad tensor shape for its type");
    t.bytes = *bytes;
    if (t.offset % f.alignment != 0) return fail("gguf: misaligned tensor");
    f.tensors.push_back(std::move(t));
    if (c.position() > limits.max_header_bytes) return fail("gguf: header too large");
  }
  const std::uint64_t header_end = c.position();
  f.data_offset = (header_end + f.alignment - 1) / f.alignment * f.alignment;
  if (f.data_offset > file_size) return fail("gguf: truncated file");
  const std::uint64_t data_bytes = file_size - f.data_offset;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
  spans.reserve(f.tensors.size());
  for (const auto& t : f.tensors) {
    if (t.offset > data_bytes || t.bytes > data_bytes - t.offset) return fail("gguf: tensor outside the file");
    spans.emplace_back(t.offset, t.bytes);
  }
  std::sort(spans.begin(), spans.end());
  for (std::size_t i = 1; i < spans.size(); ++i) {
    if (spans[i - 1].first + spans[i - 1].second > spans[i].first) return fail("gguf: overlapping tensors");
  }
  std::vector<std::string_view> names;
  names.reserve(f.tensors.size());
  for (const auto& t : f.tensors) names.emplace_back(t.name);
  std::sort(names.begin(), names.end());
  if (std::adjacent_find(names.begin(), names.end()) != names.end()) return fail("gguf: duplicate tensor name");
  return f;
}

std::variant<GgufFile, GgufError> read_gguf_header(const std::string& path, const GgufLimits& limits) {
  ReadOnlyFile file;
  if (!file.open(path)) return fail("gguf: cannot open the model file");
  const std::uint64_t want = std::min<std::uint64_t>(file.size(), limits.max_header_bytes + 4096);
  std::vector<std::uint8_t> head(static_cast<std::size_t>(want));
  if (want > 0 && !file.read_at(0, head.data(), head.size())) return fail("gguf: cannot read the model file");
  return parse_gguf(head.data(), head.size(), file.size(), limits);
}

}  // namespace g4
