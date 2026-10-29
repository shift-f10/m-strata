// MTP speculative decoding's host rules (spec §12), after llama.cpp's draft-mtp driver (common/speculative.cpp,
// common_speculative_impl_draft_mtp) and its acceptance (common/sampling.cpp, common_sampler_sample_and_accept_n).
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace g4 {

// The verify window's rows at most (strata::kernels::kVerifyMaxT: the grouped expert kernels and the CPU lane's window
// dispatcher take a window of up to 8 tokens).
inline constexpr int kSpecMaxRows = 8;
// The drafter's candidates: llama.cpp's draft sampler is top-k 10 then dist (speculative.cpp:1397-1400), whose
// softmax gives the draft's probability.
inline constexpr int kDraftTopK = 10;

// The top-1 probability of the drafter's `k` highest logits (vals highest first): their softmax, in float, as
// llama_sampler_dist computes it after top-k.
[[nodiscard]] float draft_top_prob(const float* vals, int k);

// The drafts offered (at most n): those before the first whose probability is below p_min (speculative.cpp:1686).
[[nodiscard]] int drafts_kept(const float* probs, int n, float p_min);

// The drafts a step may offer: at most n_max, so that the window's answer stays within max_new (a window emits up to
// drafts + 1 tokens; `produced` already counts the token the window starts from) and its rows within the context (rows
// pos .. pos + drafts).
[[nodiscard]] int draft_budget(int n_max, long long produced, long long max_new, int pos, int ctx);

// common_sampler_sample_and_accept_n: rows 0, 1, ... sampled in order by `sample(row)` - the request's own sampler,
// whose history the caller extends with each sample - while each sample equals the row's draft. Returns the samples:
// the accepted drafts, then one more (the first row that disagreed, or the row after the last draft). A negative
// sample (-1 an engine failure, -2 non-finite logits) ends the list with it.
[[nodiscard]] std::vector<std::int32_t> accept_drafts(const std::vector<std::int32_t>& drafts, const std::function<int(int)>& sample);

}  // namespace g4
