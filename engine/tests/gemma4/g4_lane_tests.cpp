// Gemma 4's CPU lane on the engine's shared expert code (design 2026-10-26-gemma4-paper-tiering-design.md, phase 3):
// the native expert rows, the CPU expert pool and the verify window's dispatcher at Gemma 4's geometry (n_embd 2816,
// n_ff 704, Q8_0 or BF16 experts, GEGLU), against references computed here from the blob's own numbers.  No GPU and no
// model file: synthetic experts in the GGUF block formats.
//
//   g4_lane_tests --case NAME
#include "g4_lane.hpp"
#include "strata/core/expert_source.hpp"
#include "strata/kernels/cpu/expert_layout.hpp"
#include "strata/kernels/cpu/native_expert.hpp"
#include "strata/kernels/cpu/pool.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace C = strata::kernels::cpu;

namespace {

constexpr int kEmbd = 2816, kFf = 704;
constexpr int kQ8_0 = 8, kBf16 = 30;

int g_fail = 0;
void check(bool ok, const std::string& what) {
  std::printf("  %-90s %s\n", what.c_str(), ok ? "ok" : "FAIL");
  if (!ok) ++g_fail;
}

float half_to_float(std::uint16_t h) {
  const std::uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
  float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(static_cast<float>(m | 1024u), static_cast<int>(e) - 25);
  return s ? -v : v;
}
std::uint16_t float_to_half(float f) {  // normal positive scales only (the test's block scales)
  int e = 0;
  const float m = std::frexp(f, &e);  // f = m * 2^e, m in [0.5, 1)
  const std::uint32_t mant = static_cast<std::uint32_t>(std::lround((m * 2.0f - 1.0f) * 1024.0f)) & 1023u;
  return static_cast<std::uint16_t>((static_cast<std::uint32_t>(e - 1 + 15) << 10) | mant);
}
float bf16_to_float(std::uint16_t b) {
  const std::uint32_t u = static_cast<std::uint32_t>(b) << 16;
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
std::uint16_t float_to_bf16(float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, 4);
  return static_cast<std::uint16_t>(u >> 16);  // truncation: any bf16 value will do for a weight
}

// One row of `cols` weights in `type` from the generator, and its values.
void make_row(int type, int cols, std::mt19937& rng, std::uint8_t* dst, float* vals) {
  std::uniform_int_distribution<int> q(-127, 127);
  std::uniform_real_distribution<float> u(-0.06f, 0.06f);
  if (type == kQ8_0) {
    for (int b = 0; b < cols / 32; ++b) {
      const std::uint16_t d = float_to_half(0.0003f + 0.0002f * static_cast<float>(b % 5));
      std::memcpy(dst + b * 34, &d, 2);
      for (int i = 0; i < 32; ++i) {
        const auto v = static_cast<std::int8_t>(q(rng));
        dst[b * 34 + 2 + i] = static_cast<std::uint8_t>(v);
        vals[b * 32 + i] = half_to_float(d) * static_cast<float>(v);
      }
    }
  } else {
    for (int i = 0; i < cols; ++i) {
      const std::uint16_t w = float_to_bf16(u(rng));
      std::memcpy(dst + 2 * i, &w, 2);
      vals[i] = bf16_to_float(w);
    }
  }
}

// A Gemma 4 expert blob [gate | up | down] in the layer's formats, and its weights as floats.
struct Expert {
  std::vector<std::uint8_t> blob;
  std::vector<float> gate, up, down;  // [kFf][kEmbd], [kFf][kEmbd], [kEmbd][kFf]
};
Expert make_expert(const C::NativeFmt& f, std::uint32_t seed) {
  std::mt19937 rng(seed);
  Expert x;
  x.blob.assign(f.bytes, 0);
  x.gate.resize(static_cast<std::size_t>(kFf) * kEmbd);
  x.up.resize(x.gate.size());
  x.down.resize(static_cast<std::size_t>(kEmbd) * kFf);
  for (int r = 0; r < kFf; ++r) make_row(f.gu_type, kEmbd, rng, x.blob.data() + r * f.gu_row, x.gate.data() + static_cast<std::size_t>(r) * kEmbd);
  for (int r = 0; r < kFf; ++r)
    make_row(f.gu_type, kEmbd, rng, x.blob.data() + f.up_off + r * f.gu_row, x.up.data() + static_cast<std::size_t>(r) * kEmbd);
  for (int r = 0; r < kEmbd; ++r)
    make_row(f.d_type, kFf, rng, x.blob.data() + f.down_off + r * f.d_row, x.down.data() + static_cast<std::size_t>(r) * kFf);
  return x;
}

std::vector<float> make_x(std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  std::vector<float> x(kEmbd);
  for (float& v : x) v = u(rng);
  return x;
}

// The activation the CPU dots read (native_quant_act's bytes), back as floats.
std::vector<float> act_values(const C::NativeFmt& f, const std::uint8_t* q, int n) {
  std::vector<float> v(static_cast<std::size_t>(n));
  if (f.gu_act == kQ8_0) {
    for (int b = 0; b < n / 32; ++b) {
      std::uint16_t d;
      std::memcpy(&d, q + b * 34, 2);
      for (int i = 0; i < 32; ++i) v[static_cast<std::size_t>(b * 32 + i)] = half_to_float(d) * static_cast<float>(static_cast<std::int8_t>(q[b * 34 + 2 + i]));
    }
  } else {
    for (int i = 0; i < n; ++i) {
      std::uint16_t w;
      std::memcpy(&w, q + 2 * i, 2);
      v[static_cast<std::size_t>(i)] = bf16_to_float(w);
    }
  }
  return v;
}

double gelu_tanh(double g) { return 0.5 * g * (1.0 + std::tanh(0.7978845608028654 * g * (1.0 + 0.044715 * g * g))); }
double silu(double g) { return g / (1.0 + std::exp(-g)); }

C::NativeFmt gemma_fmt(int gu, int dn, int act, std::string& err) {
  C::NativeFmt f;
  if (!C::native_fmt(gu, dn, kEmbd, kFf, f, err)) return {};
  f.act = act;
  return f;
}

// ff[t][r] for one expert, the pool's way and the reference's.
bool rows_match_reference(int gu, int dn, int act, double* worst) {
  std::string err;
  const C::NativeFmt f = gemma_fmt(gu, dn, act, err);
  if (f.bytes == 0) {
    std::printf("    native_fmt: %s\n", err.c_str());
    return false;
  }
  const Expert e = make_expert(f, 7u + static_cast<std::uint32_t>(gu));
  const std::vector<float> x = make_x(11);
  std::vector<std::uint8_t> qa(C::kNativeActBytes);
  C::native_quant_act(f, x.data(), qa.data());
  const std::vector<float> xa = act_values(f, qa.data(), kEmbd);
  std::vector<float> ff(kFf);
  const void* acts[1] = {qa.data()};
  float* outs[1] = {ff.data()};
  C::native_gu_rows(f, e.blob.data(), acts, 1, outs, 0, kFf);
  double big = 0, err_max = 0;
  for (int r = 0; r < kFf; ++r) {
    double g = 0, u = 0;
    for (int i = 0; i < kEmbd; ++i) {
      g += static_cast<double>(e.gate[static_cast<std::size_t>(r) * kEmbd + i]) * xa[static_cast<std::size_t>(i)];
      u += static_cast<double>(e.up[static_cast<std::size_t>(r) * kEmbd + i]) * xa[static_cast<std::size_t>(i)];
    }
    const double want = (act == 1 ? gelu_tanh(g) : silu(g)) * u;
    big = std::max(big, std::fabs(want));
    err_max = std::max(err_max, std::fabs(want - static_cast<double>(ff[static_cast<std::size_t>(r)])));
  }
  *worst = big > 0 ? err_max / big : err_max;
  return *worst < 1e-4;
}

// --- the cases

void native_fmt_takes_the_gemma4_geometry() {
  std::string err;
  C::NativeFmt q, b, m;
  const bool okq = C::native_fmt(kQ8_0, kQ8_0, kEmbd, kFf, q, err);
  const std::size_t gu_row = kEmbd / 32 * 34, d_row = kFf / 32 * 34;
  check(okq && q.up_off == kFf * gu_row && q.down_off == 2 * q.up_off && q.bytes == q.down_off + kEmbd * d_row,
        "Q8_0 experts: [gate 704 | up 704 | down 2816] rows of the GGUF's blocks" + (okq ? "" : ": " + err));
  const bool okb = C::native_fmt(kBf16, kBf16, kEmbd, kFf, b, err);
  check(okb && b.act_bytes == std::size_t{2} * kEmbd && b.h_bytes == std::size_t{2} * kFf,
        "BF16 experts (the Q8_K_XL file's layer 1): a 5632-byte activation fits" + (okb ? "" : ": " + err));
  const bool okm = C::native_fmt(kQ8_0, kBf16, kEmbd, kFf, m, err);
  check(okm && m.d_act == kBf16, "Q8_0 gate/up with a BF16 down (layer 29)" + (okm ? "" : ": " + err));
  C::NativeFmt wide;
  check(!C::native_fmt(kQ8_0, kQ8_0, kEmbd, C::kNativeMaxFF + 32, wide, err),
        "an n_ff wider than the pool's rows is refused");
}

void geglu_rows_match_the_reference() {
  const struct {
    int type, act;
    const char* what;
  } runs[] = {{kQ8_0, 1, "Q8_0: gelu_tanh(gate) * up"}, {kBf16, 1, "BF16: gelu_tanh(gate) * up"}, {kQ8_0, 0, "act 0 stays SwiGLU"}};
  for (const auto& r : runs) {
    double w = -1;
    const bool ok = rows_match_reference(r.type, r.type, r.act, &w);   // before the message: it reports this run's error
    check(ok, std::string(r.what) + " (rel. error " + std::to_string(w) + ")");
  }
}

// The sequential reference of a pool job: ggml-cpu's rows, one call each, in the pool's order of phases.
std::vector<float> one_expert(const C::NativeFmt& f, const std::uint8_t* blob, const std::vector<std::vector<std::uint8_t>>& acts) {
  const int nt = static_cast<int>(acts.size());
  std::vector<std::vector<float>> ff(static_cast<std::size_t>(nt), std::vector<float>(kFf));
  const void* a[C::MAXT];
  float* o[C::MAXT];
  for (int t = 0; t < nt; ++t) {
    a[t] = acts[static_cast<std::size_t>(t)].data();
    o[t] = ff[static_cast<std::size_t>(t)].data();
  }
  C::native_gu_rows(f, blob, a, nt, o, 0, kFf);
  std::vector<std::vector<std::uint8_t>> hq(static_cast<std::size_t>(nt), std::vector<std::uint8_t>(C::kNativeHBytes));
  const void* h[C::MAXT];
  for (int t = 0; t < nt; ++t) {
    C::native_quant_h(f, ff[static_cast<std::size_t>(t)].data(), hq[static_cast<std::size_t>(t)].data());
    h[t] = hq[static_cast<std::size_t>(t)].data();
  }
  std::vector<float> out(static_cast<std::size_t>(nt) * kEmbd);
  for (int t = 0; t < nt; ++t) o[t] = out.data() + static_cast<std::size_t>(t) * kEmbd;
  C::native_down_rows(f, blob, h, nt, o, 0, kEmbd);
  return out;
}

void pool_runs_gemma4_experts() {
  std::string err;
  for (int gu : {kQ8_0, kBf16}) {
    const C::NativeFmt f = gemma_fmt(gu, gu, 1, err);
    if (f.bytes == 0) {
      check(false, "native_fmt: " + err);
      continue;
    }
    const Expert e0 = make_expert(f, 21), e1 = make_expert(f, 22);
    std::vector<std::vector<std::uint8_t>> acts;
    for (int t = 0; t < 3; ++t) {
      acts.emplace_back(C::kNativeActBytes);
      const std::vector<float> x = make_x(31u + static_cast<std::uint32_t>(t));
      C::native_quant_act(f, x.data(), acts.back().data());
    }
    const std::vector<float> want0 = one_expert(f, e0.blob.data(), acts), want1 = one_expert(f, e1.blob.data(), {acts[1]});
    std::vector<float> got0(3 * static_cast<std::size_t>(kEmbd), -1.0f), got1(kEmbd, -1.0f);
    C::ExpertJobMulti jobs[2];
    jobs[0].blob = e0.blob.data();
    jobs[0].nt = 3;
    for (int t = 0; t < 3; ++t) {
      jobs[0].nact[t] = acts[static_cast<std::size_t>(t)].data();
      jobs[0].out[t] = got0.data() + static_cast<std::size_t>(t) * kEmbd;
    }
    jobs[1].blob = e1.blob.data();
    jobs[1].nt = 1;
    jobs[1].nact[0] = acts[1].data();
    jobs[1].out[0] = got1.data();
    C::ExpertPool pool(3, /*pin=*/false);
    pool.run_split_multi_native(f, jobs, 2);
    const bool same = std::memcmp(got0.data(), want0.data(), got0.size() * 4) == 0 &&
                      std::memcmp(got1.data(), want1.data(), got1.size() * 4) == 0;
    check(same, std::string(gu == kQ8_0 ? "Q8_0" : "BF16") +
                    ": the pool's rows (704 wide, split over 4 threads) are the sequential rows bit for bit");
  }
}

// An in-memory arena: layer 0's experts back to back.
class MemorySource final : public strata::core::ExpertSource {
 public:
  MemorySource(const std::uint8_t* base, std::size_t bytes) : base_(base), bytes_(bytes) {}
  const std::uint8_t* blob(std::int64_t layer, std::int64_t expert) override {
    return layer == 0 ? base_ + bytes_ * static_cast<std::size_t>(expert) : nullptr;
  }

 private:
  const std::uint8_t* base_;
  std::size_t bytes_;
};

void window_dispatch_uses_the_layers_width() {
  std::string err;
  const C::NativeFmt f = gemma_fmt(kQ8_0, kQ8_0, 1, err);
  if (f.bytes == 0) {
    check(false, "native_fmt: " + err);
    return;
  }
  constexpr int kNe = 4, kK = 2;
  std::vector<std::uint8_t> arena(f.bytes * kNe);
  std::vector<Expert> ex;
  for (int e = 0; e < kNe; ++e) {
    ex.push_back(make_expert(f, 41u + static_cast<std::uint32_t>(e)));
    std::memcpy(arena.data() + f.bytes * static_cast<std::size_t>(e), ex.back().blob.data(), f.bytes);
  }
  C::ExpertLayout lay;
  lay.native = true;
  lay.n_layers = 1;
  lay.n_expert = kNe;
  lay.fmt = {f};
  lay.offset = {0};
  lay.bytes = {f.bytes};
  lay.max_blob = f.bytes;
  lay.total = arena.size();
  C::expert_layout_set(lay);
  MemorySource src(arena.data(), f.bytes);
  C::ExpertPool pool(3, /*pin=*/false);
  std::vector<std::int32_t> res = {-1, -1, 0, -1};  // expert 2 is "in VRAM": its row is the GPU's (zero here)
  strata::core::ExpertDispatch d;
  d.pool = &pool;
  d.src = &src;
  d.n_expert = kNe;
  d.host_res = res.data();
  const std::vector<float> x0 = make_x(51), x1 = make_x(52);
  std::vector<float> x(x0);
  x.insert(x.end(), x1.begin(), x1.end());
  const std::int32_t ids[4] = {3, 2, 1, 3};  // token 0: experts 3, 2; token 1: experts 1, 3
  std::vector<float> out(4 * static_cast<std::size_t>(kEmbd), -1.0f);
  d.layers = 0;
  strata::core::expert_pool_dispatch_multi(d, x.data(), ids, 2, kK, out.data());
  check(!d.failed, std::string("the window dispatches") + (d.failed && d.fail ? ": " + std::string(d.fail) : ""));
  std::vector<std::uint8_t> a0(C::kNativeActBytes), a1(C::kNativeActBytes);
  C::native_quant_act(f, x0.data(), a0.data());
  C::native_quant_act(f, x1.data(), a1.data());
  const std::vector<float> w3 = one_expert(f, ex[3].blob.data(), {a0, a1}), w1 = one_expert(f, ex[1].blob.data(), {a1});
  const std::size_t N = kEmbd;
  const bool row0 = std::memcmp(out.data(), w3.data(), N * 4) == 0;
  bool row1 = true;
  for (std::size_t i = 0; i < N; ++i) row1 = row1 && out[N + i] == 0.0f;
  const bool row2 = std::memcmp(out.data() + 2 * N, w1.data(), N * 4) == 0;
  const bool row3 = std::memcmp(out.data() + 3 * N, w3.data() + N, N * 4) == 0;
  check(row0 && row1 && row2 && row3,
        "rows of 2816: each token's experts on the pool, the resident one's row zero, each at entry * n_embd");
  check(d.multi_misses == 2 && d.cache_hits == 1 && d.multi_entries == 3, "two CPU experts for three entries, one hit");
}

// The lane's GPU plan for a verify window (spec §12): the layout of a window of T tokens holds 8 T entries; at T = 1
// it is the decode step's layout word for word (the captured decode graph is unchanged), and every u64 table sits on
// an 8-byte word, the tables never overlap.
void lane_plan_layout_grows_with_the_window() {
  const g4::LanePlan one = g4::LanePlan::at(g4::LanePlan::kCap);
  check(one.counts == g4::LanePlan::kCounts && one.start == g4::LanePlan::kStart && one.dst == g4::LanePlan::kDst &&
            one.tok == g4::LanePlan::kTok && one.ptr == g4::LanePlan::kPtr && one.ptr2 == g4::LanePlan::kPtr2 &&
            one.start2 == g4::LanePlan::kStart2 && one.words == g4::LanePlan::kWords,
        "one token: the decode step's layout");
  bool ok = true;
  for (int t = 1; t <= 8; ++t) {
    const g4::LanePlan p = g4::LanePlan::at(8 * t);
    const int cap = 8 * t;
    ok = ok && p.cap == cap && p.start >= p.counts + 3 && p.dst >= p.start + cap + 1 && p.tok >= p.dst + cap &&
         p.ptr >= p.tok + cap && p.ptr % 2 == 0 && p.ptr2 >= p.ptr + 2 * cap && p.ptr2 % 2 == 0 && p.start2 >= p.ptr2 + 2 * cap &&
         p.words >= p.start2 + cap + 1 && p.words <= g4::kLanePlanMaxWords;
  }
  check(ok, "windows of 2-8 tokens: 8 T entries, aligned u64 tables, no overlap, within the mapped plan's words");
}

// A verify window's routing as the lane hands it to the dispatcher: T tokens at once, each missed expert computed once
// for all of the tokens routed to it, its rows bitwise the rows of each token computed alone.
void window_dispatch_rows_are_the_single_token_rows() {
  std::string err;
  const C::NativeFmt f = gemma_fmt(kQ8_0, kQ8_0, 1, err);
  if (f.bytes == 0) {
    check(false, "native_fmt: " + err);
    return;
  }
  constexpr int kNe = 6, kK = 2, kT = 3;
  std::vector<std::uint8_t> arena(f.bytes * kNe);
  for (int e = 0; e < kNe; ++e) {
    const Expert x = make_expert(f, 61u + static_cast<std::uint32_t>(e));
    std::memcpy(arena.data() + f.bytes * static_cast<std::size_t>(e), x.blob.data(), f.bytes);
  }
  C::ExpertLayout lay;
  lay.native = true;
  lay.n_layers = 1;
  lay.n_expert = kNe;
  lay.fmt = {f};
  lay.offset = {0};
  lay.bytes = {f.bytes};
  lay.max_blob = f.bytes;
  lay.total = arena.size();
  C::expert_layout_set(lay);
  MemorySource src(arena.data(), f.bytes);
  C::ExpertPool pool(3, /*pin=*/false);
  std::vector<std::int32_t> res = {-1, -1, -1, 0, -1, -1};  // expert 3 in VRAM
  auto dispatch = [&](const float* x, const std::int32_t* ids, int n_tok, float* out) {
    strata::core::ExpertDispatch d;
    d.pool = &pool;
    d.src = &src;
    d.n_expert = kNe;
    d.host_res = res.data();
    d.layers = 0;
    strata::core::expert_pool_dispatch_multi(d, x, ids, n_tok, kK, out);
    return !d.failed;
  };
  std::vector<float> x;
  for (int t = 0; t < kT; ++t) {
    const auto row = make_x(71u + static_cast<std::uint32_t>(t));
    x.insert(x.end(), row.begin(), row.end());
  }
  const std::int32_t ids[kT * kK] = {1, 3, 1, 5, 5, 1};  // expert 1 in all three tokens, 5 in two
  std::vector<float> win(kT * kK * static_cast<std::size_t>(kEmbd), -1.0f), alone(win.size(), -2.0f);
  bool ok = dispatch(x.data(), ids, kT, win.data());
  for (int t = 0; t < kT && ok; ++t)
    ok = dispatch(x.data() + static_cast<std::size_t>(t) * kEmbd, ids + t * kK, 1, alone.data() + static_cast<std::size_t>(t) * kK * kEmbd);
  check(ok, "the window and the single tokens dispatch");
  check(std::memcmp(win.data(), alone.data(), win.size() * 4) == 0, "every entry's row: the window's bits are the single token's");
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::pair<std::string, std::function<void()>>> cases = {
      {"lane_plan_layout_grows_with_the_window", lane_plan_layout_grows_with_the_window},
      {"window_dispatch_rows_are_the_single_token_rows", window_dispatch_rows_are_the_single_token_rows},
      {"native_fmt_takes_the_gemma4_geometry", native_fmt_takes_the_gemma4_geometry},
      {"geglu_rows_match_the_reference", geglu_rows_match_the_reference},
      {"pool_runs_gemma4_experts", pool_runs_gemma4_experts},
      {"window_dispatch_uses_the_layers_width", window_dispatch_uses_the_layers_width},
  };
  const std::string want = argc == 3 && std::string(argv[1]) == "--case" ? argv[2] : "";
  int ran = 0;
  for (const auto& [name, fn] : cases) {
    if (!want.empty() && name != want) continue;
    std::printf("%s\n", name.c_str());
    fn();
    ++ran;
  }
  if (ran == 0) {
    std::printf("no such case: %s\n", want.c_str());
    return 2;
  }
  std::printf(g_fail ? "g4_lane_tests: %d FAILED\n" : "g4_lane_tests: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
