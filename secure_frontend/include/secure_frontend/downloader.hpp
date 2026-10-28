#pragma once

#include "secure_frontend/acquire_pull.hpp"
#include "secure_frontend/confinement_plan.hpp"
#include "secure_frontend/downloader_command.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace secure_frontend {

struct DownloaderOptions final {
  std::filesystem::path executable;                // pull.downloader (validated by probe(), lazily, §3.9)
  std::vector<ProtectedPath> protected_paths;      // config file, roots.models, roots.secret (canonical)
  std::filesystem::path cwd;                       // roots.models/.staging/.cwd
  std::optional<std::uint16_t> proxy_port;         // Landlock ABI >= 4 connect rule (443 otherwise)
  const std::atomic<bool>* interrupted{nullptr};   // checked while waiting for output
  // SECURE_FRONTEND_TEST_HOOKS builds only (ignored otherwise):
  std::filesystem::path record_file;               // §10.1 record channel → child fd 4 / CRT entry 4
  bool test_fail_pre_exec{false};                  // fault injection: the pre-exec hook returns EPERM
  bool test_force_no_landlock{false};              // behave as if Landlock were unavailable
};

// The confined system curl (§3.1-§3.3). Every spawn is a probe or a transfer.
class ConfinedDownloader final : public HopTransport, public DownloaderProbe {
 public:
  explicit ConfinedDownloader(DownloaderOptions options) : options_(std::move(options)) {}
  [[nodiscard]] HopExit run(const TransferRequest& request, const std::function<bool(std::string_view)>& on_bytes) override;
  [[nodiscard]] std::optional<AcquireError> probe() override;
  [[nodiscard]] std::string confinement() const override;
  [[nodiscard]] const std::optional<ProbeResult>& probe_result() const noexcept { return probe_result_; }

 private:
  struct SpawnOutput final {
    std::optional<int> exit;
    std::optional<AcquireError> failure;
    bool killed{false};
  };
  SpawnOutput spawn(const std::vector<std::string>& argv, std::string_view stdin_bytes,
                    const std::function<bool(std::string_view)>& on_bytes);
  DownloaderOptions options_;
  std::optional<std::filesystem::path> validated_;
  std::optional<ProbeResult> probe_result_;
};

// §3.2 "Executable" row. Linux: absolute, canonical, regular, file and parent not
// group/other-writable (SP1 open_executable). Windows (Task 26): the same, with
// TrustedInstaller also an allowed writer and INHERIT_ONLY ACEs ignored.
[[nodiscard]] std::variant<std::filesystem::path, AcquireError> validate_downloader(const std::filesystem::path& requested);
// Landlock ABI version (0: absent, disabled, or not Linux).
[[nodiscard]] int landlock_abi() noexcept;
#if !defined(_WIN32)
// SP2 report pipe bytes → reason: 'H' + errno → downloader_confinement_failed; an
// execve errno ENOENT → downloader_missing, EACCES → downloader_untrusted_path,
// anything else (or a malformed report) → downloader_unsupported. Empty input
// (EOF: execve succeeded) is not a failure; callers check that first.
[[nodiscard]] AcquireReason classify_spawn_report(std::string_view report) noexcept;
#endif

}  // namespace secure_frontend
