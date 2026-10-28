// SP4 Task 22 (spec §2.7, §5.3): pull --bundle over the scripted transport.
#include "bundle_cases.hpp"
#include "secure_frontend/bundle_io.hpp"
#include "support/expect.hpp"
#include "support/pull_harness.hpp"

namespace bundle_cases {

namespace fx = secure_frontend_test::acquire;
using secure_frontend_test::expect;
using secure_frontend_test::pull::PullHarness;

void pull_bundle_resumes_after_interrupt() {
  PullHarness h;
  const auto payload = fx::qwen_payload();
  h.serve(payload);
  const auto text = fx::manifest_text("m", "t", 1, payload);
  auto request = h.third_party(text);
  request.bundle_dir = h.scratch / "usb" / "bundle";
  secure_frontend_test::make_private_directory(h.scratch / "usb");
  h.transport->after_first_chunk = [&h] { h.interrupted.store(true); };
  expect(h.pull(request).exit == secure_frontend::ExitCode::interrupted, "interrupted");
  expect(std::filesystem::exists(*request.bundle_dir / ".staging"), "the bundle holds only its own staging");
  h.interrupted.store(false);
  h.serve(payload);
  const auto resumed = h.pull(request);
  expect(resumed.exit == secure_frontend::ExitCode::success && resumed.bundle_complete, "the same pull --bundle resumes and completes");
  expect(secure_frontend_test::read_file(*request.bundle_dir / "STRATA-BUNDLE") == "strata-bundle 1\n", "marker last");
  expect(!std::filesystem::exists(*request.bundle_dir / ".staging"), "staging removed");
  secure_frontend::ProgressSink quiet;
  expect(std::holds_alternative<secure_frontend::OpenedBundle>(
             secure_frontend::open_bundle(*request.bundle_dir, *request.trust_digest, secure_frontend::ManifestRules{}, quiet)),
         "the bundle opens for import");
  expect(h.index().models.empty(), "pull --bundle installs nothing");
}

void pull_bundle_incomplete_without_pack_sources() {
  PullHarness h;
  auto payload = fx::qwen_payload();
  h.serve(payload);
  // Shards have sources; pack files do not (the §2.7 D5 (B) shape).
  std::string text = fx::manifest_text("m", "t", 1, payload);
  for (std::size_t i = 2; i < payload.size(); ++i) {
    const std::string source = "\"https://fixture.example/fx/" + payload[i].path + "\"";
    text.replace(text.find(source), source.size(), "");
  }
  auto request = h.third_party(text);
  request.bundle_dir = h.scratch / "bundle";
  const auto outcome = h.pull(request);
  expect(outcome.exit == secure_frontend::ExitCode::success && !outcome.bundle_complete, "sourced files fetched");
  expect(h.transport->argvs.size() == 2, "only the two shards were requested");
  expect(std::filesystem::exists(*request.bundle_dir / "MISSING") && !std::filesystem::exists(*request.bundle_dir / "SHA256SUMS") &&
             !std::filesystem::exists(*request.bundle_dir / "STRATA-BUNDLE"),
         "MISSING written; neither SHA256SUMS nor STRATA-BUNDLE");
  secure_frontend::ProgressSink quiet;
  const auto opened = secure_frontend::open_bundle(*request.bundle_dir, *request.trust_digest, secure_frontend::ManifestRules{}, quiet);
  expect(std::holds_alternative<secure_frontend::AcquireError>(opened) &&
             std::get<secure_frontend::AcquireError>(opened).reason == secure_frontend::AcquireReason::bundle_incomplete,
         "import says bundle_incomplete");
  expect(h.ui.text().find("make_bundle.py") != std::string::npos, "the developer step is printed");
}

// F-56 / §3.7: a run boundary is a trust boundary, also for files/ written by an earlier
// pull --bundle run: they are re-hashed, and a file that no longer matches is fetched again.
void pull_bundle_resume_rehashes_files_from_earlier_run() {
  PullHarness h;
  const auto payload = fx::qwen_payload();
  h.serve(payload, "response https://fixture.example/fx/" + payload[1].path + "\nstatus 503 Busy\nend\n");
  const auto text = fx::manifest_text("m", "t", 1, payload);
  auto request = h.third_party(text);
  request.bundle_dir = h.scratch / "bundle";
  const auto first = h.pull(request);
  expect(first.exit == secure_frontend::ExitCode::transport, "the second shard fails: exit 3");
  const auto shard_one = *request.bundle_dir / "files" / "weights" / "fx-00001-of-00002.gguf";
  expect(std::filesystem::exists(shard_one), "the first shard reached files/");
  std::string rotten = payload[0].bytes;
  rotten[0] ^= 0x20;
  secure_frontend_test::write_file(shard_one, rotten);
  h.serve(payload);
  const auto resumed = h.pull(request);
  expect(resumed.exit == secure_frontend::ExitCode::success && resumed.bundle_complete, "resumed and complete");
  bool refetched = false;
  for (const auto& config : h.transport->configs) refetched = refetched || config.find(payload[0].path) != std::string::npos;
  expect(refetched, "the altered file was fetched again");
  secure_frontend::ProgressSink quiet;
  expect(std::holds_alternative<secure_frontend::OpenedBundle>(
             secure_frontend::open_bundle(*request.bundle_dir, *request.trust_digest, secure_frontend::ManifestRules{}, quiet)),
         "the bundle opens");
}

}  // namespace bundle_cases
