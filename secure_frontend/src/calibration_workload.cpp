#include "secure_frontend/calibration_workload.hpp"
#include "secure_frontend/json.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <thread>

namespace secure_frontend {
namespace {
constexpr JsonLimits kResponseLimits{kCalibrationResponseMaxBytes, 32U, 8192U, kCalibrationResponseMaxBytes, true};
constexpr std::string_view kToolExpected = R"([{"name":"record_value","arguments":{"key":"alpha","value":7}},{"name":"record_value","arguments":{"key":"beta","value":11}}])";
constexpr std::string_view kTools = R"([{"type":"function","function":{"name":"record_value","description":"Record the supplied key and integer value. The benchmark never executes this tool.","parameters":{"type":"object","properties":{"key":{"type":"string"},"value":{"type":"integer"}},"required":["key","value"],"additionalProperties":false}}}])";
std::string quote(std::string_view s) { std::string out; append_json_string(&out, s); return out; }
const JsonValue* field(const JsonValue* v, std::string_view key) {
  if (v && v->if_object()) for (const auto& item : *v->if_object()) if (item.key == key) return &item.value;
  return nullptr;
}
const std::string* text(const JsonValue* v) { return v ? v->if_string() : nullptr; }
std::optional<JsonValue> json(std::string_view s) {
  auto parsed = parse_json(s, kResponseLimits);
  if (auto* v = std::get_if<JsonValue>(&parsed)) return std::move(*v);
  return std::nullopt;
}
bool equal(const JsonValue& a, const JsonValue& b) {
  if (a.kind() != b.kind()) return false;
  switch (a.kind()) {
    case JsonKind::null: return true;
    case JsonKind::boolean: return *a.if_boolean() == *b.if_boolean();
    case JsonKind::integer: return *a.if_integer() == *b.if_integer();
    case JsonKind::decimal: return *a.if_decimal() == *b.if_decimal();
    case JsonKind::string: return *a.if_string() == *b.if_string();
    case JsonKind::array:
      if (a.if_array()->size() != b.if_array()->size()) return false;
      for (std::size_t i = 0; i < a.if_array()->size(); ++i) if (!equal((*a.if_array())[i], (*b.if_array())[i])) return false;
      return true;
    case JsonKind::object:
      if (a.if_object()->size() != b.if_object()->size()) return false;
      for (const auto& item : *a.if_object()) {
        const auto* other = field(&b, item.key);
        if (!other || !equal(item.value, *other)) return false;
      }
      return true;
  }
  return false;
}
std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}
std::optional<JsonValue> known_json_answer(std::string_view s) {
  s = trim(s);
  if (s.starts_with("```")) {
    const auto line = s.find('\n');
    if (line == std::string_view::npos) return std::nullopt;
    const auto opener = trim(s.substr(0, line));
    const auto closing = s.rfind('\n');
    if ((opener != "```json" && opener != "```") || closing == line || trim(s.substr(closing + 1U)) != "```") return std::nullopt;
    s = trim(s.substr(line + 1U, closing - line - 1U));
  }
  return json(s);
}
bool valid(const CalibrationWorkloadRequest& r, const CalibrationTokenCounter& count) {
  return count && !r.model.empty() && r.model.size() <= 256U && is_valid_utf8(r.model) &&
      r.max_completion_tokens > 0U && r.max_completion_tokens <= 4096U &&
      r.context_tokens > r.max_completion_tokens && r.max_prompt_tokens > 0U;
}
std::string body(const CalibrationWorkloadRequest& r, std::string_view prompt, bool stream, bool tools = false) {
  return "{\"model\":" + quote(r.model) + ",\"messages\":[{\"role\":\"user\",\"content\":" + quote(prompt) +
      "}],\"strata_search\":false,\"temperature\":0,\"max_tokens\":" + std::to_string(r.max_completion_tokens) + ",\"stream\":" +
      (stream ? "true" : "false") + (r.disable_thinking ? ",\"reasoning_effort\":\"none\"" : "") + (tools ? ",\"tools\":" + std::string(kTools) +
      ",\"tool_choice\":\"required\",\"parallel_tool_calls\":true" : "") + "}";
}
bool measure(CalibrationCase& test, const CalibrationWorkloadRequest& r, const CalibrationTokenCounter& count) {
  const auto n = count(test.request_body);
  const auto target = r.context_tokens - r.max_completion_tokens;
  if (!n || *n == 0U || *n > std::min(target, r.max_prompt_tokens)) return false;
  test.prompt_tokens = *n; test.target_prompt_tokens = target;
  test.context_coverage = static_cast<double>(*n) / static_cast<double>(target);
  return true;
}
struct Call { std::string id, name, arguments; bool function_type{false}; };
struct Answer { std::string content, finish; std::map<std::size_t, Call> calls; };
bool append(std::string& to, const JsonValue* v) {
  if (!v || v->kind() == JsonKind::null) return true;
  const auto* part = v->if_string();
  if (!part || part->size() > kCalibrationResponseMaxBytes - to.size()) return false;
  to += *part; return true;
}
bool read_calls(Answer& out, const JsonValue* v, bool stream) {
  if (!v) return true;
  const auto* calls = v->if_array();
  if (!calls || calls->size() > 16U) return false;
  for (std::size_t i = 0; i < calls->size(); ++i) {
    const auto& item = (*calls)[i];
    std::size_t index = i;
    if (stream) {
      const auto* n = field(&item, "index");
      if (!n || !n->if_integer() || *n->if_integer() < 0 || *n->if_integer() >= 16) return false;
      index = static_cast<std::size_t>(*n->if_integer());
    }
    const auto* type = field(&item, "type");
    if ((!stream && !type) || (type && (!text(type) || *text(type) != "function"))) return false;
    const auto* f = field(&item, "function");
    if (!f || !f->if_object()) return false;
    auto& call = out.calls[index];
    call.function_type = call.function_type || type != nullptr;
    if (!append(call.id, field(&item, "id")) || !append(call.name, field(f, "name")) || !append(call.arguments, field(f, "arguments"))) return false;
  }
  return true;
}
bool consume(Answer& out, const JsonValue& root, bool stream, bool& finished) {
  if (!root.if_object() || field(&root, "error")) return false;
  const auto* list = field(&root, "choices");
  if (!list || !list->if_array()) return false;
  if (stream && list->if_array()->empty()) return field(&root, "usage") != nullptr;
  if (list->if_array()->size() != 1U || finished) return false;
  const auto& choice = list->if_array()->front();
  const auto* index = field(&choice, "index");
  if (!index || !index->if_integer() || *index->if_integer() != 0) return false;
  const auto* message = field(&choice, stream ? "delta" : "message");
  if (!message || !message->if_object()) return false;
  const auto* role = field(message, "role");
  if (role && (!text(role) || *text(role) != "assistant")) return false;
  if (!append(out.content, field(message, "content")) || !read_calls(out, field(message, "tool_calls"), stream)) return false;
  const auto* finish = field(&choice, "finish_reason");
  if (!finish) return false;
  if (finish->kind() != JsonKind::null) {
    if (!text(finish)) return false;
    out.finish = *text(finish); finished = true;
  }
  return true;
}
std::optional<Answer> read_answer(std::string_view wire, bool stream) {
  if (wire.size() > kCalibrationResponseMaxBytes) return std::nullopt;
  Answer out;
  bool finished = false;
  if (!stream) {
    const auto parsed = json(wire);
    if (!parsed || !consume(out, *parsed, false, finished) || !finished) return std::nullopt;
    return out;
  }
  bool done = false;
  std::string data;
  std::size_t offset = 0, events = 0;
  while (offset < wire.size()) {
    const auto end = wire.find('\n', offset);
    if (end == std::string_view::npos) return std::nullopt;
    auto line = wire.substr(offset, end - offset); offset = end + 1U;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) {
      if (data.empty()) continue;
      if (++events > 8192U || done) return std::nullopt;
      if (data == "[DONE]") { if (!finished) return std::nullopt; done = true; }
      else { const auto parsed = json(data); if (!parsed || !consume(out, *parsed, true, finished)) return std::nullopt; }
      data.clear();
    } else if (line.starts_with("data:")) {
      line.remove_prefix(5U);
      if (!line.empty() && line.front() == ' ') line.remove_prefix(1U);
      if (!data.empty()) data += '\n';
      data += line;
    } else if (!line.starts_with(':') && !line.starts_with("event:")) return std::nullopt;
  }
  if (!done || !data.empty()) return std::nullopt;
  return out;
}
} // namespace

static std::variant<CalibrationCase, std::string> build_probe(const CalibrationWorkloadRequest& r,
    const CalibrationTokenCounter& count, bool performance);

CalibrationCases build_calibration_workload(const CalibrationWorkloadRequest& r, const CalibrationTokenCounter& count) {
  if (!valid(r, count)) return std::string{"invalid calibration workload limits"};
  if (r.workload == BenchmarkWorkload::long_context) {
    auto probe = build_calibration_memory_probe(r, count);
    if (auto* failure = std::get_if<std::string>(&probe)) return *failure;
    auto performance_request = r;
    performance_request.max_completion_tokens = std::max(r.max_completion_tokens, std::size_t{128});
    auto performance = build_probe(performance_request, count, true);
    if (auto* failure = std::get_if<std::string>(&performance)) return *failure;
    return std::vector<CalibrationCase>{std::get<CalibrationCase>(std::move(probe)), std::get<CalibrationCase>(std::move(performance))};
  }
  std::vector<CalibrationCase> tests;
  const auto add = [&](std::string id, std::string_view prompt, CalibrationAcceptance acceptance, std::string expected,
                       bool stream = false, bool tools = false) {
    CalibrationCase test;
    auto request = r;
    if (acceptance == CalibrationAcceptance::performance_text)
      request.max_completion_tokens = std::max(r.max_completion_tokens, std::size_t{128});
    test.id = std::move(id); test.request_body = body(request, prompt, stream, tools);
    test.stream = stream; test.acceptance = acceptance; test.expected = std::move(expected);
    test.measured = acceptance == CalibrationAcceptance::performance_text || acceptance == CalibrationAcceptance::tool_calls;
    tests.push_back(std::move(test));
  };
  switch (r.workload) {
    case BenchmarkWorkload::chat:
      add("chat-explanation", "Write a detailed practical explanation of how to organize a community library, covering cataloging, lending, accessibility, staffing and maintenance. Use at least 400 words. Do not use tools.", CalibrationAcceptance::performance_text, "");
      add("chat-process", "Explain in two paragraphs how a refrigerator moves heat from inside to outside. Do not use tools.", CalibrationAcceptance::performance_text, "");
      add("chat-list", "List twelve common kitchen tools with a sentence about a practical use for each. Do not use tools.", CalibrationAcceptance::performance_text, "");
      add("chat-arithmetic", "Calculate (17 * 3) + 8. Reply only with the integer answer, without explanations.", CalibrationAcceptance::exact_text, "59");
      add("chat-facts", "Facts: The parcel is blue, its code is LIME-482, and it contains 6 books. Return only JSON with keys color, code, count in that order.", CalibrationAcceptance::json_value, R"({"color":"blue","code":"LIME-482","count":6})");
      break;
    case BenchmarkWorkload::coding:
      add("coding-explanation", "Explain a robust implementation of a bounded LRU cache, including data structures, lookup, insertion, eviction, concurrency and testing. Include pseudocode and discuss edge cases. Use at least 400 words. Do not execute code or use tools.", CalibrationAcceptance::performance_text, "");
      add("coding-merge", "Write a Python function that merges two sorted lists into one sorted list, with a docstring and two tests. Do not run the code or use tools.", CalibrationAcceptance::performance_text, "");
      add("coding-queue", "Explain a bounded producer-consumer queue, its synchronization, shutdown behavior and failure handling. Include pseudocode. Do not execute code or use tools.", CalibrationAcceptance::performance_text, "");
      add("coding-trace", "Trace this Python function without running tools:\ndef count_even(xs):\n    return sum(1 for x in xs if x % 2 == 0)\nFor inputs [1,2,3,4], [], and [0,-2,8,9], return only JSON {\"result\":[the three integer outputs]}.", CalibrationAcceptance::json_value, R"({"result":[2,0,3]})");
      add("coding-index", "Trace JavaScript: const a=[3,5,8]; const b=a.map((x,i)=>x-i); Return only JSON {\"result\":b} with the actual integer array. Do not execute code or call tools.", CalibrationAcceptance::json_value, R"({"result":[3,4,6]})");
      break;
    case BenchmarkWorkload::tools:
      for (const bool stream : {false, true}) add(stream ? "tools-stream" : "tools-json", "Make exactly two record_value function calls: first key alpha with value 7, then key beta with value 11. Do not combine records. Do not answer in text.", CalibrationAcceptance::tool_calls, std::string(kToolExpected), stream, true);
      break;
    case BenchmarkWorkload::custom:
      if (r.custom_prompt.empty() || r.custom_prompt.size() > 65536U || !is_valid_utf8(r.custom_prompt)) return std::string{"custom workload must contain bounded UTF-8 text"};
      add("custom", r.custom_prompt, CalibrationAcceptance::performance_text, "");
      break;
    case BenchmarkWorkload::long_context: break;
  }
  for (auto& test : tests) {
    auto request = r;
    if (test.acceptance == CalibrationAcceptance::performance_text)
      request.max_completion_tokens = std::max(r.max_completion_tokens, std::size_t{128});
    if (!valid(request, count) || !measure(test, request, count)) return std::string{"workload cannot fit or render with the selected model and chat limits"};
  }
  return tests;
}

std::variant<CalibrationCase, std::string> build_calibration_memory_probe(const CalibrationWorkloadRequest& r, const CalibrationTokenCounter& count) {
  return build_probe(r, count, false);
}

static std::variant<CalibrationCase, std::string> build_probe(const CalibrationWorkloadRequest& r,
    const CalibrationTokenCounter& count, bool performance) {
  if (!valid(r, count)) return std::string{"invalid memory probe limits"};
  const std::string prefix = performance ? "Study this record list as an example of a large log for the explanation requested after it.\n" :
      "Read the entire record list. The one calibration_target value is the answer. Ignore other records.\n";
  const std::string needle = "\ncalibration_target=cobalt-7391\n";
  const std::string suffix = performance ? "\nWrite a detailed 400-word explanation of a reliable algorithm for finding unique labeled records in large logs like these. Discuss parsing, malformed input, streaming and verification. Do not use tools." :
      "\nWhat is calibration_target? Reply only with its exact value and no explanation.";
  constexpr std::string_view line = "record: granite maple river lantern ordinary non-target information.\n";
  std::string filler;
  filler.reserve(kCalibrationPromptMaxBytes);
  while (filler.size() < kCalibrationPromptMaxBytes) filler.append(line.substr(0, kCalibrationPromptMaxBytes - filler.size()));
  const auto candidate = [&](std::size_t bytes) {
    CalibrationCase test; test.id = "long-context-retrieval"; test.expected = "cobalt-7391";
    test.context_probe = true;
    test.measured = performance;
    if (performance) { test.id = "long-context-explanation"; test.expected.clear(); test.acceptance = CalibrationAcceptance::performance_text; }
    const auto half = bytes / 2U;
    test.request_body = body(r, prefix + filler.substr(0, half) + needle + filler.substr(half, bytes - half) + suffix, false);
    return test;
  };
  auto best = candidate(0U);
  if (!measure(best, r, count)) return std::string{"memory probe cannot fit or render with the selected model and chat limits"};
  std::size_t low = 0, high = kCalibrationPromptMaxBytes - prefix.size() - needle.size() - suffix.size();
  while (low < high) {
    const auto middle = low + (high - low + 1U) / 2U;
    auto test = candidate(middle);
    if (measure(test, r, count)) { low = middle; best = std::move(test); } else high = middle - 1U;
  }
  best.full_context_probe = best.context_coverage >= 0.99;
  return best;
}

CalibrationQuality validate_calibration_response(const CalibrationCase& test, int status, std::string_view wire) {
  const bool semantic = test.acceptance != CalibrationAcceptance::structural_only && test.acceptance != CalibrationAcceptance::performance_text;
  const auto fail = [semantic](const char* reason) { return CalibrationQuality{false, semantic, reason}; };
  if (status != 200) return fail("request failed");
  const auto answer = read_answer(wire, test.stream);
  if (!answer) return fail("response is malformed, incomplete or too large");
  if (test.acceptance == CalibrationAcceptance::tool_calls) {
    const auto expected = json(test.expected);
    if (!expected || !expected->if_array() || expected->if_array()->size() != answer->calls.size() || answer->finish != "tool_calls" || !trim(answer->content).empty()) return fail("tool-call structure or completion is incorrect");
    std::set<std::string> ids;
    std::vector<bool> used(answer->calls.size(), false);
    std::size_t ordinal = 0;
    for (const auto& [index, call] : answer->calls) {
      if (index != ordinal++ || !call.function_type || call.id.empty() || call.id.size() > 256U || !ids.insert(call.id).second) return fail("tool-call identity is invalid");
      const auto args = json(call.arguments);
      if (!args || !args->if_object()) return fail("tool arguments are not complete JSON objects");
      bool matched = false;
      for (std::size_t i = 0; i < used.size(); ++i) {
        const auto& wanted = (*expected->if_array())[i];
        const auto* name = text(field(&wanted, "name"));
        const auto* wanted_args = field(&wanted, "arguments");
        if (!used[i] && name && wanted_args && call.name == *name && equal(*args, *wanted_args)) { used[i] = true; matched = true; break; }
      }
      if (!matched) return fail("tool names or arguments differ from the known answer");
    }
  } else {
    const bool capped_performance = test.acceptance == CalibrationAcceptance::performance_text && answer->finish == "length";
    if ((answer->finish != "stop" && !capped_performance) || !answer->calls.empty()) return fail("answer did not complete normally");
    if (test.acceptance == CalibrationAcceptance::exact_text && trim(answer->content) != test.expected) return fail("answer differs from the known answer");
    if (test.acceptance == CalibrationAcceptance::json_value) {
      const auto expected = json(test.expected), got = known_json_answer(answer->content);
      if (!expected || !got || !equal(*expected, *got)) return fail("structured answer differs from the known answer");
    }
    if (!semantic && trim(answer->content).empty()) return fail("performance workload returned no content");
  }
  return {true, semantic, semantic ? "known-answer acceptance passed; this is not a general accuracy proof" : "performance response completed; semantic quality is unvalidated"};
}

CalibrationCapture::CalibrationCapture(std::function<bool()> cancelled) : cancelled_(std::move(cancelled)) {}
bool CalibrationCapture::send_headers(const HttpHead& head) {
  if (status_ != 0 || client_gone()) { failed_ = true; return false; }
  status_ = head.status; return true;
}
bool CalibrationCapture::send_event(std::string_view bytes) {
  if (status_ == 0 || client_gone() || finished_ || bytes.size() > kCalibrationResponseMaxBytes - body_.size()) { failed_ = true; return false; }
  body_ += bytes; return true;
}
bool CalibrationCapture::writable(std::chrono::milliseconds) { return !client_gone() && !finished_; }
bool CalibrationCapture::client_gone() { if (cancelled_ && cancelled_()) failed_ = true; return failed_; }
void CalibrationCapture::finish() { if (status_ == 0 || client_gone()) failed_ = true; finished_ = true; }

std::optional<MonitorHostSnapshot> await_calibration_baseline(
    const std::function<std::optional<MonitorHostSnapshot>()>& read,
    const std::function<bool()>& cancelled, ServeMonitor::SteadyNow now,
    std::function<void(std::chrono::milliseconds)> pause) {
  if (!read || (cancelled && cancelled())) return std::nullopt;
  if (!now) now = [] { return std::chrono::steady_clock::now(); };
  if (!pause) pause = [](std::chrono::milliseconds duration) { std::this_thread::sleep_for(duration); };
  const auto began = now();
  std::optional<std::chrono::steady_clock::time_point> first;
  for (unsigned attempt = 0; attempt <= 30U; ++attempt) {
    if (cancelled && cancelled()) return std::nullopt;
    const auto at = now();
    if (at < began || at - began > std::chrono::seconds(3)) return std::nullopt;
    auto sample = read();
    if (!sample && attempt == 0U) return std::nullopt;
    if (sample && sample->sampled_at > began && sample->sampled_at <= at) {
      if (first && sample->sampled_at > *first) {
        sample->sample.gpu.reset();
        sample->sample.gpu_note = "baseline GPU acquisition time is unavailable";
        return sample;
      }
      if (!first) first = sample->sampled_at;
    }
    if (attempt < 30U) pause(std::chrono::milliseconds(100));
  }
  return std::nullopt;
}

CalibrationHostDiagnostics calibration_host_diagnostics(const std::optional<MonitorHostSnapshot>& before,
    const std::optional<MonitorHostSnapshot>& after, std::chrono::steady_clock::time_point now,
    std::optional<std::chrono::steady_clock::time_point> trial_started, bool calibration_model_loaded) {
  CalibrationHostDiagnostics out;
  const auto flag = [&](const char* reason, bool contaminated) {
    if (std::find(out.flags.begin(), out.flags.end(), reason) == out.flags.end()) out.flags.emplace_back(reason);
    out.contaminated = out.contaminated || contaminated;
  };
  const auto fresh = [](const std::optional<MonitorHostSnapshot>& s, std::chrono::steady_clock::time_point at) { return s && s->sampled_at <= at && at - s->sampled_at <= std::chrono::seconds(5); };
  const auto baseline_at = trial_started.value_or(now);
  const bool before_fresh = baseline_at <= now && fresh(before, baseline_at);
  const bool after_fresh = fresh(after, now);
  if (!before_fresh || !after_fresh) flag("host_readings_unavailable_or_stale", false);
  if (before_fresh) {
    const auto& s = before->sample;
    if (s.cpu_percent && std::isfinite(*s.cpu_percent) && *s.cpu_percent >= 50.0) flag("pretrial_cpu_activity", true);
    if (s.gpu && s.gpu->util_percent && std::isfinite(*s.gpu->util_percent) && *s.gpu->util_percent >= 20.0) flag("pretrial_gpu_activity", true);
  }
  for (const auto* snapshot : {&before, &after}) {
    if (!(snapshot == &before ? before_fresh : after_fresh)) continue;
    const auto& s = (*snapshot)->sample;
    if (s.memory && s.memory->total_bytes > 0U && s.memory->used_bytes <= s.memory->total_bytes) {
      const double free = 100.0 * static_cast<double>(s.memory->total_bytes - s.memory->used_bytes) / static_cast<double>(s.memory->total_bytes);
      out.ram_headroom_percent = out.ram_headroom_percent ? std::min(*out.ram_headroom_percent, free) : free;
      if (free < 5.0) flag("ram_pressure", snapshot == &before && !calibration_model_loaded);
    }
    if (s.gpu) {
      const auto& g = *s.gpu;
      if (g.mem_total_bytes && g.mem_used_bytes && *g.mem_total_bytes > 0U && *g.mem_used_bytes <= *g.mem_total_bytes) {
        const double free = 100.0 * static_cast<double>(*g.mem_total_bytes - *g.mem_used_bytes) / static_cast<double>(*g.mem_total_bytes);
        out.gpu_headroom_percent = out.gpu_headroom_percent ? std::min(*out.gpu_headroom_percent, free) : free;
        if (free < 5.0) flag("gpu_memory_pressure", snapshot == &before && !calibration_model_loaded);
      }
      if (g.temp_c && std::isfinite(*g.temp_c) && *g.temp_c >= 85.0) flag("high_gpu_temperature_throttling_unconfirmed", false);
    }
  }
  return out;
}
} // namespace secure_frontend
