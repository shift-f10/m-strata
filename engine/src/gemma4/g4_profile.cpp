#include "g4_profile.hpp"

#include "g4_model.hpp"

#include <algorithm>

namespace g4 {

std::vector<std::vector<int>> fill_from_profile(const std::vector<std::pair<int, int>>& ranked, int slots) {
  const int s = std::clamp(slots, 0, kExperts);
  std::vector<std::vector<int>> fill(static_cast<std::size_t>(kLayers));
  std::vector<std::vector<char>> taken(static_cast<std::size_t>(kLayers), std::vector<char>(static_cast<std::size_t>(kExperts), 0));
  for (const auto& [l, e] : ranked) {
    if (l < 0 || l >= kLayers || e < 0 || e >= kExperts) continue;
    auto& f = fill[static_cast<std::size_t>(l)];
    char& t = taken[static_cast<std::size_t>(l)][static_cast<std::size_t>(e)];
    if (t != 0 || static_cast<int>(f.size()) >= s) continue;
    t = 1;
    f.push_back(e);
  }
  for (int l = 0; l < kLayers; ++l) {
    auto& f = fill[static_cast<std::size_t>(l)];
    for (int e = 0; e < kExperts && static_cast<int>(f.size()) < s; ++e)
      if (taken[static_cast<std::size_t>(l)][static_cast<std::size_t>(e)] == 0) f.push_back(e);
  }
  return fill;
}

}  // namespace g4
