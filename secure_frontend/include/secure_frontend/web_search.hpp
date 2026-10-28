#pragma once

// Web search (docs/superpowers/specs/2026-10-27-web-search-design.md §2): one search of a SearXNG instance's JSON API -
// built only with SECURE_FRONTEND_WEB_SEARCH, for strata-search (search_mcp_server.hpp), never the server. The only
// connection it makes is to the configured instance: `GET <url>/search?q=<query>&format=json`, http or https (TLS
// checked as the email client checks it, smtp_tls.hpp), no redirect followed, the reply bounded in size and time.

#include "secure_frontend/search_url.hpp"
#include "secure_frontend/smtp_client.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace secure_frontend {

// The request strata-search sends for `query` (UTF-8), percent-encoded; Connection: close.
[[nodiscard]] std::string search_request(const SearchOrigin& origin, std::string_view query);

// An HTTP/1.x reply: its status, Content-Type and body (Content-Length, chunked, or up to the close).
struct HttpReply final {
  int status{0};
  std::string content_type;
  std::string body;
};
// The reply in `bytes` once it is whole: the reply, nullopt while more is needed (`closed`: no more will come), or a
// fixed error. Replies past `max_bytes` are refused.
using HttpReplyResult = std::variant<HttpReply, std::string>;
[[nodiscard]] std::optional<HttpReplyResult> parse_http_reply(std::string_view bytes, bool closed, std::size_t max_bytes);

// SearXNG's JSON reply as the text the model reads: its answers, then up to `max_results` results numbered, each its
// title, URL, date (when given) and snippet; or a fixed error for a reply that is not SearXNG's JSON.
using SearchText = std::variant<std::string, std::string>;  // index 0: the text, 1: an error
[[nodiscard]] SearchText format_search_results(std::string_view json, std::string_view query, std::size_t max_results);

struct SearchSettings final {
  SearchOrigin origin;
  std::size_t results{5};                    // the default number of results (1-10)
  std::chrono::seconds timeout{30};          // each read and write
  std::size_t max_reply_bytes{4U << 20U};    // the whole reply
};
// The connection to the instance (connect_smtp in production: plain TCP or TLS from the first byte).
using SearchConnect = std::function<std::unique_ptr<SmtpTransport>(const SearchOrigin&, std::chrono::seconds, std::string*)>;
// One search: the request, the reply, the text (or why there is none).
[[nodiscard]] SearchText web_search(const SearchSettings& settings, std::string_view query, std::size_t max_results,
                                    const SearchConnect& connect);
// connect_smtp for an origin: plain TCP (never upgraded) for http, TLS from the first byte for https.
[[nodiscard]] std::unique_ptr<SmtpTransport> connect_search(const SearchOrigin& origin, std::chrono::seconds timeout,
                                                            std::string* error);

}  // namespace secure_frontend
