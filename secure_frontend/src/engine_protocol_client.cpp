#include "secure_frontend/engine_protocol_client.hpp"

#include "secure_frontend/config_defaults.hpp"
#include "secure_frontend/deadline.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace secure_frontend {
namespace {

bool ascii_printable(const std::string_view text) {
  for (const unsigned char character : text) {
    if (character < 0x20U || character > 0x7eU) return false;
  }
  return true;
}

bool nonnegative_decimal_metric(const std::string_view text) {
  if (text.empty()) return false;
  bool seen_digit = false;
  bool seen_dot = false;
  for (const char character : text) {
    if (character >= '0' && character <= '9') {
      seen_digit = true;
      continue;
    }
    if (character == '.' && !seen_dot) {
      seen_dot = true;
      continue;
    }
    return false;
  }
  return seen_digit;
}

template <typename Number>
bool parse_decimal(const std::string_view text, Number& value) {
  if (text.empty()) return false;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

std::vector<std::string_view> fields(const std::string_view line) {  std::vector<std::string_view> result;
  std::size_t begin = 0;
  while (begin < line.size()) {
    const auto end = line.find(' ', begin);
    if (end == std::string_view::npos) {
      result.push_back(line.substr(begin));
      break;
    }
    result.push_back(line.substr(begin, end - begin));
    begin = end + 1;
  }
  return result;
}

// DONE <generated> <prompt> <prompt_ms> <decode_ms> <finish> ... (generate.cpp);
// the two-field "DONE <finish>" form and older short forms ("DONE 1") are an ordinary stop.
// A full record (6+ fields) whose finish is not one of the four literals is malformed:
// fail closed, like every other unknown protocol shape.
std::optional<std::string_view> finish_literal(const std::vector<std::string_view>& done) {
  static constexpr std::array<std::string_view, 4> kFinishes{"stop", "length", "cancel", "nonfinite"};
  const auto match = [](const std::string_view word) -> std::optional<std::string_view> {
    for (const std::string_view literal : kFinishes) {
      if (literal == word) return literal;
    }
    return std::nullopt;
  };
  if (done.size() >= 6U) return match(done[5]);
  if (done.size() == 2U) {
    if (const auto literal = match(done[1])) return literal;
  }
  return std::string_view{"stop"};
}

// The Monitor's DONE figures (DoneFigures): positional fields 2-4 and 8 of a full record; a
// field that is not a plain non-negative number is left empty.
DoneFigures done_figures(const std::vector<std::string_view>& done) {
  DoneFigures figures;
  if (done.size() < 6U) return figures;
  const auto count = [](const std::string_view text) -> std::optional<std::size_t> {
    std::size_t value = 0;
    if (text.empty() || text.front() == '+' || text.front() == '-') return std::nullopt;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
  };
  const auto milliseconds = [](const std::string_view text) -> std::optional<double> {
    double value = 0.0;
    if (text.empty() || text.front() == '+' || text.front() == '-') return std::nullopt;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::fixed);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return std::nullopt;
    return value;
  };
  figures.prompt_tokens = count(done[2]);
  figures.prompt_ms = milliseconds(done[3]);
  figures.decode_ms = milliseconds(done[4]);
  if (done.size() >= 9U) figures.reused = count(done[8]);
  return figures;
}

}  // namespace

bool is_engine_fatal_line(const std::string_view line) noexcept {
  return std::any_of(kEngineFatalPrefixes.begin(), kEngineFatalPrefixes.end(),
                     [line](const std::string_view prefix) { return line.substr(0, prefix.size()) == prefix; });
}

std::string render_sampling_keys(const SamplingParams& sampling, const bool zero_window_disables) {
  std::string out;
  const auto number = [&out](std::string_view key, const double value) {
    char buffer[64];
    const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);  // shortest round-trip
    if (ec != std::errc{}) return;
    out += ' ';
    out += key;
    out += '=';
    out.append(buffer, end);
  };
  // The pinned v0.2.2 wire rules (serve/server.py:1367-1401), in the pin's
  // order: neutral values the engine already defaults to are omitted, top_k
  // clamps wide lists to the engine's 64 candidates, penalties take the
  // engine's alias names, and any penalty without a window gets the engine's
  // default window of 64.
  if (sampling.temperature.has_value() && *sampling.temperature > 0.0) number("temperature", *sampling.temperature);
  if (sampling.top_p.has_value() && *sampling.top_p < 1.0) number("top_p", *sampling.top_p);
  if (sampling.top_k.has_value() && *sampling.top_k >= 0) {
    const auto candidates = (*sampling.top_k >= 1 && *sampling.top_k <= 64) ? *sampling.top_k : 64;
    out += " top_k=" + std::to_string(candidates);
  }
  if (sampling.min_p.has_value() && *sampling.min_p > 0.0 && *sampling.min_p <= 1.0) number("min_p", *sampling.min_p);
  const bool repeat_on = sampling.repetition_penalty.has_value() && *sampling.repetition_penalty != 1.0;
  const bool frequency_on = sampling.frequency_penalty.has_value() && *sampling.frequency_penalty != 0.0;
  const bool presence_on = sampling.presence_penalty.has_value() && *sampling.presence_penalty != 0.0;
  // SP5-P2 Task 7: on the GLM engine a window of 0 means NO penalty (generate.cpp:3467
  // `pen_on = pen_n > 0`; maya serve/server.py:274-275 in words) — the keys are omitted
  // entirely, so the picks are exactly as without them. The strata pin keeps its
  // default-window-of-64 rule for the same input (serve/server.py:1360-1394).
  const bool glm_zero_off = zero_window_disables && sampling.penalty_last_n.has_value() && *sampling.penalty_last_n == 0;
  if (!glm_zero_off) {
    if (repeat_on) number("penalty_repeat", *sampling.repetition_penalty);
    if (frequency_on) number("penalty_freq", *sampling.frequency_penalty);
    if (presence_on) number("penalty_present", *sampling.presence_penalty);
    if (repeat_on || frequency_on || presence_on) {
      const auto window =
          (sampling.penalty_last_n.has_value() && *sampling.penalty_last_n > 0) ? *sampling.penalty_last_n : 64;
      out += " penalty_last_n=" + std::to_string(window);
    }
  }
  if (sampling.seed.has_value() && *sampling.seed > 0U) out += " seed=" + std::to_string(*sampling.seed);
  return out;
}

EngineProtocolClient::EngineProtocolClient(EngineLineTransport& transport,
                                           const EngineProtocolLimits limits,
                                           const EngineProtocolFlavor flavor)
    : transport_(transport),
      limits_(limits),
      flavor_(flavor),
      effective_max_line_bytes_(limits.max_line_bytes != 0U ? limits.max_line_bytes : kDefaultInboundLineBytes) {}

// Monitor: keep the key=value fields of INFO (pre-READY, both flavors) and the maya_118
// STAT cadence, last value seen per key. The records' SHAPE is already fail-closed at
// the call sites; this only bounds and stores — meaning belongs to the consumers.
void EngineProtocolClient::capture_engine_readings(const std::vector<std::string_view>& parsed) {
  for (std::size_t i = 1; i < parsed.size(); ++i) {
    const auto equals = parsed[i].find('=');
    if (equals == std::string_view::npos || equals == 0U || equals > 32U) continue;
    const auto key = parsed[i].substr(0, equals);
    const auto value = parsed[i].substr(equals + 1);
    if (value.empty() || value.size() > 64U) continue;
    bool key_ok = true;
    for (const char c : key) {
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
        key_ok = false;
        break;
      }
    }
    if (!key_ok) continue;
    if (engine_readings_.size() >= 96U && engine_readings_.count(std::string(key)) == 0U) continue;
    engine_readings_[std::string(key)] = std::string(value);
  }
}

std::size_t EngineProtocolClient::inbound_max_line_bytes() const noexcept {
  return limits_.max_line_bytes != 0U ? limits_.max_line_bytes : kDefaultInboundLineBytes;
}

std::optional<EngineProtocolError> EngineProtocolClient::fail(const EngineProtocolErrorCode code,
                                                              std::string message) {
  state_ = State::failed;
  failure_code_ = code;
  return EngineProtocolError{code, std::move(message)};
}

std::optional<EngineProtocolError> EngineProtocolClient::validate_incoming(const std::string_view line) const {
  if (line.empty() || line.size() > inbound_max_line_bytes()) {
    return EngineProtocolError{line.empty() ? EngineProtocolErrorCode::malformed_line
                                             : EngineProtocolErrorCode::line_too_long,
                               "engine sent an empty or oversized protocol line"};
  }
  if (!ascii_printable(line)) {
    return EngineProtocolError{EngineProtocolErrorCode::malformed_line,
                               "engine sent a non-ASCII or control protocol line"};
  }
  return std::nullopt;
}

std::optional<EngineProtocolError> EngineProtocolClient::write_command(const std::string_view command,
                                                                        const MonotonicClock::time_point deadline) {
  if (command.empty() || command.size() > effective_max_line_bytes_ || !ascii_printable(command)) {
    return fail(EngineProtocolErrorCode::invalid_argument, "outbound protocol command is invalid");
  }
  std::string transport_error;
  if (!transport_.write_line(command, deadline, transport_error)) {
    return fail(EngineProtocolErrorCode::transport_write_failed,
                transport_error.empty() ? "engine command write failed" : transport_error);
  }
  return std::nullopt;
}

std::optional<EngineProtocolError> EngineProtocolClient::await_ready(const std::chrono::milliseconds timeout,
                                                                      EngineReady& ready) {
  if (state_ != State::waiting_ready) {
    return EngineProtocolError{EngineProtocolErrorCode::invalid_state, "engine is not awaiting READY"};
  }
  const auto deadline = deadline_after(timeout);
  while (true) {
    const auto remaining = remaining_until(deadline);
    if (remaining.count() == 0) {
      return EngineProtocolError{EngineProtocolErrorCode::timeout, "engine did not report READY before timeout"};
    }
    const auto response = transport_.read_line(remaining);
    if (response.kind == EngineReadKind::timeout) {
      return EngineProtocolError{EngineProtocolErrorCode::timeout, "engine did not report READY before timeout"};
    }
    if (response.kind == EngineReadKind::closed) return fail(EngineProtocolErrorCode::closed, "engine closed before READY");
    if (response.kind == EngineReadKind::failure) {
      return fail(EngineProtocolErrorCode::transport_read_failed,
                  response.message.empty() ? "engine READY read failed" : response.message);
    }
    if (const auto validation = validate_incoming(response.line); validation.has_value()) {
      return fail(validation->code, validation->message);
    }
    if (is_engine_fatal_line(response.line)) {
      return fail(EngineProtocolErrorCode::fatal_engine_line, "engine reported a fatal verification failure");
    }
    const auto parsed = fields(response.line);
    if (parsed.size() >= 2 && parsed.front() == "INFO") {
      capture_engine_readings(parsed);
      continue;
    }
    if (flavor_ == EngineProtocolFlavor::maya_118 && !parsed.empty() && (parsed.front() == "VRAM" || parsed.front() == "NOTE")) {
      // Maya v1.1.21's fast path prints one `VRAM key=value ...` record per GPU part
      // before READY (maya-ref src/program/generate.cpp:1054-1062) and, when Windows'
      // page file caps the commit, a `NOTE experts: ...` line (:1065-1068); its own
      // reader skips both (serve/server.py:456-473). Consumed here, never output,
      // bounded: VRAM under the STAT rule (at least one pair, every key non-empty
      // before its `=`), NOTE non-empty. Anything else before READY stays unexpected.
      if (parsed.size() < 2U) return fail(EngineProtocolErrorCode::unexpected_line, "engine emitted an empty startup record");
      if (parsed.front() == "VRAM") {
        for (std::size_t i = 1; i < parsed.size(); ++i) {
          const auto equals = parsed[i].find('=');
          if (equals == std::string_view::npos || equals == 0U) {
            return fail(EngineProtocolErrorCode::unexpected_line, "engine emitted a malformed VRAM record");
          }
        }
      }
      continue;
    }
    if (parsed.size() < 2 || parsed.front() != "READY") {
      return fail(EngineProtocolErrorCode::unexpected_line, "engine did not report a valid READY line");
    }
    std::uint32_t context = 0;
    if (!parse_decimal(parsed[1], context) || context == 0) {
      return fail(EngineProtocolErrorCode::malformed_line, "engine READY context is invalid");
    }
    bool can_stop = false;
    for (std::size_t index = 2; index < parsed.size(); ++index) {
      if (parsed[index].empty()) return fail(EngineProtocolErrorCode::malformed_line, "engine READY contains empty field");
      if (parsed[index] == "stop") can_stop = true;
    }
    ready = EngineReady{context, can_stop};
    // The engine reads stdin lines without bound, so the frontend caps its own
    // GEN line: a full-context prompt of 10-digit ids plus separators and header,
    // clamped to the transport's hard write cap so a huge advertised context
    // yields a clean invalid_argument instead of a write the transport refuses.
    effective_max_line_bytes_ = limits_.max_line_bytes != 0U
        ? limits_.max_line_bytes
        : std::min<std::size_t>(32U + 11U * static_cast<std::size_t>(context), kProtocolMaxLineBytes);
    supports_stop_ = can_stop;
    state_ = State::idle;
    return std::nullopt;
  }
}

std::optional<EngineProtocolError> EngineProtocolClient::begin_generation(
    const std::uint32_t max_new_tokens, const std::vector<std::uint32_t>& prompt_token_ids,
    const MonotonicClock::time_point write_deadline, const SamplingParams& sampling, const std::string_view embeddings) {
  stop_cause_.reset();  // a cause never outlives its request, even if this GEN is refused
  if (state_ != State::idle) return EngineProtocolError{EngineProtocolErrorCode::invalid_state, "engine is not idle"};
  if (max_new_tokens == 0 || max_new_tokens > limits_.max_new_tokens || prompt_token_ids.empty() ||
      prompt_token_ids.size() > limits_.max_prompt_tokens) {
    return EngineProtocolError{EngineProtocolErrorCode::invalid_argument, "generation request exceeds protocol limits"};
  }
  // The embeddings path is protocol-visible: whitespace or control bytes would split the
  // line (#480 — the pin's own rule for the encoder's file names).
  if (!embeddings.empty() &&
      (embeddings.find_first_of(" \t\r\n") != std::string_view::npos || embeddings.size() > kProtocolMaxLineBytes / 2U)) {
    return EngineProtocolError{EngineProtocolErrorCode::invalid_argument, "embeddings path is not a protocol field"};
  }
  std::string command = embeddings.empty() ? "GEN " : "GENI ";
  command += std::to_string(max_new_tokens) + render_sampling_keys(sampling, flavor_ == EngineProtocolFlavor::maya_118);
  // A request that asks to be repeatable (a seed, or temperature 0 asked for) keeps its expert-cache misses on the
  // GPU on the GLM engine: the CPU lane's experts round differently from the GPU's, and which ones it takes depends
  // on the cache earlier requests left, so the same request would not repeat. It costs that request the lane's speed.
  const bool repeatable = (sampling.seed.has_value() && *sampling.seed > 0U) ||
                          (sampling.temperature.has_value() && *sampling.temperature <= 0.0);
  if (flavor_ == EngineProtocolFlavor::maya_118 && repeatable) command += " pcie_frac=1";
  command += ' ';
  for (std::size_t index = 0; index < prompt_token_ids.size(); ++index) {
    if (index != 0) command.push_back(',');
    command += std::to_string(prompt_token_ids[index]);
    if (command.size() > effective_max_line_bytes_) {
      return EngineProtocolError{EngineProtocolErrorCode::invalid_argument, "generation command exceeds line limit"};
    }
  }
  if (!embeddings.empty()) {
    command.push_back(' ');
    command.append(embeddings);
    if (command.size() > effective_max_line_bytes_) {  // the tail rides the same cap (SP3 DOS rule, extended)
      return EngineProtocolError{EngineProtocolErrorCode::invalid_argument, "generation command exceeds line limit"};
    }
  }
  if (const auto error = write_command(command, write_deadline); error.has_value()) return error;
  active_generation_ = EngineGeneration{};
  stop_requested_ = false;
  state_ = State::generating;
  return std::nullopt;
}

GenerationEvent EngineProtocolClient::next_event(const std::chrono::milliseconds timeout) {
  if (state_ != State::generating) return EngineProtocolError{EngineProtocolErrorCode::invalid_state, "no generation is active"};
  const auto response = transport_.read_line(timeout);
  if (response.kind == EngineReadKind::timeout) {
    return EngineProtocolError{EngineProtocolErrorCode::timeout, "engine generation read timed out"};
  }
  if (response.kind == EngineReadKind::closed) return *fail(EngineProtocolErrorCode::closed, "engine closed during generation");
  if (response.kind == EngineReadKind::failure) {
    return *fail(EngineProtocolErrorCode::transport_read_failed,
                 response.message.empty() ? "engine generation read failed" : response.message);
  }
  if (const auto validation = validate_incoming(response.line); validation.has_value()) {
    return *fail(validation->code, validation->message);
  }
  if (is_engine_fatal_line(response.line)) {
    return *fail(EngineProtocolErrorCode::fatal_engine_line, "engine reported a fatal verification failure");
  }
  if (response.line.rfind("T ", 0) == 0) {
    std::uint32_t token = 0;
    if (!parse_decimal(std::string_view(response.line).substr(2), token)) {
      return *fail(EngineProtocolErrorCode::malformed_line, "engine token line is invalid");
    }
    if (active_generation_.token_ids.size() >= limits_.max_generated_tokens) {
      return *fail(EngineProtocolErrorCode::response_limit, "engine exceeded generated-token limit");
    }
    active_generation_.token_ids.push_back(token);
    return TokenEvent{token};
  }
  if (response.line.rfind("PP ", 0) == 0) {
    // PP is a private prefill-progress heartbeat, never client-visible
    // output. Both engine profiles emit it, with different semantics:
    //
    // Strata (unchanged from v0.1.44.1 through v0.2.2): `generate.cpp`
    // prints
    // `PP <position reached> <prompt tokens> <ms> <fresh tokens/s>` after
    // every batched prompt chunk (`%lld %lld %.0f %.1f`, :9757/:13033/
    // :13094/:13948 at v0.2.2), and the
    // `qwen35moe` engine (`q36_engine.cu`) prints `PP <done> <fresh> <ms>
    // <tokens/s>` where the second field counts only the tokens not reused
    // from the held state. Upstream's own server accepts any `PP` with two
    // decimal fields (`serve/server.py`). The frontend accepts the same
    // family of shapes, bounded: 3 to 5 fields, two decimal counters,
    // optional non-negative metrics. No ordering between the two counters
    // is assumed, because `fresh` is not a total.
    //
    // GLM (startup-selected LightTransport profile): exactly
    // `PP <read> <total>` or `PP <read> <total> <ms> <tokens/s>` with
    // `0 < total` and `read <= total` (SP6 research §4; strict on purpose,
    // the engine is built and audited independently).
    const auto progress = fields(response.line);
    std::uint32_t first = 0;
    std::uint32_t second = 0;
    const bool metrics_ok = (progress.size() < 4U || nonnegative_decimal_metric(progress[3])) &&
                            (progress.size() < 5U || nonnegative_decimal_metric(progress[4]));
    const bool counters_ok = progress.size() >= 3U &&
                             parse_decimal(progress[1], first) && parse_decimal(progress[2], second);
    bool accepted = false;
    if (flavor_ == EngineProtocolFlavor::maya_118) {
      // Maya v1.1.21: the engine prints `PP <done> <total> <ms> <tok/s> <reuse>`
      // per prompt chunk (maya-ref src/program/generate.cpp:6102, :6667) and
      // its own pump accepts any `PP` with decimal fields 1-2 and an optional
      // float/reuse tail (serve/server.py:835-847). Bounded like the other
      // flavors — 3 to 6 fields, decimal counters, optional non-negative
      // metrics, optional decimal reuse; no ordering between the counters is
      // assumed (Maya assumes none).
      std::uint32_t reused = 0;
      accepted = counters_ok && metrics_ok && progress.size() <= 6U &&
                 (progress.size() < 6U || parse_decimal(progress[5], reused));
    } else if (flavor_ == EngineProtocolFlavor::lighttransport_glm53) {
      accepted = counters_ok && metrics_ok && progress.size() >= 3U && progress.size() <= 5U && progress.size() != 4U &&
                 second != 0U && first <= second;
    } else if (flavor_ == EngineProtocolFlavor::unified) {
      // The merged engine's dialect (design §3.3): exactly `PP <done> <total> <ms> <tok_s>` — four
      // fields, two decimal counters then two non-negative decimal metrics. Strict, fail-closed:
      // the superset carries no shorter or longer PP, so anything else closes the protocol.
      accepted = counters_ok && metrics_ok && progress.size() == 5U;
    } else {
      accepted = counters_ok && metrics_ok && progress.size() <= 5U;
    }
    if (!accepted) {
      return *fail(EngineProtocolErrorCode::unexpected_line,
                   "engine emitted an invalid or unsupported prefill progress line");
    }
    return ProgressEvent{};
  }
  if (flavor_ == EngineProtocolFlavor::maya_118 && response.line.rfind("NOTE ", 0) == 0) {
    // Maya prints what happens to the kept conversations as `NOTE <text>`
    // (maya-ref serve/server.py:851, :457, :559, :1028): its own server only
    // logs the text. We consume it as a ProgressEvent — never client output,
    // never a protocol error — bounded: a non-empty record after `NOTE `.
    if (fields(response.line).size() < 2U) {
      return *fail(EngineProtocolErrorCode::unexpected_line, "engine emitted an empty note record");
    }
    return ProgressEvent{};
  }
  if ((flavor_ == EngineProtocolFlavor::maya_118 || flavor_ == EngineProtocolFlavor::unified) &&
      response.line.rfind("STAT ", 0) == 0) {
    // The GLM engine's live expert-tier counters, `STAT key=value ...` every
    // few tokens (maya-ref src/program/generate.cpp:2014, :2360; the pump's
    // parse at serve/server.py:677-690, consumed for the Monitor and never
    // output). The merged engine (design §3.3) emits the same optional cadence.
    // Cadence record: consumed as a ProgressEvent, and fail-closed
    // on shape — at least one pair, every field carrying a non-empty key
    // before its `=`.
    const auto pairs = fields(response.line);
    if (pairs.size() < 2U) {
      return *fail(EngineProtocolErrorCode::unexpected_line, "engine emitted an empty stat record");
    }
    for (std::size_t i = 1; i < pairs.size(); ++i) {
      const auto equals = pairs[i].find('=');
      if (equals == std::string_view::npos || equals == 0U) {
        return *fail(EngineProtocolErrorCode::unexpected_line, "engine emitted a malformed stat record");
      }
    }
    capture_engine_readings(pairs);
    return ProgressEvent{};
  }
  if ((response.line.rfind("RESUME ", 0) == 0 || response.line.rfind("REUSED ", 0) == 0) &&
      flavor_ == EngineProtocolFlavor::strata_0143) {
    // RESUME/REUSED are private prompt-progress records of the main engine
    // family, printed for every accepted GEN and unconditionally: `RESUME <n>`
    // before reading the prompt (v0.1.44.1 generate.cpp:11214, v0.2.1.3
    // :12809, v0.2.2 :12899) and `REUSED <n>` once it is read (:11849 / :13910
    // / :14006). Upstream's
    // own server consumes them (serve/server.py). Like PP they are never
    // client output and must never fail a request; the qwen35moe engine
    // prints neither, and YIELDED needs the --batch/--slots flags the
    // frontend refuses. Bounded grammar, fail-closed otherwise; the GLM
    // profile keeps its exact record and never reaches this branch.
    // Surfaced as a ProgressEvent: invisible to every caller's output path.
    const auto progress = fields(response.line);
    std::uint32_t reused = 0;
    if (progress.size() != 2U || !parse_decimal(progress[1], reused)) {
      return *fail(EngineProtocolErrorCode::unexpected_line, "engine emitted an invalid or unsupported prompt progress line");
    }
    return ProgressEvent{};
  }
  if (response.line.rfind("DONE ", 0) == 0 && response.line.size() > 5) {
    const auto finish = finish_literal(fields(response.line));
    if (!finish) return *fail(EngineProtocolErrorCode::malformed_line, "engine DONE line is invalid");
    active_generation_.done_line = response.line;
    state_ = State::idle;
    // A nonfinite finish means the engine's NaN guard tripped: no usable
    // output, but the engine is alive and resets on the next GEN, so it is
    // not counted as a successful generation (SP0 E3).
    if (*finish != "nonfinite") completed_generations_.fetch_add(1U);
    return DoneEvent{*finish, active_generation_.token_ids.size(), done_figures(fields(response.line))};
  }
  if (response.line.rfind("ERR", 0) == 0 && (response.line.size() == 3 || response.line[3] == ' ')) {
    return *fail(EngineProtocolErrorCode::engine_error, response.line);
  }
  return *fail(EngineProtocolErrorCode::unexpected_line, "engine emitted an unexpected generation line");
}

std::optional<EngineProtocolError> EngineProtocolClient::read_generation(const std::chrono::milliseconds timeout,
                                                                          EngineGeneration& generation) {
  if (state_ != State::generating) return EngineProtocolError{EngineProtocolErrorCode::invalid_state, "no generation is active"};
  const auto deadline = deadline_after(timeout);
  for (;;) {
    const auto remaining = remaining_until(deadline);
    if (remaining.count() == 0) {
      generation = active_generation_;
      return EngineProtocolError{EngineProtocolErrorCode::timeout, "engine generation read timed out"};
    }
    auto event = next_event(remaining);
    if (auto* error = std::get_if<EngineProtocolError>(&event)) {
      generation = active_generation_;
      return std::move(*error);
    }
    if (const auto* done = std::get_if<DoneEvent>(&event)) {
      generation = active_generation_;
      // A nonfinite finish fails the request but not the protocol (SP0 E3).
      if (done->finish == "nonfinite") return EngineProtocolError{EngineProtocolErrorCode::engine_error, "engine reported a non-finite result"};
      return std::nullopt;
    }
  }
}

std::optional<EngineProtocolError> EngineProtocolClient::stop(const EngineStopCause cause) {
  if (state_ != State::generating) return EngineProtocolError{EngineProtocolErrorCode::invalid_state, "no generation is active"};
  if (!supports_stop_) return EngineProtocolError{EngineProtocolErrorCode::invalid_state, "engine did not advertise STOP support"};
  if (stop_requested_) return std::nullopt;
  if (const auto error = write_command("STOP", deadline_after(config_defaults::kStdinWriteDeadline)); error.has_value()) return error;
  stop_requested_ = true;
  stop_cause_ = cause;
  return std::nullopt;
}

std::optional<EngineProtocolError> EngineProtocolClient::quit() {
  if (state_ == State::closed) return std::nullopt;
  if (state_ == State::failed) return EngineProtocolError{EngineProtocolErrorCode::invalid_state, "engine protocol has failed"};
  if (const auto error = write_command("QUIT", deadline_after(config_defaults::kStdinWriteDeadline)); error.has_value()) return error;
  state_ = State::closed;
  return std::nullopt;
}

void EngineProtocolClient::abandon() noexcept {
  state_ = State::failed;
  if (!failure_code_) failure_code_ = EngineProtocolErrorCode::abandoned;  // ignored STOP: drain failed (§3.5(3))
}

bool EngineProtocolClient::ready() const noexcept { return state_ == State::idle || state_ == State::generating; }
bool EngineProtocolClient::generating() const noexcept { return state_ == State::generating; }
bool EngineProtocolClient::idle() const noexcept { return state_ == State::idle; }
bool EngineProtocolClient::closed() const noexcept { return state_ == State::closed || state_ == State::failed; }

}  // namespace secure_frontend
