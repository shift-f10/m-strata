// strata-gemma4-engine: the model guard (spec §4.1). It accepts exactly the Gemma 4
// 26B-A4B text shape (spec §2.1, confirmed against the owner's file in §2.6) and binds every
// tensor of the table in §2.2 once; anything else is refused with a fixed message.
#pragma once

#include "g4_gguf.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace g4 {

inline constexpr int kLayers = 30;
inline constexpr int kEmbd = 2816;
inline constexpr int kHeads = 16;
inline constexpr int kHeadSwa = 256;
inline constexpr int kHeadFull = 512;
inline constexpr int kKvHeadsSwa = 8;
inline constexpr int kKvHeadsFull = 2;
inline constexpr int kWindow = 1024;
inline constexpr int kFf = 2112;
inline constexpr int kExperts = 128;
inline constexpr int kExpertsUsed = 8;
inline constexpr int kFfExp = 704;
inline constexpr int kVocab = 262144;
inline constexpr int kRopeFreqs = 256;  // rope_freqs: one factor per rotated pair of a 512-wide head

// One tensor's place in the file. Matrices: `rows` outputs of `cols` inputs each
// (ggml dims[1] x dims[0]); expert stacks add `experts` (dims[2]); vectors have rows == 1.
struct TensorSpan final {
  TensorType type{TensorType::f32};
  std::uint64_t cols{0};
  std::uint64_t rows{1};
  std::uint64_t experts{1};
  std::uint64_t file_offset{0};  // absolute
  std::uint64_t bytes{0};
  bool present{false};
};

struct LayerDesc final {
  bool sliding{true};
  int head_dim{kHeadSwa};
  int kv_heads{kKvHeadsSwa};
  TensorSpan attn_norm, attn_q, attn_k, attn_v, attn_output, attn_q_norm, attn_k_norm, post_attention_norm;
  TensorSpan ffn_norm, ffn_gate, ffn_up, ffn_down, post_ffw_norm_1;
  TensorSpan gate_inp, gate_inp_scale, pre_ffw_norm_2, gate_up_exps, down_exps, down_exps_scale, post_ffw_norm_2;
  TensorSpan post_ffw_norm, layer_output_scale;  // layer_output_scale optional (1 when absent)
};

struct ModelDesc final {
  std::array<LayerDesc, kLayers> layers{};
  TensorSpan token_embd, output_norm, rope_freqs;
  float rms_eps{1e-6f};
  float rope_base_full{1e6f};
  float rope_base_swa{1e4f};
  float softcap{30.0f};
  std::int64_t context_train{262144};
  std::int32_t bos_id{2};
  std::int32_t eos_id{1};       // tokenizer.ggml.eos_token_id
  std::int32_t turn_close_id{106};  // the "<turn|>" token, found by its literal
};

struct ModelError final {
  std::string message;  // fixed text naming the rule (and a tensor name from the file's own table)
};

[[nodiscard]] std::variant<ModelDesc, ModelError> check_model(const GgufFile& file);

}  // namespace g4
