#pragma once

// The Monitor's conversation cache (owner 2026-10-28: "upstream strata has disk read and conversation cache as well"),
// as the Qwen engine's stderr tells it: upstream's ConvCacheLog (serve/server.py:757-811) reads the same lines from
// the engine's log. The engine prints them (engine/src/program/generate.cpp :9519, :9650, :12953):
//
//   strata serve: conversation cache: parked|skipped N tokens in F ms; parked=P bytes=B evictions=E snapshot_bytes=S reused_kv_bytes=R
//   strata serve: conversation cache: restored N tokens (live|checkpoint) in F ms; parked=P bytes=B
//   strata serve: conversation cache: dropped N superseded copy|copies of this conversation; parked=P
//
// The parser takes exactly these whole lines (the tails after bytes=B may be shorter, as upstream's pattern allows:
// no evictions, or no snapshot fields) and gives back only their integers and the event's kind. Anything else -
// another stderr line, a near miss, a line with any other text in it - is refused. Nothing of a line's text is kept.

#include <cstdint>
#include <optional>
#include <string_view>

namespace secure_frontend {

enum class ConversationCacheEvent { parked, skipped, restored, dropped };

struct ConversationCacheLine final {
  ConversationCacheEvent event{ConversationCacheEvent::parked};
  std::uint64_t tokens{0};                  // N (dropped: the copies dropped)
  std::uint64_t parked{0};                  // P: the conversations parked now
  std::optional<std::uint64_t> bytes{};     // B: their bytes (not on a dropped line)
  std::optional<std::uint64_t> evictions{}; // E: evictions so far (parked/skipped lines that carry it)
};

// One stderr line without its line end; nullopt for anything but the three forms above.
[[nodiscard]] std::optional<ConversationCacheLine> parse_conversation_cache_line(std::string_view line) noexcept;

// Upstream's ConvCacheLog.state: what the lines of the engine's current run add up to.
struct ConversationCacheState final {
  std::uint64_t parked{0};
  std::uint64_t bytes{0};
  std::uint64_t evictions{0};
  std::uint64_t parks{0};
  std::uint64_t restores{0};
  std::optional<ConversationCacheEvent> last_event{};  // parked or restored (a skip is not a switch)
  std::optional<std::uint64_t> last_tokens{};
  std::optional<std::int64_t> last_at{};               // unix seconds, when the line was read
};

// Upstream's poll() for one line (server.py:799-810): a park, skip or restore sets parked and bytes (and evictions when
// given); a park or restore counts and becomes the last event; a drop sets parked.
void apply_conversation_cache_line(ConversationCacheState& state, const ConversationCacheLine& line, std::int64_t now_unix) noexcept;

}  // namespace secure_frontend
