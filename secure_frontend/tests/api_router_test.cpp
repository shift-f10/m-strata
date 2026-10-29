#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/api_router.hpp"
#include "secure_frontend/config.hpp"
#include "secure_frontend/http_policy.hpp"
#include "secure_frontend/request_router.hpp"
#include "secure_frontend/secret_bytes.hpp"
#include "secure_frontend/token_verifier.hpp"
#include "secure_frontend/web_ui.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

constexpr std::string_view kRouterToken = "router-token-0123456789abcdefghijklmnopq";
constexpr secure_frontend::HeadLimits kRouterLimits{4096, 16384, 64, 65536};

secure_frontend::ApiRouter router_with(secure_frontend::HealthProvider health, std::string model = "fake-model",
                                       std::string owned_by = "local") {
  secure_frontend::SecretBytes secret;
  (void)secret.assign(kRouterToken);
  return secure_frontend::ApiRouter({
      .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address("127.0.0.1"), 8080, {}, {}),
      .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
      .model_id = std::move(model), .owned_by = std::move(owned_by), .chat_body_limit = 49152,
      .health = health ? std::move(health) : secure_frontend::HealthProvider{[] { return secure_frontend::HealthView{true, "ready", ""}; }},
      .chat = {}});
}
secure_frontend::RequestHead head_of(std::string method, std::string target) {
  secure_frontend::RequestHead head;
  head.method = std::move(method);
  head.target = std::move(target);
  head.fields = {{"Host", "127.0.0.1:8080"}, {"Authorization", "Bearer " + std::string{kRouterToken}}};
  return head;
}
secure_frontend::RouteDecision decision_of(secure_frontend::ApiRouter& router, std::string method, std::string target) {
  return router.authorize(head_of(std::move(method), std::move(target)));
}
secure_frontend::HttpResponse complete(secure_frontend::RouteResult result) {
  return std::get<secure_frontend::HttpResponse>(std::move(result));
}
secure_frontend::HttpResponse get(secure_frontend::ApiRouter& router, const std::string& target) {
  const auto decision = decision_of(router, "GET", target);
  if (decision.early) return *decision.early;
  return complete(router.handle(head_of("GET", target), "", decision, {}));
}

// health_samples_status_before_state_once_each: the provider is called exactly once per /health.
void health_samples_status_before_state_once_each() {
  int calls = 0;
  auto router = router_with([&calls] { ++calls; return secure_frontend::HealthView{true, "ready", ""}; });
  (void)router.handle(head_of("GET", "/health"), "", decision_of(router, "GET", "/health"), {});
  expect(calls == 1, "one snapshot per /health request");
  // SP2 (spec §3.4): READY is consumed eagerly, so `starting` is reported 503.
  auto starting = router_with([] { return secure_frontend::HealthView{false, "starting", ""}; });
  const auto r = get(starting, "/health");
  expect(r.status == 503 && r.body == "{\"status\":\"unavailable\",\"engine\":\"starting\"}\n", "a starting engine: " + r.body);
}

// Pre-flight T6b(a)/V11: recovery is reported only with an unhealthy engine.
void health_carries_recovery_only_with_failed() {
  auto restarting = router_with([] { return secure_frontend::HealthView{false, "failed", "restarting"}; });
  const auto r = get(restarting, "/health");
  expect(r.status == 503 && r.body == "{\"status\":\"unavailable\",\"engine\":\"failed\",\"recovery\":\"restarting\"}\n",
         "failed + restarting: " + r.body);
  auto exhausted = router_with([] { return secure_frontend::HealthView{false, "failed", "exhausted"}; });
  expect(get(exhausted, "/health").body == "{\"status\":\"unavailable\",\"engine\":\"failed\",\"recovery\":\"exhausted\"}\n",
         "failed + exhausted");
  auto ready = router_with([] { return secure_frontend::HealthView{true, "ready", ""}; });
  const auto healthy = get(ready, "/health");
  expect(healthy.status == 200 && healthy.body == "{\"status\":\"ok\",\"engine\":\"ready\"}\n", "ready carries no recovery field");
  const std::string fail_closed = secure_frontend::render_error_body(secure_frontend::ApiError::engine_unavailable);
  auto mixed = router_with([] { return secure_frontend::HealthView{true, "ready", "restarting"}; });
  const auto m = get(mixed, "/health");
  expect(m.status == 503 && m.body == fail_closed, "recovery with a healthy engine fails closed: " + m.body);
  auto unknown = router_with([] { return secure_frontend::HealthView{false, "failed", "/secret"}; });
  const auto u = get(unknown, "/health");
  expect(u.status == 503 && u.body == fail_closed && u.body.find("secret") == std::string::npos,
         "an unknown recovery literal is never echoed");
}
void health_includes_engine_state_when_provided() {
  auto router = router_with([] { return secure_frontend::HealthView{true, "generating", ""}; });
  const auto r = complete(router.handle(head_of("GET", "/health"), "", decision_of(router, "GET", "/health"), {}));
  expect(r.status == 200 && r.body == "{\"status\":\"ok\",\"engine\":\"generating\"}\n", "SP2 body unchanged: " + r.body);
  expect(r.header_value("Content-Type") == std::optional<std::string_view>{"application/json; charset=utf-8"}, "JSON content type");
}
void health_is_503_with_engine_state_when_unavailable() {
  auto router = router_with([] { return secure_frontend::HealthView{false, "failed", "restarting"}; });
  const auto r = complete(router.handle(head_of("GET", "/health"), "", decision_of(router, "GET", "/health"), {}));
  expect(r.status == 503 && r.body == "{\"status\":\"unavailable\",\"engine\":\"failed\",\"recovery\":\"restarting\"}\n", "503 body: " + r.body);
  auto bogus = router_with([] { return secure_frontend::HealthView{true, "/secret/path\"x", ""}; });
  const auto b = get(bogus, "/health");
  expect(b.status == 503 && b.body == secure_frontend::render_error_body(secure_frontend::ApiError::engine_unavailable),
         "an engine state outside the fixed literals is never echoed: " + b.body);
}
void dispatches_exact_health_route() {
  auto router = router_with([] { return secure_frontend::HealthView{true, "ready", ""}; });
  expect(decision_of(router, "GET", "/health").route == secure_frontend::RouteId::health && !decision_of(router, "GET", "/health").early,
         "/health is routed");
}
void dispatches_models_with_json_escaping() {
  auto router = router_with({}, "quote\"model", "own\\er");
  const auto r = complete(router.handle(head_of("GET", "/v1/models"), "", decision_of(router, "GET", "/v1/models"), {}));
  expect(r.status == 200 && r.body == "{\"object\":\"list\",\"data\":[{\"id\":\"quote\\\"model\",\"object\":\"model\",\"created\":0,\"owned_by\":\"own\\\\er\"}]}\n",
         "only the served model, escaped: " + r.body);
  const auto model = complete(router.handle(head_of("GET", "/v1/models/other"), "", decision_of(router, "GET", "/v1/models/other"), {}));
  expect(model.status == 404 && model.body == secure_frontend::render_error_body(secure_frontend::ApiError::model_not_found),
         "other model id: 404 model_not_found");
  auto plain = router_with({});
  const auto same = get(plain, "/v1/models/fake-model");
  expect(same.status == 200 && same.body == "{\"id\":\"fake-model\",\"object\":\"model\",\"created\":0,\"owned_by\":\"local\"}\n",
         "the served id answers the model object: " + same.body);
}
void rejects_non_exact_routes() {
  auto router = router_with({});
  for (const char* target : {"/health/", "//health", "/HEALTH", "/v1/models/", "/v1//models", "/v1/chat/completions/"}) {
    const auto d = decision_of(router, "GET", target);
    expect(d.early.has_value() && d.early->status == 404, std::string{"404 for "} + target);
    if (d.early) {
      expect(d.early->body == secure_frontend::render_error_body(secure_frontend::ApiError::not_found),
             std::string{"404 body is the §7.2 not_found row for "} + target);
    }
  }
}
void fails_closed_when_provider_throws_or_returns_invalid_model() {
  auto router = router_with([]() -> secure_frontend::HealthView { throw 1; });
  const auto r = complete(router.handle(head_of("GET", "/health"), "", decision_of(router, "GET", "/health"), {}));
  expect(r.status == 503 && r.body == secure_frontend::render_error_body(secure_frontend::ApiError::engine_unavailable),
         "a throwing health provider is a fixed 503");
  const std::string internal = secure_frontend::render_error_body(secure_frontend::ApiError::internal_error);
  for (const std::string& bad_id : {std::string{"bad\x01model"}, std::string{}, std::string(1025, 'a'), std::string{"bad\x7fmodel"}}) {
    auto bad = router_with({}, bad_id);
    const auto models = complete(bad.handle(head_of("GET", "/v1/models"), "", decision_of(bad, "GET", "/v1/models"), {}));
    expect(models.status == 500 && models.body == internal, "an invalid model id fails closed (size " + std::to_string(bad_id.size()) + ")");
  }
  auto bad_owner = router_with({}, "fake-model", "own\ner");
  expect(get(bad_owner, "/v1/models").status == 500, "a control byte in owned_by fails closed");
  expect(get(bad_owner, "/v1/models/fake-model").status == 500, "and on /v1/models/<id> too");
  auto longest = router_with({}, std::string(1024, 'a'));
  expect(get(longest, "/v1/models").status == 200, "1024 bytes is the inclusive bound");
}
void auth01_every_route_rejects_missing_malformed_wrong_token() {
  auto router = router_with({});
  const std::string token{kRouterToken};
  const std::pair<std::string, int> variants[] = {
      {"", 401}, {"Authorization: bearer " + token + "\r\n", 401}, {"Authorization: Bearer \r\n", 401},
      {"Authorization: Bearer\t" + token + "\r\n", 401}, {"Authorization: Basic " + token + "\r\n", 401},
      {"Authorization: Bearer " + token + "\r\nAuthorization: Bearer " + token + "\r\n", 400},
      {"Authorization: Bearer wrong-token-0123456789abcdefghijklmn\r\n", 401}};
  std::vector<std::pair<std::string, std::string>> routes;
  for (const auto& entry : secure_frontend::kRouteTable) {
    routes.emplace_back(std::string{entry.method}, entry.prefix ? std::string{entry.path} + "fake-model" : std::string{entry.path});
  }
  routes.emplace_back("GET", "/does-not-exist");
  for (const auto& [method, target] : routes) {
    for (const auto& [headers, status] : variants) {
      std::string request = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + headers;
      request += method == "POST" ? "Content-Length: 2\r\nContent-Type: application/json\r\n\r\n{}" : "\r\n";
      const auto response = secure_frontend::handle_http_request(request, router, kRouterLimits);
      expect(response.status == status, method + " " + target + " must answer " + std::to_string(status));
      expect(response.body.find("fake-model") == std::string::npos && response.body.find("status") == std::string::npos,
             "no route data before authentication");
      if (status == 401) {
        expect(response.header_value("WWW-Authenticate") == std::optional<std::string_view>{"Bearer"}, "401 names the Bearer scheme");
      }
    }
  }
  const auto allowed = secure_frontend::handle_http_request(
      "GET /v1/models HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nAuthorization: Bearer " + token + "\r\n\r\n", router, kRouterLimits);
  expect(allowed.status == 200 && allowed.body.find("fake-model") != std::string::npos,
         "the same route answers with the right token (the negatives are not vacuous)");
}
void models_list_keeps_utf8_model_id() {
  auto router = router_with({}, "mod\xc3\xa8le-\xc3\xbc");
  const auto r = complete(router.handle(head_of("GET", "/v1/models"), "", decision_of(router, "GET", "/v1/models"), {}));
  expect(r.status == 200 && r.body.find("mod\xc3\xa8le-\xc3\xbc") != std::string::npos && r.body.find("\\u00") == std::string::npos,
         "UTF-8 model id copied unchanged");
  // Spec §3.9: copied unchanged only after validation; invalid UTF-8 fails closed.
  const std::string internal = secure_frontend::render_error_body(secure_frontend::ApiError::internal_error);
  for (const std::string& invalid : {std::string{"mod\xc3"}, std::string{"\xff-model"}, std::string{"\xed\xa0\x80"},
                                     std::string{"\xc0\xaf"}}) {
    auto bad = router_with({}, invalid);
    const auto models = get(bad, "/v1/models");
    expect(models.status == 500 && models.body == internal, "an invalid UTF-8 model id fails closed");
    auto bad_owner = router_with({}, "fake-model", invalid);
    expect(get(bad_owner, "/v1/models").status == 500, "an invalid UTF-8 owned_by fails closed");
  }
}
// --- The built-in chat page (listener.web_ui) and the keyless loopback listener -------
// (owner decisions 2026-10-21, session log step 12)

secure_frontend::ApiRouter page_router(const bool web_ui, const bool require_key) {
  secure_frontend::SecretBytes secret;
  (void)secret.assign(kRouterToken);
  return secure_frontend::ApiRouter({
      .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address("127.0.0.1"), 8080, {}, {}),
      .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
      .model_id = "fake-model", .owned_by = "local", .chat_body_limit = 49152,
      .health = [] { return secure_frontend::HealthView{true, "ready", ""}; },
      .chat = {}, .web_ui = web_ui, .require_key = require_key});
}
// One request with no Authorization header; `fields` are extra header lines.
secure_frontend::HttpResponse bare(secure_frontend::ApiRouter& router, const std::string& method, const std::string& target,
                                   const std::string& fields = "") {
  std::string request = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n" + fields;
  request += method == "POST" ? "Content-Length: 2\r\nContent-Type: application/json\r\n\r\n{}" : "\r\n";
  return secure_frontend::handle_http_request(request, router, kRouterLimits);
}

void web_ui_off_by_default_serves_nothing_public() {
  auto router = router_with({});
  for (const auto& asset : secure_frontend::web_ui_assets()) {
    const auto response = bare(router, "GET", std::string{asset.target});
    expect(response.status == 401 && response.content_security_policy.empty(),
           "web_ui off: " + std::string{asset.target} + " needs the key like every other target");
    expect(get(router, std::string{asset.target}).status == 404, "web_ui off: with the key it is not a route at all");
  }
}

void web_ui_serves_the_bundled_page_without_a_key() {
  auto router = page_router(true, true);
  // Session log step 14: the page, its two scripts and three stylesheets (upstream's UI kit,
  // MIT), the icon sprite and favicon, and the Outfit font (OFL) - each at one exact target.
  const std::vector<std::pair<std::string, std::string>> expected{
      {"/", "text/html; charset=utf-8"},
      {"/ui/theme.js", "text/javascript; charset=utf-8"},
      {"/ui/app.js", "text/javascript; charset=utf-8"},
      {"/ui/tokens.css", "text/css; charset=utf-8"},
      {"/ui/components.css", "text/css; charset=utf-8"},
      {"/ui/app.css", "text/css; charset=utf-8"},
      {"/ui/sprite.svg", "image/svg+xml; charset=utf-8"},
      {"/ui/favicon.svg", "image/svg+xml; charset=utf-8"},
      {"/ui/fonts/outfit-latin-wght.woff2", "font/woff2"},
      {"/ui/fonts/outfit-latin-ext-wght.woff2", "font/woff2"}};
  expect(secure_frontend::web_ui_assets().size() == expected.size(), "exactly the bundled files");
  for (const auto& [target, type] : expected) {
    const auto* asset = secure_frontend::find_web_asset(target);
    expect(asset != nullptr && asset->content_type == type, target + " is bundled as " + type + " (nosniff needs it exact)");
  }
  for (const auto& asset : secure_frontend::web_ui_assets()) {
    const std::string target{asset.target};
    const auto response = bare(router, "GET", target);
    expect(response.status == 200, target + " answers 200 without a key");
    expect(response.body == asset.body, target + " is the bundled file, byte for byte");
    expect(response.header_value("Content-Type") == std::optional<std::string_view>{asset.content_type},
           target + " has its own type: " + std::string{response.header_value("Content-Type").value_or("<none>")});
    expect(response.content_security_policy == secure_frontend::kWebUiContentSecurityPolicy,
           target + " carries the page policy (self only), not default-src 'none'");
    expect(response.header_value("Cross-Origin-Opener-Policy") == std::optional<std::string_view>{"same-origin"},
           target + " isolates its browsing context");
  }
  const auto authorized = get(router, "/");
  expect(authorized.status == 200 && authorized.body == secure_frontend::find_web_asset("/")->body,
         "a request that carries the key gets the same page");
}

void web_ui_leaves_every_api_route_authenticated() {
  auto router = page_router(true, true);
  for (const auto& entry : secure_frontend::kRouteTable) {
    const std::string target = entry.prefix ? std::string{entry.path} + "fake-model" : std::string{entry.path};
    const auto response = bare(router, std::string{entry.method}, target);
    expect(response.status == 401 && response.body.find("fake-model") == std::string::npos,
           std::string{entry.method} + " " + target + " still needs the key with web_ui on");
  }
  // Only the exact GET targets are public: no other method, suffix or neighbour (401, the
  // key is asked for), and no query at all (the parser refuses '?' in a target: 400).
  for (const auto& [method, target, status] : std::vector<std::tuple<std::string, std::string, int>>{
           {"POST", "/", 401}, {"HEAD", "/", 401}, {"GET", "/ui/", 401}, {"GET", "/ui/other.js", 401},
           {"GET", "/index.html", 401}, {"GET", "//", 401}, {"GET", "/UI/app.js", 401}, {"POST", "/ui/app.js", 401},
           {"GET", "/ui/fonts/", 401}, {"GET", "/ui/fonts/OFL.txt", 401}, {"GET", "/ui/LICENSE.strata-ui.txt", 401},
           {"GET", "/?a=1", 400}, {"GET", "/ui/app.js?v=1", 400}}) {
    const auto response = bare(router, method, target);
    expect(response.status == status && response.content_security_policy.empty() && response.body.find("<html") == std::string::npos,
           method + " " + target + " is not a page file (" + std::to_string(response.status) + ")");
  }
}

void web_ui_still_checks_host_and_origin() {
  auto router = page_router(true, true);
  expect(bare(router, "GET", "/").status == 200, "control: the page answers");
  const auto rebound = secure_frontend::handle_http_request("GET / HTTP/1.1\r\nHost: evil.example:8080\r\n\r\n", router, kRouterLimits);
  expect(rebound.status == 403 && rebound.body.find("<html") == std::string::npos, "a foreign Host (DNS rebinding) gets no page");
  const auto cross = bare(router, "GET", "/", "Origin: https://evil.example\r\n");
  expect(cross.status == 403, "a foreign Origin gets no page");
  const auto same = bare(router, "GET", "/ui/app.js", "Origin: http://127.0.0.1:8080\r\n");
  expect(same.status == 200, "the page's own origin is same-origin");
}

// The bundled files themselves (session log steps 12 and 14): every text file ASCII with LF line
// ends; nothing names another server (the SVG namespace URI is an identifier, never fetched); no
// inline script, style or handler (the CSP would refuse them anyway); the script renders text
// only, keeps the key in sessionStorage (this tab) and never in localStorage, which only the
// theme script uses, for the theme; the sprite carries no metadata blob; the fonts are WOFF2.
void web_ui_assets_are_bundled_and_inert() {
  const auto body = [](std::string_view target) { return std::string{secure_frontend::find_web_asset(target)->body}; };
  constexpr std::string_view kSvgNamespace = "http://www.w3.org/2000/svg";
  for (const auto& asset : secure_frontend::web_ui_assets()) {
    const std::string name{asset.target};
    std::string text{asset.body};
    if (asset.content_type == "font/woff2") {
      expect(text.rfind("wOF2", 0) == 0, name + " is a WOFF2 font");
      continue;
    }
    expect(!text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80U && c != '\r'; }),
           name + " is ASCII with LF line ends");
    for (std::size_t at = text.find(kSvgNamespace); at != std::string::npos; at = text.find(kSvgNamespace)) {
      text.erase(at, kSvgNamespace.size());
    }
    expect(text.find("http://") == std::string::npos && text.find("https://") == std::string::npos && text.find("//cdn") == std::string::npos,
           name + " names no other server");
  }
  const std::string html = body("/");
  expect(html.find("<script src=\"/ui/theme.js\"></script>") != std::string::npos &&
             html.find("<script src=\"/ui/app.js\" defer></script>") != std::string::npos,
         "the page loads exactly its two bundled scripts");
  std::size_t scripts = 0;
  for (std::size_t at = html.find("<script"); at != std::string::npos; at = html.find("<script", at + 1U)) ++scripts;
  expect(scripts == 2U, "two script elements, both bundled (no inline script)");
  for (const char* sheet : {"/ui/tokens.css", "/ui/components.css", "/ui/app.css"}) {
    expect(html.find(std::string{"<link rel=\"stylesheet\" href=\""} + sheet + "\">") != std::string::npos,
           std::string{"the page loads "} + sheet);
  }
  expect(html.find(" style=") == std::string::npos && html.find("<style") == std::string::npos, "no inline style");
  for (const char* handler : {" onclick=", " onload=", " onerror=", " onsubmit=", " oninput=", " onkeydown="}) {
    expect(html.find(handler) == std::string::npos, std::string{"no inline handler:"} + handler);
  }
  expect(html.find("id=\"key\" type=\"password\"") != std::string::npos && html.find("name=\"key\"") == std::string::npos,
         "the key field is a password input with no name, so no form can ever put it in a URL");
  const std::string js = body("/ui/app.js");
  for (const char* sink : {"innerHTML", "outerHTML", "insertAdjacentHTML", "document.write", "eval(", "new Function",
                           "localStorage", "setAttribute(\"on", "javascript:"}) {
    expect(js.find(sink) == std::string::npos, std::string{"app.js never uses "} + sink);
  }
  expect(js.find("sessionStorage") != std::string::npos && js.find("textContent") != std::string::npos &&
             js.find("credentials = \"omit\"") != std::string::npos,
         "the key lives in sessionStorage, output is text, no cookies are sent");
  const std::string theme = body("/ui/theme.js");
  expect(theme.find("strata.theme") != std::string::npos && theme.find("sessionStorage") == std::string::npos &&
             theme.find("fetch(") == std::string::npos && theme.find("Key") == std::string::npos,
         "the theme script stores the theme and nothing else");
  const std::string sprite = body("/ui/sprite.svg");
  expect(sprite.find("<metadata") == std::string::npos && sprite.find("c2pa") == std::string::npos && sprite.find("i-new-chat") != std::string::npos,
         "the icon sprite is the symbols only");
}

// Session log step 16: the page has upstream's Monitor tab (fed by GET /metrics, built from DOM
// nodes like the rest) and upstream's four thinking levels, sent as reasoning_effort.
void web_ui_has_the_monitor_and_four_thinking_levels() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  expect(html.find("id=\"tab-btn-monitor\"") != std::string::npos && html.find("id=\"view-monitor\"") != std::string::npos &&
             html.find("id=\"monitor-off\"") != std::string::npos,
         "a Monitor tab, its view, and the note for a server with the Monitor off");
  for (const char* level : {"data-v=\"none\"", "data-v=\"low\"", "data-v=\"medium\"", "data-v=\"high\""}) {
    expect(html.find(level) != std::string::npos, std::string{"the Thinking control offers "} + level);
  }
  expect(html.find("data-v=\"on\"") == std::string::npos, "the old Off / On control is gone");
  expect(js.find("api(\"/metrics\")") != std::string::npos && js.find("response.status === 404") != std::string::npos,
         "the Monitor asks /metrics through the keyed API helper and handles the switch being off");
  expect(js.find("reasoning_effort: LEVELS[s.thinking] ? s.thinking : \"high\"") != std::string::npos,
         "the level is sent as reasoning_effort, high by default");
  // Gemma 4 plan 1 Task 12 (spec §6.6, D10): the drawer says what the levels mean for Gemma.
  expect(html.find("Gemma models think or answer directly; Low, Medium and High all mean thinking on") != std::string::npos,
         "the Thinking control notes that Gemma has no levels");
}

// Owner 2026-10-27 (item 46's fix, "Page omits it"): a running card whose profile refuses reasoning_effort (the list's
// "reasoning_effort": false - a GLM card) gets no reasoning_effort from the page; every other chat still sends the
// level (the server says so per card; with model control off the page cannot tell and sends it as before).
void the_page_sends_reasoning_effort_only_where_it_is_taken() {
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  expect(js.find("reasoning_effort: LEVELS[s.thinking] ? s.thinking : \"high\"") != std::string::npos,
         "the level is still the request's reasoning_effort");
  expect(js.find("if (!sendsReasoning()) { delete body.reasoning_effort; }") != std::string::npos,
         "the page leaves it out where the running card does not take it");
  expect(js.find("return !card || card.reasoning_effort !== false;") != std::string::npos,
         "only a card that says false loses it");
}

// Model launcher plan Task 5 (spec §6): the Models tab. It is in the page from the start but
// hidden, and shown only when GET /v1/strata/models answers; a server with model control off
// answers 404 and the tab stays hidden, so the page is exactly today's.
void web_ui_has_the_models_tab_hidden_until_the_server_offers_it() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  expect(html.find("<button class=\"st-tab\" role=\"tab\" id=\"tab-btn-models\" aria-controls=\"view-models\" "
                   "aria-selected=\"false\" data-tab=\"models\" hidden>") != std::string::npos,
         "a Models tab beside Chat and Monitor, hidden until the server offers it");
  expect(html.find("id=\"tab-btn-models\"") > html.find("id=\"tab-btn-chat\"") &&
             html.find("id=\"tab-btn-models\"") < html.find("id=\"tab-btn-monitor\""),
         "the Models tab sits between Chat and Monitor");
  expect(html.find("<section class=\"view view--models\" id=\"view-models\" role=\"tabpanel\" aria-labelledby=\"tab-btn-models\" hidden>") !=
             std::string::npos,
         "the Models view, hidden like every other view");
  for (const char* id : {"id=\"model-list\"", "id=\"model-detail\"", "id=\"model-options\"", "id=\"model-start\"",
                         "id=\"model-stop\"", "id=\"model-progress\"", "id=\"model-start-error\"", "id=\"model-detail-error\""}) {
    expect(html.find(id) != std::string::npos, std::string{"the Models view has "} + id);
  }
  expect(js.find("api(\"/v1/strata/models\")") != std::string::npos,
         "the card list is asked through the keyed API helper");
  expect(js.find("if (response.status === 404) { setLauncherEnabled(false); return; }") != std::string::npos &&
             js.find("$(\"tab-btn-models\").hidden = !on;") != std::string::npos,
         "a 404 (model control off) hides the Models tab");
  // Owner report 2026-10-23 ("I do not see the new tab", from another machine): a 403
  // (models.control_remote off for a remote client) shows the tab with the reason and the fix,
  // instead of hiding it with no word.
  expect(js.find("if (response.status === 403) { setLauncherDenied(); return; }") != std::string::npos &&
             js.find("$(\"models-denied\").hidden = false;") != std::string::npos,
         "a 403 shows the Models tab with the reason");
  expect(html.find("id=\"models-denied\"") != std::string::npos && html.find("control_remote") != std::string::npos,
         "the reason names the switch that allows other machines");
  // (2026-10-28: the Settings tab joined the views)
  expect(js.find("[\"login\", \"chat\", \"models\", \"monitor\", \"settings\", \"about\"]") != std::string::npos &&
             js.find("$(\"tab-btn-models\").addEventListener(\"click\", function () { showView(\"models\"); });") != std::string::npos,
         "the Models tab switches views like the others");
}

// The Models view drives the control API: every call through the keyed helper (one fetch in
// the whole script), every option kind rendered from the server's description, a confirmation
// before a running model is replaced, the server's refusal shown beside Start (with the option
// it names), and a 2 s poll while a card is starting.
// Owner request 2026-10-23: the context and KV streaming as dropdowns. An integer option the server
// describes with a value list renders as a select labelled with the server's labels ("128K", "1M",
// "Off"), and an option with at_most (KV streaming) offers no value above the other option's
// (the context), following it as it changes and after Defaults.
void web_ui_listed_integers_are_dropdowns() {
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  expect(js.find("case \"integer\": return Array.isArray(option.choices) ? choiceControl(option, value) : numberControl(option, value, true, false);") !=
             std::string::npos,
         "a listed integer is a dropdown, any other integer a number box");
  expect(js.find("option.choice_labels") != std::string::npos, "the dropdown shows the server's labels");
  expect(js.find("c.option.at_most") != std::string::npos && js.find("ceiling.element.addEventListener(\"change\"") != std::string::npos,
         "KV streaming follows the context's dropdown");
  expect(js.find("launcher.limits.forEach(function (apply) { apply(); });") != std::string::npos,
         "Defaults applies the ceiling again");
}

void web_ui_models_tab_starts_stops_and_polls() {
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  // top_p's lower bound is the smallest double above 0 (an open bound): the page says
  // "above 0", never "5e-324" (box run 2026-10-23).
  expect(js.find("n > 0 && n < 1e-6") != std::string::npos && js.find("\"above 0\"") != std::string::npos,
         "a tiny positive bound reads as above 0");
  expect(js.find("fetch(") != std::string::npos && js.find("fetch(") == js.rfind("fetch("),
         "one fetch in the script: every call goes through the keyed API helper");
  expect(js.find("api(\"/v1/strata/models/start\", {") != std::string::npos &&
             js.find("JSON.stringify({ name: card.name, options: collected.values })") != std::string::npos,
         "Start posts the card's name and its typed option values");
  expect(js.find("api(\"/v1/strata/models/stop\", { method: \"POST\" })") != std::string::npos, "Stop posts to the stop route");
  for (const char* kind : {"case \"boolean\":", "case \"integer\":", "case \"decimal\":", "case \"choice\":",
                           "case \"integer_or_auto\":"}) {
    expect(js.find(kind) != std::string::npos, std::string{"the options renderer handles "} + kind);
  }
  expect(js.find("toggle.setAttribute(\"role\", \"switch\")") != std::string::npos && js.find("el(\"select\", \"st-input\")") != std::string::npos &&
             js.find("auto.type = \"checkbox\"") != std::string::npos && js.find("input.step = integral ? \"1\" : \"any\"") != std::string::npos,
         "a switch, a choice list, an auto box and number inputs with integer or decimal steps");
  expect(js.find("input.min = String(option.min)") != std::string::npos && js.find("input.max = String(option.max)") != std::string::npos,
         "number inputs carry the card's bounds");
  expect(js.find("window.confirm(") != std::string::npos && js.find("if (running && !window.confirm(") != std::string::npos,
         "Start asks before a running model is stopped");
  expect(js.find("parsed.error.param") != std::string::npos && js.find("$(\"model-start-error\").textContent") != std::string::npos,
         "a refusal is shown beside Start, naming the option");
  expect(js.find("launcher.timer = window.setTimeout(pollLauncher, 2000);") != std::string::npos &&
             js.find("function launcherStarting(") != std::string::npos,
         "the page asks again every 2 s while a card is starting");
  expect(js.find("$(\"model-progress\").textContent") != std::string::npos, "a progress line while a card loads");
}

// GUI-first start-up (spec 2026-10-27 §4.2-§4.5): the Models tab adds a model from files already on this PC (the
// server's folder browser, then cards/add; a folder others may write is offered to be made private), a card the page
// made shows its folder and follows a moved one, and its Danger zone deletes it behind the server's three checks: a
// code shown and typed back, the API key typed again (never the tab's), and a last "permanent" confirmation.
void web_ui_cards_from_this_pc_and_the_danger_zone() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  const std::string css{secure_frontend::find_web_asset("/ui/app.css")->body};
  for (const char* id : {"id=\"folder-picker\"", "id=\"folder-picker-title\"", "id=\"folder-picker-path\"", "id=\"folder-picker-up\"",
                         "id=\"folder-picker-list\"", "id=\"folder-picker-free\"", "id=\"folder-picker-typed\"", "id=\"folder-picker-new\"",
                         "id=\"folder-picker-new-name\"", "id=\"folder-picker-use\"", "id=\"folder-picker-cancel\"", "id=\"folder-picker-error\"",
                         "id=\"local-models\"", "id=\"local-models-pick\"", "id=\"local-models-error\"", "id=\"models-empty-add\"",
                         "id=\"model-folder\"", "id=\"model-folder-path\"", "id=\"model-folder-change\"", "id=\"danger-zone\"",
                         "id=\"model-delete\"", "id=\"delete-dialog\"", "id=\"delete-code\"", "id=\"delete-code-input\"",
                         "id=\"delete-key-input\"", "id=\"delete-final-text\"", "id=\"delete-next\"", "id=\"delete-cancel\"",
                         "id=\"delete-error\""}) {
    expect(html.find(id) != std::string::npos, std::string{"the page has "} + id);
  }
  expect(html.find("<dialog class=\"st-card folder-picker\" id=\"folder-picker\"") != std::string::npos &&
             html.find("<dialog class=\"st-card delete-dialog\" id=\"delete-dialog\"") != std::string::npos,
         "the folder picker and the delete steps are dialogs");
  expect(html.find("<details class=\"danger-zone\" id=\"danger-zone\" hidden>") != std::string::npos &&
             html.find("<summary>Danger zone</summary>") != std::string::npos,
         "a collapsed Danger zone, hidden unless the card is the page's");
  const auto key = html.find("id=\"delete-key-input\"");
  const std::string key_tag = key == std::string::npos ? std::string{} : html.substr(html.rfind('<', key), html.find('>', key) - html.rfind('<', key));
  expect(key_tag.find("type=\"password\"") != std::string::npos && key_tag.find("name=") == std::string::npos,
         "the key is typed again into a password field with no name");
  expect(html.find("id=\"local-models\"") > html.find("id=\"models-pane-download\"") &&
             html.find("id=\"local-models\"") < html.find("id=\"downloads\""),
         "model files on this PC come first in the Download pane");
  for (const char* route : {"api(\"/v1/strata/folders\", jsonPost(", "api(\"/v1/strata/folders/private\", jsonPost(",
                            "api(\"/v1/strata/folders/new\", jsonPost(", "api(\"/v1/strata/cards/add\", jsonPost(",
                            "api(\"/v1/strata/cards/folder\", jsonPost(", "api(\"/v1/strata/cards/delete/begin\", jsonPost(",
                            "api(\"/v1/strata/cards/delete\", jsonPost("}) {
    expect(js.find(route) != std::string::npos, std::string{"the page calls "} + route);
  }
  expect(js.find("answer.code === \"folder_not_private\"") != std::string::npos && js.find("function offerPrivate(") != std::string::npos,
         "a folder others may write is offered to be made private, then the add is tried again");
  expect(js.find("permanent: true") != std::string::npos && js.find("key: removal.key") != std::string::npos &&
             js.find("$(\"delete-key-input\").value = \"\";") != std::string::npos,
         "the delete sends the typed key and permanent: true, and the key field is emptied");
  expect(js.find("card.page_card === true") != std::string::npos && js.find("$(\"danger-zone\").hidden = !pageCard;") != std::string::npos,
         "only a card the page made shows its folder and its Danger zone");
  expect(js.find("$(\"model-delete\").disabled = inUse") != std::string::npos, "a running model can't be deleted from the page");
  expect(js.find("if (done.download_removed === false) {") != std::string::npos,
         "a downloaded model whose store kept its record says so after the delete");
  // Owner 2026-10-28 ("an ability to remove an active model card but not delete files"): Remove card, the files kept, a
  // running card stopped first; asked first, through the remove route
  expect(html.find("id=\"model-remove-box\"") != std::string::npos && html.find("id=\"model-remove\"") != std::string::npos &&
             html.find(">Remove card</button>") != std::string::npos,
         "a card the page made can be removed, its files kept");
  expect(js.find("api(\"/v1/strata/cards/remove\", jsonPost({ name: card.name }))") != std::string::npos &&
             js.find("Its model files stay on this PC") != std::string::npos && js.find("$(\"model-remove-box\").hidden = !pageCard;") != std::string::npos &&
             js.find("$(\"model-remove\").disabled = launcher.acting || card.state === \"starting\" || card.state === \"calibrating\";") !=
                 std::string::npos,
         "the remove asks first, says the files stay, and calls the remove route; a running card's too");
  expect(css.find(".model-remove") != std::string::npos, "styled: .model-remove");
  expect(js.find("$(\"local-models\").hidden = !on;") != std::string::npos, "the PC's model files follow model control");
  for (const char* rule : {".folder-picker", ".folder-row", ".delete-dialog", ".danger-zone"}) {
    expect(css.find(rule) != std::string::npos, std::string{"styled: "} + rule);
  }
}

// GUI-first start-up (spec 2026-10-27 §5.3): each card shows its calibration - its state, the speed before and after and
// the kept flags - with Calibrate (Recalibrate once there is one), the progress while it runs (asked every 2 s like a
// start), Cancel, and the "Recalibrate when settings change" switch.
void web_ui_models_tab_calibrates() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  for (const char* id : {"id=\"model-calibration\"", "id=\"model-calibration-state\"", "id=\"model-calibration-text\"",
                         "id=\"model-calibration-prefill\"", "id=\"model-calibration-progress\"", "id=\"model-calibrate\"",
                         "id=\"model-calibrate-cancel\"", "id=\"model-calibration-auto\""}) {
    const auto at = html.find(id);
    expect(at != std::string::npos && at > html.find("id=\"model-detail\"") && at < html.find("id=\"models-pane-download\""),
           std::string{"the model detail has "} + id);
  }
  expect(js.find("api(\"/v1/strata/models/calibrate\", jsonPost({ name: card.name, options: collected.values }))") != std::string::npos &&
             js.find("api(\"/v1/strata/models/calibrate/cancel\", jsonPost({}))") != std::string::npos &&
             js.find("api(\"/v1/strata/models/calibrate/auto\", jsonPost({ name: card.name, on: on }))") != std::string::npos,
         "Calibrate posts the card and its options; Cancel and the switch have their routes");
  expect(js.find("calibrating: [\"st-badge--reading\", \"Calibrating\"]") != std::string::npos &&
             js.find("card.state === \"starting\" || card.state === \"calibrating\"") != std::string::npos,
         "a calibrating card has its badge and is polled like a start");
  expect(js.find("function calibrationText(") != std::string::npos && js.find("cal.state === \"settings_changed\"") != std::string::npos &&
             js.find("cal.state === \"stale\"") != std::string::npos,
         "the calibration's states are said in words");
  expect(js.find("$(\"model-calibration\").hidden = !cal;") != std::string::npos, "a card with no calibration shows none");
  // 2026-10-28 (every model calibrates): only the engines that calibrate before a first start say so; MiniMax shows its
  // prompt reading (the prefill calibration) - measured, failed, out of date or still to come
  expect(js.find("cal.first_start === true") != std::string::npos && js.find("function prefillText(") != std::string::npos &&
             js.find("p.state === \"failed\"") != std::string::npos && js.find("p.state === \"stale\"") != std::string::npos &&
             js.find("stage === \"prompt reading\"") != std::string::npos,
         "the first start and MiniMax's prompt reading are said in words");
}

// The Settings tab (spec 2026-10-28-server-window-design, Part 1; the owner: "a configuration tab ... api key, etc"): the
// API key (the one the page signed in with, shown on request and copied - never fetched), its rotation (typed ROTATE,
// the new key shown once), and the server's settings drawn from POST /v1/strata/config and saved through
// /v1/strata/config/settings (each with its type, its default as the placeholder, and the schema's own words).
void web_ui_has_server_settings() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  for (const char* id : {"id=\"key-settings\"", "id=\"key-value\"", "id=\"key-show\"", "id=\"key-copy\"", "id=\"key-rotate-confirm\"",
                         "id=\"key-rotate\"", "id=\"key-new\"", "id=\"server-settings\"", "id=\"limits-settings\"",
                         "id=\"paths-settings\"", "id=\"settings-restart-note\""}) {
    expect(html.find(id) != std::string::npos, std::string{"the Settings tab has "} + id);
  }
  expect(js.find("api(\"/v1/strata/config\", jsonPost({}))") != std::string::npos &&
             js.find("api(\"/v1/strata/config/settings\", jsonPost({ values: values }))") != std::string::npos &&
             js.find("api(\"/v1/strata/key/rotate\", jsonPost({ confirm: \"ROTATE\" }))") != std::string::npos,
         "the three routes");
  expect(js.find("$(\"key-value\").textContent = shown ? state.key : ") != std::string::npos,
         "the key shown is the one the page holds (no route hands a key out)");
  expect(js.find("if ($(\"key-rotate-confirm\").value.trim() !== \"ROTATE\")") != std::string::npos,
         "rotation is typed ROTATE first");
  expect(js.find("\"listener.port\"") != std::string::npos && js.find("\"limits.engine.generation_timeout_s\"") != std::string::npos &&
             js.find("\"models.downloads_root\"") != std::string::npos,
         "the three groups name their settings");
  // the two folders are chosen with the page's folder picker (spec Part 1, Folders), or typed
  expect(js.find("var CONFIG_FOLDERS = [\"models.downloads_root\", \"models.default_root\"];") != std::string::npos &&
             js.find("var picked = await pickFolder(label, ") != std::string::npos &&
             js.find("if (picked && picked.folder) { input.value = String(picked.folder); }") != std::string::npos,
         "a folder setting has Choose, through the folder picker");
}

// A one-time login link is claimed at load and also when it arrives in an open tab's address (strata-window opens a
// restarted server's new link in the page it shows: a fragment change, no reload - after a key rotation the old key
// no longer works, so the new link must be claimed). A failed link while signed in keeps the session.
void web_ui_claims_a_login_link_at_load_and_on_hashchange() {
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  expect(js.find("async function claimFromAddress(signedIn) {") != std::string::npos, "one claim for both");
  expect(js.find("if (await claimFromAddress(false)) { return; }") != std::string::npos, "at load");
  expect(js.find("window.addEventListener(\"hashchange\", function () { claimFromAddress(!!state.key || state.keyless); });") !=
             std::string::npos,
         "and when a new link arrives in the address");
  expect(js.find("if (signedIn) { toast(\"error\", expired); return true; }") != std::string::npos,
         "a failed link while signed in keeps the session");
  // a restart's link buys the same key: the key is shown once only when it is new to this tab (a rotation's new key)
  expect(js.find("var before = state.key;") != std::string::npos &&
             js.find("if (state.key === claimed && claimed !== before) { showKeyOnce(claimed); }") != std::string::npos,
         "the key dialog only for a key new to this tab");
}

// The view an address names (#view=<name>, or #login=<code>&view=<name>; chat, monitor or about - the views every server
// has): strata-window opens the page on the Monitor (the owner: "the open window shows the Monitor statistics"). A
// restarted server's link signs the open page in again where it was, not back on Chat.
void web_ui_opens_the_view_its_address_names() {
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  expect(js.find("var asked = /(?:^#|&)view=(chat|monitor|about)$/.exec(hash || \"\");") != std::string::npos,
         "the views an address may name");
  expect(js.find("/^#login=([A-Za-z0-9_-]{16,128})(?:&view=[a-z]+)?$/.exec(window.location.hash || \"\")") != std::string::npos,
         "a login link may name one");
  expect(js.find("var view = addressView(window.location.hash) || (signedIn ? state.view : null);") != std::string::npos &&
             js.find("await signIn(claimed, view);") != std::string::npos,
         "the link's view, else the view shown (a restart)");
  expect(js.find("function enterApp(models, view) {") != std::string::npos && js.find("showView(view || \"chat\");") != std::string::npos,
         "signed in on that view, else Chat");
  expect(js.find("if (typeof key === \"string\" && key) { await signIn(key, addressView(window.location.hash)); return; }") !=
             std::string::npos,
         "a held key opens the named view too");
}

// GUI-first start-up (spec 2026-10-27 §5a): a Settings sub-tab with the email alerts - shown only when the server has
// them (its alerts route answers; a build without them answers 404) - the IONOS preset, which alerts, the password
// (a password field, saved on its own and never shown again), Save and "Send a test email", and a note that the
// settings apply when the server restarts.
void web_ui_has_email_alert_settings() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  // (2026-10-28, the Settings tab: the settings moved from Models > Settings to a tab of their own)
  expect(html.find("<button class=\"st-tab\" role=\"tab\" id=\"tab-btn-settings\" aria-controls=\"view-settings\" aria-selected=\"false\" "
                   "data-tab=\"settings\" hidden>") != std::string::npos,
         "a Settings tab, hidden until the server offers it");
  for (const char* id : {"id=\"view-settings\"", "id=\"alerts-enabled\"", "id=\"alerts-host\"", "id=\"alerts-port\"",
                         "id=\"alerts-security\"", "id=\"alerts-username\"", "id=\"alerts-from\"", "id=\"alerts-to\"",
                         "id=\"alerts-events\"", "id=\"alerts-ionos\"", "id=\"alerts-save\"", "id=\"alerts-test\"",
                         "id=\"alerts-password\"", "id=\"alerts-password-save\"", "id=\"alerts-password-state\"",
                         "id=\"alerts-result\"", "id=\"alerts-error\""}) {
    expect(html.find(id) != std::string::npos, std::string{"the alert settings have "} + id);
  }
  const auto password = html.find("id=\"alerts-password\"");
  const std::string tag = password == std::string::npos ? std::string{}
                                                        : html.substr(html.rfind('<', password), html.find('>', password) - html.rfind('<', password));
  expect(tag.find("type=\"password\"") != std::string::npos && tag.find("name=") == std::string::npos, "the password field is a password input");
  expect(html.find("apply when the server restarts") != std::string::npos, "the page says when they apply");
  expect(js.find("api(\"/v1/strata/alerts\", jsonPost({}))") != std::string::npos &&
             js.find("api(\"/v1/strata/alerts/settings\", jsonPost(alertsForm()))") != std::string::npos &&
             js.find("api(\"/v1/strata/alerts/password\", jsonPost({ password: password }))") != std::string::npos &&
             js.find("api(\"/v1/strata/alerts/test\", jsonPost({}))") != std::string::npos,
         "the four routes");
  // (changed 2026-10-27 with the Model folders setting: the sub-tab shows with it, alerts or web search; 2026-10-28: a tab,
  // shown with the server's own settings too)
  expect(js.find("var any = alerts.built || search.built || folders.built || serverConfig.built;") != std::string::npos &&
             js.find("$(\"tab-btn-settings\").hidden = !any;") != std::string::npos &&
             js.find("$(\"alerts-settings\").hidden = !alerts.built;") != std::string::npos,
         "the sub-tab shows when the server has alerts (or web search, or model folders), the alerts card only with alerts");
  expect(js.find("\"smtp.ionos.com\"") != std::string::npos && js.find("$(\"alerts-port\").value = \"465\";") != std::string::npos,
         "the IONOS preset: smtp.ionos.com, 465, SSL/TLS");
  expect(js.find("$(\"alerts-password\").value = \"\";") != std::string::npos, "the password field is emptied once sent");
}

// Web search (spec 2026-10-27-web-search-design §2): the Settings sub-tab's web search section - on/off, the SearXNG
// address, the results, rounds and time limit, save and a test search over the three routes, shown only when the server
// has search - and the chat's note of what a reply searched for (its strata_search events).
void web_ui_has_web_search_settings() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  for (const char* id : {"id=\"search-settings\"", "id=\"search-enabled\"", "id=\"search-url\"", "id=\"search-results\"",
                         "id=\"search-rounds\"", "id=\"search-timeout\"", "id=\"search-save\"", "id=\"search-query\"",
                         "id=\"search-test\"", "id=\"search-state\"", "id=\"search-result\"", "id=\"search-output\"",
                         "id=\"search-error\""}) {
    expect(html.find(id) != std::string::npos, std::string{"the search settings have "} + id);
  }
  expect(html.find("<div class=\"st-card alerts-settings\" id=\"search-settings\" hidden>") != std::string::npos,
         "the section is hidden until the server offers it");
  expect(html.find("leave this PC through that instance") != std::string::npos && html.find("search: formats:") != std::string::npos,
         "the page says where the searches go and what the instance needs");
  expect(js.find("api(\"/v1/strata/search\", jsonPost({}))") != std::string::npos &&
             js.find("api(\"/v1/strata/search/settings\", jsonPost(searchForm()))") != std::string::npos &&
             js.find("api(\"/v1/strata/search/test\", jsonPost({ query: $(\"search-query\").value.trim() }))") != std::string::npos,
         "the three routes");
  expect(js.find("$(\"search-settings\").hidden = !search.built;") != std::string::npos, "the section shows only with search");
  expect(js.find("var search = event.strata_search;") != std::string::npos && js.find("\"Searched the web\"") != std::string::npos,
         "a reply notes what it searched for");
}

// Owner 2026-10-28: the searches in a box, as the thinking is ("they look out of place"), the round limit said as
// upstream's page says it, and About's engines in a table ("messy especially when it wraps").
void web_ui_search_box_limit_note_and_engine_table() {
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  const std::string css{secure_frontend::find_web_asset("/ui/app.css")->body};
  expect(js.find("el(\"details\", \"st-collapse search-box\")") != std::string::npos && js.find("el(\"ul\", \"search-list\")") != std::string::npos,
         "the searches are a collapsible box with a list, as the thinking is");
  expect(js.find("if (search && search.event === \"limit\") { run.limit = search.max_rounds; }") != std::string::npos &&
             js.find("Reached the limit of ") != std::string::npos,
         "the round limit is noted in the box");
  expect(js.find("el(\"table\", \"st-table about-engines\")") != std::string::npos && css.find(".about-engines") != std::string::npos,
         "About lists the engines as a table");
  // seen live 2026-10-28 in a narrow window: "multiengine_maya_d / ll" - a library name is never cut mid-word
  expect(css.find(".about-engines code { font: 400 12px var(--st-font-mono); white-space: nowrap; }") != std::string::npos &&
             css.find(".about-engines td { overflow-wrap: anywhere;") == std::string::npos,
         "a library name stays on one line; the table scrolls in its card instead");
}

// Owner 2026-10-28 ("Both"): the chat settings have a Web search switch, shown only while the running card offers web
// search and has it on; switched off, the page's chats send "strata_search": false. The card's own checkbox is its
// web_search option, which the cards render like every boolean option.
void web_ui_chat_can_turn_search_off() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  expect(html.find("<div class=\"toggle-row\" id=\"s-search-row\" hidden>") != std::string::npos &&
             html.find("id=\"s-search\" role=\"switch\"") != std::string::npos,
         "the drawer has a Web search switch, hidden until a model may search");
  expect(js.find("options[i].name === \"web_search\"") != std::string::npos &&
             js.find("$(\"s-search-row\").hidden = !searchOffered();") != std::string::npos,
         "it shows while the running card's Web search is on");
  expect(js.find("if (s.search === false && searchOffered()) { body.strata_search = false; }") != std::string::npos,
         "switched off, the chat asks for no search");
  expect(js.find("search: true") != std::string::npos, "on by default");
}

// Owner 2026-10-27 (folders "other users can access", used as they are): the Settings sub-tab's Model folders section -
// the switch, what it means, Save over its route, the note that it applies when the server restarts - and the "make it
// private" question names the switch.
void web_ui_has_model_folder_setting() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  for (const char* id : {"id=\"folders-settings\"", "id=\"folders-shared\"", "id=\"folders-save\"", "id=\"folders-state\"",
                         "id=\"folders-result\"", "id=\"folders-error\""}) {
    expect(html.find(id) != std::string::npos, std::string{"the model folder setting has "} + id);
  }
  expect(html.find("<div class=\"st-card alerts-settings\" id=\"folders-settings\" hidden>") != std::string::npos,
         "the section is hidden until the server offers it");
  expect(html.find("never changes their permissions") != std::string::npos &&
             html.find("other users of this PC can read or change") != std::string::npos,
         "the page says what the switch does and what it allows");
  expect(js.find("api(\"/v1/strata/folders/sharing\", jsonPost({}))") != std::string::npos &&
             js.find("api(\"/v1/strata/folders/sharing\", jsonPost({ shared: $(\"folders-shared\").checked }))") != std::string::npos,
         "it reads and saves over its route");
  expect(js.find("$(\"folders-settings\").hidden = !folders.built;") != std::string::npos, "the section shows only when served");
  expect(js.find("Use model folders as they are") != std::string::npos, "the private-folder question names the switch");
  // owner 2026-10-27 ("0.0 GB · 3 files"): a download card whose size is not pinned says when it is known instead
  expect(js.find("card.size_known === false ? \"size read from Hugging Face at the start\"") != std::string::npos,
         "a download card with no pinned size says so, not 0.0 GB");
}

// Owner 2026-10-27 ("in the about, it should list the build of our engines and also list the hardware of the system").
void web_ui_about_lists_the_engines_and_this_pc() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  for (const char* id : {"id=\"facts-build\"", "id=\"facts-hardware\"", "id=\"about-build-note\""}) {
    expect(html.find(id) != std::string::npos, std::string{"the About has "} + id);
  }
  expect(html.find("<span class=\"card-title\">Engines</span>") != std::string::npos &&
             html.find("<span class=\"card-title\">This PC</span>") != std::string::npos,
         "an Engines card and a This PC card");
  expect(js.find("api(\"/v1/strata/about\")") != std::string::npos && js.find("fact(hardware, \"CPU\"") != std::string::npos &&
             js.find("fact(hardware, \"GPU\"") != std::string::npos && js.find("(b.engines || []).forEach") != std::string::npos,
         "it reads its route and lists each engine and the hardware");
}

// Owner 2026-10-27: after a Stop the Monitor says so (a Stopped state, no live figures of the engine that ran), and each
// recent request names the model it ran on.
void web_ui_monitor_shows_stopped_and_the_model() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  expect(html.find("<span class=\"st-badge st-badge--stopped\" data-s=\"stopped\">Stopped</span>") != std::string::npos,
         "a Stopped badge in the model state");
  expect(html.find("<thead><tr><th>Time</th><th>Model</th><th>Status</th>") != std::string::npos &&
             html.find("<td colspan=\"9\" class=\"muted\">No requests yet</td>") != std::string::npos,
         "the recent requests have a Model column");
  expect(js.find("live.state === \"stopped\"") != std::string::npos && js.find("No model is running") != std::string::npos,
         "the page words the stopped state");
  expect(js.find("cell(r.model || \"-\")") != std::string::npos && js.find("td.colSpan = 9;") != std::string::npos,
         "each row shows its model");
}

// Owner 2026-10-28: the RAM card shows the experts and the engine's memory, not the PC's GB alone; the live figures
// reset when no engine is up (stopped, unloaded, failed) or the server does not answer; upstream's disk read tile and
// conversation cache card; and the panels in even rows, the recent requests last across the whole width.
void web_ui_monitor_is_even_and_shows_memory_disk_and_cache() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  const std::string css{secure_frontend::find_web_asset("/ui/app.css")->body};
  expect(js.find("var mem = loaded && eng.memory ? eng.memory : {};") != std::string::npos &&
             js.find("has(mem.experts_ram)") != std::string::npos && js.find("has(mem.experts_ram_bytes)") != std::string::npos &&
             js.find("\"engine \" + size(mem.process_bytes)") != std::string::npos && js.find("has(mem.experts_vram)") != std::string::npos,
         "the RAM and VRAM lines read engine.memory: the experts, their GB, the engine process");
  expect(js.find("ex.expert_slots - ex.expert_slots_primary") == std::string::npos,
         "the old guess (all the experts less the VRAM ones) is gone: on upstream's engine it is the other GPUs' share");
  expect(js.find("var loaded = eng.loaded === undefined ? !stopped && !offline : eng.loaded === true;") != std::string::npos &&
             js.find("var last = loaded ? requests[0] : null;") != std::string::npos,
         "the last request's figures only while an engine is up");
  expect(js.find("live.state === \"unloaded\"") != std::string::npos &&
             html.find("<span class=\"st-badge st-badge--stopped\" data-s=\"unloaded\">Unloaded</span>") != std::string::npos,
         "an engine unloaded after idling says so");
  expect(js.find("renderMonitor(offlineSnapshot(monitor.last));") != std::string::npos && js.find("The server is not answering") != std::string::npos,
         "a server that does not answer leaves no live figure on the page");
  expect(js.find("{ key: \"disk\", label: \"Disk read\", icon: \"i-disk\", series: \"disk_read_mb\", tone: \"info\" }") != std::string::npos &&
             js.find("hw.disk_read_mb >= 1000") != std::string::npos && js.find("\"peak \" + rate(diskPeak)") != std::string::npos,
         "upstream's Disk read tile: MB/s, GB/s past 1000, with its peak");
  expect(html.find("id=\"cc-card\"") != std::string::npos && html.find("id=\"cc-bars\"") != std::string::npos &&
             html.find("id=\"cc-facts\"") != std::string::npos && html.find("id=\"cc-note\"") != std::string::npos &&
             js.find("renderConvCache(m.conversation_cache, live.state);") != std::string::npos &&
             js.find("Not used by this engine") != std::string::npos,
         "upstream's Conversation cache card, with the state of an engine that has none");
  const auto row = html.find("id=\"monitor-row\"");
  const auto cache = html.find("id=\"cc-card\"");
  const auto requests = html.find("id=\"req-card\"");
  expect(row != std::string::npos && cache != std::string::npos && requests != std::string::npos && row < cache && cache < requests &&
             html.find("\n      <div class=\"monitor-row\" id=\"monitor-row\">") != std::string::npos &&
             html.find("\n      <div class=\"st-card req-card\" id=\"req-card\">") != std::string::npos,
         "the two panels side by side in their row, the recent requests after it at the same level (the whole width)");
  expect(css.find(".monitor-row { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr));") != std::string::npos &&
             css.find(".metrics { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr));") != std::string::npos,
         "even rows: two equal panels, the tiles four across");
  std::size_t tiles = 0;
  for (std::size_t at = js.find("var METRICS = ["); at != std::string::npos && at < js.find("];", js.find("var METRICS = ["));
       at = js.find("{ key: \"", at + 1U)) {
    if (js.compare(at, 8U, "{ key: \"") == 0) ++tiles;
  }
  expect(tiles == 8U, "eight tiles: even rows at four, two and one across (" + std::to_string(tiles) + ")");
}

// §4.3, §4.3a: downloads go to a chosen drive or folder (its free space shown, the folder sent with every start and link
// check), and a model that already has a card - from its download, or the same file added from this PC - is not offered
// for download; since 2026-10-28 (owner: "change the banner from available to Locally Provided vs Downloaded") it is
// shown as what this PC has, not hidden (web_ui_downloads_say_what_is_on_this_pc).
void web_ui_downloads_go_to_a_chosen_folder() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  for (const char* id : {"id=\"downloads-dest\"", "id=\"downloads-dest-path\"", "id=\"downloads-dest-change\""}) {
    expect(html.find(id) != std::string::npos, std::string{"the Downloads section has "} + id);
  }
  expect(js.find("function withFolder(") != std::string::npos && js.find("payload.folder = downloads.folder;") != std::string::npos,
         "the chosen folder rides with each request");
  expect(js.find("var request = { id: card.id };") != std::string::npos && js.find("jsonPost(withFolder(request))") != std::string::npos &&
             js.find("jsonPost(withFolder({ url: offer.url, confirm: true }))") != std::string::npos &&
             js.find("jsonPost(withFolder(body))") != std::string::npos,
         "a start, a link's start and a link check name the folder");
  expect(js.find("var startable = !onPc && oneOf(DL_STARTABLE, card.state);") != std::string::npos,
         "a download whose model is already on this PC is not offered for download");
  expect(js.find("$(\"models-subtab-download\").hidden = !on && !launcher.enabled;") != std::string::npos,
         "the Download sub-tab stays for model files on this PC even with downloads off");
}

// Owner 2026-10-28: "when we point it to a local folder with the model, if it match the card of the model that is
// downloadable, we should change the banner from available to Locally Provided vs Downloaded. That way the user knows
// what they already have in the system." The server's list says it ("have", "have_card": one rule for the page and
// strata-tui); the page shows those entries, no longer hidden, with a badge of their own and the card's name, and offers
// no Download for them.
void web_ui_downloads_say_what_is_on_this_pc() {
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  const std::string css{secure_frontend::find_web_asset("/ui/app.css")->body};
  expect(js.find("function carded(") == std::string::npos && js.find("!carded(card)") == std::string::npos,
         "no download is hidden for having a card");
  expect(js.find("var DL_HAVE = { downloaded: [\"st-badge--generating\", \"Downloaded\"], local: [\"st-badge--reading\", \"Locally provided\"],") !=
             std::string::npos,
         "the two badges: Downloaded and Locally provided");
  expect(js.find("function haveOf(card) {") != std::string::npos && js.find("card.have === \"downloaded\" || card.have === \"local\"") != std::string::npos,
         "the badge comes from the server's have");
  expect(js.find("function onThisPc(card) {") != std::string::npos &&
             js.find("return have !== null && !oneOf(DL_ACTIVE, card.state) && card.state !== \"paused\" ? have : null;") != std::string::npos &&
             js.find("var have = onThisPc(card);") != std::string::npos && js.find("var onPc = have !== null;") != std::string::npos &&
             js.find("head.appendChild(onPc ? haveBadge(have) : other ? haveBadge(\"other\") : dlBadge(card.state));") != std::string::npos,
         "an entry on this PC wears its have badge, an other version its own (a download under way keeps its state's)");
  expect(js.find("\"Already on this PC\" + (card.have_card ? \"; its card: \" + String(card.have_card) : \"\")") != std::string::npos &&
             js.find("if (card.state === \"failed\" && !onPc) {") != std::string::npos,
         "the entry says it is already here and names its card (an old failed attempt is not shown)");
  expect(js.find("var offerVision = !onPc && visionBytes > 0") != std::string::npos, "nor are its vision files offered");
  // another file of the same name on this PC (another revision: other bytes): said, with its card, and still offered
  expect(js.find("function otherVersion(card) {") != std::string::npos &&
             js.find("return card.have === \"other\" && !oneOf(DL_ACTIVE, card.state) && card.state !== \"paused\";") != std::string::npos &&
             js.find("other: [\"st-badge--queued\", \"Other version on this PC\"]") != std::string::npos &&
             js.find("\"Another version of this file is on this PC\"") != std::string::npos,
         "a same-named file of another size is an other version, shown with its card");
  expect(js.find("var startable = !onPc && oneOf(DL_STARTABLE, card.state);") != std::string::npos,
         "and its download is still offered (an other version is not onPc)");
  expect(css.find(".dl-card[data-have=") != std::string::npos, "an entry on this PC is styled");
}

// Owner 2026-10-28: "To make the download catalog easier to view and not so busy, maybe we can put accordion control so
// that user can expand per related section instead of seeing a busy page". Each family group is an accordion section:
// a real button (aria-expanded, aria-controls) naming the family and what it holds; closed by default unless it holds a
// download under way, a paused or failed one, or a link just added; what the viewer opened is remembered for the tab
// (the page's guarded sessionStorage helpers: the threat model pins no localStorage in app.js), the page working without it.
void web_ui_downloads_groups_are_accordions() {
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  const std::string css{secure_frontend::find_web_asset("/ui/app.css")->body};
  expect(js.find("var toggle = el(\"button\", \"dl-group__head\");") != std::string::npos && js.find("toggle.type = \"button\";") != std::string::npos &&
             js.find("toggle.setAttribute(\"aria-expanded\", open ? \"true\" : \"false\");") != std::string::npos &&
             js.find("toggle.setAttribute(\"aria-controls\", panel.id);") != std::string::npos,
         "each group's header is a button that says whether it is open and what it opens");
  expect(js.find("panel.hidden = !open;") != std::string::npos && js.find("panel.setAttribute(\"role\", \"region\");") != std::string::npos &&
             js.find("panel.setAttribute(\"aria-labelledby\", toggle.id);") != std::string::npos,
         "its cards are a region labelled by the header, hidden while closed");
  expect(js.find("function groupSummary(members) {") != std::string::npos &&
             js.find("fmt(members.length) + (members.length === 1 ? \" model\" : \" models\")") != std::string::npos &&
             js.find("\" on this PC\"") != std::string::npos && js.find("\" downloading\"") != std::string::npos &&
             js.find("var here = count(function (card) { return onThisPc(card) !== null; });") != std::string::npos,
         "the header sums the group up: models, downloads under way, those on this PC (as its cards show them)");
  expect(js.find("function groupWantsOpen(members) {") != std::string::npos &&
             js.find("oneOf(DL_ACTIVE, card.state) || card.state === \"paused\" || (card.state === \"failed\" && onThisPc(card) === null) ||") !=
                 std::string::npos &&
             js.find("card.id === downloads.justAdded") != std::string::npos,
         "a group holding anything under way, paused, failed (and not on this PC) or just added opens itself");
  expect(js.find("var DL_OPEN_SLOT = \"strata.downloads.open\";") != std::string::npos &&
             js.find("var saved = load(DL_OPEN_SLOT, null);") != std::string::npos &&
             js.find("function saveOpenGroups() { save(DL_OPEN_SLOT, downloads.open); }") != std::string::npos &&
             js.find("localStorage") == std::string::npos,
         "the groups opened are remembered for the tab through the guarded storage helpers, never localStorage");
  expect(js.find("downloads.justAdded = answer.body && typeof answer.body.id === \"string\" ? answer.body.id : \"\";") != std::string::npos,
         "a link just started opens its group");
  expect(css.find(".dl-group__head {") != std::string::npos && css.find(".dl-group__head:focus-visible") != std::string::npos &&
             css.find(".dl-group__head[aria-expanded=\"true\"] .dl-group__chevron") != std::string::npos,
         "the header is styled, with a focus ring and a chevron that turns");
  const auto phone = css.find("@media (max-width: 640px) {");
  const std::string block = phone == std::string::npos ? std::string{} : css.substr(phone, css.find("\n}", phone) - phone);
  expect(block.find(".dl-group__head") != std::string::npos, "the header has a phone-width rule");
}

// Owner 2026-10-28: "we should also make prominent the device and download location selection so that it does not look
// just a bunch of text". Where downloads go is a panel at the top of the list: "Download to", the drive and the folder
// (the path in a chip that shortens in its middle), a free-space gauge (used, free, total), a real Change button that
// opens the folder picker, the server's folder marked as the default, and a warning (in words) when the free space
// cannot hold the largest download queued, under way, paused or confirmed. The picker and withFolder are as before.
void web_ui_downloads_destination_is_a_panel() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  const std::string css{secure_frontend::find_web_asset("/ui/app.css")->body};
  const auto panel = html.find("<section class=\"dl-dest\" id=\"downloads-dest\" aria-labelledby=\"downloads-dest-title\">");
  expect(panel != std::string::npos && panel < html.find("id=\"downloads-list\""), "a destination panel heads the list");
  expect(html.find("<h3 class=\"dl-dest__title\" id=\"downloads-dest-title\">Download to</h3>") != std::string::npos,
         "it is headed Download to");
  for (const char* id : {"id=\"downloads-dest-drive\"", "id=\"downloads-dest-path\"", "id=\"downloads-dest-default\"", "id=\"downloads-dest-gauge\"",
                         "id=\"downloads-dest-gauge-bar\"", "id=\"downloads-disk\"", "id=\"downloads-dest-warn\"", "id=\"downloads-dest-reset\""}) {
    expect(html.find(id) != std::string::npos, std::string{"the panel has "} + id);
  }
  expect(html.find("<button type=\"button\" class=\"st-btn st-btn--secondary st-btn--sm\" id=\"downloads-dest-change\" "
                   "aria-describedby=\"downloads-dest-title\">Change&hellip;</button>") != std::string::npos,
         "a real Change button");
  expect(html.find("role=\"meter\"") != std::string::npos && js.find("gauge.setAttribute(\"aria-valuenow\", String(used));") != std::string::npos &&
             js.find("gauge.setAttribute(\"aria-valuetext\", ") != std::string::npos,
         "the gauge is a meter with its value in words");
  expect(js.find("function middleChip(node, path) {") != std::string::npos && js.find("node.title = path;") != std::string::npos,
         "the path shortens in its middle and keeps the whole path as its title");
  expect(js.find("\"The server's downloads folder (default)\"") != std::string::npos, "the server's folder is the default");
  expect(js.find("Number(disk.total_bytes) > 0 ? disk.free_bytes : null;") != std::string::npos,
         "a drive the server could not read (0 of 0 bytes) has no free space known, so no false warning");
  expect(js.find("function sameFolder(a, b) {") != std::string::npos && js.find("sameFolder(card.folder, here)") != std::string::npos,
         "the downloads pending there are found whatever the path's case or trailing separator");
  expect(js.find("function destinationShort() {") != std::string::npos &&
             js.find("\"Not enough room for \" + short.name + \": \" + dgb(short.bytes) + \" GB needed, \" + dgb(free) + \" GB free\"") !=
                 std::string::npos &&
             js.find("box.setAttribute(\"data-short\", short ? \"true\" : \"false\");") != std::string::npos,
         "a warning in words when the largest pending download cannot fit");
  expect(js.find("$(\"downloads-dest-change\").addEventListener(\"click\", chooseDestination);") != std::string::npos &&
             js.find("downloads.folder = picked.folder;") != std::string::npos,
         "Change opens the folder picker, as before");
  expect(css.find(".dl-dest {") != std::string::npos && css.find(".dl-dest__path") != std::string::npos &&
             css.find(".dl-dest[data-short=\"true\"]") != std::string::npos,
         "the panel, its chip and its warning state are styled");
  const auto phone = css.find("@media (max-width: 640px) {");
  const std::string block = phone == std::string::npos ? std::string{} : css.substr(phone, css.find("\n}", phone) - phone);
  expect(block.find(".dl-dest") != std::string::npos, "the panel has a phone-width rule");
}

// Model downloads plan Task 5 (spec sections 3-4, O1-O4): a Downloads section on the Models tab.
// It is in the page from the start but hidden, and shown only when GET /v1/strata/downloads
// answers: a server with models.downloads off answers 404 and the section stays hidden (the tab is
// exactly the launcher's); a 403 (a remote client without models.control_remote) shows the
// reason and the switch, like the Models tab's own note. A disk line heads the cards (O2), which
// are grouped by where they come from (O1), and a paste box takes one Hugging Face link (O3).
// Owner 2026-10-23: "tab out the pages for the models that can be selected to be started and those
// that are available to download so it's not too busy"; "organize them into sections like based on
// similarities". The Models tab has two sub-tabs (Start a model / Download models), and the download
// cards are grouped into family sections.
void web_ui_models_tab_subtabs_and_family_groups() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  const std::string css{secure_frontend::find_web_asset("/ui/app.css")->body};
  // Two sub-tab controls, and the two panes they switch between, inside the Models view.
  const auto view = html.find("id=\"view-models\"");
  for (const char* id : {"id=\"models-subtab-start\"", "id=\"models-subtab-download\"", "id=\"models-pane-start\"", "id=\"models-pane-download\""}) {
    const auto at = html.find(id);
    expect(at != std::string::npos && at > view && at < html.find("id=\"view-monitor\""), std::string{"the Models view has "} + id);
  }
  expect(html.find(">Start a model<") != std::string::npos && html.find(">Download models<") != std::string::npos,
         "the sub-tabs are labelled Start a model and Download models");
  // The launcher cards live in the start pane, the Downloads section in the download pane.
  expect(html.find("id=\"models-pane-start\"") < html.find("id=\"model-list\"") &&
             html.find("id=\"model-list\"") < html.find("id=\"models-pane-download\""),
         "the launcher card list is in the start pane");
  expect(html.find("id=\"models-pane-download\"") < html.find("id=\"downloads\""),
         "the Downloads section is in the download pane");
  // Switching shows one pane and hides the other.
  expect(js.find("function showModelsSubtab(") != std::string::npos &&
             js.find("$(\"models-pane-start\").hidden =") != std::string::npos &&
             js.find("$(\"models-pane-download\").hidden =") != std::string::npos,
         "a sub-tab switcher shows one pane and hides the other");
  expect(js.find("showModelsSubtab(\"start\")") != std::string::npos && js.find("showModelsSubtab(\"download\")") != std::string::npos,
         "the two sub-tab buttons select their pane");
  // The Download sub-tab button is hidden when the server offers no downloads (404).
  expect(js.find("$(\"models-subtab-download\").hidden") != std::string::npos,
         "the Download sub-tab is hidden when downloads are off");
  // The download cards are grouped into family sections (friendly headings), stable order.
  expect(js.find("var DL_FAMILIES = [") != std::string::npos, "a family group table");
  for (const char* fam : {"[\"qwen\", \"Qwen3.8-Flash-Next\"]", "[\"unsloth\", \"Qwen3.8-Flash-Next (Unsloth)\"]",
                          "[\"gyro\", \"Qwen3.8-Flash-Next (Gyro)\"]", "[\"coder\", \"Qwen3.8-Flash-Next Coder\"]",
                          "[\"qwen36\", \"Qwen3.6-35B-A3B\"]", "[\"swift\", \"Swift 1.5\"]",
                          "[\"glm53\", \"GLM-5.3-Flash (Project Maya)\"]", "[\"custom\", \"Your downloads\"]",
                          // owner 2026-10-28: Maya's DeepSeek quants and MiniMax's, its licence named in its heading
                          "[\"deepseek\", \"DeepSeek-V4.1-Flash (Project Maya)\"]",
                          "[\"minimax\", \"MiniMax-M3.1-Flash (Project Maya; MiniMax Community License)\"]",
                          // owner 2026-10-28: our own pins - Gemma 4 with its MTP draft file, flint, raon-1-mini
                          "[\"gemma4\", \"Gemma 4 (Google)\"]", "[\"flint\", \"flint (RecursiveSI)\"]",
                          "[\"raon1\", \"Raon-1-mini (KRAFTON)\"]"}) {
    expect(js.find(fam) != std::string::npos, std::string{"a friendly family heading: "} + fam);
  }
  expect(js.find("card.family") != std::string::npos, "the cards are grouped by their family field");
  // The sub-tabs stay usable on a phone.
  const auto phone = css.find("@media (max-width: 640px) {");
  const std::string block = phone == std::string::npos ? std::string{} : css.substr(phone, css.find("\n}", phone) - phone);
  expect(css.find(".models-subtab") != std::string::npos, "the sub-tabs are styled");
  expect(block.find(".models-subtab") != std::string::npos, "the sub-tabs have a phone-width rule");
}

// Owner 2026-10-23: "the downloads must be matched to a current settings we have available ... the
// user selects [a profile]"; "check if the model is a multipart gguf ... paste the other links".
// The paste box gains a profile picker (filled from the list's "profiles") and a multipart parts
// area; the link check sends the chosen profile and the extra part links.
void web_ui_downloads_profile_picker_and_multipart() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  for (const char* id : {"id=\"downloads-profile\"", "id=\"downloads-parts\""}) {
    expect(html.find(id) != std::string::npos, std::string{"the paste box has "} + id);
  }
  expect(html.find("<label class=\"st-label\" for=\"downloads-profile\">Run it as</label>") != std::string::npos,
         "the profile picker has a label");
  expect(js.find("function populateProfiles(") != std::string::npos && js.find("downloads.data.profiles") != std::string::npos,
         "the profile picker is filled from the list's profiles");
  expect(js.find("body.profile = profile;") != std::string::npos && js.find("body.extra_urls = extra;") != std::string::npos,
         "the link check sends the chosen profile and the extra part links");
  expect(js.find("function renderParts(") != std::string::npos && js.find("function gatherParts(") != std::string::npos,
         "the multipart parts are gathered and rendered");
  expect(js.find("!fits || !complete || downloads.acting") != std::string::npos,
         "Download waits until every shard of a split model is pasted");
}

void web_ui_models_tab_has_a_downloads_section() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  const auto section = html.find("<div class=\"st-card downloads\" id=\"downloads\" hidden>");
  expect(section != std::string::npos && section > html.find("id=\"view-models\"") && section < html.find("id=\"view-monitor\""),
         "a Downloads section inside the Models view, hidden until the server offers it");
  for (const char* id : {"id=\"downloads-refresh\"", "id=\"downloads-denied\"", "id=\"downloads-body\"", "id=\"downloads-disk\"",
                         "id=\"downloads-list\"", "id=\"downloads-link-form\"", "id=\"downloads-url\"", "id=\"downloads-check\"",
                         "id=\"downloads-link-error\"", "id=\"downloads-confirm\""}) {
    expect(html.find(id) != std::string::npos, std::string{"the Downloads section has "} + id);
  }
  expect(html.find("<label class=\"st-label\" for=\"downloads-url\">Hugging Face link to a .gguf file</label>") != std::string::npos &&
             html.find("id=\"downloads-check\">Check</button>") != std::string::npos,
         "a paste box for a Hugging Face .gguf link, with a Check button");
  expect(html.find("id=\"downloads-url\" type=\"text\"") != std::string::npos && html.find("name=\"url\"") == std::string::npos,
         "the link field has no name, so no form can ever put it in a URL");
  const auto denied = html.find("id=\"downloads-denied\"");
  expect(denied != std::string::npos && html.find("control_remote", denied) < html.find("</p>", denied),
         "the downloads 403 note names the switch that allows other machines");
  expect(js.find("api(\"/v1/strata/downloads\")") != std::string::npos, "the download list is asked through the keyed API helper");
  expect(js.find("if (name === \"models\") { renderModels(false); refreshLauncher(); refreshDownloads(); showModelsSubtab(modelsSubtab); }") != std::string::npos,
         "opening the Models tab reads the downloads too and applies the sub-tab");
  expect(js.find("if (response.status === 404) { setDownloadsShown(false); return; }") != std::string::npos &&
             js.find("$(\"downloads\").hidden = !on;") != std::string::npos,
         "a 404 (downloads off) keeps the section hidden");
  expect(js.find("if (response.status === 403) { setDownloadsDenied(); return; }") != std::string::npos &&
             js.find("$(\"downloads-denied\").hidden = false;") != std::string::npos && js.find("$(\"downloads-body\").hidden = true;") != std::string::npos,
         "a 403 shows the section with the reason instead of the cards");
  expect(js.find("\"Free space on \" + root") != std::string::npos && js.find("\" GB of \" + dgb(disk.total_bytes) + \" GB\"") != std::string::npos &&
             js.find("function dgb(bytes) { return fmt((Number(bytes) || 0) / 1e9, 1); }") != std::string::npos,
         "a disk line: the downloads drive's free space of its total, in decimal GB with one decimal");
for (const char* group : {"[\"qwen\", \"Qwen3.8-Flash-Next\"]", "[\"glm53\", \"GLM-5.3-Flash (Project Maya)\"]",
                            "[\"custom\", \"Your downloads\"]"}) {
    expect(js.find(group) != std::string::npos, std::string{"the download cards are grouped by family: "} + group);
  }
  expect(js.find("if (card.runnable === false && card.note)") != std::string::npos, "a card that cannot run here shows its note");
}

// The Downloads cards drive the download routes, every call through the keyed helper: Download
// and Cancel post the card's id; a card that does not fit says so with both figures and cannot
// start; a refusal (507 insufficient_storage too) is the server's sentence on its card; a progress
// bar and line for the running transfer; a 2 s poll while a card is queued, downloading,
// verifying or preparing, and none otherwise; the model list read again when a card turns ready;
// a pasted link pre-checked (O4: https on huggingface.co, a .gguf file before any query) before
// the server is asked, then shown and confirmed.
// Owner: "Have a download pause and resume functionality". Pause while a card is queued or
// downloading; a paused card keeps its bar and figures, offers Resume (the start route: the server
// continues from the partial files) and Cancel (which deletes them, and says so first).
void web_ui_downloads_pause_and_resume() {
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  expect(js.find("paused: [\"st-badge--queued\", \"Paused\"]") != std::string::npos, "a paused card has its badge");
  expect(js.find("api(\"/v1/strata/downloads/pause\", jsonPost({ id: card.id }))") != std::string::npos,
         "Pause posts the card's id to the pause route");
  const auto pause_at = js.find("var pause = el(\"button\", \"st-btn st-btn--secondary st-btn--sm\", \"Pause\");");
  expect(pause_at != std::string::npos && js.rfind("if (card.state === \"queued\" || card.state === \"downloading\") {", pause_at) != std::string::npos &&
             js.find("pause.addEventListener(\"click\", function () { pauseDownload(card); });") != std::string::npos,
         "Pause is offered while a card is queued or downloading");
  expect(js.find("var resume = el(\"button\", \"st-btn st-btn--primary st-btn--sm\", \"Resume\");") != std::string::npos &&
             js.find("resume.addEventListener(\"click\", function () { startDownload(card); });") != std::string::npos &&
             js.find("resume.disabled = lacksRoom(card) || downloads.acting;") != std::string::npos &&
             js.find("if (!downloads.folder) { return card.fits === false; }") != std::string::npos,
         "a paused card offers Resume, which is a start, and only when the rest fits");
  expect(js.find("var DL_CANCELLABLE = [\"queued\", \"downloading\", \"paused\"];") != std::string::npos &&
             js.find("if (oneOf(DL_CANCELLABLE, card.state)) {") != std::string::npos,
         "Cancel is offered while a card is queued, downloading or paused");
  expect(js.find("\"Cancel the download of \" + card.name + \"? The part downloaded so far is deleted.\"") != std::string::npos,
         "Cancel says that the partial download is deleted");
  expect(js.find("if (card.state === \"downloading\" || card.state === \"paused\") {") != std::string::npos &&
             js.find("\"Paused at \" + transferText(figures, null) + \"; Resume continues from there\"") != std::string::npos,
         "a paused card keeps its bar and says how far it got");
  expect(js.find("if (card.state === \"paused\" && lacksRoom(card)) { box.appendChild(el(\"p\", \"msg-warn\", noRoomText(card.needed_bytes))); }") !=
             std::string::npos,
         "a paused card whose rest does not fit says so");
  expect(js.find("toast(\"info\", \"Download paused\", card.name + \": Resume continues from where it stopped\");") != std::string::npos,
         "a pause is confirmed");
  expect(js.find("(parsed.reason ? \": \" + parsed.reason : \"\")") != std::string::npos,
         "a refused link shows the server's fixed reason");
  const std::string css{secure_frontend::find_web_asset("/ui/app.css")->body};
  expect(css.find(".dl-card[data-state=\"paused\"]") != std::string::npos, "a paused card is styled");
}

// Owner: "On mobile the api login is not showing up, we need to make sure it is mobile
// responsive". Signed in, the header (brand, four tabs, New chat, the pill, the theme button)
// needs about 480 px; on a phone it takes two rows, the tabs sharing the full width below, so
// the page never lays out wider than the screen.
void web_ui_header_fits_a_phone() {
  const std::string css{secure_frontend::find_web_asset("/ui/app.css")->body};
  const auto phone = css.find("@media (max-width: 640px) {");
  expect(phone != std::string::npos, "a phone-width block");
  const std::string block = phone == std::string::npos ? std::string{} : css.substr(phone, css.find("\n}", phone) - phone);
  expect(block.find(".st-header { flex-wrap: wrap; height: auto; padding: var(--st-s-2) var(--st-s-3); gap: var(--st-s-2); }") !=
             std::string::npos,
         "the header wraps onto a second row");
  expect(block.find(".st-header .st-tabs { order: 3; flex: 1 0 100%; margin-left: 0; }") != std::string::npos,
         "the tabs take their own full-width row");
  expect(block.find(".st-tab { flex: 1; justify-content: center; padding: 0 6px; }") != std::string::npos,
         "the four tabs share that row");
  const std::string html{secure_frontend::find_web_asset("/")->body};
  expect(html.find("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">") != std::string::npos,
         "the page lays out at the device's width");
  // owner 2026-10-27: the brand is "M-Strata"; its hyphen must not break it over two lines in a narrow header
  expect(css.find(".st-header__brand { white-space: nowrap; flex: none; }") != std::string::npos, "the brand stays on one line");
  // owner 2026-10-28 ("make sure the front end is mobile responsive"), from a 375 px check of every view: a folder path
  // takes its own line in a smaller font with its button below; the About's engines are stacked, each row labelled
  // (the table hid Built from and Size behind a sideways scroll); toasts keep even margins
  expect(block.find(".model-folder__row { flex-direction: column; align-items: stretch; gap: var(--st-s-2); }") != std::string::npos &&
             block.find(".model-folder__row code { font-size: var(--st-fs-sm); }") != std::string::npos &&
             block.find(".model-folder__row .st-btn { align-self: flex-start; }") != std::string::npos,
         "a folder path on its own line, its button below");
  expect(block.find(".about-engines thead { display: none; }") != std::string::npos &&
             block.find(".about-engines td[data-label]::before { content: attr(data-label); ") != std::string::npos,
         "the engines stack, each value labelled");
  expect(block.find(".st-toasts { left: var(--st-s-3); right: var(--st-s-3); bottom: var(--st-s-3); }") != std::string::npos,
         "toasts keep even margins");
  // 2026-10-28, the Settings tab (a fifth tab): the one-row header needs about 800 px with the tab icons, so between a
  // phone and that width the icons go and the tabs and the header pad less (a 764 px check scrolled sideways)
  const auto middle = css.find("@media (max-width: 900px) {");
  const std::string between = middle == std::string::npos ? std::string{} : css.substr(middle, css.find("\n}", middle) - middle);
  expect(between.find(".st-tab .st-icon { display: none; }") != std::string::npos &&
             between.find(".st-tab { padding: 0 10px; }") != std::string::npos &&
             between.find(".st-header { padding: 0 var(--st-s-4); }") != std::string::npos,
         "five tabs fit one row down to the phone width");
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  expect(js.find("cell.setAttribute(\"data-label\", label);") != std::string::npos, "each engine cell carries its column's name");
}

// Multi-GPU like upstream (owner 2026-10-28): a card's GPUs choice ("devices") is one box per GPU of this PC, as the
// About lists them (nvidia-smi's numbers), none ticked auto, one at most on a single-GPU engine; hidden on a one-GPU PC,
// where nothing is sent.
void web_ui_offers_the_gpus_of_this_pc() {
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  expect(js.find("case \"devices\": return devicesControl(option, value);") != std::string::npos &&
             js.find("return { value: picked.length ? picked.join(\",\") : \"auto\" };") != std::string::npos &&
             js.find("if (gpus.length < 2) { return { skip: true }; }") != std::string::npos &&
             js.find("field.hidden = control.hidden === true;") != std::string::npos,
         "the GPUs choice: a box per GPU, auto when none, not sent on a one-GPU PC");
  expect(js.find("setHostGpus(d && d.hardware && Array.isArray(d.hardware.gpus) ? d.hardware.gpus : []);") != std::string::npos &&
             js.find("fact(hardware, \"GPU \" + g.index,") != std::string::npos,
         "this PC's GPUs come from the About, which lists each");
}

void web_ui_downloads_start_cancel_poll_and_check_links() {
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  expect(js.find("function jsonPost(payload) { return { method: \"POST\", headers: { \"Content-Type\": \"application/json\" }, "
                 "body: JSON.stringify(payload) }; }") != std::string::npos,
         "the download routes take JSON bodies");
  expect(js.find("var request = { id: card.id };") != std::string::npos &&
             js.find("api(\"/v1/strata/downloads/start\", jsonPost(withFolder(request)))") != std::string::npos,
         "Download posts the card's id (and the chosen folder) to the start route");
  // vision for cards (spec 2026-10-28): the Vision files box rides with the start where the entry has picture files
  expect(js.find("if ((Number(card.vision_bytes) || 0) > 0) { request.vision = wantsVision(card); }") != std::string::npos &&
             js.find("\"Vision files (+\" + dgb(visionBytes) + \" GB): read pictures with this model\"") != std::string::npos,
         "Download sends the Vision files box of an entry that has them");
  expect(js.find("api(\"/v1/strata/downloads/cancel\", jsonPost({ id: card.id }))") != std::string::npos,
         "Cancel posts the card's id to the cancel route");
  expect(js.find("var body = { url: url };") != std::string::npos &&
             js.find("api(\"/v1/strata/downloads/link\", jsonPost(withFolder(body)))") != std::string::npos,
         "Check posts the pasted link (with the chosen profile and any extra parts) to the link route");
  expect(js.find("api(\"/v1/strata/downloads/start\", jsonPost(withFolder({ url: offer.url, confirm: true })))") != std::string::npos,
         "a confirmed link starts with confirm: true");
  expect(js.find("var noRoom = startable && lacksRoom(card);") != std::string::npos &&
             js.find("download.disabled = noRoom || downloads.acting;") != std::string::npos,
         "a card that does not fit cannot start");
  expect(js.find("if (noRoom) { box.appendChild(el(\"p\", \"msg-warn\", noRoomText(card.needed_bytes))); }") != std::string::npos &&
             js.find("return \"Insufficient disk space: \" + dgb(needed) + \" GB needed, \" + (free === null ? \"-\" : dgb(free)) + \" GB free\";") !=
                 std::string::npos,
         "a card that does not fit says how much it needs and how much is free");
  expect(js.find("if (card.state === \"queued\" || card.state === \"downloading\") {") != std::string::npos,
         "Cancel is offered while a card is queued or downloading");
  expect(js.find("if (answer.error) { downloads.errors[card.id] = answer.error; }") != std::string::npos &&
             js.find("var refused = downloads.errors[card.id];") != std::string::npos,
         "a refusal is shown on its card");
  expect(js.find("fill.style.width = ") != std::string::npos && js.find("\" of \" + dgb(figures.total) + \" GB (\" + pct + \"%)\"") != std::string::npos &&
             js.find("\" MB/s\"") != std::string::npos && js.find("\"ETA \" + clock(active.eta_s)") != std::string::npos,
         "the running transfer has a bar and a line: GB done of the total, the percentage, the rate and the ETA");
  expect(js.find("\"Verifying: ") != std::string::npos && js.find("\"Preparing: ") != std::string::npos,
         "verifying and preparing are said in words");
  expect(js.find("var DL_ACTIVE = [\"queued\", \"downloading\", \"verifying\", \"preparing\"];") != std::string::npos &&
             js.find("downloads.timer = window.setTimeout(pollDownloads, 2000);") != std::string::npos &&
             js.find("if (!downloadsActive(parsed)) { stopDownloadsPoll(); return; }") != std::string::npos,
         "the page asks again every 2 s while a card is active, and stops when none is");
  expect(js.find("if (card.state === \"ready\") { readyNow.push(card); }") != std::string::npos &&
             js.find("if (readyNow.length) { refreshLauncher();") != std::string::npos,
         "a card that turns ready reads the model list again, so its new model card appears");
  expect(js.find(R"(var HF_LINK = /^https:\/\/huggingface\.co\//;)") != std::string::npos &&
             js.find(R"(var GGUF_FILE = /\.gguf$/i;)") != std::string::npos &&
             js.find("if (!GGUF_FILE.test(url.split(/[?#]/)[0]))") != std::string::npos,
         "a pasted link must be https on huggingface.co and name a .gguf file (before any query)");
  const auto precheck = js.find("var problem = linkProblem(url);");
  expect(precheck != std::string::npos &&
             js.find("if (problem) { hideConfirm(); showLinkError(problem); return; }", precheck) < js.find("api(\"/v1/strata/downloads/link\"", precheck),
         "a link that fails the pre-check is refused without asking the server");
  expect(js.find("row(\"Repository\", ") != std::string::npos && js.find("row(\"Commit\", commit ? commit.slice(0, 7) : \"-\", commit);") != std::string::npos &&
             js.find("go.disabled = !fits || !complete || downloads.acting;") != std::string::npos,
         "the confirm block shows the repository and the short commit, and cannot start what does not fit or is missing a shard");
}

// The chat follows the running model: the header names it, the composer is off with a link to
// the Models tab when none runs, /v1/models is asked again when the running card changes, and
// the launcher's chat refusals read as plain sentences.
void web_ui_chat_follows_the_running_model() {
  const std::string html{secure_frontend::find_web_asset("/")->body};
  const std::string js{secure_frontend::find_web_asset("/ui/app.js")->body};
  expect(html.find("id=\"header-model\"") != std::string::npos, "the header has a place for the running model's name");
  expect(html.find("No model is running &mdash; open the Models tab") != std::string::npos &&
             html.find("id=\"no-model-open\"") != std::string::npos,
         "the composer's notice and its button to the Models tab");
  expect(js.find("$(\"no-model-open\").addEventListener(\"click\", function () { showView(\"models\"); });") != std::string::npos,
         "the notice's button opens the Models tab");
  expect(js.find("$(\"input\").disabled = !on;") != std::string::npos && js.find("$(\"send-btn\").disabled = !on;") != std::string::npos,
         "with no model running the composer is disabled");
  expect(js.find("$(\"header-model\").textContent") != std::string::npos, "the header shows the running model's name");
  expect(js.find("refreshChatModel(") != std::string::npos && js.find("listModels()") != std::string::npos,
         "the chat's model is read again from /v1/models when the running card changes");
  for (const char* code : {"model_switching:", "no_model_running:", "model_not_running:"}) {
    expect(js.find(code) != std::string::npos, std::string{"a friendly chat error for "} + code);
  }
  expect(js.find("CHAT_ERRORS[code]") != std::string::npos, "the chat's error path uses the friendly errors");
}

void keyless_router_answers_without_a_key() {
  auto keyless = page_router(true, false);
  const auto models = bare(keyless, "GET", "/v1/models");
  expect(models.status == 200 && models.body.find("fake-model") != std::string::npos, "keyless: /v1/models without a key");
  expect(bare(keyless, "GET", "/health").status == 200, "keyless: /health without a key");
  expect(get(keyless, "/v1/models").status == 200, "keyless: a request that still sends a key is served too");
  const auto rebound =
      secure_frontend::handle_http_request("GET /v1/models HTTP/1.1\r\nHost: evil.example:8080\r\n\r\n", keyless, kRouterLimits);
  expect(rebound.status == 403, "keyless: a foreign Host is still refused (DNS rebinding)");
  expect(bare(keyless, "GET", "/v1/models", "Origin: https://evil.example\r\n").status == 403,
         "keyless: a foreign Origin is still refused (another site's page)");
  expect(bare(keyless, "POST", "/v1/chat/completions", "").status != 401, "keyless: chat needs no key");
  auto keyed = page_router(true, true);
  expect(bare(keyed, "GET", "/v1/models").status == 401, "control: the same router with require_key answers 401");
}

// --- The Monitor (listener.monitor; session log step 16) ---------------------------------
// GET /metrics is an API route: the key first, then the switch. Off (no provider) it is not
// served at all; on, it answers the provider's JSON, asked once per request.
secure_frontend::ApiRouter metrics_router(secure_frontend::MetricsProvider metrics) {
  secure_frontend::SecretBytes secret;
  (void)secret.assign(kRouterToken);
  return secure_frontend::ApiRouter({
      .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address("127.0.0.1"), 8080, {}, {}),
      .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
      .model_id = "fake-model", .owned_by = "local", .chat_body_limit = 49152,
      .health = [] { return secure_frontend::HealthView{true, "ready", ""}; },
      .chat = {}, .metrics = std::move(metrics)});
}

void metrics_route_needs_the_key_and_the_switch() {
  auto off = metrics_router({});
  expect(bare(off, "GET", "/metrics").status == 401, "the Monitor's route needs the key");
  expect(get(off, "/metrics").status == 404, "off: with the key it is not served at all");
  const std::string off_key = "Authorization: Bearer " + std::string{kRouterToken} + "\r\n";
  expect(bare(off, "POST", "/metrics", off_key).status == 404,
         "off: no method reveals the route either (EXT-01: upstream's /metrics is 404, not 405)");
  int calls = 0;
  auto on = metrics_router([&calls] {
    ++calls;
    return std::string{"{\"live\":{\"state\":\"idle\"}}\n"};
  });
  expect(bare(on, "GET", "/metrics").status == 401 && calls == 0, "on: the key is still asked first; the provider is untouched");
  const auto served = get(on, "/metrics");
  expect(served.status == 200 && served.body == "{\"live\":{\"state\":\"idle\"}}\n" && calls == 1,
         "on: the provider's JSON, asked once (" + std::to_string(served.status) + ")");
  expect(served.header_value("Content-Type") == std::optional<std::string_view>{"application/json; charset=utf-8"},
         "served as JSON");
  const std::string key = "Authorization: Bearer " + std::string{kRouterToken} + "\r\n";
  expect(bare(on, "POST", "/metrics", key).status == 405, "GET only");
  expect(get(on, "/metrics/prometheus").status == 404 && get(on, "/Metrics").status == 404, "the exact target only");
  auto throwing = metrics_router([]() -> std::string { throw std::runtime_error("monitor"); });
  expect(get(throwing, "/metrics").status == 500, "a provider that throws is an internal error, nothing else");
}

// The page's About (owner 2026-10-27: the build of our engines, this PC's hardware): GET /v1/strata/about is an API route
// like /metrics - the key first; served from its provider, asked once per request; without one, not a route at all.
void about_route_needs_the_key_and_answers_its_provider() {
  secure_frontend::SecretBytes secret;
  (void)secret.assign(kRouterToken);
  int calls = 0;
  auto on = secure_frontend::ApiRouter({
      .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address("127.0.0.1"), 8080, {}, {}),
      .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
      .model_id = "fake-model", .owned_by = "local", .chat_body_limit = 49152,
      .health = [] { return secure_frontend::HealthView{true, "ready", ""}; },
      .chat = {}, .about = [&calls] {
        ++calls;
        return std::string{"{\"build\":null,\"hardware\":{}}\n"};
      }});
  expect(bare(on, "GET", "/v1/strata/about").status == 401 && calls == 0, "the key is asked first");
  const auto served = get(on, "/v1/strata/about");
  expect(served.status == 200 && served.body == "{\"build\":null,\"hardware\":{}}\n" && calls == 1 &&
             served.header_value("Content-Type") == std::optional<std::string_view>{"application/json; charset=utf-8"},
         "the provider's JSON, asked once");
  const std::string key = "Authorization: Bearer " + std::string{kRouterToken} + "\r\n";
  expect(bare(on, "POST", "/v1/strata/about", key).status == 405, "GET only");
  auto off = metrics_router({});
  expect(get(off, "/v1/strata/about").status == 404, "without a provider it is not served");
}
}  // namespace

// --- The model launcher's routes (plan 2026-10-23 Task 4) ---------------------------------

// A launcher-mode router: the served model comes from a provider and the control routes are
// handled by `control` (each call recorded in `calls`).
secure_frontend::ApiRouter launcher_router(std::vector<std::string>* calls, std::optional<std::string> running = "Gemma-4",
                                           secure_frontend::HealthView health = {true, "ready", ""}, bool control = true) {
  secure_frontend::SecretBytes secret;
  (void)secret.assign(kRouterToken);
  secure_frontend::ModelControl handlers;
  if (control) {
    handlers.list = [calls](const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
      calls->push_back(std::string{"list "} + (context.loopback_peer ? "loopback" : "remote"));
      return secure_frontend::HttpResponse{.status = 200, .headers = {}, .body = "{\"object\":\"list\"}\n"};
    };
    handlers.start = [calls](std::string_view body, const secure_frontend::RequestContext&) -> secure_frontend::RouteResult {
      calls->push_back("start " + std::string{body});
      return secure_frontend::HttpResponse{.status = 202, .headers = {}, .body = "{\"state\":\"starting\"}\n"};
    };
    handlers.stop = [calls](const secure_frontend::RequestContext&) -> secure_frontend::RouteResult {
      calls->push_back("stop");
      return secure_frontend::HttpResponse{.status = 202, .headers = {}, .body = "{\"state\":\"stopped\"}\n"};
    };
  }
  return secure_frontend::ApiRouter({
      .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address("127.0.0.1"), 8080, {}, {}),
      .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
      .model_id = "", .owned_by = "local", .chat_body_limit = 49152,
      .health = [health] { return health; },
      .chat = {},
      .served = [running] {
        secure_frontend::ServedModels served;
        served.id = running;
        if (running) served.aliases = {"gemma"};
        served.extension = "[{\"id\":\"Gemma-4\",\"state\":\"running\"},{\"id\":\"Qwen\",\"state\":\"stopped\"}]";
        return served;
      },
      .control = std::move(handlers)});
}

secure_frontend::HttpResponse request(secure_frontend::ApiRouter& router, const std::string& method, const std::string& target,
                                      const std::string& body = "", bool keyed = true, bool loopback = true,
                                      const std::string& content_type = "application/json") {
  auto head = head_of(method, target);
  if (!keyed) head.fields = {{"Host", "127.0.0.1:8080"}};
  if (!body.empty() || method == "POST") {
    head.fields.emplace_back("Content-Type", content_type);
    head.content_length = body.size();
  }
  const auto decision = router.authorize(head);
  if (decision.early) return *decision.early;
  secure_frontend::RequestContext context;
  context.loopback_peer = loopback;
  return complete(router.handle(head, body, decision, context));
}

// GUI-first start-up (spec 2026-10-27 §3, D1): POST /v1/strata/login-link is the one API route that answers without
// the key, and only while its handler is set (listener.login_link); its body is small JSON; it learns the client's class
// (the handler refuses a remote one). Every other route still needs the key.
void login_link_route_answers_without_the_key_only_when_on() {
  secure_frontend::SecretBytes secret;
  (void)secret.assign(kRouterToken);
  std::vector<std::string> calls;
  secure_frontend::ApiRouter router({
      .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address("127.0.0.1"), 8080, {}, {}),
      .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
      .model_id = "fake-model", .owned_by = "local", .chat_body_limit = 49152,
      .health = [] { return secure_frontend::HealthView{true, "ready", ""}; },
      .chat = {},
      .login_link = [&calls](const std::string_view body, const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
        calls.push_back(std::string{body} + (context.loopback_peer ? "" : " remote"));
        return secure_frontend::HttpResponse{.status = 200, .headers = {}, .body = "{}", .log_code = "ok"};
      }});
  expect(request(router, "POST", "/v1/strata/login-link", "{\"code\":\"abc\"}", false).status == 200 &&
             calls == std::vector<std::string>{"{\"code\":\"abc\"}"},
         "the login link answers without the key");
  expect(request(router, "POST", "/v1/strata/login-link", "{\"code\":\"abc\"}", false, false).status == 200 && calls.back().ends_with(" remote"),
         "the handler learns the client's class");
  expect(request(router, "GET", "/v1/strata/login-link", "", false).status == 405, "the link is POST only");
  expect(request(router, "POST", "/v1/strata/login-link", std::string(600, ' '), false).status == 413, "its body is small");
  expect(request(router, "POST", "/v1/strata/login-link", "{}", false, true, "text/plain").status == 415, "its body is JSON");
  expect(request(router, "GET", "/v1/models", "", false).status == 401, "every other route still needs the key");
  auto off = router_with({});
  expect(request(off, "POST", "/v1/strata/login-link", "{\"code\":\"abc\"}", false).status == 401,
         "with the link off the route is like any other: the key first");
}

void model_control_routes_need_the_switch_and_the_key() {
  std::vector<std::string> calls;
  auto router = launcher_router(&calls);
  for (const auto& [method, target] : std::vector<std::pair<std::string, std::string>>{
           {"GET", "/v1/strata/models"}, {"POST", "/v1/strata/models/start"}, {"POST", "/v1/strata/models/stop"}}) {
    expect(request(router, method, target, method == "POST" ? "{}" : "", false).status == 401, method + " " + target + " needs the key");
  }
  expect(calls.empty(), "no handler runs before the key");
  expect(request(router, "GET", "/v1/strata/models").status == 200 && calls == std::vector<std::string>{"list loopback"},
         "the list answers with the key");
  expect(request(router, "GET", "/v1/strata/models", "", true, false).status == 200 && calls.back() == "list remote",
         "the handler learns the client's class");
  expect(request(router, "POST", "/v1/strata/models/start", "{\"name\":\"Qwen\"}").status == 202 &&
             calls.back() == "start {\"name\":\"Qwen\"}",
         "a start reaches its handler with the body");
  expect(request(router, "POST", "/v1/strata/models/stop", "{}").status == 202 && calls.back() == "stop", "a stop reaches its handler");
  expect(request(router, "POST", "/v1/strata/models").status == 405, "the list is GET only");
  expect(request(router, "GET", "/v1/strata/models/start").status == 405, "a start is POST only");
  expect(request(router, "POST", "/v1/strata/models/start", "{}", true, true, "text/plain").status == 415, "a start body is JSON");
  expect(request(router, "POST", "/v1/strata/models/start", std::string(5000, ' ')).status == 413, "a start body is small");
  expect(request(router, "GET", "/v1/strata/models/other").status == 404, "no other control route");
  std::vector<std::string> none;
  auto off = launcher_router(&none, "Gemma-4", {true, "ready", ""}, false);
  for (const auto& [method, target] : std::vector<std::pair<std::string, std::string>>{
           {"GET", "/v1/strata/models"}, {"POST", "/v1/strata/models/start"}, {"POST", "/v1/strata/models/stop"}}) {
    expect(request(off, method, target, method == "POST" ? "{}" : "").status == 404, method + " " + target + " is no route with control off");
  }
  auto single = router_with({});
  expect(get(single, "/v1/strata/models").status == 404, "a single-model server has no control routes");
}

// Model downloads (plan 2026-10-23-model-downloads Task 4): the download routes, keyed, served
// only while their handler is set (models.downloads), each POST a small JSON body.
secure_frontend::ApiRouter downloads_router(std::vector<std::string>* calls, const bool on) {
  secure_frontend::SecretBytes secret;
  (void)secret.assign(kRouterToken);
  secure_frontend::ModelDownloads downloads;
  if (on) {
    downloads.handle = [calls](const secure_frontend::RouteId route, const std::string_view body,
                               const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
      calls->push_back(std::to_string(static_cast<int>(route)) + " " + std::string{body} + (context.loopback_peer ? "" : " remote"));
      return secure_frontend::HttpResponse{.status = 202, .headers = {}, .body = "{}\n"};
    };
  }
  return secure_frontend::ApiRouter({
      .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address("127.0.0.1"), 8080, {}, {}),
      .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
      .model_id = "", .owned_by = "local", .chat_body_limit = 49152,
      .health = [] { return secure_frontend::HealthView{true, "none", ""}; },
      .chat = {},
      .served = [] { return secure_frontend::ServedModels{}; },
      .downloads = std::move(downloads)});
}

void model_download_routes_need_the_switch_and_the_key() {
  using secure_frontend::RouteId;
  const std::vector<std::tuple<std::string, std::string, RouteId>> routes{
      {"GET", "/v1/strata/downloads", RouteId::strata_downloads},
      {"POST", "/v1/strata/downloads/start", RouteId::strata_download_start},
      {"POST", "/v1/strata/downloads/pause", RouteId::strata_download_pause},
      {"POST", "/v1/strata/downloads/cancel", RouteId::strata_download_cancel},
      {"POST", "/v1/strata/downloads/link", RouteId::strata_download_link}};
  std::vector<std::string> calls;
  auto router = downloads_router(&calls, true);
  for (const auto& [method, target, route] : routes) {
    expect(request(router, method, target, method == "POST" ? "{}" : "", false).status == 401, method + " " + target + " needs the key");
  }
  expect(calls.empty(), "no download handler runs before the key");
  for (const auto& [method, target, route] : routes) {
    const std::string body = method == "POST" ? "{\"id\":\"x\"}" : "";
    expect(request(router, method, target, body).status == 202 && !calls.empty() &&
               calls.back() == std::to_string(static_cast<int>(route)) + " " + body,
           method + " " + target + " reaches the handler with its route and body");
  }
  expect(request(router, "GET", "/v1/strata/downloads", "", true, false).status == 202 && calls.back().ends_with(" remote"),
         "the handler learns the client's class");
  expect(request(router, "POST", "/v1/strata/downloads").status == 405, "the list is GET only");
  expect(request(router, "GET", "/v1/strata/downloads/pause").status == 405, "a pause is POST only");
  expect(request(router, "POST", "/v1/strata/downloads/start", "{}", true, true, "text/plain").status == 415, "a start body is JSON");
  expect(request(router, "POST", "/v1/strata/downloads/link", std::string(5000, ' ')).status == 413, "a link body is small");
  expect(request(router, "GET", "/v1/strata/downloads/other").status == 404, "no other download route");
  std::vector<std::string> none;
  auto off = downloads_router(&none, false);
  for (const auto& [method, target, route] : routes) {
    expect(request(off, method, target, method == "POST" ? "{}" : "").status == 404, method + " " + target + " is no route with downloads off");
  }
  expect(none.empty(), "downloads off: nothing is called");
}

// GUI-first start-up (spec 2026-10-27 §5a): the alert settings routes - keyed, POST, a small JSON body - served only
// while their handler is set (a build with email alerts and model control).
void alert_routes_need_their_handler_and_the_key() {
  const std::vector<std::pair<std::string, secure_frontend::RouteId>> routes{
      {"/v1/strata/alerts", secure_frontend::RouteId::strata_alerts},
      {"/v1/strata/alerts/settings", secure_frontend::RouteId::strata_alerts_settings},
      {"/v1/strata/alerts/password", secure_frontend::RouteId::strata_alerts_password},
      {"/v1/strata/alerts/test", secure_frontend::RouteId::strata_alerts_test}};
  const auto make = [](std::vector<std::string>* calls, const bool on) {
    secure_frontend::SecretBytes secret;
    (void)secret.assign(kRouterToken);
    secure_frontend::ModelCardRoutes alerts;
    if (on) {
      alerts.handle = [calls](const secure_frontend::RouteId route, const std::string_view body,
                              const secure_frontend::RequestContext&) -> secure_frontend::RouteResult {
        calls->push_back(std::to_string(static_cast<int>(route)) + " " + std::string{body});
        return secure_frontend::HttpResponse{.status = 200, .headers = {}, .body = "{}\n"};
      };
    }
    return secure_frontend::ApiRouter({
        .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address("127.0.0.1"), 8080, {}, {}),
        .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
        .model_id = "", .owned_by = "local", .chat_body_limit = 49152,
        .health = [] { return secure_frontend::HealthView{true, "none", ""}; },
        .chat = {},
        .served = [] { return secure_frontend::ServedModels{}; },
        .alerts = std::move(alerts)});
  };
  std::vector<std::string> calls;
  auto router = make(&calls, true);
  for (const auto& [target, route] : routes) {
    expect(request(router, "POST", target, "{}", false).status == 401, target + " needs the key");
    expect(request(router, "POST", target, "{\"x\":1}").status == 200 && !calls.empty() &&
               calls.back() == std::to_string(static_cast<int>(route)) + " {\"x\":1}",
           target + " reaches the handler");
    expect(request(router, "GET", target).status == 405, target + " is POST only");
    expect(request(router, "POST", target, "{}", true, true, "text/plain").status == 415, target + " takes JSON");
  }
  std::vector<std::string> none;
  auto off = make(&none, false);
  for (const auto& [target, route] : routes) expect(request(off, "POST", target, "{}").status == 404, target + " is no route when off");
  expect(none.empty(), "off: nothing is called");
}

// Web search (spec 2026-10-27-web-search-design §2): the search settings routes - keyed, POST, a small JSON body - served
// only while their handler is set (a build with web search and model control).
void search_routes_need_their_handler_and_the_key() {
  const std::vector<std::pair<std::string, secure_frontend::RouteId>> routes{
      {"/v1/strata/search", secure_frontend::RouteId::strata_search},
      {"/v1/strata/search/settings", secure_frontend::RouteId::strata_search_settings},
      {"/v1/strata/search/test", secure_frontend::RouteId::strata_search_test}};
  const auto make = [](std::vector<std::string>* calls, const bool on) {
    secure_frontend::SecretBytes secret;
    (void)secret.assign(kRouterToken);
    secure_frontend::ModelCardRoutes search;
    if (on) {
      search.handle = [calls](const secure_frontend::RouteId route, const std::string_view body,
                              const secure_frontend::RequestContext&) -> secure_frontend::RouteResult {
        calls->push_back(std::to_string(static_cast<int>(route)) + " " + std::string{body});
        return secure_frontend::HttpResponse{.status = 200, .headers = {}, .body = "{}\n"};
      };
    }
    return secure_frontend::ApiRouter({
        .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address("127.0.0.1"), 8080, {}, {}),
        .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
        .model_id = "", .owned_by = "local", .chat_body_limit = 49152,
        .health = [] { return secure_frontend::HealthView{true, "none", ""}; },
        .chat = {},
        .served = [] { return secure_frontend::ServedModels{}; },
        .search = std::move(search)});
  };
  std::vector<std::string> calls;
  auto router = make(&calls, true);
  for (const auto& [target, route] : routes) {
    expect(request(router, "POST", target, "{}", false).status == 401, target + " needs the key");
    expect(request(router, "POST", target, "{\"x\":1}").status == 200 && !calls.empty() &&
               calls.back() == std::to_string(static_cast<int>(route)) + " {\"x\":1}",
           target + " reaches the handler");
    expect(request(router, "GET", target).status == 405, target + " is POST only");
    expect(request(router, "POST", target, "{}", true, true, "text/plain").status == 415, target + " takes JSON");
  }
  std::vector<std::string> none;
  auto off = make(&none, false);
  for (const auto& [target, route] : routes) expect(request(off, "POST", target, "{}").status == 404, target + " is no route when off");
  expect(none.empty(), "off: nothing is called");
}

// The Settings tab (spec 2026-10-28-server-window-design, Part 1): its routes are keyed, POST, JSON, served only while
// their handler is set (model control on), as the search settings'.
void config_routes_need_their_handler_and_the_key() {
  const std::vector<std::pair<std::string, secure_frontend::RouteId>> routes{
      {"/v1/strata/config", secure_frontend::RouteId::strata_config},
      {"/v1/strata/config/settings", secure_frontend::RouteId::strata_config_settings},
      {"/v1/strata/key/rotate", secure_frontend::RouteId::strata_key_rotate}};
  const auto make = [](std::vector<std::string>* calls, const bool on) {
    secure_frontend::SecretBytes secret;
    (void)secret.assign(kRouterToken);
    secure_frontend::ModelCardRoutes settings;
    if (on) {
      settings.handle = [calls](const secure_frontend::RouteId route, const std::string_view body,
                                const secure_frontend::RequestContext&) -> secure_frontend::RouteResult {
        calls->push_back(std::to_string(static_cast<int>(route)) + " " + std::string{body});
        return secure_frontend::HttpResponse{.status = 200, .headers = {}, .body = "{}\n"};
      };
    }
    return secure_frontend::ApiRouter({
        .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address("127.0.0.1"), 8080, {}, {}),
        .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
        .model_id = "", .owned_by = "local", .chat_body_limit = 49152,
        .health = [] { return secure_frontend::HealthView{true, "none", ""}; },
        .chat = {},
        .served = [] { return secure_frontend::ServedModels{}; },
        .settings = std::move(settings)});
  };
  std::vector<std::string> calls;
  auto router = make(&calls, true);
  for (const auto& [target, route] : routes) {
    expect(request(router, "POST", target, "{}", false).status == 401, target + " needs the key");
    expect(request(router, "POST", target, "{\"x\":1}").status == 200 && !calls.empty() &&
               calls.back() == std::to_string(static_cast<int>(route)) + " {\"x\":1}",
           target + " reaches the handler");
    expect(request(router, "GET", target).status == 405, target + " is POST only");
    expect(request(router, "POST", target, "{}", true, true, "text/plain").status == 415, target + " takes JSON");
  }
  std::vector<std::string> none;
  auto off = make(&none, false);
  for (const auto& [target, route] : routes) expect(request(off, "POST", target, "{}").status == 404, target + " is no route when off");
  expect(none.empty(), "off: nothing is called");
}

// GUI-first start-up (spec 2026-10-27 §5.3): the calibration routes are control routes - keyed, POST, a small JSON
// body - served only while their handlers are set; a launcher without them (or with control off) answers 404.
void calibration_routes_need_their_handlers_and_the_key() {
  std::vector<std::string> calls;
  auto plain = launcher_router(&calls);
  for (const char* target : {"/v1/strata/models/calibrate", "/v1/strata/models/calibrate/cancel", "/v1/strata/models/calibrate/auto"}) {
    expect(request(plain, "POST", target, "{}").status == 404, std::string{target} + " is no route without its handler");
  }
  secure_frontend::SecretBytes secret;
  (void)secret.assign(kRouterToken);
  secure_frontend::ModelControl handlers;
  handlers.list = [](const secure_frontend::RequestContext&) -> secure_frontend::RouteResult {
    return secure_frontend::HttpResponse{.status = 200, .headers = {}, .body = "{}\n"};
  };
  handlers.start = [](std::string_view, const secure_frontend::RequestContext&) -> secure_frontend::RouteResult {
    return secure_frontend::HttpResponse{.status = 202, .headers = {}, .body = "{}\n"};
  };
  handlers.stop = [](const secure_frontend::RequestContext&) -> secure_frontend::RouteResult {
    return secure_frontend::HttpResponse{.status = 202, .headers = {}, .body = "{}\n"};
  };
  const auto record = [&calls](const char* what) {
    return [&calls, what](std::string_view body, const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
      calls.push_back(std::string{what} + " " + std::string{body} + (context.loopback_peer ? "" : " remote"));
      return secure_frontend::HttpResponse{.status = 202, .headers = {}, .body = "{}\n"};
    };
  };
  handlers.calibrate = record("calibrate");
  handlers.cancel_calibration = record("cancel");
  handlers.auto_recalibrate = record("auto");
  secure_frontend::ApiRouter router({
      .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address("127.0.0.1"), 8080, {}, {}),
      .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
      .model_id = "", .owned_by = "local", .chat_body_limit = 49152,
      .health = [] { return secure_frontend::HealthView{true, "none", ""}; },
      .chat = {},
      .served = [] { return secure_frontend::ServedModels{}; },
      .control = std::move(handlers)});
  calls.clear();
  for (const auto& [target, what] : std::vector<std::pair<std::string, std::string>>{{"/v1/strata/models/calibrate", "calibrate"},
                                                                                      {"/v1/strata/models/calibrate/cancel", "cancel"},
                                                                                      {"/v1/strata/models/calibrate/auto", "auto"}}) {
    expect(request(router, "POST", target, "{\"name\":\"x\"}", false).status == 401, target + " needs the key");
    expect(request(router, "POST", target, "{\"name\":\"x\"}").status == 202 && !calls.empty() && calls.back() == what + " {\"name\":\"x\"}",
           target + " reaches its handler with the body");
    expect(request(router, "GET", target).status == 405, target + " is POST only");
    expect(request(router, "POST", target, "{}", true, true, "text/plain").status == 415, target + " takes JSON");
    expect(request(router, "POST", target, std::string(5000, ' ')).status == 413, target + " takes a small body");
  }
  expect(request(router, "POST", "/v1/strata/models/calibrate", "{}", true, false).status == 202 && calls.back().ends_with(" remote"),
         "the handler learns the client's class");
}

// GUI-first start-up (spec 2026-10-27 §4): the page's card and folder routes, keyed, served only while their handler
// is set (models.control), each a POST of a small JSON body.
void model_card_routes_need_the_switch_and_the_key() {
  using secure_frontend::RouteId;
  const std::vector<std::pair<std::string, RouteId>> routes{
      {"/v1/strata/folders", RouteId::strata_folders},
      {"/v1/strata/folders/private", RouteId::strata_folder_private},
      {"/v1/strata/folders/new", RouteId::strata_folder_new},
      {"/v1/strata/folders/sharing", RouteId::strata_folder_sharing},
      {"/v1/strata/cards/add", RouteId::strata_card_add},
      {"/v1/strata/cards/folder", RouteId::strata_card_folder},
      {"/v1/strata/cards/delete/begin", RouteId::strata_card_delete_begin},
      {"/v1/strata/cards/delete", RouteId::strata_card_delete},
      {"/v1/strata/cards/remove", RouteId::strata_card_remove}};
  const auto make = [](std::vector<std::string>* calls, const bool on) {
    secure_frontend::SecretBytes secret;
    (void)secret.assign(kRouterToken);
    secure_frontend::ModelCardRoutes cards;
    if (on) {
      cards.handle = [calls](const secure_frontend::RouteId route, const std::string_view body,
                             const secure_frontend::RequestContext& context) -> secure_frontend::RouteResult {
        calls->push_back(std::to_string(static_cast<int>(route)) + " " + std::string{body} + (context.loopback_peer ? "" : " remote"));
        return secure_frontend::HttpResponse{.status = 200, .headers = {}, .body = "{}\n"};
      };
    }
    return secure_frontend::ApiRouter({
        .policy = secure_frontend::make_http_policy(*secure_frontend::parse_listener_address("127.0.0.1"), 8080, {}, {}),
        .validator = secure_frontend::make_bearer_validator(std::make_shared<const secure_frontend::TokenVerifier>(std::move(secret))),
        .model_id = "", .owned_by = "local", .chat_body_limit = 49152,
        .health = [] { return secure_frontend::HealthView{true, "none", ""}; },
        .chat = {},
        .served = [] { return secure_frontend::ServedModels{}; },
        .cards = std::move(cards)});
  };
  std::vector<std::string> calls;
  auto router = make(&calls, true);
  for (const auto& [target, route] : routes) {
    expect(request(router, "POST", target, "{}", false).status == 401, target + " needs the key");
  }
  expect(calls.empty(), "no card handler runs before the key");
  for (const auto& [target, route] : routes) {
    const std::string body = "{\"name\":\"x\"}";
    expect(request(router, "POST", target, body).status == 200 && !calls.empty() &&
               calls.back() == std::to_string(static_cast<int>(route)) + " " + body,
           target + " reaches the handler with its route and body");
    expect(request(router, "GET", target).status == 405, target + " is POST only");
  }
  expect(request(router, "POST", "/v1/strata/folders", "{}", true, false).status == 200 && calls.back().ends_with(" remote"),
         "the handler learns the client's class");
  expect(request(router, "POST", "/v1/strata/cards/add", "{}", true, true, "text/plain").status == 415, "a body is JSON");
  expect(request(router, "POST", "/v1/strata/cards/folder", std::string(5000, ' ')).status == 413, "a body is small");
  expect(request(router, "POST", "/v1/strata/cards/other", "{}").status == 404, "no other card route");
  std::vector<std::string> none;
  auto off = make(&none, false);
  for (const auto& [target, route] : routes) {
    expect(request(off, "POST", target, "{}").status == 404, target + " is no route with model control off");
  }
  expect(none.empty(), "control off: nothing is called");
}

void served_models_follow_the_running_card() {
  std::vector<std::string> calls;
  auto router = launcher_router(&calls);
  const auto models = request(router, "GET", "/v1/models");
  expect(models.status == 200 && models.body.find("\"id\":\"Gemma-4\"") != std::string::npos &&
             models.body.find("\"id\":\"gemma\"") != std::string::npos &&
             models.body.find("\"strata_models\":[{\"id\":\"Gemma-4\",\"state\":\"running\"},{\"id\":\"Qwen\",\"state\":\"stopped\"}]") !=
                 std::string::npos,
         "/v1/models lists the running card, its aliases and the cards' states: " + models.body);
  expect(request(router, "GET", "/v1/models/gemma").status == 200, "an alias of the running card is found");
  expect(request(router, "GET", "/v1/models/Qwen").status == 404, "a stopped card is not a served model");
  auto idle = launcher_router(&calls, std::nullopt, {true, "none", ""});
  const auto empty = request(idle, "GET", "/v1/models");
  expect(empty.status == 200 && empty.body.find("\"data\":[]") != std::string::npos && empty.body.find("strata_models") != std::string::npos,
         "nothing running: an empty list and the cards: " + empty.body);
  const auto health = request(idle, "GET", "/health");
  expect(health.status == 200 && health.body == "{\"status\":\"ok\",\"engine\":\"none\"}\n", "nothing running is a healthy server: " + health.body);
  auto unhealthy_none = router_with([] { return secure_frontend::HealthView{false, "none", ""}; });
  expect(get(unhealthy_none, "/health").status == 503, "\"none\" is only ever healthy (fails closed otherwise)");
}

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"health_samples_status_before_state_once_each", health_samples_status_before_state_once_each},
      {"model_control_routes_need_the_switch_and_the_key", model_control_routes_need_the_switch_and_the_key},
      {"login_link_route_answers_without_the_key_only_when_on", login_link_route_answers_without_the_key_only_when_on},
      {"model_download_routes_need_the_switch_and_the_key", model_download_routes_need_the_switch_and_the_key},
      {"model_card_routes_need_the_switch_and_the_key", model_card_routes_need_the_switch_and_the_key},
      {"calibration_routes_need_their_handlers_and_the_key", calibration_routes_need_their_handlers_and_the_key},
      {"alert_routes_need_their_handler_and_the_key", alert_routes_need_their_handler_and_the_key},
      {"search_routes_need_their_handler_and_the_key", search_routes_need_their_handler_and_the_key},
      {"config_routes_need_their_handler_and_the_key", config_routes_need_their_handler_and_the_key},
      {"web_ui_cards_from_this_pc_and_the_danger_zone", web_ui_cards_from_this_pc_and_the_danger_zone},
      {"web_ui_downloads_go_to_a_chosen_folder", web_ui_downloads_go_to_a_chosen_folder},
      {"web_ui_downloads_say_what_is_on_this_pc", web_ui_downloads_say_what_is_on_this_pc},
      {"web_ui_downloads_groups_are_accordions", web_ui_downloads_groups_are_accordions},
      {"web_ui_downloads_destination_is_a_panel", web_ui_downloads_destination_is_a_panel},
      {"web_ui_models_tab_calibrates", web_ui_models_tab_calibrates},
      {"web_ui_has_email_alert_settings", web_ui_has_email_alert_settings},
      {"web_ui_has_server_settings", web_ui_has_server_settings},
      {"web_ui_claims_a_login_link_at_load_and_on_hashchange", web_ui_claims_a_login_link_at_load_and_on_hashchange},
      {"web_ui_opens_the_view_its_address_names", web_ui_opens_the_view_its_address_names},
      {"web_ui_has_web_search_settings", web_ui_has_web_search_settings},
      {"web_ui_has_model_folder_setting", web_ui_has_model_folder_setting},
      {"web_ui_monitor_shows_stopped_and_the_model", web_ui_monitor_shows_stopped_and_the_model},
      {"web_ui_monitor_is_even_and_shows_memory_disk_and_cache", web_ui_monitor_is_even_and_shows_memory_disk_and_cache},
      {"web_ui_about_lists_the_engines_and_this_pc", web_ui_about_lists_the_engines_and_this_pc},
      {"web_ui_search_box_limit_note_and_engine_table", web_ui_search_box_limit_note_and_engine_table},
      {"web_ui_chat_can_turn_search_off", web_ui_chat_can_turn_search_off},
      {"web_ui_listed_integers_are_dropdowns", web_ui_listed_integers_are_dropdowns},
      {"served_models_follow_the_running_card", served_models_follow_the_running_card},
      {"health_carries_recovery_only_with_failed", health_carries_recovery_only_with_failed},
      {"health_includes_engine_state_when_provided", health_includes_engine_state_when_provided},
      {"health_is_503_with_engine_state_when_unavailable", health_is_503_with_engine_state_when_unavailable},
      {"dispatches_exact_health_route", dispatches_exact_health_route},
      {"dispatches_models_with_json_escaping", dispatches_models_with_json_escaping},
      {"rejects_non_exact_routes", rejects_non_exact_routes},
      {"fails_closed_when_provider_throws_or_returns_invalid_model", fails_closed_when_provider_throws_or_returns_invalid_model},
      {"auth01_every_route_rejects_missing_malformed_wrong_token", auth01_every_route_rejects_missing_malformed_wrong_token},
      {"models_list_keeps_utf8_model_id", models_list_keeps_utf8_model_id},
      {"web_ui_off_by_default_serves_nothing_public", web_ui_off_by_default_serves_nothing_public},
      {"web_ui_serves_the_bundled_page_without_a_key", web_ui_serves_the_bundled_page_without_a_key},
      {"web_ui_leaves_every_api_route_authenticated", web_ui_leaves_every_api_route_authenticated},
      {"web_ui_still_checks_host_and_origin", web_ui_still_checks_host_and_origin},
      {"web_ui_assets_are_bundled_and_inert", web_ui_assets_are_bundled_and_inert},
      {"web_ui_has_the_monitor_and_four_thinking_levels", web_ui_has_the_monitor_and_four_thinking_levels},
      {"the_page_sends_reasoning_effort_only_where_it_is_taken", the_page_sends_reasoning_effort_only_where_it_is_taken},
      {"web_ui_has_the_models_tab_hidden_until_the_server_offers_it", web_ui_has_the_models_tab_hidden_until_the_server_offers_it},
      {"web_ui_models_tab_starts_stops_and_polls", web_ui_models_tab_starts_stops_and_polls},
      {"web_ui_chat_follows_the_running_model", web_ui_chat_follows_the_running_model},
      {"web_ui_models_tab_has_a_downloads_section", web_ui_models_tab_has_a_downloads_section},
      {"web_ui_downloads_profile_picker_and_multipart", web_ui_downloads_profile_picker_and_multipart},
      {"web_ui_models_tab_subtabs_and_family_groups", web_ui_models_tab_subtabs_and_family_groups},
      {"web_ui_downloads_start_cancel_poll_and_check_links", web_ui_downloads_start_cancel_poll_and_check_links},
      {"web_ui_offers_the_gpus_of_this_pc", web_ui_offers_the_gpus_of_this_pc},
      {"web_ui_downloads_pause_and_resume", web_ui_downloads_pause_and_resume},
      {"web_ui_header_fits_a_phone", web_ui_header_fits_a_phone},
      {"keyless_router_answers_without_a_key", keyless_router_answers_without_a_key},
      {"metrics_route_needs_the_key_and_the_switch", metrics_route_needs_the_key_and_the_switch},
      {"about_route_needs_the_key_and_answers_its_provider", about_route_needs_the_key_and_answers_its_provider},
  }, failures);
}
