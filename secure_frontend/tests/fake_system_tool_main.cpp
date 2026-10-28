// A stand-in for a Windows system tool (schtasks) in the service tests. It appends
// its arguments, space-separated, as one line to <its own path>.calls (the .exe
// replaced) and exits 0. A test copies it into a directory under the tool's name;
// never installed. Windows only (GetModuleFileNameW names the copy that ran).
#include "secure_frontend/win32_prelude.hpp"

#include <filesystem>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
  wchar_t self[32768];
  const DWORD length = GetModuleFileNameW(nullptr, self, 32768);
  if (length == 0U || length >= 32768U) return 2;
  std::filesystem::path record{std::wstring{self, length}};
  record.replace_extension(".calls");
  std::string line;
  for (int i = 1; i < argc; ++i) {
    if (i > 1) line += ' ';
    line += argv[i];
  }
  std::ofstream out(record, std::ios::app | std::ios::binary);
  out << line << '\n';
  return out ? 0 : 2;
}
