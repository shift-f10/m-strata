#pragma once
#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"

#include <aclapi.h>
#include <sddl.h>

#include <array>
#include <cstddef>
#include <string>

namespace secure_frontend::windows_security {

bool current_user_sid(std::array<std::byte, SECURITY_MAX_SID_SIZE>& storage, PSID* result);
std::wstring current_user_sid_string();
// Owner is the user; DACL protected; allow-ACEs only for the user, SYSTEM, Administrators.
bool has_private_owner_dacl(HANDLE handle, PSID current_user);
enum class Trust { trusted, untrusted_owner, shared_writable, unreadable };
// Owner is the user/SYSTEM/Administrators and no effective allow-ACE grants a
// write right to anyone else (the Windows form of "not group/other-writable").
// also_trusted_installer (SP4 §3.2, pull.downloader only): the TrustedInstaller
// service SID is an additional permitted owner and writer. Default: unchanged.
Trust write_access_is_private(HANDLE handle, bool also_trusted_installer = false);
std::wstring final_path_for_handle(HANDLE handle);
bool is_reparse_point(HANDLE handle);
std::wstring extended_path(const std::wstring& absolute);

// O:<user>D:P(A;[OICI];FA;;;<user>)(A;[OICI];FA;;;SY): accepted by has_private_owner_dacl.
class PrivateSecurityAttributes final {
 public:
  explicit PrivateSecurityAttributes(bool directory);
  ~PrivateSecurityAttributes();
  PrivateSecurityAttributes(const PrivateSecurityAttributes&) = delete;
  PrivateSecurityAttributes& operator=(const PrivateSecurityAttributes&) = delete;
  [[nodiscard]] SECURITY_ATTRIBUTES* get() noexcept { return descriptor_ != nullptr ? &attributes_ : nullptr; }

 private:
  PSECURITY_DESCRIPTOR descriptor_{nullptr};
  SECURITY_ATTRIBUTES attributes_{};
};

}  // namespace secure_frontend::windows_security
#endif
