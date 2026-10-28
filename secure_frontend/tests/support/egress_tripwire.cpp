// Test-only LD_PRELOAD egress tripwire (spec §6.2). Every outbound attempt is
// appended to $STRATA_EGRESS_RECORD and fails with EACCES.
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace {
void record(const char* what) {
  const char* path = std::getenv("STRATA_EGRESS_RECORD");
  if (path == nullptr || *path == '\0') return;
  const int fd = ::open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) return;
  const std::size_t n = std::strlen(what);
  if (::write(fd, what, n) == static_cast<ssize_t>(n)) {
    // The newline is best-effort, but g++ -O3 warns on a discarded
    // warn_unused_result call even with a (void) cast, so keep the value.
    const ssize_t newline = ::write(fd, "\n", 1);
    (void)newline;
  }
  ::close(fd);
}
template <typename Fn>
Fn next_symbol(const char* name) { return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name)); }
}  // namespace

extern "C" {

int connect(int, const sockaddr*, socklen_t) {
  record("connect");
  errno = EACCES;
  return -1;
}

ssize_t sendto(int fd, const void* buffer, size_t length, int flags, const sockaddr* address, socklen_t address_length) {
  if (address != nullptr) { record("sendto"); errno = EACCES; return -1; }
  using Fn = ssize_t (*)(int, const void*, size_t, int, const sockaddr*, socklen_t);
  static const Fn real = next_symbol<Fn>("sendto");
  return real(fd, buffer, length, flags, address, address_length);
}

ssize_t sendmsg(int fd, const msghdr* message, int flags) {
  if (message != nullptr && message->msg_name != nullptr) { record("sendmsg"); errno = EACCES; return -1; }
  using Fn = ssize_t (*)(int, const msghdr*, int);
  static const Fn real = next_symbol<Fn>("sendmsg");
  return real(fd, message, flags);
}

int getaddrinfo(const char*, const char*, const addrinfo*, addrinfo** result) {
  record("getaddrinfo");
  if (result != nullptr) *result = nullptr;
  return EAI_FAIL;
}

hostent* gethostbyname(const char*) {
  record("gethostbyname");
  h_errno = NO_RECOVERY;
  return nullptr;
}

int socket(int domain, int type, int protocol) {
  if (domain == AF_UNIX) { record("socket_unix"); errno = EACCES; return -1; }
  if (domain == AF_NETLINK) { record("socket_netlink"); errno = EACCES; return -1; }
  if ((type & 0xf) == SOCK_DGRAM) { record("socket_dgram"); errno = EACCES; return -1; }
  using Fn = int (*)(int, int, int);
  static const Fn real = next_symbol<Fn>("socket");
  return real(domain, type, protocol);
}

}  // extern "C"
