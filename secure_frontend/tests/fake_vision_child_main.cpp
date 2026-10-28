// strata-fake-vision-child: a test-only stand-in for `maya-vision` (SP5 M1 Task 11).
// Never installed. It speaks the pin's encoder lifecycle (upstream/serve/server.py
// VisionServer :2511-2620, :2755-2792): on start it records its argv (one "ARG" line
// each, after a "SPAWN" line) into --record-file=PATH, prints "READY ..." on stdout,
// then serves stdin: "ENC <img> <out>" -> writes the named files' sibling .sve
// embedding and answers "OK <n>"; "QUIT" -> exit 0. --mode selects the failure
// shapes the supervision tests need:
//   oversize : after the record lines, print one > 8192-byte line (never READY) and exit 3
//   diedie   : exit 3 immediately after the record lines (never READY)
//   badenc   : READY, then every ENC is answered "ERR broken" (the child stays up)
//   silentenc: READY, then ENC is never answered (the encode timeout must fire)
//   dieenc   : READY, the FIRST ENC is answered "OK 5" and the child then exits
// Unknown arguments (--mmproj PATH, --model PATH, --threads N, ...) were accepted and
// echoed while the child modelled the upstream reader; SP5-P2 Task 6 pins the grammar
// of the engine's own maya-vision at 451f9cb6 (tools/vision/strata_vision.cpp:61-79):
// the set is --mmproj/--model/--gpu/--threads/--max-tokens/--no-flash-attn/--no-warmup/
// --measure, and any other argument prints "unknown argument X" and exits 2. The
// test-only options (--record-file=, --mode=) are consumed before that check.
// Run as "strata-vision" (the name the tests install it under for a model card, upstream's encoder), it speaks
// upstream's tools/vision/strata_vision.cpp instead: --min-tokens N and --flash-attn on|off|auto join the grammar,
// and an ENC is answered "OK <tokens> <nx> <ny> <ms>" (the token count first, as upstream's server reads it).
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string option_value(const std::vector<std::string>& args, const std::string_view flag) {
  for (const auto& arg : args) {
    if (arg.starts_with(flag)) return arg.substr(flag.size());
  }
  return {};
}

// The maya-vision grammar at the pin: value-taking flags consume their value, the rest
// are bare. Anything else is the pin's exit-2 path.
bool grammar_accepts(const std::vector<std::string>& args, const bool upstream) {
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg.starts_with("--record-file=") || arg.starts_with("--mode=")) continue;
    auto take_value = [&args, &i](const std::string& flag) {
      if (i + 1 >= args.size()) {
        std::cerr << flag << " needs a value\n";
        return false;
      }
      ++i;
      return true;
    };
    if (arg == "--mmproj" || arg == "--model") {
      if (!take_value(arg)) return false;
    } else if (arg == "--threads" || arg == "--max-tokens") {
      if (!take_value(arg)) return false;
    } else if (upstream && (arg == "--min-tokens" || arg == "--flash-attn")) {
      if (!take_value(arg)) return false;
    } else if (arg != "--gpu" && arg != "--no-flash-attn" && arg != "--no-warmup" && arg != "--measure") {
      std::cerr << "unknown argument " << arg << "\n";
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc) - 1U);
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  const std::string record = option_value(args, "--record-file=");
  const std::string mode = option_value(args, "--mode=");
  const bool upstream = argc > 0 && std::filesystem::path(argv[0]).stem() == "strata-vision";
  if (!grammar_accepts(args, upstream)) return 2;  // the pin's exit-2 path for an unknown argument

  if (!record.empty()) {
    // Binary: the tests match "SPAWN\n"; a text stream writes \r\n on Windows.
    std::ofstream out(std::filesystem::path(record), std::ios::app | std::ios::binary);  // test paths are ASCII; no u8path deprecation
    out << "SPAWN\n";
    for (const auto& arg : args) out << "ARG " << arg << "\n";
    out.flush();
  }
  std::cout.flush();
  std::cerr.flush();

  if (mode == "diedie") return 3;
  if (mode == "oversize") {
    std::cout << std::string(20000U, 'x') << "\n" << std::flush;
    return 3;
  }

  std::cout << "READY vision\n" << std::flush;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "QUIT") return 0;
    if (line.starts_with("ENC ")) {
      if (mode == "silentenc") continue;  // never answers: the encode timeout must fire
      if (mode == "badenc") {
        std::cout << "ERR broken image\n" << std::flush;
        continue;
      }
      // "ENC <img> <out>" (pin #480: names are relative to the child's cwd, no spaces).
      const auto rest = line.substr(4);
      const auto space = rest.find(' ');
      const std::string out_name = space == std::string::npos ? std::string{"out.sve"} : rest.substr(space + 1);
      std::ofstream out(std::filesystem::path(out_name), std::ios::binary);
      out.write("\x00sve0", 5);  // five bytes including the leading NUL (a << b"..." would stop at it)
      out.flush();
      std::cout << (upstream ? "OK 5 8 6 12\n" : "OK 5\n") << std::flush;
      if (mode == "dieenc") return 0;  // the first answer is also the exit
    }
  }
  return 0;  // stdin closed (the frontend's EOF path): exit like the pin's stop
}
