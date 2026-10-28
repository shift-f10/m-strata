#pragma once

#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/api_errors.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

inline constexpr std::string_view kSseDoneEvent = "data: [DONE]\n\n";
inline constexpr std::string_view kSseKeepAliveComment = ": keep-alive\n\n";
inline constexpr std::string_view kChunkedTerminator = "0\r\n\r\n";

// id, created and model are fixed per stream (§3.2).
struct SseIdentity final {
  std::string id;
  std::int64_t created{0};
  std::string model;
};

// Every event is `data: <one-line JSON>\n\n`; text is escaped by SP1's
// append_json_string (spec §3.9), so no raw LF/CR appears inside an event.
[[nodiscard]] std::string render_role_event(const SseIdentity& identity);
// The next delta: at most api_constants::kMaxDeltaTextBytes bytes, ending on
// a code-point boundary.
[[nodiscard]] std::size_t next_delta_size(std::string_view text) noexcept;
[[nodiscard]] std::string render_content_event(const SseIdentity& identity, std::string_view text);
// A reasoning delta: {"reasoning_content": text} (upstream serve/server.py:5449-5450).
[[nodiscard]] std::string render_reasoning_event(const SseIdentity& identity, std::string_view text);
[[nodiscard]] std::string render_final_event(const SseIdentity& identity, std::string_view finish_reason);
// Web search: one search loop event (a JSON object, search_rounds.hpp) as a chunk with an empty delta and the event as
// strata_search, the way upstream sends its strata_mcp chunks.
[[nodiscard]] std::string render_search_event(const SseIdentity& identity, std::string_view event_json);
[[nodiscard]] std::string render_usage_event(const SseIdentity& identity, std::size_t prompt_tokens, std::size_t completion_tokens);
[[nodiscard]] std::string render_error_event(ApiError code);
// One HTTP/1.1 chunk: "<hex size>\r\n<bytes>\r\n".
[[nodiscard]] std::string frame_chunk(std::string_view bytes);

}  // namespace secure_frontend
