#include "secure_frontend/search_service.hpp"

#include "secure_frontend/json.hpp"

#include <ctime>
#include <utility>
#include <variant>
#include <vector>

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

}  // namespace

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

SearchService::SearchService(SearchServiceSettings settings) : settings_(std::move(settings)) {
  if (!settings_.today) settings_.today = local_date_today;
  if (!settings_.log) settings_.log = [](std::string_view) {};
}

SearchService::~SearchService() {
  std::lock_guard lock(mutex_);
  stop_child();
}

void SearchService::stop_child() noexcept {
  client_.reset();
  if (process_ != nullptr) {
    process_->close_stdin();  // strata-search ends at the end of its input
    (void)process_->terminate(std::chrono::seconds(2));
    process_.reset();
  }
}

bool SearchService::ensure_started() {
  if (process_ != nullptr && client_ != nullptr && process_->running()) return true;
  stop_child();
  const auto now = std::chrono::steady_clock::now();
  if (failed_at_.has_value() && now - *failed_at_ < kRestartPause) return false;
  process_ = std::make_unique<EngineStdioProcess>();
  const EngineCommand command{.executable = settings_.program,
                              .arguments = {"--url", settings_.url, "--results", std::to_string(settings_.results), "--timeout",
                                            std::to_string(settings_.timeout.count())},
                              .environment = settings_.environment,
                              .insert_serve = false,
                              .max_read_line_bytes = kMaxReplyLine};
  if (const auto error = process_->start(command)) {
    settings_.log("[strata] search: strata-search did not start; chats are served without search for now");
    process_.reset();
    failed_at_ = now;
    return false;
  }
  // a call waits for strata-search's own connect, request and read steps, and a little more
  client_ = std::make_unique<McpClient>(
      *process_, McpLimits{.start_timeout = std::chrono::seconds(30),
                           .call_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(settings_.timeout * 3 + std::chrono::seconds(5)),
                           .max_result_chars = 20000U});
  if (const auto failure = client_->start()) {
    settings_.log("[strata] search: strata-search did not answer (" + *failure + "); chats are served without search for now");
    stop_child();
    failed_at_ = now;
    return false;
  }
  failed_at_.reset();
  settings_.log("[strata] search: strata-search is running");
  return true;
}

std::optional<ToolOffer> SearchService::offer_of(const McpTool& tool) const {
  auto parsed = parse_json(tool.input_schema, kSchemaLimits);
  auto* schema = std::get_if<JsonValue>(&parsed);
  if (schema == nullptr || schema->if_object() == nullptr) return std::nullopt;
  ToolDeclaration declaration{tool.name, {}};
  if (const auto* properties = member(*schema, "properties"); properties != nullptr && properties->if_object() != nullptr) {
    for (const auto& p : *properties->if_object()) {
      const auto* type = member(p.value, "type");
      if (type != nullptr && type->if_string() != nullptr && *type->if_string() == "string") declaration.string_parameters.push_back(p.key);
    }
  }
  std::string description = tool.description;
  description += (description.empty() ? "" : " ") + std::string{"Today's date is "} + settings_.today() + ".";
  std::vector<JsonMember> members;
  members.push_back({"name", JsonValue::make_string(tool.name)});
  members.push_back({"description", JsonValue::make_string(std::move(description))});
  members.push_back({"parameters", std::move(*schema)});
  return ToolOffer{std::move(declaration), python_json_text(JsonValue::make_object(std::move(members)))};
}

std::optional<ToolOffer> SearchService::offer() {
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    // a search runs: the program is up, and the tool it listed stands
    const std::lock_guard listed(listed_mutex_);
    return listed_.has_value() ? offer_of(*listed_) : std::nullopt;
  }
  const bool running = ensure_started();
  const std::lock_guard listed(listed_mutex_);
  listed_.reset();
  if (!running) return std::nullopt;
  for (const McpTool& tool : client_->tools()) {
    if (tool.name != "web_search") continue;
    listed_ = tool;
    return offer_of(tool);
  }
  return std::nullopt;  // a strata-search without web_search: none
}

// Owner 2026-10-28: the model knows the world only up to its training; the date tells it how far behind that may be.
std::string SearchService::system_note() const {
  return "Today's date is " + settings_.today() +
         ". What you know may be out of date: for anything recent, or anything that may have changed, search the web with "
         "web_search before you answer.";
}

void SearchService::served_without(const std::string_view why) {
  {
    const std::lock_guard lock(said_mutex_);
    if (!said_.emplace(why).second) return;
  }
  if (settings_.log) {
    settings_.log("[strata] search: this model's prompt cannot carry the web search tool (" + std::string{why} +
                  "); its chats are served without search");
  }
}

ToolResult SearchService::run(const ToolCall& call, const std::function<bool()>& cancelled) {
  std::lock_guard lock(mutex_);
  const auto started = std::chrono::steady_clock::now();
  const auto took = [&started] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
  };
  if (!ensure_started()) {
    const std::string text = "error: the search is not available now";
    return ToolResult{false, text, text.size(), false, false, took()};
  }
  std::string arguments{"{"};
  for (std::size_t i = 0; i < call.arguments.size(); ++i) {
    if (i > 0U) arguments += ',';
    append_json_string(&arguments, call.arguments[i].name);
    arguments += ':';
    compact(call.arguments[i].value, arguments);
  }
  arguments += '}';
  McpCallResult result = client_->call(call.name, arguments, cancelled);
  if (!process_->running()) stop_child();  // it ended: the next chat starts it again
  return ToolResult{result.ok, std::move(result.text), result.chars, result.truncated, result.cancelled, took()};
}

}  // namespace secure_frontend
