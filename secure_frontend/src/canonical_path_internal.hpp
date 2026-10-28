#pragma once
#include "secure_frontend/canonical_path.hpp"

#include <memory>
#include <string>
#include <utility>

namespace secure_frontend::detail {

struct PathFactory final {
  static CanonicalRoot root(std::filesystem::path path, const NativeHandle handle, const std::uint64_t device,
                            const bool external = false) {
    return CanonicalRoot{std::move(path), std::make_shared<const OwnedHandle>(handle), device, external};
  }
  static CanonicalFile file(std::filesystem::path path) { return CanonicalFile{std::move(path)}; }
  static OpenedFile opened(const NativeHandle handle, const std::uint64_t size) { return OpenedFile{handle, size}; }
};

inline PathError path_error(const PathErrorCode code, std::string message) { return PathError{code, std::move(message)}; }

// Opens an existing root (platform files); open_root() wraps it. RootPolicy::external
// skips the owner/writability checks and marks the root external.
// also_trusted_installer: Windows only (see ExecutableWriters); ignored on POSIX.
std::variant<CanonicalRoot, PathError> open_existing_root(const std::filesystem::path& requested,
                                                          RootPolicy policy = RootPolicy::existing,
                                                          bool also_trusted_installer = false);

}  // namespace secure_frontend::detail
