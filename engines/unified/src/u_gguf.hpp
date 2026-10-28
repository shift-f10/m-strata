// strata-unified-engine: a bounded GGUF v3 reader (header, metadata and tensor directory).
// Every count, length and offset is checked against fixed caps and the file's size; a file
// that breaks any rule is refused with a fixed message. No tensor data is read here.
//
// Mirrored from engines/gemma4/src/g4_gguf.{hpp,cpp} (the reviewed reference) into our tree.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace u {

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
// with the tensor named by its position (never its bytes). The k-quants (q4_k/q5_k/q6_k) and
// the i-quants (iq2_xxs/iq2_s/iq3_xxs) match the GGML type ids and u::k::Quant; they are needed by
// the non-gemma4 families (e.g. GLM-5.3: q6_k attention/dense + i-quant MoE experts).
enum class TensorType : std::uint32_t {
  f32 = 0, f16 = 1, q8_0 = 8, q2_k = 10, q3_k = 11, q4_k = 12, q5_k = 13, q6_k = 14,
  iq2_xxs = 16, iq3_xxs = 18, iq2_s = 22, bf16 = 30,  // GGML type ids (IQ2_S is 22, not 19 = IQ1_S)
  // qwen4exp routed-expert + dense i-quants: IQ4_NL (32-elem block), IQ3_S / IQ4_XS (256-elem super-
  // blocks), and Q2_0 (id 42, a Strata-custom 64-elem routed-expert encoding absent from stock ggml).
  // NVFP4 (id 40, flint's native quant — ggml MASTER GGML_TYPE_NVFP4, llama.cpp MODEL_ARCH.FLINT):
  // 64-elem blocks = 4 UE4M3 sub-scale bytes FIRST (one per 16-elem sub-block) + 32 packed E2M1
  // nibble bytes = 36 bytes (4.5 bpw); the F32 tensor scale is a SEPARATE `<name>.scale` sidecar
  // (an ordinary f32 tensor). Block math: kernels/nvfp4_ref.hpp.
  iq4_nl = 20, iq3_s = 21, iq4_xs = 23, nvfp4 = 40, q2_0 = 42
};

struct GgufTensor final {
  std::string name;
  std::vector<std::uint64_t> dims;  // ggml order: dims[0] is the row length
  TensorType type{TensorType::f32};
  std::uint64_t offset{0};          // from the data section's start (of this tensor's own shard)
  std::uint64_t bytes{0};           // computed from type and dims
  // Resolved ABSOLUTE host pointer to this tensor's data, set by the loader (u_main) once the file(s)
  // are mapped. For a single-file model it is base + data_offset + offset; for a split/multi-part model
  // it is the owning shard's mapped base + that shard's data_offset + offset. A graph's load() reads this
  // instead of computing base + offset, so multi-part spanning is transparent to the family graphs.
  const std::uint8_t* host_data{nullptr};
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

// Caps: generous for a large MoE file, small enough that a hostile header cannot make the
// reader allocate gigabytes.
struct GgufLimits final {
  std::uint64_t max_metadata = 4096;
  std::uint64_t max_tensors = 65536;
  std::uint64_t max_key_bytes = 256;
  std::uint64_t max_string_bytes = 1u << 20;     // one string (a chat template is ~12 KB)
  std::uint64_t max_array_elements = 1u << 21;  // 2,097,152 (merges can be ~500k)
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

}  // namespace u
