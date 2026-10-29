// The MTP drafter (spec §12): the gemma4-assistant model (llama.cpp src/models/gemma4-assistant.cpp) beside the target,
// on the target's own pieces - its token embedding goes in, its final hidden state h is the drafter's other input, and
// each drafter layer attends Q-only over a target layer's KV cache (llama.cpp's shared memory, common/speculative.cpp
// draft-mtp): nothing of the drafter's is written to any cache, so a rejected draft costs nothing to undo.
//
// One draft step at position pos0 (every step of a chain uses the same pos0, llama.cpp speculative.cpp:1717-1721):
//   xh = [target_embd(id) * sqrt(2816) | h]; x = pre_projection(xh)
//   per layer: q = rope(rms_head(Wq rms(x)) * q_norm, pos0); a = x + rms(Wo attn(q, target K/V < pos0)) * post_attn_norm;
//              x = (a + rms(down(gelu(gate(rms(a))) * up(rms(a)))) * post_ffw_norm) * layer_output_scale
//   n = rms(x) * output_norm; logits = token_embd_d n (no softcap); h_next = post_projection n
// The draft is the logits' top-1 (llama.cpp's top-k 10 sampler keeps cur_p[0]), its probability the softmax of the top 10.
// A chain of n steps is one captured CUDA graph (each step's token is the previous step's top-1 on the device), so a
// round of drafts costs one launch and one sync; with a probability floor (--spec-min-p) a graph per step, the chain
// stopping at the first draft below the floor.
#pragma once

#include "g4_kernels.cuh"
#include "g4_model.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace strata::core {
class CapturedGraph;
}

namespace g4 {

// What the drafter reads of the target engine.
struct DraftTarget final {
  g4k::Mat embd{};                 // the target's token_embd (Q8 planes): the drafter's input embedding
  const __half* kc_swa{};          // target layer n-2 (sliding): its K / V ring
  const __half* vc_swa{};
  int cap_swa{0};
  int ring{0};
  const __half* kc_full{};         // target layer n-1 (full)
  const __half* vc_full{};
  int cap_full{0};
  int max_splits{0};               // the decode attention's split bound for the target's context
  float* attn_scratch{};           // the target's decode-attention scratch (kHeads x max_splits x (512 + 2) floats): the
                                   // drafter's steps run on the target's stream between its steps, never beside them
};

// Puts the drafter's tensors into device memory the engine owns (and frees with its own).
class DraftUploader {
 public:
  virtual ~DraftUploader() = default;
  virtual bool matrix(const TensorSpan& t, g4k::Mat& out) = 0;
  virtual bool vector(const TensorSpan& t, float*& out) = 0;
  virtual bool bytes(void*& out, std::size_t n) = 0;  // device memory (zeroed is not required)
};

class Drafter final {
 public:
  Drafter();
  ~Drafter();
  Drafter(const Drafter&) = delete;
  Drafter& operator=(const Drafter&) = delete;

  // The weights (all in VRAM) and the step buffers for chains of up to `max_steps` drafts.
  bool load(const AssistantDesc& d, const DraftTarget& target, int max_steps, DraftUploader& up, std::string& error);
  // The device row the next chain starts from: the target's final normed hidden state of the row that gave id_last.
  [[nodiscard]] float* h_in() const { return h_in_; }
  // Up to `n` chained drafts after `id_last` at position `pos0` (pos0 >= 1: the target holds 0 .. pos0-1): ids[i] and
  // the top-1 probability probs[i]. Returns how many to offer: n, or fewer at the first draft below p_min (not
  // offered; with p_min > 0 the steps after it are not run) or at a step whose logits were not finite; -1 on a CUDA
  // failure. Runs on `st` and syncs it.
  int run(std::int32_t id_last, int pos0, int n, float p_min, std::int32_t* ids, float* probs, bool graph, cudaStream_t st,
          std::string& error);
  [[nodiscard]] double vram_mib() const { return vram_mib_; }
  [[nodiscard]] int layers() const { return static_cast<int>(layers_.size()); }

 private:
  struct Layer {
    bool sliding{true};
    int hd{0}, n_kv{0}, n_ff{0};
    g4k::Mat q{}, o{}, gate{}, up{}, down{};
    float *attn_norm{}, *q_norm{}, *post_attn_norm{}, *ffn_norm{}, *post_ffw_norm{}, *out_scale{};
  };
  void chain(int j0, int j1, int split_swa, int split_full, cudaStream_t st);  // steps [j0, j1)'s launches
  bool launch(int j0, int j1, int split_swa, int split_full, bool graph, cudaStream_t st, std::string& error);
  void step(int j, int split_swa, int split_full, cudaStream_t st);

  AssistantDesc desc_{};
  DraftTarget t_{};
  std::vector<Layer> layers_;
  g4k::Mat head_{}, pre_{}, post_{};
  float *output_norm_{}, *rope_freqs_{};
  int max_steps_{0};
  // step buffers
  float *xh_{}, *x_{}, *hn_{}, *q_{}, *qo_{}, *att_{}, *o_{}, *a_{}, *g_{}, *u_{}, *m_{}, *f_{}, *n_{}, *logits_{}, *scratch_{};
  float* h_in_{};
  float* top_v_{};   // [max_steps][kDraftTopK]
  int* top_i_{};     // [max_steps][kDraftTopK]
  int* flag_{};      // [max_steps]: a step's logits held a NaN or an infinity
  int* words_{};     // device: token, pos0, pos0 - 1
  unsigned char* topk_scratch_{};
  int* pin_{};       // pinned: words (3), then the outputs: top_v, top_i, flags
  std::map<std::vector<int>, std::unique_ptr<strata::core::CapturedGraph>> graphs_;
  bool graph_failed_{false};
  double vram_mib_{0.0};
};

}  // namespace g4
