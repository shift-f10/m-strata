// strata-unified-engine: the CPU expert lane -- a worker pool that computes a layer's missed routed experts from RAM
// while the GPU computes the resident ones (upstream's ExpertPool, src/kernels/cpu/pool.cpp, and its per-layer
// serve in src/core/verify.cpp Verifier::run; MIT, Niko1221 and the Strata contributors).
//
// What is upstream's here: a layer's CPU experts as multi-token jobs whose rows are read once for every token routed
// to them (run_split_multi), every expert split by rows across all threads in three phases -- gate/up rows, the
// intermediate's quantization, down rows -- the host thread draining with the workers (host_works), workers that
// spin for 20 ms between batches and then sleep (kSpinBeforeSleep, issue #4), and a claim protocol that tags every
// task with its batch so a late worker never runs a stale one (issue #29). The row dots are ggml's (u_cpu_iq).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace u::cpu {

// One MoE layer's routed experts in host memory (the gate/up/down matrices of expert e at base + e*stride).
struct LaneLayer final {
  int gate_type = 0, up_type = 0, down_type = 0;  // GGML type ids (u::cpu::iq_type_supported)
  const std::uint8_t* gate = nullptr;
  const std::uint8_t* up = nullptr;
  const std::uint8_t* down = nullptr;
  std::int64_t gate_stride = 0, up_stride = 0, down_stride = 0;
  int n_embd = 0, n_ff = 0;
  [[nodiscard]] bool usable() const;  // every type has a CPU dot and the shapes fit its blocks
};

class LanePool final {
 public:
  // n_workers threads beside the caller (<= 0: one per physical core but the caller's). pin: one worker per physical
  // core (logical processors 2, 4, ... on Windows' SMT numbering), leaving processor 0's core to the caller.
  explicit LanePool(int n_workers = 0, bool pin = true);
  ~LanePool();
  LanePool(const LanePool&) = delete;
  LanePool& operator=(const LanePool&) = delete;
  [[nodiscard]] int threads() const { return n_ + 1; }

  // The layer's entries j < n_tok*k (token j / k, expert ids[j]) with eff[j] == cpu_code: ymiss[j * n_embd ..] = the
  // expert's SwiGLU FFN output for its token's x row (unweighted -- the GPU's combine applies the router weight).
  // Other entries' rows are not touched. Blocks until done. False (nothing done) unless 1 <= n_tok <= 8.
  bool serve(const LaneLayer& L, int n_tok, int k, const float* x, const int* ids, const int* eff, int cpu_code,
             float* ymiss);

  // Accumulated: experts and tokens served, the three phases' wall ms, bytes of expert rows read.
  std::int64_t experts = 0, rows_tokens = 0, bytes = 0;
  double ms_gu = 0, ms_q = 0, ms_down = 0;

 private:
  void worker(int id);
  void run_phase(int n_tasks, const std::function<void(int)>& fn);  // fn(task) on every thread, until done
  bool claim(std::uint32_t epoch, int& task);

  int n_ = 0;
  std::vector<std::thread> threads_;
  const std::function<void(int)>* fn_ = nullptr;
  std::atomic<int> n_tasks_{0};  // read by a worker before its claim's CAS, so atomic
  // the claim counter, the completion counter and the batch epoch each get a cache line (pad bytes, not alignas,
  // which MSVC's C4324 rejects): the workers hammer head_ and done_ while they idle-spin on epoch_
  char pad0_[64] = {};
  std::atomic<std::uint64_t> head_{0};  // epoch << 32 | next task
  char pad1_[56] = {};
  std::atomic<std::uint32_t> done_{0};
  char pad2_[60] = {};
  std::atomic<std::uint32_t> epoch_{0};
  char pad3_[60] = {};
  std::atomic<bool> stop_{false};
  std::atomic<std::uint32_t> sleepers_{0};
  std::mutex sleep_mu_;
  std::condition_variable sleep_cv_;

  // serve()'s working set (the caller's thread builds it; the phases read it)
  struct Job {
    std::int32_t e = 0;
    int nt = 0;
    int tok[8] = {};
    int entry[8] = {};
  };
  std::vector<Job> jobs_;
  std::vector<std::uint8_t> xq_;   // per token: Q8_1 blocks of its x row (the GPU's activation quant)
  std::vector<float> gu_;          // per (job, token): gate | up outputs [2 * n_ff]
  std::vector<std::uint8_t> hq_;   // per (job, token): Q8_1 blocks of h
};

// The scalar reference of one expert for one token (tests): out = down(silu(gate x) * up x), the activations
// quantized as the lane quantizes them.
void lane_expert_ref(const LaneLayer& L, std::int32_t e, const float* x, float* out);

}  // namespace u::cpu
