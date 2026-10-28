#pragma once

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/token_store.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

enum class InitErrorCode { invalid_config, config_exists, token_exists, write_failed, token_failed };

// message is fixed text; issues carries config validation problems by key.
struct InitError final {
  InitErrorCode code;
  std::string message;
  ConfigErrors issues;
};

// Both atomic and owner-only; both refuse to overwrite unless `force`.
[[nodiscard]] std::optional<TokenStoreError> write_default_config(const CanonicalRoot& dir, const RelativePath& file,
                                                                  std::string_view json, bool force);
[[nodiscard]] std::optional<TokenStoreError> generate_token(const CanonicalRoot& secret_root, const RelativePath& file,
                                                            bool force);

// The mechanics behind `strata-secure-server init` (spec §3.9). The UX --
// default locations, the strata wrapper, what is printed -- belongs to the
// product-experience spec.
[[nodiscard]] std::optional<InitError> init_frontend(const std::filesystem::path& config_file,
                                                     const FrontendConfigDraft& flags, bool force);

}  // namespace secure_frontend
