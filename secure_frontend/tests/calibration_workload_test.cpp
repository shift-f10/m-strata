#include "secure_frontend/calibration_workload.hpp"
#include "secure_frontend/json.hpp"
#include "support/test_support.hpp"
#include <iostream>

namespace {
using namespace secure_frontend;
int failures = 0;
void expect(bool ok, const char* message) { if (!ok) { ++failures; std::cerr << message << '\n'; } }
std::string quoted(std::string_view value) { std::string out; append_json_string(&out, value); return out; }
std::string reply(std::string_view answer) {
  return "{\"choices\":[{\"index\":0,\"finish_reason\":\"stop\",\"message\":{\"content\":" + quoted(answer) + "}}]}";
}
const std::string expected_tools = R"([{"name":"record_value","arguments":{"key":"alpha","value":7}},{"name":"record_value","arguments":{"key":"beta","value":11}}])";
CalibrationCase tools_case(bool stream) { CalibrationCase out; out.acceptance = CalibrationAcceptance::tool_calls; out.expected = expected_tools; out.stream = stream; return out; }
std::string tool_reply(std::string_view second = R"({"key":"beta","value":11})") {
  return "{\"choices\":[{\"index\":0,\"finish_reason\":\"tool_calls\",\"message\":{\"tool_calls\":["
      "{\"id\":\"call_a\",\"type\":\"function\",\"function\":{\"name\":\"record_value\",\"arguments\":" + quoted(R"({"key":"alpha","value":7})") + "}},"
      "{\"id\":\"call_b\",\"type\":\"function\",\"function\":{\"name\":\"record_value\",\"arguments\":" + quoted(second) + "}}]}}]}";
}
std::string event(std::string_view delta, std::string_view finish = "null") {
  return "data: {\"choices\":[{\"index\":0,\"delta\":" + std::string(delta) + ",\"finish_reason\":" + std::string(finish) + "}]}\n\n";
}
void fixed_workloads_have_known_answers_and_real_token_counts() {
  CalibrationWorkloadRequest input; input.model = "fixture";
  const CalibrationTokenCounter counter = [](std::string_view body) -> std::optional<std::size_t> { return body.size() / 4U + 1U; };
  for (const auto kind : {BenchmarkWorkload::chat, BenchmarkWorkload::coding, BenchmarkWorkload::long_context, BenchmarkWorkload::tools}) {
    input.workload = kind;
    const auto result = build_calibration_workload(input, counter);
    const auto* cases = std::get_if<std::vector<CalibrationCase>>(&result);
    expect(cases && !cases->empty(), "each fixed workload builds actual requests");
    if (!cases) continue;
    bool performance = false, known_answer = false;
    for (const auto& test : *cases) {
      if (test.acceptance == CalibrationAcceptance::performance_text) performance = performance || test.measured;
      else {
        known_answer = true; expect(!test.expected.empty(), "fixed quality uses known answers");
        expect(kind == BenchmarkWorkload::tools || !test.measured, "tiny quality answers do not bias throughput measurements");
      }
      expect(test.prompt_tokens == *counter(test.request_body), "counter sees final encoded body");
      expect(test.context_probe == (kind == BenchmarkWorkload::long_context), "only context-fill cases are probes despite every case having a target token count");
      expect(test.request_body.find("reasoning_effort") == std::string::npos, "profile-incompatible reasoning override is absent");
      expect(test.request_body.find("\"strata_search\":false") != std::string::npos, "benchmark requests cannot invoke server web search");
    }
    if (kind == BenchmarkWorkload::tools) expect(cases->size() == 2U && !cases->front().stream && cases->back().stream, "tools cover both modes");
    else expect(performance && known_answer, "standard workloads separate representative performance from known-answer quality");
    input.disable_thinking = true;
    const auto direct = build_calibration_workload(input, counter);
    const auto* direct_cases = std::get_if<std::vector<CalibrationCase>>(&direct);
    expect(direct_cases != nullptr, "thinking-controllable profiles build workloads");
    if (direct_cases) for (const auto& test : *direct_cases)
      expect(test.request_body.find("\"reasoning_effort\":\"none\"") != std::string::npos,
             "benchmark answer budget cannot be consumed by optional thinking");
    input.disable_thinking = false;
  }
}
void memory_probe_reports_partial_coverage_and_honors_limits() {
  CalibrationWorkloadRequest input; input.model = "fixture"; input.context_tokens = 8192; input.max_prompt_tokens = 1000;
  const auto result = build_calibration_memory_probe(input, [](std::string_view body) -> std::optional<std::size_t> { return body.size() / 3U; });
  const auto* test = std::get_if<CalibrationCase>(&result);
  expect(test && test->prompt_tokens <= 1000U && test->prompt_tokens >= 990U, "probe fills actual admitted tokens");
  expect(test && !test->full_context_probe && test->context_coverage < 0.2, "truncated context never claims full coverage");
  expect(test && test->context_probe && test->target_prompt_tokens != 0U, "capped context probes remain identifiable for controller coverage rejection");
  expect(test && !test->measured, "memory retrieval is a quality probe rather than a tiny decode benchmark");
  input.max_prompt_tokens = input.context_tokens;
  const auto full = build_calibration_memory_probe(input, [](std::string_view body) -> std::optional<std::size_t> { return body.size() / 3U; });
  const auto* all = std::get_if<CalibrationCase>(&full);
  expect(all && all->full_context_probe && all->context_coverage > 0.99, "selected context genuinely filled");
  expect(std::holds_alternative<std::string>(build_calibration_memory_probe(input, [](std::string_view) -> std::optional<std::size_t> { return std::nullopt; })), "unrenderable base fails closed");
}
void quality_rejects_wrong_answers_truncation_and_incomplete_tool_arguments() {
  CalibrationCase text; text.expected = "59";
  expect(validate_calibration_response(text, 200, reply("59")).accepted, "correct text accepted");
  expect(!validate_calibration_response(text, 200, reply("58")).accepted, "wrong answer rejected");
  expect(!validate_calibration_response(text, 500, reply("59")).accepted, "failure status rejected");
  text.acceptance = CalibrationAcceptance::json_value; text.expected = R"({"result":[2,0,3]})";
  expect(validate_calibration_response(text, 200, reply(text.expected)).accepted, "known coding output accepted without executing code");
  expect(!validate_calibration_response(text, 200, reply(R"({"result":[2,0,4]})")).accepted, "incorrect code trace rejected");
  expect(validate_calibration_response(tools_case(false), 200, tool_reply()).accepted, "two complete matching calls accepted");
  expect(!validate_calibration_response(tools_case(false), 200, tool_reply("{\"key\":")).accepted, "truncated arguments rejected");
  expect(!validate_calibration_response(tools_case(false), 200, tool_reply(R"({"key":"beta","value":12})")).accepted, "wrong tool values rejected");
}
void streamed_tools_require_two_complete_calls_and_terminal_done() {
  std::string stream = event(R"({"tool_calls":[{"index":0,"id":"a","type":"function","function":{"name":"record_value","arguments":"{\"key\":\"alpha\","}}]})");
  stream += event(R"({"tool_calls":[{"index":1,"id":"b","type":"function","function":{"name":"record_value","arguments":"{\"key\":\"beta\",\"value\":11}"}},{"index":0,"function":{"arguments":"\"value\":7}"}}]})");
  stream += event("{}", "\"tool_calls\"");
  const auto test = tools_case(true);
  expect(!validate_calibration_response(test, 200, stream).accepted, "missing DONE rejected");
  stream += "data: [DONE]\n\n";
  expect(validate_calibration_response(test, 200, stream).accepted, "interleaved tool fragments assemble exactly");
  auto missing_type = stream;
  const auto type_pos = missing_type.find("\"type\":\"function\",");
  missing_type.erase(type_pos, std::string_view("\"type\":\"function\",").size());
  expect(!validate_calibration_response(test, 200, missing_type).accepted, "each streamed tool must declare its function type");
  expect(!validate_calibration_response(test, 200, stream + event("{}")).accepted, "data after DONE rejected");
  expect(!validate_calibration_response(test, 200, "data: [DONE]\n\n").accepted, "DONE alone is not completion");
}
void captures_are_bounded_and_custom_quality_is_not_claimed() {
  CalibrationCapture capture;
  expect(capture.send_headers({200, {}}) && capture.send_event("data: [DONE]\n\n"), "capture accepts stream");
  capture.finish(); expect(capture.complete() && capture.status() == 200, "capture tracks terminal finish");
  CalibrationCapture large; expect(large.send_headers({200, {}}), "large headers");
  expect(!large.send_event(std::string(kCalibrationResponseMaxBytes + 1, 'x')) && !large.complete(), "oversized capture fails closed");
  CalibrationCapture cancel([] { return true; }); expect(cancel.client_gone(), "controller cancellation propagates");
  CalibrationCase custom; custom.acceptance = CalibrationAcceptance::structural_only;
  const auto checked = validate_calibration_response(custom, 200, reply("anything"));
  expect(checked.accepted && !checked.semantic_check, "custom output has no correctness claim");
  auto capped = reply("A representative explanation continues until the explicit token budget.");
  capped.replace(capped.find("\"stop\""), 6, "\"length\"");
  custom.acceptance = CalibrationAcceptance::performance_text;
  const auto performance = validate_calibration_response(custom, 200, capped);
  expect(performance.accepted && !performance.semantic_check, "token-capped performance prose has no semantic claim");
  custom.acceptance = CalibrationAcceptance::exact_text;
  expect(!validate_calibration_response(custom, 200, capped).accepted, "token-capped quality answers still fail");
}
void host_diagnostics_distinguish_busy_baseline_and_thermal_evidence() {
  const auto now = std::chrono::steady_clock::now();
  MonitorHostSnapshot before{{}, now}, after{{}, now};
  before.sample.cpu_percent = 80.0;
  after.sample.memory = MemoryReading{99, 100};
  after.sample.gpu = GpuReading{"fixture", 99.0, 50U, 100U, 95.0};
  const auto result = calibration_host_diagnostics(before, after, now);
  expect(result.contaminated && !result.flags.empty(), "pretrial CPU activity and RAM pressure flagged");
  before.sample.cpu_percent = 0.0; after.sample.memory = MemoryReading{50, 100};
  expect(!calibration_host_diagnostics(before, after, now).contaminated, "busy measured GPU and high temperature alone do not prove competing activity or throttling");
  after.sample.gpu->mem_used_bytes = 99U;
  expect(!calibration_host_diagnostics(before, after, now).contaminated, "model-owned VRAM allocation alone is not competing activity");
  before.sample.gpu = after.sample.gpu;
  expect(calibration_host_diagnostics(before, after, now).contaminated, "pre-existing GPU memory pressure remains contamination");
  before.sample.gpu->util_percent = 0.0;
  expect(!calibration_host_diagnostics(before, after, now, std::nullopt, true).contaminated,
         "a reused benchmark's own loaded model is not external memory pressure");
  before.sample.gpu.reset();
  before.sample.cpu_percent = 80.0;
  after.sampled_at = now + std::chrono::minutes(2);
  expect(calibration_host_diagnostics(before, after, after.sampled_at, now).contaminated,
         "long trials retain pretrial interference evidence measured at trial start");
  before.sampled_at = now - std::chrono::seconds(6);
  expect(!calibration_host_diagnostics(before, after, after.sampled_at, now).contaminated,
         "already stale baseline is not mistaken for current interference");
  auto clock = now;
  std::size_t reads = 0, waits = 0;
  const auto baseline = await_calibration_baseline([&]() -> std::optional<MonitorHostSnapshot> {
    MonitorHostSnapshot sample{{}, clock};
    sample.sample.cpu_percent = reads < 3U ? 99.0 : 1.0;
    sample.sample.gpu = GpuReading{"cached", 100.0, 99U, 100U, 95.0};
    if (reads == 0U) sample.sampled_at = now - std::chrono::seconds(1);
    if (reads == 2U) sample.sampled_at = now + std::chrono::milliseconds(100);
    ++reads;
    return sample;
  }, {}, [&] { return clock; }, [&](std::chrono::milliseconds duration) { ++waits; clock += duration; });
  expect(baseline && baseline->sample.cpu_percent == 1.0 && !baseline->sample.gpu,
         "baseline waits for two distinct post-stop samples and omits unverifiable cached GPU data");
  expect(reads == 4U && waits == 3U, "repeated host snapshot does not count as a refresh");
  waits = 0;
  expect(!await_calibration_baseline([&] { return std::optional<MonitorHostSnapshot>{MonitorHostSnapshot{{}, now}}; }, {}, [&] { return clock; },
      [&](std::chrono::milliseconds duration) { ++waits; clock += duration; }) && waits == 30U,
      "stalled telemetry wait is bounded to three seconds");
  waits = 0;
  expect(!await_calibration_baseline([] { return std::optional<MonitorHostSnapshot>{}; }, {}, [&] { return clock; },
      [&](std::chrono::milliseconds) { ++waits; }) && waits == 0U,
      "absent telemetry returns immediately");
  waits = 0;
  expect(!await_calibration_baseline([] { return std::optional<MonitorHostSnapshot>{}; }, [] { return true; },
      [&] { return clock; }, [&](std::chrono::milliseconds) { ++waits; }) && waits == 0U,
      "cancellation interrupts baseline settling immediately");
}
} // namespace
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"fixed_workloads_have_known_answers_and_real_token_counts", fixed_workloads_have_known_answers_and_real_token_counts},
      {"memory_probe_reports_partial_coverage_and_honors_limits", memory_probe_reports_partial_coverage_and_honors_limits},
      {"quality_rejects_wrong_answers_truncation_and_incomplete_tool_arguments", quality_rejects_wrong_answers_truncation_and_incomplete_tool_arguments},
      {"streamed_tools_require_two_complete_calls_and_terminal_done", streamed_tools_require_two_complete_calls_and_terminal_done},
      {"captures_are_bounded_and_custom_quality_is_not_claimed", captures_are_bounded_and_custom_quality_is_not_claimed},
      {"host_diagnostics_distinguish_busy_baseline_and_thermal_evidence", host_diagnostics_distinguish_busy_baseline_and_thermal_evidence}}, failures);
}
