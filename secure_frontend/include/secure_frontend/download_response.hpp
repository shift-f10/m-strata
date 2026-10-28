#pragma once

#include "secure_frontend/acquire_reason.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace secure_frontend {

inline constexpr std::size_t kMaxHeaderBlockBytes = 16U * 1024U;
inline constexpr std::size_t kMaxHeaderLines = 64;
inline constexpr std::size_t kMaxInformationalBlocks = 4;

struct ResponseHead final {
  int status{0};
  std::vector<std::pair<std::string, std::string>> headers;  // names lowercased, values OWS-trimmed
};

[[nodiscard]] std::vector<std::string_view> header_values(const ResponseHead& head, std::string_view lower_name);

// Parses the header blocks curl writes with --include, skipping up to four 1xx
// blocks. Lines end with CRLF; status lines are HTTP/1.0, HTTP/1.1 or HTTP/2.
class ResponseHeadParser final {
 public:
  enum class State { need_more, head_ready, malformed };
  struct Feed final {
    State state;
    std::size_t consumed;  // bytes of this call that were header bytes
  };
  [[nodiscard]] Feed feed(std::string_view bytes);
  [[nodiscard]] const ResponseHead& head() const noexcept { return head_; }

 private:
  std::string block_;
  std::size_t informational_{0};
  ResponseHead head_;
  State state_{State::need_more};
};

struct HopExpectation final {
  std::uint64_t file_size;
  std::optional<std::uint64_t> range_from;
};

enum class HeadAction { write_body, restart_from_zero, redirect, fail };

struct HeadDecision final {
  HeadAction action;
  AcquireReason reason{AcquireReason::response_malformed};
  bool transient{false};  // 429 and 5xx are retried on the same source (§3.4)
  std::string location;   // raw Location value; parse with parse_location, never print
  int status{0};
};

// Spec §3.4 status table.
[[nodiscard]] HeadDecision classify_head(const ResponseHead& head, const HopExpectation& expectation);

}  // namespace secure_frontend
