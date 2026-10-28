#include "secure_frontend/socket_io.hpp"
#include "secure_frontend/strata_cli.hpp"
#include "secure_frontend/utf8_argv.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

int main(int argc, char** argv) {
  // Spec §10, as strata-secure-server: Winsock once per process. `strata serve`
  // binds through the same HttpSocketServer::bind, which never calls WSAStartup.
  const secure_frontend::WinsockGuard winsock;
#if !defined(_WIN32)
  // SP4 §3.2: strata writes to pipes whose reader (the downloader) can die first;
  // that must be an EPIPE, never a SIGPIPE death.
  std::signal(SIGPIPE, SIG_IGN);
#endif
  // Spec A2 (as strata-secure-server): on Windows every argument is UTF-8 from
  // the wide command line, never the ANSI code page; the verbs decode paths
  // with path_from_utf8.
  const auto command_line = secure_frontend::utf8_command_line(argc, argv);
  if (!command_line) {
    std::cerr << "Error: the command line is not valid Unicode\n";
    return EXIT_FAILURE;
  }
  const std::vector<std::string> args(command_line->size() > 1U ? command_line->begin() + 1 : command_line->end(),
                                      command_line->end());
#if defined(_WIN32)
  const bool interactive = _isatty(_fileno(stdin)) != 0;
#else
  const bool interactive = ::isatty(STDIN_FILENO) == 1;
#endif
  secure_frontend::CliStreams streams{std::cin, std::cout, std::cerr, interactive};
  secure_frontend::CliServices services;
  return secure_frontend::run_strata(args, streams, services);
}
