// engines/unified/src/u_cpu_expert.cpp - CPU expert-compute lane arithmetic (Stage C3).
//
// Host dequant of the GLM-5.3 expert i-quants (IQ2_XXS / IQ2_S / IQ3_XXS) and the qwen4exp expert
// i-quants (IQ4_NL / IQ3_S / IQ4_XS / Q2_0) + a plain f32 row matvec + the gated activation. The
// dequants follow the ggml/maya reference float dequants (iq_kernels.cu dq_iq2_xxs / _iq2_s /
// _iq3_xxs / _iq4_nl / _iq3_s / _iq4_xs / _q2_0); the codebook grids are the GPU tables verbatim
// (u_cpu_expert_grids.inc), so a dequantized block matches the GPU's values. The matvec accumulates
// in f32 (the GPU uses a fused Q8_1 int dot), so the result is numerically valid but not
// bit-identical -- see the header.
#include "u_cpu_expert.hpp"

#include <cmath>
#include <cstring>

#include "u_cpu_expert_grids.inc"  // iq2xxs/iq2s/iq3xxs grids + iq3s_grid_host[512], kvalues_iq4nl_host[16]

namespace u {
namespace {

constexpr int kIq2xxs = 16, kIq3xxs = 18, kIq2s = 22, kIq4nl = 20, kIq3s = 21, kIq4xs = 23, kQ20 = 42;

inline bool is_iq(int t) {
  return t == kIq2xxs || t == kIq3xxs || t == kIq2s || t == kIq4nl || t == kIq3s || t == kIq4xs || t == kQ20;
}

// Bytes per 256 elements for each supported quant. IQ4_NL uses 32-element blocks (8 x 18B), Q2_0
// uses 64-element blocks (4 x 18B); the rest are single 256-element blocks.
inline int block_bytes(int t) {
  switch (t) {
    case kIq2xxs: return 66;
    case kIq2s:   return 82;
    case kIq3xxs: return 98;
    case kIq3s:   return 110;
    case kIq4xs:  return 136;
    case kIq4nl:  return 144;
    case kQ20:    return 72;
    default:      return 0;
  }
}

inline std::uint16_t read16(const std::uint8_t* p) {
  std::uint16_t v;
  std::memcpy(&v, p, 2);
  return v;
}

inline int popc8(std::uint8_t v) {
  v = (std::uint8_t)(v - ((v >> 1) & 0x55));
  v = (std::uint8_t)((v & 0x33) + ((v >> 2) & 0x33));
  return (v + (v >> 4)) & 0x0f;
}

// IEEE half -> float.
inline float fp16_to_f32(std::uint16_t h) {
  const std::uint32_t sign = (std::uint32_t)(h & 0x8000u) << 16;
  std::uint32_t exp = (h >> 10) & 0x1f;
  std::uint32_t man = h & 0x3ffu;
  std::uint32_t f;
  if (exp == 0) {
    if (man == 0) {
      f = sign;
    } else {
      exp = 127 - 15 + 1;
      while ((man & 0x400u) == 0) { man <<= 1; --exp; }
      man &= 0x3ffu;
      f = sign | (exp << 23) | (man << 13);
    }
  } else if (exp == 0x1f) {
    f = sign | 0x7f800000u | (man << 13);
  } else {
    f = sign | ((exp - 15 + 127) << 23) | (man << 13);
  }
  float out;
  std::memcpy(&out, &f, 4);
  return out;
}

// Dequantize one 256-element block of each i-quant into out[256]. Mirrors u_iq.cu's vec_dot semantics
// (grid lookup + ksigns-derived signs + the per-group scale), as the ggml reference float dequant.
void dq_iq2xxs(const std::uint8_t* b, float* out) {
  std::uint16_t dh;
  std::memcpy(&dh, b, 2);
  const float d = fp16_to_f32(dh);
  const std::uint8_t* qs = b + 2;  // 64 bytes
  for (int ib = 0; ib < 8; ++ib) {
    const std::uint8_t* g8 = qs + 8 * ib;
    std::uint32_t a1;
    std::memcpy(&a1, g8 + 4, 4);
    const float db = d * 0.25f * (0.5f + (float)(a1 >> 28));
    for (int l = 0; l < 4; ++l) {
      const std::uint8_t* grid = (const std::uint8_t*)&iq2xxs_grid_host[g8[l]];
      const std::uint8_t v = (std::uint8_t)((a1 >> (7 * l)) & 127);
      const std::uint8_t s = (std::uint8_t)(v ^ ((popc8(v) & 1) << 7));
      for (int j = 0; j < 8; ++j) {
        const float val = db * (float)grid[j];
        *out++ = (s >> j) & 1 ? -val : val;
      }
    }
  }
}

void dq_iq3xxs(const std::uint8_t* b, float* out) {
  std::uint16_t dh;
  std::memcpy(&dh, b, 2);
  const float d = fp16_to_f32(dh);
  const std::uint8_t* qs = b + 2;        // 64 bytes of grid indices
  const std::uint8_t* ss = qs + 64;      // 32 bytes: 8 x uint32 signs+scale
  for (int ib = 0; ib < 8; ++ib) {
    std::uint32_t a1;
    std::memcpy(&a1, ss + 4 * ib, 4);
    const float db = d * 0.5f * (0.5f + (float)(a1 >> 28));
    for (int l = 0; l < 4; ++l) {
      const std::uint8_t* g1 = (const std::uint8_t*)&iq3xxs_grid_host[qs[8 * ib + 2 * l + 0]];
      const std::uint8_t* g2 = (const std::uint8_t*)&iq3xxs_grid_host[qs[8 * ib + 2 * l + 1]];
      const std::uint8_t v = (std::uint8_t)((a1 >> (7 * l)) & 127);
      const std::uint8_t s = (std::uint8_t)(v ^ ((popc8(v) & 1) << 7));
      for (int j = 0; j < 4; ++j) {
        const float val = db * (float)g1[j];
        *out++ = (s >> j) & 1 ? -val : val;
      }
      for (int j = 0; j < 4; ++j) {
        const float val = db * (float)g2[j];
        *out++ = (s >> (4 + j)) & 1 ? -val : val;
      }
    }
  }
}

void dq_iq2s(const std::uint8_t* b, float* out) {
  std::uint16_t dh;
  std::memcpy(&dh, b, 2);
  const float d = fp16_to_f32(dh);
  const std::uint8_t* qs = b + 2;        // 32 grid idx + 32 signs
  const std::uint8_t* sgns = qs + 32;
  const std::uint8_t* qh = qs + 64;      // 8
  const std::uint8_t* sc = qs + 72;      // 8
  for (int ib = 0; ib < 8; ++ib) {
    const float db0 = d * 0.25f * (0.5f + (float)(sc[ib] & 0xf));
    const float db1 = d * 0.25f * (0.5f + (float)(sc[ib] >> 4));
    for (int l = 0; l < 4; ++l) {
      const int idx = qs[4 * ib + l] | ((qh[ib] << (8 - 2 * l)) & 0x300);
      const std::uint8_t* grid = (const std::uint8_t*)&iq2s_grid_host[idx];
      const std::uint8_t sgn = sgns[4 * ib + l];
      const float dl = l < 2 ? db0 : db1;
      for (int j = 0; j < 8; ++j) {
        const float val = dl * (float)grid[j];
        *out++ = (sgn >> j) & 1 ? -val : val;
      }
    }
  }
}

// IQ4_NL: one 32-element block = 18 bytes (f16 d + 16 B of nibbles); eight per 256. Value =
// d * kvalues_iq4nl[nib]; the low nibble feeds positions +0..15, the high nibble +16..31 of each
// 4-byte group (iq_kernels.cu dq_iq4_nl).
void dq_iq4nl(const std::uint8_t* b, float* out) {
  for (int ib = 0; ib < 8; ++ib) {
    const float d = fp16_to_f32(read16(b + 18 * ib));
    const std::uint8_t* q4 = b + 18 * ib + 2;
    for (int il = 0; il < 4; ++il) {
      for (int j = 0; j < 4; ++j) {
        const std::uint8_t byte = q4[4 * il + j];
        out[32 * ib + 4 * il + j] = d * (float)kvalues_iq4nl_host[byte & 0xf];
        out[32 * ib + 4 * il + j + 16] = d * (float)kvalues_iq4nl_host[byte >> 4];
      }
    }
  }
}

// IQ4_XS: one 256-element block = 136 bytes (f16 d, u16 scales_h, 4 B scales_l, 128 B nibbles).
// Per 32-column group the 6-bit ls = (scales_l nibble) | (scales_h bits << 4) offsets by -32;
// value = d * (ls - 32) * kvalues_iq4nl[nib] (iq_kernels.cu dq_iq4_xs; mirrors u_iq.cu's
// iq4xs_elem). qh is not used by this engine's IQ4_XS decode.
void dq_iq4xs(const std::uint8_t* b, float* out) {
  const float d0 = fp16_to_f32(read16(b));
  const std::uint16_t sh = read16(b + 2);
  const std::uint8_t* sl = b + 4;   // 4 bytes: two group nibbles each
  const std::uint8_t* qs = b + 8;   // 128 bytes of nibbles
  for (int ib = 0; ib < 8; ++ib) {
    const int ls = ((sl[ib / 2] >> (4 * (ib % 2))) & 0xf) | (((sh >> (2 * ib)) & 0x3u) << 4);
    const float d = d0 * (float)(ls - 32);
    const std::uint8_t* q4 = qs + 16 * ib;
    for (int il = 0; il < 4; ++il) {
      for (int j = 0; j < 4; ++j) {
        const std::uint8_t byte = q4[4 * il + j];
        out[32 * ib + 4 * il + j] = d * (float)kvalues_iq4nl_host[byte & 0xf];
        out[32 * ib + 4 * il + j + 16] = d * (float)kvalues_iq4nl_host[byte >> 4];
      }
    }
  }
}

// IQ3_S: one 256-element block = 110 bytes (f16 d, 64 B qs, 8 B qh, 32 B signs, 4 B scales). Each
// qs byte picks a 4-value grid entry, extended to a 9-bit index by one qh bit; one 4-bit scale per
// 32-column chunk multiplies by (1 + 2*sc); signs flip the grid bytes (iq_kernels.cu dq_iq3_s).
void dq_iq3s(const std::uint8_t* b, float* out) {
  const float d0 = fp16_to_f32(read16(b));
  const std::uint8_t* qs = b + 2;    // 64 grid indices
  const std::uint8_t* qh = b + 66;   // 9th grid-index bit per lookup
  const std::uint8_t* sg = b + 74;   // 32 sign bytes (one per (chunk, il) pair of grid lookups)
  const std::uint8_t* sc = b + 106;  // 4 scale bytes (two 32-column chunks each)
  for (int ib = 0; ib < 8; ++ib) {
    const float d = d0 * (float)(1 + 2 * ((sc[ib / 2] >> (4 * (ib % 2))) & 0xf));
    for (int il = 0; il < 4; ++il) {
      const std::uint32_t g1 = iq3s_grid_host[qs[8 * ib + 2 * il + 0] | ((qh[ib] << (8 - 2 * il)) & 0x100)];
      const std::uint32_t g2 = iq3s_grid_host[qs[8 * ib + 2 * il + 1] | ((qh[ib] << (7 - 2 * il)) & 0x100)];
      const std::uint8_t signs = sg[4 * ib + il];
      for (int j = 0; j < 4; ++j) {
        const float v1 = d * (float)((g1 >> (8 * j)) & 0xffu);
        const float v2 = d * (float)((g2 >> (8 * j)) & 0xffu);
        out[32 * ib + 8 * il + j] = (signs >> j) & 1 ? -v1 : v1;
        out[32 * ib + 8 * il + j + 4] = (signs >> (4 + j)) & 1 ? -v2 : v2;
      }
    }
  }
}

// Q2_0 (Strata-custom, id 42): one 64-element block = 18 bytes (f16 d + 16 B packing four 2-bit
// codes per byte); four per 256. Value = d * (code - 1) -- codes 0..3 decode to -1, 0, +1, +2
// (iq_kernels.cu dq_q2_0, matching u_iq.cu's Q8_1 dot byte-perm mapping).
void dq_q2_0(const std::uint8_t* b, float* out) {
  for (int blk = 0; blk < 4; ++blk) {
    const float d = fp16_to_f32(read16(b + 18 * blk));
    const std::uint8_t* qs = b + 18 * blk + 2;
    for (int i = 0; i < 64; ++i) {
      const int code = (qs[i / 4] >> (2 * (i % 4))) & 3;
      out[64 * blk + i] = d * (float)(code - 1);
    }
  }
}

// y[n_out] = W(type) . x, W row-major [n_out, n_in] (n_in % 256 == 0), x[n_in].
void matvec(int t, const void* w, int n_out, int n_in, const float* x, float* y) {
  const int nb = n_in / 256;
  const std::size_t bb = (std::size_t)block_bytes(t);
  const std::size_t row_bytes = (std::size_t)nb * bb;
  const std::uint8_t* W = (const std::uint8_t*)w;
  float blk[256];
  for (int r = 0; r < n_out; ++r) {
    const std::uint8_t* row = W + (std::size_t)r * row_bytes;
    float acc = 0.0f;
    for (int ib = 0; ib < nb; ++ib) {
      const std::uint8_t* bp = row + (std::size_t)ib * bb;
      if (t == kIq2xxs) dq_iq2xxs(bp, blk);
      else if (t == kIq2s) dq_iq2s(bp, blk);
      else if (t == kIq3xxs) dq_iq3xxs(bp, blk);
      else if (t == kIq4nl) dq_iq4nl(bp, blk);
      else if (t == kIq3s) dq_iq3s(bp, blk);
      else if (t == kIq4xs) dq_iq4xs(bp, blk);
      else dq_q2_0(bp, blk);
      const float* xx = x + (std::size_t)ib * 256;
      for (int j = 0; j < 256; ++j) acc += blk[j] * xx[j];
    }
    y[r] = acc;
  }
}

inline float act_one(int act, float g, float u, float limit) {
  if (act == 0) {  // geglu: gelu(g) * u (tanh approximation, matching u::k::geglu)
    const float gg = 0.5f * g * (1.0f + std::tanh(0.7978845608f * (g + 0.044715f * g * g * g)));
    return gg * u;
  }
  if (act == 1) {  // swiglu: silu(g) * u
    return (g / (1.0f + std::exp(-g))) * u;
  }
  // swiglu_clamped (GLM/ds4/raon1): silu(min(g, limit)) * clamp(u, -limit, +limit)
  const float gc = g < limit ? g : limit;
  const float sg = gc / (1.0f + std::exp(-gc));
  const float uc = u < -limit ? -limit : (u > limit ? limit : u);
  return sg * uc;
}

}  // namespace

bool cpu_expert_supported(int gate_type, int up_type, int down_type) {
  return is_iq(gate_type) && is_iq(up_type) && is_iq(down_type);
}

void cpu_expert_ffn(int gate_type, const void* gate, int up_type, const void* up, int down_type,
                    const void* down, int n_embd, int n_ff, int act, float limit, const float* x,
                    float* out, float* scratch) {
  float* g = scratch;
  float* u = scratch + n_ff;
  matvec(gate_type, gate, n_ff, n_embd, x, g);
  matvec(up_type, up, n_ff, n_embd, x, u);
  for (int i = 0; i < n_ff; ++i) g[i] = act_one(act, g[i], u[i], limit);
  matvec(down_type, down, n_embd, n_ff, g, out);
}

}  // namespace u
