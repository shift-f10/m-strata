// strata-unified-engine: the AVX2 forms of the CPU i-quant row dots (see u_cpu_iq.hpp). Compiled with AVX2 + FMA
// (CMake sets the flags for this file only); u::cpu::dot calls in here only when have_avx2().
// IQ2_S, IQ3_XXS, IQ3_S, IQ4_XS (x Q8_K) and IQ4_NL (x Q8_0) are ggml's AVX2 dot products (MIT, The ggml authors;
// llama.cpp @ 4615f346 ggml/src/ggml-cpu/arch/x86/quants.c), transcribed; the Q2_0 form is ours.
#include "u_cpu_iq.hpp"
#include "u_cpu_iq_detail.hpp"

#include <immintrin.h>

#include <cstring>

namespace u::cpu::detail {

namespace {

#include "u_cpu_iq_tables.inc"

#define U_MM256_SET_M128I(a, b) _mm256_insertf128_si256(_mm256_castsi128_si256(b), (a), 1)

inline float hsum_float_8(const __m256 x) {
  __m128 res = _mm256_extractf128_ps(x, 1);
  res = _mm_add_ps(res, _mm256_castps256_ps128(x));
  res = _mm_add_ps(res, _mm_movehl_ps(res, res));
  res = _mm_add_ss(res, _mm_movehdup_ps(res));
  return _mm_cvtss_f32(res);
}

inline __m256i mul_add_epi8(const __m256i x, const __m256i y) {
  const __m256i ax = _mm256_sign_epi8(x, x);
  const __m256i sy = _mm256_sign_epi8(y, x);
  return _mm256_maddubs_epi16(ax, sy);
}

// ggml's k_shuffle: row r (of 8) is the byte pair (2r, 2r+1) repeated 16 times
struct ScaleShuffle {
  alignas(32) std::uint8_t v[256];
  constexpr ScaleShuffle() : v{} {
    for (int r = 0; r < 8; ++r)
      for (int j = 0; j < 32; ++j) v[r * 32 + j] = (std::uint8_t)(2 * r + (j & 1));
  }
};
constexpr ScaleShuffle k_scale_shuffle{};
inline __m256i get_scale_shuffle_k4(int i) {
  return _mm256_load_si256(reinterpret_cast<const __m256i*>(k_scale_shuffle.v) + i);
}

alignas(32) const std::uint8_t k_mask1[32] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01,
                                              0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
                                              0x02, 0x02, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03};
alignas(32) const std::uint8_t k_mask2[32] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x01, 0x02, 0x04,
                                              0x08, 0x10, 0x20, 0x40, 0x80, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20,
                                              0x40, 0x80, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};

float dot_iq2_s(const BlockIq2s* x, const BlockQ8K* y, int nb) {
  const __m128i m4 = _mm_set1_epi8(0xf);
  const __m128i m1 = _mm_set1_epi8(1);
  const __m256i mask1 = _mm256_load_si256(reinterpret_cast<const __m256i*>(k_mask1));
  const __m256i mask2 = _mm256_load_si256(reinterpret_cast<const __m256i*>(k_mask2));
  std::uint64_t aux64 = 0;
  __m256 accumf = _mm256_setzero_ps();
  for (int i = 0; i < nb; ++i) {
    const float d = fp16_to_f32(x[i].d) * y[i].d;
    const std::uint8_t* qs = x[i].qs;
    const std::uint8_t* qh = x[i].qh;
    const std::uint16_t* signs = reinterpret_cast<const std::uint16_t*>(x[i].qs + kQK / 8);
    const std::int8_t* q8 = y[i].qs;
    std::memcpy(&aux64, x[i].scales, 8);
    const __m128i scales8 =
        _mm_add_epi8(_mm_slli_epi16(_mm_and_si128(_mm_set_epi64x((long long)(aux64 >> 4), (long long)aux64), m4), 1), m1);
    const __m256i scales16 = _mm256_cvtepi8_epi16(scales8);  // 0 2 4 6 8 10 12 14 1 3 5 7 9 11 13 15
    __m256i sumi1 = _mm256_setzero_si256();
    __m256i sumi2 = _mm256_setzero_si256();
    for (int ib32 = 0; ib32 < kQK / 32; ib32 += 2) {
      const __m256i q8_1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(q8));
      q8 += 32;
      const __m256i q8_2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(q8));
      q8 += 32;
      const __m256i q2_1 = _mm256_set_epi64x((long long)k_iq2s_grid[qs[3] | ((qh[ib32 + 0] << 2) & 0x300)],
                                             (long long)k_iq2s_grid[qs[2] | ((qh[ib32 + 0] << 4) & 0x300)],
                                             (long long)k_iq2s_grid[qs[1] | ((qh[ib32 + 0] << 6) & 0x300)],
                                             (long long)k_iq2s_grid[qs[0] | ((qh[ib32 + 0] << 8) & 0x300)]);
      const __m256i q2_2 = _mm256_set_epi64x((long long)k_iq2s_grid[qs[7] | ((qh[ib32 + 1] << 2) & 0x300)],
                                             (long long)k_iq2s_grid[qs[6] | ((qh[ib32 + 1] << 4) & 0x300)],
                                             (long long)k_iq2s_grid[qs[5] | ((qh[ib32 + 1] << 6) & 0x300)],
                                             (long long)k_iq2s_grid[qs[4] | ((qh[ib32 + 1] << 8) & 0x300)]);
      qs += 8;
      __m256i aux256 = _mm256_set1_epi32((int)(signs[0] | ((std::uint32_t)signs[1] << 16)));
      aux256 = _mm256_and_si256(_mm256_shuffle_epi8(aux256, mask1), mask2);
      const __m256i s2_1 = _mm256_cmpeq_epi8(aux256, mask2);
      const __m256i q8s_1 = _mm256_sub_epi8(_mm256_xor_si256(s2_1, q8_1), s2_1);
      aux256 = _mm256_set1_epi32((int)(signs[2] | ((std::uint32_t)signs[3] << 16)));
      aux256 = _mm256_and_si256(_mm256_shuffle_epi8(aux256, mask1), mask2);
      const __m256i s2_2 = _mm256_cmpeq_epi8(aux256, mask2);
      const __m256i q8s_2 = _mm256_sub_epi8(_mm256_xor_si256(s2_2, q8_2), s2_2);
      signs += 4;
      const __m256i dot1 = _mm256_maddubs_epi16(q2_1, q8s_1);
      const __m256i dot2 = _mm256_maddubs_epi16(q2_2, q8s_2);
      const __m256i p1 = _mm256_madd_epi16(dot1, _mm256_shuffle_epi8(scales16, get_scale_shuffle_k4(ib32 + 0)));
      const __m256i p2 = _mm256_madd_epi16(dot2, _mm256_shuffle_epi8(scales16, get_scale_shuffle_k4(ib32 + 1)));
      sumi1 = _mm256_add_epi32(sumi1, p1);
      sumi2 = _mm256_add_epi32(sumi2, p2);
    }
    accumf = _mm256_fmadd_ps(_mm256_set1_ps(d), _mm256_cvtepi32_ps(_mm256_add_epi32(sumi1, sumi2)), accumf);
  }
  return 0.125f * hsum_float_8(accumf);
}

float dot_iq3_xxs(const BlockIq3xxs* x, const BlockQ8K* y, int nb) {
  const std::uint64_t* signs64 = reinterpret_cast<const std::uint64_t*>(k_keven_signs_q2xs);
  std::uint32_t aux32[2];
  __m256 accumf = _mm256_setzero_ps();
  for (int i = 0; i < nb; ++i) {
    const float d = fp16_to_f32(x[i].d) * y[i].d;
    const std::uint8_t* q3 = x[i].qs;
    const std::uint8_t* gas = x[i].qs + kQK / 4;
    const std::int8_t* q8 = y[i].qs;
    __m256i sumi1 = _mm256_setzero_si256();
    __m256i sumi2 = _mm256_setzero_si256();
    for (int ib32 = 0; ib32 < kQK / 32; ib32 += 2) {
      const __m256i q8_1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(q8));
      q8 += 32;
      const __m256i q8_2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(q8));
      q8 += 32;
      const __m256i q2_1 = _mm256_set_epi32((int)k_iq3xxs_grid[q3[7]], (int)k_iq3xxs_grid[q3[6]], (int)k_iq3xxs_grid[q3[5]],
                                            (int)k_iq3xxs_grid[q3[4]], (int)k_iq3xxs_grid[q3[3]], (int)k_iq3xxs_grid[q3[2]],
                                            (int)k_iq3xxs_grid[q3[1]], (int)k_iq3xxs_grid[q3[0]]);
      q3 += 8;
      const __m256i q2_2 = _mm256_set_epi32((int)k_iq3xxs_grid[q3[7]], (int)k_iq3xxs_grid[q3[6]], (int)k_iq3xxs_grid[q3[5]],
                                            (int)k_iq3xxs_grid[q3[4]], (int)k_iq3xxs_grid[q3[3]], (int)k_iq3xxs_grid[q3[2]],
                                            (int)k_iq3xxs_grid[q3[1]], (int)k_iq3xxs_grid[q3[0]]);
      q3 += 8;
      std::memcpy(aux32, gas, 8);
      gas += 8;
      const __m256i s2_1 =
          _mm256_set_epi64x((long long)signs64[(aux32[0] >> 21) & 127], (long long)signs64[(aux32[0] >> 14) & 127],
                            (long long)signs64[(aux32[0] >> 7) & 127], (long long)signs64[(aux32[0] >> 0) & 127]);
      const __m256i s2_2 =
          _mm256_set_epi64x((long long)signs64[(aux32[1] >> 21) & 127], (long long)signs64[(aux32[1] >> 14) & 127],
                            (long long)signs64[(aux32[1] >> 7) & 127], (long long)signs64[(aux32[1] >> 0) & 127]);
      const __m256i q8s_1 = _mm256_sign_epi8(q8_1, s2_1);
      const __m256i q8s_2 = _mm256_sign_epi8(q8_2, s2_2);
      const __m256i dot1 = _mm256_maddubs_epi16(q2_1, q8s_1);
      const __m256i dot2 = _mm256_maddubs_epi16(q2_2, q8s_2);
      const std::uint16_t ls1 = (std::uint16_t)(aux32[0] >> 28);
      const std::uint16_t ls2 = (std::uint16_t)(aux32[1] >> 28);
      const __m256i p1 = _mm256_madd_epi16(dot1, _mm256_set1_epi16((short)(2 * ls1 + 1)));
      const __m256i p2 = _mm256_madd_epi16(dot2, _mm256_set1_epi16((short)(2 * ls2 + 1)));
      sumi1 = _mm256_add_epi32(sumi1, p1);
      sumi2 = _mm256_add_epi32(sumi2, p2);
    }
    accumf = _mm256_fmadd_ps(_mm256_set1_ps(d), _mm256_cvtepi32_ps(_mm256_add_epi32(sumi1, sumi2)), accumf);
  }
  return 0.25f * hsum_float_8(accumf);
}

float dot_iq3_s(const BlockIq3s* x, const BlockQ8K* y, int nb) {
  const __m256i mask1 = _mm256_load_si256(reinterpret_cast<const __m256i*>(k_mask1));
  const __m256i mask2 = _mm256_load_si256(reinterpret_cast<const __m256i*>(k_mask2));
  const __m256i idx_shift = _mm256_set_epi32(1, 2, 3, 4, 5, 6, 7, 8);
  const __m256i idx_mask = _mm256_set1_epi32(256);
  union {
    __m256i vec[2];
    std::uint32_t index[16];
  } idx;
  __m256 accumf = _mm256_setzero_ps();
  for (int i = 0; i < nb; ++i) {
    const float d = fp16_to_f32(x[i].d) * y[i].d;
    const std::uint8_t* qs = x[i].qs;
    const std::uint8_t* qh = x[i].qh;
    const std::uint16_t* signs = reinterpret_cast<const std::uint16_t*>(x[i].signs);
    const std::int8_t* q8 = y[i].qs;
    __m256i sumi1 = _mm256_setzero_si256();
    __m256i sumi2 = _mm256_setzero_si256();
    for (int ib32 = 0; ib32 < kQK / 32; ib32 += 2) {
      const __m256i q8_1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(q8));
      q8 += 32;
      const __m256i q8_2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(q8));
      q8 += 32;
      const __m256i idx_l = _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(qs)));
      qs += 16;
      idx.vec[0] = _mm256_set1_epi32(qh[ib32 + 0]);
      idx.vec[1] = _mm256_set1_epi32(qh[ib32 + 1]);
      idx.vec[0] = _mm256_and_si256(_mm256_sllv_epi32(idx.vec[0], idx_shift), idx_mask);
      idx.vec[1] = _mm256_and_si256(_mm256_sllv_epi32(idx.vec[1], idx_shift), idx_mask);
      idx.vec[0] = _mm256_or_si256(idx.vec[0], _mm256_cvtepi16_epi32(_mm256_castsi256_si128(idx_l)));
      idx.vec[1] = _mm256_or_si256(idx.vec[1], _mm256_cvtepi16_epi32(_mm256_extractf128_si256(idx_l, 1)));
      const __m256i q2_1 =
          _mm256_set_epi32((int)k_iq3s_grid[idx.index[7]], (int)k_iq3s_grid[idx.index[6]], (int)k_iq3s_grid[idx.index[5]],
                           (int)k_iq3s_grid[idx.index[4]], (int)k_iq3s_grid[idx.index[3]], (int)k_iq3s_grid[idx.index[2]],
                           (int)k_iq3s_grid[idx.index[1]], (int)k_iq3s_grid[idx.index[0]]);
      const __m256i q2_2 =
          _mm256_set_epi32((int)k_iq3s_grid[idx.index[15]], (int)k_iq3s_grid[idx.index[14]], (int)k_iq3s_grid[idx.index[13]],
                           (int)k_iq3s_grid[idx.index[12]], (int)k_iq3s_grid[idx.index[11]], (int)k_iq3s_grid[idx.index[10]],
                           (int)k_iq3s_grid[idx.index[9]], (int)k_iq3s_grid[idx.index[8]]);
      __m256i aux256 = _mm256_set1_epi32((int)(signs[0] | ((std::uint32_t)signs[1] << 16)));
      aux256 = _mm256_and_si256(_mm256_shuffle_epi8(aux256, mask1), mask2);
      const __m256i s2_1 = _mm256_cmpeq_epi8(aux256, mask2);
      const __m256i q8s_1 = _mm256_sub_epi8(_mm256_xor_si256(s2_1, q8_1), s2_1);
      aux256 = _mm256_set1_epi32((int)(signs[2] | ((std::uint32_t)signs[3] << 16)));
      aux256 = _mm256_and_si256(_mm256_shuffle_epi8(aux256, mask1), mask2);
      const __m256i s2_2 = _mm256_cmpeq_epi8(aux256, mask2);
      const __m256i q8s_2 = _mm256_sub_epi8(_mm256_xor_si256(s2_2, q8_2), s2_2);
      signs += 4;
      const __m256i dot1 = _mm256_maddubs_epi16(q2_1, q8s_1);
      const __m256i dot2 = _mm256_maddubs_epi16(q2_2, q8s_2);
      const std::uint16_t ls1 = (std::uint16_t)(x[i].scales[ib32 / 2] & 0xf);
      const std::uint16_t ls2 = (std::uint16_t)(x[i].scales[ib32 / 2] >> 4);
      const __m256i p1 = _mm256_madd_epi16(dot1, _mm256_set1_epi16((short)(2 * ls1 + 1)));
      const __m256i p2 = _mm256_madd_epi16(dot2, _mm256_set1_epi16((short)(2 * ls2 + 1)));
      sumi1 = _mm256_add_epi32(sumi1, p1);
      sumi2 = _mm256_add_epi32(sumi2, p2);
    }
    accumf = _mm256_fmadd_ps(_mm256_set1_ps(d), _mm256_cvtepi32_ps(_mm256_add_epi32(sumi1, sumi2)), accumf);
  }
  return hsum_float_8(accumf);
}

float dot_iq4_xs(const BlockIq4xs* x, const BlockQ8K* y, int nb) {
  const __m128i values128 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(k_kvalues_iq4nl));
  const __m128i m4b = _mm_set1_epi8(0x0f);
  __m256 accum = _mm256_setzero_ps();
  for (int ibl = 0; ibl < nb; ++ibl) {
    const std::uint8_t* qs = x[ibl].qs;
    const std::int8_t* q8 = y[ibl].qs;
    std::uint16_t sh = x[ibl].scales_h;
    __m256i sumi1 = _mm256_setzero_si256();
    __m256i sumi2 = _mm256_setzero_si256();
    for (int ib = 0; ib < kQK / 32; ib += 2) {
      const __m128i q4bits_1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(qs));
      qs += 16;
      const __m128i q4bits_2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(qs));
      qs += 16;
      const __m256i q8b_1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(q8));
      q8 += 32;
      const __m256i q8b_2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(q8));
      q8 += 32;
      const __m256i q4b_1 = U_MM256_SET_M128I(_mm_shuffle_epi8(values128, _mm_and_si128(_mm_srli_epi16(q4bits_1, 4), m4b)),
                                              _mm_shuffle_epi8(values128, _mm_and_si128(q4bits_1, m4b)));
      const __m256i q4b_2 = U_MM256_SET_M128I(_mm_shuffle_epi8(values128, _mm_and_si128(_mm_srli_epi16(q4bits_2, 4), m4b)),
                                              _mm_shuffle_epi8(values128, _mm_and_si128(q4bits_2, m4b)));
      const __m256i p16_1 = mul_add_epi8(q4b_1, q8b_1);
      const __m256i p16_2 = mul_add_epi8(q4b_2, q8b_2);
      const std::int16_t ls1 = (std::int16_t)(((x[ibl].scales_l[ib / 2] & 0xf) | ((sh << 4) & 0x30)) - 32);
      const std::int16_t ls2 = (std::int16_t)(((x[ibl].scales_l[ib / 2] >> 4) | ((sh << 2) & 0x30)) - 32);
      sh = (std::uint16_t)(sh >> 4);
      const __m256i p_1 = _mm256_madd_epi16(p16_1, _mm256_set1_epi16(ls1));
      const __m256i p_2 = _mm256_madd_epi16(p16_2, _mm256_set1_epi16(ls2));
      sumi1 = _mm256_add_epi32(p_1, sumi1);
      sumi2 = _mm256_add_epi32(p_2, sumi2);
    }
    accum = _mm256_fmadd_ps(_mm256_set1_ps(fp16_to_f32(x[ibl].d) * y[ibl].d),
                            _mm256_cvtepi32_ps(_mm256_add_epi32(sumi1, sumi2)), accum);
  }
  return hsum_float_8(accum);
}

float dot_iq4_nl(const BlockIq4nl* x, const BlockQ80* y, int nb) {
  const __m128i values128 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(k_kvalues_iq4nl));
  const __m128i m4b = _mm_set1_epi8(0x0f);
  const __m256i mone = _mm256_set1_epi16(1);
  __m256 accum1 = _mm256_setzero_ps();
  __m256 accum2 = _mm256_setzero_ps();
  int ib = 0;
  for (; ib + 1 < nb; ib += 2) {
    const __m128i q4bits_1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x[ib + 0].qs));
    const __m128i q4bits_2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x[ib + 1].qs));
    const __m256i q8b_1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(y[ib + 0].qs));
    const __m256i q8b_2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(y[ib + 1].qs));
    const __m256i q4b_1 = U_MM256_SET_M128I(_mm_shuffle_epi8(values128, _mm_and_si128(_mm_srli_epi16(q4bits_1, 4), m4b)),
                                            _mm_shuffle_epi8(values128, _mm_and_si128(q4bits_1, m4b)));
    const __m256i q4b_2 = U_MM256_SET_M128I(_mm_shuffle_epi8(values128, _mm_and_si128(_mm_srli_epi16(q4bits_2, 4), m4b)),
                                            _mm_shuffle_epi8(values128, _mm_and_si128(q4bits_2, m4b)));
    const __m256i p16_1 = mul_add_epi8(q4b_1, q8b_1);
    const __m256i p16_2 = mul_add_epi8(q4b_2, q8b_2);
    const __m256i p_1 = _mm256_madd_epi16(p16_1, mone);
    const __m256i p_2 = _mm256_madd_epi16(p16_2, mone);
    accum1 = _mm256_fmadd_ps(_mm256_set1_ps(fp16_to_f32(y[ib + 0].d) * fp16_to_f32(x[ib + 0].d)), _mm256_cvtepi32_ps(p_1),
                             accum1);
    accum2 = _mm256_fmadd_ps(_mm256_set1_ps(fp16_to_f32(y[ib + 1].d) * fp16_to_f32(x[ib + 1].d)), _mm256_cvtepi32_ps(p_2),
                             accum2);
  }
  float sumf = hsum_float_8(_mm256_add_ps(accum1, accum2));
  for (; ib < nb; ++ib) {  // the odd last block, as ggml's scalar tail
    const float d = fp16_to_f32(y[ib].d) * fp16_to_f32(x[ib].d);
    int sumi1 = 0, sumi2 = 0;
    for (int j = 0; j < 16; ++j) {
      sumi1 += y[ib].qs[j + 0] * k_kvalues_iq4nl[x[ib].qs[j] & 0xf];
      sumi2 += y[ib].qs[j + 16] * k_kvalues_iq4nl[x[ib].qs[j] >> 4];
    }
    sumf += d * (float)(sumi1 + sumi2);
  }
  return sumf;
}

// Q2_0 . Q8_0: the 2-bit codes of a block widened to bytes in value order, sum((c - 1) * q8) = sum(c*q8) - sum(q8).
float dot_q2_0(const BlockQ20* x, const BlockQ80* y, int nb) {
  const __m128i m3 = _mm_set1_epi8(3);
  const __m256i ones8 = _mm256_set1_epi8(1);
  const __m256i ones16 = _mm256_set1_epi16(1);
  __m256 accum = _mm256_setzero_ps();
  for (int ib = 0; ib < nb; ++ib) {
    const __m128i q = _mm_loadu_si128(reinterpret_cast<const __m128i*>(x[ib].qs));
    const __m128i c0 = _mm_and_si128(q, m3);
    const __m128i c1 = _mm_and_si128(_mm_srli_epi16(q, 2), m3);
    const __m128i c2 = _mm_and_si128(_mm_srli_epi16(q, 4), m3);
    const __m128i c3 = _mm_and_si128(_mm_srli_epi16(q, 6), m3);
    const __m128i a_lo = _mm_unpacklo_epi8(c0, c1), b_lo = _mm_unpacklo_epi8(c2, c3);  // bytes 0..7
    const __m128i a_hi = _mm_unpackhi_epi8(c0, c1), b_hi = _mm_unpackhi_epi8(c2, c3);  // bytes 8..15
    // value n = 4*byte + k: values 0..15, 16..31 | 32..47, 48..63
    const __m256i v0 = U_MM256_SET_M128I(_mm_unpackhi_epi16(a_lo, b_lo), _mm_unpacklo_epi16(a_lo, b_lo));
    const __m256i v1 = U_MM256_SET_M128I(_mm_unpackhi_epi16(a_hi, b_hi), _mm_unpacklo_epi16(a_hi, b_hi));
    const float dx = fp16_to_f32(x[ib].d);
    for (int h = 0; h < 2; ++h) {
      const BlockQ80& yb = y[2 * ib + h];
      const __m256i q8 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(yb.qs));
      const __m256i cq = _mm256_madd_epi16(_mm256_maddubs_epi16(h == 0 ? v0 : v1, q8), ones16);
      const __m256i sq = _mm256_madd_epi16(_mm256_maddubs_epi16(ones8, q8), ones16);
      accum = _mm256_fmadd_ps(_mm256_set1_ps(dx * fp16_to_f32(yb.d)), _mm256_cvtepi32_ps(_mm256_sub_epi32(cq, sq)),
                              accum);
    }
  }
  return hsum_float_8(accum);
}

}  // namespace

float dot_avx2(int type, const void* row, const void* xq, int n) {
  switch (type) {
    case 22: return dot_iq2_s(static_cast<const BlockIq2s*>(row), static_cast<const BlockQ8K*>(xq), n / kQK);
    case 18: return dot_iq3_xxs(static_cast<const BlockIq3xxs*>(row), static_cast<const BlockQ8K*>(xq), n / kQK);
    case 21: return dot_iq3_s(static_cast<const BlockIq3s*>(row), static_cast<const BlockQ8K*>(xq), n / kQK);
    case 23: return dot_iq4_xs(static_cast<const BlockIq4xs*>(row), static_cast<const BlockQ8K*>(xq), n / kQK);
    case 20: return dot_iq4_nl(static_cast<const BlockIq4nl*>(row), static_cast<const BlockQ80*>(xq), n / 32);
    case 42: return dot_q2_0(static_cast<const BlockQ20*>(row), static_cast<const BlockQ80*>(xq), n / 64);
    default: return 0.0f;
  }
}

}  // namespace u::cpu::detail
