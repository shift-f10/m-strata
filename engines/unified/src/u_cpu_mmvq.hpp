// strata-unified-engine: the CPU mirror of the GPU's MMVQ i-quant x Q8_1 dot (kernels/u_iq.cu Fmt<TY>::dot /
// vec_dot_iqX_q8_1). The CPU expert lane must compute an expert BIT-FOR-BIT as the GPU grouped kernel does, so
// a missed expert the CPU takes equals the one the GPU would have produced: that means the SAME activation
// quantization (Q8_1, u::k::quantize_q8_1) and the SAME integer dp4a / sign / scale arithmetic, NOT ggml's
// reference IQ x Q8_K path (u_cpu_iq.*), which quantizes the activation differently and so diverges.
//
// Scalar host translations of __dp4a / __vsub4 / __vcmpne4 / __byte_perm / get_int_b* against a host copy of
// the same codebook grids (u_cpu_iq_tables.inc). The only residual difference from the GPU is the order the
// per-block float terms are summed (the GPU warp-reduces; here a row sums left to right), ~1e-6 relative.
#pragma once

#include <cstdint>

namespace u::cpu {

// Q8_1 activation block, byte-identical to the GPU block_q8_1 (half2 ds = {d, sum}, then int8 qs[32]). 36 bytes.
struct BlockQ81 final {
  std::uint16_t d = 0;    // fp16 scale (half2.x)
  std::uint16_t s = 0;    // fp16 block sum (half2.y)
  std::int8_t qs[32] = {};
};
static_assert(sizeof(BlockQ81) == 36, "BlockQ81 must match the GPU block_q8_1");

// Quantize n floats (n % 32 == 0) into n/32 Q8_1 blocks, exactly as u::k::quantize_q8_1_kernel: per 32-value
// block d = max|x| / 127 (clamped to fp16 max), qs = round(x / d) clamped to +-127, and the block sum, both
// stored as fp16.
void quantize_q81(const float* x, BlockQ81* y, int n);

// True iff the CPU MMVQ mirror decodes this GGML type id (the qwen4exp expert set: IQ2_S 22, IQ3_XXS 18,
// IQ3_S 21, IQ4_XS 23 against Q8_1; IQ4_NL 20, Q2_0 42 against Q8_1).
[[nodiscard]] bool mmvq_type_supported(int type);

// y[n_out] = W(type) . x, W row-major [n_out, n_in] in the type's packed quant, x the n_in-value activation as
// n_in/32 Q8_1 blocks. == the GPU iq_mmvq for this row (iq_row_dot<type> summed over the row's blocks).
void mmvq_rows(int type, const std::uint8_t* w, std::int64_t row_bytes, const BlockQ81* x, float* y, int n_in,
               int n_out);

// One row's dot (the building block of mmvq_rows): == iq_row_dot<type>(w, x, n_in/qk).
[[nodiscard]] float mmvq_row_dot(int type, const std::uint8_t* w, const BlockQ81* x, int n_in);

}  // namespace u::cpu
