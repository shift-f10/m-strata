#pragma once

// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §5a): the connection the email
// alerts send over - built only with SECURE_FRONTEND_EMAIL_ALERTS, and the server's only outbound connection. TCP to
// host:port through the system's resolver, then TLS from the first byte (SmtpSecurity::ssl) or at STARTTLS: TLS 1.2 or
// later only, the server's certificate checked against the system's trust store and the host name. Windows: SChannel
// (SSPI); elsewhere: OpenSSL.

#include "secure_frontend/smtp_client.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace secure_frontend {

// nullptr and a fixed `error` (never a secret) when the server cannot be reached or the TLS handshake fails.
[[nodiscard]] std::unique_ptr<SmtpTransport> connect_smtp(const std::string& host, std::uint16_t port, SmtpSecurity security,
                                                          std::chrono::seconds timeout, std::string* error);

}  // namespace secure_frontend
