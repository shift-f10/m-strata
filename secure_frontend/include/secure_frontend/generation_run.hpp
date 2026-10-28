#pragma once

#include "secure_frontend/api_constants.hpp"
#include "secure_frontend/engine_protocol_client.hpp"
#include "secure_frontend/tokenizer_pack.hpp"

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace secure_frontend {

// Why a generation was cut short from the frontend side.
enum class RunCancel { none, client_gone, write_stall, shutting_down };

// How one driven generation ended.
// stop: DONE … stop. length: DONE … length, the byte cap, or the generation
// timeout (B4). cancelled: the observer cancelled (client gone, stall or
// shutdown) and STOP drained. engine_error: DONE … nonfinite (engine idle),
// ERR/fatal line (protocol closed), or more tokens than GEN allowed
// (abandoned). engine_unavailable: exit, protocol failure, a failed STOP
// write or a drain that missed its deadline (protocol closed).
// undecodable: a generated id that is not ordinary output, STOP drained.
enum class RunEnd { stop, length, cancelled, engine_error, engine_unavailable, undecodable };

// The Monitor's meter (listener.monitor, session log step 16): what one generation did,
// never what it said. Called on the loop's thread; an implementation must not block.
class GenerationMeter {
 public:
  virtual void on_token(std::size_t generated) = 0;      // after each counted T record
  virtual void on_done(const DoneFigures& figures) = 0;  // the engine's DONE figures
  // Monitor: the engine's own readings (INFO before READY + the maya_118 STAT cadence),
  // pushed whenever a progress record flows — the last push carries the latest values.
  virtual void on_engine_stats(const std::map<std::string, std::string>&) {}
  virtual ~GenerationMeter() = default;
};

// The most bytes the channel-name line may take, its newline included
// (GenerationRunLimits::channel_name).
inline constexpr std::size_t kChannelNameLineBytes = 64U;

struct GenerationRunLimits final {
  std::chrono::milliseconds generation_timeout{std::chrono::seconds(120)};
  std::chrono::milliseconds stop_drain_timeout{std::chrono::seconds(5)};
  std::size_t max_generated_bytes{1048576U};
  std::size_t max_tokens{0};  // the GEN max_new; 0 = unchecked
  std::chrono::milliseconds slice{api_constants::kStreamPollSlice};
  std::chrono::milliseconds shutdown_drain{api_constants::kShutdownDrainBudget};  // drain cap when cancel == shutting_down
  // SP5 M1 Task 8 (engine_silence_s / repeat_stop_tokens; serve/server.py:274-276, :197/:4908):
  // the silence watchdog ends a stalled generation at the silence bound (0: off — the total
  // deadline alone), and a run of this many equal token ids ends a reply as length (0: off).
  std::chrono::milliseconds silence{0};
  std::size_t repeat_stop_tokens{0};
  // The Qwen3 reasoning split (upstream OutputParser, serve/frontend.py:884, :1245-1284):
  // with think_tokens set, text before </think> goes to on_reasoning and text after it to
  // on_text, the newlines right after </think> dropped. The two tags themselves are never
  // output; a <think> while answering starts a new reasoning span. starts_in_reasoning:
  // the prompt left a think block open. Without think_tokens both tags fail closed as any
  // CONTROL/USER_DEFINED token does.
  std::optional<ThinkTokens> think_tokens{};
  bool starts_in_reasoning{false};
  // MiniMax-M3.1 (owner 2026-10-28): the newlines right after </think> stay in the answer, as Maya's parser keeps them
  // for MiniMax (its lead rule is GLM's and Qwen's only). False: upstream's rule, they are dropped.
  bool keeps_newlines_after_think{false};
  // Issue 26: end-of-turn ids (find_qwen_end_of_turn_tokens). One ends the reply as stop: it
  // is never output, and anything the engine sends after it before DONE is dropped
  // (upstream breaks out of its token loop there, serve/server.py:5086-5091).
  std::vector<std::size_t> stop_tokens{};
  // The turn markers Maya's server stops on itself (DeepSeek-V4.1, MiniMax-M3.1: serve/server.py:1749-1764), its engines
  // stopping only on their EOS. One ends the reply as stop, as a stop_tokens id does, and the frontend then sends one
  // STOP and drains to DONE, as Maya's server does (:1019-1044); an engine that had ended the turn itself reads that STOP
  // after its DONE and clears it at the next GEN (maya-ds4 ds4_main.cpp:846, maya-mm3 mm3_main.cpp:312).
  std::vector<std::size_t> server_stop_tokens{};
  // Gemma 4 plan 1 Task 9 (spec §6.4): the thought channel's name line ("thought" for gemma4).
  // Set, the decoded text right after each open tag up to its first newline must equal it, and that
  // line is dropped; any other line, none within kChannelNameLineBytes, a tag or an end-of-turn
  // id inside it, or the engine's own stop inside it ends the reply as undecodable (STOP and a
  // drain while the engine still generates). Unset (qwen, glm53): the open tag starts the
  // reasoning at once, as before.
  std::optional<std::string> channel_name{};
  // Web search (spec 2026-10-27-web-search-design §2): special tokens output as their vocabulary literal, in the channel
  // they were written in - the Qwen tool tags while the server offers its tool, for QwenToolCallParser (upstream decodes
  // every token as its text, and its OutputParser reads the calls out of that text). Empty: they fail closed as any
  // CONTROL or USER_DEFINED token does.
  std::vector<std::pair<std::size_t, std::string>> literal_tokens{};
  // The Monitor's meter; null (the default): nothing is measured.
  GenerationMeter* meter{nullptr};
};

struct GenerationRunResult final {
  RunEnd end{RunEnd::engine_unavailable};
  std::size_t completion_tokens{0};
  bool timed_out{false};
  bool bytes_capped{false};
  RunCancel cancel{RunCancel::none};
};

// Receives the decoded text and the liveness beat of one driven generation.
class GenerationObserver {
 public:
  virtual void on_text(std::string_view text) = 0;  // complete UTF-8, never empty
  // Reasoning text (GenerationRunLimits::think_tokens), complete UTF-8, never empty. An
  // observer that does not override it drops the reasoning and keeps the answer.
  virtual void on_reasoning(std::string_view /*text*/) {}
  // The </think> that ended a reasoning span (after its last on_reasoning): the answer follows.
  virtual void on_reasoning_end() {}
  virtual RunCancel on_tick(bool progress) = 0;     // at least once per slice, and after every PP
  virtual ~GenerationObserver() = default;
};

// One generation loop for both paths (spec §3.3/§3.4): reads records as
// events in bounded slices, decodes tokens through the pack and the UTF-8
// stream decoder, enforces the byte cap on code-point boundaries, and
// answers timeout / cap / observer cancel with exactly one STOP and a
// bounded drain to DONE. The protocol must already be past READY and GEN.
[[nodiscard]] GenerationRunResult drive_generation(EngineProtocolClient& protocol, const TokenByteDecoder& decoder,
                                                   const GenerationRunLimits& limits, GenerationObserver& observer);

}  // namespace secure_frontend
