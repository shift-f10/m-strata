// strata-gemma4-engine: a bounded GGUF v3 reader (header, metadata and tensor directory).
// Every count, length and offset is checked against fixed caps and the file's size; a file
// that breaks any rule is refused with a fixed message. No tensor data is read here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace g4 {

// The GGUF value types (gguf.h: GGUF_TYPE_*).
enum class GgufType : std::uint32_t {
  u8 = 0, i8 = 1, u16 = 2, i16 = 3, u32 = 4, i32 = 5, f32 = 6, boolean = 7,
  string = 8, array = 9, u64 = 10, i64 = 11, f64 = 12
};

// One metadata value. Scalars keep their type; arrays keep their element type and either
// numbers (as int64/double) or strings.
struct GgufValue final {
  GgufType type{GgufType::u8};
  GgufType element{GgufType::u8};  // arrays only
  std::int64_t integer{0};         // u8..i64 and bool
  double real{0.0};                // f32 / f64
  std::string text;                // string
  std::vector<std::int64_t> integers;  // arrays of integers / bools
  std::vector<double> reals;            // arrays of f32 / f64
  std::vector<std::string> texts;       // arrays of strings
  std::uint64_t count{0};               // arrays: element count
};

// The ggml tensor types this engine knows by size. Others are refused at parse time
// with the tensor named by its position (never its bytes).
enum class TensorType : std::uint32_t { f32 = 0, f16 = 1, q8_0 = 8, bf16 = 30 };

struct GgufTensor final {
  std::string name;
  std::vector<std::uint64_t> dims;  // ggml order: dims[0] is the row length
  TensorType type{TensorType::f32};
  std::uint64_t offset{0};          // from the data section's start
  std::uint64_t bytes{0};           // computed from type and dims
  [[nodiscard]] std::uint64_t elements() const;
};

struct GgufFile final {
  std::uint32_t version{0};
  std::map<std::string, GgufValue> metadata;
  std::vector<GgufTensor> tensors;
  std::uint64_t alignment{32};
  std::uint64_t data_offset{0};  // absolute file offset of the data section
  std::uint64_t file_size{0};
  [[nodiscard]] const GgufValue* find(std::string_view key) const;
  [[nodiscard]] const GgufTensor* tensor(std::string_view name) const;
};

struct GgufError final {
  std::string message;  // fixed text; never contains file bytes
};

// Caps: generous for the 26B-A4B file (58 keys, 658 tensors, 514,906 merges, a 12,045-byte
// template), small enough that a hostile header cannot make the reader allocate gigabytes.
struct GgufLimits final {
  std::uint64_t max_metadata = 4096;
  std::uint64_t max_tensors = 65536;
  std::uint64_t max_key_bytes = 256;
  std::uint64_t max_string_bytes = 1u << 20;     // one string (the chat template is 12 KB)
  std::uint64_t max_array_elements = 1u << 21;  // 2,097,152 (the merges are 514,906)
  std::uint64_t max_header_bytes = 64u << 20;   // metadata + tensor directory, in all
  std::uint32_t max_dims = 4;
};

// Parses a GGUF image held in memory (the header part of the file is enough as long as
// `file_size` is the whole file's size: tensors are checked against it).
[[nodiscard]] std::variant<GgufFile, GgufError> parse_gguf(const std::uint8_t* data, std::size_t size,
                                                           std::uint64_t file_size, const GgufLimits& limits = {});

// Reads and parses the header of the file at `path` (UTF-8). It reads at most
// limits.max_header_bytes + 4096 bytes from the start.
[[nodiscard]] std::variant<GgufFile, GgufError> read_gguf_header(const std::string& path, const GgufLimits& limits = {});

// Bytes of one tensor of `type` with `elements` elements and a row length of `row`
// (Q8_0 needs row % 32 == 0); nullopt for an unknown type or a bad row length.
[[nodiscard]] std::optional<std::uint64_t> tensor_bytes(TensorType type, std::uint64_t row, std::uint64_t elements);

}  // namespace g4
