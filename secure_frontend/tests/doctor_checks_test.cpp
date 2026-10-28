// SP7 M1 Task 3: the D-7 doctor checks, every one driven by a fake ProbeBundle.
#include "doctor_checks.hpp"
#include "secure_frontend/config_file.hpp"
#include "support/config_json.hpp"
#include "support/test_support.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
bool contains(const std::string& haystack, std::string_view needle) { return haystack.find(needle) != std::string::npos; }
using secure_frontend_test::json_literal;
using namespace secure_frontend::doctor;

// A real validated config for the pass path (the loader result is reused verbatim).
// validate() resolves roots.models and the engine executable, so these come from
// a scratch directory.
std::shared_ptr<const secure_frontend::FrontendConfig> valid_config() {
  const auto scratch = secure_frontend_test::make_scratch_directory();
  const auto models = scratch / "models";
  secure_frontend_test::make_private_directory(models);
  const auto engine = scratch / "engine";
  secure_frontend_test::write_file(engine, "binary", 0700);
  const std::string text = secure_frontend_test::render_config_json({
      {"schema_version", "1"},
      {"listener.port", "8123"},
      {"roots.models", json_literal(models.string())},
      {"roots.state", json_literal((scratch / "state").string())},
      {"engine.executable", json_literal(engine.string())},
      {"engine.model_id", json_literal("m:t")},
      {"token.source", json_literal("env:SF_TEST_TOKEN")},
  });
  const auto draft = secure_frontend::parse_config_text(text, scratch.string());
  const auto* draft_value = std::get_if<secure_frontend::FrontendConfigDraft>(&draft);
  if (draft_value == nullptr) return nullptr;
  const auto validated = secure_frontend::validate(*draft_value);
  const auto* ok = std::get_if<secure_frontend::ValidatedConfig>(&validated);
  return ok != nullptr ? ok->shared() : nullptr;
}

ProbeBundle passing_bundle() {
  ProbeBundle bundle;
  bundle.config = valid_config();
  bundle.nvidia_smi = [] { return std::optional<std::string>{"580.65.07"}; };
  bundle.file_mode = [](const std::filesystem::path& p) -> std::optional<unsigned> {
    if (p.filename() == "token") return 0600U;
    if (p.filename() == "libcudart.so.13") return 0755U;
    return std::nullopt;
  };
  bundle.program_dir = "/opt/strata/bin";
  bundle.token_file = "/srv/secret/token";
  bundle.roots = {{"roots.models", ""}, {"roots.secret", ""}, {"roots.state", ""}};
  bundle.port_free = [] { return true; };
  bundle.memlock_limit = [] { return std::optional<std::uint64_t>{std::numeric_limits<std::uint64_t>::max()}; };
  bundle.deep_digest_failures = [] { return std::vector<std::string>{}; };
  return bundle;
}

void doctor_check_config_names_rejected_keys() {
  ProbeBundle bundle;
  bundle.config_issues = {
      {"listener.port", secure_frontend::ConfigIssueCode::out_of_range, "must be between 1 and 65535"},
      {"roots.state", secure_frontend::ConfigIssueCode::invalid_value, "fixed reason"},
  };
  const auto result = check_config(bundle);
  expect(!result.ok, "a rejected config is a failed check");
  expect(contains(result.human_fix, "listener.port") && contains(result.human_fix, "roots.state"),
         "every rejected key is named: " + result.human_fix);
  expect(!contains(result.human_fix, "8123") && !contains(result.human_fix, "/srv"), "values are never echoed: " + result.human_fix);
}

void doctor_check_config_valid_passes() {
  const auto bundle = passing_bundle();
  const auto result = check_config(bundle);
  expect(result.ok, "the reused loader result passes with no issues: " + result.human_fix);
}

void doctor_check_token_modes() {
  ProbeBundle bundle;
  bundle.token_file = "/srv/secret/token";
  for (const unsigned mode : {0600U, 0400U}) {
    bundle.file_mode = [mode](const std::filesystem::path&) { return std::optional<unsigned>{mode}; };
    expect(check_token(bundle).ok, "mode 0" + std::to_string(mode) + " passes");
  }
  bundle.file_mode = [](const std::filesystem::path&) { return std::optional<unsigned>{0644U}; };
  auto result = check_token(bundle);
  expect(!result.ok && !result.human_fix.empty(), "mode 0644 fails with a fix");
  bundle.file_mode = [](const std::filesystem::path&) { return std::optional<unsigned>{}; };
  result = check_token(bundle);
  expect(!result.ok, "a missing token file fails");
  ProbeBundle env_bundle;  // token.source env: — no local file to check
  expect(check_token(env_bundle).ok, "an env: token has no file mode to check");
}

void doctor_check_roots_report_named_failures() {
  ProbeBundle bundle;
  bundle.roots = {{"roots.models", ""}, {"roots.secret", "owned by another user"}};
  const auto result = check_roots(bundle);
  expect(!result.ok && contains(result.human_fix, "roots.secret") && contains(result.human_fix, "owned by another user"),
         "the failing root is named with its CFG reason: " + result.human_fix);
  bundle.roots = {{"roots.models", ""}, {"roots.secret", ""}, {"roots.state", ""}};
  expect(check_roots(bundle).ok, "clean roots pass");
}

void doctor_check_port_busy_fails_with_fix() {
  ProbeBundle bundle;
  bundle.port_free = [] { return true; };
  expect(check_port(bundle).ok, "a free port passes");
  bundle.port_free = [] { return false; };
  const auto result = check_port(bundle);
  expect(!result.ok && contains(result.human_fix, "listener.port"), "a busy port names the config key: " + result.human_fix);
}

void doctor_check_gpu_and_driver() {
  ProbeBundle bundle;
  bundle.nvidia_smi = [] { return std::optional<std::string>{}; };
  expect(!check_gpu(bundle).ok, "no nvidia-smi fails the gpu check");
  expect(!check_driver(bundle).ok, "and the driver check reads nothing");
  for (const std::string& good : {std::string{"580.65.07"}, std::string{"581"}, std::string{"Driver Version: 1000.1.2"}}) {
    bundle.nvidia_smi = [&good] { return std::optional<std::string>{good}; };
    expect(check_gpu(bundle).ok && check_driver(bundle).ok, "driver >= 580 passes: " + good);
  }
  for (const std::string& bad : {std::string{"470.199.02"}, std::string{"579.9"}, std::string{"no version here"}}) {
    bundle.nvidia_smi = [&bad] { return std::optional<std::string>{bad}; };
    const auto result = check_driver(bundle);
    expect(!result.ok && contains(result.human_fix, "580"), "older/unreadable driver fails naming the minimum: " + bad);
  }
}

void doctor_check_cuda_runtime() {
  ProbeBundle bundle;
  bundle.program_dir = "/opt/strata/bin";
  bundle.file_mode = [](const std::filesystem::path&) { return std::optional<unsigned>{}; };
  auto result = check_cuda_runtime(bundle);
  expect(!result.ok && contains(result.human_fix, "libcudart"), "no CUDA runtime next to the program fails: " + result.human_fix);
  bundle.file_mode = [](const std::filesystem::path& p) -> std::optional<unsigned> {
    return p.filename() == "libcudart.so.12" ? std::optional<unsigned>{0755U} : std::optional<unsigned>{};
  };
  expect(check_cuda_runtime(bundle).ok, "a candidate found next to the program (or under lib/) passes");
}

void doctor_check_memlock() {
  ProbeBundle bundle;
  bundle.memlock_limit = [] { return std::optional<std::uint64_t>{}; };
  expect(!check_memlock(bundle).ok, "unreadable LimitMEMLOCK fails");
  bundle.memlock_limit = [] { return std::optional<std::uint64_t>{8388608ULL}; };
  auto result = check_memlock(bundle);
  expect(!result.ok && contains(result.human_fix, "unlimited"), "a limited value fails and names the need: " + result.human_fix);
  bundle.memlock_limit = [] { return std::optional<std::uint64_t>{std::numeric_limits<std::uint64_t>::max()}; };
  expect(check_memlock(bundle).ok, "unlimited passes");
  ProbeBundle windows;
  windows.memlock_applicable = false;
  expect(check_memlock(windows).ok, "Windows large pages ride a different mechanism (R-T3-c): not a false failure");
}

void doctor_deep_digests_and_run_order() {
  const auto shallow = run_checks(passing_bundle());
  const std::vector<std::string> expected{
      "gpu", "driver", "cuda", "config", "token", "roots", "port", "memlock"};
  std::vector<std::string> names;
  for (const auto& check : shallow) names.push_back(check.name);
  expect(names == expected, "the shallow set is the D-7 order without digests: " + [&] {
    std::string joined;
    for (const auto& n : names) { if (!joined.empty()) joined += ","; joined += n; }
    return joined;
  }());
  for (const auto& check : shallow) expect(check.ok, "the passing bundle passes " + check.name + ": " + check.human_fix);
  auto deep = passing_bundle();
  deep.deep = true;
  deep.deep_digest_failures = [] { return std::vector<std::string>{"m:t: manifest digest changed"}; };
  const auto deep_checks = run_checks(deep);
  expect(deep_checks.size() == expected.size() + 1U && deep_checks.back().name == "model-digests", "--deep appends the digest hook");
  expect(!deep_checks.back().ok && contains(deep_checks.back().human_fix, "m:t"), "a fake tiny-model failure is reported");
}

void doctor_human_fix_never_carries_install_commands() {
  ProbeBundle bundle;  // everything fails
  bundle.nvidia_smi = [] { return std::optional<std::string>{}; };
  bundle.file_mode = [](const std::filesystem::path&) { return std::optional<unsigned>{}; };
  bundle.token_file = "/srv/secret/token";
  bundle.roots = {{"roots.secret", "shared writable"}};
  bundle.port_free = [] { return false; };
  bundle.memlock_limit = [] { return std::optional<std::uint64_t>{4096ULL}; };
  bundle.deep = true;
  bundle.deep_digest_failures = [] { return std::vector<std::string>{"m:t: digest changed"}; };
  for (const auto& check : run_checks(bundle)) {
    expect(!check.ok, "the broken bundle fails " + check.name);
    for (const char* forbidden : {"sudo", "apt", "dnf", "yum", "pacman", "pip install", "conda", "msiexec", "powershell", "wget",
                                  "curl ", "rpm ", "dpkg -i", "choco"}) {
      expect(!contains(check.human_fix, forbidden), "the fix for " + check.name + " carries no installing command <" + forbidden +
                                                        ">: " + check.human_fix);
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"doctor_check_config_names_rejected_keys", doctor_check_config_names_rejected_keys},
      {"doctor_check_config_valid_passes", doctor_check_config_valid_passes},
      {"doctor_check_token_modes", doctor_check_token_modes},
      {"doctor_check_roots_report_named_failures", doctor_check_roots_report_named_failures},
      {"doctor_check_port_busy_fails_with_fix", doctor_check_port_busy_fails_with_fix},
      {"doctor_check_gpu_and_driver", doctor_check_gpu_and_driver},
      {"doctor_check_cuda_runtime", doctor_check_cuda_runtime},
      {"doctor_check_memlock", doctor_check_memlock},
      {"doctor_deep_digests_and_run_order", doctor_deep_digests_and_run_order},
      {"doctor_human_fix_never_carries_install_commands", doctor_human_fix_never_carries_install_commands},
  }, failures);
}
