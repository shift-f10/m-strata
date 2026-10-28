// A minimal case runner for the engine's tests: `binary --case NAME` runs one case, no
// argument runs all; a case fails when it records any failed expectation.
#pragma once

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace g4_test {

inline int& failures() {
  static int count = 0;
  return count;
}

inline void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures();
    std::fprintf(stderr, "FAILED: %s\n", message.c_str());
  }
}

using Case = std::pair<const char*, void (*)()>;

inline int run_cases(int argc, char** argv, const std::vector<Case>& cases) {
  const char* only = nullptr;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--case") == 0) only = argv[i + 1];
  }
  int ran = 0;
  for (const auto& [name, fn] : cases) {
    if (only != nullptr && std::strcmp(only, name) != 0) continue;
    const int before = failures();
    fn();
    std::fprintf(stderr, "%s %s\n", failures() == before ? "PASS" : "FAIL", name);
    ++ran;
  }
  if (ran == 0) {
    std::fprintf(stderr, "no such case\n");
    return 2;
  }
  return failures() == 0 ? 0 : 1;
}

}  // namespace g4_test
