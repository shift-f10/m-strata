// multiengine_flint: load, placement and the forward passes (design 2026-10-27-flint-engine-design.md, sections 2-3) -
// Gemma 4's engine (g4_engine.cu) at flint's shape, on the same tier: the pinned expert arena (paper 3.1), the VRAM
// expert cache filled from a profile and moved during decode without anyone waiting (3.1, 3.4), the CPU lane for the
// decode token's misses (3.3), the decode step as one captured graph (3.6) and prompt chunks with the misses streamed
// to MMQ (3.6). flint's own: the expert tier holds the 47 MoE layers (tier layer t is model layer kDenseLayers + t);
// attention with per-head sinks, NeoX RoPE only on the layers rope_pattern names, the 513-key window only on the
// sliding ones; the sigmoid router's top 6 with an ungated shared expert; SwiGLU; an untied head; any number of shards.
#include "f_engine.hpp"

#include "f_kernels.cuh"
#include "f_lane.hpp"
#include "f_profile.hpp"
#include "g4_file.hpp"

#include "strata/core/expert_cache.hpp"
#include "strata/core/graph.hpp"
#include "strata/kernels/elementwise.hpp"
#include "strata/kernels/iq_kernels.hpp"
#include "strata/kernels/s2_expert_grouped.hpp"
#include "strata/kernels/verify_kernels.hpp"
#include "strata/prefill/moe_mmq.hpp"

#include <cublas_v2.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <numeric>
#include <type_traits>
#include <vector>

namespace fl {
namespace {

using Clock = std::chrono::steady_clock;

namespace MMQ = strata::prefill::mmq;

constexpr int kGuRows = 2 * kFfExp;  // 1024: gate rows then up rows
constexpr int kPicks = kExpertsUsed;

// P5 (paper 3.6): a prompt chunk of at least kMmqMinRows tokens computes its experts with llama.cpp's MMQ - the
// VRAM-resident ones straight from their slots, the others streamed from the pinned arena by a copy stream into a
// ring of kRingBufs staging buffers of kRingGroup experts each, while the GPU computes the ones before them.
constexpr int kMmqMinRows = 64;
constexpr int kRingBufs = 3;
constexpr int kRingGroup = 16;
// a layer's positions' bounds (<= 2 x 256 + 1) and its groups' relative bounds (<= 16 x 17)
constexpr int kMqBoundsInts = 1024;
// a streamed grouped-kernel call's plan (moe_ring_grouped): [0] its group count, [kRgStart..] the groups' starts
// (count + 1), [kRgPtr..] their blobs (u64); at most one call for the slots and one per expert
constexpr int kRgStart = 2;
constexpr int kRgPtr = kRgStart + kExperts + 2;
constexpr int kRgCallInts = kRgPtr + 2 * kExperts;
constexpr int kRgCalls = 1 + kExperts;

std::size_t row_bytes(TensorType t, std::uint64_t cols) {
  return t == TensorType::q8_0 ? static_cast<std::size_t>(cols / 32 * 34)
         : t == TensorType::bf16 ? static_cast<std::size_t>(cols * 2)
                                 : static_cast<std::size_t>(cols * 4);
}

// GGUF Q8_0 rows ([fp16 d][32 x int8] blocks) into the engine's int8 plane and fp16 scale plane.
void repack_q8(const std::uint8_t* src, std::size_t rows, std::size_t cols, std::int8_t* q, std::uint16_t* s) {
  const std::size_t nb = cols / 32;
  for (std::size_t r = 0; r < rows; ++r) {
    for (std::size_t b = 0; b < nb; ++b) {
      const std::uint8_t* blk = src + (r * nb + b) * 34;
      std::memcpy(&s[r * nb + b], blk, 2);
      std::memcpy(&q[r * cols + b * 32], blk + 2, 32);
    }
  }
}

// One expert blob in Strata's native form (kernels::NativeExpertLayout): [gate rows | up rows | down rows], the GGUF's
// own blocks - the expert's slices of ffn_gate_exps, ffn_up_exps and ffn_down_exps copied as the file stores them
// (paper 3.6). SwiGLU between them.
strata::kernels::NativeExpertLayout native_layout(TensorType gu, TensorType dn) {
  auto L = strata::kernels::native_expert_layout(static_cast<int>(gu), static_cast<int>(dn), kEmbd, kFfExp);
  L.act = 0;  // SwiGLU: silu(gate) * up
  return L;
}

struct DevLayer {
  int index{0};    // the model layer
  int tier{-1};    // the expert tier's layer (index - kDenseLayers), -1 on the dense layers
  bool moe{false};
  bool sliding{true};
  bool rope{true};
  bool mmq{false};  // the prompt's experts on MMQ (both expert types have it: Q8_0)
  std::size_t stream_stride{0};  // a streamed prompt's expert stride: the slots' and the ring's (blob, guard, aligned)
  int stream_group{0};           // experts of this layer one ring buffer holds (at most kRingGroup)
  flk::Mat q{}, k{}, v{}, o{}, gate{}, up{}, down{}, router{}, gs{}, us{}, ds{};
  float *attn_norm{}, *q_norm{}, *k_norm{}, *sinks{}, *ffn_norm{};
  strata::kernels::NativeExpertLayout nl{};  // the layer's native expert blob (GGUF blocks, SwiGLU)
  std::uint8_t* host_experts{};   // the pinned arena's range for this layer: 256 blobs (mapped)
  std::uint8_t* host_alias{};     // the device's address for it
  int n_slots{0};
  int slot0{0};                   // the layer's first slot in the expert cache
  std::vector<int> slot_of;       // expert -> slot index in the cache, -1 when only in RAM, -2 landing
  std::vector<int> expert_in;     // the layer's slot j -> expert (-1: empty, or a swap still landing)
  std::vector<unsigned long long> ptr_host;  // expert -> its blob's device address (VRAM slot or the arena alias)
  unsigned long long* ptrtab{};   // device [256]: the same
  int* counts{};                  // device [256] routing counts since the last rebalance (in counts_all_)
  std::vector<double> score;      // decayed routing counts
  __half* kc{};
  __half* vc{};
  int cap{0};
};

class FlintEngine final : public Engine {
 public:
  ~FlintEngine() override { release(); }

  bool load(const EngineConfig& cfg, const ModelDesc& m, std::string& error);

  int context() const override { return ctx_; }
  int ring_spare() const override { return ring_ - kWindow; }
  int position() const override { return pos_; }
  void truncate(int p) override { pos_ = std::clamp(p, 0, pos_); }
  int process(const std::int32_t* ids, int n, const std::function<bool(int)>& progress) override;
  bool logits(std::vector<float>& out) override;
  int greedy() override;
  int top_candidates(int k, float* vals, int* ids) override;
  void rebalance(int max_swaps) override;
  void set_layer_sink(std::vector<float>* sink) override { sink_ = sink; }
  bool save_profile(bool force, std::string& error) override;
  LoadReport report() const override { return report_; }

 private:
  DevLayer& tier(int t) { return layers_[static_cast<std::size_t>(kDenseLayers + t)]; }
  bool cuda_ok(cudaError_t e, const char* what);
  bool launch_ok(const char* what) { return cuda_ok(cudaGetLastError(), what); }
  template <typename T>
  bool alloc(T*& p, std::size_t count) {
    void* raw = nullptr;
    if (!cuda_ok(cudaMalloc(&raw, std::max<std::size_t>(count, 1) * sizeof(T)), "cudaMalloc")) return false;
    allocations_.push_back(raw);
    p = static_cast<T*>(raw);
    return true;
  }
  bool read(const TensorSpan& t, std::uint64_t at, void* out, std::size_t bytes);
  bool upload_vector(const TensorSpan& t, float*& out);
  bool upload_matrix(const TensorSpan& t, flk::Mat& out);
  bool load_experts(DevLayer& L, const LayerDesc& d);
  bool place_experts(double vram_gib, int forced_slots, const std::string& profile, std::string& error);
  bool moe_experts(DevLayer& L, int T);
  bool moe_lane(DevLayer& L);
  bool moe_mmq(DevLayer& L, int T);
  bool moe_ring_grouped(DevLayer& L, int T);
  int stream_plan(const DevLayer& L, int A, int* bounds, int* inv, int* ent, int* tok);
  bool ring_begin();
  bool ring_kick(std::size_t i);
  const std::uint8_t* ring_take(const DevLayer& L, int g);
  bool ring_give();
  struct Rows {
    std::size_t rows{0}, s_floats{0}, bytes{0};
    float *x{}, *x1{}, *h{}, *o{}, *ffn{}, *moe{}, *qraw{}, *qout{}, *attn{}, *kraw{}, *vraw{};
    float *g{}, *u{}, *m{}, *rl{}, *yg{}, *s_buf{}, *mq_gu{}, *mq_h{};
    __half *q16{}, *p_buf{};
    std::uint8_t *xq{}, *escratch{}, *mq_x{}, *mq_hq{}, *ring{};
  };
  std::size_t layout_rows(std::uint8_t* base, std::size_t rows, std::size_t s_floats, bool streamed, Rows& r) const;
  void bind(const Rows& r);
  bool lend();
  bool give_back();
  bool start_lane(const EngineConfig& cfg, std::string& error);
  void set_res(const DevLayer& L, int e, int slot);
  bool set_ptr(DevLayer& L, int e, unsigned long long p);
  void land_swaps();
  bool gemm(const flk::Mat& w, const float* x, float* y, int T);
  bool sgemm(int rows, int T, int cols, const float* w, const float* x, float* y);
  bool forward_one(std::int32_t id);
  bool decode_body(int pos, bool lane, int split_full, int split_swa);
  bool forward_batch(const std::int32_t* ids, int T);
  bool prompt_attention(DevLayer& L, int pos0, int T);
  bool ffn_dense(DevLayer& L, int T, bool batch);
  bool ffn_shared(DevLayer& L, int T, bool batch);
  bool head(const float* x_row);
  void dump_layer(int layer, int pos0, int T);
  // Test-only (STRATA_FLINT_SEAMS=<dir>): the prompt path's named tensors in llama.cpp's cb() names and its seam files'
  // format (int64 ne[4], then the f32 rows), for a per-seam diff against the oracle. Off: nothing is read back.
  void seam(const char* name, int layer, const float* d, int n, int T);
  void release();

  std::vector<g4::ReadOnlyFile> files_;   // one per shard
  std::vector<std::uint8_t> staging_;
  std::vector<void*> allocations_;
  std::vector<void*> pinned_;
  cudaStream_t st_{};
  cudaStream_t copy_{};   // the copy engine's stream: the expert cache's swaps (paper finding 10)
  cublasHandle_t blas_{};
  bool broken_{false};

  strata::core::ExpertCache cache_;
  struct Swap {
    int tier{0}, slot{0}, expert{0};
    cudaEvent_t landed{};
  };
  std::vector<Swap> swaps_;
  std::vector<cudaEvent_t> free_events_;
  int* counts_h_{};
  cudaEvent_t counts_ev_{};
  cudaEvent_t evict_ev_{};
  bool counts_pending_{false};
  unsigned long long* plan_ptr_{};
  int *plan_start_{}, *plan_n_{}, *plan_dst_{}, *plan_tok_{};
  int* plan_h_{};
  unsigned long long* plan_ptr_h_{};
  std::uint8_t* xq_{};
  std::uint8_t* escratch_{};
  int* ident_{};
  std::unique_ptr<CpuLane> lane_;
  std::int32_t* res_h_{};
  int* res_d_{};
  bool res_dirty_{false};
  std::int32_t* lplan_{};
  float* hit_out_{};
  std::uint8_t* lstage_{};
  std::size_t lstage_bytes_{0};
  bool mmq_on_{false};
  std::unique_ptr<MMQ::Context> mctx_;
  std::size_t mmq_guard_{0};
  std::uint8_t *mq_x_{}, *mq_hq_{};
  float *mq_gu_{}, *mq_h_{};
  int *mq_bounds_{}, *mq_ids_{}, *mq_pin_{};
  cudaStream_t ring_st_{};
  std::uint8_t* ring_buf_{};
  std::size_t ring_buf_bytes_{0};
  int *rg_h_{}, *rg_d_{};
  cudaEvent_t ring_landed_[kRingBufs]{}, ring_freed_[kRingBufs]{};
  bool ring_dirty_[kRingBufs]{};
  struct RingGroup {
    int tier{0}, first{0}, n{0};
  };
  std::vector<RingGroup> ring_seq_;
  std::size_t ring_next_{0};
  std::vector<std::vector<int>> miss_;   // per tier layer, this chunk: the experts without a slot, ascending
  std::vector<std::pair<std::int32_t, std::int32_t>> profile_prior_;
  std::string profile_save_;
  Clock::time_point profile_saved_{};
  Rows small_, big_;
  std::size_t rows_{0};
  bool borrow_{false}, lent_{false};
  int lend_slot_{-1};
  std::vector<std::pair<int, int>> lent_experts_;   // (slot, expert)
  bool graph_on_{true};
  std::unique_ptr<strata::core::CapturedGraph> graph_;
  int graph_key_{-1};

  int ctx_{0}, chunk_{0}, ring_{0}, pos_{0}, max_splits_{0}, n_run_{kLayers};
  float eps_{1e-6f}, rope_base_{1e4f}, w_scale_{1.0f}, attn_scale_{0.0f};
  std::vector<DevLayer> layers_;
  flk::Mat embd_{}, output_{};
  float* output_norm_{};

  int* ids_d_{};
  int* ids_h_{};
  float *x_{}, *x1_{}, *h_{}, *o_{}, *ffn_{}, *moe_{};
  float *qraw_{}, *qout_{}, *attn_{}, *kraw_{}, *vraw_{};
  __half* q16_{};
  float *g_{}, *u_{}, *m_{}, *rl_{}, *wts_{};
  int* picks_{};
  int* picks_h_{};
  float* yg_{};
  float* wdq_{};
  float* s_buf_{};
  __half* p_buf_{};
  std::size_t s_floats_{0};
  float* dscratch_{};
  float* logits_{};
  float* xn_{};
  float* argmax_scratch_{};
  int* counts_all_{};
  unsigned char* topk_scratch_{};
  float* cand_v_{};
  int* cand_i_{};
  int* cand_flag_{};
  int* cand_h_{};
  int* argmax_out_{};
  int* argmax_h_{};

  std::vector<float>* sink_{nullptr};
  std::string seams_;   // STRATA_FLINT_SEAMS
  LoadReport report_{};
};

void FlintEngine::seam(const char* name, int layer, const float* d, int n, int T) {
  if (seams_.empty()) return;
  std::vector<float> rows(static_cast<std::size_t>(n) * T);
  cudaMemcpyAsync(rows.data(), d, rows.size() * 4, cudaMemcpyDeviceToHost, st_);
  cudaStreamSynchronize(st_);
  const std::string path = seams_ + "/" + name + "-" + std::to_string(layer) + ".bin";
  std::FILE* f = nullptr;
#if defined(_WIN32)
  if (fopen_s(&f, path.c_str(), "wb") != 0) f = nullptr;
#else
  f = std::fopen(path.c_str(), "wb");
#endif
  if (f == nullptr) return;
  const std::int64_t ne[4] = {n, T, 1, 1};
  std::fwrite(ne, sizeof(ne), 1, f);
  std::fwrite(rows.data(), 4, rows.size(), f);
  std::fclose(f);
}

bool FlintEngine::cuda_ok(cudaError_t e, const char* what) {
  if (e == cudaSuccess) return true;
  std::fprintf(stderr, "multiengine-flint: CUDA error in %s: %s\n", what, cudaGetErrorString(e));
  broken_ = true;
  return false;
}

void FlintEngine::release() {
  if (!broken_) {
    std::string why;
    if (!save_profile(true, why)) std::fprintf(stderr, "multiengine-flint: the expert profile was not saved: %s\n", why.c_str());
  }
  if (lane_) {
    const CpuLane::Stats s = lane_->stats();
    if (s.tokens > 0) {
      std::fprintf(stderr, "multiengine-flint: CPU lane: %lld decode steps; %lld expert entries from VRAM, %lld staged experts, %lld "
                   "entries (%lld experts) on the CPU pool in %.0f ms\n", s.tokens, s.hits, s.staged, s.cpu_entries, s.cpu_experts,
                   s.pool_ms);
    }
    lane_->stop();
  }
  if (st_ != nullptr) cudaStreamSynchronize(st_);
  lane_.reset();
  graph_.reset();
  if (ring_st_ != nullptr) cudaStreamSynchronize(ring_st_);
  for (int k = 0; k < kRingBufs; ++k) {
    if (ring_landed_[k] != nullptr) cudaEventDestroy(ring_landed_[k]);
    if (ring_freed_[k] != nullptr) cudaEventDestroy(ring_freed_[k]);
    ring_landed_[k] = ring_freed_[k] = nullptr;
  }
  if (ring_st_ != nullptr) cudaStreamDestroy(ring_st_);
  ring_st_ = nullptr;
  mctx_.reset();
  if (copy_ != nullptr) cudaStreamSynchronize(copy_);
  for (auto& s : swaps_) cudaEventDestroy(s.landed);
  swaps_.clear();
  for (cudaEvent_t e : free_events_) cudaEventDestroy(e);
  free_events_.clear();
  if (counts_ev_ != nullptr) cudaEventDestroy(counts_ev_);
  counts_ev_ = nullptr;
  if (evict_ev_ != nullptr) cudaEventDestroy(evict_ev_);
  evict_ev_ = nullptr;
  cache_.close();
  if (blas_ != nullptr) cublasDestroy(blas_);
  blas_ = nullptr;
  for (void* p : allocations_) cudaFree(p);
  allocations_.clear();
  for (void* p : pinned_) cudaFreeHost(p);
  pinned_.clear();
  if (copy_ != nullptr) cudaStreamDestroy(copy_);
  copy_ = nullptr;
  if (st_ != nullptr) cudaStreamDestroy(st_);
  st_ = nullptr;
}

bool FlintEngine::read(const TensorSpan& t, std::uint64_t at, void* out, std::size_t bytes) {
  if (t.shard < 0 || t.shard >= static_cast<int>(files_.size()) || !files_[static_cast<std::size_t>(t.shard)].read_at(t.file_offset + at, out, bytes)) {
    std::fprintf(stderr, "multiengine-flint: cannot read the model file\n");
    return false;
  }
  return true;
}

bool FlintEngine::upload_vector(const TensorSpan& t, float*& out) {
  if (!t.present) {
    out = nullptr;
    return true;
  }
  staging_.resize(static_cast<std::size_t>(t.bytes));
  if (!read(t, 0, staging_.data(), staging_.size())) return false;
  if (!alloc(out, static_cast<std::size_t>(t.bytes / 4))) return false;
  return cuda_ok(cudaMemcpy(out, staging_.data(), staging_.size(), cudaMemcpyHostToDevice), "upload");
}

bool FlintEngine::upload_matrix(const TensorSpan& t, flk::Mat& out) {
  out.rows = static_cast<int>(t.rows);
  out.cols = static_cast<int>(t.cols);
  const std::size_t rows = static_cast<std::size_t>(t.rows), cols = static_cast<std::size_t>(t.cols);
  const std::size_t rb = row_bytes(t.type, t.cols);
  if (t.type == TensorType::f32 || t.type == TensorType::bf16) {
    out.kind = t.type == TensorType::f32 ? flk::WKind::f32 : flk::WKind::bf16;
    std::uint8_t* d = nullptr;
    if (!alloc(d, rows * rb)) return false;
    const std::size_t step = std::max<std::size_t>(1, (64u << 20) / rb);
    for (std::size_t r0 = 0; r0 < rows; r0 += step) {
      const std::size_t n = std::min(step, rows - r0);
      staging_.resize(n * rb);
      if (!read(t, r0 * rb, staging_.data(), n * rb)) return false;
      if (!cuda_ok(cudaMemcpy(d + r0 * rb, staging_.data(), n * rb, cudaMemcpyHostToDevice), "upload")) return false;
    }
    out.w = d;
    out.s = nullptr;
    return true;
  }
  out.kind = flk::WKind::q8;
  std::int8_t* q = nullptr;
  __half* s = nullptr;
  if (!alloc(q, rows * cols) || !alloc(s, rows * (cols / 32))) return false;
  const std::size_t step = std::max<std::size_t>(1, (32u << 20) / rb);
  std::vector<std::int8_t> hq;
  std::vector<std::uint16_t> hs;
  for (std::size_t r0 = 0; r0 < rows; r0 += step) {
    const std::size_t n = std::min(step, rows - r0);
    staging_.resize(n * rb);
    if (!read(t, r0 * rb, staging_.data(), n * rb)) return false;
    hq.resize(n * cols);
    hs.resize(n * (cols / 32));
    repack_q8(staging_.data(), n, cols, hq.data(), hs.data());
    if (!cuda_ok(cudaMemcpy(q + r0 * cols, hq.data(), hq.size(), cudaMemcpyHostToDevice), "upload") ||
        !cuda_ok(cudaMemcpy(s + r0 * (cols / 32), hs.data(), hs.size() * 2, cudaMemcpyHostToDevice), "upload")) {
      return false;
    }
  }
  out.w = q;
  out.s = s;
  return true;
}

// The pinned arena (paper 3.1, finding 13): every routed expert in page-locked RAM, one range per layer (a copy or a
// read never spans two), mapped so the GPU can reach it too. Each blob is the GGUF's own bytes: the expert's gate slice
// then its up slice into [0, down_off), its down slice into [down_off, bytes).
bool FlintEngine::load_experts(DevLayer& L, const LayerDesc& d) {
  L.nl = native_layout(d.gate_exps.type, d.down_exps.type);
  const std::size_t g_file = row_bytes(d.gate_exps.type, kEmbd) * kFfExp;
  const std::size_t dn_file = row_bytes(d.down_exps.type, kFfExp) * kEmbd;
  if (2 * g_file != L.nl.down_off || L.nl.down_off + dn_file != L.nl.bytes) {
    std::fprintf(stderr, "multiengine-flint: the expert tensors do not match the native blob layout\n");
    return false;
  }
  void* host = nullptr;
  if (!cuda_ok(cudaHostAlloc(&host, L.nl.bytes * kExperts, cudaHostAllocMapped | cudaHostAllocPortable), "cudaHostAlloc")) {
    std::fprintf(stderr, "multiengine-flint: cannot pin %.2f GiB of host memory for a layer's experts\n",
                 double(L.nl.bytes * kExperts) / double(1ull << 30));
    return false;
  }
  pinned_.push_back(host);
  L.host_experts = static_cast<std::uint8_t*>(host);
  void* alias = nullptr;
  if (!cuda_ok(cudaHostGetDevicePointer(&alias, host, 0), "cudaHostGetDevicePointer")) return false;
  L.host_alias = static_cast<std::uint8_t*>(alias);
  for (int e = 0; e < kExperts; ++e) {
    std::uint8_t* blob = L.host_experts + L.nl.bytes * static_cast<std::size_t>(e);
    if (!read(d.gate_exps, g_file * static_cast<std::size_t>(e), blob, g_file) ||
        !read(d.up_exps, g_file * static_cast<std::size_t>(e), blob + g_file, g_file) ||
        !read(d.down_exps, dn_file * static_cast<std::size_t>(e), blob + L.nl.down_off, dn_file)) {
      return false;
    }
  }
  return true;
}

bool FlintEngine::set_ptr(DevLayer& L, int e, unsigned long long p) {
  L.ptr_host[static_cast<std::size_t>(e)] = p;
  flk::set_u64(L.ptrtab, e, p, st_);
  return launch_ok("expert table");
}

void FlintEngine::set_res(const DevLayer& L, int e, int slot) {
  if (res_h_ == nullptr) return;
  res_h_[static_cast<std::size_t>(L.tier) * kExperts + static_cast<std::size_t>(e)] = slot;
  res_dirty_ = true;
}

std::size_t FlintEngine::layout_rows(std::uint8_t* base, std::size_t rows, std::size_t s_floats, bool streamed, Rows& r) const {
  std::size_t at = 0;
  auto take = [&](auto*& p, std::size_t count) {
    using T = std::remove_reference_t<decltype(*p)>;
    at = (at + 255) & ~std::size_t{255};
    p = base != nullptr ? reinterpret_cast<T*>(base + at) : nullptr;
    at += std::max<std::size_t>(count, 1) * sizeof(T);
  };
  r = Rows{};
  for (float** p : {&r.x, &r.x1, &r.h, &r.o, &r.ffn, &r.moe}) take(*p, rows * kEmbd);
  for (float** p : {&r.qraw, &r.qout, &r.attn}) take(*p, rows * kHeads * kHead);
  take(r.q16, rows * kHeads * kHead);
  for (float** p : {&r.kraw, &r.vraw}) take(*p, rows * kKvHeads * kHead);
  for (float** p : {&r.g, &r.u, &r.m}) take(*p, rows * kFf);
  take(r.rl, rows * kExperts);
  take(r.yg, rows * kPicks * kEmbd);
  take(r.xq, rows * (kEmbd / 32) * 36);
  take(r.escratch, strata::kernels::native_expert_scratch_bytes(static_cast<std::int64_t>(rows) * kPicks, kFfExp));
  take(r.s_buf, s_floats);
  take(r.p_buf, s_floats);
  if (streamed) {
    const auto A = static_cast<std::int64_t>(rows) * kPicks;
    take(r.ring, std::size_t(kRingBufs) * ring_buf_bytes_);
    take(r.mq_x, MMQ::q8_bytes(A, kEmbd));
    take(r.mq_gu, static_cast<std::size_t>(A) * kGuRows);
    take(r.mq_h, static_cast<std::size_t>(A) * kFfExp);
    take(r.mq_hq, MMQ::q8_bytes(A, kFfExp));
  }
  r.rows = rows;
  r.s_floats = s_floats;
  r.bytes = (at + 255) & ~std::size_t{255};
  return r.bytes;
}

void FlintEngine::bind(const Rows& r) {
  x_ = r.x; x1_ = r.x1; h_ = r.h; o_ = r.o; ffn_ = r.ffn; moe_ = r.moe;
  qraw_ = r.qraw; qout_ = r.qout; attn_ = r.attn; q16_ = r.q16; kraw_ = r.kraw; vraw_ = r.vraw;
  g_ = r.g; u_ = r.u; m_ = r.m; rl_ = r.rl; yg_ = r.yg; xq_ = r.xq; escratch_ = r.escratch;
  s_buf_ = r.s_buf; p_buf_ = r.p_buf; s_floats_ = r.s_floats;
  ring_buf_ = r.ring; mq_x_ = r.mq_x; mq_gu_ = r.mq_gu; mq_h_ = r.mq_h; mq_hq_ = r.mq_hq;
  rows_ = r.rows;
}

bool FlintEngine::lend() {
  if (borrow_ && !lent_) {
    if (!cuda_ok(cudaStreamSynchronize(copy_), "expert swaps")) return false;
    land_swaps();
    for (int t = 0; t < kMoeLayers; ++t) {
      DevLayer& L = tier(t);
      for (int j = 0; j < L.n_slots; ++j) {
        const int s = L.slot0 + j, e = L.expert_in[static_cast<std::size_t>(j)];
        if (s < lend_slot_ || e < 0) continue;
        lent_experts_.emplace_back(s, e);
        L.expert_in[static_cast<std::size_t>(j)] = -1;
        L.slot_of[static_cast<std::size_t>(e)] = -1;
        if (!set_ptr(L, e, reinterpret_cast<unsigned long long>(L.host_alias + L.nl.bytes * static_cast<std::size_t>(e)))) return false;
        set_res(L, e, -1);
      }
    }
    lent_ = true;
    for (bool& d : ring_dirty_) d = true;
  }
  bind(big_);
  return true;
}

bool FlintEngine::give_back() {
  bind(small_);
  if (!lent_) return true;
  lent_ = false;
  const int S = tier(0).n_slots;
  const int N = kMoeLayers * S;
  const std::uint64_t at = cache_.slot_offset(lend_slot_);
  if (!cuda_ok(cudaMemsetAsync(cache_.device_slot(lend_slot_), 0, static_cast<std::size_t>(cache_.slot_offset(N) - at), st_), "lent slots") ||
      !cuda_ok(cudaEventRecord(evict_ev_, st_), "event") || !cuda_ok(cudaStreamWaitEvent(copy_, evict_ev_, 0), "event")) {
    return false;
  }
  for (const auto& [slot, held] : lent_experts_) {
    DevLayer& L = tier(slot / S);
    int e = held;
    if (L.slot_of[static_cast<std::size_t>(e)] != -1) {
      e = -1;
      for (int x = 0; x < kExperts; ++x)
        if (L.slot_of[static_cast<std::size_t>(x)] == -1 && (e < 0 || L.score[static_cast<std::size_t>(x)] > L.score[static_cast<std::size_t>(e)])) e = x;
      if (e < 0) continue;
    }
    Swap sw{L.tier, slot, e, nullptr};
    if (!free_events_.empty()) {
      sw.landed = free_events_.back();
      free_events_.pop_back();
    } else if (!cuda_ok(cudaEventCreateWithFlags(&sw.landed, cudaEventDisableTiming), "event")) {
      return false;
    }
    L.slot_of[static_cast<std::size_t>(e)] = -2;
    if (!cuda_ok(cudaMemcpyAsync(cache_.device_slot(slot), L.host_experts + L.nl.bytes * static_cast<std::size_t>(e), L.nl.bytes,
                                 cudaMemcpyHostToDevice, copy_),
                 "lent slots") ||
        !cuda_ok(cudaEventRecord(sw.landed, copy_), "event")) {
      return false;
    }
    swaps_.push_back(sw);
  }
  lent_experts_.clear();
  return true;
}

bool FlintEngine::start_lane(const EngineConfig& cfg, std::string& error) {
  void* res = nullptr;
  if (!cuda_ok(cudaHostAlloc(&res, sizeof(std::int32_t) * kMoeLayers * kExperts, cudaHostAllocDefault), "cudaHostAlloc")) {
    error = "cannot pin the CPU lane's residency table";
    return false;
  }
  pinned_.push_back(res);
  res_h_ = static_cast<std::int32_t*>(res);
  std::vector<CpuLane::Layer> lanes;
  for (int t = 0; t < kMoeLayers; ++t) {
    const DevLayer& L = tier(t);
    for (int e = 0; e < kExperts; ++e) {
      const int s = L.slot_of[static_cast<std::size_t>(e)];
      res_h_[static_cast<std::size_t>(t) * kExperts + static_cast<std::size_t>(e)] = s >= 0 ? s : -1;
    }
    lanes.push_back({L.host_experts, L.nl.bytes, L.nl.gu_type, L.nl.d_type});
  }
  if (!cuda_ok(cudaMemcpy(res_d_, res_h_, sizeof(std::int32_t) * kMoeLayers * kExperts, cudaMemcpyHostToDevice), "residency")) {
    error = "cannot initialise the CPU lane's residency table";
    return false;
  }
  const int n_slots = kMoeLayers * tier(0).n_slots;
  std::vector<std::uint64_t> offs(static_cast<std::size_t>(n_slots));
  const std::uint8_t* base = n_slots > 0 ? cache_.device_slot(0) : nullptr;
  for (int s = 0; s < n_slots; ++s) offs[static_cast<std::size_t>(s)] = static_cast<std::uint64_t>(cache_.device_slot(s) - base);
  const double frac = cfg.pcie_frac >= 0 ? cfg.pcie_frac : kDefaultPcieFrac;
  lane_ = std::make_unique<CpuLane>();
  if (!lane_->start(lanes, kEmbd, kFfExp, res_h_, base, std::move(offs), lstage_, lstage_bytes_,
                    static_cast<int>(std::lround(frac * 256.0)), cfg.cpu_threads, error)) {
    lane_.reset();
    return false;
  }
  report_.lane_workers = lane_->workers();
  return true;
}

// A decode step's routed experts on the paper's three tiers (3.3), as Gemma 4's: the doorbell publishes the routing and
// the input; the lane's plan names the VRAM groups, the staged groups and leaves the rest to the CPU pool.
bool FlintEngine::moe_lane(DevLayer& L) {
  namespace K = strata::kernels;
  using P = LanePlan;
  const CpuLane::Device& D = lane_->device();
  const auto ring = static_cast<std::uint32_t>(L.tier + 1);
  K::quantize_q8_1_rows(h_, 1, kEmbd, xq_, st_);
  K::doorbell_publish_res(h_, picks_, res_d_ + static_cast<std::size_t>(L.tier) * kExperts, kExperts, kEmbd, kPicks, D.x, D.ids,
                          D.seq, st_);
  K::wait_flag_ge(D.flag_a, ring, st_);
  K::copy_i32_from_mapped(lplan_, D.plan, P::kWords, st_);
  const auto* ptr = reinterpret_cast<const unsigned long long*>(lplan_ + P::kPtr);
  const auto* ptr2 = reinterpret_cast<const unsigned long long*>(lplan_ + P::kPtr2);
  K::native_expert_grouped(L.nl, ptr, lplan_ + P::kStart, lplan_ + P::kCounts, lplan_ + P::kDst, lplan_ + P::kTok, P::kCap, P::kCap,
                           xq_, escratch_, hit_out_, st_);
  K::wait_flag_ge(D.flag_b, ring, st_);
  K::native_expert_grouped(L.nl, ptr2, lplan_ + P::kStart2, lplan_ + P::kCounts + 2, lplan_ + P::kDst, lplan_ + P::kTok, P::kCap,
                           P::kCap, xq_, escratch_, hit_out_, st_);
  K::copy_rows_from_mapped_after(D.flag_c, ring, yg_, D.rows, kPicks, kEmbd, lplan_ + P::kDst, lplan_ + P::kCounts + 1, st_);
  K::moe_hit_add(yg_, hit_out_, lplan_ + P::kDst, lplan_ + P::kCounts + 1, P::kCap, kEmbd, st_);
  flk::moe_combine(yg_, wts_, kPicks, kEmbd, moe_, st_);
  return launch_ok("experts (CPU lane)");
}

bool FlintEngine::place_experts(double vram_gib, int forced_slots, const std::string& profile, std::string& error) {
  std::size_t free_b = 0, total_b = 0;
  if (!cuda_ok(cudaMemGetInfo(&free_b, &total_b), "cudaMemGetInfo")) {
    error = "cannot read the free VRAM";
    return false;
  }
  std::size_t per_slot = 0;
  const auto guard = static_cast<std::int64_t>(mmq_guard_);
  for (int t = 0; t < kMoeLayers; ++t)
    per_slot += static_cast<std::size_t>(strata::core::ExpertCache::slot_stride(static_cast<std::int64_t>(tier(t).nl.bytes), guard));
  const std::size_t reserve = std::size_t(640) << 20;
  const std::size_t budget = vram_gib >= 0 ? static_cast<std::size_t>(vram_gib * double(1ull << 30))
                                           : (free_b > reserve ? free_b - reserve : 0);
  int S = forced_slots > 0 ? forced_slots : static_cast<int>(std::min<std::size_t>(kExperts, budget / per_slot));
  if (S > kExperts) S = kExperts;
  if (static_cast<std::size_t>(S) * per_slot + (std::size_t(128) << 20) > free_b) {
    if (forced_slots > 0) {
      error = "--expert-slots does not fit in the free VRAM";
      return false;
    }
    S = static_cast<int>((free_b > (std::size_t(128) << 20) ? free_b - (std::size_t(128) << 20) : 0) / per_slot);
  }
  if (!alloc(counts_all_, static_cast<std::size_t>(kMoeLayers) * kExperts) ||
      !cuda_ok(cudaMemset(counts_all_, 0, sizeof(int) * kMoeLayers * kExperts), "counts")) {
    error = "cannot allocate the routing counts";
    return false;
  }
  for (int t = 0; t < kMoeLayers; ++t) tier(t).counts = counts_all_ + static_cast<std::size_t>(t) * kExperts;
  std::vector<std::int64_t> slot_bytes;
  for (int t = 0; t < kMoeLayers; ++t) slot_bytes.insert(slot_bytes.end(), static_cast<std::size_t>(S), static_cast<std::int64_t>(tier(t).nl.bytes));
  std::string cerr;
  cache_.set_slot_guard(guard);
  if (S > 0 && !cache_.open_sized(slot_bytes, kMoeLayers, kExperts, cerr)) {
    error = "cannot allocate the expert cache: " + cerr;
    return false;
  }
  std::vector<std::pair<int, int>> ranked;
  if (!profile.empty()) {
    std::int64_t pslots = 0;
    if (!strata::core::read_expert_profile(profile, kMoeLayers, kExperts, profile_prior_, pslots, cerr)) {
      error = cerr;
      return false;
    }
    for (const auto& [l, e] : profile_prior_) ranked.emplace_back(l, e);
    report_.profile_filled = true;
  }
  const std::vector<std::vector<int>> fill = fill_from_profile(ranked, S);
  for (int t = 0; t < kMoeLayers; ++t) {
    DevLayer& L = tier(t);
    L.n_slots = S;
    L.slot0 = t * S;
    L.slot_of.assign(kExperts, -1);
    L.expert_in.assign(static_cast<std::size_t>(S), -1);
    L.score.assign(kExperts, 0.0);
    L.ptr_host.resize(kExperts);
    for (int e = 0; e < kExperts; ++e)
      L.ptr_host[static_cast<std::size_t>(e)] = reinterpret_cast<unsigned long long>(L.host_alias + L.nl.bytes * static_cast<std::size_t>(e));
    for (int j = 0; j < S; ++j) {
      const int e = fill[static_cast<std::size_t>(t)][static_cast<std::size_t>(j)];
      const int slot = L.slot0 + j;
      if (!cache_.fill_slot_queued(slot, L.host_experts + L.nl.bytes * static_cast<std::size_t>(e), cerr,
                                   static_cast<std::int64_t>(L.nl.bytes))) {
        error = "cannot fill the expert slots: " + cerr;
        return false;
      }
      L.slot_of[static_cast<std::size_t>(e)] = slot;
      L.expert_in[static_cast<std::size_t>(j)] = e;
      L.ptr_host[static_cast<std::size_t>(e)] = reinterpret_cast<unsigned long long>(cache_.device_slot(slot));
    }
    if (!alloc(L.ptrtab, kExperts) ||
        !cuda_ok(cudaMemcpy(L.ptrtab, L.ptr_host.data(), sizeof(unsigned long long) * kExperts, cudaMemcpyHostToDevice), "table")) {
      return false;
    }
  }
  if (S > 0 && !cache_.sync_queued(cerr)) {
    error = "cannot fill the expert slots: " + cerr;
    return false;
  }
  report_.slots_per_layer = S;
  report_.resident_pct = 100.0 * S / kExperts;
  return true;
}

bool FlintEngine::load(const EngineConfig& cfg, const ModelDesc& m, std::string& error) {
  const auto t0 = Clock::now();
  files_ = std::vector<g4::ReadOnlyFile>(cfg.shards.size());
  for (std::size_t s = 0; s < cfg.shards.size(); ++s) {
    if (!files_[s].open(cfg.shards[s])) {
      error = "cannot open the model file";
      return false;
    }
  }
  ctx_ = cfg.max_context;
  chunk_ = cfg.chunk;
  graph_on_ = cfg.graph;
  n_run_ = std::clamp(cfg.layers, 1, kLayers);
  if (const char* sd = std::getenv("STRATA_FLINT_SEAMS"); sd != nullptr) seams_ = sd;
  ring_ = kWindow + std::max(cfg.chunk, kWindow);
  eps_ = m.rms_eps;
  rope_base_ = m.rope_base;
  w_scale_ = m.expert_weights_scale;
  attn_scale_ = 1.0f / std::sqrt(float(kHead));
  if (!cuda_ok(cudaStreamCreateWithFlags(&st_, cudaStreamNonBlocking), "stream") ||
      !cuda_ok(cudaStreamCreateWithFlags(&copy_, cudaStreamNonBlocking), "stream") ||
      !cuda_ok(cudaEventCreateWithFlags(&counts_ev_, cudaEventDisableTiming), "event") ||
      !cuda_ok(cudaEventCreateWithFlags(&evict_ev_, cudaEventDisableTiming), "event") || cublasCreate(&blas_) != CUBLAS_STATUS_SUCCESS ||
      cublasSetStream(blas_, st_) != CUBLAS_STATUS_SUCCESS) {
    error = "cannot initialise CUDA or cuBLAS";
    return false;
  }
  if (!upload_matrix(m.token_embd, embd_) || !upload_vector(m.output_norm, output_norm_) || !upload_matrix(m.output, output_)) {
    error = "cannot load the embedding or the head";
    return false;
  }
  layers_.resize(kLayers);
  std::size_t pinned = 0;
  for (int i = 0; i < kLayers; ++i) {
    const LayerDesc& d = m.layers[static_cast<std::size_t>(i)];
    DevLayer& L = layers_[static_cast<std::size_t>(i)];
    L.index = i;
    L.moe = d.moe;
    L.tier = d.moe ? i - kDenseLayers : -1;
    L.sliding = d.sliding;
    L.rope = d.rope;
    bool ok = upload_vector(d.attn_norm, L.attn_norm) && upload_matrix(d.attn_q, L.q) && upload_matrix(d.attn_k, L.k) &&
              upload_matrix(d.attn_v, L.v) && upload_matrix(d.attn_output, L.o) && upload_vector(d.attn_q_norm, L.q_norm) &&
              upload_vector(d.attn_k_norm, L.k_norm) && upload_vector(d.attn_sinks, L.sinks) && upload_vector(d.ffn_norm, L.ffn_norm);
    if (ok && !L.moe) {
      ok = upload_matrix(d.ffn_gate, L.gate) && upload_matrix(d.ffn_up, L.up) && upload_matrix(d.ffn_down, L.down);
    } else if (ok) {
      ok = upload_matrix(d.gate_inp, L.router) && upload_matrix(d.gate_shexp, L.gs) && upload_matrix(d.up_shexp, L.us) &&
           upload_matrix(d.down_shexp, L.ds) && load_experts(L, d);
      if (ok) pinned += L.nl.bytes * kExperts;
    }
    if (!ok) {
      error = "cannot load layer weights";
      return false;
    }
    L.cap = L.sliding ? ring_ : ctx_;
    const std::size_t cache = static_cast<std::size_t>(kKvHeads) * L.cap * kHead;
    if (!alloc(L.kc, cache) || !alloc(L.vc, cache)) {
      error = "cannot allocate the KV cache (lower --max-context)";
      return false;
    }
    if (i % 8 == 0 || i == kLayers - 1)
      std::fprintf(stderr, "multiengine-flint: layer %d loaded (%s, %s) at %.1f s\n", i, L.sliding ? "sliding" : "full",
                   L.moe ? "MoE" : "dense", std::chrono::duration<double>(Clock::now() - t0).count());
  }
  staging_.clear();
  staging_.shrink_to_fit();
  const std::size_t C = static_cast<std::size_t>(chunk_);
  max_splits_ = flk::attn_decode_splits(ctx_);
  const bool partial = n_run_ < kLayers;   // test-only: the plain paths (no CPU lane, no expert stream)
  // P5: MMQ for the prompt's experts on the layers whose types it has (Q8_0), and the stream's geometry
  std::size_t mmq_blob = 0;
  const bool no_mmq = std::getenv("STRATA_FLINT_NO_MMQ") != nullptr || partial;
  if (!no_mmq && MMQ::built() && MMQ::supported(8) && MMQ::fits(8, kGuRows) && MMQ::fits(8, kEmbd)) {
    for (int t = 0; t < kMoeLayers; ++t) {
      DevLayer& L = tier(t);
      L.mmq = L.nl.gu_type == 8 && L.nl.d_type == 8;
      if (L.mmq) mmq_blob = std::max(mmq_blob, L.nl.bytes);
    }
  }
  mmq_on_ = mmq_blob > 0 && chunk_ >= kMmqMinRows;
  if (mmq_on_) {
    constexpr std::size_t kBlockGrid = 4352;   // lcm(34, 256): MMQ steps from expert to expert in Q8_0 blocks
    const std::size_t over = std::max(MMQ::overread_bytes(8, kEmbd), MMQ::overread_bytes(8, kFfExp));
    mmq_guard_ = (mmq_blob + over + kBlockGrid - 1) / kBlockGrid * kBlockGrid - mmq_blob;
    auto stride_of = [&](std::size_t bytes) {
      return static_cast<std::size_t>(strata::core::ExpertCache::slot_stride(static_cast<std::int64_t>(bytes), static_cast<std::int64_t>(mmq_guard_)));
    };
    std::size_t widest = 0;
    for (int t = 0; t < kMoeLayers; ++t) {
      DevLayer& L = tier(t);
      L.stream_stride = stride_of(L.nl.bytes);
      widest = std::max(widest, L.stream_stride);
    }
    ring_buf_bytes_ = std::max(std::size_t(kRingGroup) * stride_of(mmq_blob), widest);
    for (int t = 0; t < kMoeLayers; ++t)
      tier(t).stream_group = static_cast<int>(std::min<std::size_t>(kRingGroup, ring_buf_bytes_ / tier(t).stream_stride));
  }
  borrow_ = mmq_on_ && cfg.prompt_borrow;
  constexpr std::size_t kBigScores = std::size_t(32) << 20;
  const std::size_t small_rows = borrow_ ? std::size_t(kMmqMinRows - 1) : C;
  const std::size_t small_scores = borrow_ ? std::max<std::size_t>(std::size_t(4) << 20, std::size_t(8) * static_cast<std::size_t>(ctx_))
                                           : kBigScores;
  std::uint8_t* small_mem = nullptr;
  const std::size_t small_bytes = layout_rows(nullptr, small_rows, small_scores, mmq_on_ && !borrow_, small_);
  const std::size_t wdq = std::max<std::size_t>(std::size_t(kFf) * kEmbd, std::size_t(kHeads) * kHead * kEmbd);
  const bool scratch_ok =
      alloc(small_mem, small_bytes) && alloc(ids_d_, std::max<std::size_t>(C, 2)) && alloc(wts_, C * kPicks) && alloc(picks_, C * kPicks) &&
      alloc(wdq_, wdq) && alloc(plan_ptr_, kExperts) && alloc(plan_start_, kExperts + 1) && alloc(plan_n_, 1) &&
      alloc(plan_dst_, C * kPicks) && alloc(plan_tok_, C * kPicks) && alloc(ident_, C * kPicks) &&
      alloc(dscratch_, std::size_t(kHeads) * max_splits_ * (kHead + 2)) && alloc(logits_, kVocab) && alloc(xn_, kEmbd) &&
      alloc(argmax_out_, 1) && alloc(argmax_scratch_, 4096) && alloc(topk_scratch_, flk::topk_scratch_bytes(kVocab)) &&
      alloc(cand_v_, flk::kTopkMax) && alloc(cand_i_, flk::kTopkMax) && alloc(cand_flag_, 1);
  if (!scratch_ok) {
    error = "cannot allocate the scratch buffers (lower --chunk or --max-context)";
    return false;
  }
  layout_rows(small_mem, small_rows, small_scores, mmq_on_ && !borrow_, small_);
  if (small_.ring != nullptr && !cuda_ok(cudaMemset(small_.ring, 0, std::size_t(kRingBufs) * ring_buf_bytes_), "ring")) {
    error = "cannot initialise the expert stream";
    return false;
  }
  big_ = small_;
  bind(small_);
  void* pin = nullptr;
  // ids (2C), picks (6C), the prompt plan (n, start[257], dst 6C, tok 6C), argmax, then the routing counts
  const std::size_t pin_ints = 2 * C + kPicks * C + (1 + kExperts + 1 + 2 * kPicks * C) + 16 + static_cast<std::size_t>(kMoeLayers) * kExperts;
  if (!cuda_ok(cudaHostAlloc(&pin, pin_ints * sizeof(int), cudaHostAllocDefault), "cudaHostAlloc")) {
    error = "cannot pin the transfer buffers";
    return false;
  }
  pinned_.push_back(pin);
  ids_h_ = static_cast<int*>(pin);
  picks_h_ = ids_h_ + 2 * C;
  plan_h_ = picks_h_ + kPicks * C;
  argmax_h_ = plan_h_ + (1 + kExperts + 1 + 2 * kPicks * C);
  counts_h_ = argmax_h_ + 16;
  void* ppin = nullptr;
  if (!cuda_ok(cudaHostAlloc(&ppin, sizeof(unsigned long long) * kExperts, cudaHostAllocDefault), "cudaHostAlloc")) {
    error = "cannot pin the transfer buffers";
    return false;
  }
  pinned_.push_back(ppin);
  plan_ptr_h_ = static_cast<unsigned long long*>(ppin);
  {
    std::vector<int> ident(C * kPicks);
    std::iota(ident.begin(), ident.end(), 0);
    if (!cuda_ok(cudaMemcpy(ident_, ident.data(), ident.size() * sizeof(int), cudaMemcpyHostToDevice), "ident")) {
      error = "cannot initialise the expert plan";
      return false;
    }
  }
  void* cand_pin = nullptr;
  if (!cuda_ok(cudaHostAlloc(&cand_pin, sizeof(int) * (1 + 2 * flk::kTopkMax), cudaHostAllocDefault), "cudaHostAlloc")) {
    error = "cannot pin the transfer buffers";
    return false;
  }
  pinned_.push_back(cand_pin);
  cand_h_ = static_cast<int*>(cand_pin);
  if (mmq_on_) {
    const std::size_t A = C * kPicks;
    void* hp = nullptr;
    void* rgp = nullptr;
    bool ok = alloc(mq_bounds_, kMqBoundsInts) && alloc(mq_ids_, 2 * A) &&
              cuda_ok(cudaHostAlloc(&hp, sizeof(int) * (kMqBoundsInts + 2 * A), cudaHostAllocDefault), "cudaHostAlloc") &&
              alloc(rg_d_, std::size_t(kRgCalls) * kRgCallInts) &&
              cuda_ok(cudaHostAlloc(&rgp, sizeof(int) * kRgCalls * kRgCallInts, cudaHostAllocDefault), "cudaHostAlloc") &&
              cuda_ok(cudaStreamCreateWithFlags(&ring_st_, cudaStreamNonBlocking), "stream");
    if (hp != nullptr) pinned_.push_back(hp);
    if (rgp != nullptr) pinned_.push_back(rgp);
    mq_pin_ = static_cast<int*>(hp);
    rg_h_ = static_cast<int*>(rgp);
    for (int k = 0; ok && k < kRingBufs; ++k)
      ok = cuda_ok(cudaEventCreateWithFlags(&ring_landed_[k], cudaEventDisableTiming), "event") &&
           cuda_ok(cudaEventCreateWithFlags(&ring_freed_[k], cudaEventDisableTiming), "event") &&
           cuda_ok(cudaEventRecord(ring_freed_[k], st_), "event");
    if (!ok) {
      error = "cannot allocate the prompt's MMQ buffers";
      return false;
    }
    mctx_ = std::make_unique<MMQ::Context>();
    miss_.assign(kMoeLayers, {});
  }
  const bool lane = cfg.cpu_lane && !partial;
  if (lane) {
    std::size_t max_blob = 0;
    for (int t = 0; t < kMoeLayers; ++t) max_blob = std::max(max_blob, tier(t).nl.bytes);
    lstage_bytes_ = 4 * max_blob;
    if (!alloc(lstage_, lstage_bytes_) || !alloc(lplan_, LanePlan::kWords) || !alloc(hit_out_, std::size_t(LanePlan::kCap) * kEmbd) ||
        !alloc(res_d_, static_cast<std::size_t>(kMoeLayers) * kExperts)) {
      error = "cannot allocate the CPU lane's buffers";
      return false;
    }
  }
  profile_save_ = cfg.expert_profile_save;
  profile_saved_ = Clock::now();
  if (!place_experts(cfg.expert_vram_gib, cfg.expert_slots, cfg.expert_profile, error)) return false;
  if (borrow_) {
    const std::size_t big_bytes = layout_rows(nullptr, C, kBigScores, true, big_);
    const int N = kMoeLayers * tier(0).n_slots;
    const std::uint64_t end = N > 0 ? cache_.slot_offset(N) : 0;
    int s0 = N;
    while (s0 > 0 && end - cache_.slot_offset(s0) < big_bytes) --s0;
    if (N > 0 && end - cache_.slot_offset(s0) >= big_bytes && 2 * (N - s0) <= N) {
      lend_slot_ = s0;
      layout_rows(cache_.device_slot(s0), C, kBigScores, true, big_);
      report_.lent_slots = N - s0;
    } else {
      std::uint8_t* own = nullptr;
      if (!alloc(own, big_bytes) || !cuda_ok(cudaMemset(own, 0, big_bytes), "prompt buffers")) {
        error = "the expert cache cannot lend the prompt's buffers and there is no VRAM for them (lower --chunk)";
        return false;
      }
      layout_rows(own, C, kBigScores, true, big_);
      borrow_ = false;
    }
  }
  if (lane && report_.slots_per_layer < kExperts && !start_lane(cfg, error)) return false;
  std::size_t free_b = 0, total_b = 0;
  cudaMemGetInfo(&free_b, &total_b);
  report_.vram_used_mib = double(total_b - free_b) / double(1 << 20);
  report_.vram_total_mib = double(total_b) / double(1 << 20);
  report_.pinned_gib = double(pinned) / double(1ull << 30);
  report_.ring_rows = ring_;
  report_.load_seconds = std::chrono::duration<double>(Clock::now() - t0).count();
  for (auto& f : files_) f.close();
  return !broken_;
}

bool FlintEngine::sgemm(int rows, int T, int cols, const float* w, const float* x, float* y) {
  const float one = 1.0f, zero = 0.0f;
  // row-major y[T][rows] = x[T][cols] * w[rows][cols]^T  ==  column-major y^T = w^T(op T) * x^T
  return cublasSgemm(blas_, CUBLAS_OP_T, CUBLAS_OP_N, rows, T, cols, &one, w, cols, x, cols, &zero, y, rows) == CUBLAS_STATUS_SUCCESS;
}

bool FlintEngine::gemm(const flk::Mat& w, const float* x, float* y, int T) {
  const float* wf = static_cast<const float*>(w.w);
  if (w.kind != flk::WKind::f32) {
    flk::dequant(w, wdq_, st_);
    wf = wdq_;
  }
  if (!sgemm(w.rows, T, w.cols, wf, x, y)) {
    std::fprintf(stderr, "multiengine-flint: cuBLAS SGEMM failed\n");
    broken_ = true;
    return false;
  }
  return true;
}

void FlintEngine::dump_layer(int layer, int pos0, int T) {
  if (sink_ == nullptr) return;
  const std::size_t need = static_cast<std::size_t>(pos0 + T) * kLayers * kEmbd;
  if (sink_->size() < need) sink_->resize(need, 0.0f);
  std::vector<float> rows(static_cast<std::size_t>(T) * kEmbd);
  cudaMemcpyAsync(rows.data(), x_, rows.size() * 4, cudaMemcpyDeviceToHost, st_);
  cudaStreamSynchronize(st_);
  for (int t = 0; t < T; ++t) {
    std::memcpy(sink_->data() + (static_cast<std::size_t>(pos0 + t) * kLayers + layer) * kEmbd,
                rows.data() + static_cast<std::size_t>(t) * kEmbd, kEmbd * 4);
  }
}

bool FlintEngine::head(const float* x_row) {
  flk::rmsnorm(x_row, output_norm_, xn_, 1, kEmbd, eps_, st_);
  flk::matvec(output_, xn_, logits_, 1, st_);
  return launch_ok("head");
}

// The dense SwiGLU of layers 0-1 on h_ into ffn_ (T rows; batch: cuBLAS on dequantised weights, else the matvec).
bool FlintEngine::ffn_dense(DevLayer& L, int T, bool batch) {
  if (batch) {
    if (!gemm(L.gate, h_, g_, T) || !gemm(L.up, h_, u_, T)) return false;
  } else {
    flk::matvec(L.gate, h_, g_, T, st_);
    flk::matvec(L.up, h_, u_, T, st_);
  }
  flk::swiglu(g_, kFf, u_, kFf, m_, T, kFf, st_);
  if (batch) return gemm(L.down, m_, ffn_, T);
  flk::matvec(L.down, m_, ffn_, T, st_);
  return launch_ok("dense FFN");
}

// The shared expert (width 1024) on h_ into ffn_, the same way.
bool FlintEngine::ffn_shared(DevLayer& L, int T, bool batch) {
  if (batch) {
    if (!gemm(L.gs, h_, g_, T) || !gemm(L.us, h_, u_, T)) return false;
  } else {
    flk::matvec(L.gs, h_, g_, T, st_);
    flk::matvec(L.us, h_, u_, T, st_);
  }
  flk::swiglu(g_, kFfShexp, u_, kFfShexp, m_, T, kFfShexp, st_);
  if (batch) return gemm(L.ds, m_, ffn_, T);
  flk::matvec(L.ds, m_, ffn_, T, st_);
  return launch_ok("shared expert");
}

// One decode step (paper 3.6, finding 7): the whole token as ONE captured graph, replayed every step; the token and its
// position are device data (ids_h_[0..1]), so a graph serves every position whose attention splits it was captured with.
bool FlintEngine::forward_one(std::int32_t id) {
  const int pos = pos_;
  if (!cuda_ok(cudaStreamSynchronize(st_), "decode")) return false;
  if (!swaps_.empty()) land_swaps();
  const bool lane = lane_ && lane_->running();
  if (lane) {
    if (res_dirty_) {
      if (!cuda_ok(cudaMemcpyAsync(res_d_, res_h_, sizeof(std::int32_t) * kMoeLayers * kExperts, cudaMemcpyHostToDevice, st_), "residency"))
        return false;
      res_dirty_ = false;
    }
  }
  ids_h_[0] = id;
  ids_h_[1] = pos;
  const int split_full = flk::attn_decode_splits(flk::attn_decode_keys(pos, 0));
  const int split_swa = flk::attn_decode_splits(flk::attn_decode_keys(pos, kWindow));
  const bool graph = graph_on_ && sink_ == nullptr;
  if (graph) {
    const int key = split_full * 64 + split_swa;
    if (!graph_ || graph_key_ != key) {
      graph_ = std::make_unique<strata::core::CapturedGraph>();
      std::string why;
      bool ok = graph_->begin(st_, why);
      if (ok) {
        const bool body = decode_body(pos, lane, split_full, split_swa);
        (void) cudaGetLastError();
        ok = graph_->end(st_, why) && body;
      }
      if (!ok) {
        std::fprintf(stderr, "multiengine-flint: the decode graph could not be captured (%s); plain launches from here\n", why.c_str());
        graph_.reset();
        graph_on_ = false;
        (void) cudaGetLastError();
        if (broken_) return false;
        return forward_one(id);
      }
      graph_key_ = key;
    }
    if (lane) lane_->begin_token();
    std::string why;
    if (!graph_->launch(st_, why)) {
      std::fprintf(stderr, "multiengine-flint: %s\n", why.c_str());
      if (lane) lane_->abort_token();
      broken_ = true;
      return false;
    }
  } else {
    if (lane) lane_->begin_token();
    if (!decode_body(pos, lane, split_full, split_swa)) {
      if (lane) lane_->abort_token();
      return false;
    }
  }
  if (lane) {
    std::string why;
    if (!cuda_ok(cudaStreamSynchronize(st_), "decode")) {
      lane_->abort_token();
      return false;
    }
    if (!lane_->end_token(why)) {
      std::fprintf(stderr, "multiengine-flint: %s\n", why.c_str());
      broken_ = true;
      return false;
    }
  }
  pos_ = pos + 1;
  return true;
}

// The step's launches (captured by forward_one, or run as they are): only device data changes between steps.
bool FlintEngine::decode_body(int pos, bool lane, int split_full, int split_swa) {
  (void) pos;
  const int* dpos = ids_d_ + 1;
  cudaMemcpyAsync(ids_d_, ids_h_, 2 * sizeof(int), cudaMemcpyHostToDevice, st_);
  flk::embed(ids_d_, 1, embd_, x_, st_);
  for (int l = 0; l < n_run_; ++l) {
    DevLayer& L = layers_[static_cast<std::size_t>(l)];
    flk::rmsnorm(x_, L.attn_norm, h_, 1, kEmbd, eps_, st_);
    flk::matvec(L.q, h_, qraw_, 1, st_);
    flk::matvec(L.k, h_, kraw_, 1, st_);
    flk::matvec(L.v, h_, vraw_, 1, st_);
    flk::QkvPost p{qraw_, kraw_, vraw_, L.q_norm, L.k_norm, L.rope, rope_base_, 0, 1, L.sliding ? ring_ : 0, L.cap, eps_,
                   qout_, nullptr, L.kc, L.vc, 0.0f, dpos};
    flk::qkv_post(p, st_);
    flk::attn_decode_dev(qout_, L.kc, L.vc, L.cap, L.sliding ? ring_ : 0, L.sliding ? kWindow : 0, dpos, L.sliding ? split_swa : split_full,
                         L.sinks, attn_scale_, attn_, dscratch_, max_splits_, st_);
    flk::matvec(L.o, attn_, o_, 1, st_);
    flk::add_rows(x_, o_, nullptr, x1_, 1, kEmbd, st_);
    flk::rmsnorm(x1_, L.ffn_norm, h_, 1, kEmbd, eps_, st_);
    if (!L.moe) {
      if (!ffn_dense(L, 1, false)) return false;
      flk::add_rows(x1_, ffn_, nullptr, x_, 1, kEmbd, st_);
    } else {
      flk::matvec(L.router, h_, rl_, 1, st_);
      flk::router_topk(rl_, 1, picks_, wts_, L.counts, w_scale_, st_);
      if (!(lane ? moe_lane(L) : moe_experts(L, 1))) return false;
      if (!ffn_shared(L, 1, false)) return false;
      flk::add_rows(x1_, moe_, ffn_, x_, 1, kEmbd, st_);
    }
    if (sink_ != nullptr) dump_layer(l, pos_, 1);
  }
  return head(x_);
}

bool FlintEngine::prompt_attention(DevLayer& L, int pos0, int T) {
  const int ring = L.sliding ? ring_ : 0;
  const int Lk = L.sliding ? std::min(ring_, pos0 + T) : pos0 + T;
  constexpr int G = kHeads / kKvHeads;
  const int Ts = static_cast<int>(std::max<std::size_t>(1, std::min<std::size_t>(T, s_floats_ / (std::size_t(G) * Lk))));
  const float one = 1.0f, zero = 0.0f;
  for (int g = 0; g < kKvHeads; ++g) {
    const __half* K = L.kc + static_cast<std::size_t>(g) * L.cap * kHead;
    const __half* V = L.vc + static_cast<std::size_t>(g) * L.cap * kHead;
    for (int t0 = 0; t0 < T; t0 += Ts) {
      const int ts = std::min(Ts, T - t0);
      const __half* Q = q16_ + (static_cast<std::size_t>(t0) * kHeads + static_cast<std::size_t>(g) * G) * kHead;
      float* O = attn_ + (static_cast<std::size_t>(t0) * kHeads + static_cast<std::size_t>(g) * G) * kHead;
      if (cublasGemmStridedBatchedEx(blas_, CUBLAS_OP_T, CUBLAS_OP_N, Lk, ts, kHead, &one, K, CUDA_R_16F, kHead, 0, Q, CUDA_R_16F,
                                     kHeads * kHead, kHead, &zero, s_buf_, CUDA_R_32F, Lk, static_cast<long long>(ts) * Lk, G,
                                     CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT) != CUBLAS_STATUS_SUCCESS) {
        std::fprintf(stderr, "multiengine-flint: cuBLAS attention scores failed\n");
        broken_ = true;
        return false;
      }
      flk::attn_softmax(s_buf_, p_buf_, G, ts, Lk, pos0 + t0, ring, L.sliding ? kWindow : 0, pos0 + T - 1,
                        L.sinks + static_cast<std::size_t>(g) * G, attn_scale_, st_);
      if (cublasGemmStridedBatchedEx(blas_, CUBLAS_OP_N, CUBLAS_OP_N, kHead, ts, Lk, &one, V, CUDA_R_16F, kHead, 0, p_buf_, CUDA_R_16F,
                                     Lk, static_cast<long long>(ts) * Lk, &zero, O, CUDA_R_32F, kHeads * kHead, kHead, G,
                                     CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT) != CUBLAS_STATUS_SUCCESS) {
        std::fprintf(stderr, "multiengine-flint: cuBLAS attention output failed\n");
        broken_ = true;
        return false;
      }
    }
  }
  return launch_ok("prompt attention");
}

// The layer's routed experts for T tokens through Strata's grouped expert kernel on the native blobs (as Gemma 4's):
// the input h_ as q8_1 blocks, one group per expert with entries, each group's blob read where the expert lives.
bool FlintEngine::moe_experts(DevLayer& L, int T) {
  const int A = T * kPicks;
  strata::kernels::quantize_q8_1_rows(h_, T, kEmbd, xq_, st_);
  int groups = kPicks;
  if (T == 1) {
    flk::plan_decode(picks_, kPicks, L.ptrtab, plan_ptr_, plan_start_, plan_n_, plan_dst_, plan_tok_, st_);
  } else {
    if (!cuda_ok(cudaMemcpyAsync(picks_h_, picks_, sizeof(int) * A, cudaMemcpyDeviceToHost, st_), "picks") ||
        !cuda_ok(cudaStreamSynchronize(st_), "sync")) {
      return false;
    }
    int count[kExperts] = {};
    for (int a = 0; a < A; ++a) ++count[picks_h_[a]];
    int* start = plan_h_ + 1;
    int* dst = start + (kExperts + 1);
    int* tok = dst + A;
    groups = 0;
    int at = 0;
    int first_entry[kExperts];
    for (int e = 0; e < kExperts; ++e) {
      first_entry[e] = at;
      if (count[e] == 0) continue;
      plan_ptr_h_[groups] = L.ptr_host[static_cast<std::size_t>(e)];
      start[groups++] = at;
      at += count[e];
    }
    start[groups] = at;
    plan_h_[0] = groups;
    for (int a = 0; a < A; ++a) {
      const int i = first_entry[picks_h_[a]]++;
      dst[i] = a;
      tok[i] = a / kPicks;
    }
    if (!cuda_ok(cudaMemcpyAsync(plan_n_, plan_h_, sizeof(int), cudaMemcpyHostToDevice, st_), "plan") ||
        !cuda_ok(cudaMemcpyAsync(plan_start_, start, sizeof(int) * (groups + 1), cudaMemcpyHostToDevice, st_), "plan") ||
        !cuda_ok(cudaMemcpyAsync(plan_dst_, dst, sizeof(int) * A, cudaMemcpyHostToDevice, st_), "plan") ||
        !cuda_ok(cudaMemcpyAsync(plan_tok_, tok, sizeof(int) * A, cudaMemcpyHostToDevice, st_), "plan") ||
        !cuda_ok(cudaMemcpyAsync(plan_ptr_, plan_ptr_h_, sizeof(unsigned long long) * groups, cudaMemcpyHostToDevice, st_), "plan")) {
      return false;
    }
  }
  strata::kernels::native_expert_grouped(L.nl, plan_ptr_, plan_start_, plan_n_, plan_dst_, plan_tok_, groups, A, xq_, escratch_, yg_, st_);
  if (T == 1) flk::moe_combine(yg_, wts_, kPicks, kEmbd, moe_, st_);
  else flk::scatter_combine(yg_, ident_, wts_, T, kEmbd, moe_, st_);
  return launch_ok("experts");
}

bool FlintEngine::ring_begin() {
  ring_seq_.clear();
  ring_next_ = 0;
  for (int t = 0; t < kMoeLayers; ++t) {
    const DevLayer& L = tier(t);
    auto& m = miss_[static_cast<std::size_t>(t)];
    m.clear();
    for (int e = 0; e < kExperts; ++e)
      if (L.slot_of[static_cast<std::size_t>(e)] < 0) m.push_back(e);
    for (int f = 0; f < static_cast<int>(m.size()); f += L.stream_group)
      ring_seq_.push_back({t, f, std::min(L.stream_group, static_cast<int>(m.size()) - f)});
  }
  for (std::size_t i = 0; i < ring_seq_.size() && i < std::size_t(kRingBufs); ++i)
    if (!ring_kick(i)) return false;
  return true;
}

bool FlintEngine::ring_kick(std::size_t i) {
  const RingGroup& g = ring_seq_[i];
  const std::size_t k = i % kRingBufs;
  const DevLayer& L = tier(g.tier);
  if (!cuda_ok(cudaStreamWaitEvent(ring_st_, ring_freed_[k], 0), "event")) return false;
  std::uint8_t* dst = ring_buf_ + k * ring_buf_bytes_;
  if (L.mmq && ring_dirty_[k]) {
    if (!cuda_ok(cudaMemsetAsync(dst, 0, ring_buf_bytes_, ring_st_), "expert stream")) return false;
    ring_dirty_[k] = false;
  }
  if (!L.mmq) ring_dirty_[k] = true;
  for (int j = 0; j < g.n; ++j) {
    const int e = miss_[static_cast<std::size_t>(g.tier)][static_cast<std::size_t>(g.first + j)];
    if (!cuda_ok(cudaMemcpyAsync(dst + static_cast<std::size_t>(j) * L.stream_stride, L.host_experts + L.nl.bytes * static_cast<std::size_t>(e),
                                 L.nl.bytes, cudaMemcpyHostToDevice, ring_st_),
                 "expert stream"))
      return false;
  }
  return cuda_ok(cudaEventRecord(ring_landed_[k], ring_st_), "event");
}

const std::uint8_t* FlintEngine::ring_take(const DevLayer& L, int g) {
  const std::size_t i = ring_next_;
  if (i >= ring_seq_.size() || ring_seq_[i].tier != L.tier || ring_seq_[i].first != g * L.stream_group) {
    std::fprintf(stderr, "multiengine-flint: the expert stream is out of step (layer %d)\n", L.index);
    broken_ = true;
    return nullptr;
  }
  const std::size_t k = i % kRingBufs;
  if (!cuda_ok(cudaStreamWaitEvent(st_, ring_landed_[k], 0), "event")) return nullptr;
  return ring_buf_ + k * ring_buf_bytes_;
}

bool FlintEngine::ring_give() {
  const std::size_t i = ring_next_++;
  if (!cuda_ok(cudaEventRecord(ring_freed_[i % kRingBufs], st_), "event")) return false;
  return i + kRingBufs >= ring_seq_.size() || ring_kick(i + kRingBufs);
}

int FlintEngine::stream_plan(const DevLayer& L, int A, int* bounds, int* inv, int* ent, int* tok) {
  const int S = L.n_slots;
  const auto& miss = miss_[static_cast<std::size_t>(L.tier)];
  const int M = static_cast<int>(miss.size());
  const int P = S + M;
  int pos_of[kExperts];
  std::fill(pos_of, pos_of + kExperts, -1);
  for (int j = 0; j < S; ++j) {
    const int e = L.expert_in[static_cast<std::size_t>(j)];
    if (e >= 0) pos_of[e] = j;
  }
  for (int q = 0; q < M; ++q) pos_of[miss[static_cast<std::size_t>(q)]] = S + q;
  std::fill(bounds, bounds + P + 1, 0);
  for (int a = 0; a < A; ++a) {
    const int p = pos_of[picks_h_[a]];
    if (p < 0) {
      std::fprintf(stderr, "multiengine-flint: a routed expert has no position in the prompt's plan\n");
      broken_ = true;
      return -1;
    }
    ++bounds[p + 1];
  }
  for (int p = 0; p < P; ++p) bounds[p + 1] += bounds[p];
  int cur[kExperts + kExperts + 1];
  std::copy(bounds, bounds + P, cur);
  for (int a = 0; a < A; ++a) {
    const int r = cur[pos_of[picks_h_[a]]]++;
    if (inv != nullptr) inv[a] = r;
    if (ent != nullptr) ent[r] = a;
    if (tok != nullptr) tok[r] = a / kPicks;
  }
  return P;
}

// A streamed prompt chunk's experts on a layer MMQ has no kernel for (BF16): Strata's grouped kernel, from the slots
// and then from each streamed group once it has landed.
bool FlintEngine::moe_ring_grouped(DevLayer& L, int T) {
  const int A = T * kPicks;
  if (!cuda_ok(cudaMemcpyAsync(picks_h_, picks_, sizeof(int) * A, cudaMemcpyDeviceToHost, st_), "picks") ||
      !cuda_ok(cudaStreamSynchronize(st_), "sync")) {
    return false;
  }
  int bounds[kExperts + kExperts + 1];
  int* dst = plan_h_ + 1 + (kExperts + 1);
  int* tok = dst + A;
  const int P = stream_plan(L, A, bounds, nullptr, dst, tok);
  if (P < 0) return false;
  const int S = L.n_slots, G = L.stream_group;
  const int calls = 1 + (P - S + G - 1) / G;
  for (int c = 0; c < calls; ++c) {
    const int p0 = c == 0 ? 0 : S + (c - 1) * G, p1 = c == 0 ? S : std::min(P, p0 + G);
    int* w = rg_h_ + static_cast<std::size_t>(c) * kRgCallInts;
    auto* ptr = reinterpret_cast<unsigned long long*>(w + kRgPtr);
    const std::uint8_t* buf = c == 0 ? nullptr : ring_buf_ + ((ring_next_ + static_cast<std::size_t>(c - 1)) % kRingBufs) * ring_buf_bytes_;
    int n = 0;
    for (int p = p0; p < p1; ++p) {
      if (bounds[p + 1] == bounds[p]) continue;
      w[kRgStart + n] = bounds[p];
      ptr[n] = reinterpret_cast<unsigned long long>(c == 0 ? cache_.device_slot(L.slot0 + p)
                                                           : buf + static_cast<std::size_t>(p - p0) * L.stream_stride);
      ++n;
    }
    w[0] = n;
    w[kRgStart + n] = bounds[p1];
  }
  if (!cuda_ok(cudaMemcpyAsync(rg_d_, rg_h_, sizeof(int) * kRgCallInts * static_cast<std::size_t>(calls), cudaMemcpyHostToDevice, st_), "plan") ||
      !cuda_ok(cudaMemcpyAsync(plan_dst_, dst, sizeof(int) * A, cudaMemcpyHostToDevice, st_), "plan") ||
      !cuda_ok(cudaMemcpyAsync(plan_tok_, tok, sizeof(int) * A, cudaMemcpyHostToDevice, st_), "plan")) {
    return false;
  }
  strata::kernels::quantize_q8_1_rows(h_, T, kEmbd, xq_, st_);
  for (int c = 0; c < calls; ++c) {
    if (c > 0 && ring_take(L, c - 1) == nullptr) return false;
    const int n = rg_h_[static_cast<std::size_t>(c) * kRgCallInts];
    const int* w = rg_d_ + static_cast<std::size_t>(c) * kRgCallInts;
    if (n > 0)
      strata::kernels::native_expert_grouped(L.nl, reinterpret_cast<const unsigned long long*>(w + kRgPtr), w + kRgStart, w, plan_dst_,
                                             plan_tok_, n, A, xq_, escratch_, yg_, st_);
    if (c > 0 && !ring_give()) return false;
  }
  flk::scatter_combine(yg_, ident_, wts_, T, kEmbd, moe_, st_);
  return launch_ok("experts (streamed)");
}

// A prompt chunk's routed experts on llama.cpp's MMQ (as Gemma 4's, for flint's blobs and SwiGLU): the entries sorted by
// their expert's position - its slot for a resident expert, else after the slots in the order the misses stream - then
// for the slots and for each streamed group: gate+up as one [1024] product per expert, SwiGLU, the hidden quantized,
// down into the entry's row of yg_, and the combine.
bool FlintEngine::moe_mmq(DevLayer& L, int T) {
  const int A = T * kPicks;
  if (!cuda_ok(cudaMemcpyAsync(picks_h_, picks_, sizeof(int) * A, cudaMemcpyDeviceToHost, st_), "picks") ||
      !cuda_ok(cudaStreamSynchronize(st_), "sync")) {
    return false;
  }
  const int S = L.n_slots, G = L.stream_group;
  int* bounds = mq_pin_;
  int* inv = mq_pin_ + kMqBoundsInts;
  int* tok = inv + A;
  const int P = stream_plan(L, A, bounds, inv, nullptr, tok);
  if (P < 0) return false;
  const int M = P - S;
  int* rel = bounds + (P + 1);
  int most_slots = 0;
  for (int p = 0; p < S; ++p) most_slots = std::max(most_slots, bounds[p + 1] - bounds[p]);
  const int groups = (M + G - 1) / G;
  for (int g = 0; g < groups; ++g) {
    const int f = g * G, n = std::min(G, M - f);
    for (int j = 0; j <= n; ++j) rel[g * (kRingGroup + 1) + j] = bounds[S + f + j] - bounds[S + f];
  }
  const int words = P + 1 + groups * (kRingGroup + 1);
  if (words > kMqBoundsInts) {
    std::fprintf(stderr, "multiengine-flint: the prompt's expert plan does not fit its table\n");
    broken_ = true;
    return false;
  }
  if (!cuda_ok(cudaMemcpyAsync(mq_bounds_, bounds, sizeof(int) * static_cast<std::size_t>(words), cudaMemcpyHostToDevice, st_), "plan") ||
      !cuda_ok(cudaMemcpyAsync(mq_ids_, inv, sizeof(int) * static_cast<std::size_t>(2 * A), cudaMemcpyHostToDevice, st_), "plan")) {
    return false;
  }
  const int* inv_d = mq_ids_;
  const int* tok_d = mq_ids_ + A;
  MMQ::quantize(h_, tok_d, mq_x_, 8, kEmbd, kEmbd, A, st_);   // (8: the Q8_0 weights' ggml type)
  auto experts = [&](const std::uint8_t* w, std::size_t stride, int n, const int* abs_bounds, const int* rel_bounds, int r0, int r1,
                     int most) {
    if (r1 <= r0) return;
    MMQ::Product gu;
    gu.w = w; gu.type = 8; gu.w_rows = kGuRows; gu.w_cols = kEmbd; gu.expert_bytes = stride; gu.n = n;
    gu.xq = mq_x_; gu.bounds = abs_bounds; gu.ids = ident_; gu.total_rows = A; gu.max_rows = most;
    gu.dst = mq_gu_; gu.ld_dst = kGuRows;
    mctx_->run(gu, st_);
    float* gr = mq_gu_ + static_cast<std::size_t>(r0) * kGuRows;
    flk::swiglu(gr, kGuRows, gr + kFfExp, kGuRows, mq_h_ + static_cast<std::size_t>(r0) * kFfExp, r1 - r0, kFfExp, st_);
    MMQ::quantize(mq_h_ + static_cast<std::size_t>(r0) * kFfExp, nullptr, mq_hq_, 8, kFfExp, kFfExp, r1 - r0, st_);
    MMQ::Product dn;
    dn.w = w + L.nl.down_off; dn.type = 8; dn.w_rows = kEmbd; dn.w_cols = kFfExp; dn.expert_bytes = stride; dn.n = n;
    dn.xq = mq_hq_; dn.bounds = rel_bounds; dn.ids = ident_; dn.total_rows = r1 - r0; dn.max_rows = most;
    dn.dst = yg_ + static_cast<std::size_t>(r0) * kEmbd; dn.ld_dst = kEmbd;
    mctx_->run(dn, st_);
  };
  if (S > 0) experts(cache_.device_slot(L.slot0), L.stream_stride, S, mq_bounds_, mq_bounds_, 0, bounds[S], most_slots);
  for (int g = 0; g < groups; ++g) {
    const int f = g * G, n = std::min(G, M - f);
    int most = 0;
    for (int j = 0; j < n; ++j) most = std::max(most, bounds[S + f + j + 1] - bounds[S + f + j]);
    const std::uint8_t* buf = ring_take(L, g);
    if (buf == nullptr) return false;
    experts(buf, L.stream_stride, n, mq_bounds_ + S + f, mq_bounds_ + (P + 1) + g * (kRingGroup + 1), bounds[S + f], bounds[S + f + n],
            most);
    if (!ring_give()) return false;
  }
  flk::scatter_combine(yg_, inv_d, wts_, T, kEmbd, moe_, st_);
  return launch_ok("experts (MMQ)");
}

bool FlintEngine::forward_batch(const std::int32_t* ids, int T) {
  const int pos0 = pos_;
  const bool mmq = mmq_on_ && T >= kMmqMinRows;
  if (static_cast<std::size_t>(T) > rows_ || (mmq && ring_buf_ == nullptr)) {
    std::fprintf(stderr, "multiengine-flint: a prompt chunk of %d rows on a row set of %zu\n", T, rows_);
    broken_ = true;
    return false;
  }
  if (mmq && !ring_begin()) return false;
  std::memcpy(ids_h_, ids, sizeof(int) * static_cast<std::size_t>(T));
  cudaMemcpyAsync(ids_d_, ids_h_, sizeof(int) * static_cast<std::size_t>(T), cudaMemcpyHostToDevice, st_);
  flk::embed(ids_d_, T, embd_, x_, st_);
  for (int l = 0; l < n_run_; ++l) {
    DevLayer& L = layers_[static_cast<std::size_t>(l)];
    flk::rmsnorm(x_, L.attn_norm, h_, T, kEmbd, eps_, st_);
    seam("attn_norm", l, h_, kEmbd, T);
    if (!gemm(L.q, h_, qraw_, T) || !gemm(L.k, h_, kraw_, T) || !gemm(L.v, h_, vraw_, T)) return false;
    flk::QkvPost p{qraw_, kraw_, vraw_, L.q_norm, L.k_norm, L.rope, rope_base_, pos0, T, L.sliding ? ring_ : 0, L.cap, eps_,
                   qout_, q16_, L.kc, L.vc, 0.0f, nullptr};
    flk::qkv_post(p, st_);
    if (!prompt_attention(L, pos0, T)) return false;
    seam("kqv_out", l, attn_, kHeads * kHead, T);
    if (!gemm(L.o, attn_, o_, T)) return false;
    flk::add_rows(x_, o_, nullptr, x1_, T, kEmbd, st_);
    seam("ffn_inp", l, x1_, kEmbd, T);
    flk::rmsnorm(x1_, L.ffn_norm, h_, T, kEmbd, eps_, st_);
    seam("ffn_norm", l, h_, kEmbd, T);
    if (!L.moe) {
      if (!ffn_dense(L, T, true)) return false;
      seam("ffn_out", l, ffn_, kEmbd, T);
      flk::add_rows(x1_, ffn_, nullptr, x_, T, kEmbd, st_);
    } else {
      if (!gemm(L.router, h_, rl_, T)) return false;
      seam("ffn_moe_logits", l, rl_, kExperts, T);
      flk::router_topk(rl_, T, picks_, wts_, L.counts, w_scale_, st_);
      if (!(mmq ? (L.mmq ? moe_mmq(L, T) : moe_ring_grouped(L, T)) : moe_experts(L, T))) return false;
      seam("ffn_moe_out", l, moe_, kEmbd, T);
      if (!ffn_shared(L, T, true)) return false;
      seam("ffn_shexp", l, ffn_, kEmbd, T);
      flk::add_rows(x1_, moe_, ffn_, x_, T, kEmbd, st_);
    }
    seam("l_out", l, x_, kEmbd, T);
    if (sink_ != nullptr) dump_layer(l, pos0, T);
  }
  if (mmq && ring_next_ != ring_seq_.size()) {
    std::fprintf(stderr, "multiengine-flint: the expert stream ended out of step\n");
    broken_ = true;
    return false;
  }
  if (!head(x_ + static_cast<std::size_t>(T - 1) * kEmbd)) return false;
  pos_ = pos0 + T;
  return true;
}

int FlintEngine::process(const std::int32_t* ids, int n, const std::function<bool(int)>& progress) {
  if (broken_) return -1;
  if (n <= 0 || pos_ + n > ctx_) return 0;
  int done = 0;
  while (done < n) {
    const int left = n - done;
    if (left <= 4) {
      if (!give_back()) return -1;
      for (int i = 0; i < left; ++i) {
        if (!forward_one(ids[done + i])) return -1;
      }
      done = n;
    } else {
      const int T = std::min(left, chunk_);
      if (!(mmq_on_ && T >= kMmqMinRows ? lend() : give_back())) return -1;
      if (!forward_batch(ids + done, T)) return -1;
      done += T;
      if (n > 1) rebalance(256);
    }
    if (!cuda_ok(cudaStreamSynchronize(st_), "process")) return -1;
    if (n > 1 && progress && !progress(done) && done < n) break;
  }
  if (!give_back()) return -1;
  if (n > 4) rebalance(256);
  return done;
}

bool FlintEngine::logits(std::vector<float>& out) {
  out.resize(kVocab);
  return cuda_ok(cudaMemcpyAsync(out.data(), logits_, sizeof(float) * kVocab, cudaMemcpyDeviceToHost, st_), "logits") &&
         cuda_ok(cudaStreamSynchronize(st_), "logits");
}

int FlintEngine::greedy() {
  flk::argmax(logits_, kVocab, argmax_out_, argmax_scratch_, st_);
  if (!cuda_ok(cudaMemcpyAsync(argmax_h_, argmax_out_, sizeof(int), cudaMemcpyDeviceToHost, st_), "argmax") ||
      !cuda_ok(cudaStreamSynchronize(st_), "argmax")) {
    return -1;
  }
  return argmax_h_[0];
}

int FlintEngine::top_candidates(int k, float* vals, int* ids) {
  if (k <= 0 || k > flk::kTopkMax) return -1;
  cudaMemsetAsync(cand_flag_, 0, sizeof(int), st_);
  flk::topk(logits_, kVocab, k, cand_v_, cand_i_, cand_flag_, topk_scratch_, st_);
  float* hv = reinterpret_cast<float*>(cand_h_ + 1 + flk::kTopkMax);
  if (!cuda_ok(cudaMemcpyAsync(cand_h_, cand_flag_, sizeof(int), cudaMemcpyDeviceToHost, st_), "candidates") ||
      !cuda_ok(cudaMemcpyAsync(cand_h_ + 1, cand_i_, sizeof(int) * static_cast<std::size_t>(k), cudaMemcpyDeviceToHost, st_), "candidates") ||
      !cuda_ok(cudaMemcpyAsync(hv, cand_v_, sizeof(float) * static_cast<std::size_t>(k), cudaMemcpyDeviceToHost, st_), "candidates") ||
      !cuda_ok(cudaStreamSynchronize(st_), "candidates")) {
    return -1;
  }
  if (cand_h_[0] != 0) return -2;
  std::memcpy(ids, cand_h_ + 1, sizeof(int) * static_cast<std::size_t>(k));
  std::memcpy(vals, hv, sizeof(float) * static_cast<std::size_t>(k));
  return k;
}

bool FlintEngine::save_profile(bool force, std::string& error) {
  if (profile_save_.empty() || layers_.empty()) return true;
  const auto now = Clock::now();
  if (!force && std::chrono::duration<double>(now - profile_saved_).count() < kProfileSaveMinutes * 60.0) return true;
  std::vector<std::uint8_t> resident(static_cast<std::size_t>(kMoeLayers) * kExperts, 0);
  std::vector<double> heat(resident.size(), 0.0);
  for (int t = 0; t < kMoeLayers; ++t) {
    const DevLayer& L = tier(t);
    for (int e = 0; e < kExperts; ++e) {
      const std::size_t i = static_cast<std::size_t>(t) * kExperts + static_cast<std::size_t>(e);
      resident[i] = L.slot_of[static_cast<std::size_t>(e)] != -1 ? 1 : 0;
      heat[i] = L.score[static_cast<std::size_t>(e)];
    }
  }
  const auto ranked = strata::core::rank_learned_profile(kMoeLayers, kExperts, resident, heat, profile_prior_);
  if (!strata::core::write_expert_profile(profile_save_, kMoeLayers, kExperts, ranked, error)) return false;
  profile_saved_ = now;
  return true;
}

void FlintEngine::land_swaps() {
  for (auto it = swaps_.begin(); it != swaps_.end();) {
    if (cudaEventQuery(it->landed) != cudaSuccess) {
      ++it;
      continue;
    }
    DevLayer& L = tier(it->tier);
    L.expert_in[static_cast<std::size_t>(it->slot - L.slot0)] = it->expert;
    L.slot_of[static_cast<std::size_t>(it->expert)] = it->slot;
    set_ptr(L, it->expert, reinterpret_cast<unsigned long long>(cache_.device_slot(it->slot)));
    set_res(L, it->expert, it->slot);
    free_events_.push_back(it->landed);
    it = swaps_.erase(it);
  }
}

// The adaptive tier (paper 3.4, findings 4 and 10), as Gemma 4's: the routing counts, decayed, move the hottest RAM
// experts into the coldest slots without anyone waiting.
void FlintEngine::rebalance(int max_swaps) {
  if (broken_ || layers_.empty() || tier(0).n_slots == 0 || tier(0).n_slots >= kExperts) return;
  land_swaps();
  if (counts_pending_ && cudaEventQuery(counts_ev_) != cudaSuccess) return;
  const bool have_counts = counts_pending_;
  if (have_counts) {
    for (int t = 0; t < kMoeLayers; ++t) {
      auto& score = tier(t).score;
      for (int e = 0; e < kExperts; ++e)
        score[static_cast<std::size_t>(e)] = score[static_cast<std::size_t>(e)] * 0.85 + counts_h_[static_cast<std::size_t>(t) * kExperts + static_cast<std::size_t>(e)];
    }
  }
  if (cudaMemcpyAsync(counts_h_, counts_all_, sizeof(int) * kMoeLayers * kExperts, cudaMemcpyDeviceToHost, st_) != cudaSuccess ||
      cudaMemsetAsync(counts_all_, 0, sizeof(int) * kMoeLayers * kExperts, st_) != cudaSuccess ||
      cudaEventRecord(counts_ev_, st_) != cudaSuccess) {
    broken_ = true;
    return;
  }
  counts_pending_ = true;
  if (!have_counts) return;
  struct Pick {
    int tier, slot, expert;
  };
  std::vector<Pick> picks;
  for (std::size_t round = 0; static_cast<int>(picks.size()) < max_swaps; ++round) {
    bool any = false;
    for (int t = 0; t < kMoeLayers && static_cast<int>(picks.size()) < max_swaps; ++t) {
      DevLayer& L = tier(t);
      int hot = -1, cold_slot = -1;
      for (int e = 0; e < kExperts; ++e) {
        if (L.slot_of[static_cast<std::size_t>(e)] == -1 && (hot < 0 || L.score[static_cast<std::size_t>(e)] > L.score[static_cast<std::size_t>(hot)])) hot = e;
      }
      for (int s = 0; s < L.n_slots; ++s) {
        const int e = L.expert_in[static_cast<std::size_t>(s)];
        if (e < 0) continue;
        if (cold_slot < 0 || L.score[static_cast<std::size_t>(e)] < L.score[static_cast<std::size_t>(L.expert_in[static_cast<std::size_t>(cold_slot)])]) cold_slot = s;
      }
      if (hot < 0 || cold_slot < 0) continue;
      const int cold = L.expert_in[static_cast<std::size_t>(cold_slot)];
      if (L.score[static_cast<std::size_t>(hot)] <= L.score[static_cast<std::size_t>(cold)] * 1.25 + 0.5) continue;
      L.expert_in[static_cast<std::size_t>(cold_slot)] = -1;
      L.slot_of[static_cast<std::size_t>(cold)] = -1;
      set_ptr(L, cold, reinterpret_cast<unsigned long long>(L.host_alias + L.nl.bytes * static_cast<std::size_t>(cold)));
      set_res(L, cold, -1);
      L.slot_of[static_cast<std::size_t>(hot)] = -2;
      picks.push_back({t, L.slot0 + cold_slot, hot});
      any = true;
    }
    if (!any) break;
  }
  if (picks.empty()) return;
  if (!cuda_ok(cudaEventRecord(evict_ev_, st_), "event") || !cuda_ok(cudaStreamWaitEvent(copy_, evict_ev_, 0), "event")) return;
  for (const Pick& p : picks) {
    const DevLayer& L = tier(p.tier);
    Swap s{p.tier, p.slot, p.expert, nullptr};
    if (!free_events_.empty()) {
      s.landed = free_events_.back();
      free_events_.pop_back();
    } else if (!cuda_ok(cudaEventCreateWithFlags(&s.landed, cudaEventDisableTiming), "event")) {
      return;
    }
    if (!cuda_ok(cudaMemcpyAsync(cache_.device_slot(p.slot), L.host_experts + L.nl.bytes * static_cast<std::size_t>(p.expert), L.nl.bytes,
                                 cudaMemcpyHostToDevice, copy_),
                 "expert swap") ||
        !cuda_ok(cudaEventRecord(s.landed, copy_), "event")) {
      return;
    }
    swaps_.push_back(s);
  }
}

}  // namespace

std::unique_ptr<Engine> Engine::create(const EngineConfig& config, const ModelDesc& model, std::string& error) {
  auto e = std::make_unique<FlintEngine>();
  if (!e->load(config, model, error)) {
    if (error.empty()) error = "the engine could not load the model";
    return nullptr;
  }
  return e;
}

}  // namespace fl
