// SP4 Task 28 (spec §10.5): release-only proof that the shipped strata spawns its
// downloader with exactly descriptors 0-2 (no record descriptor 4 without hooks).
#include "fetch_cases.hpp"

#if !defined(_WIN32)
#include "support/cli_harness.hpp"
#include "support/expect.hpp"
#include "support/process_runner.hpp"

namespace fetch_cases {

using secure_frontend_test::expect;

void release_spawn_has_no_fd4() {
  const auto& args = secure_frontend_test::extra_arguments();  // [0] strata, [1] fd census
  secure_frontend_test::cli::CliHarness h;
  secure_frontend_test::make_private_directory(h.scratch / "downloader");
  const auto census = h.scratch / "downloader" / "census";
  std::filesystem::copy_file(args.at(1), census);
  std::filesystem::permissions(census, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
  std::string text = secure_frontend_test::read_file(h.config);
  text.replace(text.find(h.missing_downloader()), h.missing_downloader().size(), "\"" + census.generic_string() + "\"");
  secure_frontend_test::write_file(h.config, text);
  const auto result = secure_frontend_test::run_to_completion(
      secure_frontend_test::ProcessSpec{args.at(0), {"--config", h.config.string(), "doctor", "--downloader"}, {}});
  // D-7 (R-T3-b): exit is 0 iff every check passed; this host has no GPU bundle, and the
  // fds-0-2 fact under test is the curl probe line, asserted exactly as before.
  expect(((result.exit_code == 0) == secure_frontend_test::contains(result.out, "doctor: all checks passed")) &&
             secure_frontend_test::contains(result.out, "downloader: curl 8.4.0"),
         "the shipped binary's probe child has exactly fds 0-2: " + result.err);
}

}  // namespace fetch_cases
#endif
