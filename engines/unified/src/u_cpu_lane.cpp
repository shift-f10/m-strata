// strata-unified-engine: the CPU expert lane (see u_cpu_lane.hpp) -- upstream's ExpertPool run_split_multi shape.
#include "u_cpu_lane.hpp"

#include "u_cpu_iq.hpp"    // row_bytes (the packed weight-row size, unchanged by the activation quant)
#include "u_cpu_mmvq.hpp"  // the GPU-matching Q8_1 dots the lane computes with

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define U_LANE_PAUSE() _mm_pause()
#else
#define U_LANE_PAUSE() std::this_thread::yield()
#endif

namespace u::cpu {

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kSpinBeforeSleep = std::chrono::milliseconds(20);  // upstream ExpertPool::kSpinBeforeSleep
constexpr int kGuRowsPerTask = 32, kDownRowsPerTask = 64;
constexpr int kMaxTok = 8;

void pin_to(int cpu) {
#if defined(_WIN32)
  if (cpu >= 0 && cpu < 64) SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cpu);
#else
  (void)cpu;
#endif
}

float silu(float v) { return v / (1.0f + std::exp(-v)); }

}  // namespace

bool LaneLayer::usable() const {
  if (gate == nullptr || up == nullptr || down == nullptr || n_embd <= 0 || n_ff <= 0) return false;
  if (!mmvq_type_supported(gate_type) || !mmvq_type_supported(up_type) || !mmvq_type_supported(down_type))
    return false;
  if (n_embd % 256 != 0 || n_ff % 64 != 0) return false;  // gate/up are 256-elem i-quants; down is 32/64-elem
  return row_bytes(gate_type, n_embd) > 0 && row_bytes(up_type, n_embd) > 0 && row_bytes(down_type, n_ff) > 0;
}

LanePool::LanePool(int n_workers, bool pin) {
  const int hw = (int)std::thread::hardware_concurrency();
  n_ = n_workers > 0 ? n_workers : std::max(0, hw / 2 - 1);  // one per physical core but the caller's
  const bool pin_ok = pin && hw >= 2 * (n_ + 1);
  for (int i = 0; i < n_; ++i)
    threads_.emplace_back([this, i, pin_ok] {
      if (pin_ok) pin_to(2 * (i + 1));  // SMT siblings are adjacent on Windows: one worker per physical core
      worker(i);
    });
}

LanePool::~LanePool() {
  stop_.store(true, std::memory_order_seq_cst);
  {
    std::lock_guard<std::mutex> lk(sleep_mu_);
    sleep_cv_.notify_all();
  }
  for (auto& t : threads_)
    if (t.joinable()) t.join();
}

bool LanePool::claim(std::uint32_t epoch, int& task) {
  std::uint64_t h = head_.load(std::memory_order_acquire);
  for (;;) {
    if ((std::uint32_t)(h >> 32) != epoch) return false;  // a later batch (or none): never run a stale task
    const std::uint32_t idx = (std::uint32_t)h;
    if ((int)idx >= n_tasks_.load(std::memory_order_relaxed)) return false;
    if (head_.compare_exchange_weak(h, h + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
      task = (int)idx;
      return true;
    }
  }
}

void LanePool::worker(int /*id*/) {
  std::uint32_t seen = epoch_.load(std::memory_order_acquire);
  for (;;) {
    auto t0 = Clock::now();
    std::uint32_t spins = 0, ep = 0;
    while ((ep = epoch_.load(std::memory_order_acquire)) == seen && !stop_.load(std::memory_order_relaxed)) {
      U_LANE_PAUSE();
      if ((++spins & 1023u) == 0 && Clock::now() - t0 > kSpinBeforeSleep) {  // idle: sleep until the next batch
        std::unique_lock<std::mutex> lk(sleep_mu_);
        sleepers_.fetch_add(1, std::memory_order_seq_cst);
        sleep_cv_.wait(lk, [&] {
          return epoch_.load(std::memory_order_seq_cst) != seen || stop_.load(std::memory_order_seq_cst);
        });
        sleepers_.fetch_sub(1, std::memory_order_seq_cst);
        t0 = Clock::now();
      }
    }
    if (stop_.load(std::memory_order_relaxed)) return;
    seen = ep;
    int task = 0;
    while (claim(ep, task)) {
      (*fn_)(task);
      done_.fetch_add(1, std::memory_order_acq_rel);
    }
  }
}

void LanePool::run_phase(int n_tasks, const std::function<void(int)>& fn) {
  if (n_tasks <= 0) return;
  fn_ = &fn;
  n_tasks_.store(n_tasks, std::memory_order_relaxed);
  done_.store(0, std::memory_order_relaxed);
  const std::uint32_t ep = epoch_.load(std::memory_order_relaxed) + 1;
  head_.store((std::uint64_t)ep << 32, std::memory_order_release);
  epoch_.store(ep, std::memory_order_seq_cst);
  if (sleepers_.load(std::memory_order_seq_cst) > 0) {
    std::lock_guard<std::mutex> lk(sleep_mu_);
    sleep_cv_.notify_all();
  }
  int task = 0;
  while (claim(ep, task)) {  // the caller drains too (upstream host_works)
    fn(task);
    done_.fetch_add(1, std::memory_order_acq_rel);
  }
  while ((int)done_.load(std::memory_order_acquire) < n_tasks) U_LANE_PAUSE();
}

bool LanePool::serve(const LaneLayer& L, int n_tok, int k, const float* x, const int* ids, const int* eff,
                     int cpu_code, float* ymiss) {
  if (n_tok < 1 || n_tok > kMaxTok || k < 1) return false;
  // the layer's CPU experts as multi-token jobs (an expert's rows are read once for all of its tokens)
  jobs_.clear();
  bool tok_used[kMaxTok] = {};
  for (int j = 0; j < n_tok * k; ++j) {
    if (eff[j] != cpu_code) continue;
    const int t = j / k;
    Job* job = nullptr;
    for (auto& jb : jobs_)
      if (jb.e == ids[j]) job = &jb;
    if (job == nullptr) {
      jobs_.push_back(Job{});
      job = &jobs_.back();
      job->e = ids[j];
    }
    if (job->nt < kMaxTok) {
      job->tok[job->nt] = t;
      job->entry[job->nt] = j;
      ++job->nt;
    }
    tok_used[t] = true;
  }
  if (jobs_.empty()) return true;
  const int ne = L.n_embd, nf = L.n_ff;
  const std::size_t xq_bytes = (std::size_t)(ne / 32) * sizeof(BlockQ81);
  const std::size_t hq_bytes = (std::size_t)(nf / 32) * sizeof(BlockQ81);
  xq_.resize((std::size_t)n_tok * xq_bytes);
  for (int t = 0; t < n_tok; ++t)
    if (tok_used[t]) quantize_q81(x + (std::size_t)t * ne, reinterpret_cast<BlockQ81*>(xq_.data() + t * xq_bytes), ne);
  const int nj = (int)jobs_.size();
  gu_.resize((std::size_t)nj * kMaxTok * 2 * nf);
  hq_.resize((std::size_t)nj * kMaxTok * hq_bytes);
  const std::int64_t rb_g = row_bytes(L.gate_type, ne), rb_u = row_bytes(L.up_type, ne), rb_d = row_bytes(L.down_type, nf);

  // phase 1: gate | up rows, split across every thread
  const auto a0 = Clock::now();
  const int tpj_gu = (2 * nf + kGuRowsPerTask - 1) / kGuRowsPerTask;
  const std::function<void(int)> gu = [&](int task) {
    const Job& jb = jobs_[(std::size_t)(task / tpj_gu)];
    const int r0 = (task % tpj_gu) * kGuRowsPerTask, r1 = std::min(2 * nf, r0 + kGuRowsPerTask);
    float* out = gu_.data() + (std::size_t)(task / tpj_gu) * kMaxTok * 2 * nf;
    for (int r = r0; r < r1; ++r) {
      const bool g = r < nf;
      const std::uint8_t* row = g ? L.gate + jb.e * L.gate_stride + (std::int64_t)r * rb_g
                                  : L.up + jb.e * L.up_stride + (std::int64_t)(r - nf) * rb_u;
      const int type = g ? L.gate_type : L.up_type;
      for (int s = 0; s < jb.nt; ++s)
        out[(std::size_t)s * 2 * nf + r] =
            mmvq_row_dot(type, row, reinterpret_cast<const BlockQ81*>(xq_.data() + (std::size_t)jb.tok[s] * xq_bytes), ne);
    }
  };
  run_phase(nj * tpj_gu, gu);
  // phase 2: h = silu(gate) * up, quantized to Q8_1 like the GPU's down-proj input (a task per (job, token))
  const auto a1 = Clock::now();
  std::vector<int> pairs;  // job * kMaxTok + slot
  for (int jb = 0; jb < nj; ++jb)
    for (int s = 0; s < jobs_[(std::size_t)jb].nt; ++s) pairs.push_back(jb * kMaxTok + s);
  const std::function<void(int)> act = [&](int task) {
    thread_local std::vector<float> h;
    h.resize((std::size_t)nf);
    const int p = pairs[(std::size_t)task];
    const float* g = gu_.data() + (std::size_t)p * 2 * nf;
    for (int i = 0; i < nf; ++i) h[(std::size_t)i] = silu(g[i]) * g[nf + i];
    quantize_q81(h.data(), reinterpret_cast<BlockQ81*>(hq_.data() + (std::size_t)p * hq_bytes), nf);
  };
  run_phase((int)pairs.size(), act);
  // phase 3: down rows, split across every thread, straight into the tokens' ymiss rows
  const auto a2 = Clock::now();
  const int tpj_d = (ne + kDownRowsPerTask - 1) / kDownRowsPerTask;
  const std::function<void(int)> down = [&](int task) {
    const int jbi = task / tpj_d;
    const Job& jb = jobs_[(std::size_t)jbi];
    const int r0 = (task % tpj_d) * kDownRowsPerTask, r1 = std::min(ne, r0 + kDownRowsPerTask);
    for (int r = r0; r < r1; ++r) {
      const std::uint8_t* row = L.down + jb.e * L.down_stride + (std::int64_t)r * rb_d;
      for (int s = 0; s < jb.nt; ++s)
        ymiss[(std::size_t)jb.entry[s] * ne + r] = mmvq_row_dot(
            L.down_type, row, reinterpret_cast<const BlockQ81*>(hq_.data() + ((std::size_t)jbi * kMaxTok + s) * hq_bytes), nf);
    }
  };
  run_phase(nj * tpj_d, down);
  const auto a3 = Clock::now();
  ms_gu += std::chrono::duration<double, std::milli>(a1 - a0).count();
  ms_q += std::chrono::duration<double, std::milli>(a2 - a1).count();
  ms_down += std::chrono::duration<double, std::milli>(a3 - a2).count();
  experts += nj;
  rows_tokens += (std::int64_t)pairs.size();
  bytes += (std::int64_t)nj * (nf * (rb_g + rb_u) + (std::int64_t)ne * rb_d);
  return true;
}

void lane_expert_ref(const LaneLayer& L, std::int32_t e, const float* x, float* out) {
  const int ne = L.n_embd, nf = L.n_ff;
  std::vector<BlockQ81> xq((std::size_t)(ne / 32));
  quantize_q81(x, xq.data(), ne);
  const std::int64_t rb_g = row_bytes(L.gate_type, ne), rb_u = row_bytes(L.up_type, ne), rb_d = row_bytes(L.down_type, nf);
  std::vector<float> h((std::size_t)nf);
  for (int r = 0; r < nf; ++r) {
    const float g = mmvq_row_dot(L.gate_type, L.gate + e * L.gate_stride + (std::int64_t)r * rb_g, xq.data(), ne);
    const float u = mmvq_row_dot(L.up_type, L.up + e * L.up_stride + (std::int64_t)r * rb_u, xq.data(), ne);
    h[(std::size_t)r] = silu(g) * u;
  }
  std::vector<BlockQ81> hq((std::size_t)(nf / 32));
  quantize_q81(h.data(), hq.data(), nf);
  for (int r = 0; r < ne; ++r)
    out[r] = mmvq_row_dot(L.down_type, L.down + e * L.down_stride + (std::int64_t)r * rb_d, hq.data(), nf);
}

}  // namespace u::cpu
