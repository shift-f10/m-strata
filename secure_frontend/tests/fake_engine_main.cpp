// A stand-in for the Strata --serve engine used by the end-to-end tests. It
// never touches a model. Options (passed by the server via engine.args):
//   --marker=PATH        create PATH as soon as the process starts
//   --dump=PATH          write "argv:<arg>" and "env:<k=v>" lines to PATH
//   --ready-delay-ms=N   wait N ms before printing READY
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
  _setmode(_fileno(stdout), _O_BINARY);
  char** environment = _environ;
#else
  char** environment = environ;
#endif
  std::string marker;
  std::string dump;
  long ready_delay_ms = 0;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument.rfind("--marker=", 0) == 0) marker = argument.substr(9);
    else if (argument.rfind("--dump=", 0) == 0) dump = argument.substr(7);
    else if (argument.rfind("--ready-delay-ms=", 0) == 0) ready_delay_ms = std::strtol(argument.c_str() + 17, nullptr, 10);
  }
  if (!marker.empty()) std::ofstream(marker) << "started\n";
  if (!dump.empty()) {
    std::ofstream out(dump, std::ios::binary);
    for (int index = 0; index < argc; ++index) out << "argv:" << argv[index] << '\n';
    for (char** entry = environment; entry != nullptr && *entry != nullptr; ++entry) out << "env:" << *entry << '\n';
  }
  if (ready_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ready_delay_ms));
  std::cout << "READY 4096 stop\n" << std::flush;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("GEN ", 0) == 0) {
      const unsigned long count = std::strtoul(line.c_str() + 4, nullptr, 10);
      for (unsigned long i = 0; i < count; ++i) std::cout << "T 0\n";
      std::cout << "DONE " << count << '\n' << std::flush;
    } else if (line == "STOP") {
      std::cout << "DONE 0 cancel\n" << std::flush;
    }
  }
  return 0;
}
