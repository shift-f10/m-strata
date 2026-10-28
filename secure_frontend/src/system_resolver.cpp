#include "secure_frontend/fetch_file.hpp"

#include <cstring>
#include <string>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace secure_frontend {
namespace {
#if defined(_WIN32)
bool winsock_ready() {
  static const bool ready = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ready;
}
#endif
}  // namespace

// Spec §3.5: getaddrinfo with AI_ADDRCONFIG. Only answers are returned; the caller
// applies the address policy and never prints them (§8.1).
std::optional<std::vector<ResolvedAddress>> SystemResolver::resolve(const std::string_view host) {
#if defined(_WIN32)
  if (!winsock_ready()) return std::nullopt;
#endif
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_ADDRCONFIG;
  addrinfo* list = nullptr;
  if (getaddrinfo(std::string{host}.c_str(), "443", &hints, &list) != 0 || list == nullptr) return std::nullopt;
  std::vector<ResolvedAddress> answers;
  for (const addrinfo* item = list; item != nullptr; item = item->ai_next) {
    ResolvedAddress address;
    if (item->ai_family == AF_INET && item->ai_addrlen >= sizeof(sockaddr_in)) {
      sockaddr_in v4{};
      std::memcpy(&v4, item->ai_addr, sizeof v4);
      std::memcpy(address.bytes.data(), &v4.sin_addr, 4);
    } else if (item->ai_family == AF_INET6 && item->ai_addrlen >= sizeof(sockaddr_in6)) {
      sockaddr_in6 v6{};
      std::memcpy(&v6, item->ai_addr, sizeof v6);
      address.ipv6 = true;
      std::memcpy(address.bytes.data(), &v6.sin6_addr, 16);
    } else {
      continue;
    }
    answers.push_back(address);
  }
  freeaddrinfo(list);
  return answers;
}

}  // namespace secure_frontend
