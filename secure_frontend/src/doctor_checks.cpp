// SP7 M1 Task 3 (spec D-7): the full offline doctor check set — pure functions
// of the injected ProbeBundle. Every human fix is fixed prose that tells the
// human what to do; it never carries a command that installs anything, and the
// doctor itself installs nothing and contacts no network (PX §8).
#include "doctor_checks.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace secure_frontend {
namespace doctor {
namespace {

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for (const auto& part : parts) {
    if (!out.empty()) out += sep;
    out += part;
  }
  return out;
}

// Dotted-integer tuple compare, missing tail treated as zero.
bool version_at_least(const std::vector<unsigned>& version, const std::vector<unsigned>& minimum) {
  const std::size_t shared = std::max(version.size(), minimum.size());
  for (std::size_t i = 0; i < shared; ++i) {
    const unsigned left = i < version.size() ? version[i] : 0U;
    const unsigned right = i < minimum.size() ? minimum[i] : 0U;
    if (left != right) return left > right;
  }
  return true;
}

}  // namespace

const ProfileExpectations& strata_engine_profile_expectations() noexcept {
  static const ProfileExpectations profile;
  return profile;
}

CheckResult check_config(const ProbeBundle& bundle) {
  if (!bundle.config_issues.empty()) {
    std::vector<std::string> keys;
    for (const auto& issue : bundle.config_issues) keys.push_back(issue.key_path);
    return CheckResult{"config", false,
                       "config: the config file was rejected; fix these keys: " + join(keys, ", ") +
                           " (strata prints every issue with strata --config <file> doctor)"};
  }
  if (bundle.config == nullptr) {
    return CheckResult{"config", false, "config: the config was never loaded — pass --config <file> before the verb"};
  }
  return CheckResult{"config", true, ""};
}

CheckResult check_token(const ProbeBundle& bundle) {
  if (!bundle.token_file.has_value()) {
    return CheckResult{"token", true, ""};  // token.source env: — no local file mode to check (CFG-06)
  }
  const auto mode = bundle.file_mode ? bundle.file_mode(*bundle.token_file) : std::nullopt;
  if (!mode.has_value()) {
    return CheckResult{"token", false,
                       "token: the token file below roots.secret is missing; create it with strata key rotate"};
  }
  if (*mode == 0600U || *mode == 0400U) return CheckResult{"token", true, ""};
  return CheckResult{"token", false,
                     "token: the token file is readable beyond you (only 0600/0400 are accepted); replace it with "
                     "strata key rotate, which writes it 0600"};
}

CheckResult check_roots(const ProbeBundle& bundle) {
  std::vector<std::string> failures;
  for (const auto& root : bundle.roots) {
    if (!root.error.empty()) failures.push_back(root.label + ": " + root.error);
  }
  if (!failures.empty()) {
    return CheckResult{"roots", false,
                       "roots: a configured root is not yours alone and canonical (CFG rules) — " + join(failures, "; ") +
                           "; strata never relaxes ownership or permissions on its own roots"};
  }
  return CheckResult{"roots", true, ""};
}

CheckResult check_port(const ProbeBundle& bundle) {
  if (bundle.port_free && bundle.port_free()) return CheckResult{"port", true, ""};
  return CheckResult{"port", false,
                     "port: the configured listen port is already in use; stop the other listener or set "
                     "listener.port to a free one"};
}

CheckResult check_gpu(const ProbeBundle& bundle) {
  if (bundle.nvidia_smi && bundle.nvidia_smi()) return CheckResult{"gpu", true, ""};
  return CheckResult{"gpu", false,
                     "gpu: nvidia-smi is not runnable on this PATH, so no usable NVIDIA driver is visible; the engine "
                     "wants NVIDIA driver 580 or newer from nvidia.com/drivers (strata never installs drivers)"};
}

CheckResult check_driver(const ProbeBundle& bundle) {
  const auto report = bundle.nvidia_smi ? bundle.nvidia_smi() : std::optional<std::string>{};
  const auto version = report.has_value() ? parse_driver_version(*report) : std::nullopt;
  const auto minimum = parse_driver_version(bundle.profile.min_driver);
  if (!version.has_value() || !minimum.has_value()) {
    return CheckResult{"driver", false,
                       "driver: no driver version could be read from nvidia-smi; the engine wants 580 or newer "
                       "(check nvidia-smi yourself — strata changes nothing)"};
  }
  if (!version_at_least(*version, *minimum)) {
    return CheckResult{"driver", false,
                       "driver: this driver is older than the engine profile's minimum " + bundle.profile.min_driver +
                           "; update it from nvidia.com/drivers (strata never installs drivers)"};
  }
  return CheckResult{"driver", true, ""};
}

CheckResult check_cuda_runtime(const ProbeBundle& bundle) {
  for (const auto& lib : bundle.profile.cuda_runtime_libs) {
    if (bundle.file_mode && (bundle.file_mode(bundle.program_dir / lib) || bundle.file_mode(bundle.program_dir / "lib" / lib))) {
      return CheckResult{"cuda", true, ""};
    }
  }
  std::vector<std::string> names;
  for (const auto& lib : bundle.profile.cuda_runtime_libs) names.push_back(lib);
  return CheckResult{"cuda", false,
                     "cuda: none of the CUDA runtime libraries (" + join(names, ", ") +
                         ") sits next to the program; re-extract the strata release bundle over the install — strata "
                         "never downloads anything itself"};
}

CheckResult check_memlock(const ProbeBundle& bundle) {
  if (!bundle.memlock_applicable) {
    return CheckResult{"memlock", true, ""};  // R-T3-c: Windows large pages ride the user right, not RLIMIT_MEMLOCK
  }
  const auto current = bundle.memlock_limit ? bundle.memlock_limit() : std::nullopt;
  if (!current.has_value()) {
    return CheckResult{"memlock", false,
                       "memlock: the locked-memory limit (LimitMEMLOCK) could not be read; the engine profile wants it "
                       "unlimited — raise it in your session's defaults (strata changes nothing)"};
  }
  if (bundle.profile.memlock_unlimited) {
    if (*current == std::numeric_limits<std::uint64_t>::max()) return CheckResult{"memlock", true, ""};
    return CheckResult{"memlock", false,
                       "memlock: LimitMEMLOCK is " + std::to_string(*current) +
                           " bytes; the engine profile wants unlimited — raise the limit for your user session "
                           "(strata changes nothing)"};
  }
  return CheckResult{"memlock", true, ""};
}

CheckResult check_model_digests(const ProbeBundle& bundle) {
  const auto failures = bundle.deep_digest_failures ? bundle.deep_digest_failures() : std::vector<std::string>{};
  if (failures.empty()) return CheckResult{"model-digests", true, ""};
  return CheckResult{"model-digests", false,
                     "model-digests: an installed model failed its manifest re-check: " + join(failures, "; ") +
                         "; strata never loads it — re-acquire the model (strata pull or strata import)"};
}

std::vector<CheckResult> run_checks(const ProbeBundle& bundle) {
  std::vector<CheckResult> checks{
      check_gpu(bundle), check_driver(bundle), check_cuda_runtime(bundle), check_config(bundle), check_token(bundle),
      check_roots(bundle), check_port(bundle), check_memlock(bundle)};
  if (bundle.deep) checks.push_back(check_model_digests(bundle));
  return checks;
}

std::optional<std::vector<unsigned>> parse_driver_version(const std::string& report) {
  for (std::size_t i = 0; i < report.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(report[i]))) continue;
    std::vector<unsigned> parts;
    unsigned current = 0;
    bool any = false;
    for (std::size_t j = i; j <= report.size(); ++j) {
      if (j < report.size() && std::isdigit(static_cast<unsigned char>(report[j]))) {
        current = current * 10U + static_cast<unsigned>(report[j] - '0');
        any = true;
        continue;
      }
      if (j < report.size() && report[j] == '.' && j + 1 < report.size() && std::isdigit(static_cast<unsigned char>(report[j + 1]))) {
        parts.push_back(current);
        current = 0;
        continue;
      }
      break;
    }
    if (any) parts.push_back(current);
    if (!parts.empty()) return parts;
  }
  return std::nullopt;
}

}  // namespace doctor
}  // namespace secure_frontend
