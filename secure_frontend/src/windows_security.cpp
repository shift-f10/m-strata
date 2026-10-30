#if defined(_WIN32)
#include "windows_security.hpp"

#include <string>
#include <string_view>

namespace secure_frontend::windows_security {
namespace {

// NT SERVICE\TrustedInstaller. A file-local copy, so SP1 has no dependency on SP4's
// confinement_plan.hpp (which holds the same SID for the downloader's DACL plan).
constexpr std::wstring_view kTrustedInstallerSid = L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

// File-local RAII for the process token (OwnedHandle is the path module's handle type).
class ProcessToken final {
 public:
  explicit ProcessToken(const HANDLE handle) noexcept : handle_(handle) {}
  ~ProcessToken() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
  }
  ProcessToken(const ProcessToken&) = delete;
  ProcessToken& operator=(const ProcessToken&) = delete;
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_;
};

class LocalSecurityDescriptor final {
 public:
  explicit LocalSecurityDescriptor(PSECURITY_DESCRIPTOR descriptor = nullptr) noexcept : descriptor_(descriptor) {}
  ~LocalSecurityDescriptor() {
    if (descriptor_ != nullptr) {
      LocalFree(descriptor_);
    }
  }
  LocalSecurityDescriptor(const LocalSecurityDescriptor&) = delete;
  LocalSecurityDescriptor& operator=(const LocalSecurityDescriptor&) = delete;

 private:
  PSECURITY_DESCRIPTOR descriptor_;
};

bool sid_is_well_known(const PSID sid, const WELL_KNOWN_SID_TYPE type) {
  DWORD sid_size = SECURITY_MAX_SID_SIZE;
  std::array<std::byte, SECURITY_MAX_SID_SIZE> buffer{};
  PSID expected = buffer.data();
  if (CreateWellKnownSid(type, nullptr, expected, &sid_size) == 0) {
    return false;
  }
  return EqualSid(sid, expected) != 0;
}

bool is_permitted_acl_principal(const PSID principal, const PSID owner) {
  return EqualSid(principal, owner) != 0 || sid_is_well_known(principal, WinLocalSystemSid) ||
         sid_is_well_known(principal, WinBuiltinAdministratorsSid);
}

// SP4 §3.2: NT SERVICE\TrustedInstaller (a service SID, so no WELL_KNOWN_SID_TYPE).
bool is_trusted_installer(const PSID principal) {
  const std::wstring text{kTrustedInstallerSid};
  PSID expected = nullptr;
  if (ConvertStringSidToSidW(text.c_str(), &expected) == 0) return false;
  const bool equal = EqualSid(principal, expected) != 0;
  LocalFree(expected);
  return equal;
}

}  // namespace

std::wstring final_path_for_handle(const HANDLE handle) {
  const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0U, FILE_NAME_NORMALIZED);
  if (required == 0U || required > 32768U) {
    return {};
  }
  std::wstring path(required, L'\0');
  const DWORD written = GetFinalPathNameByHandleW(handle, path.data(), required, FILE_NAME_NORMALIZED);
  if (written == 0U || written >= required) {
    return {};
  }
  path.resize(written);
  return path;
}

bool is_reparse_point(const HANDLE handle) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  return GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) == 0 ||
         (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
}

bool current_user_sid(std::array<std::byte, SECURITY_MAX_SID_SIZE>& storage, PSID* const result) {
  HANDLE raw_token = INVALID_HANDLE_VALUE;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) == 0) {
    return false;
  }
  const ProcessToken token(raw_token);
  DWORD bytes_required = 0U;
  (void)GetTokenInformation(token.get(), TokenUser, nullptr, 0U, &bytes_required);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes_required == 0U || bytes_required > 4096U) {
    return false;
  }
  std::array<std::byte, 4096U> information{};
  if (GetTokenInformation(token.get(), TokenUser, information.data(), bytes_required, &bytes_required) == 0) {
    return false;
  }
  const auto* const user = reinterpret_cast<const TOKEN_USER*>(information.data());
  DWORD sid_size = SECURITY_MAX_SID_SIZE;
  if (CopySid(sid_size, storage.data(), user->User.Sid) == 0) {
    return false;
  }
  *result = storage.data();
  return true;
}

// A protected token must not rely on inherited ACLs: an ACL inherited from a
// writable ancestor can be silently broadened after deployment.  The only
// allowed allow-ACEs are the service user, LocalSystem, and Administrators.
// The latter two are privileged operating-system principals, not broad users.
bool has_private_owner_dacl(const HANDLE handle, const PSID current_user) {
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
  const DWORD status = GetSecurityInfo(handle, SE_FILE_OBJECT,
                                       OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                       &owner, nullptr, &dacl, nullptr, &raw_descriptor);
  LocalSecurityDescriptor descriptor(raw_descriptor);
  if (status != ERROR_SUCCESS || owner == nullptr || dacl == nullptr || EqualSid(owner, current_user) == 0) {
    return false;
  }

  SECURITY_DESCRIPTOR_CONTROL control = 0U;
  DWORD revision = 0U;
  if (GetSecurityDescriptorControl(raw_descriptor, &control, &revision) == 0 ||
      (control & SE_DACL_PROTECTED) == 0U) {
    return false;
  }

  for (DWORD index = 0U; index < dacl->AceCount; ++index) {
    void* raw_ace = nullptr;
    if (GetAce(dacl, index, &raw_ace) == 0 || raw_ace == nullptr) {
      return false;
    }
    const auto* const header = static_cast<const ACE_HEADER*>(raw_ace);
    if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
      continue;
    }
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
      return false;
    }
    const auto* const allowed = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
    PSID ace_sid = reinterpret_cast<PSID>(const_cast<DWORD*>(&allowed->SidStart));
    if (IsValidSid(ace_sid) == 0 || !is_permitted_acl_principal(ace_sid, owner)) {
      return false;
    }
  }
  return true;
}

std::wstring current_user_sid_string() {
  std::array<std::byte, SECURITY_MAX_SID_SIZE> storage{};
  PSID sid = nullptr;
  if (!current_user_sid(storage, &sid)) return {};
  LPWSTR text = nullptr;
  if (ConvertSidToStringSidW(sid, &text) == 0) return {};
  std::wstring out{text};
  LocalFree(text);
  return out;
}

Trust write_access_is_private(const HANDLE handle, const bool also_trusted_installer) {
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR raw = nullptr;
  if (GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
                      &dacl, nullptr, &raw) != ERROR_SUCCESS) {
    return Trust::unreadable;
  }
  LocalSecurityDescriptor guard(raw);
  std::array<std::byte, SECURITY_MAX_SID_SIZE> storage{};
  PSID user = nullptr;
  if (!current_user_sid(storage, &user)) return Trust::unreadable;
  const auto permitted = [user, also_trusted_installer](const PSID principal) {
    return is_permitted_acl_principal(principal, user) || (also_trusted_installer && is_trusted_installer(principal));
  };
  if (owner == nullptr || !permitted(owner)) return Trust::untrusted_owner;
  if (dacl == nullptr) return Trust::shared_writable;  // a NULL DACL grants everyone full access
  constexpr ACCESS_MASK kWrite = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
                                 FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
  for (DWORD index = 0U; index < dacl->AceCount; ++index) {
    void* raw_ace = nullptr;
    if (GetAce(dacl, index, &raw_ace) == 0 || raw_ace == nullptr) return Trust::unreadable;
    const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
    if ((header->AceFlags & INHERIT_ONLY_ACE) != 0U || header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) return Trust::shared_writable;  // fail closed on other allow forms
    const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
    PSID sid = reinterpret_cast<PSID>(const_cast<DWORD*>(&allowed->SidStart));
    if ((allowed->Mask & kWrite) != 0U && !permitted(sid)) return Trust::shared_writable;
  }
  return Trust::trusted;
}

std::wstring extended_path(const std::wstring& absolute) {
  if (absolute.rfind(L"\\\\?\\", 0) == 0) return absolute;
  // a share's folder (\\server\share\...): \\?\UNC\server\share\...
  if (absolute.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + absolute.substr(2);
  return L"\\\\?\\" + absolute;
}

PrivateSecurityAttributes::PrivateSecurityAttributes(const bool directory) {
  const std::wstring sid = current_user_sid_string();
  if (sid.empty()) return;
  const std::wstring inherit = directory ? L"OICI" : L"";
  const std::wstring sddl = L"O:" + sid + L"D:P(A;" + inherit + L";FA;;;" + sid + L")(A;" + inherit + L";FA;;;SY)";
  if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr) == 0) {
    descriptor_ = nullptr;
    return;
  }
  attributes_ = SECURITY_ATTRIBUTES{sizeof(SECURITY_ATTRIBUTES), descriptor_, FALSE};
}

PrivateSecurityAttributes::~PrivateSecurityAttributes() {
  if (descriptor_ != nullptr) LocalFree(descriptor_);
}

}  // namespace secure_frontend::windows_security
#endif
