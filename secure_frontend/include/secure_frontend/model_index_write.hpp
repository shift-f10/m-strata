#pragma once

#include "secure_frontend/acquire_reason.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/model_index_model.hpp"

#include <optional>

namespace secure_frontend {

// §4.5 step 5: atomic replace of roots.models/index.json (0600 / private DACL).
// Any failure, including a rendering over kIndexMaxBytes, is index_write_failed.
[[nodiscard]] std::optional<AcquireError> write_model_index(const CanonicalRoot& models, const ModelIndex& index);

}  // namespace secure_frontend
