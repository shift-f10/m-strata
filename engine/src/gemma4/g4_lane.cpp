// The decode token's CPU lane (g4_lane.hpp).
#include "g4_lane.hpp"

#include "strata/core/expert_source.hpp"
#include "strata/kernels/cpu/expert_layout.hpp"
#include "strata/kernels/cpu/native_expert.hpp"
#include "strata/kernels/cpu/pool.hpp"

#include <cuda_runtime.h>
#include <immintrin.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#include <intrin.h>
#endif

namespace g4 {
namespace {

namespace C = strata::kernels::cpu;
using Clock = std::chrono::steady_clock;

constexpr int kTopK = 8;
constexpr int kExperts = 128;
constexpr std::uint32_t kReleased = 0x7fffffffu;  // every flag past any ring: the GPU runs to the token's end

// Flag B only rises: a copy's host function may run after the lane raised it directly for a layer without copies.
void raise_flag(std::uint32_t* flag, std::uint32_t value) {
#if defined(_WIN32)
  volatile long* f = reinterpret_cast<volatile long*>(flag);
  long cur = *f;
  while (static_cast<std::uint32_t>(cur) < value) {
    const long prev = _InterlockedCompareExchange(f, static_cast<long>(value), cur);
    if (prev == cur) break;
    cur = prev;
  }
#else
  std::uint32_t cur = __atomic_load_n(flag, __ATOMIC_SEQ_CST);
  while (cur < value && !__atomic_compare_exchange_n(flag, &cur, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
  }
#endif
}

void store_flag(std::uint32_t* flag, std::uint32_t value) {
  _mm_sfence();
  *reinterpret_cast<volatile std::uint32_t*>(flag) = value;
}

std::uint32_t load_flag(const std::uint32_t* flag) { return *reinterpret_cast<const volatile std::uint32_t*>(flag); }

// The pinned arena as the dispatcher's expert source: every blob page-locked, so the copy engine reads it directly.
class ArenaSource final : public strata::core::ExpertSource {
 public:
  explicit ArenaSource(std::vector<CpuLane::Layer> layers) : layers_(std::move(layers)) {}
  const std::uint8_t* blob(std::int64_t layer, std::int64_t expert) override {
    if (layer < 0 || layer >= static_cast<std::int64_t>(layers_.size()) || expert < 0 || expert >= kExperts) return nullptr;
    const auto& L = layers_[static_cast<std::size_t>(layer)];
    return L.host + L.bytes * static_cast<std::size_t>(expert);
  }
  bool pinned(std::int64_t, std::int64_t) const override { return true; }
  bool pcie_layer(std::int64_t) const override { return true; }  // the copy engine stages a share (pcie_mode 0)

 private:
  std::vector<CpuLane::Layer> layers_;
};

struct FlagSet {
  std::uint32_t* flag{};
  std::uint32_t value{0};
};

}  // namespace

struct CpuLane::Impl {
  int device{0};
  int n_layers{0}, n_embd{0};
  std::vector<std::size_t> blob_bytes;
  std::unique_ptr<ArenaSource> src;
  std::unique_ptr<C::ExpertPool> pool;
  strata::core::ExpertDispatch d;
  strata::core::GpuPlanSink sink;
  std::vector<std::uint64_t> slot_off;
  void* mapped{};
  std::uint32_t *h_seq{}, *h_flag_a{}, *h_flag_b{}, *h_flag_c{};
  float *h_x{}, *h_rows{};
  std::int32_t *h_ids{}, *h_plan{};
  cudaStream_t dma{};
  std::uint8_t* staging{};
  std::size_t staging_bytes{0};
  std::vector<FlagSet> flag_sets;
  std::uint32_t ring{0};
  int n_tok{1};  // the step's tokens (begin_token)
  std::atomic<bool> abort{false};
  double pool_ms{0};
  long long staged{0};
};

CpuLane::CpuLane() : impl_(std::make_unique<Impl>()) {}

CpuLane::~CpuLane() {
  stop();
  Impl& I = *impl_;
  if (I.dma != nullptr) {
    cudaStreamSynchronize(I.dma);
    cudaStreamDestroy(I.dma);
  }
  I.pool.reset();
  if (I.mapped != nullptr) cudaFreeHost(I.mapped);
}

int CpuLane::workers() const { return impl_->pool ? impl_->pool->workers() + (impl_->pool->host_works() ? 1 : 0) : 0; }

bool CpuLane::start(const std::vector<Layer>& layers, int n_embd, int n_ff, const std::int32_t* host_res, const std::uint8_t* cache_base,
                    std::vector<std::uint64_t> slot_off, std::uint8_t* staging, std::size_t staging_bytes, int pcie_num, int workers,
                    std::string& error) {
  Impl& I = *impl_;
  if (layers.empty() || (n_embd % 4) != 0) {
    error = "the CPU lane needs layers of a width the row copy takes";
    return false;
  }
  if (!C::native_experts_available()) {
    error = "this build has no ggml-cpu expert path (STRATA_NATIVE_EXPERTS=OFF)";
    return false;
  }
  // the process-wide expert table the dispatcher and the pool read: one native format per layer, GEGLU
  C::ExpertLayout lay;
  lay.native = true;
  lay.n_layers = static_cast<std::int64_t>(layers.size());
  lay.n_expert = kExperts;
  std::uint64_t at = 0, max_blob = 0;
  for (const Layer& L : layers) {
    C::NativeFmt f;
    if (!C::native_fmt(L.gu_type, L.d_type, n_embd, n_ff, f, error)) return false;
    if (f.bytes != L.bytes) {
      error = "the CPU's native expert layout differs from the GPU's";
      return false;
    }
    f.act = 1;
    f.layer = static_cast<int>(lay.fmt.size());
    lay.fmt.push_back(f);
    lay.offset.push_back(at);
    lay.bytes.push_back(L.bytes);
    max_blob = std::max<std::uint64_t>(max_blob, L.bytes);
    at += L.bytes * kExperts;
    I.blob_bytes.push_back(L.bytes);
  }
  lay.max_blob = max_blob;
  lay.total = at;
  C::expert_layout_set(std::move(lay));
  I.n_layers = static_cast<int>(layers.size());
  I.n_embd = n_embd;
  cudaGetDevice(&I.device);

  // the mapped words: the doorbell and the three flags on lines of their own, then the payload, the plan and the rows,
  // each sized for a verify window of LanePlan::kMaxTok tokens (a decode step uses their first token's part)
  auto line = [](std::size_t b) { return (b + 63) & ~std::size_t{63}; };
  const std::size_t o_seq = 0, o_a = 64, o_b = 128, o_c = 192, o_ids = 256;
  const std::size_t o_plan = line(o_ids + sizeof(std::int32_t) * LanePlan::kMaxCap);
  const std::size_t o_x = line(o_plan + sizeof(std::int32_t) * kLanePlanMaxWords);
  const std::size_t o_rows = line(o_x + sizeof(float) * LanePlan::kMaxTok * static_cast<std::size_t>(n_embd));
  const std::size_t total = line(o_rows + sizeof(float) * LanePlan::kMaxCap * static_cast<std::size_t>(n_embd));
  if (cudaHostAlloc(&I.mapped, total, cudaHostAllocMapped | cudaHostAllocPortable) != cudaSuccess) {
    error = "cannot allocate the CPU lane's mapped memory";
    return false;
  }
  std::memset(I.mapped, 0, total);
  void* dmapped = nullptr;
  if (cudaHostGetDevicePointer(&dmapped, I.mapped, 0) != cudaSuccess) {
    error = "cannot map the CPU lane's memory for the GPU";
    return false;
  }
  auto* hb = static_cast<std::uint8_t*>(I.mapped);
  auto* db = static_cast<std::uint8_t*>(dmapped);
  I.h_seq = reinterpret_cast<std::uint32_t*>(hb + o_seq);
  I.h_flag_a = reinterpret_cast<std::uint32_t*>(hb + o_a);
  I.h_flag_b = reinterpret_cast<std::uint32_t*>(hb + o_b);
  I.h_flag_c = reinterpret_cast<std::uint32_t*>(hb + o_c);
  I.h_ids = reinterpret_cast<std::int32_t*>(hb + o_ids);
  I.h_plan = reinterpret_cast<std::int32_t*>(hb + o_plan);
  I.h_x = reinterpret_cast<float*>(hb + o_x);
  I.h_rows = reinterpret_cast<float*>(hb + o_rows);
  dev_.seq = reinterpret_cast<std::uint32_t*>(db + o_seq);
  dev_.flag_a = reinterpret_cast<const std::uint32_t*>(db + o_a);
  dev_.flag_b = reinterpret_cast<const std::uint32_t*>(db + o_b);
  dev_.flag_c = reinterpret_cast<const std::uint32_t*>(db + o_c);
  dev_.ids = reinterpret_cast<std::int32_t*>(db + o_ids);
  dev_.plan = reinterpret_cast<const std::int32_t*>(db + o_plan);
  dev_.x = reinterpret_cast<float*>(db + o_x);
  dev_.rows = reinterpret_cast<const float*>(db + o_rows);
  if (cudaStreamCreateWithFlags(&I.dma, cudaStreamNonBlocking) != cudaSuccess) {
    error = "cannot create the CPU lane's copy stream";
    return false;
  }
  I.staging = staging;
  I.staging_bytes = staging_bytes;
  I.flag_sets.assign(layers.size(), FlagSet{});
  I.slot_off = std::move(slot_off);

  I.src = std::make_unique<ArenaSource>(layers);
  I.pool = std::make_unique<C::ExpertPool>(workers);
  strata::core::GpuPlanSink& P = I.sink;
  P.counts = I.h_plan + LanePlan::kCounts;
  P.start = I.h_plan + LanePlan::kStart;
  P.dst = I.h_plan + LanePlan::kDst;
  P.tok = I.h_plan + LanePlan::kTok;
  P.ptr = reinterpret_cast<unsigned long long*>(I.h_plan + LanePlan::kPtr);
  P.ptr2 = reinterpret_cast<unsigned long long*>(I.h_plan + LanePlan::kPtr2);
  P.start2 = I.h_plan + LanePlan::kStart2;
  P.staging = reinterpret_cast<unsigned long long>(staging);
  P.cap = LanePlan::kCap;
  P.publish = &CpuLane::publish;
  P.fetch = &CpuLane::fetch;
  P.ctx = this;
  P.pcie_mode = 0;
  strata::core::ExpertDispatch& D = I.d;
  D.pool = I.pool.get();
  D.src = I.src.get();
  D.n_expert = kExperts;
  D.host_res = host_res;
  D.cache_base = cache_base;
  D.cache_slot_off = I.slot_off.empty() ? nullptr : I.slot_off.data();
  D.plan = &I.sink;
  D.pcie_num = std::clamp(pcie_num, 0, 256);
  thread_ = std::thread([this] { run(); });
  return true;
}

void CpuLane::stop() {
  if (thread_.joinable()) {
    impl_->abort.store(true);
    {
      std::lock_guard<std::mutex> lk(mu_);
      quit_ = true;
    }
    cv_.notify_one();
    thread_.join();
  }
}

void CpuLane::abort_token() { impl_->abort.store(true); }

void CpuLane::begin_token(const int n_tok) {
  Impl& I = *impl_;
  // the plan's layout for the step's entries (the lane thread waits on the condition variable: it reads them once armed)
  I.n_tok = std::clamp(n_tok, 1, LanePlan::kMaxTok);
  const LanePlan L = LanePlan::at(kTopK * I.n_tok);
  strata::core::GpuPlanSink& P = I.sink;
  P.counts = I.h_plan + L.counts;
  P.start = I.h_plan + L.start;
  P.dst = I.h_plan + L.dst;
  P.tok = I.h_plan + L.tok;
  P.ptr = reinterpret_cast<unsigned long long*>(I.h_plan + L.ptr);
  P.ptr2 = reinterpret_cast<unsigned long long*>(I.h_plan + L.ptr2);
  P.start2 = I.h_plan + L.start2;
  P.cap = L.cap;
  if (I.n_tok > 1) ++windows_;
  store_flag(I.h_seq, 0);
  store_flag(I.h_flag_a, 0);
  store_flag(I.h_flag_b, 0);
  store_flag(I.h_flag_c, 0);
  I.abort.store(false);
  {
    std::lock_guard<std::mutex> lk(mu_);
    armed_ = true;
  }
  ++begun_;
  cv_.notify_one();
}

bool CpuLane::end_token(std::string& error) {
  // the stream has synced, so the GPU is past every flag of the token: the lane is at most in its last layer's epilogue
  const auto t0 = Clock::now();
  while (served_.load(std::memory_order_acquire) < begun_) {
    _mm_pause();
    if (Clock::now() - t0 > std::chrono::seconds(30)) {
      error = "the CPU lane did not finish its token";
      return false;
    }
  }
  if (failed_.load()) {
    error = fail_;
    return false;
  }
  return true;
}

CpuLane::Stats CpuLane::stats() const {
  const Impl& I = *impl_;
  Stats s;
  s.tokens = begun_;
  s.windows = windows_;
  s.hits = I.d.cache_hits;
  s.staged = I.d.pcie_experts;
  s.cpu_entries = I.d.multi_entries;
  s.cpu_experts = I.d.multi_misses;
  s.pool_ms = I.pool_ms;
  return s;
}

void CpuLane::run() {
  Impl& I = *impl_;
  cudaSetDevice(I.device);
  // the host core the pool keeps free of workers (its "host" placement): the spin never takes a worker's cycles
  const int core = C::planned_host_core();
  C::ThreadAffinity previous;
  if (core >= 0) previous = C::pin_current_thread(core);
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return armed_ || quit_; });
      if (quit_) break;
      armed_ = false;
    }
    bool ok = true;
    for (int l = 0; l < I.n_layers && ok; ++l) {
      const auto t0 = Clock::now();
      unsigned spins = 0;
      while (load_flag(I.h_seq) < static_cast<std::uint32_t>(l + 1)) {
        _mm_pause();
        if ((++spins & 4095u) != 0) continue;
        if (I.abort.load()) {
          ok = false;
          break;
        }
        if (Clock::now() - t0 > std::chrono::seconds(60)) {
          fail_ = "the GPU did not ring the CPU lane";
          failed_.store(true);
          ok = false;
          break;
        }
      }
      if (ok) ok = serve(l);
    }
    if (!ok) release_gpu();
    served_.fetch_add(1, std::memory_order_release);
  }
  if (previous.valid) C::restore_thread_affinity(previous);
}

bool CpuLane::serve(int layer) {
  Impl& I = *impl_;
  I.ring = static_cast<std::uint32_t>(layer + 1);
  I.d.layers = layer;
  const std::size_t bb = I.blob_bytes[static_cast<std::size_t>(layer)];
  I.sink.staging_cap = static_cast<std::int64_t>(std::min<std::size_t>(64, bb > 0 ? I.staging_bytes / bb : 0));
  std::atomic_thread_fence(std::memory_order_acquire);
  const auto t0 = Clock::now();
  strata::core::expert_pool_dispatch_multi(I.d, I.h_x, I.h_ids, I.n_tok, kTopK, I.h_rows);
  I.pool_ms += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  if (I.d.failed) {
    fail_ = std::string("the CPU lane failed: ") + (I.d.fail != nullptr ? I.d.fail : "unknown");
    failed_.store(true);
    return false;
  }
  store_flag(I.h_flag_c, I.ring);  // the CPU's rows are in the mapped rows
  return true;
}

// A failed or aborted token: no groups, and every flag released, so the GPU runs to the token's end (its result is
// not used: end_token reports the failure).
void CpuLane::release_gpu() {
  Impl& I = *impl_;
  I.h_plan[LanePlan::kCounts] = 0;
  I.h_plan[LanePlan::kCounts + 1] = 0;
  I.h_plan[LanePlan::kCounts + 2] = 0;
  store_flag(I.h_flag_a, kReleased);
  raise_flag(I.h_flag_b, kReleased);
  store_flag(I.h_flag_c, kReleased);
}

void CpuLane::publish(void* ctx) {
  Impl& I = *static_cast<CpuLane*>(ctx)->impl_;
  store_flag(I.h_flag_a, I.ring);  // the plan is written: the GPU starts its VRAM groups
}

// The copy engine's share (paper 3.3, finding 6): the blobs into staging while the CPU computes its own share; a host
// function raises flag B when they have landed.
void CpuLane::fetch(void* ctx, const std::uint8_t* const* src, int n, std::size_t bytes) {
  CpuLane* self = static_cast<CpuLane*>(ctx);
  Impl& I = *self->impl_;
  if (n <= 0) {
    raise_flag(I.h_flag_b, I.ring);
    return;
  }
  bool ok = true;
  for (int i = 0; i < n && ok; ++i)
    ok = cudaMemcpyAsync(I.staging + static_cast<std::size_t>(i) * bytes, src[i], bytes, cudaMemcpyHostToDevice, I.dma) == cudaSuccess;
  FlagSet& fs = I.flag_sets[static_cast<std::size_t>(I.ring - 1)];
  fs.flag = I.h_flag_b;
  fs.value = I.ring;
  if (!ok || cudaLaunchHostFunc(I.dma, [](void* p) { auto* s = static_cast<FlagSet*>(p); raise_flag(s->flag, s->value); }, &fs) != cudaSuccess) {
    self->fail_ = "the CPU lane could not stage experts";
    self->failed_.store(true);
    raise_flag(I.h_flag_b, I.ring);
    return;
  }
  // into the driver once: a submission WDDM batches would otherwise wait for the next call (graph.hpp, NOTE 1)
  (void) cudaStreamQuery(I.dma);
  I.staged += n;
}

}  // namespace g4
