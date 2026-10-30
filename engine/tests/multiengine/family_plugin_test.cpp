// tests/multiengine/family_plugin_test.cpp - the launcher's engine family plugins (the owner, 2026-10-30: "our engine
// should have a published way so that we or another user can use those guidelines and create a library that we can
// just add later on"; docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md).
//
// usage: multiengine_family_test <multiengine> <testfam lib> <badabi lib> <wrongid lib> <nosym lib>
//
// A scratch install holds a copy of the launcher in engine/ and plugins/<id>/multiengine_<id> copies of the fake plugin
// (tests/multiengine/fake_family.cpp); each case runs the launcher there and reads its exit code and output:
//   - a plugin with the right SHA-256 runs, handed only its own arguments, after the launcher's process steps (--gpu);
//   - a wrong SHA-256, another ABI major, another id, no strata_family_info, no such plugin, an id that is not a plain
//     name, and a --family without its --family-sha256 are each refused, naming why, before the plugin runs.
#include "../../src/prepare/sha256.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

#if defined(_WIN32)
const char* const kLibraryPrefix = "multiengine_";
const char* const kLibrarySuffix = ".dll";
#else
const char* const kLibraryPrefix = "libmultiengine_";
const char* const kLibrarySuffix = ".so";
#endif

std::string read_all(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

std::string sha256_of(const fs::path& file) {
  const std::string bytes = read_all(file);
  strata_prepare::Sha256 hash;
  hash.update(bytes.data(), bytes.size());
  return hash.hex();
}

struct Ran {
  int code = -1;
  std::string output;
};

// The launcher in `folder`, its output (stdout and stderr) read back from a file.
Ran run(const fs::path& folder, const fs::path& launcher, const std::string& arguments) {
  const fs::path out = folder / "out.txt";
  std::string command = "\"" + launcher.string() + "\" " + arguments + " > \"" + out.string() + "\" 2>&1";
#if defined(_WIN32)
  command = "\"" + command + "\"";   // cmd /c keeps the inner quotes when the whole line is quoted
#endif
  const int status = std::system(command.c_str());
  Ran ran;
#if defined(_WIN32)
  ran.code = status;
#else
  ran.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  ran.output = read_all(out);
  // Windows' text-mode stdout ends a line with \r\n
  std::string lf;
  for (const char c : ran.output) {
    if (c != '\r') lf += c;
  }
  ran.output = lf;
  return ran;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 6) {
    std::cerr << "usage: multiengine_family_test <multiengine> <testfam> <badabi> <wrongid> <nosym>\n";
    return 2;
  }
  const fs::path folder = fs::temp_directory_path() / ("multiengine-family-test-" + std::to_string(std::rand()) + "-" +
                                                       std::to_string(fs::file_size(argv[1])));
  std::error_code ec;
  fs::remove_all(folder, ec);
  fs::create_directories(folder);
  fs::create_directories(folder / "engine");
  const fs::path launcher = folder / "engine" / fs::path(argv[1]).filename();
  fs::copy_file(argv[1], launcher);
  const std::vector<std::pair<std::string, const char*>> plugins{
      {"testfam", argv[2]}, {"badabi", argv[3]}, {"wrongid", argv[4]}, {"nosym", argv[5]}};
  std::vector<std::string> sums;
  for (const auto& [id, library] : plugins) {
    const fs::path into = folder / "plugins" / id;
    fs::create_directories(into);
    const fs::path copy = into / (std::string{kLibraryPrefix} + id + kLibrarySuffix);
    fs::copy_file(library, copy);
    sums.push_back(sha256_of(copy));
  }

  // the plugin runs: only its own arguments, after the launcher's process steps (--gpu as the visible GPUs)
  const Ran ok = run(folder, launcher, "--family testfam --family-sha256 " + sums[0] + " --native model.gguf --gpu 3");
  expect(ok.code == 7, "the plugin's exit code is the process's: " + std::to_string(ok.code) + " " + ok.output);
  expect(ok.output.find("fake family: 4 args: --native model.gguf --gpu 3\n") != std::string::npos,
         "it is handed its own arguments only: " + ok.output);
  expect(ok.output.find("visible gpus: 3\n") != std::string::npos, "after the launcher's process steps: " + ok.output);
  // the SHA-256 is compared without regard to case
  std::string upper = sums[0];
  for (char& c : upper) c = static_cast<char>(c >= 'a' && c <= 'f' ? c - 'a' + 'A' : c);
  expect(run(folder, launcher, "--family testfam --family-sha256 " + upper).code == 7, "an upper-case SHA-256 is the same");

  // refusals, each naming why, and the plugin never runs
  const auto refused = [&](const std::string& arguments, const std::string& why, const std::string& what) {
    const Ran ran = run(folder, launcher, arguments);
    expect(ran.code == 1 && ran.output.find(why) != std::string::npos && ran.output.find("fake family") == std::string::npos,
           what + ": " + std::to_string(ran.code) + " " + ran.output);
  };
  std::string wrong = sums[0];
  wrong[0] = wrong[0] == '0' ? '1' : '0';
  refused("--family testfam --family-sha256 " + wrong, "SHA-256", "a library whose SHA-256 is not the pinned one");
  refused("--family badabi --family-sha256 " + sums[1], "ABI", "another ABI major");
  refused("--family wrongid --family-sha256 " + sums[2], "id", "an id that is not its folder's");
  refused("--family nosym --family-sha256 " + sums[3], "strata_family_info", "no strata_family_info");
  refused("--family ghost --family-sha256 " + sums[0], "cannot open", "no such plugin");
  refused("--family ../testfam --family-sha256 " + sums[0], "family id", "an id that is not a plain name");
  refused("--family testfam --native model.gguf", "--family-sha256", "--family without its --family-sha256");
  refused("--family testfam --family-sha256 1234", "--family-sha256", "a SHA-256 that is not 64 hex digits");

  fs::remove_all(folder, ec);
  if (failures == 0) std::cout << "multiengine_family_test: all passed\n";
  return failures == 0 ? 0 : 1;
}
