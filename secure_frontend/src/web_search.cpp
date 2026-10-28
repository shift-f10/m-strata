#include "secure_frontend/web_search.hpp"

#include "secure_frontend/json.hpp"
#include "secure_frontend/smtp_tls.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <utility>
#include <vector>

namespace secure_frontend {
namespace {

constexpr std::size_t kMaxTitleBytes = 200U;
constexpr std::size_t kMaxSnippetBytes = 500U;
constexpr std::size_t kMaxUrlBytes = 2000U;
constexpr std::size_t kMaxQueryBytes = 512U;

std::string lower(std::string_view text) {
  std::string out(text);
  for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// RFC 3986 percent-encoding of every byte that is not unreserved.
std::string percent_encoded(const std::string_view text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (const char c : text) {
    const auto b = static_cast<unsigned char>(c);
    if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '-' || b == '.' || b == '_' ||
        b == '~') {
      out.push_back(c);
    } else {
      out.push_back('%');
      out.push_back(kHex[b >> 4U]);
      out.push_back(kHex[b & 0x0FU]);
    }
  }
  return out;
}

// Whitespace runs as one space, trimmed; then cut to `limit` bytes on a code point, with an ellipsis when cut.
std::string tidy(const std::string_view text, const std::size_t limit) {
  std::string out;
  bool space = false;
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
      space = !out.empty();
      continue;
    }
    if (space) out.push_back(' ');
    space = false;
    out.push_back(c);
  }
  if (out.size() <= limit) return out;
  std::size_t cut = limit;
  while (cut > 0U && (static_cast<unsigned char>(out[cut]) & 0xC0U) == 0x80U) --cut;
  out.resize(cut);
  return out + "...";
}

const JsonValue* member(const JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& m : *members) {
    if (m.key == key) return &m.value;
  }
  return nullptr;
}

std::string text_of(const JsonValue& object, const std::string_view key) {
  const auto* value = member(object, key);
  return value != nullptr && value->if_string() != nullptr ? *value->if_string() : std::string{};
}

}  // namespace

std::string search_request(const SearchOrigin& origin, const std::string_view query) {
  const bool default_port = origin.port == (origin.tls ? 443U : 80U);
  const bool v6 = origin.host.find(':') != std::string::npos;
  const std::string host = (v6 ? "[" + origin.host + "]" : origin.host) + (default_port ? "" : ":" + std::to_string(origin.port));
  return "GET " + origin.base_path + "/search?q=" + percent_encoded(query) + "&format=json HTTP/1.1\r\nHost: " + host +
         "\r\nAccept: application/json\r\nUser-Agent: strata-search\r\nConnection: close\r\n\r\n";
}

std::optional<HttpReplyResult> parse_http_reply(const std::string_view bytes, const bool closed, const std::size_t max_bytes) {
  if (bytes.size() > max_bytes) return HttpReplyResult{std::in_place_index<1>, "the reply is larger than allowed"};
  const auto head_end = bytes.find("\r\n\r\n");
  if (head_end == std::string_view::npos) {
    if (closed) return HttpReplyResult{std::in_place_index<1>, "the reply is not HTTP"};
    return std::nullopt;
  }
  const std::string_view head = bytes.substr(0, head_end);
  const auto line_end = head.find("\r\n");
  const std::string_view status_line = head.substr(0, line_end);
  if (status_line.rfind("HTTP/1.", 0) != 0 || status_line.size() < 12U || status_line[8] != ' ') {
    return HttpReplyResult{std::in_place_index<1>, "the reply is not HTTP"};
  }
  HttpReply reply;
  const auto status = status_line.substr(9, 3);
  const auto [end, error] = std::from_chars(status.data(), status.data() + status.size(), reply.status);
  if (error != std::errc{} || end != status.data() + status.size()) return HttpReplyResult{std::in_place_index<1>, "the reply is not HTTP"};
  std::optional<std::size_t> length;
  bool chunked = false;
  for (std::size_t at = line_end == std::string_view::npos ? head.size() : line_end + 2U; at < head.size();) {
    auto next = head.find("\r\n", at);
    if (next == std::string_view::npos) next = head.size();
    const auto line = head.substr(at, next - at);
    at = next + 2U;
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    const std::string name = lower(line.substr(0, colon));
    auto value = line.substr(colon + 1U);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    if (name == "content-type") reply.content_type = lower(value);
    if (name == "transfer-encoding" && lower(value).find("chunked") != std::string::npos) chunked = true;
    if (name == "content-length") {
      std::size_t n = 0;
      const auto [e, err] = std::from_chars(value.data(), value.data() + value.size(), n);
      if (err != std::errc{} || e != value.data() + value.size()) return HttpReplyResult{std::in_place_index<1>, "the reply's length is invalid"};
      length = n;
    }
  }
  const std::string_view body = bytes.substr(head_end + 4U);
  if (chunked) {
    std::string decoded;
    for (std::size_t at = 0;;) {
      const auto size_end = body.find("\r\n", at);
      if (size_end == std::string_view::npos) break;
      auto size_text = body.substr(at, size_end - at);
      if (const auto semi = size_text.find(';'); semi != std::string_view::npos) size_text = size_text.substr(0, semi);
      std::size_t size = 0;
      const auto [e, err] = std::from_chars(size_text.data(), size_text.data() + size_text.size(), size, 16);
      if (err != std::errc{} || e != size_text.data() + size_text.size()) {
        return HttpReplyResult{std::in_place_index<1>, "the reply's chunks are invalid"};
      }
      if (size == 0U) {
        reply.body = std::move(decoded);
        return HttpReplyResult{std::move(reply)};
      }
      const std::size_t data = size_end + 2U;
      if (body.size() < data + size + 2U) break;
      decoded.append(body.substr(data, size));
      at = data + size + 2U;
    }
    if (closed) return HttpReplyResult{std::in_place_index<1>, "the reply ended early"};
    return std::nullopt;
  }
  if (length) {
    if (body.size() >= *length) {
      reply.body = std::string(body.substr(0, *length));
      return HttpReplyResult{std::move(reply)};
    }
    if (closed) return HttpReplyResult{std::in_place_index<1>, "the reply ended early"};
    return std::nullopt;
  }
  if (!closed) return std::nullopt;
  reply.body = std::string(body);
  return HttpReplyResult{std::move(reply)};
}

SearchText format_search_results(const std::string_view json, const std::string_view query, const std::size_t max_results) {
  const auto parsed = parse_json(json, JsonLimits{.max_bytes = 8U << 20U, .max_depth = 32U, .max_container_entries = 100000U,
                                                  .max_string_bytes = 1U << 20U, .allow_fractions = true});
  const auto* root = std::get_if<JsonValue>(&parsed);
  if (root == nullptr || root->if_object() == nullptr) return SearchText{std::in_place_index<1>, "the instance did not answer SearXNG's JSON"};
  const auto* results = member(*root, "results");
  if (results == nullptr || results->if_array() == nullptr) {
    return SearchText{std::in_place_index<1>, "the instance did not answer SearXNG's JSON"};
  }
  std::string out = "Web search results for \"" + tidy(query, kMaxQueryBytes) + "\" (SearXNG):\n";
  if (const auto* answers = member(*root, "answers"); answers != nullptr && answers->if_array() != nullptr) {
    for (const auto& answer : *answers->if_array()) {
      const std::string text = answer.if_string() != nullptr ? *answer.if_string() : text_of(answer, "answer");
      if (!text.empty()) out += "\nAnswer: " + tidy(text, kMaxSnippetBytes) + "\n";
    }
  }
  std::size_t n = 0;
  for (const auto& result : *results->if_array()) {
    if (n == max_results) break;
    const std::string url = text_of(result, "url");
    if (url.empty() || url.size() > kMaxUrlBytes) continue;
    ++n;
    out += "\n" + std::to_string(n) + ". " + tidy(text_of(result, "title"), kMaxTitleBytes) + "\n   " + url + "\n";
    if (const std::string date = text_of(result, "publishedDate"); !date.empty()) out += "   Published: " + tidy(date, 40U) + "\n";
    if (const std::string snippet = tidy(text_of(result, "content"), kMaxSnippetBytes); !snippet.empty()) out += "   " + snippet + "\n";
  }
  if (n == 0U) {
    // SearXNG's unresponsive_engines ([name, reason] pairs): an empty search names the engines that refused it, so the
    // model does not take a refusal for a search with no matches and try again and again (owner 2026-10-28)
    std::string refused;
    if (const auto* engines = member(*root, "unresponsive_engines"); engines != nullptr && engines->if_array() != nullptr) {
      std::size_t named = 0U;
      for (const auto& engine : *engines->if_array()) {
        const auto* pair = engine.if_array();
        if (pair == nullptr || pair->size() != 2U || (*pair)[0].if_string() == nullptr || (*pair)[1].if_string() == nullptr) continue;
        if (named++ == 8U) break;
        refused += (refused.empty() ? "" : "; ") + tidy(*(*pair)[0].if_string(), 40U) + ": " + tidy(*(*pair)[1].if_string(), 80U);
      }
    }
    out += refused.empty() ? std::string{"\nNo results.\n"}
                           : "\nNo results: the search engines did not answer (" + refused + "), so searching again now is unlikely to help.\n";
  }
  return SearchText{std::in_place_index<0>, std::move(out)};
}

SearchText web_search(const SearchSettings& settings, const std::string_view query, const std::size_t max_results,
                      const SearchConnect& connect) {
  if (query.empty() || query.size() > kMaxQueryBytes || !is_valid_utf8(query)) {
    return SearchText{std::in_place_index<1>, "the query must be 1-512 bytes of UTF-8"};
  }
  std::string error;
  auto transport = connect ? connect(settings.origin, settings.timeout, &error) : nullptr;
  if (!transport) return SearchText{std::in_place_index<1>, "the search instance could not be reached"};
  if (!transport->write(search_request(settings.origin, query))) {
    return SearchText{std::in_place_index<1>, "the search instance could not be reached"};
  }
  std::string bytes;
  for (;;) {
    const auto chunk = transport->read_chunk();
    if (chunk) bytes += *chunk;
    const auto reply = parse_http_reply(bytes, !chunk.has_value(), settings.max_reply_bytes);
    if (!reply) continue;
    if (const auto* failure = std::get_if<1>(&*reply)) return SearchText{std::in_place_index<1>, *failure};
    const auto& got = std::get<HttpReply>(*reply);
    if (got.status >= 300 && got.status < 400) return SearchText{std::in_place_index<1>, "the search instance answered a redirect (not followed)"};
    if (got.status == 403) {
      return SearchText{std::in_place_index<1>, "the search instance refused JSON (enable \"json\" in its search.formats)"};
    }
    if (got.status != 200) return SearchText{std::in_place_index<1>, "the search instance answered HTTP " + std::to_string(got.status)};
    return format_search_results(got.body, query, std::clamp<std::size_t>(max_results, 1U, 10U));
  }
}

std::unique_ptr<SmtpTransport> connect_search(const SearchOrigin& origin, const std::chrono::seconds timeout, std::string* error) {
  return connect_smtp(origin.host, origin.port, origin.tls ? SmtpSecurity::ssl : SmtpSecurity::starttls, timeout, error);
}

}  // namespace secure_frontend
