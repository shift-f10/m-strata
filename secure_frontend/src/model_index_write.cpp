#include "secure_frontend/model_index_write.hpp"

#include "secure_frontend/models_store.hpp"

#include <string>

namespace secure_frontend {

std::optional<AcquireError> write_model_index(const CanonicalRoot& models, const ModelIndex& index) {
  const std::string text = render_model_index(index);
  if (text.size() > kIndexMaxBytes) return AcquireError{AcquireReason::index_write_failed, "index.json would exceed 4 MiB"};
  if (auto failure = replace_small_file(models, must_rel(kIndexFileName), text)) {
    return AcquireError{AcquireReason::index_write_failed, failure->detail};
  }
  return std::nullopt;
}

}  // namespace secure_frontend
