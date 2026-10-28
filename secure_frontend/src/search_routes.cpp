#include "secure_frontend/search_routes.hpp"

#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/config_writer.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/python_text.hpp"
#include "secure_frontend/search_service.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

constexpr JsonLimits kBodyLimits{.max_bytes = kModelControlBodyLimit, .max_depth = 3U, .max_container_entries = 16U,
                                 .max_string_bytes = 2048U};
constexpr std::size_t kTestTextChars = 4000U;  // what the page shows of a test search

std::string json_text(const std::string_view text) {
  std::string out;
  append_json_string(&out, text);
  return out;
}

HttpResponse json_answer(const int status, std::string body) {
  return {.status = status, .headers = {{"Content-Type", "application/json; charset=utf-8"}, {"Cache-Control", "no-store"}},
          .body = std::move(body)};
}

ConfigDraftEntry file_entry(ConfigDraftValue value) { return ConfigDraftEntry{std::move(value), ConfigSource::file}; }

}  // namespace

SearchRoutes::SearchRoutes(SearchRouteSettings settings) : settings_(std::move(settings)) {}

HttpResponse SearchRoutes::answer(const RouteId route, const std::string_view body, const RequestContext& context) {
  // what this PC searches through is the business of this PC and the clients the server's rule lets in
  const bool allowed = settings_.client_allowed ? settings_.client_allowed(context) : context.loopback_peer;
  if (!allowed) return make_error_response(ApiError::control_not_allowed);
  switch (route) {
    case RouteId::strata_search: return settings_json();
    case RouteId::strata_search_settings: return save(body);
    case RouteId::strata_search_test: return test(body);
    default: return make_error_response(ApiError::not_found);
  }
}

// The settings the config file holds now (what applies at the next start).
std::optional<SearchConfig> SearchRoutes::saved() const {
  if (!settings_.config_file.has_value()) return SearchConfig{};
  const auto draft = read_config_file(*settings_.config_file);
  if (!std::holds_alternative<FrontendConfigDraft>(draft)) return std::nullopt;
  const auto search = search_from_draft(std::get<FrontendConfigDraft>(draft));
  if (!std::holds_alternative<SearchConfig>(search)) return std::nullopt;
  return std::get<SearchConfig>(search);
}

HttpResponse SearchRoutes::settings_json() const {
  const auto search = saved();
  if (!search) return make_error_response(ApiError::settings_refused, {.reason = "the config file could not be read"});
  return json_answer(200, std::string{"{\"built\":true,\"editable\":"} + (settings_.config_file ? "true" : "false") +
                              ",\"running\":" + (settings_.running ? "true" : "false") + ",\"enabled\":" +
                              (search->enabled ? "true" : "false") + ",\"url\":" + json_text(search->url) +
                              ",\"results\":" + std::to_string(search->results) + ",\"max_rounds\":" +
                              std::to_string(search->max_rounds) + ",\"timeout_s\":" + std::to_string(search->timeout.count()) + "}\n");
}

HttpResponse SearchRoutes::save(const std::string_view body) {
  if (!settings_.config_file.has_value()) {
    return make_error_response(ApiError::settings_refused, {.reason = "this server was started without a config file to write"});
  }
  const auto parsed = parse_json(body, kBodyLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root == nullptr ? nullptr : root->if_object();
  if (members == nullptr) return make_error_response(ApiError::invalid_request);
  // The page's fields as the config's search.* rows; checked alone, then written over the file's.
  FrontendConfigDraft rows;
  for (const auto& member : *members) {
    const std::string row = "search." + member.key;
    if (const auto* flag = member.value.if_boolean(); flag != nullptr && member.key == "enabled") {
      rows.entries[row] = file_entry(*flag);
    } else if (const auto* number = member.value.if_integer();
               number != nullptr && (member.key == "results" || member.key == "max_rounds" || member.key == "timeout_s")) {
      rows.entries[row] = file_entry(*number);
    } else if (const auto* text = member.value.if_string(); text != nullptr && member.key == "url") {
      if (!text->empty()) rows.entries[row] = file_entry(*text);
    } else {
      return make_error_response(ApiError::invalid_request);
    }
  }
  const auto checked = search_from_draft(rows);
  if (const auto* issues = std::get_if<ConfigErrors>(&checked); issues != nullptr && !issues->empty()) {
    return make_error_response(ApiError::settings_refused, {.reason = format_issue(issues->front())});
  }
  const std::scoped_lock lock(file_);
  const auto failure = update_config_file(*settings_.config_file, [&rows](FrontendConfigDraft& draft) {
    std::erase_if(draft.entries, [](const auto& entry) { return entry.first.rfind("search.", 0) == 0U; });
    for (auto& [row, entry] : rows.entries) draft.entries[row] = std::move(entry);
  });
  if (failure) return make_error_response(ApiError::settings_refused, {.reason = config_update_reason(*failure)});
  if (settings_.log) settings_.log("[strata] search: the web search settings were saved (they apply at the next start)");
  return json_answer(200, "{\"saved\":true,\"restart_needed\":true}\n");
}

HttpResponse SearchRoutes::test(const std::string_view body) {
  const auto parsed = parse_json(body, kBodyLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root == nullptr ? nullptr : root->if_object();
  if (members == nullptr) return make_error_response(ApiError::invalid_request);
  std::string query = "SearXNG";
  for (const auto& member : *members) {
    const auto* text = member.value.if_string();
    if (member.key != "query" || text == nullptr || text->size() > 512U) return make_error_response(ApiError::invalid_request);
    if (!python_strip(*text).empty()) query = *text;
  }
  const auto search = saved();
  if (!search) return make_error_response(ApiError::settings_refused, {.reason = "the config file could not be read"});
  if (search->url.empty()) return make_error_response(ApiError::settings_refused, {.reason = "no SearXNG URL is saved yet"});
  SearchService service(SearchServiceSettings{.program = settings_.program,
                                              .url = search->url,
                                              .results = search->results,
                                              .timeout = search->timeout,
                                              .environment = settings_.environment,
                                              .today = {},
                                              .log = settings_.log});
  ToolResult result;
  if (service.offer().has_value()) {
    result = service.run(ToolCall{"web_search", {{"query", JsonValue::make_string(query)}}}, [] { return false; });
  } else {
    result.text = "error: strata-search did not start (it belongs beside the server)";
    result.chars = result.text.size();
  }
  const std::size_t shown = std::min(python_length(result.text), kTestTextChars);
  std::size_t cut = 0U;
  for (std::size_t seen = 0U; cut < result.text.size() && seen < shown; ++seen) cut += utf8_code_point(result.text, cut).second;
  if (settings_.log) settings_.log(std::string{"[strata] search: a test search "} + (result.ok ? "found results" : "failed"));
  return json_answer(200, std::string{"{\"ok\":"} + (result.ok ? "true" : "false") + ",\"text\":" + json_text(result.text.substr(0U, cut)) +
                              ",\"chars\":" + std::to_string(result.chars) + ",\"truncated\":" +
                              (result.truncated || cut < result.text.size() ? "true" : "false") + ",\"ms\":" +
                              std::to_string(result.took.count()) + "}\n");
}

}  // namespace secure_frontend
