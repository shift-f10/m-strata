#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// Landlock UAPI access bits (stable kernel ABI; defined here so the planning
// code builds and is tested on every platform).
namespace landlock_access {
inline constexpr std::uint64_t kFsExecute = 1ULL << 0U;
inline constexpr std::uint64_t kFsWriteFile = 1ULL << 1U;
inline constexpr std::uint64_t kFsReadFile = 1ULL << 2U;
inline constexpr std::uint64_t kFsReadDir = 1ULL << 3U;
inline constexpr std::uint64_t kFsRemoveDir = 1ULL << 4U;
inline constexpr std::uint64_t kFsRemoveFile = 1ULL << 5U;
inline constexpr std::uint64_t kFsMakeChar = 1ULL << 6U;
inline constexpr std::uint64_t kFsMakeDir = 1ULL << 7U;
inline constexpr std::uint64_t kFsMakeReg = 1ULL << 8U;
inline constexpr std::uint64_t kFsMakeSock = 1ULL << 9U;
inline constexpr std::uint64_t kFsMakeFifo = 1ULL << 10U;
inline constexpr std::uint64_t kFsMakeBlock = 1ULL << 11U;
inline constexpr std::uint64_t kFsMakeSym = 1ULL << 12U;
inline constexpr std::uint64_t kFsRefer = 1ULL << 13U;     // ABI 2
inline constexpr std::uint64_t kFsTruncate = 1ULL << 14U;  // ABI 3
inline constexpr std::uint64_t kFsIoctlDev = 1ULL << 15U;  // ABI 5
inline constexpr std::uint64_t kNetBindTcp = 1ULL << 0U;   // ABI 4
inline constexpr std::uint64_t kNetConnectTcp = 1ULL << 1U;
}  // namespace landlock_access

// handled_* = every right the probed ABI defines (fs/net through ABI 5; ABI 6+ `scoped`
// rights stay unhandled, plan note 14); rules grant only read/execute (§3.2).
struct LandlockRights final {
  std::uint64_t handled_fs;
  std::uint64_t handled_net;
  std::uint64_t directory_rule;  // READ_FILE | READ_DIR | EXECUTE beneath allowed dirs
  std::uint64_t file_rule;       // READ_FILE on /dev/null, /dev/urandom
};
[[nodiscard]] LandlockRights landlock_rights_for_abi(int abi) noexcept;

inline constexpr std::array<std::string_view, 7> kLandlockSystemDirs{"/usr", "/lib", "/lib64", "/lib32", "/bin", "/sbin", "/etc"};
inline constexpr std::array<std::string_view, 2> kLandlockReadableFiles{"/dev/null", "/dev/urandom"};

// A path the downloader must not read: the config file, roots.models, roots.secret.
struct ProtectedPath final {
  std::string key_path;   // "config file", "roots.models", "roots.secret"
  std::string canonical;  // canonical absolute path, '/'-separated
};

struct ConfinementPlan final {
  std::vector<std::string> read_dirs;       // existing system dirs, then the downloader's parent if not covered
  std::optional<std::string> readable_key;  // first protected path beneath a read dir
};

// Separator-boundary containment: "/usr" contains "/usr" and "/usr/x", not "/usrx".
[[nodiscard]] bool path_is_beneath(std::string_view dir, std::string_view path) noexcept;
[[nodiscard]] ConfinementPlan plan_linux_confinement(std::string_view downloader_canonical,
                                                     std::span<const ProtectedPath> protected_paths,
                                                     const std::function<bool(std::string_view)>& directory_exists);
// "full", "reduced:no_landlock" or "reduced:readable:<key path>" (§7.3).
[[nodiscard]] std::string confinement_label(bool landlock_available, const std::optional<std::string>& readable_key);

inline constexpr std::string_view kLowIntegritySid = "S-1-16-4096";
inline constexpr std::string_view kTrustedInstallerSid = "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

// Microsoft CRT descriptor-inheritance block (STARTUPINFO.lpReserved2), §3.2.
namespace crt_flags {
inline constexpr std::uint8_t kOpen = 0x01;
inline constexpr std::uint8_t kPipe = 0x08;
inline constexpr std::uint8_t kDevice = 0x40;
}  // namespace crt_flags
inline constexpr std::size_t kCrtBlockEntries = 5;
inline constexpr std::uint64_t kInvalidHandleBits = ~0ULL;
// int32 n = 5 (little endian), n flag bytes, n handles of handle_width bytes (little endian), packed.
[[nodiscard]] std::vector<std::uint8_t> build_crt_fd_block(std::span<const std::uint64_t, 5> handles,
                                                           std::span<const std::uint8_t, 5> flags, std::size_t handle_width);

}  // namespace secure_frontend
