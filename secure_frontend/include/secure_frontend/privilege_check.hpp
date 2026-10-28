#pragma once

#include <optional>
#include <string_view>

namespace secure_frontend {

// Spec §3.12 / G4: the frontend runs as the invoking unprivileged user.
struct ProcessPrivileges final {
  bool posix_root{false};        // effective uid 0
  bool windows_elevated{false};  // TokenElevation.TokenIsElevated
};

[[nodiscard]] ProcessPrivileges current_process_privileges();
// Fixed text when start must be refused, nullopt otherwise.
[[nodiscard]] std::optional<std::string_view> privileged_start_error(const ProcessPrivileges& privileges) noexcept;

}  // namespace secure_frontend
