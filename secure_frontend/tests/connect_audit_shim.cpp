// LD_PRELOAD shim for the DL-01/PRIV-01 socket audit (spec §10.3). Records every
// non-loopback connect/sendto/sendmsg/sendmmsg destination and every name or
// address lookup that can leave the host (getaddrinfo, getnameinfo,
// gethostbyname[_r], gethostbyaddr) to $SF_CONNECT_AUDIT; with
// $SF_CONNECT_AUDIT_BLOCK set it refuses them, so a test can prove the shim works
// (positive control) without any real network access.
// With $SF_CONNECT_AUDIT_LOADED set it also appends "loaded" there when it is
// mapped, so an empty audit is proven to come from a process the shim was in.
// Out of reach by design: raw syscall(SYS_connect)/sendto and io_uring bypass libc;
// DL-01 relies on the link-set check and the record channel for those.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

void write_line(const char* file, const char* text) {
  const int fd = ::open(file, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) return;
  (void)!::write(fd, text, std::strlen(text));
  ::close(fd);
}

__attribute__((constructor)) void announce_loaded() {
  if (const char* file = std::getenv("SF_CONNECT_AUDIT_LOADED")) write_line(file, "loaded\n");
}

void audit(const char* what, const char* detail) {
  const char* file = std::getenv("SF_CONNECT_AUDIT");
  if (file == nullptr) return;
  char line[512];
  const int n = std::snprintf(line, sizeof line, "%s %s\n", what, detail);
  if (n > 0) write_line(file, line);
}

bool blocking() { return std::getenv("SF_CONNECT_AUDIT_BLOCK") != nullptr; }

bool loopback_v4(const in_addr& address) { return (ntohl(address.s_addr) >> 24U) == 127U; }

bool loopback_v6(const in6_addr& address) {
  static const unsigned char kLoopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  return std::memcmp(&address, kLoopback, 16) == 0;
}

// Ruling I17: only AF_INET/AF_INET6 can leave the host; every other family
// (AF_UNIX, AF_NETLINK, ...) and a missing address are local.
bool loopback(const sockaddr* address) {
  if (address == nullptr) return true;
  if (address->sa_family == AF_INET) return loopback_v4(reinterpret_cast<const sockaddr_in*>(address)->sin_addr);
  if (address->sa_family == AF_INET6) return loopback_v6(reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr);
  return true;
}

const char* family_name(const int family) { return family == AF_INET6 ? "inet6" : "inet"; }

bool local_name(const char* node) {
  return node == nullptr || std::strcmp(node, "localhost") == 0 || std::strncmp(node, "127.", 4) == 0 || std::strcmp(node, "::1") == 0;
}

template <typename Fn>
Fn next(const char* name) {
  return reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, name));
}

}  // namespace

extern "C" {

int connect(int fd, const sockaddr* address, socklen_t length) {
  static const auto real = next<int (*)(int, const sockaddr*, socklen_t)>("connect");
  if (!loopback(address)) {
    audit("connect", family_name(address->sa_family));
    if (blocking()) {
      errno = ECONNREFUSED;
      return -1;
    }
  }
  if (real == nullptr) {
    errno = ENOSYS;
    return -1;
  }
  return real(fd, address, length);
}

ssize_t sendto(int fd, const void* buffer, size_t size, int flags, const sockaddr* address, socklen_t length) {
  static const auto real = next<ssize_t (*)(int, const void*, size_t, int, const sockaddr*, socklen_t)>("sendto");
  if (!loopback(address)) {
    audit("sendto", "datagram");
    if (blocking()) {
      errno = ENETUNREACH;
      return -1;
    }
  }
  if (real == nullptr) {
    errno = ENOSYS;
    return -1;
  }
  return real(fd, buffer, size, flags, address, length);
}

// A message without msg_name goes to the connected peer (its connect was audited).
ssize_t sendmsg(int fd, const msghdr* message, int flags) {
  static const auto real = next<ssize_t (*)(int, const msghdr*, int)>("sendmsg");
  if (message != nullptr && !loopback(static_cast<const sockaddr*>(message->msg_name))) {
    audit("sendmsg", "datagram");
    if (blocking()) {
      errno = ENETUNREACH;
      return -1;
    }
  }
  if (real == nullptr) {
    errno = ENOSYS;
    return -1;
  }
  return real(fd, message, flags);
}

int sendmmsg(int fd, mmsghdr* messages, unsigned int count, int flags) {
  static const auto real = next<int (*)(int, mmsghdr*, unsigned int, int)>("sendmmsg");
  for (unsigned int i = 0; messages != nullptr && i < count; ++i) {
    if (!loopback(static_cast<const sockaddr*>(messages[i].msg_hdr.msg_name))) {
      audit("sendmmsg", "datagram");
      if (blocking()) {
        errno = ENETUNREACH;
        return -1;
      }
      break;
    }
  }
  if (real == nullptr) {
    errno = ENOSYS;
    return -1;
  }
  return real(fd, messages, count, flags);
}

int getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** result) {
  static const auto real = next<int (*)(const char*, const char*, const addrinfo*, addrinfo**)>("getaddrinfo");
  if (!local_name(node)) {
    audit("getaddrinfo", node);
    if (blocking()) return EAI_NONAME;
  }
  if (real == nullptr) return EAI_SYSTEM;
  return real(node, service, hints, result);
}

// A reverse lookup of a non-local address can query DNS unless only the numeric form is asked for.
int getnameinfo(const sockaddr* address, socklen_t length, char* host, socklen_t host_length, char* service,
                socklen_t service_length, int flags) {
  static const auto real = next<int (*)(const sockaddr*, socklen_t, char*, socklen_t, char*, socklen_t, int)>("getnameinfo");
  if (host != nullptr && (flags & NI_NUMERICHOST) == 0 && !loopback(address)) {
    audit("getnameinfo", family_name(address->sa_family));
    if (blocking()) return EAI_FAIL;
  }
  if (real == nullptr) return EAI_SYSTEM;
  return real(address, length, host, host_length, service, service_length, flags);
}

hostent* gethostbyname(const char* name) {
  static const auto real = next<hostent* (*)(const char*)>("gethostbyname");
  if (!local_name(name)) {
    audit("gethostbyname", name);
    if (blocking()) {
      h_errno = HOST_NOT_FOUND;
      return nullptr;
    }
  }
  if (real == nullptr) {
    h_errno = NO_RECOVERY;
    return nullptr;
  }
  return real(name);
}

int gethostbyname_r(const char* name, hostent* entry, char* buffer, size_t length, hostent** result, int* error) {
  static const auto real = next<int (*)(const char*, hostent*, char*, size_t, hostent**, int*)>("gethostbyname_r");
  if (!local_name(name)) {
    audit("gethostbyname_r", name);
    if (blocking()) {
      if (result != nullptr) *result = nullptr;
      if (error != nullptr) *error = HOST_NOT_FOUND;
      return ENOENT;
    }
  }
  if (real == nullptr) {
    if (result != nullptr) *result = nullptr;
    return ENOSYS;
  }
  return real(name, entry, buffer, length, result, error);
}

hostent* gethostbyaddr(const void* address, socklen_t length, int type) {
  static const auto real = next<hostent* (*)(const void*, socklen_t, int)>("gethostbyaddr");
  const bool local = address == nullptr || (type == AF_INET && length >= sizeof(in_addr) && loopback_v4(*static_cast<const in_addr*>(address))) ||
                     (type == AF_INET6 && length >= sizeof(in6_addr) && loopback_v6(*static_cast<const in6_addr*>(address))) ||
                     (type != AF_INET && type != AF_INET6);
  if (!local) {
    audit("gethostbyaddr", family_name(type));
    if (blocking()) {
      h_errno = HOST_NOT_FOUND;
      return nullptr;
    }
  }
  if (real == nullptr) {
    h_errno = NO_RECOVERY;
    return nullptr;
  }
  return real(address, length, type);
}

}  // extern "C"
