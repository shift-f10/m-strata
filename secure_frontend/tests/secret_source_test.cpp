#include "secure_frontend/secret_source.hpp"
#include "support/test_support.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <variant>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

const secure_frontend::SecretSource* source_from(
    const secure_frontend::SecretSourceValidation& result) {
  return std::get_if<secure_frontend::SecretSource>(&result);
}

const secure_frontend::SecretSourceError* error_from(
    const secure_frontend::SecretSourceValidation& result) {
  return std::get_if<secure_frontend::SecretSourceError>(&result);
}

void accepts_environment_variable_name_without_retaining_a_secret() {
  const auto result = secure_frontend::make_environment_secret_source("STRATA_API_KEY");
  const auto* source = source_from(result);
  expect(source != nullptr, "a conventional environment variable name should be accepted");
  if (source != nullptr) {
    expect(source->kind() == secure_frontend::SecretSourceKind::environment_variable,
           "the source should identify an environment variable");
    expect(source->location() == "STRATA_API_KEY", "the source should retain only its name");
  }
}

void accepts_protected_file_path_metadata() {
  const auto result = secure_frontend::make_protected_file_secret_source("secrets/api-key");
  const auto* source = source_from(result);
  expect(source != nullptr, "a relative protected-file path should be accepted as metadata");
  if (source != nullptr) {
    expect(source->kind() == secure_frontend::SecretSourceKind::protected_file,
           "the source should identify a protected file");
    expect(source->location() == "secrets/api-key",
           "the source should retain only its path metadata");
  }
}

void rejects_unsafe_environment_variable_names() {
  for (const auto name : {"", " ", "STRATA-API-KEY", "9STRATA_KEY", "STRATA KEY", "STRATA\tKEY",
                          "STRATA\nKEY"}) {
    const auto result = secure_frontend::make_environment_secret_source(name);
    const auto* error = error_from(result);
    expect(error != nullptr && error->code == secure_frontend::SecretSourceErrorCode::invalid_environment_variable_name,
           "unsafe environment variable name should be rejected: " + std::string{name});
  }
}

void rejects_blank_or_control_containing_file_paths() {
  for (const auto path : {"", " \t", "/var/lib/strata/key\nnext", "secrets/api\rkey",
                          "secrets/api\tkey"}) {
    const auto result = secure_frontend::make_protected_file_secret_source(path);
    const auto* error = error_from(result);
    expect(error != nullptr && error->code == secure_frontend::SecretSourceErrorCode::invalid_protected_file_path,
           "unsafe protected-file path should be rejected");
  }
}

void rejects_paths_that_escape_or_trigger_platform_special_handling() {
  for (const auto path : {"../token", "secrets/../token", "/etc/token", "\\\\server\\share\\token",
                          "\\\\?\\C:\\token", "C:\\token", "token:alternate", "NUL", "con.txt",
                          "COM1", "LPT9.log", "./token", "folder//token"}) {
    const auto result = secure_frontend::make_protected_file_secret_source(path);
    const auto* error = error_from(result);
    expect(error != nullptr && error->code == secure_frontend::SecretSourceErrorCode::invalid_protected_file_path,
           "unsafe protected-file path should be rejected: " + std::string{path});
  }
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"accepts_environment_variable_name_without_retaining_a_secret", accepts_environment_variable_name_without_retaining_a_secret},
      {"accepts_protected_file_path_metadata", accepts_protected_file_path_metadata},
      {"rejects_unsafe_environment_variable_names", rejects_unsafe_environment_variable_names},
      {"rejects_blank_or_control_containing_file_paths", rejects_blank_or_control_containing_file_paths},
      {"rejects_paths_that_escape_or_trigger_platform_special_handling", rejects_paths_that_escape_or_trigger_platform_special_handling},
  }, failures);
}
