#pragma once

// The page's About (owner 2026-10-27: "it should list the build of our engines and also list the hardware of the
// system"): what GET /v1/strata/about tells a key holder - the engine folder's build record (make.py writes
// engine/build.json: when and from which commit the folder was built, the pinned ggml, the GPU targets and features,
// each family library with the models it runs and the source it is built from) and this PC's hardware (the CPU and its
// threads, the RAM, the GPU, the operating system). Never a path, a user or a host name.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace secure_frontend {

struct EngineBuildLibrary final {
  std::string library;   // its file name
  std::string models;    // the models it runs
  std::string source;    // what it is built from
  std::uint64_t bytes{0};
};
struct EngineBuild final {
  std::string built;     // UTC, ISO 8601
  std::string commit;    // the repository commit
  std::string ggml;      // the pinned ggml commit
  std::string cuda_arch; // the GPU targets
  std::vector<std::string> features;
  std::vector<EngineBuildLibrary> engines;
};
// <engine_folder>/build.json, read and checked: schema 1, at most 64 KiB, only these fields, each text printable ASCII
// of at most 200 characters, at most 32 engines and 8 features. nullopt: none there, or not such a record.
[[nodiscard]] std::optional<EngineBuild> read_engine_build(const std::filesystem::path& engine_folder);

struct HardwareFacts final {
  std::string cpu;                         // "<name>, <n> threads"
  std::optional<std::uint64_t> ram_bytes;  // physical memory
  std::string gpu;                         // "<name>, <MiB> MiB" (the Monitor's reading); empty: none
  std::string os;                          // Windows: edition, version and build; POSIX: PRETTY_NAME and the kernel
};
// This PC's CPU, RAM and operating system (the GPU is the caller's: the Monitor reads it). Printable ASCII only.
[[nodiscard]] HardwareFacts hardware_facts();

// {"build": {...} | null, "hardware": {"cpu", "ram_bytes", "gpu", "os"}} - a missing reading is null.
[[nodiscard]] std::string render_about_json(const std::optional<EngineBuild>& build, const HardwareFacts& hardware);

}  // namespace secure_frontend
