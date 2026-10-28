#pragma once

// Web search (docs/superpowers/specs/2026-10-27-web-search-design.md §2): the page's search settings (kSearchRoutes) -
// built only with SECURE_FRONTEND_WEB_SEARCH. For the clients the alert settings serve (loopback, and the ones
// SearchRouteSettings::client_allowed lets in); each body a small JSON object:
//
//   POST /v1/strata/search           {}  -> {"built": true, "editable", "running", "enabled", "url", "results",
//                                            "max_rounds", "timeout_s"}   (the config file's; running: this server's)
//   POST /v1/strata/search/settings  {"enabled","url","results","max_rounds","timeout_s"}
//                                    -> {"saved": true, "restart_needed": true}   (checked by search_from_draft, written
//                                       to the config file; the running server keeps its settings until it restarts)
//   POST /v1/strata/search/test      {"query"?} -> {"ok", "text", "chars", "truncated", "ms"}   (one search through the
//                                       saved settings, by a strata-search started for it)

#include "secure_frontend/api_router.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/request_router.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

struct SearchRouteSettings final {
  std::optional<std::filesystem::path> config_file{};  // where the settings live (unset: read-only defaults)
  std::filesystem::path program{};                     // strata-search, beside the server
  std::map<std::string, std::string, std::less<>> environment{};  // its closed child environment
  bool running{false};                               // this server serves search (search.enabled when it started)
  std::function<void(std::string_view)> log{};
  // Who may use these routes (the server passes model_files_allowed, as for the model-file routes; owner 2026-10-27).
  // Empty: loopback clients only.
  std::function<bool(const RequestContext& context)> client_allowed{};
};

class SearchRoutes final {
 public:
  explicit SearchRoutes(SearchRouteSettings settings);
  [[nodiscard]] HttpResponse answer(RouteId route, std::string_view body, const RequestContext& context);

 private:
  [[nodiscard]] std::optional<SearchConfig> saved() const;
  [[nodiscard]] HttpResponse settings_json() const;
  [[nodiscard]] HttpResponse save(std::string_view body);
  [[nodiscard]] HttpResponse test(std::string_view body);

  SearchRouteSettings settings_;
  std::mutex file_;  // one config write at a time
};

}  // namespace secure_frontend
