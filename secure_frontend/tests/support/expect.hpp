#pragma once
// One failure counter for test binaries whose cases span several source files.
#include <iostream>
#include <string>

namespace secure_frontend_test {
inline int failures = 0;
inline void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}
}  // namespace secure_frontend_test
