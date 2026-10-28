// The VRAM expert tier's start from a usage profile (paper 3.1; Strata's --expert-profile, `profile.bin` read by
// strata::core::read_expert_profile and written by write_expert_profile).
#pragma once

#include <utility>
#include <vector>

namespace g4 {

// Each layer's `slots` experts for a profile's ranked (layer, expert) pairs: that layer's experts in rank order (a
// pair ranked twice counts once; pairs out of range are skipped), then its lowest unranked experts. One list per
// layer, kLayers of them.
[[nodiscard]] std::vector<std::vector<int>> fill_from_profile(const std::vector<std::pair<int, int>>& ranked, int slots);

}  // namespace g4
