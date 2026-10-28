#include "secure_frontend/chunked_stream_sink.hpp"

#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/http_wire.hpp"
#include "secure_frontend/sse_encoder.hpp"

#include <algorithm>
#include <utility>

namespace secure_frontend {

HttpHead make_sse_head() {
  HttpHead head;
  head.status = 200;
  head.headers = {{"Content-Type", "text/event-stream; charset=utf-8"}, {"Transfer-Encoding", "chunked"}, {"X-Accel-Buffering", "no"}};
  return head;  // Cache-Control, Connection: close and the §4.6 headers come from serialize_head
}

ChunkedStreamSink::ChunkedStreamSink(StreamSocket& socket, const StreamSinkTimeouts timeouts, std::optional<std::string> allow_origin,
                                     std::function<Clock::time_point()> now)
    : socket_(socket), timeouts_(timeouts), now_(std::move(now)), allow_origin_(std::move(allow_origin)) {}

void ChunkedStreamSink::append(std::string bytes) {
  if (outbound_.size() == offset_) last_progress_ = now_();  // the stall clock starts with new pending output
  outbound_ += bytes;
}

bool ChunkedStreamSink::flush(const std::chrono::milliseconds wait) {
  const auto until = now_() + wait;
  while (!failed_ && offset_ < outbound_.size()) {
    const auto chunk = std::string_view(outbound_).substr(offset_, std::min(api_constants::kMaxSendChunk, outbound_.size() - offset_));
    const SendStep step = socket_.send_some(chunk);
    if (step.status == SendStatus::progressed && step.sent > 0U) {
      offset_ += step.sent;
      bytes_sent_ += step.sent;
      last_progress_ = now_();
      continue;
    }
    if (step.status == SendStatus::failed) {
      failed_ = true;
      break;
    }
    const auto now = now_();
    if (now - last_progress_ >= timeouts_.write_stall) {
      failed_ = true;
      stalled_ = true;
      break;
    }
    if (now >= until) break;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - now);
    (void)socket_.wait_writable(std::max(std::min(left, std::chrono::milliseconds(api_constants::kStreamPollSlice)), std::chrono::milliseconds(1)));
  }
  if (offset_ == outbound_.size()) {
    outbound_.clear();
    offset_ = 0U;
  }
  return !failed_ && outbound_.empty();
}

bool ChunkedStreamSink::send_headers(const HttpHead& head) {
  if (failed_ || headers_sent_) return false;
  headers_sent_ = true;
  HttpHead with_cors = head;  // the same CORS rule as M1's SocketStreamSink (spec §4.4: SSE follows complete responses)
  if (allow_origin_) {
    with_cors.headers.emplace_back("Access-Control-Allow-Origin", *allow_origin_);
    with_cors.headers.emplace_back("Vary", "Origin");
  }
  status_ = head.status;
  append(serialize_head(with_cors, std::chrono::system_clock::now()));
  (void)flush(std::chrono::milliseconds(0));
  return !failed_;
}

bool ChunkedStreamSink::send_event(const std::string_view event) {
  if (failed_) return false;
  append(frame_chunk(event));
  (void)flush(std::chrono::milliseconds(0));
  return !failed_;
}

bool ChunkedStreamSink::writable(const std::chrono::milliseconds wait) { return flush(wait); }

bool ChunkedStreamSink::client_gone() {
  if (!failed_ && socket_.peer_gone()) failed_ = true;
  return failed_;
}

void ChunkedStreamSink::finish() {
  if (failed_) return;
  append(std::string(kChunkedTerminator));
  std::optional<Clock::time_point> shutdown_seen;
  while (!failed_ && !outbound_.empty()) {  // bounded by the stall rule ...
    if (timeouts_.shutting_down && timeouts_.shutting_down()) {  // ... and, once shutdown is seen, by kShutdownDrainBudget (R-H)
      if (!shutdown_seen) shutdown_seen = now_();
      if (now_() >= *shutdown_seen + api_constants::kShutdownDrainBudget) return;  // cut: finished_ stays false
    }
    (void)flush(std::chrono::milliseconds(api_constants::kStreamPollSlice));
  }
  finished_ = !failed_ && outbound_.empty();
}

void ChunkedStreamSink::set_log_code(const std::string_view code) { log_code_ = code; }  // fixed literals only

bool ChunkedStreamSink::failed() const noexcept { return failed_; }
bool ChunkedStreamSink::stalled() const noexcept { return stalled_; }
std::size_t ChunkedStreamSink::pending_bytes() const noexcept { return outbound_.size() - offset_; }
int ChunkedStreamSink::status() const noexcept { return status_; }
std::size_t ChunkedStreamSink::bytes() const noexcept { return bytes_sent_; }
bool ChunkedStreamSink::finished() const noexcept { return finished_; }
std::string_view ChunkedStreamSink::log_code() const noexcept { return log_code_; }

}  // namespace secure_frontend
