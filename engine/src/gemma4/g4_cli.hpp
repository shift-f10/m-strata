// strata-gemma4-engine: the command line (spec §4.8; plan R5, R6) and the serve protocol's
// request parsing and sampling (the q36 engine's grammar and sampler semantics).
#pragma once

#include "g4_engine.hpp"

#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace g4 {

struct EngineOptions final {
  std::string model;             // --native GGUF (required)
  bool serve{false};             // --serve
  std::vector<int> gpus;         // --gpu i,j (the first is used)
  int max_context{32768};        // --max-context 1-262144
  int chunk{512};                // --chunk 1-8192 (prompt tokens per forward pass)
  bool kv_f32{false};            // --kv f16|f32 (f32 refused at load in v1, plan R6)
  bool graph{true};              // --no-graph
  double expert_vram_gib{-1.0};  // --expert-vram GiB (-1: from the free VRAM)
  int expert_slots{0};           // --expert-slots per layer (0: from the budget)
  int hybrid_rows{128};          // --hybrid-rows (unused until the CPU pool, plan R2)
  int cpu_threads{0};            // --cpu-threads: the CPU expert pool's workers (0: one per physical core but the host's)
  double pcie_frac{-1.0};        // --pcie-frac 0-1: the share of a layer's RAM experts the copy engine stages (-1: default)
  bool cpu_lane{true};           // --cpu-lane on|off: off = the GPU reads every RAM expert over PCIe in place (A/B)
  std::string expert_profile;    // --expert-profile P: fill the VRAM tier from Strata's profile.bin (empty: experts 0..S-1)
  std::string expert_profile_save;  // --expert-profile-save P: write what the tier learned there (on exit, and between requests)
  bool prompt_borrow{true};      // --no-prompt-borrow: the prompt's buffers in VRAM of their own, not lent by the expert cache
  // Test-only (refused with --serve).
  std::string tokens;            // --tokens id,id,...: run them, then greedy --n tokens
  int n_gen{16};                 // --n
  std::string force;             // --force id,...: teacher-forced continuation
  std::string dump_full_logits;  // --dump-full-logits FILE: every step's logits, f32 [step][vocab]
  std::string dump_layers;       // --dump-layers FILE: every position's layer outputs, f32 [pos][layer][2816]
  int bench_pp{0};               // --bench-pp N: time an N-token prompt
  int bench_tg{0};               // --bench-tg N: time N decode steps
  bool check{false};             // --check: load the header, run the guard, print a summary
};

// Fixed messages; never echoes an argument.
[[nodiscard]] std::variant<EngineOptions, std::string> parse_cli(const std::vector<std::string>& args);

struct Sampling final {
  float temperature{0.0f}, top_p{1.0f}, min_p{0.0f}, repeat{1.0f}, freq{0.0f}, present{0.0f};
  int top_k{20}, last_n{0};
  unsigned long long seed{0};
  [[nodiscard]] bool needs_logits() const { return temperature > 0.0f || repeat != 1.0f || freq != 0.0f || present != 0.0f; }
};

struct GenRequest final {
  long long max_new{0};
  Sampling sampling;
  std::vector<std::int32_t> ids;
};

// `GEN <max_new> [key=value ...] <id,id,...>` with the keys the frontend renders
// (temperature top_p top_k min_p penalty_repeat penalty_freq penalty_present penalty_last_n
// seed). Every id must be < vocab. The error is a fixed ERR text (without the "ERR ").
[[nodiscard]] std::variant<GenRequest, std::string> parse_gen(std::string_view line, int vocab);

[[nodiscard]] std::vector<std::int32_t> parse_id_list(std::string_view text, bool& ok);

// The q36 engine's sampler (q36_engine.cu sample()): penalties over the last `last_n`
// history ids, then greedy at temperature 0, else top-k (64 when unset) / min-p / top-p.
[[nodiscard]] int sample(std::vector<float>& logits, const Sampling& s, const std::vector<std::int32_t>& history,
                         std::mt19937_64& rng);

// The q36 sampler's last step on candidates already ranked highest first (top-k done):
// temperature, min-p, top-p, then one draw. sample() calls it after its own ranking; the serve
// loop calls it on the GPU's top-k when no penalty is set (g4k::topk).
[[nodiscard]] int sample_sorted(const float* vals, const int* ids, int kk, const Sampling& s, std::mt19937_64& rng);

// The sampler's seed for a request: the GEN line's seed when it set one (> 0), else one
// derived from `clock_ticks` (the caller passes the steady clock), as upstream's generate.cpp
// does (:13784) - so an unseeded request is not the same draw every time. Never 0.
[[nodiscard]] unsigned long long request_seed(unsigned long long requested, unsigned long long clock_ticks);

// True when sample() would apply a penalty (they change logits before the ranking).
[[nodiscard]] bool penalties_on(const Sampling& s);

// The q36 sampler's candidate count: top_k when set (> 0), else 64, at most `vocab`.
[[nodiscard]] int candidate_count(const Sampling& s, int vocab);

// The INFO record printed before READY (strata_0143 lets INFO key=value records precede it;
// the frontend keeps them for the Monitor). Besides our own figures it carries the main
// engine's expert keys the Monitor's VRAM and RAM cards read: expert_slots (every layer's
// experts, 30 x 128) and expert_slots_primary (those held in VRAM slots).
[[nodiscard]] std::string engine_info_line(const LoadReport& report, int context);

// Prefix reuse (plan R4): the held tokens' longest common prefix with the prompt, kept
// only when the held tail past it fits the sliding ring's spare rows (`spare` = ring -
// window) and at least one prompt token is left to read. 0 means start over.
[[nodiscard]] std::size_t reuse_length(const std::vector<std::int32_t>& held, const std::vector<std::int32_t>& prompt,
                                       std::size_t spare);

}  // namespace g4
