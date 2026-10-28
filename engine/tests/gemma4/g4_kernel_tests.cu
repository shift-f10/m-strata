// strata-gemma4-engine: every kernel against a CPU computation on random data (box only:
// needs the GPU). Plan Task 5.
#include "g4_kernels.cuh"
#include "g4_test.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

using g4_test::expect;

namespace {

std::mt19937 rng(12345);

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

// two buffers with the same bits
template <typename T>
bool same_bits(const std::vector<T>& a, const std::vector<T>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0;
}

bool close(const std::vector<float>& got, const std::vector<float>& want, float rel, float abs_tol, const std::string& what) {
  if (got.size() != want.size()) {
    expect(false, what + ": size");
    return false;
  }
  double worst = 0.0;
  std::size_t at = 0;
  for (std::size_t i = 0; i < got.size(); ++i) {
    const double err = std::fabs(double(got[i]) - double(want[i]));
    const double tol = abs_tol + rel * std::fabs(double(want[i]));
    if (!(err <= tol) && err - tol > worst) {
      worst = err - tol;
      at = i;
    }
  }
  if (worst > 0.0) {
    expect(false, what + ": index " + std::to_string(at) + " got " + std::to_string(got[at]) + " want " + std::to_string(want[at]));
    return false;
  }
  return true;
}

// Q8_0 quantisation into the engine's two planes (and the f32 values it stands for).
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

std::vector<float> cpu_matvec(const std::vector<float>& w, const std::vector<float>& x, int rows, int cols, int T) {
  std::vector<float> y(std::size_t(T) * rows);
  for (int t = 0; t < T; ++t)
    for (int r = 0; r < rows; ++r) {
      double acc = 0;
      for (int c = 0; c < cols; ++c) acc += double(w[std::size_t(r) * cols + c]) * x[std::size_t(t) * cols + c];
      y[std::size_t(t) * rows + r] = float(acc);
    }
  return y;
}

void matvec_matches_cpu_for_each_kind() {
  for (const auto& [rows, cols, T] : std::vector<std::tuple<int, int, int>>{{37, 2816, 1}, {2816, 704, 2}, {50, 2112, 3}, {16, 32, 1}}) {
    const auto w = randv(std::size_t(rows) * cols);
    const auto x = randv(std::size_t(T) * cols);
    float* dx = dev(x);
    float* dy = nullptr;
    cudaMalloc(&dy, std::size_t(T) * rows * 4);
    const std::string shape = std::to_string(rows) + "x" + std::to_string(cols) + " T" + std::to_string(T);
    {
      const Q8 q = quantise(w, rows, cols);
      auto* dq = dev(q.q);
      auto* ds = dev(q.s);
      g4k::matvec({g4k::WKind::q8, rows, cols, dq, ds}, dx, dy, T, nullptr);
      close(host(dy, std::size_t(T) * rows), cpu_matvec(q.values, x, rows, cols, T), 1e-4f, 1e-4f, "q8 matvec " + shape);
      cudaFree(dq);
      cudaFree(ds);
    }
    {
      std::vector<float> values;
      const auto b = to_bf16(w, values);
      auto* db = dev(b);
      g4k::matvec({g4k::WKind::bf16, rows, cols, db, nullptr}, dx, dy, T, nullptr);
      close(host(dy, std::size_t(T) * rows), cpu_matvec(values, x, rows, cols, T), 1e-4f, 1e-4f, "bf16 matvec " + shape);
      cudaFree(db);
    }
    {
      auto* dw = dev(w);
      g4k::matvec({g4k::WKind::f32, rows, cols, dw, nullptr}, dx, dy, T, nullptr);
      close(host(dy, std::size_t(T) * rows), cpu_matvec(w, x, rows, cols, T), 1e-4f, 1e-4f, "f32 matvec " + shape);
      cudaFree(dw);
    }
    cudaFree(dx);
    cudaFree(dy);
  }
}

void expert_matvec_reads_the_table_in_vram_and_pinned_ram() {
  const int rows = 1408, cols = 2816, E = 4;
  std::vector<Q8> experts;
  for (int e = 0; e < E; ++e) experts.push_back(quantise(randv(std::size_t(rows) * cols), rows, cols));
  // experts 0 and 2 in VRAM, 1 and 3 in mapped pinned host memory
  std::vector<g4k::ExpertPtr> table(E);
  std::vector<void*> pinned;
  for (int e = 0; e < E; ++e) {
    if (e % 2 == 0) {
      table[std::size_t(e)].gu_w = dev(experts[std::size_t(e)].q);
      table[std::size_t(e)].gu_s = dev(experts[std::size_t(e)].s);
    } else {
      void* hq = nullptr;
      void* hs = nullptr;
      cudaHostAlloc(&hq, experts[std::size_t(e)].q.size(), cudaHostAllocMapped);
      cudaHostAlloc(&hs, experts[std::size_t(e)].s.size() * 2, cudaHostAllocMapped);
      std::memcpy(hq, experts[std::size_t(e)].q.data(), experts[std::size_t(e)].q.size());
      std::memcpy(hs, experts[std::size_t(e)].s.data(), experts[std::size_t(e)].s.size() * 2);
      void* dq = nullptr;
      void* ds = nullptr;
      cudaHostGetDevicePointer(&dq, hq, 0);
      cudaHostGetDevicePointer(&ds, hs, 0);
      table[std::size_t(e)].gu_w = dq;
      table[std::size_t(e)].gu_s = static_cast<const __half*>(ds);
      pinned.push_back(hq);
      pinned.push_back(hs);
    }
  }
  auto* dtable = dev(table);
  const std::vector<int> picks{2, 1, 3};
  auto* dpicks = dev(picks);
  for (const int stride : {0, cols}) {
    const auto x = randv(std::size_t(stride == 0 ? 1 : 3) * cols);
    float* dx = dev(x);
    float* dy = nullptr;
    cudaMalloc(&dy, 3 * rows * 4);
    g4k::matvec_experts(g4k::WKind::q8, 0, rows, cols, dtable, dpicks, 3, dx, stride, dy, nullptr);
    std::vector<float> want;
    for (int k = 0; k < 3; ++k) {
      const std::vector<float> xk(x.begin() + std::ptrdiff_t(k) * stride, x.begin() + std::ptrdiff_t(k) * stride + cols);
      const auto y = cpu_matvec(experts[std::size_t(picks[std::size_t(k)])].values, xk, rows, cols, 1);
      want.insert(want.end(), y.begin(), y.end());
    }
    close(host(dy, 3 * rows), want, 1e-4f, 1e-4f, "expert matvec, x stride " + std::to_string(stride));
    cudaFree(dx);
    cudaFree(dy);
  }
  for (void* p : pinned) cudaFreeHost(p);
}

std::vector<float> cpu_rms(const std::vector<float>& x, const float* w, const float* w2, float scale, int rows, int n, float eps) {
  std::vector<float> out(x.size());
  for (int r = 0; r < rows; ++r) {
    double ss = 0;
    for (int i = 0; i < n; ++i) ss += double(x[std::size_t(r) * n + i]) * x[std::size_t(r) * n + i];
    const double inv = 1.0 / std::sqrt(ss / n + eps);
    for (int i = 0; i < n; ++i)
      out[std::size_t(r) * n + i] = float(x[std::size_t(r) * n + i] * inv * (w ? w[i] : 1.0f) * scale * (w2 ? w2[i] : 1.0f));
  }
  return out;
}

void norms_match_cpu() {
  const int rows = 3, n = 2816;
  const float eps = 1e-6f;
  const auto x = randv(std::size_t(rows) * n, -3, 3), y = randv(std::size_t(rows) * n, -5, 5), z = randv(std::size_t(rows) * n);
  const auto w = randv(n, 0, 2), w2 = randv(n, 0, 2), w3 = randv(n, 0, 2);
  auto* dx = dev(x);
  auto* dy = dev(y);
  auto* dz = dev(z);
  auto* dw = dev(w);
  auto* dw2 = dev(w2);
  auto* dw3 = dev(w3);
  float* out = nullptr;
  cudaMalloc(&out, x.size() * 4);
  g4k::rmsnorm(dx, dw, nullptr, 1.0f, out, rows, n, eps, nullptr);
  close(host(out, x.size()), cpu_rms(x, w.data(), nullptr, 1.0f, rows, n, eps), 1e-5f, 1e-6f, "rmsnorm with weight");
  const float sc = 1.0f / std::sqrt(2816.0f);
  g4k::rmsnorm(dx, nullptr, dw2, sc, out, rows, n, eps, nullptr);
  close(host(out, x.size()), cpu_rms(x, nullptr, w2.data(), sc, rows, n, eps), 1e-5f, 1e-6f, "router rms (scale, second weight)");
  g4k::add_rmsnorm(dx, dy, dw, out, rows, n, eps, nullptr);
  {
    auto want = cpu_rms(y, w.data(), nullptr, 1.0f, rows, n, eps);
    for (std::size_t i = 0; i < want.size(); ++i) want[i] += x[i];
    close(host(out, x.size()), want, 1e-5f, 1e-5f, "add_rmsnorm");
  }
  const std::vector<float> ls{0.75f};
  auto* dls = dev(ls);
  g4k::ffn_end(dx, dy, dz, dw, dw2, dw3, dls, out, rows, n, eps, nullptr);
  {
    const auto d = cpu_rms(y, w.data(), nullptr, 1.0f, rows, n, eps), m = cpu_rms(z, w2.data(), nullptr, 1.0f, rows, n, eps);
    std::vector<float> s(d.size());
    for (std::size_t i = 0; i < s.size(); ++i) s[i] = d[i] + m[i];
    auto f = cpu_rms(s, w3.data(), nullptr, 1.0f, rows, n, eps);
    for (std::size_t i = 0; i < f.size(); ++i) f[i] = (x[i] + f[i]) * 0.75f;
    close(host(out, x.size()), f, 1e-5f, 1e-5f, "ffn_end");
  }
}

void embedding_and_small_ops_match_cpu() {
  const int n = 2816, V = 40;
  const Q8 table = quantise(randv(std::size_t(V) * n), V, n);
  auto* dq = dev(table.q);
  auto* ds = dev(table.s);
  const std::vector<int> ids{3, 39, 0};
  auto* dids = dev(ids);
  float* out = nullptr;
  cudaMalloc(&out, 3 * n * 4);
  g4k::embed_q8(dids, 3, dq, ds, n, 2.5f, out, nullptr);
  std::vector<float> want;
  for (int id : ids)
    for (int i = 0; i < n; ++i) want.push_back(table.values[std::size_t(id) * n + i] * 2.5f);
  close(host(out, 3 * n), want, 1e-6f, 1e-6f, "embed_q8");

  const auto g = randv(4 * 2 * 704, -4, 4);
  auto* dg = dev(g);
  float* m = nullptr;
  cudaMalloc(&m, 4 * 704 * 4);
  g4k::geglu(dg, 1408, dg + 704, 1408, m, 4, 704, nullptr);
  std::vector<float> wm;
  for (int r = 0; r < 4; ++r)
    for (int i = 0; i < 704; ++i) {
      const float a = g[std::size_t(r) * 1408 + i], u = g[std::size_t(r) * 1408 + 704 + i];
      wm.push_back(0.5f * a * (1.0f + std::tanh(0.79788456080286535588f * a * (1.0f + 0.044715f * a * a))) * u);
    }
  close(host(m, 4 * 704), wm, 1e-5f, 1e-6f, "geglu");

  auto lg = randv(1000, -100, 100);
  auto* dl = dev(lg);
  g4k::softcap(dl, 1000, 30.0f, nullptr);
  std::vector<float> wl(lg.size());
  for (std::size_t i = 0; i < lg.size(); ++i) wl[i] = 30.0f * std::tanh(lg[i] / 30.0f);
  close(host(dl, 1000), wl, 1e-5f, 1e-5f, "softcap");

  std::vector<float> big = randv(262144);
  big[200001] = 5.0f;
  big[3] = 5.0f;  // a tie: the lowest index wins
  auto* db = dev(big);
  int* di = nullptr;
  void* scratch = nullptr;
  cudaMalloc(&di, 4);
  cudaMalloc(&scratch, 2 * 1024 * 8);
  g4k::argmax(db, 262144, di, scratch, nullptr);
  expect(host(di, 1)[0] == 3, "argmax (lowest index on a tie)");
  std::vector<float> bad(1000, std::nanf(""));
  auto* dbad = dev(bad);
  g4k::argmax(dbad, 1000, di, scratch, nullptr);
  expect(host(di, 1)[0] == -2, "argmax reports logits that are all NaN as -2");

  const std::vector<int> picks{5, 1, 7};
  const auto y = randv(3 * n), wts = randv(3, 0, 1), dsc = randv(8, 0.5f, 2.0f);
  auto* dy = dev(y);
  auto* dw = dev(wts);
  auto* dp = dev(picks);
  auto* dd = dev(dsc);
  g4k::moe_combine(dy, dw, dp, dd, 3, n, out, nullptr);
  std::vector<float> wc(n, 0.0f);
  for (int k = 0; k < 3; ++k)
    for (int i = 0; i < n; ++i) wc[std::size_t(i)] += y[std::size_t(k) * n + i] * wts[std::size_t(k)] * dsc[std::size_t(picks[std::size_t(k)])];
  close(host(out, n), wc, 1e-5f, 1e-6f, "moe_combine");
}

void router_picks_the_top_eight() {
  const int T = 3;
  auto logits = randv(std::size_t(T) * 128, -3, 3);
  auto* dl = dev(logits);
  int* picks = nullptr;
  float* w = nullptr;
  int* counts = nullptr;
  cudaMalloc(&picks, T * 8 * 4);
  cudaMalloc(&w, T * 8 * 4);
  cudaMalloc(&counts, 128 * 4);
  cudaMemset(counts, 0, 128 * 4);
  g4k::router_topk(dl, T, picks, w, counts, nullptr);
  const auto hp = host(picks, T * 8);
  const auto hw = host(w, T * 8);
  const auto hc = host(counts, 128);
  int total = 0;
  for (int c : hc) total += c;
  expect(total == T * 8, "counts add one per pick");
  for (int t = 0; t < T; ++t) {
    std::vector<int> idx(128);
    for (int i = 0; i < 128; ++i) idx[std::size_t(i)] = i;
    const float* lt = logits.data() + std::size_t(t) * 128;
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return lt[a] > lt[b]; });
    double mx = lt[idx[0]], sum = 0;
    for (int i = 0; i < 128; ++i) sum += std::exp(lt[i] - mx);
    double top = 0;
    for (int k = 0; k < 8; ++k) top += std::exp(lt[idx[std::size_t(k)]] - mx) / sum;
    for (int k = 0; k < 8; ++k) {
      expect(hp[std::size_t(t) * 8 + k] == idx[std::size_t(k)], "pick " + std::to_string(k) + " of token " + std::to_string(t));
      const double want = std::exp(lt[idx[std::size_t(k)]] - mx) / sum / top;
      expect(std::fabs(hw[std::size_t(t) * 8 + k] - want) < 1e-5, "renormalised weight " + std::to_string(k));
    }
  }
}

// CPU reference of qkv_post for one token and one layer kind.
void cpu_head(std::vector<float>& h, const float* w, int hd, float eps, bool rope, int pos, float base, const float* ff) {
  double ss = 0;
  for (float v : h) ss += double(v) * v;
  const double inv = 1.0 / std::sqrt(ss / hd + eps);
  for (int i = 0; i < hd; ++i) h[std::size_t(i)] = float(h[std::size_t(i)] * inv * (w ? w[i] : 1.0f));
  if (!rope) return;
  for (int i = 0; i < hd / 2; ++i) {
    double theta = double(pos) * std::pow(double(base), -2.0 * i / double(hd));
    if (ff) theta /= double(ff[i]);
    const float c = float(std::cos(theta)), s = float(std::sin(theta));
    const float a = h[std::size_t(i)], b = h[std::size_t(i + hd / 2)];
    h[std::size_t(i)] = a * c - b * s;
    h[std::size_t(i + hd / 2)] = a * s + b * c;
  }
}

void qkv_post_matches_cpu_for_both_layer_kinds() {
  for (const bool full : {true, false}) {
    const int hd = full ? 512 : 256, n_kv = full ? 2 : 8, n_head = 16, T = 3, pos0 = full ? 7 : 2046;
    const int ring = full ? 0 : 2048, cap = full ? 4096 : 2048;
    const float base = full ? 1e6f : 1e4f, eps = 1e-6f;
    const auto q = randv(std::size_t(T) * n_head * hd, -2, 2), k = randv(std::size_t(T) * n_kv * hd, -2, 2),
               v = randv(std::size_t(T) * n_kv * hd, -2, 2);
    const auto qn = randv(hd, 0.5f, 1.5f), kn = randv(hd, 0.5f, 1.5f);
    std::vector<float> ff(256, 1e30f);
    for (int i = 0; i < 64; ++i) ff[std::size_t(i)] = 1.0f;
    auto* dq = dev(q);
    auto* dk = dev(k);
    auto* dv = dev(v);
    auto* dqn = dev(qn);
    auto* dkn = dev(kn);
    auto* dff = dev(ff);
    float* qo = nullptr;
    __half* q16 = nullptr;
    __half* kc = nullptr;
    __half* vc = nullptr;
    cudaMalloc(&qo, q.size() * 4);
    cudaMalloc(&q16, q.size() * 2);
    cudaMalloc(&kc, std::size_t(n_kv) * cap * hd * 2);
    cudaMalloc(&vc, std::size_t(n_kv) * cap * hd * 2);
    g4k::QkvPost p{dq, dk, full ? dk : dv, dqn, dkn, full ? dff : nullptr, base, hd, n_head, n_kv, pos0, T, ring, cap, eps,
                   qo, q16, kc, vc};
    g4k::qkv_post(p, nullptr);
    const auto hq = host(qo, q.size());
    const auto hq16 = host(q16, q.size());
    const auto hk = host(kc, std::size_t(n_kv) * cap * hd);
    const auto hv = host(vc, std::size_t(n_kv) * cap * hd);
    std::vector<float> wq, gq, gq16, wk, gk, wv, gv;
    for (int t = 0; t < T; ++t) {
      const int pos = pos0 + t, slot = ring ? pos % ring : pos;
      for (int h = 0; h < n_head; ++h) {
        std::vector<float> x(q.begin() + (std::ptrdiff_t(t) * n_head + h) * hd, q.begin() + (std::ptrdiff_t(t) * n_head + h + 1) * hd);
        cpu_head(x, qn.data(), hd, eps, true, pos, base, full ? ff.data() : nullptr);
        wq.insert(wq.end(), x.begin(), x.end());
        for (int i = 0; i < hd; ++i) {
          gq.push_back(hq[(std::size_t(t) * n_head + h) * hd + i]);
          gq16.push_back(__half2float(hq16[(std::size_t(t) * n_head + h) * hd + i]));
        }
      }
      for (int h = 0; h < n_kv; ++h) {
        std::vector<float> xk(k.begin() + (std::ptrdiff_t(t) * n_kv + h) * hd, k.begin() + (std::ptrdiff_t(t) * n_kv + h + 1) * hd);
        const std::vector<float>& vs = full ? k : v;
        std::vector<float> xv(vs.begin() + (std::ptrdiff_t(t) * n_kv + h) * hd, vs.begin() + (std::ptrdiff_t(t) * n_kv + h + 1) * hd);
        cpu_head(xk, kn.data(), hd, eps, true, pos, base, full ? ff.data() : nullptr);
        cpu_head(xv, nullptr, hd, eps, false, pos, base, nullptr);
        for (int i = 0; i < hd; ++i) {
          wk.push_back(__half2float(__float2half(xk[std::size_t(i)])));
          wv.push_back(__half2float(__float2half(xv[std::size_t(i)])));
          gk.push_back(__half2float(hk[(std::size_t(h) * cap + slot) * hd + i]));
          gv.push_back(__half2float(hv[(std::size_t(h) * cap + slot) * hd + i]));
        }
      }
    }
    const std::string kind = full ? "full" : "sliding";
    // float RoPE: the angle pos * scale^i is rounded to float (about 2 ulp of it, up to
    // pos * 2.4e-7 rad), so the rotated values may differ by that much times their size
    const float rope_tol = 2.0f * float(pos0 + T) * 2.4e-7f * 2.5f;
    close(gq, wq, 2e-4f, 2e-4f + rope_tol, kind + " q");
    close(gq16, wq, 2e-3f, 2e-3f + rope_tol, kind + " q16");
    close(gk, wk, 2e-3f, 2e-3f + rope_tol, kind + " k cache");
    close(gv, wv, 2e-3f, 2e-3f, kind + " v cache");
  }
}

// CPU attention for one query over cached K/V (f16 values), scale 1.
std::vector<float> cpu_attn(const std::vector<float>& q, const std::vector<__half>& kc, const std::vector<__half>& vc, int hd,
                            int n_head, int n_kv, int cap, int ring, int window, int pos) {
  std::vector<float> out(std::size_t(n_head) * hd);
  const int G = n_head / n_kv;
  const int lo = window > 0 ? std::max(0, pos - window + 1) : 0;
  for (int h = 0; h < n_head; ++h) {
    const int g = h / G;
    std::vector<double> sc;
    double mx = -1e300;
    for (int p = lo; p <= pos; ++p) {
      const int slot = ring ? p % ring : p;
      double s = 0;
      for (int i = 0; i < hd; ++i) s += double(q[std::size_t(h) * hd + i]) * __half2float(kc[(std::size_t(g) * cap + slot) * hd + i]);
      sc.push_back(s);
      mx = std::max(mx, s);
    }
    double sum = 0;
    for (double& s : sc) sum += (s = std::exp(s - mx));
    for (int i = 0; i < hd; ++i) {
      double acc = 0;
      for (int p = lo; p <= pos; ++p) {
        const int slot = ring ? p % ring : p;
        acc += sc[std::size_t(p - lo)] * __half2float(vc[(std::size_t(g) * cap + slot) * hd + i]);
      }
      out[std::size_t(h) * hd + i] = float(acc / sum);
    }
  }
  return out;
}

void decode_attention_matches_cpu() {
  struct Case { int hd, n_kv, cap, ring, window, pos; };
  for (const Case c : {Case{512, 2, 4096, 0, 0, 3000}, Case{512, 2, 4096, 0, 0, 0}, Case{256, 8, 2048, 2048, 1024, 2500},
                       Case{256, 8, 2048, 2048, 1024, 100}}) {
    const int n_head = 16;
    std::vector<__half> kc(std::size_t(c.n_kv) * c.cap * c.hd), vc(kc.size());
    const auto kf = randv(kc.size(), -1, 1), vf = randv(vc.size(), -1, 1);
    for (std::size_t i = 0; i < kc.size(); ++i) {
      kc[i] = __float2half(kf[i] * 3.0f);
      vc[i] = __float2half(vf[i]);
    }
    const auto q = randv(std::size_t(n_head) * c.hd, -1, 1);
    auto* dq = dev(q);
    auto* dk = dev(kc);
    auto* dv = dev(vc);
    float* out = nullptr;
    float* scratch = nullptr;
    const int splits = 64;
    cudaMalloc(&out, q.size() * 4);
    cudaMalloc(&scratch, std::size_t(n_head) * splits * (c.hd + 2) * 4);
    g4k::attn_decode(dq, dk, dv, c.hd, n_head, c.n_kv, c.cap, c.ring, c.window, c.pos, out, scratch, splits, nullptr);
    close(host(out, q.size()), cpu_attn(q, kc, vc, c.hd, n_head, c.n_kv, c.cap, c.ring, c.window, c.pos), 1e-4f, 1e-4f,
          "decode attention hd " + std::to_string(c.hd) + " pos " + std::to_string(c.pos));
    cudaFree(dq);
    cudaFree(dk);
    cudaFree(dv);
    cudaFree(out);
    cudaFree(scratch);
  }
}

// A captured decode graph replays its kernel arguments, so the step's position comes from device memory: the same
// QKV rows and the same attention, bit for bit, as with the position passed in.
void decode_kernels_read_the_position_from_the_device() {
  int* dpos = nullptr;
  cudaMalloc(&dpos, sizeof(int));
  struct Case { int hd, n_kv, cap, ring, window, pos; };
  for (const Case c : {Case{512, 2, 4096, 0, 0, 3000}, Case{512, 2, 4096, 0, 0, 0}, Case{256, 8, 2048, 2048, 1024, 2500},
                       Case{256, 8, 2048, 2048, 1024, 100}}) {
    const int n_head = 16;
    const std::string what = "hd " + std::to_string(c.hd) + " pos " + std::to_string(c.pos);
    cudaMemcpy(dpos, &c.pos, sizeof(int), cudaMemcpyHostToDevice);
    // QKV post: one row at the position
    const bool full = c.ring == 0;
    const auto q = randv(std::size_t(n_head) * c.hd, -2, 2), k = randv(std::size_t(c.n_kv) * c.hd, -2, 2),
               v = randv(std::size_t(c.n_kv) * c.hd, -2, 2), qn = randv(c.hd, 0.5f, 1.5f), kn = randv(c.hd, 0.5f, 1.5f);
    auto* dq = dev(q);
    auto* dk = dev(k);
    auto* dv = dev(v);
    auto* dqn = dev(qn);
    auto* dkn = dev(kn);
    float *qo_a = nullptr, *qo_b = nullptr;
    __half *kc_a = nullptr, *vc_a = nullptr, *kc_b = nullptr, *vc_b = nullptr;
    const std::size_t cache = std::size_t(c.n_kv) * c.cap * c.hd;
    cudaMalloc(&qo_a, q.size() * 4);
    cudaMalloc(&qo_b, q.size() * 4);
    for (__half** p : {&kc_a, &vc_a, &kc_b, &vc_b}) {
      cudaMalloc(p, cache * 2);
      cudaMemset(*p, 0, cache * 2);
    }
    g4k::QkvPost pa{dq, dk, full ? dk : dv, dqn, dkn, nullptr, full ? 1e6f : 1e4f, c.hd, n_head, c.n_kv, c.pos, 1, c.ring, c.cap,
                    1e-6f, qo_a, nullptr, kc_a, vc_a};
    g4k::QkvPost pb = pa;
    pb.pos0 = -12345;  // ignored: the device word is the position
    pb.d_pos = dpos;
    pb.q_out = qo_b;
    pb.k_cache = kc_b;
    pb.v_cache = vc_b;
    g4k::qkv_post(pa, nullptr);
    g4k::qkv_post(pb, nullptr);
    expect(same_bits(host(qo_a, q.size()), host(qo_b, q.size())) && same_bits(host(kc_a, cache), host(kc_b, cache)) &&
               same_bits(host(vc_a, cache), host(vc_b, cache)),
           "qkv_post with the device position, " + what);
    // decode attention over a cache of random keys
    std::vector<__half> hk(cache), hv(cache);
    const auto kf = randv(cache, -1, 1), vf = randv(cache, -1, 1);
    for (std::size_t i = 0; i < cache; ++i) {
      hk[i] = __float2half(kf[i] * 3.0f);
      hv[i] = __float2half(vf[i]);
    }
    auto* dkc = dev(hk);
    auto* dvc = dev(hv);
    float *out_a = nullptr, *out_b = nullptr, *scratch = nullptr;
    const int splits = 64;
    cudaMalloc(&out_a, q.size() * 4);
    cudaMalloc(&out_b, q.size() * 4);
    cudaMalloc(&scratch, std::size_t(n_head) * splits * (c.hd + 2) * 4);
    g4k::attn_decode(dq, dkc, dvc, c.hd, n_head, c.n_kv, c.cap, c.ring, c.window, c.pos, out_a, scratch, splits, nullptr);
    const int n_split = g4k::attn_decode_splits(g4k::attn_decode_keys(c.pos, c.window));
    g4k::attn_decode_dev(dq, dkc, dvc, c.hd, n_head, c.n_kv, c.cap, c.ring, c.window, dpos, n_split, out_b, scratch, splits, nullptr);
    expect(same_bits(host(out_a, q.size()), host(out_b, q.size())), "decode attention with the device position, " + what);
    for (void* p : std::initializer_list<void*>{dq, dk, dv, dqn, dkn, qo_a, qo_b, kc_a, vc_a, kc_b, vc_b, dkc, dvc, out_a, out_b, scratch})
      cudaFree(p);
  }
  cudaFree(dpos);
}

void prompt_softmax_masks_like_the_model() {
  struct Case { int G, Ts, L, qpos0, ring, window, last_pos; };
  for (const Case c : {Case{8, 5, 37, 30, 0, 0, 34}, Case{2, 4, 64, 100, 64, 16, 103}}) {
    const auto s = randv(std::size_t(c.G) * c.Ts * c.L, -10, 10);
    auto* ds = dev(s);
    __half* dp = nullptr;
    cudaMalloc(&dp, s.size() * 2);
    g4k::attn_softmax(ds, dp, c.G, c.Ts, c.L, c.qpos0, c.ring, c.window, c.last_pos, nullptr);
    const auto hp = host(dp, s.size());
    std::vector<float> got, want;
    for (int g = 0; g < c.G; ++g)
      for (int t = 0; t < c.Ts; ++t) {
        const int qp = c.qpos0 + t;
        std::vector<double> e(std::size_t(c.L), 0.0);
        double mx = -1e300, sum = 0;
        auto valid = [&](int slot) {
          int kp = slot;
          if (c.ring) kp = c.last_pos - (((c.last_pos - slot) % c.ring) + c.ring) % c.ring;
          return kp >= 0 && kp <= qp && (c.window == 0 || qp - kp < c.window);
        };
        for (int j = 0; j < c.L; ++j)
          if (valid(j)) mx = std::max(mx, double(s[(std::size_t(g) * c.Ts + t) * c.L + j]));
        for (int j = 0; j < c.L; ++j)
          if (valid(j)) sum += (e[std::size_t(j)] = std::exp(s[(std::size_t(g) * c.Ts + t) * c.L + j] - mx));
        for (int j = 0; j < c.L; ++j) {
          want.push_back(float(e[std::size_t(j)] / sum));
          got.push_back(__half2float(hp[(std::size_t(g) * c.Ts + t) * c.L + j]));
        }
      }
    close(got, want, 2e-3f, 1e-4f, "prompt softmax G" + std::to_string(c.G));
  }
}

void dequant_and_gather_match_cpu() {
  const int rows = 40, cols = 704;
  const auto w = randv(std::size_t(rows) * cols);
  const Q8 q = quantise(w, rows, cols);
  auto* dq = dev(q.q);
  auto* ds = dev(q.s);
  float* out = nullptr;
  cudaMalloc(&out, w.size() * 4);
  g4k::dequant({g4k::WKind::q8, rows, cols, dq, ds}, out, nullptr);
  close(host(out, w.size()), q.values, 0, 0, "q8 dequant");
  std::vector<float> bv;
  const auto b = to_bf16(w, bv);
  auto* db = dev(b);
  g4k::dequant({g4k::WKind::bf16, rows, cols, db, nullptr}, out, nullptr);
  close(host(out, w.size()), bv, 0, 0, "bf16 dequant");
  std::vector<g4k::ExpertPtr> table(2);
  table[1].dn_w = dq;
  table[1].dn_s = ds;
  auto* dt = dev(table);
  g4k::dequant_expert(g4k::WKind::q8, 1, rows, cols, dt, 1, out, nullptr);
  close(host(out, w.size()), q.values, 0, 0, "expert dequant (down)");

  const int n = 2816, T = 2;
  const auto x = randv(std::size_t(T) * n);
  auto* dx = dev(x);
  const std::vector<int> order{9, 0, 15, 3};  // assignments t*8+k: tokens 1, 0, 1, 0
  auto* dor = dev(order);
  float* g = nullptr;
  cudaMalloc(&g, 4 * n * 4);
  g4k::gather_rows(dx, dor, 4, n, g, nullptr);
  std::vector<float> wg;
  for (int a : order) wg.insert(wg.end(), x.begin() + std::ptrdiff_t(a / 8) * n, x.begin() + std::ptrdiff_t(a / 8 + 1) * n);
  close(host(g, 4 * n), wg, 0, 0, "gather_rows");

  // scatter_combine over T=2 tokens with 8 picks each: y rows in some order
  const auto y = randv(16 * std::size_t(n));
  std::vector<int> inv(16), picks(16);
  for (int i = 0; i < 16; ++i) {
    inv[std::size_t(i)] = 15 - i;
    picks[std::size_t(i)] = i % 5;
  }
  const auto wts = randv(16, 0, 1), dsc = randv(5, 0.5f, 2.0f);
  auto* dy = dev(y);
  auto* dinv = dev(inv);
  auto* dw = dev(wts);
  auto* dp = dev(picks);
  auto* dd = dev(dsc);
  float* moe = nullptr;
  cudaMalloc(&moe, std::size_t(T) * n * 4);
  g4k::scatter_combine(dy, dinv, dw, dp, dd, T, n, moe, nullptr);
  std::vector<float> wm(std::size_t(T) * n, 0.0f);
  for (int t = 0; t < T; ++t)
    for (int k = 0; k < 8; ++k) {
      const int a = t * 8 + k;
      for (int i = 0; i < n; ++i)
        wm[std::size_t(t) * n + i] += y[std::size_t(inv[std::size_t(a)]) * n + i] * wts[std::size_t(a)] * dsc[std::size_t(picks[std::size_t(a)])];
    }
  close(host(moe, std::size_t(T) * n), wm, 1e-5f, 1e-6f, "scatter_combine");
}

// The GPU's top-k (the sampler's candidates) against a CPU ranking: highest first, ties by
// the lower index, for k 1, 20 and 64, over the vocabulary's size and over a short row; a
// non-finite value raises the flag.
void topk_matches_a_cpu_ranking() {
  for (const int n : {262144, 1000, 64, 70}) {
    auto x = randv(std::size_t(n), -30, 30);
    if (n > 100) {
      x[std::size_t(n) - 1] = 29.9999f;
      x[17] = 29.9999f;  // a tie: id 17 before id n-1
      x[std::size_t(n) / 2] = 31.0f;
    }
    auto* dx = dev(x);
    float* vals = nullptr;
    int* ids = nullptr;
    int* flag = nullptr;
    void* scratch = nullptr;
    cudaMalloc(&vals, 64 * 4);
    cudaMalloc(&ids, 64 * 4);
    cudaMalloc(&flag, 4);
    cudaMemset(flag, 0, 4);
    cudaMalloc(&scratch, std::max<std::size_t>(16, g4k::topk_scratch_bytes(n)));
    std::vector<int> order(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) order[std::size_t(i)] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
      return x[std::size_t(a)] > x[std::size_t(b)] || (x[std::size_t(a)] == x[std::size_t(b)] && a < b);
    });
    for (const int k : {1, 20, 64}) {
      g4k::topk(dx, n, k, vals, ids, flag, scratch, nullptr);
      const auto hv = host(vals, std::size_t(k));
      const auto hi = host(ids, std::size_t(k));
      bool same = true;
      for (int j = 0; j < k; ++j) same = same && hi[std::size_t(j)] == order[std::size_t(j)] && hv[std::size_t(j)] == x[std::size_t(order[std::size_t(j)])];
      expect(same, "top-" + std::to_string(k) + " of " + std::to_string(n) + " equals the CPU ranking");
    }
    expect(host(flag, 1)[0] == 0, "finite logits leave the flag clear");
    x[std::size_t(n) / 3] = std::nanf("");
    cudaMemcpy(dx, x.data(), x.size() * 4, cudaMemcpyHostToDevice);
    g4k::topk(dx, n, 8, vals, ids, flag, scratch, nullptr);
    expect(host(flag, 1)[0] == 1, "a NaN raises the non-finite flag (n " + std::to_string(n) + ")");
    cudaFree(dx);
    cudaFree(vals);
    cudaFree(ids);
    cudaFree(flag);
    cudaFree(scratch);
  }
}

}  // namespace

int main(int argc, char** argv) {
  int devices = 0;
  if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) {
    std::fprintf(stderr, "no CUDA device: the kernel tests need the GPU\n");
    return 2;
  }
  return g4_test::run_cases(argc, argv, {
      {"matvec_matches_cpu_for_each_kind", matvec_matches_cpu_for_each_kind},
      {"expert_matvec_reads_the_table_in_vram_and_pinned_ram", expert_matvec_reads_the_table_in_vram_and_pinned_ram},
      {"norms_match_cpu", norms_match_cpu},
      {"embedding_and_small_ops_match_cpu", embedding_and_small_ops_match_cpu},
      {"router_picks_the_top_eight", router_picks_the_top_eight},
      {"qkv_post_matches_cpu_for_both_layer_kinds", qkv_post_matches_cpu_for_both_layer_kinds},
      {"decode_attention_matches_cpu", decode_attention_matches_cpu},
      {"decode_kernels_read_the_position_from_the_device", decode_kernels_read_the_position_from_the_device},
      {"prompt_softmax_masks_like_the_model", prompt_softmax_masks_like_the_model},
      {"dequant_and_gather_match_cpu", dequant_and_gather_match_cpu},
      {"topk_matches_a_cpu_ranking", topk_matches_a_cpu_ranking},
  });
}
