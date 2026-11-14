#pragma once

#include "secure_frontend/canonical_path.hpp"
#include <string>
#include <variant>

namespace secure_frontend {
// The local manifest detects incomplete/mixed installations; it is not a signed
// provenance boundary. The same trusted installation owns binaries and hashes.
[[nodiscard]] std::variant<CanonicalFile, std::string> select_engine_variant(
    const CanonicalFile& accurate, bool fast_math);
}  // namespace secure_frontend
