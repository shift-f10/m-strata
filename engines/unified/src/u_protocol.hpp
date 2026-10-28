// strata-unified-engine: the converged "unified" wire dialect (design §3.3, decision 3).
// One strict superset grammar for the merged engine, parsed and emitted here on the host.
// Every line is ASCII-printable, non-empty, at most 8192 bytes, and split on single spaces
// (a leading, trailing or doubled space is malformed). Anything that is not exactly one of
// the records below fails closed -- in particular there is NO NOTE and NO RESUME/REUSED
// record (reuse is reported by the DONE <reused> field).
//
//   Pre-READY : INFO <key=value ...>              startup readings (stored)
//   Ready     : READY <ctx:u32> [stop]            stop => a mid-generation STOP is honored
//   Outbound  : GEN <max_new:u32> [key=value ...] <id,id,...>
//               STOP
//               QUIT
//   Inbound   : T <id:u32>                         one generated token
//               PP <done:u32> <total:u32> <ms> <tok_s>   prompt progress (dropped, counted)
//               STAT <key=value ...>               optional telemetry (dropped)
//               DONE <produced:u32> <prompt:u32> <prompt_ms> <decode_ms> <finish>
//                    <spec_hits:u32> <spec_steps:u32> <reused:u32>   terminal
//               ERR [text]                         terminal, failed
//
// `ms` / `tok_s` / `prompt_ms` / `decode_ms` are nonnegative decimals (digits and at most
// one '.'); `finish` is one of stop | length | cancel | nonfinite.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace u {

inline constexpr std::size_t kMaxLineBytes = 8192;

enum class Finish { stop, length, cancel, nonfinite };

struct KeyVal final {
  std::string key;
  std::string value;
};

struct Info final { std::vector<KeyVal> fields; };                 // INFO
struct Ready final { std::uint32_t ctx{0}; bool stop_honored{false}; };  // READY
struct Gen final {                                                 // GEN
  std::uint32_t max_new{0};
  std::vector<KeyVal> params;
  std::vector<std::uint32_t> ids;
};
struct Stop final {};                                              // STOP
struct Quit final {};                                              // QUIT
struct Token final { std::uint32_t id{0}; };                       // T
struct PromptProgress final {                                      // PP
  std::uint32_t done{0};
  std::uint32_t total{0};
  double ms{0.0};
  double tok_s{0.0};
};
struct Stat final { std::vector<KeyVal> fields; };                 // STAT
struct Done final {                                                // DONE (terminal)
  std::uint32_t produced{0};
  std::uint32_t prompt{0};
  double prompt_ms{0.0};
  double decode_ms{0.0};
  Finish finish{Finish::length};
  std::uint32_t spec_hits{0};
  std::uint32_t spec_steps{0};
  std::uint32_t reused{0};
};
struct Err final { std::string text; };                            // ERR (terminal)

using Record = std::variant<Info, Ready, Gen, Stop, Quit, Token, PromptProgress, Stat, Done, Err>;

struct ParseError final { std::string message; };

// Parses one wire line into a record. Fails closed (a ParseError) on an empty/oversized/
// non-printable line, a malformed field, a bad number, or an unknown record prefix.
[[nodiscard]] std::variant<Record, ParseError> parse_line(std::string_view line);

// Emits a record to its wire line (no trailing newline). The inverse of parse_line for every
// record a well-formed engine or client produces.
[[nodiscard]] std::string emit(const Record& r);

// True for the two terminal inbound records (DONE, ERR).
[[nodiscard]] bool is_terminal(const Record& r);

// The one record kind a client may accept before READY is INFO (READY itself ends the phase).
// Everything else before READY fails closed.
[[nodiscard]] bool allowed_before_ready(const Record& r);

[[nodiscard]] std::string_view finish_name(Finish f);

}  // namespace u
