#include "secure_frontend/run_target.hpp"

#include "secure_frontend/config.hpp"
#include "secure_frontend/json.hpp"

namespace secure_frontend {

std::variant<RunTarget, RunTargetError> run_target_from_server_json(const std::string_view json) {
  const RunTargetError invalid{kRunServerFileInvalidMessage};
  const auto parsed = parse_json(json, {.max_bytes = 4096U, .max_depth = 2U, .max_container_entries = 16U, .max_string_bytes = 256U});
  const auto* root = std::get_if<JsonValue>(&parsed);
  const auto* members = root != nullptr ? root->if_object() : nullptr;
  if (members == nullptr) return invalid;
  const std::string* host = nullptr;
  const std::int64_t* port = nullptr;
  for (const auto& member : *members) {
    if (member.key == "host") host = member.value.if_string();
    if (member.key == "port") port = member.value.if_integer();
  }
  if (host == nullptr || port == nullptr || *port < 1 || *port > 65535) return invalid;
  const auto address = parse_listener_address(*host);
  if (!address) return invalid;
  auto loopback = LoopbackAddress::from(*address);
  if (!loopback) return RunTargetError{kRunNeedsLoopbackMessage};
  return RunTarget{std::move(*loopback), static_cast<std::uint16_t>(*port)};
}

}  // namespace secure_frontend
