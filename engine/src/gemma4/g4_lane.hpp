// The paper's CPU side of a decode token (strata-paper 3.3 and 3.5, findings 3, 5 and 6; design
// 2026-10-26-gemma4-paper-tiering-design.md, phase 3), on the engine's own pieces: the verify window's dispatcher
// (strata::core::expert_pool_dispatch_multi), the CPU expert pool on ggml-cpu's dot products and its GPU plan sink.
//
// Per layer the GPU publishes the routing and the experts' input to mapped memory and rings a doorbell
// (doorbell_publish_res).  This thread, spinning on the ring, splits the layer's experts: the VRAM-resident ones go to
// the GPU (the plan's first groups, flag A), a share of the misses is copied into VRAM staging by the copy engine
// (the plan's second groups, flag B when the copies land), and the rest run on the CPU pool, whose rows come back
// through mapped memory (flag C).  The GPU waits on each flag in turn, so nothing crosses the host's driver calls.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace g4 {

// The plan the lane writes for the GPU (int32 words of mapped memory, copied to the device after flag A): a decode
// step's 8 entries.  ptr / ptr2 are device addresses (8-byte aligned words).
struct LanePlan {
  static constexpr int kCap = 8;
  static constexpr int kCounts = 0;   // [0] VRAM groups, [1] entries, [2] staged groups
  static constexpr int kStart = 4;    // kCap + 1
  static constexpr int kDst = 13;     // kCap: the entry's row (its rank in the routing)
  static constexpr int kTok = 21;     // kCap: the entry's token (0)
  static constexpr int kPtr = 30;     // kCap x u64: the VRAM groups' blobs
  static constexpr int kPtr2 = 46;    // kCap x u64: the staged groups' blobs (staging slots)
  static constexpr int kStart2 = 62;  // kCap + 1
  static constexpr int kWords = 72;
};

class CpuLane {
 public:
  struct Layer {
    const std::uint8_t* host{};  // the layer's 128 blobs in the pinned arena
    std::size_t bytes{0};        // one blob
    int gu_type{0}, d_type{0};   // ggml types of gate/up and down
  };
  // The mapped words, as the GPU addresses them.
  struct Device {
    std::uint32_t* seq{};      // the doorbell (the publish kernel increments word 0)
    const std::uint32_t* flag_a{};
    const std::uint32_t* flag_b{};
    const std::uint32_t* flag_c{};
    float* x{};                // the experts' input row (kEmbd floats)
    std::int32_t* ids{};       // the 8 routed experts
    const float* rows{};       // the CPU's rows, [8][kEmbd]
    const std::int32_t* plan{};  // LanePlan::kWords
  };
  struct Stats {
    long long tokens{0}, hits{0}, staged{0}, cpu_entries{0}, cpu_experts{0};
    double pool_ms{0};
  };

  CpuLane();
  ~CpuLane();
  CpuLane(const CpuLane&) = delete;
  CpuLane& operator=(const CpuLane&) = delete;

  // `host_res` is the engine's [layers][128] residency (a cache slot, or -1), written by the engine between tokens
  // only; `cache_base` and `slot_off` give a slot's device address.  `staging` (device, `staging_bytes`) takes the
  // copy engine's share; `pcie_num` / 256 of each layer's misses go there.  `workers` 0: the pool's default.
  bool start(const std::vector<Layer>& layers, int n_embd, int n_ff, const std::int32_t* host_res, const std::uint8_t* cache_base,
             std::vector<std::uint64_t> slot_off, std::uint8_t* staging, std::size_t staging_bytes, int pcie_num, int workers,
             std::string& error);
  // Ends the thread (a token in flight is released: every flag raised).  The mapped memory stays until the lane is
  // destroyed, after the caller's stream has synced.
  void stop();
  // A token the caller could not enqueue whole: the lane stops waiting for its rings and releases the GPU.
  void abort_token();
  [[nodiscard]] bool running() const { return thread_.joinable(); }
  [[nodiscard]] const Device& device() const { return dev_; }
  [[nodiscard]] int workers() const;

  // One decode token, between stream syncs (the GPU is idle): the doorbell and the flags back to 0, then the lane
  // serves the token's layers as they ring.
  void begin_token();
  // After the token's stream sync: false (and the reason) when the lane could not serve a layer.
  bool end_token(std::string& error);
  [[nodiscard]] Stats stats() const;

 private:
  struct Impl;
  void run();
  bool serve(int layer);
  void release_gpu();
  static void publish(void* ctx);
  static void fetch(void* ctx, const std::uint8_t* const* src, int n, std::size_t bytes);

  std::unique_ptr<Impl> impl_;
  Device dev_{};
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool armed_{false}, quit_{false};
  std::atomic<long long> served_{0};
  long long begun_{0};
  std::atomic<bool> failed_{false};
  std::string fail_;
};

}  // namespace g4
