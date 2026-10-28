#include "secure_frontend/alert_routes.hpp"

#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/config_writer.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/token_store.hpp"

#include <algorithm>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <unistd.h>
#endif

namespace secure_frontend {
namespace {

constexpr JsonLimits kBodyLimits{.max_bytes = kModelControlBodyLimit, .max_depth = 3U, .max_container_entries = 16U,
                                 .max_string_bytes = 512U};

std::string json_text(const std::string_view text) {
  std::string out;
  append_json_string(&out, text);
  return out;
}

std::string json_list(const std::vector<std::string>& items) {
  std::string out = "[";
  for (std::size_t i = 0; i < items.size(); ++i) out += (i == 0U ? "" : ",") + json_text(items[i]);
  return out + "]";
}

HttpResponse json_answer(const int status, std::string body) {
  return {.status = status, .headers = {{"Content-Type", "application/json; charset=utf-8"}, {"Cache-Control", "no-store"}},
          .body = std::move(body)};
}

ConfigDraftEntry file_entry(ConfigDraftValue value) { return ConfigDraftEntry{std::move(value), ConfigSource::file}; }

}  // namespace

std::string server_display_name() {
  std::string name;
#if defined(_WIN32)
  wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1]{};
  DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
  if (GetComputerNameW(buffer, &size) != 0) {
    for (DWORD i = 0; i < size; ++i) name.push_back(buffer[i] > 0x20 && buffer[i] < 0x7f ? static_cast<char>(buffer[i]) : '?');
  }
#else
  char buffer[256]{};
  if (gethostname(buffer, sizeof(buffer) - 1U) == 0) {
    for (const char* c = buffer; *c != '\0'; ++c) name.push_back(*c > 0x20 && *c < 0x7f ? *c : '?');
  }
#endif
  if (name.size() > 64U) name.resize(64U);
  return name.empty() ? std::string{"this PC"} : name;
}

AlertRoutes::AlertRoutes(AlertRouteSettings settings) : settings_(std::move(settings)) {}

HttpResponse AlertRoutes::answer(const RouteId route, const std::string_view body, const RequestContext& context) {
  // the password and the mail server are the business of this PC and the clients the server's rule lets in
  const bool allowed = settings_.client_allowed ? settings_.client_allowed(context) : context.loopback_peer;
  if (!allowed) return make_error_response(ApiError::control_not_allowed);
  switch (route) {
    case RouteId::strata_alerts: return settings_json();
    case RouteId::strata_alerts_settings: return save(body);
    case RouteId::strata_alerts_password: return password(body);
    case RouteId::strata_alerts_test: return test();
    default: return make_error_response(ApiError::not_found);
  }
}

// The settings the config file holds now (what applies at the next start).
std::optional<EmailAlertsConfig> AlertRoutes::saved() const {
  if (!settings_.config_file.has_value()) return EmailAlertsConfig{};
  const auto draft = read_config_file(*settings_.config_file);
  if (!std::holds_alternative<FrontendConfigDraft>(draft)) return std::nullopt;
  const auto alerts = alerts_from_draft(std::get<FrontendConfigDraft>(draft));
  if (!std::holds_alternative<AlertsConfig>(alerts)) return std::nullopt;
  return std::get<AlertsConfig>(alerts).email;
}

HttpResponse AlertRoutes::settings_json() const {
  const auto email = saved();
  if (!email) return make_error_response(ApiError::settings_refused, {.reason = "the config file could not be read"});
  return json_answer(200, std::string{"{\"built\":true,\"editable\":"} + (settings_.config_file ? "true" : "false") +
                              ",\"enabled\":" + (email->enabled ? "true" : "false") + ",\"host\":" + json_text(email->host) +
                              ",\"port\":" + std::to_string(email->port) +
                              ",\"security\":" + (email->security == SmtpSecurity::ssl ? "\"ssl\"" : "\"starttls\"") +
                              ",\"username\":" + json_text(email->username) + ",\"from\":" + json_text(email->from) +
                              ",\"to\":" + json_list(email->to) + ",\"events\":" + json_list(email->events) +
                              ",\"password_set\":" + (read_smtp_password(settings_.secret_dir) ? "true" : "false") + "}\n");
}

HttpResponse AlertRoutes::save(const std::string_view body) {
  if (!settings_.config_file.has_value()) {
    return make_error_response(ApiError::settings_refused, {.reason = "this server was started without a config file to write"});
  }
  const auto parsed = parse_json(body, kBodyLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root == nullptr ? nullptr : root->if_object();
  if (members == nullptr) return make_error_response(ApiError::invalid_request);
  // The page's fields as the config's alerts.email.* rows; validated alone, then written over the file's.
  FrontendConfigDraft rows;
  for (const auto& member : *members) {
    const std::string row = "alerts.email." + member.key;
    if (const auto* flag = member.value.if_boolean(); flag != nullptr && member.key == "enabled") rows.entries[row] = file_entry(*flag);
    else if (const auto* number = member.value.if_integer(); number != nullptr && member.key == "port") rows.entries[row] = file_entry(*number);
    else if (const auto* text = member.value.if_string();
             text != nullptr && (member.key == "host" || member.key == "security" || member.key == "username" || member.key == "from")) {
      if (!text->empty()) rows.entries[row] = file_entry(*text);
    } else if (const auto* list = member.value.if_array(); list != nullptr && (member.key == "to" || member.key == "events")) {
      std::vector<std::string> values;
      for (const auto& item : *list) {
        if (item.if_string() == nullptr) return make_error_response(ApiError::invalid_request);
        values.push_back(*item.if_string());
      }
      rows.entries[row] = file_entry(std::move(values));
    } else {
      return make_error_response(ApiError::invalid_request);
    }
  }
  const auto checked = alerts_from_draft(rows);
  if (const auto* issues = std::get_if<ConfigErrors>(&checked); issues != nullptr && !issues->empty()) {
    const std::string reason = format_issue(issues->front());
    return make_error_response(ApiError::settings_refused, {.reason = reason});
  }
  const std::scoped_lock lock(file_);
  const auto failure = update_config_file(*settings_.config_file, [&rows](FrontendConfigDraft& draft) {
    std::erase_if(draft.entries, [](const auto& entry) { return entry.first.rfind("alerts.", 0) == 0U; });
    for (auto& [row, entry] : rows.entries) draft.entries[row] = std::move(entry);
  });
  if (failure) return make_error_response(ApiError::settings_refused, {.reason = config_update_reason(*failure)});
  if (settings_.log) settings_.log("[strata] alerts: the email alert settings were saved (they apply at the next start)");
  return json_answer(200, "{\"saved\":true,\"restart_needed\":true}\n");
}

HttpResponse AlertRoutes::password(const std::string_view body) {
  const auto parsed = parse_json(body, kBodyLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root == nullptr ? nullptr : root->if_object();
  if (members == nullptr || members->size() != 1U || members->front().key != "password" || members->front().value.if_string() == nullptr) {
    return make_error_response(ApiError::invalid_request);
  }
  if (const auto failure = store_smtp_password(settings_.secret_dir, *members->front().value.if_string())) {
    return make_error_response(ApiError::settings_refused, {.reason = *failure});
  }
  if (settings_.log) settings_.log("[strata] alerts: the SMTP password was set");
  return json_answer(200, "{\"password_set\":true}\n");
}

HttpResponse AlertRoutes::test() {
  const auto email = saved();
  if (!email) return make_error_response(ApiError::settings_refused, {.reason = "the config file could not be read"});
  AlertSettings alert_settings{.config = *email, .secret_dir = settings_.secret_dir, .server_name = settings_.server_name};
  if (settings_.connect) {
    const auto connect = settings_.connect;
    const EmailAlertsConfig config = *email;
    alert_settings.connect = [connect, config](std::string* error) { return connect(config, error); };
  }
  alert_settings.log = settings_.log;
  EmailAlerts alerts(std::move(alert_settings));
  const auto result = alerts.send_test();
  if (settings_.log) settings_.log("[strata] alerts: a test email " + std::string{result.sent ? "was sent" : "was not sent (" + result.stage + ")"});
  return json_answer(200, std::string{"{\"sent\":"} + (result.sent ? "true" : "false") + ",\"stage\":" + json_text(result.stage) +
                              ",\"code\":" + std::to_string(result.code) + ",\"reply\":" + json_text(result.reply) + "}\n");
}

}  // namespace secure_frontend
