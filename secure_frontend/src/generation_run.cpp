#include "secure_frontend/generation_run.hpp"

#include "secure_frontend/utf8_stream_decoder.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

using Clock = std::chrono::steady_clock;

std::chrono::milliseconds left_until(const Clock::time_point deadline) {
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
  return left.count() > 0 ? left : std::chrono::milliseconds(0);
}

// Longest prefix of valid UTF-8 `text` of at most `limit` bytes ending on a code-point boundary.
std::size_t code_point_prefix(const std::string_view text, std::size_t limit) {
  if (limit >= text.size()) return text.size();
  while (limit > 0U && (static_cast<unsigned char>(text[limit]) & 0xC0U) == 0x80U) --limit;
  return limit;
}

// turn_end: a server stop token ended the reply (GenerationRunLimits::server_stop_tokens), the STOP only ends the engine.
enum class StopCause { none, timeout, bytes, cancel, undecodable, repeat, turn_end };

}  // namespace

GenerationRunResult drive_generation(EngineProtocolClient& protocol, const TokenByteDecoder& decoder,
                                     const GenerationRunLimits& limits, GenerationObserver& observer) {
  GenerationRunResult result;
  Utf8StreamDecoder utf8;
  std::size_t released = 0;
  StopCause cause = StopCause::none;
  const auto generation_deadline = Clock::now() + limits.generation_timeout;
  // SP5 M1 Task 8 (engine_silence_s; serve/server.py:274-276): the last moment the engine
  // said anything valid (a token, a progress line, DONE). A timeout slice is not activity.
  auto last_activity = Clock::now();
  // SP5 M1 Task 8 (repeat_stop_tokens; serve/server.py:4908): the run of one repeated id.
  std::uint32_t run_id = 0;
  std::size_t run_len = 0;
  Clock::time_point drain_deadline{};
  auto last_tick = Clock::now();
  // Issue 19, the reasoning split (limits.think_tokens): which channel decoded text goes
  // to, and whether the newlines right after a </think> are still to be dropped.
  bool reasoning = limits.think_tokens.has_value() && limits.starts_in_reasoning;
  bool after_close = false;
  // Issue 26: an end-of-turn token was seen; the reply is over, the rest until DONE is dropped.
  bool turn_ended = false;
  // Gemma 4 plan 1 Task 9 (limits.channel_name): an open tag was seen and its channel-name line
  // is still being read; the line is held here, never output.
  bool naming = false;
  std::string name_line;

  // false: STOP could not be written (the protocol is abandoned).
  const auto send_stop = [&](const StopCause why) {
    cause = why;
    // Shutdown (R-H): the drain is bounded by min(stop_drain, kShutdownDrainBudget), as M1's loop did.
    const bool shutting = why == StopCause::cancel && result.cancel == RunCancel::shutting_down;
    drain_deadline = Clock::now() + (shutting ? std::min(limits.stop_drain_timeout, limits.shutdown_drain) : limits.stop_drain_timeout);
    // Ruling t5-stopcause: the SP2 kill-reason contract (KillReason::client_stop_drain_failed
    // follows stop_cause() == client_cancelled) survives the shared loop.
    const EngineStopCause stop_cause = why == StopCause::cancel && result.cancel == RunCancel::client_gone
                                           ? EngineStopCause::client_cancelled
                                           : EngineStopCause::generation_timeout;
    if (protocol.stop(stop_cause).has_value()) {
      protocol.abandon();
      return false;
    }
    return true;
  };
  // Hands text to the channel it was decoded in (reasoning or the answer).
  const auto emit = [&](const std::string_view text, const bool as_reasoning) {
    if (as_reasoning) observer.on_reasoning(text);
    else observer.on_text(text);
  };
  // Releases decoded text within the byte cap (one cap over both channels); on
  // reaching it, cuts on a code point and (while the engine still generates) sends STOP.
  const auto release = [&](std::string text, const bool as_reasoning) {
    if (cause != StopCause::none || text.empty()) return true;
    if (!as_reasoning && after_close && !limits.keeps_newlines_after_think) {
      // Upstream's lead rule (serve/frontend.py:1279-1284): newlines only, spaces stay (not MiniMax's, Maya keeps them).
      const auto first = text.find_first_not_of('\n');
      if (first == std::string::npos) return true;
      text.erase(0, first);
      after_close = false;
    }
    if (released + text.size() > limits.max_generated_bytes) {
      text.resize(code_point_prefix(text, limits.max_generated_bytes - released));
      result.bytes_capped = true;
      if (!text.empty()) {
        released += text.size();
        emit(text, as_reasoning);
      }
      if (!protocol.generating()) {
        cause = StopCause::bytes;
        return true;
      }
      return send_stop(StopCause::bytes);
    }
    released += text.size();
    emit(text, as_reasoning);
    return true;
  };
  // Gemma 4 plan 1 Task 9 (limits.channel_name, spec §6.4): decoded text after an open tag goes
  // to the channel-name line first. The line must be exactly the name and a newline within
  // kChannelNameLineBytes; it is dropped and what follows it is reasoning. Any other line, or none
  // in the bound, ends the reply as undecodable (STOP and drain). Never output, never capped.
  const auto read_name = [&](const std::string& text) {
    name_line += text;  // bounded: at most one token's bytes past kChannelNameLineBytes
    const std::size_t newline = name_line.find('\n');
    if (newline == std::string::npos) return name_line.size() < kChannelNameLineBytes ? true : send_stop(StopCause::undecodable);
    if (newline >= kChannelNameLineBytes || std::string_view{name_line}.substr(0U, newline) != *limits.channel_name) {
      return send_stop(StopCause::undecodable);
    }
    naming = false;
    std::string rest = name_line.substr(newline + 1U);
    name_line.clear();
    return release(std::move(rest), reasoning);
  };

  for (;;) {
    if (cause == StopCause::none && Clock::now() >= generation_deadline) {
      result.timed_out = true;
      if (!send_stop(StopCause::timeout)) return result;  // engine_unavailable
    }
    // SP5 M1 Task 8 (engine_silence_s): a stalled engine — nothing valid for silence_s —
    // reaches the same stall path (STOP, drain, timed-out length) at the silence bound,
    // before the total generation deadline (serve/server.py:274-276, :2861-2868).
    if (cause == StopCause::none && limits.silence.count() > 0 && Clock::now() - last_activity >= limits.silence) {
      result.timed_out = true;
      if (!send_stop(StopCause::timeout)) return result;  // engine_unavailable
    }
    if (cause != StopCause::none && Clock::now() >= drain_deadline) {
      protocol.abandon();  // the engine ignored STOP: SP2 drain_failed
      return result;       // engine_unavailable
    }
    const auto budget = std::min(limits.slice, left_until(cause == StopCause::none ? generation_deadline : drain_deadline));
    auto event = protocol.next_event(std::max(budget, std::chrono::milliseconds(1)));
    bool progress = false;
    bool idle_slice = false;
    if (const auto* failure = std::get_if<EngineProtocolError>(&event)) {
      if (failure->code != EngineProtocolErrorCode::timeout) {
        result.end = failure->code == EngineProtocolErrorCode::engine_error || failure->code == EngineProtocolErrorCode::fatal_engine_line
                         ? RunEnd::engine_error
                         : RunEnd::engine_unavailable;
        return result;
      }
      idle_slice = true;
    } else if (const auto* token = std::get_if<TokenEvent>(&event)) {
      ++result.completion_tokens;
      if (limits.meter != nullptr) limits.meter->on_token(result.completion_tokens);
      last_activity = Clock::now();  // a token is the engine speaking
      if (limits.max_tokens != 0U && result.completion_tokens > limits.max_tokens) {
        protocol.abandon();  // more tokens than GEN allowed: not trusted further (M1: engine_error)
        result.end = RunEnd::engine_error;
        return result;
      }
      const bool server_end =
          std::find(limits.server_stop_tokens.begin(), limits.server_stop_tokens.end(), token->id) != limits.server_stop_tokens.end();
      const bool end_of_turn =
          server_end || std::find(limits.stop_tokens.begin(), limits.stop_tokens.end(), token->id) != limits.stop_tokens.end();
      if (cause == StopCause::none && !turn_ended && end_of_turn && naming) {
        // Gemma 4 plan 1 Task 9: the turn ended inside the channel-name line. Fail closed; the
        // STOP may reach an engine that already ended its turn, which is harmless (the strata
        // and Gemma serve loops clear a stale STOP at the next GEN), and the drain reads DONE.
        if (!send_stop(StopCause::undecodable)) return result;  // engine_unavailable
      } else if (cause == StopCause::none && !turn_ended && end_of_turn) {
        // Never output; a held partial character ends as one U+FFFD, as at a stop end.
        if (!release(utf8.finish(false), reasoning)) return result;  // engine_unavailable
        turn_ended = true;
        // The engine does not stop on it by itself: one STOP, then the drain (the run still ends as stop).
        if (server_end && !send_stop(StopCause::turn_end)) return result;  // engine_unavailable
      } else if (cause == StopCause::none && !turn_ended) {
        if (token->id == run_id) ++run_len;
        else {
          run_id = token->id;
          run_len = 1;
        }
        const bool think_open = limits.think_tokens.has_value() && token->id == limits.think_tokens->open;
        const bool think_close = limits.think_tokens.has_value() && token->id == limits.think_tokens->close;
        const auto literal = std::find_if(limits.literal_tokens.begin(), limits.literal_tokens.end(),
                                          [&token](const auto& entry) { return entry.first == token->id; });
        bool ok = true;
        if ((think_open || think_close) && naming) {
          ok = send_stop(StopCause::undecodable);  // Gemma 4 plan 1 Task 9: a tag inside the name line
        } else if (think_open || think_close) {
          // A tag is never output. It ends its channel: a held partial character becomes
          // one U+FFFD there (as at a stop end) and never crosses into the other channel.
          ok = release(utf8.finish(false), reasoning);
          const bool ended = reasoning && think_close;
          reasoning = think_open;
          after_close = think_close;
          if (ended && ok && cause == StopCause::none) observer.on_reasoning_end();
          if (think_open && limits.channel_name.has_value()) {
            naming = true;  // Gemma 4 plan 1 Task 9: the channel-name line comes first
            name_line.clear();
          }
        } else if (literal != limits.literal_tokens.end() && !naming) {
          ok = release(utf8.push(literal->second), reasoning);  // web search: the tag's own text, in its channel
        } else {
          const auto bytes = decoder.bytes_for(token->id);
          if (!std::holds_alternative<std::string>(bytes)) ok = send_stop(StopCause::undecodable);
          else if (naming) ok = read_name(utf8.push(std::get<std::string>(bytes)));
          else ok = release(utf8.push(std::get<std::string>(bytes)), reasoning);
        }
        if (!ok) return result;  // engine_unavailable
        // SP5 M1 Task 8 (repeat_stop_tokens, serve/server.py:4908, :5088): the pin's own
        // runaway detector — this many of one token in a row end the reply. The run is kept
        // (it is already released above), everything after it is dropped by the STOP+drain.
        if (limits.repeat_stop_tokens != 0U && run_len >= limits.repeat_stop_tokens) {
          if (!send_stop(StopCause::repeat)) return result;  // engine_unavailable
        }
      }
    } else if (std::holds_alternative<ProgressEvent>(event)) {
      progress = true;
      last_activity = Clock::now();  // a progress line is the engine speaking (pin: "nothing valid")
      if (limits.meter != nullptr) limits.meter->on_engine_stats(protocol.engine_readings());
    } else {
      const auto& done = std::get<DoneEvent>(event);
      if (limits.meter != nullptr) limits.meter->on_done(done.figures);
      if (cause == StopCause::none && !turn_ended && !naming) (void)release(utf8.finish(done.finish != "stop"), reasoning);
      // Gemma 4 plan 1 Task 9: the engine's own stop inside the channel-name line fails closed
      // (no STOP: the engine is already idle); a length or cancel end there outputs none of it.
      if (cause == StopCause::none && !turn_ended && naming && done.finish == "stop") cause = StopCause::undecodable;
      if (done.finish == "nonfinite") {
        result.end = RunEnd::engine_error;
        return result;
      }
      switch (cause) {
        case StopCause::none: result.end = turn_ended || done.finish == "stop" ? RunEnd::stop : RunEnd::length; break;
        case StopCause::turn_end: result.end = RunEnd::stop; break;
        case StopCause::timeout:
        case StopCause::bytes:
        case StopCause::repeat: result.end = RunEnd::length; break;  // the pin's "ended as length" (:5088)
        case StopCause::cancel: result.end = RunEnd::cancelled; break;
        case StopCause::undecodable: result.end = RunEnd::undecodable; break;
      }
      return result;
    }
    const auto now = Clock::now();
    if (progress || idle_slice || now - last_tick >= limits.slice) {
      last_tick = now;
      const RunCancel cancel = observer.on_tick(progress);
      if (cancel != RunCancel::none && cause == StopCause::none) {
        result.cancel = cancel;
        if (!send_stop(StopCause::cancel)) return result;  // engine_unavailable
      }
    }
  }
}

}  // namespace secure_frontend
