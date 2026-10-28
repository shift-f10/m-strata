#pragma once

// strata-search's MCP server (docs/superpowers/specs/2026-10-27-web-search-design.md §2): JSON-RPC 2.0 over stdio,
// newline-delimited, protocol 2025-06-18 - the messages upstream's and Maya's MCP client send (serve/mcp.py):
// initialize, notifications/initialized, tools/list, tools/call, ping, notifications/cancelled. One tool, web_search.
// Built only with SECURE_FRONTEND_WEB_SEARCH.

#include "secure_frontend/web_search.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

inline constexpr std::string_view kMcpProtocolVersion = "2025-06-18";
inline constexpr std::string_view kWebSearchTool = "web_search";
// The tool's description: when the model should search (the owner's ask), how to use the results.
inline constexpr std::string_view kWebSearchDescription =
    "Search the web through a SearXNG instance and get the top results (title, URL, snippet). Call it when you are not "
    "sure of a fact, when the answer depends on recent events or anything after your training data, or when the user "
    "asks you to look something up. Then answer from the results and cite the URLs you used.";

class SearchMcpServer final {
 public:
  // `search`: one search for (query, max_results) - web_search in production.
  using Search = std::function<SearchText(std::string_view query, std::size_t max_results)>;
  SearchMcpServer(Search search, std::size_t default_results);

  // One message line in, the reply lines out (none for a notification). A line that is not JSON is a parse error
  // (-32700, id null); an unknown method -32601; bad parameters -32602.
  [[nodiscard]] std::vector<std::string> handle(std::string_view line);

 private:
  Search search_;
  std::size_t default_results_;
};

}  // namespace secure_frontend
