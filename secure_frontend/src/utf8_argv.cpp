#include "secure_frontend/utf8_argv.hpp"

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <shellapi.h>

#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <utility>
#endif

namespace secure_frontend {

std::optional<std::vector<std::string>> utf8_command_line(const int argc, char** argv) {
#if defined(_WIN32)
  (void)argc;
  (void)argv;
  int count = 0;
  wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
  if (wide == nullptr) return std::nullopt;
  std::vector<std::string> arguments;
  bool valid = true;
  for (int index = 0; index < count && valid; ++index) {
    const int length = static_cast<int>(std::wcslen(wide[index]));
    if (length == 0) {
      arguments.emplace_back();
      continue;
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[index], length, nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<std::size_t>(std::max(size, 0)), '\0');
    valid = size > 0 && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[index], length, text.data(), size, nullptr, nullptr) == size;
    arguments.push_back(std::move(text));
  }
  LocalFree(wide);
  if (!valid) return std::nullopt;
  return arguments;
#else
  return std::vector<std::string>(argv, argv + argc);
#endif
}

}  // namespace secure_frontend
