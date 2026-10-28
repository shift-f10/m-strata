// strata-api-probe: test-only client for the SP3 GPU validation kit (docs/validation/sp3-api-kit.md)
// and for the SP5 parity acceptance kit (docs/validation/sp5-parity-kit.md): corpus mode
// (--corpus) runs a fixed corpus of equivalence-classed cases, prints and writes the fixed
// results schema `case,class,RESULT,detail` with both servers' command lines and the engine
// versions in the header, --compare diffs two sides' results files, and --dry-run renders
// the exact owner-box commands. Built in every preset, never installed. Talks only to a
// loopback address.
#include "secure_frontend/api_client.hpp"
#include "secure_frontend/config.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/sha256.hpp"
#include "secure_frontend/utf8_stream_decoder.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using secure_frontend::ChatExchangeEnd;
using secure_frontend::ChatExchangeResult;

struct Probe final {
  secure_frontend::LoopbackAddress address;
  std::uint16_t port;
  std::string token;
  std::string model;
};

long long ms_since(const Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

std::string json_text(const std::string& text) {
  std::string out;
  secure_frontend::append_json_string(&out, text);
  return out;
}

std::string body(const Probe& probe, const std::string& prompt, const std::string& extra) {
  return R"({"model":)" + json_text(probe.model) + R"(,"messages":[{"role":"user","content":)" + json_text(prompt) + "}]" +
         (extra.empty() ? "" : "," + extra) + "}";
}

std::optional<secure_frontend::JsonValue> parse(const std::string& text) {
  auto parsed = secure_frontend::parse_json(text, {.max_bytes = 16U << 20U, .max_depth = 8U, .max_container_entries = 4096U, .max_string_bytes = 16U << 20U});
  if (auto* value = std::get_if<secure_frontend::JsonValue>(&parsed)) return std::move(*value);
  return std::nullopt;
}

const secure_frontend::JsonValue* member(const secure_frontend::JsonValue* object, const std::string_view key) {
  const auto* members = object != nullptr ? object->if_object() : nullptr;
  if (members == nullptr) return nullptr;
  for (const auto& entry : *members) {
    if (entry.key == key) return &entry.value;
  }
  return nullptr;
}

const secure_frontend::JsonValue* first_choice(const secure_frontend::JsonValue* root) {
  const auto* choices = member(root, "choices");
  return choices != nullptr && choices->if_array() != nullptr && !choices->if_array()->empty() ? &choices->if_array()->front() : nullptr;
}

std::string string_of(const secure_frontend::JsonValue* value) { return value != nullptr && value->if_string() ? *value->if_string() : ""; }

struct StreamFacts final {
  std::string text;
  std::string finish;
  std::size_t content_events{0};
  long long first_content_ms{-1};
};

StreamFacts facts_of(const ChatExchangeResult& result) {
  StreamFacts facts;
  for (const auto& item : result.items) {
    if (item.kind != secure_frontend::SseItemKind::data || item.text == "[DONE]") continue;
    const auto root = parse(item.text);
    const auto* choice = first_choice(root ? &*root : nullptr);
    const auto* content = member(member(choice, "delta"), "content");
    if (content != nullptr && content->if_string() && !content->if_string()->empty()) {
      facts.text += *content->if_string();
      ++facts.content_events;
    }
    const auto finish = string_of(member(choice, "finish_reason"));
    if (!finish.empty()) facts.finish = finish;
  }
  return facts;
}

std::string complete_text(const ChatExchangeResult& result) {
  const auto root = parse(result.body);
  return string_of(member(member(first_choice(root ? &*root : nullptr), "message"), "content"));
}

ChatExchangeResult send(const Probe& probe, const std::string& json, secure_frontend::ChatRequestOptions options = {}) {
  if (options.timeout == std::chrono::seconds(30)) options.timeout = 600s;  // real prefill can be long
  return secure_frontend::post_chat_completion(probe.address, probe.port, probe.token, json, options);
}

int pass(const std::string& mode, const std::string& facts) {
  std::cout << "PASS " << mode << ' ' << facts << '\n';
  return 0;
}
int fail(const std::string& mode, const std::string& reason) {
  std::cout << "FAIL " << mode << ' ' << reason << '\n';
  return 1;
}

bool complete_stream(const ChatExchangeResult& result) {
  return result.end == ChatExchangeEnd::complete && result.streamed && result.done_seen && result.terminal_chunk_seen;
}

// After a cancel: how long until a one-token request is served (STOP -> DONE ... cancel -> next lease).
long long next_request_ms(const Probe& probe) {
  const auto start = Clock::now();
  const auto next = send(probe, body(probe, "Say OK.", R"("max_tokens":1)"));
  return next.head.status == 200 ? ms_since(start) : -1;
}

int run_mode(const Probe& probe, const std::string& mode) {
  if (mode == "stream") {
    const auto start = Clock::now();
    const auto result = send(probe, body(probe, "Say hello in five words.", R"("max_tokens":64,"stream":true)"));
    const auto facts = facts_of(result);
    if (!complete_stream(result) || facts.finish.empty()) return fail(mode, "stream not complete (needs [DONE] and the terminal chunk)");
    return pass(mode, "events=" + std::to_string(facts.content_events) + " bytes=" + std::to_string(facts.text.size()) +
                          " finish=" + facts.finish + " total_ms=" + std::to_string(ms_since(start)));
  }
  if (mode == "nonstream") {
    const auto result = send(probe, body(probe, "Say hello in five words.", R"("max_tokens":64)"));
    if (result.end != ChatExchangeEnd::complete || result.head.status != 200) return fail(mode, "status " + std::to_string(result.head.status));
    return pass(mode, "bytes=" + std::to_string(complete_text(result).size()));
  }
  if (mode.rfind("cancel-after=", 0) == 0) {
    const int after = std::stoi(mode.substr(13));
    int seen = 0;
    secure_frontend::ChatRequestOptions options;
    options.on_item = [&seen, after](const secure_frontend::SseItem& item) {
      seen += item.text.find(R"("delta":{"content")") != std::string::npos;
      return seen < after;
    };
    const auto result = send(probe, body(probe, "Count from 1 to 500.", R"("max_tokens":2048,"stream":true)"), options);
    if (result.end != ChatExchangeEnd::cancelled) return fail(mode, "the stream ended before the cancel");
    const auto next = next_request_ms(probe);
    if (next < 0) return fail(mode, "the next request failed after the cancel");
    return pass(mode, "next_request_ms=" + std::to_string(next) + " (compare with limits.engine.stop_drain_timeout_s)");
  }
  if (mode == "cancel-during-prefill") {
    std::string prompt;
    while (prompt.size() < 40000U) prompt += "Summarize this sentence carefully. ";
    secure_frontend::ChatRequestOptions options;
    options.on_item = [](const secure_frontend::SseItem&) { return false; };  // close at the role chunk
    (void)send(probe, body(probe, prompt, R"("max_tokens":16,"stream":true)"), options);
    const auto next = next_request_ms(probe);
    if (next < 0) return fail(mode, "the next request failed after a cancel during prefill");
    return pass(mode, "next_request_ms=" + std::to_string(next) + " (risk R4: STOP latency during prefill)");
  }
  if (mode.rfind("concurrent=", 0) == 0) {
    const int count = std::stoi(mode.substr(11));
    std::mutex lock;
    std::vector<int> finished;
    std::vector<std::thread> clients;
    for (int index = 0; index < count; ++index) {
      clients.emplace_back([&, index] {
        const auto result = send(probe, body(probe, "Say OK.", R"("max_tokens":16)"));
        std::lock_guard guard(lock);
        finished.push_back(result.head.status == 200 ? index : -1 - index);
      });
      std::this_thread::sleep_for(100ms);  // admission order = start order
    }
    for (auto& client : clients) client.join();
    for (int index = 0; index < count; ++index) {
      if (finished[static_cast<std::size_t>(index)] != index) return fail(mode, "completion order is not admission order (FIFO)");
    }
    return pass(mode, "fifo=yes clients=" + std::to_string(count));
  }
  if (mode == "utf8") {
    const std::string prompt = "Repeat exactly: 東京 и 🌍 — ok";
    const auto streamed = send(probe, body(probe, prompt, R"("max_tokens":64,"seed":7,"temperature":0,"stream":true)"));
    const auto complete = send(probe, body(probe, prompt, R"("max_tokens":64,"seed":7,"temperature":0)"));
    const auto facts = facts_of(streamed);
    secure_frontend::Utf8StreamDecoder check;
    const bool valid = check.push(facts.text) == facts.text && check.finish(false).empty();
    if (!complete_stream(streamed) || !valid) return fail(mode, "stream incomplete or not valid UTF-8");
    if (facts.text != complete_text(complete)) return fail(mode, "stream text differs from non-stream text at a fixed seed");
    return pass(mode, "bytes=" + std::to_string(facts.text.size()));
  }
  if (mode == "seed") {
    const auto first = send(probe, body(probe, "Write one sentence about rivers.", R"("max_tokens":48,"seed":42,"temperature":0.7)"));
    const auto second = send(probe, body(probe, "Write one sentence about rivers.", R"("max_tokens":48,"seed":42,"temperature":0.7)"));
    if (first.head.status != 200 || complete_text(first) != complete_text(second)) return fail(mode, "the same seed gave different text");
    return pass(mode, "same_text=yes");
  }
  return -1;
}

// ---------------------------------------------------------------------------
// SP5 M2 Task 9: the parity acceptance kit (spec §7, C-6 equivalence classes).
// A corpus file is one JSON object per line (blank lines and # lines ignored):
//   {"case":"name","class":"text|shape|divergent-by-decision","request":{...},
//    "expect":{"status":200,"finish":"length"},"authorised_by":"B6"}
// The probe injects its --model and refuses stream:true (streaming is the SP3
// kit's ground). Every case lands exactly one `case,class,RESULT,detail` line:
// RESULT is PASS, FAIL or RECORDED — never a skip.
// ---------------------------------------------------------------------------

constexpr const char* kResultsTitle = "# strata-api-probe sp5-parity results";
constexpr const char* kCompareTitle = "# strata-api-probe sp5-parity compare";
constexpr const char* kResultsSchema = "case,class,RESULT,detail";

std::string sha256_hex(const std::string& text) {
  secure_frontend::Sha256 hasher;
  hasher.update(text);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (const std::uint8_t byte : hasher.finish()) {
    out.push_back(kHex[byte >> 4U]);
    out.push_back(kHex[byte & 0x0FU]);
  }
  return out;
}

// Round-trips a parsed JSON value back to JSON text (the corpus request is
// parsed to find it, then sent verbatim in shape). Fractions render as plain
// fixed-point decimals, valid JSON for the server either way.
void append_json_value(std::string* out, const secure_frontend::JsonValue& value) {
  switch (value.kind()) {
    case secure_frontend::JsonKind::null: out->append("null"); return;
    case secure_frontend::JsonKind::boolean: out->append(*value.if_boolean() ? "true" : "false"); return;
    case secure_frontend::JsonKind::integer: out->append(std::to_string(*value.if_integer())); return;
    case secure_frontend::JsonKind::decimal: out->append(std::to_string(*value.if_decimal())); return;
    case secure_frontend::JsonKind::string: secure_frontend::append_json_string(out, *value.if_string()); return;
    case secure_frontend::JsonKind::array: {
      out->push_back('[');
      bool first = true;
      for (const auto& item : *value.if_array()) {
        if (!first) out->push_back(',');
        first = false;
        append_json_value(out, item);
      }
      out->push_back(']');
      return;
    }
    case secure_frontend::JsonKind::object: {
      out->push_back('{');
      bool first = true;
      for (const auto& entry : *value.if_object()) {
        if (!first) out->push_back(',');
        first = false;
        secure_frontend::append_json_string(out, entry.key);
        out->push_back(':');
        append_json_value(out, entry.value);
      }
      out->push_back('}');
      return;
    }
  }
}

std::string corpus_body(const Probe& probe, const secure_frontend::JsonValue& request) {
  std::string out = R"({"model":)" + json_text(probe.model);
  for (const auto& entry : *request.if_object()) {
    if (entry.key == "model") continue;  // the probe's --model wins on both sides
    out.push_back(',');
    secure_frontend::append_json_string(&out, entry.key);
    out.push_back(':');
    append_json_value(&out, entry.value);
  }
  out.push_back('}');
  return out;
}

struct ShapeFacts final {
  int status{0};
  std::string finish;
  bool usage{false};
  bool parsed{false};
};

ShapeFacts shape_facts(const ChatExchangeResult& result) {
  ShapeFacts facts;
  facts.status = static_cast<int>(result.head.status);
  const auto root = parse(result.body);
  if (!root.has_value()) return facts;
  facts.parsed = true;
  facts.finish = string_of(member(first_choice(&*root), "finish_reason"));
  facts.usage = member(&*root, "usage") != nullptr;
  return facts;
}

struct KitCase final {
  std::string name;
  std::string class_name;
  std::string detail;  // the observed facts, comma-free
  std::string result;  // PASS, FAIL or RECORDED
};

KitCase run_corpus_case(const Probe& probe, const secure_frontend::JsonValue& line, std::size_t number) {
  const auto text_of = [&](const char* key) -> std::string { return string_of(member(&line, key)); };
  KitCase item;
  item.name = text_of("case");
  item.class_name = text_of("class");
  if (item.name.empty() || item.class_name.empty()) {
    item.class_name = item.class_name.empty() ? "corpus" : item.class_name;
    item.name = item.name.empty() ? "line" + std::to_string(number) : item.name;
    item.result = "FAIL";
    item.detail = "missing_case_or_class";
    return item;
  }
  const auto* request = member(&line, "request");
  if (request == nullptr || request->if_object() == nullptr) {
    item.result = "FAIL";
    item.detail = "missing_request_object";
    return item;
  }
  for (const auto& member : *request->if_object()) {
    if (member.key == "stream" && member.value.if_boolean() && *member.value.if_boolean()) {
      item.result = "FAIL";
      item.detail = "stream_not_in_corpus_mode_sp3_kit_covers_streaming";
      return item;
    }
  }
  const auto json = corpus_body(probe, *request);
  if (item.class_name == "text") {
    // C-6 text class: the same request twice; identical valid-UTF-8 text is the
    // deterministic pair; the kit then text-for-text diffs the two sides' hashes.
    const auto first = send(probe, json);
    const auto second = send(probe, json);
    const auto first_text = complete_text(first);
    const auto second_text = complete_text(second);
    const bool valid = secure_frontend::is_valid_utf8(first_text);
    if (first.head.status != 200 || second.head.status != 200) {
      item.result = "FAIL";
      item.detail = "status=" + std::to_string(first.head.status) + "/" + std::to_string(second.head.status);
    } else if (!valid) {
      item.result = "FAIL";
      item.detail = "text_not_valid_utf8";
    } else if (first_text != second_text) {
      item.result = "FAIL";
      item.detail = "same_seed_text_differs_between_two_runs";
    } else {
      item.result = "PASS";
      item.detail = "bytes=" + std::to_string(first_text.size()) + " sha256=" + sha256_hex(first_text);
    }
    return item;
  }
  if (item.class_name == "shape") {
    const auto facts = shape_facts(send(probe, json));
    item.detail = "status=" + std::to_string(facts.status) + " finish=" + (facts.finish.empty() ? "-" : facts.finish) +
                  " usage=" + (facts.usage ? "yes" : "no");
    const auto* expect = member(&line, "expect");
    const auto* want_status = expect != nullptr ? member(expect, "status") : nullptr;
    const auto want_finish = string_of(expect != nullptr ? member(expect, "finish") : nullptr);
    bool ok = facts.parsed;
    if (want_status != nullptr && want_status->if_integer() != nullptr &&
        static_cast<int>(*want_status->if_integer()) != facts.status)
      ok = false;
    if (!want_finish.empty() && want_finish != facts.finish) ok = false;
    if (expect != nullptr) {
      if (const auto* want_usage = member(expect, "usage"); want_usage != nullptr && want_usage->if_boolean() != nullptr) {
        if (*want_usage->if_boolean() != facts.usage) ok = false;
      }
    }
    item.result = ok ? "PASS" : "FAIL";
    return item;
  }
  if (item.class_name == "divergent-by-decision") {
    const auto authorised = text_of("authorised_by");
    if (authorised.empty()) {
      item.result = "FAIL";
      item.detail = "authorised_by_missing";
      return item;
    }
    const auto facts = shape_facts(send(probe, json));
    if (!facts.parsed && facts.status == 0) {
      item.result = "FAIL";
      item.detail = "no_http_exchange_authorisation_records_a_shape";
    } else {
      item.result = "RECORDED";
      item.detail = "authorised_by=" + authorised + " status=" + std::to_string(facts.status) +
                    " finish=" + (facts.finish.empty() ? "-" : facts.finish);
    }
    return item;
  }
  item.result = "FAIL";
  item.detail = "unknown_class_" + item.class_name;
  return item;
}

int run_corpus_mode(const Probe& probe, const std::string& corpus_path, const std::vector<std::string>& command_lines,
                    const std::vector<std::string>& versions, const std::optional<std::string>& results_path) {
  std::ifstream file(corpus_path, std::ios::binary);
  if (!file) {
    std::cerr << "cannot open the corpus file\n";
    return 2;
  }
  std::string report = std::string{kResultsTitle} + "\n";
  for (const auto& line : command_lines) report += "# command: " + line + "\n";
  for (const auto& version : versions) report += "# engine-version: " + version + "\n";
  report += std::string{kResultsSchema} + "\n";
  bool failed = false;
  std::size_t number = 0;
  std::string line;
  while (std::getline(file, line)) {
    ++number;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line.front() == '#') continue;
    const auto parsed = secure_frontend::parse_json(line, secure_frontend::JsonLimits{.max_bytes = 262144U,
                                                                                      .max_depth = 24U,
                                                                                      .max_container_entries = 256U,
                                                                                      .max_string_bytes = 65536U,
                                                                                      .allow_fractions = true});
    const auto* value = std::get_if<secure_frontend::JsonValue>(&parsed);
    if (value == nullptr || value->if_object() == nullptr) {
      report += "corpus,corpus,FAIL,unparsable_line_" + std::to_string(number) + "\n";
      failed = true;
      continue;
    }
    const auto item = run_corpus_case(probe, *value, number);
    if (item.name.find(',') != std::string::npos || item.class_name.find(',') != std::string::npos ||
        item.detail.find(',') != std::string::npos) {
      report += "corpus,corpus,FAIL,comma_in_field_line_" + std::to_string(number) + "\n";
      failed = true;
      continue;
    }
    report += item.name + "," + item.class_name + "," + item.result + "," + item.detail + "\n";
    failed = failed || item.result == "FAIL";
  }
  std::cout << report;
  if (results_path.has_value()) {
    std::ofstream out(*results_path, std::ios::binary);
    if (!out) {
      std::cerr << "cannot write the results file\n";
      return 2;
    }
    out << report;
  }
  return failed ? 1 : 0;
}

// The compare mode reads a results file into rows (in document order).
struct ParsedResults final {
  bool ok{false};
  std::vector<std::array<std::string, 4>> rows{};  // case,class,RESULT,detail
};

ParsedResults read_results_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  ParsedResults parsed;
  if (!file) return parsed;
  bool schema_seen = false;
  std::string line;
  while (std::getline(file, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line.front() == '#') continue;
    if (line == kResultsSchema) {
      schema_seen = true;
      continue;
    }
    if (!schema_seen) continue;
    std::array<std::string, 4> row{};
    std::size_t field = 0;
    std::size_t start = 0;
    for (std::size_t index = 0; index <= line.size() && field < 4U; ++index) {
      if (index == line.size() || (line[index] == ',' && field < 3U)) {
        row[field++] = line.substr(start, index - start);
        start = index + 1U;
      }
    }
    if (field == 4U) parsed.rows.push_back(row);
  }
  parsed.ok = schema_seen;
  return parsed;
}

std::string detail_token(const std::string& detail, const std::string& key) {
  const auto at = detail.find(key + "=");
  if (at == std::string::npos) return {};
  const auto end = detail.find(' ', at);
  return detail.substr(at + key.size() + 1U, end == std::string::npos ? std::string::npos : end - at - key.size() - 1U);
}

int run_compare_mode(const std::string& left, const std::string& right) {
  std::string report = std::string{kCompareTitle} + "\n# results: " + left + "\n# results: " + right + "\n";
  report += std::string{kResultsSchema} + "\n";
  const auto left_rows = read_results_file(left);
  const auto right_rows = read_results_file(right);
  bool failed = false;
  const auto finish_with = [&](const std::array<std::string, 4>& row, const std::string& result, const std::string& detail) {
    report += row[0] + "," + row[1] + "," + result + "," + detail + "\n";
    failed = failed || result == "FAIL";
  };
  if (!left_rows.ok || left_rows.rows.empty()) {
    report += "corpus,compare,FAIL,missing_results_file:" + left + "\n";
    failed = true;
  }
  if (!right_rows.ok || right_rows.rows.empty()) {
    report += "corpus,compare,FAIL,missing_results_file:" + right + "\n";
    failed = true;
  }
  if (!failed) {
    if (left_rows.rows.size() != right_rows.rows.size()) {
      report += "corpus,compare,FAIL,case_count_differs\n";
      failed = true;
    } else {
      for (std::size_t index = 0; index < left_rows.rows.size(); ++index) {
        const auto& a = left_rows.rows[index];
        const auto& b = right_rows.rows[index];
        if (a[0] != b[0] || a[1] != b[1]) {
          report += "corpus,compare,FAIL,case_order_differs_at_line_" + std::to_string(index + 1U) + "\n";
          failed = true;
          continue;
        }
        if (a[2] == "FAIL" || b[2] == "FAIL") {
          finish_with(a, "FAIL", std::string{a[2] == "FAIL" ? "left" : "right"} + "_result_failed");
          continue;
        }
        if (a[1] == "text") {
          const auto left_sha = detail_token(a[3], "sha256");
          const auto right_sha = detail_token(b[3], "sha256");
          finish_with(a, !left_sha.empty() && left_sha == right_sha ? "PASS" : "FAIL",
                      !left_sha.empty() && left_sha == right_sha ? "sha256_equal" : "text_differs");
        } else if (a[1] == "shape") {
          const auto left_finish = detail_token(a[3], "finish");
          finish_with(a, a[2] == "PASS" && left_finish == detail_token(b[3], "finish") ? "PASS" : "FAIL",
                      a[2] == "PASS" && left_finish == detail_token(b[3], "finish") ? "finish_equal" : "finish_differs");
        } else {
          finish_with(a, a[2] == "RECORDED" && b[2] == "RECORDED" ? "PASS" : "FAIL",
                      a[2] == "RECORDED" && b[2] == "RECORDED" ? "recorded_both" : "record_missing");
        }
      }
    }
  }
  std::cout << report;
  return failed ? 1 : 0;
}

std::string shell_quote(const std::string& value) {
  std::string out = "'";
  for (const char byte : value) {
    if (byte == '\'') out += "'\\''";
    else out.push_back(byte);
  }
  out.push_back('\'');
  return out;
}

int run_dry_run_mode(const std::string& argv0, const std::string& corpus, const std::string& token_file,
                     const std::string& model, int ref_port, int serve_port, const std::string& ref_results,
                     const std::string& serve_results, const std::vector<std::string>& command_lines,
                     const std::vector<std::string>& versions) {
  std::string common = " --token-file " + shell_quote(token_file) + " --model " + shell_quote(model) + " --corpus " +
                       shell_quote(corpus);
  for (const auto& line : command_lines) common += " --command-line " + shell_quote(line);
  for (const auto& version : versions) common += " --engine-version " + shell_quote(version);
  std::cout << "# reference side (reference server up, then this run)\n"
            << shell_quote(argv0) << " --port " << ref_port << common << " --results " << shell_quote(ref_results)
            << "\n# our side (strata serve up, then this run)\n"
            << shell_quote(argv0) << " --port " << serve_port << common << " --results " << shell_quote(serve_results)
            << "\n# compare\n"
            << shell_quote(argv0) << " --compare " << shell_quote(ref_results) << " " << shell_quote(serve_results) << "\n";
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::optional<std::uint16_t> port;
  std::string token_file;
  std::string model;
  std::string host = "127.0.0.1";
  std::string mode;
  std::string corpus;
  std::optional<std::string> results;
  std::string compare_left;
  std::string compare_right;
  bool dry_run = false;
  std::vector<std::string> command_lines;
  std::vector<std::string> versions;
  std::string ref_results;
  std::string serve_results;
  int ref_port = 0;
  int serve_port = 0;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const bool has_value = index + 1 < argc;
    if (argument == "--port" && has_value) port = static_cast<std::uint16_t>(std::stoi(argv[++index]));
    else if (argument == "--token-file" && has_value) token_file = argv[++index];
    else if (argument == "--model" && has_value) model = argv[++index];
    else if (argument == "--host" && has_value) host = argv[++index];
    else if (argument == "--corpus" && has_value) corpus = argv[++index];
    else if (argument == "--results" && has_value) results = argv[++index];
    else if (argument == "--command-line" && has_value) command_lines.push_back(argv[++index]);
    else if (argument == "--engine-version" && has_value) versions.push_back(argv[++index]);
    else if (argument == "--ref-results" && has_value) ref_results = argv[++index];
    else if (argument == "--serve-results" && has_value) serve_results = argv[++index];
    else if (argument == "--ref-port" && has_value) ref_port = std::stoi(argv[++index]);
    else if (argument == "--serve-port" && has_value) serve_port = std::stoi(argv[++index]);
    else if (argument == "--dry-run") dry_run = true;
    else if (argument == "--compare" && index + 2 < argc) {
      compare_left = argv[++index];
      compare_right = argv[++index];
    } else mode = argument;
  }
  if (!compare_left.empty()) return run_compare_mode(compare_left, compare_right);
  if (dry_run) {
    if (corpus.empty() || token_file.empty() || model.empty() || ref_port <= 0 || serve_port <= 0 || ref_results.empty() ||
        serve_results.empty() || command_lines.empty() || versions.empty()) {
      std::cerr << "usage: strata-api-probe --dry-run --corpus <file> --token-file <path> --model <id> "
                   "--ref-port <n> --serve-port <n> --ref-results <path> --serve-results <path> "
                   "--command-line '<cmd>' ... --engine-version '<v>' ...\n";
      return 2;
    }
    return run_dry_run_mode(argv[0], corpus, token_file, model, ref_port, serve_port, ref_results, serve_results,
                            command_lines, versions);
  }
  const auto ip = secure_frontend::parse_listener_address(host);
  auto address = ip ? secure_frontend::LoopbackAddress::from(*ip) : std::nullopt;
  std::ifstream file(token_file, std::ios::binary);
  std::string token((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) token.pop_back();
  if (!port || !address || token.empty() || model.empty()) {
    std::cerr << "usage: strata-api-probe --port <n> --token-file <path> --model <id> [--host 127.0.0.1|::1] "
                 "<stream|nonstream|cancel-after=<n>|cancel-during-prefill|concurrent=<k>|utf8|seed>\n"
                 "   or: strata-api-probe --port <n> --token-file <path> --model <id> --corpus <file> "
                 "[--results <path>] [--command-line '<cmd>'] [--engine-version '<v>']   (SP5 parity kit)\n"
                 "   or: strata-api-probe --compare <file-a> <file-b>                     (SP5 parity kit)\n"
                 "   or: strata-api-probe --dry-run ...                                   (render the kit commands)\n";
    return 2;
  }
  const Probe probe{std::move(*address), *port, std::move(token), std::move(model)};
  if (!corpus.empty()) return run_corpus_mode(probe, corpus, command_lines, versions, results);
  const int result = run_mode(probe, mode);
  if (result < 0) {
    std::cerr << "unknown mode\n";
    return 2;
  }
  return result;
}
