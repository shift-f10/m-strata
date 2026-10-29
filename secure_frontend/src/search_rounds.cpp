#include "secure_frontend/search_rounds.hpp"

#include "secure_frontend/json.hpp"

#include <utility>

namespace secure_frontend {
namespace {

// A value as compact JSON (the events' wire form; the template's own spacing is python_json_text's).
void compact(const JsonValue& value, std::string& out) {
  switch (value.kind()) {
    case JsonKind::array: {
      out += '[';
      bool first = true;
      for (const auto& item : *value.if_array()) {
        if (!first) out += ',';
        first = false;
        compact(item, out);
      }
      out += ']';
      return;
    }
    case JsonKind::object: {
      out += '{';
      bool first = true;
      for (const auto& m : *value.if_object()) {
        if (!first) out += ',';
        first = false;
        append_json_string(&out, m.key);
        out += ':';
        compact(m.value, out);
      }
      out += '}';
      return;
    }
    default: out += python_json_text(value); return;  // a scalar: the same text either way
  }
}

std::string arguments_json(const ToolCall& call) {
  std::string out{"{"};
  bool first = true;
  for (const auto& argument : call.arguments) {
    if (!first) out += ',';
    first = false;
    append_json_string(&out, argument.name);
    out += ':';
    compact(argument.value, out);
  }
  return out + "}";
}

std::string result_event(const std::size_t round, const ToolResult& result) {
  std::string out = "{\"event\":\"result\",\"round\":" + std::to_string(round) + ",\"ok\":" + (result.ok ? "true" : "false") + ",\"text\":";
  append_json_string(&out, result.text);
  return out + ",\"chars\":" + std::to_string(result.chars) + ",\"truncated\":" + (result.truncated ? "true" : "false") +
         ",\"ms\":" + std::to_string(result.took.count()) + "}";
}

std::string skipped_event(const std::size_t round) {
  return "{\"event\":\"result\",\"round\":" + std::to_string(round) +
         ",\"ok\":false,\"skipped\":true,\"text\":\"not run\",\"chars\":0,\"truncated\":false,\"ms\":0}";
}

}  // namespace

ToolCallObserver::ToolCallObserver(GenerationObserver& client, std::vector<ToolDeclaration> tools, const ToolFormat format)
    : client_(client), parser_(make_tool_call_parser(format, std::move(tools))) {}

void ToolCallObserver::on_text(const std::string_view text) { deliver(parser_->feed_content(text)); }
void ToolCallObserver::on_reasoning(const std::string_view text) { deliver(parser_->feed_reasoning(text)); }

void ToolCallObserver::on_reasoning_end() {
  deliver(parser_->end_reasoning());
  client_.on_reasoning_end();
}

RunCancel ToolCallObserver::on_tick(const bool progress) { return client_.on_tick(progress); }

void ToolCallObserver::finish(const bool natural) {
  if (finished_) return;
  finished_ = true;
  deliver(parser_->finish(natural));
}

void ToolCallObserver::deliver(std::vector<ToolParseEvent> events) {
  for (auto& event : events) {
    switch (event.kind) {
      case ToolParseEvent::Kind::reasoning:
        reasoning_ += event.text;
        client_.on_reasoning(event.text);
        break;
      case ToolParseEvent::Kind::content:
        content_ += event.text;
        client_.on_text(event.text);
        break;
      case ToolParseEvent::Kind::call: calls_.push_back(std::move(event.call)); break;
    }
  }
}

SearchRounds::SearchRounds(ToolRunner& runner, ToolOffer offer, const std::size_t max_rounds)
    : runner_(runner), offer_(std::move(offer)), max_rounds_(max_rounds) {}

SearchRounds::Next SearchRounds::after(const ToolCallObserver& round, const RunEnd end, const std::function<bool()>& cancelled,
                                       const std::function<void(std::string)>& event) {
  const auto& calls = round.calls();
  const std::size_t number = rounds_.size() + 1U;
  bool run_them = !calls.empty() && end == RunEnd::stop && !cancelled() && !answer_round_;
  if (run_them && rounds_.size() >= max_rounds_) {
    event("{\"event\":\"limit\",\"max_rounds\":" + std::to_string(max_rounds_) + "}");
    // owner 2026-10-28: the calls are not run, and the model answers in one more round from what it found
    for (std::size_t k = 0U; k < calls.size(); ++k) event(skipped_event(number));
    rounds_.push_back(ToolRound{round.reasoning(), round.content(), calls, std::vector<std::string>(calls.size(), std::string{kSearchLimitNote})});
    answer_round_ = true;
    return Next::again;
  }
  if (!run_them) {
    for (std::size_t k = 0U; k < calls.size(); ++k) event(skipped_event(number));  // announced, never run
    return Next::done;
  }
  ToolRound kept{round.reasoning(), round.content(), calls, {}};
  for (const auto& call : calls) {
    std::string announced = "{\"event\":\"call\",\"round\":" + std::to_string(number) + ",\"name\":";
    append_json_string(&announced, call.name);
    event(announced + ",\"arguments\":" + arguments_json(call) + "}");
    ToolResult result = runner_.run(call, cancelled);
    if (result.cancelled) return Next::cancelled;
    event(result_event(number, result));
    kept.results.push_back(std::move(result.text));
  }
  if (cancelled()) return Next::cancelled;
  rounds_.push_back(std::move(kept));
  return Next::again;
}

ChatCompletionRequest with_system_note(ChatCompletionRequest request, const std::string_view note) {
  if (note.empty()) return request;
  if (!request.messages.empty() && request.messages.front().role == "system") {
    ChatMessage& system = request.messages.front();
    const std::string added = system.content.empty() ? std::string{note} : "\n\n" + std::string{note};
    system.content += added;
    if (!system.parts.empty()) system.parts.push_back(ChatMessagePart{added, {}});   // a parts-form system text keeps both
    return request;
  }
  request.messages.insert(request.messages.begin(), ChatMessage{"system", std::string{note}});
  return request;
}

ChatCompletionRequest answers_as_text(ChatCompletionRequest request) {
  for (ChatMessage& message : request.messages) {
    if (message.rounds.empty()) continue;
    std::string text;
    const auto add = [&text](const std::string& piece) {
      if (piece.empty()) return;
      if (!text.empty()) text += "\n\n";
      text += piece;
    };
    for (const ToolRound& round : message.rounds) add(round.content);
    add(message.content);
    message.content = std::move(text);
    message.rounds.clear();
  }
  return request;
}

}  // namespace secure_frontend
