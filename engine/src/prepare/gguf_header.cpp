#include "gguf_header.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace strata_prepare {
namespace {

struct TypeRow {
  std::uint32_t id;
  const char* name;
  std::uint64_t block_elements;   // 0: no geometry in gguf_reader.py (expected_bytes is None)
  std::uint64_t block_bytes;
};

// tools/gguf_reader.py: GGML_TYPES and BLOCK_GEOMETRY, row for row.
constexpr std::array<TypeRow, 47> kTypes{{
    {0, "F32", 1, 4},          {1, "F16", 1, 2},          {2, "Q4_0", 32, 18},      {3, "Q4_1", 32, 20},
    {4, "Q4_2", 0, 0},         {5, "Q4_3", 0, 0},         {6, "Q5_0", 32, 22},      {7, "Q5_1", 32, 24},
    {8, "Q8_0", 32, 34},       {9, "Q8_1", 32, 36},       {10, "Q2_K", 256, 84},    {11, "Q3_K", 256, 110},
    {12, "Q4_K", 256, 144},    {13, "Q5_K", 256, 176},    {14, "Q6_K", 256, 210},   {15, "Q8_K", 256, 292},
    {16, "IQ2_XXS", 256, 66},  {17, "IQ2_XS", 256, 74},   {18, "IQ3_XXS", 256, 98}, {19, "IQ1_S", 256, 50},
    {20, "IQ4_NL", 32, 18},    {21, "IQ3_S", 256, 110},   {22, "IQ2_S", 256, 82},   {23, "IQ4_XS", 256, 136},
    {24, "I8", 0, 0},          {25, "I16", 0, 0},         {26, "I32", 0, 0},        {27, "I64", 0, 0},
    {28, "F64", 1, 8},         {29, "IQ1_M", 256, 56},    {30, "BF16", 1, 2},       {31, "Q4_0_4_4", 0, 0},
    {32, "Q4_0_4_8", 0, 0},    {33, "Q4_0_8_8", 0, 0},    {34, "TQ1_0", 0, 0},      {35, "TQ2_0", 0, 0},
    {36, "IQ4_NL_4_4", 0, 0},  {37, "IQ4_NL_4_8", 0, 0},  {38, "IQ4_NL_8_8", 0, 0}, {39, "MXFP4", 32, 17},
    {40, "NVFP4", 64, 36},     {41, "Q4_0_8_8", 0, 0},    {42, "Q2_0", 64, 18},     {143, "PTQ1_0", 128, 28},
    {144, "TQ2_T", 128, 34},   {145, "TQK6", 128, 26},    {146, "TQK7", 128, 30},
}};

const TypeRow* row_of(const std::uint32_t id) {
  for (const auto& row : kTypes) {
    if (row.id == id) return &row;
  }
  return nullptr;
}

class Reader {
 public:
  Reader(std::ifstream& in, std::string file) : in_(in), file_(std::move(file)) {}
  template <class T> T read() {
    T value{};
    in_.read(reinterpret_cast<char*>(&value), sizeof(T));
    if (!in_) throw std::runtime_error(file_ + ": unexpected end of file in the GGUF header");
    return value;
  }
  std::string text() {
    const auto n = read<std::uint64_t>();
    if (n > (1ULL << 32U)) throw std::runtime_error(file_ + ": a GGUF string is implausibly long");
    std::string s(static_cast<std::size_t>(n), '\0');
    if (n != 0U) in_.read(s.data(), static_cast<std::streamsize>(n));
    if (!in_) throw std::runtime_error(file_ + ": unexpected end of file in the GGUF header");
    return utf8_replace(s);   // fh.read(n).decode("utf-8", "replace")
  }
  MetaValue value(const std::uint32_t type, const bool in_array = false) {
    MetaValue v;
    switch (type) {
      case 0: v.integer = read<std::uint8_t>(); break;
      case 1: v.integer = read<std::int8_t>(); break;
      case 2: v.integer = read<std::uint16_t>(); break;
      case 3: v.integer = read<std::int16_t>(); break;
      case 4: v.integer = read<std::uint32_t>(); break;
      case 5: v.integer = read<std::int32_t>(); break;
      case 6: v.kind = MetaValue::Kind::real; v.real = read<float>(); break;
      case 7: v.integer = read<std::uint8_t>() != 0U ? 1 : 0; break;
      case 8: v.kind = MetaValue::Kind::text; v.text = text(); break;
      case 9: {
        if (in_array) throw std::runtime_error(file_ + ": a nested GGUF array");
        const auto element = read<std::uint32_t>();
        const auto count = read<std::uint64_t>();
        if (count > (1ULL << 28U)) throw std::runtime_error(file_ + ": a GGUF array is implausibly long");
        v.kind = MetaValue::Kind::list;
        v.items.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) v.items.push_back(value(element, true));
        break;
      }
      case 10: v.integer = static_cast<std::int64_t>(read<std::uint64_t>()); break;
      case 11: v.integer = read<std::int64_t>(); break;
      case 12: v.kind = MetaValue::Kind::real; v.real = read<double>(); break;
      default: throw std::runtime_error(file_ + ": unknown GGUF metadata type " + std::to_string(type));
    }
    return v;
  }

 private:
  std::ifstream& in_;
  std::string file_;
};

}  // namespace

std::string utf8_replace(const std::string& bytes) {
  const auto valid_ascii = [&bytes] {
    for (const char c : bytes) {
      if (static_cast<unsigned char>(c) >= 0x80U) return false;
    }
    return true;
  };
  if (valid_ascii()) return bytes;
  std::string out;
  out.reserve(bytes.size());
  const auto n = bytes.size();
  for (std::size_t i = 0; i < n;) {
    const auto c = static_cast<unsigned char>(bytes[i]);
    if (c < 0x80U) {
      out.push_back(static_cast<char>(c));
      ++i;
      continue;
    }
    // the sequence a lead byte starts: its length and the range of its first continuation byte (Unicode Table 3-7)
    std::size_t need = 0;
    unsigned lo = 0x80U;
    unsigned hi = 0xBFU;
    if (c >= 0xC2U && c <= 0xDFU) {
      need = 1;
    } else if (c >= 0xE0U && c <= 0xEFU) {
      need = 2;
      lo = c == 0xE0U ? 0xA0U : 0x80U;
      hi = c == 0xEDU ? 0x9FU : 0xBFU;
    } else if (c >= 0xF0U && c <= 0xF4U) {
      need = 3;
      lo = c == 0xF0U ? 0x90U : 0x80U;
      hi = c == 0xF4U ? 0x8FU : 0xBFU;
    }
    // [i, j): the lead byte and the continuation bytes that fit; a sequence cut short is one U+FFFD up to the byte
    // that does not fit (which starts the next one)
    std::size_t j = i + 1;
    bool whole = need != 0U;
    for (std::size_t k = 0; whole && k < need; ++k) {
      const unsigned b = j < n ? static_cast<unsigned char>(bytes[j]) : 0U;
      if (j >= n || b < (k == 0U ? lo : 0x80U) || b > (k == 0U ? hi : 0xBFU)) {
        whole = false;
      } else {
        ++j;
      }
    }
    if (whole) {
      out.append(bytes, i, j - i);
    } else {
      out += "\xEF\xBF\xBD";   // U+FFFD for the maximal subpart [i, j)
    }
    i = j;
  }
  return out;
}

std::string type_name_of(const std::uint32_t type_id) {
  const auto* row = row_of(type_id);
  return row != nullptr ? std::string{row->name} : "type" + std::to_string(type_id);
}

std::uint64_t TensorInfo::elements() const {
  std::uint64_t n = 1;
  for (const auto d : shape) n *= d;
  return n;
}

std::optional<std::uint64_t> TensorInfo::expected_bytes() const {
  const auto* row = row_of(type_id);
  if (row == nullptr || row->block_elements == 0U) return std::nullopt;
  const auto n = elements();
  if (n % row->block_elements != 0U) return std::nullopt;
  return n / row->block_elements * row->block_bytes;
}

GgufHeader::GgufHeader(const std::filesystem::path& file) : path(file) {
  std::ifstream in(file, std::ios::binary);
  const std::string name = file.filename().string();
  if (!in) throw std::runtime_error(name + ": cannot be opened");
  std::error_code ec;
  file_size = std::filesystem::file_size(file, ec);
  Reader r(in, name);
  const auto magic = r.read<std::uint32_t>();
  if (magic != 0x46554747U) {
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%#x", magic);
    throw std::runtime_error(name + ": not a GGUF file (magic " + hex + ")");
  }
  version = r.read<std::uint32_t>();
  const auto n_tensors = r.read<std::uint64_t>();
  const auto n_kv = r.read<std::uint64_t>();
  if (version != 3U) throw std::runtime_error(name + ": GGUF v" + std::to_string(version) + ", this reader handles v3");
  for (std::uint64_t i = 0; i < n_kv; ++i) {
    std::string key = r.text();
    const auto type = r.read<std::uint32_t>();
    if (metadata.count(key) == 0U) keys.push_back(key);
    metadata[std::move(key)] = r.value(type);
  }
  for (std::uint64_t i = 0; i < n_tensors; ++i) {
    TensorInfo t;
    t.name = r.text();
    const auto dims = r.read<std::uint32_t>();
    if (dims > 8U) throw std::runtime_error(name + ": a tensor with " + std::to_string(dims) + " dimensions");
    for (std::uint32_t d = 0; d < dims; ++d) t.shape.push_back(r.read<std::uint64_t>());
    t.type_id = r.read<std::uint32_t>();
    t.offset = r.read<std::uint64_t>();
    t.type_name = type_name_of(t.type_id);
    tensors.push_back(std::move(t));
  }
  // `align = metadata.get("general.alignment"); if isinstance(align, int) and align: alignment = align`
  if (const auto* align = get("general.alignment"); align != nullptr && align->kind == MetaValue::Kind::integer && align->integer != 0) {
    alignment = static_cast<std::uint64_t>(align->integer);
  }
  const auto pos = static_cast<std::uint64_t>(in.tellg());
  data_start = (pos + alignment - 1U) / alignment * alignment;
}

const MetaValue* GgufHeader::get(const std::string& key) const {
  const auto it = metadata.find(key);
  return it == metadata.end() ? nullptr : &it->second;
}

}  // namespace strata_prepare
