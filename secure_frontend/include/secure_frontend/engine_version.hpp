#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend {

// MAJOR.MINOR.PATCH with an optional fourth .N; each component 0..9999.
struct EngineVersion final {
  std::array<std::uint32_t, 4> parts{};
  std::uint8_t count{3};
};

[[nodiscard]] std::optional<EngineVersion> parse_engine_version(std::string_view text) noexcept;
// <0, 0, >0; a missing fourth component compares as 0.
[[nodiscard]] int compare_versions(const EngineVersion& a, const EngineVersion& b) noexcept;
[[nodiscard]] std::string render_engine_version(const EngineVersion& version);

}  // namespace secure_frontend
