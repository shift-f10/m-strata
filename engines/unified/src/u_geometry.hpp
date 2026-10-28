// strata-unified-engine: the per-family geometry guard. It reads `general.architecture`
// and that family's `<arch>.*` metadata keys into one typed Geometry struct. The guard is
// strict and fails closed: an unknown architecture, a missing key, a key of the wrong GGUF
// type, or a value outside that family's accepted range all refuse the file with a fixed
// message. No tensor data is touched (that is the GPU-box graph's job, later).
//
// The key names follow the GGUF `<arch>.*` convention used by the gemma4 reference
// (g4_model.cpp §2.6 header list). Raon-1-mini's values are pinned to its published shape
// (block_count 25, hidden 2560, heads 40, head_dim 128, intermediate 12032,
// moe_intermediate 1088, experts 128, used 6, shared 1, vocab 259840, context 262144,
// sigmoid router, NoPE).
#pragma once

#include "u_dispatch.hpp"
#include "u_gguf.hpp"

#include <cstdint>
#include <string>
#include <variant>

namespace u {

// How a family routes its MoE experts and whether it applies rotary position embeddings.
// These are fixed properties of the family (not read from the file); a RoPE family must
// carry a valid `<arch>.rope.freq_base`, a NoPE family (raon1) must not depend on one.
enum class RouterKind { softmax, sigmoid };
enum class RopeKind { rope, nope };

// The common geometry every supported family exposes. Family-specific graph details
// (KDA/MLA/GDN layer splits, draft blocks) are left to the GPU-box graph modules.
struct Geometry final {
  FamilyId family{FamilyId::raon1};
  std::string arch;                      // the `general.architecture` string
  std::int64_t block_count{0};           // <arch>.block_count
  std::int64_t embedding_length{0};      // <arch>.embedding_length (hidden)
  std::int64_t head_count{0};            // <arch>.attention.head_count
  std::int64_t head_dim{0};              // <arch>.attention.key_length
  std::int64_t feed_forward_length{0};   // <arch>.feed_forward_length (dense intermediate)
  std::int64_t expert_feed_forward_length{0};  // <arch>.expert_feed_forward_length (moe intermediate)
  std::int64_t expert_count{0};          // <arch>.expert_count
  std::int64_t expert_used_count{0};     // <arch>.expert_used_count
  std::int64_t expert_shared_count{0};   // <arch>.expert_shared_count
  std::int64_t context_length{0};        // <arch>.context_length
  std::int64_t vocab_size{0};            // <arch>.vocab_size
  RouterKind router{RouterKind::sigmoid};
  RopeKind rope{RopeKind::nope};
  double rope_freq_base{0.0};            // <arch>.rope.freq_base (0 for a NoPE family)
};

struct GeometryError final {
  std::string message;  // fixed text naming the rule and the key (never file bytes)
};

// Reads and guards the geometry for the file's `general.architecture`. Fails closed on an
// unknown architecture or any key that is missing, the wrong type, or out of range.
[[nodiscard]] std::variant<Geometry, GeometryError> read_geometry(const GgufFile& file);

}  // namespace u
