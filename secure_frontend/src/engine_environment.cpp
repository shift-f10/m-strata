#include "secure_frontend/engine_environment.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include "windows_security.hpp"
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace secure_frontend {
namespace {

std::string native(const std::filesystem::path& path) {
  auto copy = path;
  copy.make_preferred();
  const auto text = copy.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string join(const std::vector<std::filesystem::path>& paths, const char separator) {
  std::string joined;
  for (const auto& path : paths) {
    if (!joined.empty()) joined += separator;
    joined += native(path);
  }
  return joined;
}

void add_optional(EnvironmentBlock& block, const EngineEnvironment& environment) {
  if (environment.hipblaslt_tuning) block.emplace("STRATA_HIPBLASLT_TUNING", native(*environment.hipblaslt_tuning));
  if (environment.watchdog_s) block.emplace("STRATA_WATCHDOG_S", std::to_string(*environment.watchdog_s));
  if (environment.watchdog_io_s) block.emplace("STRATA_WATCHDOG_IO_S", std::to_string(*environment.watchdog_io_s));
}

// The constant part of each family's rows, validated at compile time. strata
// forces and suppresses nothing: its rendering is exactly the §3.9 allowlist.
// The glm family's constant rows (SP5 M2 Task 4, D-7 superseding A10's bound
// on SP6 §4.9): the §4.9 safety values as forced defaults — a typed
// environment.glm_knobs row of the same name replaces one (the hook below) —
// and the two rows the vendored MAYA v1.1.21 has no literal for (SP6 §4.9:
// WATCHDOG_IO absent, HIPBLASLT tuning A4), suppressed.
struct ConstantForcedValue final {
  std::string_view name;
  std::string_view value;
};
struct ConstantFamilyRows final {
  std::span<const ConstantForcedValue> forced;
  std::span<const std::string_view> suppressed;
};
constexpr std::array<ConstantForcedValue, 5> kGlmSafetyDefaults{{
    {"STRATA_GLM_SLOTS", "0"},        // SP6 §4.9 A10: conversation slots off (generate.cpp:1270, 1354-1360)
    {"STRATA_GLM_LOOP_THINK", "0"},   // SP6 §4.9 A11: loop guard off while thinking (generate.cpp:1938-1941)
    {"STRATA_GLM_LOOP_ANSWER", "0"},  // SP6 §4.9 A11: loop guard off in the answer (same lines)
    {"STRATA_GLM_SPLIT", "0"},        // SP6 §4.9: one device even if a driver exposes more (glm_model.cu:1945-1972)
    {"STRATA_WATCHDOG_S", "0"},       // SP6 §4.9 §9/M5: the engine's own watchdog off (generate.cpp:747)
}};
constexpr std::array<std::string_view, 2> kGlmSuppressed{{"STRATA_HIPBLASLT_TUNING", "STRATA_WATCHDOG_IO_S"}};
constexpr ConstantFamilyRows kStrataRows{};
constexpr ConstantFamilyRows kGlmRows{kGlmSafetyDefaults, kGlmSuppressed};

constexpr bool contains(const std::span<const std::string_view> names, const std::string_view name) noexcept {
  for (const auto candidate : names) {
    if (candidate == name) return true;
  }
  return false;
}

// The one validity check, for constant rows (compile time) and derived rows
// (run time alike). `forced` entries have .name/.value; `suppressed` are names.
template <typename Forced, typename Suppressed>
constexpr bool family_rows_are_valid(const Forced& forced, const Suppressed& suppressed) noexcept {
  for (std::size_t i = 0; i < std::size(forced); ++i) {
    const std::string_view name = forced[i].name;
    const std::string_view value = forced[i].value;
    if (!contains(kFamilyForcibleEnvironment, name)) return false;
    if (contains(kNeverForwardedEnvironment, name) && !contains(kFamilyGpuSelectors, name)) return false;
    if (value.find('\0') != std::string_view::npos) return false;
    for (std::size_t j = 0; j < i; ++j) {
      if (std::string_view{forced[j].name} == name) return false;
    }
  }
  for (std::size_t i = 0; i < std::size(suppressed); ++i) {
    const std::string_view name = suppressed[i];
    if (!contains(kFamilySuppressibleEnvironment, name)) return false;
    for (std::size_t j = 0; j < i; ++j) {
      if (std::string_view{suppressed[j]} == name) return false;
    }
    for (std::size_t j = 0; j < std::size(forced); ++j) {
      if (std::string_view{forced[j].name} == name) return false;
    }
  }
  return true;
}

static_assert(family_rows_are_valid(kStrataRows.forced, kStrataRows.suppressed) &&
                  family_rows_are_valid(kGlmRows.forced, kGlmRows.suppressed),
              "every family's constant rows pass the forced-values validation");

FamilyEnvironmentRows owned(const ConstantFamilyRows& rows) {
  FamilyEnvironmentRows result;
  for (const auto& entry : rows.forced) result.forced.push_back({std::string(entry.name), std::string(entry.value)});
  for (const auto name : rows.suppressed) result.suppressed.emplace_back(name);
  return result;
}

}  // namespace

FamilyEnvironmentRows family_environment_rows(const EngineEnvironment& environment) {
  if (environment.family == EngineFamily::strata) return owned(kStrataRows);
  // glm (SP5 M2 Task 4, D-7): the §4.9 safety defaults, each moved by a typed
  // row of the same name; the typed knob rows themselves; and the CUDA
  // selectors — GPU selection is environment for this family only
  // (maya-ref serve/server.py:1280-1288, SP6 §4.9 A8). A name off the closed
  // allowlist, or a name both forced and suppressed, fails the merge and the
  // spawn is refused: the table bounds what can ever reach the child.
  FamilyEnvironmentRows rows = owned(kGlmRows);
  for (const auto& knob : environment.glm_knobs) {
    std::erase_if(rows.forced, [&knob](const ForcedEnvironmentValue& forced) { return forced.name == knob.name; });
    rows.forced.push_back({knob.name, knob.value});
  }
  // SP5 M2 Task 8 (D-8): the VLEND-family row exists only for a vision turn (the
  // on-demand encoder the engine lends expert-pool memory to, maya-ref serve/
  // server.py:4015/:4026). Without a turn the row is suppressed even when the
  // knob channel carries it — the table bounds what can reach the child.
  if (!environment.glm_vision_turn) {
    std::erase_if(rows.forced,
                  [](const ForcedEnvironmentValue& forced) { return forced.name == "STRATA_GLM_VISION_LEND_MB"; });
    rows.suppressed.emplace_back("STRATA_GLM_VISION_LEND_MB");
  }
  // Multi-GPU like upstream (owner 2026-10-28): Maya's engine splits the layers across the visible GPUs by default
  // (glm_model.cu load_pack_place); the §4.9 single device stays forced unless several GPUs are named. A pinned split
  // (a knob above) is left as it is. The rendering is closed, so without the row the engine sees none.
  const bool pinned = std::any_of(environment.glm_knobs.begin(), environment.glm_knobs.end(),
                                  [](const ForcedEnvironmentValue& knob) { return knob.name == "STRATA_GLM_SPLIT"; });
  if (environment.glm_gpus.size() > 1U && !pinned) {
    std::erase_if(rows.forced, [](const ForcedEnvironmentValue& forced) { return forced.name == "STRATA_GLM_SPLIT"; });
  }
  std::string devices;
  for (const unsigned gpu : environment.glm_gpus) devices += (devices.empty() ? "" : ",") + std::to_string(gpu);
  if (devices.empty()) devices = "0";  // SP6 §4.9: an absent list renders device 0.
  rows.forced.push_back({"CUDA_DEVICE_ORDER", "PCI_BUS_ID"});
  rows.forced.push_back({"CUDA_VISIBLE_DEVICES", std::move(devices)});
  return rows;
}

bool merge_forced_environment(EnvironmentBlock& block, const FamilyEnvironmentRows& rows) {
  if (!family_rows_are_valid(rows.forced, rows.suppressed)) return false;
  for (const auto& name : rows.suppressed) block.erase(name);
  for (const auto& entry : rows.forced) block.insert_or_assign(entry.name, entry.value);
  return true;
}

std::optional<EnvironmentBlock> apply_family_environment(EnvironmentBlock generic, const EngineEnvironment& environment,
                                                         const FamilyEnvironmentHook hook) {
  if (hook == nullptr || !merge_forced_environment(generic, hook(environment))) return std::nullopt;
  return generic;
}

std::optional<EnvironmentBlock> render_family_environment(const EngineEnvironment& environment, const FamilyEnvironmentHook hook) {
  return apply_family_environment(render_engine_environment(environment), environment, hook);
}

EnvironmentBlock render_posix_environment(const EngineEnvironment& environment) {
  EnvironmentBlock block;
  block.emplace("PATH", "/usr/bin:/bin");
  if (!environment.lib_dirs.empty()) block.emplace("LD_LIBRARY_PATH", join(environment.lib_dirs, ':'));
  block.emplace("TMPDIR", native(environment.temp_dir));
  block.emplace("CUDA_CACHE_PATH", native(environment.cuda_cache_dir));
  add_optional(block, environment);
  return block;
}

EnvironmentBlock render_windows_environment(const EngineEnvironment& environment, const WindowsSystemValues& system) {
  EnvironmentBlock block;
  std::string path = join(environment.lib_dirs, ';');
  if (!path.empty()) path += ';';
  path += system.system_root + "\\System32";
  block.emplace("PATH", path);
  block.emplace("TEMP", native(environment.temp_dir));
  block.emplace("TMP", native(environment.temp_dir));
  block.emplace("CUDA_CACHE_PATH", native(environment.cuda_cache_dir));
  block.emplace("SystemRoot", system.system_root);
  block.emplace("WINDIR", system.system_root);
  block.emplace("SystemDrive", system.system_drive);
  block.emplace("NUMBER_OF_PROCESSORS", std::to_string(system.processors));
  add_optional(block, environment);
  return block;
}

WindowsSystemValues query_windows_system_values() {
  WindowsSystemValues values;
#if defined(_WIN32)
  wchar_t buffer[MAX_PATH + 1]{};
  const UINT length = GetSystemWindowsDirectoryW(buffer, MAX_PATH + 1);
  if (length > 0 && length <= MAX_PATH) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    values.system_root.assign(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) (void)WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), values.system_root.data(), size, nullptr, nullptr);
  }
  values.system_drive = values.system_root.substr(0, 2);
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  values.processors = info.dwNumberOfProcessors;
#endif
  return values;
}

EnvironmentBlock render_engine_environment(const EngineEnvironment& environment) {
#if defined(_WIN32)
  return render_windows_environment(environment, query_windows_system_values());
#else
  return render_posix_environment(environment);
#endif
}

bool is_owner_only_directory(const std::filesystem::path& path) {
#if defined(_WIN32)
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  return !error && std::filesystem::is_directory(status);
#else
  struct stat info {};
  if (lstat(path.c_str(), &info) != 0) return false;
  return S_ISDIR(info.st_mode) && info.st_uid == geteuid() && (info.st_mode & 077) == 0;
#endif
}

bool is_private_state_root(const std::filesystem::path& path) {
#if defined(_WIN32)
  namespace ws = windows_security;
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error);
  if (error) return false;
  HANDLE handle = CreateFileW(ws::extended_path(absolute.wstring()).c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  FILE_ATTRIBUTE_TAG_INFO tag{};
  std::array<std::byte, SECURITY_MAX_SID_SIZE> user_storage{};
  PSID user = nullptr;
  const bool directory = GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) != 0 &&
                         (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
  const bool owner_only = directory && !ws::is_reparse_point(handle) && ws::current_user_sid(user_storage, &user) &&
                          ws::has_private_owner_dacl(handle, user);
  CloseHandle(handle);
  return owner_only;
#else
  return is_owner_only_directory(path);
#endif
}

EngineEnvironment engine_environment_for(const std::filesystem::path& state_dir, const std::optional<std::filesystem::path>& cache_root,
                                         std::vector<std::filesystem::path> lib_dirs) {
  EngineEnvironment environment;
  environment.lib_dirs = std::move(lib_dirs);
  environment.temp_dir = state_dir / "engine-tmp";
  environment.cuda_cache_dir = cache_root ? *cache_root / "cuda" : state_dir / "cache" / "cuda";
  return environment;
}

bool create_owner_only_directory(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::create_directories(path, error);
  if (error) return false;
#if !defined(_WIN32)
  if (chmod(path.c_str(), 0700) != 0) return false;
#endif
  return is_owner_only_directory(path);
}

bool clear_directory_contents(const std::filesystem::path& directory) noexcept {
  try {
    std::error_code error;
    bool cleared = true;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
      std::error_code remove_error;
      std::filesystem::remove_all(entry.path(), remove_error);  // a symlink entry is removed, not followed
      cleared = cleared && !remove_error;
    }
    return cleared && !error;
  } catch (...) {
    return false;
  }
}

}  // namespace secure_frontend
