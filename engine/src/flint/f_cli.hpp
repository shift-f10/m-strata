// multiengine_flint: the command line and the serve protocol's request parsing and sampling - Gemma 4's (g4_cli.hpp,
// the q36 engine's grammar and sampler semantics) for flint's shape.
#pragma once

#include "f_engine.hpp"

#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fl {

struct EngineOptions final {
  std::string model;             // --native GGUF, any shard of a split model (required)
  bool serve{false};             // --serve
  std::vector<int> gpus;         // --gpu i,j (the first is used)
  int max_context{32768};        // --max-context 1-262144
  int chunk{512};                // --chunk 1-8192 (prompt tokens per forward pass)
  bool graph{true};              // --no-graph
  double expert_vram_gib{-1.0};  // --expert-vram GiB (-1: from the free VRAM)
  int expert_slots{0};           // --expert-slots per layer (0: from the budget)
  int cpu_threads{0};            // --cpu-threads: the CPU expert pool's workers
  double pcie_frac{-1.0};        // --pcie-frac 0-1
  bool cpu_lane{true};           // --cpu-lane on|off
  std::string expert_profile;    // --expert-profile P
  std::string expert_profile_save;  // --expert-profile-save P
  bool prompt_borrow{true};      // --no-prompt-borrow
  // Test-only (refused with --serve).
  std::string tokens;            // --tokens id,id,...: run them, then greedy --n tokens
  int n_gen{16};                 // --n
  std::string force;             // --force id,...: teacher-forced continuation
  std::string dump_full_logits;  // --dump-full-logits FILE: every step's logits, f32 [step][vocab]
  std::string dump_layers;       // --dump-layers FILE: every position's layer outputs, f32 [pos][layer][2048]
  int layers{kLayers};           // --layers 1-49: the first N layers only (the head then reads layer N-1's output)
  int bench_pp{0};               // --bench-pp N
  int bench_tg{0};               // --bench-tg N
  bool check{false};             // --check: read the headers, run the guard, print a summary
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

// `GEN <max_new> [key=value ...] <id,id,...>` with the keys the frontend renders.
[[nodiscard]] std::variant<GenRequest, std::string> parse_gen(std::string_view line, int vocab);
[[nodiscard]] std::vector<std::int32_t> parse_id_list(std::string_view text, bool& ok);
[[nodiscard]] int sample(std::vector<float>& logits, const Sampling& s, const std::vector<std::int32_t>& history,
                         std::mt19937_64& rng);
[[nodiscard]] int sample_sorted(const float* vals, const int* ids, int kk, const Sampling& s, std::mt19937_64& rng);
[[nodiscard]] unsigned long long request_seed(unsigned long long requested, unsigned long long clock_ticks);
[[nodiscard]] bool penalties_on(const Sampling& s);
[[nodiscard]] int candidate_count(const Sampling& s, int vocab);

// The INFO record printed before READY, with the main engine's expert keys the Monitor reads: expert_slots (every MoE
// layer's experts, 47 x 256) and expert_slots_primary (those held in VRAM slots).
[[nodiscard]] std::string engine_info_line(const LoadReport& report, int context);

// Prefix reuse: the held tokens' longest common prefix with the prompt, kept only when the held tail past it fits the
// sliding ring's spare rows and at least one prompt token is left to read. 0 means start over.
[[nodiscard]] std::size_t reuse_length(const std::vector<std::int32_t>& held, const std::vector<std::int32_t>& prompt,
                                       std::size_t spare);

}  // namespace fl
