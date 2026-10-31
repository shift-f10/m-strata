#pragma once

// Tool plugins (docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md, section 10; the owner's decisions 6
// and 7): each tool plugin that is on, and whose settings fit its manifest, is an MCP server on stdio - JSON-RPC one
// message a line, initialize, tools/list, tools/call, as strata-search speaks it (upstream's serve/mcp.py client side,
// mcp_client.hpp). The hub starts each one at the first chat that needs a tool and offers that chat every tool they
// list, at most kMaxTools: a name a plugin listed earlier keeps it, and the log says once that the later one is left
// out. Each call goes to the plugin that listed its name. A program that does not start or answer is left out for
// restart_pause, then started again by a later chat. Each child gets the closed environment the downloads' children
// get plus what its manifest declares (a secret only there), its own arguments, and nothing else; the server stays the
// boundary. This replaces the one search service of the SECURE_FRONTEND_WEB_SEARCH builds: strata-search is now the
// search plugin's program.

#include "secure_frontend/engine_stdio_process.hpp"
#include "secure_frontend/mcp_client.hpp"
#include "secure_frontend/tool_runner.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// One tool plugin, as the server runs it.
struct ToolPlugin final {
  std::string id;                                               // the plugin's (the log's name for it)
  std::filesystem::path program;                                // in its folder, checked when the server started
  std::vector<std::string> arguments;                           // its manifest's, each setting's value filled in
  std::map<std::string, std::string, std::less<>> environment;  // the closed environment and its declared variables
  std::string note;                                             // its system-message note ({today} as written)
  std::chrono::milliseconds call_timeout{std::chrono::seconds(120)};
  std::filesystem::path working_directory{};                    // its own folder, <roots.state>/plugins/<id> (empty: inherit)
};

struct ToolHubSettings final {
  std::vector<ToolPlugin> plugins;
  std::function<std::string()> today{};          // YYYY-MM-DD (default: this PC's local date)
  std::function<void(std::string_view)> log{};   // one fixed line per start, failure and tool left out (default: none)
  std::chrono::seconds restart_pause{60};        // after a failed start, that plugin's tools are left out this long
  // Each line a program writes on stderr goes to `log` as "[plugin <id>] <line>", printable and cut at 400 characters
  // (while the server logs its engines' own output: the Debugging plugin). false: its stderr goes nowhere.
  bool log_stderr{false};
};

class ToolHub final : public ToolRunner {
 public:
  static constexpr std::size_t kMaxTools = 16U;
  static constexpr std::size_t kMaxResultChars = 20000U;  // what the model reads of one result (upstream's cut)

  explicit ToolHub(ToolHubSettings settings);
  ~ToolHub() override;
  ToolHub(const ToolHub&) = delete;
  ToolHub& operator=(const ToolHub&) = delete;

  // Every tool the plugins list, as the template shows each (its description ending with today's date when its plugin's
  // note names {today}). While a plugin's call runs (another chat's), the tools it listed are offered as they are.
  [[nodiscard]] std::vector<ToolOffer> offers() override;
  [[nodiscard]] ToolResult run(const ToolCall& call, const std::function<bool()>& cancelled) override;
  void served_without(std::string_view why) override;
  // The notes of the plugins whose tools are offered, {today} filled in, a blank line between them.
  [[nodiscard]] std::string system_note() const override;
  // The plugins (by id) whose programs are running now, each with the tools it lists (the Plugins tab's "Try").
  [[nodiscard]] std::map<std::string, std::vector<McpTool>> listed() const;
  // One call of `tool` on plugin `id` with `arguments_json` (an object), for the Plugins tab's "Try".
  [[nodiscard]] ToolResult try_call(std::string_view id, std::string_view tool, std::string_view arguments_json);
  // Plugin `id` removed from the page (plugin spec section 12): its program stopped (after a call of it that runs), its
  // tools no longer offered, called or tried, and it is not started again while this server runs. false: no such plugin.
  bool stop(std::string_view id);

 private:
  struct Running;
  [[nodiscard]] bool ensure_started(Running& plugin);  // its mutex is held
  void say_once(const std::string& line);

  ToolHubSettings settings_;
  std::vector<std::unique_ptr<Running>> plugins_;
  mutable std::mutex routes_mutex_;              // which plugin answers which tool name (the last offers()'s)
  std::map<std::string, std::size_t, std::less<>> routes_;
  mutable std::mutex said_mutex_;
  std::set<std::string, std::less<>> said_;
};

// This PC's local date, YYYY-MM-DD.
[[nodiscard]] std::string local_date_today();

}  // namespace secure_frontend
