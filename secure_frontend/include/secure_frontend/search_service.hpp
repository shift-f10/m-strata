#pragma once

// Web search (docs/superpowers/specs/2026-10-27-web-search-design.md §2): the server's tool - built only with
// SECURE_FRONTEND_WEB_SEARCH. It runs strata-search, the bundled MCP server, beside the server and talks to it over the
// child's stdio with the MCP client (upstream's serve/mcp.py). The program starts on the first chat that needs it and
// runs until the server stops; one that failed is started again by a later chat (after kRestartPause). The child gets
// the closed environment the downloads' children get and only the instance's URL and limits as arguments: it is the
// only process of the install that connects out for search, and only to that instance.

#include "secure_frontend/engine_stdio_process.hpp"
#include "secure_frontend/mcp_client.hpp"
#include "secure_frontend/tool_runner.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace secure_frontend {

struct SearchServiceSettings final {
  std::filesystem::path program{};   // strata-search, beside the server
  std::string url{};                 // search.url
  std::size_t results{5};            // search.results
  std::chrono::seconds timeout{30};  // search.timeout_s: strata-search's each network step
  std::map<std::string, std::string, std::less<>> environment{};  // the closed child environment
  // Today's date for the tool's description, YYYY-MM-DD (default: this PC's local date).
  std::function<std::string()> today{};
  std::function<void(std::string_view)> log{};  // one fixed line per start and failure (default: none)
};

class SearchService final : public ToolRunner {
 public:
  static constexpr std::chrono::seconds kRestartPause{60};  // after a failed start, chats are served without search

  explicit SearchService(SearchServiceSettings settings);
  ~SearchService() override;
  SearchService(const SearchService&) = delete;
  SearchService& operator=(const SearchService&) = delete;

  // web_search as the template shows it: strata-search's own name, description and schema (its tools/list), the
  // description ending with today's date. While a search runs (another chat's), the tool it listed is offered as is.
  [[nodiscard]] std::optional<ToolOffer> offer() override;
  [[nodiscard]] ToolResult run(const ToolCall& call, const std::function<bool()>& cancelled) override;
  // One log line per reason a model's chats are served without the tool (each reason said once while the server runs).
  void served_without(std::string_view why) override;
  [[nodiscard]] std::string system_note() const override;

 private:
  [[nodiscard]] bool ensure_started();  // the mutex is held
  void stop_child() noexcept;           // the mutex is held
  [[nodiscard]] std::optional<ToolOffer> offer_of(const McpTool& tool) const;

  SearchServiceSettings settings_;
  std::mutex mutex_;  // one start, one call at a time
  std::unique_ptr<EngineStdioProcess> process_;
  std::unique_ptr<McpClient> client_;
  std::optional<std::chrono::steady_clock::time_point> failed_at_;
  std::mutex listed_mutex_;           // the tool the running program listed (kept while it runs)
  std::optional<McpTool> listed_;
  std::mutex said_mutex_;             // the reasons served_without has logged
  std::set<std::string, std::less<>> said_;
};

// This PC's local date, YYYY-MM-DD.
[[nodiscard]] std::string local_date_today();

}  // namespace secure_frontend
