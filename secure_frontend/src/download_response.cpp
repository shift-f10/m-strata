#include "secure_frontend/download_response.hpp"

#include <algorithm>

namespace secure_frontend {
namespace {

constexpr bool is_digit(const char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool is_tchar(const char c) noexcept {
  return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         std::string_view{"!#$%&'*+-.^_`|~"}.find(c) != std::string_view::npos;
}

std::string lower(std::string_view text) {
  std::string out{text};
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

std::optional<int> parse_status_line(const std::string_view line) {
  std::string_view rest;
  for (const auto* prefix : {"HTTP/1.0 ", "HTTP/1.1 ", "HTTP/2 "}) {
    if (line.starts_with(prefix)) {
      rest = line.substr(std::string_view{prefix}.size());
      break;
    }
  }
  if (rest.size() < 3 || !is_digit(rest[0]) || !is_digit(rest[1]) || !is_digit(rest[2])) return std::nullopt;
  if (rest.size() > 3 && rest[3] != ' ') return std::nullopt;
  return (rest[0] - '0') * 100 + (rest[1] - '0') * 10 + (rest[2] - '0');
}

// block ends with "\r\n\r\n".
std::optional<ResponseHead> parse_block(const std::string_view block) {
  ResponseHead head;
  std::size_t start = 0;
  std::size_t lines = 0;
  const std::size_t end = block.size() - 2;  // drop the final empty line's CRLF
  while (start < end) {
    const auto stop = block.find("\r\n", start);
    if (stop == std::string_view::npos || stop >= end + 2) return std::nullopt;
    const auto line = block.substr(start, stop - start);
    if (++lines > kMaxHeaderLines) return std::nullopt;
    if (line.find('\n') != std::string_view::npos || line.find('\r') != std::string_view::npos) return std::nullopt;
    if (lines == 1) {
      const auto status = parse_status_line(line);
      if (!status) return std::nullopt;
      head.status = *status;
    } else {
      const auto colon = line.find(':');
      if (colon == std::string_view::npos || colon == 0) return std::nullopt;
      const auto name = line.substr(0, colon);
      if (!std::all_of(name.begin(), name.end(), is_tchar)) return std::nullopt;
      const auto value = trim(line.substr(colon + 1));
      if (std::any_of(value.begin(), value.end(), [](const char c) {
            return (static_cast<unsigned char>(c) < 0x20 && c != '\t') || c == 0x7F;
          })) {
        return std::nullopt;
      }
      head.headers.emplace_back(lower(name), std::string{value});
    }
    start = stop + 2;
  }
  return lines == 0 ? std::nullopt : std::optional<ResponseHead>{std::move(head)};
}

std::optional<std::uint64_t> parse_decimal(const std::string_view text) {
  if (text.empty() || text.size() > 19 || !std::all_of(text.begin(), text.end(), is_digit)) return std::nullopt;
  std::uint64_t value = 0;
  for (const char c : text) value = value * 10U + static_cast<std::uint64_t>(c - '0');
  return value;
}

HeadDecision fail(const AcquireReason reason, const int status, const bool transient = false) {
  return HeadDecision{HeadAction::fail, reason, transient, {}, status};
}

}  // namespace

std::vector<std::string_view> header_values(const ResponseHead& head, const std::string_view lower_name) {
  std::vector<std::string_view> values;
  for (const auto& [name, value] : head.headers) {
    if (name == lower_name) values.emplace_back(value);
  }
  return values;
}

ResponseHeadParser::Feed ResponseHeadParser::feed(const std::string_view bytes) {
  if (state_ != State::need_more) return {state_, 0};
  std::size_t used = 0;
  while (used < bytes.size()) {
    block_.push_back(bytes[used++]);
    if (block_.size() > kMaxHeaderBlockBytes) {
      state_ = State::malformed;
      return {state_, used};
    }
    if (block_.size() < 4 || block_.compare(block_.size() - 4, 4, "\r\n\r\n") != 0) continue;
    auto parsed = parse_block(block_);
    block_.clear();
    if (!parsed) {
      state_ = State::malformed;
      return {state_, used};
    }
    if (parsed->status >= 100 && parsed->status < 200) {
      if (++informational_ > kMaxInformationalBlocks) {
        state_ = State::malformed;
        return {state_, used};
      }
      continue;
    }
    head_ = std::move(*parsed);
    state_ = State::head_ready;
    return {state_, used};
  }
  return {state_, used};
}

HeadDecision classify_head(const ResponseHead& head, const HopExpectation& expectation) {
  const int status = head.status;
  const auto lengths = header_values(head, "content-length");
  if (lengths.size() > 1) return fail(AcquireReason::response_malformed, status);
  std::optional<std::uint64_t> length;
  if (lengths.size() == 1) {
    length = parse_decimal(lengths.front());
    if (!length) return fail(AcquireReason::response_malformed, status);
  }
  if (status == 200) {
    if (length && *length != expectation.file_size) return fail(AcquireReason::size_mismatch_announced, status);
    return HeadDecision{expectation.range_from ? HeadAction::restart_from_zero : HeadAction::write_body,
                        AcquireReason::response_malformed, false, {}, status};
  }
  if (status == 206) {
    if (!expectation.range_from) return fail(AcquireReason::range_mismatch, status);
    const auto ranges = header_values(head, "content-range");
    const std::string expected = "bytes " + std::to_string(*expectation.range_from) + "-" +
                                 std::to_string(expectation.file_size - 1) + "/" + std::to_string(expectation.file_size);
    if (ranges.size() != 1 || ranges.front() != expected) return fail(AcquireReason::range_mismatch, status);
    if (length && *length != expectation.file_size - *expectation.range_from) {
      return fail(AcquireReason::size_mismatch_announced, status);
    }
    return HeadDecision{HeadAction::write_body, AcquireReason::response_malformed, false, {}, status};
  }
  if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
    const auto locations = header_values(head, "location");
    if (locations.size() != 1) return fail(AcquireReason::response_malformed, status);
    return HeadDecision{HeadAction::redirect, AcquireReason::redirect_not_allowed, false, std::string{locations.front()}, status};
  }
  if (status == 416) return fail(AcquireReason::range_mismatch, status);
  if (status == 429 || (status >= 500 && status <= 599)) return fail(AcquireReason::http_status, status, true);
  return fail(AcquireReason::http_status, status);
}

}  // namespace secure_frontend
