#pragma once

#include "secure_frontend/engine_protocol_client.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace test_support {

// One DUMP report from strata-supervision-fake-engine (format: SP2 plan, Task 2).
struct FakeDump final {
  std::vector<int> fds;                 // POSIX: open descriptors
  unsigned long windows_pid{0};         // Windows: HANDLES <pid>
  std::vector<std::string> argv;
  std::vector<std::string> env;         // sorted NAME=value
  std::string sig_blocked;              // POSIX SigBlk, 16 hex digits
  std::string sig_ignored;              // POSIX SigIgn, 16 hex digits
  std::string cwd;
  std::map<std::string, std::string, std::less<>> ids;
};

inline std::optional<std::string> from_hex(const std::string_view hex) {
  if (hex.size() % 2U != 0U) return std::nullopt;
  const auto nibble = [](const char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  std::string out;
  for (std::size_t index = 0; index < hex.size(); index += 2U) {
    const int high = nibble(hex[index]);
    const int low = nibble(hex[index + 1U]);
    if (high < 0 || low < 0) return std::nullopt;
    out.push_back(static_cast<char>((high << 4) | low));
  }
  return out;
}

inline std::optional<std::vector<std::string>> hex_list(const std::string_view payload) {
  std::vector<std::string> items;
  if (payload.empty()) return items;
  std::size_t begin = 0;
  for (;;) {
    const auto comma = payload.find(',', begin);
    const auto item = from_hex(payload.substr(begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin));
    if (!item) return std::nullopt;
    items.push_back(*item);
    if (comma == std::string_view::npos) return items;
    begin = comma + 1U;
  }
}

inline std::map<std::string, std::string, std::less<>> key_values(const std::string_view payload) {
  std::map<std::string, std::string, std::less<>> values;
  std::size_t begin = 0;
  while (begin < payload.size()) {
    auto end = payload.find(' ', begin);
    if (end == std::string_view::npos) end = payload.size();
    const auto pair = payload.substr(begin, end - begin);
    const auto equals = pair.find('=');
    if (equals != std::string_view::npos) values.emplace(std::string(pair.substr(0, equals)), std::string(pair.substr(equals + 1U)));
    begin = end + 1U;
  }
  return values;
}

// Parses the report lines up to (not including) END.
inline std::optional<FakeDump> parse_dump(const std::vector<std::string>& lines) {
  FakeDump dump;
  for (const auto& line : lines) {
    const auto space = line.find(' ');
    const std::string_view tag = std::string_view(line).substr(0, space);
    const std::string_view payload = space == std::string::npos ? std::string_view{} : std::string_view(line).substr(space + 1U);
    if (tag == "FDS") {
      std::size_t begin = 0;
      while (begin < payload.size()) {
        auto end = payload.find(',', begin);
        if (end == std::string_view::npos) end = payload.size();
        dump.fds.push_back(std::stoi(std::string(payload.substr(begin, end - begin))));
        begin = end + 1U;
      }
    } else if (tag == "HANDLES") {
      dump.windows_pid = std::stoul(std::string(payload));
    } else if (tag == "ARGV" || tag == "ENV") {
      const auto items = hex_list(payload);
      if (!items) return std::nullopt;
      (tag == "ARGV" ? dump.argv : dump.env) = *items;
    } else if (tag == "SIG") {
      const auto values = key_values(payload);
      if (const auto found = values.find("blk"); found != values.end()) dump.sig_blocked = found->second;
      if (const auto found = values.find("ign"); found != values.end()) dump.sig_ignored = found->second;
    } else if (tag == "CWD") {
      const auto cwd = from_hex(payload);
      if (!cwd) return std::nullopt;
      dump.cwd = *cwd;
    } else if (tag == "IDS") {
      dump.ids = key_values(payload);
    } else {
      return std::nullopt;
    }
  }
  return dump;
}

// Sends DUMP and reads the report (at most 16 lines, each within `timeout`).
inline std::optional<FakeDump> request_dump(secure_frontend::EngineLineTransport& transport, const std::chrono::milliseconds timeout) {
  std::string error;
  if (!transport.write_line("DUMP", secure_frontend::deadline_after(timeout), error)) return std::nullopt;
  std::vector<std::string> lines;
  for (int count = 0; count < 16; ++count) {
    const auto result = transport.read_line(timeout);
    if (result.kind != secure_frontend::EngineReadKind::line) return std::nullopt;
    if (result.line == "END") return parse_dump(lines);
    lines.push_back(result.line);
  }
  return std::nullopt;
}

// Reads a report written with --dump-file=<path>, waiting up to `timeout` for it.
inline std::optional<FakeDump> read_dump_file(const std::filesystem::path& path, const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    std::ifstream input(path, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line == "END") return parse_dump(lines);
      lines.push_back(line);
    }
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

}  // namespace test_support
