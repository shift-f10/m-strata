// strata-unified-engine: CPU row dot products for the qwen4exp routed-expert quants (the CPU expert lane).
//
// The verify window's missed experts are read from the 46.8 GB host arena; on the owner's box the GPU's zero-copy
// reads of a pinned footprint that large run at 3-7 GB/s (STRATA_KERNEL_SELFTEST's pcie lines), while the CPU
// reads the same bytes from RAM at its memory bandwidth -- upstream computes most misses on its CPU pool for this
// reason. These are ggml's row dot products (MIT, The ggml authors; llama.cpp @ 4615f346): the scalar forms
// (ggml-cpu/quants.c *_generic) as the reference, the AVX2 forms (ggml-cpu/arch/x86/quants.c) for speed, plus the
// Strata-custom Q2_0 written here. Gate/up types take a Q8_K activation (256-value blocks), IQ4_NL / Q2_0 a Q8_0
// one (32-value blocks), as in llama.cpp's CPU backend.
#pragma once

#include <cstdint>

namespace u::cpu {

struct BlockQ8K final {   // ggml block_q8_K
  float d;
  std::int8_t qs[256];
  std::int16_t bsums[16];
};
struct BlockQ80 final {   // ggml block_q8_0 (fp16 scale)
  std::uint16_t d;
  std::int8_t qs[32];
};

// GGML type ids: 18 IQ3_XXS, 20 IQ4_NL, 21 IQ3_S, 22 IQ2_S, 23 IQ4_XS, 42 Q2_0 (Strata).
[[nodiscard]] bool iq_type_supported(int type) noexcept;
[[nodiscard]] bool iq_type_q8k(int type) noexcept;  // a Q8_K activation (else Q8_0)
[[nodiscard]] bool have_avx2() noexcept;             // the CPU runs the AVX2 forms

// ggml quantize_row_q8_K_ref / quantize_row_q8_0_ref (n % 256 == 0 / n % 32 == 0).
void quantize_q8k(const float* x, BlockQ8K* y, int n);
void quantize_q80(const float* x, BlockQ80* y, int n);

// One weight row (n values of `type`) . the quantized activation (BlockQ8K* or BlockQ80* by iq_type_q8k).
[[nodiscard]] float dot_ref(int type, const void* row, const void* xq, int n);  // the scalar reference
[[nodiscard]] float dot(int type, const void* row, const void* xq, int n);      // AVX2 when available

// Bytes of one row of n values.
[[nodiscard]] std::int64_t row_bytes(int type, int n) noexcept;

// fp16 <-> fp32 (IEEE binary16), for the blocks' scales.
[[nodiscard]] float fp16_to_f32(std::uint16_t h) noexcept;
[[nodiscard]] std::uint16_t f32_to_fp16(float f) noexcept;

namespace detail {  // the AVX2 forms (u_cpu_iq_avx2.cpp, compiled with AVX2 + FMA)
float dot_avx2(int type, const void* row, const void* xq, int n);
}

}  // namespace u::cpu
