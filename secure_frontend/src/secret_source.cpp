#include "secure_frontend/secret_source.hpp"

#include "secure_frontend/canonical_path.hpp"

#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

bool is_ascii_letter(const char value) {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

bool is_ascii_digit(const char value) {
  return value >= '0' && value <= '9';
}

bool is_valid_environment_variable_name(std::string_view name) {
  if (name.empty() || name.size() > 128U || !(is_ascii_letter(name.front()) || name.front() == '_')) {
    return false;
  }

  for (const char character : name) {
    if (!(is_ascii_letter(character) || is_ascii_digit(character) || character == '_')) {
      return false;
    }
  }
  return true;
}

}  // namespace

SecretSource::SecretSource(const SecretSourceKind kind, std::string location)
    : kind_(kind), location_(std::move(location)) {}

SecretSourceKind SecretSource::kind() const noexcept {
  return kind_;
}

const std::string& SecretSource::location() const noexcept {
  return location_;
}

SecretSourceValidation make_environment_secret_source(const std::string_view environment_variable_name) {
  if (!is_valid_environment_variable_name(environment_variable_name)) {
    return SecretSourceError{.code = SecretSourceErrorCode::invalid_environment_variable_name,
                             .message = "Environment variable names must use [A-Za-z_][A-Za-z0-9_]*."};
  }

  return SecretSource{SecretSourceKind::environment_variable, std::string{environment_variable_name}};
}

SecretSourceValidation make_protected_file_secret_source(const std::string_view protected_file_path) {
  const auto parsed = parse_relative_path(protected_file_path);
  const auto* path = std::get_if<RelativePath>(&parsed);
  if (path == nullptr) {
    return SecretSourceError{.code = SecretSourceErrorCode::invalid_protected_file_path,
                             .message = "Protected-file paths must be safe relative paths below the configured secret root."};
  }
  return SecretSource{SecretSourceKind::protected_file, path->generic()};
}

}  // namespace secure_frontend
