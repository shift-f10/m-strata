// SP4 Task 27 (spec §3.2, §7.3, §10.3): confinement as strata announces and logs it.
#include "fetch_cases.hpp"
#include "secure_frontend/downloader.hpp"
#include "support/cli_harness.hpp"
#include "support/expect.hpp"
#include "support/fake_curl_support.hpp"
#include "support/pull_harness.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fetch_cases {
namespace {

namespace fx = secure_frontend_test::acquire;
using secure_frontend_test::expect;

// Adds "logs": <dir> to the harness config's roots object.
void set_logs_root(secure_frontend_test::cli::CliHarness& h, const std::filesystem::path& logs) {
  std::string text = secure_frontend_test::read_file(h.config);
  text.replace(text.find("\"roots\": {"), 10, "\"roots\": {\"logs\": \"" + logs.generic_string() + "\", ");
  secure_frontend_test::write_file(h.config, text);
}

std::string trust_argument(const std::string& manifest) {
  return "sha256:" + secure_frontend::to_hex(secure_frontend::sha256(manifest));
}

#if !defined(_WIN32)
// The real confined downloader is the fake curl copied into <scratch>/downloader/; payload
// files are served under flat names.
secure_frontend_test::FakeCurlInstall install_serving_fake(secure_frontend_test::cli::CliHarness& h) {
  std::string scenario = "probe 8.4.0 OpenSSL/3.0.13\n";
  std::vector<std::pair<std::string, std::string>> fixtures;
  for (const auto& f : fx::qwen_payload()) {
    std::string name = f.path;
    std::replace(name.begin(), name.end(), '/', '_');
    fixtures.emplace_back(name, f.bytes);
    scenario += "response https://fixture.example/fx/" + f.path + "\nstatus 200 OK\nbody-file " + name + "\nend\n";
  }
  const auto fake = secure_frontend_test::install_fake_curl(h.scratch, secure_frontend_test::extra_arguments().at(0), scenario, fixtures);
  std::string text = secure_frontend_test::read_file(h.config);
  text.replace(text.find(h.missing_downloader()), h.missing_downloader().size(), "\"" + fake.executable.generic_string() + "\"");
  secure_frontend_test::write_file(h.config, text);
  return fake;
}
#endif

}  // namespace

void confinement_reduced_written_to_log_record() {
  secure_frontend_test::cli::CliHarness h;
  secure_frontend_test::make_private_directory(h.scratch / "logs");
  set_logs_root(h, h.scratch / "logs");
  auto transport = std::make_shared<secure_frontend_test::ScriptedTransport>(
      *secure_frontend_test::curl::parse_scenario([] {
        std::string s;
        for (const auto& f : fx::qwen_payload()) s += "response https://fixture.example/fx/" + f.path + "\nstatus 200 OK\nbody-file " + f.path + "\nend\n";
        return s;
      }()),
      [] {
        std::map<std::string, std::string> m;
        for (const auto& f : fx::qwen_payload()) m[f.path] = f.bytes;
        return m;
      }());
  auto probe = std::make_shared<secure_frontend_test::pull::FakeProbe>();
  probe->label = "reduced:no_landlock";
  h.services.transport = [transport](const secure_frontend::FrontendConfig&) {
    return std::unique_ptr<secure_frontend::HopTransport>(std::make_unique<secure_frontend_test::pull::ForwardTransport>(*transport));
  };
  h.services.probe = [probe](const secure_frontend::FrontendConfig&) {
    return std::unique_ptr<secure_frontend::DownloaderProbe>(std::make_unique<secure_frontend_test::pull::ForwardProbe>(*probe));
  };
  h.services.resolver = [] {
    auto r = std::make_unique<secure_frontend_test::ScriptedResolver>();
    r->answers["fixture.example"] = {secure_frontend::make_ipv4(93, 184, 216, 34)};
    return std::unique_ptr<secure_frontend::Resolver>(std::move(r));
  };
  const auto text_manifest = fx::manifest_text("m", "t", 1, fx::qwen_payload());
  secure_frontend_test::write_file(h.scratch / "m.json", text_manifest);
  const int code = h.run({"pull", "--manifest", (h.scratch / "m.json").string(), "--trust-manifest", trust_argument(text_manifest), "--yes"});
  expect(code == 0, "pull: " + h.err);
  expect(h.out.find("confinement: reduced:no_landlock") != std::string::npos, "announced before the confirmation prompt");
  const std::string log = secure_frontend_test::read_file(h.scratch / "logs" / "acquire.log");
  expect(log.find("verb=pull model=m:t") != std::string::npos && log.find("confinement=reduced:no_landlock") != std::string::npos,
         "one record per file names the confinement");
  expect(log.find("path=pack/index.txt") != std::string::npos && log.find("host=fixture.example result=ok") != std::string::npos,
         "relative path, host, result");
  expect(log.find("manifest=" + secure_frontend::to_hex(secure_frontend::sha256(text_manifest))) != std::string::npos, "manifest digest");
  expect(static_cast<std::size_t>(std::count(log.begin(), log.end(), '\n')) == fx::qwen_payload().size(),
         "one record per transferred file");
  expect(log.find("93.184.216.34") == std::string::npos && log.find("https://") == std::string::npos, "never an address or URL");
}

#if !defined(_WIN32)
// Ruling B1: doctor --downloader on a store that has never been pulled into: the probe's cwd
// (roots.models/.staging/.cwd) is created first, so the probe runs instead of failing closed.
void doctor_downloader_probes_on_fresh_store() {
  secure_frontend_test::cli::CliHarness h;
  (void)install_serving_fake(h);
  const int code = h.run({"doctor", "--downloader"});
  // D-7 (SP7 M1 T3, R-T3-b): doctor now exits 0 iff every check passed; the host's GPU
  // decides that part. The probe behaviour below stays asserted exactly as before.
  const bool verdict_ok = h.out.find("doctor: all checks passed") != std::string::npos;
  expect((code == 0) == verdict_ok, "doctor --downloader exits 0 iff every check passed: " + h.err);
  expect(h.out.find("downloader: curl 8.4.0 (OpenSSL)") != std::string::npos, "probe result printed");
  expect(std::filesystem::is_directory(h.models_path / ".staging" / ".cwd"), "the downloader cwd exists");
}
#endif

#if !defined(_WIN32) && defined(SECURE_FRONTEND_TEST_HOOKS)
// The real confined downloader (fake curl) with a root or the config beneath the downloader's parent.
void confinement_reduced_when_root_beneath_allowed_dir() {
  const bool landlock = secure_frontend::landlock_abi() > 0;
  const std::string reduced_models = landlock ? "reduced:readable:roots.models" : "reduced:no_landlock";
  const std::string reduced_config = landlock ? "reduced:readable:config file" : "reduced:no_landlock";
  {
    // (1) roots.models placed beneath the downloader's own directory: doctor announces it.
    secure_frontend_test::cli::CliHarness h;
    const auto fake = install_serving_fake(h);
    const auto inner = fake.executable.parent_path() / "models";
    secure_frontend_test::make_private_directory(inner);
    std::string text = secure_frontend_test::read_file(h.config);
    const auto old_models = std::filesystem::canonical(h.models_path).generic_string();
    text.replace(text.find(old_models), old_models.size(), std::filesystem::canonical(inner).generic_string());
    secure_frontend_test::write_file(h.config, text);
    const int doctor = h.run({"doctor"});
    // D-7 verdict rule (R-T3-b); the confinement announcement below is the invariant.
    expect((doctor == 0) == (h.out.find("doctor: all checks passed") != std::string::npos), "doctor exit follows the D-7 verdict: " + h.err);
    expect(h.out.find("confinement: " + reduced_models) != std::string::npos,
           "a root beneath an allowed directory reduces confinement and doctor says so");

    // (3) Ruling R8(b): a pull through the fake downloader logs that confinement per file.
    secure_frontend_test::make_private_directory(h.scratch / "logs");
    set_logs_root(h, h.scratch / "logs");
    h.services.resolver = [] {
      auto r = std::make_unique<secure_frontend_test::ScriptedResolver>();
      r->answers["fixture.example"] = {secure_frontend::make_ipv4(93, 184, 216, 34)};
      return std::unique_ptr<secure_frontend::Resolver>(std::move(r));
    };
    const auto manifest = fx::manifest_text("m", "t", 1, fx::qwen_payload());
    secure_frontend_test::write_file(h.scratch / "m.json", manifest);
    const auto record = h.scratch / "downloader-record.log";
    const int code = h.run_raw({"--test-record-file", record.string(), "--config", h.config.string(), "pull", "--manifest",
                                (h.scratch / "m.json").string(), "--trust-manifest", trust_argument(manifest), "--yes"});
    expect(code == 0, "pull through the confined fake: " + h.err);
    expect(h.out.find("confinement: " + reduced_models) != std::string::npos, "announced before the confirmation");
    const auto lines = secure_frontend_test::read_record_lines(record);
    expect(lines.size() == 1 + fx::qwen_payload().size(), "one probe and one transfer per file went through the downloader");
    const std::string log = secure_frontend_test::read_file(h.scratch / "logs" / "acquire.log");
    expect(log.find("verb=pull model=m:t") != std::string::npos && log.find("confinement=" + reduced_models) != std::string::npos,
           "acquire.log names the reduced confinement");
  }
  {
    // (2) Ruling R8(a): the config file placed inside the downloader's directory.
    secure_frontend_test::cli::CliHarness h;
    const auto fake = install_serving_fake(h);
    const auto inside = fake.executable.parent_path() / "config.json";
    secure_frontend_test::write_file(inside, secure_frontend_test::read_file(h.config));
    const int doctor = h.run_raw({"--config", inside.string(), "doctor"});
    // D-7 verdict rule (R-T3-b); the confinement announcement below is the invariant.
    expect((doctor == 0) == (h.out.find("doctor: all checks passed") != std::string::npos), "doctor exit follows the D-7 verdict: " + h.err);
    expect(h.out.find("confinement: " + reduced_config) != std::string::npos,
           "a config file beneath an allowed directory reduces confinement and doctor says so");
  }
}
#endif

}  // namespace fetch_cases
