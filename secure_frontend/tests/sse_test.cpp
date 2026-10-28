#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/chat_stream.hpp"
#include "secure_frontend/chunked_stream_sink.hpp"
#include "secure_frontend/sse_encoder.hpp"
#include "secure_frontend/utf8_stream_decoder.hpp"
#include "support/test_support.hpp"
#include "support/test_tokenizer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using secure_frontend::Utf8StreamDecoder;

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}
const std::string kR{secure_frontend::kReplacementCharacter};

void utf8_split_across_tokens_is_held_then_released() {
  Utf8StreamDecoder decoder;
  expect(decoder.push("A\xE2") == "A", "ASCII released, a lead byte held");
  expect(decoder.held_bytes() == 1U, "one byte held");
  expect(decoder.push("\x82").empty(), "still incomplete");
  expect(decoder.push("\xAC") == "\xE2\x82\xAC", "the euro sign released whole");
  expect(decoder.push("\xF0\x9F").empty() && decoder.push("\x98\x80!") == "\xF0\x9F\x98\x80!", "4-byte sequence across tokens");
  expect(decoder.finish(false).empty() && decoder.held_bytes() == 0U, "nothing held at the end");
}

void utf8_invalid_sequence_becomes_replacement_character() {
  Utf8StreamDecoder decoder;
  expect(decoder.push("a\x80" "b") == "a" + kR + "b", "lone continuation byte");
  expect(decoder.push("\xC0\xAF") == kR + kR, "overlong lead C0 and its continuation");
  expect(decoder.push("\xE2\x82" "A") == kR + "A", "truncated sequence then ASCII: one U+FFFD");
  expect(decoder.push("\xED\xA0\x80") == kR + kR + kR, "surrogate encoding");
  expect(decoder.push("\xF5\xFF") == kR + kR, "bytes that never start a sequence");
  expect(decoder.push("\xF4\x90\x80\x80") == kR + kR + kR + kR, "above U+10FFFF");
  expect(decoder.push("ok") == "ok", "the decoder continues after invalid input");
}

void utf8_incomplete_tail_dropped_on_length_and_cancel() {
  Utf8StreamDecoder decoder;
  expect(decoder.push("ok\xF0\x9F\x98") == "ok", "three bytes of an emoji held");
  expect(decoder.finish(true).empty(), "length/cancel end: the cut tail is dropped");
  expect(decoder.held_bytes() == 0U, "finish resets the decoder");
}

void utf8_incomplete_tail_on_stop_becomes_one_replacement() {
  Utf8StreamDecoder decoder;
  expect(decoder.push("ok\xE2\x82") == "ok", "two bytes held");
  expect(decoder.finish(false) == kR, "stop end: the tail is one U+FFFD");
}

// SP3 M2 Task 6: the SSE encoder.
const secure_frontend::SseIdentity kIdentity{"chatcmpl-0123456789abcdef0123456789abcdef", 1760000000, "fake-model"};

// True when the only LF bytes are the event's own "\n\n" terminator.
bool single_line_event(const std::string& event) {
  return event.size() >= 2U && event.compare(event.size() - 2U, 2U, "\n\n") == 0 && event.find('\n') == event.size() - 2U;
}

void sse_events_never_contain_raw_newline() {
  const std::string hostile = "a\nb\r\nc\x01\"\\\xE2\x80\xA8 end";
  const std::vector<std::string> events{
      secure_frontend::render_role_event(kIdentity), secure_frontend::render_content_event(kIdentity, hostile),
      secure_frontend::render_final_event(kIdentity, "stop"), secure_frontend::render_usage_event(kIdentity, 3, 4),
      secure_frontend::render_error_event(secure_frontend::ApiError::engine_error)};
  for (const auto& event : events) {
    expect(single_line_event(event), "one line per event: " + event);
    expect(event.rfind("data: {", 0) == 0, "every event is a data line");
    expect(event.find('\r') == std::string::npos, "no raw CR either");
  }
  expect(events[1].find(R"("content":"a\nb\r\nc\u0001\"\\)") != std::string::npos, "escaped by the shared writer: " + events[1]);
  expect(events[4] == R"(data: {"error":{"message":")" + std::string(secure_frontend::api_error_row(secure_frontend::ApiError::engine_error).message) +
                         R"(","type":"server_error","param":null,"code":"engine_error"}})" "\n\n",
         "OpenAI error shape with the fixed §7.2 message");
}

void sse_coalesced_control_heavy_text_respects_event_cap() {
  const std::string pattern = std::string("\"\\\x01\x1f") + "\xC3\xA9" + "\xF0\x9F\x98\x80" + "x";  // 4 escapes, 2-byte, 4-byte, ASCII
  std::string text;
  while (text.size() < 1048576U) text += pattern;
  std::string rebuilt;
  std::size_t events = 0;
  for (std::string_view rest = text; !rest.empty();) {
    const std::size_t size = secure_frontend::next_delta_size(rest);
    expect(size > 0U && size <= secure_frontend::api_constants::kMaxDeltaTextBytes, "delta within 16 KiB");
    secure_frontend::Utf8StreamDecoder check;
    const std::string_view delta = rest.substr(0, size);
    expect(check.push(delta) == delta && check.held_bytes() == 0U, "delta ends on a code-point boundary");
    const auto event = secure_frontend::render_content_event(kIdentity, delta);
    expect(event.size() <= secure_frontend::api_constants::kMaxSseEventBytes, "event within 128 KiB: " + std::to_string(event.size()));
    rebuilt += delta;
    rest.remove_prefix(size);
    ++events;
  }
  expect(rebuilt == text, "deltas concatenate to the text");
  expect(events >= 64U, "1 MiB is split into at least 64 deltas");
  const std::string worst(secure_frontend::api_constants::kMaxDeltaTextBytes, '\x01');
  expect(secure_frontend::render_content_event(kIdentity, worst).size() <= secure_frontend::api_constants::kMaxSseEventBytes, "6x escape growth fits the cap");
}

void sse_chunk_framing_units() {
  expect(secure_frontend::frame_chunk("data: [DONE]\n\n") == "e\r\ndata: [DONE]\n\n\r\n", "hex size, CRLF, bytes, CRLF");
  expect(secure_frontend::kChunkedTerminator == "0\r\n\r\n", "terminal chunk");
}

// SP3 M2 Task 7: the chunked sink.
using namespace std::chrono_literals;

class FakeStreamSocket final : public secure_frontend::StreamSocket {
 public:
  std::string wire;
  std::size_t accept_budget{std::numeric_limits<std::size_t>::max()};
  bool fail_sends{false};
  bool gone{false};
  std::chrono::steady_clock::time_point clock{};
  secure_frontend::SendStep send_some(const std::string_view bytes) override {
    if (fail_sends) return {secure_frontend::SendStatus::failed, 0U};
    const std::size_t n = std::min(bytes.size(), accept_budget);
    if (n == 0U) return {secure_frontend::SendStatus::would_block, 0U};
    if (accept_budget != std::numeric_limits<std::size_t>::max()) accept_budget -= n;
    wire.append(bytes.substr(0, n));
    return {secure_frontend::SendStatus::progressed, n};
  }
  bool wait_writable(const std::chrono::milliseconds timeout) override {
    clock += timeout;
    return accept_budget > 0U;
  }
  bool peer_gone() override { return gone; }
};

secure_frontend::ChunkedStreamSink make_sink(FakeStreamSocket& socket, const std::chrono::milliseconds stall = 500ms) {
  return secure_frontend::ChunkedStreamSink(socket, {stall}, std::nullopt, [&socket] { return socket.clock; });
}

void sse_chunked_framing_and_terminal_chunk() {
  FakeStreamSocket socket;
  auto sink = make_sink(socket);
  expect(sink.send_headers(secure_frontend::make_sse_head()), "head sent");
  const auto head_end = socket.wire.find("\r\n\r\n");
  const std::string head = socket.wire.substr(0, head_end + 4U);
  expect(head.rfind("HTTP/1.1 200 ", 0) == 0, "status line: " + head);
  expect(head.find("\r\nContent-Type: text/event-stream; charset=utf-8\r\n") != std::string::npos, "SSE content type");
  expect(head.find("\r\nTransfer-Encoding: chunked\r\n") != std::string::npos, "chunked (decision 5)");
  expect(head.find("\r\nX-Accel-Buffering: no\r\n") != std::string::npos, "proxy buffering off");
  expect(head.find("Content-Length") == std::string::npos, "a stream has no Content-Length");
  expect(sink.send_event("data: a\n\n") && sink.send_event(secure_frontend::kSseDoneEvent), "events sent");
  sink.finish();
  expect(socket.wire.substr(head.size()) == "9\r\ndata: a\n\n\r\ne\r\ndata: [DONE]\n\n\r\n0\r\n\r\n", "one chunk per event, then 0\\r\\n\\r\\n");
  expect(sink.writable(0ms) && !sink.client_gone(), "a finished stream is clean");
  expect(sink.finished() && sink.status() == 200 && sink.bytes() == socket.wire.size(), "accessors deliver_route_result reads");
  FakeStreamSocket cors_socket;
  secure_frontend::ChunkedStreamSink cors(cors_socket, {500ms}, std::string("http://app.example:3000"), [&cors_socket] { return cors_socket.clock; });
  (void)cors.send_headers(secure_frontend::make_sse_head());
  expect(cors_socket.wire.find("\r\nAccess-Control-Allow-Origin: http://app.example:3000\r\n") != std::string::npos &&
             cors_socket.wire.find("\r\nVary: Origin\r\n") != std::string::npos, "the decision's CORS origin reaches the stream head");
  expect(socket.wire.find("Access-Control-Allow-Origin") == std::string::npos, "no CORS header without an allowed origin");
}

void sink_buffers_on_would_block_and_flushes_later() {
  FakeStreamSocket socket;
  auto sink = make_sink(socket);
  socket.accept_budget = 5U;
  expect(sink.send_event("data: hello\n\n"), "a partial send is not a failure");
  expect(!sink.writable(0ms) && sink.pending_bytes() > 0U, "the rest is pending");
  socket.accept_budget = std::numeric_limits<std::size_t>::max();
  expect(sink.writable(20ms) && sink.pending_bytes() == 0U, "flushed once the socket drains");
  expect(socket.wire == "d\r\ndata: hello\n\n\r\n", "bytes are complete and in order");
}

void sink_write_stall_marks_client_gone() {
  FakeStreamSocket socket;
  auto sink = make_sink(socket, 500ms);
  socket.accept_budget = 0U;
  (void)sink.send_event("data: x\n\n");
  for (int poll = 0; poll < 40 && !sink.failed(); ++poll) (void)sink.writable(20ms);
  expect(sink.failed() && sink.stalled() && sink.client_gone(), "no progress for write_stall fails the sink");
  const auto before = socket.wire.size();
  socket.accept_budget = std::numeric_limits<std::size_t>::max();
  sink.finish();
  expect(socket.wire.size() == before, "a failed sink sends no terminal chunk");
}

void sink_send_failure_and_peer_probe_are_gone() {
  FakeStreamSocket failing;
  auto broken = make_sink(failing);
  failing.fail_sends = true;
  expect(!broken.send_event("data: x\n\n") && broken.client_gone() && !broken.stalled(), "a send error is gone, not a stall");
  FakeStreamSocket closed;
  auto sink = make_sink(closed);
  closed.gone = true;
  expect(sink.client_gone(), "peer EOF, reset or a stray byte is gone");
}

// SP3 M2 Task 8: the stream pump. Ruling t8-test (as t4-test/t5-test): M1's
// write_line carries the absolute write deadline and begin_generation the GEN
// write deadline; the brief's shorter helper signatures are adapted, assertions unchanged.

// Thread-safe (mandatory: test threads append while the pump reads). "<close>" reads as an engine exit.
class ScriptedTransport final : public secure_frontend::EngineLineTransport {
 public:
  void push(std::string line) {
    std::lock_guard lock(mutex_);
    lines_.push_back(std::move(line));
  }
  [[nodiscard]] std::size_t stop_count() const {
    std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(std::count(writes_.begin(), writes_.end(), "STOP"));
  }
  bool write_line(const std::string_view line, const secure_frontend::MonotonicClock::time_point, std::string&) override {
    std::lock_guard lock(mutex_);
    writes_.emplace_back(line);
    return true;
  }
  secure_frontend::EngineReadResult read_line(const std::chrono::milliseconds timeout) override {
    std::string line;
    {
      std::lock_guard lock(mutex_);
      if (!lines_.empty()) {
        line = std::move(lines_.front());
        lines_.pop_front();
      }
    }
    if (line.empty()) {
      std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(5)));
      return {secure_frontend::EngineReadKind::timeout, {}, {}};
    }
    if (line == "<close>") return {secure_frontend::EngineReadKind::closed, {}, {}};
    return {secure_frontend::EngineReadKind::line, std::move(line), {}};
  }

 private:
  mutable std::mutex mutex_;
  std::deque<std::string> lines_;
  std::vector<std::string> writes_;
};

// Records accepted events; `blocked_polls` makes writable() report false that many times.
class RecordingSink final : public secure_frontend::StreamSink {
 public:
  std::vector<std::string> events;
  bool headers{false};
  bool finished{false};
  std::atomic<int> blocked_polls{0};
  std::atomic<bool> gone{false};
  bool violated{false};      // send_event after writable() returned false
  bool last_writable{true};
  bool send_headers(const secure_frontend::HttpHead& head) override { headers = head.status == 200; return true; }
  bool send_event(const std::string_view event) override {
    if (!last_writable) violated = true;
    events.emplace_back(event);
    return !gone;
  }
  bool writable(const std::chrono::milliseconds wait) override {
    if (blocked_polls > 0) {
      --blocked_polls;
      std::this_thread::sleep_for(wait);
      return last_writable = false;
    }
    return last_writable = !gone;
  }
  bool client_gone() override { return gone; }
  void finish() override { finished = true; }
};

struct StreamRun final {
  ScriptedTransport transport;
  secure_frontend::EngineProtocolClient protocol{transport};
  secure_frontend::TokenizerPack pack = test_support::byte_tokenizer();
  RecordingSink sink;
  int ended_calls{0};
  explicit StreamRun(std::vector<std::string> records) {
    transport.push("READY 4096 stop");
    secure_frontend::EngineReady ready{};
    (void)protocol.await_ready(std::chrono::seconds(1), ready);
    (void)protocol.begin_generation(64, {1, 2, 3}, secure_frontend::deadline_after(std::chrono::seconds(5)));
    for (auto& record : records) transport.push(std::move(record));
  }
  secure_frontend::ChatStreamOutcome run(secure_frontend::ChatStreamSettings settings = {},
                                         std::function<bool()> shutting_down = [] { return false; }) {
    const secure_frontend::TokenByteDecoder decoder(pack);
    return secure_frontend::run_chat_stream(sink, protocol, decoder, kIdentity, settings, shutting_down,
                                            [this](const secure_frontend::GenerationRunResult&) { ++ended_calls; });
  }
  [[nodiscard]] std::string text() const {
    std::string all;
    for (const auto& event : sink.events) {
      const auto marker = event.find(R"("delta":{"content":")");
      if (marker == std::string::npos) continue;
      const auto start = marker + 20U;
      all += event.substr(start, event.find('"', start) - start);  // the tests use plain ASCII text
    }
    return all;
  }
};

std::string tok(const unsigned char byte) { return "T " + std::to_string(test_support::id_for_byte(byte)); }

void sse_role_content_final_done_sequence() {
  StreamRun stream({tok('H'), tok('i'), "DONE 2 3 1.0 1.0 stop"});
  const auto outcome = stream.run();
  const auto& events = stream.sink.events;
  expect(stream.sink.headers && events.size() >= 4U, "head, role, content, final, DONE");
  expect(events.front() == secure_frontend::render_role_event(kIdentity), "role chunk first");
  expect(events[events.size() - 2U] == secure_frontend::render_final_event(kIdentity, "stop"), "final chunk with stop");
  expect(events.back() == secure_frontend::kSseDoneEvent, "[DONE] last");
  expect(stream.text() == "Hi", "content deltas in order: " + stream.text());
  expect(stream.sink.finished && outcome.terminal_chunk_sent && outcome.end == secure_frontend::RunEnd::stop, "terminal chunk");
  expect(stream.ended_calls == 1 && !stream.sink.violated, "engine released once; never sent while unwritable");
}

void sse_include_usage_adds_usage_chunk() {
  StreamRun stream({tok('H'), tok('i'), "DONE 2 3 1.0 1.0 length"});
  secure_frontend::ChatStreamSettings settings;
  settings.include_usage = true;
  settings.prompt_tokens = 3U;
  (void)stream.run(settings);
  const auto& events = stream.sink.events;
  expect(events.size() >= 5U && events[events.size() - 2U] == secure_frontend::render_usage_event(kIdentity, 3U, 2U), "usage before [DONE]");
  expect(events[events.size() - 3U] == secure_frontend::render_final_event(kIdentity, "length"), "final chunk before usage");
  expect(events[events.size() - 2U].find(R"("choices":[],"usage":{"prompt_tokens":3,"completion_tokens":2,"total_tokens":5})") != std::string::npos,
         "OpenAI usage chunk shape");
}

void sse_ids_and_created_constant_per_stream() {
  StreamRun stream({tok('a'), tok('b'), tok('c'), "DONE 3 3 1.0 1.0 stop"});
  (void)stream.run();
  const std::string id = R"("id":")" + kIdentity.id + R"(","object":"chat.completion.chunk","created":1760000000,"model":"fake-model")";
  for (std::size_t index = 0; index + 1U < stream.sink.events.size(); ++index) {
    expect(stream.sink.events[index].find(id) != std::string::npos, "same id, created and model in every chunk");
  }
}

void stream_with_no_tokens_has_role_final_done() {
  StreamRun stream({"DONE 0 3 1.0 1.0 stop"});
  const auto outcome = stream.run();
  const auto& events = stream.sink.events;
  expect(events.size() == 3U && events[0] == secure_frontend::render_role_event(kIdentity) &&
             events[1] == secure_frontend::render_final_event(kIdentity, "stop") && events[2] == secure_frontend::kSseDoneEvent,
         "role, final stop, [DONE]; no empty content chunk");
  expect(outcome.terminal_chunk_sent, "a zero-token stream is a normal end");
}

void stream_slow_reader_coalesces_deltas() {
  std::vector<std::string> records;
  for (int index = 0; index < 200; ++index) records.push_back(tok('a'));
  records.emplace_back("DONE 200 3 1.0 1.0 stop");
  StreamRun stream(std::move(records));
  stream.sink.blocked_polls = 1000000;  // the reader takes nothing while the engine runs ...
  std::thread reader([&stream] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stream.sink.blocked_polls = 0;    // ... then drains
  });
  const auto outcome = stream.run();
  reader.join();
  std::size_t content = 0;
  for (const auto& event : stream.sink.events) content += event.find(R"("delta":{"content")") != std::string::npos ? 1U : 0U;
  expect(stream.text() == std::string(200U, 'a'), "all text delivered");
  expect(content <= 2U, "200 tokens coalesce into at most 2 content events: " + std::to_string(content));
  expect(stream.ended_calls == 1 && outcome.terminal_chunk_sent && !stream.sink.violated, "normal end");
}

void stream_tail_deadline_cuts_without_terminal_chunk() {
  StreamRun stream({tok('a'), "DONE 1 3 1.0 1.0 stop"});
  stream.sink.blocked_polls = 1000000;
  secure_frontend::ChatStreamSettings settings;
  settings.tail_deadline = std::chrono::milliseconds(100);
  const auto outcome = stream.run(settings);
  expect(!outcome.terminal_chunk_sent && !stream.sink.finished, "tail deadline passed: no terminal chunk");
  expect(stream.ended_calls == 1, "the engine was released before the tail");
}

void stream_client_gone_sends_stop() {
  std::vector<std::string> records;
  for (int index = 0; index < 5; ++index) records.push_back(tok('a'));
  StreamRun stream(std::move(records));  // no DONE until STOP
  std::thread closer([&stream] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stream.sink.gone = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stream.transport.push("DONE 5 3 1.0 1.0 cancel");
  });
  const auto outcome = stream.run();
  closer.join();
  expect(stream.transport.stop_count() == 1U, "client gone sends STOP");
  expect(outcome.end == secure_frontend::RunEnd::cancelled && outcome.log_code == "client_gone" && !outcome.terminal_chunk_sent,
         "cancelled, logged as client_gone, no terminal chunk");
  expect(!stream.sink.finished && stream.ended_calls == 1, "nothing more written; engine released");
}

void keepalive_never_splits_a_pending_event() {
  {  // long prefill: comments, then content
    StreamRun stream({});
    std::thread engine([&stream] {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      stream.transport.push(tok('a'));
      stream.transport.push("DONE 1 3 1.0 1.0 stop");
    });
    secure_frontend::ChatStreamSettings settings;
    settings.keepalive = std::chrono::milliseconds(100);
    (void)stream.run(settings);
    engine.join();
    std::size_t comments = 0;
    for (const auto& event : stream.sink.events) {
      if (event == secure_frontend::kSseKeepAliveComment) ++comments;
      if (event.find(R"("delta":{"content")") != std::string::npos) break;
    }
    expect(comments >= 2U, "comments while no content flows: " + std::to_string(comments));
  }
  {  // blocked reader with pending text: no comment is queued or interleaved
    std::vector<std::string> records{tok('a')};
    StreamRun stream(std::move(records));
    stream.sink.blocked_polls = 1000000;
    std::thread engine([&stream] {
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
      stream.sink.blocked_polls = 0;
      stream.transport.push("DONE 1 3 1.0 1.0 stop");
    });
    secure_frontend::ChatStreamSettings settings;
    settings.keepalive = std::chrono::milliseconds(50);
    (void)stream.run(settings);
    engine.join();
    expect(!stream.sink.violated, "nothing is sent while the sink is not writable");
    const auto content = std::find_if(stream.sink.events.begin(), stream.sink.events.end(),
                                      [](const std::string& e) { return e.find(R"("delta":{"content")") != std::string::npos; });
    expect(content != stream.sink.events.end() && std::find(stream.sink.events.begin() + 1, content, std::string(secure_frontend::kSseKeepAliveComment)) == content,
           "pending content goes out before any comment");
  }
}

void stream_error_events_follow_text_and_end_with_done() {
  const auto check = [](std::vector<std::string> records, const secure_frontend::ApiError code, const std::string& name) {
    StreamRun stream(std::move(records));
    const auto outcome = stream.run();
    const auto& events = stream.sink.events;
    expect(events.size() >= 3U && events[events.size() - 2U] == secure_frontend::render_error_event(code), name + ": error event");
    expect(events.back() == secure_frontend::kSseDoneEvent && stream.sink.finished && outcome.terminal_chunk_sent, name + ": [DONE] + terminal");
    for (const auto& event : events) expect(event.find(R"("finish_reason":")") == std::string::npos, name + ": no finish_reason after an error");
    expect(stream.text() == "a", name + ": text before the failure was delivered");
  };
  check({tok('a'), "ERR boom"}, secure_frontend::ApiError::engine_error, "ERR");
  check({tok('a'), "DONE 1 3 1.0 1.0 nonfinite"}, secure_frontend::ApiError::engine_error, "nonfinite");
  check({tok('a'), "<close>"}, secure_frontend::ApiError::engine_unavailable, "engine exit");
  check({tok('a'), "T 256", "DONE 2 3 1.0 1.0 cancel"}, secure_frontend::ApiError::internal_error, "control token in output");
}

void stream_shutdown_sends_error_event_and_done() {
  std::vector<std::string> records{tok('a')};
  StreamRun stream(std::move(records));
  std::atomic<bool> stopping{false};
  std::thread signal([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stopping = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stream.transport.push("DONE 1 3 1.0 1.0 cancel");
  });
  const auto outcome = stream.run({}, [&stopping] { return stopping.load(); });
  signal.join();
  const auto& events = stream.sink.events;
  expect(events.size() >= 3U && events[events.size() - 2U] == secure_frontend::render_error_event(secure_frontend::ApiError::server_shutting_down),
         "server_shutting_down event");
  expect(events.back() == secure_frontend::kSseDoneEvent && outcome.terminal_chunk_sent, "[DONE] and the terminal chunk");
  expect(stream.transport.stop_count() == 1U, "the generation was stopped");
  expect(outcome.log_code == "server_shutting_down", "log code");
}

// Issue 19: a reasoning delta is {"reasoning_content": text} (upstream serve/server.py:5449-5450).
void sse_reasoning_event_shape() {
  const auto event = secure_frontend::render_reasoning_event(kIdentity, "a\"b");
  expect(event == R"(data: {"id":"chatcmpl-0123456789abcdef0123456789abcdef","object":"chat.completion.chunk","created":1760000000,)"
                  R"("model":"fake-model","choices":[{"index":0,"delta":{"reasoning_content":"a\"b"},"logprobs":null,"finish_reason":null}]})"
                  "\n\n",
         "reasoning delta shape: " + event);
  const std::string worst(secure_frontend::api_constants::kMaxDeltaTextBytes, '\x01');
  expect(secure_frontend::render_reasoning_event(kIdentity, worst).size() <= secure_frontend::api_constants::kMaxSseEventBytes,
         "6x escape growth fits the cap for reasoning too");
}

void stream_sends_reasoning_deltas_before_the_answer() {
  auto pack = test_support::byte_tokenizer();
  pack.tokens_by_id.emplace_back("<think>");
  pack.token_types.push_back(4);
  pack.tokens_by_id.emplace_back("</think>");
  pack.token_types.push_back(4);
  const secure_frontend::ThinkTokens think{pack.tokens_by_id.size() - 2U, pack.tokens_by_id.size() - 1U};
  StreamRun stream({tok('h'), tok('m'), "T " + std::to_string(think.close), tok('\n'), tok('O'), tok('K'), "DONE 6 3 1.0 1.0 stop"});
  stream.pack = pack;
  secure_frontend::ChatStreamSettings settings;
  settings.run.think_tokens = think;
  settings.run.starts_in_reasoning = true;
  const auto outcome = stream.run(settings);
  std::string reasoning;
  std::size_t first_reasoning = stream.sink.events.size();
  std::size_t first_content = stream.sink.events.size();
  for (std::size_t index = 0; index < stream.sink.events.size(); ++index) {
    const auto& event = stream.sink.events[index];
    expect(event.find("think>") == std::string::npos, "no event carries a think tag");
    const auto marker = event.find(R"("delta":{"reasoning_content":")");
    if (marker != std::string::npos) {
      const auto start = marker + 30U;
      reasoning += event.substr(start, event.find('"', start) - start);
      first_reasoning = std::min(first_reasoning, index);
    }
    if (event.find(R"("delta":{"content":")") != std::string::npos && index > 0U) first_content = std::min(first_content, index);
  }
  expect(outcome.end == secure_frontend::RunEnd::stop && outcome.terminal_chunk_sent, "the stream ends normally");
  expect(reasoning == "hm" && stream.text() == "OK", "reasoning [" + reasoning + "] then answer [" + stream.text() + "]");
  expect(first_reasoning < first_content, "reasoning deltas go out before the answer's");
}

// Issue 26: the end-of-turn token ends the stream with a stop chunk, never an error.
void stream_ends_at_the_end_of_turn_token() {
  StreamRun probe({});
  std::size_t im_end = probe.pack.tokens_by_id.size();
  for (std::size_t id = 0; id < probe.pack.tokens_by_id.size(); ++id) {
    if (probe.pack.tokens_by_id[id] == "<|im_end|>") im_end = id;
  }
  StreamRun stream({tok('H'), tok('i'), "T " + std::to_string(im_end), "DONE 3 3 1.0 1.0 stop"});
  secure_frontend::ChatStreamSettings settings;
  settings.run.stop_tokens = {im_end};
  const auto outcome = stream.run(settings);
  const auto& events = stream.sink.events;
  expect(outcome.end == secure_frontend::RunEnd::stop && outcome.terminal_chunk_sent, "the stream ends as stop");
  expect(stream.text() == "Hi" && events.size() >= 2U && events[events.size() - 2U] == secure_frontend::render_final_event(kIdentity, "stop"),
         "answer then the stop chunk: " + stream.text());
  for (const auto& event : events) expect(event.find(R"("error")") == std::string::npos, "no error event");
}

// Web search (spec 2026-10-27-web-search-design §2): a search event is a chunk with an empty delta and the event as
// strata_search, as upstream's strata_mcp chunks are.
void sse_search_event_shape() {
  const auto event = secure_frontend::render_search_event(kIdentity, R"({"event":"limit","max_rounds":8})");
  expect(event == R"(data: {"id":"chatcmpl-0123456789abcdef0123456789abcdef","object":"chat.completion.chunk","created":1760000000,)"
                  R"("model":"fake-model","choices":[{"index":0,"delta":{},"logprobs":null,"finish_reason":null}],)"
                  R"("strata_search":{"event":"limit","max_rounds":8}})"
                  "\n\n",
         "search chunk shape: " + event);
}

// The search loop's rounds: one stream, one engine release at the very end, an event between the rounds after the
// text before it, the usage of all rounds (the last round's prompt).
struct FakeRounds final : secure_frontend::StreamRounds {
  StreamRun* stream{nullptr};
  std::vector<std::string> round_two;
  std::optional<secure_frontend::GenerationRunResult> fail;  // the answer ends as this after the first round
  int nexts{0};
  int observed{0};
  bool alive_ok{false};
  bool event_ok{false};
  secure_frontend::GenerationObserver& observer(secure_frontend::GenerationObserver& client) override {
    ++observed;
    return client;
  }
  Next next(const secure_frontend::GenerationRunResult& run, const std::function<bool()>& alive,
            const std::function<bool(std::string_view)>& event) override {
    ++nexts;
    if (nexts > 1 || run.end != secure_frontend::RunEnd::stop) return {};
    alive_ok = alive();
    event_ok = event(secure_frontend::render_search_event(kIdentity, R"({"event":"call"})"));
    if (fail.has_value()) return Next{{}, 0U, fail};
    (void)stream->protocol.begin_generation(64, {1, 2, 3, 4, 5}, secure_frontend::deadline_after(std::chrono::seconds(5)));
    for (auto& record : round_two) stream->transport.push(record);
    secure_frontend::GenerationRunLimits limits;
    limits.max_tokens = 64U;
    return Next{limits, 5U, std::nullopt};
  }
};

void stream_rounds_share_one_stream_and_one_release() {
  StreamRun stream({tok('A'), "DONE 1 3 1.0 1.0 stop"});
  FakeRounds rounds;
  rounds.stream = &stream;
  rounds.round_two = {tok('B'), tok('C'), "DONE 2 5 1.0 1.0 stop"};
  secure_frontend::ChatStreamSettings settings;
  settings.include_usage = true;
  settings.prompt_tokens = 3U;
  settings.rounds = &rounds;
  const auto outcome = stream.run(settings);
  const auto& events = stream.sink.events;
  expect(outcome.end == secure_frontend::RunEnd::stop && outcome.terminal_chunk_sent, "the answer ends normally");
  expect(rounds.nexts == 2 && rounds.observed == 2 && rounds.alive_ok && rounds.event_ok, "two rounds, each observed, then asked what follows");
  expect(stream.text() == "ABC", "both rounds' text: " + stream.text());
  const auto at = [&events](const std::string_view needle) {
    for (std::size_t i = 0; i < events.size(); ++i) {
      if (events[i].find(needle) != std::string::npos) return i;
    }
    return events.size();
  };
  expect(at(R"("content":"A")") < at(R"("strata_search")") && at(R"("strata_search")") < at(R"("content":"B)"),
         "the first round's text, the event, the second round's text");
  expect(events.size() >= 3U && events[events.size() - 2U] == secure_frontend::render_usage_event(kIdentity, 5U, 3U),
         "usage: the last round's prompt, every round's tokens");
  expect(stream.ended_calls == 1, "the engine is released once, after the last round");
}

void stream_rounds_can_end_the_answer() {
  StreamRun stream({tok('A'), "DONE 1 3 1.0 1.0 stop"});
  FakeRounds rounds;
  rounds.stream = &stream;
  secure_frontend::GenerationRunResult failed;
  failed.end = secure_frontend::RunEnd::engine_error;
  rounds.fail = failed;
  secure_frontend::ChatStreamSettings settings;
  settings.rounds = &rounds;
  const auto outcome = stream.run(settings);
  const auto& events = stream.sink.events;
  expect(outcome.end == secure_frontend::RunEnd::engine_error && outcome.log_code == "engine_error", "the answer ends as the failure");
  expect(events.size() >= 2U && events[events.size() - 2U] == secure_frontend::render_error_event(secure_frontend::ApiError::engine_error) &&
             events.back() == secure_frontend::kSseDoneEvent,
         "an error event, then [DONE]");
  expect(stream.ended_calls == 1, "released once");
}

}  // namespace

int main(int argc, char* argv[]) {
  return secure_frontend_test::run_cases(argc, argv,
                                         {
                                             {"utf8_split_across_tokens_is_held_then_released", utf8_split_across_tokens_is_held_then_released},
                                             {"sse_reasoning_event_shape", sse_reasoning_event_shape},
                                             {"stream_sends_reasoning_deltas_before_the_answer", stream_sends_reasoning_deltas_before_the_answer},
                                             {"stream_ends_at_the_end_of_turn_token", stream_ends_at_the_end_of_turn_token},
                                             {"sse_search_event_shape", sse_search_event_shape},
                                             {"stream_rounds_share_one_stream_and_one_release", stream_rounds_share_one_stream_and_one_release},
                                             {"stream_rounds_can_end_the_answer", stream_rounds_can_end_the_answer},
                                             {"utf8_invalid_sequence_becomes_replacement_character", utf8_invalid_sequence_becomes_replacement_character},
                                             {"utf8_incomplete_tail_dropped_on_length_and_cancel", utf8_incomplete_tail_dropped_on_length_and_cancel},
                                             {"utf8_incomplete_tail_on_stop_becomes_one_replacement", utf8_incomplete_tail_on_stop_becomes_one_replacement},
                                             {"sse_events_never_contain_raw_newline", sse_events_never_contain_raw_newline},
                                             {"sse_coalesced_control_heavy_text_respects_event_cap", sse_coalesced_control_heavy_text_respects_event_cap},
                                             {"sse_chunk_framing_units", sse_chunk_framing_units},
                                             {"sse_chunked_framing_and_terminal_chunk", sse_chunked_framing_and_terminal_chunk},
                                             {"sink_buffers_on_would_block_and_flushes_later", sink_buffers_on_would_block_and_flushes_later},
                                             {"sink_write_stall_marks_client_gone", sink_write_stall_marks_client_gone},
                                             {"sink_send_failure_and_peer_probe_are_gone", sink_send_failure_and_peer_probe_are_gone},
                                             {"sse_role_content_final_done_sequence", sse_role_content_final_done_sequence},
                                             {"sse_include_usage_adds_usage_chunk", sse_include_usage_adds_usage_chunk},
                                             {"sse_ids_and_created_constant_per_stream", sse_ids_and_created_constant_per_stream},
                                             {"stream_with_no_tokens_has_role_final_done", stream_with_no_tokens_has_role_final_done},
                                             {"stream_slow_reader_coalesces_deltas", stream_slow_reader_coalesces_deltas},
                                             {"stream_tail_deadline_cuts_without_terminal_chunk", stream_tail_deadline_cuts_without_terminal_chunk},
                                             {"stream_client_gone_sends_stop", stream_client_gone_sends_stop},
                                             {"keepalive_never_splits_a_pending_event", keepalive_never_splits_a_pending_event},
                                             {"stream_error_events_follow_text_and_end_with_done", stream_error_events_follow_text_and_end_with_done},
                                             {"stream_shutdown_sends_error_event_and_done", stream_shutdown_sends_error_event_and_done},
                                         },
                                         failures);
}
