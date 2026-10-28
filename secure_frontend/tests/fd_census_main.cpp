// fetch.release_spawn_has_no_fd4 (spec §10.5): a stand-in downloader that answers
// the probe and exits 0 only if no descriptor above 2 is open at execve. It
// counts with fcntl(F_GETFD) for 3..1024 because Landlock leaves no /proc access.
#include <cstdio>
#include <cstring>
#include <fcntl.h>

int main(int argc, char** argv) {
  for (int fd = 3; fd <= 1024; ++fd) {
    if (::fcntl(fd, F_GETFD) != -1) return 97;
  }
  if (argc == 3 && std::strcmp(argv[1], "-q") == 0 && std::strcmp(argv[2], "--version") == 0) {
    std::fputs("curl 8.4.0 (census) libcurl/8.4.0 OpenSSL/3.0.13\n", stdout);
    return 0;
  }
  return 98;
}
