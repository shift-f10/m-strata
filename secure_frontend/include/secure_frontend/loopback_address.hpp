#pragma once

#include "secure_frontend/config.hpp"

#include <optional>
#include <utility>

namespace secure_frontend {

// Built only from an SP1 IpAddress the classifier reports as loopback (127.0.0.0/8, ::1, ::ffff:127.0.0.0/104).
// No conversion from IpAddress exists, so passing a remote address to the client is a compile error at the call site.
class LoopbackAddress final {
 public:
  [[nodiscard]] static std::optional<LoopbackAddress> from(const IpAddress& address);
  [[nodiscard]] const IpAddress& ip() const noexcept { return ip_; }

 private:
  explicit LoopbackAddress(IpAddress ip) : ip_(std::move(ip)) {}
  IpAddress ip_;
};

}  // namespace secure_frontend
