#pragma once

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/secret_bytes.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

enum class TokenStoreMode { create_new, replace };
enum class TokenStoreErrorCode { already_exists, unsafe_location, write_failed, protect_failed };

// Fixed text; never the path or the token.
struct TokenStoreError final {
  TokenStoreErrorCode code;
  std::string message;
};

// Writes `bytes` to root/file so a reader sees the old or the new content,
// never a partial file. The directory holding the file must be owned by the
// user with no group/other access (Linux) or carry a private owner DACL
// (Windows); an existing target must be a regular file (a symlink is refused).
[[nodiscard]] std::optional<TokenStoreError> write_private_file(const CanonicalRoot& root, const RelativePath& file,
                                                                std::string_view bytes, TokenStoreMode mode);

[[nodiscard]] std::optional<TokenStoreError> store_token(const CanonicalRoot& secret_root, const RelativePath& file,
                                                         const SecretBytes& token, TokenStoreMode mode);

}  // namespace secure_frontend
