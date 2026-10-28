#include "fetch_cases.hpp"

#if !defined(_WIN32)
#include "secure_frontend/downloader.hpp"
#include "support/expect.hpp"
#include "support/fake_curl_support.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fetch_cases {
namespace {
using secure_frontend::AcquireReason;
using secure_frontend_test::expect;
}  // namespace

#if defined(SECURE_FRONTEND_TEST_HOOKS)
namespace {

const char* kScenario =
    "probe 8.4.0 OpenSSL/3.0.13\n"
    "response https://fixture.example/f.bin\nstatus 200 OK\nheader Content-Length: 5\nbody-text hello\nend\n";

secure_frontend::TransferRequest fixture_request(const std::string& url) {
  secure_frontend::TransferRequest request;
  request.url = url;
  request.host = "fixture.example";
  request.pin = secure_frontend::make_ipv4(93, 184, 216, 34);
  return request;
}

struct SpawnHarness final {
  explicit SpawnHarness(const std::string& scenario = kScenario)
      : scratch(secure_frontend_test::make_scratch_directory()),
        fake(secure_frontend_test::install_fake_curl(scratch, secure_frontend_test::extra_arguments().at(0), scenario)) {
    secure_frontend_test::make_private_directory(scratch / "models");
    secure_frontend_test::make_private_directory(scratch / "cwd");
    options.executable = fake.executable;
    options.cwd = std::filesystem::canonical(scratch / "cwd");
    options.protected_paths = {{"roots.models", std::filesystem::canonical(scratch / "models").string()}};
    options.record_file = scratch / "downloader-record.log";
  }

  secure_frontend::HopExit transfer(const std::string& url, std::string* body = nullptr) {
    secure_frontend::ConfinedDownloader downloader(options);
    return downloader.run(fixture_request(url), [body](const std::string_view bytes) {
      if (body != nullptr) body->append(bytes);
      return true;
    });
  }

  std::vector<std::string> records() const { return secure_frontend_test::read_record_lines(options.record_file); }

  std::filesystem::path scratch;
  secure_frontend_test::FakeCurlInstall fake;
  secure_frontend::DownloaderOptions options;
};

}  // namespace

void priv04_downloader_argv_exact() {
  SpawnHarness h;
  std::string body;
  const auto exit = h.transfer("https://fixture.example/f.bin", &body);
  expect(exit.curl_exit == 0 && !exit.failure && body.ends_with("hello"), "transfer through the confined child");
  const auto lines = h.records();
  expect(lines.size() == 1, "one transfer record");
  if (!lines.empty()) {
    expect(lines[0].find("\"argv\":[\"-q\",\"--config\",\"-\",\"--silent\",\"--include\",\"--suppress-connect-headers\",\"--globoff\"") !=
               std::string::npos, "-q first, --config -, --globoff");
    expect(lines[0].find("\"--user-agent\",\"strata\"") != std::string::npos && lines[0].find("\"--noproxy\",\"*\"") != std::string::npos,
           "fixed user agent, --noproxy *");
    expect(lines[0].find("\"-L\"") == std::string::npos && lines[0].find("fixture.example/f.bin\",") == std::string::npos,
           "no -L and no URL in argv");
  }
}

void priv04_downloader_env_is_empty() {
  SpawnHarness h;
  (void)h.transfer("https://fixture.example/f.bin");
  const auto lines = h.records();
  expect(!lines.empty() && lines[0].find("\"env\":[]") != std::string::npos, "empty environment");
}

void redirect_url_absent_from_downloader_argv() {
  SpawnHarness h("response https://fixture.example/f.bin?sig=SENTINEL\nstatus 200 OK\nbody-text x\nend\n");
  (void)h.transfer("https://fixture.example/f.bin?sig=SENTINEL");
  const auto lines = h.records();
  const auto argv_end = lines.empty() ? std::string::npos : lines[0].find("\"env\"");
  expect(!lines.empty() && lines[0].substr(0, argv_end).find("SENTINEL") == std::string::npos, "never in argv (/proc/<pid>/cmdline)");
  expect(!lines.empty() && lines[0].find("SENTINEL", argv_end) != std::string::npos, "only in the stdin config");
}

void dl03_downloader_untrusted_path_rejected() {
  SpawnHarness h;
  ::chmod(h.fake.executable.parent_path().c_str(), 0777);
  secure_frontend::ConfinedDownloader writable_parent(h.options);
  const auto shared = writable_parent.probe();
  expect(shared && shared->reason == AcquireReason::downloader_untrusted_path &&
             secure_frontend::exit_class(shared->reason) == secure_frontend::ExitCode::downloader, "group/other-writable parent: exit 9");
  ::chmod(h.fake.executable.parent_path().c_str(), 0700);
  auto missing_options = h.options;
  missing_options.executable = h.scratch / "downloader" / "nope";
  secure_frontend::ConfinedDownloader missing(missing_options);
  const auto absent = missing.probe();
  expect(absent && absent->reason == AcquireReason::downloader_missing, "missing file: downloader_missing");
  expect(h.records().empty(), "nothing spawned");
}

void downloader_below_floor_exits_9() {
  SpawnHarness h("probe 7.80.0 OpenSSL/1.1.1\n");
  secure_frontend::ConfinedDownloader downloader(h.options);
  const auto failure = downloader.probe();
  expect(failure && failure->reason == AcquireReason::downloader_version, "below the D3 floor: downloader_version");
  const auto lines = h.records();
  expect(lines.size() == 1 && lines[0].find("\"kind\":\"probe\"") != std::string::npos &&
             lines[0].find("\"argv\":[\"-q\",\"--version\"]") != std::string::npos, "the probe is exactly -q --version");
}

void dl03_downloader_cannot_write_files() {
  const auto target = secure_frontend_test::make_scratch_directory() / "pwned";
  SpawnHarness h("response https://fixture.example/f.bin\nstatus 200 OK\nbody-text x\nwrite-attempt " + target.string() + "\nend\n");
  if (secure_frontend::landlock_abi() == 0) {  // R7: no Landlock in this kernel (< 5.13 or LSM off): skip with a fixed reason
    std::cout << "SKIPPED: Landlock is unavailable in this kernel\n";
    return;
  }
  (void)h.transfer("https://fixture.example/f.bin");
  const auto lines = h.records();
  expect(!lines.empty() && lines[0].find("\"write_errno\":\"" + std::to_string(EACCES) + "\"") != std::string::npos,
         "a write outside every allowed directory is denied");
  expect(!std::filesystem::exists(target), "no file created");
}

void dl03_downloader_cannot_read_home() {
  SpawnHarness probe_harness;
  secure_frontend_test::write_file(probe_harness.scratch / "models" / "secret.txt", "secret");
  const auto secret = (probe_harness.scratch / "models" / "secret.txt").string();
  SpawnHarness h("response https://fixture.example/f.bin\nstatus 200 OK\nbody-text x\nread-attempt " + secret + "\nend\n");
  if (secure_frontend::landlock_abi() == 0) {  // R7: no Landlock in this kernel (< 5.13 or LSM off): skip with a fixed reason
    std::cout << "SKIPPED: Landlock is unavailable in this kernel\n";
    return;
  }
  (void)h.transfer("https://fixture.example/f.bin");
  const auto lines = h.records();
  expect(!lines.empty() && lines[0].find("\"read_errno\":\"" + std::to_string(EACCES) + "\"") != std::string::npos,
         "files outside the system directories and the downloader's parent are unreadable");
}

// Final fix F3 (§8.3): a probe killed because of the interrupt flag is `interrupted` (exit 8),
// never downloader_unsupported (exit 9).
void probe_interrupted_exits_8() {
  SpawnHarness h;
  const std::atomic<bool> interrupted{true};
  h.options.interrupted = &interrupted;
  secure_frontend::ConfinedDownloader downloader(h.options);
  const auto failure = downloader.probe();
  const std::string got = failure ? std::string{secure_frontend::reason_name(failure->reason)} : std::string{"none"};
  expect(failure && failure->reason == AcquireReason::interrupted &&
             secure_frontend::exit_class(failure->reason) == secure_frontend::ExitCode::interrupted,
         "interrupted probe: exit 8 (got " + got + ")");
}

void pre_exec_failure_fails_closed_exit_9() {
  SpawnHarness h;
  h.options.test_fail_pre_exec = true;
  secure_frontend::ConfinedDownloader downloader(h.options);
  const auto failure = downloader.probe();
  expect(failure && failure->reason == AcquireReason::downloader_confinement_failed, "'H' marker → downloader_confinement_failed");
  expect(h.records().empty(), "curl never runs unconfined");
}

// Ruling B2: with Landlock present, a ruleset that cannot be created is a
// failure (exit 9), never an unconfined spawn. The descriptor table is filled so
// landlock_create_ruleset fails with EMFILE; without B2 the spawn would go on
// and fail later on its pipes as downloader_unsupported.
void ruleset_failure_fails_closed_exit_9() {
  SpawnHarness h;
  if (secure_frontend::landlock_abi() == 0) {  // R7: no Landlock in this kernel (< 5.13 or LSM off): skip with a fixed reason
    std::cout << "SKIPPED: Landlock is unavailable in this kernel\n";
    return;
  }
  h.options.record_file.clear();
  secure_frontend::ConfinedDownloader downloader(h.options);
  expect(!downloader.probe().has_value(), "the probe runs confined first (validates the downloader)");
  rlimit saved{};
  expect(::getrlimit(RLIMIT_NOFILE, &saved) == 0, "RLIMIT_NOFILE read");
  rlimit low = saved;
  low.rlim_cur = std::min<rlim_t>(saved.rlim_cur, 256);
  expect(::setrlimit(RLIMIT_NOFILE, &low) == 0, "RLIMIT_NOFILE lowered");
  std::vector<int> fillers;
  for (int fd; (fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC)) >= 0;) fillers.push_back(fd);
  const auto exit = downloader.run(fixture_request("https://fixture.example/f.bin"), [](const std::string_view) { return true; });
  for (const int fd : fillers) ::close(fd);
  (void)::setrlimit(RLIMIT_NOFILE, &saved);
  expect(exit.failure && exit.failure->reason == AcquireReason::downloader_confinement_failed && !exit.curl_exit,
         "ruleset creation failed: downloader_confinement_failed, nothing spawned");
}

void record_fd_does_not_clobber_report_pipe() {
  // Free fd 3 so the frontend's record descriptor (opened before the Landlock
  // ruleset) is fd 3, the number SP2 uses for kReportFd.
  const int saved = ::fcntl(3, F_DUPFD_CLOEXEC, 100);
  ::close(3);
  SpawnHarness h;
  std::string body;
  const auto exit = h.transfer("https://fixture.example/f.bin", &body);
  if (saved >= 0) {
    ::dup2(saved, 3);
    ::close(saved);
  }
  expect(exit.curl_exit == 0 && body.ends_with("hello"), "the report pipe still reports success (EOF)");
  expect(h.records().size() == 1, "and the record reached fd 4");
}

void confinement_reduced_is_announced_without_landlock() {
  SpawnHarness h;
  h.options.test_force_no_landlock = true;
  const secure_frontend::ConfinedDownloader reduced(h.options);
  expect(reduced.confinement() == "reduced:no_landlock", "announced before confirmation");
  SpawnHarness full;
  const secure_frontend::ConfinedDownloader confined(full.options);
  expect(secure_frontend::landlock_abi() == 0 || confined.confinement() == "full", "full when Landlock is present");
  auto readable = full.options;
  readable.protected_paths = {{"roots.models", full.fake.executable.parent_path().string() + "/models-here"}};
  expect(secure_frontend::landlock_abi() == 0 ||
             secure_frontend::ConfinedDownloader(readable).confinement() == "reduced:readable:roots.models",
         "a root beneath the downloader's parent reduces confinement");
}

#endif  // SECURE_FRONTEND_TEST_HOOKS

void downloader_spawn_report_classified() {
  const auto marked = [](const int error) {
    std::string bytes(1 + sizeof(int), 'H');
    std::memcpy(bytes.data() + 1, &error, sizeof error);
    return bytes;
  };
  const auto plain = [](const int error) {
    std::string bytes(sizeof(int), '\0');
    std::memcpy(bytes.data(), &error, sizeof error);
    return bytes;
  };
  using secure_frontend::classify_spawn_report;
  expect(classify_spawn_report(marked(EPERM)) == AcquireReason::downloader_confinement_failed, "'H' + errno: confinement failed");
  expect(classify_spawn_report(marked(ENOENT)) == AcquireReason::downloader_confinement_failed, "the marker wins over the errno");
  expect(classify_spawn_report(plain(ENOENT)) == AcquireReason::downloader_missing, "execve ENOENT: missing");
  expect(classify_spawn_report(plain(EACCES)) == AcquireReason::downloader_untrusted_path, "execve EACCES: untrusted path");
  expect(classify_spawn_report(plain(ENOEXEC)) == AcquireReason::downloader_unsupported, "other errno: unsupported");
  expect(classify_spawn_report("xyz") == AcquireReason::downloader_unsupported, "malformed: unsupported, never success");
}

}  // namespace fetch_cases
#endif
