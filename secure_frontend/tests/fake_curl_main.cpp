// SP4 §10.1: the fake downloader. Accepts exactly the §3.3 grammar (exit 98 and
// an "invalid" record otherwise), records argv/env/stdin over the inherited
// record descriptor (fd 4 on POSIX; CRT block entry 4 on Windows) and replays
// a scripted response found in <own directory>/scenario/scenario.txt.
// It never opens a socket.
#include "support/curl_scenario.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <fcntl.h>
#include <io.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

namespace curl = secure_frontend_test::curl;

std::filesystem::path own_directory(const char* argv0) {
#if defined(_WIN32)
  (void)argv0;
  std::wstring buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(length);
  return std::filesystem::path(buffer).parent_path();
#else
  return std::filesystem::path(argv0).parent_path();
#endif
}

std::vector<std::string> environment() {
  std::vector<std::string> out;
#if defined(_WIN32)
  wchar_t* block = GetEnvironmentStringsW();
  for (const wchar_t* p = block; p != nullptr && *p != L'\0'; p += std::wcslen(p) + 1) {
    out.push_back(std::filesystem::path(p).string());
  }
  if (block != nullptr) FreeEnvironmentStringsW(block);
#else
  for (char** p = environ; p != nullptr && *p != nullptr; ++p) out.emplace_back(*p);
#endif
  return out;
}

void write_record(const std::string& line) {
#if defined(_WIN32)
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  GetStartupInfoW(&startup);
  if (startup.lpReserved2 == nullptr || startup.cbReserved2 < 4) return;
  int count = 0;
  std::memcpy(&count, startup.lpReserved2, sizeof count);
  if (count < 5 || startup.cbReserved2 < 4U + static_cast<unsigned>(count) * (1U + sizeof(HANDLE))) return;
  const BYTE* flags = startup.lpReserved2 + 4;
  HANDLE handle = nullptr;
  std::memcpy(&handle, startup.lpReserved2 + 4 + count + 4 * sizeof(HANDLE), sizeof handle);
  if ((flags[4] & 0x01U) == 0 || handle == nullptr || handle == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  (void)WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
#else
  if (fcntl(4, F_GETFD) == -1) return;
  std::size_t offset = 0;
  while (offset < line.size()) {
    const ssize_t n = write(4, line.data() + offset, line.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return;
    offset += static_cast<std::size_t>(n);
  }
#endif
}

std::string read_stdin() {
  std::string input;
  char buffer[4096];
  for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, stdin)) > 0;) input.append(buffer, n);
  return input;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string attempt_write(const std::string& path) {
#if defined(_WIN32)
  HANDLE h = CreateFileW(std::filesystem::path(path).wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::to_string(GetLastError());
  CloseHandle(h);
  return "0";
#else
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return std::to_string(errno);
  close(fd);
  return "0";
#endif
}

std::string attempt_read(const std::string& path) {
#if defined(_WIN32)
  HANDLE h =
      CreateFileW(std::filesystem::path(path).wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::to_string(GetLastError());
  CloseHandle(h);
  return "0";
#else
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::to_string(errno);
  close(fd);
  return "0";
#endif
}

std::string integrity_level() {
#if defined(_WIN32)
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return "unknown";
  DWORD size = 0;
  GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
  std::vector<unsigned char> buffer(size);
  std::string text = "unknown";
  if (size != 0 && GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size)) {
    wchar_t* sid = nullptr;
    if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data())->Label.Sid, &sid)) {
      text = std::filesystem::path(sid).string();
      LocalFree(sid);
    }
  }
  CloseHandle(token);
  return text;
#else
  return "";
#endif
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stdin), _O_BINARY);
#endif
  const std::vector<std::string> args(argv + 1, argv + argc);
  const auto env = environment();
  const auto scenario_dir = own_directory(argv[0]) / "scenario";
  const auto scenario = curl::parse_scenario(read_file(scenario_dir / "scenario.txt"));
  std::vector<std::pair<std::string, std::string>> extras;
  if (const auto level = integrity_level(); !level.empty()) extras.emplace_back("integrity", level);
  const auto parsed = curl::parse_curl_argv(args);
  if (!parsed) {
    write_record(curl::record_line("invalid", args, env, "", extras));
    return 98;
  }
  if (parsed->probe) {
    write_record(curl::record_line("probe", args, env, "", extras));
    const curl::Scenario defaults;
    const auto& s = scenario ? *scenario : defaults;
    const std::string text = "curl " + s.probe_version + " (fake) libcurl/" + s.probe_version + " " + s.probe_tls + "\n";
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
    return s.probe_exit;
  }
  const std::string input = read_stdin();
  const auto url = curl::parse_config_line(input);
  if (!url) {
    write_record(curl::record_line("invalid", args, env, input, extras));
    return 98;
  }
  if (!scenario) {
    write_record(curl::record_line("no_scenario", args, env, input, extras));
    return 99;
  }
  const auto* response = curl::select(*scenario, *url, parsed->range);
  if (response != nullptr && !response->write_attempt.empty()) extras.emplace_back("write_errno", attempt_write(response->write_attempt));
  if (response != nullptr && !response->read_attempt.empty()) extras.emplace_back("read_errno", attempt_read(response->read_attempt));
  write_record(curl::record_line("transfer", args, env, input, extras));
  if (response == nullptr) {
    const std::string out = "HTTP/1.1 404 Not Found\r\n\r\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
    return 0;
  }
  const std::string file = response->body_file.empty() ? std::string{} : read_file(scenario_dir / response->body_file);
  const std::string head = curl::render_head(*response);
  std::fwrite(head.data(), 1, head.size(), stdout);
  std::fflush(stdout);
  // delay-ms: the head is out, the body waits (an interrupt lands mid-transfer).
  if (response->delay_ms != 0) std::this_thread::sleep_for(std::chrono::milliseconds(response->delay_ms));
  const std::string body = curl::render_body(*response, file);
  std::fwrite(body.data(), 1, body.size(), stdout);
  std::fflush(stdout);
  return response->exit_code;
}
