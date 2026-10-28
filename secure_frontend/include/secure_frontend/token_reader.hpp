#pragma once

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/secret_bytes.hpp"
#include "secure_frontend/secret_source.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>

namespace secure_frontend {

// Windows token files written by store_token: this magic followed by a
// CryptProtectData blob (user scope). Plaintext files under a private DACL are
// accepted too (operator-made).
inline constexpr std::string_view kDpapiTokenMagic = "STRATAK1";

// A token file larger than this is rejected before any token byte is examined.
inline constexpr std::size_t kMaximumTokenFileBytes = 4096U;
// The token itself (after the one optional trailing newline is removed).
inline constexpr std::size_t kMaximumTokenBytes = 256U;  // == kTokenWindowBytes
static_assert(kMaximumTokenBytes == kTokenWindowBytes, "a token must fit SecretBytes");
// Owner decision (spec §10): operator tokens shorter than this are rejected.
inline constexpr std::size_t kMinimumOperatorTokenBytes = 32U;

enum class TokenReadErrorCode {
  environment_variable_not_set,
  empty_token,
  invalid_token,
  secret_root_unavailable,
  token_not_found,
  path_escapes_root,
  symlink_not_allowed,
  not_regular_file,
  insecure_file_permissions,
  file_not_owned_by_current_user,
  read_failed,
  token_too_large,
  hard_link_not_allowed,
  token_too_short,
};

struct TokenReadError final {
  TokenReadErrorCode code;
  std::string message;
};

// The token is held in SecretBytes: inline, move-only, wiped on destruction and
// move-from. Every read buffer is wiped before it goes out of scope.
using TokenReadResult = std::variant<SecretBytes, TokenReadError>;

// Format rule (both OSes): 1-256 printable ASCII bytes 0x21-0x7E; a file may end
// in exactly one "\n" or "\r\n" (allow_trailing_newline). A UTF-8 BOM,
// whitespace, a second newline, or a token wrapped in quotes is invalid_token
// with a message naming the problem; fewer than 32 bytes is token_too_short.
[[nodiscard]] std::variant<SecretBytes, TokenReadError> token_from_bytes(std::string_view raw,
                                                                         bool allow_trailing_newline);

// Resolves a SecretSource at the last responsible moment. For a protected file
// the root is opened once (open_root) and the file is opened below it with
// open_beneath (no symlink, no mount crossing, no second hard link); on Linux
// it must then be owned by the effective user with no group/other bits.
//
// On Windows (ruling R7) the root is opened through its link — the root itself
// may be a symlink or junction — and every component below it is opened with
// READ_CONTROL | FILE_READ_ATTRIBUTES and FILE_FLAG_OPEN_REPARSE_POINT; a
// reparse point below the root is symlink_not_allowed; each handle's final path must lie at or below the
// root's (path_escapes_root); the root, every directory and the file must be
// owned by the current user with a protected DACL whose only allow-ACEs name
// that user, SYSTEM or Administrators (insecure_file_permissions); a file with
// more than one hard link is rejected. Runtime-tested only by the
// windows-msys2-clang job (label windows-runtime).
[[nodiscard]] TokenReadResult read_token(const SecretSource& source, const CanonicalRoot& protected_file_root);
// Opens `protected_file_root` with open_root first; used where only a path is known.
[[nodiscard]] TokenReadResult read_token(const SecretSource& source,
                                         const std::filesystem::path& protected_file_root);
// Maps a canonical-path failure to the token reader's fixed codes and messages.
[[nodiscard]] TokenReadError token_error_from(const PathError& error);

}  // namespace secure_frontend
