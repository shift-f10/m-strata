#include "secure_frontend/chat_stream.hpp"

#include "secure_frontend/api_constants.hpp"

#include <algorithm>
#include <deque>
#include <optional>
#include <string>

namespace secure_frontend {
namespace {

using Clock = std::chrono::steady_clock;

// How long a sub-16 KiB delta may be held mid-stream before it goes out anyway
// (ruling t11-age). Well under the 2 s STOP bounds of the DOS-02 cancel tests.
constexpr auto kPartialFlushHold = std::chrono::milliseconds(500);

// Holds decoded text and renders events only when the sink can take one
// without blocking: the engine is read at engine speed (spec §2.7).
// Ruling t8-unused: members are carried only where they are read (clang
// -Wunused-private-field under -Werror).
class StreamObserver final : public GenerationObserver {
 public:
  StreamObserver(StreamSink& sink, const SseIdentity& identity, const ChatStreamSettings& settings, const std::function<bool()>& shutting_down)
      : sink_(sink), identity_(identity), settings_(settings), shutting_down_(shutting_down) {}

  void on_text(const std::string_view text) override { queue(Channel::content, text); }
  void on_reasoning(const std::string_view text) override { queue(Channel::reasoning, text); }

  RunCancel on_tick(bool /*progress*/) override {
    if (shutting_down_()) {
      if (!shutdown_seen_) shutdown_seen_ = Clock::now();
      return RunCancel::shutting_down;
    }
    if (sink_.client_gone()) return gone_reason();
    flush_ready();
    if (sink_.client_gone()) return gone_reason();
    // Keepalive (spec §3.2): only while nothing is pending and the sink takes
    // it now — a comment never queues behind or interleaves with content.
    if (pending_.empty() && Clock::now() - last_activity_ >= settings_.keepalive && sink_.writable(std::chrono::milliseconds(0))) {
      (void)sink_.send_event(kSseKeepAliveComment);
      last_activity_ = Clock::now();
    }
    return RunCancel::none;
  }

  // Ruling t11-age (reconciles t10-coalesce with Task 11's cancellation tests):
  // a tick sends a FULL 16 KiB delta whenever one is available, and a PARTIAL
  // delta only after it has waited kPartialFlushHold — so a reading client gets
  // timely deltas (DOS-02 disconnect tests: 3 content chunks on a 4 KiB stream)
  // while a never-reading client sees full deltas only (the ≤ 80-event DOS-02
  // bound holds deterministically: pending crosses 16 KiB long before the hold
  // expires). Partial tails always flush in flush_all. blocked_since_ tracks
  // how long output was refused (write_stall vs client_gone). Issue 19: a
  // reasoning span followed by answer text (or the reverse) is finished, so it
  // goes out without waiting for the hold.
  void flush_ready() {
    for (;;) {
      if (!sink_.writable(std::chrono::milliseconds(0))) {
        if (!blocked_since_) blocked_since_ = Clock::now();
        return;
      }
      blocked_since_.reset();
      if (pending_.empty()) return;
      if (pending_.front().text.size() >= api_constants::kMaxDeltaTextBytes || pending_.size() > 1U ||
          Clock::now() - pending_since_ >= kPartialFlushHold) {
        send_one();
        continue;
      }
      return;
    }
  }

  // write_stall when output was refused for the whole stall window, else client_gone (§8 log codes).
  [[nodiscard]] RunCancel gone_reason() const {
    return blocked_since_ && Clock::now() - *blocked_since_ >= settings_.write_stall ? RunCancel::write_stall : RunCancel::client_gone;
  }

  // Waits (in poll slices) until the sink can take an event; false on deadline or gone.
  bool wait_writable(const Clock::time_point until) {
    while (!sink_.writable(api_constants::kStreamPollSlice)) {
      if (shutting_down_()) {
        if (!shutdown_seen_) shutdown_seen_ = Clock::now();
        if (Clock::now() >= *shutdown_seen_ + api_constants::kShutdownDrainBudget) return false;  // R-H: cut, no terminal chunk
      }
      if (Clock::now() >= until || sink_.client_gone()) return false;
    }
    return true;
  }

  bool flush_all(const Clock::time_point until) {
    while (!pending_.empty()) {
      if (!wait_writable(until)) return false;
      send_one();
    }
    return true;
  }

  bool send(const std::string_view event, const Clock::time_point until) { return wait_writable(until) && sink_.send_event(event); }

  // First moment the shutdown probe turned true (R-H bounds every later wait from here).
  [[nodiscard]] std::optional<Clock::time_point> shutdown_seen() const { return shutdown_seen_; }

 private:
  enum class Channel { content, reasoning };
  struct Segment final {
    Channel channel;
    std::string text;
  };

  // Decoded text in arrival order: one segment per run of one channel.
  void queue(const Channel channel, const std::string_view text) {
    if (pending_.empty()) pending_since_ = Clock::now();
    if (pending_.empty() || pending_.back().channel != channel) pending_.push_back(Segment{channel, std::string{}});
    pending_.back().text += text;
  }

  void send_one() {
    Segment& front = pending_.front();
    const std::size_t size = next_delta_size(front.text);
    const std::string_view delta = std::string_view(front.text).substr(0, size);
    (void)sink_.send_event(front.channel == Channel::reasoning ? render_reasoning_event(identity_, delta)
                                                               : render_content_event(identity_, delta));
    front.text.erase(0, size);
    if (front.text.empty()) pending_.pop_front();
    last_activity_ = Clock::now();
  }

  StreamSink& sink_;
  const SseIdentity& identity_;
  const ChatStreamSettings& settings_;
  const std::function<bool()>& shutting_down_;
  std::deque<Segment> pending_;  // decoded text, bounded by limits.chat.max_generated_bytes
  Clock::time_point pending_since_{};
  std::optional<Clock::time_point> blocked_since_;
  std::optional<Clock::time_point> shutdown_seen_;
  // Constructed after the role chunk is queued, so the keepalive timer starts there (Task 12).
  Clock::time_point last_activity_{Clock::now()};
};

}  // namespace

ChatStreamOutcome run_chat_stream(StreamSink& sink, EngineProtocolClient& protocol, const TokenByteDecoder& decoder,
                                  const SseIdentity& identity, const ChatStreamSettings& settings,
                                  const std::function<bool()>& shutting_down, const GenerationEnded& generation_ended) {
  ChatStreamOutcome outcome;
  (void)sink.send_headers(make_sse_head());
  (void)sink.send_event(render_role_event(identity));
  StreamObserver observer(sink, identity, settings, shutting_down);  // after the role chunk: the keepalive timer starts here
  GenerationRunResult run =
      drive_generation(protocol, decoder, settings.run, settings.rounds != nullptr ? settings.rounds->observer(observer) : observer);
  std::size_t prompt_tokens = settings.prompt_tokens;
  if (settings.rounds != nullptr) {
    // Web search: the rounds share this stream and the lease; their tokens add up, the usage names the last prompt.
    std::size_t completion_tokens = run.completion_tokens;
    const auto alive = [&observer] { return observer.on_tick(false) == RunCancel::none; };
    const auto event = [&observer, &settings](const std::string_view text) {
      const auto until = Clock::now() + settings.tail_deadline;
      return observer.flush_all(until) && observer.send(text, until);
    };
    for (;;) {
      const StreamRounds::Next next = settings.rounds->next(run, alive, event);
      if (next.ended.has_value()) {
        run = *next.ended;
        break;
      }
      if (!next.limits.has_value()) break;
      prompt_tokens = next.prompt_tokens;
      run = drive_generation(protocol, decoder, *next.limits, settings.rounds->observer(observer));
      completion_tokens += run.completion_tokens;
    }
    run.completion_tokens = completion_tokens;
  }
  generation_ended(run);  // the Lease is gone from here on: the next generation may start
  outcome.end = run.end;
  if (run.timed_out) outcome.log_code = "generation_timeout";
  if (run.cancel == RunCancel::shutting_down || shutting_down()) {
    outcome.log_code = "server_shutting_down";
    // R-H: bounded by what is left of kShutdownDrainBudget since shutdown was first seen, never by the 60 s tail deadline.
    const auto seen = observer.shutdown_seen().value_or(Clock::now());
    const auto until = std::min(seen + api_constants::kShutdownDrainBudget, Clock::now() + settings.tail_deadline);
    if (!observer.flush_all(until) || !observer.send(render_error_event(ApiError::server_shutting_down), until) ||
        !observer.send(kSseDoneEvent, until) || !observer.wait_writable(until)) {
      return outcome;
    }
    sink.finish();
    outcome.terminal_chunk_sent = sink.writable(std::chrono::milliseconds(0));
    return outcome;
  }
  if (run.end == RunEnd::cancelled && (run.cancel == RunCancel::client_gone || run.cancel == RunCancel::write_stall)) {
    outcome.log_code = run.cancel == RunCancel::write_stall ? "write_stall" : "client_gone";
    return outcome;  // nothing more is written to a gone client (spec §2.6)
  }
  const auto until = Clock::now() + settings.tail_deadline;
  if (run.end == RunEnd::engine_error || run.end == RunEnd::engine_unavailable || run.end == RunEnd::undecodable) {
    const ApiError code = run.end == RunEnd::engine_error ? ApiError::engine_error
                              : run.end == RunEnd::engine_unavailable ? ApiError::engine_unavailable
                                                                      : ApiError::internal_error;
    // Issue 20: an undecodable token keeps the client's generic internal_error; the log says why.
    outcome.log_code = run.end == RunEnd::undecodable ? std::string_view{"undecodable_token"} : api_error_row(code).code;
    if (!observer.flush_all(until) || !observer.send(render_error_event(code), until) || !observer.send(kSseDoneEvent, until) ||
        !observer.wait_writable(until)) {
      return outcome;
    }
    sink.finish();
    outcome.terminal_chunk_sent = sink.writable(std::chrono::milliseconds(0));
    return outcome;
  }
  if (run.end != RunEnd::stop && run.end != RunEnd::length) return outcome;  // shutdown: Task 14
  if (!observer.flush_all(until)) return outcome;
  if (!observer.send(render_final_event(identity, run.end == RunEnd::stop ? "stop" : "length"), until)) return outcome;
  if (settings.include_usage && !observer.send(render_usage_event(identity, prompt_tokens, run.completion_tokens), until)) return outcome;
  if (!observer.send(kSseDoneEvent, until) || !observer.wait_writable(until)) return outcome;
  sink.finish();
  outcome.terminal_chunk_sent = sink.writable(std::chrono::milliseconds(0));
  return outcome;
}

}  // namespace secure_frontend
