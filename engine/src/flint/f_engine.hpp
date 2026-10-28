// multiengine_flint: the inference engine (design 2026-10-27-flint-engine-design.md, sections 2-3), Gemma 4's engine
// interface (g4_engine.hpp) for flint's shape. One session: the held tokens' KV state, read by `process` (prompt chunks
// or one decode step) and continued or truncated by the serve loop's reuse rule.
#pragma once

#include "f_model.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fl {

inline constexpr int kSampleCandidatesMax = 64;

// Decode rebalancing, as Gemma 4's (measured there; flint's own figures come with the Q8 file): every
// kDecodeRebalanceEvery tokens, at most kDecodeRebalanceSwaps expert copies into VRAM slots.
inline constexpr int kDecodeRebalanceEvery = 8;
inline constexpr int kDecodeRebalanceSwaps = 32;

struct EngineConfig final {
  std::vector<std::string> shards;   // ModelDesc::shards
  int max_context{32768};
  int chunk{512};
  double expert_vram_gib{-1.0};  // -1: the free VRAM after everything else, less a reserve
  int expert_slots{0};           // 0: from the budget
  // The decode token's CPU lane (paper 3.3) when some experts live only in RAM: the misses not staged by the copy
  // engine run on the CPU pool.  Off: the GPU reads them over PCIe in place (the A/B arm).
  bool cpu_lane{true};
  double pcie_frac{-1.0};        // the copy engine's share of a layer's misses (-1: kDefaultPcieFrac)
  int cpu_threads{0};            // the CPU pool's workers (0: one per physical core but the host's)
  bool graph{true};              // a decode step as one captured CUDA graph (false: plain launches, --no-graph)
  std::string expert_profile;       // Strata's profile.bin to fill the VRAM tier from (empty: experts 0..S-1)
  std::string expert_profile_save;  // where save_profile writes what the tier learned (empty: never)
  bool prompt_borrow{true};      // a streamed prompt's buffers lent by the expert cache's top slots (paper 3.6)
  int layers{kLayers};           // test-only (--layers): run the first N layers, then the head
};

inline constexpr double kProfileSaveMinutes = 10.0;
inline constexpr double kDefaultPcieFrac = 0.0;   // Gemma 4's measured default; flint's is measured with the Q8 file

struct LoadReport final {
  int slots_per_layer{0};
  double resident_pct{0.0};
  double vram_used_mib{0.0};
  double vram_total_mib{0.0};
  double pinned_gib{0.0};
  double load_seconds{0.0};
  int ring_rows{0};
  int lane_workers{0};
  bool profile_filled{false};
  int lent_slots{0};
};

class Engine {
 public:
  virtual ~Engine() = default;
  // Loads the model (checked by check_model) onto the current CUDA device. nullptr and a fixed `error` on failure.
  [[nodiscard]] static std::unique_ptr<Engine> create(const EngineConfig& config, const ModelDesc& model, std::string& error);

  [[nodiscard]] virtual int context() const = 0;
  [[nodiscard]] virtual int ring_spare() const = 0;  // sliding-ring rows beyond the window (reuse rule)
  [[nodiscard]] virtual int position() const = 0;    // tokens held
  virtual void truncate(int p) = 0;
  // Reads `n` tokens at position(): one decode step for n == 1, else prompt chunks of at most `chunk` tokens;
  // `progress(done)` after each chunk may return false to stop between chunks. Returns the tokens read; -1 on a CUDA
  // failure (the engine is then unusable).
  virtual int process(const std::int32_t* ids, int n, const std::function<bool(int)>& progress) = 0;
  virtual bool logits(std::vector<float>& out) = 0;   // the last read position's logits
  virtual int greedy() = 0;  // argmax of those logits; -1 on failure, -2 when they are not finite
  // The k highest of those logits (k <= kSampleCandidatesMax), highest first, ties by the lower id. Returns k; -1 on
  // failure; -2 when any logit is not finite.
  virtual int top_candidates(int k, float* vals, int* ids) = 0;
  virtual void rebalance(int max_swaps) = 0;
  // Test hook: every read position's layer outputs land in sink[(pos*kLayers + layer)*kEmbd ...].
  virtual void set_layer_sink(std::vector<float>* sink) = 0;
  virtual bool save_profile(bool force, std::string& error) = 0;
  [[nodiscard]] virtual LoadReport report() const = 0;
};

}  // namespace fl
