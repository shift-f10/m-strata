#pragma once

// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §5a): the page's email alert
// settings (kAlertRoutes) - built only with SECURE_FRONTEND_EMAIL_ALERTS. For loopback clients and the clients
// AlertRouteSettings::client_allowed lets in (the server: the listed hosts and subnets that may manage model files;
// owner 2026-10-27); the password is written, never shown. Each body a small JSON object:
//
//   POST /v1/strata/alerts           {}  -> {"built": true, "editable", "enabled", "host", "port", "security",
//                                            "username", "from", "to", "events", "password_set"}   (the config file's)
//   POST /v1/strata/alerts/settings  {"enabled","host","port","security","username","from","to","events"}
//                                    -> {"saved": true, "restart_needed": true}   (validated, written to the config
//                                       file; the running server keeps its settings until it restarts)
//   POST /v1/strata/alerts/password  {"password"} -> {"password_set": true}   (roots.secret, never shown again)
//   POST /v1/strata/alerts/test      {}  -> {"sent", "stage", "code", "reply"}   (through the saved settings, now)

#include "secure_frontend/api_router.hpp"
#include "secure_frontend/email_alerts.hpp"
#include "secure_frontend/request_router.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

struct AlertRouteSettings final {
  std::optional<std::filesystem::path> config_file;  // where the settings live (unset: read-only defaults)
  std::filesystem::path secret_dir;                  // roots.secret: smtp-password
  std::string server_name;                           // what the mails call this server
  // One connection with these settings (production: connect_smtp); a test's scripted server.
  std::function<std::unique_ptr<SmtpTransport>(const EmailAlertsConfig&, std::string*)> connect{};
  std::function<void(std::string_view)> log{};
  // Who may use these routes (the server passes model_files_allowed over models.control_remote_networks, as for the
  // model-file routes; owner 2026-10-27). Empty: loopback clients only.
  std::function<bool(const RequestContext& context)> client_allowed{};
};

class AlertRoutes final {
 public:
  explicit AlertRoutes(AlertRouteSettings settings);
  [[nodiscard]] HttpResponse answer(RouteId route, std::string_view body, const RequestContext& context);

 private:
  [[nodiscard]] std::optional<EmailAlertsConfig> saved() const;
  HttpResponse settings_json() const;
  HttpResponse save(std::string_view body);
  HttpResponse password(std::string_view body);
  HttpResponse test();

  AlertRouteSettings settings_;
  std::mutex file_;  // one config write at a time
};

// This PC's name as the alerts call it (printable ASCII, at most 64; "this PC" when unknown).
[[nodiscard]] std::string server_display_name();

}  // namespace secure_frontend
