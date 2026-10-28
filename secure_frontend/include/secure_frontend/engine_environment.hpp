#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend {

// SP6 §4.9 (mechanism, R5): which engine family the environment is rendered
// for. A family's hook may force and suppress rows (family_environment_rows).
// `strata` has no rows, so it renders exactly the §3.9 allowlist. `glm` is
// reserved for SP6: it currently renders strata rows, is not reachable from
// config, and SP6 fills its table.
enum class EngineFamily { strata, glm };

// One variable a family forces (owning: a hook may derive the value from the
// EngineEnvironment, e.g. SP6's CUDA_VISIBLE_DEVICES from the gpu list).
struct ForcedEnvironmentValue final {
  std::string name;
  std::string value;
};

// Spec §3.9: the engine's environment is a closed type rendered by the
// frontend. Every value is generated or comes from validated configuration
// (SP1); nothing is copied from the parent environment by name.
// The optional members carry `{}` so designated initializers that name only
// the required three (as the tests and the server do) compile under -Werror.
struct EngineEnvironment final {
  std::vector<std::filesystem::path> lib_dirs;              // POSIX LD_LIBRARY_PATH; Windows head of PATH
  std::filesystem::path temp_dir;                           // TMPDIR | TEMP + TMP
  std::filesystem::path cuda_cache_dir;                     // CUDA_CACHE_PATH (replaces ~/.nv/ComputeCache; HOME is unset)
  std::optional<std::filesystem::path> hipblaslt_tuning{};  // STRATA_HIPBLASLT_TUNING (under the install root)
  std::optional<unsigned> watchdog_s{};                     // STRATA_WATCHDOG_S
  std::optional<unsigned> watchdog_io_s{};                  // STRATA_WATCHDOG_IO_S
  EngineFamily family{EngineFamily::strata};                // SP6 §4.9: selects the forced-values table
  // SP5 M2 Task 4 (D-7): the glm family's typed knob rows, names validated
  // against the closed allowlist by the merge step; a typed row moves the
  // §4.9 forced default of the same name. glm_gpus renders the CUDA selectors
  // (SP6 §4.9 A8: GPU selection is environment for this family only).
  std::vector<unsigned> glm_gpus{};
  std::vector<ForcedEnvironmentValue> glm_knobs{};
  // SP5 M2 Task 8 (D-8): a GLM vision turn (vision.enabled, encoder not resident —
  // maya-ref serve/server.py:4015). The VLEND-family row (STRATA_GLM_VISION_LEND_MB,
  // :4026) reaches the engine only with a turn configured; without one the render
  // suppresses it even if the knob channel carries it (fail closed).
  bool glm_vision_turn{false};
};

// Windows values from GetSystemWindowsDirectoryW / GetSystemInfo, never from the environment.
struct WindowsSystemValues final {
  std::string system_root;   // e.g. C:\Windows
  std::string system_drive;  // e.g. C:
  unsigned processors{1};
};

using EnvironmentBlock = std::map<std::string, std::string, std::less<>>;

// Names the engine never receives (spec §3.9); tests assert their absence. The
// token variable named by --token-env and every other STRATA_* are excluded too.
// RF9: this is the "never inherited from the parent" list. A family's forced
// values are rendered by the frontend; of this list they may set only the two
// CUDA selectors (kFamilyGpuSelectors). The N1 assertion applies to EngineFamily::strata.
inline constexpr std::array<std::string_view, 16> kNeverForwardedEnvironment{
    "STRATA_API_KEY", "LD_PRELOAD",  "LD_AUDIT",    "CUDA_LAUNCH_BLOCKING", "HOME",       "USERPROFILE",
    "APPDATA",        "HTTP_PROXY",  "HTTPS_PROXY", "ALL_PROXY",            "http_proxy", "https_proxy",
    "all_proxy",      "CUDA_VISIBLE_DEVICES", "CUDA_DEVICE_ORDER", "HIP_VISIBLE_DEVICES"};

// What a family's hook returns for one rendering: rows to force (replace or
// add) and generic optional rows to suppress.
struct FamilyEnvironmentRows final {
  std::vector<ForcedEnvironmentValue> forced{};
  std::vector<std::string> suppressed{};
};

// The family hook: derives the family's rows from the environment. The
// production hook is family_environment_rows; tests inject their own.
using FamilyEnvironmentHook = FamilyEnvironmentRows (*)(const EngineEnvironment& environment);

// The closed list of names a family may force (controller ruling, SP6 §4.9,
// as amended by D-7 2026-10-21): the glm safety constants, the watchdog
// override, the two GPU selectors — and every typed engine_env name of the
// GLM parity table, which is this list's source of truth (the
// sp6_env_forcible_names_are_the_ruled_set case regenerates it from the
// table; rejected names such as STRATA_GLM_DFLASH stay out). Sorted.
inline constexpr std::array<std::string_view, 167> kFamilyForcibleEnvironment{
    "STRATA_GLM_AHEAD", "STRATA_GLM_CKPTS", "STRATA_GLM_CKPT_EVERY", "STRATA_GLM_COMPLETE_POOL", "STRATA_GLM_CPU_1T",
    "STRATA_GLM_CPU_CONCURRENT", "STRATA_GLM_CPU_LANE", "STRATA_GLM_CPU_LANE0", "STRATA_GLM_CPU_LANE1",
    "STRATA_GLM_CPU_MT", "STRATA_GLM_CPU_PIN", "STRATA_GLM_CPU_PLAN", "STRATA_GLM_CPU_RETIME",
    "STRATA_GLM_CPU_SHARED", "STRATA_GLM_CPU_SPLIT", "STRATA_GLM_DISK_QD", "STRATA_GLM_DISK_READERS",
    "STRATA_GLM_DISK_SKIP_FROM", "STRATA_GLM_DISK_SKIP_W", "STRATA_GLM_DROP_CACHE", "STRATA_GLM_DSA_ALL",
    "STRATA_GLM_DSA_SCORE", "STRATA_GLM_DUAL_PREFILL", "STRATA_GLM_EVEN_GROUPS", "STRATA_GLM_EXL3_DECODE",
    "STRATA_GLM_EXL3_MMA_ROWS", "STRATA_GLM_EXL3_MMA_ROWS_SHORT", "STRATA_GLM_EXL3_ROT", "STRATA_GLM_EXPERT_CACHE",
    "STRATA_GLM_EXPERT_HELPER", "STRATA_GLM_FLUSH", "STRATA_GLM_GROUP_EXPERTS", "STRATA_GLM_GU_STAGE",
    "STRATA_GLM_HEAD_SPLIT", "STRATA_GLM_HEAD_SPLIT_CHECK", "STRATA_GLM_KDA_GRAPH", "STRATA_GLM_KQ",
    "STRATA_GLM_KV_INT8", "STRATA_GLM_KV_PREFETCH", "STRATA_GLM_KV_RESIDENT", "STRATA_GLM_LEND_PARK_LRU",
    "STRATA_GLM_LOOP_ANSWER", "STRATA_GLM_LOOP_THINK", "STRATA_GLM_MISS_SKIP_FROM", "STRATA_GLM_MISS_SKIP_W",
    "STRATA_GLM_MLA_MMA", "STRATA_GLM_MLA_V70", "STRATA_GLM_MLA_V70_SPLITS", "STRATA_GLM_MMQ_GLU",
    "STRATA_GLM_MMQ_RESERVE_MB", "STRATA_GLM_MTP_DECODE", "STRATA_GLM_MTP_KEEP", "STRATA_GLM_MTP_MISS",
    "STRATA_GLM_MULTI_BENCH", "STRATA_GLM_MULTI_BOUNDARY", "STRATA_GLM_MULTI_DUMP", "STRATA_GLM_MULTI_MTP",
    "STRATA_GLM_MULTI_MTP_DRAFT_ROWS", "STRATA_GLM_MULTI_MTP_MAX", "STRATA_GLM_MULTI_MTP_ROWS",
    "STRATA_GLM_MULTI_MTP_VOCAB", "STRATA_GLM_MULTI_ROOM", "STRATA_GLM_MULTI_SPEC", "STRATA_GLM_MV_P100",
    "STRATA_GLM_MV_P100_R", "STRATA_GLM_MV_RDNA", "STRATA_GLM_NO_MTP", "STRATA_GLM_NO_PREFILL", "STRATA_GLM_NO_SPEC",
    "STRATA_GLM_NUMA", "STRATA_GLM_PARALLEL_LOAD", "STRATA_GLM_PCIE_SHARE", "STRATA_GLM_POOL_CHECK",
    "STRATA_GLM_POOL_STATS", "STRATA_GLM_PREFETCH_AT", "STRATA_GLM_PREFETCH_BLOCKS", "STRATA_GLM_PREFETCH_N",
    "STRATA_GLM_PREFETCH_RANK", "STRATA_GLM_PREFILL_ATTN", "STRATA_GLM_PREFILL_ATTN_CHECK",
    "STRATA_GLM_PREFILL_CALC", "STRATA_GLM_PREFILL_CALC_CHECK", "STRATA_GLM_PREFILL_CALC_SHARE",
    "STRATA_GLM_PREFILL_CHUNK", "STRATA_GLM_PREFILL_CPU", "STRATA_GLM_PREFILL_CPU_ROW_MS",
    "STRATA_GLM_PREFILL_FLUSH", "STRATA_GLM_PREFILL_FUSED", "STRATA_GLM_PREFILL_GAPS",
    "STRATA_GLM_PREFILL_GAP_CREDIT", "STRATA_GLM_PREFILL_INTERLEAVE", "STRATA_GLM_PREFILL_INTERLEAVE_ALWAYS",
    "STRATA_GLM_PREFILL_KEEP", "STRATA_GLM_PREFILL_MB", "STRATA_GLM_PREFILL_MIN_CHUNKS", "STRATA_GLM_PREFILL_STAGES",
    "STRATA_GLM_PREFILL_SUB", "STRATA_GLM_PREFILL_TAIL_SKIP", "STRATA_GLM_PREFILL_TRACE",
    "STRATA_GLM_PREFILL_WINDOW", "STRATA_GLM_PREFIX_CUT", "STRATA_GLM_PRESTAGE", "STRATA_GLM_PRESTAGE_ADAPT",
    "STRATA_GLM_PROFILE_WEIGHT", "STRATA_GLM_PROMOTE", "STRATA_GLM_PROMOTE_FILL", "STRATA_GLM_PROMOTE_MIN",
    "STRATA_GLM_RAM_GB", "STRATA_GLM_RAM_HEADROOM_GB", "STRATA_GLM_RAM_RESIDENT", "STRATA_GLM_RAM_SLACK",
    "STRATA_GLM_READ_CHUNKS", "STRATA_GLM_RESERVE_LATER_MB", "STRATA_GLM_RESERVE_MB", "STRATA_GLM_RESIDENT_SKIP",
    "STRATA_GLM_ROWS_ATTN", "STRATA_GLM_ROWS_BATCH", "STRATA_GLM_ROWS_SMALL", "STRATA_GLM_SCORE",
    "STRATA_GLM_SCORE_PREFILL", "STRATA_GLM_SEQS", "STRATA_GLM_SEQ_CTX", "STRATA_GLM_SERVICE_IDLE_MS",
    "STRATA_GLM_SKIP_EMPTY", "STRATA_GLM_SLOTS", "STRATA_GLM_SLOT_DAYS", "STRATA_GLM_SLOT_GB",
    "STRATA_GLM_SLOT_KEEP", "STRATA_GLM_SLOT_MIN", "STRATA_GLM_SPEC_CANCEL", "STRATA_GLM_SPEC_FOLLOW",
    "STRATA_GLM_SPEC_FOLLOW_SAVE", "STRATA_GLM_SPEC_HEAD", "STRATA_GLM_SPEC_SAMPLE", "STRATA_GLM_SPLIT",
    "STRATA_GLM_SPLIT_LAYER_MS", "STRATA_GLM_SPLIT_LOG", "STRATA_GLM_SPLIT_ROWS", "STRATA_GLM_TIER_DIAG",
    "STRATA_GLM_TIER_DIAG_EVERY", "STRATA_GLM_TIMING", "STRATA_GLM_TRUNK_KEEP", "STRATA_GLM_TRUNK_KEEP_MAP",
    "STRATA_GLM_VISION_LEND_MB", "STRATA_GLM_VRAM_EVICT", "STRATA_GLM_WARM_PROMPT", "STRATA_GLM_WIN_ALLCPU",
    "STRATA_GLM_WIN_BATCH", "STRATA_GLM_WIN_COMMIT_WAIT", "STRATA_GLM_WIN_CPU", "STRATA_GLM_WIN_LANES",
    "STRATA_GLM_WIN_MULTI", "STRATA_GLM_WIN_PIPE", "STRATA_GLM_WIN_REPLAY", "STRATA_GLM_WIN_SKIP",
    "STRATA_GLM_WIN_STAGGER", "STRATA_GLM_WIN_THREADS", "STRATA_GLM_WIN_X3", "STRATA_GLM_WIN_X3_POOLS",
    "STRATA_NO_IQ512", "STRATA_PF_WMMA", "STRATA_PREFILL_LEND_PCT", "STRATA_SPLIT_COVER_B",
    "STRATA_VIDEO_MAX_FRAMES", "STRATA_WATCHDOG_S", "CUDA_DEVICE_ORDER", "CUDA_VISIBLE_DEVICES"};

// The never-list names a family may still render (RF9): GPU selection by environment.
inline constexpr std::array<std::string_view, 2> kFamilyGpuSelectors{"CUDA_DEVICE_ORDER", "CUDA_VISIBLE_DEVICES"};
// The generic optional rows a family may suppress (controller ruling); no other.
inline constexpr std::array<std::string_view, 4> kFamilySuppressibleEnvironment{"STRATA_WATCHDOG_IO_S", "STRATA_HIPBLASLT_TUNING",
                                                                                "STRATA_WATCHDOG_S", "STRATA_GLM_VISION_LEND_MB"};

// The production hook: the rows of `environment.family`. strata: none.
// glm currently renders strata rows; not reachable from config; SP6 fills its table.
[[nodiscard]] FamilyEnvironmentRows family_environment_rows(const EngineEnvironment& environment);
// Applies `rows` to `block`: suppressed names are removed, then each forced
// name replaces the rendered value or is added. All-or-nothing: false, with
// `block` unchanged, when a forced name is not on kFamilyForcibleEnvironment or
// is on kNeverForwardedEnvironment without being a GPU selector, a forced value
// contains a NUL, a suppressed name is not on kFamilySuppressibleEnvironment,
// or any name appears twice (forced twice, suppressed twice, or both). The
// result does not depend on the rows' order.
[[nodiscard]] bool merge_forced_environment(EnvironmentBlock& block, const FamilyEnvironmentRows& rows);
// The family step on a generic rendering: nullopt when the hook's rows are
// refused (the caller must not spawn; never an empty or partial environment).
[[nodiscard]] std::optional<EnvironmentBlock> apply_family_environment(EnvironmentBlock generic, const EngineEnvironment& environment,
                                                                       FamilyEnvironmentHook hook = family_environment_rows);
// The native generic rendering followed by the family step: what the engine is spawned with.
[[nodiscard]] std::optional<EnvironmentBlock> render_family_environment(const EngineEnvironment& environment,
                                                                        FamilyEnvironmentHook hook = family_environment_rows);

// The generic §3.9 rows (exactly the strata rendering). They do not apply the
// family; the spawn path uses render_family_environment.
[[nodiscard]] EnvironmentBlock render_posix_environment(const EngineEnvironment& environment);
[[nodiscard]] EnvironmentBlock render_windows_environment(const EngineEnvironment& environment, const WindowsSystemValues& system);
[[nodiscard]] WindowsSystemValues query_windows_system_values();
// The native generic rendering (POSIX or Windows).
[[nodiscard]] EnvironmentBlock render_engine_environment(const EngineEnvironment& environment);
// The engine environment for a validated config: temp root <state>/engine-tmp,
// CUDA cache <cache root>/cuda when roots.cache is set, else <state>/cache/cuda.
[[nodiscard]] EngineEnvironment engine_environment_for(const std::filesystem::path& state_dir,
                                                       const std::optional<std::filesystem::path>& cache_root,
                                                       std::vector<std::filesystem::path> lib_dirs);

// POSIX: creates `path` (and parents) and makes it 0700; true only if it is
// then an owner-only directory. Windows: creates it (DACL checks are SP1's).
[[nodiscard]] bool create_owner_only_directory(const std::filesystem::path& path);
// POSIX: a real directory (not a symlink), owned by the effective uid, no group/other bits.
// Windows: a directory that is not a symlink.
[[nodiscard]] bool is_owner_only_directory(const std::filesystem::path& path);
// The --state-dir (roots.state) check, run by serve before anything is spawned.
// POSIX: is_owner_only_directory (lstat checks only the final component).
// Windows: opened with FILE_FLAG_OPEN_REPARSE_POINT, so a junction or symlink
// root is refused, and SP1's has_private_owner_dacl must hold: owner is the
// current user, protected DACL, allow-ACEs only for the user, SYSTEM and
// Administrators (a read ACE for anyone else is refused too).
[[nodiscard]] bool is_private_state_root(const std::filesystem::path& path);
// Removes every entry below `directory` (a symlink entry itself, never its
// target), keeping `directory`. Used at startup on <state>/engine-tmp: temp
// directories of generations that ended with a previous frontend never leak.
bool clear_directory_contents(const std::filesystem::path& directory) noexcept;

}  // namespace secure_frontend
