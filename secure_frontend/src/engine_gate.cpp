#include "secure_frontend/engine_gate.hpp"

#include "secure_frontend/catalog.hpp"

#include <string>
#include <string_view>

namespace secure_frontend {

namespace {

// Gemma 4 plan 1 Task 2: fixed text (never manifest text); plan 4 opens the gate.
constexpr std::string_view kGemma4EngineMissing = "the Gemma 4 engine is not installed";
// unified-engine design §2.5 / plan 4: the merged engine (deepseek, raon1) is recognized but
// not yet installed — fixed text, never manifest text; it never claims runnable.
constexpr std::string_view kUnifiedEngineMissing = "the unified engine is not installed";

}  // namespace

std::optional<AcquireError> engine_gate(const AcquireManifest& manifest) {
  switch (manifest.profile) {
    case ManifestProfile::qwen:
      if (manifest.family != ManifestFamily::strata) {
        return AcquireError{AcquireReason::engine_profile_unavailable, std::string{profile_name(manifest.profile)}};
      }
      break;
    case ManifestProfile::gemma4:
      return AcquireError{AcquireReason::engine_profile_unavailable, std::string{kGemma4EngineMissing}};
    case ManifestProfile::deepseek:
    case ManifestProfile::raon1:
      return AcquireError{AcquireReason::engine_profile_unavailable, std::string{kUnifiedEngineMissing}};
    case ManifestProfile::glm53:
    case ManifestProfile::raw:
    case ManifestProfile::gguf:
    case ManifestProfile::mtp:
      return AcquireError{AcquireReason::engine_profile_unavailable, std::string{profile_name(manifest.profile)}};
  }
  if (compare_versions(manifest.min_version, built_engine_version()) > 0) {
    return AcquireError{AcquireReason::engine_too_old, "needs engine strata >= " + render_engine_version(manifest.min_version) +
                                                           ", this release has " + std::string{built_engine_version_text()}};
  }
  return std::nullopt;
}

std::string engine_state_text(const AcquireManifest& manifest) {
  if (!engine_gate(manifest)) return "runnable";
  if (manifest.profile == ManifestProfile::gemma4) return "engine not available (" + std::string{kGemma4EngineMissing} + ")";
  if (manifest.profile == ManifestProfile::deepseek || manifest.profile == ManifestProfile::raon1) {
    return "engine not available (" + std::string{kUnifiedEngineMissing} + ")";
  }
  if (manifest.family == ManifestFamily::none) return "not runnable (profile " + std::string{profile_name(manifest.profile)} + ")";
  return "needs engine " + std::string{family_name(manifest.family)} + " >= " + render_engine_version(manifest.min_version);
}

}  // namespace secure_frontend
