// multiengine_flint: every kernel against a CPU computation in double precision on random data (needs the GPU). The
// helpers are Gemma 4's (tests/gemma4/g4_kernel_tests.cu), the cases flint's.
#include "f_kernels.cuh"
#include "g4_test.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using g4_test::expect;

namespace {

std::mt19937 rng(20261027);

std::vector<float> randv(std::size_t n, float lo = -1.0f, float hi = 1.0f) {
  std::uniform_real_distribution<float> d(lo, hi);
  std::vector<float> v(n);
  for (auto& x : v) x = d(rng);
  return v;
}

template <typename T>
T* dev(const std::vector<T>& h) {
  T* p = nullptr;
  if (cudaMalloc(&p, std::max<std::size_t>(1, h.size()) * sizeof(T)) != cudaSuccess) std::abort();
  if (!h.empty() && cudaMemcpy(p, h.data(), h.size() * sizeof(T), cudaMemcpyHostToDevice) != cudaSuccess) std::abort();
  return p;
}

template <typename T>
std::vector<T> host(const T* p, std::size_t n) {
  std::vector<T> h(n);
  if (cudaDeviceSynchronize() != cudaSuccess) {
    std::fprintf(stderr, "CUDA error: %s\n", cudaGetErrorString(cudaGetLastError()));
    std::abort();
  }
  if (cudaMemcpy(h.data(), p, n * sizeof(T), cudaMemcpyDeviceToHost) != cudaSuccess) std::abort();
  return h;
}

bool close(const std::vector<float>& got, const std::vector<double>& want, double rel, double abs_tol, const std::string& what) {
  if (got.size() != want.size()) {
    expect(false, what + ": size");
    return false;
  }
  double worst = 0.0;
  std::size_t at = 0;
  for (std::size_t i = 0; i < got.size(); ++i) {
    const double err = std::fabs(double(got[i]) - want[i]);
    const double tol = abs_tol + rel * std::fabs(want[i]);
    if (!(err <= tol) && (err - tol > worst || worst == 0.0)) {
      worst = std::max(err - tol, 1e-300);
      at = i;
    }
  }
  if (worst > 0.0) {
    expect(false, what + ": index " + std::to_string(at) + " got " + std::to_string(got[at]) + " want " + std::to_string(want[at]));
    return false;
  }
  return true;
}

// Q8_0 quantisation into the engine's two planes (and the values it stands for).
struct Q8 {
  std::vector<std::int8_t> q;
  std::vector<__half> s;
  std::vector<float> values;
};
Q8 quantise(const std::vector<float>& w, int rows, int cols) {
  Q8 r;
  r.q.resize(w.size());
  r.s.resize(w.size() / 32);
  r.values.resize(w.size());
  for (int i = 0; i < rows; ++i) {
    for (int b = 0; b < cols / 32; ++b) {
      float amax = 0.0f;
      for (int j = 0; j < 32; ++j) amax = std::max(amax, std::fabs(w[std::size_t(i) * cols + b * 32 + j]));
      const float d = amax / 127.0f;
      const __half dh = __float2half(d);
      const float dr = __half2float(dh);
      r.s[std::size_t(i) * (cols / 32) + b] = dh;
      for (int j = 0; j < 32; ++j) {
        const std::size_t k = std::size_t(i) * cols + b * 32 + j;
        const int v = d == 0.0f ? 0 : int(std::lround(w[k] / d));
        r.q[k] = std::int8_t(std::clamp(v, -127, 127));
        r.values[k] = float(r.q[k]) * dr;
      }
    }
  }
  return r;
}

std::vector<std::uint16_t> to_bf16(const std::vector<float>& w, std::vector<float>& values) {
  std::vector<std::uint16_t> b(w.size());
  values.resize(w.size());
  for (std::size_t i = 0; i < w.size(); ++i) {
    std::uint32_t u;
    std::memcpy(&u, &w[i], 4);
    b[i] = std::uint16_t((u + 0x7FFF + ((u >> 16) & 1)) >> 16);
    const std::uint32_t back = std::uint32_t(b[i]) << 16;
    std::memcpy(&values[i], &back, 4);
  }
  return b;
}

std::vector<double> cpu_matvec(const std::vector<float>& w, const std::vector<float>& x, int rows, int cols, int T) {
  std::vector<double> y(std::size_t(T) * rows);
  for (int t = 0; t < T; ++t)
    for (int r = 0; r < rows; ++r) {
      double a = 0.0;
      for (int c = 0; c < cols; ++c) a += double(w[std::size_t(r) * cols + c]) * x[std::size_t(t) * cols + c];
      y[std::size_t(t) * rows + r] = a;
    }
  return y;
}

void matvec_matches_cpu_for_each_kind() {
  const int rows = 4096, cols = 2048, T = 3;
  const auto w = randv(std::size_t(rows) * cols), x = randv(std::size_t(T) * cols);
  float* dx = dev(x);
  float* dy = nullptr;
  cudaMalloc(&dy, sizeof(float) * T * rows);
  {
    const Q8 q = quantise(w, rows, cols);
    auto* dq = dev(q.q);
    auto* ds = dev(q.s);
    flk::matvec({flk::WKind::q8, rows, cols, dq, ds}, dx, dy, T, nullptr);
    close(host(dy, std::size_t(T) * rows), cpu_matvec(q.values, x, rows, cols, T), 1e-4, 1e-3, "Q8_0 matvec");
    cudaFree(dq);
    cudaFree(ds);
  }
  {
    std::vector<float> values;
    const auto b = to_bf16(w, values);
    auto* db = dev(b);
    flk::matvec({flk::WKind::bf16, rows, cols, db, nullptr}, dx, dy, T, nullptr);
    close(host(dy, std::size_t(T) * rows), cpu_matvec(values, x, rows, cols, T), 1e-4, 1e-3, "BF16 matvec");
    cudaFree(db);
  }
  {
    float* dw = dev(w);
    flk::matvec({flk::WKind::f32, rows, cols, dw, nullptr}, dx, dy, T, nullptr);
    close(host(dy, std::size_t(T) * rows), cpu_matvec(w, x, rows, cols, T), 1e-4, 1e-3, "F32 matvec");
    cudaFree(dw);
  }
  cudaFree(dx);
  cudaFree(dy);
}

void norms_adds_and_embedding_match_cpu() {
  const int n = 2048, rows = 3;
  const auto x = randv(std::size_t(rows) * n, -3.0f, 3.0f), w = randv(n, 0.5f, 1.5f), a = randv(std::size_t(rows) * n),
             b = randv(std::size_t(rows) * n);
  float *dx = dev(x), *dw = dev(w), *da = dev(a), *db = dev(b), *dout = nullptr;
  cudaMalloc(&dout, sizeof(float) * rows * n);
  flk::rmsnorm(dx, dw, dout, rows, n, 1e-6f, nullptr);
  std::vector<double> want(std::size_t(rows) * n);
  for (int r = 0; r < rows; ++r) {
    double ss = 0.0;
    for (int i = 0; i < n; ++i) ss += double(x[std::size_t(r) * n + i]) * x[std::size_t(r) * n + i];
    const double inv = 1.0 / std::sqrt(ss / n + 1e-6);
    for (int i = 0; i < n; ++i) want[std::size_t(r) * n + i] = x[std::size_t(r) * n + i] * inv * w[std::size_t(i)];
  }
  close(host(dout, want.size()), want, 1e-5, 1e-5, "rmsnorm");
  flk::add_rows(dx, da, db, dout, rows, n, nullptr);
  for (std::size_t i = 0; i < want.size(); ++i) want[i] = double(x[i]) + a[i] + b[i];
  close(host(dout, want.size()), want, 1e-6, 1e-6, "add_rows with two addends");
  flk::add_rows(dx, da, nullptr, dout, rows, n, nullptr);
  for (std::size_t i = 0; i < want.size(); ++i) want[i] = double(x[i]) + a[i];
  close(host(dout, want.size()), want, 1e-6, 1e-6, "add_rows with one addend");
  // the embedding: Q8_0 and BF16 tables, rows picked by id
  const int vocab = 64;
  const auto table = randv(std::size_t(vocab) * n);
  const std::vector<int> ids{5, 0, 63};
  int* dids = dev(ids);
  const Q8 q = quantise(table, vocab, n);
  auto* dq = dev(q.q);
  auto* ds = dev(q.s);
  flk::embed(dids, 3, {flk::WKind::q8, vocab, n, dq, ds}, dout, nullptr);
  for (int t = 0; t < 3; ++t)
    for (int i = 0; i < n; ++i) want[std::size_t(t) * n + i] = q.values[std::size_t(ids[std::size_t(t)]) * n + i];
  close(host(dout, want.size()), want, 0.0, 0.0, "Q8_0 embedding rows");
  std::vector<float> bv;
  const auto bt = to_bf16(table, bv);
  auto* dbt = dev(bt);
  flk::embed(dids, 3, {flk::WKind::bf16, vocab, n, dbt, nullptr}, dout, nullptr);
  for (int t = 0; t < 3; ++t)
    for (int i = 0; i < n; ++i) want[std::size_t(t) * n + i] = bv[std::size_t(ids[std::size_t(t)]) * n + i];
  close(host(dout, want.size()), want, 0.0, 0.0, "BF16 embedding rows");
  for (void* p : std::initializer_list<void*>{dx, dw, da, db, dout, dids, dq, ds, dbt}) cudaFree(p);
}

// The CPU's q/k head: RMSNorm (weighted), then NeoX RoPE when `rope`. The angle is ggml-cuda's (rope.cu: theta_base =
// pos * powf(theta_scale, i), theta_scale = powf(base, -2 / n_dims) on the host), computed in float as the oracle
// computes it; the rotation then in double.
void cpu_head(std::vector<double>& h, const float* w, bool norm, bool rope, int pos, double base) {
  const int hd = flk::kHead, half = hd / 2;
  if (norm) {
    double ss = 0.0;
    for (double v : h) ss += v * v;
    const double inv = 1.0 / std::sqrt(ss / hd + 1e-6);
    for (int i = 0; i < hd; ++i) h[std::size_t(i)] *= inv * w[i];
  }
  if (rope) {
    const float theta_scale = std::pow(static_cast<float>(base), -2.0f / float(hd));
    for (int i = 0; i < half; ++i) {
      const double theta = static_cast<float>(pos) * std::pow(theta_scale, static_cast<float>(i));
      const double a = h[std::size_t(i)], b = h[std::size_t(i + half)];
      h[std::size_t(i)] = a * std::cos(theta) - b * std::sin(theta);
      h[std::size_t(i + half)] = a * std::sin(theta) + b * std::cos(theta);
    }
  }
}

void qkv_post_follows_the_layer() {
  const int T = 2, hd = flk::kHead, nh = 32, nkv = 4;
  for (const bool rope : {true, false}) {
    for (const int ring : {0, 1025}) {
      const int pos0 = ring ? 1500 : 7, cap = ring ? ring : 64;
      const auto q = randv(std::size_t(T) * nh * hd, -2.0f, 2.0f), k = randv(std::size_t(T) * nkv * hd, -2.0f, 2.0f),
                 v = randv(std::size_t(T) * nkv * hd, -2.0f, 2.0f), qn = randv(hd, 0.5f, 1.5f), kn = randv(hd, 0.5f, 1.5f);
      float *dq = dev(q), *dk = dev(k), *dv = dev(v), *dqn = dev(qn), *dkn = dev(kn), *dqo = nullptr;
      __half *dq16 = nullptr, *dkc = nullptr, *dvc = nullptr;
      cudaMalloc(&dqo, sizeof(float) * T * nh * hd);
      cudaMalloc(&dq16, sizeof(__half) * T * nh * hd);
      cudaMalloc(&dkc, sizeof(__half) * nkv * cap * hd);
      cudaMalloc(&dvc, sizeof(__half) * nkv * cap * hd);
      cudaMemset(dkc, 0, sizeof(__half) * nkv * cap * hd);
      cudaMemset(dvc, 0, sizeof(__half) * nkv * cap * hd);
      flk::QkvPost p{dq, dk, dv, dqn, dkn, rope, 10000.0f, pos0, T, ring, cap, 1e-6f, dqo, dq16, dkc, dvc, 0.0f, nullptr};
      flk::qkv_post(p, nullptr);
      const auto qo = host(dqo, std::size_t(T) * nh * hd);
      const auto q16 = host(dq16, std::size_t(T) * nh * hd);
      const auto kc = host(dkc, std::size_t(nkv) * cap * hd);
      const auto vc = host(dvc, std::size_t(nkv) * cap * hd);
      std::vector<double> want_q(qo.size());
      std::vector<float> q16f(q16.size()), kgot, vgot;
      std::vector<double> want_k, want_v;
      for (std::size_t i = 0; i < q16.size(); ++i) q16f[i] = __half2float(q16[i]);
      for (int t = 0; t < T; ++t) {
        const int pos = pos0 + t, slot = ring ? pos % ring : pos;
        for (int h = 0; h < nh; ++h) {
          std::vector<double> hh(q.begin() + (std::ptrdiff_t(t) * nh + h) * hd, q.begin() + (std::ptrdiff_t(t) * nh + h + 1) * hd);
          cpu_head(hh, qn.data(), true, rope, pos, 10000.0);
          std::copy(hh.begin(), hh.end(), want_q.begin() + (std::ptrdiff_t(t) * nh + h) * hd);
        }
        for (int h = 0; h < nkv; ++h) {
          std::vector<double> kk(k.begin() + (std::ptrdiff_t(t) * nkv + h) * hd, k.begin() + (std::ptrdiff_t(t) * nkv + h + 1) * hd);
          cpu_head(kk, kn.data(), true, rope, pos, 10000.0);
          want_k.insert(want_k.end(), kk.begin(), kk.end());
          want_v.insert(want_v.end(), v.begin() + (std::ptrdiff_t(t) * nkv + h) * hd, v.begin() + (std::ptrdiff_t(t) * nkv + h + 1) * hd);
          for (int i = 0; i < hd; ++i) {
            kgot.push_back(__half2float(kc[(std::size_t(h) * cap + slot) * hd + i]));
            vgot.push_back(__half2float(vc[(std::size_t(h) * cap + slot) * hd + i]));
          }
        }
      }
      const std::string tag = std::string(rope ? "RoPE" : "no RoPE") + (ring ? ", ring" : ", full");
      // The angle is a float of magnitude up to pos (rope.cu's theta_base), and the GPU's powf / sincosf and the host's
      // agree only to a few ulps of it: each rotated value then carries up to 4 ulps of the angle times its pair's
      // magnitude on top of the base tolerance (at pos 1500 that is ~2e-4 rad; at pos 7-8, under 4e-6).
      if (rope) {
        double worst = 0.0;
        std::size_t at = 0;
        bool bad = false;
        for (int t = 0; t < T; ++t)
          for (int h = 0; h < nh; ++h)
            for (int i = 0; i < hd; ++i) {
              const std::size_t e = (std::size_t(t) * nh + h) * hd + std::size_t(i);
              const int j = i % (hd / 2);
              const std::size_t a = e - std::size_t(i) + std::size_t(j), b = a + hd / 2;
              const double theta = double(pos0 + t) * std::pow(10000.0, -2.0 * j / hd);
              const double ulp = std::ldexp(1.0, std::ilogb(std::max(theta, 1e-30)) - 23);
              const double mag = std::hypot(want_q[a], want_q[b]);
              const double tol = 1e-4 + 1e-4 * std::fabs(want_q[e]) + 4.0 * ulp * mag;
              const double err = std::fabs(double(qo[e]) - want_q[e]);
              if (err > tol && (!bad || err - tol > worst)) {
                bad = true;
                worst = err - tol;
                at = e;
              }
            }
        expect(!bad, "q out (" + tag + "): index " + std::to_string(at) + " beyond float RoPE's precision");
      } else {
        close(qo, want_q, 1e-4, 1e-4, "q out (" + tag + ")");
      }
      close(q16f, want_q, 2e-3, 2e-3, "q f16 (" + tag + ")");
      close(kgot, want_k, 2e-3, 2e-3, "k cache row (" + tag + ")");
      close(vgot, want_v, 1e-3, 1e-3, "v cache row, not normalised (" + tag + ")");
      for (void* x : std::initializer_list<void*>{dq, dk, dv, dqn, dkn, dqo, dq16, dkc, dvc}) cudaFree(x);
    }
  }
}

// The reference attention of one query position over the cache, with the sink: double precision.
std::vector<double> cpu_attention(const std::vector<float>& q, const std::vector<__half>& kc, const std::vector<__half>& vc, int cap,
                                  int ring, int window, int pos, const float* sinks, double scale) {
  const int hd = flk::kHead, nh = 32;
  std::vector<double> out(std::size_t(nh) * hd, 0.0);
  const int lo = window > 0 ? std::max(0, pos - window + 1) : 0;
  for (int h = 0; h < nh; ++h) {
    const int g = h / flk::kGroup;
    std::vector<double> s;
    for (int p = lo; p <= pos; ++p) {
      const int slot = ring ? p % ring : p;
      double a = 0.0;
      for (int d = 0; d < hd; ++d) a += double(q[std::size_t(h) * hd + d]) * __half2float(kc[(std::size_t(g) * cap + slot) * hd + d]);
      s.push_back(a * scale);
    }
    double m = *std::max_element(s.begin(), s.end());
    if (sinks) m = std::max(m, double(sinks[h]));
    double l = 0.0;
    for (double v : s) l += std::exp(v - m);
    if (sinks) l += std::exp(double(sinks[h]) - m);
    for (int p = lo; p <= pos; ++p) {
      const int slot = ring ? p % ring : p;
      const double w = std::exp(s[std::size_t(p - lo)] - m) / l;
      for (int d = 0; d < hd; ++d) out[std::size_t(h) * hd + d] += w * __half2float(vc[(std::size_t(g) * cap + slot) * hd + d]);
    }
  }
  return out;
}

std::vector<__half> half_rand(std::size_t n, float lo, float hi) {
  const auto f = randv(n, lo, hi);
  std::vector<__half> h(n);
  for (std::size_t i = 0; i < n; ++i) h[i] = __float2half(f[i]);
  return h;
}

void decode_attention_with_sinks_matches_cpu() {
  const int hd = flk::kHead, nh = 32, nkv = 4;
  const float scale = 1.0f / std::sqrt(128.0f);
  const int max_splits = flk::attn_decode_splits(4096);
  float* dscr = nullptr;
  cudaMalloc(&dscr, sizeof(float) * nh * max_splits * (hd + 2));
  float* dout = nullptr;
  cudaMalloc(&dout, sizeof(float) * nh * hd);
  int* dpos = nullptr;
  cudaMalloc(&dpos, sizeof(int));
  struct Case {
    int cap, ring, window, pos;
    bool sinks;
  };
  for (const Case c : {Case{4096, 0, 0, 0, true}, Case{4096, 0, 0, 300, true}, Case{4096, 0, 0, 2999, true}, Case{4096, 0, 0, 300, false},
                       Case{1025, 1025, 513, 100, true}, Case{1025, 1025, 513, 2000, true}, Case{1025, 1025, 513, 2000, false}}) {
    const auto q = randv(std::size_t(nh) * hd, -2.0f, 2.0f);
    const auto kc = half_rand(std::size_t(nkv) * c.cap * hd, -2.0f, 2.0f), vc = half_rand(std::size_t(nkv) * c.cap * hd, -1.0f, 1.0f);
    const auto sinks = randv(nh, -3.0f, 6.0f);
    float *dq = dev(q), *dsk = dev(sinks);
    __half *dk = dev(kc), *dv = dev(vc);
    const float* sk = c.sinks ? dsk : nullptr;
    flk::attn_decode(dq, dk, dv, c.cap, c.ring, c.window, c.pos, sk, scale, dout, dscr, max_splits, nullptr);
    const auto got = host(dout, std::size_t(nh) * hd);
    const std::string tag = "pos " + std::to_string(c.pos) + (c.window ? " sliding" : " full") + (c.sinks ? " with sinks" : " no sinks");
    close(got, cpu_attention(q, kc, vc, c.cap, c.ring, c.window, c.pos, c.sinks ? sinks.data() : nullptr, scale), 1e-4, 1e-5,
          "decode attention, " + tag);
    // the device-position form gives the same bits
    cudaMemcpy(dpos, &c.pos, sizeof(int), cudaMemcpyHostToDevice);
    flk::attn_decode_dev(dq, dk, dv, c.cap, c.ring, c.window, dpos, flk::attn_decode_splits(flk::attn_decode_keys(c.pos, c.window)), sk,
                         scale, dout, dscr, max_splits, nullptr);
    const auto dgot = host(dout, std::size_t(nh) * hd);
    expect(std::memcmp(got.data(), dgot.data(), got.size() * 4) == 0, "the device position gives the same bits, " + tag);
    for (void* x : std::initializer_list<void*>{dq, dsk, dk, dv}) cudaFree(x);
  }
  expect(flk::attn_decode_keys(2000, 513) == 513 && flk::attn_decode_keys(100, 513) == 101 && flk::attn_decode_keys(2000, 0) == 2001,
         "a sliding query reads itself and the 512 keys before it");
  cudaFree(dscr);
  cudaFree(dout);
  cudaFree(dpos);
}

void prompt_softmax_masks_and_sinks_like_the_model() {
  const int G = flk::kGroup, Ts = 5;
  const float scale = 1.0f / std::sqrt(128.0f);
  struct Case {
    int L, qpos0, ring, window, last_pos;
  };
  for (const Case c : {Case{20, 15, 0, 0, 19}, Case{1025, 1400, 1025, 513, 1404}}) {
    const auto s = randv(std::size_t(G) * Ts * c.L, -20.0f, 20.0f), sinks = randv(G, -3.0f, 6.0f);
    float *ds = dev(s), *dsk = dev(sinks);
    __half* dp = nullptr;
    cudaMalloc(&dp, sizeof(__half) * G * Ts * c.L);
    flk::attn_softmax(ds, dp, G, Ts, c.L, c.qpos0, c.ring, c.window, c.last_pos, dsk, scale, nullptr);
    const auto p = host(dp, std::size_t(G) * Ts * c.L);
    std::vector<float> got(p.size());
    std::vector<double> want(p.size(), 0.0);
    for (std::size_t i = 0; i < p.size(); ++i) got[i] = __half2float(p[i]);
    for (int gi = 0; gi < G; ++gi)
      for (int t = 0; t < Ts; ++t) {
        const int qp = c.qpos0 + t;
        const std::size_t row = (std::size_t(gi) * Ts + t) * c.L;
        auto kpos = [&](int j) { return c.ring ? c.last_pos - (((c.last_pos - j) % c.ring) + c.ring) % c.ring : j; };
        auto valid = [&](int j) { const int kp = kpos(j); return kp >= 0 && kp <= qp && (c.window == 0 || qp - kp < c.window); };
        double m = sinks[std::size_t(gi)];
        for (int j = 0; j < c.L; ++j)
          if (valid(j)) m = std::max(m, double(s[row + std::size_t(j)]) * scale);
        double l = std::exp(double(sinks[std::size_t(gi)]) - m);
        for (int j = 0; j < c.L; ++j)
          if (valid(j)) l += std::exp(double(s[row + std::size_t(j)]) * scale - m);
        for (int j = 0; j < c.L; ++j)
          if (valid(j)) want[row + std::size_t(j)] = std::exp(double(s[row + std::size_t(j)]) * scale - m) / l;
      }
    close(got, want, 2e-3, 1e-4, c.ring ? "prompt softmax, sliding ring" : "prompt softmax, full");
    for (void* x : std::initializer_list<void*>{ds, dsk, dp}) cudaFree(x);
  }
}

void router_picks_the_top_six_by_sigmoid() {
  const int T = 5, E = flk::kRouted;
  auto lg = randv(std::size_t(T) * E, -6.0f, 6.0f);
  lg[3] = lg[7] = 9.0f;   // token 0: a tie at the top, the lower id first
  float* dl = dev(lg);
  int *dp = nullptr, *dc = nullptr;
  float* dw = nullptr;
  cudaMalloc(&dp, sizeof(int) * T * 6);
  cudaMalloc(&dw, sizeof(float) * T * 6);
  cudaMalloc(&dc, sizeof(int) * E);
  cudaMemset(dc, 0, sizeof(int) * E);
  flk::router_topk(dl, T, dp, dw, dc, 1.0f, nullptr);
  const auto picks = host(dp, std::size_t(T) * 6);
  const auto w = host(dw, std::size_t(T) * 6);
  const auto counts = host(dc, E);
  std::vector<int> want_counts(E, 0);
  bool ok = true;
  std::vector<double> want_w;
  std::vector<float> got_w;
  for (int t = 0; t < T; ++t) {
    std::vector<int> idx(E);
    std::iota(idx.begin(), idx.end(), 0);
    auto prob = [&](int e) { return 1.0 / (1.0 + std::exp(-double(lg[std::size_t(t) * E + e]))); };
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return prob(a) > prob(b); });
    double sum = 0.0;
    for (int k = 0; k < 6; ++k) sum += prob(idx[std::size_t(k)]);
    for (int k = 0; k < 6; ++k) {
      ok = ok && picks[std::size_t(t) * 6 + k] == idx[std::size_t(k)];
      want_w.push_back(prob(idx[std::size_t(k)]) / sum);
      got_w.push_back(w[std::size_t(t) * 6 + k]);
      ++want_counts[std::size_t(idx[std::size_t(k)])];
    }
  }
  expect(ok, "the six highest sigmoid probabilities, highest first, ties by the lower id");
  close(got_w, want_w, 1e-5, 1e-6, "renormalised weights");
  expect(counts == want_counts, "the routing counts");
  flk::router_topk(dl, T, dp, dw, nullptr, 2.5f, nullptr);
  const auto w2 = host(dw, std::size_t(T) * 6);
  for (auto& x : want_w) x *= 2.5;
  close(w2, want_w, 1e-5, 1e-6, "expert_weights_scale applies after the renormalisation");
  for (void* x : std::initializer_list<void*>{dl, dp, dw, dc}) cudaFree(x);
}

void swiglu_and_combines_match_cpu() {
  const int rows = 4, n = 512, gs = 1024;
  const auto g = randv(std::size_t(rows) * gs, -8.0f, 8.0f), u = randv(std::size_t(rows) * gs, -2.0f, 2.0f);
  float *dg = dev(g), *du = dev(u), *dm = nullptr;
  cudaMalloc(&dm, sizeof(float) * rows * n);
  flk::swiglu(dg, gs, du + n, gs, dm, rows, n, nullptr);   // gate rows then up rows of one product, as an expert's
  std::vector<double> want(std::size_t(rows) * n);
  for (int r = 0; r < rows; ++r)
    for (int i = 0; i < n; ++i) {
      const double a = g[std::size_t(r) * gs + i];
      want[std::size_t(r) * n + i] = a / (1.0 + std::exp(-a)) * u[std::size_t(r) * gs + n + i];
    }
  close(host(dm, want.size()), want, 1e-5, 1e-6, "swiglu");
  const int E = 2048, P = 6, T = 3;
  const auto y = randv(std::size_t(T) * P * E), wts = randv(std::size_t(T) * P, 0.0f, 1.0f);
  float *dy = dev(y), *dw = dev(wts), *dmoe = nullptr;
  cudaMalloc(&dmoe, sizeof(float) * T * E);
  flk::moe_combine(dy, dw, P, E, dmoe, nullptr);
  std::vector<double> want1(E, 0.0);
  for (int k = 0; k < P; ++k)
    for (int i = 0; i < E; ++i) want1[std::size_t(i)] += double(y[std::size_t(k) * E + i]) * wts[std::size_t(k)];
  close(host(dmoe, E), want1, 1e-5, 1e-6, "decode combine");
  std::vector<int> inv(std::size_t(T) * P);
  std::iota(inv.begin(), inv.end(), 0);
  std::shuffle(inv.begin(), inv.end(), rng);
  int* dinv = dev(inv);
  flk::scatter_combine(dy, dinv, dw, T, E, dmoe, nullptr);
  std::vector<double> want2(std::size_t(T) * E, 0.0);
  for (int t = 0; t < T; ++t)
    for (int k = 0; k < P; ++k)
      for (int i = 0; i < E; ++i)
        want2[std::size_t(t) * E + i] += double(y[std::size_t(inv[std::size_t(t) * P + k]) * E + i]) * wts[std::size_t(t) * P + k];
  close(host(dmoe, want2.size()), want2, 1e-5, 1e-6, "prompt combine through the row table");
  for (void* x : std::initializer_list<void*>{dg, du, dm, dy, dw, dmoe, dinv}) cudaFree(x);
}

void argmax_topk_dequant_and_plan_match_cpu() {
  const int n = 150144;
  auto x = randv(n, -10.0f, 10.0f);
  x[777] = x[90001] = 50.0f;   // a tie at the top: the lower index
  float* dx = dev(x);
  int* dout = nullptr;
  void* scratch = nullptr;
  cudaMalloc(&dout, sizeof(int));
  cudaMalloc(&scratch, std::max<std::size_t>(8192, flk::topk_scratch_bytes(n)));
  flk::argmax(dx, n, dout, scratch, nullptr);
  expect(host(dout, 1)[0] == 777, "argmax: the lower index of a tie");
  float* dv = nullptr;
  int *di = nullptr, *dflag = nullptr;
  cudaMalloc(&dv, sizeof(float) * 64);
  cudaMalloc(&di, sizeof(int) * 64);
  cudaMalloc(&dflag, sizeof(int));
  cudaMemset(dflag, 0, sizeof(int));
  flk::topk(dx, n, 20, dv, di, dflag, scratch, nullptr);
  const auto ids = host(di, 20);
  std::vector<int> order(static_cast<std::size_t>(n));
  std::iota(order.begin(), order.end(), 0);
  std::partial_sort(order.begin(), order.begin() + 20, order.end(), [&](int a, int b) {
    return x[std::size_t(a)] > x[std::size_t(b)] || (x[std::size_t(a)] == x[std::size_t(b)] && a < b);
  });
  expect(std::equal(ids.begin(), ids.end(), order.begin()) && host(dflag, 1)[0] == 0, "top-20 ranked like the CPU");
  // dequant of a Q8_0 matrix
  const int rows = 8, cols = 2048;
  const auto w = randv(std::size_t(rows) * cols);
  const Q8 q = quantise(w, rows, cols);
  auto* dq = dev(q.q);
  auto* ds = dev(q.s);
  float* dd = nullptr;
  cudaMalloc(&dd, sizeof(float) * rows * cols);
  flk::dequant({flk::WKind::q8, rows, cols, dq, ds}, dd, nullptr);
  const auto deq = host(dd, std::size_t(rows) * cols);
  expect(std::equal(deq.begin(), deq.end(), q.values.begin()), "Q8_0 dequantisation, exact");
  // a decode step's plan: one group per pick
  const std::vector<int> picks{9, 200, 3, 255, 0, 17};
  std::vector<unsigned long long> tab(256);
  for (int e = 0; e < 256; ++e) tab[std::size_t(e)] = 0x1000ull * (e + 1);
  int* dpk = dev(picks);
  auto* dtab = dev(tab);
  unsigned long long* gp = nullptr;
  int *gs = nullptr, *gn = nullptr, *ed = nullptr, *et = nullptr;
  cudaMalloc(&gp, sizeof(unsigned long long) * 6);
  cudaMalloc(&gs, sizeof(int) * 7);
  cudaMalloc(&gn, sizeof(int));
  cudaMalloc(&ed, sizeof(int) * 6);
  cudaMalloc(&et, sizeof(int) * 6);
  flk::plan_decode(dpk, 6, dtab, gp, gs, gn, ed, et, nullptr);
  const auto hp = host(gp, 6);
  const auto hs = host(gs, 7);
  bool plan_ok = host(gn, 1)[0] == 6 && hs[6] == 6;
  for (int k = 0; k < 6; ++k) plan_ok = plan_ok && hp[std::size_t(k)] == tab[std::size_t(picks[std::size_t(k)])] && hs[std::size_t(k)] == k;
  expect(plan_ok, "the decode plan: group k is pick k's blob");
  for (void* p : std::initializer_list<void*>{dx, dout, scratch, dv, di, dflag, dq, ds, dd, dpk, dtab, gp, gs, gn, ed, et}) cudaFree(p);
}

}  // namespace

int main(int argc, char** argv) {
  return g4_test::run_cases(argc, argv,
                            {
                                {"matvec_matches_cpu_for_each_kind", matvec_matches_cpu_for_each_kind},
                                {"norms_adds_and_embedding_match_cpu", norms_adds_and_embedding_match_cpu},
                                {"qkv_post_follows_the_layer", qkv_post_follows_the_layer},
                                {"decode_attention_with_sinks_matches_cpu", decode_attention_with_sinks_matches_cpu},
                                {"prompt_softmax_masks_and_sinks_like_the_model", prompt_softmax_masks_and_sinks_like_the_model},
                                {"router_picks_the_top_six_by_sigmoid", router_picks_the_top_six_by_sigmoid},
                                {"swiglu_and_combines_match_cpu", swiglu_and_combines_match_cpu},
                                {"argmax_topk_dequant_and_plan_match_cpu", argmax_topk_dequant_and_plan_match_cpu},
                            });
}
