#pragma once

#include "secure_frontend/deadline.hpp"
#include "secure_frontend/sampling_params.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace secure_frontend {

// The process layer deliberately does not expose stdin/stdout handles.  This
// narrow interface permits a pipe-backed implementation later while keeping
// protocol parsing independently testable and refusing ambiguous I/O results.
// Hard ceiling on any outbound protocol line (the GEN record), shared by the
// protocol client's READY-derived cap and the stdio transport's write cap.
inline constexpr std::size_t kProtocolMaxLineBytes = 16U * 1024U * 1024U;

enum class EngineReadKind { line, timeout, closed, failure };

struct EngineReadResult final {
  EngineReadKind kind;
  std::string line;
  std::string message;
};

class EngineLineTransport {
 public:
  virtual ~EngineLineTransport() = default;
  // `line` never contains a newline. Implementations must write it as one
  // atomic protocol record (line plus LF) or report failure. `deadline` is the
  // absolute overall bound for the whole record (spec §3.10, owner ruling
  // 2026-10-14): a write still incomplete at `deadline` fails exactly like a
  // chunk that misses its per-chunk deadline. No write is ever unbounded.
  [[nodiscard]] virtual bool write_line(std::string_view line, MonotonicClock::time_point deadline,
                                        std::string& error) = 0;
  [[nodiscard]] virtual EngineReadResult read_line(std::chrono::milliseconds timeout) = 0;
};

enum class EngineProtocolErrorCode {
  invalid_argument,
  invalid_state,
  transport_write_failed,
  transport_read_failed,
  timeout,
  closed,
  line_too_long,
  malformed_line,
  unexpected_line,
  engine_error,
  response_limit,
  fatal_engine_line,
  abandoned,
};

// The engine's fatal verification lines (upstream/serve/server.py:734-735
// FATAL_PREFIXES), matched as prefixes. The engine lives on but is unusable;
// a trailing DONE is never accepted (upstream _pump, upstream/serve/server.py:952-967).
inline constexpr std::array<std::string_view, 4> kEngineFatalPrefixes{
    "ERR verify: timed out at layer ",
    "ERR verify batch: timed out at layer ",
    "ERR verify: an earlier window never finished",
    "ERR verify batch: an earlier window never finished",
};

[[nodiscard]] bool is_engine_fatal_line(std::string_view line) noexcept;

// SP3 B6: the GEN sampling keys for these params — " temperature=0.5 top_p=0.9
// seed=42", shortest round-trip form, "" when nothing is set (engine stays
// greedy). Values are already bounded by the chat schema (finite, in range).
// zero_window_disables: the GLM engine's semantics for a window of 0 (no penalty at
// all — generate.cpp:3467 pen_on at the pin, maya serve/server.py:274-275); the strata
// pin instead renders its default window of 64. Only the maya_118 flavor passes true.
[[nodiscard]] std::string render_sampling_keys(const SamplingParams& sampling, bool zero_window_disables = false);

struct EngineProtocolError final {
  EngineProtocolErrorCode code;
  std::string message;
};

// SP3 M2 (spec §3.3): one generation record at a time. `finish` is always one
// of the static literals stop|length|cancel|nonfinite.
struct TokenEvent final {
  std::uint32_t id;
};
// The engine's own figures from a full DONE record, `DONE <generated> <prompt> <prompt_ms>
// <decode_ms> <finish> [<drafts_accepted> <drafts_offered> <reused> ...]` (generate.cpp; the
// fields upstream's /metrics reads, serve/server.py _parse_done). For the Monitor
// (listener.monitor, session log step 16) only: a short record leaves them all empty, and a
// field that is not a number leaves that one empty. They never decide the record's validity.
struct DoneFigures final {
  std::optional<std::size_t> prompt_tokens{};
  std::optional<double> prompt_ms{};
  std::optional<double> decode_ms{};
  std::optional<std::size_t> reused{};   // prompt tokens the engine kept from the last turn
};
struct DoneEvent final {
  std::string_view finish;
  std::size_t generated;  // this generation's T records
  DoneFigures figures{};
};
struct ProgressEvent final {};
using GenerationEvent = std::variant<TokenEvent, DoneEvent, ProgressEvent, EngineProtocolError>;

struct EngineProtocolLimits final {
  // 0 means "derive from READY": min(32 + 11 * max_context_tokens, kProtocolMaxLineBytes)
  // (10 digits + separator per id).
  // A non-zero value is a fixed cap for both outbound commands and inbound records.
  // With 0, inbound records are capped at kDefaultInboundLineBytes (engine records are short).
  std::size_t max_line_bytes{0};
  // 1,048,576: upstream Strata runs contexts up to 1M with YaRN rope scaling (session log
  // step 13); a GEN line of that many ids still fits kProtocolMaxLineBytes.
  std::size_t max_prompt_tokens{1048576};
  std::size_t max_generated_tokens{1048576};
  std::uint32_t max_new_tokens{1048576};
};

// The original Strata profile has no prefill progress record. The separately
// built LightTransport GLM-5.3 engine may emit its documented PP progress
// record while it reads a prompt. The Maya GLM engine (v1.1.8 through
// v1.1.21) frames the same conversation additionally with NOTE conversation
// notes and the STAT expert-tier cadence, and prints the five-field PP (D-8).
// These are three explicit engine-wire contracts, one per flavor, selected at
// construction — never auto-detected from untrusted output, and each flavor
// fails closed on the other flavors' records.
enum class EngineProtocolFlavor {
  strata_0143,           // upstream Strata v0.2.2 (RESUME/REUSED, 3-5 field PP)
  lighttransport_glm53,  // the SP6-built LightTransport engine (exact 3- or 5-field PP)
  maya_118,              // Project Maya's engine, v1.1.8 through v1.1.21 (NOTE, STAT cadence,
                         // 3-6 field PP; v1.1.21's pre-READY VRAM/NOTE records). The name
                         // keeps the first pinned version: the v1.1.21 records are additive.
  unified,               // the merged unified engine (design §3.3, decision 3): one converged
                         // superset dialect — READY/GEN/T/DONE plus a strict 4-field PP, an
                         // optional STAT cadence, and ERR. No NOTE, no RESUME/REUSED; every
                         // other record (including the other flavors' private ones) fails closed.
};

// Who asked for a STOP (SP3 D-2). The frontend's own deadline
// (ChatExecutionService's generation_timeout) is `generation_timeout`; SP3's
// cancellation for a client that went away (SP3 spec §2.6) is `client_cancelled`.
enum class EngineStopCause { generation_timeout, client_cancelled };

struct EngineReady final {
  std::uint32_t max_context_tokens;
  bool supports_stop;
};

struct EngineGeneration final {
  std::vector<std::uint32_t> token_ids;
  std::string done_line;
};

// Serial, fail-closed client for Strata's documented solo --serve protocol:
// READY <context> [stop], GEN <max_new> <comma-separated token ids>, T <id>,
// DONE ..., ERR ..., STOP and QUIT. The explicitly selected flavor also
// recognizes its own bounded private progress records — LightTransport GLM
// the 3/5-field PP heartbeat, Maya (v1.1.8 through v1.1.21) the NOTE and
// STAT cadence plus its PP form — and nothing else: records unknown to the
// selected flavor are rejected rather than guessed at.
class EngineProtocolClient final {
 public:
  explicit EngineProtocolClient(EngineLineTransport& transport,
                                EngineProtocolLimits limits = {},
                                EngineProtocolFlavor flavor = EngineProtocolFlavor::strata_0143);

  [[nodiscard]] std::optional<EngineProtocolError> await_ready(std::chrono::milliseconds timeout,
                                                                 EngineReady& ready);
  // `write_deadline` bounds the whole GEN write (the request's generation
  // deadline, supplied by ChatExecutionService); a miss fails the protocol with
  // transport_write_failed, which the supervisor kills as write_timeout.
  // SP3 (B6, ruling T15-signature): `sampling` carries the schema-bounded
  // temperature/top_p/seed keys between max_new and the ids; empty = greedy.
  // SP5 M1 Task 12 (Ruling T12-i): `embeddings` non-empty renders the pin's GENI
  // variant — the same head and ids, with the combined image-embeddings file at the
  // END of the line (serve/server.py:1778). Empty renders today's GEN byte-for-byte.
  [[nodiscard]] std::optional<EngineProtocolError> begin_generation(
      std::uint32_t max_new_tokens, const std::vector<std::uint32_t>& prompt_token_ids,
      MonotonicClock::time_point write_deadline, const SamplingParams& sampling = {}, std::string_view embeddings = {});
  // Reads until DONE. `timeout` is the TOTAL budget for this call, not a
  // per-line allowance. A timeout leaves a generation active so the caller may
  // issue STOP and continue draining; all malformed/terminal failures close it.
  // A DONE whose finish field is `nonfinite` returns engine_error but leaves
  // the client idle: the engine is alive and resets on the next GEN.
  [[nodiscard]] std::optional<EngineProtocolError> read_generation(
      std::chrono::milliseconds timeout, EngineGeneration& generation);
  // Reads at most one record within `timeout` (a per-call budget, SP3 §3.3).
  // Timeout: EngineProtocolError{timeout}, generation still active. DONE (also
  // DONE ... nonfinite) returns to idle. Any other failure closes the protocol.
  [[nodiscard]] GenerationEvent next_event(std::chrono::milliseconds timeout);
  // Sends STOP once per generation; `cause` is recorded only when STOP was
  // written and is forgotten by the next begin_generation() call, even a refused one.
  // STOP and QUIT are single-chunk control records sent after (or without) a
  // request deadline: each write is bounded overall by one per-chunk deadline
  // (config_defaults::kStdinWriteDeadline) from the call.
  [[nodiscard]] std::optional<EngineProtocolError> stop(EngineStopCause cause = EngineStopCause::generation_timeout);
  [[nodiscard]] std::optional<EngineProtocolError> quit();
  // Marks the protocol failed without writing anything, e.g. when the engine
  // ignored STOP and cannot be trusted with further commands.
  void abandon() noexcept;

  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] bool generating() const noexcept;
  // True only when the engine is between generations and accepts GEN.
  [[nodiscard]] bool idle() const noexcept;
  [[nodiscard]] bool closed() const noexcept;
  // The code that failed the protocol (set by every terminal failure), or nullopt.
  [[nodiscard]] std::optional<EngineProtocolErrorCode> failure_code() const noexcept { return failure_code_; }
  // Monitor (session log, Monitor issues): the engine's own startup and cadence records —
  // the INFO line before READY (upstream generate.cpp:10933; maya-ref pre-READY INFO) and
  // the maya_118 `STAT key=value` expert-tier cadence (generate.cpp:2014) — kept as the
  // LAST value seen per key. Bounded (96 keys, short keys/values); never parsed further:
  // consumers (ServeMonitor's whitelist) own the meaning.
  [[nodiscard]] const std::map<std::string, std::string>& engine_readings() const noexcept { return engine_readings_; }
  // Why STOP was sent for the current (or last) generation; nullopt if none was.
  [[nodiscard]] std::optional<EngineStopCause> stop_cause() const noexcept { return stop_cause_; }
  // Generations that ended with a successful DONE (a `nonfinite` DONE is not
  // one). Atomic: the supervisor thread reads it while a Lease holder may use
  // the client (SP3 D-2 streak reset); everything else here is single-threaded.
  [[nodiscard]] std::uint64_t completed_generations() const noexcept { return completed_generations_.load(); }

 private:
  enum class State { waiting_ready, idle, generating, closed, failed };

  EngineLineTransport& transport_;
  EngineProtocolLimits limits_;
  EngineProtocolFlavor flavor_;
  State state_{State::waiting_ready};
  EngineGeneration active_generation_;
  bool supports_stop_{false};
  bool stop_requested_{false};
  std::optional<EngineStopCause> stop_cause_;
  std::atomic<std::uint64_t> completed_generations_{0};
  std::optional<EngineProtocolErrorCode> failure_code_;
  // Last key=value pairs seen in INFO (pre-READY, both flavors) and, on the maya_118
  // flavor, the STAT cadence; bounded in the .cpp capture helper; Monitor-only.
  std::map<std::string, std::string> engine_readings_{};
  // Records `key=value` fields into engine_readings_ (last value wins per key). Bounded:
  // at most 96 keys, keys 1..32 chars of [a-z0-9_], values 1..64 chars; over-cap pairs
  // are ignored (readings are Monitor sugar; the record's SHAPE is already fail-closed
  // at the call sites). The record name (field 0) is never a key.
  void capture_engine_readings(const std::vector<std::string_view>& parsed);
  // Outbound command cap; sized from READY when limits_.max_line_bytes == 0.
  std::size_t effective_max_line_bytes_;

  static constexpr std::size_t kDefaultInboundLineBytes = 8192;
  [[nodiscard]] std::size_t inbound_max_line_bytes() const noexcept;

  [[nodiscard]] std::optional<EngineProtocolError> write_command(std::string_view command,
                                                                  MonotonicClock::time_point deadline);
  [[nodiscard]] std::optional<EngineProtocolError> validate_incoming(std::string_view line) const;
  [[nodiscard]] std::optional<EngineProtocolError> fail(EngineProtocolErrorCode code,
                                                         std::string message);
};

}  // namespace secure_frontend
