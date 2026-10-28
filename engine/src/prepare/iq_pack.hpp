#pragma once
// strata-prepare pack: upstream Strata's tools/iq_pack.py (v0.2.6) in C++ - the native pack the engine runs a Qwen3.8
// family model from (--pack beside --native), written byte for byte as the Python tool writes it: index.txt,
// native_experts.txt (v3/v4, v5 with hadamard.txt), dense.bin, conversions.json, compat-bf16.json (--compat-bf16) and
// experts.bin with experts.bin.src.json (--experts-bin, or NVFP4 experts). Not ported: --base, --skip-experts and the
// tokenizer export (the server reads the tokenizer from the GGUF). Spec docs/superpowers/specs/2026-10-27-strata-prepare-design.md.

#include <filesystem>
#include <ostream>

namespace strata_prepare {

struct PackOptions {
  std::filesystem::path gguf;   // the model's shard 1
  std::filesystem::path out;    // the pack folder (created when missing)
  bool compat_bf16 = false;     // dequantize the small projections the engine reads as BF16 (rounds to nearest-even)
  bool experts_bin = false;     // also write experts.bin
};

// iq_pack.py's main(): its messages on `log`, 0 when the pack is complete, else 1 (nothing published past the step that
// failed, as the Python tool leaves it).
[[nodiscard]] int iq_pack(const PackOptions& options, std::ostream& log);

// The bits of IEEE half (round to nearest, ties to even; numpy's astype(float16)); `overflow` set past F16's range.
[[nodiscard]] unsigned short f16_bits(float value, bool& overflow) noexcept;
// ggml's round-to-nearest-even float -> BF16 (finite values).
[[nodiscard]] unsigned short bf16_bits(float value) noexcept;

}  // namespace strata_prepare
