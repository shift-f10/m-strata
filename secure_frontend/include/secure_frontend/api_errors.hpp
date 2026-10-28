#pragma once

#include "secure_frontend/http_request_handler.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace secure_frontend {

// Spec §7.2. Every client-visible error is one of these rows; the message is
// a compile-time literal and never contains request, config or engine data.
enum class ApiError {
  invalid_request, invalid_json, unsupported_parameter, extension_disabled, context_length_exceeded,
  invalid_api_key, forbidden, not_found, model_not_found, method_not_allowed, request_timeout, length_required,
  request_too_large, request_line_too_long, unsupported_media_type, expectation_failed, header_too_large,
  internal_error, server_busy, engine_unavailable, engine_error, server_shutting_down, http_version_not_supported,
  // Model launcher (plan 2026-10-23 Tasks 3-4; spec §5.3-§5.4, D2/D6).
  invalid_option, control_not_allowed, model_not_running, switch_in_progress, model_switching, no_model_running,
  // Model downloads (plan 2026-10-23-model-downloads Task 4).
  download_in_progress, upstream_unavailable, insufficient_storage, download_not_active,
  // GUI-first start-up (spec 2026-10-27 §3, §4): the login link, and the page's cards and folders.
  login_link_refused, folder_refused, folder_not_private, card_refused, card_exists, card_read_only, card_in_use,
  delete_refused,
  // (spec 2026-10-27 §5) calibration.
  calibration_unsupported, not_calibrating,
  // (spec 2026-10-27 §5a) the page's settings.
  settings_refused,
};

struct ApiErrorRow final {
  ApiError error;
  int status;
  std::string_view type;
  std::string_view code;
  std::string_view message;
};

[[nodiscard]] std::span<const ApiErrorRow> api_error_table() noexcept;
[[nodiscard]] const ApiErrorRow& api_error_row(ApiError error) noexcept;
// The fixed key lists of spec §3.1 (unsupported_parameter and extension_disabled).
[[nodiscard]] bool is_fixed_param_key(std::string_view key) noexcept;

struct ErrorOptions final {
  std::string_view param{};             // unsupported_parameter / extension_disabled: a fixed key; invalid_option: an option name
  std::optional<int> retry_after{};     // engine_unavailable: 5 or none (spec §3.8)
  std::string_view allow{};             // method_not_allowed
  // card_refused / folder_refused / settings_refused: the module's own fixed reason (CardError, FolderError, a config
  // issue: never a path or a secret), in place of the row's message. Ignored for every other row.
  std::string_view reason{};
};

// {"error":{"message":"<fixed>","type":"<type>","param":null|"<key>","code":"<code>"}}\n
[[nodiscard]] std::string render_error_body(ApiError error, std::string_view param = {}, std::string_view reason = {});
[[nodiscard]] HttpResponse make_error_response(ApiError error, const ErrorOptions& options = {});
// Spec §3.8: engine_unavailable carries Retry-After 5 unless recovery=exhausted.
[[nodiscard]] std::optional<int> engine_retry_after(std::string_view recovery) noexcept;

}  // namespace secure_frontend
