// engines/unified/src/u_ple.hpp
// -----------------------------------------------------------------------------
// PLE (per-layer embedding) n-gram addressing — the PURE host part of qwen4exp's
// per-layer embedding lookup: the n-gram context window (with the EOS reset), the
// n-gram hash, and the head -> table-row mapping. The qwen4exp PLE path is
// box-proven end to end (token-exact parity with the upstream oracle, stage 6 of
// the qwen4exp lane, docs/AGENT_HANDOFF.md); the formulas lived inside the
// CUDA-only qwen4exp_graph.cu where no host build could reach or anchor them.
// This header is HOST-SAFE (and CUDA-inert: plain host inline functions called
// from the graph's host phase), so u_host_tests can anchor the semantics the box
// already certified, and the GPU path keeps calling the same code (no drift).
//
// Faithfulness: 1:1 with Qwen4ExpGraph::ple_prepare_into (qwen4exp_graph.cu, the
// stage-6 parity state) and with the upstream reference it was ported from
// (`mixed_n = XOR_j ctx[j]*mult[j]`, `row = mixed % vocab[h] + off[h]`, EOS
// resets the window) — the same formulas the handoff records for the oracle.
// -----------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace u {
namespace ple {

// The addressing parameters of one PLE layer, as the GGUF spells them
// (qwen4exp.ple.* keys). Heads are ordered by n-gram order: heads of order nn
// (2..ngram) occupy indices (nn-2)*per_gram .. (nn-1)*per_gram - 1, so
// n_heads = (ngram - 1) * per_gram (2 orders x 8 = 16 for the shipped model).
struct Spec {
  int ngram = 0;      // n-gram size (3 -> orders 2 and 3)
  int per_gram = 0;   // heads per order
  int eos = 0;        // the id that resets the window
  std::vector<std::uint64_t> mult;   // [ngram]     qwen4exp.ple.layer_multipliers
  std::vector<std::uint32_t> vocab;  // [n_heads]   qwen4exp.ple.head_vocab_sizes
  std::vector<std::uint32_t> off;    // [n_heads]   qwen4exp.ple.head_offsets
  [[nodiscard]] int n_heads() const { return (ngram - 1) * per_gram; }
};

// Fill ctx [ngram]: ctx[0] = the token being embedded, ctx[j] (j >= 1) = the token
// at p - j, taken from `prev` (prev[j-1] = the token at p - j; prev_n how many are
// actually in the history). The reference feeds an explicit EOS pad: once a slot
// is EOS or missing, every LATER slot of the window also takes the EOS id (the
// window is cut, not left stale).
inline void ngram_ctx(const Spec& s, std::int32_t token, const std::int32_t* prev, std::size_t prev_n,
                      std::int64_t* ctx) {
  ctx[0] = token;
  bool cut = false;
  for (int j = 1; j < s.ngram; ++j) {
    const std::int64_t tp = (cut || (std::size_t) (j - 1) >= prev_n)
                                ? -1
                                : (std::int64_t) prev[(std::size_t) j - 1];
    cut = cut || tp < 0 || tp == s.eos;
    ctx[j] = cut ? s.eos : tp;
  }
}

// The order-nn n-gram hash (nn in 2..ngram): XOR over the mixed products of the
// first nn context slots (per the upstream reference: mult[0] applies to ctx[0]).
inline std::uint64_t mixed_n(const Spec& s, const std::int64_t* ctx, int nn) {
  std::uint64_t mixed = (std::uint64_t) ctx[0] * s.mult[0];
  for (int j = 1; j < nn; ++j) mixed ^= (std::uint64_t) ctx[(std::size_t) j] * s.mult[(std::size_t) j];
  return mixed;
}

// The table row of head h (0..n_heads-1) for a given hash: the head's own vocab
// region inside the shared 320M-row table — mixed % vocab[h] + off[h].
inline std::int64_t head_row(const Spec& s, std::uint64_t mixed, int h) {
  return (std::int64_t) (mixed % s.vocab[(std::size_t) h]) + (std::int64_t) s.off[(std::size_t) h];
}

}  // namespace ple
}  // namespace u
