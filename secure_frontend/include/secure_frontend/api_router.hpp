#pragma once

#include "secure_frontend/http_policy.hpp"
#include "secure_frontend/request_router.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// One SP2 snapshot rendered as the /health body. engine/recovery are SP2's
// fixed literals (health_for); recovery empty = absent. Any other value, or
// recovery with a healthy engine or a state other than "failed", fails closed
// (503 engine_unavailable) so the body never echoes dynamic text.
struct HealthView final {
  bool healthy;
  std::string_view engine;
  std::string_view recovery;
};
// Called exactly once per authenticated GET /health.
using HealthProvider = std::function<HealthView()>;
// listener.monitor (session log step 16): the Monitor's JSON for one authenticated GET /metrics.
using MetricsProvider = std::function<std::string()>;
// A chat handler returns a complete response or (M2) a StreamHandler.
using ChatHandler = std::function<RouteResult(std::string_view body, const RequestContext& context)>;

// Model launcher (plan 2026-10-23 Task 4; spec §5.4): with model cards, the served model is the
// running card's. `id` empty: none runs (/v1/models lists nothing); `extension` is the
// strata_models array (each card's id and state), added to /v1/models as is.
struct ServedModels final {
  std::optional<std::string> id{};
  std::vector<std::string> aliases{};
  std::string extension{};
};
using ServedModelsProvider = std::function<ServedModels()>;
// The control routes' handlers (kModelControlRoutes); all empty: the routes are not served (404).
struct ModelControl final {
  std::function<RouteResult(const RequestContext& context)> list{};
  std::function<RouteResult(std::string_view body, const RequestContext& context)> start{};
  std::function<RouteResult(const RequestContext& context)> stop{};
  // GUI-first start-up (spec 2026-10-27 §5.3): each optional; unset, its route is not served.
  std::function<RouteResult(std::string_view body, const RequestContext& context)> calibrate{};
  std::function<RouteResult(std::string_view body, const RequestContext& context)> cancel_calibration{};
  std::function<RouteResult(std::string_view body, const RequestContext& context)> auto_recalibrate{};
};

// The download routes' handler (kModelDownloadRoutes); empty: the routes are not served (404).
struct ModelDownloads final {
  std::function<RouteResult(RouteId route, std::string_view body, const RequestContext& context)> handle{};
};

// The page's card and folder routes' handler (kModelCardRoutes); empty: the routes are not served (404).
struct ModelCardRoutes final {
  std::function<RouteResult(RouteId route, std::string_view body, const RequestContext& context)> handle{};
};

struct ApiRouterConfig final {
  HttpPolicy policy;
  BearerTokenValidator validator;
  std::string model_id;   // the one served id; /v1/models lists only it (spec §3.6, E9)
  std::string owned_by;
  // SP5 M1 Task 8 (model.aliases; runconfig.py:34 "lists and answers to"): /v1/models
  // lists the served id and these aliases, and GET /v1/models/<alias> finds the model.
  std::vector<std::string> aliases{};
  std::size_t chat_body_limit;
  HealthProvider health;
  ChatHandler chat;
  // listener.web_ui: serve the built-in page's bundled files (web_ui.hpp) at their exact
  // GET targets without the key. Nothing else changes: every kRouteTable route still needs it.
  bool web_ui{false};
  // false only for listener.loopback_without_key (a loopback bind, validated at load and
  // again at bind): routes answer without the key. Host and Origin checks still apply.
  bool require_key{true};
  // listener.monitor: GET /metrics answers this provider's JSON; empty (the switch off), 404.
  MetricsProvider metrics{};
  // The page's About (owner 2026-10-27): GET /v1/strata/about answers this provider's JSON (about_info.hpp); empty, 404.
  MetricsProvider about{};
  // Model launcher: set, /v1/models and /v1/models/<id> answer from it instead of model_id/aliases.
  ServedModelsProvider served{};
  ModelControl control{};
  ModelDownloads downloads{};
  ModelCardRoutes cards{};
  ModelCardRoutes alerts{};  // kAlertRoutes (a build with email alerts only); empty: not served
  ModelCardRoutes search{};  // kSearchRoutes (a build with web search only); empty: not served
  // GUI-first start-up (spec 2026-10-27 §3, D1; listener.login_link): set, POST /v1/strata/login-link answers without
  // the key (it hands the key out once, to a loopback client with the one-time code; the handler checks both).
  ChatHandler login_link{};
};

// application/json, optionally followed by `; charset=utf-8`; ASCII
// case-insensitive, optional whitespace around each part (spec §4.2). Never allocates.
[[nodiscard]] bool is_json_content_type(std::string_view value) noexcept;

// The header-first router (spec §4.1 steps 3-7): Host, Origin, preflight,
// bearer token, kRouteTable, then the chat route's Content-Type and body
// limit. Nothing about routes, models or the engine is revealed before the
// token is accepted. Every response after a CORS-approved origin carries
// Access-Control-Allow-Origin and Vary: Origin, a 401 included.
class ApiRouter final : public RequestRouter {
 public:
  explicit ApiRouter(ApiRouterConfig config);
  [[nodiscard]] RouteDecision authorize(const RequestHead& head) override;
  [[nodiscard]] RouteResult handle(const RequestHead& head, std::string_view body, const RouteDecision& decision,
                                   const RequestContext& context) override;

 private:
  ApiRouterConfig config_;
};

}  // namespace secure_frontend
