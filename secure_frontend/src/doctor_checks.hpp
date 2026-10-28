#pragma once
// SP7 M1 Task 3 (spec D-7): the full offline doctor check set. Every check is
// a pure function of an injected ProbeBundle (filesystem stats, PATH probes,
// port bind, config load result, rlimit read), so every check is unit-tested
// with fakes and the verb only composes. Human-fix strings are fixed prose
// that never carry a command that installs anything (D-7/PX §8).
#include "secure_frontend/frontend_config.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace secure_frontend {
namespace doctor {

struct CheckResult final {
  std::string name;
  bool ok{false};
  std::string human_fix;  // set when !ok
};

// The engine profile's expectations (R-T3-a): compiled in per profile from the
// pinned upstream's own documents — driver >= 580 (upstream/docs/INSTALL.md:29
// at pin 011c9e60), unlimited locked memory (upstream/docs/DOCKER.md runs the
// engine with --ulimit memlock=-1), CUDA 12/13 runtime libraries. Injectable
// so tests vary them; the doctor never contacts the network or installs.
struct ProfileExpectations final {
  std::string min_driver{"580"};
  bool memlock_unlimited{true};
  std::vector<std::string> cuda_runtime_libs{"libcudart.so.12", "libcudart.so.13"};
  // The multiengine build carries the CUDA runtime itself and loads cuBLAS (with its cuBLASLt) from its own folder
  // or engine.lib_dirs; any one complete set passes (owner 2026-10-28, the README audit).
#if defined(_WIN32)
  std::vector<std::vector<std::string>> cublas_sets{{"cublas64_13.dll", "cublasLt64_13.dll"},
                                                    {"cublas64_12.dll", "cublasLt64_12.dll"}};
#else
  std::vector<std::vector<std::string>> cublas_sets{{"libcublas.so.13", "libcublasLt.so.13"},
                                                    {"libcublas.so.12", "libcublasLt.so.12"}};
#endif
};
[[nodiscard]] const ProfileExpectations& strata_engine_profile_expectations() noexcept;

// One open_root-class verdict per configured root, computed by the verb; an
// empty error means the root passed (canonical, owned, private per the CFG
// rules). The message may name a path (doctor's job is saying where things are).
struct RootProbe final {
  std::string label;
  std::string error;
};

struct ProbeBundle final {
  ProfileExpectations profile{strata_engine_profile_expectations()};
  // The SP1 loader's result, reused verbatim: either a validated config or the
  // full ConfigErrors list (every rejected key by name, values never echoed).
  std::shared_ptr<const FrontendConfig> config;
  ConfigErrors config_issues;
  // nvidia-smi PATH probe: nullopt = absent or unrunnable; otherwise its
  // driver-version report text.
  std::function<std::optional<std::string>()> nvidia_smi;
  // Regular-file stat: the 0777 mode bits, or nullopt when absent/not regular.
  std::function<std::optional<unsigned>(const std::filesystem::path&)> file_mode;
  std::filesystem::path program_dir;  // CUDA runtime libraries live next to the program
  std::vector<std::filesystem::path> engine_dirs;  // the engine's own folder, then engine.lib_dirs: where cuBLAS loads from
  std::optional<std::filesystem::path> token_file;  // nullopt: an env: token — no local file to check
  // Windows keeps the token file private by its ACL, which mode bits do not show: the verb sets false and the check
  // asks the store instead (token_store_error: the store's refusal, nullopt when it reads the token under its rules).
  bool file_modes_apply{true};
  std::function<std::optional<std::string>()> token_store_error;
  std::vector<RootProbe> roots;
  std::function<bool()> port_free;  // the bind probe for listener.host:listener.port
  // Current RLIMIT_MEMLOCK in bytes; RLIM_INFINITY arrives as std::numeric_limits<
  // std::uint64_t>::max(); nullopt: unreadable. Windows (large pages ride the
  // "Lock pages in memory" user right instead, R-T3-c) sets memlock_applicable=false.
  std::function<std::optional<std::uint64_t>()> memlock_limit;
  bool memlock_applicable{true};
  // --deep only: one fixed human sentence per installed model whose manifest
  // digest re-check failed (empty: every installed model verified, or none).
  std::function<std::vector<std::string>()> deep_digest_failures;
  bool deep{false};
};

[[nodiscard]] CheckResult check_config(const ProbeBundle& bundle);
[[nodiscard]] CheckResult check_token(const ProbeBundle& bundle);
[[nodiscard]] CheckResult check_roots(const ProbeBundle& bundle);
[[nodiscard]] CheckResult check_port(const ProbeBundle& bundle);
[[nodiscard]] CheckResult check_gpu(const ProbeBundle& bundle);
[[nodiscard]] CheckResult check_driver(const ProbeBundle& bundle);
[[nodiscard]] CheckResult check_cuda_runtime(const ProbeBundle& bundle);
[[nodiscard]] CheckResult check_memlock(const ProbeBundle& bundle);
[[nodiscard]] CheckResult check_model_digests(const ProbeBundle& bundle);

// The whole set in D-7 order: gpu, driver, cuda, config, token, roots, port,
// memlock, then model digests under --deep.
[[nodiscard]] std::vector<CheckResult> run_checks(const ProbeBundle& bundle);

// The first dotted integer in a report ("Driver Version: 580.65.07" ->
// {580, 65, 7}); nullopt when the text holds none.
[[nodiscard]] std::optional<std::vector<unsigned>> parse_driver_version(const std::string& report);

}  // namespace doctor
}  // namespace secure_frontend
