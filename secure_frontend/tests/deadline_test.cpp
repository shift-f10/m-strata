#include "secure_frontend/deadline.hpp"
#include "secure_frontend/engine_protocol_client.hpp"
#include "secure_frontend/engine_stdio_process.hpp"

#include "support/test_paths.hpp"

#include <chrono>
#include <deque>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

class CountingTransport final : public secure_frontend::EngineLineTransport {
 public:
  std::deque<secure_frontend::EngineReadResult> reads;
  int read_calls{0};
  bool write_line(std::string_view, secure_frontend::MonotonicClock::time_point, std::string&) override { return true; }
  secure_frontend::EngineReadResult read_line(std::chrono::milliseconds) override {
    ++read_calls;
    if (reads.empty()) return {secure_frontend::EngineReadKind::timeout, {}, {}};
    auto result = std::move(reads.front());
    reads.pop_front();
    return result;
  }
};

secure_frontend::EngineReadResult line(std::string text) { return {secure_frontend::EngineReadKind::line, std::move(text), {}}; }

void deadline_zero_timeout_is_immediate_everywhere() {
  using secure_frontend::EngineProtocolErrorCode;
  const auto now = std::chrono::steady_clock::now();
  expect(secure_frontend::remaining_until(secure_frontend::deadline_after(0ms, now), now) == 0ms, "zero budget: nothing remains");
  expect(secure_frontend::remaining_until(secure_frontend::deadline_after(-5ms, now), now) == 0ms, "a negative budget is zero");
  expect(secure_frontend::remaining_until(secure_frontend::deadline_after(1500ms, now), now) == 1500ms, "a positive budget is kept");
  expect(secure_frontend::remaining_until(now, now + 1ms) == 0ms, "a passed deadline leaves zero, never a negative value");

  // await_ready: expired before the first read; READY stays queued.
  CountingTransport transport;
  transport.reads = {line("READY 4096 stop")};
  secure_frontend::EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  const auto ready_error = client.await_ready(0ms, ready);
  expect(ready_error && ready_error->code == EngineProtocolErrorCode::timeout, "await_ready(0) is an immediate timeout");
  expect(transport.read_calls == 0 && transport.reads.size() == 1U, "await_ready(0) reads nothing");
  expect(!client.await_ready(1s, ready).has_value(), "the queued READY is still delivered afterwards");

  // read_generation: the same rule.
  transport.reads = {line("T 1"), line("DONE stop")};
  expect(!client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(), "GEN");
  const int calls_before = transport.read_calls;
  secure_frontend::EngineGeneration generation{};
  const auto generation_error = client.read_generation(0ms, generation);
  expect(generation_error && generation_error->code == EngineProtocolErrorCode::timeout, "read_generation(0) is an immediate timeout");
  expect(transport.read_calls == calls_before && transport.reads.size() == 2U, "read_generation(0) reads nothing");

  // read_line on the real transport: a buffered line is not consumed by a zero budget.
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value(), "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  std::string error;
  expect(child.write_line("GEN 1 1", secure_frontend::deadline_after(std::chrono::seconds(5)), error), "GEN");
  expect(child.read_line(5s).line == "T 7", "T 7 (the fake writes DONE in the same flush, so it waits in the buffer)");
  std::this_thread::sleep_for(100ms);
  expect(child.read_line(0ms).kind == secure_frontend::EngineReadKind::timeout, "read_line(0) is an immediate timeout even with a buffered line");
  expect(child.read_line(5s).line == "DONE stop", "the buffered line is still delivered afterwards");
  (void)child.terminate(250ms);
}

}  // namespace

int main(int argc, char* argv[]) {
  return secure_frontend_test::run_cases(argc, argv, {{"deadline_zero_timeout_is_immediate_everywhere", deadline_zero_timeout_is_immediate_everywhere}},
                                 failures);
}
