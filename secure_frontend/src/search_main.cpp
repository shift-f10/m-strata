// strata-search (docs/superpowers/specs/2026-10-27-web-search-design.md §2): the server's web search, an MCP server on
// stdio with one tool (web_search) over one SearXNG instance. Built only with SECURE_FRONTEND_WEB_SEARCH.
//
//   strata-search --url <http(s)://host[:port][/path]> [--results N] [--timeout S]
//
// stdin: one JSON-RPC message a line; stdout: the replies, one a line; stderr: why it stopped. It never prints a query.
#include "secure_frontend/search_mcp_server.hpp"
#include "secure_frontend/web_search.hpp"

#include <charconv>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

constexpr std::size_t kMaxLineBytes = 1U << 20U;

bool integer_in(const std::string_view text, const int low, const int high, int& out) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
  return error == std::errc{} && end == text.data() + text.size() && out >= low && out <= high;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  (void)_setmode(_fileno(stdin), _O_BINARY);
  (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
  secure_frontend::SearchSettings settings;
  bool have_url = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    const bool valued = i + 1 < argc;
    int value = 0;
    if (flag == "--url" && valued) {
      const auto origin = secure_frontend::parse_search_url(argv[++i]);
      if (!origin) {
        std::cerr << "strata-search: --url must be http(s)://host[:port][/path]\n";
        return 2;
      }
      settings.origin = *origin;
      have_url = true;
    } else if (flag == "--results" && valued && integer_in(argv[++i], 1, 10, value)) {
      settings.results = static_cast<std::size_t>(value);
    } else if (flag == "--timeout" && valued && integer_in(argv[++i], 1, 600, value)) {
      settings.timeout = std::chrono::seconds(value);
    } else {
      std::cerr << "usage: strata-search --url <http(s)://host[:port][/path]> [--results 1-10] [--timeout 1-600]\n";
      return 2;
    }
  }
  if (!have_url) {
    std::cerr << "usage: strata-search --url <http(s)://host[:port][/path]> [--results 1-10] [--timeout 1-600]\n";
    return 2;
  }
  secure_frontend::SearchMcpServer server(
      [&settings](const std::string_view query, const std::size_t results) {
        return secure_frontend::web_search(settings, query, results, secure_frontend::connect_search);
      },
      settings.results);
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.size() > kMaxLineBytes) continue;  // (never a message of ours)
    for (const auto& out : server.handle(line)) std::cout << out << '\n' << std::flush;
  }
  return 0;
}
