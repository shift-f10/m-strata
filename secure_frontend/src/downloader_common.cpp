#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/downloader.hpp"

namespace secure_frontend {
namespace {

// Ruling I8: downloader wording for SP1's open_executable codes (fixed text, no path).
std::string downloader_path_text(const PathErrorCode code) {
  switch (code) {
    case PathErrorCode::not_absolute:
      return "pull.downloader must be an absolute path";
    case PathErrorCode::not_found:
      return "pull.downloader does not exist";
    case PathErrorCode::not_regular_file:
      return "pull.downloader must be a regular file";
    case PathErrorCode::untrusted_owner:
#if defined(_WIN32)
      return "pull.downloader must be owned by the current user, SYSTEM, Administrators or TrustedInstaller";
#else
      return "pull.downloader must be owned by the current user or root";
#endif
    case PathErrorCode::shared_writable:
#if defined(_WIN32)
      return "pull.downloader and its directory must not be writable by another account";
#else
      return "pull.downloader and its directory must not be writable by group or others";
#endif
    default:
      return "pull.downloader cannot be used";
  }
}

}  // namespace

// §3.2 "Executable" row: SP1's open_executable on both platforms (normalisation,
// final-path classification, regular disk file, file and parent trust), with
// TrustedInstaller also an allowed writer on Windows (Task 26 fix round 1).
std::variant<std::filesystem::path, AcquireError> validate_downloader(const std::filesystem::path& requested) {
  auto opened = open_executable(requested, ExecutableWriters::also_trusted_installer);
  if (auto* failure = std::get_if<PathError>(&opened)) {
    return AcquireError{failure->code == PathErrorCode::not_found ? AcquireReason::downloader_missing
                                                                  : AcquireReason::downloader_untrusted_path,
                        downloader_path_text(failure->code)};
  }
  return std::get<CanonicalFile>(opened).path();
}

// argv is UTF-8 on both platforms (path_to_utf8: no Windows ANSI code page; POSIX
// bytes unchanged); the Windows spawn decodes it with path_from_utf8.
std::optional<AcquireError> ConfinedDownloader::probe() {
  auto valid = validate_downloader(options_.executable);
  if (auto* failure = std::get_if<AcquireError>(&valid)) return *failure;
  validated_ = std::get<std::filesystem::path>(valid);
  std::string output;
  const auto result = spawn(build_probe_argv(path_to_utf8(*validated_)), {}, [&output](const std::string_view bytes) {
    output.append(bytes);
    return output.size() <= 65536;
  });
  if (result.failure) return result.failure;
  // §8.3 (final fix F3): a probe killed because of the interrupt flag is an interrupt (exit 8).
  if (result.killed && options_.interrupted != nullptr && options_.interrupted->load()) {
    return AcquireError{AcquireReason::interrupted, "the downloader probe was stopped"};
  }
  if (result.exit.value_or(-1) != 0) return AcquireError{AcquireReason::downloader_unsupported, "the probe exited non-zero"};
  const auto parsed = parse_probe_output(output);
  if (!parsed) return AcquireError{AcquireReason::downloader_unsupported, "not a curl with a TLS backend"};
  if (!meets_version_floor(*parsed, kThisDownloaderPlatform)) {
    return AcquireError{AcquireReason::downloader_version, "curl " + std::to_string(parsed->major) + "." + std::to_string(parsed->minor) +
                                                               "." + std::to_string(parsed->patch) + " is below the floor"};
  }
  probe_result_ = parsed;
  return std::nullopt;
}

HopExit ConfinedDownloader::run(const TransferRequest& request, const std::function<bool(std::string_view)>& on_bytes) {
  if (!validated_) {
    auto valid = validate_downloader(options_.executable);
    if (auto* failure = std::get_if<AcquireError>(&valid)) return HopExit{std::nullopt, *failure, false};
    validated_ = std::get<std::filesystem::path>(valid);
  }
  const auto result = spawn(build_transfer_argv(path_to_utf8(*validated_), request), build_config_stdin(request), on_bytes);
  return HopExit{result.exit, result.failure, result.killed};
}

}  // namespace secure_frontend
