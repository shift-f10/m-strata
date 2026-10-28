#include "secure_frontend/privilege_check.hpp"

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <unistd.h>
#endif

namespace secure_frontend {

ProcessPrivileges current_process_privileges() {
  ProcessPrivileges privileges;
#if defined(_WIN32)
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    privileges.windows_elevated = GetTokenInformation(token, TokenElevation, &elevation, size, &size) && elevation.TokenIsElevated != 0;
    CloseHandle(token);
  }
#else
  privileges.posix_root = geteuid() == 0;
#endif
  return privileges;
}

std::optional<std::string_view> privileged_start_error(const ProcessPrivileges& privileges) noexcept {
  if (privileges.posix_root || privileges.windows_elevated) {
    return "Refusing to run as root or with an elevated token; start strata-secure-server as an unprivileged user.";
  }
  return std::nullopt;
}

}  // namespace secure_frontend
