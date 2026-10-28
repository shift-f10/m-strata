// strata-gemma4-engine: the inference engine (plan Task 6). One session: the held tokens'
// KV state, read by `process` (prompt chunks or one decode step) and continued or truncated
// by the serve loop's reuse rule.
#pragma once

#include "g4_model.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace g4 {

inline constexpr int kSampleCandidatesMax = 64;

// Decode rebalancing: every kDecodeRebalanceEvery tokens, at most kDecodeRebalanceSwaps expert
// copies into VRAM slots (each about 6.3 MB). Measured on the owner's box (validation file,
// 2026-10-23): 8/32 lifted a 225-token sampled story from 29 to 54 tok/s from a cold start
// and to 77-84 tok/s once the slots had settled, against 32/8 before.
inline constexpr int kDecodeRebalanceEvery = 8;
inline constexpr int kDecodeRebalanceSwaps = 32;

struct EngineConfig final {
  std::string model;
  int max_context{32768};
  int chunk{512};
  double expert_vram_gib{-1.0};  // -1: the free VRAM after everything else, less a reserve
  int expert_slots{0};           // 0: from the budget
  // The decode token's CPU lane (paper 3.3; g4_lane.hpp) when some experts live only in RAM: the misses not staged by
  // the copy engine run on the CPU pool.  Off: the GPU reads them over PCIe in place (the A/B arm).
  bool cpu_lane{true};
  double pcie_frac{-1.0};        // the copy engine's share of a layer's misses (-1: kDefaultPcieFrac)
  int cpu_threads{0};            // the CPU pool's workers (0: one per physical core but the host's)
  bool graph{true};              // a decode step as one captured CUDA graph (false: plain launches, --no-graph)
  std::string expert_profile;       // Strata's profile.bin to fill the VRAM tier from (empty: experts 0..S-1)
  std::string expert_profile_save;  // where save_profile writes what the tier learned (empty: never)
  // P5: a streamed prompt's buffers (its chunk rows, the MMQ buffers, the miss ring) live in the expert cache's top
  // slots, lent for the prompt and refilled after it (paper 3.6, Strata's default); false: VRAM of their own
  bool prompt_borrow{true};
};

// save_profile's interval between requests (Strata's --expert-profile-save-every default).
inline constexpr double kProfileSaveMinutes = 10.0;

// The copy engine's default share: none.  Measured on the owner's box (RTX 4080 SUPER on PCIe 4.0 x16, 2026-10-26,
// 96 decode steps after a 64-token prompt): misses all on the CPU pool 51.5 tok/s; half staged 45.4; three quarters
// 41.7; all staged 36.5; no lane (every miss read over PCIe in place) 39.8.  The pool reads RAM faster than the link
// copies, and the two share the RAM's bandwidth.  A faster link may want --pcie-frac.
inline constexpr double kDefaultPcieFrac = 0.0;

struct LoadReport final {
  int slots_per_layer{0};
  double resident_pct{0.0};
  double vram_used_mib{0.0};
  double vram_total_mib{0.0};
  double pinned_gib{0.0};
  double load_seconds{0.0};
  int ring_rows{0};
  int lane_workers{0};  // the CPU lane's pool threads (0: no lane - every expert in VRAM, or --cpu-lane off)
  bool profile_filled{false};  // the slots were filled from --expert-profile
  int lent_slots{0};           // expert slots a streamed prompt borrows for its buffers (0: none, or VRAM of their own)
};

class Engine {
 public:
  virtual ~Engine() = default;
  // Loads the model (checked by check_model) onto the current CUDA device. nullptr and a
  // fixed `error` on failure.
  [[nodiscard]] static std::unique_ptr<Engine> create(const EngineConfig& config, const ModelDesc& model, std::string& error);

  [[nodiscard]] virtual int context() const = 0;
  [[nodiscard]] virtual int ring_spare() const = 0;  // sliding-ring rows beyond the window (reuse rule)
  [[nodiscard]] virtual int position() const = 0;    // tokens held
  // Keeps the first `p` held positions (p <= position()); the serve loop's reuse rule
  // decides when that is exact.
  virtual void truncate(int p) = 0;
  // Reads `n` tokens at position(): one decode step for n == 1, else prompt chunks of at most
  // `chunk` tokens; `progress(done)` after each chunk may return false to stop between chunks.
  // Returns the tokens read; -1 on a CUDA failure (the engine is then unusable).
  virtual int process(const std::int32_t* ids, int n, const std::function<bool(int)>& progress) = 0;
  // The last read position's logits (soft-capped).
  virtual bool logits(std::vector<float>& out) = 0;
  virtual int greedy() = 0;  // argmax of those logits; -1 on failure, -2 when they are not finite
  // The k highest of those logits (k <= kSampleCandidatesMax), highest first, ties by the lower id,
  // ranked on the GPU (1 MB of logits never crosses PCIe). Returns k; -1 on failure; -2 when any
  // logit is not finite.
  virtual int top_candidates(int k, float* vals, int* ids) = 0;
  // Moves VRAM expert slots toward the most-routed experts (at most `max_swaps` copies).
  virtual void rebalance(int max_swaps) = 0;
  // Test hook: every read position's layer outputs land in sink[(pos*30 + layer)*2816 ...].
  virtual void set_layer_sink(std::vector<float>* sink) = 0;
  // Writes the expert tier's learned profile to EngineConfig::expert_profile_save (Strata's format: the resident
  // experts first, then by routing) - when `force`, or kProfileSaveMinutes after the last write. True when nothing
  // was due or the file was written; false (and the reason) when it could not be.
  virtual bool save_profile(bool force, std::string& error) = 0;
  [[nodiscard]] virtual LoadReport report() const = 0;
};

}  // namespace g4
