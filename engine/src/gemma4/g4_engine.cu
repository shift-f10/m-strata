// strata-gemma4-engine: load, placement and the forward passes (plan Task 6; spec §2.3, §4).
#include "g4_engine.hpp"

#include "g4_file.hpp"
#include "g4_kernels.cuh"
#include "g4_lane.hpp"
#include "g4_profile.hpp"

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

namespace g4 {
namespace {

using Clock = std::chrono::steady_clock;

namespace MMQ = strata::prefill::mmq;

constexpr int kGuRows = 2 * kFfExp;  // 1408: gate rows then up rows

// P5 (paper 3.6): a prompt chunk of at least kMmqMinRows tokens computes its experts with llama.cpp's MMQ - the
// VRAM-resident ones straight from their slots, the others streamed from the pinned arena by a copy stream into a
// ring of kRingBufs staging buffers of kRingGroup experts each, while the GPU computes the ones before them.
constexpr int kMmqMinRows = 64;
constexpr int kRingBufs = 3;
constexpr int kRingGroup = 16;
constexpr int kMqBoundsInts = 512;  // a layer's positions' bounds (<= 257) and its groups' relative bounds (<= 8 x 17)
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

// One expert blob in Strata's native form (kernels::NativeExpertLayout): [gate rows | up rows | down rows], the
// GGUF's own blocks.  Gemma's fused ffn_gate_up_exps slice of an expert is its 704 gate rows then its 704 up rows,
// so the blob is that slice followed by the expert's ffn_down_exps slice, copied as the file stores them (paper
// 3.6).  The two may differ in type (the Q8_K_XL file's layer 29: Q8_0 gate_up, BF16 down); the activation between
// them is GEGLU.
strata::kernels::NativeExpertLayout native_layout(TensorType gu, TensorType dn) {
  auto L = strata::kernels::native_expert_layout(static_cast<int>(gu), static_cast<int>(dn), kEmbd, kFfExp);
  L.act = 1;  // GEGLU: gelu_tanh(gate) * up
  return L;
}

struct DevLayer {
  int index{0};
  bool mmq{false};  // the prompt's experts on MMQ (both expert types have it: the Q8_0 layers)
  std::size_t stream_stride{0};  // a streamed prompt's expert stride: the slots' and the ring's (blob, guard, aligned)
  int stream_group{0};           // experts of this layer one ring buffer holds (at most kRingGroup)
  bool sliding{true};
  int hd{kHeadSwa};
  int n_kv{kKvHeadsSwa};
  g4k::Mat q{}, k{}, v{}, o{}, gate{}, up{}, down{}, router{};
  float *attn_norm{}, *q_norm{}, *k_norm{}, *post_attn_norm{}, *ffn_norm{}, *post_ffw_norm_1{}, *gate_inp_scale{};
  float *pre_ffw_norm_2{}, *down_scale{}, *post_ffw_norm_2{}, *post_ffw_norm{}, *layer_scale{};
  strata::kernels::NativeExpertLayout nl{};  // the layer's native expert blob (GGUF blocks, GEGLU)
  std::uint8_t* host_experts{};   // the pinned arena's range for this layer: 128 blobs (mapped)
  std::uint8_t* host_alias{};     // the device's address for it
  int n_slots{0};
  int slot0{0};                   // the layer's first slot in the expert cache (its slots are slot0 .. slot0+n_slots-1)
  std::vector<int> slot_of;       // expert -> slot index in the cache, -1 when only in RAM
  std::vector<int> expert_in;     // the layer's slot j -> expert (-1: empty, or a swap still landing)
  std::vector<unsigned long long> ptr_host;  // expert -> its blob's device address (VRAM slot or the arena alias)
  unsigned long long* ptrtab{};   // device [128]: the same
  int* counts{};                  // device [128] routing counts since the last rebalance (in counts_all_)
  std::vector<double> score;      // decayed routing counts
  __half* kc{};
  __half* vc{};
  int cap{0};
};

class Gemma4Engine final : public Engine {
 public:
  ~Gemma4Engine() override { release(); }

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
  bool upload_vector(const TensorSpan& t, float*& out);
  bool upload_matrix(const TensorSpan& t, g4k::Mat& out);
  bool load_experts(DevLayer& L, const LayerDesc& d);
  bool place_experts(double vram_gib, int forced_slots, const std::string& profile, std::string& error);
  // the layer's experts for T tokens (e_in_ -> yg_ rows, then moe_): Strata's grouped kernel on the native blobs
  bool moe_experts(DevLayer& L, int T);
  // a decode step's experts through the CPU lane (paper 3.3): VRAM groups, the staged share, the CPU's rows
  bool moe_lane(DevLayer& L);
  // a prompt chunk's experts on MMQ (paper 3.6): the slots, then the streamed misses group by group
  bool moe_mmq(DevLayer& L, int T);
  // the same chunk's experts on a layer MMQ has no kernel for (BF16): the grouped kernel on the slots and the stream
  bool moe_ring_grouped(DevLayer& L, int T);
  int stream_plan(const DevLayer& L, int A, int* bounds, int* inv, int* ent, int* tok);
  bool ring_begin();                 // the chunk's miss groups, and the first kRingBufs copies
  bool ring_kick(std::size_t i);     // group i's copies into its ring buffer, after that buffer's last reader
  const std::uint8_t* ring_take(const DevLayer& L, int g);   // the layer's g-th group, landed
  bool ring_give();                  // ... computed: its buffer goes to the group kRingBufs later
  // A chunk's rows: the activations, attention, the experts' rows and scratch, the prompt attention's scores, and for
  // a streamed prompt the MMQ buffers and the miss ring. Decode and short batches use small_, of their own; a
  // streamed prompt big_, carved from the expert cache's top slots lent for it (paper 3.6, Strata's default).
  struct Rows {
    std::size_t rows{0}, s_floats{0}, bytes{0};
    float *x{}, *x1{}, *h{}, *o{}, *dense{}, *rn{}, *e_in{}, *moe{}, *qraw{}, *qout{}, *attn{}, *kraw{}, *vraw{};
    float *g{}, *u{}, *m{}, *rl{}, *yg{}, *s_buf{}, *mq_gu{}, *mq_h{};
    __half *q16{}, *p_buf{};
    std::uint8_t *xq{}, *escratch{}, *mq_x{}, *mq_hq{}, *ring{};
  };
  std::size_t layout_rows(std::uint8_t* base, std::size_t rows, std::size_t s_floats, bool streamed, Rows& r) const;
  void bind(const Rows& r);
  bool lend();        // a streamed prompt's chunk: big_ bound (on the lent slots, the first time)
  bool give_back();   // small_ bound; lent slots zeroed and refilled as swaps that land without a wait
  bool start_lane(const EngineConfig& cfg, std::string& error);
  void set_res(const DevLayer& L, int e, int slot);         // the lane's residency of expert e (a slot, or -1)
  bool set_ptr(DevLayer& L, int e, unsigned long long p);   // expert e's blob address, host and device table
  void land_swaps();                                        // admit the swaps whose copies have landed
  bool gemm(const g4k::Mat& w, const float* x, float* y, int T);
  bool sgemm(int rows, int T, int cols, const float* w, const float* x, float* y);
  bool forward_one(std::int32_t id);
  bool decode_body(int pos, bool lane, int split_full, int split_swa);
  bool forward_batch(const std::int32_t* ids, int T);
  bool prompt_attention(DevLayer& L, int pos0, int T);
  bool head(const float* x_row);
  void dump_layer(int layer, int pos0, int T);
  void release();

  ReadOnlyFile file_;
  std::vector<std::uint8_t> staging_;
  std::vector<void*> allocations_;
  std::vector<void*> pinned_;
  cudaStream_t st_{};
  cudaStream_t copy_{};   // the copy engine's stream: the expert cache's swaps (paper finding 10)
  cublasHandle_t blas_{};
  bool broken_{false};

  // the VRAM expert tier (paper 3.1): Strata's ExpertCache arena, each layer's slots back to back, byte-sized to
  // its blob (finding 8).  A swap evicts at once and admits when its copy has landed, so nobody waits (finding 10).
  strata::core::ExpertCache cache_;
  struct Swap {
    int layer{0}, slot{0}, expert{0};
    cudaEvent_t landed{};
  };
  std::vector<Swap> swaps_;
  std::vector<cudaEvent_t> free_events_;
  int* counts_h_{};          // pinned: the routing counts read back without a wait
  cudaEvent_t counts_ev_{};  // ... recorded after that copy; the next rebalance uses them once it has completed
  cudaEvent_t evict_ev_{};   // the compute stream past a rebalance's evictions: its copies wait for it
  bool counts_pending_{false};
  // the grouped kernel's plan, q8_1 activations and scratch
  unsigned long long* plan_ptr_{};
  int *plan_start_{}, *plan_n_{}, *plan_dst_{}, *plan_tok_{};
  int* plan_h_{};                  // pinned: the prompt path's plan (n, start[129], dst, tok), then ptrs
  unsigned long long* plan_ptr_h_{};
  std::uint8_t* xq_{};             // the experts' input as q8_1 blocks, a row per token
  std::uint8_t* escratch_{};       // native_expert_grouped's scratch for chunk * 8 entries
  int* ident_{};                   // device [chunk * 8]: 0, 1, 2, ... (the prompt's rows are in routed order)
  // the CPU lane (paper 3.3): its thread, the residency it plans from (pinned [30][128], a cache slot or -1, written
  // between tokens only) and the device's copy for the doorbell (patched before a token when it changed), the plan's
  // device copy, the GPU's own rows of a step, and the staging the copy engine fills
  std::unique_ptr<CpuLane> lane_;
  std::int32_t* res_h_{};
  int* res_d_{};
  bool res_dirty_{false};
  std::int32_t* lplan_{};
  float* hit_out_{};
  std::uint8_t* lstage_{};
  std::size_t lstage_bytes_{0};
  // the prompt's MMQ path: its context and buffers (activations, gate/up rows, h, h quantized), the bounds / row
  // tables (pinned staging, then the device copies), the slots' guard for MMQ's over-read, and the miss ring
  bool mmq_on_{false};
  std::unique_ptr<MMQ::Context> mctx_;
  std::size_t mmq_guard_{0};
  std::uint8_t *mq_x_{}, *mq_hq_{};
  float *mq_gu_{}, *mq_h_{};
  int *mq_bounds_{}, *mq_ids_{}, *mq_pin_{};   // mq_ids_: [A] sorted row -> entry, then [A] sorted row -> token
  cudaStream_t ring_st_{};
  std::uint8_t* ring_buf_{};       // kRingBufs buffers of ring_buf_bytes_, each a group's experts stream_stride apart
  std::size_t ring_buf_bytes_{0};
  int *rg_h_{}, *rg_d_{};          // moe_ring_grouped's call plans (pinned, device): kRgCalls x kRgCallInts
  cudaEvent_t ring_landed_[kRingBufs]{}, ring_freed_[kRingBufs]{};
  bool ring_dirty_[kRingBufs]{};   // the buffer may hold non-zero bytes where an MMQ group's guards go
  struct RingGroup {
    int layer{0}, first{0}, n{0};  // the layer's misses [first, first + n)
  };
  std::vector<RingGroup> ring_seq_;
  std::size_t ring_next_{0};       // the next group a layer computes
  std::vector<std::vector<int>> miss_;  // per layer, this chunk: the experts without a slot, ascending
  // --expert-profile's ranking (the prior a saved profile keeps for the experts this run never routed), and where
  // and when save_profile last wrote
  std::vector<std::pair<std::int32_t, std::int32_t>> profile_prior_;
  std::string profile_save_;
  Clock::time_point profile_saved_{};
  // the row sets, which is bound (rows_), and the loan: the first lent slot (every slot from it to the arena's end),
  // the experts the lent slots held, and big_'s own VRAM when the cache could not lend it
  Rows small_, big_;
  std::size_t rows_{0};
  bool borrow_{false}, lent_{false};
  int lend_slot_{-1};
  std::vector<std::pair<int, int>> lent_experts_;   // (slot, expert)
  // the decode step's graph (paper 3.6) and the attention splits it was captured for
  bool graph_on_{true};
  std::unique_ptr<strata::core::CapturedGraph> graph_;
  int graph_key_{-1};

  int ctx_{0}, chunk_{0}, ring_{0}, pos_{0}, max_splits_{0};
  float eps_{1e-6f}, base_full_{1e6f}, base_swa_{1e4f}, softcap_{30.0f};
  std::vector<DevLayer> layers_;
  g4k::Mat embd_{};
  float* output_norm_{};
  float* rope_freqs_{};

  // scratch
  int* ids_d_{};
  int* ids_h_{};  // pinned
  float *x_{}, *x1_{}, *h_{}, *o_{}, *dense_{}, *rn_{}, *e_in_{}, *moe_{};
  float *qraw_{}, *qout_{}, *attn_{}, *kraw_{}, *vraw_{};
  __half* q16_{};
  float *g_{}, *u_{}, *m_{}, *rl_{}, *wts_{};
  int* picks_{};
  int* picks_h_{};  // pinned
  float* yg_{};     // the experts' output rows: [chunk * 8][2816], row t*8+k = token t's k-th pick
  float* wdq_{};
  float* s_buf_{};
  __half* p_buf_{};
  std::size_t s_floats_{0};
  float* dscratch_{};
  float* logits_{};
  float* xn_{};
  float* argmax_scratch_{};
  int* counts_all_{};  // device [30][128]: every layer's routing counts, read with one copy
  unsigned char* topk_scratch_{};
  float* cand_v_{};
  int* cand_i_{};
  int* cand_flag_{};
  int* cand_h_{};  // pinned: flag, then k ids, then k values
  int* argmax_out_{};
  int* argmax_h_{};  // pinned

  std::vector<float>* sink_{nullptr};
  LoadReport report_{};
};

bool Gemma4Engine::cuda_ok(cudaError_t e, const char* what) {
  if (e == cudaSuccess) return true;
  std::fprintf(stderr, "strata-gemma4: CUDA error in %s: %s\n", what, cudaGetErrorString(e));
  broken_ = true;
  return false;
}

void Gemma4Engine::release() {
  if (!broken_) {
    std::string why;
    if (!save_profile(true, why)) std::fprintf(stderr, "strata-gemma4: the expert profile was not saved: %s\n", why.c_str());
  }
  if (lane_) {
    const CpuLane::Stats s = lane_->stats();
    if (s.tokens > 0) {
      const long long gpu = s.hits, cpu = s.cpu_entries;
      std::fprintf(stderr, "strata-gemma4: CPU lane: %lld decode steps; %lld expert entries from VRAM, %lld staged experts, %lld "
                   "entries (%lld experts) on the CPU pool in %.0f ms\n", s.tokens, gpu, s.staged, cpu, s.cpu_experts, s.pool_ms);
    }
    lane_->stop();   // a token still in flight is released before the stream is waited on
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

bool Gemma4Engine::upload_vector(const TensorSpan& t, float*& out) {
  if (!t.present) {
    out = nullptr;
    return true;
  }
  staging_.resize(static_cast<std::size_t>(t.bytes));
  if (!file_.read_at(t.file_offset, staging_.data(), staging_.size())) {
    std::fprintf(stderr, "strata-gemma4: cannot read the model file\n");
    return false;
  }
  if (!alloc(out, static_cast<std::size_t>(t.bytes / 4))) return false;
  return cuda_ok(cudaMemcpy(out, staging_.data(), staging_.size(), cudaMemcpyHostToDevice), "upload");
}

bool Gemma4Engine::upload_matrix(const TensorSpan& t, g4k::Mat& out) {
  out.rows = static_cast<int>(t.rows);
  out.cols = static_cast<int>(t.cols);
  const std::size_t rows = static_cast<std::size_t>(t.rows), cols = static_cast<std::size_t>(t.cols);
  const std::size_t rb = row_bytes(t.type, t.cols);
  if (t.type == TensorType::f32 || t.type == TensorType::bf16) {
    out.kind = t.type == TensorType::f32 ? g4k::WKind::f32 : g4k::WKind::bf16;
    std::uint8_t* d = nullptr;
    if (!alloc(d, rows * rb)) return false;
    const std::size_t step = std::max<std::size_t>(1, (64u << 20) / rb);
    for (std::size_t r0 = 0; r0 < rows; r0 += step) {
      const std::size_t n = std::min(step, rows - r0);
      staging_.resize(n * rb);
      if (!file_.read_at(t.file_offset + r0 * rb, staging_.data(), n * rb)) return false;
      if (!cuda_ok(cudaMemcpy(d + r0 * rb, staging_.data(), n * rb, cudaMemcpyHostToDevice), "upload")) return false;
    }
    out.w = d;
    out.s = nullptr;
    return true;
  }
  out.kind = g4k::WKind::q8;
  std::int8_t* q = nullptr;
  __half* s = nullptr;
  if (!alloc(q, rows * cols) || !alloc(s, rows * (cols / 32))) return false;
  const std::size_t step = std::max<std::size_t>(1, (32u << 20) / rb);
  std::vector<std::int8_t> hq;
  std::vector<std::uint16_t> hs;
  for (std::size_t r0 = 0; r0 < rows; r0 += step) {
    const std::size_t n = std::min(step, rows - r0);
    staging_.resize(n * rb);
    if (!file_.read_at(t.file_offset + r0 * rb, staging_.data(), n * rb)) return false;
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

// The pinned arena (paper 3.1, finding 13): every routed expert in page-locked RAM, one range per layer (a copy
// or a read never spans two), mapped so the GPU can reach it too.  Each blob is the GGUF's own bytes: the expert's
// gate_up slice into [0, down_off), its down slice into [down_off, bytes).
bool Gemma4Engine::load_experts(DevLayer& L, const LayerDesc& d) {
  L.nl = native_layout(d.gate_up_exps.type, d.down_exps.type);
  const std::size_t gu_file = row_bytes(d.gate_up_exps.type, kEmbd) * kGuRows;
  const std::size_t dn_file = row_bytes(d.down_exps.type, kFfExp) * kEmbd;
  if (gu_file != L.nl.down_off || L.nl.down_off + dn_file != L.nl.bytes) {
    std::fprintf(stderr, "strata-gemma4: the expert tensors do not match the native blob layout\n");
    return false;
  }
  void* host = nullptr;
  if (!cuda_ok(cudaHostAlloc(&host, L.nl.bytes * kExperts, cudaHostAllocMapped | cudaHostAllocPortable), "cudaHostAlloc")) {
    std::fprintf(stderr, "strata-gemma4: cannot pin %.2f GiB of host memory for a layer's experts\n",
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
    if (!file_.read_at(d.gate_up_exps.file_offset + gu_file * static_cast<std::size_t>(e), blob, gu_file) ||
        !file_.read_at(d.down_exps.file_offset + dn_file * static_cast<std::size_t>(e), blob + L.nl.down_off, dn_file)) {
      return false;
    }
  }
  return true;
}

bool Gemma4Engine::set_ptr(DevLayer& L, int e, unsigned long long p) {
  L.ptr_host[static_cast<std::size_t>(e)] = p;
  // ordered with the compute stream: kernels queued before it read the old address, those after the new one
  g4k::set_u64(L.ptrtab, e, p, st_);
  return launch_ok("expert table");
}

void Gemma4Engine::set_res(const DevLayer& L, int e, int slot) {
  if (res_h_ == nullptr) return;
  res_h_[static_cast<std::size_t>(L.index) * kExperts + static_cast<std::size_t>(e)] = slot;
  res_dirty_ = true;
}

// A row set of `rows` rows laid out from `base` in 256-byte aligned pieces (base null: only its size), with the MMQ
// buffers and the miss ring when `streamed`. Returns the bytes.
std::size_t Gemma4Engine::layout_rows(std::uint8_t* base, std::size_t rows, std::size_t s_floats, bool streamed, Rows& r) const {
  std::size_t at = 0;
  auto take = [&](auto*& p, std::size_t count) {
    using T = std::remove_reference_t<decltype(*p)>;
    at = (at + 255) & ~std::size_t{255};
    p = base != nullptr ? reinterpret_cast<T*>(base + at) : nullptr;
    at += std::max<std::size_t>(count, 1) * sizeof(T);
  };
  r = Rows{};
  for (float** p : {&r.x, &r.x1, &r.h, &r.o, &r.dense, &r.rn, &r.e_in, &r.moe}) take(*p, rows * kEmbd);
  for (float** p : {&r.qraw, &r.qout, &r.attn}) take(*p, rows * kHeads * kHeadFull);
  take(r.q16, rows * kHeads * kHeadFull);
  for (float** p : {&r.kraw, &r.vraw}) take(*p, rows * kKvHeadsSwa * kHeadSwa);
  for (float** p : {&r.g, &r.u, &r.m}) take(*p, rows * kFf);
  take(r.rl, rows * kExperts);
  take(r.yg, rows * 8 * kEmbd);
  take(r.xq, rows * (kEmbd / 32) * 36);
  take(r.escratch, strata::kernels::native_expert_scratch_bytes(static_cast<std::int64_t>(rows) * 8, kFfExp));
  take(r.s_buf, s_floats);
  take(r.p_buf, s_floats);
  if (streamed) {
    const auto A = static_cast<std::int64_t>(rows) * 8;
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

void Gemma4Engine::bind(const Rows& r) {
  x_ = r.x; x1_ = r.x1; h_ = r.h; o_ = r.o; dense_ = r.dense; rn_ = r.rn; e_in_ = r.e_in; moe_ = r.moe;
  qraw_ = r.qraw; qout_ = r.qout; attn_ = r.attn; q16_ = r.q16; kraw_ = r.kraw; vraw_ = r.vraw;
  g_ = r.g; u_ = r.u; m_ = r.m; rl_ = r.rl; yg_ = r.yg; xq_ = r.xq; escratch_ = r.escratch;
  s_buf_ = r.s_buf; p_buf_ = r.p_buf; s_floats_ = r.s_floats;
  ring_buf_ = r.ring; mq_x_ = r.mq_x; mq_gu_ = r.mq_gu; mq_h_ = r.mq_h; mq_hq_ = r.mq_hq;
  rows_ = r.rows;
}

// A streamed prompt chunk: big_. On the loan's first chunk the lent slots give up their experts (each read from the
// arena from here on, in stream order) - after the copy stream's swaps, one of which may be landing in a lent slot.
bool Gemma4Engine::lend() {
  if (borrow_ && !lent_) {
    if (!cuda_ok(cudaStreamSynchronize(copy_), "expert swaps")) return false;
    land_swaps();
    for (auto& L : layers_) {
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
    for (bool& d : ring_dirty_) d = true;   // the lent slots' bytes are where the ring's guards go
  }
  bind(big_);
  return true;
}

// The loan ends (before a decode step or a short batch, and after the prompt): small_ again; the lent bytes back to
// zero (a slot's guard is MMQ's over-read), then each lent slot refilled - with the expert it held, or when the tier
// placed that one elsewhere meanwhile, its layer's hottest one in RAM only - as the adaptive tier's swaps: copied on
// the copy stream once the compute stream is past the prompt, and used once landed (land_swaps). Nobody waits.
bool Gemma4Engine::give_back() {
  bind(small_);
  if (!lent_) return true;
  lent_ = false;
  const int N = kLayers * layers_[0].n_slots;
  const std::uint64_t at = cache_.slot_offset(lend_slot_);
  if (!cuda_ok(cudaMemsetAsync(cache_.device_slot(lend_slot_), 0, static_cast<std::size_t>(cache_.slot_offset(N) - at), st_), "lent slots") ||
      !cuda_ok(cudaEventRecord(evict_ev_, st_), "event") || !cuda_ok(cudaStreamWaitEvent(copy_, evict_ev_, 0), "event")) {
    return false;
  }
  for (const auto& [slot, held] : lent_experts_) {
    DevLayer& L = layers_[static_cast<std::size_t>(slot / layers_[0].n_slots)];   // a layer's slots are slot0 .. slot0 + S - 1
    int e = held;
    if (L.slot_of[static_cast<std::size_t>(e)] != -1) {
      e = -1;
      for (int x = 0; x < kExperts; ++x)
        if (L.slot_of[static_cast<std::size_t>(x)] == -1 && (e < 0 || L.score[static_cast<std::size_t>(x)] > L.score[static_cast<std::size_t>(e)])) e = x;
      if (e < 0) continue;
    }
    Swap sw{L.index, slot, e, nullptr};
    if (!free_events_.empty()) {
      sw.landed = free_events_.back();
      free_events_.pop_back();
    } else if (!cuda_ok(cudaEventCreateWithFlags(&sw.landed, cudaEventDisableTiming), "event")) {
      return false;
    }
    L.slot_of[static_cast<std::size_t>(e)] = -2;   // landing
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

// The CPU lane: the residency it plans from, the slots' device addresses, and its thread with the CPU pool.
bool Gemma4Engine::start_lane(const EngineConfig& cfg, std::string& error) {
  void* res = nullptr;
  if (!cuda_ok(cudaHostAlloc(&res, sizeof(std::int32_t) * kLayers * kExperts, cudaHostAllocDefault), "cudaHostAlloc")) {
    error = "cannot pin the CPU lane's residency table";
    return false;
  }
  pinned_.push_back(res);
  res_h_ = static_cast<std::int32_t*>(res);
  std::vector<CpuLane::Layer> lanes;
  for (const auto& L : layers_) {
    for (int e = 0; e < kExperts; ++e) {
      const int s = L.slot_of[static_cast<std::size_t>(e)];
      res_h_[static_cast<std::size_t>(L.index) * kExperts + static_cast<std::size_t>(e)] = s >= 0 ? s : -1;
    }
    lanes.push_back({L.host_experts, L.nl.bytes, L.nl.gu_type, L.nl.d_type});
  }
  if (!cuda_ok(cudaMemcpy(res_d_, res_h_, sizeof(std::int32_t) * kLayers * kExperts, cudaMemcpyHostToDevice), "residency")) {
    error = "cannot initialise the CPU lane's residency table";
    return false;
  }
  const int n_slots = kLayers * layers_[0].n_slots;
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

// A decode step's routed experts on the paper's three tiers (3.3): the doorbell publishes the routing and the input;
// the lane's plan names the VRAM groups (run at once), the staged groups (once the copy engine has landed them) and
// leaves the rest to the CPU pool, whose rows arrive in mapped memory.  Rows yg_[k] in routing order, then the combine
// as the GPU-only path's.
bool Gemma4Engine::moe_lane(DevLayer& L) {
  namespace K = strata::kernels;
  using P = LanePlan;
  const CpuLane::Device& D = lane_->device();
  const auto ring = static_cast<std::uint32_t>(L.index + 1);
  K::quantize_q8_1_rows(e_in_, 1, kEmbd, xq_, st_);
  K::doorbell_publish_res(e_in_, picks_, res_d_ + static_cast<std::size_t>(L.index) * kExperts, kExperts, kEmbd, 8, D.x, D.ids,
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
  // the CPU's rows (the GPU's own rows written as zeros), then the GPU's rows added into them
  K::copy_rows_from_mapped_after(D.flag_c, ring, yg_, D.rows, 8, kEmbd, lplan_ + P::kDst, lplan_ + P::kCounts + 1, st_);
  K::moe_hit_add(yg_, hit_out_, lplan_ + P::kDst, lplan_ + P::kCounts + 1, P::kCap, kEmbd, st_);
  g4k::moe_combine(yg_, wts_, picks_, L.down_scale, 8, kEmbd, moe_, st_);
  return launch_ok("experts (CPU lane)");
}

bool Gemma4Engine::place_experts(double vram_gib, int forced_slots, const std::string& profile, std::string& error) {
  std::size_t free_b = 0, total_b = 0;
  if (!cuda_ok(cudaMemGetInfo(&free_b, &total_b), "cudaMemGetInfo")) {
    error = "cannot read the free VRAM";
    return false;
  }
  std::size_t per_slot = 0;
  const auto guard = static_cast<std::int64_t>(mmq_guard_);   // MMQ's over-read past a slot's blob (zeroed with the arena)
  for (const auto& L : layers_)
    per_slot += static_cast<std::size_t>(strata::core::ExpertCache::slot_stride(static_cast<std::int64_t>(L.nl.bytes), guard));
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
  if (!alloc(counts_all_, static_cast<std::size_t>(kLayers) * kExperts) ||
      !cuda_ok(cudaMemset(counts_all_, 0, sizeof(int) * kLayers * kExperts), "counts")) {
    error = "cannot allocate the routing counts";
    return false;
  }
  for (std::size_t l = 0; l < layers_.size(); ++l) layers_[l].counts = counts_all_ + l * kExperts;
  std::vector<std::int64_t> slot_bytes;
  for (const auto& L : layers_) slot_bytes.insert(slot_bytes.end(), static_cast<std::size_t>(S), static_cast<std::int64_t>(L.nl.bytes));
  std::string cerr;
  cache_.set_slot_guard(guard);
  if (S > 0 && !cache_.open_sized(slot_bytes, kLayers, kExperts, cerr)) {
    error = "cannot allocate the expert cache: " + cerr;
    return false;
  }
  // the experts each layer's slots start with: a usage profile's (paper 3.1, Strata's profile.bin), else 0..S-1
  std::vector<std::pair<int, int>> ranked;
  if (!profile.empty()) {
    std::int64_t pslots = 0;
    if (!strata::core::read_expert_profile(profile, kLayers, kExperts, profile_prior_, pslots, cerr)) {
      error = cerr;
      return false;
    }
    for (const auto& [l, e] : profile_prior_) ranked.emplace_back(l, e);
    report_.profile_filled = true;
  }
  const std::vector<std::vector<int>> fill = fill_from_profile(ranked, S);
  for (std::size_t l = 0; l < layers_.size(); ++l) {
    DevLayer& L = layers_[l];
    L.n_slots = S;
    L.slot0 = static_cast<int>(l) * S;
    L.slot_of.assign(kExperts, -1);
    L.expert_in.assign(static_cast<std::size_t>(S), -1);
    L.score.assign(kExperts, 0.0);
    L.ptr_host.resize(kExperts);
    for (int e = 0; e < kExperts; ++e)
      L.ptr_host[static_cast<std::size_t>(e)] = reinterpret_cast<unsigned long long>(L.host_alias + L.nl.bytes * static_cast<std::size_t>(e));
    for (int j = 0; j < S; ++j) {
      const int e = fill[l][static_cast<std::size_t>(j)];
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

bool Gemma4Engine::load(const EngineConfig& cfg, const ModelDesc& m, std::string& error) {
  const auto t0 = Clock::now();
  if (!file_.open(cfg.model)) {
    error = "cannot open the model file";
    return false;
  }
  ctx_ = cfg.max_context;
  chunk_ = cfg.chunk;
  graph_on_ = cfg.graph;
  ring_ = kWindow + std::max(cfg.chunk, kWindow);
  eps_ = m.rms_eps;
  base_full_ = m.rope_base_full;
  base_swa_ = m.rope_base_swa;
  softcap_ = m.softcap;
  if (!cuda_ok(cudaStreamCreateWithFlags(&st_, cudaStreamNonBlocking), "stream") ||
      !cuda_ok(cudaStreamCreateWithFlags(&copy_, cudaStreamNonBlocking), "stream") ||
      !cuda_ok(cudaEventCreateWithFlags(&counts_ev_, cudaEventDisableTiming), "event") ||
      !cuda_ok(cudaEventCreateWithFlags(&evict_ev_, cudaEventDisableTiming), "event") || cublasCreate(&blas_) != CUBLAS_STATUS_SUCCESS ||
      cublasSetStream(blas_, st_) != CUBLAS_STATUS_SUCCESS) {
    error = "cannot initialise CUDA or cuBLAS";
    return false;
  }
  if (!upload_matrix(m.token_embd, embd_) || !upload_vector(m.output_norm, output_norm_) || !upload_vector(m.rope_freqs, rope_freqs_)) {
    error = "cannot load the embedding";
    return false;
  }
  layers_.resize(kLayers);
  std::size_t pinned = 0;
  for (int i = 0; i < kLayers; ++i) {
    const LayerDesc& d = m.layers[static_cast<std::size_t>(i)];
    DevLayer& L = layers_[static_cast<std::size_t>(i)];
    L.index = i;
    L.sliding = d.sliding;
    L.hd = d.head_dim;
    L.n_kv = d.kv_heads;
    const bool ok = upload_vector(d.attn_norm, L.attn_norm) && upload_matrix(d.attn_q, L.q) && upload_matrix(d.attn_k, L.k) &&
                    (!d.attn_v.present || upload_matrix(d.attn_v, L.v)) && upload_matrix(d.attn_output, L.o) &&
                    upload_vector(d.attn_q_norm, L.q_norm) && upload_vector(d.attn_k_norm, L.k_norm) &&
                    upload_vector(d.post_attention_norm, L.post_attn_norm) && upload_vector(d.ffn_norm, L.ffn_norm) &&
                    upload_matrix(d.ffn_gate, L.gate) && upload_matrix(d.ffn_up, L.up) && upload_matrix(d.ffn_down, L.down) &&
                    upload_vector(d.post_ffw_norm_1, L.post_ffw_norm_1) && upload_matrix(d.gate_inp, L.router) &&
                    upload_vector(d.gate_inp_scale, L.gate_inp_scale) && upload_vector(d.pre_ffw_norm_2, L.pre_ffw_norm_2) &&
                    upload_vector(d.down_exps_scale, L.down_scale) && upload_vector(d.post_ffw_norm_2, L.post_ffw_norm_2) &&
                    upload_vector(d.post_ffw_norm, L.post_ffw_norm) && upload_vector(d.layer_output_scale, L.layer_scale) &&
                    load_experts(L, d);
    if (!ok) {
      error = "cannot load layer weights";
      return false;
    }
    pinned += L.nl.bytes * kExperts;
    L.cap = L.sliding ? ring_ : ctx_;
    const std::size_t cache = static_cast<std::size_t>(L.n_kv) * L.cap * L.hd;
    if (!alloc(L.kc, cache) || !alloc(L.vc, cache)) {
      error = "cannot allocate the KV cache (lower --max-context)";
      return false;
    }
    std::fprintf(stderr, "strata-gemma4: layer %d loaded (%s; experts %s/%s) at %.1f s\n", i, L.sliding ? "sliding" : "full",
                 L.nl.gu_type == 8 ? "Q8_0" : "BF16", L.nl.d_type == 8 ? "Q8_0" : "BF16",
                 std::chrono::duration<double>(Clock::now() - t0).count());
  }
  staging_.clear();
  staging_.shrink_to_fit();
  const std::size_t C = static_cast<std::size_t>(chunk_);
  max_splits_ = g4k::attn_decode_splits(ctx_);
  // P5: MMQ for the prompt's experts on the layers whose types it has (Q8_0; the BF16 ones keep the grouped
  // kernel), and the stream's geometry
  std::size_t mmq_blob = 0;
  const bool no_mmq = std::getenv("STRATA_G4_NO_MMQ") != nullptr;   // the A/B arm: the grouped kernel as before
  if (!no_mmq && MMQ::built() && MMQ::supported(8) && MMQ::fits(8, kGuRows) && MMQ::fits(8, kEmbd)) {
    for (auto& L : layers_) {
      L.mmq = L.nl.gu_type == 8 && L.nl.d_type == 8;
      if (L.mmq) mmq_blob = std::max(mmq_blob, L.nl.bytes);
    }
  }
  mmq_on_ = mmq_blob > 0 && chunk_ >= kMmqMinRows;
  if (mmq_on_) {
    // the guard covers MMQ's over-read past an expert's down rows, and keeps the slots' stride a whole number of
    // Q8_0 blocks as well as 256-byte aligned: MMQ steps from expert to expert in blocks (lcm(34, 256) = 4352 bytes)
    constexpr std::size_t kBlockGrid = 4352;
    const std::size_t over = std::max(MMQ::overread_bytes(8, kEmbd), MMQ::overread_bytes(8, kFfExp));
    mmq_guard_ = (mmq_blob + over + kBlockGrid - 1) / kBlockGrid * kBlockGrid - mmq_blob;
    // every layer's misses stream (the BF16 layers' too: read over PCIe in place, a group's weights would cross the
    // link once per tile of its entries); a ring buffer holds kRingGroup of the Q8_0 experts, fewer of the wider ones
    auto stride_of = [&](std::size_t bytes) {
      return static_cast<std::size_t>(strata::core::ExpertCache::slot_stride(static_cast<std::int64_t>(bytes), static_cast<std::int64_t>(mmq_guard_)));
    };
    std::size_t widest = 0;
    for (auto& L : layers_) {
      L.stream_stride = stride_of(L.nl.bytes);
      widest = std::max(widest, L.stream_stride);
    }
    ring_buf_bytes_ = std::max(std::size_t(kRingGroup) * stride_of(mmq_blob), widest);
    for (auto& L : layers_) L.stream_group = static_cast<int>(std::min<std::size_t>(kRingGroup, ring_buf_bytes_ / L.stream_stride));
  }
  // the row sets: a streamed prompt borrows big_ from the cache, so small_ only serves decode and batches below
  // kMmqMinRows; without the loan one set of C rows serves everything
  borrow_ = mmq_on_ && cfg.prompt_borrow;
  constexpr std::size_t kBigScores = std::size_t(32) << 20;   // 128 MB of prompt-attention scores
  const std::size_t small_rows = borrow_ ? std::size_t(kMmqMinRows - 1) : C;
  const std::size_t small_scores = borrow_ ? std::max<std::size_t>(std::size_t(4) << 20, std::size_t(8) * static_cast<std::size_t>(ctx_))
                                           : kBigScores;
  std::uint8_t* small_mem = nullptr;
  const std::size_t small_bytes = layout_rows(nullptr, small_rows, small_scores, mmq_on_ && !borrow_, small_);
  const bool scratch_ok =
      alloc(small_mem, small_bytes) && alloc(ids_d_, std::max<std::size_t>(C, 2)) && alloc(wts_, C * 8) && alloc(picks_, C * 8) &&
      alloc(wdq_, std::size_t(kHeads) * kHeadFull * kEmbd) &&
      alloc(plan_ptr_, kExperts) && alloc(plan_start_, kExperts + 1) && alloc(plan_n_, 1) && alloc(plan_dst_, C * 8) &&
      alloc(plan_tok_, C * 8) && alloc(ident_, C * 8) && alloc(dscratch_, std::size_t(kHeads) * max_splits_ * (kHeadFull + 2)) &&
      alloc(logits_, kVocab) && alloc(xn_, kEmbd) && alloc(argmax_out_, 1) && alloc(argmax_scratch_, 4096) &&
      alloc(topk_scratch_, g4k::topk_scratch_bytes(kVocab)) &&
      alloc(cand_v_, g4k::kTopkMax) && alloc(cand_i_, g4k::kTopkMax) && alloc(cand_flag_, 1);
  if (!scratch_ok) {
    error = "cannot allocate the scratch buffers (lower --chunk or --max-context)";
    return false;
  }
  layout_rows(small_mem, small_rows, small_scores, mmq_on_ && !borrow_, small_);
  if (small_.ring != nullptr && !cuda_ok(cudaMemset(small_.ring, 0, std::size_t(kRingBufs) * ring_buf_bytes_), "ring")) {   // the guards stay zero
    error = "cannot initialise the expert stream";
    return false;
  }
  big_ = small_;   // until a loan (or big_'s own VRAM) is laid out below
  bind(small_);
  void* pin = nullptr;
  // ids (2C), picks (8C), the prompt plan (n, start[129], dst 8C, tok 8C), argmax, then the routing counts
  const std::size_t pin_ints = 2 * C + 8 * C + (1 + kExperts + 1 + 16 * C) + 16 + static_cast<std::size_t>(kLayers) * kExperts;
  if (!cuda_ok(cudaHostAlloc(&pin, pin_ints * sizeof(int), cudaHostAllocDefault), "cudaHostAlloc")) {
    error = "cannot pin the transfer buffers";
    return false;
  }
  pinned_.push_back(pin);
  ids_h_ = static_cast<int*>(pin);
  picks_h_ = ids_h_ + 2 * C;
  plan_h_ = picks_h_ + 8 * C;
  argmax_h_ = plan_h_ + (1 + kExperts + 1 + 16 * C);
  counts_h_ = argmax_h_ + 16;
  void* ppin = nullptr;
  if (!cuda_ok(cudaHostAlloc(&ppin, sizeof(unsigned long long) * kExperts, cudaHostAllocDefault), "cudaHostAlloc")) {
    error = "cannot pin the transfer buffers";
    return false;
  }
  pinned_.push_back(ppin);
  plan_ptr_h_ = static_cast<unsigned long long*>(ppin);
  {
    std::vector<int> ident(C * 8);
    std::iota(ident.begin(), ident.end(), 0);
    if (!cuda_ok(cudaMemcpy(ident_, ident.data(), ident.size() * sizeof(int), cudaMemcpyHostToDevice), "ident")) {
      error = "cannot initialise the expert plan";
      return false;
    }
  }
  void* cand_pin = nullptr;
  if (!cuda_ok(cudaHostAlloc(&cand_pin, sizeof(int) * (1 + 2 * g4k::kTopkMax), cudaHostAllocDefault), "cudaHostAlloc")) {
    error = "cannot pin the transfer buffers";
    return false;
  }
  pinned_.push_back(cand_pin);
  cand_h_ = static_cast<int*>(cand_pin);
  // P5's fixed pieces: the row tables and plans (pinned staging and their device copies), the stream and its events
  if (mmq_on_) {
    const std::size_t A = C * 8;
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
    miss_.assign(kLayers, {});
  }
  if (cfg.cpu_lane) {
    // the copy engine's staging (4 of the largest blobs: a Q8_0 layer stages up to 7) and the step's buffers, before
    // the expert slots take the free VRAM
    std::size_t max_blob = 0;
    for (const auto& L : layers_) max_blob = std::max(max_blob, L.nl.bytes);
    lstage_bytes_ = 4 * max_blob;
    if (!alloc(lstage_, lstage_bytes_) || !alloc(lplan_, LanePlan::kWords) || !alloc(hit_out_, std::size_t(LanePlan::kCap) * kEmbd) ||
        !alloc(res_d_, static_cast<std::size_t>(kLayers) * kExperts)) {
      error = "cannot allocate the CPU lane's buffers";
      return false;
    }
  }
  profile_save_ = cfg.expert_profile_save;
  profile_saved_ = Clock::now();
  if (!place_experts(cfg.expert_vram_gib, cfg.expert_slots, cfg.expert_profile, error)) return false;
  if (borrow_) {
    // big_ on the cache's top slots (paper 3.6: the prompt's buffers are borrowed from the expert cache): the fewest
    // slots from the arena's end that hold it, at most half of them; else VRAM of its own
    const std::size_t big_bytes = layout_rows(nullptr, C, kBigScores, true, big_);
    const int N = kLayers * layers_[0].n_slots;
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
      borrow_ = false;   // big_ is its own: nothing to lend or give back
    }
  }
  if (cfg.cpu_lane && report_.slots_per_layer < kExperts && !start_lane(cfg, error)) return false;
  std::size_t free_b = 0, total_b = 0;
  cudaMemGetInfo(&free_b, &total_b);
  report_.vram_used_mib = double(total_b - free_b) / double(1 << 20);
  report_.vram_total_mib = double(total_b) / double(1 << 20);
  report_.pinned_gib = double(pinned) / double(1ull << 30);
  report_.ring_rows = ring_;
  report_.load_seconds = std::chrono::duration<double>(Clock::now() - t0).count();
  file_.close();
  return !broken_;
}

bool Gemma4Engine::sgemm(int rows, int T, int cols, const float* w, const float* x, float* y) {
  const float one = 1.0f, zero = 0.0f;
  // row-major y[T][rows] = x[T][cols] * w[rows][cols]^T  ==  column-major y^T = w^T(op T) * x^T
  return cublasSgemm(blas_, CUBLAS_OP_T, CUBLAS_OP_N, rows, T, cols, &one, w, cols, x, cols, &zero, y, rows) ==
         CUBLAS_STATUS_SUCCESS;
}

bool Gemma4Engine::gemm(const g4k::Mat& w, const float* x, float* y, int T) {
  const float* wf = static_cast<const float*>(w.w);
  if (w.kind != g4k::WKind::f32) {
    g4k::dequant(w, wdq_, st_);
    wf = wdq_;
  }
  if (!sgemm(w.rows, T, w.cols, wf, x, y)) {
    std::fprintf(stderr, "strata-gemma4: cuBLAS SGEMM failed\n");
    broken_ = true;
    return false;
  }
  return true;
}

void Gemma4Engine::dump_layer(int layer, int pos0, int T) {
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

bool Gemma4Engine::head(const float* x_row) {
  g4k::rmsnorm(x_row, output_norm_, nullptr, 1.0f, xn_, 1, kEmbd, eps_, st_);
  g4k::matvec(embd_, xn_, logits_, 1, st_);
  g4k::softcap(logits_, kVocab, softcap_, st_);
  return launch_ok("head");
}

// One decode step (paper 3.6, finding 7): the whole token as ONE captured graph, replayed every step.  Everything
// that changes per step is device data - the token and its position (ids_h_[0..1], copied in by the graph's first
// node), the residency the doorbell reads, the lane's plan - so a graph serves every position whose attention
// splits it was captured with (graph_key_: a new capture each 128 positions, when a split is added).
bool Gemma4Engine::forward_one(std::int32_t id) {
  const int pos = pos_;
  // the last step's (or chunk's) work is done: its copy out of ids_h_ has run before ids_h_ is rewritten below (a
  // prompt tail of 2-4 tokens queues steps back to back), and the lane is idle
  if (!cuda_ok(cudaStreamSynchronize(st_), "decode")) return false;
  if (!swaps_.empty()) land_swaps();   // the swaps (a prompt's refilled slots among them) whose copies have landed
  // the CPU lane serves this step's layers as they ring; the residency it plans from changes only between steps
  const bool lane = lane_ && lane_->running();
  if (lane) {
    if (res_dirty_) {
      if (!cuda_ok(cudaMemcpyAsync(res_d_, res_h_, sizeof(std::int32_t) * kLayers * kExperts, cudaMemcpyHostToDevice, st_), "residency"))
        return false;
      res_dirty_ = false;
    }
  }
  ids_h_[0] = id;
  ids_h_[1] = pos;
  const int split_full = g4k::attn_decode_splits(g4k::attn_decode_keys(pos, 0));
  const int split_swa = g4k::attn_decode_splits(g4k::attn_decode_keys(pos, kWindow));
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
      if (!ok) {   // the step runs as plain launches from here on
        std::fprintf(stderr, "strata-gemma4: the decode graph could not be captured (%s); plain launches from here\n", why.c_str());
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
      std::fprintf(stderr, "strata-gemma4: %s\n", why.c_str());
      if (lane) lane_->abort_token();
      broken_ = true;
      return false;
    }
  } else {
    if (lane) lane_->begin_token();
    if (!decode_body(pos, lane, split_full, split_swa)) {
      if (lane) lane_->abort_token();   // a step that could not be queued whole: the lane releases the GPU
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
      std::fprintf(stderr, "strata-gemma4: %s\n", why.c_str());
      broken_ = true;
      return false;
    }
  }
  pos_ = pos + 1;
  return true;
}

// The step's launches (captured by forward_one, or run as they are): only device data changes between steps.
bool Gemma4Engine::decode_body(int pos, bool lane, int split_full, int split_swa) {
  const int* dpos = ids_d_ + 1;
  cudaMemcpyAsync(ids_d_, ids_h_, 2 * sizeof(int), cudaMemcpyHostToDevice, st_);
  g4k::embed_q8(ids_d_, 1, static_cast<const std::int8_t*>(embd_.w), embd_.s, kEmbd, std::sqrt(float(kEmbd)), x_, st_);
  for (int l = 0; l < kLayers; ++l) {
    DevLayer& L = layers_[static_cast<std::size_t>(l)];
    g4k::rmsnorm(x_, L.attn_norm, nullptr, 1.0f, h_, 1, kEmbd, eps_, st_);
    g4k::matvec(L.q, h_, qraw_, 1, st_);
    g4k::matvec(L.k, h_, kraw_, 1, st_);
    if (L.sliding) g4k::matvec(L.v, h_, vraw_, 1, st_);
    g4k::QkvPost p{qraw_, kraw_, L.sliding ? vraw_ : kraw_, L.q_norm, L.k_norm, L.sliding ? nullptr : rope_freqs_,
                   L.sliding ? base_swa_ : base_full_, L.hd, kHeads, L.n_kv, pos, 1, L.sliding ? ring_ : 0, L.cap, eps_,
                   qout_, nullptr, L.kc, L.vc, 0.0f, dpos};
    g4k::qkv_post(p, st_);
    g4k::attn_decode_dev(qout_, L.kc, L.vc, L.hd, kHeads, L.n_kv, L.cap, L.sliding ? ring_ : 0, L.sliding ? kWindow : 0, dpos,
                         L.sliding ? split_swa : split_full, attn_, dscratch_, max_splits_, st_);
    g4k::matvec(L.o, attn_, o_, 1, st_);
    g4k::add_rmsnorm(x_, o_, L.post_attn_norm, x1_, 1, kEmbd, eps_, st_);
    g4k::rmsnorm(x1_, L.ffn_norm, nullptr, 1.0f, h_, 1, kEmbd, eps_, st_);
    g4k::matvec(L.gate, h_, g_, 1, st_);
    g4k::matvec(L.up, h_, u_, 1, st_);
    g4k::geglu(g_, kFf, u_, kFf, m_, 1, kFf, st_);
    g4k::matvec(L.down, m_, dense_, 1, st_);
    g4k::rmsnorm(x1_, nullptr, L.gate_inp_scale, 1.0f / std::sqrt(float(kEmbd)), rn_, 1, kEmbd, eps_, st_);
    g4k::matvec(L.router, rn_, rl_, 1, st_);
    g4k::router_topk(rl_, 1, picks_, wts_, L.counts, st_);
    g4k::rmsnorm(x1_, L.pre_ffw_norm_2, nullptr, 1.0f, e_in_, 1, kEmbd, eps_, st_);
    if (!(lane ? moe_lane(L) : moe_experts(L, 1))) return false;
    g4k::ffn_end(x1_, dense_, moe_, L.post_ffw_norm_1, L.post_ffw_norm_2, L.post_ffw_norm, L.layer_scale, x_, 1, kEmbd, eps_, st_);
    if (sink_ != nullptr) dump_layer(l, pos, 1);
  }
  return head(x_);
}

bool Gemma4Engine::prompt_attention(DevLayer& L, int pos0, int T) {
  const int ring = L.sliding ? ring_ : 0;
  const int Lk = L.sliding ? std::min(ring_, pos0 + T) : pos0 + T;
  const int G = kHeads / L.n_kv;
  const int hd = L.hd;
  const int Ts = static_cast<int>(std::max<std::size_t>(1, std::min<std::size_t>(T, s_floats_ / (std::size_t(G) * Lk))));
  const float one = 1.0f, zero = 0.0f;
  for (int g = 0; g < L.n_kv; ++g) {
    const __half* K = L.kc + static_cast<std::size_t>(g) * L.cap * hd;
    const __half* V = L.vc + static_cast<std::size_t>(g) * L.cap * hd;
    for (int t0 = 0; t0 < T; t0 += Ts) {
      const int ts = std::min(Ts, T - t0);
      const __half* Q = q16_ + (static_cast<std::size_t>(t0) * kHeads + static_cast<std::size_t>(g) * G) * hd;
      float* O = attn_ + (static_cast<std::size_t>(t0) * kHeads + static_cast<std::size_t>(g) * G) * hd;
      if (cublasGemmStridedBatchedEx(blas_, CUBLAS_OP_T, CUBLAS_OP_N, Lk, ts, hd, &one, K, CUDA_R_16F, hd, 0, Q, CUDA_R_16F,
                                     kHeads * hd, hd, &zero, s_buf_, CUDA_R_32F, Lk, static_cast<long long>(ts) * Lk, G,
                                     CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT) != CUBLAS_STATUS_SUCCESS) {
        std::fprintf(stderr, "strata-gemma4: cuBLAS attention scores failed\n");
        broken_ = true;
        return false;
      }
      g4k::attn_softmax(s_buf_, p_buf_, G, ts, Lk, pos0 + t0, ring, L.sliding ? kWindow : 0, pos0 + T - 1, st_);
      if (cublasGemmStridedBatchedEx(blas_, CUBLAS_OP_N, CUBLAS_OP_N, hd, ts, Lk, &one, V, CUDA_R_16F, hd, 0, p_buf_, CUDA_R_16F,
                                     Lk, static_cast<long long>(ts) * Lk, &zero, O, CUDA_R_32F, kHeads * hd, hd, G,
                                     CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT) != CUBLAS_STATUS_SUCCESS) {
        std::fprintf(stderr, "strata-gemma4: cuBLAS attention output failed\n");
        broken_ = true;
        return false;
      }
    }
  }
  return launch_ok("prompt attention");
}

// The layer's routed experts for T tokens, through Strata's grouped expert kernel on the native blobs: the input
// e_in_ as q8_1 blocks (llama.cpp's CUDA contract), one group per expert that has entries, each group's blob read
// where the expert lives (its VRAM slot, else the pinned arena over PCIe).  Output rows yg_[t*8 + k], then the
// combine: moe[t] = sum_k w[t][k] * down_exps.scale[e] * yg_[t*8 + k].
bool Gemma4Engine::moe_experts(DevLayer& L, int T) {
  const int A = T * 8;
  strata::kernels::quantize_q8_1_rows(e_in_, T, kEmbd, xq_, st_);
  int groups = 8;
  if (T == 1) {
    g4k::plan_decode(picks_, 8, L.ptrtab, plan_ptr_, plan_start_, plan_n_, plan_dst_, plan_tok_, st_);
  } else {
    // the prompt: group the chunk's entries by expert on the host (one picks read-back a layer)
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
      tok[i] = a / 8;
    }
    if (!cuda_ok(cudaMemcpyAsync(plan_n_, plan_h_, sizeof(int), cudaMemcpyHostToDevice, st_), "plan") ||
        !cuda_ok(cudaMemcpyAsync(plan_start_, start, sizeof(int) * (groups + 1), cudaMemcpyHostToDevice, st_), "plan") ||
        !cuda_ok(cudaMemcpyAsync(plan_dst_, dst, sizeof(int) * A, cudaMemcpyHostToDevice, st_), "plan") ||
        !cuda_ok(cudaMemcpyAsync(plan_tok_, tok, sizeof(int) * A, cudaMemcpyHostToDevice, st_), "plan") ||
        !cuda_ok(cudaMemcpyAsync(plan_ptr_, plan_ptr_h_, sizeof(unsigned long long) * groups, cudaMemcpyHostToDevice, st_), "plan")) {
      return false;
    }
  }
  strata::kernels::native_expert_grouped(L.nl, plan_ptr_, plan_start_, plan_n_, plan_dst_, plan_tok_, groups, A, xq_, escratch_,
                                         yg_, st_);
  if (T == 1) g4k::moe_combine(yg_, wts_, picks_, L.down_scale, 8, kEmbd, moe_, st_);
  else g4k::scatter_combine(yg_, ident_, wts_, picks_, L.down_scale, T, kEmbd, moe_, st_);
  return launch_ok("experts");
}

// The chunk's misses: every layer's experts without a slot (a prompt chunk of kMmqMinRows tokens or more routes to
// nearly all 128, so they are streamed whatever the routing), in groups of the layer's stream_group (as many as one
// ring buffer holds, at most kRingGroup), layer by layer; the first kRingBufs groups start copying now.
bool Gemma4Engine::ring_begin() {
  ring_seq_.clear();
  ring_next_ = 0;
  for (const auto& L : layers_) {
    auto& m = miss_[static_cast<std::size_t>(L.index)];
    m.clear();
    for (int e = 0; e < kExperts; ++e)
      if (L.slot_of[static_cast<std::size_t>(e)] < 0) m.push_back(e);   // in RAM only, or a swap still landing
    for (int f = 0; f < static_cast<int>(m.size()); f += L.stream_group)
      ring_seq_.push_back({L.index, f, std::min(L.stream_group, static_cast<int>(m.size()) - f)});
  }
  for (std::size_t i = 0; i < ring_seq_.size() && i < std::size_t(kRingBufs); ++i)
    if (!ring_kick(i)) return false;
  return true;
}

bool Gemma4Engine::ring_kick(std::size_t i) {
  const RingGroup& g = ring_seq_[i];
  const std::size_t k = i % kRingBufs;
  const DevLayer& L = layers_[static_cast<std::size_t>(g.layer)];
  // the copy engine reuses buffer k once the compute stream is past its last group's products
  if (!cuda_ok(cudaStreamWaitEvent(ring_st_, ring_freed_[k], 0), "event")) return false;
  std::uint8_t* dst = ring_buf_ + k * ring_buf_bytes_;
  // an MMQ group's guards (after each blob) must be zero for MMQ's over-read: a buffer a wider layer's group or a fresh
  // loan left bytes in is zeroed once first (an MMQ group's own copies never write a guard)
  if (L.mmq && ring_dirty_[k]) {
    if (!cuda_ok(cudaMemsetAsync(dst, 0, ring_buf_bytes_, ring_st_), "expert stream")) return false;
    ring_dirty_[k] = false;
  }
  if (!L.mmq) ring_dirty_[k] = true;
  for (int j = 0; j < g.n; ++j) {
    const int e = miss_[static_cast<std::size_t>(g.layer)][static_cast<std::size_t>(g.first + j)];
    if (!cuda_ok(cudaMemcpyAsync(dst + static_cast<std::size_t>(j) * L.stream_stride, L.host_experts + L.nl.bytes * static_cast<std::size_t>(e),
                                 L.nl.bytes, cudaMemcpyHostToDevice, ring_st_),
                 "expert stream"))
      return false;
  }
  return cuda_ok(cudaEventRecord(ring_landed_[k], ring_st_), "event");
}

// The layer's g-th streamed group, once its copies have landed: its ring buffer (nullptr when the stream is out of
// step with the layers, which would compute the wrong experts).
const std::uint8_t* Gemma4Engine::ring_take(const DevLayer& L, int g) {
  const std::size_t i = ring_next_;
  if (i >= ring_seq_.size() || ring_seq_[i].layer != L.index || ring_seq_[i].first != g * L.stream_group) {
    std::fprintf(stderr, "strata-gemma4: the expert stream is out of step (layer %d)\n", L.index);
    broken_ = true;
    return nullptr;
  }
  const std::size_t k = i % kRingBufs;
  if (!cuda_ok(cudaStreamWaitEvent(st_, ring_landed_[k], 0), "event")) return nullptr;
  return ring_buf_ + k * ring_buf_bytes_;
}

// The group ring_take gave is computed (in stream order): its buffer takes the group kRingBufs later.
bool Gemma4Engine::ring_give() {
  const std::size_t i = ring_next_++;
  if (!cuda_ok(cudaEventRecord(ring_freed_[i % kRingBufs], st_), "event")) return false;
  return i + kRingBufs >= ring_seq_.size() || ring_kick(i + kRingBufs);
}

// The routed entries of a prompt chunk (picks_h_, A of them) sorted by their expert's position - slot j's expert at j
// (none for an empty or landing slot), the layer's miss q at S + q, the order they stream in: bounds[P + 1] the rows
// of each position, inv[a] the row of entry a, ent[r] / tok[r] the entry and the token of row r (each may be null).
// Returns P; -1 (the engine broken) for an expert without a position.
int Gemma4Engine::stream_plan(const DevLayer& L, int A, int* bounds, int* inv, int* ent, int* tok) {
  const int S = L.n_slots;
  const auto& miss = miss_[static_cast<std::size_t>(L.index)];
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
      std::fprintf(stderr, "strata-gemma4: a routed expert has no position in the prompt's plan\n");
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
    if (tok != nullptr) tok[r] = a / 8;
  }
  return P;
}

// A streamed prompt chunk's experts on a layer MMQ has no kernel for (BF16: the Q8_K_XL file's layers 1 and 29):
// Strata's grouped kernel, from the slots and then from each streamed group once it has landed - never over PCIe in
// place, where a group's weights would be read once per tile of its entries.
bool Gemma4Engine::moe_ring_grouped(DevLayer& L, int T) {
  const int A = T * 8;
  if (!cuda_ok(cudaMemcpyAsync(picks_h_, picks_, sizeof(int) * A, cudaMemcpyDeviceToHost, st_), "picks") ||
      !cuda_ok(cudaStreamSynchronize(st_), "sync")) {
    return false;
  }
  int bounds[kExperts + kExperts + 1];
  int* dst = plan_h_ + 1 + (kExperts + 1);   // the grouped path's entry tables: row -> entry, row -> token
  int* tok = dst + A;
  const int P = stream_plan(L, A, bounds, nullptr, dst, tok);
  if (P < 0) return false;
  const int S = L.n_slots, G = L.stream_group;
  const int calls = 1 + (P - S + G - 1) / G;   // the slots, then each streamed group
  // per call c, at rg_h_ + c * kRgCallInts: its group count, its groups' starts (count + 1) and blob addresses -
  // slot p's for the slots, else the place of the position in its group's ring buffer (group i of the stream always
  // takes buffer i % kRingBufs, and this layer's groups are the next ones)
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
  strata::kernels::quantize_q8_1_rows(e_in_, T, kEmbd, xq_, st_);
  for (int c = 0; c < calls; ++c) {
    if (c > 0 && ring_take(L, c - 1) == nullptr) return false;
    const int n = rg_h_[static_cast<std::size_t>(c) * kRgCallInts];
    const int* w = rg_d_ + static_cast<std::size_t>(c) * kRgCallInts;
    if (n > 0)
      strata::kernels::native_expert_grouped(L.nl, reinterpret_cast<const unsigned long long*>(w + kRgPtr), w + kRgStart, w, plan_dst_,
                                             plan_tok_, n, A, xq_, escratch_, yg_, st_);
    if (c > 0 && !ring_give()) return false;
  }
  g4k::scatter_combine(yg_, ident_, wts_, picks_, L.down_scale, T, kEmbd, moe_, st_);
  return launch_ok("experts (streamed)");
}

// A prompt chunk's routed experts on llama.cpp's MMQ (the Qwen family's prompt path, for Gemma's blobs and GEGLU):
// the entries sorted by their expert's position - its slot for a resident expert, else after the slots in the order
// the misses stream - then for the slots and for each streamed group: gate+up as one [1408] product per expert,
// GEGLU, the hidden quantized, down into the entry's row of yg_, and the combine as the grouped path's.
bool Gemma4Engine::moe_mmq(DevLayer& L, int T) {
  const int A = T * 8;
  if (!cuda_ok(cudaMemcpyAsync(picks_h_, picks_, sizeof(int) * A, cudaMemcpyDeviceToHost, st_), "picks") ||
      !cuda_ok(cudaStreamSynchronize(st_), "sync")) {
    return false;
  }
  const int S = L.n_slots, G = L.stream_group;
  int* bounds = mq_pin_;                     // [P + 1] absolute row bounds per position
  int* inv = mq_pin_ + kMqBoundsInts;        // [A] entry (t * 8 + k) -> its sorted row (the combine's)
  int* tok = inv + A;                        // [A] sorted row -> token
  const int P = stream_plan(L, A, bounds, inv, nullptr, tok);
  if (P < 0) return false;
  const int M = P - S;
  int* rel = bounds + (P + 1);               // per streamed group: its n + 1 bounds less its first row
  int most_slots = 0;
  for (int p = 0; p < S; ++p) most_slots = std::max(most_slots, bounds[p + 1] - bounds[p]);
  const int groups = (M + G - 1) / G;
  for (int g = 0; g < groups; ++g) {
    const int f = g * G, n = std::min(G, M - f);
    for (int j = 0; j <= n; ++j) rel[g * (kRingGroup + 1) + j] = bounds[S + f + j] - bounds[S + f];
  }
  const int words = P + 1 + groups * (kRingGroup + 1);
  if (!cuda_ok(cudaMemcpyAsync(mq_bounds_, bounds, sizeof(int) * static_cast<std::size_t>(words), cudaMemcpyHostToDevice, st_), "plan") ||
      !cuda_ok(cudaMemcpyAsync(mq_ids_, inv, sizeof(int) * static_cast<std::size_t>(2 * A), cudaMemcpyHostToDevice, st_), "plan")) {
    return false;
  }
  const int* inv_d = mq_ids_;       // inv and tok are uploaded back to back
  const int* tok_d = mq_ids_ + A;
  MMQ::quantize(e_in_, tok_d, mq_x_, 8, kEmbd, kEmbd, A, st_);
  // gate+up over `n` experts lying `stride` apart from `w` (rows from the absolute bounds), GEGLU and the hidden's
  // quantization over their rows [r0, r1), then down into rows r0.. of yg_ (the relative bounds): yg_ holds the rows
  // in sorted order, which the combine reads through inv (MMQ's row table stays the identity, as the Qwen path's)
  auto experts = [&](const std::uint8_t* w, std::size_t stride, int n, const int* abs_bounds, const int* rel_bounds, int r0, int r1,
                     int most) {
    if (r1 <= r0) return;
    MMQ::Product gu;
    gu.w = w; gu.type = 8; gu.w_rows = kGuRows; gu.w_cols = kEmbd; gu.expert_bytes = stride; gu.n = n;
    gu.xq = mq_x_; gu.bounds = abs_bounds; gu.ids = ident_; gu.total_rows = A; gu.max_rows = most;
    gu.dst = mq_gu_; gu.ld_dst = kGuRows;
    mctx_->run(gu, st_);
    float* gr = mq_gu_ + static_cast<std::size_t>(r0) * kGuRows;
    g4k::geglu(gr, kGuRows, gr + kFfExp, kGuRows, mq_h_ + static_cast<std::size_t>(r0) * kFfExp, r1 - r0, kFfExp, st_);
    MMQ::quantize(mq_h_ + static_cast<std::size_t>(r0) * kFfExp, nullptr, mq_hq_, 8, kFfExp, kFfExp, r1 - r0, st_);
    MMQ::Product dn;
    dn.w = w + L.nl.down_off; dn.type = 8; dn.w_rows = kEmbd; dn.w_cols = kFfExp; dn.expert_bytes = stride; dn.n = n;
    dn.xq = mq_hq_; dn.bounds = rel_bounds; dn.ids = ident_; dn.total_rows = r1 - r0; dn.max_rows = most;
    dn.dst = yg_ + static_cast<std::size_t>(r0) * kEmbd; dn.ld_dst = kEmbd;
    mctx_->run(dn, st_);
  };
  // the resident experts, from their slots (the layer's slots lie one stream_stride apart)
  if (S > 0) experts(cache_.device_slot(L.slot0), L.stream_stride, S, mq_bounds_, mq_bounds_, 0, bounds[S], most_slots);
  for (int g = 0; g < groups; ++g) {   // the streamed ones, each group once its copies have landed
    const int f = g * G, n = std::min(G, M - f);
    int most = 0;
    for (int j = 0; j < n; ++j) most = std::max(most, bounds[S + f + j + 1] - bounds[S + f + j]);
    const std::uint8_t* buf = ring_take(L, g);
    if (buf == nullptr) return false;
    experts(buf, L.stream_stride, n, mq_bounds_ + S + f, mq_bounds_ + (P + 1) + g * (kRingGroup + 1), bounds[S + f], bounds[S + f + n],
            most);
    if (!ring_give()) return false;
  }
  g4k::scatter_combine(yg_, inv_d, wts_, picks_, L.down_scale, T, kEmbd, moe_, st_);
  return launch_ok("experts (MMQ)");
}

bool Gemma4Engine::forward_batch(const std::int32_t* ids, int T) {
  const int pos0 = pos_;
  const bool mmq = mmq_on_ && T >= kMmqMinRows;   // P5: the experts on MMQ, the misses streamed
  if (static_cast<std::size_t>(T) > rows_ || (mmq && ring_buf_ == nullptr)) {   // process binds the set that holds the chunk
    std::fprintf(stderr, "strata-gemma4: a prompt chunk of %d rows on a row set of %zu\n", T, rows_);
    broken_ = true;
    return false;
  }
  if (mmq && !ring_begin()) return false;
  std::memcpy(ids_h_, ids, sizeof(int) * static_cast<std::size_t>(T));
  cudaMemcpyAsync(ids_d_, ids_h_, sizeof(int) * static_cast<std::size_t>(T), cudaMemcpyHostToDevice, st_);
  g4k::embed_q8(ids_d_, T, static_cast<const std::int8_t*>(embd_.w), embd_.s, kEmbd, std::sqrt(float(kEmbd)), x_, st_);
  for (int l = 0; l < kLayers; ++l) {
    DevLayer& L = layers_[static_cast<std::size_t>(l)];
    g4k::rmsnorm(x_, L.attn_norm, nullptr, 1.0f, h_, T, kEmbd, eps_, st_);
    if (!gemm(L.q, h_, qraw_, T) || !gemm(L.k, h_, kraw_, T) || (L.sliding && !gemm(L.v, h_, vraw_, T))) return false;
    g4k::QkvPost p{qraw_, kraw_, L.sliding ? vraw_ : kraw_, L.q_norm, L.k_norm, L.sliding ? nullptr : rope_freqs_,
                   L.sliding ? base_swa_ : base_full_, L.hd, kHeads, L.n_kv, pos0, T, L.sliding ? ring_ : 0, L.cap, eps_,
                   qout_, q16_, L.kc, L.vc};
    g4k::qkv_post(p, st_);
    if (!prompt_attention(L, pos0, T)) return false;
    if (!gemm(L.o, attn_, o_, T)) return false;
    g4k::add_rmsnorm(x_, o_, L.post_attn_norm, x1_, T, kEmbd, eps_, st_);
    g4k::rmsnorm(x1_, L.ffn_norm, nullptr, 1.0f, h_, T, kEmbd, eps_, st_);
    if (!gemm(L.gate, h_, g_, T) || !gemm(L.up, h_, u_, T)) return false;
    g4k::geglu(g_, kFf, u_, kFf, m_, T, kFf, st_);
    if (!gemm(L.down, m_, dense_, T)) return false;
    g4k::rmsnorm(x1_, nullptr, L.gate_inp_scale, 1.0f / std::sqrt(float(kEmbd)), rn_, T, kEmbd, eps_, st_);
    if (!gemm(L.router, rn_, rl_, T)) return false;
    g4k::router_topk(rl_, T, picks_, wts_, L.counts, st_);
    g4k::rmsnorm(x1_, L.pre_ffw_norm_2, nullptr, 1.0f, e_in_, T, kEmbd, eps_, st_);
    if (!(mmq ? (L.mmq ? moe_mmq(L, T) : moe_ring_grouped(L, T)) : moe_experts(L, T))) return false;
    g4k::ffn_end(x1_, dense_, moe_, L.post_ffw_norm_1, L.post_ffw_norm_2, L.post_ffw_norm, L.layer_scale, x_, T, kEmbd, eps_, st_);
    if (sink_ != nullptr) dump_layer(l, pos0, T);
  }
  if (mmq && ring_next_ != ring_seq_.size()) {
    std::fprintf(stderr, "strata-gemma4: the expert stream ended out of step\n");
    broken_ = true;
    return false;
  }
  if (!head(x_ + static_cast<std::size_t>(T - 1) * kEmbd)) return false;
  pos_ = pos0 + T;
  return true;
}

int Gemma4Engine::process(const std::int32_t* ids, int n, const std::function<bool(int)>& progress) {
  if (broken_) return -1;
  if (n <= 0 || pos_ + n > ctx_) return 0;
  int done = 0;
  while (done < n) {
    const int left = n - done;
    if (left <= 4) {
      if (!give_back()) return -1;   // decode steps run on small_ (their graph was captured on it)
      for (int i = 0; i < left; ++i) {
        if (!forward_one(ids[done + i])) return -1;
      }
      done = n;
    } else {
      const int T = std::min(left, chunk_);
      // a streamed chunk on big_ (the cache's lent slots), a short one on small_
      if (!(mmq_on_ && T >= kMmqMinRows ? lend() : give_back())) return -1;
      if (!forward_batch(ids + done, T)) return -1;
      done += T;
      if (n > 1) rebalance(256);
    }
    if (!cuda_ok(cudaStreamSynchronize(st_), "process")) return -1;
    if (n > 1 && progress && !progress(done) && done < n) break;
  }
  if (!give_back()) return -1;
  // the prompt's routing to the tier: a rebalance reads the counts the previous one asked for (landed with the sync
  // above), so a prompt of one chunk - whose only rebalance started that read - moves the slots before decode too
  if (n > 4) rebalance(256);
  return done;
}

bool Gemma4Engine::logits(std::vector<float>& out) {
  out.resize(kVocab);
  return cuda_ok(cudaMemcpyAsync(out.data(), logits_, sizeof(float) * kVocab, cudaMemcpyDeviceToHost, st_), "logits") &&
         cuda_ok(cudaStreamSynchronize(st_), "logits");
}

int Gemma4Engine::greedy() {
  g4k::argmax(logits_, kVocab, argmax_out_, argmax_scratch_, st_);
  if (!cuda_ok(cudaMemcpyAsync(argmax_h_, argmax_out_, sizeof(int), cudaMemcpyDeviceToHost, st_), "argmax") ||
      !cuda_ok(cudaStreamSynchronize(st_), "argmax")) {
    return -1;
  }
  return argmax_h_[0];
}

int Gemma4Engine::top_candidates(int k, float* vals, int* ids) {
  if (k <= 0 || k > g4k::kTopkMax) return -1;
  cudaMemsetAsync(cand_flag_, 0, sizeof(int), st_);
  g4k::topk(logits_, kVocab, k, cand_v_, cand_i_, cand_flag_, topk_scratch_, st_);
  float* hv = reinterpret_cast<float*>(cand_h_ + 1 + g4k::kTopkMax);
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

// What the adaptive tier learned, as Strata's profile (rank_learned_profile): the experts in a slot (or on their way
// to one) first, then by their decayed routing counts, then by the profile the engine started from.
bool Gemma4Engine::save_profile(bool force, std::string& error) {
  if (profile_save_.empty() || layers_.empty()) return true;
  const auto now = Clock::now();
  if (!force && std::chrono::duration<double>(now - profile_saved_).count() < kProfileSaveMinutes * 60.0) return true;
  std::vector<std::uint8_t> resident(static_cast<std::size_t>(kLayers) * kExperts, 0);
  std::vector<double> heat(resident.size(), 0.0);
  for (const auto& L : layers_) {
    for (int e = 0; e < kExperts; ++e) {
      const std::size_t i = static_cast<std::size_t>(L.index) * kExperts + static_cast<std::size_t>(e);
      resident[i] = L.slot_of[static_cast<std::size_t>(e)] != -1 ? 1 : 0;   // -2: its copy is on the way
      heat[i] = L.score[static_cast<std::size_t>(e)];
    }
  }
  const auto ranked = strata::core::rank_learned_profile(kLayers, kExperts, resident, heat, profile_prior_);
  if (!strata::core::write_expert_profile(profile_save_, kLayers, kExperts, ranked, error)) return false;
  profile_saved_ = now;
  return true;
}

// A swap whose copy has landed: the expert takes its slot (its table entry now points there).
void Gemma4Engine::land_swaps() {
  for (auto it = swaps_.begin(); it != swaps_.end();) {
    if (cudaEventQuery(it->landed) != cudaSuccess) {  // still copying (cudaErrorNotReady): a later call
      ++it;
      continue;
    }
    DevLayer& L = layers_[static_cast<std::size_t>(it->layer)];
    L.expert_in[static_cast<std::size_t>(it->slot - L.slot0)] = it->expert;
    L.slot_of[static_cast<std::size_t>(it->expert)] = it->slot;
    set_ptr(L, it->expert, reinterpret_cast<unsigned long long>(cache_.device_slot(it->slot)));
    set_res(L, it->expert, it->slot);
    free_events_.push_back(it->landed);
    it = swaps_.erase(it);
  }
}

// The adaptive tier (paper 3.4, findings 4 and 10): the routing counts, decayed, move the hottest RAM experts into the
// coldest slots.  Nothing here waits: the counts are the ones read back by the previous call's copy (once it has
// completed), an eviction takes effect at once in stream order (the expert is then read from the arena), and the
// incoming expert's copy runs on the copy engine after the compute stream has stopped reading the slot; it is
// admitted when that copy has landed (land_swaps, at a later call).
void Gemma4Engine::rebalance(int max_swaps) {
  if (broken_ || layers_.empty() || layers_[0].n_slots == 0 || layers_[0].n_slots >= kExperts) return;
  land_swaps();
  if (counts_pending_ && cudaEventQuery(counts_ev_) != cudaSuccess) return;  // the last read-back is not in yet
  const bool have_counts = counts_pending_;
  if (have_counts) {
    for (std::size_t l = 0; l < layers_.size(); ++l) {
      auto& score = layers_[l].score;
      for (int e = 0; e < kExperts; ++e) {
        score[static_cast<std::size_t>(e)] = score[static_cast<std::size_t>(e)] * 0.85 + counts_h_[l * kExperts + static_cast<std::size_t>(e)];
      }
    }
  }
  // the next read-back, used by the next call
  if (cudaMemcpyAsync(counts_h_, counts_all_, sizeof(int) * kLayers * kExperts, cudaMemcpyDeviceToHost, st_) != cudaSuccess ||
      cudaMemsetAsync(counts_all_, 0, sizeof(int) * kLayers * kExperts, st_) != cudaSuccess ||
      cudaEventRecord(counts_ev_, st_) != cudaSuccess) {
    broken_ = true;
    return;
  }
  counts_pending_ = true;
  if (!have_counts) return;
  struct Pick {
    int layer, slot, expert;
  };
  std::vector<Pick> picks;
  for (std::size_t round = 0; static_cast<int>(picks.size()) < max_swaps; ++round) {
    bool any = false;
    for (std::size_t l = 0; l < layers_.size() && static_cast<int>(picks.size()) < max_swaps; ++l) {
      DevLayer& L = layers_[l];
      int hot = -1, cold_slot = -1;
      for (int e = 0; e < kExperts; ++e) {   // in RAM only (not resident, not landing)
        if (L.slot_of[static_cast<std::size_t>(e)] == -1 && (hot < 0 || L.score[static_cast<std::size_t>(e)] > L.score[static_cast<std::size_t>(hot)])) hot = e;
      }
      for (int s = 0; s < L.n_slots; ++s) {   // holding an expert (not a swap still landing)
        const int e = L.expert_in[static_cast<std::size_t>(s)];
        if (e < 0) continue;
        if (cold_slot < 0 || L.score[static_cast<std::size_t>(e)] < L.score[static_cast<std::size_t>(L.expert_in[static_cast<std::size_t>(cold_slot)])]) cold_slot = s;
      }
      if (hot < 0 || cold_slot < 0) continue;
      const int cold = L.expert_in[static_cast<std::size_t>(cold_slot)];
      if (L.score[static_cast<std::size_t>(hot)] <= L.score[static_cast<std::size_t>(cold)] * 1.25 + 0.5) continue;
      // evict now: the cold expert is read from the arena from here on in stream order
      L.expert_in[static_cast<std::size_t>(cold_slot)] = -1;
      L.slot_of[static_cast<std::size_t>(cold)] = -1;
      set_ptr(L, cold, reinterpret_cast<unsigned long long>(L.host_alias + L.nl.bytes * static_cast<std::size_t>(cold)));
      set_res(L, cold, -1);
      L.slot_of[static_cast<std::size_t>(hot)] = -2;   // landing
      picks.push_back({static_cast<int>(l), L.slot0 + cold_slot, hot});
      any = true;
    }
    if (!any) break;
  }
  if (picks.empty()) return;
  // the copies start once the compute stream is past every kernel that may still read the evicted experts' slots
  // (re-recording evict_ev_ later is safe: a wait takes the event's state when it is queued)
  if (!cuda_ok(cudaEventRecord(evict_ev_, st_), "event") || !cuda_ok(cudaStreamWaitEvent(copy_, evict_ev_, 0), "event")) return;
  for (const Pick& p : picks) {
    const DevLayer& L = layers_[static_cast<std::size_t>(p.layer)];
    Swap s{p.layer, p.slot, p.expert, nullptr};
    if (!free_events_.empty()) {
      s.landed = free_events_.back();
      free_events_.pop_back();
    } else if (!cuda_ok(cudaEventCreateWithFlags(&s.landed, cudaEventDisableTiming), "event")) {
      return;
    }
    if (!cuda_ok(cudaMemcpyAsync(cache_.device_slot(p.slot), L.host_experts + L.nl.bytes * static_cast<std::size_t>(p.expert),
                                 L.nl.bytes, cudaMemcpyHostToDevice, copy_),
                 "expert swap") ||
        !cuda_ok(cudaEventRecord(s.landed, copy_), "event")) {
      return;
    }
    swaps_.push_back(s);
  }
}

}  // namespace

std::unique_ptr<Engine> Engine::create(const EngineConfig& config, const ModelDesc& model, std::string& error) {
  auto e = std::make_unique<Gemma4Engine>();
  if (!e->load(config, model, error)) {
    if (error.empty()) error = "the engine could not load the model";
    return nullptr;
  }
  return e;
}

}  // namespace g4
