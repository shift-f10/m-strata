#include "u_dispatch.hpp"

#include <array>

namespace u {
namespace {

struct Entry final {
  std::string_view arch;
  FamilyId family;
};

// The one fixed arch -> family table (design §3.1). Adding a family means adding a row here
// (and its geometry spec in u_geometry); there is no runtime registry.
constexpr std::array<Entry, 9> kTable{{
    {"glm5next", FamilyId::glm5next},
    {"glm5-next", FamilyId::glm5next},  // llama.cpp spelling (unsloth uses "glm5next"); both map here
    {"ds4", FamilyId::ds4},
    // The real ds4 GGUFs (Maya's deepseek41 checkpoints) spell general.architecture "deepseek41"
    // (engines/maya-ref tools/ds4/pack_gguf.py:387). The guard reads <arch>.* with the file's own
    // prefix, so the same spec row serves both names.
    {"deepseek41", FamilyId::ds4},
    {"raon1", FamilyId::raon1},
    {"qwen35moe", FamilyId::qwen35moe},
    {"qwen4exp", FamilyId::qwen4exp},
    {"gemma4", FamilyId::gemma4},
    // flint (RecursiveSI, Apache-2.0; spec 2026-10-25-flint-engine-design): the GGUF's own
    // general.architecture string, as llama.cpp master's MODEL_ARCH.FLINT writes it.
    {"flint", FamilyId::flint},
}};

}  // namespace

std::string_view arch_of(FamilyId family) {
  for (const auto& e : kTable) {
    if (e.family == family) return e.arch;
  }
  return {};
}

std::string_view name_of(FamilyId family) { return arch_of(family); }

std::optional<FamilyId> family_for_arch(std::string_view arch) {
  for (const auto& e : kTable) {
    if (e.arch == arch) return e.family;
  }
  return std::nullopt;
}

}  // namespace u
