// The MTP drafter (g4_mtp.hpp; spec §12).
#include "g4_mtp.hpp"

#include "g4_spec.hpp"

#include "strata/core/graph.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace g4 {

Drafter::Drafter() = default;

Drafter::~Drafter() {
  graphs_.clear();
  if (pin_ != nullptr) cudaFreeHost(pin_);
}

bool Drafter::load(const AssistantDesc& d, const DraftTarget& target, int max_steps, DraftUploader& up, std::string& error) {
  desc_ = d;
  t_ = target;
  max_steps_ = std::clamp(max_steps, 1, kSpecMaxRows);
  double bytes = 0.0;
  auto mat = [&](const TensorSpan& s, g4k::Mat& m) {
    bytes += double(s.bytes);
    return up.matrix(s, m);
  };
  auto vec = [&](const TensorSpan& s, float*& v) {
    bytes += double(s.bytes);
    return up.vector(s, v);
  };
  if (!mat(d.token_embd, head_) || !mat(d.pre_projection, pre_) || !mat(d.post_projection, post_) || !vec(d.output_norm, output_norm_) ||
      !vec(d.rope_freqs, rope_freqs_)) {
    error = "cannot load the drafter's weights";
    return false;
  }
  int max_ff = 0;
  layers_.resize(d.layers.size());
  for (std::size_t i = 0; i < d.layers.size(); ++i) {
    const AssistantLayerDesc& s = d.layers[i];
    Layer& L = layers_[i];
    L.sliding = s.sliding;
    L.hd = s.head_dim;
    L.n_kv = s.kv_heads;
    L.n_ff = s.n_ff;
    max_ff = std::max(max_ff, s.n_ff);
    if (!mat(s.attn_q, L.q) || !mat(s.attn_output, L.o) || !mat(s.ffn_gate, L.gate) || !mat(s.ffn_up, L.up) || !mat(s.ffn_down, L.down) ||
        !vec(s.attn_norm, L.attn_norm) || !vec(s.attn_q_norm, L.q_norm) || !vec(s.post_attention_norm, L.post_attn_norm) ||
        !vec(s.ffn_norm, L.ffn_norm) || !vec(s.post_ffw_norm, L.post_ffw_norm) || !vec(s.layer_output_scale, L.out_scale)) {
      error = "cannot load the drafter's layer weights";
      return false;
    }
  }
  if (!layers_.empty() && rope_freqs_ == nullptr && std::any_of(layers_.begin(), layers_.end(), [](const Layer& L) { return !L.sliding; })) {
    error = "the drafter's full layers need its rope_freqs";
    return false;
  }
  const std::size_t E = static_cast<std::size_t>(d.n_embd), Q = std::size_t(kHeads) * kHeadFull, F = static_cast<std::size_t>(max_ff);
  const std::size_t S = static_cast<std::size_t>(max_steps_);
  std::size_t buffers = 0;
  auto floats = [&](float*& p, std::size_t n) {
    void* raw = nullptr;
    buffers += n * sizeof(float);
    if (!up.bytes(raw, std::max<std::size_t>(n, 1) * sizeof(float))) return false;
    p = static_cast<float*>(raw);
    return true;
  };
  auto ints = [&](int*& p, std::size_t n) {
    void* raw = nullptr;
    buffers += n * sizeof(int);
    if (!up.bytes(raw, std::max<std::size_t>(n, 1) * sizeof(int))) return false;
    p = static_cast<int*>(raw);
    return true;
  };
  scratch_ = t_.attn_scratch;
  if (scratch_ == nullptr) {
    error = "the drafter needs the target's attention scratch";
    return false;
  }
  void* tks = nullptr;
  const bool ok = floats(xh_, 2 * std::size_t(kEmbd)) && floats(x_, E) && floats(hn_, E) && floats(q_, Q) && floats(qo_, Q) &&
                  floats(att_, Q) && floats(o_, E) && floats(a_, E) && floats(g_, F) && floats(u_, F) && floats(m_, F) &&
                  floats(f_, E) && floats(n_, E) && floats(logits_, kVocab) && floats(h_in_, kEmbd) &&
                  floats(top_v_, S * kDraftTopK) && ints(top_i_, S * kDraftTopK) && ints(flag_, S) && ints(words_, 4) &&
                  up.bytes(tks, g4k::topk_scratch_bytes(kVocab));
  if (!ok) {
    error = "cannot allocate the drafter's buffers";
    return false;
  }
  topk_scratch_ = static_cast<unsigned char*>(tks);
  buffers += g4k::topk_scratch_bytes(kVocab);
  void* pin = nullptr;
  if (cudaHostAlloc(&pin, sizeof(int) * (4 + 2 * S * kDraftTopK + S), cudaHostAllocDefault) != cudaSuccess) {
    error = "cannot pin the drafter's transfer buffers";
    return false;
  }
  pin_ = static_cast<int*>(pin);
  vram_mib_ = (bytes + double(buffers)) / double(1 << 20);
  return true;
}

void Drafter::step(int j, int split_swa, int split_full, cudaStream_t st) {
  const int E = desc_.n_embd;
  const float eps = desc_.rms_eps;
  // the token: the chain's first (words_[0]) or the previous step's top-1
  const int* tok = j == 0 ? words_ : top_i_ + static_cast<std::size_t>(j - 1) * kDraftTopK;
  g4k::embed_q8(tok, 1, static_cast<const std::int8_t*>(t_.embd.w), t_.embd.s, kEmbd, std::sqrt(float(kEmbd)), xh_, st);
  g4k::matvec(pre_, xh_, x_, 1, st);
  for (const Layer& L : layers_) {
    g4k::rmsnorm(x_, L.attn_norm, nullptr, 1.0f, hn_, 1, E, eps, st);
    g4k::matvec(L.q, hn_, q_, 1, st);
    // Q only (the drafter has no K or V): per-head rms * q_norm, RoPE at pos0 (words_[1]); the full layers by the
    // drafter's own rope_freqs, as the target's
    g4k::QkvPost p{q_, nullptr, nullptr, L.q_norm, nullptr, L.sliding ? nullptr : rope_freqs_,
                   L.sliding ? desc_.rope_base_swa : desc_.rope_base_full, L.hd, kHeads, 0, 0, 1, 0, 0, eps,
                   qo_, nullptr, nullptr, nullptr, 0.0f, words_ + 1};
    g4k::qkv_post(p, st);
    // the target's keys before pos0: [pos0 - 1023, pos0 - 1] on a sliding layer (llama.cpp's mask: pos0 - key < 1024 over
    // the target's cells 0 .. pos0-1), [0, pos0 - 1] on a full one - decode attention at pos0 - 1 (words_[2])
    g4k::attn_decode_dev(qo_, L.sliding ? t_.kc_swa : t_.kc_full, L.sliding ? t_.vc_swa : t_.vc_full, L.hd, kHeads, L.n_kv,
                         L.sliding ? t_.cap_swa : t_.cap_full, L.sliding ? t_.ring : 0, L.sliding ? kWindow - 1 : 0, words_ + 2,
                         L.sliding ? split_swa : split_full, att_, scratch_, t_.max_splits, st);
    g4k::matvec(L.o, att_, o_, 1, st);
    g4k::add_rmsnorm(x_, o_, L.post_attn_norm, a_, 1, E, eps, st);
    g4k::rmsnorm(a_, L.ffn_norm, nullptr, 1.0f, hn_, 1, E, eps, st);
    g4k::matvec(L.gate, hn_, g_, 1, st);
    g4k::matvec(L.up, hn_, u_, 1, st);
    g4k::geglu(g_, L.n_ff, u_, L.n_ff, m_, 1, L.n_ff, st);
    g4k::matvec(L.down, m_, f_, 1, st);
    g4k::add_rmsnorm_scale(a_, f_, L.post_ffw_norm, L.out_scale, x_, 1, E, eps, st);
  }
  g4k::rmsnorm(x_, output_norm_, nullptr, 1.0f, n_, 1, E, eps, st);
  g4k::matvec(head_, n_, logits_, 1, st);
  g4k::topk(logits_, kVocab, kDraftTopK, top_v_ + static_cast<std::size_t>(j) * kDraftTopK, top_i_ + static_cast<std::size_t>(j) * kDraftTopK,
            flag_ + j, topk_scratch_, st);
  g4k::matvec(post_, n_, xh_ + kEmbd, 1, st);  // the next step's h
}

// Steps [j0, j1) of a chain, then their outputs to the pinned buffer; the chain's start (its words, its flags, its h)
// with step 0.
void Drafter::chain(int j0, int j1, int split_swa, int split_full, cudaStream_t st) {
  const std::size_t S = static_cast<std::size_t>(max_steps_);
  if (j0 == 0) {
    cudaMemcpyAsync(words_, pin_, 3 * sizeof(int), cudaMemcpyHostToDevice, st);
    cudaMemsetAsync(flag_, 0, S * sizeof(int), st);
    cudaMemcpyAsync(xh_ + kEmbd, h_in_, sizeof(float) * kEmbd, cudaMemcpyDeviceToDevice, st);
  }
  for (int j = j0; j < j1; ++j) step(j, split_swa, split_full, st);
  int* out = pin_ + 4;
  const std::size_t at = static_cast<std::size_t>(j0) * kDraftTopK, k = static_cast<std::size_t>(j1 - j0) * kDraftTopK;
  cudaMemcpyAsync(out + at, top_v_ + at, k * sizeof(float), cudaMemcpyDeviceToHost, st);
  cudaMemcpyAsync(out + S * kDraftTopK + at, top_i_ + at, k * sizeof(int), cudaMemcpyDeviceToHost, st);
  cudaMemcpyAsync(out + 2 * S * kDraftTopK + j0, flag_ + j0, static_cast<std::size_t>(j1 - j0) * sizeof(int), cudaMemcpyDeviceToHost, st);
}

bool Drafter::launch(int j0, int j1, int split_swa, int split_full, bool graph, cudaStream_t st, std::string& error) {
  if (graph && !graph_failed_) {
    const std::vector<int> key{j0, j1, split_swa, split_full};
    auto it = graphs_.find(key);
    if (it == graphs_.end()) {
      if (graphs_.size() >= 64) graphs_.clear();
      auto g = std::make_unique<strata::core::CapturedGraph>();
      std::string why;
      bool ok = g->begin(st, why);
      if (ok) {
        chain(j0, j1, split_swa, split_full, st);
        (void) cudaGetLastError();
        ok = g->end(st, why);
      }
      (void) cudaGetLastError();
      if (ok) {
        it = graphs_.emplace(key, std::move(g)).first;
      } else {   // plain launches from here on: a capture that failed once is not retried every step
        std::fprintf(stderr, "strata-gemma4: the draft graph could not be captured (%s); plain launches from here\n", why.c_str());
        graph_failed_ = true;
        graphs_.clear();
      }
    }
    if (it != graphs_.end()) {
      std::string why;
      if (!it->second->launch(st, why)) {
        error = why;
        return false;
      }
      return true;
    }
  }
  chain(j0, j1, split_swa, split_full, st);
  return true;
}

int Drafter::run(std::int32_t id_last, int pos0, int n, float p_min, std::int32_t* ids, float* probs, bool graph, cudaStream_t st,
                 std::string& error) {
  n = std::min(n, max_steps_);
  if (n <= 0 || pos0 < 1) return 0;
  pin_[0] = id_last;
  pin_[1] = pos0;
  pin_[2] = pos0 - 1;
  const int hi = pos0 - 1;
  const int split_swa = g4k::attn_decode_splits(g4k::attn_decode_keys(hi, kWindow - 1));
  const int split_full = g4k::attn_decode_splits(g4k::attn_decode_keys(hi, 0));
  if (split_full > t_.max_splits) {
    error = "the drafter's position is past the context";
    return -1;
  }
  const std::size_t S = static_cast<std::size_t>(max_steps_);
  const auto* vals = reinterpret_cast<const float*>(pin_ + 4);
  const int* top = pin_ + 4 + S * kDraftTopK;
  const int* flags = pin_ + 4 + 2 * S * kDraftTopK;
  auto sync = [&]() {
    const cudaError_t e = cudaStreamSynchronize(st);
    if (e != cudaSuccess || cudaGetLastError() != cudaSuccess) {
      error = std::string("the drafter failed: ") + cudaGetErrorString(e);
      return false;
    }
    return true;
  };
  // without a floor every draft is offered: the chain is one launch; with one, llama.cpp stops at the first draft below
  // it (speculative.cpp:1686), so the chain runs a step at a time and the steps after that one are never run
  const int part = p_min > 0.0f ? 1 : n;
  for (int j0 = 0; j0 < n; j0 += part) {
    const int j1 = std::min(n, j0 + part);
    if (!launch(j0, j1, split_swa, split_full, graph, st, error) || !sync()) return -1;
    for (int j = j0; j < j1; ++j) {
      if (flags[j] != 0) return j;  // non-finite logits: this draft and the ones built on it are not offered
      ids[j] = top[static_cast<std::size_t>(j) * kDraftTopK];
      probs[j] = draft_top_prob(vals + static_cast<std::size_t>(j) * kDraftTopK, kDraftTopK);
      if (probs[j] < p_min) return j;
    }
  }
  return n;
}

}  // namespace g4
