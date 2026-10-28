#include "fetch_cases.hpp"

#if defined(_WIN32)
#include "secure_frontend/downloader.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/win32_prelude.hpp"
#include "support/expect.hpp"
#include "support/fake_curl_support.hpp"

namespace fetch_cases {
namespace {

using secure_frontend_test::expect;

struct WindowsSpawn final {
  WindowsSpawn()
      : scratch(secure_frontend_test::make_scratch_directory()),
        fake(secure_frontend_test::install_fake_curl(scratch, secure_frontend_test::extra_arguments().at(0),
                                                     "probe 8.4.0 Schannel\nresponse https://fixture.example/f.bin\n"
                                                     "status 200 OK\nbody-text hello\nend\n")) {
    secure_frontend_test::make_private_directory(scratch / "cwd");
    options.executable = fake.executable;
    options.cwd = scratch / "cwd";
    options.record_file = scratch / "downloader-record.log";
  }
  std::vector<std::string> records() const { return secure_frontend_test::read_record_lines(options.record_file); }
  std::filesystem::path scratch;
  secure_frontend_test::FakeCurlInstall fake;
  secure_frontend::DownloaderOptions options;
};

secure_frontend::HopExit transfer(secure_frontend::ConfinedDownloader& downloader, std::string& body) {
  secure_frontend::TransferRequest request;
  request.url = "https://fixture.example/f.bin";
  request.host = "fixture.example";
  request.pin = secure_frontend::make_ipv4(93, 184, 216, 34);
  return downloader.run(request, [&body](const std::string_view bytes) {
    body.append(bytes);
    return true;
  });
}

}  // namespace

void downloader_transfer_through_job_windows() {
  WindowsSpawn w;
  secure_frontend::ConfinedDownloader downloader(w.options);
  expect(!downloader.probe().has_value(), "probe through the Job at Low integrity (8.4.0 meets the Windows floor)");
  std::string body;
  const auto exit = transfer(downloader, body);
  expect(exit.curl_exit == 0 && body.ends_with("hello"), "stdout pipe readable from a Low-integrity writer");
}

void dl03_downloader_runs_at_low_integrity_windows() {
  WindowsSpawn w;
  secure_frontend::ConfinedDownloader downloader(w.options);
  std::string body;
  (void)transfer(downloader, body);
  const auto lines = w.records();
  expect(!lines.empty() && lines[0].find("\"integrity\":\"S-1-16-4096\"") != std::string::npos, "the child token is Low integrity");
}

void priv04_downloader_env_is_systemroot_only_windows() {
  WindowsSpawn w;
  secure_frontend::ConfinedDownloader downloader(w.options);
  std::string body;
  (void)transfer(downloader, body);
  std::wstring windows(MAX_PATH, L'\0');
  windows.resize(GetSystemWindowsDirectoryW(windows.data(), MAX_PATH));
  const std::string expected = "\"env\":[\"SystemRoot=" + std::filesystem::path(windows).string() + "\"]";
  std::string escaped;
  for (const char c : expected) escaped += c == '\\' ? std::string{"\\\\"} : std::string(1, c);
  const auto lines = w.records();
  expect(!lines.empty() && lines[0].find(escaped) != std::string::npos, "exactly SystemRoot, from GetSystemWindowsDirectoryW");
}

void dl03_default_windows_curl_path_accepted_windows() {
  const auto path = secure_frontend::default_downloader_path();
  if (!std::filesystem::exists(path)) {
    expect(false, "System32\\curl.exe is present on supported Windows (10 1803+) and on the CI image");
    return;
  }
  const auto validated = secure_frontend::validate_downloader(path);
  expect(std::holds_alternative<std::filesystem::path>(validated),
         "TrustedInstaller-owned System32\\curl.exe is accepted (INHERIT_ONLY ACEs ignored)");
}

// Fix round 1: SP1's open_executable rules apply — '/' separators and '..' are
// normalised before \\?\ is added, and the result carries no \\?\ prefix.
void dl03_downloader_path_normalised_windows() {
  WindowsSpawn w;
  const auto plain = secure_frontend::validate_downloader(w.fake.executable);
  const auto spelled = (w.fake.executable.parent_path() / "scenario" / ".." / w.fake.executable.filename()).generic_wstring();
  expect(spelled.find(L'/') != std::wstring::npos && spelled.find(L"..") != std::wstring::npos, "the request uses '/' and '..'");
  const auto normalised = secure_frontend::validate_downloader(std::filesystem::path(spelled));
  const auto* a = std::get_if<std::filesystem::path>(&plain);
  const auto* b = std::get_if<std::filesystem::path>(&normalised);
  expect(a != nullptr && b != nullptr, "both spellings validate");
  if (a == nullptr || b == nullptr) return;
  expect(*a == *b, "both spellings validate to the same canonical path");
  expect(b->is_absolute() && b->wstring().rfind(L"\\\\?\\", 0) != 0, "the canonical path carries no \\\\?\\ prefix");
}

void dl03_downloader_unc_path_rejected_windows() {
  const auto validated = secure_frontend::validate_downloader(std::filesystem::path(L"\\\\server\\share\\curl.exe"));
  const auto* failure = std::get_if<secure_frontend::AcquireError>(&validated);
  expect(failure != nullptr && failure->reason == secure_frontend::AcquireReason::downloader_untrusted_path,
         "a network-share downloader is refused (SP1 root-form rule)");
}

}  // namespace fetch_cases
#endif
