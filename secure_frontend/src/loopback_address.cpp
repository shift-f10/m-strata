#include "secure_frontend/loopback_address.hpp"

namespace secure_frontend {

std::optional<LoopbackAddress> LoopbackAddress::from(const IpAddress& address) {
  if (address.scope() != AddressScope::loopback) return std::nullopt;  // SP1 classifies from the bytes (§3.7)
  return LoopbackAddress(address);
}

}  // namespace secure_frontend
