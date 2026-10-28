#pragma once
// SP4 §10.1: the fake downloader's scenario format and the exact §3.3 argv
// grammar, shared by tests/fake_curl_main.cpp and the in-process
// ScriptedTransport (tests/support/scripted_transport.hpp, Task 21).
//
// Scenario text, one directive per line ('#' comments, blank lines ignored):
//   probe <version> <tls-token>      what "-q --version" prints
//   probe-exit <code>
//   response <url> [range <N>]       starts a response; selected by URL and --range N-
//   status <code> [reason...]        starts a header block (1xx blocks first)
//   header <Name>: <value>
//   body-file <path>                 relative to the scenario directory
//   body-text <text>                 the rest of the line
//   truncate-at <N>                  emit only N body bytes
//   exceed-by <N>                    append N 'X' bytes after the body
//   exit <code>                      curl exit status (default 0)
//   write-attempt <path>             try to create this file; errno recorded
//   read-attempt <path>              try to open this file for reading; errno recorded
//   end
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace secure_frontend_test::curl {

struct Block final {
  int status{200};
  std::string reason{"OK"};
  std::vector<std::pair<std::string, std::string>> headers;
};

struct Response final {
  std::string url;
  std::optional<std::uint64_t> range;
  std::vector<Block> blocks;
  std::string body_file;
  std::string body_text;
  std::optional<std::uint64_t> truncate_at;
  std::uint64_t exceed_by{0};
  int exit_code{0};
  std::string write_attempt;
  std::string read_attempt;
  std::uint64_t delay_ms{0};  // the fake sleeps this long after the head, before the body
};

struct Scenario final {
  std::vector<Response> responses;
  std::string probe_version{"8.4.0"};
  std::string probe_tls{"OpenSSL/3.0.13"};
  int probe_exit{0};
};

inline std::optional<std::uint64_t> to_u64(const std::string_view text) {
  if (text.empty() || text.size() > 19) return std::nullopt;
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10U + static_cast<std::uint64_t>(c - '0');
  }
  return value;
}

inline std::pair<std::string_view, std::string_view> split_word(const std::string_view line) {
  const auto space = line.find(' ');
  if (space == std::string_view::npos) return {line, {}};
  return {line.substr(0, space), line.substr(space + 1)};
}

inline std::optional<Scenario> parse_scenario(const std::string_view text) {
  Scenario scenario;
  std::optional<Response> current;
  std::size_t start = 0;
  while (start < text.size()) {
    auto end = text.find('\n', start);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line.front() == '#') continue;
    const auto [word, rest] = split_word(line);
    if (word == "probe") {
      const auto [version, tls] = split_word(rest);
      if (version.empty() || tls.empty()) return std::nullopt;
      scenario.probe_version = std::string{version};
      scenario.probe_tls = std::string{tls};
    } else if (word == "probe-exit") {
      const auto code = to_u64(rest);
      if (!code) return std::nullopt;
      scenario.probe_exit = static_cast<int>(*code);
    } else if (word == "response") {
      if (current) return std::nullopt;
      const auto [url, tail] = split_word(rest);
      current = Response{};
      current->url = std::string{url};
      if (!tail.empty()) {
        const auto [keyword, number] = split_word(tail);
        const auto n = to_u64(number);
        if (keyword != "range" || !n) return std::nullopt;
        current->range = *n;
      }
    } else if (!current) {
      return std::nullopt;
    } else if (word == "status") {
      const auto [code, reason] = split_word(rest);
      const auto n = to_u64(code);
      if (!n || code.size() != 3) return std::nullopt;
      current->blocks.push_back(Block{static_cast<int>(*n), std::string{reason}, {}});
    } else if (word == "header") {
      const auto colon = rest.find(": ");
      if (current->blocks.empty() || colon == std::string_view::npos) return std::nullopt;
      current->blocks.back().headers.emplace_back(std::string{rest.substr(0, colon)}, std::string{rest.substr(colon + 2)});
    } else if (word == "body-file") {
      current->body_file = std::string{rest};
    } else if (word == "body-text") {
      current->body_text = std::string{rest};
    } else if (word == "truncate-at") {
      current->truncate_at = to_u64(rest);
      if (!current->truncate_at) return std::nullopt;
    } else if (word == "exceed-by") {
      const auto n = to_u64(rest);
      if (!n) return std::nullopt;
      current->exceed_by = *n;
    } else if (word == "exit") {
      const auto n = to_u64(rest);
      if (!n) return std::nullopt;
      current->exit_code = static_cast<int>(*n);
    } else if (word == "write-attempt") {
      current->write_attempt = std::string{rest};
    } else if (word == "read-attempt") {
      current->read_attempt = std::string{rest};
    } else if (word == "delay-ms") {
      const auto n = to_u64(rest);
      if (!n) return std::nullopt;
      current->delay_ms = *n;
    } else if (word == "end") {
      scenario.responses.push_back(std::move(*current));
      current.reset();
    } else {
      return std::nullopt;
    }
  }
  if (current) return std::nullopt;
  return scenario;
}

inline const Response* select(const Scenario& scenario, const std::string_view url, const std::optional<std::uint64_t> range) {
  for (const auto& response : scenario.responses) {
    if (response.url == url && response.range == range) return &response;
  }
  return nullptr;
}

inline std::string render_head(const Response& response) {
  std::string out;
  for (const auto& block : response.blocks) {
    out += "HTTP/1.1 " + std::to_string(block.status) + (block.reason.empty() ? "" : " " + block.reason) + "\r\n";
    for (const auto& [name, value] : block.headers) out += name + ": " + value + "\r\n";
    out += "\r\n";
  }
  return out;
}

inline std::string render_body(const Response& response, const std::string_view file_bytes) {
  std::string body = response.body_file.empty() ? response.body_text : std::string{file_bytes};
  if (response.truncate_at && *response.truncate_at < body.size()) body.resize(*response.truncate_at);
  body.append(response.exceed_by, 'X');
  return body;
}

struct ParsedArgv final {
  bool probe{false};
  std::uint64_t connect_timeout{0};
  std::uint64_t stall_timeout{0};
  std::uint64_t max_time{0};
  std::optional<std::string> proxy;
  std::optional<std::string> resolve;
  std::optional<std::string> cacert;
  std::optional<std::uint64_t> range;
};

// Exactly spec §3.3 (args exclude argv[0]); "#" marks a number slot.
inline std::optional<ParsedArgv> parse_curl_argv(const std::span<const std::string> args) {
  ParsedArgv parsed;
  if (args.size() == 2 && args[0] == "-q" && args[1] == "--version") {
    parsed.probe = true;
    return parsed;
  }
  static const std::vector<std::string> fixed{
      "-q", "--config", "-", "--silent", "--include", "--suppress-connect-headers", "--globoff", "--proto", "=https",
      "--max-redirs", "0", "--tlsv1.2", "--connect-timeout", "#", "--speed-limit", "1024", "--speed-time", "#",
      "--max-time", "#", "--user-agent", "strata", "--header", "Accept-Encoding: identity"};
  if (args.size() < fixed.size() + 2) return std::nullopt;
  std::vector<std::uint64_t> numbers;
  for (std::size_t i = 0; i < fixed.size(); ++i) {
    if (fixed[i] == "#") {
      const auto n = to_u64(args[i]);
      if (!n || *n == 0) return std::nullopt;
      numbers.push_back(*n);
    } else if (args[i] != fixed[i]) {
      return std::nullopt;
    }
  }
  parsed.connect_timeout = numbers[0];
  parsed.stall_timeout = numbers[1];
  parsed.max_time = numbers[2];
  std::size_t i = fixed.size();
  if (args[i] == "--noproxy" && args[i + 1] == "*") {
    i += 2;
    if (i + 1 < args.size() && args[i] == "--resolve") {
      parsed.resolve = args[i + 1];
      i += 2;
    }
  } else if (args[i] == "--proxy") {
    parsed.proxy = args[i + 1];
    i += 2;
  } else {
    return std::nullopt;
  }
  if (i + 1 < args.size() && args[i] == "--range") {
    // "lo-" (resume, open-ended) or "lo-hi" (a bounded parallel piece); `range` is lo either way.
    const std::string_view value = args[i + 1];
    const auto dash = value.find('-');
    if (dash == std::string_view::npos) return std::nullopt;
    parsed.range = to_u64(value.substr(0, dash));
    if (!parsed.range) return std::nullopt;
    const std::string_view hi = value.substr(dash + 1);
    if (!hi.empty() && !to_u64(hi)) return std::nullopt;  // a non-empty high must be a number
    i += 2;
  }
  if (i + 1 < args.size() && args[i] == "--cacert") {
    parsed.cacert = args[i + 1];
    i += 2;
  }
  if (i != args.size()) return std::nullopt;
  return parsed;
}

inline std::optional<std::string> parse_config_line(const std::string_view input) {
  constexpr std::string_view kPrefix = "url = \"";
  if (!input.starts_with(kPrefix) || !input.ends_with("\"\n")) return std::nullopt;
  const auto url = input.substr(kPrefix.size(), input.size() - kPrefix.size() - 2);
  if (url.find_first_of("\"\n\\") != std::string_view::npos) return std::nullopt;
  return std::string{url};
}

inline std::string json_escape(const std::string_view text) {
  std::string out;
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          static constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[(static_cast<unsigned char>(c) >> 4U) & 0xFU]);
          out.push_back(kHex[static_cast<unsigned char>(c) & 0xFU]);
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

inline std::string record_line(const std::string_view kind, const std::span<const std::string> args,
                               const std::span<const std::string> env, const std::string_view stdin_bytes,
                               const std::span<const std::pair<std::string, std::string>> extras) {
  const auto list = [](const std::span<const std::string> items) {
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i) out += (i ? ",\"" : "\"") + json_escape(items[i]) + "\"";
    return out + "]";
  };
  std::string line = "{\"kind\":\"" + json_escape(kind) + "\",\"argv\":" + list(args) + ",\"env\":" + list(env) +
                     ",\"stdin\":\"" + json_escape(stdin_bytes) + "\"";
  for (const auto& [key, value] : extras) line += ",\"" + json_escape(key) + "\":\"" + json_escape(value) + "\"";
  return line + "}\n";
}

}  // namespace secure_frontend_test::curl
