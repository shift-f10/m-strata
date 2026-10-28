// Calls the outbound functions the tripwire must record and deny.
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

int main() {
  addrinfo* result = nullptr;
  const int rc = getaddrinfo("localhost", "80", nullptr, &result);
  std::printf("getaddrinfo=%s\n", rc == EAI_FAIL ? "denied" : "allowed");
  if (rc == 0) freeaddrinfo(result);
  const hostent* host = gethostbyname("localhost");
  std::printf("gethostbyname=%s\n", host == nullptr ? "denied" : "allowed");
  const int tcp = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(9);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const int connected = tcp >= 0 ? connect(tcp, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) : -1;
  std::printf("connect=%s\n", connected != 0 && errno == EACCES ? "denied" : "allowed");
  if (tcp >= 0) close(tcp);
  const int local = socket(AF_UNIX, SOCK_STREAM, 0);
  std::printf("unix=%s\n", local < 0 && errno == EACCES ? "denied" : "allowed");
  const int udp = socket(AF_INET, SOCK_DGRAM, 0);
  std::printf("udp=%s\n", udp < 0 && errno == EACCES ? "denied" : "allowed");
  return 0;
}
