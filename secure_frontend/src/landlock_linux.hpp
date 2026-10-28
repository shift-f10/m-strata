#pragma once
// Linux-only internal header for SP4's confined downloader (spec §3.2). The
// UAPI structs are defined here so older kernel headers suffice.
#if defined(__linux__)
#include <cstddef>
#include <cstdint>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#endif
#ifndef SYS_landlock_add_rule
#define SYS_landlock_add_rule 445
#endif
#ifndef SYS_landlock_restrict_self
#define SYS_landlock_restrict_self 446
#endif

namespace secure_frontend::landlock {

inline constexpr std::uint32_t kCreateRulesetVersion = 1U << 0U;
inline constexpr int kRulePathBeneath = 1;
inline constexpr int kRuleNetPort = 2;

struct RulesetAttr final {
  std::uint64_t handled_access_fs;
  std::uint64_t handled_access_net;
};

struct __attribute__((packed)) PathBeneathAttr final {
  std::uint64_t allowed_access;
  std::int32_t parent_fd;
};

struct NetPortAttr final {
  std::uint64_t allowed_access;
  std::uint64_t port;
};

inline int create_ruleset(const RulesetAttr* attr, const std::size_t size, const std::uint32_t flags) noexcept {
  return static_cast<int>(::syscall(SYS_landlock_create_ruleset, attr, size, flags));
}
inline int add_rule(const int ruleset, const int type, const void* attr) noexcept {
  return static_cast<int>(::syscall(SYS_landlock_add_rule, ruleset, type, attr, 0U));
}
// Async-signal-safe (a raw syscall): called from the pre-exec hook.
inline int restrict_self(const int ruleset) noexcept { return static_cast<int>(::syscall(SYS_landlock_restrict_self, ruleset, 0U)); }

}  // namespace secure_frontend::landlock
#endif
