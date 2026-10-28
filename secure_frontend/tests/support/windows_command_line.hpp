#pragma once

// One Windows command-line quoter for every test launcher (SP1's
// process_runner, the SP2 server harness, secure_frontend_non_elevated_run).

#if defined(_WIN32)

#include <cstddef>
#include <string>

namespace secure_frontend_test {

// Encodes one argument so CommandLineToArgvW (and the MSVC/UCRT argv parser)
// returns it unchanged: unquoted when it has no space, tab, newline, vertical
// tab or quote; otherwise quoted, with backslashes doubled before a quote and
// at the end, and each embedded quote escaped.
inline std::wstring quote_windows_argument(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
  std::wstring out = L"\"";
  for (auto it = argument.begin();; ++it) {
    std::size_t backslashes = 0;
    while (it != argument.end() && *it == L'\\') { ++it; ++backslashes; }
    if (it == argument.end()) { out.append(backslashes * 2, L'\\'); break; }
    if (*it == L'"') { out.append(backslashes * 2 + 1, L'\\'); out.push_back(L'"'); }
    else { out.append(backslashes, L'\\'); out.push_back(*it); }
  }
  out.push_back(L'"');
  return out;
}

}  // namespace secure_frontend_test

#endif  // defined(_WIN32)
