// strata-unified-engine: the NVFP4 host reference (flint lane, spec 2026-10-25-flint-engine-design
// step H2). Host-safe header (no CUDA): the math the C1 device kernel must match bit for bit, and
// the anchor of the host case nvfp4_block_reference_matches_ggml_master.
//
// Ported 1:1 from ggml MASTER (llama.cpp, MIT):
//   ggml/src/ggml-common.h:221-227  block_nvfp4 { uint8_t d[4] (UE4M3, scales FIRST, one per
//                                   16-element sub-block); uint8_t qs[32] (packed E2M1 nibbles) }
//   ggml/src/ggml-common.h:1124-1128 kvalues_fp4 — the DOUBLED E2M1 table (0,1,2,3,4,6,8,12 for
//                                   codes 0..7; code bit 3 = sign), shared by MXFP4/NVFP4
//   ggml/src/ggml-impl.h:512-527    ggml_ue4m3_to_fp32 — returns raw*0.5 to absorb that doubling;
//                                   0x00 and 0x7F read as 0 (0x7F is the NaN/unused code)
//   ggml/src/ggml-impl.h:529-567    ggml_fp32_to_ue4m3 — quantizer-side encoder (subnormal round,
//                                   saturation to 0x7E at >= 15 exponent)
//   ggml/src/ggml-quants.c:589-611  dequantize_row_nvfp4 — the element mapping pinned by our test
//
// A 64-element block is 4 + 32 = 36 bytes (exactly 4.5 bits/element); ggml_row_size agrees
// (u_gguf.cpp tensor_bytes(nvfp4) computes the same sizes we parse flint GGUFs with).
// The flint GGUF additionally carries a per-tensor/per-expert F32 `weight scale` as a SEPARATE
// `<name>.scale` sidecar tensor — NOT part of this block math (the consumer multiplies the
// dequantized value by it; see the spec §3).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace u::nvfp4 {

inline constexpr int kBlockElems = 64;  // QK_NVFP4
inline constexpr int kSubElems = 16;    // QK_NVFP4_SUB
inline constexpr int kSubs = 4;         // QK_NVFP4 / QK_NVFP4_SUB
inline constexpr int kBlockBytes = 36;  // 4 scale bytes + 32 nibble bytes (scales FIRST)

// E2M1 values doubled (ggml kvalues_fp4): code -> 2*value, sign in bit 3.
inline constexpr std::int8_t kE2m1x2[16] = {0, 1, 2, 3, 4, 6, 8, 12, 0, -1, -2, -3, -4, -6, -8, -12};

// UE4M3: unsigned, 4 exp bits (bias 7), 3 mantissa bits. Returns raw * 0.5 — the pairing with the
// doubled kE2m1x2 table that makes value = E2M1 * raw exact for every code. 0x00 and 0x7F are zero.
[[nodiscard]] inline float ue4m3_to_f32(std::uint8_t x) noexcept {
  if (x == 0 || x == 0x7F) return 0.0f;
  const int exp = (x >> 3) & 0xF;
  const int man = x & 0x7;
  const float raw = (exp == 0) ? static_cast<float>(man) * 0.001953125f  // man * 2^-9
                               : std::ldexp(1.0f + static_cast<float>(man) / 8.0f, exp - 7);
  return raw * 0.5f;
}

// The quantizer-side encoder mirroring ggml_fp32_to_ue4m3 (ggml-impl.h:529). Only needed to
// SYNTHESIZE valid blocks in tests today; a flint writer would use the same mapping.
[[nodiscard]] inline std::uint8_t f32_to_ue4m3(float x) noexcept {
  if (!(x > 0.0f)) return 0;
  if (x > 448.0f) x = 448.0f;
  std::uint32_t bits = 0;
  static_assert(sizeof(float) == 4);
  std::memcpy(&bits, &x, 4);
  const int fp32_exp = static_cast<int>((bits >> 23) & 0xFF) - 127;
  const int fp32_man = static_cast<int>((bits >> 20) & 0x7);
  int ue_exp = fp32_exp + 7;
  if (ue_exp <= 0) {                                     // subnormal: value = man * 2^-9
    int man = static_cast<int>(x * 512.0f + 0.5f);
    if (man > 7) man = 7;
    if (man < 1) return 0;
    return static_cast<std::uint8_t>(man);
  }
  if (ue_exp >= 15) return 0x7E;
  int ue_man = fp32_man + static_cast<int>((bits >> 19) & 1);  // round on the dropped bit
  if (ue_man > 7) {
    ue_man = 0;
    ue_exp++;
    if (ue_exp >= 15) return 0x7E;
  }
  return static_cast<std::uint8_t>((ue_exp << 3) | ue_man);
}

// Dequant one 36-byte block into 64 floats: value = kE2m1x2[nib] * ue4m3_to_f32(d[s]).
// Element mapping inside sub-block s: qs[s*8+j] low nibble = element s*16+j, high nibble = element
// s*16+8+j (ggml-quants.c:604-608). Exact binary arithmetic: kE2m1x2 entries are multiples of 1
// (doubled), the scale is a multiple of 2^-9, so no rounding ever occurs.
inline void dequant_block(const std::uint8_t* block, float* out64) noexcept {
  for (int s = 0; s < kSubs; ++s) {
    const float d = ue4m3_to_f32(block[s]);
    for (int j = 0; j < kSubElems / 2; ++j) {
      const std::uint8_t q = block[kSubs + s * (kSubElems / 2) + j];
      out64[s * kSubElems + j] = static_cast<float>(kE2m1x2[q & 0x0F]) * d;
      out64[s * kSubElems + kSubElems / 2 + j] = static_cast<float>(kE2m1x2[q >> 4]) * d;
    }
  }
}

}  // namespace u::nvfp4
