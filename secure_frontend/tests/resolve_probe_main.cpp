// The socket audit's positive control (spec §10.1): one call of every interposed
// lookup and send, each to a non-loopback name or literal (TEST-NET-1, 192.0.2.1).
// Run only under the shim with SF_CONNECT_AUDIT_BLOCK set: each call is refused and
// recorded, so nothing leaves the host. A call is made only when the symbol the
// process would use resolves into the shim (dladdr); otherwise it is skipped (its
// audit line is then missing and the test fails) and never reaches libc.
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

bool from_shim(const char* name) {
  void* symbol = ::dlsym(RTLD_DEFAULT, name);
  Dl_info info{};
  return symbol != nullptr && ::dladdr(symbol, &info) != 0 && info.dli_fname != nullptr &&
         std::strstr(info.dli_fname, "secure_frontend_connect_audit") != nullptr;
}

sockaddr_in test_net(const unsigned short port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  (void)::inet_pton(AF_INET, "192.0.2.1", &address.sin_addr);
  return address;
}

}  // namespace

int main() {
  if (std::getenv("SF_CONNECT_AUDIT_BLOCK") == nullptr) return 2;  // never without the block
  if (from_shim("getaddrinfo")) {
    addrinfo* result = nullptr;
    if (::getaddrinfo("fixture.example", "443", nullptr, &result) == 0 && result != nullptr) ::freeaddrinfo(result);
  }
  if (from_shim("gethostbyname")) (void)::gethostbyname("fixture.example");
  if (from_shim("gethostbyname_r")) {
    hostent entry{};
    hostent* found = nullptr;
    char buffer[1024];
    int error = 0;
    (void)::gethostbyname_r("fixture.example", &entry, buffer, sizeof buffer, &found, &error);
  }
  const auto remote = test_net(443);
  if (from_shim("gethostbyaddr")) (void)::gethostbyaddr(&remote.sin_addr, sizeof remote.sin_addr, AF_INET);
  if (from_shim("getnameinfo")) {
    char host[256];
    (void)::getnameinfo(reinterpret_cast<const sockaddr*>(&remote), sizeof remote, host, sizeof host, nullptr, 0, 0);
  }
  if (from_shim("connect")) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
      (void)::connect(fd, reinterpret_cast<const sockaddr*>(&remote), sizeof remote);
      ::close(fd);
    }
  }
  const int udp = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (udp >= 0) {
    const auto dns = test_net(53);
    char byte = 0;
    if (from_shim("sendto")) (void)::sendto(udp, &byte, 1, 0, reinterpret_cast<const sockaddr*>(&dns), sizeof dns);
    iovec vector{&byte, 1};
    msghdr message{};
    message.msg_name = const_cast<sockaddr_in*>(&dns);
    message.msg_namelen = sizeof dns;
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    if (from_shim("sendmsg")) (void)::sendmsg(udp, &message, 0);
    mmsghdr batch{};
    batch.msg_hdr = message;
    if (from_shim("sendmmsg")) (void)::sendmmsg(udp, &batch, 1, 0);
    ::close(udp);
  }
  return 0;
}
