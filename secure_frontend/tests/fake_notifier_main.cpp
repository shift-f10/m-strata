// A stand-in notifier plugin for the notifier host's tests (plugin spec 2026-10-30, section 10): it appends every line
// it reads to the --record file, answers the test line with {"ok": true, "reply"} naming its own command line, working
// folder and the FAKE_NOTIFIER_TOKEN it got, and answers an event too with --answer-events. --no-test-answer leaves
// the test unanswered; --exit-after N ends it after N lines. Never installed.
#include "secure_frontend/crt_environment.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

std::string json_text(const std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    if (static_cast<unsigned char>(c) < 0x20U) continue;
    out += c;
  }
  return out + "\"";
}

// The value of "event" in a line the host wrote (its own JSON: no escapes in an event name).
std::string event_of(const std::string& line) {
  const auto at = line.find("\"event\":\"");
  if (at == std::string::npos) return {};
  const auto start = at + 9U;
  return line.substr(start, line.find('"', start) - start);
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  (void)_setmode(_fileno(stdin), _O_BINARY);
  (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
  std::string record;
  std::string command_line;
  bool answer_events = false;
  bool answer_test = true;
  long exit_after = -1;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    command_line += (i > 1 ? " " : "") + std::string{arg};
    if (arg == "--record" && i + 1 < argc) record = argv[i + 1];
    if (arg == "--answer-events") answer_events = true;
    if (arg == "--no-test-answer") answer_test = false;
    if (arg == "--exit-after" && i + 1 < argc) exit_after = std::stol(argv[i + 1]);
  }
  const char* token = secure_frontend::crt_getenv("FAKE_NOTIFIER_TOKEN");
  long lines = 0;
  for (std::string line; std::getline(std::cin, line);) {
    if (!record.empty()) {
      std::ofstream out(record, std::ios::app | std::ios::binary);
      out << line << '\n';
    }
    if (line.find("\"test\":true") != std::string::npos) {
      if (answer_test) {
        std::cout << "{\"ok\":true,\"reply\":"
                  << json_text("args: " + command_line + "; cwd: " + std::filesystem::current_path().string() + "; token: " +
                               (token != nullptr ? token : "none"))
                  << "}\n" << std::flush;
      }
    } else if (answer_events) {
      std::cout << "{\"event\":" << json_text(event_of(line)) << ",\"ok\":true,\"reply\":\"noted\"}\n" << std::flush;
    }
    if (exit_after >= 0 && ++lines >= exit_after) return 0;
  }
  return 0;
}
