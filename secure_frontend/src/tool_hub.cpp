#include "secure_frontend/tool_hub.hpp"

#include "secure_frontend/json.hpp"

#include <algorithm>
#include <ctime>
#include <optional>
#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

constexpr std::size_t kMaxReplyLine = 1U << 20U;  // one tools/call answer: 20,000 characters and its JSON, with room
constexpr JsonLimits kSchemaLimits{.max_bytes = 1U << 20U, .max_depth = 32U, .max_container_entries = 4096U,
                                   .max_string_bytes = 1U << 16U, .allow_fractions = true};

const JsonValue* member(const JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& m : *members) {
    if (m.key == key) return &m.value;
  }
  return nullptr;
}

// A value as compact JSON, for tools/call's arguments.
void compact(const JsonValue& value, std::string& out) {
  if (const auto* items = value.if_array()) {
    out += '[';
    for (std::size_t i = 0; i < items->size(); ++i) {
      if (i > 0U) out += ',';
      compact((*items)[i], out);
    }
    out += ']';
  } else if (const auto* members = value.if_object()) {
    out += '{';
    for (std::size_t i = 0; i < members->size(); ++i) {
      if (i > 0U) out += ',';
      append_json_string(&out, (*members)[i].key);
      out += ':';
      compact((*members)[i].value, out);
    }
    out += '}';
  } else {
    out += python_json_text(value);
  }
}

std::string with_today(std::string text, const std::string& today) {
  for (auto at = text.find("{today}"); at != std::string::npos; at = text.find("{today}", at + today.size())) text.replace(at, 7U, today);
  return text;
}

ToolResult failed(std::string text, const std::chrono::steady_clock::time_point started) {
  const auto chars = text.size();
  return ToolResult{false, std::move(text), chars, false, false,
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)};
}

}  // namespace

// One plugin's child and its MCP client; a call holds its mutex, the tools it listed are kept apart for offers().
struct ToolHub::Running final {
  ToolPlugin plugin;
  std::mutex mutex;
  std::unique_ptr<EngineStdioProcess> process;
  std::unique_ptr<McpClient> client;
  std::optional<std::chrono::steady_clock::time_point> failed_at;
  mutable std::mutex listed_mutex;
  std::vector<McpTool> listed;  // while it runs

  void stop() noexcept {  // its mutex is held
    client.reset();
    if (process != nullptr) {
      process->close_stdin();  // an MCP server on stdio ends at the end of its input
      (void)process->terminate(std::chrono::seconds(2));
      process.reset();
    }
    const std::lock_guard lock(listed_mutex);
    listed.clear();
  }
};

std::string local_date_today() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  if (localtime_s(&local, &now) != 0) return "unknown";
#else
  if (localtime_r(&now, &local) == nullptr) return "unknown";
#endif
  char text[16];
  return std::strftime(text, sizeof(text), "%Y-%m-%d", &local) == 10U ? std::string{text} : std::string{"unknown"};
}

ToolHub::ToolHub(ToolHubSettings settings) : settings_(std::move(settings)) {
  if (!settings_.today) settings_.today = local_date_today;
  if (!settings_.log) settings_.log = [](std::string_view) {};
  for (auto& plugin : settings_.plugins) {
    auto running = std::make_unique<Running>();
    running->plugin = plugin;
    plugins_.push_back(std::move(running));
  }
}

ToolHub::~ToolHub() {
  for (auto& plugin : plugins_) {
    const std::lock_guard lock(plugin->mutex);
    plugin->stop();
  }
}

void ToolHub::say_once(const std::string& line) {
  {
    const std::lock_guard lock(said_mutex_);
    if (!said_.insert(line).second) return;
  }
  settings_.log(line);
}

bool ToolHub::ensure_started(Running& plugin) {
  if (plugin.process != nullptr && plugin.client != nullptr && plugin.process->running()) return true;
  plugin.stop();
  const auto now = std::chrono::steady_clock::now();
  if (plugin.failed_at.has_value() && now - *plugin.failed_at < settings_.restart_pause) return false;
  const std::string& id = plugin.plugin.id;
  plugin.process = std::make_unique<EngineStdioProcess>();
  // in its own folder, with no new privileges (Linux); its stderr to the log only while the server logs its engines' own
  // output, each line marked with the plugin and cut at 400 characters (as EngineStdioProcess hands it: printable)
  EngineCommand command{.executable = plugin.plugin.program,
                        .arguments = plugin.plugin.arguments,
                        .environment = plugin.plugin.environment,
                        .working_directory = plugin.plugin.working_directory,
                        .no_new_privileges = true,
                        .insert_serve = false,
                        .max_read_line_bytes = kMaxReplyLine};
  if (settings_.log_stderr) {
    command.stderr_lines = [log = settings_.log, prefix = "[plugin " + id + "] "](const std::string_view line) {
      constexpr std::size_t kMaxStderrChars = 400U;
      log(prefix + std::string{line.substr(0, kMaxStderrChars)} + (line.size() > kMaxStderrChars ? " [cut]" : ""));
    };
  }
  if (const auto error = plugin.process->start(command)) {
    settings_.log("[strata] plugins: " + id + "'s program did not start; chats are served without its tools for now");
    plugin.process.reset();
    plugin.failed_at = now;
    return false;
  }
  plugin.client = std::make_unique<McpClient>(*plugin.process, McpLimits{.start_timeout = std::chrono::seconds(30),
                                                                         .call_timeout = plugin.plugin.call_timeout,
                                                                         .max_result_chars = kMaxResultChars});
  if (const auto failure = plugin.client->start()) {
    settings_.log("[strata] plugins: " + id + "'s program did not answer (" + *failure + "); chats are served without its tools for now");
    plugin.stop();
    plugin.failed_at = now;
    return false;
  }
  plugin.failed_at.reset();
  {
    const std::lock_guard lock(plugin.listed_mutex);
    plugin.listed = plugin.client->tools();
  }
  settings_.log("[strata] plugins: " + id + " is running");
  return true;
}

std::vector<ToolOffer> ToolHub::offers() {
  std::vector<ToolOffer> out;
  std::map<std::string, std::size_t, std::less<>> routes;
  const std::string today = settings_.today();
  for (std::size_t index = 0; index < plugins_.size(); ++index) {
    Running& plugin = *plugins_[index];
    {
      // a call of this plugin runs (another chat's): the program is up, and the tools it listed stand
      std::unique_lock lock(plugin.mutex, std::try_to_lock);
      if (lock.owns_lock() && !ensure_started(plugin)) continue;
    }
    std::vector<McpTool> listed;
    {
      const std::lock_guard lock(plugin.listed_mutex);
      listed = plugin.listed;
    }
    const bool dated = plugin.plugin.note.find("{today}") != std::string::npos;
    for (const McpTool& tool : listed) {
      if (const auto taken = routes.find(tool.name); taken != routes.end()) {
        say_once("[strata] plugins: " + plugin.plugin.id + "'s tool " + tool.name + " is left out: " + plugins_[taken->second]->plugin.id +
                 " lists it too");
        continue;
      }
      if (out.size() >= kMaxTools) {
        say_once("[strata] plugins: a chat is offered at most 16 tools; the rest are left out");
        break;
      }
      auto parsed = parse_json(tool.input_schema, kSchemaLimits);
      auto* schema = std::get_if<JsonValue>(&parsed);
      if (schema == nullptr || schema->if_object() == nullptr) continue;
      ToolDeclaration declaration{tool.name, {}};
      if (const auto* properties = member(*schema, "properties"); properties != nullptr && properties->if_object() != nullptr) {
        for (const auto& p : *properties->if_object()) {
          const auto* type = member(p.value, "type");
          if (type != nullptr && type->if_string() != nullptr && *type->if_string() == "string") declaration.string_parameters.push_back(p.key);
        }
      }
      std::string description = tool.description;
      if (dated) description += (description.empty() ? "" : " ") + std::string{"Today's date is "} + today + ".";
      std::vector<JsonMember> members;
      members.push_back({"name", JsonValue::make_string(tool.name)});
      members.push_back({"description", JsonValue::make_string(std::move(description))});
      members.push_back({"parameters", std::move(*schema)});
      routes.emplace(tool.name, index);
      out.push_back(ToolOffer{std::move(declaration), python_json_text(JsonValue::make_object(std::move(members)))});
    }
  }
  const std::lock_guard lock(routes_mutex_);
  routes_ = std::move(routes);
  return out;
}

std::string ToolHub::system_note() const {
  std::set<std::size_t> offering;
  {
    const std::lock_guard lock(routes_mutex_);
    for (const auto& [name, index] : routes_) offering.insert(index);
  }
  const std::string today = settings_.today();
  std::string note;
  for (const std::size_t index : offering) {
    const std::string& text = plugins_[index]->plugin.note;
    if (text.empty()) continue;
    note += (note.empty() ? "" : "\n\n") + with_today(text, today);
  }
  return note;
}

void ToolHub::served_without(const std::string_view why) {
  say_once("[strata] plugins: this model's prompt cannot carry the plugins' tools (" + std::string{why} +
           "); its chats are served without them");
}

ToolResult ToolHub::run(const ToolCall& call, const std::function<bool()>& cancelled) {
  const auto started = std::chrono::steady_clock::now();
  std::optional<std::size_t> index;
  {
    const std::lock_guard lock(routes_mutex_);
    if (const auto found = routes_.find(call.name); found != routes_.end()) index = found->second;
  }
  if (!index) return failed("error: no tool named " + call.name + " is offered", started);
  Running& plugin = *plugins_[*index];
  const std::lock_guard lock(plugin.mutex);
  if (!ensure_started(plugin)) return failed("error: the tool " + call.name + " is not available now", started);
  std::string arguments{"{"};
  for (std::size_t i = 0; i < call.arguments.size(); ++i) {
    if (i > 0U) arguments += ',';
    append_json_string(&arguments, call.arguments[i].name);
    arguments += ':';
    compact(call.arguments[i].value, arguments);
  }
  arguments += '}';
  McpCallResult result = plugin.client->call(call.name, arguments, cancelled);
  if (!plugin.process->running()) plugin.stop();  // it ended: the next chat starts it again
  return ToolResult{result.ok, std::move(result.text), result.chars, result.truncated, result.cancelled,
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)};
}

std::map<std::string, std::vector<McpTool>> ToolHub::listed() const {
  std::map<std::string, std::vector<McpTool>> out;
  for (const auto& plugin : plugins_) {
    const std::lock_guard lock(plugin->listed_mutex);
    if (!plugin->listed.empty()) out[plugin->plugin.id] = plugin->listed;
  }
  return out;
}

ToolResult ToolHub::try_call(const std::string_view id, const std::string_view tool, const std::string_view arguments_json) {
  const auto started = std::chrono::steady_clock::now();
  const auto found = std::find_if(plugins_.begin(), plugins_.end(), [id](const auto& plugin) { return plugin->plugin.id == id; });
  if (found == plugins_.end()) return failed("error: no tool plugin " + std::string{id} + " runs in this server", started);
  Running& plugin = **found;
  const std::lock_guard lock(plugin.mutex);
  if (!ensure_started(plugin)) return failed("error: " + std::string{id} + "'s program is not running now", started);
  const auto& tools = plugin.client->tools();
  if (std::none_of(tools.begin(), tools.end(), [tool](const McpTool& t) { return t.name == tool; })) {
    return failed("error: " + std::string{id} + " lists no tool " + std::string{tool}, started);
  }
  McpCallResult result = plugin.client->call(tool, arguments_json, [] { return false; });
  if (!plugin.process->running()) plugin.stop();
  return ToolResult{result.ok, std::move(result.text), result.chars, result.truncated, result.cancelled,
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)};
}

}  // namespace secure_frontend
