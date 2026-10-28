#pragma once

#include "secure_frontend/acquire_manifest.hpp"
#include "secure_frontend/acquire_reason.hpp"

#include <optional>
#include <string>

namespace secure_frontend {

// Spec §2.3: what this release can run. One implementation (F-58) for list/show's
// state, `run` (Task 27) and the managed launch (Task 24b): a profile other than
// qwen, or a family other than strata, is engine_profile_unavailable; a min_version
// above the built engine is engine_too_old. Both are exit 11 for run/serve.
[[nodiscard]] std::optional<AcquireError> engine_gate(const AcquireManifest& manifest);

// §4.5 list state from engine_gate: "runnable" or "needs engine <family> >= <version>".
[[nodiscard]] std::string engine_state_text(const AcquireManifest& manifest);

}  // namespace secure_frontend
