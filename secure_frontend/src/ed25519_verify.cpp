// SP7 M2 Task 1 (spec D-2): RFC 8032 Ed25519 verification, verify path only.
//
// Structure follows RFC 8032 section 6 - the reference Python the RFC itself
// contains - whose behaviour the RFC 8032 section 7.1 vectors in
// tests/ed25519_test.cpp were cross-checked against before being inlined:
// extended coordinates (X, Y, Z, T) with x = X/Z, y = Y/Z, x*y = T/Z; the
// unified point_add of section 6; the canonical S < L check of the RFC's
// verify; no cofactor multiplication; no signing here (plan binding: "no
// signing in C++"). Field arithmetic is portable 4x64-bit limbs with a
// 64x64->128 multiply built from 32-bit halves (the build runs -Wpedantic,
// which rules out __int128) and fold-by-38 reduction (2^256 = 38 mod p,
// p = 2^255 - 19); every helper leaves its result < p.
#include "ed25519_verify.hpp"

#include "sha512.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace secure_frontend {
namespace {

using u64 = std::uint64_t;

struct BorrowPair final {
  u64 diff;
  u64 borrow;
};

constexpr BorrowPair subtract_with_borrow(const u64 a, const u64 b, const u64 borrow) noexcept {
  const u64 d1 = a - b;
  const u64 borrow1 = a < b ? 1ULL : 0ULL;
  const u64 d2 = d1 - borrow;
  const u64 borrow2 = d1 < borrow ? 1ULL : 0ULL;
  return BorrowPair{d2, borrow1 | borrow2};
}

// Minimal unsigned 128-bit value built from standard integers.
struct U128 final {
  u64 lo;
  u64 hi;
};

constexpr U128 mul_u64(const u64 a, const u64 b) noexcept {
  const u64 al = a & 0xFFFFFFFFULL;
  const u64 ah = a >> 32;
  const u64 bl = b & 0xFFFFFFFFULL;
  const u64 bh = b >> 32;
  const u64 ll = al * bl;
  const u64 lh = al * bh;
  const u64 hl = ah * bl;
  const u64 hh = ah * bh;
  const u64 mid = (ll >> 32) + (lh & 0xFFFFFFFFULL) + (hl & 0xFFFFFFFFULL);  // < 3 * 2^32
  const u64 lo = (mid << 32) | (ll & 0xFFFFFFFFULL);  // bits past 64 reappear as mid >> 32
  const u64 hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
  return U128{lo, hi};
}

constexpr U128 u128_add(const U128 a, const U128 b) noexcept {
  const u64 lo = a.lo + b.lo;
  return U128{lo, a.hi + b.hi + (lo < a.lo ? 1ULL : 0ULL)};
}

constexpr U128 u128_from(const u64 lo) noexcept { return U128{lo, 0ULL}; }

// a + b + c for u64 inputs; the carry out is 0, 1 or 2 (three 64-bit addends
// sum to below 3 * 2^64).
constexpr U128 add_u64x3(const u64 a, const u64 b, const u64 c) noexcept {
  const u64 s1 = a + b;
  const u64 carry1 = s1 < a ? 1ULL : 0ULL;
  const u64 s2 = s1 + c;
  const u64 carry2 = s2 < c ? 1ULL : 0ULL;
  return U128{s2, carry1 + carry2};
}

// Little-endian 64-bit limbs, kept < p after every operation below.
struct Field final {
  u64 v[4];
};

constexpr Field kZero{0ULL, 0ULL, 0ULL, 0ULL};
constexpr Field kOne{1ULL, 0ULL, 0ULL, 0ULL};
constexpr Field kP {0xFFFFFFFFFFFFFFEDULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL};

// Group order L (named q in RFC 8032 section 6).
constexpr std::array<u64, 4> kOrder {0x5812631A5CF5D3EDULL, 0x14DEF9DEA2F79CD6ULL, 0x0000000000000000ULL, 0x1000000000000000ULL};

// Exponents: p - 2 (Fermat inversion), (p + 3) / 8 and (p - 1) / 4 (section 6
// recover_x), as limbs; computed offline, not transcribed by hand.
constexpr std::array<u64, 4> kExpPMinus2 {0xFFFFFFFFFFFFFFEBULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL};
constexpr std::array<u64, 4> kExpPPlus3Div8 {0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0x0FFFFFFFFFFFFFFFULL};
constexpr std::array<u64, 4> kExpPMinus1Div4 {0xFFFFFFFFFFFFFFFBULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0x1FFFFFFFFFFFFFFFULL};

bool field_is_zero(const Field& a) noexcept {
  return a.v[0] == 0ULL && a.v[1] == 0ULL && a.v[2] == 0ULL && a.v[3] == 0ULL;
}

bool is_ge_p(const Field& a) noexcept {
  for (int i = 3; i >= 0; --i) {
    if (a.v[i] != kP.v[i]) return a.v[i] > kP.v[i];
  }
  return true;  // equal
}

void subtract_p(Field& a) noexcept {
  // Only called when a >= p, so (a - p) mod 2^256 = a - p exactly.
  u64 borrow = 0ULL;
  for (int i = 0; i < 4; ++i) {
    const BorrowPair diff = subtract_with_borrow(a.v[i], kP.v[i], borrow);
    a.v[i] = diff.diff;
    borrow = diff.borrow;
  }
}

// Brings a value < 2^256 (fold form) down to < p.
void canonical(Field& a) noexcept {
  if ((a.v[3] >> 63) != 0) {  // 2^255 = 19 mod p
    a.v[3] &= 0x7FFFFFFFFFFFFFFFULL;
    u64 carry = 19ULL;
    for (int i = 0; i < 4 && carry != 0ULL; ++i) {
      const U128 sum = add_u64x3(a.v[i], carry, 0ULL);
      a.v[i] = sum.lo;
      carry = sum.hi;
    }
  }
  if (is_ge_p(a)) subtract_p(a);
  if (is_ge_p(a)) subtract_p(a);
}

Field field_add(const Field& a, const Field& b) noexcept {
  Field r{};
  u64 carry = 0ULL;
  for (int i = 0; i < 4; ++i) {
    const U128 sum = add_u64x3(a.v[i], b.v[i], carry);
    r.v[i] = sum.lo;
    carry = sum.hi;
  }
  if (carry != 0ULL) {  // 2^256 = 38 mod p
    u64 fold = 38ULL;
    for (int i = 0; i < 4 && fold != 0ULL; ++i) {
      const U128 sum = add_u64x3(r.v[i], fold, 0ULL);
      r.v[i] = sum.lo;
      fold = sum.hi;
    }
  }
  canonical(r);
  return r;
}

Field field_sub(const Field& a, const Field& b) noexcept {
  // a + (p - b): b < p, p - b in (0, p], the sum < 2p < 2^256.
  Field neg_p{};
  u64 borrow = 0ULL;
  for (int i = 0; i < 4; ++i) {
    const BorrowPair diff = subtract_with_borrow(kP.v[i], b.v[i], borrow);
    neg_p.v[i] = diff.diff;
    borrow = diff.borrow;
  }
  canonical(neg_p);  // b == 0 leaves p; normalize to 0
  return field_add(a, neg_p);
}

Field field_mul(const Field& a, const Field& b) noexcept {
  // Schoolbook into eight u64 words. Overflow out of the top word c[7] is
  // possible (the true product needs up to 510 bits); it is kept in
  // top_carry * 2^512 and folded by 38*38 below, never dropped.
  std::array<u64, 8> c{};
  u64 top_carry = 0ULL;
  for (int i = 0; i < 4; ++i) {
    u64 carry = 0ULL;
    for (int j = 0; j < 4; ++j) {
      const U128 prod = mul_u64(a.v[i], b.v[j]);
      const U128 s1 = add_u64x3(c[i + j], prod.lo, carry);
      c[i + j] = s1.lo;
      carry = prod.hi + s1.hi;  // prod.hi < 2^64 - 1 and s1.hi <= 1, so no wrap
    }
    const U128 s2 = add_u64x3(c[i + 4], carry, 0ULL);
    c[i + 4] = s2.lo;
    top_carry += s2.hi;  // one carry per row at most, and fewer rows than 2^64
  }
  Field r{{c[0], c[1], c[2], c[3]}};
  U128 carry = u128_from(0ULL);
  for (int k = 4; k < 8; ++k) {  // 2^(64k) = 38 * 2^(64(k-4)) mod p
    carry = u128_add(carry, mul_u64(c[k], 38ULL));
    const U128 sum = add_u64x3(r.v[k - 4], carry.lo, 0ULL);
    r.v[k - 4] = sum.lo;
    carry = U128{sum.hi + carry.hi, 0ULL};  // pending value, one lane up
  }
  // top_carry sits at 2^512 = 38^2 mod p: one factor of 38 joins the pending
  // fold here, the second is applied by the tail loop below.
  carry = u128_add(carry, mul_u64(top_carry, 38ULL));
  while ((carry.hi != 0ULL) || (carry.lo != 0ULL)) {  // carry out folds by 38 again
    U128 fold = mul_u64(carry.lo, 38ULL);
    fold.hi += carry.hi * 38ULL;
    const U128 sum = add_u64x3(r.v[0], fold.lo, 0ULL);
    r.v[0] = sum.lo;
    u64 carry2 = sum.hi + fold.hi;
    for (int i = 1; i < 4 && carry2 != 0ULL; ++i) {
      const U128 nxt = add_u64x3(r.v[i], carry2, 0ULL);
      r.v[i] = nxt.lo;
      carry2 = nxt.hi;
    }
    carry = u128_from(carry2);
  }
  canonical(r);
  return r;
}

// Fermat inversion with square-and-multiply. Inputs here are always public
// verifier values, so the variable-time loop leaks nothing secret.
Field field_pow(const Field& base, const std::array<u64, 4>& exponent) noexcept {
  Field result = kOne;
  Field power = base;
  for (int bit = 0; bit < 256; ++bit) {
    if (((exponent[bit / 64] >> (bit % 64)) & 1ULL) != 0ULL) {
      result = field_mul(result, power);
    }
    power = field_mul(power, power);
  }
  return result;
}

Field field_inv(const Field& a) noexcept { return field_pow(a, kExpPMinus2); }

u64 field_low_bit(const Field& a) noexcept { return a.v[0] & 1ULL; }

// d = -121665 / 121666 mod p (RFC 8032 section 5.1).
Field curve_d() noexcept {
  static const Field d = [] {
    const Field neg_num {0xFFFFFFFFFFFE24ACULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL};
    const Field denominator{121666ULL, 0ULL, 0ULL, 0ULL};
    return field_mul(neg_num, field_inv(denominator));
  }();
  return d;
}

Field curve_2d() noexcept {
  static const Field twice = [] { return field_add(curve_d(), curve_d()); }();
  return twice;
}

// sqrt(-1) = 2^((p-1)/4) (RFC 8032 section 6 modp_sqrt_m1).
Field curve_sqrt_minus_one() noexcept {
  static const Field s = [] {
    const Field two{2ULL, 0ULL, 0ULL, 0ULL};
    return field_pow(two, kExpPMinus1Div4);
  }();
  return s;
}

// RFC 8032 section 6 recover_x. y is already reduced mod p; sign selects x.
bool recover_x(const Field& y, const u64 sign, Field& x) noexcept {
  const Field y2 = field_mul(y, y);
  const Field denom = field_add(field_mul(curve_d(), y2), kOne);
  const Field x2 = field_mul(field_sub(y2, kOne), field_inv(denom));
  if (field_is_zero(x2)) {
    if (sign != 0ULL) return false;
    x = kZero;
    return true;
  }
  Field root = field_pow(x2, kExpPPlus3Div8);
  if (!field_is_zero(field_sub(field_mul(root, root), x2))) {
    root = field_mul(root, curve_sqrt_minus_one());
    if (!field_is_zero(field_sub(field_mul(root, root), x2))) return false;
  }
  if (field_low_bit(root) != sign) {
    root = field_sub(kZero, root);
  }
  x = root;
  return true;
}

// RFC 8032 section 6 point_decompress: false for y >= p or a non-square x^2.
bool point_decompress(const std::uint8_t* bytes, Field& x, Field& y) noexcept {
  y = Field{};
  for (int i = 0; i < 4; ++i) {
    u64 word = 0ULL;
    for (int j = 0; j < 8; ++j) {
      word = (word << 8) | bytes[8 * i + (7 - j)];
    }
    y.v[i] = word;
  }
  const u64 sign = (y.v[3] >> 63) & 1ULL;
  y.v[3] &= 0x7FFFFFFFFFFFFFFFULL;
  if (is_ge_p(y)) return false;
  return recover_x(y, sign, x);
}

struct Point final {
  Field X, Y, Z, T;
};

Point point_identity() noexcept { return Point{kZero, kOne, kOne, kZero}; }

// RFC 8032 section 6 point_add, unified (valid for doubling too):
//   A = (y1-x1)(y2-x2), B = (y1+x1)(y2+x2), C = 2 d t1 t2, D = 2 z1 z2,
//   E = B-A, F = D-C, G = D+C, H = B+A, return (E*F, G*H, F*G, E*H).
Point point_add(const Point& p1, const Point& p2) noexcept {
  const Field a = field_mul(field_sub(p1.Y, p1.X), field_sub(p2.Y, p2.X));
  const Field b = field_mul(field_add(p1.Y, p1.X), field_add(p2.Y, p2.X));
  const Field c = field_mul(field_mul(p1.T, p2.T), curve_2d());
  const Field dd = field_add(field_mul(p1.Z, p2.Z), field_mul(p1.Z, p2.Z));
  const Field e = field_sub(b, a);
  const Field f = field_sub(dd, c);
  const Field g = field_add(dd, c);
  const Field h = field_add(b, a);
  return Point{field_mul(e, f), field_mul(g, h), field_mul(f, g), field_mul(e, h)};
}

// RFC 8032 section 6 point_mul (bottom-up double-and-add over public scalars).
Point point_mul(const std::array<u64, 4>& scalar, const Point& input) noexcept {
  Point result = point_identity();
  Point power = input;
  for (int bit = 0; bit < 256; ++bit) {
    if (((scalar[bit / 64] >> (bit % 64)) & 1ULL) != 0ULL) {
      result = point_add(result, power);
    }
    power = point_add(power, power);
  }
  return result;
}

// RFC 8032 section 6 point_equal: projective cross-multiplication.
bool point_equal(const Point& p1, const Point& p2) noexcept {
  if (!field_is_zero(field_sub(field_mul(p1.X, p2.Z), field_mul(p2.X, p1.Z)))) return false;
  return field_is_zero(field_sub(field_mul(p1.Y, p2.Z), field_mul(p2.Y, p1.Z)));
}

// G = base point (RFC 8032 section 6: g_y = 4/5, g_x = recover_x(g_y, 0)).
const Point& base_point() noexcept {
  static const Point g = [] {
    const Field four{4ULL, 0ULL, 0ULL, 0ULL};
    const Field five{5ULL, 0ULL, 0ULL, 0ULL};
    const Field gy = field_mul(four, field_inv(five));
    Field gx{};
    const bool ok = recover_x(gy, 0ULL, gx);
    if (!ok) return point_identity();  // unreachable; guards the static
    return Point{gx, gy, kOne, field_mul(gx, gy)};
  }();
  return g;
}

bool scalar_lt_order(const std::array<u64, 4>& s) noexcept {
  for (int i = 3; i >= 0; --i) {
    if (s[i] != kOrder[i]) return s[i] < kOrder[i];
  }
  return false;  // equal
}

// RFC 8032 section 6 sha512_modq: little-endian 512-bit integer mod L, done
// bit by bit (the accumulator stays < L < 2^253, so doubling cannot carry
// past limb 3), one conditional subtract per step.
std::array<u64, 4> hash_mod_order(const Sha512Digest& digest) noexcept {
  std::array<u64, 4> r{};
  for (int i = 511; i >= 0; --i) {
    u64 carry = 0ULL;
    for (int k = 0; k < 4; ++k) {
      const u64 next_carry = r[k] >> 63;
      r[k] = (r[k] << 1) | carry;
      carry = next_carry;
    }
    const u64 bit = (digest[static_cast<std::size_t>(i / 8)] >> (i % 8)) & 1ULL;
    r[0] |= bit;
    std::array<u64, 4> trial{};
    u64 borrow = 0ULL;
    for (int k = 0; k < 4; ++k) {
      const BorrowPair diff = subtract_with_borrow(r[k], kOrder[k], borrow);
      trial[k] = diff.diff;
      borrow = diff.borrow;
    }
    if (borrow == 0ULL) {
      r = trial;
    }
  }
  return r;
}

}  // namespace

bool ed25519_verify(const std::array<std::uint8_t, 32>& public_key, std::span<const std::uint8_t> message,
                    const std::array<std::uint8_t, 64>& signature) noexcept {
  Field ax{};
  Field ay{};
  if (!point_decompress(public_key.data(), ax, ay)) return false;
  Field rx{};
  Field ry{};
  if (!point_decompress(signature.data(), rx, ry)) return false;

  std::array<u64, 4> s{};
  for (int i = 0; i < 4; ++i) {
    u64 word = 0ULL;
    for (int j = 0; j < 8; ++j) {
      word = (word << 8) | signature[32 + 8 * i + (7 - j)];
    }
    s[i] = word;
  }
  if (!scalar_lt_order(s)) return false;  // RFC 8032 canonical encoding check

  // k = sha512(R-bytes || A-bytes || message) mod L, exactly as section 6.
  Sha512 hash;
  hash.update(std::span<const std::uint8_t>{signature.data(), 32});
  hash.update(std::span<const std::uint8_t>{public_key.data(), 32});
  hash.update(message);
  const std::array<u64, 4> k = hash_mod_order(hash.finish());

  const Point point_s_b = point_mul(s, base_point());
  const Point point_a{ax, ay, kOne, field_mul(ax, ay)};
  const Point point_h_a = point_mul(k, point_a);
  const Point point_r{rx, ry, kOne, field_mul(rx, ry)};
  return point_equal(point_s_b, point_add(point_r, point_h_a));
}

}  // namespace secure_frontend
