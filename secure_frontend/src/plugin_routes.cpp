#include "secure_frontend/plugin_routes.hpp"

#include "secure_frontend/api_errors.hpp"
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/token_store.hpp"

#include <algorithm>
#include <utility>
#include <variant>

namespace secure_frontend {
namespace {

constexpr JsonLimits kBodyLimits{.max_bytes = kModelControlBodyLimit, .max_depth = 2U, .max_container_entries = 8U,
                                 .max_string_bytes = 256U};

std::string json_text(const std::string_view text) {
  std::string out;
  append_json_string(&out, text);
  return out;
}

std::string json_or_null(const std::string_view text) { return text.empty() ? std::string{"null"} : json_text(text); }

HttpResponse json_answer(const int status, std::string body) {
  return {.status = status, .headers = {{"Content-Type", "application/json; charset=utf-8"}, {"Cache-Control", "no-store"}},
          .body = std::move(body)};
}

HttpResponse refused(const std::string& reason) { return make_error_response(ApiError::plugin_refused, {.reason = reason}); }

HttpResponse unreadable_choices() {
  return refused("plugins.json in the data folder's state folder is not one this server reads: fix it or move it away");
}

// {"id"} (an approval) or {"id", "on"} (a switch), nothing else; nullopt: not such a body.
struct PluginRequest final {
  std::string id;
  bool on{false};
};
std::optional<PluginRequest> parse_request(const std::string_view body, const bool with_on) {
  const auto parsed = parse_json(body, kBodyLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root == nullptr ? nullptr : root->if_object();
  if (members == nullptr || members->size() != (with_on ? 2U : 1U)) return std::nullopt;
  PluginRequest request;
  bool id = false;
  bool on = false;
  for (const auto& member : *members) {
    if (member.key == "id" && member.value.if_string() != nullptr) {
      request.id = *member.value.if_string();
      id = true;
    } else if (with_on && member.key == "on" && member.value.if_boolean() != nullptr) {
      request.on = *member.value.if_boolean();
      on = true;
    }
  }
  if (!id || (with_on && !on)) return std::nullopt;
  return request;
}

std::string entry_json(const PluginEntry& entry, const bool running) {
  const auto* manifest = entry.manifest ? &*entry.manifest : nullptr;
  std::string out = "{\"id\":" + json_text(entry.id);
  out += ",\"kind\":" + (manifest != nullptr ? json_text(plugin_kind_name(manifest->kind)) : std::string{"null"});
  out += ",\"name\":" + (manifest != nullptr ? json_text(manifest->name) : std::string{"null"});
  out += ",\"version\":" + (manifest != nullptr ? json_text(manifest->version) : std::string{"null"});
  out += ",\"publisher\":" + (manifest != nullptr ? json_text(manifest->publisher) : std::string{"null"});
  out += ",\"license\":" + (manifest != nullptr ? json_text(manifest->license) : std::string{"null"});
  out += ",\"about\":" + (manifest != nullptr ? json_text(manifest->about) : std::string{"null"});
  out += std::string{",\"first_party\":"} + (entry.first_party ? "true" : "false");
  out += ",\"state\":" + json_text(plugin_state_name(entry.state)) + ",\"reason\":" + json_or_null(entry.reason);
  out += std::string{",\"running\":"} + (running ? "true" : "false");
  out += ",\"manifest_sha256\":" + json_or_null(entry.pin.manifest) + ",\"files\":[";
  bool first = true;
  for (const auto& [path, sum] : entry.pin.files) {
    out += (first ? "" : ",") + std::string{"{\"path\":"} + json_text(path) + ",\"sha256\":" + json_text(sum) + "}";
    first = false;
  }
  out += "],\"switches\":[";
  first = true;
  if (manifest != nullptr) {
    for (const auto value : manifest->switches) {
      out += (first ? "" : ",") + json_text(diagnostic_switch_name(value));
      first = false;
    }
  }
  return out + "]}";
}

}  // namespace

PluginRoutes::PluginRoutes(PluginRouteSettings settings) : settings_(std::move(settings)) {}

HttpResponse PluginRoutes::answer(const RouteId route, const std::string_view body, const RequestContext& context) {
  const bool allowed = settings_.client_allowed ? settings_.client_allowed(context) : context.loopback_peer;
  if (!allowed) return make_error_response(ApiError::control_not_allowed);
  switch (route) {
    case RouteId::strata_plugins: return list();
    case RouteId::strata_plugins_approve: return approve(body);
    case RouteId::strata_plugins_switch: return change(body);
    default: return make_error_response(ApiError::not_found);
  }
}

std::optional<PluginChoices> PluginRoutes::choices() const {
  return settings_.state_dir ? read_plugin_choices(*settings_.state_dir) : std::optional<PluginChoices>{PluginChoices{}};
}

bool PluginRoutes::save(const PluginChoices& choices) {
  if (!settings_.state_dir) return false;
  const auto root = open_root(*settings_.state_dir, RootPolicy::create_private);
  const auto name = parse_relative_path(kPluginChoicesFile);
  if (!std::holds_alternative<CanonicalRoot>(root) || !std::holds_alternative<RelativePath>(name)) return false;
  return !write_private_file(std::get<CanonicalRoot>(root), std::get<RelativePath>(name), render_plugin_choices(choices),
                             TokenStoreMode::replace)
              .has_value();
}

HttpResponse PluginRoutes::list() const {
  const auto chosen = choices();
  if (!chosen) return unreadable_choices();
  std::string out = "{\"folder\":" + json_text(path_to_utf8(settings_.plugins_dir)) + ",\"editable\":" +
                    (settings_.state_dir ? "true" : "false") + ",\"plugins\":[";
  bool first = true;
  for (const auto& entry : discover_plugins(settings_.plugins_dir, settings_.first_party, *chosen)) {
    out += (first ? "" : ",") + entry_json(entry, settings_.running.contains(entry.id));
    first = false;
  }
  return json_answer(200, out + "]}\n");
}

HttpResponse PluginRoutes::approve(const std::string_view body) {
  const auto request = parse_request(body, false);
  if (!request) return make_error_response(ApiError::invalid_request);
  if (!settings_.state_dir) return refused("this server was started without a data folder to keep the approvals in");
  const std::scoped_lock lock(file_);
  auto chosen = choices();
  if (!chosen) return unreadable_choices();
  const auto entries = discover_plugins(settings_.plugins_dir, settings_.first_party, *chosen);
  const auto entry = std::find_if(entries.begin(), entries.end(), [&request](const PluginEntry& item) { return item.id == request->id; });
  if (entry == entries.end()) return refused("there is no plugin " + request->id.substr(0, 64) + " in the plugins folder");
  if (entry->state == PluginState::on || entry->state == PluginState::off) {
    return refused(entry->first_party ? "it is already trusted: this install was built with it" : "it is already approved");
  }
  if (entry->state == PluginState::refused) return refused(entry->reason);
  // What is approved is what is there now: its manifest's and its files' SHA-256s, pinned.
  chosen->approved[entry->id] = entry->pin;
  chosen->enabled[entry->id] = true;
  if (!save(*chosen)) return refused("plugins.json could not be written in the data folder's state folder");
  const auto state = plugin_kind_runs(entry->manifest->kind) ? PluginState::on : PluginState::refused;
  if (settings_.log) settings_.log("[strata] plugins: " + entry->id + " was approved (it applies at the next start)");
  return json_answer(200, "{\"id\":" + json_text(entry->id) + ",\"state\":" + json_text(plugin_state_name(state)) + ",\"restart_needed\":true}\n");
}

HttpResponse PluginRoutes::change(const std::string_view body) {
  const auto request = parse_request(body, true);
  if (!request) return make_error_response(ApiError::invalid_request);
  if (!settings_.state_dir) return refused("this server was started without a data folder to keep the choices in");
  const std::scoped_lock lock(file_);
  auto chosen = choices();
  if (!chosen) return unreadable_choices();
  const auto entries = discover_plugins(settings_.plugins_dir, settings_.first_party, *chosen);
  const auto entry = std::find_if(entries.begin(), entries.end(), [&request](const PluginEntry& item) { return item.id == request->id; });
  if (entry == entries.end()) return refused("there is no plugin " + request->id.substr(0, 64) + " in the plugins folder");
  if (entry->state == PluginState::waiting || entry->state == PluginState::changed) return refused("approve it first: " + entry->reason);
  if (entry->state == PluginState::refused) return refused(entry->reason);
  chosen->enabled[entry->id] = request->on;
  if (!save(*chosen)) return refused("plugins.json could not be written in the data folder's state folder");
  if (settings_.log) {
    settings_.log("[strata] plugins: " + entry->id + " was turned " + (request->on ? "on" : "off") + " (it applies at the next start)");
  }
  return json_answer(200, "{\"id\":" + json_text(entry->id) + ",\"state\":" + (request->on ? "\"on\"" : "\"off\"") + ",\"restart_needed\":true}\n");
}

}  // namespace secure_frontend
