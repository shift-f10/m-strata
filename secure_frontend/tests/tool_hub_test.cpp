// Tool plugins (the owner, 2026-10-30, decisions 6 and 7; plugin spec section 10): the tool hub offers a chat every tool
// the plugins that are on list, routes each call to the plugin that listed its name, gives each child only its own
// arguments and environment, leaves out a name another plugin listed first and a plugin that does not start, and joins
// the plugins' notes - over real child processes (the fake tool program, an MCP server on stdio).
#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/tool_hub.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
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

using secure_frontend::ToolHub;
using secure_frontend::ToolPlugin;

// The variables a child needs to start on this OS, and nothing of this process's own.
std::map<std::string, std::string, std::less<>> base_environment() {
  std::map<std::string, std::string, std::less<>> environment;
#if defined(_WIN32)
  for (const char* name : {"SystemRoot", "PATH"}) {
#else
  for (const char* name : {"PATH"}) {
#endif
    if (const char* value = secure_frontend::crt_getenv(name)) environment[name] = value;
  }
  return environment;
}

ToolPlugin fake(const std::string& id, const std::vector<std::string>& tools, const std::string& note = {}) {
  ToolPlugin plugin{.id = id, .program = FAKE_MCP_TOOL_PATH, .arguments = {"--id", id}, .environment = base_environment(), .note = note};
  for (const auto& tool : tools) {
    plugin.arguments.push_back("--tool");
    plugin.arguments.push_back(tool);
  }
  return plugin;
}

std::vector<std::string> names_of(const std::vector<secure_frontend::ToolOffer>& offers) {
  std::vector<std::string> names;
  for (const auto& offer : offers) names.push_back(offer.declaration.name);
  return names;
}

secure_frontend::ToolCall call(const std::string& name, const std::string& query) {
  return secure_frontend::ToolCall{name, {{"query", secure_frontend::JsonValue::make_string(query)}}};
}

const std::function<bool()> kNever = [] { return false; };

// Two plugins' tools in one chat, each call to the plugin that listed it; a name a second plugin lists too is left out,
// and the log says so once.
void several_plugins_tools_are_offered_and_routed() {
  std::vector<std::string> logged;
  ToolHub hub({.plugins = {fake("alpha", {"lookup"}), fake("beta", {"weather", "lookup"})},
               .log = [&logged](const std::string_view line) { logged.emplace_back(line); }});
  const auto offers = hub.offers();
  expect(names_of(offers) == std::vector<std::string>{"lookup", "weather"}, "both plugins' tools, once each");
  expect(!offers.empty() && offers[0].declaration.string_parameters == std::vector<std::string>{"query"} &&
             offers[0].template_json.find("\"name\": \"lookup\"") != std::string::npos,
         "each offered as the template writes it: " + (offers.empty() ? std::string{} : offers[0].template_json));
  (void)hub.offers();
  const auto left_out = std::count_if(logged.begin(), logged.end(), [](const std::string& line) {
    return line.find("beta") != std::string::npos && line.find("lookup") != std::string::npos && line.find("alpha") != std::string::npos;
  });
  expect(left_out == 1, "the log says once that beta's lookup is left out (alpha lists it)");
  const auto looked = hub.run(call("lookup", "rust"), kNever);
  expect(looked.ok && looked.text.find("lookup called with {\"query\": \"rust\"}") == 0U && looked.text.find("--id alpha") != std::string::npos,
         "lookup goes to alpha: " + looked.text);
  const auto weather = hub.run(call("weather", "oslo"), kNever);
  expect(weather.ok && weather.text.find("weather called with") == 0U && weather.text.find("--id beta") != std::string::npos,
         "weather goes to beta: " + weather.text);
  const auto unknown = hub.run(call("nothing", "x"), kNever);
  expect(!unknown.ok && unknown.text.find("error:") == 0U, "a tool no plugin lists is an error result: " + unknown.text);
}

// A plugin's process gets its own arguments and the environment its manifest declares (a secret only there) - and no
// other plugin's.
void a_plugin_gets_its_own_arguments_and_environment() {
  auto with_token = fake("alpha", {"lookup"});
  with_token.environment["FAKE_TOOL_TOKEN"] = "s3cret";
  ToolHub hub({.plugins = {with_token, fake("beta", {"weather"})}});
  (void)hub.offers();
  const auto alpha = hub.run(call("lookup", "x"), kNever);
  const auto beta = hub.run(call("weather", "x"), kNever);
  expect(alpha.text.find("args: --id alpha --tool lookup; token: s3cret") != std::string::npos, "alpha's own: " + alpha.text);
  expect(beta.text.find("args: --id beta --tool weather; token: none") != std::string::npos, "beta's own, not alpha's: " + beta.text);
}

// A plugin whose program does not start is left out (the others still offered) and the log says so; it is tried again
// after its pause.
void a_plugin_that_does_not_start_is_left_out() {
  std::vector<std::string> logged;
  auto ghost = fake("ghost", {"haunt"});
  ghost.program = ghost.program.parent_path() / "no-such-program.exe";
  ToolHub hub({.plugins = {ghost, fake("alpha", {"lookup"})},
               .log = [&logged](const std::string_view line) { logged.emplace_back(line); },
               .restart_pause = std::chrono::seconds(0)});
  expect(names_of(hub.offers()) == std::vector<std::string>{"lookup"}, "the plugin that runs is offered");
  expect(std::any_of(logged.begin(), logged.end(), [](const std::string& line) { return line.find("ghost") != std::string::npos; }),
         "the log names the plugin that did not start");
  expect(names_of(hub.offers()) == std::vector<std::string>{"lookup"}, "and again after its pause");
  expect(hub.listed().contains("alpha") && !hub.listed().contains("ghost"), "listed: the plugins that run");
}

// The plugins' notes join a chat's system message, today's date filled in; a plugin whose note names {today} has its
// tools' descriptions end with the date too (as web search's did).
void notes_and_dates() {
  ToolHub hub({.plugins = {fake("alpha", {"lookup"}, "Today's date is {today}. Look things up."), fake("beta", {"weather"}, "Weather is local.")},
               .today = [] { return std::string{"2026-10-30"}; }});
  const auto offers = hub.offers();
  expect(hub.system_note() == "Today's date is 2026-10-30. Look things up.\n\nWeather is local.", "the notes: " + hub.system_note());
  expect(offers.size() == 2U && offers[0].template_json.find("Fake tool lookup. Today's date is 2026-10-30.") != std::string::npos &&
             offers[1].template_json.find("Fake tool weather.\"") != std::string::npos,
         "the date ends a description only where its note names {today}");
}

// At most kMaxTools in a chat; the log says so once.
void at_most_sixteen_tools() {
  std::vector<std::string> tools;
  for (int i = 0; i < 20; ++i) tools.push_back("t" + std::to_string(i));
  std::vector<std::string> logged;
  ToolHub hub({.plugins = {fake("many", tools)}, .log = [&logged](const std::string_view line) { logged.emplace_back(line); }});
  expect(hub.offers().size() == ToolHub::kMaxTools, "16 offered");
  (void)hub.offers();
  expect(std::count_if(logged.begin(), logged.end(), [](const std::string& line) { return line.find("16") != std::string::npos; }) == 1,
         "the log says once that the rest are left out");
}

// A call that does not answer ends at its plugin's time limit; the Plugins tab's "Try" calls one plugin's tool.
void a_stuck_call_ends_and_try_calls_one() {
  auto stuck = fake("stuck", {"slow"});
  stuck.arguments.push_back("--listing-only");
  stuck.call_timeout = std::chrono::seconds(1);
  ToolHub hub({.plugins = {stuck, fake("alpha", {"lookup"})}});
  (void)hub.offers();
  const auto started = std::chrono::steady_clock::now();
  const auto slow = hub.run(call("slow", "x"), kNever);
  expect(!slow.ok && std::chrono::steady_clock::now() - started < std::chrono::seconds(10), "a stuck call ends at its time limit: " + slow.text);
  const auto tried = hub.try_call("alpha", "lookup", R"({"query":"tried"})");
  expect(tried.ok && tried.text.find("lookup called with {\"query\": \"tried\"}") == 0U, "Try: " + tried.text);
  expect(!hub.try_call("nobody", "lookup", "{}").ok && !hub.try_call("alpha", "weather", "{}").ok, "Try a plugin or a tool not running: an error");
  const auto listed = hub.listed();
  expect(listed.contains("alpha") && listed.at("alpha").size() == 1U && listed.at("alpha")[0].name == "lookup", "listed: each running plugin's tools");
}

}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"several_plugins_tools_are_offered_and_routed", several_plugins_tools_are_offered_and_routed},
      {"a_plugin_gets_its_own_arguments_and_environment", a_plugin_gets_its_own_arguments_and_environment},
      {"a_plugin_that_does_not_start_is_left_out", a_plugin_that_does_not_start_is_left_out},
      {"notes_and_dates", notes_and_dates},
      {"at_most_sixteen_tools", at_most_sixteen_tools},
      {"a_stuck_call_ends_and_try_calls_one", a_stuck_call_ends_and_try_calls_one},
  }, failures);
}
