#pragma once

#include "secure_frontend/chunked_stream_sink.hpp"
#include "secure_frontend/generation_run.hpp"
#include "secure_frontend/sse_encoder.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>

namespace secure_frontend {

// Web search (spec 2026-10-27-web-search-design §2): further generations inside one stream and one engine lease - the
// search loop's rounds. Each generation reports to observer() (the round's parser before the stream's own observer);
// after each one, next() says what follows. While next() works (the searches), `alive` keeps the stream going - the
// pending text out, keepalives, the client and the shutdown checked (false: stop waiting) - and `event` writes one event
// after the text before it (false: the client is gone).
class StreamRounds {
 public:
  virtual ~StreamRounds() = default;
  [[nodiscard]] virtual GenerationObserver& observer(GenerationObserver& stream) = 0;
  struct Next final {
    std::optional<GenerationRunLimits> limits{};  // the next generation's, its GEN written; unset: the answer is complete
    std::size_t prompt_tokens{0};                 // that generation's prompt (the usage chunk's)
    std::optional<GenerationRunResult> ended{};   // the answer ends as this instead (a cancel or an engine failure)
  };
  [[nodiscard]] virtual Next next(const GenerationRunResult& run, const std::function<bool()>& alive,
                                  const std::function<bool(std::string_view)>& event) = 0;
};

struct ChatStreamSettings final {
  GenerationRunLimits run{};
  std::chrono::milliseconds write_stall{std::chrono::seconds(10)};    // limits.http.write_stall
  std::chrono::milliseconds tail_deadline{std::chrono::minutes(1)};   // limits.http.response_write_deadline
  std::chrono::milliseconds keepalive{std::chrono::seconds(15)};      // limits.http.stream_keepalive
  bool include_usage{false};
  std::size_t prompt_tokens{0};
  StreamRounds* rounds{nullptr};  // web search: the rounds after the first generation (null: one generation)
};

struct ChatStreamOutcome final {
  RunEnd end{RunEnd::engine_unavailable};
  bool terminal_chunk_sent{false};
  std::string_view log_code{"-"};  // §8: "-", a §7.2 code, client_gone, write_stall, generation_timeout or undecodable_token
};

// Called once, as soon as the engine finished (DONE, failure or cancel), before the tail is flushed.
// The protocol must not be used after it returns (the caller destroys its Lease there).
using GenerationEnded = std::function<void(const GenerationRunResult&)>;

// The streamed chat (spec §2.7, §3.2): headers and the role chunk, then the
// engine read at engine speed through drive_generation, decoded text held
// until the sink can take it (coalesced into ≤ 16 KiB deltas), then — after
// the engine was released — the tail under the response-write deadline:
// final chunk, optional usage, [DONE], terminal chunk. Precondition: GEN was
// written (ChatExecutionService::begin succeeded).
[[nodiscard]] ChatStreamOutcome run_chat_stream(StreamSink& sink, EngineProtocolClient& protocol, const TokenByteDecoder& decoder,
                                                const SseIdentity& identity, const ChatStreamSettings& settings,
                                                const std::function<bool()>& shutting_down, const GenerationEnded& generation_ended);

}  // namespace secure_frontend
