// Web search, phase 4 (docs/superpowers/specs/2026-10-27-web-search-design.md §4): the search loop's pieces, upstream's
// run_with_mcp - a round's reply through the call parser to the client, and what happens after the round.
#include "secure_frontend/search_rounds.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

struct Client final : secure_frontend::GenerationObserver {
  std::string text;
  std::string reasoning;
  std::size_t ticks{0};
  void on_text(const std::string_view piece) override { text += piece; }
  void on_reasoning(const std::string_view piece) override { reasoning += piece; }
  secure_frontend::RunCancel on_tick(bool) override {
    ++ticks;
    return secure_frontend::RunCancel::none;
  }
};

struct FakeRunner final : secure_frontend::ToolRunner {
  std::vector<std::string> queries;
  std::vector<secure_frontend::ToolResult> results;  // in order; the last one repeats
  std::optional<secure_frontend::ToolOffer> offer() override { return std::nullopt; }
  secure_frontend::ToolResult run(const secure_frontend::ToolCall& call, const std::function<bool()>& cancelled) override {
    std::string query;
    for (const auto& a : call.arguments) {
      if (a.name == "query" && a.value.if_string() != nullptr) query = *a.value.if_string();
    }
    queries.push_back(query);
    (void)cancelled();
    auto result = results.empty() ? secure_frontend::ToolResult{} : results[std::min(queries.size(), results.size()) - 1U];
    return result;
  }
};

secure_frontend::ToolOffer web_search() {
  return {{"web_search", {"query"}}, "{\"name\": \"web_search\"}"};
}

std::string call_text(const std::string& query, const std::string& extra = "") {
  return "<tool_call>\n<function=web_search>\n<parameter=query>\n" + query + "\n</parameter>\n" + extra + "</function>\n</tool_call>";
}

void a_rounds_calls_never_reach_the_client() {
  Client client;
  secure_frontend::ToolCallObserver round(client, {web_search().declaration});
  round.on_reasoning("I should look this up.\n");
  round.on_reasoning_end();
  round.on_text("Let me check.\n\n");
  const std::string call = call_text("world cup", "<parameter=max_results>\n3\n</parameter>\n");
  for (const char c : call) round.on_text(std::string(1U, c));  // as the tokens arrive
  expect(round.on_tick(false) == secure_frontend::RunCancel::none && client.ticks == 1U, "a tick is the client's");
  round.finish(true);
  round.finish(true);  // a second finish is nothing
  expect(client.reasoning == "I should look this up.\n", "the reasoning reaches the client: [" + client.reasoning + "]");
  expect(client.text == "Let me check.", "the text before the call reaches it, the call does not: [" + client.text + "]");
  expect(round.content() == "Let me check." && round.reasoning() == "I should look this up.\n", "the round keeps both");
  expect(round.calls().size() == 1U && round.calls()[0].name == "web_search" && round.calls()[0].arguments.size() == 2U,
         "the call is kept");
}

void finish_releases_what_the_parser_held() {
  Client client;
  secure_frontend::ToolCallObserver round(client, {web_search().declaration});
  round.on_text("The end <tool_");
  expect(client.text == "The end ", "a partial tag is held: [" + client.text + "]");
  round.finish(false);
  expect(client.text == "The end <tool_", "the end of the reply releases it: [" + client.text + "]");
}

void a_round_that_calls_runs_them_and_is_kept() {
  Client client;
  secure_frontend::ToolCallObserver round(client, {web_search().declaration});
  round.on_reasoning("Search.");
  round.on_reasoning_end();
  round.on_text(call_text("a") + "\n" + call_text("b"));
  round.finish(true);
  FakeRunner runner;
  runner.results = {{true, "results for a", 13U, false, false, std::chrono::milliseconds(5)},
                    {false, "error: SearXNG answered 500", 27U, false, false, std::chrono::milliseconds(7)}};
  secure_frontend::SearchRounds rounds(runner, web_search(), 8U);
  std::vector<std::string> events;
  const auto next = rounds.after(round, secure_frontend::RunEnd::stop, [] { return false; }, [&events](std::string e) { events.push_back(std::move(e)); });
  expect(next == secure_frontend::SearchRounds::Next::again, "a round that called goes on");
  expect(runner.queries == std::vector<std::string>{"a", "b"}, "both calls ran, in order");
  expect(rounds.rounds().size() == 1U, "the round is kept");
  if (rounds.rounds().size() == 1U) {
    const auto& kept = rounds.rounds()[0];
    expect(kept.reasoning == "Search." && kept.content.empty() && kept.calls.size() == 2U &&
               kept.results == std::vector<std::string>{"results for a", "error: SearXNG answered 500"},
           "with its reasoning, calls and one result per call (a failed call's text too)");
  }
  const std::vector<std::string> expected{
      R"({"event":"call","round":1,"name":"web_search","arguments":{"query":"a"}})",
      R"({"event":"result","round":1,"ok":true,"text":"results for a","chars":13,"truncated":false,"ms":5})",
      R"({"event":"call","round":1,"name":"web_search","arguments":{"query":"b"}})",
      R"({"event":"result","round":1,"ok":false,"text":"error: SearXNG answered 500","chars":27,"truncated":false,"ms":7})"};
  expect(events == expected, "upstream's call and result events: " + (events.empty() ? std::string{"none"} : events.front()));
}

void a_round_without_calls_is_the_answer() {
  Client client;
  secure_frontend::ToolCallObserver round(client, {web_search().declaration});
  round.on_text("Paris.");
  round.finish(true);
  FakeRunner runner;
  secure_frontend::SearchRounds rounds(runner, web_search(), 8U);
  std::vector<std::string> events;
  expect(rounds.after(round, secure_frontend::RunEnd::stop, [] { return false; }, [&events](std::string e) { events.push_back(std::move(e)); }) ==
             secure_frontend::SearchRounds::Next::done,
         "no call: the answer is complete");
  expect(events.empty() && runner.queries.empty() && rounds.rounds().empty(), "nothing ran, nothing was said");
}

void calls_not_run_are_skipped() {
  Client client;
  secure_frontend::ToolCallObserver round(client, {web_search().declaration});
  round.on_text(call_text("q"));
  round.finish(true);
  FakeRunner runner;
  secure_frontend::SearchRounds rounds(runner, web_search(), 8U);
  std::vector<std::string> events;
  const auto collect = [&events](std::string e) { events.push_back(std::move(e)); };
  expect(rounds.after(round, secure_frontend::RunEnd::length, [] { return false; }, collect) == secure_frontend::SearchRounds::Next::done,
         "a reply cut short runs nothing");
  expect(runner.queries.empty() && events == std::vector<std::string>{
                                       R"({"event":"result","round":1,"ok":false,"skipped":true,"text":"not run","chars":0,"truncated":false,"ms":0})"},
         "its call is closed as skipped: " + (events.empty() ? std::string{"none"} : events.front()));
}

// Upstream ends the answer at the limit, often before the model wrote any (owner 2026-10-28: "it did search but did not
// give me an answer"; he chose "give the model one final answer round after the limit"): the calls past the limit are
// not run, each is answered with a note that the limit is reached, and the model has one more round to answer from what
// it found. A call in that round ends the answer as upstream's limit does.
void the_round_limit_leaves_one_answer_round() {
  FakeRunner runner;
  runner.results = {{true, "r", 1U, false, false, std::chrono::milliseconds(1)}};
  secure_frontend::SearchRounds rounds(runner, web_search(), 1U);
  std::vector<std::string> events;
  const auto collect = [&events](std::string e) { events.push_back(std::move(e)); };
  Client client;
  secure_frontend::ToolCallObserver first(client, {web_search().declaration});
  first.on_text(call_text("one"));
  first.finish(true);
  expect(rounds.after(first, secure_frontend::RunEnd::stop, [] { return false; }, collect) == secure_frontend::SearchRounds::Next::again,
         "the first round runs");
  events.clear();
  secure_frontend::ToolCallObserver second(client, {web_search().declaration});
  second.on_reasoning("More.");
  second.on_reasoning_end();
  second.on_text(call_text("two"));
  second.finish(true);
  expect(rounds.after(second, secure_frontend::RunEnd::stop, [] { return false; }, collect) == secure_frontend::SearchRounds::Next::again,
         "max_rounds 1: the second round's search does not run, but the model answers in one more round");
  expect(runner.queries == std::vector<std::string>{"one"}, "only the first search ran");
  expect(events == std::vector<std::string>{R"({"event":"limit","max_rounds":1})",
                                            R"({"event":"result","round":2,"ok":false,"skipped":true,"text":"not run","chars":0,"truncated":false,"ms":0})"},
         "the limit, then the call closed: " + (events.empty() ? std::string{"none"} : events.front()));
  expect(rounds.rounds().size() == 2U && rounds.rounds()[1].reasoning == "More." && rounds.rounds()[1].calls.size() == 1U &&
             rounds.rounds()[1].results == std::vector<std::string>{std::string{secure_frontend::kSearchLimitNote}},
         "the round is kept for the prompt, its call answered with the limit's note");
  expect(std::string{secure_frontend::kSearchLimitNote}.find("Answer the user now") != std::string::npos, "the note asks for the answer");
  events.clear();
  secure_frontend::ToolCallObserver third(client, {web_search().declaration});
  third.on_text(call_text("three"));
  third.finish(true);
  expect(rounds.after(third, secure_frontend::RunEnd::stop, [] { return false; }, collect) == secure_frontend::SearchRounds::Next::done &&
             runner.queries.size() == 1U && rounds.rounds().size() == 2U,
         "a call in the answer round ends the answer, as upstream's limit does");
  expect(events == std::vector<std::string>{R"({"event":"result","round":3,"ok":false,"skipped":true,"text":"not run","chars":0,"truncated":false,"ms":0})"},
         "that call closed as skipped, the limit said once: " + (events.empty() ? std::string{"none"} : events.front()));
}

void a_cancel_while_a_search_runs_ends_the_answer() {
  Client client;
  secure_frontend::ToolCallObserver round(client, {web_search().declaration});
  round.on_text(call_text("a") + call_text("b"));
  round.finish(true);
  FakeRunner runner;
  runner.results = {{false, "", 0U, false, true, std::chrono::milliseconds(0)}};
  secure_frontend::SearchRounds rounds(runner, web_search(), 8U);
  std::vector<std::string> events;
  bool asked = false;
  // the chat stops once the first search has started
  const auto stopped = [&asked, &runner] {
    asked = asked || !runner.queries.empty();
    return !runner.queries.empty();
  };
  const auto next = rounds.after(round, secure_frontend::RunEnd::stop, stopped, [&events](std::string e) { events.push_back(std::move(e)); });
  expect(next == secure_frontend::SearchRounds::Next::cancelled && asked, "the cancel reaches the call and ends the answer");
  expect(runner.queries.size() == 1U && rounds.rounds().empty(), "no later call runs, no round is kept");
  const auto calls = static_cast<std::size_t>(std::count_if(events.begin(), events.end(), [](const std::string& e) {
    return e.find("\"event\":\"call\"") != std::string::npos;
  }));
  expect(calls == 1U, "only the call that started was announced");
}

void events_escape_what_they_carry() {
  Client client;
  secure_frontend::ToolCallObserver round(client, {web_search().declaration});
  round.on_text(call_text("say \"hi\"\n2", "<parameter=max_results>\n2\n</parameter>\n"));
  round.finish(true);
  FakeRunner runner;
  runner.results = {{true, "line\n\"q\" \xC3\xA9", 10U, true, false, std::chrono::milliseconds(1234)}};
  secure_frontend::SearchRounds rounds(runner, web_search(), 8U);
  std::vector<std::string> events;
  (void)rounds.after(round, secure_frontend::RunEnd::stop, [] { return false; }, [&events](std::string e) { events.push_back(std::move(e)); });
  expect(events.size() == 2U && events[0] == R"({"event":"call","round":1,"name":"web_search","arguments":{"query":"say \"hi\"\n2","max_results":2}})",
         "the arguments as compact JSON: " + (events.empty() ? std::string{"none"} : events[0]));
  expect(events.size() == 2U && events[1] == "{\"event\":\"result\",\"round\":1,\"ok\":true,\"text\":\"line\\n\\\"q\\\" \xC3\xA9\",\"chars\":10,"
                                              "\"truncated\":true,\"ms\":1234}",
         "the result's text escaped, UTF-8 kept");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"a_rounds_calls_never_reach_the_client", a_rounds_calls_never_reach_the_client},
      {"finish_releases_what_the_parser_held", finish_releases_what_the_parser_held},
      {"a_round_that_calls_runs_them_and_is_kept", a_round_that_calls_runs_them_and_is_kept},
      {"a_round_without_calls_is_the_answer", a_round_without_calls_is_the_answer},
      {"calls_not_run_are_skipped", calls_not_run_are_skipped},
      {"the_round_limit_leaves_one_answer_round", the_round_limit_leaves_one_answer_round},
      {"a_cancel_while_a_search_runs_ends_the_answer", a_cancel_while_a_search_runs_ends_the_answer},
      {"events_escape_what_they_carry", events_escape_what_they_carry},
  }, failures);
}
