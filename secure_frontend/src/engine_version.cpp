#include "secure_frontend/engine_version.hpp"

namespace secure_frontend {

std::optional<EngineVersion> parse_engine_version(const std::string_view text) noexcept {
  EngineVersion version;
  std::size_t index = 0;
  std::size_t position = 0;
  while (true) {
    if (index == 4) return std::nullopt;
    std::size_t digits = 0;
    std::uint32_t value = 0;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
      if (++digits > 4) return std::nullopt;
      value = value * 10U + static_cast<std::uint32_t>(text[position] - '0');
      ++position;
    }
    if (digits == 0 || (digits > 1 && text[position - digits] == '0')) return std::nullopt;
    version.parts[index++] = value;
    if (position == text.size()) break;
    if (text[position] != '.') return std::nullopt;
    ++position;
  }
  if (index < 3) return std::nullopt;
  version.count = static_cast<std::uint8_t>(index);
  return version;
}

int compare_versions(const EngineVersion& a, const EngineVersion& b) noexcept {
  for (std::size_t i = 0; i < 4; ++i) {
    if (a.parts[i] != b.parts[i]) return a.parts[i] < b.parts[i] ? -1 : 1;
  }
  return 0;
}

std::string render_engine_version(const EngineVersion& version) {
  std::string text;
  for (std::size_t i = 0; i < version.count; ++i) {
    if (i != 0) text.push_back('.');
    text += std::to_string(version.parts[i]);
  }
  return text;
}

}  // namespace secure_frontend
