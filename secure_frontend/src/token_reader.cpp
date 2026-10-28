#include "secure_frontend/token_reader.hpp"
#include "secure_frontend/crt_environment.hpp"

#include "secure_frontend/secret_bytes.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"

#include <wincrypt.h>
#include <dpapi.h>
#endif

namespace secure_frontend {
namespace {

TokenReadError error(const TokenReadErrorCode code, std::string message) {
  return TokenReadError{.code = code, .message = std::move(message)};
}

// A stack buffer that is wiped when it leaves scope, whatever the exit path.
// One byte over the file cap, so a file that grew past the cap after the size
// check is seen as too large rather than silently truncated.
struct WipedBuffer final {
  std::array<char, kMaximumTokenFileBytes + 1U> bytes{};
  WipedBuffer() = default;
  WipedBuffer(const WipedBuffer&) = delete;
  WipedBuffer& operator=(const WipedBuffer&) = delete;
  ~WipedBuffer() { secure_wipe(bytes.data(), bytes.size()); }
};

TokenReadResult read_protected_file(const SecretSource& source, const CanonicalRoot& root) {
  const auto parsed = parse_relative_path(source.location());
  const auto* relative = std::get_if<RelativePath>(&parsed);
  if (relative == nullptr) return error(TokenReadErrorCode::read_failed, "The protected token file path is not valid.");
  auto opened = open_beneath(root, *relative,
                             OpenOptions{.kind = OpenKind::regular_file, .trust = FileTrust::owner_only,
                                         .reject_hard_links = true});
  if (const auto* failure = std::get_if<PathError>(&opened)) return token_error_from(*failure);
  const auto& file = std::get<OpenedFile>(opened);
  if (file.size() > kMaximumTokenFileBytes) return error(TokenReadErrorCode::token_too_large, "The token file is larger than 4 KiB.");
  // Read straight into wiped storage, not OpenedFile::read_all: its 64 KiB chunk
  // and std::string reallocations would leave token bytes behind unwiped.
  WipedBuffer buffer;
  std::size_t used = 0U;
  while (used < buffer.bytes.size()) {
    auto got = file.read_some(buffer.bytes.data() + used, buffer.bytes.size() - used);
    if (std::holds_alternative<PathError>(got)) return error(TokenReadErrorCode::read_failed, "The protected token file could not be read.");
    const std::size_t count = std::get<std::size_t>(got);
    if (count == 0U) break;
    used += count;
  }
  // A file that grew past the cap after the size check is too large, not a read failure.
  if (used > kMaximumTokenFileBytes) return error(TokenReadErrorCode::token_too_large, "The token file is larger than 4 KiB.");
  const std::string_view raw{buffer.bytes.data(), used};
#if defined(_WIN32)
  if (raw.substr(0, kDpapiTokenMagic.size()) == kDpapiTokenMagic) {
    const std::string_view blob = raw.substr(kDpapiTokenMagic.size());
    DATA_BLOB input{static_cast<DWORD>(blob.size()), reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))};
    DATA_BLOB output{};
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output) == 0) {
      return error(TokenReadErrorCode::invalid_token, "The token file could not be decrypted for the current user.");
    }
    auto result = token_from_bytes(std::string_view{reinterpret_cast<const char*>(output.pbData), output.cbData}, false);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
  }
#endif
  return token_from_bytes(raw, true);
}

TokenReadResult read_environment_token(const SecretSource& source) {
  const std::string& name = source.location();
  char* const value = secure_frontend::crt_getenv(name.c_str());
  if (value == nullptr) {
    return error(TokenReadErrorCode::environment_variable_not_set, "The configured token environment variable is not set.");
  }
  auto result = token_from_bytes(value, false);
  // Overwrite the value where it lives, then remove the variable, so
  // /proc/<pid>/environ and every later child lack it (CFG-05).
  secure_wipe(value, std::strlen(value));
#if defined(_WIN32)
  // Accepted deviation (spec §3.6): Windows has no /proc/<pid>/environ. The CRT
  // copies are wiped in place -- the narrow one above, the wide one here (the
  // name is ASCII by the env-name grammar) -- then the variable is removed from
  // the CRT and from the Win32 environment block. The block entry is removed,
  // not overwritten in place: no Win32 API can do that.
  const std::wstring wide_name(name.begin(), name.end());
  if (wchar_t* const wide = secure_frontend::crt_wgetenv(wide_name.c_str())) secure_wipe(wide, std::wcslen(wide) * sizeof(wchar_t));
  (void)_putenv_s(name.c_str(), "");
  (void)SetEnvironmentVariableW(wide_name.c_str(), nullptr);
#else
  (void)::unsetenv(name.c_str());
#endif
  return result;
}

}  // namespace

std::variant<SecretBytes, TokenReadError> token_from_bytes(const std::string_view raw, const bool allow_trailing_newline) {
  if (raw.size() >= 3U && raw.substr(0, 3) == "\xEF\xBB\xBF") {
    return error(TokenReadErrorCode::invalid_token, "The token file starts with a UTF-8 byte order mark; save it without one.");
  }
  std::string_view value = raw;
  if (allow_trailing_newline) {
    if (value.size() >= 2U && value.substr(value.size() - 2U) == "\r\n") value.remove_suffix(2U);
    else if (!value.empty() && value.back() == '\n') value.remove_suffix(1U);
  }
  if (value.empty()) return error(TokenReadErrorCode::empty_token, "The resolved token must not be empty.");
  if (value.size() > kMaximumTokenBytes) return error(TokenReadErrorCode::token_too_large, "The token is longer than 256 bytes.");
  for (const char c : value) {
    const auto b = static_cast<unsigned char>(c);
    if (b >= 0x21U && b <= 0x7eU) continue;
    if (c == ' ' || c == '\t') {
      return error(TokenReadErrorCode::invalid_token, "The token contains whitespace; only one trailing newline is allowed.");
    }
    if (c == '\n' || c == '\r') {
      return error(TokenReadErrorCode::invalid_token,
                   "The token file has more than one trailing newline (or a line break inside the token).");
    }
    return error(TokenReadErrorCode::invalid_token, "The token must be printable ASCII.");
  }
  if (value.size() >= 2U && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
    return error(TokenReadErrorCode::invalid_token, "The token is wrapped in quotes; remove them.");
  }
  if (value.size() < kMinimumOperatorTokenBytes) {
    return error(TokenReadErrorCode::token_too_short,
                 "Tokens must be at least 32 bytes; run strata-secure-server init to generate one.");
  }
  SecretBytes token;
  (void)token.assign(value);
  return token;
}

TokenReadError token_error_from(const PathError& failure) {
  switch (failure.code) {
    case PathErrorCode::symlink_not_allowed: return error(TokenReadErrorCode::symlink_not_allowed, "Protected token paths may not include links or reparse points.");
    case PathErrorCode::escapes_root:
    case PathErrorCode::cross_device: return error(TokenReadErrorCode::path_escapes_root, "The protected token file resolves outside the configured root.");
    case PathErrorCode::not_found: return error(TokenReadErrorCode::token_not_found, "The protected token file does not exist.");
    case PathErrorCode::not_directory:
    case PathErrorCode::not_regular_file: return error(TokenReadErrorCode::not_regular_file, "The protected token path must be a regular file.");
    case PathErrorCode::not_owned_by_user: return error(TokenReadErrorCode::file_not_owned_by_current_user, "The protected token file must be owned by the current user.");
    case PathErrorCode::permissions_too_broad:
    case PathErrorCode::untrusted_owner:
    case PathErrorCode::shared_writable: return error(TokenReadErrorCode::insecure_file_permissions, "The protected token file must not grant access to other users.");
    case PathErrorCode::hard_link_not_allowed: return error(TokenReadErrorCode::hard_link_not_allowed, "The protected token file must not have other hard links.");
    case PathErrorCode::not_absolute:
    case PathErrorCode::unsupported_root_form:
    case PathErrorCode::network_location:
    case PathErrorCode::root_unavailable: return error(TokenReadErrorCode::secret_root_unavailable, "The configured protected-file root is unavailable.");
    default: return error(TokenReadErrorCode::read_failed, "The protected token file could not be opened safely.");
  }
}

TokenReadResult read_token(const SecretSource& source, const CanonicalRoot& protected_file_root) {
  if (source.kind() == SecretSourceKind::environment_variable) return read_environment_token(source);
  return read_protected_file(source, protected_file_root);
}

TokenReadResult read_token(const SecretSource& source, const std::filesystem::path& protected_file_root) {
  if (source.kind() == SecretSourceKind::environment_variable) return read_environment_token(source);
  auto root = open_root(protected_file_root);
  if (const auto* failure = std::get_if<PathError>(&root)) return token_error_from(*failure);
  return read_protected_file(source, std::get<CanonicalRoot>(root));
}

}  // namespace secure_frontend
