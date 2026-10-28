#include "secure_frontend/api_router.hpp"

#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/http_request_head.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/web_ui.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

HttpResponse json_response(int status, std::string body) {
  return {.status = status, .headers = {{"Content-Type", "application/json; charset=utf-8"}}, .body = std::move(body)};
}

// Spec §3.9: copied unchanged only after validation — 1..kMaxModelFieldBytes
// bytes of valid UTF-8 with no C0 control or DEL.
bool valid_field(std::string_view value) {
  return !value.empty() && value.size() <= api_constants::kMaxModelFieldBytes && is_valid_utf8(value) &&
         std::none_of(value.begin(), value.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20U || c == 0x7f; });
}

// SP2's fixed /health literals (health_for); recovery only with a failed, unhealthy engine.
// Model launcher: "none" (no model card runs) is a healthy server and nothing else.
bool valid_health(const HealthView& view) {
  const std::string_view e = view.engine;
  if (e == "none") return view.healthy && view.recovery.empty();
  if (e != "starting" && e != "ready" && e != "generating" && e != "failed") return false;
  if (view.recovery.empty()) return true;
  return !view.healthy && e == "failed" && (view.recovery == "restarting" || view.recovery == "exhausted");
}

std::string model_object(std::string_view id, std::string_view owned_by) {
  std::string body{"{\"id\":"};
  append_json_string(&body, id);
  body.append(",\"object\":\"model\",\"created\":0,\"owned_by\":");
  append_json_string(&body, owned_by);
  body.push_back('}');
  return body;
}

void add_cors(HttpResponse* response, const std::optional<std::string>& allow_origin) {
  if (!allow_origin.has_value()) return;
  response->headers.emplace_back("Access-Control-Allow-Origin", *allow_origin);
  response->headers.emplace_back("Vary", "Origin");
}

}  // namespace

bool is_json_content_type(const std::string_view value) noexcept {
  constexpr std::string_view kType = "application/json";
  const std::string_view text = trim_ows(value);
  if (text.size() < kType.size() || !ascii_iequal(text.substr(0, kType.size()), kType)) return false;
  const std::string_view rest = trim_ows(text.substr(kType.size()));
  if (rest.empty()) return true;
  if (rest.front() != ';') return false;
  return ascii_iequal(trim_ows(rest.substr(1U)), "charset=utf-8");
}

ApiRouter::ApiRouter(ApiRouterConfig config) : config_(std::move(config)) {}

RouteDecision ApiRouter::authorize(const RequestHead& head) {
  RouteDecision decision;
  const auto early = [&decision](HttpResponse response) {
    add_cors(&response, decision.allow_origin);
    decision.early = std::move(response);
    return decision;
  };
  // Step 3: Host and Origin policy.
  const PolicyVerdict host = check_host(config_.policy, head.values("host"));
  if (host != PolicyVerdict::allowed) {
    return early(make_error_response(host == PolicyVerdict::malformed ? ApiError::invalid_request : ApiError::forbidden));
  }
  const OriginDecision origin = check_origin(config_.policy, head.values("origin"));
  if (origin.verdict != PolicyVerdict::allowed) return early(make_error_response(ApiError::forbidden));
  // Step 4: CORS preflight, before authentication; it carries its own CORS headers.
  if (auto preflight = evaluate_preflight(config_.policy, head)) {
    decision.early = std::move(*preflight);
    return decision;
  }
  decision.allow_origin = origin.allow_origin;

  // The built-in page (listener.web_ui): its bundled files are public — they hold no secret
  // and reach no route. Exact GET targets only, after Host and Origin; anything else goes on
  // to the key check like every API request.
  if (config_.web_ui && head.method == "GET" && find_web_asset(head.target) != nullptr) {
    decision.route = RouteId::web;
    decision.expect_continue = head.expect_continue;
    decision.body_limit = head.content_length.value_or(0U);
    return decision;
  }

  // GUI-first start-up (spec 2026-10-27 §3, D1): the one-time login link is the one API route that answers without the
  // key - it is how a browser gets the key - and only while listener.login_link set its handler. JSON, small.
  if (config_.login_link && head.target == kLoginLinkPath) {
    if (head.method != "POST") return early(make_error_response(ApiError::method_not_allowed, {.allow = "POST"}));
    const auto types = head.values("content-type");
    if (types.size() != 1U || !is_json_content_type(types.front())) return early(make_error_response(ApiError::unsupported_media_type));
    if (head.content_length.value_or(0U) > kLoginLinkBodyLimit) return early(make_error_response(ApiError::request_too_large));
    decision.route = RouteId::strata_login_link;
    decision.expect_continue = head.expect_continue;
    decision.body_limit = head.content_length.value_or(0U);
    return decision;
  }

  // Step 5: exactly one `Bearer <non-empty>` the validator accepts — unless this is the
  // keyless loopback listener (listener.loopback_without_key; require_key false).
  if (config_.require_key) {
    const auto authorizations = head.values("authorization");
    if (authorizations.size() > 1U) return early(make_error_response(ApiError::invalid_request));
    constexpr std::string_view kBearer = "Bearer ";
    bool authorized = false;
    if (authorizations.size() == 1U && authorizations.front().size() > kBearer.size() &&
        authorizations.front().substr(0, kBearer.size()) == kBearer) {
      try {
        authorized = config_.validator && config_.validator(authorizations.front().substr(kBearer.size()));
      } catch (...) {
        authorized = false;
      }
    }
    if (!authorized) return early(make_error_response(ApiError::invalid_api_key));
  }

  // Step 6: kRouteTable, byte-exact; a prefix route needs a non-empty rest. The model
  // launcher's control routes join it only while their handlers are set (models.control).
  const RouteEntry* matched = nullptr;
  std::string_view allow;
  const std::string_view target = head.target;
  const bool control = config_.control.list && config_.control.start && config_.control.stop;
  if (control) {
    for (const auto& entry : kModelControlRoutes) {
      if (target != entry.path) continue;
      // the calibration routes only while their handlers are set
      if ((entry.id == RouteId::strata_calibrate && !config_.control.calibrate) ||
          (entry.id == RouteId::strata_calibrate_cancel && !config_.control.cancel_calibration) ||
          (entry.id == RouteId::strata_calibrate_auto && !config_.control.auto_recalibrate)) {
        continue;
      }
      allow = entry.method;
      if (head.method == entry.method) matched = &entry;
    }
  }
  if (config_.downloads.handle) {
    for (const auto& entry : kModelDownloadRoutes) {
      if (target != entry.path) continue;
      allow = entry.method;
      if (head.method == entry.method) matched = &entry;
    }
  }
  if (config_.cards.handle) {
    for (const auto& entry : kModelCardRoutes) {
      if (target != entry.path) continue;
      allow = entry.method;
      if (head.method == entry.method) matched = &entry;
    }
  }
  if (config_.alerts.handle) {
    for (const auto& entry : kAlertRoutes) {
      if (target != entry.path) continue;
      allow = entry.method;
      if (head.method == entry.method) matched = &entry;
    }
  }
  if (config_.search.handle) {
    for (const auto& entry : kSearchRoutes) {
      if (target != entry.path) continue;
      allow = entry.method;
      if (head.method == entry.method) matched = &entry;
    }
  }
  for (const auto& entry : kRouteTable) {
    // listener.monitor off: /metrics is no route at all, for any method (EXT-01: upstream's
    // /metrics stays 404 after authentication, never 405).
    if (entry.id == RouteId::metrics && !config_.metrics) continue;
    if (entry.id == RouteId::strata_about && !config_.about) continue;
    const bool path_match = entry.prefix
                                ? (target.size() > entry.path.size() && target.substr(0, entry.path.size()) == entry.path)
                                : target == entry.path;
    if (!path_match) continue;
    allow = entry.method;
    if (head.method == entry.method) matched = &entry;
  }
  if (matched == nullptr) {
    if (allow.empty()) return early(make_error_response(ApiError::not_found));
    return early(make_error_response(ApiError::method_not_allowed, {.allow = allow}));
  }
  decision.route = matched->id;
  // Step 7: the chat route's header checks.
  if (decision.route == RouteId::chat) {
    const auto types = head.values("content-type");
    if (types.size() != 1U || !is_json_content_type(types.front())) return early(make_error_response(ApiError::unsupported_media_type));
    if (head.content_length.value_or(0U) > config_.chat_body_limit) return early(make_error_response(ApiError::request_too_large));
  }
  // The control routes' bodies: JSON (a start names its card and options; a stop's is ignored), small.
  const bool download_post = decision.route == RouteId::strata_download_start || decision.route == RouteId::strata_download_pause ||
                             decision.route == RouteId::strata_download_cancel || decision.route == RouteId::strata_download_link;
  const bool card_post = std::any_of(kModelCardRoutes.begin(), kModelCardRoutes.end(),
                                     [&decision](const RouteEntry& entry) { return entry.id == decision.route; }) ||
                         std::any_of(kAlertRoutes.begin(), kAlertRoutes.end(),
                                     [&decision](const RouteEntry& entry) { return entry.id == decision.route; }) ||
                         std::any_of(kSearchRoutes.begin(), kSearchRoutes.end(),
                                     [&decision](const RouteEntry& entry) { return entry.id == decision.route; });
  const bool calibration_post = decision.route == RouteId::strata_calibrate || decision.route == RouteId::strata_calibrate_cancel ||
                                decision.route == RouteId::strata_calibrate_auto;
  if (decision.route == RouteId::strata_start || decision.route == RouteId::strata_stop || download_post || card_post ||
      calibration_post) {
    const auto types = head.values("content-type");
    if (head.content_length.value_or(0U) > 0U && (types.size() != 1U || !is_json_content_type(types.front()))) {
      return early(make_error_response(ApiError::unsupported_media_type));
    }
    if (head.content_length.value_or(0U) > kModelControlBodyLimit) return early(make_error_response(ApiError::request_too_large));
  }
  decision.expect_continue = head.expect_continue;
  decision.body_limit = head.content_length.value_or(0U);
  return decision;
}

RouteResult ApiRouter::handle(const RequestHead& head, const std::string_view body, const RouteDecision& decision,
                              const RequestContext& context) {
  HttpResponse response = make_error_response(ApiError::internal_error);
  try {
    switch (decision.route) {
      case RouteId::health: {
        if (!config_.health) {
          response = make_error_response(ApiError::engine_unavailable);
          break;
        }
        const HealthView view = config_.health();   // exactly once per request
        if (!valid_health(view)) {
          response = make_error_response(ApiError::engine_unavailable);
          break;
        }
        std::string text{view.healthy ? "{\"status\":\"ok\",\"engine\":" : "{\"status\":\"unavailable\",\"engine\":"};
        append_json_string(&text, view.engine);
        if (!view.recovery.empty()) {
          text.append(",\"recovery\":");
          append_json_string(&text, view.recovery);
        }
        text.append("}\n");
        response = json_response(view.healthy ? 200 : 503, std::move(text));
        break;
      }
      case RouteId::models:
        if (config_.served) {
          // Model launcher: the running card (none: an empty list) and every card's state.
          const ServedModels served = config_.served();
          if (!valid_field(config_.owned_by)) break;
          std::string data;
          if (served.id.has_value()) {
            if (!valid_field(*served.id) ||
                !std::all_of(served.aliases.begin(), served.aliases.end(), [](const std::string& alias) { return valid_field(alias); }))
              break;
            data = model_object(*served.id, config_.owned_by);
            for (const auto& alias : served.aliases) data += "," + model_object(alias, config_.owned_by);
          }
          std::string text = "{\"object\":\"list\",\"data\":[" + std::move(data) + "]";
          if (!served.extension.empty()) text += ",\"strata_models\":" + served.extension;
          response = json_response(200, std::move(text) + "}\n");
          break;
        }
        if (!valid_field(config_.model_id) || !valid_field(config_.owned_by)) break;   // internal_error
        if (!std::all_of(config_.aliases.begin(), config_.aliases.end(), [](const std::string& alias) { return valid_field(alias); }))
          break;  // internal_error: an alias is schema-checked; a router fed a bad one is a bug
        {
          std::string data = model_object(config_.model_id, config_.owned_by);
          for (const auto& alias : config_.aliases) data += "," + model_object(alias, config_.owned_by);
          response = json_response(200, "{\"object\":\"list\",\"data\":[" + std::move(data) + "]}\n");
        }
        break;
      case RouteId::model: {
        constexpr std::string_view kPrefix = "/v1/models/";
        const std::string_view target = head.target;
        if (config_.served) {
          const ServedModels served = config_.served();
          if (!valid_field(config_.owned_by) || target.size() <= kPrefix.size()) break;
          const std::string_view wanted = target.substr(kPrefix.size());
          const bool named = served.id.has_value() && valid_field(*served.id) &&
                             (wanted == *served.id ||
                              std::find(served.aliases.begin(), served.aliases.end(), std::string{wanted}) != served.aliases.end());
          response = named ? json_response(200, model_object(*served.id, config_.owned_by) + "\n")
                           : make_error_response(ApiError::model_not_found);
          break;
        }
        if (!valid_field(config_.model_id) || !valid_field(config_.owned_by) || target.size() <= kPrefix.size()) break;
        const std::string_view wanted = target.substr(kPrefix.size());
        // SP5 M1 Task 8: an alias names the same model.
        const bool named = wanted == config_.model_id ||
                           std::find(config_.aliases.begin(), config_.aliases.end(), std::string{wanted}) != config_.aliases.end();
        response = named ? json_response(200, model_object(config_.model_id, config_.owned_by) + "\n")
                         : make_error_response(ApiError::model_not_found);
        break;
      }
      case RouteId::strata_login_link: {
        RouteResult result = config_.login_link ? config_.login_link(body, context)
                                                : RouteResult{make_error_response(ApiError::not_found)};
        if (auto* complete = std::get_if<HttpResponse>(&result)) add_cors(complete, decision.allow_origin);
        return result;
      }
      case RouteId::chat: {
        RouteResult result =
            config_.chat ? config_.chat(body, context) : RouteResult{make_error_response(ApiError::engine_unavailable)};
        // A StreamHandler gets the CORS headers from deliver_route_result (Task 8b), which adds them to its head.
        if (auto* complete = std::get_if<HttpResponse>(&result)) add_cors(complete, decision.allow_origin);
        return result;
      }
      case RouteId::web: {
        const WebAsset* asset = find_web_asset(head.target);
        if (asset == nullptr) break;  // internal_error: authorize matched it
        response = HttpResponse{.status = 200,
                                .headers = {{"Content-Type", std::string{asset->content_type}},
                                            {"Cross-Origin-Opener-Policy", "same-origin"}},
                                .body = std::string{asset->body},
                                .content_security_policy = kWebUiContentSecurityPolicy};
        break;
      }
      case RouteId::metrics:
        // listener.monitor (session log step 16): keyed above like every route; off, not served.
        response = config_.metrics ? json_response(200, config_.metrics()) : make_error_response(ApiError::not_found);
        break;
      case RouteId::strata_about:
        // the page's About: keyed above like every route
        response = config_.about ? json_response(200, config_.about()) : make_error_response(ApiError::not_found);
        break;
      // Model launcher (plan 2026-10-23 Task 4): keyed above; the handlers answer their own refusals.
      case RouteId::strata_models:
      case RouteId::strata_start:
      case RouteId::strata_stop: {
        RouteResult result = decision.route == RouteId::strata_models ? config_.control.list(context)
                             : decision.route == RouteId::strata_start ? config_.control.start(body, context)
                                                                        : config_.control.stop(context);
        if (auto* complete = std::get_if<HttpResponse>(&result)) {
          response = std::move(*complete);
          break;
        }
        return result;
      }
      // GUI-first start-up (spec 2026-10-27 §5.3): keyed above; served only while set.
      case RouteId::strata_calibrate:
      case RouteId::strata_calibrate_cancel:
      case RouteId::strata_calibrate_auto: {
        const auto& handler = decision.route == RouteId::strata_calibrate          ? config_.control.calibrate
                              : decision.route == RouteId::strata_calibrate_cancel ? config_.control.cancel_calibration
                                                                                   : config_.control.auto_recalibrate;
        RouteResult result = handler(body, context);
        if (auto* complete = std::get_if<HttpResponse>(&result)) {
          response = std::move(*complete);
          break;
        }
        return result;
      }
      // Model downloads (plan 2026-10-23-model-downloads Task 4): keyed above; the handler
      // answers its own refusals (403 for a remote client without models.control_remote).
      case RouteId::strata_downloads:
      case RouteId::strata_download_start:
      case RouteId::strata_download_pause:
      case RouteId::strata_download_cancel:
      case RouteId::strata_download_link: {
        RouteResult result = config_.downloads.handle(decision.route, body, context);
        if (auto* complete = std::get_if<HttpResponse>(&result)) {
          response = std::move(*complete);
          break;
        }
        return result;
      }
      // GUI-first start-up (spec 2026-10-27 §4): keyed above; the handler answers its own refusals.
      case RouteId::strata_folders:
      case RouteId::strata_folder_private:
      case RouteId::strata_folder_new:
      case RouteId::strata_folder_sharing:
      case RouteId::strata_card_add:
      case RouteId::strata_card_folder:
      case RouteId::strata_card_delete_begin:
      case RouteId::strata_card_delete: {
        RouteResult result = config_.cards.handle(decision.route, body, context);
        if (auto* complete = std::get_if<HttpResponse>(&result)) {
          response = std::move(*complete);
          break;
        }
        return result;
      }
      case RouteId::strata_alerts:
      case RouteId::strata_alerts_settings:
      case RouteId::strata_alerts_password:
      case RouteId::strata_alerts_test: {
        RouteResult result = config_.alerts.handle(decision.route, body, context);
        if (auto* complete = std::get_if<HttpResponse>(&result)) {
          response = std::move(*complete);
          break;
        }
        return result;
      }
      case RouteId::strata_search:
      case RouteId::strata_search_settings:
      case RouteId::strata_search_test: {
        RouteResult result = config_.search.handle(decision.route, body, context);
        if (auto* complete = std::get_if<HttpResponse>(&result)) {
          response = std::move(*complete);
          break;
        }
        return result;
      }
      case RouteId::other:
        response = make_error_response(ApiError::not_found);
        break;
    }
  } catch (...) {
    response = make_error_response(decision.route == RouteId::health ? ApiError::engine_unavailable : ApiError::internal_error);
  }
  add_cors(&response, decision.allow_origin);
  return response;
}

}  // namespace secure_frontend
