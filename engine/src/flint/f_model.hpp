// multiengine_flint: the model guard (design docs/superpowers/specs/2026-10-27-flint-engine-design.md, section 1). It
// accepts exactly flint's shape - read from the owner's file and from llama.cpp's flint graph - across the GGUF's
// shards, binds every tensor once and records the shard holding it; anything else is refused with a fixed message.
#pragma once

#include "g4_gguf.hpp"   // the bounded GGUF reader, shared with Gemma 4 (engine/src/gemma4, unchanged)

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace fl {

using g4::GgufFile;
using g4::TensorType;

inline constexpr int kLayers = 49;
inline constexpr int kDenseLayers = 2;                    // leading_dense_block_count: layers 0 and 1
inline constexpr int kMoeLayers = kLayers - kDenseLayers;  // 47: the expert tier's layers
inline constexpr int kEmbd = 2048;
inline constexpr int kHeads = 32;
inline constexpr int kKvHeads = 4;
inline constexpr int kHead = 128;
inline constexpr int kWindow = 513;   // a sliding layer's query sees itself and the 512 keys before it
inline constexpr int kFf = 6144;      // the dense layers' FFN
inline constexpr int kExperts = 256;
inline constexpr int kExpertsUsed = 6;
inline constexpr int kFfExp = 512;
inline constexpr int kFfShexp = 1024;  // the one shared expert
inline constexpr int kVocab = 150144;
inline constexpr int kMaxShards = 64;

// One tensor's place: `rows` outputs of `cols` inputs (ggml dims[1] x dims[0]); expert stacks add `experts`
// (dims[2]); vectors have rows == 1. `file_offset` is absolute within shard `shard`.
struct TensorSpan final {
  TensorType type{TensorType::f32};
  std::uint64_t cols{0};
  std::uint64_t rows{1};
  std::uint64_t experts{1};
  int shard{0};
  std::uint64_t file_offset{0};
  std::uint64_t bytes{0};
  bool present{false};
};

struct LayerDesc final {
  bool sliding{true};   // attention.sliding_window_pattern: the kWindow keys (else every key)
  bool rope{true};      // attention.rope_pattern: NeoX RoPE on q and k (else none)
  bool moe{true};       // false on the leading dense layers
  TensorSpan attn_norm, attn_q, attn_k, attn_v, attn_output, attn_q_norm, attn_k_norm, attn_sinks, ffn_norm;
  TensorSpan ffn_gate, ffn_up, ffn_down;               // dense layers
  TensorSpan gate_inp, gate_exps, up_exps, down_exps;  // MoE layers: the router, the routed experts' stacks
  TensorSpan gate_shexp, up_shexp, down_shexp;         // ... and the shared expert
};

struct ModelDesc final {
  std::array<LayerDesc, kLayers> layers{};
  TensorSpan token_embd, output_norm, output;
  float rms_eps{1e-6f};
  float rope_base{1e4f};
  float expert_weights_scale{1.0f};
  std::int64_t context_train{262144};
  std::int32_t bos_id{150000};
  std::int32_t eos_id{150001};
  std::vector<std::int32_t> eog_ids;   // tokenizer.ggml.eog_token_ids (with eos_id among them)
  std::vector<std::string> shards;     // read_model: the shard paths, in order (shard 0 holds the metadata)
};

struct ModelError final {
  std::string message;  // fixed text naming the rule (and a tensor name from the file's own table)
};

// The guard over the shards' parsed headers, in shard order: shard 0 carries the metadata (and split.count when there
// are several), every tensor is in exactly one shard.
[[nodiscard]] std::variant<ModelDesc, ModelError> check_model(const std::vector<GgufFile>& shards);

// The shard names of a split model named by any one of them (llama.cpp's gguf-split names: <stem>-00001-of-0000N.gguf),
// in order; a name without the pattern is its own one-shard list. Empty when the pattern is malformed or N is over
// kMaxShards.
[[nodiscard]] std::vector<std::string> split_names(const std::string& any);

// split_names, then every shard's header read (g4::read_gguf_header), then check_model; ModelDesc::shards is set.
[[nodiscard]] std::variant<ModelDesc, ModelError> read_model(const std::string& any_shard);

}  // namespace fl
