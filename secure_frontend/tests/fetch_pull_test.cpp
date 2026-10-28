// SP4 Task 22 (spec §3.7, §4.5): a wrong digest from a mirror never replaces the installed version.
#include "fetch_cases.hpp"
#include "secure_frontend/model_index_store.hpp"
#include "support/expect.hpp"
#include "support/pull_harness.hpp"

namespace fetch_cases {

namespace fx = secure_frontend_test::acquire;
using secure_frontend_test::expect;
using secure_frontend_test::pull::PullHarness;

void dl03_wrong_digest_rejected_prior_version_intact() {
  PullHarness h;
  const auto v1 = fx::qwen_payload("1");
  h.serve(v1);
  expect(h.pull(h.third_party(fx::manifest_text("m", "t", 1, v1))).exit == secure_frontend::ExitCode::success, "v1 installed");
  const auto before = h.index().models.front();
  const auto v2 = fx::qwen_payload("2");
  auto served = v2;
  served[3].bytes[0] ^= 0x20;  // a malicious mirror alters bytes (same length)
  h.serve(served);
  const auto outcome = h.pull(h.third_party(fx::manifest_text("m", "t", 2, v2)));
  expect(outcome.error && outcome.error->reason == secure_frontend::AcquireReason::digest_mismatch &&
             outcome.exit == secure_frontend::ExitCode::verification, "digest_mismatch, exit 4");
  const auto index = h.index();
  expect(index.models.size() == 1 && index.models.front().dir == before.dir, "the prior version stays active");
  expect(std::holds_alternative<secure_frontend::InstalledModel>(secure_frontend::load_installed(h.models, before)), "and intact");
}

}  // namespace fetch_cases
