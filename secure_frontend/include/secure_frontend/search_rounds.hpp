#pragma once

// The search loop's pieces (docs/superpowers/specs/2026-10-27-web-search-design.md §2), upstream's run_with_mcp
// (serve/server.py): each round's reply goes through the tool-call parser on its way to the client, and after a round
// that wrote calls the server runs them, keeps the round for the next prompt and generates again - at most
// max_rounds times. Shared by the streamed and the complete chat.

#include "secure_frontend/generation_run.hpp"
#include "secure_frontend/tool_calls.hpp"
#include "secure_frontend/tool_runner.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// One round's observer: the reply's text through the family's call parser (make_tool_call_parser); what is no call goes
// on to `client` (reasoning and content, as the parser releases them), the calls are kept. finish() ends the parse once
// the generation has ended.
class ToolCallObserver final : public GenerationObserver {
 public:
  ToolCallObserver(GenerationObserver& client, std::vector<ToolDeclaration> tools, ToolFormat format = ToolFormat::qwen);

  void on_text(std::string_view text) override;
  void on_reasoning(std::string_view text) override;
  void on_reasoning_end() override;
  RunCancel on_tick(bool progress) override;
  // `natural`: the reply ended as stop.
  void finish(bool natural);

  [[nodiscard]] const std::string& reasoning() const noexcept { return reasoning_; }
  [[nodiscard]] const std::string& content() const noexcept { return content_; }
  [[nodiscard]] const std::vector<ToolCall>& calls() const noexcept { return calls_; }

 private:
  void deliver(std::vector<ToolParseEvent> events);

  GenerationObserver& client_;
  std::unique_ptr<ToolCallParser> parser_;
  std::string reasoning_;
  std::string content_;
  std::vector<ToolCall> calls_;
  bool finished_{false};
};

// What a call past the limit is answered with in the prompt of the answer round (owner 2026-10-28).
inline constexpr std::string_view kSearchLimitNote =
    "Not run: this answer has made all the searches it may. Answer the user now from the results above, without searching "
    "again.";

// The rounds of one answer. After each generation (its observer finished), after() decides as run_with_mcp does: a
// round that ended as stop and wrote calls runs them - unless max_rounds rounds already ran ("limit") - and each call
// reports a "call" and a "result" event; calls that are not run report a skipped result. Unlike upstream, the first
// round past the limit is kept with kSearchLimitNote for each call and the model answers in one more round (owner
// 2026-10-28: upstream's limit often ended the answer before any was written); a call in that round ends the answer.
// Events are one JSON object each, upstream's strata_mcp items:
//
//   {"event":"call","round":N,"name":"web_search","arguments":{...}}
//   {"event":"result","round":N,"ok":true,"text":"...","chars":C,"truncated":false,"ms":M}
//   {"event":"result","round":N,"ok":false,"skipped":true,"text":"not run","chars":0,"truncated":false,"ms":0}
//   {"event":"limit","max_rounds":8}
class SearchRounds final {
 public:
  SearchRounds(ToolRunner& runner, ToolOffer offer, std::size_t max_rounds);

  enum class Next { done, again, cancelled };
  // done: the answer is complete; again: the round is kept and its results follow it in the next prompt; cancelled:
  // `cancelled` turned true while a call ran (the answer ends there). `event` gets each event as it happens.
  [[nodiscard]] Next after(const ToolCallObserver& round, RunEnd end, const std::function<bool()>& cancelled,
                           const std::function<void(std::string)>& event);

  [[nodiscard]] const ToolOffer& offer() const noexcept { return offer_; }
  [[nodiscard]] std::span<const ToolRound> rounds() const noexcept { return rounds_; }

 private:
  ToolRunner& runner_;
  ToolOffer offer_;
  std::size_t max_rounds_;
  std::vector<ToolRound> rounds_;
  bool answer_round_{false};  // the limit was reached and the model has had its answer round
};

}  // namespace secure_frontend
