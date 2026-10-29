#include "g4_spec.hpp"

#include <algorithm>
#include <cmath>

namespace g4 {

float draft_top_prob(const float* vals, const int k) {
  if (k <= 0) return 0.0f;
  // llama_sampler_softmax_impl: p_i = expf(l_i - max) / sum, in float
  float sum = 0.0f;
  for (int i = 0; i < k; ++i) sum += std::exp(vals[i] - vals[0]);
  return 1.0f / sum;
}

int drafts_kept(const float* probs, const int n, const float p_min) {
  int i = 0;
  while (i < n && probs[i] >= p_min) ++i;
  return i;
}

int draft_budget(const int n_max, const long long produced, const long long max_new, const int pos, const int ctx) {
  const long long by_answer = max_new - produced - 1;
  const long long by_context = static_cast<long long>(ctx) - pos - 1;
  return static_cast<int>(std::max(0LL, std::min({static_cast<long long>(n_max), by_answer, by_context})));
}

std::vector<std::int32_t> accept_drafts(const std::vector<std::int32_t>& drafts, const std::function<int(int)>& sample) {
  std::vector<std::int32_t> out;
  out.reserve(drafts.size() + 1);
  for (std::size_t i = 0; i < drafts.size(); ++i) {
    const int s = sample(static_cast<int>(i));
    out.push_back(s);
    if (s < 0 || s != drafts[i]) return out;
  }
  out.push_back(sample(static_cast<int>(drafts.size())));
  return out;
}

}  // namespace g4
