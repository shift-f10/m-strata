#pragma once

#include "secure_frontend/loopback_address.hpp"

#include <cstdint>
#include <string_view>
#include <variant>

namespace secure_frontend {

inline constexpr std::string_view kRunNeedsLoopbackMessage =
    "strata run needs a server on a loopback address; set listener.host to 127.0.0.1 or ::1.";
inline constexpr std::string_view kRunServerFileInvalidMessage = "The server file is not valid.";

struct RunTarget final {
  LoopbackAddress address;
  std::uint16_t port;
};

struct RunTargetError final {
  std::string_view message;  // one of the two literals above
};

// Reads PX's server.json: "host" (IP literal, required), "port" (1-65535, required); pid, start time,
// version and any other keys are ignored. Never echoes the file's values.
[[nodiscard]] std::variant<RunTarget, RunTargetError> run_target_from_server_json(std::string_view json);

}  // namespace secure_frontend
