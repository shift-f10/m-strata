#pragma once

#include <string>
#include <string_view>
#include <variant>

namespace secure_frontend {

// Describes where an API token is obtained. This type deliberately stores
// source metadata only; it has no field for the resolved token value.
enum class SecretSourceKind {
  environment_variable,
  protected_file,
};

enum class SecretSourceErrorCode {
  invalid_environment_variable_name,
  invalid_protected_file_path,
};

struct SecretSourceError final {
  SecretSourceErrorCode code;
  std::string message;
};

class SecretSource final {
 public:
  [[nodiscard]] SecretSourceKind kind() const noexcept;
  [[nodiscard]] const std::string& location() const noexcept;

 private:
  friend std::variant<SecretSource, SecretSourceError> make_environment_secret_source(
      std::string_view environment_variable_name);
  friend std::variant<SecretSource, SecretSourceError> make_protected_file_secret_source(
      std::string_view protected_file_path);

  SecretSource(SecretSourceKind kind, std::string location);

  SecretSourceKind kind_;
  std::string location_;
};

using SecretSourceValidation = std::variant<SecretSource, SecretSourceError>;

// Environment names use the portable ASCII form [A-Za-z_][A-Za-z0-9_]*.
// The caller resolves this name only at the point a token is needed.
[[nodiscard]] SecretSourceValidation make_environment_secret_source(
    std::string_view environment_variable_name);

// This accepts only a path in the canonical-path module's relative grammar
// (parse_relative_path), below a caller-configured secret root; the token
// reader opens it with open_beneath.
[[nodiscard]] SecretSourceValidation make_protected_file_secret_source(
    std::string_view protected_file_path);

}  // namespace secure_frontend
