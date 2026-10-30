#pragma once

// Email alerts' old password (spec 2026-10-27 §5a): roots.secret/smtp-password, Windows DPAPI for this user, elsewhere
// 0600. The email plugin keeps its password as its own secret now (plugin-email-password; plugin spec 2026-10-30, section
// 10), and the first start moves it there, once.

#include "secure_frontend/secret_bytes.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

inline constexpr std::string_view kSmtpPasswordFile = "smtp-password";

// nullopt: stored; else a fixed reason. 1-256 bytes without line breaks or NUL. (An old install's file: tests.)
[[nodiscard]] std::optional<std::string> store_smtp_password(const std::filesystem::path& secret_dir, std::string_view password);
[[nodiscard]] std::optional<SecretBytes> read_smtp_password(const std::filesystem::path& secret_dir);

// Moves smtp-password to the email plugin's password (plugin-email-password) and removes it: nullopt when there was none;
// else one fixed line for the log, never the password. A password the plugin already has stays (the old file is then
// removed too); one that cannot be read or moved is left where it is, and the line says so.
[[nodiscard]] std::optional<std::string> move_smtp_password(const std::filesystem::path& secret_dir);

}  // namespace secure_frontend
