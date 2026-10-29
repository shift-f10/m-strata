#include "secure_frontend/chat_execution.hpp"
#include "secure_frontend/generation_run.hpp"
#include "support/test_support.hpp"
#include "support/test_tokenizer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

std::string byte_symbol(const unsigned char byte) {
  if (byte >= 33U && byte <= 126U) return std::string(1U, static_cast<char>(byte));
  unsigned int printable_before = 0U;
  for (unsigned int value = 33U; value <= 126U && value < byte; ++value) ++printable_before;
  const unsigned int code_point = 256U + static_cast<unsigned int>(byte) - printable_before;
  return {static_cast<char>(0xc0U | (code_point >> 6U)),
          static_cast<char>(0x80U | (code_point & 0x3fU))};
}

secure_frontend::TokenizerPack tokenizer_for(const std::string& text) {
  secure_frontend::TokenizerPack pack;
  std::unordered_set<std::string> seen;
  for (const unsigned char byte : text) {
    const std::string token = byte_symbol(byte);
    if (seen.insert(token).second) pack.tokens_by_id.push_back(token);
  }
  pack.token_types.assign(pack.tokens_by_id.size(), 1);
  // The fixed Qwen renderer owns these framing literals.  They must be
  // modeled as tokenizer CONTROL tokens, never as ordinary user text.
  pack.tokens_by_id.push_back("<|im_start|>");
  pack.token_types.push_back(3);
  pack.tokens_by_id.push_back("<|im_end|>");
  pack.token_types.push_back(3);
  return pack;
}

std::size_t token_id_for(const secure_frontend::TokenizerPack& pack, const std::string& token) {
  for (std::size_t index = 0; index < pack.tokens_by_id.size(); ++index) {
    if (pack.tokens_by_id[index] == token) return index;
  }
  return pack.tokens_by_id.size();
}

class FakeTransport final : public secure_frontend::EngineLineTransport {
 public:
  std::deque<secure_frontend::EngineReadResult> reads;
  std::vector<std::string> writes;
  std::vector<secure_frontend::MonotonicClock::time_point> deadlines;
  std::chrono::milliseconds delay_per_read{0};

  bool write_line(const std::string_view line, const secure_frontend::MonotonicClock::time_point deadline,
                  std::string&) override {
    writes.emplace_back(line);
    deadlines.push_back(deadline);
    return true;
  }

  secure_frontend::EngineReadResult read_line(std::chrono::milliseconds) override {
    if (delay_per_read.count() > 0) std::this_thread::sleep_for(delay_per_read);
    if (reads.empty()) return {secure_frontend::EngineReadKind::timeout, {}, {}};
    auto result = std::move(reads.front());
    reads.pop_front();
    return result;
  }
};

secure_frontend::EngineReadResult line(std::string value) {
  return {secure_frontend::EngineReadKind::line, std::move(value), {}};
}

secure_frontend::ChatCompletionRequest valid_request(const std::size_t max_tokens = 2U) {
  return {"qwen-local", {{"user", "Hi"}}, max_tokens};
}

secure_frontend::TokenizerPack glm_tokenizer_for(const std::string& text) {
  auto pack = tokenizer_for(text);
  // The Qwen control entries supplied by tokenizer_for are harmless here: the
  // GLM encoder allows only its own exact fixed control sequence.
  for (const std::string_view marker :
       {"[gMASK]", "<sop>", "<|system|>", "<|user|>", "<|assistant|>", "<think>"}) {
    pack.tokens_by_id.emplace_back(marker);
    pack.token_types.push_back(3);
  }
  return pack;
}

void executes_a_bounded_non_streaming_completion() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt + "OK");
  const auto output_o = token_id_for(tokenizer, "O");
  const auto output_k = token_id_for(tokenizer, "K");
  expect(output_o < tokenizer.tokens_by_id.size() && output_k < tokenizer.tokens_by_id.size(),
         "test tokenizer must include generated tokens");

  FakeTransport transport;
  transport.reads = {line("INFO private-engine"), line("READY 512"),
                     line("T " + std::to_string(output_o)),
                     line("T " + std::to_string(output_k)), line("DONE 2")};
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::ChatExecutionService service(protocol);
  const auto result = service.execute(valid_request(), tokenizer);
  const auto* completion = std::get_if<secure_frontend::ChatExecutionResult>(&result);
  expect(completion != nullptr && completion->text == "OK" && completion->generated_tokens == 2U,
         "READY, GEN, token output, and local decode must compose into text");
  expect(transport.writes.size() == 1U && transport.writes.front().rfind("GEN 2 ", 0) == 0,
         "only canonical GEN may be sent to the local engine");

  transport.reads = {line("T " + std::to_string(output_o)), line("DONE 1")};
  const auto next_result = service.execute(valid_request(1U), tokenizer);
  const auto* next_completion = std::get_if<secure_frontend::ChatExecutionResult>(&next_result);
  expect(next_completion != nullptr && next_completion->text == "O" &&
             transport.writes.size() == 2U && transport.writes.back().rfind("GEN 1 ", 0) == 0,
         "an idle private engine may serve a second request without accepting a second READY");
}

void rejects_context_and_output_limits_before_generation() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt);
  FakeTransport context_transport;
  context_transport.reads = {line("READY 1")};
  secure_frontend::EngineProtocolClient context_protocol(context_transport);
  secure_frontend::ChatExecutionService context_service(context_protocol);
  const auto context_result = context_service.execute(valid_request(), tokenizer);
  const auto* context_error = std::get_if<secure_frontend::ChatExecutionError>(&context_result);
  expect(context_error != nullptr &&
             context_error->code == secure_frontend::ChatExecutionErrorCode::context_limit_exceeded &&
             context_transport.writes.empty(),
         "context overflow must not emit GEN");

  FakeTransport limit_transport;
  limit_transport.reads = {line("READY 512"), line("DONE 0")};
  secure_frontend::EngineProtocolClient limit_protocol(limit_transport);
  secure_frontend::ChatExecutionService limit_service(
      limit_protocol, {.maximum_prompt_tokens = 128U, .maximum_output_tokens = 1U});
  const auto limit_result = limit_service.execute(valid_request(2U), tokenizer);
  expect(std::holds_alternative<secure_frontend::ChatExecutionResult>(limit_result) &&
             limit_transport.writes.size() == 1U && limit_transport.writes.front().rfind("GEN 1 ", 0) == 0,
         "a request above the local output bound is clamped to it (B2), not rejected");
}

void fails_closed_on_invalid_engine_output() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  auto tokenizer = tokenizer_for(prompt + "Z");
  const auto forbidden = token_id_for(tokenizer, "Z");
  expect(forbidden < tokenizer.token_types.size(), "test tokenizer must include forbidden output token");
  if (forbidden >= tokenizer.token_types.size()) return;
  tokenizer.token_types[forbidden] = 3;

  FakeTransport transport;
  transport.reads = {line("READY 512"), line("T " + std::to_string(forbidden)), line("DONE 1")};
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::ChatExecutionService service(protocol);
  const auto result = service.execute(valid_request(1U), tokenizer);
  const auto* failure = std::get_if<secure_frontend::ChatExecutionError>(&result);
  expect(failure != nullptr &&
             failure->code == secure_frontend::ChatExecutionErrorCode::output_decode_failed,
         "CONTROL token output must fail closed rather than be returned as text");
}

void executes_glm53_only_when_explicitly_selected() {
  const std::string prompt = "[gMASK]<sop><|user|>Hi<|assistant|><think>";
  const auto tokenizer = glm_tokenizer_for(prompt + "OK");
  const auto output_o = token_id_for(tokenizer, "O");
  const auto output_k = token_id_for(tokenizer, "K");
  FakeTransport transport;
  transport.reads = {line("READY 512"), line("PP 8 16 2.0 4.0"), line("T " + std::to_string(output_o)),
                     line("T " + std::to_string(output_k)), line("DONE 2")};
  secure_frontend::EngineProtocolClient protocol(
      transport, {}, secure_frontend::EngineProtocolFlavor::lighttransport_glm53);
  secure_frontend::ChatExecutionService service(
      protocol, {}, secure_frontend::NativeChatProfile::glm53);
  const auto result = service.execute({"glm-local", {{"user", "Hi"}}, 2U}, tokenizer);
  const auto* completion = std::get_if<secure_frontend::ChatExecutionResult>(&result);
  expect(completion != nullptr && completion->text == "OK" &&
             transport.writes.size() == 1U && transport.writes.front().rfind("GEN 2 ", 0) == 0,
         "the explicit GLM profile must use the same bounded private engine protocol");
}

// Gemma 4 plan 1 Task 3: the plan selection is an exhaustive switch, so a gemma4 service never
// takes the GLM (or qwen) plan: with a GLM or Qwen pack, which those plans accept, a gemma4
// request fails at planning and nothing reaches the engine (its own plan needs Gemma's markers).
void executes_gemma4_only_when_explicitly_selected() {
  const std::string glm_prompt = "[gMASK]<sop><|user|>Hi<|assistant|><think>";
  for (const auto& tokenizer : {glm_tokenizer_for(glm_prompt + "OK"), tokenizer_for("<|im_start|>user\nHi<|im_end|>\nOK")}) {
    FakeTransport transport;
    transport.reads = {line("READY 512"), line("T 0"), line("DONE 1")};
    secure_frontend::EngineProtocolClient protocol(transport, {}, secure_frontend::EngineProtocolFlavor::strata_0143);
    secure_frontend::ChatExecutionService service(protocol, {}, secure_frontend::NativeChatProfile::gemma4);
    const auto result = service.execute({"gemma-local", {{"user", "Hi"}}, 2U}, tokenizer);
    const auto* failure = std::get_if<secure_frontend::ChatExecutionError>(&result);
    expect(failure != nullptr && failure->code == secure_frontend::ChatExecutionErrorCode::planning_failed,
           "a gemma4 request on a non-Gemma pack fails at planning");
    expect(transport.writes.empty(), "nothing reaches the engine (no GEN from another profile's plan)");
  }
  // The same GLM pack through the glm53 profile does plan (the control above is meaningful).
  const auto glm = glm_tokenizer_for(glm_prompt + "OK");
  expect(std::holds_alternative<secure_frontend::ChatGenerationPlan>(
             secure_frontend::make_glm53_chat_generation_plan({"glm-local", {{"user", "Hi"}}, 2U}, glm)),
         "the GLM pack plans under glm53");
}

// SP5 M2 Task 6 (D-8): the GLM profile end to end over the maya_118 wire dialect.
// Identical token payloads yield an identical visible stream under the maya framing
// (NOTE/STAT/5-field PP — the Task-5 consumed records) as under the strata framing:
// the framing rides as ProgressEvents and never surfaces, errors, or changes the
// GEN write or the decoded answer.
void glm_profile_generation_completes_over_maya_dialect() {
  const std::string prompt = "[gMASK]<sop><|user|>Hi<|assistant|><think>";
  const auto tokenizer = glm_tokenizer_for(prompt + "OK");
  const auto output_o = token_id_for(tokenizer, "O");
  const auto output_k = token_id_for(tokenizer, "K");
  // Non-stream: Maya's records around the same T payload (generate.cpp:1347 NOTE,
  // :6102 PP, :2014 STAT; server.py:457/:677-690 the pump's own reads).
  FakeTransport transport;
  transport.reads = {line("READY 512 stop pause"), line("NOTE sessions: 1"),
                     line("PP 8 16 120 33.3 0"),
                     line("T " + std::to_string(output_o)),
                     line("STAT tok_s=42.00 ms_tok=23.81 vram_hit=0.7500 vram_used=8192"),
                     line("PP 16 16 200 33.3 16"),
                     line("T " + std::to_string(output_k)),
                     line("DONE 2 1 1.0 2.0 stop")};
  secure_frontend::EngineProtocolClient protocol(transport, {}, secure_frontend::EngineProtocolFlavor::maya_118);
  secure_frontend::ChatExecutionService service(protocol, {}, secure_frontend::NativeChatProfile::glm53);
  const auto result = service.execute({"glm-local", {{"user", "Hi"}}, 2U}, tokenizer);
  const auto* completion = std::get_if<secure_frontend::ChatExecutionResult>(&result);
  expect(completion != nullptr && completion->text == "OK" && completion->generated_tokens == 2U &&
             transport.writes.size() == 1U && transport.writes.front().rfind("GEN 2 ", 0) == 0,
         "the maya dialect completes exactly like the pinned glm53 case: same GEN, same text");
  // Stream: the event pump's visible sequence equals the strata framing's for the
  // same token payload; the maya-only records appear only as ProgressEvents.
  const auto pump_events = [&](const secure_frontend::EngineProtocolFlavor flavor,
                               const std::deque<secure_frontend::EngineReadResult>& reads) {
    FakeTransport stream;
    stream.reads = reads;
    secure_frontend::EngineProtocolClient client(stream, {}, flavor);
    secure_frontend::EngineReady ready{};
    expect(!client.await_ready(std::chrono::seconds(1), ready).has_value(), "the stream client starts on READY");
    expect(!client.begin_generation(4, {1, 2}, secure_frontend::deadline_after(std::chrono::seconds(5))).has_value(),
           "the stream generation starts");
    std::vector<std::string> visible;
    int progress = 0;
    for (;;) {
      const auto event = client.next_event(std::chrono::seconds(1));
      if (const auto* token = std::get_if<secure_frontend::TokenEvent>(&event))
        visible.push_back("T " + std::to_string(token->id));
      else if (const auto* done = std::get_if<secure_frontend::DoneEvent>(&event)) {
        visible.push_back("DONE " + std::string{done->finish} + " " + std::to_string(done->generated));
        break;
      } else if (std::holds_alternative<secure_frontend::ProgressEvent>(event))
        ++progress;
      else {
        expect(false, "no protocol error under the stream pump");
        break;
      }
    }
    return std::pair{visible, progress};
  };
  const auto strata = pump_events(secure_frontend::EngineProtocolFlavor::strata_0143,
                                  {line("READY 64"), line("PP 8 16 120 33.3"), line("T " + std::to_string(output_o)),
                                   line("T " + std::to_string(output_k)), line("DONE 2 stop")});
  const auto maya = pump_events(secure_frontend::EngineProtocolFlavor::maya_118,
                                {line("READY 64 stop pause"), line("NOTE sessions: 1"), line("PP 8 16 120 33.3 0"),
                                 line("T " + std::to_string(output_o)), line("STAT tok_s=42.00 vram_hit=0.7500"),
                                 line("T " + std::to_string(output_k)), line("NOTE mid-stream note"), line("DONE 2 stop")});
  expect(strata.first == maya.first, "identical TokenEvent/DoneEvent stream for identical token payloads");
  expect(strata.second == 1U && maya.second == 4U, "Maya's extra framing rides as private ProgressEvents only");
}

// SP5 M2 Task 6: the cancellation round trip is unchanged under the dialect —
// a timeout writes exactly one STOP, the drain to DONE accepts the maya
// cadence records, the protocol recovers idle and the next request serves.
void maya_dialect_cancellation_round_trips() {
  const std::string prompt = "[gMASK]<sop><|user|>Hi<|assistant|><think>";
  const auto tokenizer = glm_tokenizer_for(prompt + "OK");
  const auto output_o = token_id_for(tokenizer, "O");
  const auto output_k = token_id_for(tokenizer, "K");
  FakeTransport transport;
  // The maya engine advertises `READY <ctx> stop pause` (generate.cpp:3251) —
  // stop support, like the strata engine's READY, is what makes the STOP round
  // trip legal at all.
  transport.reads = {line("READY 512 stop pause"), line("NOTE sessions: 1")};
  for (int i = 0; i < 10; ++i) {
    transport.reads.push_back(line("T " + std::to_string(output_o)));
    if (i % 3 == 2) transport.reads.push_back(line("STAT tok_s=40.00 ms_tok=25.00 vram_used=8192"));
    if (i == 4) transport.reads.push_back(line("NOTE slots: keeping 1 conversation"));
  }
  transport.reads.push_back(line("DONE 10 1 1.0 1.0 cancel"));      // the drain after STOP
  transport.reads.push_back(line("T " + std::to_string(output_k)));  // the next request
  transport.reads.push_back(line("DONE 1 1 1.0 2.0 stop"));
  transport.delay_per_read = std::chrono::milliseconds(60);
  secure_frontend::EngineProtocolClient protocol(transport, {}, secure_frontend::EngineProtocolFlavor::maya_118);
  secure_frontend::ChatExecutionService service(protocol,
                                                {.generation_timeout = std::chrono::milliseconds(200),
                                                 .stop_drain_timeout = std::chrono::seconds(2)},
                                                secure_frontend::NativeChatProfile::glm53);
  const auto first = service.execute({"glm-local", {{"user", "Hi"}}, 10U}, tokenizer);
  const auto* first_error = std::get_if<secure_frontend::ChatExecutionError>(&first);
  expect(first_error != nullptr && first_error->code == secure_frontend::ChatExecutionErrorCode::generation_timed_out,
         "a slow maya-dialect generation times out: " + std::to_string(first_error ? static_cast<int>(first_error->code) : -1));
  expect(std::count(transport.writes.begin(), transport.writes.end(), "STOP") == 1, "exactly one STOP under the dialect");
  expect(protocol.idle(), "the drain through the cadence records returns the protocol to idle");
  transport.delay_per_read = std::chrono::milliseconds(0);
  const auto second = service.execute({"glm-local", {{"user", "Hi"}}, 1U}, tokenizer);
  const auto* second_completion = std::get_if<secure_frontend::ChatExecutionResult>(&second);
  expect(second_completion != nullptr && second_completion->text == "K", "the next request serves after recovery");
}

// R1 (owner ruling 2026-10-14, RF10): the GEN write is bounded by the
// request's generation deadline, fixed when the generation starts; the read to
// DONE then has only what the write left of that same deadline.
void chat_gen_write_bounded_by_generation_deadline() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt + "OK");
  FakeTransport transport;
  transport.reads = {line("READY 512 stop"), line("T " + std::to_string(token_id_for(tokenizer, "O"))), line("DONE 1")};
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::ChatExecutionService service(protocol, {.generation_timeout = std::chrono::milliseconds(7000)});
  const auto before = secure_frontend::MonotonicClock::now();
  const auto result = service.execute(valid_request(1U), tokenizer);
  const auto after = secure_frontend::MonotonicClock::now();
  expect(std::holds_alternative<secure_frontend::ChatExecutionResult>(result), "the request succeeds");
  expect(transport.deadlines.size() == 1U && transport.deadlines.front() >= before + std::chrono::milliseconds(7000) &&
             transport.deadlines.front() <= after + std::chrono::milliseconds(7000),
         "the GEN write deadline is the request's generation deadline");
}

// E2: after a timeout the service sends STOP, drains to DONE, and serves the next request.
void chat_recovers_after_generation_timeout() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt + "OK");
  const auto o = token_id_for(tokenizer, "O");
  FakeTransport transport;
  transport.reads = {line("READY 512 stop")};
  for (int i = 0; i < 10; ++i) transport.reads.push_back(line("T " + std::to_string(o)));
  transport.reads.push_back(line("DONE 10 1 1.0 1.0 cancel"));   // the drain after STOP
  transport.reads.push_back(line("T " + std::to_string(o)));      // the next request
  transport.reads.push_back(line("DONE 1 1 1.0 1.0 length"));
  transport.delay_per_read = std::chrono::milliseconds(60);
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::ChatExecutionService service(protocol, {.generation_timeout = std::chrono::milliseconds(200), .stop_drain_timeout = std::chrono::seconds(2)});
  const auto first = service.execute(valid_request(10U), tokenizer);
  const auto* first_error = std::get_if<secure_frontend::ChatExecutionError>(&first);
  expect(first_error != nullptr && first_error->code == secure_frontend::ChatExecutionErrorCode::generation_timed_out, "a slow generation must time out");
  expect(std::count(transport.writes.begin(), transport.writes.end(), "STOP") == 1, "exactly one STOP must be sent after the timeout");
  expect(protocol.idle(), "the protocol must be idle again after the drain");
  transport.delay_per_read = std::chrono::milliseconds(0);
  const auto second = service.execute(valid_request(1U), tokenizer);
  expect(std::holds_alternative<secure_frontend::ChatExecutionResult>(second), "the next request must succeed after recovery");
}

void chat_marks_engine_failed_when_drain_fails() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt + "O");
  const auto o = token_id_for(tokenizer, "O");
  FakeTransport transport;
  transport.reads = {line("READY 512 stop")};
  for (int i = 0; i < 10; ++i) transport.reads.push_back(line("T " + std::to_string(o)));
  // No DONE ever arrives: the drain must give up.
  transport.delay_per_read = std::chrono::milliseconds(60);
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::ChatExecutionService service(protocol, {.generation_timeout = std::chrono::milliseconds(150), .stop_drain_timeout = std::chrono::milliseconds(300)});
  const auto result = service.execute(valid_request(10U), tokenizer);
  const auto* error = std::get_if<secure_frontend::ChatExecutionError>(&result);
  expect(error != nullptr && error->code == secure_frontend::ChatExecutionErrorCode::engine_protocol_failed, "a failed drain is a protocol failure");
  expect(protocol.closed(), "a failed drain must close the protocol so health reports it");
}

// Review Focus 3: DONE ... nonfinite is a 503 for this request only.
void chat_rejects_nonfinite_done_and_stays_ready() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt + "O");
  const auto o = token_id_for(tokenizer, "O");
  FakeTransport transport;
  transport.reads = {line("READY 512 stop"), line("DONE 0 1 1.0 1.0 nonfinite"), line("T " + std::to_string(o)), line("DONE 1 1 1.0 1.0 length")};
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::ChatExecutionService service(protocol);
  const auto first = service.execute(valid_request(1U), tokenizer);
  const auto* error = std::get_if<secure_frontend::ChatExecutionError>(&first);
  expect(error != nullptr && error->code == secure_frontend::ChatExecutionErrorCode::engine_protocol_failed, "nonfinite must not produce text");
  expect(protocol.idle(), "nonfinite does not kill the engine; the protocol stays idle");
  const auto second = service.execute(valid_request(1U), tokenizer);
  expect(std::holds_alternative<secure_frontend::ChatExecutionResult>(second), "the engine is usable after a nonfinite request");
}

void health_reflects_protocol_state() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt + "O");
  const auto o = token_id_for(tokenizer, "O");
  FakeTransport transport;
  transport.reads = {line("READY 512 stop"), line("T " + std::to_string(o)), line("DONE 1 1 1.0 1.0 length")};
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::ChatExecutionService service(protocol);
  expect(service.health() == secure_frontend::EngineHealth::starting, "before READY the engine is starting");
  (void)service.execute(valid_request(1U), tokenizer);
  expect(service.health() == secure_frontend::EngineHealth::ready, "after a completed request the engine is ready");
}

// Review Focus 4: the engine closing stdout mid-generation is a failure, not text.
void health_is_failed_after_engine_closed_mid_generation() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt + "O");
  const auto o = token_id_for(tokenizer, "O");
  FakeTransport transport;
  transport.reads = {line("READY 512 stop"), line("T " + std::to_string(o)),
                     {secure_frontend::EngineReadKind::closed, {}, {}}};
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::ChatExecutionService service(protocol);
  const auto result = service.execute(valid_request(2U), tokenizer);
  expect(std::holds_alternative<secure_frontend::ChatExecutionError>(result), "a closed engine must not yield partial text");
  const auto* failure = std::get_if<secure_frontend::ChatExecutionError>(&result);
  expect(failure != nullptr && failure->code == secure_frontend::ChatExecutionErrorCode::engine_protocol_failed,
         "a closed engine mid-generation must report engine_protocol_failed");
  expect(service.health() == secure_frontend::EngineHealth::failed, "health must report failed after the engine closed");
}

void health_is_failed_when_engine_closes_before_ready() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt);
  FakeTransport transport;
  transport.reads = {{secure_frontend::EngineReadKind::closed, {}, {}}};
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::ChatExecutionService service(protocol);
  const auto result = service.execute(valid_request(1U), tokenizer);
  expect(std::holds_alternative<secure_frontend::ChatExecutionError>(result), "an engine that closes before READY must fail the request");
  expect(service.health() == secure_frontend::EngineHealth::failed, "health must report failed when the engine closes before READY");
}

void chat_applies_engine_context_rule_with_eight_token_margin() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt);
  // Count the prompt tokens by running one successful request first.
  FakeTransport probe; probe.reads = {line("READY 4096 stop"), line("DONE 0 1 1.0 1.0 length")};
  secure_frontend::EngineProtocolClient probe_protocol(probe);
  secure_frontend::ChatExecutionService probe_service(probe_protocol);
  (void)probe_service.execute(valid_request(1U), tokenizer);
  const std::string& gen = probe.writes.front();
  const std::size_t prompt_tokens = static_cast<std::size_t>(std::count(gen.begin(), gen.end(), ',')) + 1U;
  // ctx = prompt + max_new + 7 -> exceeds by one; ctx = prompt + max_new + 8 -> fits.
  for (const std::size_t margin : {7U, 8U}) {
    FakeTransport t; t.reads = {line("READY " + std::to_string(prompt_tokens + 1U + margin) + " stop"), line("DONE 0 1 1.0 1.0 length")};
    secure_frontend::EngineProtocolClient p(t);
    secure_frontend::ChatExecutionService s(p);
    const auto r = s.execute(valid_request(1U), tokenizer);
    const bool rejected = std::holds_alternative<secure_frontend::ChatExecutionError>(r);
    expect(rejected == (margin == 7U), "context rule must be prompt + max_new + 8 <= ctx (margin " + std::to_string(margin) + ")");
  }
}

void chat_uses_adopted_ready_without_waiting() {
  const std::string prompt = "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n";
  const auto tokenizer = tokenizer_for(prompt + "OK");
  FakeTransport transport;
  transport.reads = {line("READY 512 stop"), line("T " + std::to_string(token_id_for(tokenizer, "O"))), line("DONE 1")};
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::EngineReady ready{};
  expect(!protocol.await_ready(std::chrono::seconds(1), ready).has_value(), "the supervisor consumed READY");
  secure_frontend::ChatExecutionService service(protocol);
  service.adopt_ready(ready);
  expect(service.health() == secure_frontend::EngineHealth::ready, "an adopted READY is ready before any request");
  const auto result = service.execute(valid_request(), tokenizer);
  expect(std::holds_alternative<secure_frontend::ChatExecutionResult>(result), "execute does not wait for a second READY");
}

void resolve_max_tokens_follows_decisions_1_and_2() {
  using secure_frontend::resolve_max_tokens;
  // requested, prompt, ctx, max_completion, default -> result
  expect(resolve_max_tokens(100U, 10U, 4096U, 4096U, 0U) == std::optional<std::size_t>{100U}, "fits as asked");
  expect(resolve_max_tokens(16384U, 10U, 100000U, 4096U, 0U) == std::optional<std::size_t>{4096U}, "clamped to max_completion (B2)");
  expect(resolve_max_tokens(1000U, 50U, 100U, 4096U, 0U) == std::optional<std::size_t>{42U}, "clamped to ctx - n - 8 (B2)");
  expect(resolve_max_tokens(std::nullopt, 50U, 100U, 4096U, 0U) == std::optional<std::size_t>{42U}, "auto = min(max, ctx - n - 8) (B1)");
  expect(resolve_max_tokens(std::nullopt, 10U, 100000U, 4096U, 0U) == std::optional<std::size_t>{4096U}, "auto capped by max_completion");
  expect(resolve_max_tokens(std::nullopt, 10U, 4096U, 4096U, 256U) == std::optional<std::size_t>{256U}, "a configured default applies");
  expect(!resolve_max_tokens(5U, 92U, 100U, 4096U, 0U).has_value(), "prompt alone fills the context: 400 (B2)");
  expect(resolve_max_tokens(5U, 91U, 100U, 4096U, 0U) == std::optional<std::size_t>{1U}, "one token of room is still allowed");
  expect(resolve_max_tokens(5U, 100000U, 0U, 4096U, 0U) == std::optional<std::size_t>{5U}, "unknown context: no context clamp");
}

// SP5 M1 Task 5 (parity row fit_max_tokens): the pin's two behaviours (serve/server.py:4526-4544).
// fit_max_tokens=false turns an EXPLICIT max_tokens over the room into a refusal instead of a
// shortening (:4536-4543); an auto request still takes the room (:4526-4540) and the engine-internal
// max_completion cap still clamps (B2 is not the pin's room rule; C-8 keeps it).
void resolve_max_tokens_fit_false_refuses_like_the_pin() {
  using secure_frontend::resolve_max_tokens;
  expect(resolve_max_tokens(1000U, 50U, 100U, 4096U, 0U, false) == std::nullopt,
         "fit false: an explicit request over the room is refused, never shortened (:4536)");
  expect(resolve_max_tokens(std::nullopt, 50U, 100U, 4096U, 0U, false) == std::optional<std::size_t>{42U},
         "fit false: an auto request still fits the room (:4526)");
  expect(resolve_max_tokens(16384U, 10U, 100000U, 4096U, 0U, false) == std::optional<std::size_t>{4096U},
         "fit false: the max_completion cap still clamps (B2)");
  expect(resolve_max_tokens(1000U, 50U, 100U, 4096U, 0U, true) == std::optional<std::size_t>{42U},
         "fit true keeps the shortening, today's behaviour (:4545)");
}

// SP5 M1 Task 8 (parity rows engine.min_tokens / engine.max_tokens, Task-2 rows "added
// frontend bound per C-1"): the resolved output budget rides the pin's §7.2 room rules —
// the floor lifts a tiny request first, the ceiling caps below the completion ceiling
// last, and neither ever rescues a fit=false room refusal.
void min_and_max_tokens_bounds_clamp_at_resolve() {
  using secure_frontend::resolve_max_tokens;
  expect(resolve_max_tokens(5U, 10U, 100000U, 4096U, 0U, true, 64U, 0U) == std::optional<std::size_t>{64U},
         "engine.min_tokens lifts a tiny explicit request");
  expect(resolve_max_tokens(std::nullopt, 10U, 100000U, 4096U, 0U, true, 64U, 0U) == std::optional<std::size_t>{4096U},
         "the max_completion cap still wins over the floor");
  expect(resolve_max_tokens(4000U, 10U, 100000U, 4096U, 0U, true, 0U, 512U) == std::optional<std::size_t>{512U},
         "engine.max_tokens caps below the completion ceiling");
  expect(resolve_max_tokens(4000U, 50U, 100U, 4096U, 0U, false, 0U, 512U) == std::nullopt,
         "the ceiling never rescues a fit=false room refusal (:4536 stays)");
  expect(resolve_max_tokens(5U, 50U, 100U, 4096U, 0U, true, 0U, 0U) == std::optional<std::size_t>{5U},
         "unset bounds change nothing");
  expect(resolve_max_tokens(5U, 80U, 100U, 4096U, 0U, true, 64U, 0U) == std::optional<std::size_t>{12U},
         "the context room still caps a lifted floor");
}

// SP3 Task 15 (run_generation, spec §2.6 P6): a transport that answers STOP
// with a cancel DONE the next time it is read. Adapted to the re-anchored
// write_line(line, deadline, error) signature; assertions as planned.
class StopAwareTransport final : public secure_frontend::EngineLineTransport {
 public:
  std::deque<std::string> reads;
  std::vector<std::string> writes;
  std::string cancel_line{"DONE 1 3 0 0 cancel"};
  bool answer_stop{true};
  bool write_line(const std::string_view line, const secure_frontend::MonotonicClock::time_point, std::string&) override {
    writes.emplace_back(line);
    if (line == "STOP" && answer_stop) reads.push_back(cancel_line);
    return true;
  }
  secure_frontend::EngineReadResult read_line(const std::chrono::milliseconds timeout) override {
    if (reads.empty()) {
      std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(2)));
      return {secure_frontend::EngineReadKind::timeout, {}, {}};
    }
    std::string next = std::move(reads.front());
    reads.pop_front();
    return {secure_frontend::EngineReadKind::line, std::move(next), {}};
  }
};

struct ReadyService final {
  StopAwareTransport transport;
  secure_frontend::EngineProtocolClient protocol{transport};
  secure_frontend::ChatExecutionService service;
  explicit ReadyService(secure_frontend::ChatExecutionLimits limits = {}) : service(protocol, limits) {
    transport.reads = {"READY 512 stop"};
    secure_frontend::EngineReady ready{};
    expect(!protocol.await_ready(std::chrono::seconds(1), ready).has_value(), "READY consumed");
    service.adopt_ready(ready);
  }
};

void run_generation_reports_finish_usage_and_context() {
  const auto tokenizer = tokenizer_for("OK");
  ReadyService f;
  expect(f.service.context_tokens() == 512U, "context_tokens is the READY context");
  const auto o = token_id_for(tokenizer, "O");
  const auto k = token_id_for(tokenizer, "K");
  f.transport.reads = {"T " + std::to_string(o), "T " + std::to_string(k), "DONE 2 3 1.0 1.0 length"};
  const auto outcome = f.service.run_generation({1U, 2U, 3U}, {.max_tokens = 2U}, {}, tokenizer);
  const auto* result = std::get_if<secure_frontend::GenerationResult>(&outcome);
  expect(result != nullptr && result->text == "OK" && result->prompt_tokens == 3U && result->completion_tokens == 2U &&
         result->finish == secure_frontend::FinishReason::length, "text, usage and finish from DONE");
  expect(f.transport.writes.front().rfind("GEN 2 ", 0) == 0, "GEN carries the resolved budget");
  f.transport.reads = {"T " + std::to_string(o), "DONE stop"};
  const auto short_form = f.service.run_generation({1U}, {.max_tokens = 5U}, {}, tokenizer);
  expect(std::holds_alternative<secure_frontend::GenerationResult>(short_form) &&
         std::get<secure_frontend::GenerationResult>(short_form).finish == secure_frontend::FinishReason::stop, "short DONE is stop");
}

void run_generation_cancel_sends_stop_and_drains() {
  const auto tokenizer = tokenizer_for("O");
  ReadyService f;
  f.transport.reads = {"T " + std::to_string(token_id_for(tokenizer, "O"))};
  int checks = 0;
  const auto outcome = f.service.run_generation({1U, 2U, 3U}, {.max_tokens = 100U}, {.cancelled = [&checks] { return ++checks > 2; }}, tokenizer);
  expect(std::holds_alternative<secure_frontend::GenerationFailure>(outcome) &&
         std::get<secure_frontend::GenerationFailure>(outcome) == secure_frontend::GenerationFailure::cancelled, "a gone client cancels");
  expect(std::count(f.transport.writes.begin(), f.transport.writes.end(), "STOP") == 1, "exactly one STOP");
  expect(f.protocol.idle(), "drained to DONE: the engine is idle for the next request");
}

void run_generation_timeout_stops_and_reports_timed_out() {
  const auto tokenizer = tokenizer_for("O");
  ReadyService f({.generation_timeout = std::chrono::milliseconds(100), .stop_drain_timeout = std::chrono::milliseconds(500)});
  const auto started = std::chrono::steady_clock::now();
  const auto outcome = f.service.run_generation({1U}, {.max_tokens = 100U}, {}, tokenizer);
  const auto* result = std::get_if<secure_frontend::GenerationResult>(&outcome);
  expect(result != nullptr && result->timed_out && result->finish == secure_frontend::FinishReason::length,
         "a timeout returns the text so far with length (B4), flagged timed_out for the log");
  expect(std::chrono::steady_clock::now() - started < std::chrono::seconds(1), "bounded by generation_timeout + drain");
  expect(f.protocol.idle(), "STOP drained");
}

// SP5 M1 Task 8 (parity row engine_silence_s; runconfig.py:39, serve/server.py:274-276):
// the stall watchdog fires at the configured silence window — an engine that says nothing
// valid reaches the SP3 stall path (STOP, drain, timed-out length) at silence_s, not at
// the full generation deadline.
void engine_silence_s_sets_the_stall_watchdog() {
  const auto tokenizer = tokenizer_for("O");
  ReadyService f({.generation_timeout = std::chrono::milliseconds(3000), .stop_drain_timeout = std::chrono::milliseconds(500),
                  .silence_timeout = std::chrono::milliseconds(100)});
  const auto started = std::chrono::steady_clock::now();
  const auto outcome = f.service.run_generation({1U}, {.max_tokens = 100U}, {}, tokenizer);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  const auto* result = std::get_if<secure_frontend::GenerationResult>(&outcome);
  expect(result != nullptr && result->timed_out && result->finish == secure_frontend::FinishReason::length,
         "the silence window takes the same stall path as generation_timeout (B4)");
  expect(elapsed < std::chrono::seconds(1), "the watchdog fires at the silence bound, not the 3 s deadline");
  expect(std::count(f.transport.writes.begin(), f.transport.writes.end(), "STOP") == 1, "the stalled generation is stopped");
  expect(f.protocol.idle(), "STOP drained to DONE");
}

// SP5 M1 Task 8 (parity row repeat_stop_tokens; serve/server.py:197/:4908, notice :5088):
// N equal tokens in a row end a reply as "length" — the pin's detector is the frontend's,
// so nothing reaches the engine beyond the usual STOP. The run is kept; everything after
// it is dropped. 0 keeps the old behaviour (the pin's opt-out, :8115).
void repeat_stop_tokens_ends_a_runaway_reply() {
  const auto tokenizer = tokenizer_for("O");
  const auto o = token_id_for(tokenizer, "O");
  ReadyService f({.generation_timeout = std::chrono::seconds(5), .stop_drain_timeout = std::chrono::milliseconds(500),
                  .repeat_stop_tokens = 4U});
  for (int i = 0; i < 40; ++i) f.transport.reads.push_back("T " + std::to_string(o));
  const auto outcome = f.service.run_generation({1U}, {.max_tokens = 100U}, {}, tokenizer);
  const auto* result = std::get_if<secure_frontend::GenerationResult>(&outcome);
  expect(result != nullptr && result->text == "OOOO" && result->finish == secure_frontend::FinishReason::length,
         "the run of four identical tokens ends the reply as length (the pin's word, :5088)");
  expect(std::count(f.transport.writes.begin(), f.transport.writes.end(), "STOP") == 1, "the runaway reply is stopped once");
  expect(f.protocol.idle(), "drained to DONE: the engine is idle for the next request");
  ReadyService off({.generation_timeout = std::chrono::seconds(5)});
  for (int i = 0; i < 40; ++i) off.transport.reads.push_back("T " + std::to_string(o));
  off.transport.reads.push_back("DONE 40");
  const auto open = off.service.run_generation({1U}, {.max_tokens = 100U}, {}, tokenizer);
  expect(std::holds_alternative<secure_frontend::GenerationResult>(open) &&
             std::get<secure_frontend::GenerationResult>(open).text.size() == 40U,
         "repeat_stop_tokens 0: no stop rule (the pin's opt-out)");
}

void run_generation_drain_failure_abandons() {
  const auto tokenizer = tokenizer_for("O");
  ReadyService f({.generation_timeout = std::chrono::milliseconds(50), .stop_drain_timeout = std::chrono::milliseconds(100)});
  f.transport.answer_stop = false;
  const auto outcome = f.service.run_generation({1U}, {.max_tokens = 100U}, {}, tokenizer);
  expect(std::holds_alternative<secure_frontend::GenerationFailure>(outcome) &&
         std::get<secure_frontend::GenerationFailure>(outcome) == secure_frontend::GenerationFailure::engine_unavailable,
         "an engine that ignores STOP is unavailable");
  expect(f.protocol.closed(), "and abandoned (the route reports drain_failed through the port)");
}

void run_generation_shutdown_uses_short_drain() {
  const auto tokenizer = tokenizer_for("O");
  ReadyService f;
  std::atomic<bool> shutting{true};
  const auto outcome = f.service.run_generation({1U}, {.max_tokens = 100U}, {.cancelled = {}, .shutting_down = &shutting}, tokenizer);
  expect(std::holds_alternative<secure_frontend::GenerationFailure>(outcome) &&
         std::get<secure_frontend::GenerationFailure>(outcome) == secure_frontend::GenerationFailure::shutting_down, "shutdown");
  expect(std::count(f.transport.writes.begin(), f.transport.writes.end(), "STOP") == 1, "STOP sent during shutdown");
}

void run_generation_maps_engine_errors() {
  const auto tokenizer = tokenizer_for("O");
  {
    ReadyService f;
    f.transport.reads = {"DONE 0 1 1.0 1.0 nonfinite"};
    const auto outcome = f.service.run_generation({1U}, {.max_tokens = 5U}, {}, tokenizer);
    expect(std::get<secure_frontend::GenerationFailure>(outcome) == secure_frontend::GenerationFailure::engine_error && f.protocol.idle(),
           "nonfinite: engine_error, engine stays idle");
  }
  {
    ReadyService f;
    f.transport.reads = {"ERR bad request"};
    const auto outcome = f.service.run_generation({1U}, {.max_tokens = 5U}, {}, tokenizer);
    expect(std::get<secure_frontend::GenerationFailure>(outcome) == secure_frontend::GenerationFailure::engine_error && f.protocol.closed(),
           "ERR: engine_error, protocol closed (SP2 restarts and counts)");
  }
  {
    ReadyService f;
    f.transport.reads = {"T 0", "T 0", "DONE 2 1 0 0 stop"};
    const auto outcome = f.service.run_generation({1U}, {.max_tokens = 1U}, {}, tokenizer);
    expect(std::get<secure_frontend::GenerationFailure>(outcome) == secure_frontend::GenerationFailure::engine_error, "more tokens than asked");
  }
}

// SP3 M2 Task 5: drive_generation and the rebuilt run_generation.
// Ruling t5-test (as in Task 4): M1's write_line carries the absolute write
// deadline and begin_generation the GEN write deadline; the brief's shorter
// helper signatures are adapted to them, assertions unchanged.
std::string byte_id(const unsigned char byte) { return std::to_string(test_support::id_for_byte(byte)); }

// Thread-safe scripted transport (mandatory: a test thread appends while the loop reads).
class LockedTransport final : public secure_frontend::EngineLineTransport {
 public:
  void push(std::string line) { std::lock_guard lock(mutex_); reads_.push_back(std::move(line)); }
  [[nodiscard]] std::size_t stops() const { std::lock_guard lock(mutex_); return static_cast<std::size_t>(std::count(writes_.begin(), writes_.end(), "STOP")); }
  bool write_line(const std::string_view line, const secure_frontend::MonotonicClock::time_point, std::string&) override {
    std::lock_guard lock(mutex_);
    writes_.emplace_back(line);
    return true;
  }
  secure_frontend::EngineReadResult read_line(const std::chrono::milliseconds timeout) override {
    {
      std::lock_guard lock(mutex_);
      if (!reads_.empty()) {
        std::string next = std::move(reads_.front());
        reads_.pop_front();
        return {secure_frontend::EngineReadKind::line, std::move(next), {}};
      }
    }
    std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(2)));
    return {secure_frontend::EngineReadKind::timeout, {}, {}};
  }

 private:
  mutable std::mutex mutex_;
  std::deque<std::string> reads_;
  std::vector<std::string> writes_;
};

struct Collector final : secure_frontend::GenerationObserver {
  std::string text;
  int cancel_after_ticks{-1};
  int ticks{0};
  void on_text(const std::string_view piece) override { text += piece; }
  secure_frontend::RunCancel on_tick(bool) override {
    ++ticks;
    return cancel_after_ticks >= 0 && ticks > cancel_after_ticks ? secure_frontend::RunCancel::client_gone : secure_frontend::RunCancel::none;
  }
};

// A protocol already past READY and GEN, reading `records`.
struct Running final {
  LockedTransport transport;
  secure_frontend::EngineProtocolClient protocol{transport};
  explicit Running(const std::vector<std::string>& records) {
    transport.push("READY 512 stop");
    secure_frontend::EngineReady ready{};
    (void)protocol.await_ready(std::chrono::seconds(1), ready);
    (void)protocol.begin_generation(16, {1}, secure_frontend::deadline_after(std::chrono::seconds(5)));
    for (const auto& record : records) transport.push(record);
  }
};

void drive_generation_bytes_cap_cuts_on_code_point() {
  const auto pack = test_support::byte_tokenizer();
  const secure_frontend::TokenByteDecoder decoder(pack);
  Running run({"T " + byte_id('A'), "T " + byte_id(0xE2), "T " + byte_id(0x82), "T " + byte_id(0xAC), "T " + byte_id('B'),
               "DONE 5 1 0 0 cancel"});
  Collector collector;
  const auto result = secure_frontend::drive_generation(run.protocol, decoder, {.max_generated_bytes = 2U, .max_tokens = 16U}, collector);
  expect(collector.text == "A", "the euro sign does not fit in the 1 byte left and is not split: [" + collector.text + "]");
  expect(result.end == secure_frontend::RunEnd::length && result.bytes_capped, "the cap ends with length");
  expect(run.transport.stops() == 1U, "the cap sends STOP once");
  expect(run.protocol.idle(), "drained to idle");
}

void drive_generation_drops_tail_on_length_and_replaces_on_stop() {
  const auto pack = test_support::byte_tokenizer();
  const secure_frontend::TokenByteDecoder decoder(pack);
  Running cut({"T " + byte_id('A'), "T " + byte_id(0xE2), "DONE 2 1 0 0 length"});
  Collector at_length;
  expect(secure_frontend::drive_generation(cut.protocol, decoder, {}, at_length).end == secure_frontend::RunEnd::length && at_length.text == "A",
         "length: the held lead byte is dropped");
  Running eos({"T " + byte_id('A'), "T " + byte_id(0xE2), "DONE 2 1 0 0 stop"});
  Collector at_stop;
  expect(secure_frontend::drive_generation(eos.protocol, decoder, {}, at_stop).end == secure_frontend::RunEnd::stop &&
             at_stop.text == "A\xEF\xBF\xBD", "stop: the held tail becomes one U+FFFD");
}

void drive_generation_cancel_sends_stop_and_drains() {
  const auto pack = test_support::byte_tokenizer();
  const secure_frontend::TokenByteDecoder decoder(pack);
  Running run({"T " + byte_id('A')});
  Collector collector;
  collector.cancel_after_ticks = 1;
  std::thread late([&run] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    run.transport.push("DONE 1 1 0 0 cancel");
  });
  const auto result = secure_frontend::drive_generation(run.protocol, decoder, {}, collector);
  late.join();
  expect(result.end == secure_frontend::RunEnd::cancelled && result.cancel == secure_frontend::RunCancel::client_gone, "cancel drains to cancelled");
  expect(run.transport.stops() == 1U, "one STOP");
}

void chat_max_tokens_cut_mid_character_returns_length() {
  const auto pack = test_support::byte_tokenizer();
  LockedTransport transport;
  transport.push("READY 512 stop");
  secure_frontend::EngineProtocolClient protocol(transport);
  secure_frontend::ChatExecutionService service(protocol);
  for (const auto& record : {"T " + byte_id('A'), "T " + byte_id(0xE2), "T " + byte_id(0x82), std::string("DONE 3 1 0 0 length")}) transport.push(record);
  const auto outcome = service.run_generation({1U, 2U}, {.max_tokens = 3U}, {}, pack);
  const auto* result = std::get_if<secure_frontend::GenerationResult>(&outcome);
  expect(result != nullptr, "a response cut mid-character is not decode_failed (spec §3.3 latent bug)");
  if (result != nullptr) {
    expect(result->text == "A" && result->finish == secure_frontend::FinishReason::length && result->completion_tokens == 3U, "text so far, length");
  }
}

// Issue 19 (docs/validation/2026-10-21-windows-gpu-session.md): Qwen3.8 emits <think> and
// </think> as USER_DEFINED tokens, and every chat failed as decode_failed. The reasoning
// split follows upstream's OutputParser (serve/frontend.py:884, :1245-1284).
secure_frontend::TokenizerPack think_tokenizer() {
  auto pack = test_support::byte_tokenizer();
  pack.tokens_by_id.emplace_back("<think>");
  pack.token_types.push_back(4);
  pack.tokens_by_id.emplace_back("</think>");
  pack.token_types.push_back(4);
  return pack;
}

secure_frontend::ThinkTokens think_ids(const secure_frontend::TokenizerPack& pack) {
  return {pack.tokens_by_id.size() - 2U, pack.tokens_by_id.size() - 1U};
}

std::string id_line(const std::size_t id) { return "T " + std::to_string(id); }

struct ReasoningCollector final : secure_frontend::GenerationObserver {
  std::string text;
  std::string reasoning;
  std::string order;  // one letter per callback: r(easoning) or c(ontent)
  void on_text(const std::string_view piece) override {
    text += piece;
    order += 'c';
  }
  void on_reasoning(const std::string_view piece) override {
    reasoning += piece;
    order += 'r';
  }
  secure_frontend::RunCancel on_tick(bool) override { return secure_frontend::RunCancel::none; }
};

void drive_generation_splits_reasoning_at_the_think_close() {
  const auto pack = think_tokenizer();
  const auto think = think_ids(pack);
  const secure_frontend::TokenByteDecoder decoder(pack);
  Running run({"T " + byte_id('h'), "T " + byte_id('m'), id_line(think.close), "T " + byte_id('\n'), "T " + byte_id('\n'),
               "T " + byte_id('O'), "T " + byte_id('K'), "DONE 7 1 0 0 stop"});
  ReasoningCollector collector;
  const auto result = secure_frontend::drive_generation(
      run.protocol, decoder, {.max_tokens = 16U, .think_tokens = think, .starts_in_reasoning = true}, collector);
  expect(result.end == secure_frontend::RunEnd::stop && result.completion_tokens == 7U, "the reply ends normally, every token counted");
  expect(collector.reasoning == "hm", "text before </think> is reasoning: [" + collector.reasoning + "]");
  expect(collector.text == "OK", "the tag and the newlines after it are not answer text: [" + collector.text + "]");

  Running spaced({id_line(think.close), "T " + byte_id('\n'), "T " + byte_id(' '), "T " + byte_id('x'), "DONE 4 1 0 0 stop"});
  ReasoningCollector kept;
  (void)secure_frontend::drive_generation(spaced.protocol, decoder,
                                          {.max_tokens = 16U, .think_tokens = think, .starts_in_reasoning = true}, kept);
  expect(kept.text == " x" && kept.reasoning.empty(), "only newlines are dropped after </think>, spaces stay (frontend.py:1279-1284)");
}

// Thinking off closes the block in the prompt, yet the model may still open one: the
// tags never reach the answer (upstream would pass them through as content).
void drive_generation_think_open_while_answering_starts_reasoning() {
  const auto pack = think_tokenizer();
  const auto think = think_ids(pack);
  const secure_frontend::TokenByteDecoder decoder(pack);
  Running run({"T " + byte_id('A'), id_line(think.open), "T " + byte_id('x'), id_line(think.close), "T " + byte_id('\n'),
               "T " + byte_id('B'), "DONE 6 1 0 0 stop"});
  ReasoningCollector collector;
  const auto result = secure_frontend::drive_generation(run.protocol, decoder, {.max_tokens = 16U, .think_tokens = think}, collector);
  expect(result.end == secure_frontend::RunEnd::stop, "a reopened block is not an error");
  expect(collector.text == "AB" && collector.reasoning == "x" && collector.order == "crc",
         "answer, reasoning, answer in order: [" + collector.text + "] [" + collector.reasoning + "] " + collector.order);
}

void drive_generation_caps_reasoning_and_answer_together() {
  const auto pack = think_tokenizer();
  const auto think = think_ids(pack);
  const secure_frontend::TokenByteDecoder decoder(pack);
  Running run({"T " + byte_id('a'), "T " + byte_id('b'), id_line(think.close), "T " + byte_id('c'), "T " + byte_id('d'),
               "DONE 5 1 0 0 cancel"});
  ReasoningCollector collector;
  const auto result = secure_frontend::drive_generation(
      run.protocol, decoder, {.max_generated_bytes = 3U, .max_tokens = 16U, .think_tokens = think, .starts_in_reasoning = true},
      collector);
  expect(collector.reasoning == "ab" && collector.text == "c", "one byte cap over both: [" + collector.reasoning + "] [" + collector.text + "]");
  expect(result.end == secure_frontend::RunEnd::length && result.bytes_capped && run.transport.stops() == 1U, "the cap stops once");
}

void drive_generation_ends_a_split_character_at_the_tag() {
  const auto pack = think_tokenizer();
  const auto think = think_ids(pack);
  const secure_frontend::TokenByteDecoder decoder(pack);
  Running run({"T " + byte_id('a'), "T " + byte_id(0xE2), id_line(think.close), "T " + byte_id('b'), "DONE 4 1 0 0 stop"});
  ReasoningCollector collector;
  (void)secure_frontend::drive_generation(run.protocol, decoder,
                                          {.max_tokens = 16U, .think_tokens = think, .starts_in_reasoning = true}, collector);
  expect(collector.reasoning == "a\xEF\xBF\xBD" && collector.text == "b",
         "an incomplete character before the tag becomes one U+FFFD in its own channel, never crosses it");
}

void drive_generation_keeps_other_special_tokens_fail_closed() {
  const auto pack = think_tokenizer();
  const auto think = think_ids(pack);
  const secure_frontend::TokenByteDecoder decoder(pack);
  const auto start = token_id_for(pack, "<|im_start|>");
  Running run({"T " + byte_id('a'), id_line(start), "DONE 2 1 0 0 cancel"});
  ReasoningCollector collector;
  expect(secure_frontend::drive_generation(run.protocol, decoder,
                                           {.max_tokens = 16U, .think_tokens = think, .starts_in_reasoning = true}, collector)
                 .end == secure_frontend::RunEnd::undecodable,
         "a CONTROL token other than the two tags still fails closed");
  Running unconfigured({id_line(think.close), "DONE 1 1 0 0 cancel"});
  ReasoningCollector plain;
  expect(secure_frontend::drive_generation(unconfigured.protocol, decoder, {.max_tokens = 16U}, plain).end ==
             secure_frontend::RunEnd::undecodable,
         "without think_tokens the tags fail closed as before");
}

// Web search (spec 2026-10-27-web-search-design §2): while the server offers its tool, the Qwen tool tags are output as
// their text in the channel they were written in (upstream decodes every token as its text), for the call parser; the
// end of the reasoning is told once, at its </think>.
struct LiteralCollector final : secure_frontend::GenerationObserver {
  std::string text;
  std::string reasoning;
  std::string order;  // r(easoning), e(nd of the reasoning), c(ontent)
  void on_text(const std::string_view piece) override {
    text += piece;
    order += 'c';
  }
  void on_reasoning(const std::string_view piece) override {
    reasoning += piece;
    order += 'r';
  }
  void on_reasoning_end() override { order += 'e'; }
  secure_frontend::RunCancel on_tick(bool) override { return secure_frontend::RunCancel::none; }
};

void drive_generation_outputs_literal_tokens_as_their_text() {
  auto pack = think_tokenizer();
  const auto think = think_ids(pack);
  pack.tokens_by_id.emplace_back("<tool_call>");
  pack.token_types.push_back(4);
  pack.tokens_by_id.emplace_back("</tool_call>");
  pack.token_types.push_back(4);
  const std::size_t open = pack.tokens_by_id.size() - 2U;
  const std::size_t close = pack.tokens_by_id.size() - 1U;
  const secure_frontend::TokenByteDecoder decoder(pack);
  const std::vector<std::pair<std::size_t, std::string>> literals{{open, "<tool_call>"}, {close, "</tool_call>"}};
  Running run({"T " + byte_id('a'), id_line(open), "T " + byte_id('b'), id_line(think.close), "T " + byte_id('\n'),
               id_line(open), "T " + byte_id('q'), id_line(close), "DONE 8 1 0 0 stop"});
  LiteralCollector collector;
  const auto result = secure_frontend::drive_generation(
      run.protocol, decoder,
      {.max_tokens = 16U, .think_tokens = think, .starts_in_reasoning = true, .literal_tokens = literals}, collector);
  expect(result.end == secure_frontend::RunEnd::stop && result.completion_tokens == 8U, "the reply ends as stop");
  expect(collector.reasoning == "a<tool_call>b", "a tag inside the reasoning is reasoning text: [" + collector.reasoning + "]");
  expect(collector.text == "<tool_call>q</tool_call>", "the tags in the answer are answer text: [" + collector.text + "]");
  expect(collector.order.find('e') != std::string::npos && collector.order.find('e') == collector.order.rfind('e') &&
             collector.order.substr(0U, collector.order.find('e')).find('c') == std::string::npos &&
             collector.order.substr(collector.order.find('e')).find('r') == std::string::npos,
         "one end of the reasoning, between its text and the answer: " + collector.order);

  Running bare({id_line(open), "DONE 1 1 0 0 cancel"});
  LiteralCollector plain;
  expect(secure_frontend::drive_generation(bare.protocol, decoder, {.max_tokens = 16U, .think_tokens = think}, plain).end ==
             secure_frontend::RunEnd::undecodable,
         "without literal_tokens a tag fails closed as before");

  Running reopened({"T " + byte_id('A'), id_line(think.open), "T " + byte_id('x'), id_line(think.close), "T " + byte_id('B'),
                    "DONE 5 1 0 0 stop"});
  LiteralCollector twice;
  (void)secure_frontend::drive_generation(reopened.protocol, decoder, {.max_tokens = 16U, .think_tokens = think}, twice);
  expect(twice.order == "crec", "a reopened span ends once too: " + twice.order);
}

// Issue 26 (docs/validation/2026-10-21-windows-gpu-session.md): in serve mode the engine
// prints the end-of-turn token as a T record before DONE ... stop (generate.cpp:14578-14590).
// It ends the reply and is never output (upstream's stop ids, serve/server.py:5086-5091).
void drive_generation_end_of_turn_ends_the_reply_as_stop() {
  const auto pack = think_tokenizer();
  const auto think = think_ids(pack);
  const auto im_end = token_id_for(pack, "<|im_end|>");
  const secure_frontend::TokenByteDecoder decoder(pack);
  Running run({"T " + byte_id('O'), "T " + byte_id('K'), id_line(im_end), "DONE 3 1 0 0 stop"});
  ReasoningCollector collector;
  const auto result = secure_frontend::drive_generation(run.protocol, decoder, {.max_tokens = 16U, .stop_tokens = {im_end}}, collector);
  expect(result.end == secure_frontend::RunEnd::stop && collector.text == "OK" && result.completion_tokens == 3U,
         "<|im_end|> ends the reply as stop and is not text: [" + collector.text + "]");
  expect(run.transport.stops() == 0U && run.protocol.idle(), "the engine ends the turn itself: no STOP");

  Running trailing({"T " + byte_id('O'), id_line(im_end), "T " + byte_id('X'), "DONE 3 1 0 0 length"});
  ReasoningCollector cut;
  const auto cut_result = secure_frontend::drive_generation(trailing.protocol, decoder, {.max_tokens = 16U, .stop_tokens = {im_end}}, cut);
  expect(cut_result.end == secure_frontend::RunEnd::stop && cut.text == "O", "nothing after the end of the turn is output");

  Running thinking({"T " + byte_id('a'), id_line(im_end), "DONE 2 1 0 0 stop"});
  ReasoningCollector unanswered;
  const auto thinking_result = secure_frontend::drive_generation(
      thinking.protocol, decoder,
      {.max_tokens = 16U, .think_tokens = think, .starts_in_reasoning = true, .stop_tokens = {im_end}}, unanswered);
  expect(thinking_result.end == secure_frontend::RunEnd::stop && unanswered.reasoning == "a" && unanswered.text.empty(),
         "a turn may end inside the reasoning");

  Running unconfigured({id_line(im_end), "DONE 1 1 0 0 cancel"});
  ReasoningCollector plain;
  expect(secure_frontend::drive_generation(unconfigured.protocol, decoder, {.max_tokens = 16U}, plain).end ==
             secure_frontend::RunEnd::undecodable,
         "without stop_tokens a CONTROL token still fails closed");
}

// DeepSeek-V4.1 / MiniMax-M3.1 (spec 2026-10-27-strata-prepare-design §7.1): Maya's engines stop only on their EOS, and
// Maya's server ends the reply on the turn markers itself, then sends STOP and drains (serve/server.py:1749-1764,
// :1019-1044). A server stop token ends the reply as stop and is never output; exactly one STOP goes to the engine, and
// the DONE after it (cancel, or the engine's own stop when it had ended the turn already) ends the run as stop.
void drive_generation_server_stop_token_sends_stop_and_drains() {
  const auto pack = think_tokenizer();
  const auto think = think_ids(pack);
  const auto marker = token_id_for(pack, "<|im_end|>");  // (standing for <｜User｜>)
  const secure_frontend::TokenByteDecoder decoder(pack);
  Running run({"T " + byte_id('O'), "T " + byte_id('K'), id_line(marker), "T " + byte_id('X'), "DONE 4 1 0 0 cancel"});
  ReasoningCollector collector;
  const auto result =
      secure_frontend::drive_generation(run.protocol, decoder, {.max_tokens = 16U, .server_stop_tokens = {marker}}, collector);
  expect(result.end == secure_frontend::RunEnd::stop && collector.text == "OK" && result.completion_tokens == 4U,
         "a server stop token ends the reply as stop and is not text: [" + collector.text + "]");
  expect(run.transport.stops() == 1U && run.protocol.idle(), "one STOP, drained to DONE");

  Running ended({"T " + byte_id('O'), id_line(marker), "DONE 2 1 0 0 stop"});
  ReasoningCollector own;
  const auto own_result =
      secure_frontend::drive_generation(ended.protocol, decoder, {.max_tokens = 16U, .server_stop_tokens = {marker}}, own);
  expect(own_result.end == secure_frontend::RunEnd::stop && own.text == "O" && ended.transport.stops() == 1U &&
             ended.protocol.idle(),
         "an engine that had stopped on it itself still reads one STOP, and the run is a stop");

  Running thinking({"T " + byte_id('a'), id_line(marker), "DONE 2 1 0 0 cancel"});
  ReasoningCollector unanswered;
  const auto thinking_result = secure_frontend::drive_generation(
      thinking.protocol, decoder,
      {.max_tokens = 16U, .think_tokens = think, .starts_in_reasoning = true, .server_stop_tokens = {marker}}, unanswered);
  expect(thinking_result.end == secure_frontend::RunEnd::stop && unanswered.reasoning == "a" && unanswered.text.empty(),
         "a turn may end inside the reasoning");
}

// MiniMax-M3.1 (owner 2026-10-28): Maya's parser keeps the newlines after the reasoning's close for MiniMax (its lead
// rule is GLM's and Qwen's: `if self.lead and self.dsml is None`); keeps_newlines_after_think does the same.
void drive_generation_keeps_the_newlines_after_think_when_asked() {
  const auto pack = think_tokenizer();
  const auto think = think_ids(pack);
  const secure_frontend::TokenByteDecoder decoder(pack);
  const auto reply = [&](const bool keep) {
    Running run({"T " + byte_id('r'), id_line(think.close), "T " + byte_id('\n'), "T " + byte_id('\n'), "T " + byte_id('A'),
                 "DONE 5 1 0 0 stop"});
    ReasoningCollector collector;
    (void)secure_frontend::drive_generation(
        run.protocol, decoder,
        {.max_tokens = 16U, .think_tokens = think, .starts_in_reasoning = true, .keeps_newlines_after_think = keep}, collector);
    return collector.text;
  };
  expect(reply(false) == "A", "the default drops them (upstream's lead rule)");
  expect(reply(true) == "\n\nA", "kept when asked: [" + reply(true) + "]");
}

// MiniMax-M3.1's reply as maya-mm3 writes it and Maya's server reads it: <mm:think> dropped, the reasoning, </mm:think>,
// the answer with its leading newlines, then [e~[ - a server stop: one STOP, the drain, a stop result.
void run_generation_reads_a_minimax_reply_like_mayas_server() {
  auto pack = test_support::byte_tokenizer();
  const auto add = [&pack](const std::string_view literal, const int type) {
    pack.tokens_by_id.emplace_back(literal);
    pack.token_types.push_back(type);
    return pack.tokens_by_id.size() - 1U;
  };
  (void)add("]~!b[", 3);
  (void)add("]~b]", 3);
  const auto turn_end = add("[e~[", 3);
  const auto open = add("<mm:think>", 4);
  const auto close = add("</mm:think>", 4);
  pack.mode = secure_frontend::TokenizerMode::minimax_bpe;
  secure_frontend::ChatCompletionRequest request;
  request.model = "m";
  request.messages = {{"user", "Hi", {}}};
  const auto planned = secure_frontend::make_minimax_chat_generation_plan(request, pack, 4096U, 64U, {}, {});
  const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&planned);
  expect(plan != nullptr, "a MiniMax plan");
  if (plan == nullptr) return;
  ReadyService f;
  f.transport.cancel_line = "DONE 6 3 1.0 1.0 cancel";
  f.transport.reads = {id_line(open), "T " + byte_id('r'), id_line(close), "T " + byte_id('\n'), "T " + byte_id('A'), id_line(turn_end)};
  const auto outcome = f.service.run_generation(
      {1U, 2U, 3U},
      {.max_tokens = 16U, .think_tokens = plan->think_tokens, .starts_in_reasoning = plan->starts_in_reasoning,
       .server_stop_tokens = plan->server_stop_tokens, .keeps_newlines_after_think = plan->keeps_newlines_after_think},
      {}, pack);
  const auto* result = std::get_if<secure_frontend::GenerationResult>(&outcome);
  expect(result != nullptr && result->reasoning == "r" && result->text == "\nA" && result->finish == secure_frontend::FinishReason::stop,
         "reasoning and answer apart, the answer's newline kept, [e~[ a stop: [" + (result ? result->reasoning + "|" + result->text : "") + "]");
  expect(std::count(f.transport.writes.begin(), f.transport.writes.end(), "STOP") == 1, "one STOP for the server stop");
}

void run_generation_ends_at_the_end_of_turn() {
  const auto tokenizer = think_tokenizer();
  const auto im_end = token_id_for(tokenizer, "<|im_end|>");
  ReadyService f;
  f.transport.reads = {"T " + byte_id('H'), "T " + byte_id('i'), id_line(im_end), "DONE 3 3 1.0 1.0 stop"};
  const auto outcome = f.service.run_generation({1U, 2U, 3U}, {.max_tokens = 8U, .stop_tokens = {im_end}}, {}, tokenizer);
  const auto* result = std::get_if<secure_frontend::GenerationResult>(&outcome);
  expect(result != nullptr && result->text == "Hi" && result->finish == secure_frontend::FinishReason::stop &&
             result->completion_tokens == 3U,
         "a reply ending in <|im_end|> is a stop result, not decode_failed");
}

void run_generation_returns_reasoning_beside_the_answer() {
  const auto tokenizer = think_tokenizer();
  const auto think = think_ids(tokenizer);
  ReadyService f;
  f.transport.reads = {"T " + byte_id('r'), id_line(think.close), "T " + byte_id('\n'), "T " + byte_id('\n'), "T " + byte_id('A'),
                       "DONE 5 3 1.0 1.0 stop"};
  const auto outcome = f.service.run_generation(
      {1U, 2U, 3U}, {.max_tokens = 8U, .think_tokens = think, .starts_in_reasoning = true}, {}, tokenizer);
  const auto* result = std::get_if<secure_frontend::GenerationResult>(&outcome);
  expect(result != nullptr, "a reply with a think block is a result, not decode_failed");
  if (result == nullptr) return;
  expect(result->text == "A" && result->reasoning == "r" && result->completion_tokens == 5U &&
             result->finish == secure_frontend::FinishReason::stop,
         "answer and reasoning come back apart: [" + result->text + "] [" + result->reasoning + "]");
}

// 2026-10-27 (a GLM-5.3-Flash card's chat, an internal_error after 8 s: its </think> reached the byte decoder): the
// glm53 plan on a pack typed as GLM-5.3-Flash's (the markers CONTROL, <think> and </think> USER_DEFINED) runs a reply
// the way Maya's engine writes it: reasoning, </think>, the answer, then <|user|> as a T record before DONE stop.
void run_generation_reads_a_glm_reply_like_mayas_server() {
  auto pack = test_support::byte_tokenizer();
  const auto add = [&pack](const std::string_view literal, const int type) {
    pack.tokens_by_id.emplace_back(literal);
    pack.token_types.push_back(type);
    return pack.tokens_by_id.size() - 1U;
  };
  for (const std::string_view marker : {"[gMASK]", "<sop>", "<|system|>", "<|assistant|>"}) (void)add(marker, 3);
  const auto user = add("<|user|>", 3);
  (void)add("<think>", 4);
  const auto think_close = add("</think>", 4);
  (void)add("<|endoftext|>", 3);
  (void)add("<|observation|>", 3);
  const auto planned = secure_frontend::make_glm53_chat_generation_plan({"glm-local", {{"user", "Hi"}}, 16U}, pack);
  const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&planned);
  expect(plan != nullptr, "the glm53 plan composes on a GLM-5.3-Flash pack");
  if (plan == nullptr) return;
  ReadyService f;
  f.transport.reads = {"T " + byte_id('r'), id_line(think_close), "T " + byte_id('\n'), "T " + byte_id('3'),
                       "T " + byte_id('9'), "T " + byte_id('1'), id_line(user), "DONE 7 3 1.0 1.0 stop"};
  const auto outcome = f.service.run_generation(
      {1U, 2U, 3U},
      {.max_tokens = 16U, .think_tokens = plan->think_tokens, .starts_in_reasoning = plan->starts_in_reasoning,
       .stop_tokens = plan->stop_tokens},
      {}, pack);
  const auto* result = std::get_if<secure_frontend::GenerationResult>(&outcome);
  expect(result != nullptr, "a GLM reply with a think block and a turn end is a result, not decode_failed");
  if (result == nullptr) return;
  expect(result->text == "391" && result->reasoning == "r" && result->finish == secure_frontend::FinishReason::stop &&
             result->completion_tokens == 7U,
         "the answer, its reasoning apart, ended as stop: [" + result->text + "] [" + result->reasoning + "]");
}

// DeepSeek-V4.1 (spec 2026-10-27-strata-prepare-design §7.2): the deepseek plan on a DeepSeek-shaped pack runs a reply the
// way Maya's maya-ds4 writes one - reasoning, </think>, the answer, then <｜User｜>, on which the engine does not stop:
// the frontend sends STOP, as Maya's server does, and the cancel DONE after it still ends the reply as stop.
void run_generation_reads_a_deepseek_reply_like_mayas_server() {
  auto pack = test_support::byte_tokenizer();
  const auto add = [&pack](const std::string_view literal, const int type) {
    pack.tokens_by_id.emplace_back(literal);
    pack.token_types.push_back(type);
    return pack.tokens_by_id.size() - 1U;
  };
  for (const std::size_t i : {0U, 1U, 3U, 6U}) (void)add(secure_frontend::kDeepseekMarkers[i], 3);
  const auto user = add(secure_frontend::kDeepseekMarkers[2], 3);
  (void)add("<think>", 4);
  const auto think_close = add("</think>", 4);
  pack.mode = secure_frontend::TokenizerMode::deepseek_bpe;
  const auto planned = secure_frontend::make_deepseek_chat_generation_plan({"deepseek-local", {{"user", "Hi"}}, 16U}, pack);
  const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&planned);
  expect(plan != nullptr, "the deepseek plan composes on a DeepSeek-shaped pack");
  if (plan == nullptr) return;
  ReadyService f;
  f.transport.cancel_line = "DONE 8 3 1.0 1.0 cancel";
  f.transport.reads = {"T " + byte_id('r'), id_line(think_close), "T " + byte_id('\n'), "T " + byte_id('3'),
                       "T " + byte_id('9'), "T " + byte_id('1'), id_line(user)};
  const auto outcome = f.service.run_generation(
      {1U, 2U, 3U},
      {.max_tokens = 16U, .think_tokens = plan->think_tokens, .starts_in_reasoning = plan->starts_in_reasoning,
       .stop_tokens = plan->stop_tokens, .server_stop_tokens = plan->server_stop_tokens},
      {}, pack);
  const auto* result = std::get_if<secure_frontend::GenerationResult>(&outcome);
  expect(result != nullptr, "a DeepSeek reply ending in <｜User｜> is a result");
  if (result == nullptr) return;
  expect(result->text == "391" && result->reasoning == "r" && result->finish == secure_frontend::FinishReason::stop,
         "the answer, its reasoning apart, ended as stop: [" + result->text + "] [" + result->reasoning + "]");
  expect(std::count(f.transport.writes.begin(), f.transport.writes.end(), "STOP") == 1, "one STOP went to the engine");
}

// Gemma 4 plan 1 Task 9 (spec §6.4, §6.5): the thought channel <|channel>thought\n…<channel|> and
// the end-of-turn ids <turn|> and <eos>, on the synthetic Gemma pack (test_support::spm_tokenizer)
// with the limits the gemma4 plan composes.
struct Gemma4Run final {
  secure_frontend::TokenizerPack pack = test_support::spm_tokenizer();
  secure_frontend::GenerationRunLimits limits{};
  std::size_t channel_open{0};
  std::size_t channel_close{0};
  std::size_t turn_close{0};
  std::size_t eos{0};
  Gemma4Run() {
    const auto result = secure_frontend::make_gemma4_chat_generation_plan({"gemma-local", {{"user", "Hi"}}, 16U}, pack, 4096U, 16U, {}, true);
    const auto* plan = std::get_if<secure_frontend::ChatGenerationPlan>(&result);
    expect(plan != nullptr, "the gemma4 plan composes on the Gemma pack");
    if (plan != nullptr) {
      limits.max_tokens = 256U;
      limits.think_tokens = plan->think_tokens;
      limits.starts_in_reasoning = plan->starts_in_reasoning;
      limits.stop_tokens = plan->stop_tokens;
      limits.channel_name = plan->channel_name;
    }
    channel_open = test_support::token_id(pack, "<|channel>");
    channel_close = test_support::token_id(pack, "<channel|>");
    turn_close = test_support::token_id(pack, "<turn|>");
    eos = test_support::token_id(pack, "<eos>");
  }
  // The T records of `text` in this pack (spm_bpe, so "thought" is one merged token).
  [[nodiscard]] std::vector<std::string> text(const std::string_view value) const {
    const auto encoded = secure_frontend::encode_text(pack, value);
    std::vector<std::string> records;
    if (const auto* ids = std::get_if<std::vector<std::size_t>>(&encoded)) {
      for (const std::size_t id : *ids) records.push_back(id_line(id));
    }
    expect(!records.empty() || value.empty(), "test text encodes in the Gemma pack");
    return records;
  }
};

std::vector<std::string> joined(std::initializer_list<std::vector<std::string>> parts) {
  std::vector<std::string> all;
  for (const auto& part : parts) all.insert(all.end(), part.begin(), part.end());
  return all;
}

void gemma4_thought_channel_goes_to_reasoning() {
  const Gemma4Run g;
  expect(g.limits.channel_name == std::optional<std::string>{"thought"}, "the gemma4 plan names its channel thought");
  expect(g.limits.think_tokens.has_value() && g.limits.think_tokens->open == g.channel_open && g.limits.think_tokens->close == g.channel_close &&
             !g.limits.starts_in_reasoning,
         "the split tags are <|channel> and <channel|>; the reply starts outside the channel");
  const secure_frontend::TokenByteDecoder decoder(g.pack);
  const auto records = joined({{id_line(g.channel_open)}, g.text("thought\n"), g.text("Think hard."), {id_line(g.channel_close)},
                               g.text("Paris."), {id_line(g.turn_close), "DONE 9 1 0 0 stop"}});
  Running run(records);
  ReasoningCollector collector;
  const auto result = secure_frontend::drive_generation(run.protocol, decoder, g.limits, collector);
  expect(result.end == secure_frontend::RunEnd::stop, "the turn close ends the reply as stop");
  expect(collector.reasoning == "Think hard.", "the channel-name line is dropped, the rest is reasoning: [" + collector.reasoning + "]");
  expect(collector.text == "Paris.", "the answer follows the channel: [" + collector.text + "]");
  expect(run.transport.stops() == 0U && run.protocol.idle(), "no STOP: the engine ended its own turn");

  // The name line may arrive with reasoning in the same token, or split over several tokens.
  const auto split = joined({{id_line(g.channel_open)}, g.text("tho"), g.text("ught"), g.text("\nr"), {id_line(g.channel_close)},
                             g.text("a"), {id_line(g.eos), "DONE 8 1 0 0 stop"}});
  Running pieces(split);
  ReasoningCollector pieced;
  const auto pieced_result = secure_frontend::drive_generation(pieces.protocol, decoder, g.limits, pieced);
  expect(pieced_result.end == secure_frontend::RunEnd::stop && pieced.reasoning == "r" && pieced.text == "a",
         "a name split over tokens is still the name line: [" + pieced.reasoning + "] [" + pieced.text + "]");

  // Thinking off: the prompt closed the channel, the reply is the answer from its first token.
  Running direct(joined({g.text("Paris."), {id_line(g.turn_close), "DONE 3 1 0 0 stop"}}));
  ReasoningCollector answered;
  (void)secure_frontend::drive_generation(direct.protocol, decoder, g.limits, answered);
  expect(answered.text == "Paris." && answered.reasoning.empty(), "an answer with no channel is all text");

  // Through run_generation (the complete path), the plan's channel name rides GenerationSettings.
  ReadyService f;
  for (const auto& record : records) f.transport.reads.push_back(record);
  const auto outcome = f.service.run_generation(
      {1U, 2U, 3U},
      {.max_tokens = 64U, .think_tokens = g.limits.think_tokens, .starts_in_reasoning = false, .stop_tokens = g.limits.stop_tokens,
       .channel_name = g.limits.channel_name},
      {}, g.pack);
  const auto* complete = std::get_if<secure_frontend::GenerationResult>(&outcome);
  expect(complete != nullptr && complete->reasoning == "Think hard." && complete->text == "Paris." &&
             complete->finish == secure_frontend::FinishReason::stop,
         "run_generation returns the reasoning and the answer apart");
}

void gemma4_wrong_channel_name_fails_closed() {
  const Gemma4Run g;
  const secure_frontend::TokenByteDecoder decoder(g.pack);
  const std::vector<std::vector<std::string>> broken{
      joined({{id_line(g.channel_open)}, g.text("analysis\nr"), {id_line(g.channel_close)}, g.text("a")}),  // another name
      joined({{id_line(g.channel_open)}, g.text("thoughts\nr"), {id_line(g.channel_close)}, g.text("a")}),  // a longer name
      joined({{id_line(g.channel_open)}, g.text("though\nr"), {id_line(g.channel_close)}, g.text("a")}),    // a shorter name
      joined({{id_line(g.channel_open)}, g.text("\nr"), {id_line(g.channel_close)}, g.text("a")}),          // no name
      joined({{id_line(g.channel_open)}, g.text(" thought\nr"), {id_line(g.channel_close)}, g.text("a")}),  // a leading space
      joined({{id_line(g.channel_open)}, g.text("thought"), {id_line(g.channel_close)}, g.text("a")}),      // closed before the newline
      joined({{id_line(g.channel_open)}, {id_line(g.channel_close)}, g.text("a")}),                         // an empty channel
      joined({{id_line(g.channel_open)}, {id_line(g.channel_open)}, g.text("thought\nr")}),                 // opened twice
      joined({{id_line(g.channel_open)}, g.text("thought"), {id_line(g.turn_close)}}),                      // the turn ends in the name
      joined({{id_line(g.channel_open)}, g.text(std::string(80U, 'b') + "\nr")}),                           // no line in 64 bytes
      joined({g.text("a"), {id_line(g.channel_open)}, g.text("analysis\nr"), {id_line(g.channel_close)}}),  // reopened while answering
  };
  for (const auto& tokens : broken) {
    auto records = tokens;
    records.push_back("DONE " + std::to_string(tokens.size()) + " 1 0 0 cancel");
    Running run(records);
    ReasoningCollector collector;
    const auto result = secure_frontend::drive_generation(run.protocol, decoder, g.limits, collector);
    expect(result.end == secure_frontend::RunEnd::undecodable, "a channel not named exactly thought ends the reply as undecodable");
    expect(run.transport.stops() == 1U && run.protocol.idle(), "one STOP and a drain to DONE");
    expect(collector.reasoning.empty() && collector.text.find("analysis") == std::string::npos && collector.text.find('r') == std::string::npos,
           "nothing of the channel is output: [" + collector.reasoning + "] [" + collector.text + "]");
  }
  // The engine ends its turn on its own inside the name line (DONE stop): still undecodable.
  Running ended(joined({{id_line(g.channel_open)}, g.text("tho"), {"DONE 2 1 0 0 stop"}}));
  ReasoningCollector cut;
  expect(secure_frontend::drive_generation(ended.protocol, decoder, g.limits, cut).end == secure_frontend::RunEnd::undecodable &&
             cut.reasoning.empty() && cut.text.empty(),
         "a reply may not stop inside the channel-name line");
  // A token budget ending inside the name line is a length end with nothing output.
  Running budget(joined({{id_line(g.channel_open)}, g.text("tho"), {"DONE 2 1 0 0 length"}}));
  ReasoningCollector capped;
  expect(secure_frontend::drive_generation(budget.protocol, decoder, g.limits, capped).end == secure_frontend::RunEnd::length &&
             capped.reasoning.empty() && capped.text.empty(),
         "a length end inside the name line outputs nothing of it");
  // Without a channel name (qwen, glm53) the open tag starts reasoning at once, as before.
  auto unnamed = g.limits;
  unnamed.channel_name.reset();
  Running plain(joined({{id_line(g.channel_open)}, g.text("analysis\nr"), {id_line(g.channel_close)}, g.text("a"), {"DONE 5 1 0 0 stop"}}));
  ReasoningCollector qwen_like;
  expect(secure_frontend::drive_generation(plain.protocol, decoder, unnamed, qwen_like).end == secure_frontend::RunEnd::stop &&
             qwen_like.reasoning == "analysis\nr" && qwen_like.text == "a",
         "channel_name unset: the split is the qwen split, unchanged");
}

void gemma4_turn_close_ends_the_reply_as_stop() {
  const Gemma4Run g;
  expect(secure_frontend::find_gemma4_end_of_turn_tokens(g.pack) == std::vector<std::size_t>{g.turn_close, g.eos},
         "the end-of-turn ids are <turn|> then <eos>, found by literal");
  expect(g.limits.stop_tokens == std::vector<std::size_t>{g.turn_close, g.eos}, "the gemma4 plan stops on exactly those");
  const secure_frontend::TokenByteDecoder decoder(g.pack);
  for (const std::size_t stop : {g.turn_close, g.eos}) {
    Running run(joined({g.text("Paris."), {id_line(stop)}, g.text("x"), {"DONE 4 1 0 0 stop"}}));
    ReasoningCollector collector;
    const auto result = secure_frontend::drive_generation(run.protocol, decoder, g.limits, collector);
    expect(result.end == secure_frontend::RunEnd::stop && collector.text == "Paris." && run.transport.stops() == 0U,
           "an end-of-turn id ends the reply as stop, is never output, and nothing after it is: [" + collector.text + "]");
  }
  Running thinking(joined({{id_line(g.channel_open)}, g.text("thought\nr"), {id_line(g.turn_close), "DONE 3 1 0 0 stop"}}));
  ReasoningCollector unanswered;
  expect(secure_frontend::drive_generation(thinking.protocol, decoder, g.limits, unanswered).end == secure_frontend::RunEnd::stop &&
             unanswered.reasoning == "r" && unanswered.text.empty(),
         "a turn may end inside the reasoning, after the name line");

  // Each literal counts only as exactly one CONTROL token.
  for (const std::string_view literal : {"<turn|>", "<eos>"}) {
    auto twice = g.pack;
    twice.tokens_by_id.emplace_back(literal);
    twice.token_types.push_back(3);
    auto user_defined = g.pack;
    user_defined.token_types[test_support::token_id(user_defined, literal)] = 4;
    for (const auto* pack : {&twice, &user_defined}) {
      const auto found = secure_frontend::find_gemma4_end_of_turn_tokens(*pack);
      expect(found.size() == 1U, "an ambiguous or mistyped end-of-turn literal is not a stop id: " + std::string{literal});
    }
  }
  // a later GGUF's <eos>, typed NORMAL (unsloth's Q8_0 at c099eb48, owner 2026-10-28), still ends the turn; <turn|> must
  // stay CONTROL
  auto later = g.pack;
  later.token_types[test_support::token_id(later, "<eos>")] = 1;
  expect(secure_frontend::find_gemma4_end_of_turn_tokens(later) == std::vector<std::size_t>{g.turn_close, g.eos},
         "a NORMAL <eos> is still a stop id");
  auto normal_close = g.pack;
  normal_close.token_types[test_support::token_id(normal_close, "<turn|>")] = 1;
  expect(secure_frontend::find_gemma4_end_of_turn_tokens(normal_close).size() == 1U, "a NORMAL <turn|> is not");
  auto byte_mode = g.pack;
  byte_mode.mode = secure_frontend::TokenizerMode::byte_bpe;
  expect(secure_frontend::find_gemma4_end_of_turn_tokens(byte_mode).empty(), "a byte_bpe pack has no Gemma end-of-turn ids");
}

void gemma4_tool_markers_in_output_fail_closed() {
  const Gemma4Run g;
  const secure_frontend::TokenByteDecoder decoder(g.pack);
  // The limits under test are the whole gemma4 set: its two stop ids and its channel name.
  expect(g.limits.stop_tokens == std::vector<std::size_t>{g.turn_close, g.eos} && g.limits.channel_name.has_value(),
         "the gemma4 plan's stop ids and channel name are in force");
  Running control(joined({{id_line(g.channel_open)}, g.text("thought\nr"), {id_line(g.turn_close)}, g.text("x"), {"DONE 4 1 0 0 stop"}}));
  ReasoningCollector controlled;
  expect(secure_frontend::drive_generation(control.protocol, decoder, g.limits, controlled).end == secure_frontend::RunEnd::stop &&
             controlled.reasoning == "r",
         "control: the same reply ended by <turn|> is a stop with the reasoning r");
  for (const std::string_view literal : {"<|tool_call>", "<tool_call|>", "<|tool_response>", "<tool_response|>", "<|tool>", "<|\"|>",
                                         "<|image|>", "<|image>", "<|audio|>", "<|video|>", "<|turn>", "<|think|>", "<bos>", "<pad>"}) {
    const auto id = test_support::token_id(g.pack, literal);
    expect(std::find(g.limits.stop_tokens.begin(), g.limits.stop_tokens.end(), id) == g.limits.stop_tokens.end(),
           "a marker is never a stop id: " + std::string{literal});
    for (const bool in_reasoning : {false, true}) {
      const auto records = in_reasoning ? joined({{id_line(g.channel_open)}, g.text("thought\nr"), {id_line(id)}, g.text("x"), {"DONE 4 1 0 0 cancel"}})
                                        : joined({g.text("a"), {id_line(id)}, g.text("x"), {"DONE 3 1 0 0 cancel"}});
      Running run(records);
      ReasoningCollector collector;
      const auto result = secure_frontend::drive_generation(run.protocol, decoder, g.limits, collector);
      expect(result.end == secure_frontend::RunEnd::undecodable && run.transport.stops() == 1U,
             "a tool, media or framing marker in the output fails closed: " + std::string{literal});
      expect(collector.text.find('x') == std::string::npos && collector.reasoning.find('x') == std::string::npos,
             "nothing after the marker is output");
      expect(in_reasoning ? collector.reasoning == "r" && collector.text.empty() : collector.text == "a" && collector.reasoning.empty(),
             "only what came before the marker is output, the name line dropped: [" + collector.reasoning + "] [" + collector.text + "]");
    }
  }
}

// Session log step 16 (listener.monitor): the meter hears every counted token and the
// engine's DONE figures, on both paths, and never any text; without one nothing changes.
struct CountingMeter final : secure_frontend::GenerationMeter {
  std::vector<std::size_t> tokens;
  std::optional<secure_frontend::DoneFigures> done;
  void on_token(const std::size_t generated) override { tokens.push_back(generated); }
  void on_done(const secure_frontend::DoneFigures& figures) override { done = figures; }
};

void drive_generation_feeds_the_monitor_meter() {
  const auto pack = test_support::byte_tokenizer();
  const secure_frontend::TokenByteDecoder decoder(pack);
  Running run({"T " + byte_id('A'), "T " + byte_id('B'), "T " + byte_id('C'), "DONE 3 9 120.5 60 stop 0 0 4"});
  Collector collector;
  CountingMeter meter;
  const auto result = secure_frontend::drive_generation(run.protocol, decoder, {.max_tokens = 16U, .meter = &meter}, collector);
  expect(result.end == secure_frontend::RunEnd::stop && collector.text == "ABC", "the run itself is unchanged");
  expect(meter.tokens == std::vector<std::size_t>({1U, 2U, 3U}), "one call per counted token");
  expect(meter.done.has_value() && meter.done->prompt_tokens == std::optional<std::size_t>{9U} &&
             meter.done->prompt_ms == std::optional<double>{120.5} && meter.done->decode_ms == std::optional<double>{60.0} &&
             meter.done->reused == std::optional<std::size_t>{4U},
         "the DONE figures reach the meter");

  ReadyService f;
  f.transport.reads = {"T " + byte_id('H'), "T " + byte_id('i'), "DONE 2 3 5.0 8.0 stop"};
  CountingMeter route_meter;
  const auto outcome =
      f.service.run_generation({1U, 2U, 3U}, {.max_tokens = 8U, .meter = &route_meter}, {}, test_support::byte_tokenizer());
  expect(std::holds_alternative<secure_frontend::GenerationResult>(outcome) &&
             route_meter.tokens == std::vector<std::size_t>({1U, 2U}) && route_meter.done.has_value() &&
             route_meter.done->decode_ms == std::optional<double>{8.0},
         "run_generation hands GenerationSettings::meter to the loop");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"executes_a_bounded_non_streaming_completion", executes_a_bounded_non_streaming_completion},
      {"rejects_context_and_output_limits_before_generation", rejects_context_and_output_limits_before_generation},
      {"fails_closed_on_invalid_engine_output", fails_closed_on_invalid_engine_output},
      {"executes_glm53_only_when_explicitly_selected", executes_glm53_only_when_explicitly_selected},
      {"executes_gemma4_only_when_explicitly_selected", executes_gemma4_only_when_explicitly_selected},
      {"glm_profile_generation_completes_over_maya_dialect", glm_profile_generation_completes_over_maya_dialect},
      {"maya_dialect_cancellation_round_trips", maya_dialect_cancellation_round_trips},
      {"chat_recovers_after_generation_timeout", chat_recovers_after_generation_timeout},
      {"chat_gen_write_bounded_by_generation_deadline", chat_gen_write_bounded_by_generation_deadline},
      {"chat_marks_engine_failed_when_drain_fails", chat_marks_engine_failed_when_drain_fails},
      {"chat_rejects_nonfinite_done_and_stays_ready", chat_rejects_nonfinite_done_and_stays_ready},
      {"health_reflects_protocol_state", health_reflects_protocol_state},
      {"health_is_failed_after_engine_closed_mid_generation", health_is_failed_after_engine_closed_mid_generation},
      {"health_is_failed_when_engine_closes_before_ready", health_is_failed_when_engine_closes_before_ready},
      {"chat_applies_engine_context_rule_with_eight_token_margin", chat_applies_engine_context_rule_with_eight_token_margin},
      {"chat_uses_adopted_ready_without_waiting", chat_uses_adopted_ready_without_waiting},
      {"resolve_max_tokens_follows_decisions_1_and_2", resolve_max_tokens_follows_decisions_1_and_2},
      {"resolve_max_tokens_fit_false_refuses_like_the_pin", resolve_max_tokens_fit_false_refuses_like_the_pin},
      {"min_and_max_tokens_bounds_clamp_at_resolve", min_and_max_tokens_bounds_clamp_at_resolve},
      {"engine_silence_s_sets_the_stall_watchdog", engine_silence_s_sets_the_stall_watchdog},
      {"repeat_stop_tokens_ends_a_runaway_reply", repeat_stop_tokens_ends_a_runaway_reply},
      {"run_generation_reports_finish_usage_and_context", run_generation_reports_finish_usage_and_context},
      {"run_generation_cancel_sends_stop_and_drains", run_generation_cancel_sends_stop_and_drains},
      {"run_generation_timeout_stops_and_reports_timed_out", run_generation_timeout_stops_and_reports_timed_out},
      {"run_generation_drain_failure_abandons", run_generation_drain_failure_abandons},
      {"run_generation_shutdown_uses_short_drain", run_generation_shutdown_uses_short_drain},
      {"drive_generation_bytes_cap_cuts_on_code_point", drive_generation_bytes_cap_cuts_on_code_point},
      {"drive_generation_drops_tail_on_length_and_replaces_on_stop", drive_generation_drops_tail_on_length_and_replaces_on_stop},
      {"drive_generation_cancel_sends_stop_and_drains", drive_generation_cancel_sends_stop_and_drains},
      {"chat_max_tokens_cut_mid_character_returns_length", chat_max_tokens_cut_mid_character_returns_length},
      {"drive_generation_splits_reasoning_at_the_think_close", drive_generation_splits_reasoning_at_the_think_close},
      {"drive_generation_think_open_while_answering_starts_reasoning", drive_generation_think_open_while_answering_starts_reasoning},
      {"drive_generation_caps_reasoning_and_answer_together", drive_generation_caps_reasoning_and_answer_together},
      {"drive_generation_ends_a_split_character_at_the_tag", drive_generation_ends_a_split_character_at_the_tag},
      {"drive_generation_keeps_other_special_tokens_fail_closed", drive_generation_keeps_other_special_tokens_fail_closed},
      {"drive_generation_outputs_literal_tokens_as_their_text", drive_generation_outputs_literal_tokens_as_their_text},
      {"run_generation_returns_reasoning_beside_the_answer", run_generation_returns_reasoning_beside_the_answer},
      {"run_generation_reads_a_glm_reply_like_mayas_server", run_generation_reads_a_glm_reply_like_mayas_server},
      {"run_generation_reads_a_deepseek_reply_like_mayas_server", run_generation_reads_a_deepseek_reply_like_mayas_server},
      {"drive_generation_end_of_turn_ends_the_reply_as_stop", drive_generation_end_of_turn_ends_the_reply_as_stop},
      {"drive_generation_server_stop_token_sends_stop_and_drains", drive_generation_server_stop_token_sends_stop_and_drains},
      {"drive_generation_keeps_the_newlines_after_think_when_asked", drive_generation_keeps_the_newlines_after_think_when_asked},
      {"run_generation_reads_a_minimax_reply_like_mayas_server", run_generation_reads_a_minimax_reply_like_mayas_server},
      {"drive_generation_feeds_the_monitor_meter", drive_generation_feeds_the_monitor_meter},
      {"run_generation_ends_at_the_end_of_turn", run_generation_ends_at_the_end_of_turn},
      {"run_generation_maps_engine_errors", run_generation_maps_engine_errors},
      {"gemma4_thought_channel_goes_to_reasoning", gemma4_thought_channel_goes_to_reasoning},
      {"gemma4_wrong_channel_name_fails_closed", gemma4_wrong_channel_name_fails_closed},
      {"gemma4_turn_close_ends_the_reply_as_stop", gemma4_turn_close_ends_the_reply_as_stop},
      {"gemma4_tool_markers_in_output_fail_closed", gemma4_tool_markers_in_output_fail_closed},
  }, failures);
}
