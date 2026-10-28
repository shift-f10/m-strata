#include "secure_frontend/engine_protocol_client.hpp"
#include "support/test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using secure_frontend::EngineLineTransport;
using secure_frontend::EngineProtocolClient;
using secure_frontend::EngineProtocolErrorCode;
using secure_frontend::EngineReadKind;
using secure_frontend::EngineReadResult;

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

class FakeTransport final : public EngineLineTransport {
 public:
  std::deque<EngineReadResult> reads;
  std::vector<std::string> writes;
  std::vector<secure_frontend::MonotonicClock::time_point> deadlines;  // one per write attempt
  bool reject_writes{false};
  std::chrono::milliseconds delay_per_read{0};
  bool write_line(const std::string_view line, const secure_frontend::MonotonicClock::time_point deadline, std::string& error) override {
    deadlines.push_back(deadline);
    if (reject_writes) { error = "fake write failure"; return false; }
    writes.emplace_back(line); return true;
  }
  EngineReadResult read_line(std::chrono::milliseconds) override {
    if (delay_per_read.count() > 0) std::this_thread::sleep_for(delay_per_read);
    if (reads.empty()) return {EngineReadKind::timeout, {}, {}};
    auto result = std::move(reads.front()); reads.pop_front(); return result;
  }
};

EngineReadResult line(std::string value) { return {EngineReadKind::line, std::move(value), {}}; }

void runs_the_solo_protocol() {
  FakeTransport transport;
  transport.reads = {line("INFO engine=0.1.43"), line("READY 8192 stop"), line("T 12"), line("T 4294967295"), line("DONE 2 3 1.0 2.0 length")};
  EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  expect(!client.await_ready(std::chrono::seconds(1), ready).has_value(), "READY should parse");
  expect(ready.max_context_tokens == 8192 && ready.supports_stop, "READY facts should parse exactly");
  expect(!client.begin_generation(4, {1, 2, 3}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(), "GEN should be emitted");
  expect(transport.writes == std::vector<std::string>({"GEN 4 1,2,3"}), "GEN must have canonical literal syntax");
  secure_frontend::EngineGeneration generation;
  expect(!client.read_generation(std::chrono::seconds(1), generation).has_value(), "T/DONE sequence should parse");
  expect(generation.token_ids == std::vector<std::uint32_t>({12U, 4294967295U}), "all token ids should be collected");
  expect(generation.done_line == "DONE 2 3 1.0 2.0 length" && client.ready(), "DONE should restore idle state");
}

// SP5 M1 Task 12 (Ruling T12-i): the GENI variant — the same head, the request's combined embeddings file, then the
// ids: upstream's engine reads "GENI <max_new> [keys] <file> <id,id,...>" (engine/src/program/generate.cpp; its server
// writes it so, serve/server.py:2199). The older pin put the file last; the engine we ship refuses that (the owner's
// first real picture, 2026-10-28). With no embeddings the GEN line stays byte-for-byte today's.
void gen_line_unchanged_without_embeddings_geni_with_them() {
  {
    FakeTransport transport;
    transport.reads = {line("READY 8192 stop")};
    EngineProtocolClient client(transport);
    secure_frontend::EngineReady ready{};
    expect(!client.await_ready(std::chrono::seconds(1), ready).has_value(), "READY");
    expect(!client.begin_generation(4, {1, 2, 3}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(),
           "GEN (no embeddings) is emitted exactly as today");
    expect(transport.writes == std::vector<std::string>({"GEN 4 1,2,3"}), "GEN unchanged");
  }
  {
    FakeTransport transport;
    transport.reads = {line("READY 8192 stop")};
    EngineProtocolClient client(transport);
    secure_frontend::EngineReady ready{};
    expect(!client.await_ready(std::chrono::seconds(1), ready).has_value(), "READY");
    expect(!client.begin_generation(4, {1, 2, 3}, secure_frontend::deadline_after(std::chrono::seconds(5)), {},
                                    "vision/req-0123456789ab.sve")
                .has_value(),
           "GENI with an embeddings path is accepted");
    expect(transport.writes == std::vector<std::string>({"GENI 4 vision/req-0123456789ab.sve 1,2,3"}),
           "GENI renders upstream's order: head, the path, then the ids");
  }
  {
    // The path rides the same line cap: a line that would overrun the cap is refused
    // before anything is written (SP3 DOS rule extended to the new tail).
    FakeTransport transport;
    transport.reads = {line("READY 1 stop")};  // tiny context -> tiny effective cap
    EngineProtocolClient client(transport);
    secure_frontend::EngineReady ready{};
    expect(!client.await_ready(std::chrono::seconds(1), ready).has_value(), "READY");
    const auto oversize_path = "v/" + std::string(200U, 'x') + ".sve";
    const auto result = client.begin_generation(4, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)), {},
                                                oversize_path);
    expect(result.has_value() && transport.writes.empty(), "an over-long GENI line is refused, nothing written");
  }
}

void stop_and_quit_are_explicit() {
  FakeTransport transport;
  transport.reads = {line("READY 1024 stop"), line("T 7"), line("DONE 1 1 1 1 stop")};
  EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  (void)client.await_ready(std::chrono::seconds(1), ready);
  (void)client.begin_generation(2, {9}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  expect(!client.stop().has_value(), "STOP should be accepted during a generation");
  expect(!client.stop().has_value(), "a repeated STOP should be idempotent");
  secure_frontend::EngineGeneration generation;
  expect(!client.read_generation(std::chrono::seconds(1), generation).has_value(), "STOP must still drain through DONE");
  expect(!client.quit().has_value() && client.closed(), "QUIT should close a clean idle protocol");
  expect(transport.writes == std::vector<std::string>({"GEN 2 9", "STOP", "QUIT"}), "control writes must be exact");
}

void refuses_stop_when_not_advertised() {
  FakeTransport transport;
  transport.reads = {line("READY 1024")};
  EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  (void)client.await_ready(std::chrono::seconds(1), ready);
  (void)client.begin_generation(2, {9}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  const auto error = client.stop();
  expect(error.has_value() && error->code == EngineProtocolErrorCode::invalid_state,
         "STOP must not be sent to engines that did not advertise support");
  expect(transport.writes == std::vector<std::string>({"GEN 2 9"}), "unsupported STOP must not be written");
}

void rejects_malformed_and_unexpected_engine_output() {
  FakeTransport bad_ready;
  bad_ready.reads = {line("READY 0")};
  EngineProtocolClient client(bad_ready);
  secure_frontend::EngineReady ready{};
  const auto ready_error = client.await_ready(std::chrono::seconds(1), ready);
  expect(ready_error.has_value() && ready_error->code == EngineProtocolErrorCode::malformed_line && client.closed(),
         "zero READY context must fail closed");

  FakeTransport bad_token;
  bad_token.reads = {line("READY 1"), line("T 4 extra")};
  EngineProtocolClient second(bad_token);
  (void)second.await_ready(std::chrono::seconds(1), ready);
  (void)second.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  secure_frontend::EngineGeneration generation;
  const auto token_error = second.read_generation(std::chrono::seconds(1), generation);
  expect(token_error.has_value() && token_error->code == EngineProtocolErrorCode::malformed_line && second.closed(),
         "ambiguous token lines must fail closed");
}

void bounds_and_engine_errors_fail_closed() {
  FakeTransport transport;
  transport.reads = {line("READY 1"), line("ERR model rejected prompt")};
  EngineProtocolClient client(transport, {.max_line_bytes = 32, .max_prompt_tokens = 2, .max_generated_tokens = 1, .max_new_tokens = 4});
  secure_frontend::EngineReady ready{};
  (void)client.await_ready(std::chrono::seconds(1), ready);
  expect(client.begin_generation(5, {1}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(), "over-limit max_new must be rejected before write");
  expect(client.begin_generation(1, {1, 2, 3}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(), "over-limit prompt must be rejected before write");
  expect(!client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(), "bounded request should start");
  secure_frontend::EngineGeneration generation;
  const auto error = client.read_generation(std::chrono::seconds(1), generation);
  expect(error.has_value() && error->code == EngineProtocolErrorCode::engine_error && client.closed(),
         "ERR must terminate the protocol instead of being ignored");
}

void prefill_progress_is_bounded_in_both_profiles() {
  FakeTransport glm_transport;
  glm_transport.reads = {line("READY 64"), line("PP 16 32 10.5 1523.8"),
                         line("T 9"), line("DONE 1")};
  EngineProtocolClient glm_client(
      glm_transport, {}, secure_frontend::EngineProtocolFlavor::lighttransport_glm53);
  secure_frontend::EngineReady ready{};
  expect(!glm_client.await_ready(std::chrono::seconds(1), ready).has_value(),
         "GLM engine must still start with a normal READY record");
  expect(!glm_client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(), "GLM generation should start normally");
  secure_frontend::EngineGeneration generation;
  expect(!glm_client.read_generation(std::chrono::seconds(1), generation).has_value() &&
             generation.token_ids == std::vector<std::uint32_t>({9U}),
         "only a valid GLM PP heartbeat may occur before token output");

  // Strata profile, upstream v0.1.44.1 shapes verbatim: generate.cpp prints
  // `PP <position> <prompt tokens> <ms> <tok/s>` per batched chunk; the
  // qwen35moe engine prints `PP <done> <fresh> <ms> <tok/s>` where <fresh>
  // is not a total (held-state reuse), and the bare 3-field form is what
  // upstream's server minimally accepts. None of these may fail a request.
  FakeTransport qwen_transport;
  qwen_transport.reads = {line("READY 64"), line("PP 4096 8192 1234 3318.6"), line("PP 8192 8192 2469 3318.1"),
                          line("PP 300 12 7 42.0"), line("PP 16 32"), line("PP 1 2 3"), line("T 9"), line("DONE 1")};
  EngineProtocolClient qwen_client(qwen_transport);
  (void)qwen_client.await_ready(std::chrono::seconds(1), ready);
  (void)qwen_client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  expect(!qwen_client.read_generation(std::chrono::seconds(1), generation).has_value() &&
             generation.token_ids == std::vector<std::uint32_t>({9U}) && !qwen_client.closed(),
         "Strata-profile PP heartbeats in upstream's shapes are consumed silently before token output");

  for (const char* bad : {"PP x 32 1 2.0", "PP 16 32 -1 2.0", "PP 16 32 1 2.0 extra", "PP 16", "PP 16 32 1 nan"}) {
    FakeTransport bad_transport;
    bad_transport.reads = {line("READY 64"), line(bad), line("DONE 0")};
    EngineProtocolClient bad_client(bad_transport);
    (void)bad_client.await_ready(std::chrono::seconds(1), ready);
    (void)bad_client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
    const auto bad_error = bad_client.read_generation(std::chrono::seconds(1), generation);
    expect(bad_error.has_value() && bad_error->code == EngineProtocolErrorCode::unexpected_line && bad_client.closed(),
           std::string("Strata-profile PP outside the bounded shapes must fail closed: ") + bad);
  }

  FakeTransport glm_four_fields;
  glm_four_fields.reads = {line("READY 64"), line("PP 16 32 10"), line("DONE 0")};
  EngineProtocolClient glm_four_client(
      glm_four_fields, {}, secure_frontend::EngineProtocolFlavor::lighttransport_glm53);
  (void)glm_four_client.await_ready(std::chrono::seconds(1), ready);
  (void)glm_four_client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  const auto glm_four_error = glm_four_client.read_generation(std::chrono::seconds(1), generation);
  expect(glm_four_error.has_value() && glm_four_error->code == EngineProtocolErrorCode::unexpected_line &&
             glm_four_client.closed(),
         "GLM keeps its exact 3- or 5-field record");

  FakeTransport malformed_transport;
  malformed_transport.reads = {line("READY 64"), line("PP 33 32"), line("DONE 0")};
  EngineProtocolClient malformed_client(
      malformed_transport, {}, secure_frontend::EngineProtocolFlavor::lighttransport_glm53);
  (void)malformed_client.await_ready(std::chrono::seconds(1), ready);
  (void)malformed_client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  const auto malformed_error = malformed_client.read_generation(std::chrono::seconds(1), generation);
  expect(malformed_error.has_value() && malformed_error->code == EngineProtocolErrorCode::unexpected_line &&
             malformed_client.closed(),
         "malformed GLM progress must fail closed");
}

// The main engine's serve loop prints `RESUME <n>` before reading the prompt and
// `REUSED <n>` once it is read, for EVERY accepted GEN and unconditionally even
// when nothing is reused (`RESUME 0`): upstream v0.1.44.1 src/program/generate.cpp
// :11214/:11849, v0.2.1.3 :12809/:13910, v0.2.2 :12899/:14006. They are private progress records like
// PP and must never fail a request. The qwen35moe engine never prints them;
// YIELDED is unreachable because the frontend refuses the --batch/--slots flags
// that would produce it. The GLM profile keeps its exact record and refuses them.
void resume_and_reused_progress_is_consumed_in_the_strata_profile() {
  FakeTransport transport;
  transport.reads = {line("READY 64"), line("RESUME 0"), line("PP 4096 8192 1234 3318.6"),
                     line("REUSED 0"), line("T 9"), line("DONE 1 8192 1.0 2.0 length")};
  EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  (void)client.await_ready(std::chrono::seconds(1), ready);
  (void)client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  secure_frontend::EngineGeneration generation;
  expect(!client.read_generation(std::chrono::seconds(1), generation).has_value() &&
             generation.token_ids == std::vector<std::uint32_t>({9U}) && !client.closed(),
         "RESUME/REUSED records of the main engine family must be consumed silently");
  FakeTransport reuse_transport;
  reuse_transport.reads = {line("READY 64"), line("RESUME 2147483647"), line("REUSED 2147483647"),
                           line("T 9"), line("DONE 1")};
  EngineProtocolClient reuse_client(reuse_transport);
  (void)reuse_client.await_ready(std::chrono::seconds(1), ready);
  (void)reuse_client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  expect(!reuse_client.read_generation(std::chrono::seconds(1), generation).has_value(),
         "a held-state reuse count must be accepted as a decimal");
  for (const char* bad : {"RESUME", "RESUME x", "RESUME 1 2", "REUSED", "REUSED 1.5"}) {
    FakeTransport bad_transport;
    bad_transport.reads = {line("READY 64"), line(bad), line("DONE 0")};
    EngineProtocolClient bad_client(bad_transport);
    (void)bad_client.await_ready(std::chrono::seconds(1), ready);
    (void)bad_client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
    const auto error = bad_client.read_generation(std::chrono::seconds(1), generation);
    expect(error.has_value() && error->code == EngineProtocolErrorCode::unexpected_line && bad_client.closed(),
           std::string("RESUME/REUSED outside the bounded two-field decimal form must fail closed: ") + bad);
  }
  FakeTransport glm_transport;
  glm_transport.reads = {line("READY 64"), line("RESUME 0"), line("DONE 0")};
  EngineProtocolClient glm_client(glm_transport, {}, secure_frontend::EngineProtocolFlavor::lighttransport_glm53);
  (void)glm_client.await_ready(std::chrono::seconds(1), ready);
  (void)glm_client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  const auto glm_error = glm_client.read_generation(std::chrono::seconds(1), generation);
  expect(glm_error.has_value() && glm_error->code == EngineProtocolErrorCode::unexpected_line && glm_client.closed(),
         "the GLM profile keeps its exact record and must refuse RESUME/REUSED");
}

// Session log step 16 (listener.monitor): a full DONE carries the engine's own counts and
// timings to the Monitor. A short record, or a field that is not a number, leaves them empty
// and never fails the generation.
void done_figures_reach_the_done_event() {
  const auto done_of = [](const std::string& record) -> std::optional<secure_frontend::DoneEvent> {
    FakeTransport transport;
    transport.reads = {line("READY 64"), line("T 9"), line(record)};
    EngineProtocolClient client(transport);
    secure_frontend::EngineReady ready{};
    (void)client.await_ready(std::chrono::seconds(1), ready);
    (void)client.begin_generation(4, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
    (void)client.next_event(std::chrono::seconds(1));  // T 9
    const auto event = client.next_event(std::chrono::seconds(1));
    const auto* done = std::get_if<secure_frontend::DoneEvent>(&event);
    if (done == nullptr || client.closed()) return std::nullopt;
    return *done;
  };
  const auto full = done_of("DONE 1 8192 1250.5 400 stop 0 0 4096 7 9");
  expect(full.has_value() && full->finish == "stop" && full->figures.prompt_tokens == std::optional<std::size_t>{8192U} &&
             full->figures.prompt_ms == std::optional<double>{1250.5} && full->figures.decode_ms == std::optional<double>{400.0} &&
             full->figures.reused == std::optional<std::size_t>{4096U},
         "a full DONE gives prompt tokens, both timings and the reused count");
  const auto six = done_of("DONE 1 12 3.5 7 length");
  expect(six.has_value() && six->figures.prompt_tokens == std::optional<std::size_t>{12U} && !six->figures.reused.has_value(),
         "the six-field record has no reused count");
  for (const char* short_form : {"DONE 1", "DONE stop"}) {
    const auto done = done_of(short_form);
    expect(done.has_value() && !done->figures.prompt_tokens && !done->figures.prompt_ms && !done->figures.decode_ms &&
               !done->figures.reused,
           std::string{"a short record leaves the figures empty: "} + short_form);
  }
  const auto odd = done_of("DONE 1 x 2.5 -3 stop 0 0 y");
  expect(odd.has_value() && !odd->figures.prompt_tokens && odd->figures.prompt_ms == std::optional<double>{2.5} &&
             !odd->figures.decode_ms && !odd->figures.reused,
         "a field that is not a number (or is negative) is left empty, the rest kept, the record still accepted");
}

// E1: the timeout is a total budget, not a per-line allowance.
void generation_total_deadline_is_enforced() {
  FakeTransport transport;
  transport.reads = {line("READY 64 stop")};
  for (int i = 0; i < 20; ++i) transport.reads.push_back(line("T 1"));
  transport.reads.push_back(line("DONE 20 1 1.0 1.0 length"));
  transport.delay_per_read = std::chrono::milliseconds(100);
  EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  (void)client.await_ready(std::chrono::seconds(1), ready);
  (void)client.begin_generation(20, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  secure_frontend::EngineGeneration generation;
  const auto start = std::chrono::steady_clock::now();
  const auto error = client.read_generation(std::chrono::milliseconds(350), generation);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  expect(error.has_value() && error->code == EngineProtocolErrorCode::timeout, "20 lines at 100 ms each must exceed a 350 ms total budget");
  expect(elapsed < std::chrono::milliseconds(900), "the client must stop reading at the total deadline");
  expect(client.generating() && !client.idle(), "a timeout leaves the generation active for STOP/drain");
}

// R1 (owner ruling 2026-10-14): GEN carries the caller's absolute deadline to
// the transport unchanged; STOP and QUIT have no request deadline and carry the
// explicit control bound of one per-chunk deadline from the call.
void write_deadlines_reach_the_transport() {
  FakeTransport transport;
  transport.reads = {line("READY 1024 stop")};
  EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  (void)client.await_ready(std::chrono::seconds(1), ready);
  const auto gen_deadline = secure_frontend::MonotonicClock::now() + std::chrono::milliseconds(1234);
  expect(!client.begin_generation(2, {9}, gen_deadline).has_value(), "GEN written");
  expect(transport.deadlines.size() == 1U && transport.deadlines.back() == gen_deadline, "GEN uses exactly the supplied deadline");
  const auto before_stop = secure_frontend::MonotonicClock::now();
  expect(!client.stop().has_value(), "STOP written");
  const auto after_stop = secure_frontend::MonotonicClock::now();
  expect(transport.deadlines.size() == 2U && transport.deadlines.back() >= before_stop + std::chrono::seconds(1) &&
             transport.deadlines.back() <= after_stop + std::chrono::seconds(1),
         "STOP is bounded by one per-chunk deadline from the call");
  transport.reads = {line("DONE 0 1 1 1 cancel")};
  secure_frontend::EngineGeneration generation;
  (void)client.read_generation(std::chrono::seconds(1), generation);
  const auto before_quit = secure_frontend::MonotonicClock::now();
  expect(!client.quit().has_value(), "QUIT written");
  const auto after_quit = secure_frontend::MonotonicClock::now();
  expect(transport.deadlines.size() == 3U && transport.deadlines.back() >= before_quit + std::chrono::seconds(1) &&
             transport.deadlines.back() <= after_quit + std::chrono::seconds(1),
         "QUIT is bounded by one per-chunk deadline from the call");
}

// SP3 D-2: the STOP cause belongs to one generation request; every
// begin_generation() forgets it, even one that is refused. Successful DONE
// records are counted (the supervisor's uncounted-restart streak resets on one).
void stop_cause_and_completed_generations() {
  FakeTransport transport;
  transport.reads = {line("READY 1024 stop"), line("DONE 0 1 1 1 cancel"), line("DONE 0 1 1 1 nonfinite")};
  EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  (void)client.await_ready(std::chrono::seconds(1), ready);
  expect(!client.stop_cause().has_value() && client.completed_generations() == 0U, "nothing recorded before any GEN");
  (void)client.begin_generation(2, {9}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  expect(!client.stop(secure_frontend::EngineStopCause::client_cancelled).has_value(), "client STOP accepted");
  expect(client.stop_cause() == secure_frontend::EngineStopCause::client_cancelled, "the cause is recorded");
  secure_frontend::EngineGeneration generation;
  expect(!client.read_generation(std::chrono::seconds(1), generation).has_value(), "the STOP drained to DONE");
  expect(client.completed_generations() == 1U, "a successful DONE is counted");
  expect(client.begin_generation(2, {}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(), "an empty prompt is refused");
  expect(!client.stop_cause().has_value(), "a refused GEN still forgets the previous request's cause");
  (void)client.begin_generation(2, {9}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  expect(client.read_generation(std::chrono::seconds(1), generation).has_value(), "a nonfinite DONE fails the request");
  expect(client.completed_generations() == 1U, "a nonfinite DONE is not a successful one");
}

// SP3 Task 15: GEN carries the bounded sampling keys between max_new and the
// ids; absent params stay greedy; numbers take the shortest round-trip form.
// SP3 Task 15 (ruling T15-fake): uses this file's existing FakeTransport
// instead of the plan's near-duplicate RecordingTransport; assertions identical.
void gen_carries_bounded_sampling_keys() {
  FakeTransport transport;
  transport.reads = {line("READY 4096 stop")};
  secure_frontend::EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  expect(!client.await_ready(std::chrono::seconds(1), ready).has_value(), "ready");
  secure_frontend::SamplingParams sampling;
  sampling.temperature = 0.5;
  sampling.top_p = 0.9;
  sampling.seed = 42U;
  expect(!client.begin_generation(16U, {1U, 2U, 3U}, secure_frontend::deadline_after(std::chrono::seconds(5)), sampling)
              .has_value(),
         "GEN written");
  expect(transport.writes.back() == "GEN 16 temperature=0.5 top_p=0.9 seed=42 1,2,3", "keys between max_new and the ids: " + transport.writes.back());
  expect(secure_frontend::render_sampling_keys({}).empty(), "no keys when the request sets none (engine stays greedy)");
  secure_frontend::SamplingParams ints;
  ints.temperature = 1.0;
  ints.top_p = 1.0;
  expect(secure_frontend::render_sampling_keys(ints) == " temperature=1", "shortest round-trip form; the pin omits top_p=1 (the engine default)");
  secure_frontend::SamplingParams big;
  big.seed = 9223372036854775807ULL;
  expect(secure_frontend::render_sampling_keys(big) == " seed=9223372036854775807", "full seed range");
}

// SP5 M1 Task 4: the full pin wire shape (serve/server.py:1360-1394) —
// temperature only >0, top_p only <1, top_k clamped to 1..64, min_p,
// penalty_repeat/freq/present when != 1/0/0, penalty_last_n default 64 when
// any penalty is on, seed only >0.
void sampling_defaults_reach_gen_request() {
  FakeTransport transport;
  transport.reads = {line("READY 4096 stop")};
  secure_frontend::EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  expect(!client.await_ready(std::chrono::seconds(1), ready).has_value(), "ready");
  secure_frontend::SamplingParams p;
  p.temperature = 0.0;   // the pin omits it: the engine default is greedy
  p.top_p = 1.0;         // the pin omits it: 1 is the engine default
  p.top_k = 10;
  p.min_p = 0.1;
  p.presence_penalty = 0.5;
  p.repetition_penalty = 1.0;  // the pin omits the neutral value
  p.frequency_penalty = 0.0;   // the pin omits the neutral value
  expect(!client.begin_generation(16U, {1U, 2U, 3U}, secure_frontend::deadline_after(std::chrono::seconds(5)), p).has_value(),
         "GEN written");
  expect(transport.writes.back() == "GEN 16 top_k=10 min_p=0.1 penalty_present=0.5 penalty_last_n=64 1,2,3",
         "the pin's key names and default window: " + transport.writes.back());
  secure_frontend::SamplingParams clamp;
  clamp.top_k = 100;  // the pin clamps wide lists to the engine's 64 candidates
  expect(secure_frontend::render_sampling_keys(clamp) == " top_k=64", "top_k clamp: " + secure_frontend::render_sampling_keys(clamp));
  secure_frontend::SamplingParams rep;
  rep.repetition_penalty = 1.2;
  expect(secure_frontend::render_sampling_keys(rep) == " penalty_repeat=1.2 penalty_last_n=64",
         "penalty alias + default window: " + secure_frontend::render_sampling_keys(rep));
  secure_frontend::SamplingParams window;
  window.presence_penalty = 0.2;
  window.penalty_last_n = 8;
  expect(secure_frontend::render_sampling_keys(window) == " penalty_present=0.2 penalty_last_n=8",
         "explicit window: " + secure_frontend::render_sampling_keys(window));
}

}  // namespace

// E4: a prompt at the engine's advertised context must fit the GEN line.
void gen_line_fits_max_prompt_tokens() {
  FakeTransport transport;
  transport.reads = {line("READY 4096 stop"), line("DONE 0 4000 1.0 1.0 length")};
  EngineProtocolClient client(transport);  // default limits: derive from READY
  secure_frontend::EngineReady ready{};
  (void)client.await_ready(std::chrono::seconds(1), ready);
  std::vector<std::uint32_t> prompt(4000U, 4294967295U);  // worst case: 10-digit ids
  expect(!client.begin_generation(1, prompt, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(), "4000 ten-digit ids must fit the derived GEN line limit");
  expect(transport.writes.size() == 1U && transport.writes.front().size() > 40000U, "the GEN line must actually carry every id");
}

// Session log step 13 (owner: Strata runs contexts up to 1M with YaRN): the default limits take
// a prompt near 1,048,576 tokens from a READY that advertises that context; the GEN line (six-digit
// ids, as the tokenizer's) stays under the 16 MiB protocol cap.
void default_limits_take_a_million_token_prompt() {
  const secure_frontend::EngineProtocolLimits defaults{};
  expect(defaults.max_prompt_tokens == 1048576U && defaults.max_generated_tokens == 1048576U &&
             defaults.max_new_tokens == 1048576U,
         "the default token limits are 1,048,576");
  FakeTransport transport;
  transport.reads = {line("READY 1048576 stop")};
  EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  (void)client.await_ready(std::chrono::seconds(1), ready);
  const std::vector<std::uint32_t> prompt(1048576U - 64U, 248046U);
  expect(!client.begin_generation(1, prompt, secure_frontend::deadline_after(std::chrono::seconds(30))).has_value(),
         "a prompt just under 1M tokens is sent");
  expect(transport.writes.size() == 1U && transport.writes.front().size() > 7U * 1000U * 1000U, "the GEN line carries every id");
}

// The READY-derived outbound cap is clamped to the shared 16 MiB protocol cap, so a
// huge advertised context cannot let begin_generation build a line the transport refuses.
void derived_line_cap_is_clamped_to_protocol_max() {
  FakeTransport transport;
  transport.reads = {line("READY 4294967295 stop")};
  EngineProtocolClient client(transport, {.max_line_bytes = 0, .max_prompt_tokens = 4U * 1024U * 1024U});
  secure_frontend::EngineReady ready{};
  (void)client.await_ready(std::chrono::seconds(1), ready);
  const std::vector<std::uint32_t> prompt(2U * 1024U * 1024U, 4294967295U);  // ~22 MiB GEN line
  const auto error = client.begin_generation(1, prompt, secure_frontend::deadline_after(std::chrono::seconds(5)));
  expect(error.has_value() && error->code == EngineProtocolErrorCode::invalid_argument && transport.writes.empty(),
         "a GEN line beyond kProtocolMaxLineBytes must be rejected cleanly before any write");
}

// SP3 M2 Task 4: next_event. Ruling t4-test: EngineProtocolClient holds an
// atomic member (never moved or copied), so the plan's by-value
// generating_client helper is inlined into each test; begin_generation keeps
// M1's write_deadline argument. Semantics identical.
template <typename T>
const T* as(const secure_frontend::GenerationEvent& event) { return std::get_if<T>(&event); }

void next_event_yields_tokens_progress_and_done() {
  FakeTransport transport;
  transport.reads = {line("READY 64 stop"), line("T 5"), line("PP 1 2"), line("T 6"), line("DONE 2 1 1.0 1.0 length")};
  EngineProtocolClient client(transport, {}, secure_frontend::EngineProtocolFlavor::lighttransport_glm53);
  secure_frontend::EngineReady ready{};
  (void)client.await_ready(std::chrono::seconds(1), ready);
  (void)client.begin_generation(4, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  const auto first = client.next_event(std::chrono::seconds(1));
  expect(as<secure_frontend::TokenEvent>(first) && as<secure_frontend::TokenEvent>(first)->id == 5U, "first token");
  expect(as<secure_frontend::ProgressEvent>(client.next_event(std::chrono::seconds(1))) != nullptr, "PP is a progress event");
  expect(as<secure_frontend::TokenEvent>(client.next_event(std::chrono::seconds(1))) != nullptr, "second token");
  const auto done = client.next_event(std::chrono::seconds(1));
  expect(as<secure_frontend::DoneEvent>(done) && as<secure_frontend::DoneEvent>(done)->finish == "length" &&
             as<secure_frontend::DoneEvent>(done)->generated == 2U, "DONE carries finish and count");
  expect(client.idle(), "DONE returns the client to idle");
}

void next_event_timeout_keeps_generation_active() {
  FakeTransport transport;
  transport.reads = {line("READY 64 stop")};
  EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  (void)client.await_ready(std::chrono::seconds(1), ready);
  (void)client.begin_generation(4, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  const auto waited = client.next_event(std::chrono::milliseconds(10));
  expect(as<secure_frontend::EngineProtocolError>(waited) &&
             as<secure_frontend::EngineProtocolError>(waited)->code == EngineProtocolErrorCode::timeout, "an empty slice is a timeout");
  expect(client.generating(), "the generation stays active");
  expect(!client.stop().has_value(), "STOP can still be sent");
  transport.reads.push_back(line("DONE 0 1 1.0 1.0 cancel"));
  const auto done = client.next_event(std::chrono::seconds(1));
  expect(as<secure_frontend::DoneEvent>(done) && as<secure_frontend::DoneEvent>(done)->finish == "cancel", "drained to cancel");
}

void next_event_finish_forms_and_nonfinite_stays_idle() {
  const auto finish_of = [](const std::string& done_line) -> std::string {
    FakeTransport transport;
    transport.reads = {line("READY 64 stop"), line(done_line)};
    EngineProtocolClient client(transport);
    secure_frontend::EngineReady ready{};
    (void)client.await_ready(std::chrono::seconds(1), ready);
    (void)client.begin_generation(4, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
    const auto event = client.next_event(std::chrono::seconds(1));
    if (const auto* done = as<secure_frontend::DoneEvent>(event)) return std::string(done->finish) + (client.idle() ? "/idle" : "/busy");
    return client.closed() ? "closed" : "other";
  };
  expect(finish_of("DONE 1") == "stop/idle", "legacy short form is an ordinary end");
  expect(finish_of("DONE stop") == "stop/idle", "two-field finish form");
  expect(finish_of("DONE 1 1 1.0 1.0 nonfinite") == "nonfinite/idle", "nonfinite keeps the engine idle (SP0 E3)");
  expect(finish_of("DONE 1 1 1.0 1.0 bogus") == "closed", "unknown finish in a full record is malformed");
}

// SP5 M2 Task 5 (D-8): the Maya v1.1.21 wire dialect. The vendored pump
// consumes NOTE (serve/server.py:851 — it prints the kept-conversation note
// to its own log), the STAT expert-tier cadence every few tokens
// (:849, _parse_stat :677-690; the engine's print at src/program/
// generate.cpp:2014) and the five-field PP that the engine prints per prompt
// chunk (`PP <done> <total> <ms> <tok/s> <reuse>`, generate.cpp:6102, :6667;
// the pump's tolerant read at server.py:835-847). All are private: consumed
// as ProgressEvents, never client output, never a protocol error; anything
// else stays fail-closed.
void maya_flavor_consumes_note_and_progress() {
  FakeTransport transport;
  transport.reads = {line("READY 8192 stop"),
                     line("NOTE slots: keeping 2 conversations"),
                     line("PP 512 4096 120 33.3 0"),
                     line("STAT tok_s=30.10 ms_tok=33.22 vram_hit=0.5500 promo=1.000"),
                     line("T 9"),
                     line("STAT tok_s=31.00 tok_s=31.00"),
                     line("NOTE mid-stream note"),
                     line("DONE 1 stop")};
  EngineProtocolClient client(transport, {}, secure_frontend::EngineProtocolFlavor::maya_118);
  secure_frontend::EngineReady ready{};
  expect(!client.await_ready(std::chrono::seconds(1), ready).has_value(), "the maya_118 client starts on READY");
  expect(!client.begin_generation(4, {1, 2}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(),
         "the maya_118 generation starts");
  secure_frontend::EngineGeneration generation;
  const auto error = client.read_generation(std::chrono::seconds(1), generation);
  expect(!error.has_value() && generation.token_ids == std::vector<std::uint32_t>({9U}) && !client.closed(),
         "NOTE/STAT/5-field PP are consumed silently: exactly the tokens survive");

  // Through next_event too: each progress record is a ProgressEvent, so the
  // pump loop sees no protocol error and the caller's output path sees none.
  FakeTransport event_transport;
  event_transport.reads = {line("READY 64"), line("NOTE n"), line("STAT a=1"), line("PP 1 2 3 4.0 5"), line("T 4")};
  EngineProtocolClient event_client(event_transport, {}, secure_frontend::EngineProtocolFlavor::maya_118);
  (void)event_client.await_ready(std::chrono::seconds(1), ready);
  (void)event_client.begin_generation(4, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  int progress_count = 0;
  int token_count = 0;
  for (int i = 0; i < 4; ++i) {
    const auto event = event_client.next_event(std::chrono::seconds(1));
    if (std::holds_alternative<secure_frontend::ProgressEvent>(event)) ++progress_count;
    else if (std::holds_alternative<secure_frontend::TokenEvent>(event)) ++token_count;
    else
      expect(false, "a maya progress record surfaced as an error or a terminal event");
  }
  expect(progress_count == 3 && token_count == 1, "each consumed record surfaces only as a ProgressEvent");
}

// Cross-flavor strictness unchanged: neither other flavor accepts NOTE or
// STAT, and the maya flavor still rejects strata-only RESUME/REUSED.
void strata_flavor_still_fails_closed_on_note() {
  secure_frontend::EngineReady ready{};
  for (const auto flavor : {secure_frontend::EngineProtocolFlavor::strata_0143,
                            secure_frontend::EngineProtocolFlavor::lighttransport_glm53}) {
    for (const char* record : {"NOTE slots", "STAT tok_s=1"}) {
      FakeTransport transport;
      transport.reads = {line("READY 64"), line(record), line("DONE 0")};
      EngineProtocolClient client(transport, {}, flavor);
      (void)client.await_ready(std::chrono::seconds(1), ready);
      (void)client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
      secure_frontend::EngineGeneration generation;
      const auto error = client.read_generation(std::chrono::seconds(1), generation);
      expect(error.has_value() && error->code == EngineProtocolErrorCode::unexpected_line && client.closed(),
             std::string("NOTE/STAT stay unknown to the other flavors: ") + record);
    }
  }
  FakeTransport resume;
  resume.reads = {line("READY 64"), line("RESUME 0"), line("DONE 0")};
  EngineProtocolClient maya_client(resume, {}, secure_frontend::EngineProtocolFlavor::maya_118);
  (void)maya_client.await_ready(std::chrono::seconds(1), ready);
  (void)maya_client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  secure_frontend::EngineGeneration generation;
  const auto error = maya_client.read_generation(std::chrono::seconds(1), generation);
  expect(error.has_value() && error->code == EngineProtocolErrorCode::unexpected_line && maya_client.closed(),
         "the maya_118 flavor keeps the strata-only RESUME record unknown (fail closed)");
}

// The Maya PP grammar, pinned to the vendored reads: the pump accepts any
// `PP` with digit fields 1-2 and optional float/reuse tails (server.py:835-
// 847); the engine prints the five-field form (generate.cpp:6102, :6667).
// Our flavor bounds it like the others: 3 to 6 fields after `PP`, decimal
// counters, optional non-negative decimal metrics, optional decimal reuse —
// anything else is unexpected_line and closes the protocol.
//
// SP5-P2 Task 5: two more pins on the maya_118 framing at the new pin.
// (1) The slot-mode records — `T <rid> <tok>` (generate.cpp:2295) and the
// rid-prefixed DONE (:2400) — exist only when the engine runs with slots
// enabled; a glm53 launch forces STRATA_GLM_SLOTS=0 (SP6 §4.9, sheet S5;
// kGlmSafetyDefaults), so a slot line reaching the client is a protocol error:
// fail closed, never parse. (2) The v1.1.21 DONE record (:2111) — DONE
// <produced> <prompt> <prompt_ms> <decode_ms> <finish> <spec-hits>
// <spec-steps> <reuse> <tier-hits> <tier-total> — is consumed fail-closed:
// timings and reuse from the positional fields, the spec/tier counters
// accepted and untrusted; the tagged-form PP (:1757, empty tag in a single
// session) is a 6-field heartbeat and stays progress, not output.
void maya_slot_mode_records_are_protocol_errors() {
  secure_frontend::EngineReady ready{};
  for (const char* slot_line : {"T 3 42", "DONE 3 0 100 0.0 0.0 cancel 0 0 0 0 0"}) {
    FakeTransport transport;
    transport.reads = {line("READY 64"), line(slot_line)};
    EngineProtocolClient client(transport, {}, secure_frontend::EngineProtocolFlavor::maya_118);
    (void)client.await_ready(std::chrono::seconds(1), ready);
    (void)client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
    secure_frontend::EngineGeneration generation;
    const auto error = client.read_generation(std::chrono::seconds(1), generation);
    expect(error.has_value() && client.closed(), std::string{"the slot-mode line fails closed: "} + slot_line);
  }
}

void maya_full_v1_1_21_done_record_feeds_the_done_event() {
  FakeTransport transport;
  transport.reads = {line("READY 64"), line("T 9"), line("PP 12 100 300.0 40.5 80"),
                     line("DONE 1 100 300.0 40.5 stop 2 3 80 90 120")};
  EngineProtocolClient client(transport, {}, secure_frontend::EngineProtocolFlavor::maya_118);
  secure_frontend::EngineReady ready{};
  (void)client.await_ready(std::chrono::seconds(1), ready);
  (void)client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
  const auto token = client.next_event(std::chrono::seconds(1));
  const auto progress = client.next_event(std::chrono::seconds(1));
  const auto event = client.next_event(std::chrono::seconds(1));
  const auto* done = std::get_if<secure_frontend::DoneEvent>(&event);
  expect(std::holds_alternative<secure_frontend::TokenEvent>(token) &&
             std::holds_alternative<secure_frontend::ProgressEvent>(progress) && done != nullptr && !client.closed(),
         "the PP heartbeat is consumed and the full record terminates the generation");
  expect(done != nullptr && done->finish == "stop" && done->figures.prompt_tokens == std::optional<std::size_t>{100U} &&
             done->figures.prompt_ms == std::optional<double>{300.0} &&
             done->figures.decode_ms == std::optional<double>{40.5} &&
             done->figures.reused == std::optional<std::size_t>{80U},
         "the v1.1.21 record yields timings and reuse; the spec/tier counters stay untrusted");
}

void maya_pp_grammar_is_bounded() {
  secure_frontend::EngineReady ready{};
  for (const char* good : {"PP 512 4096 120 33.3 0", "PP 512 4096 120.0 33.3 2048", "PP 16 32",
                           "PP 16 32 10", "PP 16 32 10 1523.8", "PP 0 0 0.0 0.0 0"}) {
    FakeTransport transport;
    transport.reads = {line("READY 64"), line(good), line("T 9"), line("DONE 0")};
    EngineProtocolClient client(transport, {}, secure_frontend::EngineProtocolFlavor::maya_118);
    (void)client.await_ready(std::chrono::seconds(1), ready);
    (void)client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
    secure_frontend::EngineGeneration generation;
    const auto error = client.read_generation(std::chrono::seconds(1), generation);
    expect(!error.has_value() && generation.token_ids == std::vector<std::uint32_t>({9U}),
           std::string("the vendored PP forms are consumed: ") + good);
  }
  for (const char* bad : {"PP 16", "PP x 32", "PP 16 32 -1 2.0", "PP 16 32 x 2.0", "PP 16 32 1 nan",
                          "PP 16 32 1 2.0 -5", "PP 16 32 1 2.0 3 9", "PP 16 32 1 2.0 x"}) {
    FakeTransport transport;
    transport.reads = {line("READY 64"), line(bad), line("DONE 0")};
    EngineProtocolClient client(transport, {}, secure_frontend::EngineProtocolFlavor::maya_118);
    (void)client.await_ready(std::chrono::seconds(1), ready);
    (void)client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
    secure_frontend::EngineGeneration generation;
    const auto error = client.read_generation(std::chrono::seconds(1), generation);
    expect(error.has_value() && error->code == EngineProtocolErrorCode::unexpected_line && client.closed(),
           std::string("a malformed PP fails closed under maya_118: ") + bad);
  }
}

// Maya v1.1.21 (issue 27 of docs/validation/2026-10-21-windows-gpu-session.md): before READY
// the GLM fast path prints one `VRAM dev=<n> pci=<bus> total=... layers=<a>-<b>` record per GPU
// part (maya-ref src/program/generate.cpp:1054-1062) and, on a Windows APU whose page file caps
// the commit, a `NOTE experts: ...` line plus a second INFO (:1065-1071). Maya's own reader
// skips them (serve/server.py:456-473). The maya_118 flavor accepts exactly these shapes
// before READY — VRAM under the STAT rule (at least one pair, every key non-empty before its
// `=`), NOTE non-empty — and nothing else; the other flavors keep rejecting VRAM.
void maya_flavor_accepts_vram_and_note_before_ready() {
  const std::string vram =
      "VRAM dev=0 pci=0000:01:00.0 total=17163091968 free=1073741824 weights=9000000000 kv=100 kv_ram=0 act=1 "
      "drafter=0 window=0 pool=2 tail=0 borrow=0 vision=0 slots=0 ctx=32768 layers=0-40";
  FakeTransport transport;
  transport.reads = {line("INFO context=32768 kv=int8 engine_kind=glm-fast"), line(vram),
                     line("VRAM dev=1 pci=- total=1 free=1 layers=41-77"),
                     line("NOTE experts: 12 GB stay off the GPU - Windows' page file is too small to hold them there"),
                     line("INFO gpu_commit_capped_gb=12 pagefile_hint_gb=96"), line("READY 32768 stop pause")};
  EngineProtocolClient client(transport, {}, secure_frontend::EngineProtocolFlavor::maya_118);
  secure_frontend::EngineReady ready{};
  const auto started = client.await_ready(std::chrono::seconds(1), ready);
  expect(!started.has_value() && ready.max_context_tokens == 32768U && ready.supports_stop && client.idle(),
         "the v1.1.21 startup records before READY are consumed");

  for (const char* bad : {"VRAM", "VRAM dev", "VRAM =0", "VRAM dev=0 =1", "VRAM dev=0 layers", "NOTE",
                          "STAT tok_s=1", "PP 1 2", "RESUME 0", "BOGUS dev=0"}) {
    FakeTransport refused;
    refused.reads = {line("INFO context=64"), line(bad), line("READY 64 stop")};
    EngineProtocolClient strict(refused, {}, secure_frontend::EngineProtocolFlavor::maya_118);
    const auto error = strict.await_ready(std::chrono::seconds(1), ready);
    expect(error.has_value() && error->code == EngineProtocolErrorCode::unexpected_line && strict.closed(),
           std::string("anything else before READY fails closed under maya_118: ") + bad);
  }
  for (const auto flavor : {secure_frontend::EngineProtocolFlavor::strata_0143,
                            secure_frontend::EngineProtocolFlavor::lighttransport_glm53}) {
    FakeTransport other;
    other.reads = {line(vram), line("READY 64 stop")};
    EngineProtocolClient other_client(other, {}, flavor);
    const auto error = other_client.await_ready(std::chrono::seconds(1), ready);
    expect(error.has_value() && error->code == EngineProtocolErrorCode::unexpected_line,
           "VRAM stays unknown to the other flavors");
  }
}

// The Monitor needs the engines' expert-tier counters (owner: the VRAM and RAM cards
// show experts loaded, not only GB used). INFO (both flavors; upstream generate.cpp
// :10933, maya-ref pre-READY INFO) and the maya_118 STAT cadence (generate.cpp :2014)
// must be KEPT, not just consumed: the client retains the last value per key.
void info_and_stat_readings_are_captured() {
  FakeTransport transport;
  transport.reads = {line("INFO engine=0.1.43 context=8192 expert_slots=384 expert_slots_primary=212 expert_cache_mib=10240"),
                     line("READY 8192 stop"), line("T 1"), line("DONE 1 1 1.0 1.0 stop")};
  EngineProtocolClient client(transport);
  secure_frontend::EngineReady ready{};
  expect(!client.await_ready(std::chrono::seconds(1), ready).has_value(), "READY parses");
  const auto readings = client.engine_readings();
  expect(readings.count("expert_slots") != 0U && readings.at("expert_slots") == "384" &&
         readings.count("context") != 0U && readings.at("context") == "8192" &&
         readings.count("engine") != 0U && readings.at("engine") == "0.1.43",
         "the INFO record's fields are kept for the Monitor");
  expect(!client.begin_generation(4, {1}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(),
         "the generation starts");
  secure_frontend::EngineGeneration generation;
  expect(!client.read_generation(std::chrono::seconds(1), generation).has_value(), "the generation completes");

  FakeTransport maya;
  maya.reads = {line("READY 8192 stop"),
                line("STAT tok_s=30.10 vram_used=2100 ram_used=1792 vram_gb=14.6 ram_gb=41.2"),
                line("T 9"),
                line("STAT vram_used=2102 ram_used=1790"),
                line("DONE 1 stop")};
  EngineProtocolClient mc(maya, {}, secure_frontend::EngineProtocolFlavor::maya_118);
  expect(!mc.await_ready(std::chrono::seconds(1), ready).has_value(), "the maya_118 client starts");
  expect(!mc.begin_generation(4, {1}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(),
         "the maya_118 generation starts");
  expect(!mc.read_generation(std::chrono::seconds(1), generation).has_value(), "the maya_118 generation completes");
  const auto live = mc.engine_readings();
  expect(live.count("vram_used") != 0U && live.at("vram_used") == "2102" && live.at("ram_used") == "1790" &&
         live.count("vram_gb") != 0U && live.at("vram_gb") == "14.6",
         "the STAT cadence keeps the last value seen per key");
}

// SP5-P2 Task 7 (issue 27, penalty semantics): the two engines disagree on a window
// of 0. The strata pin (serve/server.py:1360-1394) treats a penalty without a usable
// window as the default window of 64; the GLM engine at 451f9cb6 treats window 0 as
// NO PENALTY AT ALL — `pen_on = pen_n > 0` (generate.cpp:3467), and maya's own server
// says it in words (serve/server.py:274-275: "0 = no penalty, -1 = the whole context,
// below -1 refused"). The client renders per flavor so an operator's 0 means the same
// thing on the wire as it means on that engine; negative windows never reach here
// (config bounds penalty_last_n to 0..8192, frontend_config.cpp:167).
void penalty_window_zero_means_no_penalty_on_glm_only() {
  secure_frontend::SamplingParams p;
  p.repetition_penalty = 1.2;
  p.penalty_last_n = 0;
  expect(secure_frontend::render_sampling_keys(p) == " penalty_repeat=1.2 penalty_last_n=64",
         "the strata rule is untouched: 0 renders the default window");
  expect(secure_frontend::render_sampling_keys(p, true).empty(),
         "the GLM rule: window 0 switches the penalties off — no keys at all");

  FakeTransport maya;
  maya.reads = {line("READY 64")};
  EngineProtocolClient mc(maya, {}, secure_frontend::EngineProtocolFlavor::maya_118);
  secure_frontend::EngineReady ready{};
  (void)mc.await_ready(std::chrono::seconds(1), ready);
  expect(!mc.begin_generation(8, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)), p).has_value(),
         "the maya_118 GEN is written");
  expect(maya.writes.back().find("penalty") == std::string::npos,
         "no penalty keys on the GLM GEN line: " + maya.writes.back());
}

// A request that asks to be repeatable - a seed, or temperature 0 asked for - keeps its expert-cache misses on the
// GPU on the GLM engine (`pcie_frac=1`: none to the CPU lane). The lane's experts round differently from the GPU's
// and which experts it takes depends on the cache earlier requests left, so the same seeded or greedy request
// diverged after ~50 tokens; with the key it repeats (raon1, 2026-10-26: 3 of 3 identical with a sampled request
// between them, at ~39 tok/s instead of ~56). An unset temperature is the engine's default, not a request for it,
// and the strata engines have no such key.
void glm_repeatable_requests_keep_misses_on_the_gpu() {
  const auto gen_line = [](const secure_frontend::EngineProtocolFlavor flavor, const secure_frontend::SamplingParams& p) {
    FakeTransport transport;
    transport.reads = {line("READY 64")};
    EngineProtocolClient client(transport, {}, flavor);
    secure_frontend::EngineReady ready{};
    (void)client.await_ready(std::chrono::seconds(1), ready);
    (void)client.begin_generation(8, {1, 2}, secure_frontend::deadline_after(std::chrono::seconds(5)), p);
    return transport.writes.empty() ? std::string{} : transport.writes.back();
  };
  using secure_frontend::EngineProtocolFlavor;
  secure_frontend::SamplingParams seeded;
  seeded.temperature = 0.6;
  seeded.seed = 42U;
  expect(gen_line(EngineProtocolFlavor::maya_118, seeded) == "GEN 8 temperature=0.6 seed=42 pcie_frac=1 1,2",
         "a seeded request is repeatable: " + gen_line(EngineProtocolFlavor::maya_118, seeded));
  secure_frontend::SamplingParams greedy;
  greedy.temperature = 0.0;
  expect(gen_line(EngineProtocolFlavor::maya_118, greedy) == "GEN 8 pcie_frac=1 1,2",
         "temperature 0 asked for is repeatable: " + gen_line(EngineProtocolFlavor::maya_118, greedy));
  secure_frontend::SamplingParams sampled;
  sampled.temperature = 0.6;
  expect(gen_line(EngineProtocolFlavor::maya_118, sampled) == "GEN 8 temperature=0.6 1,2",
         "an unseeded sampled request keeps the CPU lane: " + gen_line(EngineProtocolFlavor::maya_118, sampled));
  expect(gen_line(EngineProtocolFlavor::maya_118, {}) == "GEN 8 1,2",
         "an unset temperature is the engine's default, not a request: " + gen_line(EngineProtocolFlavor::maya_118, {}));
  expect(gen_line(EngineProtocolFlavor::strata_0143, seeded) == "GEN 8 temperature=0.6 seed=42 1,2",
         "the strata engine has no such key: " + gen_line(EngineProtocolFlavor::strata_0143, seeded));
}

// Unified-engine design §3.3 (decision 3): the merged engine's one converged dialect, a STRICT
// SUPERSET, fail-closed. Inbound: T, a strict 4-field PP `PP <done> <total> <ms> <tok_s>` (dropped),
// an optional STAT cadence (dropped), the 8-field DONE `DONE <produced> <prompt> <prompt_ms>
// <decode_ms> <finish> <spec_hits> <spec_steps> <reused>` (finish in stop|length|cancel|nonfinite),
// and ERR. No NOTE, no RESUME/REUSED; every other record (the other flavors' private ones included)
// fails closed.
void unified_flavor_grammar_is_a_strict_superset() {
  using secure_frontend::EngineProtocolFlavor;
  secure_frontend::EngineReady ready{};
  // Accepted end to end: READY, the 4-field PP, a STAT cadence, T, and the full 8-field DONE.
  {
    FakeTransport t;
    t.reads = {line("READY 8192 stop"), line("PP 16 32 10.5 1523.8"), line("STAT tok_s=30.1 vram_hit=0.55"),
               line("T 9"), line("DONE 1 32 300.0 40.5 stop 2 3 16")};
    EngineProtocolClient client(t, {}, EngineProtocolFlavor::unified);
    expect(!client.await_ready(std::chrono::seconds(1), ready).has_value() && ready.max_context_tokens == 8192U &&
               ready.supports_stop,
           "the unified client starts on READY <ctx> stop");
    expect(!client.begin_generation(4, {1}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(),
           "the unified generation starts");
    secure_frontend::EngineGeneration generation;
    const auto error = client.read_generation(std::chrono::seconds(1), generation);
    expect(!error.has_value() && generation.token_ids == std::vector<std::uint32_t>({9U}) && !client.closed(),
           "the unified dialect (4-field PP, STAT, 8-field DONE) is consumed; exactly the token survives");
    expect(generation.done_line == "DONE 1 32 300.0 40.5 stop 2 3 16", "the full DONE record is kept");
  }
  // The 8-field DONE feeds the Monitor figures (prompt tokens, both timings and the reused tail).
  {
    FakeTransport t;
    t.reads = {line("READY 64 stop"), line("T 9"), line("DONE 1 32 300.0 40.5 length 2 3 16")};
    EngineProtocolClient client(t, {}, EngineProtocolFlavor::unified);
    (void)client.await_ready(std::chrono::seconds(1), ready);
    (void)client.begin_generation(4, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
    (void)client.next_event(std::chrono::seconds(1));  // T 9
    const auto event = client.next_event(std::chrono::seconds(1));
    const auto* done = std::get_if<secure_frontend::DoneEvent>(&event);
    expect(done != nullptr && done->finish == "length" && done->figures.prompt_tokens == std::optional<std::size_t>{32U} &&
               done->figures.prompt_ms == std::optional<double>{300.0} &&
               done->figures.decode_ms == std::optional<double>{40.5} &&
               done->figures.reused == std::optional<std::size_t>{16U},
           "the unified 8-field DONE yields the Monitor figures and keeps the client idle");
  }
  // The other flavors' private records fail closed under unified: NOTE (maya), RESUME/REUSED (strata).
  for (const char* foreign : {"NOTE slots: keeping 2", "RESUME 0", "REUSED 0"}) {
    FakeTransport t;
    t.reads = {line("READY 64 stop"), line(foreign), line("DONE 0")};
    EngineProtocolClient client(t, {}, EngineProtocolFlavor::unified);
    (void)client.await_ready(std::chrono::seconds(1), ready);
    (void)client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
    secure_frontend::EngineGeneration generation;
    const auto error = client.read_generation(std::chrono::seconds(1), generation);
    expect(error.has_value() && error->code == EngineProtocolErrorCode::unexpected_line && client.closed(),
           std::string("a foreign-flavor record fails closed under unified: ") + foreign);
  }
  // Malformed records fail closed: a PP that is not exactly four fields, a non-numeric or negative PP
  // field, a bad DONE finish, an empty or malformed STAT, and an unknown prefix.
  for (const char* bad : {"PP 16 32", "PP 16 32 10", "PP 16 32 10 1523.8 extra", "PP x 32 1 2.0",
                          "PP 16 32 -1 2.0", "STAT", "STAT =1", "DONE 1 32 300.0 40.5 bogus 2 3 16", "BOGUS 1"}) {
    FakeTransport t;
    t.reads = {line("READY 64 stop"), line(bad), line("DONE 0")};
    EngineProtocolClient client(t, {}, EngineProtocolFlavor::unified);
    (void)client.await_ready(std::chrono::seconds(1), ready);
    (void)client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
    secure_frontend::EngineGeneration generation;
    const auto error = client.read_generation(std::chrono::seconds(1), generation);
    expect(error.has_value() && client.closed(), std::string("a malformed unified record fails closed: ") + bad);
  }
  // Cross-flavor strictness the other way: the unified-only STAT stays unknown to the strata flavor.
  {
    FakeTransport t;
    t.reads = {line("READY 64"), line("STAT tok_s=1"), line("DONE 0")};
    EngineProtocolClient client(t, {}, EngineProtocolFlavor::strata_0143);
    (void)client.await_ready(std::chrono::seconds(1), ready);
    (void)client.begin_generation(1, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
    secure_frontend::EngineGeneration generation;
    const auto error = client.read_generation(std::chrono::seconds(1), generation);
    expect(error.has_value() && error->code == EngineProtocolErrorCode::unexpected_line && client.closed(),
           "STAT stays unknown to the strata flavor");
  }
}

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"unified_flavor_grammar_is_a_strict_superset", unified_flavor_grammar_is_a_strict_superset},
      {"maya_flavor_accepts_vram_and_note_before_ready", maya_flavor_accepts_vram_and_note_before_ready},
      {"info_and_stat_readings_are_captured", info_and_stat_readings_are_captured},
      {"runs_the_solo_protocol", runs_the_solo_protocol},
      {"gen_line_unchanged_without_embeddings_geni_with_them", gen_line_unchanged_without_embeddings_geni_with_them},
      {"stop_and_quit_are_explicit", stop_and_quit_are_explicit},
      {"write_deadlines_reach_the_transport", write_deadlines_reach_the_transport},
      {"refuses_stop_when_not_advertised", refuses_stop_when_not_advertised},
      {"rejects_malformed_and_unexpected_engine_output", rejects_malformed_and_unexpected_engine_output},
      {"bounds_and_engine_errors_fail_closed", bounds_and_engine_errors_fail_closed},
      {"prefill_progress_is_bounded_in_both_profiles", prefill_progress_is_bounded_in_both_profiles},
      {"resume_and_reused_progress_is_consumed_in_the_strata_profile",
       resume_and_reused_progress_is_consumed_in_the_strata_profile},
      {"done_figures_reach_the_done_event", done_figures_reach_the_done_event},
      {"generation_total_deadline_is_enforced", generation_total_deadline_is_enforced},
      {"gen_line_fits_max_prompt_tokens", gen_line_fits_max_prompt_tokens},
      {"default_limits_take_a_million_token_prompt", default_limits_take_a_million_token_prompt},
      {"derived_line_cap_is_clamped_to_protocol_max", derived_line_cap_is_clamped_to_protocol_max},
      {"stop_cause_and_completed_generations", stop_cause_and_completed_generations},
      {"gen_carries_bounded_sampling_keys", gen_carries_bounded_sampling_keys},
      {"sampling_defaults_reach_gen_request", sampling_defaults_reach_gen_request},
      {"next_event_yields_tokens_progress_and_done", next_event_yields_tokens_progress_and_done},
      {"next_event_timeout_keeps_generation_active", next_event_timeout_keeps_generation_active},
      {"next_event_finish_forms_and_nonfinite_stays_idle", next_event_finish_forms_and_nonfinite_stays_idle},
      {"maya_flavor_consumes_note_and_progress", maya_flavor_consumes_note_and_progress},
      {"strata_flavor_still_fails_closed_on_note", strata_flavor_still_fails_closed_on_note},
      {"maya_pp_grammar_is_bounded", maya_pp_grammar_is_bounded},
      {"maya_slot_mode_records_are_protocol_errors", maya_slot_mode_records_are_protocol_errors},
      {"maya_full_v1_1_21_done_record_feeds_the_done_event", maya_full_v1_1_21_done_record_feeds_the_done_event},
      {"penalty_window_zero_means_no_penalty_on_glm_only", penalty_window_zero_means_no_penalty_on_glm_only},
      {"glm_repeatable_requests_keep_misses_on_the_gpu", glm_repeatable_requests_keep_misses_on_the_gpu},
  }, failures);
}
